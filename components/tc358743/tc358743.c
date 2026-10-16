#include "tc358743.h"
#include "tc358743_edid.h"
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "tc358743";

// Register definitions based on Linux driver tc358743_regs.h
#define CHIPID           0x0000
#define SYSCTL           0x0002
#define CONFCTL          0x0004
#define FIFOCTL          0x000A
#define INTSTATUS        0x0014
#define INTMASK          0x0016
#define PLLCTL0          0x001A
#define PLLCTL1          0x001C

#define SYS_FREQ0        0x8540
#define SYS_FREQ1        0x8542
#define PHY_CTL0         0x8544
#define DDC_CTL          0x8543
#define HPD_CONTROL      0x85D8
#define EDID_MODE        0x85E4
#define EDID_LEN1        0x85E8
#define EDID_LEN2        0x85EA
#define VI_MODE          0x8570
#define VOUT_SET2        0x8573
#define VOUT_SET3        0x8574

// CSI Registers
#define CSI_START        0x0100
#define CSI_CONFW        0x0104

// EDID RAM base address
#define EDID_RAM         0x8C00

// Masks and values
#define MASK_CHIPID      0xff00
#define MASK_REVID       0x00ff
#define MASK_CTXRST      0x0008
#define MASK_HDMIRST     0x0002
#define MASK_IRRST       0x0010
#define MASK_CECRST      0x0020
#define MASK_SLEEP       0x0001
#define MASK_HPD_OUT0    0x0001
#define MASK_HPD_OUT1    0x0002
#define MASK_EDID_MODE_E_DDC 0x01
#define MASK_EDID_MODE   0x03
#define MASK_DDC5V_MODE  0x03
#define MASK_RGB_DVI     0x08
#define MASK_VOUTCOLORMODE 0x07
#define MASK_VOUTCOLORMODE_AUTO 0x00
#define MASK_VOUT_EXTCNT 0x08
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

    if (sleep) sysctl &= ~MASK_SLEEP;
    else sysctl |= MASK_SLEEP; // 1 means not sleeping, according to Linux driver

    return tc_write16(dev, SYSCTL, sysctl);
}

// Set reference clock (from tc358743_set_ref_clk)
static esp_err_t tc_set_ref_clk(tc358743_handle_t dev) {
    uint32_t refclk_hz = dev->config.refclk_hz ? dev->config.refclk_hz : 27000000;
    uint32_t sys_freq = refclk_hz / 10000;

    tc_write8(dev, SYS_FREQ0, sys_freq & 0x00ff);
    tc_write8(dev, SYS_FREQ1, (sys_freq & 0xff00) >> 8);

    tc_update8(dev, PHY_CTL0, MASK_PHY_SYSCLK_IND,
               (refclk_hz == 42000000) ? MASK_PHY_SYSCLK_IND : 0x0);

    return ESP_OK;
}

// --- EDID Programming ---
static esp_err_t tc_write_edid(tc358743_handle_t dev) {
    if (!dev->config.enable_edid) return ESP_OK;

    ESP_LOGI(TAG, "Generating and writing 256-byte EDID to RAM...");

    uint8_t edid_data[256];
    size_t edid_len = tc358743_edid_build(edid_data, sizeof(edid_data), TC358743_EDID_MAX_PIXEL_CLOCK_MHZ);
    if (edid_len != 256) {
        ESP_LOGE(TAG, "Failed to build EDID");
        return ESP_FAIL;
    }

    // The Linux driver uses 256 for the EDID length.
    // Length registers (EDID_LEN1 and EDID_LEN2) determine how many bytes
    // the chip exposes via DDC.
    tc_write16(dev, EDID_LEN1, 256);
    tc_write16(dev, EDID_LEN2, 256);

    // Write 256 bytes to EDID RAM (0x8C00)
    // We can write it in chunks if the I2C buffer is small, but new ESP-IDF API handles large buffers well.
    // However, the TC358743 auto-increments the EDID RAM pointer, so we can write sequentially.
    // The safest way is to write in chunks or use register auto-increment.
    // We will do a single continuous write starting at 0x8C00.
    // We need an array with the register address followed by the data.
    uint8_t tx_buf[256 + 2];
    tx_buf[0] = EDID_RAM >> 8;
    tx_buf[1] = EDID_RAM & 0xFF;
    memcpy(tx_buf + 2, edid_data, 256);

    esp_err_t err = i2c_master_transmit(dev->i2c_dev, tx_buf, sizeof(tx_buf), -1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write EDID to RAM (err: %d)", err);
        return err;
    }

    ESP_LOGI(TAG, "EDID programmed successfully");
    return ESP_OK;
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
        .scl_speed_hz = 400000,
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

    // 2. Initial Setup (tc358743_initial_setup)
    tc_update16(dev, SYSCTL, MASK_IRRST | MASK_CECRST, MASK_IRRST | MASK_CECRST);
    tc_reset(dev, MASK_CTXRST | MASK_HDMIRST);
    tc_sleep_mode(dev, false);

    tc_write16(dev, FIFOCTL, 16); // fifo_level from Linux pdata defaults
    tc_set_ref_clk(dev);

    // DDC delay and EDID mode
    tc_update8(dev, DDC_CTL, MASK_DDC5V_MODE, 0); // delay 0
    tc_update8(dev, EDID_MODE, MASK_EDID_MODE, MASK_EDID_MODE_E_DDC);

    // HDMI PHY, HDCP, Audio, Infoframe (Skipped detailed impl for now, minimum needed for CSI)
    tc_update8(dev, VI_MODE, MASK_RGB_DVI, 0);
    tc_update8(dev, VOUT_SET2, MASK_VOUTCOLORMODE, MASK_VOUTCOLORMODE_AUTO);
    tc_write8(dev, VOUT_SET3, MASK_VOUT_EXTCNT);

    // 3. HPD Control Order (from HANDOFF 4 and Linux driver)
    // Pull HPD down before writing EDID and configuring CSI
    ESP_LOGI(TAG, "Lowering HPD...");
    tc_update8(dev, HPD_CONTROL, MASK_HPD_OUT0 | MASK_HPD_OUT1, 0);

    // Write EDID
    if (tc_write_edid(dev) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to program EDID");
        // We can continue, but without EDID the source might not output
    }

    // Configure CSI (Minimal setup to enable the bridge)
    // In actual implementation, this sets lane counts, CSI timings, etc.
    // corresponding to tc358743_set_csi and tc358743_set_csi_color_space
    // For now, reset CSI TX
    tc_reset(dev, MASK_CTXRST);
    // TODO: Full CSI configuration based on negotiated timings

    // Bring up HPD to signal the source that we are ready
    ESP_LOGI(TAG, "Bringing up HPD...");
    tc_update8(dev, HPD_CONTROL, MASK_HPD_OUT0 | MASK_HPD_OUT1, MASK_HPD_OUT0 | MASK_HPD_OUT1);

    return dev;
}

void tc358743_deinit(tc358743_handle_t dev) {
    if (dev) {
        if (dev->i2c_dev) i2c_master_bus_rm_device(dev->i2c_dev);
        free(dev);
    }
}

// --- Diagnostic Logging ---
#define SYS_STATUS       0x8520
#define CSI_STATUS       0x0114
#define CSI_ERR_INTSTATUS 0x0140

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
        // MASK_S_TMDS = 0x02, MASK_S_SYNC = 0x10 in Linux driver
        ESP_LOGI(TAG, "HDMI SYS_STATUS: 0x%02x (TMDS detected: %s, SYNC: %s)",
                 sys_status, (sys_status & 0x02) ? "yes" : "no", (sys_status & 0x10) ? "yes" : "no");
    }

    uint16_t csi_status;
    if (tc_read16(dev, CSI_STATUS, &csi_status) == ESP_OK) {
        ESP_LOGI(TAG, "CSI_STATUS: 0x%04x", csi_status);
    }

    uint16_t csi_err;
    if (tc_read16(dev, CSI_ERR_INTSTATUS, &csi_err) == ESP_OK) {
        ESP_LOGI(TAG, "CSI_ERR_INTSTATUS: 0x%04x", csi_err);
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
