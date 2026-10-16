#include "unity.h"
#include "tc358743_edid.h"

void setUp(void) {}
void tearDown(void) {}

void test_edid_generation_basic(void) {
    uint8_t buffer[256];
    size_t len = tc358743_edid_build(buffer, sizeof(buffer), 81);

    TEST_ASSERT_EQUAL(256, len);

    // Check header
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buffer[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buffer[6]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[7]);

    // Check extension count
    TEST_ASSERT_EQUAL_HEX8(0x01, buffer[126]);

    // Check extension block tag and revision
    TEST_ASSERT_EQUAL_HEX8(0x02, buffer[128]);
    TEST_ASSERT_EQUAL_HEX8(0x03, buffer[129]);

    // Checksums
    uint8_t sum1 = 0;
    for (int i = 0; i < 128; i++) sum1 += buffer[i];
    TEST_ASSERT_EQUAL_HEX8(0x00, sum1);

    uint8_t sum2 = 0;
    for (int i = 128; i < 256; i++) sum2 += buffer[i];
    TEST_ASSERT_EQUAL_HEX8(0x00, sum2);
}

void test_edid_max_pixel_clock_filtering(void) {
    uint8_t buffer_81mhz[256];
    uint8_t buffer_150mhz[256];

    tc358743_edid_build(buffer_81mhz, sizeof(buffer_81mhz), 81);
    tc358743_edid_build(buffer_150mhz, sizeof(buffer_150mhz), 150);

    // Offset 128 is tag. Offset 130 is dtd_offset.
    // At offset 132 is the first data block in our structure.

    // For 81MHz: Should contain 1080p30 (VIC 34), 720p60 (VIC 4), 480p60 (VIC 1)
    // Tag 2 (VDB), length 3 => (0x02 << 5) | 3 = 0x43
    TEST_ASSERT_EQUAL_HEX8(0x43, buffer_81mhz[132]);
    TEST_ASSERT_EQUAL_HEX8(34, buffer_81mhz[133]);
    TEST_ASSERT_EQUAL_HEX8(4, buffer_81mhz[134]);
    TEST_ASSERT_EQUAL_HEX8(1, buffer_81mhz[135]);

    // For 150MHz: Should also contain 1080p60 (VIC 16)
    // Tag 2 (VDB), length 4 => (0x02 << 5) | 4 = 0x44
    TEST_ASSERT_EQUAL_HEX8(0x44, buffer_150mhz[132]);
    TEST_ASSERT_EQUAL_HEX8(16, buffer_150mhz[133]);
    TEST_ASSERT_EQUAL_HEX8(34, buffer_150mhz[134]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_edid_generation_basic);
    RUN_TEST(test_edid_max_pixel_clock_filtering);
    return UNITY_END();
}
