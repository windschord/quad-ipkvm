#include "usb_hid_logic.h"

#include <string.h>

#define USB_HID_KBD_REPORT_DESC_LEN 65u
#define USB_HID_ABS_REPORT_DESC_LEN 63u
#define USB_HID_REL_REPORT_DESC_LEN 52u

#define HID_DESC_EP_MAX_PACKET 8u
#define HID_INTERVAL_FS        1u /* 1 ms @ Full-Speed */
#define HID_INTERVAL_HS        4u /* 2^(4-1) * 125 us = 1 ms @ High-Speed */

#define USB_DESC_TYPE_CONF 0x02u
#define USB_DESC_TYPE_ITF  0x04u
#define USB_DESC_TYPE_EP   0x05u
#define USB_DESC_TYPE_HID  0x21u
#define USB_DESC_TYPE_RPT  0x22u

#define USB_CLASS_HID_VAL      0x03u
#define USB_SUBCLASS_NONE_VAL  0x00u
#define USB_SUBCLASS_BOOT_VAL  0x01u
#define USB_PROTO_NONE_VAL     0x00u
#define USB_PROTO_KBD_VAL      0x01u
#define USB_PROTO_MOUSE_VAL    0x02u

/* Standard 6KRO Boot Keyboard report descriptor (USB HID 1.11 Appendix B.1), no Report ID. */
static const uint8_t s_kbd_report_desc[USB_HID_KBD_REPORT_DESC_LEN] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,       /*   Usage Minimum (224 = Left Control) */
    0x29, 0xE7,       /*   Usage Maximum (231 = Right GUI) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) - Modifiers */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x01,       /*   Input (Constant) - Reserved OEM byte */
    0x95, 0x05,       /*   Report Count (5) */
    0x75, 0x01,       /*   Report Size (1) */
    0x05, 0x08,       /*   Usage Page (LEDs) */
    0x19, 0x01,       /*   Usage Minimum (Num Lock) */
    0x29, 0x05,       /*   Usage Maximum (Kana) */
    0x91, 0x02,       /*   Output (Data, Variable, Absolute) - LEDs */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x03,       /*   Report Size (3) */
    0x91, 0x01,       /*   Output (Constant) - LED padding */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) - 16-bit LE required (>127) */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x00, /*   Usage Maximum (255) - 16-bit LE required (>127) */
    0x81, 0x00,       /*   Input (Data, Array) - 6 keycodes */
    0xC0              /* End Collection */
};
_Static_assert(sizeof(s_kbd_report_desc) == USB_HID_KBD_REPORT_DESC_LEN, "kbd desc len");

/* Absolute Pointer (0..32767 X/Y LE + relative wheel -127..127), no Report ID. */
static const uint8_t s_abs_report_desc[USB_HID_ABS_REPORT_DESC_LEN] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */
    0x05, 0x09,       /*     Usage Page (Button) */
    0x19, 0x01,       /*     Usage Minimum (Button 1) */
    0x29, 0x03,       /*     Usage Maximum (Button 3) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x25, 0x01,       /*     Logical Maximum (1) */
    0x95, 0x03,       /*     Report Count (3) */
    0x75, 0x01,       /*     Report Size (1) */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute) - Buttons */
    0x95, 0x01,       /*     Report Count (1) */
    0x75, 0x05,       /*     Report Size (5) */
    0x81, 0x01,       /*     Input (Constant) - Button padding */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x09, 0x30,       /*     Usage (X) */
    0x09, 0x31,       /*     Usage (Y) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x26, 0xFF, 0x7F, /*     Logical Maximum (32767) */
    0x75, 0x10,       /*     Report Size (16) */
    0x95, 0x02,       /*     Report Count (2) */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute) */
    0x09, 0x38,       /*     Usage (Wheel) */
    0x15, 0x81,       /*     Logical Minimum (-127) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x06,       /*     Input (Data, Variable, Relative) */
    0xC0,             /*   End Collection (Physical) */
    0xC0              /* End Collection (Application) */
};
_Static_assert(sizeof(s_abs_report_desc) == USB_HID_ABS_REPORT_DESC_LEN, "abs desc len");

/* Relative Boot Mouse (3B boot-compatible buttons+dx+dy + 1B wheel), no Report ID. */
static const uint8_t s_rel_report_desc[USB_HID_REL_REPORT_DESC_LEN] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x02, /* Usage (Mouse) */
    0xA1, 0x01, /* Collection (Application) */
    0x09, 0x01, /*   Usage (Pointer) */
    0xA1, 0x00, /*   Collection (Physical) */
    0x05, 0x09, /*     Usage Page (Button) */
    0x19, 0x01, /*     Usage Minimum (Button 1) */
    0x29, 0x03, /*     Usage Maximum (Button 3) */
    0x15, 0x00, /*     Logical Minimum (0) */
    0x25, 0x01, /*     Logical Maximum (1) */
    0x95, 0x03, /*     Report Count (3) */
    0x75, 0x01, /*     Report Size (1) */
    0x81, 0x02, /*     Input (Data, Variable, Absolute) - Buttons */
    0x95, 0x01, /*     Report Count (1) */
    0x75, 0x05, /*     Report Size (5) */
    0x81, 0x01, /*     Input (Constant) - Button padding */
    0x05, 0x01, /*     Usage Page (Generic Desktop) */
    0x09, 0x30, /*     Usage (X) */
    0x09, 0x31, /*     Usage (Y) */
    0x09, 0x38, /*     Usage (Wheel) */
    0x15, 0x81, /*     Logical Minimum (-127) */
    0x25, 0x7F, /*     Logical Maximum (127) */
    0x75, 0x08, /*     Report Size (8) */
    0x95, 0x03, /*     Report Count (3) */
    0x81, 0x06, /*     Input (Data, Variable, Relative) */
    0xC0,       /*   End Collection (Physical) */
    0xC0        /* End Collection (Application) */
};
_Static_assert(sizeof(s_rel_report_desc) == USB_HID_REL_REPORT_DESC_LEN, "rel desc len");

#define HID_ITF_DESC_BLOCK(itf_num, str_idx, subclass, proto, rpt_len, ep_in, ep_interval) \
    /* Interface Descriptor (9B) */ \
    9u, USB_DESC_TYPE_ITF, (uint8_t)(itf_num), 0u, 1u, USB_CLASS_HID_VAL, \
        (uint8_t)(subclass), (uint8_t)(proto), (uint8_t)(str_idx), \
    /* HID Descriptor (9B) */ \
    9u, USB_DESC_TYPE_HID, 0x11u, 0x01u, 0x00u, 1u, USB_DESC_TYPE_RPT, \
        (uint8_t)((rpt_len) & 0xFFu), (uint8_t)(((rpt_len) >> 8) & 0xFFu), \
    /* Endpoint IN Descriptor (7B) */ \
    7u, USB_DESC_TYPE_EP, (uint8_t)(ep_in), 0x03u, \
        (uint8_t)(HID_DESC_EP_MAX_PACKET & 0xFFu), 0x00u, (uint8_t)(ep_interval)

#define COMPOSITE_CONFIG_DESC(ep_interval) { \
    /* Configuration Descriptor (9B) */ \
    /* TODO(hw-verify): bmAttributes=0xC0 (self-powered) / bMaxPower=0 (0 mA) matches 3-wire J3 without VBUS; verify BIOS/host compatibility */ \
    9u, USB_DESC_TYPE_CONF, \
    (uint8_t)(USB_HID_CONFIG_DESC_LEN & 0xFFu), \
    (uint8_t)((USB_HID_CONFIG_DESC_LEN >> 8) & 0xFFu), \
    USB_HID_ITF_COUNT, 1u, 0u, 0xC0u, 0u, \
    /* Interface 0: Boot Keyboard */ \
    HID_ITF_DESC_BLOCK(USB_HID_ITF_KEYBOARD, 4u, USB_SUBCLASS_BOOT_VAL, USB_PROTO_KBD_VAL, \
                       USB_HID_KBD_REPORT_DESC_LEN, USB_HID_EP_KEYBOARD, (ep_interval)), \
    /* Interface 1: Absolute Pointer (SubClass=0, Protocol=0) */ \
    HID_ITF_DESC_BLOCK(USB_HID_ITF_ABS_MOUSE, 5u, USB_SUBCLASS_NONE_VAL, USB_PROTO_NONE_VAL, \
                       USB_HID_ABS_REPORT_DESC_LEN, USB_HID_EP_ABS_MOUSE, (ep_interval)), \
    /* Interface 2: Relative Boot Mouse (SubClass=1, Protocol=2) */ \
    HID_ITF_DESC_BLOCK(USB_HID_ITF_REL_MOUSE, 6u, USB_SUBCLASS_BOOT_VAL, USB_PROTO_MOUSE_VAL, \
                       USB_HID_REL_REPORT_DESC_LEN, USB_HID_EP_REL_MOUSE, (ep_interval)) \
}

static const uint8_t s_fs_config_desc[USB_HID_CONFIG_DESC_LEN] = COMPOSITE_CONFIG_DESC(HID_INTERVAL_FS);
static const uint8_t s_hs_config_desc[USB_HID_CONFIG_DESC_LEN] = COMPOSITE_CONFIG_DESC(HID_INTERVAL_HS);

static int8_t clamp_i8_sym(int8_t v) {
    return (v == -128) ? -127 : v;
}

const uint8_t *usb_hid_get_kbd_report_desc(size_t *out_len) {
    if (out_len) {
        *out_len = sizeof(s_kbd_report_desc);
    }
    return s_kbd_report_desc;
}

const uint8_t *usb_hid_get_abs_report_desc(size_t *out_len) {
    if (out_len) {
        *out_len = sizeof(s_abs_report_desc);
    }
    return s_abs_report_desc;
}

const uint8_t *usb_hid_get_rel_report_desc(size_t *out_len) {
    if (out_len) {
        *out_len = sizeof(s_rel_report_desc);
    }
    return s_rel_report_desc;
}

const uint8_t *usb_hid_get_report_desc(uint8_t itf, size_t *out_len) {
    switch (itf) {
        case USB_HID_ITF_KEYBOARD:  return usb_hid_get_kbd_report_desc(out_len);
        case USB_HID_ITF_ABS_MOUSE: return usb_hid_get_abs_report_desc(out_len);
        case USB_HID_ITF_REL_MOUSE: return usb_hid_get_rel_report_desc(out_len);
        default:
            if (out_len) {
                *out_len = 0u;
            }
            return NULL;
    }
}

const uint8_t *usb_hid_get_fs_config_desc(size_t *out_len) {
    if (out_len) {
        *out_len = sizeof(s_fs_config_desc);
    }
    return s_fs_config_desc;
}

const uint8_t *usb_hid_get_hs_config_desc(size_t *out_len) {
    if (out_len) {
        *out_len = sizeof(s_hs_config_desc);
    }
    return s_hs_config_desc;
}

bool usb_hid_pack_keyboard(const hid_kbd_report_t *in,
                           uint8_t out[USB_HID_KBD_REPORT_LEN]) {
    if (!in || !out) {
        return false;
    }
    out[0] = in->modifiers;
    out[1] = 0u;
    memcpy(&out[2], in->keys, 6u);
    return true;
}

bool usb_hid_pack_abs_mouse(const hid_abs_report_t *in,
                            uint8_t out[USB_HID_ABS_REPORT_LEN]) {
    if (!in || !out) {
        return false;
    }
    uint16_t x = (in->x > HID_ABS_MAX) ? (uint16_t)HID_ABS_MAX : in->x;
    uint16_t y = (in->y > HID_ABS_MAX) ? (uint16_t)HID_ABS_MAX : in->y;
    out[0] = in->buttons & 0x07u;
    out[1] = (uint8_t)(x & 0xFFu);
    out[2] = (uint8_t)((x >> 8) & 0xFFu);
    out[3] = (uint8_t)(y & 0xFFu);
    out[4] = (uint8_t)((y >> 8) & 0xFFu);
    out[5] = (uint8_t)clamp_i8_sym(in->wheel);
    return true;
}

bool usb_hid_pack_rel_mouse(const hid_rel_report_t *in,
                            uint8_t out[USB_HID_REL_REPORT_LEN]) {
    if (!in || !out) {
        return false;
    }
    out[0] = in->buttons & 0x07u;
    out[1] = (uint8_t)clamp_i8_sym(in->dx);
    out[2] = (uint8_t)clamp_i8_sym(in->dy);
    out[3] = (uint8_t)clamp_i8_sym(in->wheel);
    return true;
}

static bool sink_ctx_valid(const usb_hid_sink_ctx_t *sc) {
    return sc && sc->ops && sc->ops->mounted && sc->ops->itf_ready && sc->ops->send_report;
}

static bool sink_can_send(const usb_hid_sink_ctx_t *sc, uint8_t itf) {
    if (!sink_ctx_valid(sc)) {
        return false;
    }
    if (!sc->ops->mounted(sc->hw_ctx)) {
        return false;
    }
    return sc->ops->itf_ready(sc->hw_ctx, itf);
}

static bool sink_ready(void *ctx) {
    /* Non-blocking check: requires USB enumeration + primary keyboard EP idle. */
    return sink_can_send((const usb_hid_sink_ctx_t *)ctx, USB_HID_ITF_KEYBOARD);
}

static bool sink_send_keyboard(void *ctx, const hid_kbd_report_t *r) {
    const usb_hid_sink_ctx_t *sc = (const usb_hid_sink_ctx_t *)ctx;
    uint8_t buf[USB_HID_KBD_REPORT_LEN];
    if (!r || !sink_can_send(sc, USB_HID_ITF_KEYBOARD) || !usb_hid_pack_keyboard(r, buf)) {
        return false;
    }
    return sc->ops->send_report(sc->hw_ctx, USB_HID_ITF_KEYBOARD, 0u, buf, sizeof(buf));
}

static bool sink_send_abs(void *ctx, const hid_abs_report_t *r) {
    const usb_hid_sink_ctx_t *sc = (const usb_hid_sink_ctx_t *)ctx;
    uint8_t buf[USB_HID_ABS_REPORT_LEN];
    if (!r || !sink_can_send(sc, USB_HID_ITF_ABS_MOUSE) || !usb_hid_pack_abs_mouse(r, buf)) {
        return false;
    }
    return sc->ops->send_report(sc->hw_ctx, USB_HID_ITF_ABS_MOUSE, 0u, buf, sizeof(buf));
}

static bool sink_send_rel(void *ctx, const hid_rel_report_t *r) {
    const usb_hid_sink_ctx_t *sc = (const usb_hid_sink_ctx_t *)ctx;
    uint8_t buf[USB_HID_REL_REPORT_LEN];
    if (!r || !sink_can_send(sc, USB_HID_ITF_REL_MOUSE) || !usb_hid_pack_rel_mouse(r, buf)) {
        return false;
    }
    return sc->ops->send_report(sc->hw_ctx, USB_HID_ITF_REL_MOUSE, 0u, buf, sizeof(buf));
}

bool usb_hid_sink_init(hid_sink_t *out_sink, usb_hid_sink_ctx_t *ctx_storage,
                       const usb_hid_hw_ops_t *ops, void *hw_ctx) {
    if (!out_sink || !ctx_storage || !ops ||
        !ops->mounted || !ops->itf_ready || !ops->send_report) {
        return false;
    }
    ctx_storage->ops = ops;
    ctx_storage->hw_ctx = hw_ctx;

    out_sink->ctx = ctx_storage;
    out_sink->ready = sink_ready;
    out_sink->send_keyboard = sink_send_keyboard;
    out_sink->send_abs = sink_send_abs;
    out_sink->send_rel = sink_send_rel;
    return true;
}
