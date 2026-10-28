#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "frame_hub.h"

esp_err_t web_server_start(frame_hub_t *hub);

#ifdef __cplusplus
}
#endif
