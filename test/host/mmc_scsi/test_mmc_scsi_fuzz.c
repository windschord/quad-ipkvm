/*
 * mmc_scsi: 固定シードのファズ（ASan / UBSan）。任意の CDB（ランダムなバイト列と、各コマンドに沿って作って一部を壊したもの）
 * を、メディアなし / ありのどちらでも実行し、メディアの操作（マウント、取り外し、バスリセット、force_eject、PREVENT）と、
 * バックエンドの失敗（size / acquire / read）を混ぜる。全コマンドの後に、次を検査する。
 *   1. クラッシュ、範囲外の読み書きがない（out の前後に番兵）。out_cap が小さくても、あふれない。data_len <= out_cap
 *   2. status は GOOD か CHECK CONDITION。CHECK CONDITION のとき data_len = 0、read.is_read = 偽、センスは許可した組だけ。
 *      GOOD のときセンスは 0
 *   3. read.is_read は READ(10)/(12) で GOOD のときだけ。data_len = 0。lba + blocks <= capacity_blocks（64 ビットで）
 *   4. 参照: blocks > 0 の READ が GOOD のときだけ、ちょうど 1 つ保持する。それ以外は 0。バックエンドの生きた参照の数と一致。
 *      command_done の後は 0。execute の途中の失敗でも、リークしない。生きていない参照での read / release がない
 *   5. 切り詰めの性質: 同じ状態から、大きな out_cap で実行した結果の、先頭の min(data_len, out_cap) バイトと一致する
 *   6. mmc_scsi_read_blocks: 実データ（size_bytes 未満）はバックエンドの内容、それ以降は 0。バックエンドの失敗は MEDIUM ERROR で
 *      バッファを 0 にする。バスリセット / force_eject の後は ABORTED COMMAND
 * 加えて、空振りでないことを、コマンドの種類ごとの件数の下限で確認する。
 */
#include "mm_common.h"

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

void setUp(void) { fx_init(); }
void tearDown(void) {}

static int cur_seed;
static long cur_iter;
#define CHECK(cond, msg)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            char b_[160];                                                                        \
            snprintf(b_, sizeof(b_), "seed=%d iter=%ld: %s", cur_seed, cur_iter, msg);           \
            TEST_FAIL_MESSAGE(b_);                                                               \
        }                                                                                        \
    } while (0)

typedef struct {
    unsigned executed, good, cc;
    unsigned data_replies;
    unsigned reads_ok, reads_zero_blocks, read_blocks_ok, read_blocks_zero_filled, read_blocks_err, read_blocks_aborted;
    unsigned truncated;
    unsigned by_op_good[256];
    unsigned by_sense[16];
    unsigned media_ops;
} stats_t;
static stats_t ST;

static const uint8_t K_OPS[] = {0x00, 0x03, 0x12, 0x1A, 0x1B, 0x1E, 0x23, 0x25, 0x28, 0x43, 0x46, 0x4A, 0x51, 0x52, 0x5A, 0xA8};
static const uint32_t K_IDS[] = {ID_A, ID_B, ID_BIG, ID_HUGE, ID_UNKNOWN, ID_C, ID_ZERO, ID_BIGP};

static size_t cdb_len_for(uint8_t op) {
    switch (op >> 5) {
        case 0: return 6;
        case 1:
        case 2: return 10;
        case 4: return 16;
        case 5: return 12;
        default: return 10;
    }
}

static uint8_t pick_byte(void) {
    static const uint8_t v[] = {0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x08, 0x10, 0x18, 0x20, 0x3F, 0x40, 0x80, 0xC0, 0xFF};
    /* 乱数の評価順は、1 つの式に複数の rnd() を書かず、文で分けて固定する */
    uint32_t choose = rnd() % 3;
    uint32_t any = rnd();
    uint32_t idx = rnd() % (uint32_t)sizeof(v);
    return (choose == 0) ? (uint8_t)any : v[idx];
}

static uint32_t pick_lba(void) {
    switch (rnd() % 8) {
        case 0: return 0;
        case 1: return rnd() % 120;
        case 2: return 100;
        case 3: return 101;
        case 4: return CAP_BIG - 1u;
        case 5: return 0xFFFFFFFFu;
        case 6: return 0x80000000u;
        default: return rnd();
    }
}

static uint32_t pick_blocks(void) {
    switch (rnd() % 6) {
        case 0: return 0;
        case 1: return 1;
        case 2: return 1 + rnd() % 8;
        case 3: return 1 + rnd() % 120;
        case 4: return 0xFFFFFFFFu;
        default: return rnd() % 4;
    }
}

/* MODE SENSE の byte 2（PC と ページコード）。乱数の評価順を固定するため、文で分ける */
static uint8_t mode_sense_page_byte(void) {
    uint32_t pc = rnd() % 4;
    uint32_t pick_a = rnd() & 1;
    uint32_t pick_b = rnd() & 1;
    uint32_t page = pick_a ? 0x2Au : (pick_b ? 0x3Fu : 0x01u);
    return (uint8_t)((pc << 6) | page);
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v);
}

/* コマンドに沿った CDB（予約は 0）。len を返す。 */
static size_t clean_cdb(uint8_t *c, uint8_t *op_out) {
    uint8_t op = K_OPS[rnd() % sizeof(K_OPS)];
    uint16_t alloc;
    size_t len = cdb_len_for(op);
    {
        uint32_t kind = rnd() % 3;
        uint32_t small = rnd() % 40;
        uint32_t big = rnd() & 1;
        alloc = (uint16_t)(kind == 0 ? small : (big ? 0xFFFFu : 0x00FFu));
    }
    memset(c, 0, 16);
    c[0] = op;
    *op_out = op;
    switch (op) {
        case 0x03: c[4] = (uint8_t)alloc; break;
        case 0x12: put16(c + 3, alloc); break;
        case 0x1A: c[2] = mode_sense_page_byte(); c[4] = (uint8_t)alloc; break;
        case 0x5A: c[2] = mode_sense_page_byte(); put16(c + 7, alloc); break;
        case 0x1B: c[4] = (uint8_t)(rnd() % 4); break;
        case 0x1E: c[4] = (uint8_t)(rnd() % 3 == 0 ? 0 : 1); break;
        case 0x23: put16(c + 7, alloc); break;
        case 0x25: if (rnd() % 4 == 0) { c[8] = 1; put32(c + 2, pick_lba() % 130); } break;
        case 0x28: put32(c + 2, pick_lba()); put16(c + 7, (uint16_t)pick_blocks()); c[1] = (uint8_t)(rnd() % 3 == 0 ? 0x18 : 0); break;
        case 0xA8: put32(c + 2, pick_lba()); put32(c + 6, pick_blocks()); break;
        case 0x43: {
            static const uint8_t trk[] = {0, 1, 2, 0xAA};
            c[1] = (uint8_t)((rnd() & 1) << 1);
            c[2] = (uint8_t)(rnd() % 7);
            c[6] = trk[rnd() % 4];
            put16(c + 7, alloc);
            if (rnd() % 5 == 0) { c[2] = 0; c[9] = (uint8_t)((rnd() % 4) << 6); }
            break;
        }
        case 0x46: {
            static const uint16_t st[] = {0, 1, 2, 3, 0x10, 0x11, 0xFFFF};
            c[1] = (uint8_t)(rnd() % 4);
            put16(c + 2, st[rnd() % 7]);
            put16(c + 7, alloc);
            break;
        }
        case 0x4A: {
            static const uint8_t cl[] = {0x10, 0x10, 0x00, 0x7E, 0x02};
            c[1] = (uint8_t)(rnd() % 8 == 0 ? 0 : 1);
            c[4] = cl[rnd() % 5];
            {
                uint32_t short_len = rnd() % 3;
                uint32_t len_value = rnd() % 12;
                put16(c + 7, short_len == 0 ? len_value : 8u);
            }
            break;
        }
        case 0x51: {
            uint32_t use_type = rnd() % 4;
            uint32_t type = rnd() % 4;
            c[1] = (uint8_t)(use_type == 0 ? type : 0);
            put16(c + 7, alloc);
            break;
        }
        case 0x52: {
            uint32_t type = rnd() % 4;
            uint32_t small = rnd() & 1;
            uint32_t small_value = rnd() % 3;
            uint32_t lba_value = pick_lba() % 130;
            c[1] = (uint8_t)type;
            put32(c + 2, small ? small_value : lba_value);
            put16(c + 7, alloc);
            break;
        }
        default: break;
    }
    return len;
}

static size_t make_cdb(uint8_t *c, uint8_t *op_out) {
    size_t len;
    switch (rnd() % 8) {
        case 0: /* 完全にランダム（長さ 0..20 を含む） */
            len = rnd() % 21;
            for (size_t i = 0; i < 16; i++) c[i] = (uint8_t)rnd();
            *op_out = c[0];
            return len;
        case 1: /* 書き込み系、未対応など、ランダムなオペコード + 0 の本体 */
            memset(c, 0, 16);
            c[0] = (uint8_t)rnd();
            *op_out = c[0];
            return 6 + (rnd() % 11);
        default: /* コマンドに沿って作り、25% で壊す */
            len = clean_cdb(c, op_out);
            if (rnd() % 4 == 0) {
                uint32_t flip = rnd() & 1;
                uint32_t pos = 1 + rnd() % 9;
                uint32_t bit = rnd() % 8;
                uint8_t replacement = pick_byte();
                if (flip) c[pos] ^= (uint8_t)(1u << bit);
                else c[pos] = replacement;
            }
            if (rnd() % 16 == 0) len = len > 1 ? len - 1 - rnd() % 2 : len; /* 短い CDB */
            return len;
    }
}

static void random_media_op(void) {
    ST.media_ops++;
    switch (rnd() % 12) {
        case 0:
        case 1:
        case 2: {
            uint32_t which = rnd() % 8;
            bool force = (rnd() & 3) == 0;
            (void)iso_media_mount(&MEDIA, K_IDS[which], force);
            break;
        }
        case 3: {
            bool force = (rnd() & 3) == 0;
            (void)iso_media_eject(&MEDIA, force);
            break;
        }
        case 4: {
            bool prevent = (rnd() & 1) != 0;
            (void)iso_media_set_prevent(&MEDIA, prevent);
            break;
        }
        case 5: iso_media_bus_reset(&MEDIA); break;
        case 6: iso_media_force_eject(&MEDIA); break;
        case 7: (void)iso_media_request_eject(&MEDIA); break;
        case 8: (void)iso_media_target_eject(&MEDIA); break;
        default: break;
    }
}

static bool sense_allowed(uint8_t k, uint8_t a, uint8_t q) {
    static const uint8_t ok[][3] = {{2, 0x3A, 0}, {5, 0x24, 0}, {5, 0x20, 0}, {5, 0x21, 0}, {5, 0x39, 0}, {5, 0x53, 2},
                                    {3, 0x11, 0}, {7, 0x27, 0}, {6, 0x28, 0}, {6, 0x29, 0}};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        if (ok[i][0] == k && ok[i][1] == a && ok[i][2] == q) return true;
    }
    return false;
}

#define GUARD 64
static uint8_t ARENA[GUARD + 8192 + GUARD];

static void run_fuzz(int seeds, long iters) {
    static const size_t caps[] = {0, 1, 2, 3, 4, 7, 8, 9, 12, 18, 19, 36, 37, 64, 100, 4096};
    static uint8_t big_out[8192];
    static uint8_t rb[(size_t)4 * 2048 + 2 * GUARD];
    for (int seed = 1; seed <= seeds; seed++) {
        cur_seed = seed;
        rs = 0x2545F4914F6CDD1Dull * (uint64_t)seed + 0x9E3779B97F4A7C15ull;
        fx_init();
        for (long it = 0; it < iters; it++) {
            uint8_t cdb[16];
            uint8_t op = 0;
            size_t len;
            size_t cap;
            mmc_dir_t dir = (mmc_dir_t)(rnd() % 3);
            mmc_result_t r, shadow_r;
            iso_media_t media2;
            mmc_scsi_t scsi2;
            bool had_media;
            uint32_t exec_id = 0;
            cur_iter = it;

            if (rnd() % 6 == 0) {
                random_media_op();
            }
            /* 失敗の注入（まれに） */
            {
                uint32_t size_fail = rnd() % 40;
                uint32_t acq_fail = rnd() % 40;
                uint32_t acq_kind = rnd() & 1;
                uint32_t read_fail = rnd() % 20;
                uint32_t read_kind = rnd() & 1;
                uint32_t read_after = rnd() % 3;
                uint32_t bump = rnd() % 60;
                MOCK.size_rc = (size_fail == 0) ? ISO_ERR_IO : 0;
                MOCK.acquire_rc = (acq_fail == 0) ? (int)(acq_kind ? ISO_ERR_NOT_FOUND : ISO_ERR_EPOCH) : 0;
                MOCK.read_rc = (read_fail == 0) ? (int)(read_kind ? ISO_ERR_EPOCH : ISO_ERR_IO) : 0;
                MOCK.read_fail_after = MOCK.reads + read_after;
                MOCK.bump_epoch_on_read = (bump == 0);
            }

            len = make_cdb(cdb, &op);
            cap = caps[rnd() % (sizeof(caps) / sizeof(caps[0]))];
            had_media = iso_media_get_id(&MEDIA, &exec_id);

            /* 同じ状態から、大きな out_cap で実行する（切り詰めの性質の基準）。READ のバックエンドの参照は、すぐ放す。 */
            memcpy(&media2, &MEDIA, sizeof(media2)); /* atomic を含む構造体なので memcpy */
            memcpy(&scsi2, &SCSI, sizeof(scsi2));
            scsi2.media = &media2;
            scsi2.held = false; /* 本番が持っている参照は、本番だけが放す */
            scsi2.held_handle = NULL;
            memset(big_out, 0xA5, sizeof(big_out));
            {
                const bool bump = MOCK.bump_epoch_on_read;
                MOCK.bump_epoch_on_read = 0; /* shadow の READ は read_blocks を呼ばない */
                CHECK(mmc_scsi_execute(&scsi2, cdb, len, dir, 0, big_out, sizeof(big_out), &shadow_r) == 0, "shadow execute");
                MOCK.bump_epoch_on_read = bump;
            }
            mmc_scsi_command_done(&scsi2);

            /* 本番: 番兵つきの out */
            memset(ARENA, 0xA5, sizeof(ARENA));
            {
                uint32_t expected_len = rnd();
                CHECK(mmc_scsi_execute(&SCSI, cdb, len, dir, expected_len, ARENA + GUARD, cap, &r) == 0, "execute returned an error");
            }
            ST.executed++;

            for (size_t i = 0; i < GUARD; i++) CHECK(ARENA[i] == 0xA5, "wrote before out");
            for (size_t i = GUARD + cap; i < sizeof(ARENA); i++) CHECK(ARENA[i] == 0xA5, "wrote beyond out_cap");

            CHECK(r.status == MMC_STATUS_GOOD || r.status == MMC_STATUS_CHECK_CONDITION, "bad status");
            CHECK(r.data_len <= cap, "data_len > out_cap");
            if (r.status == MMC_STATUS_CHECK_CONDITION) {
                ST.cc++;
                CHECK(r.data_len == 0, "CC with data");
                CHECK(!r.read.is_read, "CC with read range");
                CHECK(sense_allowed(r.sense.key, r.sense.asc, r.sense.ascq), "unexpected sense");
                if (r.sense.key < 16) ST.by_sense[r.sense.key]++;
                {
                    mmc_sense_t ls;
                    CHECK(mmc_scsi_last_sense(&SCSI, &ls), "sense not kept");
                    CHECK(ls.key == r.sense.key && ls.asc == r.sense.asc && ls.ascq == r.sense.ascq, "kept sense differs");
                }
            } else {
                ST.good++;
                ST.by_op_good[op]++;
                CHECK(r.sense.key == 0 && r.sense.asc == 0 && r.sense.ascq == 0, "GOOD with sense");
                if (r.data_len) ST.data_replies++;
            }

            /* 切り詰めの性質: shadow（大きな out_cap）と比べる */
            if (shadow_r.status == MMC_STATUS_GOOD && shadow_r.data_len > 0) {
                size_t want = shadow_r.data_len < cap ? shadow_r.data_len : cap;
                CHECK(r.status == MMC_STATUS_GOOD, "status differs from the big-cap run");
                CHECK(r.data_len == want, "data_len is not min(full, out_cap)");
                CHECK(memcmp(ARENA + GUARD, big_out, want) == 0, "truncated reply is not a prefix of the full reply");
                if (cap < shadow_r.data_len) ST.truncated++;
            }
            CHECK(r.status == shadow_r.status, "status differs from the big-cap run");
            CHECK(r.sense.key == shadow_r.sense.key && r.sense.asc == shadow_r.sense.asc && r.sense.ascq == shadow_r.sense.ascq,
                  "sense differs from the big-cap run");
            CHECK(r.read.is_read == shadow_r.read.is_read && r.read.lba == shadow_r.read.lba && r.read.blocks == shadow_r.read.blocks,
                  "read range differs from the big-cap run");

            /* READ の範囲と参照 */
            if (r.read.is_read) {
                uint64_t size = 0;
                CHECK(r.status == MMC_STATUS_GOOD, "read range with CC");
                CHECK(op == 0x28 || op == 0xA8, "read range for a non-READ opcode");
                CHECK(r.data_len == 0, "READ with data_len");
                CHECK(had_media && mock_size_bytes(&MOCK, exec_id, &size) == 0, "read without media");
                CHECK((uint64_t)r.read.lba + r.read.blocks <= (size + 2047) / 2048, "read range beyond capacity");
                if (r.read.blocks == 0) ST.reads_zero_blocks++;
                else ST.reads_ok++;
            }
            CHECK(mmc_scsi_holds_reference(&SCSI) == (r.read.is_read && r.read.blocks > 0), "reference state");
            CHECK(MOCK.live == (mmc_scsi_holds_reference(&SCSI) ? 1 : 0), "backend live references");
            CHECK(MOCK.bad_use == 0, "backend used a dead handle");

            /* read_blocks（参照を持っているとき） */
            if (mmc_scsi_holds_reference(&SCSI)) {
                /* 相対のオフセットと残りのブロック数を先に求め、絶対の LBA は 64 ビットで計算する。
                 * read_blocks の LBA は 32 ビットなので、検証に使うのは、32 ビットに収まる範囲だけ */
                uint32_t rel = rnd() % r.read.blocks;
                uint64_t abs_lba;
                uint64_t remaining;
                uint64_t span;
                uint32_t sub;
                uint32_t n;
                uint32_t pick;
                if ((uint64_t)r.read.lba + rel > 0xFFFFFFFFull) rel = 0;
                abs_lba = (uint64_t)r.read.lba + rel;
                remaining = r.read.blocks - rel;
                span = remaining < 4 ? remaining : 4;
                if (abs_lba + span > 0x100000000ull) span = 0x100000000ull - abs_lba; /* 最後のブロックまで */
                pick = rnd();
                n = 1 + (uint32_t)(pick % span);
                sub = (uint32_t)abs_lba;
                size_t bytes = (size_t)n * 2048;
                uint32_t gen_at_exec = iso_media_abort_generation(&MEDIA);
                mmc_status_t st;
                if (rnd() % 20 == 0) {
                    iso_media_bus_reset(&MEDIA); /* 実行中のリセット */
                }
                memset(rb, 0xA5, sizeof(rb));
                {
                    const unsigned reads_before = MOCK.reads;
                    const bool inject = MOCK.read_rc != 0 && (MOCK.reads + 1 > MOCK.read_fail_after);
                    st = mmc_scsi_read_blocks(&SCSI, sub, n, rb + GUARD, bytes);
                    for (size_t i = 0; i < GUARD; i++) CHECK(rb[i] == 0xA5, "read_blocks wrote before buf");
                    for (size_t i = GUARD + bytes; i < sizeof(rb); i++) CHECK(rb[i] == 0xA5, "read_blocks wrote beyond buf");
                    if (iso_media_abort_generation(&MEDIA) != gen_at_exec) {
                        CHECK(st == MMC_STATUS_CHECK_CONDITION, "aborted read returned data");
                        CHECK(MOCK.reads == reads_before || MOCK.bump_epoch_on_read, "read after abort");
                        ST.read_blocks_aborted++;
                        for (size_t i = 0; i < bytes; i++) CHECK(rb[GUARD + i] == 0, "aborted read left data");
                    } else if (inject) {
                        CHECK(st == MMC_STATUS_CHECK_CONDITION, "failed backend read returned GOOD");
                        {
                            mmc_sense_t ls;
                            CHECK(mmc_scsi_last_sense(&SCSI, &ls) && ls.key == 0x3 && ls.asc == 0x11 && ls.ascq == 0x00, "not MEDIUM ERROR");
                        }
                        for (size_t i = 0; i < bytes; i++) CHECK(rb[GUARD + i] == 0, "failed read left data");
                        ST.read_blocks_err++;
                    } else {
                        uint64_t size = mock_size_of(exec_id);
                        CHECK(st == MMC_STATUS_GOOD, "read_blocks failed");
                        for (size_t i = 0; i < bytes; i++) {
                            uint64_t off = (uint64_t)sub * 2048u + i;
                            uint8_t want = off < size ? mock_pattern(exec_id, off) : 0;
                            if (rb[GUARD + i] != want) CHECK(false, "read_blocks data mismatch");
                            if (off >= size) ST.read_blocks_zero_filled++;
                        }
                        ST.read_blocks_ok++;
                    }
                }
            }
            if (rnd() % 16 != 0) { /* まれに command_done を呼び損ねる。次の execute が放す */
                mmc_scsi_command_done(&SCSI);
                CHECK(!mmc_scsi_holds_reference(&SCSI) && MOCK.live == 0, "leak after command_done");
                mmc_scsi_command_done(&SCSI); /* 冪等 */
                CHECK(MOCK.live == 0 && MOCK.bad_use == 0, "double release");
            }
        }
        mmc_scsi_command_done(&SCSI);
        TEST_ASSERT_EQUAL_INT(0, MOCK.live);
        TEST_ASSERT_EQUAL_UINT(0, MOCK.bad_use);
        TEST_ASSERT_EQUAL_UINT(MOCK.acquires, MOCK.releases);
    }
}

static void test_fuzz_random_cdbs_keep_all_invariants(void) {
    memset(&ST, 0, sizeof(ST));
    run_fuzz(12, 40000);
    printf("fuzz stats: executed=%u good=%u cc=%u data_replies=%u truncated=%u reads_ok=%u reads_zero_blocks=%u "
           "read_blocks_ok=%u zero_filled_bytes=%u read_blocks_err=%u read_blocks_aborted=%u media_ops=%u\n",
           ST.executed, ST.good, ST.cc, ST.data_replies, ST.truncated, ST.reads_ok, ST.reads_zero_blocks, ST.read_blocks_ok,
           ST.read_blocks_zero_filled, ST.read_blocks_err, ST.read_blocks_aborted, ST.media_ops);
    for (size_t i = 0; i < sizeof(K_OPS); i++) printf(" op%02X=%u", K_OPS[i], ST.by_op_good[K_OPS[i]]);
    printf("\nsense keys: 2=%u 3=%u 5=%u 6=%u 7=%u\n", ST.by_sense[2], ST.by_sense[3], ST.by_sense[5], ST.by_sense[6], ST.by_sense[7]);
    /* 空振りでないこと（固定シードなので、実測の下限） */
    TEST_ASSERT_GREATER_THAN_UINT(200000, ST.executed);
    TEST_ASSERT_GREATER_THAN_UINT(20000, ST.good);
    TEST_ASSERT_GREATER_THAN_UINT(20000, ST.cc);
    TEST_ASSERT_GREATER_THAN_UINT(5000, ST.data_replies);
    TEST_ASSERT_GREATER_THAN_UINT(2000, ST.truncated);
    TEST_ASSERT_GREATER_THAN_UINT(2000, ST.reads_ok);
    TEST_ASSERT_GREATER_THAN_UINT(200, ST.reads_zero_blocks);
    TEST_ASSERT_GREATER_THAN_UINT(1000, ST.read_blocks_ok);
    TEST_ASSERT_GREATER_THAN_UINT(100, ST.read_blocks_zero_filled);
    TEST_ASSERT_GREATER_THAN_UINT(50, ST.read_blocks_err);
    TEST_ASSERT_GREATER_THAN_UINT(50, ST.read_blocks_aborted);
    for (size_t i = 0; i < sizeof(K_OPS); i++) {
        char m[64];
        snprintf(m, sizeof(m), "opcode %02X never succeeded", K_OPS[i]);
        TEST_ASSERT_TRUE_MESSAGE(ST.by_op_good[K_OPS[i]] > 100, m);
    }
    TEST_ASSERT_GREATER_THAN_UINT(100, ST.by_sense[2]);
    TEST_ASSERT_GREATER_THAN_UINT(40, ST.by_sense[3]);
    TEST_ASSERT_GREATER_THAN_UINT(1000, ST.by_sense[5]);
    TEST_ASSERT_GREATER_THAN_UINT(100, ST.by_sense[6]);
    TEST_ASSERT_GREATER_THAN_UINT(100, ST.by_sense[7]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_random_cdbs_keep_all_invariants);
    return UNITY_END();
}
