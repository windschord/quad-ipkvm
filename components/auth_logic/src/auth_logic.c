#include "auth_logic.h"
#include <string.h>

// Helper to write uint32_t as little endian
static void write_u32_le(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

// Helper to read uint32_t as little endian
static uint32_t read_u32_le(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static bool is_weak_password(const char *password, size_t len) {
    if (len == 5 && memcmp(password, "admin", 5) == 0) return true;
    if (len == 8 && memcmp(password, "password", 8) == 0) return true;
    if (len == 10 && memcmp(password, "1234567890", 10) == 0) return true;
    return false;
}

int auth_logic_init(auth_logic_t *ctx, const auth_crypto_if_t *crypto, const auth_logic_config_t *config) {
    if (!ctx || !crypto || !config) return -1;
    if (!crypto->sha256 || !crypto->hmac_sha256 || !crypto->pbkdf2_hmac_sha256 || !crypto->random || !crypto->const_time_memcmp) return -1;

    if (config->pbkdf2_iterations < AUTH_LOGIC_MIN_PBKDF2_ITERATIONS) return -1;
    if (config->min_password_len < AUTH_LOGIC_MIN_PASSWORD_LEN) return -1;

    memset(ctx, 0, sizeof(auth_logic_t));
    ctx->crypto = crypto;
    ctx->config = *config;
    ctx->state = AUTH_STATE_NEEDS_SETUP;

    return 0;
}

int auth_logic_load_state(auth_logic_t *ctx, const uint8_t *serialized, size_t len) {
    if (!ctx || !serialized || len != AUTH_LOGIC_SERIALIZED_LEN) return -1;

    if (serialized[0] != AUTH_LOGIC_SERIALIZED_VERSION) return -1;

    size_t offset = 1;
    memcpy(ctx->salt, &serialized[offset], AUTH_LOGIC_SALT_LEN);
    offset += AUTH_LOGIC_SALT_LEN;

    ctx->iterations = read_u32_le(&serialized[offset]);
    offset += 4;

    memcpy(ctx->hash, &serialized[offset], AUTH_LOGIC_HASH_LEN);

    ctx->state = AUTH_STATE_READY;
    return 0;
}

int auth_logic_serialize_state(const auth_logic_t *ctx, uint8_t *serialized_out, size_t max_len, size_t *out_len) {
    if (!ctx || !serialized_out || !out_len || max_len < AUTH_LOGIC_SERIALIZED_LEN) return -1;
    if (ctx->state != AUTH_STATE_READY) return -1;

    size_t offset = 0;
    serialized_out[offset++] = AUTH_LOGIC_SERIALIZED_VERSION;

    memcpy(&serialized_out[offset], ctx->salt, AUTH_LOGIC_SALT_LEN);
    offset += AUTH_LOGIC_SALT_LEN;

    write_u32_le(&serialized_out[offset], ctx->iterations);
    offset += 4;

    memcpy(&serialized_out[offset], ctx->hash, AUTH_LOGIC_HASH_LEN);
    offset += AUTH_LOGIC_HASH_LEN;

    *out_len = offset;
    return 0;
}

int auth_logic_setup_password(auth_logic_t *ctx, const char *password, size_t pwd_len) {
    if (!ctx || !password) return -1;
    if (ctx->state != AUTH_STATE_NEEDS_SETUP) return -1; // Prevent double setup

    if (pwd_len < ctx->config.min_password_len || pwd_len > AUTH_LOGIC_MAX_PASSWORD_LEN) return -1;
    if (is_weak_password(password, pwd_len)) return -1;

    // Fail-closed random generation
    if (ctx->crypto->random(ctx->salt, AUTH_LOGIC_SALT_LEN) != 0) {
        memset(ctx->salt, 0, AUTH_LOGIC_SALT_LEN); // Clear partial data
        return -1;
    }

    ctx->iterations = ctx->config.pbkdf2_iterations;

    int ret = ctx->crypto->pbkdf2_hmac_sha256((const uint8_t*)password, pwd_len, ctx->salt, AUTH_LOGIC_SALT_LEN, ctx->iterations, AUTH_LOGIC_HASH_LEN, ctx->hash);
    if (ret != 0) {
        memset(ctx->salt, 0, AUTH_LOGIC_SALT_LEN);
        memset(ctx->hash, 0, AUTH_LOGIC_HASH_LEN);
        return -1;
    }

    ctx->state = AUTH_STATE_READY;
    return 0;
}

static bool check_rate_limit(auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms) {
    // Check global lockout first
    if (ctx->global_lockout_until_ms > 0 && now_ms < ctx->global_lockout_until_ms) {
        return false; // Locked out globally
    }

    // Find entry
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].source_ip == source_ip && (ctx->rate_limits[i].attempts > 0 || ctx->rate_limits[i].lockout_until_ms > 0)) {
            if (ctx->rate_limits[i].lockout_until_ms > 0 && now_ms < ctx->rate_limits[i].lockout_until_ms) {
                return false; // Locked out
            }
            return true; // Found, not locked out
        }
    }
    return true; // Not found, not locked out
}

static void record_rate_limit_failure(auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms) {
    int empty_idx = -1;
    int evict_idx = -1;

    // Try to find existing entry or empty slot
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].source_ip == source_ip && (ctx->rate_limits[i].attempts > 0 || ctx->rate_limits[i].lockout_until_ms > 0)) {
            ctx->rate_limits[i].attempts++;
            if (ctx->rate_limits[i].attempts >= ctx->config.rate_limit_max_attempts) {
                ctx->rate_limits[i].lockout_until_ms = now_ms + ctx->config.rate_limit_lockout_ms;
            }
            return;
        }
        if (empty_idx == -1 && ctx->rate_limits[i].attempts == 0 && ctx->rate_limits[i].lockout_until_ms == 0) {
            empty_idx = i;
        }
        // Evictable if expired lockout AND no attempts (reset), but we only clear attempts on success.
        // Actually, if lockout is expired, attempts is still > 0.
        // If lockout expired, we keep the entry but it's not strictly "empty".
        // To prevent eviction of active lockouts/failures, we only evict if truly empty.
    }

    // If we found an empty slot
    if (empty_idx != -1) {
        ctx->rate_limits[empty_idx].source_ip = source_ip;
        ctx->rate_limits[empty_idx].attempts = 1;
        ctx->rate_limits[empty_idx].lockout_until_ms = 0;
        return;
    }

    // Look for expired lockout entries to evict.
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].lockout_until_ms > 0 && now_ms >= ctx->rate_limits[i].lockout_until_ms) {
            evict_idx = i;
            break;
        }
    }

    if (evict_idx != -1) {
        ctx->rate_limits[evict_idx].source_ip = source_ip;
        ctx->rate_limits[evict_idx].attempts = 1;
        ctx->rate_limits[evict_idx].lockout_until_ms = 0;
        return;
    }

    // Table full, no evictable entries. Overflow to global counter.
    ctx->global_attempts++;
    if (ctx->global_attempts >= ctx->config.rate_limit_max_attempts) {
        ctx->global_lockout_until_ms = now_ms + ctx->config.rate_limit_lockout_ms;
    }
}

static void reset_rate_limit(auth_logic_t *ctx, uint32_t source_ip) {
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].source_ip == source_ip) {
            ctx->rate_limits[i].attempts = 0;
            ctx->rate_limits[i].lockout_until_ms = 0;
            // Keep source_ip or clear it, clearing allows reuse
            ctx->rate_limits[i].source_ip = 0;
            return;
        }
    }
    // Note: Success resets the global counter if we were overflowing,
    // but typically a successful login means the global attack isn't affecting this user.
    // We will clear global attempts on *any* successful login to recover from global lockout.
    ctx->global_attempts = 0;
    ctx->global_lockout_until_ms = 0;
}

int auth_logic_login(auth_logic_t *ctx, const char *password, size_t pwd_len, uint32_t source_ip, uint64_t now_ms, uint8_t *token_out) {
    if (!ctx || !password || !token_out) return -1;
    if (ctx->state != AUTH_STATE_READY) return -1;

    if (pwd_len > AUTH_LOGIC_MAX_PASSWORD_LEN) {
        // Pre-PBKDF2 lockout check even for invalid length
        if (!check_rate_limit(ctx, source_ip, now_ms)) return -1;
        record_rate_limit_failure(ctx, source_ip, now_ms);
        return -1;
    }

    // Pre-PBKDF2 lockout check
    if (!check_rate_limit(ctx, source_ip, now_ms)) {
        return -1;
    }

    uint8_t computed_hash[AUTH_LOGIC_HASH_LEN];
    int ret = ctx->crypto->pbkdf2_hmac_sha256((const uint8_t*)password, pwd_len, ctx->salt, AUTH_LOGIC_SALT_LEN, ctx->iterations, AUTH_LOGIC_HASH_LEN, computed_hash);
    if (ret != 0) return -1;

    if (ctx->crypto->const_time_memcmp(computed_hash, ctx->hash, AUTH_LOGIC_HASH_LEN) != 0) {
        record_rate_limit_failure(ctx, source_ip, now_ms);
        return -1;
    }

    reset_rate_limit(ctx, source_ip);

    // Generate token (fail-closed)
    if (ctx->crypto->random(token_out, AUTH_LOGIC_TOKEN_LEN) != 0) {
        return -1;
    }

    // Compute hash for session table
    uint8_t token_hash[AUTH_LOGIC_HASH_LEN];
    if (ctx->crypto->sha256(token_out, AUTH_LOGIC_TOKEN_LEN, token_hash) != 0) {
        return -1;
    }

    // Find empty slot or evict oldest
    int slot = -1;
    uint64_t oldest_time = UINT64_MAX;
    int oldest_idx = -1;

    for (int i = 0; i < AUTH_LOGIC_SESSION_MAX; i++) {
        if (!ctx->sessions[i].is_active) {
            slot = i;
            break;
        }
        // Also cleanup expired ones lazily
        if (now_ms - ctx->sessions[i].last_used_ms > ctx->config.session_idle_timeout_ms ||
            now_ms - ctx->sessions[i].created_ms > ctx->config.session_absolute_timeout_ms) {
            ctx->sessions[i].is_active = false;
            if (slot == -1) slot = i;
        }

        if (ctx->sessions[i].is_active && ctx->sessions[i].last_used_ms < oldest_time) {
            oldest_time = ctx->sessions[i].last_used_ms;
            oldest_idx = i;
        }
    }

    if (slot == -1) {
        slot = oldest_idx; // Evict oldest
    }

    ctx->sessions[slot].is_active = true;
    ctx->sessions[slot].created_ms = now_ms;
    ctx->sessions[slot].last_used_ms = now_ms;
    memcpy(ctx->sessions[slot].token_hash, token_hash, AUTH_LOGIC_HASH_LEN);

    return 0;
}

int auth_logic_validate_session(auth_logic_t *ctx, const uint8_t *token, size_t token_len, uint64_t now_ms) {
    if (!ctx || !token || token_len != AUTH_LOGIC_TOKEN_LEN) return -1;
    if (ctx->state != AUTH_STATE_READY) return -1;

    uint8_t token_hash[AUTH_LOGIC_HASH_LEN];
    if (ctx->crypto->sha256(token, token_len, token_hash) != 0) return -1;

    for (int i = 0; i < AUTH_LOGIC_SESSION_MAX; i++) {
        if (ctx->sessions[i].is_active) {
            // Check timeouts
            if (now_ms - ctx->sessions[i].last_used_ms > ctx->config.session_idle_timeout_ms ||
                now_ms - ctx->sessions[i].created_ms > ctx->config.session_absolute_timeout_ms) {
                ctx->sessions[i].is_active = false;
                continue;
            }

            if (ctx->crypto->const_time_memcmp(ctx->sessions[i].token_hash, token_hash, AUTH_LOGIC_HASH_LEN) == 0) {
                ctx->sessions[i].last_used_ms = now_ms;
                return 0; // Valid
            }
        }
    }

    return -1; // Not found or invalid
}

int auth_logic_logout(auth_logic_t *ctx, const uint8_t *token, size_t token_len) {
    if (!ctx || !token || token_len != AUTH_LOGIC_TOKEN_LEN) return -1;

    uint8_t token_hash[AUTH_LOGIC_HASH_LEN];
    if (ctx->crypto->sha256(token, token_len, token_hash) != 0) return -1;

    for (int i = 0; i < AUTH_LOGIC_SESSION_MAX; i++) {
        if (ctx->sessions[i].is_active && ctx->crypto->const_time_memcmp(ctx->sessions[i].token_hash, token_hash, AUTH_LOGIC_HASH_LEN) == 0) {
            ctx->sessions[i].is_active = false;
            memset(ctx->sessions[i].token_hash, 0, AUTH_LOGIC_HASH_LEN);
            return 0;
        }
    }

    return -1;
}

void auth_logic_logout_all(auth_logic_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < AUTH_LOGIC_SESSION_MAX; i++) {
        ctx->sessions[i].is_active = false;
        memset(ctx->sessions[i].token_hash, 0, AUTH_LOGIC_HASH_LEN);
    }
}
