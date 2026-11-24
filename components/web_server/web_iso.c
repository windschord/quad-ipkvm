#include "web_iso.h"

#if CONFIG_ISO_ENABLE

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "web_iso_put_logic.h"

/* /api/v1/iso* の HTTP の結線。docs/WEB_API.md 8 章。ここは薄く保ち、判定と対応表は iso_api(ホストでテスト済み)に任せる。 */

static const char *TAG = "web_iso";

static const iso_api_env_t *s_env;              /* NULL = 起動していない */
static const web_iso_put_hooks_t *s_put_hooks;  /* NULL = PUT の受信ループが未結線 */
static char s_prog_id[ISO_ID_LEN + 1];
static int s_prog_bucket = -1;

/* GET の応答の組み立て用。esp_http_server のハンドラーは 1 つのタスクで直列なので、静的でよい(スタックは 10 KiB しかない) */
static char s_rsp_body[ISO_API_RESPONSE_MAX];

void web_iso_set_env(const iso_api_env_t *env) { s_env = env; }
void web_iso_set_put_hooks(const web_iso_put_hooks_t *hooks) { s_put_hooks = hooks; }

static void log_put_progress(const iso_api_put_t *put) {
    if (!put || !s_env || !s_env->state_lock || !s_env->state_unlock || !s_env->snapshot) {
        return;
    }
    if (strcmp(s_prog_id, put->id) != 0) {
        memcpy(s_prog_id, put->id, sizeof(s_prog_id));
        s_prog_bucket = -1;
    }

    uint64_t received = 0;
    uint64_t total = 0;
    bool found = false;
    s_env->state_lock(s_env->ctx);
    const iso_view_t *v = &s_env->snapshot->view;
    const size_t n = (v->count > ISO_MAX_IMAGES) ? ISO_MAX_IMAGES : v->count;
    for (size_t i = 0; i < n; i++) {
        if (v->images[i].state != ISO_STATE_NONE && strcmp(v->images[i].id, put->id) == 0) {
            received = v->images[i].received;
            total = v->images[i].size_bytes;
            found = true;
            break;
        }
    }
    s_env->state_unlock(s_env->ctx);

    if (found && total > 0) {
        const int pct = web_iso_progress_step_pct(received, total, &s_prog_bucket);
        if (pct >= 0) {
            ESP_LOGI(TAG, "iso put id=%s progress=%d%% (%" PRIu64 "/%" PRIu64 " B)",
                     put->id, pct, received, total);
        }
    }
}

static const char *status_line(int status) {
    switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 411: return "411 Length Required";
    case 413: return "413 Payload Too Large";
    case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Entity";
    case 503: return "503 Service Unavailable";
    case 507: return "507 Insufficient Storage";
    default: return "500 Internal Server Error";
    }
}

static void close_after_response(httpd_req_t *req) {
    /* 残りの本体を読み捨てて、単一スレッドの httpd が止まるのを避ける(設計 6.1)。閉じるのは、ハンドラーが ESP_FAIL を返すことで行う(send_rsp)。
     * httpd_sess_trigger_close は呼ばない: ESP_FAIL で先にセッションが消えると、積んだ close が、同じスロットを再利用した別の新しい接続を閉じる
     * (ESP-IDF v6.0.1 の httpd_sess_close は、lru_counter が 0 でないスロットを閉じる)。
     * TODO(A-15 / hw-verify): ESP_FAIL と Connection: close で、実際にソケットが閉じることを、最初に確かめる。 */
    (void)httpd_resp_set_hdr(req, "Connection", "close");
}

/* 応答を送る(ログは、名前・セッション情報・データを含めない。イメージの id と大きさだけ) */
static esp_err_t send_rsp(httpd_req_t *req, const iso_api_response_t *rsp) {
    if (rsp->log) {
        if (rsp->log_has_size) {
            ESP_LOGI(TAG, "iso %s id=%s size=%" PRIu64 " -> %d %s", rsp->log_kind, rsp->log_id[0] ? rsp->log_id : "-", rsp->log_size, rsp->status,
                     rsp->log_reason);
        } else {
            ESP_LOGI(TAG, "iso %s id=%s -> %d %s", rsp->log_kind, rsp->log_id[0] ? rsp->log_id : "-", rsp->status, rsp->log_reason);
        }
    }
    char retry[12];
    if (rsp->has_retry_after) {
        snprintf(retry, sizeof(retry), "%" PRIu32, rsp->retry_after_s);
        (void)httpd_resp_set_hdr(req, "Retry-After", retry);
    }
    if (rsp->allow) (void)httpd_resp_set_hdr(req, "Allow", rsp->allow);
    if (rsp->close_connection) close_after_response(req);
    (void)httpd_resp_set_status(req, status_line(rsp->status));
    (void)httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, rsp->body, (ssize_t)rsp->body_len);
    /* 拒否(close_connection)のときは、ESP_FAIL を返す: esp_http_server は、ESP_OK のハンドラーの後で、未受信の本文を読み捨てる。低速な送信側が、本文を送り続けると、単一スレッドの httpd が受信待ちを続ける。ESP_FAIL なら、すぐ閉じる。 */
    return rsp->close_connection ? ESP_FAIL : err;
}

static iso_api_method_t map_method(int m) {
    switch (m) {
    case HTTP_GET: return ISO_API_M_GET;
    case HTTP_POST: return ISO_API_M_POST;
    case HTTP_PUT: return ISO_API_M_PUT;
    case HTTP_DELETE: return ISO_API_M_DELETE;
    default: return ISO_API_M_OTHER;
    }
}

/* 前後の空白(OWS)を取り除く(ヘッダーの値の末尾の空白) */
static void trim_trailing_ows(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/* JSON の本体を読む(len <= ISO_API_BODY_MAX。段 5 が保証する)。失敗(タイムアウト、切断)なら false */
static bool read_body(httpd_req_t *req, char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) (void)httpd_resp_send_408(req);
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

static esp_err_t iso_handler(httpd_req_t *req) {
    const iso_api_method_t method = map_method(req->method);
    const bool mutating = (method == ISO_API_M_POST || method == ISO_API_M_PUT || method == ISO_API_M_DELETE);
    const bool is_put = (method == ISO_API_M_PUT);
    const bool close_on_deny = is_put || (req->content_len > ISO_API_BODY_MAX);

    /* 段 1・2: 認証と Origin(http_guard)。PUT と過大な未読ボディの拒否は、本体を読まずに閉じる */
    if (!web_server_iso_guard(req, mutating, close_on_deny)) return close_on_deny ? ESP_FAIL : ESP_OK;

    /* ヘッダーの収集 */
    char ctype[64];
    char offset[24];
    /* 戻り値を、完全な値(ESP_OK)/ ない(ESP_ERR_NOT_FOUND)/ それ以外(領域が足りず、値が切り詰められた ESP_ERR_HTTPD_RESULT_TRUNC を含む)に分ける。
     * 切り詰めた値は、検査に渡さない(iso_api_hdr_value が、どの検査にも合わない印を返す。Content-Type は 415、X-Upload-Offset は 400)。 */
    esp_err_t e_ct = httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    esp_err_t e_off = httpd_req_get_hdr_value_str(req, "X-Upload-Offset", offset, sizeof(offset));
    const iso_api_hdr_status_t st_ct = iso_api_hdr_status(e_ct == ESP_OK, e_ct == ESP_ERR_NOT_FOUND);
    const iso_api_hdr_status_t st_off = iso_api_hdr_status(e_off == ESP_OK, e_off == ESP_ERR_NOT_FOUND);
    if (st_off == ISO_API_HDR_OK) trim_trailing_ows(offset);

    iso_api_request_t rq;
    memset(&rq, 0, sizeof(rq));
    rq.method = method;
    rq.path = req->uri; /* iso_api_route が、`?` 以降を見ない */
    rq.content_type = iso_api_hdr_value(st_ct, ctype);
    rq.has_content_length = (httpd_req_get_hdr_value_len(req, "Content-Length") > 0);
    rq.content_length = (uint64_t)req->content_len;
    rq.upload_offset = iso_api_hdr_value(st_off, offset);

    iso_api_response_t rsp;
    iso_api_response_init(&rsp, s_rsp_body, sizeof(s_rsp_body));

    if (is_put) {
        /* 本体を読む前の判定(段 3〜12)。拒否は、本体を読まずに応答して、Connection: close */
        if (!iso_api_put_precheck(s_env, &rq, &rsp)) return send_rsp(req, &rsp);
        const iso_api_put_t put = rsp.put;
        if (!s_put_hooks || !s_put_hooks->recv_chunk) { /* TODO(A-15): 受信ループ(ISO_PUT_RECV_TIMEOUT_S 以内にチャンク全体をバッファへ) */
            static const char NOT_READY[] = "{\"error\":\"not_ready\"}";
            iso_api_response_init(&rsp, s_rsp_body, sizeof(s_rsp_body));
            memcpy(rsp.body, NOT_READY, sizeof(NOT_READY));
            rsp.body_len = sizeof(NOT_READY) - 1;
            rsp.status = 503;
            rsp.close_connection = true; /* 本体を読んでいない */
            return send_rsp(req, &rsp);
        }
        const uint8_t *chunk = NULL;
        int rc = s_put_hooks->recv_chunk(s_put_hooks->ctx, req, put.len, &chunk);
        if (rc < 0) { /* タイムアウト、切断: 何も書かずに、接続を閉じる */
            if (s_put_hooks->release_chunk) s_put_hooks->release_chunk(s_put_hooks->ctx, chunk);
            return ESP_FAIL; /* ESP_FAIL で、httpd が閉じる(httpd_sess_trigger_close は呼ばない。close_after_response を参照) */
        }
        iso_api_response_init(&rsp, s_rsp_body, sizeof(s_rsp_body));
        iso_api_put_commit(s_env, &put, chunk, put.len, &rsp);
        if (s_put_hooks->release_chunk) s_put_hooks->release_chunk(s_put_hooks->ctx, chunk);
        if (rsp.status == 200) log_put_progress(&put);
        return send_rsp(req, &rsp);
    }

    /* 段 3〜5(本体を読む前) */
    if (!iso_api_headers_check(&rq, &rsp)) {
        if (req->content_len > ISO_API_BODY_MAX) rsp.close_connection = true;
        return send_rsp(req, &rsp);
    }

    /* JSON のエンドポイントの本体(512 バイトまで。段 5 が保証した) */
    char body[ISO_API_BODY_MAX];
    iso_api_route_t rt;
    iso_api_route(method, req->uri, &rt);
    if (iso_api_ep_has_body(rt.ep) && req->content_len > 0) {
        if (!read_body(req, body, req->content_len)) return ESP_FAIL;
        rq.body = body;
        rq.body_len = req->content_len;
    }

    iso_api_handle(s_env, &rq, &rsp);
    /* 小さな GET / DELETE の未読本体は、ESP_OK の返却後に ESP-IDF v6.0.1 の httpd_req_delete() が読み捨てる。
     * ISO_API_BODY_MAX を超える未読本体は、単一スレッドの httpd を止めないよう接続を閉じる。 */
    if (!iso_api_ep_has_body(rt.ep) && req->content_len > ISO_API_BODY_MAX) {
        rsp.close_connection = true;
    }
    return send_rsp(req, &rsp);
}

esp_err_t web_iso_register(httpd_handle_t server) {
    /* すべてのメソッドを、このハンドラーへ渡す(HTTP_ANY)。esp_http_server は、URI が一致してもメソッドが一致しないと、ハンドラーを呼ばずに 405 にする
     * ので、メソッドごとの登録では、未認証の PATCH などが認証(web_server_iso_guard)に届かず、経路の存在が分かる。認証の後の 404 / 405 は、iso_api が決める。 */
    static const char *const URIS[] = {"/api/v1/iso", "/api/v1/iso/*"};
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        httpd_uri_t u = {.uri = URIS[i], .method = HTTP_ANY, .handler = iso_handler, .user_ctx = NULL};
        esp_err_t err = httpd_register_uri_handler(server, &u);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s: %s", URIS[i], esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

#endif /* CONFIG_ISO_ENABLE */
