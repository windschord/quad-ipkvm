# iso_upload のテストとミューテーションテストの結果

作成: 2026-11-19(Issue #139、C-10 の後半)。`components/iso_upload`(アップロードの受信の状態機械: `begin`、`iso_upload_write_chunk`、アイドル、中止、`finish` の受付、
`FINISH` ジョブのステップ(`iso_upload_hash_step`、署名、宣言値との比較)、`INDEX` のステップ(`iso_index_step`)。純 C)のホスト単体テストの構成と、
`iso_upload.c` / `iso_finish.c` / `iso_index.c` に対するミューテーションテストの結果。契約は `docs/ISO_MOUNT_DESIGN.md` §3.1、§3.4、§4.2、§6、§9、`docs/WEB_API.md` §8、`docs/INTERFACES.md` §7。
ワーカーのジョブの契約(キュー、優先順位、`DELETE`、`iso_upload_tick`、SD の再初期化)は `docs/ISO_CORE_TESTS.md`。

## 1. 構成と、公開する関数

| ファイル | 内容 |
|---|---|
| `components/iso_upload/iso_upload.c` | `iso_upload_init`、`iso_upload_begin`(検査の順序、空き容量の式)、`iso_upload_write_chunk`(パーツの境界での分割、`sync`、SHA-256 の逐次、書き込み失敗の `truncate`)、`iso_upload_idle_close`、`iso_upload_abort`、`iso_upload_finish_begin` / `iso_upload_finish_undo`、`iso_upload_reload_received`、`iso_upload_reinit`、空き容量のキャッシュ、`iso_upload_activity*` |
| `components/iso_upload/iso_finish.c` | `iso_upload_finish_step`(`FINISH` ジョブ。`sync` + `close` → SHA-256 → 宣言値 → 署名 → エクステント → meta を ready)、`iso_upload_hash_step`(全パーツを読み直して SHA-256) |
| `components/iso_upload/iso_index.c` | `iso_index_step`(パーツごとの `stat` + `get_extents` → 期待値(meta)との照合 → `iso_extent_table_build`。`FINISH` と `INDEX` ジョブが共有) |
| `components/iso_upload/include/iso_upload.h` | 公開ヘッダー(結果の列挙 `iso_res_t`、フック、ジョブの型) |
| `components/iso_seams/include/iso_seams.h` | **`iso_fs_t.read_at` を足した**(位置指定の読み出し。`close` が読み取りハンドルにも使えることをコメントに明記)。ほかは変更なし |

戻り値の規則(`iso_catalog_apply` と同じ): 0 = 受理、正 = `iso_res_t`(拒否・結果。API のステータスの元)、負 = `iso_err_t`(I/O などのエラー)。

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_upload/test_up_sha256.c` | テスト用 SHA-256 の参照実装(FIPS 180-4 から書いた)の検査: NIST の `"abc"` / 空 / 448 ビット / 100 万個の `a`、パディングの境界(55〜128 バイト。Python の `hashlib` で別に求めた定数)、どこで分けて `update` しても同じ、生成データの署名の位置 | 9 |
| `test/host/iso_upload/test_up_begin.c` | `begin`: 検査の順序(400 → 枠 → 同名 → 上限 → 空き容量)、`size` の境界(34815 / 34816、16 GiB と +1)、空き容量の式の境界(`size + 64 MiB`、`total_bytes` 超、設計の数値例 `10 − 5 − (6 − 5) = 4 GiB`、途中のイメージの数え方)、キャッシュ未採取、`mkdir` / `meta` の失敗の巻き戻し | 18 |
| `test/host/iso_upload/test_up_write.c` | `iso_upload_write_chunk`: `offset == received` のときだけ書く(不一致は何も書かず、現在の `received`)、チャンクの上限(1000 / 1001、既定 128 KiB)、`size` を超える(34816 / +1)、状態ごとの拒否、**パーツの境界**(またぐチャンク、パーツ最後の 1 バイト、次のパーツの先頭、3 パーツにまたがる 1 チャンク、**既定の 1 GiB の境界**)、`sync` の間隔(`>=`、リセット、パーツの終わり、既定 4 MiB)、SHA-256 の逐次、アイドル(`IDLE_CLOSE`)、中止、再開(`meta` を書かない、`open_a` / `open_w`、`received` の決め直し、空き容量の再検査)、`receiving` / `idle` の境(5 秒) | 42 |
| `test/host/iso_upload/test_up_failures.c` | 書き込み・`sync`・`open` の失敗: チャンク全体を `truncate` で取り消して `incomplete`(部分書き込みつきの `ISO_ERR_NO_SPACE` は 507 相当)、パーツをまたぐチャンクの両方の取り消し、`truncate` が失敗したら `received` は実ファイルの大きさ(それも読めなければ書く前の値のまま、再開で決め直す)、`meta` の書き込みの失敗でもメモリ上は `incomplete`、再開の最初の `PUT` の失敗、`idle_close` の失敗 | 23 |
| `test/host/iso_upload/test_up_finish.c` | `finish` の受付(`size_mismatch`、枠、冪等、`invalid`、`finalizing`、取り消し)と、`FINISH` ジョブのステップ: 逐次の SHA-256 / 再起動後の全パーツの読み直し(`budget` ごと、読み出しの回数を手で数えた期待値)、宣言値の一致・不一致、署名(`CD001` / `BEA01` / 不正 / 後ろのパーツ)、エクステント(4096 本ちょうど、4100 本、作業領域の枯渇、`get_extents` の `TOO_FRAGMENTED`)、`part_mismatch`(欠落、短い、長い、`part_count` の食い違い)、I/O エラー(`incomplete` と再試行)、確保の失敗、`meta` の失敗、中止、古いジョブ、`epoch` の記録 | 72 |
| `test/host/iso_upload/test_up_index.c` | `iso_index_step`: 1 パーツずつ + 表の構築(ステップ数 = 初期化 + パーツ + 構築)、`iso_extent_map` で変換を確かめる、孤児の末尾のクリップ、`epoch`、`part_mismatch` / `too_fragmented` / I/O エラー / 確保の失敗 | 26 |
| `test/host/iso_upload/test_up_powercut.c` | **電源断の網羅**(下記) | 4 |
| `test/host/iso_upload/test_up_fuzz.c` | **固定シードのファズ**(下記) | 1 |
| `test/host/iso_catalog/iso_mock_fs.c` / `.h` | モックの `iso_fs_t` に、`read_at`、`open_for_write` / `write` / `sync` / `truncate` / `close`(未同期の分は電源断で失われる、書き込み中のファイルは読み取りで開けない)、`get_extents`、`mount_info` の設定、書き込みの失敗の注入(`mock_fs_fail_write`)、即時の電源断(`mock_fs_crash`)を足した(既存の関数は変えていない) | - |
| `test/host/iso_upload/up_common.h`、`up_sha256.c` / `.h` | 共通の部品(偽の時計、確保の数え上げ、決定的なデータの生成 `gen_byte`、ディスク上の中身の照合、SHA-256) | - |

合計 195 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルが 8 個増える)。

**期待値はすべて手書き**。SHA-256 の定数は Python の `hashlib` で、同じ生成規則(`gen_byte`)から別に求めた。書き込みの総数(28)は、手順から手で数えた(`test_up_powercut.c`)。パーツの大きさは、テストでは 17 ブロック(34816 バイト)など小さくして、境界を安く試す
(`begin` の後に `iso_catalog_write_meta` で `part_blocks` を書き直す。`iso_upload` は、常に `meta.part_blocks` を使う)。1 GiB の境界は、大きさだけを持つ(中身を捨てる)モードと、何もしない SHA-256 で試す。

### 電源断の網羅(`test_up_powercut.c`)

アップロードの全工程(`begin` → 書き直し → チャンク 6 つ → アイドル → 再開 → チャンク 4 つ → `finish`)の、書き込み(28 回: `mkdir`、`meta`、`open`、`write`、`sync`、`close` の同期)の
**1 つ 1 つで電源を落とし**、落とし方は `NONE`(何も起きない)/ `HALF`(`meta` が前半だけ)/ `FULL`(完了していて戻り値だけエラー)の 3 通り。**未同期の分は、電源断で失われる**(モックは、ディレクトリ上の大きさと、メモリ上の大きさを別に持つ)。
`sync` の間隔は 3 通り(12000 バイト、既定の 4 MiB、毎回)で、28 × 3 × 3 = 252 通りを、間隔ごとに掃引する。再起動(スキャン)して、次を確かめる:

- イメージは、ない / `meta_corrupt`(削除できる)/ `incomplete` / ready(`finish` の最後の `meta` が `FULL` で済んでいた場合。`INDEX` で表が作れる)のどれか。`uploading` / `finalizing` は残らない。
- `incomplete` の `received` は、**落ちる前に同期が済んだことが保証されるバイト数(テストの中で、設計 3.1 の規則から別に数える)以上、受け取ったバイト数 + 実行中のチャンク以下**。
- ディスク上のパーツの中身は、`received` までソースの先頭と一致する(穴、ずれ、別のチャンクの混入がない)。
- `received` から再開して送り切り(SHA-256 の状態は失われているので、`finish` は読み直す)、`ready` になり、`meta` の SHA-256 がソースから別に計算した値と一致する。
- 掃引が、すべての場合に当たっている(イメージなし / `meta_corrupt` / `incomplete` / `ready`、未同期の分が失われた場合)ことを、件数で確かめる。

### ファズ(`test_up_fuzz.c`。固定シード、ASan / UBSan)

100 シード × 120 操作(`begin`、正しい / 間違った `offset` の `PUT`、アイドル、`finish`、削除(遷移 → 中止 → 削除のステップ)、書き込みの失敗の注入(`write` の n 回目が k バイトで失敗)、エラーの注入(n 番目の書き込み)、電源断の予約、即時の電源断、再起動)。
パーツの大きさ(17 / 18 / 25 ブロック / 既定)、チャンクの大きさ、`sync` の間隔、署名の良し悪し、宣言した SHA-256(なし / 正しい / 1 ビット違い)は、シードごとに変える。
**操作のたびに、ディスク上のバイト列を、実装を使わずに読んだ基準**と照合する: パーツの中身 = ソースの先頭、連鎖の構造(空でないパーツの前は満杯)、同期済みの大きさ <= メモリ上の大きさ、`uploading` の `received` = メモリ上の連鎖の長さ、
`incomplete` の `received` <= 連鎖の長さ(取り消せなかったときの古い値は小さい側)、`ready` の SHA-256 = ソースから別に計算した値、`ready` の `meta` との一致、枠は 1 つ以下、ハンドルの数 = 枠の持ち主の 1 つ。
`finish` の結果は予測する(宣言値が違えば `checksum_mismatch`、署名が悪ければ `not_iso`、それ以外は `ready`)。実行時に集計を表示する(固定シードなので毎回同じ)。

## 3. ミューテーションテスト

方法: `tools/test/mutate_iso_upload.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_upload.c` / `iso_finish.c` / `iso_index.c` の 1 か所を置換 → 再ビルド → 7 個のテストの実行ファイルを順に実行、を繰り返す
(`tools/test/mutate_iso_extent.py` の共通部分。`-jN` 対応)。ミュータントの定義(`tools/test/iso_upload_mutants.py`)は、**`tools/test/gen_iso_mutants.py` がソースから機械的に作る**(関係演算子の置換、`&&` / `||` の置換、`if` 条件の否定、
算術演算子の置換、文の削除、定数の ±1、真偽値の反転)。ソースを変えたら、作り直す(`python3 tools/test/gen_iso_mutants.py upload . > tools/test/iso_upload_mutants.py`)。
1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。

```
python3 tools/test/mutate_iso_upload.py <repo> <スクラッチ> tools/test/iso_upload_mutants.py <結果.json> -j4
```

経緯: 最初の実行(785 個)は、殺した 578、生き残り 124、ビルドエラー 83(括弧の省略などの `-Werror`)だった。生き残りを 1 つずつ調べ、**同値でないものには、テストを足した**(`notify` の回数、`close` だけが失敗する場合、`sync` と `close` の両方の失敗、`received` の決め直し、新しいアップロードでの SHA-256 の初期化、`read_at` の境界、`hash_step` の単体、古いジョブの判定の各条件、`reinit` の後始末、16 パーツ、`init` の必須の欄、ジョブの再実行が I/O をしないこと、など)。
**実装も単純にした**: 使われない欄(`f_part`)、重複していた `since_sync` のリセット、`touched` の重複の検査、`UPLOADING` で `f` が閉じているときの不要な `reload`、`hash_step` の冗長な初期化、使われない引数(`hash_fail` の理由 / エラー)、`received_before` の代入(`received` は成功したときだけ進める)を取り除いた。
ミュータントのコンパイラの警告(括弧の省略、未使用の変数など)は、ミュータントごとの `#pragma` で無効にして、**ビルドエラーを、コンパイラが範囲外などの誤りとして検出したものだけにした**。最後に、ソースから作り直した 740 個を、**最初から全数**実行した。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー(コンパイラが検出) |
|---|---|---|---|
| 740 | 667 | 65 | 8 |

### 3.1 同値のミュータント(65 個)

生き残ったものは、1 つずつ、変更の前後の挙動が同じ理由を確かめた。同じ理由のものは、まとめた。

| ID | 同値の理由 |
|---|---|
| U029, U030, U031, U034, U035, U036 | `iso_up_release_slot` / `iso_up_discard_sha` が、読まれない欄を片づけるだけ: `slot_finalizing`、`slot_id`、`sha_id`、`sha_bytes` は、`slot_active` / `sha_valid` が偽なら、どこからも読まれない(`take_slot` / `begin` が、次に使うときに書き直す) |
| U042, U142, F008, X008 | id のコピーの末尾の NUL(`ISO_ID_LEN + 1` → `+ 0`): コピー先は、0 で初期化済みで、9 バイト目に 0 以外を書くことがない |
| F010, X010, X053, X055 | ジョブの初期化で、`memset` 済み(0)の欄を、もう一度 0 にする(`FP_START` と `IX_INIT` は 0、`pool_used` と `part` も 0) |
| U055, U056, U062, U063 | `iso_up_force_incomplete`: `iso_catalog_apply` が成功(0)したときは、状態はすでに `incomplete` で、後ろの分岐も `ISO_OK` を返す。`rc == 0` の扱いを変えても、結果は同じ |
| U070, U071 | `iso_upload_refresh_free` の `mount_info` の出力の `memset`: `mount_info` は、すべての欄を埋める(契約)。失敗したときは、読まない |
| U078, U089, U421 | カタログの走査で `<=` / `received <= size`: `images[count]` は 0 で埋まっていて(`meta_valid` が偽・状態なし)、読み飛ばされる。`received == size` の差分は 0 |
| U096, U098 | 空き容量の推定: キャッシュ未採取なら `free_bytes` は 0 なので、早期の return がなくても 0。`free_bytes > used` と `>=` は、等しいとき両方 0 |
| U104, U105, U106 | 関数の定義の配列引数の大きさ(`char out_id[ISO_ID_LEN + 1]` の大きさ)は、ポインターに読み替えられ、意味を持たない |
| U166, X073, F048, F083, F174 | 初期値が、直後の呼び出しで必ず上書きされる(`r`、`n`、`got`、`ok`)。失敗の戻り値のときは読まない |
| U175, F026, F077, X062, U267, U382, U391, U392 | 起きない失敗の分岐: パスの組み立て(`ISO_PATH_MAX` に収まる)、`iso_catalog_apply(PUT / FINISH)` の拒否(直前の検査で除かれている。FINISH と PUT は `meta` を書かない) |
| U278, F040, F043, U403, U405 | 境界で、両方の分岐が同じ値になる: `seg > len - done` と `>=`(等しければ代入しても同じ)、読み出しの長さの `>` と `>=`、経過の `now >= last` と `>`(等しければ 0)、巻き戻りの経過が 0 か 1(どちらも `receiving`) |
| U334, U335, U338 | `iso_upload_idle_close` の枠の検査: 「`uploading` のイメージは、必ず枠の持ち主」という不変条件があるので、後ろの `e->state != ISO_STATE_UPLOADING` の検査だけで同じ結果(防御として残している) |
| F112, F115, F117, F118, F122, F124, F125, F126, F131, F148, F203, F193, F196 | `end_invalid` / `end_ioerr` の冗長な後始末: 作業領域は、その前に解放済み、SHA-256 の状態は検証の前に捨てている、書き込み用のハンドルは、先頭の `close` で閉じ済み、`phase = FP_DONE` がなくても、状態が `finalizing` でなくなるので、次のステップは古いジョブとして終わる、`rc` の分岐は `rc == 0` でも同じ |
| F165, F166 | `FP_VERIFY` で `have_digest` を立てても、その後は読まれない |
| X040 | `n == 0 \|\| n > ISO_MAX_PARTS` は、`n != part_count`(meta の検査で 1〜16)の検査に含まれる(配列の範囲の防御として残している) |

### 3.2 ビルドエラー(8 個。コンパイラが検出するので、殺したものとして数えない)

`-Werror` のコンパイル(配列の範囲外の読み出しや、使われなくなった変数など)で、ビルドできない変更。ミュータント側で、括弧の省略や未使用の変数の警告は無効にしてあるので、残ったものは、コンパイラが範囲外などの誤りとして検出したもの。

| ID | 変更箇所 |
|---|---|
| U141 | L120: const 1+1 -> 2 |
| U212 | L193: delete statement |
| F096 | L112: + -> - |
| F097 | L112: + -> - |
| F098 | L112: const 1+1 -> 2 |
| F100 | L112: const 5+1 -> 6 |
| F103 | L112: const 1+1 -> 2 |
| F105 | L112: const 5+1 -> 6 |

### 3.3 殺したミュータントの一覧(667 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(最初に落ちたテストの実行ファイルで止める)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| U001 | L8: \|\| -> && | `test_up_begin:crash/rc=1` |
| U002 | L8: \|\| -> && | `test_up_begin:crash/rc=1` |
| U003 | L8: \|\| -> && | `test_up_begin:crash/rc=1` |
| U004 | L8: \|\| -> && | `test_up_begin:crash/rc=1` |
| U005 | L8: \|\| -> && | `test_up_begin:crash/rc=1` |
| U006 | L8: \|\| -> && | `test_init_validates_the_config` |
| U007 | L8: \|\| -> && | `test_init_validates_the_config` |
| U008 | L8: \|\| -> && | `test_init_validates_the_config` |
| U009 | L9: const 0+1 -> 1 | `test_init_validates_the_config` |
| U010 | L11: const 0+1 -> 1 | `test_up_begin:crash/rc=1` |
| U011 | L11: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| U012 | L12: delete statement | `test_up_begin:crash/rc=1` |
| U013 | L13: == -> != | `test_init_validates_the_config` |
| U014 | L13: negate if condition | `test_init_validates_the_config` |
| U015 | L13: * -> / | `test_init_validates_the_config` |
| U016 | L13: const 0+1 -> 1 | `test_init_validates_the_config` |
| U017 | L13: const 1024+1 -> 1025 | `test_init_validates_the_config` |
| U018 | L13: const 1024-1 -> 1023 | `test_init_validates_the_config` |
| U019 | L13: delete statement | `test_init_validates_the_config` |
| U020 | L14: == -> != | `test_init_validates_the_config` |
| U021 | L14: negate if condition | `test_init_validates_the_config` |
| U022 | L14: * -> / | `test_init_validates_the_config` |
| U023 | L14: const 0+1 -> 1 | `test_init_validates_the_config` |
| U024 | L14: const 1024+1 -> 1025 | `test_init_validates_the_config` |
| U025 | L14: const 1024-1 -> 1023 | `test_init_validates_the_config` |
| U026 | L14: delete statement | `test_init_validates_the_config` |
| U027 | L21: false -> true | `test_begin_size_boundaries`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U028 | L21: delete statement | `test_begin_size_boundaries`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U032 | L27: false -> true | `test_abort_closes_the_file_drops_sha_and_frees_the_slot`, `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U033 | L27: delete statement | `test_abort_closes_the_file_drops_sha_and_frees_the_slot`, `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U037 | L33: true -> false | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U038 | L33: delete statement | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U039 | L34: delete statement | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U040 | L35: + -> - | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_size_boundaries` |
| U041 | L35: const 1+1 -> 2 | `test_up_begin:crash/rc=1` |
| U043 | L35: delete statement | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_size_boundaries` |
| U044 | L39: negate if condition | `test_up_begin:crash/rc=1` |
| U045 | L39: delete statement | `test_up_begin:crash/rc=1` |
| U046 | L42: negate if condition | `test_chunk_across_a_part_boundary_is_split`, `test_chunk_ending_on_the_last_byte_of_a_part` |
| U047 | L42: delete statement | `test_chunk_across_a_part_boundary_is_split`, `test_chunk_ending_on_the_last_byte_of_a_part` |
| U048 | L44: delete statement | `test_default_chunk_size_is_128_kib`, `test_chunk_across_a_part_boundary_is_split` |
| U049 | L45: const 0+1 -> 1 | `test_sync_count_resets_after_each_sync_and_at_part_end` |
| U050 | L45: delete statement | `test_sync_count_resets_after_each_sync_and_at_part_end` |
| U051 | L46: < -> <= | `test_idle_close_reports_a_failing_close_after_a_good_sync`, `test_the_end_of_a_part_reports_a_failing_close` |
| U052 | L46: const 0+1 -> 1 | `test_idle_close_reports_a_failing_close_after_a_good_sync`, `test_the_end_of_a_part_reports_a_failing_close` |
| U053 | L51: == -> != | `test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory`, `test_idle_close_meta_failure_still_frees_the_slot` |
| U054 | L51: negate if condition | `test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory`, `test_idle_close_meta_failure_still_frees_the_slot` |
| U057 | L52: == -> != | `test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory`, `test_idle_close_meta_failure_still_frees_the_slot` |
| U058 | L52: == -> != | `test_fuzz_random_operations_with_power_cuts_and_failures` |
| U059 | L52: \|\| -> && | `test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory`, `test_idle_close_meta_failure_still_frees_the_slot` |
| U060 | L52: negate if condition | `test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory`, `test_idle_close_meta_failure_still_frees_the_slot` |
| U061 | L52: delete statement | `test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory`, `test_idle_close_meta_failure_still_frees_the_slot` |
| U064 | L59: true -> false | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U065 | L59: delete statement | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U066 | L60: delete statement | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U067 | L61: delete statement | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U068 | L62: const 0+1 -> 1 | `test_storage_boundary_is_size_plus_64_mib`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U069 | L62: delete statement | `test_set_free_resets_consumed_since` |
| U072 | L70: < -> <= | `test_refresh_free_reads_mount_info_and_keeps_the_cache_on_error` |
| U073 | L70: negate if condition | `test_refresh_free_reads_mount_info_and_keeps_the_cache_on_error` |
| U074 | L70: const 0+1 -> 1 | `test_refresh_free_reads_mount_info_and_keeps_the_cache_on_error` |
| U075 | L70: delete statement | `test_refresh_free_reads_mount_info_and_keeps_the_cache_on_error` |
| U076 | L71: delete statement | `test_refresh_free_reads_mount_info_and_keeps_the_cache_on_error` |
| U077 | L77: const 0+1 -> 1 | `test_storage_boundary_is_size_plus_64_mib`, `test_storage_formula_design_example` |
| U079 | L78: const 0+1 -> 1 | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U080 | L80: == -> != | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U081 | L80: \|\| -> && | `test_resume_storage_check_needs_remaining_plus_reserve`, `test_resume_storage_check_counts_other_images_but_not_itself` |
| U082 | L80: negate if condition | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U083 | L80: delete statement | `test_resume_storage_check_needs_remaining_plus_reserve`, `test_resume_storage_check_counts_other_images_but_not_itself` |
| U084 | L81: != -> == | `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U085 | L81: != -> == | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U086 | L81: && -> \|\| | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U087 | L81: negate if condition | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U088 | L81: delete statement | `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U090 | L82: negate if condition | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U091 | L82: - -> + | `test_storage_formula_design_example` |
| U092 | L82: += -> -= | `test_begin_size_boundaries`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U093 | L82: delete statement | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U094 | L88: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U095 | L88: const 0+1 -> 1 | `test_storage_unknown_cache_rejects` |
| U097 | L89: + -> - | `test_begin_size_boundaries`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U099 | L90: - -> + | `test_storage_formula_design_example`, `test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid` |
| U100 | L90: const 0+1 -> 1 | `test_free_estimate_saturates_at_zero` |
| U101 | L97: >= -> > | `test_storage_boundary_is_size_plus_64_mib`, `test_storage_formula_design_example` |
| U102 | L97: + -> - | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U103 | L97: * -> / | `test_storage_boundary_is_size_plus_64_mib`, `test_storage_formula_design_example` |
| U107 | L104: \|\| -> && | `test_up_begin:crash/rc=1` |
| U108 | L104: \|\| -> && | `test_begin_with_a_null_output_is_invalid_even_when_the_slot_is_busy` |
| U109 | L104: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U110 | L104: delete statement | `test_up_begin:crash/rc=1` |
| U111 | L105: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U112 | L105: delete statement | `test_begin_order_slot_then_name_then_count_then_storage` |
| U113 | L106: < -> <= | `test_begin_size_boundaries` |
| U114 | L106: > -> >= | `test_begin_size_boundaries` |
| U115 | L106: \|\| -> && | `test_begin_size_boundaries`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U116 | L106: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U117 | L106: delete statement | `test_begin_size_boundaries`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U118 | L108: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U119 | L108: delete statement | `test_begin_order_slot_then_name_then_count_then_storage` |
| U120 | L109: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U121 | L109: delete statement | `test_begin_order_slot_then_name_then_count_then_storage` |
| U122 | L110: >= -> > | `test_begin_order_slot_then_name_then_count_then_storage` |
| U123 | L110: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U124 | L110: delete statement | `test_begin_order_slot_then_name_then_count_then_storage` |
| U125 | L111: > -> >= | `test_size_over_total_bytes_is_507_even_with_enough_free` |
| U126 | L111: \|\| -> && | `test_size_over_total_bytes_is_507_even_with_enough_free` |
| U127 | L111: \|\| -> && | `test_storage_boundary_is_size_plus_64_mib`, `test_size_over_total_bytes_is_507_even_with_enough_free` |
| U128 | L111: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_with_a_declared_sha_is_stored_as_flag_1` |
| U129 | L111: delete statement | `test_storage_boundary_is_size_plus_64_mib`, `test_size_over_total_bytes_is_507_even_with_enough_free` |
| U130 | L114: < -> <= | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U131 | L114: negate if condition | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U132 | L114: const 0+1 -> 1 | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U133 | L114: delete statement | `test_begin_mkdir_failure_leaves_nothing`, `test_begin_meta_failure_cleans_up_and_keeps_the_slot_free` |
| U134 | L116: false -> true | `test_begin_order_slot_then_name_then_count_then_storage` |
| U135 | L116: delete statement | `test_begin_creates_the_image_and_takes_the_slot`, `test_begin_order_slot_then_name_then_count_then_storage` |
| U136 | L117: delete statement | `test_begin_starts_sha_state_and_stamps_the_time` |
| U137 | L118: delete statement | `test_sha_state_follows_the_received_bytes_across_parts` |
| U138 | L119: true -> false | `test_begin_starts_sha_state_and_stamps_the_time` |
| U139 | L119: delete statement | `test_begin_starts_sha_state_and_stamps_the_time` |
| U140 | L120: + -> - | `test_begin_starts_sha_state_and_stamps_the_time` |
| U143 | L120: delete statement | `test_begin_starts_sha_state_and_stamps_the_time` |
| U144 | L121: const 0+1 -> 1 | `test_begin_starts_sha_state_and_stamps_the_time` |
| U145 | L121: delete statement | `test_a_new_upload_after_an_incomplete_one_starts_a_fresh_sha_state` |
| U146 | L122: delete statement | `test_begin_creates_the_image_and_takes_the_slot` |
| U147 | L129: const 0+1 -> 1 | `test_info_reports_the_state` |
| U148 | L129: delete statement | `test_info_reports_the_state` |
| U149 | L130: delete statement | `test_info_reports_the_state` |
| U150 | L131: delete statement | `test_info_reports_the_state` |
| U151 | L133: == -> != | `test_info_reports_the_state`, `test_info_of_an_image_with_corrupt_meta` |
| U152 | L133: == -> != | `test_info_reports_the_state`, `test_info_of_an_image_with_corrupt_meta` |
| U153 | L133: \|\| -> && | `test_up_write:crash/rc=1` |
| U154 | L133: \|\| -> && | `test_info_reports_the_state` |
| U155 | L133: negate if condition | `test_up_write:crash/rc=1` |
| U156 | L133: delete statement | `test_up_write:crash/rc=1` |
| U157 | L134: true -> false | `test_info_reports_the_state`, `test_info_of_an_image_with_corrupt_meta` |
| U158 | L134: delete statement | `test_info_reports_the_state`, `test_info_of_an_image_with_corrupt_meta` |
| U159 | L135: delete statement | `test_info_reports_the_state`, `test_info_of_an_image_with_corrupt_meta` |
| U160 | L136: delete statement | `test_info_reports_the_state` |
| U161 | L137: const 0+1 -> 1 | `test_info_of_an_image_with_corrupt_meta` |
| U162 | L137: delete statement | `test_info_reports_the_state` |
| U163 | L138: delete statement | `test_info_reports_the_state` |
| U164 | L139: const 0+1 -> 1 | `test_info_reports_the_state` |
| U165 | L139: delete statement | `test_info_reports_the_state` |
| U167 | L149: < -> <= | `test_resume_recomputes_received_from_the_disk` |
| U168 | L149: negate if condition | `test_resume_recomputes_received_from_the_disk` |
| U169 | L149: const 0+1 -> 1 | `test_resume_recomputes_received_from_the_disk` |
| U170 | L149: delete statement | `test_truncate_and_stat_failures_keep_the_old_received_until_the_resume`, `test_resume_reports_an_error_when_the_part_sizes_cannot_be_read` |
| U171 | L150: delete statement | `test_resume_recomputes_received_from_the_disk` |
| U172 | L161: < -> <= | `test_up_write:crash/rc=1` |
| U173 | L161: negate if condition | `test_up_write:crash/rc=1` |
| U174 | L161: const 0+1 -> 1 | `test_up_write:crash/rc=1` |
| U176 | L163: > -> >= | `test_first_chunk_creates_p000_and_does_not_sync`, `test_chunk_across_a_part_boundary_is_split` |
| U177 | L163: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_chunk_across_a_part_boundary_is_split` |
| U178 | L163: const 0+1 -> 1 | `test_resume_one_byte_into_the_part_appends_without_truncating` |
| U179 | L164: true -> false | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_recomputes_received_from_the_disk` |
| U180 | L164: delete statement | `test_up_write:crash/rc=1` |
| U181 | L166: false -> true | `test_first_chunk_creates_p000_and_does_not_sync`, `test_chunk_across_a_part_boundary_is_split` |
| U182 | L166: delete statement | `test_up_write:crash/rc=1` |
| U183 | L167: == -> != | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U184 | L167: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U185 | L169: const 0+1 -> 1 | `test_resume_after_a_failed_first_write_reuses_the_empty_part_file`, `test_failure_in_the_second_part_rolls_back_the_whole_chunk` |
| U186 | L169: delete statement | `test_resume_after_a_failed_first_write_reuses_the_empty_part_file`, `test_failure_in_the_second_part_rolls_back_the_whole_chunk` |
| U187 | L170: == -> != | `test_up_failures:crash/rc=1` |
| U188 | L170: negate if condition | `test_up_failures:crash/rc=1` |
| U189 | L170: true -> false | `test_resume_after_a_failed_first_write_reuses_the_empty_part_file`, `test_failure_in_the_second_part_rolls_back_the_whole_chunk` |
| U190 | L170: delete statement | `test_up_failures:crash/rc=1` |
| U191 | L173: < -> <= | `test_up_write:crash/rc=1` |
| U192 | L173: negate if condition | `test_up_write:crash/rc=1` |
| U193 | L173: const 0+1 -> 1 | `test_up_write:crash/rc=1` |
| U194 | L173: delete statement | `test_up_failures:crash/rc=1` |
| U195 | L174: delete statement | `test_up_write:crash/rc=1` |
| U196 | L187: false -> true | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U197 | L187: delete statement | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_resume_after_a_failed_first_write_reuses_the_empty_part_file` |
| U198 | L188: false -> true | `test_a_clean_rollback_does_not_read_the_disk_again` |
| U199 | L189: > -> >= | `test_up_failures:crash/rc=1` |
| U200 | L189: const 0+1 -> 1 | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U201 | L191: < -> <= | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U202 | L191: \|\| -> && | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U203 | L191: - -> + | `test_failure_in_the_second_part_rolls_back_the_whole_chunk`, `test_a_clean_rollback_does_not_read_the_disk_again` |
| U204 | L191: const 1+1 -> 2 | `test_up_failures:crash/rc=1` |
| U205 | L191: const 1-1 -> 0 | `test_failure_in_the_second_part_rolls_back_the_whole_chunk`, `test_a_clean_rollback_does_not_read_the_disk_again` |
| U206 | L191: const 0+1 -> 1 | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U207 | L192: - -> + | `test_partial_write_with_no_space_is_truncated_back_and_reports_507`, `test_failure_in_the_second_part_rolls_back_the_whole_chunk` |
| U208 | L192: const 1+1 -> 2 | `test_up_failures:crash/rc=1` |
| U209 | L192: const 1-1 -> 0 | `test_partial_write_with_no_space_is_truncated_back_and_reports_507`, `test_failure_in_the_second_part_rolls_back_the_whole_chunk` |
| U210 | L192: const 0+1 -> 1 | `test_a_clean_rollback_does_not_read_the_disk_again` |
| U211 | L193: true -> false | `test_truncate_failure_takes_received_from_the_file_size`, `test_a_failed_rollback_reads_the_file_size` |
| U213 | L196: negate if condition | `test_truncate_failure_takes_received_from_the_file_size`, `test_a_clean_rollback_does_not_read_the_disk_again` |
| U214 | L196: delete statement | `test_truncate_failure_takes_received_from_the_file_size`, `test_a_failed_rollback_reads_the_file_size` |
| U215 | L197: delete statement | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U216 | L198: delete statement | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_resume_after_a_failed_first_write_reuses_the_empty_part_file` |
| U217 | L199: negate if condition | `test_truncate_failure_takes_received_from_the_file_size` |
| U218 | L199: delete statement | `test_truncate_failure_takes_received_from_the_file_size` |
| U219 | L200: delete statement | `test_a_clean_rollback_does_not_read_the_disk_again` |
| U220 | L201: == -> != | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_resume_after_a_failed_first_write_reuses_the_empty_part_file` |
| U221 | L205: negate if condition | `test_chunk_size_limit` |
| U222 | L205: const 0+1 -> 1 | `test_chunk_size_limit` |
| U223 | L205: delete statement | `test_chunk_size_limit` |
| U224 | L206: == -> != | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U225 | L206: \|\| -> && | `test_empty_and_null_chunks_are_invalid` |
| U226 | L206: \|\| -> && | `test_up_write:crash/rc=1` |
| U227 | L206: \|\| -> && | `test_empty_and_null_chunks_are_invalid` |
| U228 | L206: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U229 | L206: const 0+1 -> 1 | `test_empty_and_null_chunks_are_invalid`, `test_chunk_must_not_pass_size` |
| U230 | L206: delete statement | `test_empty_and_null_chunks_are_invalid` |
| U231 | L207: > -> >= | `test_chunk_size_limit`, `test_default_chunk_size_is_128_kib` |
| U232 | L207: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U233 | L207: delete statement | `test_chunk_size_limit`, `test_default_chunk_size_is_128_kib` |
| U234 | L211: == -> != | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U235 | L211: == -> != | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U236 | L211: \|\| -> && | `test_up_write:crash/rc=1` |
| U237 | L211: \|\| -> && | `test_state_checks` |
| U238 | L211: negate if condition | `test_up_write:crash/rc=1` |
| U239 | L211: delete statement | `test_up_write:crash/rc=1` |
| U240 | L214: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U241 | L215: negate if condition | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U242 | L215: delete statement | `test_resume_is_refused_while_another_image_uses_the_slot` |
| U243 | L216: != -> == | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U244 | L216: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U245 | L220: negate if condition | `test_sequential_chunks_append_to_the_open_file`, `test_offset_mismatch_writes_nothing_and_reports_received` |
| U246 | L222: < -> <= | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U247 | L222: negate if condition | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U248 | L222: const 0+1 -> 1 | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U249 | L222: delete statement | `test_resume_reports_an_error_when_the_part_sizes_cannot_be_read` |
| U250 | L224: negate if condition | `test_offset_mismatch_writes_nothing_and_reports_received`, `test_offset_mismatch_takes_priority_over_exceeds_size` |
| U251 | L224: delete statement | `test_offset_mismatch_writes_nothing_and_reports_received`, `test_offset_mismatch_takes_priority_over_exceeds_size` |
| U252 | L225: != -> == | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U253 | L225: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U254 | L225: delete statement | `test_offset_mismatch_writes_nothing_and_reports_received`, `test_offset_mismatch_takes_priority_over_exceeds_size` |
| U255 | L226: > -> >= | `test_chunk_must_not_pass_size`, `test_exceeds_size_by_one_byte_in_the_middle` |
| U256 | L226: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U257 | L226: - -> + | `test_chunk_must_not_pass_size`, `test_exceeds_size_by_one_byte_in_the_middle` |
| U258 | L226: delete statement | `test_chunk_must_not_pass_size`, `test_exceeds_size_by_one_byte_in_the_middle` |
| U259 | L227: negate if condition | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_storage_check_needs_remaining_plus_reserve` |
| U260 | L228: negate if condition | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U261 | L228: - -> + | `test_resume_storage_check_needs_remaining_plus_reserve` |
| U262 | L228: delete statement | `test_resume_storage_check_needs_remaining_plus_reserve`, `test_resume_storage_check_counts_other_images_but_not_itself` |
| U263 | L229: true -> false | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U264 | L231: != -> == | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U265 | L231: negate if condition | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U266 | L231: const 0+1 -> 1 | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_exactly_at_a_part_boundary_creates_the_next_part` |
| U268 | L232: false -> true | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_one_byte_into_the_part_appends_without_truncating` |
| U269 | L232: delete statement | `test_resume_after_idle_appends_without_writing_meta`, `test_resume_one_byte_into_the_part_appends_without_truncating` |
| U270 | L234: const 0+1 -> 1 | `test_sequential_chunks_append_to_the_open_file`, `test_offset_mismatch_writes_nothing_and_reports_received` |
| U271 | L235: negate if condition | `test_sequential_chunks_append_to_the_open_file`, `test_offset_mismatch_writes_nothing_and_reports_received` |
| U272 | L235: delete statement | `test_truncate_failure_takes_received_from_the_file_size` |
| U273 | L240: const 0+1 -> 1 | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U274 | L242: const 0+1 -> 1 | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U275 | L244: < -> <= | `test_up_write:crash/rc=1` |
| U276 | L245: / -> * | `test_chunk_across_a_part_boundary_is_split`, `test_chunk_ending_on_the_last_byte_of_a_part` |
| U277 | L247: - -> + | `test_chunk_across_a_part_boundary_is_split`, `test_chunk_one_byte_before_a_part_boundary_stays_in_the_part` |
| U279 | L248: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U280 | L248: - -> + | `test_default_chunk_size_is_128_kib`, `test_one_chunk_covering_three_parts` |
| U281 | L248: - -> + | `test_default_chunk_size_is_128_kib`, `test_chunk_across_a_part_boundary_is_split` |
| U282 | L248: delete statement | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U283 | L250: negate if condition | `test_up_write:crash/rc=1` |
| U284 | L251: delete statement | `test_up_write:crash/rc=1` |
| U285 | L252: < -> <= | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U286 | L252: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U287 | L252: const 0+1 -> 1 | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U288 | L252: delete statement | `test_up_failures:crash/rc=1` |
| U289 | L254: delete statement | `test_failure_in_the_second_part_rolls_back_the_whole_chunk`, `test_a_clean_rollback_does_not_read_the_disk_again` |
| U290 | L255: delete statement | `test_partial_write_with_no_space_is_truncated_back_and_reports_507`, `test_failure_in_the_second_part_rolls_back_the_whole_chunk` |
| U291 | L256: delete statement | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_partial_write_with_no_space_is_truncated_back_and_reports_507` |
| U292 | L257: + -> - | `test_up_write:crash/rc=1` |
| U293 | L257: delete statement | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U294 | L258: < -> <= | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U295 | L258: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U296 | L258: const 0+1 -> 1 | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U297 | L258: delete statement | `test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete`, `test_resume_after_a_failed_first_write_reuses_the_empty_part_file` |
| U298 | L259: += -> -= | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U299 | L259: delete statement | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U300 | L260: += -> -= | `test_default_chunk_size_is_128_kib`, `test_chunk_across_a_part_boundary_is_split` |
| U301 | L260: delete statement | `test_up_write:crash/rc=1` |
| U302 | L261: += -> -= | `test_first_chunk_creates_p000_and_does_not_sync`, `test_chunk_across_a_part_boundary_is_split` |
| U303 | L261: delete statement | `test_sync_every_sync_bytes_boundary_is_inclusive`, `test_sync_count_resets_after_each_sync_and_at_part_end` |
| U304 | L262: == -> != | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U305 | L262: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U306 | L262: + -> - | `test_chunk_across_a_part_boundary_is_split`, `test_chunk_ending_on_the_last_byte_of_a_part` |
| U307 | L263: true -> false | `test_chunk_across_a_part_boundary_is_split`, `test_chunk_ending_on_the_last_byte_of_a_part` |
| U308 | L263: delete statement | `test_chunk_across_a_part_boundary_is_split`, `test_chunk_ending_on_the_last_byte_of_a_part` |
| U309 | L264: < -> <= | `test_default_chunk_size_is_128_kib`, `test_chunk_must_not_pass_size` |
| U310 | L264: negate if condition | `test_default_chunk_size_is_128_kib`, `test_chunk_must_not_pass_size` |
| U311 | L264: const 0+1 -> 1 | `test_default_chunk_size_is_128_kib`, `test_chunk_must_not_pass_size` |
| U312 | L264: delete statement | `test_sync_failure_at_the_end_of_a_part_rolls_back`, `test_the_end_of_a_part_reports_a_failing_close` |
| U313 | L265: >= -> > | `test_sync_every_sync_bytes_boundary_is_inclusive`, `test_default_sync_is_4_mib` |
| U314 | L265: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_chunk_across_a_part_boundary_is_split` |
| U315 | L266: delete statement | `test_sync_every_sync_bytes_boundary_is_inclusive`, `test_sync_count_resets_after_each_sync_and_at_part_end` |
| U316 | L267: < -> <= | `test_default_part_size_boundary_at_1_gib`, `test_default_part_size_boundary_chunk_straddles_1_gib` |
| U317 | L267: negate if condition | `test_default_part_size_boundary_at_1_gib`, `test_default_part_size_boundary_chunk_straddles_1_gib` |
| U318 | L267: const 0+1 -> 1 | `test_default_part_size_boundary_at_1_gib`, `test_default_part_size_boundary_chunk_straddles_1_gib` |
| U319 | L267: delete statement | `test_periodic_sync_failure_rolls_back_the_chunk` |
| U320 | L268: const 0+1 -> 1 | `test_sync_every_sync_bytes_boundary_is_inclusive` |
| U321 | L268: delete statement | `test_sync_every_sync_bytes_boundary_is_inclusive`, `test_sync_count_resets_after_each_sync_and_at_part_end` |
| U322 | L272: delete statement | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U323 | L273: negate if condition | `test_sequential_chunks_append_to_the_open_file`, `test_offset_mismatch_writes_nothing_and_reports_received` |
| U324 | L274: delete statement | `test_sha_state_follows_the_received_bytes_across_parts` |
| U325 | L275: += -> -= | `test_sequential_chunks_append_to_the_open_file`, `test_offset_mismatch_writes_nothing_and_reports_received` |
| U326 | L275: delete statement | `test_sequential_chunks_append_to_the_open_file`, `test_offset_mismatch_writes_nothing_and_reports_received` |
| U327 | L277: += -> -= | `test_sequential_chunks_append_to_the_open_file`, `test_resume_after_idle_appends_without_writing_meta` |
| U328 | L277: delete statement | `test_sequential_chunks_append_to_the_open_file`, `test_consumed_since_accumulates_and_lowers_the_estimate` |
| U329 | L278: delete statement | `test_time_of_the_last_put_is_recorded` |
| U330 | L279: negate if condition | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U331 | L279: delete statement | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U332 | L280: delete statement | `test_first_chunk_creates_p000_and_does_not_sync`, `test_state_changes_notify_the_view` |
| U333 | L287: != -> == | `test_begin_order_slot_then_name_then_count_then_storage` |
| U336 | L287: negate if condition | `test_begin_order_slot_then_name_then_count_then_storage` |
| U337 | L287: const 0+1 -> 1 | `test_begin_order_slot_then_name_then_count_then_storage` |
| U339 | L289: != -> == | `test_begin_order_slot_then_name_then_count_then_storage` |
| U340 | L289: \|\| -> && | `test_idle_close_is_a_noop_for_other_images_and_states` |
| U341 | L289: negate if condition | `test_begin_order_slot_then_name_then_count_then_storage` |
| U342 | L289: delete statement | `test_idle_close_is_a_noop_for_other_images_and_states` |
| U343 | L291: true -> false | `test_idle_close_syncs_closes_writes_meta_and_frees_the_slot` |
| U344 | L292: < -> <= | `test_a_good_idle_close_does_not_read_the_sizes_again` |
| U345 | L292: negate if condition | `test_idle_close_when_sync_and_close_both_fail_takes_received_from_the_disk`, `test_a_good_idle_close_does_not_read_the_sizes_again` |
| U346 | L292: const 0+1 -> 1 | `test_a_good_idle_close_does_not_read_the_sizes_again` |
| U347 | L292: delete statement | `test_idle_close_when_sync_and_close_both_fail_takes_received_from_the_disk` |
| U348 | L294: delete statement | `test_begin_order_slot_then_name_then_count_then_storage` |
| U349 | L295: delete statement | `test_state_changes_notify_the_view` |
| U350 | L296: < -> <= | `test_idle_close_meta_failure_still_frees_the_slot`, `test_resume_after_a_failed_idle_close_works` |
| U351 | L296: const 0+1 -> 1 | `test_idle_close_meta_failure_still_frees_the_slot`, `test_resume_after_a_failed_idle_close_works` |
| U352 | L300: != -> == | `test_begin_size_boundaries` |
| U353 | L300: \|\| -> && | `test_abort_closes_the_file_drops_sha_and_frees_the_slot`, `test_state_changes_notify_the_view` |
| U354 | L300: negate if condition | `test_begin_size_boundaries` |
| U355 | L300: const 0+1 -> 1 | `test_begin_size_boundaries` |
| U356 | L300: delete statement | `test_abort_closes_the_file_drops_sha_and_frees_the_slot`, `test_state_changes_notify_the_view` |
| U357 | L301: false -> true | `test_abort_closes_the_file_drops_sha_and_frees_the_slot` |
| U358 | L301: delete statement | `test_abort_closes_the_file_drops_sha_and_frees_the_slot` |
| U359 | L302: == -> != | `test_abort_closes_the_file_drops_sha_and_frees_the_slot` |
| U360 | L302: negate if condition | `test_abort_closes_the_file_drops_sha_and_frees_the_slot` |
| U361 | L302: const 0+1 -> 1 | `test_abort_closes_the_file_drops_sha_and_frees_the_slot` |
| U362 | L302: delete statement | `test_abort_closes_the_file_drops_sha_and_frees_the_slot` |
| U363 | L303: delete statement | `test_begin_size_boundaries` |
| U364 | L304: delete statement | `test_state_changes_notify_the_view` |
| U365 | L310: == -> != | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U366 | L310: == -> != | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U367 | L310: \|\| -> && | `test_up_finish:crash/rc=1` |
| U368 | L310: \|\| -> && | `test_finish_begin_other_states` |
| U369 | L310: negate if condition | `test_up_finish:crash/rc=1` |
| U370 | L310: delete statement | `test_up_finish:crash/rc=1` |
| U371 | L317: == -> != | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U372 | L317: negate if condition | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U373 | L319: < -> <= | `test_finish_begin_incomplete_with_less_than_size_is_size_mismatch`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U374 | L319: negate if condition | `test_finish_begin_incomplete_with_less_than_size_is_size_mismatch`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U375 | L319: const 0+1 -> 1 | `test_finish_begin_incomplete_with_less_than_size_is_size_mismatch`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U376 | L319: delete statement | `test_finish_begin_reload_error_changes_nothing` |
| U377 | L321: negate if condition | `test_up_finish:crash/rc=1` |
| U378 | L321: delete statement | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U379 | L324: < -> <= | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U380 | L324: negate if condition | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U381 | L324: const 0+1 -> 1 | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U383 | L325: == -> != | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U384 | L325: negate if condition | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U385 | L325: delete statement | `test_finish_begin_size_mismatch_reports_received`, `test_finish_begin_incomplete_with_less_than_size_is_size_mismatch` |
| U386 | L326: == -> != | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U387 | L326: negate if condition | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U388 | L326: delete statement | `test_finish_begin_incomplete_complete_with_the_slot_used_is_upload_in_progress` |
| U389 | L327: > -> >= | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U390 | L327: negate if condition | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_size_mismatch_reports_received` |
| U393 | L328: true -> false | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U394 | L328: delete statement | `test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot`, `test_finish_begin_incomplete_complete_takes_the_free_slot` |
| U395 | L329: delete statement | `test_finish_notifies_the_view_at_the_acceptance_and_at_the_end_only`, `test_invalid_end_notifies_and_refreshes_the_free_space` |
| U396 | L334: delete statement | `test_finish_undo_restores_the_state_and_the_slot` |
| U397 | L335: == -> != | `test_finish_undo_restores_the_state_and_the_slot` |
| U398 | L335: negate if condition | `test_finish_undo_restores_the_state_and_the_slot` |
| U399 | L336: delete statement | `test_finish_undo_restores_the_state_and_the_slot` |
| U400 | L338: false -> true | `test_finish_undo_restores_the_state_and_the_slot` |
| U401 | L338: delete statement | `test_finish_undo_restores_the_state_and_the_slot` |
| U402 | L340: delete statement | `test_undo_and_acceptance_notify_the_view` |
| U404 | L346: - -> + | `test_activity_boundaries`, `test_activity_of_the_slot` |
| U406 | L347: <= -> < | `test_activity_boundaries`, `test_activity_of_the_slot` |
| U407 | L347: * -> / | `test_activity_boundaries`, `test_activity_of_the_slot` |
| U408 | L347: const 1000+1 -> 1001 | `test_activity_boundaries`, `test_activity_of_the_slot` |
| U409 | L347: const 1000-1 -> 999 | `test_activity_boundaries`, `test_activity_of_the_slot` |
| U410 | L351: negate if condition | `test_activity_of_the_slot` |
| U411 | L351: delete statement | `test_activity_of_the_slot` |
| U412 | L352: negate if condition | `test_activity_of_the_slot` |
| U413 | L352: delete statement | `test_activity_of_the_slot` |
| U414 | L357: negate if condition | `test_up_begin:crash/rc=1` |
| U415 | L359: delete statement | `test_first_chunk_creates_p000_and_does_not_sync`, `test_sequential_chunks_append_to_the_open_file` |
| U416 | L360: delete statement | `test_reinit_drops_the_file_the_sha_state_and_the_slot`, `test_reinit_closes_the_handle_even_if_close_fails` |
| U417 | L362: const 0+1 -> 1 | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U418 | L362: delete statement | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U419 | L363: delete statement | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U420 | L364: delete statement | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U422 | L366: const 0+1 -> 1 | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U423 | L368: == -> != | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U424 | L368: == -> != | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U425 | L368: \|\| -> && | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U426 | L368: negate if condition | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| U427 | L368: delete statement | `test_reinit_drops_the_file_the_sha_state_and_the_slot` |
| F001 | L15: \|\| -> && | `test_up_index:crash/rc=1` |
| F002 | L15: negate if condition | `test_up_index:crash/rc=1` |
| F003 | L15: delete statement | `test_up_index:crash/rc=1` |
| F004 | L16: const 0+1 -> 1 | `test_job_init_clears_the_whole_job` |
| F005 | L16: delete statement | `test_up_index:crash/rc=1` |
| F006 | L17: + -> - | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F007 | L17: const 1+1 -> 2 | `test_up_index:crash/rc=1` |
| F009 | L17: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F011 | L27: delete statement | `test_missing_part_found_by_the_hash_step`, `test_short_part_found_while_rehashing` |
| F012 | L28: delete statement | `test_missing_part_found_by_the_hash_step`, `test_short_part_found_while_rehashing` |
| F013 | L33: delete statement | `test_hash_read_error_makes_incomplete`, `test_hash_open_error_makes_incomplete` |
| F014 | L34: delete statement | `test_hash_read_error_makes_incomplete`, `test_hash_step_of_an_unknown_image_fails_without_touching_the_sha_state` |
| F015 | L42: \|\| -> && | `test_up_finish:crash/rc=1` |
| F016 | L42: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F017 | L42: delete statement | `test_up_finish:crash/rc=1` |
| F018 | L45: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F019 | L46: delete statement | `test_hash_step_discards_the_streamed_state_it_overwrites` |
| F020 | L47: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F021 | L48: true -> false | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F022 | L48: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F023 | L54: < -> <= | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F024 | L54: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F025 | L54: const 0+1 -> 1 | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F027 | L56: delete statement | `test_up_finish:crash/rc=1` |
| F028 | L57: == -> != | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F029 | L57: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F030 | L57: delete statement | `test_missing_part_found_by_the_hash_step` |
| F031 | L58: < -> <= | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F032 | L58: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F033 | L58: const 0+1 -> 1 | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F034 | L58: delete statement | `test_up_finish:crash/rc=1` |
| F035 | L61: const 0+1 -> 1 | `test_hash_step_budget_zero_still_progresses` |
| F036 | L62: < -> <= | `test_up_finish:crash/rc=1` |
| F037 | L62: < -> <= | `test_up_finish:crash/rc=1` |
| F038 | L62: && -> \|\| | `test_up_finish:crash/rc=1` |
| F039 | L63: - -> + | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F041 | L64: negate if condition | `test_up_finish:crash/rc=1` |
| F042 | L64: delete statement | `test_up_finish:crash/rc=1` |
| F044 | L65: negate if condition | `test_up_finish:crash/rc=1` |
| F045 | L65: - -> + | `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F046 | L65: - -> + | `test_up_finish:crash/rc=1` |
| F047 | L65: delete statement | `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F049 | L67: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F050 | L68: < -> <= | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F051 | L68: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F052 | L68: const 0+1 -> 1 | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F053 | L69: delete statement | `test_hash_read_error_makes_incomplete` |
| F054 | L72: != -> == | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F055 | L72: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F056 | L73: delete statement | `test_short_part_found_while_rehashing` |
| F057 | L76: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F058 | L77: += -> -= | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F059 | L77: delete statement | `test_up_finish:timeout` |
| F060 | L78: += -> -= | `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps`, `test_hash_step_runs_to_the_digest_across_parts` |
| F061 | L78: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F062 | L80: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F063 | L82: >= -> > | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F064 | L82: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F065 | L83: delete statement | `test_up_finish:timeout` |
| F066 | L84: const 0+1 -> 1 | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F067 | L84: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F068 | L85: >= -> > | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F069 | L85: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F070 | L86: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F071 | L87: true -> false | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F072 | L87: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F073 | L102: / -> * | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F074 | L103: < -> <= | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F075 | L103: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F076 | L103: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F078 | L105: delete statement | `test_up_finish:crash/rc=1` |
| F079 | L106: < -> <= | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F080 | L106: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F081 | L106: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F082 | L106: delete statement | `test_up_finish:crash/rc=1` |
| F084 | L109: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F085 | L110: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F086 | L111: < -> <= | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F087 | L111: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F088 | L111: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F089 | L111: delete statement | `test_signature_read_error_makes_incomplete` |
| F090 | L112: == -> != | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F091 | L112: == -> != | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F092 | L112: == -> != | `test_signature_bea01_is_accepted`, `test_wrong_signature_is_not_iso` |
| F093 | L112: && -> \|\| | `test_wrong_signature_is_not_iso`, `test_invalid_image_stays_invalid_for_later_finish` |
| F094 | L112: \|\| -> && | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F095 | L112: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F099 | L112: const 1-1 -> 0 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F101 | L112: const 5-1 -> 4 | `test_wrong_signature_is_not_iso` |
| F102 | L112: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F104 | L112: const 1-1 -> 0 | `test_signature_bea01_is_accepted` |
| F106 | L112: const 5-1 -> 4 | `test_wrong_signature_is_not_iso` |
| F107 | L112: const 0+1 -> 1 | `test_signature_bea01_is_accepted`, `test_wrong_signature_is_not_iso` |
| F108 | L112: true -> false | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F109 | L112: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F110 | L122: != -> == | `test_invalid_meta_write_failure_returns_error_but_state_is_invalid` |
| F111 | L122: negate if condition | `test_invalid_meta_write_failure_returns_error_but_state_is_invalid` |
| F113 | L123: delete statement | `test_invalid_meta_write_failure_returns_error_but_state_is_invalid` |
| F114 | L124: delete statement | `test_invalid_meta_write_failure_returns_error_but_state_is_invalid` |
| F116 | L127: delete statement | `test_wrong_signature_is_not_iso`, `test_declared_sha_mismatch_with_streamed_state` |
| F119 | L130: delete statement | `test_invalid_end_notifies_and_refreshes_the_free_space` |
| F120 | L131: delete statement | `test_invalid_end_notifies_and_refreshes_the_free_space` |
| F121 | L132: < -> <= | `test_wrong_signature_is_not_iso`, `test_declared_sha_mismatch_with_streamed_state` |
| F123 | L132: const 0+1 -> 1 | `test_wrong_signature_is_not_iso`, `test_declared_sha_mismatch_with_streamed_state` |
| F127 | L138: delete statement | `test_ioerr_end_notifies_and_takes_received_from_the_disk` |
| F128 | L139: delete statement | `test_get_extents_io_error_makes_incomplete_and_a_retry_succeeds`, `test_stat_io_error_during_index` |
| F129 | L140: delete statement | `test_meta_write_failure_at_the_end_does_not_install_the_table` |
| F130 | L141: delete statement | `test_get_extents_io_error_makes_incomplete_and_a_retry_succeeds`, `test_stat_io_error_during_index` |
| F132 | L143: delete statement | `test_ioerr_end_notifies_and_takes_received_from_the_disk` |
| F133 | L149: == -> != | `test_extent_count_over_the_limit_is_too_fragmented_by_the_table_build`, `test_get_extents_too_fragmented_is_too_fragmented` |
| F134 | L149: negate if condition | `test_extent_count_over_the_limit_is_too_fragmented_by_the_table_build`, `test_get_extents_too_fragmented_is_too_fragmented` |
| F135 | L149: delete statement | `test_extent_count_over_the_limit_is_too_fragmented_by_the_table_build`, `test_get_extents_too_fragmented_is_too_fragmented` |
| F136 | L154: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F137 | L155: delete statement | `test_aborted_job_frees_its_work_area_and_changes_nothing` |
| F138 | L156: delete statement | `test_up_finish:crash/rc=1` |
| F139 | L160: == -> != | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F140 | L160: != -> == | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F141 | L160: \|\| -> && | `test_up_finish:crash/rc=1` |
| F142 | L160: \|\| -> && | `test_up_finish:crash/rc=1` |
| F143 | L160: \|\| -> && | `test_stale_job_when_the_state_changed_but_the_slot_is_still_held`, `test_stale_job_when_the_meta_is_invalid` |
| F144 | L160: \|\| -> && | `test_stale_job_when_the_state_changed_but_the_slot_is_still_held` |
| F145 | L160: \|\| -> && | `test_stale_job_when_the_slot_is_not_finalizing` |
| F146 | L161: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F147 | L162: delete statement | `test_up_finish:crash/rc=1` |
| F149 | L170: delete statement | `test_up_finish:timeout` |
| F150 | L175: true -> false | `test_close_failure_keeps_the_streamed_sha_for_the_retry` |
| F151 | L176: < -> <= | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F152 | L176: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F153 | L176: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F154 | L176: delete statement | `test_close_failure_keeps_the_streamed_sha_for_the_retry`, `test_close_failure_after_a_good_sync_is_an_io_error` |
| F155 | L177: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_resumed_upload_after_idle_still_streams_the_sha` |
| F156 | L178: delete statement | `test_up_finish:timeout` |
| F157 | L183: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F158 | L184: != -> == | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F159 | L184: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F160 | L184: delete statement | `test_missing_part_found_by_the_hash_step`, `test_short_part_found_while_rehashing` |
| F161 | L185: negate if condition | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F162 | L185: delete statement | `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps`, `test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps` |
| F163 | L190: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F164 | L191: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F167 | L194: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_get_extents_io_error_makes_incomplete_and_a_retry_succeeds` |
| F168 | L195: == -> != | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps` |
| F169 | L195: != -> == | `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag`, `test_declared_sha_mismatch_with_streamed_state` |
| F170 | L195: && -> \|\| | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F171 | L195: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F172 | L195: const 0+1 -> 1 | `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag`, `test_declared_sha_mismatch_with_streamed_state` |
| F173 | L197: delete statement | `test_up_finish:timeout` |
| F175 | L204: == -> != | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F176 | L204: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F177 | L204: delete statement | `test_missing_first_part_with_the_streamed_sha_is_part_mismatch` |
| F178 | L205: < -> <= | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F179 | L205: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F180 | L205: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F181 | L205: delete statement | `test_signature_read_error_makes_incomplete`, `test_signature_open_error_makes_incomplete` |
| F182 | L206: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F183 | L206: delete statement | `test_wrong_signature_is_not_iso`, `test_invalid_image_stays_invalid_for_later_finish` |
| F184 | L207: delete statement | `test_up_finish:timeout` |
| F185 | L213: == -> != | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F186 | L213: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F187 | L213: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F188 | L214: == -> != | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F189 | L214: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F190 | L215: delete statement | `test_up_finish:timeout` |
| F191 | L223: true -> false | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F192 | L225: != -> == | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F194 | L225: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F195 | L225: const 0+1 -> 1 | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F197 | L225: delete statement | `test_meta_write_failure_at_the_end_does_not_install_the_table` |
| F198 | L228: delete statement | `test_up_powercut:crash/rc=1` |
| F199 | L229: negate if condition | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F200 | L230: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_refreshes_the_free_space_cache` |
| F201 | L232: delete statement | `test_table_hook_may_be_absent` |
| F202 | L234: delete statement | `test_finish_with_the_streamed_sha_reaches_ready`, `test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag` |
| F204 | L236: delete statement | `test_finish_refreshes_the_free_space_cache` |
| F205 | L237: delete statement | `test_finish_notifies_the_view_at_the_acceptance_and_at_the_end_only` |
| X001 | L13: \|\| -> && | `test_job_init_rejects_a_bad_id` |
| X002 | L13: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X003 | L13: delete statement | `test_job_init_rejects_a_bad_id` |
| X004 | L14: const 0+1 -> 1 | `test_up_index:crash/rc=1` |
| X005 | L14: delete statement | `test_job_init_clears_the_whole_job` |
| X006 | L15: + -> - | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X007 | L15: const 1+1 -> 2 | `test_up_index:crash/rc=1` |
| X009 | L15: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X011 | L21: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X012 | L22: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X013 | L23: delete statement | `test_up_index:crash/rc=1` |
| X014 | L28: delete statement | `test_missing_part_is_part_mismatch`, `test_missing_first_part_is_part_mismatch` |
| X015 | L29: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X016 | L30: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X017 | L31: delete statement | `test_up_index:crash/rc=1` |
| X018 | L37: delete statement | `test_missing_part_is_part_mismatch`, `test_missing_first_part_is_part_mismatch` |
| X019 | L38: delete statement | `test_missing_part_is_part_mismatch`, `test_missing_first_part_is_part_mismatch` |
| X020 | L39: delete statement | `test_missing_part_is_part_mismatch`, `test_missing_first_part_is_part_mismatch` |
| X021 | L40: delete statement | `test_step_after_an_invalid_or_error_outcome_stays_done_and_allocates_nothing` |
| X022 | L45: delete statement | `test_stat_io_error`, `test_get_extents_io_error` |
| X023 | L46: delete statement | `test_stat_io_error`, `test_get_extents_io_error` |
| X024 | L47: delete statement | `test_stat_io_error`, `test_get_extents_io_error` |
| X025 | L48: delete statement | `test_step_after_an_invalid_or_error_outcome_stays_done_and_allocates_nothing` |
| X026 | L55: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X027 | L55: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X028 | L55: delete statement | `test_up_index:crash/rc=1` |
| X029 | L56: != -> == | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X030 | L56: != -> == | `test_unknown_or_unusable_image_is_an_error_outcome` |
| X031 | L56: && -> \|\| | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X032 | L56: \|\| -> && | `test_up_index:crash/rc=1` |
| X033 | L56: \|\| -> && | `test_unknown_or_unusable_image_is_an_error_outcome` |
| X034 | L56: negate if condition | `test_up_index:crash/rc=1` |
| X035 | L60: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X036 | L60: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X037 | L62: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X038 | L62: != -> == | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X039 | L62: > -> >= | `test_exactly_16_parts_are_accepted` |
| X041 | L62: \|\| -> && | `test_part_count_that_disagrees_with_the_meta_is_part_mismatch` |
| X042 | L62: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X043 | L62: const 0+1 -> 1 | `test_minimal_image_34816_bytes_one_part`, `test_extent_count_at_the_limit_is_accepted` |
| X044 | L62: delete statement | `test_part_count_that_disagrees_with_the_meta_is_part_mismatch` |
| X045 | L63: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X046 | L64: const 0+1 -> 1 | `test_index_without_blockdev_uses_epoch_0` |
| X047 | L64: delete statement | `test_index_builds_the_table_part_by_part`, `test_epoch_is_recorded_at_the_start` |
| X048 | L65: * -> / | `test_up_index:crash/rc=1` |
| X049 | L65: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X050 | L66: negate if condition | `test_up_index:crash/rc=1` |
| X051 | L66: delete statement | `test_up_index:crash/rc=1` |
| X052 | L67: const 0+1 -> 1 | `test_up_index:crash/rc=1` |
| X054 | L68: const 0+1 -> 1 | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X056 | L69: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X057 | L73: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X058 | L73: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X059 | L76: < -> <= | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X060 | L76: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X061 | L76: const 0+1 -> 1 | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X063 | L78: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X064 | L79: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X065 | L79: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X066 | L79: delete statement | `test_missing_part_is_part_mismatch`, `test_missing_first_part_is_part_mismatch` |
| X067 | L80: < -> <= | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X068 | L80: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X069 | L80: const 0+1 -> 1 | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X070 | L80: delete statement | `test_stat_io_error_during_index` |
| X071 | L81: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X072 | L81: delete statement | `test_part_that_is_a_directory_is_part_mismatch` |
| X074 | L83: + -> - | `test_up_index:crash/rc=1` |
| X075 | L83: - -> + | `test_up_finish:crash/rc=1` |
| X076 | L83: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X077 | L84: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X078 | L84: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X079 | L84: delete statement | `test_too_fragmented_from_the_fs` |
| X080 | L85: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X081 | L85: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X082 | L85: delete statement | `test_get_extents_not_found_is_part_mismatch` |
| X083 | L86: < -> <= | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X084 | L86: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X085 | L86: const 0+1 -> 1 | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X086 | L86: delete statement | `test_get_extents_io_error`, `test_step_after_an_invalid_or_error_outcome_stays_done_and_allocates_nothing` |
| X087 | L87: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X088 | L88: + -> - | `test_up_index:crash/rc=1` |
| X089 | L88: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X090 | L89: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X091 | L90: += -> -= | `test_up_index:crash/rc=1` |
| X092 | L90: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X093 | L91: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X094 | L92: >= -> > | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X095 | L92: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X096 | L92: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X097 | L99: == -> != | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X098 | L99: negate if condition | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X099 | L100: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X100 | L101: delete statement | `test_index_builds_the_table_part_by_part`, `test_index_step_count_is_init_plus_parts_plus_build` |
| X101 | L102: delete statement | `test_index_builds_the_table_part_by_part` |
| X102 | L103: delete statement | `test_up_index:crash/rc=1` |
| X103 | L106: == -> != | `test_wrong_size_part_is_part_mismatch`, `test_extents_that_do_not_cover_the_file_are_part_mismatch` |
| X104 | L106: negate if condition | `test_wrong_size_part_is_part_mismatch`, `test_extents_that_do_not_cover_the_file_are_part_mismatch` |
| X105 | L106: delete statement | `test_extent_count_over_the_limit_is_too_fragmented_by_the_table_build` |
| X106 | L107: == -> != | `test_wrong_size_part_is_part_mismatch`, `test_extents_that_do_not_cover_the_file_are_part_mismatch` |
| X107 | L107: negate if condition | `test_wrong_size_part_is_part_mismatch`, `test_extents_that_do_not_cover_the_file_are_part_mismatch` |
| X108 | L107: delete statement | `test_allocation_failures` |


## 4. 設計に書かれていないこと、設計と違うことを、実装で決めた箇所(レビューで確認してほしい)

テストで固定してあるが、`ISO_MOUNT_DESIGN.md` に明示がない、または設計の記述と違うので、設計の文書への反映(または変更)を提案する。

1. **`FINISH` の順序**: 設計 3.4 の本文は「SHA-256 → 署名 → 全パーツを sync + close → エクステント」だが、実装は **先にファイルを sync + close する**(FatFs のファイルの排他: 書き込みで開いたままのファイルは、読み取りで開けない(`FR_LOCKED`)。モックも `ISO_ERR_BUSY` にする)。結果は同じ。
2. **逐次の SHA-256 の状態の寿命**: 検査の途中(確定(`final`)の後)の I/O エラーで `incomplete` になると、状態は使い切っているので、**再試行(`finish`)は、全パーツを読み直す**。確定の前(ファイルの `close` の失敗)なら、状態は残り、読み直さない。
3. **`received` の決め直し**: 再開(`incomplete` の最初の `PUT`)と、`incomplete` の `finish` の受付は、**ディスク上のパーツの大きさから `received` を決め直す**(メモリの値は、取り消せなかった書き込みなどで、小さいことがある)。`uploading` の進行中は、メモリの値を信じる。`truncate` も大きさの読み出しも失敗したときは、書く前の値のまま(古いが小さい側)で、再開の最初の `PUT` が決め直し、`offset_mismatch` で正しい値を返す。
4. **書き込み・`sync` の失敗は、チャンク全体を取り消す**: パーツをまたいだチャンクの、最初のパーツの分も `truncate` で書く前の大きさに戻す(新しく作ったパーツは 0)。次の再開で、空のパーツが残っていれば、`open(新規)` が `EXISTS` で失敗するので、0 に `truncate` してから追記で開く。部分書き込みつきの `ISO_ERR_NO_SPACE` は、取り消した上で `ISO_RES_INSUFFICIENT_STORAGE`(507 相当)を返す。
5. **`consumed_since` は、成功したチャンクの分だけ足す**(失敗して取り消した分は、解放されるので足さない)。キャッシュを一度も採取していない(マウント前)ときは、`begin` は空きなしとして拒否する。
6. **`begin` の検査の順序**: 名前・`size`(400)→ 枠 → 同名 → 上限 → 空き容量(`size` が `total_bytes` 超を含む)。`iso_catalog_begin` の検査(名前、`size`、同名、上限)を、空き容量の検査のために、先に行う(二重になるが、`iso_catalog_begin` も同じ検査を持つ)。
7. **`meta` の書き込みが失敗しても、メモリ上の状態は進める**: `uploading` → `incomplete`(アイドル、書き込み失敗)、`finalizing` → `incomplete` / `invalid`(`FINISH` の失敗)は、`meta` の書き込みが失敗しても、メモリ上は遷移し、枠を空ける(ディスク上の `meta` は `uploading` のまま = 再起動で `incomplete`)。エラーは返す。
8. **`part_count` の照合**: `INDEX` は、`meta.part_count` が、`size_bytes` と `part_blocks` から計算した値と違えば `part_mismatch`、パーツが 16 個を超えても `part_mismatch`。
9. **エクステントの作業領域**: `ISO_MAX_EXTENTS + ISO_MAX_PARTS`(4112)本。`get_extents` の `max` は、残りの本数。**孤児の末尾のエクステントがパーツごとに 1 本ありうる**ので、16 本の余裕を持たせた。残りが足りない(`ISO_ERR_TOO_FRAGMENTED`)ときは `too_fragmented`。
10. **`iso_upload_hash_step` の失敗の返し方**: `DONE` を返し、`job->fail`(`INVALID` + 理由、または `IOERR` + エラー)に入れる。`ERROR` ではない(`iso_step_result_t` に、理由を持たせるのを避けた)。
11. **`receiving` / `idle` の境**: 経過 <= 5000 ms が `receiving`(「5 秒以内」)、アイドルのタイムアウトは経過 >= 300000 ms。時刻の巻き戻りは経過 0(`receiving`)。
12. **SHA-256 の状態は、イメージごとではなく 1 つ**(`state_size` の領域を呼び出し側が 1 つ渡す)。別のイメージの `begin` で、前のイメージの状態は上書きされる(前のイメージは、`finish` で読み直す)。
13. **既定の 1 GiB のパーツの境界のテストは、中身を捨てるモード**で行う(1 GiB を持てない)。パーツの境界の分割は、17 ブロックのパーツで網羅し、既定の大きさは、1 GiB ちょうどの 1 バイト手前、境界をまたぐチャンクだけ。
