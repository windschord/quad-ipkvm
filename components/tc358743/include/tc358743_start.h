#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// EDID profiles
typedef enum {
    TC358743_EDID_PROFILE_720P_60 = 0,
    TC358743_EDID_PROFILE_1080P_30
} tc358743_edid_profile_t;

// Color formats
typedef enum {
    TC358743_FORMAT_RGB888 = 0,
    TC358743_FORMAT_UYVY = 1
} tc358743_format_t;

typedef struct {
    uint32_t lane_rate_mbps;
    uint32_t lanes;
    tc358743_format_t format;
    bool continuous_clock;
} tc358743_mode_t;

// Abstract operations for the start sequence
typedef struct {
    void* ctx;
    bool (*write8)(void* ctx, uint16_t reg, uint8_t val);
    bool (*write16)(void* ctx, uint16_t reg, uint16_t val);
    bool (*write32)(void* ctx, uint16_t reg, uint32_t val);
    bool (*read8)(void* ctx, uint16_t reg, uint8_t* val);
    bool (*read16)(void* ctx, uint16_t reg, uint16_t* val);
    void (*delay_ms)(void* ctx, uint32_t ms);
    void (*reset_gpio)(void* ctx, bool level); // true = High (release), false = Low (assert)
} tc358743_io_ops_t;

bool tc358743_start_sequence(const tc358743_io_ops_t* ops, const tc358743_mode_t* mode, tc358743_edid_profile_t edid_profile, bool* out_locked);

// Returns bits-per-pixel for a TC358743 output format (24 for RGB888, 16 for UYVY, 0 if invalid).
uint32_t tc358743_format_bpp(tc358743_format_t format);

// Computes FIFOCTL (in 32-bit words) for a given active horizontal resolution and bits-per-pixel.
// Returns 374 when line_words > 374 (or when inputs are 0), otherwise max(16, line_words * 7 / 8)
// so narrow modes do not stall waiting for a 374-word threshold that a single line never reaches.
// Source: docs/P2B_TC358743_REFERENCE.md §3-5, §7 item 8.
uint16_t tc358743_calc_fifoctl(uint32_t active_width, uint32_t bits_per_pixel);

// Re-arms the TC358743 CSI-2 TX block after HDMI locks onto a new resolution without toggling HPD
// or resetting HDMI RX PHY (mutes stream, updates FIFOCTL, pulses CTXRST, programs CSI lanes/timings
// and colour space, re-enables stream, and writes CSI_START = 1).
// Source: docs/P2B_TC358743_REFERENCE.md §2-2, §7 items 2 & 8.
bool tc358743_rearm_csi_sequence(const tc358743_io_ops_t* ops, const tc358743_mode_t* mode, uint32_t active_width);

// Pulses HPD Low (150 ms) -> High to nudge a connected HDMI source (DDC5V present, no SYNC)
// into re-reading EDID and restarting TMDS output.
// Source: docs/P2B_TC358743_REFERENCE.md §2-2, §7 item 9.
bool tc358743_hpd_nudge_sequence(const tc358743_io_ops_t* ops);

#ifdef __cplusplus
}
#endif
