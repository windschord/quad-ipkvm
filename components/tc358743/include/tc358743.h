#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <driver/i2c_master.h>

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
void tc358743_deinit(tc358743_handle_t dev);

// Diagnostic logging
void tc358743_dump_diagnostics(tc358743_handle_t dev);
uint16_t tc358743_poll_interrupts(tc358743_handle_t dev);

#ifdef __cplusplus
}
#endif
