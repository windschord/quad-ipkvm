#include "mcp23017_logic.h"
#include <string.h>

#define IODIRA_EXPECTED 0xF0 // A0-A3 output, A4-A7 input
#define IODIRB_EXPECTED 0xFF // B0-B7 input
#define GPPUA_EXPECTED  0xF0 // A4-A7 pullup
#define GPPUB_EXPECTED  0xFF // B0-B7 pullup

static int write_reg(mcp23017_t* ctx, uint8_t reg, uint8_t value) {
    return ctx->cfg.write_reg(ctx->cfg.user, reg, value);
}

static int read_reg(mcp23017_t* ctx, uint8_t reg, uint8_t* value) {
    return ctx->cfg.read_reg(ctx->cfg.user, reg, value);
}

static int do_init_sequence(mcp23017_t* ctx) {
    if (ctx->cfg.set_reset_line) {
        if (ctx->cfg.set_reset_line(ctx->cfg.user, 1) != 0) return -1;
    }

    // (2) Write IOCON
    uint8_t iocon = MCP23017_IOCON_MIRROR; // INTPOL=0, ODR=0, SEQOP=0
    if (write_reg(ctx, MCP23017_REG_IOCON, iocon) != 0) return -1;

    // (3) Write OLATA with shadow state (initialized by init before calling this)
    if (write_reg(ctx, MCP23017_REG_OLATA, ctx->state.shadow_olata) != 0) return -1;

    // (4) Configure pullups, polarity, interrupts
    if (write_reg(ctx, MCP23017_REG_GPPUA, GPPUA_EXPECTED) != 0) return -1;
    if (write_reg(ctx, MCP23017_REG_GPPUB, GPPUB_EXPECTED) != 0) return -1;
    if (write_reg(ctx, MCP23017_REG_IPOLA, 0x00) != 0) return -1;
    if (write_reg(ctx, MCP23017_REG_IPOLB, 0x00) != 0) return -1;

    if (write_reg(ctx, MCP23017_REG_GPINTENA, 0xF0) != 0) return -1;
    if (write_reg(ctx, MCP23017_REG_GPINTENB, 0x01) != 0) return -1;

    if (write_reg(ctx, MCP23017_REG_INTCONA, 0x00) != 0) return -1;
    if (write_reg(ctx, MCP23017_REG_INTCONB, 0x00) != 0) return -1;

    if (write_reg(ctx, MCP23017_REG_DEFVALA, 0x00) != 0) return -1;
    if (write_reg(ctx, MCP23017_REG_DEFVALB, 0x00) != 0) return -1;

    // (5) Finally, set IODIR
    if (write_reg(ctx, MCP23017_REG_IODIRA, IODIRA_EXPECTED) != 0) return -1;
    if (write_reg(ctx, MCP23017_REG_IODIRB, IODIRB_EXPECTED) != 0) return -1;

    // Verify
    uint8_t val;
    if (read_reg(ctx, MCP23017_REG_IODIRA, &val) != 0 || val != IODIRA_EXPECTED) return -1;
    if (read_reg(ctx, MCP23017_REG_GPPUA, &val) != 0 || val != GPPUA_EXPECTED) return -1;

    return 0;
}

int mcp23017_logic_init(mcp23017_t* ctx, const mcp23017_config_t* cfg) {
    if (!ctx || !cfg) return -1;
    if (!cfg->write_reg || !cfg->read_reg) return -1;

    memset(ctx, 0, sizeof(mcp23017_t));
    ctx->cfg = *cfg;
    if (ctx->cfg.debounce_ms == 0) ctx->cfg.debounce_ms = 20;
    if (ctx->cfg.max_consecutive_errors == 0) ctx->cfg.max_consecutive_errors = 5;
    if (ctx->cfg.verify_interval_polls == 0) ctx->cfg.verify_interval_polls = 100;
    if (ctx->cfg.retry_interval_ms == 0) ctx->cfg.retry_interval_ms = 1000;

    ctx->state.shadow_olata = ctx->cfg.led_active_high ? 0x00 : 0x0F;

    int ret = do_init_sequence(ctx);
    if (ret != 0) {
        ctx->state.is_offline = true;
    }
    return ret;
}

int mcp23017_logic_set_led(mcp23017_t* ctx, uint8_t led_idx, bool on) {
    if (!ctx || led_idx >= MCP23017_NUM_PANEL_LEDS) return -1;

    bool target_level = ctx->cfg.led_active_high ? on : !on;
    uint8_t new_olata = ctx->state.shadow_olata;

    if (target_level) {
        new_olata |= (1 << led_idx);
    } else {
        new_olata &= ~(1 << led_idx);
    }

    // ensure upper 4 bits are 0
    new_olata &= 0x0F;

    if (ctx->state.is_offline) {
        ctx->state.shadow_olata = new_olata;
        return 0; // return ok when offline, just shadow update
    }

    if (write_reg(ctx, MCP23017_REG_OLATA, new_olata) == 0) {
        ctx->state.shadow_olata = new_olata;
        ctx->state.consecutive_errors = 0;
        return 0;
    }

    ctx->state.consecutive_errors++;
    return -1;
}

int mcp23017_logic_set_leds(mcp23017_t* ctx, uint8_t mask) {
    if (!ctx) return -1;

    uint8_t new_olata = ctx->state.shadow_olata & 0xF0; // keep upper
    for (int i = 0; i < MCP23017_NUM_PANEL_LEDS; i++) {
        bool on = (mask & (1 << i)) != 0;
        bool target_level = ctx->cfg.led_active_high ? on : !on;
        if (target_level) {
            new_olata |= (1 << i);
        }
    }

    if (ctx->state.is_offline) {
        ctx->state.shadow_olata = new_olata;
        return 0;
    }

    if (write_reg(ctx, MCP23017_REG_OLATA, new_olata) == 0) {
        ctx->state.shadow_olata = new_olata;
        ctx->state.consecutive_errors = 0;
        return 0;
    }

    ctx->state.consecutive_errors++;
    return -1;
}

static void push_event(mcp23017_t* ctx, uint8_t id, mcp23017_event_type_t type) {
    uint32_t next_head = (ctx->state.event_head + 1) % 16;
    if (next_head == ctx->state.event_tail) {
        ctx->state.event_overflow_count++;
        return;
    }
    ctx->state.event_queue[ctx->state.event_head].button_id = id;
    ctx->state.event_queue[ctx->state.event_head].type = type;
    ctx->state.event_head = next_head;
}

static void handle_debounce(mcp23017_t* ctx, uint8_t id, bool new_raw, uint64_t now_ms) {
    if (ctx->state.btn_state_raw[id] != new_raw) {
        ctx->state.btn_state_raw[id] = new_raw;
        ctx->state.btn_last_changed_ms[id] = now_ms;
    } else {
        if (now_ms - ctx->state.btn_last_changed_ms[id] >= ctx->cfg.debounce_ms) {
            if (ctx->state.btn_state_debounced[id] != new_raw) {
                ctx->state.btn_state_debounced[id] = new_raw;
                push_event(ctx, id, new_raw ? MCP23017_EVENT_PRESSED : MCP23017_EVENT_RELEASED);
            }
        }
    }
}

static void handle_error(mcp23017_t* ctx) {
    ctx->state.consecutive_errors++;
    if (ctx->state.consecutive_errors >= ctx->cfg.max_consecutive_errors) {
        ctx->state.is_offline = true;
    }
}

void mcp23017_logic_poll(mcp23017_t* ctx, uint64_t now_ms) {
    if (!ctx) return;

    // Time rollback check
    if (now_ms < ctx->state.last_poll_ms) {
        for (int i = 0; i < MCP23017_NUM_BUTTONS; i++) {
            ctx->state.btn_last_changed_ms[i] = now_ms;
        }
        ctx->state.last_reinit_attempt_ms = now_ms;
    }
    ctx->state.last_poll_ms = now_ms;

    if (ctx->state.is_offline) {
        if (now_ms - ctx->state.last_reinit_attempt_ms < ctx->cfg.retry_interval_ms) {
            return;
        }
        ctx->state.last_reinit_attempt_ms = now_ms;

        if (do_init_sequence(ctx) == 0) {
            ctx->state.is_offline = false;
            ctx->state.consecutive_errors = 0;
            ctx->state.reinit_count++;
        } else {
            return;
        }
    }

    // Verification check periodically
    if (ctx->cfg.verify_interval_polls > 0) {
        ctx->state.verify_counter++;
        if (ctx->state.verify_counter >= ctx->cfg.verify_interval_polls) {
            ctx->state.verify_counter = 0;
            uint8_t v1, v2, v3, v4;
            int r1 = read_reg(ctx, MCP23017_REG_IODIRA, &v1);
            int r2 = read_reg(ctx, MCP23017_REG_GPPUA, &v2);
            int r3 = read_reg(ctx, MCP23017_REG_IODIRB, &v3);
            int r4 = read_reg(ctx, MCP23017_REG_GPPUB, &v4);
            if (r1 != 0 || r2 != 0 || r3 != 0 || r4 != 0) {
                handle_error(ctx);
                return;
            }
            if (v1 != IODIRA_EXPECTED || v2 != GPPUA_EXPECTED ||
                v3 != IODIRB_EXPECTED || v4 != GPPUB_EXPECTED) {
                // Unexpected reset
                ctx->state.is_offline = true;
                return;
            }
        }
    }

    uint8_t buf[2];
    if (ctx->cfg.read_regs) {
        if (ctx->cfg.read_regs(ctx->cfg.user, MCP23017_REG_GPIOA, buf, 2) != 0) {
            handle_error(ctx);
            return;
        }
    } else {
        if (read_reg(ctx, MCP23017_REG_GPIOA, &buf[0]) != 0 ||
            read_reg(ctx, MCP23017_REG_GPIOB, &buf[1]) != 0) {
            handle_error(ctx);
            return;
        }
    }
    ctx->state.consecutive_errors = 0;

    // Buttons are active low (pullup is enabled, pressed = 0)
    uint8_t gpioa = buf[0];
    uint8_t gpiob = buf[1];

    bool b0 = (gpioa & (1 << 4)) == 0;
    bool b1 = (gpioa & (1 << 5)) == 0;
    bool b2 = (gpioa & (1 << 6)) == 0;
    bool b3 = (gpioa & (1 << 7)) == 0;
    bool b4 = (gpiob & (1 << 0)) == 0;

    handle_debounce(ctx, MCP23017_BTN_PANEL_0, b0, now_ms);
    handle_debounce(ctx, MCP23017_BTN_PANEL_1, b1, now_ms);
    handle_debounce(ctx, MCP23017_BTN_PANEL_2, b2, now_ms);
    handle_debounce(ctx, MCP23017_BTN_PANEL_3, b3, now_ms);
    handle_debounce(ctx, MCP23017_BTN_DISPLAY, b4, now_ms);
}

bool mcp23017_logic_next_event(mcp23017_t* ctx, mcp23017_event_t* event) {
    if (!ctx || !event) return false;

    if (ctx->state.event_head == ctx->state.event_tail) {
        return false;
    }

    *event = ctx->state.event_queue[ctx->state.event_tail];
    ctx->state.event_tail = (ctx->state.event_tail + 1) % 16;
    return true;
}
