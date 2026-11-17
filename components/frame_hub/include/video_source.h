#pragma once

#include "frame_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct video_source_t video_source_t;

struct video_source_t {
    const char *name;
    int (*start)(const video_source_t *source, frame_hub_t *hub);
    void (*stop)(const video_source_t *source);
    void *ctx;
    /* Appended for P7 (docs/INTERFACES.md 1.4). Positional initialisers {name, start, stop, ctx} leave these 0 / NULL
     * (= MJPEG only). */
    uint32_t supported_codecs;  // bit mask (1u << VIDEO_CODEC_*). 0 = MJPEG only
    int (*set_codec)(const video_source_t *source, video_codec_t codec);  // 0 = accepted. Asynchronous, non-blocking, may be called repeatedly. NULL = MJPEG only
    void (*request_keyframe)(const video_source_t *source);               // Make the next frame an IDR. Non-blocking, callable from several tasks. NULL = MJPEG only
};

#ifdef __cplusplus
}
#endif
