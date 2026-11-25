#include "iso_usb_msc.h"
#include "sdkconfig.h"

#if defined(CONFIG_USB_MSC_ENABLE) && CONFIG_USB_MSC_ENABLE

#if defined(CONFIG_TINYUSB_MSC_ENABLED) && CONFIG_TINYUSB_MSC_ENABLED
#error "CONFIG_USB_MSC_ENABLE requires CONFIG_TINYUSB_MSC_ENABLED=n"
#endif

#include <string.h>

#include "device/usbd_pvt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iso_usb_msc_glue.h"
#include "tusb.h"

#define ISO_MSC_CLASS        0x08u
#define ISO_MSC_SUBCLASS     0x06u
#define ISO_MSC_PROTOCOL_BOT 0x50u
#define ISO_MSC_REQ_RESET    0xFFu
#define ISO_MSC_REQ_MAX_LUN  0xFEu

#define ISO_MSC_DMA_ALIGN    64u
#define ISO_MSC_CTRL_BUF_SZ  64u
#define ISO_MSC_BUF_SIZE     ((uint32_t)CONFIG_USB_MSC_BUF_BLOCKS * MMC_SCSI_BLOCK_SIZE)

static const char *TAG = "iso_usb_msc";

static bool s_initialized = false;
static SemaphoreHandle_t s_state_mutex = NULL;
static SemaphoreHandle_t s_backend_mutex = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static QueueHandle_t s_res_queue = NULL;
static TaskHandle_t s_worker_task = NULL;

static iso_media_t s_owned_media;
static iso_media_t *s_media_ptr = NULL;
static void *s_ext_state_ctx = NULL;
static void (*s_ext_state_lock)(void *ctx) = NULL;
static void (*s_ext_state_unlock)(void *ctx) = NULL;
static bool s_using_ext_resolver = false;
static iso_usb_msc_extent_backend_t s_backend;
static mmc_scsi_t s_scsi;
static msc_bot_t s_bot;

static uint8_t *s_cbw_buf = NULL;
static uint8_t *s_csw_buf = NULL;
static uint8_t *s_data_buf[2] = {NULL, NULL};

/* TinyUSB task-only endpoint / BOT control state. */
static uint8_t s_rhport = 0u;
static uint8_t s_itf_num = 0u;
static uint8_t s_ep_out = 0u;
static uint8_t s_ep_in = 0u;
static bool s_in_armed = false;
static bool s_out_armed = false;
static bool s_reset_pending = false;
static bool s_reset_stalled = false;
static tusb_control_request_t s_reset_req;
static uint32_t s_cmd_inflight = 0u;
static bool s_usb_uninstalling = false;

static void backend_lock_cb(void *ctx) {
    (void)ctx;
    if (s_backend_mutex != NULL) {
        xSemaphoreTake(s_backend_mutex, portMAX_DELAY);
    }
}

static void backend_unlock_cb(void *ctx) {
    (void)ctx;
    if (s_backend_mutex != NULL) {
        xSemaphoreGive(s_backend_mutex);
    }
}

static void msc_state_lock(void) {
    if (s_ext_state_lock != NULL) {
        s_ext_state_lock(s_ext_state_ctx);
    } else if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }
}

static void msc_state_unlock(void) {
    if (s_ext_state_unlock != NULL) {
        s_ext_state_unlock(s_ext_state_ctx);
    } else if (s_state_mutex != NULL) {
        xSemaphoreGive(s_state_mutex);
    }
}

static void standalone_on_force_eject_cb(void *ctx) {
    (void)ctx;
    backend_lock_cb(NULL);
    const iso_blockdev_t *bdev = s_backend.bdev;
    backend_unlock_cb(NULL);
    if (bdev != NULL && bdev->epoch_bump != NULL) {
        bdev->epoch_bump(bdev->ctx);
    }
}

static void on_msc_result_deferred(void *param) {
    (void)param;
    if (__atomic_load_n(&s_usb_uninstalling, __ATOMIC_SEQ_CST)) {
        return;
    }
    msc_bot_cmd_result_t res;
    while (xQueueReceive(s_res_queue, &res, 0) == pdTRUE) {
        msc_bot_command_complete(&s_bot, &res);
    }
}

static void iso_msc_task(void *arg) {
    (void)arg;
    msc_bot_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        msc_bot_cmd_result_t res;
        if (cmd.kind == MSC_BOT_CMD_EXECUTE) {
            msc_state_lock();
            iso_usb_msc_exec_cmd(&s_scsi, &cmd, &res);
            if (!s_using_ext_resolver) {
                iso_usb_msc_sync_media_backend(s_media_ptr, &s_backend);
            }
            msc_state_unlock();
        } else {
            /* READ_BLOCKS performs blocking SD I/O without iso_state_mutex (mmc_scsi.h). */
            iso_usb_msc_exec_cmd(&s_scsi, &cmd, &res);
        }

        if (xQueueSend(s_res_queue, &res, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to enqueue MSC command result");
        } else if (!__atomic_load_n(&s_usb_uninstalling, __ATOMIC_SEQ_CST)) {
            usbd_defer_func(on_msc_result_deferred, NULL, false);
        }
        __atomic_fetch_sub(&s_cmd_inflight, 1u, __ATOMIC_SEQ_CST);
    }
}

/* ---- msc_bot hardware/OS callbacks (executed on TinyUSB device task) ---- */

static int bot_xfer_in(void *ctx, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    if (s_ep_in == 0u || buf == NULL || len > UINT16_MAX || usbd_edpt_busy(s_rhport, s_ep_in)) {
        return -1;
    }
    /* TinyUSB takes a non-const buffer pointer even for IN transfers. */
    if (!usbd_edpt_xfer(s_rhport, s_ep_in, (uint8_t *)(uintptr_t)buf, (uint16_t)len, false)) {
        return -1;
    }
    s_in_armed = true;
    return 0;
}

static int bot_xfer_out(void *ctx, uint8_t *buf, uint32_t len) {
    (void)ctx;
    if (s_ep_out == 0u || buf == NULL || len > UINT16_MAX || usbd_edpt_busy(s_rhport, s_ep_out)) {
        return -1;
    }
    if (!usbd_edpt_xfer(s_rhport, s_ep_out, buf, (uint16_t)len, false)) {
        return -1;
    }
    s_out_armed = true;
    return 0;
}

static void bot_stall_in(void *ctx) {
    (void)ctx;
    s_in_armed = false;
    if (s_ep_in != 0u) {
        usbd_edpt_stall(s_rhport, s_ep_in);
    }
}

static void bot_stall_out(void *ctx) {
    (void)ctx;
    s_out_armed = false;
    if (s_ep_out != 0u) {
        usbd_edpt_stall(s_rhport, s_ep_out);
    }
}

static void bot_unstall(void *ctx, msc_bot_ep_t ep) {
    (void)ctx;
    const uint8_t ep_addr = (ep == MSC_BOT_EP_IN) ? s_ep_in : s_ep_out;
    if (ep == MSC_BOT_EP_IN) {
        s_in_armed = false;
    } else {
        s_out_armed = false;
    }
    if (ep_addr != 0u && usbd_edpt_stalled(s_rhport, ep_addr)) {
        usbd_edpt_clear_stall(s_rhport, ep_addr);
    }
}

static void cancel_non_stalled_ep(uint8_t ep_addr) {
    if (ep_addr == 0u) {
        return;
    }
    /* BOT 3.1 keeps halted endpoints STALLed until host CLEAR_FEATURE(ENDPOINT_HALT). */
    if (!usbd_edpt_stalled(s_rhport, ep_addr) && usbd_edpt_busy(s_rhport, ep_addr)) {
        /* DWC2 has no abort-without-stall API; stall + clear disables EP & flushes FIFO cleanly. */
        usbd_edpt_stall(s_rhport, ep_addr);
        usbd_edpt_clear_stall(s_rhport, ep_addr);
    }
}

static void bot_cancel_xfers(void *ctx) {
    (void)ctx;
    s_in_armed = false;
    s_out_armed = false;
    cancel_non_stalled_ep(s_ep_in);
    cancel_non_stalled_ep(s_ep_out);
}

static int bot_submit_command(void *ctx, const msc_bot_cmd_t *cmd) {
    (void)ctx;
    if (cmd == NULL || s_cmd_queue == NULL || __atomic_load_n(&s_usb_uninstalling, __ATOMIC_SEQ_CST)) {
        return -1;
    }
    __atomic_fetch_add(&s_cmd_inflight, 1u, __ATOMIC_SEQ_CST);
    if (xQueueSend(s_cmd_queue, cmd, 0) != pdTRUE) {
        __atomic_fetch_sub(&s_cmd_inflight, 1u, __ATOMIC_SEQ_CST);
        return -1;
    }
    return 0;
}

static void bot_command_done(void *ctx) {
    (void)ctx;
    msc_state_lock();
    mmc_scsi_command_done(&s_scsi);
    if (!s_using_ext_resolver) {
        iso_usb_msc_sync_media_backend(s_media_ptr, &s_backend);
    }
    msc_state_unlock();
}

static void bot_reset_done(void *ctx) {
    (void)ctx;
    if (s_reset_pending) {
        s_reset_pending = false;
        tud_control_status(s_rhport, &s_reset_req);
    }
}

static void bot_reset_failed(void *ctx) {
    (void)ctx;
    if (s_reset_pending) {
        s_reset_pending = false;
        s_reset_stalled = true;
        usbd_edpt_stall(s_rhport, 0x00u);
        usbd_edpt_stall(s_rhport, 0x80u);
    }
}

/* ---- TinyUSB custom class driver ---- */

static void msc_drv_init(void) {
}

static bool msc_drv_deinit(void) {
    s_in_armed = false;
    s_out_armed = false;
    s_reset_pending = false;
    s_reset_stalled = false;
    s_ep_in = 0u;
    s_ep_out = 0u;
    s_itf_num = 0u;

    if (s_initialized) {
        __atomic_store_n(&s_usb_uninstalling, true, __ATOMIC_SEQ_CST);
        msc_state_lock();
        iso_media_bus_reset(s_media_ptr);
        msc_state_unlock();
        msc_bot_detach(&s_bot);

        /* Wait for any in-flight worker command so usbd_defer_func() never hits a torndown queue. */
        msc_bot_cmd_result_t res;
        const TickType_t step_ticks = (pdMS_TO_TICKS(10u) > 0u) ? pdMS_TO_TICKS(10u) : 1u;
        TickType_t elapsed = 0u;
        while (elapsed < pdMS_TO_TICKS(2000u)) {
            while (s_res_queue != NULL && xQueueReceive(s_res_queue, &res, 0) == pdTRUE) {
                msc_bot_command_complete(&s_bot, &res);
            }
            if (__atomic_load_n(&s_cmd_inflight, __ATOMIC_SEQ_CST) == 0u) {
                break;
            }
            vTaskDelay(step_ticks);
            elapsed += step_ticks;
        }
        while (s_res_queue != NULL && xQueueReceive(s_res_queue, &res, 0) == pdTRUE) {
            msc_bot_command_complete(&s_bot, &res);
        }
        if (__atomic_load_n(&s_cmd_inflight, __ATOMIC_SEQ_CST) != 0u) {
            ESP_LOGW(TAG, "MSC deinit drain timeout (%u in-flight cmd)",
                     (unsigned)__atomic_load_n(&s_cmd_inflight, __ATOMIC_SEQ_CST));
        }
    }
    return true;
}

static void msc_drv_reset(uint8_t rhport) {
    (void)rhport;
    s_in_armed = false;
    s_out_armed = false;
    s_reset_pending = false;
    s_reset_stalled = false;
    s_ep_in = 0u;
    s_ep_out = 0u;
    s_itf_num = 0u;

    if (!s_initialized) {
        return;
    }
    msc_state_lock();
    iso_media_bus_reset(s_media_ptr);
    msc_state_unlock();
    msc_bot_detach(&s_bot);
}

static uint16_t msc_drv_open(uint8_t rhport, tusb_desc_interface_t const *desc_intf, uint16_t max_len) {
    const uint16_t drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) + 2u * sizeof(tusb_desc_endpoint_t));
    if (!s_initialized || desc_intf == NULL || max_len < drv_len) {
        return 0u;
    }
    if (desc_intf->bInterfaceClass != ISO_MSC_CLASS ||
        desc_intf->bInterfaceSubClass != ISO_MSC_SUBCLASS ||
        desc_intf->bInterfaceProtocol != ISO_MSC_PROTOCOL_BOT ||
        desc_intf->bNumEndpoints != 2u) {
        return 0u;
    }

    uint8_t ep_out = 0u;
    uint8_t ep_in = 0u;
    if (!usbd_open_edpt_pair(rhport, tu_desc_next(desc_intf), 2u, TUSB_XFER_BULK, &ep_out, &ep_in)) {
        return 0u;
    }

    s_rhport = rhport;
    s_itf_num = desc_intf->bInterfaceNumber;
    s_ep_out = ep_out;
    s_ep_in = ep_in;
    s_in_armed = false;
    s_out_armed = false;
    s_reset_pending = false;
    s_reset_stalled = false;
    __atomic_store_n(&s_usb_uninstalling, false, __ATOMIC_SEQ_CST);

    msc_bot_attach(&s_bot);
    return drv_len;
}

static bool msc_drv_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (!s_initialized || request == NULL) {
        return false;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_ENDPOINT) {
        const uint8_t ep_addr = tu_u16_low(request->wIndex);
        if (ep_addr != s_ep_in && ep_addr != s_ep_out) {
            return false;
        }
        if (stage == CONTROL_STAGE_SETUP) {
            s_reset_pending = false;
        }
        if (request->bRequest == TUSB_REQ_CLEAR_FEATURE &&
            request->wValue == TUSB_REQ_FEATURE_EDPT_HALT) {
            if (stage == CONTROL_STAGE_SETUP) {
                if (ep_addr == s_ep_in) {
                    s_in_armed = false;
                    msc_bot_halt_cleared(&s_bot, MSC_BOT_EP_IN);
                } else {
                    s_out_armed = false;
                    msc_bot_halt_cleared(&s_bot, MSC_BOT_EP_OUT);
                }
            }
            return true;
        }
        if (request->bRequest == TUSB_REQ_SET_FEATURE &&
            request->wValue == TUSB_REQ_FEATURE_EDPT_HALT) {
            return true;
        }
        return false;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
        if (tu_u16_low(request->wIndex) != s_itf_num) {
            return false;
        }
        if (stage == CONTROL_STAGE_SETUP) {
            s_reset_pending = false;
        }
        if (request->bRequest == ISO_MSC_REQ_MAX_LUN) {
            if (request->bmRequestType_bit.direction != TUSB_DIR_IN ||
                request->wValue != 0u || request->wLength != 1u) {
                return false;
            }
            if (stage == CONTROL_STAGE_SETUP) {
                static uint8_t s_max_lun = 0u;
                return tud_control_xfer(rhport, request, &s_max_lun, 1u);
            }
            return true;
        }
        if (request->bRequest == ISO_MSC_REQ_RESET) {
            if (request->bmRequestType_bit.direction != TUSB_DIR_OUT ||
                request->wValue != 0u || request->wLength != 0u) {
                return false;
            }
            if (stage == CONTROL_STAGE_SETUP) {
                s_reset_req = *request;
                s_reset_pending = true;
                s_reset_stalled = false;
                /* BOT 3.1: omit tud_control_status() when deferred -> DWC2 NAKs EP0 until reset_done. */
                msc_bot_reset(&s_bot);
                return !s_reset_stalled;
            }
            return true;
        }
    }

    return false;
}

static bool msc_drv_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)rhport;
    if (!s_initialized) {
        return true;
    }
    const bool ok = (result == XFER_RESULT_SUCCESS);
    if (ep_addr == s_ep_in) {
        if (!s_in_armed) {
            return true;
        }
        s_in_armed = false;
        msc_bot_xfer_complete(&s_bot, MSC_BOT_EP_IN, xferred_bytes, ok);
        return true;
    }
    if (ep_addr == s_ep_out) {
        if (!s_out_armed) {
            return true;
        }
        s_out_armed = false;
        msc_bot_xfer_complete(&s_bot, MSC_BOT_EP_OUT, xferred_bytes, ok);
        return true;
    }
    return false;
}

static usbd_class_driver_t const s_iso_msc_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "ISO_MSC",
#endif
    .init = msc_drv_init,
    .deinit = msc_drv_deinit,
    .reset = msc_drv_reset,
    .open = msc_drv_open,
    .control_xfer_cb = msc_drv_control_xfer_cb,
    .xfer_cb = msc_drv_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    if (driver_count != NULL) {
        *driver_count = 1u;
    }
    return &s_iso_msc_driver;
}

static void cleanup_init_resources(void) {
    if (s_worker_task != NULL) {
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
    }
    if (s_cmd_queue != NULL) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    if (s_res_queue != NULL) {
        vQueueDelete(s_res_queue);
        s_res_queue = NULL;
    }
    if (s_state_mutex != NULL) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
    if (s_backend_mutex != NULL) {
        vSemaphoreDelete(s_backend_mutex);
        s_backend_mutex = NULL;
    }
    heap_caps_free(s_cbw_buf);
    heap_caps_free(s_csw_buf);
    heap_caps_free(s_data_buf[0]);
    heap_caps_free(s_data_buf[1]);
    s_cbw_buf = NULL;
    s_csw_buf = NULL;
    s_data_buf[0] = NULL;
    s_data_buf[1] = NULL;
    s_media_ptr = NULL;
    s_ext_state_ctx = NULL;
    s_ext_state_lock = NULL;
    s_ext_state_unlock = NULL;
    s_using_ext_resolver = false;
}

esp_err_t iso_usb_msc_init(const iso_blockdev_t *bdev) {
    const iso_usb_msc_config_t cfg = {
        .bdev = bdev,
    };
    return iso_usb_msc_init_ex(&cfg);
}

esp_err_t iso_usb_msc_init_ex(const iso_usb_msc_config_t *cfg) {
    if (s_initialized) {
        if (cfg != NULL) {
            iso_usb_msc_set_blockdev(cfg->bdev);
            if (cfg->resolver.lookup_size_locked != NULL &&
                cfg->resolver.acquire_table_locked != NULL) {
                iso_usb_msc_extent_backend_set_resolver(&s_backend, &cfg->resolver);
                msc_state_lock();
                s_using_ext_resolver = true;
                msc_state_unlock();
            }
        }
        return ESP_OK;
    }

    if (cfg != NULL) {
        if ((cfg->state_lock != NULL) != (cfg->state_unlock != NULL)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->shared_media != NULL && cfg->state_lock == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if ((cfg->resolver.lookup_size_locked != NULL) !=
            (cfg->resolver.acquire_table_locked != NULL)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    s_ext_state_ctx = (cfg != NULL) ? cfg->state_lock_ctx : NULL;
    s_ext_state_lock = (cfg != NULL) ? cfg->state_lock : NULL;
    s_ext_state_unlock = (cfg != NULL) ? cfg->state_unlock : NULL;
    s_using_ext_resolver =
        (cfg != NULL && cfg->resolver.lookup_size_locked != NULL &&
         cfg->resolver.acquire_table_locked != NULL);

    if (s_ext_state_lock == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            cleanup_init_resources();
            return ESP_ERR_NO_MEM;
        }
    }
    s_backend_mutex = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(1, sizeof(msc_bot_cmd_t));
    s_res_queue = xQueueCreate(4, sizeof(msc_bot_cmd_result_t));
    s_cbw_buf = heap_caps_aligned_alloc(ISO_MSC_DMA_ALIGN, ISO_MSC_CTRL_BUF_SZ,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_csw_buf = heap_caps_aligned_alloc(ISO_MSC_DMA_ALIGN, ISO_MSC_CTRL_BUF_SZ,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_data_buf[0] = heap_caps_aligned_alloc(ISO_MSC_DMA_ALIGN, ISO_MSC_BUF_SIZE,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_data_buf[1] = heap_caps_aligned_alloc(ISO_MSC_DMA_ALIGN, ISO_MSC_BUF_SIZE,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (s_backend_mutex == NULL || s_cmd_queue == NULL || s_res_queue == NULL ||
        s_cbw_buf == NULL || s_csw_buf == NULL ||
        s_data_buf[0] == NULL || s_data_buf[1] == NULL) {
        cleanup_init_resources();
        return ESP_ERR_NO_MEM;
    }

    if (cfg != NULL && cfg->shared_media != NULL) {
        s_media_ptr = cfg->shared_media;
    } else {
        s_media_ptr = &s_owned_media;
        iso_media_init(s_media_ptr, standalone_on_force_eject_cb, NULL);
    }
    iso_usb_msc_extent_backend_init(&s_backend, (cfg != NULL) ? cfg->bdev : NULL,
                                    backend_lock_cb, backend_unlock_cb, NULL);
    if (s_using_ext_resolver) {
        iso_usb_msc_extent_backend_set_resolver(&s_backend, &cfg->resolver);
    }
    const mmc_scsi_backend_t scsi_be = iso_usb_msc_extent_backend_as_scsi(&s_backend);
    if (mmc_scsi_init(&s_scsi, s_media_ptr, &scsi_be) != 0) {
        cleanup_init_resources();
        return ESP_ERR_INVALID_STATE;
    }

    const msc_bot_cfg_t bot_cfg = {
        .ops = {
            .ctx = NULL,
            .xfer_in = bot_xfer_in,
            .xfer_out = bot_xfer_out,
            .stall_in = bot_stall_in,
            .stall_out = bot_stall_out,
            .unstall = bot_unstall,
            .cancel_xfers = bot_cancel_xfers,
            .submit_command = bot_submit_command,
            .command_done = bot_command_done,
            .reset_done = bot_reset_done,
            .reset_failed = bot_reset_failed,
        },
        .cbw_buf = s_cbw_buf,
        .csw_buf = s_csw_buf,
        .buf = {s_data_buf[0], s_data_buf[1]},
        .buf_size = ISO_MSC_BUF_SIZE,
        .out_stall = true,
    };
    if (msc_bot_init(&s_bot, &bot_cfg) != MSC_BOT_OK) {
        cleanup_init_resources();
        return ESP_ERR_INVALID_ARG;
    }

    __atomic_store_n(&s_cmd_inflight, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_usb_uninstalling, false, __ATOMIC_SEQ_CST);

    if (xTaskCreate(iso_msc_task, "iso_msc", CONFIG_USB_MSC_TASK_STACK_SIZE, NULL,
                    CONFIG_USB_MSC_TASK_PRIO, &s_worker_task) != pdPASS) {
        s_worker_task = NULL;
        cleanup_init_resources();
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "USB MSC driver ready (buf=%luBx2, blocks=%d)",
             (unsigned long)ISO_MSC_BUF_SIZE, CONFIG_USB_MSC_BUF_BLOCKS);
    return ESP_OK;
}

void iso_usb_msc_set_blockdev(const iso_blockdev_t *bdev) {
    if (!s_initialized) {
        return;
    }
    iso_usb_msc_extent_backend_set_blockdev(&s_backend, bdev);
}

esp_err_t iso_usb_msc_mount_image(uint32_t media_id, iso_extent_table_t *table, bool force) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (media_id == 0u || table == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    msc_state_lock();
    backend_lock_cb(NULL);
    const iso_blockdev_t *bdev = s_backend.bdev;
    backend_unlock_cb(NULL);
    /* Fail closed (ISO_MOUNT_DESIGN §5.5): reject mount if blockdev or epoch source is missing/stale. */
    if (!iso_usb_msc_table_epoch_matches(bdev, table)) {
        msc_state_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const iso_media_result_t mr = iso_media_mount(s_media_ptr, media_id, force);
    if (mr == ISO_MEDIA_MOUNTED && !s_using_ext_resolver) {
        iso_usb_msc_extent_backend_set_image(&s_backend, media_id, table);
    }
    msc_state_unlock();

    switch (mr) {
    case ISO_MEDIA_MOUNTED:
    case ISO_MEDIA_ALREADY_MOUNTED:
        return ESP_OK;
    case ISO_MEDIA_LOCKED:
        return ESP_ERR_INVALID_STATE;
    case ISO_MEDIA_INVALID:
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t iso_usb_msc_eject_image(bool force) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    msc_state_lock();
    const iso_media_result_t mr = iso_media_eject(s_media_ptr, force);
    if (!s_using_ext_resolver && (mr == ISO_MEDIA_EJECTED || mr == ISO_MEDIA_NOT_MOUNTED)) {
        iso_usb_msc_extent_backend_clear(&s_backend);
    }
    msc_state_unlock();

    if (mr == ISO_MEDIA_LOCKED) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void iso_usb_msc_force_eject(void) {
    if (!s_initialized) {
        return;
    }
    msc_state_lock();
    iso_media_force_eject(s_media_ptr);
    if (!s_using_ext_resolver) {
        iso_usb_msc_extent_backend_clear(&s_backend);
    }
    msc_state_unlock();
}

void iso_usb_msc_notify_disconnect(void) {
    if (!s_initialized || s_media_ptr == NULL) {
        return;
    }
    msc_state_lock();
    iso_usb_msc_on_disconnect_media(s_media_ptr);
    msc_state_unlock();
}

esp_err_t iso_usb_msc_get_status(iso_media_status_t *out) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    msc_state_lock();
    if (!s_using_ext_resolver) {
        iso_usb_msc_sync_media_backend(s_media_ptr, &s_backend);
    }
    iso_media_get_status(s_media_ptr, out);
    msc_state_unlock();
    return ESP_OK;
}

bool iso_usb_msc_is_initialized(void) {
    return s_initialized;
}

#else /* !CONFIG_USB_MSC_ENABLE */

esp_err_t iso_usb_msc_init(const iso_blockdev_t *bdev) {
    (void)bdev;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t iso_usb_msc_init_ex(const iso_usb_msc_config_t *cfg) {
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void iso_usb_msc_set_blockdev(const iso_blockdev_t *bdev) {
    (void)bdev;
}

void iso_usb_msc_notify_disconnect(void) {
}

esp_err_t iso_usb_msc_mount_image(uint32_t media_id, iso_extent_table_t *table, bool force) {
    (void)media_id;
    (void)table;
    (void)force;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t iso_usb_msc_eject_image(bool force) {
    (void)force;
    return ESP_ERR_NOT_SUPPORTED;
}

void iso_usb_msc_force_eject(void) {
}

esp_err_t iso_usb_msc_get_status(iso_media_status_t *out) {
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

bool iso_usb_msc_is_initialized(void) {
    return false;
}

#endif /* CONFIG_USB_MSC_ENABLE */
