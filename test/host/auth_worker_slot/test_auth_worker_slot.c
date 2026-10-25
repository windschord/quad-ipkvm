#include "unity.h"
#include "auth_worker_slot.h"

void setUp(void) {}
void tearDown(void) {}

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
    return UNITY_END();
}
