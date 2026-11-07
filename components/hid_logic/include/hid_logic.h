#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "hid_report.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HID_MAX_CONTROLLERS 1
#define HID_HOLD_TIMEOUT_MS 3000
#define HID_RATE_PER_SEC 400
#define HID_RATE_BURST 100
#define HID_ABUSE_WINDOW_MS 5000
#define HID_ABUSE_MAX_DROPS 1000
#define HID_MAX_KEYS 255
#define HID_REL_MAX_REPORTS_PER_CALL 8 /* relative reports sent per call; the rest waits for the next tick */

typedef enum {
    HID_RESULT_OK = 0,
    HID_RESULT_INVALID, // Invalid length, type, or usage
    HID_RESULT_RATE_LIMITED, // Dropped due to rate limiting
    HID_RESULT_NOT_OWNER, // Caller is not the current owner
    HID_RESULT_ABUSE, // Client abused the rate limit, should be disconnected
} hid_result_t;

typedef struct {
    uint32_t hold_timeout_ms;
    uint32_t rate_per_sec;
    uint32_t rate_burst;
} hid_logic_config_t;

typedef enum {
    HID_PTR_TYPE_NONE = 0,
    HID_PTR_TYPE_ABS,
    HID_PTR_TYPE_REL
} hid_ptr_type_t;

typedef struct {
    hid_logic_config_t cfg;
    hid_sink_t sink;

    int owner_id; // -1 if unowned

    // State for keys
    uint8_t modifiers; // Current modifiers
    uint8_t keys[HID_MAX_KEYS]; // List of currently pressed keys (usages), excluding modifiers
    size_t num_keys; // Number of pressed normal keys

    // State for pointers
    hid_ptr_type_t last_ptr_type;
    uint8_t buttons;
    uint16_t abs_x;
    uint16_t abs_y;
    int32_t rel_dx; // Accumulated relative dx
    int32_t rel_dy; // Accumulated relative dy
    int32_t rel_wheel; // Accumulated relative wheel

    // Retransmission state
    bool need_kbd_retransmit;
    bool need_abs_retransmit; // Indicates if absolute pointer needs sending/retransmission
    uint8_t rel_buttons;      // Button state the unsent relative movement belongs to (what the relative interface last saw)
    int rel_budget;           // Relative reports still allowed in the current call (reset at every entry point)
    bool need_rel_send;       // A relative report is owed even without movement (button change)
    bool ptr_switch_pending;  // Indicates an old pointer type zero-button event is pending to be sent

    // Timeouts and stats
    uint64_t last_msg_time; // Last time any key/button was held or KEEPALIVE received
    uint64_t last_tick_time;

    // Rate limit token bucket
    int32_t tokens;
    uint64_t token_last_update;

    // Abuse tracking
    uint32_t dropped_count;
    uint64_t abuse_window_start;

    uint32_t invalid_count;
} hid_logic_t;

/** Initializes `h`. `cfg` may be NULL (defaults from this header). `sink` is copied; its callbacks must not block and
 *  must not call back into hid_logic. */
void hid_logic_init(hid_logic_t *h, const hid_logic_config_t *cfg, const hid_sink_t *sink);

/** Handles one WebSocket input message from `owner_id` at time `now_ms`. Never logs the message content.
 *  HID_RESULT_ABUSE means the caller should release_all and close the connection. */
hid_result_t hid_logic_on_message(hid_logic_t *h, int owner_id, const uint8_t *msg, size_t len, uint64_t now_ms);

/** Periodic work (call about every 10-20 ms): hold timeout, re-sending what the sink refused, draining relative movement. */
void hid_logic_tick(hid_logic_t *h, uint64_t now_ms);

/** Releases every key and button (the all-released reports are sent now, or on the next tick if the sink refuses). */
void hid_logic_release_all(hid_logic_t *h);

/** The USB host (re-)enumerated or resumed: it forgot all key/button state. Drops unsent movement and sends all-released. */
void hid_logic_on_usb_mounted(hid_logic_t *h);

/** First come, first served: returns true if `owner_id` is (now) the controller, false if another one holds it. */
bool hid_logic_acquire(hid_logic_t *h, int owner_id);

/** If `owner_id` is the controller: release_all and free the slot. Anyone else: nothing happens. */
void hid_logic_release_owner(hid_logic_t *h, int owner_id);

#ifdef __cplusplus
}
#endif
