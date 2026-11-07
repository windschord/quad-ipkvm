#include "unity.h"
#include "hid_msg.h"

void setUp(void) {}
void tearDown(void) {}

void test_msg_parse_key_down(void) {
    hid_msg_t m;
    uint8_t msg[] = {0x01, 0x04};
    TEST_ASSERT_TRUE(hid_msg_parse(msg, 2, &m));
    TEST_ASSERT_EQUAL(HID_MSG_TYPE_KEY_DOWN, m.type);
    TEST_ASSERT_EQUAL(0x04, m.data.key.usage);

    TEST_ASSERT_FALSE(hid_msg_parse(msg, 1, &m)); // Length 1
    TEST_ASSERT_FALSE(hid_msg_parse(msg, 3, &m)); // Length 3
}

void test_msg_parse_key_up(void) {
    hid_msg_t m;
    uint8_t msg[] = {0x02, 0xA4};
    TEST_ASSERT_TRUE(hid_msg_parse(msg, 2, &m));
    TEST_ASSERT_EQUAL(HID_MSG_TYPE_KEY_UP, m.type);
    TEST_ASSERT_EQUAL(0xA4, m.data.key.usage);

    TEST_ASSERT_FALSE(hid_msg_parse(msg, 1, &m));
    TEST_ASSERT_FALSE(hid_msg_parse(msg, 3, &m));
}

void test_msg_parse_release_all(void) {
    hid_msg_t m;
    uint8_t msg[] = {0x03};
    TEST_ASSERT_TRUE(hid_msg_parse(msg, 1, &m));
    TEST_ASSERT_EQUAL(HID_MSG_TYPE_RELEASE_ALL, m.type);

    TEST_ASSERT_FALSE(hid_msg_parse(msg, 2, &m));
}

void test_msg_parse_keepalive(void) {
    hid_msg_t m;
    uint8_t msg[] = {0x04};
    TEST_ASSERT_TRUE(hid_msg_parse(msg, 1, &m));
    TEST_ASSERT_EQUAL(HID_MSG_TYPE_KEEPALIVE, m.type);

    TEST_ASSERT_FALSE(hid_msg_parse(msg, 2, &m));
}

void test_msg_parse_ptr_abs(void) {
    hid_msg_t m;
    uint8_t msg[] = {0x10, 0x0F, 0xFF, 0x7F, 0x00, 0x80, 0x80}; // buttons=0x0F(should be masked to 7), x=32767, y=32768, wheel=-128
    TEST_ASSERT_TRUE(hid_msg_parse(msg, 7, &m));
    TEST_ASSERT_EQUAL(HID_MSG_TYPE_PTR_ABS, m.type);
    TEST_ASSERT_EQUAL(0x07, m.data.ptr_abs.buttons); // Masked to lower 3 bits
    TEST_ASSERT_EQUAL(32767, m.data.ptr_abs.x);
    TEST_ASSERT_EQUAL(32768, m.data.ptr_abs.y);
    TEST_ASSERT_EQUAL(-128, m.data.ptr_abs.wheel);

    TEST_ASSERT_FALSE(hid_msg_parse(msg, 6, &m));
    TEST_ASSERT_FALSE(hid_msg_parse(msg, 8, &m));
}

void test_msg_parse_ptr_rel(void) {
    hid_msg_t m;
    uint8_t msg[] = {0x11, 0x05, 0x00, 0x01, 0xFF, 0xFF, 0x7F}; // buttons=5, dx=256, dy=-1, wheel=127
    TEST_ASSERT_TRUE(hid_msg_parse(msg, 7, &m));
    TEST_ASSERT_EQUAL(HID_MSG_TYPE_PTR_REL, m.type);
    TEST_ASSERT_EQUAL(0x05, m.data.ptr_rel.buttons);
    TEST_ASSERT_EQUAL(256, m.data.ptr_rel.dx);
    TEST_ASSERT_EQUAL(-1, m.data.ptr_rel.dy);
    TEST_ASSERT_EQUAL(127, m.data.ptr_rel.wheel);

    TEST_ASSERT_FALSE(hid_msg_parse(msg, 6, &m));
    TEST_ASSERT_FALSE(hid_msg_parse(msg, 8, &m));

    uint8_t wide[] = {0x11, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00}; // only the lower 3 button bits are valid
    TEST_ASSERT_TRUE(hid_msg_parse(wide, 7, &m));
    TEST_ASSERT_EQUAL(0x07, m.data.ptr_rel.buttons);
}

void test_msg_parse_invalid(void) {
    hid_msg_t m;
    uint8_t msg[] = {0x55, 0x00};
    TEST_ASSERT_FALSE(hid_msg_parse(msg, 2, &m));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_msg_parse_key_down);
    RUN_TEST(test_msg_parse_key_up);
    RUN_TEST(test_msg_parse_release_all);
    RUN_TEST(test_msg_parse_keepalive);
    RUN_TEST(test_msg_parse_ptr_abs);
    RUN_TEST(test_msg_parse_ptr_rel);
    RUN_TEST(test_msg_parse_invalid);
    return UNITY_END();
}
