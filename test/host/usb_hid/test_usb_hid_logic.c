#include "unity.h"
#include "usb_hid_logic.h"

#include <string.h>

typedef struct {
    bool mounted;
    bool ready[USB_HID_ITF_COUNT];
    bool send_ok;

    size_t send_calls;
    uint8_t last_itf;
    uint8_t last_report_id;
    uint16_t last_len;
    uint8_t last_data[16];
} mock_hw_t;

static mock_hw_t s_hw;

static bool mock_mounted(void *ctx) {
    const mock_hw_t *hw = (const mock_hw_t *)ctx;
    return hw->mounted;
}

static bool mock_itf_ready(void *ctx, uint8_t itf) {
    const mock_hw_t *hw = (const mock_hw_t *)ctx;
    if (itf >= USB_HID_ITF_COUNT) {
        return false;
    }
    return hw->ready[itf];
}

static bool mock_send_report(void *ctx, uint8_t itf, uint8_t report_id,
                             const void *data, uint16_t len) {
    mock_hw_t *hw = (mock_hw_t *)ctx;
    hw->send_calls++;
    hw->last_itf = itf;
    hw->last_report_id = report_id;
    hw->last_len = len;
    if (data && len <= sizeof(hw->last_data)) {
        memcpy(hw->last_data, data, len);
    }
    return hw->send_ok;
}

static const usb_hid_hw_ops_t s_ops = {
    .mounted = mock_mounted,
    .itf_ready = mock_itf_ready,
    .send_report = mock_send_report,
};

void setUp(void) {
    memset(&s_hw, 0, sizeof(s_hw));
    s_hw.mounted = true;
    for (size_t i = 0; i < USB_HID_ITF_COUNT; i++) {
        s_hw.ready[i] = true;
    }
    s_hw.send_ok = true;
}

void tearDown(void) {}

static bool contains_bytes(const uint8_t *buf, size_t buf_len,
                           const uint8_t *needle, size_t needle_len) {
    if (!buf || !needle || needle_len == 0 || buf_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i <= buf_len - needle_len; i++) {
        if (memcmp(&buf[i], needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

void test_report_descriptors_lengths_and_no_report_ids(void) {
    size_t kbd_len = 0, abs_len = 0, rel_len = 0, bad_len = 999;
    const uint8_t *kbd = usb_hid_get_kbd_report_desc(&kbd_len);
    const uint8_t *abs = usb_hid_get_abs_report_desc(&abs_len);
    const uint8_t *rel = usb_hid_get_rel_report_desc(&rel_len);

    TEST_ASSERT_NOT_NULL(kbd);
    TEST_ASSERT_NOT_NULL(abs);
    TEST_ASSERT_NOT_NULL(rel);
    TEST_ASSERT_EQUAL_UINT32(65u, (uint32_t)kbd_len);
    TEST_ASSERT_EQUAL_UINT32(63u, (uint32_t)abs_len);
    TEST_ASSERT_EQUAL_UINT32(52u, (uint32_t)rel_len);

    TEST_ASSERT_EQUAL_PTR(kbd, usb_hid_get_report_desc(USB_HID_ITF_KEYBOARD, &bad_len));
    TEST_ASSERT_EQUAL_UINT32(kbd_len, (uint32_t)bad_len);
    TEST_ASSERT_EQUAL_PTR(abs, usb_hid_get_report_desc(USB_HID_ITF_ABS_MOUSE, &bad_len));
    TEST_ASSERT_EQUAL_UINT32(abs_len, (uint32_t)bad_len);
    TEST_ASSERT_EQUAL_PTR(rel, usb_hid_get_report_desc(USB_HID_ITF_REL_MOUSE, &bad_len));
    TEST_ASSERT_EQUAL_UINT32(rel_len, (uint32_t)bad_len);

    TEST_ASSERT_NULL(usb_hid_get_report_desc(USB_HID_ITF_COUNT, &bad_len));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)bad_len);

    /* Ensure Report ID global item tag (0x85) is NOT present in any interface descriptor. */
    for (size_t i = 0; i < kbd_len; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT8(0x85u, kbd[i]);
    }
    for (size_t i = 0; i < abs_len; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT8(0x85u, abs[i]);
    }
    for (size_t i = 0; i < rel_len; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT8(0x85u, rel[i]);
    }

    /* Boot keyboard requires 16-bit Logical Max (255) = 0x26, 0xFF, 0x00. */
    const uint8_t kbd_max_255[] = {0x26u, 0xFFu, 0x00u};
    TEST_ASSERT_TRUE(contains_bytes(kbd, kbd_len, kbd_max_255, sizeof(kbd_max_255)));

    /* Absolute pointer requires 0..32767 logical range (0x26, 0xFF, 0x7F) + 16-bit X/Y + wheel. */
    const uint8_t abs_max_32767[] = {0x15u, 0x00u, 0x26u, 0xFFu, 0x7Fu, 0x75u, 0x10u, 0x95u, 0x02u, 0x81u, 0x02u};
    TEST_ASSERT_TRUE(contains_bytes(abs, abs_len, abs_max_32767, sizeof(abs_max_32767)));

    /* Relative mouse uses relative wheel & X/Y (-127..127). */
    const uint8_t rel_axes[] = {0x15u, 0x81u, 0x25u, 0x7Fu, 0x75u, 0x08u, 0x95u, 0x03u, 0x81u, 0x06u};
    TEST_ASSERT_TRUE(contains_bytes(rel, rel_len, rel_axes, sizeof(rel_axes)));
}

void test_config_descriptors_fs_and_hs_structure(void) {
    size_t fs_len = 0, hs_len = 0;
    const uint8_t *fs = usb_hid_get_fs_config_desc(&fs_len);
    const uint8_t *hs = usb_hid_get_hs_config_desc(&hs_len);

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(hs);
    TEST_ASSERT_EQUAL_UINT32(USB_HID_CONFIG_DESC_LEN, (uint32_t)fs_len);
    TEST_ASSERT_EQUAL_UINT32(USB_HID_CONFIG_DESC_LEN, (uint32_t)hs_len);

    /* Config header (9 bytes) */
    TEST_ASSERT_EQUAL_UINT8(9u, fs[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02u, fs[1]);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_CONFIG_DESC_LEN & 0xFFu, fs[2]);
    TEST_ASSERT_EQUAL_UINT8((USB_HID_CONFIG_DESC_LEN >> 8) & 0xFFu, fs[3]);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_ITF_COUNT, fs[4]);
    TEST_ASSERT_EQUAL_UINT8(0xC0u, fs[7]); /* bmAttributes: self-powered */
    TEST_ASSERT_EQUAL_UINT8(0u, fs[8]);    /* bMaxPower: 0 mA */

    /* Verify FS and HS match everywhere except Endpoint bInterval bytes (offsets 33, 58, 83). */
    for (size_t i = 0; i < USB_HID_CONFIG_DESC_LEN; i++) {
        if (i == 33u || i == 58u || i == 83u) {
            TEST_ASSERT_EQUAL_UINT8(1u, fs[i]);
            TEST_ASSERT_EQUAL_UINT8(4u, hs[i]);
        } else {
            TEST_ASSERT_EQUAL_UINT8(fs[i], hs[i]);
        }
    }

    /* Walk each 25-byte HID interface block: off=9 (kbd), 34 (abs), 59 (rel) */
    const uint8_t *itf0 = &fs[9];
    const uint8_t *itf1 = &fs[34];
    const uint8_t *itf2 = &fs[59];

    /* ITF 0: Boot Keyboard (SubClass=1, Protocol=1, EP 0x81, ReportDescLen=65) */
    TEST_ASSERT_EQUAL_UINT8(0u, itf0[2]);
    TEST_ASSERT_EQUAL_UINT8(0x03u, itf0[5]);
    TEST_ASSERT_EQUAL_UINT8(0x01u, itf0[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01u, itf0[7]);
    TEST_ASSERT_EQUAL_UINT8(65u, itf0[9 + 7]);
    TEST_ASSERT_EQUAL_UINT8(0u, itf0[9 + 8]);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_EP_KEYBOARD, itf0[18 + 2]);
    TEST_ASSERT_EQUAL_UINT8(8u, itf0[18 + 4]);
    TEST_ASSERT_EQUAL_UINT8(0u, itf0[18 + 5]);

    /* ITF 1: Absolute Pointer (SubClass=0, Protocol=0, EP 0x82, ReportDescLen=63) */
    TEST_ASSERT_EQUAL_UINT8(1u, itf1[2]);
    TEST_ASSERT_EQUAL_UINT8(0x03u, itf1[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, itf1[6]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, itf1[7]);
    TEST_ASSERT_EQUAL_UINT8(63u, itf1[9 + 7]);
    TEST_ASSERT_EQUAL_UINT8(0u, itf1[9 + 8]);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_EP_ABS_MOUSE, itf1[18 + 2]);
    TEST_ASSERT_EQUAL_UINT8(8u, itf1[18 + 4]);
    TEST_ASSERT_EQUAL_UINT8(0u, itf1[18 + 5]);

    /* ITF 2: Relative Boot Mouse (SubClass=1, Protocol=2, EP 0x83, ReportDescLen=52) */
    TEST_ASSERT_EQUAL_UINT8(2u, itf2[2]);
    TEST_ASSERT_EQUAL_UINT8(0x03u, itf2[5]);
    TEST_ASSERT_EQUAL_UINT8(0x01u, itf2[6]);
    TEST_ASSERT_EQUAL_UINT8(0x02u, itf2[7]);
    TEST_ASSERT_EQUAL_UINT8(52u, itf2[9 + 7]);
    TEST_ASSERT_EQUAL_UINT8(0u, itf2[9 + 8]);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_EP_REL_MOUSE, itf2[18 + 2]);
    TEST_ASSERT_EQUAL_UINT8(8u, itf2[18 + 4]);
    TEST_ASSERT_EQUAL_UINT8(0u, itf2[18 + 5]);
}

void test_pack_keyboard_abs_and_rel_reports(void) {
    uint8_t kbd_buf[USB_HID_KBD_REPORT_LEN];
    hid_kbd_report_t kbd = {
        .modifiers = 0x05u,
        .keys = {0x04u, 0x1Eu, 0x1Fu, 0x20u, 0x21u, 0x28u},
    };
    memset(kbd_buf, 0xAA, sizeof(kbd_buf));
    TEST_ASSERT_TRUE(usb_hid_pack_keyboard(&kbd, kbd_buf));
    TEST_ASSERT_EQUAL_UINT8(0x05u, kbd_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, kbd_buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x04u, kbd_buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x1Eu, kbd_buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x1Fu, kbd_buf[4]);
    TEST_ASSERT_EQUAL_UINT8(0x20u, kbd_buf[5]);
    TEST_ASSERT_EQUAL_UINT8(0x21u, kbd_buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0x28u, kbd_buf[7]);
    TEST_ASSERT_FALSE(usb_hid_pack_keyboard(NULL, kbd_buf));
    TEST_ASSERT_FALSE(usb_hid_pack_keyboard(&kbd, NULL));

    uint8_t abs_buf[USB_HID_ABS_REPORT_LEN];
    hid_abs_report_t abs_in_range = {
        .buttons = 0x05u,
        .x = 0x1234u,
        .y = 0x5678u,
        .wheel = 12,
    };
    TEST_ASSERT_TRUE(usb_hid_pack_abs_mouse(&abs_in_range, abs_buf));
    TEST_ASSERT_EQUAL_UINT8(0x05u, abs_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34u, abs_buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x12u, abs_buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x78u, abs_buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x56u, abs_buf[4]);
    TEST_ASSERT_EQUAL_INT8(12, (int8_t)abs_buf[5]);

    hid_abs_report_t abs = {
        .buttons = 0xFFu, /* Upper unused bits must be masked to 0x07 */
        .x = 0x1234u,
        .y = 40000u, /* > HID_ABS_MAX (32767) -> clamped to 32767 (0x7FFF) */
        .wheel = -128, /* clamped to -127 */
    };
    TEST_ASSERT_TRUE(usb_hid_pack_abs_mouse(&abs, abs_buf));
    TEST_ASSERT_EQUAL_UINT8(0x07u, abs_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34u, abs_buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x12u, abs_buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, abs_buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x7Fu, abs_buf[4]);
    TEST_ASSERT_EQUAL_INT8(-127, (int8_t)abs_buf[5]);
    TEST_ASSERT_FALSE(usb_hid_pack_abs_mouse(NULL, abs_buf));
    TEST_ASSERT_FALSE(usb_hid_pack_abs_mouse(&abs, NULL));

    uint8_t rel_buf[USB_HID_REL_REPORT_LEN];
    hid_rel_report_t rel = {
        .buttons = 0xF3u, /* masked to 0x03 */
        .dx = -128,       /* clamped to -127 */
        .dy = 127,
        .wheel = -5,
    };
    TEST_ASSERT_TRUE(usb_hid_pack_rel_mouse(&rel, rel_buf));
    TEST_ASSERT_EQUAL_UINT8(0x03u, rel_buf[0]);
    TEST_ASSERT_EQUAL_INT8(-127, (int8_t)rel_buf[1]);
    TEST_ASSERT_EQUAL_INT8(127, (int8_t)rel_buf[2]);
    TEST_ASSERT_EQUAL_INT8(-5, (int8_t)rel_buf[3]);
    TEST_ASSERT_FALSE(usb_hid_pack_rel_mouse(NULL, rel_buf));
    TEST_ASSERT_FALSE(usb_hid_pack_rel_mouse(&rel, NULL));
}

void test_sink_nonblocking_ready_and_routing(void) {
    hid_sink_t sink;
    usb_hid_sink_ctx_t ctx;

    TEST_ASSERT_FALSE(usb_hid_sink_init(NULL, &ctx, &s_ops, &s_hw));
    TEST_ASSERT_FALSE(usb_hid_sink_init(&sink, NULL, &s_ops, &s_hw));
    TEST_ASSERT_FALSE(usb_hid_sink_init(&sink, &ctx, NULL, &s_hw));
    TEST_ASSERT_TRUE(usb_hid_sink_init(&sink, &ctx, &s_ops, &s_hw));

    hid_kbd_report_t kbd = {.modifiers = 0x02u, .keys = {0x04u, 0, 0, 0, 0, 0}};
    hid_abs_report_t abs = {.buttons = 0x01u, .x = 16384u, .y = 8192u, .wheel = 1};
    hid_rel_report_t rel = {.buttons = 0x02u, .dx = -12, .dy = 34, .wheel = -1};

    /* Mounted and all EPs idle -> ready() is true and sends route to 0/1/2 with report_id=0 */
    TEST_ASSERT_TRUE(sink.ready(sink.ctx));

    TEST_ASSERT_TRUE(sink.send_keyboard(sink.ctx, &kbd));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)s_hw.send_calls);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_ITF_KEYBOARD, s_hw.last_itf);
    TEST_ASSERT_EQUAL_UINT8(0u, s_hw.last_report_id);
    TEST_ASSERT_EQUAL_UINT16(USB_HID_KBD_REPORT_LEN, s_hw.last_len);
    TEST_ASSERT_EQUAL_UINT8(0x02u, s_hw.last_data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x04u, s_hw.last_data[2]);

    TEST_ASSERT_TRUE(sink.send_abs(sink.ctx, &abs));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)s_hw.send_calls);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_ITF_ABS_MOUSE, s_hw.last_itf);
    TEST_ASSERT_EQUAL_UINT8(0u, s_hw.last_report_id);
    TEST_ASSERT_EQUAL_UINT16(USB_HID_ABS_REPORT_LEN, s_hw.last_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_hw.last_data[1]);
    TEST_ASSERT_EQUAL_UINT8(0x40u, s_hw.last_data[2]);

    TEST_ASSERT_TRUE(sink.send_rel(sink.ctx, &rel));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)s_hw.send_calls);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_ITF_REL_MOUSE, s_hw.last_itf);
    TEST_ASSERT_EQUAL_UINT8(0u, s_hw.last_report_id);
    TEST_ASSERT_EQUAL_UINT16(USB_HID_REL_REPORT_LEN, s_hw.last_len);
    TEST_ASSERT_EQUAL_INT8(-12, (int8_t)s_hw.last_data[1]);
    TEST_ASSERT_EQUAL_INT8(34, (int8_t)s_hw.last_data[2]);

    /* Busy abs EP returns false immediately without blocking or calling send_report; other ITFs stay active */
    s_hw.ready[USB_HID_ITF_ABS_MOUSE] = false;
    TEST_ASSERT_FALSE(sink.send_abs(sink.ctx, &abs));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)s_hw.send_calls);
    TEST_ASSERT_TRUE(sink.send_rel(sink.ctx, &rel));
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)s_hw.send_calls);

    /* Unmounted returns false for ready() and all sends without calling send_report */
    s_hw.ready[USB_HID_ITF_ABS_MOUSE] = true;
    s_hw.mounted = false;
    TEST_ASSERT_FALSE(sink.ready(sink.ctx));
    TEST_ASSERT_FALSE(sink.send_keyboard(sink.ctx, &kbd));
    TEST_ASSERT_FALSE(sink.send_abs(sink.ctx, &abs));
    TEST_ASSERT_FALSE(sink.send_rel(sink.ctx, &rel));
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)s_hw.send_calls);

    /* Keyboard EP busy keeps ready() true while mounted, blocks only send_keyboard, and allows abs/rel */
    s_hw.mounted = true;
    s_hw.ready[USB_HID_ITF_KEYBOARD] = false;
    TEST_ASSERT_TRUE(sink.ready(sink.ctx));
    TEST_ASSERT_FALSE(sink.send_keyboard(sink.ctx, &kbd));
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)s_hw.send_calls);
    TEST_ASSERT_TRUE(sink.send_abs(sink.ctx, &abs));
    TEST_ASSERT_TRUE(sink.send_rel(sink.ctx, &rel));
    TEST_ASSERT_EQUAL_UINT32(6u, (uint32_t)s_hw.send_calls);

    /* NULL ctx and NULL report rejected cleanly */
    TEST_ASSERT_FALSE(sink.ready(NULL));
    TEST_ASSERT_FALSE(sink.send_keyboard(NULL, &kbd));
    s_hw.ready[USB_HID_ITF_KEYBOARD] = true;
    TEST_ASSERT_FALSE(sink.send_keyboard(sink.ctx, NULL));
    TEST_ASSERT_FALSE(sink.send_abs(sink.ctx, NULL));
    TEST_ASSERT_FALSE(sink.send_rel(sink.ctx, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_report_descriptors_lengths_and_no_report_ids);
    RUN_TEST(test_config_descriptors_fs_and_hs_structure);
    RUN_TEST(test_pack_keyboard_abs_and_rel_reports);
    RUN_TEST(test_sink_nonblocking_ready_and_routing);
    return UNITY_END();
}
