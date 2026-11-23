# iso_api のテストとミューテーションテストの結果

作成: 2026-11-20(Issue #141、C-12a)。`components/iso_api`(`/api/v1/iso*` の要求の検査、**判定の順序(13 段のうち 3〜13)**、状態ごとのエラーの対応表、`GET /api/v1/iso` の応答。純 C、IDF 非依存。**ロックは、`iso_api_env_t` のコールバックで取る**。スレッドを作らない)のホスト単体テストの構成と、`iso_api.c` / `iso_api_json.c` / `iso_api_fmt.c` に対するミューテーションテストの結果。
契約は `docs/WEB_API.md` §8、`docs/ISO_MOUNT_DESIGN.md` §7・§9(実装で決めたことは §12)、`docs/INTERFACES.md` §7.4。部品の下は `docs/ISO_UPLOAD_TESTS.md`、`docs/ISO_CORE_TESTS.md`(`iso_core` / `iso_upload` / `iso_catalog` / `iso_media` の実物を、モックの fs と一緒に使う)。

## 1. 構成と、公開する関数

| ファイル | 内容 |
|---|---|
| `components/iso_api/include/iso_api.h` | 公開する型と関数(下)。先頭のコメントが、グルー(`web_iso.c`、A-13 / A-14 / A-15)への契約 |
| `iso_api.c` | 経路(`iso_api_route`)、Content-Type の分類、段 3〜13、処理の結果の対応表、`PUT` の `precheck` / `commit` |
| `iso_api_json.c` | JSON のボディの厳密な解析(`\u` エスケープ、サロゲートペア、名前の規則、`size`、`sha256`、`id`、`force`、`cursor`) |
| `iso_api_fmt.c` | 書き出しの部品(固定バッファ。溢れたら失敗)と、`GET /api/v1/iso` の本文 |

- `iso_api_handle(env, rq, rsp)`: `PUT` 以外の全部。`env` が NULL(または必須のメンバーが NULL)なら「サービスが起動していない」: `GET` は `sd.state = no_card` の 200、ほかは、形式の検査(段 3〜6)の後で 503 `not_ready`(`eject` は 200 `not_mounted`)。
- `iso_api_headers_check(rq, rsp)`: 段 3〜5(本体を読む前)。`iso_api_put_precheck(env, rq, rsp)`: `PUT` の段 3〜12(**507 を含む**)。拒否なら `rsp->close_connection`(グルーは `Connection: close`)。`iso_api_put_commit(env, put, data, len, rsp)`: 本体の受信の後。
- `iso_api_snapshot_update(snap, view, up)`: 公開ビューのコピー(グルーが `hooks.view_update` の中で呼ぶ)。段 7〜12 と `GET` は、このコピーと `iso_media` の状態だけから決める(I/O なし)。
- ロック: 判定は `state_lock`(`iso_state_mutex`)の下、処理は `op_trylock`(`iso_op_mutex`。取れなければ 503 `busy` + `Retry-After: 1`)の下。`state_lock` を持ったまま `op_trylock` を呼ばない(テストの `t_op_try` が違反を記録して、`tearDown` で検査する)。

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_api/test_api_route.c` | 経路の表(定義していないメソッドは 405 + `Allow`、`<id>` の形式不正、`finish`、定義していない経路は 404、`?` 以降は見ない)、`iso_api_allow_for_path`、Content-Type の分類(大文字小文字、パラメーター、前後の空白、`application/jsonx`)、厳密な u64、id の変換 | 7 |
| `test/host/iso_api/test_api_json.c` | JSON のボディ: begin / mount / eject / rescan / finish の正しい形と、不正(重複、定義していないキー、型、範囲、末尾の余計な文字、NUL、512 バイト)、**`\u` エスケープ(BMP、サロゲートペア、上位だけ / 下位だけ、対にならない、部分的な 16 進)**、名前の規則(C0 / DEL / C1、`/`、`\`、前後の空白、63 バイト、UTF-8 の不正)、`sha256`(大文字小文字、桁数、各桁の 16 進の境界)、**本体と同じ大きさのヒープに置いた、すべての先頭部分(読み出しの超過を ASan が検出)** | 25 |
| `test/host/iso_api/test_api_list.c` | `GET` の本文を、仕様の例を含めて、手書きの文字列と比較(sd の各状態、イメージの各状態と `invalid_reason`、`meta_corrupt`、名前のエスケープ、`mounted` / `target` / `upload` / `hidden_images` / `chunk_size`)、**16 個の最大の本文が `ISO_API_RESPONSE_MAX` に収まること、ちょうどの大きさ**、小さいバッファでの失敗(すべての大きさで、ヒープの境界を超えない)、公開ビューのコピー(table の NULL 化、count の頭打ち)、サービスが起動していないとき | 18 |
| `test/host/iso_api/test_api_order.c` | **判定の順序の各段**: 段 3 → 4 → 5 → 6 → 7 → 8 → 9(begin、mount、finish、PUT、delete、rescan の各 409 の並び)→ 10 → 11 → 12(507 とその境界)→ 13(`busy`。判定で拒否するときは `iso_op_mutex` を取りに行かないこと)。段ごとに、「前の段のエラーが先に出る」組み合わせ | 42 |
| `test/host/iso_api/test_api_ops.c` | **処理と結果の対応表**: begin(受理、SHA-256 の宣言値、UTF-8 の名前、I/O エラー、`<id>` の枯渇)、PUT(チャンク 2 つ、再開、`commit` の各結果、書き込みの失敗)、finish(宣言値あり / 不一致 / 署名なし、`incomplete` の再開、`ready` で表なし、`meta_corrupt`、ワーカーのキューのあふれ)、mount / eject(ターゲットの取り外し、防止、差し替え)、delete(アップロードの中止、`crosslinked`)、rescan(`cursor` で続ける)。古いビュー(`use_stale`)と `OL_HOOK` で、判定と処理の間の競合を再現 | 45 |
| `test/host/iso_api/test_api_edges.c` | 応答の初期化(汚れた応答を渡しても、前の内容が残らない)、書き出しの部品の境界、引数の不正、応答の置き場が小さいとき、**公開ビューが満杯(16 個)のときの探索、空き容量の推定**、処理が判定の後の状態(SD が消えた、すでにマウントされた、表がない)を見ること(`SL_HOOK` で `iso_state_mutex` の中の状態も変える)。**PR #158 のレビューの分**: 500 の短い本体の最小の `cap`(27)、SD が使えない間の `eject` が `ejected_by_target` を戻す(`eject` だけ)、`rsp->put` をそのまま `put_commit` に渡す、**受理の後にカードが替わったら書かない**(`epoch`)、ヘッダーの取得結果の分類(切り詰め) | 24 |
| `test/host/iso_api/test_api_guard.c` | 段 1・2(`http_guard_decide`。Host → 初期設定 → 認証 → Origin の順。GET は Origin 不要)の前提と、Host / 認証 / Origin の全組み合わせの表の固定 | 3 |
| `test/host/iso_api/test_api_fuzz.c` | **固定シードのファズ**(下記) | 3 |
| `test/host/iso_api/api_common.h` | 共通の部品(`iso_core_t` の設定、ロックの数え上げ、公開ビューのコピー、要求の組み立て) | - |

合計 167 件(`test/host/CMakeLists.txt` に、`iso_api` のテストのリンクの規則を足した。ctest の実行ファイルが 8 個増える。PR #158 のレビューの nitpick の 3 件も、`test_api_order.c`(PUT の事前判定はロックなしで受理される)、`test_api_ops.c`(`rescan` の 2 件目の取り込みの失敗と再試行、`FINISH` との競合に、実際に受理された PUT を使う)に足した)。期待値は手書き。

### ファズ(`test_api_fuzz.c`。固定シード、ASan / UBSan)

1. **壊した本体**: 正しい本体のバイト列を、ランダムに(反転、削除、挿入、切り詰め、記号への置き換え)壊して 60000 回解析する。受理したものは、仕様の制約(名前の規則、`size` の範囲、`cursor`、`id` の形式)を満たす。
2. **ランダムな名前の往復**: ランダムな符号位置(ASCII、2 / 3 / 4 バイトの領域)の名前を、ランダムに `\u` エスケープ(4 バイトはサロゲートペア、大文字小文字)して解析し、復号したバイト列が一致する(4000 回)。
3. **要求列**: 40 シード × 160 操作。ランダムな要求(正しいもの、メソッド・Content-Type・本文・Content-Length・offset を壊したもの)に、ワーカーのステップ、時刻の飛び、メディアの事象(ターゲットの取り外し、防止、バスリセット、強制取り外し)、SD の状態、`op_trylock` の失敗、書き込みの失敗の注入を混ぜる。要求のたびに:
   - ステータスが表の集合の中にある(**500 は出ない**)。本文が JSON の形(`{` で始まり `}` で終わる、引用符が釣り合う、生の制御文字がない)で、`body_len` が長さと一致し、400 以上は `{"error":"` で始まる。`405` に `Allow`、`Retry-After` は 503 だけ。
   - ロックの順序と釣り合い(`state_lock` の入れ子なし、`state_lock` を持ったままの `op_trylock` なし、要求の後でどちらも放している)。
   - **拒否(400 / 404 / 405 / 409 / 411 / 413 / 415 / 422 / 503 busy)は、カタログ、メディア、ワーカーのキュー、アップロードの枠を変えない**。`PUT` の `precheck` は、受理でも拒否でも変えない。
   - `GET` の一覧が、カタログと一致する(イメージの数、マウント中の有無)。
   最後に、ワーカーを流し切り、資源(ファイル、メモリ)の漏れを調べる。

## 3. ミューテーションテスト

方法: `tools/test/mutate_iso_api.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_api.c` / `iso_api_json.c` / `iso_api_fmt.c` の 1 か所を置換 → 再ビルド → 7 個のテストの実行ファイルを順に実行、を繰り返す(`tools/test/mutate_iso_extent.py` の共通部分。`-jN` 対応)。ミュータントの定義(`tools/test/iso_api_mutants.py`)は、**`tools/test/gen_iso_mutants.py api` がソースから機械的に作る**(`upload` / `core` と同じ演算子: 関係演算子の置換、`&&` / `||`、`if` 条件の否定、算術、文の削除、定数の ±1、真偽値の反転)。ソースを変えたら、作り直す(`python3 tools/test/gen_iso_mutants.py api . > tools/test/iso_api_mutants.py`)。
1 つでも失敗(アサーション、ASan / UBSan(`-fno-sanitize-recover=undefined`)、LeakSanitizer、クラッシュ、タイムアウト)すれば「殺した」。**`build_error` は殺したものに数えない**。

```
UNITY_SRC=<build>/_deps/unity-src python3 tools/test/mutate_iso_api.py <repo> <スクラッチ> tools/test/iso_api_mutants.py <結果.json> -j4
```

経緯: 最初の実行(1687 個)は、殺した 1503、生き残り 165、ビルドエラー 19 だった。生き残りを 1 つずつ調べ、**同値でないものには、テストを足した**(`test_api_edges.c`: 汚れた応答、書き出しの境界、満杯のビュー、`SL_HOOK` / `OL_HOOK` での競合、`iso_api_headers_check` の直接の呼び出しなど。`test_api_json.c`: 定義していないキーを、どの値でも拒否すること、部分的な `\u`、サロゲートの対の条件、先頭部分のヒープの検査)。
**実装も単純にした**(同値のミュータントを減らすため): ID の一時バッファ(`route_image`)、`memset` の後の再代入、Content-Type の早期の戻り、`view_enter` の `memset`(呼び出し側が `{0}` で初期化)、「サービスが起動していない」の 6 か所の同じ分岐(`proceed()` に 1 つ)。`name_exists` / `avail_bytes` は、**配列の添字で直接読む**形にした(アドレスを取る形は、UBSan の bounds が 1 つ先を許すので、満杯のときの範囲外の読み出しを検出できない)。
2 回目の全数(1692 個)は、殺した 1583、生き残り 88、ビルドエラー 21。同値でない 4 個(`<id>` の先頭の文字と `'9'` の境界、表がない場合の `epoch` の比較)を、テストを足して殺した。

**PR #158 の CodeRabbit のレビューの後**(`iso_api.c` / `iso_api.h` を変えた: `eject` の印の解除、`put_commit` の `put` のコピー、受理時のカードの世代、ヘッダーの取得結果の分類、`ISO_OP_LOCK_WAIT_MS`)、ミュータントを作り直し(1719 個)、**最初から全数**を実行した(`-j3`): 殺した 1611、生き残り 87、ビルドエラー 21。生き残りを新しい ID で調べ直し(ID は、ソースを変えるたびに振り直される)、同値でないもの 1 個(`A586`: `eject` でない操作でも `ejected_by_target` を戻してしまう `&&` → `||`)を、テスト(`test_only_eject_clears_ejected_by_target_when_the_sd_goes_away`)を足して殺した。**足した後に、もう一度、全数を実行して、結果を確定した**(下の表。ソースは同じなので、ID は同じ)。
`tools/test/mutate_iso_extent.py` は、ミュータントの定義を、`import` の仕組みを通さずソースから読む(既存の `__pycache__` の .pyc を使わない、.pyc を作らない)。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数 | ビルドエラー(コンパイラが検出) |
|---|---|---|---|
| 1719 | 1612 | 86 | 21 |


### 3.1 生き残ったミュータント(86 個)

1 つずつ、変更の前後の挙動が同じ(または、サニタイザーで検出できない、到達しない防御)理由を確かめた。同じ理由のものは、まとめた。

| ID | 理由 |
|---|---|
| A004, A193, A670, A816, A817, A847, A928, A929, A1020, A1071, J132, J164, J321, J348, J355, J393 | 初期値(`= 0`)の変更: 直後の呼び出しが必ず上書きする、または成功のときだけ読む(`uint32_t v`(8 桁 = 32 ビットで、初期値は溢れて消える)、`size_t n`、`klen`、`cp`、`lo`、`received`、`offset`、`id_u32`、`uint64_t v`)。`memset(&rr / &out, 0, ...)` は、呼ばれた関数が全部の欄を書く(`iso_catalog_rescan` / `iso_upload_write_chunk`) |
| A276, A622, A867, A868, M152 | 配列の大きさ・コピーの長さが、実際の使い方と同じ: `char id[ISO_ID_LEN + 2]`(余分な 1 バイトを使わない)、`set_log_id` / `put.id` の `memcpy` の `ISO_ID_LEN + 1` → `+ 0`(コピー先は `memset` 済みで、9 バイト目は 0)、`+ 2`(コピー元の構造体の中の、パディングの 1 バイトを読むだけで、サニタイザーは検出しない) |
| A141, A278, A322, A327, A609, A611 | 境界で、両方の分岐が同じ値になる: `received <= size_bytes`(等しければ足すのは 0)、`free_bytes >= used`(等しければ 0 が 0)、`count >= ISO_MAX_IMAGES`(等しければ代入しても同じ)、`lower()` の `'Z'`(比較する文字列に `z` はない)、`rc <= 0` / `rc < 1`(`fail_rc` は 0 では呼ばれない) |
| A334, A363 | `name_exists` の `meta_valid` と `NONE` の条件の `||` → `&&`: `meta_valid` が偽(`meta_corrupt`)のイメージの名前の長さは 0 で、1 バイト以上の名前とは一致しない。`NONE` はカタログに入らない。`log_id` の `has_id && id_ok` → `||`: `id_ok` が偽なら `id` は 0 のままで、何もコピーしない結果と同じ |
| A464, A873 | **`eject` の印の解除の補助(PR #158 の分)**: `clear_ejected_flag` の `iso_media_eject(media, false → true)`: `force` は、メディアがあるときの `locked_by_target` だけに効き、この関数は、メディアがないときだけ呼ぶ。`put.epoch` の `0 → 1`(`bdev` がないとき): `has_epoch` が偽なら、`epoch` は比べない |
| A577, A578, A615, A616, A617, A638, A639, A640, A692, A701, A702, A785, A786, A787, A809, A810, A811, A1078, A1079, A1080 | **到達しない防御(500 `internal_error`、「サービスが起動していない」の分岐)**: `proceed()` の `!usable`(公開ビューが空 = `no_card` なので、判定がすべて先に拒否する)、`fail_rc` の未知の正の値、`exec_begin` の `ISO_ERR_INVALID`(名前・size は検査済み)、`exec_mount` / `exec_eject` の `ISO_MEDIA_INVALID`(`media` は必須)、`iso_api_handle` の `default:`(全エンドポイントを列挙済み)。`exec_finish` の `e` が NULL / `meta_valid` が偽の分岐(`SIZE_MISMATCH` と `INVALID_IMAGE` はカタログにあるイメージだけ) |
| J042, J060, J061, J107, J112, J119, J120, J125, J130, J134, J136, J137, J140, J141, J158, J166, J172, J184, J264 | `iso_api_json.c` の、**部分的な `\u`・サロゲートの失敗の分岐の `return false` → `true`、または削除**: `parse_string` が `true` を返しても、`*out_len` は書かれない(呼び出し側の 0 のまま)ので、名前(長さ 0)、`id` / `sha256`(長さが違う)、キー(長さ 0 = 定義していない)として、そのあと必ず拒否される。削除したものは、不正な符号位置(NUL、`0xFFFFFFFF`、サロゲートの UTF-8 符号化、制御文字)が、名前の規則(`iso_name_valid`)か、16 進の検査で、必ず拒否される(多層の防御。名前の規則は、`test_api_json.c` で単独にも固定してある) |
| J044, J047, J049, J148, J154, J249 | `read_u4` と 2 文字目の判定の境界(`len - i < 4`、`< 2`): 境界ちょうどの長さで読み切れる入力は、直後に閉じの引用符がなく、どちらの分岐でも、本体の終わりで拒否される。`literal` の `len - i < n`(`true` / `false` の直後が本体の終わり = `}` がない)も同じ |
| J070, J175, J198 | `cp < 0x80` → `<=`: `0x80` を 1 バイトにしても、名前の規則(C1 制御文字、生の 0x80 の継続バイト)で、必ず拒否される。`J175`(`cp - 0xD800` → `cp + 0xD800`): 差は、ビット 22 以上にだけ出て、`utf8_encode` の `(char)(0xF0 | (cp >> 18))` の `char` への切り詰めで消える(`cp` が 0x10FFFF を超える値でも、出力のバイトは同じ)。`J198`(`len > 20` → `21`): 21 桁は、先頭ゼロ(拒否)か、u64 の超過(拒否) |
| J284, J286, J313, J354 | `K_CURSOR` のビットの値(`1u << 5` → `2u << 5` / `1u << 6`): どちらも 64 で、ほかのキーの印(1〜16)と重ならない。`name[name_len] = '\0'` / `id[ISO_ID_LEN] = '\0'` の削除: `iso_api_parse_body` の先頭の `memset` で、すでに 0 |
| J323, J350 | `sha256` / `id` の `parse_string(...) || n != 長さ` → `&&`: 長さが違う(`n` が短い)とき、後ろの 16 進の検査が、バッファの残り(スタックの値 / `id` は `memset` 済みの 0)を読んで、必ず拒否する(64 桁の `sha256` の最後の 1 文字は、初期化していないスタックなので、偶然 16 進になる可能性は、テストでは作れない)。`iso_id_valid` は、`id` の NUL を拒否する |
| M073 | 書き出しの `iso_w_finish` の、失敗のときの `buf[0] = 0` の条件(`cap > 0` → `cap > 1`): 失敗は、`iso_w_init` が先頭を NUL にした後でしか起きないので、`cap == 1` でも、すでに NUL |

### 3.2 ビルドエラー(21 個。コンパイラが検出するので、殺したものとして数えない)

`-Werror` のコンパイルで、ビルドできない変更。ミュータント側で、括弧の省略や未使用の変数の警告は無効にしてあるので、残ったものは、コンパイラが範囲外などの誤りとして検出したもの。

| ID | 変更箇所 |
|---|---|
| A063 | L68: - -> + |
| A140 | L129: delete statement |
| A293 | L325: delete statement |
| A838 | L843: const 0+1 -> 1 |
| A991 | L962: const 0+1 -> 1 |
| J265 | L166: const 1+1 -> 2 |
| J267 | L166: const 0+1 -> 1 |
| J268 | L167: const 1+1 -> 2 |
| J270 | L167: const 1+1 -> 2 |
| J271 | L167: const 1-1 -> 0 |
| J272 | L168: const 1+1 -> 2 |
| J274 | L168: const 2+1 -> 3 |
| J275 | L168: const 2-1 -> 1 |
| J276 | L169: const 1+1 -> 2 |
| J278 | L169: const 3+1 -> 4 |
| J279 | L169: const 3-1 -> 2 |
| J280 | L170: const 1+1 -> 2 |
| J282 | L170: const 4+1 -> 5 |
| J283 | L170: const 4-1 -> 3 |
| J287 | L171: const 5-1 -> 4 |
| J441 | L272: const 0+1 -> 1 |

### 3.3 殺したミュータントの一覧(1612 個)

| ID | 変更箇所 | 最初に落ちたテスト |
|---|---|---|
| A001 | L12: negate if condition | `test_id_conversions` |
| A002 | L12: false -> true | `test_id_conversions` |
| A003 | L12: delete statement | `test_id_conversions` |
| A005 | L14: < -> <= | `test_id_conversions` |
| A006 | L14: const 0+1 -> 1 | `test_id_conversions` |
| A007 | L16: >= -> > | `test_id_conversions` |
| A008 | L16: - -> + | `test_id_conversions` |
| A009 | L16: + -> - | `test_id_conversions` |
| A010 | L16: - -> + | `test_id_conversions` |
| A011 | L16: const 10+1 -> 11 | `test_id_conversions` |
| A012 | L16: const 10-1 -> 9 | `test_id_conversions` |
| A013 | L17: const 4+1 -> 5 | `test_id_conversions` |
| A014 | L17: const 4-1 -> 3 | `test_id_conversions` |
| A015 | L17: delete statement | `test_id_conversions` |
| A016 | L20: true -> false | `test_id_conversions` |
| A017 | L25: < -> <= | `test_api_route:crash/rc=1` |
| A018 | L25: - -> + | `test_api_route:crash/rc=1` |
| A019 | L25: * -> / | `test_api_route:crash/rc=1` |
| A020 | L25: const 0+1 -> 1 | `test_id_conversions` |
| A021 | L25: const 28+1 -> 29 | `test_id_conversions` |
| A022 | L25: const 28-1 -> 27 | `test_api_route:crash/rc=1` |
| A023 | L25: const 4+1 -> 5 | `test_api_route:crash/rc=1` |
| A024 | L25: const 4-1 -> 3 | `test_id_conversions` |
| A025 | L26: delete statement | `test_id_conversions` |
| A026 | L35: == -> != | `test_route_table`, `test_allow_for_path` |
| A027 | L35: == -> != | `test_route_table`, `test_allow_for_path` |
| A028 | L35: && -> \|\| | `test_api_route:crash/rc=1` |
| A029 | L35: const 0+1 -> 1 | `test_route_table`, `test_allow_for_path` |
| A030 | L38: == -> != | `test_route_table`, `test_allow_for_path` |
| A031 | L38: negate if condition | `test_route_table`, `test_allow_for_path` |
| A032 | L39: delete statement | `test_route_table` |
| A033 | L40: delete statement | `test_route_table` |
| A034 | L42: delete statement | `test_route_table`, `test_allow_for_path` |
| A035 | L43: delete statement | `test_route_table`, `test_allow_for_path` |
| A036 | L49: != -> == | `test_route_table` |
| A037 | L49: negate if condition | `test_route_table` |
| A038 | L49: false -> true | `test_route_table` |
| A039 | L49: delete statement | `test_route_table` |
| A040 | L50: < -> <= | `test_route_table` |
| A041 | L50: const 0+1 -> 1 | `test_route_table` |
| A042 | L51: <= -> < | `test_route_table` |
| A043 | L51: <= -> < | `test_route_table` |
| A044 | L51: >= -> > | `test_route_table` |
| A045 | L51: >= -> > | `test_route_table` |
| A046 | L51: && -> \|\| | `test_route_table` |
| A047 | L51: && -> \|\| | `test_route_table` |
| A048 | L51: \|\| -> && | `test_route_table` |
| A049 | L51: negate if condition | `test_route_table` |
| A050 | L51: false -> true | `test_route_table` |
| A051 | L51: delete statement | `test_route_table` |
| A052 | L53: true -> false | `test_route_table` |
| A053 | L57: true -> false | `test_route_table` |
| A054 | L57: delete statement | `test_route_table` |
| A055 | L58: negate if condition | `test_route_table` |
| A056 | L59: true -> false | `test_route_table` |
| A057 | L59: delete statement | `test_route_table` |
| A058 | L60: delete statement | `test_route_table` |
| A059 | L65: const 0+1 -> 1 | `test_route_table`, `test_route_null_path_is_not_found` |
| A060 | L65: delete statement | `test_api_route:crash/rc=1` |
| A061 | L66: negate if condition | `test_api_route:crash/rc=1` |
| A062 | L66: delete statement | `test_api_route:crash/rc=1` |
| A064 | L69: != -> == | `test_route_table`, `test_allow_for_path` |
| A065 | L69: < -> <= | `test_route_table`, `test_allow_for_path` |
| A066 | L69: \|\| -> && | `test_api_route:crash/rc=1` |
| A067 | L69: negate if condition | `test_route_table`, `test_allow_for_path` |
| A068 | L69: const 0+1 -> 1 | `test_route_table`, `test_allow_for_path` |
| A069 | L69: delete statement | `test_api_route:crash/rc=1` |
| A070 | L70: + -> - | `test_api_route:crash/rc=1` |
| A071 | L71: - -> + | `test_route_table`, `test_allow_for_path` |
| A072 | L73: == -> != | `test_route_table`, `test_allow_for_path` |
| A073 | L73: negate if condition | `test_route_table`, `test_allow_for_path` |
| A074 | L73: const 0+1 -> 1 | `test_route_table`, `test_allow_for_path` |
| A075 | L74: == -> != | `test_route_table`, `test_allow_for_path` |
| A076 | L74: negate if condition | `test_route_table`, `test_allow_for_path` |
| A077 | L75: delete statement | `test_route_table` |
| A078 | L76: delete statement | `test_route_table` |
| A079 | L77: == -> != | `test_route_table`, `test_allow_for_path` |
| A080 | L77: negate if condition | `test_route_table`, `test_allow_for_path` |
| A081 | L78: delete statement | `test_route_table` |
| A082 | L79: delete statement | `test_route_table` |
| A083 | L81: delete statement | `test_route_table`, `test_allow_for_path` |
| A084 | L82: delete statement | `test_route_table`, `test_allow_for_path` |
| A085 | L86: != -> == | `test_route_table`, `test_allow_for_path` |
| A086 | L86: negate if condition | `test_route_table`, `test_allow_for_path` |
| A087 | L86: delete statement | `test_route_table` |
| A088 | L87: delete statement | `test_route_table`, `test_allow_for_path` |
| A089 | L88: delete statement | `test_route_table`, `test_allow_for_path` |
| A090 | L89: const 0+1 -> 1 | `test_route_table` |
| A091 | L90: != -> == | `test_route_table`, `test_allow_for_path` |
| A092 | L90: < -> <= | `test_route_table`, `test_allow_for_path` |
| A093 | L90: && -> \|\| | `test_api_route:crash/rc=1` |
| A094 | L90: delete statement | `test_route_table`, `test_allow_for_path` |
| A095 | L91: == -> != | `test_route_table`, `test_allow_for_path` |
| A096 | L91: negate if condition | `test_route_table`, `test_allow_for_path` |
| A097 | L91: const 0+1 -> 1 | `test_route_table`, `test_allow_for_path` |
| A098 | L91: delete statement | `test_route_table`, `test_allow_for_path` |
| A099 | L93: == -> != | `test_route_table`, `test_allow_for_path` |
| A100 | L93: negate if condition | `test_route_table`, `test_allow_for_path` |
| A101 | L94: negate if condition | `test_route_table`, `test_allow_for_path` |
| A102 | L95: delete statement | `test_route_table`, `test_allow_for_path` |
| A103 | L96: negate if condition | `test_route_table`, `test_allow_for_path` |
| A104 | L97: delete statement | `test_route_table`, `test_allow_for_path` |
| A105 | L98: negate if condition | `test_route_table`, `test_allow_for_path` |
| A106 | L99: delete statement | `test_route_table`, `test_allow_for_path` |
| A107 | L101: delete statement | `test_route_table` |
| A108 | L102: == -> != | `test_route_table`, `test_allow_for_path` |
| A109 | L102: negate if condition | `test_route_table`, `test_allow_for_path` |
| A110 | L103: delete statement | `test_route_table` |
| A111 | L104: delete statement | `test_route_table` |
| A112 | L105: == -> != | `test_route_table`, `test_allow_for_path` |
| A113 | L105: negate if condition | `test_route_table`, `test_allow_for_path` |
| A114 | L106: delete statement | `test_route_table` |
| A115 | L107: delete statement | `test_route_table` |
| A116 | L109: delete statement | `test_route_table`, `test_allow_for_path` |
| A117 | L110: delete statement | `test_route_table`, `test_allow_for_path` |
| A118 | L116: negate if condition | `test_route_table`, `test_allow_for_path` |
| A119 | L116: + -> - | `test_route_table`, `test_allow_for_path` |
| A120 | L116: + -> - | `test_route_table`, `test_allow_for_path` |
| A121 | L116: - -> + | `test_route_table`, `test_allow_for_path` |
| A122 | L116: - -> + | `test_route_table`, `test_allow_for_path` |
| A123 | L116: const 1+1 -> 2 | `test_route_table`, `test_allow_for_path` |
| A124 | L116: const 1-1 -> 0 | `test_route_table`, `test_allow_for_path` |
| A125 | L116: const 1+1 -> 2 | `test_route_table`, `test_allow_for_path` |
| A126 | L116: const 1-1 -> 0 | `test_route_table`, `test_allow_for_path` |
| A127 | L116: delete statement | `test_route_table`, `test_allow_for_path` |
| A128 | L117: delete statement | `test_route_table` |
| A129 | L118: delete statement | `test_route_table`, `test_allow_for_path` |
| A130 | L123: delete statement | `test_allow_for_path` |
| A131 | L124: == -> != | `test_allow_for_path` |
| A132 | L128: == -> != | `test_endpoints_with_a_json_body` |
| A133 | L128: == -> != | `test_endpoints_with_a_json_body` |
| A134 | L128: == -> != | `test_endpoints_with_a_json_body` |
| A135 | L128: == -> != | `test_endpoints_with_a_json_body` |
| A136 | L128: \|\| -> && | `test_endpoints_with_a_json_body` |
| A137 | L128: \|\| -> && | `test_endpoints_with_a_json_body` |
| A138 | L128: \|\| -> && | `test_endpoints_with_a_json_body` |
| A139 | L128: \|\| -> && | `test_endpoints_with_a_json_body` |
| A142 | L134: >= -> > | `test_ctype_table` |
| A143 | L134: && -> \|\| | `test_ctype_table` |
| A144 | L134: - -> + | `test_ctype_table` |
| A145 | L134: + -> - | `test_ctype_table` |
| A146 | L139: < -> <= | `test_ctype_table` |
| A147 | L139: const 0+1 -> 1 | `test_api_route:crash/rc=1` |
| A148 | L140: == -> != | `test_ctype_table` |
| A149 | L140: != -> == | `test_ctype_table` |
| A150 | L140: \|\| -> && | `test_ctype_table` |
| A151 | L140: negate if condition | `test_ctype_table` |
| A152 | L140: const 0+1 -> 1 | `test_ctype_table` |
| A153 | L140: delete statement | `test_ctype_table` |
| A154 | L146: negate if condition | `test_ctype_table` |
| A155 | L146: delete statement | `test_api_route:crash/rc=1` |
| A156 | L148: == -> != | `test_api_route:crash/rc=1` |
| A157 | L148: == -> != | `test_api_route:crash/rc=1` |
| A158 | L148: \|\| -> && | `test_ctype_table` |
| A159 | L148: delete statement | `test_ctype_table` |
| A160 | L151: == -> != | `test_ctype_table` |
| A161 | L151: negate if condition | `test_ctype_table` |
| A162 | L151: const 0+1 -> 1 | `test_ctype_table` |
| A163 | L152: delete statement | `test_ctype_table` |
| A164 | L153: delete statement | `test_ctype_table` |
| A165 | L155: == -> != | `test_ctype_table` |
| A166 | L155: negate if condition | `test_ctype_table` |
| A167 | L155: const 0+1 -> 1 | `test_ctype_table` |
| A168 | L155: delete statement | `test_ctype_table` |
| A169 | L156: += -> -= | `test_api_route:crash/rc=1` |
| A170 | L156: delete statement | `test_ctype_table` |
| A171 | L157: == -> != | `test_api_route:crash/rc=1` |
| A172 | L157: == -> != | `test_api_route:crash/rc=1` |
| A173 | L157: \|\| -> && | `test_ctype_table` |
| A174 | L157: delete statement | `test_ctype_table` |
| A175 | L158: == -> != | `test_ctype_table` |
| A176 | L158: == -> != | `test_ctype_table` |
| A177 | L158: \|\| -> && | `test_ctype_table` |
| A178 | L162: negate if condition | `test_header_status_and_value`, `test_a_truncated_header_is_rejected_at_its_own_step` |
| A179 | L162: delete statement | `test_header_status_and_value`, `test_a_truncated_header_is_rejected_at_its_own_step` |
| A180 | L167: == -> != | `test_header_status_and_value`, `test_a_truncated_header_is_rejected_at_its_own_step` |
| A181 | L167: negate if condition | `test_header_status_and_value`, `test_a_truncated_header_is_rejected_at_its_own_step` |
| A182 | L167: delete statement | `test_header_status_and_value`, `test_a_truncated_header_is_rejected_at_its_own_step` |
| A183 | L168: == -> != | `test_header_status_and_value` |
| A184 | L174: const 0+1 -> 1 | `test_unknown_paths_and_methods_are_404_and_405`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A185 | L174: delete statement | `test_unknown_paths_and_methods_are_404_and_405`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A186 | L175: delete statement | `test_not_started_service_reports_no_card` |
| A187 | L176: delete statement | `test_not_started_service_reports_no_card` |
| A188 | L177: > -> >= | `test_response_init_clears_the_response_and_the_first_byte` |
| A189 | L177: && -> \|\| | `test_response_init_clears_the_response_and_the_first_byte` |
| A190 | L177: negate if condition | `test_response_init_clears_the_response_and_the_first_byte`, `test_entry_points_discard_what_the_response_held_before` |
| A191 | L177: const 0+1 -> 1 | `test_response_init_clears_the_response_and_the_first_byte` |
| A192 | L177: delete statement | `test_response_init_clears_the_response_and_the_first_byte`, `test_entry_points_discard_what_the_response_held_before` |
| A194 | L185: == -> != | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A195 | L185: negate if condition | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A196 | L185: const 0+1 -> 1 | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A197 | L186: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A198 | L187: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A199 | L188: delete statement | `test_begin_accepted`, `test_finish_flow_with_a_declared_sha256` |
| A200 | L192: delete statement | `test_a_body_that_does_not_fit_becomes_the_short_internal_error` |
| A201 | L193: delete statement | `test_a_body_that_does_not_fit_becomes_the_short_internal_error` |
| A202 | L194: delete statement | `test_a_body_that_does_not_fit_becomes_the_short_internal_error` |
| A203 | L195: const 500+1 -> 501 | `test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty` |
| A204 | L195: const 500-1 -> 499 | `test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty` |
| A205 | L195: delete statement | `test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty` |
| A206 | L196: delete statement | `test_a_body_that_does_not_fit_becomes_the_short_internal_error` |
| A207 | L201: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A208 | L202: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A209 | L203: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A210 | L204: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A211 | L205: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_unknown_paths_and_methods_are_404_and_405` |
| A212 | L210: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A213 | L211: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A214 | L212: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A215 | L213: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A216 | L214: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A217 | L215: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A218 | L216: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A219 | L217: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A220 | L218: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A221 | L223: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A222 | L224: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A223 | L225: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A224 | L226: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A225 | L227: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A226 | L228: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A227 | L229: const 409+1 -> 410 | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A228 | L229: const 409-1 -> 408 | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A229 | L229: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A230 | L233: const 503+1 -> 504 | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A231 | L233: const 503-1 -> 502 | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A232 | L233: delete statement | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A233 | L234: true -> false | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A234 | L234: delete statement | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A235 | L235: const 1+1 -> 2 | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A236 | L235: const 1-1 -> 0 | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A237 | L235: delete statement | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A238 | L238: const 503+1 -> 504 | `test_step6_format_errors_are_400_before_the_sd_check`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A239 | L238: const 503-1 -> 502 | `test_step6_format_errors_are_400_before_the_sd_check`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A240 | L243: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A241 | L244: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A242 | L245: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A243 | L246: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A244 | L247: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A245 | L248: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A246 | L249: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A247 | L255: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A248 | L256: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A249 | L257: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A250 | L258: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A251 | L259: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A252 | L260: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A253 | L261: const 200+1 -> 201 | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A254 | L261: const 200-1 -> 199 | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A255 | L261: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A256 | L278: negate if condition | `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A257 | L279: const 500+1 -> 501 | `test_an_unknown_invalid_reason_in_the_view_is_a_500` |
| A258 | L279: const 500-1 -> 499 | `test_an_unknown_invalid_reason_in_the_view_is_a_500` |
| A259 | L279: delete statement | `test_an_unknown_invalid_reason_in_the_view_is_a_500` |
| A260 | L282: const 422+1 -> 423 | `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A261 | L282: const 422-1 -> 421 | `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A262 | L282: delete statement | `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A263 | L287: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A264 | L288: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A265 | L289: negate if condition | `test_api_order:crash/rc=1` |
| A266 | L290: delete statement | `test_finish_flow_with_a_declared_sha256` |
| A267 | L291: delete statement | `test_finish_flow_with_a_declared_sha256` |
| A268 | L292: delete statement | `test_finish_flow_with_a_declared_sha256` |
| A269 | L294: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A270 | L296: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A271 | L297: const 200+1 -> 201 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A272 | L297: const 200-1 -> 199 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A273 | L297: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A274 | L300: + -> - | `test_api_ops:crash/rc=1` |
| A275 | L300: const 1+1 -> 2 | `test_api_order:crash/rc=1` |
| A277 | L305: delete statement | `test_snapshot_update_copies_the_view_and_drops_the_table_pointers` |
| A279 | L306: negate if condition | `test_snapshot_update_copies_the_view_and_drops_the_table_pointers` |
| A280 | L306: delete statement | `test_snapshot_update_copies_the_view_and_drops_the_table_pointers` |
| A281 | L307: < -> <= | `test_api_list:crash/rc=1` |
| A282 | L307: const 0+1 -> 1 | `test_snapshot_update_copies_the_view_and_drops_the_table_pointers` |
| A283 | L308: const 0+1 -> 1 | `test_snapshot_update_copies_the_view_and_drops_the_table_pointers` |
| A284 | L308: delete statement | `test_snapshot_update_copies_the_view_and_drops_the_table_pointers` |
| A285 | L319: && -> \|\| | `test_api_list:crash/rc=1` |
| A286 | L319: && -> \|\| | `test_api_list:crash/rc=1` |
| A287 | L319: && -> \|\| | `test_api_list:crash/rc=1` |
| A288 | L319: && -> \|\| | `test_api_list:crash/rc=1` |
| A289 | L319: && -> \|\| | `test_api_list:crash/rc=1` |
| A290 | L319: && -> \|\| | `test_api_list:crash/rc=1` |
| A291 | L319: && -> \|\| | `test_api_list:crash/rc=1` |
| A292 | L324: negate if condition | `test_api_list:crash/rc=1` |
| A294 | L328: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A295 | L329: delete statement | `test_api_order:crash/rc=1` |
| A296 | L330: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step9_delete_of_the_mounted_image_is_409` |
| A297 | L334: negate if condition | `test_api_list:crash/rc=1` |
| A298 | L334: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A299 | L337: == -> != | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A300 | L340: < -> <= | `test_api_edges:crash/rc=1` |
| A301 | L340: const 0+1 -> 1 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A302 | L341: == -> != | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A303 | L341: != -> == | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A304 | L341: && -> \|\| | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step9_mount_image_not_ready_before_locked_by_target` |
| A305 | L341: negate if condition | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A306 | L341: const 0+1 -> 1 | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_mount_image_not_ready_before_locked_by_target` |
| A307 | L341: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A308 | L349: negate if condition | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A309 | L349: const 0+1 -> 1 | `test_step12_begin_without_a_sampled_free_size_is_507_with_zero`, `test_step12_put_resume_storage_boundary` |
| A310 | L349: delete statement | `test_step12_begin_without_a_sampled_free_size_is_507_with_zero`, `test_step12_put_resume_storage_boundary` |
| A311 | L352: < -> <= | `test_api_edges:crash/rc=1` |
| A312 | L352: const 0+1 -> 1 | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A313 | L353: == -> != | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A314 | L353: \|\| -> && | `test_step12_put_resume_storage_boundary`, `test_the_resume_check_does_not_count_the_image_itself_twice` |
| A315 | L353: negate if condition | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A316 | L353: delete statement | `test_step12_put_resume_storage_boundary`, `test_the_resume_check_does_not_count_the_image_itself_twice` |
| A317 | L354: != -> == | `test_estimate_ignores_images_that_are_not_being_uploaded` |
| A318 | L354: != -> == | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A319 | L354: && -> \|\| | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A320 | L354: negate if condition | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A321 | L354: delete statement | `test_estimate_ignores_images_that_are_not_being_uploaded` |
| A323 | L355: negate if condition | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A324 | L355: - -> + | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A325 | L355: += -> -= | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A326 | L355: delete statement | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A328 | L357: - -> + | `test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A329 | L357: const 0+1 -> 1 | `test_estimate_is_zero_when_the_used_bytes_exceed_the_free_space` |
| A330 | L361: < -> <= | `test_api_order:crash/rc=1` |
| A331 | L361: const 0+1 -> 1 | `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A332 | L362: == -> != | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A333 | L362: == -> != | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit` |
| A335 | L362: \|\| -> && | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit` |
| A336 | L362: negate if condition | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit` |
| A337 | L362: delete statement | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit` |
| A338 | L363: negate if condition | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A339 | L363: true -> false | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A340 | L363: delete statement | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A341 | L365: false -> true | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit` |
| A342 | L368: != -> == | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A343 | L368: * -> / | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A344 | L368: const 0+1 -> 1 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A345 | L368: const 1024+1 -> 1025 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A346 | L368: const 1024-1 -> 1023 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A347 | L385: delete statement | `test_not_started_service_reports_no_card` |
| A348 | L386: == -> != | `test_not_started_service_reports_no_card` |
| A349 | L386: negate if condition | `test_not_started_service_reports_no_card` |
| A350 | L387: const 404+1 -> 405 | `test_unknown_paths_and_methods_are_404_and_405` |
| A351 | L387: const 404-1 -> 403 | `test_unknown_paths_and_methods_are_404_and_405` |
| A352 | L387: delete statement | `test_unknown_paths_and_methods_are_404_and_405` |
| A353 | L388: false -> true | `test_unknown_paths_and_methods_are_404_and_405` |
| A354 | L390: == -> != | `test_not_started_service_reports_no_card` |
| A355 | L390: negate if condition | `test_not_started_service_reports_no_card` |
| A356 | L391: const 405+1 -> 406 | `test_unknown_paths_and_methods_are_404_and_405`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A357 | L391: const 405-1 -> 404 | `test_unknown_paths_and_methods_are_404_and_405`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A358 | L391: delete statement | `test_unknown_paths_and_methods_are_404_and_405`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A359 | L392: delete statement | `test_unknown_paths_and_methods_are_404_and_405` |
| A360 | L393: false -> true | `test_unknown_paths_and_methods_are_404_and_405`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A361 | L395: delete statement | `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A362 | L396: delete statement | `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A364 | L397: negate if condition | `test_finish_flow_with_a_declared_sha256`, `test_delete_flow` |
| A365 | L397: delete statement | `test_finish_flow_with_a_declared_sha256`, `test_delete_flow` |
| A366 | L400: && -> \|\| | `test_not_started_service_reports_no_card` |
| A367 | L400: negate if condition | `test_not_started_service_reports_no_card` |
| A368 | L401: const 404+1 -> 405 | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A369 | L401: const 404-1 -> 403 | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A370 | L401: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A371 | L402: false -> true | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A372 | L408: false -> true | `test_not_started_service_reports_no_card` |
| A373 | L418: negate if condition | `test_not_started_service_reports_no_card` |
| A374 | L419: const 415+1 -> 416 | `test_step4_content_type_is_415_before_the_length_checks` |
| A375 | L419: const 415-1 -> 414 | `test_step4_content_type_is_415_before_the_length_checks` |
| A376 | L419: delete statement | `test_step4_content_type_is_415_before_the_length_checks` |
| A377 | L420: false -> true | `test_step4_content_type_is_415_before_the_length_checks` |
| A378 | L424: == -> != | `test_not_started_service_reports_no_card` |
| A379 | L424: && -> \|\| | `test_not_started_service_reports_no_card` |
| A380 | L424: negate if condition | `test_not_started_service_reports_no_card` |
| A381 | L425: const 411+1 -> 412 | `test_step5_put_needs_a_content_length` |
| A382 | L425: const 411-1 -> 410 | `test_step5_put_needs_a_content_length` |
| A383 | L425: delete statement | `test_step5_put_needs_a_content_length` |
| A384 | L426: false -> true | `test_step5_put_needs_a_content_length` |
| A385 | L428: > -> >= | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed` |
| A386 | L428: && -> \|\| | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed` |
| A387 | L428: && -> \|\| | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed` |
| A388 | L428: negate if condition | `test_not_started_service_reports_no_card` |
| A389 | L429: const 413+1 -> 414 | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed` |
| A390 | L429: const 413-1 -> 412 | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed` |
| A391 | L429: delete statement | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed` |
| A392 | L430: false -> true | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed` |
| A393 | L432: true -> false | `test_not_started_service_reports_no_card` |
| A394 | L436: negate if condition | `test_api_edges:crash/rc=1` |
| A395 | L436: false -> true | `test_headers_check_directly` |
| A396 | L436: delete statement | `test_api_edges:crash/rc=1` |
| A397 | L437: delete statement | `test_entry_points_discard_what_the_response_held_before` |
| A398 | L438: \|\| -> && | `test_api_edges:crash/rc=1` |
| A399 | L438: negate if condition | `test_api_edges:crash/rc=1` |
| A400 | L439: const 500+1 -> 501 | `test_headers_check_directly` |
| A401 | L439: const 500-1 -> 499 | `test_headers_check_directly` |
| A402 | L439: delete statement | `test_headers_check_directly` |
| A403 | L440: false -> true | `test_headers_check_directly` |
| A404 | L444: == -> != | `test_headers_check_directly` |
| A405 | L444: && -> \|\| | `test_headers_check_directly` |
| A406 | L444: negate if condition | `test_entry_points_discard_what_the_response_held_before`, `test_headers_check_directly` |
| A407 | L444: true -> false | `test_headers_check_directly` |
| A408 | L444: delete statement | `test_headers_check_directly` |
| A409 | L454: negate if condition | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step9_begin_conflicts_in_the_documented_order` |
| A410 | L455: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A411 | L456: false -> true | `test_step7_sd_not_ready_is_503_before_not_found` |
| A412 | L458: negate if condition | `test_step9_begin_conflicts_in_the_documented_order`, `test_step9_begin_name_exists_then_too_many_images_then_507` |
| A413 | L459: const 409+1 -> 410 | `test_step9_begin_conflicts_in_the_documented_order` |
| A414 | L459: const 409-1 -> 408 | `test_step9_begin_conflicts_in_the_documented_order` |
| A415 | L459: delete statement | `test_step9_begin_conflicts_in_the_documented_order` |
| A416 | L460: false -> true | `test_step9_begin_conflicts_in_the_documented_order` |
| A417 | L462: negate if condition | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit` |
| A418 | L463: const 409+1 -> 410 | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A419 | L463: const 409-1 -> 408 | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A420 | L463: delete statement | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A421 | L464: false -> true | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A422 | L466: >= -> > | `test_step9_begin_name_exists_then_too_many_images_then_507` |
| A423 | L466: negate if condition | `test_step9_begin_name_exists_then_too_many_images_then_507`, `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit` |
| A424 | L467: const 409+1 -> 410 | `test_step9_begin_name_exists_then_too_many_images_then_507` |
| A425 | L467: const 409-1 -> 408 | `test_step9_begin_name_exists_then_too_many_images_then_507` |
| A426 | L467: delete statement | `test_step9_begin_name_exists_then_too_many_images_then_507` |
| A427 | L468: false -> true | `test_step9_begin_name_exists_then_too_many_images_then_507` |
| A428 | L471: < -> <= | `test_step12_begin_fits_exactly_with_the_reserve` |
| A429 | L471: > -> >= | `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A430 | L471: \|\| -> && | `test_begin_larger_than_the_card_is_decided_from_the_view_without_the_op_lock` |
| A431 | L471: \|\| -> && | `test_step12_begin_one_byte_short_is_507` |
| A432 | L471: negate if condition | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A433 | L471: + -> - | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A434 | L472: const 507+1 -> 508 | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A435 | L472: const 507-1 -> 506 | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A436 | L472: delete statement | `test_step12_begin_one_byte_short_is_507`, `test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space` |
| A437 | L473: false -> true | `test_step12_begin_one_byte_short_is_507` |
| A438 | L475: true -> false | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A439 | L480: negate if condition | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A440 | L481: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A441 | L482: false -> true | `test_step7_sd_not_ready_is_503_before_not_found` |
| A442 | L486: const 404+1 -> 405 | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A443 | L486: const 404-1 -> 403 | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A444 | L486: delete statement | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A445 | L487: false -> true | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A446 | L489: != -> == | `test_step9_mount_image_not_ready_before_locked_by_target`, `test_step9_mount_already_mounted_wins_over_the_lock` |
| A447 | L489: negate if condition | `test_step9_mount_image_not_ready_before_locked_by_target`, `test_step9_mount_already_mounted_wins_over_the_lock` |
| A448 | L490: const 409+1 -> 410 | `test_step9_mount_image_not_ready_before_locked_by_target`, `test_step9_mount_of_a_ready_image_without_a_table_is_image_not_ready` |
| A449 | L490: const 409-1 -> 408 | `test_step9_mount_image_not_ready_before_locked_by_target`, `test_step9_mount_of_a_ready_image_without_a_table_is_image_not_ready` |
| A450 | L490: delete statement | `test_step9_mount_image_not_ready_before_locked_by_target`, `test_step9_mount_of_a_ready_image_without_a_table_is_image_not_ready` |
| A451 | L491: false -> true | `test_step9_mount_image_not_ready_before_locked_by_target` |
| A452 | L493: == -> != | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A453 | L493: && -> \|\| | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A454 | L493: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A455 | L494: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A456 | L495: false -> true | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A457 | L497: && -> \|\| | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A458 | L497: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A459 | L498: const 409+1 -> 410 | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A460 | L498: const 409-1 -> 408 | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A461 | L498: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A462 | L501: true -> false | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A463 | L507: negate if condition | `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A465 | L507: delete statement | `test_eject_while_the_sd_is_unusable_clears_ejected_by_target` |
| A466 | L511: negate if condition | `test_step7_eject_is_the_exception_it_answers_200_not_mounted`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A467 | L512: const 200+1 -> 201 | `test_step7_eject_is_the_exception_it_answers_200_not_mounted`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A468 | L512: const 200-1 -> 199 | `test_step7_eject_is_the_exception_it_answers_200_not_mounted`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A469 | L512: delete statement | `test_step7_eject_is_the_exception_it_answers_200_not_mounted`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A470 | L513: false -> true | `test_step7_eject_is_the_exception_it_answers_200_not_mounted`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A471 | L515: && -> \|\| | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A472 | L515: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A473 | L516: const 409+1 -> 410 | `test_eject_is_refused_while_the_target_prevents_removal_unless_forced` |
| A474 | L516: const 409-1 -> 408 | `test_eject_is_refused_while_the_target_prevents_removal_unless_forced` |
| A475 | L516: delete statement | `test_eject_is_refused_while_the_target_prevents_removal_unless_forced` |
| A476 | L519: true -> false | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A477 | L523: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A478 | L524: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A479 | L525: false -> true | `test_step7_sd_not_ready_is_503_before_not_found` |
| A480 | L527: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_rescan_during_an_upload_is_409` |
| A481 | L528: const 409+1 -> 410 | `test_step9_rescan_during_an_upload_is_409` |
| A482 | L528: const 409-1 -> 408 | `test_step9_rescan_during_an_upload_is_409` |
| A483 | L528: delete statement | `test_step9_rescan_during_an_upload_is_409` |
| A484 | L529: false -> true | `test_step9_rescan_during_an_upload_is_409` |
| A485 | L531: true -> false | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A486 | L536: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A487 | L537: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A488 | L538: false -> true | `test_step7_sd_not_ready_is_503_before_not_found` |
| A489 | L542: const 404+1 -> 405 | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A490 | L542: const 404-1 -> 403 | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A491 | L542: delete statement | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A492 | L543: false -> true | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A493 | L547: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A494 | L548: false -> true | `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A495 | L550: delete statement | `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A496 | L551: false -> true | `test_step9_finish_ready_and_invalid_are_answered_from_the_state` |
| A497 | L554: < -> <= | `test_step9_finish_size_mismatch_before_upload_in_progress`, `test_step13_busy_for_finish_and_put_commit` |
| A498 | L554: negate if condition | `test_step9_finish_size_mismatch_before_upload_in_progress`, `test_step13_busy_for_finish_and_put_commit` |
| A499 | L555: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A500 | L556: false -> true | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A501 | L558: == -> != | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A502 | L558: && -> \|\| | `test_step13_busy_for_finish_and_put_commit` |
| A503 | L558: negate if condition | `test_step9_finish_size_mismatch_before_upload_in_progress`, `test_step13_busy_for_finish_and_put_commit` |
| A504 | L559: const 409+1 -> 410 | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A505 | L559: const 409-1 -> 408 | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A506 | L559: delete statement | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A507 | L560: false -> true | `test_step9_finish_size_mismatch_before_upload_in_progress` |
| A508 | L562: true -> false | `test_step13_busy_for_finish_and_put_commit` |
| A509 | L568: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A510 | L569: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A511 | L570: false -> true | `test_step7_sd_not_ready_is_503_before_not_found` |
| A512 | L573: negate if condition | `test_api_order:crash/rc=1` |
| A513 | L574: const 404+1 -> 405 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A514 | L574: const 404-1 -> 403 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A515 | L574: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A516 | L575: false -> true | `test_step8_not_found_is_404_before_the_state_conflicts`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A517 | L577: == -> != | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A518 | L577: != -> == | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A519 | L577: && -> \|\| | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A520 | L577: && -> \|\| | `test_step9_delete_of_the_mounted_image_is_409` |
| A521 | L577: negate if condition | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A522 | L578: const 409+1 -> 410 | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A523 | L578: const 409-1 -> 408 | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A524 | L578: delete statement | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A525 | L579: false -> true | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A526 | L581: true -> false | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A527 | L587: negate if condition | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A528 | L588: delete statement | `test_step6_format_errors_are_400_before_the_sd_check`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A529 | L589: false -> true | `test_step6_format_errors_are_400_before_the_sd_check`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A530 | L593: const 404+1 -> 405 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A531 | L593: const 404-1 -> 403 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A532 | L593: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A533 | L594: false -> true | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A534 | L596: != -> == | `test_step9_put_to_images_that_cannot_receive_is_not_uploading`, `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch` |
| A535 | L596: != -> == | `test_step9_put_to_images_that_cannot_receive_is_not_uploading`, `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch` |
| A536 | L596: && -> \|\| | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A537 | L596: negate if condition | `test_step9_put_to_images_that_cannot_receive_is_not_uploading`, `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch` |
| A538 | L597: const 409+1 -> 410 | `test_step9_put_to_images_that_cannot_receive_is_not_uploading` |
| A539 | L597: const 409-1 -> 408 | `test_step9_put_to_images_that_cannot_receive_is_not_uploading` |
| A540 | L597: delete statement | `test_step9_put_to_images_that_cannot_receive_is_not_uploading` |
| A541 | L598: false -> true | `test_step9_put_to_images_that_cannot_receive_is_not_uploading` |
| A542 | L600: == -> != | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A543 | L600: && -> \|\| | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A544 | L600: negate if condition | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A545 | L601: const 409+1 -> 410 | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch` |
| A546 | L601: const 409-1 -> 408 | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch` |
| A547 | L601: delete statement | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch` |
| A548 | L602: false -> true | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch` |
| A549 | L604: != -> == | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A550 | L604: negate if condition | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A551 | L605: const 409+1 -> 410 | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A552 | L605: const 409-1 -> 408 | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A553 | L605: delete statement | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A554 | L606: false -> true | `test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch`, `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A555 | L608: > -> >= | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A556 | L608: negate if condition | `test_step10_offset_mismatch_before_exceeds_size_and_storage`, `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A557 | L608: - -> + | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A558 | L609: const 400+1 -> 401 | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A559 | L609: const 400-1 -> 399 | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A560 | L609: delete statement | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A561 | L610: false -> true | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check` |
| A562 | L612: == -> != | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A563 | L612: negate if condition | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A564 | L613: - -> + | `test_step12_put_resume_storage_boundary`, `test_the_resume_check_does_not_count_the_image_itself_twice` |
| A565 | L614: < -> <= | `test_step12_put_resume_storage_boundary`, `test_the_resume_check_does_not_count_the_image_itself_twice` |
| A566 | L614: negate if condition | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A567 | L614: + -> - | `test_step12_put_resume_storage_boundary`, `test_the_resume_check_does_not_count_the_image_itself_twice` |
| A568 | L615: const 507+1 -> 508 | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A569 | L615: const 507-1 -> 506 | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A570 | L615: delete statement | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A571 | L616: false -> true | `test_step11_chunk_beyond_the_size_is_400_before_the_storage_check`, `test_step12_put_resume_storage_boundary` |
| A572 | L619: true -> false | `test_step10_offset_mismatch_before_exceeds_size_and_storage`, `test_step12_put_resume_storage_boundary` |
| A573 | L625: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A574 | L625: false -> true | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A575 | L625: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A576 | L626: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A579 | L630: true -> false | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A580 | L637: != -> == | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_the_op_lock_wait_can_be_configured` |
| A581 | L637: const 0+1 -> 1 | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_the_op_lock_wait_can_be_configured` |
| A582 | L638: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A583 | L639: delete statement | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A584 | L640: false -> true | `test_step13_busy_when_the_op_lock_cannot_be_taken`, `test_step13_busy_for_finish_and_put_commit` |
| A585 | L642: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A586 | L644: && -> \|\| | `test_only_eject_clears_ejected_by_target_when_the_sd_goes_away` |
| A587 | L644: negate if condition | `test_eject_while_the_sd_is_unusable_clears_ejected_by_target`, `test_only_eject_clears_ejected_by_target_when_the_sd_goes_away` |
| A588 | L644: delete statement | `test_eject_while_the_sd_is_unusable_clears_ejected_by_target` |
| A589 | L645: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A590 | L646: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A591 | L647: delete statement | `test_commit_rechecks_the_sd_after_taking_the_op_lock`, `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A592 | L648: negate if condition | `test_commit_rechecks_the_sd_after_taking_the_op_lock`, `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A593 | L649: const 200+1 -> 201 | `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A594 | L649: const 200-1 -> 199 | `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A595 | L649: delete statement | `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A596 | L651: delete statement | `test_commit_rechecks_the_sd_after_taking_the_op_lock`, `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A597 | L653: false -> true | `test_commit_rechecks_the_sd_after_taking_the_op_lock`, `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A598 | L655: true -> false | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A599 | L662: == -> != | `test_begin_io_errors`, `test_commit_write_failures` |
| A600 | L662: == -> != | `test_begin_io_errors`, `test_begin_exhausted_ids_are_a_retryable_503` |
| A601 | L662: \|\| -> && | `test_begin_exhausted_ids_are_a_retryable_503`, `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing` |
| A602 | L662: negate if condition | `test_begin_io_errors`, `test_begin_exhausted_ids_are_a_retryable_503` |
| A603 | L663: delete statement | `test_begin_exhausted_ids_are_a_retryable_503`, `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing` |
| A604 | L664: == -> != | `test_begin_io_errors`, `test_commit_write_failures` |
| A605 | L664: negate if condition | `test_begin_io_errors`, `test_commit_write_failures` |
| A606 | L665: const 507+1 -> 508 | `test_begin_io_errors` |
| A607 | L665: const 507-1 -> 506 | `test_begin_io_errors` |
| A608 | L665: delete statement | `test_begin_io_errors` |
| A610 | L666: negate if condition | `test_begin_io_errors`, `test_commit_write_failures` |
| A612 | L667: const 503+1 -> 504 | `test_begin_io_errors`, `test_commit_write_failures` |
| A613 | L667: const 503-1 -> 502 | `test_begin_io_errors`, `test_commit_write_failures` |
| A614 | L667: delete statement | `test_begin_io_errors`, `test_commit_write_failures` |
| A618 | L674: negate if condition | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A619 | L674: false -> true | `test_every_changing_operation_rechecks_the_sd_after_taking_the_op_lock`, `test_only_eject_clears_ejected_by_target_when_the_sd_goes_away` |
| A620 | L674: delete statement | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A621 | L676: + -> - | `test_api_order:crash/rc=1` |
| A623 | L676: const 1-1 -> 0 | `test_api_order:crash/rc=1` |
| A624 | L678: == -> != | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A625 | L678: negate if condition | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A626 | L679: delete statement | `test_api_ops:crash/rc=1` |
| A627 | L681: delete statement | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A628 | L682: delete statement | `test_begin_accepted` |
| A629 | L683: delete statement | `test_begin_accepted` |
| A630 | L684: delete statement | `test_begin_accepted` |
| A631 | L685: delete statement | `test_begin_accepted` |
| A632 | L686: delete statement | `test_begin_accepted` |
| A633 | L687: const 201+1 -> 202 | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A634 | L687: const 201-1 -> 200 | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A635 | L687: delete statement | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A636 | L688: == -> != | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A637 | L688: negate if condition | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A641 | L690: == -> != | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A642 | L690: negate if condition | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A643 | L691: const 409+1 -> 410 | `test_begin_exec_mapping_when_the_view_is_stale` |
| A644 | L691: const 409-1 -> 408 | `test_begin_exec_mapping_when_the_view_is_stale` |
| A645 | L691: delete statement | `test_begin_exec_mapping_when_the_view_is_stale` |
| A646 | L692: == -> != | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A647 | L692: == -> != | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A648 | L692: \|\| -> && | `test_begin_exec_mapping_when_the_view_is_stale` |
| A649 | L692: negate if condition | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A650 | L693: const 409+1 -> 410 | `test_begin_exec_mapping_when_the_view_is_stale` |
| A651 | L693: const 409-1 -> 408 | `test_begin_exec_mapping_when_the_view_is_stale` |
| A652 | L693: delete statement | `test_begin_exec_mapping_when_the_view_is_stale` |
| A653 | L694: == -> != | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A654 | L694: == -> != | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_io_errors` |
| A655 | L694: \|\| -> && | `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A656 | L694: negate if condition | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A657 | L695: const 409+1 -> 410 | `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A658 | L695: const 409-1 -> 408 | `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A659 | L695: delete statement | `test_begin_exec_too_many_images_when_the_view_is_stale` |
| A660 | L696: == -> != | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_io_errors` |
| A661 | L696: negate if condition | `test_begin_exec_mapping_when_the_view_is_stale`, `test_begin_io_errors` |
| A662 | L697: const 507+1 -> 508 | `test_begin_exec_mapping_when_the_view_is_stale` |
| A663 | L697: const 507-1 -> 506 | `test_begin_exec_mapping_when_the_view_is_stale` |
| A664 | L697: delete statement | `test_begin_exec_mapping_when_the_view_is_stale` |
| A665 | L699: delete statement | `test_begin_io_errors`, `test_begin_exhausted_ids_are_a_retryable_503` |
| A666 | L701: delete statement | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A667 | L705: negate if condition | `test_step13_busy_for_finish_and_put_commit` |
| A668 | L705: false -> true | `test_every_changing_operation_rechecks_the_sd_after_taking_the_op_lock` |
| A669 | L705: delete statement | `test_step13_busy_for_finish_and_put_commit` |
| A671 | L710: == -> != | `test_finish_flow_with_a_declared_sha256`, `test_finish_with_a_wrong_declared_sha256_ends_invalid` |
| A672 | L710: == -> != | `test_finish_flow_with_a_declared_sha256`, `test_finish_exec_mapping_when_the_view_is_stale` |
| A673 | L710: == -> != | `test_finish_exec_mapping_when_the_view_is_stale`, `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A674 | L710: \|\| -> && | `test_finish_flow_with_a_declared_sha256`, `test_finish_with_a_wrong_declared_sha256_ends_invalid` |
| A675 | L710: \|\| -> && | `test_finish_flow_with_a_declared_sha256` |
| A676 | L710: negate if condition | `test_finish_flow_with_a_declared_sha256`, `test_finish_with_a_wrong_declared_sha256_ends_invalid` |
| A677 | L711: const 202+1 -> 203 | `test_finish_flow_with_a_declared_sha256`, `test_finish_with_a_wrong_declared_sha256_ends_invalid` |
| A678 | L711: const 202-1 -> 201 | `test_finish_flow_with_a_declared_sha256`, `test_finish_with_a_wrong_declared_sha256_ends_invalid` |
| A679 | L711: delete statement | `test_finish_flow_with_a_declared_sha256`, `test_finish_with_a_wrong_declared_sha256_ends_invalid` |
| A680 | L712: == -> != | `test_finish_of_a_ready_image_without_a_table_is_still_finalizing`, `test_finish_exec_mapping_when_the_view_is_stale` |
| A681 | L712: negate if condition | `test_finish_of_a_ready_image_without_a_table_is_still_finalizing`, `test_finish_exec_mapping_when_the_view_is_stale` |
| A682 | L713: \|\| -> && | `test_finish_of_a_ready_image_without_a_table_is_still_finalizing` |
| A683 | L713: negate if condition | `test_finish_of_a_ready_image_without_a_table_is_still_finalizing` |
| A684 | L714: const 202+1 -> 203 | `test_finish_of_a_ready_image_without_a_table_is_still_finalizing` |
| A685 | L714: const 202-1 -> 201 | `test_finish_of_a_ready_image_without_a_table_is_still_finalizing` |
| A686 | L714: delete statement | `test_finish_of_a_ready_image_without_a_table_is_still_finalizing` |
| A687 | L716: == -> != | `test_finish_that_finds_the_image_ready_reports_the_computed_sha_only` |
| A688 | L716: && -> \|\| | `test_finish_that_finds_the_image_ready_reports_the_computed_sha_only` |
| A689 | L716: delete statement | `test_finish_that_finds_the_image_ready_reports_the_computed_sha_only` |
| A690 | L718: == -> != | `test_finish_exec_mapping_when_the_view_is_stale`, `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A691 | L718: negate if condition | `test_finish_exec_mapping_when_the_view_is_stale`, `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A693 | L719: delete statement | `test_finish_exec_mapping_when_the_view_is_stale` |
| A694 | L720: == -> != | `test_finish_exec_mapping_when_the_view_is_stale`, `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A695 | L720: negate if condition | `test_finish_exec_mapping_when_the_view_is_stale`, `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A696 | L721: const 404+1 -> 405 | `test_finish_exec_mapping_when_the_view_is_stale` |
| A697 | L721: const 404-1 -> 403 | `test_finish_exec_mapping_when_the_view_is_stale` |
| A698 | L721: delete statement | `test_finish_exec_mapping_when_the_view_is_stale` |
| A699 | L722: == -> != | `test_finish_exec_mapping_when_the_view_is_stale`, `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A700 | L722: negate if condition | `test_finish_exec_mapping_when_the_view_is_stale`, `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A703 | L723: delete statement | `test_finish_exec_mapping_when_the_view_is_stale` |
| A704 | L724: == -> != | `test_finish_exec_upload_in_progress_when_the_view_is_stale`, `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing` |
| A705 | L724: negate if condition | `test_finish_exec_upload_in_progress_when_the_view_is_stale`, `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing` |
| A706 | L725: const 409+1 -> 410 | `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A707 | L725: const 409-1 -> 408 | `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A708 | L725: delete statement | `test_finish_exec_upload_in_progress_when_the_view_is_stale` |
| A709 | L727: delete statement | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_finish_io_error_while_reloading_received_is_503` |
| A710 | L729: delete statement | `test_finish_flow_with_a_declared_sha256`, `test_finish_with_a_wrong_declared_sha256_ends_invalid` |
| A711 | L733: negate if condition | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A712 | L733: false -> true | `test_every_changing_operation_rechecks_the_sd_after_taking_the_op_lock`, `test_only_eject_clears_ejected_by_target_when_the_sd_goes_away` |
| A713 | L733: delete statement | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A714 | L737: != -> == | `test_step9_mounted_is_reported_before_crosslinked` |
| A715 | L737: != -> == | `test_step9_mounted_is_reported_before_crosslinked` |
| A716 | L737: && -> \|\| | `test_api_ops:crash/rc=1` |
| A717 | L737: && -> \|\| | `test_api_ops:crash/rc=1` |
| A718 | L737: negate if condition | `test_step9_mounted_is_reported_before_crosslinked` |
| A719 | L739: delete statement | `test_step9_mounted_is_reported_before_crosslinked` |
| A720 | L740: == -> != | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A721 | L740: negate if condition | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A722 | L741: const 409+1 -> 410 | `test_step9_mounted_is_reported_before_crosslinked` |
| A723 | L741: const 409-1 -> 408 | `test_step9_mounted_is_reported_before_crosslinked` |
| A724 | L741: delete statement | `test_step9_mounted_is_reported_before_crosslinked` |
| A725 | L742: delete statement | `test_step9_mounted_is_reported_before_crosslinked` |
| A726 | L746: == -> != | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_flow` |
| A727 | L746: negate if condition | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_flow` |
| A728 | L746: delete statement | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_flow` |
| A729 | L747: == -> != | `test_step9_delete_of_the_mounted_image_is_409` |
| A730 | L747: == -> != | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish` |
| A731 | L747: \|\| -> && | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A732 | L747: negate if condition | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A733 | L748: const 202+1 -> 203 | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A734 | L748: const 202-1 -> 201 | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A735 | L748: delete statement | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A736 | L749: == -> != | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_exec_mapping` |
| A737 | L749: negate if condition | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_exec_mapping` |
| A738 | L750: const 404+1 -> 405 | `test_delete_exec_mapping` |
| A739 | L750: const 404-1 -> 403 | `test_delete_exec_mapping` |
| A740 | L750: delete statement | `test_delete_exec_mapping` |
| A741 | L751: == -> != | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_exec_mapping` |
| A742 | L751: negate if condition | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_exec_mapping` |
| A743 | L752: const 409+1 -> 410 | `test_delete_exec_mapping` |
| A744 | L752: const 409-1 -> 408 | `test_delete_exec_mapping` |
| A745 | L752: delete statement | `test_delete_exec_mapping` |
| A746 | L754: delete statement | `test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing`, `test_delete_exec_mapping` |
| A747 | L756: delete statement | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| A748 | L760: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A749 | L760: false -> true | `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A750 | L760: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A751 | L763: == -> != | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A752 | L763: == -> != | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A753 | L763: \|\| -> && | `test_api_ops:crash/rc=1` |
| A754 | L763: \|\| -> && | `test_mount_exec_when_the_catalog_changed_after_the_check` |
| A755 | L763: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A756 | L764: const 404+1 -> 405 | `test_mount_exec_when_the_catalog_changed_after_the_check` |
| A757 | L764: const 404-1 -> 403 | `test_mount_exec_when_the_catalog_changed_after_the_check` |
| A758 | L764: delete statement | `test_mount_exec_when_the_catalog_changed_after_the_check` |
| A759 | L769: != -> == | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A760 | L769: != -> == | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A761 | L769: \|\| -> && | `test_mount_of_an_image_without_a_table_is_refused_even_if_the_view_says_ready` |
| A762 | L769: \|\| -> && | `test_mount_of_an_image_without_a_table_is_refused_even_if_the_view_says_ready` |
| A763 | L769: \|\| -> && | `test_mount_exec_requires_a_table_of_the_current_card` |
| A764 | L769: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A765 | L770: const 409+1 -> 410 | `test_mount_exec_requires_a_table_of_the_current_card`, `test_mount_exec_when_the_catalog_changed_after_the_check` |
| A766 | L770: const 409-1 -> 408 | `test_mount_exec_requires_a_table_of_the_current_card`, `test_mount_exec_when_the_catalog_changed_after_the_check` |
| A767 | L770: delete statement | `test_mount_exec_requires_a_table_of_the_current_card`, `test_mount_exec_when_the_catalog_changed_after_the_check` |
| A768 | L772: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A769 | L775: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A770 | L775: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A771 | L776: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A772 | L777: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A773 | L778: delete statement | `test_mount_checks_the_sd_again_inside_the_state_lock` |
| A774 | L779: == -> != | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A775 | L779: negate if condition | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A776 | L780: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A777 | L781: == -> != | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A778 | L781: negate if condition | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A779 | L782: delete statement | `test_mount_that_finds_the_image_already_mounted_reports_it` |
| A780 | L783: == -> != | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A781 | L783: negate if condition | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A782 | L784: const 409+1 -> 410 | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A783 | L784: const 409-1 -> 408 | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A784 | L784: delete statement | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A788 | L790: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A789 | L794: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A790 | L794: true -> false | `test_mount_and_eject_exec_when_the_sd_went_away_after_the_check` |
| A791 | L794: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A792 | L795: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A793 | L797: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A794 | L798: == -> != | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A795 | L798: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A796 | L799: const 200+1 -> 201 | `test_eject_flow_and_target_eject`, `test_eject_is_refused_while_the_target_prevents_removal_unless_forced` |
| A797 | L799: const 200-1 -> 199 | `test_eject_flow_and_target_eject`, `test_eject_is_refused_while_the_target_prevents_removal_unless_forced` |
| A798 | L799: delete statement | `test_eject_flow_and_target_eject`, `test_eject_is_refused_while_the_target_prevents_removal_unless_forced` |
| A799 | L800: == -> != | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A800 | L800: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A801 | L801: const 200+1 -> 201 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A802 | L801: const 200-1 -> 199 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A803 | L801: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A804 | L802: == -> != | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A805 | L802: negate if condition | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A806 | L803: const 409+1 -> 410 | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A807 | L803: const 409-1 -> 408 | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A808 | L803: delete statement | `test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check` |
| A812 | L807: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A813 | L811: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A814 | L811: false -> true | `test_every_changing_operation_rechecks_the_sd_after_taking_the_op_lock`, `test_only_eject_clears_ejected_by_target_when_the_sd_goes_away` |
| A815 | L811: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A818 | L816: delete statement | `test_rescan_imports_and_reports` |
| A819 | L817: delete statement | `test_rescan_imports_and_reports` |
| A820 | L818: < -> <= | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A821 | L818: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A822 | L818: const 0+1 -> 1 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A823 | L819: delete statement | `test_rescan_io_error_is_503_but_keeps_what_was_imported` |
| A824 | L822: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A825 | L823: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A826 | L824: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A827 | L825: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A828 | L826: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A829 | L827: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A830 | L828: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A831 | L829: delete statement | `test_rescan_continues_with_the_cursor` |
| A832 | L831: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A833 | L833: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A834 | L834: const 200+1 -> 201 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A835 | L834: const 200-1 -> 199 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A836 | L834: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete` |
| A837 | L836: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_the_op_lock_wait_can_be_configured` |
| A839 | L846: delete statement | `test_api_order:crash/rc=1` |
| A840 | L848: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A841 | L849: > -> >= | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A842 | L849: negate if condition | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A843 | L850: const 413+1 -> 414 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A844 | L850: const 413-1 -> 412 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A845 | L850: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A846 | L851: false -> true | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks` |
| A848 | L856: == -> != | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A849 | L856: != -> == | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A850 | L856: \|\| -> && | `test_api_order:crash/rc=1` |
| A851 | L856: \|\| -> && | `test_api_order:crash/rc=1` |
| A852 | L856: negate if condition | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A853 | L856: const 0+1 -> 1 | `test_step6_format_errors_are_400_before_the_sd_check` |
| A854 | L856: const 0+1 -> 1 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A855 | L857: const 400+1 -> 401 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A856 | L857: const 400-1 -> 399 | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A857 | L857: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A858 | L858: false -> true | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A859 | L862: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A860 | L864: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A861 | L865: negate if condition | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A862 | L865: false -> true | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A863 | L865: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A864 | L867: true -> false | `test_step13_busy_for_finish_and_put_commit` |
| A865 | L867: delete statement | `test_step13_busy_for_finish_and_put_commit` |
| A866 | L868: + -> - | `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A869 | L868: delete statement | `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A870 | L869: delete statement | `test_step10_offset_mismatch_before_exceeds_size_and_storage` |
| A871 | L870: delete statement | `test_step10_offset_mismatch_before_exceeds_size_and_storage`, `test_step13_busy_for_finish_and_put_commit` |
| A872 | L872: delete statement | `test_api_edges:crash/rc=1` |
| A874 | L873: delete statement | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A875 | L874: true -> false | `test_step10_offset_mismatch_before_exceeds_size_and_storage`, `test_step12_put_resume_storage_boundary` |
| A876 | L878: negate if condition | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_step4_content_type_is_415_before_the_length_checks` |
| A877 | L878: false -> true | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A878 | L878: delete statement | `test_api_ops:crash/rc=1` |
| A879 | L879: delete statement | `test_entry_points_discard_what_the_response_held_before` |
| A880 | L880: \|\| -> && | `test_api_ops:crash/rc=1` |
| A881 | L880: negate if condition | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_step4_content_type_is_415_before_the_length_checks` |
| A882 | L881: const 500+1 -> 501 | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A883 | L881: const 500-1 -> 499 | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A884 | L881: delete statement | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A885 | L882: true -> false | `test_put_precheck_and_commit_argument_errors` |
| A886 | L882: delete statement | `test_put_precheck_and_commit_argument_errors` |
| A887 | L883: false -> true | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A888 | L887: != -> == | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A889 | L887: && -> \|\| | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A890 | L887: negate if condition | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_step4_content_type_is_415_before_the_length_checks` |
| A891 | L888: const 500+1 -> 501 | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A892 | L888: const 500-1 -> 499 | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A893 | L888: delete statement | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A894 | L889: false -> true | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A895 | L889: delete statement | `test_put_precheck_rejects_a_request_that_is_not_a_put` |
| A896 | L891: negate if condition | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_step4_content_type_is_415_before_the_length_checks` |
| A897 | L891: delete statement | `test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A898 | L892: negate if condition | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_step4_content_type_is_415_before_the_length_checks` |
| A899 | L892: true -> false | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_step4_content_type_is_415_before_the_length_checks` |
| A900 | L892: delete statement | `test_step3_an_invalid_id_in_the_path_is_404_before_everything_else`, `test_step4_content_type_is_415_before_the_length_checks` |
| A901 | L897: negate if condition | `test_step13_busy_for_finish_and_put_commit` |
| A902 | L897: delete statement | `test_api_edges:crash/rc=1` |
| A903 | L900: delete statement | `test_entry_points_discard_what_the_response_held_before` |
| A904 | L901: == -> != | `test_step13_busy_for_finish_and_put_commit` |
| A905 | L901: != -> == | `test_step13_busy_for_finish_and_put_commit` |
| A906 | L901: \|\| -> && | `test_commit_rejects_bad_arguments` |
| A907 | L901: \|\| -> && | `test_commit_rejects_bad_arguments` |
| A908 | L901: \|\| -> && | `test_commit_rejects_bad_arguments` |
| A909 | L901: \|\| -> && | `test_commit_rejects_bad_arguments` |
| A910 | L901: negate if condition | `test_step13_busy_for_finish_and_put_commit` |
| A911 | L901: const 0+1 -> 1 | `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A912 | L902: const 400+1 -> 401 | `test_commit_rejects_bad_arguments` |
| A913 | L902: const 400-1 -> 399 | `test_commit_rejects_bad_arguments` |
| A914 | L902: delete statement | `test_commit_rejects_bad_arguments` |
| A915 | L905: negate if condition | `test_step13_busy_for_finish_and_put_commit` |
| A916 | L906: delete statement | `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A917 | L909: negate if condition | `test_step13_busy_for_finish_and_put_commit` |
| A918 | L909: false -> true | `test_commit_rechecks_the_sd_after_taking_the_op_lock` |
| A919 | L909: delete statement | `test_step13_busy_for_finish_and_put_commit` |
| A920 | L912: != -> == | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A921 | L912: && -> \|\| | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A922 | L912: \|\| -> && | `test_api_edges:crash/rc=1` |
| A923 | L912: negate if condition | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A924 | L913: const 503+1 -> 504 | `test_api_edges:crash/rc=1` |
| A925 | L913: const 503-1 -> 502 | `test_api_edges:crash/rc=1` |
| A926 | L913: delete statement | `test_api_edges:crash/rc=1` |
| A927 | L914: delete statement | `test_api_edges:crash/rc=1` |
| A930 | L920: == -> != | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A931 | L920: negate if condition | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A932 | L922: delete statement | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A933 | L923: delete statement | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A934 | L924: delete statement | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A935 | L925: delete statement | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A936 | L926: const 200+1 -> 201 | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A937 | L926: const 200-1 -> 199 | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A938 | L926: delete statement | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A939 | L927: == -> != | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A940 | L927: negate if condition | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A941 | L928: const 413+1 -> 414 | `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A942 | L928: const 413-1 -> 412 | `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A943 | L928: delete statement | `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A944 | L929: == -> != | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A945 | L929: negate if condition | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A946 | L930: const 404+1 -> 405 | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A947 | L930: const 404-1 -> 403 | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A948 | L930: delete statement | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A949 | L931: == -> != | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A950 | L931: negate if condition | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A951 | L932: const 409+1 -> 410 | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A952 | L932: const 409-1 -> 408 | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A953 | L932: delete statement | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A954 | L933: == -> != | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A955 | L933: negate if condition | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A956 | L934: const 409+1 -> 410 | `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A957 | L934: const 409-1 -> 408 | `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A958 | L934: delete statement | `test_commit_upload_in_progress_for_a_resume_when_another_upload_started` |
| A959 | L935: == -> != | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A960 | L935: negate if condition | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck`, `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A961 | L936: const 409+1 -> 410 | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A962 | L936: const 409-1 -> 408 | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A963 | L936: delete statement | `test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck` |
| A964 | L937: == -> != | `test_commit_with_a_fabricated_put_maps_the_remaining_results`, `test_commit_507_when_the_resume_no_longer_fits` |
| A965 | L937: == -> != | `test_commit_507_when_the_resume_no_longer_fits`, `test_commit_write_failures` |
| A966 | L937: \|\| -> && | `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A967 | L937: negate if condition | `test_commit_with_a_fabricated_put_maps_the_remaining_results`, `test_commit_507_when_the_resume_no_longer_fits` |
| A968 | L938: const 400+1 -> 401 | `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A969 | L938: const 400-1 -> 399 | `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A970 | L938: delete statement | `test_commit_with_a_fabricated_put_maps_the_remaining_results` |
| A971 | L939: == -> != | `test_commit_507_when_the_resume_no_longer_fits`, `test_commit_write_failures` |
| A972 | L939: negate if condition | `test_commit_507_when_the_resume_no_longer_fits`, `test_commit_write_failures` |
| A973 | L940: const 507+1 -> 508 | `test_commit_507_when_the_resume_no_longer_fits`, `test_commit_write_failures` |
| A974 | L940: const 507-1 -> 506 | `test_commit_507_when_the_resume_no_longer_fits`, `test_commit_write_failures` |
| A975 | L940: delete statement | `test_commit_507_when_the_resume_no_longer_fits`, `test_commit_write_failures` |
| A976 | L942: delete statement | `test_commit_write_failures` |
| A977 | L944: delete statement | `test_put_flow_two_chunks`, `test_put_resume_of_an_incomplete_image` |
| A978 | L950: negate if condition | `test_not_started_service_reports_no_card` |
| A979 | L950: delete statement | `test_api_ops:crash/rc=1` |
| A980 | L951: delete statement | `test_entry_points_discard_what_the_response_held_before` |
| A981 | L952: \|\| -> && | `test_api_ops:crash/rc=1` |
| A982 | L952: negate if condition | `test_not_started_service_reports_no_card` |
| A983 | L953: const 500+1 -> 501 | `test_put_precheck_and_commit_argument_errors` |
| A984 | L953: const 500-1 -> 499 | `test_put_precheck_and_commit_argument_errors` |
| A985 | L953: delete statement | `test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty` |
| A986 | L957: negate if condition | `test_not_started_service_reports_no_card` |
| A987 | L958: == -> != | `test_unknown_paths_and_methods_are_404_and_405` |
| A988 | L958: negate if condition | `test_unknown_paths_and_methods_are_404_and_405` |
| A989 | L958: true -> false | `test_unknown_paths_and_methods_are_404_and_405` |
| A990 | L958: delete statement | `test_unknown_paths_and_methods_are_404_and_405` |
| A992 | L967: delete statement | `test_not_started_service_reports_no_card` |
| A993 | L969: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_get_and_put_precheck_do_not_take_the_op_lock` |
| A994 | L970: != -> == | `test_not_started_service_reports_no_card` |
| A995 | L970: negate if condition | `test_not_started_service_reports_no_card` |
| A996 | L970: const 0+1 -> 1 | `test_not_started_service_reports_no_card` |
| A997 | L971: const 500+1 -> 501 | `test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty` |
| A998 | L971: const 500-1 -> 499 | `test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty` |
| A999 | L971: delete statement | `test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty` |
| A1000 | L973: const 200+1 -> 201 | `test_not_started_service_reports_no_card` |
| A1001 | L973: const 200-1 -> 199 | `test_not_started_service_reports_no_card` |
| A1002 | L973: delete statement | `test_not_started_service_reports_no_card` |
| A1003 | L978: negate if condition | `test_fuzz_request_sequences` |
| A1004 | L978: true -> false | `test_fuzz_request_sequences` |
| A1005 | L978: delete statement | `test_entry_points_discard_what_the_response_held_before` |
| A1006 | L981: != -> == | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1007 | L981: negate if condition | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1008 | L981: const 0+1 -> 1 | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step9_begin_conflicts_in_the_documented_order` |
| A1009 | L982: const 400+1 -> 401 | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1010 | L982: const 400-1 -> 399 | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1011 | L982: delete statement | `test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1012 | L985: true -> false | `test_begin_accepted` |
| A1013 | L985: delete statement | `test_begin_accepted` |
| A1014 | L986: delete statement | `test_begin_accepted` |
| A1015 | L987: delete statement | `test_api_order:crash/rc=1` |
| A1016 | L989: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step9_begin_conflicts_in_the_documented_order` |
| A1017 | L990: negate if condition | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step9_begin_conflicts_in_the_documented_order` |
| A1018 | L990: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step9_begin_conflicts_in_the_documented_order` |
| A1019 | L991: delete statement | `test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit`, `test_step12_begin_fits_exactly_with_the_reserve` |
| A1021 | L996: != -> == | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1022 | L996: \|\| -> && | `test_step9_mount_already_mounted_wins_over_the_lock` |
| A1023 | L996: negate if condition | `test_step6_format_errors_are_400_before_the_sd_check`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A1024 | L996: const 0+1 -> 1 | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1025 | L997: const 400+1 -> 401 | `test_step6_format_errors_are_400_before_the_sd_check`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A1026 | L997: const 400-1 -> 399 | `test_step6_format_errors_are_400_before_the_sd_check`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A1027 | L997: delete statement | `test_step6_format_errors_are_400_before_the_sd_check`, `test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks` |
| A1028 | L1000: delete statement | `test_mount_flow` |
| A1029 | L1001: delete statement | `test_api_order:crash/rc=1` |
| A1030 | L1003: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1031 | L1004: negate if condition | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1032 | L1004: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1033 | L1005: delete statement | `test_step9_mount_already_mounted_wins_over_the_lock`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A1034 | L1009: != -> == | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1035 | L1009: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1036 | L1009: const 0+1 -> 1 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A1037 | L1010: const 400+1 -> 401 | `test_step6_format_errors_are_400_before_the_sd_check` |
| A1038 | L1010: const 400-1 -> 399 | `test_step6_format_errors_are_400_before_the_sd_check` |
| A1039 | L1010: delete statement | `test_step6_format_errors_are_400_before_the_sd_check` |
| A1040 | L1013: delete statement | `test_api_order:crash/rc=1` |
| A1041 | L1015: && -> \|\| | `test_api_order:crash/rc=1` |
| A1042 | L1015: && -> \|\| | `test_api_order:crash/rc=1` |
| A1043 | L1015: negate if condition | `test_api_order:crash/rc=1` |
| A1044 | L1015: delete statement | `test_eject_while_the_sd_is_unusable_clears_ejected_by_target` |
| A1045 | L1016: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A1046 | L1017: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A1047 | L1017: delete statement | `test_step7_eject_is_the_exception_it_answers_200_not_mounted` |
| A1048 | L1018: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A1049 | L1022: != -> == | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1050 | L1022: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1051 | L1022: const 0+1 -> 1 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A1052 | L1023: const 400+1 -> 401 | `test_step6_format_errors_are_400_before_the_sd_check`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A1053 | L1023: const 400-1 -> 399 | `test_step6_format_errors_are_400_before_the_sd_check`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A1054 | L1023: delete statement | `test_step6_format_errors_are_400_before_the_sd_check`, `test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock` |
| A1055 | L1026: delete statement | `test_api_order:crash/rc=1` |
| A1056 | L1028: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A1057 | L1029: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A1058 | L1029: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step9_rescan_during_an_upload_is_409` |
| A1059 | L1030: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step13_busy_when_the_op_lock_cannot_be_taken` |
| A1060 | L1034: != -> == | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1061 | L1034: negate if condition | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step6_format_errors_are_400_before_the_sd_check` |
| A1062 | L1034: const 0+1 -> 1 | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A1063 | L1035: const 400+1 -> 401 | `test_step6_format_errors_are_400_before_the_sd_check` |
| A1064 | L1035: const 400-1 -> 399 | `test_step6_format_errors_are_400_before_the_sd_check` |
| A1065 | L1035: delete statement | `test_step6_format_errors_are_400_before_the_sd_check` |
| A1066 | L1038: delete statement | `test_api_order:crash/rc=1` |
| A1067 | L1040: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A1068 | L1041: negate if condition | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1069 | L1041: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1070 | L1042: delete statement | `test_step13_busy_for_finish_and_put_commit` |
| A1072 | L1047: delete statement | `test_step9_delete_of_the_mounted_image_is_409`, `test_step9_mounted_is_reported_before_crosslinked` |
| A1073 | L1048: delete statement | `test_api_order:crash/rc=1` |
| A1074 | L1050: delete statement | `test_step4_is_not_applied_to_requests_without_a_body_or_to_delete`, `test_step7_sd_not_ready_is_503_before_not_found` |
| A1075 | L1051: negate if condition | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1076 | L1051: delete statement | `test_step7_sd_not_ready_is_503_before_not_found`, `test_step8_not_found_is_404_before_the_state_conflicts` |
| A1077 | L1052: delete statement | `test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish`, `test_step9_delete_of_the_mounted_image_is_409` |
| J001 | L17: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J002 | L17: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J003 | L17: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J004 | L17: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J005 | L17: \|\| -> && | `test_begin_whitespace_and_key_order`, `test_the_body_is_at_most_512_bytes` |
| J006 | L17: \|\| -> && | `test_begin_whitespace_and_key_order`, `test_finish_body` |
| J007 | L17: \|\| -> && | `test_begin_whitespace_and_key_order`, `test_finish_body` |
| J008 | L20: < -> <= | `test_the_body_is_at_most_512_bytes` |
| J009 | L20: && -> \|\| | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J010 | L20: delete statement | `test_begin_whitespace_and_key_order`, `test_the_body_is_at_most_512_bytes` |
| J011 | L24: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J012 | L24: < -> <= | `test_api_json:crash/rc=1` |
| J013 | L24: && -> \|\| | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J014 | L24: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J015 | L25: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J016 | L26: true -> false | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J017 | L28: false -> true | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J018 | L32: <= -> < | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J019 | L32: >= -> > | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J020 | L32: && -> \|\| | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J021 | L32: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J022 | L32: - -> + | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J023 | L32: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J024 | L33: <= -> < | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J025 | L33: >= -> > | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J026 | L33: && -> \|\| | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J027 | L33: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J028 | L33: - -> + | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J029 | L33: + -> - | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J030 | L33: const 10+1 -> 11 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J031 | L33: const 10-1 -> 9 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J032 | L33: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J033 | L34: <= -> < | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J034 | L34: >= -> > | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J035 | L34: && -> \|\| | `test_begin_sha256_accepts_either_case`, `test_begin_name_surrogate_errors` |
| J036 | L34: negate if condition | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J037 | L34: - -> + | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J038 | L34: + -> - | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J039 | L34: const 10+1 -> 11 | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J040 | L34: const 10-1 -> 9 | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J041 | L34: delete statement | `test_begin_whitespace_and_key_order`, `test_begin_sha256_accepts_either_case` |
| J043 | L35: const 1-1 -> 0 | `test_begin_sha256_accepts_either_case`, `test_begin_name_surrogate_errors` |
| J045 | L40: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J046 | L40: - -> + | `test_api_json:crash/rc=1` |
| J048 | L40: const 4-1 -> 3 | `test_api_json:crash/rc=1` |
| J050 | L40: delete statement | `test_api_json:crash/rc=1` |
| J051 | L41: const 0+1 -> 1 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J052 | L42: < -> <= | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J053 | L42: const 0+1 -> 1 | `test_begin_name_utf8_and_escapes`, `test_begin_name_surrogate_errors` |
| J054 | L42: const 4+1 -> 5 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J055 | L42: const 4-1 -> 3 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J056 | L43: + -> - | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J057 | L44: < -> <= | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J058 | L44: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J059 | L44: const 0+1 -> 1 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J062 | L45: const 4+1 -> 5 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J063 | L45: const 4-1 -> 3 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J064 | L45: delete statement | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J065 | L47: += -> -= | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J066 | L47: const 4+1 -> 5 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J067 | L47: const 4-1 -> 3 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J068 | L47: delete statement | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J069 | L49: true -> false | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J071 | L54: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J072 | L55: delete statement | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J073 | L56: const 1+1 -> 2 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J074 | L56: const 1-1 -> 0 | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J075 | L58: < -> <= | `test_begin_name_utf8_and_escapes` |
| J076 | L58: negate if condition | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J077 | L59: const 6+1 -> 7 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J078 | L59: const 6-1 -> 5 | `test_begin_name_utf8_and_escapes`, `test_begin_name_forbidden_characters_after_decoding` |
| J079 | L59: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J080 | L60: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J081 | L61: const 2+1 -> 3 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J082 | L61: const 2-1 -> 1 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J083 | L63: < -> <= | `test_begin_name_utf8_and_escapes`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J084 | L63: negate if condition | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J085 | L64: const 12+1 -> 13 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J086 | L64: const 12-1 -> 11 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J087 | L64: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J088 | L65: const 6+1 -> 7 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J089 | L65: const 6-1 -> 5 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J090 | L65: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J091 | L66: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J092 | L67: const 3+1 -> 4 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J093 | L67: const 3-1 -> 2 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J094 | L69: const 18+1 -> 19 | `test_begin_name_utf8_and_escapes`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J095 | L69: const 18-1 -> 17 | `test_begin_name_utf8_and_escapes`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J096 | L69: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J097 | L70: const 12+1 -> 13 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J098 | L70: const 12-1 -> 11 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J099 | L70: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J100 | L71: const 6+1 -> 7 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J101 | L71: const 6-1 -> 5 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J102 | L71: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J103 | L72: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J104 | L73: const 4+1 -> 5 | `test_api_json:crash/rc=1` |
| J105 | L73: const 4-1 -> 3 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J106 | L78: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J108 | L78: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J109 | L79: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J110 | L81: >= -> > | `test_api_json:crash/rc=1` |
| J111 | L81: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J113 | L81: delete statement | `test_api_json:crash/rc=1` |
| J114 | L83: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J115 | L83: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J116 | L83: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J117 | L84: < -> <= | `test_begin_name_utf8_and_escapes` |
| J118 | L84: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J121 | L85: != -> == | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J122 | L85: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J123 | L86: >= -> > | `test_api_json:crash/rc=1` |
| J124 | L86: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J126 | L86: delete statement | `test_api_json:crash/rc=1` |
| J127 | L87: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J128 | L90: >= -> > | `test_api_json:crash/rc=1` |
| J129 | L90: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J131 | L90: delete statement | `test_api_json:crash/rc=1` |
| J133 | L103: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J135 | L103: delete statement | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J138 | L104: && -> \|\| | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J139 | L104: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J142 | L105: <= -> < | `test_begin_name_utf8_and_escapes`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J143 | L105: >= -> > | `test_begin_name_utf8_and_escapes`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J144 | L105: && -> \|\| | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J145 | L105: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J146 | L106: != -> == | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J147 | L106: != -> == | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J149 | L106: \|\| -> && | `test_api_json:crash/rc=1` |
| J150 | L106: \|\| -> && | `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J151 | L106: negate if condition | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J152 | L106: - -> + | `test_api_json:crash/rc=1` |
| J153 | L106: + -> - | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J155 | L106: const 2-1 -> 1 | `test_api_json:crash/rc=1` |
| J156 | L106: const 1+1 -> 2 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J157 | L106: const 1-1 -> 0 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J159 | L106: delete statement | `test_api_json:crash/rc=1` |
| J160 | L107: += -> -= | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J161 | L107: const 2+1 -> 3 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J162 | L107: const 2-1 -> 1 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J163 | L107: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J165 | L109: negate if condition | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J167 | L109: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J168 | L110: < -> <= | `test_begin_name_utf8_and_escapes`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J169 | L110: > -> >= | `test_begin_name_utf8_and_escapes`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J170 | L110: \|\| -> && | `test_begin_name_surrogate_errors`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J171 | L110: negate if condition | `test_begin_name_utf8_and_escapes`, `test_begin_name_surrogate_errors` |
| J173 | L110: delete statement | `test_begin_name_surrogate_errors`, `test_a_high_surrogate_needs_an_escaped_low_surrogate_right_after_it` |
| J174 | L111: + -> - | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J176 | L111: + -> - | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J177 | L111: - -> + | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J178 | L111: const 10+1 -> 11 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J179 | L111: const 10-1 -> 9 | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J180 | L111: delete statement | `test_begin_name_utf8_and_escapes`, `test_begin_name_length_limit_is_63_bytes_after_decoding` |
| J181 | L118: > -> >= | `test_begin_name_length_limit_is_63_bytes_after_decoding`, `test_partial_unicode_escapes_are_not_decoded` |
| J182 | L118: negate if condition | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J183 | L118: - -> + | `test_api_json:crash/rc=1` |
| J185 | L118: delete statement | `test_api_json:crash/rc=1` |
| J186 | L119: + -> - | `test_api_json:crash/rc=1` |
| J187 | L119: delete statement | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J188 | L120: += -> -= | `test_api_json:crash/rc=1` |
| J189 | L120: delete statement | `test_begin_sha256_length_and_type`, `test_begin_name_utf8_and_escapes` |
| J190 | L123: true -> false | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J191 | L127: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J192 | L127: > -> >= | `test_api_json:crash/rc=1` |
| J193 | L127: \|\| -> && | `test_api_route:crash/rc=1` |
| J194 | L127: \|\| -> && | `test_rescan_body` |
| J195 | L127: \|\| -> && | `test_rescan_body` |
| J196 | L127: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J197 | L127: const 0+1 -> 1 | `test_rescan_body` |
| J199 | L127: const 20-1 -> 19 | `test_api_json:crash/rc=1` |
| J200 | L127: const 1+1 -> 2 | `test_parse_u64` |
| J201 | L127: const 1-1 -> 0 | `test_rescan_body` |
| J202 | L127: delete statement | `test_rescan_body` |
| J203 | L128: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J204 | L128: > -> >= | `test_rescan_body` |
| J205 | L128: && -> \|\| | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J206 | L128: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J207 | L128: const 1+1 -> 2 | `test_rescan_body` |
| J208 | L128: const 1-1 -> 0 | `test_rescan_body` |
| J209 | L128: const 1+1 -> 2 | `test_parse_u64` |
| J210 | L128: const 1-1 -> 0 | `test_rescan_body` |
| J211 | L128: delete statement | `test_begin_size_must_be_a_plain_integer`, `test_rescan_body` |
| J212 | L129: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J213 | L130: < -> <= | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J214 | L130: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J215 | L131: < -> <= | `test_begin_minimal_and_full`, `test_rescan_body` |
| J216 | L131: > -> >= | `test_begin_size_range` |
| J217 | L131: \|\| -> && | `test_parse_u64` |
| J218 | L131: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J219 | L131: const 1+1 -> 2 | `test_parse_u64` |
| J220 | L131: const 1-1 -> 0 | `test_parse_u64` |
| J221 | L131: delete statement | `test_parse_u64` |
| J222 | L132: - -> + | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J223 | L133: > -> >= | `test_api_json:crash/rc=1` |
| J224 | L133: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J225 | L133: - -> + | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J226 | L133: / -> * | `test_rescan_body` |
| J227 | L133: const 10+1 -> 11 | `test_api_json:crash/rc=1` |
| J228 | L133: const 10-1 -> 9 | `test_rescan_body` |
| J229 | L133: const 1+1 -> 2 | `test_parse_u64` |
| J230 | L133: const 1-1 -> 0 | `test_rescan_body` |
| J231 | L133: delete statement | `test_rescan_body` |
| J232 | L134: * -> / | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J233 | L134: + -> - | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J234 | L134: const 10+1 -> 11 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J235 | L134: const 10-1 -> 9 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J236 | L134: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J237 | L137: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J238 | L142: <= -> < | `test_begin_size_range` |
| J239 | L142: >= -> > | `test_begin_minimal_and_full`, `test_rescan_body` |
| J240 | L142: < -> <= | `test_api_json:crash/rc=1` |
| J241 | L142: && -> \|\| | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J242 | L142: && -> \|\| | `test_api_json:crash/rc=1` |
| J243 | L142: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J244 | L143: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J245 | L143: + -> - | `test_api_json:crash/rc=1` |
| J246 | L143: - -> + | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J247 | L143: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J248 | L148: != -> == | `test_mount_body`, `test_mount_body_errors` |
| J250 | L148: \|\| -> && | `test_api_json:crash/rc=1` |
| J251 | L148: negate if condition | `test_mount_body`, `test_mount_body_errors` |
| J252 | L148: - -> + | `test_api_json:crash/rc=1` |
| J253 | L148: + -> - | `test_api_json:crash/rc=1` |
| J254 | L148: const 0+1 -> 1 | `test_mount_body`, `test_eject_body` |
| J255 | L148: false -> true | `test_mount_body`, `test_eject_body` |
| J256 | L148: delete statement | `test_mount_body`, `test_mount_body_errors` |
| J257 | L149: += -> -= | `test_mount_body`, `test_eject_body` |
| J258 | L149: delete statement | `test_mount_body`, `test_eject_body` |
| J259 | L150: true -> false | `test_mount_body`, `test_eject_body` |
| J260 | L154: negate if condition | `test_mount_body`, `test_eject_body` |
| J261 | L156: true -> false | `test_mount_body`, `test_eject_body` |
| J262 | L158: negate if condition | `test_mount_body`, `test_eject_body` |
| J263 | L160: true -> false | `test_mount_body`, `test_eject_body` |
| J266 | L166: const 1-1 -> 0 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J269 | L167: const 1-1 -> 0 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J273 | L168: const 1-1 -> 0 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J277 | L169: const 1-1 -> 0 | `test_mount_body` |
| J281 | L170: const 1-1 -> 0 | `test_mount_body`, `test_eject_body` |
| J285 | L171: const 1-1 -> 0 | `test_rescan_body` |
| J288 | L175: == -> != | `test_api_json:crash/rc=1` |
| J289 | L175: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J290 | L175: && -> \|\| | `test_api_json:crash/rc=1` |
| J291 | L175: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J292 | L182: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J293 | L182: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J294 | L183: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J295 | L183: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J296 | L184: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J297 | L184: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J298 | L185: const 0+1 -> 1 | `test_begin_structure_errors` |
| J299 | L187: negate if condition | `test_mount_body` |
| J300 | L187: delete statement | `test_mount_body` |
| J301 | L188: negate if condition | `test_mount_body`, `test_unknown_keys_are_rejected_in_every_schema_whatever_the_value` |
| J302 | L188: delete statement | `test_mount_body` |
| J303 | L189: const 0+1 -> 1 | `test_mount_body_errors`, `test_unknown_keys_are_rejected_in_every_schema_whatever_the_value` |
| J304 | L191: negate if condition | `test_eject_body`, `test_unknown_keys_are_rejected_in_every_schema_whatever_the_value` |
| J305 | L191: delete statement | `test_eject_body` |
| J306 | L192: const 0+1 -> 1 | `test_eject_body`, `test_unknown_keys_are_rejected_in_every_schema_whatever_the_value` |
| J307 | L194: negate if condition | `test_rescan_body`, `test_unknown_keys_are_rejected_in_every_schema_whatever_the_value` |
| J308 | L194: delete statement | `test_rescan_body` |
| J309 | L195: const 0+1 -> 1 | `test_unknown_keys_are_rejected_in_every_schema_whatever_the_value` |
| J310 | L203: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J311 | L203: false -> true | `test_fuzz_mutated_bodies` |
| J312 | L203: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J314 | L207: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J315 | L207: false -> true | `test_begin_size_range`, `test_begin_size_must_be_a_plain_integer` |
| J316 | L207: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J317 | L208: <= -> < | `test_begin_size_range` |
| J318 | L208: >= -> > | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J319 | L208: && -> \|\| | `test_begin_size_range` |
| J320 | L210: * -> / | `test_api_json:crash/rc=1` |
| J322 | L212: != -> == | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J324 | L212: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J325 | L212: false -> true | `test_begin_sha256_length_and_type` |
| J326 | L212: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J327 | L213: < -> <= | `test_api_json:crash/rc=1` |
| J328 | L213: const 0+1 -> 1 | `test_begin_sha256_accepts_either_case`, `test_begin_sha256_length_and_type` |
| J329 | L214: * -> / | `test_api_json:crash/rc=1` |
| J330 | L215: * -> / | `test_api_json:crash/rc=1` |
| J331 | L215: + -> - | `test_api_json:crash/rc=1` |
| J332 | L215: const 1+1 -> 2 | `test_api_json:crash/rc=1` |
| J333 | L215: const 1-1 -> 0 | `test_begin_sha256_accepts_either_case`, `test_begin_sha256_length_and_type` |
| J334 | L216: < -> <= | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J335 | L216: < -> <= | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J336 | L216: \|\| -> && | `test_api_json:crash/rc=1` |
| J337 | L216: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J338 | L216: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J339 | L216: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J340 | L216: false -> true | `test_begin_sha256_accepts_either_case` |
| J341 | L216: delete statement | `test_api_json:crash/rc=1` |
| J342 | L217: const 4+1 -> 5 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J343 | L217: const 4-1 -> 3 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J344 | L217: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J345 | L219: true -> false | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J346 | L219: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J347 | L220: true -> false | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J349 | L224: != -> == | `test_mount_body` |
| J351 | L224: negate if condition | `test_mount_body` |
| J352 | L224: false -> true | `test_mount_body_errors` |
| J353 | L224: delete statement | `test_mount_body` |
| J356 | L231: > -> >= | `test_rescan_body` |
| J357 | L231: \|\| -> && | `test_rescan_body` |
| J358 | L231: negate if condition | `test_rescan_body` |
| J359 | L231: const 65535+1 -> 65536 | `test_rescan_body` |
| J360 | L231: const 65535-1 -> 65534 | `test_rescan_body` |
| J361 | L231: false -> true | `test_rescan_body` |
| J362 | L231: delete statement | `test_rescan_body` |
| J363 | L232: delete statement | `test_rescan_body` |
| J364 | L233: true -> false | `test_rescan_body` |
| J365 | L240: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_mount_body` |
| J366 | L240: delete statement | `test_api_json:crash/rc=1` |
| J367 | L241: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J368 | L241: == -> != | `test_begin_structure_errors`, `test_the_body_is_at_most_512_bytes` |
| J369 | L241: == -> != | `test_the_body_is_at_most_512_bytes`, `test_mount_body_errors` |
| J370 | L241: \|\| -> && | `test_begin_structure_errors`, `test_mount_body_errors` |
| J371 | L241: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J372 | L241: const 0+1 -> 1 | `test_the_body_is_at_most_512_bytes`, `test_eject_body` |
| J373 | L241: const 1+1 -> 2 | `test_begin_structure_errors`, `test_mount_body_errors` |
| J374 | L241: const 1-1 -> 0 | `test_begin_structure_errors`, `test_mount_body_errors` |
| J375 | L241: const 0+1 -> 1 | `test_the_body_is_at_most_512_bytes`, `test_eject_body` |
| J376 | L241: delete statement | `test_the_body_is_at_most_512_bytes`, `test_eject_body` |
| J377 | L242: > -> >= | `test_the_body_is_at_most_512_bytes` |
| J378 | L242: \|\| -> && | `test_the_body_is_at_most_512_bytes` |
| J379 | L242: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J380 | L242: const 1+1 -> 2 | `test_the_body_is_at_most_512_bytes` |
| J381 | L242: const 1-1 -> 0 | `test_the_body_is_at_most_512_bytes` |
| J382 | L242: delete statement | `test_the_body_is_at_most_512_bytes` |
| J383 | L244: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J384 | L245: delete statement | `test_begin_whitespace_and_key_order`, `test_mount_body` |
| J385 | L246: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J386 | L246: const 1+1 -> 2 | `test_begin_structure_errors`, `test_mount_body_errors` |
| J387 | L246: const 1-1 -> 0 | `test_begin_structure_errors`, `test_mount_body_errors` |
| J388 | L246: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J389 | L247: delete statement | `test_eject_body`, `test_finish_body` |
| J390 | L248: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J391 | L249: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J392 | L251: delete statement | `test_begin_whitespace_and_key_order`, `test_mount_body` |
| J394 | L254: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J395 | L254: const 1+1 -> 2 | `test_begin_size_must_be_a_plain_integer`, `test_begin_structure_errors` |
| J396 | L254: const 1-1 -> 0 | `test_begin_size_must_be_a_plain_integer`, `test_begin_structure_errors` |
| J397 | L254: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J398 | L256: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J399 | L256: != -> == | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J400 | L256: \|\| -> && | `test_begin_structure_errors`, `test_mount_body_errors` |
| J401 | L256: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J402 | L256: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J403 | L256: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J404 | L256: const 1+1 -> 2 | `test_begin_structure_errors`, `test_mount_body_errors` |
| J405 | L256: const 1-1 -> 0 | `test_begin_structure_errors`, `test_mount_body_errors` |
| J406 | L256: delete statement | `test_begin_structure_errors`, `test_mount_body_errors` |
| J407 | L258: delete statement | `test_begin_whitespace_and_key_order`, `test_mount_body` |
| J408 | L259: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J409 | L259: const 1+1 -> 2 | `test_begin_structure_errors` |
| J410 | L259: const 1-1 -> 0 | `test_begin_structure_errors` |
| J411 | L259: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J412 | L260: delete statement | `test_begin_whitespace_and_key_order`, `test_mount_body` |
| J413 | L261: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J414 | L261: const 1+1 -> 2 | `test_begin_sha256_accepts_either_case`, `test_begin_sha256_length_and_type` |
| J415 | L261: const 1-1 -> 0 | `test_begin_sha256_accepts_either_case`, `test_begin_sha256_length_and_type` |
| J416 | L261: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J417 | L262: delete statement | `test_begin_whitespace_and_key_order`, `test_mount_body` |
| J418 | L263: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J419 | L263: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J420 | L264: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J421 | L264: delete statement | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J422 | L265: const 1+1 -> 2 | `test_begin_name_escape_syntax_errors`, `test_begin_size_must_be_a_plain_integer` |
| J423 | L265: const 1-1 -> 0 | `test_begin_name_escape_syntax_errors`, `test_begin_size_must_be_a_plain_integer` |
| J424 | L268: delete statement | `test_begin_whitespace_and_key_order`, `test_the_body_is_at_most_512_bytes` |
| J425 | L269: != -> == | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J426 | L269: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J427 | L269: const 1+1 -> 2 | `test_begin_structure_errors`, `test_begin_a_nul_byte_after_the_object_is_rejected` |
| J428 | L269: const 1-1 -> 0 | `test_begin_structure_errors`, `test_begin_a_nul_byte_after_the_object_is_rejected` |
| J429 | L269: delete statement | `test_begin_structure_errors`, `test_begin_a_nul_byte_after_the_object_is_rejected` |
| J430 | L271: == -> != | `test_begin_structure_errors`, `test_the_body_is_at_most_512_bytes` |
| J431 | L271: != -> == | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J432 | L271: && -> \|\| | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J433 | L271: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J434 | L271: const 1+1 -> 2 | `test_begin_structure_errors` |
| J435 | L271: const 1-1 -> 0 | `test_begin_structure_errors` |
| J436 | L271: delete statement | `test_begin_structure_errors` |
| J437 | L272: == -> != | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J438 | L272: == -> != | `test_mount_body`, `test_mount_body_errors` |
| J439 | L272: && -> \|\| | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J440 | L272: negate if condition | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| J442 | L272: const 1+1 -> 2 | `test_mount_body_errors` |
| J443 | L272: const 1-1 -> 0 | `test_mount_body_errors` |
| J444 | L272: delete statement | `test_mount_body_errors` |
| J445 | L273: const 0+1 -> 1 | `test_begin_minimal_and_full`, `test_begin_whitespace_and_key_order` |
| M001 | L8: delete statement | `test_api_list:crash/rc=1` |
| M002 | L9: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M003 | L10: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M004 | L10: delete statement | `test_writer_primitives` |
| M005 | L11: const 0+1 -> 1 | `test_writer_edges` |
| M006 | L11: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M007 | L12: negate if condition | `test_api_list:crash/rc=1` |
| M008 | L12: delete statement | `test_writer_edges` |
| M009 | L16: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M010 | L16: delete statement | `test_api_edges:crash/rc=1` |
| M011 | L17: >= -> > | `test_api_list:crash/rc=1` |
| M012 | L17: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M013 | L17: - -> + | `test_api_list:crash/rc=1` |
| M014 | L18: false -> true | `test_api_list:crash/rc=1` |
| M015 | L18: delete statement | `test_api_list:crash/rc=1` |
| M016 | L21: + -> - | `test_api_list:crash/rc=1` |
| M017 | L21: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M018 | L22: += -> -= | `test_api_list:crash/rc=1` |
| M019 | L22: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M020 | L23: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M021 | L30: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M022 | L32: + -> - | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M023 | L32: const 10+1 -> 11 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M024 | L32: const 10-1 -> 9 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M025 | L32: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M026 | L33: const 10+1 -> 11 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M027 | L33: const 10-1 -> 9 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M028 | L34: != -> == | `test_api_list:crash/rc=1` |
| M029 | L34: const 0+1 -> 1 | `test_api_list:crash/rc=1` |
| M030 | L36: < -> <= | `test_api_list:crash/rc=1` |
| M031 | L36: - -> + | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M032 | L36: - -> + | `test_api_list:crash/rc=1` |
| M033 | L36: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M034 | L36: const 1+1 -> 2 | `test_api_list:crash/rc=1` |
| M035 | L36: const 1-1 -> 0 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M036 | L37: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M037 | L44: const 1+1 -> 2 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M038 | L44: const 1-1 -> 0 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M039 | L44: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M040 | L45: < -> <= | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M041 | L45: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M042 | L47: == -> != | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M043 | L47: == -> != | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M044 | L47: \|\| -> && | `test_names_are_json_escaped`, `test_mounted_target_flags` |
| M045 | L47: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M046 | L49: const 2+1 -> 3 | `test_api_list:crash/rc=1` |
| M047 | L49: const 2-1 -> 1 | `test_names_are_json_escaped`, `test_mounted_target_flags` |
| M048 | L49: delete statement | `test_names_are_json_escaped`, `test_mounted_target_flags` |
| M049 | L50: < -> <= | `test_names_are_json_escaped`, `test_mounted_target_flags` |
| M050 | L50: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M051 | L51: const 4+1 -> 5 | `test_names_are_json_escaped`, `test_writer_primitives` |
| M052 | L51: const 4-1 -> 3 | `test_names_are_json_escaped`, `test_writer_primitives` |
| M053 | L52: const 6+1 -> 7 | `test_api_list:crash/rc=1` |
| M054 | L52: const 6-1 -> 5 | `test_names_are_json_escaped`, `test_writer_primitives` |
| M055 | L52: delete statement | `test_names_are_json_escaped`, `test_writer_primitives` |
| M056 | L54: const 1+1 -> 2 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M057 | L54: const 1-1 -> 0 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M058 | L54: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M059 | L57: const 1+1 -> 2 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M060 | L57: const 1-1 -> 0 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M061 | L57: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M062 | L62: < -> <= | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M063 | L62: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M064 | L63: const 4+1 -> 5 | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M065 | L63: const 4-1 -> 3 | `test_api_list:crash/rc=1` |
| M066 | L64: const 2+1 -> 3 | `test_api_list:crash/rc=1` |
| M067 | L64: const 2-1 -> 1 | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M068 | L64: delete statement | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M069 | L69: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M070 | L70: > -> >= | `test_writer_edges` |
| M071 | L70: && -> \|\| | `test_api_list:crash/rc=1` |
| M072 | L70: negate if condition | `test_api_list:crash/rc=1` |
| M074 | L70: delete statement | `test_api_list:crash/rc=1` |
| M075 | L71: negate if condition | `test_api_list:crash/rc=1` |
| M076 | L71: const 0+1 -> 1 | `test_api_list:crash/rc=1` |
| M077 | L71: delete statement | `test_api_list:crash/rc=1` |
| M078 | L72: const 1+1 -> 2 | `test_api_list:crash/rc=1` |
| M079 | L72: const 1-1 -> 0 | `test_api_list:crash/rc=1` |
| M080 | L74: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M081 | L74: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M082 | L75: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M083 | L110: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M084 | L111: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M085 | L112: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M086 | L113: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M087 | L114: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M088 | L116: delete statement | `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M089 | L118: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M090 | L119: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M091 | L120: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M092 | L122: delete statement | `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M093 | L124: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M094 | L125: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M095 | L126: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M096 | L127: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M097 | L128: delete statement | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M098 | L129: delete statement | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M099 | L130: delete statement | `test_the_example_of_the_spec`, `test_the_worst_case_fits_in_the_response_buffer` |
| M100 | L132: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M101 | L134: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M102 | L135: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M103 | L136: delete statement | `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M104 | L137: == -> != | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M105 | L137: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M106 | L138: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M107 | L140: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M108 | L142: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M109 | L144: == -> != | `test_every_image_state_and_invalid_reason`, `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M110 | L144: negate if condition | `test_every_image_state_and_invalid_reason`, `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M111 | L154: negate if condition | `test_api_list:crash/rc=1` |
| M112 | L155: delete statement | `test_every_image_state_and_invalid_reason`, `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M113 | L156: delete statement | `test_every_image_state_and_invalid_reason`, `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M114 | L157: delete statement | `test_every_image_state_and_invalid_reason`, `test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes` |
| M115 | L159: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M116 | L161: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M117 | L166: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M118 | L167: \|\| -> && | `test_api_list:crash/rc=1` |
| M119 | L167: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M120 | L167: false -> true | `test_api_list:crash/rc=1` |
| M121 | L167: delete statement | `test_api_list:crash/rc=1` |
| M122 | L168: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M123 | L168: delete statement | `test_api_list:crash/rc=1` |
| M124 | L173: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M125 | L174: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M126 | L175: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M127 | L176: && -> \|\| | `test_sd_states_other_than_ready`, `test_an_unknown_sd_state_is_reported_as_error` |
| M128 | L176: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M129 | L177: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M130 | L179: delete statement | `test_sd_states_other_than_ready`, `test_an_unknown_sd_state_is_reported_as_error` |
| M131 | L181: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M132 | L182: && -> \|\| | `test_sd_states_other_than_ready`, `test_an_unknown_sd_state_is_reported_as_error` |
| M133 | L182: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M134 | L183: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M135 | L185: delete statement | `test_sd_states_other_than_ready`, `test_an_unknown_sd_state_is_reported_as_error` |
| M136 | L187: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M137 | L188: negate if condition | `test_the_example_of_the_spec`, `test_sd_states_other_than_ready` |
| M138 | L189: true -> false | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M139 | L190: < -> <= | `test_api_list:crash/rc=1` |
| M140 | L190: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M141 | L191: == -> != | `test_api_list:crash/rc=1` |
| M142 | L191: negate if condition | `test_api_list:crash/rc=1` |
| M143 | L191: delete statement | `test_api_list:crash/rc=1` |
| M144 | L192: negate if condition | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M145 | L192: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M146 | L193: false -> true | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M147 | L193: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M148 | L194: delete statement | `test_the_example_of_the_spec`, `test_every_image_state_and_invalid_reason` |
| M149 | L197: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M150 | L198: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M151 | L199: + -> - | `test_api_list:crash/rc=1` |
| M153 | L199: const 1-1 -> 0 | `test_api_list:crash/rc=1` |
| M154 | L200: delete statement | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M155 | L202: < -> <= | `test_api_list:crash/rc=1` |
| M156 | L202: && -> \|\| | `test_api_list:crash/rc=1` |
| M157 | L202: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M158 | L203: == -> != | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M159 | L203: negate if condition | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M160 | L203: const 0+1 -> 1 | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M161 | L203: delete statement | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M162 | L205: delete statement | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M163 | L206: delete statement | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M164 | L207: delete statement | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M165 | L208: && -> \|\| | `test_api_list:crash/rc=1` |
| M166 | L208: negate if condition | `test_api_list:crash/rc=1` |
| M167 | L209: delete statement | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M168 | L211: delete statement | `test_mounted_image_missing_from_the_view_is_named_by_its_id` |
| M169 | L213: delete statement | `test_the_example_of_the_spec`, `test_mounted_target_flags` |
| M170 | L215: delete statement | `test_an_empty_ready_card`, `test_sd_states_other_than_ready` |
| M171 | L217: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M172 | L218: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M173 | L219: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M174 | L220: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M175 | L221: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M176 | L222: != -> == | `test_chunk_size_and_hidden_images`, `test_the_worst_case_fits_in_the_response_buffer` |
| M177 | L222: * -> / | `test_chunk_size_and_hidden_images`, `test_not_started_service_reports_no_card` |
| M178 | L222: const 0+1 -> 1 | `test_chunk_size_and_hidden_images`, `test_not_started_service_reports_no_card` |
| M179 | L222: const 1024+1 -> 1025 | `test_chunk_size_and_hidden_images`, `test_not_started_service_reports_no_card` |
| M180 | L222: const 1024-1 -> 1023 | `test_chunk_size_and_hidden_images`, `test_not_started_service_reports_no_card` |
| M181 | L222: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M182 | L223: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M183 | L224: const 0+1 -> 1 | `test_sd_states_other_than_ready`, `test_an_unknown_sd_state_is_reported_as_error` |
| M184 | L224: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M185 | L225: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M186 | L226: && -> \|\| | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M187 | L226: negate if condition | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M188 | L227: delete statement | `test_upload_slot_states`, `test_the_worst_case_fits_in_the_response_buffer` |
| M189 | L228: delete statement | `test_upload_slot_states`, `test_the_worst_case_fits_in_the_response_buffer` |
| M190 | L229: delete statement | `test_upload_slot_states`, `test_the_worst_case_fits_in_the_response_buffer` |
| M191 | L230: delete statement | `test_upload_slot_states`, `test_the_worst_case_fits_in_the_response_buffer` |
| M192 | L231: delete statement | `test_upload_slot_states`, `test_the_worst_case_fits_in_the_response_buffer` |
| M193 | L233: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |
| M194 | L235: delete statement | `test_the_example_of_the_spec`, `test_an_empty_ready_card` |

## 4. 設計に書かれていないこと、設計と違うことを、実装で決めた箇所

`docs/ISO_MOUNT_DESIGN.md` §12 に、まとめて書いた(レビューで確認してほしい)。要点: 段 1・2 は `iso_api` の外(`http_guard_decide`)、405 は 404(`<id>` の形式不正)より先、`finish` の 200 / 422 は公開ビューだけで決める(ロックなし)、`ready` で表がないときの `finish` は 202 `finalizing`、`sha256: null` は 400、`X-Upload-Offset` は先頭ゼロ不可、公開ビューのコピーと `consumed_since`、処理の側の再検査(`sd.state`、`epoch`)、エラーの対応(`busy` / `sd_unavailable` / 500)、`web_server` の結線(IDF で未確認)。
