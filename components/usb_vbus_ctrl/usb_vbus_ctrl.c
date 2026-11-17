#include "usb_vbus_ctrl.h"

#include <string.h>

/* 10 秒の保証（ヘッダーの不変条件 3）: 3P + D + F の最大に、完了の余裕を足しても、仕様の上限に収まる。 */
typedef char usb_vbus_ctrl_max_is_within_spec[(3u * USB_VBUS_CTRL_POLL_PERIOD_MAX_MS + USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS +
                                               USB_VBUS_CTRL_FORCE_OFF_MAX_MS + USB_VBUS_CTRL_COMPLETION_MARGIN_MS <=
                                               USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS) ? 1 : -1];

usb_vbus_ctrl_config_t usb_vbus_ctrl_config_default(void) {
    usb_vbus_ctrl_config_t c;
    c.enabled = false;
    c.connect_debounce_ms = USB_VBUS_CTRL_DEFAULT_CONNECT_DEBOUNCE_MS;
    c.disconnect_debounce_ms = USB_VBUS_CTRL_DEFAULT_DISCONNECT_DEBOUNCE_MS;
    c.force_off_after_ms = USB_VBUS_CTRL_DEFAULT_FORCE_OFF_AFTER_MS;
    return c;
}

void usb_vbus_ctrl_init(usb_vbus_ctrl_t *c, const usb_vbus_ctrl_config_t *config) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->config = config ? *config : usb_vbus_ctrl_config_default();
    if (c->config.disconnect_debounce_ms > USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS) {
        c->config.disconnect_debounce_ms = USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS;
    }
    if (c->config.force_off_after_ms > USB_VBUS_CTRL_FORCE_OFF_MAX_MS) {
        c->config.force_off_after_ms = USB_VBUS_CTRL_FORCE_OFF_MAX_MS;
    }
    if (c->config.force_off_after_ms == 0) {
        c->config.force_off_after_ms = 1; /* 0 は「待たない」= 最短の 1 ms（10 秒の保証側に倒す） */
    }
    /* connected = false（切断）、数えなし、統計 0 は memset */
}

usb_vbus_ctrl_action_t usb_vbus_ctrl_update(usb_vbus_ctrl_t *c, const usb_vbus_ctrl_input_t *input) {
    if (!c) return USB_VBUS_CTRL_ACTION_NONE;

    /* 入力がない: 読み出しの失敗（VBUS なし）、時計は進まない、として扱う（安全側） */
    usb_vbus_ctrl_input_t none_in;
    none_in.now_ms = c->last_now_ms;
    none_in.vbus_present = false;
    none_in.sample_valid = false;
    const usb_vbus_ctrl_input_t *in = input ? input : &none_in;

    if (!c->config.enabled) {
        /* 検出なし（いまの動作）: 最初の update で CONNECT を 1 回だけ */
        if (c->legacy_done) return USB_VBUS_CTRL_ACTION_NONE;
        c->legacy_done = true;
        return USB_VBUS_CTRL_ACTION_CONNECT;
    }

    /* 時計の逆行: 数えているデバウンスを、この now_ms から数え直す（数えていないなら、次の開始で上書きされる） */
    if (in->now_ms < c->last_now_ms) {
        c->run_start_ms = in->now_ms;
        c->disc_active = false; /* 数え直し。強制遮断の待ちも最初から */
    }
    c->last_now_ms = in->now_ms;

    bool detected = in->sample_valid && in->vbus_present;
    /* 数えるのは、いまの状態と逆の向き（切断の状態なら VBUS 検出、接続の状態なら VBUS なし）の連続 */
    bool opposite = c->connected ? !detected : detected;
    if (!opposite) {
        c->run_active = false;
        c->disc_active = false;
        return USB_VBUS_CTRL_ACTION_NONE;
    }
    if (!c->run_active) {
        c->run_active = true;
        c->run_start_ms = in->now_ms;
    }

    uint64_t elapsed = in->now_ms - c->run_start_ms;
    if (c->connected) {
        if (elapsed >= c->config.disconnect_debounce_ms) {
            /* 強制遮断: 最初の DISCONNECT を返してから force_off_after_ms 経っても、まだ接続の状態（= 成功していない） */
            if (c->disc_active && in->now_ms - c->disc_first_ms >= c->config.force_off_after_ms) {
                return USB_VBUS_CTRL_ACTION_FORCE_OFF;
            }
            if (!c->disc_active) {
                c->disc_active = true;
                c->disc_first_ms = in->now_ms;
            }
            return USB_VBUS_CTRL_ACTION_DISCONNECT;
        }
    } else {
        if (elapsed >= c->config.connect_debounce_ms) return USB_VBUS_CTRL_ACTION_CONNECT;
    }
    return USB_VBUS_CTRL_ACTION_NONE;
}

void usb_vbus_ctrl_applied(usb_vbus_ctrl_t *c, usb_vbus_ctrl_action_t action, bool ok) {
    if (!c) return;
    if (action != USB_VBUS_CTRL_ACTION_CONNECT && action != USB_VBUS_CTRL_ACTION_DISCONNECT &&
        action != USB_VBUS_CTRL_ACTION_FORCE_OFF) {
        return;
    }
    if (!ok) {
        /* 状態も数えも変えない: 条件が続いている限り、次の update でも同じ（または FORCE_OFF の）action が返る
         * （デバウンスはやり直さない） */
        c->stats.failures++;
        if (action == USB_VBUS_CTRL_ACTION_DISCONNECT) c->stats.disconnect_failure_count++;
        return;
    }
    c->connected = (action == USB_VBUS_CTRL_ACTION_CONNECT);
    c->run_active = false; /* 状態が変わった。新しい状態の「逆の向き」を、次の tick から最初から数える */
    c->disc_active = false;
    if (action == USB_VBUS_CTRL_ACTION_CONNECT) {
        c->stats.connects++;
    } else if (action == USB_VBUS_CTRL_ACTION_DISCONNECT) {
        c->stats.disconnects++;
    } else {
        c->stats.force_off_count++;
    }
}

bool usb_vbus_ctrl_is_connected(const usb_vbus_ctrl_t *c) {
    return c ? c->connected : false;
}

void usb_vbus_ctrl_get_stats(const usb_vbus_ctrl_t *c, usb_vbus_ctrl_stats_t *out) {
    if (!out) return;
    if (!c) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = c->stats;
}
