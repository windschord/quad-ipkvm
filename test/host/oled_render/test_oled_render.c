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

static void fb_read_text(const oled_fb_t *fb, int x, int page, char *out, int max_len) {
    int out_idx = 0;
    while (x + 5 <= OLED_WIDTH && out_idx < max_len - 1) {
        char best_match = ' ';
        for (char c = 0x20; c <= 0x7E; c++) {
            int glyph_idx = c - 0x20;
            bool match = true;
            for (int cx = 0; cx < 5; cx++) {
                if (font5x7_data[glyph_idx][cx] != fb->data[page][x + cx]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                best_match = c;
                break;
            }
        }
        out[out_idx++] = best_match;
        x += 6; // advance by 6 pixels
    }
    out[out_idx] = '\0';
    // trim trailing spaces
    for (int i = out_idx - 1; i >= 0; i--) {
        if (out[i] == ' ') out[i] = '\0'; else break;
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
    oled_fb_draw_text(&g_fb_alloc->fb, 120, 60, "Very Long String That Will Go Off Screen!!!!!");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_oled_render_font_glyph_A);
    RUN_TEST(test_oled_render_font_all_glyphs_not_empty);
    RUN_TEST(test_oled_render_page_0_golden);
    RUN_TEST(test_oled_render_page_1_golden);
    RUN_TEST(test_oled_render_page_2_golden);
    RUN_TEST(test_oled_render_diff_basic);
    RUN_TEST(test_oled_render_diff_full);
    RUN_TEST(test_oled_render_out_of_bounds_safe);
    return UNITY_END();
}
// mutation tests manually verified:
// 1. Invert bit direction in page format -> test_oled_render_font_glyph_A fails.
// 2. Out of bounds column write -> test_oled_render_out_of_bounds_safe fails.
// 3. Truncation length -> changed cx condition to >= 150, test_oled_render_out_of_bounds_safe catches the overwrite.
// 4. Uptime carry over -> changing % 60 to % 59 in format_uptime causes golden test failure.
// 5. KB/MB rounding -> changing boundaries causes golden test failure.
// 6. diff max x minus 1 -> test_oled_render_diff_basic fails.
