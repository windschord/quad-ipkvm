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
    /* Generation number of this connection: assigned by ws_clients_add() from a counter that only goes up, so two
     * connections that use the same fd one after the other never have the same value. Never 0 for a used slot. */
    uint32_t gen;
} ws_client_t;

typedef struct {
    ws_client_t clients[WS_CLIENTS_MAX];
    int max_clients;
    uint32_t last_gen; /* the generation given to the latest connection (0 = none yet). Never decreases, except by the wrap-around. */
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
 * @brief Get the generation number of the connection in a slot.
 *
 * The generation identifies one connection, not an fd: a later connection that gets the same fd (or the same slot)
 * has a different generation. A caller that wants to act on a connection later (for example, to close it from another
 * task) reads the generation now, and checks it with ws_clients_find_gen() when it acts.
 *
 * @param ctx The ws_clients context.
 * @param slot The slot index.
 * @return The generation (never 0), or 0 if the slot is invalid or FREE.
 */
uint32_t ws_clients_slot_gen(const ws_clients_t *ctx, int slot);

/**
 * @brief Find a WebSocket client by its file descriptor, but only if it is still the connection with this generation.
 *
 * Like ws_clients_find(), a slot in the CLOSING state is found (the table still holds that connection). The caller
 * decides what a CLOSING connection means for it.
 *
 * @param ctx The ws_clients context.
 * @param fd The socket file descriptor to search for.
 * @param gen The generation read earlier with ws_clients_slot_gen().
 * @return The slot index, or -1 if there is no such connection (the slot is FREE, the fd differs, or the connection was replaced: the generation differs).
 */
int ws_clients_find_gen(const ws_clients_t *ctx, int fd, uint32_t gen);

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
