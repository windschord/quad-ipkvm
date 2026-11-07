#include "hid_msg.h"

bool hid_msg_parse(const uint8_t *msg, size_t len, hid_msg_t *out) {
    if (!msg || !out || len == 0) return false;

    out->type = HID_MSG_TYPE_INVALID;
    uint8_t type_byte = msg[0];

    switch (type_byte) {
        case 0x01: // KEY_DOWN
            if (len != 2) return false;
            out->type = HID_MSG_TYPE_KEY_DOWN;
            out->data.key.usage = msg[1];
            return true;

        case 0x02: // KEY_UP
            if (len != 2) return false;
            out->type = HID_MSG_TYPE_KEY_UP;
            out->data.key.usage = msg[1];
            return true;

        case 0x03: // RELEASE_ALL
            if (len != 1) return false;
            out->type = HID_MSG_TYPE_RELEASE_ALL;
            return true;

        case 0x04: // KEEPALIVE
            if (len != 1) return false;
            out->type = HID_MSG_TYPE_KEEPALIVE;
            return true;

        case 0x10: // PTR_ABS
            if (len != 7) return false;
            out->type = HID_MSG_TYPE_PTR_ABS;
            out->data.ptr_abs.buttons = msg[1] & 0x07; // limit to lower 3 bits
            out->data.ptr_abs.x = (uint16_t)(msg[2] | (msg[3] << 8));
            out->data.ptr_abs.y = (uint16_t)(msg[4] | (msg[5] << 8));
            out->data.ptr_abs.wheel = (int8_t)msg[6];
            return true;

        case 0x11: // PTR_REL
            if (len != 7) return false;
            out->type = HID_MSG_TYPE_PTR_REL;
            out->data.ptr_rel.buttons = msg[1] & 0x07;
            out->data.ptr_rel.dx = (int16_t)(msg[2] | (msg[3] << 8));
            out->data.ptr_rel.dy = (int16_t)(msg[4] | (msg[5] << 8));
            out->data.ptr_rel.wheel = (int8_t)msg[6];
            return true;

        default:
            return false;
    }
}
