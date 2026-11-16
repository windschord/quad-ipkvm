#pragma once
// Pure-C, IDF-independent adaptive JPEG quality controller (docs/VIDEO_TRANSPORT.md §2, PR #106).
//
// Adjusts quality for subsequent frames without re-encoding the current frame:
// - frame_bytes > max_frame_bytes or encode_oversize: q -= 10 (down to q_min = min(30, q_cfg))
// - frame_bytes > 85% of max_frame_bytes:             q -= 5  (down to q_min = min(30, q_cfg))
// - 90 consecutive frames < 60% of max_frame_bytes:  q += 5  (up to q_cfg)
// - any downward step or frame >= 60% resets the low_streak hysteresis counter.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPEG_QUALITY_CTRL_MIN_VALID      1u
#define JPEG_QUALITY_CTRL_MAX_VALID      100u

// TODO(hw-verify): tune thresholds/steps/floor on 1080p high-entropy HDMI content
#define JPEG_QUALITY_CTRL_FLOOR_DEFAULT  30u
#define JPEG_QUALITY_CTRL_STEP_OVERSIZE  10u
#define JPEG_QUALITY_CTRL_STEP_PROACTIVE 5u
#define JPEG_QUALITY_CTRL_STEP_RECOVER   5u
#define JPEG_QUALITY_CTRL_HIGH_PCT       85u
#define JPEG_QUALITY_CTRL_LOW_PCT        60u
#define JPEG_QUALITY_CTRL_RECOVER_FRAMES 90u

typedef struct {
    uint8_t q_cfg;          // configured target/ceiling quality (clamped to 1..100)
    uint8_t q_min;          // effective floor: min(JPEG_QUALITY_CTRL_FLOOR_DEFAULT, q_cfg)
    uint8_t q_cur;          // current quality for the next frame (q_min..q_cfg)
    size_t max_frame_bytes; // frame_hub max frame size in bytes
    uint32_t low_streak;    // consecutive frames with frame_bytes < 60% of max_frame_bytes
    uint32_t down_steps;    // cumulative number of downward quality adjustments
    uint32_t up_steps;      // cumulative number of upward quality adjustments
} jpeg_quality_ctrl_t;

// Initializes controller state. q_cfg is clamped to [1, 100]; q_min = min(30, q_cfg).
void jpeg_quality_ctrl_init(jpeg_quality_ctrl_t *c, uint8_t q_cfg, size_t max_frame_bytes);

// Resets active quality back to q_cfg and clears low_streak (e.g., on HDMI mode switch),
// while preserving cumulative down_steps / up_steps telemetry counters.
void jpeg_quality_ctrl_reset_mode(jpeg_quality_ctrl_t *c);

// Feeds one encoded frame observation and returns the updated quality for subsequent frames.
uint8_t jpeg_quality_ctrl_on_frame(jpeg_quality_ctrl_t *c, size_t frame_bytes, bool encode_oversize);

#ifdef __cplusplus
}
#endif
