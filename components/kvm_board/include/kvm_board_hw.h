#ifndef KVM_BOARD_HW_H
#define KVM_BOARD_HW_H

/*
 * kvm_board_hw: ESP-IDF hardware driver implementing kvm_board_hal_t using
 * ESP32-P4 GPIO and a 1 MHz GPTimer one-shot watchdog timer.
 *
 * Contract: docs/INTERFACES.md §6.2, safety policy: docs/PLAN.md §4.4.
 */

#include "esp_err.h"
#include "kvm_board.h"
#include "kvm_board_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize GPIO + GPTimer board driver with explicit configuration. */
esp_err_t kvm_board_init(const kvm_board_logic_config_t *cfg);

/*
 * Initialize board driver from CONFIG_KVM_BOARD_* Kconfig symbols.
 * Returns ESP_ERR_NOT_SUPPORTED when CONFIG_KVM_BOARD_ENABLE=n, or
 * ESP_ERR_INVALID_ARG if any GPIO is unassigned (-1) or invalid.
 */
esp_err_t kvm_board_init_from_kconfig(void);

/*
 * Return the singleton HAL handle to pass to kvm_service_start().
 * hal->video_sync is NULL (kvm_service_start() substitutes video_status_hal_sync).
 * Returns NULL until kvm_board_init*() succeeds.
 */
const kvm_board_hal_t *kvm_board_hal(void);

/*
 * Release all active lines, stop the hardware timer, and free driver resources.
 * Precondition: callers must stop all concurrent kvm_board_hal_t users (e.g., kvm_service)
 * before calling kvm_board_deinit().
 */
void kvm_board_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* KVM_BOARD_HW_H */
