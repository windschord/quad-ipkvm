#ifndef KC_COMMON_H
#define KC_COMMON_H

/*
 * kvm_ctrl のテスト共通の部品（フィクスチャ）。各テストファイルが include する（実行ファイルごとに別のコピー）。
 *
 * 時刻の約束: tick は 10 ms 刻み（既定の setup_time_ms = 10 以上）。テストは fx_tick(t) で、
 * モックの時刻（ボードのタイマー）と kvm_ctrl_tick を同じ時刻に進める。
 */

#include "unity.h"
#include "kvm_ctrl.h"
#include "kvm_board_mock.h"
#include <string.h>
#include <stdio.h>

static kvm_board_mock_t M;
static kvm_ctrl_t C;
static kvm_ctrl_config_t CFG;
static uint64_t T;

static inline kvm_ctrl_result_t fx_init(void) {
    kvm_board_hal_t hal = mock_hal(&M);
    return kvm_ctrl_init(&C, &CFG, &hal);
}

/* モックの時刻と kvm_ctrl_tick を time_ms に進める */
static inline kvm_ctrl_result_t fx_tick(uint64_t time_ms) {
    T = time_ms;
    mock_advance(&M, time_ms);
    return kvm_ctrl_tick(&C, time_ms);
}

/* 10 ms 進めて tick する */
static inline kvm_ctrl_result_t fx_step(void) { return fx_tick(T + 10); }

/* n 回 fx_step する（最後の戻り値） */
static inline kvm_ctrl_result_t fx_steps(int n) {
    kvm_ctrl_result_t r = KVM_CTRL_OK;
    for (int i = 0; i < n; i++) r = fx_step();
    return r;
}

/* 設定を既定にして、モック（ボードのアドレスは addr）を起動直後にする。init はしない */
static inline void fx_reset(uint8_t board_addr) {
    mock_init(&M, board_addr);
    memset(&C, 0, sizeof(C));
    kvm_ctrl_config_default(&CFG);
    T = 0;
}

/* LED が ch を示す状態で起動し、idle(active == ch) まで進める。init 後の記録は消す */
static inline void fx_boot(uint8_t ch) {
    fx_reset(1);
    mock_set_led_channel(&M, ch);
    mock_set_sync(&M, KVM_VIDEO_SYNC_LOCKED);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    fx_tick(0);
    for (int i = 0; i < 6; i++) fx_step(); /* 10..60 ms。LED の安定 50 ms を超える */
    kvm_ctrl_status_t st;
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_get_status(&C, &st));
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(ch, st.active_channel);
    TEST_ASSERT_FALSE(st.atx_locked);
    mock_reset_calls(&M);
}

static inline kvm_ctrl_status_t fx_status(void) {
    kvm_ctrl_status_t st;
    memset(&st, 0xAB, sizeof(st));
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_get_status(&C, &st));
    return st;
}

/* 条件が成り立つまで fx_step する（max_steps で打ち切り。成り立たなければ false） */
typedef bool (*fx_pred_t)(void);
static inline bool fx_run_until(fx_pred_t pred, int max_steps) {
    for (int i = 0; i < max_steps; i++) {
        if (pred()) return true;
        fx_step();
    }
    return pred();
}

static inline bool fx_state_is_idle(void) { return fx_status().state == KVM_CTRL_STATE_IDLE; }

/* 記録から、fn の index 番目(0 始まり)の呼び出しの位置を返す（無ければ -1） */
static inline int fx_find(mock_fn_t fn, int nth) {
    int seen = 0;
    for (size_t i = 0; i < M.log_len; i++) {
        if (M.log[i].fn == fn) {
            if (seen == nth) return (int)i;
            seen++;
        }
    }
    return -1;
}

/* 記録の index 番目が、期待どおりの呼び出しか */
static inline void fx_expect_call(size_t index, mock_fn_t fn, int arg) {
    const mock_call_t *e = mock_log_at(&M, index);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(fn, e->fn);
    TEST_ASSERT_EQUAL_INT(arg, e->arg);
}

/*
 * 記録のうち index 以降の「出力を動かす呼び出し」だけを、文字列にする（比較しやすいように）。
 *   set_addr(n) -> "a<n>@<時刻>"、assert_line(line, max_ms) -> "S<line>:<max_ms>@<時刻>"、
 *   release_line(line) -> "r<line>@<時刻>"、release_all -> "R@<時刻>"。失敗した呼び出しは末尾に "!"。
 * line の番号: POWER=0, POWER_FORCE=1, RESET=2, KVM=3。
 */
static inline const char *fx_trace_from(size_t index) {
    static char buf[4096];
    size_t n = 0;
    buf[0] = '\0';
    for (size_t i = index; i < M.log_len && n + 40 < sizeof(buf); i++) {
        const mock_call_t *e = &M.log[i];
        const char *bang = e->ret < 0 ? "!" : "";
        int w = 0;
        switch (e->fn) {
            case MOCK_FN_SET_ADDR:
                w = snprintf(buf + n, sizeof(buf) - n, "%sa%d@%llu%s", n ? " " : "", e->arg, (unsigned long long)e->at_ms, bang);
                break;
            case MOCK_FN_ASSERT_LINE:
                w = snprintf(buf + n, sizeof(buf) - n, "%sS%d:%u@%llu%s", n ? " " : "", e->arg, (unsigned)e->max_ms, (unsigned long long)e->at_ms, bang);
                break;
            case MOCK_FN_RELEASE_LINE:
                w = snprintf(buf + n, sizeof(buf) - n, "%sr%d@%llu%s", n ? " " : "", e->arg, (unsigned long long)e->at_ms, bang);
                break;
            case MOCK_FN_RELEASE_ALL:
                w = snprintf(buf + n, sizeof(buf) - n, "%sR@%llu%s", n ? " " : "", (unsigned long long)e->at_ms, bang);
                break;
            default:
                break;
        }
        if (w > 0) n += (size_t)w;
    }
    return buf;
}
static inline const char *fx_trace(void) { return fx_trace_from(0); }

/* 切替の要求から完了まで、LED をボタンに合わせて模擬する(LED は pulse の後に target へ変える)。idle まで進める。 */
static inline void fx_complete_switch(uint8_t target) {
    int guard = 0;
    while (fx_status().state != KVM_CTRL_STATE_IDLE && guard++ < 1000) {
        if (C.logic.state == KVM_STATE_SWITCH_WAIT_LED) { /* ボタンのパルスが終わった */
            mock_set_led_channel(&M, target);
        }
        fx_step();
    }
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, fx_status().state);
}

#endif /* KC_COMMON_H */
