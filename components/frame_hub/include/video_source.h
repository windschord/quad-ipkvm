#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * Interface contract for video frames.
 */
typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint64_t sequence_num;
    uint64_t timestamp_ms;
} video_frame_t;

#ifdef __cplusplus
}
#endif
