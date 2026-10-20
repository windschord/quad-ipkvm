#include "frame_hub.h"
#include <string.h>

#define MAX_POOL_SIZE 32
#define MAX_CLIENTS 32

typedef struct {
    video_frame_t frame;
    int ref_count;
    uint64_t seq;
    int active;
    int is_writing;
} frame_slot_t;

typedef struct {
    int id;
    int active;
    int is_writing;
    int64_t last_seen_seq;
    frame_slot_t* held_slot;
} client_state_t;

struct frame_hub_t {
    frame_hub_config_t config;
    frame_slot_t* pool;
    client_state_t* clients;

    int64_t latest_seq;
    frame_slot_t* latest_slot;

    frame_hub_stats_t stats;
};

frame_hub_t* frame_hub_create(const frame_hub_config_t* config) {
    if (!config) return NULL;
    if (config->pool_size < 2 || config->pool_size > MAX_POOL_SIZE) return NULL;
    if (config->max_clients < 1 || config->max_clients > MAX_CLIENTS) return NULL;
    if (config->max_frame_size < 1) return NULL;
    if (!config->alloc_cb || !config->free_cb) return NULL;

    frame_hub_t* hub = (frame_hub_t*)config->alloc_cb(sizeof(frame_hub_t), config->alloc_ctx);
    if (!hub) return NULL;

    memset(hub, 0, sizeof(frame_hub_t));
    hub->config = *config;
    hub->latest_seq = -1;

    hub->pool = (frame_slot_t*)config->alloc_cb(sizeof(frame_slot_t) * config->pool_size, config->alloc_ctx);
    if (!hub->pool) {
        config->free_cb(hub, config->alloc_ctx);
        return NULL;
    }
    memset(hub->pool, 0, sizeof(frame_slot_t) * config->pool_size);

    for (int i = 0; i < config->pool_size; i++) {
        hub->pool[i].frame.data = (uint8_t*)config->alloc_cb(config->max_frame_size, config->alloc_ctx);
        if (!hub->pool[i].frame.data) {
            // Cleanup on failure
            for (int j = 0; j < i; j++) {
                config->free_cb(hub->pool[j].frame.data, config->alloc_ctx);
            }
            config->free_cb(hub->pool, config->alloc_ctx);
            config->free_cb(hub, config->alloc_ctx);
            return NULL;
        }
        hub->pool[i].frame.size = config->max_frame_size;
    }

    hub->clients = (client_state_t*)config->alloc_cb(sizeof(client_state_t) * config->max_clients, config->alloc_ctx);
    if (!hub->clients) {
        for (int i = 0; i < config->pool_size; i++) {
            config->free_cb(hub->pool[i].frame.data, config->alloc_ctx);
        }
        config->free_cb(hub->pool, config->alloc_ctx);
        config->free_cb(hub, config->alloc_ctx);
        return NULL;
    }
    memset(hub->clients, 0, sizeof(client_state_t) * config->max_clients);
    for (int i = 0; i < config->max_clients; i++) {
        hub->clients[i].id = i;
        hub->clients[i].last_seen_seq = -1;
    }

    return hub;
}

void frame_hub_destroy(frame_hub_t* hub) {
    if (!hub) return;

    for (int i = 0; i < hub->config.pool_size; i++) {
        if (hub->pool[i].frame.data) {
            hub->config.free_cb(hub->pool[i].frame.data, hub->config.alloc_ctx);
        }
    }
    hub->config.free_cb(hub->pool, hub->config.alloc_ctx);
    hub->config.free_cb(hub->clients, hub->config.alloc_ctx);
    hub->config.free_cb(hub, hub->config.alloc_ctx);
}

video_frame_t* frame_hub_begin_publish(frame_hub_t* hub, size_t required_size) {
    if (!hub) return NULL;
    if (required_size > hub->config.max_frame_size || required_size == 0) return NULL;

    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);

    frame_slot_t* selected_slot = NULL;
    uint64_t oldest_seq = UINT64_MAX;

    for (int i = 0; i < hub->config.pool_size; i++) {
        frame_slot_t* slot = &hub->pool[i];

                if (slot->is_writing) {
            if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
            return NULL;
        }

        // Cannot reuse referenced slots or the latest published slot
        if (slot->ref_count > 0 || slot == hub->latest_slot) {
            continue;
        }

        if (!slot->active) {
            selected_slot = slot;
            break;
        }

        if (slot->seq < oldest_seq) {
            oldest_seq = slot->seq;
            selected_slot = slot;
        }
    }

    if (!selected_slot) {
        hub->stats.dropped_frames++;
        if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
        return NULL;
    }

    selected_slot->active = 1;
    selected_slot->is_writing = 1;
    // Don't update sequence here, update on commit.

    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
    return &selected_slot->frame;
}

void frame_hub_commit(frame_hub_t* hub, video_frame_t* frame, size_t size, uint32_t width, uint32_t height, uint64_t timestamp_ms) {
    if (!hub || !frame) return;

    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);

    // Find slot
    frame_slot_t* slot = NULL;
    for(int i=0; i<hub->config.pool_size; i++) {
        if(&hub->pool[i].frame == frame) {
            slot = &hub->pool[i];
            break;
        }
    }

    if (!slot || !slot->is_writing) {
        if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
        return;
    }

    slot->is_writing = 0;

    if (size > hub->config.max_frame_size || size == 0) {
        // Discard
        slot->active = 0;
        hub->stats.dropped_frames++;
    } else {
        frame->size = size;
        frame->width = width;
        frame->height = height;
        frame->timestamp_ms = timestamp_ms;
        hub->latest_seq++;
        frame->seq = hub->latest_seq;
        slot->seq = hub->latest_seq;
        hub->latest_slot = slot;
        hub->stats.published_frames++;
    }

    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
}

void frame_hub_abort_publish(frame_hub_t* hub, video_frame_t* frame) {
    if (!hub || !frame) return;

    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);

    frame_slot_t* slot = NULL;
    for(int i=0; i<hub->config.pool_size; i++) {
        if(&hub->pool[i].frame == frame) {
            slot = &hub->pool[i];
            break;
        }
    }

    if (!slot || !slot->is_writing) {
        if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
        return;
    }

    slot->is_writing = 0;
    slot->active = 0;

    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
}

int frame_hub_register_client(frame_hub_t* hub) {
    if (!hub) return -1;
    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);

    int client_id = -1;
    for (int i = 0; i < hub->config.max_clients; i++) {
        if (!hub->clients[i].active) {
            hub->clients[i].active = 1;
            hub->clients[i].last_seen_seq = -1;
            hub->clients[i].held_slot = NULL;
            hub->stats.active_clients++;
            client_id = hub->clients[i].id;
            break;
        }
    }

    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
    return client_id;
}

void frame_hub_unregister_client(frame_hub_t* hub, int client_id) {
    if (!hub) return;
    if (client_id < 0 || client_id >= hub->config.max_clients) return;

    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);

    if (hub->clients[client_id].active) {
        if (hub->clients[client_id].held_slot) {
            hub->clients[client_id].held_slot->ref_count--;
            hub->clients[client_id].held_slot = NULL;
        }
        hub->clients[client_id].active = 0;
        hub->stats.active_clients--;
    }

    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
}

const video_frame_t* frame_hub_get_latest_frame(frame_hub_t* hub, int client_id) {
    if (!hub) return NULL;
    if (client_id < 0 || client_id >= hub->config.max_clients) return NULL;

    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);

    if (!hub->clients[client_id].active) {
        if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
        return NULL;
    }

    if (hub->clients[client_id].held_slot != NULL) {
        // Client already holding a frame
        if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
        return NULL;
    }

    if (!hub->latest_slot || hub->latest_seq <= hub->clients[client_id].last_seen_seq) {
        // No new frames
        if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
        return NULL;
    }

    frame_slot_t* slot = hub->latest_slot;
    slot->ref_count++;

    hub->clients[client_id].held_slot = slot;
    hub->clients[client_id].last_seen_seq = slot->seq;

    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
    return &slot->frame;
}

int frame_hub_release_frame(frame_hub_t* hub, int client_id, const video_frame_t* frame) {
    if (!hub || !frame) return -1;
    if (client_id < 0 || client_id >= hub->config.max_clients) return -1;

    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);

    if (!hub->clients[client_id].active || hub->clients[client_id].held_slot == NULL || &hub->clients[client_id].held_slot->frame != frame) {
        if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
        return -1;
    }

    hub->clients[client_id].held_slot->ref_count--;
    hub->clients[client_id].held_slot = NULL;

    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
    return 0;
}

void frame_hub_get_stats(frame_hub_t* hub, frame_hub_stats_t* stats) {
    if (!hub || !stats) return;

    if (hub->config.lock_cb) hub->config.lock_cb(hub->config.lock_ctx);
    *stats = hub->stats;
    if (hub->config.unlock_cb) hub->config.unlock_cb(hub->config.lock_ctx);
}
