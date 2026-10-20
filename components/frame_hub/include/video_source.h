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
};

#ifdef __cplusplus
}
#endif
