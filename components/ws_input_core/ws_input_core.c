#include "ws_input_core.h"
#include <string.h>

int ws_input_core_init(ws_input_core_t* core, const ws_input_core_config_t* config, hid_logic_t* hid) {
    if (!core || !config || !hid) return -1;
    if (config->max_clients < 1 || config->max_clients > WS_CLIENTS_MAX) return -1;
    if (!config->io_send || !config->io_request_close) return -1;

    memset(core, 0, sizeof(ws_input_core_t));
    core->config = *config;
    core->hid = hid;
    core->last_usb_ready = false;
    ws_clients_init(&core->clients, config->max_clients);
    return 0;
}

void ws_input_core_destroy(ws_input_core_t* core) {
    if (core) {
        // ws_clients doesn't have a deinit.
    }
}

/* Server -> client messages (docs/WEB_API.md 6): 0x80 GRANTED, 0x81 DENIED(reason), 0x82 USB_STATE(state).
 * io_send must not block; a failure (< 0) is returned so that the caller can retry what matters (USB_STATE). */
static int send_granted(ws_input_core_t* core, ws_input_conn_t conn) {
    uint8_t pkt[1] = { 0x80 };
    return core->config.io_send(core->config.user_data, conn, pkt, 1);
}

static int send_denied(ws_input_core_t* core, ws_input_conn_t conn, uint8_t reason) {
    uint8_t pkt[2] = { 0x81, reason };
    return core->config.io_send(core->config.user_data, conn, pkt, 2);
}

static int send_usb_state(ws_input_core_t* core, ws_input_conn_t conn, bool ready) {
    uint8_t pkt[2] = { 0x82, ready ? 1 : 0 };
    return core->config.io_send(core->config.user_data, conn, pkt, 2);
}

int ws_input_core_on_connect(ws_input_core_t* core, ws_input_conn_t conn, int fd, const uint8_t token[32], uint64_t now_ms) {
    if (!token) return -1;
    int idx = ws_clients_add(&core->clients, fd, 0, now_ms);
    if (idx < 0) {
        return -1; // Full, or the same fd twice
    }

    core->conns[idx] = conn;
    memcpy(core->tokens[idx], token, 32);

    if (hid_logic_acquire(core->hid, fd)) {
        if (send_granted(core, conn) < 0) {
            /* The viewer sends nothing before GRANTED: a lost GRANTED would leave a controller that cannot type.
             * Give the slot back and close; the viewer reconnects. */
            hid_logic_release_owner(core->hid, fd);
            core->config.io_request_close(core->config.user_data, conn);
        } else {
            bool ready = core->config.usb_ready ? core->config.usb_ready(core->config.user_data) : false;
            if (send_usb_state(core, conn, ready) < 0) {
                /* Make the next tick tell every connection again (a duplicate is harmless). */
                core->last_usb_ready = !ready;
            }
            /* On success core->last_usb_ready is not touched: it is what the OTHER connections were last told. */
        }
    } else {
        send_denied(core, conn, 1); // Reason 1: Someone else is controlling
        core->config.io_request_close(core->config.user_data, conn);
    }

    if (ws_clients_count(&core->clients) == 1) {
        core->last_session_check_ms = now_ms; /* the periodic session check starts with the first connection */
    }

    return 0;
}

void ws_input_core_on_close(ws_input_core_t* core, int fd) {
    int idx = ws_clients_find(&core->clients, fd);
    if (idx >= 0) {
        if (core->hid->owner_id == fd) {
            hid_logic_release_owner(core->hid, fd);
        }
        ws_clients_remove(&core->clients, idx);
    }
}

/* HTTPD_WS_TYPE_BINARY is 2 (ESP-IDF esp_http_server). Control frames (ping/pong/close) are handled by httpd and
 * never get here (handle_ws_control_frames = false). */
bool ws_input_core_check_frame(uint8_t type, bool final, size_t len) {
    if (type != WS_INPUT_FRAME_TYPE_BINARY) return false;
    if (!final) return false;
    if (len == 0 || len > WS_INPUT_MAX_FRAME_LEN) return false;
    return true;
}

void ws_input_core_on_violation(ws_input_core_t* core, int fd) {
    int idx = ws_clients_find(&core->clients, fd);
    if (idx < 0) return;
    /* The controller's keys and buttons are released NOW (not when the socket is finally closed). */
    if (core->hid->owner_id == fd) {
        hid_logic_release_owner(core->hid, fd);
    }
    core->config.io_request_close(core->config.user_data, core->conns[idx]);
}

void ws_input_core_on_frame(ws_input_core_t* core, int fd, const uint8_t* payload, size_t len, uint64_t now_ms) {
    int idx = ws_clients_find(&core->clients, fd);
    if (idx < 0 || !payload) return;

    if (core->hid->owner_id != fd) {
        // Not owner -> close
        core->config.io_request_close(core->config.user_data, core->conns[idx]);
        return;
    }

    hid_result_t res = hid_logic_on_message(core->hid, fd, payload, len, now_ms);
    if (res == HID_RESULT_ABUSE) {
        hid_logic_release_owner(core->hid, fd);
        core->config.io_request_close(core->config.user_data, core->conns[idx]);
    } else if (res == HID_RESULT_NOT_OWNER) {
        core->config.io_request_close(core->config.user_data, core->conns[idx]);
    }
}

void ws_input_core_tick(ws_input_core_t* core, uint64_t now_ms) {
    hid_logic_tick(core->hid, now_ms);

    /* USB_STATE: told to every connection when the state changes. If a send fails, last_usb_ready stays as it was,
     * so the next tick tries again (a client must not keep a stale "target connected"). */
    bool usb_ready = core->config.usb_ready ? core->config.usb_ready(core->config.user_data) : false;
    if (usb_ready != core->last_usb_ready) {
        bool all_sent = true;
        for (int i = 0; i < core->clients.max_clients; i++) {
            if (core->clients.clients[i].state == WS_CLIENT_ACTIVE) {
                if (send_usb_state(core, core->conns[i], usb_ready) < 0) all_sent = false;
            }
        }
        if (all_sent) core->last_usb_ready = usb_ready;
    }

    /* The session of every connection is checked again every session_check_interval_ms. */
    if (now_ms < core->last_session_check_ms) core->last_session_check_ms = now_ms; /* the clock went back */
    if (now_ms - core->last_session_check_ms >= core->config.session_check_interval_ms) {
        core->last_session_check_ms = now_ms;
        for (int i = 0; i < core->clients.max_clients; i++) {
            if (core->clients.clients[i].state != WS_CLIENT_ACTIVE) continue;
            if (core->config.io_validate_session && !core->config.io_validate_session(core->config.user_data, core->tokens[i])) {
                if (core->hid->owner_id == core->clients.clients[i].fd) {
                    hid_logic_release_owner(core->hid, core->clients.clients[i].fd);
                }
                core->config.io_request_close(core->config.user_data, core->conns[i]);
            }
        }
    }
}

void ws_input_core_on_usb_mounted(ws_input_core_t* core) {
    hid_logic_on_usb_mounted(core->hid);
}
