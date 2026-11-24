#ifndef ISO_SD_H
#define ISO_SD_H

/*
 * iso_sd: ESP32-P4 microSD (SDMMC Slot 0 + ESP_LDO_VO4 + PMOS GPIO45) + FAT32 +
 * serialized diskio (sd_io_lock) implementing docs/INTERFACES.md §7.2 seams.
 *
 * Pinout from docs/HW_VERIFY.md I1 (Waveshare ESP32-P4-ETH):
 *   D0=GPIO39, D1=GPIO40, D2=GPIO41, D3=GPIO42, CLK=GPIO43, CMD=GPIO44, PWR_SW=GPIO45 (0=ON, 1=OFF)
 */

#include <stdbool.h>
#include "esp_err.h"
#include "iso_seams.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*iso_sd_event_cb_t)(void *ctx);

typedef struct {
    /* Legacy unmanaged mode (managed_reinit == false): invoked with NO iso_sd locks held
     * when card enters error/removal reinit. May be NULL. */
    iso_sd_event_cb_t on_force_eject;
    void *on_force_eject_ctx;
    /* Legacy unmanaged mode: invoked with NO iso_sd locks held when card mounts cleanly. May be NULL. */
    iso_sd_event_cb_t on_ready;
    void *on_ready_ctx;

    /* Managed mode (for iso_worker): delegate unmount/remount to on_fault / on_reprobe
     * callbacks so iso_worker executes docs/ISO_MOUNT_DESIGN.md §3.4 steps (1)-(7). */
    bool managed_reinit;
    iso_sd_event_cb_t on_fault;
    void *on_fault_ctx;
    iso_sd_event_cb_t on_reprobe;
    void *on_reprobe_ctx;
} iso_sd_config_t;

esp_err_t iso_sd_init(const iso_sd_config_t *cfg);
/* Stop monitor task callbacks before tearing down callers, leaving iso_sd locks intact. */
void iso_sd_stop_monitor(void);
void iso_sd_deinit(void);

/* Explicit mount/unmount/state helpers for managed iso_worker §3.4 sequences. */
int iso_sd_remount(void);
void iso_sd_unmount(void);
void iso_sd_set_state(iso_sd_state_t st);

const iso_blockdev_t *iso_sd_blockdev(void);
const iso_fs_t *iso_sd_fs(void);
const iso_sd_status_t *iso_sd_status(void);
const iso_clock_t *iso_sd_clock(void);

/* Non-destructive hardware/FAT32/CLMT/raw-sector verification (requires mounted FAT32 card). */
esp_err_t iso_sd_run_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* ISO_SD_H */
