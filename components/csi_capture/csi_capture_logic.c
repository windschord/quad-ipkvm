#include "csi_capture_logic.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define CSI_CAPTURE_IRAM_ATTR IRAM_ATTR
#else
#define CSI_CAPTURE_IRAM_ATTR
#endif

uint32_t csi_pixel_format_bpp(csi_pixel_format_t fmt) {
    if (fmt == CSI_PIXEL_FORMAT_RGB888) return 24u;
    if (fmt == CSI_PIXEL_FORMAT_UYVY) return 16u;
    return 0u;
}

csi_geom_result_t csi_capture_check_format_geometry(uint32_t width, uint32_t height,
                                                    csi_pixel_format_t fmt,
                                                    uint32_t max_width, uint32_t max_height,
                                                    size_t capacity, size_t *out_bytes) {
    if (!out_bytes) return CSI_GEOM_ERR_ARG;
    *out_bytes = 0;
    uint32_t bpp = csi_pixel_format_bpp(fmt);
    if (bpp == 0u) return CSI_GEOM_ERR_BPP;
    if (width == 0u || height == 0u) return CSI_GEOM_ERR_ZERO;
    if (fmt == CSI_PIXEL_FORMAT_UYVY && (width % 2u) != 0u) return CSI_GEOM_ERR_ALIGN;
    return csi_capture_check_geometry(width, height, bpp, max_width, max_height, capacity, out_bytes);
}

csi_geom_result_t csi_capture_check_geometry(uint32_t width, uint32_t height, uint32_t bits_per_pixel,
                                             uint32_t max_width, uint32_t max_height,
                                             size_t capacity, size_t *out_bytes) {
    if (!out_bytes) return CSI_GEOM_ERR_ARG;
    *out_bytes = 0;
    if (width == 0 || height == 0 || bits_per_pixel == 0) return CSI_GEOM_ERR_ZERO;
    if ((bits_per_pixel % 8u) != 0 || bits_per_pixel > 32u) return CSI_GEOM_ERR_BPP;
    if (width > max_width || height > max_height) return CSI_GEOM_ERR_DIM;

    uint64_t bits = (uint64_t)width * (uint64_t)height * (uint64_t)bits_per_pixel;
    // The driver computes v_res * h_res * bpp in `int`; keep it representable.
    if (bits > (uint64_t)INT32_MAX) return CSI_GEOM_ERR_OVERFLOW;
    // DMA transfer size is bits / 64; a remainder would leave the frame partially transferred.
    if ((bits % 64u) != 0) return CSI_GEOM_ERR_ALIGN;

    uint64_t bytes = bits / 8u;
    if (bytes > (uint64_t)capacity) return CSI_GEOM_ERR_CAPACITY;
    *out_bytes = (size_t)bytes;
    return CSI_GEOM_OK;
}

size_t csi_capture_round_up(size_t n, size_t align) {
    if (align == 0) return 0;
    size_t rem = n % align;
    if (rem == 0) return n;
    size_t add = align - rem;
    if (n > (size_t)-1 - add) return 0;
    return n + add;
}

void csi_pp_init(csi_pp_t *pp, uint32_t skip_frames) {
    pp->writing = CSI_PP_NONE;
    pp->latest = CSI_PP_NONE;
    pp->held = CSI_PP_NONE;
    pp->skip_frames = skip_frames;
    pp->completed = 0;
    pp->published = 0;
    pp->overwritten = 0;
}

// Conservation invariant across all transitions:
//   published == acquired + overwritten + (latest != CSI_PP_NONE ? 1 : 0)
int CSI_CAPTURE_IRAM_ATTR csi_pp_next_dma(csi_pp_t *pp) {
    if (pp->writing != CSI_PP_NONE) {
        pp->completed++;
        if (pp->completed > pp->skip_frames) {
            if (pp->latest != CSI_PP_NONE && pp->latest != pp->writing) {
                pp->overwritten++;  // previous unconsumed latest is being replaced
            }
            pp->latest = pp->writing;
            pp->published++;
        }
    }

    // Prefer a buffer that is neither held by the consumer nor the newest complete frame.
    int next = CSI_PP_NONE;
    for (int i = 0; i < CSI_PP_NUM_BUFS; i++) {
        if (i != pp->held && i != pp->latest) {
            next = i;
            break;
        }
    }
    if (next == CSI_PP_NONE) {
        // Both buffers are taken (one held, one latest): reclaim the unconsumed latest frame.
        next = pp->latest;
        pp->latest = CSI_PP_NONE;
        pp->overwritten++;
    }
    pp->writing = (int8_t)next;
    return next;
}

int csi_pp_acquire(csi_pp_t *pp) {
    if (pp->held != CSI_PP_NONE) return CSI_PP_NONE;
    if (pp->latest == CSI_PP_NONE) return CSI_PP_NONE;
    pp->held = pp->latest;
    pp->latest = CSI_PP_NONE;
    return pp->held;
}

bool csi_pp_release(csi_pp_t *pp, int idx) {
    if (idx == CSI_PP_NONE || idx != pp->held) return false;
    pp->held = CSI_PP_NONE;
    return true;
}

void csi_pp_stop(csi_pp_t *pp) {
    pp->writing = CSI_PP_NONE;
}

csi_jpeg_bitstream_result_t csi_jpeg_validate_bitstream(const uint8_t *buf,
                                                        size_t max_output_bytes,
                                                        size_t encoded_bytes) {
    if (!buf || max_output_bytes == 0u) {
        return CSI_JPEG_BITSTREAM_ERR_MARKERS;
    }
    if (encoded_bytes > max_output_bytes) {
        return CSI_JPEG_BITSTREAM_ERR_OVERSIZE;
    }
    if (encoded_bytes < 4u ||
        buf[0] != 0xFFu || buf[1] != 0xD8u ||
        buf[encoded_bytes - 2u] != 0xFFu || buf[encoded_bytes - 1u] != 0xD9u) {
        return CSI_JPEG_BITSTREAM_ERR_MARKERS;
    }
    return CSI_JPEG_BITSTREAM_OK;
}

void csi_jpeg_arm_overflow_sentinel(uint8_t *tail_buf, size_t tail_len) {
    if (!tail_buf || tail_len == 0u) {
        return;
    }
    for (size_t i = 0u; i < tail_len; i++) {
        tail_buf[i] = (i & 1u) ? 0xA5u : 0xFFu;
    }
}

bool csi_jpeg_overflow_sentinel_tripped(const uint8_t *tail_buf, size_t tail_len) {
    if (!tail_buf || tail_len == 0u) {
        return false;
    }
    for (size_t i = 0u; i < tail_len; i++) {
        uint8_t expected = (i & 1u) ? 0xA5u : 0xFFu;
        if (tail_buf[i] != expected) {
            return true;
        }
    }
    return false;
}
