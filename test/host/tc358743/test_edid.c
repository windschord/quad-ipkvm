#include "unity.h"
#include "tc358743_edid.h"

void setUp(void) {}
void tearDown(void) {}

void test_edid_generation_basic(void) {
    uint8_t buffer[256];
    size_t len = tc358743_edid_build(buffer, sizeof(buffer), 81000);

    TEST_ASSERT_EQUAL(256, len);

    // Check header
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buffer[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buffer[6]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[7]);

    // Check Vendor ID for "ESP"
    TEST_ASSERT_EQUAL_HEX8(0x70, buffer[8]);
    TEST_ASSERT_EQUAL_HEX8(0x16, buffer[9]);

    // Check Monitor Name Descriptor is present
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[72]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[73]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[74]);
    TEST_ASSERT_EQUAL_HEX8(0xFC, buffer[75]);
    TEST_ASSERT_EQUAL_HEX8('E', buffer[77]);

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

    tc358743_edid_build(buffer_81mhz, sizeof(buffer_81mhz), 81000);
    tc358743_edid_build(buffer_150mhz, sizeof(buffer_150mhz), 150000);

    // For 81MHz: Should contain 1080p30 (VIC 34), 720p60 (VIC 4), 480p60 (VIC 1)
    TEST_ASSERT_EQUAL_HEX8(0x43, buffer_81mhz[132]);
    TEST_ASSERT_EQUAL_HEX8(34, buffer_81mhz[133]);
    TEST_ASSERT_EQUAL_HEX8(4, buffer_81mhz[134]);
    TEST_ASSERT_EQUAL_HEX8(1, buffer_81mhz[135]);

    // For 150MHz: Should also contain 1080p60 (VIC 16)
    TEST_ASSERT_EQUAL_HEX8(0x44, buffer_150mhz[132]);
    TEST_ASSERT_EQUAL_HEX8(16, buffer_150mhz[133]);
    TEST_ASSERT_EQUAL_HEX8(34, buffer_150mhz[134]);
}

void test_edid_vendor_id_decode(void) {
    uint8_t buffer[256];
    tc358743_edid_build(buffer, sizeof(buffer), 81000);

    // Vendor ID at offset 8 and 9 (Little Endian)
    uint16_t vendor_id = buffer[8] | (buffer[9] << 8);

    char c1 = ((vendor_id >> 10) & 0x1F) + 'A' - 1;
    char c2 = ((vendor_id >> 5) & 0x1F) + 'A' - 1;
    char c3 = (vendor_id & 0x1F) + 'A' - 1;

    TEST_ASSERT_EQUAL_CHAR('E', c1);
    TEST_ASSERT_EQUAL_CHAR('S', c2);
    TEST_ASSERT_EQUAL_CHAR('P', c3);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_edid_generation_basic);
    RUN_TEST(test_edid_max_pixel_clock_filtering);
    RUN_TEST(test_edid_vendor_id_decode);
    return UNITY_END();
}
