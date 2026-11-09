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

### 6. WebSocket 入力（`WS /ws`）
キーボードとポインタの入力を、ブラウザからデバイスへ送る。**認証が必須**で、`/video` と同じ検査（Host、Origin、セッション Cookie）を、アップグレードの前に行う。拒否は、`101` を返さず接続を閉じる。

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

### 7. KVM 切替と ATX 電源（`/api/v1/kvm/*`、`/api/v1/power/*`）
4 台の切替（前段の 4ch HDMI KVM）と、選択中の PC への電源操作（ATX）。Issue #85（C-5）。**認証が必須**。`POST` は **Origin 必須**（Host と一致）で、`Content-Type: application/json` のみ受け付ける（それ以外は 415）。ボディは 64 バイト以下（超過は 413、`Content-Length` なしは 411）。

**基本方針**
- **KVM の LED が「状態の真実」**。切替要求は、KVM 本体のボタンを押して LED の変化を待つ。成功は、LED が確定したことで決まる（要求の受理 ≠ 切替の完了）。
- **ATX は `channel` が必須**。現在のチャンネルと違えば **409**（人が別の PC を見ている間に、別の PC の電源を切る事故を避ける）。**判定は状態機械の内部で行う**（API 層で事前に確認しない。確認と実行の間に切り替わる競合を避ける）。
- **パルス長はクライアントが決めない**。操作の種類ごとの長さはサーバーの設定（Kconfig `KVM_*` / `ATX_*`。値は `TODO(hw-verify)`）で、ボードの HAL の上限（INTERFACES.md §6）が最後に頭を押さえる。
- 受理した操作は**非同期**に実行する（`202`）。各操作には**操作 ID**（`op_id`、起動からの通し番号、u32）が付き、`202` の応答で返る。結果は `GET /api/v1/kvm/status` の `last_op` で見る（下記）。**結果を保持するのは直近の 1 操作だけ**: 後続の操作を受理すると、前の操作の結果は消える（`op_id` が違えば、クライアントは「結果は不明」として扱う）。操作は一度に 1 つしか実行しないので（`busy`）、自分の操作の完了を待ってから次を出せば、結果を取りこぼさない。

#### 7.1 状態取得 `GET /api/v1/kvm/status`
`200`:
```json
{"state":"idle","active_channel":2,"video_sync":"locked","atx_locked":false,"atx_anomaly":false,"fault":false,"cooldown_ms":0,"last_op":{"op_id":7,"kind":"power_press","channel":2,"result":"completed"}}
```
| フィールド | 意味 |
|---|---|
| `state` | `unknown`（現在チャンネルを確定できない。起動直後、LED が全消灯・複数点灯・チャタリング）、`idle`、`switching`（切替中）、`tracking`（手動切替に追従中）、`atx`（ATX パルス中） |
| `active_channel` | `1..4`。`state` が `unknown` のときは `null` |
| `video_sync` | `unknown` / `no_signal` / `locked` |
| `atx_locked` | ATX が拒否される状態（`unknown`、切替中、切替直後の安定待ち） |
| `atx_anomaly` | ATX のパルス（またはその準備）中に LED が変化・不正になった（パルスは中断済み）。**LED がほかの有効なチャンネルを示していれば `tracking`、無点灯・複数点灯なら `unknown`** に移る（`kvm_logic` の現在の挙動）。**次の ATX パルスの開始で消える** |
| `fault` | ボードの HAL が失敗した（ラッチ。**全 Ē を High にして停止し、以後 tick は出力を一切有効にしない**。`release_all` の再試行だけ続ける。再起動まで解除しない） |
| `cooldown_ms` | 次の ATX 操作を、**クールダウンのために**受け付けない残り時間（ms）。**0 は「クールダウンによる拒否がない」だけの意味**: `state` が `unknown` / `switching` / `tracking` / `atx`、`atx_locked`、`fault` のときは、0 でも ATX は拒否される |
| `last_op` | 直近に**受理した**操作（まだ無ければ `null`）。`op_id`、`kind`（`select` / `power_press` / `power_reset` / `power_force`）、`channel`、`result`: `pending`（実行中）、`completed`（切替: LED と映像の確認まで済んだ。ATX: パルスを異常なく出し終えた）、`aborted`（切替: タイムアウトか、手動切替に追い越された。ATX: パルス中に LED が変わって中断した、または `fault`）。`kvm_ctrl` が `kvm_logic` の状態の遷移から判定する |

#### 7.2 切替 `POST /api/v1/kvm/select`
リクエスト: `{"channel":3}`（`1..4`。**ほかのキーがあれば 400**）。
| 結果 | 応答 |
|---|---|
| 受理 | `202 {"result":"accepted","channel":3,"op_id":8}` |
| すでにそのチャンネルで `idle` | `200 {"result":"already_selected","channel":3}`（何も押さない） |
| ボディ不正、`channel` が欠落・範囲外・整数でない | `400 {"error":"invalid_request"}` |
| 切替中、ATX パルス中、追従中 | `409 {"error":"busy"}` |
| `state` が `unknown` | `409 {"error":"unknown_state"}`（現在の実装の `kvm_logic` は、`unknown` では切替を受け付けない。LED が確定すれば受け付ける） |
| ボードが `fault` | `503 {"error":"hw_fault"}` |
| サービス未起動 | `503 {"error":"not_ready"}` |

#### 7.3 電源操作 `POST /api/v1/power/press`、`/reset`、`/force`
| パス | 動作 | パルス長の上限（ハード上限。`TODO(hw-verify)`） |
|---|---|---|
| `press` | 電源ボタンを短く押す | 500 ms |
| `reset` | リセットボタンを押す | 200 ms |
| `force` | 電源ボタンの長押し（強制断） | 5000 ms |

リクエスト: `{"channel":2}`（**必須**。ほかのキーがあれば 400）。
| 結果 | 応答 |
|---|---|
| 受理 | `202 {"result":"accepted","channel":2,"action":"press","op_id":9}` |
| ボディ不正、`channel` の欠落・範囲外 | `400 {"error":"invalid_request"}` |
| **`channel` が現在のチャンネルと違う** | `409 {"error":"channel_mismatch","active_channel":1}` |
| 切替中・ATX パルス中 | `409 {"error":"busy"}` |
| ATX ロック中 | `409 {"error":"atx_locked"}` |
| `state` が `unknown` | `409 {"error":"unknown_state"}` |
| 前回の ATX 操作から `ATX_MIN_INTERVAL_MS`（`TODO(hw-verify)`）が経っていない | `429 {"error":"cooldown"}` + `Retry-After: <秒>` |
| ボードが `fault` | `503 {"error":"hw_fault"}` |
| サービス未起動 | `503 {"error":"not_ready"}` |

**判定の順序**（複数当てはまるときの優先。テストで固定する）: 認証 → Origin → 形式 → `channel` の範囲 → サービス未起動/`fault`（503）→ `unknown_state` → `busy` → `channel_mismatch` → `atx_locked` → `cooldown` → 受理。`channel_mismatch` を `cooldown` より先にするのは、「別の PC への操作」を、時間の問題として隠さないため。

**ログ**: 受理した操作と拒否の理由（`channel_mismatch` など）を、チャンネルと種類だけ記録する（セッション情報は出さない）。

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
