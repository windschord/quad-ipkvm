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
 *     MOCK_TEAR_BOTH: rename だけ。移動先のエントリが作られ、移動元も消えていない(同じ先頭クラスタを 2 つのエントリが指す。
 *                     FatFs の f_rename の、新エントリの登録 -> 旧エントリの削除の間の電源断)。他の操作は NONE と同じ
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

typedef enum { MOCK_TEAR_NONE = 0, MOCK_TEAR_HALF = 1, MOCK_TEAR_FULL = 2, MOCK_TEAR_BOTH = 3 } mock_tear_t;

typedef struct {
    bool used;
    bool is_dir;
    char path[ISO_PATH_MAX];
    uint8_t *data; /* 先頭の data_len バイト(疎なファイルは size まで 0) */
    size_t data_len;
    uint64_t size;
    uint32_t first_cluster;
    uint64_t seq;
} mock_node_t;

typedef struct {
    bool used;
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
