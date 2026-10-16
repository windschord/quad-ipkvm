#include "tc358743.h"
#include "tc358743_edid.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include "driver/i2c_master.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

// I2C 8-bit register write
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

// Setup PLL matching Linux driver logic
static esp_err_t tc_set_pll(tc358743_handle_t dev) {
    esp_err_t err = ESP_OK;

    // TODO(hw-verify): Proper PLL prd/fbd parameters based on actual required CSI clock
    // Using a typical fallback: PRD=3, FBD=85, FRS=1
    uint16_t pllctl0_new = SET_PLL_PRD(3) | SET_PLL_FBD(85);
    uint16_t pllctl1_new = SET_PLL_FRS(1);

    uint16_t pllctl0, pllctl1;
    tc_read16(dev, PLLCTL0, &pllctl0);
    tc_read16(dev, PLLCTL1, &pllctl1);

    if (pllctl0 != pllctl0_new || pllctl1 != pllctl1_new) {
        // Disable PLL
        err |= tc_write16(dev, PLLCTL1, pllctl1_new & ~MASK_PLL_EN);
        err |= tc_write16(dev, PLLCTL0, pllctl0_new);
    }

    err |= tc_write16(dev, PLLCTL1, pllctl1_new | MASK_PLL_EN);
    vTaskDelay(pdMS_TO_TICKS(10));

    return err;
}

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
#define MASK_CLW_LANEDISABLE                  0x00000001
#define MASK_D0W_LANEDISABLE                  0x00000001
#define MASK_D1W_LANEDISABLE                  0x00000001
#define MASK_D2W_LANEDISABLE                  0x00000001
#define MASK_D3W_LANEDISABLE                  0x00000001
// Source: tc358743_regs.h MASK_STRT
#define MASK_STRT                             0x00000001

// Setup CSI matching Linux driver logic
static esp_err_t tc_set_csi(tc358743_handle_t dev) {
    esp_err_t err = ESP_OK;

    // We assume 2 lanes based on X630 hardware.
    // Disable unused lanes (D2, D3). Wait to confirm D1/D0.
    err |= tc_write32(dev, D2W_CNTRL, MASK_D2W_LANEDISABLE);
    err |= tc_write32(dev, D3W_CNTRL, MASK_D3W_LANEDISABLE);

    // TODO(hw-verify): Proper CSI lane timing (CSI_CONFW, CSI_START) configuration
    // The exact timings (e.g. THSTX, THSZERO, TCLKPREPARE) usually come from the CSI clock rate
    // Linux uses tc358743_set_csi_color_space() to configure color output first
    // For now we do a minimal start

    err |= tc_write32(dev, CSI_START, MASK_STRT);
    return err;
}

static esp_err_t tc_write8(tc358743_handle_t dev, uint16_t reg, uint8_t val) {
    uint8_t tx[3] = { reg >> 8, reg & 0xFF, val };
    return i2c_master_transmit(dev->i2c_dev, tx, 3, -1);
}

static esp_err_t tc_update16(tc358743_handle_t dev, uint16_t reg, uint16_t mask, uint16_t val) {
    uint16_t cur;
    esp_err_t err = tc_read16(dev, reg, &cur);
    if (err != ESP_OK) return err;
    return tc_write16(dev, reg, (cur & ~mask) | val);
}

static esp_err_t tc_update8(tc358743_handle_t dev, uint16_t reg, uint8_t mask, uint8_t val) {
    uint8_t cur;
    esp_err_t err = tc_read8(dev, reg, &cur);
    if (err != ESP_OK) return err;
    return tc_write8(dev, reg, (cur & ~mask) | val);
}

// --- Software Reset Sequence ---
static esp_err_t tc_reset(tc358743_handle_t dev, uint16_t mask) {
    uint16_t sysctl;
    esp_err_t err = tc_read16(dev, SYSCTL, &sysctl);
    if (err != ESP_OK) return err;

    // As per Linux driver `tc358743_reset`
    err = tc_write16(dev, SYSCTL, sysctl | mask);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    return tc_write16(dev, SYSCTL, sysctl & ~mask);
}

static esp_err_t tc_sleep_mode(tc358743_handle_t dev, bool sleep) {
    uint16_t sysctl;
    esp_err_t err = tc_read16(dev, SYSCTL, &sysctl);
    if (err != ESP_OK) return err;

    // If sleep is true, set MASK_SLEEP to 1, else clear it.
    if (sleep) sysctl |= MASK_SLEEP;
    else sysctl &= ~MASK_SLEEP;

    return tc_write16(dev, SYSCTL, sysctl);
}

// Set reference clock (from tc358743_set_ref_clk)
static esp_err_t tc_set_ref_clk(tc358743_handle_t dev) {
    uint32_t refclk_hz = dev->config.refclk_hz ? dev->config.refclk_hz : 27000000;
    if (refclk_hz != 26000000 && refclk_hz != 27000000 && refclk_hz != 42000000) {
        ESP_LOGE(TAG, "Unsupported refclk %ld", refclk_hz);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t sys_freq = refclk_hz / 10000;

    esp_err_t err = ESP_OK;
    err |= tc_write8(dev, SYS_FREQ0, sys_freq & 0x00ff);
    err |= tc_write8(dev, SYS_FREQ1, (sys_freq & 0xff00) >> 8);

    err |= tc_update8(dev, PHY_CTL0, MASK_PHY_SYSCLK_IND,
               (refclk_hz == 42000000) ? MASK_PHY_SYSCLK_IND : 0x0);

    return err;
}

// --- EDID Programming ---
static esp_err_t tc_write_edid(tc358743_handle_t dev) {
    if (!dev->config.enable_edid) return ESP_OK;

    ESP_LOGI(TAG, "Generating and writing 256-byte EDID to RAM...");

    uint8_t edid_data[256];
    size_t edid_len = tc358743_edid_build(edid_data, sizeof(edid_data), TC358743_EDID_MAX_PIXEL_CLOCK_KHZ);
    if (edid_len != 256) {
        ESP_LOGE(TAG, "Failed to build EDID");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;

    // 1. Disable EDID Mode
    err |= tc_update8(dev, EDID_MODE, MASK_EDID_MODE, MASK_EDID_MODE_DISABLE);

    // 2. Set Length (8-bit registers)
    err |= tc_write8(dev, EDID_LEN1, 0x00); // 256 lower byte
    err |= tc_write8(dev, EDID_LEN2, 0x01); // 256 upper byte

    if (err != ESP_OK) return err;

    // 3. Write 256 bytes to EDID RAM (0x8C00) in 128-byte chunks
    for (int i = 0; i < 2; i++) {
        uint8_t tx_buf[128 + 2];
        uint16_t offset = EDID_RAM + (i * 128);
        tx_buf[0] = offset >> 8;
        tx_buf[1] = offset & 0xFF;
        memcpy(tx_buf + 2, edid_data + (i * 128), 128);

        err = i2c_master_transmit(dev->i2c_dev, tx_buf, sizeof(tx_buf), -1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write EDID block %d to RAM (err: %d)", i, err);
            return err;
        }
    }

    // 4. Enable EDID Mode
    err = tc_update8(dev, EDID_MODE, MASK_EDID_MODE, MASK_EDID_MODE_E_DDC);

    ESP_LOGI(TAG, "EDID programmed successfully");
    return err;
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

    // TODO(hw-verify): X630 CSI I2C address, usually 0x0F
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

    esp_err_t err = ESP_OK;

    // 2. Initial Setup
    err |= tc_update16(dev, SYSCTL, MASK_IRRST | MASK_CECRST, MASK_IRRST | MASK_CECRST);
    err |= tc_reset(dev, MASK_CTXRST | MASK_HDMIRST);
    err |= tc_sleep_mode(dev, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial setup failed");
        tc358743_deinit(dev);
        return NULL;
    }

    err |= tc_write16(dev, FIFOCTL, 16); // fifo_level from Linux pdata defaults
    err |= tc_set_ref_clk(dev);

    // DDC delay
    err |= tc_update8(dev, DDC_CTL, MASK_DDC5V_MODE, 0); // delay 0

    // HDMI PHY, HDCP, Audio, Infoframe (Skipped detailed impl for now, minimum needed for CSI)
    err |= tc_update8(dev, VI_MODE, MASK_RGB_DVI, 0);
    err |= tc_update8(dev, VOUT_SET2, MASK_VOUTCOLORMODE, MASK_VOUTCOLORMODE_AUTO);
    err |= tc_write8(dev, VOUT_SET3, MASK_VOUT_EXTCNT);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PHY/DDC setup failed");
        tc358743_deinit(dev);
        return NULL;
    }

    // 3. HPD Control Order
    // Pull HPD down before writing EDID and configuring CSI
    ESP_LOGI(TAG, "Lowering HPD...");
    // Linux uses HPD_CTL (0x8544), MASK_HPD_CTL0 (0x10) and MASK_HPD_OUT0 (0x01)
    err |= tc_update8(dev, HPD_CTL, MASK_HPD_CTL0 | MASK_HPD_OUT0, 0);

    // Write EDID
    if (tc_write_edid(dev) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to program EDID");
        // We can continue, but without EDID the source might not output
    }

    // Configure CSI (Minimal setup to enable the bridge)
    err |= tc_set_pll(dev);
    err |= tc_reset(dev, MASK_CTXRST);
    err |= tc_set_csi(dev);

    // Bring up HPD to signal the source that we are ready
    ESP_LOGI(TAG, "Bringing up HPD...");
    err |= tc_update8(dev, HPD_CTL, MASK_HPD_CTL0 | MASK_HPD_OUT0, MASK_HPD_CTL0 | MASK_HPD_OUT0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialization completed with errors");
    }

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

    uint16_t csi_status;
    if (tc_read16(dev, CSI_STATUS, &csi_status) == ESP_OK) {
        ESP_LOGI(TAG, "CSI_STATUS: 0x%04x", csi_status);
    }

    uint16_t csi_err;
    if (tc_read16(dev, CSI_ERR, &csi_err) == ESP_OK) {
        ESP_LOGI(TAG, "CSI_ERR: 0x%04x", csi_err);
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
