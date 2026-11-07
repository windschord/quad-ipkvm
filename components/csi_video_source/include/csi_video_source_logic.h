#pragma once
// Pure-C, IDF-independent logic for csi_video_source (host-unit-tested):
// 1. Baseline JPEG frame validation & Frame Hub max_frame_size check (docs/INTERFACES.md §1.2).
// 2. Reusable JPEG encoder output buffer sizing.
// 3. Wait-free writer / bounded-retry lock-free seqlock status store (docs/INTERFACES.md §1.3).
// 4. Publish / drop / encode-latency statistics accumulator.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kvm_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Minimum and maximum reusable output buffer sizes for the HW JPEG encoder.
#define CSI_JPEG_MIN_RECOMMENDED_OUT  (512u * 1024u)
#define CSI_JPEG_MAX_OUT_BYTES        (4u * 1024u * 1024u)

// Default bounded retry count for csi_video_seq_read() (docs/INTERFACES.md §1.3).
#define CSI_VIDEO_SEQ_DEFAULT_RETRIES 8u

typedef enum {
    CSI_JPEG_FRAME_OK = 0,
    CSI_JPEG_FRAME_ERR_EMPTY,     // NULL pointer or fewer than 4 bytes
    CSI_JPEG_FRAME_ERR_MARKERS,   // missing SOI (FF D8) or EOI (FF D9)
    CSI_JPEG_FRAME_ERR_OVERSIZE,  // exceeds Frame Hub max_frame_size (INTERFACES.md §1.2(b))
} csi_jpeg_frame_err_t;

// Consistent point-in-time snapshot of HDMI sync state and active mode (docs/INTERFACES.md §1.3).
typedef struct {
    kvm_video_sync_state_t state;
    uint32_t width;   // valid only when state == KVM_VIDEO_SYNC_LOCKED; 0 otherwise
    uint32_t height;
} csi_video_status_t;

// Lock-free seqlock state (no mutex or critical section; safe across dual RISC-V cores).
// Even seq = quiescent; odd seq = writer update in progress.
typedef struct {
    uint32_t seq;
    int state;
    uint32_t width;
    uint32_t height;
} csi_video_seq_t;

// Cumulative + window statistics for csi_video_source telemetry and HW_VERIFY recording.
typedef struct {
    uint64_t published;
    uint64_t pool_busy_drops;   // frame_hub_begin_publish() returned NULL (pool full)
    uint64_t oversize_drops;    // JPEG exceeded hub_max_frame_bytes (INTERFACES.md §1.2(b))
    uint64_t encode_errors;     // HW JPEG error or malformed markers

    uint32_t win_frames;        // encoded frames in current reporting window (published + drops)
    uint64_t win_encode_us;     // sum of encode durations in window
    uint32_t win_max_encode_us; // peak encode duration in window
    uint64_t win_jpeg_bytes;    // sum of encoded JPEG sizes in window
    size_t win_min_jpeg_bytes;  // minimum JPEG size in window (0 if win_frames == 0)
    size_t win_max_jpeg_bytes;  // maximum JPEG size in window (includes oversize frames)
    size_t peak_jpeg_bytes;     // all-time maximum JPEG size observed since start
} csi_video_pub_stats_t;

// Checks that data[0..len-1] starts with SOI (0xFF 0xD8), ends with EOI (0xFF 0xD9),
// and fits within max_frame_bytes (must be > 0).
csi_jpeg_frame_err_t csi_jpeg_check_frame(const uint8_t *data, size_t len, size_t max_frame_bytes);

// Computes the reusable PSRAM output buffer size for csi_jpeg_open():
// at least max(hub_max_frame_bytes * 2, 512 KiB) so oversize frames (> hub_max_frame_bytes)
// still finish DMA2D encoding and report their exact byte size instead of overflowing RX DMA,
// capped at CSI_JPEG_MAX_OUT_BYTES (4 MiB). Returns 0 if hub_max_frame_bytes is 0 or > 4 MiB.
size_t csi_jpeg_calc_output_buf_size(size_t hub_max_frame_bytes);

// Seqlock operations (single writer, arbitrary non-blocking concurrent readers).
void csi_video_seq_init(csi_video_seq_t *s);
void csi_video_seq_begin_write(csi_video_seq_t *s);
void csi_video_seq_end_write(csi_video_seq_t *s, kvm_video_sync_state_t state,
                             uint32_t width, uint32_t height);
void csi_video_seq_write(csi_video_seq_t *s, kvm_video_sync_state_t state,
                         uint32_t width, uint32_t height);

// Reads a consistent snapshot within at most max_retries attempts.
// Optional on_mid_read hook (pass NULL in production) is invoked between reading the payload
// fields and re-reading seq so single-threaded host tests can deterministically exercise
// concurrent writer interference. If retries are exhausted, *out is set to {UNKNOWN, 0, 0}
// and false is returned (docs/INTERFACES.md §1.3).
bool csi_video_seq_read_ex(const csi_video_seq_t *s, uint32_t max_retries,
                           void (*on_mid_read)(void *ctx), void *ctx,
                           csi_video_status_t *out);
bool csi_video_seq_read(const csi_video_seq_t *s, uint32_t max_retries,
                        csi_video_status_t *out);

// Statistics helpers.
void csi_video_pub_stats_init(csi_video_pub_stats_t *st);
void csi_video_pub_stats_reset_window(csi_video_pub_stats_t *st);
void csi_video_pub_stats_on_published(csi_video_pub_stats_t *st, size_t jpeg_bytes, uint32_t encode_us);
void csi_video_pub_stats_on_pool_busy(csi_video_pub_stats_t *st, size_t jpeg_bytes, uint32_t encode_us);
void csi_video_pub_stats_on_oversize(csi_video_pub_stats_t *st, size_t jpeg_bytes, uint32_t encode_us);
void csi_video_pub_stats_on_error(csi_video_pub_stats_t *st);

// ---- P2b-7 Mode-stability, resolution-change & HPD-nudge state machine ----
// Source: docs/P2B_PLAN.md P2b-7, docs/P2B_TC358743_REFERENCE.md §2-2, §7 items 6, 7, 9.
#define CSI_MODE_POLL_INTERVAL_MS     200u
#define CSI_MODE_STABLE_COUNT         2u
#define CSI_MODE_STALL_TIMEOUT_MS     2000u
#define CSI_MODE_RECOVERY_COOLDOWN_MS 8000u
#define CSI_HPD_NUDGE_STEPS           3u
#define CSI_HPD_NUDGE_DELAY_0_MS      10000u
#define CSI_HPD_NUDGE_DELAY_1_MS      20000u
#define CSI_HPD_NUDGE_DELAY_2_MS      40000u

typedef enum {
    CSI_MODE_ACT_WAIT = 0,              // Stay unlocked / waiting for next 200 ms poll
    CSI_MODE_ACT_KEEP_STREAMING,        // Locked resolution unchanged; keep acquiring frames
    CSI_MODE_ACT_START_MODE,            // 2 consecutive identical valid samples; (re)configure & start CSI
    CSI_MODE_ACT_STOP_NO_SIGNAL,        // Signal lost while streaming; stop CSI & publish NO_SIGNAL
    CSI_MODE_ACT_STOP_MODE_CHANGED,     // Active resolution changed while streaming; stop CSI & debounce new mode
    CSI_MODE_ACT_REJECT_OUT_OF_RANGE,   // Sync locked on out-of-range mode; stop CSI (if active) & publish NO_SIGNAL
    CSI_MODE_ACT_NUDGE_HPD,             // DDC5V present without sync for 10s -> 20s -> 40s; pulse HPD
    CSI_MODE_ACT_RECOVER_STALL,         // CSI DMA stalled >= 2000 ms while locked; stop CSI & pulse HPD
} csi_mode_action_t;

typedef struct {
    uint64_t now_ms;
    bool ddc5v;
    bool sync_locked;
    bool geom_ok;
    uint32_t width;
    uint32_t height;
} csi_mode_sample_t;

typedef struct {
    bool streaming;
    uint32_t active_w;
    uint32_t active_h;

    uint32_t cand_w;
    uint32_t cand_h;
    uint32_t cand_hits;

    bool nudge_timer_active;
    uint64_t nudge_since_ms;
    uint32_t nudge_stage;

    uint64_t last_frame_ms;
    bool has_recovered;
    uint64_t last_recovery_ms;
    uint32_t mode_switches;
    uint32_t hpd_nudges;
    uint32_t stall_recoveries;
} csi_mode_tracker_t;

void csi_mode_tracker_init(csi_mode_tracker_t *m);
csi_mode_action_t csi_mode_tracker_step(csi_mode_tracker_t *m, const csi_mode_sample_t *s);
void csi_mode_tracker_on_frame(csi_mode_tracker_t *m, uint64_t now_ms);
csi_mode_action_t csi_mode_tracker_check_stall(csi_mode_tracker_t *m, uint64_t now_ms);
// If CSI start/reconfigure fails after CSI_MODE_ACT_START_MODE, revert streaming state so the
// next 200 ms poll retries cleanly instead of thinking DMA is running.
void csi_mode_tracker_abort_start(csi_mode_tracker_t *m);

#ifdef __cplusplus
}
#endif
