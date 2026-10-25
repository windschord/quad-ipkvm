# Quad IP-KVM Web API

## エンドポイント

ベース URL: `/api/v1/auth/`

### 0. 静的ページ
- **メソッド**: `GET`
- **パス**: `/` (index.html), `/app.js`, `/app.css`
- **認証**: 不要 (Host ヘッダの検証は行われます)
- **応答**: 静的ファイル (HTML, JS, CSS)
  - `Content-Security-Policy` ヘッダと `Cache-Control: no-store` ヘッダが付与されます。
  - HTML や JS には秘密情報が含まれません。
- **ステータス**:
  - `200 OK` (成功時)
  - `403 Forbidden` (不正な Host ヘッダ)
  - `404 Not Found` (未登録の URI)
  - `405 Method Not Allowed` (GET以外のメソッド)

### 1. 状態取得
- **メソッド**: `GET`
- **パス**: `/status`
- **認証**: 不要 (Host ヘッダの検証は行われます)
- **応答例**: `{"state":"needs_setup", "authenticated":false}`
- **ステータス**:
  - `200 OK` (成功時)
  - `403 Forbidden` (不正な Host ヘッダ)
  - `405 Method Not Allowed` (GET以外のメソッド)

### 2. 初回設定
- **メソッド**: `POST`
- **パス**: `/setup`
- **Content-Type**: `application/json`
- **認証**: 不要 (ただし、状態が `NEEDS_SETUP` である必要があります)
- **リクエスト例**: `{"password": "MySuperSecretPassword"}`
- **応答例**: `{}`
- **ステータス**:
  - `200 OK` (成功時)
  - `400 Bad Request` (パスワードが短い、弱い、不正な JSON。本文は下記の `error` コードを持つ JSON)
  - `403 Forbidden` (不正な Host/Origin、または設定中の状態不整合エラー)
  - `405 Method Not Allowed` (POST以外のメソッド)
  - `409 Conflict` (すでに設定済み (`READY` 状態))
  - `411 Length Required` (Content-Length ヘッダがない)
  - `413 Payload Too Large` (リクエストボディが 512 バイトを超過)
  - `415 Unsupported Media Type` (Content-Type が application/json でない)
  - `500 Internal Server Error` (内部エラー)
  - `503 Service Unavailable` (現在パスワードハッシュ計算中。`Retry-After: 1` を参照)
- **400 の本文**: `{"error":"<code>"}` (`Content-Type: application/json`)。複数の規則に当たる場合は、下表の上から順で最初のものを返します。

  | `error` | 意味 |
  | --- | --- |
  | `password_too_short` | 10 文字未満 (長さはバイト数で数える。設定された最小長がより長ければそちら) |
  | `password_too_long` | 128 バイト超 (UTF-8 のバイト数。文字数ではない) |
  | `password_contains_admin` | 大文字小文字を問わず `admin` を含む |
  | `password_all_same` | 同じ文字だけの繰り返し |
  | `password_sequential` | 連番 (`0123456789`、`9876543210`、`abcdefghij` など。`1234567890` は連番ではなく下の一覧で拒否される) |
  | `password_common` | よく使われるパスワードの一覧に含まれる (大文字小文字を問わない) |
  | `bad_request` | 本文が不正 (JSON として読めない、`password` キーがない、128 バイト超でパーサーが拒否した場合など)、またはその他の設定失敗 |

  - 128 バイトを超えるパスワードはパーサーが先に拒否するため、HTTP では `password_too_long` ではなく `bad_request` になります。`password_too_long` は UI が送信前にバイト数を確認したときに表示するメッセージです。
  - 拒否理由はパスワードの内容を含みません。応答にパスワードは含まれません。

### 3. ログイン
- **メソッド**: `POST`
- **パス**: `/login`
- **Content-Type**: `application/json`
- **認証**: 不要 (状態が `READY` である必要があります)
- **リクエスト例**: `{"password": "MySuperSecretPassword"}`
- **応答例**: `{}`
  - 成功時には `Set-Cookie` でセッションが発行されます。
- **ステータス**:
  - `200 OK` (成功時)
  - `400 Bad Request` (不正な JSON)
  - `401 Unauthorized` (パスワード間違いなど)
  - `403 Forbidden` (不正な Host/Origin、または `NEEDS_SETUP` 状態)
  - `405 Method Not Allowed` (POST以外のメソッド)
  - `411 Length Required` (Content-Length ヘッダがない)
  - `413 Payload Too Large` (リクエストボディが 512 バイトを超過)
  - `415 Unsupported Media Type` (Content-Type が application/json でない)
  - `429 Too Many Requests` (レート制限中。`Retry-After: 10` を参照)
  - `503 Service Unavailable` (現在パスワードハッシュ計算中。`Retry-After: 1` を参照)

### 4. ログアウト
- **メソッド**: `POST`
- **パス**: `/logout`
- **認証**: 要 (Cookie でセッショントークンを送信)
- **リクエスト**: ボディなし
- **応答例**: `{}`
- **ステータス**:
  - `200 OK` (成功時)
  - `401 Unauthorized` (セッションなし)
  - `403 Forbidden` (不正な Host/Origin)
  - `405 Method Not Allowed` (POST以外のメソッド)

## セキュリティ上の前提
- **ネットワーク環境**: 本システムは VPN などの安全な LAN 内で使用されることを前提としています。
- **自己署名証明書**: 初回起動時にランダムな自己署名証明書を生成します。ブラウザで警告が出ますが、想定通りの動作です。証明書の有効期限は 2025 年から 2036 年の固定値としています（機器に時計がないため）。
- **平文保存**: パスワードハッシュおよび証明書・秘密鍵は NVS に平文で保存されます（フラッシュ暗号化は非対応）。NVS の読み出しエラー等で設定が取得できない場合は、フェイルクローズとしてサーバーを起動しません。
- **ハッシュ計算時間**: PBKDF2 ハッシュは 10000 反復で計算され、ESP32-P4 上で約 0.63〜0.85 秒かかります。この計算中は他のハッシュ計算要求は 503 を返します。
- **DoS耐性**: LAN内の全体ロックアウト DoS 攻撃は受容した残存リスクです。

## HTTP ヘッダーと Cookie
- **Cookie バッファ**: Cookie ヘッダ全体を受け取るため最大 512 バイトのバッファを確保しています。それを超える場合は切り捨てにより拒否されます。

## Cookie
- **名前**: `__Host-session`
- **属性**: `HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=<絶対期限>`
- **理由**: セッションハイジャックと CSRF を防ぐため。`__Host-` プレフィックスにより、HTTPS 通信のみで送信され、サブドメインからの干渉を防ぎます。

## 確認手順 (curl)
以下の手順で動作確認を行えます。`-k` オプションで証明書の検証をスキップし、`Host` と `Origin` ヘッダを指定します。

1. **初回設定**
```sh
curl -k -c cookies.txt -H "Host: quad-ipkvm.local" -H "Origin: https://quad-ipkvm.local" -H "Content-Type: application/json" -X POST https://quad-ipkvm.local/api/v1/auth/setup -d '{"password": "MySecurePassword123"}'
```

2. **ログイン**
```sh
curl -k -c cookies.txt -b cookies.txt -H "Host: quad-ipkvm.local" -H "Origin: https://quad-ipkvm.local" -H "Content-Type: application/json" -X POST https://quad-ipkvm.local/api/v1/auth/login -d '{"password": "MySecurePassword123"}'
```

3. **状態取得**
```sh
curl -k -b cookies.txt -H "Host: quad-ipkvm.local" https://quad-ipkvm.local/api/v1/auth/status
```

4. **ログアウト**
```sh
curl -k -c cookies.txt -b cookies.txt -H "Host: quad-ipkvm.local" -H "Origin: https://quad-ipkvm.local" -X POST https://quad-ipkvm.local/api/v1/auth/logout
```
