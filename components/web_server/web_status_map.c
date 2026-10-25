#include "web_status_map.h"

static bool is_locked_out(const auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms) {
    if (ctx->global_lockout_until_ms > 0 && now_ms < ctx->global_lockout_until_ms) return true;

    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].source_ip == source_ip) {
            if (ctx->rate_limits[i].lockout_until_ms > 0 && now_ms < ctx->rate_limits[i].lockout_until_ms) {
                return true;
            }
        }
    }
    return false;
}

int web_status_map_login(int auth_ret, const auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms) {
    if (auth_ret == 0) return 200;

    if (is_locked_out(ctx, source_ip, now_ms)) {
        return 429;
    }

    return 401; // Default to Unauthorized
}

int web_status_map_setup(int auth_ret, const auth_logic_t *ctx) {
    if (auth_ret == 0) return 200;

    if (ctx->state != AUTH_STATE_NEEDS_SETUP) {
        return 403; // Already setup
    }

    return 400; // Bad request (weak password, etc)
}

const char *web_status_map_password_issue_code(auth_password_issue_e issue) {
    switch (issue) {
        case AUTH_PASSWORD_TOO_SHORT: return "password_too_short";
        case AUTH_PASSWORD_TOO_LONG: return "password_too_long";
        case AUTH_PASSWORD_CONTAINS_ADMIN: return "password_contains_admin";
        case AUTH_PASSWORD_ALL_SAME: return "password_all_same";
        case AUTH_PASSWORD_SEQUENTIAL: return "password_sequential";
        case AUTH_PASSWORD_COMMON: return "password_common";
        case AUTH_PASSWORD_OK: break;
    }
    return NULL;
}

const char *web_status_map_setup_error_code(int status_code, const auth_logic_t *ctx, const char *password, size_t password_len) {
    if (status_code != 400) return NULL;
    if (!password) return "bad_request"; // the request body could not be parsed

    const char *code = web_status_map_password_issue_code(auth_logic_password_issue(ctx, password, password_len));
    return code ? code : "bad_request"; // rejected although the policy accepts it: some other setup failure
}

int web_status_map_action_code(http_guard_action_e action) {
    switch (action) {
        case HTTP_GUARD_ACTION_ALLOW: return 200;
        case HTTP_GUARD_ACTION_401: return 401;
        case HTTP_GUARD_ACTION_403_SETUP_REQUIRED: return 403;
        case HTTP_GUARD_ACTION_403_ORIGIN: return 403;
        case HTTP_GUARD_ACTION_403_HOST: return 403;
        case HTTP_GUARD_ACTION_405: return 405;
        case HTTP_GUARD_ACTION_409_ALREADY_SETUP: return 409;
        case HTTP_GUARD_ACTION_415: return 415;
        default: return 500;
    }
}

const char *web_status_map_action_message(http_guard_action_e action) {
    switch (action) {
        case HTTP_GUARD_ACTION_ALLOW: return NULL;
        case HTTP_GUARD_ACTION_401: return "unauthorized";
        case HTTP_GUARD_ACTION_403_SETUP_REQUIRED: return "setup_required";
        case HTTP_GUARD_ACTION_403_ORIGIN: return "forbidden_origin";
        case HTTP_GUARD_ACTION_403_HOST: return "forbidden_host";
        case HTTP_GUARD_ACTION_405: return "method_not_allowed";
        case HTTP_GUARD_ACTION_409_ALREADY_SETUP: return "already_setup";
        case HTTP_GUARD_ACTION_415: return "unsupported_media_type";
        default: return "internal_error";
    }
}
