# iso_core のテストとミューテーションテストの結果

作成: 2026-11-19(Issue #139、C-10 の後半)。`components/iso_core`(ワーカーのジョブの契約: キューと INDEX の集合、優先順位、同じイメージのジョブの合流、`iso_core_pick` / `iso_job_step`、`DELETE` の参照待ち、
公開ビューの更新、`iso_upload_tick`、SD の再初期化 `iso_core_sd_reinit`、起動 `iso_core_boot`。純 C。**ロックを持たない、スレッドを作らない。イベント・時間・ロックを待たない。ただし、シームの I/O(`write` / `sync` / `read` など)が終わるまでは、呼び出し側が待つ**)のホスト単体テストの構成と、`iso_core.c` に対するミューテーションテストの結果。
契約は `docs/ISO_MOUNT_DESIGN.md` §3.4(状態の遷移、SD の再初期化の手順)、§4.1(ロック)、§4.2(ワーカー)、§6.1、`docs/INTERFACES.md` §7.5。受信の状態機械と `FINISH` / `INDEX` のステップは `docs/ISO_UPLOAD_TESTS.md`。

## 1. 構成と、グルーへの契約

`components/iso_core/include/iso_core.h` の先頭のコメントが、**グルー(A-13 の `iso_worker`、A-15 の HTTP ハンドラー)への契約**(ロックの取り方、ワーカーのループ、SD の再初期化の呼び出し順、ビューのコールバックの規則)。要点:

- `iso_core_*`、`iso_upload_*`(`iso_core_upload()` が返すもの)、`iso_job_step`、`iso_upload_tick` は、`iso_op_mutex` を取って呼ぶ。`hooks.view_update` は `iso_op_mutex` の下で呼ばれ、実装は `iso_state_mutex` を取る(短く、I/O なし、`iso_core` / `iso_upload` を呼ばない)。ビューの `images[].table` は、保持するなら、コールバックの中で `iso_extent_table_ref` する。
- **SD の再初期化**: ① グルーが `iso_state_mutex` だけを取って(`sd.state` を ready 以外に、`iso_media_force_eject()`、`epoch_bump`)、放してから、`iso_op_mutex` を取って `iso_core_sd_reinit(core, remount, ctx)` を呼ぶ。中で、② ジョブの全取り消し → ③ 開いているファイルを捨てる → ④ アップロードを `incomplete` に、SHA-256 の状態を捨てる → ⑤ `remount`(グルーの `f_mount`)→ ⑥ カタログの再構築 → ⑦ `ready` のイメージごとに `INDEX`、`deleting` のイメージの `DELETE`。自動ではマウントしない。
- 戻り値は、`iso_res_t`(`iso_upload.h`。0 = 受理、`ISO_RES_JOINED` = 合流、正 = 拒否)と `iso_err_t`(負。キューのあふれは `ISO_ERR_BUSY`)。

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_core/test_core_queue.c` | 優先順位(`DELETE`、`FINISH`、`IDLE_CLOSE`、`INDEX`。同じ種類は入れた順)、後から入れた高い優先順位が先に動く、動いているジョブは `pick` が返し続ける、`INDEX` はキューに積まない(入れた順に 1 つずつ)、**キューは 8 でいっぱい、9 つ目は `ISO_ERR_BUSY`**(`DELETE` / `FINISH` は状態を変えずに取り消す。待っているジョブを置き換える分は数えない)、同じイメージの重複する投入の合流、`FINISH` と待っている `IDLE_CLOSE`、**`DELETE` が動いている `FINISH` / `INDEX` に中止の印**を立てて枠を空ける(別のイメージには立てない)、待っているジョブの取り消し、アップロード中の `DELETE`(ファイルを閉じ、SHA-256 を捨て、枠を空ける)、マウント中は拒否(何も変えない)、`meta_corrupt` の削除、**動いているジョブと同じ中身の別のオブジェクトを `iso_job_step` に渡しても、動いているジョブに触らない**、キューが満杯のときの `PUT` | 36 |
| `test/host/iso_core/test_core_jobs.c` | 各ジョブのステップ: `FINISH`(完了、I/O エラーで `incomplete` + 再試行、署名の誤りは DONE で `invalid`)、`INDEX`(表の公開、`part_mismatch` で `invalid` と古い表の解放(READ の参照は残る)、I/O エラーは `ready` の表なし、`meta` 書き込みの失敗)、`IDLE_CLOSE`、**`DELETE`**(`free_bytes` の更新、表なしは待たない、**参照待ち: 待つ間は何も消さない、30 秒(29999 ms / 30000 ms)で `ERROR(ISO_ERR_BUSY)` で `deleting` のまま、時刻の巻き戻りは経過 0**、I/O エラーと続き)、ビューの内容(状態の表示: `ready` で表がなければ `finalizing`、`sha256`、`hidden`、`meta_corrupt`、`sd_state`)、フックなしでも動く、**`IDLE_CLOSE` を積んだ後に `PUT` が成功したら、取り除く(選ばれた後なら、中止の印で何もしない)。受理されなかった `PUT` では取り除かない。受理された `PUT` は、選ばれた `INDEX` を中止しない** | 32 |
| `test/host/iso_core/test_core_reinit.c` | **SD の再初期化の順序**: remount の時点で、キュー・INDEX の集合・動いているジョブ・表・ファイルハンドル・枠・SHA-256 の状態が片づいていて、カタログは古いまま、`uploading` は `incomplete`、READ の参照は残る(遅延解放)。その後、カタログを作り直し、`ready` は INDEX の集合へ、`deleting` は `DELETE` へ、`free_bytes` を採り直し、ビューを更新する。remount / scan の失敗(空のカタログ)、二度の呼び出し、動いている `FINISH` / `DELETE` の取り消し。起動(`uploading` → `incomplete`、`deleting` の再開は最大 8 つ、前の状態を捨てる)、**表が 16 個ある状態の再初期化** | 14 |
| `test/host/iso_core/test_core_tick.c` | `iso_upload_tick`: **経過 299999 ms は積まず、300000 ms で `IDLE_CLOSE`**、重複しない、最後の `PUT` から数える、`finalizing` はタイムアウトしない、**時刻の巻き戻りは経過 0 で基準を引き直す**、キューがあふれたら次の `tick` で再試行、`receiving` → `idle` の境(5000 ms ちょうどは receiving、5001 ms で idle。変わったときだけビューを更新)、**時計の巻き戻しで、積んだ `IDLE_CLOSE` を取り消す(待っているもの、選ばれた後のもの、最後の `PUT` とちょうど同じ時刻)** | 14 |
| `test/host/iso_core/test_core_fuzz.c` | **固定シードのファズ**(下記) | 1 |
| `test/host/iso_core/core_common.h` | 共通の部品(`iso_core` の設定、フック、ワーカーのループの代わりの `drive()` / `run_job()`) | - |

合計 97 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルが 5 個増える)。期待値は手書き。

### ファズ(`test_core_fuzz.c`。固定シード、ASan / UBSan)

40 シード × 150 操作。`PUT`、`begin`、送り切って `finish`、`submit`(`FINISH` / `DELETE` / `IDLE_CLOSE` / `INDEX`)、**ワーカーのステップ(途中で止める。他の操作と交わる)**、`tick`(時刻の飛びと巻き戻り)、MSC の READ の参照の取得と解放、
書き込みの失敗・エラーの注入、電源断の予約、即時の電源断、SD の再初期化、起動。操作のたびに、不変条件を調べる:

- キュー: 長さ <= 8、同じイメージのジョブは 1 つ(動いているジョブと待っている `DELETE`、`IDLE_CLOSE` と `FINISH` の組だけ例外)、`INDEX` の集合に重複がない。
- エクステントの表: カタログの `ready`(または削除待ちの `deleting`)のイメージにだけあり、重複せず、参照 >= 1。
- 枠: `uploading` / `finalizing` は 1 つ以下、`iso_upload_t` の枠と一致、ハンドルは枠の持ち主のものだけ。
- ディスク(実装を使わずに読んだ基準): パーツの中身 = ソースの先頭、`ready` の SHA-256 = ソースから別に計算した値、`invalid` の理由は宣言値の食い違いと署名だけ(電源断や I/O の失敗で `part_mismatch` にならない)。
- ビュー(`iso_core_publish` の後): カタログと表の状態と一致する(`ready` で表がなければ `finalizing` の表示)。
- 最後に、注入と電源断を止め、参照を放して全部のジョブを流し切り、**静止した状態**(`ready` は全部、表がある。`deleting` は残らない。枠は空き、キューは空)を確かめる。

## 3. ミューテーションテスト

方法: `tools/test/mutate_iso_core.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_core.c` の 1 か所を置換 → 再ビルド → 5 個のテストの実行ファイルを順に実行、を繰り返す。ミュータントの定義(`tools/test/iso_core_mutants.py`)は、
`iso_upload` と同じく `tools/test/gen_iso_mutants.py` がソースから機械的に作る(`python3 tools/test/gen_iso_mutants.py core . > tools/test/iso_core_mutants.py`)。

```
python3 tools/test/mutate_iso_core.py <repo> <スクラッチ> tools/test/iso_core_mutants.py <結果.json> -j4
```

経緯: 最初の実行(490 個)は、殺した 407、生き残り 82、ビルドエラー 1 だった。生き残りを 1 つずつ調べ、**同値でないものには、テストを足した**(16 イメージ全部に表がある場合の取り外し、`FINALIZING` でジョブがない場合の積み直しと、あふれたときに受付を取り消さないこと、`chunk_bytes` / `sync_bytes` の引き渡し、`INDEX` の途中でカタログから消えた場合、終わったジョブのステップ、remount / scan の失敗で `hidden` を忘れること、など)。
**実装も単純にした**: `INDEX_OK` / `INDEX_IOERR` の `iso_catalog_apply`(状態も meta も変えない)、`submit_finish` の重複した `publish`、遷移の文脈の使われない欄を取り除いた。ミュータントの警告(括弧の省略、未使用の変数)は、ミュータントごとの `#pragma` で無効にした(`iso_upload` と同じ)。ソースから作り直した 480 個を、**最初から全数**実行した。最後に残った 1 つ(`K327`: scan が途中で失敗したとき `hidden` を 0 に戻す)は、テストを直した後に、個別に再実行して殺した(結果の JSON に反映)。

PR #155 のレビュー(`IDLE_CLOSE` の取り消しのフック `on_chunk_accepted`、ヘッダーの文言)でコードを変えた後、ミュータントをソースから作り直し、**最初から全数**実行した。
`iso_job_step` の検査の `||` → `&&` の 2 つのミュータントは、前の結果では、どちらも「終わったジョブを渡しても `default` の分岐が同じ結果を返す」として同値としていたが、
**そのうち `!c->cur_active || job != &c->cur` の方は、同値ではなかった**: 動いているジョブがあるときに、別のオブジェクトを渡すと、検査をすり抜け、別のオブジェクトのジョブを動かしてしまう。
`test_step_with_a_foreign_pointer_does_not_touch_the_running_job` を足して、殺した。残る 1 つ(`!job || !c->cur_active` → `&&`、K459)は、NULL が `job != &c->cur` で弾かれ、終わったジョブの渡しは `default` が同じ結果を返すので、同値。
`on_chunk_accepted` の `cur_active || ...`(K090)は、選ばれた `INDEX` を中止してしまうので、テスト `test_a_put_does_not_abort_a_picked_index_job` で殺した。

**再レビュー(同じ PR の 58bffcf に対する 8 件)の後の再実行**(最終、493 個): 前の結果で「配列の次の要素は 0 で埋まっている」として同値にまとめていた `<` → `<=`(`table_clear_all` の K036 など)は、**表が満杯(16 個)のときは、配列の外を読むので、同値ではなかった**。
満杯の入れ物のテストを足して、殺した: K036(`test_reinit_with_every_image_indexed_releases_all_sixteen_tables`)、K001(満杯の表で、ない id を探す)、K150(`test_a_put_with_a_full_queue_leaves_the_other_jobs_alone`)。
**そのために、`iso_core` / `iso_upload` のホストのビルドに `-fno-sanitize-recover=undefined` を足した**(それまでは、部品の側の UBSan の検出は表示されるだけだった)。
UBSan が検出できないもの(アドレスを取る形: `index_ids[i]`、`&images[i]`)は、同値にまとめず、理由に「範囲外の読み出しだけが違う」と書いた(K185、K210、K323)。K171 / K179 / K007 は、満杯のときに、その id を探すことが起きないので、同値。
`on_chunk_accepted` を時計の巻き戻しにも使うようにした(`now_ms <= last_put_ms`)ので、`K480` 相当(`<` と `<=`)は、境界のテスト `test_a_clock_rewound_exactly_to_the_last_put_cancels_a_queued_idle_close` で殺した。

**第 3 回の再レビューの後の再実行**(最終、493 個): iso_core のコードは変えていない(CMakeLists の C 標準の指定だけ)ので、結果は、第 2 回と同じ(433 / 59 / 1)。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー(コンパイラが検出) |
|---|---|---|---|
| 493 | 433 | 59 | 1 |

### 3.1 同値のミュータント(59 個)

生き残ったものは、1 つずつ、変更の前後の挙動が同じ理由を確かめた。同じ理由のものは、まとめた。

| ID | 同値の理由 |
|---|---|
| K007 | `table_remove` の走査の `<` → `<=`: 表が満杯(`table_n == ISO_MAX_IMAGES`)のとき、表のない id を消すことが起きない(満杯なら、カタログの全イメージが表を持つ)。満杯でなければ、`tables[table_n]` は 0 で埋まっていて、一致しない |
| K171, K179 | `index_has` / `index_drop` の走査の `<` → `<=`: INDEX の集合が満杯(`index_n == ISO_MAX_IMAGES`)のとき、集合にない id を探すことが起きない(満杯なら、カタログの全イメージが入っている)。満杯でなければ、`index_ids[index_n]` は 0 で埋まっていて、一致しない |
| K185 | `index_drop` の詰め直しの `k < index_n` → `<=`: 満杯のときだけ、`index_ids[index_n]`(配列の外。同じ構造体の次の欄の 9 バイト)を 1 つ前へ複写するが、直後の `index_n--` と `memset(index_ids[index_n])` で消す。**範囲外の読み出しだけが違い、サニタイザーは検出しない**(アドレスを取る形は、1 つ先を許す)。満杯の集合のテスト(`test_sixteen_images_all_get_tables_and_each_can_be_removed`)は通り、結果は変わらない |
| K210, K323 | `iso_core_mark_unindexed` / 再初期化の走査の `<` → `<=`: カタログが満杯のときだけ、`images[count]`(配列の外)の `state` を読む。**範囲外の読み出しだけが違い、サニタイザーは検出しない**(`&images[i]` のアドレスを取る形)。満杯のカタログのテストは通り、結果は変わらない。範囲外の値に依存するので、厳密な同値とは言わない(検出手段がない) |
| K022, K023, K039, K040, K102, K103, K126, K127, K196, K197, K199, K200, K301, K311, K314, K315, K318, K319, K367, K394 | 空いた要素や、使い終わった領域を 0 にする衛生の書き込み: `table_n` / `qn` / `index_n` / `count` より後ろは、読まれない(次に使うときに、`memcpy` / `memset` で全部書く)。`job_release` は、動いていないときは、0 の `iso_job_t` を放すだけ。`iso_upload_cfg_t uc` は、全部の欄を代入する |
| K031, K032, K055, K056, K080, K081, K163, K194, K207, K370, K371 | id のコピーの末尾の NUL(`ISO_ID_LEN + 1` → `+ 0` / `+ 2`): コピー先は、0 で埋めてあり(ビューは作るたびに `memset`、`queue` / `tables` / `cur` は空けるときと使うときに 0)、9 バイト目に 0 以外を書くことがない |
| K026, K027, K029, K201 | 到達しない分岐: カタログは 16 個までで、イメージごとに表は 1 つ(同じ id の表は、入れる前に取り除く)。`ISO_MAX_IMAGES` の溢れや、`NULL` の表の引き渡しは起きない |
| K251, K254, K255 | `iso_catalog_apply(ISO_EV_DELETE)` の拒否は、直前の遷移の検査(マウント中)で除かれている。`DELETE` のイベントは `meta` を書かない |
| K412, K414, K419, K421, K466, K468 | `rc == 0` の扱い: `iso_catalog_apply` / `iso_upload_idle_close` が成功(0)なら、`rc != 0 && 状態が ready` は偽(状態は invalid になっている)、`rc < 0 ? rc : ISO_OK` は、`rc <= 0` でも同じ値 |
| K448, K449, K450 | `del.phase = 1` は、参照待ちを終えた印だが、`phase == 0` のまま次のステップに入っても、表はすでに取り除かれていて(参照待ちは起きない)、同じ処理をもう一度するだけ |
| K434 | 時刻の巻き戻りの基準の引き直しの `<` → `<=`: 等しいときは、基準を同じ値で上書きするだけ |
| K459 | `iso_job_step` の検査の `!job \|\| !c->cur_active` → `&&`: NULL は、続く `job != &c->cur` で弾かれる。終わったジョブの `&c->cur`(`cur_active` が偽)を渡しても、`end_job` が `cur` を 0 にしてあり、種類は NONE なので、`default` の分岐が `ISO_ERR_INVALID` を返す。結果は同じ。(同じ行の `!cur_active \|\| job != &cur` → `&&` は、動いているジョブと同じ中身の別のオブジェクトを渡すテスト `test_step_with_a_foreign_pointer_does_not_touch_the_running_job` で殺した) |
| K475, K479 | `iso_upload_tick` の早期の return と、`finalizing` のときの `publish`: 枠がなければ、`IDLE_CLOSE` は NOT_FOUND で積まれず、表示は枠を空けたときに更新済み。`finalizing` の表示は、`finish` の受付で更新済み |
| K335 | `iso_catalog_scan` が、失敗する前に `count = 0` にしている(`rebuild` の失敗の分岐の重複) |
| K091 | `on_chunk_accepted` の `cur_active && kind == IDLE_CLOSE && id が同じ` の 2 つ目の `&&` → `\|\|`(`(cur_active && kind == IDLE_CLOSE) \|\| id が同じ`): 受理された `PUT` と同じ id の、別の種類の動いているジョブは存在しない(`FINISH` は枠を持ち、`PUT` は枠で拒否される。`INDEX` は `ready`、`DELETE` は `deleting` のイメージで、`PUT` は拒否される)。動いていないとき(`cur_active` が偽)の `aborted` は、読まれない(次の `pick` が `cur` を 0 にする) |

### 3.2 ビルドエラー(1 個。コンパイラが検出するので、殺したものとして数えない)

`-Werror` のコンパイル(配列の範囲外の読み出しや、使われなくなった変数など)で、ビルドできない変更。ミュータント側で、括弧の省略や未使用の変数の警告は無効にしてあるので、残ったものは、コンパイラが範囲外などの誤りとして検出したもの。

| ID | 変更箇所 |
|---|---|
| K192 | L196: const 1-1 -> 0 |

### 3.3 殺したミュータントの一覧(433 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(最初に落ちたテストの実行ファイルで止める)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| K001 | L15: < -> <= | `test_core_jobs:crash/rc=1` |
| K002 | L15: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K003 | L16: == -> != | `test_delete_of_the_mounted_image_is_refused_without_side_effects`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K004 | L16: negate if condition | `test_delete_of_the_mounted_image_is_refused_without_side_effects`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K005 | L16: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K006 | L16: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K008 | L22: const 0+1 -> 1 | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K009 | L23: != -> == | `test_priority_is_delete_finish_idle_close_index` |
| K010 | L23: negate if condition | `test_priority_is_delete_finish_idle_close_index` |
| K011 | L23: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index` |
| K012 | L23: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K013 | L24: delete statement | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K014 | L25: < -> <= | `test_core_jobs:crash/rc=1` |
| K015 | L25: + -> - | `test_core_jobs:crash/rc=1` |
| K016 | L25: - -> + | `test_core_jobs:crash/rc=1` |
| K017 | L25: const 1+1 -> 2 | `test_core_jobs:crash/rc=1` |
| K018 | L25: const 1-1 -> 0 | `test_core_jobs:crash/rc=1` |
| K019 | L25: const 1+1 -> 2 | `test_core_jobs:crash/rc=1` |
| K020 | L25: const 1-1 -> 0 | `test_core_jobs:crash/rc=1` |
| K021 | L26: delete statement | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K024 | L34: delete statement | `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K025 | L35: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K028 | L36: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K030 | L40: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K033 | L40: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K034 | L41: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K035 | L42: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K036 | L46: < -> <= | `test_core_reinit:crash/rc=1` |
| K037 | L46: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K038 | L47: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K041 | L50: const 0+1 -> 1 | `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K042 | L50: delete statement | `test_reinit_with_every_image_indexed_releases_all_sixteen_tables` |
| K043 | L60: const 0+1 -> 1 | `test_view_in_the_middle_of_a_finish_job`, `test_finish_job_with_a_wrong_signature_ends_invalid_with_done` |
| K044 | L60: delete statement | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table` |
| K045 | L61: delete statement | `test_view_contents`, `test_view_reports_the_sd_state_and_hidden_images` |
| K046 | L62: delete statement | `test_view_contents` |
| K047 | L63: delete statement | `test_view_contents` |
| K048 | L64: delete statement | `test_finish_job_end_to_end`, `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K049 | L65: delete statement | `test_view_reports_the_sd_state_and_hidden_images` |
| K050 | L66: delete statement | `test_view_contents` |
| K051 | L67: delete statement | `test_core_jobs:crash/rc=1` |
| K052 | L68: < -> <= | `test_core_jobs:crash/rc=1` |
| K053 | L68: const 0+1 -> 1 | `test_core_jobs:crash/rc=1` |
| K054 | L71: + -> - | `test_core_jobs:crash/rc=1` |
| K057 | L71: delete statement | `test_core_jobs:crash/rc=1` |
| K058 | L72: delete statement | `test_finish_job_end_to_end`, `test_view_in_the_middle_of_a_finish_job` |
| K059 | L73: delete statement | `test_finish_job_with_a_wrong_signature_ends_invalid_with_done`, `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K060 | L74: delete statement | `test_view_contents` |
| K061 | L75: delete statement | `test_finish_job_end_to_end`, `test_finish_job_io_error_becomes_incomplete_and_the_view_follows` |
| K062 | L76: negate if condition | `test_finish_job_end_to_end`, `test_view_contents` |
| K063 | L77: delete statement | `test_view_contents` |
| K064 | L78: delete statement | `test_view_contents` |
| K065 | L79: delete statement | `test_finish_job_end_to_end`, `test_view_contents` |
| K066 | L81: == -> != | `test_finish_job_end_to_end`, `test_finish_job_io_error_becomes_incomplete_and_the_view_follows` |
| K067 | L81: negate if condition | `test_finish_job_end_to_end`, `test_finish_job_io_error_becomes_incomplete_and_the_view_follows` |
| K068 | L82: delete statement | `test_finish_job_end_to_end`, `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K069 | L83: negate if condition | `test_finish_job_end_to_end`, `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K070 | L83: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_index_job_io_error_keeps_ready_without_a_table_and_can_be_retried` |
| K071 | L84: == -> != | `test_finish_job_end_to_end`, `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K072 | L84: && -> \|\| | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K073 | L84: negate if condition | `test_finish_job_end_to_end`, `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K074 | L85: true -> false | `test_finish_job_end_to_end` |
| K075 | L85: delete statement | `test_finish_job_end_to_end` |
| K076 | L86: delete statement | `test_finish_job_end_to_end` |
| K077 | L90: delete statement | `test_view_in_the_middle_of_a_finish_job` |
| K078 | L91: negate if condition | `test_view_in_the_middle_of_a_finish_job` |
| K079 | L91: + -> - | `test_view_in_the_middle_of_a_finish_job` |
| K082 | L91: delete statement | `test_view_in_the_middle_of_a_finish_job` |
| K083 | L92: delete statement | `test_view_in_the_middle_of_a_finish_job` |
| K084 | L93: delete statement | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked` |
| K085 | L94: negate if condition | `test_core_jobs:crash/rc=1` |
| K086 | L94: delete statement | `test_core_jobs:crash/rc=1` |
| K087 | L107: delete statement | `test_a_put_after_the_idle_close_was_queued_cancels_it` |
| K088 | L108: == -> != | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K089 | L108: == -> != | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K090 | L108: && -> \|\| | `test_a_put_does_not_abort_a_picked_index_job` |
| K092 | L108: negate if condition | `test_a_put_does_not_abort_a_picked_index_job`, `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K093 | L108: const 0+1 -> 1 | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K094 | L108: true -> false | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K095 | L108: delete statement | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K096 | L114: \|\| -> && | `test_core_queue:crash/rc=1` |
| K097 | L114: negate if condition | `test_core_queue:crash/rc=1` |
| K098 | L114: delete statement | `test_core_queue:crash/rc=1` |
| K099 | L115: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K100 | L115: delete statement | `test_index_jobs_do_not_take_queue_slots`, `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K101 | L116: delete statement | `test_core_queue:crash/rc=1` |
| K104 | L119: delete statement | `test_core_queue:crash/rc=1` |
| K105 | L120: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K106 | L121: delete statement | `test_core_queue:crash/rc=1` |
| K107 | L122: delete statement | `test_core_queue:crash/rc=1` |
| K108 | L123: delete statement | `test_core_queue:crash/rc=1` |
| K109 | L124: delete statement | `test_core_queue:crash/rc=1` |
| K110 | L125: delete statement | `test_core_queue:crash/rc=1` |
| K111 | L126: delete statement | `test_core_queue:crash/rc=1` |
| K112 | L127: delete statement | `test_config_chunk_and_sync_sizes_reach_the_upload` |
| K113 | L128: delete statement | `test_config_chunk_and_sync_sizes_reach_the_upload` |
| K114 | L129: delete statement | `test_core_queue:crash/rc=1` |
| K115 | L130: delete statement | `test_view_in_the_middle_of_a_finish_job` |
| K116 | L131: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K117 | L132: delete statement | `test_a_put_after_the_idle_close_was_queued_cancels_it`, `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K118 | L143: < -> <= | `test_core_queue:crash/rc=1` |
| K119 | L143: + -> - | `test_core_queue:crash/rc=1` |
| K120 | L143: - -> + | `test_core_queue:crash/rc=1` |
| K121 | L143: const 1+1 -> 2 | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K122 | L143: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K123 | L143: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K124 | L143: const 1-1 -> 0 | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K125 | L144: delete statement | `test_core_queue:timeout` |
| K128 | L149: < -> <= | `test_core_queue:crash/rc=1` |
| K129 | L149: const 0+1 -> 1 | `test_fuzz_random_operations_across_the_worker_contract` |
| K130 | L150: == -> != | `test_overflow_of_delete_changes_nothing`, `test_duplicate_submissions_join_the_existing_job` |
| K131 | L150: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K132 | L150: && -> \|\| | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K133 | L150: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K134 | L150: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job` |
| K135 | L150: true -> false | `test_duplicate_submissions_join_the_existing_job` |
| K136 | L150: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K137 | L152: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K138 | L157: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K139 | L158: < -> <= | `test_core_queue:crash/rc=1` |
| K140 | L158: const 0+1 -> 1 | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K141 | L159: == -> != | `test_overflow_of_delete_changes_nothing` |
| K142 | L159: == -> != | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K143 | L159: == -> != | `test_core_queue:crash/rc=1` |
| K144 | L159: && -> \|\| | `test_core_queue:crash/rc=1` |
| K145 | L159: \|\| -> && | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K146 | L159: negate if condition | `test_core_queue:crash/rc=1` |
| K147 | L159: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K148 | L159: delete statement | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K149 | L166: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K150 | L167: < -> <= | `test_core_queue:crash/rc=1` |
| K151 | L168: == -> != | `test_core_queue:crash/rc=1` |
| K152 | L168: == -> != | `test_core_queue:crash/rc=1` |
| K153 | L168: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K154 | L168: && -> \|\| | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K155 | L168: \|\| -> && | `test_core_queue:crash/rc=1` |
| K156 | L168: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K157 | L168: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_overflow_of_delete_changes_nothing` |
| K158 | L169: delete statement | `test_core_queue:timeout` |
| K159 | L171: delete statement | `test_core_queue:timeout` |
| K160 | L177: delete statement | `test_core_queue:crash/rc=1` |
| K161 | L178: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K162 | L178: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K164 | L178: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K165 | L179: delete statement | `test_core_queue:crash/rc=1` |
| K166 | L183: == -> != | `test_running_job_absorbs_duplicates`, `test_delete_aborts_the_running_finish_job_of_the_same_image` |
| K167 | L183: == -> != | `test_running_job_absorbs_duplicates`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K168 | L183: && -> \|\| | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K169 | L183: && -> \|\| | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K170 | L183: const 0+1 -> 1 | `test_running_job_absorbs_duplicates`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K172 | L187: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K173 | L188: == -> != | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K174 | L188: negate if condition | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K175 | L188: const 0+1 -> 1 | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K176 | L188: true -> false | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K177 | L188: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K178 | L190: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K180 | L194: const 0+1 -> 1 | `test_core_queue:timeout` |
| K181 | L195: != -> == | `test_core_queue:timeout` |
| K182 | L195: negate if condition | `test_core_queue:timeout` |
| K183 | L195: const 0+1 -> 1 | `test_core_queue:timeout` |
| K184 | L195: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K186 | L196: + -> - | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K187 | L196: - -> + | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K188 | L196: + -> - | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K189 | L196: const 1+1 -> 2 | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K190 | L196: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K191 | L196: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K193 | L196: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K195 | L197: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K198 | L198: const 1+1 -> 2 | `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K202 | L204: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K203 | L204: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K204 | L204: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K205 | L205: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K206 | L205: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K208 | L205: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K209 | L206: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K211 | L210: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K212 | L212: != -> == | `test_core_queue:crash/rc=1` |
| K213 | L212: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K214 | L212: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K215 | L212: negate if condition | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K216 | L212: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K217 | L213: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K218 | L223: == -> != | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K219 | L223: negate if condition | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K220 | L224: \|\| -> && | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K221 | L224: negate if condition | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K222 | L224: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K223 | L225: delete statement | `test_a_finalizing_image_without_a_job_gets_a_job_again`, `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K224 | L226: delete statement | `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K225 | L228: != -> == | `test_core_queue:crash/rc=1` |
| K226 | L228: negate if condition | `test_core_queue:crash/rc=1` |
| K227 | L228: delete statement | `test_delete_of_a_missing_image_is_not_found` |
| K228 | L229: >= -> > | `test_core_queue:crash/rc=1` |
| K229 | L229: negate if condition | `test_core_queue:crash/rc=1` |
| K230 | L229: - -> + | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K231 | L230: != -> == | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K232 | L230: negate if condition | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K233 | L230: delete statement | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K234 | L233: delete statement | `test_core_queue:crash/rc=1` |
| K235 | L234: delete statement | `test_core_queue:crash/rc=1` |
| K236 | L240: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K237 | L240: \|\| -> && | `test_core_queue:crash/rc=1` |
| K238 | L240: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K239 | L240: delete statement | `test_core_queue:crash/rc=1` |
| K240 | L241: \|\| -> && | `test_duplicate_submissions_join_the_existing_job` |
| K241 | L241: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K242 | L241: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K243 | L244: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K244 | L244: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K245 | L244: delete statement | `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K246 | L245: >= -> > | `test_core_queue:crash/rc=1` |
| K247 | L245: negate if condition | `test_core_queue:crash/rc=1` |
| K248 | L245: - -> + | `test_overflow_of_delete_changes_nothing` |
| K249 | L245: delete statement | `test_core_queue:crash/rc=1` |
| K250 | L248: != -> == | `test_core_queue:crash/rc=1` |
| K252 | L248: negate if condition | `test_core_queue:crash/rc=1` |
| K253 | L248: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K256 | L249: delete statement | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K257 | L250: delete statement | `test_core_queue:crash/rc=1` |
| K258 | L251: delete statement | `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K259 | L252: == -> != | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K260 | L252: && -> \|\| | `test_delete_of_another_image_does_not_abort_the_running_job` |
| K261 | L252: negate if condition | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K262 | L252: const 0+1 -> 1 | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K263 | L252: true -> false | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K264 | L252: delete statement | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K265 | L253: delete statement | `test_core_queue:crash/rc=1` |
| K266 | L254: delete statement | `test_core_jobs:crash/rc=1` |
| K267 | L264: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K268 | L264: \|\| -> && | `test_core_queue:crash/rc=1` |
| K269 | L264: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K270 | L264: delete statement | `test_delete_of_a_missing_image_is_not_found` |
| K271 | L265: < -> <= | `test_core_queue:crash/rc=1` |
| K272 | L265: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K273 | L266: == -> != | `test_index_jobs_do_not_take_queue_slots`, `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K274 | L266: negate if condition | `test_index_jobs_do_not_take_queue_slots`, `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K275 | L266: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K276 | L266: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K277 | L268: == -> != | `test_queue_holds_8_jobs_and_the_9th_is_busy`, `test_running_job_absorbs_duplicates` |
| K278 | L268: && -> \|\| | `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K279 | L268: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K280 | L268: const 0+1 -> 1 | `test_running_job_absorbs_duplicates` |
| K281 | L268: delete statement | `test_running_job_absorbs_duplicates` |
| K282 | L269: >= -> > | `test_core_queue:crash/rc=1` |
| K283 | L269: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K284 | L269: delete statement | `test_core_queue:crash/rc=1` |
| K285 | L270: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_do_not_take_queue_slots` |
| K286 | L275: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K287 | L275: \|\| -> && | `test_core_queue:crash/rc=1` |
| K288 | L275: negate if condition | `test_core_queue:crash/rc=1` |
| K289 | L275: delete statement | `test_core_queue:crash/rc=1` |
| K290 | L276: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K291 | L276: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K292 | L276: delete statement | `test_index_submission_requires_a_ready_image` |
| K293 | L277: == -> != | `test_core_reinit:crash/rc=1` |
| K294 | L277: && -> \|\| | `test_core_reinit:crash/rc=1` |
| K295 | L277: \|\| -> && | `test_duplicate_submissions_join_the_existing_job` |
| K296 | L277: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K297 | L277: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K298 | L277: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K299 | L278: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K300 | L288: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K302 | L289: == -> != | `test_core_reinit:crash/rc=1` |
| K303 | L289: negate if condition | `test_core_reinit:crash/rc=1` |
| K304 | L289: delete statement | `test_core_reinit:crash/rc=1` |
| K305 | L290: == -> != | `test_delete_aborts_the_running_index_job` |
| K306 | L290: negate if condition | `test_delete_aborts_the_running_index_job` |
| K307 | L290: delete statement | `test_delete_aborts_the_running_index_job` |
| K308 | L291: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K309 | L291: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K310 | L292: const 0+1 -> 1 | `test_delete_job_without_a_table_does_not_wait` |
| K312 | L297: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K313 | L297: delete statement | `test_core_reinit:crash/rc=1` |
| K316 | L299: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K317 | L299: delete statement | `test_core_reinit:crash/rc=1` |
| K320 | L301: delete statement | `test_core_reinit:crash/rc=1` |
| K321 | L302: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K322 | L303: delete statement | `test_idle_close_joins_a_waiting_finish_of_the_same_image`, `test_a_put_with_a_full_queue_leaves_the_other_jobs_alone` |
| K324 | L308: const 0+1 -> 1 | `test_reinit_resumes_deleting_images` |
| K325 | L310: == -> != | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K326 | L310: < -> <= | `test_core_reinit:crash/rc=1` |
| K327 | L310: && -> \|\| | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K328 | L310: negate if condition | `test_core_queue:crash/rc=1` |
| K329 | L310: delete statement | `test_reinit_cancels_everything_before_remount_and_rebuilds_after`, `test_reinit_resumes_deleting_images` |
| K330 | L312: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K331 | L317: < -> <= | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K332 | L317: negate if condition | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K333 | L317: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K334 | L318: const 0+1 -> 1 | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_boot_scan_error_is_returned` |
| K336 | L319: const 0+1 -> 1 | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K337 | L319: delete statement | `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K338 | L320: delete statement | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K339 | L323: delete statement | `test_view_contents` |
| K340 | L324: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K341 | L325: delete statement | `test_core_jobs:crash/rc=1` |
| K342 | L330: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K343 | L335: delete statement | `test_core_reinit:crash/rc=1` |
| K344 | L336: negate if condition | `test_core_reinit:crash/rc=1` |
| K345 | L338: < -> <= | `test_core_reinit:crash/rc=1` |
| K346 | L338: negate if condition | `test_core_reinit:crash/rc=1` |
| K347 | L338: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K348 | L339: const 0+1 -> 1 | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs` |
| K349 | L339: delete statement | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs` |
| K350 | L340: const 0+1 -> 1 | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K351 | L340: delete statement | `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K352 | L341: delete statement | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K353 | L351: < -> <= | `test_core_queue:crash/rc=1` |
| K354 | L351: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K355 | L352: == -> != | `test_core_queue:crash/rc=1` |
| K356 | L352: negate if condition | `test_core_queue:crash/rc=1` |
| K357 | L352: delete statement | `test_core_queue:crash/rc=1` |
| K358 | L358: negate if condition | `test_pick_with_nothing_queued_returns_null`, `test_priority_is_delete_finish_idle_close_index` |
| K359 | L358: delete statement | `test_pick_returns_the_running_job_until_it_ends`, `test_step_with_a_foreign_pointer_does_not_touch_the_running_job` |
| K360 | L360: < -> <= | `test_core_queue:crash/rc=1` |
| K361 | L360: / -> * | `test_core_queue:crash/rc=1` |
| K362 | L360: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K363 | L362: >= -> > | `test_core_queue:crash/rc=1` |
| K364 | L362: negate if condition | `test_core_queue:crash/rc=1` |
| K365 | L362: delete statement | `test_core_queue:crash/rc=1` |
| K366 | L363: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K368 | L364: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K369 | L365: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K372 | L365: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K373 | L366: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K374 | L367: true -> false | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K375 | L367: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K376 | L368: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K377 | L368: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K378 | L368: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K379 | L369: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K380 | L369: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K381 | L369: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K382 | L372: > -> >= | `test_core_queue:timeout` |
| K383 | L372: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K384 | L373: + -> - | `test_core_queue:crash/rc=1` |
| K385 | L373: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K386 | L373: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K387 | L374: delete statement | `test_core_queue:timeout` |
| K388 | L375: delete statement | `test_core_queue:timeout` |
| K389 | L377: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K390 | L377: \|\| -> && | `test_index_pick_skips_images_that_are_no_longer_ready` |
| K391 | L377: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K392 | L377: delete statement | `test_index_pick_skips_images_that_are_no_longer_ready` |
| K393 | L378: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K395 | L379: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K396 | L380: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K397 | L381: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K398 | L382: true -> false | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K399 | L382: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K400 | L388: && -> \|\| | `test_core_queue:crash/rc=1` |
| K401 | L391: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K402 | L392: delete statement | `test_core_jobs:crash/rc=1` |
| K403 | L399: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K404 | L399: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K405 | L399: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K406 | L401: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K407 | L401: delete statement | `test_index_job_whose_image_left_the_catalog_ends_done` |
| K408 | L405: delete statement | `test_core_queue:crash/rc=1` |
| K409 | L406: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K410 | L412: == -> != | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K411 | L412: != -> == | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K413 | L412: negate if condition | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K415 | L413: delete statement | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K416 | L414: delete statement | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K417 | L416: delete statement | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K418 | L417: < -> <= | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K420 | L417: const 0+1 -> 1 | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K422 | L425: == -> != | `test_core_jobs:crash/rc=1` |
| K423 | L425: negate if condition | `test_core_jobs:crash/rc=1` |
| K424 | L425: const 0+1 -> 1 | `test_core_jobs:crash/rc=1` |
| K425 | L427: > -> >= | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K426 | L427: && -> \|\| | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K427 | L427: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K428 | L427: const 1+1 -> 2 | `test_core_jobs:crash/rc=1` |
| K429 | L427: const 1-1 -> 0 | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K430 | L429: negate if condition | `test_core_jobs:crash/rc=1` |
| K431 | L430: true -> false | `test_core_jobs:crash/rc=1` |
| K432 | L430: delete statement | `test_core_jobs:crash/rc=1` |
| K433 | L431: delete statement | `test_core_jobs:crash/rc=1` |
| K435 | L433: negate if condition | `test_core_jobs:crash/rc=1` |
| K436 | L433: delete statement | `test_core_jobs:crash/rc=1` |
| K437 | L434: >= -> > | `test_core_jobs:crash/rc=1` |
| K438 | L434: negate if condition | `test_core_jobs:crash/rc=1` |
| K439 | L434: - -> + | `test_core_jobs:crash/rc=1` |
| K440 | L434: * -> / | `test_core_jobs:crash/rc=1` |
| K441 | L434: const 1000+1 -> 1001 | `test_core_jobs:crash/rc=1` |
| K442 | L434: const 1000-1 -> 999 | `test_core_jobs:crash/rc=1` |
| K443 | L437: true -> false | `test_core_jobs:crash/rc=1` |
| K444 | L437: delete statement | `test_core_jobs:crash/rc=1` |
| K445 | L440: false -> true | `test_delete_job_without_a_table_does_not_wait`, `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table` |
| K446 | L440: delete statement | `test_delete_job_waits_for_readers_then_proceeds` |
| K447 | L441: delete statement | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K451 | L445: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K452 | L445: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K453 | L445: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K454 | L446: == -> != | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K455 | L446: negate if condition | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K456 | L446: delete statement | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K457 | L451: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K458 | L451: \|\| -> && | `test_core_queue:crash/rc=1` |
| K460 | L451: \|\| -> && | `test_core_queue:crash/rc=1` |
| K461 | L451: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K462 | L451: delete statement | `test_core_queue:crash/rc=1` |
| K463 | L454: negate if condition | `test_idle_close_job_makes_the_upload_incomplete`, `test_idle_close_job_error` |
| K464 | L454: delete statement | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K465 | L456: < -> <= | `test_priority_is_delete_finish_idle_close_index` |
| K467 | L456: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index` |
| K469 | L460: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K470 | L460: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K471 | L460: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K472 | L464: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K473 | L464: delete statement | `test_delete_aborts_the_running_index_job` |
| K474 | L475: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K476 | L476: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K477 | L477: != -> == | `test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked` |
| K478 | L477: negate if condition | `test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked` |
| K480 | L480: <= -> < | `test_a_clock_rewound_exactly_to_the_last_put_cancels_a_queued_idle_close` |
| K481 | L480: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K482 | L481: delete statement | `test_clock_going_backwards_counts_as_zero_elapsed_and_rebases`, `test_a_clock_rewind_cancels_a_queued_idle_close` |
| K483 | L482: delete statement | `test_a_clock_rewind_cancels_a_queued_idle_close`, `test_a_clock_rewind_aborts_a_picked_idle_close` |
| K484 | L484: - -> + | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K485 | L485: >= -> > | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K486 | L485: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K487 | L485: * -> / | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K488 | L485: const 1000+1 -> 1001 | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K489 | L485: const 1000-1 -> 999 | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K490 | L485: delete statement | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K491 | L486: != -> == | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |
| K492 | L486: negate if condition | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |
| K493 | L486: delete statement | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |

## 4. 設計に書かれていないこと、設計と違うことを、実装で決めた箇所(レビューで確認してほしい)

1. **`DELETE` の「動いているジョブに中止の印」は、同じイメージのジョブだけ**。別のイメージの `FINISH` / `INDEX` を中止しない(設計の文面は、どのジョブとも読める)。同じイメージの待っている `FINISH` / `IDLE_CLOSE` / `INDEX` は取り消す。
2. **ジョブは最後まで動く(横取りしない)**: 数分かかる `INDEX`(16 パーツで十数秒)が動いている間に `FINISH` が入っても、`INDEX` が終わるまで待つ(パーツごとのステップの間で、`pick` は同じジョブを返し続ける)。横取りが必要なら、`INDEX` を集合に戻す形に変える(提案)。
3. **キューのあふれの数え方**: 同じイメージの待っているジョブを置き換える `DELETE` / `FINISH` は、その分を引いて数える。**あふれたら、状態を変えない**(`FINISH` は受付を取り消す(`iso_upload_finish_undo`))。
4. **`iso_upload_tick`(INTERFACES 7.5 の名前)は `iso_core.h` にあり、`iso_core_t` を取る**。時刻は引数 `now_ms`(注入の時計ではなく、呼び出し側が渡す)。**時刻の巻き戻りは、基準(最後の `PUT` の時刻)を `now_ms` に引き直す**(巻き戻った分だけ、タイムアウトが延びるのを避ける)。`DELETE` の参照待ちの基準も同じ。
5. **`iso_core_sd_reinit` は、`remount` のコールバックを引数に取る**(設計 3.4 の ⑤ `f_mount` が、④ と ⑥ の間にあるため)。① はグルーの仕事で、`iso_core` は触らない。remount か scan が失敗したら、カタログは空、ジョブなしで返る(グルーが `sd.state` を `error` にして、後でもう一度呼ぶ)。
6. **起動時の `deleting` の再開は、最大 `ISO_WORKER_QUEUE`(8)個**。あふれた分は `deleting` のまま残り、再度の `DELETE` か次の起動で続ける(自動の再試行は、ワーカーが `ERROR` のイメージを際限なく試すのを避けるため、しない)。
7. **ビューに、マウント中のイメージ(`iso_media`)は含まない**(`iso_media` が持つ。`GET` は、両方を `iso_state_mutex` の下で組み立てる)。`ready` でエクステントの表がないイメージは、`finalizing` と表示する(`INDEX` 待ち・中。枠は使わない)。ビューの `images[].table` は、コールバックの間だけ有効。
8. **ビューの更新のタイミング**: 状態が変わったとき(`submit`、`iso_upload` の遷移、ジョブの終わり、`boot` / `sd_reinit` の終わり、`tick` で `receiving` ↔ `idle` が変わったとき)。ジョブの途中のステップ(`CONTINUE`)では更新しない。`PUT` のたびに(`received` が進むので)更新する。
9. **`DELETE` の完了と `FINISH` の完了で、`free_bytes` のキャッシュを採り直す**(`mount_info`。失敗したら、古い値のまま)。**`INDEX` の完了では採り直さない**。
10. **未実装**: `DELETE` の前の `crosslinked` の照合(`import/` の同じ先頭クラスタ。設計 3.3)は、`iso_catalog_delete_precheck`(C-10a)が持つ。**`iso_core_submit_delete` は呼ばない**(C-12 の `iso_api` が、`iso_core_submit_delete` の前に呼ぶ。呼ばなくても、削除のステップの最初が、同じ照合をして `ERROR(crosslinked)` で止まる)。`rescan` の呼び出しと、その後の `iso_core_mark_unindexed` は、C-12。
11. **`IDLE_CLOSE` を積んだ後に `PUT` が成功したら、閉じない(PR #155 のレビュー。設計 3.4 へ反映済み)**: `iso_upload_tick` が `IDLE_CLOSE` を積んでから、ワーカーが実行するまでの間に、受理された `PUT` があると、以前は、最近受信した枠を閉じて(`incomplete`)しまい、クライアントの次のチャンクが `upload_in_progress` ではなく再開の経路になり、ファイルの開き直しと `sync` が余計にかかった。
    **修正**: `iso_upload_write_chunk` が成功したとき、`chunk_accepted` フックを呼ぶ。`iso_core` は、(a) キューに待っている同じイメージの `IDLE_CLOSE` を取り除く、(b) すでに選ばれて(`iso_core_pick`)ステップ前の同じイメージの `IDLE_CLOSE` には、中止の印を立てる(印のある `IDLE_CLOSE` のステップは、何もせずに `DONE`)。実行の時刻に経過時間を見直す方法(実行時の判定)も考えたが、**フックの方が、時計の巻き戻しや、`iso_upload_tick` の境界と二重に判定を持たずに済む**ので採らなかった。`iso_upload_idle_close` の直接の呼び出し(SD のエラーからの復帰、`iso_core` の `IDLE_CLOSE` ステップ)は、強制の閉じのまま。受理されなかった `PUT` は、取り除かない。
    テスト: `test_a_put_after_the_idle_close_was_queued_cancels_it`、`test_a_put_after_the_idle_close_was_picked_aborts_it`、`test_a_rejected_put_does_not_cancel_the_queued_idle_close`。
12. **時計の巻き戻しで、タイムアウト由来の `IDLE_CLOSE` を取り消す(PR #155 の再レビュー)**: `iso_upload_tick` が、`last_put_ms` を、巻き戻った時計の値に引き直したとき、**巻き戻る前の時計で積んだ(経過 >= 300 秒と判定した)同じイメージの `IDLE_CLOSE` を、待っているものは取り除き、選ばれた後(実行前)のものには中止の印を立てる**(`on_chunk_accepted` と同じ処理)。**巻き戻り先が、最後の PUT の時刻とちょうど同じ(`now <= last_put_ms`)でも、同じ**(経過 0 で、古い時計で積んだ閉じは前提が崩れている)。引き直した後の経過は 0 なので、すでに積んだ閉じは、前提が崩れている。**強制の閉じ**(`iso_upload_idle_close` の直接の呼び出し)は、残す。
    テスト: `test_a_clock_rewind_cancels_a_queued_idle_close`(取り消した後に、強制の閉じは効く)、`test_a_clock_rewind_aborts_a_picked_idle_close`、`test_a_clock_rewound_exactly_to_the_last_put_cancels_a_queued_idle_close`。
13. **ステップ 1 回の長さ**: `iso_core.h` の「FatFs の 1 回の呼び出し程度」は、上限に見えるので、直した。1 回のステップは、複数の同期 I/O を順に行うことがある(`IDLE_CLOSE` は、パーツの `sync` と `close` の後に `meta` を書く)。グルーは、`iso_op_mutex` の保持時間を、ステップ 1 回の中の I/O をすべて数えて見積もる。
