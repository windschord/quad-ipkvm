#ifndef USB_VBUS_CTRL_H
#define USB_VBUS_CTRL_H

/*
 * usb_vbus_ctrl: USB デバイスの D+ プルアップの「接続・切断の判断」だけをする、状態を持つ純 C。IDF 非依存。
 * 設計は docs/USB_VBUS_DESIGN.md。仕様の一次は USB 2.0 仕様 §7.1.5.1・§7.2.1（原文は docs/HW_VERIFY.md H.0）:
 *   VBUS がないとき、D+ のプルアップに給電しない。VBUS が外れたら、10 秒以内にプルアップへの給電を外す。
 *
 * このコンポーネントは GPIO を読まず、tud_connect / tud_disconnect も呼ばない。呼び出し側（usb_hid の結線）が、
 *   1. 一定の周期 P（サンプル周期）で VBUS の GPIO を読み、usb_vbus_ctrl_update() に渡す、
 *   2. 返った action（CONNECT / DISCONNECT / FORCE_OFF）を実行し、その結果を usb_vbus_ctrl_applied() で知らせる。
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
 *   強制遮断（接続の状態で、切断が成功しないとき。docs/USB_VBUS_DESIGN.md 3.2）: 最初の DISCONNECT を返した tick の now_ms から、
 *     force_off_after_ms 以上経った tick で、DISCONNECT の代わりに FORCE_OFF を返す（経過 >= force_off_after_ms。ちょうどで返す）。
 *     以後、VBUS なしが続き、成功しない限り、毎回 FORCE_OFF（DISCONNECT には戻らない）。
 *     - 待ちの起点は「最初の DISCONNECT を返した時刻」。DISCONNECT を一度も返していなければ、FORCE_OFF は返さない
 *       （サンプルの間隔が長くて、最初の tick が既にデバウンスを大きく超えていても、最初は DISCONNECT。
 *        ソフトの切断を試してから、強制遮断に進む）。
 *     - applied(FORCE_OFF, ok = true) で「切断」の状態になる（強制遮断の回数 force_off_count を数える。disconnects には数えない）。
 *       ok = false なら接続のまま、次の update でも FORCE_OFF（再試行）。DISCONNECT が成功すれば、強制遮断の待ちはなくなる。
 *       VBUS が戻れば（VBUS なしでないサンプル）、連続が切れるので、強制遮断の待ちも、最初からやり直す（再び VBUS なしになったら、
 *       デバウンス → DISCONNECT → 待ち、の順）。
 *     - force_off_after_ms は、init で 1..USB_VBUS_CTRL_FORCE_OFF_MAX_MS に切り詰める（0 は 1。理由: 小さいほど、10 秒の
 *       保証側に倒れる。0 を既定に直すと、ゼロ初期化の設定が、黙って長い待ちになる。0 は「待たない」の意味で、最短の 1 ms）。
 *     - FORCE_OFF が実際に何をするか（DCTL.sftdiscon の直接書き込み、tinyusb_driver_uninstall、UTMI の PHY を落とす）は、
 *       呼び出し側（usb_hid の結線）の責任。このコンポーネントは判断だけ。
 *   連続の定義: 数えているのは「いまの状態と逆の向き」（切断の状態なら VBUS 検出、接続の状態なら VBUS なし）のサンプルの連続。
 *     逆の向きでないサンプルが 1 回でも来たら、数えを捨てる（再試行の最中でも、VBUS が戻れば、その action は返らなくなる）。
 *     applied(ok = true) で状態が変わったら、数えを捨てる（次の tick から、新しい状態の「逆の向き」を最初から数える）。
 *   時計の逆行: now_ms が直前の tick より小さい（単調でない）とき、数えているデバウンスを、その now_ms から数え直す
 *     （連続の保証が崩れた側に倒す。同時刻は逆行ではない）。強制遮断の待ちも、同じく数え直す（DISCONNECT を返した記録も捨てる）。
 *     切断が遅れる側の影響は、1 回の逆行につき、高々 disconnect_debounce_ms + force_off_after_ms。
 *     （逆行が続く限り、保証は崩れる。now_ms は単調な時計であること。下の不変条件は、単調な now_ms が前提。）
 *     経過の計算は 64 ビットの引き算だけで、オーバーフローしない。
 *   enabled == false（検出なし。「いまの動作」）: 最初の update で CONNECT を 1 回だけ返し、以後は NONE（FORCE_OFF も返さない）。VBUS の入力も now_ms も見ない。
 *     再試行はしない（applied(CONNECT, false) でも、もう返さない）。**この場合、USB 2.0 §7.2.1 を満たさない**
 *     （VBUS がなくてもプルアップが入る）。docs/USB_VBUS_DESIGN.md 3.2。
 *
 * 仕様の不変条件（ホストテストとファズで検査する）
 *   記号: P = update の呼び出し周期の上限（連続する update の now_ms の差。<= USB_VBUS_CTRL_POLL_PERIOD_MAX_MS）、
 *   D = disconnect_debounce_ms、F = force_off_after_ms（init で切り詰めた後の値）。判定は update の呼び出しごとにしか行われないので、
 *   P の分が、段ごとに加算される。VBUS が（物理的に）外れてから、接続の状態で:
 *   1. 最初の VBUS なしを観測するまで <= P。
 *   2. 最初の action（DISCONNECT。成功すれば、ここで終わり）が返るまで、合計 <= 2P + D。
 *   3. applied(DISCONNECT, false) が続く場合、FORCE_OFF が返るまで、合計 <= 3P + D + F（最初の action から <= F + P）。
 *   P <= 1000、D <= 3000（MAX_MS）、F <= 2000（MAX_MS）のとき、2 と 3 は 8000 ms 以下。USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS（10 秒）から、
 *   操作の完了（DCTL への書き込みと、update を呼ぶタスクの実行待ち）の余裕 USB_VBUS_CTRL_COMPLETION_MARGIN_MS（2 秒）を引いた値以下
 *   （コンパイル時に検査する）。P = 2000 にすると、3P + D + F = 11000 で、10 秒を超える。
 *   これらは、applied が成功を返す限り（DISCONNECT が成功すれば 2 で終わり、失敗が続けば FORCE_OFF まで）の話。
 *   FORCE_OFF も失敗し続ける故障では、仕様を満たせない（呼び出し側が、ESP_LOGE と統計に残す）。
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
#define USB_VBUS_CTRL_DEFAULT_FORCE_OFF_AFTER_MS 1000u

/* USB 2.0 §7.2.1: VBUS が外れたら、この時間以内にプルアップへの給電を外す。 */
#define USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS 10000u
/* disconnect_debounce_ms の上限。init が切り詰める。 */
#define USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS 3000u
/* force_off_after_ms の上限（init が切り詰める。下限は 1）。 */
#define USB_VBUS_CTRL_FORCE_OFF_MAX_MS 2000u
/* 呼び出し側が守る update の呼び出し周期の上限（規則。コンポーネントは検査できない）。 */
#define USB_VBUS_CTRL_POLL_PERIOD_MAX_MS 1000u
/* 操作の完了（DCTL への書き込みと、タスクの実行待ち）の余裕。 */
#define USB_VBUS_CTRL_COMPLETION_MARGIN_MS 2000u

typedef struct {
    bool enabled;                     /* false = VBUS を検出しない（「いまの動作」）。USB_DEVICE_VBUS_SENSE_GPIO >= 0 で true */
    uint32_t connect_debounce_ms;     /* VBUS 検出がこの時間、連続したら接続 */
    uint32_t disconnect_debounce_ms;  /* VBUS なしがこの時間、連続したら切断。init で MAX_MS に切り詰める */
    uint32_t force_off_after_ms;      /* 切断が成功しないとき、最初の DISCONNECT を返してからこの時間で、強制遮断。init で 1..FORCE_OFF_MAX_MS */
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
    USB_VBUS_CTRL_ACTION_FORCE_OFF,  /* 強制遮断（DCTL.sftdiscon の直接書き込み、最後の手段はドライバの停止）。切断が成功しないとき */
} usb_vbus_ctrl_action_t;

typedef struct {
    uint32_t connects;     /* applied(CONNECT, ok = true) の回数 */
    uint32_t disconnects;  /* applied(DISCONNECT, ok = true) の回数 */
    uint32_t failures;     /* applied(CONNECT / DISCONNECT / FORCE_OFF, ok = false) の回数（全部） */
    uint32_t force_off_count;           /* applied(FORCE_OFF, ok = true) の回数（disconnects には数えない） */
    uint32_t disconnect_failure_count;  /* applied(DISCONNECT, ok = false) の回数（failures の内数） */
} usb_vbus_ctrl_stats_t;

typedef struct {
    usb_vbus_ctrl_config_t config;
    bool connected;        /* コントローラが「接続した」と思っているか */
    bool run_active;       /* 「逆の向き」のサンプルが連続している最中 */
    uint64_t run_start_ms; /* 連続の始まりの now_ms（run_active のときだけ有効） */
    uint64_t last_now_ms;  /* 直前の tick の now_ms（最初は 0） */
    bool disc_active;         /* 今の連続の中で、DISCONNECT を返したか（FORCE_OFF の条件） */
    uint64_t disc_first_ms;   /* 最初の DISCONNECT を返した now_ms（disc_active のときだけ有効） */
    bool legacy_done;      /* enabled == false で CONNECT を返したか */
    usb_vbus_ctrl_stats_t stats;
} usb_vbus_ctrl_t;

/* 既定: enabled = false、接続 500 ms、切断 1000 ms、強制遮断 1000 ms。 */
usb_vbus_ctrl_config_t usb_vbus_ctrl_config_default(void);

/* 状態を「切断」・数えなし・統計 0 にして、config を保持する（disconnect_debounce_ms は MAX_MS、force_off_after_ms は 1..FORCE_OFF_MAX_MS に切り詰める）。
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
