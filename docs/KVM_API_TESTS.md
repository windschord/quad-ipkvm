# kvm_api / http_guard(KVM 追加分)のテストとミューテーションテストの結果

作成: 2026-11-10(Issue #85、C-5b)。`components/kvm_api`(純 C: ボディの厳密な解析、状態 JSON、結果の HTTP 対応)と、`http_guard_decide` の KVM 用パスのホスト単体テストの構成と、ミューテーションテストの結果。シナリオとテストの対応は `docs/KVM_LOGIC_SCENARIOS.md` の 6 章。

## 1. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/kvm_api/test_kvm_api_parse.c` | ボディの解析の表(正常、境界 0/1/4/5、ほかのキー、重複、エスケープしたキー、文字列、小数、指数、負数、先頭ゼロ、巨大な数、余計な文字、NUL、64/65 バイト、全接頭辞、1 バイト置換) | 17 |
| `test/host/kvm_api/test_kvm_api_format.c` | 状態 JSON(7.1 の例と完全一致、unknown、全列挙値、最長の JSON、バッファの全境界、範囲外の列挙値) | 15 |
| `test/host/kvm_api/test_kvm_api_handle.c` | モック backend: 全結果コードの対応、Retry-After(切り上げ、最小 1、0〜5001 ms の全値)、lock/unlock の対応、サービス未起動の 4 通り、判定の順序、想定外の結果 | 32 |
| `test/host/kvm_api/test_kvm_api_integration.c` | 実際の `kvm_ctrl` + モック HAL: select -> 受理 -> 完了 -> status、power の mismatch は何も出さない、busy / cooldown / unknown / atx_locked / 追従、フォールト後は 503、未起動 | 15 |
| `test/host/kvm_api/test_kvm_api_gate.c` | 未認証・Origin なし・JSON でないリクエストが backend に届かないこと(`decide` -> `handle` の手順を全入力で) | 2 |
| `test/host/kvm_api/test_kvm_api_fuzz.c` | 固定シード、ASan/UBSan。(1) パーサーを POSIX 正規表現のオラクルと 30 万回突き合わせ (2) ランダムな backend・本文・エンドポイントで応答の不変条件 20 万回 (3) 実 `kvm_ctrl` + モック HAL へのランダムな API 列(60 回 x 4000 ステップ): 電源操作の受理は常に「その時点の現在チャンネル」、拒否は何も始めない、モックの違反フラグが立たない | 3 |
| `test/host/http_guard/test_decide_kvm.c` | `http_guard_decide` の KVM パス: 状態 x メソッド x パス x 4 つの真偽値の全 288 通り(独立に書いた期待値)、順序(Host -> セットアップ -> セッション -> メソッド -> Origin -> JSON)、既存パスの不変 | 10 |
| `test/host/web_assets/test_web_headers.c`(既存に追記) | `web_headers_allow_for_uri` の KVM パス(405 の `Allow` ヘッダー) | 既存のテストの中 |

追加した Unity のテストケースは合計 94 件(新しい ctest の実行ファイルは 7 つ)。既存を含む全 56 の ctest が通る。

ホストで確かめられないこと: `web_server.c` のハンドラが、`http_guard_decide` -> ボディの読み取り -> `kvm_api_handle` の順序で書かれていること、`kvm_service`(タスク・ミューテックス)の動作。IDF のヘッダーをスタブにした `gcc -fsyntax-only` までは通した。実機・IDF でのビルドと動作の確認は、Antigravity の device-check で行う。

## 2. ミューテーションテスト

方法: `tools/test/mutate_kvm_api.py` が、`components/` と `test/` をスクラッチにコピーし、`kvm_api.c` または `http_guard.c` の 1 か所を置換 -> 再ビルド -> 関連するテストを実行、を繰り返す(ミュータントの定義は `tools/test/kvm_api_mutants.py`)。実行するテストは、`kvm_api.c` のミュータントでは `test_kvm_api_*` の 6 つ、`http_guard.c` のミュータントでは `test_decide`、`test_decide_kvm`、`test_kvm_api_gate`、`test_kvm_api_integration`。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。`http_guard.c` のミュータントは、KVM 追加分(`HTTP_GUARD_PATH_KVM_*`、`POWER` のブロック)だけ。

```
python3 tools/test/mutate_kvm_api.py <repo> <スクラッチ> tools/test/kvm_api_mutants.py <結果.json>
```

**結果**(実行した最終の 1 回。ミュータントを入れる前のベースラインは、どちらも全テスト成功):

| 対象 | 実行した数 | 殺した数 | 生き残った数 | ビルドエラー |
|---|---|---|---|---|
| `kvm_api.c` | 129 | 128 | 1 | 0 |
| `http_guard.c`(KVM 追加分) | 18 | 18 | 0 | 0 |
| 合計 | 147 | 146 | 1 | 0 |

経緯: 最初の実行は 144 個(3 個はミュータントの old 文字列が一意でなく未実行)で、138 個を殺し、1 個が生き残り、5 個がビルドエラー(`-Werror` の未使用変数。ミュータントの書き方の問題)だった。ミュータントの書き方を直して(`(void)x;` を足す、old 文字列に前後を足して一意にする)、上の表のとおり**全体を最初から実行し直した**。テストの追加が必要になった生き残りはなかった。

### 2.1 同値のミュータント

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| K01 | parse: body_len == 0 accepted | `body_len == 0` の検査を外しても、空のボディでは `skip_ws` が進まず、次の `expect_char` が `*i < len` で偽になって -1 を返す。同じ結果になる冗長な防御 |

### 2.2 殺したミュータントの一覧

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(クラッシュは `<実行ファイル>:crash/rc=N`)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| K02 | parse: length limit 64 -> 65 | `test_length_limit_64`, `test_body_over_64_bytes_is_400` |
| K03 | parse: length limit 64 -> 63 (>=) | `test_length_limit_64`, `test_body_over_64_bytes_is_400` |
| K04 | parse: length limit removed | `test_length_limit_64`, `test_body_over_64_bytes_is_400` |
| K05 | parse: NULL channel out pointer not checked | `test_kvm_api_parse:crash/rc=1` |
| K06 | parse: NULL body not checked | `test_kvm_api_parse:crash/rc=1`, `test_kvm_api_handle:crash/rc=1` |
| K07 | parse: leading whitespace not skipped | `test_valid_with_json_whitespace`, `test_length_limit_64` |
| K08 | parse: '{' not required (any char) | `test_kvm_api_parse:crash/rc=1`, `test_kvm_api_fuzz:crash/rc=1` |
| K09 | parse: whitespace after '{' not skipped | `test_valid_with_json_whitespace`, `test_every_proper_prefix_is_rejected` |
| K10 | parse: key length check removed (over-read) | `test_kvm_api_parse:crash/rc=1`, `test_kvm_api_handle:crash/rc=1` |
| K11 | parse: key compare only first 8 chars | `test_single_byte_substitutions`, `test_parser_matches_independent_oracle` |
| K12 | parse: key compare ignores the memcmp result | `test_missing_or_other_keys`, `test_structure_errors` |
| K13 | parse: whitespace after key not skipped | `test_valid_with_json_whitespace`, `test_every_proper_prefix_is_rejected` |
| K14 | parse: ':' not required | `test_structure_errors`, `test_single_byte_substitutions` |
| K15 | parse: whitespace after ':' not skipped | `test_valid_with_json_whitespace`, `test_every_proper_prefix_is_rejected` |
| K16 | parse: value lower bound '1' -> '0' | `test_range_boundaries`, `test_single_byte_substitutions` |
| K17 | parse: value upper bound '4' -> '5' | `test_range_boundaries`, `test_single_byte_substitutions` |
| K18 | parse: value upper bound '4' -> '3' | `test_valid_channels`, `test_valid_with_json_whitespace` |
| K19 | parse: value lower bound '1' -> '2' | `test_valid_channels`, `test_valid_with_json_whitespace` |
| K20 | parse: value index check removed (over-read) | `test_kvm_api_parse:crash/rc=1`, `test_kvm_api_fuzz:crash/rc=1` |
| K21 | parse: value off by one (body[i] - '0' + 1) | `test_valid_channels`, `test_valid_with_json_whitespace` |
| K22 | parse: whitespace after value not skipped | `test_valid_with_json_whitespace`, `test_every_proper_prefix_is_rejected` |
| K23 | parse: '}' not required | `test_structure_errors`, `test_every_proper_prefix_is_rejected` |
| K24 | parse: trailing whitespace not skipped | `test_valid_with_json_whitespace`, `test_length_limit_64` |
| K25 | parse: trailing garbage allowed | `test_structure_errors`, `test_extra_characters_after_object` |
| K26 | parse: *channel written even on failure path (early) | `test_range_boundaries`, `test_missing_or_other_keys` |
| K27 | parse: vertical tab accepted as whitespace | `test_non_json_whitespace_is_rejected`, `test_parser_matches_independent_oracle` |
| K28 | parse: '\r' not whitespace | `test_valid_with_json_whitespace`, `test_parser_matches_independent_oracle` |
| K29 | parse: '\t' not whitespace | `test_valid_with_json_whitespace`, `test_parser_matches_independent_oracle` |
| K30 | parse: '\n' not whitespace | `test_valid_with_json_whitespace`, `test_parser_matches_independent_oracle` |
| K31 | parse: ' ' not whitespace | `test_valid_with_json_whitespace`, `test_length_limit_64` |
| K32 | parse: NUL treated as whitespace | `test_nul_bytes_are_rejected`, `test_parser_matches_independent_oracle` |
| K33 | parse: skip_ws ignores length (over-read) | `test_kvm_api_parse:crash/rc=1`, `test_body_over_64_bytes_is_400` |
| K34 | expect_char: length check removed (over-read) | `test_kvm_api_parse:crash/rc=1`, `test_kvm_api_fuzz:crash/rc=1` |
| K35 | status: state name idle -> busy | `test_doc_example_exact`, `test_state_names` |
| K36 | status: state name tracking -> switching | `test_state_names`, `test_tracking_manual_switch_is_busy` |
| K37 | status: state name atx -> power | `test_state_names`, `test_power_flow_busy_cooldown_and_retry_after` |
| K38 | status: invalid state accepted | `test_invalid_enum_values_fail`, `test_status_unexpected_failures_are_500` |
| K39 | status: sync name no_signal -> nosignal | `test_video_sync_names` |
| K40 | status: invalid sync accepted | `test_invalid_enum_values_fail` |
| K41 | status: op kind power_reset -> power_force | `test_last_op_all_kinds_and_results` |
| K42 | status: op result aborted -> completed | `test_last_op_all_kinds_and_results`, `test_fault_during_a_pulse_aborts_the_op_and_is_reported` |
| K43 | status: invalid op kind accepted | `test_invalid_enum_values_fail` |
| K44 | status: invalid op result accepted | `test_invalid_enum_values_fail` |
| K45 | status: active_channel not null for unknown | `test_unknown_state_hides_a_stale_channel` |
| K46 | status: active_channel lower bound removed | `test_active_channel_out_of_range_is_null` |
| K47 | status: active_channel upper bound 4 -> 5 | `test_active_channel_out_of_range_is_null` |
| K48 | status: atx_locked and atx_anomaly swapped | `test_unknown_state_has_null_channel_and_no_last_op`, `test_flags_are_independent` |
| K49 | status: fault always false | `test_flags_are_independent`, `test_status_with_fault_is_still_200` |
| K50 | status: cooldown_ms not reported | `test_cooldown_ms_values`, `test_status_ok` |
| K51 | status: last_op shown even if absent | `test_unknown_state_has_null_channel_and_no_last_op`, `test_last_op_hidden_when_has_last_op_false` |
| K52 | status: last_op channel reports op_id | `test_doc_example_exact`, `test_last_op_all_kinds_and_results` |
| K53 | status: NULL status not rejected | `test_kvm_api_format:crash/rc=1` |
| K54 | status: writer overflow not reported (>= -> >) | `test_kvm_api_format:crash/rc=1` |
| K55 | status: failure leaves partial output | `test_kvm_api_format:crash/rc=1` |
| K56 | status: out_len not zeroed on failure | `test_kvm_api_format:crash/rc=1` |
| K57 | status: u32 digit order reversed | `test_cooldown_ms_values`, `test_last_op_all_kinds_and_results` |
| K58 | status: u32 zero printed as empty | `test_doc_example_exact`, `test_unknown_state_has_null_channel_and_no_last_op` |
| K59 | status: w_init ok even with cap 0 | `test_kvm_api_format:crash/rc=1` |
| K60 | map: select accepted status 202 -> 200 | `test_select_accepted`, `test_select_op_id_is_passed_through` |
| K61 | map: power accepted omits action | `test_power_accepted_each_action`, `test_power_flow_busy_cooldown_and_retry_after` |
| K62 | map: power action name from wrong endpoint (reset -> press) | `test_power_accepted_each_action`, `test_power_flow_busy_cooldown_and_retry_after` |
| K63 | map: force endpoint passes press | `test_power_accepted_each_action`, `test_each_power_action_pulses_its_own_line_only_for_the_active_channel` |
| K64 | map: press endpoint passes reset | `test_power_accepted_each_action`, `test_power_flow_busy_cooldown_and_retry_after` |
| K65 | map: action name press -> power | `test_power_accepted_each_action`, `test_power_flow_busy_cooldown_and_retry_after` |
| K66 | map: op_id omitted from accepted body | `test_select_accepted`, `test_select_op_id_is_passed_through` |
| K67 | map: accepted channel reports 0 | `test_select_accepted`, `test_select_op_id_is_passed_through` |
| K68 | map: already_selected status 200 -> 202 | `test_select_already_selected_is_200`, `test_select_flow_to_completion_is_reflected_in_status` |
| K69 | map: already_selected for power not an error | `test_unexpected_results_are_500` |
| K70 | map: invalid_param -> 409 | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K71 | map: busy -> 429 | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K72 | map: busy code renamed | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K73 | map: unknown_state -> 400 | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K74 | map: channel_mismatch status 409 -> 400 | `test_channel_mismatch_reports_active_channel`, `test_channel_mismatch_with_unusable_active_channel_is_null` |
| K75 | map: channel_mismatch reports requested channel, not active | `test_channel_mismatch_reports_active_channel`, `test_channel_mismatch_with_unusable_active_channel_is_null` |
| K76 | map: mismatch active 1..4 check lower bound removed | `test_channel_mismatch_with_unusable_active_channel_is_null` |
| K77 | map: mismatch active upper bound 5 | `test_channel_mismatch_with_unusable_active_channel_is_null` |
| K78 | map: atx_locked -> 503 | `test_power_error_mapping_each_action`, `test_atx_locked_is_409` |
| K79 | map: cooldown -> 409 | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_with_unreadable_status_still_has_retry_after_1` |
| K80 | map: cooldown without Retry-After | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_with_unreadable_status_still_has_retry_after_1` |
| K81 | map: Retry-After rounds down | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_retry_after_for_every_ms_up_to_5_seconds` |
| K82 | map: Retry-After rounds to nearest | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_retry_after_for_every_ms_up_to_5_seconds` |
| K83 | map: Retry-After +1000 (exact multiples bumped) | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_retry_after_for_every_ms_up_to_5_seconds` |
| K84 | map: Retry-After minimum removed | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_retry_after_for_every_ms_up_to_5_seconds` |
| K85 | map: Retry-After computed in 32 bits (wraps) | `test_cooldown_retry_after_rounds_up_with_minimum_1` |
| K86 | map: Retry-After uses 0 instead of status cooldown | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_retry_after_for_every_ms_up_to_5_seconds` |
| K87 | map: status read only on mismatch (not cooldown) | `test_cooldown_retry_after_rounds_up_with_minimum_1`, `test_cooldown_retry_after_for_every_ms_up_to_5_seconds` |
| K88 | map: status read only on cooldown (not mismatch) | `test_channel_mismatch_reports_active_channel`, `test_channel_mismatch_status_is_read_under_the_same_lock` |
| K89 | map: status read after unlock (not under lock) | `test_channel_mismatch_reports_active_channel`, `test_channel_mismatch_status_is_read_under_the_same_lock` |
| K90 | map: status failure on mismatch used anyway | `test_channel_mismatch_with_unusable_active_channel_is_null`, `test_cooldown_with_unreadable_status_still_has_retry_after_1` |
| K91 | map: fault -> 500 | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K92 | map: fault code renamed | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K93 | map: kvm_ctrl NOT_READY mapped to internal error | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K94 | map: unexpected result treated as accepted | `test_unexpected_results_are_500`, `test_handle_invariants_with_random_backend` |
| K95 | map: not_ready -> 500 | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K96 | map: internal_error -> 503 | `test_unexpected_results_are_500`, `test_invalid_endpoint_is_500_and_touches_nothing` |
| K97 | handle: unlock not called after select/power | `test_select_accepted`, `test_select_already_selected_is_200` |
| K98 | handle: unlock skipped on mismatch/cooldown path (leak) | `test_channel_mismatch_reports_active_channel`, `test_channel_mismatch_status_is_read_under_the_same_lock` |
| K99 | handle: status unlock missing | `test_status_ok`, `test_status_not_ready_cases` |
| K100 | handle: select endpoint calls power | `test_select_accepted`, `test_select_already_selected_is_200` |
| K101 | handle: power endpoint calls select | `test_power_accepted_each_action`, `test_power_error_mapping_each_action` |
| K102 | handle: backend gets a different channel | `test_select_accepted`, `test_select_flow_to_completion_is_reflected_in_status` |
| K103 | handle: not_ready checked before the form (400 hidden) | `test_invalid_body_is_400_and_does_not_reach_the_backend`, `test_not_ready_cases_for_every_post_endpoint` |
| K104 | handle: invalid body does not set 400 status | `test_invalid_body_is_400_and_does_not_reach_the_backend`, `test_body_over_64_bytes_is_400` |
| K105 | handle: log_channel not recorded | `test_select_accepted`, `test_select_already_selected_is_200` |
| K106 | handle: lock failure ignored | `test_not_ready_cases_for_every_post_endpoint`, `test_handle_invariants_with_random_backend` |
| K107 | handle: status lock failure ignored | `test_status_not_ready_cases`, `test_handle_invariants_with_random_backend` |
| K108 | handle: ready() not consulted | `test_not_ready_cases_for_every_post_endpoint`, `test_not_ready_beats_every_kvm_ctrl_result_by_construction` |
| K109 | handle: ready function pointer not checked | `test_kvm_api_handle:crash/rc=1` |
| K110 | handle: lock function pointer not checked | `test_kvm_api_handle:crash/rc=1` |
| K111 | handle: unlock function pointer not checked | `test_kvm_api_handle:crash/rc=1` |
| K112 | handle: get_status function pointer not checked | `test_backend_with_missing_function_is_not_ready` |
| K113 | handle: select function pointer not checked | `test_backend_with_missing_function_is_not_ready` |
| K114 | handle: power function pointer not checked | `test_kvm_api_handle:crash/rc=1` |
| K115 | handle: NULL backend not checked | `test_kvm_api_handle:crash/rc=1`, `test_kvm_api_fuzz:crash/rc=1` |
| K116 | handle: status endpoint logs on success | `test_status_ok`, `test_status_after_boot` |
| K117 | handle: status NOT_READY mapped to 500 | `test_status_not_ready_cases`, `test_kvm_ctrl_not_initialized_is_503_not_ready` |
| K118 | handle: status accepts non-OK result | `test_status_unexpected_failures_are_500` |
| K119 | handle: status format failure ignored | `test_status_ok`, `test_status_with_fault_is_still_200` |
| K120 | handle: status body_len not set | `test_status_ok`, `test_handle_invariants_with_random_backend` |
| K121 | handle: invalid endpoint falls through to power | `test_invalid_endpoint_is_500_and_touches_nothing`, `test_handle_invariants_with_random_backend` |
| K122 | handle: NULL response not checked | `test_kvm_api_handle:crash/rc=1` |
| K123 | handle: POST not logged | `test_select_accepted`, `test_select_already_selected_is_200` |
| K124 | handle: endpoint kind select -> power_press | `test_select_accepted`, `test_select_already_selected_is_200` |
| K125 | handle: endpoint kind power_force -> power_reset | `test_power_accepted_each_action`, `test_power_error_mapping_each_action` |
| K126 | handle: response not zeroed | `test_kvm_api_handle:crash/rc=1`, `test_kvm_api_fuzz:crash/rc=1` |
| K127 | handle: set_error body_len not set | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K128 | handle: set_error log_reason not set | `test_select_error_mapping`, `test_power_error_mapping_each_action` |
| K129 | handle: status endpoint dispatched as POST | `test_body_is_ignored_for_status`, `test_backend_with_missing_function_is_not_ready` |
| G01 | guard: status allows POST | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G02 | guard: status requires POST instead of GET | `test_exhaustive_kvm_paths`, `test_allow_requires_everything` |
| G03 | guard: status always allowed (method ignored) | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G04 | guard: status requires Origin | `test_exhaustive_kvm_paths`, `test_allow_requires_everything` |
| G05 | guard: select/power allow GET | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G06 | guard: select/power method check removed | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G07 | guard: select/power Origin not required | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G08 | guard: select/power JSON not required | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G09 | guard: JSON checked before Origin | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G10 | guard: Origin checked before method | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G11 | guard: power path not covered (falls to generic POST) | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G12 | guard: select path not covered (falls to generic POST) | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G13 | guard: status path not covered (falls to generic) | `test_exhaustive_kvm_paths`, `test_check_order_session_method_origin_json` |
| G14 | guard: KVM block placed before the session check (no 401) | `test_exhaustive_kvm_paths`, `test_unauthenticated_is_401_on_every_kvm_path_and_method` |
| G15 | guard: power path ignores the session | `test_exhaustive_kvm_paths`, `test_unauthenticated_is_401_on_every_kvm_path_and_method` |
| G16 | guard: status ignores the session | `test_exhaustive_kvm_paths`, `test_unauthenticated_is_401_on_every_kvm_path_and_method` |
| G17 | guard: NEEDS_SETUP lets the KVM paths through | `test_exhaustive_kvm_paths`, `test_needs_setup_blocks_every_kvm_path` |
| G18 | guard: select allowed without session check in NEEDS_SETUP | `test_exhaustive_kvm_paths`, `test_needs_setup_blocks_every_kvm_path` |
