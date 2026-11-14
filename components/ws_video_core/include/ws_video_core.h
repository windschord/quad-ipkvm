#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "frame_hub.h"
#include "ws_clients.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* ws_video_conn_t;

typedef int (*ws_video_hub_register_cb)(void* user_data);
typedef void (*ws_video_hub_unregister_cb)(void* user_data, int hub_client_id);
typedef const video_frame_t* (*ws_video_hub_get_latest_cb)(void* user_data, int hub_client_id);
typedef void (*ws_video_hub_release_cb)(void* user_data, int hub_client_id, const video_frame_t* frame);

typedef int (*ws_video_io_begin_send_cb)(void* user_data, ws_video_conn_t conn, const uint8_t* data, size_t len);
typedef void (*ws_video_io_request_close_cb)(void* user_data, ws_video_conn_t conn);
typedef bool (*ws_video_io_validate_session_cb)(void* user_data, const uint8_t token[32]);

typedef struct {
    int max_clients;
    uint64_t session_check_interval_ms;
    uint64_t send_timeout_ms; // 0 = disabled; >0 = request close if an in-flight send stalls >= send_timeout_ms
                              // (send_started_ms = ws_video_core_on_send_begin() の時刻から数える。下の「互換」を参照)

    /* 送信の「キュー待ち」と「実際の送信」の分離 (docs/VIDEO_TRANSPORT.md 3 章)。
     * io_begin_send が成功した時刻 = キューに入れた時刻 (queued_ms)。作業関数が実際に送り始めるときに
     * ws_video_core_on_send_begin() を呼ぶと、そこから送信の時間 (send_started_ms) を数える。 */
    uint64_t queue_timeout_ms; // 0 = disabled; >0 = on_send_begin 前のキュー待ちが >= queue_timeout_ms ならそのクライアントを閉じる。
                               // 互換: 0 のとき (分離しない設定) は、on_send_begin が呼ばれていない送信の停止を、
                               // キューに入れた時刻から send_timeout_ms で数える (従来の動作)。>0 のとき、キュー待ちは send_timeout_ms に数えない。
                               // IDF 側は 10000 を渡す。

    /* 1 フレーム全体の期限: deadline = base + size * 1000 / min_rate (ws_video_frame_deadline_ms)。
     * on_send_begin の後で、(now - send_started) >= deadline(そのクライアントのフレームの大きさ) なら、そのクライアントだけを閉じる。 */
    uint32_t frame_deadline_base_ms;          // 期限の固定分
    uint32_t frame_min_rate_bytes_per_s;      // 0 = 期限なし

    ws_video_hub_register_cb hub_register;
    ws_video_hub_unregister_cb hub_unregister;
    ws_video_hub_get_latest_cb hub_get_latest;
    ws_video_hub_release_cb hub_release;

    ws_video_io_begin_send_cb io_begin_send;
    ws_video_io_request_close_cb io_request_close;
    ws_video_io_validate_session_cb io_validate_session;

    void* user_data;
} ws_video_core_config_t;

typedef struct {
    ws_clients_t clients;
    ws_video_core_config_t config;

    ws_video_conn_t conns[WS_CLIENTS_MAX];
    uint8_t tokens[WS_CLIENTS_MAX][32];
    const video_frame_t* current_frames[WS_CLIENTS_MAX];
    int64_t last_seqs[WS_CLIENTS_MAX];
    uint64_t send_started_ms[WS_CLIENTS_MAX]; // on_send_begin の時刻 (send_begun[i] のときだけ有効)
    uint64_t queued_ms[WS_CLIENTS_MAX];       // io_begin_send が成功した (キューに入れた) 時刻
    bool send_begun[WS_CLIENTS_MAX];          // on_send_begin が呼ばれた (実際に送り始めた)
    bool close_requested[WS_CLIENTS_MAX];
} ws_video_core_t;

/**
 * All API callbacks are expected to be called under a single lock by the user.
 * Callbacks must not block and must not re-enter the core.
 */

int ws_video_core_init(ws_video_core_t* core, const ws_video_core_config_t* config);
void ws_video_core_destroy(ws_video_core_t* core);

int ws_video_core_on_connect(ws_video_core_t* core, ws_video_conn_t conn, int fd, const uint8_t token[32], uint64_t now_ms);
void ws_video_core_on_close(ws_video_core_t* core, int fd);
/**
 * IDF 側の作業関数が、フレームを実際に送り始めるときに呼ぶ (io_begin_send でキューに入れた後)。
 * ここで send_started_ms を設定する。is_sending でない fd・未知の fd・既に呼ばれた後の 2 回目は無視する
 * (2 回目で開始時刻を延ばして、期限を逃れることを許さない)。
 * on_send_begin なしで on_send_done が来てもよい (送信に失敗した、など)。
 */
void ws_video_core_on_send_begin(ws_video_core_t* core, int fd, uint64_t now_ms);
void ws_video_core_on_send_done(ws_video_core_t* core, int fd, bool ok);
void ws_video_core_on_client_data(ws_video_core_t* core, int fd);
void ws_video_core_tick(ws_video_core_t* core, uint64_t now_ms);

/**
 * 1 フレーム全体の送信の期限 (ms) = base_ms + size_bytes * 1000 / min_rate_bytes_per_s (切り捨て)。
 * min_rate_bytes_per_s == 0 は「期限なし」で UINT64_MAX を返す。結果が 64 ビットを超えるときは UINT64_MAX に飽和する
 * (途中の size_bytes * 1000 で溢れさせない)。
 */
uint64_t ws_video_frame_deadline_ms(size_t size_bytes, uint32_t base_ms, uint32_t min_rate_bytes_per_s);

#ifdef __cplusplus
}
#endif
