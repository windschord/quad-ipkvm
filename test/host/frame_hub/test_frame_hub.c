#include "unity.h"
#include "frame_hub.h"
#include <stdlib.h>
#include <string.h>

static int alloc_count = 0;
static int free_count = 0;
static size_t total_allocated = 0;
static int inject_alloc_fail_at = -1;
static int current_alloc = 0;

static int lock_count = 0;
static int lock_depth = 0;

static void* test_alloc(size_t size, void* ctx) {
    (void)ctx;
    current_alloc++;
    if (inject_alloc_fail_at == current_alloc) {
        return NULL;
    }
    alloc_count++;
    total_allocated += size;
    return malloc(size);
}

static void test_free(void* ptr, void* ctx) {
    (void)ctx;
    if (ptr) {
        free_count++;
        free(ptr);
    }
}

static void test_lock(void* ctx) {
    (void)ctx;
    TEST_ASSERT_EQUAL(0, lock_depth); // Prevent double lock
    lock_depth++;
    lock_count++;
}

static void test_unlock(void* ctx) {
    (void)ctx;
    TEST_ASSERT_EQUAL(1, lock_depth); // Prevent unlock without lock
    lock_depth--;
}

static frame_hub_t* hub = NULL;

void setUp(void) {
    alloc_count = 0;
    free_count = 0;
    total_allocated = 0;
    inject_alloc_fail_at = -1;
    current_alloc = 0;
    lock_count = 0;
    lock_depth = 0;
    hub = NULL;
}

void tearDown(void) {
    if (hub) {
        frame_hub_destroy(hub);
        hub = NULL;
    }
    TEST_ASSERT_EQUAL(0, lock_depth); // Verify balanced locks
    TEST_ASSERT_EQUAL_MESSAGE(alloc_count, free_count, "Memory leak detected in test allocator");
}

void test_frame_hub_create_invalid_config(void) {
    frame_hub_config_t cfg = {
        .pool_size = 1, // Invalid
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    TEST_ASSERT_NULL(frame_hub_create(&cfg));

    cfg.pool_size = 2;
    cfg.max_clients = 0; // Invalid
    TEST_ASSERT_NULL(frame_hub_create(&cfg));

    cfg.max_clients = 2;
    cfg.max_frame_size = 0; // Invalid
    TEST_ASSERT_NULL(frame_hub_create(&cfg));

    cfg.max_frame_size = 100;
    cfg.alloc_cb = NULL; // Invalid
    TEST_ASSERT_NULL(frame_hub_create(&cfg));
}

void test_frame_hub_alloc_failure(void) {
    frame_hub_config_t cfg = {
        .pool_size = 2,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };

    // Fail allocation for pool data (3rd alloc)
    inject_alloc_fail_at = 3;
    TEST_ASSERT_NULL(frame_hub_create(&cfg));
    TEST_ASSERT_EQUAL(alloc_count, free_count);
}

void test_frame_hub_basic_publish_subscribe(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
        .lock_cb = test_lock,
        .unlock_cb = test_unlock
    };

    hub = frame_hub_create(&cfg);
    TEST_ASSERT_NOT_NULL(hub);

    int client_id = frame_hub_register_client(hub);
    TEST_ASSERT_EQUAL(0, client_id);

    // Publish 1st frame
    video_frame_t* f1 = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NOT_NULL(f1);
    memset(f1->data, 0xAA, 50);
    frame_hub_commit(hub, f1, 50, 640, 480, 1000);

    // Get frame
    const video_frame_t* c1_f = frame_hub_get_latest_frame(hub, client_id);
    TEST_ASSERT_NOT_NULL(c1_f);
    TEST_ASSERT_EQUAL(50, c1_f->size);
    TEST_ASSERT_EQUAL(0xAA, c1_f->data[0]);
    TEST_ASSERT_EQUAL(0, c1_f->seq);

    // No new frame
    TEST_ASSERT_NULL(frame_hub_get_latest_frame(hub, client_id));

    // Release frame
    TEST_ASSERT_EQUAL(0, frame_hub_release_frame(hub, client_id, c1_f));
}

void test_frame_hub_buffer_reuse_and_exhaustion(void) {
    frame_hub_config_t cfg = {
        .pool_size = 2,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
        .lock_cb = test_lock,
        .unlock_cb = test_unlock
    };

    hub = frame_hub_create(&cfg);
    TEST_ASSERT_NOT_NULL(hub);

    int client_id = frame_hub_register_client(hub);

    // Frame 1
    video_frame_t* f1 = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f1, 50, 640, 480, 1000);

    // Client holds Frame 1
    const video_frame_t* c_f = frame_hub_get_latest_frame(hub, client_id);
    TEST_ASSERT_NOT_NULL(c_f);

    // Frame 2
    video_frame_t* f2 = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f2, 50, 640, 480, 2000);

    // Frame 3 (Pool exhausted, Frame 1 is held, Frame 2 is latest, pool size 2)
    video_frame_t* f3 = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NULL(f3);

    frame_hub_stats_t stats;
    frame_hub_get_stats(hub, &stats);
    TEST_ASSERT_EQUAL(1, stats.dropped_frames);

    // Release Frame 1
    frame_hub_release_frame(hub, client_id, c_f);

    // Now Frame 3 should succeed
    f3 = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NOT_NULL(f3);
    frame_hub_commit(hub, f3, 50, 640, 480, 3000);
}

void test_frame_hub_invalid_release(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);
    int cid = frame_hub_register_client(hub);

    video_frame_t* f = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f, 50, 10, 10, 10);

    const video_frame_t* cf = frame_hub_get_latest_frame(hub, cid);

    // Double release should fail
    TEST_ASSERT_EQUAL(0, frame_hub_release_frame(hub, cid, cf));
    TEST_ASSERT_EQUAL(-1, frame_hub_release_frame(hub, cid, cf));

    // Invalid client
    TEST_ASSERT_EQUAL(-1, frame_hub_release_frame(hub, 99, cf));
}

void test_frame_hub_max_clients(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);

    int c1 = frame_hub_register_client(hub);
    int c2 = frame_hub_register_client(hub);
    int c3 = frame_hub_register_client(hub);

    TEST_ASSERT_NOT_EQUAL(-1, c1);
    TEST_ASSERT_NOT_EQUAL(-1, c2);
    TEST_ASSERT_EQUAL(-1, c3);

    frame_hub_unregister_client(hub, c1);

    int c4 = frame_hub_register_client(hub);
    TEST_ASSERT_NOT_EQUAL(-1, c4);
}

void test_frame_hub_commit_oversize(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);

    video_frame_t* f = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f, 150, 10, 10, 10); // Oversize

    frame_hub_stats_t s;
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.dropped_frames);
    TEST_ASSERT_EQUAL(0, s.published_frames);
}

void test_frame_hub_size_alternation(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);
    int cid = frame_hub_register_client(hub);

    // Frame 1: 50 bytes
    video_frame_t* f1 = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f1, 50, 640, 480, 1000);
    const video_frame_t* c1 = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_EQUAL(50, c1->size);
    frame_hub_release_frame(hub, cid, c1);

    // Frame 2: 90 bytes
    video_frame_t* f2 = frame_hub_begin_publish(hub, 90);
    frame_hub_commit(hub, f2, 90, 640, 480, 1000);
    const video_frame_t* c2 = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_EQUAL(90, c2->size);
    frame_hub_release_frame(hub, cid, c2);

    // Frame 3: 20 bytes
    video_frame_t* f3 = frame_hub_begin_publish(hub, 20);
    frame_hub_commit(hub, f3, 20, 640, 480, 1000);
    const video_frame_t* c3 = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_EQUAL(20, c3->size);
    frame_hub_release_frame(hub, cid, c3);
}

void test_frame_hub_slow_client(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);
    int cid = frame_hub_register_client(hub);

    video_frame_t* f1 = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f1, 50, 640, 480, 1000);

    // Client gets f1, but is slow processing
    const video_frame_t* c1 = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_EQUAL(0, c1->seq);

    // f2 and f3 are published while client is busy
    video_frame_t* f2 = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f2, 50, 640, 480, 2000);
    video_frame_t* f3 = frame_hub_begin_publish(hub, 50);
    frame_hub_commit(hub, f3, 50, 640, 480, 3000);

    // Client finishes f1
    frame_hub_release_frame(hub, cid, c1);

    // Client gets next frame, it should be f3 (seq 2), skipping f2 (seq 1)
    const video_frame_t* c2 = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_NOT_NULL(c2);
    TEST_ASSERT_EQUAL(2, c2->seq);

    frame_hub_release_frame(hub, cid, c2);
}

void test_frame_hub_publish_abort(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);

    video_frame_t* f = frame_hub_begin_publish(hub, 50);
    // Abort
    frame_hub_commit(hub, f, 0, 10, 10, 10); // size 0 acts as discard

    frame_hub_stats_t s;
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.dropped_frames);
    TEST_ASSERT_EQUAL(0, s.published_frames);

    int cid = frame_hub_register_client(hub);
    TEST_ASSERT_NULL(frame_hub_get_latest_frame(hub, cid));
}

// Xorshift32 for deterministic fuzzing
static uint32_t fuzz_state = 123456789;
static uint32_t xorshift32() {
    fuzz_state ^= fuzz_state << 13;
    fuzz_state ^= fuzz_state >> 17;
    fuzz_state ^= fuzz_state << 5;
    return fuzz_state;
}

static void run_fuzz_seed(uint32_t seed) {
    fuzz_state = seed;
    frame_hub_config_t cfg = {
        .pool_size = 5,
        .max_clients = 3,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);

    int clients[3] = {-1, -1, -1};
    const video_frame_t* held_frames[3] = {NULL, NULL, NULL};
    int64_t model_seq = -1;
    int64_t client_seq[3] = {-1, -1, -1};

    // Independent model trackers
    uint64_t model_published = 0;
    uint64_t model_dropped = 0;
    int model_active_clients = 0;

    // To track memory leak injection or pool exhaustion correctly, we simulate the pool state
    // pool slots: 0 to 4. Each has a ref count.
    int pool_ref_count[5] = {0, 0, 0, 0, 0};
    int pool_active[5] = {0, 0, 0, 0, 0}; // 1 if contains data
    uint64_t pool_seq[5] = {0, 0, 0, 0, 0}; // Track exact seq of slot
    int pool_held_by_client[5][3] = {{0}}; // mapping slot -> client -> held
    int latest_pool_idx = -1;

    for (int i = 0; i < 1000; i++) {
        uint32_t op = xorshift32() % 5;
        if (op == 0) { // register
            for (int c = 0; c < 3; c++) {
                if (clients[c] == -1) {
                    clients[c] = frame_hub_register_client(hub);
                    TEST_ASSERT_NOT_EQUAL(-1, clients[c]);
                    client_seq[c] = -1;
                    model_active_clients++;
                    break;
                }
            }
        } else if (op == 1) { // unregister
            for (int c = 0; c < 3; c++) {
                if (clients[c] != -1 && xorshift32() % 2 == 0) {
                    if (held_frames[c] != NULL) {
                        // find pool idx and decrement
                        for (int p=0; p<5; p++) {
                            if (pool_held_by_client[p][c]) {
                                pool_ref_count[p]--;
                                pool_held_by_client[p][c] = 0;
                            }
                        }
                        held_frames[c] = NULL;
                    }
                    frame_hub_unregister_client(hub, clients[c]);
                    clients[c] = -1;
                    client_seq[c] = -1;
                    model_active_clients--;
                    break;
                }
            }
        } else if (op == 2) { // publish
            video_frame_t* f = frame_hub_begin_publish(hub, 50);

            // Model calculation of available slot
            int selected_idx = -1;
            uint64_t oldest_seq = UINT64_MAX;
            for (int p=0; p<5; p++) {
                if (pool_ref_count[p] == 0 && p != latest_pool_idx) {
                    if (!pool_active[p]) {
                        selected_idx = p;
                        break;
                    }
                    if (pool_seq[p] < oldest_seq) {
                        oldest_seq = pool_seq[p];
                        selected_idx = p;
                    }
                }
            }

            if (f) {
                TEST_ASSERT_NOT_EQUAL(-1, selected_idx); // Should only succeed if model says it can
                model_seq++;
                model_published++;
                latest_pool_idx = selected_idx;
                pool_active[latest_pool_idx] = 1;
                pool_seq[latest_pool_idx] = model_seq;

                // Fill pattern: use sequence number as pattern
                for(int j=0; j<50; j++) f->data[j] = (uint8_t)(model_seq & 0xFF);
                frame_hub_commit(hub, f, 50, 640, 480, 1000);
            } else {
                // Wait, our simple model might differ slightly on 'oldest' tie-breaks, but
                // pool exhaustion (selected_idx == -1) must match perfectly.
                TEST_ASSERT_EQUAL(-1, selected_idx);
                model_dropped++;
            }
        } else if (op == 3) { // get latest
            for (int c = 0; c < 3; c++) {
                if (clients[c] != -1 && held_frames[c] == NULL) {
                    held_frames[c] = frame_hub_get_latest_frame(hub, clients[c]);
                    if (held_frames[c]) {
                        TEST_ASSERT_TRUE(held_frames[c]->seq > client_seq[c]);
                        client_seq[c] = held_frames[c]->seq;

                        pool_ref_count[latest_pool_idx]++;
                        pool_held_by_client[latest_pool_idx][c] = 1;

                        // Verify pattern of newly acquired frame
                        for(int j=0; j<50; j++) TEST_ASSERT_EQUAL((uint8_t)(client_seq[c] & 0xFF), held_frames[c]->data[j]);
                    }
                }
            }
        } else if (op == 4) { // release
            for (int c = 0; c < 3; c++) {
                if (clients[c] != -1 && held_frames[c] != NULL) {
                    int res = frame_hub_release_frame(hub, clients[c], held_frames[c]);
                    TEST_ASSERT_EQUAL(0, res);
                    for (int p=0; p<5; p++) {
                        if (pool_held_by_client[p][c]) {
                            pool_ref_count[p]--;
                            pool_held_by_client[p][c] = 0;
                        }
                    }
                    held_frames[c] = NULL;
                }
            }
        }

        // At every step, assert that currently held frames are not corrupted (i.e. not overwritten by another publish)
        for (int c = 0; c < 3; c++) {
            if (clients[c] != -1 && held_frames[c] != NULL) {
                for(int j=0; j<50; j++) {
                    TEST_ASSERT_EQUAL_MESSAGE((uint8_t)(client_seq[c] & 0xFF), held_frames[c]->data[j], "Held frame was corrupted (reused while ref_count > 0)");
                }
            }
        }
    }

    // Verify stats
    frame_hub_stats_t s;
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(model_published, s.published_frames);
    TEST_ASSERT_EQUAL(model_dropped, s.dropped_frames);
    TEST_ASSERT_EQUAL(model_active_clients, s.active_clients);

    // Release remaining
    for (int c = 0; c < 3; c++) {
        if (clients[c] != -1 && held_frames[c] != NULL) {
            frame_hub_release_frame(hub, clients[c], held_frames[c]);
            held_frames[c] = NULL;
        }
    }
    frame_hub_destroy(hub);
    hub = NULL;
}

void test_frame_hub_commit_misuse(void) {
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);

    // Test double begin_publish returns NULL
    video_frame_t* f1 = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NOT_NULL(f1);
    video_frame_t* f_double = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NULL(f_double);

    // Commit
    frame_hub_commit(hub, f1, 50, 640, 480, 1000);

    frame_hub_stats_t s;
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);

    // Double commit (should be ignored)
    frame_hub_commit(hub, f1, 50, 640, 480, 2000);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);

    // Commit to non-begun slot
    video_frame_t* f2 = frame_hub_begin_publish(hub, 50);
    frame_hub_abort_publish(hub, f2);
    frame_hub_commit(hub, f2, 50, 640, 480, 3000);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);

    // Abort publish works and we can publish again
    video_frame_t* f3 = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NOT_NULL(f3);
    frame_hub_abort_publish(hub, f3);

    video_frame_t* f4 = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NOT_NULL(f4);
    frame_hub_commit(hub, f4, 50, 640, 480, 4000);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(2, s.published_frames);
}

void test_frame_hub_fuzz(void) {
    uint32_t seeds[] = {
        123456789, 987654321, 111111111, 222222222,
        333333333, 444444444, 555555555, 666666666,
        777777777, 888888888, 999999999, 101010101,
        202020202, 303030303, 404040404, 505050505
    };
    for (int i = 0; i < 16; i++) {
        run_fuzz_seed(seeds[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_frame_hub_create_invalid_config);
    RUN_TEST(test_frame_hub_alloc_failure);
    RUN_TEST(test_frame_hub_basic_publish_subscribe);
    RUN_TEST(test_frame_hub_buffer_reuse_and_exhaustion);
    RUN_TEST(test_frame_hub_invalid_release);
    RUN_TEST(test_frame_hub_max_clients);
    RUN_TEST(test_frame_hub_commit_oversize);
    RUN_TEST(test_frame_hub_size_alternation);
    RUN_TEST(test_frame_hub_slow_client);
    RUN_TEST(test_frame_hub_publish_abort);
    RUN_TEST(test_frame_hub_commit_misuse);
    RUN_TEST(test_frame_hub_fuzz);
    return UNITY_END();
}
