/*
 * iso_seams.h — P8（ISO の仮想メディア）のシームの宣言。宣言だけで、実装は持たない。
 *
 * 契約: docs/ISO_MOUNT_DESIGN.md、docs/INTERFACES.md §7。
 * 純 C の部品（iso_catalog、iso_upload、iso_media、iso_extent、mmc_scsi、msc_bot、iso_api）は、
 * IDF のヘッダーに依存せず、I/O・時刻・乱数・SHA-256 を、ここの関数ポインタで受け取る。
 * 実装（SD、FatFs、mbedTLS、ワーカー、USB）は、実機側（Antigravity）が持つ。
 *
 * ロックの規則（ISO_MOUNT_DESIGN.md §4.1）:
 *   - sd_io_lock は、このヘッダーの iso_blockdev_t / iso_fs_t の実装の中が、自分で取る。
 *     呼び出し側（純 C、ワーカー、MSC）は、決して取らない。
 *   - 呼び出し側は、iso_state_mutex を持ったまま、これらの関数を呼ばない。
 */
#ifndef ISO_SEAMS_H
#define ISO_SEAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- エラー ------------------------------------------------------------ */
typedef enum {
    ISO_OK = 0,
    ISO_ERR_NO_CARD = -1,        /* カードがない、または初期化できていない */
    ISO_ERR_NO_SPACE = -2,       /* 空き容量が足りない */
    ISO_ERR_IO = -3,             /* I/O エラー（タイムアウト、CRC など） */
    ISO_ERR_NOT_FOUND = -4,      /* ファイル・ディレクトリがない */
    ISO_ERR_EXISTS = -5,         /* すでに存在する（rename の宛先など） */
    ISO_ERR_INVALID = -6,        /* 引数、またはパスが不正 */
    ISO_ERR_TOO_FRAGMENTED = -7, /* get_extents: 断片が多すぎる（FR_NOT_ENOUGH_CORE、または max 超過） */
    ISO_ERR_EPOCH = -8,          /* read_sectors: カードの世代が変わった（別のカード、再初期化） */
    ISO_ERR_BUSY = -9,
    ISO_ERR_NOMEM = -10,
    ISO_ERR_UNSUPPORTED_FS = -11 /* FAT32 でない、またはクラスタが 2048 バイト未満 */
} iso_err_t;

/* ---- パス ---------------------------------------------------------------
 * iso_fs_t のパスは、ボリュームのルートからの絶対パス（例 "/iso/a1b2c3d4/p000"）。
 * 区切りは '/'。最大 ISO_PATH_MAX（終端の NUL を含む。超えるものは切り詰めず ISO_ERR_INVALID）。ドライブ名（"0:" など）は、実装が付ける。
 * ユーザーの入力から、パスを作らない（ISO_MOUNT_DESIGN.md §7）。 */
#define ISO_NAME_MAX 255 /* ディレクトリの 1 エントリの名前（FatFs の LFN。UTF-8 のバイト数）*/
#define ISO_PATH_MAX 288 /* "/iso/import/" (12) + ISO_NAME_MAX + NUL に余裕。切り詰めない（超えたら ISO_ERR_INVALID）*/

/* ---- SD の状態 ---------------------------------------------------------- */
typedef enum {
    ISO_SD_NO_CARD = 0,
    ISO_SD_UNSUPPORTED_FS,
    ISO_SD_ERROR,
    ISO_SD_READY
} iso_sd_state_t;

/* ---- iso_blockdev_t: SD の 512B セクタの直接読み出し（書き込みはない）---- */
typedef struct {
    void *ctx;
    /* セクタ数（カードなし・未初期化は 0）。セクタの大きさは 512 固定。 */
    uint64_t (*sector_count)(void *ctx);
    /* カードの世代（原子的な uint32_t。持ち主はこの実装）。SD の再初期化と force_eject が epoch_bump で進める。
     * epoch / epoch_bump は、どのロックの中からも呼んでよい（ロックを取らない）。 */
    uint32_t (*epoch)(void *ctx);
    void (*epoch_bump)(void *ctx);
    /* ブロックする。0 = 成功、負 = iso_err_t。sd_io_lock を内部で取り、取った後で
     * 現在の epoch（ロックなしの原子的な読み出し）と引数 epoch を比べ、違えば ISO_ERR_EPOCH を返す。
     * out は、DMA 可能で、キャッシュライン境界に合わせたバッファ（USB のバッファと共用するかは A-14 が決める）。 */
    int (*read_sectors)(void *ctx, uint32_t epoch, uint64_t lba, uint32_t n, uint8_t *out);
} iso_blockdev_t;

/* ---- iso_fs_t: FAT32 のファイル操作 ------------------------------------- */
typedef struct iso_file iso_file_t; /* 不透明（実装が定義する） */
typedef struct iso_dir iso_dir_t;   /* 不透明 */

typedef struct {
    bool is_dir;
    uint64_t size;           /* ファイルの大きさ（バイト） */
    uint32_t first_cluster;  /* 先頭クラスタ（ファイル。ディレクトリは 0 でもよい）。import のクロスリンクの検出に使う */
} iso_stat_t;

typedef struct {
    bool fat32;              /* FAT32 か（FAT12/16、exFAT は false） */
    uint32_t cluster_bytes;  /* 1 クラスタの大きさ（バイト） */
    uint64_t total_bytes;
    uint64_t free_bytes;     /* キャッシュ値でよい。更新はマウント時とワーカーだけ（httpd から更新しない） */
} iso_mount_info_t;

typedef struct {
    uint64_t start_sector;   /* SD の絶対セクタ（512B）。fs->database + csize × (クラスタ − 2) */
    uint64_t sector_count;
} iso_extent_t;

typedef struct {
    void *ctx;

    int (*mount_info)(void *ctx, iso_mount_info_t *out);

    /* すでに存在すれば ISO_ERR_EXISTS（<id> の引き直しに使う）。 */
    int (*mkdir)(void *ctx, const char *path);
    /* ファイル、または空のディレクトリを消す。ファイルの消去はクラスタの連鎖を解放する（import の
     * クロスリンクしたファイルには、呼んではならない）。 */
    int (*remove)(void *ctx, const char *path);
    /* 宛先が存在すると ISO_ERR_EXISTS（FatFs の f_rename）。import の取り込みだけが使う。 */
    int (*rename)(void *ctx, const char *from, const char *to);
    int (*stat)(void *ctx, const char *path, iso_stat_t *out);

    int (*dir_open)(void *ctx, const char *path, iso_dir_t **out);
    /* 次の名前を name（ISO_NAME_MAX + 1 バイト）に入れる。終わりは 1 を返す。0 = 取得、負 = エラー。 */
    int (*dir_next)(void *ctx, iso_dir_t *dir, char *name, iso_stat_t *st);
    void (*dir_close)(void *ctx, iso_dir_t *dir);

    /* 書き込み用。append = true は、末尾に続けて書く（無ければ作る）。append = false は、新規作成で、
     * すでに存在すれば ISO_ERR_EXISTS（FA_CREATE_NEW）。順に追記だけ。 */
    int (*open_for_write)(void *ctx, const char *path, bool append, iso_file_t **out);
    /* 書いたバイト数が len に満たない（FatFs は、空きがないとき FR_OK のまま bw < btw で返す）ときは、
     * ISO_ERR_NO_SPACE を返す。 */
    int (*write)(void *ctx, iso_file_t *f, const void *data, size_t len);
    int (*sync)(void *ctx, iso_file_t *f);
    /* 失敗した write の後でも呼べる。**読み取り用に開いたハンドル（open_for_read）も、これで閉じる**。
     * 書き込み用のハンドルは f_sync してから閉じる。失敗しても、ハンドルは解放する。 */
    int (*close)(void *ctx, iso_file_t *f);
    /* close → 開き直し → f_lseek(size) → f_truncate。ハンドル版はない（失敗した write のハンドルでは
     * f_truncate が即エラーになるため）。実ファイルが size より小さければ ISO_ERR_INVALID（f_lseek がファイルを
     * 伸ばしてしまうため）。close が失敗してもハンドルは解放する。ISO_MOUNT_DESIGN.md §3.1。 */
    int (*truncate)(void *ctx, const char *path, uint64_t size);
    /* 作成、または先頭から上書きして、同期する。meta.a / meta.b の更新用（256 バイト）。 */
    int (*overwrite)(void *ctx, const char *path, const void *data, size_t len);

    int (*open_for_read)(void *ctx, const char *path, iso_file_t **out);
    /* 読んだバイト数を *got に入れる。EOF は *got < len。 */
    int (*read)(void *ctx, iso_file_t *f, void *buf, size_t len, size_t *got);
    /* 位置を指定した読み出し（f_lseek + f_read。open_for_read のハンドルで使う）。
     * **同じハンドルで `read` と `read_at` を混ぜない。`read_at` の後の `read` の位置は未定義**（位置を保存して戻す実装は、後ろ向きの seek になり、
     * FatFs が 1 GiB パーツ（約 65536 クラスタ）のチェーンを毎回先頭からたどり直して、1 MiB ずつ読む FINISH が極端に遅くなるため）。
     * 読んだバイト数を *got に入れる。EOF は *got < len（offset が EOF 以降なら *got = 0）。 */
    int (*read_at)(void *ctx, iso_file_t *f, uint64_t offset, void *buf, size_t len, size_t *got);

    /* ファイルを読み取りモードで開き、CLMT から連続した SD セクタの区間の一覧を作る。
     * 配列（cltbl）の確保と解放は、この関数の中。out は呼び出し側の配列（max 個）。
     * *n に区間の数（max を超えるときは必要な数）を入れ、足りなければ ISO_ERR_TOO_FRAGMENTED。 */
    int (*get_extents)(void *ctx, const char *path, iso_extent_t *out, size_t max, size_t *n);
} iso_fs_t;

/* ---- 時刻、乱数、SHA-256、SD の状態 ------------------------------------- */
typedef struct {
    void *ctx;
    uint64_t (*now_ms)(void *ctx); /* 単調増加。巻き戻りは、純 C 側が「経過 0」に倒す */
} iso_clock_t;

typedef struct {
    void *ctx;
    uint32_t (*next)(void *ctx);   /* <id>（8 桁の 16 進）の生成用。衝突したら、純 C 側が引き直す */
} iso_random_t;

#define ISO_SHA256_LEN 32
typedef struct {
    size_t state_size;             /* 呼び出し側が確保する、逐次の状態の大きさ（バイト） */
    void (*init)(void *state);
    void (*update)(void *state, const void *data, size_t len);
    /* ダイジェストを out に確定する(状態は、これで使えなくなる)。成功なら ISO_OK、失敗(PSA などのエラー)なら ISO_ERR_IO。
     * 失敗したときの out は、使ってはならない(実装は 0 で埋めてよいが、呼び出し側は読まない)。呼び出し側は、ISO_OK 以外を
     * すべて失敗として扱い、そのダイジェストを保存も比較もしない(FINISH は incomplete にして、再試行を待つ)。 */
    int (*final)(void *state, uint8_t out[ISO_SHA256_LEN]);
} iso_sha256_t;

typedef struct {
    void *ctx;
    iso_sd_state_t (*state)(void *ctx); /* sd.state の供給元。SD の監視（A-13）が決める */
} iso_sd_status_t;

#ifdef __cplusplus
}
#endif

#endif /* ISO_SEAMS_H */
