#ifndef USB_VBUS_CTRL_H
#define USB_VBUS_CTRL_H

/*
 * usb_vbus_ctrl: USB デバイスの D+ プルアップの「接続・切断の判断」だけをする、状態を持つ純 C。IDF 非依存。
 * 設計は docs/USB_VBUS_DESIGN.md。仕様の一次は USB 2.0 仕様 §7.1.5.1・§7.2.1（原文は docs/HW_VERIFY.md H.0）:
 *   VBUS がないとき、D+ のプルアップに給電しない。VBUS が外れたら、10 秒以内にプルアップへの給電を外す。
 *
 * このコンポーネントは GPIO を読まず、tud_connect / tud_disconnect も呼ばない。呼び出し側（usb_hid の結線）が、
 *   1. 一定の周期 P（サンプル周期）で VBUS の GPIO を読み、usb_vbus_ctrl_update() に渡す、
 *   2. 返った action（CONNECT / DISCONNECT）を実行し、その結果を usb_vbus_ctrl_applied() で知らせる。
 * 時計も持たない（now_ms は呼び出し側が渡す単調な時計）。
 *
 * 規則
 *   起動時（init 直後）は「切断」（プルアップなし）。コントローラは、自分が「接続した」と思っているか（is_connected）だけを持つ。
 *   「VBUS 検出」= sample_valid && vbus_present。**sample_valid == false（GPIO の読み出しの失敗）は、VBUS なしとみなす**（安全側）。
 *
 *   接続（切断の状態のとき）: VBUS 検出が、connect_debounce_ms 以上、**連続して**続いたら、その tick で CONNECT を返す
 *     （連続の始まりの now_ms からの経過 >= connect_debounce_ms。ちょうどで返す。0 なら、検出された最初の tick）。
 *     applied(CONNECT, ok = true) で「接続」の状態になる。ok = false なら、切断のまま。**デバウンスはやり直さない**:
 *     検出が続いている限り、次の update でも CONNECT を返す（成功するまで再試行）。
 *   切断（接続の状態のとき）: VBUS なし（検出されない、または sample_valid == false）が、disconnect_debounce_ms 以上、**連続して**
 *     続いたら、その tick で DISCONNECT を返す（経過 >= disconnect_debounce_ms）。applied(DISCONNECT, ok = true) で
 *     「切断」の状態になる。**ok = false なら、接続のまま、VBUS なしが続いている限り、次の update でも毎回 DISCONNECT を返す**
 *     （デバウンスはやり直さない。成功するまで再試行する）。
 *   連続の定義: 数えているのは「いまの状態と逆の向き」（切断の状態なら VBUS 検出、接続の状態なら VBUS なし）のサンプルの連続。
 *     逆の向きでないサンプルが 1 回でも来たら、数えを捨てる（再試行の最中でも、VBUS が戻れば、その action は返らなくなる）。
 *     applied(ok = true) で状態が変わったら、数えを捨てる（次の tick から、新しい状態の「逆の向き」を最初から数える）。
 *   時計の逆行: now_ms が直前の tick より小さい（単調でない）とき、数えているデバウンスを、その now_ms から数え直す
 *     （連続の保証が崩れた側に倒す。同時刻は逆行ではない）。切断が遅れる側の影響は、1 回の逆行につき、高々 disconnect_debounce_ms。
 *     経過の計算は 64 ビットの引き算だけで、オーバーフローしない。
 *   enabled == false（検出なし。「いまの動作」）: 最初の update で CONNECT を 1 回だけ返し、以後は NONE。VBUS の入力も now_ms も見ない。
 *     再試行はしない（applied(CONNECT, false) でも、もう返さない）。**この場合、USB 2.0 §7.2.1 を満たさない**
 *     （VBUS がなくてもプルアップが入る）。docs/USB_VBUS_DESIGN.md 3.2。
 *
 * 仕様の不変条件（ホストテストとファズで検査する）
 *   連続する update の now_ms の差（サンプル周期）が P 以下で、VBUS なしのサンプルが連続して始まった時刻を ta とすると、
 *   接続の状態で最初に DISCONNECT が返るのは、now - ta <= disconnect_debounce_ms + P の tick。disconnect_debounce_ms は
 *   USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS 以下に切り詰められるので、P <= 5000 ms なら、10 秒（USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS）未満。
 *   （ta は「最初に VBUS なしを読んだ」時刻。物理的に VBUS が外れたのは、その P 以内の前。したがって、物理的な外れからは
 *    disconnect_debounce_ms + 2P 以下。呼び出し側は、P と、update から tud_disconnect までの遅れを小さく保つこと。）
 *   VBUS が一度も検出されなければ（sample_valid == false を含む）、CONNECT は決して返らない。
 *
 * NULL: init(NULL) は何もしない。update(NULL, ...) は NONE。update(c, input == NULL) は「sample_valid == false、
 *   now_ms は直前の値」のサンプルとして扱う（安全側。CONNECT は返らない）。init の config == NULL は既定値（enabled = false）。
 *   is_connected(NULL) は false。applied(NULL, ...) は何もしない。get_stats(NULL, out) は out を 0 にする。out == NULL は何もしない。
 *
 * スレッド: 状態の更新は 1 つのタスクからだけ行う（update と applied は同じタスク）。ロックしない。
 */

#include <stdbool.h>
#include <stdint.h>

/* 既定値。TODO(hw-verify): VBUS の立ち上がり・ノイズ（分圧の回路、PC の USB ポート）に合わせて、実機で決める。 */
#define USB_VBUS_CTRL_DEFAULT_CONNECT_DEBOUNCE_MS 500u
#define USB_VBUS_CTRL_DEFAULT_DISCONNECT_DEBOUNCE_MS 1000u

/* USB 2.0 §7.2.1: VBUS が外れたら、この時間以内にプルアップへの給電を外す。 */
#define USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS 10000u
/* disconnect_debounce_ms の上限（仕様の上限の半分。サンプル周期と呼び出しの遅れの余裕）。init が切り詰める。 */
#define USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS 5000u

typedef struct {
    bool enabled;                     /* false = VBUS を検出しない（「いまの動作」）。USB_DEVICE_VBUS_SENSE_GPIO >= 0 で true */
    uint32_t connect_debounce_ms;     /* VBUS 検出がこの時間、連続したら接続 */
    uint32_t disconnect_debounce_ms;  /* VBUS なしがこの時間、連続したら切断。init で MAX_MS に切り詰める */
} usb_vbus_ctrl_config_t;

typedef struct {
    uint64_t now_ms;     /* 呼び出し側の単調な時計（ミリ秒） */
    bool vbus_present;   /* GPIO の読み出し値（VBUS あり = true） */
    bool sample_valid;   /* 読み出しが成功したか。false のときは、vbus_present を見ず、VBUS なしとみなす */
} usb_vbus_ctrl_input_t;

typedef enum {
    USB_VBUS_CTRL_ACTION_NONE = 0,
    USB_VBUS_CTRL_ACTION_CONNECT,    /* tud_connect を呼ぶ（プルアップを入れる） */
    USB_VBUS_CTRL_ACTION_DISCONNECT, /* tud_disconnect を呼ぶ（プルアップを外す） */
} usb_vbus_ctrl_action_t;

typedef struct {
    uint32_t connects;     /* applied(CONNECT, ok = true) の回数 */
    uint32_t disconnects;  /* applied(DISCONNECT, ok = true) の回数 */
    uint32_t failures;     /* applied(CONNECT / DISCONNECT, ok = false) の回数 */
} usb_vbus_ctrl_stats_t;

typedef struct {
    usb_vbus_ctrl_config_t config;
    bool connected;        /* コントローラが「接続した」と思っているか */
    bool run_active;       /* 「逆の向き」のサンプルが連続している最中 */
    uint64_t run_start_ms; /* 連続の始まりの now_ms（run_active のときだけ有効） */
    uint64_t last_now_ms;  /* 直前の tick の now_ms（最初は 0） */
    bool legacy_done;      /* enabled == false で CONNECT を返したか */
    usb_vbus_ctrl_stats_t stats;
} usb_vbus_ctrl_t;

/* 既定: enabled = false、接続 500 ms、切断 1000 ms。 */
usb_vbus_ctrl_config_t usb_vbus_ctrl_config_default(void);

/* 状態を「切断」・数えなし・統計 0 にして、config を保持する（disconnect_debounce_ms は MAX_MS に切り詰める）。
 * config == NULL は既定値。c == NULL は何もしない。 */
void usb_vbus_ctrl_init(usb_vbus_ctrl_t *c, const usb_vbus_ctrl_config_t *config);

/* 1 サンプル進めて、呼び出し側がすべき action を返す。規則は上の説明。 */
usb_vbus_ctrl_action_t usb_vbus_ctrl_update(usb_vbus_ctrl_t *c, const usb_vbus_ctrl_input_t *input);

/* update が返した action を実行した結果を知らせる。NONE は無視。 */
void usb_vbus_ctrl_applied(usb_vbus_ctrl_t *c, usb_vbus_ctrl_action_t action, bool ok);

/* コントローラが、いま「接続した」と思っているか。c == NULL は false。 */
bool usb_vbus_ctrl_is_connected(const usb_vbus_ctrl_t *c);

void usb_vbus_ctrl_get_stats(const usb_vbus_ctrl_t *c, usb_vbus_ctrl_stats_t *out);

#endif /* USB_VBUS_CTRL_H */
