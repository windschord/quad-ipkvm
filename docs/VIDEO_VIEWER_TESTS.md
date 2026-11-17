# ビューア(映像の接続先、証明書の案内)と状態 API `video_port` のテストの結果

作成: 2026-11-14(Issue #82 ③ B-3 のうち、ブラウザ側と状態 API の、ホストで検証できる部分)。仕様は `docs/VIDEO_TRANSPORT.md` 5 章の「ビューア」と「証明書の信頼」。IDF 側(`web_server.c` の 2 つ目の `httpd`、分割送信、Kconfig、`EMBED_FILES`)は別の担当で、ここには含まない。**IDF のビルドは確認していない**(`web_server.c` の変更は、`http_guard_format_status` の呼び出しに引数 `0` を足した 1 行だけ)。

## 1. 変更の要点

- `http_guard_format_status(state, authenticated, video_port, out, out_len)`: `video_port` は `uint16_t`。`0` は「映像専用のサーバーなし(メインのポートの `/video`)」で、`video_port` のキーを出さない(従来の応答と同じ)。それ以外は `,"video_port":8444` を末尾に出す。
- `app.js`: 純関数 `VideoTarget`(`resolve` / `needsTrustHint` / `trustUrl`)。
  - `video_port` が JSON の**整数で 1〜65535**のときだけ使い、`wss://` + `location.hostname` + `:` + ポート + `/video` に接続する。文字列、小数、`null`、真偽値、配列、範囲外、巨大な値は無視して、従来の `wss://<location.host>/video`。
  - `location.hostname` も検査する(英数字・`.`・`_`・`-`、または `[` 16 進数と `:` `.` `]` の IPv6 リテラル。255 文字まで)。合わなければ従来の接続先。`location.host` からホストを切り出したり、`video_port` を文字列で連結したりしない。
  - `video_port` がページ自身のポート(`location.port`。空は 443)と同じなら、別のサーバーではないので従来の接続先(案内も出さない)。
  - 案内: 映像の WebSocket が**開く前に**失敗(`onerror` または開く前の `onclose`)し、かつ別のポートのときだけ、`https://<hostname>:<video_port>/` へのリンクと文を表示する。接続が開いたら消す(次の接続を始めるときは消さない = 点滅しない)。ログイン画面へ戻る経路(`disconnectWebSocket`)でも消す。再接続のバックオフ(1000 ms から 2 倍、上限 10000 ms)は変えていない。
- `video_trust.html`: 映像サーバーの `GET /` 用の静的ページ。**スクリプト、スタイル、リンク、画像、フォントを一切持たず**、動的な値も含まない。

## 2. 契約との食い違い・判断した点

1. **信頼ページに「元のページへのリンク」を置いていない。** 指示は「`location.hostname` から `https://<hostname>/` を組み立てる」が、(a) 組み立てるには JavaScript が要る、(b) CSP(`web_assets_csp`)は `script-src 'self'` で、インラインのスクリプトは許されず(既存のテスト `check_forbidden_patterns` も禁止)、(c) 映像サーバーは `GET /` と `/video` だけを持つので、別ファイル(`video_trust.js`)を配るには URI ハンドラーがもう 1 つ要る。このため、リンクではなく、文で「元のタブに戻って再読み込み」「元のタブがなければ元のアドレスを手で開く」と案内した。ビューアが信頼ページを `target="_blank"` で開くので、元のタブは残る。リンクが要るなら、`video_trust.js` を足して映像サーバーに `GET /video_trust.js` を持たせる必要がある(IDF 側の判断)。
2. **「`web_assets.c` に登録」は、何も変えていない。** 既存のアセットの表(`s_web_assets[]`)と `_binary_*` の extern は `web_server.c` にあり、`web_assets.c` には探索と CSP の関数しかない。`web_server.c` は触らない指示なので、表への追加は IDF 側。ホストのテストは、実ファイルを読み、`web_asset_t` の表(`"/"` だけ)に入れて、`web_assets_find` で引けること、MIME タイプ、長さ、CSP を検査する。
3. `http_guard_format_status` の引数の位置は、`out` と `out_len` の組を保つため、`authenticated` の次にした。
4. `web_server.c` の呼び出しは `0` を渡している(コメントに `TODO(video-viewer-b3)`)。これは、映像専用のサーバーがまだ無いので正しい値。
5. `test/host/CMakeLists.txt` に、`node` があれば `check_video_viewer`(ctest)を登録する行を足した(`node` が無ければ登録しない)。既存の `check_msgbuilder.js` と `check_keymap.py` は、これまで同様、ctest には登録していない。

## 3. テストの構成と結果(実際に走らせたもの)

全体: `cmake -S test/host -B <dir> && cmake --build <dir> -j && ctest --test-dir <dir> --output-on-failure` で **53 個すべて成功**(変更前の 51 + `test_format_status` + `check_video_viewer`)。`check_msgbuilder.js --mutations` も、変更後に 16 個とも殺して成功。

| 内容 | 場所 | 件数 |
|---|---|---|
| `http_guard_format_status`(`video_port` あり・なし、1 / 9 / 10 / 443 / 32767 / 32768 / 65534 / 65535、1〜65535 の全値、バッファのちょうど・1 足りない、境界の外に書かないこと、NULL と長さ 0) | `test/host/http_guard/test_format_status.c`(Unity) | 9 ケース |
| 信頼ページ(共通の禁止規則、サイズ上限 1500 バイト、スクリプト・リンク・フォーム・画像などが無い、動的な値の印が無い、文、`web_asset_t` の表で `"/"` から引ける・MIME・長さ・CSP)、`index.html` の非表示の案内と `app.css` の規則、`app.js` の `/video` の URL が `VideoTarget` の中にだけあること | `test/host/web_assets/test_web_pages.c`(Unity。既存の 8 に 6 を追加して 14 ケース) | 新規 6 ケース |
| 純関数 `resolve`(53 件: `video_port` の有無・範囲・型、巨大な値、`-0`、`NaN`、`Infinity`、`valueOf` を持つオブジェクト、ホスト名の文字・長さ 255/256・IPv6、ページのポート)、`needsTrustHint`(8 件)、`trustUrl`(5 件)、`pagePort`(5 件) | `tools/test/check_video_viewer.js` (A) | 71 件 |
| `app.js` 全体を、偽の DOM / `WebSocket` / `fetch` / タイマーの中で動かし、`index.html` の id と class を実ファイルから読む: 状態の応答 -> 接続の URL -> `onerror` のみ / `onclose` のみ / 開いた後の失敗 -> 案内の表示と消去、再接続の URL、不正な `video_port` 9 種、IPv6、バックオフの列(1000, 2000, 4000, 8000, 10000, 10000、open で 1000 に戻る)、ログイン画面で案内が消える、`video_port` が消えた後の再接続 | `tools/test/check_video_viewer.js` (B) | 18 シナリオ |

実行: `node tools/test/check_video_viewer.js [--mutations]`(依存なし)。

## 4. ミューテーションテスト

**合計 107 個 / 殺した 103 / 同値 4 / 生き残り 0。**

### 4.1 `app.js` の追加分(`node tools/test/check_video_viewer.js --mutations`): 60 個、殺した 57、同値 3、生き残り 0

対象: `validPort`、`validHostname`、`pagePort`、`resolve`、`needsTrustHint`、`trustUrl`、`showTrustHint`、`hideTrustHint`、`connectWebSocket` の追加分(`opened`、`onopen` / `onclose` / `onerror` の呼び出し)、`fetchStatus` の `video_port` の読み取り、`disconnectWebSocket` の消去、バックオフ(既存の挙動が変わらないこと)。各ミュータントは `app.js` の 1 か所の文字列置換(置換前の文字列がちょうど 1 回現れることを確認する。合わなければ `NOMATCH` で失敗)。

同値(殺せないのが正しい)の 3 個:
- **J07** `validPort` の `typeof v === 'number'` を除く: `Number.isInteger` が数値以外を常に `false` にするので、結果が変わらない(二重の防御)。
- **J10** `validHostname` の `h.length === 0 ||` を除く: 2 つの正規表現が、どちらも 1 文字以上(`+`)を要求するので、空文字は拒否される(二重の防御)。
- **J22** `pagePort` の桁数の上限 5 -> 6: `location.port` は最大 5 桁なので、実際の値では結果が変わらない。

### 4.2 C 側とアセット(`python3 tools/test/mutate_video_viewer.py <repo> <作業用> <結果.json>`): 47 個、殺した 46、同値 1、生き残り 0

方法: `components/` `test/` `tools/` を作業用にコピーし、1 か所を置換 -> 再ビルド -> テストを実行、を繰り返す。`http_guard.c` のミュータント(17 個)には `test/host/http_guard/` の全テスト、HTML / CSS のミュータント(30 個)には `test_web_pages` と `check_video_viewer.js` を実行する。1 つでも失敗(アサーション、ASan / UBSan、終了コード非 0、タイムアウト)すれば「殺した」。ベースライン(ミュータントなし)は 2 グループとも成功。

- `http_guard_format_status`(H01〜H17): 0 の判定(`== 1`、反転、分岐の除去)、キー名、8 ビット・符号つき 16 ビットの切り詰め、閉じ括弧・カンマの欠落、バッファの検査(1 つ多い・少ない、除去、成功を返す)、NULL の検査、状態・認証の文字列の入れ替え、0 のときの形(末尾のカンマ)。
- `video_trust.html`(T01〜T20): 外部・インラインのスクリプト、リンク、スタイルシート、`%s`、`cookie` / `session` の語、画像、`https://` の URL、`//`、フォーム、`<style>`、`on*` 属性、文の削除、サイズ超過、`</html>`・charset・DOCTYPE の欠落。
- `index.html`(I01〜I08)と `app.css`(C01、C02): 案内が最初から表示される、id の改名、リンクの `target` / `rel` / 静的 `href`、文の変更、非表示の規則の `display` の変更・削除。

同値の 1 個:
- **H13** `format_status` の `out_len == 0` の検査を除く: `out_len == 0` のとき `snprintf` は何も書かず、戻り値(必要な長さ、1 以上)が `out_len` 以上になるので、後ろの検査が `-1` を返す(二重の防御)。

注意: スクラッチへのコピーは更新時刻を保つため、`build` を残したまま再実行すると古い成果物が新しいと見なされる(1 度、ベースラインが偽に失敗した)。道具は、起動のたびに `build` を消す。

## 5. 実行できなかった確認

- **IDF のビルド**(`web_server.c` の 1 行の変更を含む)と、実機: 確認していない。
- **実際のブラウザ**(Chrome / Firefox / Safari)での、`wss://host:8444/video` の接続、証明書の例外の受け入れ、案内の見た目: 確認していない。`app.js` は、偽の DOM で、`onerror` / `onclose` / `onopen` を手で呼んで動かしただけ。実際のブラウザが、証明書の拒否で `onerror` と `onclose` を出す順序・回数は未確認(どちらでも案内を出すので、順序には依存しない)。
- 信頼ページの CSP: `web_assets_csp("text/html")` の文字列を検査しただけで、ブラウザでは確認していない(スクリプトもスタイルも使わないので、`default-src 'none'` でも壊れないはず)。

## 6. 映像の状態の表示(Issue #86 C-6b のビューア側。`docs/WEB_API.md` §5、`docs/VIDEO_TRANSPORT.md` §8)

作成: 2026-11-15。サーバー側(`ws_video_core`、`web_server.c`)は別の担当で、ここには含まない。**実際のブラウザでの確認はしていない**(下の 6.4)。

### 6.1 変更の要点

- `app.js`: 純関数 `VideoStatus`(`parse`、`resolutionLabel`、`statusLine`、`overlayMessage`、`isResolutionChange`、`changeNotice`)と、表示(`setVideoOverlay`、`resetVideoStatus`、`applyVideoStatus`)。`ws.onmessage` は、`typeof e.data === 'string'` のものだけを `VideoStatus.parse` に渡す。バイナリは従来どおり JPEG(`binaryType` も `"blob"` のまま)。テキストの処理は、復号の重複防止(`isDecoding`)の**前**にあるので、フレームの復号中でも状態が反映され、不正なテキストは復号に影響しない。
- 採用するもの: `t === "video"`、`state` が `unknown` / `no_signal` / `locked`、`w`・`h` が 0〜65535 の整数。`locked` は `w`・`h` が 1 以上(0 は捨てる)、`locked` 以外は `w` = `h` = 0 に直す。未知のキーは無視。それ以外(壊れた JSON、`null`、配列、型違い、範囲外、1024 文字を超えるもの)は捨てる。
- 表示: `locked` は重ねる表示なしで、`#video-status` に `接続完了 / 受信中 (1280x720)`。`no_signal` は「信号なし」、`unknown` は「映像の状態を確認中…」を、`#video-overlay`(`role="status"`、`aria-live="polite"`、既定は非表示)に出す。キャンバスは触らない(直前のフレームが残る)。`locked` → 別の解像度の `locked` は、「解像度が変わりました（1920x1080）」を 2000 ms のタイマーで出して消す。次の状態が来たら、すぐ消してタイマーも消す。文は、すべて `textContent` で入れる。
- リセット(`resetVideoStatus`): 1 か所に集約。`onclose`、`disconnectWebSocket`(ログアウトなど)、新しい接続の開始で、重ねる表示を消し、タイマーを消し、最後の状態を忘れる(前の接続の解像度と比べて、変更の表示を出さない)。
- `index.html`: `<div class="video-wrap">` の中、キャンバスの後ろに `#video-overlay`。`app.css`: `.video-wrap`(基準の位置)、`.video-overlay`(中央、半透明の背景、`pointer-events: none` でキャンバスのクリックを妨げない)、`prefers-color-scheme: dark` の規則、`.video-overlay-hidden`。

### 6.2 テスト(実際に走らせたもの)

`cmake -S test/host -B <dir> && cmake --build <dir> -j && ctest --test-dir <dir> --output-on-failure`: **63 個すべて成功**(`test_web_pages` は 14 -> 16 ケース。ctest の登録の数は変わらない)。`node tools/test/check_video_viewer.js` も成功。

| 内容 | 場所 | 件数 |
|---|---|---|
| `VideoStatus.parse`(正常 3 状態、`locked` 以外の `w`・`h` を 0 に直す、`locked` の 0 を捨てる、`t` 違い、`state` 違い、`w`・`h` の小数・文字列・負数・65536・`null`・真偽値・配列・オブジェクト・欠落・巨大な値、未知のキー、`__proto__`、重複キー、配列・`null`・空文字・壊れた JSON、長さ 1024 / 1025 / 1 MB、文字列でない入力) | `tools/test/check_video_viewer.js` (C) | 95 件 |
| `isResolutionChange`(11 件)、`resolutionLabel`(4 件)、`statusLine`(4 件)、`overlayMessage`(6 件)、`changeNotice` | 同上 | 26 件 |
| 偽の DOM / `WebSocket` / タイマーでのシナリオ(接続直後の `locked`、`no_signal` でフレームが残る(キャンバスの呼び出しが `drawImage` だけ、大きさも同じ)、`locked` に戻って消える、`unknown`、解像度の変更の表示と、2000 ms のタイマーで消える、変更の最中に `no_signal` / `unknown` / 同じ `locked` / 別の変更が来て、すぐ消えて古いタイマーが残らない、切断・再接続・ログアウトのボタンで消える、前の接続の解像度を覚えていない、バイナリが状態に影響しない、不正なテキスト 19 種を無視してもバイナリの処理が続く、復号の最中のテキスト、`innerHTML` への代入がない、`binaryType` が `"blob"`、入力用の `/ws` に影響しない、`#video-overlay` の `role`・`aria-live`・既定の非表示) | 同上 (B) | 17 シナリオ |
| `index.html` の `#video-overlay`(`role`、`aria-live`、既定で非表示、`.video-wrap` の中のキャンバスの後ろの最後の要素、空)、`app.css`(`display: none`、基準の位置、`absolute`、`pointer-events: none`、ダークモード)、`app.js` に `innerHTML` / `outerHTML` / `insertAdjacentHTML` がなく、`textContent` で入れること(既存の `test_app_js` の禁止パターンにも、`app.js` は引き続き通る) | `test/host/web_assets/test_web_pages.c`(Unity) | 2 ケース |

### 6.3 ミューテーションテスト

**ビューアの全体で、`app.js` が 129 個(殺した 119、同値 10、生き残り 0)、`index.html` / `app.css` / C 側が 62 個(殺した 61、同値 1、生き残り 0)。** 今回の追加分は、`app.js` が 69 個、`index.html` / `app.css` が 15 個。

- `app.js`(`node tools/test/check_video_viewer.js --mutations`): 既存の 60 個(J01〜J60。J07 と J57 の置換前の文字列は、新しいコードと重なる / 行が増えたので、一意になるように直した。意味は同じ)に、**V01〜V69 の 69 個**を追加。対象は、`validDimension`(型・小数・下限・上限)、`parse`(長さの上限、型、`try/catch`、`null`、`t`、`state` の 3 つ、`w`・`h` の検査、`locked` の 0、`w`・`h` の入れ替え、`locked` 以外を 0 に直す)、`resolutionLabel`、`statusLine`、`overlayMessage`、`isResolutionChange`(各条件)、表示の文、2000 ms、`setVideoOverlay`(隠す・出す・`textContent` を `innerHTML` に)、`resetVideoStatus`(タイマー・最後の状態・表示)、`applyVideoStatus`(最後の状態、タイマーの取り消し、ステータス行、変更の表示、タイマーの開始・実行、全部の `locked` を変更と見なす)、`onmessage`(文字列の判定、`return` の除去、`null` の判定、`isDecoding` の後ろに移す)、`onclose` / `disconnectWebSocket` / 接続開始のリセット、`binaryType`。
- 同値の 7 個(今回の追加分):
  - **V07** `validDimension` の `typeof v === 'number'` を除く: `Number.isInteger` が数値以外を常に `false` にする(二重の防御)。
  - **V11** `parse` の空文字の検査を除く: 空文字は `JSON.parse` が例外を投げ、`catch` が `null` を返す(二重の防御)。
  - **V15** `typeof obj !== 'object'` を除く、**V16** `Array.isArray(obj)` を除く: JSON の数・文字列・真偽値・配列は `t` を持たないので、次の `t` の検査が捨てる(二重の防御)。
  - **V30** `Math.trunc` を除く: `parse` が `w`・`h` を整数に限っているので、結果は変わらない。
  - **V58** タイマーの実行時に `resolutionNoticeTimer = null` を除く: 実行済みの id への `clearTimeout` は何もしない。
  - **V68** 接続の開始時の `resetVideoStatus()` を除く: 接続の前には、必ず `onclose` か `disconnectWebSocket` が先に走って消している(二重の防御)。
- `index.html` / `app.css`(`python3 tools/test/mutate_video_viewer.py <repo> <作業用> <結果.json>`): 既存の 47 個に、**I09〜I16(8 個)、C03〜C09(7 個)の 15 個**を追加し、全体で 62 個を走らせた(殺した 61、同値 1 = 既存の H13、生き残り 0、ビルドエラー 0)。追加分は 15 個とも殺した。内容: 重ねる表示が最初から見える、`role` の除去、`aria-live` の変更、`id` の改名、キャンバスの前に置く、枠の外に出す、枠の名前の変更、最初から文がある、`display: none` を `block` に・規則の除去、基準の位置を外す、`absolute` を `fixed` に、`pointer-events` の除去、ダークモードの規則の除去、規則の名前の変更。
  - 初回の実行では、I16(重ねる表示を `.video-wrap` の外に出す)が生き残った。`test_web_pages.c` のテストが「閉じタグの直後」を調べていなかったため。テストに、「`#video-overlay` の直後が `.video-wrap` の閉じタグ」の検査を足して、殺せることを確認した。

### 6.4 実行できなかった確認・判断した点

- **実際のブラウザ(Chrome / Firefox / Safari)での表示の確認はしていない。** 重ねる表示の見た目(位置、大きさ、ダークモードの色、`pointer-events: none` でキャンバスのクリックが通ること、`aria-live` での読み上げ)は、CSS を読み、文字列を検査しただけ。`display: none` から表示に切り替えたときに、スクリーンリーダーが `aria-live` の領域の変化を読み上げるかも、未確認(確実にするには、非表示にしない(視覚的にだけ隠す)作りにする必要があるかもしれない)。
- 実機(device-check)、IDF のビルド、サーバー側との結合(`/video` に実際にテキストフレームが来る)は、確認していない(サーバー側は別の担当)。テストは、`ws.onmessage` に文字列を直接渡している。
- 実際のブラウザは、テキストフレームを `string`、バイナリを `Blob`(`binaryType = "blob"`)で `e.data` に渡す。テストの偽のフレームは、`Blob` ではなく単なるオブジェクトで、`typeof` が `string` でなければバイナリとして扱う実装の分岐だけを調べている。
- 契約(`docs/WEB_API.md` §5)に書かれていない判断: (a) `locked` 以外の状態のとき、ステータス行は `接続完了 / 受信中`(解像度なし)に戻す。(b) 1024 文字を超えるテキストは捨てる(契約の上限は 128 バイト。将来のキーの追加の余裕を見た)。(c) `w`・`h` が欠けている(契約は常に含む)テキストは、`no_signal` / `unknown` でも捨てる。(d) 同じ解像度の `locked` が、変更の表示の最中にもう一度来たら、表示を消す(契約では同じ内容は繰り返さないので、起きない想定)。


## 7. H.264(WebCodecs)の経路(P7。`docs/VIDEO_TRANSPORT.md` 9.2〜9.4、9.8)

作成: 2026-11-16。サーバー側(`video_codec_policy`、`ws_video_core` の `need_key`、`web_server.c` の `accept` の解析、`csi_h264`)は別の担当で、ここには含まない。**実際のブラウザでの再生は確認していない**(7.5)。

### 7.1 変更の要点(`app.js` のみ。`index.html` / `app.css` / `web_server.c` は変更なし)

- `VideoTarget.withAccept(target, accept)`: `accept === true` のときだけ、URL に `?accept=h264` を足した**新しい**オブジェクトを返す(他のフィールドはそのまま、引数は変えない)。`resolve` は変えていない(既存の URL の組み立ての検査は、そのまま通る)。
- `accept` を付ける条件: `VideoDecoder`・`EncodedVideoChunk`・`VideoDecoder.isConfigSupported` があり、起動時の確認(`isConfigSupported({codec: "avc1.42c01f", optimizeForLatency: true})`)が `supported === true` で、このタブで H.264 が一度も失敗していない。最初の接続は確認を待つ(終わらないときは 1500 ms で `accept` なしで接続)。
- 純関数 `H264`: `classify`(先頭の 2 バイトが `FF D8` なら `jpeg`、先頭の 4 バイトが `00 00 00 01` なら `h264`、それ以外は `null`)、`parseAnnexB`(3 バイト / 4 バイトのスタートコード、開始コードの前のゼロ、末尾のゼロ、`forbidden_zero_bit`、空の NAL)、`analyze` / `isKeyAccessUnit`(SPS(type 7)または IDR スライス(type 5)を含む = キー)、`avcCodecStringFromSps`(`67 42 c0 1f` → `avc1.42c01f`)、`sameBytes`。
- 判別とデコーダの制御: `accept` を付けた接続は `binaryType = "arraybuffer"`。先頭で JPEG / H.264 / 捨てる、に分ける。JPEG は `Blob` に包んで従来の `createImageBitmap` へ(**`accept` を付けない接続は、従来のまま**。`blob` で、先頭バイトも見ない)。H.264 は、最初の IDR まで P を捨て、IDR の SPS から codec 文字列を作って `isConfigSupported` で確認し、`configure({codec, optimizeForLatency: true})`(`description` なし)。SPS が変わったら作り直す。`decodeQueueSize >= 3` で、次の IDR まで P を捨てる。出てきた `VideoFrame` は canvas に描いて、**必ず `close()`**。
- フォールバック: `isConfigSupported` が偽 / 拒否 / 例外、デコーダの作成・`configure`・`EncodedVideoChunk`・`decode` の例外、`error` コールバック。**そのタブで 1 回だけ**: 以後 `accept` を付けず、`ws.close()` で既存の再接続(1000 ms)に任せる。
- 表示: 映像のステータス行に ` / H.264（低帯域）`(例: `接続完了 / 受信中 (1280x720) / H.264（低帯域）`)。最初の `VideoFrame` を描いたときから。JPEG が来たら消える。`VideoStatus`(C-6b)は変えていない。

### 7.2 テスト(`node tools/test/check_video_viewer.js`。実際に走らせたもの)

変更前: 53 resolve / 8 hint / 5 trust-url / 95 status-parse / 11 resolution-change のケースと、既存のブラウザのシナリオ。ミュータント 129 個。
変更後の出力: `video viewer ok (53 resolve cases, 8 hint cases, 5 trust-url cases, 95 status-parse cases, 11 resolution-change cases, 26 classify cases, 25 annex-b cases, 22 key-unit cases, 26 codec-string cases, browser scenarios)`。既存のケースは、すべて、変えずに通る(偽の `WebSocket` の `videoSockets()` が `?accept=h264` を許す、偽の 2D コンテキストが引数を記録する、`boot` が追加のグローバルを受ける、の 3 点だけ道具を広げた)。

| 内容 | 件数 |
|---|---|
| `classify`(境界: `FF D8` だけ 2 バイト、`FF` 1 バイト、`00 00 00 01` だけ 4 バイト、`00 00 01`(3 バイトのスタートコード)、`00 00 00 02`、`01 00 00 01`、空、非配列、長さが文字列のオブジェクト) | 26 + 非配列 7 |
| `parseAnnexB`(3 / 4 バイトのスタートコードの混在、末尾・開始コード前のゼロ、`00 00 03 01` のエミュレーション防止、`forbidden_zero_bit`、空の NAL、末尾の開始コード、type 23 / 21(5 ビット)、1 MB の入力) | 25 + 非配列 6 |
| `analyze` / `isKeyAccessUnit`(SPS+PPS+IDR、IDR だけ、SPS だけ、P、PPS だけ、SEI+P、SPS が 2 つ、type 2〜4、壊れた NAL) | 22 |
| `avcCodecStringFromSps`(`67 42 c0 1f`、`67 42 c0 29`、ゼロの詰め `0f` / `10` / `0a`、3 バイト、PPS / IDR、`forbidden_zero_bit`) | 26 + 非配列 6 |
| `sameBytes`、`withAccept`(`true` だけ。`'true'` / `1` / `null` / `undefined` は付けない。引数を変えない) | 5 + 2、8 |
| 偽の `VideoDecoder` / `EncodedVideoChunk` / `VideoFrame` / `Blob` での、ブラウザ側のシナリオ(7.3) | 42 回(29 の定義。表の繰り返しを含む) |

### 7.3 ブラウザ側のシナリオ(偽の環境)

`accept` の条件(`VideoDecoder` なし、`EncodedVideoChunk` なし、確認が偽・拒否・例外・`supported` が `'yes'` / `1` / 無い / `null` / 結果なし、確認が終わるまで接続しない、終わらないときは 1500 ms で `accept` なし、確認が終われば待ちのタイマーが残らない、`video_port` なしの URL、証明書の案内は `?accept=h264` に影響されない)。JPEG(`FF D8` の 2 バイトだけ、`ArrayBuffer`、`Uint8Array`、オフセットのある view)が、従来どおり描かれ、`image/jpeg` の `Blob` になること。**3 バイトのスタートコード、`00 00 00 02`、1 バイトなどは、捨てられ、落ちず、接続も切れない**こと。最初の IDR まで P を捨てる、`description` なし、`key` / `delta`、時刻が増える、最初の `VideoFrame` を描いて `close()`、表示の文字列(`VideoStatus` の `locked` / `no_signal` / `unknown` / 解像度の変更と並ぶ)。SPS のない IDR、SPS だけのアクセスユニット。解像度の変化(codec 文字列が変わる、同じ codec 文字列でも SPS が変わる、同じ SPS では作り直さない、古いデコーダの出力と `error` は無視)。`isConfigSupported` の待ちの間の重複・取り消し(接続の切断の後の遅い答えを無視)。`decodeQueueSize` が 2 / 3 / 0 / 5 / 100 のとき。`VideoFrame` がどの場合も 1 回 `close()` される(描画の例外、古いデコーダ、接続の切断の後)。フォールバック 10 種(確認が偽 / 拒否 / 例外 / `null` / `supported` なし、コンストラクタ・`configure`・`EncodedVideoChunk`・`decode`(IDR と P)の例外、`error` コールバック)それぞれで: 接続が閉じる、デコーダが全部閉じる、閉じている間の入力は無視、再接続が **1 回**、その URL に `accept` がない、`blob`、その後の再接続にも `accept` がない、古いデコーダの `error` が新しい接続を閉じない。サーバーが MJPEG に下げた(JPEG が来た)とき、ラベルが消え、次の IDR まで P を捨てる。切断・再接続・ログアウトでデコーダが閉じる(`close()` が例外でも)。不正な入力(空、`00 00 00 01` だけ、壊れた SPS、`forbidden_zero_bit`、NAL のない 2 MB)で落ちず、フォールバックもしない。`accept` を付けない接続では、`createImageBitmap` に、受け取ったオブジェクトがそのまま渡る。処理されない Promise の拒否は、失敗として数える(`process.on('unhandledRejection')`)。

### 7.4 ミューテーションテスト

**全体で 337 個 / 殺した 307 / 同値 30 / 生き残り 0 / 一致なし 0**(`node tools/test/check_video_viewer.js --mutations`。`--mutations D114 ...` のように ID を付けると、そのミュータントだけ走らせる)。今回の追加分は **208 個(D01〜D209。D204 は欠番): 殺した 188、同値 20、生き残り 0**。既存の 129 個(J / V)は、置換前の文字列が一意でなくなった 2 個(J54、V50)の文字列を直しただけで、全部、同じ結果(119 個を殺し、10 個が同値)。

- 対象: `withAccept`(6)、`classify`(17)、`parseAnnexB`(19)、`analyze` / `isKeyAccessUnit`(7)、`avcCodecStringFromSps`(15)、`sameBytes`(5)、`toBytes`(4)、`h264Available` と確認(`probeH264`、`waitForProbe`)(17)、`setH264Shown`(8)、`resetH264`(7)、`h264Fallback`(5)、`drawH264Frame`(16)、`decodeH264`(11)、`startH264Decoder`(26)、`feedH264`(23)、`noteMjpeg`(2)、`connectWebSocket` と `onmessage` の追加分(16)、状態のリセットと表示(4)。
- 同値の 20 個(いずれも、二重の防御、または結果に影響しない値。理由は `check_video_viewer.js` の各ミュータントに書いてある): D11(JPEG の長さ >= 1)、D17(H.264 の長さ >= 3)(範囲外の読み出しが `undefined` を返すので、長さの検査は二重の防御)、D28(開始コードの後に 1 つだけ進める)、D32(NAL の終わりを 1 つ後ろにする。直後のゼロの削りが取り除く)、D36(ゼロの削りの `end > s`)、D75(`isConfigSupported` の有無の検査。呼び出しが `try/catch` の中)、D93(`setH264Shown` の同じ状態を飛ばす検査。書き換えを減らすだけ)、D101 と D167(`resetH264` の `h264NeedKey` と、`feedH264` のデコーダの検査。どちらか一方だけなら同値。両方外すとデコーダなしの P で落ちる)、D107・D110(`h264Fallback` の 2 重呼び出しの検査と `ws` の検査。呼び出し側から到達できない)、D113(`h264Dec` が `null` のときの `TypeError` を、すぐ下の `catch` が捨てる)、D132(時刻の増分の値。増えれば何でもよい)、D136(`delta` でも `h264NeedKey` を偽にする。`delta` を送るのは、既に偽のときだけ)、D155(`dec` が `null` のときの `close()` の `TypeError` を、隣の `try/catch` が捨てる)、D159(SPS をコピーせず view のまま持つ)、D178(SPS がないとき、その後の `avcCodecStringFromSps(null)` が `null` で捨てる)、D181(`codec` の比較。同じ SPS からは同じ `codec` ができる)、D197(H.264 のメッセージの後の `return`。次の `kind !== 'jpeg'` が捨てる)、D203(`bytes === null` の検査。`classify(null)` が `null`)。
- **最初の実行で生き残った 3 個を、テストを足して潰した**: D40(`type` のマスクが 4 ビット: type 23 / 21 のケースを足した)、D114(表示サイズより `codedWidth` が広いフレーム: 1366 → 1376 のケースを足した)、D155(同値と判断。上)。D167 も同値と判断した。
- **テストが見つけた不具合**: `configure` が例外を投げると、作った `VideoDecoder` を閉じずに、フォールバックしていた(`h.env.decoders.every(closed)` の検査で発覚)。`configure` の例外のときも `close()` するように直した。
- 道具の注意: 変異の途中で `app.js` の内側の `Promise` が拒否されると、Node のプロセスごと落ちる(最初の実行は D83 で止まった)。`unhandledRejection` を失敗として数えるようにして、全数を走らせられるようにした。

C 側・アセットの道具 `python3 tools/test/mutate_video_viewer.py <repo> <作業用> <結果.json>`(work-dir の重なりの確認、シェルを通さない実行は、そのまま)は、今回 `index.html` / `app.css` / C を変えていないので、ミュータントは足していない。**新しい `app.js` と新しいテストでも、ベースラインが通り、62 個(殺した 61、同値 1 = 既存の H13、生き残り 0、ビルドエラー 0)の結果が変わらないこと**を確認した。

### 7.5 実行できなかった確認(未確認)

- **実際のブラウザ(Chrome / Edge / Safari / Firefox)での再生**: 確認していない。`VideoDecoder` に Annex B のアクセスユニット(`description` なし)を渡して、SPS / PPS の入った IDR から復号できるか、`avc1.42c01f` / `avc1.42c029` が `isConfigSupported` で真になるか、1080p(高さ 1088 でクロップ)の `VideoFrame` の `displayHeight` が 1080 になるか、`optimizeForLatency` の効果と体感の遅延、IDR 495 KB の復号時間、**いずれも未確認**。偽の `VideoDecoder` で、呼び方(引数、順序、`close()`)だけを確認した。
- **Safari / Firefox**: WebCodecs の `VideoDecoder` の対応状況と、Annex B の扱いの差は、確認していない(非対応なら `accept` を付けず MJPEG のまま、という経路だけを、偽の環境で確認した)。
- 長時間の再生、チャンネルの切替からの復帰、低帯域での体感、`decodeQueueSize` の実際の値(`TODO(hw-verify)`: 3 は目安)、確認の待ちの時間 1500 ms(`TODO(hw-verify)`)。
- サーバー側との結合(`/video?accept=h264` を `web_server.c` が受け付けるか。`esp_http_server` が URI のクエリを除いて照合するか、`http_guard` / Origin の検査がクエリで壊れないか)。**未確認(`web_server.c` 側の確認事項)**。
- ビューアは、サーバーへ IDR を要求できない(ビューアからのデータフレームは切断の対象。`docs/WEB_API.md` §5)。`decodeQueueSize` で P を捨てた後は、次の IDR(`CSI_H264_GOP`、既定 fps × 2 = 約 2 秒)まで映像が止まる。この間隔が実用上許せるかは、実機で確認する。
- `new Blob([bytes], {type: "image/jpeg"})` を `createImageBitmap` に渡す経路(`accept` を付けた接続の JPEG)の描画の確認(偽の `createImageBitmap` で、呼び出しだけ確認した)。
