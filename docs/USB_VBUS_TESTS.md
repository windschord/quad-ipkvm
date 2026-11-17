# usb_vbus_ctrl のテストとミューテーションテストの結果

作成: 2026-11-17。`components/usb_vbus_ctrl`(USB の D+ プルアップの「接続・切断の判断」だけをする、状態を持つ純 C、IDF 非依存)のホスト単体テストの構成と、`usb_vbus_ctrl.c` に対するミューテーションテストの結果。設計は `docs/USB_VBUS_DESIGN.md`(3.2)、仕様の一次は USB 2.0 仕様 §7.1.5.1・§7.2.1(原文は `docs/HW_VERIFY.md` H.0)。GPIO の読み出しと `tud_connect` / `tud_disconnect` / 強制遮断の実行は、呼び出し側(`usb_hid` の結線。Antigravity)の担当で、このコンポーネントは判断だけをする。

## 1. 規則と、設計から判断して固定した点

| 点 | 決めたこと | 理由 |
|---|---|---|
| 起動時 | 切断(プルアップなし)。`enabled` かつ VBUS 検出の間だけ判断する | 設計 3.2。§7.2.1 |
| VBUS 検出 | `sample_valid && vbus_present`。`sample_valid = false`(GPIO の読み出しの失敗)は VBUS なし | 安全側(切断の側。CONNECT は返らず、接続中なら切断の数えが進む) |
| 連続の定義 | 数えるのは「いまの状態と逆の向き」(切断の状態なら検出、接続の状態なら VBUS なし)。逆の向きでないサンプルが 1 回でも来たら、数えを捨てる | 設計「VBUS の状態が 1 回でも反転したら数え直し」 |
| 境界 | 経過 `>= connect_debounce_ms` で CONNECT、`>= disconnect_debounce_ms` で DISCONNECT(ちょうどで返す。0 なら最初の tick) | 「この時間、連続して」を満たした tick |
| `applied(ok = true)` | 状態が変わり、数えを捨てる。次の tick から新しい状態の「逆の向き」を最初から数える | 接続の tick までの「VBUS あり」の連続を、切断側の数えに持ち越さない |
| `applied(ok = false)` | 状態も数えも変えない。条件が続く限り、次の update で同じ action(切断が長引けば FORCE_OFF)。デバウンスはやり直さない | 設計「成功するまで再試行」 |
| 再試行の最中に VBUS が戻る(切断)/ 消える(接続) | action は返らなくなり、数えは最初から | 連続の定義。VBUS が戻れば、プルアップは仕様に反さない |
| 強制遮断(FORCE_OFF) | 接続の状態で、最初の `DISCONNECT` を返した tick から `force_off_after_ms` 以上経ったら、`DISCONNECT` の代わりに `FORCE_OFF`(以後、成功するまで毎回)。`DISCONNECT` を一度も返していなければ返さない | 設計 3.2。サンプルが疎でも、ソフトの切断を先に試す |
| `applied(FORCE_OFF, ok = true)` | 切断の状態になり、`force_off_count` を数える(`disconnects` には数えない)。`disconnect_failure_count` は `DISCONNECT` の失敗だけ、`failures` は全部 | 統計の意味を分ける |
| `force_off_after_ms` = 0 | init が 1 にする | 小さいほど 10 秒の保証側。0 を既定に直すと、ゼロ初期化の設定が、黙って長い待ちになる |
| 切り詰め(init) | `disconnect_debounce_ms` は 3000 以下、`force_off_after_ms` は 1..2000。`connect_debounce_ms` は切り詰めない | 設計 3.2 の制限値 |
| 時計の逆行 | 数えているデバウンスと、強制遮断の待ち(「DISCONNECT を返した」記録)を、その `now_ms` から数え直す。同時刻は逆行ではない | 連続の保証が崩れた側に倒す。逆行が続く限り、保証は崩れる(単調な時計が前提) |
| オーバーフロー | `now_ms` は 64 ビットで、引き算(経過)だけ。32 ビットに切り詰めない | |
| `enabled = false` | 最初の `update` で CONNECT を 1 回だけ。以後は NONE(FORCE_OFF も出さない)。VBUS の入力も `now_ms` も見ない。`applied(CONNECT, false)` でも再試行しない | 「いまの動作」。**この場合 §7.2.1 は満たさない**(設計 3.2) |
| NULL | `update(NULL)` は NONE、`is_connected(NULL)` は false、`applied(NULL, ...)` は何もしない、`get_stats(NULL, out)` は out を 0 に。`update(c, NULL)` は「`sample_valid = false`、`now_ms` は直前の値」のサンプル | 安全側 |

### 1.1 10 秒の保証(仕様の不変条件)

記号: `P` = `update` の呼び出し周期の上限(連続する `update` の `now_ms` の差。`USB_VBUS_CTRL_POLL_PERIOD_MAX_MS` = 1000)、`D` = `disconnect_debounce_ms`(上限 3000)、`F` = `force_off_after_ms`(上限 2000)。判定は `update` の呼び出しごとにしか行われないので、`P` の分が段ごとに加算される。VBUS が物理的に外れてから:

1. 最初に「なし」を観測するまで <= `P`
2. 最初の action(`DISCONNECT`)が返るまで、合計 <= **2P + D**
3. `applied(DISCONNECT, false)` が続く場合、`FORCE_OFF` が返るまで、合計 <= **3P + D + F**(最初の action から <= `F + P`)

`P` = 1000、`D` = 3000、`F` = 2000 のとき、2 と 3 は 8000 ms 以下 = `USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS`(10000)から、操作の完了の余裕 `USB_VBUS_CTRL_COMPLETION_MARGIN_MS`(2000)を引いた値。この関係(`3P + D + F + 余裕 <= 10000`)は、`usb_vbus_ctrl.c` のコンパイル時の検査にもなっている。`P` = 2000 にすると 3P + D + F = 11000 で、10 秒を超える。

テストは、物理的な外れの時刻を、最悪側の「最後に VBUS ありを読んだ時刻 `lp`」で代用して測る(物理的な外れは `lp` の後、最初の「なし」のサンプルの前)。`lp` からの時間は、実際の外れからの時間以上なので、保証側に倒れる。

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/usb_vbus_ctrl/test_usb_vbus_ctrl.c` | 既定値と init(NULL config、`disconnect_debounce_ms` と `force_off_after_ms` の切り詰めの境界)、起動時は切断、接続・切断それぞれのデバウンスの境界(`-1` / ちょうど / `+1`、0、1 ms ごとの tick)、途中の反転での数え直し(境界の 1 ms 前を含む)、`applied` の失敗の再試行(接続・切断)と、再試行中の VBUS の変化、`applied(ok)` での数えのリセット、`sample_valid = false`(VBUS なし扱い)、`enabled = false`、強制遮断(境界、最初の DISCONNECT が起点、疎なサンプルでも DISCONNECT が先、成功・失敗・VBUS 復帰・DISCONNECT 成功・逆行での待ちの解除、統計の分離、`force_off_after_ms` = 0 → 1)、`now_ms` の逆行・同時刻・64 ビット、NULL、統計、仕様の不変条件(固定のサンプル周期 6 通り × D 9 通り × F 7 通り × 位相 2 通り。上限ちょうど P = 1000、D = 3000、F = 2000 の最悪配置)、ファズ 3 本 | 69 |

ctest の全件: 作業前 68 → 作業後 69(このコンポーネントの実行ファイルが 1 つ増えた)。

ファズ:
- `test_fuzz_monotone_against_reference_model_and_invariants`(800 シード × 400 tick、`now_ms` は単調、間隔 1..P): **独立に書いた参照モデル**(内部状態を持たず、サンプルの履歴から連続の始まりと、最初の DISCONNECT の時刻を後ろ向きに探す)との一致、`applied` の結果は接続・切断・強制遮断で別々の確率(0〜100%)で失敗させる、統計の一致。モデルに依らない不変条件を毎 tick 検査する: 一度も VBUS が present にならなければ CONNECT は返らない、CONNECT は連続した有効な VBUS が `connect_debounce_ms` 以上のときだけで、その条件で必ず返る、DISCONNECT / FORCE_OFF は接続の状態で VBUS なしが `D` 以上続いたときだけで、その条件で必ず返る、**最初の action は `lp` から 2P + D 以内(8000 ms 以下)**、**FORCE_OFF は最初の DISCONNECT から F 以上 F + P 以内、`lp` から 3P + D + F 以内(8000 ms 以下)**、DISCONNECT を一度も返さずに FORCE_OFF は返らない、FORCE_OFF のあとに DISCONNECT には戻らない、最初の DISCONNECT から F 以上経てば必ず FORCE_OFF、`connects - disconnects - force_off_count` が現在の状態と一致。空振りで通っていないことも検査する(接続 2000 回超、切断 800 回超、強制遮断 150 回超、各再試行 200 / 200 / 50 回超、最初の action と FORCE_OFF の時間の検査がそれぞれ 1500 / 300 回超)。
- `test_fuzz_non_monotone_clock_against_reference_model`: `now_ms` が戻る・同時刻が混じる系列(逆行 5000 回超)で、参照モデルとの一致だけを見る(時間の不変条件は、単調な時計が前提なので対象外)。
- `test_fuzz_disabled_connects_exactly_once`: `enabled = false` では、どんな入力でも最初の `update` だけが CONNECT。

**この範囲で確かめていないこと**
- IDF のビルドは確認していない。`CMakeLists.txt` の IDF 側(`idf_component_register`)は、`video_codec_policy` と同じ形で書いたが、この環境にはない。
- 結線(GPIO の読み出し、`tud_connect` / `tud_disconnect`、FORCE_OFF の実行= `DCTL.sftdiscon` の直接書き込み・`tinyusb_driver_uninstall`・UTMI の PHY 停止、`update` の周期 `P` <= 1000 ms の確保、Kconfig `USB_DEVICE_VBUS_SENSE_GPIO`)は、このコンポーネントの範囲外で、未実装(Antigravity。Issue #89)。**`P` を呼び出し側が守ることは、このテストでは確かめられない**(`update` を呼ぶタスクが止まる、優先度の逆転など。設計 3.2「保証できない条件」)。
- 操作の完了(レジスタへの書き込みと実行待ち)の 2000 ms の余裕は、設計の見積りで、実測していない(`TODO(hw-verify)`)。
- 既定値(`connect_debounce_ms` = 500、`disconnect_debounce_ms` = 1000、`force_off_after_ms` = 1000)の妥当性は、実機で確認していない(`TODO(hw-verify)`: VBUS の立ち上がり・ノイズは分圧の回路と PC のポートによる)。
- 複数のタスクからの同時の呼び出しは、仕様どおり禁止(`update` と `applied` は同じタスク)で、テストしていない。
- `applied` の action が状態と合わない呼び出し(接続の状態で `applied(CONNECT, ...)` など)の振る舞いは、仕様としては決めていない(呼び出し側の誤り。実装は、`CONNECT` なら接続、それ以外の成功なら切断にする)。テストしていない。
- 時計の逆行が毎 tick 続く場合、切断(と強制遮断)は、決して返らない(数えが毎回やり直しになる)。`now_ms` が単調な時計であることが前提(設計は、単調でない時計を許さない)。

## 3. ミューテーションテスト

方法: `tools/test/mutate_usb_vbus_ctrl.py` が、`components/` と `test/` をスクラッチにコピーし(作業先が repo と重なる場合は、何も消さずに終了する。外部コマンドはシェルを通さずに実行する)、`usb_vbus_ctrl.c` の 1 か所を置換 -> 再ビルド -> `test_usb_vbus_ctrl` を実行、を繰り返す(ミュータントの定義は `tools/test/usb_vbus_ctrl_mutants.py`)。失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。対象は `usb_vbus_ctrl.c` の全体(`config_default`、`init`、`update`、`applied`、`is_connected`、`get_stats`)。

```
python3 tools/test/mutate_usb_vbus_ctrl.py <repo> <スクラッチ> tools/test/usb_vbus_ctrl_mutants.py <結果.json>
```

**結果**(最終の 1 回。ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数 | ビルドエラー |
|---|---|---|---|
| 118 | 118 | 0 | 0 |

### 3.1 経緯(正直な記録)

- 最初の実装(FORCE_OFF なし。83 個のミュータント)は、設計の追加(FORCE_OFF、制限値の変更)で、書き直した。**その最初の版の完全な実行結果は残していない**(途中で止めて、設計の追加に取りかかったため)。数字として残るのは、上の最終の 1 回(118 個)だけ。
- 最初の最終候補の実行(118 個)は、115 個を殺し、3 個がビルドエラー(`-Werror`)だった。生き残りは 0。ビルドエラーは、ミュータントの書き方の問題で、書き直して、その 3 個だけを再実行して殺した(`U21`: 未使用変数、`F22`: 置換が `c->config` の `>` に当たっていた、`U85`: 未使用の引数)。その後、最終の 1 回を全数、最初から実行し直した(上の表)。
- 実行中に、**テストの穴ではなく、テストの書き方の問題**を 1 つ見つけた: `test_spec_removal_bound_when_disconnect_succeeds` の `while (a == NONE)` に上限がなく、「切断が返らない」ミュータント(`U41`)で、タイムアウトまで止まらなかった。上限を付けた(殺し方は変わらず、アサーションで落ちる)。
- ファズが強いので、多くのミュータントが、単体のテストとファズの両方で殺される(表の「殺したテスト」の 2 つ目が `test_fuzz_*` のもの)。ミュータントを消して逃げたものはない。

### 3.2 同値の変更(ミュータントに数えない。`tools/test/usb_vbus_ctrl_equivalents.py`)

同じ実行器で流して、**6 個とも、全テストが通る(survived)ことを確認した**(同値であることの機械的な裏づけ)。

| ID | 変更 | 同値の理由 |
|---|---|---|
| E01 | `disconnect_debounce_ms` の切り詰めの `>` を `>=` | `==` MAX のとき、MAX を MAX に代入するだけ |
| E02 | `force_off_after_ms` の切り詰めの `>` を `>=` | 同上 |
| E03 | `force_off_after_ms == 0` を `< 1` | 符号なしの整数。同じ集合 |
| E04 | `update(c, NULL)` の合成サンプルを (`sample_valid = true`, `vbus_present = false`) | 検出は `sample_valid && vbus_present` なので、どちらかが偽なら VBUS なしで同じ |
| E05 | 同じく (`sample_valid = false`, `vbus_present = true`) | 同上 |
| E06 | 時計の逆行で、`run_start_ms = now` の代わりに `run_active = false` | その tick が「逆の向き」なら、次の行が `now` から数え始める(同じ)。そうでなければ、どのみち `run_active = false` になる |

### 3.3 結果の表

| ID | 変更 | 殺したテスト(先頭の 2 つ) |
|---|---|---|
| U01 | default: enabled true | `test_default_config`, `test_init_null_config_is_default` |
| U02 | default: connect debounce 500 -> 499 | `test_default_config` |
| U03 | default: disconnect debounce 1000 -> 999 | `test_default_config` |
| U04 | default: connect and disconnect debounce swapped | `test_default_config` |
| U05 | init: c NULL not checked | `test_usb_vbus_ctrl:crash/rc=1` |
| U06 | init: state not cleared (no memset) | `test_init_starts_disconnected_with_zero_stats_and_keeps_config`, `test_init_null_config_is_default` |
| U07 | init: config ignored (always default) | `test_init_starts_disconnected_with_zero_stats_and_keeps_config`, `test_init_clamps_disconnect_debounce_to_max` |
| U08 | init: NULL config not handled (dereferenced) | `test_usb_vbus_ctrl:crash/rc=1` |
| U09 | init: NULL config gives enabled config instead of default | `test_init_null_config_is_default` |
| U10 | init: disconnect debounce clamp removed | `test_init_clamps_disconnect_debounce_to_max`, `test_clamped_disconnect_debounce_is_what_the_state_machine_uses` |
| U11 | init: clamp limit off by one (MAX + 1) | `test_init_clamps_disconnect_debounce_to_max`, `test_clamped_disconnect_debounce_is_what_the_state_machine_uses` |
| U12 | init: clamp condition inverted (> -> <) | `test_init_starts_disconnected_with_zero_stats_and_keeps_config`, `test_init_null_config_is_default` |
| U13 | init: clamp applied to connect debounce as well | `test_init_clamps_disconnect_debounce_to_max`, `test_connect_boundary_minus1_exact_plus1` |
| U14 | init: clamp checks the connect debounce instead of the disconnect debounce | `test_init_clamps_disconnect_debounce_to_max`, `test_clamped_disconnect_debounce_is_what_the_state_machine_uses` |
| U15 | init: clamp sets the connect debounce instead of the disconnect debounce | `test_init_clamps_disconnect_debounce_to_max`, `test_clamped_disconnect_debounce_is_what_the_state_machine_uses` |
| U20 | update: c NULL not checked | `test_usb_vbus_ctrl:crash/rc=1` |
| U21 | update: input NULL not handled (dereferenced) | `test_usb_vbus_ctrl:crash/rc=1` |
| U22 | update: NULL input treated as a valid VBUS-present sample | `test_null_input_is_an_invalid_sample_and_does_not_connect`, `test_null_input_counts_as_absent_when_connected` |
| U23 | update: NULL input uses now_ms = 0 instead of the last now_ms | `test_null_input_counts_as_absent_when_connected` |
| U24 | update: enabled check inverted | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_startup_is_disconnected_and_never_connects_without_vbus` |
| U25 | update: enabled check removed (VBUS always sensed) | `test_disabled_connects_once_on_first_update_then_none`, `test_disabled_ignores_input_and_clock` |
| U26 | legacy: CONNECT returned every update (done flag not checked) | `test_disabled_connects_once_on_first_update_then_none`, `test_disabled_ignores_input_and_clock` |
| U27 | legacy: done flag never set | `test_disabled_connects_once_on_first_update_then_none`, `test_disabled_ignores_input_and_clock` |
| U28 | legacy: first update returns NONE (never connects) | `test_disabled_connects_once_on_first_update_then_none`, `test_disabled_ignores_input_and_clock` |
| U29 | legacy: first update returns DISCONNECT | `test_disabled_connects_once_on_first_update_then_none`, `test_disabled_ignores_input_and_clock` |
| U30 | clock: backward test < -> <= (same time restarts) | `test_clock_same_time_is_not_backward`, `test_null_input_counts_as_absent_when_connected` |
| U31 | clock: backward test < -> > (forward steps restart) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U32 | clock: backward step does not restart the count | `test_clock_backward_restarts_the_disconnect_count_from_the_new_now`, `test_clock_backward_by_one_ms_restarts` |
| U33 | clock: backward step restarts from the old last_now_ms | `test_clock_backward_restarts_the_disconnect_count_from_the_new_now`, `test_clock_backward_by_one_ms_restarts` |
| U34 | clock: last_now_ms never updated | `test_clock_backward_restarts_the_disconnect_count_from_the_new_now`, `test_clock_backward_by_one_ms_restarts` |
| U35 | clock: last_now_ms only ever increases (max) | `test_clock_backward_restarts_the_connect_count`, `test_clock_backward_while_not_counting_has_no_effect` |
| U36 | clock: backward step restarts from 0 | `test_clock_backward_restarts_the_disconnect_count_from_the_new_now`, `test_clock_backward_by_one_ms_restarts` |
| U40 | sample: sample_valid ignored | `test_invalid_sample_never_connects_even_if_vbus_present_is_true`, `test_invalid_sample_breaks_a_connect_run` |
| U41 | sample: sample_valid \|\| vbus_present | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_startup_is_disconnected_and_never_connects_without_vbus` |
| U42 | sample: vbus_present ignored | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_startup_is_disconnected_and_never_connects_without_vbus` |
| U43 | sample: invalid sample counted as VBUS present (valid false -> present) | `test_invalid_sample_never_connects_even_if_vbus_present_is_true`, `test_invalid_sample_breaks_a_connect_run` |
| U44 | sample: vbus_present inverted | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_startup_is_disconnected_and_never_connects_without_vbus` |
| U50 | opposite: connected branch counts VBUS present | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_applied_connect_ok_connects_and_counts` |
| U51 | opposite: disconnected branch counts VBUS absent | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_startup_is_disconnected_and_never_connects_without_vbus` |
| U52 | opposite: both branches swapped | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_startup_is_disconnected_and_never_connects_without_vbus` |
| U53 | opposite: the non-counting sample does not drop the count (run_active kept) | `test_connect_run_restarts_after_a_flip`, `test_connect_flip_one_ms_before_the_boundary_restarts` |
| U54 | opposite: the non-counting sample falls through to the debounce check | `test_connect_debounce_zero_connects_on_first_detected_tick`, `test_connect_run_restarts_after_a_flip` |
| U55 | run: start restarted every tick (run_active test always true) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U56 | run: run_active never set (start restarted every tick) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U57 | run: start time not recorded | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U60 | elapsed: truncated to 32 bits | `test_large_now_values_do_not_overflow` |
| U61 | elapsed: reversed subtraction | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U62 | disconnect: >= -> > | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U63 | connect: >= -> > | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U64 | disconnect: elapsed + 1 >= (one ms early) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U65 | connect: elapsed + 1 >= (one ms early) | `test_connect_boundary_minus1_exact_plus1`, `test_connect_run_restarts_after_a_flip` |
| U66 | disconnect: uses the connect debounce | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U67 | connect: uses the disconnect debounce | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U68 | disconnect: never returned | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U69 | connect: never returned | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U70 | disconnect branch returns CONNECT | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U71 | connect branch returns DISCONNECT | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U72 | debounce branch selected by !connected | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_connect_boundary_minus1_exact_plus1` |
| U73 | disconnect decision: elapsed test removed (disconnect on first absent tick) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U74 | connect decision: elapsed test removed (connect on first present tick) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_startup_with_vbus_present_waits_for_debounce` |
| F01 | force-off: never returned (DISCONNECT instead) | `test_force_off_boundary_minus1_exact_plus1`, `test_force_off_wait_starts_at_the_first_disconnect` |
| F02 | force-off: returned as NONE | `test_force_off_boundary_minus1_exact_plus1`, `test_force_off_wait_starts_at_the_first_disconnect` |
| F03 | force-off: >= -> > (one ms late) | `test_force_off_boundary_minus1_exact_plus1`, `test_force_off_wait_starts_at_the_first_disconnect` |
| F04 | force-off: now - first + 1 >= (one ms early) | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_force_off_boundary_minus1_exact_plus1` |
| F05 | force-off: waits the disconnect debounce instead of force_off_after_ms | `test_force_off_boundary_minus1_exact_plus1`, `test_force_off_wait_starts_at_the_first_disconnect` |
| F06 | force-off: waits the connect debounce instead of force_off_after_ms | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_force_off_boundary_minus1_exact_plus1` |
| F07 | force-off: does not require a DISCONNECT first | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| F08 | force-off: reversed subtraction (first - now) | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_null_input_counts_as_absent_when_connected` |
| F09 | force-off: measured from the start of the absence (run_start) instead of the first DISCONNECT | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_null_input_counts_as_absent_when_connected` |
| F10 | force-off: first-DISCONNECT time refreshed on every DISCONNECT | `test_force_off_boundary_minus1_exact_plus1`, `test_force_off_wait_starts_at_the_first_disconnect` |
| F11 | force-off: first-DISCONNECT time not recorded | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_null_input_counts_as_absent_when_connected` |
| F12 | force-off: disc_active never set | `test_force_off_boundary_minus1_exact_plus1`, `test_force_off_wait_starts_at_the_first_disconnect` |
| F13 | force-off: sticky FORCE_OFF lost (next tick back to DISCONNECT) | `test_force_off_boundary_minus1_exact_plus1`, `test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure` |
| F14 | force-off: recorded time is the run start, not the DISCONNECT tick | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_null_input_counts_as_absent_when_connected` |
| F15 | force-off: wait not restarted when VBUS returns (disc_active kept) | `test_disconnect_retry_stops_when_vbus_returns_and_debounce_restarts`, `test_vbus_return_restarts_the_force_off_wait` |
| F16 | force-off: wait not restarted on a backward clock | `test_clock_backward_restarts_the_force_off_wait`, `test_fuzz_non_monotone_clock_against_reference_model` |
| F17 | force-off: wait not cleared by a successful applied | `test_fuzz_monotone_against_reference_model_and_invariants`, `test_fuzz_non_monotone_clock_against_reference_model` |
| F20 | init: force_off_after_ms upper clamp removed | `test_init_clamps_force_off_after_to_1_to_max`, `test_spec_removal_bound_for_fixed_sample_periods` |
| F21 | init: force_off_after_ms clamp limit off by one (MAX + 1) | `test_init_clamps_force_off_after_to_1_to_max`, `test_spec_removal_bound_for_fixed_sample_periods` |
| F22 | init: force_off_after_ms clamp condition inverted (> -> <) | `test_init_clamps_force_off_after_to_1_to_max`, `test_force_off_boundary_minus1_exact_plus1` |
| F23 | init: force_off_after_ms clamped to the disconnect debounce limit | `test_init_clamps_force_off_after_to_1_to_max`, `test_spec_removal_bound_for_fixed_sample_periods` |
| F24 | init: force_off_after_ms zero not raised to 1 | `test_init_clamps_force_off_after_to_1_to_max`, `test_force_off_after_zero_is_one_ms` |
| F25 | init: force_off_after_ms zero raised to 2 | `test_init_clamps_force_off_after_to_1_to_max`, `test_force_off_after_zero_is_one_ms` |
| F26 | init: force_off_after_ms zero replaced by the default instead of 1 | `test_init_clamps_force_off_after_to_1_to_max`, `test_force_off_after_zero_is_one_ms` |
| F27 | init: force_off_after_ms lower clamp applied to every nonzero value (== -> !=) | `test_init_clamps_force_off_after_to_1_to_max`, `test_disconnect_failure_retries_every_tick_without_redebounce` |
| F28 | default: force_off_after_ms 1000 -> 999 | `test_default_config`, `test_disconnect_failure_retries_every_tick_without_redebounce` |
| F29 | default: force_off_after_ms not set (0) | `test_default_config`, `test_disconnect_failure_retries_every_tick_without_redebounce` |
| U80 | applied: c NULL not checked | `test_usb_vbus_ctrl:crash/rc=1` |
| U81 | applied: NONE not ignored | `test_applied_none_is_ignored` |
| U82 | applied: && -> \|\| (everything ignored) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_applied_connect_ok_connects_and_counts` |
| U83 | applied: failure not counted | `test_connect_failure_retries_every_tick_without_redebounce`, `test_disconnect_failure_retries_every_tick_without_redebounce` |
| U84 | applied: failure restarts the debounce | `test_connect_failure_retries_every_tick_without_redebounce`, `test_disconnect_failure_retries_every_tick_without_redebounce` |
| U85 | applied: failure treated as success | `test_connect_failure_retries_every_tick_without_redebounce`, `test_connect_retry_stops_when_vbus_is_lost_and_debounce_restarts` |
| U86 | applied: failure also changes the state | `test_connect_failure_retries_every_tick_without_redebounce`, `test_connect_retry_stops_when_vbus_is_lost_and_debounce_restarts` |
| U87 | applied: ok inverted (success treated as failure) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_applied_connect_ok_connects_and_counts` |
| U88 | applied: state set from the wrong comparison (== -> !=) | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_applied_connect_ok_connects_and_counts` |
| U89 | applied: success does not clear the count | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U90 | applied: success clears the count only after CONNECT | `test_after_disconnect_the_present_run_is_counted_from_its_own_start`, `test_stats_count_connects_disconnects_failures` |
| U91 | applied: success clears the count only after DISCONNECT | `test_clamped_disconnect_debounce_is_what_the_state_machine_uses`, `test_disconnect_boundary_minus1_exact_plus1` |
| U92 | applied: connects and disconnects counters swapped | `test_applied_connect_ok_connects_and_counts`, `test_connect_failure_retries_every_tick_without_redebounce` |
| F40 | applied: FORCE_OFF not accepted (ignored) | `test_force_off_ok_disconnects_and_counts_separately`, `test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure` |
| F41 | applied: FORCE_OFF success counted as a disconnect | `test_force_off_ok_disconnects_and_counts_separately`, `test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure` |
| F42 | applied: FORCE_OFF success not counted | `test_force_off_ok_disconnects_and_counts_separately`, `test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure` |
| F43 | applied: FORCE_OFF success counted twice | `test_force_off_ok_disconnects_and_counts_separately`, `test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure` |
| F44 | applied: DISCONNECT success counted as a force-off (else-if always true) | `test_force_off_ok_disconnects_and_counts_separately`, `test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure` |
| F45 | applied: disconnect failures not counted | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_force_off_ok_disconnects_and_counts_separately` |
| F46 | applied: every failure counted as a disconnect failure | `test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure`, `test_fuzz_monotone_against_reference_model_and_invariants` |
| F47 | applied: only FORCE_OFF failures counted as disconnect failures | `test_disconnect_failure_retries_every_tick_without_redebounce`, `test_force_off_ok_disconnects_and_counts_separately` |
| U93 | applied: connects counted twice | `test_applied_connect_ok_connects_and_counts`, `test_connect_failure_retries_every_tick_without_redebounce` |
| U94 | applied: disconnects not counted | `test_applied_disconnect_ok_disconnects_and_counts`, `test_disconnect_failure_retries_every_tick_without_redebounce` |
| U95 | applied: connects not counted | `test_applied_connect_ok_connects_and_counts`, `test_connect_failure_retries_every_tick_without_redebounce` |
| U100 | is_connected: c NULL not checked | `test_usb_vbus_ctrl:crash/rc=1` |
| U101 | is_connected: NULL gives true | `test_null_arguments_do_not_crash_and_are_safe` |
| U102 | is_connected: value inverted | `test_init_starts_disconnected_with_zero_stats_and_keeps_config`, `test_init_null_config_is_default` |
| U103 | get_stats: out NULL not checked | `test_usb_vbus_ctrl:crash/rc=1` |
| U104 | get_stats: c NULL not checked | `test_usb_vbus_ctrl:crash/rc=1` |
| U105 | get_stats: c NULL leaves out untouched | `test_null_arguments_do_not_crash_and_are_safe` |
| U106 | get_stats: stats not copied | `test_init_starts_disconnected_with_zero_stats_and_keeps_config`, `test_applied_connect_ok_connects_and_counts` |
