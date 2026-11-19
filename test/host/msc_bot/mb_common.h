/*
 * msc_bot のホストテストの共通部品: USB のハードウェアと iso_msc_task のモック、ホスト（PC）の役、検査の道具。
 *
 * モックは、ボード側の義務（msc_bot が守るべき呼び出しの順序）を違反フラグとして記録する:
 *   - 同じ向きの USB の転送を、2 つ同時に発行しない。STALL 中の Bulk に転送を発行しない。STALL を、その Bulk の
 *     転送が残っているときに発行しない（msc_bot は、必ず cancel_xfers の後、または転送の完了の後に STALL する）。
 *   - iso_msc_task への依頼は、同時に 1 つまで。command_done は、EXECUTE を依頼したコマンドごとに 1 回で、
 *     iso_msc_task が実行中のときに呼ばない（呼ぶと、スナップショットの参照を放した後に、読み出しが続く）。
 *   - 引数の範囲（buf のサイズ、blocks x 2048 <= cap、CDB の長さ 1..16）。
 * 違反は mb_assert_clean() で検査する（コールバックの中では、Unity の失敗を使わない）。
 *
 * ホストの役: CBW を送る（msc_bot が Bulk-OUT に受信を発行しているときだけ）、IN の転送を受け取る（STALL 中は受け取れない）、
 * OUT のデータを送る、CLEAR_FEATURE(HALT) を発行する。mb_settle() は、BOT 5.3 / 6.7 に従う「お行儀のよいホスト」。
 */
#ifndef MB_COMMON_H
#define MB_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msc_bot.h"
#include "unity.h"

#define MB_BUF_MAX 8192u
#define MB_LOG_MAX 8192
#define MB_RX_MAX (4u * 1024u * 1024u)

typedef enum {
    A_XFER_IN = 1,
    A_XFER_OUT,
    A_STALL_IN,
    A_STALL_OUT,
    A_UNSTALL_IN,
    A_UNSTALL_OUT,
    A_CANCEL,
    A_SUBMIT_EXEC,
    A_SUBMIT_READ,
    A_DONE,
    A_RESET_DONE
} mb_act_t;

typedef struct {
    mb_act_t a;
    uint32_t arg;
} mb_actrec_t;

typedef struct {
    bool valid;
    uint32_t sig, tag, residue;
    uint8_t status;
} mb_csw_rx_t;

typedef struct mb {
    msc_bot_t bot;
    msc_bot_cfg_t cfg;
    uint8_t cbw[MSC_BOT_CBW_BUF_SIZE];
    uint8_t csw[MSC_BOT_CSW_BUF_SIZE];
    uint8_t buf[2][MB_BUF_MAX];
    uint32_t buf_size;

    mb_actrec_t log[MB_LOG_MAX];
    int nlog;

    /* ハードウェアのモデル */
    bool in_pending;
    const uint8_t *in_ptr;
    uint32_t in_len;
    bool out_pending;
    uint8_t *out_ptr;
    uint32_t out_len;
    bool hw_in_stalled, hw_out_stalled;

    /* iso_msc_task のモデル */
    bool cmd_pending;
    msc_bot_cmd_t cmd;
    bool sync;            /* true: submit_command の中で、すぐに完了を返す（再入） */
    bool hold_cmd;        /* mb_settle が、依頼の完了を返さない */
    bool script_on;       /* true: EXECUTE の結果を script で与える */
    msc_bot_cmd_result_t script;
    int read_fail_nth;    /* 1 つのコマンドの中で、n 番目（0 始まり）の READ_BLOCKS を失敗させる。-1 = しない */
    int reads_in_cmd;
    bool refuse_in, refuse_out, refuse_submit;
    bool sync_xfer;       /* true: USB の転送を、発行された関数の中で、すぐ完了させる（ホストが即座に応える。再入） */
    void (*hook)(struct mb *m, mb_act_t a); /* 各 ops の最後に呼ぶ（イベントの注入用。NULL 可） */
    void (*on_csw)(struct mb *m);           /* ホストが CSW を受け取ったとき（csw_rx を設定した後）に呼ぶ（検査用。NULL 可） */

    /* 計数 */
    unsigned n_exec, n_read, n_done, n_reset_done, n_cancel, n_stall_in, n_stall_out;
    unsigned live;        /* EXECUTE を依頼して、command_done をまだ受けていない数 */
    unsigned viol;
    char viol_msg[160];

    /* ホスト側 */
    uint8_t *rx;          /* IN で受け取ったデータ（CSW を除く） */
    uint32_t rx_len;
    mb_csw_rx_t csw_rx;
    unsigned n_csw;
    uint32_t host_out_left; /* mb_settle が OUT で送るバイト数 */
    bool auto_clear;      /* mb_settle が、STALL された Bulk-IN をクリアする */
    uint32_t in_xfers;    /* 受け取った IN の転送（データのみ）の数 */
    uint32_t in_xfer_sizes[256];
} mb_t;

static inline void mb_deliver_in(mb_t *m, bool ok);
static inline void mb_deliver_out(mb_t *m, uint32_t nbytes, bool ok);

#define MB_HOOK(m, a)             \
    do {                          \
        if ((m)->hook) (m)->hook((m), (a)); \
    } while (0)

static mb_t M_storage;
static uint8_t RX_storage[MB_RX_MAX];

static inline uint8_t mb_pat(uint64_t lba, uint32_t i) { return (uint8_t)(lba * 31u + i * 7u + (i >> 8) + 3u); }

#define MB_VIOL(m, msg)                                              \
    do {                                                             \
        if ((m)->viol == 0) {                                        \
            snprintf((m)->viol_msg, sizeof((m)->viol_msg), "%s", msg); \
        }                                                            \
        (m)->viol++;                                                 \
    } while (0)

static inline void mb_log(mb_t *m, mb_act_t a, uint32_t arg) {
    if (m->nlog < MB_LOG_MAX) {
        m->log[m->nlog].a = a;
        m->log[m->nlog].arg = arg;
        m->nlog++;
    } else {
        MB_VIOL(m, "log overflow");
    }
}

/* ---- iso_msc_task のモデル ----------------------------------------------- */
static inline msc_bot_cmd_result_t mb_exec_result(mb_t *m, const msc_bot_cmd_t *c) {
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    r.seq = c->seq;
    r.good = true;
    if (m->script_on) {
        r = m->script;
        r.seq = c->seq;
    } else {
        switch (c->cdb[0]) {
        case 0x00: /* TEST UNIT READY */
            break;
        case 0x12: /* INQUIRY: 36 バイトを返す */
            r.dev_dir = MSC_BOT_DIR_IN;
            r.data_len = 36;
            break;
        case 0x28: /* READ(10) */
            r.is_read = true;
            r.read_lba = ((uint64_t)c->cdb[2] << 24) | ((uint64_t)c->cdb[3] << 16) | ((uint64_t)c->cdb[4] << 8) | c->cdb[5];
            r.read_blocks = ((uint32_t)c->cdb[7] << 8) | c->cdb[8];
            r.data_len = 0; /* INTERFACES.md 7.3: READ では data_len は 0 */
            break;
        case 0xA8: /* READ(12) */
            r.is_read = true;
            r.read_lba = ((uint64_t)c->cdb[2] << 24) | ((uint64_t)c->cdb[3] << 16) | ((uint64_t)c->cdb[4] << 8) | c->cdb[5];
            r.read_blocks = ((uint32_t)c->cdb[6] << 24) | ((uint32_t)c->cdb[7] << 16) | ((uint32_t)c->cdb[8] << 8) | c->cdb[9];
            r.data_len = 0;
            break;
        case 0x2A: /* WRITE(10): DATA PROTECT（データを受け取らず、失敗） */
            r.good = false;
            break;
        case 0xF0: /* テスト用: デバイスが OUT のデータを受け取る予定。長さは cdb[1..2]、good は cdb[3] */
            r.dev_dir = MSC_BOT_DIR_OUT;
            r.data_len = ((uint32_t)c->cdb[1] << 8) | c->cdb[2];
            r.good = c->cdb[3] != 0;
            break;
        default:
            r.good = false;
            break;
        }
    }
    /* READ 以外の IN のデータを out に入れる */
    if (r.good && !r.is_read && r.dev_dir == MSC_BOT_DIR_IN && c->out != NULL) {
        for (uint32_t i = 0; i < r.data_len && i < c->out_cap; i++) {
            c->out[i] = (uint8_t)(0xA0u + i);
        }
    }
    return r;
}

static inline msc_bot_cmd_result_t mb_read_result(mb_t *m, const msc_bot_cmd_t *c) {
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    r.seq = c->seq;
    r.good = true;
    if ((uint64_t)c->blocks * MSC_BOT_BLOCK_SIZE > c->cap) {
        MB_VIOL(m, "READ_BLOCKS larger than cap");
    }
    if (m->read_fail_nth >= 0 && m->reads_in_cmd == m->read_fail_nth) {
        r.good = false;
    } else {
        for (uint32_t b = 0; b < c->blocks; b++) {
            for (uint32_t i = 0; i < MSC_BOT_BLOCK_SIZE; i++) {
                c->buf[(uint64_t)b * MSC_BOT_BLOCK_SIZE + i] = mb_pat(c->lba + b, i);
            }
        }
    }
    m->reads_in_cmd++;
    return r;
}

static inline void mb_complete_cmd(mb_t *m) {
    if (!m->cmd_pending) {
        TEST_FAIL_MESSAGE("mb_complete_cmd: no command pending");
        return;
    }
    msc_bot_cmd_result_t r;
    if (m->cmd.kind == MSC_BOT_CMD_EXECUTE) {
        r = mb_exec_result(m, &m->cmd);
    } else {
        r = mb_read_result(m, &m->cmd);
    }
    m->cmd_pending = false;
    msc_bot_command_complete(&m->bot, &r);
}

/* ---- ops ------------------------------------------------------------------ */
static int mb_xfer_in(void *ctx, const uint8_t *buf, uint32_t len) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, A_XFER_IN, len);
    if (m->refuse_in) {
        return -1;
    }
    if (m->in_pending) MB_VIOL(m, "xfer_in while an IN transfer is pending");
    if (m->hw_in_stalled) MB_VIOL(m, "xfer_in while Bulk-IN is stalled");
    if (len == 0) MB_VIOL(m, "xfer_in with len 0");
    if (buf == m->csw) {
        if (len != MSC_BOT_CSW_LEN) MB_VIOL(m, "CSW with a length other than 13");
    } else if (buf != m->buf[0] && buf != m->buf[1]) {
        MB_VIOL(m, "xfer_in from a buffer that is not a data buffer or the CSW buffer");
    } else if (len > m->buf_size) {
        MB_VIOL(m, "xfer_in larger than the buffer");
    }
    m->in_pending = true;
    m->in_ptr = buf;
    m->in_len = len;
    if (m->sync_xfer && !m->hw_in_stalled) {
        mb_deliver_in(m, true);
    }
    MB_HOOK(m, A_XFER_IN);
    return 0;
}

static int mb_xfer_out(void *ctx, uint8_t *buf, uint32_t len) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, A_XFER_OUT, len);
    if (m->refuse_out) {
        return -1;
    }
    if (m->out_pending) MB_VIOL(m, "xfer_out while an OUT transfer is pending");
    if (m->hw_out_stalled) MB_VIOL(m, "xfer_out while Bulk-OUT is stalled");
    if (len == 0) MB_VIOL(m, "xfer_out with len 0");
    if (buf == m->cbw) {
        if (len != MSC_BOT_CBW_LEN) MB_VIOL(m, "CBW receive with a length other than 31");
    } else if (buf != m->buf[0]) {
        MB_VIOL(m, "xfer_out into a buffer that is not buf[0] or the CBW buffer");
    } else if (len > m->buf_size) {
        MB_VIOL(m, "xfer_out larger than the buffer");
    }
    m->out_pending = true;
    m->out_ptr = buf;
    m->out_len = len;
    if (m->sync_xfer && buf != m->cbw && m->host_out_left > 0 && !m->hw_out_stalled) {
        uint32_t n = m->host_out_left < len ? m->host_out_left : len;
        m->host_out_left -= n;
        mb_deliver_out(m, n, true);
    }
    MB_HOOK(m, A_XFER_OUT);
    return 0;
}

static void mb_stall_in(void *ctx) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, A_STALL_IN, 0);
    m->n_stall_in++;
    if (m->in_pending) MB_VIOL(m, "stall_in while an IN transfer is pending");
    m->hw_in_stalled = true;
    MB_HOOK(m, A_STALL_IN);
}

static void mb_stall_out(void *ctx) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, A_STALL_OUT, 0);
    m->n_stall_out++;
    if (m->out_pending) MB_VIOL(m, "stall_out while an OUT transfer is pending");
    m->hw_out_stalled = true;
    MB_HOOK(m, A_STALL_OUT);
}

static void mb_unstall(void *ctx, msc_bot_ep_t ep) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, ep == MSC_BOT_EP_IN ? A_UNSTALL_IN : A_UNSTALL_OUT, 0);
    if (ep == MSC_BOT_EP_IN) {
        m->hw_in_stalled = false;
    } else {
        m->hw_out_stalled = false;
    }
    MB_HOOK(m, ep == MSC_BOT_EP_IN ? A_UNSTALL_IN : A_UNSTALL_OUT);
}

static void mb_cancel(void *ctx) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, A_CANCEL, 0);
    m->n_cancel++;
    if (!m->in_pending && !m->out_pending) MB_VIOL(m, "cancel_xfers with nothing pending");
    m->in_pending = false;
    m->out_pending = false;
    MB_HOOK(m, A_CANCEL);
}

static int mb_submit(void *ctx, const msc_bot_cmd_t *cmd) {
    mb_t *m = (mb_t *)ctx;
    if (cmd->kind == MSC_BOT_CMD_EXECUTE) {
        mb_log(m, A_SUBMIT_EXEC, cmd->expected_len);
    } else {
        mb_log(m, A_SUBMIT_READ, cmd->blocks);
    }
    if (m->refuse_submit) {
        return -1;
    }
    if (m->cmd_pending) MB_VIOL(m, "second command submitted while one is outstanding");
    if (cmd->kind == MSC_BOT_CMD_EXECUTE) {
        if (cmd->cdb_len < 1 || cmd->cdb_len > MSC_BOT_MAX_CDB) MB_VIOL(m, "cdb_len out of range");
        if (cmd->out != m->buf[0] || cmd->out_cap != m->buf_size) MB_VIOL(m, "EXECUTE out/out_cap wrong");
        if ((cmd->expected_len == 0) != (cmd->dir == MSC_BOT_DIR_NONE)) MB_VIOL(m, "EXECUTE dir/expected_len mismatch");
        m->n_exec++;
        m->live++;
        m->reads_in_cmd = 0;
    } else {
        if (cmd->blocks == 0) MB_VIOL(m, "READ_BLOCKS with 0 blocks");
        if (cmd->buf != m->buf[0] && cmd->buf != m->buf[1]) MB_VIOL(m, "READ_BLOCKS buf is not a data buffer");
        if (m->live == 0) MB_VIOL(m, "READ_BLOCKS without a live command (snapshot reference already released)");
        m->n_read++;
    }
    m->cmd = *cmd;
    m->cmd_pending = true;
    if (m->sync) {
        mb_complete_cmd(m); /* 再入。msc_bot は、イベントを積んで、この呼び出しの後に処理する */
    }
    MB_HOOK(m, cmd->kind == MSC_BOT_CMD_EXECUTE ? A_SUBMIT_EXEC : A_SUBMIT_READ);
    return 0;
}

static void mb_done(void *ctx) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, A_DONE, 0);
    m->n_done++;
    if (m->live == 0) MB_VIOL(m, "command_done without a live command (double done)");
    else m->live--;
    if (m->cmd_pending) MB_VIOL(m, "command_done while iso_msc_task still has a command outstanding");
    MB_HOOK(m, A_DONE);
}

static void mb_reset_done(void *ctx) {
    mb_t *m = (mb_t *)ctx;
    mb_log(m, A_RESET_DONE, 0);
    m->n_reset_done++;
    MB_HOOK(m, A_RESET_DONE);
}

/* ---- 準備 ----------------------------------------------------------------- */
static inline mb_t *mb_init(uint32_t buf_size, bool out_stall) {
    mb_t *m = &M_storage;
    memset(m, 0, sizeof(*m));
    m->rx = RX_storage;
    m->buf_size = buf_size;
    m->read_fail_nth = -1;
    m->auto_clear = true;
    memset(&m->cfg, 0, sizeof(m->cfg));
    m->cfg.ops.ctx = m;
    m->cfg.ops.xfer_in = mb_xfer_in;
    m->cfg.ops.xfer_out = mb_xfer_out;
    m->cfg.ops.stall_in = mb_stall_in;
    m->cfg.ops.stall_out = mb_stall_out;
    m->cfg.ops.unstall = mb_unstall;
    m->cfg.ops.cancel_xfers = mb_cancel;
    m->cfg.ops.submit_command = mb_submit;
    m->cfg.ops.command_done = mb_done;
    m->cfg.ops.reset_done = mb_reset_done;
    m->cfg.cbw_buf = m->cbw;
    m->cfg.csw_buf = m->csw;
    m->cfg.buf[0] = m->buf[0];
    m->cfg.buf[1] = m->buf[1];
    m->cfg.buf_size = buf_size;
    m->cfg.out_stall = out_stall;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_OK, msc_bot_init(&m->bot, &m->cfg));
    return m;
}

/* attach して、CBW の受信が発行されていることを確かめる。 */
static inline void mb_start(mb_t *m) {
    msc_bot_attach(&m->bot);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_IDLE, msc_bot_state(&m->bot));
    TEST_ASSERT_TRUE(m->out_pending);
    TEST_ASSERT_TRUE(m->out_ptr == m->cbw);
    TEST_ASSERT_EQUAL_UINT32(MSC_BOT_CBW_LEN, m->out_len);
}

static inline void mb_clear_log(mb_t *m) { m->nlog = 0; }

/* ---- ホストの役 ----------------------------------------------------------- */
static inline void mb_make_cbw(uint8_t raw[31], uint32_t tag, uint32_t len, bool in, const uint8_t *cdb, uint8_t cdb_len) {
    memset(raw, 0, 31);
    raw[0] = 'U';
    raw[1] = 'S';
    raw[2] = 'B';
    raw[3] = 'C';
    raw[4] = (uint8_t)tag;
    raw[5] = (uint8_t)(tag >> 8);
    raw[6] = (uint8_t)(tag >> 16);
    raw[7] = (uint8_t)(tag >> 24);
    raw[8] = (uint8_t)len;
    raw[9] = (uint8_t)(len >> 8);
    raw[10] = (uint8_t)(len >> 16);
    raw[11] = (uint8_t)(len >> 24);
    raw[12] = in ? 0x80 : 0x00;
    raw[13] = 0;
    raw[14] = cdb_len;
    if (cdb != NULL) {
        memcpy(raw + 15, cdb, cdb_len);
    }
}

/* CBW の受信が発行されているときに、生のバイト列を送る。actual_len は USB の転送の長さ。 */
static inline void mb_send_raw_cbw(mb_t *m, const uint8_t *raw, uint32_t actual_len) {
    TEST_ASSERT_TRUE_MESSAGE(m->out_pending && m->out_ptr == m->cbw, "host sent a CBW but the device has no CBW receive armed");
    memcpy(m->cbw, raw, actual_len < 31 ? actual_len : 31);
    m->out_pending = false;
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, actual_len, true);
}

static inline void mb_send_cbw(mb_t *m, uint32_t tag, uint32_t len, bool in, const uint8_t *cdb, uint8_t cdb_len) {
    uint8_t raw[31];
    mb_make_cbw(raw, tag, len, in, cdb, cdb_len);
    mb_send_raw_cbw(m, raw, 31);
}

/* よく使うコマンドの CBW */
static inline void mb_send_inquiry(mb_t *m, uint32_t tag, uint32_t host_len) {
    const uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
    mb_send_cbw(m, tag, host_len, true, cdb, 6);
}
static inline void mb_send_tur(mb_t *m, uint32_t tag) {
    const uint8_t cdb[6] = {0};
    mb_send_cbw(m, tag, 0, false, cdb, 6);
}
static inline void mb_send_read10(mb_t *m, uint32_t tag, uint32_t lba, uint16_t blocks, uint32_t host_len) {
    const uint8_t cdb[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                             0,    (uint8_t)(blocks >> 8), (uint8_t)blocks, 0};
    mb_send_cbw(m, tag, host_len, true, cdb, 10);
}
static inline void mb_send_read12(mb_t *m, uint32_t tag, uint32_t lba, uint32_t blocks, uint32_t host_len) {
    const uint8_t cdb[12] = {0xA8, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                             (uint8_t)(blocks >> 24), (uint8_t)(blocks >> 16), (uint8_t)(blocks >> 8), (uint8_t)blocks, 0, 0};
    mb_send_cbw(m, tag, host_len, true, cdb, 12);
}
static inline void mb_send_write10(mb_t *m, uint32_t tag, uint32_t host_len) {
    const uint8_t cdb[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    mb_send_cbw(m, tag, host_len, false, cdb, 10);
}

static inline void mb_deliver_in(mb_t *m, bool ok) {
    TEST_ASSERT_TRUE_MESSAGE(m->in_pending, "mb_deliver_in: no IN transfer pending");
    TEST_ASSERT_FALSE_MESSAGE(m->hw_in_stalled, "mb_deliver_in: Bulk-IN is stalled (the host would get a STALL)");
    const uint8_t *p = m->in_ptr;
    uint32_t len = m->in_len;
    m->in_pending = false;
    if (ok) {
        if (p == m->csw) {
            m->csw_rx.valid = (len == MSC_BOT_CSW_LEN);
            m->csw_rx.sig = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            m->csw_rx.tag = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            m->csw_rx.residue = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
            m->csw_rx.status = p[12];
            m->n_csw++;
            if (m->on_csw) m->on_csw(m);
        } else {
            if (m->rx_len + len > MB_RX_MAX) {
                TEST_FAIL_MESSAGE("rx capture overflow");
            }
            memcpy(m->rx + m->rx_len, p, len);
            m->rx_len += len;
            if (m->in_xfers < 256) m->in_xfer_sizes[m->in_xfers] = len;
            m->in_xfers++;
        }
    }
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, len, ok);
}

/* ホストが nbytes を Bulk-OUT に送る（デバイスが発行した受信に対して。CBW の受信には使わない）。 */
static inline void mb_deliver_out(mb_t *m, uint32_t nbytes, bool ok) {
    TEST_ASSERT_TRUE_MESSAGE(m->out_pending && m->out_ptr != m->cbw, "mb_deliver_out: no OUT data transfer pending");
    TEST_ASSERT_FALSE_MESSAGE(m->hw_out_stalled, "mb_deliver_out: Bulk-OUT is stalled");
    m->out_pending = false;
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, nbytes, ok);
}

static inline void mb_host_clear_halt(mb_t *m, msc_bot_ep_t ep) {
    if (ep == MSC_BOT_EP_IN) {
        m->hw_in_stalled = false;
    } else {
        m->hw_out_stalled = false;
    }
    msc_bot_halt_cleared(&m->bot, ep);
}

/* お行儀のよいホストと iso_msc_task が、止まるまで進める。 */
static inline void mb_settle(mb_t *m) {
    for (int guard = 0; guard < 2000000; guard++) {
        if (m->cmd_pending && !m->hold_cmd) {
            mb_complete_cmd(m);
        } else if (m->in_pending && !m->hw_in_stalled) {
            mb_deliver_in(m, true);
        } else if (m->out_pending && m->out_ptr != m->cbw && !m->hw_out_stalled && m->host_out_left > 0) {
            uint32_t n = m->host_out_left < m->out_len ? m->host_out_left : m->out_len;
            m->host_out_left -= n;
            mb_deliver_out(m, n, true);
        } else if (m->hw_in_stalled && m->auto_clear && msc_bot_state(&m->bot) == MSC_BOT_STATUS_WAIT_UNSTALL) {
            mb_host_clear_halt(m, MSC_BOT_EP_IN);
        } else {
            return;
        }
    }
    TEST_FAIL_MESSAGE("mb_settle: did not settle");
}

/* ---- 検査 ----------------------------------------------------------------- */
static inline void mb_assert_clean(mb_t *m) {
    if (m->viol != 0) {
        char b[220];
        snprintf(b, sizeof(b), "mock violation (%u): %s", m->viol, m->viol_msg);
        TEST_FAIL_MESSAGE(b);
    }
    msc_bot_stats_t st = msc_bot_stats(&m->bot);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, st.queue_overflow, "event FIFO overflowed");
}

#define MB_ASSERT_STATE(m, s) TEST_ASSERT_EQUAL_STRING(msc_bot_state_name(s), msc_bot_state_name(msc_bot_state(&(m)->bot)))

/* 呼び出しの記録を、1 つの文字列にする。例: "exec in36 done in13"。 */
static inline const char *mb_log_str(mb_t *m) {
    static char out[MB_LOG_MAX * 12];
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < m->nlog; i++) {
        char tok[24];
        switch (m->log[i].a) {
        case A_XFER_IN: snprintf(tok, sizeof(tok), "in%u", (unsigned)m->log[i].arg); break;
        case A_XFER_OUT: snprintf(tok, sizeof(tok), "out%u", (unsigned)m->log[i].arg); break;
        case A_STALL_IN: snprintf(tok, sizeof(tok), "stallI"); break;
        case A_STALL_OUT: snprintf(tok, sizeof(tok), "stallO"); break;
        case A_UNSTALL_IN: snprintf(tok, sizeof(tok), "unstallI"); break;
        case A_UNSTALL_OUT: snprintf(tok, sizeof(tok), "unstallO"); break;
        case A_CANCEL: snprintf(tok, sizeof(tok), "cancel"); break;
        case A_SUBMIT_EXEC: snprintf(tok, sizeof(tok), "exec"); break;
        case A_SUBMIT_READ: snprintf(tok, sizeof(tok), "read%u", (unsigned)m->log[i].arg); break;
        case A_DONE: snprintf(tok, sizeof(tok), "done"); break;
        case A_RESET_DONE: snprintf(tok, sizeof(tok), "rdone"); break;
        default: snprintf(tok, sizeof(tok), "?"); break;
        }
        n += (size_t)snprintf(out + n, sizeof(out) - n, "%s%s", i ? " " : "", tok);
    }
    return out;
}

#define MB_ASSERT_LOG(m, expected) TEST_ASSERT_EQUAL_STRING((expected), mb_log_str(m))

static inline void mb_assert_csw(mb_t *m, uint32_t tag, uint8_t status, uint32_t residue) {
    TEST_ASSERT_TRUE_MESSAGE(m->csw_rx.valid, "no valid CSW received");
    TEST_ASSERT_EQUAL_HEX32(MSC_BOT_CSW_SIGNATURE, m->csw_rx.sig);
    TEST_ASSERT_EQUAL_HEX32(tag, m->csw_rx.tag);
    TEST_ASSERT_EQUAL_UINT8(status, m->csw_rx.status);
    TEST_ASSERT_EQUAL_UINT32(residue, m->csw_rx.residue);
}

/* rx の [off, off+len) が、LBA から始まる ISO のデータの [off, off+len) と一致するか。 */
static inline void mb_assert_rx_pattern(mb_t *m, uint64_t lba, uint32_t off, uint32_t len) {
    TEST_ASSERT_TRUE(off + len <= m->rx_len);
    for (uint32_t i = off; i < off + len; i++) {
        uint8_t want = mb_pat(lba + i / MSC_BOT_BLOCK_SIZE, i % MSC_BOT_BLOCK_SIZE);
        if (m->rx[i] != want) {
            char b[80];
            snprintf(b, sizeof(b), "rx data mismatch at byte %u", (unsigned)i);
            TEST_FAIL_MESSAGE(b);
        }
    }
}

/* 1 つのコマンドを、最後まで（CSW まで）通す。結果の CSW は m->csw_rx。 */
static inline void mb_run_command_to_idle(mb_t *m) {
    mb_settle(m);
    mb_assert_clean(m);
}

#endif /* MB_COMMON_H */
