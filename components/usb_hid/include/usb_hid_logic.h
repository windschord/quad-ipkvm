#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_report.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB HID interface indices for the composite device.
 *
 * Keep each function on its own interface without Report IDs so BIOS/UEFI
 * boot drivers can bind Interface 0 (Boot Keyboard) and Interface 2 (Boot Mouse)
 * cleanly without parsing composite Report ID prefixes.
 */
#define USB_HID_ITF_KEYBOARD  0u
#define USB_HID_ITF_ABS_MOUSE 1u
#define USB_HID_ITF_REL_MOUSE 2u
#define USB_HID_ITF_COUNT     3u

/** @brief Interrupt IN endpoint addresses (ESP32-P4 OTG_HS supports EP0 + 7 IN EPs). */
#define USB_HID_EP_KEYBOARD   0x81u
#define USB_HID_EP_ABS_MOUSE  0x82u
#define USB_HID_EP_REL_MOUSE  0x83u

/** @brief Fixed input report byte lengths (no Report ID prefix byte). */
#define USB_HID_KBD_REPORT_LEN 8u
#define USB_HID_ABS_REPORT_LEN 6u
#define USB_HID_REL_REPORT_LEN 4u

/** @brief Composite configuration descriptor total length: 9B config + 3 * 25B HID. */
#define USB_HID_CONFIG_DESC_LEN 84u

/**
 * @brief Hardware operations bound into the non-blocking @ref hid_sink_t adapter.
 */
typedef struct {
    bool (*mounted)(void *hw_ctx);
    bool (*itf_ready)(void *hw_ctx, uint8_t itf);
    bool (*send_report)(void *hw_ctx, uint8_t itf, uint8_t report_id,
                        const void *data, uint16_t len);
} usb_hid_hw_ops_t;

/**
 * @brief Storage for the @ref hid_sink_t context pointer.
 */
typedef struct {
    const usb_hid_hw_ops_t *ops;
    void *hw_ctx;
} usb_hid_sink_ctx_t;

const uint8_t *usb_hid_get_kbd_report_desc(size_t *out_len);
const uint8_t *usb_hid_get_abs_report_desc(size_t *out_len);
const uint8_t *usb_hid_get_rel_report_desc(size_t *out_len);

/**
 * @brief Return the HID Report Descriptor for @p itf (0..USB_HID_ITF_COUNT-1), or NULL.
 */
const uint8_t *usb_hid_get_report_desc(uint8_t itf, size_t *out_len);

/**
 * @brief Return the Full-Speed (12 Mbps) Configuration Descriptor (84 bytes).
 */
const uint8_t *usb_hid_get_fs_config_desc(size_t *out_len);

/**
 * @brief Return the High-Speed (480 Mbps) Configuration Descriptor (84 bytes).
 */
const uint8_t *usb_hid_get_hs_config_desc(size_t *out_len);

/**
 * @brief Pack a 6KRO Boot Keyboard report (8 bytes, no Report ID).
 */
bool usb_hid_pack_keyboard(const hid_kbd_report_t *in,
                           uint8_t out[USB_HID_KBD_REPORT_LEN]);

/**
 * @brief Pack an Absolute Pointer report (6 bytes, 0..32767 LE + clamped wheel, no Report ID).
 */
bool usb_hid_pack_abs_mouse(const hid_abs_report_t *in,
                            uint8_t out[USB_HID_ABS_REPORT_LEN]);

/**
 * @brief Pack a Relative Boot Mouse report (4 bytes, clamped -127..127, no Report ID).
 */
bool usb_hid_pack_rel_mouse(const hid_rel_report_t *in,
                            uint8_t out[USB_HID_REL_REPORT_LEN]);

/**
 * @brief Initialize a non-blocking @ref hid_sink_t backed by @p ops.
 */
bool usb_hid_sink_init(hid_sink_t *out_sink, usb_hid_sink_ctx_t *ctx_storage,
                       const usb_hid_hw_ops_t *ops, void *hw_ctx);

#ifdef __cplusplus
}
#endif
