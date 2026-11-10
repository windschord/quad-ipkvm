#include "kvm_ctrl.h"
#include <string.h>

/* 「どの線も有効でない」を表す値（kvm_board_line_t の範囲外） */
#define NO_LINE KVM_BOARD_LINE_COUNT

/* ------------------------------------------------------------------------------------------------
 * 設定
 * ---------------------------------------------------------------------------------------------- */

void kvm_ctrl_config_default(kvm_ctrl_config_t *config) {
    if (!config) return;
    kvm_logic_config_default(&config->logic);
    /* すべて既定の上限（kvm_logic_config_default = KVM_ATX_HARD_MAX_*）以下 */
    config->press_ms = 200;              /* TODO(hw-verify): 電源ボタンの短押し（上限 500） */
    config->reset_ms = 100;              /* TODO(hw-verify): リセットボタン（上限 200） */
    config->force_ms = 4500;             /* TODO(hw-verify): 強制断の長押し（上限 5000） */
    config->atx_min_interval_ms = 2000;  /* TODO(hw-verify): ソフトのクールダウン。保守的な値 */
    config->assert_margin_ms = 100;      /* TODO(hw-verify): tick の周期（既定 20ms）より十分に大きく */
}

/* ------------------------------------------------------------------------------------------------
 * 内部
 * ---------------------------------------------------------------------------------------------- */

uint8_t kvm_ctrl_led_mask_to_raw(uint8_t mask) {
    mask &= 0x0F;
    switch (mask) {
        case 0x00: return 0;
        case 0x01: return 1;
        case 0x02: return 2;
        case 0x04: return 3;
        case 0x08: return 4;
        default:   return 0xFF; /* 複数点灯 */
    }
}

/* フォールトの印だけを付ける（HAL は呼ばない）。最初の原因を保持する。受理中の操作は中断扱い。 */
static void set_fault(kvm_ctrl_t *c, kvm_ctrl_fault_t reason) {
    if (!c->fault) {
        c->fault = true;
        c->fault_reason = reason;
    }
    c->asserted = NO_LINE;
    if (c->has_last_op && c->last_op.result == KVM_CTRL_OP_PENDING) {
        c->last_op.result = KVM_CTRL_OP_ABORTED;
    }
}

/* 即座に release_all してから、フォールトをラッチする。release_all の失敗は、後続の tick で再試行する。 */
static void fault_and_release(kvm_ctrl_t *c, kvm_ctrl_fault_t reason) {
    (void)c->hal.release_all(c->hal.ctx);
    set_fault(c, reason);
}

/* 受理中（pending）の操作に対応する線。無ければ NO_LINE。 */
static kvm_board_line_t pending_line(const kvm_ctrl_t *c) {
    if (!c->has_last_op || c->last_op.result != KVM_CTRL_OP_PENDING) return NO_LINE;
    switch (c->last_op.kind) {
        case KVM_CTRL_OP_SELECT:      return KVM_BOARD_LINE_KVM;
        case KVM_CTRL_OP_POWER_PRESS: return KVM_BOARD_LINE_POWER;
        case KVM_CTRL_OP_POWER_RESET: return KVM_BOARD_LINE_RESET;
        case KVM_CTRL_OP_POWER_FORCE: return KVM_BOARD_LINE_POWER_FORCE;
        default:                      return NO_LINE;
    }
}

/* assert_line に渡す max_ms: そのパルスの長さ + 余裕。ATX は kvm_logic のハード上限を超えない（ボードの上限の前の防御）。 */
static uint32_t line_max_ms(const kvm_ctrl_t *c, kvm_board_line_t line) {
    uint64_t pulse = 0;
    uint64_t cap = UINT32_MAX;
    switch (line) {
        case KVM_BOARD_LINE_POWER:       pulse = c->config.press_ms; cap = KVM_ATX_HARD_MAX_POWER_MS; break;
        case KVM_BOARD_LINE_POWER_FORCE: pulse = c->config.force_ms; cap = KVM_ATX_HARD_MAX_FORCE_MS; break;
        case KVM_BOARD_LINE_RESET:       pulse = c->config.reset_ms; cap = KVM_ATX_HARD_MAX_RESET_MS; break;
        case KVM_BOARD_LINE_KVM:         pulse = c->config.logic.switch_kvm_pulse_time_ms; break;
        default: break;
    }
    uint64_t sum = pulse + c->config.assert_margin_ms;
    if (sum > cap) sum = cap;
    return (uint32_t)sum;
}

/*
 * kvm_logic の出力を HAL に適用する。安全な順序:
 *   1. 無効にすべき線を release_line
 *   2. A0/A1 が変わるなら、全線が High のときだけ set_addr（同じ tick で線を有効にもしない）
 *   3. 新たに有効にする線を assert_line
 * 失敗（HAL の < 0、または kvm_logic の出力の不整合）は、release_all してフォールトをラッチし、false を返す。
 */
static bool apply_outputs(kvm_ctrl_t *c, const kvm_logic_outputs_t *out) {
    int low = (out->e_power_low ? 1 : 0) + (out->e_reset_low ? 1 : 0) + (out->e_kvm_low ? 1 : 0);
    if (low > 1) {
        fault_and_release(c, KVM_CTRL_FAULT_LOGIC_OUTPUT);
        return false;
    }
    if (out->a0a1_channel < 1 || out->a0a1_channel > 4) {
        fault_and_release(c, KVM_CTRL_FAULT_LOGIC_OUTPUT);
        return false;
    }

    /* 有効にすべき線。対応する受理済みの操作が無いのに Ē を Low にする出力は、不整合として扱う */
    kvm_board_line_t expect = pending_line(c);
    kvm_board_line_t want = NO_LINE;
    bool consistent = true;
    if (out->e_kvm_low) {
        want = KVM_BOARD_LINE_KVM;
        consistent = (expect == KVM_BOARD_LINE_KVM);
    } else if (out->e_reset_low) {
        want = KVM_BOARD_LINE_RESET;
        consistent = (expect == KVM_BOARD_LINE_RESET);
    } else if (out->e_power_low) {
        want = expect;
        consistent = (expect == KVM_BOARD_LINE_POWER || expect == KVM_BOARD_LINE_POWER_FORCE);
    }
    if (!consistent) {
        fault_and_release(c, KVM_CTRL_FAULT_LOGIC_OUTPUT);
        return false;
    }

    /* 1. 無効にすべき線 */
    if (c->asserted != NO_LINE && c->asserted != want) {
        if (c->hal.release_line(c->hal.ctx, c->asserted) < 0) {
            fault_and_release(c, KVM_CTRL_FAULT_RELEASE_LINE);
            return false;
        }
        c->asserted = NO_LINE;
    }

    /* 2. A0/A1。全線 High のときだけ。同じ tick で線を有効にしない（セットアップ時間は kvm_logic が守る） */
    if (out->a0a1_channel != c->board_addr) {
        if (c->asserted != NO_LINE || want != NO_LINE) {
            fault_and_release(c, KVM_CTRL_FAULT_LOGIC_OUTPUT);
            return false;
        }
        if (c->hal.set_addr(c->hal.ctx, out->a0a1_channel) < 0) {
            fault_and_release(c, KVM_CTRL_FAULT_SET_ADDR);
            return false;
        }
        c->board_addr = out->a0a1_channel;
    }

    /* 3. 新たに有効にする線 */
    if (want != NO_LINE && c->asserted != want) {
        if (c->hal.assert_line(c->hal.ctx, want, line_max_ms(c, want)) < 0) {
            fault_and_release(c, KVM_CTRL_FAULT_ASSERT_LINE);
            return false;
        }
        c->asserted = want;
    }
    return true;
}

/* kvm_logic の状態遷移から、受理中の操作の結果を決める */
static void update_op(kvm_ctrl_t *c, const kvm_logic_outputs_t *out) {
    if (!c->has_last_op || c->last_op.result != KVM_CTRL_OP_PENDING) return;

    if (c->last_op.kind == KVM_CTRL_OP_SELECT) {
        /* 切替: idle に戻り、active == 目標なら完了、それ以外（タイムアウト・手動切替に追い越された）は中断 */
        if (c->logic.state == KVM_STATE_IDLE) {
            c->last_op.result = (out->active_channel == c->last_op.channel) ? KVM_CTRL_OP_COMPLETED
                                                                           : KVM_CTRL_OP_ABORTED;
        }
        return;
    }

    /* ATX: パルスの準備中・パルス中は実行中。idle に戻り atx_anomaly なしなら完了、それ以外（中断）は中断 */
    if (c->logic.state == KVM_STATE_ATX_WAIT_SETUP || c->logic.state == KVM_STATE_ATX_PULSE) return;
    c->last_op.result = (c->logic.state == KVM_STATE_IDLE && !out->atx_anomaly_detected) ? KVM_CTRL_OP_COMPLETED
                                                                                         : KVM_CTRL_OP_ABORTED;
}

static void record_op(kvm_ctrl_t *c, kvm_ctrl_op_kind_t kind, uint8_t channel, uint32_t *op_id) {
    c->last_op.op_id = c->next_op_id++;
    c->last_op.kind = kind;
    c->last_op.channel = channel;
    c->last_op.result = KVM_CTRL_OP_PENDING;
    c->has_last_op = true;
    if (op_id) *op_id = c->last_op.op_id;
}

static kvm_ctrl_result_t map_logic_result(kvm_result_t r) {
    switch (r) {
        case KVM_RESULT_OK:                     return KVM_CTRL_OK;
        case KVM_RESULT_ERR_CHANNEL_MISMATCH:   return KVM_CTRL_ERR_CHANNEL_MISMATCH;
        case KVM_RESULT_ERR_LOCKED:             return KVM_CTRL_ERR_ATX_LOCKED;
        case KVM_RESULT_ERR_UNKNOWN_STATE:      return KVM_CTRL_ERR_UNKNOWN_STATE;
        case KVM_RESULT_ERR_SWITCHING:          return KVM_CTRL_ERR_BUSY;
        case KVM_RESULT_ERR_INVALID_PARAM:      return KVM_CTRL_ERR_INVALID_PARAM;
        default:                                return KVM_CTRL_ERR_INVALID_PARAM;
    }
}

/* クールダウンの残り(ms)。直近の tick の時刻で数える（now_ms は単調なので、引き算は負にならない） */
static uint32_t cooldown_remaining(const kvm_ctrl_t *c) {
    if (!c->atx_has_last || c->config.atx_min_interval_ms == 0) return 0;
    uint64_t elapsed = c->now_ms - c->atx_last_ms;
    if (elapsed >= c->config.atx_min_interval_ms) return 0;
    return (uint32_t)(c->config.atx_min_interval_ms - elapsed);
}

/* ------------------------------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------------------------------- */

kvm_ctrl_result_t kvm_ctrl_init(kvm_ctrl_t *c, const kvm_ctrl_config_t *config, const kvm_board_hal_t *hal) {
    if (!c) return KVM_CTRL_ERR_INVALID_PARAM;
    memset(c, 0, sizeof(*c));
    c->asserted = NO_LINE;
    if (!config || !hal) return KVM_CTRL_ERR_INVALID_PARAM;
    if (!hal->set_addr || !hal->assert_line || !hal->release_line || !hal->release_all ||
        !hal->read_led_mask || !hal->video_sync) {
        return KVM_CTRL_ERR_INVALID_PARAM;
    }

    /* 設定の検証。kvm_logic_init が logic の設定を検証し、ハード上限へクランプする */
    if (kvm_logic_init(&c->logic, &config->logic) != KVM_RESULT_OK) {
        memset(c, 0, sizeof(*c));
        c->asserted = NO_LINE;
        return KVM_CTRL_ERR_INVALID_PARAM;
    }
    if (config->press_ms == 0 || config->press_ms > c->logic.config.max_power_pulse_ms ||
        config->reset_ms == 0 || config->reset_ms > c->logic.config.max_reset_pulse_ms ||
        config->force_ms == 0 || config->force_ms > c->logic.config.max_force_power_pulse_ms ||
        config->assert_margin_ms == 0) {
        memset(c, 0, sizeof(*c));
        c->asserted = NO_LINE;
        return KVM_CTRL_ERR_INVALID_PARAM;
    }

    c->config = *config;
    c->hal = *hal;
    c->last_sync = KVM_VIDEO_SYNC_UNKNOWN;
    c->next_op_id = 1;
    c->initialized = true;

    /* ① release_all。失敗したら（全線 High を確認できないので）set_addr は呼ばずにフォールトで始まる */
    if (c->hal.release_all(c->hal.ctx) < 0) {
        set_fault(c, KVM_CTRL_FAULT_INIT);
        return KVM_CTRL_ERR_FAULT;
    }

    /* ② 全線 High のまま、kvm_logic の初期出力のアドレスを必ず設定する（差分適用では設定されないため） */
    uint8_t initial = c->logic.current_outputs.a0a1_channel;
    if (c->hal.set_addr(c->hal.ctx, initial) < 0) {
        fault_and_release(c, KVM_CTRL_FAULT_INIT);
        return KVM_CTRL_ERR_FAULT;
    }
    c->board_addr = initial;
    return KVM_CTRL_OK;
}

kvm_ctrl_result_t kvm_ctrl_tick(kvm_ctrl_t *c, uint64_t now_ms) {
    if (!c || !c->initialized) return KVM_CTRL_ERR_NOT_READY;

    if (c->fault) {
        /* ラッチ後は kvm_logic_step の出力を適用しない。release_all の再試行だけを続ける */
        (void)c->hal.release_all(c->hal.ctx);
        return KVM_CTRL_ERR_FAULT;
    }

    if (c->ticked && now_ms < c->now_ms) {
        fault_and_release(c, KVM_CTRL_FAULT_TIME_REWIND);
        return KVM_CTRL_ERR_FAULT;
    }
    c->now_ms = now_ms;
    c->ticked = true;

    uint8_t mask = 0;
    if (c->hal.read_led_mask(c->hal.ctx, &mask) < 0) {
        fault_and_release(c, KVM_CTRL_FAULT_READ_LED);
        return KVM_CTRL_ERR_FAULT;
    }
    kvm_video_sync_state_t sync = c->hal.video_sync(c->hal.ctx);
    if (sync != KVM_VIDEO_SYNC_NO_SIGNAL && sync != KVM_VIDEO_SYNC_LOCKED) sync = KVM_VIDEO_SYNC_UNKNOWN;
    c->last_sync = sync;

    kvm_logic_inputs_t in;
    in.led_raw_state = kvm_ctrl_led_mask_to_raw(mask);
    in.video_sync = sync;
    kvm_logic_outputs_t out;
    kvm_logic_step(&c->logic, &in, now_ms, &out);

    if (!apply_outputs(c, &out)) return KVM_CTRL_ERR_FAULT;
    update_op(c, &out);
    return KVM_CTRL_OK;
}

kvm_ctrl_result_t kvm_ctrl_select(kvm_ctrl_t *c, uint8_t channel, uint32_t *op_id) {
    if (op_id) *op_id = 0;
    if (!c) return KVM_CTRL_ERR_INVALID_PARAM;
    if (!c->initialized) return KVM_CTRL_ERR_NOT_READY;
    if (channel < 1 || channel > 4) return KVM_CTRL_ERR_INVALID_PARAM;
    if (c->fault) return KVM_CTRL_ERR_FAULT;
    /* kvm_logic は unknown での切替要求を SWITCHING で返すので、先に区別する */
    if (c->logic.state == KVM_STATE_UNKNOWN) return KVM_CTRL_ERR_UNKNOWN_STATE;
    if (c->logic.state == KVM_STATE_IDLE && c->logic.current_outputs.active_channel == channel) {
        return KVM_CTRL_ALREADY_SELECTED;
    }

    kvm_result_t r = kvm_logic_request_switch(&c->logic, channel);
    if (r != KVM_RESULT_OK) return map_logic_result(r);

    record_op(c, KVM_CTRL_OP_SELECT, channel, op_id);
    return KVM_CTRL_OK;
}

kvm_ctrl_result_t kvm_ctrl_power(kvm_ctrl_t *c, uint8_t channel, kvm_ctrl_power_action_t action, uint32_t *op_id) {
    if (op_id) *op_id = 0;
    if (!c) return KVM_CTRL_ERR_INVALID_PARAM;
    if (!c->initialized) return KVM_CTRL_ERR_NOT_READY;
    if (channel < 1 || channel > 4) return KVM_CTRL_ERR_INVALID_PARAM;

    kvm_atx_target_t target;
    kvm_ctrl_op_kind_t kind;
    uint32_t pulse_ms;
    switch (action) {
        case KVM_CTRL_POWER_PRESS:
            target = KVM_ATX_TARGET_POWER;       kind = KVM_CTRL_OP_POWER_PRESS; pulse_ms = c->config.press_ms; break;
        case KVM_CTRL_POWER_RESET:
            target = KVM_ATX_TARGET_RESET;       kind = KVM_CTRL_OP_POWER_RESET; pulse_ms = c->config.reset_ms; break;
        case KVM_CTRL_POWER_FORCE:
            target = KVM_ATX_TARGET_POWER_FORCE; kind = KVM_CTRL_OP_POWER_FORCE; pulse_ms = c->config.force_ms; break;
        default:
            return KVM_CTRL_ERR_INVALID_PARAM;
    }

    if (c->fault) return KVM_CTRL_ERR_FAULT;

    /* 判定は kvm_logic の中（unknown -> busy -> channel_mismatch -> atx_locked）。クールダウンは、その後。
     * クールダウンで拒否するときに kvm_logic の状態を進めないよう、コピーで試して、受理するときだけ反映する
     * （同じ関数・同じ状態で判定するので、確認と実行の間に競合しない）。 */
    kvm_logic_t trial = c->logic;
    kvm_result_t r = kvm_logic_request_atx(&trial, channel, target, pulse_ms);
    if (r != KVM_RESULT_OK) return map_logic_result(r);
    if (cooldown_remaining(c) > 0) return KVM_CTRL_ERR_COOLDOWN;

    c->logic = trial;
    c->atx_has_last = true;
    c->atx_last_ms = c->now_ms;
    record_op(c, kind, channel, op_id);
    return KVM_CTRL_OK;
}

static kvm_ctrl_state_t map_state(kvm_logic_state_t s) {
    switch (s) {
        case KVM_STATE_IDLE:
            return KVM_CTRL_STATE_IDLE;
        case KVM_STATE_SWITCH_WAIT_SETUP:
        case KVM_STATE_SWITCH_PULSE:
        case KVM_STATE_SWITCH_WAIT_LED:
        case KVM_STATE_SWITCH_WAIT_VIDEO:
        case KVM_STATE_SWITCH_REVERT_SETUP:
            return KVM_CTRL_STATE_SWITCHING;
        case KVM_STATE_MANUAL_TRACK_WAIT_LED:
        case KVM_STATE_MANUAL_TRACK_WAIT_VIDEO:
            return KVM_CTRL_STATE_TRACKING;
        case KVM_STATE_ATX_WAIT_SETUP:
        case KVM_STATE_ATX_PULSE:
            return KVM_CTRL_STATE_ATX;
        case KVM_STATE_UNKNOWN:
        default:
            return KVM_CTRL_STATE_UNKNOWN;
    }
}

kvm_ctrl_result_t kvm_ctrl_get_status(const kvm_ctrl_t *c, kvm_ctrl_status_t *status) {
    if (!c || !c->initialized) return KVM_CTRL_ERR_NOT_READY;
    if (!status) return KVM_CTRL_ERR_INVALID_PARAM;

    status->state = map_state(c->logic.state);
    status->active_channel = (status->state == KVM_CTRL_STATE_UNKNOWN) ? 0 : c->logic.current_outputs.active_channel;
    status->video_sync = c->last_sync;
    status->atx_locked = c->logic.current_outputs.atx_locked;
    status->atx_anomaly = c->logic.current_outputs.atx_anomaly_detected;
    status->fault = c->fault;
    status->fault_reason = c->fault_reason;
    status->cooldown_ms = cooldown_remaining(c);
    status->has_last_op = c->has_last_op;
    if (c->has_last_op) {
        status->last_op = c->last_op;
    } else {
        memset(&status->last_op, 0, sizeof(status->last_op));
    }
    return KVM_CTRL_OK;
}
