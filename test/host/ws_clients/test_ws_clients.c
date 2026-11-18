#include "unity.h"
#include "ws_clients.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_ws_clients_init(void) {
    ws_clients_t ctx;

    // Invalid args
    TEST_ASSERT_EQUAL(-1, ws_clients_init(NULL, 4));
    TEST_ASSERT_EQUAL(-1, ws_clients_init(&ctx, 0));
    TEST_ASSERT_EQUAL(-1, ws_clients_init(&ctx, WS_CLIENTS_MAX + 1));

    // Valid args
    TEST_ASSERT_EQUAL(0, ws_clients_init(&ctx, 4));
    TEST_ASSERT_EQUAL(4, ctx.max_clients);
    for(int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(WS_CLIENT_FREE, ctx.clients[i].state);
    }
}

void test_ws_clients_add_remove(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 2);

    // Add first
    int slot1 = ws_clients_add(&ctx, 10, 100, 1000);
    TEST_ASSERT_EQUAL(0, slot1);
    TEST_ASSERT_EQUAL(1, ws_clients_count(&ctx));

    // Duplicate fd
    TEST_ASSERT_EQUAL(-2, ws_clients_add(&ctx, 10, 101, 1000));

    // Add second
    int slot2 = ws_clients_add(&ctx, 11, 101, 1000);
    TEST_ASSERT_EQUAL(1, slot2);
    TEST_ASSERT_EQUAL(2, ws_clients_count(&ctx));

    // Full
    TEST_ASSERT_EQUAL(-1, ws_clients_add(&ctx, 12, 102, 1000));

    // Find
    TEST_ASSERT_EQUAL(0, ws_clients_find(&ctx, 10));
    TEST_ASSERT_EQUAL(1, ws_clients_find(&ctx, 11));
    TEST_ASSERT_EQUAL(-1, ws_clients_find(&ctx, 12));

    // Remove slot 0
    TEST_ASSERT_EQUAL(0, ws_clients_remove(&ctx, 0));
    TEST_ASSERT_EQUAL(WS_CLIENT_FREE, ctx.clients[0].state);
    TEST_ASSERT_EQUAL(1, ws_clients_count(&ctx));
    TEST_ASSERT_EQUAL(-1, ws_clients_find(&ctx, 10)); // Shouldn't be found

    // Invalid remove
    TEST_ASSERT_EQUAL(-1, ws_clients_remove(&ctx, -1));
    TEST_ASSERT_EQUAL(-1, ws_clients_remove(&ctx, 2)); // Out of bounds
    TEST_ASSERT_EQUAL(-1, ws_clients_remove(&ctx, 0)); // Already free

    // Free slot check for find
    TEST_ASSERT_EQUAL(-1, ws_clients_find(&ctx, -1));
}

void test_ws_clients_send_closing(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 2);

    ws_clients_add(&ctx, 10, 100, 1000);

    // Begin send
    TEST_ASSERT_TRUE(ws_clients_try_begin_send(&ctx, 0));
    TEST_ASSERT_TRUE(ctx.clients[0].is_sending);

    // Already sending
    TEST_ASSERT_FALSE(ws_clients_try_begin_send(&ctx, 0));

    // Remove while sending -> CLOSING
    TEST_ASSERT_EQUAL(0, ws_clients_remove(&ctx, 0));
    TEST_ASSERT_EQUAL(WS_CLIENT_CLOSING, ctx.clients[0].state);

    // Session due should be false for CLOSING
    TEST_ASSERT_FALSE(ws_clients_session_due(&ctx, 0, 5000, 1000));

    // End send -> FREE
    TEST_ASSERT_EQUAL(0, ws_clients_end_send(&ctx, 0));
    TEST_ASSERT_EQUAL(WS_CLIENT_FREE, ctx.clients[0].state);

    // Session due should be false for FREE
    TEST_ASSERT_FALSE(ws_clients_session_due(&ctx, 0, 5000, 1000));

    // Invalid end send
    TEST_ASSERT_EQUAL(-1, ws_clients_end_send(&ctx, 0)); // Already free
}

void test_ws_clients_session(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 2);

    ws_clients_add(&ctx, 10, 100, 1000);

    // Exact interval
    TEST_ASSERT_TRUE(ws_clients_session_due(&ctx, 0, 2000, 1000));
    // Interval - 1
    TEST_ASSERT_FALSE(ws_clients_session_due(&ctx, 0, 1999, 1000));

    // Checked
    TEST_ASSERT_EQUAL(0, ws_clients_session_checked(&ctx, 0, 2000));
    TEST_ASSERT_FALSE(ws_clients_session_due(&ctx, 0, 2500, 1000));

    // Time went backwards -> fail safe true
    ctx.clients[0].last_session_check_ms = 3000;
    TEST_ASSERT_TRUE(ws_clients_session_due(&ctx, 0, 2500, 1000));

    // Giant interval, giant time, no wrap around panic
    ctx.clients[0].last_session_check_ms = UINT64_MAX - 5;
    TEST_ASSERT_FALSE(ws_clients_session_due(&ctx, 0, UINT64_MAX, UINT64_MAX));

    // now == last and interval == 0
    ctx.clients[0].last_session_check_ms = 5000;
    TEST_ASSERT_TRUE(ws_clients_session_due(&ctx, 0, 5000, 0));

    // now == last and interval > 0
    TEST_ASSERT_FALSE(ws_clients_session_due(&ctx, 0, 5000, 1000));
}

static int iter_count = 0;
static void iter_cb(ws_clients_t *ctx, int slot, const ws_client_t *client, void *user_data) {
    (void)ctx; (void)slot;
    iter_count++;
    *(int*)user_data += client->fd;
}

void test_ws_clients_iter(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 4);

    ws_clients_add(&ctx, 10, 100, 1000);
    ws_clients_add(&ctx, 11, 101, 1000);
    ws_clients_add(&ctx, 12, 102, 1000);

    // Mark one as closing to ensure it's not iterated
    ws_clients_try_begin_send(&ctx, 1);
    ws_clients_remove(&ctx, 1);

    int sum_fd = 0;
    iter_count = 0;
    ws_clients_for_each_active(&ctx, iter_cb, &sum_fd);

    TEST_ASSERT_EQUAL(2, iter_count);
    TEST_ASSERT_EQUAL(10 + 12, sum_fd);
}

// Simple reference model for fuzzing
typedef struct {
    int active;
    int closing;
    int fds[WS_CLIENTS_MAX];
    bool sending[WS_CLIENTS_MAX];
} ref_model_t;

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void test_ws_clients_fuzz(void) {
    for (int seed = 1; seed <= 16; seed++) {
        uint32_t rng = seed;
        ws_clients_t ctx;
        TEST_ASSERT_EQUAL(0, ws_clients_init(&ctx, WS_CLIENTS_MAX));

        ref_model_t ref = {0};
        for(int i = 0; i < WS_CLIENTS_MAX; i++) ref.fds[i] = -1;

        for (int step = 0; step < 1000; step++) {
            int op = xorshift32(&rng) % 6;
            int fd = (xorshift32(&rng) % 20) + 1; // 1 to 20
            int slot = xorshift32(&rng) % WS_CLIENTS_MAX;

            switch (op) {
                case 0: { // Add
                    int res = ws_clients_add(&ctx, fd, 100, 0);
                    // Check duplicate in ref
                    bool dup = false;
                    for (int i=0; i<WS_CLIENTS_MAX; i++) {
                        if (ref.fds[i] == fd) dup = true;
                    }
                    if (dup) {
                        TEST_ASSERT_EQUAL(-2, res);
                    } else if (ref.active + ref.closing >= WS_CLIENTS_MAX) {
                        TEST_ASSERT_EQUAL(-1, res);
                    } else {
                        TEST_ASSERT_GREATER_OR_EQUAL(0, res);
                        ref.fds[res] = fd;
                        ref.active++;
                    }
                    break;
                }
                case 1: { // Remove
                    int res = ws_clients_remove(&ctx, slot);
                    if (ref.fds[slot] == -1) {
                        TEST_ASSERT_EQUAL(-1, res);
                    } else {
                        TEST_ASSERT_EQUAL(0, res);
                        if (ref.sending[slot]) {
                            ref.active--;
                            ref.closing++;
                            // fd remains in model to represent closing state not being reused
                        } else {
                            ref.active--;
                            ref.fds[slot] = -1;
                        }
                    }
                    break;
                }
                case 2: { // Begin send
                    bool res = ws_clients_try_begin_send(&ctx, slot);
                    if (ref.fds[slot] == -1 || ref.sending[slot] || ctx.clients[slot].state == WS_CLIENT_CLOSING) {
                        TEST_ASSERT_FALSE(res);
                    } else {
                        TEST_ASSERT_TRUE(res);
                        ref.sending[slot] = true;
                    }
                    break;
                }
                case 3: { // End send
                    int res = ws_clients_end_send(&ctx, slot);
                    if (ref.fds[slot] == -1) {
                        TEST_ASSERT_EQUAL(-1, res);
                    } else {
                        TEST_ASSERT_EQUAL(0, res);
                        ref.sending[slot] = false;
                        if (ctx.clients[slot].state == WS_CLIENT_FREE) { // Was closing
                            ref.closing--;
                            ref.fds[slot] = -1;
                        }
                    }
                    break;
                }
                case 4: { // Find
                    int res = ws_clients_find(&ctx, fd);
                    int expected = -1;
                    for (int i=0; i<WS_CLIENTS_MAX; i++) {
                        if (ref.fds[i] == fd) { expected = i; break; }
                    }
                    TEST_ASSERT_EQUAL(expected, res);
                    break;
                }
                case 5: { // Count
                    TEST_ASSERT_EQUAL(ref.active + ref.closing, ws_clients_count(&ctx));
                    break;
                }
            }

            // Invariants check
            int actual_count = 0;
            for (int i = 0; i < WS_CLIENTS_MAX; i++) {
                if (ctx.clients[i].state != WS_CLIENT_FREE) {
                    actual_count++;
                    TEST_ASSERT_EQUAL(ref.fds[i], ctx.clients[i].fd);
                    if(ctx.clients[i].state == WS_CLIENT_CLOSING) {
                        TEST_ASSERT_TRUE(ctx.clients[i].is_sending); // Should only be CLOSING if sending
                    }
                } else {
                    TEST_ASSERT_EQUAL(-1, ref.fds[i]);
                }
            }
            TEST_ASSERT_EQUAL(actual_count, ws_clients_count(&ctx));
        }
    }
}

/* ---- Generation numbers (a close request must not hit a later connection that reuses the fd) ---- */

void test_ws_clients_gen_basic(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 4);

    // Nothing there: invalid, FREE slot -> 0
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, 0));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, -1));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, 4));    // >= max_clients
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, WS_CLIENTS_MAX));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(NULL, 0));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(NULL, 10, 1));

    int a = ws_clients_add(&ctx, 10, 0, 0);
    int b = ws_clients_add(&ctx, 11, 0, 0);
    int c = ws_clients_add(&ctx, 12, 0, 0);
    uint32_t ga = ws_clients_slot_gen(&ctx, a);
    uint32_t gb = ws_clients_slot_gen(&ctx, b);
    uint32_t gc = ws_clients_slot_gen(&ctx, c);

    // Never 0 for a used slot; different for every connection
    TEST_ASSERT_NOT_EQUAL(0, ga);
    TEST_ASSERT_NOT_EQUAL(0, gb);
    TEST_ASSERT_NOT_EQUAL(0, gc);
    TEST_ASSERT_NOT_EQUAL(ga, gb);
    TEST_ASSERT_NOT_EQUAL(gb, gc);
    TEST_ASSERT_NOT_EQUAL(ga, gc);

    // find_gen: right fd and generation -> the slot
    TEST_ASSERT_EQUAL(a, ws_clients_find_gen(&ctx, 10, ga));
    TEST_ASSERT_EQUAL(b, ws_clients_find_gen(&ctx, 11, gb));
    TEST_ASSERT_EQUAL(c, ws_clients_find_gen(&ctx, 12, gc));
    // right fd, other connection's generation -> not found
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 10, gb));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 10, ga + 1000));
    // right generation, wrong fd -> not found
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 11, ga));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 99, ga));
    // generation 0 never matches a used slot
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 10, 0));
    // The 4th slot (never used) is FREE -> 0
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, 3));

    // A failed add (duplicate fd, full) takes no generation: the counter is only used by a connection
    uint32_t before = ctx.last_gen;
    TEST_ASSERT_EQUAL(-2, ws_clients_add(&ctx, 10, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(before, ctx.last_gen);
    TEST_ASSERT_EQUAL(3, ws_clients_add(&ctx, 13, 0, 0));
    TEST_ASSERT_EQUAL(-1, ws_clients_add(&ctx, 14, 0, 0)); // full
    TEST_ASSERT_EQUAL_UINT32(before + 1, ctx.last_gen);
}

/* A table with every slot used (max_clients == WS_CLIENTS_MAX): the first and the last slot, and the bounds around them.
 * (The struct field after clients[] is max_clients (8), then last_gen: a read of clients[WS_CLIENTS_MAX] would see
 * state = 8 (not FREE) and fd = last_gen, so ask for fd == last_gen.) */
void test_ws_clients_gen_full_table_bounds(void) {
    ws_clients_t ctx;
    TEST_ASSERT_EQUAL(0, ws_clients_init(&ctx, WS_CLIENTS_MAX));
    for (int i = 0; i < WS_CLIENTS_MAX; i++) {
        TEST_ASSERT_EQUAL(i, ws_clients_add(&ctx, 101 + i, 0, 0));
    }
    TEST_ASSERT_EQUAL_UINT32(WS_CLIENTS_MAX, ctx.last_gen);
    // fd == last_gen (8) is not any connection's fd (they are 101..108)
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, (int)ctx.last_gen, ctx.last_gen));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 999, 1));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, WS_CLIENTS_MAX));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, -1));
    // The last slot works
    TEST_ASSERT_EQUAL_UINT32(WS_CLIENTS_MAX, ws_clients_slot_gen(&ctx, WS_CLIENTS_MAX - 1));
    TEST_ASSERT_EQUAL(WS_CLIENTS_MAX - 1, ws_clients_find_gen(&ctx, 101 + WS_CLIENTS_MAX - 1, WS_CLIENTS_MAX));
}

/* Slots outside the table must not be looked at: put a used-looking slot (state ACTIVE, fd 77, generation 777) just
 * before clients[0] and just after clients[max_clients - 1], where a wrong bound would read it. */
typedef struct {
    ws_client_t before;
    ws_clients_t ctx;
} guarded_ctx_t;

void test_ws_clients_gen_outside_the_table(void) {
    guarded_ctx_t g;
    memset(&g, 0, sizeof(g));
    TEST_ASSERT_EQUAL(0, ws_clients_init(&g.ctx, 4));
    g.before.state = WS_CLIENT_ACTIVE; g.before.fd = 77; g.before.gen = 777;
    g.ctx.clients[4].state = WS_CLIENT_ACTIVE; g.ctx.clients[4].fd = 77; g.ctx.clients[4].gen = 777; // == max_clients

    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&g.ctx, -1));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&g.ctx, 4));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&g.ctx, 77, 777));
    TEST_ASSERT_EQUAL(-1, ws_clients_find(&g.ctx, 77)); // (the same bound as the existing find)
}

void test_ws_clients_gen_reconnect_same_fd(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 4);

    // Connection A on fd 46, then a close request is "queued" with A's generation
    int a = ws_clients_add(&ctx, 46, 7, 0);
    uint32_t ga = ws_clients_slot_gen(&ctx, a);
    TEST_ASSERT_EQUAL(a, ws_clients_find_gen(&ctx, 46, ga));

    // A closes (close_fn), and B gets the same fd (the same slot, since it is the lowest FREE one)
    TEST_ASSERT_EQUAL(0, ws_clients_remove(&ctx, a));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 46, ga));      // in between: nothing there
    int b = ws_clients_add(&ctx, 46, 8, 0);
    TEST_ASSERT_EQUAL(a, b);                                       // same slot index is reused
    uint32_t gb = ws_clients_slot_gen(&ctx, b);
    TEST_ASSERT_NOT_EQUAL(ga, gb);                                 // but a different generation
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 46, ga));      // the old request does not match B
    TEST_ASSERT_EQUAL(b, ws_clients_find_gen(&ctx, 46, gb));       // B's own does
    TEST_ASSERT_TRUE(gb > ga);                                     // the counter only goes up

    // Many reconnects on the same fd and slot: every old generation stays stale
    uint32_t old[50];
    uint32_t cur = gb;
    for (int i = 0; i < 50; i++) {
        old[i] = cur;
        TEST_ASSERT_EQUAL(0, ws_clients_remove(&ctx, b));
        b = ws_clients_add(&ctx, 46, 8, 0);
        TEST_ASSERT_EQUAL(a, b);
        cur = ws_clients_slot_gen(&ctx, b);
        TEST_ASSERT_TRUE(cur > old[i]);
    }
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 46, old[i]));
    }
    TEST_ASSERT_EQUAL(b, ws_clients_find_gen(&ctx, 46, cur));
}

void test_ws_clients_gen_free_and_closing(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 2);

    int a = ws_clients_add(&ctx, 20, 0, 0);
    uint32_t ga = ws_clients_slot_gen(&ctx, a);

    // FREE slot: the generation it had must not match, whatever fd is asked (a FREE slot's fd is -1)
    TEST_ASSERT_EQUAL(0, ws_clients_remove(&ctx, a));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, a));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 20, ga));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, -1, ga));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, -1, 0));

    // CLOSING (removed while sending): the table still holds that connection, so the same generation is found
    // (this is the same meaning as ws_clients_find(), which also finds CLOSING). The caller decides what to do with it.
    int b = ws_clients_add(&ctx, 21, 0, 0);
    uint32_t gb = ws_clients_slot_gen(&ctx, b);
    TEST_ASSERT_TRUE(ws_clients_try_begin_send(&ctx, b));
    TEST_ASSERT_EQUAL(0, ws_clients_remove(&ctx, b));
    TEST_ASSERT_EQUAL(WS_CLIENT_CLOSING, ctx.clients[b].state);
    TEST_ASSERT_EQUAL_UINT32(gb, ws_clients_slot_gen(&ctx, b));
    TEST_ASSERT_EQUAL(b, ws_clients_find_gen(&ctx, 21, gb));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 21, gb + 1));  // other generation: still not found
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 22, gb));      // other fd: not found
    // The same fd cannot be added again while the slot is CLOSING (-2), so no new generation appears in that time
    uint32_t last = ctx.last_gen;
    TEST_ASSERT_EQUAL(-2, ws_clients_add(&ctx, 21, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(last, ctx.last_gen);

    // The send ends: FREE, no match any more
    TEST_ASSERT_EQUAL(0, ws_clients_end_send(&ctx, b));
    TEST_ASSERT_EQUAL(WS_CLIENT_FREE, ctx.clients[b].state);
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 21, gb));
    TEST_ASSERT_EQUAL_UINT32(0, ws_clients_slot_gen(&ctx, b));

    // A connection that reuses the fd after that has another generation
    int c = ws_clients_add(&ctx, 21, 0, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, c);
    TEST_ASSERT_NOT_EQUAL(gb, ws_clients_slot_gen(&ctx, c));
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 21, gb));
}

void test_ws_clients_gen_wraparound(void) {
    ws_clients_t ctx;
    ws_clients_init(&ctx, 2);

    // Init starts the counter at 0 -> the first connection has generation 1
    TEST_ASSERT_EQUAL_UINT32(0, ctx.last_gen);
    int a = ws_clients_add(&ctx, 5, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, ws_clients_slot_gen(&ctx, a));
    ws_clients_remove(&ctx, a);

    // Freeing a slot does not give the counter back
    TEST_ASSERT_EQUAL_UINT32(1, ctx.last_gen);
    int b = ws_clients_add(&ctx, 5, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(2, ws_clients_slot_gen(&ctx, b));
    ws_clients_remove(&ctx, b);

    // Wrap-around: UINT32_MAX is used, then 0 is skipped (0 means "no connection")
    ctx.last_gen = UINT32_MAX - 1;
    int c = ws_clients_add(&ctx, 5, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, ws_clients_slot_gen(&ctx, c));
    ws_clients_remove(&ctx, c);
    int d = ws_clients_add(&ctx, 5, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, ws_clients_slot_gen(&ctx, d));
    TEST_ASSERT_EQUAL_UINT32(1, ctx.last_gen);
    // The stale one (UINT32_MAX) does not match the connection that came after the wrap-around
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 5, UINT32_MAX));
    TEST_ASSERT_EQUAL(d, ws_clients_find_gen(&ctx, 5, 1));
    // 0 never matches
    TEST_ASSERT_EQUAL(-1, ws_clients_find_gen(&ctx, 5, 0));

    // From the very last value directly
    ws_clients_remove(&ctx, d);
    ctx.last_gen = UINT32_MAX;
    int e = ws_clients_add(&ctx, 6, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, ws_clients_slot_gen(&ctx, e));
}

/* Fuzz: random add / remove / begin_send / end_send; after every step, for every connection ever made, "it is found by
 * (fd, generation)" is true exactly when that very connection is still in the table. So an old generation never matches
 * a later connection, whatever fd and slot it reuses. The model gives every connection an id of its own. */
#define GEN_FUZZ_HISTORY 3000
static int g_h_fd[GEN_FUZZ_HISTORY];
static uint32_t g_h_gen[GEN_FUZZ_HISTORY];
static int g_h_slot[GEN_FUZZ_HISTORY];

void test_ws_clients_gen_fuzz(void) {
    for (int seed = 1; seed <= 16; seed++) {
        uint32_t rng = (uint32_t)seed * 2654435761u;
        if (rng == 0) rng = 1;
        ws_clients_t ctx;
        TEST_ASSERT_EQUAL(0, ws_clients_init(&ctx, 1 + (seed % WS_CLIENTS_MAX)));

        int h_n = 0;
        int slot_conn[WS_CLIENTS_MAX];  // the id (index in the history) of the connection now in the slot, -1 = none
        for (int i = 0; i < WS_CLIENTS_MAX; i++) slot_conn[i] = -1;
        uint32_t prev_gen = 0;

        for (int step = 0; step < 600; step++) {
            int op = xorshift32(&rng) % 5;
            int fd = (xorshift32(&rng) % 6) + 1;  // few fds, so they are reused a lot
            int slot = xorshift32(&rng) % ctx.max_clients;

            if (op <= 1) {
                int res = ws_clients_add(&ctx, fd, 0, 0);
                if (res >= 0) {
                    TEST_ASSERT_TRUE(h_n < GEN_FUZZ_HISTORY);
                    uint32_t g = ws_clients_slot_gen(&ctx, res);
                    TEST_ASSERT_NOT_EQUAL(0, g);
                    TEST_ASSERT_TRUE(g > prev_gen); // never goes down, never repeats (no wrap-around in this run)
                    prev_gen = g;
                    g_h_fd[h_n] = fd; g_h_gen[h_n] = g; g_h_slot[h_n] = res;
                    slot_conn[res] = h_n++;
                }
            } else if (op == 2) {
                int res = ws_clients_remove(&ctx, slot);
                if (res == 0 && ctx.clients[slot].state == WS_CLIENT_FREE) slot_conn[slot] = -1;
            } else if (op == 3) {
                (void)ws_clients_try_begin_send(&ctx, slot);
            } else {
                int res = ws_clients_end_send(&ctx, slot);
                if (res == 0 && ctx.clients[slot].state == WS_CLIENT_FREE) slot_conn[slot] = -1;
            }

            for (int k = 0; k < h_n; k++) {
                bool still_here = (slot_conn[g_h_slot[k]] == k);
                int found = ws_clients_find_gen(&ctx, g_h_fd[k], g_h_gen[k]);
                if (still_here) {
                    TEST_ASSERT_EQUAL(g_h_slot[k], found);
                    TEST_ASSERT_EQUAL_UINT32(g_h_gen[k], ws_clients_slot_gen(&ctx, g_h_slot[k]));
                } else {
                    TEST_ASSERT_EQUAL(-1, found);
                }
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ws_clients_init);
    RUN_TEST(test_ws_clients_add_remove);
    RUN_TEST(test_ws_clients_send_closing);
    RUN_TEST(test_ws_clients_session);
    RUN_TEST(test_ws_clients_iter);
    RUN_TEST(test_ws_clients_fuzz);
    RUN_TEST(test_ws_clients_gen_basic);
    RUN_TEST(test_ws_clients_gen_full_table_bounds);
    RUN_TEST(test_ws_clients_gen_outside_the_table);
    RUN_TEST(test_ws_clients_gen_reconnect_same_fd);
    RUN_TEST(test_ws_clients_gen_free_and_closing);
    RUN_TEST(test_ws_clients_gen_wraparound);
    RUN_TEST(test_ws_clients_gen_fuzz);
    return UNITY_END();
}
