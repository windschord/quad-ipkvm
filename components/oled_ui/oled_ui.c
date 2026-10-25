#include "oled_ui.h"
#include <stddef.h>

bool oled_ui_init(oled_ui_t *ui, const oled_ui_config_t *config, uint64_t now_ms) {
    if (!ui || !config || config->num_pages == 0 || config->display_timeout_ms == 0) {
        return false;
    }

    ui->config = *config;
    if (ui->config.redraw_interval_ms == 0) {
        ui->config.redraw_interval_ms = 1000;
    }

    ui->is_on = true;
    ui->current_page = 0;
    ui->last_action_time_ms = now_ms;
    ui->last_redraw_time_ms = now_ms;

    return true;
}

uint32_t oled_ui_on_button(oled_ui_t *ui, uint64_t now_ms) {
    if (!ui) {
        return OLED_UI_FLAG_NONE;
    }

    uint32_t flags = OLED_UI_FLAG_NONE;

    if (!ui->is_on) {
        ui->is_on = true;
        ui->current_page = 0;
        flags = OLED_UI_FLAG_TURN_ON | OLED_UI_FLAG_REDRAW;
    } else {
        ui->current_page = (ui->current_page + 1) % ui->config.num_pages;
        flags = OLED_UI_FLAG_REDRAW;
    }

    ui->last_action_time_ms = now_ms;
    ui->last_redraw_time_ms = now_ms;

    return flags;
}

uint32_t oled_ui_on_ip_acquired(oled_ui_t *ui, uint64_t now_ms) {
    if (!ui) {
        return OLED_UI_FLAG_NONE;
    }

    uint32_t flags = OLED_UI_FLAG_NONE;

    if (!ui->is_on) {
        ui->is_on = true;
        flags = OLED_UI_FLAG_TURN_ON | OLED_UI_FLAG_REDRAW;
    } else {
        flags = OLED_UI_FLAG_REDRAW;
    }

    ui->current_page = 0;
    ui->last_action_time_ms = now_ms;
    ui->last_redraw_time_ms = now_ms;

    return flags;
}

uint32_t oled_ui_tick(oled_ui_t *ui, uint64_t now_ms) {
    if (!ui || !ui->is_on) {
        return OLED_UI_FLAG_NONE;
    }

    if (now_ms < ui->last_action_time_ms || now_ms < ui->last_redraw_time_ms) {
        // Time went backwards, fail safe to OFF
        ui->is_on = false;
        ui->current_page = 0;
        return OLED_UI_FLAG_TURN_OFF;
    }

    uint64_t elapsed_since_action = now_ms - ui->last_action_time_ms;
    if (elapsed_since_action >= ui->config.display_timeout_ms) {
        ui->is_on = false;
        ui->current_page = 0;
        return OLED_UI_FLAG_TURN_OFF;
    }

    uint64_t elapsed_since_redraw = now_ms - ui->last_redraw_time_ms;
    if (elapsed_since_redraw >= ui->config.redraw_interval_ms) {
        ui->last_redraw_time_ms = now_ms;
        return OLED_UI_FLAG_REDRAW;
    }

    return OLED_UI_FLAG_NONE;
}

bool oled_ui_is_on(const oled_ui_t *ui) {
    if (!ui) {
        return false;
    }
    return ui->is_on;
}

uint8_t oled_ui_page(const oled_ui_t *ui) {
    if (!ui) {
        return 0;
    }
    return ui->current_page;
}
