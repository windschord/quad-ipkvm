#ifndef UP_SHA256_H
#define UP_SHA256_H

/* iso_upload / iso_core のテスト用の SHA-256(FIPS 180-4 から書いた参照実装)と、iso_sha256_t。
 * 速度は問わない。本番は mbedTLS(A-15)。 */

#include <stddef.h>
#include <stdint.h>
#include "iso_seams.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint8_t block[64];
    size_t block_len;
    uint64_t total;
} up_sha256_state_t;

void up_sha256_init(up_sha256_state_t *s);
void up_sha256_update(up_sha256_state_t *s, const void *data, size_t len);
void up_sha256_final(up_sha256_state_t *s, uint8_t out[32]);
/* 一括 */
void up_sha256(const void *data, size_t len, uint8_t out[32]);

/* iso_sha256_t(state_size = sizeof(up_sha256_state_t))。状態は呼び出し側が確保する */
iso_sha256_t up_sha256_iface(void);

#ifdef __cplusplus
}
#endif

#endif
