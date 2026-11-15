# video_status のテストとミューテーションテストの結果

作成: 2026-11-15(Issue #86、C-6a)。`components/video_status`(映像の同期状態の「提供元」)と、`kvm_ctrl` + モック HAL への結線のホスト単体テストの構成と、`video_status.c` に対するミューテーションテストの結果。契約は `docs/INTERFACES.md` 1.3(提供元)と 6.4。

## 1. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/video_status/test_video_status_provider.c` | 既定(UNKNOWN / 0 / 0)、登録・取り替え・NULL に戻す、ctx の受け渡し、範囲外の state の補正、`LOCKED` で 0 の解像度、`LOCKED` 以外で 0 でない解像度、`out == NULL`、`video_status_hal_sync` | 16 |
| `test/host/video_status/test_video_status_fake.c` | 偽の提供元(`video_status_fake_t`): 初期化、設定、検査しないこと、3 状態の全遷移(9 通り)、NULL の扱い、独立性 | 8 |
| `test/host/video_status/test_video_status_wiring.c` | 結線(統合): 実際の `kvm_ctrl`(`kvm_logic`)+ モック HAL(`test/host/kvm_board/`)+ 偽の映像状態。HAL の `video_sync` を `video_status_hal_sync` に差し替える(`kvm_service` と同じ形) | 14 |

合計 38 件。`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルが 3 つ増える。

**この範囲で確かめていないこと**: `kvm_service.c`、`main/main.c`、`test_pattern_source.c` は IDF 専用で、ホストのテストにない。IDF のビルドは確認していない(`kvm_service.c` と `test_pattern_source.c` は、スタブのヘッダーで `gcc -fsyntax-only` を通し、`test_pattern_source` の既定値と切り替えは、使い捨てのホストの小さなプログラムで確認しただけで、リポジトリには入れていない)。

## 2. 結線のテストが固定した `kvm_logic` の既存の挙動

期待値は、`kvm_logic.c` を読んで決めた。依頼の想定と食い違った点も含む。**実装は変えていない**。

| 場面 | 実際の挙動 | テスト |
|---|---|---|
| 起動直後(映像 UNKNOWN) | LED が安定すれば idle になる。idle の ATX のロックは映像に依存しない(ロック解除) | `test_boot_with_unknown_video_reaches_idle_and_reports_unknown` |
| 切替(映像が NO_SIGNAL -> LOCKED) | 映像待ち(`SWITCH_WAIT_VIDEO`)の間は、ATX がロック(`busy`)。LOCKED になった次の tick で完了(settle 時間を待たない) | `test_switch_completes_when_video_goes_no_signal_then_locked` |
| 切替(NO_SIGNAL のまま) | 映像待ちに入ってから `no_signal_settle_ms`(既定 500 ms)で完了 | `test_switch_with_persistent_no_signal_completes_after_settle` |
| 切替(**LOCKED のまま変わらない**) | **タイムアウトを待たず**、映像待ちに入った次の tick で完了する(LOCKED の立ち上がりは要求しない) | `test_switch_with_video_locked_throughout_completes_at_once` |
| 切替(UNKNOWN のまま) | `switch_video_sync_timeout_ms`(既定 3000 ms)後に完了 | `test_switch_with_video_unknown_completes_at_the_timeout`, `test_without_provider_video_stays_unknown_and_kvm_still_works_safely` |
| **idle で映像が NO_SIGNAL** | **ATX はロックされない**(受理される)。電源の入っていない PC を起動するために必要 | `test_atx_is_not_locked_in_idle_while_video_is_no_signal` |
| 手動の切替の追従 | 追従中は ATX がロック。映像 LOCKED で idle に戻る | `test_manual_switch_tracking_waits_for_video_and_keeps_atx_locked` |
| 提供元がない | 映像は UNKNOWN のまま。idle には入れて ATX も受理される。切替はタイムアウトで完了 | `test_without_provider_video_stays_unknown_and_kvm_still_works_safely` |
| `status` の `video_sync` | 直近の tick で読んだ値。tick の間は変わらない。不整合な値は UNKNOWN | `test_status_video_sync_follows_the_fake_per_tick`, `test_inconsistent_provider_values_reach_kvm_as_unknown` |
| 結線の形 | tick ごとに提供元を 1 回だけ読む(`kvm_ctrl_get_status`、要求は読まない)。モック HAL 自身の `video_sync` は呼ばれず、`mock_set_sync` は効かない | `test_provider_is_read_once_per_tick_and_the_boards_own_video_sync_is_not_used` |

## 3. ミューテーションテスト

方法: `tools/test/mutate_video_status.py` が、`components/` と `test/` をスクラッチにコピーし、`video_status.c` の 1 か所を置換 -> 再ビルド -> `test_video_status_provider` / `_fake` / `_wiring` を実行、を繰り返す(ミュータントの定義は `tools/test/video_status_mutants.py`)。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。対象は `video_status.c` の全体(`normalize`、`set_provider`、`get`、`hal_sync`、偽の提供元)。

```
python3 tools/test/mutate_video_status.py <repo> <スクラッチ> tools/test/video_status_mutants.py <結果.json>
```

**結果**(最終の 1 回。ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|
| 54 | 52 | 2 | 0 |

経緯: 最初の実行(54 個)では、5 個がビルドエラーだった(`-Werror` の未使用の引数・関数の警告。ミュータントの書き方の問題で、`(void)` を足して書き直した)。書き直した 5 個を単独で実行して全部殺したあと、**54 個全部を最初から実行し直した**のが上の結果。生き残りに対してテストは足していない(下の 2 個は同値)。

### 3.1 同値のミュータント(2 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| V11 | normalize: range check forgets UNKNOWN | UNKNOWN を「範囲外」の分岐に入れても、代入するのは UNKNOWN で、そのあとの「LOCKED 以外は解像度 0」も同じ。結果が変わらない |
| V22 | set_provider: ctx not discarded when fn is NULL | `fn` が NULL のとき、`get` は提供元を呼ばずに戻るので、保持した `ctx` は読まれない(再登録は `p` ごと上書き)。「ctx も捨てる」は、意図を明示するための防御で、観測できない |

### 3.2 殺したミュータントの一覧(52 個)

「殺したテスト」は、失敗したテストのうち先頭の最大 2 つ。`crash` は、NULL の参照などで ASan が落とした(テストの名前が取れない)もの。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| V01 | normalize: LOCKED zero check || -> && (needs both 0) | `test_locked_with_zero_width_or_height_becomes_unknown`, `test_inconsistent_provider_values_reach_kvm_as_unknown` |
| V02 | normalize: LOCKED zero check ignores height | `test_locked_with_zero_width_or_height_becomes_unknown` |
| V03 | normalize: LOCKED zero check ignores width | `test_locked_with_zero_width_or_height_becomes_unknown`, `test_inconsistent_provider_values_reach_kvm_as_unknown` |
| V04 | normalize: LOCKED zero check never fires | `test_locked_with_zero_width_or_height_becomes_unknown`, `test_hal_sync_returns_the_checked_state` |
| V05 | normalize: LOCKED zero check boundary (width == 1 treated as zero) | `test_valid_values_pass_through` |
| V06 | normalize: LOCKED with zero size -> NO_SIGNAL instead of UNKNOWN | `test_locked_with_zero_width_or_height_becomes_unknown`, `test_hal_sync_returns_the_checked_state` |
| V07 | normalize: LOCKED branch is `if` not `else if` (LOCKED then fails the range check) | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V08 | normalize: range check && -> || (NO_SIGNAL and UNKNOWN fail it) | `test_provider_can_be_replaced`, `test_provider_registered_with_null_ctx_gets_null` |
| V09 | normalize: range check forgets NO_SIGNAL (becomes UNKNOWN) | `test_provider_can_be_replaced`, `test_provider_registered_with_null_ctx_gets_null` |
| V10 | normalize: range check never fires (out-of-range kept) | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_hal_sync_returns_the_checked_state` |
| V12 | normalize: out-of-range state -> NO_SIGNAL instead of UNKNOWN | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_hal_sync_returns_the_checked_state` |
| V13 | normalize: out-of-range state -> LOCKED | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_hal_sync_returns_the_checked_state` |
| V14 | normalize: zeroing only for UNKNOWN (NO_SIGNAL keeps its size) | `test_non_locked_states_get_zero_size` |
| V15 | normalize: zeroing only for NO_SIGNAL (UNKNOWN keeps its size) | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_locked_with_zero_width_or_height_becomes_unknown` |
| V16 | normalize: non-LOCKED width not zeroed | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_locked_with_zero_width_or_height_becomes_unknown` |
| V17 | normalize: non-LOCKED height not zeroed | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_locked_with_zero_width_or_height_becomes_unknown` |
| V18 | normalize: zeroing also for LOCKED (always) | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V19 | normalize: zeroing never happens | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_locked_with_zero_width_or_height_becomes_unknown` |
| V20 | set_provider: fn ignored (always none) | `test_registered_provider_is_called_with_its_ctx`, `test_provider_is_called_once_per_get_and_receives_registered_ctx` |
| V21 | set_provider: ctx always NULL | `test_video_status_provider:crash/rc=1`, `test_fake_all_state_transitions_through_video_status_get` |
| V23 | set_provider: NULL fn does not reset (keeps the old provider) | `test_video_status_provider:crash/rc=1`, `test_removing_the_provider_reports_unknown_from_the_next_tick` |
| V24 | set_provider: stores nothing | `test_registered_provider_is_called_with_its_ctx`, `test_provider_is_called_once_per_get_and_receives_registered_ctx` |
| V25 | get: out == NULL not checked | `test_video_status_provider:crash/rc=1` |
| V26 | get: no provider check (calls NULL fn) | `test_video_status_provider:crash/rc=1`, `test_video_status_wiring:crash/rc=1` |
| V27 | get: provider called with NULL ctx | `test_video_status_provider:crash/rc=1`, `test_fake_all_state_transitions_through_video_status_get` |
| V28 | get: provider called with out == NULL | `test_video_status_provider:crash/rc=1`, `test_fake_all_state_transitions_through_video_status_get` |
| V29 | get: provider result not normalized | `test_out_of_range_state_becomes_unknown_with_zero_size`, `test_locked_with_zero_width_or_height_becomes_unknown` |
| V30 | get: default state is NO_SIGNAL | `test_default_is_unknown_zero_zero`, `test_default_hal_sync_is_unknown` |
| V31 | get: out->width not initialized before the provider | `test_default_is_unknown_zero_zero`, `test_null_provider_restores_default` |
| V32 | get: out->height not initialized before the provider | `test_default_is_unknown_zero_zero`, `test_null_provider_restores_default` |
| V33 | get: out->state not initialized before the provider | `test_default_is_unknown_zero_zero`, `test_null_provider_restores_default` |
| V34 | get: provider called twice | `test_provider_is_called_once_per_get_and_receives_registered_ctx`, `test_provider_is_read_once_per_tick_and_the_boards_own_video_sync_is_not_used` |
| V35 | hal_sync: always UNKNOWN | `test_registered_provider_is_called_with_its_ctx`, `test_hal_sync_returns_the_checked_state` |
| V36 | hal_sync: LOCKED reported as NO_SIGNAL | `test_registered_provider_is_called_with_its_ctx`, `test_hal_sync_returns_the_checked_state` |
| V37 | hal_sync: NO_SIGNAL reported as UNKNOWN | `test_hal_sync_returns_the_checked_state`, `test_fake_all_state_transitions_through_video_status_get` |
| V38 | hal_sync: does not read video_status_get (stale local) | `test_registered_provider_is_called_with_its_ctx`, `test_provider_is_called_once_per_get_and_receives_registered_ctx` |
| V39 | fake_init: state is NO_SIGNAL | `test_fake_init_is_unknown_zero_zero`, `test_two_fakes_are_independent` |
| V40 | fake_init: width not cleared | `test_fake_init_is_unknown_zero_zero` |
| V41 | fake_init: height not cleared | `test_fake_init_is_unknown_zero_zero` |
| V42 | fake_init: NULL not checked | `test_video_status_fake:crash/rc=1` |
| V43 | fake_set: state not stored | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V44 | fake_set: width not stored | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V45 | fake_set: height not stored | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V46 | fake_set: width and height swapped | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V47 | fake_set: NULL not checked | `test_video_status_fake:crash/rc=1` |
| V48 | fake_provider: out == NULL not checked | `test_video_status_fake:crash/rc=1` |
| V49 | fake_provider: NULL ctx leaves out untouched | `test_fake_provider_with_null_ctx_is_unknown` |
| V50 | fake_provider: NULL ctx -> NO_SIGNAL | `test_fake_provider_with_null_ctx_is_unknown` |
| V51 | fake_provider: copies only the state | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V52 | fake_provider: copies state and width only | `test_registered_provider_is_called_with_its_ctx`, `test_provider_can_be_replaced` |
| V53 | fake_provider: NULL ctx not checked | `test_video_status_fake:crash/rc=1` |
| V54 | fake_provider: normalizes (the fake must stay unchecked) | `test_fake_provider_returns_the_stored_value_unchecked` |
