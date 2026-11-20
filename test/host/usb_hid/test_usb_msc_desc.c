#include "unity.h"
#include "usb_hid_logic.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static uint16_t rd16_le(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static void assert_msc_desc_common(const uint8_t *cfg, size_t cfg_len,
                                   const uint8_t *hid_only, size_t hid_only_len,
                                   uint16_t expected_mps) {
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_NOT_NULL(hid_only);
    TEST_ASSERT_EQUAL_UINT(USB_HID_CONFIG_DESC_LEN, hid_only_len);
    TEST_ASSERT_EQUAL_UINT(USB_HID_MSC_CONFIG_DESC_LEN, cfg_len);
    TEST_ASSERT_EQUAL_UINT(107u, cfg_len);

    /* Configuration descriptor header (9B) */
    TEST_ASSERT_EQUAL_UINT8(9u, cfg[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02u, cfg[1]);
    TEST_ASSERT_EQUAL_UINT16(107u, rd16_le(&cfg[2]));
    TEST_ASSERT_EQUAL_UINT8(USB_HID_MSC_ITF_COUNT, cfg[4]);
    TEST_ASSERT_EQUAL_UINT8(4u, cfg[4]);
    TEST_ASSERT_EQUAL_UINT8(1u, cfg[5]);    /* bConfigurationValue */
    TEST_ASSERT_EQUAL_UINT8(0u, cfg[6]);    /* iConfiguration */
    TEST_ASSERT_EQUAL_UINT8(0xC0u, cfg[7]); /* self-powered */
    TEST_ASSERT_EQUAL_UINT8(0u, cfg[8]);    /* bMaxPower */

    /* Interfaces 0..2 match existing 84B HID-only descriptor identically */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&hid_only[9], &cfg[9], USB_HID_CONFIG_DESC_LEN - 9u);

    /* Interface 3: USB Mass Storage (SCSI transparent + BOT), offset 84 */
    const uint8_t *itf = &cfg[USB_HID_CONFIG_DESC_LEN];
    TEST_ASSERT_EQUAL_UINT8(9u, itf[0]);               /* bLength */
    TEST_ASSERT_EQUAL_UINT8(0x04u, itf[1]);            /* INTERFACE */
    TEST_ASSERT_EQUAL_UINT8(USB_MSC_ITF_NUM, itf[2]);  /* bInterfaceNumber = 3 */
    TEST_ASSERT_EQUAL_UINT8(0u, itf[3]);               /* bAlternateSetting */
    TEST_ASSERT_EQUAL_UINT8(2u, itf[4]);               /* bNumEndpoints = 2 */
    TEST_ASSERT_EQUAL_UINT8(0x08u, itf[5]);            /* Mass Storage */
    TEST_ASSERT_EQUAL_UINT8(0x06u, itf[6]);            /* SCSI transparent */
    TEST_ASSERT_EQUAL_UINT8(0x50u, itf[7]);            /* Bulk-Only Transport */
    TEST_ASSERT_EQUAL_UINT8(USB_MSC_STR_IDX, itf[8]);  /* iInterface = 7 */

    /* Bulk OUT endpoint, offset 93 */
    const uint8_t *ep_out = &cfg[USB_HID_CONFIG_DESC_LEN + 9u];
    TEST_ASSERT_EQUAL_UINT8(7u, ep_out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x05u, ep_out[1]);
    TEST_ASSERT_EQUAL_UINT8(USB_MSC_EP_OUT, ep_out[2]); /* 0x04 */
    TEST_ASSERT_EQUAL_UINT8(0x02u, ep_out[3]);          /* Bulk */
    TEST_ASSERT_EQUAL_UINT16(expected_mps, rd16_le(&ep_out[4]));
    TEST_ASSERT_EQUAL_UINT8(0u, ep_out[6]);

    /* Bulk IN endpoint, offset 100 */
    const uint8_t *ep_in = &cfg[USB_HID_CONFIG_DESC_LEN + 16u];
    TEST_ASSERT_EQUAL_UINT8(7u, ep_in[0]);
    TEST_ASSERT_EQUAL_UINT8(0x05u, ep_in[1]);
    TEST_ASSERT_EQUAL_UINT8(USB_MSC_EP_IN, ep_in[2]);   /* 0x84 */
    TEST_ASSERT_EQUAL_UINT8(0x02u, ep_in[3]);           /* Bulk */
    TEST_ASSERT_EQUAL_UINT16(expected_mps, rd16_le(&ep_in[4]));
    TEST_ASSERT_EQUAL_UINT8(0u, ep_in[6]);
}

void test_hid_only_descriptors_unchanged(void) {
    size_t fs_len = 0;
    size_t hs_len = 0;
    const uint8_t *fs = usb_hid_get_fs_config_desc(&fs_len);
    const uint8_t *hs = usb_hid_get_hs_config_desc(&hs_len);

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(hs);
    TEST_ASSERT_EQUAL_UINT(84u, fs_len);
    TEST_ASSERT_EQUAL_UINT(84u, hs_len);
    TEST_ASSERT_EQUAL_UINT16(84u, rd16_le(&fs[2]));
    TEST_ASSERT_EQUAL_UINT16(84u, rd16_le(&hs[2]));
    TEST_ASSERT_EQUAL_UINT8(3u, fs[4]);
    TEST_ASSERT_EQUAL_UINT8(3u, hs[4]);
}

void test_fs_config_desc_with_msc(void) {
    size_t msc_len = 0;
    size_t hid_len = 0;
    const uint8_t *msc = usb_hid_get_fs_config_desc_with_msc(&msc_len);
    const uint8_t *hid = usb_hid_get_fs_config_desc(&hid_len);
    assert_msc_desc_common(msc, msc_len, hid, hid_len, 64u);
}

void test_hs_config_desc_with_msc(void) {
    size_t msc_len = 0;
    size_t hid_len = 0;
    const uint8_t *msc = usb_hid_get_hs_config_desc_with_msc(&msc_len);
    const uint8_t *hid = usb_hid_get_hs_config_desc(&hid_len);
    assert_msc_desc_common(msc, msc_len, hid, hid_len, 512u);
}

void test_runtime_config_desc_fallback_when_msc_not_ready(void) {
    size_t hid_fs_len = 0, hid_hs_len = 0;
    const uint8_t *hid_fs = usb_hid_get_fs_config_desc(&hid_fs_len);
    const uint8_t *hid_hs = usb_hid_get_hs_config_desc(&hid_hs_len);

    size_t fs_len = 0, hs_len = 0;
    const uint8_t *fs_fallback = usb_hid_select_fs_config_desc(false, &fs_len);
    const uint8_t *hs_fallback = usb_hid_select_hs_config_desc(false, &hs_len);
    TEST_ASSERT_EQUAL_UINT(USB_HID_CONFIG_DESC_LEN, fs_len);
    TEST_ASSERT_EQUAL_UINT(USB_HID_CONFIG_DESC_LEN, hs_len);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_ITF_COUNT, fs_fallback[4]);
    TEST_ASSERT_EQUAL_UINT8(USB_HID_ITF_COUNT, hs_fallback[4]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(hid_fs, fs_fallback, USB_HID_CONFIG_DESC_LEN);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(hid_hs, hs_fallback, USB_HID_CONFIG_DESC_LEN);

    const uint8_t *fs_comp = usb_hid_select_fs_config_desc(true, &fs_len);
    const uint8_t *hs_comp = usb_hid_select_hs_config_desc(true, &hs_len);
    assert_msc_desc_common(fs_comp, fs_len, hid_fs, hid_fs_len, 64u);
    assert_msc_desc_common(hs_comp, hs_len, hid_hs, hid_hs_len, 512u);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hid_only_descriptors_unchanged);
    RUN_TEST(test_fs_config_desc_with_msc);
    RUN_TEST(test_hs_config_desc_with_msc);
    RUN_TEST(test_runtime_config_desc_fallback_when_msc_not_ready);
    return UNITY_END();
}
