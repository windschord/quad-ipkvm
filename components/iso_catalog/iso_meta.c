#include "iso_catalog.h"

#include <string.h>

/* meta のレイアウト (設計 3.2、リトルエンディアン) */
#define OFF_MAGIC 0
#define OFF_VERSION 4
#define OFF_STATE 6
#define OFF_REASON 7
#define OFF_SIZE 8
#define OFF_PARTS 24
#define OFF_PART_BLOCKS 28
#define OFF_SHA 32
#define OFF_SHA_FLAG 64
#define OFF_NAME_LEN 65
#define OFF_NAME 66
#define NAME_FIELD 64
#define OFF_GENERATION 130
#define OFF_CRC 252

static const uint8_t MAGIC[4] = {'I', 'S', 'O', '1'};

uint32_t iso_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_le32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint64_t get_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

int iso_meta_validate(const iso_meta_t *m) {
    if (!m) return ISO_ERR_INVALID;
    if (m->state > ISO_META_DELETING) return ISO_ERR_INVALID;
    if (m->invalid_reason > ISO_INVALID_PART_MISMATCH) return ISO_ERR_INVALID; /* meta_corrupt (5) は保存しない */
    if (m->size_bytes < 1 || m->size_bytes > ISO_IMAGE_BYTES_MAX) return ISO_ERR_INVALID;
    if (m->part_count < 1 || m->part_count > ISO_MAX_PARTS) return ISO_ERR_INVALID;
    if (m->part_blocks < 1 || m->part_blocks > ISO_PART_BLOCKS_MAX) return ISO_ERR_INVALID;
    if (m->sha_flag > ISO_SHA_COMPUTED) return ISO_ERR_INVALID;
    if (!iso_name_valid(m->name, m->name_len)) return ISO_ERR_INVALID;
    return ISO_OK;
}

int iso_meta_encode(const iso_meta_t *m, uint8_t out[ISO_META_SIZE]) {
    if (!out || iso_meta_validate(m) != ISO_OK) return ISO_ERR_INVALID;
    uint8_t *b = out; /* 検査は済んでいる(以降は失敗しない)ので、直接書く */
    memset(b, 0, ISO_META_SIZE);
    memcpy(b + OFF_MAGIC, MAGIC, sizeof(MAGIC));
    put_le16(b + OFF_VERSION, ISO_META_VERSION);
    b[OFF_STATE] = m->state;
    b[OFF_REASON] = m->invalid_reason;
    put_le64(b + OFF_SIZE, m->size_bytes);
    put_le16(b + OFF_PARTS, m->part_count);
    put_le32(b + OFF_PART_BLOCKS, m->part_blocks);
    memcpy(b + OFF_SHA, m->sha256, ISO_SHA256_LEN);
    b[OFF_SHA_FLAG] = m->sha_flag;
    b[OFF_NAME_LEN] = m->name_len;
    memcpy(b + OFF_NAME, m->name, m->name_len);
    put_le32(b + OFF_GENERATION, m->generation);
    put_le32(b + OFF_CRC, iso_crc32(b, OFF_CRC));
    return ISO_OK;
}

int iso_meta_decode(const uint8_t *buf, size_t len, iso_meta_t *out) {
    if (!buf || !out || len != ISO_META_SIZE) return ISO_ERR_INVALID;
    if (memcmp(buf + OFF_MAGIC, MAGIC, sizeof(MAGIC)) != 0) return ISO_ERR_INVALID;
    if (get_le16(buf + OFF_VERSION) != ISO_META_VERSION) return ISO_ERR_INVALID;
    if (get_le32(buf + OFF_CRC) != iso_crc32(buf, OFF_CRC)) return ISO_ERR_INVALID;

    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = buf[OFF_STATE];
    m.invalid_reason = buf[OFF_REASON];
    m.size_bytes = get_le64(buf + OFF_SIZE);
    m.part_count = get_le16(buf + OFF_PARTS);
    m.part_blocks = get_le32(buf + OFF_PART_BLOCKS);
    memcpy(m.sha256, buf + OFF_SHA, ISO_SHA256_LEN);
    m.sha_flag = buf[OFF_SHA_FLAG];
    m.name_len = buf[OFF_NAME_LEN];
    if (m.name_len < 1 || m.name_len > ISO_NAME_MAX_BYTES) return ISO_ERR_INVALID; /* 64 バイトの枠を超えて読まない */
    memcpy(m.name, buf + OFF_NAME, m.name_len);
    m.name[m.name_len] = '\0';
    m.generation = get_le32(buf + OFF_GENERATION);
    if (iso_meta_validate(&m) != ISO_OK) return ISO_ERR_INVALID;
    *out = m;
    return ISO_OK;
}
