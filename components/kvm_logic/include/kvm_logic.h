#ifndef KVM_LOGIC_H
#define KVM_LOGIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Video Sync States */
typedef enum {
    KVM_VIDEO_SYNC_UNKNOWN = 0,
    KVM_VIDEO_SYNC_NO_SIGNAL,
    KVM_VIDEO_SYNC_LOCKED
} kvm_video_sync_state_t;

/* Channel definition. 1-4 for valid channels, 0 for unknown/invalid */
typedef uint8_t kvm_channel_t;

/* ATX Target definition */
typedef enum {
    KVM_ATX_TARGET_POWER = 0,
    KVM_ATX_TARGET_RESET,
    KVM_ATX_TARGET_NONE
} kvm_atx_target_t;

/* Result Codes */
typedef enum {
    KVM_RESULT_OK = 0,
    KVM_RESULT_ERR_CHANNEL_MISMATCH = -1,
    KVM_RESULT_ERR_LOCKED = -2,
    KVM_RESULT_ERR_UNKNOWN_STATE = -3,
    KVM_RESULT_ERR_SWITCHING = -4,
    KVM_RESULT_ERR_INVALID_PARAM = -5
} kvm_result_t;

/* Configuration Parameters (with default suggestions in comments) */
typedef struct {
    uint32_t led_stable_time_ms;         /* TODO(hw-verify): Time to wait for LED debounce */
    uint32_t switch_kvm_pulse_time_ms;   /* TODO(hw-verify): Length of KVM switch button press (100-200ms) */
    uint32_t switch_led_wait_timeout_ms; /* TODO(hw-verify): Timeout waiting for LED after button press */
    uint32_t switch_video_sync_timeout_ms;/* TODO(hw-verify): Timeout waiting for video sync after LED change */
    uint32_t setup_time_ms;              /* TODO(hw-verify): Setup time for A0/A1 before E goes low */

    /* Max pulse lengths for ATX operations */
    uint32_t max_power_pulse_ms;         /* TODO(hw-verify): 200-500ms max */
    uint32_t max_reset_pulse_ms;         /* TODO(hw-verify): 200ms max */
    uint32_t max_force_power_pulse_ms;   /* TODO(hw-verify): ~5000ms max */
} kvm_logic_config_t;

/* Inputs to the State Machine */
typedef struct {
    uint8_t led_raw_state;
    kvm_video_sync_state_t video_sync;
} kvm_logic_inputs_t;

/* Outputs from the State Machine */
typedef struct {
    kvm_channel_t a0a1_channel;

    bool e_power_low;
    bool e_reset_low;
    bool e_kvm_low;

    bool atx_locked;
    bool atx_anomaly_detected;

    kvm_channel_t active_channel;
} kvm_logic_outputs_t;


/* Internal State Enums */
typedef enum {
    KVM_STATE_UNKNOWN = 0,
    KVM_STATE_IDLE,
    KVM_STATE_SWITCH_WAIT_SETUP,
    KVM_STATE_SWITCH_PULSE,
    KVM_STATE_SWITCH_WAIT_LED,
    KVM_STATE_SWITCH_WAIT_VIDEO,
    KVM_STATE_SWITCH_REVERT_SETUP,
    KVM_STATE_MANUAL_TRACK_WAIT_LED,
    KVM_STATE_MANUAL_TRACK_WAIT_VIDEO,
    KVM_STATE_ATX_WAIT_SETUP,
    KVM_STATE_ATX_PULSE
} kvm_logic_state_t;


/* State Machine Context */
typedef struct {
    kvm_logic_config_t config;

    kvm_logic_state_t state;

    /* Timers & history */
    uint64_t state_start_ms;

    kvm_channel_t target_channel;
    kvm_channel_t last_stable_led;
    uint8_t last_raw_led;
    uint64_t led_stable_start_ms;

    /* ATX operation state */
    kvm_atx_target_t atx_target;
    uint32_t atx_requested_pulse_ms;

    /* Output state cache to avoid emitting redundant commands */
    kvm_logic_outputs_t current_outputs;

} kvm_logic_t;


/* API */

/**
 * @brief Initialize the KVM logic state machine.
 */
void kvm_logic_init(kvm_logic_t *logic, const kvm_logic_config_t *config);

/**
 * @brief Request a KVM channel switch.
 * @return KVM_RESULT_OK if accepted, or error code (e.g. KVM_RESULT_ERR_SWITCHING).
 */
kvm_result_t kvm_logic_request_switch(kvm_logic_t *logic, kvm_channel_t channel);

/**
 * @brief Request an ATX operation.
 * @param channel The channel the caller thinks is active. Will be rejected if it mismatches the current active channel.
 * @param target POWER or RESET.
 * @param pulse_ms Requested pulse duration in ms (will be clamped by max limits).
 * @return KVM_RESULT_OK if accepted, or error code (mismatch, locked, etc).
 */
kvm_result_t kvm_logic_request_atx(kvm_logic_t *logic, kvm_channel_t channel, kvm_atx_target_t target, uint32_t pulse_ms);

/**
 * @brief Advance the state machine by one tick. Pure function, no side effects outside `outputs`.
 * @param logic The state machine context.
 * @param inputs Current hardware inputs (raw LED state, video sync).
 * @param now_ms Current monotonic time in milliseconds.
 * @param outputs Populated with the desired hardware outputs.
 */
void kvm_logic_step(kvm_logic_t *logic, const kvm_logic_inputs_t *inputs, uint64_t now_ms, kvm_logic_outputs_t *outputs);

int kvm_logic_dummy(void);

#ifdef __cplusplus
}
#endif

#endif // KVM_LOGIC_H
