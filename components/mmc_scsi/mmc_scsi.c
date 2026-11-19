#include "mmc_scsi.h"

#include <string.h>

/*
 * 出典と未確認の項目は docs/MMC_SCSI_TESTS.md 1 章。確認できなかった値には TODO(device-check) を付けてある。
 * 数値の欄の名前は、MMC-6 / SPC-4 の欄の名前に合わせた。
 */

/* ---- オペコード ---- */
#define OP_TEST_UNIT_READY 0x00
#define OP_WRITE_6 0x0A
#define OP_REQUEST_SENSE 0x03
#define OP_FORMAT_UNIT 0x04
#define OP_INQUIRY 0x12
#define OP_MODE_SENSE_6 0x1A
#define OP_START_STOP_UNIT 0x1B
#define OP_PREVENT_ALLOW 0x1E
#define OP_READ_FORMAT_CAPACITIES 0x23
#define OP_READ_CAPACITY_10 0x25
#define OP_READ_10 0x28
#define OP_WRITE_10 0x2A
#define OP_WRITE_VERIFY_10 0x2E
#define OP_WRITE_SAME_10 0x41
#define OP_READ_TOC 0x43
#define OP_GET_CONFIGURATION 0x46
#define OP_GET_EVENT_STATUS 0x4A
#define OP_READ_DISC_INFO 0x51
#define OP_READ_TRACK_INFO 0x52
#define OP_MODE_SENSE_10 0x5A
#define OP_WRITE_16 0x8A
#define OP_WRITE_VERIFY_16 0x8E
#define OP_WRITE_SAME_16 0x93
#define OP_BLANK 0xA1
#define OP_WRITE_12 0xAA
#define OP_WRITE_VERIFY_12 0xAE
#define OP_READ_12 0xA8

/* ---- 応答の定数 ---- */
#define PROFILE_DVD_ROM 0x0010u
#define FEATURE_PROFILE_LIST 0x0000u
#define FEATURE_CORE 0x0001u
#define FEATURE_REMOVABLE_MEDIUM 0x0003u
#define FEATURE_RANDOM_READABLE 0x0010u

#define MSF_LBA_OFFSET 150u          /* MSF = LBA + 150（2 秒のリードイン） */
#define MSF_MAX_LBA_PLUS_150 1151999u /* 255:59:74。TODO(device-check): 大きなイメージの丸め規則（docs/MMC_SCSI_TESTS.md） */

/* ---- 小さな部品 ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(((unsigned)p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

typedef struct {
    mmc_scsi_t *s;
    const uint8_t *cdb;
    uint8_t *out;
    size_t out_cap;
    mmc_result_t *r;
} env_t;

static void fail(mmc_result_t *r, uint8_t key, uint8_t asc, uint8_t ascq) {
    memset(r, 0, sizeof(*r));
    r->status = MMC_STATUS_CHECK_CONDITION;
    r->sense.key = key;
    r->sense.asc = asc;
    r->sense.ascq = ascq;
}

static void invalid_field(env_t *e) { fail(e->r, MMC_SENSE_ILLEGAL_REQUEST, 0x24, 0x00); }          /* INVALID FIELD IN CDB */
static void no_medium(env_t *e) { fail(e->r, MMC_SENSE_NOT_READY, 0x3A, 0x00); }                    /* MEDIUM NOT PRESENT */
static void medium_error(env_t *e) { fail(e->r, MMC_SENSE_MEDIUM_ERROR, 0x11, 0x00); }              /* UNRECOVERED READ ERROR */

static bool zero_bytes(const uint8_t *cdb, size_t from, size_t to) {
    for (size_t i = from; i <= to; i++) {
        if (cdb[i] != 0) return false;
    }
    return true;
}

/* CONTROL バイト: ビット 7-6 はベンダー固有（無視）。NACA、FLAG、LINK と予約ビットは 0 であること。 */
static bool control_ok(const uint8_t *cdb, size_t idx) { return (cdb[idx] & 0x3F) == 0; }

/* 割り当て長で切り詰め、さらに out_cap で切り詰めて out に書く。書いたバイト数を返す。 */
static size_t reply(env_t *e, const uint8_t *buf, size_t full_len, size_t alloc_len) {
    size_t n = full_len;
    if (alloc_len < n) n = alloc_len;
    if (e->out_cap < n) n = e->out_cap;
    if (n > 0 && e->out == NULL) n = 0;
    if (n > 0) memcpy(e->out, buf, n);
    e->r->data_len = (uint32_t)n;
    return n;
}

static uint64_t capacity_blocks(uint64_t size_bytes) { return (size_bytes + (MMC_SCSI_BLOCK_SIZE - 1u)) / MMC_SCSI_BLOCK_SIZE; }

/* メディアがあれば真で、その実バイト数を返す（失敗にはしない）。 */
static bool query_medium(const mmc_scsi_t *s, uint64_t *size_bytes) {
    uint32_t id = 0;
    uint64_t size = 0;
    if (!iso_media_get_id(s->media, &id) || s->backend.size_bytes(s->backend.ctx, id, &size) != 0 || size == 0) return false;
    *size_bytes = size;
    return true;
}

/* メディアがあれば真。なければ NOT READY（メディアなし）にして偽。 */
static bool need_medium(env_t *e, uint64_t *size_bytes) {
    if (query_medium(e->s, size_bytes)) return true;
    no_medium(e);
    return false;
}

/* 32 ビットに収める（READ CAPACITY(10) などの欄）。 */
static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v; }

/* ---- TEST UNIT READY（SPC-4） ---- */
static void cmd_test_unit_ready(env_t *e) {
    uint64_t size;
    if (!zero_bytes(e->cdb, 1, 4) || !control_ok(e->cdb, 5)) { invalid_field(e); return; }
    (void)need_medium(e, &size);
}

/* ---- REQUEST SENSE（SPC-4）。固定形式だけ（DESC = 0） ---- */
void mmc_scsi_build_sense(const mmc_sense_t *sense, uint8_t out[MMC_SCSI_SENSE_LEN]) {
    memset(out, 0, MMC_SCSI_SENSE_LEN);
    out[0] = 0x70; /* 現在のエラー、固定形式 */
    out[2] = sense->key & 0x0F;
    out[7] = 10; /* 追加のセンスの長さ */
    out[12] = sense->asc;
    out[13] = sense->ascq;
}

static void cmd_request_sense(env_t *e) {
    uint8_t buf[MMC_SCSI_SENSE_LEN];
    mmc_sense_t none = {MMC_SENSE_NO_SENSE, 0, 0};
    /* DESC ビット（記述子形式）は未対応: 要求されたら INVALID FIELD IN CDB */
    if (e->cdb[1] != 0 || !zero_bytes(e->cdb, 2, 3) || !control_ok(e->cdb, 5)) { invalid_field(e); return; }
    mmc_scsi_build_sense(e->s->sense_valid ? &e->s->sense : &none, buf);
    if (e->cdb[4] > 0) e->s->sense_valid = false; /* 返したら消える */
    (void)reply(e, buf, sizeof(buf), e->cdb[4]);
}

/* ---- INQUIRY（SPC-4）。標準の応答 36 バイト ---- */
static const uint8_t k_inquiry[36] = {
    0x05,                                   /* PERIPHERAL QUALIFIER 0、デバイス種別 5（CD/DVD） */
    0x80,                                   /* RMB = 1 */
    0x05,                                   /* VERSION: SPC-3 */
    0x02,                                   /* RESPONSE DATA FORMAT = 2 */
    31,                                     /* ADDITIONAL LENGTH = 36 - 5 */
    0x00, 0x00, 0x00,                       /* SCCS 等 */
    'Q', 'U', 'A', 'D', 'K', 'V', 'M', ' ', /* T10 VENDOR IDENTIFICATION（8） */
    'V', 'i', 'r', 't', 'u', 'a', 'l', ' ', 'C', 'D', '-', 'R', 'O', 'M', ' ', ' ', /* PRODUCT（16） */
    '0', '0', '0', '1'                      /* REVISION（4） */
};

static void cmd_inquiry(env_t *e) {
    const uint8_t *c = e->cdb;
    if ((c[1] & 0xFE) != 0 || !control_ok(c, 5)) { invalid_field(e); return; } /* EVPD 以外（CMDDT を含む）は予約 */
    if ((c[1] & 0x01) != 0 || c[2] != 0) { invalid_field(e); return; }          /* VPD ページは未対応 */
    (void)reply(e, k_inquiry, sizeof(k_inquiry), rd16(c + 3));
}

/* ---- MODE SENSE(6) / (10)（SPC-4。MMC-6 のページ 2Ah） ---- */
#define MODE_PAGE_CAPS 0x2A
#define MODE_PAGE_ALL 0x3F
#define CAPS_PAGE_LEN 22 /* ページ 2Ah 全体。ページ長の欄 = 0x14（MMC-3 の長さ）。TODO(device-check) */

static size_t build_caps_page(const env_t *e, uint8_t pc, uint8_t *p) {
    memset(p, 0, CAPS_PAGE_LEN);
    p[0] = MODE_PAGE_CAPS;
    p[1] = CAPS_PAGE_LEN - 2;
    if (pc == 1) return CAPS_PAGE_LEN; /* 変更可能な値のマスク: 変えられるものはない */
    p[2] = 0x08; /* DVD-ROM を読める。TODO(device-check) */
    /* p[6]: ローディング機構 = トレイ（1 << 5）、イジェクト可（0x08）、ロック可（0x01）、ロックの状態（0x02） */
    p[6] = (uint8_t)(0x20 | 0x08 | 0x01 | (iso_media_prevent_removal(e->s->media) ? 0x02 : 0x00));
    return CAPS_PAGE_LEN;
}

static void mode_sense(env_t *e, bool ten) {
    const uint8_t *c = e->cdb;
    uint8_t buf[8 + CAPS_PAGE_LEN];
    size_t hdr = ten ? 8u : 4u;
    uint8_t pc = (uint8_t)(c[2] >> 6);
    uint8_t page = c[2] & 0x3F;
    uint8_t sub = c[3];
    size_t alloc;
    size_t total;
    if (ten) {
        if ((c[1] & ~0x18) != 0 || !zero_bytes(c, 4, 6) || !control_ok(c, 9)) { invalid_field(e); return; }
        alloc = rd16(c + 7);
    } else {
        if ((c[1] & ~0x08) != 0 || !control_ok(c, 5)) { invalid_field(e); return; }
        alloc = c[4];
    }
    if (pc == 3) { fail(e->r, MMC_SENSE_ILLEGAL_REQUEST, 0x39, 0x00); return; } /* SAVING PARAMETERS NOT SUPPORTED */
    if (page != MODE_PAGE_CAPS && page != MODE_PAGE_ALL) { invalid_field(e); return; }
    if (page == MODE_PAGE_ALL ? (sub != 0x00 && sub != 0xFF) : sub != 0x00) { invalid_field(e); return; }
    memset(buf, 0, hdr);
    total = hdr + build_caps_page(e, pc, buf + hdr);
    if (ten) {
        wr16(buf, (uint32_t)(total - 2)); /* MODE DATA LENGTH */
    } else {
        buf[0] = (uint8_t)(total - 1);
    }
    /* MEDIUM TYPE = 0（既定）、DEVICE-SPECIFIC PARAMETER = 0、ブロック記述子なし */
    (void)reply(e, buf, total, alloc);
}

/* ---- START STOP UNIT（MMC-6） ---- */
static void cmd_start_stop_unit(env_t *e) {
    const uint8_t *c = e->cdb;
    bool loej = (c[4] & 0x02) != 0;
    bool start = (c[4] & 0x01) != 0;
    /* byte1: IMMED（ビット 0）だけ。byte2、byte3: 予約（power condition modifier は未対応）。byte4: POWER CONDITION = 0 */
    if ((c[1] & 0xFE) != 0 || c[2] != 0 || c[3] != 0 || (c[4] & 0xFC) != 0 || !control_ok(c, 5)) { invalid_field(e); return; }
    if (loej && !start) {
        /* イジェクト */
        if (iso_media_target_eject(e->s->media) == ISO_MEDIA_LOCKED) {
            fail(e->r, MMC_SENSE_ILLEGAL_REQUEST, 0x53, 0x02); /* MEDIA REMOVAL PREVENTED */
        }
    }
    /* ロード・スタート・ストップは何もしない（GOOD）。TODO(device-check): メディアなしの Start = 1 */
}

/* ---- PREVENT ALLOW MEDIUM REMOVAL（SPC-4 / MMC-6） ---- */
static void cmd_prevent_allow(env_t *e) {
    const uint8_t *c = e->cdb;
    if (!zero_bytes(c, 1, 3) || (c[4] & 0xFE) != 0 || !control_ok(c, 5)) { invalid_field(e); return; } /* PERSISTENT（bit 1）は未対応 */
    if (!iso_media_set_prevent(e->s->media, (c[4] & 0x01) != 0)) no_medium(e); /* TODO(device-check): メディアなしの PREVENT */
}

/* ---- READ CAPACITY(10)（SBC-3 / MMC-6） ---- */
static void cmd_read_capacity_10(env_t *e) {
    const uint8_t *c = e->cdb;
    uint8_t buf[8];
    uint64_t size;
    uint64_t cap;
    bool pmi = (c[8] & 0x01) != 0;
    if (c[1] != 0 || !zero_bytes(c, 6, 7) || (c[8] & 0xFE) != 0 || !control_ok(c, 9)) { invalid_field(e); return; }
    if (!pmi && !zero_bytes(c, 2, 5)) { invalid_field(e); return; } /* PMI = 0 のとき、LOGICAL BLOCK ADDRESS は 0 */
    if (!need_medium(e, &size)) return;
    cap = capacity_blocks(size);
    if (pmi && rd32(c + 2) >= cap) { invalid_field(e); return; }
    wr32(buf, clamp32(cap - 1u)); /* RETURNED LOGICAL BLOCK ADDRESS = 最終 LBA（4 GiB 超は 0xFFFFFFFF） */
    wr32(buf + 4, MMC_SCSI_BLOCK_SIZE);
    (void)reply(e, buf, sizeof(buf), sizeof(buf));
}

/* ---- READ FORMAT CAPACITIES（MMC-6） ---- */
static void cmd_read_format_capacities(env_t *e) {
    const uint8_t *c = e->cdb;
    uint8_t buf[12];
    uint64_t size;
    if (c[1] != 0 || !zero_bytes(c, 2, 6) || !control_ok(c, 9)) { invalid_field(e); return; }
    memset(buf, 0, sizeof(buf));
    buf[3] = 8; /* CAPACITY LIST LENGTH: 記述子 1 つ */
    if (query_medium(e->s, &size)) {
        wr32(buf + 4, clamp32(capacity_blocks(size)));
        buf[8] = 0x02; /* DESCRIPTOR TYPE: 現在のメディアの大きさ（フォーマット済み） */
    } else { /* メディアなしは失敗にしない（最大容量の記述子） */
        wr32(buf + 4, 0xFFFFFFFFu);
        buf[8] = 0x03; /* メディアなし。TODO(device-check) */
    }
    buf[9] = 0x00;
    buf[10] = 0x08;
    buf[11] = 0x00; /* BLOCK LENGTH = 2048 */
    (void)reply(e, buf, sizeof(buf), rd16(c + 7));
}

/* ---- READ(10) / READ(12)（SBC-3 / MMC-6） ---- */
static void cmd_read(env_t *e, bool is12) {
    const uint8_t *c = e->cdb;
    mmc_scsi_t *s = e->s;
    uint64_t size;
    uint64_t cap;
    uint32_t lba;
    uint32_t blocks;
    uint32_t id = 0;
    void *handle = NULL;
    int rc;
    /* byte1: RDPROTECT（7-5）= 0、予約（2、0）= 0。DPO、FUA、FUA_NV は受け付けて無視（読み取り専用）。
     * READ(10) byte6、READ(12) byte10: GROUP NUMBER（4-0）。ストリーミング（READ(12) の bit 7）は未対応 */
    if ((c[1] & 0xE5) != 0) { invalid_field(e); return; }
    if (is12) {
        if ((c[10] & 0xE0) != 0 || !control_ok(c, 11)) { invalid_field(e); return; }
        blocks = rd32(c + 6);
    } else {
        if ((c[6] & 0xE0) != 0 || !control_ok(c, 9)) { invalid_field(e); return; }
        blocks = rd16(c + 7);
    }
    lba = rd32(c + 2);
    if (!need_medium(e, &size)) return;
    cap = capacity_blocks(size);
    if ((uint64_t)lba + (uint64_t)blocks > cap) { fail(e->r, MMC_SENSE_ILLEGAL_REQUEST, 0x21, 0x00); return; } /* LBA OUT OF RANGE */
    e->r->read.is_read = true;
    e->r->read.lba = lba;
    e->r->read.blocks = blocks;
    if (blocks == 0) return; /* 転送なし。エラーではない */
    (void)iso_media_get_id(s->media, &id);
    rc = s->backend.acquire(s->backend.ctx, id, &handle);
    if (rc != 0) {
        if (rc == ISO_ERR_NOT_FOUND || rc == ISO_ERR_NO_CARD) { no_medium(e); return; }
        { medium_error(e); return; }
    }
    /* 参照を取った後は、失敗しない（取った参照は command_done が放す） */
    s->held = true;
    s->held_handle = handle;
    s->held_size_bytes = size;
    s->held_abort_generation = iso_media_abort_generation(s->media);
}

/* ---- READ TOC/PMA/ATIP（MMC-6）。形式 0 / 1 / 2 ---- */
static void msf_of(uint64_t lba, uint8_t out[3]) {
    uint64_t v = lba + MSF_LBA_OFFSET;
    if (v > MSF_MAX_LBA_PLUS_150) v = MSF_MAX_LBA_PLUS_150; /* TODO(device-check) */
    out[0] = (uint8_t)(v / (60u * 75u));
    out[1] = (uint8_t)((v / 75u) % 60u);
    out[2] = (uint8_t)(v % 75u);
}

static void toc_track_desc(uint8_t *p, uint8_t track, uint64_t lba, bool msf) {
    p[0] = 0x00;
    p[1] = 0x14; /* ADR = 1、CONTROL = 4（データトラック、複製禁止） */
    p[2] = track;
    p[3] = 0x00;
    if (msf) {
        p[4] = 0x00;
        msf_of(lba, p + 5);
    } else {
        wr32(p + 4, clamp32(lba));
    }
}

/* 形式 2（フル TOC）の記述子 11 バイト。時刻は MSF（形式 2 は MSF ビットによらず MSF）。 */
static void toc_full_desc(uint8_t *p, uint8_t point, uint8_t pmin, uint8_t psec, uint8_t pframe) {
    p[0] = 1;    /* SESSION NUMBER */
    p[1] = 0x14; /* ADR = 1、CONTROL = 4 */
    p[2] = 0;    /* TNO */
    p[3] = point;
    p[4] = p[5] = p[6] = 0; /* MIN、SEC、FRAME */
    p[7] = 0;               /* ZERO */
    p[8] = pmin;
    p[9] = psec;
    p[10] = pframe;
}

static void cmd_read_toc(env_t *e) {
    const uint8_t *c = e->cdb;
    uint8_t buf[4 + 4 * 11];
    uint64_t size;
    uint64_t leadout;
    size_t total;
    uint8_t fmt = c[2] & 0x0F;
    bool msf = (c[1] & 0x02) != 0;
    if ((c[1] & ~0x02) != 0 || (c[2] & 0xF0) != 0 || !zero_bytes(c, 3, 5) || (c[9] & 0x3F) != 0) { invalid_field(e); return; }
    if (fmt == 0) fmt = (uint8_t)(c[9] >> 6); /* SFF-8020i 互換: 形式は byte 9 のビット 7-6 */
    if (fmt > 2) { invalid_field(e); return; }      /* 3 = PMA、4 = ATIP は記録型のメディアだけ。5 = CD-TEXT は未対応 */
    if (!need_medium(e, &size)) return;
    leadout = capacity_blocks(size);
    memset(buf, 0, sizeof(buf));
    buf[2] = 1; /* 最初のトラック（形式 1、2 では最初のセッション） */
    buf[3] = 1; /* 最後のトラック（形式 1、2 では最後のセッション） */
    if (fmt == 0) {
        uint8_t track = c[6];
        if (track > 1 && track != 0xAA) { invalid_field(e); return; }
        total = 4;
        if (track != 0xAA) {
            toc_track_desc(buf + total, 1, 0, msf);
            total += 8;
        }
        toc_track_desc(buf + total, 0xAA, leadout, msf);
        total += 8;
    } else if (fmt == 1) {
        total = 12;
        toc_track_desc(buf + 4, 1, 0, msf); /* 最後のセッションの最初のトラック */
    } else {
        uint8_t m[3];
        if (c[6] > 1) { invalid_field(e); return; } /* セッション番号 */
        total = 4;
        toc_full_desc(buf + total, 0xA0, 1, 0x00, 0); /* 最初のトラック = 1、ディスクの種類 = 0（CD-DA / CD-ROM） */
        total += 11;
        toc_full_desc(buf + total, 0xA1, 1, 0, 0); /* 最後のトラック = 1 */
        total += 11;
        msf_of(leadout, m);
        toc_full_desc(buf + total, 0xA2, m[0], m[1], m[2]); /* リードアウト */
        total += 11;
        msf_of(0, m);
        toc_full_desc(buf + total, 0x01, m[0], m[1], m[2]); /* トラック 1 */
        total += 11;
    }
    wr16(buf, (uint32_t)(total - 2)); /* TOC DATA LENGTH（この欄の 2 バイトを除く） */
    (void)reply(e, buf, total, rd16(c + 7));
}

/* ---- GET CONFIGURATION（MMC-6） ---- */
static size_t put_feature(uint8_t *p, uint16_t code, uint8_t flags, uint8_t add_len) {
    wr16(p, code);
    p[2] = flags; /* VERSION（5-2）、PERSISTENT（1）、CURRENT（0） */
    p[3] = add_len;
    return 4u;
}

static void cmd_get_configuration(env_t *e) {
    const uint8_t *c = e->cdb;
    uint8_t buf[64];
    size_t n = 8;
    uint8_t rt = c[1] & 0x03;
    uint16_t start = rd16(c + 2);
    uint64_t size_unused;
    bool media;
    if ((c[1] & 0xFC) != 0 || !zero_bytes(c, 4, 6) || !control_ok(c, 9)) { invalid_field(e); return; }
    if (rt == 3) { invalid_field(e); return; }
    media = query_medium(e->s, &size_unused);
    memset(buf, 0, sizeof(buf));
    wr16(buf + 6, media ? PROFILE_DVD_ROM : 0); /* CURRENT PROFILE */
    /* フィーチャーは、コードの昇順。RT = 0: start 以上のすべて、RT = 1: start 以上の current のもの、RT = 2: start だけ */
#define WANT(code, current)                                                              \
    ((rt == 2) ? (start == (code)) : ((start <= (code)) && (rt == 0 || (current))))
    if (WANT(FEATURE_PROFILE_LIST, true)) {
        n += put_feature(buf + n, FEATURE_PROFILE_LIST, 0x03, 4);
        wr16(buf + n, PROFILE_DVD_ROM);
        buf[n + 2] = media ? 0x01 : 0x00; /* CURRENT P */
        n += 4;
    }
    if (WANT(FEATURE_CORE, true)) {
        n += put_feature(buf + n, FEATURE_CORE, 0x0B, 8); /* バージョン 2、永続、現在。TODO(device-check) */
        wr32(buf + n, 8);                                 /* PHYSICAL INTERFACE STANDARD = USB。TODO(device-check) */
        n += 8;                                           /* DBE = 0（残りは 0） */
    }
    if (WANT(FEATURE_REMOVABLE_MEDIUM, true)) {
        n += put_feature(buf + n, FEATURE_REMOVABLE_MEDIUM, 0x0B, 4); /* TODO(device-check): バージョン */
        buf[n] = 0x29; /* ローディング機構 = トレイ、イジェクト可、ロック可 */
        n += 4;
    }
    if (WANT(FEATURE_RANDOM_READABLE, media)) {
        n += put_feature(buf + n, FEATURE_RANDOM_READABLE, media ? 0x01 : 0x00, 8);
        wr32(buf + n, MMC_SCSI_BLOCK_SIZE); /* LOGICAL BLOCK SIZE */
        wr16(buf + n + 4, 16);              /* BLOCKING（DVD の ECC ブロック）。TODO(device-check) */
        n += 8;                             /* PP = 0 */
    }
#undef WANT
    wr32(buf, (uint32_t)(n - 4)); /* DATA LENGTH（この欄の 4 バイトを除く） */
    (void)reply(e, buf, n, rd16(c + 7));
}

/* ---- GET EVENT STATUS NOTIFICATION（MMC-6）。ポーリング形式、メディアクラス ---- */
static void cmd_get_event_status(env_t *e) {
    const uint8_t *c = e->cdb;
    uint8_t buf[8];
    size_t n;
    size_t alloc = rd16(c + 7);
    bool media_class = (c[4] & 0x10) != 0;
    if ((c[1] & 0xFE) != 0 || (c[1] & 0x01) == 0 || !zero_bytes(c, 2, 3) || (c[4] & 0x81) != 0 || !zero_bytes(c, 5, 6) ||
        !control_ok(c, 9)) {
        { invalid_field(e); return; } /* ポーリング（POLLED = 1）だけ。非同期は未対応 */
    }
    memset(buf, 0, sizeof(buf));
    buf[3] = 0x10; /* SUPPORTED EVENT CLASSES: メディア */
    if (!media_class) {
        buf[2] = 0x80; /* NEA（要求されたクラスに、報告できるものがない）。EVENT DESCRIPTOR LENGTH = 0 */
        n = 4;
    } else {
        iso_media_event_t ev = iso_media_peek_event(e->s->media);
        wr16(buf, 4); /* EVENT DESCRIPTOR LENGTH。TODO(device-check): 定義（4 か 6 か） */
        buf[2] = 0x04; /* 通知クラス = メディア */
        switch (ev) {
            case ISO_MEDIA_EVENT_EJECT_REQUEST: buf[4] = 0x01; break;
            case ISO_MEDIA_EVENT_NEW_MEDIA: buf[4] = 0x02; break;
            case ISO_MEDIA_EVENT_MEDIA_REMOVAL: buf[4] = 0x03; break;
            default: buf[4] = 0x00; break; /* NO CHANGE */
        }
        buf[5] = iso_media_is_loaded(e->s->media) ? 0x02 : 0x00; /* MEDIA STATUS: メディアあり（bit 1）、トレイは閉じている */
        n = 8;
        /* 記述子の全体を返せたときだけ、イベントを消費する */
        if (reply(e, buf, n, alloc) >= 8 && ev != ISO_MEDIA_EVENT_NONE) (void)iso_media_take_event(e->s->media);
        return;
    }
    (void)reply(e, buf, n, alloc);
}

/* ---- READ DISC INFORMATION（MMC-6）。標準のディスク情報 ---- */
static void cmd_read_disc_info(env_t *e) {
    const uint8_t *c = e->cdb;
    uint8_t buf[34];
    uint64_t size;
    if ((c[1] & 0xF8) != 0 || !zero_bytes(c, 2, 6) || !control_ok(c, 9)) { invalid_field(e); return; }
    if ((c[1] & 0x07) != 0) { invalid_field(e); return; } /* DATA TYPE: 標準のディスク情報だけ */
    if (!need_medium(e, &size)) return;
    memset(buf, 0, sizeof(buf));
    wr16(buf, 32);
    buf[2] = 0x0E; /* DISC STATUS = 完結、最後のセッションの状態 = 完結 */
    buf[3] = 1;    /* 最初のトラック番号 */
    buf[4] = 1;    /* セッション数 */
    buf[5] = 1;    /* 最後のセッションの最初のトラック */
    buf[6] = 1;    /* 最後のセッションの最後のトラック */
    buf[7] = 0x20; /* URU（無制限の使用） */
    buf[8] = 0x00; /* DISC TYPE: CD-DA / CD-ROM / DVD-ROM。TODO(device-check): DVD-ROM での扱い */
    (void)reply(e, buf, sizeof(buf), rd16(c + 7));
}

/* ---- READ TRACK INFORMATION（MMC-6）。MMC-3 の 36 バイトの形 ---- */
static void cmd_read_track_info(env_t *e) {
    const uint8_t *c = e->cdb;
    uint8_t buf[36];
    uint64_t size;
    uint64_t cap;
    uint32_t value = rd32(c + 2);
    uint8_t type = c[1] & 0x03;
    if ((c[1] & 0xF8) != 0 || c[6] != 0 || !control_ok(c, 9)) { invalid_field(e); return; }
    if (!need_medium(e, &size)) return;
    cap = capacity_blocks(size);
    if (type == 0 ? (value >= cap) : (type == 3 || value != 1)) { invalid_field(e); return; } /* 0 = LBA、1 = トラック、2 = セッション */
    memset(buf, 0, sizeof(buf));
    wr16(buf, 34);
    buf[2] = 1;    /* トラック番号 */
    buf[3] = 1;    /* セッション番号 */
    buf[5] = 0x04; /* TRACK MODE = 4（データ、中断なし） */
    buf[6] = 0x01; /* DATA MODE = 1 */
    wr32(buf + 24, clamp32(cap)); /* TRACK SIZE。TODO(device-check): 長さ 36 か 48 か */
    (void)reply(e, buf, sizeof(buf), rd16(c + 7));
}

/* ---- 書き込み系と未対応 ---- */
static bool is_write_opcode(uint8_t op) {
    switch (op) {
        case OP_WRITE_6:
        case OP_WRITE_10:
        case OP_WRITE_12:
        case OP_WRITE_16:
        case OP_WRITE_VERIFY_10:
        case OP_WRITE_VERIFY_12:
        case OP_WRITE_VERIFY_16:
        case OP_WRITE_SAME_10:
        case OP_WRITE_SAME_16:
        case OP_FORMAT_UNIT:
        case OP_BLANK:
            return true;
        default:
            return false;
    }
}

/* オペコードのグループから、CDB の長さ（SPC-4 の CDB の形式）。グループ 3、6、7（予約・ベンダー固有）は 0。 */
static size_t cdb_length_of(uint8_t op) {
    switch (op >> 5) {
        case 0: return 6;
        case 1:
        case 2: return 10;
        case 4: return 16;
        case 5: return 12;
        default: return 0;
    }
}

static void dispatch(env_t *e, size_t cdb_len) {
    uint8_t op = e->cdb[0];
    size_t need = cdb_length_of(op);
    if (is_write_opcode(op)) { /* データを受け取らず失敗にする */
        fail(e->r, MMC_SENSE_DATA_PROTECT, 0x27, 0x00); /* WRITE PROTECTED */
        return;
    }
    switch (op) {
        case OP_TEST_UNIT_READY:
        case OP_REQUEST_SENSE:
        case OP_INQUIRY:
        case OP_MODE_SENSE_6:
        case OP_START_STOP_UNIT:
        case OP_PREVENT_ALLOW:
        case OP_READ_FORMAT_CAPACITIES:
        case OP_READ_CAPACITY_10:
        case OP_READ_10:
        case OP_READ_TOC:
        case OP_GET_CONFIGURATION:
        case OP_GET_EVENT_STATUS:
        case OP_READ_DISC_INFO:
        case OP_READ_TRACK_INFO:
        case OP_MODE_SENSE_10:
        case OP_READ_12:
            break;
        default:
            fail(e->r, MMC_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); /* INVALID COMMAND OPERATION CODE（MODE SELECT を含む） */
            return;
    }
    if (cdb_len < need) { invalid_field(e); return; }
    switch (op) {
        case OP_TEST_UNIT_READY: cmd_test_unit_ready(e); break;
        case OP_REQUEST_SENSE: cmd_request_sense(e); break;
        case OP_INQUIRY: cmd_inquiry(e); break;
        case OP_MODE_SENSE_6: mode_sense(e, false); break;
        case OP_MODE_SENSE_10: mode_sense(e, true); break;
        case OP_START_STOP_UNIT: cmd_start_stop_unit(e); break;
        case OP_PREVENT_ALLOW: cmd_prevent_allow(e); break;
        case OP_READ_FORMAT_CAPACITIES: cmd_read_format_capacities(e); break;
        case OP_READ_CAPACITY_10: cmd_read_capacity_10(e); break;
        case OP_READ_10: cmd_read(e, false); break;
        case OP_READ_12: cmd_read(e, true); break;
        case OP_READ_TOC: cmd_read_toc(e); break;
        case OP_GET_CONFIGURATION: cmd_get_configuration(e); break;
        case OP_GET_EVENT_STATUS: cmd_get_event_status(e); break;
        case OP_READ_DISC_INFO: cmd_read_disc_info(e); break;
        default: cmd_read_track_info(e); break; /* OP_READ_TRACK_INFO */
    }
}

/* ---- 公開 API ---- */
int mmc_scsi_init(mmc_scsi_t *s, iso_media_t *media, const mmc_scsi_backend_t *backend) {
    if (!s || !media || !backend || !backend->size_bytes || !backend->acquire || !backend->read || !backend->release) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->media = media;
    s->backend = *backend;
    s->seen_abort_generation = iso_media_abort_generation(media);
    return 0;
}

void mmc_scsi_command_done(mmc_scsi_t *s) {
    void *handle;
    if (!s || !s->held) return;
    handle = s->held_handle;
    s->held = false;
    s->held_handle = NULL;
    s->backend.release(s->backend.ctx, handle);
}

bool mmc_scsi_holds_reference(const mmc_scsi_t *s) { return s && s->held; }

bool mmc_scsi_last_sense(const mmc_scsi_t *s, mmc_sense_t *out) {
    if (!s || !s->sense_valid) return false;
    if (out) *out = s->sense;
    return true;
}

static void remember_sense(mmc_scsi_t *s, uint8_t key, uint8_t asc, uint8_t ascq) {
    s->sense_valid = true;
    s->sense.key = key;
    s->sense.asc = asc;
    s->sense.ascq = ascq;
}

int mmc_scsi_execute(mmc_scsi_t *s, const uint8_t *cdb, size_t cdb_len, mmc_dir_t dir, uint32_t expected_len,
                     uint8_t *out, size_t out_cap, mmc_result_t *result) {
    env_t e;
    uint8_t op;
    bool exempt;
    uint8_t ua_asc = 0;
    uint8_t ua_ascq = 0;
    (void)dir;          /* CBW の 13 のケースは msc_bot が処理する */
    (void)expected_len;
    if (!s || !cdb || !result) return -1;
    memset(result, 0, sizeof(*result));
    result->status = MMC_STATUS_GOOD;
    mmc_scsi_command_done(s); /* 前のコマンドが command_done を呼び損ねたときの、参照のリークの防止 */

    e.s = s;
    e.cdb = cdb;
    e.out = out;
    e.out_cap = out_cap;
    e.r = result;

    if (iso_media_abort_generation(s->media) != s->seen_abort_generation) { /* バスリセット、force_eject */
        s->seen_abort_generation = iso_media_abort_generation(s->media);
        s->sense_valid = false;
    }

    if (cdb_len == 0 || cdb_len > MMC_SCSI_CDB_MAX) {
        invalid_field(&e);
    } else {
        op = cdb[0];
        exempt = (op == OP_INQUIRY || op == OP_REQUEST_SENSE);
        if (!exempt) s->sense_valid = false; /* 新しいコマンドで、前のセンスは消える（SPC-4 のセンスデータの保持の規則） */
        if (!exempt && iso_media_take_unit_attention(s->media, &ua_asc, &ua_ascq)) {
            fail(result, MMC_SENSE_UNIT_ATTENTION, ua_asc, ua_ascq);
        } else {
            dispatch(&e, cdb_len);
        }
    }
    if (result->status == MMC_STATUS_CHECK_CONDITION) {
        remember_sense(s, result->sense.key, result->sense.asc, result->sense.ascq);
    }
    return 0;
}

mmc_status_t mmc_scsi_read_blocks(mmc_scsi_t *s, uint32_t lba, uint32_t blocks, uint8_t *buf, size_t cap) {
    uint64_t bytes;
    uint64_t start;
    int rc;
    if (!s) return MMC_STATUS_CHECK_CONDITION;
    if (!s->held) { /* execute が参照を取っていない */
        remember_sense(s, MMC_SENSE_NOT_READY, 0x3A, 0x00);
        return MMC_STATUS_CHECK_CONDITION;
    }
    bytes = (uint64_t)blocks * MMC_SCSI_BLOCK_SIZE;
    if ((uint64_t)lba + (uint64_t)blocks > capacity_blocks(s->held_size_bytes)) {
        remember_sense(s, MMC_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
        return MMC_STATUS_CHECK_CONDITION;
    }
    if (bytes > cap || (bytes > 0 && !buf)) {
        remember_sense(s, MMC_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
        return MMC_STATUS_CHECK_CONDITION;
    }
    if (blocks == 0) return MMC_STATUS_GOOD;
    if (iso_media_abort_generation(s->media) != s->held_abort_generation) {
        memset(buf, 0, (size_t)bytes);
        remember_sense(s, MMC_SENSE_ABORTED_COMMAND, 0x00, 0x00);
        return MMC_STATUS_CHECK_CONDITION;
    }
    rc = s->backend.read(s->backend.ctx, s->held_handle, lba, blocks, buf);
    if (rc != 0) { /* ISO_ERR_EPOCH、ISO_ERR_IO など */
        memset(buf, 0, (size_t)bytes);
        remember_sense(s, MMC_SENSE_MEDIUM_ERROR, 0x11, 0x00);
        return MMC_STATUS_CHECK_CONDITION;
    }
    if (iso_media_abort_generation(s->media) != s->held_abort_generation) { /* 読み出し中のリセット */
        memset(buf, 0, (size_t)bytes);
        remember_sense(s, MMC_SENSE_ABORTED_COMMAND, 0x00, 0x00);
        return MMC_STATUS_CHECK_CONDITION;
    }
    /* size_bytes 以降は 0（ファイルの最後のセクタの EOF 以降は、0 とは限らない） */
    start = (uint64_t)lba * MMC_SCSI_BLOCK_SIZE;
    if (s->held_size_bytes < start + bytes) {
        uint64_t valid = s->held_size_bytes > start ? s->held_size_bytes - start : 0;
        memset(buf + valid, 0, (size_t)(bytes - valid));
    }
    return MMC_STATUS_GOOD;
}
