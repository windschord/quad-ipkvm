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

    kvm_logic_step(&logic, &inputs, 0, &outputs);
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms, &outputs);
}
void tearDown(void) {}

void test_switch_normal_success(void) {
    kvm_logic_request_switch(&logic, 2);

    uint64_t ms = config.led_stable_time_ms;

    ms += 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_SETUP, logic.state);
    TEST_ASSERT_TRUE(outputs.atx_locked);
    TEST_ASSERT_EQUAL(2, outputs.a0a1_channel);

    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_PULSE, logic.state);
    TEST_ASSERT_TRUE(outputs.e_kvm_low);

    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_LED, logic.state);
    TEST_ASSERT_FALSE(outputs.e_kvm_low);

    inputs.led_raw_state = 2;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, logic.state);

    inputs.video_sync = KVM_VIDEO_SYNC_NO_SIGNAL;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    inputs.video_sync = KVM_VIDEO_SYNC_LOCKED;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
    TEST_ASSERT_EQUAL(2, outputs.active_channel);
}

void test_switch_to_current_channel_is_ignored(void) {
    kvm_result_t res = kvm_logic_request_switch(&logic, 1);
    TEST_ASSERT_EQUAL(KVM_RESULT_OK, res);
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);

    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms + 10, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
}

void test_switch_led_timeout_reverts_a0a1(void) {
    kvm_logic_request_switch(&logic, 2);
    uint64_t ms = config.led_stable_time_ms;

    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_LED, logic.state);

    ms += config.switch_led_wait_timeout_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_REVERT_SETUP, logic.state);
    TEST_ASSERT_EQUAL(1, outputs.a0a1_channel);
    TEST_ASSERT_FALSE(outputs.e_kvm_low);

    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
    TEST_ASSERT_EQUAL(1, outputs.active_channel);
}

void test_switch_video_sync_timeout_releases_atx_lock(void) {
    kvm_logic_request_switch(&logic, 2);
    uint64_t ms = config.led_stable_time_ms;

    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    inputs.led_raw_state = 2;
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // LED tracks 2
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // stable
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, logic.state);

    inputs.video_sync = KVM_VIDEO_SYNC_NO_SIGNAL;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    ms += config.switch_video_sync_timeout_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
    TEST_ASSERT_EQUAL(2, outputs.active_channel);
}

void test_switch_video_no_signal_releases_atx_lock(void) {
    // Start with a long 60 sec period of NO_SIGNAL before switching
    uint64_t ms = config.led_stable_time_ms;

    inputs.video_sync = KVM_VIDEO_SYNC_NO_SIGNAL;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += 60000;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    // Now switch
    kvm_logic_request_switch(&logic, 2);

    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    inputs.led_raw_state = 2;
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_VIDEO reached
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, logic.state);

    // At this point NO_SIGNAL has been active for 60 seconds. But we ONLY want to consider it settled if it stays active for `no_signal_settle_ms` MORE from WAIT_VIDEO entry time.
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, logic.state); // Not yet

    ms += config.no_signal_settle_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Now it should settle

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
    TEST_ASSERT_EQUAL(2, outputs.active_channel);
}

void test_manual_switch_tracking(void) {
    uint64_t ms = config.led_stable_time_ms;

    // Have a long NO_SIGNAL period beforehand
    inputs.video_sync = KVM_VIDEO_SYNC_NO_SIGNAL;
    ms += 60000;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    inputs.led_raw_state = 2;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state);
    TEST_ASSERT_TRUE(outputs.atx_locked);

    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_VIDEO, logic.state);
    TEST_ASSERT_EQUAL(2, outputs.a0a1_channel);

    // It should wait setup time + settle time from NO SIGNAL BEFORE unlocking!
    ms += config.setup_time_ms + 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_VIDEO, logic.state);

    ms += config.no_signal_settle_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
    TEST_ASSERT_EQUAL(2, outputs.active_channel);
}

void test_startup_led_stabilization(void) {
    kvm_logic_t logic2;
    kvm_logic_init(&logic2, &config);
    kvm_logic_outputs_t out2;

    inputs.led_raw_state = 3;
    kvm_logic_step(&logic2, &inputs, 0, &out2);
    TEST_ASSERT_EQUAL(KVM_STATE_UNKNOWN, logic2.state);

    kvm_logic_step(&logic2, &inputs, config.led_stable_time_ms, &out2);
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic2.state);
    TEST_ASSERT_EQUAL(3, out2.active_channel);
    TEST_ASSERT_EQUAL(3, out2.a0a1_channel);
}

void test_duplicate_switch_requests_rejected(void) {
    kvm_logic_request_switch(&logic, 2);
    kvm_result_t res = kvm_logic_request_switch(&logic, 3);
    TEST_ASSERT_EQUAL(KVM_RESULT_ERR_SWITCHING, res);
}

void test_switch_interrupted_by_unexpected_led_change(void) {
    kvm_logic_request_switch(&logic, 2);
    uint64_t ms = config.led_stable_time_ms;

    // Process step for request
    kvm_logic_step(&logic, &inputs, ms, &outputs); // state entered becomes true

    ms += config.setup_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Switch wait pulse
    ms += config.switch_kvm_pulse_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Switch wait LED

    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_LED, logic.state);

    inputs.led_raw_state = 3;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state);
}

void test_manual_switch_tracked_after_switch_timeout(void) {
    kvm_logic_request_switch(&logic, 2);
    uint64_t ms = config.led_stable_time_ms;

    // Request setup
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    ms += config.setup_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += config.switch_kvm_pulse_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    ms += config.switch_led_wait_timeout_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_REVERT_SETUP, logic.state);

    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);

    inputs.led_raw_state = 2;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state);
}

void test_idle_retracks_if_led_changes(void) {
    // Tests that if IDLE detects an unexpected LED change or unmatching state, it re-tracks
    uint64_t ms = config.led_stable_time_ms;

    inputs.led_raw_state = 3;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Wait LED tracking...
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Wait video...
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    ms += 10;
    inputs.video_sync = KVM_VIDEO_SYNC_LOCKED;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Back to IDLE

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);

    // Now abruptly change led
    inputs.led_raw_state = 4;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_switch_normal_success);
    RUN_TEST(test_switch_to_current_channel_is_ignored);
    RUN_TEST(test_switch_led_timeout_reverts_a0a1);
    RUN_TEST(test_switch_video_sync_timeout_releases_atx_lock);
    RUN_TEST(test_switch_video_no_signal_releases_atx_lock);
    RUN_TEST(test_manual_switch_tracking);
    RUN_TEST(test_startup_led_stabilization);
    RUN_TEST(test_duplicate_switch_requests_rejected);
    RUN_TEST(test_switch_interrupted_by_unexpected_led_change);
    RUN_TEST(test_manual_switch_tracked_after_switch_timeout);
    RUN_TEST(test_idle_retracks_if_led_changes);
    return UNITY_END();
}
