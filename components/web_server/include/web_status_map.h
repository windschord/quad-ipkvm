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

// Error code string (for the JSON body {"error":"<code>"}) for a password policy issue.
// Returns NULL for AUTH_PASSWORD_OK.
const char *web_status_map_password_issue_code(auth_password_issue_e issue);

// Error code for a failed setup request whose HTTP status is `status_code` (from web_status_map_setup()).
// Only a 400 has a code; every other status returns NULL. `password` is the candidate password taken from
// the request, or NULL when the request body could not be parsed (-> "bad_request"). A 400 for a password
// that passes the policy (any other setup failure) is also "bad_request".
// Pure function: the HTTP handlers only call this, so a later change of the handlers keeps the same codes.
const char *web_status_map_setup_error_code(int status_code, const auth_logic_t *ctx, const char *password, size_t password_len);

// Maps http_guard_action_e to HTTP status code
int web_status_map_action_code(http_guard_action_e action);
const char *web_status_map_action_message(http_guard_action_e action);

#ifdef __cplusplus
}
#endif
