#include "unity.h"
#include "frame_hub.h"
#include <stdlib.h>

static void* dummy_alloc(size_t size) { return malloc(size); }
static void dummy_free(void* ptr) { free(ptr); }

static frame_hub_config_t config = {
    .alloc = dummy_alloc,
    .free = dummy_free,
    .pool_size = 3,
    .max_clients = 2
};

void test_frame_hub_lifecycle(void) {
    frame_hub_t* hub = frame_hub_create(&config);
    TEST_ASSERT_NOT_NULL(hub);
    frame_hub_destroy(hub);
}

void test_frame_hub_publish_and_subscribe(void) {
    frame_hub_t* hub = frame_hub_create(&config);
    int client1 = frame_hub_register_client(hub);
    TEST_ASSERT_EQUAL(0, client1);

    // Publish a frame
    video_frame_t* f1 = frame_hub_begin_publish(hub, 100);
    TEST_ASSERT_NOT_NULL(f1);
    f1->data[0] = 0xAA;
    frame_hub_commit(hub, f1, 640, 480, 1000);

    // Subscribe
    video_frame_t* latest1 = frame_hub_get_latest_frame(hub, client1);
    TEST_ASSERT_NOT_NULL(latest1);
    TEST_ASSERT_EQUAL(0xAA, latest1->data[0]);
    TEST_ASSERT_EQUAL(640, latest1->width);

    // Subscribe again without new frame
    video_frame_t* latest2 = frame_hub_get_latest_frame(hub, client1);
    TEST_ASSERT_NULL(latest2);

    frame_hub_release_frame(hub, latest1);

    frame_hub_unregister_client(hub, client1);
    frame_hub_destroy(hub);
}

void test_frame_hub_pool_exhaustion(void) {
    frame_hub_t* hub = frame_hub_create(&config);

    int client_id = frame_hub_register_client(hub);

    // pool_size is 3. We hold 3 frames in reference, next publish should fail.
    video_frame_t* f1 = frame_hub_begin_publish(hub, 100); frame_hub_commit(hub, f1, 10, 10, 1);
    video_frame_t* ref1 = frame_hub_get_latest_frame(hub, client_id);

    video_frame_t* f2 = frame_hub_begin_publish(hub, 100); frame_hub_commit(hub, f2, 10, 10, 2);
    video_frame_t* ref2 = frame_hub_get_latest_frame(hub, client_id);

    video_frame_t* f3 = frame_hub_begin_publish(hub, 100); frame_hub_commit(hub, f3, 10, 10, 3);
    video_frame_t* ref3 = frame_hub_get_latest_frame(hub, client_id);

    video_frame_t* f4 = frame_hub_begin_publish(hub, 100);
    TEST_ASSERT_NULL(f4); // Should be exhausted

    frame_hub_release_frame(hub, ref1);
    frame_hub_release_frame(hub, ref2);
    frame_hub_release_frame(hub, ref3);

    video_frame_t* f5 = frame_hub_begin_publish(hub, 100);
    TEST_ASSERT_NOT_NULL(f5);
    frame_hub_abort_publish(hub, f5);

    frame_hub_destroy(hub);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_frame_hub_lifecycle);
    RUN_TEST(test_frame_hub_publish_and_subscribe);
    RUN_TEST(test_frame_hub_pool_exhaustion);
    return UNITY_END();
}
