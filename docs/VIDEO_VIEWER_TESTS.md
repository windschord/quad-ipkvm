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
