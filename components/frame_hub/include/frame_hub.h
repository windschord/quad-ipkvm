#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct frame_hub_t frame_hub_t;

// Frame struct
typedef struct {
    uint8_t* data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint64_t timestamp_ms;
    int64_t seq;
} video_frame_t;

// Callbacks
typedef void* (*frame_hub_alloc_fn)(size_t size, void* ctx);
typedef void (*frame_hub_free_fn)(void* ptr, void* ctx);
typedef void (*frame_hub_lock_fn)(void* ctx);
typedef void (*frame_hub_unlock_fn)(void* ctx);

typedef struct {
    int pool_size;
    int max_clients;
    size_t max_frame_size;

    frame_hub_alloc_fn alloc_cb;
    frame_hub_free_fn free_cb;
    void* alloc_ctx;

    frame_hub_lock_fn lock_cb;
    frame_hub_unlock_fn unlock_cb;
    void* lock_ctx;
} frame_hub_config_t;

typedef struct {
    uint64_t published_frames;
    uint64_t dropped_frames;
    int active_clients;
} frame_hub_stats_t;

frame_hub_t* frame_hub_create(const frame_hub_config_t* config);
void frame_hub_destroy(frame_hub_t* hub);

// Publisher API (assumed to be called by a single thread)
video_frame_t* frame_hub_begin_publish(frame_hub_t* hub, size_t required_size);
void frame_hub_abort_publish(frame_hub_t* hub, video_frame_t* frame);
void frame_hub_commit(frame_hub_t* hub, video_frame_t* frame, size_t size, uint32_t width, uint32_t height, uint64_t timestamp_ms);

// Client API
int frame_hub_register_client(frame_hub_t* hub);
void frame_hub_unregister_client(frame_hub_t* hub, int client_id);

const video_frame_t* frame_hub_get_latest_frame(frame_hub_t* hub, int client_id);
int frame_hub_release_frame(frame_hub_t* hub, int client_id, const video_frame_t* frame);

void frame_hub_get_stats(frame_hub_t* hub, frame_hub_stats_t* stats);

#ifdef __cplusplus
}
#endif
