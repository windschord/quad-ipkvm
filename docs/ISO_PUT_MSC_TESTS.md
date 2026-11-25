# web_iso_put_logic と iso_usb_msc_glue のテストとミューテーションテストの結果

作成: 2026-11-24(C-16)。対象は純 C の 2 ファイル(実装のコードは変えていない。テストと道具と文書だけ):

- `components/web_server/web_iso_put_logic.c`(約 70 行。PUT のチャンク受信ループの期限(`web_iso_put_recv_all`)と、5% ごとの進捗(`web_iso_progress_step_pct`))
- `components/iso_usb_msc/iso_usb_msc_glue.c`(MSC の BOT(`msc_bot`)と `mmc_scsi` と `iso_extent` の橋: コマンドの実行の写し、リゾルバ、`iso_extent` の表を引いて SD を読むバックエンド、世代(epoch)の一致の検査、USB 切断での `prevent_removal` の解除)

`web_iso_service.c`(esp_http_server、`SO_RCVTIMEO`)と `iso_usb_msc.c`(TinyUSB、FreeRTOS)は IDF 専用で、ホストではテストできないので対象外(実機の確認は `HW_VERIFY.md`)。

## 1. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/web_server/test_web_iso_put_logic.c`(A-15) | 受信ループの基本(1 回で完了、分割、最小スライス、期限、IO エラーと EOF、読みすぎ、時計の逆行、不正な引数)、進捗の 5% 刻み、大きい合計 | 7 |
| `test/host/web_server/test_web_iso_put_logic_mut.c`(C-16。このドキュメント) | ミューテーションテストの生き残りを殺すためのテスト(2.4) | 18 |
| `test/host/iso_usb_msc/test_iso_usb_msc_glue.c`(A-14 など) | BOT のトランザクション全体(INQUIRY、マウント、READ(10) / (12)、非 2048 の末尾、リセット、断片の多い表、START STOP UNIT、リゾルバ、epoch、切断、force_eject) | 11 |
| `test/host/iso_usb_msc/test_iso_usb_msc_glue_mut.c`(C-16。このドキュメント) | 同上(3.4) | 26 |

合計 62 件。ホストのテスト全体は ctest で 129 件(既存の自動発見(`test/host/CMakeLists.txt` の `GLOB_RECURSE`)で、実行ファイル `test_iso_usb_msc_glue_mut` が増える。`test_web_iso_put_logic_mut` は、`web_iso_put_logic` のライブラリを結ぶ条件(`test_web_iso_put_logic.c` の名前の一致)を `test_web_iso_put_logic(_mut)?\.c` に広げた)。ASan / UBSan、`-Werror`、`-fno-sanitize-recover=undefined`。期待値は手書き。パディングの値に依存するアサーションはない(構造体は、欄ごとに比べる)。

テストの後始末: `test_iso_usb_msc_glue_mut.c` は、フィクスチャ(表、メディア、バックエンド、`mmc_scsi`)が生きているかを `FX_LIVE` で持ち、アサーションが失敗しても `tearDown` が参照と表を放す(`test_iso_worker_logic_mut.c` の `W_LIVE` と同じ。後始末の検査(リーク、ロックの釣り合い)は、`fx_teardown` が、正常終了のときだけ行う)。`test_web_iso_put_logic_mut.c` は、確保するものがない。

## 2. web_iso_put_logic.c のミューテーションテスト

方法: `tools/test/mutate_iso_put.py` が、`components/` と `test/` をスクラッチにコピーし、`web_iso_put_logic.c` の 1 か所を置換 → 再ビルド → 2 個のテストの実行ファイル(`test_web_iso_put_logic_mut`、`test_web_iso_put_logic`)を順に実行、を繰り返す(`mutate_iso_extent.py` の `main()`。1 つ落ちれば殺した。**ビルドエラーは殺したに数えない**)。新しいテストを先に実行するのは、終わらないミュータント(ループを抜けない)を、モックの呼び出しの上限で早く止めるため。元のテストだけでの結果は、下の「経緯」。

- `tools/test/iso_put_mutants.py`(**機械が作る**): `tools/test/gen_iso_mutants.py put .` が、ソースから作る(演算子の置換、`if` 条件の否定、文の削除、定数 ±1、真偽値の反転。**79 個**: `P001`〜)。
- `tools/test/iso_put_mutants_extra.py`(**手書き**。**55 個**: `Q01`〜): 機械が作らない形。引数の検査の項を 1 つずつ落とす、戻り値の取り違え(INVALID / IO / TIMEOUT / OK)、三項演算子の枝の入れ替えと落とし、コールバックの引数(`ctx` を NULL、`buf` の位置、`recv` の長さ、`set_timeout` のスライス)、`*last_bucket = bucket;`(行の先頭が `*` なので、機械は作らない)、`return bucket * 5;`、`bucket <= 0 ||` の項を落とす、大きい合計の経路の防御。

```
UNITY_SRC=<unity のソース> FETCHCONTENT_SOURCE_DIR_UNITY=<unity のソース> \
python3 tools/test/mutate_iso_put.py <repo> <スクラッチ> tools/test/iso_put_mutants.py <結果.json> -j2
python3 tools/test/mutate_iso_put.py <repo> <スクラッチ> tools/test/iso_put_mutants_extra.py <結果_extra.json> -j2
```

結果の JSON とログ(1 行 1 ミュータント)は、`tools/test/results/c16/` に置いた: `put.json`(P)、`put_extra.json`(Q)、`msc.json`(G)、`msc_extra.json`(Y)。元のテストだけで実行した結果は、環境変数 `MUTATE_ORIGINAL_ONLY=1` をつけて実行したもの(`*_orig.json`)。

`mutate_iso_put.py` は、ビルドに `-ftrivial-auto-var-init=pattern` を足す(`mutate_iso_worker.py` と同じ。通常の ctest のビルドには入れない)。

経緯: A-15 のテストだけ(`test_web_iso_put_logic`。7 件)で実行したときに殺した数: P 61 / 79、Q 46 / 55(殺した数 / 実行した数)。生き残りを 1 つずつ調べ、**同値でないものは、テストを足して殺した**(2.4 の 18 件)。主な穴: **期限の境界**(経過が `timeout - 1` と `timeout`)、**最小のスライスの境界**(`MIN - 1`、`MIN`、`MIN + 1`)、**recv に渡す位置と長さ**、**読みすぎ(`r == rem` と `rem + 1`)**、**`IO_TIMEOUT` 以外の負の値**、**`recv` が時間切れの後の、時計の読み直し**(経過 0、`T - 1`、`T`、時計の戻り)、**進捗の `last_bucket` の更新と抑止**、**`UINT64_MAX / 100` の境界での計算の切り替え**。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| | 実行した数 | 殺した数 | 生き残った数 | 同値と判断 | 殺せない(同値ではない) | ビルドエラー(コンパイラが検出) |
|---|---|---|---|---|---|---|
| 機械(P) | 79 | 70 | 9 | 9 | 0 | 0 |
| 手書き(Q) | 55 | 53 | 2 | 2 | 0 | 0 |
| **合計** | 134 | 123 | 11 | 11 | 0 | 0 |

### 2.1 同値のミュータント(11 個)

生き残ったものは、1 つずつ、変更の前後の挙動が同じ理由を確かめた。**配列の外の読み書きだけが違うものは、同値にしていない**(このファイルには、そのようなものはない)。

| ID | 同値の理由 |
|---|---|
| P020 | `rem_us < MIN` → `<=`: 残りがちょうど `WEB_ISO_PUT_MIN_SLICE_US` のとき、どちらの枝も同じ値(MIN)になる |
| P045 | `received >= total` → `>`: `received == total` は、次の枝でも 100 になる(`total <= UINT64_MAX / 100` なら `total * 100 / total = 100`。それより大きいとき、`denom = total / 100`、`total = 100 * denom + r`(`r < 100`)で、`total / denom = 100 + r / denom = 100`) |
| P047 | `pct = 100` → 101: `bucket = pct / 5` は 20 のまま(101 / 5 = 20)。返す値は `bucket * 5 = 100` |
| P061 | `denom > 0` → `>=`: `denom == 0` は、`total < 100` のときだけ。この枝に来るのは `received > UINT64_MAX / 100` かつ `received < total` なので、`total > UINT64_MAX / 100`、`denom > UINT64_MAX / 10000`。**到達しない** |
| P063 | `raw > 100` → `>=`: `raw` は 100 を超えない(`received < total = 100 * denom + r` なので `raw <= 100 + (r - 1) / denom = 100`)。`raw == 100` のとき、どちらも 100 |
| P064 | `raw > 100` → `> 101`: `raw > 100` は到達しない(上の P063)ので、どちらも `(int)raw` |
| P065 | `raw > 100` → `> 99`: `raw == 100` のとき、どちらも 100 |
| P066 | `? 100 : ...` の 100 → 101: `raw > 100` が真になることがない(P063)。到達しない |
| P067 | 同上(100 → 99) |
| Q54 | ゼロ除算の防御(`denom > 0`)を外す: P061 と同じ理由で、`denom == 0` に到達しない |
| Q55 | `raw > 100` の頭打ちを外す: P063 と同じ理由で、`raw` は 100 を超えない |

### 2.2 殺せないが、同値でもないミュータント(0 個)

なし。

### 2.3 ビルドエラー(0 個)

なし。

### 2.4 足したテスト(18 件。`test/host/web_server/test_web_iso_put_logic_mut.c`)

モック: 時計は `now_us` の呼び出しごとに決めた値を返す。`recv` は決めた戻り値を返し、正のときペイロードを(`max_len` まで)`buf` に写す。呼び出しの順序(`N` = now_us、`S` = set_timeout_us、`R` = recv)、スライスの値、`recv` の位置と長さ、`ctx` の取り違えを記録する。決めた数を超えて呼ばれたら、そのテストを失敗にする(ループの終わらないミュータントを、タイムアウトを待たずに殺す)。

| テスト | 内容 |
|---|---|
| `test_invalid_arguments_are_rejected_without_calling_any_callback` | 8 種類の不正(`io`、`now_us`、`recv`、`buf`、`want_len == 0`、`want_len > cap`、`cap == 0`、`timeout == 0`)は `INVALID`。コールバックを何も呼ばない |
| `test_boundary_arguments_are_accepted` | `want_len == cap`、`timeout_us == 1` は有効(残り 1 us は最小のスライスに引き上げる) |
| `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` | 経過 `T - 1` は続ける、`T` と `T + 1` は `TIMEOUT`(recv も set_timeout もしない)、経過 0 のスライスは `T` |
| `test_slice_is_the_remaining_time_but_never_below_the_minimum` | 残り `MIN - 1` / `MIN` / `MIN + 1` / `3 MIN` / 任意 / `UINT64_MAX` のスライス |
| `test_set_timeout_runs_before_every_recv_and_is_optional` | 周回ごとに `S` の後に `R`。`set_timeout_us == NULL` でも動く |
| `test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns` | `recv` の位置(0、3、7)と最大の長さ(10、7、3)。`cap` より小さい `want_len` で、後ろを書かない |
| `test_a_read_longer_than_requested_is_an_io_error_but_exactly_the_rest_is_fine` | 残り + 1 は `IO`(最初と途中)、残りちょうどは成功 |
| `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error` | 0、-1、-3、-4、-100、`INT_MIN` は `IO`(最初と、データを受け取った後) |
| `test_a_recv_timeout_retries_until_the_total_deadline_and_the_slice_shrinks` | `IO_TIMEOUT` を繰り返しても期限まで続け、スライスが `T`、`T - 300 ms`、`T - 600 ms` と縮む |
| `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` | `IO_TIMEOUT` の後の読み直しで、経過 `T - 1` は続ける、`T` は直ちに `TIMEOUT`(それ以上 recv も時計の読みもしない)、時計が戻れば `UINT64_MAX` の期限でも `TIMEOUT` |
| `test_a_recv_timeout_with_a_clock_that_has_not_moved_keeps_waiting` | 経過 0 の `IO_TIMEOUT` は、期限ではない |
| `test_a_clock_that_goes_backwards_at_the_top_of_the_loop_is_a_timeout` | ループの先頭で時計が戻る(100 us)と `TIMEOUT`(`UINT64_MAX` の期限でも。引き算の回り込みに頼らない) |
| `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket` | `total == 0`、`last_bucket == NULL` は -1 で、バケットを変えない |
| `test_progress_matches_an_integer_model_for_every_byte_of_small_totals` | 合計 1、3、7、19、20、21、99、100、101、999、1000 の全バイトで、整数の模型(`received * 100 / total / 5`)と一致。最後は 100% |
| `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` | 0〜4% は出さない、同じ・戻ったバケットは -1 でバケットを変えない、飛び越し、最初の `last_bucket` が 20 / 0 / -100 / 1 のとき |
| `test_progress_received_beyond_the_total_is_one_hundred_percent` | `received > total` は 100(一度だけ) |
| `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100` | `UINT64_MAX / 100` ちょうどは正確な計算(4.99..% は出さない)、1 つ上は割り算を先にする計算(回り込まず 50%) |
| `test_progress_large_totals_use_the_division_first_path_and_stay_in_range` | 合計 1e19 で、4.99..% / 5% / 10% / 50% / 99.99..% / 100% |

### 2.5 殺したミュータントの一覧(123 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(最初に落ちたテストの実行ファイルで止める)。`crash/rc=1` は、そのテストの実行ファイルが、`TEST_ASSERT` の失敗の後に、ASan / UBSan で異常終了した(または、失敗の表示が出る前に落ちた)もの。変更箇所の `L` は、変更前のソースの行。`P` はソースから機械が作ったもの、`Q` は手書き。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| P001 | L8: == -> != | `test_invalid_arguments_are_rejected_without_calling_any_callback`, `test_boundary_arguments_are_accepted` |
| P002 | L8: == -> != | `test_invalid_arguments_are_rejected_without_calling_any_callback`, `test_boundary_arguments_are_accepted` |
| P003 | L8: > -> >= | `test_boundary_arguments_are_accepted`, `test_set_timeout_runs_before_every_recv_and_is_optional` |
| P004 | L8: \|\| -> && | `test_web_iso_put_logic_mut:crash/rc=1` |
| P005 | L8: \|\| -> && | `test_web_iso_put_logic_mut:crash/rc=1` |
| P006 | L8: \|\| -> && | `test_web_iso_put_logic_mut:crash/rc=1` |
| P007 | L8: \|\| -> && | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| P008 | L8: \|\| -> && | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| P009 | L8: \|\| -> && | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| P010 | L8: negate if condition | `test_web_iso_put_logic_mut:crash/rc=1` |
| P011 | L8: const 0+1 -> 1 | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| P012 | L8: const 0+1 -> 1 | `test_invalid_arguments_are_rejected_without_calling_any_callback`, `test_boundary_arguments_are_accepted` |
| P013 | L13: const 0+1 -> 1 | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P014 | L15: < -> <= | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P015 | L17: >= -> > | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P016 | L17: - -> + | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P017 | L18: >= -> > | `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary`, `test_a_clock_that_goes_backwards_at_the_top_of_the_loop_is_a_timeout` |
| P018 | L18: negate if condition | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P019 | L22: - -> + | `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary`, `test_slice_is_the_remaining_time_but_never_below_the_minimum` |
| P021 | L24: negate if condition | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P022 | L25: delete statement | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P023 | L28: - -> + | `test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns`, `test_a_read_longer_than_requested_is_an_io_error_but_exactly_the_rest_is_fine` |
| P024 | L29: + -> - | `test_web_iso_put_logic_mut:crash/rc=1` |
| P025 | L30: > -> >= | `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error` |
| P026 | L30: negate if condition | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P027 | L30: const 0+1 -> 1 | `test_set_timeout_runs_before_every_recv_and_is_optional` |
| P028 | L31: > -> >= | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P029 | L31: negate if condition | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P030 | L34: += -> -= | `test_set_timeout_runs_before_every_recv_and_is_optional`, `test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns` |
| P031 | L34: delete statement | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| P032 | L35: == -> != | `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error`, `test_a_recv_timeout_retries_until_the_total_deadline_and_the_slice_shrinks` |
| P033 | L35: negate if condition | `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error`, `test_a_recv_timeout_retries_until_the_total_deadline_and_the_slice_shrinks` |
| P034 | L37: >= -> > | `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| P035 | L37: < -> <= | `test_a_recv_timeout_with_a_clock_that_has_not_moved_keeps_waiting` |
| P036 | L37: \|\| -> && | `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| P037 | L37: negate if condition | `test_a_recv_timeout_retries_until_the_total_deadline_and_the_slice_shrinks`, `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| P038 | L37: - -> + | `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| P039 | L49: == -> != | `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket`, `test_progress_matches_an_integer_model_for_every_byte_of_small_totals` |
| P040 | L49: \|\| -> && | `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket` |
| P041 | L49: negate if condition | `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket`, `test_progress_matches_an_integer_model_for_every_byte_of_small_totals` |
| P042 | L49: const 0+1 -> 1 | `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket`, `test_progress_matches_an_integer_model_for_every_byte_of_small_totals` |
| P043 | L50: const 1+1 -> 2 | `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket` |
| P044 | L50: const 1-1 -> 0 | `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket` |
| P046 | L54: negate if condition | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P048 | L55: const 100-1 -> 99 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P049 | L55: delete statement | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P050 | L56: <= -> < | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100` |
| P051 | L56: negate if condition | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100` |
| P052 | L56: / -> * | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100`, `test_progress_large_totals_use_the_division_first_path_and_stay_in_range` |
| P053 | L56: const 100+1 -> 101 | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100` |
| P054 | L56: const 100-1 -> 99 | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100` |
| P055 | L57: * -> / | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P056 | L57: / -> * | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P057 | L57: const 100+1 -> 101 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100` |
| P058 | L57: const 100-1 -> 99 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P059 | L57: delete statement | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P060 | L59: / -> * | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100`, `test_progress_large_totals_use_the_division_first_path_and_stay_in_range` |
| P062 | L60: / -> * | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100`, `test_progress_large_totals_use_the_division_first_path_and_stay_in_range` |
| P068 | L61: delete statement | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100`, `test_progress_large_totals_use_the_division_first_path_and_stay_in_range` |
| P069 | L64: / -> * | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P070 | L65: <= -> < | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P071 | L65: <= -> < | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P072 | L65: \|\| -> && | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P073 | L65: negate if condition | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P074 | L65: const 0+1 -> 1 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P075 | L66: const 1+1 -> 2 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P076 | L66: const 1-1 -> 0 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P077 | L69: * -> / | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P078 | L69: const 5+1 -> 6 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| P079 | L69: const 5-1 -> 4 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q01 | args: now_us term dropped | `test_web_iso_put_logic_mut:crash/rc=1` |
| Q02 | args: recv term dropped | `test_web_iso_put_logic_mut:crash/rc=1` |
| Q03 | args: buf term dropped | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| Q04 | args: want_len == 0 term dropped | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| Q05 | args: want_len > cap term dropped | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| Q06 | args: timeout_us == 0 term dropped | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| Q07 | args: INVALID returns IO | `test_invalid_arguments_are_rejected_without_calling_any_callback` |
| Q08 | args: want_len > cap becomes want_len + 1 > cap | `test_boundary_arguments_are_accepted`, `test_set_timeout_runs_before_every_recv_and_is_optional` |
| Q09 | clock: start_us fixed at 0 | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q10 | clock: start read gets a NULL ctx | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q11 | clock: loop read gets a NULL ctx | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q12 | clock: post-timeout read gets a NULL ctx | `test_a_recv_timeout_retries_until_the_total_deadline_and_the_slice_shrinks`, `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| Q13 | clock: backwards clock treated as zero elapsed | `test_a_clock_that_goes_backwards_at_the_top_of_the_loop_is_a_timeout` |
| Q14 | clock: backwards guard dropped (wraps) | `test_a_clock_that_goes_backwards_at_the_top_of_the_loop_is_a_timeout` |
| Q15 | clock: loop-top timeout returns IO | `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary`, `test_a_clock_that_goes_backwards_at_the_top_of_the_loop_is_a_timeout` |
| Q16 | clock: post-recv timeout returns IO | `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| Q17 | clock: post-recv backwards term dropped | `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| Q18 | clock: post-recv elapsed term dropped | `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| Q19 | clock: post-recv check never ends the loop | `test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read` |
| Q20 | slice: no minimum clamp | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q21 | slice: always the minimum | `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary`, `test_slice_is_the_remaining_time_but_never_below_the_minimum` |
| Q22 | slice: ternary branches swapped | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q23 | slice: set_timeout gets a NULL ctx | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q24 | slice: set_timeout gets the unclamped remainder | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q25 | slice: set_timeout gets the whole timeout | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q26 | slice: remainder uses want_len | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q27 | recv: buf offset dropped | `test_set_timeout_runs_before_every_recv_and_is_optional`, `test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns` |
| Q28 | recv: asks for the whole want_len | `test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns` |
| Q29 | recv: NULL ctx | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q30 | recv: over-read returns TIMEOUT | `test_a_read_longer_than_requested_is_an_io_error_but_exactly_the_rest_is_fine` |
| Q31 | recv: over-read accepted and clamped | `test_a_read_longer_than_requested_is_an_io_error_but_exactly_the_rest_is_fine` |
| Q32 | recv: counts the whole remainder | `test_set_timeout_runs_before_every_recv_and_is_optional`, `test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns` |
| Q33 | recv: every negative result is a timeout | `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error` |
| Q34 | recv: every non-positive result is a timeout | `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error` |
| Q35 | recv: IO error returns TIMEOUT | `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error` |
| Q36 | recv: IO error is retried | `test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error` |
| Q37 | recv: success returns the byte count | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q38 | recv: success returns IO | `test_boundary_arguments_are_accepted`, `test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary` |
| Q39 | progress: invalid returns 0 | `test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket` |
| Q40 | progress: suppressed returns 0 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q41 | progress: last_bucket not updated | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q42 | progress: last_bucket gets the percentage | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q43 | progress: last_bucket gets the milestone | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q44 | progress: returns the percentage, not the milestone | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q45 | progress: returns the bucket | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q46 | progress: bucket 0 not suppressed | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q47 | progress: repeated bucket not suppressed | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q48 | progress: bucket width 10 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q49 | progress: bucket width 4 | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q50 | progress: total + 1 in the exact path | `test_progress_matches_an_integer_model_for_every_byte_of_small_totals`, `test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it` |
| Q51 | progress: pct from the wrong operand | `test_web_iso_put_logic_mut:crash/rc=1` |
| Q52 | progress: large path divides by total | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100`, `test_progress_large_totals_use_the_division_first_path_and_stay_in_range` |
| Q53 | progress: large path rounds the divisor up | `test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100`, `test_progress_large_totals_use_the_division_first_path_and_stay_in_range` |

## 3. iso_usb_msc_glue.c のミューテーションテスト

方法は 2 章と同じ(`tools/test/mutate_iso_msc.py`。実行ファイルは `test_iso_usb_msc_glue_mut`、`test_iso_usb_msc_glue`)。

- `tools/test/iso_msc_mutants.py`(**機械が作る**): `tools/test/gen_iso_mutants.py msc .`。**182 個**: `G001`〜。
- `tools/test/iso_msc_mutants_extra.py`(**手書き**。**147 個**: `Y01`〜): 方向の対応表の戻り値、`mmc_scsi_execute` / `mmc_scsi_read_blocks` / `iso_extent_map` / `read_sectors` / `iso_extent_valid_bytes` への引数、結果の欄の値、行の先頭が `*` の文(`*handle = ...`、`*out = ...`)、リゾルバの関数ポインターの取り違えと呼ぶ前のロックの解放、`ISO_ERR_*` の戻り値の取り違え、`set_image` の参照とロックの順序、ゼロ埋めの範囲(1 バイトの過不足)、`iso_usb_msc_on_disconnect_media` が呼ぶ関数、`as_scsi` の欄、`init` / `set_resolver` の欄。

```
python3 tools/test/mutate_iso_msc.py <repo> <スクラッチ> tools/test/iso_msc_mutants.py <結果.json> -j2
python3 tools/test/mutate_iso_msc.py <repo> <スクラッチ> tools/test/iso_msc_mutants_extra.py <結果_extra.json> -j2
```

経緯: A-14 のテストだけ(`test_iso_usb_msc_glue`。11 件)で殺した数: G 126 / 182、Y 79 / 147(殺した数 / 実行した数)。生き残りを調べて、同値でないものはテストを足して殺した(3.4 の 26 件)。**注意**: `mmc_scsi_read_blocks` は、バックエンドの読み出しの後に、`size_bytes` 以降を 0 で埋め直す。そのため、グルーのファイルの終わり以降のゼロ埋めは、`mmc_scsi` を通すと見えず、A-14 のテストでは検査できていなかった。新しいテストは、バックエンドの `read`(`iso_usb_msc_extent_backend_as_scsi` の関数ポインター)を直接呼んで検査する。主な穴: ロックの釣り合いと、リゾルバをロックの外で呼ぶこと、`EXECUTE` の結果の欄(`dev_dir`、`read_lba`、`data_len`)、方向の対応、`lba` が 32 ビットを超える `READ_BLOCKS`、`size_bytes` / `acquire` の引数の検査と戻り値、`be_read` の引数の検査の順序、スパンごとの読み出し(位置、長さ、世代)、断片が 64 を超えたときの縮め方、容量を超える範囲、`set_image` の参照の増減とロックの外の解放、`sync` の条件、`on_disconnect` が呼ぶ関数。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| | 実行した数 | 殺した数 | 生き残った数 | 同値と判断 | 殺せない(同値ではない) | ビルドエラー(コンパイラが検出) |
|---|---|---|---|---|---|---|
| 機械(G) | 182 | 168 | 12 | 11 | 1 | 2 |
| 手書き(Y) | 147 | 135 | 12 | 11 | 1 | 0 |
| **合計** | 329 | 303 | 24 | 22 | 2 | 2 |

### 3.1 同値のミュータント(22 個)

生き残ったものは、1 つずつ、変更の前後の挙動が同じ理由を確かめた。**配列の外の読み書きだけが違うものは、同値にしていない**(3.2)。

| ID | 同値の理由 |
|---|---|
| G016 | `memset(&mr, 0, ...)` → 値 1: `mmc_scsi_execute` が、先頭で `result` 全体を 0 にして埋める(`cdb` は `cmd->cdb`(配列)で NULL ではないので、`-1` で戻って `result` を触らない経路には来ない) |
| G017 | `memset(&mr, 0, ...)` の削除: G016 と同じ |
| G033 | `lba > UINT32_MAX` の分岐の `good = false`: 直前の `memset(out_res, 0, ...)` で、すでに false |
| G037 | `default:` の `good = false`: 同上(`memset` で false) |
| G086 | `size_t n_spans = 0u` → 1: `iso_extent_map` が、成功でも失敗でも、先頭で `*n_spans = 0` にして、最後に値を入れる。初期値は読まれない |
| G090 | `step > 1u` → `>=`: `step == 1` で `SPANS_FULL` になることがない(1 ブロックは高々 4 つの断片。配列は 64 個)。到達しない |
| G093 | `step > 1u` → `> 2u`: `SPANS_FULL` になるのは、17 ブロック以上(16 ブロック = 64 個まで入る)のときだけ。`step > 2` は常に真 |
| G094 | `step > 1u` → `> 0u`: G090 と同じ |
| G095 | `step /= 2u` → `/= 3u`: 縮める割合は、1 回に読む範囲(バッチ)の大きさだけを変える。読むセクタとその順序、出力は同じ |
| G113 | `valid < step_bytes` → `<=`: 等しいとき、`memset` の長さは 0 |
| G175 | `clear` の `set_image(be, 0u, NULL)` の 0 → 1: 表が NULL のとき、`media_id` は引数によらず 0 |
| Y04 | 方向の対応で OUT → NONE: `mmc_scsi` が方向を見るのは `== MMC_DIR_IN` だけ(`mmc_scsi.c` の GET EVENT STATUS NOTIFICATION)。OUT と NONE の区別は、どの結果にも出ない(IN に取り違えるミュータント Y03 は殺した) |
| Y85 | `step > 1u` の条件を外す: G090 と同じ(`step == 1` で `SPANS_FULL` にならない) |
| Y86 | `iso_extent_map` の失敗で `ISO_ERR_IO` の代わりに `rc` を返す: 到達できる失敗は `ISO_EXT_ERR_RANGE`(= -3)だけで、`ISO_ERR_IO` も -3(数値が一致している。他の `ISO_EXT_ERR_*` は、引数が正しいこのグルーからは出ない)。**数値の一致に頼っている点は、3 章で指摘** |
| Y89 | `step /= 2u` → `step -= 1u`: G095 と同じ(バッチの大きさだけ) |
| Y90 | `step /= 2u` → `step = 1u`: G095 と同じ(1 ブロックずつ読むだけ。結果は同じ) |
| Y94 | 断片の合計の超過で `ISO_ERR_NO_CARD` を返す(`ISO_ERR_IO` の代わり): この枝に到達しない(下の Y93 / G112 の理由)ので、戻り値の違いは出ない |
| YA4 | ゼロ埋めの有効バイト数を、`step` ではなく `rem` で求める: `step < rem` になるのは、断片が多すぎて縮めた最初の 1 回だけ。そのとき範囲は、要求の最後のブロックより前(要求は容量の中)なので、全部有効(`valid >= step_bytes`)。どちらでも `memset` は 0 バイト |
| YB3 | `be_release` の `handle != NULL` の防御を外す: `iso_extent_table_unref(NULL)` は 0 を返して何もしない |
| YB5 | `set_image` の、新しい表の NULL の防御を外す: `iso_extent_table_ref(NULL)` は何もしない |
| YB6 | `set_image` の、古い表の NULL の防御を外す: `iso_extent_table_unref(NULL)` は何もしない |
| YC2 | G175 と同じ |

### 3.2 殺せないが、同値でもないミュータント(2 個)

| ID | 変更箇所 | 理由 |
|---|---|---|
| G112 | L177: `bytes_done += span_bytes;` の削除 | `bytes_done` は、直後の `bytes_done + span_bytes > step_bytes`(防御)にしか使われない。この防御は、`iso_extent_map` が契約(断片の合計は `step` ブロックのセクタ数以下)を守る限り、真にならない。契約を破る表は、公開 API では作れない。外すと、契約を破る `iso_extent_map` のときだけ、`dst` が出力の外に書く(配列の外への書き込み)ので、同値とは書かない |
| Y93 | `if (bytes_done + span_bytes > step_bytes) return ISO_ERR_IO;` の削除 | G112 と同じ。この検査そのものが、配列の外への書き込みを防ぐ防御。通常の入力では結果は変わらない |

### 3.3 ビルドエラー(2 個。コンパイラが検出するので、殺したものとして数えない)

機械が作る「文の削除」が、複数行にまたがる文の 1 行だけを消して、構文エラーになったもの。

| ID | 変更箇所 |
|---|---|
| G034 | L61: `mmc_scsi_read_blocks(...)` の 1 行目だけを `;` にする(続きの行が残って、構文エラー) |
| G088 | L151: `iso_extent_map(...)` の呼び出しの 2 行目を `;` にする(同上) |

### 3.4 足したテスト(26 件。`test/host/iso_usb_msc/test_iso_usb_msc_glue_mut.c`)

部品: ロックの記録(深さ、回数、`ctx`)、アロケーターの記録(生きている数、ロックの中で解放されたか)、SD のモック(呼び出しごとの記録、N 回目の失敗、ロックの中で呼ばれたか)、表の組み立てと期待するイメージの組み立て。バックエンドの関数は、`iso_usb_msc_extent_backend_as_scsi` の関数ポインターを直接呼ぶ。

| テスト | 内容 |
|---|---|
| `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone` | `scsi` / `cmd` が NULL なら `out_res` を触らない。`out_res == NULL` は可 |
| `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` | 未知の `kind`(0、3、99): `good = false`、`seq` だけ写して他は 0 |
| `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` | TUR(失敗)、REQUEST SENSE、TUR(成功)、INQUIRY(`out_cap` での切り詰めと、`out_cap` を超えて書かないこと)、1 バイトの応答、`cdb_len` が足りない場合の `good` / `data_len` / `dev_dir` |
| `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` | READ(10) の範囲(`read_lba`、`read_blocks`、`data_len == 0`、`dev_dir`)、1 ブロック、0 ブロック(`NONE`)、範囲外(失敗。READ の欄は立たない) |
| `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` | 方向が効く GET EVENT STATUS NOTIFICATION で、IN(`expected_len >= 8`)だけがイベントを消費する。OUT、NONE、未知の値、`expected_len` が 7 のときは消費しない |
| `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` | `READ_BLOCKS` の範囲(`lba`、`blocks`、`buf`、`cap`)と、`blocks` を超えて書かないこと、`cap` 不足と SD の失敗で `good = false`、他の欄は 0 |
| `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` | `lba` が 32 ビットを超えると、読まずに失敗(下位 32 ビットが 0 になる値で確認)。`UINT32_MAX` は `mmc_scsi` に渡す(センスが残る) |
| `test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table` | NULL の検査(ロックの前)、一致、別の id、id 0、表なし。失敗のとき `out` を書かない。ロックの回数 |
| `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first` | NULL の検査、`handle` を先に NULL にする、一致で参照 +1、不一致と表なしで参照が増えない |
| `test_backend_release_unrefs_a_handle_and_ignores_null_and_the_ctx` | `release` は `ctx` を使わず、`handle == NULL` は何もしない |
| `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` | リゾルバが表より先、`ctx` と id をそのまま渡す、戻り値をそのまま返す、**バックエンドのロックを放してから呼ぶ**、`lookup_size_locked == NULL` なら表を引く |
| `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` | リゾルバが返した表を `handle` に入れ、参照を足さない、NULL なら `NOT_FOUND` と `handle == NULL`、`acquire_table_locked == NULL` なら表を引く、ロックの外で呼ぶ |
| `test_set_resolver_copies_the_whole_struct_and_null_clears_it` | `set_resolver` が構造体の全欄を写す。NULL で全欄を消す。ロックの回数 |
| `test_backend_read_rejects_null_arguments_in_the_documented_order` | `be` / `handle` / `buf` の検査の順序(`blocks == 0` は `buf == NULL` でも OK)、`bdev` と `read_sectors` が NULL なら `NO_CARD`、検査はロックの前 |
| `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` | スパンごとに 1 回、表の世代、開始セクタ、数、続く書き込み先。途中のブロックから。ロックは `bdev` のポインターを取る 1 回 |
| `test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure` | ブロックデバイスのエラーコードをそのまま返し、最初の失敗で止める。世代の不一致は `ISO_ERR_EPOCH` |
| `test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request` | ファイルの終わり以降のゼロ埋め(SD の同じセクタの後ろの 0xAA を消す)。ヒープにちょうどの大きさの出力で、範囲を超える書き込みを ASan が検出する |
| `test_backend_read_rejects_a_range_beyond_the_table_with_io_error_and_reads_nothing` | 容量を超える範囲は `ISO_ERR_IO` で、何も読まない |
| `test_backend_read_halves_the_step_when_the_spans_do_not_fit_and_keeps_going_to_the_end` | 79 個の断片(> 64)を、縮めて読み切る(全セクタを順に、末尾の欠けと共に)。64 個以下は、そのまま読む |
| `test_backend_init_clears_everything_and_stores_the_arguments` | `init` が全欄を 0 にして、引数を保存する。NULL のロックでも動く |
| `test_set_blockdev_replaces_the_pointer_under_the_lock_and_ignores_a_null_backend` | ロックの中で `bdev` を入れ替える |
| `test_set_image_takes_its_own_reference_replaces_the_old_one_outside_the_lock_and_clear_resets_the_id` | 参照の増減(同じ表の再設定を含む)、NULL で id が 0、`clear`、古い表の最後の参照の解放はロックの外 |
| `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` | NULL は何もしない、ロード済みは保持、空なら表を放す(2 回目も安全) |
| `test_as_scsi_binds_the_backend_and_all_four_functions` | `ctx` と 4 つの関数、`mmc_scsi_init` が受け付ける |
| `test_epoch_matches_compares_the_table_epoch_with_the_blockdev_epoch_through_its_ctx` | 一致、世代 0 同士、±1、`bdev->ctx` を渡す、`epoch == NULL` / `bdev == NULL` / `table == NULL`(世代 0 のときも不一致) |
| `test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded` | `prevent_removal` の解除、`abort_generation` +1、UNIT ATTENTION 29h/00h、ロード済みのまま。空のメディアでは世代だけ進む |

### 3.5 殺したミュータントの一覧(303 個)

見方は 2.5 と同じ。`G` は機械、`Y` は手書き。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| G001 | L6: != -> == | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G002 | L6: negate if condition | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G003 | L7: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G004 | L12: != -> == | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G005 | L12: negate if condition | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G006 | L13: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G007 | L30: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G008 | L30: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G009 | L30: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G010 | L30: \|\| -> && | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone` |
| G011 | L30: \|\| -> && | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G012 | L30: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G013 | L33: const 0+1 -> 1 | `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G014 | L33: delete statement | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G015 | L34: delete statement | `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else`, `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| G018 | L42: delete statement | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G019 | L43: delete statement | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G020 | L44: delete statement | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G021 | L45: delete statement | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G022 | L46: delete statement | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| G023 | L48: negate if condition | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G024 | L49: > -> >= | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G025 | L49: const 0+1 -> 1 | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G026 | L49: delete statement | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G027 | L51: > -> >= | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G028 | L51: const 0+1 -> 1 | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| G029 | L51: delete statement | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| G030 | L56: > -> >= | `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| G031 | L56: negate if condition | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| G032 | L57: false -> true | `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| G035 | L62: delete statement | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G036 | L66: false -> true | `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G038 | L73: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G039 | L73: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G040 | L73: \|\| -> && | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G041 | L73: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G042 | L76: delete statement | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G043 | L78: != -> == | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G044 | L78: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G045 | L79: delete statement | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` |
| G046 | L82: == -> != | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G047 | L82: != -> == | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G048 | L82: \|\| -> && | `test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table` |
| G049 | L82: negate if condition | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G050 | L83: delete statement | `test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table` |
| G051 | L87: delete statement | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G052 | L93: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G053 | L93: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G054 | L93: \|\| -> && | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G055 | L93: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G056 | L97: delete statement | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G057 | L99: != -> == | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G058 | L99: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G059 | L100: delete statement | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G060 | L102: == -> != | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G061 | L102: negate if condition | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G062 | L108: == -> != | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G063 | L108: != -> == | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G064 | L108: \|\| -> && | `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first` |
| G065 | L108: negate if condition | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G066 | L109: delete statement | `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first` |
| G067 | L112: delete statement | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G068 | L114: delete statement | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G069 | L120: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G070 | L120: == -> != | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G071 | L120: \|\| -> && | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G072 | L120: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G073 | L123: == -> != | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G074 | L123: negate if condition | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G075 | L123: const 0+1 -> 1 | `test_backend_read_rejects_null_arguments_in_the_documented_order`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| G076 | L126: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G077 | L126: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G078 | L130: delete statement | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G079 | L132: delete statement | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G080 | L134: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G081 | L134: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G082 | L134: \|\| -> && | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G083 | L134: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G084 | L144: > -> >= | `test_iso_usb_msc_glue_mut:timeout` |
| G085 | L144: const 0+1 -> 1 | `test_backend_read_rejects_null_arguments_in_the_documented_order`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| G087 | L151: / -> * | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G089 | L152: == -> != | `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| G091 | L152: && -> \|\| | `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| G092 | L152: negate if condition | `test_iso_usb_msc_glue_mut:timeout` |
| G096 | L153: const 2-1 -> 1 | `test_iso_usb_msc_glue_mut:timeout` |
| G097 | L156: != -> == | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G098 | L156: negate if condition | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G099 | L162: * -> / | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G100 | L164: const 0+1 -> 1 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G101 | L166: < -> <= | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G102 | L166: const 0+1 -> 1 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G103 | L167: * -> / | `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations`, `test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request` |
| G104 | L168: > -> >= | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G105 | L168: negate if condition | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G106 | L168: + -> - | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G107 | L173: != -> == | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| G108 | L173: negate if condition | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| G109 | L176: += -> -= | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G110 | L176: delete statement | `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations`, `test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request` |
| G111 | L177: += -> -= | `test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request` |
| G114 | L181: negate if condition | `test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request` |
| G115 | L182: + -> - | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G116 | L182: - -> + | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G117 | L182: const 0+1 -> 1 | `test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request` |
| G118 | L182: delete statement | `test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request` |
| G119 | L185: += -> -= | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G120 | L185: delete statement | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G121 | L186: -= -> += | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G122 | L186: delete statement | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G123 | L187: += -> -= | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G124 | L187: delete statement | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G125 | L195: != -> == | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G126 | L195: negate if condition | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G127 | L196: delete statement | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| G128 | L205: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G129 | L205: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G130 | L208: const 0+1 -> 1 | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G131 | L208: delete statement | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G132 | L209: delete statement | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| G133 | L210: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G134 | L211: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G135 | L212: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G136 | L217: == -> != | `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G137 | L217: negate if condition | `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G138 | L220: delete statement | `test_backend_read_rejects_null_arguments_in_the_documented_order`, `test_set_blockdev_replaces_the_pointer_under_the_lock_and_ignores_a_null_backend` |
| G139 | L221: delete statement | `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| G140 | L222: delete statement | `test_backend_read_rejects_null_arguments_in_the_documented_order`, `test_set_blockdev_replaces_the_pointer_under_the_lock_and_ignores_a_null_backend` |
| G141 | L227: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G142 | L227: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G143 | L230: delete statement | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G144 | L231: != -> == | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G145 | L231: negate if condition | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G146 | L232: delete statement | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G147 | L234: const 0+1 -> 1 | `test_set_resolver_copies_the_whole_struct_and_null_clears_it` |
| G148 | L234: delete statement | `test_set_resolver_copies_the_whole_struct_and_null_clears_it` |
| G149 | L236: delete statement | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| G150 | L240: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G151 | L240: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G152 | L240: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G153 | L240: \|\| -> && | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G154 | L240: \|\| -> && | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G155 | L240: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G156 | L241: false -> true | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G157 | L243: == -> != | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G158 | L247: != -> == | `test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded` |
| G159 | L247: negate if condition | `test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded` |
| G160 | L248: delete statement | `test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded` |
| G161 | L255: == -> != | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G162 | L255: negate if condition | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G163 | L258: != -> == | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G164 | L258: negate if condition | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G165 | L259: delete statement | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G166 | L261: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G167 | L263: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G168 | L264: != -> == | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G169 | L264: const 0+1 -> 1 | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| G170 | L264: delete statement | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| G171 | L265: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G172 | L267: != -> == | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G173 | L267: negate if condition | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G174 | L268: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G176 | L273: delete statement | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| G177 | L278: == -> != | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| G178 | L278: == -> != | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| G179 | L278: \|\| -> && | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| G180 | L278: negate if condition | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| G181 | L281: negate if condition | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| G182 | L282: delete statement | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| Y01 | dir map: IN -> OUT | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y02 | dir map: IN -> NONE | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y03 | dir map: OUT -> IN | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y05 | dir map: default -> IN | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y06 | dir map: NONE -> IN | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y07 | execute: cdb_len - 1 | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y08 | execute: expected_len 0 | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y09 | execute: out NULL | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y10 | execute: out_cap 0 | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y11 | execute: dir always IN | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y12 | execute: dir always NONE | `test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction` |
| Y13 | execute: out_cap + 1 | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| Y14 | execute: good always true | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y15 | execute: is_read always false | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y16 | execute: read_lba 0 | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y17 | execute: read_blocks 0 | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y18 | execute: data_len 0 | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| Y19 | execute: data_len gets read blocks | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y20 | execute: read dir always IN | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y21 | execute: read dir always NONE | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y22 | execute: read dir swapped | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y23 | execute: read dir OUT | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y24 | execute: data dir always IN | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y25 | execute: data dir always NONE | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| Y26 | execute: data dir swapped | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y27 | execute: data dir OUT | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| Y28 | execute: data dir decided by the read blocks | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| Y29 | read_blocks: lba not forwarded | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| Y30 | read_blocks: lba + 1 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| Y31 | read_blocks: blocks 1 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| Y32 | read_blocks: buf NULL | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| Y33 | read_blocks: cap 0 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| Y34 | read_blocks: cap = blocks * 2048 (ignores the caller's cap) | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| Y35 | read_blocks: good always true | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| Y36 | read_blocks: lba above 32 bits reports good | `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| Y37 | read_blocks: lba above 32 bits is truncated and read | `test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba` |
| Y38 | unknown kind reports good | `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| Y39 | result: seq always 0 | `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else`, `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result` |
| Y40 | size: NULL args return NOT_FOUND | `test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table` |
| Y41 | size: missing table returns INVALID | `test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table`, `test_backend_init_clears_everything_and_stores_the_arguments` |
| Y42 | size: table-null term dropped | `test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table` |
| Y43 | size: media-id term dropped | `test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table` |
| Y44 | size: result not written | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| Y45 | size: result is the capacity in blocks | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| Y46 | size: resolver ctx NULL | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` |
| Y47 | size: resolver id 0 | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` |
| Y48 | size: resolver id from the backend | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` |
| Y49 | size: resolver result forced to OK | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` |
| Y50 | size: resolver chosen by the acquire pointer | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y51 | size: resolver called under the backend lock | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` |
| Y52 | size: resolver ignored | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock` |
| Y53 | acquire: handle not cleared first | `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y54 | acquire: NULL args return NOT_FOUND | `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first` |
| Y55 | acquire: resolver NULL table returns INVALID | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y56 | acquire: resolver table not stored in the handle | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y57 | acquire: resolver NULL table accepted | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y58 | acquire: resolver ctx NULL | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y59 | acquire: resolver id 0 | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y60 | acquire: resolver id from the backend | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y61 | acquire: resolver chosen by the size pointer | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y62 | acquire: resolver ignored | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y63 | acquire: resolver called under the backend lock | `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| Y64 | acquire: table path misses the table-null term | `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first` |
| Y65 | acquire: table path misses the media-id term | `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first` |
| Y66 | acquire: table path misses NOT_FOUND code | `test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first` |
| Y67 | acquire: table path handle not stored | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| Y68 | acquire: table path handle is the media id | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| Y69 | read: handle-null term dropped | `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y70 | read: be-null term dropped | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y71 | read: zero blocks returns INVALID | `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y72 | read: NULL buf returns NO_CARD | `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y73 | read: NULL buf accepted | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y74 | read: missing bdev returns IO | `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y75 | read: read_sectors-null term dropped | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y76 | read: bdev-null term dropped | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y77 | read: epoch 0 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y78 | read: epoch from the blockdev | `test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure` |
| Y79 | read: map from the request lba | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y80 | read: map the remaining blocks | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y81 | read: map span capacity 128 (overruns the 64-entry array) | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y82 | read: map span capacity 1 | `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations`, `test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure` |
| Y83 | read: any map error halves the step | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y84 | read: SPANS_FULL never halves | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y87 | read: map error returns NO_CARD | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y88 | read: map error ignored | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| Y91 | read: step bytes use the sector size | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y92 | read: span bytes use the block size | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y95 | read: bdev ctx NULL | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y96 | read: bdev epoch 0 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| Y97 | read: bdev start sector 0 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| Y98 | read: bdev sector count 1 | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| Y99 | read: bdev dst does not advance | `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| YA0 | read: bdev error returns IO | `test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure` |
| YA1 | read: bdev error ignored | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure` |
| YA2 | read: bdev error skips the span | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure` |
| YA3 | read: valid bytes from the request lba | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YA5 | read: zero fill starts at the buffer start | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YA6 | read: zero fill runs one byte too far | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YA7 | read: zero fill stops one byte short | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YA8 | read: zero fill uses 0xFF | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YA9 | read: zero fill from the wrong place (step start) | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YB0 | read: zero fill length is the whole step | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YB1 | read: success returns IO | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| YB2 | read: the lock is not released before the read (bdev under the lock) | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_rejects_null_arguments_in_the_documented_order` |
| YB4 | release: unref dropped | `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count`, `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good` |
| YB7 | set_image: id always kept | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YB8 | set_image: id always 0 | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| YB9 | set_image: id kept only for no table | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| YC0 | set_image: the old table is released under the lock, before the new reference | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YC1 | set_image: the old table is released after the lock but the new table's reference is taken twice | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| YC3 | clear: does nothing | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| YC4 | sync: clears when loaded | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| YC5 | sync: always clears | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| YC6 | sync: never clears | `test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded` |
| YC7 | epoch_matches: bdev ctx NULL | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YC8 | epoch_matches: compares with the blockdev's epoch + 1 | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YC9 | epoch_matches: epoch-null term dropped | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YD0 | epoch_matches: table-null term dropped | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YD1 | epoch_matches: bdev-null term dropped | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YD2 | disconnect: force_eject instead of bus_reset | `test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded` |
| YD3 | disconnect: only the prevent flag is cleared | `test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded` |
| YD4 | disconnect: ejects the media | `test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded` |
| YD5 | as_scsi: ctx NULL | `test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result`, `test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count` |
| YD6 | as_scsi: size_bytes missing | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YD7 | as_scsi: acquire missing | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YD8 | as_scsi: read missing | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YD9 | as_scsi: release missing | `test_iso_usb_msc_glue_mut:crash/rc=1` |
| YE0 | init: bdev not stored | `test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good`, `test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations` |
| YE1 | init: lock_ctx not stored | `test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone`, `test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else` |
| YE2 | set_resolver: copies only the ctx | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| YE3 | set_resolver: copies only the functions | `test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock`, `test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference` |
| YE4 | set_resolver: NULL keeps the old resolver | `test_set_resolver_copies_the_whole_struct_and_null_clears_it` |
| YE5 | set_resolver: NULL clears only the size function | `test_set_resolver_copies_the_whole_struct_and_null_clears_it` |
| YE6 | set_resolver: NULL clears only the acquire function | `test_set_resolver_copies_the_whole_struct_and_null_clears_it` |
| YE7 | set_resolver: NULL clears only the ctx | `test_set_resolver_copies_the_whole_struct_and_null_clears_it` |

## 4. 実装について気づいたこと(レビューで確認してほしい)

**バグは見つからなかった。** 次は、バグではないが、確認してほしい点。

1. **`web_iso_progress_step_pct` の大きい合計の経路に、到達しない防御がある**: `received > UINT64_MAX / 100` かつ `received < total` の枝で、`denom > 0` と `raw > 100` の防御は、常に偽(P061、P063〜P067、Q54、Q55)。実害はない。また、この経路は割り算を先にするので、`total - 99` 以上の `received`(`total` が約 1.8e17 バイト = 184 PB を超えるときだけ)で、`received < total` でも 100 を返すことがある。ISO の上限(16 GiB)の約 1000 万倍なので、実際には起きない。
2. **`web_iso_put_recv_all` の期限は、最小のスライス(10 ms)のために、最大で 10 ms(+ `SO_RCVTIMEO` の ms への切り上げの 1 ms)超えうる**(`WEB_ISO_PUT_MIN_SLICE_US` の設計どおり。`SO_RCVTIMEO` が 0 で無限待ちになることを防ぐ)。`IO_TIMEOUT` の後の時計の読み直しは、`recv_all` が自分で行い、期限なら直ちに `TIMEOUT` を返す(テストで固定した)。
3. **`iso_usb_msc_glue.c` の `be_read` が、`iso_extent_map` の失敗を `ISO_ERR_IO` にする(Y86)のは、数値が偶然一致している**: 到達できる失敗の `ISO_EXT_ERR_RANGE` は -3 で、`ISO_ERR_IO` も -3。コード上は変換しているので正しいが、`ISO_EXT_ERR_*` と `ISO_ERR_*` の数値を揃えた設計ではないので、変換を外すと、一見動いたまま、他のコードが混ざる。
4. **`be_read` の「断片の合計が `step_bytes` を超える」検査(`bytes_done`)は、`iso_extent_map` の契約が守られる限り、到達しない**(G112、Y93。3.2)。配列の外への書き込みを防ぐ防御なので、残してよい。
5. **リゾルバ(`lookup_size_locked` / `acquire_table_locked`)は、名前に反して、バックエンドのロック(`be->lock`)を放してから呼ばれる**。ヘッダーの「共有の `iso_state_mutex` の下で呼ぶ」は、呼び出し側(`iso_msc_task`)が持つ `iso_state_mutex` のことで、バックエンドのロックではない。ロックの順序(状態 → バックエンド)は、`iso_usb_msc.c` のほかの経路と一致していて問題ない。テストで「リゾルバはバックエンドのロックの外」を固定した。ヘッダーのコメントに、これを書くとよい。
6. **`iso_usb_msc_mount_image`(`iso_usb_msc.c`)は、`bdev->epoch == NULL` のとき世代の検査を飛ばす**(設計 §5.5 は「mount は表の epoch が現在の値と同じことも検査する」)。実際の SD の `bdev` は `epoch` を持つので問題ないが、グルー(`iso_usb_msc_table_epoch_matches`)は `epoch == NULL` を不一致にしているのと、扱いが食い違う(対象外のファイル)。
