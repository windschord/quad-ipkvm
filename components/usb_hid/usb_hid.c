#include "usb_hid.h"
#include "usb_hid_logic.h"

#include "sdkconfig.h"

#if CONFIG_USB_DEVICE_ENABLE

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/usb_dwc_struct.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_vbus_ctrl.h"

#if CFG_TUD_HID < USB_HID_ITF_COUNT
#error "CONFIG_TINYUSB_HID_COUNT must be >= 3 when CONFIG_USB_DEVICE_ENABLE=y"
#endif

#ifndef CONFIG_USB_DEVICE_VBUS_SENSE_GPIO
#define CONFIG_USB_DEVICE_VBUS_SENSE_GPIO -1
#endif
#ifndef CONFIG_USB_DEVICE_VBUS_POLL_MS
#define CONFIG_USB_DEVICE_VBUS_POLL_MS 100
#endif

#define USB_HID_VBUS_TASK_STACK_SIZE 3072u
#define USB_HID_VBUS_TASK_PRIO       5u
#define USB_HID_VBUS_LOG_INTERVAL_MS 10000u
#define USB_HID_FORCE_OFF_DRAIN_US   200000LL

static const char *TAG = "usb_hid";

static usb_hid_mounted_cb_t s_on_mounted = NULL;
static bool s_initialized = false;
static bool s_force_uninstalled = false;
static usb_hid_gate_t s_hid_gate = {0};
static hid_sink_t s_sink;

static bool is_initialized(void) {
    return __atomic_load_n(&s_initialized, __ATOMIC_ACQUIRE);
}

static void set_initialized(bool val) {
    __atomic_store_n(&s_initialized, val, __ATOMIC_RELEASE);
}

static bool is_force_uninstalled(void) {
    return __atomic_load_n(&s_force_uninstalled, __ATOMIC_ACQUIRE);
}

static void set_force_uninstalled(bool val) {
    __atomic_store_n(&s_force_uninstalled, val, __ATOMIC_RELEASE);
}
static usb_hid_sink_ctx_t s_sink_ctx;
static char s_serial_str[17] = "QUADIPKVM0001";
static usb_vbus_ctrl_t s_vbus_ctrl;
static SemaphoreHandle_t s_usb_ctrl_mux = NULL;
#if CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0
static TaskHandle_t s_vbus_task = NULL;
static uint32_t s_uninstalled_connect_err_count = 0;
#endif

static portMUX_TYPE s_report_mux = portMUX_INITIALIZER_UNLOCKED;
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

#ifndef CONFIG_USB_DEVICE_START_DISCONNECTED
#define CONFIG_USB_DEVICE_START_DISCONNECTED 0
#endif

static void log_dwc_hs_regs(const char *stage) {
    usb_dwc_dctl_reg_t dctl = USB_DWC_HS.dctl_reg;
    ESP_LOGI(TAG,
             "OTG_HS regs (%s): gotgctl=0x%08" PRIx32 " dcfg=0x%08" PRIx32
             " dctl=0x%08" PRIx32 " (sftdiscon=%u) dsts=0x%08" PRIx32,
             stage,
             USB_DWC_HS.gotgctl_reg.val,
             USB_DWC_HS.dcfg_reg.val,
             dctl.val,
             (unsigned)dctl.sftdiscon,
             USB_DWC_HS.dsts_reg.val);
}

static bool hw_mounted(void *hw_ctx) {
    (void)hw_ctx;
    if (!usb_hid_gate_enter(&s_hid_gate)) {
        return false;
    }
    bool mounted = is_initialized() && tud_mounted();
    usb_hid_gate_leave(&s_hid_gate);
    return mounted;
}

static bool hw_itf_ready(void *hw_ctx, uint8_t itf) {
    (void)hw_ctx;
    if (itf >= USB_HID_ITF_COUNT || !usb_hid_gate_enter(&s_hid_gate)) {
        return false;
    }
    bool ready = is_initialized() && tud_hid_n_ready(itf);
    usb_hid_gate_leave(&s_hid_gate);
    return ready;
}

static bool hw_send_report(void *hw_ctx, uint8_t itf, uint8_t report_id,
                           const void *data, uint16_t len) {
    (void)hw_ctx;
    if (!data || itf >= USB_HID_ITF_COUNT || !usb_hid_gate_enter(&s_hid_gate)) {
        return false;
    }
    bool sent = is_initialized() && tud_hid_n_report(itf, report_id, data, len);
    usb_hid_gate_leave(&s_hid_gate);
    if (!sent) {
        return false;
    }
    portENTER_CRITICAL(&s_report_mux);
    if (itf == USB_HID_ITF_KEYBOARD && len == sizeof(s_last_kbd_report)) {
        memcpy(s_last_kbd_report, data, sizeof(s_last_kbd_report));
    } else if (itf == USB_HID_ITF_ABS_MOUSE && len == sizeof(s_last_abs_report)) {
        memcpy(s_last_abs_report, data, sizeof(s_last_abs_report));
    } else if (itf == USB_HID_ITF_REL_MOUSE && len == sizeof(s_last_rel_report)) {
        memcpy(s_last_rel_report, data, sizeof(s_last_rel_report));
    }
    portEXIT_CRITICAL(&s_report_mux);
    return true;
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
    if (!buffer || reqlen == 0) {
        return 0;
    }
    uint16_t copy_len = 0;
    portENTER_CRITICAL(&s_report_mux);
    if (report_type == HID_REPORT_TYPE_OUTPUT && instance == USB_HID_ITF_KEYBOARD && reqlen >= 1) {
        buffer[0] = s_last_kbd_leds;
        copy_len = 1;
    } else if (report_type == HID_REPORT_TYPE_INPUT) {
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
        if (src) {
            copy_len = (reqlen < src_len) ? reqlen : src_len;
            memcpy(buffer, src, copy_len);
        }
    }
    portEXIT_CRITICAL(&s_report_mux);
    return copy_len;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)report_id;
    if (instance == USB_HID_ITF_KEYBOARD &&
        report_type == HID_REPORT_TYPE_OUTPUT &&
        buffer && bufsize >= 1) {
        uint8_t leds = buffer[0];
        portENTER_CRITICAL(&s_report_mux);
        s_last_kbd_leds = leds;
        portEXIT_CRITICAL(&s_report_mux);
        ESP_LOGD(TAG, "Keyboard LEDs updated: 0x%02x", leds);
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

#if CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0
static bool is_utmi_phy_stopped(esp_err_t *out_err, usb_phy_status_t *out_st) {
    usb_phy_status_t phy_st = USB_PHY_STATUS_FREE;
    esp_err_t phy_err = usb_phy_get_phy_status(USB_PHY_TARGET_UTMI, &phy_st);
    if (out_err) {
        *out_err = phy_err;
    }
    if (out_st) {
        *out_st = phy_st;
    }
    /* When tinyusb_driver_uninstall() releases the last PHY handle, phy_uninstall()
     * frees the PHY controller object so usb_phy_get_phy_status() returns ESP_ERR_INVALID_STATE. */
    return (phy_err == ESP_ERR_INVALID_STATE) ||
           (phy_err == ESP_OK && phy_st == USB_PHY_STATUS_FREE);
}

static void log_vbus_stats(const char *reason, bool is_err) {
    usb_vbus_ctrl_stats_t st = {0};
    usb_vbus_ctrl_get_stats(&s_vbus_ctrl, &st);
    UBaseType_t hwm = s_vbus_task ? uxTaskGetStackHighWaterMark(NULL) : 0;
    ESP_LOG_LEVEL_LOCAL(
        is_err ? ESP_LOG_ERROR : ESP_LOG_INFO, TAG,
        "VBUS stats (%s): conn=%d uninstalled=%d connects=%" PRIu32
        " disconnects=%" PRIu32 " force_off=%" PRIu32
        " disc_fail=%" PRIu32 " failures=%" PRIu32 " stack_hwm=%u",
        reason, usb_vbus_ctrl_is_connected(&s_vbus_ctrl) ? 1 : 0,
        is_force_uninstalled() ? 1 : 0, st.connects, st.disconnects,
        st.force_off_count, st.disconnect_failure_count, st.failures,
        (unsigned)hwm);
}

static bool vbus_apply_action(usb_vbus_ctrl_action_t action) {
    switch (action) {
        case USB_VBUS_CTRL_ACTION_CONNECT: {
            if (is_force_uninstalled() || usb_hid_gate_is_closed(&s_hid_gate)) {
                s_uninstalled_connect_err_count++;
                if (s_uninstalled_connect_err_count == 1u ||
                    (s_uninstalled_connect_err_count % 100u) == 0u) {
                    ESP_LOGE(TAG,
                             "VBUS CONNECT rejected: TinyUSB driver uninstalled after stage-2 FORCE_OFF "
                             "(reboot required; count=%" PRIu32 ")",
                             s_uninstalled_connect_err_count);
                }
                return false;
            }
            bool call_ok = tud_connect();
            bool reg_ok = (USB_DWC_HS.dctl_reg.sftdiscon == 0);
            log_dwc_hs_regs("vbus-connect");
            if (!call_ok || !reg_ok) {
                ESP_LOGE(TAG, "VBUS CONNECT failed: tud_connect=%d sftdiscon=%u",
                         call_ok ? 1 : 0, (unsigned)USB_DWC_HS.dctl_reg.sftdiscon);
                return false;
            }
            return true;
        }
        case USB_VBUS_CTRL_ACTION_DISCONNECT: {
            if (is_force_uninstalled()) {
                return true;
            }
            if (usb_hid_gate_is_closed(&s_hid_gate)) {
                return false;
            }
            bool call_ok = tud_disconnect();
            bool reg_ok = (USB_DWC_HS.dctl_reg.sftdiscon == 1);
            log_dwc_hs_regs("vbus-disconnect");
            if (!call_ok || !reg_ok) {
                ESP_LOGE(TAG, "VBUS DISCONNECT failed: tud_disconnect=%d sftdiscon=%u",
                         call_ok ? 1 : 0, (unsigned)USB_DWC_HS.dctl_reg.sftdiscon);
                return false;
            }
            return true;
        }
        case USB_VBUS_CTRL_ACTION_FORCE_OFF: {
            if (is_force_uninstalled()) {
                usb_phy_status_t phy_st = USB_PHY_STATUS_FREE;
                esp_err_t phy_err = ESP_OK;
                bool phy_stopped = is_utmi_phy_stopped(&phy_err, &phy_st);
                if (!phy_stopped) {
                    ESP_LOGE(TAG,
                             "VBUS FORCE_OFF retry: driver already uninstalled, waiting for PHY stop "
                             "(phy_err=%s phy_status=%d)",
                             esp_err_to_name(phy_err), (int)phy_st);
                }
                return phy_stopped;
            }
            if (!usb_hid_gate_is_closed(&s_hid_gate)) {
                usb_dwc_dctl_reg_t dctl = USB_DWC_HS.dctl_reg;
                dctl.sftdiscon = 1;
                USB_DWC_HS.dctl_reg.val = dctl.val;
                bool stage1_ok = (USB_DWC_HS.dctl_reg.sftdiscon == 1);
                log_dwc_hs_regs("vbus-force-off-stage1");
                if (stage1_ok) {
                    ESP_LOGE(TAG, "VBUS FORCE_OFF stage-1 succeeded via direct DCTL.sftdiscon=1");
                    return true;
                }
                ESP_LOGE(TAG,
                         "VBUS FORCE_OFF stage-1 failed (sftdiscon=%u); uninstalling TinyUSB + UTMI PHY",
                         (unsigned)USB_DWC_HS.dctl_reg.sftdiscon);
            }
            /* Block all new external tud_* callers and drain active ones BEFORE uninstalling TinyUSB. */
            uint32_t inflight = usb_hid_gate_begin_teardown(&s_hid_gate);
            set_initialized(false);
            const int64_t drain_deadline = esp_timer_get_time() + USB_HID_FORCE_OFF_DRAIN_US;
            while (inflight > 0u && esp_timer_get_time() < drain_deadline) {
                vTaskDelay(1);
                inflight = usb_hid_gate_in_flight(&s_hid_gate);
            }
            if (inflight > 0u) {
                ESP_LOGE(TAG,
                         "VBUS FORCE_OFF stage-2 deferred: %" PRIu32 " in-flight HID call(s) active",
                         inflight);
                return false;
            }
            esp_err_t uninst_err = tinyusb_driver_uninstall();
            usb_phy_status_t phy_st = USB_PHY_STATUS_FREE;
            esp_err_t phy_err = ESP_OK;
            bool phy_stopped = is_utmi_phy_stopped(&phy_err, &phy_st);
            if (uninst_err == ESP_OK) {
                set_force_uninstalled(true);
            }
            bool stage2_ok = (uninst_err == ESP_OK) && phy_stopped;
            ESP_LOGE(TAG,
                     "VBUS FORCE_OFF stage-2: uninstall=%s phy_err=%s phy_status=%d (10s removal bound not guaranteed)",
                     esp_err_to_name(uninst_err), esp_err_to_name(phy_err), (int)phy_st);
            return stage2_ok;
        }
        default:
            return true;
    }
}

static void usb_vbus_task(void *pvParameters) {
    (void)pvParameters;
    uint64_t last_log_ms = (uint64_t)(esp_timer_get_time() / 1000);
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t poll_ticks = pdMS_TO_TICKS(CONFIG_USB_DEVICE_VBUS_POLL_MS) > 0
                                      ? pdMS_TO_TICKS(CONFIG_USB_DEVICE_VBUS_POLL_MS)
                                      : 1;

    while (1) {
        vTaskDelayUntil(&last_wake, poll_ticks);
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        bool valid = GPIO_IS_VALID_GPIO(CONFIG_USB_DEVICE_VBUS_SENSE_GPIO);
        bool present = valid ? (gpio_get_level((gpio_num_t)CONFIG_USB_DEVICE_VBUS_SENSE_GPIO) != 0) : false;

        xSemaphoreTake(s_usb_ctrl_mux, portMAX_DELAY);
        usb_vbus_ctrl_input_t in = {
            .now_ms = now_ms,
            .vbus_present = present,
            .sample_valid = valid,
        };
        usb_vbus_ctrl_action_t action = usb_vbus_ctrl_update(&s_vbus_ctrl, &in);
        if (action != USB_VBUS_CTRL_ACTION_NONE) {
            bool ok = vbus_apply_action(action);
            usb_vbus_ctrl_applied(&s_vbus_ctrl, action, ok);
            bool suppress_uninstalled_repeat =
                (action == USB_VBUS_CTRL_ACTION_CONNECT) &&
                (is_force_uninstalled() || usb_hid_gate_is_closed(&s_hid_gate)) &&
                (s_uninstalled_connect_err_count > 1u) &&
                ((s_uninstalled_connect_err_count % 100u) != 0u);
            if (!suppress_uninstalled_repeat) {
                log_vbus_stats(ok ? "action" : "action-fail",
                               !ok || action == USB_VBUS_CTRL_ACTION_FORCE_OFF);
                last_log_ms = now_ms;
            } else if (now_ms - last_log_ms >= USB_HID_VBUS_LOG_INTERVAL_MS) {
                log_vbus_stats("periodic", false);
                last_log_ms = now_ms;
            }
        } else if (now_ms - last_log_ms >= USB_HID_VBUS_LOG_INTERVAL_MS) {
            log_vbus_stats("periodic", false);
            last_log_ms = now_ms;
        }
        xSemaphoreGive(s_usb_ctrl_mux);
    }
}
#endif /* CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0 */

esp_err_t usb_hid_init(usb_hid_mounted_cb_t on_mounted) {
    if (is_initialized() || is_force_uninstalled() || usb_hid_gate_is_closed(&s_hid_gate)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_usb_ctrl_mux) {
        s_usb_ctrl_mux = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_usb_ctrl_mux != NULL, ESP_ERR_NO_MEM, TAG, "mutex create failed");
    }

#if CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(CONFIG_USB_DEVICE_VBUS_SENSE_GPIO),
                        ESP_ERR_INVALID_ARG, TAG,
                        "Invalid USB_DEVICE_VBUS_SENSE_GPIO=%d",
                        CONFIG_USB_DEVICE_VBUS_SENSE_GPIO);
    gpio_config_t vbus_gpio_cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)CONFIG_USB_DEVICE_VBUS_SENSE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&vbus_gpio_cfg), TAG, "VBUS GPIO config failed");
#endif

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
    /*
     * In espressif/esp_tinyusb v2.4.0 (tinyusb.c:1564-1595), setting phy.self_powered = true
     * triggers VBUS GPIO monitoring via tinyusb_vbus_monitor_init(), which rejects vbus_monitor_io == -1.
     * Keep phy.self_powered = false when relying on DWC2 GOTGCTL software BVALID override without a
     * direct VBUS pin into esp_tinyusb; external divided VBUS is polled by usb_vbus_ctrl instead.
     * TODO(hw-verify): confirm enumeration behavior with 0xC0 / 0 mA descriptor on target BIOS/PC.
     */
    tusb_cfg.phy.self_powered = false;
    tusb_cfg.phy.vbus_monitor_io = -1;
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.qualifier = &s_qualifier_desc;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = (int)(sizeof(s_string_desc) / sizeof(s_string_desc[0]));
    tusb_cfg.descriptor.full_speed_config = fs_cfg;
    tusb_cfg.descriptor.high_speed_config = hs_cfg;

    usb_hid_gate_init(&s_hid_gate);

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG,
                        "tinyusb_driver_install failed");
    log_dwc_hs_regs("post-install");

    bool boot_disc_ok = tud_disconnect();
    bool boot_reg_ok = (USB_DWC_HS.dctl_reg.sftdiscon == 1);
    log_dwc_hs_regs("soft-disconnect");
    if (!boot_disc_ok || !boot_reg_ok) {
        USB_DWC_HS.dctl_reg.sftdiscon = 1;
        boot_reg_ok = (USB_DWC_HS.dctl_reg.sftdiscon == 1);
        log_dwc_hs_regs("soft-disconnect-direct");
        if (!boot_reg_ok) {
            ESP_LOGE(TAG,
                     "Boot soft-disconnect failed (tud_disconnect=%d sftdiscon=%u); "
                     "starting VBUS ctrl as connected for DISCONNECT->FORCE_OFF recovery",
                     boot_disc_ok ? 1 : 0, (unsigned)USB_DWC_HS.dctl_reg.sftdiscon);
        }
    }

#if CONFIG_USB_DEVICE_BVALID_OVERRIDE
    /* 3-wire/4-wire J3 cable leaves J3 pin 1 disconnected: assert BVALID/VBVALID override in DWC2. */
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

    usb_vbus_ctrl_config_t vbus_cfg = usb_vbus_ctrl_config_default();
#if CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0
    s_uninstalled_connect_err_count = 0;
    vbus_cfg.enabled = true;
    usb_vbus_ctrl_init(&s_vbus_ctrl, &vbus_cfg);
    if (!boot_reg_ok) {
        usb_vbus_ctrl_applied(&s_vbus_ctrl, USB_VBUS_CTRL_ACTION_CONNECT, true);
    }
    log_dwc_hs_regs("vbus-wait-disconnected");
    ESP_LOGI(TAG, "USB soft-disconnected at boot; VBUS sense on GPIO %d (poll=%d ms)",
             CONFIG_USB_DEVICE_VBUS_SENSE_GPIO, CONFIG_USB_DEVICE_VBUS_POLL_MS);
    ESP_RETURN_ON_FALSE(usb_hid_gate_activate(&s_hid_gate), ESP_ERR_INVALID_STATE,
                        TAG, "usb_hid_gate_activate failed");
    set_initialized(true);
    if (xTaskCreate(usb_vbus_task, "usb_vbus", USB_HID_VBUS_TASK_STACK_SIZE, NULL,
                    USB_HID_VBUS_TASK_PRIO, &s_vbus_task) != pdPASS) {
        (void)usb_hid_gate_begin_teardown(&s_hid_gate);
        set_initialized(false);
        s_vbus_task = NULL;
        (void)tinyusb_driver_uninstall();
        usb_hid_gate_init(&s_hid_gate);
        return ESP_ERR_NO_MEM;
    }
#else
    (void)boot_reg_ok;
    vbus_cfg.enabled = false;
    usb_vbus_ctrl_init(&s_vbus_ctrl, &vbus_cfg);
    ESP_RETURN_ON_FALSE(usb_hid_gate_activate(&s_hid_gate), ESP_ERR_INVALID_STATE,
                        TAG, "usb_hid_gate_activate failed");
    set_initialized(true);
#if CONFIG_USB_DEVICE_START_DISCONNECTED
    log_dwc_hs_regs("start-disconnected");
    ESP_LOGI(TAG, "USB soft-disconnected at boot (USB_DEVICE_START_DISCONNECTED=y)");
#else
    tud_connect();
    log_dwc_hs_regs("post-config-connect");
#endif
#endif

    ESP_LOGI(TAG,
             "USB composite HID ready (VID=0x%04x PID=0x%04x fs=%s bvalid=%s vbus_gpio=%d serial=%s ready=%d)",
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
             CONFIG_USB_DEVICE_VBUS_SENSE_GPIO,
             s_serial_str,
             s_sink.ready(s_sink.ctx) ? 1 : 0);
    return ESP_OK;
}

const hid_sink_t *usb_hid_get_sink(void) {
    return is_initialized() ? &s_sink : NULL;
}

esp_err_t usb_hid_set_connected(bool connected) {
    ESP_RETURN_ON_FALSE(is_initialized() && !is_force_uninstalled() &&
                        !usb_hid_gate_is_closed(&s_hid_gate),
                        ESP_ERR_INVALID_STATE, TAG, "not initialized");
#if CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0
    (void)connected;
    ESP_LOGW(TAG, "usb_hid_set_connected() rejected while VBUS sensing (GPIO %d) governs connection",
             CONFIG_USB_DEVICE_VBUS_SENSE_GPIO);
    return ESP_ERR_INVALID_STATE;
#else
    xSemaphoreTake(s_usb_ctrl_mux, portMAX_DELAY);
    if (!is_initialized() || is_force_uninstalled() || !usb_hid_gate_enter(&s_hid_gate)) {
        xSemaphoreGive(s_usb_ctrl_mux);
        return ESP_ERR_INVALID_STATE;
    }
    bool ok = connected ? tud_connect() : tud_disconnect();
    usb_hid_gate_leave(&s_hid_gate);
    log_dwc_hs_regs(connected ? "connect" : "disconnect");
    xSemaphoreGive(s_usb_ctrl_mux);
    return ok ? ESP_OK : ESP_FAIL;
#endif
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
