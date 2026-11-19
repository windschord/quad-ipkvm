#include "iso_extent.h"

#include <string.h>

typedef struct {
    uint32_t first_extent; /* extents[] の中の先頭の位置 */
    uint32_t extent_count; /* クリップ後の個数 */
} part_t;

struct iso_extent_table {
    iso_allocator_t alloc;
    uint32_t refs;
    uint32_t epoch;
    uint64_t size_bytes;
    uint32_t part_blocks;
    uint32_t n_parts;
    uint32_t n_extents;
    uint32_t reserved;
    part_t *parts;
    iso_extent_t *extents;
};

/* ---- 純粋な計算 ---------------------------------------------------------- */

uint64_t iso_extent_capacity_blocks(uint64_t size_bytes) {
    uint64_t q = size_bytes / ISO_CD_BLOCK_BYTES;
    return (size_bytes % ISO_CD_BLOCK_BYTES) ? q + 1 : q;
}

uint64_t iso_extent_file_sectors(uint64_t bytes) {
    uint64_t q = bytes / ISO_SECTOR_BYTES;
    return (bytes % ISO_SECTOR_BYTES) ? q + 1 : q;
}

uint64_t iso_extent_part_count(uint64_t size_bytes, uint32_t part_blocks) {
    if (size_bytes == 0 || part_blocks == 0) return 0;
    uint64_t pb = (uint64_t)part_blocks * ISO_CD_BLOCK_BYTES;
    uint64_t q = size_bytes / pb;
    return (size_bytes % pb) ? q + 1 : q;
}

uint64_t iso_extent_part_bytes(uint64_t size_bytes, uint32_t part_blocks, uint32_t index) {
    uint64_t n = iso_extent_part_count(size_bytes, part_blocks);
    if (index >= n) return 0;
    uint64_t pb = (uint64_t)part_blocks * ISO_CD_BLOCK_BYTES;
    if (index + 1 < n) return pb;
    return size_bytes - (n - 1) * pb;
}

/* ---- 表の構築 ------------------------------------------------------------ */

/* part のエクステントを、need セクタまでにクリップして数える(emit != NULL なら書き出す)。
 * 戻り値: 0 = OK、負 = iso_ext_err_t。*kept にクリップ後の個数。 */
static int clip_part(const iso_part_input_t *in, uint64_t need, iso_extent_t *emit, uint32_t *kept) {
    uint64_t taken = 0;
    uint32_t k = 0;
    for (size_t i = 0; i < in->n_extents && taken < need; i++) {
        const iso_extent_t *e = &in->extents[i];
        if (e->sector_count == 0) continue;
        if (e->start_sector > UINT64_MAX - e->sector_count) return ISO_EXT_ERR_INVALID;
        uint64_t n = e->sector_count;
        if (n > need - taken) n = need - taken;
        if (emit) {
            emit[k].start_sector = e->start_sector;
            emit[k].sector_count = n;
        }
        k++;
        taken += n;
    }
    *kept = k;
    if (taken < need) return ISO_EXT_ERR_PART_MISMATCH;
    return ISO_EXT_OK;
}

int iso_extent_table_build(const iso_allocator_t *alloc, uint32_t epoch, uint64_t size_bytes, uint32_t part_blocks,
                           const iso_part_input_t *parts, size_t n_parts, iso_extent_table_t **out) {
    if (!out) return ISO_EXT_ERR_INVALID;
    *out = NULL;
    if (!alloc || !alloc->alloc || !alloc->free || !parts) return ISO_EXT_ERR_INVALID;
    if (size_bytes == 0 || size_bytes > ISO_IMAGE_BYTES_MAX) return ISO_EXT_ERR_INVALID;
    if (part_blocks == 0 || part_blocks > ISO_PART_BLOCKS_MAX) return ISO_EXT_ERR_INVALID;

    uint64_t expect_parts = iso_extent_part_count(size_bytes, part_blocks);
    if (expect_parts > ISO_MAX_PARTS) return ISO_EXT_ERR_INVALID;
    if (n_parts != expect_parts) return ISO_EXT_ERR_PART_MISMATCH;

    /* 1 回目: 検査と個数の集計(何も確保しない) */
    uint32_t total = 0;
    for (uint32_t i = 0; i < n_parts; i++) {
        uint64_t expect_bytes = iso_extent_part_bytes(size_bytes, part_blocks, i);
        if (parts[i].file_bytes != expect_bytes) return ISO_EXT_ERR_PART_MISMATCH;
        if (parts[i].n_extents > 0 && !parts[i].extents) return ISO_EXT_ERR_INVALID;
        uint32_t kept = 0;
        int rc = clip_part(&parts[i], iso_extent_file_sectors(expect_bytes), NULL, &kept);
        if (rc != ISO_EXT_OK) return rc;
        if (kept > ISO_MAX_EXTENTS - total) return ISO_EXT_ERR_TOO_FRAGMENTED;
        total += kept;
    }

    size_t parts_off = (sizeof(struct iso_extent_table) + 7u) & ~(size_t)7u;
    size_t ext_off = parts_off + (((size_t)n_parts * sizeof(part_t) + 7u) & ~(size_t)7u);
    size_t bytes = ext_off + (size_t)total * sizeof(iso_extent_t);
    uint8_t *base = (uint8_t *)alloc->alloc(alloc->ctx, bytes);
    if (!base) return ISO_EXT_ERR_NOMEM;

    iso_extent_table_t *t = (iso_extent_table_t *)(void *)base;
    memset(t, 0, sizeof(*t));
    t->alloc = *alloc;
    t->refs = 1;
    t->epoch = epoch;
    t->size_bytes = size_bytes;
    t->part_blocks = part_blocks;
    t->n_parts = (uint32_t)n_parts;
    t->n_extents = total;
    t->parts = (part_t *)(void *)(base + parts_off);
    t->extents = (iso_extent_t *)(void *)(base + ext_off);

    /* 2 回目: 書き出し(1 回目で検査済み) */
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n_parts; i++) {
        uint32_t kept = 0;
        uint64_t need = iso_extent_file_sectors(iso_extent_part_bytes(size_bytes, part_blocks, i));
        (void)clip_part(&parts[i], need, t->extents + pos, &kept);
        t->parts[i].first_extent = pos;
        t->parts[i].extent_count = kept;
        pos += kept;
    }
    *out = t;
    return ISO_EXT_OK;
}

void iso_extent_table_ref(iso_extent_table_t *t) {
    if (t) t->refs++;
}

uint32_t iso_extent_table_unref(iso_extent_table_t *t) {
    if (!t || t->refs == 0) return 0;
    t->refs--;
    uint32_t left = t->refs;
    if (left == 0) {
        iso_allocator_t a = t->alloc;
        a.free(a.ctx, t);
    }
    return left;
}

uint32_t iso_extent_table_refs(const iso_extent_table_t *t) {
    return t ? t->refs : 0;
}

uint32_t iso_extent_table_epoch(const iso_extent_table_t *t) {
    return t ? t->epoch : 0;
}

uint64_t iso_extent_table_size_bytes(const iso_extent_table_t *t) {
    return t ? t->size_bytes : 0;
}

uint64_t iso_extent_table_capacity_blocks(const iso_extent_table_t *t) {
    return t ? iso_extent_capacity_blocks(t->size_bytes) : 0;
}

uint32_t iso_extent_table_part_count(const iso_extent_table_t *t) {
    return t ? t->n_parts : 0;
}

uint32_t iso_extent_table_part_blocks(const iso_extent_table_t *t) {
    return t ? t->part_blocks : 0;
}

uint32_t iso_extent_table_extent_count(const iso_extent_table_t *t) {
    return t ? t->n_extents : 0;
}

/* ---- 変換 ---------------------------------------------------------------- */

int iso_extent_map(const iso_extent_table_t *t, uint64_t lba, uint32_t blocks, iso_extent_t *spans, size_t max_spans,
                   size_t *n_spans) {
    if (!n_spans) return ISO_EXT_ERR_INVALID;
    *n_spans = 0;
    if (!t || (!spans && max_spans > 0)) return ISO_EXT_ERR_INVALID;

    uint64_t cap = iso_extent_capacity_blocks(t->size_bytes);
    if (lba >= cap) return ISO_EXT_ERR_RANGE;
    if (blocks > cap - lba) return ISO_EXT_ERR_RANGE;
    if (blocks == 0) return ISO_EXT_OK;

    uint64_t first = lba * ISO_SECTORS_PER_BLOCK;
    uint64_t want = (uint64_t)blocks * ISO_SECTORS_PER_BLOCK;
    uint64_t avail = iso_extent_file_sectors(t->size_bytes) - first; /* lba < cap なので 1 以上 */
    uint64_t remaining = want < avail ? want : avail;

    uint64_t part_sectors = (uint64_t)t->part_blocks * ISO_SECTORS_PER_BLOCK;
    uint64_t p = first / part_sectors;
    uint64_t local = first % part_sectors;
    if (p >= t->n_parts) return ISO_EXT_ERR_INVALID;

    /* p 番目のパーツの中で、local セクタ目を含むエクステントを探す */
    uint32_t ei = t->parts[p].first_extent;
    uint32_t end = ei + t->parts[p].extent_count;
    uint64_t base = 0;
    while (ei < end && local >= base + t->extents[ei].sector_count) {
        base += t->extents[ei].sector_count;
        ei++;
    }
    if (ei >= end) return ISO_EXT_ERR_INVALID;
    uint64_t off = local - base;

    size_t n = 0;
    while (remaining > 0) {
        const iso_extent_t *e = &t->extents[ei];
        uint64_t take = e->sector_count - off;
        if (take > remaining) take = remaining;
        uint64_t start = e->start_sector + off;
        if (n > 0 && spans[n - 1].start_sector + spans[n - 1].sector_count == start) {
            spans[n - 1].sector_count += take;
        } else {
            if (n == max_spans) {
                *n_spans = 0;
                return ISO_EXT_ERR_SPANS_FULL;
            }
            spans[n].start_sector = start;
            spans[n].sector_count = take;
            n++;
        }
        remaining -= take;
        off = 0;
        if (remaining == 0) break;
        ei++;
        if (ei >= end) {
            /* 次のパーツの先頭へ */
            p++;
            if (p >= t->n_parts) {
                *n_spans = 0;
                return ISO_EXT_ERR_INVALID;
            }
            ei = t->parts[p].first_extent;
            end = ei + t->parts[p].extent_count;
            if (ei >= end) {
                *n_spans = 0;
                return ISO_EXT_ERR_INVALID;
            }
        }
    }
    *n_spans = n;
    return ISO_EXT_OK;
}

uint64_t iso_extent_valid_bytes(const iso_extent_table_t *t, uint64_t lba, uint32_t blocks) {
    if (!t) return 0;
    if (lba >= iso_extent_capacity_blocks(t->size_bytes)) return 0;
    uint64_t start = lba * ISO_CD_BLOCK_BYTES;
    if (start >= t->size_bytes) return 0;
    uint64_t len = (uint64_t)blocks * ISO_CD_BLOCK_BYTES;
    uint64_t rem = t->size_bytes - start;
    return len < rem ? len : rem;
}
