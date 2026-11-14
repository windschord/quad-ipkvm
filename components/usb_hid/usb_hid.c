#include "usb_hid.h"
#include "usb_hid_logic.h"

#include "sdkconfig.h"

#if CONFIG_USB_DEVICE_ENABLE

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "soc/usb_dwc_struct.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#if CFG_TUD_HID < USB_HID_ITF_COUNT
#error "CONFIG_TINYUSB_HID_COUNT must be >= 3 when CONFIG_USB_DEVICE_ENABLE=y"
#endif

static const char *TAG = "usb_hid";

static usb_hid_mounted_cb_t s_on_mounted = NULL;
static bool s_initialized = false;
static hid_sink_t s_sink;
static usb_hid_sink_ctx_t s_sink_ctx;
static char s_serial_str[17] = "QUADIPKVM0001";

static uint8_t s_last_kbd_report[USB_HID_KBD_REPORT_LEN];
static uint8_t s_last_abs_report[USB_HID_ABS_REPORT_LEN];
static uint8_t s_last_rel_report[USB_HID_REL_REPORT_LEN];
static uint8_t s_last_kbd_leds = 0;

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_USB_DEVICE_VID,
    .idProduct = CONFIG_USB_DEVICE_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_qualifier_desc = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

/* LangID (0x0409 LE) followed by UTF-8 descriptors up to USB_STRING_DESCRIPTOR_ARRAY_SIZE (8). */
static const char s_lang_id[3] = {0x09, 0x04, 0x00};
static const char *s_string_desc[] = {
    s_lang_id,
    CONFIG_USB_DEVICE_MANUFACTURER,
    CONFIG_USB_DEVICE_PRODUCT,
    s_serial_str,
    "Quad-IPKVM Boot Keyboard",
    "Quad-IPKVM Absolute Pointer",
    "Quad-IPKVM Relative Mouse",
};

static void log_dwc_hs_regs(const char *stage) {
    ESP_LOGI(TAG,
             "OTG_HS regs (%s): gotgctl=0x%08" PRIx32 " dcfg=0x%08" PRIx32
             " dctl=0x%08" PRIx32 " dsts=0x%08" PRIx32,
             stage,
             USB_DWC_HS.gotgctl_reg.val,
             USB_DWC_HS.dcfg_reg.val,
             USB_DWC_HS.dctl_reg.val,
             USB_DWC_HS.dsts_reg.val);
}

static bool hw_mounted(void *hw_ctx) {
    (void)hw_ctx;
    return s_initialized && tud_mounted();
}

static bool hw_itf_ready(void *hw_ctx, uint8_t itf) {
    (void)hw_ctx;
    return s_initialized && (itf < USB_HID_ITF_COUNT) && tud_hid_n_ready(itf);
}

static bool hw_send_report(void *hw_ctx, uint8_t itf, uint8_t report_id,
                           const void *data, uint16_t len) {
    (void)hw_ctx;
    if (!s_initialized || !data || itf >= USB_HID_ITF_COUNT) {
        return false;
    }
    if (itf == USB_HID_ITF_KEYBOARD && len == sizeof(s_last_kbd_report)) {
        memcpy(s_last_kbd_report, data, sizeof(s_last_kbd_report));
    } else if (itf == USB_HID_ITF_ABS_MOUSE && len == sizeof(s_last_abs_report)) {
        memcpy(s_last_abs_report, data, sizeof(s_last_abs_report));
    } else if (itf == USB_HID_ITF_REL_MOUSE && len == sizeof(s_last_rel_report)) {
        memcpy(s_last_rel_report, data, sizeof(s_last_rel_report));
    }
    return tud_hid_n_report(itf, report_id, data, len);
}

static const usb_hid_hw_ops_t s_hw_ops = {
    .mounted = hw_mounted,
    .itf_ready = hw_itf_ready,
    .send_report = hw_send_report,
};

static void usb_hid_event_cb(tinyusb_event_t *event, void *arg) {
    (void)arg;
    if (!event) {
        return;
    }
    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            ESP_LOGI(TAG, "USB attached (rhport=%u, speed=%d)",
                     (unsigned)event->rhport, (int)tud_speed_get());
            if (s_on_mounted) {
                s_on_mounted();
            }
            break;
        case TINYUSB_EVENT_DETACHED:
            ESP_LOGI(TAG, "USB detached (rhport=%u)", (unsigned)event->rhport);
            break;
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
        case TINYUSB_EVENT_SUSPENDED:
            ESP_LOGI(TAG, "USB suspended (remote_wakeup=%d)",
                     (int)event->suspended.remote_wakeup);
            break;
#endif
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
        case TINYUSB_EVENT_RESUMED:
            ESP_LOGI(TAG, "USB resumed (rhport=%u)", (unsigned)event->rhport);
            if (s_on_mounted && tud_mounted()) {
                s_on_mounted();
            }
            break;
#endif
        default:
            break;
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    return usb_hid_get_report_desc(instance, NULL);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)report_id;
    if (!buffer) {
        return 0;
    }
    if (report_type == HID_REPORT_TYPE_OUTPUT && instance == USB_HID_ITF_KEYBOARD && reqlen >= 1) {
        buffer[0] = s_last_kbd_leds;
        return 1;
    }
    if (report_type != HID_REPORT_TYPE_INPUT) {
        return 0;
    }
    const uint8_t *src = NULL;
    uint16_t src_len = 0;
    if (instance == USB_HID_ITF_KEYBOARD) {
        src = s_last_kbd_report;
        src_len = sizeof(s_last_kbd_report);
    } else if (instance == USB_HID_ITF_ABS_MOUSE) {
        src = s_last_abs_report;
        src_len = sizeof(s_last_abs_report);
    } else if (instance == USB_HID_ITF_REL_MOUSE) {
        src = s_last_rel_report;
        src_len = sizeof(s_last_rel_report);
    }
    if (!src || reqlen == 0) {
        return 0;
    }
    uint16_t copy_len = (reqlen < src_len) ? reqlen : src_len;
    memcpy(buffer, src, copy_len);
    return copy_len;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)report_id;
    if (instance == USB_HID_ITF_KEYBOARD &&
        report_type == HID_REPORT_TYPE_OUTPUT &&
        buffer && bufsize >= 1) {
        s_last_kbd_leds = buffer[0];
        ESP_LOGD(TAG, "Keyboard LEDs updated: 0x%02x", s_last_kbd_leds);
    }
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {
    ESP_LOGI(TAG, "HID itf %u SET_PROTOCOL -> %s",
             (unsigned)instance,
             (protocol == HID_PROTOCOL_BOOT) ? "BOOT(0)" : "REPORT(1)");
}

static void init_serial_from_mac(void) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK ||
        esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(s_serial_str, sizeof(s_serial_str),
                 "KVM%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

esp_err_t usb_hid_init(usb_hid_mounted_cb_t on_mounted) {
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    init_serial_from_mac();
    s_on_mounted = on_mounted;
    memset(s_last_kbd_report, 0, sizeof(s_last_kbd_report));
    memset(s_last_abs_report, 0, sizeof(s_last_abs_report));
    memset(s_last_rel_report, 0, sizeof(s_last_rel_report));
    s_last_kbd_leds = 0;

    ESP_RETURN_ON_FALSE(usb_hid_sink_init(&s_sink, &s_sink_ctx, &s_hw_ops, NULL),
                        ESP_FAIL, TAG, "usb_hid_sink_init failed");

    size_t fs_len = 0;
    size_t hs_len = 0;
    const uint8_t *fs_cfg = usb_hid_get_fs_config_desc(&fs_len);
    const uint8_t *hs_cfg = usb_hid_get_hs_config_desc(&hs_len);
    ESP_RETURN_ON_FALSE(fs_cfg && hs_cfg && fs_len == USB_HID_CONFIG_DESC_LEN &&
                        hs_len == USB_HID_CONFIG_DESC_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "Invalid config descriptor");

    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_HIGH_SPEED(usb_hid_event_cb, NULL);
    tusb_cfg.phy.self_powered = false;
    tusb_cfg.phy.vbus_monitor_io = -1;
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.qualifier = &s_qualifier_desc;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = (int)(sizeof(s_string_desc) / sizeof(s_string_desc[0]));
    tusb_cfg.descriptor.full_speed_config = fs_cfg;
    tusb_cfg.descriptor.high_speed_config = hs_cfg;

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG,
                        "tinyusb_driver_install failed");
    log_dwc_hs_regs("post-install");

    tud_disconnect();
    log_dwc_hs_regs("soft-disconnect");

#if CONFIG_USB_DEVICE_BVALID_OVERRIDE
    /* 3-wire J3 cable leaves VBUS (pin 1) floating: assert BVALID/VBVALID override in DWC2. */
    usb_dwc_gotgctl_reg_t gotgctl = USB_DWC_HS.gotgctl_reg;
    gotgctl.vbvalidoven = 1;
    gotgctl.vbvalidovval = 1;
    gotgctl.bvalidoven = 1;
    gotgctl.bvalidovval = 1;
    USB_DWC_HS.gotgctl_reg.val = gotgctl.val;
    log_dwc_hs_regs("bvalid-override");
#endif

#if CONFIG_USB_DEVICE_FORCE_FULL_SPEED
    /* DCFG.devspd = 1: Full-Speed (12 Mbps) on HS UTMI PHY for signal-integrity/BIOS fallback. */
    usb_dwc_dcfg_reg_t dcfg = USB_DWC_HS.dcfg_reg;
    dcfg.devspd = 1;
    USB_DWC_HS.dcfg_reg.val = dcfg.val;
    log_dwc_hs_regs("force-fs");
#endif

    tud_connect();
    log_dwc_hs_regs("post-config-connect");

    s_initialized = true;
    ESP_LOGI(TAG,
             "USB composite HID ready (VID=0x%04x PID=0x%04x fs=%s bvalid=%s serial=%s ready=%d)",
             CONFIG_USB_DEVICE_VID, CONFIG_USB_DEVICE_PID,
#if CONFIG_USB_DEVICE_FORCE_FULL_SPEED
             "on",
#else
             "off",
#endif
#if CONFIG_USB_DEVICE_BVALID_OVERRIDE
             "on",
#else
             "off",
#endif
             s_serial_str,
             s_sink.ready(s_sink.ctx) ? 1 : 0);
    return ESP_OK;
}

const hid_sink_t *usb_hid_get_sink(void) {
    return s_initialized ? &s_sink : NULL;
}

esp_err_t usb_hid_set_connected(bool connected) {
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    bool ok = connected ? tud_connect() : tud_disconnect();
    log_dwc_hs_regs(connected ? "connect" : "disconnect");
    return ok ? ESP_OK : ESP_FAIL;
}

#else /* !CONFIG_USB_DEVICE_ENABLE */

esp_err_t usb_hid_init(usb_hid_mounted_cb_t on_mounted) {
    (void)on_mounted;
    return ESP_ERR_NOT_SUPPORTED;
}

const hid_sink_t *usb_hid_get_sink(void) {
    return NULL;
}

esp_err_t usb_hid_set_connected(bool connected) {
    (void)connected;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_USB_DEVICE_ENABLE */
