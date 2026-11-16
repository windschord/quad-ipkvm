#include "jpeg_quality_ctrl.h"

#include <string.h>

static uint8_t clamp_quality(uint8_t q)
{
    if (q < JPEG_QUALITY_CTRL_MIN_VALID) {
        return JPEG_QUALITY_CTRL_MIN_VALID;
    }
    if (q > JPEG_QUALITY_CTRL_MAX_VALID) {
        return JPEG_QUALITY_CTRL_MAX_VALID;
    }
    return q;
}

static size_t pct_floor(size_t max_bytes, uint8_t pct, bool *out_exact)
{
    size_t q = max_bytes / 100u;
    size_t r = max_bytes % 100u;
    size_t rem_num = r * (size_t)pct;
    if (out_exact) {
        *out_exact = (rem_num % 100u) == 0u;
    }
    return q * (size_t)pct + rem_num / 100u;
}

static bool is_above_pct(size_t frame_bytes, size_t max_bytes, uint8_t pct)
{
    return frame_bytes > pct_floor(max_bytes, pct, NULL);
}

static bool is_below_pct(size_t frame_bytes, size_t max_bytes, uint8_t pct)
{
    bool exact = false;
    size_t thresh = pct_floor(max_bytes, pct, &exact);
    return exact ? (frame_bytes < thresh) : (frame_bytes <= thresh);
}

static void step_down(jpeg_quality_ctrl_t *c, uint8_t step)
{
    c->low_streak = 0u;
    if (c->q_cur <= c->q_min) {
        return;
    }
    uint8_t avail = (uint8_t)(c->q_cur - c->q_min);
    uint8_t delta = (avail > step) ? step : avail;
    c->q_cur = (uint8_t)(c->q_cur - delta);
    c->down_steps++;
}

void jpeg_quality_ctrl_init(jpeg_quality_ctrl_t *c, uint8_t q_cfg, size_t max_frame_bytes)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->q_cfg = clamp_quality(q_cfg);
    c->q_min = (c->q_cfg < JPEG_QUALITY_CTRL_FLOOR_DEFAULT)
                   ? c->q_cfg
                   : JPEG_QUALITY_CTRL_FLOOR_DEFAULT;
    c->q_cur = c->q_cfg;
    c->max_frame_bytes = max_frame_bytes;
}

void jpeg_quality_ctrl_reset_mode(jpeg_quality_ctrl_t *c)
{
    if (!c) {
        return;
    }
    c->q_cur = c->q_cfg;
    c->low_streak = 0u;
}

uint8_t jpeg_quality_ctrl_on_frame(jpeg_quality_ctrl_t *c, size_t frame_bytes, bool encode_oversize)
{
    if (!c) {
        return 0u;
    }
    if (c->max_frame_bytes == 0u) {
        return c->q_cur;
    }

    if (encode_oversize || frame_bytes > c->max_frame_bytes) {
        step_down(c, JPEG_QUALITY_CTRL_STEP_OVERSIZE);
        return c->q_cur;
    }

    if (is_above_pct(frame_bytes, c->max_frame_bytes, JPEG_QUALITY_CTRL_HIGH_PCT)) {
        step_down(c, JPEG_QUALITY_CTRL_STEP_PROACTIVE);
        return c->q_cur;
    }

    if (is_below_pct(frame_bytes, c->max_frame_bytes, JPEG_QUALITY_CTRL_LOW_PCT)) {
        if (c->q_cur < c->q_cfg) {
            c->low_streak++;
            if (c->low_streak >= JPEG_QUALITY_CTRL_RECOVER_FRAMES) {
                c->low_streak = 0u;
                uint8_t room = (uint8_t)(c->q_cfg - c->q_cur);
                uint8_t delta = (room > JPEG_QUALITY_CTRL_STEP_RECOVER)
                                    ? JPEG_QUALITY_CTRL_STEP_RECOVER
                                    : room;
                c->q_cur = (uint8_t)(c->q_cur + delta);
                c->up_steps++;
            }
        } else {
            c->low_streak = 0u;
        }
        return c->q_cur;
    }

    c->low_streak = 0u;
    return c->q_cur;
}
