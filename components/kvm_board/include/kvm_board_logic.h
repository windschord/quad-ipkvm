#ifndef KVM_BOARD_LOGIC_H
#define KVM_BOARD_LOGIC_H

/*
 * kvm_board_logic: Pure-C hardware safety logic for the 4ch KVM + ATX board HAL
 * (docs/INTERFACES.md §6.2, docs/HANDOFF.md §3.2-3.3, docs/PLAN.md §4.4).
 *
 * Host-testable without ESP-IDF dependencies:
 *   - Pin configuration validation (unassigned < 0, range 0..54, duplicates,
 *     Waveshare ESP32-P4-ETH reserved peripheral / strapping / at-reset pins)
 *   - Hardware safety caps per line (POWER <= 500 ms, RESET <= 200 ms, FORCE <= 5000 ms)
 *   - Mutual exclusion across all active-low E lines (POWER / POWER_FORCE / RESET / KVM)
 *   - A0/A1 address decoding guarded against switching while any line is asserted
 *   - Strictly monotonic 64-bit alarm tokens so delayed timer ISRs never release a newer pulse
 *   - Configurable active-low / active-high KVM LED polarity decoding
 */

#include <stdbool.h>
#include <stdint.h>
#include "kvm_board.h"
#include "kvm_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_BOARD_GPIO_MAX                54
#define KVM_BOARD_NUM_CHANNELS            4u
#define KVM_BOARD_NUM_ENABLE_LINES        3u
#define KVM_BOARD_DEFAULT_HARD_MAX_KVM_MS 1000u  /* TODO(hw-verify) */
#define KVM_BOARD_HARD_MAX_KVM_CAP_MS     5000u  /* TODO(hw-verify) */

typedef struct {
    int a0;
    int a1;
    int e_power;
    int e_reset;
    int e_kvm;
    int led_ch[KVM_BOARD_NUM_CHANNELS];
} kvm_board_pins_t;

typedef struct {
    kvm_board_pins_t pins;
    uint32_t max_power_ms;
    uint32_t max_reset_ms;
    uint32_t max_force_ms;
    uint32_t max_kvm_ms;
    bool led_active_low;       /* TODO(hw-verify): confirm LED polarity on KVM PCB */
    bool allow_jtag_inputs;    /* Allow GPIO2..5 on LED inputs only (never on outputs) */
    uint64_t reserved_gpio_mask; /* Extra board-specific reserved GPIO bitmask (1ULL << gpio) */
} kvm_board_logic_config_t;

typedef struct {
    kvm_board_logic_config_t config;
    bool initialized;
    uint8_t current_channel;       /* 1..4 */
    bool has_active_line;
    kvm_board_line_t active_line;
    uint64_t last_alarm_us;
    uint64_t active_alarm_token;   /* Non-zero while has_active_line == true */
} kvm_board_logic_t;

/* Populate safe fail-closed defaults: all pins -1, hard ATX caps, active-low LEDs. */
void kvm_board_logic_config_default(kvm_board_logic_config_t *cfg);

/* Built-in mask of Waveshare ESP32-P4-ETH GPIOs bound to fixed peripherals or strapping. */
uint64_t kvm_board_logic_fixed_reserved_mask(void);

/* Mask of GPIOs unsafe for 74HC139 outputs (JTAG GPIO2..5, at-reset input-active GPIO32..33). */
uint64_t kvm_board_logic_output_forbidden_mask(void);

/* Initialize state machine and validate pins/caps. Returns 0 on success, -1 on invalid config. */
int kvm_board_logic_init(kvm_board_logic_t *b, const kvm_board_logic_config_t *cfg);

/* Map channel (1..4) to A0/A1 logic levels (ch 1 -> 00 .. ch 4 -> 11). Returns -1 if out of range. */
int kvm_board_logic_channel_to_addr(uint8_t ch, bool *out_a0, bool *out_a1);

/* Validate and record a channel address change. Fails (-1) if any E line is currently asserted. */
int kvm_board_logic_begin_set_addr(kvm_board_logic_t *b, uint8_t ch, bool *out_a0, bool *out_a1);

/*
 * Start an active-low pulse assertion under mutual exclusion and compute a strictly monotonic
 * 1 MHz hardware timer schedule (base_us + clamped_ms * 1000 == alarm_us).
 * Fails (-1) if uninitialized, line invalid, or any line is currently asserted.
 */
int kvm_board_logic_begin_assert(kvm_board_logic_t *b, kvm_board_line_t line,
                                 uint32_t requested_max_ms, uint64_t now_us,
                                 uint64_t *out_base_us, uint64_t *out_alarm_us,
                                 int *out_gpio);

/*
 * Release the specified line if it is currently active. Returns 0 if valid line (setting
 * *out_released = true if the hardware GPIO must be driven High, false if already inactive
 * or another logical line owns that pin such as POWER vs POWER_FORCE), -1 on invalid args.
 */
int kvm_board_logic_release_line(kvm_board_logic_t *b, kvm_board_line_t line,
                                 bool *out_released, int *out_gpio);

/*
 * Process a hardware timer alarm callback inside ISR context. Returns true (and writes
 * *out_gpio to drive High) only if a pulse matching alarm_value is currently active.
 */
bool kvm_board_logic_on_timer_alarm(kvm_board_logic_t *b, uint64_t alarm_value, int *out_gpio);

/* Force all active-low E lines to inactive state and fill out_gpios[3] = {e_power, e_reset, e_kvm}. */
int kvm_board_logic_release_all(kvm_board_logic_t *b, int out_gpios[KVM_BOARD_NUM_ENABLE_LINES]);

/* Convert 4 raw GPIO levels (bit 0 = CH1 .. bit 3 = CH4) into a 4-bit LED active mask. */
uint8_t kvm_board_logic_decode_leds(const bool raw_levels[KVM_BOARD_NUM_CHANNELS], bool active_low);

#ifdef __cplusplus
}
#endif

#endif /* KVM_BOARD_LOGIC_H */
