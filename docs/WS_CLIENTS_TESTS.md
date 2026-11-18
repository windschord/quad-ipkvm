# ws_clients の世代番号のテストとミューテーションテストの結果

作成: 2026-11-18(`/video` と `/ws` の「古い close 要求が、同じ fd の新しい接続を閉じる」問題への対策。経緯と、`web_server.c` 側は `docs/VIDEO_TRANSPORT.md` 5.1)。`components/ws_clients`(WebSocket クライアントの表。純 C、IDF 非依存)に足した、**接続ごとの世代番号**のホスト単体テストの構成と、足した部分のミューテーションテストの結果。

## 1. 規則と、判断して固定した点

| 点 | 決めたこと | 理由 |
|---|---|---|
| 割り当て | `ws_clients_add` が成功したときだけ、`last_gen + 1` を、その接続の `gen` にする。`ws_clients_init` で `last_gen = 0` | 失敗した add(重複 fd -2、満杯 -1)は、番号を使わない |
| 0 | 「接続なし」を意味し、割り当てない。`UINT32_MAX` の次は 1(0 を飛ばす) | `ws_clients_slot_gen` が、FREE / 無効なスロットに 0 を返すため、0 が本物の世代だと区別できなくなる |
| 戻さない | スロットを解放しても、カウンタは下げない | 同じ fd(同じスロット)の再接続が、古い接続と同じ世代にならないため。これが、この機能の要点 |
| `ws_clients_slot_gen` | 範囲外・FREE は 0。CLOSING は、その接続の世代を返す | 表がまだその接続を持っている(`ws_clients_find` が CLOSING を見つけるのと同じ意味) |
| `ws_clients_find_gen` | fd が一致し、状態が FREE でなく、世代が一致するときだけ、スロットを返す。それ以外は -1。**CLOSING は、世代が同じなら見つかる** | `ws_clients_find` と同じ意味に揃える(表の中の判定と、呼ぶ側の方針を分ける)。CLOSING の接続を閉じるかどうかは、呼ぶ側が決める |
| 呼ぶ側の方針(`web_server.c`) | **ACTIVE のときだけ**、`httpd_sess_trigger_close` を呼ぶ。CLOSING は呼ばない | `ws_clients_remove` は `close_fn`(`httpd` がそのセッションを既に削除した後)からしか呼ばれないので、CLOSING の接続には、閉じるセッションがもう無い。呼ぶと、同じ fd で accept 済みで、まだ登録されていない新しい接続を閉じかねない |
| fd の一意性 | 表の中で、FREE でないスロットの fd は一意(`ws_clients_add` が重複を -2 で拒否する)。CLOSING の間も、同じ fd は追加できない | `ws_clients_find_gen` は、最初に fd が一致したスロットで結論を出してよい(ミュータント G26 が同値な理由) |

## 2. テストの構成

`test/host/ws_clients/test_ws_clients.c`。既存の 6 件(init、add / remove、send / closing、session、iter、fuzz)は**変更していない**。足したのは 7 件(合計 13 件):

| テスト | 内容 |
|---|---|
| `test_ws_clients_gen_basic` | 無効 / FREE / NULL は 0・-1。接続ごとに 0 でなく、互いに異なる。fd と世代の両方が合うときだけ見つかる(別の接続の世代、別の fd、世代 0 は -1)。失敗した add はカウンタを進めない |
| `test_ws_clients_gen_full_table_bounds` | 満杯の表(`max_clients == WS_CLIENTS_MAX`)で、範囲の境界(`slot == max_clients`、`slot == -1`、最後のスロットが正しく見つかる)。(1 つ先を読む実装は、構造体の次のフィールドを読むだけで、ここでは値に現れない。それを落とすのは、次の `outside_the_table`) |
| `test_ws_clients_gen_outside_the_table` | 表の外(`clients[-1]`、`clients[max_clients]`)に「使用中に見えるスロット」を置いて、見ないこと(`slot_gen`、`find_gen`、既存の `find` と同じ境界) |
| `test_ws_clients_gen_reconnect_same_fd` | 同じ fd の再接続(remove → add)で世代が変わり、古い世代では見つからない。同じスロットを 50 回再利用して、古い世代がすべて古いまま。世代は単調に増える |
| `test_ws_clients_gen_free_and_closing` | FREE になったスロットの世代は、どの fd(-1 を含む)でも見つからない。**CLOSING は、世代が同じなら見つかる**(別の世代・別の fd は -1)。CLOSING の間は同じ fd を追加できない(-2)ので世代が進まない。送信が終わって FREE になったら、見つからない。その fd の新しい接続は別の世代 |
| `test_ws_clients_gen_wraparound` | 最初は 1。解放してもカウンタは戻らない。`UINT32_MAX` の次は 0 でなく 1。ラップアラウンドの前の世代(`UINT32_MAX`)は、後の接続に合わない。0 は合わない |
| `test_ws_clients_gen_fuzz` | 固定シード 16 本 × 600 手のランダムな add / remove / begin_send / end_send(fd は 6 種類だけで、よく再利用される。`max_clients` は 1〜8)。**毎手、それまでに作った全接続について、「(fd, 世代)で見つかる」と「その接続が今も表にある」が同値**であること(古い世代は、同じ fd・同じスロットの新しい接続に一致しない)。世代が単調に増えて、0 でないこと |

ctest の全件: 作業前 72 → 作業後 72(新しい実行ファイルはない。`test_ws_clients` の中のケースが 6 → 13)。既存の `ws_video_core`・`ws_input_core` のテストは、変更なしで通る(`ws_clients_t` / `ws_client_t` に、フィールドを足しただけ)。

**この範囲で確かめていないこと**
- `web_server.c` の世代を使う部分(`ws_queue_close`、`*_close_work_cb`)は、この範囲外。IDF のビルドも実機もない環境のため、関数を切り出したハーネスで確認した(`docs/VIDEO_TRANSPORT.md` 5.1。ハーネスはコミットしていない)。
- 世代が 2^32 回を超えて一周し、古い要求が同じ世代に当たること(32 ビットのカウンタの一周)は、扱っていない。接続のたびに 1 つ進む。実用上、1 つの close 要求が生きている間に 40 億回の接続は起きない。

## 3. ミューテーションテスト

方法: `tools/test/mutate_ws_clients.py` が、`components/` と `test/` をスクラッチにコピーし(作業先が repo と重なる場合は、何も消さずに終了する。外部コマンドはシェルを通さずに実行する)、`ws_clients.c` の 1 か所を置換 -> 再ビルド -> `test_ws_clients` を実行、を繰り返す(ミュータントの定義は `tools/test/ws_clients_mutants.py`)。失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。対象は、**世代番号の追加分**(`ws_clients_add` の割り当て、`ws_clients_slot_gen`、`ws_clients_find_gen`、解放でカウンタを戻す変更)。既存の関数の既存部分には、ミュータントを入れていない(既存のテストの範囲)。同値と判断したものは、定義に理由を持たせてあり、実行の最後に「理由なしの生き残り」を表示する。

```
python3 tools/test/mutate_ws_clients.py <repo> <スクラッチ> tools/test/ws_clients_mutants.py <結果.json>
```

**結果**(最終の 1 回。ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(同値) | 理由なしの生き残り | ビルドエラー |
|---|---|---|---|---|
| 26 | 25 | 1 | 0 | 0 |

生き残り 1 つ: **G26**(`find_gen`: 世代が合わないとき、-1 を返さず、探索を続ける)。**同値**: FREE でないスロットの fd は表の中で一意(`ws_clients_add` が重複を -2 で拒否する)ので、続きを探しても、同じ fd のスロットは他にない。どちらも -1 を返す。

途中で、最初の実行では 3 つが生き残った(G13: `slot_gen` が負の slot を見ない、G14: `slot_gen` が `slot == max_clients` を受け入れる、G24: `find_gen` が 1 つ先まで読む)。表の**外**のメモリは、ASan にも値にも現れないことが原因で、テストの不足だった。`test_ws_clients_gen_outside_the_table` と `..._full_table_bounds` を足して、殺した。

| ID | 変更 | 殺したテスト(代表) |
|---|---|---|
| G01 | add: 世代をスロットに入れない | `gen_basic`、`gen_full_table_bounds` |
| G02 | add: カウンタを保存しない | `gen_basic` |
| G03 | add: ラップアラウンドで 0 を飛ばさない | `gen_wraparound` |
| G04 | add: ラップアラウンドで 1 でなく 2 | `gen_wraparound` |
| G05 | add: 世代がスロットの番号 + 1(接続ごとに違わない) | `gen_reconnect_same_fd`、`gen_free_and_closing` |
| G06 | add: カウンタを進めない | `gen_basic` |
| G07 | add: カウンタが 2 ずつ進む | `gen_basic` |
| G08 | add: 重複 fd(-2)でも番号を使う | `gen_basic`、`gen_free_and_closing` |
| G09 | add: 満杯(-1)でも番号を使う | `gen_basic` |
| G10 | 解放でカウンタを戻す | `gen_reconnect_same_fd`、`gen_wraparound` |
| G11 | 再利用したスロットが、古い世代を持ち続ける | `gen_reconnect_same_fd`、`gen_free_and_closing` |
| G12 | slot_gen: NULL を見ない | クラッシュ |
| G13 | slot_gen: 負の slot を見ない | `gen_outside_the_table` |
| G14 | slot_gen: `slot == max_clients` を受け入れる | `gen_outside_the_table` |
| G15 | slot_gen: FREE が古い世代を返す | `gen_free_and_closing` |
| G16 | slot_gen: CLOSING が 0 | `gen_free_and_closing`、`gen_fuzz` |
| G17 | slot_gen: 世代 + 1 | `gen_basic` |
| G18 | find_gen: NULL を見ない | クラッシュ |
| G19 | find_gen: 世代を比べない | `gen_basic`、`gen_reconnect_same_fd` |
| G20 | find_gen: 世代の比較が逆 | `gen_basic` |
| G21 | find_gen: fd を比べない | `gen_basic` |
| G22 | find_gen: FREE を見る | `gen_free_and_closing` |
| G23 | find_gen: CLOSING を見つけない | `gen_free_and_closing`、`gen_fuzz` |
| G24 | find_gen: 1 つ先まで読む(`<` が `<=`) | `gen_outside_the_table` |
| G25 | find_gen: 番号 + 1 を返す | `gen_basic` |
| G26 | find_gen: 世代違いで探索を続ける | **同値(生き残り)** |
