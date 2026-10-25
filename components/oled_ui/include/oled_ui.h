#ifndef OLED_UI_H
#define OLED_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Return flags for oled_ui tick and events
#define OLED_UI_FLAG_NONE       0
#define OLED_UI_FLAG_TURN_ON    (1 << 0)
#define OLED_UI_FLAG_TURN_OFF   (1 << 1)
#define OLED_UI_FLAG_REDRAW     (1 << 2)

typedef struct {
    uint8_t num_pages;
    uint32_t display_timeout_ms;
    uint32_t redraw_interval_ms;
} oled_ui_config_t;

typedef struct {
    oled_ui_config_t config;
    bool is_on;
    uint8_t current_page;
    uint64_t last_action_time_ms;
    uint64_t last_redraw_time_ms;
} oled_ui_t;

/**
 * @brief Initialize the OLED UI state machine.
 *
 * @param ui Pointer to the UI object.
 * @param config Configuration parameters.
 * @param now_ms Current time in milliseconds.
 * @return true on success, false on invalid parameters.
 */
bool oled_ui_init(oled_ui_t *ui, const oled_ui_config_t *config, uint64_t now_ms);

/**
 * @brief Process a button press event.
 *
 * @param ui Pointer to the UI object.
 * @param now_ms Current time in milliseconds.
 * @return Flags indicating resulting actions (TURN_ON, REDRAW).
 */
uint32_t oled_ui_on_button(oled_ui_t *ui, uint64_t now_ms);

/**
 * @brief Process an IP acquired event.
 *
 * @param ui Pointer to the UI object.
 * @param now_ms Current time in milliseconds.
 * @return Flags indicating resulting actions (TURN_ON, REDRAW).
 */
uint32_t oled_ui_on_ip_acquired(oled_ui_t *ui, uint64_t now_ms);

/**
 * @brief Perform periodic updates.
 *
 * @param ui Pointer to the UI object.
 * @param now_ms Current time in milliseconds.
 * @return Flags indicating resulting actions (TURN_OFF, REDRAW).
 */
uint32_t oled_ui_tick(oled_ui_t *ui, uint64_t now_ms);

/**
 * @brief Check if the display is currently on.
 *
 * @param ui Pointer to the UI object.
 * @return true if on, false otherwise.
 */
bool oled_ui_is_on(const oled_ui_t *ui);

/**
 * @brief Get the current page index.
 *
 * @param ui Pointer to the UI object.
 * @return Current page index.
 */
uint8_t oled_ui_page(const oled_ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif // OLED_UI_H
