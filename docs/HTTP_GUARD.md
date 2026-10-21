# HTTP Guard 仕様書

`http_guard` コンポーネントは、Quad-IPKVM のネットワーク通信において、悪意のある入力や不正な状態遷移を防ぐための純粋な判定ロジック層です。

## 1. 認可判定表 (`http_guard_decide`)

システムの認証状態 (`NEEDS_SETUP` / `READY`) とリクエストの特性に基づき、どのアクションをとるかを決定します。

### 1.1 状態: `NEEDS_SETUP` (初期設定待ち)

| Path Category | Method | Origin OK | is_json | セッション有無 | 判定結果 | 理由 |
|---|---|---|---|---|---|---|
| `STATUS` | 任意 | - | - | - | `ALLOW` | 状態確認は常に許可 |
| `SETUP` | `POST` | YES | YES | - | `ALLOW` | 初期パスワード設定を許可 |
| `SETUP` | `POST` | NO | - | - | `403_ORIGIN` | CSRF 防御 (Origin 不一致) |
| `SETUP` | `POST` | YES | NO | - | `415` | Content-Type が `application/json` ではない |
| `SETUP` | `GET`/他 | - | - | - | `405` | 設定は POST のみ許可 |
| その他全て | 任意 | - | - | - | `403_SETUP_REQUIRED` | 設定が完了するまで他の API は利用不可 |

### 1.2 状態: `READY` (通常運用)

| Path Category | Method | Origin OK | is_json | セッション有無 | 判定結果 | 理由 |
|---|---|---|---|---|---|---|
| `STATUS` | 任意 | - | - | - | `ALLOW` | 状態確認は常に許可 |
| `SETUP` | 任意 | - | - | - | `403_SETUP_REQUIRED` | 設定済みの場合は設定 API へのアクセスを拒否 (仕様上は事実上ブロック扱い) |
| `LOGIN` | `POST` | YES | YES | - | `ALLOW` | ログインを許可 |
| `LOGIN` | `POST` | NO | - | - | `403_ORIGIN` | CSRF 防御 (Origin 不一致) |
| `LOGIN` | `POST` | YES | NO | - | `415` | Content-Type が `application/json` ではない |
| `LOGIN` | `GET`/他 | - | - | - | `405` | ログインは POST のみ許可 |
| `LOGOUT`| `POST` | YES | - | YES | `ALLOW` | ログアウトを許可 |
| `LOGOUT`| `POST` | NO | - | YES | `403_ORIGIN` | CSRF 防御 |
| `LOGOUT`| `GET`/他 | - | - | YES | `405` | ログアウトは POST のみ許可 |
| `LOGOUT`| 任意 | - | - | NO | `401` | セッションがない場合はログアウト不可 (未認証) |
| `OTHER` (保護リソース) | `GET` | - | - | YES | `ALLOW` | 認証済み GET アクセスを許可 |
| `OTHER` (保護リソース) | `POST` | YES | - | YES | `ALLOW` | 認証済み POST アクセスを許可 |
| `OTHER` (保護リソース) | `POST` | NO | - | YES | `403_ORIGIN` | CSRF 防御 (POST には Origin 必須) |
| `OTHER` (保護リソース) | 任意 | - | - | NO | `401` | 未認証アクセスの拒否 |

## 2. Origin 検証規則 (`http_guard_origin_ok`)

状態を変更する可能性のある `POST` リクエスト等において、`Origin` ヘッダと `Host` ヘッダを厳密に比較します。

*   **必須スキーム**: `https://` で始まること（大文字小文字は区別しない）。
*   **拒否条件**:
    *   `Origin: null` の拒否。
    *   `http://` スキームの拒否。
    *   ホスト部分に `@` (ユーザー情報) が含まれる場合の拒否。
    *   末尾のドット (FQD の終端) が含まれる場合の拒否。
    *   IPv6 リテラル (`[::1]`) の拒否（パーサの単純化とセキュリティのため）。
*   **Host ヘッダとの一致**: `Origin` のホスト（およびポート）部分は、`Host` ヘッダの値と完全に一致する必要があります（大文字小文字は区別しない）。

## 3. JSON 解析仕様 (`http_guard_parse_json_*`)

*   **最小限のパーサ**: 外部ライブラリを使用せず、動的メモリ確保 (malloc) を一切行わない厳格なパーサです。
*   **受付形式**:
    *   Setup / Login 用: `{"password":"..."}`
    *   Change Password 用: `{"old_password":"...","new_password":"..."}`
*   **制約**:
    *   未知のキーや重複するキーは即座に拒否します。
    *   末尾のゴミ（JSON オブジェクトの後の余分な文字）は拒否します。
    *   パスワード長が `AUTH_LOGIC_MAX_PASSWORD_LEN` (128 バイト) を超える場合は拒否します。
    *   制御文字 (`0x00`-`0x1F`) はエスケープされていない限り拒否します。
    *   サロゲートペア (`\uD800` ~ `\uDFFF`) はパーサの単純化とセキュリティのため拒否します。それ以外の `\uXXXX` は UTF-8 にデコードして格納します。

## 4. エラー語彙仕様 (`http_guard_format_*`)

HTTP レスポンスに乗せる JSON は、入力値を埋め込まずに固定の語彙のみを使用します。
*   エラー応答: `{"error":"<メッセージ>"}`
*   ステータス応答: `{"state":"needs_setup" | "ready", "authenticated": true | false}`
