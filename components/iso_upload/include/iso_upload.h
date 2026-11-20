#ifndef ISO_UPLOAD_H
#define ISO_UPLOAD_H

/*
 * iso_upload: ISO のアップロードの受信の状態機械。純 C、IDF 非依存。I/O・時刻・SHA-256・メモリの確保は、iso_seams.h の
 * 関数ポインタ(と iso_allocator_t)で注入する。
 * 契約: docs/ISO_MOUNT_DESIGN.md 3.1(パーツと書き込み)、3.4(状態、received)、4.2(ワーカー、空き容量)、6(アップロードの方式)、
 *       docs/WEB_API.md 8(判定の順序)、docs/INTERFACES.md 7.4 / 7.5。
 *
 * 構成
 *   iso_upload.c  begin、write_chunk(パーツの境界での分割、sync、SHA-256 の逐次、書き込み失敗の truncate)、アイドルの close、
 *                 中止、finish の受付(状態の遷移)、空き容量
 *   iso_finish.c  FINISH ジョブのステップ(sync + close、SHA-256 の再計算 = iso_upload_hash_step、宣言値との比較、署名、エクステント、
 *                 meta を ready で書く)
 *   iso_index.c   INDEX のステップ(iso_index_step。FINISH と INDEX ジョブが共有)
 *
 * スレッド: スレッドセーフではない。呼び出し側(グルー)が iso_op_mutex で守る(設計 4.1)。スレッドを作らない。イベント・時間・ロックを待たない。ただし、シームの I/O(`write` / `sync` / `read` / `truncate` など)が終わるまでは、呼び出し側が待つ(長い処理は、ステップに分けてある)。
 * ビューの更新: 操作の最後に hooks.view_changed を呼ぶ(グルーが iso_state_mutex の下で、公開ビューを更新する。短く、I/O なし)。
 *
 * 戻り値の規則(iso_catalog_apply と同じ): 0 = 受理、正 = iso_res_t(拒否・結果。API のステータスの元)、負 = iso_err_t(I/O などのエラー)。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iso_seams.h"
#include "iso_extent.h"
#include "iso_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ISO_UPLOAD_CHUNK_KB
#define ISO_UPLOAD_CHUNK_KB 128u /* Kconfig ISO_UPLOAD_CHUNK_KB (16〜1024) */
#endif
#ifndef ISO_SYNC_KIB
#define ISO_SYNC_KIB 4096u /* Kconfig ISO_SYNC_KIB */
#endif
#ifndef ISO_FREE_RESERVE_MIB
#define ISO_FREE_RESERVE_MIB 64u /* Kconfig ISO_FREE_RESERVE_MIB */
#endif
#ifndef ISO_UPLOAD_IDLE_S
#define ISO_UPLOAD_IDLE_S 300u /* Kconfig ISO_UPLOAD_IDLE_S */
#endif
#ifndef ISO_UPLOAD_ACTIVE_S
#define ISO_UPLOAD_ACTIVE_S 5u /* 固定: GET の upload.state の receiving / idle の境 */
#endif
#ifndef ISO_WORKER_BUDGET_KIB
#define ISO_WORKER_BUDGET_KIB 1024u /* Kconfig ISO_WORKER_BUDGET_KIB */
#endif

/* ---- 結果 -------------------------------------------------------------------- */

typedef enum {
    ISO_RES_OK = 0,
    ISO_RES_JOINED,              /* (iso_core) 同じイメージのジョブに合流した。受理 */
    ISO_RES_ALREADY_READY,       /* finish: すでに ready (200。冪等) */
    ISO_RES_ALREADY_FINALIZING,  /* finish: すでに finalizing (202。冪等) */
    ISO_RES_NOT_FOUND,           /* 404 not_found (deleting を含む) */
    ISO_RES_NOT_UPLOADING,       /* 409 not_uploading (ready / invalid / finalizing へ PUT) */
    ISO_RES_UPLOAD_IN_PROGRESS,  /* 409 upload_in_progress (枠が使用中) */
    ISO_RES_OFFSET_MISMATCH,     /* 409 offset_mismatch (out->received が現在の値。何も書かない) */
    ISO_RES_CHUNK_TOO_LARGE,     /* 413 payload_too_large */
    ISO_RES_EXCEEDS_SIZE,        /* 400 invalid_request (チャンクが size を超える) */
    ISO_RES_INSUFFICIENT_STORAGE,/* 507 insufficient_storage (空き容量、または書き込み中の空きなし。後者は取り消し済みで incomplete) */
    ISO_RES_SIZE_MISMATCH,       /* finish: 409 size_mismatch (received < size) */
    ISO_RES_INVALID_IMAGE,       /* finish: 422 (invalid_reason は iso_catalog_entry_t) */
    ISO_RES_NAME_EXISTS,         /* begin: 409 name_exists */
    ISO_RES_TOO_MANY_IMAGES,     /* begin: 409 too_many_images */
    ISO_RES_MOUNTED              /* (iso_core) delete: 409 mounted */
} iso_res_t;

/* 取り消せなかった truncate の保留の最大数(イメージ数 × 1 イメージのパーツ数。溢れない) */
#define ISO_PEND_MAX (ISO_MAX_IMAGES * ISO_MAX_PARTS)

/* ---- フック(グルーが実装する) ---------------------------------------------------- */

typedef struct {
    void *ctx;
    /* 公開ビューの更新を促す(NULL でよい)。iso_state_mutex を取るのはグルーの実装の中。この中から iso_upload を呼ばない。 */
    void (*view_changed)(void *ctx);
    /* エクステントの表を引き渡す(NULL なら、受け取らず、すぐ unref する)。**所有権(参照 1)が移る**。
     * 同じ id の以前の表があれば、受け取った側が unref する(ここでは触らない)。 */
    void (*table_ready)(void *ctx, const char *id, iso_extent_table_t *table);
    /* チャンクを受理した(NULL でよい)。iso_core が、待っている IDLE_CLOSE を取り消す(積んだ後、実行の前に PUT が成功したとき、閉じないため)。
     * この中から iso_upload を呼ばない。 */
    void (*chunk_accepted)(void *ctx, const char *id);
} iso_upload_hooks_t;

typedef struct {
    iso_catalog_t *cat;               /* カタログ(fs と乱数を持つ) */
    const iso_blockdev_t *bdev;       /* epoch の供給元(エクステントの表に記録する)。NULL なら 0 */
    const iso_clock_t *clock;
    const iso_sha256_t *sha;
    void *sha_state;                  /* sha->state_size バイト。呼び出し側が確保する */
    uint8_t *io_buf;                  /* SHA-256 の再計算の読み出しバッファ */
    size_t io_buf_size;               /* 1 以上 */
    const iso_allocator_t *alloc;     /* エクステントの作業領域と表(PSRAM) */
    uint32_t chunk_bytes;             /* 0 = ISO_UPLOAD_CHUNK_KB * 1024 */
    uint32_t sync_bytes;              /* 0 = ISO_SYNC_KIB * 1024 */
    iso_upload_hooks_t hooks;
} iso_upload_cfg_t;

typedef struct iso_upload {
    iso_upload_cfg_t cfg;
    /* アップロードの枠: uploading のイメージと、FINISH ジョブ中の finalizing のイメージが使う(INDEX は使わない) */
    bool slot_active;
    bool slot_finalizing;
    char slot_id[ISO_ID_LEN + 1];
    uint64_t last_put_ms;             /* 直近の begin / PUT の時刻(アイドルと receiving / idle の判定) */
    /* 書き込み用のハンドル(現在のパーツ) */
    iso_file_t *f;
    uint64_t since_sync;              /* 最後の sync からの書き込みのバイト数 */
    /* SHA-256 の逐次の状態(メモリ上にだけある。設計 3.4)。有効 = 先頭から sha_bytes バイトを、sha_id のイメージについて計算済み */
    bool sha_valid;
    char sha_id[ISO_ID_LEN + 1];
    uint64_t sha_bytes;
    /* 空き容量(キャッシュ値。更新はマウント時とワーカーだけ) */
    bool free_known;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t consumed_since;          /* キャッシュの採取後に、アップロードが書いたバイト数(取り消せなかった分は、書こうとした分の上限を足す) */
    /* 取り消せなかった truncate(設計 3.1)。次の再開・finish の受付の最初に、もう一度行う(メモリ上だけ。再起動すると、ディスクの大きさから決める)。
     * 全イメージで共有する。**捨てない**: 1 つのイメージの保留は、1 つのチャンクが触るパーツの数(ISO_MAX_PARTS 以下)で、そのイメージの次の再開が先に片づける
     * (片づけられなければ、書かずに断る)ので、イメージごとに ISO_MAX_PARTS 件以下。イメージは ISO_MAX_IMAGES 個以下なので、ISO_PEND_MAX 件で足りる
     * (1 件 24 バイト。全体で 6144 バイト) */
    struct {
        char id[ISO_ID_LEN + 1];
        uint32_t part;
        uint64_t size;
    } pend[ISO_PEND_MAX];
    size_t pend_n;
} iso_upload_t;

/* cfg を検査してコピーする。NULL / io_buf_size 0 / sha_state が必要なのに NULL などは ISO_ERR_INVALID。 */
int iso_upload_init(iso_upload_t *u, const iso_upload_cfg_t *cfg);

/* ---- 空き容量のキャッシュ ----------------------------------------------------------- */

/* 採取した値で、キャッシュを置き換える(consumed_since を 0 に戻す)。マウント時と、ワーカーの FINISH / DELETE の完了後。 */
void iso_upload_set_free(iso_upload_t *u, uint64_t total_bytes, uint64_t free_bytes);
/* fs->mount_info を呼んで iso_upload_set_free する(ワーカーの文脈)。失敗したら、キャッシュは変えず、エラーを返す。 */
int iso_upload_refresh_free(iso_upload_t *u);
/* 507 の応答の free_bytes: キャッシュ値 − consumed_since − 途中のイメージの (size_bytes − received)(0 未満は 0)。未採取は 0。 */
uint64_t iso_upload_free_estimate(const iso_upload_t *u);

/* ---- begin、PUT ----------------------------------------------------------------- */

/*
 * アップロードの開始(WEB_API 8.2)。検査の順(設計 / WEB_API の判定の順序):
 *   名前の規則・size の範囲(ISO_ERR_INVALID。400)→ 枠(ISO_RES_UPLOAD_IN_PROGRESS)→ 同名(ISO_RES_NAME_EXISTS)→ 上限(ISO_RES_TOO_MANY_IMAGES)
 *   → 空き容量(ISO_RES_INSUFFICIENT_STORAGE。size が total_bytes 超、または 空き − 途中のイメージの残り < size + ISO_FREE_RESERVE_MIB。
 *      キャッシュを採取していなければ、空きなしとして拒否する)→ iso_catalog_begin(mkdir + meta.a。負 = I/O のエラー)。
 * 成功すると、枠を取り(uploading)、SHA-256 を初期化する。ファイル(p000)は、最初の PUT で作る。
 */
int iso_upload_begin(iso_upload_t *u, const char *name, size_t name_len, uint64_t size_bytes, const uint8_t *sha256,
                     char out_id[ISO_ID_LEN + 1]);

typedef struct {
    uint64_t received; /* 受理したら、受信済みのバイト数。ISO_RES_OFFSET_MISMATCH のときは、現在の値 */
} iso_upload_write_t;

/*
 * チャンクの書き込み(WEB_API 8.3 の判定の 5〜13 を、受信の後で、もう一度行う)。**offset == received のときだけ**書く。
 * 検査の順: data == NULL / len == 0(ISO_ERR_INVALID)→ len > chunk_bytes(ISO_RES_CHUNK_TOO_LARGE)→ イメージがない・deleting(ISO_RES_NOT_FOUND)
 *   → 状態(uploading 以外で、incomplete でもない: ISO_RES_NOT_UPLOADING。incomplete で枠が使用中: ISO_RES_UPLOAD_IN_PROGRESS)
 *   → offset != received(ISO_RES_OFFSET_MISMATCH)→ offset + len > size(ISO_RES_EXCEEDS_SIZE)
 *   → 再開(incomplete)のとき: 残りの大きさ + 予備の空き(ISO_RES_INSUFFICIENT_STORAGE)→ 書き込み。
 * 再開(incomplete → uploading)は meta を書かない(C-10a の決定)。ファイルを開いていない(再開、アイドルの後)ときは、
 *   received を、ディスク上のパーツの大きさから決め直す(設計 3.4)。
 * 書き込み: チャンク全体を、パーツの境界で分割して追記する(パーツを先頭から。次のパーツは新規作成)。sync は、
 *   sync_bytes ごと、パーツの終わり。SHA-256 は、全体の書き込みが成功した後に更新する。
 *   **書き込み・sync の失敗**: ファイルを閉じ、触ったパーツを書く前の大きさに truncate して(チャンク全体を取り消す)、
 *   incomplete にし、枠を空ける。**書き込みと truncate の両方が失敗しても、received は書く前の値のまま**(増やさない。ディスクには、未受理のバイトが残りうる)。
 *   取り消せなかったパーツは、メモリに保留し、**次の再開の PUT と finish の受付が、先に truncate をやり直す**。保留が複数あり、先の truncate が成功して後の truncate が失敗したときは、
 *   成功した分の truncate と保留の削除は、そのまま維持し、残りを次の試行に持ち越す。そのエラーを返し、新しい PUT や FINISH の受付には進まない(received も、状態も、変えない)。
 *   再起動すると保留は失われ、再開のときの received は、ディスク上のパーツの大きさから決め直す(設計 3.1、3.4)。
 *   戻り値は、ISO_ERR_NO_SPACE なら ISO_RES_INSUFFICIENT_STORAGE(507 相当)、それ以外は元のエラー(負。API は 503 sd_unavailable)。
 * out は NULL でもよい。
 */
int iso_upload_write_chunk(iso_upload_t *u, const char *id, uint64_t offset, const void *data, size_t len, iso_upload_write_t *out);

/* ---- 状態の問い合わせ(iso_api_put_precheck が使う。ビューではなく、カタログを直接見る版) ------------- */

typedef struct {
    bool found;               /* deleting は false */
    iso_state_t state;
    uint64_t size_bytes;      /* meta_valid のときだけ */
    uint64_t received;
    bool meta_valid;
    bool slot_free;           /* 枠が空いている(incomplete の再開と finish に必要) */
    bool slot_owner;          /* このイメージが枠を使っている */
    uint32_t chunk_bytes;
} iso_upload_info_t;

void iso_upload_info(const iso_upload_t *u, const char *id, iso_upload_info_t *out);
bool iso_upload_slot_busy(const iso_upload_t *u);
/* 枠を使っているイメージの id(なければ NULL)。 */
const char *iso_upload_slot_id(const iso_upload_t *u);
uint32_t iso_upload_chunk_bytes(const iso_upload_t *u);

/* ---- アイドル、中止、finish の受付 ---------------------------------------------------- */

/*
 * IDLE_CLOSE ジョブ(1 ステップ): uploading で枠を持つイメージの、ファイルを sync + close し、meta を incomplete で書く(IDLE_TIMEOUT)。
 * 枠を空ける。**SHA-256 の状態は保つ**。対象が uploading でない(finish が先に来た、すでに incomplete など)なら、何もしない(OK)。
 * 失敗しても(sync / close / meta の書き込み)、メモリ上は incomplete にして枠を空け(meta は uploading のまま = 再起動で incomplete)、
 * 最初のエラーを返す(received は、ディスクの大きさから決め直す)。
 */
int iso_upload_idle_close(iso_upload_t *u, const char *id);

/* 枠を持つイメージの中止(DELETE。設計 6.1): ファイルを閉じ、SHA-256 の状態を捨て、枠を空ける。他のイメージには何もしない。 */
void iso_upload_abort(iso_upload_t *u, const char *id);

/*
 * finish の受付(WEB_API 8.4。FINISH ジョブを積む前に呼ぶ)。
 *   ISO_RES_NOT_FOUND / ISO_RES_ALREADY_READY / ISO_RES_ALREADY_FINALIZING / ISO_RES_INVALID_IMAGE(422)/ ISO_RES_SIZE_MISMATCH(out_received)/
 *   ISO_RES_UPLOAD_IN_PROGRESS(incomplete で別のイメージが枠を使用中)/ 負(incomplete の received を、ディスクから決め直す I/O)。
 *   ISO_RES_OK: 状態が finalizing になり、枠は finalizing として、このイメージが持つ。
 * out_received は NULL でもよい。
 */
int iso_upload_finish_begin(iso_upload_t *u, const char *id, uint64_t *out_received);

/* iso_upload_finish_begin が ISO_RES_OK を返した後の取り消し(ワーカーのキューがあふれたとき)。prev は、finish_begin の前の状態
 * (uploading / incomplete)。状態を戻し、incomplete だったなら枠を空ける(uploading だったなら、枠は uploading のまま)。 */
void iso_upload_finish_undo(iso_upload_t *u, iso_catalog_entry_t *e, iso_state_t prev);

/* uploading / incomplete のイメージの received を、ディスク上のパーツの大きさから決め直す(設計 3.4)。 */
int iso_upload_reload_received(iso_upload_t *u, iso_catalog_entry_t *e);

/* ---- アクティビティ ----------------------------------------------------------------- */

typedef enum {
    ISO_UP_ACT_NONE = 0,   /* 枠が空いている */
    ISO_UP_ACT_RECEIVING,  /* 直近 ISO_UPLOAD_ACTIVE_S 秒以内に PUT があった */
    ISO_UP_ACT_IDLE,
    ISO_UP_ACT_FINALIZING
} iso_upload_activity_t;

/* 経過 = now - last(巻き戻りは 0)。経過 <= ISO_UPLOAD_ACTIVE_S * 1000 なら receiving。 */
iso_upload_activity_t iso_upload_activity_from(uint64_t last_put_ms, uint64_t now_ms);
iso_upload_activity_t iso_upload_activity(const iso_upload_t *u, uint64_t now_ms);

/*
 * SD の再初期化(設計 3.4 の ③④)の、アップロード側: 開いているファイルを捨て(close のエラーは見ない)、SHA-256 の状態を捨て、
 * 枠を空ける。uploading / finalizing のイメージは、メモリ上で incomplete にする(meta は書かない。カードがない)。
 */
void iso_upload_reinit(iso_upload_t *u);

/* ---- FINISH のジョブ(iso_finish.c)と INDEX のジョブ(iso_index.c) ---------------------------- */

typedef enum {
    ISO_OUTCOME_NONE = 0,
    ISO_OUTCOME_OK,
    ISO_OUTCOME_INVALID,  /* reason: iso_invalid_reason_t(1..4) */
    ISO_OUTCOME_IOERR     /* err: iso_err_t */
} iso_outcome_t;

#define ISO_INDEX_POOL (ISO_MAX_EXTENTS + ISO_MAX_PARTS)

typedef struct {
    char id[ISO_ID_LEN + 1];
    uint8_t phase;
    uint32_t part;                 /* 次に get_extents するパーツ */
    uint32_t n_parts;
    uint32_t epoch;
    iso_extent_t *pool;            /* ISO_INDEX_POOL 個。iso_index_job_release が解放する */
    size_t pool_used;
    iso_part_input_t inputs[ISO_MAX_PARTS];
    /* 結果(step が DONE を返したとき) */
    iso_outcome_t outcome;
    uint8_t reason;
    int err;
    iso_extent_table_t *table;     /* outcome == OK のとき。所有権は呼び出し側(取り出したら NULL にする) */
} iso_index_job_t;

/* id の形式が不正なら ISO_ERR_INVALID。 */
int iso_index_job_init(iso_index_job_t *job, const char *id);
/*
 * 1 回の呼び出しで、1 つのパーツの stat + get_extents(最後は表の構築)。meta の期待値と照合する(設計 3.4):
 *   パーツの数・大きさ・欠落 → INVALID(part_mismatch)、エクステントが ISO_MAX_EXTENTS 超(get_extents の TOO_FRAGMENTED を含む)→ INVALID(too_fragmented)、
 *   I/O のエラー・アロケータの失敗 → IOERR。DONE のとき job->outcome が結果。ERROR は返さない(結果は outcome)。
 * イメージが READY または FINALIZING でなければ、IOERR(ISO_ERR_NOT_FOUND)。
 */
iso_step_result_t iso_index_step(iso_upload_t *u, iso_index_job_t *job);
/* 作業領域と、取り出していない表を解放する。冪等。 */
void iso_index_job_release(iso_upload_t *u, iso_index_job_t *job);

typedef struct {
    char id[ISO_ID_LEN + 1];
    uint8_t phase;
    bool hash_started;
    uint32_t hash_part;
    uint64_t hash_off;             /* hash_part の中の位置 */
    uint8_t digest[ISO_SHA256_LEN];
    bool have_digest;              /* digest が確定した */
    uint32_t epoch0;               /* FINISH の開始(最初のステップ)のときの、カードの epoch。ready を書く前に、変わっていないことを確かめる */
    /* iso_upload_hash_step の失敗(DONE を返し、ここに入れる): fail == NONE なら、全パーツを読み終えて digest が確定した */
    iso_outcome_t fail;
    uint8_t fail_reason;
    int fail_err;
    iso_index_job_t idx;
} iso_finish_job_t;

int iso_finish_job_init(iso_finish_job_t *job, const char *id);
/*
 * FINISH ジョブの 1 ステップ(設計 3.4。budget_bytes は SHA-256 の再計算の 1 ステップの読み出しの上限。0 でも 1 回は進む)。
 * 順序: ファイルを sync + close(FatFs の排他のため、読む前に閉じる)→ SHA-256(状態があれば確定、なければ iso_upload_hash_step で全パーツを読み直す)
 *   → 宣言値(meta のフラグ 1)との比較 → 署名(セクタ 16 のバイト 1〜5 が CD001 / BEA01。read_at)→ iso_index_step(パーツごと)
 *   → meta を ready(SHA フラグ 2)で書く → 表を hooks.table_ready で引き渡す → 枠を空ける → free_bytes のキャッシュを更新。
 * 結果: CONTINUE(続き)/ DONE(ready または invalid になった。または、中止された / 対象が finalizing でなくなっていた)/
 *   ERROR(I/O エラー: 状態は incomplete になり、枠は空く。再試行は finish)。invalid の理由: checksum_mismatch / not_iso / too_fragmented / part_mismatch。
 * aborted が真なら、何もせず(資源を解放して)DONE を返す(DELETE が割り込んだ。枠は iso_upload_abort が空けている)。
 */
iso_step_result_t iso_upload_finish_step(iso_upload_t *u, iso_finish_job_t *job, uint32_t budget_bytes, bool aborted);
/* SHA-256 の再計算の 1 ステップ(finish_step が使う。単体でも呼べる)。**逐次の SHA-256 と同じ `cfg.sha_state` を使う**(開始で、逐次の状態を捨てる)ので、
 * 同じ iso_upload_t では、再計算の開始から完了まで(DONE まで)、SHA-256 を使うほかの操作(begin、write_chunk、別イメージの finish)を、交互に進められない。
 * budget_bytes ごとに CONTINUE。全パーツを読み終えると
 * job->digest / have_digest に入れて DONE。失敗(パーツが短い・ない = INVALID part_mismatch、I/O = IOERR)は、job->fail に入れて DONE。 */
iso_step_result_t iso_upload_hash_step(iso_upload_t *u, iso_finish_job_t *job, uint32_t budget_bytes);
void iso_finish_job_release(iso_upload_t *u, iso_finish_job_t *job);

#ifdef __cplusplus
}
#endif

#endif /* ISO_UPLOAD_H */
