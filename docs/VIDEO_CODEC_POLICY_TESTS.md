# video_codec_policy のテストとミューテーションテストの結果

作成: 2026-11-16(P7、`docs/VIDEO_TRANSPORT.md` 9.5 の契約の実装)。`components/video_codec_policy`(全クライアントに配るコーデック MJPEG / H.264 を決める、状態を持つ純 C、IDF 非依存)のホスト単体テストの構成と、`video_codec_policy.c` に対するミューテーションテストの結果。契約は `docs/VIDEO_TRANSPORT.md` 9.4・9.5・9.10(方針 1)と `docs/INTERFACES.md` 1.4。

## 1. 規則と、契約から判断して固定した点

H.264 になるのは、**許可 / 能力 / 申告 / 入力 / 安定**の 5 条件がすべて満たされたとき(9.5)。それ以外は MJPEG。

| 点 | 決めたこと | 理由 |
|---|---|---|
| 待ちの境界 | 経過 `>= upgrade_dwell_ms` で昇格(ちょうどで昇格) | 契約は「`VIDEO_CODEC_UPGRADE_DWELL_MS` 続いたとき」。ちょうど続いたときは満たす |
| 待ちの始まり | 条件(許可・能力・申告・入力)が**最初に満たされた tick** の `now_ms`。1 tick でも外れたら捨てる | 契約は「続いたとき」。連続でなければ続いていない |
| 降格 | 許可・能力・入力のどれかが外れたら、**接続数に関係なく**その tick で MJPEG。接続が 1 つ以上で、全員が申告していないときも、その tick で MJPEG | 契約は「即時」。許可が外れた・ソースが変わったのに H.264 のまま、は、おかしい |
| 接続が 0 個 | コーデックは**変えない**(H.264 は H.264、MJPEG は MJPEG)。ただし、上のハードな条件(許可・能力・入力)が外れたら MJPEG | 契約は「接続が 0 個になったら、コーデックは変えない(次の接続で、条件を評価し直す)」。許可などは接続と独立 |
| 接続が 0 個のときの待ち | **捨てる**(次の接続で、`upgrade_dwell_ms` を最初から数える) | 「申告」は client_count >= 1 を要求するので、0 個の間は条件が満たされない = 続いていない。「止めて持ち越す」と、切断をまたいで、短い接続の合計で昇格してしまう |
| 不整合(`h264_client_count > client_count`) | 「全員が申告」が偽。H.264 なら即 MJPEG(`client_count == 0` で `h264_client_count > 0` のときも)、MJPEG なら昇格しない | 安全側(MJPEG)に倒す。契約に記述なし(判断) |
| `input == NULL` | 全条件が外れた扱い(H.264 なら MJPEG、待ちは捨てる)。時計は進めない | 安全側。契約に記述なし(判断) |
| `now_ms` の逆行 | 待ちを、その `now_ms` からやり直す(昇格が遅れる側)。降格には影響しない。`>` ではなく `<` で判定し、同時刻は逆行ではない | 時計を信用できない。不正な昇格をしない。契約に記述なし(判断) |
| オーバーフロー | `now_ms` は 64 ビットで、引き算(経過)だけ。32 ビットに切り詰めない | |

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/video_codec_policy/test_video_codec_policy.c` | 既定値と init(NULL config)、許可の否定、5 条件+接続 0 個+能力のビットマスクの単独の否定(11 通りの表)、待ちの境界(`dwell - 1` / ちょうど / `+ 1`、dwell = 0 / 1、始まりの時刻)、待ちの途中の外れ(11 通りの表、境界の tick での外れ、2 台目の古いビューアの出入り)、即時の降格(ハードな条件の表、0 個の接続でも、1 台が申告なし、2 台中 1 台が申告なし)、降格のあとの待ち、接続 0 個(H.264 / MJPEG を保つ、待ちを捨てる)、解像度の上限の境界(1920x1080 可、1921x1080 / 1920x1081 不可、設定に追従、0 の上限)、時計の逆行(1 ms、0 まで、64 ビットの上限、同時刻)、不整合な入力と NULL、許可の切替、統計、ファズ 2 本 | 53 |

ctest の全件: 作業前 66 → 作業後 67(このコンポーネントの実行ファイルが 1 つ増えた)。

ファズ(`test_fuzz_against_reference_model_and_invariants`): 600 シード × 400 tick。**独立に書いた参照モデル**(連続の始まりを持つ、実装と別の式)の出力と一致すること、および、モデルに依らない不変条件を毎 tick 検査する: 許可が偽なら常に MJPEG、ハードな条件が外れたら MJPEG(即時)、前が H.264 で申告なしのクライアントがいれば MJPEG、前が H.264 で 0 個の接続ならそのまま、昇格したときは全条件が成り立つこと、統計の整合(`upgrades - downgrades` が現在のコーデックに一致)。昇格と降格が、それぞれ 300 回を超えて実際に起きること(空振りで通っていないこと)も検査する。もう 1 本(`test_fuzz_never_allowed_is_always_mjpeg`)は、許可が偽の設定で、ランダムな時刻・接続数でも常に MJPEG であること。

**この範囲で確かめていないこと**
- IDF のビルドは確認していない。`CMakeLists.txt` の IDF 側(`idf_component_register`)は、`video_status` と同じ形で書いたが、この環境にはない。
- 結線(`web_server.c` が `video_codec_policy_update` を呼び、`video_source_t.set_codec()` に伝えること、`accept=h264` の解析、クライアント数の集計)は、このコンポーネントの範囲外で、未実装。
- 実機での既定値の妥当性(`VIDEO_CODEC_POLICY_DEFAULT_DWELL_MS` = 5000 など)は未確認(`TODO(hw-verify)`)。
- `video_codec_t`(`frame_hub.h`)は、このブランチにまだなく、policy は同じ値のローカルの enum(`VIDEO_CODEC_POLICY_MJPEG = 0`、`VIDEO_CODEC_POLICY_H264 = 1`)を持つ。値の一致は、結線のときに確認する。
- 複数のタスクからの同時の呼び出しは、仕様どおり禁止(1 つのタスクだけが tick を回す)で、テストしていない。

## 3. ミューテーションテスト

方法: `tools/test/mutate_video_codec_policy.py` が、`components/` と `test/` をスクラッチにコピーし(作業先が repo と重なる場合は、何も消さずに終了する。外部コマンドはシェルを通さずに実行する)、`video_codec_policy.c` の 1 か所を置換 -> 再ビルド -> `test_video_codec_policy` を実行、を繰り返す(ミュータントの定義は `tools/test/video_codec_policy_mutants.py`)。失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。対象は `video_codec_policy.c` の全体。

```
python3 tools/test/mutate_video_codec_policy.py <repo> <スクラッチ> tools/test/video_codec_policy_mutants.py <結果.json>
```

**結果**(最終の 1 回。ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数 | ビルドエラー |
|---|---|---|---|
| 98 | 98 | 0 | 0 |

### 3.1 経緯(正直な記録)

最初の実行(100 個)は、94 個を殺し、3 個が生き残り、3 個がビルドエラー(`-Werror`)だった。**ミュータントを消して逃げず**、1 個ずつ原因を調べた。

| 最初の結果 | 原因 | 対処 |
|---|---|---|
| 生き残り: 「`source_conditions_ok` の末尾の `return true;` を `return true \|\| ...;` にする」 | 元から `true` を返す所で、変更になっていなかった(無意味なミュータント) | ミュータントを、意味のあるものに置き換えた(件数は後述) |
| 生き残り: 「不整合の検査(`h264_client_count <= client_count`)を消す」 | **実装の冗長だった**。不整合の入力は、下流の `h264_client_count == client_count`(昇格)と `!=`(降格)の比較で、すでに安全側に倒れる。検査を消しても、振る舞いが変わらない | 冗長な検査をコードから削り、不整合は等価比較で扱う設計にした(コメントに理由)。不整合のテスト(`test_inconsistent_*`)は、そのまま通る |
| 生き残り: 「経過を 32 ビットに切り詰める」 | **テストの穴**。経過が 2^32 ms を超える tick の間隔のテストがなかった | `test_large_now_values_do_not_overflow` に、経過が 2^32 ms ちょうど、および 2^32 + dwell - 1 のときの 2 ケースを足した |
| ビルドエラー 3 個 | `-Werror` の未使用変数・`>= 0` の常に真・未使用の引数(ミュータントの書き方の問題) | 書き直した |

そのほか、コードを書いた時点で、**同値になるミュータントがあった冗長**を、先に削った: `have_last`(`last_now_ms` の初期値 0 で足りる)、時計の逆行の判定の `&& dwell_active`(待ちの最中でなければ、`dwell_start_ms` は使われず、次の開始で上書きされる)、H.264 側の降格の `client_count != 0 &&`(整合した入力では、`client_count == 0` なら `h264 == client` なので不要)、`init` の `codec = MJPEG`(`memset` が 0 = MJPEG にする)、昇格のときの `dwell_active = false`(H.264 側の先頭で消す)。

その後、ミュータントを置き換え・追加し直して(98 個)、**98 個全部を最初から実行し直した**のが、上の結果。生き残りは 0。

### 3.2 同値のミュータント

**0 個**。同値になる変更は、ミュータントに入れず、先にコード側の冗長を削った(3.1)。残した判断: H.264 側の降格の `h264_client_count != client_count` を `<` にする変更は、整合した入力(`h264 <= client`)では区別できないが、**不整合な入力(`h264 > client`)では区別できる**(`!=` は降格、`<` は H.264 のまま)ので、同値ではない。ミュータント(P63)に入れ、`test_inconsistent_counts_downgrade_h264_even_with_zero_clients` が殺す。

### 3.3 殺したミュータントの一覧(98 個)

「殺したテスト」は、失敗したテストのうち先頭の最大 2 つ。`crash` は、NULL の参照などで ASan が落とした(テストの名前が取れない)もの。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| P01 | default: h264_allowed true | `test_config_default_values`, `test_init_with_null_config_is_default_and_never_h264` |
| P02 | default: dwell 5000 -> 4999 | `test_config_default_values`, `test_init_with_null_config_is_default_and_never_h264` |
| P03 | default: max_width not set from the default constant (0) | `test_config_default_values`, `test_capability_mask_only_the_h264_bit_counts` |
| P04 | default: max_height not set from the default constant (0) | `test_config_default_values`, `test_capability_mask_only_the_h264_bit_counts` |
| P05 | default: max_width and max_height swapped | `test_config_default_values`, `test_capability_mask_only_the_h264_bit_counts` |
| P06 | init: policy NULL not checked | `test_video_codec_policy:crash/rc=1` |
| P07 | init: state not cleared (no memset) | `test_init_is_mjpeg_with_zero_stats_and_keeps_config`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P08 | init: config ignored (always default) | `test_init_is_mjpeg_with_zero_stats_and_keeps_config`, `test_capability_mask_only_the_h264_bit_counts` |
| P09 | init: NULL config not handled (dereferenced) | `test_video_codec_policy:crash/rc=1` |
| P10 | init: NULL config gives allowed config instead of default | `test_init_with_null_config_is_default_and_never_h264` |
| P11 | set_allowed: policy NULL not checked | `test_video_codec_policy:crash/rc=1` |
| P12 | set_allowed: value inverted | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P13 | set_allowed: value ignored | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P14 | allowed check removed | `test_init_with_null_config_is_default_and_never_h264`, `test_not_allowed_is_always_mjpeg_for_any_input` |
| P15 | allowed check inverted | `test_init_with_null_config_is_default_and_never_h264`, `test_not_allowed_is_always_mjpeg_for_any_input` |
| P16 | capability check removed | `test_each_condition_alone_blocks_the_upgrade`, `test_capability_mask_only_the_h264_bit_counts` |
| P17 | capability: any non-zero mask counts (bit not tested) | `test_each_condition_alone_blocks_the_upgrade`, `test_capability_mask_only_the_h264_bit_counts` |
| P18 | capability: tests the MJPEG bit (bit 0) instead of the H.264 bit | `test_each_condition_alone_blocks_the_upgrade`, `test_capability_mask_only_the_h264_bit_counts` |
| P19 | capability: requires the mask to equal the H.264 bit exactly | `test_capability_mask_only_the_h264_bit_counts`, `test_fuzz_against_reference_model_and_invariants` |
| P20 | capability: inverted | `test_each_condition_alone_blocks_the_upgrade`, `test_capability_mask_only_the_h264_bit_counts` |
| P21 | locked check removed | `test_each_condition_alone_blocks_the_upgrade`, `test_dwell_counts_from_the_first_satisfied_tick_not_from_init` |
| P22 | locked check inverted | `test_each_condition_alone_blocks_the_upgrade`, `test_capability_mask_only_the_h264_bit_counts` |
| P23 | zero size: \|\| -> && (needs both 0) | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P24 | zero size: width not checked | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P25 | zero size: height not checked | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P26 | zero size: check removed | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P27 | zero size: boundary (width 1 treated as zero) | `test_resolution_limit_boundaries_with_defaults` |
| P28 | zero size: boundary (height 1 treated as zero) | `test_resolution_limit_boundaries_with_defaults` |
| P29 | limit: \|\| -> && (needs both over) | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P30 | limit: width not checked | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P31 | limit: height not checked | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P32 | limit: width boundary (>= instead of >) | `test_capability_mask_only_the_h264_bit_counts`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P33 | limit: height boundary (>= instead of >) | `test_capability_mask_only_the_h264_bit_counts`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P34 | limit: width boundary (off by one, allows max + 1) | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P35 | limit: height boundary (off by one, allows max + 1) | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P36 | limit: width compared against max_height | `test_capability_mask_only_the_h264_bit_counts`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P37 | limit: height compared against max_width | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P38 | limit: check removed | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P39 | limit: hard-coded 1920x1080 instead of the config | `test_resolution_limit_follows_the_config`, `test_zero_limit_allows_no_resolution` |
| P40 | update: policy NULL not checked | `test_video_codec_policy:crash/rc=1` |
| P41 | update: NULL input not checked in src_ok (dereferenced) | `test_video_codec_policy:crash/rc=1` |
| P42 | update: src_ok ignores the source conditions (only input != NULL) | `test_init_with_null_config_is_default_and_never_h264`, `test_not_allowed_is_always_mjpeg_for_any_input` |
| P43 | update: src_ok && -> \|\| (a non-NULL input is always ok) | `test_init_with_null_config_is_default_and_never_h264`, `test_not_allowed_is_always_mjpeg_for_any_input` |
| P44 | update: src_ok inverted for non-NULL input | `test_init_with_null_config_is_default_and_never_h264`, `test_not_allowed_is_always_mjpeg_for_any_input` |
| P45 | clock: regression not handled | `test_clock_regression_restarts_the_dwell`, `test_clock_regression_below_the_dwell_start_does_not_wrap_around` |
| P46 | clock: regression test is <= (equal time resets the dwell) | `test_equal_now_ms_is_not_a_regression`, `test_fuzz_against_reference_model_and_invariants` |
| P47 | clock: regression test is > (inverted) | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P48 | clock: last_now_ms never updated | `test_clock_regression_restarts_the_dwell`, `test_clock_regression_below_the_dwell_start_does_not_wrap_around` |
| P49 | clock: regression does not reset the dwell start (sets the last time only) | `test_clock_regression_restarts_the_dwell`, `test_clock_regression_below_the_dwell_start_does_not_wrap_around` |
| P50 | clock: regression resets the dwell start to 0 | `test_clock_regression_restarts_the_dwell`, `test_clock_regression_below_the_dwell_start_does_not_wrap_around` |
| P51 | clock: input NULL not checked (dereferenced) | `test_video_codec_policy:crash/rc=1` |
| P52 | h264: dwell flag not cleared while in H.264 (stale dwell after a downgrade) | `test_one_of_two_clients_undeclared_downgrades_at_once`, `test_after_a_downgrade_the_dwell_starts_over` |
| P53 | h264: hard downgrade removed (conditions never downgrade) | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_hard_conditions_downgrade_even_with_zero_clients` |
| P54 | h264: hard downgrade inverted | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_hard_conditions_downgrade_even_with_zero_clients` |
| P55 | h264: hard downgrade does not count the stat | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_after_a_downgrade_the_dwell_starts_over` |
| P56 | h264: hard downgrade only when there are clients (zero clients keep H.264 even if the conditions fail) | `test_hard_conditions_downgrade_even_with_zero_clients` |
| P57 | h264: hard downgrade does not change the codec (only counts) | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_hard_conditions_downgrade_even_with_zero_clients` |
| P58 | h264: undeclared client downgrade removed | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_undeclared_client_downgrades_at_once` |
| P59 | h264: undeclared client downgrade inverted (downgrades when everyone declared) | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_undeclared_client_downgrades_at_once` |
| P60 | h264: undeclared client downgrade only when no client declared (h264 == 0) | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_one_of_two_clients_undeclared_downgrades_at_once` |
| P61 | h264: undeclared client downgrade only when at least 2 clients | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_undeclared_client_downgrades_at_once` |
| P62 | h264: zero clients downgrade (codec changed when the last client leaves) | `test_zero_clients_keeps_h264`, `test_h264_after_zero_clients_is_judged_again_by_the_next_connection` |
| P63 | h264: undeclared client check is `<` (an inconsistent h264 > client count keeps H.264) | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_inconsistent_counts_downgrade_h264_even_with_zero_clients` |
| P64 | h264: undeclared client downgrade does not count the stat | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_fuzz_against_reference_model_and_invariants` |
| P65 | h264: undeclared client downgrade counts twice | `test_every_hard_condition_downgrades_at_once_from_h264`, `test_fuzz_against_reference_model_and_invariants` |
| P66 | h264: always returns MJPEG on this branch (never stays) | `test_h264_stays_while_all_conditions_hold`, `test_h264_stays_with_a_second_declared_client_joining` |
| P67 | mjpeg: source conditions not part of declared | `test_init_with_null_config_is_default_and_never_h264`, `test_not_allowed_is_always_mjpeg_for_any_input` |
| P68 | mjpeg: declared needs no client (zero clients count as all declared) | `test_zero_clients_blocks_the_upgrade`, `test_zero_clients_keeps_mjpeg` |
| P69 | mjpeg: declared needs 2 clients (> 1) | `test_capability_mask_only_the_h264_bit_counts`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P70 | mjpeg: declared ignores the H.264 count (any client count >= 1) | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P71 | mjpeg: declared needs only one declaring client (>= 1) | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P72 | mjpeg: declared needs all but one declaring | `test_each_condition_alone_blocks_the_upgrade`, `test_any_single_break_restarts_the_dwell` |
| P73 | mjpeg: declared && -> \|\| (clients check) | `test_each_condition_alone_blocks_the_upgrade`, `test_zero_clients_blocks_the_upgrade` |
| P74 | mjpeg: not-declared does not discard the dwell | `test_any_single_break_restarts_the_dwell`, `test_break_at_the_very_tick_the_dwell_would_complete_prevents_the_upgrade` |
| P75 | mjpeg: not-declared returns H.264 | `test_init_with_null_config_is_default_and_never_h264`, `test_not_allowed_is_always_mjpeg_for_any_input` |
| P76 | mjpeg: dwell start recorded every tick (never accumulates) | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P77 | mjpeg: dwell flag not set (start recorded every tick) | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P78 | mjpeg: dwell start not recorded (stays 0) | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P79 | mjpeg: dwell start recorded one ms late | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P80 | mjpeg: dwell boundary > instead of >= | `test_capability_mask_only_the_h264_bit_counts`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P81 | mjpeg: dwell boundary off by one early (dwell - 1) | `test_dwell_boundary_minus_one_exact_plus_one`, `test_dwell_one_boundary` |
| P82 | mjpeg: dwell comparison <= (inverted) | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P83 | mjpeg: dwell compares the absolute time (not the elapsed time) | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P84 | mjpeg: dwell is skipped (upgrade on the first satisfied tick) | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P85 | mjpeg: dwell never completes | `test_capability_mask_only_the_h264_bit_counts`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P86 | mjpeg: dwell elapsed computed as a sum | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_dwell_boundary_minus_one_exact_plus_one` |
| P87 | mjpeg: dwell elapsed truncated to 32 bits | `test_large_now_values_do_not_overflow` |
| P88 | mjpeg: dwell configured value ignored (hard-coded 5000) | `test_capability_mask_only_the_h264_bit_counts`, `test_dwell_zero_upgrades_on_the_first_satisfied_tick` |
| P89 | mjpeg: upgrade does not change the codec | `test_capability_mask_only_the_h264_bit_counts`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P90 | mjpeg: upgrade does not count the stat | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_every_hard_condition_downgrades_at_once_from_h264` |
| P91 | mjpeg: upgrade counts twice | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_every_hard_condition_downgrades_at_once_from_h264` |
| P92 | get_codec: always MJPEG | `test_allowed_switched_off_downgrades_even_with_zero_clients`, `test_fuzz_against_reference_model_and_invariants` |
| P93 | get_codec: NULL policy not checked | `test_video_codec_policy:crash/rc=1` |
| P94 | get_stats: out NULL not checked | `test_video_codec_policy:crash/rc=1` |
| P95 | get_stats: NULL policy not checked | `test_video_codec_policy:crash/rc=1` |
| P96 | get_stats: NULL policy leaves out untouched | `test_null_arguments_do_not_crash_and_are_safe` |
| P97 | get_stats: stats not copied | `test_init_is_mjpeg_with_zero_stats_and_keeps_config`, `test_upgrade_waits_exactly_dwell_ms_ticking_every_second` |
| P98 | get_stats: upgrades and downgrades swapped | `test_upgrade_waits_exactly_dwell_ms_ticking_every_second`, `test_h264_stays_while_all_conditions_hold` |
