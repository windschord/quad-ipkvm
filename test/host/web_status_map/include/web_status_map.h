#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "auth_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the appropriate HTTP status code based on auth_logic_login/setup return values and state
int web_status_map_login(int auth_ret, const auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms);
int web_status_map_setup(int auth_ret, const auth_logic_t *ctx);

#ifdef __cplusplus
}
#endif
