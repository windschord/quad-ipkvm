#include "hid_logic.h"
#include "hid_msg.h"
#include <string.h>

#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

static void add_key(hid_logic_t *h, uint8_t usage) {
    for (size_t i = 0; i < h->num_keys; i++) {
        if (h->keys[i] == usage) return;
    }
    if (h->num_keys < HID_MAX_KEYS) {
        h->keys[h->num_keys++] = usage;
    }
}

static void remove_key(hid_logic_t *h, uint8_t usage) {
    for (size_t i = 0; i < h->num_keys; i++) {
        if (h->keys[i] == usage) {
            memmove(&h->keys[i], &h->keys[i + 1], h->num_keys - i - 1);
            h->num_keys--;
            return;
        }
    }
}

static bool is_modifier(uint8_t usage) {
    return usage >= 0xE0 && usage <= 0xE7;
}

static bool is_valid_usage(uint8_t usage) {
    return (usage >= 0x04 && usage <= 0xA4) || is_modifier(usage);
}

static void send_kbd(hid_logic_t *h) {
    hid_kbd_report_t r = {0};
    r.modifiers = h->modifiers;

    if (h->num_keys >= 7) {
        for (int i = 0; i < 6; i++) {
            r.keys[i] = 0x01; // ErrorRollOver
        }
    } else {
        for (size_t i = 0; i < h->num_keys; i++) {
            r.keys[i] = h->keys[i];
        }
    }

    if (h->sink.ready && h->sink.ready(h->sink.ctx)) {
        if (h->sink.send_keyboard) {
            h->need_kbd_retransmit = !h->sink.send_keyboard(h->sink.ctx, &r);
        } else {
            h->need_kbd_retransmit = false;
        }
    } else {
        h->need_kbd_retransmit = true;
    }
}

static bool sink_ready(const hid_logic_t *h) {
    return h->sink.ready != NULL && h->sink.ready(h->sink.ctx);
}

/* The pointer type changed: the old interface must see "all buttons released" before the new one is used
 * (docs/INTERFACES.md 5.2). last_ptr_type is already the NEW type here. Returns true once the old side is clean. */
static bool resolve_ptr_switch(hid_logic_t *h) {
    if (!h->ptr_switch_pending) return true;
    if (!sink_ready(h)) return false;

    bool sent = true; /* a missing send function means nothing to release */
    if (h->last_ptr_type == HID_PTR_TYPE_REL && h->sink.send_abs) {
        hid_abs_report_t r = {0};
        r.x = h->abs_x;
        r.y = h->abs_y;
        sent = h->sink.send_abs(h->sink.ctx, &r);
    } else if (h->last_ptr_type == HID_PTR_TYPE_ABS && h->sink.send_rel) {
        hid_rel_report_t r = {0};
        sent = h->sink.send_rel(h->sink.ctx, &r);
    }
    if (sent) h->ptr_switch_pending = false;
    return sent;
}

static void send_abs(hid_logic_t *h, int8_t wheel) {
    if (!resolve_ptr_switch(h) || !sink_ready(h)) {
        h->need_abs_retransmit = true;
        return;
    }
    if (!h->sink.send_abs) {
        h->need_abs_retransmit = false;
        return;
    }
    hid_abs_report_t r = {0};
    r.buttons = h->buttons;
    r.x = h->abs_x;
    r.y = h->abs_y;
    r.wheel = wheel;
    h->need_abs_retransmit = !h->sink.send_abs(h->sink.ctx, &r);
}

static int8_t take_chunk(int32_t *acc) {
    int32_t v = *acc;
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    *acc -= v;
    return (int8_t)v;
}

static bool rel_motion_pending(const hid_logic_t *h) {
    return h->rel_dx != 0 || h->rel_dy != 0 || h->rel_wheel != 0;
}

static bool rel_pending(const hid_logic_t *h) {
    return h->need_rel_send || h->rel_buttons != h->buttons || rel_motion_pending(h);
}

/* Sends the owed relative reports, at most HID_REL_MAX_REPORTS_PER_CALL per call (the rest goes out on the next tick).
 * `owe_report` forces one report even without movement (a button change is state and must reach the target).
 * Movement that has not been sent yet keeps the button state it was made under (`rel_buttons`); a changed button
 * state follows in its own report once that movement is out, so a click never lands before the cursor arrives. */
static void send_rel(hid_logic_t *h, bool owe_report) {
    if (h->last_ptr_type != HID_PTR_TYPE_REL) {
        /* The relative interface is not the active pointer: its state is not ours to send (the switch already
         * released it), and the button state belongs to the other interface. */
        h->rel_dx = h->rel_dy = h->rel_wheel = 0;
        h->rel_buttons = h->buttons;
        h->need_rel_send = false;
        return;
    }
    if (owe_report) h->need_rel_send = true;
    if (!rel_pending(h)) return;
    if (!resolve_ptr_switch(h) || !sink_ready(h)) return;
    if (!h->sink.send_rel) { /* no sink: nothing can be delivered, drop it */
        h->rel_dx = h->rel_dy = h->rel_wheel = 0;
        h->rel_buttons = h->buttons;
        h->need_rel_send = false;
        return;
    }

    while (h->rel_budget > 0 && rel_pending(h)) {
        h->rel_budget--;
        bool motion = rel_motion_pending(h);
        hid_rel_report_t r = {0};
        r.buttons = motion ? h->rel_buttons : h->buttons;
        r.dx = take_chunk(&h->rel_dx);
        r.dy = take_chunk(&h->rel_dy);
        r.wheel = take_chunk(&h->rel_wheel);
        if (!h->sink.send_rel(h->sink.ctx, &r)) { /* keep it for the next tick */
            h->rel_dx += r.dx;
            h->rel_dy += r.dy;
            h->rel_wheel += r.wheel;
            h->need_rel_send = true;
            return;
        }
        h->rel_buttons = r.buttons;
        h->need_rel_send = false;
    }
}

void hid_logic_init(hid_logic_t *h, const hid_logic_config_t *cfg, const hid_sink_t *sink) {
    memset(h, 0, sizeof(*h));
    if (cfg) h->cfg = *cfg;
    else {
        h->cfg.hold_timeout_ms = HID_HOLD_TIMEOUT_MS;
        h->cfg.rate_per_sec = HID_RATE_PER_SEC;
        h->cfg.rate_burst = HID_RATE_BURST;
    }
    if (sink) h->sink = *sink;
    h->owner_id = -1;
    h->tokens = h->cfg.rate_burst;
}

static void update_tokens(hid_logic_t *h, uint64_t now_ms) {
    if (now_ms < h->token_last_update) {
        h->token_last_update = now_ms; // Clock moved backwards
    }
    uint64_t dt = now_ms - h->token_last_update;
    if (dt > 0) {
        uint64_t add_tokens = (dt * h->cfg.rate_per_sec) / 1000;
        if (add_tokens > 0) {
            /* Clamp before adding: after a long idle the sum would not fit in int32_t. */
            int64_t sum = (int64_t)h->tokens + (add_tokens > (uint64_t)h->cfg.rate_burst ? (int64_t)h->cfg.rate_burst : (int64_t)add_tokens);
            h->tokens = sum > (int64_t)h->cfg.rate_burst ? (int32_t)h->cfg.rate_burst : (int32_t)sum;
            h->token_last_update += (add_tokens * 1000) / h->cfg.rate_per_sec;
        }
    }
}

void hid_logic_release_all(hid_logic_t *h) {
    h->rel_budget = HID_REL_MAX_REPORTS_PER_CALL;
    h->modifiers = 0;
    h->num_keys = 0;
    send_kbd(h);

    h->buttons = 0;
    if (h->last_ptr_type == HID_PTR_TYPE_ABS) {
        send_abs(h, 0);
    } else if (h->last_ptr_type == HID_PTR_TYPE_REL) {
        send_rel(h, true);
    }
}

void hid_logic_on_usb_mounted(hid_logic_t *h) {
    /* The host (re-)enumerated: it has forgotten every key and button, and the old pointer interface's state too.
     * Drop what was never sent, then send/owe the all-released reports. */
    h->rel_dx = 0;
    h->rel_dy = 0;
    h->rel_wheel = 0;
    h->ptr_switch_pending = false;
    hid_logic_release_all(h);
}

bool hid_logic_acquire(hid_logic_t *h, int owner_id) {
    if (h->owner_id == -1 || h->owner_id == owner_id) {
        h->owner_id = owner_id;
        return true;
    }
    return false;
}

void hid_logic_release_owner(hid_logic_t *h, int owner_id) {
    if (h->owner_id == owner_id) {
        hid_logic_release_all(h);
        h->owner_id = -1;
    }
}

void hid_logic_tick(hid_logic_t *h, uint64_t now_ms) {
    h->rel_budget = HID_REL_MAX_REPORTS_PER_CALL;
    if (now_ms < h->last_tick_time) {
        h->last_msg_time = now_ms;
        h->token_last_update = now_ms;
        h->abuse_window_start = now_ms;
    }
    h->last_tick_time = now_ms;

    if (h->owner_id != -1 && (h->modifiers != 0 || h->num_keys > 0 || h->buttons != 0)) {
        if (now_ms - h->last_msg_time > h->cfg.hold_timeout_ms) {
            hid_logic_release_all(h);
            return; // State changed, no need to do retransmits here since release_all does it.
        }
    }

    if (h->need_kbd_retransmit) {
        send_kbd(h);
    }

    if (h->need_abs_retransmit) {
        send_abs(h, 0);
    } else if (h->ptr_switch_pending) {
        resolve_ptr_switch(h);
    }

    send_rel(h, false);
}

hid_result_t hid_logic_on_message(hid_logic_t *h, int owner_id, const uint8_t *msg, size_t len, uint64_t now_ms) {
    h->rel_budget = HID_REL_MAX_REPORTS_PER_CALL;
    if (now_ms < h->last_tick_time) {
        h->last_msg_time = now_ms;
        h->token_last_update = now_ms;
        h->abuse_window_start = now_ms;
    }
    h->last_tick_time = now_ms;

    if (h->owner_id != owner_id) {
        return HID_RESULT_NOT_OWNER;
    }

    hid_msg_t m;
    if (!hid_msg_parse(msg, len, &m)) {
        h->invalid_count++;
        return HID_RESULT_INVALID;
    }

    h->last_msg_time = now_ms;

    update_tokens(h, now_ms);

    if (now_ms - h->abuse_window_start >= HID_ABUSE_WINDOW_MS) {
        h->abuse_window_start = now_ms;
        h->dropped_count = 0;
    }

    bool bypass_rate_limit = false;
    if (m.type == HID_MSG_TYPE_KEY_UP || m.type == HID_MSG_TYPE_RELEASE_ALL) {
        bypass_rate_limit = true;
    } else if (m.type == HID_MSG_TYPE_PTR_ABS) {
        if (m.data.ptr_abs.buttons != h->buttons) bypass_rate_limit = true;
    } else if (m.type == HID_MSG_TYPE_PTR_REL) {
        if (m.data.ptr_rel.buttons != h->buttons) bypass_rate_limit = true;
    }

    if (!bypass_rate_limit) {
        if (h->tokens > 0) {
            h->tokens--;
        } else {
            h->dropped_count++;
            if (h->dropped_count > HID_ABUSE_MAX_DROPS) {
                return HID_RESULT_ABUSE;
            }
            return HID_RESULT_RATE_LIMITED;
        }
    } else {
        if (h->tokens <= 0) {
            h->dropped_count++;
            if (h->dropped_count > HID_ABUSE_MAX_DROPS) {
                return HID_RESULT_ABUSE;
            }
        } else {
            h->tokens--;
        }
    }

    switch (m.type) {
        case HID_MSG_TYPE_KEY_DOWN:
            if (!is_valid_usage(m.data.key.usage)) {
                h->invalid_count++;
                return HID_RESULT_INVALID;
            }
            if (is_modifier(m.data.key.usage)) {
                h->modifiers = (uint8_t)(h->modifiers | (1u << (m.data.key.usage - 0xE0)));
            } else {
                add_key(h, m.data.key.usage);
            }
            send_kbd(h);
            break;

        case HID_MSG_TYPE_KEY_UP:
            if (!is_valid_usage(m.data.key.usage)) {
                h->invalid_count++;
                return HID_RESULT_INVALID;
            }
            if (is_modifier(m.data.key.usage)) {
                h->modifiers = (uint8_t)(h->modifiers & ~(1u << (m.data.key.usage - 0xE0)));
            } else {
                remove_key(h, m.data.key.usage);
            }
            send_kbd(h);
            break;

        case HID_MSG_TYPE_RELEASE_ALL:
            hid_logic_release_all(h);
            break;

        case HID_MSG_TYPE_KEEPALIVE:
            // Handled by last_msg_time update
            break;

        case HID_MSG_TYPE_PTR_ABS:
            if (h->last_ptr_type == HID_PTR_TYPE_REL) {
                h->ptr_switch_pending = true; /* send_rel() drops relative state while absolute is active */
            }
            h->last_ptr_type = HID_PTR_TYPE_ABS;
            h->buttons = m.data.ptr_abs.buttons;
            h->abs_x = CLAMP(m.data.ptr_abs.x, 0, HID_ABS_MAX);
            h->abs_y = CLAMP(m.data.ptr_abs.y, 0, HID_ABS_MAX);

            int8_t wheel_abs = m.data.ptr_abs.wheel == -128 ? -127 : m.data.ptr_abs.wheel;
            send_abs(h, wheel_abs);
            break;

        case HID_MSG_TYPE_PTR_REL:
            if (h->last_ptr_type == HID_PTR_TYPE_ABS) {
                h->ptr_switch_pending = true;
                h->need_abs_retransmit = false; /* superseded by the release report sent on the switch */
            }
            h->last_ptr_type = HID_PTR_TYPE_REL;
            if (m.data.ptr_rel.buttons != h->buttons && rel_motion_pending(h)) {
                send_rel(h, false); /* push the backlog out with the old button state first */
            }
            h->buttons = m.data.ptr_rel.buttons;
            if (!rel_motion_pending(h)) {
                h->rel_buttons = h->buttons; /* no backlog: this message's movement and buttons form one report */
            }
            int32_t dx = h->rel_dx + m.data.ptr_rel.dx;
            h->rel_dx = CLAMP(dx, -32767, 32767);
            int32_t dy = h->rel_dy + m.data.ptr_rel.dy;
            h->rel_dy = CLAMP(dy, -32767, 32767);
            int32_t wheel = h->rel_wheel + (m.data.ptr_rel.wheel == -128 ? -127 : m.data.ptr_rel.wheel);
            h->rel_wheel = CLAMP(wheel, -32767, 32767);
            send_rel(h, true);
            break;

        case HID_MSG_TYPE_INVALID:
        default:
            return HID_RESULT_INVALID;
    }

    return HID_RESULT_OK;
}
