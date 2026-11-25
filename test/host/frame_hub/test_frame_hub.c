#include "video_source.h"
#include "unity.h"
#include "frame_hub.h"
#include <stdbool.h>
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


void test_frame_hub_commit_held_frame(void) {
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

    const video_frame_t* c1 = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_NOT_NULL(c1);
    TEST_ASSERT_EQUAL(50, c1->size);
    TEST_ASSERT_EQUAL(0, c1->seq);

    frame_hub_stats_t s;
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);

    // Attempt to commit to the HELD frame (this should be blocked by !is_writing)
    // We cast away const just to simulate the misuse
    frame_hub_commit(hub, (video_frame_t*)c1, 99, 10, 10, 2000);

    frame_hub_get_stats(hub, &s);
    // Should NOT increase
    TEST_ASSERT_EQUAL(1, s.published_frames);

    // Properties of held frame should NOT change
    TEST_ASSERT_EQUAL(50, c1->size);
    TEST_ASSERT_EQUAL(0, c1->seq);

    frame_hub_release_frame(hub, cid, c1);
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


static int dummy_start(const video_source_t *source, frame_hub_t *hub) { (void)source;
    if (!hub) return -1;
    video_frame_t* f = frame_hub_begin_publish(hub, 50);
    if (f) {
        frame_hub_commit(hub, f, 50, 10, 10, 100);
    }
    return 0;
}

static void dummy_stop(const video_source_t *source) {
    (void)source;
}

void test_frame_hub_video_source_contract(void) {
    video_source_t src = {
        .name = "dummy",
        .start = dummy_start,
        .stop = dummy_stop,
        .ctx = NULL
    };

    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
    };
    hub = frame_hub_create(&cfg);

    TEST_ASSERT_EQUAL_STRING("dummy", src.name);

    int res = src.start(&src, hub);
    TEST_ASSERT_EQUAL(0, res);

    frame_hub_stats_t s;
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);

    src.stop(&src);
}


// ---- P7: codec / flags (docs/INTERFACES.md 1.4) ----

static frame_hub_t* make_hub_p7(int pool, size_t max_size) {
    frame_hub_config_t cfg = {
        .pool_size = pool,
        .max_clients = 2,
        .max_frame_size = max_size,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
        .lock_cb = test_lock,
        .unlock_cb = test_unlock,
    };
    return frame_hub_create(&cfg);
}

void test_frame_hub_commit_sets_mjpeg_key(void) {
    hub = make_hub_p7(3, 100);
    int cid = frame_hub_register_client(hub);
    video_frame_t* f = frame_hub_begin_publish(hub, 50);
    TEST_ASSERT_NOT_NULL(f);
    frame_hub_commit(hub, f, 50, 640, 480, 1000);

    const video_frame_t* got = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT8(VIDEO_CODEC_MJPEG, got->codec);
    TEST_ASSERT_EQUAL_UINT8(VIDEO_FRAME_FLAG_KEY, got->flags);
    TEST_ASSERT_EQUAL(50, got->size);
    TEST_ASSERT_EQUAL(640, got->width);
    TEST_ASSERT_EQUAL(480, got->height);
    TEST_ASSERT_EQUAL_UINT64(1000, got->timestamp_ms);
    TEST_ASSERT_EQUAL_INT(0, frame_hub_release_frame(hub, cid, got));
}

void test_frame_hub_commit_ex_passes_codec_and_flags(void) {
    hub = make_hub_p7(4, 100);
    int cid = frame_hub_register_client(hub);

    struct { video_codec_t codec; uint8_t flags; size_t size; } cases[] = {
        {VIDEO_CODEC_H264, VIDEO_FRAME_FLAG_KEY, 80},
        {VIDEO_CODEC_H264, 0, 7},
        {VIDEO_CODEC_MJPEG, 0, 33},     // commit_ex does not force KEY for MJPEG
        {VIDEO_CODEC_H264, 0xFEu, 100}, // flags are passed as they are (size == max_frame_size is accepted)
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        video_frame_t* f = frame_hub_begin_publish(hub, cases[i].size);
        TEST_ASSERT_NOT_NULL(f);
        frame_hub_commit_info_t info = {
            .size = cases[i].size, .width = 1920, .height = 1080 + i, .timestamp_ms = 5000 + i,
            .codec = cases[i].codec, .flags = cases[i].flags,
        };
        frame_hub_commit_ex(hub, f, &info);

        const video_frame_t* got = frame_hub_get_latest_frame(hub, cid);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)cases[i].codec, got->codec);
        TEST_ASSERT_EQUAL_UINT8(cases[i].flags, got->flags);
        TEST_ASSERT_EQUAL(cases[i].size, got->size);
        TEST_ASSERT_EQUAL(1920, got->width);
        TEST_ASSERT_EQUAL(1080 + i, got->height);
        TEST_ASSERT_EQUAL_UINT64(5000 + i, got->timestamp_ms);
        TEST_ASSERT_EQUAL_INT64((int64_t)i, got->seq);
        TEST_ASSERT_EQUAL_INT(0, frame_hub_release_frame(hub, cid, got));
    }
    frame_hub_stats_t s;
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(4, s.published_frames);
    TEST_ASSERT_EQUAL(0, s.dropped_frames);
}

// A slot that held an H.264 key frame must not leak codec / flags into the next publish.
void test_frame_hub_begin_publish_resets_codec_and_flags(void) {
    hub = make_hub_p7(2, 100);
    video_frame_t* f = frame_hub_begin_publish(hub, 10);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT8(VIDEO_CODEC_MJPEG, f->codec);
    TEST_ASSERT_EQUAL_UINT8(0, f->flags);
    frame_hub_commit_info_t info = {.size = 10, .width = 1, .height = 1, .timestamp_ms = 1,
                                    .codec = VIDEO_CODEC_H264, .flags = VIDEO_FRAME_FLAG_KEY};
    frame_hub_commit_ex(hub, f, &info);

    // Reuse the slots of the (2-slot) pool several times: always reset on begin_publish.
    for (int i = 0; i < 6; i++) {
        video_frame_t* g = frame_hub_begin_publish(hub, 10);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQUAL_UINT8(VIDEO_CODEC_MJPEG, g->codec);
        TEST_ASSERT_EQUAL_UINT8(0, g->flags);
        // Dirty the fields as a publisher might, then abort or commit: the next begin must still reset them.
        g->codec = VIDEO_CODEC_H264;
        g->flags = 0xFFu;
        if (i % 2) {
            frame_hub_abort_publish(hub, g);
        } else {
            frame_hub_commit_ex(hub, g, &info);
        }
    }
}

// frame_hub_commit() overrides whatever the publisher wrote into the fields.
void test_frame_hub_commit_overrides_dirty_fields(void) {
    hub = make_hub_p7(3, 100);
    int cid = frame_hub_register_client(hub);
    video_frame_t* f = frame_hub_begin_publish(hub, 10);
    TEST_ASSERT_NOT_NULL(f);
    f->codec = VIDEO_CODEC_H264;
    f->flags = 0;
    frame_hub_commit(hub, f, 10, 2, 2, 2);
    const video_frame_t* got = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT8(VIDEO_CODEC_MJPEG, got->codec);
    TEST_ASSERT_EQUAL_UINT8(VIDEO_FRAME_FLAG_KEY, got->flags);
    frame_hub_release_frame(hub, cid, got);
}

// size 0 / size > max_frame_size: same handling as frame_hub_commit() (discard, dropped_frames++, slot freed, no seq)
void test_frame_hub_commit_ex_size_rules_match_commit(void) {
    hub = make_hub_p7(2, 100);
    int cid = frame_hub_register_client(hub);
    frame_hub_stats_t s;

    video_frame_t* f = frame_hub_begin_publish(hub, 100);
    TEST_ASSERT_NOT_NULL(f);
    frame_hub_commit_info_t info = {.size = 101, .width = 1, .height = 1, .timestamp_ms = 1,
                                    .codec = VIDEO_CODEC_H264, .flags = VIDEO_FRAME_FLAG_KEY};
    frame_hub_commit_ex(hub, f, &info);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.dropped_frames);
    TEST_ASSERT_EQUAL(0, s.published_frames);
    TEST_ASSERT_NULL(frame_hub_get_latest_frame(hub, cid));

    f = frame_hub_begin_publish(hub, 10);
    TEST_ASSERT_NOT_NULL(f);
    info.size = 0;
    frame_hub_commit_ex(hub, f, &info);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(2, s.dropped_frames);
    TEST_ASSERT_EQUAL(0, s.published_frames);
    TEST_ASSERT_NULL(frame_hub_get_latest_frame(hub, cid));

    // the slot was freed: publishing still works and seq starts at 0 (a discard does not consume a seq)
    f = frame_hub_begin_publish(hub, 10);
    TEST_ASSERT_NOT_NULL(f);
    info.size = 100; // exactly max: accepted
    frame_hub_commit_ex(hub, f, &info);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(2, s.dropped_frames);
    TEST_ASSERT_EQUAL(1, s.published_frames);
    const video_frame_t* got = frame_hub_get_latest_frame(hub, cid);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT64(0, got->seq);
    TEST_ASSERT_EQUAL(100, got->size);
    frame_hub_release_frame(hub, cid, got);
}

// Misuse of commit_ex: double commit, unknown frame, NULL arguments (no crash, no publish)
void test_frame_hub_commit_ex_misuse(void) {
    hub = make_hub_p7(3, 100);
    frame_hub_stats_t s;
    video_frame_t* f = frame_hub_begin_publish(hub, 10);
    TEST_ASSERT_NOT_NULL(f);
    frame_hub_commit_info_t info = {.size = 10, .width = 1, .height = 1, .timestamp_ms = 1,
                                    .codec = VIDEO_CODEC_H264, .flags = VIDEO_FRAME_FLAG_KEY};
    frame_hub_commit_ex(hub, f, &info);
    frame_hub_commit_ex(hub, f, &info); // double commit: ignored
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);

    video_frame_t stranger;
    memset(&stranger, 0, sizeof(stranger));
    frame_hub_commit_ex(hub, &stranger, &info); // not a pool frame: ignored
    frame_hub_commit_ex(NULL, f, &info);
    frame_hub_commit_ex(hub, NULL, &info);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);
    TEST_ASSERT_EQUAL(0, s.dropped_frames);

    // info == NULL: the publish is aborted (slot freed, nothing published or counted)
    f = frame_hub_begin_publish(hub, 10);
    TEST_ASSERT_NOT_NULL(f);
    frame_hub_commit_ex(hub, f, NULL);
    frame_hub_get_stats(hub, &s);
    TEST_ASSERT_EQUAL(1, s.published_frames);
    TEST_ASSERT_EQUAL(0, s.dropped_frames);
    f = frame_hub_begin_publish(hub, 10); // not stuck in "writing"
    TEST_ASSERT_NOT_NULL(f);
    frame_hub_abort_publish(hub, f);
}

// The old positional initialiser {name, start, stop, ctx} still compiles and leaves the P7 fields at 0 / NULL.
// (gcc -Wextra warns about the missing trailing fields of a positional initialiser, so the warning is silenced here
// only. The in-tree sources use designated initialisers, which do not warn: see the next test.)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
void test_video_source_positional_init_keeps_p7_fields_zero(void) {
    video_source_t src = {"legacy", dummy_start, dummy_stop, NULL};
    TEST_ASSERT_EQUAL_STRING("legacy", src.name);
    TEST_ASSERT_EQUAL_UINT32(0, src.supported_codecs);
    TEST_ASSERT_NULL(src.set_codec);
    TEST_ASSERT_NULL(src.request_keyframe);
    static const video_source_t s_static = {"legacy_static", dummy_start, dummy_stop, NULL};
    TEST_ASSERT_EQUAL_UINT32(0, s_static.supported_codecs);
    TEST_ASSERT_NULL(s_static.set_codec);
    TEST_ASSERT_NULL(s_static.request_keyframe);
}
#pragma GCC diagnostic pop

// The designated 4-field form used by test_pattern_source / csi_video_source: no warning, P7 fields 0 / NULL.
void test_video_source_designated_init_keeps_p7_fields_zero(void) {
    static const video_source_t s_src = {
        .name = "legacy_designated",
        .start = dummy_start,
        .stop = dummy_stop,
        .ctx = NULL,
    };
    TEST_ASSERT_EQUAL_UINT32(0, s_src.supported_codecs);
    TEST_ASSERT_NULL(s_src.set_codec);
    TEST_ASSERT_NULL(s_src.request_keyframe);
}

static int ext_set_codec_last = -1;
static int ext_keyframe_calls = 0;
static int ext_set_codec(const video_source_t* s, video_codec_t c) { (void)s; ext_set_codec_last = (int)c; return 0; }
static void ext_request_keyframe(const video_source_t* s) { (void)s; ext_keyframe_calls++; }

void test_video_source_p7_extension_fields(void) {
    video_source_t src = {
        .name = "h264",
        .start = dummy_start,
        .stop = dummy_stop,
        .supported_codecs = (1u << VIDEO_CODEC_MJPEG) | (1u << VIDEO_CODEC_H264),
        .set_codec = ext_set_codec,
        .request_keyframe = ext_request_keyframe,
    };
    TEST_ASSERT_TRUE(src.supported_codecs & (1u << VIDEO_CODEC_H264));
    TEST_ASSERT_EQUAL(0, src.set_codec(&src, VIDEO_CODEC_H264));
    TEST_ASSERT_EQUAL(VIDEO_CODEC_H264, ext_set_codec_last);
    src.request_keyframe(&src);
    TEST_ASSERT_EQUAL(1, ext_keyframe_calls);
}

// Fuzz: random commit / commit_ex / abort; every frame a client gets carries exactly what its publisher wrote.
void test_frame_hub_codec_flags_fuzz(void) {
    for (uint32_t seed = 1; seed <= 20; seed++) {
        fuzz_state = seed * 2654435761u + 7u;
        hub = make_hub_p7(4, 64);
        int cid = frame_hub_register_client(hub);
        uint8_t exp_codec[4096];
        uint8_t exp_flags[4096];
        int64_t next_seq = 0;
        const video_frame_t* held = NULL;
        for (int i = 0; i < 1500; i++) {
            uint32_t op = xorshift32() % 5;
            if (op <= 2) {
                size_t size = (xorshift32() % 70); // 0 .. 69: includes 0 and > max (64)
                video_frame_t* f = frame_hub_begin_publish(hub, size ? (size > 64 ? 64 : size) : 1);
                if (!f) continue;
                int kind = (int)(xorshift32() % 3);
                uint8_t flags = (uint8_t)(xorshift32() & 0xFFu);
                video_codec_t codec = (xorshift32() & 1u) ? VIDEO_CODEC_H264 : VIDEO_CODEC_MJPEG;
                bool valid = (size >= 1 && size <= 64);
                if (kind == 0) {
                    frame_hub_commit(hub, f, size, 1, 1, 1);
                    codec = VIDEO_CODEC_MJPEG;
                    flags = VIDEO_FRAME_FLAG_KEY;
                } else if (kind == 1) {
                    frame_hub_commit_info_t info = {.size = size, .width = 1, .height = 1, .timestamp_ms = 1,
                                                    .codec = codec, .flags = flags};
                    frame_hub_commit_ex(hub, f, &info);
                } else {
                    frame_hub_abort_publish(hub, f);
                    valid = false;
                }
                if (valid) {
                    exp_codec[next_seq % 4096] = (uint8_t)codec;
                    exp_flags[next_seq % 4096] = flags;
                    next_seq++;
                }
            } else if (op == 3) {
                if (!held) held = frame_hub_get_latest_frame(hub, cid);
                if (held) {
                    TEST_ASSERT_TRUE(held->seq >= 0 && held->seq < next_seq);
                    TEST_ASSERT_EQUAL_UINT8(exp_codec[held->seq % 4096], held->codec);
                    TEST_ASSERT_EQUAL_UINT8(exp_flags[held->seq % 4096], held->flags);
                }
            } else if (held) {
                TEST_ASSERT_EQUAL_INT(0, frame_hub_release_frame(hub, cid, held));
                held = NULL;
            }
        }
        frame_hub_destroy(hub);
        hub = NULL;
    }
}

typedef struct {
    int count;
    int lock_depth_seen;
} commit_cb_probe_t;

static void on_commit_probe(void* ctx) {
    commit_cb_probe_t* p = (commit_cb_probe_t*)ctx;
    p->count++;
    p->lock_depth_seen = lock_depth;
}

void test_frame_hub_on_commit_callback(void) {
    commit_cb_probe_t probe = { .count = 0, .lock_depth_seen = -1 };
    frame_hub_config_t cfg = {
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 100,
        .alloc_cb = test_alloc,
        .free_cb = test_free,
        .lock_cb = test_lock,
        .unlock_cb = test_unlock,
        .on_commit_cb = on_commit_probe,
        .on_commit_ctx = &probe,
    };
    hub = frame_hub_create(&cfg);
    TEST_ASSERT_NOT_NULL(hub);

    // Valid commit invokes callback outside lock.
    video_frame_t* f1 = frame_hub_begin_publish(hub, 20);
    TEST_ASSERT_NOT_NULL(f1);
    frame_hub_commit(hub, f1, 20, 640, 480, 1000);
    TEST_ASSERT_EQUAL_INT(1, probe.count);
    TEST_ASSERT_EQUAL_INT(0, probe.lock_depth_seen);

    // Valid commit_ex also invokes callback outside lock.
    video_frame_t* f2 = frame_hub_begin_publish(hub, 30);
    TEST_ASSERT_NOT_NULL(f2);
    frame_hub_commit_info_t info = {
        .size = 30, .width = 1280, .height = 720, .timestamp_ms = 2000,
        .codec = VIDEO_CODEC_H264, .flags = VIDEO_FRAME_FLAG_KEY,
    };
    frame_hub_commit_ex(hub, f2, &info);
    TEST_ASSERT_EQUAL_INT(2, probe.count);
    TEST_ASSERT_EQUAL_INT(0, probe.lock_depth_seen);

    // Abort and invalid size (0 / > max) / misuse must NOT invoke callback.
    video_frame_t* f3 = frame_hub_begin_publish(hub, 20);
    TEST_ASSERT_NOT_NULL(f3);
    frame_hub_abort_publish(hub, f3);
    TEST_ASSERT_EQUAL_INT(2, probe.count);

    f3 = frame_hub_begin_publish(hub, 20);
    TEST_ASSERT_NOT_NULL(f3);
    frame_hub_commit(hub, f3, 0, 10, 10, 3000);
    TEST_ASSERT_EQUAL_INT(2, probe.count);

    f3 = frame_hub_begin_publish(hub, 20);
    TEST_ASSERT_NOT_NULL(f3);
    frame_hub_commit(hub, f3, 101, 10, 10, 4000);
    TEST_ASSERT_EQUAL_INT(2, probe.count);
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
    RUN_TEST(test_frame_hub_commit_held_frame);
    RUN_TEST(test_frame_hub_video_source_contract);
    RUN_TEST(test_frame_hub_commit_misuse);
    RUN_TEST(test_frame_hub_fuzz);
    RUN_TEST(test_frame_hub_commit_sets_mjpeg_key);
    RUN_TEST(test_frame_hub_commit_ex_passes_codec_and_flags);
    RUN_TEST(test_frame_hub_begin_publish_resets_codec_and_flags);
    RUN_TEST(test_frame_hub_commit_overrides_dirty_fields);
    RUN_TEST(test_frame_hub_commit_ex_size_rules_match_commit);
    RUN_TEST(test_frame_hub_commit_ex_misuse);
    RUN_TEST(test_video_source_positional_init_keeps_p7_fields_zero);
    RUN_TEST(test_video_source_designated_init_keeps_p7_fields_zero);
    RUN_TEST(test_video_source_p7_extension_fields);
    RUN_TEST(test_frame_hub_codec_flags_fuzz);
    RUN_TEST(test_frame_hub_on_commit_callback);
    return UNITY_END();
}
