#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "frame_hub.h"

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

#ifdef __cplusplus
}
#endif
