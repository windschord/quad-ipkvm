#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HID_MSG_TYPE_KEY_DOWN = 0x01,
    HID_MSG_TYPE_KEY_UP = 0x02,
    HID_MSG_TYPE_RELEASE_ALL = 0x03,
    HID_MSG_TYPE_KEEPALIVE = 0x04,
    HID_MSG_TYPE_PTR_ABS = 0x10,
    HID_MSG_TYPE_PTR_REL = 0x11,
    HID_MSG_TYPE_INVALID = 0xFF
} hid_msg_type_t;

typedef struct {
    hid_msg_type_t type;
    union {
        struct {
            uint8_t usage;
        } key;
        struct {
            uint8_t buttons;
            uint16_t x;
            uint16_t y;
            int8_t wheel;
        } ptr_abs;
        struct {
            uint8_t buttons;
            int16_t dx;
            int16_t dy;
            int8_t wheel;
        } ptr_rel;
    } data;
} hid_msg_t;

// Parses a WS binary message into a hid_msg_t.
// Returns true if parsing is successful and the message length is strictly correct.
// The `usage` is not validated here, `hid_logic` will handle invalid usage.
bool hid_msg_parse(const uint8_t *msg, size_t len, hid_msg_t *out);

#ifdef __cplusplus
}
#endif
