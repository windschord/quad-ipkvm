# kvm_ctrl のテストとミューテーションテストの結果

作成: 2026-11-10、更新: 2026-11-10(クールダウンの起点を ATX 操作の終了に変更)(Issue #85、C-5a)。`components/kvm_ctrl`(`kvm_logic` と HAL の結線)とモック HAL(`test/host/kvm_board/`)のホスト単体テストの構成と、`kvm_ctrl.c` に対するミューテーションテストの結果。シナリオとテストの対応は `docs/KVM_LOGIC_SCENARIOS.md` の 5 章。

## 1. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/kvm_board/test_kvm_board_mock.c` | モック HAL 自身(ボード側の義務の再現、失敗の注入、違反フラグ、記録) | 25 |
| `test/host/kvm_ctrl/test_kvm_ctrl_init.c` | 初期化の手順、設定・HAL の検証、LED の変換、起動直後の状態 | 21 |
| `test/host/kvm_ctrl/test_kvm_ctrl_ops.c` | 切替・ATX の出力と順序、判定の順序、クールダウン、操作の記録、状態の報告 | 45 |
| `test/host/kvm_ctrl/test_kvm_ctrl_fault.c` | HAL の失敗・時刻の巻き戻り・`kvm_logic` の出力の不整合 -> フォールトのラッチ | 24 |
| `test/host/kvm_ctrl/test_kvm_ctrl_fuzz.c` | 不変条件を全 tick で検査するファズ(固定シード、ASan/UBSan) | 2 |
| `test/host/kvm_ctrl/kc_common.h` | テスト共通の部品(フィクスチャ、HAL の呼び出しの文字列化) | - |

合計 117 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルは 5 つ増える)。

**ファズの検査**(全 tick の後): (1) モックの違反フラグ(全線 High でない `set_addr`、同時に 2 本の `assert_line`、範囲外の引数)が立たない (2) 同時に有効な線は 1 本以下 (3) フォールト後は `assert_line` / `set_addr` / `release_line` が 0 回、`release_all` が tick ごとに 1 回、線はラッチの次の tick までに全部下がる (4) すべての `assert_line` の `max_ms` は種別の上限以下(ATX は `KVM_ATX_HARD_MAX_*`) (5) 線が有効なのは、対応する `kvm_logic` の状態(KVM: `SWITCH_PULSE`、ATX: `ATX_PULSE` かつ LED == 現在チャンネル)のときだけ (6) フォールトでなければ、ボードの A0/A1 が `kvm_logic` の出力と一致する (7) 要求の結果が、状態から決まる期待値(WEB_API.md 7 章の判定順序を独立に実装した oracle)と一致し、受理のときだけ `op_id` が 1 進み、要求は HAL を呼ばない (8) 実行中の操作は、対応する状態にいる間だけで、フォールトなら残らない。

モックの呼び出しの記録(`MOCK_LOG_MAX` = 4096 件)は、ファズが **tick ごとに空にする**(`calls[]` の累積カウンターは残す)。空にしないと、記録が(1 tick に 2 件以上、4096 件なので)2000 tick 余りで満杯になり、以降の tick で (4) の検査とパルスの集計が何も見なくなる(PR #102 の指摘。修正前は、KVM パルスの集計が 1881 回、ATX パルスが 1361 回にとどまっていた)。1 tick の中で記録が溢れたら、ファズが失敗する。

空振りでないことの確認(件数の下限)も入れている。実測(固定シードなので毎回同じ): HAL が正常な 150 シード x 20000 tick で、切替の受理 18632 件、電源操作の受理 13171 件、KVM パルス 18480 回、ATX パルス 12856 回、操作の完了 21233 件・中断 10474 件、フォールト 0 回。HAL の失敗を注入する 150 シード x 20000 tick で、フォールト 892 回(`set_addr` 168、`assert_line` 184、`release_line` 156、`read_led` 384)と、フォールトからの再起動 890 回(KVM パルス 17762 回、ATX パルス 13055 回)。

## 2. ミューテーションテスト

方法: `tools/test/mutate_kvm_ctrl.py` が、`components/` と `test/` をスクラッチにコピーし、`kvm_ctrl.c` の 1 か所を置換 -> 再ビルド -> `test_kvm_ctrl_init` / `_ops` / `_fault` / `_fuzz` を実行、を繰り返す(ミュータントの定義は `tools/test/kvm_ctrl_mutants.py`)。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。

```
python3 tools/test/mutate_kvm_ctrl.py <repo> <スクラッチ> tools/test/kvm_ctrl_mutants.py <結果.json>
```

**結果**(実行した最終の 1 回。ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|
| 190 | 178 | 12 | 0 |

経緯: (1) クールダウンの起点を「受理」から「ATX 操作の終了(完了または中断)」に変えた際に、ミュータントの old 文字列を更新し、起点に関するミュータント(M182 から M189 の 8 個)を足して全体を実行し直した(189 個、177 個を殺し、12 個が同値)。(2) その後、ファズのモックの記録を tick ごとに空にする修正を入れ、`M190` を足して、**全体(190 個)を最初から実行し直した**のが上の結果。

**ファズの修正で確認したこと(M190 と max_ms の不変条件)**: `M190`(`line_max_ms` の上限が、起動から 60 秒経った後だけ外れる)を、修正前のファズ(記録が 20000 tick の序盤で満杯になる版)で実行すると**生き残った**(単体テストの時間は短いので見逃し、ファズの (4) の検査も、記録が満杯の tick では何も見ていなかった)。修正後のファズでは**殺した**。同じ修正前のファズで、上限を外す M21、M22、M23、M25 は、序盤の tick で検出できて殺していた(= 修正前のファズは、序盤は見ていて、後半を見ていなかった)。今回の実行で、ファズのテストだけが殺したミュータントは、M134(select が channel 5 を受理する)、M153(power が channel 0 を受理する)、M154(power が channel 5 を受理する)、M190 の 4 個で、前回の実行(M190 なし)と、ファズが殺すミュータントの結果は変わらなかった(前回は M134、M153、M154 の 3 個)。

経緯(変更前の、起点が「受理」だった版): 最初の実行(181 個)では、16 個が生き残り、9 個がビルドエラー(ミュータントの書き方の問題。-Werror の未使用警告。書き直した)だった。生き残ったもののうち 5 個は同値でなかったので、殺すテストを足した(下表)。残りは同値と判断した。テストを足した後に 181 個を最初から実行し直すと、170 個を殺し、11 個が生き残った(同値)。起点の変更後の実行では、同値のミュータントが 1 個(M161)増えた。

| 変更前の最初の実行で生き残ったミュータント | 足したテスト |
|---|---|
| M29(RESET が `press_ms` を使う) | `test_each_action_uses_its_own_configured_pulse_length`(既定値では偶然、press と reset の `max_ms` がどちらも 200 になって区別できなかった) |
| M65(ATX の完了判定が idle を見ない) | `test_atx_op_is_aborted_if_logic_leaves_atx_states_without_anomaly` |
| M80(`kvm_logic` の `INVALID_PARAM` の対応) | `test_logic_invalid_param_maps_to_invalid_param` |
| M102、M103(reset / force の上限がクランプ前の値) | `test_init_validates_pulse_lengths_and_margin` に、reset / force のクランプのケースを追加 |

### 2.1 同値のミュータント(12 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| M09 | set_fault: overwrite fault_reason every time | `set_fault` は、フォールトが未ラッチのときにしか呼ばれない(ラッチ後の `tick` は先頭で戻り、`apply_outputs` の各失敗も 1 回で戻る)。`if (!c->fault)` で守っている二重のラッチは到達しないので、上書きしても最初の原因が残る |
| M10 | set_fault: asserted not reset | ラッチ後は `apply_outputs` が二度と呼ばれず、`asserted` は `kvm_ctrl_get_status` からも読まれない。再初期化(再起動の模擬)は構造体全体を 0 にする |
| M26 | line_max_ms: cap compare >= (same value) | `sum == cap` のとき、`sum = cap` の代入は同じ値で、結果が変わらない |
| M48 | apply: addr change check ignores asserted | 手順 1 の後に `asserted != NO_LINE` なら `asserted == want`(かつ `want != NO_LINE`)なので、`want != NO_LINE` の条件だけで同じ集合になる(冗長な条件) |
| M68 | update_op: has_last_op check removed | `has_last_op == false` のとき `last_op` は、`pending_line`、`set_fault`、`get_status` のどれからも無視され(状態の報告は 0 を返す)、`record_op` が全フィールドを書き直す。`update_op` が空の `last_op.result` を書き換えても観測できない |
| M81 | map: default (unreachable) -> OK | `kvm_logic_request_*` は列挙された 6 個の `kvm_result_t` しか返さない(現在の `kvm_logic.c`)ので、`default` には到達しない |
| M83 | cooldown: boundary elapsed > interval | `elapsed == interval` のとき、`>` では残りの計算に進むが、その値は `interval - elapsed = 0` で、`>=` で 0 を返すのと同じ |
| M104 | init: kvm_logic_init failure ignored | `kvm_logic_init` が失敗すると `logic.config` は 0 のまま(`config_valid` が false)で、直後の `press_ms > logic.config.max_power_pulse_ms`(= 0)の検査が、必ず `INVALID_PARAM` で拒否する。明示的な検査は、理由を分かりやすくするための冗長な防御 |
| M110 | init: set_addr uses 1 (equivalent to logic initial output) | `kvm_logic_init` は初期出力の `a0a1_channel` を 1 に固定しているので、`initial` は常に 1 |
| M118 | tick: rewind check ignores ticked (first tick; equivalent) | 初回の `tick` では `c->now_ms == 0` なので、符号なしの `now_ms < 0` は常に偽。`ticked` の条件がなくても同じ |
| M132 | tick: logic stepped with now_ms+1 | `kvm_logic` の内部時刻は、すべて引数の `now_ms` から導出した相対値(経過時間の比較だけ)なので、全時刻を 1 ms ずらしても挙動は変わらない(初期値 0 のタイマーは最初の `step` で上書きされるか、LED が無効(unknown)のままで状態遷移に影響しない)。`kvm_ctrl` 自身の時刻(クールダウン)は別に `now_ms` を使う |
| M161 | power: cooldown not disarmed on accept | 受理できるのは `cooldown_remaining() == 0`(終了から `atx_min_interval_ms` 以上)のときだけで、経過時間は単調に増えるだけなので、受理時に `armed` を落とさなくても、実行中の `cooldown_ms` は 0 のまま。落とすのは、意図を明示するための冗長な処理 |

### 2.2 殺したミュータントの一覧(178 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(ほかにも落ちるものがあれば省略)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| M01 | default: atx_min_interval_ms = 0 (cooldown off) | `test_config_default_is_valid_and_within_limits`, `test_order_unknown_beats_mismatch_and_cooldown` |
| M02 | default: assert_margin_ms = 0 | `test_init_releases_all_then_sets_addr_1_even_when_board_already_at_1`, `test_init_forces_addr_1_when_board_was_elsewhere` |
| M03 | led_mask_to_raw: do not ignore bits 4-7 | `test_led_mask_to_raw`, `test_led_mask_upper_bits_are_ignored_by_tick` |
| M04 | led_mask_to_raw: 0x04 -> 4 | `test_led_mask_to_raw`, `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high` |
| M05 | led_mask_to_raw: 0x08 -> 3 | `test_led_mask_to_raw`, `test_select_completes_when_idle_matches_target_after_tracking` |
| M06 | led_mask_to_raw: multi-lit -> 0 instead of 0xFF | `test_led_mask_to_raw` |
| M07 | led_mask_to_raw: 0x00 -> 0xFF | `test_led_mask_to_raw` |
| M08 | led_mask_to_raw: 0x02 -> 1 | `test_led_mask_to_raw`, `test_chattering_led_never_settles` |
| M11 | set_fault: abort only completed ops (== -> !=) | `test_fault_set_addr_during_select`, `test_fault_assert_line_kvm` |
| M12 | set_fault: pending op becomes COMPLETED | `test_fault_set_addr_during_select`, `test_fault_assert_line_kvm` |
| M13 | fault_and_release: no emergency release_all | `test_init_set_addr_failure_latches_fault_and_releases_all`, `test_fault_set_addr_during_select` |
| M14 | fault_and_release: do not latch | `test_init_set_addr_failure_latches_fault_and_releases_all`, `test_fault_set_addr_during_select` |
| M15 | pending_line: SELECT -> POWER | `test_select_full_sequence_and_trace`, `test_select_busy_while_switching_or_tracking` |
| M16 | pending_line: PRESS -> POWER_FORCE | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M17 | pending_line: FORCE -> POWER | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M18 | pending_line: RESET -> POWER | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M19 | pending_line: ignore result != PENDING (finished op justifies a line) | `test_finished_op_does_not_justify_a_line` |
| M20 | pending_line: ignore has_last_op | `test_line_is_enabled_only_for_matching_pending_op` |
| M21 | line_max_ms: no hard cap for POWER | `test_assert_max_ms_is_capped_at_hard_max`, `test_fuzz_invariants_without_hal_failures` |
| M22 | line_max_ms: no hard cap for FORCE | `test_assert_max_ms_is_capped_at_hard_max`, `test_fuzz_invariants_without_hal_failures` |
| M23 | line_max_ms: no hard cap for RESET | `test_assert_max_ms_is_capped_at_hard_max`, `test_fuzz_invariants_without_hal_failures` |
| M24 | line_max_ms: margin not added | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M25 | line_max_ms: sum not capped at all | `test_assert_max_ms_is_capped_at_hard_max`, `test_assert_max_ms_does_not_overflow_with_huge_margin` |
| M27 | line_max_ms: KVM pulse uses press_ms | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M28 | line_max_ms: FORCE uses press_ms | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M29 | line_max_ms: RESET uses press_ms | `test_each_action_uses_its_own_configured_pulse_length` |
| M30 | line_max_ms: sum as uint32 (wraps) | `test_assert_max_ms_does_not_overflow_with_huge_margin` |
| M31 | line_max_ms: default cap is not UINT32_MAX | `test_assert_max_ms_does_not_overflow_with_huge_margin` |
| M32 | apply: multiple-E check threshold > 2 | `test_fault_when_logic_enables_two_lines_including_the_matching_one` |
| M33 | apply: multiple-E check > 0 | `test_select_full_sequence_and_trace`, `test_select_busy_while_switching_or_tracking` |
| M34 | apply: a0a1 lower bound removed (0 accepted) | `test_fault_when_logic_a0a1_out_of_range` |
| M35 | apply: a0a1 > 4 -> > 5 (5 accepted) | `test_fault_when_logic_a0a1_out_of_range` |
| M36 | apply: KVM line without pending select allowed | `test_line_is_enabled_only_for_matching_pending_op` |
| M37 | apply: RESET line without pending reset allowed | `test_line_is_enabled_only_for_matching_pending_op` |
| M38 | apply: POWER line without pending press/force allowed | `test_line_is_enabled_only_for_matching_pending_op`, `test_finished_op_does_not_justify_a_line` |
| M39 | apply: POWER allowed only for press (force faults) | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M40 | apply: e_power_low always maps to POWER (not force) | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M41 | apply: skip inconsistency fault | `test_line_is_enabled_only_for_matching_pending_op`, `test_finished_op_does_not_justify_a_line` |
| M42 | apply: release even the wanted line each tick | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M43 | apply: asserted not cleared after release_line | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M44 | apply: release_line failure check <= 0 | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M45 | apply: release_line failure ignored | `test_fault_release_line_at_pulse_end`, `test_fault_release_line_at_anomaly` |
| M46 | apply: addr compare == instead of != | `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high`, `test_dark_led_stays_unknown_and_outputs_nothing` |
| M47 | apply: addr change while asserted/with new line -> only &&  | `test_fault_when_addr_changes_in_the_same_tick_as_a_new_line` |
| M49 | apply: addr change check ignores want | `test_fault_when_addr_changes_in_the_same_tick_as_a_new_line` |
| M50 | apply: set_addr failure ignored | `test_fault_set_addr_during_select`, `test_fault_set_addr_during_manual_tracking` |
| M51 | apply: board_addr not updated after set_addr | `test_select_full_sequence_and_trace`, `test_select_already_selected_does_nothing` |
| M52 | apply: assert even if already asserted | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M53 | apply: assert_line failure ignored | `test_fault_assert_line_kvm`, `test_fault_assert_line_atx` |
| M54 | apply: asserted not recorded | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M55 | apply: assert_line max_ms = 0 | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M56 | apply: set_addr(channel) uses wrong value (board_addr) | `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high`, `test_select_full_sequence_and_trace` |
| M57 | apply: reset checked before kvm (KVM vs RESET priority irrelevant) -> kvm line maps to RESET | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M58 | apply: e_reset_low maps to KVM line | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M59 | update_op: select resolved when not idle (== -> !=) | `test_select_full_sequence_and_trace`, `test_select_busy_while_switching_or_tracking` |
| M60 | update_op: select completed iff active != channel | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M61 | update_op: select always completed | `test_select_timeout_reverts_a0a1_and_aborts`, `test_select_overtaken_by_manual_switch_is_aborted` |
| M62 | update_op: ATX resolved during pulse (drop ATX_PULSE) | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M63 | update_op: ATX resolved during setup (drop ATX_WAIT_SETUP) | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M64 | update_op: ATX completion ignores anomaly flag | `test_power_with_anomaly_flag_at_the_end_is_not_completed` |
| M65 | update_op: ATX completion ignores idle (non-idle also completed) | `test_atx_op_is_aborted_if_logic_leaves_atx_states_without_anomaly` |
| M66 | update_op: ATX results swapped | `test_power_actions_use_right_line_max_ms_and_order`, `test_power_anomaly_during_pulse_releases_immediately` |
| M67 | update_op: do not skip decided ops (result != PENDING check removed) | `test_anomaly_flag_clears_when_next_pulse_starts`, `test_cooldown_boundary_is_exact` |
| M69 | record_op: pre-increment op_id | `test_select_full_sequence_and_trace`, `test_select_accepts_null_op_id` |
| M70 | record_op: op_id not advanced | `test_cooldown_boundary_is_exact`, `test_op_ids_increment_and_only_last_op_is_kept` |
| M71 | record_op: initial result COMPLETED | `test_select_full_sequence_and_trace`, `test_select_busy_while_switching_or_tracking` |
| M72 | record_op: has_last_op not set | `test_select_full_sequence_and_trace`, `test_select_accepts_null_op_id` |
| M73 | record_op: channel not stored | `test_select_full_sequence_and_trace`, `test_select_completes_when_idle_matches_target_after_tracking` |
| M74 | record_op: kind not stored | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M75 | record_op: op_id output not written | `test_select_full_sequence_and_trace`, `test_select_already_selected_does_nothing` |
| M76 | map: LOCKED -> BUSY | `test_order_locked_beats_cooldown` |
| M77 | map: SWITCHING -> ATX_LOCKED | `test_select_busy_while_switching_or_tracking`, `test_order_busy_beats_mismatch_and_cooldown` |
| M78 | map: MISMATCH -> BUSY | `test_power_channel_mismatch_outputs_nothing_and_does_not_consume_cooldown`, `test_order_mismatch_beats_locked_and_cooldown` |
| M79 | map: UNKNOWN_STATE -> BUSY | `test_power_is_rejected_in_unknown_state`, `test_order_unknown_beats_mismatch_and_cooldown` |
| M80 | map: INVALID_PARAM -> BUSY | `test_logic_invalid_param_maps_to_invalid_param` |
| M82 | cooldown: interval==0 check inverted | `test_order_unknown_beats_mismatch_and_cooldown`, `test_order_busy_beats_mismatch_and_cooldown` |
| M84 | cooldown: boundary elapsed >= interval - 1 | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M85 | cooldown: remaining returns elapsed | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M86 | cooldown: ignores armed flag | `test_status_right_after_init`, `test_power_actions_use_right_line_max_ms_and_order` |
| M87 | cooldown: remaining off by one | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M88 | init: set_addr pointer not required | `test_kvm_ctrl_init:crash/rc=1` |
| M89 | init: assert_line pointer not required | `test_init_rejects_incomplete_hal_without_touching_hardware` |
| M90 | init: release_line pointer not required | `test_init_rejects_incomplete_hal_without_touching_hardware` |
| M91 | init: release_all pointer not required | `test_kvm_ctrl_init:crash/rc=1` |
| M92 | init: read_led_mask pointer not required | `test_init_rejects_incomplete_hal_without_touching_hardware` |
| M93 | init: video_sync pointer not required | `test_init_rejects_incomplete_hal_without_touching_hardware` |
| M94 | init: press_ms==0 allowed | `test_init_validates_pulse_lengths_and_margin` |
| M95 | init: press_ms bound uses unclamped logic max | `test_init_validates_pulse_lengths_and_margin` |
| M96 | init: press_ms bound off by one (>=) | `test_init_validates_pulse_lengths_and_margin`, `test_assert_max_ms_is_capped_at_hard_max` |
| M97 | init: reset_ms==0 allowed | `test_init_validates_pulse_lengths_and_margin` |
| M98 | init: reset_ms bound off by one (>=) | `test_init_validates_pulse_lengths_and_margin`, `test_assert_max_ms_is_capped_at_hard_max` |
| M99 | init: force_ms==0 allowed | `test_init_validates_pulse_lengths_and_margin` |
| M100 | init: force_ms bound off by one (>=) | `test_init_validates_pulse_lengths_and_margin`, `test_assert_max_ms_is_capped_at_hard_max` |
| M101 | init: margin==0 allowed | `test_init_validates_pulse_lengths_and_margin` |
| M102 | init: reset_ms bound uses unclamped logic max | `test_init_validates_pulse_lengths_and_margin` |
| M103 | init: force_ms bound uses unclamped logic max | `test_init_validates_pulse_lengths_and_margin` |
| M105 | init: release_all failure ignored | `test_init_release_all_failure_latches_fault_and_skips_set_addr` |
| M106 | init: release_all failure -> emergency release too | `test_init_release_all_failure_latches_fault_and_skips_set_addr` |
| M107 | init: set_addr(initial) skipped | `test_init_releases_all_then_sets_addr_1_even_when_board_already_at_1`, `test_init_forces_addr_1_when_board_was_elsewhere` |
| M108 | init: set_addr failure -> no emergency release | `test_init_set_addr_failure_latches_fault_and_releases_all` |
| M109 | init: board_addr not recorded | `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high`, `test_dark_led_stays_unknown_and_outputs_nothing` |
| M111 | init: next_op_id starts at 0 | `test_select_full_sequence_and_trace`, `test_select_accepts_null_op_id` |
| M112 | init: set_addr failure not latched (returns FAULT only) | `test_init_set_addr_failure_latches_fault_and_releases_all` |
| M113 | init: initialized flag not set | `test_init_releases_all_then_sets_addr_1_even_when_board_already_at_1`, `test_init_release_all_failure_latches_fault_and_skips_set_addr` |
| M114 | tick: latched tick does not retry release_all | `test_init_release_all_failure_latches_fault_and_skips_set_addr`, `test_fault_set_addr_during_select` |
| M115 | tick: latched tick returns OK | `test_init_release_all_failure_latches_fault_and_skips_set_addr`, `test_init_set_addr_failure_latches_fault_and_releases_all` |
| M116 | tick: latch check removed (kvm_logic output applied after fault) | `test_init_release_all_failure_latches_fault_and_skips_set_addr`, `test_init_set_addr_failure_latches_fault_and_releases_all` |
| M117 | tick: rewind check uses <= | `test_time_rewind_latches_fault`, `test_time_does_not_rewind_with_equal_or_large_jumps` |
| M119 | tick: rewind check removed | `test_time_rewind_latches_fault`, `test_time_does_not_rewind_with_equal_or_large_jumps` |
| M120 | tick: rewind -> no emergency release_all | `test_time_rewind_latches_fault` |
| M121 | tick: now_ms not stored | `test_anomaly_flag_clears_when_next_pulse_starts`, `test_cooldown_boundary_is_exact` |
| M122 | tick: ticked not stored | `test_time_rewind_latches_fault`, `test_time_does_not_rewind_with_equal_or_large_jumps` |
| M123 | tick: read_led failure ignored | `test_fault_read_led_while_idle`, `test_fault_read_led_while_line_is_asserted` |
| M124 | tick: read_led failure -> no emergency release_all | `test_fault_read_led_while_idle`, `test_fault_read_led_while_line_is_asserted` |
| M125 | tick: video_sync not sanitised | `test_video_sync_is_read_every_tick_and_reported` |
| M126 | tick: last_sync not stored | `test_video_sync_is_read_every_tick_and_reported`, `test_status_reports_lock_anomaly_cooldown_and_sync` |
| M127 | tick: video_sync not passed to kvm_logic | `test_select_overtaken_by_manual_switch_is_aborted`, `test_select_completes_when_idle_matches_target_after_tracking` |
| M128 | tick: led mask not converted (raw = mask) | `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high`, `test_multi_lit_led_is_unknown` |
| M129 | tick: apply failure ignored (returns OK) | `test_fault_set_addr_during_select`, `test_fault_set_addr_during_manual_tracking` |
| M130 | tick: update_op before apply | `test_fault_release_line_at_pulse_end`, `test_line_is_enabled_only_for_matching_pending_op` |
| M131 | tick: update_op skipped | `test_select_full_sequence_and_trace`, `test_select_timeout_reverts_a0a1_and_aborts` |
| M133 | select: channel 0 accepted | `test_order_invalid_params_beat_everything`, `test_fuzz_invariants_without_hal_failures` |
| M134 | select: channel 5 accepted | `test_fuzz_invariants_without_hal_failures`, `test_fuzz_invariants_with_hal_failures` |
| M135 | select: fault check removed | `test_fault_set_addr_during_select`, `test_fault_set_addr_during_manual_tracking` |
| M136 | select: unknown pre-check removed | `test_select_unknown_state_is_distinguished`, `test_fuzz_invariants_without_hal_failures` |
| M137 | select: already_selected compare inverted | `test_select_full_sequence_and_trace`, `test_select_rejects_invalid_channels` |
| M138 | select: already_selected ignores idle | `test_select_busy_while_switching_or_tracking`, `test_fuzz_invariants_without_hal_failures` |
| M139 | select: already_selected removed | `test_select_already_selected_does_nothing`, `test_op_ids_increment_and_only_last_op_is_kept` |
| M140 | select: kvm_logic failure ignored | `test_select_busy_while_switching_or_tracking`, `test_led_chatter_during_idle_never_outputs_and_blocks_requests` |
| M141 | select: op recorded with POWER_PRESS kind | `test_select_full_sequence_and_trace`, `test_select_busy_while_switching_or_tracking` |
| M142 | select: op_id not cleared on entry | `test_init_rejects_incomplete_hal_without_touching_hardware`, `test_select_rejects_invalid_channels` |
| M143 | select: NOT_READY check removed | `test_init_rejects_incomplete_hal_without_touching_hardware` |
| M144 | power: PRESS maps to RESET target | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M145 | power: RESET maps to POWER target | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M146 | power: FORCE maps to POWER target | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M147 | power: PRESS records RESET kind | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M148 | power: FORCE records PRESS kind | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M149 | power: PRESS uses reset_ms | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M150 | power: RESET uses press_ms | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M151 | power: FORCE uses press_ms | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M152 | power: fault check removed | `test_fault_set_addr_during_select`, `test_fault_set_addr_during_manual_tracking` |
| M153 | power: channel 0 accepted (range check removed lower bound) | `test_fuzz_invariants_with_hal_failures` |
| M154 | power: channel 5 accepted | `test_fuzz_invariants_with_hal_failures` |
| M155 | power: kvm_logic failure ignored | `test_select_busy_while_switching_or_tracking`, `test_power_channel_mismatch_outputs_nothing_and_does_not_consume_cooldown` |
| M156 | power: cooldown always rejects | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M157 | power: cooldown check removed | `test_order_locked_beats_cooldown`, `test_order_mismatch_beats_cooldown_without_tick_in_between` |
| M158 | power: cooldown checked before kvm_logic (order) | `test_order_unknown_beats_mismatch_and_cooldown`, `test_order_busy_beats_mismatch_and_cooldown` |
| M159 | power: accepted trial not committed | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M160 | power: request applied to real logic before cooldown check | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M162 | update_op: end time not recorded | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M163 | update_op: cooldown starts at time 0 | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M164 | power: cooldown starts at acceptance (old behaviour) | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M165 | power: op recorded with SELECT kind | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M166 | power: op_id not cleared on entry | `test_power_channel_mismatch_outputs_nothing_and_does_not_consume_cooldown`, `test_power_rejects_invalid_parameters` |
| M182 | update_op: cooldown never armed | `test_order_unknown_beats_mismatch_and_cooldown`, `test_order_busy_beats_mismatch_and_cooldown` |
| M183 | update_op: only completed ops start the cooldown | `test_cooldown_counts_from_an_aborted_op` |
| M184 | update_op: select ops start the cooldown too | `test_cooldown_does_not_count_while_a_select_runs_or_after_it`, `test_cooldown_does_not_apply_to_select_and_select_does_not_start_it` |
| M185 | update_op: cooldown counted while the pulse runs | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M186 | cooldown: armed check inverted | `test_status_right_after_init`, `test_power_actions_use_right_line_max_ms_and_order` |
| M187 | update_op: end time off by one (now - 1) | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M188 | update_op: end time off by one (now + 1) | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M189 | update_op: cooldown armed before the op result is decided (inside pending branch) | `test_cooldown_boundary_is_exact`, `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| M190 | line_max_ms: hard cap dropped after 60 s of uptime | `test_fuzz_invariants_without_hal_failures`, `test_fuzz_invariants_with_hal_failures` |
| M167 | map_state: TRACKING -> SWITCHING | `test_select_busy_while_switching_or_tracking`, `test_select_overtaken_by_manual_switch_is_aborted` |
| M168 | map_state: ATX -> IDLE | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length` |
| M169 | map_state: SWITCH_REVERT_SETUP -> TRACKING | `test_select_full_sequence_and_trace`, `test_status_state_mapping_covers_every_logic_state` |
| M170 | map_state: UNKNOWN -> IDLE | `test_status_right_after_init`, `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high` |
| M171 | status: active_channel not zeroed when unknown | `test_status_state_mapping_covers_every_logic_state` |
| M172 | status: active_channel zeroed when idle too | `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high`, `test_boot_with_led_ch1_does_not_set_addr_again` |
| M173 | status: atx_locked inverted | `test_status_right_after_init`, `test_boot_with_led_ch1_does_not_set_addr_again` |
| M174 | status: atx_anomaly always false | `test_power_anomaly_during_pulse_releases_immediately`, `test_power_anomaly_before_assert_never_asserts` |
| M175 | status: video_sync always UNKNOWN | `test_video_sync_is_read_every_tick_and_reported`, `test_status_reports_lock_anomaly_cooldown_and_sync` |
| M176 | status: fault_reason not reported | `test_init_release_all_failure_latches_fault_and_skips_set_addr`, `test_init_set_addr_failure_latches_fault_and_releases_all` |
| M177 | status: fault not reported | `test_init_release_all_failure_latches_fault_and_skips_set_addr`, `test_init_set_addr_failure_latches_fault_and_releases_all` |
| M178 | status: cooldown_ms not reported | `test_order_unknown_beats_mismatch_and_cooldown`, `test_order_busy_beats_mismatch_and_cooldown` |
| M179 | status: has_last_op always true | `test_status_right_after_init`, `test_select_rejects_invalid_channels` |
| M180 | status: last_op not zeroed when absent | `test_status_right_after_init` |
| M181 | status: NULL status accepted | `test_kvm_ctrl_init:crash/rc=1` |
