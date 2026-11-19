#include "unity.h"
#include "iso_sd_logic.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_format_fatfs_path_and_utf8_boundaries(void) {
    char buf[ISO_PATH_MAX + 8];

    /* Valid root & ISO paths, including UTF-8 Japanese filenames */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_format_fatfs_path('0', "/iso/a1b2c3d4/p000", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("0:/iso/a1b2c3d4/p000", buf);

    TEST_ASSERT_EQUAL_INT(
        ISO_OK,
        iso_sd_format_fatfs_path('1', "/iso/import/日本語のISO.iso", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("1:/iso/import/日本語のISO.iso", buf);

    /* Exact maximum path length (ISO_PATH_MAX - 1 chars + NUL) succeeds */
    char max_path[ISO_PATH_MAX + 1];
    max_path[0] = '/';
    memset(max_path + 1, 'a', ISO_PATH_MAX - 2u);
    max_path[ISO_PATH_MAX - 1u] = '\0';
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_format_fatfs_path('0', max_path, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(ISO_PATH_MAX + 1u), (uint32_t)strlen(buf));

    /* Path reaching ISO_PATH_MAX non-NUL bytes rejects without truncation */
    char over_path[ISO_PATH_MAX + 2];
    over_path[0] = '/';
    memset(over_path + 1, 'b', ISO_PATH_MAX - 1u);
    over_path[ISO_PATH_MAX] = '\0';
    buf[0] = 'X';
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', over_path, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_CHAR('X', buf[0]);

    /* Reject relative path, bad drive digit, small out_cap, traversal, backslash, control chars */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "iso/a1b2c3d4", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('a', "/iso", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "/iso", buf, 6u));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_format_fatfs_path('0', "/iso", buf, 7u));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "/iso/../etc", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "/iso/..", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "/iso/./meta.a", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "/iso\\bad", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "/iso/\n", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_format_fatfs_path('0', "/iso", NULL, sizeof(buf)));
}

void test_join_path(void) {
    char out[ISO_PATH_MAX];

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_join_path("/", "iso", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/iso", out);

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_join_path("/iso/import", "ubuntu.iso", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/iso/import/ubuntu.iso", out);

    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_join_path("/iso/import", ".", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_join_path("/iso/import", "..", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_join_path("/iso/import", "a/b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_join_path("/iso/import", "", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_join_path(NULL, "a", out, sizeof(out)));
}

void test_validate_mount_policy(void) {
    uint32_t cb = 0u;

    /* Valid FAT32 with cluster == 2048B (4 sectors), 4096B, 32768B */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_validate_mount(true, 512u, 4u, &cb));
    TEST_ASSERT_EQUAL_UINT32(2048u, cb);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_validate_mount(true, 512u, 64u, &cb));
    TEST_ASSERT_EQUAL_UINT32(32768u, cb);

    /* Reject non-FAT32 (FAT12/16, exFAT) */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_validate_mount(false, 512u, 64u, &cb));

    /* Reject clusters smaller than 1 CD block (< 2048B) */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_validate_mount(true, 512u, 1u, &cb));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_validate_mount(true, 512u, 2u, &cb));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_validate_mount(true, 512u, 3u, &cb));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_validate_mount(true, 512u, 0u, &cb));

    /* Reject non-512B sector size & overflow */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_validate_mount(true, 4096u, 1u, &cb));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_validate_mount(true, 512u, 0x01000000u, &cb));
}

void test_clmt_to_extents_conversion_and_limits(void) {
    iso_extent_t out[4];
    size_t n = 99u;

    /* Empty CLMT (0 fragments) */
    const uint32_t empty_clmt[] = {2u, 0u};
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_clmt_to_extents(empty_clmt, 2u, 8192u, 8u, out, 4u, &n));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)n);

    /* Multi-fragment file: database=8192, csize=8 (4 KiB clusters).
     * frag0: ncl=10, tcl=2 -> start = 8192 + (2-2)*8 = 8192, count = 80 sectors
     * frag1: ncl=5,  tcl=20 -> start = 8192 + (20-2)*8 = 8336, count = 40 sectors */
    const uint32_t two_clmt[] = {6u, 10u, 2u, 5u, 20u, 0u};
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_clmt_to_extents(two_clmt, 6u, 8192u, 8u, out, 2u, &n));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)n);
    TEST_ASSERT_EQUAL_UINT64(8192u, out[0].start_sector);
    TEST_ASSERT_EQUAL_UINT64(80u, out[0].sector_count);
    TEST_ASSERT_EQUAL_UINT64(8336u, out[1].start_sector);
    TEST_ASSERT_EQUAL_UINT64(40u, out[1].sector_count);

    /* Insufficient max_out reports required fragment count and ISO_ERR_TOO_FRAGMENTED */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_TOO_FRAGMENTED,
                          iso_sd_clmt_to_extents(two_clmt, 6u, 8192u, 8u, out, 1u, &n));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)n);

    /* Malformed CLMT: cluster < 2, missing terminator, NULL pointers */
    const uint32_t bad_clust[] = {4u, 3u, 1u, 0u};
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_clmt_to_extents(bad_clust, 4u, 8192u, 8u, out, 4u, &n));

    const uint32_t no_term[] = {3u, 3u, 2u};
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_clmt_to_extents(no_term, 3u, 8192u, 8u, out, 4u, &n));

    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_clmt_to_extents(NULL, 6u, 8192u, 8u, out, 4u, &n));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_clmt_to_extents(two_clmt, 6u, 8192u, 8u, out, 4u, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_clmt_to_extents(two_clmt, 6u, 8192u, 8u, NULL, 2u, &n));
}

void test_fresult_mapping_and_atomic_state(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_sd_fresult_to_err(ISO_SD_FR_OK));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, iso_sd_fresult_to_err(ISO_SD_FR_NO_FILE));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, iso_sd_fresult_to_err(ISO_SD_FR_NO_PATH));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_EXISTS, iso_sd_fresult_to_err(ISO_SD_FR_EXIST));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_CARD, iso_sd_fresult_to_err(ISO_SD_FR_NOT_READY));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_UNSUPPORTED_FS, iso_sd_fresult_to_err(ISO_SD_FR_NO_FILESYSTEM));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_sd_fresult_to_err(ISO_SD_FR_INVALID_NAME));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, iso_sd_fresult_to_err(ISO_SD_FR_TIMEOUT));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOMEM, iso_sd_fresult_to_err(ISO_SD_FR_NOT_ENOUGH_CORE));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_sd_fresult_to_err(ISO_SD_FR_DISK_ERR));

    iso_sd_ctrl_state_t st;
    iso_sd_state_init(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_sd_state_epoch(&st));
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, iso_sd_state_get(&st));
    TEST_ASSERT_FALSE(iso_sd_state_take_io_fault(&st));

    TEST_ASSERT_EQUAL_UINT32(2u, iso_sd_state_epoch_bump(&st));
    TEST_ASSERT_EQUAL_UINT32(2u, iso_sd_state_epoch(&st));

    /* Wraparound skips 0 */
    atomic_store(&st.epoch, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_sd_state_epoch_bump(&st));

    iso_sd_state_set(&st, ISO_SD_READY);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, iso_sd_state_get(&st));

    iso_sd_state_latch_io_fault(&st);
    TEST_ASSERT_TRUE(iso_sd_state_take_io_fault(&st));
    TEST_ASSERT_FALSE(iso_sd_state_take_io_fault(&st));
}

typedef struct {
    const uint32_t *fat;
    size_t n_fatent;
    int inject_err_at_call;
    int inject_err;
    uint32_t calls;
} mock_fat_t;

static int mock_read_fat_entry(void *ctx, uint32_t cluster, uint32_t *out_entry) {
    mock_fat_t *mf = (mock_fat_t *)ctx;
    if (mf->inject_err_at_call >= 0 && mf->calls == (uint32_t)mf->inject_err_at_call) {
        return mf->inject_err;
    }
    mf->calls++;
    if (cluster >= mf->n_fatent) {
        return ISO_ERR_IO;
    }
    *out_entry = mf->fat[cluster];
    return ISO_OK;
}

void test_walk_fat32_extents_and_cycle_guard(void) {
    iso_extent_t out[4];
    size_t n = 99u;

    /* Valid 2-run chain spanning all 6 data clusters: 2 -> 3 -> 4, then 7 -> 6 -> 5 -> EOC */
    uint32_t fat[10] = {
        [0] = 0x0FFFFFF8u,
        [1] = 0x0FFFFFFFu,
        [2] = 3u,
        [3] = 4u,
        [4] = 7u,
        [5] = 0x0FFFFFFFu,
        [6] = 5u,
        [7] = 6u,
        [8] = 0u,
        [9] = 0u,
    };
    mock_fat_t mf = {.fat = fat, .n_fatent = 10u, .inject_err_at_call = -1};

    /* Empty file */
    TEST_ASSERT_EQUAL_INT(
        ISO_OK, iso_sd_walk_fat32_extents(0u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(0u, n);
    TEST_ASSERT_EQUAL_UINT32(0u, mf.calls);

    /* Non-contiguous: 2->3->4 (3 clusters), 7 (1), 6 (1), 5 (1) => 4 extents */
    TEST_ASSERT_EQUAL_INT(
        ISO_OK, iso_sd_walk_fat32_extents(2u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(4u, n);
    TEST_ASSERT_EQUAL_UINT64(8192u, out[0].start_sector);
    TEST_ASSERT_EQUAL_UINT32(24u, out[0].sector_count);
    TEST_ASSERT_EQUAL_UINT64(8192u + 5u * 8u, out[1].start_sector);
    TEST_ASSERT_EQUAL_UINT32(8u, out[1].sector_count);
    TEST_ASSERT_EQUAL_UINT64(8192u + 4u * 8u, out[2].start_sector);
    TEST_ASSERT_EQUAL_UINT32(8u, out[2].sector_count);
    TEST_ASSERT_EQUAL_UINT64(8192u + 3u * 8u, out[3].start_sector);
    TEST_ASSERT_EQUAL_UINT32(8u, out[3].sector_count);

    /* Too fragmented when max_out < needed runs */
    TEST_ASSERT_EQUAL_INT(
        ISO_ERR_TOO_FRAGMENTED,
        iso_sd_walk_fat32_extents(2u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 2u, &n));

    /* Self-loop cycle: 5 -> 5 */
    fat[5] = 0x10000005u; /* high 4 reserved bits must be masked cleanly */
    mf.calls = 0u;
    TEST_ASSERT_EQUAL_INT(
        ISO_ERR_IO,
        iso_sd_walk_fat32_extents(5u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(0u, n);
    TEST_ASSERT_EQUAL_UINT32(1u, mf.calls);

    /* Lollipop cycle: 2 -> 3 -> 4 -> 5 -> 3 */
    fat[2] = 3u;
    fat[3] = 4u;
    fat[4] = 5u;
    fat[5] = 3u;
    mf.calls = 0u;
    TEST_ASSERT_EQUAL_INT(
        ISO_ERR_IO,
        iso_sd_walk_fat32_extents(2u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 4u, &n));
    TEST_ASSERT_TRUE(mf.calls <= 8u);

    /* Bad cluster marker 0x0FFFFFF7 and free cluster 0 rejected as ISO_ERR_IO */
    fat[2] = 0x0FFFFFF7u;
    TEST_ASSERT_EQUAL_INT(
        ISO_ERR_IO,
        iso_sd_walk_fat32_extents(2u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 4u, &n));
    fat[2] = 0u;
    TEST_ASSERT_EQUAL_INT(
        ISO_ERR_IO,
        iso_sd_walk_fat32_extents(2u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 4u, &n));

    /* Propagate epoch error from block reader */
    fat[2] = 3u;
    mf.calls = 0u;
    mf.inject_err_at_call = 0;
    mf.inject_err = ISO_ERR_EPOCH;
    TEST_ASSERT_EQUAL_INT(
        ISO_ERR_EPOCH,
        iso_sd_walk_fat32_extents(2u, 10u, 8192u, 8u, mock_read_fat_entry, &mf, out, 4u, &n));
}

void test_init_with_epoch_seed(void) {
    iso_sd_ctrl_state_t st;
    iso_sd_state_init_with_epoch(&st, 42u);
    TEST_ASSERT_EQUAL_UINT32(42u, iso_sd_state_epoch(&st));
    TEST_ASSERT_EQUAL_UINT32(43u, iso_sd_state_epoch_bump(&st));

    iso_sd_state_init_with_epoch(&st, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_sd_state_epoch(&st));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_format_fatfs_path_and_utf8_boundaries);
    RUN_TEST(test_join_path);
    RUN_TEST(test_validate_mount_policy);
    RUN_TEST(test_clmt_to_extents_conversion_and_limits);
    RUN_TEST(test_fresult_mapping_and_atomic_state);
    RUN_TEST(test_walk_fat32_extents_and_cycle_guard);
    RUN_TEST(test_init_with_epoch_seed);
    return UNITY_END();
}
