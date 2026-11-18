#include "unity.h"
#include "kvm_board_logic.h"
#include <limits.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static kvm_board_logic_config_t make_valid_config(void)
{
    kvm_board_logic_config_t cfg;
    kvm_board_logic_config_default(&cfg);
    cfg.pins.a0 = 6;
    cfg.pins.a1 = 14;
    cfg.pins.e_power = 15;
    cfg.pins.e_reset = 19;
    cfg.pins.e_kvm = 20;
    cfg.pins.led_ch[0] = 21;
    cfg.pins.led_ch[1] = 22;
    cfg.pins.led_ch[2] = 23;
    cfg.pins.led_ch[3] = 54;
    cfg.reserved_gpio_mask = (1ULL << 16) | (1ULL << 18); /* X630 RESET + USB VBUS */
    return cfg;
}

static void test_default_config_is_fail_closed(void)
{
    kvm_board_logic_config_t cfg;
    kvm_board_logic_config_default(NULL); /* safe no-op */
    kvm_board_logic_config_default(&cfg);

    TEST_ASSERT_EQUAL_INT(-1, cfg.pins.a0);
    TEST_ASSERT_EQUAL_INT(-1, cfg.pins.a1);
    TEST_ASSERT_EQUAL_INT(-1, cfg.pins.e_power);
    TEST_ASSERT_EQUAL_INT(-1, cfg.pins.e_reset);
    TEST_ASSERT_EQUAL_INT(-1, cfg.pins.e_kvm);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(-1, cfg.pins.led_ch[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(KVM_ATX_HARD_MAX_POWER_MS, cfg.max_power_ms);
    TEST_ASSERT_EQUAL_UINT32(KVM_ATX_HARD_MAX_RESET_MS, cfg.max_reset_ms);
    TEST_ASSERT_EQUAL_UINT32(KVM_ATX_HARD_MAX_FORCE_MS, cfg.max_force_ms);
    TEST_ASSERT_EQUAL_UINT32(KVM_BOARD_DEFAULT_HARD_MAX_KVM_MS, cfg.max_kvm_ms);
    TEST_ASSERT_TRUE(cfg.led_active_low);
    TEST_ASSERT_FALSE(cfg.allow_jtag_inputs);
    TEST_ASSERT_EQUAL_UINT64(0u, cfg.reserved_gpio_mask);

    kvm_board_logic_t b;
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(NULL, &cfg));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, NULL));
}

static void test_pin_validation_rejects_bad_pins_duplicates_and_reserved(void)
{
    kvm_board_logic_t b;
    kvm_board_logic_config_t valid = make_valid_config();
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_init(&b, &valid));
    TEST_ASSERT_TRUE(b.initialized);
    TEST_ASSERT_EQUAL_UINT8(1u, b.current_channel);
    TEST_ASSERT_FALSE(b.has_active_line);

    /* Any pin slot left -1 or > 54 must fail. */
    int *const pin_ptrs[] = { NULL };
    (void)pin_ptrs;
    for (int slot = 0; slot < 9; slot++) {
        const int bad_vals[] = { -1, -5, KVM_BOARD_GPIO_MAX + 1 };
        for (size_t k = 0; k < sizeof(bad_vals) / sizeof(bad_vals[0]); k++) {
            kvm_board_logic_config_t cfg = make_valid_config();
            int *fields[9] = {
                &cfg.pins.a0, &cfg.pins.a1, &cfg.pins.e_power, &cfg.pins.e_reset, &cfg.pins.e_kvm,
                &cfg.pins.led_ch[0], &cfg.pins.led_ch[1], &cfg.pins.led_ch[2], &cfg.pins.led_ch[3]
            };
            *fields[slot] = bad_vals[k];
            TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
        }
    }

    /* Duplicate between output/output and output/input must fail. */
    {
        kvm_board_logic_config_t cfg = make_valid_config();
        cfg.pins.e_kvm = cfg.pins.a0;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
    }
    {
        kvm_board_logic_config_t cfg = make_valid_config();
        cfg.pins.led_ch[3] = cfg.pins.e_power;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
    }
    {
        kvm_board_logic_config_t cfg = make_valid_config();
        cfg.pins.led_ch[0] = cfg.pins.led_ch[1];
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
    }

    /* Hardware-reserved ESP32-P4-ETH pins (0,1,7,8,9..13,24..31,34..45,49..53) must fail everywhere. */
    static const int k_reserved[] = {
        0, 1, 7, 8, 9, 13, 24, 25, 26, 27, 28, 31, 34, 38, 39, 44, 45, 49, 52, 53
    };
    for (size_t i = 0; i < sizeof(k_reserved) / sizeof(k_reserved[0]); i++) {
        kvm_board_logic_config_t cfg_out = make_valid_config();
        cfg_out.pins.e_power = k_reserved[i];
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg_out));

        kvm_board_logic_config_t cfg_in = make_valid_config();
        cfg_in.pins.led_ch[0] = k_reserved[i];
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg_in));
    }

    /* Custom reserved_gpio_mask (GPIO16 / GPIO18) must fail on both output and LED input. */
    {
        kvm_board_logic_config_t cfg = make_valid_config();
        cfg.pins.e_reset = 16;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
    }
    {
        kvm_board_logic_config_t cfg = make_valid_config();
        cfg.pins.led_ch[2] = 18;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
    }

    /* Caution pins 2..5 (JTAG) & 32..33 (at-reset active) forbidden on outputs; 2..5 gated on inputs. */
    static const int k_caution[] = { 2, 3, 4, 5, 32, 33 };
    for (size_t i = 0; i < sizeof(k_caution) / sizeof(k_caution[0]); i++) {
        kvm_board_logic_config_t cfg_out = make_valid_config();
        cfg_out.allow_jtag_inputs = true; /* JTAG outputs STILL forbidden! */
        cfg_out.pins.e_kvm = k_caution[i];
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg_out));
    }
    for (int jtag = 2; jtag <= 5; jtag++) {
        kvm_board_logic_config_t cfg_in = make_valid_config();
        cfg_in.pins.led_ch[1] = jtag;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg_in));
        cfg_in.allow_jtag_inputs = true;
        TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_init(&b, &cfg_in));
    }
    {
        kvm_board_logic_config_t cfg = make_valid_config();
        cfg.pins.a0 = 46;
        cfg.pins.a1 = 47;
        cfg.pins.e_kvm = 48;
        cfg.pins.led_ch[0] = 32;
        cfg.pins.led_ch[1] = 33;
        TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_init(&b, &cfg));
    }
}

static void test_caps_validation_and_hard_clamping(void)
{
    kvm_board_logic_t b;
    for (int zero_idx = 0; zero_idx < 4; zero_idx++) {
        kvm_board_logic_config_t cfg = make_valid_config();
        if (zero_idx == 0) cfg.max_power_ms = 0u;
        if (zero_idx == 1) cfg.max_reset_ms = 0u;
        if (zero_idx == 2) cfg.max_force_ms = 0u;
        if (zero_idx == 3) cfg.max_kvm_ms = 0u;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_init(&b, &cfg));
    }

    kvm_board_logic_config_t over = make_valid_config();
    over.max_power_ms = 99999u;
    over.max_reset_ms = 99999u;
    over.max_force_ms = 99999u;
    over.max_kvm_ms = 99999u;
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_init(&b, &over));
    TEST_ASSERT_EQUAL_UINT32(KVM_ATX_HARD_MAX_POWER_MS, b.config.max_power_ms);
    TEST_ASSERT_EQUAL_UINT32(KVM_ATX_HARD_MAX_RESET_MS, b.config.max_reset_ms);
    TEST_ASSERT_EQUAL_UINT32(KVM_ATX_HARD_MAX_FORCE_MS, b.config.max_force_ms);
    TEST_ASSERT_EQUAL_UINT32(KVM_BOARD_HARD_MAX_KVM_CAP_MS, b.config.max_kvm_ms);

    const struct {
        kvm_board_line_t line;
        uint32_t expected_cap;
        int expected_gpio;
    } cases[] = {
        { KVM_BOARD_LINE_POWER,       KVM_ATX_HARD_MAX_POWER_MS,    over.pins.e_power },
        { KVM_BOARD_LINE_RESET,       KVM_ATX_HARD_MAX_RESET_MS,    over.pins.e_reset },
        { KVM_BOARD_LINE_POWER_FORCE, KVM_ATX_HARD_MAX_FORCE_MS,    over.pins.e_power },
        { KVM_BOARD_LINE_KVM,         KVM_BOARD_HARD_MAX_KVM_CAP_MS, over.pins.e_kvm },
    };

    uint64_t t = 1000u;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint64_t base_us = 0u, alarm_us = 0u;
        int gpio = -1;
        /* requested_max_ms == 0 clamps to cap */
        TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_assert(&b, cases[i].line, 0u, t,
                                                              &base_us, &alarm_us, &gpio));
        TEST_ASSERT_EQUAL_INT(cases[i].expected_gpio, gpio);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)cases[i].expected_cap * 1000ULL, alarm_us - base_us);
        bool released = false;
        int rel_gpio = -1;
        TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, cases[i].line, &released, &rel_gpio));
        TEST_ASSERT_TRUE(released);
        TEST_ASSERT_EQUAL_INT(cases[i].expected_gpio, rel_gpio);
        t = alarm_us + 10u;

        /* requested_max_ms > cap clamps to cap */
        TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_assert(&b, cases[i].line, 88888u, t,
                                                              &base_us, &alarm_us, &gpio));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)cases[i].expected_cap * 1000ULL, alarm_us - base_us);
        TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, cases[i].line, &released, &rel_gpio));
        t = alarm_us + 10u;
    }

    /* Shorter request honors requested_max_ms cleanly. */
    uint64_t base_us = 0u, alarm_us = 0u;
    int gpio = -1;
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_POWER, 50u, t,
                                                          &base_us, &alarm_us, &gpio));
    TEST_ASSERT_EQUAL_UINT64(50000ULL, alarm_us - base_us);
}

static void test_mutual_exclusion_addr_guard_and_shared_power_pin(void)
{
    bool a0 = true, a1 = true;
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_channel_to_addr(0u, &a0, &a1));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_channel_to_addr(5u, &a0, &a1));
    for (uint8_t ch = 1u; ch <= 4u; ch++) {
        TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_channel_to_addr(ch, &a0, &a1));
        TEST_ASSERT_EQUAL((ch - 1u) & 1u, a0 ? 1u : 0u);
        TEST_ASSERT_EQUAL(((ch - 1u) >> 1) & 1u, a1 ? 1u : 0u);
    }

    kvm_board_logic_t b;
    memset(&b, 0, sizeof(b));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_set_addr(&b, 1u, &a0, &a1));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_POWER, 100u, 10u, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_POWER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_release_all(&b, NULL));

    kvm_board_logic_config_t cfg = make_valid_config();
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_init(&b, &cfg));
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_set_addr(&b, 3u, &a0, &a1));
    TEST_ASSERT_FALSE(a0);
    TEST_ASSERT_TRUE(a1);
    TEST_ASSERT_EQUAL_UINT8(3u, b.current_channel);

    uint64_t base_us = 0u, alarm_us = 0u;
    int gpio = -1;
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_COUNT, 10u, 100u,
                                                           &base_us, &alarm_us, &gpio));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_COUNT, NULL, NULL));

    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_POWER_FORCE, 1000u, 100u,
                                                          &base_us, &alarm_us, &gpio));
    TEST_ASSERT_TRUE(b.has_active_line);

    /* Mutual exclusion: any second assert fails, set_addr fails and retains ch=3. */
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_POWER_FORCE, 100u, 200u,
                                                           &base_us, &alarm_us, &gpio));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_POWER, 100u, 200u,
                                                           &base_us, &alarm_us, &gpio));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_RESET, 100u, 200u,
                                                           &base_us, &alarm_us, &gpio));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_KVM, 100u, 200u,
                                                           &base_us, &alarm_us, &gpio));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_set_addr(&b, 2u, &a0, &a1));
    TEST_ASSERT_EQUAL_UINT8(3u, b.current_channel);

    /* Releasing wrong line (including POWER while POWER_FORCE owns e_power) must NOT release! */
    bool released = true;
    int rel_gpio = 0;
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_POWER, &released, &rel_gpio));
    TEST_ASSERT_FALSE(released);
    TEST_ASSERT_EQUAL_INT(-1, rel_gpio);
    TEST_ASSERT_TRUE(b.has_active_line);

    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_RESET, &released, &rel_gpio));
    TEST_ASSERT_FALSE(released);
    TEST_ASSERT_TRUE(b.has_active_line);

    /* Correct release frees line and allows subsequent set_addr. */
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_POWER_FORCE, &released, &rel_gpio));
    TEST_ASSERT_TRUE(released);
    TEST_ASSERT_EQUAL_INT(cfg.pins.e_power, rel_gpio);
    TEST_ASSERT_FALSE(b.has_active_line);

    /* Idempotent second release_line returns released=false. */
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_POWER_FORCE, &released, &rel_gpio));
    TEST_ASSERT_FALSE(released);
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_set_addr(&b, 4u, &a0, &a1));
    TEST_ASSERT_TRUE(a0);
    TEST_ASSERT_TRUE(a1);
}

static void test_monotonic_alarm_tokens_and_isr_races(void)
{
    kvm_board_logic_t b;
    kvm_board_logic_config_t cfg = make_valid_config();
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_init(&b, &cfg));

    /* Arm long POWER_FORCE (5000 ms), then release early at t=11 ms. */
    uint64_t base1 = 0u, alarm1 = 0u;
    int gpio1 = -1;
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_POWER_FORCE, 5000u, 1000u,
                                                          &base1, &alarm1, &gpio1));
    TEST_ASSERT_EQUAL_UINT64(1000u, base1);
    TEST_ASSERT_EQUAL_UINT64(5001000u, alarm1);

    bool released = false;
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_POWER_FORCE, &released, &gpio1));
    TEST_ASSERT_TRUE(released);

    /* Even with hardware raw count still at 11000 < alarm1, next base advances past alarm1! */
    uint64_t base2 = 0u, alarm2 = 0u;
    int gpio2 = -1;
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_RESET, 100u, 11000u,
                                                          &base2, &alarm2, &gpio2));
    TEST_ASSERT_EQUAL_UINT64(alarm1 + 1ULL, base2);
    TEST_ASSERT_EQUAL_UINT64(alarm1 + 1ULL + 100000ULL, alarm2);
    TEST_ASSERT_TRUE(alarm2 > alarm1);

    /* Stale ISR delivering alarm1 or 0 must NOT release RESET. */
    int isr_gpio = -1;
    TEST_ASSERT_FALSE(kvm_board_logic_on_timer_alarm(&b, 0u, &isr_gpio));
    TEST_ASSERT_FALSE(kvm_board_logic_on_timer_alarm(&b, alarm1, &isr_gpio));
    TEST_ASSERT_FALSE(kvm_board_logic_on_timer_alarm(&b, alarm2 + 1ULL, &isr_gpio));
    TEST_ASSERT_TRUE(b.has_active_line);
    TEST_ASSERT_EQUAL_INT(-1, isr_gpio);

    /* Valid ISR matching alarm2 auto-releases RESET cleanly. */
    TEST_ASSERT_TRUE(kvm_board_logic_on_timer_alarm(&b, alarm2, &isr_gpio));
    TEST_ASSERT_EQUAL_INT(cfg.pins.e_reset, isr_gpio);
    TEST_ASSERT_FALSE(b.has_active_line);

    /* Duplicate ISR delivery and post-auto-release task release_line are safe no-ops. */
    TEST_ASSERT_FALSE(kvm_board_logic_on_timer_alarm(&b, alarm2, &isr_gpio));
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_line(&b, KVM_BOARD_LINE_RESET, &released, &gpio2));
    TEST_ASSERT_FALSE(released);

    /* release_all invalidates active token so racing ISR does nothing. */
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_KVM, 150u, alarm2 + 500u,
                                                          &base2, &alarm2, &gpio2));
    int all_gpios[KVM_BOARD_NUM_ENABLE_LINES] = { -1, -1, -1 };
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_all(&b, all_gpios));
    TEST_ASSERT_EQUAL_INT(cfg.pins.e_power, all_gpios[0]);
    TEST_ASSERT_EQUAL_INT(cfg.pins.e_reset, all_gpios[1]);
    TEST_ASSERT_EQUAL_INT(cfg.pins.e_kvm, all_gpios[2]);
    TEST_ASSERT_FALSE(b.has_active_line);
    TEST_ASSERT_FALSE(kvm_board_logic_on_timer_alarm(&b, alarm2, &isr_gpio));

    /* Near UINT64_MAX overflow fails cleanly without latching active state. */
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_logic_begin_assert(&b, KVM_BOARD_LINE_POWER, 50u,
                                                           UINT64_MAX - 100ULL,
                                                           &base2, &alarm2, &gpio2));
    TEST_ASSERT_FALSE(b.has_active_line);
}

static void test_decode_leds_polarity_and_null(void)
{
    TEST_ASSERT_EQUAL_UINT8(0u, kvm_board_logic_decode_leds(NULL, true));
    TEST_ASSERT_EQUAL_UINT8(0u, kvm_board_logic_decode_leds(NULL, false));

    const bool levels1[4] = { false, true, true, true };
    TEST_ASSERT_EQUAL_UINT8(0x01u, kvm_board_logic_decode_leds(levels1, true));
    TEST_ASSERT_EQUAL_UINT8(0x0Eu, kvm_board_logic_decode_leds(levels1, false));

    const bool levels2[4] = { true, false, true, false };
    TEST_ASSERT_EQUAL_UINT8(0x0Au, kvm_board_logic_decode_leds(levels2, true));
    TEST_ASSERT_EQUAL_UINT8(0x05u, kvm_board_logic_decode_leds(levels2, false));

    const bool all_low[4] = { false, false, false, false };
    TEST_ASSERT_EQUAL_UINT8(0x0Fu, kvm_board_logic_decode_leds(all_low, true));
    TEST_ASSERT_EQUAL_UINT8(0x00u, kvm_board_logic_decode_leds(all_low, false));
}

static uint32_t next_rng(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void test_fuzz_state_machine_invariants(void)
{
    kvm_board_logic_t b;
    kvm_board_logic_config_t cfg = make_valid_config();
    TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_init(&b, &cfg));

    uint32_t rng = 0x13579BDFu;
    uint64_t sim_now_us = 1000u;
    uint64_t max_seen_alarm = 0u;
    bool exp_active = false;
    kvm_board_line_t exp_line = KVM_BOARD_LINE_COUNT;
    uint64_t exp_token = 0u;
    uint64_t stale_tokens[8] = { 0 };
    size_t stale_idx = 0u;

    for (int step = 0; step < 10000; step++) {
        uint32_t r = next_rng(&rng);
        sim_now_us += (r & 0x3FFu);
        uint32_t op = (r >> 10) % 5u;

        if (op == 0u) {
            uint8_t ch = (uint8_t)(((r >> 14) % 6u)); /* includes invalid 0 and 5 */
            bool a0 = false, a1 = false;
            int rc = kvm_board_logic_begin_set_addr(&b, ch, &a0, &a1);
            if (!exp_active && ch >= 1u && ch <= 4u) {
                TEST_ASSERT_EQUAL_INT(0, rc);
                TEST_ASSERT_EQUAL_UINT8(ch, b.current_channel);
            } else {
                TEST_ASSERT_EQUAL_INT(-1, rc);
            }
        } else if (op == 1u) {
            kvm_board_line_t line = (kvm_board_line_t)((r >> 16) % (KVM_BOARD_LINE_COUNT + 1u));
            uint32_t req_ms = ((r >> 20) % 2u == 0u) ? 0u : (((r >> 21) % 7000u) + 1u);
            /* Occasionally test non-advancing/smaller now_us relative to last alarm. */
            uint64_t now_arg = ((r & 0x80000000u) != 0u) ? (sim_now_us / 2u) : sim_now_us;
            uint64_t base_us = 0u, alarm_us = 0u;
            int gpio = -1;
            int rc = kvm_board_logic_begin_assert(&b, line, req_ms, now_arg, &base_us, &alarm_us, &gpio);
            if (!exp_active && line < KVM_BOARD_LINE_COUNT) {
                TEST_ASSERT_EQUAL_INT(0, rc);
                TEST_ASSERT_TRUE(alarm_us > max_seen_alarm);
                max_seen_alarm = alarm_us;
                uint32_t cap = (line == KVM_BOARD_LINE_POWER) ? KVM_ATX_HARD_MAX_POWER_MS
                             : (line == KVM_BOARD_LINE_RESET) ? KVM_ATX_HARD_MAX_RESET_MS
                             : (line == KVM_BOARD_LINE_POWER_FORCE) ? KVM_ATX_HARD_MAX_FORCE_MS
                             : KVM_BOARD_DEFAULT_HARD_MAX_KVM_MS;
                uint64_t dur = alarm_us - base_us;
                TEST_ASSERT_GREATER_OR_EQUAL_UINT64(1000ULL, dur);
                TEST_ASSERT_LESS_OR_EQUAL_UINT64((uint64_t)cap * 1000ULL, dur);
                exp_active = true;
                exp_line = line;
                exp_token = alarm_us;
                stale_tokens[stale_idx++ % 8u] = alarm_us;
            } else {
                TEST_ASSERT_EQUAL_INT(-1, rc);
            }
        } else if (op == 2u) {
            kvm_board_line_t line = (kvm_board_line_t)((r >> 16) % (KVM_BOARD_LINE_COUNT + 1u));
            bool released = false;
            int gpio = -1;
            int rc = kvm_board_logic_release_line(&b, line, &released, &gpio);
            if (line >= KVM_BOARD_LINE_COUNT) {
                TEST_ASSERT_EQUAL_INT(-1, rc);
            } else {
                TEST_ASSERT_EQUAL_INT(0, rc);
                if (exp_active && line == exp_line) {
                    TEST_ASSERT_TRUE(released);
                    exp_active = false;
                    exp_line = KVM_BOARD_LINE_COUNT;
                    exp_token = 0u;
                } else {
                    TEST_ASSERT_FALSE(released);
                }
            }
        } else if (op == 3u) {
            /* Alternate stale token vs current token ISR */
            uint64_t cand = ((r >> 18) % 2u == 0u) ? exp_token : stale_tokens[(r >> 22) % 8u];
            int gpio = -1;
            bool fired = kvm_board_logic_on_timer_alarm(&b, cand, &gpio);
            if (exp_active && cand != 0u && cand == exp_token) {
                TEST_ASSERT_TRUE(fired);
                TEST_ASSERT_TRUE(gpio >= 0);
                exp_active = false;
                exp_line = KVM_BOARD_LINE_COUNT;
                exp_token = 0u;
            } else {
                TEST_ASSERT_FALSE(fired);
            }
        } else {
            int gpios[KVM_BOARD_NUM_ENABLE_LINES] = { -1, -1, -1 };
            TEST_ASSERT_EQUAL_INT(0, kvm_board_logic_release_all(&b, gpios));
            exp_active = false;
            exp_line = KVM_BOARD_LINE_COUNT;
            exp_token = 0u;
        }

        TEST_ASSERT_EQUAL(exp_active, b.has_active_line);
        TEST_ASSERT_EQUAL(exp_line, b.active_line);
        TEST_ASSERT_EQUAL_UINT64(exp_token, b.active_alarm_token);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_config_is_fail_closed);
    RUN_TEST(test_pin_validation_rejects_bad_pins_duplicates_and_reserved);
    RUN_TEST(test_caps_validation_and_hard_clamping);
    RUN_TEST(test_mutual_exclusion_addr_guard_and_shared_power_pin);
    RUN_TEST(test_monotonic_alarm_tokens_and_isr_races);
    RUN_TEST(test_decode_leds_polarity_and_null);
    RUN_TEST(test_fuzz_state_machine_invariants);
    return UNITY_END();
}
