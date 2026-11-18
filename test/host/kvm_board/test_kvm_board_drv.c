#include "unity.h"
#include "kvm_board_drv.h"
#include <string.h>

typedef enum {
    EV_SET_LVL = 1,
    EV_CFG_OUT,
    EV_CFG_IN,
    EV_TMR_STOP,
    EV_TMR_DISARM,
    EV_TMR_GET,
    EV_TMR_SET_CNT,
    EV_TMR_SET_ALM,
    EV_TMR_START,
} fake_ev_kind_t;

typedef struct {
    fake_ev_kind_t kind;
    int gpio;
    int level;
    uint64_t val;
} fake_ev_t;

typedef struct {
    kvm_board_drv_t drv;
    int gpio_level[KVM_BOARD_GPIO_MAX + 1];
    bool gpio_is_output[KVM_BOARD_GPIO_MAX + 1];
    int glitch_low_while_output;

    bool timer_running;
    bool timer_armed;
    uint64_t timer_count_us;
    uint64_t timer_alarm_us;

    fake_ev_t events[256];
    size_t event_count;

    int mutex_balance;
    int irq_balance;
    int gpio_low_events;

    /* Fault injection / race hooks */
    int fail_set_lvl_on_call;
    int set_lvl_calls;
    int fail_set_lvl_value; /* if >= 0, fail when setting that level */
    int fail_cfg_out_on_call;
    int cfg_out_calls;
    int fail_cfg_in_on_call;
    int cfg_in_calls;
    int get_lvl_fail_pin; /* -1 = none */
    bool fail_timer_get;
    bool fail_timer_set_cnt;
    bool fail_timer_set_alm;
    bool fail_timer_start;
    bool fire_alarm_during_start;
} fake_hw_t;

static void record_ev(fake_hw_t *f, fake_ev_kind_t kind, int gpio, int level, uint64_t val)
{
    if (f->event_count < sizeof(f->events) / sizeof(f->events[0])) {
        f->events[f->event_count++] = (fake_ev_t){
            .kind = kind,
            .gpio = gpio,
            .level = level,
            .val = val,
        };
    }
}

static void reset_trace(fake_hw_t *f)
{
    f->event_count = 0u;
    f->gpio_low_events = 0;
}

static int fake_gpio_set_level(void *ctx, int gpio, int level)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->set_lvl_calls++;
    if (f->fail_set_lvl_on_call > 0 && f->set_lvl_calls == f->fail_set_lvl_on_call) {
        return -1;
    }
    if (f->fail_set_lvl_value >= 0 && level == f->fail_set_lvl_value) {
        return -1;
    }
    f->gpio_level[gpio] = level;
    if (level == 0) {
        f->gpio_low_events++;
    }
    record_ev(f, EV_SET_LVL, gpio, level, 0u);
    return 0;
}

static int fake_gpio_config_output(void *ctx, int gpio)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->cfg_out_calls++;
    if (f->fail_cfg_out_on_call > 0 && f->cfg_out_calls == f->fail_cfg_out_on_call) {
        return -1;
    }
    f->gpio_is_output[gpio] = true;
    if (f->gpio_level[gpio] == 0 && (gpio == 15 || gpio == 19 || gpio == 20)) {
        f->glitch_low_while_output++;
    }
    record_ev(f, EV_CFG_OUT, gpio, f->gpio_level[gpio], 0u);
    return 0;
}

static int fake_gpio_config_input(void *ctx, int gpio)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->cfg_in_calls++;
    if (f->fail_cfg_in_on_call > 0 && f->cfg_in_calls == f->fail_cfg_in_on_call) {
        return -1;
    }
    f->gpio_is_output[gpio] = false;
    record_ev(f, EV_CFG_IN, gpio, 0, 0u);
    return 0;
}

static int fake_gpio_get_level(void *ctx, int gpio)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    if (f->get_lvl_fail_pin == gpio) {
        return -1;
    }
    return f->gpio_level[gpio];
}

static int fake_timer_stop(void *ctx)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->timer_running = false;
    record_ev(f, EV_TMR_STOP, -1, 0, 0u);
    return 0;
}

static int fake_timer_disarm(void *ctx)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->timer_armed = false;
    record_ev(f, EV_TMR_DISARM, -1, 0, 0u);
    return 0;
}

static int fake_timer_get_count(void *ctx, uint64_t *out_us)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    record_ev(f, EV_TMR_GET, -1, 0, f->timer_count_us);
    if (f->fail_timer_get) {
        return -1;
    }
    *out_us = f->timer_count_us;
    return 0;
}

static int fake_timer_set_count(void *ctx, uint64_t count_us)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    record_ev(f, EV_TMR_SET_CNT, -1, 0, count_us);
    if (f->fail_timer_set_cnt) {
        return -1;
    }
    f->timer_count_us = count_us;
    return 0;
}

static int fake_timer_set_alarm(void *ctx, uint64_t alarm_us)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    record_ev(f, EV_TMR_SET_ALM, -1, 0, alarm_us);
    if (f->fail_timer_set_alm) {
        return -1;
    }
    f->timer_alarm_us = alarm_us;
    f->timer_armed = true;
    return 0;
}

static int fake_timer_start(void *ctx)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    record_ev(f, EV_TMR_START, -1, 0, f->timer_alarm_us);
    if (f->fail_timer_start) {
        return -1;
    }
    f->timer_running = true;
    if (f->fire_alarm_during_start) {
        f->timer_count_us = f->timer_alarm_us;
        (void)kvm_board_drv_on_alarm(&f->drv, f->timer_count_us, f->timer_alarm_us);
    }
    return 0;
}

static int fake_mutex_lock(void *ctx)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->mutex_balance++;
    return 0;
}

static void fake_mutex_unlock(void *ctx)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->mutex_balance--;
}

static void fake_irq_lock(void *ctx)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->irq_balance++;
}

static void fake_irq_unlock(void *ctx)
{
    fake_hw_t *f = (fake_hw_t *)ctx;
    f->irq_balance--;
}

static kvm_board_logic_config_t make_cfg(void)
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
    return cfg;
}

static kvm_board_io_t make_io(fake_hw_t *f)
{
    return (kvm_board_io_t){
        .ctx = f,
        .gpio_set_level = fake_gpio_set_level,
        .gpio_config_output = fake_gpio_config_output,
        .gpio_config_input = fake_gpio_config_input,
        .gpio_get_level = fake_gpio_get_level,
        .timer_stop = fake_timer_stop,
        .timer_disarm = fake_timer_disarm,
        .timer_get_count = fake_timer_get_count,
        .timer_set_count = fake_timer_set_count,
        .timer_set_alarm = fake_timer_set_alarm,
        .timer_start = fake_timer_start,
        .mutex_lock = fake_mutex_lock,
        .mutex_unlock = fake_mutex_unlock,
        .irq_lock = fake_irq_lock,
        .irq_unlock = fake_irq_unlock,
    };
}

static void fake_hw_init(fake_hw_t *f)
{
    memset(f, 0, sizeof(*f));
    f->fail_set_lvl_value = -1;
    f->get_lvl_fail_pin = -1;
    f->timer_count_us = 1000u;
}

static int find_event(const fake_hw_t *f, fake_ev_kind_t kind, int gpio, int start_idx)
{
    for (size_t i = (size_t)start_idx; i < f->event_count; i++) {
        if (f->events[i].kind == kind && (gpio < 0 || f->events[i].gpio == gpio)) {
            return (int)i;
        }
    }
    return -1;
}

void setUp(void) {}
void tearDown(void) {}

static void test_drv_validation_and_lock_invariants(void)
{
    fake_hw_t f;
    fake_hw_init(&f);
    kvm_board_logic_config_t cfg = make_cfg();
    kvm_board_io_t io = make_io(&f);

    TEST_ASSERT_NULL(kvm_board_drv_hal(NULL));
    TEST_ASSERT_NULL(kvm_board_drv_hal(&f.drv));
    kvm_board_drv_deinit(NULL); /* safe no-op */

    TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(NULL, &cfg, &io));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, NULL, &io));
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, NULL));

    /* Each required callback missing individually must fail. */
    for (int i = 0; i < 10; i++) {
        kvm_board_io_t bad = io;
        if (i == 0) bad.gpio_set_level = NULL;
        if (i == 1) bad.gpio_config_output = NULL;
        if (i == 2) bad.gpio_config_input = NULL;
        if (i == 3) bad.gpio_get_level = NULL;
        if (i == 4) bad.timer_stop = NULL;
        if (i == 5) bad.timer_disarm = NULL;
        if (i == 6) bad.timer_get_count = NULL;
        if (i == 7) bad.timer_set_count = NULL;
        if (i == 8) bad.timer_set_alarm = NULL;
        if (i == 9) bad.timer_start = NULL;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, &bad));
    }

    /* Half-populated lock pairs must fail. */
    {
        kvm_board_io_t bad = io;
        bad.mutex_unlock = NULL;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, &bad));
    }
    {
        kvm_board_io_t bad = io;
        bad.mutex_lock = NULL;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, &bad));
    }
    {
        kvm_board_io_t bad = io;
        bad.irq_unlock = NULL;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, &bad));
    }
    {
        kvm_board_io_t bad = io;
        bad.irq_lock = NULL;
        TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, &bad));
    }

    /* Lock-free IO (all lock pointers NULL) works cleanly too. */
    kvm_board_io_t lockless = io;
    lockless.mutex_lock = NULL;
    lockless.mutex_unlock = NULL;
    lockless.irq_lock = NULL;
    lockless.irq_unlock = NULL;
    TEST_ASSERT_EQUAL_INT(0, kvm_board_drv_init(&f.drv, &cfg, &lockless));
    TEST_ASSERT_NOT_NULL(kvm_board_drv_hal(&f.drv));
    kvm_board_drv_deinit(&f.drv);
    TEST_ASSERT_NULL(kvm_board_drv_hal(&f.drv));
}

static void test_drv_init_glitch_free_order_and_failure_cleanup(void)
{
    fake_hw_t f;
    fake_hw_init(&f);
    kvm_board_logic_config_t cfg = make_cfg();
    kvm_board_io_t io = make_io(&f);

    /* Simulate E pins starting at 0 before init; preload must bring them High before output mode. */
    TEST_ASSERT_EQUAL_INT(0, kvm_board_drv_init(&f.drv, &cfg, &io));
    TEST_ASSERT_EQUAL_INT(0, f.glitch_low_while_output);
    TEST_ASSERT_NOT_NULL(kvm_board_drv_hal(&f.drv));

    const int e_pins[3] = { cfg.pins.e_power, cfg.pins.e_reset, cfg.pins.e_kvm };
    for (size_t i = 0; i < 3; i++) {
        int lvl_idx = find_event(&f, EV_SET_LVL, e_pins[i], 0);
        int cfg_idx = find_event(&f, EV_CFG_OUT, e_pins[i], 0);
        TEST_ASSERT_TRUE(lvl_idx >= 0);
        TEST_ASSERT_TRUE(cfg_idx > lvl_idx);
        TEST_ASSERT_EQUAL_INT(1, f.events[lvl_idx].level);
        TEST_ASSERT_EQUAL_INT(1, f.gpio_level[e_pins[i]]);
    }

    const int a_pins[2] = { cfg.pins.a0, cfg.pins.a1 };
    for (size_t i = 0; i < 2; i++) {
        int lvl_idx = find_event(&f, EV_SET_LVL, a_pins[i], 0);
        int cfg_idx = find_event(&f, EV_CFG_OUT, a_pins[i], 0);
        TEST_ASSERT_TRUE(lvl_idx >= 0);
        TEST_ASSERT_TRUE(cfg_idx > lvl_idx);
        TEST_ASSERT_EQUAL_INT(0, f.events[lvl_idx].level);
        TEST_ASSERT_EQUAL_INT(0, f.gpio_level[a_pins[i]]);
    }

    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(find_event(&f, EV_CFG_IN, cfg.pins.led_ch[i], 0) >= 0);
    }
    kvm_board_drv_deinit(&f.drv);

    /* Fault on 2nd output pin leaves 1st E pin High and driver not ready. */
    fake_hw_init(&f);
    f.fail_cfg_out_on_call = 2;
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, &io));
    TEST_ASSERT_NULL(kvm_board_drv_hal(&f.drv));
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_power]);

    /* Fault on LED input init leaves all 3 E pins High and driver not ready. */
    fake_hw_init(&f);
    f.fail_cfg_in_on_call = 2;
    TEST_ASSERT_EQUAL_INT(-1, kvm_board_drv_init(&f.drv, &cfg, &io));
    TEST_ASSERT_NULL(kvm_board_drv_hal(&f.drv));
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_power]);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_kvm]);
}

static void test_drv_assert_while_active_does_not_stop_running_timer(void)
{
    fake_hw_t f;
    fake_hw_init(&f);
    kvm_board_logic_config_t cfg = make_cfg();
    kvm_board_io_t io = make_io(&f);

    TEST_ASSERT_EQUAL_INT(0, kvm_board_drv_init(&f.drv, &cfg, &io));
    const kvm_board_hal_t *hal = kvm_board_drv_hal(&f.drv);

    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_POWER_FORCE, 1500u));
    TEST_ASSERT_TRUE(f.timer_running);
    TEST_ASSERT_EQUAL_INT(0, f.gpio_level[cfg.pins.e_power]);
    uint64_t alarm1 = f.timer_alarm_us;

    /* Review Major #1 regression: second assert_line while active MUST NOT touch timer! */
    reset_trace(&f);
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_RESET, 100u));
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_POWER, 100u));
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_COUNT, 100u));

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)f.event_count);
    TEST_ASSERT_TRUE(f.timer_running);
    TEST_ASSERT_EQUAL_INT(0, f.gpio_level[cfg.pins.e_power]);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_EQUAL_INT(0, f.mutex_balance);
    TEST_ASSERT_EQUAL_INT(0, f.irq_balance);

    /* Running timer's alarm fires cleanly and releases POWER_FORCE. */
    TEST_ASSERT_TRUE(kvm_board_drv_on_alarm(&f.drv, alarm1, alarm1));
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_power]);
    TEST_ASSERT_FALSE(f.drv.logic.has_active_line);
}

static void test_drv_assert_arms_timer_before_driving_low_and_handles_failures(void)
{
    fake_hw_t f;
    fake_hw_init(&f);
    kvm_board_logic_config_t cfg = make_cfg();
    kvm_board_io_t io = make_io(&f);

    TEST_ASSERT_EQUAL_INT(0, kvm_board_drv_init(&f.drv, &cfg, &io));
    const kvm_board_hal_t *hal = kvm_board_drv_hal(&f.drv);

    reset_trace(&f);
    f.timer_count_us = 2000u;
    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_POWER, 200u));

    int idx_stop = find_event(&f, EV_TMR_STOP, -1, 0);
    int idx_get = find_event(&f, EV_TMR_GET, -1, 0);
    int idx_alm = find_event(&f, EV_TMR_SET_ALM, -1, 0);
    int idx_start = find_event(&f, EV_TMR_START, -1, 0);
    int idx_low = find_event(&f, EV_SET_LVL, cfg.pins.e_power, 0);
    TEST_ASSERT_TRUE(idx_stop >= 0);
    TEST_ASSERT_TRUE(idx_get > idx_stop);
    TEST_ASSERT_TRUE(idx_alm > idx_get);
    TEST_ASSERT_TRUE(idx_start > idx_alm);
    TEST_ASSERT_TRUE(idx_low > idx_start);
    TEST_ASSERT_EQUAL_INT(0, f.events[idx_low].level);
    TEST_ASSERT_EQUAL_INT(0, hal->release_line(hal->ctx, KVM_BOARD_LINE_POWER));

    /* Failures during timer_get / set_count / set_alarm / start never drive E pin Low. */
    f.fail_timer_get = true;
    reset_trace(&f);
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_POWER, 100u));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_low_events);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_power]);
    f.fail_timer_get = false;

    /* Next assert with timer_count_us <= last_alarm_us forces timer_set_count. */
    f.timer_count_us = 5000u;
    f.fail_timer_set_cnt = true;
    reset_trace(&f);
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_RESET, 100u));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_low_events);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_FALSE(f.drv.logic.has_active_line);
    f.fail_timer_set_cnt = false;

    f.fail_timer_set_alm = true;
    reset_trace(&f);
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_RESET, 100u));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_low_events);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_FALSE(f.drv.logic.has_active_line);
    f.fail_timer_set_alm = false;

    f.fail_timer_start = true;
    reset_trace(&f);
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_KVM, 100u));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_low_events);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_kvm]);
    TEST_ASSERT_FALSE(f.drv.logic.has_active_line);
    f.fail_timer_start = false;

    /* Failure when driving GPIO Low cleanly releases logic and stops timer. */
    f.fail_set_lvl_value = 0;
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_KVM, 100u));
    f.fail_set_lvl_value = -1;
    TEST_ASSERT_FALSE(f.drv.logic.has_active_line);
    TEST_ASSERT_FALSE(f.timer_running);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_kvm]);
    TEST_ASSERT_EQUAL_INT(0, f.mutex_balance);
    TEST_ASSERT_EQUAL_INT(0, f.irq_balance);

    /* Driver remains healthy and usable after error recovery. */
    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_KVM, 100u));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_level[cfg.pins.e_kvm]);
    TEST_ASSERT_EQUAL_INT(0, hal->release_line(hal->ctx, KVM_BOARD_LINE_KVM));
}

static void test_drv_assert_race_alarm_before_low_never_drives_low(void)
{
    fake_hw_t f;
    fake_hw_init(&f);
    kvm_board_logic_config_t cfg = make_cfg();
    kvm_board_io_t io = make_io(&f);

    TEST_ASSERT_EQUAL_INT(0, kvm_board_drv_init(&f.drv, &cfg, &io));
    const kvm_board_hal_t *hal = kvm_board_drv_hal(&f.drv);

    reset_trace(&f);
    f.fire_alarm_during_start = true;
    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_RESET, 100u));

    TEST_ASSERT_EQUAL_INT(0, f.gpio_low_events);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_FALSE(f.drv.logic.has_active_line);
}

static void test_drv_delayed_old_isr_rejected_by_count_vs_alarm(void)
{
    fake_hw_t f;
    fake_hw_init(&f);
    kvm_board_logic_config_t cfg = make_cfg();
    kvm_board_io_t io = make_io(&f);

    TEST_ASSERT_NULL(kvm_board_drv_hal(&f.drv));
    TEST_ASSERT_FALSE(kvm_board_drv_on_alarm(NULL, 100u, 100u));

    TEST_ASSERT_EQUAL_INT(0, kvm_board_drv_init(&f.drv, &cfg, &io));
    const kvm_board_hal_t *hal = kvm_board_drv_hal(&f.drv);

    /* Pulse 1 armed long then released early; Pulse 2 armed immediately after. */
    f.timer_count_us = 1000u;
    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_POWER_FORCE, 5000u));
    TEST_ASSERT_EQUAL_INT(0, hal->release_line(hal->ctx, KVM_BOARD_LINE_POWER_FORCE));

    f.timer_count_us = 11000u;
    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_RESET, 100u));
    uint64_t base2 = f.timer_count_us;
    uint64_t alarm2 = f.timer_alarm_us;

    /* Review Minor #3 regression: IDF delayed ISR carries new alarm_value while count_value < alarm2! */
    TEST_ASSERT_FALSE(kvm_board_drv_on_alarm(&f.drv, base2 + 10u, alarm2));
    TEST_ASSERT_FALSE(kvm_board_drv_on_alarm(&f.drv, alarm2 - 1u, alarm2));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_TRUE(f.drv.logic.has_active_line);

    /* Reaching count_value >= alarm2 releases cleanly. */
    TEST_ASSERT_TRUE(kvm_board_drv_on_alarm(&f.drv, alarm2, alarm2));
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_FALSE(f.drv.logic.has_active_line);
}

static void test_drv_set_addr_release_all_leds_and_deinit(void)
{
    fake_hw_t f;
    fake_hw_init(&f);
    kvm_board_logic_config_t cfg = make_cfg();
    kvm_board_io_t io = make_io(&f);

    TEST_ASSERT_EQUAL_INT(0, kvm_board_drv_init(&f.drv, &cfg, &io));
    const kvm_board_hal_t *hal = kvm_board_drv_hal(&f.drv);

    TEST_ASSERT_EQUAL_INT(0, hal->set_addr(hal->ctx, 3u));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_level[cfg.pins.a0]);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.a1]);

    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_POWER, 200u));
    TEST_ASSERT_EQUAL_INT(-1, hal->set_addr(hal->ctx, 2u));
    TEST_ASSERT_EQUAL_INT(0, f.gpio_level[cfg.pins.a0]);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.a1]);

    /* Wrong-line release_line is no-op and MUST NOT stop timer! */
    reset_trace(&f);
    TEST_ASSERT_EQUAL_INT(0, hal->release_line(hal->ctx, KVM_BOARD_LINE_RESET));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)f.event_count);
    TEST_ASSERT_EQUAL_INT(0, f.gpio_level[cfg.pins.e_power]);

    /* Right-line release_line drives High and stops/disarms timer. */
    TEST_ASSERT_EQUAL_INT(0, hal->release_line(hal->ctx, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_power]);
    TEST_ASSERT_TRUE(find_event(&f, EV_TMR_STOP, -1, 0) >= 0);
    TEST_ASSERT_TRUE(find_event(&f, EV_TMR_DISARM, -1, 0) >= 0);

    /* LED decoding + error propagation. */
    f.gpio_level[cfg.pins.led_ch[0]] = 1;
    f.gpio_level[cfg.pins.led_ch[1]] = 0;
    f.gpio_level[cfg.pins.led_ch[2]] = 1;
    f.gpio_level[cfg.pins.led_ch[3]] = 1;
    uint8_t mask = 0u;
    TEST_ASSERT_EQUAL_INT(0, hal->read_led_mask(hal->ctx, &mask));
    TEST_ASSERT_EQUAL_UINT8(0x02u, mask);

    f.get_lvl_fail_pin = cfg.pins.led_ch[2];
    TEST_ASSERT_EQUAL_INT(-1, hal->read_led_mask(hal->ctx, &mask));
    TEST_ASSERT_EQUAL_UINT8(0x02u, mask);
    f.get_lvl_fail_pin = -1;
    TEST_ASSERT_EQUAL_INT(-1, hal->read_led_mask(hal->ctx, NULL));

    /* release_all releases active line and drives all 3 E pins High. */
    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_KVM, 300u));
    reset_trace(&f);
    TEST_ASSERT_EQUAL_INT(0, hal->release_all(hal->ctx));
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_power]);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_kvm]);
    TEST_ASSERT_TRUE(find_event(&f, EV_TMR_STOP, -1, 0) >= 0);
    TEST_ASSERT_TRUE(find_event(&f, EV_TMR_DISARM, -1, 0) >= 0);

    /* deinit during active pulse drives High and disables saved hal pointer. */
    TEST_ASSERT_EQUAL_INT(0, hal->assert_line(hal->ctx, KVM_BOARD_LINE_RESET, 100u));
    kvm_board_drv_deinit(&f.drv);
    TEST_ASSERT_NULL(kvm_board_drv_hal(&f.drv));
    TEST_ASSERT_EQUAL_INT(1, f.gpio_level[cfg.pins.e_reset]);
    TEST_ASSERT_FALSE(f.timer_running);
    TEST_ASSERT_EQUAL_INT(-1, hal->set_addr(hal->ctx, 1u));
    TEST_ASSERT_EQUAL_INT(-1, hal->assert_line(hal->ctx, KVM_BOARD_LINE_POWER, 50u));
    TEST_ASSERT_EQUAL_INT(-1, hal->release_line(hal->ctx, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL_INT(-1, hal->release_all(hal->ctx));
    TEST_ASSERT_EQUAL_INT(-1, hal->read_led_mask(hal->ctx, &mask));
    TEST_ASSERT_EQUAL_INT(0, f.mutex_balance);
    TEST_ASSERT_EQUAL_INT(0, f.irq_balance);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_drv_validation_and_lock_invariants);
    RUN_TEST(test_drv_init_glitch_free_order_and_failure_cleanup);
    RUN_TEST(test_drv_assert_while_active_does_not_stop_running_timer);
    RUN_TEST(test_drv_assert_arms_timer_before_driving_low_and_handles_failures);
    RUN_TEST(test_drv_assert_race_alarm_before_low_never_drives_low);
    RUN_TEST(test_drv_delayed_old_isr_rejected_by_count_vs_alarm);
    RUN_TEST(test_drv_set_addr_release_all_leds_and_deinit);
    return UNITY_END();
}
