#include "tc358743.h"
#include "tc358743_edid.h"
#include "tc358743_start.h"
#include <driver/gpio.h>

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>

static const char* TAG = "tc358743";

// Register definitions based on Linux driver tc358743_regs.h
// Source: tc358743_regs.h CHIPID
#define CHIPID           0x0000
// Source: tc358743_regs.h SYSCTL
#define SYSCTL           0x0002
// Source: tc358743_regs.h CONFCTL
#define CONFCTL          0x0004
// Source: tc358743_regs.h FIFOCTL
#define FIFOCTL          0x0006
// Source: tc358743_regs.h INTSTATUS
#define INTSTATUS        0x0014
// Source: tc358743_regs.h INTMASK
#define INTMASK          0x0016
// Source: tc358743_regs.h PLLCTL0
#define PLLCTL0          0x0020
// Source: tc358743_regs.h PLLCTL1
#define PLLCTL1          0x0022

// Source: tc358743_regs.h SYS_FREQ0
#define SYS_FREQ0        0x8540
// Source: tc358743_regs.h SYS_FREQ1
#define SYS_FREQ1        0x8541
// Source: tc358743_regs.h PHY_CTL0
#define PHY_CTL0         0x8531
// Source: tc358743_regs.h DDC_CTL
#define DDC_CTL          0x8543
// Source: tc358743_regs.h HPD_CTL
#define HPD_CTL          0x8544
// Source: tc358743_regs.h EDID_MODE
#define EDID_MODE        0x85C7
// Source: tc358743_regs.h EDID_LEN1
#define EDID_LEN1        0x85CA
// Source: tc358743_regs.h EDID_LEN2
#define EDID_LEN2        0x85CB
// Source: tc358743_regs.h VI_MODE
#define VI_MODE          0x8570
// Source: tc358743_regs.h VOUT_SET2
#define VOUT_SET2        0x8573
// Source: tc358743_regs.h VOUT_SET3
#define VOUT_SET3        0x8574

// Source: tc358743_regs.h CSI_START
#define CSI_START        0x0518
// Source: tc358743_regs.h CSI_CONFW
#define CSI_CONFW        0x0500

// Source: tc358743_regs.h EDID_RAM
#define EDID_RAM         0x8C00

// Masks and values
// Source: tc358743_regs.h MASK_CHIPID
#define MASK_CHIPID      0xff00
// Source: tc358743_regs.h MASK_REVID
#define MASK_REVID       0x00ff
// Source: tc358743_regs.h MASK_CTXRST
#define MASK_CTXRST      0x0200
// Source: tc358743_regs.h MASK_HDMIRST
#define MASK_HDMIRST     0x0100
// Source: tc358743_regs.h MASK_IRRST
#define MASK_IRRST       0x0800
// Source: tc358743_regs.h MASK_CECRST
#define MASK_CECRST      0x0400
// Source: tc358743_regs.h MASK_SLEEP
#define MASK_SLEEP       0x0001
// Source: tc358743_regs.h MASK_HPD_CTL0
#define MASK_HPD_CTL0    0x10
// Source: tc358743_regs.h MASK_HPD_OUT0
#define MASK_HPD_OUT0    0x01
// Source: tc358743_regs.h MASK_EDID_MODE_E_DDC
#define MASK_EDID_MODE_E_DDC 0x02
// Source: tc358743_regs.h MASK_EDID_MODE
#define MASK_EDID_MODE   0x03
// Source: tc358743_regs.h MASK_EDID_MODE_DISABLE
#define MASK_EDID_MODE_DISABLE 0x00
// Source: tc358743_regs.h MASK_DDC5V_MODE
#define MASK_DDC5V_MODE  0x03
// Source: tc358743_regs.h MASK_RGB_DVI
#define MASK_RGB_DVI     0x08
// Source: tc358743_regs.h MASK_VOUTCOLORMODE
#define MASK_VOUTCOLORMODE 0x03
// Source: tc358743_regs.h MASK_VOUTCOLORMODE_AUTO
#define MASK_VOUTCOLORMODE_AUTO 0x01
// Source: tc358743_regs.h MASK_VOUT_EXTCNT
#define MASK_VOUT_EXTCNT 0x08
// Source: tc358743_regs.h MASK_PHY_SYSCLK_IND
#define MASK_PHY_SYSCLK_IND 0x02

struct tc358743_dev_t {
    tc358743_config_t config;
    i2c_master_dev_handle_t i2c_dev;
    bool csi_started;
};

// I2C 16-bit register read (Little Endian, as seen in Linux `i2c_rd16`)
static esp_err_t tc_read16(tc358743_handle_t dev, uint16_t reg, uint16_t* val) {
    uint8_t tx[2] = { reg >> 8, reg & 0xFF };
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(dev->i2c_dev, tx, 2, rx, 2, -1);
    if (err == ESP_OK) {
        *val = rx[0] | (rx[1] << 8); // Little Endian
    }
    return err;
}

// I2C 8-bit register read
static esp_err_t tc_read8(tc358743_handle_t dev, uint16_t reg, uint8_t* val) {
    uint8_t tx[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(dev->i2c_dev, tx, 2, val, 1, -1);
}

// I2C 16-bit register write
static esp_err_t tc_write16(tc358743_handle_t dev, uint16_t reg, uint16_t val) {
    uint8_t tx[4] = { reg >> 8, reg & 0xFF, val & 0xFF, val >> 8 }; // Little Endian
    return i2c_master_transmit(dev->i2c_dev, tx, 4, -1);
}

// I2C 32-bit register write
static esp_err_t tc_write32(tc358743_handle_t dev, uint16_t reg, uint32_t val) {
    uint8_t tx[6] = { reg >> 8, reg & 0xFF, val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, (val >> 24) & 0xFF }; // Little Endian
    return i2c_master_transmit(dev->i2c_dev, tx, 6, -1);
}

// Source: tc358743_regs.h MASK_PLL_PRD
#define MASK_PLL_PRD                          0xf000
#define SET_PLL_PRD(prd)                      ((((prd) - 1) << 12) & MASK_PLL_PRD)
// Source: tc358743_regs.h MASK_PLL_FBD
#define MASK_PLL_FBD                          0x01ff
#define SET_PLL_FBD(fbd)                      (((fbd) - 1) & MASK_PLL_FBD)
// Source: tc358743_regs.h MASK_PLL_FRS
#define MASK_PLL_FRS                          0x0c00
#define SET_PLL_FRS(frs)                      (((frs) << 10) & MASK_PLL_FRS)
// Source: tc358743_regs.h MASK_PLL_EN
#define MASK_PLL_EN                           0x0001
// Source: tc358743_regs.h MASK_RESETB
#define MASK_RESETB                           0x0002
// Source: tc358743_regs.h MASK_CKEN
#define MASK_CKEN                             0x0010

// Source: tc358743_regs.h CLW_CNTRL
#define CLW_CNTRL                             0x0140
// Source: tc358743_regs.h D0W_CNTRL
#define D0W_CNTRL                             0x0144
// Source: tc358743_regs.h D1W_CNTRL
#define D1W_CNTRL                             0x0148
// Source: tc358743_regs.h D2W_CNTRL
#define D2W_CNTRL                             0x014C
// Source: tc358743_regs.h D3W_CNTRL
#define D3W_CNTRL                             0x0150
// Source: tc358743_regs.h MASK_CLW_LANEDISABLE
#define MASK_CLW_LANEDISABLE                  0x0001
#define MASK_D0W_LANEDISABLE                  0x0001
#define MASK_D1W_LANEDISABLE                  0x0001
#define MASK_D2W_LANEDISABLE                  0x0001
#define MASK_D3W_LANEDISABLE                  0x0001

#define STARTCNTRL                            0x0204
#define LINEINITCNT                           0x0210
#define LPTXTIMECNT                           0x0214
#define TCLK_HEADERCNT                        0x0218
#define TCLK_TRAILCNT                         0x021C
#define THS_HEADERCNT                         0x0220
#define TWAKEUP                               0x0224
#define TCLK_POSTCNT                          0x0228
#define THS_TRAILCNT                          0x022C
#define HSTXVREGCNT                           0x0230
#define HSTXVREGEN                            0x0234
#define TXOPTIONCNTRL                         0x0238

// Source: tc358743_regs.h MASK_STRT
#define MASK_STRT                             0x00000001
#define MASK_CONTCLKMODE                      0x00000001
#define MASK_D3M_HSTXVREGEN                   0x0010
#define MASK_D2M_HSTXVREGEN                   0x0008
#define MASK_D1M_HSTXVREGEN                   0x0004
#define MASK_D0M_HSTXVREGEN                   0x0002
#define MASK_CLM_HSTXVREGEN                   0x0001

static esp_err_t tc_write8(tc358743_handle_t dev, uint16_t reg, uint8_t val) {
    uint8_t tx[3] = { reg >> 8, reg & 0xFF, val };
    return i2c_master_transmit(dev->i2c_dev, tx, 3, -1);
}

static bool esp_io_write8(void* ctx, uint16_t reg, uint8_t val) {
    return tc_write8((tc358743_handle_t)ctx, reg, val) == ESP_OK;
}
static bool esp_io_write16(void* ctx, uint16_t reg, uint16_t val) {
    return tc_write16((tc358743_handle_t)ctx, reg, val) == ESP_OK;
}
static bool esp_io_write32(void* ctx, uint16_t reg, uint32_t val) {
    return tc_write32((tc358743_handle_t)ctx, reg, val) == ESP_OK;
}
static bool esp_io_read8(void* ctx, uint16_t reg, uint8_t* val) {
    return tc_read8((tc358743_handle_t)ctx, reg, val) == ESP_OK;
}
static bool esp_io_read16(void* ctx, uint16_t reg, uint16_t* val) {
    return tc_read16((tc358743_handle_t)ctx, reg, val) == ESP_OK;
}
static void esp_io_delay_ms(void* ctx, uint32_t ms) {
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}
static void esp_io_reset_gpio(void* ctx, bool level) {
    (void)ctx;
    (void)level;
#ifdef CONFIG_TC358743_RESET_GPIO
    if (CONFIG_TC358743_RESET_GPIO >= 0) {
        gpio_set_level(CONFIG_TC358743_RESET_GPIO, level ? 1 : 0);
    }
#endif
}

esp_err_t tc358743_start_csi(tc358743_handle_t dev, const tc358743_mode_t *mode, bool *out_locked) {
    if (!dev || !mode || !out_locked) return ESP_ERR_INVALID_ARG;

#ifdef CONFIG_TC358743_RESET_GPIO
    if (CONFIG_TC358743_RESET_GPIO >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << CONFIG_TC358743_RESET_GPIO),
            .mode = GPIO_MODE_OUTPUT_OD,
            .pull_up_en = 0,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(CONFIG_TC358743_RESET_GPIO, 1);
    }
#endif

    tc358743_io_ops_t ops = {
        .ctx = dev,
        .write8 = esp_io_write8,
        .write16 = esp_io_write16,
        .write32 = esp_io_write32,
        .read8 = esp_io_read8,
        .read16 = esp_io_read16,
        .delay_ms = esp_io_delay_ms,
        .reset_gpio = esp_io_reset_gpio
    };

    bool res = tc358743_start_sequence(&ops, mode, TC358743_EDID_PROFILE_720P_60, out_locked);
    if (res) {
        dev->csi_started = true;
        return ESP_OK;
    }
    return ESP_FAIL;
}
// --- Initialization ---
tc358743_handle_t tc358743_init(const tc358743_config_t* config) {
    ESP_LOGI(TAG, "Initializing TC358743...");

    if (!config->i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not provided");
        return NULL;
    }

    tc358743_handle_t dev = calloc(1, sizeof(struct tc358743_dev_t));
    if (!dev) return NULL;
    dev->config = *config;

    // X630 TC358743 responded at 0x0F on hardware (source: docs/HW_VERIFY.md E3)
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->i2c_addr ? config->i2c_addr : 0x0F,
        .scl_speed_hz = 100000, // 400kHz TODO(hw-verify)
    };

    if (i2c_master_bus_add_device(config->i2c_bus, &dev_cfg, &dev->i2c_dev) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device");
        free(dev);
        return NULL;
    }

    // 1. Check Chip ID
    // As per Linux driver (tc358743_probe), the expected CHIPID value (mask 0xff00) is 0x0000.
    uint16_t chipid = 0;
    if (tc_read16(dev, CHIPID, &chipid) != ESP_OK || (chipid & MASK_CHIPID) != 0x0000) {
        ESP_LOGE(TAG, "Chip ID validation failed. Read 0x%04x, expected 0x00xx", chipid);
        tc358743_deinit(dev);
        return NULL;
    }
    ESP_LOGI(TAG, "Found TC358743, Chip ID: 0x%04x (Revision 0x%02x)", chipid, chipid & MASK_REVID);

    // The reset, initial setup (HDMI RX PHY, DDC, EDID), PLL, CSI, output format and HPD are done by
    // tc358743_start_csi() (tc358743_start.c). tc358743_init() only probes the chip.
    dev->csi_started = false;
    return dev;
}

void tc358743_deinit(tc358743_handle_t dev) {
    if (dev) {
        if (dev->i2c_dev) i2c_master_bus_rm_device(dev->i2c_dev);
        free(dev);
    }
}

// --- Diagnostic Logging ---
// Source: tc358743_regs.h SYS_STATUS
#define SYS_STATUS       0x8520
// Source: tc358743_regs.h CSI_STATUS
#define CSI_STATUS       0x0410
// Source: tc358743_regs.h CSI_ERR
#define CSI_ERR          0x044C

void tc358743_dump_diagnostics(tc358743_handle_t dev) {
    if (!dev) return;

    ESP_LOGI(TAG, "----- TC358743 Diagnostics -----");

    uint16_t chipid;
    if (tc_read16(dev, CHIPID, &chipid) == ESP_OK) {
        ESP_LOGI(TAG, "Chip ID: 0x%04x (Revision 0x%02x)", chipid & MASK_CHIPID, chipid & MASK_REVID);
    }

    uint16_t sysctl;
    if (tc_read16(dev, SYSCTL, &sysctl) == ESP_OK) {
        ESP_LOGI(TAG, "SYSCTL: 0x%04x (Sleep: %d, HDMIRST: %d, CTXRST: %d)",
                 sysctl, !!(sysctl & MASK_SLEEP), !!(sysctl & MASK_HDMIRST), !!(sysctl & MASK_CTXRST));
    }

    uint8_t sys_status;
    if (tc_read8(dev, SYS_STATUS, &sys_status) == ESP_OK) {
        // MASK_S_TMDS = 0x02, MASK_S_SYNC = 0x80 in Linux driver
        ESP_LOGI(TAG, "HDMI SYS_STATUS: 0x%02x (TMDS detected: %s, SYNC: %s)",
                 sys_status, (sys_status & 0x02) ? "yes" : "no", (sys_status & 0x80) ? "yes" : "no");
    }

    if (!dev->csi_started) {
        ESP_LOGI(TAG, "CSI_STATUS/CSI_ERR: not read (CSI not started; values before CSI start are invalid)");
    } else {
        uint16_t csi_status;
        if (tc_read16(dev, CSI_STATUS, &csi_status) == ESP_OK) {
            ESP_LOGI(TAG, "CSI_STATUS: 0x%04x", csi_status);
        }

        uint16_t csi_err;
        if (tc_read16(dev, CSI_ERR, &csi_err) == ESP_OK) {
            ESP_LOGI(TAG, "CSI_ERR: 0x%04x", csi_err);
        }
    }

    ESP_LOGI(TAG, "--------------------------------");
}

uint16_t tc358743_poll_interrupts(tc358743_handle_t dev) {
    if (!dev) return 0;

    uint16_t intstatus;
    if (tc_read16(dev, INTSTATUS, &intstatus) == ESP_OK) {
        if (intstatus != 0) {
            ESP_LOGD(TAG, "Polled INTSTATUS: 0x%04x", intstatus);
            // In a real application, you'd clear the handled interrupts here by writing to INTSTATUS
            // or sub-interrupt registers (e.g., HDMI_INT0).
        }
        return intstatus;
    }
    return 0;
}
