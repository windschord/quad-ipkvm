#include "kvm_board_hw.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/gpio_ll.h"
#include "sdkconfig.h"
#include "soc/gpio_struct.h"

static const char *TAG = "kvm_board";

typedef struct {
    kvm_board_logic_t logic;
    portMUX_TYPE spinlock;
    SemaphoreHandle_t mutex;
    gptimer_handle_t timer;
    gptimer_alarm_config_t alarm_cfg; /* Kept in internal .bss for CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM */
    kvm_board_hal_t hal;
    bool hw_ready;
} kvm_board_hw_t;

static kvm_board_hw_t s_hw = {
    .spinlock = portMUX_INITIALIZER_UNLOCKED,
};

static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *user_ctx)
{
    (void)timer;
    kvm_board_hw_t *hw = (kvm_board_hw_t *)user_ctx;
    if (!hw || !edata) {
        return false;
    }

    int release_gpio = -1;
    portENTER_CRITICAL_ISR(&hw->spinlock);
    if (kvm_board_logic_on_timer_alarm(&hw->logic, edata->alarm_value, &release_gpio) &&
        release_gpio >= 0) {
        gpio_ll_set_level(&GPIO, (uint32_t)release_gpio, 1u);
    }
    portEXIT_CRITICAL_ISR(&hw->spinlock);
    return false;
}

static void stop_timer_and_disarm(kvm_board_hw_t *hw)
{
    if (!hw->timer) {
        return;
    }
    (void)gptimer_stop(hw->timer);
    (void)gptimer_set_alarm_action(hw->timer, NULL);
}

static int release_all_locked(kvm_board_hw_t *hw)
{
    int gpios[KVM_BOARD_NUM_ENABLE_LINES] = { -1, -1, -1 };

    portENTER_CRITICAL(&hw->spinlock);
    int rc = kvm_board_logic_release_all(&hw->logic, gpios);
    if (rc == 0) {
        for (size_t i = 0; i < KVM_BOARD_NUM_ENABLE_LINES; i++) {
            if (gpios[i] >= 0) {
                gpio_ll_set_level(&GPIO, (uint32_t)gpios[i], 1u);
            }
        }
    }
    portEXIT_CRITICAL(&hw->spinlock);

    stop_timer_and_disarm(hw);
    return rc;
}

static int hal_set_addr(void *ctx, uint8_t ch)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (!hw || !hw->hw_ready || !hw->mutex) {
        return -1;
    }
    if (xSemaphoreTake(hw->mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    bool a0 = false;
    bool a1 = false;
    int a0_gpio = -1;
    int a1_gpio = -1;
    portENTER_CRITICAL(&hw->spinlock);
    int rc = kvm_board_logic_begin_set_addr(&hw->logic, ch, &a0, &a1);
    if (rc == 0) {
        a0_gpio = hw->logic.config.pins.a0;
        a1_gpio = hw->logic.config.pins.a1;
        gpio_ll_set_level(&GPIO, (uint32_t)a0_gpio, a0 ? 1u : 0u);
        gpio_ll_set_level(&GPIO, (uint32_t)a1_gpio, a1 ? 1u : 0u);
    }
    portEXIT_CRITICAL(&hw->spinlock);

    xSemaphoreGive(hw->mutex);
    return rc;
}

static int hal_assert_line(void *ctx, kvm_board_line_t line, uint32_t max_ms)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (!hw || !hw->hw_ready || !hw->mutex || !hw->timer) {
        return -1;
    }
    if (xSemaphoreTake(hw->mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    /* Stop any timer still coasting after an earlier auto-release ISR before reading count. */
    (void)gptimer_stop(hw->timer);

    uint64_t now_us = 0u;
    if (gptimer_get_raw_count(hw->timer, &now_us) != ESP_OK) {
        xSemaphoreGive(hw->mutex);
        return -1;
    }

    uint64_t base_us = 0u;
    uint64_t alarm_us = 0u;
    int gpio = -1;
    portENTER_CRITICAL(&hw->spinlock);
    int rc = kvm_board_logic_begin_assert(&hw->logic, line, max_ms, now_us,
                                          &base_us, &alarm_us, &gpio);
    portEXIT_CRITICAL(&hw->spinlock);
    if (rc != 0) {
        xSemaphoreGive(hw->mutex);
        return -1;
    }

    esp_err_t err = ESP_OK;
    if (base_us != now_us) {
        err = gptimer_set_raw_count(hw->timer, base_us);
    }
    if (err == ESP_OK) {
        hw->alarm_cfg.alarm_count = alarm_us;
        hw->alarm_cfg.reload_count = 0u;
        hw->alarm_cfg.flags.auto_reload_on_alarm = false;
        err = gptimer_set_alarm_action(hw->timer, &hw->alarm_cfg);
    }
    if (err == ESP_OK) {
        err = gptimer_start(hw->timer);
    }
    if (err != ESP_OK) {
        (void)release_all_locked(hw);
        xSemaphoreGive(hw->mutex);
        ESP_LOGE(TAG, "Failed to arm GPTimer deadline for line %d (%s); keeping E lines inactive",
                 (int)line, esp_err_to_name(err));
        return -1;
    }

    /* Drive active-low only after GPTimer is running and the pulse token is still live. */
    portENTER_CRITICAL(&hw->spinlock);
    if (hw->logic.has_active_line && hw->logic.active_alarm_token == alarm_us) {
        gpio_ll_set_level(&GPIO, (uint32_t)gpio, 0u);
    }
    portEXIT_CRITICAL(&hw->spinlock);

    xSemaphoreGive(hw->mutex);
    return 0;
}

static int hal_release_line(void *ctx, kvm_board_line_t line)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (!hw || !hw->hw_ready || !hw->mutex) {
        return -1;
    }
    if (xSemaphoreTake(hw->mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    bool released = false;
    int gpio = -1;
    portENTER_CRITICAL(&hw->spinlock);
    int rc = kvm_board_logic_release_line(&hw->logic, line, &released, &gpio);
    if (rc == 0 && released && gpio >= 0) {
        gpio_ll_set_level(&GPIO, (uint32_t)gpio, 1u);
    }
    portEXIT_CRITICAL(&hw->spinlock);

    if (rc == 0 && released) {
        stop_timer_and_disarm(hw);
    }

    xSemaphoreGive(hw->mutex);
    return rc;
}

static int hal_release_all(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (!hw || !hw->hw_ready || !hw->mutex) {
        return -1;
    }
    if (xSemaphoreTake(hw->mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    int rc = release_all_locked(hw);
    xSemaphoreGive(hw->mutex);
    return rc;
}

static int hal_read_led_mask(void *ctx, uint8_t *out_mask)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (!hw || !hw->hw_ready || !out_mask) {
        return -1;
    }
    bool raw[KVM_BOARD_NUM_CHANNELS] = { false, false, false, false };
    for (size_t i = 0; i < KVM_BOARD_NUM_CHANNELS; i++) {
        raw[i] = (gpio_get_level((gpio_num_t)hw->logic.config.pins.led_ch[i]) != 0);
    }
    *out_mask = kvm_board_logic_decode_leds(raw, hw->logic.config.led_active_low);
    return 0;
}

static esp_err_t configure_gpio_list(const int *gpios, size_t count, gpio_mode_t mode, int idle_level)
{
    uint64_t mask = 0u;
    for (size_t i = 0; i < count; i++) {
        if (gpios[i] < 0 || gpios[i] > KVM_BOARD_GPIO_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        if (mode == GPIO_MODE_OUTPUT) {
            /* Preload idle output latch before enabling pad output to prevent Low glitches. */
            esp_err_t lvl_err = gpio_set_level((gpio_num_t)gpios[i], (uint32_t)idle_level);
            if (lvl_err != ESP_OK) {
                return lvl_err;
            }
        }
        mask |= (1ULL << (uint32_t)gpios[i]);
    }

    gpio_config_t gcfg = {
        .pin_bit_mask = mask,
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gcfg);
    if (err != ESP_OK) {
        return err;
    }

    if (mode == GPIO_MODE_OUTPUT) {
        for (size_t i = 0; i < count; i++) {
            (void)gpio_set_level((gpio_num_t)gpios[i], (uint32_t)idle_level);
        }
    }
    return ESP_OK;
}

static esp_err_t init_hardware_timer(kvm_board_hw_t *hw)
{
    gptimer_config_t tcfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000u, /* 1 tick == 1 us */
    };
    esp_err_t err = gptimer_new_timer(&tcfg, &hw->timer);
    if (err != ESP_OK) {
        return err;
    }

    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_timer_alarm,
    };
    err = gptimer_register_event_callbacks(hw->timer, &cbs, hw);
    if (err != ESP_OK) {
        (void)gptimer_del_timer(hw->timer);
        hw->timer = NULL;
        return err;
    }

    err = gptimer_enable(hw->timer);
    if (err != ESP_OK) {
        (void)gptimer_del_timer(hw->timer);
        hw->timer = NULL;
        return err;
    }
    return ESP_OK;
}

esp_err_t kvm_board_init(const kvm_board_logic_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_hw.hw_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    kvm_board_logic_t validated;
    if (kvm_board_logic_init(&validated, cfg) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_hw, 0, sizeof(s_hw));
    portMUX_INITIALIZE(&s_hw.spinlock);
    s_hw.logic = validated;

    s_hw.mutex = xSemaphoreCreateMutex();
    if (!s_hw.mutex) {
        memset(&s_hw.logic, 0, sizeof(s_hw.logic));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_hardware_timer(&s_hw);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_hw.mutex);
        s_hw.mutex = NULL;
        memset(&s_hw.logic, 0, sizeof(s_hw.logic));
        return err;
    }

    const int enable_pins[KVM_BOARD_NUM_ENABLE_LINES] = {
        cfg->pins.e_power,
        cfg->pins.e_reset,
        cfg->pins.e_kvm,
    };
    const int addr_pins[2] = { cfg->pins.a0, cfg->pins.a1 };

    err = configure_gpio_list(enable_pins, KVM_BOARD_NUM_ENABLE_LINES, GPIO_MODE_OUTPUT, 1);
    if (err == ESP_OK) {
        err = configure_gpio_list(addr_pins, 2u, GPIO_MODE_OUTPUT, 0);
    }
    if (err == ESP_OK) {
        err = configure_gpio_list(cfg->pins.led_ch, KVM_BOARD_NUM_CHANNELS, GPIO_MODE_INPUT, 0);
    }
    if (err != ESP_OK) {
        kvm_board_deinit();
        return err;
    }

    s_hw.hal.ctx = &s_hw;
    s_hw.hal.set_addr = hal_set_addr;
    s_hw.hal.assert_line = hal_assert_line;
    s_hw.hal.release_line = hal_release_line;
    s_hw.hal.release_all = hal_release_all;
    s_hw.hal.read_led_mask = hal_read_led_mask;
    s_hw.hal.video_sync = NULL; /* kvm_service_start() wires video_status_hal_sync */
    s_hw.hw_ready = true;

    ESP_LOGI(TAG,
             "KVM board initialized: A0=%d A1=%d E_PWR=%d E_RST=%d E_KVM=%d LEDs=[%d,%d,%d,%d] led_active_low=%d",
             cfg->pins.a0, cfg->pins.a1, cfg->pins.e_power, cfg->pins.e_reset, cfg->pins.e_kvm,
             cfg->pins.led_ch[0], cfg->pins.led_ch[1], cfg->pins.led_ch[2], cfg->pins.led_ch[3],
             cfg->led_active_low ? 1 : 0);
    return ESP_OK;
}

esp_err_t kvm_board_init_from_kconfig(void)
{
#if defined(CONFIG_KVM_BOARD_ENABLE) && CONFIG_KVM_BOARD_ENABLE
    kvm_board_logic_config_t cfg;
    kvm_board_logic_config_default(&cfg);

    cfg.pins.a0 = CONFIG_KVM_BOARD_GPIO_A0;
    cfg.pins.a1 = CONFIG_KVM_BOARD_GPIO_A1;
    cfg.pins.e_power = CONFIG_KVM_BOARD_GPIO_E_POWER;
    cfg.pins.e_reset = CONFIG_KVM_BOARD_GPIO_E_RESET;
    cfg.pins.e_kvm = CONFIG_KVM_BOARD_GPIO_E_KVM;
    cfg.pins.led_ch[0] = CONFIG_KVM_BOARD_GPIO_LED_CH1;
    cfg.pins.led_ch[1] = CONFIG_KVM_BOARD_GPIO_LED_CH2;
    cfg.pins.led_ch[2] = CONFIG_KVM_BOARD_GPIO_LED_CH3;
    cfg.pins.led_ch[3] = CONFIG_KVM_BOARD_GPIO_LED_CH4;

    cfg.max_power_ms = CONFIG_KVM_BOARD_HARD_MAX_POWER_MS;
    cfg.max_reset_ms = CONFIG_KVM_BOARD_HARD_MAX_RESET_MS;
    cfg.max_force_ms = CONFIG_KVM_BOARD_HARD_MAX_FORCE_MS;
    cfg.max_kvm_ms = CONFIG_KVM_BOARD_HARD_MAX_KVM_MS;
#if defined(CONFIG_KVM_BOARD_LED_ACTIVE_LOW) && CONFIG_KVM_BOARD_LED_ACTIVE_LOW
    cfg.led_active_low = true;
#else
    cfg.led_active_low = false;
#endif
#if defined(CONFIG_KVM_BOARD_ALLOW_JTAG_INPUTS) && CONFIG_KVM_BOARD_ALLOW_JTAG_INPUTS
    cfg.allow_jtag_inputs = true;
#else
    cfg.allow_jtag_inputs = false;
#endif

    uint64_t reserved = 0u;
#if defined(CONFIG_TC358743_RESET_GPIO)
    if (CONFIG_TC358743_RESET_GPIO >= 0 && CONFIG_TC358743_RESET_GPIO <= KVM_BOARD_GPIO_MAX) {
        reserved |= (1ULL << (uint32_t)CONFIG_TC358743_RESET_GPIO);
    }
#endif
#if defined(CONFIG_USB_DEVICE_VBUS_SENSE_GPIO)
    if (CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0 &&
        CONFIG_USB_DEVICE_VBUS_SENSE_GPIO <= KVM_BOARD_GPIO_MAX) {
        reserved |= (1ULL << (uint32_t)CONFIG_USB_DEVICE_VBUS_SENSE_GPIO);
    }
#endif
    cfg.reserved_gpio_mask = reserved;

    return kvm_board_init(&cfg);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const kvm_board_hal_t *kvm_board_hal(void)
{
    return s_hw.hw_ready ? &s_hw.hal : NULL;
}

void kvm_board_deinit(void)
{
    if (s_hw.mutex && xSemaphoreTake(s_hw.mutex, portMAX_DELAY) == pdTRUE) {
        (void)release_all_locked(&s_hw);
        s_hw.hw_ready = false;
        if (s_hw.timer) {
            (void)gptimer_disable(s_hw.timer);
            (void)gptimer_del_timer(s_hw.timer);
            s_hw.timer = NULL;
        }
        xSemaphoreGive(s_hw.mutex);
        vSemaphoreDelete(s_hw.mutex);
        s_hw.mutex = NULL;
    } else if (s_hw.timer) {
        (void)gptimer_stop(s_hw.timer);
        (void)gptimer_set_alarm_action(s_hw.timer, NULL);
        (void)gptimer_disable(s_hw.timer);
        (void)gptimer_del_timer(s_hw.timer);
        s_hw.timer = NULL;
    }
    memset(&s_hw, 0, sizeof(s_hw));
    portMUX_INITIALIZE(&s_hw.spinlock);
}
