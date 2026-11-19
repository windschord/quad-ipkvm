/*
 * msc_bot: 状態遷移表（状態 x イベント -> 次の状態 + アクション）の全行を固定する。
 * 表は include/msc_bot.h の「状態遷移表」。期待値は、その表から手で書いた（コードからは作っていない）。
 *
 * 各行は、状態ごとの標準の設定（setup）を作り、イベントを 1 つ与え、次の状態、呼び出しの記録（ops の呼び出しの順序）、
 * command_done の回数の増分、「対応する依頼がないので捨てた」イベントの数の増分、を確かめる。
 * 10 状態 x 11 イベントの全てのセルを、少なくとも 1 行が埋めることを、最後のテストで確かめる。
 * 1 つのセルが、状況で複数の遷移を持つとき（IDLE の CBW の有効 / 不正、EXEC の結果の種類、データの終わりなど）は、
 * 複数の行（setup の種類）で固定する。
 */
#include "mb_common.h"

void setUp(void) {}
void tearDown(void) {}

typedef enum {
    SU_DETACHED,
    SU_IDLE,
    SU_EXEC,         /* INQUIRY H=36（データ IN になる）。依頼は実行中 */
    SU_EXEC_TUR,     /* TEST UNIT READY H=0 */
    SU_EXEC_WRITE,   /* WRITE(10) H=512 OUT */
    SU_EXEC_BAD,     /* 結果が契約違反（READ 以外の data_len > バッファ） */
    SU_DATA_IN,      /* READ 4 ブロック。in(b0) の転送中で、2 つ目の READ_BLOCKS が実行中 */
    SU_DATA_IN_LAST, /* READ 1 ブロック。in(b0) の転送中。依頼はもうない（command_done 済み） */
    SU_DATA_IN_FAILED, /* READ の 2 つ目が失敗済み。in(b0) の転送の完了を待つ */
    SU_DATA_OUT,     /* WRITE(10) H=4096。2048 バイトの受信中 */
    SU_DATA_OUT_LAST,/* WRITE(10) H=2048。最後の受信中 */
    SU_WAIT_UNSTALL, /* TEST UNIT READY H=36 IN: Bulk-IN を STALL 済み */
    SU_STATUS_SEND,  /* CSW の送信中 */
    SU_RECOVERY,     /* 不正な CBW */
    SU_RECOVERY_CMD, /* READ の途中の IN の失敗で RECOVERY。2 つ目の READ_BLOCKS が実行中 */
    SU_RESET_WAIT,
    SU_DETACH_WAIT,
    SU_RESET_WAIT_READ,  /* READ_BLOCKS を保留したまま Reset */
    SU_DETACH_WAIT_READ, /* READ_BLOCKS を保留したまま切断 */
    SU_DETACH_WAIT_ATT,  /* 切断の後、再接続（ATT）済み */
    SU_DETACH_WAIT_ATT_RST /* 切断、再接続、Reset 済み（Reset の通知が、完了の後まで保留） */
} setup_t;

typedef enum {
    E_ATT,
    E_DET,
    E_RST,
    E_HIN,
    E_HOUT,
    E_OUTOK,
    E_OUTOK_BAD_CBW, /* IDLE で、署名の違う CBW */
    E_OUTER,
    E_INOK,
    E_INER,
    E_CMDOK,
    E_CMDFAIL,
    E_COUNT
} event_t;

static const char *EVNAME[E_COUNT] = {"ATT", "DET", "RST", "HIN", "HOUT", "OUTok", "OUTok(bad CBW)", "OUTer", "INok", "INer", "CMDok", "CMDfail"};

static msc_bot_state_t setup_state_of(setup_t s) {
    switch (s) {
    case SU_DETACHED: return MSC_BOT_DETACHED;
    case SU_IDLE: return MSC_BOT_IDLE;
    case SU_EXEC: case SU_EXEC_TUR: case SU_EXEC_WRITE: case SU_EXEC_BAD: return MSC_BOT_EXEC;
    case SU_DATA_IN: case SU_DATA_IN_LAST: case SU_DATA_IN_FAILED: return MSC_BOT_DATA_IN;
    case SU_DATA_OUT: case SU_DATA_OUT_LAST: return MSC_BOT_DATA_OUT;
    case SU_WAIT_UNSTALL: return MSC_BOT_STATUS_WAIT_UNSTALL;
    case SU_STATUS_SEND: return MSC_BOT_STATUS_SEND;
    case SU_RECOVERY: case SU_RECOVERY_CMD: return MSC_BOT_RECOVERY;
    case SU_RESET_WAIT: case SU_RESET_WAIT_READ: return MSC_BOT_RESET_WAIT;
    default: return MSC_BOT_DETACH_WAIT; /* SU_DETACH_WAIT、_READ、_ATT、_ATT_RST */
    }
}

static mb_t *make(setup_t s) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    m->auto_clear = false;
    const uint8_t tur[6] = {0};
    switch (s) {
    case SU_DETACHED:
        break;
    case SU_IDLE:
        mb_start(m);
        break;
    case SU_EXEC:
        mb_start(m);
        mb_send_inquiry(m, 1, 36);
        break;
    case SU_EXEC_TUR:
        mb_start(m);
        mb_send_tur(m, 1);
        break;
    case SU_EXEC_WRITE:
        mb_start(m);
        mb_send_write10(m, 1, 512);
        break;
    case SU_EXEC_BAD:
        m->script_on = true;
        memset(&m->script, 0, sizeof(m->script));
        m->script.good = true;
        m->script.dev_dir = MSC_BOT_DIR_IN;
        m->script.data_len = 4096; /* バッファ 2048 より大きい */
        mb_start(m);
        mb_send_inquiry(m, 1, 4096);
        break;
    case SU_DATA_IN:
        mb_start(m);
        mb_send_read10(m, 1, 0, 4, 8192);
        mb_complete_cmd(m);
        mb_complete_cmd(m);
        break;
    case SU_DATA_IN_LAST:
        mb_start(m);
        mb_send_read10(m, 1, 0, 1, 2048);
        mb_complete_cmd(m);
        mb_complete_cmd(m);
        break;
    case SU_DATA_IN_FAILED:
        m->read_fail_nth = 1;
        mb_start(m);
        mb_send_read10(m, 1, 0, 4, 8192);
        mb_complete_cmd(m);
        mb_complete_cmd(m);
        mb_complete_cmd(m); /* 2 つ目が失敗 */
        break;
    case SU_DATA_OUT:
        mb_start(m);
        mb_send_write10(m, 1, 4096);
        mb_complete_cmd(m);
        break;
    case SU_DATA_OUT_LAST:
        mb_start(m);
        mb_send_write10(m, 1, 2048);
        mb_complete_cmd(m);
        break;
    case SU_WAIT_UNSTALL:
        mb_start(m);
        mb_send_cbw(m, 1, 36, true, tur, 6);
        mb_complete_cmd(m);
        break;
    case SU_STATUS_SEND:
        mb_start(m);
        mb_send_tur(m, 1);
        mb_complete_cmd(m);
        break;
    case SU_RECOVERY: {
        mb_start(m);
        uint8_t raw[31] = {0};
        mb_send_raw_cbw(m, raw, 31);
        break;
    }
    case SU_RECOVERY_CMD:
        mb_start(m);
        mb_send_read10(m, 1, 0, 4, 8192);
        mb_complete_cmd(m);
        mb_complete_cmd(m);
        mb_deliver_in(m, false); /* in(b0) の失敗 -> RECOVERY。2 つ目の読み出しは実行中 */
        break;
    case SU_RESET_WAIT:
        mb_start(m);
        mb_send_tur(m, 1);
        msc_bot_reset(&m->bot);
        break;
    case SU_DETACH_WAIT:
        mb_start(m);
        mb_send_tur(m, 1);
        msc_bot_detach(&m->bot);
        break;
    case SU_DETACH_WAIT_ATT:
        mb_start(m);
        mb_send_tur(m, 1);
        msc_bot_detach(&m->bot);
        msc_bot_attach(&m->bot);
        break;
    case SU_DETACH_WAIT_ATT_RST:
        mb_start(m);
        mb_send_tur(m, 1);
        msc_bot_detach(&m->bot);
        msc_bot_attach(&m->bot);
        msc_bot_reset(&m->bot);
        break;
    case SU_RESET_WAIT_READ:
        mb_start(m);
        mb_send_read10(m, 1, 0, 4, 8192);
        mb_complete_cmd(m);
        TEST_ASSERT_TRUE(m->cmd_pending && m->cmd.kind == MSC_BOT_CMD_READ_BLOCKS);
        msc_bot_reset(&m->bot);
        break;
    case SU_DETACH_WAIT_READ:
        mb_start(m);
        mb_send_read10(m, 1, 0, 4, 8192);
        mb_complete_cmd(m);
        TEST_ASSERT_TRUE(m->cmd_pending && m->cmd.kind == MSC_BOT_CMD_READ_BLOCKS);
        msc_bot_detach(&m->bot);
        break;
    }
    TEST_ASSERT_EQUAL_STRING(msc_bot_state_name(setup_state_of(s)), msc_bot_state_name(msc_bot_state(&m->bot)));
    mb_assert_clean(m);
    return m;
}

/* CMDf で、実際に完了させた依頼の種類（1 = EXECUTE の CHECK CONDITION、2 = READ_BLOCKS の失敗、4 = 依頼なし）を、状態ごとに記録する。 */
static unsigned fail_kind_seen[MSC_BOT_STATE_COUNT];
static msc_bot_state_t cur_setup_state;

static void apply(mb_t *m, event_t e) {
    msc_bot_cmd_result_t spur;
    memset(&spur, 0, sizeof(spur));
    spur.seq = 1;
    switch (e) {
    case E_ATT:
        msc_bot_attach(&m->bot);
        break;
    case E_DET:
        msc_bot_detach(&m->bot);
        break;
    case E_RST:
        msc_bot_reset(&m->bot);
        break;
    case E_HIN:
        mb_host_clear_halt(m, MSC_BOT_EP_IN);
        break;
    case E_HOUT:
        mb_host_clear_halt(m, MSC_BOT_EP_OUT);
        break;
    case E_OUTOK:
        if (m->out_pending && m->out_ptr == m->cbw) {
            mb_send_tur(m, 9);
        } else if (m->out_pending) {
            mb_deliver_out(m, m->out_len, true);
        } else {
            msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, 31, true);
        }
        break;
    case E_OUTOK_BAD_CBW: {
        uint8_t raw[31] = {0};
        mb_send_raw_cbw(m, raw, 31);
        break;
    }
    case E_OUTER:
        if (m->out_pending) {
            m->out_pending = false;
        }
        msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, 0, false);
        break;
    case E_INOK:
        if (m->in_pending) {
            mb_deliver_in(m, true);
        } else {
            msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, 13, true);
        }
        break;
    case E_INER:
        if (m->in_pending) {
            mb_deliver_in(m, false);
        } else {
            msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, 0, false);
        }
        break;
    case E_CMDOK:
        if (m->cmd_pending) {
            mb_complete_cmd(m);
        } else {
            spur.good = true;
            msc_bot_command_complete(&m->bot, &spur);
        }
        break;
    case E_CMDFAIL:
        if (m->cmd_pending) {
            fail_kind_seen[cur_setup_state] |= (m->cmd.kind == MSC_BOT_CMD_EXECUTE) ? 1u : 2u;
            if (m->cmd.kind == MSC_BOT_CMD_EXECUTE) {
                /* EXECUTE の失敗 = CHECK CONDITION（データなし） */
                m->script_on = true;
                memset(&m->script, 0, sizeof(m->script));
                m->script.good = false;
            } else {
                m->read_fail_nth = (int)m->reads_in_cmd; /* 次の READ_BLOCKS を失敗させる */
            }
            mb_complete_cmd(m);
        } else {
            fail_kind_seen[cur_setup_state] |= 4u;
            spur.good = false;
            msc_bot_command_complete(&m->bot, &spur);
        }
        break;
    default:
        break;
    }
}

typedef struct {
    setup_t setup;
    event_t ev;
    msc_bot_state_t next;
    const char *log;     /* イベントの後の、ops の呼び出しの記録 */
    int done_delta;
    int stale_delta;
} row_t;

#define S(x) MSC_BOT_##x

static const row_t ROWS[] = {
    /* ---- DETACHED ---- */
    {SU_DETACHED, E_ATT, S(IDLE), "out31", 0, 0},
    {SU_DETACHED, E_DET, S(DETACHED), "", 0, 0},
    {SU_DETACHED, E_RST, S(DETACHED), "", 0, 0},
    {SU_DETACHED, E_HIN, S(DETACHED), "", 0, 0},
    {SU_DETACHED, E_HOUT, S(DETACHED), "", 0, 0},
    {SU_DETACHED, E_OUTOK, S(DETACHED), "", 0, 1},
    {SU_DETACHED, E_OUTER, S(DETACHED), "", 0, 1},
    {SU_DETACHED, E_INOK, S(DETACHED), "", 0, 1},
    {SU_DETACHED, E_INER, S(DETACHED), "", 0, 1},
    {SU_DETACHED, E_CMDOK, S(DETACHED), "", 0, 1},
    {SU_DETACHED, E_CMDFAIL, S(DETACHED), "", 0, 1},
    /* ---- IDLE ---- */
    {SU_IDLE, E_ATT, S(IDLE), "", 0, 0},
    {SU_IDLE, E_DET, S(DETACHED), "cancel", 0, 0},
    {SU_IDLE, E_RST, S(IDLE), "rdone", 0, 0},
    {SU_IDLE, E_HIN, S(IDLE), "", 0, 0},
    {SU_IDLE, E_HOUT, S(IDLE), "", 0, 0},
    {SU_IDLE, E_OUTOK, S(EXEC), "exec", 0, 0},
    {SU_IDLE, E_OUTOK_BAD_CBW, S(RECOVERY), "stallI stallO", 0, 0},
    {SU_IDLE, E_OUTER, S(RECOVERY), "stallI stallO", 0, 0},
    {SU_IDLE, E_INOK, S(IDLE), "", 0, 1},
    {SU_IDLE, E_INER, S(IDLE), "", 0, 1},
    {SU_IDLE, E_CMDOK, S(IDLE), "", 0, 1},
    {SU_IDLE, E_CMDFAIL, S(IDLE), "", 0, 1},
    /* ---- EXEC（INQUIRY H=36）---- */
    {SU_EXEC, E_ATT, S(EXEC), "", 0, 0},
    {SU_EXEC, E_DET, S(DETACH_WAIT), "", 0, 0},
    {SU_EXEC, E_RST, S(RESET_WAIT), "", 0, 0},
    {SU_EXEC, E_HIN, S(EXEC), "", 0, 0},
    {SU_EXEC, E_HOUT, S(EXEC), "", 0, 0},
    {SU_EXEC, E_OUTOK, S(EXEC), "", 0, 1},
    {SU_EXEC, E_OUTER, S(EXEC), "", 0, 1},
    {SU_EXEC, E_INOK, S(EXEC), "", 0, 1},
    {SU_EXEC, E_INER, S(EXEC), "", 0, 1},
    {SU_EXEC, E_CMDOK, S(DATA_IN), "done in36", 1, 0},                  /* ケース 6: データ IN */
    {SU_EXEC, E_CMDFAIL, S(STATUS_WAIT_UNSTALL), "done stallI", 1, 0},  /* CHECK CONDITION、データなし、H=36: ケース 4 */
    /* EXEC の結果の種類ごと */
    {SU_EXEC_TUR, E_CMDOK, S(STATUS_SEND), "done in13", 1, 0},          /* ケース 1 */
    {SU_EXEC_WRITE, E_CMDOK, S(DATA_OUT), "done out512", 1, 0},         /* ケース 9: 読み捨て */
    {SU_EXEC_BAD, E_CMDOK, S(RECOVERY), "stallI stallO done", 1, 0},    /* 契約違反 */
    /* ---- DATA_IN（READ 4 ブロック。in(b0) 転送中、2 つ目の READ_BLOCKS が実行中）---- */
    {SU_DATA_IN, E_ATT, S(DATA_IN), "", 0, 0},
    {SU_DATA_IN, E_DET, S(DETACH_WAIT), "cancel", 0, 0},
    {SU_DATA_IN, E_RST, S(RESET_WAIT), "cancel", 0, 0},
    {SU_DATA_IN, E_HIN, S(DATA_IN), "", 0, 0},
    {SU_DATA_IN, E_HOUT, S(DATA_IN), "", 0, 0},
    {SU_DATA_IN, E_OUTOK, S(DATA_IN), "", 0, 1},
    {SU_DATA_IN, E_OUTER, S(DATA_IN), "", 0, 1},
    {SU_DATA_IN, E_INOK, S(DATA_IN), "", 0, 0},                         /* b0 が空く。次は 2 つ目の READ_BLOCKS 待ち */
    {SU_DATA_IN, E_INER, S(RECOVERY), "stallI stallO", 0, 0},           /* 実行中の依頼があるので、command_done は待つ */
    {SU_DATA_IN, E_CMDOK, S(DATA_IN), "", 0, 0},                        /* b1 が埋まる。USB は b0 を送信中 */
    {SU_DATA_IN, E_CMDFAIL, S(DATA_IN), "done", 1, 0},                  /* 失敗。in(b0) の完了を待つ */
    /* データの終わり、失敗の後の終わり */
    {SU_DATA_IN_LAST, E_INOK, S(STATUS_SEND), "in13", 0, 0},
    {SU_DATA_IN_LAST, E_INER, S(RECOVERY), "stallI stallO", 0, 0},
    {SU_DATA_IN_LAST, E_DET, S(DETACHED), "cancel", 0, 0},
    {SU_DATA_IN_LAST, E_RST, S(IDLE), "cancel out31 rdone", 0, 0},
    {SU_DATA_IN_FAILED, E_INOK, S(STATUS_WAIT_UNSTALL), "stallI", 0, 0},
    {SU_DATA_IN_FAILED, E_INER, S(RECOVERY), "stallI stallO", 0, 0},
    /* ---- DATA_OUT（WRITE H=4096。2048 バイトの受信中。command_done 済み）---- */
    {SU_DATA_OUT, E_ATT, S(DATA_OUT), "", 0, 0},
    {SU_DATA_OUT, E_DET, S(DETACHED), "cancel", 0, 0},
    {SU_DATA_OUT, E_RST, S(IDLE), "cancel out31 rdone", 0, 0},
    {SU_DATA_OUT, E_HIN, S(DATA_OUT), "", 0, 0},
    {SU_DATA_OUT, E_HOUT, S(DATA_OUT), "", 0, 0},
    {SU_DATA_OUT, E_OUTOK, S(DATA_OUT), "out2048", 0, 0},               /* 次の 2048 バイト */
    {SU_DATA_OUT, E_OUTER, S(RECOVERY), "stallI stallO", 0, 0},
    {SU_DATA_OUT, E_INOK, S(DATA_OUT), "", 0, 1},
    {SU_DATA_OUT, E_INER, S(DATA_OUT), "", 0, 1},
    {SU_DATA_OUT, E_CMDOK, S(DATA_OUT), "", 0, 1},
    {SU_DATA_OUT, E_CMDFAIL, S(DATA_OUT), "", 0, 1},
    {SU_DATA_OUT_LAST, E_OUTOK, S(STATUS_SEND), "in13", 0, 0},
    /* ---- STATUS_WAIT_UNSTALL（TUR H=36 IN。Bulk-IN は STALL 済み）---- */
    {SU_WAIT_UNSTALL, E_ATT, S(STATUS_WAIT_UNSTALL), "", 0, 0},
    {SU_WAIT_UNSTALL, E_DET, S(DETACHED), "unstallI", 0, 0},
    {SU_WAIT_UNSTALL, E_RST, S(IDLE), "rdone", 0, 0},                   /* CSW は送らない。IN の STALL は残るので、CBW は受けない */
    {SU_WAIT_UNSTALL, E_HIN, S(STATUS_SEND), "in13", 0, 0},
    {SU_WAIT_UNSTALL, E_HOUT, S(STATUS_WAIT_UNSTALL), "", 0, 0},
    {SU_WAIT_UNSTALL, E_OUTOK, S(STATUS_WAIT_UNSTALL), "", 0, 1},
    {SU_WAIT_UNSTALL, E_OUTER, S(STATUS_WAIT_UNSTALL), "", 0, 1},
    {SU_WAIT_UNSTALL, E_INOK, S(STATUS_WAIT_UNSTALL), "", 0, 1},
    {SU_WAIT_UNSTALL, E_INER, S(STATUS_WAIT_UNSTALL), "", 0, 1},
    {SU_WAIT_UNSTALL, E_CMDOK, S(STATUS_WAIT_UNSTALL), "", 0, 1},
    {SU_WAIT_UNSTALL, E_CMDFAIL, S(STATUS_WAIT_UNSTALL), "", 0, 1},
    /* ---- STATUS_SEND（CSW の送信中）---- */
    {SU_STATUS_SEND, E_ATT, S(STATUS_SEND), "", 0, 0},
    {SU_STATUS_SEND, E_DET, S(DETACHED), "cancel", 0, 0},
    {SU_STATUS_SEND, E_RST, S(IDLE), "cancel out31 rdone", 0, 0},
    {SU_STATUS_SEND, E_HIN, S(STATUS_SEND), "", 0, 0},
    {SU_STATUS_SEND, E_HOUT, S(STATUS_SEND), "", 0, 0},
    {SU_STATUS_SEND, E_OUTOK, S(STATUS_SEND), "", 0, 1},
    {SU_STATUS_SEND, E_OUTER, S(STATUS_SEND), "", 0, 1},
    {SU_STATUS_SEND, E_INOK, S(IDLE), "out31", 0, 0},
    {SU_STATUS_SEND, E_INER, S(RECOVERY), "stallI stallO", 0, 0},
    {SU_STATUS_SEND, E_CMDOK, S(STATUS_SEND), "", 0, 1},
    {SU_STATUS_SEND, E_CMDFAIL, S(STATUS_SEND), "", 0, 1},
    /* ---- RECOVERY（不正な CBW。両 Bulk を STALL 済み）---- */
    {SU_RECOVERY, E_ATT, S(RECOVERY), "", 0, 0},
    {SU_RECOVERY, E_DET, S(DETACHED), "unstallI unstallO", 0, 0},
    {SU_RECOVERY, E_RST, S(IDLE), "rdone", 0, 0},                       /* STALL は残る */
    {SU_RECOVERY, E_HIN, S(RECOVERY), "stallI", 0, 0},                  /* 回復の前の解除は無効 */
    {SU_RECOVERY, E_HOUT, S(RECOVERY), "stallO", 0, 0},
    {SU_RECOVERY, E_OUTOK, S(RECOVERY), "", 0, 1},
    {SU_RECOVERY, E_OUTER, S(RECOVERY), "", 0, 1},
    {SU_RECOVERY, E_INOK, S(RECOVERY), "", 0, 1},
    {SU_RECOVERY, E_INER, S(RECOVERY), "", 0, 1},
    {SU_RECOVERY, E_CMDOK, S(RECOVERY), "", 0, 1},
    {SU_RECOVERY, E_CMDFAIL, S(RECOVERY), "", 0, 1},
    /* RECOVERY で、実行中の依頼がある */
    {SU_RECOVERY_CMD, E_CMDOK, S(RECOVERY), "done", 1, 0},
    {SU_RECOVERY_CMD, E_CMDFAIL, S(RECOVERY), "done", 1, 0},
    {SU_RECOVERY_CMD, E_DET, S(DETACH_WAIT), "unstallI unstallO", 0, 0},
    {SU_RECOVERY_CMD, E_RST, S(RESET_WAIT), "", 0, 0},
    /* ---- RESET_WAIT（実行中の依頼の完了を待つ）---- */
    {SU_RESET_WAIT, E_ATT, S(RESET_WAIT), "", 0, 0},
    {SU_RESET_WAIT, E_DET, S(DETACH_WAIT), "", 0, 0},
    {SU_RESET_WAIT, E_RST, S(RESET_WAIT), "", 0, 0},
    {SU_RESET_WAIT, E_HIN, S(RESET_WAIT), "", 0, 0},
    {SU_RESET_WAIT, E_HOUT, S(RESET_WAIT), "", 0, 0},
    {SU_RESET_WAIT, E_OUTOK, S(RESET_WAIT), "", 0, 1},
    {SU_RESET_WAIT, E_OUTER, S(RESET_WAIT), "", 0, 1},
    {SU_RESET_WAIT, E_INOK, S(RESET_WAIT), "", 0, 1},
    {SU_RESET_WAIT, E_INER, S(RESET_WAIT), "", 0, 1},
    {SU_RESET_WAIT, E_CMDOK, S(IDLE), "done out31 rdone", 1, 0},
    {SU_RESET_WAIT_READ, E_CMDFAIL, S(IDLE), "done out31 rdone", 1, 0}, /* 保留中の READ_BLOCKS の失敗 */
    {SU_RESET_WAIT_READ, E_CMDOK, S(IDLE), "done out31 rdone", 1, 0},
    /* ---- DETACH_WAIT ---- */
    {SU_DETACH_WAIT, E_ATT, S(DETACH_WAIT), "", 0, 0},
    {SU_DETACH_WAIT, E_DET, S(DETACH_WAIT), "", 0, 0},
    {SU_DETACH_WAIT, E_RST, S(DETACH_WAIT), "", 0, 0},
    {SU_DETACH_WAIT, E_HIN, S(DETACH_WAIT), "", 0, 0},
    {SU_DETACH_WAIT, E_HOUT, S(DETACH_WAIT), "", 0, 0},
    {SU_DETACH_WAIT, E_OUTOK, S(DETACH_WAIT), "", 0, 1},
    {SU_DETACH_WAIT, E_OUTER, S(DETACH_WAIT), "", 0, 1},
    {SU_DETACH_WAIT, E_INOK, S(DETACH_WAIT), "", 0, 1},
    {SU_DETACH_WAIT, E_INER, S(DETACH_WAIT), "", 0, 1},
    {SU_DETACH_WAIT, E_CMDOK, S(DETACHED), "done", 1, 0},
    {SU_DETACH_WAIT_READ, E_CMDFAIL, S(DETACHED), "done", 1, 0},       /* 保留中の READ_BLOCKS の失敗 */
    {SU_DETACH_WAIT_READ, E_CMDOK, S(DETACHED), "done", 1, 0},
    /* 再接続（ATT）の後の Reset は、完了の後で通知する。再度の切断で破棄する */
    {SU_DETACH_WAIT_ATT, E_CMDOK, S(IDLE), "done out31", 1, 0},
    {SU_DETACH_WAIT_ATT, E_RST, S(DETACH_WAIT), "", 0, 0},
    {SU_DETACH_WAIT_ATT, E_DET, S(DETACH_WAIT), "", 0, 0},
    {SU_DETACH_WAIT_ATT_RST, E_CMDOK, S(IDLE), "done out31 rdone", 1, 0},
    {SU_DETACH_WAIT_ATT_RST, E_DET, S(DETACH_WAIT), "", 0, 0},
};

static void run_row(const row_t *r) {
    mb_t *m = make(r->setup);
    cur_setup_state = setup_state_of(r->setup);
    mb_clear_log(m);
    const unsigned done0 = m->n_done;
    const uint32_t stale0 = msc_bot_stats(&m->bot).stale_events;
    apply(m, r->ev);
    char msg[160];
    snprintf(msg, sizeof(msg), "row: %s + %s (setup %d)", msc_bot_state_name(setup_state_of(r->setup)), EVNAME[r->ev], (int)r->setup);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(msc_bot_state_name(r->next), msc_bot_state_name(msc_bot_state(&m->bot)), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r->log, mb_log_str(m), msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r->done_delta, (int)(m->n_done - done0), msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r->stale_delta, (int)(msc_bot_stats(&m->bot).stale_events - stale0), msg);
    /* bot が STALL したと思っている Bulk と、モックのハードウェアの STALL が、常に一致する */
    TEST_ASSERT_EQUAL_INT_MESSAGE(msc_bot_halted(&m->bot, MSC_BOT_EP_IN), m->hw_in_stalled, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(msc_bot_halted(&m->bot, MSC_BOT_EP_OUT), m->hw_out_stalled, msg);
    mb_assert_clean(m);
}

static void test_every_row(void) {
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) run_row(&ROWS[i]);
}

/* 10 状態 x 11 イベントの全てのセルが、表にある。 */
static void test_table_covers_every_state_event_cell(void) {
    bool seen[MSC_BOT_STATE_COUNT][E_COUNT];
    memset(seen, 0, sizeof(seen));
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        event_t e = ROWS[i].ev == E_OUTOK_BAD_CBW ? E_OUTOK : ROWS[i].ev;
        seen[setup_state_of(ROWS[i].setup)][e] = true;
    }
    for (int s = 0; s < MSC_BOT_STATE_COUNT; s++) {
        for (int e = 0; e < E_COUNT; e++) {
            if (e == E_OUTOK_BAD_CBW) continue;
            char msg[100];
            snprintf(msg, sizeof(msg), "uncovered cell: %s x %s", msc_bot_state_name((msc_bot_state_t)s), EVNAME[e]);
            TEST_ASSERT_TRUE_MESSAGE(seen[s][e], msg);
        }
    }
}

/* CMDf の行が、依頼の種類まで検証している: 依頼が保留される状態（DATA_IN、RECOVERY、RESET_WAIT、DETACH_WAIT）では、
 * READ_BLOCKS の失敗を、EXEC では、EXECUTE の CHECK CONDITION を、それ以外の状態では、「依頼なし」を、実際に完了させている。 */
static void test_cmdf_rows_exercise_the_real_completion_kind(void) {
    memset(fail_kind_seen, 0, sizeof(fail_kind_seen));
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) run_row(&ROWS[i]);
    TEST_ASSERT_TRUE_MESSAGE(fail_kind_seen[MSC_BOT_EXEC] & 1u, "EXEC: no CHECK CONDITION row");
    TEST_ASSERT_TRUE_MESSAGE(fail_kind_seen[MSC_BOT_DATA_IN] & 2u, "DATA_IN: no READ_BLOCKS failure row");
    TEST_ASSERT_TRUE_MESSAGE(fail_kind_seen[MSC_BOT_RECOVERY] & 2u, "RECOVERY: no READ_BLOCKS failure row");
    TEST_ASSERT_TRUE_MESSAGE(fail_kind_seen[MSC_BOT_RESET_WAIT] & 2u, "RESET_WAIT: no READ_BLOCKS failure row");
    TEST_ASSERT_TRUE_MESSAGE(fail_kind_seen[MSC_BOT_DETACH_WAIT] & 2u, "DETACH_WAIT: no READ_BLOCKS failure row");
    TEST_ASSERT_FALSE_MESSAGE(fail_kind_seen[MSC_BOT_RESET_WAIT] & 1u, "RESET_WAIT: CMDf row uses an EXECUTE completion");
    TEST_ASSERT_FALSE_MESSAGE(fail_kind_seen[MSC_BOT_DETACH_WAIT] & 1u, "DETACH_WAIT: CMDf row uses an EXECUTE completion");
    const msc_bot_state_t no_cmd[] = {MSC_BOT_DETACHED, MSC_BOT_IDLE, MSC_BOT_DATA_OUT, MSC_BOT_STATUS_WAIT_UNSTALL, MSC_BOT_STATUS_SEND};
    for (size_t i = 0; i < sizeof(no_cmd) / sizeof(no_cmd[0]); i++) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(4u, fail_kind_seen[no_cmd[i]], msc_bot_state_name(no_cmd[i]));
    }
}

/* 次の状態として、全ての状態が表に現れる（到達できる）。 */
static void test_every_state_is_a_next_state_of_some_row(void) {
    bool seen[MSC_BOT_STATE_COUNT];
    memset(seen, 0, sizeof(seen));
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) seen[ROWS[i].next] = true;
    for (int s = 0; s < MSC_BOT_STATE_COUNT; s++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[s], msc_bot_state_name((msc_bot_state_t)s));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_row);
    RUN_TEST(test_table_covers_every_state_event_cell);
    RUN_TEST(test_cmdf_rows_exercise_the_real_completion_kind);
    RUN_TEST(test_every_state_is_a_next_state_of_some_row);
    return UNITY_END();
}
