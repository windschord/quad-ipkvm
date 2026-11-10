#ifndef KVM_CTRL_H
#define KVM_CTRL_H

/*
 * kvm_ctrl: kvm_logic（状態機械）とボード HAL（kvm_board_hal_t）の結線。純 C、IDF 非依存。
 * 契約は docs/INTERFACES.md 6.3、API の意味は docs/WEB_API.md 7 章。
 *
 * 役割
 *   - 1 回の kvm_ctrl_tick() で、LED と映像を読み、kvm_logic_step() を 1 回進め、出力を「安全な順序」で
 *     HAL に適用する（無効にする線を release -> 全線 High のときだけ set_addr -> 新たに有効にする線を assert）。
 *   - 要求 API（kvm_ctrl_select / kvm_ctrl_power）は kvm_logic に状態を持たせるだけで、HAL は呼ばない。
 *     出力はすべて tick から出る。
 *   - HAL の失敗は、即座に release_all してフォールトをラッチする。ラッチは再起動まで解除しない。
 *     ラッチ後の tick は kvm_logic_step の出力を HAL に適用しない（release_all の再試行だけ）。
 *
 * スレッド
 *   kvm_ctrl 自体はロックしない。呼び出し側（kvm_service）が 1 つのミューテックスで、
 *   kvm_ctrl_tick / kvm_ctrl_select / kvm_ctrl_power / kvm_ctrl_get_status を直列化する。
 *   HAL の関数は、そのミューテックスを持ったまま呼ばれる。HAL の中から kvm_ctrl を呼んではならない。
 *
 * 時刻
 *   now_ms は単調増加のミリ秒（uint64_t）。同じ値の繰り返しは許す。**巻き戻ったら**（now_ms が前回の tick より
 *   小さい）、kvm_logic の経過時間（符号なしの引き算）が巨大になり、セットアップ時間などが一瞬で満了してしまうので、
 *   即座に release_all してフォールトをラッチする（安全側。復旧は再起動）。
 *   要求 API は now_ms を取らない。クールダウンの判定には「直近の tick の now_ms」を使う
 *   （tick の周期 KVM_TICK_MS の分だけ粗い）。クールダウンの起点は「ATX 操作が終わった tick」
 *   （パルスを出し終えた、または中断した時点。強制断のような長いパルスの直後にも間隔を空けるため）。
 *   実行中の ATX 操作がある間は busy で拒否されるので、cooldown_ms は 0。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kvm_logic.h"
#include "kvm_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 設定 ---- */
typedef struct {
    kvm_logic_config_t logic;      /* 状態機械の設定（kvm_logic_init が検証・ハード上限へクランプする） */

    /* ATX のパルス長はサーバーの設定で決める（クライアントは決められない）。
     * 各値は 1 以上で、対応する logic.max_*_pulse_ms（ハード上限 KVM_ATX_HARD_MAX_* でクランプ後）以下。 */
    uint32_t press_ms;             /* TODO(hw-verify): 電源ボタンの短押し。0 や上限超えは kvm_ctrl_init が拒否 */
    uint32_t reset_ms;             /* TODO(hw-verify): リセットボタン */
    uint32_t force_ms;             /* TODO(hw-verify): 電源ボタンの長押し（強制断） */

    uint32_t atx_min_interval_ms;  /* TODO(hw-verify): ソフトのクールダウン。ATX 操作が終わってから（完了でも中断でも）
                                      次の ATX を受理するまでの最小間隔。0 = クールダウンなし */
    uint32_t assert_margin_ms;     /* assert_line の max_ms = パルス長 + この余裕。1 以上。
                                      tick の周期とジッタより大きくすること（ボードのタイマーが、ソフトの正常な解除より
                                      先に切れないように）。TODO(hw-verify) */
} kvm_ctrl_config_t;

/* 既定値（kvm_logic_config_default を内包）。値はすべて TODO(hw-verify)。 */
void kvm_ctrl_config_default(kvm_ctrl_config_t *config);

/* ---- 結果コード ---- */
typedef enum {
    KVM_CTRL_OK = 0,                    /* 受理（select / power）、または成功（init / tick） */
    KVM_CTRL_ALREADY_SELECTED,          /* select: すでにそのチャンネルで idle（何も押さない。op_id は進まない） */
    KVM_CTRL_ERR_INVALID_PARAM,         /* 引数が不正（NULL、channel が 1..4 でない、action が範囲外、設定・HAL の不備） */
    KVM_CTRL_ERR_BUSY,                  /* 切替中・ATX パルス中・追従中 */
    KVM_CTRL_ERR_UNKNOWN_STATE,         /* 現在チャンネルを確定できない */
    KVM_CTRL_ERR_CHANNEL_MISMATCH,      /* power: channel が現在のチャンネルと違う */
    KVM_CTRL_ERR_ATX_LOCKED,            /* ATX ロック中 */
    KVM_CTRL_ERR_COOLDOWN,              /* 前回の ATX 操作が終わってから atx_min_interval_ms が経っていない */
    KVM_CTRL_ERR_FAULT,                 /* フォールトがラッチされている（tick / init の戻り値としても使う） */
    KVM_CTRL_ERR_NOT_READY              /* kvm_ctrl_init が成功していない（ctrl が使えない） */
} kvm_ctrl_result_t;

typedef enum { KVM_CTRL_POWER_PRESS = 0, KVM_CTRL_POWER_RESET, KVM_CTRL_POWER_FORCE } kvm_ctrl_power_action_t;

/* ---- 状態の報告（WEB_API.md 7.1） ---- */
typedef enum {
    KVM_CTRL_STATE_UNKNOWN = 0,
    KVM_CTRL_STATE_IDLE,
    KVM_CTRL_STATE_SWITCHING,
    KVM_CTRL_STATE_TRACKING,
    KVM_CTRL_STATE_ATX
} kvm_ctrl_state_t;

typedef enum {
    KVM_CTRL_OP_SELECT = 0,
    KVM_CTRL_OP_POWER_PRESS,
    KVM_CTRL_OP_POWER_RESET,
    KVM_CTRL_OP_POWER_FORCE
} kvm_ctrl_op_kind_t;

typedef enum { KVM_CTRL_OP_PENDING = 0, KVM_CTRL_OP_COMPLETED, KVM_CTRL_OP_ABORTED } kvm_ctrl_op_result_t;

typedef struct {
    uint32_t op_id;                     /* 起動からの通し番号（1 から） */
    kvm_ctrl_op_kind_t kind;
    uint8_t channel;                    /* select: 目標のチャンネル / power: 対象のチャンネル */
    kvm_ctrl_op_result_t result;
} kvm_ctrl_op_t;

/* フォールトの原因（診断用。ラッチされた最初の原因を保持する） */
typedef enum {
    KVM_CTRL_FAULT_NONE = 0,
    KVM_CTRL_FAULT_INIT,                /* 初期化の release_all / set_addr が失敗 */
    KVM_CTRL_FAULT_SET_ADDR,
    KVM_CTRL_FAULT_ASSERT_LINE,
    KVM_CTRL_FAULT_RELEASE_LINE,
    KVM_CTRL_FAULT_READ_LED,
    KVM_CTRL_FAULT_LOGIC_OUTPUT,        /* kvm_logic の出力が不正（複数の Ē が同時に Low、A0/A1 が範囲外 等） */
    KVM_CTRL_FAULT_TIME_REWIND          /* now_ms が巻き戻った */
} kvm_ctrl_fault_t;

typedef struct {
    kvm_ctrl_state_t state;
    uint8_t active_channel;             /* 1..4。state が UNKNOWN のときは 0 */
    kvm_video_sync_state_t video_sync;  /* 直近の tick で読んだ値 */
    bool atx_locked;
    bool atx_anomaly;
    bool fault;
    kvm_ctrl_fault_t fault_reason;
    uint32_t cooldown_ms;               /* 0 は「クールダウンによる拒否がない」だけの意味 */
    bool has_last_op;
    kvm_ctrl_op_t last_op;
} kvm_ctrl_status_t;

/* ---- コンテキスト（フィールドは内部用。テストが状態を確認するために公開している） ---- */
typedef struct {
    bool initialized;
    kvm_ctrl_config_t config;
    kvm_board_hal_t hal;
    kvm_logic_t logic;

    uint64_t now_ms;                    /* 直近の tick の時刻 */
    bool ticked;                        /* tick を 1 回でも実行した */

    bool fault;
    kvm_ctrl_fault_t fault_reason;

    /* 適用済みの出力（差分適用のため）。asserted は直近に assert_line した線（無ければ KVM_BOARD_LINE_COUNT） */
    uint8_t board_addr;                 /* ボードに設定済みの A0/A1（0 = 未設定） */
    kvm_board_line_t asserted;

    kvm_video_sync_state_t last_sync;

    uint32_t next_op_id;
    bool has_last_op;
    kvm_ctrl_op_t last_op;

    bool atx_cooldown_armed;            /* クールダウンを数えている（直近に受理した ATX 操作が終わった）。実行中は false */
    uint64_t atx_last_ms;               /* 直近の ATX 操作が終わった（完了または中断した）tick の now_ms */
} kvm_ctrl_t;

/* ---- API ---- */

/**
 * 初期化。①release_all ②全線 High のまま set_addr(kvm_logic の初期出力のアドレス) ③以後は差分適用。
 * @return KVM_CTRL_OK: 成功。
 *         KVM_CTRL_ERR_INVALID_PARAM: NULL、設定の不備、HAL の関数ポインタが 1 つでも NULL。
 *             HAL は一切呼ばず、ctrl は使えない状態（kvm_ctrl_* は NOT_READY を返す）。
 *         KVM_CTRL_ERR_FAULT: ①か②が失敗した。ctrl は使える状態で、**フォールトがラッチされている**
 *             （tick は release_all の再試行だけを続ける。再起動まで解除しない）。
 */
kvm_ctrl_result_t kvm_ctrl_init(kvm_ctrl_t *ctrl, const kvm_ctrl_config_t *config, const kvm_board_hal_t *hal);

/**
 * 1 回の周期処理（周期 KVM_TICK_MS）。
 * @return KVM_CTRL_OK: 通常。KVM_CTRL_ERR_FAULT: フォールトがラッチされている（この tick で新たにラッチした場合を含む）。
 *         KVM_CTRL_ERR_NOT_READY: ctrl が NULL または未初期化。
 */
kvm_ctrl_result_t kvm_ctrl_tick(kvm_ctrl_t *ctrl, uint64_t now_ms);

/**
 * チャンネル切替の要求（HAL は呼ばない）。判定の順序: 引数 -> fault -> unknown -> 既に選択済み -> busy。
 * 受理（KVM_CTRL_OK）のとき *op_id に操作 ID を返す。それ以外は *op_id = 0（op_id は NULL でもよい）。
 */
kvm_ctrl_result_t kvm_ctrl_select(kvm_ctrl_t *ctrl, uint8_t channel, uint32_t *op_id);

/**
 * 電源操作（ATX）の要求（HAL は呼ばない）。パルス長はサーバーの設定（config）から決まる。
 * 判定の順序（WEB_API.md 7.3）: 引数 -> fault -> unknown -> busy -> channel_mismatch -> atx_locked -> cooldown -> 受理。
 * cooldown は、直近の ATX 操作が終わってから atx_min_interval_ms が経つまで。
 * unknown / busy / channel_mismatch / atx_locked の判定は kvm_logic_request_atx の中で行う
 * （事前に自前で比較して TOCTOU を作らない）。
 */
kvm_ctrl_result_t kvm_ctrl_power(kvm_ctrl_t *ctrl, uint8_t channel, kvm_ctrl_power_action_t action, uint32_t *op_id);

/* 状態の取得（WEB_API.md 7.1）。ctrl が NULL / 未初期化 / status が NULL のときは ERR_NOT_READY / ERR_INVALID_PARAM。 */
kvm_ctrl_result_t kvm_ctrl_get_status(const kvm_ctrl_t *ctrl, kvm_ctrl_status_t *status);

/* LED のビットマスク（ビット 0..3 = ch1..4）を kvm_logic の led_raw_state に変換する。
 * ちょうど 1 ビットなら 1..4、無点灯は 0、複数点灯は 0xFF。ビット 4 以上は無視する。 */
uint8_t kvm_ctrl_led_mask_to_raw(uint8_t mask);

#ifdef __cplusplus
}
#endif

#endif /* KVM_CTRL_H */
