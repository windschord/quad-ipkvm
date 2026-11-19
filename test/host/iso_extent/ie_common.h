#ifndef IE_COMMON_H
#define IE_COMMON_H

/*
 * iso_extent のテスト共通の部品。各テストファイルが include する(実行ファイルごとに別のコピー)。
 * 数え上げ付きのアロケータ(確保・解放の回数、失敗の注入)と、表を作る補助。
 */

#include "unity.h"
#include "iso_extent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int allocs;
    int frees;
    int fail_next; /* > 0: その回数目の alloc を失敗させる(1 = 次の alloc) */
    void *last;
} ie_heap_t;

static ie_heap_t H;

static void *ie_alloc(void *ctx, size_t n) {
    ie_heap_t *h = (ie_heap_t *)ctx;
    if (h->fail_next > 0 && --h->fail_next == 0) return NULL;
    h->allocs++;
    h->last = malloc(n);
    return h->last;
}

static void ie_free(void *ctx, void *p) {
    ie_heap_t *h = (ie_heap_t *)ctx;
    h->frees++;
    if (h->last == p) h->last = NULL;
    free(p);
}

static iso_allocator_t ie_allocator(void) {
    iso_allocator_t a;
    a.ctx = &H;
    a.alloc = ie_alloc;
    a.free = ie_free;
    return a;
}

#define EXT(s, c) {(uint64_t)(s), (uint64_t)(c)}

/*
 * 手計算で期待値を作る基準の表 A(part_blocks = 4 = 16 セクタ / パーツ、3 パーツ):
 *   size_bytes = 2 * 8192 + 6244 = 22628 -> capacity_blocks = 12(11.05 を切り上げ)、ファイルのセクタ数 = 45
 *   パーツ 0 (8192 B = 16 セクタ): [1000,10] [2000,6]
 *   パーツ 1 (8192 B = 16 セクタ): [2006,16]            <- パーツ 0 の末尾 (2000..2005) の直後 = SD 上で連続
 *   パーツ 2 (6244 B = 13 セクタ): [5000,8] [6000,8] [9000,4] -> 13 セクタにクリップされ [5000,8] [6000,5]
 *   クリップ後の合計: 2 + 1 + 2 = 5 個、epoch = 7
 */
#define TA_SIZE 22628u
#define TA_PART_BLOCKS 4u
#define TA_EPOCH 7u

static const iso_extent_t TA_P0[] = {EXT(1000, 10), EXT(2000, 6)};
static const iso_extent_t TA_P1[] = {EXT(2006, 16)};
static const iso_extent_t TA_P2[] = {EXT(5000, 8), EXT(6000, 8), EXT(9000, 4)};

static inline iso_extent_table_t *build_table_a(void) {
    iso_part_input_t in[3] = {{8192, TA_P0, 2}, {8192, TA_P1, 1}, {6244, TA_P2, 3}};
    iso_allocator_t a = ie_allocator();
    iso_extent_table_t *t = NULL;
    int rc = iso_extent_table_build(&a, TA_EPOCH, TA_SIZE, TA_PART_BLOCKS, in, 3, &t);
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, rc);
    TEST_ASSERT_NOT_NULL(t);
    return t;
}

#endif /* IE_COMMON_H */
