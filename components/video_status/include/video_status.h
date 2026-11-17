#ifndef VIDEO_STATUS_H
#define VIDEO_STATUS_H

/*
 * video_status: 映像の同期状態（UNKNOWN / NO_SIGNAL / LOCKED + 解像度）の「提供元」。純 C、IDF 非依存。
 * 契約は docs/INTERFACES.md 1.3（提供元）。
 *
 * 役割
 *   - 映像ソース（csi_video_source、test_pattern_source など）は、**自分の状態を video_status_set_provider() で
 *     登録する**（csi_video_source_get_status と同じ意味の関数を渡す）。
 *   - KVM 側（kvm_service -> kvm_ctrl -> kvm_logic）は、**video_status_get() だけを読む**。
 *     映像ソースの種類（CSI、テストパターン、将来の別ソース）を知らなくてよい。
 *
 * スレッド（重要）
 *   - **提供元の取り替え（video_status_set_provider）は、読み手のタスク（kvm_service など）を起動する前に、
 *     1 回だけ**行う。読み手はロックしない。提供元は、ctx と fn を 1 つの構造体にまとめて持ち、書き込みは
 *     起動前の 1 回の代入だけ、という規則で、「ctx と fn の組が部分的に見える」状態を作らない。
 *     起動後に取り替えてはならない（2 コアでは、組が部分的に見えうる）。
 *   - video_status_get() は、どのタスク・どのコアからも呼べる。提供元の関数は、ブロックしてはならず、
 *     どのタスクからも呼べること（csi_video_source_get_status と同じ条件）。
 *
 * 防御（video_status_get が、提供元の値を検査して直す。KVM に不整合な値を渡さない）
 *   - kvm_video_sync_state_t の範囲外の state -> UNKNOWN。
 *   - LOCKED で width または height が 0 -> UNKNOWN。
 *   - LOCKED 以外では、width = height = 0（0 でない解像度は 0 にする）。
 *   - 提供元が out を書かなかった場合は、UNKNOWN / 0 / 0（呼ぶ前に初期化する）。
 */

#include <stddef.h>
#include <stdint.h>
#include "kvm_logic.h" /* kvm_video_sync_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 映像の状態。csi_video_status_t と同じ並び。LOCKED 以外は width = height = 0 */
typedef struct {
    kvm_video_sync_state_t state;
    uint32_t width;
    uint32_t height;
} video_status_t;

/* 提供元の関数。ブロックしない。どのタスクからも呼べる。out は NULL でない */
typedef void (*video_status_get_fn)(void *ctx, video_status_t *out);

/*
 * 提供元を登録する。fn が NULL なら、既定（提供元なし）に戻す（ctx も捨てる）。
 * **読み手のタスクを起動する前に、1 回だけ**呼ぶこと（上の「スレッド」）。
 */
void video_status_set_provider(video_status_get_fn fn, void *ctx);

/*
 * 現在の映像の状態を返す。提供元がなければ UNKNOWN / 0 / 0。out が NULL でも落ちない（何もしない）。
 * 戻る値は検査済み（上の「防御」）。
 */
void video_status_get(video_status_t *out);

/*
 * kvm_board_hal_t の video_sync に使える関数（ctx は使わない）。video_status_get() の state を返す。
 * kvm_service が、HAL の video_sync をこの関数に置き換える（ボード側は video_sync を実装しなくてよい）。
 * ホストのテストも、同じ関数を HAL に差す。
 */
kvm_video_sync_state_t video_status_hal_sync(void *ctx);

/* ---- 状態の通知用の JSON（C-6b。docs/VIDEO_TRANSPORT.md 8 章、docs/WEB_API.md 5 章） ---- */

/* video_status_format_json が作る文字列の、NUL を含む最大のバイト数（128 以下と契約されている）。
 * 実際の最長は 60 バイト + NUL（数値が 10 桁ずつのとき）。 */
#define VIDEO_STATUS_JSON_MAX 128

/*
 * st を {"t":"video","state":"locked","w":1280,"h":720} の形（キーの順序と、空白なしの書式は固定）で out に書く。
 *   - state は "unknown" / "no_signal" / "locked"。範囲外の state は "unknown"。
 *   - LOCKED で w または h が 0 なら "unknown"（video_status_get と同じ補正をここでもかける）。
 *   - locked 以外は w = h = 0。
 * 戻り値は、書いた文字数（NUL を除く）。st または out が NULL、cap が足りない（文字数 + 1 未満）ときは -1。
 * -1 のとき、out != NULL かつ cap > 0 なら out[0] = '\0'（部分的な文字列を残さない）。
 */
int video_status_format_json(const video_status_t *st, char *out, size_t cap);

/* ---- 偽の提供元（テストと、デバイスのない確認用） ---- */

/* 値はそのまま保持する（検査しない）。範囲外や不整合な値も、そのまま提供元として返す
 * （video_status_get の防御を、テストが確かめるため）。メンバーは、静的初期化のために公開している。
 * 書き換え（video_status_fake_set）は、1 つのタスクからだけ。読み手と別のタスクから書くと、
 * state と解像度の組が一瞬ずれうる（偽の提供元は、そこまで守らない）。 */
typedef struct {
    video_status_t status;
} video_status_fake_t;

/* UNKNOWN / 0 / 0 にする。f が NULL なら何もしない */
void video_status_fake_init(video_status_fake_t *f);
/* 状態を設定する（そのまま保持）。f が NULL なら何もしない */
void video_status_fake_set(video_status_fake_t *f, kvm_video_sync_state_t state, uint32_t width, uint32_t height);
/* video_status_get_fn として登録できる関数。ctx は video_status_fake_t *。
 * ctx が NULL なら UNKNOWN / 0 / 0。out が NULL なら何もしない */
void video_status_fake_provider(void *ctx, video_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_STATUS_H */
