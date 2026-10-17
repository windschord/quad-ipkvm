#include "frame_hub.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    video_frame_t frame;
    size_t allocated_size;
    int ref_count;
    bool in_use;
    bool is_latest;
} frame_buffer_t;

typedef struct {
    int client_id;
    bool active;
    uint32_t last_seq_num;
} client_state_t;

struct frame_hub_s {
    frame_hub_config_t config;
    frame_buffer_t* pool;
    client_state_t* clients;

    uint32_t current_seq_num;
    int latest_buffer_idx;

    frame_hub_lock_fn lock;
    frame_hub_unlock_fn unlock;
    void* lock_ctx;

    frame_hub_stats_t stats;
};

frame_hub_t* frame_hub_create(const frame_hub_config_t* config) {
    if (!config || !config->alloc || !config->free || config->pool_size == 0 || config->max_clients == 0) {
        return NULL;
    }

    frame_hub_t* hub = (frame_hub_t*)config->alloc(sizeof(frame_hub_t));
    if (!hub) return NULL;

    memset(hub, 0, sizeof(frame_hub_t));
    hub->config = *config;
    hub->latest_buffer_idx = -1;

    hub->pool = (frame_buffer_t*)config->alloc(sizeof(frame_buffer_t) * config->pool_size);
    if (!hub->pool) {
        config->free(hub);
        return NULL;
    }
    memset(hub->pool, 0, sizeof(frame_buffer_t) * config->pool_size);

    hub->clients = (client_state_t*)config->alloc(sizeof(client_state_t) * config->max_clients);
    if (!hub->clients) {
        config->free(hub->pool);
        config->free(hub);
        return NULL;
    }
    memset(hub->clients, 0, sizeof(client_state_t) * config->max_clients);

    return hub;
}

void frame_hub_destroy(frame_hub_t* hub) {
    if (!hub) return;
    for (size_t i = 0; i < hub->config.pool_size; ++i) {
        if (hub->pool[i].frame.data) {
            hub->config.free(hub->pool[i].frame.data);
        }
    }
    hub->config.free(hub->pool);
    hub->config.free(hub->clients);
    hub->config.free(hub);
}

void frame_hub_set_lock_callbacks(frame_hub_t* hub, frame_hub_lock_fn lock, frame_hub_unlock_fn unlock, void* ctx) {
    if (!hub) return;
    hub->lock = lock;
    hub->unlock = unlock;
    hub->lock_ctx = ctx;
}

static void do_lock(frame_hub_t* hub) {
    if (hub->lock) hub->lock(hub->lock_ctx);
}

static void do_unlock(frame_hub_t* hub) {
    if (hub->unlock) hub->unlock(hub->lock_ctx);
}

video_frame_t* frame_hub_begin_publish(frame_hub_t* hub, size_t required_size) {
    if (!hub || required_size == 0) return NULL;
    do_lock(hub);

    int chosen_idx = -1;
    uint32_t oldest_seq = 0xFFFFFFFF;

    // 1. Try to find a completely unused buffer
    for (size_t i = 0; i < hub->config.pool_size; ++i) {
        if (!hub->pool[i].in_use && hub->pool[i].ref_count == 0 && !hub->pool[i].is_latest) {
            chosen_idx = i;
            break;
        }
    }

    // 2. Try to replace oldest non-referenced buffer that is not the latest
    if (chosen_idx == -1) {
        for (size_t i = 0; i < hub->config.pool_size; ++i) {
            if (hub->pool[i].ref_count == 0 && !hub->pool[i].in_use && (int)i != hub->latest_buffer_idx) {
                if (hub->pool[i].frame.sequence_num < oldest_seq) {
                    oldest_seq = hub->pool[i].frame.sequence_num;
                    chosen_idx = i;
                }
            }
        }
    }

    if (chosen_idx == -1) {
        hub->stats.dropped_frames++;
        do_unlock(hub);
        return NULL; // Pool exhausted
    }

    frame_buffer_t* buf = &hub->pool[chosen_idx];
    if (buf->allocated_size < required_size) {
        if (buf->frame.data) hub->config.free(buf->frame.data);
        buf->frame.data = (uint8_t*)hub->config.alloc(required_size);
        if (!buf->frame.data) {
            buf->allocated_size = 0;
            do_unlock(hub);
            return NULL;
        }
        buf->allocated_size = required_size;
    }

    buf->in_use = true;
    buf->is_latest = false;
    do_unlock(hub);

    return &buf->frame;
}

void frame_hub_commit(frame_hub_t* hub, video_frame_t* frame, uint32_t width, uint32_t height, uint64_t timestamp_ms) {
    if (!hub || !frame) return;
    do_lock(hub);

    int buf_idx = -1;
    for (size_t i = 0; i < hub->config.pool_size; ++i) {
        if (&hub->pool[i].frame == frame) {
            buf_idx = i;
            break;
        }
    }

    if (buf_idx != -1) {
        frame_buffer_t* buf = &hub->pool[buf_idx];
        buf->frame.width = width;
        buf->frame.height = height;
        buf->frame.timestamp_ms = timestamp_ms;
        buf->frame.sequence_num = ++hub->current_seq_num;

        if (hub->latest_buffer_idx != -1) {
             hub->pool[hub->latest_buffer_idx].is_latest = false;
        }
        hub->latest_buffer_idx = buf_idx;
        buf->is_latest = true;
        buf->in_use = false;
        hub->stats.published_frames++;
    }

    do_unlock(hub);
}

void frame_hub_abort_publish(frame_hub_t* hub, video_frame_t* frame) {
    if (!hub || !frame) return;
    do_lock(hub);
    for (size_t i = 0; i < hub->config.pool_size; ++i) {
        if (&hub->pool[i].frame == frame) {
            hub->pool[i].in_use = false;
            break;
        }
    }
    do_unlock(hub);
}

int frame_hub_register_client(frame_hub_t* hub) {
    if (!hub) return -1;
    do_lock(hub);
    int client_id = -1;
    for (size_t i = 0; i < hub->config.max_clients; ++i) {
        if (!hub->clients[i].active) {
            hub->clients[i].active = true;
            hub->clients[i].client_id = i;
            hub->clients[i].last_seq_num = 0;
            hub->stats.active_clients++;
            client_id = i;
            break;
        }
    }
    do_unlock(hub);
    return client_id;
}

void frame_hub_unregister_client(frame_hub_t* hub, int client_id) {
    if (!hub || client_id < 0 || client_id >= (int)hub->config.max_clients) return;
    do_lock(hub);
    if (hub->clients[client_id].active) {
        hub->clients[client_id].active = false;
        hub->stats.active_clients--;
    }
    do_unlock(hub);
}

video_frame_t* frame_hub_get_latest_frame(frame_hub_t* hub, int client_id) {
    if (!hub || client_id < 0 || client_id >= (int)hub->config.max_clients) return NULL;
    do_lock(hub);

    if (!hub->clients[client_id].active || hub->latest_buffer_idx == -1) {
        do_unlock(hub);
        return NULL;
    }

    frame_buffer_t* latest_buf = &hub->pool[hub->latest_buffer_idx];

    if (latest_buf->frame.sequence_num > hub->clients[client_id].last_seq_num) {
        latest_buf->ref_count++;
        hub->clients[client_id].last_seq_num = latest_buf->frame.sequence_num;
        do_unlock(hub);
        return &latest_buf->frame;
    }

    do_unlock(hub);
    return NULL;
}

void frame_hub_release_frame(frame_hub_t* hub, video_frame_t* frame) {
    if (!hub || !frame) return;
    do_lock(hub);
    for (size_t i = 0; i < hub->config.pool_size; ++i) {
        if (&hub->pool[i].frame == frame) {
            if (hub->pool[i].ref_count > 0) {
                hub->pool[i].ref_count--;
            }
            break;
        }
    }
    do_unlock(hub);
}

void frame_hub_get_stats(frame_hub_t* hub, frame_hub_stats_t* stats) {
    if (!hub || !stats) return;
    do_lock(hub);
    *stats = hub->stats;
    do_unlock(hub);
}
