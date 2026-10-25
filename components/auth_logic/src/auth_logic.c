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

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool is_weak_password(const char *password, size_t len) {
    if (len < AUTH_LOGIC_MIN_PASSWORD_LEN) return true;

    // Check if it contains username (case insensitive)
    for (size_t i = 0; i <= len - 5; i++) {
        if (to_lower(password[i]) == 'a' &&
            to_lower(password[i+1]) == 'd' &&
            to_lower(password[i+2]) == 'm' &&
            to_lower(password[i+3]) == 'i' &&
            to_lower(password[i+4]) == 'n') {
            return true;
        }
    }

    // Check for all identical characters
    bool all_same = true;
    for (size_t i = 1; i < len; i++) {
        if (password[i] != password[0]) {
            all_same = false;
            break;
        }
    }
    if (all_same) return true;

    // Check for sequential characters (up or down)
    bool seq_up = true;
    bool seq_down = true;
    for (size_t i = 1; i < len; i++) {
        if (password[i] != password[i-1] + 1) seq_up = false;
        if (password[i] != password[i-1] - 1) seq_down = false;
    }
    if (seq_up || seq_down) return true;

    // Common weak passwords (case-insensitive checking, >= 10 chars)
    const char* weak_list[] = {
        "password123", "password1234", "password12345", "password123456",
        "qwertyuiop", "1234567890", "0987654321", "1111111111",
        "administrator", "admin12345", "admin123456", "adminadmin",
        "letmein123", "sunshine123", "iloveyou12",
        "welcome123", "1q2w3e4r5t", "1qaz2wsx3edc",
        "passwordpassword", "changeme123", "default123"
    };

    char pwd_lower[AUTH_LOGIC_MAX_PASSWORD_LEN + 1];
    for (size_t i = 0; i < len; i++) {
        pwd_lower[i] = to_lower(password[i]);
    }
    pwd_lower[len] = '\0';

    for (size_t i = 0; i < sizeof(weak_list)/sizeof(weak_list[0]); i++) {
        if (strcmp(pwd_lower, weak_list[i]) == 0) return true;
    }

    return false;
}

// RED stub (TDD): replaced by the real implementation in the next commit.
auth_password_issue_e auth_logic_password_issue(const auth_logic_t *ctx, const char *password, size_t pwd_len) {
    (void)ctx;
    (void)password;
    (void)pwd_len;
    return AUTH_PASSWORD_OK;
}

void auth_logic_config_default(auth_logic_config_t *config) {
    if (!config) return;
    config->min_password_len = 10;

    // TODO(hw-verify): Benchmark on real ESP32-P4 hardware so a login attempt takes 0.5~1.0 seconds.
    config->pbkdf2_iterations = 10000;

    config->session_idle_timeout_ms = 60 * 60 * 1000; // 1 hour
    config->session_absolute_timeout_ms = 24 * 60 * 60 * 1000; // 24 hours

    config->rate_limit_window_ms = 5 * 60 * 1000; // 5 minutes window
    config->rate_limit_max_attempts = 5; // 5 failures per IP allowed before lockout

    // TODO(hw-verify): Adjust based on actual usage requirements.
    config->rate_limit_base_lockout_ms = 60 * 1000; // 1 minute
    config->rate_limit_max_lockout_ms = 60 * 60 * 1000; // 1 hour max lockout
    config->rate_limit_global_max_attempts = 25; // 5 times the IP limit
}

int auth_logic_init(auth_logic_t *ctx, const auth_crypto_if_t *crypto, const auth_logic_config_t *config) {
    if (!ctx || !crypto || !config) return -1;
    if (!crypto->sha256 || !crypto->hmac_sha256 || !crypto->pbkdf2_hmac_sha256 || !crypto->random || !crypto->const_time_memcmp) return -1;

    if (config->pbkdf2_iterations < AUTH_LOGIC_MIN_PBKDF2_ITERATIONS) return -1;
    if (config->min_password_len < AUTH_LOGIC_MIN_PASSWORD_LEN) return -1;

    // Rate limit validation
    if (config->rate_limit_window_ms == 0) return -1;
    if (config->rate_limit_max_attempts == 0) return -1;
    if (config->rate_limit_base_lockout_ms == 0) return -1;
    if (config->rate_limit_max_lockout_ms < config->rate_limit_base_lockout_ms) return -1;
    if (config->rate_limit_global_max_attempts < config->rate_limit_max_attempts) return -1;

    // Session validation
    if (config->session_idle_timeout_ms == 0) return -1;
    if (config->session_absolute_timeout_ms < config->session_idle_timeout_ms) return -1;

    memset(ctx, 0, sizeof(auth_logic_t));
    ctx->crypto = crypto;
    ctx->config = *config;
    ctx->state = AUTH_STATE_NEEDS_SETUP;

    return 0;
}

static void secure_zero(void *s, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)s;
    while (n--) *p++ = 0;
}

#define AUTH_LOGIC_MAX_PBKDF2_ITERATIONS 10000000

static bool is_all_zeros(const uint8_t *buf, size_t len) {
    uint8_t res = 0;
    for (size_t i = 0; i < len; i++) res |= buf[i];
    return res == 0;
}

int auth_logic_load_state(auth_logic_t *ctx, const uint8_t *serialized, size_t len) {
    if (!ctx || !serialized || len != AUTH_LOGIC_SERIALIZED_LEN) return -1;
    if (ctx->state != AUTH_STATE_NEEDS_SETUP) return -1; // Prevent overwriting READY state
    if (serialized[0] != AUTH_LOGIC_SERIALIZED_VERSION) return -1;

    // Verify integrity (SHA-256 of the payload)
    uint8_t computed_digest[32];
    if (ctx->crypto->sha256(&serialized[0], len - 8, computed_digest) != 0) return -1;
    if (ctx->crypto->const_time_memcmp(computed_digest, &serialized[len - 8], 8) != 0) return -1;

    size_t offset = 1;
    const uint8_t *salt = &serialized[offset];
    offset += AUTH_LOGIC_SALT_LEN;

    uint32_t iterations = read_u32_le(&serialized[offset]);
    offset += 4;

    const uint8_t *hash = &serialized[offset];

    // Validation
    if (iterations < AUTH_LOGIC_MIN_PBKDF2_ITERATIONS || iterations > AUTH_LOGIC_MAX_PBKDF2_ITERATIONS) return -1;
    if (is_all_zeros(salt, AUTH_LOGIC_SALT_LEN)) return -1;
    if (is_all_zeros(hash, AUTH_LOGIC_HASH_LEN)) return -1;

    // Atomic commit
    memcpy(ctx->salt, salt, AUTH_LOGIC_SALT_LEN);
    ctx->iterations = iterations;
    memcpy(ctx->hash, hash, AUTH_LOGIC_HASH_LEN);
    ctx->state = AUTH_STATE_READY;
    ctx->generation++;

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

    // Append integrity digest (first 8 bytes of SHA-256)
    uint8_t digest[32];
    if (ctx->crypto->sha256(serialized_out, offset, digest) != 0) return -1;
    memcpy(&serialized_out[offset], digest, 8);
    offset += 8;

    *out_len = offset;
    return 0;
}

int auth_logic_setup_password(auth_logic_t *ctx, const char *password, size_t pwd_len) {
    auth_setup_job_t job;
    if (auth_logic_setup_begin(ctx, password, pwd_len, &job) != 0) {
        return -1;
    }

    if (auth_logic_setup_compute(ctx->crypto, &job, password, pwd_len) != 0) {
        return -1;
    }

    return auth_logic_setup_finish(ctx, &job);
}

static bool check_rate_limit(auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms);
static void record_rate_limit_failure(auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms);
static void reset_rate_limit(auth_logic_t *ctx, uint32_t source_ip);



int auth_logic_change_password(auth_logic_t *ctx, const char *old_pwd, size_t old_len, const char *new_pwd, size_t new_len, uint32_t source_ip, uint64_t now_ms) {
    if (!ctx || !old_pwd || !new_pwd) return -1;
    if (ctx->state != AUTH_STATE_READY) return -1;


    if (old_len > AUTH_LOGIC_MAX_PASSWORD_LEN) {
        if (!check_rate_limit(ctx, source_ip, now_ms)) return -1;
        record_rate_limit_failure(ctx, source_ip, now_ms);
        return -1;
    }

    auth_login_job_t login_job;
    if (auth_logic_login_begin(ctx, source_ip, now_ms, &login_job) != 0) {
        return -1;
    }

    int match = auth_logic_login_compute(ctx->crypto, &login_job, old_pwd, old_len);

    if (match < 0) {
        return -1;
    }

    if (ctx->generation != login_job.generation) {
        return -1;
    }

    if (match != 1) {
        record_rate_limit_failure(ctx, source_ip, now_ms);
        return -1;
    }

    reset_rate_limit(ctx, source_ip);

    // Now validate and apply new password
    if (new_len < ctx->config.min_password_len || new_len > AUTH_LOGIC_MAX_PASSWORD_LEN) return -1;
    if (is_weak_password(new_pwd, new_len)) return -1;

    uint8_t new_salt[AUTH_LOGIC_SALT_LEN];
    uint8_t new_hash[AUTH_LOGIC_HASH_LEN];

    if (ctx->crypto->random(new_salt, AUTH_LOGIC_SALT_LEN) != 0) return -1;

    int ret = ctx->crypto->pbkdf2_hmac_sha256((const uint8_t*)new_pwd, new_len, new_salt, AUTH_LOGIC_SALT_LEN, ctx->config.pbkdf2_iterations, AUTH_LOGIC_HASH_LEN, new_hash);
    if (ret != 0) {
        secure_zero(new_hash, sizeof(new_hash));
        return -1;
    }

    // Atomic commit
    memcpy(ctx->salt, new_salt, AUTH_LOGIC_SALT_LEN);
    ctx->iterations = ctx->config.pbkdf2_iterations;
    memcpy(ctx->hash, new_hash, AUTH_LOGIC_HASH_LEN);
    ctx->generation++;

    secure_zero(new_hash, sizeof(new_hash));

    // Invalidate all sessions
    auth_logic_logout_all(ctx);

    return 0;
}

static uint64_t calculate_lockout_ms(const auth_logic_config_t *config, uint32_t lockout_count) {
    uint64_t ms = config->rate_limit_base_lockout_ms;
    if (lockout_count > 0) {
        // Exponential backoff: base * 2^(lockout_count - 1)
        int shifts = lockout_count - 1;
        if (shifts >= 30) shifts = 30; // Prevent overflow
        ms = ms * (1ULL << shifts);
    }
    if (ms > config->rate_limit_max_lockout_ms) {
        ms = config->rate_limit_max_lockout_ms;
    }
    return ms;
}

static bool is_rate_limit_active(const auth_rate_limit_entry_t *entry, uint64_t now_ms, uint64_t window_ms) {
    if (entry->lockout_until_ms > 0 && now_ms < entry->lockout_until_ms) return true; // Active lockout
    if (entry->attempts > 0 && now_ms - entry->first_attempt_ms <= window_ms) return true; // Active attempts within window
    return false;
}

static void check_and_decay_rate_limit(auth_rate_limit_entry_t *entry, uint64_t now_ms, uint64_t window_ms) {
    // If not locked out and window expired, reset everything
    if ((entry->lockout_until_ms == 0 || now_ms >= entry->lockout_until_ms) &&
        now_ms - entry->first_attempt_ms > window_ms) {
        entry->attempts = 0;
        entry->lockout_count = 0;
        entry->lockout_until_ms = 0;
        entry->first_attempt_ms = 0;
    } else if (entry->lockout_until_ms > 0 && now_ms >= entry->lockout_until_ms) {
        // Lockout expired, but maybe within window?
        // Actually, entering lockout means we want to clear attempts so we count fresh for the next lockout.
        // But if we fail immediately, we should increase lockout_count.
        // We will just clear attempts upon lockout expiration so the next fail triggers attempts=1.
        entry->attempts = 0;
        entry->lockout_until_ms = 0;
        entry->first_attempt_ms = now_ms; // Next failure starts a new window
    }
}

static bool check_rate_limit(auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms) {
    // Check global lockout
    if (ctx->global_lockout_until_ms > 0) {
        if (now_ms < ctx->global_lockout_until_ms) {
            return false; // Locked out globally
        } else {
            // Global lockout expired
            ctx->global_lockout_until_ms = 0;
            ctx->global_attempts = 0;
            ctx->global_first_attempt_ms = now_ms;
        }
    }

    // Find entry
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].source_ip == source_ip && is_rate_limit_active(&ctx->rate_limits[i], now_ms, ctx->config.rate_limit_window_ms)) {
            check_and_decay_rate_limit(&ctx->rate_limits[i], now_ms, ctx->config.rate_limit_window_ms);
            if (ctx->rate_limits[i].lockout_until_ms > 0 && now_ms < ctx->rate_limits[i].lockout_until_ms) {
                return false; // Locked out
            }
            return true; // Found, not locked out
        }
    }
    return true; // Not found, not locked out
}

static void record_rate_limit_failure(auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms) {
    int evict_idx = -1;

    // Try to find existing entry
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].source_ip == source_ip) {
            check_and_decay_rate_limit(&ctx->rate_limits[i], now_ms, ctx->config.rate_limit_window_ms);

            if (ctx->rate_limits[i].attempts == 0) {
                ctx->rate_limits[i].first_attempt_ms = now_ms;
            }

            ctx->rate_limits[i].attempts++;
            if (ctx->rate_limits[i].attempts >= ctx->config.rate_limit_max_attempts) {
                ctx->rate_limits[i].lockout_count++;
                ctx->rate_limits[i].lockout_until_ms = now_ms + calculate_lockout_ms(&ctx->config, ctx->rate_limits[i].lockout_count);
                ctx->rate_limits[i].attempts = 0; // Reset attempts for next cycle after lockout
            }
            return;
        }
    }

    // If not found, find empty or evictable slot
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        check_and_decay_rate_limit(&ctx->rate_limits[i], now_ms, ctx->config.rate_limit_window_ms);
        if (!is_rate_limit_active(&ctx->rate_limits[i], now_ms, ctx->config.rate_limit_window_ms)) {
            evict_idx = i;
            break;
        }
    }

    if (evict_idx != -1) {
        ctx->rate_limits[evict_idx].source_ip = source_ip;
        ctx->rate_limits[evict_idx].attempts = 1;
        ctx->rate_limits[evict_idx].lockout_count = 0;
        ctx->rate_limits[evict_idx].first_attempt_ms = now_ms;
        ctx->rate_limits[evict_idx].lockout_until_ms = 0;
        return;
    }

    // Table full, no evictable entries. Overflow to global counter.
    if (ctx->global_attempts == 0 || now_ms - ctx->global_first_attempt_ms > ctx->config.rate_limit_window_ms) {
        // Decay global lockout count if window passed
        if (ctx->global_attempts > 0 && now_ms - ctx->global_first_attempt_ms > ctx->config.rate_limit_window_ms) {
            ctx->global_lockout_count = 0;
        }
        ctx->global_attempts = 0;
        ctx->global_first_attempt_ms = now_ms;
    }

    ctx->global_attempts++;
    if (ctx->global_attempts >= ctx->config.rate_limit_global_max_attempts) {
        ctx->global_lockout_count++;
        ctx->global_lockout_until_ms = now_ms + calculate_lockout_ms(&ctx->config, ctx->global_lockout_count);
        ctx->global_attempts = 0;
    }
}

static void reset_rate_limit(auth_logic_t *ctx, uint32_t source_ip) {
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        if (ctx->rate_limits[i].source_ip == source_ip) {
            ctx->rate_limits[i].attempts = 0;
            ctx->rate_limits[i].lockout_until_ms = 0;
            ctx->rate_limits[i].lockout_count = 0;
            ctx->rate_limits[i].first_attempt_ms = 0;
            ctx->rate_limits[i].source_ip = 0; // clearing allows reuse
            return;
        }
    }
    // Note: Do NOT reset global limit here unconditionally to prevent bypassing global limit via one valid login.
}

int auth_logic_login(auth_logic_t *ctx, const char *password, size_t pwd_len, uint32_t source_ip, uint64_t now_ms, uint8_t *token_out) {
    if (!ctx || !password || !token_out) return -1;
    if (ctx->state != AUTH_STATE_READY) return -1;

    if (pwd_len > AUTH_LOGIC_MAX_PASSWORD_LEN) {
        if (!check_rate_limit(ctx, source_ip, now_ms)) return -1;
        record_rate_limit_failure(ctx, source_ip, now_ms);
        return -1;
    }

    auth_login_job_t job;
    if (auth_logic_login_begin(ctx, source_ip, now_ms, &job) != 0) {
        return -1;
    }

    int match = auth_logic_login_compute(ctx->crypto, &job, password, pwd_len);
    if (match < 0) {
        return -1;
    }

    return auth_logic_login_finish(ctx, &job, match, source_ip, now_ms, token_out);
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


int auth_logic_login_begin(auth_logic_t *ctx, uint32_t source_ip, uint64_t now_ms, auth_login_job_t *job) {
    if (!ctx || !job) return -1;
    if (ctx->state != AUTH_STATE_READY) return -1;

    if (!check_rate_limit(ctx, source_ip, now_ms)) {
        return -1;
    }

    memcpy(job->salt, ctx->salt, AUTH_LOGIC_SALT_LEN);
    job->iterations = ctx->iterations;
    memcpy(job->expected_hash, ctx->hash, AUTH_LOGIC_HASH_LEN);
    job->generation = ctx->generation;

    return 0;
}

int auth_logic_login_compute(const auth_crypto_if_t *crypto, const auth_login_job_t *job, const char *password, size_t pwd_len) {
    if (!crypto || !job || !password) return -1;

    uint8_t computed_hash[AUTH_LOGIC_HASH_LEN];
    int ret = crypto->pbkdf2_hmac_sha256((const uint8_t*)password, pwd_len, job->salt, AUTH_LOGIC_SALT_LEN, job->iterations, AUTH_LOGIC_HASH_LEN, computed_hash);
    if (ret != 0) {
        secure_zero(computed_hash, sizeof(computed_hash));
        return -1;
    }

    int match = (crypto->const_time_memcmp(computed_hash, job->expected_hash, AUTH_LOGIC_HASH_LEN) == 0) ? 1 : 0;
    secure_zero(computed_hash, sizeof(computed_hash));
    return match;
}

int auth_logic_login_finish(auth_logic_t *ctx, const auth_login_job_t *job, int match, uint32_t source_ip, uint64_t now_ms, uint8_t *token_out) {
    if (!ctx || !job || !token_out) return -1;

    if (ctx->state != AUTH_STATE_READY) {
        return -1;
    }

    if (ctx->generation != job->generation ||
        ctx->crypto->const_time_memcmp(ctx->salt, job->salt, AUTH_LOGIC_SALT_LEN) != 0 ||
        ctx->crypto->const_time_memcmp(ctx->hash, job->expected_hash, AUTH_LOGIC_HASH_LEN) != 0) {
        return -1; // State mismatch. Concurrent setup or change_password occurred. Do not count as rate limit failure.
    }

    if (match != 1) {
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
        secure_zero(token_out, AUTH_LOGIC_TOKEN_LEN); // Clear token if hash fails
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

    secure_zero(token_hash, AUTH_LOGIC_HASH_LEN);

    return 0;
}

int auth_logic_setup_begin(auth_logic_t *ctx, const char *password, size_t pwd_len, auth_setup_job_t *job) {
    if (!ctx || !password || !job) return -1;
    if (ctx->state != AUTH_STATE_NEEDS_SETUP) return -1; // Prevent double setup

    if (pwd_len < ctx->config.min_password_len || pwd_len > AUTH_LOGIC_MAX_PASSWORD_LEN) return -1;
    if (is_weak_password(password, pwd_len)) return -1;

    // Fail-closed random generation
    if (ctx->crypto->random(job->salt, AUTH_LOGIC_SALT_LEN) != 0) {
        memset(job->salt, 0, AUTH_LOGIC_SALT_LEN); // Clear partial data
        return -1;
    }

    job->iterations = ctx->config.pbkdf2_iterations;
    job->generation = ctx->generation;

    return 0;
}

int auth_logic_setup_compute(const auth_crypto_if_t *crypto, auth_setup_job_t *job, const char *password, size_t pwd_len) {
    if (!crypto || !job || !password) return -1;

    int ret = crypto->pbkdf2_hmac_sha256((const uint8_t*)password, pwd_len, job->salt, AUTH_LOGIC_SALT_LEN, job->iterations, AUTH_LOGIC_HASH_LEN, job->computed_hash);
    if (ret != 0) {
        memset(job->salt, 0, AUTH_LOGIC_SALT_LEN);
        memset(job->computed_hash, 0, AUTH_LOGIC_HASH_LEN);
        return -1;
    }
    return 0;
}

int auth_logic_setup_finish(auth_logic_t *ctx, const auth_setup_job_t *job) {
    if (!ctx || !job) return -1;

    if (ctx->state != AUTH_STATE_NEEDS_SETUP) return -1;
    if (ctx->generation != job->generation) return -1;
    // Protection against finishing a job that failed during compute
    bool all_zeros = true;
    for (int i = 0; i < AUTH_LOGIC_HASH_LEN; i++) {
        if (job->computed_hash[i] != 0) {
            all_zeros = false;
            break;
        }
    }
    if (all_zeros) return -1;


    memcpy(ctx->salt, job->salt, AUTH_LOGIC_SALT_LEN);
    ctx->iterations = job->iterations;
    memcpy(ctx->hash, job->computed_hash, AUTH_LOGIC_HASH_LEN);

    ctx->state = AUTH_STATE_READY;
    ctx->generation++;

    return 0;
}
