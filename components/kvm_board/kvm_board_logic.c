#include "kvm_board_logic.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define KVM_BOARD_IRAM IRAM_ATTR
#else
#define KVM_BOARD_IRAM
#endif

static uint64_t gpio_bit(int gpio)
{
    if (gpio < 0 || gpio > KVM_BOARD_GPIO_MAX) {
        return 0u;
    }
    return 1ULL << (uint32_t)gpio;
}

static uint32_t clamp_cap_ms(uint32_t value, uint32_t hard_max)
{
    return (value > hard_max) ? hard_max : value;
}

KVM_BOARD_IRAM static int line_to_gpio(const kvm_board_pins_t *pins, kvm_board_line_t line)
{
    if (line == KVM_BOARD_LINE_POWER || line == KVM_BOARD_LINE_POWER_FORCE) {
        return pins->e_power;
    }
    if (line == KVM_BOARD_LINE_RESET) {
        return pins->e_reset;
    }
    if (line == KVM_BOARD_LINE_KVM) {
        return pins->e_kvm;
    }
    return -1;
}

static uint32_t line_cap_ms(const kvm_board_logic_t *b, kvm_board_line_t line)
{
    switch (line) {
    case KVM_BOARD_LINE_POWER:
        return b->config.max_power_ms;
    case KVM_BOARD_LINE_POWER_FORCE:
        return b->config.max_force_ms;
    case KVM_BOARD_LINE_RESET:
        return b->config.max_reset_ms;
    case KVM_BOARD_LINE_KVM:
        return b->config.max_kvm_ms;
    default:
        return 0u;
    }
}

uint64_t kvm_board_logic_fixed_reserved_mask(void)
{
    /*
     * Fixed Waveshare ESP32-P4-ETH bindings (docs/HW_VERIFY.md §B1/§B6):
     *   0, 1      : not exposed on P1 (UART/ADC)
     *   7, 8      : ESP_I2C_SDA / ESP_I2C_SCL (TC358743 / OLED / MCP23017)
     *   9..13     : ES8311 audio codec
     *   16, 17    : TC358743 RESET / INT (docs/PLAN.md §4.4, docs/HW_VERIFY.md B6)
     *   24, 25    : USB Serial/JTAG
     *   26, 27    : USB 2.0 FS (USB1P1_N1/P1)
     *   28..31    : Ethernet RMII PHY
     *   34..38    : Boot strapping / JTAG select
     *   39..44    : microSD 4-bit SDIO
     *   45        : microSD card power switch (Q1)
     *   49..52    : Ethernet RMII PHY
     *   53        : Other on-board circuit (not free on P1)
     */
    static const int k_fixed[] = {
        0, 1,
        7, 8,
        9, 10, 11, 12, 13,
        16, 17,
        24, 25, 26, 27,
        28, 29, 30, 31,
        34, 35, 36, 37, 38,
        39, 40, 41, 42, 43, 44,
        45,
        49, 50, 51, 52,
        53
    };
    uint64_t mask = 0u;
    for (size_t i = 0; i < sizeof(k_fixed) / sizeof(k_fixed[0]); i++) {
        mask |= gpio_bit(k_fixed[i]);
    }
    return mask;
}

uint64_t kvm_board_logic_output_forbidden_mask(void)
{
    /*
     * Caution pins from docs/HW_VERIFY.md §B1/§B2:
     *   2..5   : JTAG (weak pull-up on GPIO2 / input enabled at reset)
     *   32, 33 : Input-enabled at reset; keep outputs on safer high-Z/pull-up P1 pins
     */
    static const int k_caution[] = { 2, 3, 4, 5, 32, 33 };
    uint64_t mask = 0u;
    for (size_t i = 0; i < sizeof(k_caution) / sizeof(k_caution[0]); i++) {
        mask |= gpio_bit(k_caution[i]);
    }
    return mask;
}

void kvm_board_logic_config_default(kvm_board_logic_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->pins.a0 = -1;
    cfg->pins.a1 = -1;
    cfg->pins.e_power = -1;
    cfg->pins.e_reset = -1;
    cfg->pins.e_kvm = -1;
    for (size_t i = 0; i < KVM_BOARD_NUM_CHANNELS; i++) {
        cfg->pins.led_ch[i] = -1;
    }
    cfg->max_power_ms = KVM_ATX_HARD_MAX_POWER_MS;
    cfg->max_reset_ms = KVM_ATX_HARD_MAX_RESET_MS;
    cfg->max_force_ms = KVM_ATX_HARD_MAX_FORCE_MS;
    cfg->max_kvm_ms = KVM_BOARD_DEFAULT_HARD_MAX_KVM_MS;
    cfg->led_active_low = true;
    cfg->allow_jtag_inputs = false;
    cfg->reserved_gpio_mask = 0u;
}

static int validate_pin(int gpio, bool is_output, const kvm_board_logic_config_t *cfg,
                        uint64_t *seen_mask)
{
    uint64_t bit = gpio_bit(gpio);
    if (bit == 0u) {
        return -1;
    }
    if ((bit & (*seen_mask)) != 0u) {
        return -1;
    }
    uint64_t fixed = kvm_board_logic_fixed_reserved_mask() | cfg->reserved_gpio_mask;
    if ((bit & fixed) != 0u) {
        return -1;
    }
    if (is_output) {
        if ((bit & kvm_board_logic_output_forbidden_mask()) != 0u) {
            return -1;
        }
    } else if (!cfg->allow_jtag_inputs) {
        uint64_t jtag_mask = gpio_bit(2) | gpio_bit(3) | gpio_bit(4) | gpio_bit(5);
        if ((bit & jtag_mask) != 0u) {
            return -1;
        }
    }
    *seen_mask |= bit;
    return 0;
}

int kvm_board_logic_init(kvm_board_logic_t *b, const kvm_board_logic_config_t *cfg)
{
    if (!b || !cfg) {
        return -1;
    }
    memset(b, 0, sizeof(*b));

    if (cfg->max_power_ms == 0u || cfg->max_reset_ms == 0u ||
        cfg->max_force_ms == 0u || cfg->max_kvm_ms == 0u) {
        return -1;
    }

    const struct {
        int gpio;
        bool is_output;
    } pin_specs[] = {
        { cfg->pins.a0, true },
        { cfg->pins.a1, true },
        { cfg->pins.e_power, true },
        { cfg->pins.e_reset, true },
        { cfg->pins.e_kvm, true },
        { cfg->pins.led_ch[0], false },
        { cfg->pins.led_ch[1], false },
        { cfg->pins.led_ch[2], false },
        { cfg->pins.led_ch[3], false },
    };

    uint64_t seen_mask = 0u;
    for (size_t i = 0; i < sizeof(pin_specs) / sizeof(pin_specs[0]); i++) {
        if (validate_pin(pin_specs[i].gpio, pin_specs[i].is_output, cfg, &seen_mask) != 0) {
            return -1;
        }
    }

    b->config = *cfg;
    b->config.max_power_ms = clamp_cap_ms(cfg->max_power_ms, KVM_ATX_HARD_MAX_POWER_MS);
    b->config.max_reset_ms = clamp_cap_ms(cfg->max_reset_ms, KVM_ATX_HARD_MAX_RESET_MS);
    b->config.max_force_ms = clamp_cap_ms(cfg->max_force_ms, KVM_ATX_HARD_MAX_FORCE_MS);
    b->config.max_kvm_ms = clamp_cap_ms(cfg->max_kvm_ms, KVM_BOARD_HARD_MAX_KVM_CAP_MS);
    b->current_channel = 1u;
    b->has_active_line = false;
    b->active_line = KVM_BOARD_LINE_COUNT;
    b->last_alarm_us = 0u;
    b->active_alarm_token = 0u;
    b->initialized = true;
    return 0;
}

int kvm_board_logic_channel_to_addr(uint8_t ch, bool *out_a0, bool *out_a1)
{
    if (ch < 1u || ch > KVM_BOARD_NUM_CHANNELS) {
        return -1;
    }
    uint8_t code = (uint8_t)(ch - 1u);
    if (out_a0) {
        *out_a0 = (code & 0x01u) != 0u;
    }
    if (out_a1) {
        *out_a1 = ((code >> 1) & 0x01u) != 0u;
    }
    return 0;
}

int kvm_board_logic_begin_set_addr(kvm_board_logic_t *b, uint8_t ch, bool *out_a0, bool *out_a1)
{
    if (!b || !b->initialized || b->has_active_line) {
        return -1;
    }
    if (kvm_board_logic_channel_to_addr(ch, out_a0, out_a1) != 0) {
        return -1;
    }
    b->current_channel = ch;
    return 0;
}

int kvm_board_logic_begin_assert(kvm_board_logic_t *b, kvm_board_line_t line,
                                 uint32_t requested_max_ms, uint64_t now_us,
                                 uint64_t *out_base_us, uint64_t *out_alarm_us,
                                 int *out_gpio)
{
    if (!b || !b->initialized || b->has_active_line || line >= KVM_BOARD_LINE_COUNT) {
        return -1;
    }
    uint32_t cap = line_cap_ms(b, line);
    int gpio = line_to_gpio(&b->config.pins, line);
    if (cap == 0u || gpio < 0) {
        return -1;
    }

    uint32_t clamped_ms = (requested_max_ms == 0u || requested_max_ms > cap) ? cap : requested_max_ms;
    uint64_t duration_us = (uint64_t)clamped_ms * 1000ULL;
    /* Advance counter base past last_alarm_us so every assertion gets a strictly unique alarm token. */
    uint64_t base_us = (now_us > b->last_alarm_us) ? now_us : (b->last_alarm_us + 1ULL);
    if (base_us > UINT64_MAX - duration_us) {
        return -1;
    }
    uint64_t alarm_us = base_us + duration_us;

    b->has_active_line = true;
    b->active_line = line;
    b->last_alarm_us = alarm_us;
    b->active_alarm_token = alarm_us;

    if (out_base_us) {
        *out_base_us = base_us;
    }
    if (out_alarm_us) {
        *out_alarm_us = alarm_us;
    }
    if (out_gpio) {
        *out_gpio = gpio;
    }
    return 0;
}

int kvm_board_logic_release_line(kvm_board_logic_t *b, kvm_board_line_t line,
                                 bool *out_released, int *out_gpio)
{
    if (!b || !b->initialized || line >= KVM_BOARD_LINE_COUNT) {
        return -1;
    }
    bool released = false;
    int gpio = -1;
    if (b->has_active_line && b->active_line == line) {
        gpio = line_to_gpio(&b->config.pins, line);
        b->has_active_line = false;
        b->active_line = KVM_BOARD_LINE_COUNT;
        b->active_alarm_token = 0u;
        released = true;
    }
    if (out_released) {
        *out_released = released;
    }
    if (out_gpio) {
        *out_gpio = gpio;
    }
    return 0;
}

KVM_BOARD_IRAM bool kvm_board_logic_on_timer_alarm(kvm_board_logic_t *b, uint64_t alarm_value,
                                                   int *out_gpio)
{
    if (!b || !b->initialized || !b->has_active_line ||
        b->active_alarm_token != alarm_value) {
        return false;
    }
    int gpio = line_to_gpio(&b->config.pins, b->active_line);
    b->has_active_line = false;
    b->active_line = KVM_BOARD_LINE_COUNT;
    b->active_alarm_token = 0u;
    if (out_gpio) {
        *out_gpio = gpio;
    }
    return true;
}

int kvm_board_logic_release_all(kvm_board_logic_t *b, int out_gpios[KVM_BOARD_NUM_ENABLE_LINES])
{
    if (!b || !b->initialized) {
        return -1;
    }
    b->has_active_line = false;
    b->active_line = KVM_BOARD_LINE_COUNT;
    b->active_alarm_token = 0u;
    if (out_gpios) {
        out_gpios[0] = b->config.pins.e_power;
        out_gpios[1] = b->config.pins.e_reset;
        out_gpios[2] = b->config.pins.e_kvm;
    }
    return 0;
}

uint8_t kvm_board_logic_decode_leds(const bool raw_levels[KVM_BOARD_NUM_CHANNELS], bool active_low)
{
    if (!raw_levels) {
        return 0u;
    }
    uint8_t mask = 0u;
    for (uint8_t i = 0u; i < KVM_BOARD_NUM_CHANNELS; i++) {
        bool lit = active_low ? !raw_levels[i] : raw_levels[i];
        if (lit) {
            mask |= (uint8_t)(1u << i);
        }
    }
    return mask;
}
