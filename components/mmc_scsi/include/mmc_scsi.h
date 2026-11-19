#ifndef MMC_SCSI_H
#define MMC_SCSI_H

/*
 * mmc_scsi: 仮想 CD-ROM（デバイス種別 5）の SCSI コマンド（MMC / SPC）。純 C、IDF 非依存。
 * 契約は docs/ISO_MOUNT_DESIGN.md 5.3 / 5.4 / 5.5、docs/INTERFACES.md 7.3。テストと出典は docs/MMC_SCSI_TESTS.md。
 *
 * 役割
 *   - USB の MSC の層は、受け取った「全ての」CDB を mmc_scsi_execute() に渡し、結果（データ、センス、ステータス）を
 *     そのまま返す（標準の TinyUSB の MSC は契約を満たさない。設計 5.1）。
 *   - READ(10) / READ(12) は、execute ではデータを作らない。result.data_len = 0 で、result.read に範囲が入る。
 *     データは、MSC の転送の大きさごとに mmc_scsi_read_blocks() で読む（先読みしない）。
 *   - READ のとき execute は、バックエンド（エクステントの表）への参照を 1 つ取る。参照を放すのは
 *     mmc_scsi_command_done() だけ（冪等。BOT 層が、正常終了・STALL・リセット・切断のどの場合も必ず呼ぶ）。
 *     iso_media_bus_reset() / iso_media_force_eject() は参照を放さない。
 *   - メディアの有無と変更の通知は iso_media が持つ。
 *
 * ロックとスレッド（設計 4.1）
 *   純 C の部品は、原則としてスレッドセーフではない。グルーは、mmc_scsi_execute() と mmc_scsi_command_done() と iso_media_* を
 *   iso_state_mutex で包む。mmc_scsi_read_blocks()（SD を読む。ブロックする）は、ロックなしで呼ぶ（参照を保持している間）。
 *   1 つの mmc_scsi_t に対するコマンドは、同時に 1 つ（LUN は 1 つ、コマンドは 1 つずつ）。
 *   mmc_scsi_execute() と mmc_scsi_read_blocks() は、同じタスク（iso_msc_task）が順に呼ぶ前提（センスなどの状態は、
 *   この 2 つだけが書く）。
 *
 *   **別のタスクと並行して動くのは、次の 2 つだけで、どちらも C11 の atomic（stdatomic.h）で同期する**:
 *     (1) iso_media_abort_generation()（bus_reset / force_eject が進め、read_blocks が読む。iso_media.h）
 *     (2) 参照の状態 ref_state（IDLE / READING / READING_RELEASE_PENDING / RELEASING）。
 *         BOT 層はリセット / 切断のとき、iso_msc_task の完了を待たずに mmc_scsi_command_done() を呼ぶことがある
 *         （防御。通常は INTERFACES.md 7.3 の契約どおり、完了を待ってから呼ぶ）。**read_blocks の実行中に command_done が
 *         呼ばれたときは、参照の解放を遅らせ、read_blocks が復帰する直前に、ちょうど 1 回だけ解放する**
 *         （read_blocks が使っている handle と size_bytes を、実行中に書き換えず、backend.read の最中に backend.release を
 *         呼ばない）。command_done は、その場合でも、すぐに復帰する（冪等）。
 *   **このため、backend.release() は、どのタスクからも呼ばれうる**（command_done を呼んだタスク、または read_blocks を
 *   呼んだタスク（遅延した解放））。backend 側（iso_extent の参照カウント）が、スレッドセーフであること。
 *   read_blocks の実行中に、次の mmc_scsi_execute() が呼ばれる（契約違反）と、READ は参照を取らず、ABORTED COMMAND で失敗する。
 *
 * 応答の長さ
 *   各応答のデータの長さ（TOC データ長など）の欄は、割り当て長で切り詰める前の「全体の長さ」を示す。
 *   data_len は、割り当て長と out_cap で切り詰めた後の、out に書いたバイト数。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iso_media.h"
#include "iso_seams.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MMC_SCSI_BLOCK_SIZE 2048u /* CD の 1 ブロック（論理ブロック長） */
#define MMC_SCSI_SENSE_LEN 18u    /* REQUEST SENSE の固定形式の長さ */
#define MMC_SCSI_CDB_MAX 16u

/* ---- 結果 ---- */
typedef enum {
    MMC_STATUS_GOOD = 0x00,            /* SAM のステータス: GOOD */
    MMC_STATUS_CHECK_CONDITION = 0x02  /* SAM のステータス: CHECK CONDITION */
} mmc_status_t;

typedef enum { MMC_DIR_NONE = 0, MMC_DIR_IN, MMC_DIR_OUT } mmc_dir_t;

/* センスキー（SPC） */
enum {
    MMC_SENSE_NO_SENSE = 0x0,
    MMC_SENSE_NOT_READY = 0x2,
    MMC_SENSE_MEDIUM_ERROR = 0x3,
    MMC_SENSE_ILLEGAL_REQUEST = 0x5,
    MMC_SENSE_UNIT_ATTENTION = 0x6,
    MMC_SENSE_DATA_PROTECT = 0x7,
    MMC_SENSE_ABORTED_COMMAND = 0xB
};

typedef struct {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
} mmc_sense_t;

typedef struct {
    mmc_status_t status;
    mmc_sense_t sense; /* status が CHECK CONDITION のときだけ意味がある（GOOD のときは 0） */
    uint32_t data_len; /* out に書いたバイト数。READ(10)/(12) のときは 0 */
    struct {
        bool is_read;
        uint32_t lba;    /* CD のブロック（2048 バイト）単位 */
        uint32_t blocks; /* 0 のこともある（転送なし。GOOD） */
    } read;
} mmc_result_t;

/* ---- 読み出しのバックエンド（iso_extent と iso_blockdev_t を、後から親が結線する）----
 * 全ての関数は、グルーが iso_state_mutex の下で呼ぶもの（size_bytes / acquire / release）と、ロックなしで呼ぶもの（read）。
 * media_id は、iso_media の LOADED(id) の id。 */
typedef struct {
    void *ctx;
    /* イメージの実バイト数（size_bytes）。0 = 成功、負 = iso_err_t（メディアなし扱い）。参照は取らない。
     * マウント中のイメージは変更できない（409 mounted）ので、同じ id では、値は変わらない。 */
    int (*size_bytes)(void *ctx, uint32_t media_id, uint64_t *out);
    /* 参照の取得。成功（0）のとき *handle に、参照の識別子（NULL でもよい）を入れる。release が必ず対になる。
     * 負 = iso_err_t（ISO_ERR_NOT_FOUND / ISO_ERR_NO_CARD = 表がない。メディアなし扱い。それ以外 = MEDIUM ERROR）。 */
    int (*acquire)(void *ctx, uint32_t media_id, void **handle);
    /* handle の表で、CD のブロック lba から blocks 個を buf（blocks * 2048 バイト）に読む。ブロックする。
     * 0 = 成功、負 = iso_err_t（ISO_ERR_EPOCH、ISO_ERR_IO など。どれも MEDIUM ERROR になる）。
     * size_bytes 以降のバイトの内容は保証しなくてよい（ゼロ埋めは mmc_scsi_read_blocks が行う）。ただし、
     * capacity_blocks 以内のブロックの読み出しを、EOF を理由に失敗させてはならない。 */
    int (*read)(void *ctx, void *handle, uint32_t lba, uint32_t blocks, uint8_t *buf);
    /* 参照を放す（acquire が成功したものだけ）。 */
    void (*release)(void *ctx, void *handle);
} mmc_scsi_backend_t;

/* ---- コンテキスト ---- */
typedef struct {
    iso_media_t *media;
    mmc_scsi_backend_t backend;

    /* 直近のセンス（REQUEST SENSE で返す）。INQUIRY と REQUEST SENSE 以外の次のコマンドの開始で消える。 */
    bool sense_valid;
    mmc_sense_t sense;
    uint32_t sense_generation;      /* センスを記録したときの iso_media の世代。その後に世代が進んだら捨てる */

    /* execute が READ のために取った参照（command_done が放す）。held / held_* は、ref_state が IDLE のとき、execute と
     * command_done だけが書く。READING の間は、read_blocks だけが読む（command_done は書き換えない） */
    bool held;
    void *held_handle;
    uint64_t held_size_bytes;
    uint32_t held_abort_generation; /* 取得時の iso_media の世代。変われば中止 */
    atomic_int ref_state;           /* MMC_REF_*（read_blocks と command_done の同期） */
} mmc_scsi_t;

/* ref_state の値 */
enum {
    MMC_REF_IDLE = 0,                    /* 誰も held を使っていない */
    MMC_REF_READING = 1,                 /* read_blocks が held を使っている */
    MMC_REF_READING_RELEASE_PENDING = 2, /* read_blocks の実行中に command_done が来た。復帰の直前に解放する */
    MMC_REF_RELEASING = 3                /* command_done または遅延した解放が、解放している */
};

/* 初期化。media と、backend の 4 つの関数が非 NULL でなければ -1。 */
int mmc_scsi_init(mmc_scsi_t *s, iso_media_t *media, const mmc_scsi_backend_t *backend);

/* 1 つの CDB を実行する。
 *   cdb_len: CBW の bCBWCBLength（1..16）。コマンドに必要な長さ以上を受け付ける（足りなければ ILLEGAL REQUEST 24h/00h）。
 *   dir / expected_len: CBW の方向と dCBWDataTransferLength。13 のケースの処理は msc_bot が行う。
 *     ここで使うのは 1 か所だけ: **GET EVENT STATUS NOTIFICATION は、ホストに全体（8 バイト）を渡せたと見なせるとき
 *     （dir == IN、expected_len >= 8、out_cap >= 8、割り当て長 >= 8）だけ、報告待ちのイベントを消費する**
 *     （そうでないと、ホストが受け取らなかったイベントが消える）。msc_bot は、expected_len に CBW の値を渡すこと。
 *     （OUT のデータを受け取るコマンド = WRITE 系と MODE SELECT は、データを受け取らず、失敗にする）。
 *   out / out_cap: IN の応答の置き場所。応答は out_cap で切り詰める（あふれない）。
 * 戻り値: 0 = result を埋めた。-1 = 引数が不正（s / cdb / result が NULL。result は触らない）。 */
int mmc_scsi_execute(mmc_scsi_t *s, const uint8_t *cdb, size_t cdb_len, mmc_dir_t dir, uint32_t expected_len,
                     uint8_t *out, size_t out_cap, mmc_result_t *result);

/* READ の 1 回分の転送を読む。execute が取った参照を使う（参照がなければ NOT READY）。
 *   lba / blocks: CD のブロック単位。buf: blocks * 2048 バイト以上（cap で渡す）。
 *   size_bytes 以降のバイトは 0 にする（ゼロ埋めはここ）。
 *   バスリセット / force_eject で世代が変わっていれば、バックエンドの戻り値の種類（ISO_ERR_EPOCH を含む）によらず、
 *   ABORTED COMMAND 0Bh/00h/00h（読み出しの前と後の両方で確認する）。世代が変わっていない、バックエンドの失敗
 *   （ISO_ERR_EPOCH、ISO_ERR_IO など）は MEDIUM ERROR 03h/11h/00h。失敗のとき buf の読み出し範囲は 0 で埋める
 *   （古い内容を残さない）。実行中に command_done が呼ばれたら、参照の解放は、この関数の復帰の直前に行う（上の「ロックとスレッド」）。
 * 戻り値: GOOD または CHECK CONDITION（センスは mmc_scsi_last_sense() と REQUEST SENSE で取れる）。 */
mmc_status_t mmc_scsi_read_blocks(mmc_scsi_t *s, uint32_t lba, uint32_t blocks, uint8_t *buf, size_t cap);

/* コマンドの終わり（正常、STALL、リセット、切断のどれでも）に、必ず呼ぶ。execute が取った参照を放す。冪等。
 * read_blocks の実行中に呼ばれたときは、解放を遅らせる（上の「ロックとスレッド」）。 */
void mmc_scsi_command_done(mmc_scsi_t *s);

/* 参照を保持しているか（テスト・診断用。遅延した解放の待ちの間は、真）。ロックなしで他のタスクから呼ばない。 */
bool mmc_scsi_holds_reference(const mmc_scsi_t *s);

/* 直近のセンスを取る。保持していれば真。 */
bool mmc_scsi_last_sense(const mmc_scsi_t *s, mmc_sense_t *out);

/* 固定形式（応答コード 70h）のセンスデータ 18 バイトを作る。 */
void mmc_scsi_build_sense(const mmc_sense_t *sense, uint8_t out[MMC_SCSI_SENSE_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* MMC_SCSI_H */
