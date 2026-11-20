#include "kvm_board_hw.h"
#include "kvm_board_drv.h"

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

#if defined(CONFIG_KVM_BOARD_ENABLE) && CONFIG_KVM_BOARD_ENABLE
#if !defined(CONFIG_GPTIMER_ISR_CACHE_SAFE) || !CONFIG_GPTIMER_ISR_CACHE_SAFE
#error "CONFIG_KVM_BOARD_ENABLE requires CONFIG_GPTIMER_ISR_CACHE_SAFE=y"
#endif
#if !defined(CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM) || !CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM
#error "CONFIG_KVM_BOARD_ENABLE requires CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y"
#endif
_Static_assert(CONFIG_KVM_BOARD_HARD_MAX_POWER_MS <= KVM_ATX_HARD_MAX_POWER_MS,
               "POWER cap exceeds hard max");
_Static_assert(CONFIG_KVM_BOARD_HARD_MAX_RESET_MS <= KVM_ATX_HARD_MAX_RESET_MS,
               "RESET cap exceeds hard max");
_Static_assert(CONFIG_KVM_BOARD_HARD_MAX_FORCE_MS <= KVM_ATX_HARD_MAX_FORCE_MS,
               "FORCE cap exceeds hard max");
_Static_assert(CONFIG_KVM_BOARD_HARD_MAX_KVM_MS <= KVM_BOARD_HARD_MAX_KVM_CAP_MS,
               "KVM cap exceeds hard max");
#endif

static const char *TAG = "kvm_board";

typedef struct {
    kvm_board_drv_t drv;
    portMUX_TYPE spinlock;
    SemaphoreHandle_t mutex;
    gptimer_handle_t timer;
    gptimer_alarm_config_t alarm_cfg; /* Kept in internal .bss for CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM */
} kvm_board_hw_t;

static kvm_board_hw_t s_hw = {
    .spinlock = portMUX_INITIALIZER_UNLOCKED,
};

static inline __attribute__((always_inline)) void isr_gpio_set_high(int gpio)
{
    gpio_ll_set_level(&GPIO, (uint32_t)gpio, 1u);
}

/* Keep ISR path in internal IRAM with always_inline LL register writes and zero flash calls. */
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
    if (kvm_board_drv_eval_alarm(&hw->drv, edata->count_value, edata->alarm_value,
                                 &release_gpio)) {
        isr_gpio_set_high(release_gpio);
    }
    portEXIT_CRITICAL_ISR(&hw->spinlock);
    return false;
}

static int io_gpio_set_level(void *ctx, int gpio, int level)
{
    (void)ctx;
    if (gpio < 0 || gpio > KVM_BOARD_GPIO_MAX) {
        return -1;
    }
    gpio_ll_set_level(&GPIO, (uint32_t)gpio, level ? 1u : 0u);
    return 0;
}

static int configure_single_gpio(int gpio, gpio_mode_t mode)
{
    if (gpio < 0 || gpio > KVM_BOARD_GPIO_MAX) {
        return -1;
    }
    gpio_config_t gcfg = {
        .pin_bit_mask = 1ULL << (uint32_t)gpio,
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return (gpio_config(&gcfg) == ESP_OK) ? 0 : -1;
}

static int io_gpio_config_output(void *ctx, int gpio)
{
    (void)ctx;
    return configure_single_gpio(gpio, GPIO_MODE_OUTPUT);
}

static int io_gpio_config_input(void *ctx, int gpio)
{
    (void)ctx;
    return configure_single_gpio(gpio, GPIO_MODE_INPUT);
}

static int io_gpio_get_level(void *ctx, int gpio)
{
    (void)ctx;
    if (gpio < 0 || gpio > KVM_BOARD_GPIO_MAX) {
        return -1;
    }
    return gpio_get_level((gpio_num_t)gpio);
}

static int io_timer_stop(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    return (hw && hw->timer && gptimer_stop(hw->timer) == ESP_OK) ? 0 : -1;
}

static int io_timer_disarm(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    return (hw && hw->timer && gptimer_set_alarm_action(hw->timer, NULL) == ESP_OK) ? 0 : -1;
}

static int io_timer_get_count(void *ctx, uint64_t *out_us)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    return (hw && hw->timer && out_us && gptimer_get_raw_count(hw->timer, out_us) == ESP_OK) ? 0 : -1;
}

static int io_timer_set_count(void *ctx, uint64_t count_us)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    return (hw && hw->timer && gptimer_set_raw_count(hw->timer, count_us) == ESP_OK) ? 0 : -1;
}

static int io_timer_set_alarm(void *ctx, uint64_t alarm_us)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (!hw || !hw->timer) {
        return -1;
    }
    hw->alarm_cfg.alarm_count = alarm_us;
    hw->alarm_cfg.reload_count = 0u;
    hw->alarm_cfg.flags.auto_reload_on_alarm = false;
    return (gptimer_set_alarm_action(hw->timer, &hw->alarm_cfg) == ESP_OK) ? 0 : -1;
}

static int io_timer_start(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    return (hw && hw->timer && gptimer_start(hw->timer) == ESP_OK) ? 0 : -1;
}

static int io_mutex_lock(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    return (hw && hw->mutex && xSemaphoreTake(hw->mutex, portMAX_DELAY) == pdTRUE) ? 0 : -1;
}

static void io_mutex_unlock(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (hw && hw->mutex) {
        xSemaphoreGive(hw->mutex);
    }
}

static void io_irq_lock(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (hw) {
        portENTER_CRITICAL(&hw->spinlock);
    }
}

static void io_irq_unlock(void *ctx)
{
    kvm_board_hw_t *hw = (kvm_board_hw_t *)ctx;
    if (hw) {
        portEXIT_CRITICAL(&hw->spinlock);
    }
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
    if (s_hw.drv.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    kvm_board_logic_t validated;
    if (kvm_board_logic_init(&validated, cfg) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_hw, 0, sizeof(s_hw));
    portMUX_INITIALIZE(&s_hw.spinlock);

    s_hw.mutex = xSemaphoreCreateMutex();
    if (!s_hw.mutex) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_hardware_timer(&s_hw);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_hw.mutex);
        s_hw.mutex = NULL;
        return err;
    }

    const kvm_board_io_t io = {
        .ctx = &s_hw,
        .gpio_set_level = io_gpio_set_level,
        .gpio_config_output = io_gpio_config_output,
        .gpio_config_input = io_gpio_config_input,
        .gpio_get_level = io_gpio_get_level,
        .timer_stop = io_timer_stop,
        .timer_disarm = io_timer_disarm,
        .timer_get_count = io_timer_get_count,
        .timer_set_count = io_timer_set_count,
        .timer_set_alarm = io_timer_set_alarm,
        .timer_start = io_timer_start,
        .mutex_lock = io_mutex_lock,
        .mutex_unlock = io_mutex_unlock,
        .irq_lock = io_irq_lock,
        .irq_unlock = io_irq_unlock,
    };

    if (kvm_board_drv_init(&s_hw.drv, cfg, &io) != 0) {
        kvm_board_deinit();
        return ESP_FAIL;
    }

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
    return kvm_board_drv_hal(&s_hw.drv);
}

void kvm_board_deinit(void)
{
    kvm_board_drv_deinit(&s_hw.drv);
    if (s_hw.timer) {
        (void)gptimer_stop(s_hw.timer);
        (void)gptimer_set_alarm_action(s_hw.timer, NULL);
        (void)gptimer_disable(s_hw.timer);
        (void)gptimer_del_timer(s_hw.timer);
        s_hw.timer = NULL;
    }
    if (s_hw.mutex) {
        vSemaphoreDelete(s_hw.mutex);
        s_hw.mutex = NULL;
    }
    memset(&s_hw.alarm_cfg, 0, sizeof(s_hw.alarm_cfg));
    portMUX_INITIALIZE(&s_hw.spinlock);
}
