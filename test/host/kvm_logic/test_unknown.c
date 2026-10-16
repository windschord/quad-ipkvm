#include "unity.h"
#include "kvm_logic.h"

kvm_logic_t logic;
kvm_logic_config_t config;
kvm_logic_inputs_t inputs;
kvm_logic_outputs_t outputs;

void setUp(void) {
    config = (kvm_logic_config_t){
        .led_stable_time_ms = 50,
        .switch_kvm_pulse_time_ms = 150,
        .switch_led_wait_timeout_ms = 2000,
        .switch_video_sync_timeout_ms = 3000,
        .setup_time_ms = 10,
        .max_power_pulse_ms = 500,
        .max_reset_pulse_ms = 200,
        .max_force_power_pulse_ms = 5000
    };
    kvm_logic_init(&logic, &config);
    inputs.led_raw_state = 0; // Unknown at start
    inputs.video_sync = KVM_VIDEO_SYNC_LOCKED;
}

void tearDown(void) {
}

void test_startup_led_stabilization(void) {
    TEST_ASSERT_EQUAL(KVM_STATE_UNKNOWN, logic.state);

    // Set LED to 1
    inputs.led_raw_state = 1;
    kvm_logic_step(&logic, &inputs, 10, &outputs);

    // Should wait stable time
    kvm_logic_step(&logic, &inputs, 10 + config.led_stable_time_ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_EQUAL(1, outputs.active_channel);
    TEST_ASSERT_EQUAL(1, outputs.a0a1_channel);
}

void test_unknown_state_on_invalid_led(void) {
    // stabilize first
    inputs.led_raw_state = 1;
    kvm_logic_step(&logic, &inputs, 0, &outputs);
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms, &outputs);

    // Now invalid LED (e.g. 0, all off, or multiple which we map to > 4 maybe, let's use 0 for invalid)
    inputs.led_raw_state = 0;
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 10, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_UNKNOWN, logic.state);
    TEST_ASSERT_TRUE(outputs.atx_locked);
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.e_reset_low);
    TEST_ASSERT_FALSE(outputs.e_kvm_low);
    TEST_ASSERT_EQUAL(1, outputs.a0a1_channel); // Should hold previous value
}

void test_switch_interrupted_by_unexpected_led_change(void) {
    inputs.led_raw_state = 1;
    kvm_logic_step(&logic, &inputs, 0, &outputs);
    uint64_t ms = config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    kvm_logic_request_switch(&logic, 2);

    ms += 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_LED

    // Unexpected LED change to 3!
    inputs.led_raw_state = 3;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    // Should transition to wait video for channel 3 after stable! Or manual track.
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state); // Because we were expecting 2
}

void test_manual_switch_tracked_after_switch_timeout(void) {
    inputs.led_raw_state = 1;
    kvm_logic_step(&logic, &inputs, 0, &outputs);
    uint64_t ms = config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    kvm_logic_request_switch(&logic, 2);

    ms += 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_LED

    // Timeout
    ms += config.switch_led_wait_timeout_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // REVERT_SETUP

    // Wait revert setup
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // IDLE

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);

    // Delayed LED change
    inputs.led_raw_state = 2;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state);
}

void test_duplicate_switch_requests_rejected(void) {
    inputs.led_raw_state = 1;
    kvm_logic_step(&logic, &inputs, 0, &outputs);
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms, &outputs);

    kvm_result_t res1 = kvm_logic_request_switch(&logic, 2);
    TEST_ASSERT_EQUAL(KVM_RESULT_OK, res1);

    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 1, &outputs); // SETUP

    kvm_result_t res2 = kvm_logic_request_switch(&logic, 3);
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_SWITCHING, res2); // Or ERR_LOCKED
}

int main(void) { UNITY_BEGIN(); RUN_TEST(test_startup_led_stabilization); RUN_TEST(test_unknown_state_on_invalid_led); RUN_TEST(test_switch_interrupted_by_unexpected_led_change); RUN_TEST(test_manual_switch_tracked_after_switch_timeout); RUN_TEST(test_duplicate_switch_requests_rejected); return UNITY_END(); }
