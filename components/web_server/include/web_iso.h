#pragma once

/*
 * web_iso: /api/v1/iso* の HTTP の結線(esp_http_server)。仕様は docs/WEB_API.md 8 章、純 C の中身は components/iso_api(iso_api.h)。
 * CONFIG_ISO_ENABLE が無効のビルドでは、このファイルをビルドせず、パスを登録しない(404)。
 *
 * このファイルの責任: 段 1・2(認証、Origin。http_guard。web_server.c の web_server_iso_guard)、ヘッダー(Content-Type、Content-Length、
 * X-Upload-Offset)と JSON の本体(512 バイトまで)の収集、iso_api への受け渡し、応答の送信(ステータス、Allow、Retry-After、Connection: close、ログ)。
 * 判定の順序(13 段)と、状態ごとの対応表は iso_api(ホストでテスト済み)。ここは esp_http_server の依存を最小にする。
 *
 * 担当の境界:
 *   - A-13 / A-14: SD と MSC の結線ができたら web_iso_set_env() で、iso_api_env_t(ロック、公開ビュー、iso_media、iso_core)を渡す。
 *     それまで(未起動): GET /api/v1/iso は sd.state = no_card の 200。**認証と形式の検査(段 1〜6)を通った POST /api/v1/iso/eject は、200 {"result":"not_mounted"}
 *     (例外)**。形式が不正な要求(404、405、411、413、415、400)は、サービスの状態を確認する前に拒否される。**503 not_ready は、先行の検査を通った要求のうち、
 *     起動済みのサービスを必要とする操作(begin、mount、finish、rescan、delete、PUT)に限る**。
 *   - A-15: PUT の本体の受信ループ(web_iso_set_put_hooks)。ISO_PUT_RECV_TIMEOUT_S 以内にチャンク全体をバッファに受信する。
 *     hooks が NULL の間、PUT は(判定を通っても)503 not_ready を返して接続を閉じる(TODO(A-15))。
 */

#include "sdkconfig.h"

#if CONFIG_ISO_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "iso_api.h"

/* PUT の 1 チャンクの受信の期限(秒)。Kconfig ISO_PUT_RECV_TIMEOUT_S。受信ループ(A-15)が使う。 */
#ifndef ISO_PUT_RECV_TIMEOUT_S
#define ISO_PUT_RECV_TIMEOUT_S CONFIG_ISO_PUT_RECV_TIMEOUT_S
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* /api/v1/iso と /api/v1/iso/ * を、HTTP_ANY で登録する(2 つ。認証の後の 404 / 405 は iso_api が決める)。
 * サーバーの設定は httpd_uri_match_wildcard(web_server.c が conf.httpd.uri_match_fn に入れる)と、max_uri_handlers の余裕(2)が要る。 */
esp_err_t web_iso_register(httpd_handle_t server);

/* SD と MSC の結線ができたとき(A-13 / A-14)に呼ぶ。env は、起動後ずっと有効なこと。NULL = 起動していない。 */
void web_iso_set_env(const iso_api_env_t *env);

/* PUT のチャンクの受信(A-15)。 */
typedef struct {
    void *ctx;
    /* req の本体の len バイトを、ISO_PUT_RECV_TIMEOUT_S 以内に、1 つのバッファに受信する。成功 = 0(*buf_out に、len バイトが入ったバッファ)。
     * 失敗(タイムアウト、切断、メモリ) = 負(何も書かず、接続を閉じる)。 */
    int (*recv_chunk)(void *ctx, httpd_req_t *req, size_t len, const uint8_t **buf_out);
    /* recv_chunk が返したバッファを返す(成功のときも、失敗のときも、必ず 1 回)。NULL でよい。 */
    void (*release_chunk)(void *ctx, const uint8_t *buf);
} web_iso_put_hooks_t;

void web_iso_set_put_hooks(const web_iso_put_hooks_t *hooks);

/* web_server.c が実装する: 段 1・2(Host、初期設定、認証、Origin)。許可なら true。拒否なら、エラーの応答を送った後で false
 * (呼び出し側は、何もせずに ESP_OK を返す)。mutating = POST / PUT / DELETE(Origin が必須)。close_on_deny = 拒否の応答に Connection: close を付ける(PUT)。 */
bool web_server_iso_guard(httpd_req_t *req, bool mutating, bool close_on_deny);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_ISO_ENABLE */
