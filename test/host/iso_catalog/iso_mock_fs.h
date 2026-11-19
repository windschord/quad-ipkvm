#ifndef ISO_MOCK_FS_H
#define ISO_MOCK_FS_H

/*
 * iso_catalog のホストテスト用のモック: メモリ上の iso_fs_t (iso_seams.h) と、iso_random_t。
 *
 * 再現すること(FatFs の VFS ラッパーの契約):
 *   - パスはボリュームのルートからの絶対パス ("/iso/a1b2c3d4/meta.a")。ルート "/" は最初からある。
 *   - mkdir: 親がなければ NOT_FOUND、すでに存在すれば EXISTS。remove: 無ければ NOT_FOUND、空でないディレクトリは INVALID。
 *   - rename: 宛先が存在すれば EXISTS (FatFs の f_rename)。移すと、ディレクトリの順は末尾になる。先頭クラスタは保つ。
 *   - overwrite: 作成、または先頭から上書き(長さは len になる)。
 *   - ディレクトリの順は、作成(または rename)した順。
 *   - stat の first_cluster は、ファイルの作成時に一意な値を割り当てる(mock_fs_add_file で指定もできる = クロスリンクの再現)。
 *   - 大きなファイルは「疎」にできる(先頭の数バイトだけ持ち、残りは 0)。
 *
 * 電源断の注入(書き込み = mkdir / remove / rename / overwrite を数える):
 *   mock_fs_cut_at(fs, n, tear): 今から n 回目の書き込みで電源が落ちる。その書き込みは IO エラーを返し、
 *   tear に従って「途中までの内容」が残る。以降は mock_fs_power_on() まで、すべての操作が IO エラー(読み出しも)。
 *     MOCK_TEAR_NONE: その書き込みは何も起きない
 *     MOCK_TEAR_HALF: overwrite は、データの前半だけを書いたファイルになる(長さ = len / 2)。他の操作は何も起きない
 *     MOCK_TEAR_FULL: その書き込みは完了している(戻り値だけ IO エラー)
 *
 * 書き込みの API(open_for_write / write / sync / truncate / close。P8 C-10b の iso_upload 用):
 *   - 書き込み用のハンドルが開いているファイルを、open_for_read / get_extents / remove / truncate / 別の open_for_write で開くと
 *     ISO_ERR_BUSY (FatFs のファイルの排他。呼び出し側が、先に close することを確かめる)。
 *   - write は末尾への追記だけ。**未同期の分は、電源断で失われる**: ノードは「同期済みの大きさ」(synced_size)を持ち、
 *     write は未同期にし、sync(と、未同期のハンドルの close)は同期済みにする。電源が落ちると(settle)、未同期のファイルは
 *     同期済みの大きさに戻る(ディレクトリ上の大きさは、sync / close のときだけ更新される、という FatFs の動き)。
 *   - 書き込み・sync・truncate・open_for_write(作成も追記も)・未同期のハンドルの close は、書き込みとして数える(電源断・エラーの注入の対象)。
 *   - 書き込みの失敗の注入: mock_fs_fail_write(nth, keep, err): 通算 nth 回目の write が、先頭の keep バイトだけ追記して err を返す
 *     (FatFs が空きなしのとき bw < btw で返す形。keep = 0 なら何も書かない)。
 *   - get_extents: 既定は、ファイルの大きさ(セクタ数に切り上げ)の 1 本のエクステント。mock_fs_set_extents / set_fragments で変える。
 *   - mock_fs_set_discard(true): write は大きさだけを増やし、中身を持たない(読み出しは 0。1 GiB のパーツの境界のテスト用)。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iso_seams.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_MAX_NODES 160
#define MOCK_MAX_HANDLES 8
#define MOCK_LOG_MAX 512
#define MOCK_LOG_LEN 340

typedef enum { MOCK_TEAR_NONE = 0, MOCK_TEAR_HALF = 1, MOCK_TEAR_FULL = 2 } mock_tear_t;

#define MOCK_MAX_EXTENTS 64

typedef struct {
    bool used;
    bool is_dir;
    char path[ISO_PATH_MAX];
    uint8_t *data; /* 先頭の data_len バイト(疎なファイルは size まで 0) */
    size_t data_len;
    uint64_t size;
    uint32_t first_cluster;
    uint64_t seq;
    /* 書き込み(未同期の分は電源断で失われる) */
    bool unsynced;
    uint64_t synced_size; /* unsynced のとき、ディレクトリ上の(同期済みの)大きさ */
    int writers;          /* 書き込み用のハンドルの数(0 または 1) */
    /* get_extents */
    bool ext_set;
    iso_extent_t ext[MOCK_MAX_EXTENTS];
    int ext_n;
    int fragments; /* ext_set でないとき、0 = 1 本、n = n 本に分ける */
    int ext_err;   /* 0 でなければ、get_extents がこのエラーで失敗する */
} mock_node_t;

typedef struct {
    bool used;
    bool write;
    int node;
    uint64_t pos;
} mock_file_t;

typedef struct {
    bool used;
    int idx[MOCK_MAX_NODES]; /* 開いた時点の、子のノード */
    int n;
    int pos;
    bool fail_next;
} mock_dir_t;

typedef struct {
    mock_node_t nodes[MOCK_MAX_NODES];
    mock_file_t files[MOCK_MAX_HANDLES];
    mock_dir_t dirs[MOCK_MAX_HANDLES];
    uint64_t seq;
    uint32_t next_cluster;

    /* 電源断 */
    int mut_count; /* これまでの書き込みの回数(電源断の書き込みを含む) */
    int cut_at;    /* 0 = なし */
    mock_tear_t tear;
    bool dead;
    int err_at;    /* 0 = なし。n 回目の書き込みだけ、err_code で失敗する(何も起きない。電源は落ちない) */
    int err_code;
    bool settled;  /* 電源断のときの「未同期の分を失う」処理を済ませたか */

    /* write の失敗の注入(通算の write の回数。mock_fs_fail_write) */
    int wf_at;     /* 0 = なし */
    size_t wf_keep;
    int wf_err;
    bool discard;  /* write は大きさだけを増やす */

    /* マウント情報(mount_info) */
    uint64_t mi_total;
    uint64_t mi_free;
    uint32_t mi_cluster;
    int mi_err;    /* 0 でなければ mount_info がこのエラーで失敗する */

    /* 呼び出しの数 */
    int n_write, n_sync, n_truncate, n_open_w, n_read, n_read_at;
    uint64_t last_read_at_offset;

    /* 読み出しの失敗の注入(完全一致のパス) */
    char fail_path[ISO_PATH_MAX];
    bool fail_path_set;
    char fail_open_path[ISO_PATH_MAX]; /* stat は成功し、open_for_read だけが IO エラー */
    bool fail_open_set;
    char fail_read_path[ISO_PATH_MAX]; /* stat と open は成功し、read だけが IO エラー */
    bool fail_read_set;
    int fail_read_nth; /* 0 = すべての read、n = n 回目の read(そのパスの、通算) */
    int fail_read_calls;
    char fail_dir_next_path[ISO_PATH_MAX]; /* dir_open は成功し、dir_next が IO エラー */
    bool fail_dir_next_set;

    /* 書き込みの記録 */
    char log[MOCK_LOG_MAX][MOCK_LOG_LEN];
    int nlog;

    /* 呼び出しの数(資源の漏れの検査) */
    int open_files;
    int open_dirs;
} mock_fs_t;

void mock_fs_init(mock_fs_t *m);
void mock_fs_free(mock_fs_t *m); /* ノードのデータを解放する */
iso_fs_t mock_fs_iface(mock_fs_t *m);

/* 準備(電源断や記録の対象にならない) */
int mock_fs_add_dir(mock_fs_t *m, const char *path);
int mock_fs_add_file(mock_fs_t *m, const char *path, const void *data, size_t len, uint32_t first_cluster);
/* size バイトのファイル。先頭の head_len バイトだけ head、残りは 0 */
int mock_fs_add_sparse(mock_fs_t *m, const char *path, uint64_t size, const void *head, size_t head_len, uint32_t first_cluster);

/* 検査 */
bool mock_fs_exists(const mock_fs_t *m, const char *path);
bool mock_fs_is_dir(const mock_fs_t *m, const char *path);
int64_t mock_fs_size(const mock_fs_t *m, const char *path); /* 無ければ -1 */
const uint8_t *mock_fs_data(const mock_fs_t *m, const char *path, size_t *len); /* 無ければ NULL */
uint32_t mock_fs_first_cluster(const mock_fs_t *m, const char *path);
int mock_fs_child_count(const mock_fs_t *m, const char *dir);
/* dir の子の名前を、ディレクトリの順に最大 max 個。個数を返す。 */
int mock_fs_children(const mock_fs_t *m, const char *dir, char names[][ISO_NAME_MAX + 1], int max);

/* ファイルの壊し方(テスト用) */
bool mock_fs_set_first_cluster(mock_fs_t *m, const char *path, uint32_t first_cluster);
bool mock_fs_poke(mock_fs_t *m, const char *path, size_t offset, uint8_t value);
bool mock_fs_set_size(mock_fs_t *m, const char *path, uint64_t size); /* 切り詰める、または 0 で伸ばす(疎) */

/* 電源断 */
void mock_fs_cut_at(mock_fs_t *m, int n, mock_tear_t tear);
/* 今から n 回目の書き込みだけが err_code (iso_err_t) で失敗する(何も起きない)。電源は落ちない。 */
void mock_fs_error_at(mock_fs_t *m, int n, int err_code);
void mock_fs_power_on(mock_fs_t *m); /* dead と cut_at を解除する(内容は、そのまま) */
void mock_fs_reset_counter(mock_fs_t *m);
/* 書き込み用のハンドルを、同期しないで全部捨てる(電源断の後の「再起動」。mock_fs_power_on もこれを行う) */
void mock_fs_drop_handles(mock_fs_t *m);
/* 電源が落ちた後の、未同期の分の消失を、いま適用する(検査用の関数も、必要なら自動で行う。冪等) */
void mock_fs_settle(mock_fs_t *m);

/* 書き込みの API の注入 */
void mock_fs_fail_write(mock_fs_t *m, int nth, size_t keep, int err); /* nth = 0 で解除 */
void mock_fs_set_discard(mock_fs_t *m, bool discard);
void mock_fs_set_mount_info(mock_fs_t *m, uint64_t total, uint64_t free_bytes, uint32_t cluster);
void mock_fs_fail_mount_info(mock_fs_t *m, int err);
bool mock_fs_set_extents(mock_fs_t *m, const char *path, const iso_extent_t *ext, int n);
bool mock_fs_set_fragments(mock_fs_t *m, const char *path, int n);
bool mock_fs_fail_extents(mock_fs_t *m, const char *path, int err);

/* 読み出しの失敗 */
void mock_fs_fail_path(mock_fs_t *m, const char *path_or_null);
void mock_fs_fail_dir_next(mock_fs_t *m, const char *dir_or_null);
void mock_fs_fail_open(mock_fs_t *m, const char *path_or_null);
void mock_fs_fail_read(mock_fs_t *m, const char *path_or_null, int nth);

/* 記録 */
void mock_fs_clear_log(mock_fs_t *m);
/* 記録の i 番目が、完全に s と一致するか */
bool mock_fs_log_is(const mock_fs_t *m, int i, const char *s);

/* ---- 乱数 ---- */
typedef struct {
    uint32_t seq[64];
    int n;
    int idx;
    int calls;
} mock_rnd_t;

void mock_rnd_set(mock_rnd_t *r, const uint32_t *seq, int n); /* 末尾まで来たら、最後の値を返し続ける */
iso_random_t mock_rnd_iface(mock_rnd_t *r);

#ifdef __cplusplus
}
#endif

#endif /* ISO_MOCK_FS_H */
