#ifndef UP_COMMON_H
#define UP_COMMON_H

/*
 * iso_upload / iso_core のテスト共通の部品(実行ファイルごとに別のコピー)。
 * モックの fs(電源断・書き込みの失敗の注入つき)、偽の時計、SHA-256 の参照実装、メモリの確保の数え上げ、
 * 決定的なデータの生成(画像の i バイト目 = gen_byte(i)。セクタ 16 のバイト 1〜5 は署名)。
 */

#include "unity.h"
#include "iso_upload.h"
#include "iso_mock_fs.h"
#include "up_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIB 1073741824ull
#define MIB 1048576ull
#define PB_SMALL 17u /* 17 ブロック = 34816 バイトのパーツ(署名のセクタ 16 が p000 に入る最小) */
#define PART_SMALL (PB_SMALL * 2048ull)

static mock_fs_t FS;
static iso_fs_t IFS;
static mock_rnd_t RND;
static iso_random_t IRND;
static iso_catalog_t CAT;
static uint64_t NOW_MS;
static iso_clock_t CLK;
static iso_sha256_t SHA;
static up_sha256_state_t SHA_STATE;
static uint8_t IOBUF[4096];
static iso_allocator_t ALLOC;
static int ALLOC_LIVE;
static int ALLOC_CALLS;
static int ALLOC_FAIL_AT; /* n 回目の確保を失敗させる(0 = なし) */
static uint32_t EPOCH;
static iso_blockdev_t BDEV;
static iso_upload_t UP;
static int VIEW_CALLS;
static int TABLES_READY;
static char TABLE_ID[ISO_ID_LEN + 1];
static iso_extent_table_t *LAST_TABLE; /* table_ready で受け取った表(テストが持つ。次の受け取りか teardown で unref) */
static const char *SIG = "CD001";      /* gen_byte が、セクタ 16 のバイト 1〜5 に置く文字列(5 文字) */

/* ---- フック、アロケータ、時計 ---------------------------------------------------------- */

static void hook_view(void *ctx) {
    (void)ctx;
    VIEW_CALLS++;
}

static void hook_table(void *ctx, const char *id, iso_extent_table_t *t) {
    (void)ctx;
    if (LAST_TABLE) iso_extent_table_unref(LAST_TABLE);
    LAST_TABLE = t;
    TABLES_READY++;
    snprintf(TABLE_ID, sizeof(TABLE_ID), "%s", id);
}

static void *alloc_fn(void *ctx, size_t n) {
    (void)ctx;
    ALLOC_CALLS++;
    if (ALLOC_FAIL_AT > 0 && ALLOC_CALLS == ALLOC_FAIL_AT) return NULL;
    void *p = malloc(n ? n : 1);
    if (p) ALLOC_LIVE++;
    return p;
}

static void free_fn(void *ctx, void *p) {
    (void)ctx;
    if (p) ALLOC_LIVE--;
    free(p);
}

static uint64_t clock_fn(void *ctx) {
    (void)ctx;
    return NOW_MS;
}

static uint32_t epoch_fn(void *ctx) {
    (void)ctx;
    return EPOCH;
}

/* ---- 準備と後始末 ------------------------------------------------------------------------ */

static inline void fx_up_cfg(iso_upload_cfg_t *cfg, uint32_t chunk, uint32_t sync) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cat = &CAT;
    cfg->bdev = &BDEV;
    cfg->clock = &CLK;
    cfg->sha = &SHA;
    cfg->sha_state = &SHA_STATE;
    cfg->io_buf = IOBUF;
    cfg->io_buf_size = sizeof(IOBUF);
    cfg->alloc = &ALLOC;
    cfg->chunk_bytes = chunk;
    cfg->sync_bytes = sync;
    cfg->hooks.view_changed = hook_view;
    cfg->hooks.table_ready = hook_table;
}

/* /iso と /iso/import がある、空き 60 GiB / 全体 64 GiB のカード。chunk / sync = 0 は既定値 */
static inline void fx_base(void) {
    mock_fs_init(&FS);
    IFS = mock_fs_iface(&FS);
    uint32_t seq[64];
    for (int i = 0; i < 64; i++) seq[i] = 0x11110001u + (uint32_t)i;
    mock_rnd_set(&RND, seq, 64);
    IRND = mock_rnd_iface(&RND);
    iso_catalog_init(&CAT, &IFS, &IRND);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/import"));
    mock_fs_set_mount_info(&FS, 64 * GIB, 60 * GIB, 32768);
    NOW_MS = 1000000;
    CLK.ctx = NULL;
    CLK.now_ms = clock_fn;
    SHA = up_sha256_iface();
    ALLOC.ctx = NULL;
    ALLOC.alloc = alloc_fn;
    ALLOC.free = free_fn;
    ALLOC_LIVE = 0;
    ALLOC_CALLS = 0;
    ALLOC_FAIL_AT = 0;
    EPOCH = 7;
    memset(&BDEV, 0, sizeof(BDEV));
    BDEV.epoch = epoch_fn;
    VIEW_CALLS = 0;
    TABLES_READY = 0;
    TABLE_ID[0] = '\0';
    LAST_TABLE = NULL;
    SIG = "CD001";
}

static inline void fx_up_setup(uint32_t chunk, uint32_t sync) {
    fx_base();
    iso_upload_cfg_t cfg;
    fx_up_cfg(&cfg, chunk, sync);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_init(&UP, &cfg));
    iso_upload_set_free(&UP, 64 * GIB, 60 * GIB);
}

static inline void fx_up_teardown(void) {
    iso_upload_reinit(&UP); /* 開いたままのハンドルは、ここで閉じる(テストの本体で、開いたままの数を検査する) */
    if (LAST_TABLE) iso_extent_table_unref(LAST_TABLE);
    LAST_TABLE = NULL;
    int live = ALLOC_LIVE;
    int open_files = FS.open_files, open_dirs = FS.open_dirs;
    mock_fs_free(&FS);
    TEST_ASSERT_EQUAL_INT(0, live); /* 作業領域と表の漏れがない */
    TEST_ASSERT_EQUAL_INT(0, open_files);
    TEST_ASSERT_EQUAL_INT(0, open_dirs);
}

/* 準備で書いたものを、記録と電源断の数から外す */
static inline void fx_settle(void) {
    mock_fs_clear_log(&FS);
    mock_fs_reset_counter(&FS);
}

/* ---- データ ----------------------------------------------------------------------------- */

/* 画像の off バイト目。セクタ 16 (32768) のバイト 1〜5 は SIG */
static inline uint8_t gen_byte(uint64_t off) {
    if (off >= 32769 && off <= 32773) return (uint8_t)SIG[off - 32769];
    uint32_t x = (uint32_t)off * 2654435761u + (uint32_t)(off >> 32) * 40503u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (uint8_t)x;
}

static inline void gen_fill(uint8_t *buf, uint64_t off, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = gen_byte(off + i);
}

/* 画像の先頭 size バイトの SHA-256(一括ではなく 4 KiB ずつ。チャンクの分け方とは無関係) */
static inline void gen_sha(uint64_t size, uint8_t out[32]) {
    up_sha256_state_t s;
    uint8_t buf[4096];
    up_sha256_init(&s);
    for (uint64_t off = 0; off < size; off += sizeof(buf)) {
        size_t n = (size - off < sizeof(buf)) ? (size_t)(size - off) : sizeof(buf);
        gen_fill(buf, off, n);
        up_sha256_update(&s, buf, n);
    }
    up_sha256_final(&s, out);
}

/* 16 進(64 桁)を 32 バイトに(手書きの期待値用) */
static inline void hex32(const char *hex, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* ---- 既存のイメージを置く(カタログのスキャンの前) -------------------------------------------- */

static inline iso_meta_t mk_meta(const char *name, uint64_t size, uint32_t pb, uint8_t state, uint32_t gen) {
    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = state;
    m.size_bytes = size;
    m.part_blocks = pb ? pb : ISO_PART_BLOCKS_DEFAULT;
    m.part_count = (uint16_t)iso_extent_part_count(size, m.part_blocks);
    m.name_len = (uint8_t)strlen(name);
    memcpy(m.name, name, m.name_len);
    m.generation = gen;
    return m;
}

/* <id> のディレクトリと meta.a を置く(meta.a だけ。世代 gen) */
static inline void put_meta(const char *id, const iso_meta_t *m) {
    char dir[ISO_PATH_MAX], path[ISO_PATH_MAX];
    snprintf(dir, sizeof(dir), "/iso/%s", id);
    if (!mock_fs_exists(&FS, dir)) TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, dir));
    snprintf(path, sizeof(path), "/iso/%s/meta.a", id);
    uint8_t buf[ISO_META_SIZE];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(m, buf));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, path, buf, sizeof(buf), 0));
}

/* ディスク上の、画像の先頭 n バイトを、パーツのファイルとして置く(中身は gen_byte。パーツの大きさ pb ブロック) */
static inline void put_parts_data(const char *id, uint32_t pb, uint64_t n) {
    const uint64_t pbytes = (uint64_t)pb * 2048ull;
    static uint8_t buf[1024 * 1024];
    for (unsigned i = 0; n > 0; i++) {
        uint64_t take = n < pbytes ? n : pbytes;
        char path[ISO_PATH_MAX];
        snprintf(path, sizeof(path), "/iso/%s/p%03u", id, i);
        uint64_t base = (uint64_t)i * pbytes;
        TEST_ASSERT_TRUE(take <= sizeof(buf));
        gen_fill(buf, base, (size_t)take);
        TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, path, buf, (size_t)take, 0));
        n -= take;
    }
}

/* ---- アップロードの補助 ------------------------------------------------------------------- */

/* begin して、パーツの大きさを pb ブロックにする(pb == 0 は既定のまま)。meta を 1 回書き直す(世代 +1) */
static inline void img_begin_pb(const char *name, uint64_t size, uint32_t pb, const uint8_t *sha, char id[ISO_ID_LEN + 1]) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, name, strlen(name), size, sha, id));
    if (pb == 0) return;
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
    TEST_ASSERT_NOT_NULL(e);
    iso_meta_t m = e->meta;
    m.part_blocks = pb;
    m.part_count = (uint16_t)iso_extent_part_count(size, pb);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_write_meta(&CAT, e, &m));
}

/* [from, from + n) を 1 つのチャンクで送る。戻り値は iso_upload_write_chunk のもの。n <= 1 MiB */
static inline int put_range(const char *id, uint64_t from, size_t n, iso_upload_write_t *out) {
    static uint8_t buf[1024 * 1024];
    gen_fill(buf, from, n);
    return iso_upload_write_chunk(&UP, id, from, buf, n, out);
}

/* [from, to) を chunk バイトずつ送る。すべて ISO_OK でなければ失敗 */
static inline void put_all(const char *id, uint64_t from, uint64_t to, size_t chunk) {
    for (uint64_t off = from; off < to;) {
        size_t n = (to - off < chunk) ? (size_t)(to - off) : chunk;
        iso_upload_write_t w;
        TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id, off, n, &w));
        off += n;
        TEST_ASSERT_EQUAL_UINT64(off, w.received);
    }
}

static inline void part_path(char *out, size_t cap, const char *id, unsigned idx) { snprintf(out, cap, "/iso/%s/p%03u", id, idx); }

/* ディスク(モック)の、パーツのファイルの中身が、画像の先頭 expect バイトと一致するか。パーツの大きさは pb ブロック。
 * 先頭から順に、パーツの大きさに満たないところで止まっていること(それ以降のパーツは、大きさ 0 かない)も見る。 */
static inline void expect_disk_prefix(const char *id, uint32_t pb, uint64_t expect) {
    const uint64_t pbytes = (uint64_t)pb * 2048ull;
    uint64_t seen = 0;
    for (unsigned i = 0; i < ISO_MAX_PARTS; i++) {
        char path[ISO_PATH_MAX];
        part_path(path, sizeof(path), id, i);
        int64_t sz = mock_fs_size(&FS, path);
        if (sz < 0) break;
        size_t dl = 0;
        const uint8_t *d = mock_fs_data(&FS, path, &dl);
        TEST_ASSERT_TRUE((uint64_t)sz <= pbytes);
        for (int64_t k = 0; k < sz; k++) {
            if (!d || (size_t)k >= dl || d[k] != gen_byte(seen + (uint64_t)k)) TEST_FAIL_MESSAGE("part content differs from the source");
        }
        seen += (uint64_t)sz;
        if ((uint64_t)sz < pbytes) break;
    }
    TEST_ASSERT_EQUAL_UINT64(expect, seen);
}

#endif /* UP_COMMON_H */
