#pragma once
// Pure-C helpers for csi_capture (no ESP-IDF dependency; host-testable).
//
// 1. Geometry check done BEFORE the CSI DMA is configured: a frame that does not fit the
//    pre-allocated buffer must never reach the DMA (overrunning PSRAM can cause a boot loop).
//    Source: docs/P2B_TC358743_REFERENCE.md §7 item 14, docs/P2B_PLAN.md P2b-4.
// 2. Two-buffer "latest frame" selector driven from the CSI DMA ISR callbacks.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CSI_PIXEL_FORMAT_RGB888 = 0,  // 24 bpp (CSI-2 DT 0x24; B,G,R in PSRAM on rev v1.3)
    CSI_PIXEL_FORMAT_UYVY = 1,    // 16 bpp (CSI-2 DT 0x1E; 4-byte reversed YVYU in PSRAM on rev v1.3)
} csi_pixel_format_t;

// Returns bits-per-pixel for a supported CSI pixel format (24 for RGB888, 16 for UYVY, 0 otherwise).
uint32_t csi_pixel_format_bpp(csi_pixel_format_t fmt);

typedef enum {
    CSI_GEOM_OK = 0,
    CSI_GEOM_ERR_ARG,        // NULL output pointer
    CSI_GEOM_ERR_ZERO,       // width, height or bits per pixel is 0
    CSI_GEOM_ERR_BPP,        // bits per pixel not a multiple of 8, or above 32
    CSI_GEOM_ERR_DIM,        // width/height above the configured maximum
    CSI_GEOM_ERR_ALIGN,      // frame bits not a multiple of 64 (DMA moves 64-bit words) or odd UYVY width
    CSI_GEOM_ERR_OVERFLOW,   // frame bits do not fit the driver's signed 32-bit arithmetic
    CSI_GEOM_ERR_CAPACITY,   // frame bytes exceed the buffer capacity
} csi_geom_result_t;

// Validates a frame geometry against the pre-allocated buffer.
// On CSI_GEOM_OK, *out_bytes is the frame size in bytes (width * height * bpp / 8).
// Source: ESP-IDF v6.0.1 esp_cam_ctlr_csi.c: fb_size = v_res * h_res * out_bpp / 8 (int arithmetic),
//         DMA transfer size = h_res * v_res * in_bpp / 64 (64-bit DMA beats).
csi_geom_result_t csi_capture_check_geometry(uint32_t width, uint32_t height, uint32_t bits_per_pixel,
                                             uint32_t max_width, uint32_t max_height,
                                             size_t capacity, size_t *out_bytes);

// Format-aware wrapper around csi_capture_check_geometry(): resolves format bpp and enforces
// even horizontal width for UYVY 4:2:2 (2 pixels per 32-bit word).
csi_geom_result_t csi_capture_check_format_geometry(uint32_t width, uint32_t height,
                                                    csi_pixel_format_t fmt,
                                                    uint32_t max_width, uint32_t max_height,
                                                    size_t capacity, size_t *out_bytes);

// Rounds n up to a multiple of align. Returns 0 if align is 0 or the result overflows size_t.
size_t csi_capture_round_up(size_t n, size_t align);

// ---- Two-buffer selector ----
#define CSI_PP_NUM_BUFS 2
#define CSI_PP_NONE (-1)

typedef struct {
    int8_t writing;        // buffer the DMA is currently filling, or CSI_PP_NONE (stopped)
    int8_t latest;         // newest complete frame not yet handed out, or CSI_PP_NONE
    int8_t held;           // buffer currently owned by the consumer, or CSI_PP_NONE
    uint32_t skip_frames;  // number of initial completions to discard (may be partial frames)
    uint32_t completed;    // DMA completions observed since init
    uint32_t published;    // completions that became `latest`
    uint32_t overwritten;  // complete frames that were overwritten before being consumed
} csi_pp_t;

void csi_pp_init(csi_pp_t *pp, uint32_t skip_frames);

// Called from on_get_new_trans (both at start and after every DMA completion).
// Accounts for the frame just completed in `writing` (if any) and returns the buffer index for
// the next DMA transfer. The returned buffer is never the one held by the consumer.
int csi_pp_next_dma(csi_pp_t *pp);

// Hands the newest complete frame to the consumer. Returns its buffer index, or CSI_PP_NONE if
// there is no new frame or the consumer already holds one.
int csi_pp_acquire(csi_pp_t *pp);

// Returns a buffer obtained from csi_pp_acquire. Returns false if idx is not the held buffer.
bool csi_pp_release(csi_pp_t *pp, int idx);

// Marks the DMA as stopped (no buffer is being written any more).
void csi_pp_stop(csi_pp_t *pp);

// ---- HW JPEG bitstream & DMA2D RX overflow sentinel helpers ----
// Extra guard bytes allocated beyond caller's max_output_bytes in csi_jpeg_open() so frames
// slightly exceeding max_output_bytes complete DMA2D and fail validate_jpeg_bitstream() with
// CSI_JPEG_ERR_OUTPUT_OVERSIZE, while frames that exhaust the entire DMA2D RX descriptor
// overwrite the trailing cache-line sentinel before JPEG_DMA2D_RX_DESC_EMPTY.
#define CSI_JPEG_OVERFLOW_GUARD_BYTES 4096u

typedef enum {
    CSI_JPEG_BITSTREAM_OK = 0,
    CSI_JPEG_BITSTREAM_ERR_OVERSIZE,  // encoded_bytes > max_output_bytes
    CSI_JPEG_BITSTREAM_ERR_MARKERS,   // NULL buf, max_output_bytes == 0, len < 4, or missing SOI/EOI
} csi_jpeg_bitstream_result_t;

// Validates that buf[0..encoded_bytes-1] fits within max_output_bytes and has SOI (FF D8)
// and EOI (FF D9) markers.
csi_jpeg_bitstream_result_t csi_jpeg_validate_bitstream(const uint8_t *buf,
                                                        size_t max_output_bytes,
                                                        size_t encoded_bytes);

// Fills the trailing DMA2D RX sentinel region with alternating [0xFF, 0xA5] bytes.
// Because jpeg_encoder_process() enables hardware 0xFF->0xFF 0x00 byte-stuffing
// (jpeg_ll_enable_ff_check(hal->dev, true), jpeg_encode.c:247), the 2-byte sequence
// [0xFF, 0xA5] cannot appear in entropy-coded JPEG scan output.
void csi_jpeg_arm_overflow_sentinel(uint8_t *tail_buf, size_t tail_len);

// Returns true if tail_buf[0..tail_len-1] was modified from the armed [0xFF, 0xA5] pattern,
// indicating DMA2D RX filled the output descriptor to the end (JPEG_DMA2D_RX_DESC_EMPTY).
bool csi_jpeg_overflow_sentinel_tripped(const uint8_t *tail_buf, size_t tail_len);

#ifdef __cplusplus
}
#endif
