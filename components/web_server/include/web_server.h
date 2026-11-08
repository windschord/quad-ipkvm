#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "frame_hub.h"
#include "hid_logic.h"

struct frame_hub_t;
esp_err_t web_server_start(struct frame_hub_t *hub);

/**
 * @brief Check if the web server requires initial setup.
 */
bool web_server_needs_setup(void);

/**
 * @brief Get the current number of connected WebSocket clients.
 */
int web_server_ws_client_count(void);

/**
 * @brief Set the HID sink (the USB side) for WS /ws. May be called before or after web_server_start.
 * Without a sink, ready() is never true (USB_STATE 0). The sink must not block and must not call back into hid_logic.
 */
void web_server_set_hid_sink(const hid_sink_t *sink);

/**
 * @brief Notify that the USB host is mounted or unmounted.
 */
void web_server_notify_usb_mounted(void);


#ifdef __cplusplus
}
#endif
