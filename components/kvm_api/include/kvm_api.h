#ifndef KVM_API_H
#define KVM_API_H

/*
 * kvm_api: KVM 切替・ATX の HTTP API の中身。純 C、IDF 非依存。仕様は docs/WEB_API.md 7 章（これが正）、
 * 層の位置づけは docs/INTERFACES.md 6.1。
 *
 * 役割
 *   - ボディ({"channel":N})の厳密な解析、状態(7.1)と応答(7.2/7.3)の JSON の生成、
 *     kvm_ctrl_result_t -> HTTP のステータス・エラーコード・Retry-After の対応。
 *   - 全体を kvm_api_handle() 1 つで扱う。kvm_ctrl には kvm_api_backend_t（関数テーブル）越しに触れる。
 *     IDF では kvm_service が、ミューテックスで直列化した kvm_ctrl を backend として渡す。ホストのテストはモックを渡す。
 *
 * 認証・Origin・Content-Type・Content-Length(411/413) は、この層の「前」（web_server + http_guard_decide）の責任。
 * kvm_api_handle() は、それらを通った後にだけ呼ばれる。ここでは再検査しない（ボディが 64 バイトを超えるときは、
 * 防御として 400 invalid_request を返す）。
 *
 * ログ
 *   ログの出力は IDF 側（web_server）。ここは「ログすべき内容」だけを response に返す。
 *   種類(log_kind)、チャンネル(log_channel、無ければ 0)、理由(log_reason)の 3 つだけで、
 *   ボディの生データやセッション情報は含まない。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kvm_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* リクエストのボディの上限（WEB_API.md 7 章: 64 バイト以下。超過は web_server が 413） */
#define KVM_API_BODY_MAX 64
/* 応答のボディのバッファ。最長の状態 JSON（7.1）は約 240 バイト */
#define KVM_API_RESPONSE_MAX 320

typedef enum {
    KVM_API_EP_STATUS = 0,      /* GET  /api/v1/kvm/status */
    KVM_API_EP_SELECT,          /* POST /api/v1/kvm/select */
    KVM_API_EP_POWER_PRESS,     /* POST /api/v1/power/press */
    KVM_API_EP_POWER_RESET,     /* POST /api/v1/power/reset */
    KVM_API_EP_POWER_FORCE      /* POST /api/v1/power/force */
} kvm_api_endpoint_t;

/*
 * kvm_ctrl へのアクセス。ctx は各関数の第 1 引数。
 *   - lock は、kvm_ctrl を直列化するミューテックスを取る。**短い上限つきで待ち**、取れなければ false
 *     （HTTP のハンドラを長くブロックしない。取れなかったときは 503 not_ready）。
 *   - get_status / select / power は lock を持っている間だけ呼ぶ。unlock は lock が true のときだけ、必ず 1 回呼ぶ
 *     （エラーの経路でも）。
 *   - ready は、サービスが起動済みかを返す（ロックしない。軽い読み取りだけ）。
 * どれかの関数ポインタが NULL の backend は、使えない（not_ready）。
 */
typedef struct {
    void *ctx;
    bool (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    bool (*ready)(void *ctx);
    kvm_ctrl_result_t (*get_status)(void *ctx, kvm_ctrl_status_t *status);
    kvm_ctrl_result_t (*select)(void *ctx, uint8_t channel, uint32_t *op_id);
    kvm_ctrl_result_t (*power)(void *ctx, uint8_t channel, kvm_ctrl_power_action_t action, uint32_t *op_id);
} kvm_api_backend_t;

typedef struct {
    int status;                         /* HTTP ステータス: 200 / 202 / 400 / 409 / 429 / 500 / 503 */
    char body[KVM_API_RESPONSE_MAX];    /* JSON（NUL 終端）。Content-Type は application/json */
    size_t body_len;                    /* NUL を含まない長さ */
    bool has_retry_after;               /* 429 cooldown のとき true */
    uint32_t retry_after_s;             /* Retry-After の秒（残りを切り上げ、最小 1） */

    /* ログすべき内容（status の成功は false: ポーリングで溢れるため）。web_server が ESP_LOGI で出す */
    bool log;
    const char *log_kind;               /* "status" / "select" / "power_press" / "power_reset" / "power_force"（静的文字列） */
    uint8_t log_channel;                /* 1..4。ボディが不正で分からないときは 0 */
    const char *log_reason;             /* "accepted" / "already_selected" / "invalid_request" / "busy" / ...（静的文字列） */
} kvm_api_response_t;

/*
 * 1 リクエストを処理する。
 *   backend: kvm_service の backend。NULL（サービス未起動）なら、ボディが正しければ 503 not_ready。
 *   body / body_len: select と power のリクエストボディ（status では無視）。
 *   判定の順序（WEB_API.md 7.3。認証・Origin は呼び出し前に済んでいる）:
 *     形式 -> channel の範囲 -> サービス未起動/ロック不可/fault(503) -> unknown_state -> busy
 *     -> channel_mismatch -> atx_locked -> cooldown -> 受理
 *     （形式と範囲は kvm_api、それ以降は kvm_ctrl の判定をそのまま対応づける）
 * response は必ず埋まる（NULL なら何もしない）。
 */
void kvm_api_handle(const kvm_api_backend_t *backend, kvm_api_endpoint_t endpoint,
                    const char *body, size_t body_len, kvm_api_response_t *response);

/* ---- 部品（テスト用に公開） ---- */

/*
 * {"channel":N} だけを受け付ける厳密な解析。N は 1..4 の 1 桁の整数。
 * 空白(' ' \t \n \r)は記号の前後に許す。ほかのキー、重複キー、キーのエスケープ、文字列・小数・指数・負数・
 * 先頭ゼロ・2 桁以上、"}" の後の余計な文字（空白以外）、NUL を含む、空、64 バイト超は不正。
 * @return 0: 成功（*channel = 1..4）、-1: 不正（*channel は変更しない）。
 */
int kvm_api_parse_channel(const char *body, size_t body_len, uint8_t *channel);

/*
 * 状態(7.1)の JSON を作る。@return 0: 成功（NUL 終端。*out_len は NUL を含まない長さ）、
 * -1: バッファ不足・引数が NULL・列挙値が範囲外（out は空文字列になる）。
 */
int kvm_api_format_status(const kvm_ctrl_status_t *status, char *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* KVM_API_H */
