#ifndef KVM_BOARD_DRV_H
#define KVM_BOARD_DRV_H

/*
 * kvm_board_drv: Host-testable hardware orchestration layer for kvm_board_hal_t.
 *
 * Enforces hardware safety sequencing across abstract GPIO, GPTimer, and locking ops:
 *   - Glitch-free GPIO output init (preload idle level before enabling output)
 *   - Mutual exclusion checked BEFORE stopping/reading the timer
 *   - 1 MHz timer armed BEFORE driving any active-low E line Low
 *   - Alarm callback rejects premature count_value < alarm_value (delayed ISR guard)
 */

#include <stdbool.h>
#include <stdint.h>
#include "kvm_board.h"
#include "kvm_board_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    int (*gpio_set_level)(void *ctx, int gpio, int level);
    int (*gpio_config_output)(void *ctx, int gpio);
    int (*gpio_config_input)(void *ctx, int gpio);
    int (*gpio_get_level)(void *ctx, int gpio);
    int (*timer_stop)(void *ctx);
    int (*timer_disarm)(void *ctx);
    int (*timer_get_count)(void *ctx, uint64_t *out_us);
    int (*timer_set_count)(void *ctx, uint64_t count_us);
    int (*timer_set_alarm)(void *ctx, uint64_t alarm_us);
    int (*timer_start)(void *ctx);
    /* Optional lock pairs: either both NULL or both non-NULL. */
    int (*mutex_lock)(void *ctx);
    void (*mutex_unlock)(void *ctx);
    void (*irq_lock)(void *ctx);
    void (*irq_unlock)(void *ctx);
} kvm_board_io_t;

typedef struct {
    kvm_board_logic_t logic;
    kvm_board_io_t io;
    kvm_board_hal_t hal;
    bool ready;
    bool timer_running;
} kvm_board_drv_t;

#if defined(__GNUC__) || defined(__clang__)
#define KVM_BOARD_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define KVM_BOARD_ALWAYS_INLINE static inline
#endif

/**
 * @brief Evaluate a timer alarm event under the caller's ISR spinlock.
 *
 * Always inlined so IRAM timer ISRs execute no flash calls.
 */
KVM_BOARD_ALWAYS_INLINE bool kvm_board_drv_eval_alarm(kvm_board_drv_t *drv,
                                                      uint64_t count_value,
                                                      uint64_t alarm_value,
                                                      int *out_release_gpio)
{
    if (!drv || !out_release_gpio || count_value < alarm_value) {
        return false;
    }
    int gpio = -1;
    if (!drv->ready ||
        !kvm_board_logic_on_timer_alarm(&drv->logic, alarm_value, &gpio) ||
        gpio < 0) {
        return false;
    }
    *out_release_gpio = gpio;
    return true;
}

int kvm_board_drv_init(kvm_board_drv_t *drv,
                       const kvm_board_logic_config_t *cfg,
                       const kvm_board_io_t *io);

const kvm_board_hal_t *kvm_board_drv_hal(const kvm_board_drv_t *drv);

/* Callers must stop all concurrent kvm_board_hal_t users before calling deinit. */
void kvm_board_drv_deinit(kvm_board_drv_t *drv);

bool kvm_board_drv_on_alarm(kvm_board_drv_t *drv,
                            uint64_t count_value,
                            uint64_t alarm_value);

#ifdef __cplusplus
}
#endif

#endif /* KVM_BOARD_DRV_H */
