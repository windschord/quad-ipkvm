# iso_worker_logic のテストとミューテーションテストの結果

作成: 2026-11-24(C-15)。`components/iso_worker/iso_worker_logic.c`(A-13 の後半。PR #160。純 C。`iso_core` / `iso_media` / `iso_catalog` / `iso_upload` / `iso_seams` をつなぐ調整役: 公開ビューと表の公開、`boot`、SD の障害の通知と再初期化(`ISO_MOUNT_DESIGN.md` §3.4 の ①)、`run_once`(ワーカーの 1 回分)、`tick`、ロックの取り方と起こし方(§4.1、§4.2))の、**ミューテーションテスト(全数)**。
`iso_worker.c`(FreeRTOS のタスク、IDF)は、ホストではテストできないので、対象外(実機の確認は `HW_VERIFY.md` I 章)。

## 1. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_worker/test_iso_worker_logic.c`(A-13。PR #160) | 起動から `FINISH` と表の引き当て、`DELETE` の参照待ち、SD の障害からの復帰(§3.4 の ①〜⑦)、`FINISH` の世代の柵、アイドルのタイムアウト、クロスリンクの削除のログ、`op_unlock` の起こし、`ensure_dirs` の失敗からの復帰 | 8 |
| `test/host/iso_worker/test_iso_worker_logic_mut.c`(C-15。このドキュメント) | ミューテーションテストの生き残りを殺すためのテスト(2.4)。部品(`w_setup` など)は、A-13 のファイルと同じ形のコピー(`OPT` で、省略できるコールバックと `bdev` を NULL にでき、`os.ctx` と `remount_ctx` に印(`&E`)を渡して、取り違えを検出する) | 32 |

合計 40 件(ホストのテスト全体は ctest で 125 件。`test/host/CMakeLists.txt` の既存の自動発見(`iso_worker/` の下の `test_*.c`)で、実行ファイル `test_iso_worker_logic_mut` が 1 個増える。登録の変更は要らない)。ASan / UBSan、`-Werror`、`-fno-sanitize-recover=undefined`。期待値は手書き。

## 2. ミューテーションテスト

方法: `tools/test/mutate_iso_worker.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_worker_logic.c` の 1 か所を置換 → 再ビルド → 2 個のテストの実行ファイル(`test_iso_worker_logic`、`test_iso_worker_logic_mut`)を順に実行、を繰り返す(`iso_core` と同じ仕組み。`mutate_iso_extent.py` の `main()`)。ミュータントの定義は 2 つのファイル:

- `tools/test/iso_worker_mutants.py`(**機械が作る**): `tools/test/gen_iso_mutants.py worker .` が、ソースから作る(演算子の置換、`if` 条件の否定、文の削除、定数 ±1、真偽値の反転。**279 個**: `W001`〜`W279`)。同じスクリプトが、OS の関数に渡す文脈(`cfg.os.ctx`)を NULL にするミュータントを、呼び出しの箇所ごとに作る(**38 個**: `WC001`〜`WC038`。ctx の取り違えを調べる)。
- `tools/test/iso_worker_mutants_extra.py`(**手書き**。**52 個**: `X01`〜`X52`): 機械が作らない形。`has_pending_core_work` の項を落とす、`iso_core_cfg_t` に渡す欄を 0 / NULL にする、`const bool x = (式);` の中の比較と論理演算、呼び出しの引数(予算、`remount_ctx`、`wait_ms`、`now_ms`)、`deinit` の `remount`、エラーのコールバックの値。

```
UNITY_SRC=<unity のソース> FETCHCONTENT_SOURCE_DIR_UNITY=<unity のソース> \
python3 tools/test/mutate_iso_worker.py <repo> <スクラッチ> tools/test/iso_worker_mutants.py <結果.json> -j3
python3 tools/test/mutate_iso_worker.py <repo> <スクラッチ> tools/test/iso_worker_mutants_extra.py <結果_extra.json> -j3
```

`mutate_iso_worker.py` は、ビルドに `-ftrivial-auto-var-init=pattern`(GCC 12 以降。環境変数 `CFLAGS` 経由)を足す。未初期化のローカル変数が、スタックの残りでたまたま 0 になって、未初期化の読み出しのミュータント(例: X38)が隠れるのを避けるため。正しいコードの挙動は変わらない。通常の ctest のビルドには入れない。

経緯: A-13 のテストだけ(`test_iso_worker_logic`。8 件。新しいファイルは、起動と後始末だけの 1 件)で、機械が作る 279 個(W)を実行したところ、**殺した 161、生き残り 117、ビルドエラー 1** だった。生き残りを 1 つずつ調べ、**同値でないものは、テストを足して殺した**(2.4 の 32 件。主な穴: **公開ビューのフックの境界**(`count` の頭打ち、`count` より後ろの要素、満杯の 16 個での配列の外の読み出し、前の公開の残り)、**公開した表の解放**(`unref` の数、`deinit` の後始末)、**`NULL` と未初期化のインスタンスでの全関数**、**`init` の検査と既定値**、**省略できるコールバックが NULL のとき**、**障害の上書き(`sd_override`)の公開と解除**、**再初期化の最中の障害**、**起こすかどうか**(動いているジョブだけ、再確認だけ)、**`os.ctx` / `remount_ctx` / `wait_ms` / 予算の受け渡し**)。その後、手書きの 52 個(X)と ctx のミュータント 38 個(WC)を足して、生き残りを同じ方法で調べた(X の 11 個と WC の 3 個が、最初は生き残り、すべてテストで殺した)。**最終は、369 個を最初から全数実行**した。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| | 実行した数 | 殺した数 | 生き残った数 | 同値と判断 | 殺せない(同値ではない) | ビルドエラー(コンパイラが検出) |
|---|---|---|---|---|---|---|
| 機械(W) | 279 | 267 | 11 | 10 | 1 | 1 |
| 機械(WC。ctx) | 38 | 38 | 0 | 0 | 0 | 0 |
| 手書き(X) | 52 | 52 | 0 | 0 | 0 | 0 |
| **合計** | **369** | **357** | **11** | **10** | **1** | **1** |

A-13 のテストだけで殺した数(先に落ちた実行ファイルが `test_iso_worker_logic`): W 165、X 29、WC 0。新しいテスト(`test_iso_worker_logic_mut`)だけが殺した数: W 102、X 23、WC 38(合計 163)。(W の 165 は、最初の 161 より 4 多い: 上の `-ftrivial-auto-var-init=pattern` が、未初期化の `to_unref[]` を読むミュータント(W035、W060、W120、W132)を、A-13 のテストでも落とす。)

### 2.1 同値のミュータント(10 個)

生き残ったものは、1 つずつ、変更の前後の挙動が同じ理由を確かめた。**範囲外の読み出しだけが違うものは、同値にしていない**(2.2)。

| ID | 同値の理由 |
|---|---|
| W017 | `memset(next_slots, 0, ...)` の削除: ローカルの配列 `next_slots` は、`next_n` 個だけが `pub_tables` に写され、その各要素は全欄を代入する。使われない要素と詰め物は、どの API からも読まれない(詰め物が 0 であることは、ミュータント W016 / W027 のように、**詰め物を別の値にする変更**では検出するが、**単なる削除**は、スタックの残りが 0 のとき見えない。`-ftrivial-auto-var-init=pattern` でも変わらなかった) |
| W018 | `v->count > ISO_MAX_IMAGES` → `>=`: 等しいとき、どちらの枝も `ISO_MAX_IMAGES`(= `v->count`)になる |
| W021 | `uint32_t id_u32 = 0u` → 1: 初期値は、`iso_worker_logic_id_to_u32` が真を返したときだけ使われ、そのとき必ず上書きされる(偽のときは使われない) |
| W028 | `memcpy(slot.id, img->id, ISO_ID_LEN + 1u)` → `ISO_ID_LEN + 0u`: 終端の NUL が写されなくなるが、`next_slots` は直前に 0 で埋めてあり、`id[8]` は 0 のまま(`memset` の値を変える W016 は、詰め物のテストが殺す) |
| W043 | `if (next_n > 0u)` → `>=`: `next_n == 0` のとき、`memcpy(..., 0)` は何も書かない |
| W063 | `on_core_is_mounted_cb` の `target_id = 0u` → 1: 初期値は、`iso_worker_logic_id_to_u32` が真のときは上書きされ、偽のときは関数を抜ける |
| W068 | `on_core_is_mounted_cb` の `cur_id = 0u` → 1: `iso_media_get_id` が真(ロード済み)のときだけ上書きされ、偽のとき `loaded && ...` は `cur_id` を見ない |
| W074 | `iso_worker_logic_id_to_u32` の `v = 0u` → 1: ループは、ちょうど 8 回(`ISO_ID_LEN`)、`v = (v << 4) \| d` を行う。`uint32_t` の初期値の 1 は、8 回の 4 ビットのシフト(合計 32 ビット)で、全部押し出される |
| W104 | `atomic_init(&w->reinit_pending, false)` の削除: 直前の `memset(w, 0, ...)` で、`_Atomic bool` は偽(0)になっている |
| W106 | `atomic_init(&w->reprobe_pending, false)` の削除: 同上 |

**詰め物に関わるミュータントの扱い**(CodeRabbit の指摘、#171)。`test_impl_pin_view_slot_padding_after_the_id_is_zero`(実装の詳細を固定するテスト。動作の検査ではない)が、実装の意図(0 で埋めてから `id` を 9 バイトだけ写す)を固定して殺している。2 つは性質が違う:
- **W016**(`memset` の値を 0 → 1): 公開スロットの**詰め物の値だけ**を変える。観測できる動作(文字列、`id_u32`、大きさ)は同じなので、動作の検査では殺せない。**このテストを外す場合は、同値の表に移す**(根拠: 詰め物は、どの API からも読まれない)。
- **W027**(`memcpy` の長さ 9 → 10): 9 バイトの `id` 配列に **10 バイトを書く(配列の外への 1 バイトの書き込み。詰め物の領域)**。詰め物の値だけの差ではないので、**同値とは書かない**。このテストを外す場合は、W214(2.2)と同じく、「殺せないが、同値でもない」の表に移す(観測できる動作は変わらないが、範囲外への書き込みは、規則上、同値にしない)。

### 2.2 殺せないが、同値でもないミュータント(1 個)

| ID | 変更箇所 | 理由 |
|---|---|---|
| W214 | L320: `char job_id[ISO_ID_LEN + 1u]` → `+ 2u` | 直後の `memcpy(job_id, job->id, sizeof(job_id))` が、`job->id`(9 バイト)の **1 バイト先(`job->aborted`)を読んで**、`job_id[9]` に写す。`job_id[8]` は NUL なので、`on_job_error` が受け取る文字列は同じ。**範囲外の読み出しだけが違い、観測できる値は変わらない**。`job->id` は構造体の中のメンバーなので、サニタイザー(ASan / UBSan)は検出しない。殺すには、`iso_job_t` のレイアウトに依存しない検出(配列の範囲の検査)が要る。実装の修正案: `memcpy(job_id, job->id, ISO_ID_LEN + 1u)`(`sizeof(job_id)` でなく、元の配列の大きさ) |

### 2.3 ビルドエラー(1 個。コンパイラが検出するので、殺したものとして数えない)

| ID | 変更箇所 |
|---|---|
| W008 | L16: delete statement(`has_pending_core_work` の `\|\|` の途中の式を削ると、構文エラー) |

### 2.4 足したテスト(33 件。`test/host/iso_worker/test_iso_worker_logic_mut.c`)

| テスト | 内容 |
|---|---|
| `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids` | `iso_worker_logic_id_to_u32`: 16 進の全桁(`0`〜`9`、`a`〜`f`)、桁の位置、長さ・文字の誤り(`NULL`、7 / 9 桁、`g`、大文字、`/` `:` `` ` ``)。失敗のとき `out` を書かない。`out == NULL` は可 |
| `test_init_rejects_null_arguments_and_every_missing_mandatory_field` | `init`: `w` / `cfg` が NULL、`fs` と 5 つの `os` の必須の関数の 1 つずつの欠け → `ISO_ERR_INVALID`。省略できる 4 つのコールバックが NULL でも成功 |
| `test_init_clears_the_whole_struct_and_defaults_the_step_budget` | `init` が構造体を 0 で埋める(前の使い方の残り 0xA5 を消す)、`step_budget_bytes == 0` の既定値(`ISO_WORKER_BUDGET_KIB * 1024`)、指定した値の維持、保留フラグが偽 |
| `test_init_failure_leaves_a_zeroed_uninitialized_instance` | `iso_core_init` が失敗したとき、構造体は 0 で、`initialized` は偽、アクセサーは NULL |
| `test_every_entry_point_is_inert_on_null_and_uninitialized_instances` | `w == NULL` と、0 で埋めた(初期化していない)インスタンスで、全ての公開関数が何もせず(OS の関数を呼ばず)、エラー / NULL / 偽を返す。`run_once` は `out_busy` / `out_waiting` を偽にする(NULL も可) |
| `test_deinit_releases_the_published_tables_itself_and_is_idempotent` | `deinit`: 公開のフックが働かなくても、公開している表の参照(16 個)を自分で放す。`pub_tables` と `pub_table_n` の掃除、2 回目は何もしない、後のアクセサーと `boot` の拒否 |
| `test_sixteen_ready_images_are_published_looked_up_and_released` | 16 個(`ISO_MAX_IMAGES`)の `ready` のイメージ: 全部の表、`lookup_size` / `acquire_table`(参照 3)、見つからない id(配列の外を読まない)、再公開で参照が増減しない(2)、id・大きさ・id_u32 の欄 |
| `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` | ビューのフック(`core.cfg.hooks.view_update`)を直接: ctx / view が NULL、`count` より後ろは見ない、`FINALIZING` / 表なし / 不正な id は表に入れない、`count` の頭打ち、`consumed_since` の持ち越し(コールバックの引数と `view_locked`)、`sd_state` の上書き、前の公開の残りが消える |
| `test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer` | `media_id == 0` と `out_bytes == NULL` の拒否。id 1 は有効。予約の id `00000000` がビューにあっても、0 では引けない |
| `test_is_mounted_hook_compares_the_loaded_image_id_only` | `is_mounted` のフック: ctx / id が NULL、不正な id、マウントなし(id `00000000` とも一致しない)、マウント中の id だけが真、取り外し後は偽 |
| `test_view_publish_hands_over_the_consumed_since_of_the_upload` | `iso_core_publish` で、`consumed_since`(アップロードが書いたバイト数)が、`on_view_published` の引数と `view_locked` に届く(公開の回数も) |
| `test_boot_with_a_not_ready_sd_only_publishes_the_state` | SD が ready でないときの `boot`: 走査せず、状態だけを 1 回公開する。起こさない |
| `test_boot_wakes_the_worker_only_when_there_is_work` | `boot` が、仕事(`INDEX` の待ち)があるときだけ `wake_worker` を呼ぶ |
| `test_boot_dirs_failure_publishes_the_sd_error_through_the_override_only` | `ensure_dirs` の失敗: `set_hw_sd_state` がなくても、上書き(`sd_override_non_ready` / `val == ERROR`)だけで、`ERROR` が公開される |
| `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` | `notify_sd_fault`: `READY` は `NO_CARD` に読み替え、他はそのまま。ビューのコールバックと `wake` が 1 回ずつ、`force_eject`、再初期化の前の再公開も障害の状態、再初期化が済むと上書きを外す |
| `test_request_reprobe_sets_the_flag_and_wakes_the_worker` | `request_reprobe`: フラグと `wake`。SD が READY で上書きもなければ、再初期化しない |
| `test_run_once_without_a_pending_flag_does_not_reinit_a_card_that_is_not_ready` | 要求がなければ、ready でないカードを `run_once` は再初期化しない |
| `test_run_once_accepts_null_out_pointers_on_every_path` | `run_once` の `out_busy` / `out_waiting` が NULL でも、全ての経路(INDEX の続き / 完了、空き、再初期化)で動く。空きの経路は、呼び出し側の値を偽に書き直す |
| `test_run_once_returns_the_job_error_and_the_error_callback_gets_kind_id_and_code` | ジョブの `ERROR` のステップだけが、エラーを返す。`on_job_error` が種類・id(終端つき)・コードを受け取る |
| `test_optional_callbacks_may_all_be_null` | 省略できるコールバック(`wake_worker`、`set_hw_sd_state`、`on_view_published`、`on_job_error`)が全部 NULL でも、`boot` / `run_once`(エラーを含む)/ `notify` / `reprobe` / `tick` / `op_unlock` が動く |
| `test_force_eject_tolerates_a_missing_epoch_bump_and_a_missing_bdev` | `epoch_bump` が NULL、`bdev` が NULL でも、`force_eject` が動く(世代は進む) |
| `test_init_wires_every_cfg_field_into_the_core_the_catalog_and_the_media` | `cfg` の全部の欄が、`iso_core` / `iso_catalog` / `iso_media` に正しく渡る(`bdev`、`clock`、`sha`、`io_buf`、`alloc`、`sd`、`chunk_bytes`、`sync_bytes`、フックの ctx など) |
| `test_op_trylock_forwards_the_wait_time_and_the_result` | `op_trylock` が `wait_ms` と結果をそのまま渡す |
| `test_a_fault_raised_during_the_reinit_keeps_the_override_and_the_pending_flag` | 再初期化の最中に来た障害(`fault_raced`)は、済んだ再初期化に消されない。上書きと保留が残り、もう 1 回再初期化する |
| `test_reprobe_reinitializes_while_a_fault_override_is_published_even_if_the_card_reads_ready` | 上書きが公開中なら、カードが READY を返していても、再確認は再初期化する |
| `test_a_missing_sd_status_is_not_ready_and_a_reprobe_reinitializes` | `cfg.sd == NULL` は ready でない(`boot` は走査しない)。再確認は再初期化する |
| `test_view_slots_copy_the_id_with_its_terminator` | 動作の検査: 公開した表の欄の `id` は、終端(NUL)までが元と同じ文字列で、`id_u32` と大きさも同じ |
| `test_impl_pin_view_slot_padding_after_the_id_is_zero` | **実装の詳細を固定するテスト(動作の検査ではない)**。公開した表の欄は、0 で埋めてから `id` を 9 バイトだけ写すので、`id` の後ろの詰め物は、元のビューの値にならない。C は、構造体の代入やコピーの後の詰め物の値を保証しない(CodeRabbit の指摘、#171)。**このテストだけが殺すミュータントは、観測できる動作を変えない。実装の意図を固定するもので、2.1 の「詰め物に関わるミュータントの扱い」に分けて書く: W016 は詰め物の値だけの差(外すなら同値)、W027 は配列の外への 1 バイトの書き込み(外しても同値とは書かない。W214 と同じ扱い)** |
| `test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook` | `ensure_dirs` の失敗が `set_hw_sd_state`(`state_lock` の中、`os.ctx` つき)に `ERROR` を伝える |
| `test_pending_work_wakes_the_worker_when_the_op_lock_is_released_or_on_tick` | 動いているジョブだけ / 再確認の要求だけが残っているとき、`op_unlock` と `tick` が起こす。何もなければ起こさない |
| `test_notify_hands_the_hw_hook_the_non_ready_state_and_the_published_consumed_since` | `set_hw_sd_state` に渡すのは、読み替えた後の状態。ビューのコールバックの `consumed_since` は、直前に公開した値 |
| `test_finish_resume_steps_follow_the_configured_budget` | ディスクから読み直す `FINISH` のステップ数が、設定したステップの予算に従う(予算 0 は既定の 1 MiB)。続きのあるステップで `waiting` は偽 |
| `test_deinit_cancels_the_jobs_and_clears_the_catalog_and_the_published_view` | `deinit`: ジョブ、`INDEX` の集合、カタログ、公開ビューを空にする(カードを読み直さない) |

(`test_iso_worker_logic.c` は変えていない。)

### 2.5 殺したミュータントの一覧(357 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(最初に落ちたテストの実行ファイルで止める)。`crash/rc=1` は、そのテストの実行ファイルが、`TEST_ASSERT` の失敗の後に、ASan / UBSan で異常終了した(または、失敗の表示が出る前に落ちた)もの。変更箇所の `L` は、変更前のソースの行。`W` / `WC` はソースから機械が作ったもの、`X` は手書き。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| W001 | L6: && -> \|\| | `test_iso_worker_logic_mut:crash/rc=1` |
| W002 | L6: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W003 | L7: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W004 | L12: > -> >= | `test_iso_worker_logic:crash/rc=1` |
| W005 | L12: \|\| -> && | `test_iso_worker_logic:crash/rc=1` |
| W006 | L12: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W007 | L13: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W009 | L21: && -> \|\| | `test_iso_worker_logic_mut:crash/rc=1` |
| W010 | L21: && -> \|\| | `test_iso_worker_logic_mut:crash/rc=1` |
| W011 | L21: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W012 | L22: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W013 | L28: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W014 | L28: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W015 | L33: const 0+1 -> 1 | `test_iso_worker_logic_mut:crash/rc=1` |
| W016 | L34: const 0+1 -> 1 | `test_iso_worker_logic_mut:crash/rc=1` |
| W019 | L37: < -> <= | `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| W020 | L37: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W022 | L40: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W023 | L40: != -> == | `test_iso_worker_logic:crash/rc=1` |
| W024 | L40: && -> \|\| | `test_iso_worker_logic:crash/rc=1` |
| W025 | L40: && -> \|\| | `test_iso_worker_logic:crash/rc=1` |
| W026 | L42: + -> - | `test_sixteen_ready_images_are_published_looked_up_and_released` |
| W027 | L42: const 1+1 -> 2 | `test_iso_worker_logic_mut:crash/rc=1` |
| W029 | L42: delete statement | `test_sixteen_ready_images_are_published_looked_up_and_released` |
| W030 | L43: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W031 | L44: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W032 | L45: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W033 | L46: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W034 | L47: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W035 | L52: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W036 | L54: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W037 | L55: < -> <= | `test_iso_worker_logic_mut:crash/rc=1` |
| W038 | L55: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W039 | L56: != -> == | `test_iso_worker_logic:crash/rc=1` |
| W040 | L56: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W041 | L60: const 0+1 -> 1 | `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| W042 | L60: delete statement | `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| W044 | L61: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W045 | L61: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W046 | L62: * -> / | `test_iso_worker_logic:crash/rc=1` |
| W047 | L62: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W048 | L64: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W049 | L66: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W050 | L67: delete statement | `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| W051 | L68: < -> <= | `test_iso_worker_logic:crash/rc=1` |
| W052 | L68: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W053 | L69: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W054 | L71: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W055 | L72: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W056 | L74: delete statement | `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images`, `test_view_publish_hands_over_the_consumed_since_of_the_upload` |
| W057 | L76: negate if condition | `test_iso_worker_logic_mut:crash/rc=1` |
| W058 | L77: delete statement | `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images`, `test_view_publish_hands_over_the_consumed_since_of_the_upload` |
| W059 | L79: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W060 | L81: < -> <= | `test_iso_worker_logic:crash/rc=1` |
| W061 | L81: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W062 | L82: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W064 | L89: \|\| -> && | `test_iso_worker_logic:crash/rc=1` |
| W065 | L89: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W066 | L90: false -> true | `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| W067 | L92: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W069 | L95: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W070 | L96: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W071 | L96: && -> \|\| | `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| W072 | L105: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W073 | L106: false -> true | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| W075 | L109: < -> <= | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| W076 | L109: const 0+1 -> 1 | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids` |
| W077 | L111: >= -> > | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| W078 | L111: - -> + | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| W079 | L111: + -> - | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| W080 | L111: - -> + | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer` |
| W081 | L111: const 10+1 -> 11 | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| W082 | L111: const 10-1 -> 9 | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| W083 | L112: const 4+1 -> 5 | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids` |
| W084 | L112: const 4-1 -> 3 | `test_id_to_u32_decodes_every_digit_and_rejects_bad_ids` |
| W085 | L112: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W086 | L114: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W087 | L117: true -> false | `test_iso_worker_logic:crash/rc=1` |
| W088 | L121: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W089 | L121: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W090 | L121: \|\| -> && | `test_init_rejects_null_arguments_and_every_missing_mandatory_field` |
| W091 | L121: \|\| -> && | `test_init_rejects_null_arguments_and_every_missing_mandatory_field` |
| W092 | L121: \|\| -> && | `test_init_rejects_null_arguments_and_every_missing_mandatory_field` |
| W093 | L125: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W094 | L125: delete statement | `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| W095 | L126: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W096 | L127: == -> != | `test_init_clears_the_whole_struct_and_defaults_the_step_budget`, `test_finish_resume_steps_follow_the_configured_budget` |
| W097 | L127: negate if condition | `test_init_clears_the_whole_struct_and_defaults_the_step_budget`, `test_finish_resume_steps_follow_the_configured_budget` |
| W098 | L127: const 0+1 -> 1 | `test_init_clears_the_whole_struct_and_defaults_the_step_budget`, `test_finish_resume_steps_follow_the_configured_budget` |
| W099 | L128: * -> / | `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| W100 | L128: const 1024+1 -> 1025 | `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| W101 | L128: const 1024-1 -> 1023 | `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| W102 | L128: delete statement | `test_init_clears_the_whole_struct_and_defaults_the_step_budget`, `test_finish_resume_steps_follow_the_configured_budget` |
| W103 | L130: false -> true | `test_iso_worker_logic:crash/rc=1` |
| W105 | L131: false -> true | `test_init_clears_the_whole_struct_and_defaults_the_step_budget`, `test_boot_with_a_not_ready_sd_only_publishes_the_state` |
| W107 | L133: delete statement | `test_worker_boot_upload_finish_and_table_map`, `test_worker_delete_waits_for_msc_ref_and_clears_after_target_eject` |
| W108 | L134: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W109 | L155: != -> == | `test_iso_worker_logic:crash/rc=1` |
| W110 | L155: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W111 | L156: const 0+1 -> 1 | `test_init_failure_leaves_a_zeroed_uninitialized_instance` |
| W112 | L156: delete statement | `test_init_failure_leaves_a_zeroed_uninitialized_instance` |
| W113 | L159: true -> false | `test_iso_worker_logic:crash/rc=1` |
| W114 | L159: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W115 | L164: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W116 | L164: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W117 | L167: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W118 | L168: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W119 | L169: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W120 | L172: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W121 | L173: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W122 | L174: < -> <= | `test_iso_worker_logic_mut:crash/rc=1` |
| W123 | L174: const 0+1 -> 1 | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W124 | L175: != -> == | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W125 | L175: negate if condition | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W126 | L176: delete statement | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W127 | L179: const 0+1 -> 1 | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W128 | L179: delete statement | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W129 | L180: const 0+1 -> 1 | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W130 | L180: delete statement | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W131 | L181: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W132 | L183: < -> <= | `test_iso_worker_logic:crash/rc=1` |
| W133 | L183: const 0+1 -> 1 | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W134 | L184: delete statement | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W135 | L186: false -> true | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W136 | L186: delete statement | `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| W137 | L190: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W138 | L190: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W139 | L194: false -> true | `test_iso_worker_logic:crash/rc=1` |
| W140 | L195: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W141 | L197: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W142 | L198: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W143 | L199: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W144 | L199: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W145 | L200: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W146 | L202: true -> false | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W147 | L202: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W148 | L203: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W149 | L204: true -> false | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W150 | L204: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W151 | L205: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W152 | L206: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W153 | L207: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W154 | L210: delete statement | `test_boot_with_a_not_ready_sd_only_publishes_the_state`, `test_a_missing_sd_status_is_not_ready_and_a_reprobe_reinitializes` |
| W155 | L213: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W156 | L215: && -> \|\| | `test_iso_worker_logic:crash/rc=1` |
| W157 | L215: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W158 | L216: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W159 | L217: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W160 | L218: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W161 | L221: negate if condition | `test_boot_with_a_not_ready_sd_only_publishes_the_state`, `test_boot_wakes_the_worker_only_when_there_is_work` |
| W162 | L222: delete statement | `test_boot_wakes_the_worker_only_when_there_is_work` |
| W163 | L228: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W164 | L228: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W165 | L231: == -> != | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` |
| W166 | L235: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W167 | L236: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W168 | L237: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W169 | L239: true -> false | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_a_fault_raised_during_the_reinit_keeps_the_override_and_the_pending_flag` |
| W170 | L239: delete statement | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_a_fault_raised_during_the_reinit_keeps_the_override_and_the_pending_flag` |
| W171 | L240: delete statement | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` |
| W172 | L241: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W173 | L242: negate if condition | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` |
| W174 | L243: delete statement | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` |
| W175 | L245: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W176 | L246: true -> false | `test_iso_worker_logic:crash/rc=1` |
| W177 | L246: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W178 | L247: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W179 | L249: delete statement | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` |
| W180 | L253: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W181 | L253: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W182 | L256: true -> false | `test_iso_worker_logic:crash/rc=1` |
| W183 | L256: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W184 | L257: delete statement | `test_request_reprobe_sets_the_flag_and_wakes_the_worker` |
| W185 | L265: negate if condition | `test_iso_worker_logic_mut:crash/rc=1` |
| W186 | L268: negate if condition | `test_iso_worker_logic_mut:crash/rc=1` |
| W187 | L271: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W188 | L271: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W189 | L275: false -> true | `test_iso_worker_logic:crash/rc=1` |
| W190 | L276: false -> true | `test_iso_worker_logic:crash/rc=1` |
| W191 | L277: && -> \|\| | `test_run_once_without_a_pending_flag_does_not_reinit_a_card_that_is_not_ready` |
| W192 | L277: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W193 | L278: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W194 | L282: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W195 | L283: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W196 | L284: true -> false | `test_iso_worker_logic:crash/rc=1` |
| W197 | L284: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W198 | L288: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W199 | L289: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W200 | L291: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W201 | L293: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W202 | L294: false -> true | `test_iso_worker_logic:crash/rc=1` |
| W203 | L294: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W204 | L296: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W205 | L297: negate if condition | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W206 | L298: delete statement | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| W207 | L301: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W208 | L302: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W209 | L308: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W210 | L310: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W211 | L312: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W212 | L313: negate if condition | `test_iso_worker_logic_mut:crash/rc=1` |
| W213 | L320: + -> - | `test_iso_worker_logic:crash/rc=1` |
| W215 | L320: const 1-1 -> 0 | `test_iso_worker_logic:crash/rc=1` |
| W216 | L321: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W217 | L324: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W218 | L324: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W219 | L326: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W220 | L327: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W221 | L330: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W222 | L337: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W223 | L339: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W224 | L339: && -> \|\| | `test_iso_worker_logic:crash/rc=1` |
| W225 | L339: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W226 | L340: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W227 | L342: negate if condition | `test_iso_worker_logic_mut:crash/rc=1` |
| W228 | L345: == -> != | `test_iso_worker_logic_mut:crash/rc=1` |
| W229 | L349: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W230 | L349: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W231 | L352: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W232 | L353: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W233 | L355: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W234 | L357: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W235 | L358: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W236 | L363: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W237 | L363: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W238 | L364: false -> true | `test_every_entry_point_is_inert_on_null_and_uninitialized_instances` |
| W239 | L370: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W240 | L370: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W241 | L374: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W242 | L375: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W243 | L376: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W244 | L381: && -> \|\| | `test_iso_worker_logic_mut:crash/rc=1` |
| W245 | L381: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W246 | L382: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W247 | L387: && -> \|\| | `test_iso_worker_logic_mut:crash/rc=1` |
| W248 | L387: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W249 | L388: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W250 | L394: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W251 | L394: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W252 | L395: negate if condition | `test_every_entry_point_is_inert_on_null_and_uninitialized_instances` |
| W253 | L400: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W254 | L409: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W255 | L409: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W256 | L409: \|\| -> && | `test_every_entry_point_is_inert_on_null_and_uninitialized_instances`, `test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer` |
| W257 | L409: \|\| -> && | `test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer` |
| W258 | L409: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W259 | L409: const 0+1 -> 1 | `test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer` |
| W260 | L412: < -> <= | `test_iso_worker_logic_mut:crash/rc=1` |
| W261 | L412: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W262 | L413: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W263 | L413: != -> == | `test_iso_worker_logic:crash/rc=1` |
| W264 | L413: && -> \|\| | `test_sixteen_ready_images_are_published_looked_up_and_released`, `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| W265 | L413: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W266 | L423: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W267 | L423: \|\| -> && | `test_iso_worker_logic_mut:crash/rc=1` |
| W268 | L423: \|\| -> && | `test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer` |
| W269 | L423: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W270 | L423: const 0+1 -> 1 | `test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer` |
| W271 | L426: < -> <= | `test_iso_worker_logic_mut:crash/rc=1` |
| W272 | L426: const 0+1 -> 1 | `test_iso_worker_logic:crash/rc=1` |
| W273 | L427: == -> != | `test_iso_worker_logic:crash/rc=1` |
| W274 | L427: != -> == | `test_iso_worker_logic:crash/rc=1` |
| W275 | L427: && -> \|\| | `test_sixteen_ready_images_are_published_looked_up_and_released`, `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| W276 | L427: negate if condition | `test_iso_worker_logic:crash/rc=1` |
| W277 | L428: delete statement | `test_iso_worker_logic:crash/rc=1` |
| W278 | L436: && -> \|\| | `test_iso_worker_logic_mut:crash/rc=1` |
| W279 | L440: && -> \|\| | `test_iso_worker_logic_mut:crash/rc=1` |
| WC001 | L7: os.ctx -> NULL | `test_deinit_releases_the_published_tables_itself_and_is_idempotent`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| WC002 | L54: os.ctx -> NULL | `test_init_rejects_null_arguments_and_every_missing_mandatory_field`, `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| WC003 | L77: os.ctx -> NULL | `test_init_clears_the_whole_struct_and_defaults_the_step_budget`, `test_deinit_releases_the_published_tables_itself_and_is_idempotent` |
| WC004 | L79: os.ctx -> NULL | `test_init_rejects_null_arguments_and_every_missing_mandatory_field`, `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| WC005 | L92: os.ctx -> NULL | `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| WC006 | L95: os.ctx -> NULL | `test_is_mounted_hook_compares_the_loaded_image_id_only` |
| WC007 | L167: os.ctx -> NULL | `test_init_rejects_null_arguments_and_every_missing_mandatory_field`, `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| WC008 | L169: os.ctx -> NULL | `test_init_rejects_null_arguments_and_every_missing_mandatory_field`, `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| WC009 | L173: os.ctx -> NULL | `test_init_rejects_null_arguments_and_every_missing_mandatory_field`, `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| WC010 | L181: os.ctx -> NULL | `test_init_rejects_null_arguments_and_every_missing_mandatory_field`, `test_init_clears_the_whole_struct_and_defaults_the_step_budget` |
| WC011 | L195: os.ctx -> NULL | `test_deinit_releases_the_published_tables_itself_and_is_idempotent`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| WC012 | L203: os.ctx -> NULL | `test_boot_dirs_failure_publishes_the_sd_error_through_the_override_only`, `test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook` |
| WC013 | L206: os.ctx -> NULL | `test_boot_dirs_failure_publishes_the_sd_error_through_the_override_only`, `test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook` |
| WC014 | L213: os.ctx -> NULL | `test_deinit_releases_the_published_tables_itself_and_is_idempotent`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| WC015 | L216: os.ctx -> NULL | `test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook` |
| WC016 | L217: os.ctx -> NULL | `test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook` |
| WC017 | L218: os.ctx -> NULL | `test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook` |
| WC018 | L235: os.ctx -> NULL | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC019 | L237: os.ctx -> NULL | `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC020 | L243: os.ctx -> NULL | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC021 | L247: os.ctx -> NULL | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC022 | L278: os.ctx -> NULL | `test_request_reprobe_sets_the_flag_and_wakes_the_worker`, `test_reprobe_reinitializes_while_a_fault_override_is_published_even_if_the_card_reads_ready` |
| WC023 | L282: os.ctx -> NULL | `test_request_reprobe_sets_the_flag_and_wakes_the_worker`, `test_reprobe_reinitializes_while_a_fault_override_is_published_even_if_the_card_reads_ready` |
| WC024 | L289: os.ctx -> NULL | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC025 | L291: os.ctx -> NULL | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC026 | L296: os.ctx -> NULL | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC027 | L301: os.ctx -> NULL | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| WC028 | L308: os.ctx -> NULL | `test_deinit_releases_the_published_tables_itself_and_is_idempotent`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| WC029 | L312: os.ctx -> NULL | `test_request_reprobe_sets_the_flag_and_wakes_the_worker`, `test_run_once_without_a_pending_flag_does_not_reinit_a_card_that_is_not_ready` |
| WC030 | L326: os.ctx -> NULL | `test_deinit_releases_the_published_tables_itself_and_is_idempotent`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| WC031 | L337: os.ctx -> NULL | `test_deinit_releases_the_published_tables_itself_and_is_idempotent`, `test_sixteen_ready_images_are_published_looked_up_and_released` |
| WC032 | L340: os.ctx -> NULL | `test_iso_worker_logic_mut:crash/rc=1` |
| WC033 | L352: os.ctx -> NULL | `test_iso_worker_logic_mut:crash/rc=1` |
| WC034 | L355: os.ctx -> NULL | `test_iso_worker_logic_mut:crash/rc=1` |
| WC035 | L366: os.ctx -> NULL | `test_sixteen_ready_images_are_published_looked_up_and_released`, `test_view_publish_hands_over_the_consumed_since_of_the_upload` |
| WC036 | L374: os.ctx -> NULL | `test_sixteen_ready_images_are_published_looked_up_and_released`, `test_view_publish_hands_over_the_consumed_since_of_the_upload` |
| WC037 | L382: os.ctx -> NULL | `test_sixteen_ready_images_are_published_looked_up_and_released`, `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| WC038 | L388: os.ctx -> NULL | `test_sixteen_ready_images_are_published_looked_up_and_released`, `test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images` |
| X01 | pending: queue_len term dropped | `test_iso_worker_logic:crash/rc=1` |
| X02 | pending: index_pending term dropped | `test_iso_worker_logic:crash/rc=1` |
| X03 | pending: cur_active term dropped | `test_iso_worker_logic_mut:crash/rc=1` |
| X04 | pending: reinit_pending term dropped | `test_a_fault_raised_during_the_reinit_keeps_the_override_and_the_pending_flag` |
| X05 | pending: reprobe_pending term dropped | `test_pending_work_wakes_the_worker_when_the_op_lock_is_released_or_on_tick` |
| X06 | init: rnd not passed to the catalog | `test_iso_worker_logic:crash/rc=1` |
| X07 | init: media force-eject ctx NULL | `test_iso_worker_logic:crash/rc=1` |
| X08 | init: core bdev dropped | `test_init_wires_every_cfg_field_into_the_core_the_catalog_and_the_media` |
| X09 | init: core sha dropped | `test_worker_boot_upload_finish_and_table_map`, `test_worker_delete_waits_for_msc_ref_and_clears_after_target_eject` |
| X10 | init: core io_buf_size dropped | `test_worker_boot_upload_finish_and_table_map`, `test_worker_delete_waits_for_msc_ref_and_clears_after_target_eject` |
| X11 | init: core sd dropped | `test_iso_worker_logic:crash/rc=1` |
| X12 | init: core chunk_bytes dropped | `test_init_wires_every_cfg_field_into_the_core_the_catalog_and_the_media` |
| X13 | init: core sync_bytes dropped | `test_init_wires_every_cfg_field_into_the_core_the_catalog_and_the_media` |
| X14 | init: core hook ctx NULL | `test_iso_worker_logic:crash/rc=1` |
| X15 | init: core view_update hook dropped | `test_iso_worker_logic:crash/rc=1` |
| X16 | init: core is_mounted hook dropped | `test_iso_worker_logic:crash/rc=1` |
| X17 | init: core clock dropped | `test_worker_boot_upload_finish_and_table_map`, `test_worker_delete_waits_for_msc_ref_and_clears_after_target_eject` |
| X18 | boot: ready check inverted | `test_iso_worker_logic:crash/rc=1` |
| X19 | boot: NULL sd taken as ready (\|\| instead of &&) | `test_iso_worker_logic_mut:crash/rc=1` |
| X20 | boot: ready without the NULL sd guard | `test_iso_worker_logic_mut:crash/rc=1` |
| X21 | boot: override value NO_CARD instead of ERROR | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| X22 | boot: hw sd state NO_CARD instead of ERROR | `test_worker_boot_ensure_dirs_failure_triggers_reprobe_recovery` |
| X23 | notify: ternary branches swapped | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` |
| X24 | notify: READY mapped to UNSUPPORTED_FS | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes` |
| X25 | notify: hw state gets the raw fault_state | `test_iso_worker_logic_mut:crash/rc=1` |
| X26 | notify: view callback gets consumed_since 0 | `test_iso_worker_logic_mut:crash/rc=1` |
| X27 | run_once: already_ready ignores the override | `test_reprobe_reinitializes_while_a_fault_override_is_published_even_if_the_card_reads_ready` |
| X28 | run_once: already_ready needs the override | `test_iso_worker_logic:crash/rc=1` |
| X29 | run_once: already_ready state compare inverted | `test_iso_worker_logic:crash/rc=1` |
| X30 | run_once: already_ready without the NULL sd guard | `test_iso_worker_logic_mut:crash/rc=1` |
| X31 | run_once: remount_ctx not forwarded | `test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes`, `test_run_once_accepts_null_out_pointers_on_every_path` |
| X32 | run_once: remount not forwarded | `test_iso_worker_logic:crash/rc=1` |
| X33 | run_once: fault_raced always false | `test_a_fault_raised_during_the_reinit_keeps_the_override_and_the_pending_flag` |
| X34 | run_once: fault_raced always true | `test_iso_worker_logic:crash/rc=1` |
| X35 | run_once: step budget 0 | `test_finish_resume_steps_follow_the_configured_budget` |
| X36 | run_once: step budget 1 | `test_iso_worker_logic_mut:crash/rc=1` |
| X37 | run_once: step budget is the raw cfg value | `test_finish_resume_steps_follow_the_configured_budget` |
| X38 | run_once: job id copy loses the NUL | `test_iso_worker_logic:crash/rc=1` |
| X39 | run_once: error callback reads the kind after the step | `test_iso_worker_logic:crash/rc=1` |
| X40 | run_once: error callback reads the id after the step | `test_iso_worker_logic:crash/rc=1` |
| X41 | run_once: error callback gets 0 instead of the error | `test_iso_worker_logic:crash/rc=1` |
| X42 | run_once: waiting always false | `test_iso_worker_logic:crash/rc=1` |
| X43 | run_once: waiting always true | `test_iso_worker_logic_mut:crash/rc=1` |
| X44 | tick: now_ms not forwarded | `test_iso_worker_logic:crash/rc=1` |
| X45 | trylock: wait_ms not forwarded | `test_op_trylock_forwards_the_wait_time_and_the_result` |
| X46 | trylock: result inverted | `test_iso_worker_logic:crash/rc=1` |
| X47 | lookup: returns the id instead of the size | `test_iso_worker_logic:crash/rc=1` |
| X48 | view hook: slot size from the id | `test_iso_worker_logic:crash/rc=1` |
| X49 | view hook: slot id_u32 not stored | `test_iso_worker_logic:crash/rc=1` |
| X50 | deinit: the failing remount succeeds (rescan) | `test_deinit_cancels_the_jobs_and_clears_the_catalog_and_the_published_view` |
| X51 | deinit: no sd_reinit | `test_iso_worker_logic:crash/rc=1` |
| X52 | deinit: remount callback dropped | `test_deinit_cancels_the_jobs_and_clears_the_catalog_and_the_published_view` |

## 3. 実装について気づいたこと(レビューで確認してほしい)

**バグは見つからなかった。** 次は、バグではないが、確認してほしい点。

1. **`iso_worker_logic_boot` を 2 回呼ぶと、1 回目の `ensure_dirs` の失敗の上書き(`sd_override_non_ready` / `ERROR`)が、2 回目の成功では解除されない**(公開ビューの `sd_state` が `ERROR` のまま。2 回目は `ISO_OK` を返す)。再現: `ensure_dirs` が `ISO_ERR_IO` を返す状態で `boot` → 返り値 `ISO_ERR_IO`、ビューは `ERROR`(期待どおり)。失敗を直して `E.sd_state = READY` にし、もう一度 `boot` → 返り値 `ISO_OK`、`sd_override_non_ready == true`、ビューの `sd_state == ERROR`(期待: `READY`)。`iso_worker.c` は `boot` を 1 回しか呼ばず、回復は `request_reprobe` → `run_once`(上書きを解除する)なので、**実害はない**。`boot` を再呼び出し可能にするなら、成功の経路で上書きを解除する。
2. **ステップの予算(`step_budget_bytes`)が効くのは、`FINISH` のうち、ディスクからパーツを読み直して SHA-256 を計算するステップだけ**。アップロードを受信しながら SHA-256 を計算できた通常の `FINISH` は、予算に関係なく、同じステップ数で終わる(`test_finish_resume_steps_follow_the_configured_budget` は、ディスクから読み直す経路(再起動後の再開、`incomplete` の `FINISH`)で固定している)。設計(`ISO_MOUNT_DESIGN.md` §4.2)の記述と矛盾しない。
3. `deinit` の公開表の後始末(`pub_tables` の `unref`)は、通常は、直前の `iso_core_sd_reinit` の最後の公開(カタログが空)が、すでに行っているので、**デッドコードに近い防御**。公開のフックを外したテスト(`test_deinit_releases_the_published_tables_itself_and_is_idempotent`)で固定してある。
4. `iso_worker_logic_notify_sd_fault` は、`pub_view` の `images[]` / `count` を変えず、`sd_state` だけを上書きする。再初期化が済むまでは、ビューにイメージの一覧が残る(`mount` と `begin` は `sd_state` を見て 503 にするので、設計(§3.4 ①)のとおり)。
5. テストの部品は、`os.ctx` に `&E` を渡して、全コールバックで一致を検査する(A-13 のテストは `NULL` を渡す)。`iso_worker.c` は、`os.ctx` も `run_once` の `remount_ctx` も `NULL` を渡す(今の配線では、ctx の取り違えは結果に出ない)が、将来 ctx を使うようになったとき、渡し間違い(`NULL` にする、別の値にする)を検出する。
