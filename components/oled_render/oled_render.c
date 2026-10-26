#include "oled_render.h"
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

extern const uint8_t font5x7_data[95][5];

void oled_fb_clear(oled_fb_t *fb) {
    if (!fb) return;
    memset(fb->data, 0, sizeof(fb->data));
}

void oled_fb_set_pixel(oled_fb_t *fb, int x, int y, bool on) {
    if (!fb) return;
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_PAGES * 8) return;

    int page = y / 8;
    int bit = y % 8;

    if (on) {
        fb->data[page][x] |= (1 << bit);
    } else {
        fb->data[page][x] &= ~(1 << bit);
    }
}

void oled_fb_draw_hline(oled_fb_t *fb, int x0, int x1, int y) {
    if (!fb) return;
    if (y < 0 || y >= OLED_PAGES * 8) return;

    if (x0 > x1) {
        int tmp = x0;
        x0 = x1;
        x1 = tmp;
    }

    if (x0 < 0) x0 = 0;
    if (x1 >= OLED_WIDTH) x1 = OLED_WIDTH - 1;
    if (x0 > x1) return;

    int page = y / 8;
    uint8_t mask = (1 << (y % 8));

    for (int x = x0; x <= x1; x++) {
        fb->data[page][x] |= mask;
    }
}

void oled_fb_invert_region(oled_fb_t *fb, int x, int y, int w, int h) {
    if (!fb) return;
    // Slow but simple implementation
    long long max_y = (long long)y + h;
    long long max_x = (long long)x + w;
    if (max_y > OLED_PAGES * 8) max_y = OLED_PAGES * 8;
    if (max_x > OLED_WIDTH) max_x = OLED_WIDTH;
    for (int cy = y; cy < max_y; cy++) {
        if (cy < 0 || cy >= OLED_PAGES * 8) continue;
        int page = cy / 8;
        uint8_t mask = (1 << (cy % 8));
        for (int cx = x; cx < max_x; cx++) {
            if (cx < 0 || cx >= OLED_WIDTH) continue;
            fb->data[page][cx] ^= mask;
        }
    }
}

static void draw_char(oled_fb_t *fb, int x, int y, char c) {
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '?';
    int glyph_idx = c - 0x20;

    // Character cell is 5x7 plus 1 pixel spacing -> 6x8 cell usually
    for (int cx = 0; cx < 5; cx++) {
        uint8_t col = font5x7_data[glyph_idx][cx];
        for (int cy = 0; cy < 8; cy++) {
            if (col & (1 << cy)) {
                oled_fb_set_pixel(fb, x + cx, y + cy, true);
            }
        }
    }
}

void oled_fb_draw_text(oled_fb_t *fb, int x, int y, const char *str) {
    if (!fb || !str) return;
    long long cx = x;
    while (*str) {
        if (cx + 5 >= OLED_WIDTH) break;
        draw_char(fb, cx, y, *str);
        cx += 6; // 5 px font + 1 px spacing
        str++;
    }
}

static void format_uptime(uint32_t seconds, char *buf, size_t size) {
    uint32_t d = seconds / 86400;
    uint32_t h = (seconds % 86400) / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    snprintf(buf, size, "%" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, d, h, m, s);
}

static void format_memory(uint32_t bytes, char *buf, size_t size) {
    if (bytes < 1024) {
        snprintf(buf, size, "%" PRIu32 " B", bytes);
    } else {
        uint32_t div = 1024;
        const char *unit = "KB";
        uint32_t v_tenths = 0;
        if (bytes < 1048525) {
            div = 1024;
            v_tenths = ((uint64_t)bytes * 10 + 512) / 1024;
        } else {
            div = 1048576;
            unit = "MB";
            v_tenths = ((uint64_t)bytes * 10 + 524288) / 1048576;
        }
        uint32_t major = v_tenths / 10;
        uint32_t minor = v_tenths % 10;
        if (major >= 1024 && div == 1024) { major = 1; minor = 0; unit = "MB"; }
        snprintf(buf, size, "%" PRIu32 ".%" PRIu32 " %s", major, minor, unit);
    }
}

void oled_render_page(oled_fb_t *fb, int page_index, const oled_status_t *st) {
    if (!fb || !st) return;
    if (page_index < 0 || page_index > 2) return;
    oled_fb_clear(fb);

    char buf[32];

    // Common header (Page indicator)
    snprintf(buf, sizeof(buf), "%d/3", page_index + 1);
    oled_fb_draw_text(fb, 110, 0, buf);
    oled_fb_draw_hline(fb, 0, OLED_WIDTH - 1, 9);

    if (page_index == 0) {
        oled_fb_draw_text(fb, 0, 0, "SYSTEM INFO");

        // IP Address
        oled_fb_draw_text(fb, 0, 12, "IP:");
        const char *ip = (st->ip_address && st->ip_address[0]) ? st->ip_address : "-";
        oled_fb_draw_text(fb, 24, 12, ip);

        if (st->needs_setup) {
            oled_fb_draw_text(fb, 0, 22, "SETUP REQUIRED");
        }

        // Hostname
        oled_fb_draw_text(fb, 0, 32, "Host:");
        const char *host = (st->hostname && st->hostname[0]) ? st->hostname : "-";
        oled_fb_draw_text(fb, 36, 32, host);

        // Link speed
        oled_fb_draw_text(fb, 0, 42, "Link:");
        if (st->link_speed_mbps == 0) {
            oled_fb_draw_text(fb, 36, 42, "DOWN");
        } else {
            snprintf(buf, sizeof(buf), "%" PRIu32 "M", st->link_speed_mbps);
            oled_fb_draw_text(fb, 36, 42, buf);
        }
    } else if (page_index == 1) {
        oled_fb_draw_text(fb, 0, 0, "KVM STATUS");

        // Channel
        oled_fb_draw_text(fb, 0, 12, "CH:");
        if (st->kvm_channel >= 0 && st->kvm_channel <= 3) {
            snprintf(buf, sizeof(buf), "CH %d", st->kvm_channel + 1);
        } else {
            snprintf(buf, sizeof(buf), "CH -");
        }
        oled_fb_draw_text(fb, 24, 12, buf);

        // Video State
        oled_fb_draw_text(fb, 0, 22, "Vid:");
        const char *vstate = "UNK";
        switch (st->video_state) {
            case OLED_VIDEO_STATE_NONE: vstate = "NONE"; break;
            case OLED_VIDEO_STATE_ACTIVE: vstate = "ACTIVE"; break;
            case OLED_VIDEO_STATE_ERROR: vstate = "ERROR"; break;
            case OLED_VIDEO_STATE_UNKNOWN: vstate = "UNK"; break;
        }
        oled_fb_draw_text(fb, 24, 22, vstate);

        // Client count
        oled_fb_draw_text(fb, 0, 32, "Clients:");
        snprintf(buf, sizeof(buf), "%" PRIu32, st->client_count);
        oled_fb_draw_text(fb, 54, 32, buf);

    } else if (page_index == 2) {
        oled_fb_draw_text(fb, 0, 0, "DEVICE STATS");

        // Uptime
        oled_fb_draw_text(fb, 0, 12, "Up:");
        format_uptime(st->uptime_seconds, buf, sizeof(buf));
        oled_fb_draw_text(fb, 24, 12, buf);

        // Version
        oled_fb_draw_text(fb, 0, 22, "Ver:");
        const char *ver = (st->firmware_version && st->firmware_version[0]) ? st->firmware_version : "-";
        oled_fb_draw_text(fb, 30, 22, ver);

        // Memory
        oled_fb_draw_text(fb, 0, 32, "Free:");
        format_memory(st->free_memory_bytes, buf, sizeof(buf));
        oled_fb_draw_text(fb, 36, 32, buf);
    }
}

void oled_render_diff(const oled_fb_t *old_fb, const oled_fb_t *new_fb, oled_dirty_t *out_dirty) {
    if (!old_fb || !new_fb || !out_dirty) return;

    for (int p = 0; p < OLED_PAGES; p++) {
        out_dirty->pages[p].has_changes = false;
        out_dirty->pages[p].min_x = 0xFF;
        out_dirty->pages[p].max_x = 0;

        for (int x = 0; x < OLED_WIDTH; x++) {
            if (old_fb->data[p][x] != new_fb->data[p][x]) {
                if (!out_dirty->pages[p].has_changes) {
                    out_dirty->pages[p].has_changes = true;
                    out_dirty->pages[p].min_x = (uint8_t)x;
                    out_dirty->pages[p].max_x = (uint8_t)x;
                } else {
                    out_dirty->pages[p].max_x = (uint8_t)x;
                }
            }
        }
    }
}
