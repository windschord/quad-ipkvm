#include "kvm_logic.h"
#include <string.h>

int kvm_logic_dummy(void) { return 42; }

void kvm_logic_config_default(kvm_logic_config_t *config) {
    if (!config) return;
    config->led_stable_time_ms = 50;           /* TODO(hw-verify) */
    config->switch_kvm_pulse_time_ms = 150;    /* TODO(hw-verify) */
    config->switch_led_wait_timeout_ms = 2000; /* TODO(hw-verify) */
    config->switch_video_sync_timeout_ms = 3000;/* TODO(hw-verify) */
    config->no_signal_settle_ms = 500;         /* TODO(hw-verify) */
    config->setup_time_ms = 10;                /* TODO(hw-verify) */
    config->max_power_pulse_ms = KVM_ATX_HARD_MAX_POWER_MS;   /* TODO(hw-verify) */
    config->max_reset_pulse_ms = KVM_ATX_HARD_MAX_RESET_MS;   /* TODO(hw-verify) */
    config->max_force_power_pulse_ms = KVM_ATX_HARD_MAX_FORCE_MS; /* TODO(hw-verify) */
}

kvm_result_t kvm_logic_init(kvm_logic_t *logic, const kvm_logic_config_t *config) {
    memset(logic, 0, sizeof(kvm_logic_t));
    logic->state = KVM_STATE_UNKNOWN;
    logic->current_outputs.a0a1_channel = 1;
    logic->current_outputs.e_power_low = false;
    logic->current_outputs.e_reset_low = false;
    logic->current_outputs.e_kvm_low = false;
    logic->current_outputs.atx_locked = true;
    logic->target_channel = 0;

    if (!config) return KVM_RESULT_ERR_INVALID_PARAM;
    if (config->setup_time_ms == 0 || config->led_stable_time_ms == 0 ||
        config->switch_kvm_pulse_time_ms == 0 || config->switch_led_wait_timeout_ms == 0 ||
        config->switch_video_sync_timeout_ms == 0 || config->max_power_pulse_ms == 0 ||
        config->max_reset_pulse_ms == 0 || config->max_force_power_pulse_ms == 0) {
        return KVM_RESULT_ERR_INVALID_PARAM;
    }

    logic->config = *config;

    /* Clamp to hard limits */
    if (logic->config.max_power_pulse_ms > KVM_ATX_HARD_MAX_POWER_MS) {
        logic->config.max_power_pulse_ms = KVM_ATX_HARD_MAX_POWER_MS;
    }
    if (logic->config.max_reset_pulse_ms > KVM_ATX_HARD_MAX_RESET_MS) {
        logic->config.max_reset_pulse_ms = KVM_ATX_HARD_MAX_RESET_MS;
    }
    if (logic->config.max_force_power_pulse_ms > KVM_ATX_HARD_MAX_FORCE_MS) {
        logic->config.max_force_power_pulse_ms = KVM_ATX_HARD_MAX_FORCE_MS;
    }

    return KVM_RESULT_OK;
}

kvm_result_t kvm_logic_request_switch(kvm_logic_t *logic, kvm_channel_t channel) {
    if (channel < 1 || channel > 4) return KVM_RESULT_ERR_INVALID_PARAM;

    if (logic->state == KVM_STATE_IDLE) {
        if (channel == logic->current_outputs.active_channel) {
            return KVM_RESULT_OK;
        }
        logic->state = KVM_STATE_SWITCH_WAIT_SETUP;
        logic->state_entered = false;
        logic->target_channel = channel;
        return KVM_RESULT_OK;
    }

    return KVM_RESULT_ERR_SWITCHING;
}

kvm_result_t kvm_logic_request_atx(kvm_logic_t *logic, kvm_channel_t channel, kvm_atx_target_t target, uint32_t pulse_ms) {
    if (channel < 1 || channel > 4) return KVM_RESULT_ERR_INVALID_PARAM;
    if (pulse_ms == 0) return KVM_RESULT_ERR_INVALID_PARAM;
    if (logic->state == KVM_STATE_UNKNOWN) return KVM_RESULT_ERR_UNKNOWN_STATE;
    if (logic->state != KVM_STATE_IDLE) return KVM_RESULT_ERR_SWITCHING;
    if (channel != logic->current_outputs.active_channel) return KVM_RESULT_ERR_CHANNEL_MISMATCH;
    if (logic->current_outputs.atx_locked) return KVM_RESULT_ERR_LOCKED;

    logic->state = KVM_STATE_ATX_WAIT_SETUP;
    logic->state_entered = false;
    logic->target_channel = channel;
    logic->atx_target = target;

    uint32_t max_ms = 0;
    if (target == KVM_ATX_TARGET_POWER) {
        max_ms = logic->config.max_power_pulse_ms;
    } else if (target == KVM_ATX_TARGET_RESET) {
        max_ms = logic->config.max_reset_pulse_ms;
    } else if (target == KVM_ATX_TARGET_POWER_FORCE) {
        max_ms = logic->config.max_force_power_pulse_ms;
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
    if (!logic->state_entered) {
        logic->state_start_ms = now_ms;
        logic->state_entered = true;
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

    if (inputs->video_sync == KVM_VIDEO_SYNC_NO_SIGNAL) {
        if (!logic->no_signal_active) {
            logic->no_signal_active = true;
            logic->no_signal_start_ms = now_ms;
        }
    } else {
        logic->no_signal_active = false;
    }

    // Instantaneous ATX anomaly detection
    if (logic->state == KVM_STATE_ATX_PULSE || logic->state == KVM_STATE_ATX_WAIT_SETUP) {
        if (inputs->led_raw_state != logic->target_channel || !is_valid_channel(inputs->led_raw_state)) {
            logic->current_outputs.e_power_low = false;
            logic->current_outputs.e_reset_low = false;
            logic->current_outputs.atx_anomaly_detected = true;
            logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
            logic->state_start_ms = now_ms;
            logic->state_entered = true;
        }
    }

    if (!is_valid_channel(inputs->led_raw_state)) {
        if (logic->state != KVM_STATE_UNKNOWN) {
            logic->state = KVM_STATE_UNKNOWN;
            logic->state_start_ms = now_ms;
            logic->state_entered = true;
            logic->current_outputs.atx_locked = true;
        }
    }

    if (logic->state == KVM_STATE_SWITCH_WAIT_LED) {
        if (inputs->led_raw_state != logic->target_channel && inputs->led_raw_state != logic->current_outputs.active_channel && is_valid_channel(inputs->led_raw_state)) {
            logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
            logic->state_start_ms = now_ms;
            logic->state_entered = true;
        }
    }

    if (logic->state == KVM_STATE_IDLE && inputs->led_raw_state != logic->current_outputs.active_channel && is_valid_channel(inputs->led_raw_state)) {
        logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
        logic->state_start_ms = now_ms;
        logic->state_entered = true;
        logic->current_outputs.atx_locked = true;
    }

    // Catch any changes to IDLE where A0/A1 mismatch happens
    if (logic->state == KVM_STATE_IDLE && logic->current_outputs.a0a1_channel != logic->current_outputs.active_channel) {
        logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
        logic->state_start_ms = now_ms;
        logic->state_entered = true;
        logic->current_outputs.atx_locked = true;
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
                logic->state_entered = true;
                logic->current_outputs.active_channel = logic->last_stable_led;
                logic->current_outputs.a0a1_channel = logic->last_stable_led;
                logic->current_outputs.atx_locked = false;
            }
            break;

        case KVM_STATE_IDLE:
            if (led_stable && !is_valid_channel(logic->last_stable_led)) {
                logic->state = KVM_STATE_UNKNOWN;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
                logic->current_outputs.atx_locked = true;
            } else if (led_stable && logic->last_stable_led != logic->current_outputs.active_channel) {
                logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
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
                logic->state_entered = true;
                logic->current_outputs.e_kvm_low = true;
            }
            break;

        case KVM_STATE_SWITCH_PULSE:
            logic->current_outputs.e_kvm_low = true;
            if (state_elapsed >= logic->config.switch_kvm_pulse_time_ms) {
                logic->current_outputs.e_kvm_low = false;
                logic->state = KVM_STATE_SWITCH_WAIT_LED;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
            }
            break;

        case KVM_STATE_SWITCH_WAIT_LED:
            if (led_stable && logic->last_stable_led == logic->target_channel) {
                logic->state = KVM_STATE_SWITCH_WAIT_VIDEO;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
            } else if (state_elapsed >= logic->config.switch_led_wait_timeout_ms) {
                logic->state = KVM_STATE_SWITCH_REVERT_SETUP;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
                logic->target_channel = logic->current_outputs.active_channel;
                logic->current_outputs.a0a1_channel = logic->target_channel;
            }
            break;

        case KVM_STATE_SWITCH_REVERT_SETUP:
            logic->current_outputs.a0a1_channel = logic->target_channel;
            if (state_elapsed >= logic->config.setup_time_ms) {
                logic->state = KVM_STATE_IDLE;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
                logic->current_outputs.active_channel = logic->target_channel;
                logic->current_outputs.atx_locked = false;
            }
            break;

        case KVM_STATE_SWITCH_WAIT_VIDEO:
            if (led_stable && logic->last_stable_led != logic->target_channel) {
                logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
            } else {
                bool no_signal_settled = (inputs->video_sync == KVM_VIDEO_SYNC_NO_SIGNAL && logic->no_signal_active &&
                                          (now_ms - logic->no_signal_start_ms) >= logic->config.no_signal_settle_ms);

                if (inputs->video_sync == KVM_VIDEO_SYNC_LOCKED || no_signal_settled ||
                    state_elapsed >= logic->config.switch_video_sync_timeout_ms) {

                    if (led_stable && logic->last_stable_led == logic->target_channel && logic->last_stable_led == logic->current_outputs.a0a1_channel) {
                        logic->state = KVM_STATE_IDLE;
                        logic->state_start_ms = now_ms;
                        logic->state_entered = true;
                        logic->current_outputs.active_channel = logic->target_channel;
                        logic->current_outputs.a0a1_channel = logic->target_channel;
                        logic->current_outputs.atx_locked = false;
                    } else {
                        logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                        logic->state_start_ms = now_ms;
                        logic->state_entered = true;
                    }
                }
            }
            break;

        case KVM_STATE_MANUAL_TRACK_WAIT_LED:
            logic->current_outputs.atx_locked = true;
            if (led_stable && is_valid_channel(logic->last_stable_led)) {
                logic->state = KVM_STATE_MANUAL_TRACK_WAIT_VIDEO;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
                logic->target_channel = logic->last_stable_led;
                logic->current_outputs.a0a1_channel = logic->last_stable_led;
            }
            break;

        case KVM_STATE_MANUAL_TRACK_WAIT_VIDEO:
            if (state_elapsed >= logic->config.setup_time_ms) {
                if (led_stable && logic->last_stable_led != logic->target_channel) {
                    logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                    logic->state_start_ms = now_ms;
                    logic->state_entered = true;
                } else {
                    bool no_signal_settled = (inputs->video_sync == KVM_VIDEO_SYNC_NO_SIGNAL && logic->no_signal_active &&
                                              (now_ms - logic->no_signal_start_ms) >= logic->config.no_signal_settle_ms);

                    if (inputs->video_sync == KVM_VIDEO_SYNC_LOCKED || no_signal_settled ||
                        (state_elapsed - logic->config.setup_time_ms) >= logic->config.switch_video_sync_timeout_ms) {

                        if (led_stable && logic->last_stable_led == logic->target_channel && logic->last_stable_led == logic->current_outputs.a0a1_channel) {
                            logic->state = KVM_STATE_IDLE;
                            logic->state_start_ms = now_ms;
                            logic->state_entered = true;
                            logic->current_outputs.active_channel = logic->last_stable_led;
                            logic->current_outputs.atx_locked = false;
                        } else {
                            logic->state = KVM_STATE_MANUAL_TRACK_WAIT_LED;
                            logic->state_start_ms = now_ms;
                            logic->state_entered = true;
                        }
                    }
                }
            }
            break;

        case KVM_STATE_ATX_WAIT_SETUP:
            if (state_elapsed >= logic->config.setup_time_ms) {
                logic->state = KVM_STATE_ATX_PULSE;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
                logic->current_outputs.atx_anomaly_detected = false;
                if (logic->atx_target == KVM_ATX_TARGET_POWER || logic->atx_target == KVM_ATX_TARGET_POWER_FORCE) logic->current_outputs.e_power_low = true;
                if (logic->atx_target == KVM_ATX_TARGET_RESET) logic->current_outputs.e_reset_low = true;
            }
            break;

        case KVM_STATE_ATX_PULSE:
            if (logic->atx_target == KVM_ATX_TARGET_POWER || logic->atx_target == KVM_ATX_TARGET_POWER_FORCE) logic->current_outputs.e_power_low = true;
            if (logic->atx_target == KVM_ATX_TARGET_RESET) logic->current_outputs.e_reset_low = true;

            if (state_elapsed >= logic->atx_requested_pulse_ms) {
                logic->current_outputs.e_power_low = false;
                logic->current_outputs.e_reset_low = false;
                logic->state = KVM_STATE_IDLE;
                logic->state_start_ms = now_ms;
                logic->state_entered = true;
            }
            break;
    }

    *outputs = logic->current_outputs;
}
