# KVM Logic Scenarios & Tests

本ドキュメントは、`kvm_logic` 状態機械の期待動作シナリオと、それぞれに対応するユニットテスト名を定義します。

## 1. 正常系・状態遷移シナリオ (test_switch.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 正常切替 (Web/パネル) | IDLE -> (ATXロック) -> A0/A1変更 -> KVMボタンパルス -> LED変化待ち -> 映像同期(LOCKED)待ち -> ATXロック解除。 | `test_switch_normal_success` |
| 現在チャンネルへの切替要求 | 即座に成功扱い（または無視）とし、ATXロック等は行わない。 | `test_switch_to_current_channel_is_ignored` |
| 切替タイムアウト | LED変化待ちでタイムアウトした場合、失敗として実際のLED値にA0/A1を戻し、ATXロック解除。A0/A1戻し時も不変条件4（全Ē High）を守る。 | `test_switch_led_timeout_reverts_a0a1` |
| 映像再同期タイムアウト / 無信号 | 切替後、LEDは変化したが映像同期がタイムアウト、または NO_SIGNAL の場合でも、ATXロックは解除する。 | `test_switch_video_sync_timeout_releases_atx_lock`, `test_switch_video_no_signal_releases_atx_lock` |
| 手動切替の追従 | 外部要因でLEDが変化した場合、即座にATXロックし、LED安定後にA0/A1を追従。映像同期を待ってからATX解除。 | `test_manual_switch_tracking` |
| 起動直後のLED確定待ち | 初期状態 (UNKNOWN) から、LEDが安定するまで待ち、確定後にA0/A1を設定してIDLEへ移行。 | `test_startup_led_stabilization` |

## 2. ATX 制御シナリオ (test_atx.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 正常 ATX パルス | チャンネルが一致・IDLE状態でパルス開始。指定時間後にパルス終了（全Ē High）。 | `test_atx_normal_pulse` |
| チャンネル不一致による拒否 | 要求チャンネルが現在LEDと異なる場合は拒否（エラーコード返却）。 | `test_atx_rejected_on_channel_mismatch` |
| ロック中・切替中による拒否 | 切替中や映像再同期待ちなどATXロック中は拒否。 | `test_atx_rejected_when_locked_or_switching` |
| UNKNOWN 状態での拒否 | LEDが不定 (UNKNOWN) の間はATX要求を拒否。 | `test_atx_rejected_when_unknown` |
| パルス長上限による clamp | 要求されたパルス長が設定(config)の最大値を超える場合、最大値に切り詰める（clampする）。※board側でも別途clampする前提。 | `test_atx_pulse_length_clamped` |
| パルス中のLED変化（異常） | パルス中にLEDが不一致になった場合、直ちに全ĒをHighにしパルス中断。異常を記録。 | `test_atx_aborted_and_anomaly_recorded_if_led_changes_during_pulse` |
| パルス終了時のLED不一致のLED異常 | パルス開始前・終了後にLEDが一致しない場合、要求拒否・異常記録。 | `test_atx_anomaly_on_pulse_start_or_end` |

## 3. 競合・異常シナリオ (test_unknown.c / test_switch.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| LED異常状態 (UNKNOWN) | LED全消灯、複数点灯、チャタリング時は UNKNOWN へ移行。ATXロック、全Ē High、A0/A1保持。 | `test_unknown_state_on_invalid_led` |
| 切替中の手動切替 (LED変化) | 切替中に予期せぬLED変化（別チャンネルへの移行など）があった場合、手動切替追従またはエラーとして安全に処理。 | `test_switch_interrupted_by_unexpected_led_change` |
| タイムアウト後にLEDが遅れて変化 | 切替タイムアウトし復旧処理に入った後・完了後に目的のLEDへ変化した場合、手動切替の追従として扱う。 | `test_manual_switch_tracked_after_switch_timeout` |
| 切替要求の重複 | 切替中に別の切替要求が来た場合は拒否（または後勝ち処理、今回は拒否とする）。 | `test_duplicate_switch_requests_rejected` |

## 4. 不変条件とその他の要件 (test_invariants.c)

| シナリオ | 期待動作 | テスト名 |
| --- | --- | --- |
| 【不変条件1】 A0/A1の一致 | 切替中(A0/A1変更からLED確定まで)を除き、A0/A1はLEDが示す現在チャンネルと一致していること。 | `test_invariant_1_a0a1_matches_led` |
| 【不変条件2】 切替中のATX保護 | 切替中(ボタン押下〜LED確定+HDMI再同期)は、ATXの Ē が決して Low にならないこと。 | `test_invariant_2_no_atx_during_switch` |
| 【不変条件3】 ATX LED監視 | ATXパルス前後・中のLED監視と上限。 | (test_atx.c で網羅) |
| 【不変条件4】 A0/A1変更手順 | A0/A1の変更時は、必ず「全Ē High -> A0/A1変更 -> セットアップ時間 -> Ē Low」の順序を守ること。いかなる時も Ē が Low の状態で A0/A1 を変更してはならない。 | `test_invariant_4_a0a1_change_sequence` |
| タイムスタンプ・ラップアラウンド安全確認 | 外部注入される `now_ms` のラップアラウンド耐性。uint32ではなくuint64を想定するが、ロジック内のタイマー差分計算が安全か。 | `test_time_wraparound_safety` |
