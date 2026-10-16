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
    inputs.led_raw_state = 1;
    inputs.video_sync = KVM_VIDEO_SYNC_LOCKED;

    // Stabilize LED to channel 1
    kvm_logic_step(&logic, &inputs, 0, &outputs);
    kvm_logic_step(&logic, &inputs, config.led_stable_time_ms, &outputs);
}

void tearDown(void) {
}

void test_switch_normal_success(void) {
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);

    kvm_result_t res = kvm_logic_request_switch(&logic, 2);
    TEST_ASSERT_EQUAL(KVM_RESULT_OK, res);

    // Transition to setup wait
    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_SETUP, logic.state);
    TEST_ASSERT_TRUE(outputs.atx_locked);
    TEST_ASSERT_FALSE(outputs.e_kvm_low); // Not low yet
    TEST_ASSERT_EQUAL(2, outputs.a0a1_channel);

    // Wait setup time
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_PULSE, logic.state);
    TEST_ASSERT_TRUE(outputs.e_kvm_low); // E low

    // Wait KVM pulse
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_LED, logic.state);
    TEST_ASSERT_FALSE(outputs.e_kvm_low); // E high again

    // Change LED
    inputs.led_raw_state = 2;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    // Wait LED stabilization
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, logic.state);

    // Change video sync
    inputs.video_sync = KVM_VIDEO_SYNC_NO_SIGNAL; // or sync unknown, then locked
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

    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    // Still in idle, didn't do anything
    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
}

void test_switch_led_timeout_reverts_a0a1(void) {
    kvm_logic_request_switch(&logic, 2);

    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_LED

    // Don't change LED. Wait for timeout.
    ms += config.switch_led_wait_timeout_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_REVERT_SETUP, logic.state);
    TEST_ASSERT_EQUAL(1, outputs.a0a1_channel);
    TEST_ASSERT_FALSE(outputs.e_power_low);
    TEST_ASSERT_FALSE(outputs.e_reset_low);
    TEST_ASSERT_FALSE(outputs.e_kvm_low);

    // Setup time passed
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
    TEST_ASSERT_EQUAL(1, outputs.active_channel);
}

void test_switch_video_sync_timeout_releases_atx_lock(void) {
    kvm_logic_request_switch(&logic, 2);

    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_LED

    inputs.led_raw_state = 2;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // register change

    ms += config.led_stable_time_ms;
    inputs.video_sync = KVM_VIDEO_SYNC_UNKNOWN; // simulate no immediate sync
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_VIDEO
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, logic.state);

    ms += config.switch_video_sync_timeout_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // Now wait timeout

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
}

void test_switch_video_no_signal_releases_atx_lock(void) {
    kvm_logic_request_switch(&logic, 2);

    uint64_t ms = config.led_stable_time_ms + 1;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // SETUP
    ms += config.setup_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // PULSE
    ms += config.switch_kvm_pulse_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_LED

    inputs.led_raw_state = 2;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // register change

    ms += config.led_stable_time_ms;
    inputs.video_sync = KVM_VIDEO_SYNC_UNKNOWN;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // WAIT_VIDEO
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, logic.state);

    inputs.video_sync = KVM_VIDEO_SYNC_NO_SIGNAL;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // immediately transitions

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
}

void test_manual_switch_tracking(void) {
    uint64_t ms = config.led_stable_time_ms + 1;

    // Change LED manually
    inputs.led_raw_state = 3;
    kvm_logic_step(&logic, &inputs, ms, &outputs); // register change

    // Should immediately go to MANUAL_TRACK_WAIT_LED and lock ATX
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_LED, logic.state);
    TEST_ASSERT_TRUE(outputs.atx_locked);

    // Wait stabilization
    ms += config.led_stable_time_ms;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    ms += config.setup_time_ms;
    inputs.video_sync = KVM_VIDEO_SYNC_UNKNOWN;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(3, outputs.a0a1_channel);

    inputs.video_sync = KVM_VIDEO_SYNC_LOCKED;
    ms += 10;
    kvm_logic_step(&logic, &inputs, ms, &outputs);

    TEST_ASSERT_EQUAL(KVM_STATE_IDLE, logic.state);
    TEST_ASSERT_FALSE(outputs.atx_locked);
}

int main(void) { UNITY_BEGIN(); RUN_TEST(test_switch_normal_success); RUN_TEST(test_switch_to_current_channel_is_ignored); RUN_TEST(test_switch_led_timeout_reverts_a0a1); RUN_TEST(test_switch_video_sync_timeout_releases_atx_lock); RUN_TEST(test_switch_video_no_signal_releases_atx_lock); RUN_TEST(test_manual_switch_tracking); return UNITY_END(); }
