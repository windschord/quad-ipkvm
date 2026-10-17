#include "unity.h"
#include "frame_hub.h"
#include <stdlib.h>
#include <stdint.h>

static void* dummy_alloc(size_t size) { return malloc(size); }
static void dummy_free(void* ptr) { free(ptr); }

static frame_hub_config_t config = {
    .alloc = dummy_alloc,
    .free = dummy_free,
    .pool_size = 5,
    .max_clients = 3
};

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void test_fuzz_frame_hub(void) {
    uint32_t seed = 0x12345678;
    frame_hub_t* hub = frame_hub_create(&config);

    int clients[3] = {-1, -1, -1};
    video_frame_t* refs[3][10] = {0};

    for (int i = 0; i < 1000; i++) {
        uint32_t op = xorshift32(&seed) % 100;

        if (op < 10) {
            // register
            int c = xorshift32(&seed) % 3;
            if (clients[c] == -1) clients[c] = frame_hub_register_client(hub);
        } else if (op < 20) {
            // unregister
            int c = xorshift32(&seed) % 3;
            if (clients[c] != -1) {
                frame_hub_unregister_client(hub, clients[c]);
                clients[c] = -1;
            }
        } else if (op < 60) {
            // publish
            video_frame_t* f = frame_hub_begin_publish(hub, (xorshift32(&seed) % 1000) + 1);
            if (f) {
                if (xorshift32(&seed) % 10 == 0) {
                    frame_hub_abort_publish(hub, f);
                } else {
                    frame_hub_commit(hub, f, 640, 480, i);
                }
            }
        } else if (op < 80) {
            // get
            int c = xorshift32(&seed) % 3;
            if (clients[c] != -1) {
                video_frame_t* f = frame_hub_get_latest_frame(hub, clients[c]);
                if (f) {
                    for(int j=0; j<10; j++) {
                        if (refs[c][j] == NULL) {
                            refs[c][j] = f;
                            break;
                        }
                    }
                    if(f && refs[c][9] != f) { /* we dropped it implicitly if array full for test */ }
                }
            }
        } else {
            // release
            int c = xorshift32(&seed) % 3;
            int r = xorshift32(&seed) % 10;
            if (refs[c][r]) {
                frame_hub_release_frame(hub, refs[c][r]);
                refs[c][r] = NULL;
            }
        }
    }

    for(int c=0; c<3; c++) {
        for(int j=0; j<10; j++) {
            if (refs[c][j]) frame_hub_release_frame(hub, refs[c][j]);
        }
    }

    frame_hub_destroy(hub);
}

void setUp(void) {}
void tearDown(void) {}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_frame_hub);
    return UNITY_END();
}
