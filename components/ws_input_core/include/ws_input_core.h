#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hid_logic.h"
#include "ws_clients.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HTTPD_WS_TYPE_BINARY of esp_http_server (the core is pure C and does not include IDF headers). */
#define WS_INPUT_FRAME_TYPE_BINARY 2
/* The largest input data frame (docs/WEB_API.md 6). */
#define WS_INPUT_MAX_FRAME_LEN 16

typedef void* ws_input_conn_t;

typedef int (*ws_input_io_send_cb)(void* user_data, ws_input_conn_t conn, const uint8_t* data, size_t len);
typedef void (*ws_input_io_request_close_cb)(void* user_data, ws_input_conn_t conn);
typedef bool (*ws_input_io_validate_session_cb)(void* user_data, const uint8_t token[32]);
typedef bool (*ws_input_usb_ready_cb)(void* user_data);

typedef struct {
    int max_clients;
    uint64_t session_check_interval_ms; // typically 5000

    ws_input_io_send_cb io_send;
    ws_input_io_request_close_cb io_request_close;
    ws_input_io_validate_session_cb io_validate_session;
    ws_input_usb_ready_cb usb_ready;

    void* user_data;
} ws_input_core_config_t;

typedef struct {
    ws_clients_t clients;
    ws_input_core_config_t config;
    hid_logic_t* hid; // pointer to the hid_logic instance

    ws_input_conn_t conns[WS_CLIENTS_MAX];
    uint8_t tokens[WS_CLIENTS_MAX][32];

    uint64_t last_session_check_ms;
    bool last_usb_ready;
} ws_input_core_t;

/**
 * All API functions must be called under a single lock by the user.
 * Callbacks must not block and must not re-enter the core.
 */

int ws_input_core_init(ws_input_core_t* core, const ws_input_core_config_t* config, hid_logic_t* hid);
void ws_input_core_destroy(ws_input_core_t* core);

int ws_input_core_on_connect(ws_input_core_t* core, ws_input_conn_t conn, int fd, const uint8_t token[32], uint64_t now_ms);
void ws_input_core_on_close(ws_input_core_t* core, int fd);

/** Decides, BEFORE the payload is read, whether an input data frame is acceptable: binary, final, 1..16 bytes. */
bool ws_input_core_check_frame(uint8_t type, bool final, size_t len);
/** A protocol violation (check_frame was false): if `fd` is the controller, release everything now; close the connection. */
void ws_input_core_on_violation(ws_input_core_t* core, int fd);
void ws_input_core_on_frame(ws_input_core_t* core, int fd, const uint8_t* payload, size_t len, uint64_t now_ms);

void ws_input_core_tick(ws_input_core_t* core, uint64_t now_ms);
void ws_input_core_on_usb_mounted(ws_input_core_t* core);

#ifdef __cplusplus
}
#endif
