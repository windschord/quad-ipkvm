#include "kvm_board_drv.h"

#include <string.h>

static bool io_callbacks_valid(const kvm_board_io_t *io)
{
    if (!io || !io->gpio_set_level || !io->gpio_config_output ||
        !io->gpio_config_input || !io->gpio_get_level ||
        !io->timer_stop || !io->timer_disarm || !io->timer_get_count ||
        !io->timer_set_count || !io->timer_set_alarm || !io->timer_start) {
        return false;
    }
    if ((io->mutex_lock != NULL) != (io->mutex_unlock != NULL)) {
        return false;
    }
    if ((io->irq_lock != NULL) != (io->irq_unlock != NULL)) {
        return false;
    }
    return true;
}

static int drv_mutex_lock(const kvm_board_drv_t *drv)
{
    if (drv->io.mutex_lock) {
        return drv->io.mutex_lock(drv->io.ctx);
    }
    return 0;
}

static void drv_mutex_unlock(const kvm_board_drv_t *drv)
{
    if (drv->io.mutex_unlock) {
        drv->io.mutex_unlock(drv->io.ctx);
    }
}

static void drv_irq_lock(const kvm_board_drv_t *drv)
{
    if (drv->io.irq_lock) {
        drv->io.irq_lock(drv->io.ctx);
    }
}

static void drv_irq_unlock(const kvm_board_drv_t *drv)
{
    if (drv->io.irq_unlock) {
        drv->io.irq_unlock(drv->io.ctx);
    }
}

static int configure_output_pin_list(const kvm_board_io_t *io,
                                     const int *gpios,
                                     size_t count,
                                     int idle_level,
                                     size_t *out_configured)
{
    size_t configured = 0u;
    for (size_t i = 0u; i < count; i++) {
        /* Preload idle output latch before enabling pad output to prevent Low glitches. */
        if (io->gpio_set_level(io->ctx, gpios[i], idle_level) != 0) {
            if (out_configured) {
                *out_configured = configured;
            }
            return -1;
        }
        if (io->gpio_config_output(io->ctx, gpios[i]) != 0) {
            if (out_configured) {
                *out_configured = configured;
            }
            return -1;
        }
        configured++;
        if (io->gpio_set_level(io->ctx, gpios[i], idle_level) != 0) {
            if (out_configured) {
                *out_configured = configured;
            }
            return -1;
        }
    }
    if (out_configured) {
        *out_configured = configured;
    }
    return 0;
}

static void drv_stop_timer_locked(kvm_board_drv_t *drv)
{
    if (drv->timer_running) {
        (void)drv->io.timer_stop(drv->io.ctx);
        drv->timer_running = false;
    }
}

static int drv_release_all_locked(kvm_board_drv_t *drv)
{
    int gpios[KVM_BOARD_NUM_ENABLE_LINES] = { -1, -1, -1 };
    int rc;

    drv_irq_lock(drv);
    rc = kvm_board_logic_release_all(&drv->logic, gpios);
    if (rc == 0) {
        for (size_t i = 0u; i < KVM_BOARD_NUM_ENABLE_LINES; i++) {
            if (gpios[i] >= 0 && drv->io.gpio_set_level(drv->io.ctx, gpios[i], 1) != 0) {
                rc = -1;
            }
        }
    }
    drv_irq_unlock(drv);

    drv_stop_timer_locked(drv);
    (void)drv->io.timer_disarm(drv->io.ctx);
    return rc;
}

static int hal_set_addr(void *ctx, uint8_t ch)
{
    kvm_board_drv_t *drv = (kvm_board_drv_t *)ctx;
    if (!drv || !drv->ready) {
        return -1;
    }
    if (drv_mutex_lock(drv) != 0) {
        return -1;
    }

    bool a0 = false;
    bool a1 = false;
    int rc = -1;
    drv_irq_lock(drv);
    if (drv->ready) {
        uint8_t prev_ch = drv->logic.current_channel;
        rc = kvm_board_logic_begin_set_addr(&drv->logic, ch, &a0, &a1);
        if (rc == 0) {
            if (drv->io.gpio_set_level(drv->io.ctx, drv->logic.config.pins.a0, a0 ? 1 : 0) != 0) {
                drv->logic.current_channel = prev_ch;
                rc = -1;
            } else if (drv->io.gpio_set_level(drv->io.ctx, drv->logic.config.pins.a1, a1 ? 1 : 0) != 0) {
                bool prev_a0 = false;
                if (kvm_board_logic_channel_to_addr(prev_ch, &prev_a0, NULL) == 0 &&
                    drv->io.gpio_set_level(drv->io.ctx, drv->logic.config.pins.a0,
                                           prev_a0 ? 1 : 0) == 0) {
                    drv->logic.current_channel = prev_ch;
                } else {
                    /* Both A1 write and best-effort A0 rollback failed: mark channel unknown. */
                    drv->logic.current_channel = KVM_BOARD_CH_UNKNOWN;
                }
                rc = -1;
            }
        }
    }
    drv_irq_unlock(drv);

    drv_mutex_unlock(drv);
    return rc;
}

static int hal_assert_line(void *ctx, kvm_board_line_t line, uint32_t max_ms)
{
    kvm_board_drv_t *drv = (kvm_board_drv_t *)ctx;
    if (!drv || !drv->ready || line >= KVM_BOARD_LINE_COUNT) {
        return -1;
    }
    if (drv_mutex_lock(drv) != 0) {
        return -1;
    }

    /* Check mutual exclusion BEFORE touching the running hardware timer. */
    drv_irq_lock(drv);
    bool can_arm = drv->ready && !drv->logic.has_active_line;
    drv_irq_unlock(drv);
    if (!can_arm) {
        drv_mutex_unlock(drv);
        return -1;
    }

    /* Stop any timer still coasting after an earlier auto-release ISR before reading count. */
    drv_stop_timer_locked(drv);

    uint64_t now_us = 0u;
    if (drv->io.timer_get_count(drv->io.ctx, &now_us) != 0) {
        drv_mutex_unlock(drv);
        return -1;
    }

    uint64_t base_us = 0u;
    uint64_t alarm_us = 0u;
    int gpio = -1;
    drv_irq_lock(drv);
    int rc = drv->ready
                 ? kvm_board_logic_begin_assert(&drv->logic, line, max_ms, now_us,
                                                &base_us, &alarm_us, &gpio)
                 : -1;
    drv_irq_unlock(drv);
    if (rc != 0) {
        drv_mutex_unlock(drv);
        return -1;
    }

    int err = 0;
    if (base_us != now_us) {
        err = drv->io.timer_set_count(drv->io.ctx, base_us);
    }
    if (err == 0) {
        err = drv->io.timer_set_alarm(drv->io.ctx, alarm_us);
    }
    if (err == 0) {
        err = drv->io.timer_start(drv->io.ctx);
        if (err == 0) {
            drv->timer_running = true;
        }
    }
    if (err != 0) {
        (void)drv_release_all_locked(drv);
        drv_mutex_unlock(drv);
        return -1;
    }

    /* Drive active-low only after timer is running and the pulse token is still live. */
    int low_rc = 0;
    drv_irq_lock(drv);
    if (drv->ready && drv->logic.has_active_line && drv->logic.active_alarm_token == alarm_us) {
        low_rc = drv->io.gpio_set_level(drv->io.ctx, gpio, 0);
    }
    drv_irq_unlock(drv);
    if (low_rc != 0) {
        (void)drv_release_all_locked(drv);
        drv_mutex_unlock(drv);
        return -1;
    }

    drv_mutex_unlock(drv);
    return 0;
}

static int hal_release_line(void *ctx, kvm_board_line_t line)
{
    kvm_board_drv_t *drv = (kvm_board_drv_t *)ctx;
    if (!drv || !drv->ready || line >= KVM_BOARD_LINE_COUNT) {
        return -1;
    }
    if (drv_mutex_lock(drv) != 0) {
        return -1;
    }

    bool released = false;
    int gpio = -1;
    int rc = -1;
    drv_irq_lock(drv);
    if (drv->ready) {
        rc = kvm_board_logic_release_line(&drv->logic, line, &released, &gpio);
        if (rc == 0 && released && gpio >= 0 &&
            drv->io.gpio_set_level(drv->io.ctx, gpio, 1) != 0) {
            rc = -1;
        }
    }
    drv_irq_unlock(drv);

    if (released) {
        drv_stop_timer_locked(drv);
        (void)drv->io.timer_disarm(drv->io.ctx);
    }

    drv_mutex_unlock(drv);
    return rc;
}

static int hal_release_all(void *ctx)
{
    kvm_board_drv_t *drv = (kvm_board_drv_t *)ctx;
    if (!drv || !drv->ready) {
        return -1;
    }
    if (drv_mutex_lock(drv) != 0) {
        return -1;
    }

    int rc = drv->ready ? drv_release_all_locked(drv) : -1;
    drv_mutex_unlock(drv);
    return rc;
}

static int hal_read_led_mask(void *ctx, uint8_t *out_mask)
{
    kvm_board_drv_t *drv = (kvm_board_drv_t *)ctx;
    if (!drv || !drv->ready || !out_mask) {
        return -1;
    }
    if (drv_mutex_lock(drv) != 0) {
        return -1;
    }

    int rc = -1;
    if (drv->ready) {
        bool raw[KVM_BOARD_NUM_CHANNELS] = { false, false, false, false };
        rc = 0;
        for (size_t i = 0u; i < KVM_BOARD_NUM_CHANNELS; i++) {
            int lvl = drv->io.gpio_get_level(drv->io.ctx, drv->logic.config.pins.led_ch[i]);
            if (lvl < 0) {
                rc = -1;
                break;
            }
            raw[i] = (lvl != 0);
        }
        if (rc == 0) {
            *out_mask = kvm_board_logic_decode_leds(raw, drv->logic.config.led_active_low);
        }
    }

    drv_mutex_unlock(drv);
    return rc;
}

int kvm_board_drv_init(kvm_board_drv_t *drv,
                       const kvm_board_logic_config_t *cfg,
                       const kvm_board_io_t *io)
{
    if (!drv || !cfg || !io_callbacks_valid(io)) {
        return -1;
    }

    kvm_board_logic_t validated;
    if (kvm_board_logic_init(&validated, cfg) != 0) {
        return -1;
    }

    memset(drv, 0, sizeof(*drv));
    drv->logic = validated;
    drv->io = *io;

    const int enable_pins[KVM_BOARD_NUM_ENABLE_LINES] = {
        cfg->pins.e_power,
        cfg->pins.e_reset,
        cfg->pins.e_kvm,
    };
    const int addr_pins[2] = { cfg->pins.a0, cfg->pins.a1 };

    size_t e_configured = 0u;
    int err = configure_output_pin_list(io, enable_pins, KVM_BOARD_NUM_ENABLE_LINES, 1, &e_configured);
    if (err == 0) {
        err = configure_output_pin_list(io, addr_pins, 2u, 0, NULL);
    }
    if (err == 0) {
        for (size_t i = 0u; i < KVM_BOARD_NUM_CHANNELS; i++) {
            if (io->gpio_config_input(io->ctx, cfg->pins.led_ch[i]) != 0) {
                err = -1;
                break;
            }
        }
    }
    if (err != 0) {
        for (size_t i = 0u; i < e_configured; i++) {
            (void)io->gpio_set_level(io->ctx, enable_pins[i], 1);
        }
        memset(drv, 0, sizeof(*drv));
        return -1;
    }

    drv->hal.ctx = drv;
    drv->hal.set_addr = hal_set_addr;
    drv->hal.assert_line = hal_assert_line;
    drv->hal.release_line = hal_release_line;
    drv->hal.release_all = hal_release_all;
    drv->hal.read_led_mask = hal_read_led_mask;
    drv->hal.video_sync = NULL;
    drv->ready = true;
    return 0;
}

const kvm_board_hal_t *kvm_board_drv_hal(const kvm_board_drv_t *drv)
{
    return (drv && drv->ready) ? &drv->hal : NULL;
}

void kvm_board_drv_deinit(kvm_board_drv_t *drv)
{
    if (!drv) {
        return;
    }
    bool locked = (drv_mutex_lock(drv) == 0);
    if (drv->ready) {
        (void)drv_release_all_locked(drv);
        drv_irq_lock(drv);
        drv->ready = false;
        drv_irq_unlock(drv);
    }
    if (locked) {
        drv_mutex_unlock(drv);
    }
    memset(&drv->logic, 0, sizeof(drv->logic));
    memset(&drv->io, 0, sizeof(drv->io));
    drv->ready = false;
    drv->timer_running = false;
}

bool kvm_board_drv_on_alarm(kvm_board_drv_t *drv,
                            uint64_t count_value,
                            uint64_t alarm_value)
{
    if (!drv) {
        return false;
    }

    bool fired = false;
    int release_gpio = -1;
    drv_irq_lock(drv);
    if (kvm_board_drv_eval_alarm(drv, count_value, alarm_value, &release_gpio)) {
        (void)drv->io.gpio_set_level(drv->io.ctx, release_gpio, 1);
        fired = true;
    }
    drv_irq_unlock(drv);
    return fired;
}
