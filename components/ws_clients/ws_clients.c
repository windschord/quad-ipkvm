#include "ws_clients.h"
#include <string.h>

int ws_clients_init(ws_clients_t *ctx, int max_clients) {
    if (!ctx) return -1;
    if (max_clients < 1 || max_clients > WS_CLIENTS_MAX) return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->max_clients = max_clients;
    for (int i = 0; i < max_clients; i++) {
        ctx->clients[i].state = WS_CLIENT_FREE;
        ctx->clients[i].fd = -1;
    }
    return 0;
}

int ws_clients_add(ws_clients_t *ctx, int fd, int hub_client_id, uint64_t now_ms) {
    if (!ctx) return -1;

    // Check for duplicate fd
    for (int i = 0; i < ctx->max_clients; i++) {
        if (ctx->clients[i].state != WS_CLIENT_FREE && ctx->clients[i].fd == fd) {
            return -2;
        }
    }

    // Find free slot
    for (int i = 0; i < ctx->max_clients; i++) {
        if (ctx->clients[i].state == WS_CLIENT_FREE) {
            ctx->clients[i].state = WS_CLIENT_ACTIVE;
            ctx->clients[i].fd = fd;
            ctx->clients[i].hub_client_id = hub_client_id;
            ctx->clients[i].is_sending = false;
            ctx->clients[i].last_session_check_ms = now_ms;
            return i;
        }
    }
    return -1; // Full
}

int ws_clients_find(const ws_clients_t *ctx, int fd) {
    if (!ctx) return -1;
    for (int i = 0; i < ctx->max_clients; i++) {
        if (ctx->clients[i].state != WS_CLIENT_FREE && ctx->clients[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

int ws_clients_remove(ws_clients_t *ctx, int slot) {
    if (!ctx || slot < 0 || slot >= ctx->max_clients) return -1;
    if (ctx->clients[slot].state == WS_CLIENT_FREE) return -1;

    if (ctx->clients[slot].is_sending) {
        ctx->clients[slot].state = WS_CLIENT_CLOSING;
    } else {
        ctx->clients[slot].state = WS_CLIENT_FREE;
        ctx->clients[slot].fd = -1;
    }
    return 0;
}

bool ws_clients_try_begin_send(ws_clients_t *ctx, int slot) {
    if (!ctx || slot < 0 || slot >= ctx->max_clients) return false;
    // Only active clients are checked. CLOSING and FREE return false.
    if (ctx->clients[slot].state != WS_CLIENT_ACTIVE) return false;
    if (ctx->clients[slot].is_sending) return false;

    ctx->clients[slot].is_sending = true;
    return true;
}

int ws_clients_end_send(ws_clients_t *ctx, int slot) {
    if (!ctx || slot < 0 || slot >= ctx->max_clients) return -1;
    if (ctx->clients[slot].state == WS_CLIENT_FREE) return -1; // Should not happen if correctly used

    ctx->clients[slot].is_sending = false;
    if (ctx->clients[slot].state == WS_CLIENT_CLOSING) {
        ctx->clients[slot].state = WS_CLIENT_FREE;
        ctx->clients[slot].fd = -1;
    }
    return 0;
}

bool ws_clients_session_due(const ws_clients_t *ctx, int slot, uint64_t now_ms, uint64_t interval_ms) {
    if (!ctx || slot < 0 || slot >= ctx->max_clients) return false;
    if (ctx->clients[slot].state != WS_CLIENT_ACTIVE) return false;

    if (now_ms < ctx->clients[slot].last_session_check_ms) {
        // Time went backwards, fail safe and re-verify
        return true;
    }

    return (now_ms - ctx->clients[slot].last_session_check_ms) >= interval_ms;
}

int ws_clients_session_checked(ws_clients_t *ctx, int slot, uint64_t now_ms) {
    if (!ctx || slot < 0 || slot >= ctx->max_clients) return -1;
    if (ctx->clients[slot].state != WS_CLIENT_ACTIVE) return -1;

    ctx->clients[slot].last_session_check_ms = now_ms;
    return 0;
}

int ws_clients_count(const ws_clients_t *ctx) {
    if (!ctx) return 0;
    int count = 0;
    for (int i = 0; i < ctx->max_clients; i++) {
        if (ctx->clients[i].state != WS_CLIENT_FREE) {
            count++;
        }
    }
    return count;
}

void ws_clients_for_each_active(ws_clients_t *ctx, ws_clients_iter_cb cb, void *user_data) {
    if (!ctx || !cb) return;
    for (int i = 0; i < ctx->max_clients; i++) {
        if (ctx->clients[i].state == WS_CLIENT_ACTIVE) {
            cb(ctx, i, &ctx->clients[i], user_data);
        }
    }
}
