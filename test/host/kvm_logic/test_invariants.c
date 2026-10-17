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

    // Stabilize LED to channel 1
    kvm_logic_step(&logic, &inputs, 0, &outputs);
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms, &outputs);
}

void tearDown(void) {
}

void test_invariant_1_a0a1_matches_led(void) {
    // Already in IDLE with LED 1
    TEST_ASSERT_EQUAL(1, outputs.a0a1_channel);

    // Manual track
    inputs.led_raw_state = 2;
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 10, &outputs);
    TEST_ASSERT_EQUAL(1, outputs.a0a1_channel); // Should still be 1 until LED stabilizes and setup time passes

    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 10 + config.led_stable_time_ms, &outputs); // stable

    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 10 + config.led_stable_time_ms + config.setup_time_ms, &outputs); // setup passed

    TEST_ASSERT_EQUAL(2, outputs.a0a1_channel); // Now it matches!
}

void test_invariant_2_no_atx_during_switch(void) {
    kvm_logic_request_switch(&logic, 2);

    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP

    // Any ATX request should fail
    kvm_result_t res = kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER, 300);
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_SWITCHING, res);

    // e_power_low, e_reset_low must be false!
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.e_reset_low);

    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.e_reset_low);
}

void test_invariant_4_a0a1_change_sequence(void) {
    // When changing A0/A1, E MUST be high!
    kvm_logic_request_switch(&logic, 2);

    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SWITCH_WAIT_SETUP

    TEST_ASSERT_FALSE(outputs.e_kvm_low);
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.e_reset_low);
    TEST_ASSERT_EQUAL(2, outputs.a0a1_channel); // A0/A1 changes immediately while E is HIGH

    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE

    TEST_ASSERT_TRUE(outputs.e_kvm_low); // E goes low ONLY AFTER setup time!
}

void test_time_wraparound_safety(void) {
    uint64_t high_ms = 0xFFFFFFFFFFFFFFF0ULL;
    kvm_logic_init(&logic, &config);
    inputs.led_raw_state = 1;
    kvm_logic_step(&logic, &inputs, high_ms, &outputs);

    // Force stabilization by moving time just far enough, and letting it wrap around safely in uint64_t math.
    uint64_t next_ms = high_ms + config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, next_ms, &outputs);
    // Now it should be IDLE.
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);

    inputs.led_raw_state = 2;
    next_ms += 10;
    kvm_logic_step(&logic, &inputs, next_ms, &outputs);

    next_ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, next_ms, &outputs); // Wait stability

    // At this point it should have transitioned to WAIT_VIDEO for tracking
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_VIDEO, logic.state);
}

int main(void) { UNITY_BEGIN(); RUN_TEST(test_invariant_1_a0a1_matches_led); RUN_TEST(test_invariant_2_no_atx_during_switch); RUN_TEST(test_invariant_4_a0a1_change_sequence); RUN_TEST(test_time_wraparound_safety); return UNITY_END(); }
