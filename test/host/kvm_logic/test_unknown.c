#include "unity.h"
#include "kvm_logic.h"

kvm_logic_t logic;
kvm_logic_config_t config;
kvm_logic_inputs_t inputs;
kvm_logic_outputs_t outputs;

void setUp(void) {
    kvm_logic_config_default(&config);
    kvm_logic_init(&logic, &config);
    inputs.led_raw_state = 1;
    inputs.video_sync = KVM_VIDEO_SYNC_LOCKED;
}

void tearDown(void) {}

void test_unknown_state_on_invalid_led(void) {
    // Already starts in UNKNOWN
    TEST_ASSERT_EQUAL(KVM_STATE_UNKNOWN, logic.state);

    // Stabilize to an invalid LED state (e.g. 0 or 5)
    inputs.led_raw_state = 0;
    kvm_logic_step(&logic, &inputs, 0, &outputs);
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms, &outputs);

    // Should still be UNKNOWN
    TEST_ASSERT_EQUAL(KVM_STATE_UNKNOWN, logic.state);
    TEST_ASSERT_TRUE(outputs.atx_locked);
    TEST_ASSERT_FALSE(outputs.e_kvm_low);
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.e_reset_low);

    // Stabilize to valid LED
    inputs.led_raw_state = 2;
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 10, &outputs); // reset timer
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 10 + config.led_stable_time_ms, &outputs); // stable

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_EQUAL(2, outputs.active_channel);
    TEST_ASSERT_EQUAL(2, outputs.a0a1_channel);
    TEST_ASSERT_FALSE(outputs.atx_locked);

    // Test transitioning back to UNKNOWN
    inputs.led_raw_state = 5;
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms * 3, &outputs); // raw change, immediate transition to UNKNOWN

    TEST_ASSERT_EQUAL(KVM_STATE_UNKNOWN, logic.state);
    TEST_ASSERT_TRUE(outputs.atx_locked);
}

int main(void) { UNITY_BEGIN(); RUN_TEST(test_unknown_state_on_invalid_led); return UNITY_END(); }
