#ifndef KVM_BOARD_MOCK_H
#define KVM_BOARD_MOCK_H

/*
 * kvm_board_hal_t のモック（ホスト用）。docs/INTERFACES.md 6.2 の「ボード側の義務」を再現する:
 *   1. 起動直後は全線 High（無効）
 *   2. assert_line は同時に 1 本まで（POWER と POWER_FORCE を含め、2 本目は失敗 -1）
 *   3. 種別ごとの上限でクランプし、タイマーで自動解除（mock_advance() で時間を進める）
 *   4. set_addr は、どれかの線が有効なら失敗 -1
 *
 * モックが拒否したことと、kvm_ctrl が「そもそも呼ばないこと」は別。モックは、義務に反する呼び出しを
 * 受けたら（拒否すると同時に）**違反フラグ**を残す。テストは、kvm_ctrl が違反を起こさないことを固定する。
 *
 * ここにある上限は、モック専用のテスト値であって、実機の値ではない（TODO(hw-verify)、A-9）。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kvm_board.h"

#define MOCK_MAX_POWER_MS        500u
#define MOCK_MAX_POWER_FORCE_MS  5000u
#define MOCK_MAX_RESET_MS        200u
#define MOCK_MAX_KVM_MS          300u

#define MOCK_LOG_MAX 4096

typedef enum {
    MOCK_FN_SET_ADDR = 0,
    MOCK_FN_ASSERT_LINE,
    MOCK_FN_RELEASE_LINE,
    MOCK_FN_RELEASE_ALL,
    MOCK_FN_READ_LED,
    MOCK_FN_VIDEO_SYNC,
    MOCK_FN_COUNT
} mock_fn_t;

typedef struct {
    mock_fn_t fn;
    int arg;                 /* set_addr: channel / assert_line, release_line: kvm_board_line_t */
    uint32_t max_ms;         /* assert_line: 要求された max_ms */
    uint32_t effective_ms;   /* assert_line: クランプ後（成功したときだけ有効） */
    int ret;                 /* 戻り値（< 0 は失敗） */
    uint64_t at_ms;          /* モックの時刻 */
} mock_call_t;

typedef struct {
    uint64_t now_ms;

    /* ボードの状態 */
    bool active[KVM_BOARD_LINE_COUNT];
    uint64_t deadline_ms[KVM_BOARD_LINE_COUNT];
    uint8_t addr;                        /* 現在の A0/A1（1..4） */
    unsigned auto_releases;              /* タイマーによる自動解除の回数 */

    /* 入力の模擬 */
    uint8_t led_mask;
    bool chatter;                        /* true なら read_led_mask のたびに chatter_a / chatter_b を交互に返す */
    uint8_t chatter_a, chatter_b;
    bool chatter_phase;
    kvm_video_sync_state_t sync;

    /* 呼び出しの記録 */
    mock_call_t log[MOCK_LOG_MAX];
    size_t log_len;
    size_t log_dropped;                  /* 記録の容量を超えた分 */
    unsigned calls[MOCK_FN_COUNT];       /* 関数ごとの呼び出し回数（mock_reset_calls まで累積） */

    /* 失敗の注入 */
    int fail_in[MOCK_FN_COUNT];          /* n > 0: n 回目の呼び出し（1 = 次の呼び出し）を失敗させる。1 回だけ */
    bool fail_all[MOCK_FN_COUNT];        /* true: 以後すべての呼び出しが失敗 */

    /* 違反フラグ（モックの拒否とは別に、kvm_ctrl がそもそも呼ばないことを固定する） */
    bool viol_set_addr_while_active;     /* どれかの線が有効なときに set_addr が呼ばれた */
    bool viol_second_assert;             /* 線が有効なときに assert_line が呼ばれた（同じ線の再 assert を含む） */
    bool viol_bad_arg;                   /* チャンネル・線の種別が範囲外、NULL の出力先 */
    bool viol_zero_max_ms;               /* assert_line の max_ms == 0 */
} kvm_board_mock_t;

/* 起動直後の状態にする（全線 High、A0/A1 = initial_addr、LED 無点灯、映像 UNKNOWN、記録・注入・違反フラグを消す） */
void mock_init(kvm_board_mock_t *m, uint8_t initial_addr);
kvm_board_hal_t mock_hal(kvm_board_mock_t *m);

/* 時間を進める。期限が来た線を自動解除する（ボードのハードウェアタイマーの模擬）。now_ms は戻さない */
void mock_advance(kvm_board_mock_t *m, uint64_t now_ms);

/* 状態の問い合わせ */
bool mock_line_active(const kvm_board_mock_t *m, kvm_board_line_t line);
int  mock_active_count(const kvm_board_mock_t *m);
bool mock_violated(const kvm_board_mock_t *m);
uint32_t mock_line_cap_ms(kvm_board_line_t line);

/* LED の模擬 */
void mock_set_led_mask(kvm_board_mock_t *m, uint8_t mask);
void mock_set_led_channel(kvm_board_mock_t *m, uint8_t channel);  /* 1..4 */
void mock_set_led_off(kvm_board_mock_t *m);
void mock_set_led_multi(kvm_board_mock_t *m);                      /* ch1 と ch2 が点灯 */
void mock_set_led_chatter(kvm_board_mock_t *m, uint8_t mask_a, uint8_t mask_b);
void mock_clear_led_chatter(kvm_board_mock_t *m);
void mock_set_sync(kvm_board_mock_t *m, kvm_video_sync_state_t s);

/* 失敗の注入 */
void mock_fail_nth(kvm_board_mock_t *m, mock_fn_t fn, int n);
void mock_fail_all(kvm_board_mock_t *m, mock_fn_t fn, bool on);
void mock_clear_failures(kvm_board_mock_t *m);

/* 記録 */
void mock_reset_calls(kvm_board_mock_t *m);                        /* 記録と呼び出し回数を消す（状態・注入・違反は残す） */
unsigned mock_calls(const kvm_board_mock_t *m, mock_fn_t fn);
const mock_call_t *mock_log_at(const kvm_board_mock_t *m, size_t index);
/* 記録のうち index 以降で、fn の呼び出しが何回あったか */
unsigned mock_count_since(const kvm_board_mock_t *m, size_t index, mock_fn_t fn);

#endif /* KVM_BOARD_MOCK_H */
