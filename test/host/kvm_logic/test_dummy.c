#include "unity.h"
#include "kvm_logic.h"

void setUp(void) {}
void tearDown(void) {}

void test_kvm_logic_dummy_returns_42(void) {
    TEST_ASSERT_EQUAL(42, kvm_logic_dummy());
}

void test_kvm_logic_config_validation(void) {
    kvm_logic_t logic;
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_INVALID_PARAM, kvm_logic_init(&logic, NULL));

    kvm_logic_config_t config = {0}; // all zeroes
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_INVALID_PARAM, kvm_logic_init(&logic, &config));

    kvm_logic_config_default(&config);
    TEST_ASSERT_EQUAL(KVM_RESULT_OK, kvm_logic_init(&logic, &config));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_kvm_logic_dummy_returns_42);
    RUN_TEST(test_kvm_logic_config_validation);
    return UNITY_END();
}
