#ifndef ISO_SD_LOGIC_H
#define ISO_SD_LOGIC_H

/*
 * iso_sd_logic: Pure-C host-testable logic for A-13 microSD / FAT32 seams
 * (docs/ISO_MOUNT_DESIGN.md §1, §3, §4; docs/INTERFACES.md §7.2).
 *
 * Host-testable without ESP-IDF or FatFs headers:
 *   - Strict volume-root path validation & FatFs "<drv>:<path>" formatting
 *   - Directory + entry joining without buffer truncation (ISO_PATH_MAX)
 *   - FAT32 + cluster-size (>= 2048 B) mount policy validation
 *   - FAT32 chain walk -> contiguous 512B SD sector spans with cycle guard
 *   - Write-fence epoch checks & numeric FRESULT -> iso_err_t mapping
 *   - Monitor probe backoff scheduling
 *   - Lock-free C11 _Atomic epoch / sd_state / io_fault state tracker
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iso_seams.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISO_SD_SECTOR_BYTES      512u
#define ISO_SD_MIN_CLUSTER_BYTES 2048u

/* Numeric FatFs FRESULT values from ESP-IDF FatFs R0.15 (ff.h:282-303). */
typedef enum {
    ISO_SD_FR_OK = 0,
    ISO_SD_FR_DISK_ERR = 1,
    ISO_SD_FR_INT_ERR = 2,
    ISO_SD_FR_NOT_READY = 3,
    ISO_SD_FR_NO_FILE = 4,
    ISO_SD_FR_NO_PATH = 5,
    ISO_SD_FR_INVALID_NAME = 6,
    ISO_SD_FR_DENIED = 7,
    ISO_SD_FR_EXIST = 8,
    ISO_SD_FR_INVALID_OBJECT = 9,
    ISO_SD_FR_WRITE_PROTECTED = 10,
    ISO_SD_FR_INVALID_DRIVE = 11,
    ISO_SD_FR_NOT_ENABLED = 12,
    ISO_SD_FR_NO_FILESYSTEM = 13,
    ISO_SD_FR_MKFS_ABORTED = 14,
    ISO_SD_FR_TIMEOUT = 15,
    ISO_SD_FR_LOCKED = 16,
    ISO_SD_FR_NOT_ENOUGH_CORE = 17,
    ISO_SD_FR_TOO_MANY_OPEN_FILES = 18,
    ISO_SD_FR_INVALID_PARAMETER = 19,
} iso_sd_fresult_t;

typedef struct {
    _Atomic uint32_t epoch;
    _Atomic int sd_state;
    _Atomic bool io_fault;
} iso_sd_ctrl_state_t;

/* Map FatFs FRESULT (0..19) to iso_err_t. */
int iso_sd_fresult_to_err(int fresult);

/* Return true iff write-fence permits diskio write/sync/trim (mount_epoch != 0 && mount_epoch == current_epoch). */
bool iso_sd_write_fence_allow(uint32_t mount_epoch, uint32_t current_epoch);

/* Map FatFs FRESULT after a filesystem call, preferring ISO_ERR_EPOCH if an epoch change occurred during failure. */
int iso_sd_map_fresult_with_epoch(int fresult, uint32_t expected_epoch, uint32_t current_epoch);

/* Compute monitor re-probe backoff interval in ms (doubles per failure from base_ms [default 2000], capped at 30000). */
uint32_t iso_sd_probe_backoff_ms(uint32_t base_ms, uint32_t consec_failures);

/* Format a volume-root path (e.g. "/iso/a1b2c3d4/p000") into "<drive_digit>:<path>".
 * Rejects NULL, relative paths, control chars, '\', "..", and paths >= ISO_PATH_MAX without truncation. */
int iso_sd_format_fatfs_path(char drive_digit, const char *path, char *out, size_t out_cap);

/* Join directory path and entry name into a normalized volume-root path (< ISO_PATH_MAX). */
int iso_sd_join_path(const char *dir_path, const char *name, char *out, size_t out_cap);

/* Validate FAT32 mount constraints (FAT32, 512B sector, cluster >= 2048B = 4 sectors). */
int iso_sd_validate_mount(bool is_fat32, uint32_t sector_size, uint32_t sectors_per_cluster,
                          uint32_t *out_cluster_bytes);

/* Callback that reads one 32-bit raw FAT32 entry for @p cluster (lower 28 bits are cluster value). */
typedef int (*iso_sd_fat32_read_entry_fn)(void *ctx, uint32_t cluster, uint32_t *out_entry);

/* Walk a FAT32 cluster chain with cycle detection (Brent's algorithm) and cluster bound checks.
 * Rejects cycles, bad clusters (0x0FFFFFF7), and out-of-range entries with ISO_ERR_IO instead of spinning.
 * On ISO_ERR_TOO_FRAGMENTED, stops when exceeding max_out; *out_n holds the count accumulated up to overflow. */
int iso_sd_walk_fat32_extents(uint32_t start_cluster,
                              uint32_t n_fatent,
                              uint64_t database_sector,
                              uint32_t sectors_per_cluster,
                              iso_sd_fat32_read_entry_fn read_entry,
                              void *read_ctx,
                              iso_extent_t *out,
                              size_t max_out,
                              size_t *out_n);

/* Initialize atomic SD control state (epoch=1, sd_state=ISO_SD_NO_CARD, io_fault=false). */
void iso_sd_state_init(iso_sd_ctrl_state_t *st);

/* Initialize atomic SD control state with an explicit non-zero initial epoch. */
void iso_sd_state_init_with_epoch(iso_sd_ctrl_state_t *st, uint32_t initial_epoch);

uint32_t iso_sd_state_epoch(const iso_sd_ctrl_state_t *st);
uint32_t iso_sd_state_epoch_bump(iso_sd_ctrl_state_t *st);

iso_sd_state_t iso_sd_state_get(const iso_sd_ctrl_state_t *st);
void iso_sd_state_set(iso_sd_ctrl_state_t *st, iso_sd_state_t s);

void iso_sd_state_latch_io_fault(iso_sd_ctrl_state_t *st);
bool iso_sd_state_take_io_fault(iso_sd_ctrl_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* ISO_SD_LOGIC_H */
