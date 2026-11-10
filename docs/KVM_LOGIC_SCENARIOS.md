# KVM Logic Scenarios & Tests

本ドキュメントは、`kvm_logic` 状態機械の期待動作シナリオと、それぞれに対応するユニットテスト名を定義します。

## 1. 正常系・状態遷移シナリオ (test_switch.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 正常切替 (Web/パネル) | IDLE -> (ATXロック) -> A0/A1変更 -> KVMボタンパルス -> LED変化待ち -> 映像同期(LOCKED)待ち -> ATXロック解除。 | `test_switch_normal_success` |
| 現在チャンネルへの切替要求 | 即座に成功扱い（または無視）とし、ATXロック等は行わない。 | `test_switch_to_current_channel_is_ignored` |
| 切替タイムアウト | LED変化待ちでタイムアウトした場合、失敗として実際のLED値にA0/A1を戻し、ATXロック解除。A0/A1戻し時も不変条件4（全Ē High）を守る。 | `test_switch_led_timeout_reverts_a0a1` |
| 映像再同期タイムアウト / 無信号 | 切替後、LEDは変化したが映像同期がタイムアウト、または NO_SIGNAL の場合でも、ATXロックは解除する。ただし、NO_SIGNAL の settle 時間は映像待ちに入った時刻、またはNO_SIGNALの開始時刻の遅いほうから起算する。 | `test_switch_video_sync_timeout_releases_atx_lock`, `test_switch_video_no_signal_releases_atx_lock` |
| 手動切替の追従 | 外部要因でLEDが変化した場合、即座にATXロックし、LED安定後にA0/A1を追従。映像同期を待ってからATX解除。 | `test_manual_switch_tracking` |
| 起動直後のLED確定待ち | 初期状態 (UNKNOWN) から、LEDが安定するまで待ち、確定後にA0/A1を設定してIDLEへ移行。 | `test_startup_led_stabilization` |

## 2. ATX 制御シナリオ (test_atx.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 正常 ATX パルス | チャンネルが一致・IDLE状態でパルス開始。指定時間後にパルス終了（全Ē High）。 | `test_atx_normal_pulse` |
| チャンネル不一致による拒否 | 要求チャンネルが現在LEDと異なる場合は拒否（エラーコード返却）。 | `test_atx_rejected_on_channel_mismatch` |
| ロック中・切替中による拒否 | 切替中や映像再同期待ちなどATXロック中は拒否。 | `test_atx_rejected_when_locked_or_switching` |
| UNKNOWN 状態での拒否 | LEDが不定 (UNKNOWN) の間はATX要求を拒否。 | `test_atx_rejected_when_unknown` |
| 無効なターゲットでの拒否 | KVM_ATX_TARGET_NONEなど無効なターゲットは拒否。 | `test_atx_invalid_targets` |
| パルス長上限による clamp | 要求されたパルス長が設定(config)の最大値を超える場合、最大値に切り詰める（clampする）。※board側でも別途clampする前提。 | `test_atx_pulse_length_clamped` |
| パルス中のLED変化（異常） | パルス中にLEDが不一致になった場合、直ちに全ĒをHighにしパルス中断。異常を記録。 | `test_atx_aborted_and_anomaly_recorded_if_led_changes_during_pulse` |
| パルス終了時のLED不一致のLED異常 | パルス開始前・終了後にLEDが一致しない場合、要求拒否・異常記録。 | `test_atx_anomaly_on_pulse_start_or_end` |

## 3. 競合・異常シナリオ (test_unknown.c / test_switch.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| LED異常状態 (UNKNOWN) | LED全消灯、複数点灯、チャタリング時は UNKNOWN へ移行。ATXロック、全Ē High、A0/A1保持。 | `test_unknown_state_on_invalid_led` |
| 無効なconfigによる初期化 | configに0が含まれるなど不正な場合は `ERR_INVALID_PARAM` で拒否され、`kvm_logic_step` は何もせず `config_valid == false` として振る舞う。 | `test_invalid_config_prevents_step_and_requests` |
| 切替中の手動切替 (LED変化) | 切替中に予期せぬLED変化（別チャンネルへの移行など）があった場合、手動切替追従またはエラーとして安全に処理。 | `test_switch_interrupted_by_unexpected_led_change` |
| タイムアウト後にLEDが遅れて変化 | 切替タイムアウトし復旧処理に入った後・完了後に目的のLEDへ変化した場合、手動切替の追従として扱う。 | `test_manual_switch_tracked_after_switch_timeout` |
| 切替要求の重複 | 切替中に別の切替要求が来た場合は拒否（または後勝ち処理、今回は拒否とする）。 | `test_duplicate_switch_requests_rejected` |

## 4. 不変条件とその他の要件 (test_invariants.c / test_fuzz_invariants.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 【不変条件1】 A0/A1の一致 | 切替中(A0/A1変更からLED確定まで)を除き、A0/A1はLEDが示す現在チャンネルと一致していること。 | `test_invariant_1_a0a1_matches_led`, `test_fuzz_invariants` |
| 【不変条件2】 切替中のATX保護 | 切替中(ボタン押下〜LED確定+HDMI再同期)は、ATXの Ē が決して Low にならないこと。 | `test_invariant_2_no_atx_during_switch`, `test_fuzz_invariants` |
| 【不変条件3】 ATX LED監視 | ATXパルス前後・中のLED監視と上限。 | `test_fuzz_invariants` (test_atx.c で網羅) |
| 【不変条件4】 A0/A1変更手順 | A0/A1の変更時は、必ず「全Ē High -> A0/A1変更 -> セットアップ時間 -> Ē Low」の順序を守ること。いかなる時も Ē が Low の状態で A0/A1 を変更してはならない。 | `test_invariant_4_a0a1_change_sequence`, `test_fuzz_invariants` |
| タイムスタンプ・ラップアラウンド安全確認 | 外部注入される `now_ms` のラップアラウンド耐性。uint32ではなくuint64を想定するが、ロジック内のタイマー差分計算が安全か。 | `test_time_wraparound_safety` |

## 5. `kvm_ctrl`（`kvm_logic` と HAL の結線）のシナリオ

`components/kvm_ctrl` は、`kvm_logic` の出力を `kvm_board_hal_t` に安全な順序で適用する（契約: `docs/INTERFACES.md` 6.3）。テストは `test/host/kvm_ctrl/`、モック HAL は `test/host/kvm_board/`。ミューテーションテストの結果は `docs/KVM_CTRL_TESTS.md`。
表のテスト名は、特に書かない限り `test_kvm_ctrl_ops.c`（ops）、`test_kvm_ctrl_init.c`（init）、`test_kvm_ctrl_fault.c`（fault）にある。

### 5.1 初期化・起動直後

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 初期化の手順 | `release_all` -> `set_addr(1)`。ボードが最初から 1 でも `set_addr` を省かない。ほかの HAL は呼ばない | `test_init_releases_all_then_sets_addr_1_even_when_board_already_at_1`, `test_init_forces_addr_1_when_board_was_elsewhere`, `test_init_makes_no_other_hal_calls` (init) |
| 初期化の失敗 | `release_all` の失敗は `set_addr` を呼ばずにフォールトで始まる。`set_addr` の失敗は `release_all` してフォールトで始まる | `test_init_release_all_failure_latches_fault_and_skips_set_addr`, `test_init_set_addr_failure_latches_fault_and_releases_all` (init) |
| HAL・引数・設定の検証 | NULL、不完全な HAL（6 つの関数ポインタのどれが NULL でも）、パルス長・余裕・上限の境界は拒否し、HAL を一切呼ばない。拒否した `ctrl` はすべての API が `NOT_READY` | `test_init_rejects_null_arguments`, `test_init_rejects_incomplete_hal_without_touching_hardware`, `test_init_validates_pulse_lengths_and_margin`, `test_init_validates_embedded_logic_config`, `test_config_default_is_valid_and_within_limits` (init) |
| LED の変換 | ちょうど 1 ビットなら 1..4、無点灯 0、複数点灯 0xFF、ビット 4 以上は無視 | `test_led_mask_to_raw`, `test_led_mask_upper_bits_are_ignored_by_tick` (init) |
| 起動直後の確定 | LED が安定するまで unknown で出力を動かさない。確定したら全線 High で `set_addr` | `test_boot_with_led_ch3_sets_addr_3_with_all_lines_high`, `test_boot_with_led_ch1_does_not_set_addr_again`, `test_status_right_after_init` (init) |
| 無点灯・複数点灯・チャタリング | unknown / 追従のまま。出力を一切動かさない | `test_dark_led_stays_unknown_and_outputs_nothing`, `test_multi_lit_led_is_unknown`, `test_chattering_led_never_settles` (init), `test_led_chatter_during_idle_never_outputs_and_blocks_requests` (ops) |
| 映像の同期状態の結線 | 毎 tick 読む。範囲外は UNKNOWN。切替の完了時間に反映される | `test_video_sync_is_read_every_tick_and_reported` (init), `test_video_sync_is_wired_into_switch_completion` (ops) |

### 5.2 切替（select）

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 成功 | 要求は HAL を呼ばない。tick で `set_addr`（全線 High）-> `assert_line(KVM, パルス長 + 余裕)` -> `release_line`。LED と映像の確認後に completed | `test_select_full_sequence_and_trace` |
| すでにそのチャンネルで idle | `ALREADY_SELECTED`。何も押さず、`op_id` は進まない | `test_select_already_selected_does_nothing` |
| unknown | `UNKNOWN_STATE`（現在のチャンネルへの要求でも `ALREADY_SELECTED` にしない） | `test_select_unknown_state_is_distinguished` |
| 切替中・追従中 | `BUSY`（現在のチャンネルへの要求も、idle でなければ `BUSY`） | `test_select_busy_while_switching_or_tracking` |
| 引数 | チャンネル 0、5 以上は `INVALID_PARAM`（`op_id` は 0）。`op_id` は NULL でもよい | `test_select_rejects_invalid_channels`, `test_select_accepts_null_op_id` |
| LED が変わらない | タイムアウトで A0/A1 を戻し（全線 High）、aborted | `test_select_timeout_reverts_a0a1_and_aborts` |
| 手動切替に追い越される | 追従中は pending、idle に戻ったとき active != 目標なら aborted、== なら completed | `test_select_overtaken_by_manual_switch_is_aborted`, `test_select_completes_when_idle_matches_target_after_tracking` |
| 手動切替への追従 | 線は動かさず、LED が安定してから `set_addr` だけ。受理した操作ではない（`last_op` は変わらない） | `test_manual_switch_is_tracked_with_no_output_lines` |

### 5.3 電源操作（ATX）

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| press / reset / force | それぞれ `POWER` / `RESET` / `POWER_FORCE` を、準備（setup）の後に `assert_line`。`max_ms` = パルス長 + 余裕。パルス長ちょうどで `release_line`。`A0/A1` は変えない | `test_power_actions_use_right_line_max_ms_and_order`, `test_each_action_uses_its_own_configured_pulse_length`, `test_power_does_not_pulse_longer_than_configured` |
| `max_ms` の上限 | ATX は `KVM_ATX_HARD_MAX_*` を超えない。余裕が大きくても桁あふれしない | `test_assert_max_ms_is_capped_at_hard_max`, `test_assert_max_ms_does_not_overflow_with_huge_margin`, `test_assert_max_ms_for_kvm_is_pulse_plus_margin` |
| パルス中の LED の変化 | 検出した tick の中で直ちに `release_line`。anomaly、aborted。LED が戻っても再開しない | `test_power_anomaly_during_pulse_releases_immediately` |
| 準備中の LED の変化 | 一度も `assert_line` しない | `test_power_anomaly_before_assert_never_asserts` |
| anomaly の寿命 | 次の ATX パルスの開始で消える | `test_anomaly_flag_clears_when_next_pulse_starts` |
| `channel` の不一致 | 何も出さず、クールダウンも消費しない | `test_power_channel_mismatch_outputs_nothing_and_does_not_consume_cooldown` |
| 引数・unknown | `INVALID_PARAM`（channel、action）、`UNKNOWN_STATE` | `test_power_rejects_invalid_parameters`, `test_power_is_rejected_in_unknown_state` |
| anomaly なしで終わらなかった | 状態が ATX を出た（anomaly なし）、またはフラグが立ったまま終わったら completed にしない | `test_atx_op_is_aborted_if_logic_leaves_atx_states_without_anomaly`, `test_power_with_anomaly_flag_at_the_end_is_not_completed` |

### 5.4 判定の順序（WEB_API.md 7.3）

引数 -> フォールト -> unknown -> busy -> channel_mismatch -> atx_locked -> cooldown -> 受理。複数の条件が重なるケースごとにテストする。

| 重なる条件 | 期待 | テスト名 |
| --- | --- | --- |
| unknown + mismatch + cooldown | `UNKNOWN_STATE` | `test_order_unknown_beats_mismatch_and_cooldown` |
| busy + mismatch + cooldown | `BUSY` | `test_order_busy_beats_mismatch_and_cooldown` |
| mismatch + locked + cooldown | `CHANNEL_MISMATCH` | `test_order_mismatch_beats_locked_and_cooldown`, `test_order_mismatch_beats_cooldown_without_tick_in_between` |
| locked + cooldown | `ATX_LOCKED`、ロックが解ければ `COOLDOWN` | `test_order_locked_beats_cooldown` |
| 引数が不正 + 上記のすべて | `INVALID_PARAM` | `test_order_invalid_params_beat_everything` |
| フォールト + 他のすべて | `FAULT`（引数が正しいとき） | `expect_latched_and_quiet`（fault の全テスト） |
| ファズ | 乱数の状態で、独立に書いた期待値と一致 | `test_fuzz_invariants_without_hal_failures`, `test_fuzz_invariants_with_hal_failures` (fuzz) |

### 5.5 クールダウン

起点は「ATX 操作が終わった（完了または中断した）tick」。実行中は数えない（busy で拒否され、`cooldown_ms` は 0）。

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 境界 | 終了から `atx_min_interval_ms - 1` 後は `COOLDOWN`（残り 1 ms）、ちょうど `atx_min_interval_ms` 後は受理。実行中は `cooldown_ms` が 0 で、終了の tick から数え直す | `test_cooldown_boundary_is_exact` |
| 長いパルス（force）の直後 | 受理からは 2000 ms を過ぎていても、終了から数える。パルス中は `cooldown_ms` が 0 で `BUSY` | `test_cooldown_counts_from_the_end_of_a_force_pulse` |
| 中断（anomaly）の後 | 準備中・パルス中のどちらで中断しても、中断した tick から数える | `test_cooldown_counts_from_an_aborted_op` |
| フォールトで中断 | 数えない（どうせ要求は `FAULT`） | `test_fault_abort_does_not_start_cooldown` (fault) |
| select との関係 | select は影響を受けず、始めもせず、終了も起点を動かさない | `test_cooldown_does_not_apply_to_select_and_select_does_not_start_it`, `test_cooldown_does_not_count_while_a_select_runs_or_after_it` |
| 拒否は状態を進めない | `COOLDOWN` で拒否しても `kvm_logic` の状態・`op_id` は変わらない | `test_cooldown_rejection_leaves_logic_untouched` |
| 0 = なし | 完了直後に再受理できる | `test_cooldown_disabled_when_interval_is_zero` |
| 時刻 | 要求は `now_ms` を取らず、直近の tick の時刻で数える | `test_cooldown_uses_time_of_last_tick` |

### 5.6 操作の記録・状態の報告

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| `op_id` と `last_op` | 受理のたびに 1 進み、直近の 1 件だけを保持。拒否・`ALREADY_SELECTED` は進めない | `test_op_ids_increment_and_only_last_op_is_kept` |
| pending は必ず決着 | 次の受理の前に completed / aborted になっている | `test_pending_op_is_always_resolved_before_next_accept` |
| `state` の対応 | `kvm_logic` の 11 状態を 5 つの `state` に対応づけ、`active_channel` は unknown のときだけ 0 | `test_status_state_mapping_covers_every_logic_state` |
| そのほかのフィールド | `video_sync`、`atx_locked`、`atx_anomaly`、`cooldown_ms` | `test_status_reports_lock_anomaly_cooldown_and_sync` |

### 5.7 フォールト（HAL の失敗、時刻の巻き戻り、`kvm_logic` の出力の不整合）

各テストは、失敗した呼び出しの直後に `release_all` が呼ばれ、フォールトがラッチされ、以後の要求が `FAULT`、**LED が回復しても `assert_line` / `set_addr` / `release_line` が 0 回**、`release_all` が tick ごとに 1 回であることを確認する（`expect_latched_and_quiet`）。

| 失敗 | テスト名 (fault) |
| --- | --- |
| `set_addr`（切替中、追従中） | `test_fault_set_addr_during_select`, `test_fault_set_addr_during_manual_tracking` |
| `assert_line`（KVM、ATX） | `test_fault_assert_line_kvm`, `test_fault_assert_line_atx` |
| `release_line`（パルス終了時、anomaly 時） | `test_fault_release_line_at_pulse_end`, `test_fault_release_line_at_anomaly` |
| `read_led_mask`（idle、線が有効な間、続けて失敗） | `test_fault_read_led_while_idle`, `test_fault_read_led_while_line_is_asserted`, `test_fault_read_led_persistent` |
| `release_all` の失敗と再試行 | `test_fault_release_all_keeps_being_retried_and_failure_does_not_unlatch` |
| 最初の原因の保持、完了済みの操作は変えない | `test_fault_keeps_first_reason`, `test_fault_does_not_change_completed_op` |
| `now_ms` の巻き戻り | `test_time_rewind_latches_fault`, `test_time_does_not_rewind_with_equal_or_large_jumps`, `test_first_tick_may_start_at_any_time` |
| `kvm_logic` が 2 本以上の Ē を Low にする | `test_fault_when_logic_enables_two_lines`, `test_fault_when_logic_enables_two_lines_including_the_matching_one` |
| 対応する受理済みの操作がない線 | `test_line_is_enabled_only_for_matching_pending_op`, `test_finished_op_does_not_justify_a_line` |
| A0/A1 が範囲外、線が有効なときの A0/A1 の変更 | `test_fault_when_logic_a0a1_out_of_range`, `test_fault_when_addr_changes_while_a_line_is_asserted`, `test_fault_when_addr_changes_in_the_same_tick_as_a_new_line` |

## 6. KVM / ATX の HTTP API（`kvm_api`、Issue #85 C-5b）

仕様は `docs/WEB_API.md` 7 章。テストは `test/host/kvm_api/`（`test_kvm_api_*`）と `test/host/http_guard/test_decide_kvm.c`。ミューテーションの結果は `docs/KVM_API_TESTS.md`。

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| ボディの解析 | `{"channel":N}`（N は 1..4）だけ受理。ほかのキー、重複、文字列、小数、指数、負数、先頭ゼロ、2 桁以上、巨大な数、余計な文字、NUL、空、65 バイト以上は不正。64 バイトちょうどは受理 | `test_valid_*`, `test_range_boundaries`, `test_number_forms`, `test_huge_numbers_do_not_wrap_into_range`, `test_length_limit_64` ほか (parse)、`test_parser_matches_independent_oracle` (fuzz) |
| 状態の JSON | 7.1 の例と一致。unknown は `active_channel: null`、`last_op` なしは `null`。バッファ不足は失敗 | `test_doc_example_exact`, `test_unknown_state_has_null_channel_and_no_last_op`, `test_buffer_boundaries` (format) |
| 結果コードの対応 | 受理 202、already_selected 200、invalid_request 400、busy / unknown_state / channel_mismatch / atx_locked 409、cooldown 429 + Retry-After、hw_fault / not_ready 503、想定外 500 | `test_select_*`, `test_power_*`, `test_channel_mismatch_*`, `test_unexpected_results_are_500` (handle) |
| Retry-After | 残りを切り上げた秒、最小 1（0 ms でも 1）。同じロックの中で読む | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_retry_after_for_every_ms_up_to_5_seconds` (handle) |
| 判定の順序（形式 -> 未起動 -> kvm_ctrl の順序） | 不正なボディは未起動でも 400。未起動なら kvm_ctrl に触れない。kvm_ctrl 内の順序は 5.4 | `test_form_error_beats_not_ready`, `test_order_unknown_beats_mismatch_and_cooldown`, `test_power_flow_busy_cooldown_and_retry_after` (integration) |
| lock / unlock の対応 | lock が取れたときだけ、全経路で unlock を 1 回。lock の外で kvm_ctrl を呼ばない | `be_assert_balanced`（handle の全テスト）、`test_handle_invariants_with_random_backend` (fuzz) |
| サービス未起動 | backend が NULL、ready が false、lock 不可、関数ポインタ欠落、kvm_ctrl 未初期化: 503 `not_ready`。status の未起動はログに出さない | `test_not_ready_cases_for_every_post_endpoint`, `test_backend_with_missing_function_is_not_ready`, `test_status_not_ready_cases`, `test_service_not_started_is_503_not_ready` |
| 実際の kvm_ctrl + モック HAL | select -> 受理 -> 切替完了 -> status、power の mismatch は何も出さない、フォールト後は 503 | `test_select_flow_to_completion_is_reflected_in_status`, `test_power_mismatch_is_409_and_outputs_nothing`, `test_fault_gives_503_hw_fault_and_nothing_is_driven` (integration) |
| ガード | 未認証・Origin なし・JSON でないリクエストは kvm_api に届かない | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` (decide_kvm)、`test_backend_is_touched_only_when_every_condition_holds` (gate) |
| 電源操作は現在チャンネルのときだけ | ランダムな API 列で、受理は常に「その時点の現在チャンネル」への操作 | `test_random_api_sequences_on_real_ctrl` (fuzz) |
