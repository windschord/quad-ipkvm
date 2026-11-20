#include "iso_api_priv.h"

#include <string.h>

/* 経路、判定の順序(docs/WEB_API.md 8 章の 13 段のうち、3〜13)、状態ごとのエラーの対応表。 */

#define RESERVE_BYTES ((uint64_t)ISO_FREE_RESERVE_MIB * 1048576u)

/* ---- id の変換 ----------------------------------------------------------------------- */

bool iso_api_id_to_u32(const char *id, uint32_t *out) {
    if (!iso_id_valid(id)) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < ISO_ID_LEN; i++) {
        char c = id[i];
        uint32_t d = (c >= 'a') ? (uint32_t)(c - 'a' + 10) : (uint32_t)(c - '0');
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

void iso_api_u32_to_id(uint32_t v, char out[ISO_ID_LEN + 1]) {
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < ISO_ID_LEN; i++) out[i] = HEX[(v >> (28u - 4u * (uint32_t)i)) & 0xFu];
    out[ISO_ID_LEN] = '\0';
}

/* ---- 経路 ---------------------------------------------------------------------------- */

#define ALLOW_LIST "GET, POST"
#define ALLOW_POST "POST"
#define ALLOW_IMAGE "PUT, DELETE"

static bool seg_is(const char *s, size_t n, const char *lit) { return n == strlen(lit) && memcmp(s, lit, n) == 0; }

static void route_set(iso_api_route_t *out, iso_api_method_t m, iso_api_method_t want, iso_api_endpoint_t ep, const char *allow) {
    if (m == want) {
        out->result = ISO_API_ROUTE_OK;
        out->ep = ep;
    } else {
        out->result = ISO_API_ROUTE_METHOD;
        out->allow = allow;
    }
}

/* seg[0..n) が <id>(8 桁の小文字の 16 進)か */
static bool seg_is_id(const char *seg, size_t n) {
    if (n != ISO_ID_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        if (!((seg[i] >= '0' && seg[i] <= '9') || (seg[i] >= 'a' && seg[i] <= 'f'))) return false;
    }
    return true;
}

static void route_image(iso_api_route_t *out, const char *seg, size_t n) {
    out->has_id = true;
    if (seg_is_id(seg, n)) {
        out->id_ok = true;
        memcpy(out->id, seg, n); /* out は memset 済み: NUL 終端 */
    }
}

void iso_api_route(iso_api_method_t method, const char *path, iso_api_route_t *out) {
    memset(out, 0, sizeof(*out)); /* result = ISO_API_ROUTE_NOT_FOUND(0) */
    if (!path) return;
    size_t plen = strcspn(path, "?");
    const size_t pre = sizeof(ISO_API_PATH_PREFIX) - 1;
    if (plen < pre || memcmp(path, ISO_API_PATH_PREFIX, pre) != 0) return;
    const char *rest = path + pre;
    size_t rlen = plen - pre;

    if (rlen == 0) { /* /api/v1/iso */
        if (method == ISO_API_M_GET) {
            out->result = ISO_API_ROUTE_OK;
            out->ep = ISO_API_EP_LIST;
        } else if (method == ISO_API_M_POST) {
            out->result = ISO_API_ROUTE_OK;
            out->ep = ISO_API_EP_BEGIN;
        } else {
            out->result = ISO_API_ROUTE_METHOD;
            out->allow = ALLOW_LIST;
        }
        return;
    }
    if (rest[0] != '/') return; /* /api/v1/isoX */
    rest++;
    rlen--;
    size_t seg = 0;
    while (seg < rlen && rest[seg] != '/') seg++;
    if (seg == 0) return; /* /api/v1/iso/ と /api/v1/iso// */

    if (seg == rlen) { /* /api/v1/iso/<seg> */
        if (seg_is(rest, seg, "mount")) {
            route_set(out, method, ISO_API_M_POST, ISO_API_EP_MOUNT, ALLOW_POST);
        } else if (seg_is(rest, seg, "eject")) {
            route_set(out, method, ISO_API_M_POST, ISO_API_EP_EJECT, ALLOW_POST);
        } else if (seg_is(rest, seg, "rescan")) {
            route_set(out, method, ISO_API_M_POST, ISO_API_EP_RESCAN, ALLOW_POST);
        } else {
            route_image(out, rest, seg);
            if (method == ISO_API_M_PUT) {
                out->result = ISO_API_ROUTE_OK;
                out->ep = ISO_API_EP_PUT;
            } else if (method == ISO_API_M_DELETE) {
                out->result = ISO_API_ROUTE_OK;
                out->ep = ISO_API_EP_DELETE;
            } else {
                out->result = ISO_API_ROUTE_METHOD;
                out->allow = ALLOW_IMAGE;
            }
        }
        return;
    }
    /* /api/v1/iso/<seg>/<rest> は、/finish だけ */
    if (!seg_is(rest + seg + 1, rlen - seg - 1, "finish")) return;
    route_image(out, rest, seg);
    route_set(out, method, ISO_API_M_POST, ISO_API_EP_FINISH, ALLOW_POST);
}

const char *iso_api_allow_for_path(const char *path) {
    iso_api_route_t rt;
    iso_api_route(ISO_API_M_OTHER, path, &rt);
    return rt.result == ISO_API_ROUTE_METHOD ? rt.allow : NULL;
}

bool iso_api_ep_has_body(iso_api_endpoint_t ep) {
    return ep == ISO_API_EP_BEGIN || ep == ISO_API_EP_MOUNT || ep == ISO_API_EP_FINISH || ep == ISO_API_EP_EJECT ||
           ep == ISO_API_EP_RESCAN;
}

/* ---- Content-Type ---------------------------------------------------------------------- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* s の先頭が lit(小文字)と、大文字小文字を区別せずに一致すれば、その長さ。そうでなければ 0。 */
static size_t starts_with_ci(const char *s, const char *lit) {
    size_t n = strlen(lit);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\0' || lower(s[i]) != lit[i]) return 0;
    }
    return n;
}

iso_api_ctype_t iso_api_ctype_parse(const char *value) {
    if (!value) return ISO_API_CT_NONE;
    const char *p = value;
    while (*p == ' ' || *p == '\t') p++;
    iso_api_ctype_t t = ISO_API_CT_JSON;
    size_t n = starts_with_ci(p, "application/json");
    if (n == 0) {
        t = ISO_API_CT_OCTET;
        n = starts_with_ci(p, "application/octet-stream");
    }
    if (n == 0) return ISO_API_CT_OTHER;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    return (*p == '\0' || *p == ';') ? t : ISO_API_CT_OTHER;
}

/* ---- 応答の組み立て ----------------------------------------------------------------------- */

void iso_api_response_init(iso_api_response_t *rsp, char *buf, size_t cap) {
    memset(rsp, 0, sizeof(*rsp));
    rsp->body = buf;
    rsp->body_cap = cap;
    if (buf && cap > 0) buf[0] = '\0';
}

static void rsp_reset(iso_api_response_t *rsp) { iso_api_response_init(rsp, rsp->body, rsp->body_cap); }

/* w を確定する。溢れていたら 500 internal_error */
static void rsp_finish(iso_api_response_t *rsp, iso_w_t *w, int status, const char *reason) {
    size_t n = 0;
    if (iso_w_finish(w, &n) == 0) {
        rsp->status = status;
        rsp->body_len = n;
        rsp->log_reason = reason;
        return;
    }
    iso_w_t e;
    iso_w_init(&e, rsp->body, rsp->body_cap);
    iso_w_str(&e, "{\"error\":\"internal_error\"}");
    (void)iso_w_finish(&e, &rsp->body_len);
    rsp->status = 500;
    rsp->log_reason = "internal_error";
}

static void set_error(iso_api_response_t *rsp, int status, const char *code) {
    iso_w_t w;
    iso_w_init(&w, rsp->body, rsp->body_cap);
    iso_w_str(&w, "{\"error\":\"");
    iso_w_str(&w, code);
    iso_w_str(&w, "\"}");
    rsp_finish(rsp, &w, status, code);
}

static void set_error_u64(iso_api_response_t *rsp, int status, const char *code, const char *key, uint64_t v) {
    iso_w_t w;
    iso_w_init(&w, rsp->body, rsp->body_cap);
    iso_w_str(&w, "{\"error\":\"");
    iso_w_str(&w, code);
    iso_w_str(&w, "\",\"");
    iso_w_str(&w, key);
    iso_w_str(&w, "\":");
    iso_w_u64(&w, v);
    iso_w_str(&w, "}");
    rsp_finish(rsp, &w, status, code);
}

static void set_size_mismatch(iso_api_response_t *rsp, uint64_t received, uint64_t size) {
    iso_w_t w;
    iso_w_init(&w, rsp->body, rsp->body_cap);
    iso_w_str(&w, "{\"error\":\"size_mismatch\",\"received\":");
    iso_w_u64(&w, received);
    iso_w_str(&w, ",\"size\":");
    iso_w_u64(&w, size);
    iso_w_str(&w, "}");
    rsp_finish(rsp, &w, 409, "size_mismatch");
}

static void set_busy(iso_api_response_t *rsp) {
    set_error(rsp, 503, "busy");
    rsp->has_retry_after = true;
    rsp->retry_after_s = 1;
}

static void set_not_ready(iso_api_response_t *rsp) { set_error(rsp, 503, "not_ready"); }

/* {"<key>":"<value>"} の 1 つ(結果の応答) */
static void set_result(iso_api_response_t *rsp, int status, const char *key, const char *value) {
    iso_w_t w;
    iso_w_init(&w, rsp->body, rsp->body_cap);
    iso_w_str(&w, "{\"");
    iso_w_str(&w, key);
    iso_w_str(&w, "\":\"");
    iso_w_str(&w, value);
    iso_w_str(&w, "\"}");
    rsp_finish(rsp, &w, status, value);
}

/* {"result":"<value>","id":"<id>"} */
static void set_result_id(iso_api_response_t *rsp, const char *value, const char *id) {
    iso_w_t w;
    iso_w_init(&w, rsp->body, rsp->body_cap);
    iso_w_str(&w, "{\"result\":\"");
    iso_w_str(&w, value);
    iso_w_str(&w, "\",\"id\":\"");
    iso_w_str(&w, id);
    iso_w_str(&w, "\"}");
    rsp_finish(rsp, &w, 200, value);
}

static const char *invalid_reason_name(uint8_t r) {
    switch (r) {
    case ISO_INVALID_CHECKSUM_MISMATCH: return "checksum_mismatch";
    case ISO_INVALID_NOT_ISO: return "not_iso";
    case ISO_INVALID_TOO_FRAGMENTED: return "too_fragmented";
    case ISO_INVALID_PART_MISMATCH: return "part_mismatch";
    case ISO_INVALID_META_CORRUPT: return "meta_corrupt";
    default: return NULL;
    }
}

/* 422 {"error":"<invalid_reason>"}。理由が分からない(あり得ない)ときは 500 */
static void set_invalid(iso_api_response_t *rsp, uint8_t reason) {
    const char *name = invalid_reason_name(reason);
    if (!name) {
        set_error(rsp, 500, "internal_error");
        return;
    }
    set_error(rsp, 422, name);
}

static void set_ready(iso_api_response_t *rsp, const uint8_t *sha /* NULL = null */) {
    iso_w_t w;
    iso_w_init(&w, rsp->body, rsp->body_cap);
    iso_w_str(&w, "{\"state\":\"ready\",\"sha256\":");
    if (sha) {
        iso_w_str(&w, "\"");
        iso_w_hex(&w, sha, ISO_SHA256_LEN);
        iso_w_str(&w, "\"");
    } else {
        iso_w_str(&w, "null");
    }
    iso_w_str(&w, "}");
    rsp_finish(rsp, &w, 200, "ready");
}

static void set_log_id(iso_api_response_t *rsp, const char *id) { memcpy(rsp->log_id, id, ISO_ID_LEN + 1); }

/* ---- 公開ビュー ---------------------------------------------------------------------------- */

void iso_api_snapshot_update(iso_api_snapshot_t *s, const iso_view_t *v, const iso_upload_t *up) {
    s->view = *v;
    if (s->view.count > ISO_MAX_IMAGES) s->view.count = ISO_MAX_IMAGES;
    for (size_t i = 0; i < ISO_MAX_IMAGES; i++) s->view.images[i].table = NULL;
    s->consumed_since = up ? up->consumed_since : 0;
}

static const iso_api_snapshot_t EMPTY_SNAPSHOT; /* 静的記憶域: 0 = sd.state が no_card、イメージなし */

typedef struct {
    const iso_api_snapshot_t *snap;
    iso_media_status_t ms;
} vctx_t;

static bool env_usable(const iso_api_env_t *e) {
    return e && e->state_lock && e->state_unlock && e->op_trylock && e->op_unlock && e->snapshot && e->media && e->core;
}

/* vc は、呼び出し側が {0} で初期化する(usable でないとき、ms は 0 = マウントなし) */
static void view_enter(const iso_api_env_t *env, bool usable, vctx_t *vc) {
    if (!usable) {
        vc->snap = &EMPTY_SNAPSHOT;
        return;
    }
    env->state_lock(env->ctx);
    vc->snap = env->snapshot;
    iso_media_get_status(env->media, &vc->ms);
}

static void view_leave(const iso_api_env_t *env, bool usable) {
    if (usable) env->state_unlock(env->ctx);
}

static bool sd_ready(const vctx_t *vc) { return vc->snap->view.sd_state == ISO_SD_READY; }

static const iso_view_image_t *find_image(const iso_view_t *v, const char *id) {
    for (size_t i = 0; i < v->count; i++) {
        if (v->images[i].state != ISO_STATE_NONE && strcmp(v->images[i].id, id) == 0) return &v->images[i];
    }
    return NULL;
}

/* 空き容量の推定(iso_upload_free_estimate と同じ式): キャッシュ値 − consumed_since − 途中のイメージの (size − received)。skip は数えない。 */
static uint64_t avail_bytes(const iso_api_snapshot_t *s, const iso_view_image_t *skip) {
    const iso_view_t *v = &s->view;
    if (!v->free_known) return 0;
    uint64_t used = s->consumed_since;
    /* 配列の添字で直接読む(count が配列の大きさのとき、1 つ先を読む誤りを、UBSan の bounds が検出できる) */
    for (size_t i = 0; i < v->count; i++) {
        if (&v->images[i] == skip || !v->images[i].meta_valid) continue;
        if (v->images[i].state != ISO_STATE_UPLOADING && v->images[i].state != ISO_STATE_INCOMPLETE) continue;
        if (v->images[i].received < v->images[i].size_bytes) used += v->images[i].size_bytes - v->images[i].received;
    }
    return v->free_bytes > used ? v->free_bytes - used : 0;
}

static bool name_exists(const iso_view_t *v, const char *name, size_t len) {
    for (size_t i = 0; i < v->count; i++) {
        if (!v->images[i].meta_valid || v->images[i].state == ISO_STATE_NONE || v->images[i].state == ISO_STATE_DELETING) continue;
        if (iso_name_equal(v->images[i].name, v->images[i].name_len, name, len)) return true;
    }
    return false;
}

static uint32_t chunk_size_of(const iso_view_t *v) { return v->chunk_bytes != 0 ? v->chunk_bytes : ISO_UPLOAD_CHUNK_KB * 1024u; }

/* ---- 段 3〜5(本体を読む前) ------------------------------------------------------------------ */

static const char *kind_name(iso_api_endpoint_t ep) {
    switch (ep) {
    case ISO_API_EP_BEGIN: return "begin";
    case ISO_API_EP_MOUNT: return "mount";
    case ISO_API_EP_EJECT: return "eject";
    case ISO_API_EP_RESCAN: return "rescan";
    case ISO_API_EP_FINISH: return "finish";
    case ISO_API_EP_DELETE: return "delete";
    default: return NULL; /* LIST と PUT は記録しない */
    }
}

static bool check_headers(const iso_api_request_t *rq, iso_api_response_t *rsp, iso_api_route_t *rt) {
    iso_api_route(rq->method, rq->path, rt);
    if (rt->result == ISO_API_ROUTE_NOT_FOUND) {
        set_error(rsp, 404, "not_found");
        return false;
    }
    if (rt->result == ISO_API_ROUTE_METHOD) {
        set_error(rsp, 405, "method_not_allowed");
        rsp->allow = rt->allow;
        return false;
    }
    rsp->log_kind = kind_name(rt->ep);
    rsp->log = (rsp->log_kind != NULL);
    if (rt->has_id && rt->id_ok) set_log_id(rsp, rt->id);

    /* 段 3: パスの id の形式不正 */
    if (rt->has_id && !rt->id_ok) {
        set_error(rsp, 404, "not_found");
        return false;
    }

    /* 段 4: Content-Type(本文がないときの finish / eject / rescan と、DELETE には適用しない) */
    const iso_api_ctype_t ct = iso_api_ctype_parse(rq->content_type);
    const bool has_body = rq->has_content_length && rq->content_length > 0;
    bool bad_type = false;
    switch (rt->ep) {
    case ISO_API_EP_PUT: bad_type = (ct != ISO_API_CT_OCTET); break;
    case ISO_API_EP_BEGIN:
    case ISO_API_EP_MOUNT: bad_type = (ct != ISO_API_CT_JSON); break;
    case ISO_API_EP_FINISH:
    case ISO_API_EP_EJECT:
    case ISO_API_EP_RESCAN: bad_type = has_body && (ct != ISO_API_CT_JSON); break;
    default: break;
    }
    if (bad_type) {
        set_error(rsp, 415, "unsupported_media_type");
        return false;
    }

    /* 段 5: PUT の Content-Length の欠落(411)、JSON のボディの大きさ(413)。PUT の chunk_size 超過(413)は、env が要るので iso_api_put_precheck */
    if (rt->ep == ISO_API_EP_PUT && !rq->has_content_length) {
        set_error(rsp, 411, "length_required");
        return false;
    }
    if (iso_api_ep_has_body(rt->ep) && rq->has_content_length && rq->content_length > ISO_API_BODY_MAX) {
        set_error(rsp, 413, "payload_too_large");
        return false;
    }
    return true;
}

bool iso_api_headers_check(const iso_api_request_t *rq, iso_api_response_t *rsp) {
    if (!rsp) return false;
    rsp_reset(rsp);
    if (!rq || !rq->path) {
        set_error(rsp, 500, "internal_error");
        return false;
    }
    iso_api_route_t rt;
    bool ok = check_headers(rq, rsp, &rt);
    if (!ok && rq->method == ISO_API_M_PUT) rsp->close_connection = true; /* 本体を読まずに応答する */
    return ok;
}

/* ---- 段 7〜12(公開ビューと iso_media の状態から。iso_state_mutex の下) -------------------------------- */

/* 真 = 処理(段 13)に進む。偽 = rsp が埋まっている(拒否、または状態だけで決まる結果) */

static bool check_begin(const vctx_t *vc, const iso_api_body_t *b, iso_api_response_t *rsp) {
    const iso_view_t *v = &vc->snap->view;
    if (!sd_ready(vc)) {
        set_not_ready(rsp);
        return false;
    }
    if (v->upload_active) {
        set_error(rsp, 409, "upload_in_progress");
        return false;
    }
    if (name_exists(v, b->name, b->name_len)) {
        set_error(rsp, 409, "name_exists");
        return false;
    }
    if (v->count >= ISO_MAX_IMAGES) {
        set_error(rsp, 409, "too_many_images");
        return false;
    }
    uint64_t avail = avail_bytes(vc->snap, NULL);
    if (!v->free_known || b->size > v->total_bytes || avail < b->size + RESERVE_BYTES) {
        set_error_u64(rsp, 507, "insufficient_storage", "free_bytes", avail);
        return false;
    }
    return true;
}

static bool check_mount(const vctx_t *vc, const char *id, uint32_t id_u32, bool force, iso_api_response_t *rsp) {
    const iso_view_t *v = &vc->snap->view;
    if (!sd_ready(vc)) {
        set_not_ready(rsp);
        return false;
    }
    const iso_view_image_t *img = find_image(v, id);
    if (!img || img->state == ISO_STATE_DELETING) {
        set_error(rsp, 404, "not_found");
        return false;
    }
    if (img->state != ISO_STATE_READY) {
        set_error(rsp, 409, "image_not_ready");
        return false;
    }
    if (vc->ms.mounted && vc->ms.id == id_u32) {
        set_result_id(rsp, "already_mounted", id);
        return false;
    }
    if (vc->ms.locked && !force) {
        set_error(rsp, 409, "locked_by_target");
        return false;
    }
    return true;
}

static bool check_eject(const vctx_t *vc, bool force, iso_api_response_t *rsp) {
    if (!sd_ready(vc)) { /* 例外: eject は、SD が使えなくても 200 not_mounted */
        set_result(rsp, 200, "result", "not_mounted");
        return false;
    }
    if (vc->ms.locked && !force) {
        set_error(rsp, 409, "locked_by_target");
        return false;
    }
    return true;
}

static bool check_rescan(const vctx_t *vc, iso_api_response_t *rsp) {
    if (!sd_ready(vc)) {
        set_not_ready(rsp);
        return false;
    }
    if (vc->snap->view.upload_active) {
        set_error(rsp, 409, "upload_in_progress");
        return false;
    }
    return true;
}

static bool check_finish(const vctx_t *vc, const char *id, iso_api_response_t *rsp) {
    const iso_view_t *v = &vc->snap->view;
    if (!sd_ready(vc)) {
        set_not_ready(rsp);
        return false;
    }
    const iso_view_image_t *img = find_image(v, id);
    if (!img || img->state == ISO_STATE_DELETING) {
        set_error(rsp, 404, "not_found");
        return false;
    }
    switch (img->state) {
    case ISO_STATE_READY: /* 冪等 */
        set_ready(rsp, img->has_sha ? img->sha256 : NULL);
        return false;
    case ISO_STATE_INVALID:
        set_invalid(rsp, img->invalid_reason);
        return false;
    case ISO_STATE_UPLOADING:
    case ISO_STATE_INCOMPLETE:
        if (img->received < img->size_bytes) {
            set_size_mismatch(rsp, img->received, img->size_bytes);
            return false;
        }
        if (img->state == ISO_STATE_INCOMPLETE && v->upload_active) {
            set_error(rsp, 409, "upload_in_progress");
            return false;
        }
        return true;
    default: return true; /* finalizing: ワーカーの FINISH(冪等)。処理で 202 */
    }
}

static bool check_delete(const vctx_t *vc, const char *id, uint32_t id_u32, iso_api_response_t *rsp) {
    if (!sd_ready(vc)) {
        set_not_ready(rsp);
        return false;
    }
    const iso_view_image_t *img = find_image(&vc->snap->view, id);
    if (!img) {
        set_error(rsp, 404, "not_found");
        return false;
    }
    if (img->state != ISO_STATE_DELETING && vc->ms.mounted && vc->ms.id == id_u32) {
        set_error(rsp, 409, "mounted");
        return false;
    }
    return true;
}

/* PUT の段 7〜12。chunk の長さ len(Content-Length)と offset は、検査済みの値 */
static bool check_put(const vctx_t *vc, const char *id, uint64_t offset, uint64_t len, iso_api_response_t *rsp) {
    const iso_view_t *v = &vc->snap->view;
    if (!sd_ready(vc)) {
        set_not_ready(rsp);
        return false;
    }
    const iso_view_image_t *img = find_image(v, id);
    if (!img || img->state == ISO_STATE_DELETING) {
        set_error(rsp, 404, "not_found");
        return false;
    }
    if (img->state != ISO_STATE_UPLOADING && img->state != ISO_STATE_INCOMPLETE) {
        set_error(rsp, 409, "not_uploading");
        return false;
    }
    if (img->state == ISO_STATE_INCOMPLETE && v->upload_active) {
        set_error(rsp, 409, "upload_in_progress");
        return false;
    }
    if (offset != img->received) {
        set_error_u64(rsp, 409, "offset_mismatch", "received", img->received);
        return false;
    }
    if (len > img->size_bytes - offset) {
        set_error(rsp, 400, "invalid_request");
        return false;
    }
    if (img->state == ISO_STATE_INCOMPLETE) { /* 再開した最初の PUT: 残りの大きさ + 予備 */
        uint64_t remaining = img->size_bytes - img->received;
        if (avail_bytes(vc->snap, img) < remaining + RESERVE_BYTES) {
            set_error_u64(rsp, 507, "insufficient_storage", "free_bytes", avail_bytes(vc->snap, NULL));
            return false;
        }
    }
    return true;
}

/* 判定(go)を通って、処理(段 13)に進むか。サービスが起動していない(usable でない)ときは、公開ビューが空(no_card)なので、判定がすべて拒否する。
 * ここは、念のための守り(env を使わせない)。 */
static bool proceed(bool go, bool usable, iso_api_response_t *rsp) {
    if (!go) return false;
    if (!usable) {
        set_not_ready(rsp);
        return false;
    }
    return true;
}

/* ---- 段 13(処理。iso_op_mutex の下) ------------------------------------------------------------ */

/* iso_op_mutex を try-lock で取り、SD の状態を再検査する(取れなければ 503 busy、使えなければ 503 not_ready)。 */
static bool op_enter(const iso_api_env_t *env, iso_api_response_t *rsp, bool eject) {
    uint32_t wait = env->op_lock_wait_ms != 0 ? env->op_lock_wait_ms : ISO_OP_LOCK_WAIT_MS;
    if (!env->op_trylock(env->ctx, wait)) {
        set_busy(rsp);
        return false;
    }
    env->state_lock(env->ctx);
    bool ready = (env->snapshot->view.sd_state == ISO_SD_READY);
    env->state_unlock(env->ctx);
    if (!ready) {
        env->op_unlock(env->ctx);
        if (eject) {
            set_result(rsp, 200, "result", "not_mounted");
        } else {
            set_not_ready(rsp);
        }
        return false;
    }
    return true;
}

static void op_leave(const iso_api_env_t *env) { env->op_unlock(env->ctx); }

/* 負の iso_err_t(と、想定外の正の値)を応答にする */
static void fail_rc(iso_api_response_t *rsp, int rc, const iso_upload_t *up) {
    if (rc == ISO_ERR_BUSY || rc == ISO_CAT_ERR_ID_EXHAUSTED) { /* ワーカーのキューのあふれ、<id> の引き直しの尽きた(乱数の衝突)。再試行してよい */
        set_busy(rsp);
    } else if (rc == ISO_ERR_NO_SPACE) {
        set_error_u64(rsp, 507, "insufficient_storage", "free_bytes", iso_upload_free_estimate(up));
    } else if (rc < 0) {
        set_error(rsp, 503, "sd_unavailable");
    } else {
        set_error(rsp, 500, "internal_error");
    }
}

static void exec_begin(const iso_api_env_t *env, const iso_api_body_t *b, iso_api_response_t *rsp) {
    if (!op_enter(env, rsp, false)) return;
    iso_upload_t *up = iso_core_upload(env->core);
    char id[ISO_ID_LEN + 1];
    int rc = iso_upload_begin(up, b->name, b->name_len, b->size, b->has_sha ? b->sha : NULL, id);
    if (rc == ISO_OK) {
        set_log_id(rsp, id);
        iso_w_t w;
        iso_w_init(&w, rsp->body, rsp->body_cap);
        iso_w_str(&w, "{\"id\":\"");
        iso_w_str(&w, id);
        iso_w_str(&w, "\",\"chunk_size\":");
        iso_w_u64(&w, iso_upload_chunk_bytes(up));
        iso_w_str(&w, "}");
        rsp_finish(rsp, &w, 201, "ok");
    } else if (rc == ISO_ERR_INVALID) {
        set_error(rsp, 400, "invalid_request");
    } else if (rc == ISO_RES_UPLOAD_IN_PROGRESS) {
        set_error(rsp, 409, "upload_in_progress");
    } else if (rc == ISO_RES_NAME_EXISTS || rc == ISO_CAT_ERR_NAME_EXISTS) {
        set_error(rsp, 409, "name_exists");
    } else if (rc == ISO_RES_TOO_MANY_IMAGES || rc == ISO_CAT_ERR_TOO_MANY) {
        set_error(rsp, 409, "too_many_images");
    } else if (rc == ISO_RES_INSUFFICIENT_STORAGE) {
        set_error_u64(rsp, 507, "insufficient_storage", "free_bytes", iso_upload_free_estimate(up));
    } else {
        fail_rc(rsp, rc, up);
    }
    op_leave(env);
}

static void exec_finish(const iso_api_env_t *env, const char *id, iso_api_response_t *rsp) {
    if (!op_enter(env, rsp, false)) return;
    iso_core_t *core = env->core;
    uint64_t received = 0;
    int rc = iso_core_submit_finish(core, id, &received);
    const iso_catalog_entry_t *e = iso_catalog_find(core->cfg.cat, id);
    if (rc == ISO_RES_OK || rc == ISO_RES_JOINED || rc == ISO_RES_ALREADY_FINALIZING) {
        set_result(rsp, 202, "state", "finalizing");
    } else if (rc == ISO_RES_ALREADY_READY) {
        if (!iso_core_table(core, id) || !e) { /* ready だが、エクステントの表がまだない(INDEX 待ち・中)。表示は finalizing */
            set_result(rsp, 202, "state", "finalizing");
        } else {
            set_ready(rsp, (e->meta_valid && e->meta.sha_flag == ISO_SHA_COMPUTED) ? e->meta.sha256 : NULL);
        }
    } else if (rc == ISO_RES_INVALID_IMAGE) {
        set_invalid(rsp, e ? e->invalid_reason : 0);
    } else if (rc == ISO_RES_NOT_FOUND) {
        set_error(rsp, 404, "not_found");
    } else if (rc == ISO_RES_SIZE_MISMATCH) {
        set_size_mismatch(rsp, received, (e && e->meta_valid) ? e->meta.size_bytes : 0);
    } else if (rc == ISO_RES_UPLOAD_IN_PROGRESS) {
        set_error(rsp, 409, "upload_in_progress");
    } else {
        fail_rc(rsp, rc, iso_core_upload(core));
    }
    op_leave(env);
}

static void exec_delete(const iso_api_env_t *env, const char *id, iso_api_response_t *rsp) {
    if (!op_enter(env, rsp, false)) return;
    iso_core_t *core = env->core;
    const iso_catalog_entry_t *e = iso_catalog_find(core->cfg.cat, id);
    int rc = ISO_OK;
    if (e && e->state != ISO_STATE_NONE && e->state != ISO_STATE_DELETING) {
        /* rename の途中の電源断のクロスリンク(設計 3.3): iso_catalog_apply(DELETE) の前に調べる */
        rc = iso_catalog_delete_precheck(core->cfg.cat, id);
        if (rc == ISO_CAT_ERR_CROSSLINKED) {
            set_error(rsp, 409, "crosslinked");
            op_leave(env);
            return;
        }
    }
    if (rc == ISO_OK) rc = iso_core_submit_delete(core, id);
    if (rc == ISO_RES_OK || rc == ISO_RES_JOINED) {
        set_result(rsp, 202, "state", "deleting");
    } else if (rc == ISO_RES_NOT_FOUND) {
        set_error(rsp, 404, "not_found");
    } else if (rc == ISO_RES_MOUNTED) {
        set_error(rsp, 409, "mounted");
    } else {
        fail_rc(rsp, rc, iso_core_upload(core));
    }
    op_leave(env);
}

static void exec_mount(const iso_api_env_t *env, const char *id, uint32_t id_u32, bool force, iso_api_response_t *rsp) {
    if (!op_enter(env, rsp, false)) return;
    iso_core_t *core = env->core;
    const iso_catalog_entry_t *e = iso_catalog_find(core->cfg.cat, id);
    if (!e || e->state == ISO_STATE_NONE || e->state == ISO_STATE_DELETING) {
        set_error(rsp, 404, "not_found");
    } else {
        /* マウントできるのは、エクステントの表が公開された ready で、表が今のカードのものであるときだけ(設計 4.1) */
        iso_extent_table_t *t = iso_core_table(core, id);
        const iso_blockdev_t *bd = core->cfg.bdev;
        if (e->state != ISO_STATE_READY || !t || !bd || iso_extent_table_epoch(t) != bd->epoch(bd->ctx)) {
            set_error(rsp, 409, "image_not_ready");
        } else {
            env->state_lock(env->ctx);
            const bool sd_ok = (env->snapshot->view.sd_state == ISO_SD_READY); /* iso_state_mutex の中で、sd.state を再検査する */
            iso_media_result_t r = ISO_MEDIA_INVALID;
            if (sd_ok) r = iso_media_mount(env->media, id_u32, force);
            env->state_unlock(env->ctx);
            if (!sd_ok) {
                set_not_ready(rsp);
            } else if (r == ISO_MEDIA_MOUNTED) {
                set_result_id(rsp, "mounted", id);
            } else if (r == ISO_MEDIA_ALREADY_MOUNTED) {
                set_result_id(rsp, "already_mounted", id);
            } else if (r == ISO_MEDIA_LOCKED) {
                set_error(rsp, 409, "locked_by_target");
            } else {
                set_error(rsp, 500, "internal_error");
            }
        }
    }
    op_leave(env);
}

static void exec_eject(const iso_api_env_t *env, bool force, iso_api_response_t *rsp) {
    if (!op_enter(env, rsp, true)) return;
    env->state_lock(env->ctx);
    iso_media_result_t r = iso_media_eject(env->media, force);
    env->state_unlock(env->ctx);
    if (r == ISO_MEDIA_EJECTED) {
        set_result(rsp, 200, "result", "ejected");
    } else if (r == ISO_MEDIA_NOT_MOUNTED) {
        set_result(rsp, 200, "result", "not_mounted");
    } else if (r == ISO_MEDIA_LOCKED) {
        set_error(rsp, 409, "locked_by_target");
    } else {
        set_error(rsp, 500, "internal_error");
    }
    op_leave(env);
}

static void exec_rescan(const iso_api_env_t *env, uint32_t cursor, iso_api_response_t *rsp) {
    if (!op_enter(env, rsp, false)) return;
    iso_core_t *core = env->core;
    iso_rescan_result_t rr;
    memset(&rr, 0, sizeof(rr));
    int rc = iso_catalog_rescan(core->cfg.cat, cursor, &rr);
    iso_core_mark_unindexed(core); /* 取り込んだものは、エクステントの表の作成(INDEX)へ */
    iso_core_publish(core);
    if (rc < 0) {
        fail_rc(rsp, rc, iso_core_upload(core));
    } else {
        iso_w_t w;
        iso_w_init(&w, rsp->body, rsp->body_cap);
        iso_w_str(&w, "{\"imported\":");
        iso_w_u64(&w, rr.imported);
        iso_w_str(&w, ",\"skipped\":");
        iso_w_u64(&w, rr.skipped);
        iso_w_str(&w, ",\"next_cursor\":");
        if (rr.has_next) {
            iso_w_u64(&w, rr.next_cursor);
        } else {
            iso_w_str(&w, "null");
        }
        iso_w_str(&w, "}");
        rsp_finish(rsp, &w, 200, "ok");
    }
    op_leave(env);
}

/* ---- PUT ---------------------------------------------------------------------------------- */

static bool put_precheck_routed(const iso_api_env_t *env, const iso_api_request_t *rq, const iso_api_route_t *rt, iso_api_response_t *rsp) {
    const bool usable = env_usable(env);
    vctx_t vc = {0};

    /* 段 5: Content-Length が chunk_size を超える(413)。chunk_size は、公開ビューの値 */
    view_enter(env, usable, &vc);
    const uint32_t chunk = chunk_size_of(&vc.snap->view);
    view_leave(env, usable);
    if (rq->content_length > chunk) {
        set_error(rsp, 413, "payload_too_large");
        return false;
    }

    /* 段 6: 形式(Content-Length の値、X-Upload-Offset) */
    uint64_t offset = 0;
    if (rq->content_length == 0 || !rq->upload_offset || iso_api_parse_u64(rq->upload_offset, strlen(rq->upload_offset), &offset) != 0) {
        set_error(rsp, 400, "invalid_request");
        return false;
    }

    /* 段 7〜12 */
    view_enter(env, usable, &vc);
    bool ok = check_put(&vc, rt->id, offset, rq->content_length, rsp);
    view_leave(env, usable);
    if (!ok) return false;

    rsp->put.accepted = true;
    memcpy(rsp->put.id, rt->id, ISO_ID_LEN + 1);
    rsp->put.offset = offset;
    rsp->put.len = (uint32_t)rq->content_length;
    return true;
}

bool iso_api_put_precheck(const iso_api_env_t *env, const iso_api_request_t *rq, iso_api_response_t *rsp) {
    if (!rsp) return false;
    rsp_reset(rsp);
    if (!rq || !rq->path) {
        set_error(rsp, 500, "internal_error");
        rsp->close_connection = true;
        return false;
    }
    iso_api_route_t rt;
    bool ok = check_headers(rq, rsp, &rt);
    if (ok && rt.ep != ISO_API_EP_PUT) { /* PUT でない要求を渡した(グルーの誤り) */
        set_error(rsp, 500, "internal_error");
        ok = false;
    }
    if (ok) ok = put_precheck_routed(env, rq, &rt, rsp);
    if (!ok) rsp->close_connection = true;
    return ok;
}

void iso_api_put_commit(const iso_api_env_t *env, const iso_api_put_t *put, const void *data, size_t len, iso_api_response_t *rsp) {
    if (!rsp) return;
    rsp_reset(rsp);
    if (!put || !put->accepted || !data || len == 0 || len != put->len) {
        set_error(rsp, 400, "invalid_request");
        return;
    }
    if (!env_usable(env)) {
        set_not_ready(rsp);
        return;
    }
    if (!op_enter(env, rsp, false)) return;
    iso_upload_t *up = iso_core_upload(env->core);
    iso_upload_write_t out;
    memset(&out, 0, sizeof(out));
    int rc = iso_upload_write_chunk(up, put->id, put->offset, data, len, &out);
    if (rc == ISO_OK) {
        iso_w_t w;
        iso_w_init(&w, rsp->body, rsp->body_cap);
        iso_w_str(&w, "{\"received\":");
        iso_w_u64(&w, out.received);
        iso_w_str(&w, "}");
        rsp_finish(rsp, &w, 200, "ok");
    } else if (rc == ISO_RES_CHUNK_TOO_LARGE) {
        set_error(rsp, 413, "payload_too_large");
    } else if (rc == ISO_RES_NOT_FOUND) {
        set_error(rsp, 404, "not_found");
    } else if (rc == ISO_RES_NOT_UPLOADING) {
        set_error(rsp, 409, "not_uploading");
    } else if (rc == ISO_RES_UPLOAD_IN_PROGRESS) {
        set_error(rsp, 409, "upload_in_progress");
    } else if (rc == ISO_RES_OFFSET_MISMATCH) {
        set_error_u64(rsp, 409, "offset_mismatch", "received", out.received);
    } else if (rc == ISO_RES_EXCEEDS_SIZE || rc == ISO_ERR_INVALID) {
        set_error(rsp, 400, "invalid_request");
    } else if (rc == ISO_RES_INSUFFICIENT_STORAGE) {
        set_error_u64(rsp, 507, "insufficient_storage", "free_bytes", iso_upload_free_estimate(up));
    } else {
        fail_rc(rsp, rc, up);
    }
    op_leave(env);
}

/* ---- エントリ ------------------------------------------------------------------------------ */

void iso_api_handle(const iso_api_env_t *env, const iso_api_request_t *rq, iso_api_response_t *rsp) {
    if (!rsp) return;
    rsp_reset(rsp);
    if (!rq || !rq->path) {
        set_error(rsp, 500, "internal_error");
        return;
    }
    iso_api_route_t rt;
    if (!check_headers(rq, rsp, &rt)) {
        if (rq->method == ISO_API_M_PUT) rsp->close_connection = true; /* 本体を読まずに応答する */
        return;
    }
    const bool usable = env_usable(env);
    vctx_t vc = {0};
    iso_api_body_t b;

    switch (rt.ep) {
    case ISO_API_EP_LIST: {
        view_enter(env, usable, &vc);
        int rc = iso_api_format_list(vc.snap, &vc.ms, rsp->body, rsp->body_cap, &rsp->body_len);
        view_leave(env, usable);
        if (rc != 0) {
            set_error(rsp, 500, "internal_error");
        } else {
            rsp->status = 200;
        }
        return;
    }
    case ISO_API_EP_PUT:
        if (!put_precheck_routed(env, rq, &rt, rsp)) rsp->close_connection = true;
        return;
    case ISO_API_EP_BEGIN: {
        if (iso_api_parse_body(ISO_API_SCHEMA_BEGIN, rq->body, rq->body_len, &b) != 0) {
            set_error(rsp, 400, "invalid_request");
            return;
        }
        rsp->log_has_size = true;
        rsp->log_size = b.size;
        view_enter(env, usable, &vc);
        bool go = check_begin(&vc, &b, rsp);
        view_leave(env, usable);
        if (!proceed(go, usable, rsp)) return;
        exec_begin(env, &b, rsp);
        return;
    }
    case ISO_API_EP_MOUNT: {
        uint32_t id_u32 = 0;
        if (iso_api_parse_body(ISO_API_SCHEMA_MOUNT, rq->body, rq->body_len, &b) != 0 || !iso_api_id_to_u32(b.id, &id_u32)) {
            set_error(rsp, 400, "invalid_request");
            return;
        }
        set_log_id(rsp, b.id);
        view_enter(env, usable, &vc);
        bool go = check_mount(&vc, b.id, id_u32, b.force, rsp);
        view_leave(env, usable);
        if (!proceed(go, usable, rsp)) return;
        exec_mount(env, b.id, id_u32, b.force, rsp);
        return;
    }
    case ISO_API_EP_EJECT: {
        if (iso_api_parse_body(ISO_API_SCHEMA_EJECT, rq->body, rq->body_len, &b) != 0) {
            set_error(rsp, 400, "invalid_request");
            return;
        }
        view_enter(env, usable, &vc);
        bool go = check_eject(&vc, b.force, rsp);
        view_leave(env, usable);
        if (!proceed(go, usable, rsp)) return;
        exec_eject(env, b.force, rsp);
        return;
    }
    case ISO_API_EP_RESCAN: {
        if (iso_api_parse_body(ISO_API_SCHEMA_RESCAN, rq->body, rq->body_len, &b) != 0) {
            set_error(rsp, 400, "invalid_request");
            return;
        }
        view_enter(env, usable, &vc);
        bool go = check_rescan(&vc, rsp);
        view_leave(env, usable);
        if (!proceed(go, usable, rsp)) return;
        exec_rescan(env, b.cursor, rsp);
        return;
    }
    case ISO_API_EP_FINISH: {
        if (iso_api_parse_body(ISO_API_SCHEMA_FINISH, rq->body, rq->body_len, &b) != 0) {
            set_error(rsp, 400, "invalid_request");
            return;
        }
        view_enter(env, usable, &vc);
        bool go = check_finish(&vc, rt.id, rsp);
        view_leave(env, usable);
        if (!proceed(go, usable, rsp)) return;
        exec_finish(env, rt.id, rsp);
        return;
    }
    case ISO_API_EP_DELETE: {
        uint32_t id_u32 = 0;
        (void)iso_api_id_to_u32(rt.id, &id_u32);
        view_enter(env, usable, &vc);
        bool go = check_delete(&vc, rt.id, id_u32, rsp);
        view_leave(env, usable);
        if (!proceed(go, usable, rsp)) return;
        exec_delete(env, rt.id, rsp);
        return;
    }
    default:
        set_error(rsp, 500, "internal_error");
        return;
    }
}
