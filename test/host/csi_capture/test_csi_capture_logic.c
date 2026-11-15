#include "unity.h"
#include "csi_capture_logic.h"

#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

// ---- geometry ----
#define CAP_1080P ((size_t)1920u * 1080u * 3u)

void test_geometry_720p_rgb888_ok(void) {
    size_t bytes = 1;
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(1280, 720, 24, 1920, 1080, CAP_1080P, &bytes));
    TEST_ASSERT_EQUAL_UINT32(2764800u, (uint32_t)bytes);
}

void test_geometry_exact_capacity_ok(void) {
    size_t bytes = 0;
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(1920, 1080, 24, 1920, 1080, CAP_1080P, &bytes));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)CAP_1080P, (uint32_t)bytes);
}

void test_geometry_capacity_one_short_rejected(void) {
    size_t bytes = 7;
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_CAPACITY,
                      csi_capture_check_geometry(1920, 1080, 24, 1920, 1080, CAP_1080P - 1, &bytes));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)bytes);  // output cleared on failure
}

void test_geometry_dimension_limits(void) {
    size_t bytes;
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_DIM, csi_capture_check_geometry(1921, 1080, 24, 1920, 1080, SIZE_MAX, &bytes));
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_DIM, csi_capture_check_geometry(1920, 1081, 24, 1920, 1080, SIZE_MAX, &bytes));
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(1920, 1080, 24, 1920, 1080, SIZE_MAX, &bytes));
}

void test_geometry_zero_and_null(void) {
    size_t bytes;
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_ARG, csi_capture_check_geometry(1280, 720, 24, 1920, 1080, CAP_1080P, NULL));
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_ZERO, csi_capture_check_geometry(0, 720, 24, 1920, 1080, CAP_1080P, &bytes));
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_ZERO, csi_capture_check_geometry(1280, 0, 24, 1920, 1080, CAP_1080P, &bytes));
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_ZERO, csi_capture_check_geometry(1280, 720, 0, 1920, 1080, CAP_1080P, &bytes));
}

void test_geometry_bpp(void) {
    size_t bytes;
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_BPP, csi_capture_check_geometry(1280, 720, 12, 1920, 1080, CAP_1080P, &bytes));
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_BPP, csi_capture_check_geometry(1280, 720, 40, 1920, 1080, SIZE_MAX, &bytes));
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(1280, 720, 32, 1920, 1080, SIZE_MAX, &bytes));
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(1280, 720, 16, 1920, 1080, CAP_1080P, &bytes));
}

void test_geometry_64bit_alignment(void) {
    size_t bytes;
    // 3 x 1 x 24 = 72 bits -> not a multiple of 64
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_ALIGN, csi_capture_check_geometry(3, 1, 24, 1920, 1080, CAP_1080P, &bytes));
    // 8 x 1 x 24 = 192 bits -> OK
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(8, 1, 24, 1920, 1080, CAP_1080P, &bytes));
    TEST_ASSERT_EQUAL_UINT32(24u, (uint32_t)bytes);
}

void test_geometry_int32_overflow(void) {
    size_t bytes;
    // 65535 x 65535 x 32 bits overflows int32 even though uint64 math is fine
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_OVERFLOW,
                      csi_capture_check_geometry(65535, 65535, 32, 65535, 65535, SIZE_MAX, &bytes));
    // 4096 x 4096 x 32 = 2^29 bits fits
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(4096, 4096, 32, 4096, 4096, SIZE_MAX, &bytes));
    // Largest 64-bit-aligned frame fitting in signed int32: 18631 * 3602 * 32 = 0x7FFFFFC0 <= INT32_MAX
    TEST_ASSERT_EQUAL(CSI_GEOM_OK, csi_capture_check_geometry(18631, 3602, 32, 65535, 65535, SIZE_MAX, &bytes));
    // INT32_MAX boundary: 2^31 bits (0x80000000) is rejected (> INT32_MAX)
    TEST_ASSERT_EQUAL(CSI_GEOM_ERR_OVERFLOW,
                      csi_capture_check_geometry(65536, 1024, 32, 65536, 65536, SIZE_MAX, &bytes));
}

void test_round_up(void) {
    TEST_ASSERT_EQUAL_UINT32(128u, (uint32_t)csi_capture_round_up(100, 64));
    TEST_ASSERT_EQUAL_UINT32(128u, (uint32_t)csi_capture_round_up(128, 64));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)csi_capture_round_up(0, 64));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)csi_capture_round_up(5, 0));
    TEST_ASSERT_TRUE(csi_capture_round_up(SIZE_MAX - 63, 64) == SIZE_MAX - 63);
    TEST_ASSERT_TRUE(csi_capture_round_up(SIZE_MAX - 62, 64) == 0);
    // Non-power-of-two align wraps to a non-zero value without the overflow guard
    TEST_ASSERT_TRUE(csi_capture_round_up(SIZE_MAX - 1, 100) == 0);
}

// ---- two-buffer selector ----
void test_pp_start_uses_buffer0_without_publishing(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 0);
    TEST_ASSERT_EQUAL_INT(0, csi_pp_next_dma(&pp));
    TEST_ASSERT_EQUAL_UINT32(0u, pp.completed);
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, csi_pp_acquire(&pp));
}

void test_pp_alternates_and_publishes(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 0);
    csi_pp_next_dma(&pp);                         // start -> 0
    TEST_ASSERT_EQUAL_INT(1, csi_pp_next_dma(&pp));  // 0 done -> latest 0, next 1
    TEST_ASSERT_EQUAL_INT(0, pp.latest);
    TEST_ASSERT_EQUAL_INT(0, csi_pp_next_dma(&pp));  // 1 done -> latest 1 (0 dropped), next 0
    TEST_ASSERT_EQUAL_INT(1, pp.latest);
    TEST_ASSERT_EQUAL_UINT32(2u, pp.completed);
    TEST_ASSERT_EQUAL_UINT32(2u, pp.published);
    TEST_ASSERT_EQUAL_UINT32(1u, pp.overwritten);
}

void test_pp_acquire_never_returns_buffer_being_written(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 0);
    csi_pp_next_dma(&pp);
    for (int i = 0; i < 50; i++) {
        csi_pp_next_dma(&pp);
        int got = csi_pp_acquire(&pp);
        TEST_ASSERT_NOT_EQUAL(CSI_PP_NONE, got);
        TEST_ASSERT_NOT_EQUAL(pp.writing, got);
        // While held, several DMA completions must never target the held buffer.
        for (int k = 0; k < 3; k++) {
            int next = csi_pp_next_dma(&pp);
            TEST_ASSERT_NOT_EQUAL(got, next);
        }
        TEST_ASSERT_TRUE(csi_pp_release(&pp, got));
    }
}

void test_pp_held_forces_overwrite_of_latest(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 0);
    csi_pp_next_dma(&pp);       // writing 0
    csi_pp_next_dma(&pp);       // latest 0, writing 1
    TEST_ASSERT_EQUAL_INT(0, csi_pp_acquire(&pp));   // held 0
    // 1 completes: latest=1, but buffers are {held 0, latest 1} -> must overwrite 1
    TEST_ASSERT_EQUAL_INT(1, csi_pp_next_dma(&pp));
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, pp.latest);
    TEST_ASSERT_EQUAL_UINT32(1u, pp.overwritten);
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, csi_pp_acquire(&pp));  // already holding one
    TEST_ASSERT_TRUE(csi_pp_release(&pp, 0));
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, csi_pp_acquire(&pp));  // no complete frame yet
    TEST_ASSERT_EQUAL_INT(0, csi_pp_next_dma(&pp));           // 1 done -> latest 1, next 0
    TEST_ASSERT_EQUAL_INT(1, csi_pp_acquire(&pp));
}

void test_pp_skip_initial_frames(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 2);
    csi_pp_next_dma(&pp);
    csi_pp_next_dma(&pp);  // completion 1 (skipped)
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, csi_pp_acquire(&pp));
    csi_pp_next_dma(&pp);  // completion 2 (skipped)
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, csi_pp_acquire(&pp));
    TEST_ASSERT_EQUAL_UINT32(0u, pp.published);
    csi_pp_next_dma(&pp);  // completion 3 -> published
    TEST_ASSERT_EQUAL_UINT32(1u, pp.published);
    TEST_ASSERT_NOT_EQUAL(CSI_PP_NONE, csi_pp_acquire(&pp));
}

void test_pp_release_rejects_wrong_index(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 0);
    TEST_ASSERT_FALSE(csi_pp_release(&pp, CSI_PP_NONE));
    TEST_ASSERT_FALSE(csi_pp_release(&pp, 0));
    csi_pp_next_dma(&pp);
    csi_pp_next_dma(&pp);
    int got = csi_pp_acquire(&pp);
    TEST_ASSERT_FALSE(csi_pp_release(&pp, 1 - got));
    TEST_ASSERT_TRUE(csi_pp_release(&pp, got));
    TEST_ASSERT_FALSE(csi_pp_release(&pp, got));
}

void test_pp_stop_keeps_latest_and_restart_does_not_count(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 0);
    csi_pp_next_dma(&pp);
    csi_pp_next_dma(&pp);   // latest 0, writing 1
    csi_pp_stop(&pp);
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, pp.writing);
    uint32_t done = pp.completed;
    int next = csi_pp_next_dma(&pp);  // restart: no completion accounted
    TEST_ASSERT_EQUAL_UINT32(done, pp.completed);
    TEST_ASSERT_NOT_EQUAL(0, next);   // latest 0 is preserved
    TEST_ASSERT_EQUAL_INT(0, csi_pp_acquire(&pp));
}

void test_pp_acquire_while_held_returns_none_even_if_latest_present(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 0);
    // Construct state where consumer holds buffer 0 and buffer 1 is marked latest:
    // re-acquiring MUST return CSI_PP_NONE without clobbering held or consuming latest.
    pp.held = 0;
    pp.latest = 1;
    TEST_ASSERT_EQUAL_INT(CSI_PP_NONE, csi_pp_acquire(&pp));
    TEST_ASSERT_EQUAL_INT(0, pp.held);
    TEST_ASSERT_EQUAL_INT(1, pp.latest);
}

void test_pp_overwritten_conservation_invariant_never_double_counts(void) {
    csi_pp_t pp;
    csi_pp_init(&pp, 1);
    uint32_t acquired = 0u;

    for (uint32_t step = 0u; step < 200u; step++) {
        (void)csi_pp_next_dma(&pp);
        uint32_t in_latest = (pp.latest != CSI_PP_NONE) ? 1u : 0u;
        TEST_ASSERT_EQUAL_UINT32(pp.published, acquired + pp.overwritten + in_latest);

        if ((step % 5u) == 2u) {
            int got = csi_pp_acquire(&pp);
            if (got != CSI_PP_NONE) {
                acquired++;
            }
            in_latest = (pp.latest != CSI_PP_NONE) ? 1u : 0u;
            TEST_ASSERT_EQUAL_UINT32(pp.published, acquired + pp.overwritten + in_latest);
        }
        if ((step % 7u) == 6u && pp.held != CSI_PP_NONE) {
            TEST_ASSERT_TRUE(csi_pp_release(&pp, pp.held));
            in_latest = (pp.latest != CSI_PP_NONE) ? 1u : 0u;
            TEST_ASSERT_EQUAL_UINT32(pp.published, acquired + pp.overwritten + in_latest);
        }
        if ((step % 23u) == 22u) {
            csi_pp_stop(&pp);
            in_latest = (pp.latest != CSI_PP_NONE) ? 1u : 0u;
            TEST_ASSERT_EQUAL_UINT32(pp.published, acquired + pp.overwritten + in_latest);
        }
    }

    // Defensive check: even if latest == writing on entry, overwritten must not double-count.
    csi_pp_init(&pp, 0);
    pp.held = 0;
    pp.latest = 1;
    pp.writing = 1;
    TEST_ASSERT_EQUAL_INT(1, csi_pp_next_dma(&pp));
    TEST_ASSERT_EQUAL_UINT32(1u, pp.overwritten);
}

void test_format_geometry_rgb888_and_uyvy(void) {
    TEST_ASSERT_EQUAL_UINT32(24u, csi_pixel_format_bpp(CSI_PIXEL_FORMAT_RGB888));
    TEST_ASSERT_EQUAL_UINT32(16u, csi_pixel_format_bpp(CSI_PIXEL_FORMAT_UYVY));
    TEST_ASSERT_EQUAL_UINT32(0u, csi_pixel_format_bpp((csi_pixel_format_t)99));

    size_t out = 0;
    TEST_ASSERT_EQUAL_INT(CSI_GEOM_ERR_ARG,
                          csi_capture_check_format_geometry(1280, 720, CSI_PIXEL_FORMAT_UYVY, 1920, 1080, SIZE_MAX, NULL));
    TEST_ASSERT_EQUAL_INT(CSI_GEOM_ERR_BPP,
                          csi_capture_check_format_geometry(1280, 720, (csi_pixel_format_t)99, 1920, 1080, SIZE_MAX, &out));
    TEST_ASSERT_EQUAL_INT(CSI_GEOM_ERR_ZERO,
                          csi_capture_check_format_geometry(0, 720, CSI_PIXEL_FORMAT_UYVY, 1920, 1080, SIZE_MAX, &out));
    // Odd width (641x480 * 16 = 4,922,880 bits IS divisible by 64, so plain check_geometry would pass,
    // whereas UYVY 4:2:2 / PPA ARGB8888 requires even horizontal width!)
    TEST_ASSERT_EQUAL_INT(CSI_GEOM_ERR_ALIGN,
                          csi_capture_check_format_geometry(641, 480, CSI_PIXEL_FORMAT_UYVY, 1920, 1080, SIZE_MAX, &out));

    TEST_ASSERT_EQUAL_INT(CSI_GEOM_OK,
                          csi_capture_check_format_geometry(1280, 720, CSI_PIXEL_FORMAT_UYVY, 1920, 1080, 4147200u, &out));
    TEST_ASSERT_EQUAL_UINT32(1843200u, (uint32_t)out);

    TEST_ASSERT_EQUAL_INT(CSI_GEOM_OK,
                          csi_capture_check_format_geometry(1920, 1080, CSI_PIXEL_FORMAT_UYVY, 1920, 1080, 4147200u, &out));
    TEST_ASSERT_EQUAL_UINT32(4147200u, (uint32_t)out);

    TEST_ASSERT_EQUAL_INT(CSI_GEOM_OK,
                          csi_capture_check_format_geometry(1920, 1080, CSI_PIXEL_FORMAT_RGB888, 1920, 1080, 6220800u, &out));
    TEST_ASSERT_EQUAL_UINT32(6220800u, (uint32_t)out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_geometry_720p_rgb888_ok);
    RUN_TEST(test_geometry_exact_capacity_ok);
    RUN_TEST(test_geometry_capacity_one_short_rejected);
    RUN_TEST(test_geometry_dimension_limits);
    RUN_TEST(test_geometry_zero_and_null);
    RUN_TEST(test_geometry_bpp);
    RUN_TEST(test_geometry_64bit_alignment);
    RUN_TEST(test_geometry_int32_overflow);
    RUN_TEST(test_round_up);
    RUN_TEST(test_pp_start_uses_buffer0_without_publishing);
    RUN_TEST(test_pp_alternates_and_publishes);
    RUN_TEST(test_pp_acquire_never_returns_buffer_being_written);
    RUN_TEST(test_pp_held_forces_overwrite_of_latest);
    RUN_TEST(test_pp_skip_initial_frames);
    RUN_TEST(test_pp_release_rejects_wrong_index);
    RUN_TEST(test_pp_stop_keeps_latest_and_restart_does_not_count);
    RUN_TEST(test_pp_acquire_while_held_returns_none_even_if_latest_present);
    RUN_TEST(test_pp_overwritten_conservation_invariant_never_double_counts);
    RUN_TEST(test_format_geometry_rgb888_and_uyvy);
    return UNITY_END();
}
