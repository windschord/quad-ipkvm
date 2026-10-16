#include "kvm_logic.h"
#include <string.h>

int kvm_logic_dummy(void) { return 42; }

void kvm_logic_init(kvm_logic_t *logic, const kvm_logic_config_t *config) {
    memset(logic, 0, sizeof(kvm_logic_t));
    if (config) {
        logic->config = *config;
    }
    logic->state = KVM_STATE_UNKNOWN;
    logic->current_outputs.a0a1_channel = 1;
    logic->current_outputs.e_power_low = false;
    logic->current_outputs.e_reset_low = false;
    logic->current_outputs.e_kvm_low = false;
    logic->current_outputs.atx_locked = true;
    logic->target_channel = 0;
}

kvm_result_t kvm_logic_request_switch(kvm_logic_t *logic, kvm_channel_t channel) {
    if (channel < 1 || channel > 4) return KVM_RESULT_ERR_INVALID_PARAM;

    if (logic->state == KVM_STATE_IDLE) {
        if (channel == logic->current_outputs.active_channel) {
            return KVM_RESULT_OK;
        }
        logic->state = KVM_STATE_SWITCH_WAIT_SETUP;
        logic->state_start_ms = 0; // will be set in step
        logic->target_channel = channel;
        return KVM_RESULT_OK;
    }

    return KVM_RESULT_ERR_SWITCHING;
}

kvm_result_t kvm_logic_request_atx(kvm_logic_t *logic, kvm_channel_t channel, kvm_atx_target_t target, uint32_t pulse_ms) {
    if (logic->state == KVM_STATE_UNKNOWN) return KVM_RESULT_ERR_UNKNOWN_STATE;
    if (logic->state != KVM_STATE_IDLE) return KVM_RESULT_ERR_SWITCHING;
    if (channel != logic->current_outputs.active_channel) return KVM_RESULT_ERR_CHANNEL_MISMATCH;
    if (channel < 1 || channel > 4) return KVM_RESULT_ERR_INVALID_PARAM;
    if (logic->current_outputs.atx_locked) return KVM_RESULT_ERR_LOCKED;

    logic->state = KVM_STATE_ATX_WAIT_SETUP;
    logic->state_start_ms = 0;
    logic->target_channel = channel;
    logic->atx_target = target;

    uint32_t max_ms = 0;
    if (target == KVM_ATX_TARGET_POWER) {
        max_ms = logic->config.max_power_pulse_ms;
        if (pulse_ms > logic->config.max_power_pulse_ms) {
            if (pulse_ms >= logic->config.max_force_power_pulse_ms) {
                max_ms = logic->config.max_force_power_pulse_ms;
            }
        }
    } else if (target == KVM_ATX_TARGET_RESET) {
        max_ms = logic->config.max_reset_pulse_ms;
    }

    if (pulse_ms > max_ms) {
        logic->atx_requested_pulse_ms = max_ms;
    } else {
        logic->atx_requested_pulse_ms = pulse_ms;
    }

    return KVM_RESULT_OK;
}

static bool is_valid_channel(uint8_t raw) {
    return (raw >= 1 && raw <= 4);
}

void kvm_logic_step(kvm_logic_t *logic, const kvm_logic_inputs_t *inputs, uint64_t now_ms, kvm_logic_outputs_t *outputs) {
    if (logic->state_start_ms == 0 && logic->state != KVM_STATE_UNKNOWN) {
        logic->state_start_ms = now_ms;
    }

    if (inputs->led_raw_state != logic->last_raw_led) {
        logic->last_raw_led = inputs->led_raw_state;
        logic->led_stable_start_ms = now_ms;
    }

    // Unsigned wrap-around safe subtraction
    uint64_t led_elapsed = now_ms - logic->led_stable_start_ms;
    bool led_stable = led_elapsed >= logic->config.led_stable_time_ms;
    if (led_stable && logic->last_stable_led != logic->last_raw_led) {
        logic->last_stable_led = logic->last_raw_led;
    }

    // Instantaneous ATX anomaly detection
    if (logic->state == KVM_STATE_ATX_PULSE || logic->state == KVM_STATE_ATX_WAIT_SETUP) {
        if (inputs->led_raw_state != logic->target_channel || !is_valid_channel(inputs->led_raw_state)) {
            logic->current_outputs.e_power_low = false;
            logic->current_outputs.e_reset_low = false;
            logic->current_outputs.atx_anomaly_detected = true;
            logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
            logic->state_start_ms = now_ms;
        }
    }

    if (!is_valid_channel(inputs->led_raw_state)) {
        if (logic->state != KVM_STATE_UNKNOWN) {
            logic->state = KVM_STATE_UNKNOWN;
            logic->state_start_ms = now_ms;
            logic->current_outputs.atx_locked = true;
        }
    }

    if (logic->state == KVM_STATE_SWITCH_WAIT_LED) {
        if (inputs->led_raw_state != logic->target_channel && inputs->led_raw_state != logic->current_outputs.active_channel && is_valid_channel(inputs->led_raw_state)) {
            logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
            logic->state_start_ms = now_ms;
        }
    }

    if (logic->state == KVM_STATE_IDLE && inputs->led_raw_state != logic->current_outputs.active_channel && is_valid_channel(inputs->led_raw_state)) {
        logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
        logic->state_start_ms = now_ms;
        logic->current_outputs.atx_locked = true;
    }

    if (logic->state_start_ms == 0) {
        logic->state_start_ms = now_ms;
    }

    uint64_t state_elapsed = now_ms - logic->state_start_ms;

    if (logic->state != KVM_STATE_IDLE && logic->state != KVM_STATE_ATX_WAIT_SETUP && logic->state != KVM_STATE_ATX_PULSE) {
        logic->current_outputs.atx_locked = true;
    }

    switch (logic->state) {
        case KVM_STATE_UNKNOWN:
            logic->current_outputs.atx_locked = true;
            logic->current_outputs.e_kvm_low = false;
            logic->current_outputs.e_power_low = false;
            logic->current_outputs.e_reset_low = false;

            if (led_stable && is_valid_channel(logic->last_stable_led)) {
                logic->state = KVM_STATE_IDLE;
                logic->state_start_ms = now_ms;
                logic->current_outputs.active_channel = logic->last_stable_led;
                logic->current_outputs.a0a1_channel = logic->last_stable_led;
                logic->current_outputs.atx_locked = false;
            }
            break;

        case KVM_STATE_IDLE:
            if (led_stable && !is_valid_channel(logic->last_stable_led)) {
                logic->state = KVM_STATE_UNKNOWN;
                logic->state_start_ms = now_ms;
                logic->current_outputs.atx_locked = true;
            } else if (led_stable && logic->last_stable_led != logic->current_outputs.active_channel) {
                logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                logic->state_start_ms = now_ms;
                logic->current_outputs.atx_locked = true;
            }
            break;

        case KVM_STATE_SWITCH_WAIT_SETUP:
            logic->current_outputs.atx_locked = true;
            logic->current_outputs.a0a1_channel = logic->target_channel;
            logic->current_outputs.e_kvm_low = false;
            logic->current_outputs.e_power_low = false;
            logic->current_outputs.e_reset_low = false;

            if (state_elapsed >= logic->config.setup_time_ms) {
                logic->state = KVM_STATE_SWITCH_PULSE;
                logic->state_start_ms = now_ms;
                logic->current_outputs.e_kvm_low = true;
            }
            break;

        case KVM_STATE_SWITCH_PULSE:
            logic->current_outputs.e_kvm_low = true;
            if (state_elapsed >= logic->config.switch_kvm_pulse_time_ms) {
                logic->current_outputs.e_kvm_low = false;
                logic->state = KVM_STATE_SWITCH_WAIT_LED;
                logic->state_start_ms = now_ms;
            }
            break;

        case KVM_STATE_SWITCH_WAIT_LED:
            if (led_stable && logic->last_stable_led == logic->target_channel) {
                logic->state = KVM_STATE_SWITCH_WAIT_VIDEO;
                logic->state_start_ms = now_ms;
            } else if (state_elapsed >= logic->config.switch_led_wait_timeout_ms) {
                logic->state = KVM_STATE_SWITCH_REVERT_SETUP;
                logic->state_start_ms = now_ms;
                logic->target_channel = logic->current_outputs.active_channel;
                logic->current_outputs.a0a1_channel = logic->target_channel;
            }
            break;

        case KVM_STATE_SWITCH_REVERT_SETUP:
            logic->current_outputs.a0a1_channel = logic->target_channel;
            if (state_elapsed >= logic->config.setup_time_ms) {
                logic->state = KVM_STATE_IDLE;
                logic->state_start_ms = now_ms;
                logic->current_outputs.active_channel = logic->target_channel;
                logic->current_outputs.atx_locked = false;
            }
            break;

        case KVM_STATE_SWITCH_WAIT_VIDEO:
            if (inputs->video_sync == KVM_VIDEO_SYNC_LOCKED || inputs->video_sync == KVM_VIDEO_SYNC_NO_SIGNAL ||
                state_elapsed >= logic->config.switch_video_sync_timeout_ms) {
                logic->state = KVM_STATE_IDLE;
                logic->state_start_ms = now_ms;
                logic->current_outputs.active_channel = logic->target_channel;
                logic->current_outputs.a0a1_channel = logic->target_channel;
                logic->current_outputs.atx_locked = false;
            } else if (led_stable && logic->last_stable_led != logic->target_channel) {
                logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                logic->state_start_ms = now_ms;
            }
            break;

        case KVM_STATE_MANUAL_TRACK_WAIT_LED:
            logic->current_outputs.atx_locked = true;
            if (led_stable && is_valid_channel(logic->last_stable_led)) {
                logic->state = KVM_STATE_MANUAL_TRACK_WAIT_VIDEO;
                logic->state_start_ms = now_ms;
                logic->target_channel = logic->last_stable_led;
                logic->current_outputs.a0a1_channel = logic->last_stable_led;
            }
            break;

        case KVM_STATE_MANUAL_TRACK_WAIT_VIDEO:
            if (state_elapsed >= logic->config.setup_time_ms) {
                if (inputs->video_sync == KVM_VIDEO_SYNC_LOCKED || inputs->video_sync == KVM_VIDEO_SYNC_NO_SIGNAL ||
                    (state_elapsed - logic->config.setup_time_ms) >= logic->config.switch_video_sync_timeout_ms) {
                    logic->state = KVM_STATE_IDLE;
                    logic->state_start_ms = now_ms;
                    logic->current_outputs.active_channel = logic->last_stable_led;
                    logic->current_outputs.atx_locked = false;
                } else if (led_stable && logic->last_stable_led != logic->target_channel) {
                    logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                    logic->state_start_ms = now_ms;
                }
            }
            break;

        case KVM_STATE_ATX_WAIT_SETUP:
            if (state_elapsed >= logic->config.setup_time_ms) {
                logic->state = KVM_STATE_ATX_PULSE;
                logic->state_start_ms = now_ms;
                logic->current_outputs.atx_anomaly_detected = false;
                if (logic->atx_target == KVM_ATX_TARGET_POWER) logic->current_outputs.e_power_low = true;
                if (logic->atx_target == KVM_ATX_TARGET_RESET) logic->current_outputs.e_reset_low = true;
            }
            break;

        case KVM_STATE_ATX_PULSE:
            if (logic->atx_target == KVM_ATX_TARGET_POWER) logic->current_outputs.e_power_low = true;
            if (logic->atx_target == KVM_ATX_TARGET_RESET) logic->current_outputs.e_reset_low = true;

            if (state_elapsed >= logic->atx_requested_pulse_ms) {
                logic->current_outputs.e_power_low = false;
                logic->current_outputs.e_reset_low = false;
                logic->state = KVM_STATE_IDLE;
                logic->state_start_ms = now_ms;
            }
            break;
    }

    *outputs = logic->current_outputs;
}
