#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of WebSocket clients. */
#define WS_CLIENTS_MAX 8

typedef enum {
    WS_CLIENT_FREE,
    WS_CLIENT_ACTIVE,
    WS_CLIENT_CLOSING
} ws_client_state_e;

typedef struct {
    ws_client_state_e state;
    int fd;
    int hub_client_id;
    bool is_sending;
    uint64_t last_session_check_ms;
} ws_client_t;

typedef struct {
    ws_client_t clients[WS_CLIENTS_MAX];
    int max_clients;
} ws_clients_t;

/**
 * @brief Initialize the WebSocket clients table.
 *
 * @param ctx The ws_clients context.
 * @param max_clients The maximum number of clients to allow (up to WS_CLIENTS_MAX).
 * @return 0 on success, -1 on failure (e.g., max_clients out of range).
 */
int ws_clients_init(ws_clients_t *ctx, int max_clients);

/**
 * @brief Add a new WebSocket client.
 *
 * @param ctx The ws_clients context.
 * @param fd The socket file descriptor.
 * @param hub_client_id The frame_hub client ID associated with this client.
 * @param now_ms Current time in milliseconds for session tracking.
 * @return The assigned slot index (0 to max_clients - 1), -1 if full, -2 if duplicate fd.
 */
int ws_clients_add(ws_clients_t *ctx, int fd, int hub_client_id, uint64_t now_ms);

/**
 * @brief Find a WebSocket client by its file descriptor.
 *
 * @param ctx The ws_clients context.
 * @param fd The socket file descriptor to search for.
 * @return The slot index, or -1 if not found.
 */
int ws_clients_find(const ws_clients_t *ctx, int fd);

/**
 * @brief Remove a WebSocket client, marking it as CLOSING.
 *
 * @param ctx The ws_clients context.
 * @param slot The slot index of the client to remove.
 * @return 0 on success, -1 if invalid slot or already free.
 */
int ws_clients_remove(ws_clients_t *ctx, int slot);

/**
 * @brief Try to begin sending data to a client.
 *
 * If the client is already sending, this will fail.
 *
 * @param ctx The ws_clients context.
 * @param slot The slot index.
 * @return true if successful (client marked as sending), false otherwise.
 */
bool ws_clients_try_begin_send(ws_clients_t *ctx, int slot);

/**
 * @brief End a send operation for a client.
 *
 * If the client was marked as CLOSING during the send, it will be freed.
 *
 * @param ctx The ws_clients context.
 * @param slot The slot index.
 * @return 0 on success, -1 on invalid slot.
 */
int ws_clients_end_send(ws_clients_t *ctx, int slot);

/**
 * @brief Check if a client is due for a session re-verification.
 *
 * @param ctx The ws_clients context.
 * @param slot The slot index.
 * @param now_ms Current time in milliseconds.
 * @param interval_ms The interval at which re-verification should happen.
 * @return true if due, false otherwise.
 */
bool ws_clients_session_due(const ws_clients_t *ctx, int slot, uint64_t now_ms, uint64_t interval_ms);

/**
 * @brief Update the last session check time for a client.
 *
 * @param ctx The ws_clients context.
 * @param slot The slot index.
 * @param now_ms Current time in milliseconds.
 * @return 0 on success, -1 on invalid slot.
 */
int ws_clients_session_checked(ws_clients_t *ctx, int slot, uint64_t now_ms);

/**
 * @brief Get the current number of active clients.
 *
 * @param ctx The ws_clients context.
 * @return The number of active (or closing) clients.
 */
int ws_clients_count(const ws_clients_t *ctx);

/**
 * @brief Callback function type for iterating over active clients.
 *
 * @param ctx The ws_clients context.
 * @param slot The slot index.
 * @param client Pointer to the client structure.
 * @param user_data User data passed to the iterator.
 */
typedef void (*ws_clients_iter_cb)(ws_clients_t *ctx, int slot, const ws_client_t *client, void *user_data);

/**
 * @brief Iterate over all active clients.
 *
 * @param ctx The ws_clients context.
 * @param cb The callback function to call for each active client.
 * @param user_data User data to pass to the callback.
 */
void ws_clients_for_each_active(ws_clients_t *ctx, ws_clients_iter_cb cb, void *user_data);


#ifdef __cplusplus
}
#endif
