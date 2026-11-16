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

/* need_key を真にする (docs/VIDEO_TRANSPORT.md 9.6)。すでに真なら何もしない (待ちの起点も、要求も、やり直さない)。
 * 偽 -> 真のときだけ、キーフレームの要求を溜める (request_keyframe がなければ溜めない)。 */
static void mark_need_key(ws_video_core_t* core, int slot) {
    if (core->need_key[slot]) return;
    core->need_key[slot] = true;
    core->key_waiting[slot] = false;
    core->need_key_since_ms[slot] = 0;
    if (core->config.request_keyframe) {
        core->idr_request_pending = true;
    }
}

/* tick の最後: 溜まった要求を、間隔が経っていれば 1 回だけ呼ぶ。誰も need_key でなければ (配れた、閉じた、など)、要求は不要なので捨てる。 */
static void flush_keyframe_request(ws_video_core_t* core, uint64_t now_ms) {
    if (!core->idr_request_pending) return;

    bool any_need = false;
    for (int i = 0; i < core->config.max_clients; i++) {
        if (core->clients.clients[i].state == WS_CLIENT_ACTIVE && !core->close_requested[i] && core->need_key[i]) {
            any_need = true;
            break;
        }
    }
    if (!any_need || !core->config.request_keyframe) {
        core->idr_request_pending = false;
        return;
    }

    if (core->idr_requested) {
        // 時刻の巻き戻り: 最後に要求した時刻を now に合わせる (間隔が、いつまでも経たなくならない)
        if (now_ms < core->last_idr_request_ms) {
            core->last_idr_request_ms = now_ms;
        }
        if (core->config.idr_min_interval_ms > 0 &&
            now_ms - core->last_idr_request_ms < core->config.idr_min_interval_ms) {
            return; // まだ早い。溜めたまま、間隔が経った tick で 1 回だけ呼ぶ
        }
    }
    core->idr_request_pending = false;
    core->idr_requested = true;
    core->last_idr_request_ms = now_ms;
    core->config.request_keyframe(core->config.user_data);
}

static void do_request_close(ws_video_core_t* core, int slot) {
    if (!core->close_requested[slot]) {
        core->close_requested[slot] = true;
        core->config.io_request_close(core->config.user_data, core->conns[slot]);
    }
}

/* 映像の状態の通知 (docs/VIDEO_TRANSPORT.md 8 章): このクライアントに、いま状態を送るべきか。
 * tick で、そのクライアントが ACTIVE・close_requested でない・送信中でないときだけ呼ぶ。 */
static bool status_due(ws_video_core_t* core, int slot, const video_status_t* cur, uint64_t now_ms) {
    if (!core->status_sent[slot]) return true; // 接続直後は、間隔に関わらず必ず 1 回送る

    // 時刻の巻き戻り: 最後に送った時刻を now に合わせる (間隔が、いつまでも経たなくならない)
    if (now_ms < core->status_last_ms[slot]) {
        core->status_last_ms[slot] = now_ms;
    }

    const video_status_t* last = &core->status_last[slot];
    if (cur->state == last->state && cur->width == last->width && cur->height == last->height) return false;

    // 変化がある。最短の間隔が経つまでは送らない (途中の状態は送らず、経った後の最新の 1 回だけを送る)
    if (core->config.status_min_interval_ms > 0 &&
        now_ms - core->status_last_ms[slot] < core->config.status_min_interval_ms) {
        return false;
    }
    return true;
}

/* 状態のテキストフレームを送り始める (呼び出し側が、ws_clients_try_begin_send に成功した後)。
 * 成功したら、フレームと同じ仕組みで「送信中」になる (on_send_done まで続く)。失敗したらそのクライアントを閉じる。 */
static void status_begin_send(ws_video_core_t* core, int slot, const video_status_t* cur, uint64_t now_ms) {
    int n = video_status_format_json(cur, (char*)core->text_buf[slot], sizeof(core->text_buf[slot]));
    int res = (n > 0) ? core->config.io_begin_send_text(core->config.user_data, core->conns[slot], core->text_buf[slot], (size_t)n)
                      : -1; // n <= 0 は起こらない (JSON は 128 バイトに収まる)。防御: 閉じる
    if (res != 0) {
        ws_clients_end_send(&core->clients, slot);
        do_request_close(core, slot);
        return;
    }
    core->sending_text[slot] = true;
    core->text_len[slot] = (size_t)n;
    core->queued_ms[slot] = now_ms; // キューに入れた時刻 (フレームと同じ。send_started_ms は on_send_begin で設定する)
    core->send_begun[slot] = false;
    core->status_last[slot] = *cur;
    core->status_last_ms[slot] = now_ms;
    core->status_sent[slot] = true;
}

int ws_video_core_init(ws_video_core_t* core, const ws_video_core_config_t* config) {
    if (!core || !config) return -1;
    if (config->max_clients < 1 || config->max_clients > WS_CLIENTS_MAX) return -1;

    // Check all 7 callbacks
    if (!config->hub_register || !config->hub_unregister || !config->hub_get_latest || !config->hub_release ||
        !config->io_begin_send || !config->io_request_close || !config->io_validate_session) {
        return -1;
    }
    // 状態の通知: status_get があるなら、テキストを送る関数も要る
    if (config->status_get && !config->io_begin_send_text) {
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
    // 映像の状態の通知: 接続直後は、まだ 1 回も送っていない (tick で必ず 1 回送る)
    core->status_sent[slot] = false;
    core->status_last[slot].state = KVM_VIDEO_SYNC_UNKNOWN;
    core->status_last[slot].width = 0;
    core->status_last[slot].height = 0;
    core->status_last_ms[slot] = 0;
    core->sending_text[slot] = false;
    core->text_len[slot] = 0;
    // キーフレーム待ち: 接続したときは、必ず need_key (docs/VIDEO_TRANSPORT.md 9.6 (a))
    core->need_key[slot] = false;
    core->key_waiting[slot] = false;
    core->need_key_since_ms[slot] = 0;
    core->last_codec[slot] = (uint8_t)VIDEO_CODEC_MJPEG;
    mark_need_key(core, slot);

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

    // テキスト (映像の状態) の送信の完了。解放するフレームはない (current_frames[slot] は NULL)
    core->sending_text[slot] = false;
    core->text_len[slot] = 0;

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

    // この tick の映像の状態 (最初に必要になったときに 1 回だけ読む。全クライアントで同じ値を使う)
    video_status_t status_now = {KVM_VIDEO_SYNC_UNKNOWN, 0, 0}; // status_get が書かなくても UNKNOWN / 0 / 0
    bool status_read = false;

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

        // キーフレーム待ちが長い (H.264 の IDR が来ない): send_timeout とは別に閉じる (再接続させる)
        if (core->need_key[i] && core->key_waiting[i] && core->config.key_wait_timeout_ms > 0) {
            if (now_ms < core->need_key_since_ms[i]) {
                core->need_key_since_ms[i] = now_ms; // 時刻の巻き戻り: 起点を now に合わせる
            }
            if (now_ms - core->need_key_since_ms[i] >= core->config.key_wait_timeout_ms) {
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
                    if (core->sending_text[i]) size = core->text_len[i]; // テキストの送信 (期限の size はテキストの長さ)
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
            // 映像の状態の通知は、フレームより先。通知を始めたら、このクライアントのフレームは次の tick 以降
            // (テキストの送信が終わる = on_send_done まで、is_sending が続くため)
            if (core->config.status_get) {
                if (!status_read) {
                    core->config.status_get(core->config.user_data, &status_now);
                    status_read = true;
                }
                if (status_due(core, i, &status_now, now_ms)) {
                    status_begin_send(core, i, &status_now, now_ms);
                    continue;
                }
            }

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

            // キーフレーム待ち (docs/VIDEO_TRANSPORT.md 9.6)。H.264 以外のコーデックは、MJPEG と同じ扱い (配る)。
            const bool is_h264 = (frame->codec == (uint8_t)VIDEO_CODEC_H264);
            const bool is_key = (frame->flags & VIDEO_FRAME_FLAG_KEY) != 0;
            if (frame->seq != core->last_seqs[i] + 1) {
                mark_need_key(core, i); // (b) 飛ばした (遅いクライアント、リングの上書き、配らなかったフレーム)
            }
            if (is_h264 && core->last_codec[i] != (uint8_t)VIDEO_CODEC_H264) {
                mark_need_key(core, i); // (c) MJPEG (または初めて) から H.264 に変わった
            }
            if (core->need_key[i] && is_h264 && !is_key) {
                // P フレームは配らない (次の IDR まで待つ)。待ちの起点は、最初に配らなかった時刻
                if (!core->key_waiting[i]) {
                    core->key_waiting[i] = true;
                    core->need_key_since_ms[i] = now_ms;
                }
                core->config.hub_release(core->config.user_data, hub_client_id, frame);
                ws_clients_end_send(&core->clients, i);
                continue;
            }

            int send_res = core->config.io_begin_send(core->config.user_data, core->conns[i], frame->data, frame->size);
            if (send_res == 0) {
                // Success
                core->current_frames[i] = frame;
                core->last_seqs[i] = frame->seq; // Update last_seq AFTER successful begin_send
                core->last_codec[i] = (uint8_t)(is_h264 ? VIDEO_CODEC_H264 : VIDEO_CODEC_MJPEG);
                // ここまで来たフレームは、MJPEG、H.264 の IDR、または need_key でないときの H.264 のフレーム。
                // どれも、need_key を偽にしてよい (need_key のまま配るのは、MJPEG と IDR だけ)。
                // key_waiting は、need_key が偽の間は参照しない (次に need_key を真にするとき、mark_need_key が戻す)。
                core->need_key[i] = false;
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

    flush_keyframe_request(core, now_ms);
}
