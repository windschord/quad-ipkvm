# 映像の配信（フレームの大きさと、遅いクライアントの扱い）

Issue #82（A-7 後続）の ②③ への回答。実機の測定は `docs/HW_VERIFY.md` §E（JPEG の最悪サイズ）と §Z（`slowclient.py`）。オーナーの決定（2026-11-14）: **A-1**（フレームの上限を 768 KB に、品質の段階的な引き下げ）と **B-3**（1 フレーム全体の送信期限 + 映像専用の `httpd`）。

## 1. 測定の要点
出典: **PR #104（Antigravity。マージ待ち）の `docs/HW_VERIFY.md` §E と §Z**（実機、2026-11-14。統合ブランチの `HW_VERIFY.md` には、まだ無い）。値は、そこに書かれた条件（HW JPEG エンコーダ、入力 `RGB888`、`q=80/70/60/50`、静止画の 1080p を PSRAM 経由でエンコード、`slowclient.py`）での測定。
- **測定**: 1080p の IDE + ターミナル画面は、`q=70` で JPEG が **599,710 B（585.7 KiB）**、`q=50` でも 441,657 B（431.3 KiB）。現在の上限 256 KiB では、すべてのフレームが `oversize_drop` になる。
- **測定**: 約 112 KiB（1280x720 の実映像、TLS 約 7 レコード）のフレームで、`slowclient.py` の `40 KB/s` のとき、`/status` の最大が **5,127 / 6,580 ms**（30 秒 / 15 秒のラン）、通常のクライアントの fps が **1.1 / 2.0**、**通常のクライアントも遅いクライアントも、サーバーに閉じられた**。`10 KB/s`、`2 KB/s`、`no read` では、遅いクライアントだけが閉じられ、通常のクライアントは 11〜12.5 fps を維持した。
- **推定（実測で確認していない）**: 112 KiB のフレームを 40 KB/s で送ると、1 フレームに約 2.8 秒かかる（112 KiB ÷ 40 KB/s の計算）。**映像の送信は `httpd` の 1 つのタスクで順に行われ**、`ws_video_core` の送信停止の判定（3000 ms）が、**作業キューに入れた時点から**数えているため、前の送信が待たせた分まで通常のクライアントの停止に数えられる — これは、コードの読み取りと、Antigravity のシリアルログの解釈からの**推定**。B-3 の実装後に、device-check で確認する（§6）。

## 2. A-1: フレームの上限と、品質の段階的な引き下げ
- `FRAME_HUB_MAX_FRAME_KB` の既定（PSRAM 有効時）を **768** に上げる（**反映済み**: §4 の送信経路〔PR #111〕のマージ後に上げた）。当初の方針: **この PR では上げない**: 256 KiB を超えるフレームが、現行の送信経路（§4 の部分書き込みの問題）に流れ込むのを避けるため、**§4 の送信経路を入れる PR で、同時に上げる**（3 スロットで 2.25 MiB。PSRAM の空きは約 19.8 MiB）。値は `TODO(hw-verify)`（実際の 1080p で `oversize_drop = 0` か）。
  **既存の `sdkconfig` は、以前の値（256）を保持している**ので、`menuconfig` で 768 に直すか、`sdkconfig` の該当行を消して再生成する（`docs/INTERFACES.md` §2 の規則: HDMI 担当が値の変更を提案してよい）。
- **品質の制御は、HDMI 担当（`csi_video_source`）が実装する**（`JPEG_*` の領域）。仕様:
  - **純 C のコントローラー**（`jpeg_quality_ctrl`、ホストでテストできる）。入力: 直近のフレームの JPEG サイズ、`max_frame_bytes`、設定の品質 `q_cfg`。出力: 次のフレームの品質 `q`。
  - **次のフレームから**下げる（同じフレームを再エンコードしない。1080p のエンコードは約 38 ms で、30 fps のフレーム予算に収まらない）。`size > max_frame_bytes` なら `q -= 10`（**下限は `min(30, q_cfg)`**。設定の `q_cfg` が 30 未満のとき、下限で品質が上がらないようにする）。`size > 85%` でも `q -= 5`（先回り）。
  - **戻す**: 連続した N フレーム（既定 90）が、上限の 60% 未満なら `q += 5`（上限 `q_cfg`）。上げ下げの振動を避けるため、下げた直後は戻さない（N フレームの数え直し）。
  - 上限を超えて捨てたフレームは、`oversize_drop` に数える（現状どおり）。`q` と、下げた回数をログの統計に出す。
  - 値（85%、60%、N、段、下限）は `TODO(hw-verify)`。

## 3. B-3 (a): 送信時間の数え方と、1 フレーム全体の期限（`ws_video_core`、純 C）
**問題**: `send_started_ms` を、`httpd` の作業キューに入れた時点で設定している。前の送信が `httpd` のタスクを占有している間（別のクライアントのせい）、キューで待つ時間まで、このクライアントの「送信停止」に数えられる。

**契約**:
1. **「キューに入れた」と「実際に送り始めた」を分ける。** `ws_video_core_on_send_begin(core, fd, now_ms)` を新設し、IDF 側の作業関数が、実際に送り始めるときに呼ぶ。`send_started_ms` はそこで設定する。送り始める前の待ち（キュー待ち）は、**別の上限** `queue_timeout_ms`（既定 10000）だけで見る（`httpd` が止まったときの保険。通常は到達しない）。
2. **1 フレーム全体の期限**: 送信を始めてから、フレーム全体を送り終えるまでの上限を、フレームの大きさで決める。
   `deadline_ms = frame_deadline_base_ms + size_bytes * 1000 / min_rate_bytes_per_s`
   （既定: `base` = 200 ms、`min_rate` = 1,000,000 B/s（8 Mbps）。112 KiB のフレームは約 315 ms、585 KiB は約 800 ms。約 2 Mbps 未満の回線は、112 KiB のフレームでも切断される。`TODO(hw-verify)`）。純 C の関数 `ws_video_frame_deadline_ms()`（飽和する。`min_rate = 0` は無効 = 期限なし）。
3. **期限を超えたら、そのクライアントを閉じる**（フレームを部分的に送ったままにしない）。ほかのクライアントには影響しない。
4. 既存の `send_timeout_ms`（送信の停止）は残す（期限とは別: 期限は「合計」、停止は「1 回の送信が進まない」）。

## 4. B-3 (b): 分割送信（IDF 側。クライアントごとに交互に送る）
`httpd_ws_send_data_async()` は、フレーム全体を 1 回の呼び出しで送り切るので、途中で期限を確認できない。さらに、**TLS の送信が途中まで成功して書き込めなくなったとき、正の部分書き込み長を返す**が、この経路が残りを再送せずに完了を通知するおそれがある（**要確認**: ESP-IDF v6.0.1 の `esp_http_server` のソースで確かめる。フレームの上限を上げると、起こりやすくなる）。次のように、`/video` の送信を自前の経路に置き換える:
- **送信の状態をクライアントごとに持つ**（フレームのポインタ、送信済みのバイト数、ヘッダーを送ったか）。
- 作業関数（`httpd_queue_work()` で `httpd` のタスクに載せる）は、**1 回の呼び出しで、最大 1 スライス**（`WEB_SERVER_WS_SLICE_BYTES`、既定 16 KiB）だけ送る。WS のヘッダー（`0x82` + 長さ）は自前で組み立て、最初のスライスの前に送る。`httpd_socket_send()` の**部分書き込み（戻り値が要求より小さい）を、残りを次のスライスに回して**扱う（全ペイロードを送り切ってから、完了を通知する）。**送り終わっていなければ、自分自身を作業キューの末尾に積み直す**。これで、**ほかの映像クライアントの作業が、スライスの間に割り込める**（作業キューは FIFO）。
- 映像のソケットの `SO_SNDTIMEO` は、1 スライスの送信が詰まったとき、ほかのクライアントを待たせすぎないように、**短くする**（`WEB_SERVER_WS_SOCK_SNDTIMEO_MS`、分割送信では既定 20 ms。`TODO(hw-verify)`）。詰まったスライスは、送れた分だけ進め、残りを積み直す。
- 1 フレームの期限（§3）は、**送り始めから送り終わりまで**（積み直しの待ちを含む）で数える。期限を超えたら、`ws_video_core_on_send_done(…, false)` を呼び、`io_request_close` で閉じる。
- 1 つのクライアントが、ほかのクライアントを待たせる最悪の時間は、**1 スライス分（`SO_SNDTIMEO` 以下 + 暗号化の時間）**。
- **`/ws`（入力）の送信は変えない**（数バイトで、`httpd_ws_send_data_async` のままでよい）。

## 5. B-3 (c): 映像専用の `httpd`
目的: 遅い映像クライアントが、`/status`、ログイン、`/ws`（入力）を止めない。

- **2 つ目の HTTPS サーバー**（`httpd_ssl_start`、同じ証明書）を、別のポート **`WEB_SERVER_VIDEO_PORT`（既定 8444）**で起動する。`ctrl_port` も、1 つ目と違う値にする。**映像用のサーバーは `/video`（WebSocket）と、`GET /`（下の「証明書の信頼」用の小さなページ）だけ**を持つ。1 つ目のサーバーから `/video` は**なくす**（ビューアが新しいポートに接続する）。
- **認証は同じ**: セッションの Cookie（`__Host-session` は、ポートを区別しない。`SameSite=Strict` も、ポートが違っても同一サイトとして送られる）を、同じ `auth_logic` で検証する（`s_auth_mutex`、ロック順序は変えない）。ログアウトとセッション切れでの切断も、同じ。
- **Origin の規則（セキュリティ上の要点）**: ブラウザの `Origin` は、ページの Origin（`https://host`、メインのポートが 443 以外なら `:ポート` つき）で、映像サーバーの `Host`（`host:8444`）と**ポートだけが違う**。新しい純 C の関数 `http_guard_origin_ok_for_port(origin, host, page_port)`（`http_guard`）を作る: **ホスト名（ポートを除く）が `Host` のホスト名と一致**し、**Origin のポートがメインのポート（Origin に無ければ 443）と一致**すること。それ以外（`null`、`@`、IPv6、末尾の `.`、別のポート、別のスキーム）は拒否する（`http_guard_origin_ok` の既存の厳しさを保つ）。`Host` の許可リスト（`WEB_SERVER_ALLOWED_HOSTS`）の検査は、ポートを除いて、同じ。
- **ソケット**: 1 つ目のサーバーの `max_open_sockets` から、映像の分を減らし、2 つ目に `WEB_SERVER_WS_MAX_CLIENTS + 2` を割り当てる。リッスン用と、制御用の分も含め、`CONFIG_LWIP_MAX_SOCKETS`（16）の `_Static_assert` を更新する（足りなければ、`sdkconfig.defaults` の値を上げる。既存の `sdkconfig` は再生成が必要）。タスクのスタックは `WEB_SERVER_VIDEO_STACK_SIZE`（既定 8192、`TODO(hw-verify)`）。
- **ビューア**（`app.js`）: `wss://` + `location.hostname` + `:` + 映像のポート + `/video`。ポートは、状態の取得（`GET /api/v1/auth/status`）の応答に `video_port` を足して渡す（ビューアが、ポートを推測しない）。
- **証明書の信頼（自己署名のため）**: 自己署名の証明書の例外は、ブラウザによって、ホスト単位（Chrome）かホスト + ポート単位（Firefox、Safari）。**別のポートの WebSocket は、例外を受け入れていないと、静かに失敗する**。そのため、映像の接続が失敗したとき、ビューアは「`https://host:8444/` を開いて証明書を受け入れてから、再読み込み」というリンクを表示する。`GET /`（映像サーバー）は、静的な小さなページ（「この接続を信頼しました。元のページへ戻ってください」+ 元の Origin へのリンク）を返す。**このページは、認証なしで返してよい**が、本文に動的な値（Cookie、状態）を含めない。
- 終了と再起動: 2 つのサーバーの `close_fn` を、それぞれ映像用 / 入力用の核に結びつける（既存の `ws_video_close_fn` を、映像サーバーの `close_fn` にする）。

## 6. 検証
- ホスト（純 C）: `ws_video_core`（`on_send_begin`、キュー待ちの別上限、期限の関数、期限超過でそのクライアントだけを閉じる）、`http_guard_origin_ok_for_port`（表のテスト + ミューテーション）、`jpeg_quality_ctrl`（HDMI 担当）。
- 実機（device-check。Antigravity）: `slowclient.py` の `40 KB/s` で、**通常のクライアントが 10 fps 以上を維持し、切断されない**（遅いクライアントのスライスが交互に入る分の遅れを含めて）、`/status` が 1 秒を超えない。§1 の「推定」（キューの待ちが原因）が正しいかも、ここで確認する（送信の開始のログと、期限の超過のログを見る）。`10 KB/s`・`2 KB/s`・`no read` で、遅いクライアントだけが、期限で閉じられる。1080p の高密度画面で `oversize_drop = 0`（品質の引き下げが働く）。Chrome と Firefox で、証明書の例外を受け入れる手順（上の「信頼」のページ）が働く。メモリ（ヒープの空き、2 つ目のサーバーのスタック）。

## 7. 担当と順序
1. この契約（Claude。`FRAME_HUB_MAX_FRAME_KB` の既定の引き上げを含む）。
2. `ws_video_core` の変更と `http_guard_origin_ok_for_port`（純 C、ホストでテストできる。Claude のサブエージェント）。
3. `web_server` の結線: チャンク送信、映像専用の `httpd`、`app.js`、`auth/status` の `video_port`（Claude のサブエージェント。IDF のビルドは、device-check で確認する）。
4. `jpeg_quality_ctrl` と結線（Antigravity）。
5. device-check（Antigravity）。

## 8. 映像の状態の通知（C-6b。オーナー決定: `/video` にテキストフレームで混ぜる）
Issue #86。`video_status`（`docs/INTERFACES.md` §1.3）の状態を、ビューアへ、映像の WebSocket の**テキストフレーム**で送る。形式とタイミングは `docs/WEB_API.md` §5。

**純 C（`ws_video_core`、`video_status`）**:
- `video_status_format_json(const video_status_t *st, char *out, size_t cap)`: `{"t":"video","state":"…","w":N,"h":N}` を作る（128 バイト以下。バッファ不足は -1。`state` が範囲外なら `unknown`、`locked` 以外は `w` = `h` = 0）。
- `ws_video_core_config_t` に `status_get`（`void (*)(void *user_data, video_status_t *out)`。`NULL` なら状態の通知なし = 従来どおり）と `io_begin_send_text`（`int (*)(void *user_data, ws_video_conn_t conn, const uint8_t *data, size_t len)`）を足す。
- クライアントごとに「最後に送った状態」を持つ。**接続直後は、必ず 1 回送る**（`status_sent = false`）。`tick` で、そのクライアントが**送信中でなく**、かつ**現在の状態が最後に送ったものと違う**（状態、または `locked` の解像度）とき、テキストフレームを `io_begin_send_text` で送る。**フレームの送信（`is_sending`）と同じ仕組みで数える**（同時に 2 つ送らない）ので、テキストの送信が終わる（`on_send_done`）まで、次のフレームは送らない。テキストの送信が失敗したら、**そのクライアントを閉じる**（フレームの送信の失敗と同じ）。
- 同じ `tick` で、状態の通知と映像のフレームの両方が送れるときは、**状態の通知を先に**送る（ビューアが、フレームを描く前に、状態を知れる）。
- `on_send_done` は、テキストの送信とフレームの送信を区別する（テキストには、解放するフレームがない）。期限（§3）は、テキストにも適用する（`size` = テキストの長さ）。
- `status_get` は、`tick` から呼ばれる（ロックの下。ブロックしない。上のアダプター経由で `video_status_get()` を呼ぶ）。状態が `UNKNOWN` → `LOCKED` などの揺れで、通知が頻発しないよう、**同じ状態の変化は、最短 `status_min_interval_ms`（既定 200）以内に 2 回送らない**（間隔の間の変化は、間隔が経った後の最新の状態を 1 回だけ送る）。 **途中の状態は送らず、間隔の間に元の状態に戻れば、何も送らない**（`docs/WEB_API.md` §5 と同じ。通知は最大 `status_min_interval_ms` 遅れうる。揺れの抑制を、即時性より優先する）。

**ビューア（`app.js`）**:
- テキストフレーム（文字列）を JSON として解析し、`t === "video"` のものだけを使う。解析に失敗したテキストは捨てる。バイナリは、従来どおり JPEG。
- 表示: `locked` は、映像のステータス行に、解像度（例: `1280x720`）を表示。`no_signal` は、キャンバスの上に「信号なし」を重ねる（直前のフレームを残すかは、実機で見て決める。既定は残して、半透明の表示）。`unknown`（確認中）は「映像の状態を確認中…」。`locked` から別の解像度の `locked` に変わったときは、短く「解像度が変わりました」。
- 状態の値の検査: `state` は 3 つの文字列だけ、`w`・`h` は 0〜65535 の整数。それ以外は、無視する。

**IDF 側（`web_server.c`。Antigravity の B-3 の PR #111 のマージの後）**: テキストフレームを送る関数（`io_begin_send_text`）。分割送信の経路（§4）の、ヘッダーのオペコードを、テキスト（`0x81`）にするだけ。128 バイトなので、1 スライスで送り切れる。`status_get` には、**2 引数のアダプター**（`void (*)(void *user_data, video_status_t *out)`。`user_data` は使わず、`video_status_get(out)` を呼ぶだけ）を渡す（`video_status_get` は 1 引数なので、直接は渡せない）。`max_frame_size` の確認は不要。`slowclient.py` の `40 KB/s` で、**状態の通知が、通常のクライアントに影響しない**ことを確認する。

**検証**: ホスト（純 C のテスト + ミューテーション、ビューアの node のテスト）。実機（device-check）: 映像の入力を抜き差しして、ビューアの表示が変わること、`slowclient.py` で通常のクライアントの fps が変わらないこと。


## 9. P7: H.264 低帯域モード（契約。オーナー指示: 2026-11-16 に P7 を進める）
評価は `docs/P7_H264_EVAL.md`（Antigravity、PR #109。実測とソースの確認）。**この章は、評価を踏まえた契約**。数値は、評価の実測（`[確認]`）と、ここでの決め（`TODO(hw-verify)`）を分ける。

### 9.1 位置づけ
- **MJPEG が既定のまま**。H.264 は**低帯域の副モード**（WAN / VPN / SSH トンネルなど）。実測では、PPA の `RGB888 → YUV420`（rev v1.3 で必須）が律速で、**fps は MJPEG より下がる**（720p で最大 約 20.5 fps、1080p で 約 9.1 fps。MJPEG は 720p 30 fps、1080p 15 fps）。一方、静止画の P フレームは 93〜592 B（MJPEG の約 1/200〜1/4000）。**帯域を優先するモード**であり、操作の滑らかさを優先するなら MJPEG を使う。
- 同時に 2 つのコーデックは作らない（1080p で JPEG と H.264 を両方作ると約 147 ms/フレーム = 7 fps 未満で、メモリも二重になる）。**映像ソースが作るコーデックは、常に 1 つ**。

### 9.2 ストリームの形式
- **H.264 Baseline、B フレームなし、Annex B**。`WS /video` の 1 バイナリメッセージ = **1 アクセスユニット（1 フレーム分の NAL をまとめたもの）**。IDR のアクセスユニットは、先頭に SPS と PPS を含む（`esp_h264` の HW エンコーダが自動で付ける）。P のアクセスユニットは、スライスだけ。
- 1080p は、高さを 16 の倍数（1088）に揃えてエンコードし、SPS のクロッピングで 1080 に戻す（評価 2.2）。**ビューアは、クロッピングを解釈するデコーダ（WebCodecs）に任せる**。
- フレームの大きさは `FRAME_HUB_MAX_FRAME_KB`（既定 768）に収まること。実測の 1080p IDR は最大 507 KB。**超えたフレームは捨て、次のフレームを IDR にする**（9.6）。

### 9.3 コーデックの識別: ペイロードで自己識別する（ヘッダーは足さない）
- JPEG は `FF D8` で始まり、H.264 は `00 00 00 01`（スタートコード）で始まる。**ビューアは、バイナリメッセージの先頭バイトで判別する**。既存の MJPEG の経路と、古いビューアは、**変更なしで動く**（追加のバイトも、新しいテキストフレームもない）。
- キーフレームかどうかは、**ビューアが NAL の種類で判断**する（IDR のアクセスユニットは、先頭の NAL が SPS（type 7）。P は slice（type 1））。**サーバー側は `video_frame_t.flags`（`VIDEO_FRAME_FLAG_KEY`）で持つ**（ペイロードを読まない）。
- WebCodecs の codec 文字列は、**IDR の SPS の先頭 3 バイト**（`profile_idc`、制約フラグ、`level_idc`）から、ビューアが作る（例: `42 c0 1f` → `avc1.42c01f`）。サーバーは、codec 文字列を送らない。

### 9.4 クライアントの申告: `GET /video?accept=h264`
- クエリ `accept` は、カンマ区切りで、**そのクライアントが受け取れる、JPEG 以外のコーデック**。いまは `h264` だけ。**JPEG（MJPEG）は、常に受け取れる前提**（`accept` に書かない）。`accept` がない・空・未知の値だけ = **MJPEG だけ**（古いビューアと同じ）。未知の値は、無視する（エラーにしない）。
- ビューアは、`VideoDecoder` があり、`VideoDecoder.isConfigSupported()` が（SPS が分かる前は、`avc1.42c01f` で仮に）真のときだけ `accept=h264` を付ける。**自己署名の HTTPS は、WebCodecs の Secure Context の条件を満たす**（評価 4.1）。

### 9.5 サーバーの方針: `video_codec_policy`（純 C）
**全クライアントに配るコーデックは 1 つ**。次の条件を**すべて**満たすときだけ H.264。それ以外は MJPEG。

| 条件 | 内容 |
|---|---|
| 許可 | 設定 `VIDEO_CODEC_H264_ALLOWED`（Kconfig。既定 `n`。`n` のとき H.264 は決して選ばれない = 現在の動作と同じ） |
| 能力 | 映像ソースが H.264 を作れる（`video_source_t.supported_codecs` に H.264 が入っている） |
| 申告 | 接続中のクライアントが 1 つ以上で、**全員が `accept=h264`** |
| 入力 | 映像の状態が `LOCKED` で、解像度が 1920x1080 以下 |
| 安定 | MJPEG から H.264 に上げるときは、上の条件が `VIDEO_CODEC_UPGRADE_DWELL_MS`（既定 5000。`TODO(hw-verify)`）続いたとき |

- **H.264 から MJPEG へ下げるのは、即時**（`accept=h264` のないクライアントが接続したとき、または条件のどれかが外れたとき）。JPEG は自己完結のフレームなので、下げるときの待ちはない。
- 接続が 0 個になったら、**コーデックは変えない**（次の接続で、条件を評価し直す）。
- 方針は、**状態を持つ純 C**（`now_ms` と、クライアントの申告の集合を入力にして、`desired_codec` を返す）。ホストで、ミューテーションまで検証する。
- **コーデックの切替を、映像ソースに伝える**: `video_source_t.set_codec()`（9.7）。切替は**非同期**で、効くのは次に publish するフレームから。切替の後の最初の H.264 フレームは、必ず IDR。

### 9.6 キーフレームの規則（`ws_video_core`）
H.264 では、P フレームは直前のフレームに依存する。**`frame_hub` は「遅いクライアントには最新へ飛ばす」設計のまま**（変えない）ので、飛ばしたら、次の IDR まで待つ。
- クライアントごとに `need_key` を持つ。**次のときに真にする**: (a) 接続したとき（コーデックに関係なく。`ws_video_core` は接続時のコーデックを知らない。MJPEG のモードでは、映像ソースの `request_keyframe` は何もしない〔契約 §9.7〕）、(b) 次に配る `frame->seq` が、そのクライアントの最後に配った `seq` + 1 でないとき（飛ばした、または、リングに上書きされた）、(c) コーデックが MJPEG から H.264 に切り替わったとき。
- `need_key` が真のクライアントは、**H.264 の P フレームを配らない**（`hub_release` して、次の tick を待つ）。IDR（`VIDEO_FRAME_FLAG_KEY`）が来たら配り、`need_key` を偽にする。**JPEG のフレームは、`need_key` に関係なく配る**。
- `need_key` を真にしたら、**`request_keyframe` を要求する**（`video_source_t.request_keyframe()`、9.7）。**要求は、tick の最後に出る（接続の直後ではない）**。要求は、**クライアントをまたいで、1 つにまとめる**（`VIDEO_CODEC_IDR_MIN_INTERVAL_MS`、既定 1000。`TODO(hw-verify)`: IDR は 495 KB に達するので、要求のたびに送ると、遅いクライアントを、さらに遅くする）。
- **エンコードに成功して、公開（`frame_hub_begin_publish`）に失敗したとき**、またはサイズ超過で捨てたときは、**映像ソース自身が、次のフレームを IDR にする**（`seq` が連続して見えるので、受信側では検出できない。評価 4.2）。
- 保険として、**映像ソースは、一定間隔で IDR を出す**（`CSI_H264_GOP`、既定 = fps × 2。`TODO(hw-verify)`。**`esp_h264` の `gop` は `uint8_t`（1〜255）なので、Kconfig の範囲は `1 255`**）。要求の取りこぼしがあっても、最長でこの間隔の後に復帰する。
- `need_key` が長く続く（IDR が来ない）クライアントは、`send_timeout` とは別に、**`VIDEO_CODEC_KEY_WAIT_MS`（既定 5000）で閉じる**（再接続させる）。**起点は、「H.264 の P フレームを、最初に配らなかった時刻」**（接続した時刻ではない。信号なしなどでフレームが来ない間に、MJPEG のクライアントまで閉じないため）。

### 9.7 契約の変更（`docs/INTERFACES.md` §1.4 に反映）
- `video_frame_t` に `uint8_t codec`（`VIDEO_CODEC_MJPEG = 0`、`VIDEO_CODEC_H264 = 1`）と `uint8_t flags`（`VIDEO_FRAME_FLAG_KEY = 1`）を**足す**。**0 が MJPEG・キーフレームでない**ので、足す前のコード（`memset` したフレーム）は、MJPEG として動く。
- `frame_hub_commit` は**変えない**（`codec = MJPEG`、`flags = KEY`。JPEG は、どのフレームも自己完結）。**`frame_hub_commit_ex(hub, frame, const frame_hub_commit_info_t *info)`** を足す（`info` に `size`、`width`、`height`、`timestamp_ms`、`codec`、`flags`）。
- `video_source_t` に、**構造体の末尾（`ctx` の後）に**足す（位置指定の初期化が、そのまま通る）: `uint32_t supported_codecs`（ビットマスク。0 = MJPEG だけ）、`int (*set_codec)(const video_source_t *, video_codec_t)`（0 = 受理。**非同期、ブロックしない、複数回呼んでよい**）、`void (*request_keyframe)(const video_source_t *)`（**ブロックしない、複数のタスクから呼んでよい、取りこぼしても保険の IDR がある**）。どちらも `NULL` でよい（MJPEG だけのソース）。
- Kconfig の接頭辞: **`CSI_H264_*`** = HDMI 担当（エンコーダ、GOP、ビットレート、`CSI_H264_ENABLE` 既定 `n`）。**`VIDEO_CODEC_*`** = Claude（方針、上の 9.5・9.6 の値）。`INTERFACES.md` §2 の表に足す。

**H.264 のメモリ**（Antigravity のレビュー、2026-11-16）: 1080p の HW H.264 は、内部 RAM 約 151 KiB + PSRAM 約 3.0 MiB を追加で使う（評価 3.2）。実機の `/video` ×2 + `/ws` ×2 のとき、内部 RAM の最小は 391,215 B だった。**H.264 を有効にしたときの内部 RAM の余裕**と、**切替のたびに確保する案と、起動時に 1 回確保する案の、PSRAM の断片化**は、`csi_h264` の実装時に device-check で確認する（`TODO(hw-verify)`）。

### 9.8 ビューア（`app.js`）
- `accept=h264` を付けて接続し、バイナリメッセージの先頭で JPEG / H.264 を判別する（9.3）。H.264 は、`VideoDecoder`（`optimizeForLatency: true`）に、`EncodedVideoChunk`（IDR は `key`、P は `delta`）で渡し、出てきた `VideoFrame` を canvas に描いて、`close()` する。**最初の IDR まで、P は捨てる**。デコーダがエラーになったら、`accept` を付けずに再接続する（= MJPEG に落とす。**以後、そのタブでは `accept` を付けない**）。
- 解像度が変わったとき（SPS が変わる）は、デコーダを作り直す。表示に「H.264（低帯域）」と出す。
- **実装（`app.js`。検査は `docs/VIDEO_VIEWER_TESTS.md` 7 章）で決めたこと**:
  - `accept=h264` を付ける条件は、(a) `VideoDecoder`・`EncodedVideoChunk`・`VideoDecoder.isConfigSupported` がある、(b) 起動時の確認（`isConfigSupported({codec: "avc1.42c01f", optimizeForLatency: true})`）が `supported === true`、(c) このタブで H.264 が一度も失敗していない。最初の接続は、確認が終わるのを待つ（確認が終わらないときは 1500 ms で、`accept` なしで接続する。`TODO(hw-verify)`）。
  - **`accept` を付けた接続だけ** `binaryType = "arraybuffer"`（先頭バイトの判別と、H.264 の到着順を保つため）。JPEG は `new Blob([bytes], {type: "image/jpeg"})` にして従来の `createImageBitmap` に渡す。**`accept` を付けない接続は、従来のまま**（`blob`、先頭バイトは見ない）。
  - IDR は、SPS（type 7）または IDR スライス（type 5）を含むアクセスユニット。デコーダの作り直しは、SPS のバイト列が変わったとき（codec 文字列が同じでも、解像度が変われば作り直す）。IDR で SPS が来たら、**その codec 文字列を `isConfigSupported` で確認してから** `configure` する（確認の間のアクセスユニットは捨てる。確認が偽、`configure` / `decode` の例外、`error` コールバックはフォールバック）。
  - フォールバックは、`WebSocket` を `close()` して、既存の再接続（`fetchStatus` を、既定 1000 ms の後に）に任せる。**そのタブでは 1 回だけ**。
  - `decodeQueueSize >= 3`（`TODO(hw-verify)`）のとき、次の IDR まで P を捨てる。**ビューアからサーバーへ送れるデータはない**（送ると切断される。`docs/WEB_API.md` §5）ので、IDR の要求はできず、次の IDR（`CSI_H264_GOP`）まで待つ。
  - 表示は、映像のステータス行に ` / H.264（低帯域）` を足す（最初の `VideoFrame` を描いたときから。JPEG が来たら消える）。C-6b の状態表示（`VideoStatus`）は、コーデックに関係なく、そのまま動く。

### 9.9 担当と順序
1. この契約（Claude）。**オーナーの確認が要る**: 9.5 の方針（同時接続のコーデック）。選択肢と比較は 9.10。
2. 純 C とビューア（Claude のサブエージェント。ホストでテストできる）: (a) `frame_hub` の `codec` / `flags` / `commit_ex`、(b) `video_codec_policy` と、`ws_video_core` の `need_key`（キーフレーム待ち）と `request_keyframe` の要求のまとめ、(c) ビューアの H.264 の経路（フェイクの `VideoDecoder` でテスト）。
3. `web_server.c` の結線: `accept` の解析、コーデック方針の呼び出し、`request_keyframe` の配線。（Claude のサブエージェントが書き、Antigravity が device-check する。IDF のビルドは、この環境にない。）
4. `csi_h264`（PPA の `RGB888 → YUV420` + `esp_h264`）、`video_source_t` の `set_codec` / `request_keyframe`、IDR の周期と、公開の失敗時の自己要求（Antigravity）。
5. device-check（Antigravity）と、ブラウザでの確認（オーナー: Chrome / Safari / Firefox。長時間の再生、チャンネルの切替からの復帰、低帯域での体感）。

### 9.10 同時接続のコーデック方針の選択肢（オーナーが変更してよい。おすすめ順）
2 台まで同時に見られ（`WEB_SERVER_WS_MAX_CLIENTS`）、エンコーダは 1 つ。
| # | 方針 | 長所 | 短所 |
|---|---|---|---|
| 1（**この契約**） | **全員が受け取れるときだけ H.264、そうでなければ MJPEG に自動で下げる**（9.5） | 拒否がない。古いビューアも、ブラウザの非対応も、自動で MJPEG に落ちる。既定 `n` なら、現在と同じ | 接続の出入りで、コーデックが変わる（待ちの 5 秒で、揺れを抑える）。切替のたびに IDR（大きい） |
| 2 | **設定だけで決める**（`H264_ALLOWED` が `y` なら、常に H.264） | 最も単純。切替の揺れがない | 非対応のブラウザは映らない（ビューアが MJPEG の再接続を求めても、サーバーが応じない） |
| 3 | **先に接続したクライアントに合わせ、合わないクライアントは拒否**（WebSocket の Close 1008） | エンコーダの切替がない | 2 台目が拒否される。操作が分かりにくい |
| 4 | **H.264 は、接続が 1 つのときだけ**（2 つ目が来たら MJPEG） | 帯域を使うのは 1 人のとき、が分かりやすい | 2 つ目の接続のたびに、1 人目の映像が切り替わる |
| 5 | **API で、管理者が、モードを切り替える**（`POST /api/v1/video/mode`） | 意図した切替だけが起きる | UI と API が増える。忘れると MJPEG のまま |
| 6 | **クライアントごとに、別のコーデックを作る**（二重にエンコード） | 全員が最適 | 1080p で 7 fps 未満・メモリが二重。**不採用** |
