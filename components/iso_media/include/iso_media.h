#ifndef ISO_MEDIA_H
#define ISO_MEDIA_H

/*
 * iso_media: 仮想 CD-ROM ドライブの「メディアの状態」。純 C、IDF 非依存。
 * 契約は docs/ISO_MOUNT_DESIGN.md 5.5、docs/WEB_API.md 8.5 / 8.6、docs/INTERFACES.md 7.3。
 *
 * 持つ状態
 *   - EMPTY / LOADED(id)。id は、イメージの <id>（8 桁の 16 進 = uint32_t）。
 *   - prevent_removal: ターゲットが PREVENT ALLOW MEDIUM REMOVAL で掛けた取り外しの防止。
 *   - unit_attention: メディア変更（28h/00h）と、バスリセット（29h/00h）。それぞれ、報告したら消える（1 回だけ）。
 *   - event_pending: GET EVENT STATUS NOTIFICATION（メディアクラス）で 1 回だけ報告するイベント。
 *   - ejected_by_target: ターゲットの START STOP UNIT で取り外された。次の mount / eject まで残る（Web の GET で見せる）。
 *   - abort_generation: バスリセット / force_eject のたびに進む世代。進行中のコマンドは、開始時の値と比べて中止する。
 *
 * この部品は、エクステントの表への参照を持たない（放さない）。参照を放すのは mmc_scsi_command_done() だけ。
 * epoch は iso_media の外（iso_blockdev_t）が持つ。force_eject() は、「呼ばれたこと」を、コールバックで通知する。
 *
 * スレッド: 原則としてスレッドセーフではない。グルーが iso_state_mutex で包んで呼ぶ（ISO_MOUNT_DESIGN.md 4.1）。
 * **例外: abort_generation だけは、C11 の atomic（_Atomic uint32_t）**。iso_state_mutex を持たずに呼ばれる
 * mmc_scsi_read_blocks()（iso_msc_task）が iso_media_abort_generation() で読み、bus_reset / force_eject（USB のタスク、
 * SD の監視タスク）が書く。書き込み側は release、読み出し側は acquire（世代を見た側は、その前の更新を見る）。
 * 他のフィールドは、iso_state_mutex の下だけで読み書きする。
 * コールバックの中から iso_media を呼んではならない。コールバックでビューを公開する形にはせず、
 * 呼び出し側が iso_media_get_status() で状態を取る。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ISO_MEDIA_EVENT_NONE = 0,
    ISO_MEDIA_EVENT_EJECT_REQUEST, /* 取り外し要求（iso_media_request_eject()） */
    ISO_MEDIA_EVENT_NEW_MEDIA,     /* メディアが入った（mount） */
    ISO_MEDIA_EVENT_MEDIA_REMOVAL  /* メディアが取り外された（eject、ターゲットの取り外し、force_eject） */
} iso_media_event_t;

typedef enum {
    ISO_MEDIA_MOUNTED = 0,    /* mount: マウントした（差し替えを含む） */
    ISO_MEDIA_ALREADY_MOUNTED,/* mount: すでに同じイメージ。何も変えない */
    ISO_MEDIA_EJECTED,        /* eject / target_eject: 取り外した */
    ISO_MEDIA_NOT_MOUNTED,    /* eject: EMPTY だった（冪等） / target_eject: EMPTY だった */
    ISO_MEDIA_LOCKED,         /* prevent_removal のため拒否（force なし）。locked_by_target */
    ISO_MEDIA_INVALID         /* 引数が不正（NULL） */
} iso_media_result_t;

/* force_eject() が呼ばれたことの通知（epoch を進めるのは、グルーがこれを受けて行う）。常に 1 回呼ばれる（EMPTY のときも）。 */
typedef void (*iso_media_force_eject_cb_t)(void *ctx);

typedef struct {
    bool loaded;
    uint32_t id; /* loaded のときだけ意味がある */
    bool prevent_removal;
    bool unit_attention_media; /* 28h/00h */
    bool unit_attention_reset; /* 29h/00h */
    iso_media_event_t event_pending;
    bool ejected_by_target;
    _Atomic uint32_t abort_generation; /* 唯一、ロックなしで読まれる（上の「スレッド」） */
    iso_media_force_eject_cb_t on_force_eject;
    void *on_force_eject_ctx;
} iso_media_t;

/* Web の GET 用のビュー（呼び出し側が取る。locked は WEB_API.md 8.1 の target.locked = loaded && prevent_removal）。 */
typedef struct {
    bool mounted;
    uint32_t id;
    bool locked;
    bool ejected_by_target;
} iso_media_status_t;

/* 初期化。EMPTY、フラグはすべて偽、世代は 0。on_force_eject は NULL でよい。 */
void iso_media_init(iso_media_t *m, iso_media_force_eject_cb_t on_force_eject, void *ctx);

/* ---- Web の操作（WEB_API.md 8.5 / 8.6）---- */

/* mount: EMPTY -> LOADED、または LOADED -> 別の LOADED（差し替え）。
 *   - 同じ id がマウント中なら ISO_MEDIA_ALREADY_MOUNTED（何も変えない。force があっても）。
 *   - 別の id がマウント中で prevent_removal が立っていれば、force なしは ISO_MEDIA_LOCKED。force ありは差し替える。
 *   - 成功すると: unit_attention_media = 真、event = NEW_MEDIA、prevent_removal = 偽、ejected_by_target = 偽。 */
iso_media_result_t iso_media_mount(iso_media_t *m, uint32_t id, bool force);

/* eject: LOADED -> EMPTY。EMPTY なら ISO_MEDIA_NOT_MOUNTED（ejected_by_target を偽に戻す。冪等）。
 *   prevent_removal が立っていれば force なしは ISO_MEDIA_LOCKED。取り外すと event = MEDIA_REMOVAL、prevent_removal = 偽、
 *   ejected_by_target = 偽。unit_attention は立てない（次のコマンドは NOT READY / メディアなし）。 */
iso_media_result_t iso_media_eject(iso_media_t *m, bool force);

/* ---- ターゲットの操作 ---- */

/* START STOP UNIT（イジェクト）: prevent_removal が掛かっていれば ISO_MEDIA_LOCKED（取り外さない）。EMPTY なら
 * ISO_MEDIA_NOT_MOUNTED（何も変えない）。取り外すと event = MEDIA_REMOVAL、ejected_by_target = 真。 */
iso_media_result_t iso_media_target_eject(iso_media_t *m);

/* PREVENT ALLOW MEDIUM REMOVAL。EMPTY のときに prevent = 真は、偽を返して何も変えない（メディアがないものは防止できない）。
 * それ以外は真を返す。 */
bool iso_media_set_prevent(iso_media_t *m, bool prevent);

/* 取り外し要求のイベントを立てる（LOADED のとき。NEW_MEDIA / MEDIA_REMOVAL が報告待ちなら上書きしない）。立てたら真。 */
bool iso_media_request_eject(iso_media_t *m);

/* ---- グルーが呼ぶ（バスの事象、SD の再初期化）---- */

/* USB のバスリセット・切断: prevent_removal を解除し、unit_attention_reset（29h/00h）を立て、abort_generation を進める。
 * 参照は放さない。LOADED / EMPTY は変えない。 */
void iso_media_bus_reset(iso_media_t *m);

/* SD の再初期化: prevent_removal を無視して EMPTY にする。prevent_removal = 偽、abort_generation を進める。
 * LOADED だったときは、unit_attention_media = 真、event = MEDIA_REMOVAL（EMPTY だったときは立てない）。
 * ejected_by_target は変えない。最後に on_force_eject を 1 回呼ぶ。参照は放さない。 */
void iso_media_force_eject(iso_media_t *m);

/* ---- 読み出し（mmc_scsi と、Web のビュー）---- */

void iso_media_get_status(const iso_media_t *m, iso_media_status_t *out);
bool iso_media_is_loaded(const iso_media_t *m);
bool iso_media_get_id(const iso_media_t *m, uint32_t *id); /* LOADED なら真 */
bool iso_media_prevent_removal(const iso_media_t *m);
uint32_t iso_media_abort_generation(const iso_media_t *m);

/* UNIT ATTENTION を 1 つ取り出して消す。バスリセット（29h/00h）が先、メディア変更（28h/00h）が後。なければ偽。 */
bool iso_media_take_unit_attention(iso_media_t *m, uint8_t *asc, uint8_t *ascq);
bool iso_media_unit_attention_pending(const iso_media_t *m);

/* 報告待ちのイベントを見る / 取り出して消す。 */
iso_media_event_t iso_media_peek_event(const iso_media_t *m);
iso_media_event_t iso_media_take_event(iso_media_t *m);

#ifdef __cplusplus
}
#endif

#endif /* ISO_MEDIA_H */
