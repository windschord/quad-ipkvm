#ifndef ISO_EXTENT_H
#define ISO_EXTENT_H

/*
 * iso_extent: CD のブロック (2048B) の範囲 -> 連続した SD のセクタ (512B) の区間(スパン)の一覧。純 C、IDF 非依存。
 * 契約: docs/ISO_MOUNT_DESIGN.md 3.1 / 4.1 / 9、docs/INTERFACES.md 7。
 *
 * 構成
 *   イメージは、固定の大きさのパーツ(既定 1 GiB = 524288 ブロック。最後だけ短い)に分割して FAT32 に置かれている。
 *   パーツごとに、SD のエクステントの一覧 (iso_extent_t = FatFs の CLMT から作った {開始セクタ, 数}) がある。
 *   iso_extent_table_build() が、パーツごとの get_extents の結果から「表」を作る(検証つき)。
 *   iso_extent_map() が、CD のブロックの範囲を、SD のセクタの区間の一覧に変換する。
 *
 * 規則
 *   - 1 CD ブロック = 4 セクタ。範囲が、パーツの境界、エクステントの境界をまたぐときは分割し、
 *     隣り合って連続する区間は併合する(パーツをまたいでも、SD 上で連続なら 1 つにする)。
 *   - LBA >= capacity_blocks、または lba + blocks > capacity_blocks は拒否する (ISO_EXT_ERR_RANGE)。
 *   - エクステントの合計は、パーツのファイルのセクタ数 (ceil(ファイルの大きさ / 512)) でクリップする
 *     (FatFs の連鎖は、クラッシュ後の孤児の末尾クラスタまで列挙しうる。設計 3.1)。
 *   - size_bytes (実バイト数) と capacity_blocks = ceil(size_bytes / 2048) は別。size_bytes が 2048 の倍数でないとき、
 *     最後のブロックの size_bytes 以降は 0 を返す。**ゼロ埋めは mmc_scsi_read_blocks の仕事**。iso_extent_map() が返す区間は、
 *     ファイルのセクタ (ceil(size_bytes / 512) まで) だけで、範囲のうち size_bytes 以降の部分には区間がない
 *     (区間の合計 = ceil(iso_extent_valid_bytes() / 512))。呼び出し側は、区間を順に読んで連結し、
 *     iso_extent_valid_bytes() バイト目以降を 0 にする。ファイルの最後のセクタの、EOF より後ろのバイトは 0 とは限らない。
 *   - 表は、作成時のカードの epoch を持つ。参照カウント(ref / unref)を持ち、0 になったら解放する(遅延解放)。
 *   - メモリの確保は、アロケータを関数ポインタで注入する(ホストテストでは malloc、IDF では PSRAM)。
 *   - 表の参照カウント (ref / unref / refs) は原子的で、どのタスクからも呼べる (C11 atomic。mmc_scsi の遅延した release は、
 *     iso_msc_task から呼ばれうる)。表の中身は作成後に変わらないので、参照を持つ間は、ロックなしで iso_extent_map できる。
 *     表の作成 (build) は、呼び出し側が iso_op_mutex などで守る。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iso_seams.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISO_CD_BLOCK_BYTES 2048u
#define ISO_SECTOR_BYTES 512u
#define ISO_SECTORS_PER_BLOCK 4u /* ISO_CD_BLOCK_BYTES / ISO_SECTOR_BYTES */

#ifndef ISO_MAX_PARTS
#define ISO_MAX_PARTS 16u /* Kconfig ISO_MAX_PARTS */
#endif
#ifndef ISO_MAX_EXTENTS
#define ISO_MAX_EXTENTS 4096u /* Kconfig ISO_MAX_EXTENTS。イメージ全体 */
#endif

/* パーツの既定の大きさ(ブロック数)。1 GiB = 524288 * 2048 */
#define ISO_PART_BLOCKS_DEFAULT 524288u
/* パーツの大きさの上限(ブロック数)。FAT32 の 1 ファイルの上限 (4 GiB - 1) を 2048 で切り上げた値。import は 1 パーツで、そのファイルの大きさ */
#define ISO_PART_BLOCKS_MAX 2097152u
/* イメージの最大の大きさ(バイト)。パーツ ISO_MAX_PARTS 個 x 1 GiB。READ CAPACITY(10) の 32 ビットに収まる */
#define ISO_IMAGE_BYTES_MAX ((uint64_t)ISO_MAX_PARTS * ISO_PART_BLOCKS_DEFAULT * ISO_CD_BLOCK_BYTES)

typedef enum {
    ISO_EXT_OK = 0,
    ISO_EXT_ERR_INVALID = -1,         /* 引数が不正 */
    ISO_EXT_ERR_NOMEM = -2,           /* アロケータが NULL を返した */
    ISO_EXT_ERR_RANGE = -3,           /* LBA が capacity_blocks 以上、または範囲が capacity_blocks を超える */
    ISO_EXT_ERR_TOO_FRAGMENTED = -4,  /* エクステントの数が ISO_MAX_EXTENTS を超える (invalid / too_fragmented) */
    ISO_EXT_ERR_PART_MISMATCH = -5,   /* パーツの数・大きさが期待と違う、またはエクステントがファイルを覆わない (invalid / part_mismatch) */
    ISO_EXT_ERR_SPANS_FULL = -6       /* 出力の配列 (max_spans) が足りない。呼び出し側は、ブロック数を減らして呼び直す。
                                       * 1 ブロックは最悪 ISO_SECTORS_PER_BLOCK (4) 個の区間になる(エクステントが 1 セクタずつ離れている場合)ので、
                                       * 出力の配列は、少なくともそれだけ確保する。1 ブロックでも足りないなら、ブロック数を減らしても進まないので、
                                       * 配列を大きくするか、エラーとして扱う */
} iso_ext_err_t;

/* アロケータ(注入)。free は alloc の対。 */
typedef struct {
    void *ctx;
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *p);
} iso_allocator_t;

/* ---- 純粋な計算 ---------------------------------------------------------- */

/* ceil(size_bytes / 2048)。READ CAPACITY が返すブロック数。 */
uint64_t iso_extent_capacity_blocks(uint64_t size_bytes);
/* ceil(bytes / 512)。ファイルのセクタ数。 */
uint64_t iso_extent_file_sectors(uint64_t bytes);
/* パーツの数 = ceil(size_bytes / (part_blocks * 2048))。size_bytes == 0 または part_blocks == 0 のときは 0。 */
uint64_t iso_extent_part_count(uint64_t size_bytes, uint32_t part_blocks);
/* パーツ index の、期待するファイルの大きさ(バイト)。最後以外は part_blocks * 2048、最後は残り。範囲外の index は 0。 */
uint64_t iso_extent_part_bytes(uint64_t size_bytes, uint32_t part_blocks, uint32_t index);

/* ---- 表 ------------------------------------------------------------------ */

typedef struct iso_extent_table iso_extent_table_t; /* 不透明 */

/* パーツ 1 つの入力。file_bytes は stat の大きさ、extents は get_extents の結果(n_extents 個)。 */
typedef struct {
    uint64_t file_bytes;
    const iso_extent_t *extents;
    size_t n_extents;
} iso_part_input_t;

/*
 * 表を作る。参照カウントは 1 (カタログの分)。
 *   検査(この順。パーツごとに、先頭から):
 *     - 引数。size_bytes は 1..ISO_IMAGE_BYTES_MAX、part_blocks は 1..ISO_PART_BLOCKS_MAX、期待するパーツ数は ISO_MAX_PARTS 以下 -> INVALID
 *     - n_parts が期待するパーツ数 (iso_extent_part_count) と違う -> PART_MISMATCH
 *     - パーツ i の file_bytes が期待する大きさ (iso_extent_part_bytes) と違う -> PART_MISMATCH
 *     - エクステントを先頭から、ファイルのセクタ数でクリップして数える(sector_count 0 は捨てる)。クリップ後に覆えるセクタが
 *       ファイルのセクタ数に満たない -> PART_MISMATCH。全体の個数が ISO_MAX_EXTENTS を超える -> TOO_FRAGMENTED
 *       (start_sector + sector_count がオーバーフローするエクステントは INVALID)
 *   失敗したとき、何も確保しない(*out = NULL)(2 回目の書き出しでの失敗は、確保した領域を解放する)。
 *   **入力(parts と、その extents)は、この関数の実行中(alloc の呼び出しを含む)は変えない**。変えると、検査の結果と食い違い、ISO_EXT_ERR_INVALID になる。
 */
int iso_extent_table_build(const iso_allocator_t *alloc, uint32_t epoch, uint64_t size_bytes, uint32_t part_blocks,
                           const iso_part_input_t *parts, size_t n_parts, iso_extent_table_t **out);

/* 参照を 1 つ増やす(NULL は何もしない)。 */
void iso_extent_table_ref(iso_extent_table_t *t);
/* 参照を 1 つ減らし、残りの参照数を返す。0 になったら解放する。NULL は 0 を返す。
 * **呼び出しごとに、呼び出し側が持っている有効な参照を 1 つだけ消費する**。解放した表への unref はできない(二重の unref を
 * 防ぐのは、参照を持つ呼び出し側の責務。mmc_scsi の command_done のように、冪等にする側が自分の状態で守る)。 */
uint32_t iso_extent_table_unref(iso_extent_table_t *t);
/* 参照数(NULL は 0)。DELETE のジョブが「カタログの分だけになるまで待つ」ために見る。 */
uint32_t iso_extent_table_refs(const iso_extent_table_t *t);

/* 表の属性(NULL は 0)。 */
uint32_t iso_extent_table_epoch(const iso_extent_table_t *t);
uint64_t iso_extent_table_size_bytes(const iso_extent_table_t *t);
uint64_t iso_extent_table_capacity_blocks(const iso_extent_table_t *t);
uint32_t iso_extent_table_part_count(const iso_extent_table_t *t);
uint32_t iso_extent_table_part_blocks(const iso_extent_table_t *t);
uint32_t iso_extent_table_extent_count(const iso_extent_table_t *t); /* クリップ後の合計 */

/*
 * CD のブロック [lba, lba + blocks) を、SD の連続したセクタの区間に変換する。
 *   spans: 出力(max_spans 個)。*n_spans に個数。区間は先頭から順(CD の順)。合計 = ceil(valid_bytes / 512) セクタ。
 *   blocks == 0 で lba < capacity_blocks なら、区間 0 個で OK。
 *   エラーのとき *n_spans = 0。
 */
int iso_extent_map(const iso_extent_table_t *t, uint64_t lba, uint32_t blocks, iso_extent_t *spans, size_t max_spans,
                   size_t *n_spans);

/* ブロックの範囲のうち、size_bytes より前のバイト数 (0..blocks * 2048)。範囲外(lba >= capacity)は 0。 */
uint64_t iso_extent_valid_bytes(const iso_extent_table_t *t, uint64_t lba, uint32_t blocks);

#ifdef __cplusplus
}
#endif

#endif /* ISO_EXTENT_H */
