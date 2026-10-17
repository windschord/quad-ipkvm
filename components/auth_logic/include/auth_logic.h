#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Crypto Interface ---
// Pure C definition for cryptographic functions needed by auth_logic.
// This allows injection of mbedTLS for firmware and reference implementation for testing.
typedef struct {
    // SHA-256 hash. Returns 0 on success.
    int (*sha256)(const uint8_t *input, size_t ilen, uint8_t output[32]);
    // HMAC-SHA-256. Returns 0 on success.
    int (*hmac_sha256)(const uint8_t *key, size_t keylen, const uint8_t *input, size_t ilen, uint8_t output[32]);
    // PBKDF2-HMAC-SHA-256. Returns 0 on success.
    int (*pbkdf2_hmac_sha256)(const uint8_t *password, size_t plen, const uint8_t *salt, size_t slen, uint32_t iteration_count, uint32_t key_length, uint8_t *output);
    // Cryptographically secure random number generator. Returns 0 on success.
    int (*random)(uint8_t *output, size_t len);
    // Constant-time memory comparison. Returns 0 if equal, non-zero otherwise.
    int (*const_time_memcmp)(const void *a, const void *b, size_t len);
} auth_crypto_if_t;


// --- Constants ---
#define AUTH_LOGIC_MAX_PASSWORD_LEN 128
#define AUTH_LOGIC_MIN_PASSWORD_LEN 10
#define AUTH_LOGIC_MIN_PBKDF2_ITERATIONS 10000
#define AUTH_LOGIC_SALT_LEN 16
#define AUTH_LOGIC_HASH_LEN 32
#define AUTH_LOGIC_TOKEN_LEN 32
#define AUTH_LOGIC_SESSION_MAX 5
#define AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE 10

#define AUTH_LOGIC_SERIALIZED_VERSION 2
#define AUTH_LOGIC_SERIALIZED_LEN (1 + AUTH_LOGIC_SALT_LEN + 4 + AUTH_LOGIC_HASH_LEN + 8) // 1 + 16 + 4 + 32 + 8(hash prefix for integrity) = 61 bytes

// --- Config ---
typedef struct {
    uint32_t min_password_len; // Typically 10
    uint32_t pbkdf2_iterations; // Min 10000. TODO(hw-verify): Benchmark on real hardware

    // Session timeouts (in ms)
    uint64_t session_idle_timeout_ms;
    uint64_t session_absolute_timeout_ms;

    // Rate limit parameters
    uint64_t rate_limit_window_ms;       // Time window for counting attempts
    uint32_t rate_limit_max_attempts;    // Max attempts per IP in window before lockout
    uint64_t rate_limit_base_lockout_ms; // Base lockout time. TODO(hw-verify): Adjust based on usage requirements
    uint64_t rate_limit_max_lockout_ms;  // Max lockout time for exponential backoff
    uint32_t rate_limit_global_max_attempts; // Global max attempts in window
} auth_logic_config_t;


// --- State ---
typedef enum {
    AUTH_STATE_NEEDS_SETUP,
    AUTH_STATE_READY,
} auth_state_e;

typedef struct {
    bool is_active;
    uint64_t last_used_ms;
    uint64_t created_ms;
    uint8_t token_hash[AUTH_LOGIC_HASH_LEN];
} auth_session_t;

typedef struct {
    uint32_t source_ip; // Key (IP address as integer)
    uint32_t attempts;
    uint32_t lockout_count; // For exponential backoff
    uint64_t first_attempt_ms; // Start of the time window
    uint64_t lockout_until_ms;
} auth_rate_limit_entry_t;

typedef struct {
    auth_state_e state;

    // Password data
    uint8_t salt[AUTH_LOGIC_SALT_LEN];
    uint32_t iterations;
    uint8_t hash[AUTH_LOGIC_HASH_LEN];

    // Sessions
    auth_session_t sessions[AUTH_LOGIC_SESSION_MAX];

    // Rate Limiting
    auth_rate_limit_entry_t rate_limits[AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE];
    uint32_t global_attempts;
    uint64_t global_first_attempt_ms;
    uint32_t global_lockout_count;
    uint64_t global_lockout_until_ms;

    const auth_crypto_if_t *crypto;
    auth_logic_config_t config;
} auth_logic_t;


// --- API ---

// Initialize the logic module. Must be called first. Returns 0 on success.
int auth_logic_init(auth_logic_t *ctx, const auth_crypto_if_t *crypto, const auth_logic_config_t *config);

// Load serialized password state from NVS. If valid, changes state to READY. Returns 0 on success.
int auth_logic_load_state(auth_logic_t *ctx, const uint8_t *serialized, size_t len);

// Save current password state to serialized format. Returns 0 on success.
int auth_logic_serialize_state(const auth_logic_t *ctx, uint8_t *serialized_out, size_t max_len, size_t *out_len);

// Set initial password. Only works if in NEEDS_SETUP state. Transitions to READY on success.
int auth_logic_setup_password(auth_logic_t *ctx, const char *password, size_t pwd_len);

// Change password. Invalidates all sessions on success.
int auth_logic_change_password(auth_logic_t *ctx, const char *old_pwd, size_t old_len, const char *new_pwd, size_t new_len, uint32_t source_ip, uint64_t now_ms);

// Login. If successful, creates a new session and returns a 32-byte token.
// IMPORTANT: `token_out` must be 32 bytes.
int auth_logic_login(auth_logic_t *ctx, const char *password, size_t pwd_len, uint32_t source_ip, uint64_t now_ms, uint8_t *token_out);

// Validate a session token. Returns 0 if valid and updates idle timeout.
int auth_logic_validate_session(auth_logic_t *ctx, const uint8_t *token, size_t token_len, uint64_t now_ms);

// Logout (invalidate a specific session token).
int auth_logic_logout(auth_logic_t *ctx, const uint8_t *token, size_t token_len);

// Invalidate all sessions.
void auth_logic_logout_all(auth_logic_t *ctx);

#ifdef __cplusplus
}
#endif
