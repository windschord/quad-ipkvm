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

    // Check Vendor ID for "ESP" (Big Endian)
    TEST_ASSERT_EQUAL_HEX8(0x16, buffer[8]);
    TEST_ASSERT_EQUAL_HEX8(0x70, buffer[9]);

    // Check Established Timings empty block
    TEST_ASSERT_EQUAL_HEX8(0x01, buffer[38]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buffer[53]);

    // Check Monitor Name Descriptor is present
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[72]);
    TEST_ASSERT_EQUAL_HEX8(0xFC, buffer[75]);
    TEST_ASSERT_EQUAL_HEX8('E', buffer[77]);

    // Check Dummy Descriptors
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[90]);
    TEST_ASSERT_EQUAL_HEX8(0x10, buffer[93]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[108]);
    TEST_ASSERT_EQUAL_HEX8(0x10, buffer[111]);

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

void test_edid_dtd_fallback(void) {
    uint8_t buffer_low_mhz[256];
    tc358743_edid_build(buffer_low_mhz, sizeof(buffer_low_mhz), 25000);

    // Should fallback to 640x480 (25.175 MHz)
    // pix_clk = 2518 -> 0x09D6
    TEST_ASSERT_EQUAL_HEX8(0xD5, buffer_low_mhz[54]);
    TEST_ASSERT_EQUAL_HEX8(0x09, buffer_low_mhz[55]);
}

void test_edid_vendor_id_decode(void) {
    uint8_t buffer[256];
    tc358743_edid_build(buffer, sizeof(buffer), 81000);

    // Vendor ID at offset 8 and 9 (Big Endian)
    uint16_t vendor_id = (buffer[8] << 8) | buffer[9];

    char c1 = ((vendor_id >> 10) & 0x1F) + 'A' - 1;
    char c2 = ((vendor_id >> 5) & 0x1F) + 'A' - 1;
    char c3 = (vendor_id & 0x1F) + 'A' - 1;

    TEST_ASSERT_EQUAL_CHAR('E', c1);
    TEST_ASSERT_EQUAL_CHAR('S', c2);
    TEST_ASSERT_EQUAL_CHAR('P', c3);
}

// "DEL" (0x10AC) decoder check
void test_edid_vendor_id_del_sam(void) {
    uint16_t del_id = 0x10AC;
    char c1 = ((del_id >> 10) & 0x1F) + 'A' - 1;
    char c2 = ((del_id >> 5) & 0x1F) + 'A' - 1;
    char c3 = (del_id & 0x1F) + 'A' - 1;
    TEST_ASSERT_EQUAL_CHAR('D', c1);
    TEST_ASSERT_EQUAL_CHAR('E', c2);
    TEST_ASSERT_EQUAL_CHAR('L', c3);

    uint16_t sam_id = 0x4C2D;
    c1 = ((sam_id >> 10) & 0x1F) + 'A' - 1;
    c2 = ((sam_id >> 5) & 0x1F) + 'A' - 1;
    c3 = (sam_id & 0x1F) + 'A' - 1;
    TEST_ASSERT_EQUAL_CHAR('S', c1);
    TEST_ASSERT_EQUAL_CHAR('A', c2);
    TEST_ASSERT_EQUAL_CHAR('M', c3);
}

void test_edid_dtd_refresh_rates(void);

void test_edid_profile_720p60(void) {
    uint8_t buffer[256];
    size_t len = tc358743_edid_build_profile(buffer, sizeof(buffer), TC358743_EDID_PROFILE_720P_60);
    TEST_ASSERT_EQUAL(256, len);

    // DTD Check for 720p60 (74.25 MHz -> 7425 = 0x1D01)
    TEST_ASSERT_EQUAL_HEX8(0x01, buffer[54]);
    TEST_ASSERT_EQUAL_HEX8(0x1D, buffer[55]);

    // Check VICs: only 4 (720p60) and 1 (480p60) should be present
    TEST_ASSERT_EQUAL_HEX8(0x42, buffer[132]);
    TEST_ASSERT_EQUAL_HEX8(4, buffer[133]);
    TEST_ASSERT_EQUAL_HEX8(1, buffer[134]);

    uint8_t sum1 = 0;
    for (int i = 0; i < 128; i++) sum1 += buffer[i];
    TEST_ASSERT_EQUAL_HEX8(0x00, sum1);

    uint8_t sum2 = 0;
    for (int i = 128; i < 256; i++) sum2 += buffer[i];
    TEST_ASSERT_EQUAL_HEX8(0x00, sum2);
}

void test_edid_profile_1080p30(void) {
    uint8_t buffer[256];
    size_t len = tc358743_edid_build_profile(buffer, sizeof(buffer), TC358743_EDID_PROFILE_1080P_30);
    TEST_ASSERT_EQUAL(256, len);

    // DTD Check for 1080p30 (74.25 MHz -> 7425 = 0x1D01)
    TEST_ASSERT_EQUAL_HEX8(0x01, buffer[54]);
    TEST_ASSERT_EQUAL_HEX8(0x1D, buffer[55]);

    // Check VICs: 34 (1080p30), 4 (720p60), and 1 (480p60) should be present
    TEST_ASSERT_EQUAL_HEX8(0x43, buffer[132]);
    TEST_ASSERT_EQUAL_HEX8(34, buffer[133]);
    TEST_ASSERT_EQUAL_HEX8(4, buffer[134]);
    TEST_ASSERT_EQUAL_HEX8(1, buffer[135]);

    uint8_t sum1 = 0;
    for (int i = 0; i < 128; i++) sum1 += buffer[i];
    TEST_ASSERT_EQUAL_HEX8(0x00, sum1);

    uint8_t sum2 = 0;
    for (int i = 128; i < 256; i++) sum2 += buffer[i];
    TEST_ASSERT_EQUAL_HEX8(0x00, sum2);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_edid_generation_basic);
    RUN_TEST(test_edid_max_pixel_clock_filtering);
    RUN_TEST(test_edid_dtd_fallback);
    RUN_TEST(test_edid_vendor_id_decode);
    RUN_TEST(test_edid_vendor_id_del_sam);
    RUN_TEST(test_edid_dtd_refresh_rates);
    RUN_TEST(test_edid_profile_720p60);
    RUN_TEST(test_edid_profile_1080p30);
    return UNITY_END();
}

void test_edid_dtd_refresh_rates(void) {
    uint8_t buffer_1080p[256];
    tc358743_edid_build(buffer_1080p, sizeof(buffer_1080p), 81000);

    // Parse 1080p30 DTD
    uint8_t* dtd1 = &buffer_1080p[54];
    uint16_t pix_clk_10k = dtd1[0] | (dtd1[1] << 8); // 10kHz unit
    uint16_t h_active = dtd1[2] | ((dtd1[4] & 0xF0) << 4);
    uint16_t h_blanking = dtd1[3] | ((dtd1[4] & 0x0F) << 8);
    uint16_t v_active = dtd1[5] | ((dtd1[7] & 0xF0) << 4);
    uint16_t v_blanking = dtd1[6] | ((dtd1[7] & 0x0F) << 8);

    float total_pixels_1 = (float)(h_active + h_blanking) * (float)(v_active + v_blanking);
    float refresh_1 = (float)(pix_clk_10k * 10000) / total_pixels_1;
    // Expected ~30.00 Hz
    TEST_ASSERT_FLOAT_WITHIN(0.01, 30.00, refresh_1);

    uint8_t buffer_480p[256];
    tc358743_edid_build(buffer_480p, sizeof(buffer_480p), 25000);

    // Parse 640x480@60 DTD
    uint8_t* dtd2 = &buffer_480p[54];
    uint16_t pix_clk_10k_2 = dtd2[0] | (dtd2[1] << 8); // 10kHz unit
    uint16_t h_active_2 = dtd2[2] | ((dtd2[4] & 0xF0) << 4);
    uint16_t h_blanking_2 = dtd2[3] | ((dtd2[4] & 0x0F) << 8);
    uint16_t v_active_2 = dtd2[5] | ((dtd2[7] & 0xF0) << 4);
    uint16_t v_blanking_2 = dtd2[6] | ((dtd2[7] & 0x0F) << 8);

    float total_pixels_2 = (float)(h_active_2 + h_blanking_2) * (float)(v_active_2 + v_blanking_2);
    float refresh_2 = (float)(pix_clk_10k_2 * 10000) / total_pixels_2;
    // Expected ~59.94 Hz (25.175 MHz / 420000) = 59.9404...
    TEST_ASSERT_FLOAT_WITHIN(0.02, 59.94, refresh_2);
}
