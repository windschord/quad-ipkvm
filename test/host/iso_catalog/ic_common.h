#ifndef IC_COMMON_H
#define IC_COMMON_H

/*
 * iso_catalog のテスト共通の部品(実行ファイルごとに別のコピー)。
 * モックの fs と乱数、カタログ、meta を作る補助。
 */

#include "unity.h"
#include "iso_catalog.h"
#include "iso_mock_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mock_fs_t FS;
static iso_fs_t IFS;
static mock_rnd_t RND;
static iso_random_t IRND;
static iso_catalog_t CAT;

#define GIB 1073741824ull

/* 既定の乱数列(64 個。末尾に来たら最後の値を返し続ける): 8 桁の 16 進にすると "11110001", "11110002", ... */
static inline void fx_default_rnd(void) {
    uint32_t seq[64];
    for (int i = 0; i < 64; i++) seq[i] = 0x11110001u + (uint32_t)i;
    mock_rnd_set(&RND, seq, 64);
}

/* /iso と /iso/import がある状態にする(あらかじめ作っておくので、書き込みの記録・電源断の数には入らない) */
static inline void fx_setup(void) {
    mock_fs_init(&FS);
    IFS = mock_fs_iface(&FS);
    fx_default_rnd();
    IRND = mock_rnd_iface(&RND);
    iso_catalog_init(&CAT, &IFS, &IRND);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/import"));
}

static inline void fx_teardown(void) {
    int open_files = FS.open_files, open_dirs = FS.open_dirs;
    mock_fs_free(&FS); /* 検査が失敗しても、メモリは先に解放する */
    TEST_ASSERT_EQUAL_INT(0, open_files);
    TEST_ASSERT_EQUAL_INT(0, open_dirs);
}

/* 準備で書いたものを、記録と電源断の数から外す */
static inline void fx_settle(void) {
    mock_fs_clear_log(&FS);
    mock_fs_reset_counter(&FS);
}

/* 有効な meta を作る(part_blocks は既定、パーツ数は大きさから) */
static inline iso_meta_t mk_meta(const char *name, uint64_t size, uint8_t state, uint32_t gen) {
    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = state;
    m.size_bytes = size;
    m.part_blocks = ISO_PART_BLOCKS_DEFAULT;
    m.part_count = (uint16_t)iso_extent_part_count(size, ISO_PART_BLOCKS_DEFAULT);
    m.name_len = (uint8_t)strlen(name);
    memcpy(m.name, name, m.name_len);
    m.generation = gen;
    return m;
}

static inline void slot_path(char *out, size_t cap, const char *id, int slot) {
    snprintf(out, cap, "/iso/%s/meta.%c", id, slot == 0 ? 'a' : 'b');
}

/* <id> のディレクトリを(なければ)作り、スロットに m を置く(スロットがあれば置き換える) */
static inline void put_slot(const char *id, int slot, const iso_meta_t *m) {
    char dir[ISO_PATH_MAX], path[ISO_PATH_MAX];
    snprintf(dir, sizeof(dir), "/iso/%s", id);
    if (!mock_fs_exists(&FS, dir)) TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, dir));
    slot_path(path, sizeof(path), id, slot);
    uint8_t buf[ISO_META_SIZE];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(m, buf));
    if (mock_fs_exists(&FS, path)) TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, path));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, path, buf, sizeof(buf), 0));
}

/* パーツのファイル(疎)を置く */
static inline void put_part(const char *id, unsigned idx, uint64_t size) {
    char path[ISO_PATH_MAX];
    snprintf(path, sizeof(path), "/iso/%s/p%03u", id, idx);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, path, size, NULL, 0, 0));
}

/* 署名つきの ISO の先頭 (セクタ 16 のバイト 1〜5 = CD001) を作って、/iso/import/<name> に置く(疎。size バイト) */
static inline void put_import(const char *name, uint64_t size, const char *sig, uint32_t first_cluster) {
    uint8_t head[32768 + 8];
    memset(head, 0, sizeof(head));
    if (sig) memcpy(head + 32768 + 1, sig, 5);
    char path[ISO_PATH_MAX];
    snprintf(path, sizeof(path), "/iso/import/%s", name);
    size_t hl = size < sizeof(head) ? (size_t)size : sizeof(head);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, path, size, head, hl, first_cluster));
}

#endif /* IC_COMMON_H */
