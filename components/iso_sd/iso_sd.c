#include "iso_sd.h"
#include "iso_sd_logic.h"

#include "sdkconfig.h"

#if defined(CONFIG_SD_ENABLE) && CONFIG_SD_ENABLE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio_impl.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"
#include "ffconf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#if !defined(CONFIG_FATFS_USE_FASTSEEK) || !CONFIG_FATFS_USE_FASTSEEK
#error "CONFIG_SD_ENABLE requires CONFIG_FATFS_USE_FASTSEEK=y"
#endif
#if defined(CONFIG_FATFS_LFN_NONE) && CONFIG_FATFS_LFN_NONE
#error "CONFIG_SD_ENABLE requires non-none FatFs LFN support (CONFIG_FATFS_LFN_HEAP=y)"
#endif
#if !defined(CONFIG_FATFS_API_ENCODING_UTF_8) || !CONFIG_FATFS_API_ENCODING_UTF_8
#error "CONFIG_SD_ENABLE requires CONFIG_FATFS_API_ENCODING_UTF_8=y"
#endif
#if !FF_FS_REENTRANT
#error "CONFIG_SD_ENABLE requires FF_FS_REENTRANT=1"
#endif
#if FF_FS_EXFAT
#error "CONFIG_SD_ENABLE expects FF_FS_EXFAT=0 per docs/ISO_MOUNT_DESIGN.md"
#endif

_Static_assert(sizeof(DWORD) == sizeof(uint32_t), "FatFs DWORD must be 32-bit for CLMT");
_Static_assert((int)FR_OK == (int)ISO_SD_FR_OK &&
                   (int)FR_NOT_ENOUGH_CORE == (int)ISO_SD_FR_NOT_ENOUGH_CORE &&
                   (int)FR_EXIST == (int)ISO_SD_FR_EXIST &&
                   (int)FR_INVALID_PARAMETER == (int)ISO_SD_FR_INVALID_PARAMETER,
               "FatFs FRESULT constants out of sync with iso_sd_fresult_t");

/* Schematic fixed pinout (docs/HW_VERIFY.md I1, Waveshare ESP32-P4-ETH) */
#define ISO_SD_PIN_D0         GPIO_NUM_39
#define ISO_SD_PIN_D1         GPIO_NUM_40
#define ISO_SD_PIN_D2         GPIO_NUM_41
#define ISO_SD_PIN_D3         GPIO_NUM_42
#define ISO_SD_PIN_CLK        GPIO_NUM_43
#define ISO_SD_PIN_CMD        GPIO_NUM_44
#define ISO_SD_PIN_PWR        GPIO_NUM_45 /* PMOS Q1 gate: 0 = power ON, 1 = power OFF */
#define ISO_SD_LDO_CHAN_ID    4           /* ESP_LDO_VO4 powers 51k pull-ups on D0..D3/CMD */

#define ISO_SD_PWR_OFF_MS     100u
#define ISO_SD_PWR_ON_MS      20u
#define ISO_SD_MON_STACK_SIZE 4096u
#define ISO_SD_MON_TASK_PRIO  3u
#define ISO_SD_ROOT_DIR       "/iso"

#ifndef CONFIG_SD_MAX_FREQ_KHZ
#define CONFIG_SD_MAX_FREQ_KHZ 40000
#endif
#ifndef CONFIG_SD_MONITOR_POLL_MS
#define CONFIG_SD_MONITOR_POLL_MS 2000
#endif

static const char *TAG = "iso_sd";

struct iso_file {
    FIL fil;
};

struct iso_dir {
    FF_DIR dir;
    char base_path[ISO_PATH_MAX];
};

typedef struct {
    bool initialized;
    bool shutdown_req;
    SemaphoreHandle_t meta_lock; /* Order: meta_lock -> io_lock -> fs calls */
    SemaphoreHandle_t io_lock;   /* Serializes diskio + blockdev.read_sectors + SDMMC HW */
    TaskHandle_t mon_task;

    iso_sd_ctrl_state_t ctrl;
    _Atomic uint64_t total_sectors;

    iso_sd_config_t cfg;
    sd_pwr_ctrl_handle_t pwr_ctrl;
    sdmmc_card_t card;
    bool host_inited;
    bool slot_inited;
    bool card_present;
    bool fat_mounted;
    BYTE pdrv;
    char drv_root[4]; /* "0:" + NUL */
    FATFS fatfs;
    uint32_t cluster_bytes;
    uint64_t total_bytes;
    uint64_t cached_free_bytes;
} iso_sd_ctx_t;

static iso_sd_ctx_t s_sd = {
    .pdrv = FF_DRV_NOT_USED,
};

static void *sd_alloc_buf(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

static void sd_set_card_power(bool on) {
    /* Low = PMOS Q1 ON (3.3V supplied to card); High = PMOS OFF. */
    (void)gpio_set_level(ISO_SD_PIN_PWR, on ? 0 : 1);
}

static esp_err_t sd_config_power_gpio(void) {
    sd_set_card_power(false);
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)ISO_SD_PIN_PWR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&pwr_cfg);
}

/* ---- Serialized FatFs diskio (acquires io_lock; NEVER calls force_eject!) ---- */

static DSTATUS sd_diskio_initialize(BYTE pdrv) {
    if (pdrv != s_sd.pdrv || !s_sd.io_lock) {
        return STA_NOINIT;
    }
    xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    DSTATUS st = STA_NOINIT;
    if (s_sd.card_present) {
        esp_err_t err = sdmmc_get_status(&s_sd.card);
        if (err == ESP_OK) {
            st = 0;
        } else {
            iso_sd_state_latch_io_fault(&s_sd.ctrl);
        }
    }
    xSemaphoreGive(s_sd.io_lock);
    return st;
}

static DSTATUS sd_diskio_status(BYTE pdrv) {
    if (pdrv != s_sd.pdrv || !s_sd.io_lock) {
        return STA_NOINIT;
    }
    xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    DSTATUS st = s_sd.card_present ? 0 : STA_NOINIT;
    xSemaphoreGive(s_sd.io_lock);
    return st;
}

static DRESULT sd_diskio_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    if (pdrv != s_sd.pdrv || !buff || !s_sd.io_lock) {
        return RES_PARERR;
    }
    if (count == 0u) {
        return RES_OK;
    }
    xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    DRESULT res = RES_NOTRDY;
    if (s_sd.card_present) {
        esp_err_t err = sdmmc_read_sectors(&s_sd.card, buff, (size_t)sector, (size_t)count);
        if (err == ESP_OK) {
            res = RES_OK;
        } else {
            ESP_LOGE(TAG, "diskio read failed (sec=%" PRIu32 " cnt=%u err=%s)",
                     (uint32_t)sector, (unsigned)count, esp_err_to_name(err));
            iso_sd_state_latch_io_fault(&s_sd.ctrl);
            res = RES_ERROR;
        }
    }
    xSemaphoreGive(s_sd.io_lock);
    return res;
}

static DRESULT sd_diskio_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
    if (pdrv != s_sd.pdrv || !buff || !s_sd.io_lock) {
        return RES_PARERR;
    }
    if (count == 0u) {
        return RES_OK;
    }
    xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    DRESULT res = RES_NOTRDY;
    if (s_sd.card_present) {
        esp_err_t err = sdmmc_write_sectors(&s_sd.card, buff, (size_t)sector, (size_t)count);
        if (err == ESP_OK) {
            res = RES_OK;
        } else {
            ESP_LOGE(TAG, "diskio write failed (sec=%" PRIu32 " cnt=%u err=%s)",
                     (uint32_t)sector, (unsigned)count, esp_err_to_name(err));
            iso_sd_state_latch_io_fault(&s_sd.ctrl);
            res = RES_ERROR;
        }
    }
    xSemaphoreGive(s_sd.io_lock);
    return res;
}

static DRESULT sd_diskio_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != s_sd.pdrv || !s_sd.io_lock) {
        return RES_PARERR;
    }
    xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    DRESULT res = RES_ERROR;
    if (!s_sd.card_present) {
        xSemaphoreGive(s_sd.io_lock);
        return RES_NOTRDY;
    }
    switch (cmd) {
        case CTRL_SYNC:
            res = RES_OK;
            break;
        case GET_SECTOR_COUNT:
            if (buff) {
                *((LBA_t *)buff) = (LBA_t)s_sd.card.csd.capacity;
                res = RES_OK;
            } else {
                res = RES_PARERR;
            }
            break;
        case GET_SECTOR_SIZE:
            if (buff) {
                *((WORD *)buff) = (WORD)s_sd.card.csd.sector_size;
                res = RES_OK;
            } else {
                res = RES_PARERR;
            }
            break;
        case GET_BLOCK_SIZE:
            if (buff) {
                *((DWORD *)buff) = 1u;
                res = RES_OK;
            } else {
                res = RES_PARERR;
            }
            break;
#if FF_USE_TRIM
        case CTRL_TRIM: {
            if (!buff) {
                res = RES_PARERR;
                break;
            }
            DWORD *range = (DWORD *)buff;
            if (range[1] < range[0]) {
                res = RES_PARERR;
                break;
            }
            if (sdmmc_can_trim(&s_sd.card) != ESP_OK) {
                res = RES_OK; /* Card without TRIM support can ignore discard hints safely. */
                break;
            }
            sdmmc_erase_arg_t arg =
                (sdmmc_can_discard(&s_sd.card) == ESP_OK) ? SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG;
            uint32_t sectors = (uint32_t)(range[1] - range[0] + 1u);
            esp_err_t err = sdmmc_erase_sectors(&s_sd.card, (size_t)range[0], (size_t)sectors, arg);
            if (err == ESP_OK) {
                res = RES_OK;
            } else {
                ESP_LOGE(TAG, "diskio TRIM failed: %s", esp_err_to_name(err));
                iso_sd_state_latch_io_fault(&s_sd.ctrl);
                res = RES_ERROR;
            }
            break;
        }
#endif
        default:
            res = RES_PARERR;
            break;
    }
    xSemaphoreGive(s_sd.io_lock);
    return res;
}

static void sd_unmount_hw_locked(void) {
    /* Assumes s_sd.meta_lock held. Takes s_sd.io_lock when tearing down card/host. */
    if (s_sd.fat_mounted) {
        (void)f_mount(NULL, s_sd.drv_root, 0);
        s_sd.fat_mounted = false;
    }
    if (s_sd.pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(s_sd.pdrv);
        s_sd.pdrv = FF_DRV_NOT_USED;
    }
    if (s_sd.io_lock) {
        xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    }
    s_sd.card_present = false;
    atomic_store_explicit(&s_sd.total_sectors, 0u, memory_order_release);
    memset(&s_sd.card, 0, sizeof(s_sd.card));
    if (s_sd.host_inited) {
        (void)sdmmc_host_deinit();
        s_sd.host_inited = false;
        s_sd.slot_inited = false;
    }
    if (s_sd.pwr_ctrl) {
        (void)sd_pwr_ctrl_del_on_chip_ldo(s_sd.pwr_ctrl);
        s_sd.pwr_ctrl = NULL;
    }
    sd_set_card_power(false);
    if (s_sd.io_lock) {
        xSemaphoreGive(s_sd.io_lock);
    }
    s_sd.cluster_bytes = 0u;
    s_sd.total_bytes = 0u;
    s_sd.cached_free_bytes = 0u;
}

static esp_err_t sd_mount_hw_locked(void) {
    /* Assumes s_sd.meta_lock held. */
    sd_unmount_hw_locked();
    (void)iso_sd_state_take_io_fault(&s_sd.ctrl);

    /* Ensure at least ISO_SD_PWR_OFF_MS off before powering ON PMOS Q1. */
    vTaskDelay(pdMS_TO_TICKS(ISO_SD_PWR_OFF_MS));
    sd_set_card_power(true);
    vTaskDelay(pdMS_TO_TICKS(ISO_SD_PWR_ON_MS));

    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = ISO_SD_LDO_CHAN_ID,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_sd.pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO_VO%d init failed: %s", ISO_SD_LDO_CHAN_ID, esp_err_to_name(err));
        iso_sd_state_set(&s_sd.ctrl, ISO_SD_ERROR);
        sd_unmount_hw_locked();
        return err;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    /* Keep strictly <= 4-bit flags so GPIO45 (D4 in 8-bit) stays under PMOS power switch control. */
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.max_freq_khz = CONFIG_SD_MAX_FREQ_KHZ;
    host.pwr_ctrl_handle = s_sd.pwr_ctrl;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = ISO_SD_PIN_CLK;
    slot.cmd = ISO_SD_PIN_CMD;
    slot.d0 = ISO_SD_PIN_D0;
    slot.d1 = ISO_SD_PIN_D1;
    slot.d2 = ISO_SD_PIN_D2;
    slot.d3 = ISO_SD_PIN_D3;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    /* Board already has 51k external pull-ups on D0..D3/CMD -> LDO_VO4; avoid conflicting internal pulls. */
    slot.flags = 0;

    xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    err = sdmmc_host_init();
    if (err == ESP_OK) {
        s_sd.host_inited = true;
        err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot);
    }
    if (err == ESP_OK) {
        s_sd.slot_inited = true;
        err = sdmmc_card_init(&host, &s_sd.card);
    }
    if (err == ESP_OK) {
        s_sd.card_present = true;
        atomic_store_explicit(&s_sd.total_sectors, (uint64_t)s_sd.card.csd.capacity,
                              memory_order_release);
    }
    xSemaphoreGive(s_sd.io_lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microSD probe failed (no card or init err): %s", esp_err_to_name(err));
        iso_sd_state_set(&s_sd.ctrl,
                         (err == ESP_ERR_TIMEOUT) ? ISO_SD_NO_CARD : ISO_SD_ERROR);
        sd_unmount_hw_locked();
        return err;
    }

    err = ff_diskio_get_drive(&s_sd.pdrv);
    if (err != ESP_OK || s_sd.pdrv > 9u) {
        ESP_LOGE(TAG, "No free FatFs pdrv available (%s)", esp_err_to_name(err));
        s_sd.pdrv = FF_DRV_NOT_USED;
        iso_sd_state_set(&s_sd.ctrl, ISO_SD_ERROR);
        sd_unmount_hw_locked();
        return ESP_ERR_NOT_FOUND;
    }

    static const ff_diskio_impl_t s_diskio_impl = {
        .init = sd_diskio_initialize,
        .status = sd_diskio_status,
        .read = sd_diskio_read,
        .write = sd_diskio_write,
        .ioctl = sd_diskio_ioctl,
    };
    ff_diskio_register(s_sd.pdrv, &s_diskio_impl);
    s_sd.drv_root[0] = (char)('0' + s_sd.pdrv);
    s_sd.drv_root[1] = ':';
    s_sd.drv_root[2] = '\0';

    FRESULT fr = f_mount(&s_sd.fatfs, s_sd.drv_root, 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "f_mount(%s) failed: fr=%d", s_sd.drv_root, (int)fr);
        iso_sd_state_set(&s_sd.ctrl,
                         (fr == FR_NO_FILESYSTEM) ? ISO_SD_UNSUPPORTED_FS : ISO_SD_ERROR);
        sd_unmount_hw_locked();
        return (fr == FR_NO_FILESYSTEM) ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }
    s_sd.fat_mounted = true;

    uint32_t cluster_bytes = 0u;
    int vrc = iso_sd_validate_mount(s_sd.fatfs.fs_type == FS_FAT32,
                                    (uint32_t)s_sd.card.csd.sector_size,
                                    (uint32_t)s_sd.fatfs.csize,
                                    &cluster_bytes);
    if (vrc != ISO_OK) {
        ESP_LOGW(TAG, "Unsupported card FS (fs_type=%u sector=%d csize=%u): must be FAT32 with cluster >= 2048B",
                 (unsigned)s_sd.fatfs.fs_type, s_sd.card.csd.sector_size,
                 (unsigned)s_sd.fatfs.csize);
        iso_sd_state_set(&s_sd.ctrl, ISO_SD_UNSUPPORTED_FS);
        sd_unmount_hw_locked();
        return ESP_ERR_NOT_SUPPORTED;
    }

    DWORD fre_clust = 0u;
    FATFS *fs_out = NULL;
    fr = f_getfree(s_sd.drv_root, &fre_clust, &fs_out);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_getfree failed: fr=%d", (int)fr);
        iso_sd_state_set(&s_sd.ctrl, ISO_SD_ERROR);
        sd_unmount_hw_locked();
        return ESP_FAIL;
    }

    uint64_t total_clusters = (s_sd.fatfs.n_fatent >= 2u) ? (uint64_t)(s_sd.fatfs.n_fatent - 2u) : 0ULL;
    s_sd.cluster_bytes = cluster_bytes;
    s_sd.total_bytes = total_clusters * (uint64_t)cluster_bytes;
    s_sd.cached_free_bytes = (uint64_t)fre_clust * (uint64_t)cluster_bytes;

    /* Ensure /iso root exists so iso_catalog / self-test never see missing root. */
    char iso_dir[ISO_PATH_MAX + 4u];
    if (iso_sd_format_fatfs_path(s_sd.drv_root[0], ISO_SD_ROOT_DIR, iso_dir, sizeof(iso_dir)) == ISO_OK) {
        FRESULT mk = f_mkdir(iso_dir);
        if (mk != FR_OK && mk != FR_EXIST) {
            ESP_LOGW(TAG, "f_mkdir(%s) failed: fr=%d", iso_dir, (int)mk);
        }
    }

    uint32_t real_khz = s_sd.card.real_freq_khz;
    if (host.get_real_freq) {
        int khz = 0;
        if (host.get_real_freq(SDMMC_HOST_SLOT_0, &khz) == ESP_OK && khz > 0) {
            real_khz = (uint32_t)khz;
        }
    }

    iso_sd_state_set(&s_sd.ctrl, ISO_SD_READY);
    ESP_LOGI(TAG,
             "microSD mounted (FAT32, cluster=%" PRIu32 "B, clk=%" PRIu32 "kHz, bus=%s, "
             "sectors=%" PRIu64 ", total=%" PRIu64 " MiB, free=%" PRIu64 " MiB)",
             s_sd.cluster_bytes,
             real_khz,
             (s_sd.card.is_ddr ? "4-bit DDR" : "4-bit"),
             (uint64_t)s_sd.card.csd.capacity,
             s_sd.total_bytes / (1024ULL * 1024ULL),
             s_sd.cached_free_bytes / (1024ULL * 1024ULL));
    return ESP_OK;
}

/* ---- Monitor task (detects card removal / IO faults & re-probes empty slot) ---- */

static void sd_monitor_task(void *arg) {
    (void)arg;
    const TickType_t poll_ticks = pdMS_TO_TICKS(CONFIG_SD_MONITOR_POLL_MS) > 0
                                      ? pdMS_TO_TICKS(CONFIG_SD_MONITOR_POLL_MS)
                                      : 1;

    while (!s_sd.shutdown_req) {
        vTaskDelay(poll_ticks);
        if (s_sd.shutdown_req) {
            break;
        }

        iso_sd_state_t cur_st = iso_sd_state_get(&s_sd.ctrl);
        if (cur_st == ISO_SD_READY) {
            bool unhealthy = iso_sd_state_take_io_fault(&s_sd.ctrl);
            if (!unhealthy && s_sd.io_lock) {
                xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
                if (!s_sd.card_present || sdmmc_get_status(&s_sd.card) != ESP_OK) {
                    unhealthy = true;
                }
                xSemaphoreGive(s_sd.io_lock);
            }
            if (unhealthy) {
                ESP_LOGW(TAG, "microSD fault/removal detected; force-ejecting media and unmounting");
                /* Bump epoch and notify force_eject BEFORE taking meta_lock/tearing down HW. */
                (void)iso_sd_state_epoch_bump(&s_sd.ctrl);
                iso_sd_state_set(&s_sd.ctrl, ISO_SD_NO_CARD);
                if (s_sd.cfg.on_force_eject) {
                    s_sd.cfg.on_force_eject(s_sd.cfg.on_force_eject_ctx);
                }
                xSemaphoreTake(s_sd.meta_lock, portMAX_DELAY);
                sd_unmount_hw_locked();
                xSemaphoreGive(s_sd.meta_lock);
            }
        } else {
            /* Try re-probing (handles hot-inserted card or transient init error). */
            xSemaphoreTake(s_sd.meta_lock, portMAX_DELAY);
            esp_err_t m_err = sd_mount_hw_locked();
            xSemaphoreGive(s_sd.meta_lock);
            if (m_err == ESP_OK && s_sd.cfg.on_ready) {
                s_sd.cfg.on_ready(s_sd.cfg.on_ready_ctx);
            }
        }
    }

    s_sd.mon_task = NULL;
    vTaskDelete(NULL);
}

/* ---- iso_blockdev_t ------------------------------------------------------ */

static uint64_t blk_sector_count(void *ctx) {
    (void)ctx;
    if (iso_sd_state_get(&s_sd.ctrl) != ISO_SD_READY) {
        return 0u;
    }
    return atomic_load_explicit(&s_sd.total_sectors, memory_order_acquire);
}

static uint32_t blk_epoch(void *ctx) {
    (void)ctx;
    return iso_sd_state_epoch(&s_sd.ctrl);
}

static void blk_epoch_bump(void *ctx) {
    (void)ctx;
    (void)iso_sd_state_epoch_bump(&s_sd.ctrl);
}

static int blk_read_sectors(void *ctx, uint32_t epoch, uint64_t lba, uint32_t n, uint8_t *out) {
    (void)ctx;
    if (!out && n > 0u) {
        return ISO_ERR_INVALID;
    }
    if (!s_sd.io_lock) {
        return ISO_ERR_NO_CARD;
    }

    xSemaphoreTake(s_sd.io_lock, portMAX_DELAY);
    if (iso_sd_state_epoch(&s_sd.ctrl) != epoch) {
        xSemaphoreGive(s_sd.io_lock);
        return ISO_ERR_EPOCH;
    }
    if (iso_sd_state_get(&s_sd.ctrl) != ISO_SD_READY || !s_sd.card_present) {
        xSemaphoreGive(s_sd.io_lock);
        return ISO_ERR_NO_CARD;
    }
    uint64_t cap = atomic_load_explicit(&s_sd.total_sectors, memory_order_acquire);
    if (n == 0u) {
        xSemaphoreGive(s_sd.io_lock);
        return (lba <= cap) ? ISO_OK : ISO_ERR_INVALID;
    }
    if (lba >= cap || (uint64_t)n > cap - lba || lba > (uint64_t)SIZE_MAX) {
        xSemaphoreGive(s_sd.io_lock);
        return ISO_ERR_INVALID;
    }

    esp_err_t err = sdmmc_read_sectors(&s_sd.card, out, (size_t)lba, (size_t)n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read_sectors failed (lba=%" PRIu64 " n=%" PRIu32 " err=%s)",
                 lba, n, esp_err_to_name(err));
        iso_sd_state_latch_io_fault(&s_sd.ctrl);
        xSemaphoreGive(s_sd.io_lock);
        return ISO_ERR_IO;
    }
    xSemaphoreGive(s_sd.io_lock);
    return ISO_OK;
}

static const iso_blockdev_t s_blockdev = {
    .ctx = NULL,
    .sector_count = blk_sector_count,
    .epoch = blk_epoch,
    .epoch_bump = blk_epoch_bump,
    .read_sectors = blk_read_sectors,
};

/* ---- iso_fs_t ------------------------------------------------------------ */

static int fs_prepare_path(const char *path, char *out_drv_path, size_t out_cap) {
    if (iso_sd_state_get(&s_sd.ctrl) != ISO_SD_READY || !s_sd.fat_mounted ||
        s_sd.pdrv == FF_DRV_NOT_USED) {
        return ISO_ERR_NO_CARD;
    }
    return iso_sd_format_fatfs_path((char)('0' + s_sd.pdrv), path, out_drv_path, out_cap);
}

static int fs_read_first_cluster(const char *drv_path, uint32_t *out_cluster) {
    FIL fil;
    memset(&fil, 0, sizeof(fil));
    FRESULT fr = f_open(&fil, drv_path, FA_READ);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    *out_cluster = (uint32_t)fil.obj.sclust;
    (void)f_close(&fil);
    return ISO_OK;
}

static int fs_mount_info(void *ctx, iso_mount_info_t *out) {
    (void)ctx;
    if (!out) {
        return ISO_ERR_INVALID;
    }
    iso_sd_state_t st = iso_sd_state_get(&s_sd.ctrl);
    if (st == ISO_SD_UNSUPPORTED_FS) {
        return ISO_ERR_UNSUPPORTED_FS;
    }
    if (st != ISO_SD_READY || !s_sd.fat_mounted) {
        return ISO_ERR_NO_CARD;
    }
    DWORD fre_clust = 0u;
    FATFS *fs_out = NULL;
    FRESULT fr = f_getfree(s_sd.drv_root, &fre_clust, &fs_out);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    s_sd.cached_free_bytes = (uint64_t)fre_clust * (uint64_t)s_sd.cluster_bytes;
    out->fat32 = (s_sd.fatfs.fs_type == FS_FAT32);
    out->cluster_bytes = s_sd.cluster_bytes;
    out->total_bytes = s_sd.total_bytes;
    out->free_bytes = s_sd.cached_free_bytes;
    return ISO_OK;
}

static int fs_mkdir(void *ctx, const char *path) {
    (void)ctx;
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    return iso_sd_fresult_to_err((int)f_mkdir(dpath));
}

static int fs_remove(void *ctx, const char *path) {
    (void)ctx;
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    return iso_sd_fresult_to_err((int)f_unlink(dpath));
}

static int fs_rename(void *ctx, const char *from, const char *to) {
    (void)ctx;
    char dfrom[ISO_PATH_MAX + 4u];
    char dto[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(from, dfrom, sizeof(dfrom));
    if (rc != ISO_OK) {
        return rc;
    }
    rc = fs_prepare_path(to, dto, sizeof(dto));
    if (rc != ISO_OK) {
        return rc;
    }
    return iso_sd_fresult_to_err((int)f_rename(dfrom, dto));
}

static int fs_stat(void *ctx, const char *path, iso_stat_t *out) {
    (void)ctx;
    if (!out) {
        return ISO_ERR_INVALID;
    }
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    FILINFO fno;
    memset(&fno, 0, sizeof(fno));
    FRESULT fr = f_stat(dpath, &fno);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    out->is_dir = (fno.fattrib & AM_DIR) != 0;
    out->size = (uint64_t)fno.fsize;
    out->first_cluster = 0u;
    if (!out->is_dir && fno.fsize > 0u) {
        rc = fs_read_first_cluster(dpath, &out->first_cluster);
        if (rc != ISO_OK) {
            return rc;
        }
    }
    return ISO_OK;
}

static int fs_dir_open(void *ctx, const char *path, iso_dir_t **out) {
    (void)ctx;
    if (!out) {
        return ISO_ERR_INVALID;
    }
    *out = NULL;
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    iso_dir_t *d = (iso_dir_t *)calloc(1u, sizeof(iso_dir_t));
    if (!d) {
        return ISO_ERR_NOMEM;
    }
    size_t plen = strlen(path);
    memcpy(d->base_path, path, plen + 1u);

    FRESULT fr = f_opendir(&d->dir, dpath);
    if (fr != FR_OK) {
        free(d);
        return iso_sd_fresult_to_err((int)fr);
    }
    *out = d;
    return ISO_OK;
}

static int fs_dir_next(void *ctx, iso_dir_t *dir, char *name, iso_stat_t *st) {
    (void)ctx;
    if (!dir || !name || !st) {
        return ISO_ERR_INVALID;
    }
    if (iso_sd_state_get(&s_sd.ctrl) != ISO_SD_READY || !s_sd.fat_mounted ||
        s_sd.pdrv == FF_DRV_NOT_USED) {
        return ISO_ERR_NO_CARD;
    }

    for (;;) {
        FILINFO fno;
        memset(&fno, 0, sizeof(fno));
        FRESULT fr = f_readdir(&dir->dir, &fno);
        if (fr != FR_OK) {
            return iso_sd_fresult_to_err((int)fr);
        }
        if (fno.fname[0] == '\0') {
            return 1;
        }
        if (strcmp(fno.fname, ".") == 0 || strcmp(fno.fname, "..") == 0) {
            continue;
        }
        size_t nlen = strlen(fno.fname);
        if (nlen == 0u || nlen > ISO_NAME_MAX) {
            return ISO_ERR_INVALID;
        }
        memcpy(name, fno.fname, nlen + 1u);
        st->is_dir = (fno.fattrib & AM_DIR) != 0;
        st->size = (uint64_t)fno.fsize;
        st->first_cluster = 0u;
        if (!st->is_dir && fno.fsize > 0u) {
            char full_path[ISO_PATH_MAX];
            char full_dpath[ISO_PATH_MAX + 4u];
            if (iso_sd_join_path(dir->base_path, fno.fname, full_path, sizeof(full_path)) == ISO_OK &&
                iso_sd_format_fatfs_path((char)('0' + s_sd.pdrv), full_path, full_dpath, sizeof(full_dpath)) == ISO_OK) {
                int crc = fs_read_first_cluster(full_dpath, &st->first_cluster);
                if (crc != ISO_OK) {
                    return crc;
                }
            }
        }
        return ISO_OK;
    }
}

static void fs_dir_close(void *ctx, iso_dir_t *dir) {
    (void)ctx;
    if (!dir) {
        return;
    }
    (void)f_closedir(&dir->dir);
    free(dir);
}

static int fs_open_for_write(void *ctx, const char *path, bool append, iso_file_t **out) {
    (void)ctx;
    if (!out) {
        return ISO_ERR_INVALID;
    }
    *out = NULL;
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    iso_file_t *f = (iso_file_t *)calloc(1u, sizeof(iso_file_t));
    if (!f) {
        return ISO_ERR_NOMEM;
    }
    BYTE mode = append ? (BYTE)(FA_WRITE | FA_OPEN_APPEND) : (BYTE)(FA_WRITE | FA_CREATE_NEW);
    FRESULT fr = f_open(&f->fil, dpath, mode);
    if (fr != FR_OK) {
        free(f);
        return iso_sd_fresult_to_err((int)fr);
    }
    *out = f;
    return ISO_OK;
}

static int fs_write(void *ctx, iso_file_t *f, const void *data, size_t len) {
    (void)ctx;
    if (!f || (!data && len > 0u)) {
        return ISO_ERR_INVALID;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0u;
    while (off < len) {
        size_t rem = len - off;
        UINT chunk = (rem > (size_t)UINT_MAX) ? UINT_MAX : (UINT)rem;
        UINT bw = 0u;
        FRESULT fr = f_write(&f->fil, p + off, chunk, &bw);
        if (fr != FR_OK) {
            return iso_sd_fresult_to_err((int)fr);
        }
        if (bw < chunk) {
            /* FatFs signals full volume via FR_OK and bw < btw. */
            return ISO_ERR_NO_SPACE;
        }
        off += (size_t)bw;
    }
    return ISO_OK;
}

static int fs_sync(void *ctx, iso_file_t *f) {
    (void)ctx;
    if (!f) {
        return ISO_ERR_INVALID;
    }
    return iso_sd_fresult_to_err((int)f_sync(&f->fil));
}

static int fs_close(void *ctx, iso_file_t *f) {
    (void)ctx;
    if (!f) {
        return ISO_OK;
    }
    FRESULT fr = f_close(&f->fil);
    free(f);
    return iso_sd_fresult_to_err((int)fr);
}

static int fs_truncate(void *ctx, const char *path, uint64_t size) {
    (void)ctx;
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    FILINFO fno;
    memset(&fno, 0, sizeof(fno));
    FRESULT fr = f_stat(dpath, &fno);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    if ((fno.fattrib & AM_DIR) != 0 || (uint64_t)fno.fsize < size || size > (uint64_t)UINT32_MAX) {
        /* Reject size > fsize so f_lseek never extends the file in write mode. */
        return ISO_ERR_INVALID;
    }
    if ((uint64_t)fno.fsize == size) {
        return ISO_OK;
    }
    FIL fil;
    memset(&fil, 0, sizeof(fil));
    fr = f_open(&fil, dpath, FA_WRITE | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    fr = f_lseek(&fil, (FSIZE_t)size);
    if (fr == FR_OK) {
        fr = f_truncate(&fil);
    }
    if (fr == FR_OK) {
        fr = f_sync(&fil);
    }
    FRESULT cfr = f_close(&fil);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    return iso_sd_fresult_to_err((int)cfr);
}

static int fs_overwrite(void *ctx, const char *path, const void *data, size_t len) {
    (void)ctx;
    if (!data && len > 0u) {
        return ISO_ERR_INVALID;
    }
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    FIL fil;
    memset(&fil, 0, sizeof(fil));
    FRESULT fr = f_open(&fil, dpath, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0u;
    int err = ISO_OK;
    while (off < len) {
        size_t rem = len - off;
        UINT chunk = (rem > (size_t)UINT_MAX) ? UINT_MAX : (UINT)rem;
        UINT bw = 0u;
        fr = f_write(&fil, p + off, chunk, &bw);
        if (fr != FR_OK) {
            err = iso_sd_fresult_to_err((int)fr);
            break;
        }
        if (bw < chunk) {
            err = ISO_ERR_NO_SPACE;
            break;
        }
        off += (size_t)bw;
    }
    if (err == ISO_OK) {
        fr = f_sync(&fil);
        if (fr != FR_OK) {
            err = iso_sd_fresult_to_err((int)fr);
        }
    }
    FRESULT cfr = f_close(&fil);
    return (err != ISO_OK) ? err : iso_sd_fresult_to_err((int)cfr);
}

static int fs_open_for_read(void *ctx, const char *path, iso_file_t **out) {
    (void)ctx;
    if (!out) {
        return ISO_ERR_INVALID;
    }
    *out = NULL;
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    iso_file_t *f = (iso_file_t *)calloc(1u, sizeof(iso_file_t));
    if (!f) {
        return ISO_ERR_NOMEM;
    }
    FRESULT fr = f_open(&f->fil, dpath, FA_READ);
    if (fr != FR_OK) {
        free(f);
        return iso_sd_fresult_to_err((int)fr);
    }
    *out = f;
    return ISO_OK;
}

static int fs_read(void *ctx, iso_file_t *f, void *buf, size_t len, size_t *got) {
    (void)ctx;
    if (!f || !got || (!buf && len > 0u)) {
        return ISO_ERR_INVALID;
    }
    *got = 0u;
    uint8_t *p = (uint8_t *)buf;
    while (*got < len) {
        size_t rem = len - *got;
        UINT chunk = (rem > (size_t)UINT_MAX) ? UINT_MAX : (UINT)rem;
        UINT br = 0u;
        FRESULT fr = f_read(&f->fil, p + *got, chunk, &br);
        if (fr != FR_OK) {
            return iso_sd_fresult_to_err((int)fr);
        }
        *got += (size_t)br;
        if (br < chunk) {
            break; /* EOF */
        }
    }
    return ISO_OK;
}

static int fs_get_extents(void *ctx, const char *path, iso_extent_t *out, size_t max, size_t *n) {
    (void)ctx;
    if (!n || (max > 0u && !out)) {
        return ISO_ERR_INVALID;
    }
    *n = 0u;
    char dpath[ISO_PATH_MAX + 4u];
    int rc = fs_prepare_path(path, dpath, sizeof(dpath));
    if (rc != ISO_OK) {
        return rc;
    }
    FILINFO fno;
    memset(&fno, 0, sizeof(fno));
    FRESULT fr = f_stat(dpath, &fno);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    if ((fno.fattrib & AM_DIR) != 0) {
        return ISO_ERR_INVALID;
    }

    FIL fil;
    memset(&fil, 0, sizeof(fil));
    fr = f_open(&fil, dpath, FA_READ);
    if (fr != FR_OK) {
        return iso_sd_fresult_to_err((int)fr);
    }
    if (fil.obj.sclust == 0u || !fil.obj.fs) {
        (void)f_close(&fil);
        *n = 0u;
        return ISO_OK;
    }

    size_t cap_frags = (max > (SIZE_MAX - 2u) / 2u) ? 4096u : max;
    size_t cltbl_words = (cap_frags == 0u) ? 4u : (cap_frags * 2u + 2u);
    DWORD *cltbl = (DWORD *)sd_alloc_buf(cltbl_words * sizeof(DWORD));
    if (!cltbl) {
        (void)f_close(&fil);
        return ISO_ERR_NOMEM;
    }
    cltbl[0] = (DWORD)cltbl_words;
    fil.cltbl = cltbl;

    fr = f_lseek(&fil, CREATE_LINKMAP);
    uint64_t db = (uint64_t)fil.obj.fs->database;
    uint32_t csize = (uint32_t)fil.obj.fs->csize;
    (void)f_close(&fil);

    if (fr == FR_NOT_ENOUGH_CORE) {
        uint32_t ulen = (uint32_t)cltbl[0];
        *n = (ulen >= 2u) ? (size_t)((ulen - 2u) / 2u) : 0u;
        free(cltbl);
        return ISO_ERR_TOO_FRAGMENTED;
    }
    if (fr != FR_OK) {
        free(cltbl);
        return iso_sd_fresult_to_err((int)fr);
    }

    rc = iso_sd_clmt_to_extents((const uint32_t *)cltbl, cltbl_words, db, csize, out, max, n);
    free(cltbl);
    return rc;
}

static const iso_fs_t s_fs = {
    .ctx = NULL,
    .mount_info = fs_mount_info,
    .mkdir = fs_mkdir,
    .remove = fs_remove,
    .rename = fs_rename,
    .stat = fs_stat,
    .dir_open = fs_dir_open,
    .dir_next = fs_dir_next,
    .dir_close = fs_dir_close,
    .open_for_write = fs_open_for_write,
    .write = fs_write,
    .sync = fs_sync,
    .close = fs_close,
    .truncate = fs_truncate,
    .overwrite = fs_overwrite,
    .open_for_read = fs_open_for_read,
    .read = fs_read,
    .get_extents = fs_get_extents,
};

/* ---- iso_sd_status_t / iso_clock_t --------------------------------------- */

static iso_sd_state_t sd_status_get(void *ctx) {
    (void)ctx;
    return iso_sd_state_get(&s_sd.ctrl);
}

static const iso_sd_status_t s_sd_status = {
    .ctx = NULL,
    .state = sd_status_get,
};

static uint64_t sd_clock_now_ms(void *ctx) {
    (void)ctx;
    int64_t us = esp_timer_get_time();
    return (us > 0) ? (uint64_t)(us / 1000LL) : 0ULL;
}

static const iso_clock_t s_clock = {
    .ctx = NULL,
    .now_ms = sd_clock_now_ms,
};

/* ---- Public API ---------------------------------------------------------- */

esp_err_t iso_sd_init(const iso_sd_config_t *cfg) {
    if (s_sd.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_sd, 0, sizeof(s_sd));
    s_sd.pdrv = FF_DRV_NOT_USED;
    iso_sd_state_init(&s_sd.ctrl);
    if (cfg) {
        s_sd.cfg = *cfg;
    }

    s_sd.meta_lock = xSemaphoreCreateMutex();
    s_sd.io_lock = xSemaphoreCreateMutex();
    if (!s_sd.meta_lock || !s_sd.io_lock) {
        iso_sd_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(sd_config_power_gpio(), TAG, "GPIO45 power switch config failed");

    xSemaphoreTake(s_sd.meta_lock, portMAX_DELAY);
    esp_err_t mount_err = sd_mount_hw_locked();
    xSemaphoreGive(s_sd.meta_lock);
    if (mount_err == ESP_OK && s_sd.cfg.on_ready) {
        s_sd.cfg.on_ready(s_sd.cfg.on_ready_ctx);
    }

    s_sd.shutdown_req = false;
    if (xTaskCreate(sd_monitor_task, "iso_sd_mon", ISO_SD_MON_STACK_SIZE, NULL,
                    ISO_SD_MON_TASK_PRIO, &s_sd.mon_task) != pdPASS) {
        iso_sd_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_sd.initialized = true;
    ESP_LOGI(TAG, "iso_sd initialized (initial state=%d, initial_err=%s)",
             (int)iso_sd_state_get(&s_sd.ctrl), esp_err_to_name(mount_err));
    return ESP_OK;
}

void iso_sd_deinit(void) {
    s_sd.shutdown_req = true;
    for (int i = 0; i < 50 && s_sd.mon_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_sd.meta_lock) {
        xSemaphoreTake(s_sd.meta_lock, portMAX_DELAY);
        sd_unmount_hw_locked();
        xSemaphoreGive(s_sd.meta_lock);
        vSemaphoreDelete(s_sd.meta_lock);
        s_sd.meta_lock = NULL;
    } else {
        sd_unmount_hw_locked();
    }
    if (s_sd.io_lock) {
        vSemaphoreDelete(s_sd.io_lock);
        s_sd.io_lock = NULL;
    }
    s_sd.initialized = false;
}

const iso_blockdev_t *iso_sd_blockdev(void) {
    return &s_blockdev;
}

const iso_fs_t *iso_sd_fs(void) {
    return &s_fs;
}

const iso_sd_status_t *iso_sd_status(void) {
    return &s_sd_status;
}

const iso_clock_t *iso_sd_clock(void) {
    return &s_clock;
}

esp_err_t iso_sd_run_self_test(void) {
    if (!s_sd.initialized || iso_sd_state_get(&s_sd.ctrl) != ISO_SD_READY) {
        ESP_LOGW(TAG, "SD self-test skipped (sd_state=%d)", (int)iso_sd_state_get(&s_sd.ctrl));
        return ESP_ERR_INVALID_STATE;
    }

    static const char k_path[] = "/iso/_selftest.tmp";
    const size_t test_bytes = 65536u;
    const size_t sector_bytes = ISO_SD_SECTOR_BYTES;
    uint8_t *wr = (uint8_t *)sd_alloc_buf(test_bytes);
    uint8_t *rd_fs = (uint8_t *)sd_alloc_buf(test_bytes);
    uint8_t *rd_raw = (uint8_t *)sd_alloc_buf(test_bytes);
    if (!wr || !rd_fs || !rd_raw) {
        free(wr);
        free(rd_fs);
        free(rd_raw);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0u; i < test_bytes; i++) {
        wr[i] = (uint8_t)(((i * 131u) ^ (i >> 9)) & 0xFFu);
    }

    (void)s_fs.remove(NULL, k_path);
    iso_file_t *wf = NULL;
    int64_t t0 = esp_timer_get_time();
    int rc = s_fs.open_for_write(NULL, k_path, false, &wf);
    if (rc == ISO_OK) {
        rc = s_fs.write(NULL, wf, wr, test_bytes);
        if (rc == ISO_OK) {
            rc = s_fs.sync(NULL, wf);
        }
        int crc = s_fs.close(NULL, wf);
        if (rc == ISO_OK) {
            rc = crc;
        }
    }
    int64_t wr_us = esp_timer_get_time() - t0;

    iso_extent_t ext[64];
    size_t ext_n = 0u;
    int64_t clmt_us = 0;
    if (rc == ISO_OK) {
        int64_t t1 = esp_timer_get_time();
        rc = s_fs.get_extents(NULL, k_path, ext, 64u, &ext_n);
        clmt_us = esp_timer_get_time() - t1;
    }

    int64_t raw_us = 0;
    uint32_t epoch = s_blockdev.epoch(NULL);
    if (rc == ISO_OK) {
        iso_file_t *rf = NULL;
        rc = s_fs.open_for_read(NULL, k_path, &rf);
        if (rc == ISO_OK) {
            size_t got = 0u;
            rc = s_fs.read(NULL, rf, rd_fs, test_bytes, &got);
            (void)s_fs.close(NULL, rf);
            if (rc == ISO_OK && got != test_bytes) {
                rc = ISO_ERR_IO;
            }
        }
    }

    if (rc == ISO_OK) {
        size_t need_sectors = test_bytes / sector_bytes;
        size_t done_sectors = 0u;
        int64_t t2 = esp_timer_get_time();
        for (size_t i = 0u; i < ext_n && done_sectors < need_sectors; i++) {
            uint64_t take = ext[i].sector_count;
            if (done_sectors + take > need_sectors) {
                take = (uint64_t)(need_sectors - done_sectors);
            }
            rc = s_blockdev.read_sectors(NULL, epoch, ext[i].start_sector, (uint32_t)take,
                                         rd_raw + done_sectors * sector_bytes);
            if (rc != ISO_OK) {
                break;
            }
            done_sectors += (size_t)take;
        }
        raw_us = esp_timer_get_time() - t2;
        if (rc == ISO_OK && done_sectors != need_sectors) {
            rc = ISO_ERR_IO;
        }
    }

    bool fs_match = (rc == ISO_OK) && (memcmp(wr, rd_fs, test_bytes) == 0);
    bool raw_match = (rc == ISO_OK) && (memcmp(rd_fs, rd_raw, test_bytes) == 0);
    int epoch_rc = s_blockdev.read_sectors(NULL, epoch + 1u, 0u, 1u, rd_raw);

    (void)s_fs.remove(NULL, k_path);
    free(wr);
    free(rd_fs);
    free(rd_raw);

    ESP_LOGI(TAG,
             "SD self-test: rc=%d fs_match=%d raw_match=%d extents=%u "
             "wr=%" PRId64 "us clmt=%" PRId64 "us raw_rd=%" PRId64 "us epoch_guard=%d",
             rc, fs_match ? 1 : 0, raw_match ? 1 : 0, (unsigned)ext_n,
             wr_us, clmt_us, raw_us, epoch_rc);
    return (rc == ISO_OK && fs_match && raw_match && epoch_rc == ISO_ERR_EPOCH) ? ESP_OK : ESP_FAIL;
}

#else /* !CONFIG_SD_ENABLE */

esp_err_t iso_sd_init(const iso_sd_config_t *cfg) {
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void iso_sd_deinit(void) {}

const iso_blockdev_t *iso_sd_blockdev(void) {
    return NULL;
}

const iso_fs_t *iso_sd_fs(void) {
    return NULL;
}

const iso_sd_status_t *iso_sd_status(void) {
    return NULL;
}

const iso_clock_t *iso_sd_clock(void) {
    return NULL;
}

esp_err_t iso_sd_run_self_test(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_SD_ENABLE */
