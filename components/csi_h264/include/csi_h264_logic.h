#pragma once
// Pure-C, IDF-independent logic for csi_h264 (host-unit-tested):
// 1. Resolution/alignment/buffer sizing and encoder parameter validation (ESP32-P4 rev v1.3).
// 2. Annex B H.264 access-unit validation (start code + SPS/IDR check + oversize check).
// 3. Lock-free/multi-task-safe codec switch & keyframe request coordination state machine
//    (docs/VIDEO_TRANSPORT.md §9.5-9.7, docs/INTERFACES.md §1.4).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ESP32-P4 rev v1.3 HW H.264 & P7 contract resolution limits (<= 1920x1080).
#define CSI_H264_MIN_WIDTH       80u
#define CSI_H264_MAX_WIDTH       1920u
#define CSI_H264_MIN_HEIGHT      80u
#define CSI_H264_MAX_HEIGHT      1080u

// Macroblock alignment (16 px) and PSRAM DMA/cache-line alignment (128 B).
#define CSI_H264_ALIGN_DIM       16u
#define CSI_H264_DMA_ALIGN       128u
#define CSI_H264_SPS_HEADROOM    CSI_H264_DMA_ALIGN
#define CSI_H264_MAX_OUT_BYTES   (4u * 1024u * 1024u)

// esp_h264 HW parameters (gop is uint8_t > 0; qp is 0..51).
#define CSI_H264_MIN_GOP         1u
#define CSI_H264_MAX_GOP         255u
#define CSI_H264_MAX_QP          51u

// Frames to stay on MJPEG fallback after H.264 OOM or persistent encode failures before retrying (~2 s at 30 fps).
#define CSI_H264_OOM_RETRY_FRAMES    60u
// Consecutive non-OOM encode/oversize failure frames before falling back to MJPEG.
#define CSI_H264_MAX_CONSEC_FAILURES 5u
// Max leading byte span scanned in csi_h264_check_frame() when locating the first IDR slice (type 5).
// Prevents scanning hundreds of KB of IDR slice entropy payload byte-by-byte.
#define CSI_H264_KEY_SCAN_LIMIT      4096u

typedef enum {
    CSI_H264_CODEC_MJPEG = 0,
    CSI_H264_CODEC_H264 = 1,
} csi_h264_codec_t;

typedef enum {
    CSI_H264_FRAME_OK = 0,
    CSI_H264_FRAME_ERR_EMPTY,     // NULL pointer or fewer than 5 bytes
    CSI_H264_FRAME_ERR_FORMAT,    // missing Annex B start code 00 00 00 01, invalid NAL header, or missing SPS/IDR when key expected
    CSI_H264_FRAME_ERR_OVERSIZE,  // exceeds Frame Hub max_frame_size
} csi_h264_frame_err_t;

typedef enum {
    CSI_H264_OUTCOME_PUBLISHED = 0,   // encoded frame committed to frame_hub
    CSI_H264_OUTCOME_PUBLISH_BUSY,    // frame_hub_begin_publish() returned NULL (pool full)
    CSI_H264_OUTCOME_OVERSIZE,        // exceeded max_frame_bytes or HW bitstream overflow
    CSI_H264_OUTCOME_ENCODE_ERR,      // PPA / HW encode / bitstream error
    CSI_H264_OUTCOME_FALLBACK_MJPEG,  // H.264 encoder unavailable on this frame; fell back cleanly without disturbing H.264 ref
} csi_h264_frame_outcome_t;

typedef struct {
    bool h264_capable;         // accessed via __atomic_* so runtime init failure can disable H.264 safely
    uint8_t gop;

    // Cross-task lock-free fields (accessed with __atomic_* builtins).
    uint32_t requested_codec;  // written by web_server (set_codec), read by publisher
    uint32_t requested_epoch;  // bumped by set_codec() so new requests clear OOM cooldown
    uint32_t active_codec;     // written by publisher, read by request_keyframe (MJPEG no-op)
    uint32_t keyframe_req;     // set by request_keyframe(), atomically cleared by publisher

    // Publisher-thread-only state.
    bool need_idr;
    uint32_t active_w;
    uint32_t active_h;
    uint32_t frames_since_idr;
    uint32_t oom_cooldown_frames;
    uint32_t consec_failures;
    uint32_t seen_req_epoch;

    // Telemetry counters (publisher-thread-only).
    uint32_t switches_to_h264;
    uint32_t switches_to_mjpeg;
    uint64_t idr_frames;
    uint64_t p_frames;
    uint64_t keyframe_reqs_applied;
    uint64_t self_idr_recoveries;
} csi_h264_ctrl_t;

// Rounds val up to a multiple of align. Returns 0 if align == 0 or on size_t overflow.
size_t csi_h264_align_up(size_t val, size_t align);

// Checks that (w, h) fits HW/contract limits [80..1920] x [80..1080], has 16-px-aligned
// width (required until non-16-aligned widths are verified on HW), and even height for YUV420 PPA.
bool csi_h264_check_resolution(uint32_t w, uint32_t h);

// Computes 16-px macroblock-aligned O_UYY_E_VYY buffer size (e.g. 1920x1080 -> 3,133,440 B).
// Returns 0 if resolution is invalid.
size_t csi_h264_calc_yuv_bytes(uint32_t w, uint32_t h);

// Computes the active PPA YUV420 byte extent before vertical 16-px zero padding
// (align16(w) * h * 3 / 2; e.g. 1920x1080 -> 3,110,400 B). Returns 0 if resolution is invalid.
size_t csi_h264_calc_ppa_active_bytes(uint32_t w, uint32_t h);

// Computes the 128-B cache-aligned output buffer size for esp_h264 (capped at 4 MiB).
// Returns 0 if hub_max_frame_bytes is 0 or above the cap / cannot align.
size_t csi_h264_calc_output_buf_size(size_t hub_max_frame_bytes);

// Returns nominal rate-control FPS for ESP32-P4 rev v1.3 PPA+H.264 pipeline
// (~20 fps for <=720p, smoothly scaled down to 9 fps for 1080p).
uint8_t csi_h264_calc_target_fps(uint32_t w, uint32_t h);

// Rewrites a Baseline SPS NAL payload (EBSP without start code, starting at 0x67) to set
// fixed_frame_rate_flag = 0 (when VUI timing_info is present) and append VUI bitstream_restriction
// with max_num_reorder_frames = 0, max_dec_frame_buffering = 1 so decoders release frames immediately.
// Returns rewritten EBSP byte count, or 0 if unsupported/malformed/out_cap too small.
size_t csi_h264_patch_sps_low_latency(const uint8_t *sps_in, size_t sps_in_len,
                                      uint8_t *sps_out, size_t sps_out_cap);

// Validates static HW encoder configuration parameters before esp_h264_enc_hw_new().
bool csi_h264_validate_enc_params(uint32_t w, uint32_t h, uint8_t gop, uint8_t fps,
                                  uint32_t bitrate_bps, uint8_t qp_min, uint8_t qp_max);

// Validates an Annex B H.264 access unit and Frame Hub size bound.
// When expect_key is true, verifies the leading NAL unit is SPS (type 7), checks
// forbidden_zero_bit == 0 on prefix NAL headers within CSI_H264_KEY_SCAN_LIMIT bytes,
// and stops as soon as a valid IDR slice (type 5) is found.
csi_h264_frame_err_t csi_h264_check_frame(const uint8_t *data, size_t len,
                                          bool expect_key, size_t max_frame_bytes);

// ---- Lock-free codec & keyframe coordinator ----
void csi_h264_ctrl_init(csi_h264_ctrl_t *c, bool h264_capable, uint8_t gop);

// Permanently disables H.264 capability at runtime (e.g. if csi_h264_create() fails),
// resetting active_codec to MJPEG and discarding any pending keyframe request.
// Thread-safe with concurrent set_codec() / request_keyframe() callers.
void csi_h264_ctrl_disable_h264(csi_h264_ctrl_t *c);

// Safe to call concurrently from any task (non-blocking, asynchronous).
// Returns 0 if accepted, -1 if invalid or unsupported.
int csi_h264_ctrl_set_codec(csi_h264_ctrl_t *c, csi_h264_codec_t codec);

// Safe to call concurrently from any task/core without locks (e.g. while holding s_ws_mutex).
// Strict no-op when H.264 is disabled/not supported or when MJPEG is active (contract §9.7).
void csi_h264_ctrl_request_keyframe(csi_h264_ctrl_t *c);

// Returns the currently requested codec snapshot (for diagnostics).
csi_h264_codec_t csi_h264_ctrl_get_requested_codec(const csi_h264_ctrl_t *c);
csi_h264_codec_t csi_h264_ctrl_get_active_codec(const csi_h264_ctrl_t *c);

// Called by publisher task when HDMI mode resets / restarts / stops.
void csi_h264_ctrl_on_mode_reset(csi_h264_ctrl_t *c);

// Called at the start of encoding one captured frame in the publisher task.
// Outputs whether to encode H.264 on this frame and whether IDR must be forced.
// Returns true if the active codec transitioned on this frame.
bool csi_h264_ctrl_begin_frame(csi_h264_ctrl_t *c, uint32_t width, uint32_t height,
                               bool *out_use_h264, bool *out_force_idr);

// Called in the publisher task after finishing one H.264 frame attempt.
// Latches self-IDR recovery on publish failure / oversize / encode error and updates GOP state.
// After CSI_H264_MAX_CONSEC_FAILURES (5) consecutive non-OOM failures (ENCODE_ERR, OVERSIZE, or
// unexpected non-IDR when IDR was required; PUBLISH_BUSY excluded), automatically falls back to
// MJPEG for CSI_H264_OOM_RETRY_FRAMES (60) frames and discards any pending keyframe request.
void csi_h264_ctrl_end_frame(csi_h264_ctrl_t *c, csi_h264_frame_outcome_t outcome,
                             bool actual_is_idr);

#ifdef __cplusplus
}
#endif
