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
