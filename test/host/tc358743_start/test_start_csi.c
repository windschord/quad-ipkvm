#include "unity.h"
#include "tc358743_start.h"
#include <string.h>
#include <stdio.h>

#define MAX_OPS 1000

typedef struct {
    char type; // 'R'ead, 'W'rite, 'D'elay, 'G'pio
    uint16_t reg;
    uint32_t val;
    uint32_t ms;
    bool level;
} op_record_t;

typedef struct {
    op_record_t ops[MAX_OPS];
    int count;
    uint8_t sys_status_mock;
    uint16_t chipid_mock;
    uint16_t sysctl_mock;
    uint8_t edid_mode_mock;
    uint8_t hpd_ctl_mock;
    uint8_t ddc_ctl_mock;
    uint8_t phy_ctl0_mock;
    uint8_t phy_ctl2_mock;
    uint8_t phy_en_mock;
    uint8_t hdcp_mode_mock;
    uint8_t vi_mode_mock;
    uint8_t vout_set2_mock;
    uint8_t vi_rep_mock;
    uint8_t nco_f0_mod_mock;
    uint8_t hdmi_det_mock;
    uint8_t hv_rst_mock;
    uint16_t confctl_mock;
    uint16_t pllctl0_mock;
    uint16_t pllctl1_mock;
    uint8_t vi_mute_mock;
} fake_ctx_t;

static fake_ctx_t g_ctx;

static void record_op(fake_ctx_t* ctx, char type, uint16_t reg, uint32_t val, uint32_t ms, bool level) {
    if (ctx->count < MAX_OPS) {
        ctx->ops[ctx->count].type = type;
        ctx->ops[ctx->count].reg = reg;
        ctx->ops[ctx->count].val = val;
        ctx->ops[ctx->count].ms = ms;
        ctx->ops[ctx->count].level = level;
        ctx->count++;
    }
}

static bool fake_write8(void* ctx, uint16_t reg, uint8_t val) {
    fake_ctx_t* c = (fake_ctx_t*)ctx;
    record_op(c, 'W', reg, val, 0, false);
    if (reg == 0x85C7) c->edid_mode_mock = val;
    if (reg == 0x8544) c->hpd_ctl_mock = val;
    if (reg == 0x8543) c->ddc_ctl_mock = val;
    if (reg == 0x8531) c->phy_ctl0_mock = val;
    if (reg == 0x8533) c->phy_ctl2_mock = val;
    if (reg == 0x8534) c->phy_en_mock = val;
    if (reg == 0x8560) c->hdcp_mode_mock = val;
    if (reg == 0x8570) c->vi_mode_mock = val;
    if (reg == 0x8573) c->vout_set2_mock = val;
    if (reg == 0x8576) c->vi_rep_mock = val;
    if (reg == 0x8670) c->nco_f0_mod_mock = val;
    if (reg == 0x8552) c->hdmi_det_mock = val;
    if (reg == 0x85AF) c->hv_rst_mock = val;
    if (reg == 0x857F) c->vi_mute_mock = val;
    return true;
}

static bool fake_write16(void* ctx, uint16_t reg, uint16_t val) {
    fake_ctx_t* c = (fake_ctx_t*)ctx;
    record_op(c, 'W', reg, val, 0, false);
    if (reg == 0x0002) c->sysctl_mock = val;
    if (reg == 0x0004) c->confctl_mock = val;
    if (reg == 0x0020) c->pllctl0_mock = val;
    if (reg == 0x0022) c->pllctl1_mock = val;
    return true;
}

static bool fake_write32(void* ctx, uint16_t reg, uint32_t val) {
    fake_ctx_t* c = (fake_ctx_t*)ctx;
    record_op(c, 'W', reg, val, 0, false);
    return true;
}

static bool fake_read8(void* ctx, uint16_t reg, uint8_t* val) {
    fake_ctx_t* c = (fake_ctx_t*)ctx;
    record_op(c, 'R', reg, 0, 0, false);
    if (reg == 0x8520) *val = c->sys_status_mock;
    else if (reg == 0x85C7) *val = c->edid_mode_mock;
    else if (reg == 0x8544) *val = c->hpd_ctl_mock;
    else if (reg == 0x8543) *val = c->ddc_ctl_mock;
    else if (reg == 0x8531) *val = c->phy_ctl0_mock;
    else if (reg == 0x8533) *val = c->phy_ctl2_mock;
    else if (reg == 0x8534) *val = c->phy_en_mock;
    else if (reg == 0x8560) *val = c->hdcp_mode_mock;
    else if (reg == 0x8570) *val = c->vi_mode_mock;
    else if (reg == 0x8573) *val = c->vout_set2_mock;
    else if (reg == 0x8576) *val = c->vi_rep_mock;
    else if (reg == 0x8670) *val = c->nco_f0_mod_mock;
    else if (reg == 0x8552) *val = c->hdmi_det_mock;
    else if (reg == 0x85AF) *val = c->hv_rst_mock;
    else if (reg == 0x857F) *val = c->vi_mute_mock;
    else *val = 0;
    return true;
}

static bool fake_read16(void* ctx, uint16_t reg, uint16_t* val) {
    fake_ctx_t* c = (fake_ctx_t*)ctx;
    record_op(c, 'R', reg, 0, 0, false);
    if (reg == 0x0000) *val = c->chipid_mock;
    else if (reg == 0x0002) *val = c->sysctl_mock;
    else if (reg == 0x0004) *val = c->confctl_mock;
    else if (reg == 0x0020) *val = c->pllctl0_mock;
    else if (reg == 0x0022) *val = c->pllctl1_mock;
    else *val = 0;
    return true;
}

static void fake_delay_ms(void* ctx, uint32_t ms) {
    fake_ctx_t* c = (fake_ctx_t*)ctx;
    record_op(c, 'D', 0, 0, ms, false);
}

static void fake_reset_gpio(void* ctx, bool level) {
    fake_ctx_t* c = (fake_ctx_t*)ctx;
    record_op(c, 'G', 0, 0, 0, level);
}

tc358743_io_ops_t fake_ops = {
    .ctx = &g_ctx,
    .write8 = fake_write8,
    .write16 = fake_write16,
    .write32 = fake_write32,
    .read8 = fake_read8,
    .read16 = fake_read16,
    .delay_ms = fake_delay_ms,
    .reset_gpio = fake_reset_gpio
};

void setUp(void) {
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.chipid_mock = 0x0000;
    g_ctx.sys_status_mock = 0x82; // TMDS + SYNC
}

void tearDown(void) {}

void test_success_972(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    bool locked = false;
    bool ret = tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);
    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT_TRUE(locked);

    // Verify ordering by checking key events
    int reset_idx = -1;
    int chipid_idx = -1;
    int hpd_low_idx = -1;
    int edid_idx = -1;
    int pll_idx = -1;
    int csi_idx = -1;
    int hpd_high_idx = -1;
    int lock_idx = -1;

    for (int i = 0; i < g_ctx.count; i++) {
        op_record_t* op = &g_ctx.ops[i];
        if (op->type == 'G' && op->level == false && reset_idx == -1) reset_idx = i;
        if (op->type == 'R' && op->reg == 0x0000 && chipid_idx == -1) chipid_idx = i;
        if (op->type == 'W' && op->reg == 0x8544 && op->val == 0x00 && hpd_low_idx == -1) hpd_low_idx = i;
        if (op->type == 'W' && op->reg == 0x8C00 && edid_idx == -1) edid_idx = i;
        if (op->type == 'W' && op->reg == 0x0020 && pll_idx == -1) pll_idx = i; // PLLCTL0
        if (op->type == 'W' && op->reg == 0x0210 && csi_idx == -1) csi_idx = i; // LINEINITCNT
        if (op->type == 'W' && op->reg == 0x8544 && op->val == 0x11 && hpd_high_idx == -1) hpd_high_idx = i; // HPD_OUT0 = 1, HPD_CTL0 is preserved as 0 (mock starts 0) so 0x01
        // wait the test actually uses 0x01 because we patched HPD.
        if (op->type == 'W' && op->reg == 0x8544 && op->val == 0x01 && hpd_high_idx == -1) hpd_high_idx = i;

        if (op->type == 'R' && op->reg == 0x8520 && lock_idx == -1) lock_idx = i;
    }

    TEST_ASSERT_TRUE(reset_idx < chipid_idx);
    TEST_ASSERT_TRUE(chipid_idx < hpd_low_idx);
    TEST_ASSERT_TRUE(hpd_low_idx < edid_idx);
    TEST_ASSERT_TRUE(edid_idx < pll_idx);
    TEST_ASSERT_TRUE(pll_idx < csi_idx);
    TEST_ASSERT_TRUE(csi_idx < hpd_high_idx);
    TEST_ASSERT_TRUE(hpd_high_idx < lock_idx);

    // Verify 972Mbps register values
    bool found_pll = false;
    bool found_lineinit = false;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x0020) {
            TEST_ASSERT_EQUAL_HEX16(0x308F, g_ctx.ops[i].val); // 972 pllctl0
            found_pll = true;
        }
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x0210) {
            TEST_ASSERT_EQUAL_HEX32(0x1B58, g_ctx.ops[i].val); // 972 lineinitcnt
            found_lineinit = true;
        }
    }
    TEST_ASSERT_TRUE(found_pll && found_lineinit);
}

void test_success_594(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 594, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    bool locked = false;
    bool ret = tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);
    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT_TRUE(locked);

    bool found_pll = false;
    bool found_lineinit = false;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x0020) {
            TEST_ASSERT_EQUAL_HEX16(0x3057, g_ctx.ops[i].val); // 594 pllctl0
            found_pll = true;
        }
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x0210) {
            TEST_ASSERT_EQUAL_HEX32(0x0E80, g_ctx.ops[i].val); // 594 lineinitcnt
            found_lineinit = true;
        }
    }
    TEST_ASSERT_TRUE(found_pll && found_lineinit);
}

void test_timeout(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    g_ctx.sys_status_mock = 0x00; // Never locks
    bool locked = true;
    bool ret = tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);

    // Returns true (proceeds normally) but locked is false
    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT_FALSE(locked);

    int reads = 0;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.ops[i].type == 'R' && g_ctx.ops[i].reg == 0x8520) reads++;
    }
    TEST_ASSERT_EQUAL(100, reads); // 5000ms / 50ms = 100 times
}

void test_unsupported_rejection(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 600, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    bool locked = false;
    bool ret = tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);
    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL(0, g_ctx.count); // Should reject before writing anything

    mode.lane_rate_mbps = 972; mode.lanes = 4;
    ret = tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);
    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL(0, g_ctx.count);

    mode.lanes = 2; mode.format = (tc358743_format_t)99;
    ret = tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);
    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL(0, g_ctx.count);
}

// ---- 追加: 書き込み列の固定（回帰の仕様）と I2C 失敗の伝播 ----
// 期待値は docs/P2B_TC358743_REFERENCE.md の起動手順（972Mbps・2 レーン・RGB888・非連続クロック）に
// 基づく。EDID RAM (0x8C00 以降) の中身は test_edid.c で検証するので、ここでは除外する。
#define GOLD_DELAY 0xFFFFu
#define GOLD_GPIO  0xFFFEu
typedef struct { uint16_t reg; uint32_t val; } gold_t;

static const gold_t k_gold_972[] = {
    { GOLD_GPIO, 1u },
    { GOLD_GPIO, 0u },
    { GOLD_DELAY, 10u },
    { GOLD_GPIO, 1u },
    { GOLD_DELAY, 200u },
    { 0x8544, 0x0u },
    { GOLD_DELAY, 20u },
    { 0x0002, 0xC00u },
    { 0x0002, 0xF00u },
    { 0x0002, 0xC00u },
    { 0x0002, 0xC00u },
    { 0x0006, 0x176u },
    { 0x8540, 0x8Cu },
    { 0x8541, 0xAu },
    { 0x8531, 0x0u },
    { 0x85AA, 0xEu },
    { 0x85AB, 0x1u },
    { 0x85AC, 0xF6u },
    { 0x85AD, 0x6u },
    { 0x8630, 0xB0u },
    { 0x8631, 0x1Eu },
    { 0x8632, 0x4u },
    { 0x8670, 0x1u },
    { 0x0028, 0x1A5u },
    { 0x002A, 0x1A5u },
    { 0x8534, 0x0u },
    { 0x8532, 0x80u },
    { 0x8533, 0x0u },
    { 0x8536, 0x40u },
    { 0x853F, 0xAu },
    { 0x8546, 0x2Du },
    { 0x8552, 0x0u },
    { 0x85AF, 0x0u },
    { 0x8534, 0x1u },
    { 0x8543, 0x2u },
    { 0x8560, 0x2u },
    { 0x8570, 0x0u },
    { 0x8573, 0x1u },
    { 0x8574, 0x8u },
    { 0x85C7, 0x0u },
    { 0x85CA, 0x0u },
    { 0x85CB, 0x1u },
    { 0x85C7, 0x2u },
    { GOLD_DELAY, 10u },
    { 0x857F, 0xD0u },
    { 0x0004, 0x0u },
    { 0x0002, 0xC01u },
    { 0x0020, 0x308Fu },
    { 0x0022, 0x3u },
    { GOLD_DELAY, 1u },
    { 0x0022, 0x13u },
    { 0x0002, 0xC00u },
    { 0x0002, 0xE00u },
    { 0x0002, 0xC00u },
    { 0x014C, 0x1u },
    { 0x0150, 0x1u },
    { 0x0210, 0x1B58u },
    { 0x0214, 0x7u },
    { 0x0218, 0x2806u },
    { 0x021C, 0x0u },
    { 0x0220, 0x806u },
    { 0x0224, 0x4268u },
    { 0x0228, 0x8u },
    { 0x022C, 0x5u },
    { 0x0230, 0x0u },
    { 0x0234, 0x7u },
    { 0x0238, 0x0u },
    { 0x0204, 0x1u },
    { 0x0518, 0x1u },
    { 0x0500, 0xA3008082u },
    { 0x0500, 0xB4000312u },
    { 0x0500, 0xD5000012u },
    { 0x0500, 0xA6000004u },
    { 0x8573, 0x1u },
    { 0x8576, 0x0u },
    { 0x0004, 0x0u },
    { 0x0014, 0xFFFFu },
    { 0x0016, 0xFCFFu },
    { 0x857F, 0xC0u },
    { 0x0004, 0x3u },
    { GOLD_DELAY, 150u },
    { 0x8544, 0x1u },
    { GOLD_DELAY, 50u },
    { 0x0518, 0x1u },
};

static int collect_seq(gold_t* out, int max) {
    int n = 0;
    for (int i = 0; i < g_ctx.count && n < max; i++) {
        const op_record_t* op = &g_ctx.ops[i];
        if (op->type == 'W') {
            if (op->reg >= 0x8C00) continue;
            out[n].reg = op->reg; out[n].val = op->val; n++;
        } else if (op->type == 'D') {
            out[n].reg = GOLD_DELAY; out[n].val = op->ms; n++;
        } else if (op->type == 'G') {
            out[n].reg = GOLD_GPIO; out[n].val = op->level ? 1 : 0; n++;
        }
    }
    return n;
}

void test_golden_972_write_sequence(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    bool locked = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked));
    gold_t got[MAX_OPS];
    int n = collect_seq(got, MAX_OPS);
    int expect_n = (int)(sizeof(k_gold_972) / sizeof(k_gold_972[0]));
    TEST_ASSERT_EQUAL_INT(expect_n, n);
    for (int i = 0; i < n; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "index %d", i);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(k_gold_972[i].reg, got[i].reg, msg);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(k_gold_972[i].val, got[i].val, msg);
    }
}

static uint32_t last_write_val(uint16_t reg, bool* found) {
    uint32_t v = 0;
    *found = false;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == reg) { v = g_ctx.ops[i].val; *found = true; }
    }
    return v;
}

void test_continuous_clock_sets_txoptioncntrl(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = true };
    bool locked = false, found = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked));
    TEST_ASSERT_EQUAL_HEX32(0x1, last_write_val(0x0238, &found));
    TEST_ASSERT_TRUE(found);
}

void test_594_register_set(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 594, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    bool locked = false, found = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked));
    TEST_ASSERT_EQUAL_HEX32(0x0E80, last_write_val(0x0210, &found));  // LINEINITCNT
    TEST_ASSERT_EQUAL_HEX32(0x0003, last_write_val(0x0214, &found));  // LPTXTIMECNT
    TEST_ASSERT_EQUAL_HEX32(0x1403, last_write_val(0x0218, &found));  // TCLK_HEADERCNT
    TEST_ASSERT_EQUAL_HEX32(0x0103, last_write_val(0x0220, &found));  // THS_HEADERCNT
    TEST_ASSERT_EQUAL_HEX32(0x4882, last_write_val(0x0224, &found));  // TWAKEUP
    TEST_ASSERT_EQUAL_HEX32(0x0002, last_write_val(0x022C, &found));  // THS_TRAILCNT
    TEST_ASSERT_EQUAL_HEX32(0x0000, last_write_val(0x0238, &found));  // TXOPTIONCNTRL
}

// 失敗の注入: n 回目の書き込みを失敗させ、すべての n で false が返ること。
static int g_fail_at;
static int g_write_count;
static bool inj_write8(void* c, uint16_t r, uint8_t v) { if (++g_write_count == g_fail_at) return false; return fake_write8(c, r, v); }
static bool inj_write16(void* c, uint16_t r, uint16_t v) { if (++g_write_count == g_fail_at) return false; return fake_write16(c, r, v); }
static bool inj_write32(void* c, uint16_t r, uint32_t v) { if (++g_write_count == g_fail_at) return false; return fake_write32(c, r, v); }

void test_every_write_failure_is_reported(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    tc358743_io_ops_t ops = fake_ops;
    ops.write8 = inj_write8; ops.write16 = inj_write16; ops.write32 = inj_write32;

    // まず失敗なしで書き込み回数を数える
    g_fail_at = 0; g_write_count = 0;
    memset(&g_ctx, 0, sizeof(g_ctx)); g_ctx.sys_status_mock = 0x82;
    bool locked = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked));
    int total = g_write_count;
    TEST_ASSERT_TRUE(total > 100);

    for (int k = 1; k <= total; k++) {
        memset(&g_ctx, 0, sizeof(g_ctx)); g_ctx.sys_status_mock = 0x82;
        g_fail_at = k; g_write_count = 0;
        locked = true;
        bool ret = tc358743_start_sequence(&ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);
        char msg[48];
        snprintf(msg, sizeof(msg), "write #%d failure not reported", k);
        TEST_ASSERT_FALSE_MESSAGE(ret, msg);
        TEST_ASSERT_FALSE_MESSAGE(locked, msg);
    }
}

void test_chipid_mismatch_stops_before_writes(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    g_ctx.chipid_mock = 0x4400; // 上位バイトが 0 でない
    bool locked = false;
    TEST_ASSERT_FALSE(tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked));
    for (int i = 0; i < g_ctx.count; i++) {
        TEST_ASSERT_NOT_EQUAL('W', g_ctx.ops[i].type);
    }
}

void test_null_args_rejected(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    bool locked;
    TEST_ASSERT_FALSE(tc358743_start_sequence(NULL, &mode, TC358743_EDID_PROFILE_720P_60, &locked));
    TEST_ASSERT_FALSE(tc358743_start_sequence(&fake_ops, NULL, TC358743_EDID_PROFILE_720P_60, &locked));
    TEST_ASSERT_FALSE(tc358743_start_sequence(&fake_ops, &mode, TC358743_EDID_PROFILE_720P_60, NULL));
}

// 実機 (X630 V1.5) で PHY_EN (0x8534) の電源投入時上位ビット 0x3E と PLLCTL1 (0x0022) の bit 9 (0x0200) を
// RMW で保持しないと TMDS がロックしない不具合の回帰テスト。
void test_rmw_preserves_phy_en_and_pllctl1_defaults(void) {
    tc358743_mode_t mode_972 = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    g_ctx.phy_en_mock = 0x3F;    // 上位ビット 0x3E + bit0 (ENABLE_PHY) = 1
    g_ctx.pllctl1_mock = 0x0EE0; // FRS(0x0C00)=3 + bit9(0x0200) + 予約(0x00E0)
    bool locked = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&fake_ops, &mode_972, TC358743_EDID_PROFILE_720P_60, &locked));
    TEST_ASSERT_TRUE(locked);

    uint32_t phy_writes[4] = {0};
    int phy_n = 0;
    uint32_t pll1_writes[4] = {0};
    int pll1_n = 0;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x8534 && phy_n < 4) {
            phy_writes[phy_n++] = g_ctx.ops[i].val;
        }
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x0022 && pll1_n < 4) {
            pll1_writes[pll1_n++] = g_ctx.ops[i].val;
        }
    }
    TEST_ASSERT_EQUAL_INT(2, phy_n);
    TEST_ASSERT_EQUAL_HEX8(0x3E, phy_writes[0]); // 0x3E を維持しつつ bit0 のみ 0
    TEST_ASSERT_EQUAL_HEX8(0x3F, phy_writes[1]); // 0x3E を維持しつつ bit0 を 1

    TEST_ASSERT_EQUAL_INT(2, pll1_n);
    TEST_ASSERT_EQUAL_HEX16(0x02E3, pll1_writes[0]); // FRS(0x0C00)->0, 0x02E0 維持, RESETB|PLL_EN(0x0003)=1
    TEST_ASSERT_EQUAL_HEX16(0x02F3, pll1_writes[1]); // 0x02E3 維持 + CKEN(0x0010)=1

    // 594Mbps (>500Mbps なので frs=0) かつ初期 PHY_EN=0x3E (bit0=0) でも RMW が正しく機能すること
    setUp();
    tc358743_mode_t mode_594 = { .lane_rate_mbps = 594, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    g_ctx.phy_en_mock = 0x3E;
    g_ctx.pllctl1_mock = 0x0EE0; // FRS(0x0C00)=3, bit9(0x0200)=1, 予約(0x00E0)
    locked = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&fake_ops, &mode_594, TC358743_EDID_PROFILE_720P_60, &locked));
    phy_n = 0;
    pll1_n = 0;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x8534 && phy_n < 4) {
            phy_writes[phy_n++] = g_ctx.ops[i].val;
        }
        if (g_ctx.ops[i].type == 'W' && g_ctx.ops[i].reg == 0x0022 && pll1_n < 4) {
            pll1_writes[pll1_n++] = g_ctx.ops[i].val;
        }
    }
    TEST_ASSERT_EQUAL_INT(2, phy_n);
    TEST_ASSERT_EQUAL_HEX8(0x3E, phy_writes[0]);
    TEST_ASSERT_EQUAL_HEX8(0x3F, phy_writes[1]);
    TEST_ASSERT_EQUAL_INT(2, pll1_n);
    TEST_ASSERT_EQUAL_HEX16(0x02E3, pll1_writes[0]);
    TEST_ASSERT_EQUAL_HEX16(0x02F3, pll1_writes[1]);
}

static int g_read_fail_at;
static int g_read_count;
static bool inj_read8(void* c, uint16_t r, uint8_t* v) { if (++g_read_count == g_read_fail_at) return false; return fake_read8(c, r, v); }
static bool inj_read16(void* c, uint16_t r, uint16_t* v) { if (++g_read_count == g_read_fail_at) return false; return fake_read16(c, r, v); }

void test_every_read_failure_is_reported(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    tc358743_io_ops_t ops = fake_ops;
    ops.read8 = inj_read8; ops.read16 = inj_read16;

    g_read_fail_at = 0; g_read_count = 0;
    setUp();
    bool locked = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked));
    int total = g_read_count;
    TEST_ASSERT_TRUE(total >= 20);

    for (int k = 1; k <= total; k++) {
        setUp();
        g_read_fail_at = k; g_read_count = 0;
        locked = true;
        bool ret = tc358743_start_sequence(&ops, &mode, TC358743_EDID_PROFILE_720P_60, &locked);
        char msg[48];
        snprintf(msg, sizeof(msg), "read #%d failure not reported", k);
        TEST_ASSERT_FALSE_MESSAGE(ret, msg);
        TEST_ASSERT_FALSE_MESSAGE(locked, msg);
    }
}

void test_calc_fifoctl_boundaries_and_narrow_modes(void) {
    // Standard 24 bpp RGB888 modes: line_words = width * 24 / 32 = width * 3 / 4 > 374 -> 374
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(1920u, 24u)); // 1440 words
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(1280u, 24u)); // 960 words
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(640u, 24u));  // 480 words

    // Exact boundary around line_words == 374 vs 375 (at 32 bpp, line_words == width)
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(375u, 32u));  // 375 > 374 -> 374
    TEST_ASSERT_EQUAL_UINT16(327u, tc358743_calc_fifoctl(374u, 32u));  // 374 * 7 / 8 = 327

    // Narrow / 16 bpp UYVY mode: 640 * 16 / 32 = 320 words <= 374 -> 320 * 7 / 8 = 280
    TEST_ASSERT_EQUAL_UINT16(280u, tc358743_calc_fifoctl(640u, 16u));

    // Ultra-narrow mode clamps to minimum 16 words
    TEST_ASSERT_EQUAL_UINT16(16u, tc358743_calc_fifoctl(16u, 16u));    // 8 * 7 / 8 = 7 -> 16
    TEST_ASSERT_EQUAL_UINT16(17u, tc358743_calc_fifoctl(20u, 32u));    // 20 * 7 / 8 = 17

    // Invalid / out-of-bounds inputs fall back safely to 374
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(0u, 24u));
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(1280u, 0u));
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(16385u, 24u));
    TEST_ASSERT_EQUAL_UINT16(374u, tc358743_calc_fifoctl(1280u, 65u));
}

void test_rearm_csi_sequence_972_and_594_and_narrow_fifoctl(void) {
    tc358743_mode_t mode = {
        .lane_rate_mbps = 972,
        .lanes = 2,
        .format = TC358743_FORMAT_RGB888,
        .continuous_clock = false,
    };
    g_ctx.sysctl_mock = 0x0C00;  // IRRST|CECRST preserved
    g_ctx.confctl_mock = 0x0003; // VBUFEN|ABUFEN initially active
    g_ctx.hpd_ctl_mock = 0x11;   // HPD_CTL0|HPD_OUT0 high

    // 400 px @ 24 bpp -> line_words = 300 <= 374 -> fifoctl = 262 (0x0106)
    TEST_ASSERT_TRUE(tc358743_rearm_csi_sequence(&fake_ops, &mode, 400u));

    bool saw_fifoctl = false;
    bool saw_ctxrst_set = false;
    bool saw_ctxrst_clr = false;
    bool saw_lineinit_972 = false;
    bool saw_csi_confw_last = false;
    int vi_mute_writes = 0;
    uint32_t first_vi_mute = 0, last_vi_mute = 0;
    for (int i = 0; i < g_ctx.count; i++) {
        const op_record_t* op = &g_ctx.ops[i];
        // Must NEVER touch HPD_CTL (0x8544), HDMIRST (0x0100 in SYSCTL), or reset GPIO!
        if (op->type == 'W') {
            TEST_ASSERT_NOT_EQUAL_HEX16(0x8544, op->reg);
            if (op->reg == 0x0002) {
                TEST_ASSERT_EQUAL_HEX16(0x0000, op->val & 0x0100);
                if (op->val == 0x0E00) saw_ctxrst_set = true;
                if (op->val == 0x0C00 && saw_ctxrst_set) saw_ctxrst_clr = true;
            }
            if (op->reg == 0x0006 && op->val == 262u) saw_fifoctl = true;
            if (op->reg == 0x0210 && op->val == 0x1B58u) saw_lineinit_972 = true;
            if (op->reg == 0x0500 && op->val == 0xA6000004u) saw_csi_confw_last = true;
            if (op->reg == 0x857F) {
                if (vi_mute_writes == 0) first_vi_mute = op->val;
                last_vi_mute = op->val;
                vi_mute_writes++;
            }
        }
        TEST_ASSERT_NOT_EQUAL('G', op->type);
    }
    TEST_ASSERT_TRUE(saw_fifoctl);
    TEST_ASSERT_TRUE(saw_ctxrst_set && saw_ctxrst_clr);
    TEST_ASSERT_TRUE(saw_lineinit_972);
    TEST_ASSERT_TRUE(saw_csi_confw_last);
    TEST_ASSERT_EQUAL_INT(2, vi_mute_writes);
    TEST_ASSERT_EQUAL_HEX8(0xD0, first_vi_mute);
    TEST_ASSERT_EQUAL_HEX8(0xC0, last_vi_mute);
    TEST_ASSERT_EQUAL_HEX16(0x0003, g_ctx.confctl_mock);

    // 594 Mbps + continuous_clock = true + 1920 px (fifoctl = 374)
    setUp();
    mode.lane_rate_mbps = 594;
    mode.continuous_clock = true;
    TEST_ASSERT_TRUE(tc358743_rearm_csi_sequence(&fake_ops, &mode, 1920u));
    bool found = false;
    TEST_ASSERT_EQUAL_HEX32(374u, last_write_val(0x0006, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_HEX32(0x0E80u, last_write_val(0x0210, &found));
    TEST_ASSERT_EQUAL_HEX32(0x1u, last_write_val(0x0238, &found));
    TEST_ASSERT_EQUAL_HEX32(0x1u, last_write_val(0x0518, &found));
}

void test_rearm_csi_invalid_args_and_io_failures(void) {
    tc358743_mode_t mode = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_RGB888, .continuous_clock = false };
    TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(NULL, &mode, 1280u));
    TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(&fake_ops, NULL, 1280u));
    TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(&fake_ops, &mode, 0u));

    tc358743_mode_t bad = mode;
    bad.lanes = 4;
    TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(&fake_ops, &bad, 1280u));
    bad = mode;
    bad.format = (tc358743_format_t)99;
    TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(&fake_ops, &bad, 1280u));
    bad = mode;
    bad.lane_rate_mbps = 600;
    TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(&fake_ops, &bad, 1280u));

    // Every write failure in rearm must return false
    tc358743_io_ops_t wops = fake_ops;
    wops.write8 = inj_write8; wops.write16 = inj_write16; wops.write32 = inj_write32;
    g_fail_at = 0; g_write_count = 0;
    setUp();
    TEST_ASSERT_TRUE(tc358743_rearm_csi_sequence(&wops, &mode, 1280u));
    int total_w = g_write_count;
    TEST_ASSERT_TRUE(total_w >= 20);
    for (int k = 1; k <= total_w; k++) {
        setUp();
        g_fail_at = k; g_write_count = 0;
        TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(&wops, &mode, 1280u));
    }

    // Every read failure in rearm must return false
    tc358743_io_ops_t rops = fake_ops;
    rops.read8 = inj_read8; rops.read16 = inj_read16;
    g_read_fail_at = 0; g_read_count = 0;
    setUp();
    TEST_ASSERT_TRUE(tc358743_rearm_csi_sequence(&rops, &mode, 1280u));
    int total_r = g_read_count;
    TEST_ASSERT_TRUE(total_r >= 5);
    for (int k = 1; k <= total_r; k++) {
        setUp();
        g_read_fail_at = k; g_read_count = 0;
        TEST_ASSERT_FALSE(tc358743_rearm_csi_sequence(&rops, &mode, 1280u));
    }
}

void test_hpd_nudge_sequence_and_failure_propagation(void) {
    TEST_ASSERT_FALSE(tc358743_hpd_nudge_sequence(NULL));

    setUp();
    g_ctx.hpd_ctl_mock = 0x11; // bit 4 (HPD_CTL0) must be preserved across Low -> High pulse
    g_ctx.confctl_mock = 0x0003;
    TEST_ASSERT_TRUE(tc358743_hpd_nudge_sequence(&fake_ops));

    const gold_t expect_nudge[] = {
        { 0x857F, 0xD0u },
        { 0x0004, 0x00u },
        { 0x8544, 0x10u },
        { GOLD_DELAY, 150u },
        { 0x857F, 0xC0u },
        { 0x0004, 0x03u },
        { GOLD_DELAY, 150u },
        { 0x8544, 0x11u },
        { GOLD_DELAY, 50u },
        { 0x0518, 0x01u },
    };
    gold_t got[MAX_OPS];
    int n = collect_seq(got, MAX_OPS);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(expect_nudge) / sizeof(expect_nudge[0])), n);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_HEX16(expect_nudge[i].reg, got[i].reg);
        TEST_ASSERT_EQUAL_HEX32(expect_nudge[i].val, got[i].val);
    }

    tc358743_io_ops_t wops = fake_ops;
    wops.write8 = inj_write8; wops.write16 = inj_write16; wops.write32 = inj_write32;
    g_fail_at = 0; g_write_count = 0;
    setUp();
    TEST_ASSERT_TRUE(tc358743_hpd_nudge_sequence(&wops));
    int total_w = g_write_count;
    for (int k = 1; k <= total_w; k++) {
        setUp();
        g_fail_at = k; g_write_count = 0;
        TEST_ASSERT_FALSE(tc358743_hpd_nudge_sequence(&wops));
    }

    tc358743_io_ops_t rops = fake_ops;
    rops.read8 = inj_read8; rops.read16 = inj_read16;
    g_read_fail_at = 0; g_read_count = 0;
    setUp();
    TEST_ASSERT_TRUE(tc358743_hpd_nudge_sequence(&rops));
    int total_r = g_read_count;
    for (int k = 1; k <= total_r; k++) {
        setUp();
        g_read_fail_at = k; g_read_count = 0;
        TEST_ASSERT_FALSE(tc358743_hpd_nudge_sequence(&rops));
    }
}

void test_uyvy_and_445_modes(void) {
    TEST_ASSERT_EQUAL_UINT32(24u, tc358743_format_bpp(TC358743_FORMAT_RGB888));
    TEST_ASSERT_EQUAL_UINT32(16u, tc358743_format_bpp(TC358743_FORMAT_UYVY));
    TEST_ASSERT_EQUAL_UINT32(0u, tc358743_format_bpp((tc358743_format_t)99));

    // P2b-8 Experiment 1: 972 Mbps UYVY (VOUT_SET2=0xC1, VI_REP=0x60, CONFCTL=0x00C3)
    setUp();
    tc358743_mode_t uyvy_972 = { .lane_rate_mbps = 972, .lanes = 2, .format = TC358743_FORMAT_UYVY, .continuous_clock = false };
    bool locked = false, found = false;
    TEST_ASSERT_TRUE(tc358743_start_sequence(&fake_ops, &uyvy_972, TC358743_EDID_PROFILE_1080P_30, &locked));
    TEST_ASSERT_TRUE(locked);
    TEST_ASSERT_EQUAL_HEX32(0xC1u, last_write_val(0x8573, &found));
    TEST_ASSERT_EQUAL_HEX32(0x60u, last_write_val(0x8576, &found));
    TEST_ASSERT_EQUAL_HEX32(0x00C3u, last_write_val(0x0004, &found));

    // Rearm in UYVY @ 640 px: 640 * 16 / 32 = 320 words <= 374 -> 320 * 7 / 8 = 280 FIFOCTL
    setUp();
    TEST_ASSERT_TRUE(tc358743_rearm_csi_sequence(&fake_ops, &uyvy_972, 640u));
    TEST_ASSERT_EQUAL_HEX32(280u, last_write_val(0x0006, &found));
    TEST_ASSERT_EQUAL_HEX32(0xC0u, last_write_val(0x8573, &found));
    TEST_ASSERT_EQUAL_HEX32(0x60u, last_write_val(0x8576, &found));
    TEST_ASSERT_EQUAL_HEX32(0x00C3u, last_write_val(0x0004, &found));

    // P2b-8 Experiment 2: 445.5 Mbps (fbd=66 -> PLLCTL0=0x3041, FRS=1 -> PLLCTL1=0x0413, 594 timing fallback)
    setUp();
    g_ctx.pllctl1_mock = 0x0200; // preserve bit 9
    tc358743_mode_t uyvy_445 = { .lane_rate_mbps = 445, .lanes = 2, .format = TC358743_FORMAT_UYVY, .continuous_clock = false };
    TEST_ASSERT_TRUE(tc358743_start_sequence(&fake_ops, &uyvy_445, TC358743_EDID_PROFILE_720P_60, &locked));
    TEST_ASSERT_EQUAL_HEX32(0x3041u, last_write_val(0x0020, &found));
    TEST_ASSERT_EQUAL_HEX32(0x0613u, last_write_val(0x0022, &found)); // bit9(0x0200) | FRS=1(0x0400) | CKEN|RESETB|PLL_EN(0x0013)
    TEST_ASSERT_EQUAL_HEX32(0x0E80u, last_write_val(0x0210, &found));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_success_972);
    RUN_TEST(test_success_594);
    RUN_TEST(test_timeout);
    RUN_TEST(test_unsupported_rejection);
    RUN_TEST(test_golden_972_write_sequence);
    RUN_TEST(test_continuous_clock_sets_txoptioncntrl);
    RUN_TEST(test_594_register_set);
    RUN_TEST(test_every_write_failure_is_reported);
    RUN_TEST(test_chipid_mismatch_stops_before_writes);
    RUN_TEST(test_null_args_rejected);
    RUN_TEST(test_rmw_preserves_phy_en_and_pllctl1_defaults);
    RUN_TEST(test_every_read_failure_is_reported);
    RUN_TEST(test_calc_fifoctl_boundaries_and_narrow_modes);
    RUN_TEST(test_rearm_csi_sequence_972_and_594_and_narrow_fifoctl);
    RUN_TEST(test_rearm_csi_invalid_args_and_io_failures);
    RUN_TEST(test_hpd_nudge_sequence_and_failure_propagation);
    RUN_TEST(test_uyvy_and_445_modes);
    return UNITY_END();
}
