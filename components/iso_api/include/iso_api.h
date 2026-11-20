#ifndef ISO_API_H
#define ISO_API_H

/*
 * iso_api: ISO の仮想メディアの HTTP API(/api/v1/iso*)の中身。純 C、IDF 非依存。仕様は docs/WEB_API.md 8 章(これが正)、
 * 設計は docs/ISO_MOUNT_DESIGN.md 7 / 9、境界は docs/INTERFACES.md 7.4。
 *
 * 役割
 *   - 経路の振り分け(定義していないメソッドは 405 + Allow、`/api/v1/iso/<id>` の id の形式不正は 404)。
 *   - JSON のボディの厳密な解析(`\u` エスケープ、サロゲートペア、名前の規則)。
 *   - **判定の順序(WEB_API.md 8 章の 13 段)**。段 1(認証)と段 2(Origin)は、この層の「前」(web_server の http_guard)の責任。
 *     この層は段 3〜13 を行う。段 7〜12 は公開ビュー(iso_api_snapshot_t)と iso_media の状態から判定し(iso_state_mutex の下。I/O なし)、
 *     段 13(処理)は iso_op_mutex を try-lock で取って、iso_core / iso_upload / iso_catalog / iso_media を呼ぶ。
 *   - 状態ごとのエラーの対応表(400 / 404 / 405 / 409 / 411 / 413 / 415 / 422 / 503 / 507)。
 *   - GET /api/v1/iso の応答の生成。
 *
 * PUT(チャンクの送信)は 2 つに分ける(INTERFACES.md 7.4):
 *   iso_api_put_precheck()  本体を読む前の判定(段 3〜12。507 を含む)。拒否なら rsp が埋まり、rsp->close_connection が真(グルーは
 *                            本体を読まずに応答し、Connection: close を付ける)。受理なら true で、rsp->put に id / offset / len が入る。
 *   iso_api_put_commit()    本体の全体をバッファに受信した後(ISO_PUT_RECV_TIMEOUT_S 以内。グルー = A-15)に呼ぶ。iso_op_mutex を try-lock で取り
 *                            (取れなければ 503 busy)、iso_upload_write_chunk を呼んで、その結果を応答に直す(状態は、ここでもう一度検査される)。
 *
 * ロック(ISO_MOUNT_DESIGN.md 4.1。この層が env のコールバックで取る。呼び出し側(グルー)は、これらのロックを持ったまま呼ばない):
 *   state_lock(iso_state_mutex): 短い区間だけ。ビューとメディアの状態を読む / iso_media を操作する。I/O、op_trylock を呼ばない。
 *   op_trylock(iso_op_mutex): wait_ms 以内に取れなければ false。取れたら、iso_core / iso_upload / iso_catalog を呼び、I/O をする。
 *     取った後に state_lock を取るのはよい(1 → 2)。逆はしない。
 *
 * ログ
 *   ログの出力は web_server 側。ここは「ログすべき内容」だけを rsp に返す(種類、結果の理由、イメージの id と大きさ)。
 *   名前、セッション情報、データは含まない。GET と PUT は記録しない(rsp->log が偽)。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iso_seams.h"
#include "iso_catalog.h"
#include "iso_upload.h"
#include "iso_core.h"
#include "iso_media.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JSON のリクエストのボディの上限(WEB_API.md 8 章。超過は 413 payload_too_large) */
#define ISO_API_BODY_MAX 512u
/* GET /api/v1/iso の応答の最大(16 イメージ、名前 63 バイトがすべて `"`、すべてのフィールドが最大のとき。ホストテストで確かめる) */
#define ISO_API_RESPONSE_MAX 8192u
/* 経路の接頭辞 */
#define ISO_API_PATH_PREFIX "/api/v1/iso"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h" /* IDF のビルドでは、Kconfig(CONFIG_ISO_OP_LOCK_WAIT_MS)を既定値にする。ホストのビルドでは、下の既定値(20) */
#endif
#ifndef ISO_OP_LOCK_WAIT_MS
#ifdef CONFIG_ISO_OP_LOCK_WAIT_MS
#define ISO_OP_LOCK_WAIT_MS ((uint32_t)CONFIG_ISO_OP_LOCK_WAIT_MS) /* Kconfig ISO_OP_LOCK_WAIT_MS */
#else
#define ISO_OP_LOCK_WAIT_MS 20u
#endif
#endif /* iso_op_mutex を待つ上限(これを超えたら 503 busy)。iso_api_env_t の op_lock_wait_ms が 0 のときの値 */

/* ---- 要求 ----------------------------------------------------------------------- */

typedef enum {
    ISO_API_M_GET = 0,
    ISO_API_M_POST,
    ISO_API_M_PUT,
    ISO_API_M_DELETE,
    ISO_API_M_OTHER
} iso_api_method_t;

typedef enum {
    ISO_API_EP_NONE = 0,
    ISO_API_EP_LIST,   /* GET    /api/v1/iso */
    ISO_API_EP_BEGIN,  /* POST   /api/v1/iso */
    ISO_API_EP_MOUNT,  /* POST   /api/v1/iso/mount */
    ISO_API_EP_EJECT,  /* POST   /api/v1/iso/eject */
    ISO_API_EP_RESCAN, /* POST   /api/v1/iso/rescan */
    ISO_API_EP_PUT,    /* PUT    /api/v1/iso/<id> */
    ISO_API_EP_FINISH, /* POST   /api/v1/iso/<id>/finish */
    ISO_API_EP_DELETE  /* DELETE /api/v1/iso/<id> */
} iso_api_endpoint_t;

typedef enum {
    ISO_API_ROUTE_NOT_FOUND = 0, /* 404 not_found(定義していない経路) */
    ISO_API_ROUTE_METHOD,        /* 405 method_not_allowed(allow を付ける) */
    ISO_API_ROUTE_OK
} iso_api_route_result_t;

typedef struct {
    iso_api_route_result_t result;
    iso_api_endpoint_t ep;       /* result == OK のとき */
    bool has_id;                 /* ep が <id> の経路(PUT / FINISH / DELETE) */
    bool id_ok;                  /* has_id のとき、<id> の形式が正しい(8 桁の小文字の 16 進) */
    char id[ISO_ID_LEN + 1];     /* id_ok のとき */
    const char *allow;           /* result == METHOD のとき(静的文字列。例 "GET, POST") */
} iso_api_route_t;

/* path(`?` 以降は見ない)と method から、経路を決める。method が OTHER なら、必ず METHOD(定義した経路)または NOT_FOUND。 */
void iso_api_route(iso_api_method_t method, const char *path, iso_api_route_t *out);
/* 405 の Allow ヘッダー: path が /api/v1/iso* の定義した経路なら、その Allow(静的文字列)。そうでなければ NULL。 */
const char *iso_api_allow_for_path(const char *path);

typedef enum {
    ISO_API_CT_NONE = 0, /* ヘッダーなし */
    ISO_API_CT_JSON,     /* application/json(パラメーターは許す) */
    ISO_API_CT_OCTET,    /* application/octet-stream */
    ISO_API_CT_OTHER
} iso_api_ctype_t;

/* Content-Type ヘッダーの値(NULL = なし)を分類する。大文字小文字を区別しない。メディアタイプの後は、終わりか、空白を挟んだ ';' だけ。 */
iso_api_ctype_t iso_api_ctype_parse(const char *value);

/*
 * 段 1・2(認証、Origin)を通った要求。グルーが、ヘッダーから集めて渡す。
 *   path: 要求の経路(クエリは見ない)。NUL 終端。
 *   content_type: Content-Type ヘッダーの値(なければ NULL)。
 *   has_content_length / content_length: Content-Length ヘッダーがあるか、その値。
 *   upload_offset: X-Upload-Offset ヘッダーの値(PUT。なければ NULL。前後の空白は、グルーが取り除く)。
 *   body / body_len: JSON のボディ(読み終えたもの。content_length バイト)。ボディがなければ、body_len = 0。
 *     グルーの手順: 段 1・2 → iso_api_headers_check()(段 3〜5。拒否なら、本体を読まずに、その応答を返す)→ JSON のエンドポイント
 *     (iso_api_ep_has_body)で content_length > 0 なら、本体を読む(段 5 が ISO_API_BODY_MAX 以下を保証する)→ iso_api_handle()。
 *     PUT は、iso_api_put_precheck() → 本体の受信 → iso_api_put_commit()。
 */
typedef struct {
    iso_api_method_t method;
    const char *path;
    const char *content_type;
    bool has_content_length;
    uint64_t content_length;
    const char *upload_offset;
    const char *body;
    size_t body_len;
} iso_api_request_t;

/* ---- 応答 ----------------------------------------------------------------------- */

/*
 * iso_api_put_precheck が受理した結果。iso_api_put_commit に、そのまま渡す(変更しない)。
 * has_epoch / epoch: **受理したときのカードの世代**(iso_blockdev_t.epoch。env->core の cfg.bdev から読む。bdev がなければ has_epoch = false)。
 * 本体の受信(ISO_PUT_RECV_TIMEOUT_S まで)の間に SD が替わると、新しいカードに同じ id の incomplete のイメージがあり、received が offset と一致する、
 * ということが起きうる。iso_api_put_commit は、書く前に現在の世代と比べ、違えば書かずに 503 sd_unavailable にする(同じカードでの再開は、従来どおり受理する)。
 */
typedef struct {
    bool accepted;
    char id[ISO_ID_LEN + 1];
    uint64_t offset;
    uint32_t len;
    bool has_epoch;
    uint32_t epoch;
} iso_api_put_t;

typedef struct {
    int status;                  /* HTTP ステータス: 200 / 201 / 202 / 400 / 404 / 405 / 409 / 411 / 413 / 415 / 422 / 500 / 503 / 507。PUT の受理(本体の待ち)は 0 */
    char *body;                  /* JSON(NUL 終端)。Content-Type は application/json。呼び出し側が確保する */
    size_t body_cap;
    size_t body_len;             /* NUL を含まない長さ */
    bool close_connection;       /* Connection: close を付ける(PUT の拒否。本体を読まずに応答する) */
    const char *allow;           /* 405 のとき(静的文字列) */
    bool has_retry_after;        /* 503 busy のとき */
    uint32_t retry_after_s;
    iso_api_put_t put;           /* iso_api_put_precheck が受理したとき(status は 0) */

    /* ログすべき内容(web_server が ESP_LOGI で出す。GET と PUT は log = false)。名前、セッション情報、データは含まない */
    bool log;
    const char *log_kind;        /* "begin" / "finish" / "mount" / "eject" / "delete" / "rescan"(静的文字列) */
    const char *log_reason;      /* "ok"、結果、またはエラーコード(静的文字列) */
    char log_id[ISO_ID_LEN + 1]; /* イメージの id。なければ "" */
    bool log_has_size;
    uint64_t log_size;           /* begin の size */
} iso_api_response_t;

/*
 * buf / cap は、本体の置き場(ISO_API_RESPONSE_MAX バイト以上を勧める)。
 * 本体が cap に入らないとき、応答は 500 `{"error":"internal_error"}` の短い本体になる。**これが入る最小の cap は 27 バイト**(26 文字 + 終端の NUL)。
 * **cap が 27 未満(0 を含む)だと、この短い本体も入らず、status = 500、body_len = 0、body は空の文字列(cap が 1 以上のとき。0 や NULL のときは書かない)**。
 * グルーは、ISO_API_RESPONSE_MAX を渡すこと(これ未満の cap は、テスト用)。
 */
void iso_api_response_init(iso_api_response_t *rsp, char *buf, size_t cap);

/* ---- 公開ビュー(GET と段 7〜12 の判定の元) ----------------------------------------------- */

/*
 * グルーが持つ、公開ビューのコピー。iso_core の hooks.view_update(iso_op_mutex の下で呼ばれる)の中で、iso_state_mutex の下で
 * iso_api_snapshot_update() を呼んで更新する。読む側(iso_api)は iso_state_mutex の下で読む。
 * iso_view_t の images[].table(コールバックの間だけ有効なポインター)は、コピーで NULL にする。
 * consumed_since は、空き容量の推定(iso_upload_free_estimate と同じ式)に要る。
 */
typedef struct {
    iso_view_t view;
    uint64_t consumed_since;
} iso_api_snapshot_t;

/* v をコピーする(count を ISO_MAX_IMAGES で頭打ち、table を NULL に)。up は iso_core_upload(core)(NULL なら consumed_since = 0)。 */
void iso_api_snapshot_update(iso_api_snapshot_t *s, const iso_view_t *v, const iso_upload_t *up);

/* ---- 環境(グルーが渡す) ---------------------------------------------------------------- */

typedef struct {
    void *ctx;
    void (*state_lock)(void *ctx);
    void (*state_unlock)(void *ctx);
    /* iso_op_mutex を、wait_ms 以内に取る。取れたら true。 */
    bool (*op_trylock)(void *ctx, uint32_t wait_ms);
    void (*op_unlock)(void *ctx);
    uint32_t op_lock_wait_ms;                  /* 0 = ISO_OP_LOCK_WAIT_MS */
    const iso_api_snapshot_t *snapshot;        /* state_lock の下で読む */
    iso_media_t *media;                        /* state_lock の下で読み書きする */
    iso_core_t *core;                          /* op_trylock の下で使う */
} iso_api_env_t;

/*
 * 要求を処理する(PUT 以外の全部。PUT は、段 3〜12 だけを行って、受理なら rsp->put.accepted = true、status = 0 で返す。
 * その後のグルーの仕事は、iso_api_put_commit の説明)。
 *   env: NULL、または必須のメンバーが NULL なら、「サービスが起動していない」(SD と MSC の実装が、まだ結線されていない):
 *        GET は sd.state = no_card の応答(200)、ほかは、形式の検査(段 3〜6)の後で 503 not_ready(eject は 200 not_mounted)。
 * rsp は必ず埋まる(NULL なら何もしない)。
 */
void iso_api_handle(const iso_api_env_t *env, const iso_api_request_t *rq, iso_api_response_t *rsp);

/* 段 3〜5(経路、id の形式、Content-Type、大きさ)だけ。本体を読む前に呼ぶ。真 = 続けてよい。偽 = rsp に応答が入る(本体は読まない)。
 * PUT は、chunk_size との比較に env が要るので、ここでは大きさを見ない(iso_api_put_precheck が行う)。 */
bool iso_api_headers_check(const iso_api_request_t *rq, iso_api_response_t *rsp);
/* ep が JSON のボディを取るか(begin / mount / finish / eject / rescan)。 */
bool iso_api_ep_has_body(iso_api_endpoint_t ep);

/* PUT の、本体を読む前の判定(段 3〜12)。真 = 受理(rsp->put に値が入る。status = 0)。偽 = rsp に応答が入る(close_connection = true)。 */
bool iso_api_put_precheck(const iso_api_env_t *env, const iso_api_request_t *rq, iso_api_response_t *rsp);

/*
 * PUT の本体を全部受信した後(グルーは ISO_PUT_RECV_TIMEOUT_S 以内に受信できなければ、何も書かずに接続を閉じる)。
 * data / len: チャンクの全体(len == put->len)。応答は rsp(200 {"received":N}、または対応する拒否)。
 */
void iso_api_put_commit(const iso_api_env_t *env, const iso_api_put_t *put, const void *data, size_t len, iso_api_response_t *rsp);

/* ---- ヘッダーの取得結果(グルーの esp_http_server の戻り値を、検査に渡す値にする。ホストでテストする) ----------------------- */

typedef enum {
    ISO_API_HDR_OK = 0,   /* 完全な値が取れた */
    ISO_API_HDR_ABSENT,   /* ヘッダーがない(ESP_ERR_NOT_FOUND) */
    ISO_API_HDR_INVALID   /* 取得に失敗した、または領域が足りず、値が切り詰められた(ESP_ERR_HTTPD_RESULT_TRUNC など) */
} iso_api_hdr_status_t;

/* esp_http_server の httpd_req_get_hdr_value_str の戻り値から: ok = (戻り値 == ESP_OK)、absent = (戻り値 == ESP_ERR_NOT_FOUND)。 */
iso_api_hdr_status_t iso_api_hdr_status(bool ok, bool absent);

/* 検査に渡すヘッダーの値の文字列。OK = value、ABSENT = NULL、INVALID = ISO_API_HDR_INVALID_VALUE(制御文字で始まる、どの検査にも合わない文字列:
 * Content-Type は ISO_API_CT_OTHER = 415、X-Upload-Offset は不正な数 = 400)。切り詰めた値を、検査に渡さない。 */
#define ISO_API_HDR_INVALID_VALUE "\x01invalid-header"
const char *iso_api_hdr_value(iso_api_hdr_status_t st, const char *value);

/* ---- 部品(テスト用に公開) ----------------------------------------------------------------- */

/* 厳密な 10 進の u64(符号、空白、先頭ゼロ(0 以外)、空、20 桁超、u64 の超過を拒否)。0 = 成功、-1 = 不正。 */
int iso_api_parse_u64(const char *s, size_t len, uint64_t *out);

/* id(8 桁の小文字の 16 進)と uint32_t の変換(iso_media の id)。形式が不正なら false。 */
bool iso_api_id_to_u32(const char *id, uint32_t *out);
void iso_api_u32_to_id(uint32_t v, char out[ISO_ID_LEN + 1]);

#ifdef __cplusplus
}
#endif

#endif /* ISO_API_H */
