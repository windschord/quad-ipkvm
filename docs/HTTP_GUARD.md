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
*   ステータス応答: `{"state":"needs_setup" | "ready", "authenticated": true | false}`
