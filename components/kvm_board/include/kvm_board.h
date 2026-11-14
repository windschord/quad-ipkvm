#ifndef KVM_BOARD_H
#define KVM_BOARD_H

/*
 * kvm_board: KVM 切替・ATX のボード HAL（ハードウェア抽象）。docs/INTERFACES.md 6.2 が契約。
 *
 * このヘッダーは「型」だけを定義する。GPIO の実装（極性、番号、ハードウェアタイマーによる自動解除）は、
 * 実機の配線が決まってから A-9（Antigravity）が書く。ホストでは test/host/kvm_board/ のモック HAL で検証する。
 *
 * HAL は論理値だけを話す。GPIO の極性・番号・フォトカプラの極性は、ボード実装の中に閉じる
 * （TODO(hw-verify): 値・極性・GPIO 番号は docs/HW_VERIFY.md。推測で確定しない）。
 */

#include <stdint.h>
#include <stdbool.h>
#include "kvm_logic.h" /* kvm_video_sync_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* POWER と POWER_FORCE は同じ Ē（同じ物理線）を使うが、上限が違う（短押し / 長押し）。
 * ボードは種別ごとに別の上限を強制する。 */
typedef enum {
    KVM_BOARD_LINE_POWER = 0,
    KVM_BOARD_LINE_POWER_FORCE,
    KVM_BOARD_LINE_RESET,
    KVM_BOARD_LINE_KVM,
    KVM_BOARD_LINE_COUNT
} kvm_board_line_t;

typedef struct {
    void *ctx;

    /* A0/A1 を設定する（channel 1..4）。全 Ē が High（未選択）のときだけ呼ばれる。
     * ボードは、どれかの線が有効なら失敗(< 0)にする（不変条件 4 の最後の砦）。戻り値 < 0 は失敗。 */
    int (*set_addr)(void *ctx, uint8_t channel);

    /* 線を有効（Ē を Low）にする。max_ms 後に、ボードのハードウェアタイマーが自動で解除する
     * （ソフトのハングへの防御）。max_ms は、種別ごとのボード側のハード上限
     * （KVM_BOARD_MAX_POWER_MS / _POWER_FORCE_MS / _RESET_MS / _KVM_MS。TODO(hw-verify)、A-9）でクランプされる。
     * 0 や上限超えは、その種別の上限に丸める。物理線が同じ POWER と POWER_FORCE も、別々の上限を強制する。
     * 同時に有効にできる線は 1 本まで（2 本目は失敗）。失敗は < 0。 */
    int (*assert_line)(void *ctx, kvm_board_line_t line, uint32_t max_ms);

    /* 線を無効（Ē を High）にする。冪等。呼び出しは常に成功しなければならない
     * （失敗するなら、ボードのフォールトとして上げる。呼び出し側は < 0 をフォールトとして扱う）。 */
    int (*release_line)(void *ctx, kvm_board_line_t line);

    /* 全線を無効にする（緊急。フォールト時、起動時、終了時）。 */
    int (*release_all)(void *ctx);

    /* KVM の LED の論理値（ビット 0..3 = ch1..4 が点灯）。フォトカプラの極性はボード側で吸収。失敗は < 0。 */
    int (*read_led_mask)(void *ctx, uint8_t *mask);

    /* 映像の同期状態（映像側の状態 INTERFACES.md 1.3 を結線する）。
     * HAL を実装しない環境では KVM_VIDEO_SYNC_UNKNOWN を返してよい（関数ポインタ自体は必須）。 */
    kvm_video_sync_state_t (*video_sync)(void *ctx);
} kvm_board_hal_t;

#ifdef __cplusplus
}
#endif

#endif /* KVM_BOARD_H */
