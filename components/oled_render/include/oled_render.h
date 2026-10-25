#pragma once

#include <stdint.h>
#include <stdbool.h>

#define OLED_WIDTH 128
#define OLED_PAGES 8

// OLED frame buffer structure (SSD1306 page format)
// 8 pages x 128 bytes. Each byte is a column of 8 pixels, LSB at the top.
typedef struct {
    uint8_t data[OLED_PAGES][OLED_WIDTH];
} oled_fb_t;

// Enum for video source state
typedef enum {
    OLED_VIDEO_STATE_NONE,
    OLED_VIDEO_STATE_ACTIVE,
    OLED_VIDEO_STATE_ERROR,
    OLED_VIDEO_STATE_UNKNOWN
} oled_video_state_t;

// Struct to hold system status for rendering
typedef struct {
    const char *ip_address; // IP string, max 15 chars + null
    const char *hostname;
    uint32_t link_speed_mbps; // 0 if link down
    int kvm_channel; // 0-3 (meaning CH 1-4), or -1 if unknown
    oled_video_state_t video_state;
    uint32_t client_count;
    uint32_t uptime_seconds;
    const char *firmware_version;
    uint32_t free_memory_bytes;
    bool needs_setup;
} oled_status_t;

// Struct for bounding box of changed columns per page
typedef struct {
    bool has_changes;
    uint8_t min_x;
    uint8_t max_x;
} oled_page_dirty_t;

typedef struct {
    oled_page_dirty_t pages[OLED_PAGES];
} oled_dirty_t;

// Basic drawing API
void oled_fb_clear(oled_fb_t *fb);
void oled_fb_set_pixel(oled_fb_t *fb, int x, int y, bool on);
void oled_fb_draw_text(oled_fb_t *fb, int x, int y, const char *str);
void oled_fb_draw_hline(oled_fb_t *fb, int x0, int x1, int y);
void oled_fb_invert_region(oled_fb_t *fb, int x, int y, int w, int h);

// Render specific pages.
// page_index must be 0, 1, or 2. If out of bounds (e.g. -1, 3), it does nothing (no clear, no drawing).
// Text drawing truncates strings so that any character which doesn't fully fit horizontally (cx + 5 >= OLED_WIDTH) will NOT be drawn.
// page: 0, 1, or 2
void oled_render_page(oled_fb_t *fb, int page_index, const oled_status_t *st);

// Calculate differences between two frame buffers
void oled_render_diff(const oled_fb_t *old_fb, const oled_fb_t *new_fb, oled_dirty_t *out_dirty);
