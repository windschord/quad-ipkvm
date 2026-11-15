#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "frame_hub.h"
#include "ws_clients.h"
#include "video_status.h"

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

/* 映像の状態の通知 (docs/VIDEO_TRANSPORT.md 8 章)。
 * status_get: 現在の映像の状態を out に書く。tick から (ロックの下で) 呼ばれる。ブロックしない。
 *   呼び出し側は video_status_get を渡す前提 (検査済みの値)。範囲外・不整合な値が来ても、core は落ちない
 *   (video_status_format_json が unknown に直す)。
 * io_begin_send_text: テキストフレーム (WebSocket のオペコード 0x1) を 1 つ送り始める。io_begin_send と同じ規則
 *   (成功 = 0 で、送信の完了を on_send_done で通知する。0 以外はそのクライアントを閉じる)。
 *   **data は、on_send_done が呼ばれるまで有効** (core が、クライアントごとのバッファに持つ)。 */
typedef void (*ws_video_status_get_cb)(void* user_data, video_status_t* out);
typedef int (*ws_video_io_begin_send_text_cb)(void* user_data, ws_video_conn_t conn, const uint8_t* data, size_t len);

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

    /* 映像の状態の通知 (docs/VIDEO_TRANSPORT.md 8 章)。status_get == NULL なら通知なし (従来と全く同じ挙動。
     * このとき io_begin_send_text と status_min_interval_ms は使わない)。status_get != NULL なら
     * io_begin_send_text も必須 (ws_video_core_init が -1 を返す)。 */
    ws_video_status_get_cb status_get;
    ws_video_io_begin_send_text_cb io_begin_send_text;
    uint64_t status_min_interval_ms; // 0 = 間隔なし。>0 = 同じクライアントへの通知の間隔の下限 (最後に送った時刻から)。
                                     // 接続直後の最初の 1 回には適用しない。TODO(hw-verify): 既定 200 は実機で確認する

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

    /* 映像の状態の通知 (クライアントごと)。status_get == NULL のときは使わない */
    bool status_sent[WS_CLIENTS_MAX];              // 接続してから、1 回でも送った (キューに入れた)
    video_status_t status_last[WS_CLIENTS_MAX];    // 最後に送った状態 (status_sent[i] のときだけ有効)
    uint64_t status_last_ms[WS_CLIENTS_MAX];       // 最後に送った時刻 (status_sent[i] のときだけ有効)
    bool sending_text[WS_CLIENTS_MAX];             // 送信中のものが、フレームでなくテキスト (is_sending のときだけ有効)
    size_t text_len[WS_CLIENTS_MAX];               // sending_text[i] のときの、テキストの長さ (期限の size)
    uint8_t text_buf[WS_CLIENTS_MAX][VIDEO_STATUS_JSON_MAX]; // テキストの本体。on_send_done まで有効
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
