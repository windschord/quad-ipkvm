#include "csi_video_source_logic.h"

#include <string.h>

csi_jpeg_frame_err_t csi_jpeg_check_frame(const uint8_t *data, size_t len, size_t max_frame_bytes)
{
    if (!data || len < 4u) {
        return CSI_JPEG_FRAME_ERR_EMPTY;
    }
    // Baseline JPEG requires SOI (0xFF 0xD8) at start and EOI (0xFF 0xD9) at end
    // (Source: ITU-T T.81 §B.2.1; docs/INTERFACES.md §1.2).
    if (data[0] != 0xFFu || data[1] != 0xD8u ||
        data[len - 2u] != 0xFFu || data[len - 1u] != 0xD9u) {
        return CSI_JPEG_FRAME_ERR_MARKERS;
    }
    if (max_frame_bytes == 0u || len > max_frame_bytes) {
        return CSI_JPEG_FRAME_ERR_OVERSIZE;
    }
    return CSI_JPEG_FRAME_OK;
}

size_t csi_jpeg_calc_output_buf_size(size_t hub_max_frame_bytes)
{
    if (hub_max_frame_bytes == 0u || hub_max_frame_bytes > CSI_JPEG_MAX_OUT_BYTES) {
        return 0u;
    }
    size_t target = (hub_max_frame_bytes <= CSI_JPEG_MAX_OUT_BYTES / 2u)
                        ? (hub_max_frame_bytes * 2u)
                        : CSI_JPEG_MAX_OUT_BYTES;
    if (target < CSI_JPEG_MIN_RECOMMENDED_OUT) {
        target = CSI_JPEG_MIN_RECOMMENDED_OUT;
    }
    return target;
}

static void normalize_status(kvm_video_sync_state_t in_state,
                             uint32_t in_w, uint32_t in_h,
                             kvm_video_sync_state_t *out_state,
                             uint32_t *out_w, uint32_t *out_h)
{
    if (in_state == KVM_VIDEO_SYNC_LOCKED) {
        if (in_w == 0u || in_h == 0u) {
            *out_state = KVM_VIDEO_SYNC_UNKNOWN;
            *out_w = 0u;
            *out_h = 0u;
        } else {
            *out_state = KVM_VIDEO_SYNC_LOCKED;
            *out_w = in_w;
            *out_h = in_h;
        }
        return;
    }
    if (in_state == KVM_VIDEO_SYNC_NO_SIGNAL) {
        *out_state = KVM_VIDEO_SYNC_NO_SIGNAL;
    } else {
        *out_state = KVM_VIDEO_SYNC_UNKNOWN;
    }
    *out_w = 0u;
    *out_h = 0u;
}

void csi_video_seq_init(csi_video_seq_t *s)
{
    if (!s) {
        return;
    }
    __atomic_store_n(&s->seq, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->state, (int)KVM_VIDEO_SYNC_UNKNOWN, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->width, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->height, 0u, __ATOMIC_SEQ_CST);
}

void csi_video_seq_begin_write(csi_video_seq_t *s)
{
    if (!s) {
        return;
    }
    uint32_t cur = __atomic_load_n(&s->seq, __ATOMIC_SEQ_CST);
    uint32_t odd = (cur & 1u) ? cur : (cur + 1u);
    __atomic_store_n(&s->seq, odd, __ATOMIC_SEQ_CST);
}

void csi_video_seq_end_write(csi_video_seq_t *s, kvm_video_sync_state_t state,
                             uint32_t width, uint32_t height)
{
    if (!s) {
        return;
    }
    kvm_video_sync_state_t norm_state;
    uint32_t norm_w, norm_h;
    normalize_status(state, width, height, &norm_state, &norm_w, &norm_h);

    uint32_t cur = __atomic_load_n(&s->seq, __ATOMIC_SEQ_CST);
    if ((cur & 1u) == 0u) {
        cur += 1u;
        __atomic_store_n(&s->seq, cur, __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(&s->state, (int)norm_state, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->width, norm_w, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->height, norm_h, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->seq, cur + 1u, __ATOMIC_SEQ_CST);
}

void csi_video_seq_write(csi_video_seq_t *s, kvm_video_sync_state_t state,
                         uint32_t width, uint32_t height)
{
    if (!s) {
        return;
    }
    csi_video_seq_begin_write(s);
    csi_video_seq_end_write(s, state, width, height);
}

bool csi_video_seq_read_ex(const csi_video_seq_t *s, uint32_t max_retries,
                           void (*on_mid_read)(void *ctx), void *ctx,
                           csi_video_status_t *out)
{
    if (!out) {
        return false;
    }
    if (!s || max_retries == 0u) {
        out->state = KVM_VIDEO_SYNC_UNKNOWN;
        out->width = 0u;
        out->height = 0u;
        return false;
    }

    for (uint32_t attempt = 0u; attempt < max_retries; attempt++) {
        uint32_t s1 = __atomic_load_n(&s->seq, __ATOMIC_SEQ_CST);
        if ((s1 & 1u) != 0u) {
            continue;
        }
        int raw_state = __atomic_load_n(&s->state, __ATOMIC_SEQ_CST);
        uint32_t raw_w = __atomic_load_n(&s->width, __ATOMIC_SEQ_CST);
        uint32_t raw_h = __atomic_load_n(&s->height, __ATOMIC_SEQ_CST);
        if (on_mid_read) {
            on_mid_read(ctx);
        }
        uint32_t s2 = __atomic_load_n(&s->seq, __ATOMIC_SEQ_CST);
        if (s1 == s2) {
            normalize_status((kvm_video_sync_state_t)raw_state, raw_w, raw_h,
                             &out->state, &out->width, &out->height);
            return true;
        }
    }

    out->state = KVM_VIDEO_SYNC_UNKNOWN;
    out->width = 0u;
    out->height = 0u;
    return false;
}

bool csi_video_seq_read(const csi_video_seq_t *s, uint32_t max_retries,
                        csi_video_status_t *out)
{
    return csi_video_seq_read_ex(s, max_retries, NULL, NULL, out);
}

static void record_frame_common(csi_video_pub_stats_t *st, size_t jpeg_bytes, uint32_t encode_us)
{
    if (st->win_frames == 0u || jpeg_bytes < st->win_min_jpeg_bytes) {
        st->win_min_jpeg_bytes = jpeg_bytes;
    }
    if (jpeg_bytes > st->win_max_jpeg_bytes) {
        st->win_max_jpeg_bytes = jpeg_bytes;
    }
    if (jpeg_bytes > st->peak_jpeg_bytes) {
        st->peak_jpeg_bytes = jpeg_bytes;
    }
    if (encode_us > st->win_max_encode_us) {
        st->win_max_encode_us = encode_us;
    }
    st->win_frames++;
    st->win_encode_us += encode_us;
    st->win_jpeg_bytes += jpeg_bytes;
}

void csi_video_pub_stats_init(csi_video_pub_stats_t *st)
{
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

void csi_video_pub_stats_reset_window(csi_video_pub_stats_t *st)
{
    if (!st) {
        return;
    }
    st->win_frames = 0u;
    st->win_encode_us = 0u;
    st->win_max_encode_us = 0u;
    st->win_jpeg_bytes = 0u;
    st->win_min_jpeg_bytes = 0u;
    st->win_max_jpeg_bytes = 0u;
}

void csi_video_pub_stats_on_published(csi_video_pub_stats_t *st, size_t jpeg_bytes, uint32_t encode_us)
{
    if (!st) {
        return;
    }
    st->published++;
    record_frame_common(st, jpeg_bytes, encode_us);
}

void csi_video_pub_stats_on_pool_busy(csi_video_pub_stats_t *st, size_t jpeg_bytes, uint32_t encode_us)
{
    if (!st) {
        return;
    }
    st->pool_busy_drops++;
    record_frame_common(st, jpeg_bytes, encode_us);
}

void csi_video_pub_stats_on_oversize(csi_video_pub_stats_t *st, size_t jpeg_bytes, uint32_t encode_us)
{
    if (!st) {
        return;
    }
    st->oversize_drops++;
    record_frame_common(st, jpeg_bytes, encode_us);
}

void csi_video_pub_stats_on_error(csi_video_pub_stats_t *st)
{
    if (!st) {
        return;
    }
    st->encode_errors++;
}

static const uint32_t k_hpd_nudge_delays_ms[CSI_HPD_NUDGE_STEPS] = {
    CSI_HPD_NUDGE_DELAY_0_MS,
    CSI_HPD_NUDGE_DELAY_1_MS,
    CSI_HPD_NUDGE_DELAY_2_MS,
};

void csi_mode_tracker_init(csi_mode_tracker_t *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
}

csi_mode_action_t csi_mode_tracker_step(csi_mode_tracker_t *m, const csi_mode_sample_t *s)
{
    if (!m || !s) {
        return CSI_MODE_ACT_WAIT;
    }

    if (!s->sync_locked) {
        m->cand_w = 0u;
        m->cand_h = 0u;
        m->cand_hits = 0u;

        if (m->streaming) {
            m->streaming = false;
            m->active_w = 0u;
            m->active_h = 0u;
            m->nudge_stage = 0u;
            if (s->ddc5v) {
                m->nudge_timer_active = true;
                m->nudge_since_ms = s->now_ms;
            } else {
                m->nudge_timer_active = false;
                m->nudge_since_ms = 0u;
            }
            return CSI_MODE_ACT_STOP_NO_SIGNAL;
        }

        if (!s->ddc5v) {
            m->nudge_timer_active = false;
            m->nudge_since_ms = 0u;
            m->nudge_stage = 0u;
            return CSI_MODE_ACT_WAIT;
        }

        if (!m->nudge_timer_active) {
            m->nudge_timer_active = true;
            m->nudge_since_ms = s->now_ms;
            return CSI_MODE_ACT_WAIT;
        }

        if (m->nudge_stage < CSI_HPD_NUDGE_STEPS &&
            s->now_ms >= m->nudge_since_ms &&
            (s->now_ms - m->nudge_since_ms) >= k_hpd_nudge_delays_ms[m->nudge_stage]) {
            m->nudge_stage++;
            m->nudge_since_ms = s->now_ms;
            m->hpd_nudges++;
            return CSI_MODE_ACT_NUDGE_HPD;
        }
        return CSI_MODE_ACT_WAIT;
    }

    // HDMI TMDS + SYNC are locked: clear no-signal HPD nudge state.
    m->nudge_timer_active = false;
    m->nudge_since_ms = 0u;
    m->nudge_stage = 0u;

    if (!s->geom_ok || s->width == 0u || s->height == 0u) {
        m->cand_w = 0u;
        m->cand_h = 0u;
        m->cand_hits = 0u;
        m->streaming = false;
        m->active_w = 0u;
        m->active_h = 0u;
        return CSI_MODE_ACT_REJECT_OUT_OF_RANGE;
    }

    if (m->streaming) {
        if (s->width == m->active_w && s->height == m->active_h) {
            return CSI_MODE_ACT_KEEP_STREAMING;
        }
        m->streaming = false;
        m->active_w = 0u;
        m->active_h = 0u;
        m->cand_w = s->width;
        m->cand_h = s->height;
        m->cand_hits = 1u;
        return CSI_MODE_ACT_STOP_MODE_CHANGED;
    }

    if (m->cand_hits > 0u && m->cand_w == s->width && m->cand_h == s->height) {
        m->cand_hits++;
    } else {
        m->cand_w = s->width;
        m->cand_h = s->height;
        m->cand_hits = 1u;
    }

    if (m->cand_hits >= CSI_MODE_STABLE_COUNT) {
        m->streaming = true;
        m->active_w = s->width;
        m->active_h = s->height;
        m->last_frame_ms = s->now_ms;
        m->mode_switches++;
        return CSI_MODE_ACT_START_MODE;
    }
    return CSI_MODE_ACT_WAIT;
}

void csi_mode_tracker_on_frame(csi_mode_tracker_t *m, uint64_t now_ms)
{
    if (!m || !m->streaming) {
        return;
    }
    m->last_frame_ms = now_ms;
}

csi_mode_action_t csi_mode_tracker_check_stall(csi_mode_tracker_t *m, uint64_t now_ms)
{
    if (!m || !m->streaming) {
        return CSI_MODE_ACT_WAIT;
    }
    if (now_ms >= m->last_frame_ms &&
        (now_ms - m->last_frame_ms) >= CSI_MODE_STALL_TIMEOUT_MS) {
        if (!m->has_recovered ||
            (now_ms >= m->last_recovery_ms &&
             (now_ms - m->last_recovery_ms) >= CSI_MODE_RECOVERY_COOLDOWN_MS)) {
            m->streaming = false;
            m->active_w = 0u;
            m->active_h = 0u;
            m->cand_w = 0u;
            m->cand_h = 0u;
            m->cand_hits = 0u;
            m->has_recovered = true;
            m->last_recovery_ms = now_ms;
            m->stall_recoveries++;
            return CSI_MODE_ACT_RECOVER_STALL;
        }
    }
    return CSI_MODE_ACT_KEEP_STREAMING;
}

csi_mode_action_t csi_mode_tracker_poll(csi_mode_tracker_t *m, bool sample_ok,
                                        const csi_mode_sample_t *s)
{
    if (!m) {
        return CSI_MODE_ACT_WAIT;
    }
    if (!sample_ok || !s) {
        m->sample_errors++;
        return m->streaming ? CSI_MODE_ACT_KEEP_STREAMING : CSI_MODE_ACT_WAIT;
    }
    csi_mode_action_t act = csi_mode_tracker_step(m, s);
    if (act == CSI_MODE_ACT_KEEP_STREAMING) {
        act = csi_mode_tracker_check_stall(m, s->now_ms);
    }
    return act;
}

void csi_mode_tracker_abort_start(csi_mode_tracker_t *m)
{
    if (!m) {
        return;
    }
    m->streaming = false;
    m->active_w = 0u;
    m->active_h = 0u;
    m->cand_w = 0u;
    m->cand_h = 0u;
    m->cand_hits = 0u;
}
