/*
 * msc_bot.c -- USB Mass Storage Bulk-Only Transport の状態機械（純 C）。仕様と状態遷移表は include/msc_bot.h。
 * 出典: USB Mass Storage Class Bulk-Only Transport 1.0（BOT 5.1 CBW、5.2 CSW、6.6 エラー、6.7 13 のケース）。
 */
#include "msc_bot.h"

#include <string.h>

enum { EV_ATTACH = 1, EV_DETACH, EV_RESET, EV_HALT_CLEARED, EV_XFER_DONE, EV_CMD_DONE };

enum { BUF_FREE = 0, BUF_FILLING, BUF_READY, BUF_SENDING };

/* ---- 小さな道具 ----------------------------------------------------------- */
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* ---- 判定（13 のケース。BOT 6.7）------------------------------------------ */
msc_bot_plan_t msc_bot_decide(bool host_dir_in_flag, uint32_t host_len, msc_bot_dir_t dev_dir, uint64_t dev_len,
                              bool out_stall) {
    msc_bot_plan_t p;
    memset(&p, 0, sizeof(p));

    /* ホスト: dCBWDataTransferLength = 0 なら、方向のビットを無視する（BOT 5.1）。 */
    p.host_len = host_len;
    if (host_len == 0) {
        p.host_dir = MSC_BOT_DIR_NONE;
    } else {
        p.host_dir = host_dir_in_flag ? MSC_BOT_DIR_IN : MSC_BOT_DIR_OUT;
    }

    /* デバイス: 長さ 0 は Dn。 */
    if (dev_len == 0 || (dev_dir != MSC_BOT_DIR_IN && dev_dir != MSC_BOT_DIR_OUT)) {
        p.dev_dir = MSC_BOT_DIR_NONE;
        p.dev_len = 0;
    } else {
        p.dev_dir = dev_dir;
        p.dev_len = dev_len;
    }

    const uint64_t H = host_len;
    const uint64_t L = p.dev_len;

    switch (p.host_dir) {
    case MSC_BOT_DIR_NONE:
        if (p.dev_dir == MSC_BOT_DIR_NONE) {
            p.case_no = 1; /* Hn = Dn */
        } else if (p.dev_dir == MSC_BOT_DIR_IN) {
            p.case_no = 2; /* Hn < Di */
            p.phase_error = true;
        } else {
            p.case_no = 3; /* Hn < Do */
            p.phase_error = true;
        }
        break;
    case MSC_BOT_DIR_IN:
        if (p.dev_dir == MSC_BOT_DIR_NONE) {
            p.case_no = 4; /* Hi > Dn */
        } else if (p.dev_dir == MSC_BOT_DIR_IN) {
            if (L < H) {
                p.case_no = 5; /* Hi > Di */
                p.xfer_dir = MSC_BOT_DIR_IN;
                p.xfer_len = (uint32_t)L;
            } else if (L == H) {
                p.case_no = 6; /* Hi = Di */
                p.xfer_dir = MSC_BOT_DIR_IN;
                p.xfer_len = host_len;
            } else {
                p.case_no = 7; /* Hi < Di */
                p.phase_error = true;
                p.xfer_dir = MSC_BOT_DIR_IN;
                p.xfer_len = host_len;
            }
        } else {
            p.case_no = 8; /* Hi <> Do */
            p.phase_error = true;
        }
        break;
    default: /* MSC_BOT_DIR_OUT */
        if (p.dev_dir == MSC_BOT_DIR_NONE) {
            p.case_no = 9; /* Ho > Dn */
            p.out_processed = 0;
        } else if (p.dev_dir == MSC_BOT_DIR_IN) {
            p.case_no = 10; /* Ho <> Di */
            p.phase_error = true;
        } else if (L < H) {
            p.case_no = 11; /* Ho > Do */
            p.out_processed = (uint32_t)L;
        } else if (L == H) {
            p.case_no = 12; /* Ho = Do */
            p.out_processed = host_len;
        } else {
            p.case_no = 13; /* Ho < Do */
            p.phase_error = true;
        }
        /* BOT 6.7.3: H バイトを受け取る、または Bulk-OUT を STALL して打ち切る。 */
        if (out_stall) {
            p.stall_out = true;
        } else {
            p.xfer_dir = MSC_BOT_DIR_OUT;
            p.xfer_len = host_len;
        }
        break;
    }
    return p;
}

msc_bot_csw_t msc_bot_resolve(const msc_bot_plan_t *plan, bool cmd_good, uint32_t sent_in, uint32_t received_out) {
    msc_bot_csw_t c;
    memset(&c, 0, sizeof(c));
    const uint32_t H = plan->host_len;

    if (plan->phase_error) {
        c.status = MSC_BOT_CSW_PHASE_ERROR;
    } else {
        c.status = cmd_good ? MSC_BOT_CSW_PASSED : MSC_BOT_CSW_FAILED;
    }

    switch (plan->host_dir) {
    case MSC_BOT_DIR_IN: {
        const uint32_t sent = min_u32(sent_in, H);
        c.residue = H - sent;
        /* BOT 6.7.2: H に満たない量しか送っていなければ、Bulk-IN を STALL する。 */
        c.stall_in = (sent < H);
        break;
    }
    case MSC_BOT_DIR_OUT: {
        const uint32_t recv = min_u32(received_out, H);
        const uint32_t processed = plan->phase_error ? recv : min_u32(plan->out_processed, recv);
        c.residue = H - processed;
        c.stall_out = plan->stall_out;
        break;
    }
    default:
        c.residue = 0;
        break;
    }
    return c;
}

/* ---- 内部の操作 ----------------------------------------------------------- */
static void on_attach(msc_bot_t *b);
static void on_detach(msc_bot_t *b);
static void on_reset(msc_bot_t *b);
static void on_halt_cleared(msc_bot_t *b, msc_bot_ep_t ep);
static void on_xfer_done(msc_bot_t *b, msc_bot_ep_t ep, uint32_t len, bool ok);
static void on_cmd_done(msc_bot_t *b, const msc_bot_cmd_result_t *res);

static void finish_cmd(msc_bot_t *b) {
    if (b->cmd_live && !b->cmd_outstanding) {
        b->cmd_live = false;
        b->stats.cmd_done_called++;
        b->cfg.ops.command_done(b->cfg.ops.ctx);
    }
}

static void cancel_xfers(msc_bot_t *b) {
    if (b->in_pending || b->out_pending) {
        b->in_pending = false;
        b->out_pending = false;
        /* 取り消し前に投入されて、まだ FIFO にある完了は、旧世代として捨てる（新しい転送の完了と取り違えない）。 */
        b->xfer_gen++;
        b->cfg.ops.cancel_xfers(b->cfg.ops.ctx);
    }
}

static void stall_ep(msc_bot_t *b, msc_bot_ep_t ep) {
    if (ep == MSC_BOT_EP_IN) {
        b->in_halted = true;
        b->cfg.ops.stall_in(b->cfg.ops.ctx);
    } else {
        b->out_halted = true;
        b->cfg.ops.stall_out(b->cfg.ops.ctx);
    }
}

/* 転送の状態を捨てる（データの段階の途中の情報）。command_done や cancel はしない。 */
static void reset_data_state(msc_bot_t *b) {
    b->din_total = b->din_issued = b->din_sent = 0;
    b->din_lba = 0;
    b->buf_st[0] = b->buf_st[1] = BUF_FREE;
    b->buf_len[0] = b->buf_len[1] = 0;
    b->din_fill = b->din_send = b->din_cur = 0;
    b->din_failed = false;
    b->dout_total = b->dout_received = 0;
    b->cmd_good = true;
}

/* BOT 6.6.1 / 6.6.2: 両 Bulk を STALL して、Reset Recovery を待つ。 */
static void enter_recovery(msc_bot_t *b) {
    cancel_xfers(b);
    reset_data_state(b);
    b->state = MSC_BOT_RECOVERY;
    if (!b->in_halted) {
        stall_ep(b, MSC_BOT_EP_IN);
    }
    if (!b->out_halted) {
        stall_ep(b, MSC_BOT_EP_OUT);
    }
    finish_cmd(b);
}

/* CBW の受信の発行の結果 */
enum { ARM_OK = 0,   /* 発行済み（今回、または前から） */
       ARM_WAIT,     /* 既存の halt の解除を待つ（まだ発行しない） */
       ARM_REFUSED   /* xfer_out が拒否した（RECOVERY に入った） */ };

/* IDLE で、halt がなければ CBW の受信を発行する。 */
static int arm_cbw(msc_bot_t *b) {
    if (b->state != MSC_BOT_IDLE) {
        return ARM_WAIT;
    }
    if (b->out_pending) {
        return ARM_OK;
    }
    if (b->in_halted || b->out_halted) {
        return ARM_WAIT;
    }
    b->out_pending = true;
    b->out_req_len = MSC_BOT_CBW_LEN;
    if (b->cfg.ops.xfer_out(b->cfg.ops.ctx, b->cfg.cbw_buf, MSC_BOT_CBW_LEN) != 0) {
        b->out_pending = false;
        enter_recovery(b);
        return ARM_REFUSED;
    }
    return ARM_OK;
}

/* Reset の結果を、グルーに伝える（成功: reset_done、CBW の受信の発行を拒否された: reset_failed）。 */
static void notify_reset(msc_bot_t *b, int arm) {
    if (arm == ARM_REFUSED) {
        if (b->cfg.ops.reset_failed != NULL) {
            b->cfg.ops.reset_failed(b->cfg.ops.ctx);
        }
    } else if (b->cfg.ops.reset_done != NULL) {
        b->cfg.ops.reset_done(b->cfg.ops.ctx);
    }
}

static void send_csw(msc_bot_t *b) {
    uint8_t *c = b->cfg.csw_buf;
    wr32le(c + 0, MSC_BOT_CSW_SIGNATURE);
    wr32le(c + 4, b->tag);
    wr32le(c + 8, b->csw.residue);
    c[12] = b->csw.status;
    b->state = MSC_BOT_STATUS_SEND;
    b->in_pending = true;
    b->in_req_len = MSC_BOT_CSW_LEN;
    if (b->cfg.ops.xfer_in(b->cfg.ops.ctx, c, MSC_BOT_CSW_LEN) != 0) {
        b->in_pending = false;
        enter_recovery(b);
    }
}

/* データの段階が終わった（または、データなし）。CSW の内容を決めて、STALL が要れば STALL して、CSW に進む。 */
static void status_phase(msc_bot_t *b, uint32_t sent_in, uint32_t received_out) {
    finish_cmd(b); /* 通常は、すでに呼んである（冪等）。 */
    b->csw = msc_bot_resolve(&b->plan, b->cmd_good && !b->din_failed, sent_in, received_out);
    reset_data_state(b);
    if (b->csw.stall_out && !b->out_halted) {
        stall_ep(b, MSC_BOT_EP_OUT);
    }
    if (b->csw.stall_in && !b->in_halted) {
        stall_ep(b, MSC_BOT_EP_IN);
    }
    if (b->in_halted) {
        /* BOT 6.7.2 のホストは、STALL を受けたら Bulk-IN をクリアして、CSW を読み直す。クリアを待ってから送る。 */
        b->state = MSC_BOT_STATUS_WAIT_UNSTALL;
        return;
    }
    send_csw(b);
}

/* ---- データ IN ------------------------------------------------------------ */
static void din_begin(msc_bot_t *b, bool is_read, uint64_t lba, uint32_t total) {
    b->state = MSC_BOT_DATA_IN;
    b->din_total = total;
    b->din_sent = 0;
    b->din_lba = lba;
    b->din_failed = false;
    b->din_fill = b->din_send = b->din_cur = 0;
    b->buf_st[0] = b->buf_st[1] = BUF_FREE;
    b->buf_len[0] = b->buf_len[1] = 0;
    if (is_read) {
        b->din_issued = 0;
    } else {
        /* READ 以外: データは EXECUTE の out（= buf[0]）にある。読み出しはない。 */
        b->din_issued = total;
        b->buf_st[0] = BUF_READY;
        b->buf_len[0] = total;
    }
}

static void din_pump(msc_bot_t *b) {
    const uint32_t bs = b->cfg.buf_size;

    /* (1) 次のバッファを READ_BLOCKS で埋める（実行中の依頼がなければ）。 */
    if (!b->din_failed && !b->cmd_outstanding && b->din_issued < b->din_total && b->buf_st[b->din_fill] == BUF_FREE) {
        const uint32_t idx = b->din_fill;
        const uint32_t chunk = min_u32(bs, b->din_total - b->din_issued);
        const uint32_t blocks = (chunk + (MSC_BOT_BLOCK_SIZE - 1u)) / MSC_BOT_BLOCK_SIZE;
        msc_bot_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = MSC_BOT_CMD_READ_BLOCKS;
        cmd.seq = ++b->cmd_seq;
        cmd.lba = b->din_lba;
        cmd.blocks = blocks;
        cmd.buf = b->cfg.buf[idx];
        cmd.cap = bs;
        b->buf_st[idx] = BUF_FILLING;
        b->buf_len[idx] = chunk;
        b->din_cur = (uint8_t)idx;
        b->din_lba += blocks;
        b->din_issued += chunk;
        b->din_fill = (uint8_t)(idx ^ 1u);
        b->cmd_outstanding = true;
        if (b->cfg.ops.submit_command(b->cfg.ops.ctx, &cmd) != 0) {
            b->cmd_outstanding = false;
            enter_recovery(b);
            return;
        }
    }

    /* (2) 以後 mmc_scsi を呼ばないと確定したら、コマンドを終える（xfer_in より先。表と設計 4.1 の「done して xfer_in」）。 */
    if (!b->cmd_outstanding && (b->din_issued == b->din_total || b->din_failed)) {
        finish_cmd(b);
    }

    /* (3) 埋まったバッファを、USB に送る（IN の転送が空いていれば）。 */
    if (!b->din_failed && !b->in_pending && b->buf_st[b->din_send] == BUF_READY) {
        const uint32_t idx = b->din_send;
        b->buf_st[idx] = BUF_SENDING;
        b->in_pending = true;
        b->in_req_len = b->buf_len[idx];
        if (b->cfg.ops.xfer_in(b->cfg.ops.ctx, b->cfg.buf[idx], b->buf_len[idx]) != 0) {
            b->in_pending = false;
            enter_recovery(b);
            return;
        }
    }

    /* (4) 終わりの判定。失敗したときは、転送中の IN が終わるのを待つ。 */
    if (b->din_failed) {
        if (!b->in_pending) {
            status_phase(b, b->din_sent, 0);
        }
    } else if (b->din_sent == b->din_total) {
        status_phase(b, b->din_sent, 0);
    }
}

static void din_on_in_done(msc_bot_t *b, uint32_t len, bool ok) {
    const uint32_t idx = b->din_send;
    if (!ok || len != b->in_req_len) {
        enter_recovery(b);
        return;
    }
    b->din_sent += len;
    b->buf_st[idx] = BUF_FREE;
    b->din_send = (uint8_t)(idx ^ 1u);
    din_pump(b);
}

static void din_on_cmd_done(msc_bot_t *b, const msc_bot_cmd_result_t *res) {
    const uint32_t idx = b->din_cur;
    b->cmd_outstanding = false;
    if (!res->good) {
        b->din_failed = true;
        b->buf_st[idx] = BUF_FREE;
    } else {
        b->buf_st[idx] = BUF_READY;
    }
    din_pump(b);
}

/* ---- データ OUT（読み捨て）------------------------------------------------ */
static void dout_next(msc_bot_t *b) {
    const uint32_t remaining = b->dout_total - b->dout_received;
    if (remaining == 0) {
        status_phase(b, 0, b->dout_received);
        return;
    }
    const uint32_t chunk = min_u32(remaining, b->cfg.buf_size);
    b->out_pending = true;
    b->out_req_len = chunk;
    if (b->cfg.ops.xfer_out(b->cfg.ops.ctx, b->cfg.buf[0], chunk) != 0) {
        b->out_pending = false;
        enter_recovery(b);
    }
}

static void dout_on_done(msc_bot_t *b, uint32_t len, bool ok) {
    if (!ok || len > b->out_req_len) {
        enter_recovery(b);
        return;
    }
    b->dout_received += len;
    if (len < b->out_req_len) {
        /* ホストが、宣言した長さに満たない量で打ち切った。BOT 6.7.3: 宣言長を受け取るか、Bulk-OUT を STALL して打ち切る。
         * 受け取った分で、データの段階を終え、Bulk-OUT を STALL する。 */
        if (!b->out_halted) {
            stall_ep(b, MSC_BOT_EP_OUT);
        }
        status_phase(b, 0, b->dout_received);
        return;
    }
    dout_next(b);
}

/* ---- EXECUTE の完了 ------------------------------------------------------- */
static void exec_done(msc_bot_t *b, const msc_bot_cmd_result_t *res) {
    b->cmd_outstanding = false;

    bool is_read = false;
    msc_bot_dir_t d = MSC_BOT_DIR_NONE;
    uint64_t l = 0;
    if (res->good && res->is_read) {
        /* INTERFACES.md 7.3: READ の転送長は read.blocks x 2048。data_len は見ない。 */
        is_read = true;
        d = MSC_BOT_DIR_IN;
        l = (uint64_t)res->read_blocks * MSC_BOT_BLOCK_SIZE;
    } else {
        d = res->dev_dir;
        l = res->data_len;
        if (d != MSC_BOT_DIR_IN && d != MSC_BOT_DIR_OUT) {
            d = MSC_BOT_DIR_NONE;
            l = 0;
        }
    }
    if (l == 0) {
        d = MSC_BOT_DIR_NONE;
        is_read = false;
    }

    /* READ 以外の IN のデータは buf[0] に入っている（EXECUTE の out）。それより大きい data_len は契約違反。 */
    if (d == MSC_BOT_DIR_IN && !is_read && l > b->cfg.buf_size) {
        enter_recovery(b);
        return;
    }

    b->plan = msc_bot_decide(b->host_in, b->host_len, d, l, b->cfg.out_stall);
    b->cmd_good = res->good;
    b->din_failed = false;
    b->dout_received = 0;

    if (b->plan.xfer_dir == MSC_BOT_DIR_IN) {
        din_begin(b, is_read, res->read_lba, b->plan.xfer_len);
        din_pump(b);
    } else if (b->plan.xfer_dir == MSC_BOT_DIR_OUT) {
        finish_cmd(b);
        b->state = MSC_BOT_DATA_OUT;
        b->dout_total = b->plan.xfer_len;
        b->dout_received = 0;
        dout_next(b);
    } else {
        finish_cmd(b);
        status_phase(b, 0, 0);
    }
}

/* ---- CBW ------------------------------------------------------------------ */
static void handle_cbw(msc_bot_t *b, uint32_t len, bool ok) {
    const uint8_t *c = b->cfg.cbw_buf;
    /* BOT 6.2.1 valid: 31 バイトで、署名が一致。 */
    if (!ok || len != MSC_BOT_CBW_LEN || rd32le(c) != MSC_BOT_CBW_SIGNATURE) {
        b->stats.invalid_cbw++;
        enter_recovery(b);
        return;
    }
    const uint32_t tag = rd32le(c + 4);
    const uint32_t xfer_len = rd32le(c + 8);
    const uint8_t flags = c[12];
    const uint8_t lun_byte = c[13];
    const uint8_t cb_byte = c[14];
    const uint8_t cb_len = (uint8_t)(cb_byte & 0x1fu);
    /* BOT 6.2.2 meaningful: 予約ビット = 0、LUN が範囲内、CDB の長さ 1..16。 */
    if ((flags & 0x7fu) != 0 || (lun_byte & 0xf0u) != 0 || (uint32_t)(lun_byte & 0x0fu) >= MSC_BOT_NUM_LUN ||
        (cb_byte & 0xe0u) != 0 || cb_len < 1u || cb_len > MSC_BOT_MAX_CDB) {
        b->stats.invalid_cbw++;
        enter_recovery(b);
        return;
    }

    b->tag = tag;
    b->host_len = xfer_len;
    b->host_in = (flags & 0x80u) != 0;
    reset_data_state(b);

    msc_bot_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = MSC_BOT_CMD_EXECUTE;
    cmd.seq = ++b->cmd_seq;
    memcpy(cmd.cdb, c + 15, cb_len);
    cmd.cdb_len = cb_len;
    cmd.dir = (xfer_len == 0) ? MSC_BOT_DIR_NONE : (b->host_in ? MSC_BOT_DIR_IN : MSC_BOT_DIR_OUT);
    cmd.expected_len = xfer_len;
    cmd.out = b->cfg.buf[0];
    cmd.out_cap = b->cfg.buf_size;

    b->state = MSC_BOT_EXEC;
    b->cmd_outstanding = true;
    b->cmd_live = true;
    if (b->cfg.ops.submit_command(b->cfg.ops.ctx, &cmd) != 0) {
        b->cmd_outstanding = false;
        b->cmd_live = false;
        enter_recovery(b);
        return;
    }
    b->stats.exec_submitted++;
}

/* ---- イベントの処理 ------------------------------------------------------- */
/* DETACHED -> IDLE。CBW の受信の発行の結果を返す。 */
static int attach_now(msc_bot_t *b) {
    b->state = MSC_BOT_IDLE;
    return arm_cbw(b);
}

static void on_attach(msc_bot_t *b) {
    if (b->state == MSC_BOT_DETACHED) {
        (void)attach_now(b);
    } else if (b->state == MSC_BOT_DETACH_WAIT) {
        b->then_attach = true;
    }
}

/* 切断の共通の処理。USB の転送を取り消し、bot が STALL した halt を解き、実行中の依頼がなければ command_done。 */
static void on_detach(msc_bot_t *b) {
    if (b->state == MSC_BOT_DETACHED) {
        return;
    }
    if (b->state == MSC_BOT_DETACH_WAIT) {
        b->then_attach = false;
        b->reset_pending = false;
        return;
    }
    cancel_xfers(b);
    if (b->in_halted) {
        b->in_halted = false;
        if (b->cfg.ops.unstall != NULL) {
            b->cfg.ops.unstall(b->cfg.ops.ctx, MSC_BOT_EP_IN);
        }
    }
    if (b->out_halted) {
        b->out_halted = false;
        if (b->cfg.ops.unstall != NULL) {
            b->cfg.ops.unstall(b->cfg.ops.ctx, MSC_BOT_EP_OUT);
        }
    }
    reset_data_state(b);
    b->then_attach = false;
    b->reset_pending = false;
    if (b->cmd_outstanding) {
        b->state = MSC_BOT_DETACH_WAIT;
    } else {
        b->state = MSC_BOT_DETACHED;
        finish_cmd(b);
    }
}

static void on_reset(msc_bot_t *b) {
    switch (b->state) {
    case MSC_BOT_DETACHED:
    case MSC_BOT_RESET_WAIT:
        return;
    case MSC_BOT_DETACH_WAIT:
        /* 再接続（ATT）の後に受けた Reset は、旧い依頼の解放と再接続の準備の後で、通知する。 */
        if (b->then_attach) {
            b->reset_pending = true;
        }
        return;
    case MSC_BOT_IDLE:
        /* 受信を発行済みの CBW は、そのまま有効（halt が残るなら、解除を待つ）。 */
        notify_reset(b, arm_cbw(b));
        return;
    default:
        break;
    }
    /* BOT 3.1: Reset は、データトグルと STALL の状態を保つ（halt は残る）。実行中の転送は捨てる。 */
    cancel_xfers(b);
    reset_data_state(b);
    if (b->cmd_outstanding) {
        b->state = MSC_BOT_RESET_WAIT;
        return;
    }
    finish_cmd(b);
    b->state = MSC_BOT_IDLE;
    notify_reset(b, arm_cbw(b));
}

static void on_halt_cleared(msc_bot_t *b, msc_bot_ep_t ep) {
    switch (b->state) {
    case MSC_BOT_DETACHED:
    case MSC_BOT_DETACH_WAIT:
        return;
    case MSC_BOT_RECOVERY:
        /* BOT 6.6.1: Reset Recovery（Reset が先）まで、STALL を保つ。回復の前の解除は無効。 */
        stall_ep(b, ep);
        return;
    default:
        break;
    }
    if (ep == MSC_BOT_EP_IN) {
        b->in_halted = false;
        if (b->state == MSC_BOT_STATUS_WAIT_UNSTALL) {
            send_csw(b);
            return;
        }
    } else {
        b->out_halted = false;
    }
    if (b->state == MSC_BOT_IDLE) {
        arm_cbw(b);
    }
}

static void on_xfer_done(msc_bot_t *b, msc_bot_ep_t ep, uint32_t len, bool ok) {
    if (ep == MSC_BOT_EP_IN) {
        if (!b->in_pending) {
            b->stats.stale_events++;
            return;
        }
        b->in_pending = false;
        if (b->state == MSC_BOT_DATA_IN) {
            din_on_in_done(b, len, ok);
        } else if (b->state == MSC_BOT_STATUS_SEND) {
            if (!ok || len != b->in_req_len) {
                enter_recovery(b);
                return;
            }
            b->state = MSC_BOT_IDLE;
            arm_cbw(b);
        } else {
            b->stats.stale_events++;
        }
    } else {
        if (!b->out_pending) {
            b->stats.stale_events++;
            return;
        }
        b->out_pending = false;
        if (b->state == MSC_BOT_IDLE) {
            handle_cbw(b, len, ok);
        } else if (b->state == MSC_BOT_DATA_OUT) {
            dout_on_done(b, len, ok);
        } else {
            b->stats.stale_events++;
        }
    }
}

static void on_cmd_done(msc_bot_t *b, const msc_bot_cmd_result_t *res) {
    if (!b->cmd_outstanding || res->seq != b->cmd_seq) {
        b->stats.stale_events++;
        return;
    }
    switch (b->state) {
    case MSC_BOT_EXEC:
        exec_done(b, res);
        break;
    case MSC_BOT_DATA_IN:
        din_on_cmd_done(b, res);
        break;
    case MSC_BOT_RESET_WAIT:
        b->cmd_outstanding = false;
        finish_cmd(b);
        b->state = MSC_BOT_IDLE;
        notify_reset(b, arm_cbw(b));
        break;
    case MSC_BOT_DETACH_WAIT:
        b->cmd_outstanding = false;
        finish_cmd(b);
        b->state = MSC_BOT_DETACHED;
        if (b->then_attach) {
            b->then_attach = false;
            const int arm = attach_now(b);
            if (b->reset_pending) {
                b->reset_pending = false;
                notify_reset(b, arm);
            }
        }
        b->reset_pending = false;
        break;
    default:
        /* RECOVERY など。結果は捨てて、コマンドを終える。 */
        b->cmd_outstanding = false;
        finish_cmd(b);
        break;
    }
}

static void dispatch(msc_bot_t *b, const msc_bot_event_t *e) {
    switch (e->type) {
    case EV_ATTACH:
        on_attach(b);
        break;
    case EV_DETACH:
        on_detach(b);
        break;
    case EV_RESET:
        on_reset(b);
        break;
    case EV_HALT_CLEARED:
        on_halt_cleared(b, (msc_bot_ep_t)e->ep);
        break;
    case EV_XFER_DONE:
        if (e->gen != b->xfer_gen) {
            /* 取り消す前に投入された、旧世代の完了。pending は変えない。 */
            b->stats.stale_events++;
            break;
        }
        on_xfer_done(b, (msc_bot_ep_t)e->ep, e->len, e->ok);
        break;
    case EV_CMD_DONE:
        on_cmd_done(b, &e->res);
        break;
    default:
        break;
    }
}

/* イベントを FIFO に積み、外側の呼び出しなら、空になるまで処理する（ops から再入しても、再帰しない）。 */
static void post(msc_bot_t *b, const msc_bot_event_t *e) {
    if (b == NULL || !b->inited) {
        return;
    }
    if (b->evq_count >= MSC_BOT_EVQ_LEN) {
        b->stats.queue_overflow++;
        return;
    }
    const uint32_t tail = ((uint32_t)b->evq_head + b->evq_count) % MSC_BOT_EVQ_LEN;
    b->evq[tail] = *e;
    if (e->type == EV_XFER_DONE) {
        b->evq[tail].gen = b->xfer_gen; /* 投入した時点の世代 */
    }
    b->evq_count++;
    if (b->in_pump) {
        return;
    }
    b->in_pump = true;
    while (b->evq_count > 0) {
        const msc_bot_event_t cur = b->evq[b->evq_head];
        b->evq_head = (uint8_t)((b->evq_head + 1u) % MSC_BOT_EVQ_LEN);
        b->evq_count--;
        dispatch(b, &cur);
    }
    b->in_pump = false;
}

/* ---- 公開 API ------------------------------------------------------------- */
int msc_bot_init(msc_bot_t *bot, const msc_bot_cfg_t *cfg) {
    if (bot == NULL || cfg == NULL) {
        return MSC_BOT_ERR_ARG;
    }
    const msc_bot_ops_t *o = &cfg->ops;
    if (o->xfer_in == NULL || o->xfer_out == NULL || o->stall_in == NULL || o->stall_out == NULL ||
        o->cancel_xfers == NULL || o->submit_command == NULL || o->command_done == NULL) {
        return MSC_BOT_ERR_ARG;
    }
    if (cfg->cbw_buf == NULL || cfg->csw_buf == NULL || cfg->buf[0] == NULL || cfg->buf[1] == NULL) {
        return MSC_BOT_ERR_ARG;
    }
    if (cfg->buf_size < MSC_BOT_BLOCK_SIZE || (cfg->buf_size % MSC_BOT_BLOCK_SIZE) != 0) {
        return MSC_BOT_ERR_ARG;
    }
    {
        /* 2 つのバッファの範囲（buf_size バイトずつ）が重ならない（隣接はよい）。終端を計算せず、差で比べる。 */
        const uintptr_t a0 = (uintptr_t)cfg->buf[0];
        const uintptr_t a1 = (uintptr_t)cfg->buf[1];
        const uintptr_t diff = a0 > a1 ? a0 - a1 : a1 - a0;
        if (diff < (uintptr_t)cfg->buf_size) {
            return MSC_BOT_ERR_ARG;
        }
    }
    memset(bot, 0, sizeof(*bot));
    bot->cfg = *cfg;
    bot->state = MSC_BOT_DETACHED;
    bot->cmd_good = true;
    bot->inited = true;
    return MSC_BOT_OK;
}

void msc_bot_attach(msc_bot_t *bot) {
    msc_bot_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_ATTACH;
    post(bot, &e);
}

void msc_bot_detach(msc_bot_t *bot) {
    msc_bot_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_DETACH;
    post(bot, &e);
}

void msc_bot_reset(msc_bot_t *bot) {
    msc_bot_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_RESET;
    post(bot, &e);
}

void msc_bot_halt_cleared(msc_bot_t *bot, msc_bot_ep_t ep) {
    msc_bot_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_HALT_CLEARED;
    e.ep = (uint8_t)ep;
    post(bot, &e);
}

void msc_bot_xfer_complete(msc_bot_t *bot, msc_bot_ep_t ep, uint32_t actual_len, bool ok) {
    msc_bot_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_XFER_DONE;
    e.ep = (uint8_t)ep;
    e.len = actual_len;
    e.ok = ok;
    post(bot, &e);
}

void msc_bot_command_complete(msc_bot_t *bot, const msc_bot_cmd_result_t *res) {
    if (res == NULL) {
        return;
    }
    msc_bot_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_CMD_DONE;
    e.res = *res;
    post(bot, &e);
}

msc_bot_state_t msc_bot_state(const msc_bot_t *bot) { return bot->state; }

const char *msc_bot_state_name(msc_bot_state_t s) {
    switch (s) {
    case MSC_BOT_DETACHED:
        return "DETACHED";
    case MSC_BOT_IDLE:
        return "IDLE";
    case MSC_BOT_EXEC:
        return "EXEC";
    case MSC_BOT_DATA_IN:
        return "DATA_IN";
    case MSC_BOT_DATA_OUT:
        return "DATA_OUT";
    case MSC_BOT_STATUS_WAIT_UNSTALL:
        return "STATUS_WAIT_UNSTALL";
    case MSC_BOT_STATUS_SEND:
        return "STATUS_SEND";
    case MSC_BOT_RECOVERY:
        return "RECOVERY";
    case MSC_BOT_RESET_WAIT:
        return "RESET_WAIT";
    case MSC_BOT_DETACH_WAIT:
        return "DETACH_WAIT";
    default:
        return "?";
    }
}

bool msc_bot_halted(const msc_bot_t *bot, msc_bot_ep_t ep) {
    return ep == MSC_BOT_EP_IN ? bot->in_halted : bot->out_halted;
}

bool msc_bot_cmd_live(const msc_bot_t *bot) { return bot->cmd_live; }

bool msc_bot_cmd_outstanding(const msc_bot_t *bot) { return bot->cmd_outstanding; }

msc_bot_stats_t msc_bot_stats(const msc_bot_t *bot) { return bot->stats; }
