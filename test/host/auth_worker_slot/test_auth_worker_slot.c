#include "unity.h"
#include "auth_worker_slot.h"

void setUp(void) {}
void tearDown(void) {}

void test_auth_worker_slot_null(void) {
    TEST_ASSERT_FALSE(auth_worker_slot_try_acquire(NULL));
    auth_worker_slot_release(NULL); // Should not crash
    auth_worker_slot_init(NULL); // Should not crash
}

void test_auth_worker_slot_double_release(void) {
    auth_worker_slot_t slot;
    auth_worker_slot_init(&slot);
    TEST_ASSERT_TRUE(auth_worker_slot_try_acquire(&slot));
    auth_worker_slot_release(&slot);
    auth_worker_slot_release(&slot); // Double release
    TEST_ASSERT_TRUE(auth_worker_slot_try_acquire(&slot));
    TEST_ASSERT_FALSE(auth_worker_slot_try_acquire(&slot));
    auth_worker_slot_release(&slot);
}

void test_auth_worker_slot_repeat(void) {
    auth_worker_slot_t slot;
    auth_worker_slot_init(&slot);
    for (int i = 0; i < 1000; i++) {
        TEST_ASSERT_TRUE(auth_worker_slot_try_acquire(&slot));
        TEST_ASSERT_FALSE(auth_worker_slot_try_acquire(&slot));
        auth_worker_slot_release(&slot);
    }
}

void test_auth_worker_slot_init_during_use(void) {
    auth_worker_slot_t slot;
    auth_worker_slot_init(&slot);
    TEST_ASSERT_TRUE(auth_worker_slot_try_acquire(&slot));
    auth_worker_slot_init(&slot);
    TEST_ASSERT_TRUE(auth_worker_slot_try_acquire(&slot));
    auth_worker_slot_release(&slot);
}

void test_auth_worker_slot_try_acquire(void) {
    auth_worker_slot_t slot;
    auth_worker_slot_init(&slot);

    TEST_ASSERT_TRUE(auth_worker_slot_try_acquire(&slot));
    TEST_ASSERT_FALSE(auth_worker_slot_try_acquire(&slot));

    auth_worker_slot_release(&slot);

    TEST_ASSERT_TRUE(auth_worker_slot_try_acquire(&slot));
    auth_worker_slot_release(&slot);
}

int main(void) {
    UNITY_BEGIN();
        RUN_TEST(test_auth_worker_slot_try_acquire);
    RUN_TEST(test_auth_worker_slot_null);
    RUN_TEST(test_auth_worker_slot_double_release);
    RUN_TEST(test_auth_worker_slot_repeat);
    RUN_TEST(test_auth_worker_slot_init_during_use);
    return UNITY_END();
}
