# 映像の配信（フレームの大きさと、遅いクライアントの扱い）

Issue #82（A-7 後続）の ②③ への回答。実機の測定は `docs/HW_VERIFY.md` §E（JPEG の最悪サイズ）と §Z（`slowclient.py`）。オーナーの決定（2026-11-14）: **A-1**（フレームの上限を 768 KB に、品質の段階的な引き下げ）と **B-3**（1 フレーム全体の送信期限 + 映像専用の `httpd`）。

## 1. 測定の要点（実機。値の出典は HW_VERIFY.md）
- 1080p の高密度な画面（IDE・ターミナル）は、`q=70` で JPEG が **585.7 KiB**、`q=50` でも 431 KiB。現在の上限 256 KiB では、すべてのフレームが捨てられる。
- 約 112 KiB（720p の実映像）のフレームは、TLS の約 7 レコード。`40 KB/s` の遅いクライアントは、1 フレームに約 2.8 秒かかる。**映像の送信は、`httpd` の 1 つのタスクで順に行われる**ので、通常のクライアントの送信が待たされ、`ws_video_core` の送信停止の判定（3000 ms）が、**キューに入れた時点から**数えているために、**通常のクライアントまで切断される**（`/status` も 5〜6 秒止まる）。

## 2. A-1: フレームの上限と、品質の段階的な引き下げ
- `FRAME_HUB_MAX_FRAME_KB` の既定（PSRAM 有効時）を **768** に上げる（3 スロットで 2.25 MiB。PSRAM の空きは約 19.8 MiB）。値は `TODO(hw-verify)`（実際の 1080p で `oversize_drop = 0` か）。
  **既存の `sdkconfig` は、以前の値（256）を保持している**ので、`menuconfig` で 768 に直すか、`sdkconfig` の該当行を消して再生成する（`docs/INTERFACES.md` §2 の規則: HDMI 担当が値の変更を提案してよい）。
- **品質の制御は、HDMI 担当（`csi_video_source`）が実装する**（`JPEG_*` の領域）。仕様:
  - **純 C のコントローラー**（`jpeg_quality_ctrl`、ホストでテストできる）。入力: 直近のフレームの JPEG サイズ、`max_frame_bytes`、設定の品質 `q_cfg`。出力: 次のフレームの品質 `q`。
  - **次のフレームから**下げる（同じフレームを再エンコードしない。1080p のエンコードは約 38 ms で、30 fps のフレーム予算に収まらない）。`size > max_frame_bytes` なら `q -= 10`（下限 30）。`size > 85%` でも `q -= 5`（先回り）。
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

## 4. B-3 (b): チャンク送信（IDF 側）
`httpd_ws_send_data_async()` は、フレーム全体を 1 回の呼び出しで送り切るので、途中で期限を確認できない。次のようにする:
- 作業関数（`httpd_queue_work()` で `httpd` のタスクに載せる）の中で、**WS のヘッダー（`0x82` + 長さ）を自前で組み立て**、`httpd_socket_send()` で、ヘッダー、ペイロードを **16 KiB 以下のチャンク**に分けて送る。チャンクの間に、期限（`esp_timer_get_time()`）を確認する。部分書き込み（戻り値が要求より小さい）に対応する。`SO_SNDTIMEO`（既定 250 ms）は、1 回の呼び出しの上限として残す。
- 期限を超えた、または送信エラーのとき、`ws_video_core_on_send_done(…, false)` を呼び、`io_request_close` で閉じる。
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
- 実機（device-check。Antigravity）: `slowclient.py` の `40 KB/s` で、**通常のクライアントが 10 fps 以上を維持し、切断されない**、`/status` が 1 秒を超えない。`10 KB/s`・`2 KB/s`・`no read` で、遅いクライアントだけが、期限で閉じられる。1080p の高密度画面で `oversize_drop = 0`（品質の引き下げが働く）。Chrome と Firefox で、証明書の例外を受け入れる手順（上の「信頼」のページ）が働く。メモリ（ヒープの空き、2 つ目のサーバーのスタック）。

## 7. 担当と順序
1. この契約（Claude。`FRAME_HUB_MAX_FRAME_KB` の既定の引き上げを含む）。
2. `ws_video_core` の変更と `http_guard_origin_ok_for_port`（純 C、ホストでテストできる。Claude のサブエージェント）。
3. `web_server` の結線: チャンク送信、映像専用の `httpd`、`app.js`、`auth/status` の `video_port`（Claude のサブエージェント。IDF のビルドは、device-check で確認する）。
4. `jpeg_quality_ctrl` と結線（Antigravity）。
5. device-check（Antigravity）。
