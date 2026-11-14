#include "ws_video_core.h"
#include <string.h>

uint64_t ws_video_frame_deadline_ms(size_t size_bytes, uint32_t base_ms, uint32_t min_rate_bytes_per_s) {
    if (min_rate_bytes_per_s == 0) return UINT64_MAX;

    /* floor(size * 1000 / rate) を、size * 1000 を作らずに求める:
     * size = q * rate + r (r < rate <= 2^32) なら、q * 1000 + r * 1000 / rate。r * 1000 < 2^42 で溢れない。 */
    uint64_t size = (uint64_t)size_bytes;
    uint64_t rate = (uint64_t)min_rate_bytes_per_s;
    uint64_t q = size / rate;
    uint64_t r = size % rate;
    if (q > UINT64_MAX / 1000u) return UINT64_MAX;
    uint64_t extra = q * 1000u + (r * 1000u) / rate;
    if (extra < q * 1000u) return UINT64_MAX; // (加算の溢れ。r*1000/rate < 1000 なので、q*1000 が上限近いときだけ)
    if (extra > UINT64_MAX - (uint64_t)base_ms) return UINT64_MAX;
    return (uint64_t)base_ms + extra;
}

static void reset_send_times(ws_video_core_t* core, int slot) {
    core->send_started_ms[slot] = 0;
    core->queued_ms[slot] = 0;
    core->send_begun[slot] = false;
}

static void do_request_close(ws_video_core_t* core, int slot) {
    if (!core->close_requested[slot]) {
        core->close_requested[slot] = true;
        core->config.io_request_close(core->config.user_data, core->conns[slot]);
    }
}

int ws_video_core_init(ws_video_core_t* core, const ws_video_core_config_t* config) {
    if (!core || !config) return -1;
    if (config->max_clients < 1 || config->max_clients > WS_CLIENTS_MAX) return -1;

    // Check all 7 callbacks
    if (!config->hub_register || !config->hub_unregister || !config->hub_get_latest || !config->hub_release ||
        !config->io_begin_send || !config->io_request_close || !config->io_validate_session) {
        return -1;
    }

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
    reset_send_times(core, slot);
    core->close_requested[slot] = false;

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
        reset_send_times(core, slot);
        core->close_requested[slot] = false;
    }
    // If it was sending, the state is now CLOSING. We will unregister in on_send_done.
}

void ws_video_core_on_send_begin(ws_video_core_t* core, int fd, uint64_t now_ms) {
    if (!core) return;
    int slot = ws_clients_find(&core->clients, fd);
    if (slot < 0) return;
    if (!core->clients.clients[slot].is_sending) return;
    if (core->send_begun[slot]) return; // 2 回目は、開始時刻を延ばさない

    core->send_begun[slot] = true;
    core->send_started_ms[slot] = now_ms;
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
    // on_send_begin が呼ばれたかどうかに関わらず、この送信の時刻の記録を捨てる
    core->send_begun[slot] = false;
    core->queued_ms[slot] = 0;

    if (frame) {
        core->config.hub_release(core->config.user_data, hub_client_id, frame);
        core->current_frames[slot] = NULL;
    }

    if (!ok && core->clients.clients[slot].state == WS_CLIENT_ACTIVE) {
        do_request_close(core, slot);
    }

    if (core->clients.clients[slot].state == WS_CLIENT_FREE) {
        // Was CLOSING, now FREE. Time to unregister.
        core->config.hub_unregister(core->config.user_data, hub_client_id);
        memset(core->tokens[slot], 0, 32);
        reset_send_times(core, slot);
        core->close_requested[slot] = false;
    }
}

void ws_video_core_on_client_data(ws_video_core_t* core, int fd) {
    if (!core) return;
    int slot = ws_clients_find(&core->clients, fd);
    if (slot >= 0) {
        do_request_close(core, slot);
    }
}

void ws_video_core_tick(ws_video_core_t* core, uint64_t now_ms) {
    if (!core) return;

    for (int i = 0; i < core->config.max_clients; i++) {
        if (core->clients.clients[i].state != WS_CLIENT_ACTIVE) continue;
        if (core->close_requested[i]) continue;

        // Session check
        if (ws_clients_session_due(&core->clients, i, now_ms, core->config.session_check_interval_ms)) {
            bool valid = core->config.io_validate_session(core->config.user_data, core->tokens[i]);
            ws_clients_session_checked(&core->clients, i, now_ms);

            if (!valid) {
                do_request_close(core, i);
                continue;
            }
        }

        // A frame is in flight: queue wait / send stall / whole-frame deadline
        if (core->clients.clients[i].is_sending) {
            if (core->send_begun[i]) {
                // 実際に送り始めた後。時刻の巻き戻りは、開始時刻を now に合わせて、誤って閉じない。
                if (now_ms < core->send_started_ms[i]) {
                    core->send_started_ms[i] = now_ms;
                }
                uint64_t elapsed = now_ms - core->send_started_ms[i];

                if (core->config.send_timeout_ms > 0 && elapsed >= core->config.send_timeout_ms) {
                    do_request_close(core, i);
                    continue;
                }
                if (core->config.frame_min_rate_bytes_per_s > 0) {
                    const video_frame_t* f = core->current_frames[i];
                    size_t size = f ? f->size : 0;
                    uint64_t deadline = ws_video_frame_deadline_ms(size, core->config.frame_deadline_base_ms,
                                                                   core->config.frame_min_rate_bytes_per_s);
                    if (elapsed >= deadline) {
                        do_request_close(core, i);
                        continue;
                    }
                }
            } else {
                // キューに入れたが、まだ送り始めていない
                if (now_ms < core->queued_ms[i]) {
                    core->queued_ms[i] = now_ms;
                }
                uint64_t waited = now_ms - core->queued_ms[i];

                if (core->config.queue_timeout_ms > 0) {
                    if (waited >= core->config.queue_timeout_ms) {
                        do_request_close(core, i);
                        continue;
                    }
                } else if (core->config.send_timeout_ms > 0 && waited >= core->config.send_timeout_ms) {
                    // 互換 (queue_timeout_ms == 0): on_send_begin を使わない呼び出し側は、従来どおり、
                    // キューに入れた時刻から送信の停止を数える。
                    do_request_close(core, i);
                    continue;
                }
            }
            continue;
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
                core->queued_ms[i] = now_ms;     // キューに入れた時刻。send_started_ms は on_send_begin で設定する
                core->send_begun[i] = false;
            } else {
                // Fail
                core->config.hub_release(core->config.user_data, hub_client_id, frame);
                ws_clients_end_send(&core->clients, i);
                do_request_close(core, i);
            }
        }
    }
}
