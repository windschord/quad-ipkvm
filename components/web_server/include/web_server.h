#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "frame_hub.h"
#include "video_source.h"
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
 * @brief Tell the web server which video source feeds the frame hub (docs/VIDEO_TRANSPORT.md 9.7). May be called before or after
 * web_server_start(). NULL (the default) = MJPEG only.
 * The web server uses supported_codecs, set_codec() and request_keyframe() of the source, and only when
 * CONFIG_VIDEO_CODEC_H264_ALLOWED is set (otherwise nothing of the source is ever called). A NULL callback is never called.
 * The pointer must stay valid (a static video_source_t, as csi_video_source_get() / test_pattern_source_get() return).
 * set_codec() is called from the ws_video task without the web server lock; request_keyframe() with the lock held (must not block).
 */
void web_server_set_video_source(const video_source_t *source);

/**
 * @brief Notify that the USB host is mounted or unmounted.
 */
void web_server_notify_usb_mounted(void);

/**
 * @brief Wake the video WebSocket dispatcher immediately when a new frame is published.
 * Safe to call from any task once web_server_start() is initialized.
 */
void web_server_notify_video_frame(void);


#ifdef __cplusplus
}
#endif
