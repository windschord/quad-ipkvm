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
| `test/host/iso_core/test_core_queue.c` | 優先順位(`DELETE`、`FINISH`、`IDLE_CLOSE`、`INDEX`。同じ種類は入れた順)、後から入れた高い優先順位が先に動く、動いているジョブは `pick` が返し続ける、`INDEX` はキューに積まない(入れた順に 1 つずつ)、**キューは 8 でいっぱい、9 つ目は `ISO_ERR_BUSY`**(`DELETE` / `FINISH` は状態を変えずに取り消す。待っているジョブを置き換える分は数えない)、同じイメージの重複する投入の合流、`FINISH` と待っている `IDLE_CLOSE`、**`DELETE` が動いている `FINISH` / `INDEX` に中止の印**を立てて枠を空ける(別のイメージには立てない)、待っているジョブの取り消し、アップロード中の `DELETE`(ファイルを閉じ、SHA-256 を捨て、枠を空ける)、マウント中は拒否(何も変えない)、`meta_corrupt` の削除、**動いているジョブと同じ中身の別のオブジェクトを `iso_job_step` に渡しても、動いているジョブに触らない** | 35 |
| `test/host/iso_core/test_core_jobs.c` | 各ジョブのステップ: `FINISH`(完了、I/O エラーで `incomplete` + 再試行、署名の誤りは DONE で `invalid`)、`INDEX`(表の公開、`part_mismatch` で `invalid` と古い表の解放(READ の参照は残る)、I/O エラーは `ready` の表なし、`meta` 書き込みの失敗)、`IDLE_CLOSE`、**`DELETE`**(`free_bytes` の更新、表なしは待たない、**参照待ち: 待つ間は何も消さない、30 秒(29999 ms / 30000 ms)で `ERROR(ISO_ERR_BUSY)` で `deleting` のまま、時刻の巻き戻りは経過 0**、I/O エラーと続き)、ビューの内容(状態の表示: `ready` で表がなければ `finalizing`、`sha256`、`hidden`、`meta_corrupt`、`sd_state`)、フックなしでも動く、**`IDLE_CLOSE` を積んだ後に `PUT` が成功したら、取り除く(選ばれた後なら、中止の印で何もしない)。受理されなかった `PUT` では取り除かない。受理された `PUT` は、選ばれた `INDEX` を中止しない** | 32 |
| `test/host/iso_core/test_core_reinit.c` | **SD の再初期化の順序**: remount の時点で、キュー・INDEX の集合・動いているジョブ・表・ファイルハンドル・枠・SHA-256 の状態が片づいていて、カタログは古いまま、`uploading` は `incomplete`、READ の参照は残る(遅延解放)。その後、カタログを作り直し、`ready` は INDEX の集合へ、`deleting` は `DELETE` へ、`free_bytes` を採り直し、ビューを更新する。remount / scan の失敗(空のカタログ)、二度の呼び出し、動いている `FINISH` / `DELETE` の取り消し。起動(`uploading` → `incomplete`、`deleting` の再開は最大 8 つ、前の状態を捨てる) | 13 |
| `test/host/iso_core/test_core_tick.c` | `iso_upload_tick`: **経過 299999 ms は積まず、300000 ms で `IDLE_CLOSE`**、重複しない、最後の `PUT` から数える、`finalizing` はタイムアウトしない、**時刻の巻き戻りは経過 0 で基準を引き直す**、キューがあふれたら次の `tick` で再試行、`receiving` → `idle` の境(5000 ms ちょうどは receiving、5001 ms で idle。変わったときだけビューを更新) | 11 |
| `test/host/iso_core/test_core_fuzz.c` | **固定シードのファズ**(下記) | 1 |
| `test/host/iso_core/core_common.h` | 共通の部品(`iso_core` の設定、フック、ワーカーのループの代わりの `drive()` / `run_job()`) | - |

合計 92 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルが 5 個増える)。期待値は手書き。

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

PR #155 のレビュー(`IDLE_CLOSE` の取り消しのフック `on_chunk_accepted`、ヘッダーの文言)でコードを変えた後、ミュータントをソースから作り直し(492 個)、**最初から全数**実行した。
`iso_job_step` の検査の `||` → `&&` の 2 つのミュータントは、前の結果では、どちらも「終わったジョブを渡しても `default` の分岐が同じ結果を返す」として同値としていたが、
**そのうち `!c->cur_active || job != &c->cur` の方は、同値ではなかった**: 動いているジョブがあるときに、別のオブジェクトを渡すと、検査をすり抜け、別のオブジェクトのジョブを動かしてしまう。
`test_step_with_a_foreign_pointer_does_not_touch_the_running_job` を足して、殺した(K460)。残る 1 つ(`!job || !c->cur_active` → `&&`、K459)は、NULL が `job != &c->cur` で弾かれ、終わったジョブの渡しは `default` が同じ結果を返すので、同値。
`on_chunk_accepted` の `cur_active || ...`(K090)は、選ばれた `INDEX` を中止してしまうので、テスト `test_a_put_does_not_abort_a_picked_index_job` で殺した。同値のものは、前の 63 個から、K449 / K450 の 2 個が 1 個になり、新しい `K091` が 1 個増えて、63 個。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー(コンパイラが検出) |
|---|---|---|---|
| 492 | 428 | 63 | 1 |

### 3.1 同値のミュータント(63 個)

生き残ったものは、1 つずつ、変更の前後の挙動が同じ理由を確かめた。同じ理由のものは、まとめた。

| ID | 同値の理由 |
|---|---|
| K001, K007, K036, K150, K171, K179, K185, K210, K323 | 走査・移動の `<` → `<=`: 最後の次の要素(`tables[table_n]`、`queue[qn]`、`index_ids[index_n]`、`images[count]`)は 0 で埋まっていて、一致も、状態も、表もない(`strcmp` は一致せず、`unref(NULL)` は 0、状態なしは読み飛ばす)。詰める移動が 1 つ多く読んでも、直後の `memset` で消す |
| K022, K023, K039, K040, K102, K103, K126, K127, K196, K197, K199, K200, K301, K311, K314, K315, K318, K319, K367, K394 | 空いた要素や、使い終わった領域を 0 にする衛生の書き込み: `table_n` / `qn` / `index_n` / `count` より後ろは、読まれない(次に使うときに、`memcpy` / `memset` で全部書く)。`job_release` は、動いていないときは、0 の `iso_job_t` を放すだけ。`iso_upload_cfg_t uc` は、全部の欄を代入する |
| K031, K032, K055, K056, K080, K081, K163, K194, K207, K370, K371 | id のコピーの末尾の NUL(`ISO_ID_LEN + 1` → `+ 0` / `+ 2`): コピー先は、0 で埋めてあり(ビューは作るたびに `memset`、`queue` / `tables` / `cur` は空けるときと使うときに 0)、9 バイト目に 0 以外を書くことがない |
| K026, K027, K029, K201 | 到達しない分岐: カタログは 16 個までで、イメージごとに表は 1 つ(同じ id の表は、入れる前に取り除く)。`ISO_MAX_IMAGES` の溢れや、`NULL` の表の引き渡しは起きない |
| K251, K254, K255 | `iso_catalog_apply(ISO_EV_DELETE)` の拒否は、直前の遷移の検査(マウント中)で除かれている。`DELETE` のイベントは `meta` を書かない |
| K412, K414, K419, K421, K466, K468 | `rc == 0` の扱い: `iso_catalog_apply` / `iso_upload_idle_close` が成功(0)なら、`rc != 0 && 状態が ready` は偽(状態は invalid になっている)、`rc < 0 ? rc : ISO_OK` は、`rc <= 0` でも同じ値 |
| K448, K449, K450 | `del.phase = 1` は、参照待ちを終えた印だが、`phase == 0` のまま次のステップに入っても、表はすでに取り除かれていて(参照待ちは起きない)、同じ処理をもう一度するだけ |
| K434, K480 | 時刻の巻き戻りの基準の引き直しの `<` → `<=`: 等しいときは、基準を同じ値で上書きするだけ |
| K459 | `iso_job_step` の検査の `!job \|\| !c->cur_active` → `&&`: NULL は、続く `job != &c->cur` で弾かれる。終わったジョブの `&c->cur`(`cur_active` が偽)を渡しても、`end_job` が `cur` を 0 にしてあり、種類は NONE なので、`default` の分岐が `ISO_ERR_INVALID` を返す。結果は同じ。(同じ行の `!cur_active \|\| job != &cur` → `&&` は、動いているジョブと同じ中身の別のオブジェクトを渡す新しいテスト `test_step_with_a_foreign_pointer_does_not_touch_the_running_job` で殺した) |
| K475, K479 | `iso_upload_tick` の早期の return と、`finalizing` のときの `publish`: 枠がなければ、`IDLE_CLOSE` は NOT_FOUND で積まれず、表示は枠を空けたときに更新済み。`finalizing` の表示は、`finish` の受付で更新済み |
| K335 | `iso_catalog_scan` が、失敗する前に `count = 0` にしている(`rebuild` の失敗の分岐の重複) |
| K091 | `on_chunk_accepted` の `cur_active && kind == IDLE_CLOSE && id が同じ` の 2 つ目の `&&` → `\|\|`(`(cur_active && kind == IDLE_CLOSE) \|\| id が同じ`): 受理された `PUT` と同じ id の、別の種類の動いているジョブは存在しない(`FINISH` は枠を持ち、`PUT` は枠で拒否される。`INDEX` は `ready`、`DELETE` は `deleting` のイメージで、`PUT` は拒否される)。動いていないとき(`cur_active` が偽)の `aborted` は、読まれない(次の `pick` が `cur` を 0 にする) |

### 3.2 ビルドエラー(1 個。コンパイラが検出するので、殺したものとして数えない)

`-Werror` のコンパイル(配列の範囲外の読み出しや、使われなくなった変数など)で、ビルドできない変更。ミュータント側で、括弧の省略や未使用の変数の警告は無効にしてあるので、残ったものは、コンパイラが範囲外などの誤りとして検出したもの。

| ID | 変更箇所 |
|---|---|
| K192 | L195: const 1-1 -> 0 |

### 3.3 殺したミュータントの一覧(428 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(最初に落ちたテストの実行ファイルで止める)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
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
| K014 | L25: < -> <= | `test_core_jobs:crash/rc=0` |
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
| K037 | L46: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K038 | L47: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K041 | L50: const 0+1 -> 1 | `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K042 | L50: delete statement | `test_fuzz_random_operations_across_the_worker_contract` |
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
| K087 | L106: delete statement | `test_a_put_after_the_idle_close_was_queued_cancels_it` |
| K088 | L107: == -> != | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K089 | L107: == -> != | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K090 | L107: && -> \|\| | `test_a_put_does_not_abort_a_picked_index_job` |
| K092 | L107: negate if condition | `test_a_put_does_not_abort_a_picked_index_job`, `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K093 | L107: const 0+1 -> 1 | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K094 | L107: true -> false | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K095 | L107: delete statement | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K096 | L113: \|\| -> && | `test_core_queue:crash/rc=1` |
| K097 | L113: negate if condition | `test_core_queue:crash/rc=1` |
| K098 | L113: delete statement | `test_core_queue:crash/rc=1` |
| K099 | L114: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K100 | L114: delete statement | `test_index_jobs_do_not_take_queue_slots`, `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K101 | L115: delete statement | `test_core_queue:crash/rc=1` |
| K104 | L118: delete statement | `test_core_queue:crash/rc=1` |
| K105 | L119: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K106 | L120: delete statement | `test_core_queue:crash/rc=1` |
| K107 | L121: delete statement | `test_core_queue:crash/rc=1` |
| K108 | L122: delete statement | `test_core_queue:crash/rc=1` |
| K109 | L123: delete statement | `test_core_queue:crash/rc=1` |
| K110 | L124: delete statement | `test_core_queue:crash/rc=1` |
| K111 | L125: delete statement | `test_core_queue:crash/rc=1` |
| K112 | L126: delete statement | `test_config_chunk_and_sync_sizes_reach_the_upload` |
| K113 | L127: delete statement | `test_config_chunk_and_sync_sizes_reach_the_upload` |
| K114 | L128: delete statement | `test_core_queue:crash/rc=1` |
| K115 | L129: delete statement | `test_view_in_the_middle_of_a_finish_job` |
| K116 | L130: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K117 | L131: delete statement | `test_a_put_after_the_idle_close_was_queued_cancels_it`, `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K118 | L142: < -> <= | `test_core_queue:crash/rc=0` |
| K119 | L142: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K120 | L142: - -> + | `test_core_queue:crash/rc=1` |
| K121 | L142: const 1+1 -> 2 | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K122 | L142: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K123 | L142: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K124 | L142: const 1-1 -> 0 | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K125 | L143: delete statement | `test_core_queue:timeout` |
| K128 | L148: < -> <= | `test_core_queue:crash/rc=0` |
| K129 | L148: const 0+1 -> 1 | `test_fuzz_random_operations_across_the_worker_contract` |
| K130 | L149: == -> != | `test_overflow_of_delete_changes_nothing`, `test_duplicate_submissions_join_the_existing_job` |
| K131 | L149: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K132 | L149: && -> \|\| | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K133 | L149: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K134 | L149: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job` |
| K135 | L149: true -> false | `test_duplicate_submissions_join_the_existing_job` |
| K136 | L149: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K137 | L151: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K138 | L156: const 0+1 -> 1 | `test_pick_returns_the_running_job_until_it_ends`, `test_overflow_of_delete_changes_nothing` |
| K139 | L157: < -> <= | `test_core_queue:crash/rc=0` |
| K140 | L157: const 0+1 -> 1 | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K141 | L158: == -> != | `test_overflow_of_delete_changes_nothing` |
| K142 | L158: == -> != | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K143 | L158: == -> != | `test_overflow_of_delete_changes_nothing`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K144 | L158: && -> \|\| | `test_overflow_of_delete_changes_nothing`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K145 | L158: \|\| -> && | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K146 | L158: negate if condition | `test_overflow_of_delete_changes_nothing`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K147 | L158: const 0+1 -> 1 | `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K148 | L158: delete statement | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K149 | L165: const 0+1 -> 1 | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full`, `test_duplicate_submissions_join_the_existing_job` |
| K151 | L167: == -> != | `test_overflow_of_delete_changes_nothing`, `test_duplicate_submissions_join_the_existing_job` |
| K152 | L167: == -> != | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full`, `test_finish_replaces_a_waiting_idle_close_of_the_same_image` |
| K153 | L167: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K154 | L167: && -> \|\| | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K155 | L167: \|\| -> && | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K156 | L167: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K157 | L167: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_overflow_of_delete_changes_nothing` |
| K158 | L168: delete statement | `test_core_queue:timeout` |
| K159 | L170: delete statement | `test_core_queue:timeout` |
| K160 | L176: delete statement | `test_core_queue:crash/rc=1` |
| K161 | L177: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K162 | L177: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K164 | L177: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K165 | L178: delete statement | `test_core_queue:crash/rc=1` |
| K166 | L182: == -> != | `test_running_job_absorbs_duplicates`, `test_delete_aborts_the_running_finish_job_of_the_same_image` |
| K167 | L182: == -> != | `test_running_job_absorbs_duplicates`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K168 | L182: && -> \|\| | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K169 | L182: && -> \|\| | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K170 | L182: const 0+1 -> 1 | `test_running_job_absorbs_duplicates`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K172 | L186: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K173 | L187: == -> != | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K174 | L187: negate if condition | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K175 | L187: const 0+1 -> 1 | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K176 | L187: true -> false | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K177 | L187: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K178 | L189: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K180 | L193: const 0+1 -> 1 | `test_core_queue:timeout` |
| K181 | L194: != -> == | `test_core_queue:timeout` |
| K182 | L194: negate if condition | `test_core_queue:timeout` |
| K183 | L194: const 0+1 -> 1 | `test_core_queue:timeout` |
| K184 | L194: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K186 | L195: + -> - | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K187 | L195: - -> + | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K188 | L195: + -> - | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K189 | L195: const 1+1 -> 2 | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K190 | L195: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K191 | L195: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K193 | L195: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K195 | L196: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K198 | L197: const 1+1 -> 2 | `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K202 | L203: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K203 | L203: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K204 | L203: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K205 | L204: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K206 | L204: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K208 | L204: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K209 | L205: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K211 | L209: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K212 | L211: != -> == | `test_core_queue:crash/rc=1` |
| K213 | L211: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K214 | L211: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K215 | L211: negate if condition | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K216 | L211: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K217 | L212: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K218 | L222: == -> != | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K219 | L222: negate if condition | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K220 | L223: \|\| -> && | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K221 | L223: negate if condition | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K222 | L223: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K223 | L224: delete statement | `test_a_finalizing_image_without_a_job_gets_a_job_again`, `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K224 | L225: delete statement | `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K225 | L227: != -> == | `test_core_queue:crash/rc=1` |
| K226 | L227: negate if condition | `test_core_queue:crash/rc=1` |
| K227 | L227: delete statement | `test_delete_of_a_missing_image_is_not_found` |
| K228 | L228: >= -> > | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K229 | L228: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K230 | L228: - -> + | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K231 | L229: != -> == | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K232 | L229: negate if condition | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K233 | L229: delete statement | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K234 | L232: delete statement | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full`, `test_finish_replaces_a_waiting_idle_close_of_the_same_image` |
| K235 | L233: delete statement | `test_core_queue:crash/rc=1` |
| K236 | L239: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K237 | L239: \|\| -> && | `test_core_queue:crash/rc=1` |
| K238 | L239: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K239 | L239: delete statement | `test_core_queue:crash/rc=1` |
| K240 | L240: \|\| -> && | `test_duplicate_submissions_join_the_existing_job` |
| K241 | L240: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K242 | L240: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K243 | L243: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K244 | L243: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K245 | L243: delete statement | `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K246 | L244: >= -> > | `test_overflow_of_delete_changes_nothing` |
| K247 | L244: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K248 | L244: - -> + | `test_overflow_of_delete_changes_nothing` |
| K249 | L244: delete statement | `test_overflow_of_delete_changes_nothing` |
| K250 | L247: != -> == | `test_core_queue:crash/rc=1` |
| K252 | L247: negate if condition | `test_core_queue:crash/rc=1` |
| K253 | L247: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K256 | L248: delete statement | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K257 | L249: delete statement | `test_overflow_of_delete_changes_nothing`, `test_duplicate_submissions_join_the_existing_job` |
| K258 | L250: delete statement | `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K259 | L251: == -> != | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K260 | L251: && -> \|\| | `test_delete_of_another_image_does_not_abort_the_running_job` |
| K261 | L251: negate if condition | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K262 | L251: const 0+1 -> 1 | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K263 | L251: true -> false | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K264 | L251: delete statement | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K265 | L252: delete statement | `test_core_queue:crash/rc=1` |
| K266 | L253: delete statement | `test_core_jobs:crash/rc=1` |
| K267 | L263: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K268 | L263: \|\| -> && | `test_core_queue:crash/rc=1` |
| K269 | L263: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K270 | L263: delete statement | `test_delete_of_a_missing_image_is_not_found` |
| K271 | L264: < -> <= | `test_core_queue:crash/rc=0` |
| K272 | L264: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K273 | L265: == -> != | `test_index_jobs_do_not_take_queue_slots`, `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K274 | L265: negate if condition | `test_index_jobs_do_not_take_queue_slots`, `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K275 | L265: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K276 | L265: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K277 | L267: == -> != | `test_queue_holds_8_jobs_and_the_9th_is_busy`, `test_running_job_absorbs_duplicates` |
| K278 | L267: && -> \|\| | `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K279 | L267: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K280 | L267: const 0+1 -> 1 | `test_running_job_absorbs_duplicates` |
| K281 | L267: delete statement | `test_running_job_absorbs_duplicates` |
| K282 | L268: >= -> > | `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K283 | L268: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K284 | L268: delete statement | `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K285 | L269: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_do_not_take_queue_slots` |
| K286 | L274: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K287 | L274: \|\| -> && | `test_core_queue:crash/rc=1` |
| K288 | L274: negate if condition | `test_core_queue:crash/rc=1` |
| K289 | L274: delete statement | `test_core_queue:crash/rc=1` |
| K290 | L275: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K291 | L275: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K292 | L275: delete statement | `test_index_submission_requires_a_ready_image` |
| K293 | L276: == -> != | `test_core_reinit:crash/rc=1` |
| K294 | L276: && -> \|\| | `test_core_reinit:crash/rc=1` |
| K295 | L276: \|\| -> && | `test_duplicate_submissions_join_the_existing_job` |
| K296 | L276: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K297 | L276: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K298 | L276: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K299 | L277: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K300 | L287: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K302 | L288: == -> != | `test_core_reinit:crash/rc=1` |
| K303 | L288: negate if condition | `test_core_reinit:crash/rc=1` |
| K304 | L288: delete statement | `test_core_reinit:crash/rc=1` |
| K305 | L289: == -> != | `test_delete_aborts_the_running_index_job` |
| K306 | L289: negate if condition | `test_delete_aborts_the_running_index_job` |
| K307 | L289: delete statement | `test_delete_aborts_the_running_index_job` |
| K308 | L290: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K309 | L290: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K310 | L291: const 0+1 -> 1 | `test_delete_job_without_a_table_does_not_wait` |
| K312 | L296: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K313 | L296: delete statement | `test_core_reinit:crash/rc=1` |
| K316 | L298: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K317 | L298: delete statement | `test_core_reinit:crash/rc=1` |
| K320 | L300: delete statement | `test_core_reinit:crash/rc=1` |
| K321 | L301: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K322 | L302: delete statement | `test_idle_close_joins_a_waiting_finish_of_the_same_image`, `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K324 | L307: const 0+1 -> 1 | `test_reinit_resumes_deleting_images` |
| K325 | L309: == -> != | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K326 | L309: < -> <= | `test_boot_queues_at_most_8_resumed_deletes` |
| K327 | L309: && -> \|\| | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K328 | L309: negate if condition | `test_core_queue:crash/rc=0` |
| K329 | L309: delete statement | `test_reinit_cancels_everything_before_remount_and_rebuilds_after`, `test_reinit_resumes_deleting_images` |
| K330 | L311: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K331 | L316: < -> <= | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K332 | L316: negate if condition | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K333 | L316: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K334 | L317: const 0+1 -> 1 | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_boot_scan_error_is_returned` |
| K336 | L318: const 0+1 -> 1 | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K337 | L318: delete statement | `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K338 | L319: delete statement | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K339 | L322: delete statement | `test_view_contents` |
| K340 | L323: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_a_put_does_not_abort_a_picked_index_job` |
| K341 | L324: delete statement | `test_core_jobs:crash/rc=1` |
| K342 | L329: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K343 | L334: delete statement | `test_core_reinit:crash/rc=1` |
| K344 | L335: negate if condition | `test_core_reinit:crash/rc=1` |
| K345 | L337: < -> <= | `test_core_reinit:crash/rc=1` |
| K346 | L337: negate if condition | `test_core_reinit:crash/rc=1` |
| K347 | L337: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K348 | L338: const 0+1 -> 1 | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs` |
| K349 | L338: delete statement | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs` |
| K350 | L339: const 0+1 -> 1 | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K351 | L339: delete statement | `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K352 | L340: delete statement | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K353 | L350: < -> <= | `test_core_queue:crash/rc=0` |
| K354 | L350: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K355 | L351: == -> != | `test_core_queue:crash/rc=1` |
| K356 | L351: negate if condition | `test_core_queue:crash/rc=1` |
| K357 | L351: delete statement | `test_core_queue:crash/rc=1` |
| K358 | L357: negate if condition | `test_pick_with_nothing_queued_returns_null`, `test_priority_is_delete_finish_idle_close_index` |
| K359 | L357: delete statement | `test_pick_returns_the_running_job_until_it_ends`, `test_step_with_a_foreign_pointer_does_not_touch_the_running_job` |
| K360 | L359: < -> <= | `test_core_queue:crash/rc=1` |
| K361 | L359: / -> * | `test_core_queue:crash/rc=1` |
| K362 | L359: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K363 | L361: >= -> > | `test_core_queue:crash/rc=1` |
| K364 | L361: negate if condition | `test_core_queue:crash/rc=1` |
| K365 | L361: delete statement | `test_core_queue:crash/rc=1` |
| K366 | L362: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K368 | L363: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K369 | L364: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K372 | L364: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K373 | L365: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K374 | L366: true -> false | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K375 | L366: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K376 | L367: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K377 | L367: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K378 | L367: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K379 | L368: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K380 | L368: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K381 | L368: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K382 | L371: > -> >= | `test_core_queue:timeout` |
| K383 | L371: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K384 | L372: + -> - | `test_core_queue:crash/rc=1` |
| K385 | L372: const 1+1 -> 2 | `test_core_queue:crash/rc=0` |
| K386 | L372: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K387 | L373: delete statement | `test_core_queue:timeout` |
| K388 | L374: delete statement | `test_core_queue:timeout` |
| K389 | L376: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K390 | L376: \|\| -> && | `test_index_pick_skips_images_that_are_no_longer_ready` |
| K391 | L376: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K392 | L376: delete statement | `test_index_pick_skips_images_that_are_no_longer_ready` |
| K393 | L377: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K395 | L378: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K396 | L379: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K397 | L380: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K398 | L381: true -> false | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K399 | L381: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K400 | L387: && -> \|\| | `test_core_queue:crash/rc=1` |
| K401 | L390: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K402 | L391: delete statement | `test_core_jobs:crash/rc=1` |
| K403 | L398: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K404 | L398: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K405 | L398: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K406 | L400: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K407 | L400: delete statement | `test_index_job_whose_image_left_the_catalog_ends_done` |
| K408 | L404: delete statement | `test_core_queue:crash/rc=1` |
| K409 | L405: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K410 | L411: == -> != | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K411 | L411: != -> == | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K413 | L411: negate if condition | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K415 | L412: delete statement | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K416 | L413: delete statement | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K417 | L415: delete statement | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K418 | L416: < -> <= | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K420 | L416: const 0+1 -> 1 | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K422 | L424: == -> != | `test_core_jobs:crash/rc=1` |
| K423 | L424: negate if condition | `test_core_jobs:crash/rc=1` |
| K424 | L424: const 0+1 -> 1 | `test_core_jobs:crash/rc=1` |
| K425 | L426: > -> >= | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K426 | L426: && -> \|\| | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K427 | L426: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K428 | L426: const 1+1 -> 2 | `test_core_jobs:crash/rc=1` |
| K429 | L426: const 1-1 -> 0 | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K430 | L428: negate if condition | `test_core_jobs:crash/rc=1` |
| K431 | L429: true -> false | `test_core_jobs:crash/rc=1` |
| K432 | L429: delete statement | `test_core_jobs:crash/rc=1` |
| K433 | L430: delete statement | `test_core_jobs:crash/rc=1` |
| K435 | L432: negate if condition | `test_core_jobs:crash/rc=1` |
| K436 | L432: delete statement | `test_core_jobs:crash/rc=1` |
| K437 | L433: >= -> > | `test_core_jobs:crash/rc=1` |
| K438 | L433: negate if condition | `test_core_jobs:crash/rc=1` |
| K439 | L433: - -> + | `test_core_jobs:crash/rc=1` |
| K440 | L433: * -> / | `test_core_jobs:crash/rc=1` |
| K441 | L433: const 1000+1 -> 1001 | `test_core_jobs:crash/rc=1` |
| K442 | L433: const 1000-1 -> 999 | `test_core_jobs:crash/rc=1` |
| K443 | L436: true -> false | `test_core_jobs:crash/rc=1` |
| K444 | L436: delete statement | `test_core_jobs:crash/rc=1` |
| K445 | L439: false -> true | `test_delete_job_without_a_table_does_not_wait`, `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table` |
| K446 | L439: delete statement | `test_delete_job_waits_for_readers_then_proceeds` |
| K447 | L440: delete statement | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K451 | L444: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K452 | L444: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K453 | L444: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K454 | L445: == -> != | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K455 | L445: negate if condition | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K456 | L445: delete statement | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K457 | L450: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K458 | L450: \|\| -> && | `test_core_queue:crash/rc=1` |
| K460 | L450: \|\| -> && | `test_core_queue:crash/rc=1` |
| K461 | L450: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K462 | L450: delete statement | `test_core_queue:crash/rc=1` |
| K463 | L453: negate if condition | `test_idle_close_job_makes_the_upload_incomplete`, `test_idle_close_job_error` |
| K464 | L453: delete statement | `test_a_put_after_the_idle_close_was_picked_aborts_it` |
| K465 | L455: < -> <= | `test_priority_is_delete_finish_idle_close_index` |
| K467 | L455: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index` |
| K469 | L459: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K470 | L459: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K471 | L459: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K472 | L463: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K473 | L463: delete statement | `test_delete_aborts_the_running_index_job` |
| K474 | L474: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K476 | L475: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K477 | L476: != -> == | `test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked` |
| K478 | L476: negate if condition | `test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked` |
| K481 | L479: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K482 | L479: delete statement | `test_clock_going_backwards_counts_as_zero_elapsed_and_rebases`, `test_clock_going_back_by_one_ms_is_also_zero_elapsed` |
| K483 | L480: - -> + | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K484 | L481: >= -> > | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K485 | L481: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K486 | L481: * -> / | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K487 | L481: const 1000+1 -> 1001 | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K488 | L481: const 1000-1 -> 999 | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K489 | L481: delete statement | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K490 | L482: != -> == | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |
| K491 | L482: negate if condition | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |
| K492 | L482: delete statement | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |

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
