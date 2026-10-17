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

void test_atx_normal_pulse(void) {
    uint64_t ms = config.led_stable_time_ms;
    kvm_result_t res = kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER, 300);
    TEST_ASSERT_EQUAL(KVM_RESULT_OK, res);

    // Step to setup
    ms += 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_ATX_WAIT_SETUP, logic.state);

    // Wait setup
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_ATX_PULSE, logic.state);
    TEST_ASSERT_TRUE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.atx_anomaly_detected);

    // End pulse
    ms += 300;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.atx_anomaly_detected);
}

void test_atx_rejected_on_channel_mismatch(void) {
    kvm_result_t res = kvm_logic_request_atx(&logic, 2, KVM_ATX_TARGET_POWER, 300);
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_CHANNEL_MISMATCH, res);

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
}

void test_atx_rejected_when_locked_or_switching(void) {
    kvm_logic_request_switch(&logic, 2);

    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SWITCH_WAIT_SETUP

    kvm_result_t res = kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER, 300);
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_SWITCHING, res); // or ERR_LOCKED
}

void test_atx_rejected_when_unknown(void) {
    uint64_t ms = config.led_stable_time_ms + 1;
    inputs.led_raw_state = 0; // Unknown
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_UNKNOWN, logic.state);

    kvm_result_t res = kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER, 300);
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_UNKNOWN_STATE, res);
}

void test_atx_invalid_targets(void) {
    // Attempt NONE
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_INVALID_PARAM, kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_NONE, 100));
    // Attempt unknown integer cast
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_INVALID_PARAM, kvm_logic_request_atx(&logic, 1, (kvm_atx_target_t)99, 100));

    // Attempt normal should succeed
    TEST_ASSERT_EQUAL(KVM_RESULT_OK, kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER_FORCE, 100));
}

void test_atx_pulse_length_clamped(void) {
    uint64_t ms = config.led_stable_time_ms;
    kvm_result_t res = kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER, 1000); // Exceeds 500 max
    TEST_ASSERT_EQUAL(KVM_RESULT_OK, res);

    ms += 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE

    ms += config.max_power_pulse_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Should finish at max_power_pulse_ms

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.e_power_low);
}

void test_atx_aborted_and_anomaly_recorded_if_led_changes_during_pulse(void) {
    uint64_t ms = config.led_stable_time_ms;
    kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER, 300);

    ms += 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE

    // LED changes during pulse
    inputs.led_raw_state = 2;
    ms += 100;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    // Should immediately abort pulse and record anomaly
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_TRUE(outputs.atx_anomaly_detected);

    // Should transition to manual tracking for the new LED
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state);
}

void test_atx_anomaly_on_pulse_start_or_end(void) {
    uint64_t ms = config.led_stable_time_ms;
    kvm_logic_request_atx(&logic, 1, KVM_ATX_TARGET_POWER, 300);

    ms += 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP

    // Change LED right before pulse
    inputs.led_raw_state = 2;
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Should abort!

    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_TRUE(outputs.atx_anomaly_detected);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_atx_normal_pulse);
    RUN_TEST(test_atx_rejected_on_channel_mismatch);
    RUN_TEST(test_atx_rejected_when_locked_or_switching);
    RUN_TEST(test_atx_rejected_when_unknown);
    RUN_TEST(test_atx_invalid_targets);
    RUN_TEST(test_atx_pulse_length_clamped);
    RUN_TEST(test_atx_aborted_and_anomaly_recorded_if_led_changes_during_pulse);
    RUN_TEST(test_atx_anomaly_on_pulse_start_or_end);
    return UNITY_END();
}
