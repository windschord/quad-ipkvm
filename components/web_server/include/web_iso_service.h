#ifndef WEB_ISO_SERVICE_H
#define WEB_ISO_SERVICE_H

/*
 * Runtime wiring between esp_http_server (/api/v1/iso*) and iso_worker.
 * Binds iso_worker view callbacks to iso_api_snapshot_t, provides the PSRAM-backed
 * PUT chunk receive buffer and timeout hooks, and installs web_iso env & put hooks.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_iso_service_init(void);
void web_iso_service_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_ISO_SERVICE_H */
