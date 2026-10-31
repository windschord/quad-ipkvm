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

#ifdef __cplusplus
}
#endif
