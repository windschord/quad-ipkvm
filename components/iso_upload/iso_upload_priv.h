#ifndef ISO_UPLOAD_PRIV_H
#define ISO_UPLOAD_PRIV_H

/* iso_upload の内部(iso_upload.c / iso_finish.c / iso_index.c で共有。公開しない) */

#include <stdio.h>
#include "iso_upload.h"

#define ISO_UP_MIB (1024ull * 1024ull)

static inline uint64_t iso_up_part_bytes_full(const iso_meta_t *m) { return (uint64_t)m->part_blocks * ISO_CD_BLOCK_BYTES; }

/* "/iso/<id>/p%03u"。長すぎたら ISO_ERR_INVALID。 */
static inline int iso_up_part_path(char *buf, size_t cap, const char *id, uint32_t part) {
    int n = snprintf(buf, cap, "/iso/%s/p%03u", id, (unsigned)part);
    return (n < 0 || (size_t)n >= cap) ? ISO_ERR_INVALID : ISO_OK;
}

static inline void iso_up_notify(iso_upload_t *u) {
    if (u->cfg.hooks.view_changed) u->cfg.hooks.view_changed(u->cfg.hooks.ctx);
}

static inline const iso_fs_t *iso_up_fs(const iso_upload_t *u) { return u->cfg.cat->fs; }

/* 枠を空ける(SHA-256 の状態は触らない) */
void iso_up_release_slot(iso_upload_t *u);
/* SHA-256 の状態を捨てる */
void iso_up_discard_sha(iso_upload_t *u);
/* 書き込み用のハンドルを閉じる(sync してから。どちらかが失敗しても、ハンドルは解放する)。最初のエラーを返す。 */
int iso_up_close_file(iso_upload_t *u, bool do_sync);
/* uploading のイメージを incomplete にする: IDLE_TIMEOUT / WRITE_FAIL / FINALIZE_IOERR のイベントで meta を書く。
 * meta の書き込みが失敗しても、メモリ上は incomplete にする(meta が uploading のままなら、再起動で incomplete)。 */
int iso_up_force_incomplete(iso_upload_t *u, iso_catalog_entry_t *e, iso_event_t ev);

#endif /* ISO_UPLOAD_PRIV_H */
