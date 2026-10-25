#include "unity.h"
#include "mcp23017_logic.h"
#include <string.h>

typedef struct {
    uint8_t regs[22];
    bool inject_error;
    bool inject_reset;
    uint32_t write_count;
    struct {
        uint8_t reg;
        uint8_t val;
    } write_history[100];
    int reset_line_state;

    // For verifying invariant
    bool violated_invariant;
} fake_mcp_t;

static fake_mcp_t g_fake;

static void reset_fake(void) {
    memset(&g_fake, 0, sizeof(fake_mcp_t));
    g_fake.regs[MCP23017_REG_IODIRA] = 0xFF;
    g_fake.regs[MCP23017_REG_IODIRB] = 0xFF;
    g_fake.reset_line_state = -1;
}

static int fake_set_reset_line(void* user, int level) {
    fake_mcp_t* fake = (fake_mcp_t*)user;
    fake->reset_line_state = level;
    if (level == 0) {
        g_fake.regs[MCP23017_REG_IODIRA] = 0xFF;
        g_fake.regs[MCP23017_REG_IODIRB] = 0xFF;
        g_fake.regs[MCP23017_REG_OLATA] = 0x00;
        // other resets...
    }
    return 0;
}

static int fake_write_reg(void* user, uint8_t reg, uint8_t value) {
    fake_mcp_t* fake = (fake_mcp_t*)user;
    if (fake->inject_error) return -1;
    if (fake->write_count < 100) {
        fake->write_history[fake->write_count].reg = reg;
        fake->write_history[fake->write_count].val = value;
        fake->write_count++;
    }

    if (reg == MCP23017_REG_IODIRA) {
        // Invariant: Upper 4 bits of IODIRA must be 1 (input)
        if ((value & 0xF0) != 0xF0) {
            fake->violated_invariant = true;
        }
        // Invariant: Before making A0-A3 outputs, OLATA must have the expected off state
        // This is handled by verifying OLATA value at this time in the test
    } else if (reg == MCP23017_REG_IODIRB) {
        // Invariant: All bits of IODIRB must be 1 (input)
        if (value != 0xFF) {
            fake->violated_invariant = true;
        }
    }

    fake->regs[reg] = value;
    return 0;
}

static int fake_read_reg(void* user, uint8_t reg, uint8_t* value) {
    fake_mcp_t* fake = (fake_mcp_t*)user;
    if (fake->inject_error) return -1;

    if (fake->inject_reset) {
        // Simulate a reset by returning defaults
        if (reg == MCP23017_REG_IODIRA || reg == MCP23017_REG_IODIRB) {
            *value = 0xFF;
        } else {
            *value = 0x00;
        }
        return 0;
    }

    *value = fake->regs[reg];
    return 0;
}

static int fake_read_regs(void* user, uint8_t start_reg, uint8_t* buf, uint8_t len) {
    fake_mcp_t* fake = (fake_mcp_t*)user;
    if (fake->inject_error) return -1;
    for (int i = 0; i < len; i++) {
        if (fake_read_reg(user, start_reg + i, &buf[i]) != 0) return -1;
    }
    return 0;
}

static mcp23017_config_t default_cfg = {
    .write_reg = fake_write_reg,
    .read_reg = fake_read_reg,
    .read_regs = fake_read_regs,
    .set_reset_line = fake_set_reset_line,
    .user = &g_fake,
    .led_active_high = true,
    .debounce_ms = 20,
    .max_consecutive_errors = 5,
    .verify_interval_polls = 100
};

void setUp(void) {
    reset_fake();
}
void tearDown(void) {}

static void verify_init_order(fake_mcp_t* fake, bool led_active_high) {
    // 1. set_reset_line(1) (if present, checked via reset_line_state)
    // 2. IOCON
    // 3. OLATA
    // 4. GPPU, IPOL, GPINTEN, INTCON, DEFVAL
    // 5. IODIRA, IODIRB

    int iocon_idx = -1;
    int olata_idx = -1;
    int iodira_idx = -1;

    for (uint32_t i = 0; i < fake->write_count; i++) {
        if (fake->write_history[i].reg == MCP23017_REG_IOCON && iocon_idx == -1) iocon_idx = i;
        if (fake->write_history[i].reg == MCP23017_REG_OLATA && olata_idx == -1) olata_idx = i;
        if (fake->write_history[i].reg == MCP23017_REG_IODIRA && iodira_idx == -1) iodira_idx = i;
    }

    TEST_ASSERT_NOT_EQUAL(-1, iocon_idx);
    TEST_ASSERT_NOT_EQUAL(-1, olata_idx);
    TEST_ASSERT_NOT_EQUAL(-1, iodira_idx);

    TEST_ASSERT_TRUE(iocon_idx < olata_idx);
    TEST_ASSERT_TRUE(olata_idx < iodira_idx);

    uint8_t expected_off = led_active_high ? 0x00 : 0x0F;
    TEST_ASSERT_EQUAL_HEX8(expected_off, fake->write_history[olata_idx].val);
}

void test_init_success(void) {
    mcp23017_t ctx;
    int ret = mcp23017_logic_init(&ctx, &default_cfg);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_FALSE(g_fake.violated_invariant);
    TEST_ASSERT_EQUAL(1, g_fake.reset_line_state);

    verify_init_order(&g_fake, true);
}

void test_init_led_polarity(void) {
    mcp23017_t ctx;
    mcp23017_config_t cfg = default_cfg;
    cfg.led_active_high = false;

    int ret = mcp23017_logic_init(&ctx, &cfg);
    TEST_ASSERT_EQUAL(0, ret);
    verify_init_order(&g_fake, false);
}

void test_set_led(void) {
    mcp23017_t ctx;
    mcp23017_logic_init(&ctx, &default_cfg);

    // Set LED 0 ON
    mcp23017_logic_set_led(&ctx, 0, true);
    TEST_ASSERT_EQUAL_HEX8(0x01, g_fake.regs[MCP23017_REG_OLATA]);

    // Set LED 1 ON
    mcp23017_logic_set_led(&ctx, 1, true);
    TEST_ASSERT_EQUAL_HEX8(0x03, g_fake.regs[MCP23017_REG_OLATA]);

    // Set LED 0 OFF
    mcp23017_logic_set_led(&ctx, 0, false);
    TEST_ASSERT_EQUAL_HEX8(0x02, g_fake.regs[MCP23017_REG_OLATA]);

    // Bounds check
    TEST_ASSERT_NOT_EQUAL(0, mcp23017_logic_set_led(&ctx, 4, true));
}

void test_set_led_error_shadow(void) {
    mcp23017_t ctx;
    mcp23017_logic_init(&ctx, &default_cfg);


    g_fake.inject_error = true;
    mcp23017_logic_set_led(&ctx, 0, true);

    g_fake.inject_error = false;
    // The previous write failed, shadow should NOT be updated. So setting LED 1 ON should only set bit 1.
    mcp23017_logic_set_led(&ctx, 1, true);
    TEST_ASSERT_EQUAL_HEX8(0x02, g_fake.regs[MCP23017_REG_OLATA]);
}

void test_debounce_and_events(void) {
    mcp23017_t ctx;
    mcp23017_logic_init(&ctx, &default_cfg);

    // All buttons released (pullup active => 1)
    g_fake.regs[MCP23017_REG_GPIOA] = 0xF0;
    g_fake.regs[MCP23017_REG_GPIOB] = 0x01;

    mcp23017_logic_poll(&ctx, 0); // initial state

    // Press button 0 (GPIOA.4 = 0)
    g_fake.regs[MCP23017_REG_GPIOA] = 0xE0;
    mcp23017_logic_poll(&ctx, 10); // chattering (10ms < 20ms)

    mcp23017_event_t ev;
    TEST_ASSERT_FALSE(mcp23017_logic_next_event(&ctx, &ev)); // no event yet

    mcp23017_logic_poll(&ctx, 30); // 30ms - 10ms = 20ms debounce elapsed
    TEST_ASSERT_TRUE(mcp23017_logic_next_event(&ctx, &ev));
    TEST_ASSERT_EQUAL(MCP23017_BTN_PANEL_0, ev.button_id);
    TEST_ASSERT_EQUAL(MCP23017_EVENT_PRESSED, ev.type);
    TEST_ASSERT_FALSE(mcp23017_logic_next_event(&ctx, &ev));

    // Release button 0
    g_fake.regs[MCP23017_REG_GPIOA] = 0xF0;
    mcp23017_logic_poll(&ctx, 40);
    mcp23017_logic_poll(&ctx, 60); // 60ms - 40ms = 20ms

    TEST_ASSERT_TRUE(mcp23017_logic_next_event(&ctx, &ev));
    TEST_ASSERT_EQUAL(MCP23017_BTN_PANEL_0, ev.button_id);
    TEST_ASSERT_EQUAL(MCP23017_EVENT_RELEASED, ev.type);
}

void test_offline_and_reinit(void) {
    mcp23017_t ctx;
    mcp23017_logic_init(&ctx, &default_cfg);

    TEST_ASSERT_FALSE(ctx.state.is_offline);


    g_fake.inject_error = true;
    for (int i = 0; i < 5; i++) {
        mcp23017_logic_poll(&ctx, i * 10);
    }

    TEST_ASSERT_TRUE(ctx.state.is_offline);

    g_fake.inject_error = false;
    uint32_t wc_before = g_fake.write_count;

    mcp23017_logic_poll(&ctx, 100);

    TEST_ASSERT_FALSE(ctx.state.is_offline);
    TEST_ASSERT_TRUE(g_fake.write_count > wc_before + 10); // Check that re-init occurred
}

void test_verify_interval_reset(void) {
    mcp23017_t ctx;
    mcp23017_logic_init(&ctx, &default_cfg);

    // Polls 0 to 98
    for (int i = 0; i < 99; i++) {
        mcp23017_logic_poll(&ctx, i * 10);
    }

    g_fake.inject_reset = true;
    mcp23017_logic_poll(&ctx, 990); // 100th poll, triggers verify

    TEST_ASSERT_TRUE(ctx.state.is_offline);

    g_fake.inject_reset = false;
    mcp23017_logic_poll(&ctx, 1000); // re-init
    TEST_ASSERT_FALSE(ctx.state.is_offline);
}

void test_event_overflow(void) {
    mcp23017_t ctx;
    mcp23017_logic_init(&ctx, &default_cfg);

    // Push events to fill queue
    // Need to account for debounce which delays events,
    // and wait long enough after each flip
    uint64_t ms = 0;
    for (int i = 0; i < 20; i++) {
        g_fake.regs[MCP23017_REG_GPIOA] = (i % 2 == 0) ? 0xE0 : 0xF0;
        mcp23017_logic_poll(&ctx, ms); // raw state changed
        ms += 30; // wait more than debounce
        mcp23017_logic_poll(&ctx, ms); // event gets pushed
        ms += 10; // offset slightly for next iteration
    }

    TEST_ASSERT_TRUE(ctx.state.event_overflow_count > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_success);
    RUN_TEST(test_init_led_polarity);
    RUN_TEST(test_set_led);
    RUN_TEST(test_set_led_error_shadow);
    RUN_TEST(test_debounce_and_events);
    RUN_TEST(test_offline_and_reinit);
    RUN_TEST(test_verify_interval_reset);
    RUN_TEST(test_event_overflow);
    return UNITY_END();
}
