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
  - `403 Forbidden` (不正な Host/Origin、または設定中の状態不整合エラー。本文: `{"error":"already_setup"}`)
  - `405 Method Not Allowed` (POST以外のメソッド)
  - `409 Conflict` (すでに設定済み (`READY` 状態))
  - `411 Length Required` (Content-Length ヘッダがない。本文: `{"error":"length_required"}`)
  - `413 Payload Too Large` (リクエストボディが 512 バイトを超過。本文: `{"error":"payload_too_large"}`)
  - `415 Unsupported Media Type` (Content-Type が application/json でない)
  - `500 Internal Server Error` (内部エラー。本文: `{"error":"internal_error"}`)
  - `503 Service Unavailable` (現在パスワードハッシュ計算中。`Retry-After: 1` を参照。本文: `{"error":"server_busy"}`)
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
  - `401 Unauthorized` (パスワード間違いなど。本文: `{"error":"unauthorized"}`)
  - `403 Forbidden` (不正な Host/Origin、または `NEEDS_SETUP` 状態)
  - `405 Method Not Allowed` (POST以外のメソッド)
  - `411 Length Required` (Content-Length ヘッダがない)
  - `413 Payload Too Large` (リクエストボディが 512 バイトを超過)
  - `415 Unsupported Media Type` (Content-Type が application/json でない)
  - `429 Too Many Requests` (レート制限中。`Retry-After: 10` を参照。本文: `{"error":"rate_limited"}`)
  - `503 Service Unavailable` (現在パスワードハッシュ計算中。`Retry-After: 1` を参照。本文: `{"error":"server_busy"}`)

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


### 5. WebSocket 映像ストリーム
- **メソッド**: `GET`
- **パス**: `/video`
- **認証**: 要 (Cookie でセッショントークンを送信)
- **Origin**: 必須 (Host と一致すること)
- **アップグレード**: `WebSocket`
- **データ形式**: バイナリフレーム (1 メッセージ = JPEG 1 フレーム)
- **同時接続**: 上限 2 (Kconfig `WEB_SERVER_WS_MAX_CLIENTS` に依存)
- **切断条件**:
  - セッション切れ
  - ログアウト
  - クライアントからのデータ送信 (ビューアからのデータフレームを受信した場合、プロトコル違反として切断)
- **拒否時の挙動**: 認証なし、または Origin 不正の場合は `101 Switching Protocols` にならずに接続が拒否されます。

### 6. WebSocket 入力（`WS /ws`）【実装予定: Issue #84（C-4b）。現時点の `web_server` は `/ws` を登録していない】
キーボードとポインタの入力を、ブラウザからデバイスへ送る。**認証が必須**で、`/video` と同じ検査（Host、Origin、セッション Cookie）を、アップグレードの前に行う（`http_guard` に `HTTP_GUARD_PATH_WS_INPUT` を足し、`/video` と同じ方針にする。拒否は、`101` を返さず接続を閉じる）。

- **メソッド**: `GET`（WebSocket のアップグレード）、**パス**: `/ws`、**Origin**: 必須（Host と一致）。
- **操作できる端末は 1 つだけ**（先着。`HID_MAX_CONTROLLERS`、既定 1）。2 つ目以降は、サーバーが `DENIED` を送って閉じる。**映像（`/video`）の視聴は、入力に影響しない。**
- **入力のデータフレームは、すべてバイナリ**。**テキストフレーム、16 バイトを超えるデータフレーム、断片化したデータフレーム**は、**プロトコル違反として接続を閉じる**（長さは、ペイロードを読む前に判定する）。**この制限は入力のデータフレームだけに適用する。** 制御フレーム（Ping、Pong、Close）は RFC 6455 に従って処理する（ペイロードは 125 バイト以下。Ping には Pong で応答し、Close には Close で応答して閉じる）。数値は **リトルエンディアン**。

#### クライアント → サーバー
| 型 | 長さ | 内容 |
|---|---|---|
| `0x01` KEY_DOWN | 2 | `usage:u8`（HID Usage、Keyboard/Keypad Page 0x07）。有効: `0x04..0xA4`、`0xE0..0xE7`（修飾キー）。範囲外は無視して `INVALID` を数える |
| `0x02` KEY_UP | 2 | `usage:u8`（同上） |
| `0x03` RELEASE_ALL | 1 | 全キー・全ボタンを離す |
| `0x04` KEEPALIVE | 1 | 何も押していなくてもよい。**キーかボタンを押している間、クライアントは 1 秒ごとに送る** |
| `0x10` PTR_ABS | 7 | `buttons:u8`（下位 3 ビット）、`x:u16`、`y:u16`（`0..32767`。範囲外は 32767 に丸める）、`wheel:i8`（`-127..127`。`-128` は `-127`） |
| `0x11` PTR_REL | 7 | `buttons:u8`、`dx:i16`、`dy:i16`、`wheel:i8` |

- キー: 同じキーの `KEY_DOWN` の繰り返しは無視（状態は変わらない）。**修飾キー（`0xE0..0xE7`）を除く**押しているキーの数が 6 を超えたら（修飾キーは `modifiers` のビットで表すので数えない。Ctrl+Shift+通常キー 6 個まではロールオーバーにならない）、キーボードのレポートは **ErrorRollOver（全スロット `0x01`）**。
- **押しっぱなしのタイムアウト**: キーかボタンが押されている間、**`HID_HOLD_TIMEOUT_MS`（既定 3000）メッセージが届かなければ `release_all`**（ネットワークが切れても、キーが押されたままにならない）。
- **レート制限**: トークンバケット（`HID_RATE_PER_SEC` 既定 400、`HID_RATE_BURST` 既定 100）。超過したメッセージは捨てて数え、**5 秒の窓の中で 1000 回を超えたら、`release_all` して接続を閉じる**。**ただし、解放側の状態遷移は捨てない**（捨てると、ターゲットでキーやボタンが押されたままになる。押しっぱなしのタイムアウトは、他のメッセージが届き続ける間は働かない）: `KEY_UP`、`RELEASE_ALL`、**ボタンの状態が変わる `PTR_ABS` / `PTR_REL`** は、トークンが無くても処理する（トークンは 0 で止め、負にはしない）。ただし超過の回数には数え、上の閉じる条件に含める。捨てるのは、`KEY_DOWN`、ボタンが変わらないポインタの移動、`KEEPALIVE` だけ。値は `TODO(hw-verify)`（実際の操作で十分か）。
- **セッション**: 5 秒ごとに再検証し、無効なら `release_all` して閉じる。ログアウトも同じ。
- **切断**: `release_all` を行うのは、**操作者（所有者）の接続が切れたとき**だけ。`DENIED` で閉じた 2 つ目以降の接続や、操作者でない接続の切断は、操作者の押しているキーやボタンに影響しない（`hid_logic_release_owner()` は所有者のときだけ解放する）。
- **ポインタの種別の切り替え**: 絶対と相対は別の HID インターフェースなので、`hid_logic` は押しているボタンを 1 組だけ持ち、**種別が切り替わるとき、旧側へ `buttons = 0` のレポートを先に送ってから、新側に送る**（旧側のボタンが押されたまま残らない。切り替えの瞬間にボタンが離れるのは許容する）。

#### サーバー → クライアント
| 型 | 長さ | 内容 |
|---|---|---|
| `0x80` GRANTED | 1 | この接続が操作者になった（接続直後に 1 回） |
| `0x81` DENIED | 2 | `reason:u8`（`1` = 他の端末が操作中、`2` = 入力が無効）。この後、サーバーが閉じる |
| `0x82` USB_STATE | 2 | `state:u8`（`0` = ターゲットに未接続・未列挙、`1` = 接続済み）。接続直後と、変化したときに送る |

#### エラーの扱い
`hid_logic` は、不正なメッセージ（無効な型・長さ・範囲）を **数えて捨てる**。接続を閉じるのは、プロトコル違反（テキスト、長さ超過、断片化）と、レート超過が続いたときだけ。**メッセージの内容は、ログに出さない**（キー入力に、パスワードが含まれうる）。

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

## レスポンスヘッダ
各エンドポイントのレスポンスには、セキュリティ向上のため以下のヘッダが付与されます。
また、レスポンス内で同一名のヘッダが重複して付与されることはありません。

### 種類ごとのヘッダ
- **静的ページ (/, /app.js, /app.css)**
  - `Content-Security-Policy`: コンテンツの種類に応じて設定されます（例: HTML では `script-src 'self'`、`style-src 'self'`、`connect-src 'self'` を含み、`unsafe-inline` や `unsafe-eval` は含まれません）。
  - `X-Content-Type-Options: nosniff`
  - `Cache-Control: no-store`
  - `Referrer-Policy: no-referrer`
- **API およびエラー応答**
  - `Content-Security-Policy: default-src 'none'; frame-ancestors 'none'`
  - `X-Content-Type-Options: nosniff`
  - `Cache-Control: no-store`
  - `Referrer-Policy: no-referrer`
