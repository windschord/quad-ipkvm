/*
 * msc_bot.h -- USB Mass Storage Bulk-Only Transport (BOT) の状態機械（純 C）。P8 の C-13（Issue #143）。
 *
 * 契約: docs/ISO_MOUNT_DESIGN.md §4.1・§5.1・§9、docs/INTERFACES.md §7.3。テストの説明: docs/MSC_BOT_TESTS.md。
 * 出典: "Universal Serial Bus Mass Storage Class, Bulk-Only Transport, Revision 1.0" (usb.org, 1999-09)。
 *       以下「BOT x.y」はその節。13 のケースは BOT 6.7、CBW/CSW は BOT 5.1/5.2、エラー処理は BOT 6.6。
 *
 * ---- 何をするか ----
 * USB のハードウェアから切り離した、BOT の状態機械。CBW の検証、13 のケースの判定、データ IN（ダブルバッファの分割転送）、
 * データ OUT の読み捨て（または STALL）、CSW、Bulk-Only Mass Storage Reset / CLEAR_FEATURE(ENDPOINT_HALT) からの復帰、
 * 切断、そして `command_done`（= mmc_scsi_command_done）をコマンドごとにちょうど 1 回呼ぶことを担う。
 * USB の転送とコマンドの実行は、関数ポインタ（msc_bot_ops_t）で注入する。結果はイベント（msc_bot_xfer_complete など）で戻る。
 *
 * ---- スレッドモデル（ISO_MOUNT_DESIGN.md §5.1「msc_bot のスレッドモデル」）----
 *  - msc_bot の状態は、USB のタスクだけが触る。msc_bot_* の全ての関数は、USB のタスクから呼ぶ。
 *  - コマンドの実行（mmc_scsi_execute / mmc_scsi_read_blocks。ブロックする）は、別のタスク（iso_msc_task）が行う。
 *    submit_command は、実行の依頼を iso_msc_task のキューに積むだけで、すぐ戻る。完了は、iso_msc_task が USB のタスクへ
 *    キューで返し、USB のタスクが msc_bot_command_complete() を呼ぶ。
 *  - 注入した関数（ops）の中から、msc_bot_* の関数（イベントの入力）を呼んでもよい。イベントは内部の FIFO に積まれ、
 *    いま処理中のイベントが終わってから、受け取った順に処理される（再入しない。再帰もしない）。同期的に完了する
 *    ホストテストのモックのため。1 回の ops の呼び出しが積んでよいイベントは 1 つまで（FIFO は MSC_BOT_EVQ_LEN 個。
 *    あふれたら、イベントを捨て、stats.queue_overflow を数える。これは契約違反）。
 *
 * ---- 同時に動くもの ----
 *  - 実行中のコマンド（iso_msc_task への依頼）は、同時に最大 1 つ。USB の転送は、IN と OUT で、それぞれ最大 1 つ。
 *  - 1 つの CBW のコマンドは、EXECUTE（mmc_scsi_execute）1 回と、READ なら READ_BLOCKS（mmc_scsi_read_blocks）の
 *    複数回。`command_done` は、EXECUTE を依頼したコマンドごとに、ちょうど 1 回。呼ぶ時刻は、「そのコマンドのために、
 *    iso_msc_task が mmc_scsi を呼ぶことが、もうなくなった」直後（最後の完了イベントの処理の中）。CSW の送信を待たない。
 *    実行中の依頼があるときは、呼ばない（Reset / 切断 / 不正な状態でも、その完了イベントを待ってから呼ぶ）。
 *
 * ---- 状態 ----
 *   DETACHED  接続されていない（初期状態。msc_bot_attach を待つ）
 *   IDLE      CBW を待つ（Bulk-OUT に 31 バイトの受信を発行済み。halt が残っていれば、解除を待ってから発行）
 *   EXEC      EXECUTE の完了を待つ
 *   DATA_IN   データ IN の転送中（READ は、ダブルバッファ。READ_BLOCKS と xfer_in が重なる）
 *   DATA_OUT  OUT のデータを読み捨てている
 *   STATUS_WAIT_UNSTALL  Bulk-IN を STALL した。ホストの CLEAR_FEATURE(HALT, IN) を待ってから CSW を送る
 *   STATUS_SEND          CSW の送信中
 *   RECOVERY  CBW が不正、または内部エラー。BOT 6.6.1 / 6.6.2: 両 Bulk を STALL し、Reset Recovery まで保つ
 *   RESET_WAIT   Reset を受けたが、実行中の依頼がある。完了を待つ
 *   DETACH_WAIT  切断されたが、実行中の依頼がある。完了を待つ
 *
 * ---- イベント ----
 *   ATT   msc_bot_attach          バスが使える（SET_CONFIGURATION 済み）
 *   DET   msc_bot_detach          切断、バスリセット、構成の解除
 *   RST   msc_bot_reset           Bulk-Only Mass Storage Reset（class request FFh。BOT 3.1）
 *   HIN   msc_bot_halt_cleared(IN)   CLEAR_FEATURE(ENDPOINT_HALT) が Bulk-IN に来た
 *   HOUT  msc_bot_halt_cleared(OUT)  同、Bulk-OUT
 *   OUTok / OUTer  msc_bot_xfer_complete(OUT, ok / 失敗)
 *   INok  / INer   msc_bot_xfer_complete(IN,  ok / 失敗)
 *   CMD   msc_bot_command_complete  iso_msc_task からの完了（EXECUTE でも READ_BLOCKS でも。seq が合うものだけ有効）
 *   CMDf  CMD のうち、READ_BLOCKS が失敗（good = false）。EXECUTE の good = false（CHECK CONDITION）は、失敗ではなく
 *         通常の結果（CSW の bCSWStatus = 1 になる）で、CMD と同じ扱い。
 *
 * ---- 状態遷移表（状態 × イベント → 次の状態 / アクション。テスト: test_msc_bot_table.c が全行を固定）----
 * 「(stale)」= 該当する依頼がないので、無視して stats.stale_events を数える。「-」= 何もしない（状態も変わらない）。
 * 「arm」= IN / OUT の halt がどちらもなければ、Bulk-OUT に CBW（31 バイト）の受信を発行（xfer_out）。
 * 「cancel」= 発行済みの USB の転送があれば cancel_xfers を 1 回呼ぶ（以後、その完了は来ない）。
 * 「unstall」= bot が STALL した方の halt を、unstall で解く。
 *
 *  状態 \ イベント | ATT          | DET                              | RST                                       | HIN
 *  ----------------+--------------+----------------------------------+-------------------------------------------+----------------------
 *  DETACHED        | IDLE; arm    | -                                | - (reset_done は呼ばない)                  | -
 *  IDLE            | -            | DETACHED; cancel, unstall        | -; reset_done（CBW の受信はそのまま）       | -; halt 解除; arm
 *  EXEC            | -            | DETACH_WAIT; cancel, unstall     | RESET_WAIT; cancel                        | -; halt 解除
 *  DATA_IN         | -            | 実行中あり: DETACH_WAIT          | 実行中あり: RESET_WAIT                     | -; halt 解除
 *                  |              | なし: DETACHED; done             | なし: IDLE; done, arm, reset_done          |
 *                  |              | (どちらも cancel, unstall)       | (どちらも cancel)                          |
 *  DATA_OUT        | -            | DETACH 同上                      | RESET 同上                                 | -; halt 解除
 *  STATUS_WAIT_UNSTALL | -        | DETACH 同上                      | RESET 同上                                 | STATUS_SEND; CSW を xfer_in
 *  STATUS_SEND     | -            | DETACH 同上                      | RESET 同上                                 | -; halt 解除
 *  RECOVERY        | -            | DETACH 同上                      | RESET 同上（IDLE になっても halt は残る）    | RECOVERY; stall_in を再度（回復の前の解除は無効）
 *  RESET_WAIT      | -            | DETACH_WAIT                      | -                                         | -; halt 解除
 *  DETACH_WAIT     | 再接続を記録  | 再接続の記録を消す                | -                                         | -
 *
 *  状態 \ イベント | HOUT         | OUTok                                  | OUTer              | INok                         | INer
 *  ----------------+--------------+----------------------------------------+--------------------+------------------------------+----------------
 *  DETACHED        | -            | (stale)                                | (stale)            | (stale)                      | (stale)
 *  IDLE            | -; halt 解除; arm | CBW が有効: EXEC; submit EXECUTE    | RECOVERY           | (stale)                      | (stale)
 *                  |              | CBW が不正: RECOVERY; stall_in, stall_out |                  |                              |
 *  EXEC            | -; halt 解除 | (stale)                                | (stale)            | (stale)                      | (stale)
 *  DATA_IN         | -; halt 解除 | (stale)                                | (stale)            | DATA_IN（次の転送）、または   | RECOVERY; stall_in,
 *                  |              |                                        |                    | データの終わりで STATUS_*     | stall_out
 *  DATA_OUT        | -; halt 解除 | DATA_OUT（次の受信）、または終わりで    | RECOVERY           | (stale)                      | (stale)
 *                  |              | STATUS_*                               |                    |                              |
 *  STATUS_WAIT_UNSTALL | -; halt 解除 | (stale)                            | (stale)            | (stale)                      | (stale)
 *  STATUS_SEND     | -; halt 解除 | (stale)                                | (stale)            | IDLE; arm                    | RECOVERY
 *  RECOVERY        | RECOVERY; stall_out を再度 | (stale)                  | (stale)            | (stale)                      | (stale)
 *  RESET_WAIT      | -; halt 解除 | (stale)                                | (stale)            | (stale)                      | (stale)
 *  DETACH_WAIT     | -            | (stale)                                | (stale)            | (stale)                      | (stale)
 *
 *  状態 \ イベント | CMD（完了。seq が一致し、実行中の依頼があるときだけ。ないときは stale）
 *  ----------------+--------------------------------------------------------------------------------------------------
 *  DETACHED        | (stale)
 *  IDLE            | (stale)
 *  EXEC            | 13 のケースの判定: データ IN → DATA_IN（READ は submit READ_BLOCKS; done は最後の READ の後。READ 以外は done して xfer_in）
 *                  |                    データ OUT → DATA_OUT（done して xfer_out）
 *                  |                    データなし → STATUS_SEND（done して CSW）。IN の不足は stall_in して STATUS_WAIT_UNSTALL
 *                  |                    結果が契約違反（READ 以外の data_len > バッファ）→ RECOVERY（done して両 STALL）
 *  DATA_IN         | READ_BLOCKS ok: DATA_IN（バッファを READY にして、次の READ と xfer_in を進める）
 *                  | READ_BLOCKS 失敗（CMDf）: DATA_IN のまま、以後の READ を止める。転送中の xfer_in が終わったら
 *                  |                    stall_in して STATUS_WAIT_UNSTALL（CSW の bCSWStatus = 1）。done は失敗の完了の直後
 *  DATA_OUT        | (stale: DATA_OUT のとき、実行中の依頼はない)
 *  STATUS_WAIT_UNSTALL | (stale)
 *  STATUS_SEND     | (stale)
 *  RECOVERY        | -; done（実行中だった依頼の完了。結果は捨てる）
 *  RESET_WAIT      | IDLE; done, arm, reset_done
 *  DETACH_WAIT     | DETACHED; done。再接続の記録があれば続けて ATT（IDLE; arm）
 *
 * ---- 13 のケース（BOT 6.7。msc_bot_decide / msc_bot_resolve。テスト: test_msc_bot_cases.c が全ケースを固定）----
 * ホストの期待（H = dCBWDataTransferLength、方向）と、デバイスの結果（D = 向きと長さ L）の組み合わせ。
 * 「送る」「読み捨てる」は実際に転送する量。stall は、Bulk-IN の STALL を CSW の前に行うか。
 * CSW の bCSWStatus は、「cmd」= コマンドの結果（good なら 0、失敗なら 1。BOT 5.2）、または 2（Phase Error）。
 * dCSWDataResidue = H - 処理した量（IN: 実際に送った量。OUT: 受け取って処理した量）。
 *
 *  ケース | ホスト | デバイス      | 実際の転送                       | STALL (IN) | bCSWStatus | residue
 *  -------+--------+---------------+----------------------------------+------------+------------+---------------------
 *   1     | Hn     | Dn            | なし                             | なし       | cmd        | 0
 *   2     | Hn     | Di            | なし                             | なし (*1)  | 2          | 0
 *   3     | Hn     | Do            | なし                             | なし (*1)  | 2          | 0
 *   4     | Hi > Dn               | なし                             | あり       | cmd        | H
 *   5     | Hi > Di (L < H)       | L バイトを送る                   | あり       | cmd        | H - L
 *   6     | Hi = Di (L = H)       | H バイトを送る                   | なし       | cmd        | 0
 *   7     | Hi < Di (L > H)       | H バイトを送る                   | なし (*2)  | 2          | 0
 *   8     | Hi <> Do              | なし                             | あり       | 2          | H
 *   9     | Ho > Dn               | H バイトを読み捨てる (*3)        | なし       | cmd        | H
 *   10    | Ho <> Di              | H バイトを読み捨てる (*3)        | なし       | 2          | H - 受け取った量
 *   11    | Ho > Do (L < H)       | H バイトを読み捨てる (*3)        | なし       | cmd        | H - L
 *   12    | Ho = Do (L = H)       | H バイトを読み捨てる (*3)        | なし       | cmd        | 0
 *   13    | Ho < Do (L > H)       | H バイトを読み捨てる (*3)        | なし       | 2          | H - 受け取った量
 *
 *  (*1) BOT 6.7.1 は「Bulk-IN を STALL してもよい（may）」。STALL しない（CSW をそのまま返す。ホストは Reset Recovery する）。
 *  (*2) BOT 6.7.2 は「H バイトを送ったなら STALL してもよい（may）」。STALL しない。
 *  (*3) BOT 6.7.3: 「H バイトを受け取る、または Bulk-OUT を STALL して打ち切る」のどちらか（shall）。
 *       既定は読み捨て。msc_bot_cfg_t.out_stall = true なら、受け取らずに Bulk-OUT を STALL（residue = H - 0 = H）。
 *       そのとき bCSWStatus はケースの値のまま。
 *  Hn のときは、方向のビットを無視する（BOT 5.1）。L = 0 のデバイスの結果は Dn。
 *  IN の転送は、実際に送った量が H に満たなければ（ケース 4、5、8、および IN の途中の失敗）、CSW の前に Bulk-IN を STALL する
 *  （BOT 6.7.2。「送った量 < H」で動的に決まる）。IN の途中で READ が失敗したら、bCSWStatus = 1（ケース 7 のときは 2 のまま）。
 *  READ(10)/(12) の L は、read.blocks x 2048（data_len は見ない。INTERFACES.md 7.3）。
 *
 * ---- CBW の検証（BOT 5.1、6.2、6.6.1）----
 * 31 バイト、署名 `USBC`（43425355h）。さらに「meaningful」の条件（予約ビット = 0、LUN が範囲内、CDB の長さ 1..16）。
 * 満たさない CBW は、いずれも BOT 6.6.1 に従う（IN を STALL。OUT も STALL。Reset Recovery まで保つ）。
 * （meaningful でない CBW への応答は BOT 6.4 で未規定。issue #143 の指示で、不正な CBW と同じ扱いにしている。）
 */
#ifndef MSC_BOT_H
#define MSC_BOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSC_BOT_CBW_LEN 31u
#define MSC_BOT_CSW_LEN 13u
#define MSC_BOT_CBW_SIGNATURE 0x43425355u /* "USBC" (little endian) */
#define MSC_BOT_CSW_SIGNATURE 0x53425355u /* "USBS" */
#define MSC_BOT_BLOCK_SIZE 2048u          /* CD-ROM のブロック長（ISO_MOUNT_DESIGN.md §5.4） */
#define MSC_BOT_MAX_CDB 16u
#define MSC_BOT_NUM_LUN 1u                /* LUN は 1 つ（有効な LUN は 0 だけ） */
#define MSC_BOT_CBW_BUF_SIZE 32u          /* cfg.cbw_buf の最小の大きさ */
#define MSC_BOT_CSW_BUF_SIZE 16u          /* cfg.csw_buf の最小の大きさ */
#define MSC_BOT_EVQ_LEN 8u

#define MSC_BOT_CSW_PASSED ((uint8_t)0)
#define MSC_BOT_CSW_FAILED ((uint8_t)1)
#define MSC_BOT_CSW_PHASE_ERROR ((uint8_t)2)

typedef enum { MSC_BOT_EP_IN = 0, MSC_BOT_EP_OUT = 1 } msc_bot_ep_t;

/* 向き（ホストの期待 = CBW の方向、デバイスの結果 = 実行の結果）。 */
typedef enum { MSC_BOT_DIR_NONE = 0, MSC_BOT_DIR_IN = 1, MSC_BOT_DIR_OUT = 2 } msc_bot_dir_t;

typedef enum {
    MSC_BOT_DETACHED = 0,
    MSC_BOT_IDLE,
    MSC_BOT_EXEC,
    MSC_BOT_DATA_IN,
    MSC_BOT_DATA_OUT,
    MSC_BOT_STATUS_WAIT_UNSTALL,
    MSC_BOT_STATUS_SEND,
    MSC_BOT_RECOVERY,
    MSC_BOT_RESET_WAIT,
    MSC_BOT_DETACH_WAIT,
    MSC_BOT_STATE_COUNT
} msc_bot_state_t;

/* ---- iso_msc_task への依頼と、その完了 ------------------------------------ */
typedef enum {
    MSC_BOT_CMD_EXECUTE = 1,    /* mmc_scsi_execute(cdb, cdb_len, dir, expected_len, out, out_cap, &result) */
    MSC_BOT_CMD_READ_BLOCKS = 2 /* mmc_scsi_read_blocks(lba, blocks, buf, cap) */
} msc_bot_cmd_kind_t;

typedef struct {
    msc_bot_cmd_kind_t kind;
    uint32_t seq;            /* 完了イベントに、そのまま返す */
    /* EXECUTE */
    uint8_t cdb[MSC_BOT_MAX_CDB];
    uint8_t cdb_len;         /* 1..16 */
    msc_bot_dir_t dir;       /* CBW の方向（dCBWDataTransferLength = 0 なら NONE） */
    uint32_t expected_len;   /* dCBWDataTransferLength */
    uint8_t *out;            /* READ 以外の IN のデータの置き場所 */
    uint32_t out_cap;
    /* READ_BLOCKS */
    uint64_t lba;
    uint32_t blocks;
    uint8_t *buf;            /* blocks x 2048 バイト以上 */
    uint32_t cap;
} msc_bot_cmd_t;

typedef struct {
    uint32_t seq;            /* msc_bot_cmd_t.seq と同じ値 */
    /* EXECUTE: ステータスが GOOD か（false = CHECK CONDITION）。READ_BLOCKS: 読み出しが成功したか */
    bool good;
    /* ここから下は EXECUTE のときだけ（mmc_scsi_execute の result から、グルーが写す）。 */
    msc_bot_dir_t dev_dir;   /* デバイスの結果の向き（NONE / IN / OUT）。READ の場合は見ない（IN とみなす） */
    uint32_t data_len;       /* READ 以外: out に入れたバイト数（IN）、または受け取る予定のバイト数（OUT）。READ では見ない */
    bool is_read;            /* result.read.is_read */
    uint64_t read_lba;
    uint32_t read_blocks;    /* 転送長は read_blocks x 2048（data_len は使わない） */
} msc_bot_cmd_result_t;

/* ---- 注入する関数 -------------------------------------------------------- */
typedef struct {
    void *ctx;
    /* USB の転送の発行（Bulk-IN: デバイス -> ホスト）。0 = 受け付けた、非 0 = 受け付けない（内部エラー扱い）。
     * 完了は msc_bot_xfer_complete(IN, actual_len, ok)。IN で同時に発行するのは 1 つまで。 */
    int (*xfer_in)(void *ctx, const uint8_t *buf, uint32_t len);
    /* Bulk-OUT（ホスト -> デバイス）。完了は msc_bot_xfer_complete(OUT, actual_len, ok)。 */
    int (*xfer_out)(void *ctx, uint8_t *buf, uint32_t len);
    void (*stall_in)(void *ctx);
    void (*stall_out)(void *ctx);
    /* bot が STALL した halt を、bot 自身が解く（切断のとき）。NULL でもよい（ホストの CLEAR_FEATURE に任せる）。 */
    void (*unstall)(void *ctx, msc_bot_ep_t ep);
    /* 発行済みの Bulk の転送を取り消す（Reset、切断、内部エラーのとき）。呼んだ後、取り消した転送の完了は来ない。 */
    void (*cancel_xfers)(void *ctx);
    /* 実行の依頼（iso_msc_task のキューに積む）。0 = 受け付けた、非 0 = 受け付けない。cmd は、この呼び出しの間だけ有効
     * （必要なら、写す）。cmd の中のバッファ（out、buf）は、完了イベントまで、bot は触らない。 */
    int (*submit_command)(void *ctx, const msc_bot_cmd_t *cmd);
    /* mmc_scsi_command_done(ctx) を呼ぶ。EXECUTE を依頼したコマンドごとに、ちょうど 1 回。 */
    void (*command_done)(void *ctx);
    /* 省略可（NULL）。Bulk-Only Mass Storage Reset が完了した（次の CBW を受けられる）。コントロール転送の
     * ステータスステージを NAK し続ける（BOT 3.1）グルーは、これで ACK する。 */
    void (*reset_done)(void *ctx);
} msc_bot_ops_t;

typedef struct {
    msc_bot_ops_t ops;
    uint8_t *cbw_buf;        /* 32 バイト以上（DMA 可能で、キャッシュラインに合わせる。呼び出し側が確保） */
    uint8_t *csw_buf;        /* 16 バイト以上（同上） */
    uint8_t *buf[2];         /* データのバッファ 2 つ（DMA 可能。互いに重ならない） */
    uint32_t buf_size;       /* 1 つの大きさ。2048 の倍数（>= 2048）。1 回の転送の最大 */
    bool out_stall;          /* false = OUT のデータを受け取って読み捨てる（既定）、true = Bulk-OUT を STALL する */
} msc_bot_cfg_t;

/* ---- 判定（純関数。13 のケース）----------------------------------------- */
typedef struct {
    uint8_t case_no;         /* 1..13 */
    msc_bot_dir_t host_dir;  /* NONE（H = 0）/ IN / OUT */
    uint32_t host_len;       /* H */
    msc_bot_dir_t dev_dir;   /* 正規化した（L = 0 なら NONE） */
    uint64_t dev_len;        /* L */
    bool phase_error;        /* ケース 2, 3, 7, 8, 10, 13 */
    msc_bot_dir_t xfer_dir;  /* データの段階で実際に転送する向き（NONE / IN / OUT） */
    uint32_t xfer_len;       /* IN: 送る量、OUT: 読み捨てる量 */
    bool stall_out;          /* out_stall のとき、OUT のケースで Bulk-OUT を STALL する */
    uint32_t out_processed;  /* OUT のケース（非 Phase Error）で、デバイスが処理する量（residue = H - この量。受信した量が上限） */
} msc_bot_plan_t;

typedef struct {
    uint8_t status;          /* bCSWStatus */
    uint32_t residue;        /* dCSWDataResidue（H 以下） */
    bool stall_in;           /* CSW の前に Bulk-IN を STALL するか */
    bool stall_out;
} msc_bot_csw_t;

/* host_dir_in_flag: CBW の方向のビット。host_len = 0 なら、方向は無視する。dev_dir / dev_len は、デバイスの結果。 */
msc_bot_plan_t msc_bot_decide(bool host_dir_in_flag, uint32_t host_len, msc_bot_dir_t dev_dir, uint64_t dev_len,
                              bool out_stall);
/* cmd_good: コマンドの結果（false なら CSW は Failed）。sent_in: 実際に送った量。received_out: 実際に受け取った量。 */
msc_bot_csw_t msc_bot_resolve(const msc_bot_plan_t *plan, bool cmd_good, uint32_t sent_in, uint32_t received_out);

/* ---- 本体 ---------------------------------------------------------------- */
#define MSC_BOT_OK 0
#define MSC_BOT_ERR_ARG (-1)

typedef struct {
    uint32_t exec_submitted;   /* 受け付けられた EXECUTE の数（command_done を呼ぶべき数） */
    uint32_t cmd_done_called;  /* command_done を呼んだ数 */
    uint32_t stale_events;     /* 対応する依頼がないので捨てたイベントの数 */
    uint32_t queue_overflow;   /* FIFO のあふれ（契約違反） */
    uint32_t invalid_cbw;      /* 不正な CBW の数 */
} msc_bot_stats_t;

typedef struct {
    uint8_t type;
    uint8_t ep;
    bool ok;
    uint32_t len;
    msc_bot_cmd_result_t res;
} msc_bot_event_t;

typedef struct {
    msc_bot_cfg_t cfg;
    msc_bot_state_t state;
    bool inited;
    /* イベントの FIFO（再入の防止） */
    msc_bot_event_t evq[MSC_BOT_EVQ_LEN];
    uint8_t evq_head, evq_count;
    bool in_pump;
    /* USB の転送 */
    bool in_pending, out_pending;
    uint32_t in_req_len, out_req_len;
    bool in_halted, out_halted;   /* bot が STALL して、ホストがまだ解いていない */
    /* コマンド */
    bool cmd_outstanding;         /* iso_msc_task が実行中 */
    bool cmd_live;                /* EXECUTE を依頼して、command_done をまだ呼んでいない */
    uint32_t cmd_seq;
    bool then_attach;             /* DETACH_WAIT の間に ATT が来た */
    /* CBW */
    uint32_t tag;
    uint32_t host_len;
    bool host_in;
    msc_bot_plan_t plan;
    bool cmd_good;
    /* データ IN */
    uint32_t din_total, din_issued, din_sent;
    uint64_t din_lba;
    uint8_t buf_st[2];
    uint32_t buf_len[2];
    uint8_t din_fill, din_send, din_cur;
    bool din_failed;
    /* データ OUT */
    uint32_t dout_total, dout_received;
    /* CSW */
    msc_bot_csw_t csw;
    msc_bot_stats_t stats;
} msc_bot_t;

/* 初期化（状態は DETACHED）。cfg の検査に失敗したら MSC_BOT_ERR_ARG。cfg は、バッファの指す先を除いて、コピーする。 */
int msc_bot_init(msc_bot_t *bot, const msc_bot_cfg_t *cfg);

/* イベントの入力（USB のタスクから）。 */
void msc_bot_attach(msc_bot_t *bot);
void msc_bot_detach(msc_bot_t *bot);
void msc_bot_reset(msc_bot_t *bot);
void msc_bot_halt_cleared(msc_bot_t *bot, msc_bot_ep_t ep);
/* actual_len: 実際に転送したバイト数。ok = false は、転送の失敗。 */
void msc_bot_xfer_complete(msc_bot_t *bot, msc_bot_ep_t ep, uint32_t actual_len, bool ok);
void msc_bot_command_complete(msc_bot_t *bot, const msc_bot_cmd_result_t *res);

/* 参照 */
msc_bot_state_t msc_bot_state(const msc_bot_t *bot);
const char *msc_bot_state_name(msc_bot_state_t s);
bool msc_bot_halted(const msc_bot_t *bot, msc_bot_ep_t ep);
bool msc_bot_cmd_live(const msc_bot_t *bot);        /* command_done を待っているコマンドがあるか */
bool msc_bot_cmd_outstanding(const msc_bot_t *bot); /* iso_msc_task が実行中か */
msc_bot_stats_t msc_bot_stats(const msc_bot_t *bot);

#ifdef __cplusplus
}
#endif

#endif /* MSC_BOT_H */
