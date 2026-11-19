/*
 * msc_bot: 13 のケース（BOT 6.7）。
 *   (A) 純関数 msc_bot_decide / msc_bot_resolve の全ケース（手書きの期待値。出典は各行のコメントの BOT の節）
 *   (B) 状態機械を通した、13 のケース全部（同期のモックと、完了を遅らせるモック。OUT の読み捨てと STALL の両方）
 *   (C) READ(10)/(12) の転送長（read.blocks x 2048。data_len は見ない。INTERFACES.md 7.3）
 * 期待値は、BOT 1.0 の 6.7.1（Hn）、6.7.2（Hi）、6.7.3（Ho）、5.2（CSW）から手で書いた。
 * msc_bot が STALL してよい（may）とされている箇所（ケース 2、3、7 の Bulk-IN）は、STALL しない設計。
 */
#include "mb_common.h"

void setUp(void) {}
void tearDown(void) {}

/* ======================= (A) 純関数 ======================= */
typedef struct {
    const char *name; /* 出典 */
    bool in_flag;
    uint32_t H;
    msc_bot_dir_t D;
    uint64_t L;
    bool out_stall;
    bool good;
    uint32_t sent;     /* 実際に送った量 */
    uint32_t received; /* 実際に受け取った量 */
    /* 期待 */
    uint8_t case_no;
    bool phase;
    msc_bot_dir_t xfer_dir;
    uint32_t xfer_len;
    bool plan_stall_out;
    uint8_t status;
    uint32_t residue;
    bool stall_in;
} pure_row_t;

#define NONE MSC_BOT_DIR_NONE
#define DIN MSC_BOT_DIR_IN
#define DOUT MSC_BOT_DIR_OUT

static const pure_row_t PURE[] = {
    /* name, in_flag, H, D, L, out_stall, good, sent, recv, | case, phase, xfer_dir, xfer_len, plan_stall_out, status, residue, stall_in */
    /* 6.7.1 Hn。方向のビットは結果に影響しない */
    {"6.7.1 (1) Hn=Dn pass, dir=0", false, 0, NONE, 0, false, true, 0, 0, 1, false, NONE, 0, false, 0, 0, false},
    {"6.7.1 (1) Hn=Dn pass, dir=1", true, 0, NONE, 0, false, true, 0, 0, 1, false, NONE, 0, false, 0, 0, false},
    {"6.7.1 (1) Hn=Dn failed", false, 0, NONE, 0, false, false, 0, 0, 1, false, NONE, 0, false, 1, 0, false},
    {"6.7.1 (1) Hn with Di L=0 is Dn", true, 0, DIN, 0, false, true, 0, 0, 1, false, NONE, 0, false, 0, 0, false},
    {"6.7.1 (2) Hn<Di dir=0", false, 0, DIN, 36, false, true, 0, 0, 2, true, NONE, 0, false, 2, 0, false},
    {"6.7.1 (2) Hn<Di dir=1, cmd failed -> still 2", true, 0, DIN, 36, false, false, 0, 0, 2, true, NONE, 0, false, 2, 0, false},
    {"6.7.1 (3) Hn<Do", false, 0, DOUT, 8, false, true, 0, 0, 3, true, NONE, 0, false, 2, 0, false},
    {"6.7.1 (3) Hn<Do out_stall: Hn never stalls OUT", true, 0, DOUT, 8, true, true, 0, 0, 3, true, NONE, 0, false, 2, 0, false},
    /* 6.7.2 Hi */
    {"6.7.2 (4) Hi>Dn pass: STALL IN, residue=H", true, 36, NONE, 0, false, true, 0, 0, 4, false, NONE, 0, false, 0, 36, true},
    {"6.7.2 (4) Hi>Dn failed: STALL IN, residue=H", true, 36, NONE, 0, false, false, 0, 0, 4, false, NONE, 0, false, 1, 36, true},
    {"6.7.2 (5) Hi>Di pass: send L, STALL IN, residue=H-L", true, 512, DIN, 36, false, true, 36, 0, 5, false, DIN, 36, false, 0, 476, true},
    {"6.7.2 (5) Hi>Di failed", true, 512, DIN, 36, false, false, 36, 0, 5, false, DIN, 36, false, 1, 476, true},
    {"6.7.2 (6) Hi=Di pass: no STALL, residue=0", true, 36, DIN, 36, false, true, 36, 0, 6, false, DIN, 36, false, 0, 0, false},
    {"6.7.2 (6) Hi=Di failed", true, 36, DIN, 36, false, false, 36, 0, 6, false, DIN, 36, false, 1, 0, false},
    {"6.7.2 (7) Hi<Di: send H, status 2, no STALL (may)", true, 16, DIN, 36, false, true, 16, 0, 7, true, DIN, 16, false, 2, 0, false},
    {"6.7.2 (7) Hi<Di, cmd failed -> still 2", true, 16, DIN, 36, false, false, 16, 0, 7, true, DIN, 16, false, 2, 0, false},
    {"6.7.2 (8) Hi<>Do: STALL IN (sent 0 < H), status 2, residue=H", true, 16, DOUT, 16, false, true, 0, 0, 8, true, NONE, 0, false, 2, 16, true},
    /* 6.7.3 Ho。読み捨て（既定） */
    {"6.7.3 (9) Ho>Dn pass: accept H, residue=H", false, 512, NONE, 0, false, true, 0, 512, 9, false, DOUT, 512, false, 0, 512, false},
    {"6.7.3 (9) Ho>Dn failed (WRITE of a read-only medium)", false, 512, NONE, 0, false, false, 0, 512, 9, false, DOUT, 512, false, 1, 512, false},
    {"6.7.3 (10) Ho<>Di: accept H, status 2", false, 512, DIN, 36, false, true, 0, 512, 10, true, DOUT, 512, false, 2, 0, false},
    {"6.7.3 (10) Ho<>Di: host sent only 100 -> residue=H-100", false, 512, DIN, 36, false, true, 0, 100, 10, true, DOUT, 512, false, 2, 412, false},
    {"6.7.3 (11) Ho>Do: residue=H-L", false, 512, DOUT, 100, false, true, 0, 512, 11, false, DOUT, 512, false, 0, 412, false},
    {"6.7.3 (12) Ho=Do: residue=0", false, 512, DOUT, 512, false, true, 0, 512, 12, false, DOUT, 512, false, 0, 0, false},
    {"6.7.3 (12) Ho=Do failed", false, 512, DOUT, 512, false, false, 0, 512, 12, false, DOUT, 512, false, 1, 0, false},
    {"6.7.3 (13) Ho<Do: accept H, status 2", false, 512, DOUT, 1000, false, true, 0, 512, 13, true, DOUT, 512, false, 2, 0, false},
    /* 6.7.3 Ho。Bulk-OUT を STALL して打ち切る（out_stall）。受け取った量は 0 */
    {"6.7.3 (9) out_stall", false, 512, NONE, 0, true, true, 0, 0, 9, false, NONE, 0, true, 0, 512, false},
    {"6.7.3 (10) out_stall", false, 512, DIN, 36, true, true, 0, 0, 10, true, NONE, 0, true, 2, 512, false},
    {"6.7.3 (11) out_stall: nothing processed -> residue=H", false, 512, DOUT, 100, true, true, 0, 0, 11, false, NONE, 0, true, 0, 512, false},
    {"6.7.3 (12) out_stall", false, 512, DOUT, 512, true, true, 0, 0, 12, false, NONE, 0, true, 0, 512, false},
    {"6.7.3 (13) out_stall", false, 512, DOUT, 1000, true, true, 0, 0, 13, true, NONE, 0, true, 2, 512, false},
    /* 境界と、不正な入力 */
    {"Hn: a Do with L=0 is Dn (case 1)", false, 0, DOUT, 0, false, true, 0, 0, 1, false, NONE, 0, false, 0, 0, false},
    {"invalid dev_dir value is Dn", true, 36, (msc_bot_dir_t)7, 36, false, true, 0, 0, 4, false, NONE, 0, false, 0, 36, true},
    {"H = 0xFFFFFFFF with Dn: residue=H", true, 0xFFFFFFFFu, NONE, 0, false, true, 0, 0, 4, false, NONE, 0, false, 0, 0xFFFFFFFFu, true},
    {"L = 2^40 (> any H): case 7, send H only", true, 2048, DIN, (uint64_t)1 << 40, false, true, 2048, 0, 7, true, DIN, 2048, false, 2, 0, false},
    {"L = H + 1: case 7", true, 2048, DIN, 2049, false, true, 2048, 0, 7, true, DIN, 2048, false, 2, 0, false},
    {"L = H - 1: case 5", true, 2048, DIN, 2047, false, true, 2047, 0, 5, false, DIN, 2047, false, 0, 1, true},
    {"Ho: L = H + 1: case 13", false, 2048, DOUT, 2049, false, true, 0, 2048, 13, true, DOUT, 2048, false, 2, 0, false},
    {"Ho: L = H - 1: case 11", false, 2048, DOUT, 2047, false, true, 0, 2048, 11, false, DOUT, 2048, false, 0, 1, false},
    /* IN の途中の失敗: 送った量が少ない（6.7.2: H に満たなければ STALL。residue = H - 送った量） */
    {"IN failed mid-way: sent 2048 of 8192", true, 8192, DIN, 8192, false, false, 2048, 0, 6, false, DIN, 8192, false, 1, 6144, true},
    {"IN failed mid-way, case 7 keeps status 2", true, 4096, DIN, 8192, false, false, 2048, 0, 7, true, DIN, 4096, false, 2, 2048, true},
    {"sent > H is clamped to H", true, 16, DIN, 16, false, true, 100, 0, 6, false, DIN, 16, false, 0, 0, false},
    {"received > H is clamped to H (Ho>Dn)", false, 16, NONE, 0, false, true, 0, 100, 9, false, DOUT, 16, false, 0, 16, false},
};

static void test_pure_all_rows(void) {
    for (size_t i = 0; i < sizeof(PURE) / sizeof(PURE[0]); i++) {
        const pure_row_t *r = &PURE[i];
        msc_bot_plan_t p = msc_bot_decide(r->in_flag, r->H, r->D, r->L, r->out_stall);
        msc_bot_csw_t c = msc_bot_resolve(&p, r->good, r->sent, r->received);
        char msg[160];
        snprintf(msg, sizeof(msg), "row %u: %s", (unsigned)i, r->name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(r->case_no, p.case_no, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->phase, p.phase_error, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->xfer_dir, p.xfer_dir, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(r->xfer_len, p.xfer_len, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->plan_stall_out, p.stall_out, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(r->status, c.status, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(r->residue, c.residue, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->stall_in, c.stall_in, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->plan_stall_out, c.stall_out, msg);
    }
}

static void test_pure_every_case_number_is_reachable(void) {
    bool seen[14] = {false};
    for (size_t i = 0; i < sizeof(PURE) / sizeof(PURE[0]); i++) {
        msc_bot_plan_t p = msc_bot_decide(PURE[i].in_flag, PURE[i].H, PURE[i].D, PURE[i].L, PURE[i].out_stall);
        TEST_ASSERT_TRUE(p.case_no >= 1 && p.case_no <= 13);
        seen[p.case_no] = true;
    }
    for (int k = 1; k <= 13; k++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[k], "a case of the 13 is not covered");
    }
}

/* 6.7.1: Hn は方向のビットを無視する。全ての (H = 0) の組み合わせで、ビットが結果に影響しない。 */
static void test_pure_hn_ignores_direction_bit(void) {
    msc_bot_dir_t ds[3] = {NONE, DIN, DOUT};
    for (int d = 0; d < 3; d++) {
        msc_bot_plan_t a = msc_bot_decide(false, 0, ds[d], 8, false);
        msc_bot_plan_t b = msc_bot_decide(true, 0, ds[d], 8, false);
        TEST_ASSERT_EQUAL_UINT8(a.case_no, b.case_no);
        TEST_ASSERT_EQUAL_INT(a.host_dir, b.host_dir);
        TEST_ASSERT_EQUAL_INT(NONE, a.host_dir);
    }
}

/* residue は H を超えない（BOT 5.2 dCSWDataResidue）。全てのケースと量で。 */
static void test_pure_residue_never_exceeds_h(void) {
    msc_bot_dir_t ds[3] = {NONE, DIN, DOUT};
    uint32_t hs[] = {0, 1, 511, 512, 2048, 4096, 0xFFFFFFFFu};
    uint64_t ls[] = {0, 1, 512, 2048, 4097, (uint64_t)1 << 33};
    uint32_t amounts[] = {0, 1, 2048, 5000, 0xFFFFFFFFu};
    for (int f = 0; f < 2; f++)
        for (int d = 0; d < 3; d++)
            for (size_t h = 0; h < sizeof(hs) / sizeof(hs[0]); h++)
                for (size_t l = 0; l < sizeof(ls) / sizeof(ls[0]); l++)
                    for (int os = 0; os < 2; os++)
                        for (size_t a = 0; a < sizeof(amounts) / sizeof(amounts[0]); a++) {
                            msc_bot_plan_t p = msc_bot_decide(f != 0, hs[h], ds[d], ls[l], os != 0);
                            msc_bot_csw_t c = msc_bot_resolve(&p, true, amounts[a], amounts[a]);
                            TEST_ASSERT_TRUE(c.residue <= hs[h]);
                            TEST_ASSERT_TRUE(c.status <= 2);
                        }
}

/* ======================= (B) 状態機械を通した 13 のケース ======================= */
typedef struct {
    const char *name;
    uint8_t cdb[16];
    uint8_t cdb_len;
    uint32_t H;
    bool in;
    /* 期待（既定: 読み捨て）*/
    uint8_t status;
    uint32_t residue;
    uint32_t rx_len;      /* ホストが受け取ったデータ */
    uint32_t out_taken;   /* デバイスが受け取って読み捨てたデータ（H のうち） */
    bool in_stalled;      /* CSW の前に Bulk-IN が STALL された */
    const char *log;      /* 読み捨てのときの、呼び出しの記録（buf_size = 2048 のとき） */
    const char *log_out_stall; /* out_stall のときの記録（NULL = log と同じ） */
} case_row_t;

/* 0xF0 の cdb: [1..2] = デバイスが OUT で受け取る予定の長さ（big endian）、[3] = good */
static const case_row_t CASES[] = {
    {"(1) Hn=Dn pass: TEST UNIT READY", {0x00}, 6, 0, false, 0, 0, 0, 0, false, "exec done in13", NULL},
    {"(1) Hn=Dn failed: unknown opcode", {0xFF}, 6, 0, true, 1, 0, 0, 0, false, "exec done in13", NULL},
    {"(2) Hn<Di: INQUIRY with H=0", {0x12, 0, 0, 0, 36, 0}, 6, 0, true, 2, 0, 0, 0, false, "exec done in13", NULL},
    {"(3) Hn<Do: device wants OUT data, H=0", {0xF0, 0, 8, 1}, 6, 0, false, 2, 0, 0, 0, false, "exec done in13", NULL},
    {"(4) Hi>Dn pass: TUR with H=36 IN", {0x00}, 6, 36, true, 0, 36, 0, 0, true, "exec done stallI in13", NULL},
    {"(4) Hi>Dn failed: WRITE(10) with H=36 IN", {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0}, 10, 36, true, 1, 36, 0, 0, true,
     "exec done stallI in13", NULL},
    {"(5) Hi>Di: INQUIRY H=100", {0x12, 0, 0, 0, 36, 0}, 6, 100, true, 0, 64, 36, 0, true, "exec done in36 stallI in13", NULL},
    {"(6) Hi=Di: INQUIRY H=36", {0x12, 0, 0, 0, 36, 0}, 6, 36, true, 0, 0, 36, 0, false, "exec done in36 in13", NULL},
    {"(7) Hi<Di: INQUIRY H=16", {0x12, 0, 0, 0, 36, 0}, 6, 16, true, 2, 0, 16, 0, false, "exec done in16 in13", NULL},
    {"(8) Hi<>Do: device wants OUT data, H=16 IN", {0xF0, 0, 16, 1}, 6, 16, true, 2, 16, 0, 0, true,
     "exec done stallI in13", NULL},
    {"(9) Ho>Dn: WRITE(10) H=512 OUT (DATA PROTECT)", {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0}, 10, 512, false, 1, 512, 0, 512, false,
     "exec done out512 in13", "exec done stallO in13"},
    {"(10) Ho<>Di: INQUIRY with H=512 OUT", {0x12, 0, 0, 0, 36, 0}, 6, 512, false, 2, 0, 0, 512, false,
     "exec done out512 in13", "exec done stallO in13"},
    {"(11) Ho>Do: device takes 100, H=512 OUT", {0xF0, 0, 100, 1}, 6, 512, false, 0, 412, 0, 512, false,
     "exec done out512 in13", "exec done stallO in13"},
    {"(12) Ho=Do: device takes 512, H=512 OUT", {0xF0, 2, 0, 1}, 6, 512, false, 0, 0, 0, 512, false,
     "exec done out512 in13", "exec done stallO in13"},
    {"(12) Ho=Do failed (cmd failed after taking the data)", {0xF0, 2, 0, 0}, 6, 512, false, 1, 0, 0, 512, false,
     "exec done out512 in13", "exec done stallO in13"},
    {"(13) Ho<Do: device wants 1000, H=512 OUT", {0xF0, 3, 232, 1}, 6, 512, false, 2, 0, 0, 512, false,
     "exec done out512 in13", "exec done stallO in13"},
};

static void run_case(const case_row_t *r, bool sync, bool out_stall, uint32_t buf_size) {
    mb_t *m = mb_init(buf_size, out_stall);
    m->sync = sync;
    mb_start(m);
    mb_clear_log(m);
    m->host_out_left = out_stall ? 0 : (r->in ? 0 : r->H); /* OUT のケースだけ、ホストが送る */
    mb_send_cbw(m, 0xC0DE0000u + r->H, r->H, r->in, r->cdb, r->cdb_len);
    mb_settle(m);
    if (out_stall && !r->in && r->H > 0) {
        /* Bulk-OUT が STALL されている。CSW は届いている。ホストは、OUT をクリアして、次のコマンドに進む。 */
        TEST_ASSERT_TRUE(m->hw_out_stalled);
        mb_host_clear_halt(m, MSC_BOT_EP_OUT);
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "%s sync=%d out_stall=%d buf=%u", r->name, sync, out_stall, (unsigned)buf_size);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("IDLE", msc_bot_state_name(msc_bot_state(&m->bot)), msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, m->n_csw, msg);
    TEST_ASSERT_TRUE_MESSAGE(m->csw_rx.valid, msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(MSC_BOT_CSW_SIGNATURE, m->csw_rx.sig, msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xC0DE0000u + r->H, m->csw_rx.tag, msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r->status, m->csw_rx.status, msg);
    uint32_t residue = r->residue;
    if (out_stall && !r->in && r->H > 0) {
        residue = r->H; /* 受け取ったデータがない（BOT 6.7.3: STALL で打ち切る）。受け取る予定の分は処理していない */
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(residue, m->csw_rx.residue, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r->rx_len, m->rx_len, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, m->n_done, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, m->n_exec, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, m->live, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, m->host_out_left, msg);
    /* 呼び出しの順序 */
    const char *want = (out_stall && r->log_out_stall != NULL) ? r->log_out_stall : r->log;
    if (out_stall && r->log_out_stall != NULL) {
        /* 最後に OUT をクリアしたので、次の CBW の受信が発行される */
        char full[200];
        snprintf(full, sizeof(full), "%s out31", want);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(full, mb_log_str(m), msg);
        TEST_ASSERT_TRUE_MESSAGE(m->out_pending, msg);
    } else {
        /* Bulk-IN の STALL があったケースは、ホストがクリアした後に CSW。IDLE では次の CBW の受信が出ている */
        char full[200];
        snprintf(full, sizeof(full), "%s out31", want);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(full, mb_log_str(m), msg);
    }
    /* IN の STALL が必要だったかは、ログの stallI と一致 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(r->in_stalled, strstr(mb_log_str(m), "stallI") != NULL, msg);
    TEST_ASSERT_FALSE_MESSAGE(m->hw_in_stalled, msg);
    /* データの中身 */
    if (r->rx_len == 36) {
        for (uint32_t i = 0; i < 36; i++) TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(0xA0u + i), m->rx[i], msg);
    }
}

static void test_all_13_cases_sync_discard(void) {
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) run_case(&CASES[i], true, false, 2048);
}
static void test_all_13_cases_delayed_discard(void) {
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) run_case(&CASES[i], false, false, 2048);
}
static void test_all_13_cases_sync_out_stall(void) {
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) run_case(&CASES[i], true, true, 2048);
}
static void test_all_13_cases_delayed_out_stall(void) {
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) run_case(&CASES[i], false, true, 2048);
}
static void test_all_13_cases_large_buffer(void) {
    /* バッファが大きくても同じ（1 回の転送の最大がバッファの大きさ） */
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) run_case(&CASES[i], false, false, 8192);
}

/* 13 のケースを全部実行したことの確認（番号の被覆）。 */
static void test_cases_table_covers_1_to_13(void) {
    bool seen[14] = {false};
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        /* 期待するケース番号は、行の名前の先頭 "(n)" */
        int n = atoi(CASES[i].name + 1);
        TEST_ASSERT_TRUE(n >= 1 && n <= 13);
        seen[n] = true;
    }
    for (int k = 1; k <= 13; k++) TEST_ASSERT_TRUE(seen[k]);
}

/* 実際に選ばれたケース番号が、名前の番号と一致する（msc_bot_decide を、モックの結果で直接確かめる）。 */
static void test_cases_numbers_match_the_decision(void) {
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        const case_row_t *r = &CASES[i];
        mb_t *m = mb_init(2048, false);
        msc_bot_cmd_t c;
        memset(&c, 0, sizeof(c));
        memcpy(c.cdb, r->cdb, sizeof(c.cdb));
        c.out = m->buf[0];
        c.out_cap = 2048;
        c.seq = 1;
        msc_bot_cmd_result_t res = mb_exec_result(m, &c);
        uint64_t l = res.good && res.is_read ? (uint64_t)res.read_blocks * 2048u : res.data_len;
        msc_bot_dir_t d = res.good && res.is_read ? DIN : res.dev_dir;
        msc_bot_plan_t p = msc_bot_decide(r->in, r->H, d, l, false);
        TEST_ASSERT_EQUAL_INT_MESSAGE(atoi(r->name + 1), p.case_no, r->name);
    }
}

/* ======================= (C) READ の転送長 ======================= */
/* INTERFACES.md 7.3 の必須テスト: READ(10)、blocks = 1、expected_len = 2048、data_len = 0 が、データ IN として完了する。 */
static void test_read10_blocks1_expected2048_datalen0_completes_as_data_in(void) {
    for (int sync = 0; sync < 2; sync++) {
        mb_t *m = mb_init(2048, false);
        m->sync = sync != 0;
        mb_start(m);
        mb_clear_log(m);
        mb_send_read10(m, 0x11, 5, 1, 2048);
        mb_settle(m);
        mb_assert_clean(m);
        MB_ASSERT_STATE(m, MSC_BOT_IDLE);
        TEST_ASSERT_EQUAL_UINT32(2048, m->rx_len);
        mb_assert_rx_pattern(m, 5, 0, 2048);
        mb_assert_csw(m, 0x11, 0, 0);
        TEST_ASSERT_EQUAL_UINT(1, m->n_done);
        MB_ASSERT_LOG(m, "exec read1 done in2048 in13 out31");
        TEST_ASSERT_FALSE(strstr(mb_log_str(m), "stallI") != NULL);
    }
}

/* data_len に値が入っていても、READ の転送長には使わない（blocks x 2048 だけ）。 */
static void test_read_ignores_data_len_even_if_nonzero(void) {
    mb_t *m = mb_init(2048, false);
    m->sync = true;
    m->script_on = true;
    memset(&m->script, 0, sizeof(m->script));
    m->script.good = true;
    m->script.is_read = true;
    m->script.read_lba = 7;
    m->script.read_blocks = 1;
    m->script.data_len = 100; /* 使われない */
    m->script.dev_dir = DOUT; /* 使われない（READ は IN） */
    mb_start(m);
    mb_send_read10(m, 1, 7, 1, 2048);
    mb_settle(m);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT32(2048, m->rx_len);
    mb_assert_rx_pattern(m, 7, 0, 2048);
    mb_assert_csw(m, 1, 0, 0);
}

static void test_read12_two_blocks_exact(void) {
    mb_t *m = mb_init(4096, false);
    mb_start(m);
    mb_send_read12(m, 2, 100, 2, 4096);
    mb_settle(m);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT32(4096, m->rx_len);
    mb_assert_rx_pattern(m, 100, 0, 4096);
    mb_assert_csw(m, 2, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, m->in_xfers); /* 1 回の転送（バッファの大きさ 4096） */
}

/* READ の case 5（Hi > Di）: H が大きい。送れる分だけ送り、STALL、residue。 */
static void test_read_hi_greater_than_di_case5(void) {
    mb_t *m = mb_init(4096, false);
    mb_start(m);
    mb_send_read10(m, 3, 0, 2, 8192); /* デバイスは 4096、ホストは 8192 */
    mb_settle(m);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT32(4096, m->rx_len);
    mb_assert_csw(m, 3, 0, 4096);
    TEST_ASSERT_TRUE(strstr(mb_log_str(m), "stallI") != NULL);
}

/* READ の case 7（Hi < Di）: H が小さい（1000）。1 ブロック読んで 1000 バイト送り、Phase Error。 */
static void test_read_hi_less_than_di_case7(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_read10(m, 4, 9, 4, 1000);
    mb_settle(m);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT32(1000, m->rx_len);
    mb_assert_rx_pattern(m, 9, 0, 1000);
    mb_assert_csw(m, 4, 2, 0);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_EQUAL_UINT(1, m->n_read);
    TEST_ASSERT_FALSE(strstr(mb_log_str(m), "stallI") != NULL);
}

/* READ の blocks = 0: デバイスは Dn。H = 0 なら case 1、H > 0 なら case 4。 */
static void test_read_blocks0(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_read10(m, 5, 0, 0, 0);
    mb_settle(m);
    mb_assert_clean(m);
    mb_assert_csw(m, 5, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0, m->n_read);
    mb_send_read10(m, 6, 0, 0, 2048);
    mb_settle(m);
    mb_assert_clean(m);
    mb_assert_csw(m, 6, 0, 2048);
    TEST_ASSERT_TRUE(strstr(mb_log_str(m), "stallI") != NULL);
    TEST_ASSERT_EQUAL_UINT(2, m->n_exec);
    TEST_ASSERT_EQUAL_UINT(2, m->n_done);
}

/* READ が EXECUTE の段階で失敗（範囲外など。CHECK CONDITION）: データなし、CSW Failed。is_read が立っていても無視。 */
static void test_read_execute_check_condition_no_data(void) {
    mb_t *m = mb_init(2048, false);
    m->script_on = true;
    memset(&m->script, 0, sizeof(m->script));
    m->script.good = false;
    m->script.is_read = true; /* 失敗なので、無視する */
    m->script.read_blocks = 4;
    mb_start(m);
    mb_send_read10(m, 7, 0xFFFFFF, 4, 8192);
    mb_settle(m);
    mb_assert_clean(m);
    mb_assert_csw(m, 7, 1, 8192);
    TEST_ASSERT_EQUAL_UINT32(0, m->rx_len);
    TEST_ASSERT_EQUAL_UINT(0, m->n_read);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
}

/* READ(12) の blocks = 2^32 - 1 に近い大きな値（blocks x 2048 が 32 ビットを超える）。H は 32 ビット。
 * デバイスの量は 64 ビットで比べるので、case 7（Hi < Di）になり、H だけ送る。 */
static void test_read12_huge_blocks_does_not_overflow(void) {
    mb_t *m = mb_init(2048, false);
    m->sync = true;
    mb_start(m);
    mb_send_read12(m, 8, 0, 0x00200001u, 4096); /* 0x200001 x 2048 = 2^32 + 2048 */
    mb_settle(m);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT32(4096, m->rx_len);
    mb_assert_csw(m, 8, 2, 0);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
}

/* out_stall: OUT の STALL を受けた後、CSW の受信が終わる前に、ホストが Bulk-OUT の halt を解除する（BOT 6.7.3 のホスト:
 * OUT をクリアしてから CSW を読む）。解除の時点では、次の CBW の受信を発行せず（CSW の転送中）、CSW の完了後に 1 回だけ発行する。 */
static void test_out_stall_cleared_before_csw_completes_arms_cbw_once_after_csw(void) {
    mb_t *m = mb_init(2048, true);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_write10(m, 0x71, 512);
    mb_complete_cmd(m); /* ケース 9: Bulk-OUT を STALL して、CSW を発行 */
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_SEND);
    TEST_ASSERT_TRUE(m->hw_out_stalled && m->in_pending);
    mb_host_clear_halt(m, MSC_BOT_EP_OUT); /* CSW の受信の前に、OUT をクリア */
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_SEND);
    TEST_ASSERT_FALSE(m->out_pending); /* まだ、次の CBW の受信を発行しない */
    MB_ASSERT_LOG(m, "exec done stallO in13");
    mb_deliver_in(m, true); /* CSW の完了 */
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    MB_ASSERT_LOG(m, "exec done stallO in13 out31"); /* 1 回だけ */
    TEST_ASSERT_TRUE(m->out_pending && m->out_ptr == m->cbw);
    mb_assert_csw(m, 0x71, 1, 512);
    mb_assert_clean(m);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pure_all_rows);
    RUN_TEST(test_pure_every_case_number_is_reachable);
    RUN_TEST(test_pure_hn_ignores_direction_bit);
    RUN_TEST(test_pure_residue_never_exceeds_h);
    RUN_TEST(test_all_13_cases_sync_discard);
    RUN_TEST(test_all_13_cases_delayed_discard);
    RUN_TEST(test_all_13_cases_sync_out_stall);
    RUN_TEST(test_all_13_cases_delayed_out_stall);
    RUN_TEST(test_all_13_cases_large_buffer);
    RUN_TEST(test_cases_table_covers_1_to_13);
    RUN_TEST(test_cases_numbers_match_the_decision);
    RUN_TEST(test_read10_blocks1_expected2048_datalen0_completes_as_data_in);
    RUN_TEST(test_read_ignores_data_len_even_if_nonzero);
    RUN_TEST(test_read12_two_blocks_exact);
    RUN_TEST(test_read_hi_greater_than_di_case5);
    RUN_TEST(test_read_hi_less_than_di_case7);
    RUN_TEST(test_read_blocks0);
    RUN_TEST(test_read_execute_check_condition_no_data);
    RUN_TEST(test_read12_huge_blocks_does_not_overflow);
    RUN_TEST(test_out_stall_cleared_before_csw_completes_arms_cbw_once_after_csw);
    return UNITY_END();
}
