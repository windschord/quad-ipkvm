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

    // End send -> FREE
    TEST_ASSERT_EQUAL(0, ws_clients_end_send(&ctx, 0));
    TEST_ASSERT_EQUAL(WS_CLIENT_FREE, ctx.clients[0].state);

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ws_clients_init);
    RUN_TEST(test_ws_clients_add_remove);
    RUN_TEST(test_ws_clients_send_closing);
    RUN_TEST(test_ws_clients_session);
    RUN_TEST(test_ws_clients_iter);
    RUN_TEST(test_ws_clients_fuzz);
    return UNITY_END();
}
