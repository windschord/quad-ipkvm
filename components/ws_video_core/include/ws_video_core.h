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
} ws_video_core_t;

/**
 * All API callbacks are expected to be called under a single lock by the user.
 * Callbacks must not block and must not re-enter the core.
 */

int ws_video_core_init(ws_video_core_t* core, const ws_video_core_config_t* config);
void ws_video_core_destroy(ws_video_core_t* core);

int ws_video_core_on_connect(ws_video_core_t* core, ws_video_conn_t conn, int fd, const uint8_t token[32], uint64_t now_ms);
void ws_video_core_on_close(ws_video_core_t* core, int fd);
void ws_video_core_on_send_done(ws_video_core_t* core, int fd, bool ok);
void ws_video_core_on_client_data(ws_video_core_t* core, int fd);
void ws_video_core_tick(ws_video_core_t* core, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
