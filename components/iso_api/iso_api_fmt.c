#include "iso_api_priv.h"

#include <string.h>

/* ---- 書き出し(固定バッファ。溢れたら ok = false) -------------------------------------------- */

void iso_w_init(iso_w_t *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = (buf != NULL && cap > 0);
    if (w->ok) buf[0] = '\0';
}

static void w_bytes(iso_w_t *w, const char *s, size_t n) {
    if (!w->ok) return;
    if (n >= w->cap - w->len) { /* NUL の 1 バイトを残す */
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

void iso_w_str(iso_w_t *w, const char *s) { w_bytes(w, s, strlen(s)); }

void iso_w_u64(iso_w_t *w, uint64_t v) {
    char tmp[20];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    char out[21];
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    w_bytes(w, out, n);
}

void iso_w_bool(iso_w_t *w, bool v) { iso_w_str(w, v ? "true" : "false"); }

void iso_w_json_str(iso_w_t *w, const char *s, size_t len) {
    static const char HEX[] = "0123456789abcdef";
    w_bytes(w, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            w_bytes(w, e, 2);
        } else if (c < 0x20u) {
            char e[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 0xFu]};
            w_bytes(w, e, 6);
        } else {
            w_bytes(w, (const char *)&s[i], 1);
        }
    }
    w_bytes(w, "\"", 1);
}

void iso_w_hex(iso_w_t *w, const uint8_t *p, size_t n) {
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char h[2] = {HEX[p[i] >> 4], HEX[p[i] & 0xFu]};
        w_bytes(w, h, 2);
    }
}

int iso_w_finish(iso_w_t *w, size_t *out_len) {
    if (!w->ok) {
        if (w->buf && w->cap > 0) w->buf[0] = '\0';
        if (out_len) *out_len = 0;
        return -1;
    }
    if (out_len) *out_len = w->len;
    return 0;
}

/* ---- GET /api/v1/iso(WEB_API.md 8.1) ------------------------------------------------------- */

static const char *sd_state_name(iso_sd_state_t s) {
    switch (s) {
    case ISO_SD_READY: return "ready";
    case ISO_SD_NO_CARD: return "no_card";
    case ISO_SD_UNSUPPORTED_FS: return "unsupported_fs";
    default: return "error";
    }
}

static const char *image_state_name(iso_state_t s) {
    switch (s) {
    case ISO_STATE_UPLOADING: return "uploading";
    case ISO_STATE_INCOMPLETE: return "incomplete";
    case ISO_STATE_FINALIZING: return "finalizing";
    case ISO_STATE_READY: return "ready";
    case ISO_STATE_INVALID: return "invalid";
    case ISO_STATE_DELETING: return "deleting";
    default: return NULL;
    }
}

static const char *upload_state_name(iso_upload_activity_t a) {
    switch (a) {
    case ISO_UP_ACT_RECEIVING: return "receiving";
    case ISO_UP_ACT_FINALIZING: return "finalizing";
    default: return "idle";
    }
}

static void w_image(iso_w_t *w, const iso_view_image_t *img) {
    iso_w_str(w, "{\"id\":\"");
    iso_w_str(w, img->id);
    iso_w_str(w, "\",\"name\":");
    if (img->meta_valid) {
        iso_w_json_str(w, img->name, img->name_len);
    } else {
        iso_w_json_str(w, img->id, ISO_ID_LEN); /* meta_corrupt: 名前は <id> */
    }
    iso_w_str(w, ",\"size\":");
    if (img->meta_valid) {
        iso_w_u64(w, img->size_bytes);
    } else {
        iso_w_str(w, "null");
    }
    iso_w_str(w, ",\"state\":\"");
    iso_w_str(w, image_state_name(img->state));
    iso_w_str(w, "\",\"sha256\":");
    if (img->has_sha) {
        iso_w_str(w, "\"");
        iso_w_hex(w, img->sha256, ISO_SHA256_LEN);
        iso_w_str(w, "\"");
    } else {
        iso_w_str(w, "null");
    }
    iso_w_str(w, ",\"received\":");
    if (!img->meta_valid) {
        iso_w_str(w, "null");
    } else if (img->state == ISO_STATE_READY) {
        iso_w_u64(w, img->size_bytes); /* ready では size */
    } else {
        iso_w_u64(w, img->received);
    }
    iso_w_str(w, ",\"invalid_reason\":");
    const char *reason = NULL;
    if (img->state == ISO_STATE_INVALID) {
        switch (img->invalid_reason) {
        case ISO_INVALID_CHECKSUM_MISMATCH: reason = "checksum_mismatch"; break;
        case ISO_INVALID_NOT_ISO: reason = "not_iso"; break;
        case ISO_INVALID_TOO_FRAGMENTED: reason = "too_fragmented"; break;
        case ISO_INVALID_PART_MISMATCH: reason = "part_mismatch"; break;
        case ISO_INVALID_META_CORRUPT: reason = "meta_corrupt"; break;
        default: break;
        }
    }
    if (reason) {
        iso_w_str(w, "\"");
        iso_w_str(w, reason);
        iso_w_str(w, "\"");
    } else {
        iso_w_str(w, "null");
    }
    iso_w_str(w, "}");
}

int iso_api_format_list(const iso_api_snapshot_t *snap, const iso_media_status_t *media, char *out, size_t cap, size_t *out_len) {
    iso_w_t w;
    iso_w_init(&w, out, cap);
    if (!snap || !media) w.ok = false;
    if (!w.ok) return iso_w_finish(&w, out_len);

    const iso_view_t *v = &snap->view;
    const bool ready = (v->sd_state == ISO_SD_READY);

    iso_w_str(&w, "{\"sd\":{\"state\":\"");
    iso_w_str(&w, sd_state_name(v->sd_state));
    iso_w_str(&w, "\",\"total_bytes\":");
    if (ready && v->free_known) {
        iso_w_u64(&w, v->total_bytes);
    } else {
        iso_w_str(&w, "null");
    }
    iso_w_str(&w, ",\"free_bytes\":");
    if (ready && v->free_known) {
        iso_w_u64(&w, v->free_bytes);
    } else {
        iso_w_str(&w, "null");
    }
    iso_w_str(&w, "},\"images\":[");
    if (ready) {
        bool first = true;
        for (size_t i = 0; i < v->count; i++) {
            if (image_state_name(v->images[i].state) == NULL) continue; /* NONE は出さない */
            if (!first) iso_w_str(&w, ",");
            first = false;
            w_image(&w, &v->images[i]);
        }
    }
    iso_w_str(&w, "],\"mounted\":");
    if (media->mounted) {
        char id[ISO_ID_LEN + 1];
        iso_api_u32_to_id(media->id, id);
        const iso_view_image_t *found = NULL;
        for (size_t i = 0; ready && i < v->count; i++) {
            if (strcmp(v->images[i].id, id) == 0) found = &v->images[i];
        }
        iso_w_str(&w, "{\"id\":\"");
        iso_w_str(&w, id);
        iso_w_str(&w, "\",\"name\":");
        if (found && found->meta_valid) {
            iso_w_json_str(&w, found->name, found->name_len);
        } else {
            iso_w_json_str(&w, id, ISO_ID_LEN);
        }
        iso_w_str(&w, "}");
    } else {
        iso_w_str(&w, "null");
    }
    iso_w_str(&w, ",\"target\":{\"locked\":");
    iso_w_bool(&w, media->locked);
    iso_w_str(&w, ",\"ejected_by_target\":");
    iso_w_bool(&w, media->ejected_by_target);
    iso_w_str(&w, "},\"chunk_size\":");
    iso_w_u64(&w, v->chunk_bytes != 0 ? v->chunk_bytes : ISO_UPLOAD_CHUNK_KB * 1024u);
    iso_w_str(&w, ",\"hidden_images\":");
    iso_w_u64(&w, ready ? v->hidden : 0u);
    iso_w_str(&w, ",\"upload\":");
    if (ready && v->upload_active) {
        iso_w_str(&w, "{\"id\":\"");
        iso_w_str(&w, v->upload_id);
        iso_w_str(&w, "\",\"state\":\"");
        iso_w_str(&w, upload_state_name(v->upload_state));
        iso_w_str(&w, "\"}");
    } else {
        iso_w_str(&w, "null");
    }
    iso_w_str(&w, "}");
    return iso_w_finish(&w, out_len);
}
