# HTTP Guard 仕様書

`http_guard` コンポーネントは、Quad-IPKVM のネットワーク通信において、悪意のある入力や不正な状態遷移を防ぐための純粋な判定ロジック層です。

## 1. 認可判定表 (`http_guard_decide`)

システムの認証状態 (`NEEDS_SETUP` / `READY`) とリクエストの特性に基づき、どのアクションをとるかを決定します。
前提として、事前に `Host` ヘッダが `http_guard_host_ok` を満たす必要があります。満たさない場合はすべて `403_HOST` となります。

### 1.1 状態: `NEEDS_SETUP` (初期設定待ち)

| Path Category | Method | Origin OK | is_json | セッション有無 | 判定結果 | 理由 |
|---|---|---|---|---|---|---|
| `STATUS` | `GET` | - | - | - | `ALLOW` | 状態確認は常に許可 |
| `STATUS` | `GET`以外 | - | - | - | `405` | 状態確認は GET のみ許可 |
| `SETUP` | `POST` | YES | YES | - | `ALLOW` | 初期パスワード設定を許可 |
| `SETUP` | `POST` | NO | - | - | `403_ORIGIN` | CSRF 防御 (Origin 不一致) |
| `SETUP` | `POST` | YES | NO | - | `415` | Content-Type が JSON ではない |
| `SETUP` | `GET`/他 | - | - | - | `405` | 設定は POST のみ許可 |
| その他全て | 任意 | - | - | - | `403_SETUP_REQUIRED` | 設定が完了するまで他の API は利用不可 |

### 1.2 状態: `READY` (通常運用)

| Path Category | Method | Origin OK | is_json | セッション有無 | 判定結果 | 理由 |
|---|---|---|---|---|---|---|
| `STATUS` | `GET` | - | - | - | `ALLOW` | 状態確認は常に許可 |
| `STATUS` | `GET`以外 | - | - | - | `405` | 状態確認は GET のみ許可 |
| `SETUP` | 任意 | - | - | - | `409_ALREADY_SETUP` | 設定済みの場合は設定 API へのアクセスを拒否 |
| `LOGIN` | `POST` | YES | YES | - | `ALLOW` | ログインを許可 |
| `LOGIN` | `POST` | NO | - | - | `403_ORIGIN` | CSRF 防御 (Origin 不一致) |
| `LOGIN` | `POST` | YES | NO | - | `415` | Content-Type が JSON ではない |
| `LOGIN` | `GET`/他 | - | - | - | `405` | ログインは POST のみ許可 |
| `LOGOUT`| `POST` | YES | - | YES | `ALLOW` | ログアウトを許可 |
| `LOGOUT`| `POST` | NO | - | YES | `403_ORIGIN` | CSRF 防御 |
| `LOGOUT`| `GET`/他 | - | - | YES | `405` | ログアウトは POST のみ許可 |
| `LOGOUT`| 任意 | - | - | NO | `401` | セッションがない場合はログアウト不可 |
| `WS_VIDEO`| `GET` | YES | - | YES | `ALLOW` | WebSocket 接続許可 (GET だけど Origin 検査必須) |
| `WS_VIDEO`| `GET` | NO | - | YES | `403_ORIGIN` | WebSocket の CSRF (CSWSH) 防御 |
| `WS_VIDEO`| `GET`以外 | - | - | YES | `405` | WebSocket は GET のみ |
| `WS_VIDEO`| 任意 | - | - | NO | `401` | 未認証アクセスの拒否 |
| `KVM_STATUS` | `GET` | - | - | YES | `ALLOW` | KVM の状態取得（Origin・JSON は不要。docs/WEB_API.md 7） |
| `KVM_STATUS` | `GET`以外 | - | - | YES | `405` | GET のみ |
| `KVM_SELECT` / `POWER` | `POST` | YES | YES | YES | `ALLOW` | 切替・電源操作（ハードウェアを動かす） |
| `KVM_SELECT` / `POWER` | `POST` | NO | - | YES | `403_ORIGIN` | CSRF 防御 |
| `KVM_SELECT` / `POWER` | `POST` | YES | NO | YES | `415` | Content-Type が JSON ではない |
| `KVM_SELECT` / `POWER` | `GET`/他 | - | - | YES | `405` | POST のみ（メソッド、Origin、JSON の順に判定） |
| `KVM_*` / `POWER` | 任意 | - | - | NO | `401` | 未認証アクセスの拒否（セッションが最初） |
| `OTHER` (保護) | `GET` | - | - | YES | `ALLOW` | 認証済み GET アクセスを許可 |
| `OTHER` (保護) | `POST` | YES | - | YES | `ALLOW` | 認証済み POST アクセスを許可 |
| `OTHER` (保護) | `POST` | NO | - | YES | `403_ORIGIN` | CSRF 防御 (POST には Origin 必須) |
| `OTHER` (保護) | `POST/GET`以外 | - | - | YES | `405` | その他メソッドの拒否 |
| `OTHER` (保護) | 任意 | - | - | NO | `401` | 未認証アクセスの拒否 |

## 2. セキュリティ検証規則

### 2.1 Host 検証 (`http_guard_host_ok`)
DNS リバインディング攻撃を防ぐため、`Host` ヘッダを厳密に検証します。
*   IPv4 アドレス（ドット表記、先頭ゼロ不可）の完全一致。
*   許可リスト (`allowed_names`) に指定されたドメイン名との一致（大文字小文字区別なし）。
*   いずれも `:ポート` (1-65535、先頭ゼロ不可) が付加されても許可します。
*   IPv6 リテラル、長すぎる値 (255 超)、不正文字は拒否します。

### 2.2 Origin 検証 (`http_guard_origin_ok`)
状態を変更する可能性のあるリクエスト（`POST` 等）および `WS_VIDEO` のアップグレードで、`Origin` と `Host` を厳密に比較します。
*   **必須スキーム**: `https://` で始まること。
*   **拒否**: `Origin: null`、`http://` スキーム、ユーザー情報 (`@`)、末尾のドット、IPv6 リテラル。
*   **Host 一致**: `Origin` のホスト（およびポート）部分は、`Host` ヘッダと完全に一致する必要があります。

### 2.2.1 Origin 検証: ページのポートつき (`http_guard_origin_ok_for_port`)
映像専用の `httpd`（別ポート、`docs/VIDEO_TRANSPORT.md` 5 章）用。ブラウザの `Origin` は**ページの Origin**（メインのポート）で、映像サーバーの `Host`（`host:8444`）とは**ポートだけが違う**ため、`http_guard_origin_ok` の「Host と完全一致」は使えない。署名は `(origin, origin_len, host, host_len, page_port)`。許可する条件は次のすべて:
*   `Origin` が `https://` で始まる（大小文字を区別しない）。
*   `Origin` のホスト名が、`Host` のホスト名（`:ポート` を除く）と一致する（大小文字を区別しない）。
*   `Origin` のポートが `page_port`（メインのポート）と一致する。`Origin` にポートが無ければ 443 と見なす（`page_port` が 443 のとき `:443` があってもよい）。`Host` のポート（映像サーバーのポート）は、形式（1〜65535、先頭ゼロ・空・非数字は不可）だけを検査し、値は問わない（`Host` の許可リストの検査は、呼び出し側が `http_guard_host_ok` で行う）。
*   拒否: `null`、`@`、`[`（IPv6）、ホスト名の先頭・末尾・連続のドット、ホスト名に使えない文字（英数字、`-`、`_`、`.` 以外。`/`、`?`、`#`、空白、制御文字、非 ASCII を含む）、ポートが空・数字でない・範囲外・先頭ゼロ、`https://` 以外のスキーム、`Origin` に `/` やパスが続く、`host` が空、`page_port == 0`、ホスト名が 255 バイトを超える。
*   `http_guard_origin_ok` との違い: 後者は文字列の完全一致なので、文字の種類は検査しない。こちらは、ホスト名の文字を英数字・`-`・`_`・`.` に限る分だけ、さらに厳しい。

### 2.3 Cookie 解析
*   **名前**: `__Host-session` に固定。これは Secure 属性と Path=/ を強制し、サブドメインや非 HTTPS からの上書きを防ぐ Cookie Prefix 規則を活用するためです。
*   **拒否**: 同名の Cookie が複数存在する場合、不正な長さ、Base64URL の非正規形（余りビットが非ゼロ）。

### 2.4 JSON 解析と UTF-8 (`http_guard_parse_json_*`)
*   **最小限のパーサ**: 外部ライブラリ不使用、動的メモリ確保なし。
*   **受付形式**:
    *   Setup / Login 用: `{"password":"..."}`
    *   Change Password 用: `{"old_password":"...","new_password":"..."}`
*   **制約**:
    *   未知のキー・重複するキー・末尾のゴミは即座に拒否します。
    *   パスワード長が `AUTH_LOGIC_MAX_PASSWORD_LEN` (128 バイト) を超える場合は拒否します。
    *   文字列値は RFC 3629 に従う厳格な UTF-8 (不正バイト列、オーバーロング、サロゲート範囲、U+10FFFF 超の拒否) に制約されます。
    *   JSON 文字列内の `\uXXXX` エンコードされたサロゲートペアは、結合されて 4 バイトの UTF-8 にデコードされます。孤立したサロゲートは拒否されます。
    *   `\u0000` は NUL バイト混入を防ぐため拒否されます。

## 3. エラー語彙仕様 (`http_guard_format_*`)
HTTP レスポンスに乗せる JSON は、入力値を埋め込まずに固定の語彙のみを使用します。
*   エラー応答: `{"error":"<メッセージ>"}`
*   ステータス応答: `{"state":"needs_setup" | "ready", "authenticated": true | false}`。映像専用のサーバーがあるときは、末尾に整数の `"video_port":N`（1〜65535）が付く（`http_guard_format_status` の引数 `video_port`。0 は「なし」で、キーを出さない）。

## 4. 静的アセット
静的ページ（ログイン画面や設定画面）用のパス（`HTTP_GUARD_PATH_STATIC`）は、認証状態に関わらず GET を許可します。このパスから配信されるアセット（HTML, JS, CSS）には秘密情報を含めないでください。
