# 映像の状態の通知（C-6b）のテストとミューテーションテストの結果

作成: 2026-11-15（Issue #86、C-6b の純 C の部分）。`video_status_format_json`（`components/video_status`）と、`ws_video_core` の状態のテキストフレームによる通知のホスト単体テストの構成と、ミューテーションテストの結果。契約は `docs/VIDEO_TRANSPORT.md` §8 と `docs/WEB_API.md` §5（**契約。これが正**）。**IDF 側（`web_server.c`）とビューア（`app.js`）は、この範囲に含まない**（別の担当）。

## 1. 実装の要点（契約からの具体化と、判断した点）

| 項目 | 実装 |
|---|---|
| `video_status_format_json(st, out, cap)` | `{"t":"video","state":"locked","w":1280,"h":720}`（キーの順序と空白なしの書式は固定）。範囲外の `state` は `unknown`、`LOCKED` で `w` または `h` が 0 なら `unknown`、`locked` 以外は `w` = `h` = 0（`video_status_get` と同じ `normalize` で補正する）。戻り値は文字数。`NULL` とバッファ不足（cap < 文字数 + 1）は -1 で、`out[0] = '\0'`（`out` が `NULL` でなく cap > 0 のとき。部分的な文字列を残さない）。最長 60 バイト（10 桁ずつ）。`VIDEO_STATUS_JSON_MAX` = 128（NUL を含む） |
| 設定（`ws_video_core_config_t`） | `status_get`（`NULL` なら通知なし = 従来と全く同じ）、`io_begin_send_text`、`status_min_interval_ms`（0 = 間隔なし。`TODO(hw-verify)`: 既定の 200 は実機で確認） |
| 初期化の検査（契約にない追加） | `status_get != NULL` で `io_begin_send_text == NULL` なら `ws_video_core_init` は -1（通知が送れない設定を、起動時に弾く） |
| クライアントごとの状態 | `status_sent`、`status_last`（最後に送った state / w / h）、`status_last_ms`（最後に送り始めた `tick` の時刻）、`sending_text`、`text_len`、`text_buf[128]` |
| いつ送るか（`tick`） | クライアントが ACTIVE・`close_requested` でない・**送信中でない**（`ws_clients_try_begin_send` に成功）とき、(1) 接続直後（`status_sent == false`）なら必ず、(2) そうでなければ、現在の状態が最後に送ったものと違い、かつ `status_min_interval_ms` が経っていれば、テキストを `io_begin_send_text` で送る |
| フレームとの順序 | 同じ `tick` では状態の通知が先。**通知を始めたら、そのクライアントのフレームは、次の `tick` 以降**（テキストの送信が終わる = `on_send_done` まで `is_sending` が続くため。`hub_get_latest` も呼ばない） |
| 失敗 | `io_begin_send_text` が 0 以外 -> `ws_clients_end_send` して、そのクライアントを閉じる（フレームの失敗と同じ）。`status_sent` は立てない |
| `on_send_done` | `sending_text[i]` を戻す。解放するフレームはない（`current_frames[i]` は `NULL`）。`ok == false` は、テキストでもフレームでも閉じる |
| 期限 | テキストにも適用（`size` = `text_len`）。`on_send_begin`、`queue_timeout_ms`、`send_timeout_ms`、互換モード（`queue_timeout_ms == 0`）も、フレームと同じ |
| 最短間隔 | `now - status_last_ms >= status_min_interval_ms` で送る（ちょうど経ったときから）。間隔の間の変化は、経った後の**最新の状態を 1 回だけ**（途中の状態は送らない。最後に送ったものに戻れば、何も送らない）。**接続直後の最初の 1 回には適用しない**。`status_last_ms` は、**送り始めた `tick` の時刻**（完了の時刻ではない） |
| 時刻の巻き戻り | `status_sent` のクライアントが `tick` で見られるとき、`now_ms < status_last_ms` なら `status_last_ms = now_ms`（既存の `queued_ms` / `send_started_ms` の流儀） |
| `status_get` の呼び方 | 1 回の `tick` で、**最初に必要になったとき 1 回だけ**読み、全クライアントで同じ値（スナップショット）を使う。送れるクライアントがいない `tick` では呼ばない |
| テキストのバッファ | **クライアントごとの `text_buf[128]`（`ws_video_core_t` の中）。`io_begin_send_text` に渡す `data` は、そのクライアントの `on_send_done` まで有効で、変わらない**（IDF 側が、非同期の送信で、そのまま参照してよい） |
| 値の比較 | 生の `(state, w, h)` で比べる。`status_get` が `video_status_get`（検査済み）なら、契約どおり「同じ内容は繰り返さない」。検査していない値（例: `NO_SIGNAL` で w が 100 -> 200）を渡すと、同じ JSON が 2 回送られうる（落ちはしない。JSON は `format_json` が直す） |

**変えなかったもの**: 既存のフレームの送信、期限、キュー待ち、セッション検査、`on_close`、`on_client_data` の動作。`status_get == NULL` のとき、新しい分岐は `tick` の `if (core->config.status_get)` の 1 か所だけで、既存のコードの経路は変わらない（既存のテスト 63 個は、そのまま全部通る。既存のミューテーション `tools/test/video_core_mutants.py`（114 個）と `video_status_mutants.py`（54 個）も、今回の変更後のソースで、定義を変えずに再実行し、`docs/VIDEO_CORE_TESTS.md` と `docs/VIDEO_STATUS_TESTS.md` の結果と同じだった: 114 個中 95 個を殺して 19 個が生存、54 個中 52 個を殺して 2 個が生存）。

## 2. テストの構成

| ファイル | 内容 | 件数（Unity のテストケース） |
|---|---|---|
| `test/host/video_status/test_video_status_format.c` | 3 つの状態の完全一致、キーの順序と空白なし、解像度の境界（1、65535、65536、`UINT32_MAX` の桁数）、最長 60 バイトで 128 以下、`locked` 以外で 0、範囲外の `state`（`INT_MIN`、`INT_MAX` を含む）、`LOCKED` で 0、バッファ（ちょうど = 文字数 + 1 は成功、1 足りないは -1 で `out` は空、cap 0 / 1、NUL の先は書かない）、`NULL`、入力を変えない、`video_status_get` の補正との一致（総当たり 6 x 5 x 5） | 15 |
| `test/host/ws_video_core/test_ws_video_core_notify.c` | init の検査、接続直後に 1 回（現在の状態を送る）、同じ状態では送らない、状態・解像度（幅だけ・高さだけ）の変化で 1 回、戻れば送らない、`status_get` が書かないとき `unknown`、範囲外の値で落ちない、テキスト・フレームの送信中は送らない、**通知が先でフレームが後**（同じ `tick` で `hub_get_latest` も呼ばない）、状態が同じならフレームは続く、テキストの送信の失敗（-1 と正の値）で閉じる・ほかを巻き込まない、`on_send_done`（テキスト後にフレームの解放が起きない、`ok = false`、CLOSING 中の完了）、スロットの再利用、最短間隔（境界 ±1 ms、1 ms、間隔の間の変化は最新の 1 回、戻れば送らない、長い静止の後は即時、送り始めの時刻から数える、接続直後には適用しない、再接続、間隔 0）、時刻の巻き戻り（3 通り、0 への巻き戻り）、期限（テキストの長さ、クライアントごとの長さ、キュー待ち、停止、互換モード、次のフレームの期限、メッセージごと）、`status_get == NULL` で従来どおり、複数のクライアントの独立（送信中、間隔、接続）、同じ `tick` のスナップショット、テキストのバッファがクライアントごとで送信中は変わらない、`close_requested` / CLOSING / セッション無効のクライアントには送らない、**不変条件ファズ** | 56 |

合計 71 件。`test/host/CMakeLists.txt` は**変更していない**（`test_*.c` の自動発見と、`/ws_video_core/`・`/video_status/` のリンクの規則で足りる）。`ws_video_core` の `CMakeLists.txt` には、`video_status`（`video_status_t`）への依存を足した（ホストの `target_link_libraries` と、IDF の `REQUIRES`。後者は**確認していない**）。ctest は 63 個 -> 65 個（全部成功）。

### 2.1 不変条件ファズ（`test_fuzz_status_notify_matches_model`）

固定シード 300 個 x 3000 ステップ（ASan / UBSan 付き）。接続、切断、`on_send_begin`、`on_send_done`（20% は失敗）、受信、状態の変化（8 種類。範囲外・不整合な値を含む）、新しいフレーム、`tick`（時刻は進む。シードの半分は、巻き戻りを混ぜる）、テキスト / フレームの送信の失敗の注入を混ぜる。間隔（0 / 50 / 200 / 1000）とタイマー（有 / 無）はシードごとに変える。

- **モデルとの照合**: `tick` ごとに、独立した小さなモデル（クライアントごとに `sent`、最後の状態、最後の時刻）が、**どのクライアントがテキストを受けるべきか**を決め、実際の `io_begin_send_text` の呼び出しと一致することを検査する。テキストを送る `tick` では、そのクライアントへのフレームがないこと。送信中・閉じたクライアントには、何も送らないこと。
- **不変条件**: 同じクライアントに同時に 2 つの送信が始まらない、閉じる要求が出たクライアントに送らない、テキストの `on_send_done` でフレームの解放が起きない、フレームの二重保持・二重解放・不正な `unregister` がない、閉じる要求が 2 回出ない、最後にフレームの保持と登録が残らない。
- **収束**: 最後に、どの値とも違う状態にして、閉じられていない全員が、その状態を**ちょうど 1 回**受け取り、本文が `format_json` の出力と一致すること。
- 空振りでないことの確認（固定シードなので毎回同じ値）: テキスト 32,721 回、間隔で保留 3,186 回、テキストの失敗 4,039 回、フレーム 6,434 回、巻き戻り 14,566 回（それぞれ下限を決めてあり、下回るとテストが落ちる）。

## 3. ミューテーションテスト

方法: `tools/test/mutate_video_notify.py` が、`components/` と `test/` をスクラッチにコピーし（作業先が repo と重ならないことを先に確認する。`shell=True` は使わない。前回のビルドは消す）、`video_status.c`（`video_status_format_json`）または `ws_video_core.c`（状態の通知）の 1 か所を置換 -> 再ビルド -> **同じディレクトリの全テスト**（`test/host/video_status/` または `test/host/ws_video_core/`）を実行、を繰り返す。1 つでも失敗（アサーション、ASan / UBSan、クラッシュ、タイムアウト）すれば「殺した」。ミュータントの定義は `tools/test/video_notify_mutants.py`（old 文字列が、対象のファイルに 1 回だけ現れることを、実行時に確認する。同値と判断したものは、理由を持つ）。

```
python3 tools/test/mutate_video_notify.py <repo> <スクラッチ> tools/test/video_notify_mutants.py <結果.json>
```

**結果**（最終の 1 回。ミュータントを入れる前のベースラインは、2 つのグループとも全テスト成功）:

| グループ | 実行した数 | 殺した数 | 生き残った数（すべて同値と判断） | ビルドエラー |
|---|---|---|---|---|
| `video_status_format_json`（N01-N35） | 35 | 32 | 3 | 0 |
| `ws_video_core` の状態の通知（W で始まる 63 個） | 63 | 54 | 9 | 0 |
| 合計 | 98 | 86 | 12 | 0 |

殺した 86 個は、すべて**新しいテスト（`test_video_status_format` または `test_ws_video_core_notify`）だけで**殺せる（既存のテストに頼っていない）。

経緯:
1. 最初の全体の実行（98 個）で 15 個が生き残った。うち 12 個は同値（下の表）。3 個は同値ではなかったので、テストを足して殺した。**W29 / W30**（テキストを入れた時刻 `queued_ms` を記録しない / 0 にする）は、キュー待ちの上限のテストが時刻 0 から始まっていたため、区別できなかった（時刻を 1000 から始めるようにした）。**W62**（期限が、スロット 0 のテキスト長を使う）は、全クライアントが同じ長さのテキストだったため、区別できなかった（`test_text_deadline_uses_each_clients_own_text_length` を足した: スロット 0 と 1 で状態を変えて、長さを違えた）。
2. 同じ実行で、W57（送信完了で `text_len` を 0 に戻さない）を「同値」と書いていたが、**殺された**（`test_send_done_after_text_does_not_release_a_frame` が、構造体の `text_len` が 0 に戻ることを直接確かめている）。振る舞いとしては外から見えない変更なので、**ホワイトボックスの確認で殺している**（同値の表からは外した）。
3. 足した後に、**全体（98 個）を最初から実行し直した**のが、上の結果。(その際、スクリプトの不具合を 1 つ直した: 前回のビルドを残したまま再実行すると、コピーしたソースの更新時刻が古いため、前回の最後のミュータントのオブジェクトが再利用され、ベースラインが落ちた。`build` を消してから始めるようにした。ベースラインの検査が先にあるので、気づかないまま結果が狂うことはなかった。**既存の `mutate_video_core.py` / `mutate_video_status.py` にも同じ形がある**が、今回は変えていない（新しい作業先で実行すれば問題ない）。)

### 3.1 同値のミュータント（12 個）

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| N33 | format: snprintf failure check `n < 0` removed (unreachable: snprintf with these arguments does not fail) | snprintf は、この書式と引数 (%s と %lu だけ) では負を返さない。防御のコードで、外から観測できない |
| N34 | format: tmp overflow check `>=` -> `>` (unreachable: the longest JSON is 60 bytes in a 128-byte buffer) | 最長でも 60 バイトで、128 バイトの tmp に必ず収まる。tmp が一杯になることは起きないので、境界は観測できない |
| N35 | format: tmp overflow check removed (unreachable) | 同上 (最長 60 バイトで tmp に必ず収まる) |
| W05 | due: rollback clamp `<` -> `<=` (same value assigned) | now_ms == status_last_ms のとき、代入は同じ値を書くだけで、観測できない |
| W17 | due: interval 0 guard removed (`> 0 &&`) | interval == 0 のとき、`now - last < 0` は符号なしの比較で常に偽。ガードがあってもなくても同じ |
| W20 | begin: `n > 0` -> `n >= 0` (unreachable: JSON is never empty) | format_json は、成功すれば 44 バイト以上を返し、128 バイトの text_buf で失敗しない。n == 0 は起きない |
| W21 | begin: failure fallback -1 -> 0 (unreachable) | n <= 0 は起きない (format_json は 128 バイトの text_buf で必ず成功する)。防御の分岐で、観測できない |
| W32 | begin: begun flag not reset (already false after on_send_done) | send_begun は、on_send_done と on_connect が必ず false に戻すので、ここで false を代入しても値は変わらない (防御の代入) |
| W42 | begin: JSON buffer capacity 1 less (still enough: longest is 60 of 128) | 最長でも 60 バイトなので、127 バイトの容量でも必ず収まる。128 と 127 の違いは観測できない |
| W53 | connect: last status not reset | status_last は status_sent が true のときだけ読まれ、最初の送信が必ず上書きする。接続時のリセットは観測できない (防御) |
| W54 | connect: last time not reset | status_last_ms は status_sent が true のときだけ読まれ、最初の送信が必ず上書きする (防御) |
| W55 | connect: sending_text/text_len not reset | sending_text は on_send_done が必ず false に戻す。送信中に接続が解放されることはない (CLOSING は on_send_done まで再利用されない) |

### 3.2 殺したミュータントの一覧（86 個）

「殺したテスト」は、失敗したテストのうち先頭の最大 2 つ。`crash` は、NULL の参照などで ASan が落とした（テストの名前が取れない）もの。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| N01 | format: NULL out not checked | `test_video_status_format:crash/rc=1` |
| N02 | format: cap == 0 not checked (writes out[0]) | `test_format_cap_zero_and_one` |
| N03 | format: out[0] not cleared (garbage left on failure) | `test_format_exact_capacity_succeeds_and_one_short_fails`, `test_format_exact_capacity_for_each_state_and_max_digits` |
| N04 | format: NULL st not checked | `test_video_status_format:crash/rc=1` |
| N05 | format: normalize not applied (out-of-range / inconsistent leak out) | `test_format_max_length_is_60_and_within_128`, `test_format_non_locked_zeroes_resolution` |
| N06 | format: NO_SIGNAL name wrong | `test_format_three_states_exact_strings`, `test_format_non_locked_zeroes_resolution` |
| N07 | format: locked name wrong | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N08 | format: unknown name wrong | `test_format_three_states_exact_strings`, `test_format_non_locked_zeroes_resolution` |
| N09 | format: NO_SIGNAL test inverted | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N10 | format: LOCKED test inverted | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N11 | format: LOCKED test replaced by UNKNOWN | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N12 | format: width and height swapped | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N13 | format: height printed twice | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N14 | format: width truncated to 16 bits | `test_format_resolution_boundaries`, `test_format_max_length_is_60_and_within_128` |
| N15 | format: height truncated to 16 bits | `test_format_resolution_boundaries`, `test_format_max_length_is_60_and_within_128` |
| N16 | format: space after the first comma | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N17 | format: space after the colon of w | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N18 | format: key t renamed | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N19 | format: type value renamed | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N20 | format: key w renamed | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N21 | format: key h renamed | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N22 | format: w and h order swapped in the template | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N23 | format: closing brace missing | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N24 | format: width padded | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N25 | format: capacity check off by one (n > cap) | `test_format_exact_capacity_succeeds_and_one_short_fails`, `test_format_exact_capacity_for_each_state_and_max_digits` |
| N26 | format: capacity check off by one (exact capacity rejected) | `test_format_exact_capacity_succeeds_and_one_short_fails`, `test_format_exact_capacity_for_each_state_and_max_digits` |
| N27 | format: capacity check removed | `test_video_status_format:crash/rc=1` |
| N28 | format: NUL not copied | `test_format_three_states_exact_strings`, `test_format_locked_common_resolutions` |
| N29 | format: return value off by one (+1) | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N30 | format: return value off by one (-1) | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| N31 | format: tmp buffer too small for 10-digit values | `test_format_is_fixed_key_order_without_whitespace`, `test_format_locked_common_resolutions` |
| N32 | format: success path returns 0 | `test_format_three_states_exact_strings`, `test_format_is_fixed_key_order_without_whitespace` |
| W01 | due: never-sent client is not due | `test_first_tick_after_connect_sends_status_once`, `test_first_status_is_the_current_status_not_a_fixed_unknown` |
| W02 | due: sent flag inverted | `test_first_tick_after_connect_sends_status_once`, `test_same_status_is_not_resent` |
| W03 | due: first-time check uses slot 0 | `test_text_failure_does_not_affect_other_clients`, `test_first_status_after_connect_ignores_the_interval` |
| W04 | due: clock-rollback clamp removed | `test_clock_rollback_clamps_last_sent_time`, `test_clock_rollback_without_change_then_change` |
| W06 | due: rollback clamp assigns 0 instead of now | `test_clock_rollback_clamps_last_sent_time`, `test_clock_rollback_without_change_then_change` |
| W07 | due: state not compared (state-only changes are missed) | `test_state_change_sends_once`, `test_min_interval_one_ms` |
| W08 | due: width not compared | `test_resolution_change_sends_once`, `test_interval_zero_sends_every_change_immediately` |
| W09 | due: height not compared | `test_resolution_change_sends_once` |
| W10 | due: comparison && -> || (one equal field hides a change) | `test_state_change_sends_once`, `test_resolution_change_sends_once` |
| W11 | due: unchanged check never fires (resend every tick) | `test_same_status_is_not_resent`, `test_state_change_sends_once` |
| W12 | due: comparison against slot 0's last | `test_first_status_after_connect_ignores_the_interval`, `test_multiple_clients_are_independent` |
| W13 | due: interval boundary `<` -> `<=` (exact interval not sent) | `test_min_interval_boundary_plus_minus_one`, `test_min_interval_one_ms` |
| W14 | due: interval boundary off by one (interval - 1) | `test_min_interval_boundary_plus_minus_one`, `test_min_interval_one_ms` |
| W15 | due: interval ignored (always due after a change) | `test_min_interval_boundary_plus_minus_one`, `test_min_interval_one_ms` |
| W16 | due: interval counted from slot 0's last time | `test_first_status_after_connect_ignores_the_interval`, `test_multiple_clients_have_independent_intervals` |
| W18 | due: interval applied with `>=` (blocks only when elapsed is large) | `test_min_interval_boundary_plus_minus_one`, `test_min_interval_one_ms` |
| W22 | begin: failure test `!= 0` -> `< 0` (positive return ignored) | `test_text_send_failure_with_positive_return_also_closes` |
| W23 | begin: failure does not end the send (client stays sending) | `test_text_send_failure_closes_the_client`, `test_text_send_failure_with_positive_return_also_closes` |
| W24 | begin: failure does not close the client | `test_text_send_failure_closes_the_client`, `test_text_send_failure_with_positive_return_also_closes` |
| W25 | begin: failure does not return (state recorded as sent) | `test_text_send_failure_closes_the_client`, `test_failed_text_is_not_counted_as_sent_and_not_retried` |
| W26 | begin: sending_text not set | `test_first_tick_after_connect_sends_status_once`, `test_text_deadline_uses_text_length` |
| W27 | begin: text_len not set | `test_first_tick_after_connect_sends_status_once`, `test_text_deadline_uses_text_length` |
| W28 | begin: text_len off by one | `test_first_tick_after_connect_sends_status_once`, `test_text_deadline_uses_text_length` |
| W29 | begin: queued_ms not recorded | `test_text_deadline_not_applied_before_send_begin_but_queue_timeout_is`, `test_text_queue_timeout_legacy_mode_counts_from_queued` |
| W30 | begin: queued_ms recorded as 0 | `test_text_deadline_not_applied_before_send_begin_but_queue_timeout_is`, `test_text_queue_timeout_legacy_mode_counts_from_queued` |
| W31 | begin: text looks already begun (queue wait counted as send) | `test_text_deadline_not_applied_before_send_begin_but_queue_timeout_is`, `test_text_send_timeout_counts_from_send_begin` |
| W33 | begin: last status not recorded | `test_state_change_sends_once`, `test_resolution_change_sends_once` |
| W34 | begin: last send time not recorded | `test_min_interval_boundary_plus_minus_one`, `test_min_interval_one_ms` |
| W35 | begin: last send time off by one | `test_min_interval_boundary_plus_minus_one`, `test_changes_inside_the_interval_send_only_the_latest_once` |
| W36 | begin: sent flag not set | `test_same_status_is_not_resent`, `test_state_change_sends_once` |
| W37 | begin: text sent to slot 0's connection | `test_first_status_after_connect_ignores_the_interval`, `test_multiple_clients_are_independent` |
| W38 | begin: text taken from slot 0's buffer | `test_each_client_gets_text_on_its_own_connection`, `test_text_buffer_stays_valid_and_unchanged_until_send_done` |
| W39 | begin: text length argument off by one | `test_first_tick_after_connect_sends_status_once`, `test_first_status_is_the_current_status_not_a_fixed_unknown` |
| W40 | begin: user_data not passed | `test_first_tick_after_connect_sends_status_once` |
| W41 | begin: time passed as 0 | `test_min_interval_boundary_plus_minus_one`, `test_min_interval_one_ms` |
| W43 | begin: JSON buffer capacity 1 byte (every status fails -> client closed) | `test_first_tick_after_connect_sends_status_once`, `test_first_status_is_the_current_status_not_a_fixed_unknown` |
| W50 | init: text callback required even without status_get | `test_basic_connect_close`, `test_defer_unregister_during_send` |
| W51 | init: missing text callback accepted | `test_init_status_get_requires_io_begin_send_text` |
| W52 | connect: sent flag not reset (reused slot skips its first status) | `test_reconnect_resets_interval_and_sent_flag`, `test_fuzz_status_notify_matches_model` |
| W56 | send_done: sending_text not cleared (leaks to the next frame's deadline) | `test_send_done_after_text_does_not_release_a_frame`, `test_text_flag_does_not_leak_to_next_connection_in_same_slot` |
| W57 | send_done: text_len not cleared (observable only through the struct: white-box assertion) | `test_send_done_after_text_does_not_release_a_frame` |
| W60 | tick: deadline ignores the text length | `test_text_deadline_uses_text_length`, `test_text_deadline_is_not_the_base_only_nor_the_frame_size` |
| W61 | tick: deadline text branch inverted | `test_deadline_boundary_per_frame_size`, `test_deadline_closes_only_the_slow_client` |
| W62 | tick: deadline uses slot 0's text length | `test_text_deadline_uses_each_clients_own_text_length` |
| W63 | tick: status_get ignored (notification off) | `test_first_tick_after_connect_sends_status_once`, `test_first_status_is_the_current_status_not_a_fixed_unknown` |
| W64 | tick: status_get NULL not checked | `test_ws_video_core:crash/rc=1`, `test_ws_video_core_b3:crash/rc=1` |
| W65 | tick: status snapshot re-read for every client | `test_all_clients_in_one_tick_see_the_same_status_snapshot` |
| W66 | tick: status snapshot flag never set (same effect as re-reading) | `test_all_clients_in_one_tick_see_the_same_status_snapshot` |
| W67 | tick: initial status LOCKED 1x1 (status_get that writes nothing) | `test_status_get_that_writes_nothing_is_unknown` |
| W68 | tick: due check inverted | `test_first_tick_after_connect_sends_status_once`, `test_first_status_is_the_current_status_not_a_fixed_unknown` |
| W69 | tick: due check always true | `test_same_status_is_not_resent`, `test_state_change_sends_once` |
| W70 | tick: frame also sent in the tick that started a status text | `test_first_tick_after_connect_sends_status_once`, `test_no_text_while_text_in_flight_then_latest_after_done` |
| W71 | tick: close-requested clients are not skipped | `test_no_send_after_session_invalid`, `test_no_send_after_send_failure` |
| W72 | tick: timers (queue wait / stall / deadline) skipped while a text is in flight | `test_text_deadline_uses_text_length`, `test_text_deadline_is_not_the_base_only_nor_the_frame_size` |

## 4. 確認していないこと

- **IDF のビルド**: `components/ws_video_core/CMakeLists.txt` の `REQUIRES video_status`、`ws_video_core.h` が `video_status.h`（`kvm_logic.h` を含む）を取り込むこと、`video_status.c` の `snprintf` の ESP-IDF 上のビルドと実行は、**確認していない**（ホストの gcc だけ）。
- **実機**: `io_begin_send_text` の実装（IDF 側）、`slowclient.py` の `40 KB/s` で通常のクライアントに影響しないこと、映像の入力の抜き差しでの表示。
- **`TODO(hw-verify)`**: `status_min_interval_ms` の既定値 200 ms（`docs/VIDEO_TRANSPORT.md` §8 の値。状態の揺れの実際の頻度は未測定）。テキストの期限は、フレームと同じ `frame_deadline_base_ms` + 長さ / `frame_min_rate_bytes_per_s` を使う（実機の値は、`docs/VIDEO_TRANSPORT.md` §3 の `TODO(hw-verify)` のまま）。

## 5. IDF 側（`web_server.c`）の担当に伝えること

1. **`io_begin_send_text(user_data, conn, data, len)`** は、`io_begin_send` と同じ規則で実装する: キューに入れて 0 を返す（失敗は 0 以外 = そのクライアントが閉じられる）。実際に送り始めるときに `ws_video_core_on_send_begin`、終わったら `ws_video_core_on_send_done(ok)` を呼ぶ（テキストでも同じ）。WebSocket のオペコードは**テキスト（`0x81`）**。**1 フレームで送り切る**（`len` は最大 60 バイト。分割しない）。
2. **`data` は、`on_send_done` が呼ばれるまで有効で、変わらない**（`ws_video_core_t` のクライアントごとのバッファ）。コピーしなくてよいが、`on_send_done` の後には参照しないこと。`data` は NUL で終わるが、`len` に NUL は含まない。
3. **`status_get`** には `video_status_get` を渡す（`void (*)(void *user_data, video_status_t *out)` に合わせる、`user_data` を捨てる薄い関数が要る）。**`tick` から、`ws_video_core` を守るロックの下で呼ばれる**ので、ブロックしないこと（`video_status_get` は提供元の関数を呼ぶだけで、ロックしない）。
4. **設定値**: `status_min_interval_ms` は 200（`TODO(hw-verify)`）、`0` なら間隔なし。`io_begin_send_text` を渡さずに `status_get` だけ渡すと、`ws_video_core_init` が -1 を返す。`max_frame_size` の確認は不要（128 バイト以下）。
5. 同時送信は、`ws_video_core` が 1 クライアントにつき 1 つに保つ（テキストとフレームは、同時に送られない）。ほかのクライアントとの交互の送信（スライス）の経路は、テキストにも使えばよい。
6. `ws_video_core` の IDF の `CMakeLists.txt` に `REQUIRES video_status` を足してある。IDF のビルドで循環が起きないか（`video_status` は `kvm_logic` だけを要求する）を、確認してほしい。
