#pragma once

#include "video_source.h"

typedef struct frame_hub_s frame_hub_t;

typedef void* (*frame_hub_alloc_fn)(size_t size);
typedef void (*frame_hub_free_fn)(void* ptr);

typedef struct {
    frame_hub_alloc_fn alloc;
    frame_hub_free_fn free;
    size_t pool_size;
    size_t max_clients;
} frame_hub_config_t;

// Locking callbacks (for thread safety)
typedef void (*frame_hub_lock_fn)(void* ctx);
typedef void (*frame_hub_unlock_fn)(void* ctx);

frame_hub_t* frame_hub_create(const frame_hub_config_t* config);
void frame_hub_destroy(frame_hub_t* hub);
void frame_hub_set_lock_callbacks(frame_hub_t* hub, frame_hub_lock_fn lock, frame_hub_unlock_fn unlock, void* ctx);

// Publish flow
video_frame_t* frame_hub_begin_publish(frame_hub_t* hub, size_t required_size);
void frame_hub_commit(frame_hub_t* hub, video_frame_t* frame, uint32_t width, uint32_t height, uint64_t timestamp_ms);
void frame_hub_abort_publish(frame_hub_t* hub, video_frame_t* frame);

// Subscribe flow
int frame_hub_register_client(frame_hub_t* hub);
void frame_hub_unregister_client(frame_hub_t* hub, int client_id);
video_frame_t* frame_hub_get_latest_frame(frame_hub_t* hub, int client_id);
void frame_hub_release_frame(frame_hub_t* hub, video_frame_t* frame);

typedef struct {
    uint32_t published_frames;
    uint32_t dropped_frames;
    uint32_t active_clients;
} frame_hub_stats_t;

void frame_hub_get_stats(frame_hub_t* hub, frame_hub_stats_t* stats);
