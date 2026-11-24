#ifndef ISO_CATALOG_H
#define ISO_CATALOG_H

/*
 * iso_catalog: ISO イメージの一覧(カタログ)。純 C、IDF 非依存。I/O は iso_seams.h の iso_fs_t / iso_random_t で注入する。
 * 契約: docs/ISO_MOUNT_DESIGN.md 3(特に 3.2 メタデータ、3.3 import、3.4 状態の遷移、削除の手順)、7(名前)、9。
 *
 * 構成
 *   iso_name.c    名前の規則、<id> の形式
 *   iso_meta.c    CRC-32、メタデータ 256 バイトの符号化・復号
 *   iso_catalog.c 2 スロット(meta.a / meta.b)、起動時のスキャン、状態の遷移、begin、削除のステップ、import
 *
 * スレッド: スレッドセーフではない。呼び出し側(グルー)が iso_op_mutex で守る(設計 4.1)。
 * ディレクトリを開いたまま、他のファイル操作をしない(列挙を先に終えて閉じてから、各エントリを処理する)。
 *
 * パス: "/iso/<id>/meta.a" のように、<id>(8 桁の小文字の 16 進。サーバーが作る)だけから作る。
 *       ユーザーの入力(名前)から、パスを作らない。import のパスだけは、/iso/import/ の下の、ファイルシステムが返した名前。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iso_seams.h"
#include "iso_extent.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ISO_MAX_IMAGES
#define ISO_MAX_IMAGES 16u /* Kconfig ISO_MAX_IMAGES */
#endif
#ifndef ISO_RESCAN_BATCH
#define ISO_RESCAN_BATCH 4u /* Kconfig ISO_RESCAN_BATCH */
#endif

#define ISO_META_SIZE 256u
#define ISO_META_VERSION 1u
#define ISO_NAME_MAX_BYTES 63u
#define ISO_ID_LEN 8u
/* begin / import の size の下限: 2048 x 17 (セクタ 0〜16。署名のセクタ 16 を含む) */
#define ISO_IMAGE_BYTES_MIN 34816u
/* <id> の mkdir が EXISTS のとき(または乱数が 0 のとき。下の予約 ID)の引き直しの上限 */
#define ISO_ID_TRIES 16u
/* 予約 ID(C-14)。iso_id_valid() は真(形式は有効で、API の id の検査は変えない。404 になる)が、
 * ワーカー(#160 の iso_worker)は media_id == 0 を「イメージなし」として扱うので、このイメージは、サイズも表も引けない。
 * iso_id_generate() は、乱数が 0 のとき引き直す(1 回に数える)。iso_catalog_scan() は、この名前のディレクトリを採用しない(hidden に数える)。 */
#define ISO_ID_RESERVED "00000000"

/* ---- 名前と id ------------------------------------------------------------ */

/* 名前の規則(設計 7、WEB_API 8): UTF-8 として正しい(不正なバイト列、過長な表現、サロゲート、U+10FFFF 超を拒否)、1〜63 バイト、
 * C0 (U+0000〜001F)、DEL、C1 (U+0080〜009F)、'/'、'\\' を含まない、前後に空白 (U+0020) がない。len は NUL を含まないバイト数。 */
bool iso_name_valid(const char *s, size_t len);
/* 同名の判定: バイト列の完全一致(大文字小文字を区別。正規化なし) */
bool iso_name_equal(const char *a, size_t alen, const char *b, size_t blen);
/* <id> の形式: 8 桁の小文字の 16 進(NUL 終端)。 */
bool iso_id_valid(const char *s);

/* ---- メタデータ (256 バイト、リトルエンディアン。設計 3.2) --------------------- */

typedef enum {
    ISO_META_UPLOADING = 0,
    ISO_META_READY = 1,
    ISO_META_INCOMPLETE = 2,
    ISO_META_INVALID = 3,
    ISO_META_DELETING = 4
} iso_meta_state_t;

typedef enum {
    ISO_INVALID_NONE = 0,
    ISO_INVALID_CHECKSUM_MISMATCH = 1,
    ISO_INVALID_NOT_ISO = 2,
    ISO_INVALID_TOO_FRAGMENTED = 3,
    ISO_INVALID_PART_MISMATCH = 4,
    ISO_INVALID_META_CORRUPT = 5 /* 実行時の理由。meta には保存しない(符号化は拒否する) */
} iso_invalid_reason_t;

typedef enum {
    ISO_SHA_NONE = 0,
    ISO_SHA_DECLARED = 1, /* begin で受け取った宣言値。finish で計算値と比べる */
    ISO_SHA_COMPUTED = 2  /* ready のとき。検証済み、または宣言なしで計算した */
} iso_sha_flag_t;

typedef struct {
    uint8_t state;          /* iso_meta_state_t */
    uint8_t invalid_reason; /* 0..4 */
    uint64_t size_bytes;    /* 1..ISO_IMAGE_BYTES_MAX */
    uint16_t part_count;    /* 1..ISO_MAX_PARTS */
    uint32_t part_blocks;   /* 1..ISO_PART_BLOCKS_MAX */
    uint8_t sha256[ISO_SHA256_LEN];
    uint8_t sha_flag;       /* iso_sha_flag_t */
    uint8_t name_len;       /* 1..63 */
    char name[ISO_NAME_MAX_BYTES + 1]; /* NUL 終端のコピー */
    uint32_t generation;
} iso_meta_t;

/* CRC-32 (IEEE 802.3、反射、初期値 0xFFFFFFFF、最終 XOR 0xFFFFFFFF)。"123456789" -> 0xCBF43926 */
uint32_t iso_crc32(const void *data, size_t len);

/* フィールドの範囲の検査(符号化・復号の両方が使う)。OK = 0、不正 = ISO_ERR_INVALID。名前は iso_name_valid で検査する。 */
int iso_meta_validate(const iso_meta_t *m);
/* 256 バイトに符号化する(予約は 0、CRC を付ける)。範囲外の値は ISO_ERR_INVALID(out は書かない)。 */
int iso_meta_encode(const iso_meta_t *m, uint8_t out[ISO_META_SIZE]);
/* 復号する。len が 256 でない、マジック・版・CRC が違う、範囲外の値は ISO_ERR_INVALID(out は変えない)。
 * 予約領域と、名前の長さより後ろの名前の枠は、内容を見ない(CRC が守る)。 */
int iso_meta_decode(const uint8_t *buf, size_t len, iso_meta_t *out);

/* ---- 2 スロット (meta.a / meta.b) ----------------------------------------- */

/* 採用するスロットを決める。-1 = どちらも無効、0 = meta.a、1 = meta.b。
 * 両方有効なら、世代が新しい方(b の方が新しいのは (int32_t)(b_gen - a_gen) > 0 のときだけ。同世代は a)。 */
int iso_slot_choose(bool a_valid, uint32_t a_gen, bool b_valid, uint32_t b_gen);

typedef struct {
    bool valid[2];
    iso_meta_t meta[2]; /* valid[i] のときだけ意味がある */
    int8_t adopted;     /* iso_slot_choose の結果 */
} iso_slots_t;

/* <id> のディレクトリから、2 つのスロットを読む。ファイルが無い、長さが 256 でない、符号化が不正 = 無効(エラーではない)。
 * I/O のエラーは、そのまま返す(無効とは扱わない)。 */
int iso_slots_read(const iso_fs_t *fs, const char *id, iso_slots_t *out);

/* ---- 状態と遷移表 (設計 3.4) ----------------------------------------------- */

typedef enum {
    ISO_STATE_NONE = 0,
    ISO_STATE_UPLOADING,
    ISO_STATE_INCOMPLETE,
    ISO_STATE_FINALIZING, /* 実行時のみ。meta には保存しない */
    ISO_STATE_READY,
    ISO_STATE_INVALID,    /* meta_corrupt を含む */
    ISO_STATE_DELETING
} iso_state_t;

typedef enum {
    ISO_EV_BEGIN = 0,
    ISO_EV_PUT,             /* 受け付けた PUT。incomplete からは再開 */
    ISO_EV_IDLE_TIMEOUT,    /* アイドルのタイムアウト(IDLE_CLOSE) */
    ISO_EV_WRITE_FAIL,      /* SD の書き込み失敗(truncate の後) */
    ISO_EV_REBOOT,          /* 起動時のスキャン */
    ISO_EV_FINISH,
    ISO_EV_FINALIZE_OK,     /* FINISH ジョブの検査に成功 */
    ISO_EV_FINALIZE_FAIL,   /* FINISH ジョブの検査に失敗(理由は apply の引数) */
    ISO_EV_FINALIZE_IOERR,  /* FINISH ジョブの SD の I/O エラー */
    ISO_EV_INDEX_OK,        /* INDEX ジョブの成功 */
    ISO_EV_INDEX_FAIL,      /* INDEX ジョブの検査の失敗(part_mismatch / too_fragmented) */
    ISO_EV_INDEX_IOERR,     /* INDEX ジョブの SD の I/O エラー */
    ISO_EV_DELETE,
    ISO_EV_DELETE_DONE,
    ISO_EV_COUNT
} iso_event_t;

typedef enum {
    ISO_TR_OK = 0,
    ISO_TR_REJECT_STATE,              /* その状態では、そのイベントは起きない(API は 409 not_uploading など) */
    ISO_TR_REJECT_SIZE_MISMATCH,      /* 409 size_mismatch */
    ISO_TR_REJECT_UPLOAD_IN_PROGRESS, /* 409 upload_in_progress */
    ISO_TR_REJECT_INVALID,            /* 422 (invalid_reason) */
    ISO_TR_REJECT_MOUNTED             /* 409 mounted */
} iso_trans_t;

typedef struct {
    bool received_complete; /* received == size_bytes (FINISH) */
    bool slot_free;         /* アップロードの枠が空いている (incomplete の PUT / FINISH) */
    bool mounted;           /* マウント中 (DELETE) */
} iso_trans_ctx_t;

/* 現在の状態 + イベント -> 次の状態と結果(純関数)。拒否のとき *next = cur。ctx が NULL なら全て false。 */
iso_trans_t iso_state_transition(iso_state_t cur, iso_event_t ev, const iso_trans_ctx_t *ctx, iso_state_t *next);
/* meta に保存する状態の値(ISO_META_*)。FINALIZING と NONE は保存しない: -1。 */
int iso_state_to_meta(iso_state_t s);
/* meta の状態の値 -> 実行時の状態(ISO_EV_REBOOT を適用する前)。範囲外は ISO_STATE_NONE。 */
iso_state_t iso_state_from_meta(uint8_t meta_state);

/* ---- カタログ -------------------------------------------------------------- */

/* iso_catalog の追加のエラー(iso_err_t と重ならない値) */
enum {
    ISO_CAT_ERR_NAME_EXISTS = -101,   /* 409 name_exists */
    ISO_CAT_ERR_TOO_MANY = -102,      /* 409 too_many_images */
    ISO_CAT_ERR_ID_EXHAUSTED = -103,  /* <id> を ISO_ID_TRIES 回引き直しても、mkdir が EXISTS */
    ISO_CAT_ERR_CROSSLINKED = -104    /* 409 crosslinked: <id>/p000 と、import/ のファイルが、同じ先頭クラスタを指している(設計 3.3) */
};

typedef struct {
    char id[ISO_ID_LEN + 1];
    iso_state_t state;         /* 実行時の状態 */
    uint8_t invalid_reason;    /* state == INVALID のとき: 0..4、または ISO_INVALID_META_CORRUPT */
    bool meta_valid;           /* false = 両方のスロットが無効 (meta_corrupt)。meta / adopted は無意味 */
    bool resume_delete;        /* meta の状態が deleting(削除を続ける印) */
    int8_t adopted;            /* 採用しているスロット: 0 = meta.a、1 = meta.b、-1 = なし */
    uint64_t received;         /* 受信済みのバイト数 (設計 3.4) */
    iso_meta_t meta;           /* 採用している meta */
} iso_catalog_entry_t;

typedef struct {
    const iso_fs_t *fs;
    const iso_random_t *rnd;
    iso_catalog_entry_t images[ISO_MAX_IMAGES];
    size_t count;
    uint32_t hidden;           /* ISO_MAX_IMAGES を超えて、一覧に出さないディレクトリの数 */
} iso_catalog_t;

void iso_catalog_init(iso_catalog_t *cat, const iso_fs_t *fs, const iso_random_t *rnd);
/* /iso と /iso/import を作る(あれば何もしない)。 */
int iso_catalog_ensure_dirs(const iso_catalog_t *cat);
iso_catalog_entry_t *iso_catalog_find(iso_catalog_t *cat, const char *id);
/* 同名があるか(deleting のものは、API から見えないので数えない。meta_corrupt のものは名前がない)。 */
bool iso_catalog_name_exists(const iso_catalog_t *cat, const char *name, size_t len);

/*
 * 起動時のスキャン(SD の再初期化の後も同じ)。カタログを作り直す。
 *   - /iso/ の直下で、<id> の形式でない名前、ディレクトリでないものは無視。
 *   - 予約 ID (ISO_ID_RESERVED = "00000000") のディレクトリは、採用しない(ISO_MAX_IMAGES の枠も使わない)。hidden に数える。
 *   - 先頭から ISO_MAX_IMAGES 個を一覧に入れ、それを超える分は hidden に数える(ディレクトリの順)。
 *   - meta の状態 uploading -> incomplete (ISO_EV_REBOOT)。両スロット無効 -> state INVALID、理由 meta_corrupt。
 *     meta の状態 deleting -> resume_delete。
 *   - received: ready は size_bytes。uploading / incomplete / invalid は iso_catalog_received()。それ以外は 0。
 *   I/O のエラーは、そのまま返す(カタログは途中の状態。呼び出し側は、SD の再初期化から)。
 */
int iso_catalog_scan(iso_catalog_t *cat);

/* 同期済みのパーツのファイルの大きさの合計(先頭から順に数え、パーツの期待する大きさに満たない/無いパーツで止める。
 * 期待する大きさを超えるパーツは、期待する大きさで数える)。 */
int iso_catalog_received(const iso_fs_t *fs, const char *id, const iso_meta_t *meta, uint64_t *out);

/*
 * begin(設計 3.2 の作成の順序): 検査 -> mkdir <id> -> meta.a (世代 1、状態 uploading、SHA-256 の宣言値があればフラグ 1) を overwrite + sync。
 *   検査の順: 名前の規則 / size の範囲 (ISO_ERR_INVALID) -> 同名 (ISO_CAT_ERR_NAME_EXISTS) -> 上限 (ISO_CAT_ERR_TOO_MANY)。
 *   <id> は iso_random_t から作り、mkdir が EXISTS なら引き直す (ISO_ID_TRIES 回まで。尽きたら ISO_CAT_ERR_ID_EXHAUSTED)。
 *   meta の書き込みに失敗したら、作ったディレクトリを片づける(ベストエフォート)。
 *   成功したら、カタログに state UPLOADING で入り、out_id (9 バイト) に <id> を返す。
 *   アップロードの枠、空き容量の検査は、API の仕事(ここでは見ない)。
 */
int iso_catalog_begin(iso_catalog_t *cat, const char *name, size_t name_len, uint64_t size_bytes, const uint8_t *sha256,
                      char out_id[ISO_ID_LEN + 1]);

/* <id> を作る(mkdir)。begin と import が使う。乱数が 0 のときは、予約 ID なので、mkdir せずに引き直す(ISO_ID_TRIES に数える)。 */
int iso_id_generate(const iso_fs_t *fs, const iso_random_t *rnd, char out[ISO_ID_LEN + 1]);

/* 採用していないスロットに、世代 + 1 で meta を書く(overwrite + sync)。成功したら e->meta / e->adopted を更新する。
 * 失敗したら e は変えない(古いスロットは無傷)。e->meta_valid が false なら ISO_ERR_INVALID。next->generation は無視する。 */
int iso_catalog_write_meta(iso_catalog_t *cat, iso_catalog_entry_t *e, const iso_meta_t *next);

typedef struct {
    uint8_t invalid_reason;      /* FINALIZE_FAIL / INDEX_FAIL: 1..4 */
    bool set_sha;                /* FINALIZE_OK: SHA-256 のフラグと値を更新する */
    uint8_t sha_flag;            /* set_sha のとき: 0 (import など) または 2 (計算値) */
    const uint8_t *sha256;       /* sha_flag == 2 のとき必須(32 バイト) */
} iso_apply_arg_t;

/*
 * イベントを適用する: 遷移表で次の状態を決め、meta を書くイベントでは meta を書いてから、状態を更新する。
 *   meta を書く: IDLE_TIMEOUT / WRITE_FAIL (-> incomplete)、FINALIZE_OK (-> ready)、FINALIZE_FAIL / INDEX_FAIL (-> invalid)、
 *                FINALIZE_IOERR (-> incomplete)。
 *   書かない: PUT(再開しても meta は変えない。再起動すれば incomplete に戻る)、FINISH(ready になるまで meta の状態は変えない)、
 *             REBOOT、INDEX_OK / INDEX_IOERR、DELETE(meta の deleting は削除のステップの最初が書く)、BEGIN(iso_catalog_begin)。
 *   FINISH は、received_complete を e->received == e->meta.size_bytes から決める。ctx の他のフィールドは呼び出し側が渡す。
 *   DELETE_DONE は、エントリをカタログから消す(e は無効になる)。
 * 戻り値: 0 = 適用した、正 = iso_trans_t の拒否(何も変えない)、負 = iso_err_t(meta の書き込みの失敗。何も変えない)。
 */
int iso_catalog_apply(iso_catalog_t *cat, iso_catalog_entry_t *e, iso_event_t ev, const iso_trans_ctx_t *ctx,
                      const iso_apply_arg_t *arg);

/* エントリをカタログから消す(配列を詰める)。 */
void iso_catalog_remove(iso_catalog_t *cat, const char *id);

/* ---- 削除 (設計 3.4 末尾) -------------------------------------------------- */

typedef enum { ISO_STEP_CONTINUE = 0, ISO_STEP_DONE = 1, ISO_STEP_ERROR = 2 } iso_step_t;
typedef struct {
    iso_step_t kind;
    int err; /* kind == ISO_STEP_ERROR のとき iso_err_t */
} iso_step_result_t;

/*
 * DELETE の受け付けの前の検査(設計 3.3): <id>/p000 の先頭クラスタが、/iso/import/ のどれかのファイルと同じなら
 * ISO_CAT_ERR_CROSSLINKED (API は 409 crosslinked)。そうでなければ ISO_OK。何も変更しない。
 * (rename の途中の電源断で、2 つのエントリが同じクラスタの連鎖を指している。p000 を unlink すると、連鎖が解放され、
 *  import/ 側のファイルが、解放済みのクラスタを指して、次の rescan で取り込まれてしまう。)
 * 先頭クラスタ 0、<id>/p000 がない、import/ がない、は一致なし。I/O のエラーは、そのまま返す。
 * ハンドラーは、iso_catalog_apply(ISO_EV_DELETE) の前に呼ぶ。
 */
int iso_catalog_delete_precheck(const iso_catalog_t *cat, const char *id);

typedef struct {
    char id[ISO_ID_LEN + 1];
    uint8_t phase;
    int8_t keep;  /* 最後に消すスロット(deleting を書いた方)。-1 = 有効なスロットがなかった */
} iso_delete_job_t;

/* id の形式が不正なら ISO_ERR_INVALID */
int iso_delete_job_init(iso_delete_job_t *job, const char *id);
/*
 * 1 回の呼び出しで、書き込み・削除を 1 つだけ行う:
 *   (1) 有効なスロットがあり、状態が deleting でなければ、deleting で書く(採用していない方へ)。
 *   (2) p* のファイルを 1 つずつ remove(ディレクトリの一覧から。meta の内容に頼らない)。
 *   (3) 採用していない(古い)スロット、最後に deleting を書いた方を remove。
 *   (4) ディレクトリを remove -> DONE (カタログからも消す)。
 * remove が NOT_FOUND ならすでに消えているとして進む。他のエラーは ERROR (カタログは変えない。次の機会に最初から続ける)。
 * 最初のステップ((1) の前)で、iso_catalog_delete_precheck と同じクロスリンクの照合をする。一致したら、何も変更せず(meta も、
 * パーツも、p000 も)ERROR(ISO_CAT_ERR_CROSSLINKED) を返す。再起動後の続き(meta がすでに deleting)でも同じ。
 * この ERROR は、呼び出し側(グルー)が警告をログに出し、PC の chkdsk で直すまで、deleting のまま残す(再試行は同じ結果になる)。
 */
iso_step_result_t iso_catalog_delete_step(iso_catalog_t *cat, iso_delete_job_t *job);

/* ---- import (設計 3.3) ------------------------------------------------------ */

typedef struct {
    uint32_t imported;
    uint32_t skipped;
    bool has_next;
    uint32_t next_cursor; /* has_next のときだけ: cursor + skipped */
} iso_rescan_result_t;

/*
 * /iso/import/ の *.iso (拡張子の大文字小文字は区別しない。ディレクトリは除く) を、ディレクトリの順に、先頭の cursor 個を読み飛ばして、
 * 最大 ISO_RESCAN_BATCH 個まで調べ、取り込む。取り込めないものは skipped に数える(ファイルは残す):
 *   名前(拡張子を除く)が名前の規則に合わない(63 バイト超を含む)、サイズ < ISO_IMAGE_BYTES_MIN、上限、同名、
 *   先頭クラスタが既存の <id>/p000 と同じ(クロスリンク)、セクタ 16 のバイト 1〜5 が CD001 / BEA01 でない。
 * 取り込みは、<id> を作り、meta.a (state ready、パーツ 1、sha なし) を書き、/iso/import/<名前> を <id>/p000 へ rename する。
 * I/O のエラーは、その時点までの結果を out に入れて、そのまま返す。
 */
int iso_catalog_rescan(iso_catalog_t *cat, uint32_t cursor, iso_rescan_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ISO_CATALOG_H */
