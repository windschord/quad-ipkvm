#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <driver/i2c_master.h>
#include "esp_err.h"
#include "tc358743_start.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for the device
typedef struct tc358743_dev_t* tc358743_handle_t;

// EDID max modes logic
#define TC358743_EDID_MAX_PIXEL_CLOCK_KHZ 81000  // TODO(hw-verify): X630 2-lane limit ~81MHz (1080p30 max)

// Initialization config
// Timing settings for CSI, usually specific to the endpoint format/clock
typedef struct {
    uint32_t lineinitcnt;
    uint32_t lptxtimecnt;
    uint32_t tclk_headercnt;
    uint32_t tclk_trailcnt;
    uint32_t ths_headercnt;
    uint32_t twakeup;
    uint32_t tclk_postcnt;
    uint32_t ths_trailcnt;
    uint32_t hstxvregcnt;
} tc358743_csi_timings_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t i2c_addr; // default: 0x0F
    uint32_t refclk_hz; // e.g. 27000000
    uint16_t pll_prd; // e.g. 3. See Linux hsck calculations.
    uint16_t pll_fbd; // e.g. 85.
    tc358743_csi_timings_t csi_timings;
    bool csi_timings_provided;
    bool enable_edid;
} tc358743_config_t;

tc358743_handle_t tc358743_init(const tc358743_config_t* config);

// Full start-up sequence (hardware reset, HDMI RX PHY, DDC/EDID, PLL, CSI, RGB888 output, HPD, lock wait).
// Returns ESP_OK when the sequence ran to the end; *out_locked tells whether TMDS and SYNC were detected.
esp_err_t tc358743_start_csi(tc358743_handle_t dev, const tc358743_mode_t* mode, bool* out_locked);
esp_err_t tc358743_start_csi_with_edid(tc358743_handle_t dev, const tc358743_mode_t* mode,
                                       tc358743_edid_profile_t edid_profile, bool* out_locked);
void tc358743_deinit(tc358743_handle_t dev);

// Diagnostic logging
void tc358743_dump_diagnostics(tc358743_handle_t dev);
uint16_t tc358743_poll_interrupts(tc358743_handle_t dev);

// Point-in-time HDMI receiver status and active resolution.
typedef struct {
    uint8_t sys_status;
    bool ddc5v;         // SYS_STATUS bit 0 (MASK_S_DDC5V = 0x01)
    bool tmds;          // SYS_STATUS bit 1 (MASK_S_TMDS  = 0x02)
    bool sync;          // SYS_STATUS bit 7 (MASK_S_SYNC  = 0x80)
    uint32_t width;     // DE_WIDTH_H when tmds && sync, else 0
    uint32_t height;    // DE_WIDTH_V when tmds && sync, else 0
} tc358743_signal_t;

// Reads SYS_STATUS and, when both TMDS and SYNC are asserted, reads DE_WIDTH_H / DE_WIDTH_V.
esp_err_t tc358743_get_signal(tc358743_handle_t dev, tc358743_signal_t* out);

// Re-arms the TC358743 CSI-2 TX block for a newly locked active horizontal width using the mode
// previously passed to tc358743_start_csi().
esp_err_t tc358743_rearm_csi(tc358743_handle_t dev, uint32_t active_width);

// Pulses HPD Low (150 ms) -> High to nudge a connected HDMI source into re-reading EDID.
esp_err_t tc358743_nudge_hpd(tc358743_handle_t dev);

// Reads the active video size measured by the HDMI receiver (DE_WIDTH_H / DE_WIDTH_V).
// Returns ESP_ERR_INVALID_STATE when SYS_STATUS reports no SYNC (size is undefined then).
esp_err_t tc358743_get_active_size(tc358743_handle_t dev, uint32_t* out_width, uint32_t* out_height);

#ifdef __cplusplus
}
#endif
