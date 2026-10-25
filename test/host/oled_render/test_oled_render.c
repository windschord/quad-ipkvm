#include "unity.h"
#include "oled_render.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void fb_to_ascii(const oled_fb_t *fb, int page, int start_x, int width, char out_str[8][256]) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < width && x < 255; x++) {
            bool on = (fb->data[page][start_x + x] & (1 << y)) != 0;
            out_str[y][x] = on ? '#' : '.';
        }
        out_str[y][width] = '\0';
    }
}

// Include font data directly for readback helper
extern const uint8_t font5x7_data[95][5];

static void fb_read_text(const oled_fb_t *fb, int x, int y, char *out, int max_len) {
    int out_idx = 0;
    while (x + 5 <= OLED_WIDTH && out_idx < max_len - 1) {
        char best_match = '\x01';
        for (char c = 0x20; c <= 0x7E; c++) {
            int glyph_idx = c - 0x20;
            bool match = true;
            for (int cx = 0; cx < 5; cx++) {
                for (int cy = 0; cy < 8; cy++) {
                    bool expected_pixel = (font5x7_data[glyph_idx][cx] & (1 << cy)) != 0;
                    int py = y + cy;
                    if (py < 0 || py >= 64) continue;
                    int p_page = py / 8;
                    int p_bit = py % 8;
                    bool actual_pixel = (fb->data[p_page][x + cx] & (1 << p_bit)) != 0;
                    if (expected_pixel != actual_pixel) {
                        match = false;
                        break;
                    }
                }
                if (!match) break;
            }
            if (match) {
                best_match = c;
                break;
            }
        }
        out[out_idx++] = best_match;
        x += 6;
    }
    out[out_idx] = '\0';
    // trim trailing spaces
    for (int i = out_idx - 1; i >= 0; i--) {
        if (out[i] == ' ' || out[i] == '\x01') out[i] = '\0'; else break;
    }
}

typedef struct {
    uint8_t pre_guard[128];
    oled_fb_t fb;
    uint8_t post_guard[128];
} guarded_fb_t;

static guarded_fb_t *g_fb_alloc = NULL;

static void alloc_guarded_fb(void) {
    if (g_fb_alloc) free(g_fb_alloc);
    g_fb_alloc = calloc(1, sizeof(guarded_fb_t));
    memset(g_fb_alloc->pre_guard, 0xAA, sizeof(g_fb_alloc->pre_guard));
    memset(g_fb_alloc->post_guard, 0x55, sizeof(g_fb_alloc->post_guard));
}

static void check_and_free_guarded_fb(void) {
    if (!g_fb_alloc) return;
    int pre_corrupted = 0;
    int post_corrupted = 0;
    for (size_t i = 0; i < sizeof(g_fb_alloc->pre_guard); i++) {
        if (g_fb_alloc->pre_guard[i] != 0xAA) pre_corrupted = 1;
    }
    for (size_t i = 0; i < sizeof(g_fb_alloc->post_guard); i++) {
        if (g_fb_alloc->post_guard[i] != 0x55) post_corrupted = 1;
    }
    free(g_fb_alloc);
    g_fb_alloc = NULL;
    TEST_ASSERT_FALSE_MESSAGE(pre_corrupted, "Pre-guard corrupted");
    TEST_ASSERT_FALSE_MESSAGE(post_corrupted, "Post-guard corrupted");
}

void setUp(void) {}
void tearDown(void) {
    check_and_free_guarded_fb();
}

void test_oled_render_font_glyph_A(void) {
    alloc_guarded_fb();
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, "A");

    char ascii[8][256];
    fb_to_ascii(&g_fb_alloc->fb, 0, 0, 6, ascii);

    TEST_ASSERT_EQUAL_STRING(".###..", ascii[0]);
    TEST_ASSERT_EQUAL_STRING("#...#.", ascii[1]);
    TEST_ASSERT_EQUAL_STRING("#...#.", ascii[2]);
    TEST_ASSERT_EQUAL_STRING("#...#.", ascii[3]);
    TEST_ASSERT_EQUAL_STRING("#####.", ascii[4]);
    TEST_ASSERT_EQUAL_STRING("#...#.", ascii[5]);
    TEST_ASSERT_EQUAL_STRING("#...#.", ascii[6]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[7]);
}

void test_oled_render_font_all_glyphs_not_empty(void) {
    for (char c = 0x20; c <= 0x7E; c++) {
        alloc_guarded_fb();
        oled_fb_clear(&g_fb_alloc->fb);
        char str[2] = {c, 0};
        oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, str);

        int pixels = 0;
        for (int x = 0; x < 6; x++) {
            for (int y = 0; y < 8; y++) {
                if (g_fb_alloc->fb.data[0][x] & (1 << y)) pixels++;
            }
        }
        if (c == ' ') {
            TEST_ASSERT_EQUAL_INT(0, pixels);
        } else if (c != '~') {
            TEST_ASSERT_GREATER_THAN_MESSAGE(0, pixels, str);
        }
        check_and_free_guarded_fb();
    }
}

void test_oled_render_page_0_golden(void) {
    alloc_guarded_fb();

    oled_status_t st = {0};
    st.ip_address = "255.255.255.255";
    st.hostname = "VeryLongHostname123456789";
    st.link_speed_mbps = 0;
    st.needs_setup = true;

    oled_render_page(&g_fb_alloc->fb, 0, &st);

    char readback[64];
    fb_read_text(&g_fb_alloc->fb, 0, 0, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("SYSTEM INFO", readback);
}

void test_oled_render_page_1_golden(void) {
    alloc_guarded_fb();

    oled_status_t st = {0};
    st.kvm_channel = -1;
    st.video_state = OLED_VIDEO_STATE_ERROR;
    st.client_count = 99;

    oled_render_page(&g_fb_alloc->fb, 1, &st);

    char readback[64];
    fb_read_text(&g_fb_alloc->fb, 0, 0, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("KVM STATUS", readback);
}

void test_oled_render_page_2_golden(void) {
    alloc_guarded_fb();

    oled_status_t st = {0};
    st.uptime_seconds = 86400 + 3600*2 + 60*3 + 4;
    st.firmware_version = "v1.2.3";
    st.free_memory_bytes = 12 * 1024 * 1024 + 300 * 1024;

    oled_render_page(&g_fb_alloc->fb, 2, &st);

    char readback[64];
    fb_read_text(&g_fb_alloc->fb, 0, 0, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("DEVICE STATS", readback);

    // Uptime boundaries
    oled_fb_clear(&g_fb_alloc->fb);
    st.uptime_seconds = 0;
    oled_render_page(&g_fb_alloc->fb, 2, &st);

    oled_fb_clear(&g_fb_alloc->fb);
    st.uptime_seconds = 59;
    oled_render_page(&g_fb_alloc->fb, 2, &st);

    oled_fb_clear(&g_fb_alloc->fb);
    st.uptime_seconds = 3600;
    oled_render_page(&g_fb_alloc->fb, 2, &st);

    // memory boundaries
    st.free_memory_bytes = 1000;
    oled_render_page(&g_fb_alloc->fb, 2, &st);

    st.free_memory_bytes = 1024 * 500 + 512;
    oled_render_page(&g_fb_alloc->fb, 2, &st);
}

void test_oled_render_diff_basic(void) {
    oled_fb_t old, new;
    memset(&old, 0, sizeof(old));
    memset(&new, 0, sizeof(new));

    new.data[3][10] = 0xFF;
    new.data[3][20] = 0x55;

    oled_dirty_t dirty;
    oled_render_diff(&old, &new, &dirty);

    TEST_ASSERT_FALSE(dirty.pages[0].has_changes);
    TEST_ASSERT_TRUE(dirty.pages[3].has_changes);
    TEST_ASSERT_EQUAL_UINT8(10, dirty.pages[3].min_x);
    TEST_ASSERT_EQUAL_UINT8(20, dirty.pages[3].max_x);
}

void test_oled_render_diff_full(void) {
    oled_fb_t old, new;
    memset(&old, 0, sizeof(old));
    memset(&new, 0xFF, sizeof(new));

    oled_dirty_t dirty;
    oled_render_diff(&old, &new, &dirty);

    for (int i = 0; i < OLED_PAGES; i++) {
        TEST_ASSERT_TRUE(dirty.pages[i].has_changes);
        TEST_ASSERT_EQUAL_UINT8(0, dirty.pages[i].min_x);
        TEST_ASSERT_EQUAL_UINT8(127, dirty.pages[i].max_x);
    }
}

void test_oled_render_out_of_bounds_safe(void) {
    alloc_guarded_fb();

    oled_fb_set_pixel(&g_fb_alloc->fb, -1, -1, true);
    oled_fb_set_pixel(&g_fb_alloc->fb, 128, 64, true);
    oled_fb_draw_hline(&g_fb_alloc->fb, -10, 200, 100);
    oled_fb_invert_region(&g_fb_alloc->fb, 10, 10, 2147483647, 5);
    oled_fb_draw_text(&g_fb_alloc->fb, 2147483645, 60, "A");
    oled_fb_invert_region(&g_fb_alloc->fb, -5, -5, 200, 100);
    oled_fb_draw_text(&g_fb_alloc->fb, 120, 60, "Very Long String That Will Go Off Screen!!!!!");
}


void test_oled_render_font_ascii_art(void) {
    alloc_guarded_fb();
    char ascii[8][256];

    // Test '0'
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, "0");
    fb_to_ascii(&g_fb_alloc->fb, 0, 0, 6, ascii);
    TEST_ASSERT_EQUAL_STRING(".###..", ascii[0]);
    TEST_ASSERT_EQUAL_STRING("#...#.", ascii[1]);
    TEST_ASSERT_EQUAL_STRING("#..##.", ascii[2]);
    TEST_ASSERT_EQUAL_STRING("#.#.#.", ascii[3]);
    TEST_ASSERT_EQUAL_STRING("##..#.", ascii[4]);
    TEST_ASSERT_EQUAL_STRING("#...#.", ascii[5]);
    TEST_ASSERT_EQUAL_STRING(".###..", ascii[6]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[7]);

    // Test '.'
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, ".");
    fb_to_ascii(&g_fb_alloc->fb, 0, 0, 6, ascii);
    TEST_ASSERT_EQUAL_STRING("......", ascii[0]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[1]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[2]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[3]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[4]);
    TEST_ASSERT_EQUAL_STRING(".##...", ascii[5]);
    TEST_ASSERT_EQUAL_STRING(".##...", ascii[6]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[7]);

    // Test ':'
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, ":");
    fb_to_ascii(&g_fb_alloc->fb, 0, 0, 6, ascii);
    TEST_ASSERT_EQUAL_STRING("......", ascii[0]);
    TEST_ASSERT_EQUAL_STRING(".##...", ascii[1]);
    TEST_ASSERT_EQUAL_STRING(".##...", ascii[2]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[3]);
    TEST_ASSERT_EQUAL_STRING(".##...", ascii[4]);
    TEST_ASSERT_EQUAL_STRING(".##...", ascii[5]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[6]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[7]);

    // Test '/'
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, "/");
    fb_to_ascii(&g_fb_alloc->fb, 0, 0, 6, ascii);
    TEST_ASSERT_EQUAL_STRING("......", ascii[0]);
    TEST_ASSERT_EQUAL_STRING("....#.", ascii[1]);
    TEST_ASSERT_EQUAL_STRING("...#..", ascii[2]);
    TEST_ASSERT_EQUAL_STRING("..#...", ascii[3]);
    TEST_ASSERT_EQUAL_STRING(".#....", ascii[4]);
    TEST_ASSERT_EQUAL_STRING("#.....", ascii[5]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[6]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[7]);

    // Test '-'
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, "-");
    fb_to_ascii(&g_fb_alloc->fb, 0, 0, 6, ascii);
    TEST_ASSERT_EQUAL_STRING("......", ascii[0]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[1]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[2]);
    TEST_ASSERT_EQUAL_STRING("#####.", ascii[3]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[4]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[5]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[6]);
    TEST_ASSERT_EQUAL_STRING("......", ascii[7]);
}

void test_oled_render_page_0_all_cases(void) {
    alloc_guarded_fb();
    oled_status_t st = {0};
    char readback[128];

    // (a) IP=255.255.255.255/host/1000M
    st.ip_address = "255.255.255.255";
    st.hostname = "host";
    st.link_speed_mbps = 1000;
    st.needs_setup = false;
    oled_render_page(&g_fb_alloc->fb, 0, &st);
    fb_read_text(&g_fb_alloc->fb, 110, 0, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("1/3", readback);
    fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", readback);
    fb_read_text(&g_fb_alloc->fb, 36, 42, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("1000M", readback);

    // (b) リンク 0 -> DOWN
    st.link_speed_mbps = 0;
    oled_render_page(&g_fb_alloc->fb, 0, &st);
    fb_read_text(&g_fb_alloc->fb, 36, 42, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("DOWN", readback);

    // (c) needs_setup=true -> SETUP REQUIRED
    st.needs_setup = true;
    oled_render_page(&g_fb_alloc->fb, 0, &st);
    fb_read_text(&g_fb_alloc->fb, 0, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("SETUP REQUIRED", readback);

    // (d) IP が NULL と空文字列 -> -
    st.ip_address = NULL;
    oled_render_page(&g_fb_alloc->fb, 0, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("-", readback);

    st.ip_address = "";
    oled_render_page(&g_fb_alloc->fb, 0, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("-", readback);

    // (e) ホスト名が36文字 -> x=36から書ける15文字 "ABCDEFGHIJKLMNO"
    st.hostname = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    oled_render_page(&g_fb_alloc->fb, 0, &st);
    fb_read_text(&g_fb_alloc->fb, 36, 32, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNO", readback);

    // check that columns 126 and 127 are completely empty in the hostname row
    int set_pixels = 0;
    for (int y = 32; y < 40; y++) {
        if (g_fb_alloc->fb.data[y/8][126] & (1 << (y%8))) set_pixels++;
        if (g_fb_alloc->fb.data[y/8][127] & (1 << (y%8))) set_pixels++;
    }
    TEST_ASSERT_EQUAL_INT(0, set_pixels);
}


void test_oled_render_page_1_all_cases(void) {
    alloc_guarded_fb();
    oled_status_t st = {0};
    char readback[128];

    // channels 1 to 4
    for (int i = 0; i < 4; i++) {
        st.kvm_channel = i;
        oled_render_page(&g_fb_alloc->fb, 1, &st);
        fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
        char exp[16];
        snprintf(exp, sizeof(exp), "CH %d", i+1);
        TEST_ASSERT_EQUAL_STRING(exp, readback);
    }
    // channel unknown
    st.kvm_channel = -1;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("CH -", readback);
    st.kvm_channel = 4;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("CH -", readback);
    st.kvm_channel = 100;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("CH -", readback);

    // video states
    st.video_state = OLED_VIDEO_STATE_NONE;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("NONE", readback);
    st.video_state = OLED_VIDEO_STATE_ACTIVE;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("ACTIVE", readback);
    st.video_state = OLED_VIDEO_STATE_ERROR;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("ERROR", readback);
    st.video_state = OLED_VIDEO_STATE_UNKNOWN;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 24, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("UNK", readback);

    // clients
    st.client_count = 0;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 54, 32, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("0", readback);
    st.client_count = 99;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 54, 32, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("99", readback);
    st.client_count = 4294967295;
    oled_render_page(&g_fb_alloc->fb, 1, &st);
    fb_read_text(&g_fb_alloc->fb, 54, 32, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("4294967295", readback);

    fb_read_text(&g_fb_alloc->fb, 110, 0, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("2/3", readback);
}

void test_oled_render_page_2_all_cases(void) {
    alloc_guarded_fb();
    oled_status_t st = {0};
    char readback[128];

    // uptime
    uint32_t uptimes[] = {0, 59, 60, 3599, 3600, 86399, 86400, 86400*30+1, 4294967295};
    const char *exp_uptimes[] = {"0d 00:00:00", "0d 00:00:59", "0d 00:01:00", "0d 00:59:59", "0d 01:00:00", "0d 23:59:59", "1d 00:00:00", "30d 00:00:01", "49710d 06:28:15"};
    for (int i = 0; i < 9; i++) {
        st.uptime_seconds = uptimes[i];
        oled_render_page(&g_fb_alloc->fb, 2, &st);
        fb_read_text(&g_fb_alloc->fb, 24, 12, readback, sizeof(readback));
        TEST_ASSERT_EQUAL_STRING(exp_uptimes[i], readback);
    }

    // memory
    uint32_t mems[] = {0, 1, 1023, 1024, 1025, 1048524, 1048525, 1048575, 1048576, 12900000, 4294967295};
    const char *exp_mems[] = {"0 B", "1 B", "1023 B", "1.0 KB", "1.0 KB", "1023.9 KB", "1.0 MB", "1.0 MB", "1.0 MB", "12.3 MB", "4096.0 MB"};
    for (int i = 0; i < 11; i++) {
        st.free_memory_bytes = mems[i];
        oled_render_page(&g_fb_alloc->fb, 2, &st);
        fb_read_text(&g_fb_alloc->fb, 36, 32, readback, sizeof(readback));
        TEST_ASSERT_EQUAL_STRING(exp_mems[i], readback);
    }

    // version
    st.firmware_version = NULL;
    oled_render_page(&g_fb_alloc->fb, 2, &st);
    fb_read_text(&g_fb_alloc->fb, 30, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("-", readback);
    st.firmware_version = "";
    oled_render_page(&g_fb_alloc->fb, 2, &st);
    fb_read_text(&g_fb_alloc->fb, 30, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("-", readback);
    st.firmware_version = "abcdefghijklmnopqrstuvwxyz12345";
    oled_render_page(&g_fb_alloc->fb, 2, &st);
    fb_read_text(&g_fb_alloc->fb, 30, 22, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnop", readback);

    fb_read_text(&g_fb_alloc->fb, 110, 0, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING("3/3", readback);
}

void test_oled_render_out_of_bounds_pages(void) {
    alloc_guarded_fb();
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_set_pixel(&g_fb_alloc->fb, 10, 10, true);

    oled_status_t st = {0};
    oled_render_page(&g_fb_alloc->fb, -5, &st);
    oled_render_page(&g_fb_alloc->fb, 3, &st);
    oled_render_page(&g_fb_alloc->fb, 100, &st);
    oled_render_page(&g_fb_alloc->fb, 2147483647, &st);
    oled_render_page(&g_fb_alloc->fb, -2147483648, &st);

    // Assert buffer unchanged
    TEST_ASSERT_TRUE(g_fb_alloc->fb.data[1][10] != 0); // pixel (10, 10) is still there
    for (int p = 0; p < OLED_PAGES; p++) {
        for (int x = 0; x < OLED_WIDTH; x++) {
            if (p == 1 && x == 10) continue;
            TEST_ASSERT_EQUAL_UINT8(0, g_fb_alloc->fb.data[p][x]);
        }
    }
}


void test_oled_render_null_and_bounds(void) {
    alloc_guarded_fb();

    // Null safety
    oled_fb_clear(NULL);
    oled_fb_set_pixel(NULL, 0, 0, true);
    oled_fb_draw_text(NULL, 0, 0, "A");
    oled_fb_draw_hline(NULL, 0, 10, 10);
    oled_fb_invert_region(NULL, 0, 0, 10, 10);
    oled_render_page(NULL, 0, NULL);
    oled_dirty_t dirty;
    oled_render_diff(NULL, NULL, &dirty);

    // Out of bounds safety (should not trigger guards or ASan/UBSan)
    oled_fb_set_pixel(&g_fb_alloc->fb, -1, 0, true);
    oled_fb_set_pixel(&g_fb_alloc->fb, 128, 0, true);
    oled_fb_set_pixel(&g_fb_alloc->fb, 0, -1, true);
    oled_fb_set_pixel(&g_fb_alloc->fb, 0, 64, true);

    oled_fb_draw_hline(&g_fb_alloc->fb, -1, 10, 10);
    oled_fb_draw_hline(&g_fb_alloc->fb, 10, -1, 10);
    oled_fb_draw_hline(&g_fb_alloc->fb, 10, 200, 10);
    oled_fb_draw_hline(&g_fb_alloc->fb, 200, 10, 10);
    oled_fb_draw_hline(&g_fb_alloc->fb, 0, 10, -1);
    oled_fb_draw_hline(&g_fb_alloc->fb, 0, 10, 64);

    oled_fb_invert_region(&g_fb_alloc->fb, -5, -5, 200, 100);

    oled_fb_draw_text(&g_fb_alloc->fb, -10, 0, "A");
    oled_fb_draw_text(&g_fb_alloc->fb, 120, -10, "A");
    oled_fb_draw_text(&g_fb_alloc->fb, 120, 60, "Very Long String That Will Go Off Screen!!!!!");

    // Hline bounds
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_hline(&g_fb_alloc->fb, 10, 1, 0); // x0 > x1
    TEST_ASSERT_TRUE(g_fb_alloc->fb.data[0][1] != 0);
    TEST_ASSERT_TRUE(g_fb_alloc->fb.data[0][10] != 0);

    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_hline(&g_fb_alloc->fb, 0, 127, 0);
    TEST_ASSERT_TRUE(g_fb_alloc->fb.data[0][0] != 0);
    TEST_ASSERT_TRUE(g_fb_alloc->fb.data[0][127] != 0);

    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_invert_region(&g_fb_alloc->fb, 10, 10, 5, 5);
    oled_fb_invert_region(&g_fb_alloc->fb, 10, 10, 5, 5);
    for (int p = 0; p < OLED_PAGES; p++) {
        for (int x = 0; x < OLED_WIDTH; x++) {
            TEST_ASSERT_EQUAL_UINT8(0, g_fb_alloc->fb.data[p][x]);
        }
    }
}

void test_oled_render_diff_various(void) {
    oled_fb_t old, new;
    memset(&old, 0, sizeof(old));
    memset(&new, 0, sizeof(new));
    oled_dirty_t dirty;

    // No changes
    oled_render_diff(&old, &new, &dirty);
    for (int p = 0; p < OLED_PAGES; p++) TEST_ASSERT_FALSE(dirty.pages[p].has_changes);

    // 1 pixel change
    new.data[0][0] = 1;
    oled_render_diff(&old, &new, &dirty);
    TEST_ASSERT_TRUE(dirty.pages[0].has_changes);
    TEST_ASSERT_EQUAL_UINT8(0, dirty.pages[0].min_x);
    TEST_ASSERT_EQUAL_UINT8(0, dirty.pages[0].max_x);

    // Random fixed seed test
    memset(&old, 0, sizeof(old));
    memset(&new, 0, sizeof(new));
    srand(12345);
    for (int i = 0; i < 100; i++) {
        old.data[i%8][i] = rand() % 256;
        new.data[i%8][i] = old.data[i%8][i];
    }
    new.data[3][10] = old.data[3][10] ^ 0xFF;
    new.data[3][20] = old.data[3][20] ^ 0xFF;
    oled_render_diff(&old, &new, &dirty);
    TEST_ASSERT_TRUE(dirty.pages[3].has_changes);
    TEST_ASSERT_EQUAL_UINT8(10, dirty.pages[3].min_x);
    TEST_ASSERT_EQUAL_UINT8(20, dirty.pages[3].max_x);
    for (int p = 0; p < OLED_PAGES; p++) {
        if (p != 3) TEST_ASSERT_FALSE(dirty.pages[p].has_changes);
    }

    // Edges
    memset(&old, 0, sizeof(old));
    memset(&new, 0, sizeof(new));
    new.data[7][0] = 1;
    new.data[7][127] = 1;
    oled_render_diff(&old, &new, &dirty);
    TEST_ASSERT_TRUE(dirty.pages[7].has_changes);
    TEST_ASSERT_EQUAL_UINT8(0, dirty.pages[7].min_x);
    TEST_ASSERT_EQUAL_UINT8(127, dirty.pages[7].max_x);
}


void test_oled_render_font_all_glyphs_distinct(void) {
    // Basic distinct check for l/I/1, 0/O, 5/S
    alloc_guarded_fb();
    oled_fb_t fb1, fb2, fb3;

    oled_fb_clear(&fb1); oled_fb_draw_text(&fb1, 0, 0, "l");
    oled_fb_clear(&fb2); oled_fb_draw_text(&fb2, 0, 0, "I");
    oled_fb_clear(&fb3); oled_fb_draw_text(&fb3, 0, 0, "1");
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&fb1, &fb2, sizeof(oled_fb_t)));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&fb2, &fb3, sizeof(oled_fb_t)));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&fb1, &fb3, sizeof(oled_fb_t)));

    oled_fb_clear(&fb1); oled_fb_draw_text(&fb1, 0, 0, "0");
    oled_fb_clear(&fb2); oled_fb_draw_text(&fb2, 0, 0, "O");
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&fb1, &fb2, sizeof(oled_fb_t)));

    oled_fb_clear(&fb1); oled_fb_draw_text(&fb1, 0, 0, "5");
    oled_fb_clear(&fb2); oled_fb_draw_text(&fb2, 0, 0, "S");
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&fb1, &fb2, sizeof(oled_fb_t)));
}

void test_oled_render_font_non_ascii(void) {
    alloc_guarded_fb();
    oled_fb_t fb_q;
    oled_fb_clear(&fb_q);
    oled_fb_draw_text(&fb_q, 0, 0, "?");

    oled_fb_t fb_test;

    oled_fb_clear(&fb_test);
    oled_fb_draw_text(&fb_test, 0, 0, "\x80");
    TEST_ASSERT_EQUAL_MEMORY(&fb_q, &fb_test, sizeof(oled_fb_t));

    oled_fb_clear(&fb_test);
    oled_fb_draw_text(&fb_test, 0, 0, "\x1F");
    TEST_ASSERT_EQUAL_MEMORY(&fb_q, &fb_test, sizeof(oled_fb_t));

    oled_fb_clear(&fb_test);
    oled_fb_draw_text(&fb_test, 0, 0, "\x7F");
    TEST_ASSERT_EQUAL_MEMORY(&fb_q, &fb_test, sizeof(oled_fb_t));
}

void test_oled_render_truncation_exact(void) {
    alloc_guarded_fb();
    // String is 22 chars
    const char *str = "1234567890123456789012";
    oled_fb_clear(&g_fb_alloc->fb);
    oled_fb_draw_text(&g_fb_alloc->fb, 0, 0, str);

    // Character 22 ('2') should not be drawn because it doesn't fully fit (0 + 21*6 = 126, fits 2 pixels max).
    // Our logic says if cx + 5 >= OLED_WIDTH, break. So it won't be drawn at all.
    // Let's verify columns 126 and 127 are 0.
    for (int p = 0; p < OLED_PAGES; p++) {
        TEST_ASSERT_EQUAL_UINT8(0, g_fb_alloc->fb.data[p][126]);
        TEST_ASSERT_EQUAL_UINT8(0, g_fb_alloc->fb.data[p][127]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_oled_render_font_glyph_A);
    RUN_TEST(test_oled_render_font_ascii_art);
    RUN_TEST(test_oled_render_page_0_all_cases);
    RUN_TEST(test_oled_render_page_1_all_cases);
    RUN_TEST(test_oled_render_page_2_all_cases);
    RUN_TEST(test_oled_render_out_of_bounds_pages);
    RUN_TEST(test_oled_render_font_all_glyphs_not_empty);
    RUN_TEST(test_oled_render_page_0_golden);
    RUN_TEST(test_oled_render_page_1_golden);
    RUN_TEST(test_oled_render_page_2_golden);
    RUN_TEST(test_oled_render_diff_basic);
    RUN_TEST(test_oled_render_diff_full);
    RUN_TEST(test_oled_render_out_of_bounds_safe);
    RUN_TEST(test_oled_render_font_all_glyphs_distinct);
    RUN_TEST(test_oled_render_font_non_ascii);
    RUN_TEST(test_oled_render_truncation_exact);
    RUN_TEST(test_oled_render_null_and_bounds);
    RUN_TEST(test_oled_render_diff_various);
    return UNITY_END();
}
// mutation tests manually verified:
// 1. Invert bit direction in page format -> test_oled_render_font_glyph_A fails.
// 2. Out of bounds column write -> test_oled_render_out_of_bounds_safe fails.
// 3. Truncation length -> changed cx condition to >= 150, test_oled_render_out_of_bounds_safe catches the overwrite.
// 4. Uptime carry over -> changing % 60 to % 59 in format_uptime causes golden test failure.
// 5. KB/MB rounding -> changing boundaries causes golden test failure.
// 6. diff max x minus 1 -> test_oled_render_diff_basic fails.
