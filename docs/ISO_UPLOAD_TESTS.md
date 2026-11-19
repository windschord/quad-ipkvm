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
| `test/host/iso_upload/test_up_begin.c` | `begin`: 検査の順序(400 → 枠 → 同名 → 上限 → 空き容量)、`size` の境界(34815 / 34816、16 GiB と +1)、空き容量の式の境界(`size + 64 MiB`、`total_bytes` 超、設計の数値例 `10 − 5 − (6 − 5) = 4 GiB`、途中のイメージの数え方)、キャッシュ未採取、`mkdir` / `meta` の失敗の巻き戻し | 17 |
| `test/host/iso_upload/test_up_write.c` | `iso_upload_write_chunk`: `offset == received` のときだけ書く(不一致は何も書かず、現在の `received`)、チャンクの上限(1000 / 1001、既定 128 KiB)、`size` を超える(34816 / +1)、状態ごとの拒否、**パーツの境界**(またぐチャンク、パーツ最後の 1 バイト、次のパーツの先頭、3 パーツにまたがる 1 チャンク、**既定の 1 GiB の境界**)、`sync` の間隔(`>=`、リセット、パーツの終わり、既定 4 MiB)、SHA-256 の逐次、アイドル(`IDLE_CLOSE`)、中止、再開(`meta` を書かない、`open_a` / `open_w`、`received` の決め直し、空き容量の再検査)、`receiving` / `idle` の境(5 秒) | 37 |
| `test/host/iso_upload/test_up_failures.c` | 書き込み・`sync`・`open` の失敗: チャンク全体を `truncate` で取り消して `incomplete`(部分書き込みつきの `ISO_ERR_NO_SPACE` は 507 相当)、パーツをまたぐチャンクの両方の取り消し、`truncate` が失敗したら `received` は実ファイルの大きさ(それも読めなければ書く前の値のまま、再開で決め直す)、`meta` の書き込みの失敗でもメモリ上は `incomplete`、再開の最初の `PUT` の失敗、`idle_close` の失敗 | 16 |
| `test/host/iso_upload/test_up_finish.c` | `finish` の受付(`size_mismatch`、枠、冪等、`invalid`、`finalizing`、取り消し)と、`FINISH` ジョブのステップ: 逐次の SHA-256 / 再起動後の全パーツの読み直し(`budget` ごと、読み出しの回数を手で数えた期待値)、宣言値の一致・不一致、署名(`CD001` / `BEA01` / 不正 / 後ろのパーツ)、エクステント(4096 本ちょうど、4100 本、作業領域の枯渇、`get_extents` の `TOO_FRAGMENTED`)、`part_mismatch`(欠落、短い、長い、`part_count` の食い違い)、I/O エラー(`incomplete` と再試行)、確保の失敗、`meta` の失敗、中止、古いジョブ、`epoch` の記録 | 57 |
| `test/host/iso_upload/test_up_index.c` | `iso_index_step`: 1 パーツずつ + 表の構築(ステップ数 = 初期化 + パーツ + 構築)、`iso_extent_map` で変換を確かめる、孤児の末尾のクリップ、`epoch`、`part_mismatch` / `too_fragmented` / I/O エラー / 確保の失敗 | 23 |
| `test/host/iso_upload/test_up_powercut.c` | **電源断の網羅**(下記) | 4 |
| `test/host/iso_upload/test_up_fuzz.c` | **固定シードのファズ**(下記) | 1 |
| `test/host/iso_catalog/iso_mock_fs.c` / `.h` | モックの `iso_fs_t` に、`read_at`、`open_for_write` / `write` / `sync` / `truncate` / `close`(未同期の分は電源断で失われる、書き込み中のファイルは読み取りで開けない)、`get_extents`、`mount_info` の設定、書き込みの失敗の注入(`mock_fs_fail_write`)、即時の電源断(`mock_fs_crash`)を足した(既存の関数は変えていない) | - |
| `test/host/iso_upload/up_common.h`、`up_sha256.c` / `.h` | 共通の部品(偽の時計、確保の数え上げ、決定的なデータの生成 `gen_byte`、ディスク上の中身の照合、SHA-256) | - |

合計 164 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルが 8 個増える)。

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

RESULTS_PLACEHOLDER

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
