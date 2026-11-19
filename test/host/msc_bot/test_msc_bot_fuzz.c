/*
 * msc_bot: 不変条件を全ステップで検査するファズ（固定シード。ASan/UBSan で実行）。
 *
 * 乱数で、次を与える: 有効 / 不正な CBW、IN / OUT の完了（成功、失敗、短い、大きい）、依頼の完了（成功、失敗、
 * 契約違反の結果）、Reset、切断、接続、CLEAR_FEATURE(HALT)、対応する依頼がない完了、注入した関数の拒否、
 * 同期 / 遅延のモック、バッファの大きさ（2048、4096、8192）、OUT の方針（読み捨て / STALL）。
 *
 * 検査（全ステップの後）:
 *   F1  モックの違反フラグが立たない（二重の転送、STALL 中の転送、実行中の依頼があるときの command_done、
 *       範囲外の引数など。mb_common.h 参照）。イベントの FIFO があふれない
 *   F2  command_done の回数: 依頼した EXECUTE の数 - command_done の数 = 生きているコマンド（0 か 1）。
 *       bot の cmd_live と一致し、bot の計数（exec_submitted、cmd_done_called）とも一致
 *   F3  bot の転送・依頼の記録が、モックのハードウェアの状態と一致（in_pending、out_pending、cmd outstanding、halt）
 *   F4  状態ごとの形: IDLE は CBW の受信を発行済み（halt が残るときを除く）、DATA_IN は IN か依頼が進行中、
 *       STATUS_SEND は CSW が転送中、RECOVERY は両 Bulk が STALL、RESET_WAIT / DETACH_WAIT は依頼が実行中、
 *       DETACHED は何も保持しない、など
 *   F5  CSW: 署名、tag、status <= 2、residue <= H。status != 2 の IN では、ホストが受け取った量 + residue = H。
 *       IN で受け取った量が H に満たないなら、CBW の後に Bulk-IN の STALL がある（BOT 6.7.2）
 *   F6  対応する依頼がない完了（stale）は、状態も、ops の呼び出しも、変えない
 * 終わりに、依頼の完了と切断で、全てを片付け、command_done の数が、依頼した数と一致することを確かめる。
 */
#include "mb_common.h"

void setUp(void) {}
void tearDown(void) {}

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

/* 1 回の rnd() の結果を、n 未満に、または確率で使う。1 つの式・1 つの関数引数の中に、複数の rnd() を書かない
 * （C は、引数の評価順を規定しない。固定シードの再現性のため、rnd() は、1 文に 1 回だけ）。 */
static uint32_t rnd_below(uint32_t n) {
    const uint32_t r = rnd();
    return r % n;
}
static bool rnd_chance(uint32_t one_in) { return rnd_below(one_in) == 0; }

static int cur_seed;
static long cur_step;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            char b_[240];                                                            \
            snprintf(b_, sizeof(b_), "seed=%d step=%ld: %s", cur_seed, cur_step, msg); \
            TEST_FAIL_MESSAGE(b_);                                                   \
        }                                                                            \
    } while (0)

typedef struct {
    unsigned cbw_valid, cbw_invalid, csw, csw_status[3], resets, detaches, attaches;
    unsigned stale_injected, faults_injected, recoveries, exec_fail_script;
    unsigned in_data_xfers, out_data_xfers, reads_failed;
    unsigned state_seen[MSC_BOT_STATE_COUNT];
} fstats_t;
static fstats_t FS;

/* 最後に送った有効な CBW の情報（CSW の検査用） */
typedef struct {
    bool valid;
    uint32_t tag, H;
    bool in;
    uint32_t rx0;
    unsigned stall_in0;
    bool csw_seen;
} cbwctx_t;
static cbwctx_t CC;

static void check_csw(mb_t *m) {
    CHECK(CC.valid, "CSW without a valid CBW");
    CHECK(!CC.csw_seen, "two CSWs for one CBW");
    CC.csw_seen = true;
    CHECK(m->csw_rx.valid, "CSW length");
    CHECK(m->csw_rx.sig == MSC_BOT_CSW_SIGNATURE, "CSW signature");
    CHECK(m->csw_rx.tag == CC.tag, "CSW tag");
    CHECK(m->csw_rx.status <= 2, "CSW status");
    CHECK(m->csw_rx.residue <= CC.H, "CSW residue exceeds H");
    FS.csw++;
    FS.csw_status[m->csw_rx.status]++;
    if (CC.H > 0 && CC.in) {
        uint32_t got = m->rx_len - CC.rx0;
        if (m->csw_rx.status != 2) {
            CHECK(got + m->csw_rx.residue == CC.H, "IN: received + residue != H");
        }
        CHECK(got <= CC.H, "IN: sent more than H");
        if (got < CC.H) {
            CHECK(m->n_stall_in > CC.stall_in0, "IN: short transfer without a Bulk-IN STALL before the CSW");
        }
    }
    if (CC.H == 0) {
        CHECK(m->csw_rx.residue == 0, "Hn: residue must be 0");
    }
}

static void check_invariants(mb_t *m) {
    msc_bot_t *b = &m->bot;
    const msc_bot_state_t st = msc_bot_state(b);
    CHECK(m->viol == 0, m->viol_msg);
    CHECK(msc_bot_stats(b).queue_overflow == 0, "event FIFO overflow");
    CHECK((int)st >= 0 && (int)st < MSC_BOT_STATE_COUNT, "state out of range");
    FS.state_seen[st]++;

    /* F2 */
    CHECK(m->n_exec >= m->n_done, "more command_done than EXECUTE");
    CHECK(m->n_exec - m->n_done == m->live, "live != exec - done");
    CHECK(m->live <= 1, "more than one live command");
    CHECK(msc_bot_cmd_live(b) == (m->live == 1), "bot cmd_live disagrees with the mock");
    CHECK(msc_bot_stats(b).exec_submitted == m->n_exec, "exec_submitted != mock EXECUTE count");
    CHECK(msc_bot_stats(b).cmd_done_called == m->n_done, "cmd_done_called != mock count");

    /* F3 */
    CHECK(msc_bot_cmd_outstanding(b) == m->cmd_pending, "cmd outstanding mismatch");
    CHECK(b->in_pending == m->in_pending, "in_pending mismatch");
    CHECK(b->out_pending == m->out_pending, "out_pending mismatch");
    CHECK(msc_bot_halted(b, MSC_BOT_EP_IN) == m->hw_in_stalled, "IN halt mismatch");
    CHECK(msc_bot_halted(b, MSC_BOT_EP_OUT) == m->hw_out_stalled, "OUT halt mismatch");
    CHECK(!m->cmd_pending || m->live == 1, "command outstanding without a live command");

    /* F4 */
    const bool cbw_armed = m->out_pending && m->out_ptr == m->cbw;
    const bool out_data = m->out_pending && m->out_ptr != m->cbw;
    if (cbw_armed) CHECK(st == MSC_BOT_IDLE, "CBW receive armed outside IDLE");
    if (out_data) CHECK(st == MSC_BOT_DATA_OUT, "OUT data receive outside DATA_OUT");
    if (m->in_pending) CHECK(st == MSC_BOT_DATA_IN || st == MSC_BOT_STATUS_SEND, "IN transfer outside DATA_IN/STATUS_SEND");
    if (m->cmd_pending) {
        CHECK(st == MSC_BOT_EXEC || st == MSC_BOT_DATA_IN || st == MSC_BOT_RECOVERY || st == MSC_BOT_RESET_WAIT ||
                  st == MSC_BOT_DETACH_WAIT,
              "command outstanding in a state that cannot have one");
    }
    switch (st) {
    case MSC_BOT_DETACHED:
        CHECK(!m->in_pending && !m->out_pending && !m->cmd_pending && m->live == 0, "DETACHED holds something");
        CHECK(!m->hw_in_stalled && !m->hw_out_stalled, "DETACHED with a stalled pipe");
        break;
    case MSC_BOT_IDLE:
        CHECK(!m->in_pending && !m->cmd_pending && m->live == 0, "IDLE holds something");
        CHECK(cbw_armed || m->hw_in_stalled || m->hw_out_stalled, "IDLE without a CBW receive and without a halt");
        if (cbw_armed) CHECK(!m->hw_in_stalled && !m->hw_out_stalled, "CBW receive armed while a pipe is halted");
        break;
    case MSC_BOT_EXEC:
        CHECK(m->cmd_pending && m->cmd.kind == MSC_BOT_CMD_EXECUTE && m->live == 1, "EXEC without an outstanding EXECUTE");
        CHECK(!m->in_pending && !m->out_pending, "EXEC with a transfer");
        break;
    case MSC_BOT_DATA_IN:
        CHECK(m->in_pending || m->cmd_pending, "DATA_IN with no progress possible");
        CHECK(!m->out_pending, "DATA_IN with an OUT transfer");
        break;
    case MSC_BOT_DATA_OUT:
        CHECK(m->out_pending && !m->cmd_pending && m->live == 0, "DATA_OUT shape");
        break;
    case MSC_BOT_STATUS_WAIT_UNSTALL:
        CHECK(m->hw_in_stalled && !m->in_pending && !m->out_pending && !m->cmd_pending && m->live == 0,
              "STATUS_WAIT_UNSTALL shape");
        break;
    case MSC_BOT_STATUS_SEND:
        CHECK(m->in_pending && m->in_ptr == m->csw && !m->out_pending && !m->cmd_pending && m->live == 0, "STATUS_SEND shape");
        break;
    case MSC_BOT_RECOVERY:
        CHECK(m->hw_in_stalled && m->hw_out_stalled, "RECOVERY with a pipe that is not stalled");
        CHECK(!m->in_pending && !m->out_pending, "RECOVERY with a transfer");
        break;
    case MSC_BOT_RESET_WAIT:
    case MSC_BOT_DETACH_WAIT:
        CHECK(m->cmd_pending && m->live == 1, "WAIT state without an outstanding command");
        CHECK(!m->in_pending && !m->out_pending, "WAIT state with a transfer");
        break;
    default:
        CHECK(false, "bad state");
    }
}

/* ---- ホストの行動 ----------------------------------------------------------- */
static const uint32_t HS[] = {0, 0, 1, 31, 36, 64, 512, 2048, 2049, 4096, 6144, 8192, 10000, 20000};

static void host_send_cbw(mb_t *m) {
    if (!(m->out_pending && m->out_ptr == m->cbw)) return;
    uint8_t raw[31];
    uint32_t tag = rnd();
    bool invalid = rnd_below(100) < 12;
    uint8_t cdb[16];
    memset(cdb, 0, sizeof(cdb));
    uint8_t cdb_len = 6;
    uint32_t H = 0;
    bool in = rnd_below(2) != 0;
    const uint32_t kind = rnd_below(8);
    uint32_t t1, t2; /* 1 文に 1 回の rnd_below の結果 */
    switch (kind) {
    case 0: /* TUR */
        cdb[0] = 0x00;
        t1 = rnd_below(4);
        t2 = rnd_below(100);
        H = (t1 == 0) ? t2 : 0;
        break;
    case 1: /* INQUIRY */
        cdb[0] = 0x12;
        t1 = rnd_below(3);
        t2 = rnd_below(200);
        H = (t1 == 0) ? 36u : t2;
        in = rnd_below(8) != 0;
        break;
    case 2: { /* READ(10) */
        const uint32_t blocks = rnd_below(14);
        cdb_len = 10;
        cdb[0] = 0x28;
        cdb[5] = (uint8_t)rnd_below(200);
        cdb[7] = (uint8_t)(blocks >> 8);
        cdb[8] = (uint8_t)blocks;
        t1 = rnd_below(3);
        t2 = rnd_below(30000);
        H = (t1 == 0) ? t2 : blocks * 2048u;
        in = rnd_below(10) != 0;
        break;
    }
    case 3: { /* READ(12) */
        const uint32_t blocks = rnd_below(20);
        cdb_len = 12;
        cdb[0] = 0xA8;
        cdb[5] = (uint8_t)rnd_below(200);
        cdb[9] = (uint8_t)blocks;
        t1 = rnd_below(3);
        t2 = rnd_below(50000);
        H = (t1 == 0) ? t2 : blocks * 2048u;
        in = rnd_below(10) != 0;
        break;
    }
    case 4: /* WRITE(10) */
        cdb_len = 10;
        cdb[0] = 0x2A;
        t1 = rnd_below((uint32_t)(sizeof(HS) / sizeof(HS[0])));
        t2 = rnd_below(2);
        H = HS[t1];
        if (t2 == 0) {
            H = rnd_below(20000);
        }
        in = rnd_below(8) == 0;
        break;
    case 5: /* デバイスが OUT を受け取る予定のテスト用コマンド */
        cdb[0] = 0xF0;
        cdb[1] = (uint8_t)rnd_below(50);
        cdb[2] = (uint8_t)rnd_below(256);
        cdb[3] = (uint8_t)rnd_below(2);
        t1 = rnd_below(2);
        t2 = rnd_below(12000);
        H = (t1 != 0) ? t2 : 0;
        break;
    case 6: /* 未知のコマンド */
        cdb[0] = 0xFF;
        t1 = rnd_below(2);
        t2 = rnd_below(5000);
        H = (t1 != 0) ? t2 : 0;
        break;
    default: /* CDB の長さを変える */
        cdb_len = (uint8_t)(1 + rnd_below(16));
        cdb[0] = rnd_chance(2) ? 0x00 : 0x12;
        H = rnd_below(100);
        break;
    }
    mb_make_cbw(raw, tag, H, in, cdb, cdb_len);
    uint32_t alen = 31;
    if (invalid) {
        const uint32_t how = rnd_below(7);
        const uint32_t a = rnd_below(8);
        const uint32_t b2 = rnd_below(200);
        const uint32_t c2 = rnd_below(2);
        switch (how) {
        case 0: raw[a % 4] ^= (uint8_t)(1u << a); break;
        case 1: alen = (c2 != 0) ? 30 : 32; break;
        case 2: raw[13] = (uint8_t)(1 + b2); break;
        case 3: raw[14] = (c2 != 0) ? 0 : (uint8_t)(17 + b2); break;
        case 4: raw[12] = (uint8_t)(raw[12] | (1u << (a % 7))); break;
        case 5: alen = 0; break;
        default: raw[14] = (uint8_t)(raw[14] | 0x20); break;
        }
    }
    /* CSW の検査（mb_deliver_in の中から呼ばれる）のため、送る前に記録する。受け付けられなければ、後で取り消す。 */
    m->rx_len = 0;
    CC.valid = true;
    CC.tag = tag;
    CC.H = H;
    CC.in = in;
    CC.rx0 = 0;
    CC.stall_in0 = m->n_stall_in;
    CC.csw_seen = false;
    const uint32_t inv0 = msc_bot_stats(&m->bot).invalid_cbw;
    const unsigned exec0 = m->n_exec;
    if (invalid) FS.cbw_invalid++; else FS.cbw_valid++;
    mb_send_raw_cbw(m, raw, alen);
    const bool rejected = msc_bot_stats(&m->bot).invalid_cbw > inv0;
    CHECK(rejected == invalid, invalid ? "an invalid CBW was accepted" : "a valid CBW was rejected as invalid");
    if (rejected) {
        CC.valid = false;
        FS.recoveries++;
    } else if (m->n_exec == exec0) {
        CC.valid = false; /* 依頼を断られた（refuse_submit）。コマンドは実行されていない */
    }
}

static void host_deliver_in(mb_t *m) {
    if (!m->in_pending || m->hw_in_stalled) return;
    unsigned r = rnd() % 100;
    const bool is_csw = (m->in_ptr == m->csw);
    if (r < 93) {
        mb_deliver_in(m, true);
        if (!is_csw) FS.in_data_xfers++;
    } else if (r < 97) {
        m->in_pending = false;
        msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, m->in_len, false);
        FS.faults_injected++;
    } else {
        uint32_t len = m->in_len;
        m->in_pending = false;
        msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, len > 0 ? len - 1 : 0, true);
        FS.faults_injected++;
    }
}

static void host_deliver_out(mb_t *m) {
    if (!m->out_pending || m->out_ptr == m->cbw || m->hw_out_stalled) return;
    unsigned r = rnd() % 100;
    uint32_t len = m->out_len;
    if (r < 85) {
        mb_deliver_out(m, len, true);
        FS.out_data_xfers++;
    } else if (r < 92) {
        mb_deliver_out(m, rnd() % len, true); /* 短い */
        FS.out_data_xfers++;
    } else if (r < 96) {
        m->out_pending = false;
        msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, 0, false);
        FS.faults_injected++;
    } else {
        m->out_pending = false;
        msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, len + 1, true);
        FS.faults_injected++;
    }
}

static void task_complete(mb_t *m) {
    if (!m->cmd_pending) return;
    const uint32_t scripted = rnd_below(100);
    if (m->cmd.kind == MSC_BOT_CMD_EXECUTE && scripted < 10) {
        /* 結果を、乱数で与える（契約違反の結果を含む） */
        m->script_on = true;
        memset(&m->script, 0, sizeof(m->script));
        m->script.good = rnd_below(3) != 0;
        m->script.dev_dir = (msc_bot_dir_t)rnd_below(4); /* 3 は不正な値 */
        const uint32_t big = rnd_below(4);
        const uint32_t len_big = rnd_below(12000);
        const uint32_t len_small = rnd_below(100);
        m->script.data_len = (big == 0) ? len_big : len_small;
        m->script.is_read = rnd_below(2) != 0;
        m->script.read_lba = rnd_below(1000);
        m->script.read_blocks = rnd_below(12);
        FS.exec_fail_script++;
        mb_complete_cmd(m);
        m->script_on = false;
        return;
    }
    if (m->cmd.kind == MSC_BOT_CMD_READ_BLOCKS && scripted < 8) {
        int saved = m->read_fail_nth;
        m->read_fail_nth = m->reads_in_cmd;
        mb_complete_cmd(m);
        m->read_fail_nth = saved;
        FS.reads_failed++;
        return;
    }
    mb_complete_cmd(m);
}

static void spurious(mb_t *m) {
    msc_bot_state_t s0 = msc_bot_state(&m->bot);
    int nlog0 = m->nlog;
    const unsigned pick = rnd_below(3);
    const uint32_t xlen = rnd_below(100);
    const bool xok = rnd_below(2) != 0;
    if (pick == 0 && !m->in_pending) {
        msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, xlen, xok);
    } else if (pick == 1 && !m->out_pending) {
        msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, xlen, xok);
    } else if (pick == 2) {
        msc_bot_cmd_result_t r;
        memset(&r, 0, sizeof(r));
        do {
            r.seq = rnd() % 8;
        } while (m->cmd_pending && r.seq == m->cmd.seq);
        if (m->cmd_pending) {
            /* 依頼が実行中でも、seq の違う完了は、捨てられる */
        }
        r.good = (rnd() & 1) != 0;
        r.dev_dir = (msc_bot_dir_t)(rnd() % 3);
        r.data_len = rnd() % 100;
        msc_bot_command_complete(&m->bot, &r);
    } else {
        return;
    }
    FS.stale_injected++;
    CHECK(msc_bot_state(&m->bot) == s0, "a stale event changed the state");
    CHECK(m->nlog == nlog0, "a stale event caused ops calls");
}

static void one_step(mb_t *m) {
    mb_clear_log(m);
    unsigned r = rnd() % 100;
    if (r < 22) {
        host_send_cbw(m);
    } else if (r < 42) {
        host_deliver_in(m);
    } else if (r < 52) {
        host_deliver_out(m);
    } else if (r < 68) {
        task_complete(m);
    } else if (r < 71) {
        msc_bot_reset(&m->bot);
        FS.resets++;
    } else if (r < 73) {
        msc_bot_detach(&m->bot);
        FS.detaches++;
    } else if (r < 78) {
        msc_bot_attach(&m->bot);
        FS.attaches++;
    } else if (r < 86) {
        /* ホストが CLEAR_FEATURE(HALT)。STALL を待っているときは、高い確率で */
        msc_bot_ep_t ep = (rnd() & 1) ? MSC_BOT_EP_IN : MSC_BOT_EP_OUT;
        if (msc_bot_state(&m->bot) == MSC_BOT_STATUS_WAIT_UNSTALL && (rnd() % 4) != 0) ep = MSC_BOT_EP_IN;
        mb_host_clear_halt(m, ep);
    } else if (r < 90) {
        spurious(m);
    } else if (r < 92) {
        m->refuse_in = rnd_chance(8);
        m->refuse_out = rnd_chance(8);
        m->refuse_submit = rnd_chance(8);
    } else if (r < 94) {
        const bool fail = rnd_chance(3);
        const uint32_t nth = rnd_below(4);
        m->read_fail_nth = fail ? (int)nth : -1;
    } else if (r < 96) {
        m->sync = (rnd() & 1) != 0;
        m->sync_xfer = (rnd() % 4) == 0;
        m->host_out_left = rnd() % 30000;
    } else {
        /* ホストが、停止中の Bulk-IN を、通常の手順で回復する */
        if (m->hw_in_stalled) mb_host_clear_halt(m, MSC_BOT_EP_IN);
    }
    cur_step++;
    check_invariants(m);
}

static void final_drain(mb_t *m) {
    /* 全ての拒否を止め、実行中の依頼を完了させ、切断する */
    m->refuse_in = m->refuse_out = m->refuse_submit = false;
    m->sync_xfer = false;
    for (int i = 0; i < 200 && m->cmd_pending; i++) {
        mb_complete_cmd(m);
        check_invariants(m);
    }
    msc_bot_detach(&m->bot);
    check_invariants(m);
    for (int i = 0; i < 200 && m->cmd_pending; i++) {
        mb_complete_cmd(m);
        check_invariants(m);
    }
    CHECK(msc_bot_state(&m->bot) == MSC_BOT_DETACHED, "not DETACHED after the final detach");
    CHECK(m->n_exec == m->n_done, "command_done count != EXECUTE count at the end");
    CHECK(m->live == 0, "a live command at the end");
}

static void fuzz_seed(int seed, long steps) {
    cur_seed = seed;
    cur_step = 0;
    rs = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull);
    if (rs == 0) rs = 1;
    for (int i = 0; i < 4; i++) (void)rnd();
    static const uint32_t sizes[3] = {2048, 4096, 8192};
    const uint32_t size_pick = rnd_below(3);
    const bool out_stall = rnd_chance(4);
    mb_t *m = mb_init(sizes[size_pick], out_stall);
    m->sync = (rnd() & 1) != 0;
    m->sync_xfer = (rnd() % 4) == 0;
    m->host_out_left = 100000;
    m->on_csw = check_csw;
    memset(&CC, 0, sizeof(CC));
    msc_bot_attach(&m->bot);
    check_invariants(m);
    for (long i = 0; i < steps; i++) {
        one_step(m);
        /* 進まない状態に落ちないよう、たまに、お行儀のよいホストが助ける */
        if ((i % 97) == 96) {
            for (int k = 0; k < 6; k++) {
                if (m->cmd_pending) task_complete(m);
                if (m->in_pending && !m->hw_in_stalled) host_deliver_in(m);
                check_invariants(m);
            }
        }
    }
    final_drain(m);
}

static void test_fuzz_invariants(void) {
    memset(&FS, 0, sizeof(FS));
    for (int seed = 1; seed <= 300; seed++) {
        fuzz_seed(seed, 6000);
    }
    /* 空振りでないこと（固定シードなので、毎回同じ件数。下限だけを見る） */
    TEST_ASSERT_TRUE_MESSAGE(FS.cbw_valid > 20000, "fuzz: too few valid CBWs");
    TEST_ASSERT_TRUE_MESSAGE(FS.cbw_invalid > 2000, "fuzz: too few invalid CBWs");
    TEST_ASSERT_TRUE_MESSAGE(FS.csw > 5000, "fuzz: too few CSWs");
    TEST_ASSERT_TRUE_MESSAGE(FS.csw_status[0] > 500 && FS.csw_status[1] > 100 && FS.csw_status[2] > 100, "fuzz: CSW status coverage");
    TEST_ASSERT_TRUE_MESSAGE(FS.resets > 1000 && FS.detaches > 500 && FS.attaches > 1000, "fuzz: too few resets/detaches");
    TEST_ASSERT_TRUE_MESSAGE(FS.stale_injected > 2000, "fuzz: too few stale events");
    TEST_ASSERT_TRUE_MESSAGE(FS.faults_injected > 500, "fuzz: too few faults");
    TEST_ASSERT_TRUE_MESSAGE(FS.in_data_xfers > 5000 && FS.out_data_xfers > 500, "fuzz: too few data transfers");
    TEST_ASSERT_TRUE_MESSAGE(FS.reads_failed > 100, "fuzz: too few failed reads");
    for (int s = 0; s < MSC_BOT_STATE_COUNT; s++) {
        char msg[80];
        snprintf(msg, sizeof(msg), "fuzz: state %s rarely visited", msc_bot_state_name((msc_bot_state_t)s));
        TEST_ASSERT_TRUE_MESSAGE(FS.state_seen[s] > 1000, msg);
    }
    for (int s = 0; s < MSC_BOT_STATE_COUNT; s++) printf("fuzz: state %-20s visited %u\n", msc_bot_state_name((msc_bot_state_t)s), FS.state_seen[s]);
    printf("fuzz: cbw valid %u invalid %u csw %u (pass %u fail %u phase %u) resets %u detaches %u attaches %u stale %u faults %u"
           " in_xfers %u out_xfers %u reads_failed %u scripted %u\n",
           FS.cbw_valid, FS.cbw_invalid, FS.csw, FS.csw_status[0], FS.csw_status[1], FS.csw_status[2], FS.resets, FS.detaches,
           FS.attaches, FS.stale_injected, FS.faults_injected, FS.in_data_xfers, FS.out_data_xfers, FS.reads_failed,
           FS.exec_fail_script);
}

/* 拒否を入れない版（全ての転送が成功し、依頼が必ず完了する）。command_done の数が、毎回、厳密に合う。 */
static void test_fuzz_without_faults_every_command_completes(void) {
    for (int seed = 1000; seed < 1100; seed++) {
        cur_seed = seed;
        cur_step = 0;
        rs = 0x1234567ull * (uint64_t)seed + 99;
        mb_t *m = mb_init(4096, false);
        m->sync = (seed & 1) != 0;
        m->sync_xfer = (seed & 2) != 0;
        m->host_out_left = 1000000;
        memset(&CC, 0, sizeof(CC));
        msc_bot_attach(&m->bot);
        unsigned executed = 0;
        for (int i = 0; i < 400; i++) {
            m->rx_len = 0;
            /* 各反復の開始時に、CBW の受信が発行済みでなければならない（前のコマンドが、IDLE に戻っている） */
            CHECK(m->out_pending && m->out_ptr == m->cbw, "CBW receive not armed at the start of an iteration");
            uint8_t raw[31];
            const uint32_t lba_pick = rnd_below(100);
            const uint32_t blocks = 1 + rnd_below(9);
            const uint32_t tag = rnd();
            const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, (uint8_t)lba_pick, 0, 0, (uint8_t)blocks, 0};
            mb_make_cbw(raw, tag, blocks * 2048u, true, cdb, 10);
            const unsigned csw0 = m->n_csw;
            mb_send_raw_cbw(m, raw, 31);
            executed++;
            mb_settle(m);
            mb_assert_clean(m);
            CHECK(msc_bot_state(&m->bot) == MSC_BOT_IDLE, "not IDLE after a command");
            CHECK(m->n_exec == executed, "EXECUTE count");
            CHECK(m->n_done == executed, "command_done count");
            /* 今回の CSW が 1 つ届き、tag が今回のもの（前回の成功の結果で通らない） */
            CHECK(m->n_csw == csw0 + 1, "exactly one CSW for the command");
            CHECK(m->csw_rx.tag == tag, "CSW tag is not the tag of this command");
            CHECK(m->csw_rx.status == 0 && m->csw_rx.residue == 0, "READ CSW");
            CHECK(m->rx_len == blocks * 2048u, "READ data length");
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_invariants);
    RUN_TEST(test_fuzz_without_faults_every_command_completes);
    return UNITY_END();
}
