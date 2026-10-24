#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "auth_logic.h"
#include "http_guard.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the appropriate HTTP status code based on auth_logic_login/setup return values and state
int web_status_map_login(int auth_ret, const auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms);
int web_status_map_setup(int auth_ret, const auth_logic_t *ctx);

// Maps http_guard_action_e to HTTP status code
int web_status_map_action_code(http_guard_action_e action);
const char *web_status_map_action_message(http_guard_action_e action);

#ifdef __cplusplus
}
#endif
