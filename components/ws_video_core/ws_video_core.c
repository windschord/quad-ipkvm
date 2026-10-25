#include "ws_video_core.h"
#include <string.h>

int ws_video_core_init(ws_video_core_t* core, const ws_video_core_config_t* config) {
    if (!core || !config) return -1;
    if (config->max_clients < 1 || config->max_clients > WS_CLIENTS_MAX) return -1;

    memset(core, 0, sizeof(*core));
    core->config = *config;

    if (ws_clients_init(&core->clients, config->max_clients) != 0) {
        return -1;
    }

    for (int i = 0; i < config->max_clients; i++) {
        core->last_seqs[i] = -1;
    }

    return 0;
}

void ws_video_core_destroy(ws_video_core_t* core) {
    if (!core) return;

    for (int i = 0; i < core->config.max_clients; i++) {
        if (core->clients.clients[i].state != WS_CLIENT_FREE) {
            int hub_client_id = core->clients.clients[i].hub_client_id;
            if (core->config.hub_unregister) {
                core->config.hub_unregister(core->config.user_data, hub_client_id);
            }
        }
    }

    // Explicitly zero out tokens and memory
    memset(core->tokens, 0, sizeof(core->tokens));
    memset(core, 0, sizeof(*core));
}

int ws_video_core_on_connect(ws_video_core_t* core, ws_video_conn_t conn, int fd, const uint8_t token[32], uint64_t now_ms) {
    if (!core) return -1;

    int hub_client_id = core->config.hub_register(core->config.user_data);
    if (hub_client_id < 0) {
        return -3; // Hub failed
    }

    int slot = ws_clients_add(&core->clients, fd, hub_client_id, now_ms);
    if (slot < 0) {
        core->config.hub_unregister(core->config.user_data, hub_client_id);
        return slot; // -1 for full, -2 for duplicate
    }

    core->conns[slot] = conn;
    memcpy(core->tokens[slot], token, 32);
    core->current_frames[slot] = NULL;
    core->last_seqs[slot] = -1;

    return slot;
}

void ws_video_core_on_close(ws_video_core_t* core, int fd) {
    if (!core) return;
    int slot = ws_clients_find(&core->clients, fd);
    if (slot < 0) return;

    bool was_sending = core->clients.clients[slot].is_sending;
    int hub_client_id = core->clients.clients[slot].hub_client_id;

    ws_clients_remove(&core->clients, slot);

    if (!was_sending) {
        // Safe to unregister immediately
        core->config.hub_unregister(core->config.user_data, hub_client_id);
        memset(core->tokens[slot], 0, 32);
    }
    // If it was sending, the state is now CLOSING. We will unregister in on_send_done.
}

void ws_video_core_on_send_done(ws_video_core_t* core, int fd, bool ok) {
    if (!core) return;
    int slot = ws_clients_find(&core->clients, fd);
    if (slot < 0) return;

    if (!core->clients.clients[slot].is_sending) {
        // Mismatched end_send, shouldn't happen if properly driven, but guard it.
        return;
    }

    int hub_client_id = core->clients.clients[slot].hub_client_id;
    const video_frame_t* frame = core->current_frames[slot];

    ws_clients_end_send(&core->clients, slot);

    if (frame) {
        core->config.hub_release(core->config.user_data, hub_client_id, frame);
        core->current_frames[slot] = NULL;
    }

    if (!ok && core->clients.clients[slot].state == WS_CLIENT_ACTIVE) {
        core->config.io_request_close(core->config.user_data, core->conns[slot]);
    }

    if (core->clients.clients[slot].state == WS_CLIENT_FREE) {
        // Was CLOSING, now FREE. Time to unregister.
        core->config.hub_unregister(core->config.user_data, hub_client_id);
        memset(core->tokens[slot], 0, 32);
    }
}

void ws_video_core_on_client_data(ws_video_core_t* core, int fd) {
    if (!core) return;
    int slot = ws_clients_find(&core->clients, fd);
    if (slot >= 0) {
        core->config.io_request_close(core->config.user_data, core->conns[slot]);
    }
}

void ws_video_core_tick(ws_video_core_t* core, uint64_t now_ms) {
    if (!core) return;

    for (int i = 0; i < core->config.max_clients; i++) {
        if (core->clients.clients[i].state != WS_CLIENT_ACTIVE) continue;

        // Session check
        if (ws_clients_session_due(&core->clients, i, now_ms, core->config.session_check_interval_ms)) {
            bool valid = core->config.io_validate_session(core->config.user_data, core->tokens[i]);
            ws_clients_session_checked(&core->clients, i, now_ms);

            if (!valid) {
                core->config.io_request_close(core->config.user_data, core->conns[i]);
                continue;
            }
        }

        // Try to send
        if (ws_clients_try_begin_send(&core->clients, i)) {
            int hub_client_id = core->clients.clients[i].hub_client_id;
            const video_frame_t* frame = core->config.hub_get_latest(core->config.user_data, hub_client_id);

            if (!frame) {
                ws_clients_end_send(&core->clients, i);
                continue;
            }

            if (frame->seq == core->last_seqs[i]) {
                core->config.hub_release(core->config.user_data, hub_client_id, frame);
                ws_clients_end_send(&core->clients, i);
                continue;
            }

            int send_res = core->config.io_begin_send(core->config.user_data, core->conns[i], frame->data, frame->size);
            if (send_res == 0) {
                // Success
                core->current_frames[i] = frame;
                core->last_seqs[i] = frame->seq; // Update last_seq AFTER successful begin_send
            } else {
                // Fail
                core->config.hub_release(core->config.user_data, hub_client_id, frame);
                ws_clients_end_send(&core->clients, i);
                core->config.io_request_close(core->config.user_data, core->conns[i]);
            }
        }
    }
}
