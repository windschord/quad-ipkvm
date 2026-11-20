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

**ビューアの画面（`/`、`web/index.html` / `app.css` / `app.js`。API ではなく、ブラウザ側の挙動）**:
- **構造**: ログイン・初期設定は、狭いカード（`#auth-card`）。ログイン後の**ダッシュボード（`#view-dashboard`）は、ウィンドウの全幅**（カードは隠れる）。`#error-message` はカードの外（どちらの画面でも見える）。
- **操作バー**（ダッシュボードの上端、狭い幅では折り返す）: タイトル、入力の切替、マウス(相対)、入力・ターゲットの状態、**表示サイズの 3 つのボタン**（`role="group" aria-label="表示サイズ"`、選択中は `aria-pressed="true"`）、**全画面**、ログアウト。映像の状態（`#video-status`、証明書の案内）とヒントは、映像の**下**（`.video-info`。映像と重ならない）。
- **表示モード**（既定は「ウィンドウに合わせる」。選んだモードは `localStorage` の `quadkvm.viewMode` に保存し、起動時に復元。保存できなくても動く。不正な保存値は既定に戻る）:
  - `fit`（ウィンドウに合わせる）: 縦横比を保ち、幅と高さの両方が、使える領域（ウィンドウの幅 × バー・状態の行を除いた高さ）に収まる最大。拡大もする。スクロールなし。
  - `width`（幅に合わせる）: 領域の幅いっぱい、高さは縦横比から。縦に収まらなければ、ページがスクロールする。
  - `actual`（実寸 100%）: 映像の解像度どおり（CSS ピクセル 1:1）。領域より大きければ、映像の領域だけがスクロールする。
  - canvas の箱は、**CSS ではなく JavaScript が、縦横比どおりの整数ピクセル（切り捨て、1 未満にしない）で `style.width / height` に設定する**（`VideoLayout.fitSize`）。マウスの絶対座標は、この箱から換算するため、`object-fit` などで黒帯を作らない。解像度の変更、ウィンドウのリサイズ、モードの変更、全画面の出入り、領域の大きさの変化（`ResizeObserver`）で再計算する。解像度または領域が不明（0 など）のときは設定しない。
- **全画面**: `.video-wrap`（映像と状態の重ね表示）に `requestFullscreen()`。全画面中は操作バーと下の文言を隠し、常に `fit` で表示する（保存したモードは変えない）。全画面から戻る手段として、映像の右上（`.video-wrap` の中、canvas の外側）に、全画面の間だけ表示する小さな「全画面を終了」ボタン（`#btn-fullscreen-exit`）を出す（Esc のない端末向け）。通常のバーのボタンは、全画面でないとき「全画面」。全画面の要求が、画面の切替（ログアウト・セッション切れ）より後に完了したときは、ダッシュボードが隠れていれば全画面を終了する。拒否されても何も起きない。Fullscreen API が無い（`document.fullscreenEnabled` が偽、または `requestFullscreen` が無い）ときは、ボタンを無効にして理由を `title` に出す。入力・相対マウス（ポインタロック）は全画面でも動く。
  - **Esc とキーボード**: 全画面でない通常の動作では、ブラウザは Esc で全画面を終了する。Chromium 系（`navigator.keyboard.lock`）では、**全画面で、入力が有効で、この接続が操作権を得ている（GRANTED を受けている）間だけ** Esc を奪って対象に送る（操作権がないと、対象に何も送れないので、ロックしない）。失敗は無視する。入力を切る、操作権を失う（拒否・切断）、または全画面を出ると `unlock` する。次の 3 点を区別する。
    - (a) **ページ**: Esc を**約 1 秒**（`app.js` の長押しの時間、1000 ms）押し続けると、入力を無効にし、キーボードロックを解除する。
    - (b) **Chromium のキーボードロック**: Esc の長押しで、ブラウザがロックを解除する（約 2 秒とされる。ブラウザと版に依存し、このリポジトリでは未確認）。
    - (c) **全画面をブラウザが終了するか**: ブラウザと版に依存する。**未確認**（実機のブラウザで確認していない）。確実な手段として、全画面中は映像の右上に「全画面を終了」ボタンを出す。
  - **キーボードの扱い**: 入力が有効な間、キーボードの操作（Tab、Enter、Space を含む）は対象に送られ、ページでは取り消される。ローカルのボタンはマウスで操作する。キーボードで操作するには、入力を無効にする。
- **ISO 画面（C-12。`app.js` の `IsoViewer`、`index.html` の `#iso-panel`、`app.css` の `.iso-*`。API は §8）**: 操作バーの「ISO」ボタンで、ウィンドウの右に重ねるパネルを開閉する（映像の配置は変えない。全画面では出ない）。ログイン後（ダッシュボード）に始まり、ログアウトやセッション切れで止まる。`GET /api/v1/iso` が 404（`ISO_ENABLE` が無効のビルド）なら、ボタンも出さず、それ以後は何も要求しない。
  - **一覧**: 名前（`textContent` だけで表示する）、大きさ、状態（`uploading` / `incomplete` / `finalizing` / `ready` / `invalid`（理由つき）/ `deleting`）、マウント中の印。一覧が変わったときだけ描き直す（フォーカスは保つ）。ポーリングは、パネルが開いている間は 3 秒ごと、`finalizing` / `deleting` / `uploading` がある間と、画面が通信を始めた処理の間は 1 秒ごと（パネルが閉じていても）。**自分の `PUT` の最中は、一覧を取らない**。`sd.state` が `ready` でなければ、アップロードと取り込みは無効（理由を表示）。
  - **アップロード**: ファイルを選ぶと名前の初期値を作る（拡張子 `.iso` を除く）。名前の規則（§8 の基本方針）を、送る前にも確かめる。SHA-256 は、**自前の逐次実装**（`crypto.subtle.digest` は一括なので、数 GiB に使えない）で、1 MiB ずつ読んで計算し、`begin` で申告する（チェックボックスで省ける）。`Blob.slice` で、`chunk_size`（`begin` の応答、再開では `GET` の最上位の値）ごとに `PUT`（`X-Upload-Offset`）。**遅い**（1 回が 1.5 秒以上）と、次から半分（最小 4096 バイト）、速い（0.5 秒未満）と倍（`chunk_size` まで）。応答が読めない（ネットワークエラー）ときも半分にして、`GET` で `received` を取り直して続ける。`offset_mismatch` は、応答の `received` から続ける。`503 busy` は `Retry-After` の後に再試行、`503 not_ready` / `sd_unavailable` は SD の復帰を待って `received` から続ける。「一時停止」と、一覧の「再開」（同じファイルを選ぶ。同じ画面なら覚えている）、同じ名前・同じ大きさの中断中のイメージがあれば、新しく作らずに続きから送る。
  - **完了の待機**: 全部送ったら `finish` を送り、`GET` で `ready` / `invalid` / `incomplete` のどれかになるまで待つ（SD が使えない間は、イメージが一覧に出ないので、戻るのを待つ）。`incomplete` なら、SD の復帰を待ち、`received == size` は `finish` を送り直し、`received < size` は `PUT` を再開する（最大 5 回）。`422` と `invalid` は理由を表示する。
  - **再読み込み**: `incomplete` で `received == size` のイメージには、SD が `ready` で枠が空いていれば、**自動で `finish` を 1 回**送る（この画面が扱ったイメージには送らない）。`incomplete` で `received < size` のものは、「再開」で元のファイルを選ぶ。`finalizing` / `deleting` はポーリングで待つ。再開のとき別のイメージが `finalizing` で枠を使っていれば、終わるのを待つ（受信中の別のアップロードなら、エラー）。
  - **マウント / 取り外し / 削除 / 取り込み**: `locked_by_target`（ターゲットが取り外しを防止）のときは、確認のダイアログを出し、承諾されたら `force` を付けて 1 回だけ再送する。削除は確認のダイアログを出す（受信中のイメージなら、その `PUT` のループも止める）。取り込み（`rescan`）は、`{}` から始めて、`next_cursor` が `null` になるまで、その値を `cursor` に入れて繰り返す（`next_cursor` が進まなければ、止めてエラー）。エラーは、§8 の各表の行ごとに、日本語の文にする。
  - 名前の入力などで、「入力を有効にする」が入っていると、キー操作がターゲットに送られるので、入力を無効にしてから使う（パネルの案内にも書いてある）。
  - 検査は `tools/test/check_iso_viewer.js`（`ctest` の `check_iso_viewer`。偽の DOM / `fetch` / タイマー / サーバー。`--mutations` で、`app.js` の ISO の区間を 1 か所ずつ変えて、落ちることを確認する）。
- 検査は `docs/VIDEO_VIEWER_TESTS.md` 8 章。

### 1. 状態取得
- **メソッド**: `GET`
- **パス**: `/status`
- **認証**: 不要 (Host ヘッダの検証は行われます)
- **応答例**: `{"state":"needs_setup", "authenticated":false}`、映像専用のサーバーがあるとき `{"state":"ready","authenticated":true,"video_port":8444}`
- **`video_port`**（整数、1〜65535。任意）: 映像専用の HTTPS サーバー（`docs/VIDEO_TRANSPORT.md` 5 章）のポート。ビューアは、これがあれば `wss://<location.hostname>:<video_port>/video` に接続する。**キーがない（サーバー側の値が 0）ときは、映像専用のサーバーなし = メインのポートの `/video`**（従来どおり）。ビューアは、整数で 1〜65535 以外（文字列、小数、`null`、範囲外）を無視して従来の接続先にする。
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
> **ビューアの接続先と証明書の案内**: ビューアは、状態の取得（§1）の `video_port` から接続先を決める。映像の接続が**開く前に失敗**し、かつ `video_port` が別のポートのときは、`https://<ホスト名>:<video_port>/` へのリンクと「証明書を受け入れてから再読み込み」の案内を出す（自己署名の証明書の例外が、ポートごとのため。接続に成功すると消える）。映像サーバーの `GET /` は、動的な値を含まない静的な信頼ページ（`web/video_trust.html`、認証なし）を返す。ビューアの検査は `docs/VIDEO_VIEWER_TESTS.md`。
> **変更予定（`docs/VIDEO_TRANSPORT.md`、Issue #82 ③ / オーナー決定 B-3）**: 映像は、**映像専用の HTTPS サーバー**（別のポート `video_port`、既定 8444）の `/video` に移る。認証・Origin の規則は下のとおりで、**Origin は「ページの Origin（メインのポート）」と一致すること**（ホスト名は Host と一致、ポートはメインのポート）。実装が入るまで、下の記述（メインのポートの `/video`）が現在の挙動。
- **メソッド**: `GET`
- **パス**: `/video`（任意のクエリ: `?accept=h264`。H.264 を受け取れるクライアントが付ける。付けない = MJPEG だけ。`docs/VIDEO_TRANSPORT.md` §9.4）
- **認証**: 要 (Cookie でセッショントークンを送信)
- **Origin**: 必須 (Host と一致すること)
- **アップグレード**: `WebSocket`
- **データ形式**: **バイナリフレーム** (1 メッセージ = JPEG 1 フレーム。**P7 の H.264 が有効なときだけ、H.264 の Annex B の 1 アクセスユニット**。先頭が `FF D8` なら JPEG、`00 00 00 01` なら H.264。ヘッダーはない。`docs/VIDEO_TRANSPORT.md` §9.3。既定は MJPEG のままで、**`VIDEO_CODEC_H264_ALLOWED` が `n` の間は、H.264 は送られない**)。これに加えて、サーバーは**映像の状態**を、**テキストフレーム（JSON）**で送る（`docs/VIDEO_TRANSPORT.md` §8）。クライアントは、フレームの種類（バイナリ / テキスト）で区別する。
  - 形式: `{"t":"video","state":"locked","w":1280,"h":720}`（`state` は `unknown` / `no_signal` / `locked`。`w`・`h` は `locked` のときだけ 1 以上の整数で、それ以外は `0`）。最大 128 バイト。
  - 送る時: **接続の直後に 1 回**（最初のメッセージ。バイナリのフレームより前）、以後は**状態か解像度が変わったとき**だけ（定期的には送らない）。同じ内容は、繰り返さない。
  - 映像のフレームが送信中のときは、その送信が終わってから送る（途中に割り込まない）。「信号なし」のときはフレームが来ないので、送信中の待ちはない。
  - **変化は、集約して送る**: 状態の揺れで通知が頻発しないよう、同じクライアントへの通知の間隔は**最短 200 ms**（`status_min_interval_ms`）。間隔の間の変化は、間隔が経った後に、**そのときの最新の状態を 1 回だけ**送る（**途中の状態は送らない**。間隔の間に、元の状態に戻れば、何も送らない）。したがって、状態の変化の通知は、**最大 200 ms 遅れうる**。接続直後の最初の 1 回には、間隔を適用しない。
  - クライアントは、**未知のキーを無視**し、`t` が `"video"` でないテキストフレームは捨てる（将来の拡張のため）。
- **H.264 の設定**: Kconfig `VIDEO_CODEC_H264_ALLOWED`（既定 `n`）、`VIDEO_CODEC_UPGRADE_DWELL_MS`、`VIDEO_CODEC_IDR_MIN_INTERVAL_MS`、`VIDEO_CODEC_KEY_WAIT_MS`（`main/Kconfig.projbuild`、`docs/VIDEO_TRANSPORT.md` §9.5・§9.6）。`?accept=` の解析は `http_guard_video_accept`（未知の値は無視、複数の `accept` はどれかに `h264` があれば真、パーセントエンコードは解釈しない）。
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
4 台の切替（前段の 4ch HDMI KVM）と、選択中の PC への電源操作（ATX）。Issue #85（C-5）。**認証が必須**。`POST` は **Origin 必須**（Host と一致。無ければ 403）で、`Content-Type: application/json` のみ受け付ける（それ以外は 415）。`GET /api/v1/kvm/status` は Origin 不要（同一オリジンの GET に、ブラウザは Origin を付けない）。メソッドが違えば 405。ボディは 64 バイト以下（超過は 413、`Content-Length` なしは 411。認証・Origin・Content-Type の検査の**後**に判定する）。実装は `components/kvm_api`（純 C）、`http_guard_decide` の KVM 用パス、`kvm_service`（タスク + ミューテックス）、`web_server`。

**基本方針**
- **KVM の LED が「状態の真実」**。切替要求は、KVM 本体のボタンを押して LED の変化を待つ。成功は、LED が確定したことで決まる（要求の受理 ≠ 切替の完了）。
- **ATX は `channel` が必須**。現在のチャンネルと違えば **409**（人が別の PC を見ている間に、別の PC の電源を切る事故を避ける）。**判定は状態機械の内部で行う**（API 層で事前に確認しない。確認と実行の間に切り替わる競合を避ける）。
- **パルス長はクライアントが決めない**。操作の種類ごとの長さはサーバーの設定（Kconfig `KVM_*` / `ATX_*`。値は `TODO(hw-verify)`）で、ボードの HAL の上限（INTERFACES.md §6）が最後に頭を押さえる。
- 受理した操作は**非同期**に実行する（`202`）。各操作には**操作 ID**（`op_id`、起動からの通し番号、u32）が付き、`202` の応答で返る。結果は `GET /api/v1/kvm/status` の `last_op` で見る（下記）。**結果を保持するのは直近の 1 操作だけ**: 後続の操作を受理すると、前の操作の結果は消える（`op_id` が違えば、クライアントは「結果は不明」として扱う）。操作は一度に 1 つしか実行しないので（`busy`）、自分の操作の完了を待ってから次を出せば、結果を取りこぼさない。

#### 7.1 状態取得 `GET /api/v1/kvm/status`
サービス未起動（実機のボードの HAL が起動していない、またはミューテックスを短い上限内に取れない）のときは `503 {"error":"not_ready"}`。ログには出さない（ポーリングされるため）。`fault` がラッチされていても、状態そのものは `200`（`"fault":true`）。

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
| `cooldown_ms` | 次の ATX 操作を、**クールダウンのために**受け付けない残り時間（ms）。クールダウンは **ATX 操作が終わった時点**（完了でも中断でも）から数える。**実行中は `busy` で拒否されるので 0**。**0 は「クールダウンによる拒否がない」だけの意味**: `state` が `unknown` / `switching` / `tracking` / `atx`、`atx_locked`、`fault` のときは、0 でも ATX は拒否される |
| `last_op` | 直近に**受理した**操作（まだ無ければ `null`）。`op_id`、`kind`（`select` / `power_press` / `power_reset` / `power_force`）、`channel`、`result`: `pending`（実行中）、`completed`（切替: LED と映像の確認まで済んだ。ATX: パルスを異常なく出し終えた）、`aborted`（切替: タイムアウトか、手動切替に追い越された。ATX: パルス中に LED が変わって中断した、または `fault`）。`kvm_ctrl` が `kvm_logic` の状態の遷移から判定する |

#### 7.2 切替 `POST /api/v1/kvm/select`
リクエスト: `{"channel":3}`（`1..4`。**ほかのキーがあれば 400**）。
| 結果 | 応答 |
|---|---|
| 受理 | `202 {"result":"accepted","channel":3,"op_id":8}` |
| すでにそのチャンネルで `idle` | `200 {"result":"already_selected","channel":3}`（何も押さない） |
| ボディ不正、`channel` が欠落・範囲外・整数でない | `400 {"error":"invalid_request"}`（受け付けるのは `{"channel":N}` だけ。N は `1`〜`4` の 1 桁の整数。`"3"`、`3.0`、`3e0`、`03`、`-3`、`10`、重複キー、ほかのキー、キーのエスケープ、`}` の後の空白以外の文字、NUL、空は不正。記号の前後の JSON の空白は許す） |
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
| 前回の ATX 操作が**終わってから** `ATX_MIN_INTERVAL_MS`（`TODO(hw-verify)`）が経っていない | `429 {"error":"cooldown"}` + `Retry-After: <秒>`（残り時間を**切り上げた秒**、最小 1） |
| ボードが `fault` | `503 {"error":"hw_fault"}` |
| サービス未起動 | `503 {"error":"not_ready"}` |

`channel_mismatch` の `active_channel` は 1〜4（`unknown` のときは `unknown_state` が先に返るので、`null` にはならない）。想定外の内部結果（バグ）は `500 {"error":"internal_error"}` で、何も実行したことにしない。

**判定の順序**（複数当てはまるときの優先。テストで固定する）: 認証 → Origin → 形式 → `channel` の範囲 → サービス未起動/`fault`（503）→ `unknown_state` → `busy` → `channel_mismatch` → `atx_locked` → `cooldown` → 受理。`channel_mismatch` を `cooldown` より先にするのは、「別の PC への操作」を、時間の問題として隠さないため。

**ログ**: 受理した操作と拒否の理由（`channel_mismatch` など）を、チャンネルと種類と理由だけ記録する（`ESP_LOGI`。セッション情報とボディの生データは出さない。ボディが不正でチャンネルが分からないときは `channel=0`）。`status` の成功は記録しない。

**サービスの起動**: `kvm_service_start()` を呼ぶのは、実機のボードの HAL を実装する A-9（Antigravity）。呼ばれるまで、`status` と `select` と `power/*` は、認証と形式の検査を通ったうえで `503 not_ready` を返す。

### 8. ISO の仮想メディア（`/api/v1/iso*`）
ISO のアップロード、一覧、マウント（ターゲットの USB に仮想 CD-ROM として見せる）、取り外し、削除。設計は `docs/ISO_MOUNT_DESIGN.md`、境界は `docs/INTERFACES.md` §7。**認証が必須**。`POST` / `PUT` / `DELETE` は **Origin 必須**（Host と一致。無ければ 403）。JSON のボディは `Content-Type: application/json` のみ（それ以外は 415）で、**512 バイトまで**（超過は 413 `payload_too_large`）。`PUT` のデータ（チャンク）だけは `application/octet-stream`。`GET /api/v1/iso` は Origin を要求しない（既存の `GET` と同じ）。`{"error":"<code>"}` の形式のエラーは、`Content-Type: application/json`。

`ISO_ENABLE` が無効のビルドでは、**このパスは登録しない**（404）。有効でも、SD の準備ができていない間は `503 {"error":"not_ready"}`（下記）。

**基本方針**
- **イメージは、サーバーが付ける `id`（8 桁の小文字の 16 進）で扱う**。パスにユーザーの名前を使わない。**パスの `id` の形式が不正なものは、「そのイメージがない」と同じ `404 not_found`**（ボディの `id` の形式不正は 400）。
- **`name` の規則**（表示用）: UTF-8 として正しい（不正なバイト列、過長な表現、サロゲートは拒否）、1〜63 バイト、C0 制御文字（U+0000〜U+001F）・DEL（U+007F）・C1 制御文字（U+0080〜U+009F）・`/`・`\` を含まない、前後に空白（U+0020）がない。**同名の判定はバイト列の完全一致**（大文字小文字を区別。正規化なし）で、409 `name_exists`。
- **アップロードは、チャンクごとの `PUT` + 再開**（`ISO_MOUNT_DESIGN.md` §6）。**アップロードの枠は同時に 1 つだけ**。**`uploading` のイメージと、`finish` の最終処理中の `finalizing` のイメージが枠を使う**（`incomplete` は使わない。再開するときに、枠が空いている必要がある）。**起動時・SD の再初期化後のエクステントの作成（`INDEX`）中の `finalizing` は、枠を使わない**。
- **変更する操作は、`iso_op_mutex` が `ISO_OP_LOCK_WAIT_MS`（既定 20 ms）以内に取れなければ、`503 {"error":"busy"}` + `Retry-After: 1` を返す**（ワーカーの長いステップの間、単一スレッドの httpd を止めない）。判定の 13 段目（処理）で返す。ワーカーのキューがあふれたときも同じ。ビューアは、1 秒後に再試行する。
- **長い処理（`finish` の最終処理、削除）は、ワーカーが行い、`202` で返す**（`esp_http_server` が 1 つのタスクなので、要求を短く保つ。`ISO_MOUNT_DESIGN.md` §4.2）。
- **マウント中のイメージは、削除できない**。**マウントできるのは `ready` だけ**。
- イメージは、ターゲットに**読み取り専用**で見える。
- **`/api/v1/iso*` で定義していないメソッドは、`405`（`Allow` ヘッダー付き）**。`GET /api/v1/iso/<id>`（単体）は定義しない（405）。**本文のない `POST`（`finish`、`rescan`、`eject`）と `DELETE` には、`Content-Type` の検査（415）を適用しない**（本文があるときだけ）。

#### 8.1 一覧と状態 `GET /api/v1/iso`
`200`:
```json
{"sd":{"state":"ready","total_bytes":31914983424,"free_bytes":22000000000},
 "images":[
  {"id":"a1b2c3d4","name":"ubuntu-24.04","size":6126805504,"state":"ready","sha256":"<64 桁の 16 進>","received":6126805504,"invalid_reason":null},
  {"id":"0f9e8d7c","name":"win11","size":5500000000,"state":"incomplete","sha256":null,"received":2147483648,"invalid_reason":null}],
 "mounted":{"id":"a1b2c3d4","name":"ubuntu-24.04"},
 "target":{"locked":false,"ejected_by_target":false},
 "chunk_size":131072,
 "hidden_images":0,
 "upload":null}
```
アップロードの枠を使っているときの `upload`: `{"id":"0f9e8d7c","state":"receiving"}`（上の例の `win11` は `incomplete` なので、`upload` は `null`）。**`chunk_size` は、最上位に常に出す**（ページを再読込して `incomplete` を再開するクライアントが知るため）。

| フィールド | 意味 |
|---|---|
| `sd.state` | `ready` / `no_card`（カードが無い、または初期化できない）/ `unsupported_fs`（FAT32 でない、またはクラスタが 2048 バイト未満）/ `error`（I/O エラー） |
| `sd.total_bytes`、`free_bytes` | カードの大きさと空き（`ready` のときだけ。それ以外は `null`）。空きはキャッシュ値（マウント時と、ワーカー（`finish`、`DELETE` の完了）が更新。httpd からは更新しない） |
| `images[].state` | `uploading`（受信中）/ `incomplete`（中断。再開できる）/ `finalizing`（最終処理中: SHA-256、エクステントの作成、署名の検査。**起動時・SD の再初期化後のエクステントの作成中も、この状態**。後者は、アップロードの枠を使わない）/ `ready` / `invalid`（検査に失敗。削除だけ）/ `deleting`（削除中。すぐ一覧から消える） |
| `images[].received` | 受信済みのバイト数（`ready` では `size`）。再起動すると、同期済みの分に戻る（`ISO_MOUNT_DESIGN.md` §3.4） |
| `images[].sha256` | `ready` のとき。**なければ `null`**（`import` で取り込んだものなど） |
| `images[].invalid_reason` | `invalid` のときだけ: `checksum_mismatch` / `not_iso` / `too_fragmented` / `part_mismatch`（パーツの欠落、またはサイズの不一致）/ `meta_corrupt`（`size` と `received` は `null`）。それ以外は `null` |
| `mounted` | マウント中のイメージ（なければ `null`） |
| `target.locked` | ターゲットが取り外しを防止している（PREVENT ALLOW MEDIUM REMOVAL）。`mounted` が `null` のときは `false` |
| `target.ejected_by_target` | ターゲットが取り外した（START STOP UNIT）。**次の `mount` または `eject` まで `true` のまま**（`GET` では消えない。ポーリングで見逃さない） |
| `upload` | アップロードの枠を使っているもの（なければ `null`）。`state` は `receiving`（直近 5 秒以内に `PUT` があった）/ `idle`（それより前）/ `finalizing` |
| `chunk_size` | 次の `PUT` の最大の大きさ（バイト） |
| `hidden_images` | `ISO_MAX_IMAGES` を超えて、一覧に出さないイメージの数（カードの空きは使っている） |

`sd.state` が `ready` でなくても `200`（状態そのものを返す。`images` は空）。**SD を読まず、RAM のカタログから返す**。ポーリングされるので、ログには出さない。**SD の再初期化（抜き取り、I/O エラー）があると、`mounted` は `null` になる**（自動では戻らない）。`sd.state` が `ready` でない間の `eject` は、`200 {"result":"not_mounted"}`。

#### 8.2 アップロードの開始 `POST /api/v1/iso`
リクエスト: `{"name":"ubuntu-24.04","size":6126805504,"sha256":"<64 桁の 16 進>"}`（`name`、`size` は必須。`sha256` は任意）。ほかのキーは 400。`size` は、**34,816 以上**（= 2048 × 17。セクタ 0〜16。署名のセクタ 16 を含む）で `ISO_MAX_PARTS` × 1 GiB（16 GiB）以下の整数。
| 結果 | 応答 |
|---|---|
| 受理 | `201 {"id":"a1b2c3d4","chunk_size":131072}`（`images` に `uploading` で入る） |
| ボディ不正、`name` が規則に合わない、`size` が範囲外、`sha256` の形式が不正 | `400 {"error":"invalid_request"}` |
| SD が使えない（`sd.state` が `ready` でない） | `503 {"error":"not_ready"}` |
| アップロードの枠が使用中（`uploading` または `finalizing` がある） | `409 {"error":"upload_in_progress"}` |
| 同じ名前がある | `409 {"error":"name_exists"}` |
| イメージが上限（`ISO_MAX_IMAGES`）に達している | `409 {"error":"too_many_images"}` |
| 空き容量が足りない（`キャッシュ値 − 採取後に書いたバイト数 − 途中のイメージの (size_bytes − received) の合計` が、`size` + 予備 `ISO_FREE_RESERVE_MIB` に満たない。`ISO_MOUNT_DESIGN.md` §4.2）、`size` がカードの全体の大きさ（`total_bytes`）を超える | `507 {"error":"insufficient_storage","free_bytes":N}` |
| 処理中の SD の I/O エラー | `503 {"error":"sd_unavailable"}` |

#### 8.3 チャンクの送信 `PUT /api/v1/iso/<id>`
ヘッダー: `Content-Type: application/octet-stream`、`Content-Length: <n>`（**必須**。1 以上 `chunk_size` 以下）、`X-Upload-Offset: <u64>`（このチャンクの先頭のバイト位置。**必須**）。ボディ: n バイト。

**ヘッダーだけで判定し（空き容量のキャッシュ値の検査 507 を含む）、拒否するときは、本体を読まずに応答して `Connection: close` を付ける**（残りの本体を読み捨てて、単一スレッドのサーバーが止まるのを避ける）。クライアントは、拒否の後、新しい接続で続ける。**ネットワークエラー（未読の本体を残した接続のリセットで、応答が読めない場合を含む）のときは、`GET /api/v1/iso` で `received` を取り直して続ける**。**1 つのチャンクの受信は `ISO_PUT_RECV_TIMEOUT_S`（既定 2 秒）以内**。超えたら、何も書かずに接続を閉じる（`/ws` の入力を止めないため）。クライアントは、遅いときは小さなチャンクを送ってよい。
| 結果 | 応答 |
|---|---|
| 受理 | `200 {"received":N}`（N = 受信済みのバイト数。全体の大きさに達しても `ready` にはならない。`finish` が要る） |
| `id` の形式が不正、そのイメージがない（`deleting` を含む） | `404 {"error":"not_found"}` |
| `Content-Type` が違う | `415 {"error":"unsupported_media_type"}` |
| `Content-Length` がない | `411 {"error":"length_required"}` |
| `Content-Length` が 0 や不正、`X-Upload-Offset` の欠落・不正 | `400 {"error":"invalid_request"}` |
| `Content-Length` が `chunk_size` を超える | `413 {"error":"payload_too_large"}` |
| SD が使えない（`sd.state` が `ready` でない） | `503 {"error":"not_ready"}` |
| そのイメージが受信できる状態でない（`ready`、`invalid`、`finalizing`） | `409 {"error":"not_uploading"}` |
| `incomplete` のイメージを再開しようとしたが、別のイメージが枠を使っている | `409 {"error":"upload_in_progress"}` |
| **`X-Upload-Offset` が `received` と違う** | `409 {"error":"offset_mismatch","received":N}`（クライアントは、この `received` から続ける。**何も書き込まない**） |
| チャンクが `size` を超える | `400 {"error":"invalid_request"}`（状態の判定の後。`offset_mismatch` の後に判定する） |
| 再開した最初の `PUT` で、残りの大きさ + 予備が空きを超える（中断中に別のイメージが空きを使った）。または、書き込み中に「空きなし」 | `507 {"error":"insufficient_storage","free_bytes":N}` |
| 書き込みに失敗（SD のエラー） | `503 {"error":"sd_unavailable"}`（チャンクは `truncate` で取り消され、イメージは `incomplete` になる。SD が戻れば、`received` から再開できる） |

`incomplete` のイメージに `PUT` すると、`uploading` に戻る（再開）。**アップロードが `ISO_UPLOAD_IDLE_S` 秒（既定 300）なければ、`incomplete` になる**（再開できる。SHA-256 の状態は保つ）。**`uploading` のイメージを `DELETE` すると、アップロードは終わり、次の `PUT` は 404**。

#### 8.4 アップロードの完了 `POST /api/v1/iso/<id>/finish`
リクエスト: `{}`（ボディなし、または空のオブジェクト。ほかのキーは 400）。**最終処理（SHA-256 の確認、エクステントの作成、ISO の署名の検査）は、ワーカーが行う**。
| 結果 | 応答 |
|---|---|
| 受理（最終処理を始めた）。すでに `finalizing`（**冪等**: 何度呼んでもよい） | `202 {"state":"finalizing"}`（クライアントは `GET /api/v1/iso` で、`ready`、`invalid`、**`incomplete`** のどれかになるまで待つ。**`incomplete` になったら（最終処理中の SD の I/O エラー）、待つのをやめ、`sd.state` が `ready` に戻るのを待って、`received == size` なら `finish` を送り直し、`received < size` なら、先にアップロードを再開する**） |
| すでに `ready` | `200 {"state":"ready","sha256":"<64 桁の 16 進、または null>"}`（冪等） |
| `invalid`（以前の最終処理で失敗した） | `422 {"error":"<invalid_reason>"}`（`checksum_mismatch` / `not_iso` / `too_fragmented` / `part_mismatch` / `meta_corrupt`） |
| `id` が不正、そのイメージがない（`deleting` を含む） | `404 {"error":"not_found"}` |
| SD が使えない（`sd.state` が `ready` でない） | `503 {"error":"not_ready"}` |
| `received` が `size` に達していない（`uploading` / `incomplete`） | `409 {"error":"size_mismatch","received":N,"size":M}` |
| `incomplete` で `received` が `size` に達しているが、別のイメージが枠を使っている | `409 {"error":"upload_in_progress"}` |
| 処理中の SD の I/O エラー | `503 {"error":"sd_unavailable"}` |

最終処理の結果は、`GET` の `state` と `invalid_reason` で見る: 宣言した `sha256` と一致しない → `invalid`（`checksum_mismatch`）、ISO の署名（セクタ 16 の `CD001`、または UDF の `BEA01`）がない → `invalid`（`not_iso`）、エクステントが `ISO_MAX_EXTENTS` を超える → `invalid`（`too_fragmented`）、パーツの欠落やサイズの不一致 → `invalid`（`part_mismatch`）。**再起動で `finalizing` が失われた `incomplete`（`received == size`）は、ビューアが一覧を見て、自動で `finish` を送る**。

#### 8.5 マウント `POST /api/v1/iso/mount`
リクエスト: `{"id":"a1b2c3d4"}`（必須。ほかのキーがあれば 400。**`"force":true` は任意**: ターゲットが取り外しを防止していても、差し替える）。
| 結果 | 応答 |
|---|---|
| 受理 | `200 {"result":"mounted","id":"a1b2c3d4"}`（すでに同じイメージがマウント中なら `200 {"result":"already_mounted","id":"..."}`。何もしない） |
| ボディ不正、`id` の形式が不正 | `400 {"error":"invalid_request"}` |
| SD が使えない（`sd.state` が `ready` でない） | `503 {"error":"not_ready"}` |
| そのイメージがない（`deleting` を含む） | `404 {"error":"not_found"}` |
| そのイメージが `ready` でない（`finalizing`、`invalid`、`uploading`、`incomplete`） | `409 {"error":"image_not_ready"}` |
| **別のイメージがマウント中で、ターゲットが取り外しを防止している**（`force` なし） | `409 {"error":"locked_by_target"}` |

別のイメージがマウント中なら、**差し替える**（取り外し + マウント。ターゲットには、メディアの変更が 1 回だけ通知される）。USB が切断されていても受け付ける（メディアは、接続されたときに見える）。`mount` は、`target.ejected_by_target` を `false` に戻す。

#### 8.6 取り外し `POST /api/v1/iso/eject`
リクエスト: `{}`、または `{"force":true}`（ほかのキーは 400）。
| 結果 | 応答 |
|---|---|
| 取り外した | `200 {"result":"ejected"}`（`target.ejected_by_target` を `false` に戻す） |
| マウントされていない | `200 {"result":"not_mounted"}`（冪等。`ejected_by_target` を `false` に戻す） |
| ターゲットが取り外しを防止している（`force` なし） | `409 {"error":"locked_by_target"}` |

#### 8.7 削除 `DELETE /api/v1/iso/<id>`
どの状態のイメージも削除できる（`uploading` / `incomplete` のものは、アップロードの中止になる）。**ファイルの削除（FAT の連鎖の解放）に時間がかかるので、ワーカーが行い、`202` で返す**。進行中の `READ` が、取り外した直後のイメージのエクステントを参照している間は、ワーカーは削除を待つ（`ISO_MOUNT_DESIGN.md` §4.1）。
| 結果 | 応答 |
|---|---|
| 削除を始めた（`state` は `deleting`。完了すると一覧から消える）。すでに `deleting`（冪等） | `202 {"state":"deleting"}` |
| `id` が不正、そのイメージがない | `404 {"error":"not_found"}` |
| SD が使えない（`sd.state` が `ready` でない） | `503 {"error":"not_ready"}` |
| **マウント中** | `409 {"error":"mounted"}`（先に取り外す） |
| `import/` に、同じ先頭クラスタのファイルがある（`rename` の途中の電源断のクロスリンク） | `409 {"error":"crosslinked"}`（PC のチェックディスクで直す。`ISO_MOUNT_DESIGN.md` §3.3） |

削除は、`finalizing` のイメージにも掛けられる（ワーカーは、1 ステップごとに中止の印を見る）。**削除が終わるまで、そのイメージが使っていた空きは戻らない**（`free_bytes` は、完了時に更新）。

#### 8.8 取り込み（rescan）`POST /api/v1/iso/rescan`
リクエスト: `{}`、または `{"cursor":C}`（前回の応答の `next_cursor`。0 以上 65535 以下の整数。ほかのキーは 400）。PC で `/iso/import/` にコピーした ISO（4 GiB 弱まで）を、イメージとして取り込む（`ISO_MOUNT_DESIGN.md` §3.3）。**同期で返る**（1 つのファイルで、`mkdir`、`meta` の作成と同期、`rename` があり、数百 ms〜数秒かかる。そのため、1 回の処理を上限で抑える。**その間、同じサーバーの他の要求は待つ**）。先頭クラスタが既存のイメージと同じファイル（`rename` の途中の電源断のクロスリンク）は、取り込まず `skipped` に数える。
| 結果 | 応答 |
|---|---|
| 完了（1 回に、エントリを最大 `ISO_RESCAN_BATCH`（既定 4）個まで調べる） | `200 {"imported":N,"skipped":M,"next_cursor":C'}`（`imported` = 取り込んだ数、`skipped` = 規則に合わずに飛ばした数、`next_cursor` = 次の呼び出しの `cursor`（**調べていないエントリがなければ `null`**））。**ビューアは、`next_cursor` が `null` になるまで、繰り返す**（各回で、取り込むか、位置が進むので、必ず終わる）。取り込んだものは、エクステントの作成のため `finalizing` を経て `ready` になる |
| ボディ不正 | `400 {"error":"invalid_request"}` |
| SD が使えない（`sd.state` が `ready` でない） | `503 {"error":"not_ready"}` |
| アップロードの枠が使用中（`uploading` または `finalizing`） | `409 {"error":"upload_in_progress"}` |

**判定の順序**（複数当てはまるときの優先。**この 13 段の順が正で、各表の行の並びは読みやすさの順**。テストで固定する）:
1. 認証（401）→ 2. Origin（403。`POST` / `PUT` / `DELETE`）→ 3. パスの `id` の形式不正（404 `not_found`）→ 4. `Content-Type`（415）→ 5. `PUT` の `Content-Length` の欠落（411）、JSON のボディの大きさ（413）、`PUT` の `Content-Length` が `chunk_size` 超過（413）→ 6. 形式（400。ボディの `id`、`X-Upload-Offset`、`Content-Length` の値を含む）→ 7. SD が使えない（503 `not_ready`。**例外: `eject` は 200 `not_mounted`**）→ 8. そのイメージがない（404）→ 9. 状態の矛盾（409: `not_uploading`、`image_not_ready`、`mounted`、`crosslinked`、`locked_by_target`、`size_mismatch`、`upload_in_progress`、`name_exists`、`too_many_images`。**同時に当てはまるときは、この並びの順**）→ 10. `offset_mismatch`（409）→ 11. 状態に依存する形式（400: チャンクが `size` を超える）→ 12. 空き容量（507。キャッシュ値）→ 13. 処理（受理。I/O エラーは 503 `sd_unavailable`）。
**`offset_mismatch` を、書き込みの前に判定する**（何も書かない）。`PUT` の 3〜12 は、本体を読む前に判定できる（純 C の `iso_api_put_precheck`。`INTERFACES.md` 7.4）。

**ログ**: 操作（`begin`、`finish`、`mount`、`eject`、`delete`、`rescan`）の結果と理由を、イメージの `id` と大きさだけ記録する（`ESP_LOGI`。**名前、セッション情報、データは出さない**）。`PUT` のチャンクごとのログは出さない。進捗は 5% ごと。`GET` の成功は記録しない。

**サービスの起動**: `ISO_ENABLE` が有効でも、SD と USB の MSC の実装（A-13、A-14）が呼ばれるまで、`GET /api/v1/iso` は `sd.state: "no_card"` を返し、ほかは認証と形式の検査を通ったうえで `503 not_ready` を返す。

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
