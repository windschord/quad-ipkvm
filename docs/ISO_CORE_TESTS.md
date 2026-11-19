# iso_core のテストとミューテーションテストの結果

作成: 2026-11-19(Issue #139、C-10 の後半)。`components/iso_core`(ワーカーのジョブの契約: キューと INDEX の集合、優先順位、同じイメージのジョブの合流、`iso_core_pick` / `iso_job_step`、`DELETE` の参照待ち、
公開ビューの更新、`iso_upload_tick`、SD の再初期化 `iso_core_sd_reinit`、起動 `iso_core_boot`。純 C。**ロックを持たない、スレッドを作らない、ブロックしない**)のホスト単体テストの構成と、`iso_core.c` に対するミューテーションテストの結果。
契約は `docs/ISO_MOUNT_DESIGN.md` §3.4(状態の遷移、SD の再初期化の手順)、§4.1(ロック)、§4.2(ワーカー)、§6.1、`docs/INTERFACES.md` §7.5。受信の状態機械と `FINISH` / `INDEX` のステップは `docs/ISO_UPLOAD_TESTS.md`。

## 1. 構成と、グルーへの契約

`components/iso_core/include/iso_core.h` の先頭のコメントが、**グルー(A-13 の `iso_worker`、A-15 の HTTP ハンドラー)への契約**(ロックの取り方、ワーカーのループ、SD の再初期化の呼び出し順、ビューのコールバックの規則)。要点:

- `iso_core_*`、`iso_upload_*`(`iso_core_upload()` が返すもの)、`iso_job_step`、`iso_upload_tick` は、`iso_op_mutex` を取って呼ぶ。`hooks.view_update` は `iso_op_mutex` の下で呼ばれ、実装は `iso_state_mutex` を取る(短く、I/O なし、`iso_core` / `iso_upload` を呼ばない)。ビューの `images[].table` は、保持するなら、コールバックの中で `iso_extent_table_ref` する。
- **SD の再初期化**: ① グルーが `iso_state_mutex` だけを取って(`sd.state` を ready 以外に、`iso_media_force_eject()`、`epoch_bump`)、放してから、`iso_op_mutex` を取って `iso_core_sd_reinit(core, remount, ctx)` を呼ぶ。中で、② ジョブの全取り消し → ③ 開いているファイルを捨てる → ④ アップロードを `incomplete` に、SHA-256 の状態を捨てる → ⑤ `remount`(グルーの `f_mount`)→ ⑥ カタログの再構築 → ⑦ `ready` のイメージごとに `INDEX`、`deleting` のイメージの `DELETE`。自動ではマウントしない。
- 戻り値は、`iso_res_t`(`iso_upload.h`。0 = 受理、`ISO_RES_JOINED` = 合流、正 = 拒否)と `iso_err_t`(負。キューのあふれは `ISO_ERR_BUSY`)。

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_core/test_core_queue.c` | 優先順位(`DELETE`、`FINISH`、`IDLE_CLOSE`、`INDEX`。同じ種類は入れた順)、後から入れた高い優先順位が先に動く、動いているジョブは `pick` が返し続ける、`INDEX` はキューに積まない(入れた順に 1 つずつ)、**キューは 8 でいっぱい、9 つ目は `ISO_ERR_BUSY`**(`DELETE` / `FINISH` は状態を変えずに取り消す。待っているジョブを置き換える分は数えない)、同じイメージの重複する投入の合流、`FINISH` と待っている `IDLE_CLOSE`、**`DELETE` が動いている `FINISH` / `INDEX` に中止の印**を立てて枠を空ける(別のイメージには立てない)、待っているジョブの取り消し、アップロード中の `DELETE`(ファイルを閉じ、SHA-256 を捨て、枠を空ける)、マウント中は拒否(何も変えない)、`meta_corrupt` の削除 | 34 |
| `test/host/iso_core/test_core_jobs.c` | 各ジョブのステップ: `FINISH`(完了、I/O エラーで `incomplete` + 再試行、署名の誤りは DONE で `invalid`)、`INDEX`(表の公開、`part_mismatch` で `invalid` と古い表の解放(READ の参照は残る)、I/O エラーは `ready` の表なし、`meta` 書き込みの失敗)、`IDLE_CLOSE`、**`DELETE`**(`free_bytes` の更新、表なしは待たない、**参照待ち: 待つ間は何も消さない、30 秒(29999 ms / 30000 ms)で `ERROR(ISO_ERR_BUSY)` で `deleting` のまま、時刻の巻き戻りは経過 0**、I/O エラーと続き)、ビューの内容(状態の表示: `ready` で表がなければ `finalizing`、`sha256`、`hidden`、`meta_corrupt`、`sd_state`)、フックなしでも動く | 28 |
| `test/host/iso_core/test_core_reinit.c` | **SD の再初期化の順序**: remount の時点で、キュー・INDEX の集合・動いているジョブ・表・ファイルハンドル・枠・SHA-256 の状態が片づいていて、カタログは古いまま、`uploading` は `incomplete`、READ の参照は残る(遅延解放)。その後、カタログを作り直し、`ready` は INDEX の集合へ、`deleting` は `DELETE` へ、`free_bytes` を採り直し、ビューを更新する。remount / scan の失敗(空のカタログ)、二度の呼び出し、動いている `FINISH` / `DELETE` の取り消し。起動(`uploading` → `incomplete`、`deleting` の再開は最大 8 つ、前の状態を捨てる) | 13 |
| `test/host/iso_core/test_core_tick.c` | `iso_upload_tick`: **経過 299999 ms は積まず、300000 ms で `IDLE_CLOSE`**、重複しない、最後の `PUT` から数える、`finalizing` はタイムアウトしない、**時刻の巻き戻りは経過 0 で基準を引き直す**、キューがあふれたら次の `tick` で再試行、`receiving` → `idle` の境(5000 ms ちょうどは receiving、5001 ms で idle。変わったときだけビューを更新) | 11 |
| `test/host/iso_core/test_core_fuzz.c` | **固定シードのファズ**(下記) | 1 |
| `test/host/iso_core/core_common.h` | 共通の部品(`iso_core` の設定、フック、ワーカーのループの代わりの `drive()` / `run_job()`) | - |

合計 87 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルが 5 個増える)。期待値は手書き。

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

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー(コンパイラが検出) |
|---|---|---|---|
| 480 | 416 | 63 | 1 |

### 3.1 同値のミュータント(63 個)

生き残ったものは、1 つずつ、変更の前後の挙動が同じ理由を確かめた。同じ理由のものは、まとめた。

| ID | 同値の理由 |
|---|---|
| K001, K007, K036, K140, K161, K169, K175, K200, K313 | 走査・移動の `<` → `<=`: 最後の次の要素(`tables[table_n]`、`queue[qn]`、`index_ids[index_n]`、`images[count]`)は 0 で埋まっていて、一致も、状態も、表もない(`strcmp` は一致せず、`unref(NULL)` は 0、状態なしは読み飛ばす)。詰める移動が 1 つ多く読んでも、直後の `memset` で消す |
| K022, K023, K039, K040, K116, K117, K186, K187, K189, K190, K304, K305, K308, K309, K301, K357, K384, K291, K093, K094 | 空いた要素や、使い終わった領域を 0 にする衛生の書き込み: `table_n` / `qn` / `index_n` / `count` より後ろは、読まれない(次に使うときに、`memcpy` / `memset` で全部書く)。`job_release` は、動いていないときは、0 の `iso_job_t` を放すだけ。`iso_upload_cfg_t uc` は、全部の欄を代入する |
| K031, K032, K055, K056, K080, K081, K153, K197, K184, K360, K361 | id のコピーの末尾の NUL(`ISO_ID_LEN + 1` → `+ 0` / `+ 2`): コピー先は、0 で埋めてあり(ビューは作るたびに `memset`、`queue` / `tables` / `cur` は空けるときと使うときに 0)、9 バイト目に 0 以外を書くことがない |
| K026, K027, K029, K191 | 到達しない分岐: カタログは 16 個までで、イメージごとに表は 1 つ(同じ id の表は、入れる前に取り除く)。`ISO_MAX_IMAGES` の溢れや、`NULL` の表の引き渡しは起きない |
| K241, K244, K245 | `iso_catalog_apply(ISO_EV_DELETE)` の拒否は、直前の遷移の検査(マウント中)で除かれている。`DELETE` のイベントは `meta` を書かない |
| K402, K404, K409, K411, K454, K456 | `rc == 0` の扱い: `iso_catalog_apply` / `iso_upload_idle_close` が成功(0)なら、`rc != 0 && 状態が ready` は偽(状態は invalid になっている)、`rc < 0 ? rc : ISO_OK` は、`rc <= 0` でも同じ値 |
| K438, K439, K440 | `del.phase = 1` は、参照待ちを終えた印だが、`phase == 0` のまま次のステップに入っても、表はすでに取り除かれていて(参照待ちは起きない)、同じ処理をもう一度するだけ |
| K424, K468 | 時刻の巻き戻りの基準の引き直しの `<` → `<=`: 等しいときは、基準を同じ値で上書きするだけ |
| K449, K450 | `iso_job_step` の検査の `\|\|` → `&&`: 終わったジョブ(`cur_active` が偽、種類は NONE)を渡しても、`default` の分岐が `ISO_ERR_INVALID` を返す。結果は同じ |
| K463, K467 | `iso_upload_tick` の早期の return と、`finalizing` のときの `publish`: 枠がなければ、`IDLE_CLOSE` は NOT_FOUND で積まれず、表示は枠を空けたときに更新済み。`finalizing` の表示は、`finish` の受付で更新済み |
| K325 | `iso_catalog_scan` が、失敗する前に `count = 0` にしている(`rebuild` の失敗の分岐の重複) |

### 3.2 ビルドエラー(1 個。コンパイラが検出するので、殺したものとして数えない)

`-Werror` のコンパイル(配列の範囲外の読み出しや、使われなくなった変数など)で、ビルドできない変更。ミュータント側で、括弧の省略や未使用の変数の警告は無効にしてあるので、残ったものは、コンパイラが範囲外などの誤りとして検出したもの。

| ID | 変更箇所 |
|---|---|
| K182 | L186: const 1-1 -> 0 |

### 3.3 殺したミュータントの一覧(416 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(最初に落ちたテストの実行ファイルで止める)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| K002 | L15: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K003 | L16: == -> != | `test_delete_of_the_mounted_image_is_refused_without_side_effects`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K004 | L16: negate if condition | `test_delete_of_the_mounted_image_is_refused_without_side_effects`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K005 | L16: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K006 | L16: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K008 | L22: const 0+1 -> 1 | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table`, `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table` |
| K009 | L23: != -> == | `test_priority_is_delete_finish_idle_close_index` |
| K010 | L23: negate if condition | `test_priority_is_delete_finish_idle_close_index` |
| K011 | L23: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index` |
| K012 | L23: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K013 | L24: delete statement | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table`, `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table` |
| K014 | L25: < -> <= | `test_core_jobs:crash/rc=0` |
| K015 | L25: + -> - | `test_core_jobs:crash/rc=1` |
| K016 | L25: - -> + | `test_core_jobs:crash/rc=1` |
| K017 | L25: const 1+1 -> 2 | `test_core_jobs:crash/rc=1` |
| K018 | L25: const 1-1 -> 0 | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K019 | L25: const 1+1 -> 2 | `test_core_jobs:crash/rc=1` |
| K020 | L25: const 1-1 -> 0 | `test_core_jobs:crash/rc=1` |
| K021 | L26: delete statement | `test_core_jobs:crash/rc=1` |
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
| K087 | L105: \|\| -> && | `test_core_queue:crash/rc=1` |
| K088 | L105: negate if condition | `test_core_queue:crash/rc=1` |
| K089 | L105: delete statement | `test_core_queue:crash/rc=1` |
| K090 | L106: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K091 | L106: delete statement | `test_index_jobs_do_not_take_queue_slots`, `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K092 | L107: delete statement | `test_core_queue:crash/rc=1` |
| K095 | L110: delete statement | `test_core_queue:crash/rc=1` |
| K096 | L111: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed` |
| K097 | L112: delete statement | `test_core_queue:crash/rc=1` |
| K098 | L113: delete statement | `test_core_queue:crash/rc=1` |
| K099 | L114: delete statement | `test_core_queue:crash/rc=1` |
| K100 | L115: delete statement | `test_core_queue:crash/rc=1` |
| K101 | L116: delete statement | `test_core_queue:crash/rc=1` |
| K102 | L117: delete statement | `test_core_queue:crash/rc=1` |
| K103 | L118: delete statement | `test_config_chunk_and_sync_sizes_reach_the_upload` |
| K104 | L119: delete statement | `test_config_chunk_and_sync_sizes_reach_the_upload` |
| K105 | L120: delete statement | `test_core_queue:crash/rc=1` |
| K106 | L121: delete statement | `test_view_in_the_middle_of_a_finish_job` |
| K107 | L122: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K108 | L133: < -> <= | `test_core_queue:crash/rc=0` |
| K109 | L133: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K110 | L133: - -> + | `test_core_queue:crash/rc=1` |
| K111 | L133: const 1+1 -> 2 | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K112 | L133: const 1-1 -> 0 | `test_priority_is_delete_finish_idle_close_index` |
| K113 | L133: const 1+1 -> 2 | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K114 | L133: const 1-1 -> 0 | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K115 | L134: delete statement | `test_core_queue:timeout` |
| K118 | L139: < -> <= | `test_core_queue:crash/rc=0` |
| K119 | L139: const 0+1 -> 1 | `test_fuzz_random_operations_across_the_worker_contract` |
| K120 | L140: == -> != | `test_overflow_of_delete_changes_nothing`, `test_duplicate_submissions_join_the_existing_job` |
| K121 | L140: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K122 | L140: && -> \|\| | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K123 | L140: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K124 | L140: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job` |
| K125 | L140: true -> false | `test_duplicate_submissions_join_the_existing_job` |
| K126 | L140: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K127 | L142: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K128 | L147: const 0+1 -> 1 | `test_pick_returns_the_running_job_until_it_ends`, `test_overflow_of_delete_changes_nothing` |
| K129 | L148: < -> <= | `test_core_queue:crash/rc=0` |
| K130 | L148: const 0+1 -> 1 | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K131 | L149: == -> != | `test_overflow_of_delete_changes_nothing` |
| K132 | L149: == -> != | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K133 | L149: == -> != | `test_overflow_of_delete_changes_nothing`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K134 | L149: && -> \|\| | `test_overflow_of_delete_changes_nothing`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K135 | L149: \|\| -> && | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K136 | L149: negate if condition | `test_overflow_of_delete_changes_nothing`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K137 | L149: const 0+1 -> 1 | `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K138 | L149: delete statement | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K139 | L156: const 0+1 -> 1 | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full`, `test_duplicate_submissions_join_the_existing_job` |
| K141 | L158: == -> != | `test_overflow_of_delete_changes_nothing`, `test_duplicate_submissions_join_the_existing_job` |
| K142 | L158: == -> != | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full`, `test_finish_replaces_a_waiting_idle_close_of_the_same_image` |
| K143 | L158: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K144 | L158: && -> \|\| | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K145 | L158: \|\| -> && | `test_overflow_of_delete_changes_nothing`, `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K146 | L158: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K147 | L158: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_overflow_of_delete_changes_nothing` |
| K148 | L159: delete statement | `test_core_queue:timeout` |
| K149 | L161: delete statement | `test_core_queue:timeout` |
| K150 | L167: delete statement | `test_core_queue:crash/rc=1` |
| K151 | L168: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K152 | L168: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K154 | L168: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K155 | L169: delete statement | `test_core_queue:crash/rc=1` |
| K156 | L173: == -> != | `test_running_job_absorbs_duplicates`, `test_delete_aborts_the_running_finish_job_of_the_same_image` |
| K157 | L173: == -> != | `test_running_job_absorbs_duplicates`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K158 | L173: && -> \|\| | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K159 | L173: && -> \|\| | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K160 | L173: const 0+1 -> 1 | `test_running_job_absorbs_duplicates`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K162 | L177: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K163 | L178: == -> != | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K164 | L178: negate if condition | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K165 | L178: const 0+1 -> 1 | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_duplicate_submissions_join_the_existing_job` |
| K166 | L178: true -> false | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K167 | L178: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_mark_unindexed_marks_ready_images_without_a_table` |
| K168 | L180: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K170 | L184: const 0+1 -> 1 | `test_core_queue:timeout` |
| K171 | L185: != -> == | `test_core_queue:timeout` |
| K172 | L185: negate if condition | `test_core_queue:timeout` |
| K173 | L185: const 0+1 -> 1 | `test_core_queue:timeout` |
| K174 | L185: delete statement | `test_priority_is_delete_finish_idle_close_index` |
| K176 | L186: + -> - | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K177 | L186: - -> + | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K178 | L186: + -> - | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K179 | L186: const 1+1 -> 2 | `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added`, `test_index_pick_skips_images_that_are_no_longer_ready` |
| K180 | L186: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K181 | L186: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K183 | L186: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K185 | L187: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K188 | L188: const 1+1 -> 2 | `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K192 | L194: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K193 | L194: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K194 | L194: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K195 | L195: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K196 | L195: const 1+1 -> 2 | `test_core_queue:crash/rc=1` |
| K198 | L195: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K199 | L196: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K201 | L200: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_index_job_io_error_keeps_ready_without_a_table_and_can_be_retried` |
| K202 | L202: != -> == | `test_core_queue:crash/rc=1` |
| K203 | L202: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K204 | L202: \|\| -> && | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K205 | L202: negate if condition | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K206 | L202: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K207 | L203: delete statement | `test_mark_unindexed_marks_ready_images_without_a_table` |
| K208 | L213: == -> != | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K209 | L213: negate if condition | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K210 | L214: \|\| -> && | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K211 | L214: negate if condition | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K212 | L214: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_running_job_absorbs_duplicates` |
| K213 | L215: delete statement | `test_a_finalizing_image_without_a_job_gets_a_job_again`, `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K214 | L216: delete statement | `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K215 | L218: != -> == | `test_core_queue:crash/rc=1` |
| K216 | L218: negate if condition | `test_core_queue:crash/rc=1` |
| K217 | L218: delete statement | `test_delete_of_a_missing_image_is_not_found` |
| K218 | L219: >= -> > | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K219 | L219: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_overflow_of_finish_rolls_back_an_incomplete_image` |
| K220 | L219: - -> + | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full` |
| K221 | L220: != -> == | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K222 | L220: negate if condition | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K223 | L220: delete statement | `test_overflow_of_finish_rolls_back_an_incomplete_image`, `test_overflow_of_finish_rolls_back_an_uploading_image` |
| K224 | L223: delete statement | `test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full`, `test_finish_replaces_a_waiting_idle_close_of_the_same_image` |
| K225 | L224: delete statement | `test_core_queue:crash/rc=1` |
| K226 | L230: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K227 | L230: \|\| -> && | `test_core_queue:crash/rc=1` |
| K228 | L230: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K229 | L230: delete statement | `test_core_queue:crash/rc=1` |
| K230 | L231: \|\| -> && | `test_duplicate_submissions_join_the_existing_job` |
| K231 | L231: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K232 | L231: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K233 | L234: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K234 | L234: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K235 | L234: delete statement | `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K236 | L235: >= -> > | `test_overflow_of_delete_changes_nothing` |
| K237 | L235: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K238 | L235: - -> + | `test_overflow_of_delete_changes_nothing` |
| K239 | L235: delete statement | `test_overflow_of_delete_changes_nothing` |
| K240 | L238: != -> == | `test_core_queue:crash/rc=1` |
| K242 | L238: negate if condition | `test_core_queue:crash/rc=1` |
| K243 | L238: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K246 | L239: delete statement | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K247 | L240: delete statement | `test_overflow_of_delete_changes_nothing`, `test_duplicate_submissions_join_the_existing_job` |
| K248 | L241: delete statement | `test_delete_cancels_the_waiting_jobs_of_the_same_image` |
| K249 | L242: == -> != | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K250 | L242: && -> \|\| | `test_delete_of_another_image_does_not_abort_the_running_job` |
| K251 | L242: negate if condition | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_of_another_image_does_not_abort_the_running_job` |
| K252 | L242: const 0+1 -> 1 | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K253 | L242: true -> false | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K254 | L242: delete statement | `test_delete_aborts_the_running_finish_job_of_the_same_image`, `test_delete_aborts_the_running_index_job` |
| K255 | L243: delete statement | `test_core_queue:crash/rc=1` |
| K256 | L244: delete statement | `test_core_jobs:crash/rc=1` |
| K257 | L254: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K258 | L254: \|\| -> && | `test_core_queue:crash/rc=1` |
| K259 | L254: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K260 | L254: delete statement | `test_delete_of_a_missing_image_is_not_found` |
| K261 | L255: < -> <= | `test_core_queue:crash/rc=0` |
| K262 | L255: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K263 | L256: == -> != | `test_index_jobs_do_not_take_queue_slots`, `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K264 | L256: negate if condition | `test_index_jobs_do_not_take_queue_slots`, `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K265 | L256: const 0+1 -> 1 | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K266 | L256: delete statement | `test_duplicate_submissions_join_the_existing_job`, `test_idle_close_joins_a_waiting_finish_of_the_same_image` |
| K267 | L258: == -> != | `test_queue_holds_8_jobs_and_the_9th_is_busy`, `test_running_job_absorbs_duplicates` |
| K268 | L258: && -> \|\| | `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K269 | L258: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K270 | L258: const 0+1 -> 1 | `test_running_job_absorbs_duplicates` |
| K271 | L258: delete statement | `test_running_job_absorbs_duplicates` |
| K272 | L259: >= -> > | `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K273 | L259: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K274 | L259: delete statement | `test_queue_holds_8_jobs_and_the_9th_is_busy` |
| K275 | L260: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_do_not_take_queue_slots` |
| K276 | L265: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K277 | L265: \|\| -> && | `test_core_queue:crash/rc=1` |
| K278 | L265: negate if condition | `test_core_queue:crash/rc=1` |
| K279 | L265: delete statement | `test_core_queue:crash/rc=1` |
| K280 | L266: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K281 | L266: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K282 | L266: delete statement | `test_index_submission_requires_a_ready_image` |
| K283 | L267: == -> != | `test_core_reinit:crash/rc=1` |
| K284 | L267: && -> \|\| | `test_core_reinit:crash/rc=1` |
| K285 | L267: \|\| -> && | `test_duplicate_submissions_join_the_existing_job` |
| K286 | L267: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K287 | L267: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K288 | L267: delete statement | `test_duplicate_submissions_join_the_existing_job` |
| K289 | L268: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K290 | L278: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K292 | L279: == -> != | `test_core_reinit:crash/rc=1` |
| K293 | L279: negate if condition | `test_core_reinit:crash/rc=1` |
| K294 | L279: delete statement | `test_core_reinit:crash/rc=1` |
| K295 | L280: == -> != | `test_delete_aborts_the_running_index_job` |
| K296 | L280: negate if condition | `test_delete_aborts_the_running_index_job` |
| K297 | L280: delete statement | `test_delete_aborts_the_running_index_job` |
| K298 | L281: false -> true | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K299 | L281: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K300 | L282: const 0+1 -> 1 | `test_delete_job_without_a_table_does_not_wait` |
| K302 | L287: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K303 | L287: delete statement | `test_core_reinit:crash/rc=1` |
| K306 | L289: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K307 | L289: delete statement | `test_core_reinit:crash/rc=1` |
| K310 | L291: delete statement | `test_core_reinit:crash/rc=1` |
| K311 | L292: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K312 | L293: delete statement | `test_idle_close_joins_a_waiting_finish_of_the_same_image`, `test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged` |
| K314 | L298: const 0+1 -> 1 | `test_reinit_resumes_deleting_images` |
| K315 | L300: == -> != | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_view_without_hooks_and_sd_status_does_not_crash` |
| K316 | L300: < -> <= | `test_boot_queues_at_most_8_resumed_deletes` |
| K317 | L300: && -> \|\| | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_view_without_hooks_and_sd_status_does_not_crash` |
| K318 | L300: negate if condition | `test_core_queue:crash/rc=0` |
| K319 | L300: delete statement | `test_reinit_cancels_everything_before_remount_and_rebuilds_after`, `test_reinit_resumes_deleting_images` |
| K320 | L302: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K321 | L307: < -> <= | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_view_contents` |
| K322 | L307: negate if condition | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_view_contents` |
| K323 | L307: const 0+1 -> 1 | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_view_contents` |
| K324 | L308: const 0+1 -> 1 | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_boot_scan_error_is_returned` |
| K326 | L309: const 0+1 -> 1 | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K327 | L309: delete statement | `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K328 | L310: delete statement | `test_scan_failure_after_remount_leaves_an_empty_catalog`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K329 | L313: delete statement | `test_view_contents` |
| K330 | L314: delete statement | `test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K331 | L315: delete statement | `test_core_jobs:crash/rc=1` |
| K332 | L320: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K333 | L325: delete statement | `test_core_reinit:crash/rc=1` |
| K334 | L326: negate if condition | `test_core_reinit:crash/rc=1` |
| K335 | L328: < -> <= | `test_core_reinit:crash/rc=1` |
| K336 | L328: negate if condition | `test_core_reinit:crash/rc=1` |
| K337 | L328: const 0+1 -> 1 | `test_core_reinit:crash/rc=1` |
| K338 | L329: const 0+1 -> 1 | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs` |
| K339 | L329: delete statement | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs` |
| K340 | L330: const 0+1 -> 1 | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K341 | L330: delete statement | `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K342 | L331: delete statement | `test_remount_failure_leaves_an_empty_catalog_and_no_jobs`, `test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails` |
| K343 | L341: < -> <= | `test_core_queue:crash/rc=0` |
| K344 | L341: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K345 | L342: == -> != | `test_core_queue:crash/rc=1` |
| K346 | L342: negate if condition | `test_core_queue:crash/rc=1` |
| K347 | L342: delete statement | `test_core_queue:crash/rc=1` |
| K348 | L348: negate if condition | `test_pick_with_nothing_queued_returns_null`, `test_priority_is_delete_finish_idle_close_index` |
| K349 | L348: delete statement | `test_pick_returns_the_running_job_until_it_ends` |
| K350 | L350: < -> <= | `test_core_queue:crash/rc=1` |
| K351 | L350: / -> * | `test_core_queue:crash/rc=1` |
| K352 | L350: const 0+1 -> 1 | `test_core_queue:crash/rc=1` |
| K353 | L352: >= -> > | `test_core_queue:crash/rc=1` |
| K354 | L352: negate if condition | `test_core_queue:crash/rc=1` |
| K355 | L352: delete statement | `test_core_queue:crash/rc=1` |
| K356 | L353: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K358 | L354: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K359 | L355: + -> - | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K362 | L355: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_a_job_submitted_later_with_higher_priority_goes_first` |
| K363 | L356: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K364 | L357: true -> false | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K365 | L357: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K366 | L358: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K367 | L358: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K368 | L358: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K369 | L359: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K370 | L359: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K371 | L359: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K372 | L362: > -> >= | `test_core_queue:timeout` |
| K373 | L362: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K374 | L363: + -> - | `test_core_queue:crash/rc=1` |
| K375 | L363: const 1+1 -> 2 | `test_core_queue:crash/rc=0` |
| K376 | L363: const 1-1 -> 0 | `test_core_queue:crash/rc=1` |
| K377 | L364: delete statement | `test_core_queue:timeout` |
| K378 | L365: delete statement | `test_core_queue:timeout` |
| K379 | L367: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K380 | L367: \|\| -> && | `test_index_pick_skips_images_that_are_no_longer_ready` |
| K381 | L367: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K382 | L367: delete statement | `test_index_pick_skips_images_that_are_no_longer_ready` |
| K383 | L368: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K385 | L369: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K386 | L370: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K387 | L371: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K388 | L372: true -> false | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K389 | L372: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K390 | L378: && -> \|\| | `test_core_queue:crash/rc=1` |
| K391 | L381: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K392 | L382: delete statement | `test_core_jobs:crash/rc=1` |
| K393 | L389: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K394 | L389: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K395 | L389: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K396 | L391: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_of_the_mounted_image_is_refused_without_side_effects` |
| K397 | L391: delete statement | `test_index_job_whose_image_left_the_catalog_ends_done` |
| K398 | L395: delete statement | `test_core_queue:crash/rc=1` |
| K399 | L396: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_index_jobs_run_one_at_a_time_in_the_order_they_were_added` |
| K400 | L402: == -> != | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K401 | L402: != -> == | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K403 | L402: negate if condition | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K405 | L403: delete statement | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K406 | L404: delete statement | `test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid` |
| K407 | L406: delete statement | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K408 | L407: < -> <= | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K410 | L407: const 0+1 -> 1 | `test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table` |
| K412 | L415: == -> != | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K413 | L415: negate if condition | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K414 | L415: const 0+1 -> 1 | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K415 | L417: > -> >= | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K416 | L417: && -> \|\| | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K417 | L417: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K418 | L417: const 1+1 -> 2 | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K419 | L417: const 1-1 -> 0 | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_delete_job_waits_for_readers_then_proceeds` |
| K420 | L419: negate if condition | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K421 | L420: true -> false | `test_delete_ref_wait_gives_up_at_30_seconds`, `test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed` |
| K422 | L420: delete statement | `test_delete_ref_wait_gives_up_at_30_seconds`, `test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed` |
| K423 | L421: delete statement | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K425 | L423: negate if condition | `test_delete_ref_wait_gives_up_at_30_seconds`, `test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed` |
| K426 | L423: delete statement | `test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed` |
| K427 | L424: >= -> > | `test_delete_ref_wait_gives_up_at_30_seconds`, `test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed` |
| K428 | L424: negate if condition | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K429 | L424: - -> + | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K430 | L424: * -> / | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K431 | L424: const 1000+1 -> 1001 | `test_delete_ref_wait_gives_up_at_30_seconds`, `test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed` |
| K432 | L424: const 1000-1 -> 999 | `test_delete_ref_wait_gives_up_at_30_seconds`, `test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed` |
| K433 | L427: true -> false | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K434 | L427: delete statement | `test_delete_job_waits_for_readers_then_proceeds`, `test_delete_ref_wait_gives_up_at_30_seconds` |
| K435 | L430: false -> true | `test_delete_job_without_a_table_does_not_wait`, `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table` |
| K436 | L430: delete statement | `test_delete_job_waits_for_readers_then_proceeds` |
| K437 | L431: delete statement | `test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table`, `test_sixteen_images_all_get_tables_and_each_can_be_removed` |
| K441 | L435: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K442 | L435: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K443 | L435: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K444 | L436: == -> != | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K445 | L436: negate if condition | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K446 | L436: delete statement | `test_delete_job_removes_the_image_and_refreshes_the_free_space` |
| K447 | L441: != -> == | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K448 | L441: \|\| -> && | `test_core_queue:crash/rc=1` |
| K451 | L441: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_pick_returns_the_running_job_until_it_ends` |
| K452 | L441: delete statement | `test_core_queue:crash/rc=1` |
| K453 | L445: < -> <= | `test_priority_is_delete_finish_idle_close_index` |
| K455 | L445: const 0+1 -> 1 | `test_priority_is_delete_finish_idle_close_index` |
| K457 | L449: == -> != | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K458 | L449: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K459 | L449: delete statement | `test_priority_is_delete_finish_idle_close_index`, `test_running_job_absorbs_duplicates` |
| K460 | L453: negate if condition | `test_priority_is_delete_finish_idle_close_index`, `test_delete_aborts_the_running_index_job` |
| K461 | L453: delete statement | `test_delete_aborts_the_running_index_job` |
| K462 | L464: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K464 | L465: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K465 | L466: != -> == | `test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked` |
| K466 | L466: negate if condition | `test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked` |
| K469 | L469: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K470 | L469: delete statement | `test_clock_going_backwards_counts_as_zero_elapsed_and_rebases`, `test_clock_going_back_by_one_ms_is_also_zero_elapsed` |
| K471 | L470: - -> + | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K472 | L471: >= -> > | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K473 | L471: negate if condition | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K474 | L471: * -> / | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K475 | L471: const 1000+1 -> 1001 | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K476 | L471: const 1000-1 -> 999 | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K477 | L471: delete statement | `test_idle_timeout_boundary_is_300_seconds`, `test_the_time_is_counted_from_the_last_put` |
| K478 | L472: != -> == | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |
| K479 | L472: negate if condition | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |
| K480 | L472: delete statement | `test_receiving_to_idle_edge_publishes_the_view_once`, `test_activity_after_a_clock_rewind_is_receiving` |


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
