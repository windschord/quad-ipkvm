# ws_video_core (B-3 a) と http_guard_origin_ok_for_port のテストとミューテーションテストの結果

作成: 2026-11-14(Issue #82 ③、B-3 のうち純 C でホストでテストできる部分)。仕様は `docs/VIDEO_TRANSPORT.md` の 3 章(送信時間の数え方と 1 フレーム全体の期限)と 5 章(Origin の規則)。IDF 側(`web_server` への結線、チャンク送信、映像専用の `httpd`)は別の担当で、ここには含まない。

## 1. 変更の要点

- `ws_video_core_on_send_begin(core, fd, now_ms)`: 作業関数が実際に送り始めるときに呼ぶ。`send_started_ms` はここで設定する。`io_begin_send` の成功時(キューに入れた時点)には、`queued_ms` だけを記録する。送信中でない fd、未知の fd、2 回目の呼び出しは無視する(2 回目で開始時刻を延ばして期限を逃れることを許さない)。
- `queue_timeout_ms`(0 = 無効。IDF 側は 10000): `on_send_begin` の前のキュー待ちが、これ以上なら、そのクライアントを閉じる。**この間は `send_timeout_ms` に数えない。**
- `frame_deadline_base_ms` と `frame_min_rate_bytes_per_s`(0 = 期限なし): `on_send_begin` の後で、`now - send_started >= ws_video_frame_deadline_ms(そのクライアントのフレームの大きさ)` なら、そのクライアントだけを閉じる。
- `ws_video_frame_deadline_ms(size, base, min_rate)` = `base + size * 1000 / min_rate`(切り捨て)。`min_rate == 0` は `UINT64_MAX`。結果が 64 ビットを超えるときだけ `UINT64_MAX` に飽和する。`size * 1000` は作らず、`size = q * rate + r` に分けて `q * 1000 + r * 1000 / rate` で求める(`size * 1000` が溢れる大きな `size` でも、結果が 64 ビットに収まれば正確な値を返す)。
- `http_guard_origin_ok_for_port(origin, origin_len, host, host_len, page_port)`: `docs/HTTP_GUARD.md` 2.2.1 に仕様。
- 時刻の巻き戻りは、既存の流儀(開始時刻を now に合わせる)で、`send_started_ms` と `queued_ms` の両方に適用する。
- **互換(判断した点)**: `queue_timeout_ms == 0` のとき(キュー待ちを分離しない設定)は、`on_send_begin` が呼ばれていない送信の停止を、従来どおり、キューに入れた時刻から `send_timeout_ms` で数える。契約の「`queue_timeout_ms == 0` かつ期限なしなら従来と同じ」を満たすため(既存の `test_ws_video_core.c` の `send_timeout` のテストは、`on_send_begin` を呼ばない)。`queue_timeout_ms > 0` のときは、キュー待ちは `send_timeout_ms` に数えない。`on_send_begin` を呼んだ後は、どちらの設定でも、その時刻から数える。

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/ws_video_core/test_ws_video_core.c` | 既存(キュー待ちを分離しない呼び出し側の互換を含む。変更なし) | 25 |
| `test/host/ws_video_core/test_ws_video_core_b3.c` | 期限の関数、`on_send_begin`、キュー待ちの上限、期限(大きさごとの境界 ±1 ms)、ほかのクライアントへの非影響、`on_send_begin` なしの `on_send_done`、時刻の巻き戻り、ファズ | 33 |
| `test/host/http_guard/test_origin_for_port.c` | `http_guard_origin_ok_for_port` の表のテスト(正常、ポートの違い、ホストの違い、大小文字、`:443` の有無、`page_port` が 443 / 非 443、拒否の全種類、極端な長さ、文字の境界)、オラクルとのファズ | 16 |

`test/host/CMakeLists.txt` は変更していない(`test_*.c` の自動発見で、ctest の実行ファイルが 2 つ増える。全体は 51 個で、すべて成功)。

**ファズ(`test_fuzz_timers_match_model`)**: 固定シード 300 個 x 4000 ステップ。設定(`send_timeout` x `queue_timeout` x 期限の速度 x 期限の base)をシードごとに変え、接続・切断・`on_send_begin`(無効な呼び出しを含む)・`on_send_done`・クライアントのデータ・`tick` を乱数で呼ぶ。**テスト側の独立したモデル**(キュー時刻・開始時刻を自分で持つ)が、`tick` ごとに「閉じるべきクライアント」を決め、実際に `io_request_close` された集合と一致することを検査する。ほかに、フレームの二重保持・二重解放・不正な unregister がないこと、1 つの接続に対する close 要求が 1 回以下であること、後始末後にフレームの保持が残らないことを検査する。空振りでないことの確認: 実測(固定シードなので毎回同じ)で、タイマーによる close は 21065 回(下限の検査は 10000 回超)。

**ファズ(`test_fuzz_against_oracle`、Origin)**: 固定シードで 200000 件の Origin / Host / `page_port` を作り、文字列を分けて比べる単純なオラクルと一致することを検査する。実測で受理は 1134 件(下限の検査は 500 件超)。

## 3. ミューテーションテスト

方法: `tools/test/mutate_video_core.py` が、`components/` と `test/` をスクラッチにコピーし、`ws_video_core.c` または `http_guard.c` の 1 か所を置換 -> 再ビルド -> テストを実行、を繰り返す(ミュータントの定義は `tools/test/video_core_mutants.py`)。`ws_video_core.c` のミュータントには `test/host/ws_video_core/` の実行ファイル、`http_guard.c` のミュータントには `test/host/http_guard/` の 8 つの実行ファイル(`http_guard` の既存のテストを含む)を実行する。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。

```
python3 tools/test/mutate_video_core.py <repo> <スクラッチ> tools/test/video_core_mutants.py <結果.json>
```

対象は、新しい・変えたコード: `ws_video_core.c`(`ws_video_frame_deadline_ms`、`reset_send_times`、`on_send_begin`、`on_send_done` と `on_close` の記録のクリア、`tick` のキュー待ち・停止・期限の分岐、キューに入れるときの記録)と、`http_guard.c` の `hg_hostname_ok`、`hg_split_authority`、`http_guard_origin_ok_for_port`。

**結果**(実行した最終の 1 回。ミュータントを入れる前のベースラインは、2 つのグループとも全テスト成功):

| グループ | 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|---|
| ws_video_core.c | 58 | 47 | 11 | 0 |
| http_guard.c | 56 | 48 | 8 | 0 |
| 合計 | 114 | 95 | 19 | 0 |

経緯: 最初の全体の実行は 114 個で、22 個が生き残った。うち 3 個は、同値ではなかったので、テストを足して殺した(下表)。足した後に、**全体(114 個)を最初から実行し直した**のが上の結果。(途中で、ミュータントの old 文字列が複数回現れる定義が 1 つあり、`BAD EDIT` で止まった。定義を直して、最初から実行し直している。)

| 最初の実行で生き残ったミュータント | 足したテスト |
|---|---|
| V20(未知の fd で `on_send_begin` が `clients[-1]` を読む) | `test_on_send_begin_unknown_fd_does_not_touch_memory_before_arrays`(core の直前に「送信中」の偽のクライアントを置き、`last_seqs` などの手前の要素が書き換わらないことを見る。**ASan は、インストルメントされていないライブラリ(`ws_video_core.c` は ASan なしでビルドされる)の範囲外アクセスを検出できない**ので、ヒープに置いて ASan に任せる方法では殺せなかった) |
| H28(ポートの数字の下限の検査がない) | `test_reject_bad_ports` に、`:4/3`(計算すると 393)などの行を追加 |
| H42(スキームの検査を飛ばす) | `test_scheme_mismatch_at_every_position`(スキームの 8 文字のどの位置が違っても拒否する。後ろの 8 文字を飛ばすと、ホスト名に一致する形) |

### 3.1 同値のミュータント(19 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| V11 | deadline: base overflow guard > becomes >= | `extra == UINT64_MAX - base` のとき、`>=` は `UINT64_MAX` に飽和させるが、正確な和 (`base + extra`) も `UINT64_MAX` ちょうどで、同じ値になる。 |
| V16 | reset: queued not cleared | `queued_ms` は、`on_send_done` が (`reset_send_times` とは別に) 0 に戻し、キューに入れるたびに上書きされる。`reset_send_times` での 0 クリアは冗長な二重の防御。(`queued_ms` の内部の値は `test_send_done_without_begin_cleans_up` と `test_slot_reuse_clears_queue_state` が検査している) |
| V17 | reset: begun not cleared | `send_begun` は、`on_send_done` が 0 に戻し (V26 は殺している)、そもそも `is_sending` のときにしか設定されない。`reset_send_times` での クリアは冗長な二重の防御。 |
| V18 | on_connect: reset_send_times not called | スロットを空ける経路 (`on_close` の送信中でない側、`on_send_done` の FREE 側) が、どちらも `reset_send_times` を呼ぶので、`on_connect` での呼び出しは冗長 (3 か所のどれか 1 つで足りる)。3 か所すべてを外した V18c は殺している。 |
| V19 | on_send_begin: NULL core not checked | `core == NULL` のとき、`&core->clients` (先頭のメンバーなので `NULL`) を `ws_clients_find` に渡し、`ws_clients_find` 自身が `ctx == NULL` で `-1` を返す。`on_send_begin` の NULL 検査は冗長な防御。 |
| V28 | on_send_done (FREE path): reset_send_times not called | V18 と同じ (3 か所の `reset_send_times` のうち 1 か所)。 |
| V29 | on_close (not sending path): reset_send_times not called | V18 と同じ (3 か所の `reset_send_times` のうち 1 か所)。 |
| V31 | tick/begun: rollback clamp uses <= (same effect) | `now_ms == send_started_ms` のとき、代入は同じ値 (`send_started_ms = now_ms`) なので、`<` と `<=` は区別できない。 |
| V35 | tick/begun: rate 0 does not skip the deadline check (deadline UINT64_MAX) | `min_rate == 0` のとき、期限は `UINT64_MAX` で、`elapsed >= UINT64_MAX` は (経過が 2^64 ms = 5.8 億年になるまで) 起きない。`> 0` の検査は、無駄な計算を省くだけの冗長な条件。 |
| V43 | tick/begun: no continue after send-timeout close (deadline check also runs) | `continue` を外すと期限の検査に進むが、`do_request_close` は 2 回目以降は何もしない (`close_requested`)。その後に、末尾の `continue` がある。 |
| V57 | tick/enqueue: begun flag reset removed | `send_begun` は、`on_send_done` が既に 0 に戻している (V26 は殺している)。キューに入れるときの 0 クリアは冗長。 |
| H03 | hostname: n == 0 check removed | `n == 0` のとき、ループは回らず、`prev_dot` の初期値が true なので、最後の `return !prev_dot` が false を返す。`n == 0` の明示の検査は冗長。 |
| H19 | hostname: digit range accepts ':' (9+1) | `hg_hostname_ok` に渡るのは、最初の `:` より前の部分だけなので、`:` を含むことがない (範囲の上端 `9` + 1 が `:` なので、変更が観測できない)。 |
| H25 | split: empty port check removed | ポートが空 (`plen == 0`) のとき、ループは回らず `v = 0` で、直後の `v < 1` が拒否する。`plen == 0` の明示の検査は冗長。 |
| H27 | split: leading zero check applies to any length | 先頭ゼロの検査を 1 桁にも広げても、1 桁の `0` は、直後の `v < 1` が拒否するので、結果は同じ。 |
| H37 | main: page_port == 0 check removed | `page_port == 0` は、Origin の実効ポート (1〜65535、または 443) と決して一致しないので、明示の検査がなくても拒否される。 |
| H38 | main: origin_len == 0 / host_len == 0 checks removed | `origin_len == 0` は、直後の `origin_len <= scheme_len` が拒否する。`host_len == 0` は、`hg_hostname_ok(host, 0)` が拒否する。冗長な検査。 |
| H39 | main: scheme length check <= becomes < | `origin_len == scheme_len` (`https://` だけ) のとき、ホスト名が空で、`hg_hostname_ok(..., 0)` が拒否する。`<=` と `<` は結果が同じ。 |
| H43 | main: Origin split failure ignored | Origin の分解が失敗すると、`o_name_len` は 0 のまま (ホスト名が不正) か、`o_port` は 0 のまま (ポートが不正) になる。前者は、有効な Host の名前の長さ (1 以上) と合わず、後者は、`page_port` (1 以上) と合わないので、後の比較が必ず拒否する。(Host 側の分解の失敗を無視する H44 は、`o` 側と違って `h_name_len` が設定された後に失敗する形があるので、殺している) |

### 3.2 殺したミュータントの一覧(95 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(ほかにも落ちるものがあれば省略)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| V01 | deadline: rate 0 returns 0 instead of UINT64_MAX | `test_deadline_rate_zero_means_no_deadline` |
| V02 | deadline: rate 0 check is == 1 | `test_ws_video_core_b3:crash/rc=1` |
| V03 | deadline: remainder part dropped | `test_deadline_values`, `test_deadline_monotonic_in_size_base_and_inverse_in_rate` |
| V04 | deadline: remainder rounded up | `test_deadline_values`, `test_deadline_saturates` |
| V05 | deadline: 1024 instead of 1000 (quotient) | `test_deadline_values`, `test_deadline_saturates` |
| V06 | deadline: 1024 instead of 1000 (remainder) | `test_deadline_values`, `test_deadline_monotonic_in_size_base_and_inverse_in_rate` |
| V07 | deadline: q guard > becomes >= | `test_deadline_saturates` |
| V08 | deadline: q guard allows one more (q+1 overflows) | `test_deadline_saturates` |
| V09 | deadline: q guard removed | `test_deadline_saturates` |
| V10 | deadline: addition-wrap guard removed | `test_deadline_saturates` |
| V12 | deadline: base overflow guard removed | `test_deadline_saturates` |
| V13 | deadline: base not added | `test_deadline_values`, `test_deadline_monotonic_in_size_base_and_inverse_in_rate` |
| V14 | deadline: base added twice | `test_deadline_values`, `test_deadline_monotonic_in_size_base_and_inverse_in_rate` |
| V15 | reset: send_started not cleared | `test_slot_reuse_clears_queue_state` |
| V18c | no reset_send_times anywhere (connect, FREE path, on_close) | `test_slot_reuse_clears_queue_state` |
| V20 | on_send_begin: unknown fd not checked | `test_on_send_begin_unknown_fd_does_not_touch_memory_before_arrays` |
| V21 | on_send_begin: idle (not sending) client accepted | `test_on_send_begin_ignored_for_unknown_or_idle_fd` |
| V22 | on_send_begin: second call restarts the clock | `test_second_on_send_begin_does_not_extend_the_start`, `test_fuzz_timers_match_model` |
| V23 | on_send_begin: begun flag not set | `test_send_started_set_by_on_send_begin_not_by_begin_send`, `test_queue_wait_not_counted_in_send_timeout` |
| V24 | on_send_begin: start time not recorded | `test_send_started_set_by_on_send_begin_not_by_begin_send`, `test_queue_wait_not_counted_in_send_timeout` |
| V25 | on_send_begin: start time off by one | `test_send_started_set_by_on_send_begin_not_by_begin_send`, `test_queue_wait_not_counted_in_send_timeout` |
| V26 | on_send_done: begun not cleared | `test_send_started_set_by_on_send_begin_not_by_begin_send` |
| V27 | on_send_done: queued not cleared | `test_send_started_set_by_on_send_begin_not_by_begin_send` |
| V30 | tick/begun: rollback clamp of send_started removed | `test_clock_rollback_after_begin_clamps_start_for_deadline` |
| V32 | tick/begun: send timeout boundary > instead of >= | `test_queue_wait_not_counted_in_send_timeout`, `test_legacy_mode_on_send_begin_restarts_the_clock` |
| V33 | tick/begun: send timeout 0 does not disable | `test_deadline_boundary_per_frame_size`, `test_deadline_base_only_when_frame_is_empty` |
| V34 | tick/begun: send timeout never fires | `test_queue_wait_not_counted_in_send_timeout`, `test_legacy_mode_on_send_begin_restarts_the_clock` |
| V36 | tick/begun: deadline never checked | `test_deadline_boundary_per_frame_size`, `test_deadline_base_only_when_frame_is_empty` |
| V37 | tick/begun: deadline boundary > instead of >= | `test_deadline_boundary_per_frame_size`, `test_deadline_base_only_when_frame_is_empty` |
| V38 | tick/begun: deadline uses size 0 always | `test_deadline_boundary_per_frame_size`, `test_deadline_closes_only_the_slow_client` |
| V39 | tick/begun: deadline uses slot 0's frame | `test_deadline_uses_each_clients_own_frame_size`, `test_fuzz_timers_match_model` |
| V40 | tick/begun: deadline base ignored | `test_deadline_boundary_per_frame_size`, `test_deadline_base_only_when_frame_is_empty` |
| V41 | tick/begun: deadline rate +1 | `test_deadline_boundary_per_frame_size`, `test_deadline_closes_only_the_slow_client` |
| V42 | tick/begun: elapsed measured from slot 0's start | `test_normal_client_not_blocked_by_slow_client_queue`, `test_fuzz_timers_match_model` |
| V44 | tick/queue: begun check always true (queue wait counted as send) | `test_send_timeout_requests_close_at_exact_boundary`, `test_send_timeout_resets_for_each_new_frame` |
| V45 | tick/queue: begun check always false (never sees begun) | `test_queue_wait_not_counted_in_send_timeout`, `test_legacy_mode_on_send_begin_restarts_the_clock` |
| V46 | tick/queue: rollback clamp of queued_ms removed | `test_send_timeout_clock_rollback_clamps_without_false_close`, `test_clock_rollback_in_queue_clamps_queued_time` |
| V47 | tick/queue: boundary > instead of >= | `test_queue_timeout_boundary`, `test_send_done_without_begin_cleans_up` |
| V48 | tick/queue: queue timeout 0 does not disable | `test_send_timeout_requests_close_at_exact_boundary`, `test_send_timeout_zero_disables_stall_disconnect` |
| V49 | tick/queue: legacy branch removed | `test_send_timeout_requests_close_at_exact_boundary`, `test_send_timeout_resets_for_each_new_frame` |
| V50 | tick/queue: legacy boundary > instead of >= | `test_send_timeout_requests_close_at_exact_boundary`, `test_send_timeout_resets_for_each_new_frame` |
| V51 | tick/queue: legacy send_timeout 0 does not disable | `test_send_timeout_zero_disables_stall_disconnect`, `test_queue_timeout_zero_is_disabled_when_send_timeout_zero` |
| V52 | tick/queue: queue wait measured from slot 0 | `test_fuzz_timers_match_model` |
| V54 | tick/enqueue: queued_ms not recorded | `test_send_timeout_requests_close_at_exact_boundary`, `test_send_timeout_resets_for_each_new_frame` |
| V55 | tick/enqueue: queued_ms recorded as 0 | `test_send_timeout_requests_close_at_exact_boundary`, `test_send_timeout_resets_for_each_new_frame` |
| V56 | tick/enqueue: send_started also set (old behaviour) | `test_send_started_set_by_on_send_begin_not_by_begin_send` |
| V58 | tick/enqueue: begun flag set (looks begun at queue time) | `test_send_started_set_by_on_send_begin_not_by_begin_send`, `test_queue_wait_not_counted_in_send_timeout` |
| H01 | hostname: max 255 -> 256 | `test_extreme_lengths` |
| H02 | hostname: max 255 -> 254 | `test_extreme_lengths`, `test_name_of_255_with_trailing_dot` |
| H04 | hostname: leading dot allowed | `test_reject_userinfo_ipv6_trailing_dot_path`, `test_fuzz_against_oracle` |
| H05 | hostname: consecutive dots allowed | `test_reject_userinfo_ipv6_trailing_dot_path`, `test_fuzz_against_oracle` |
| H06 | hostname: trailing dot allowed | `test_reject_userinfo_ipv6_trailing_dot_path`, `test_name_of_255_with_trailing_dot` |
| H07 | hostname: '-' not allowed | `test_accept_main_port_443`, `test_hostname_charset_boundaries` |
| H08 | hostname: '_' not allowed | `test_accept_main_port_443`, `test_hostname_charset_boundaries` |
| H09 | hostname: digit range starts at 1 | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H10 | hostname: digit range ends at 8 | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H11 | hostname: lowercase range ends at y | `test_hostname_charset_boundaries`, `test_consistent_with_existing_origin_ok` |
| H12 | hostname: lowercase range starts at b | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H13 | hostname: lowercase range accepts '`' (a-1) | `test_hostname_charset_boundaries` |
| H14 | hostname: lowercase range accepts '{' (z+1) | `test_hostname_charset_boundaries` |
| H15 | hostname: uppercase range ends at Y | `test_hostname_charset_boundaries` |
| H16 | hostname: uppercase range accepts '@' (A-1) | `test_hostname_charset_boundaries`, `test_fuzz_against_oracle` |
| H17 | hostname: uppercase range accepts '[' (Z+1) | `test_hostname_charset_boundaries`, `test_fuzz_against_oracle` |
| H18 | hostname: digit range accepts '/' (0-1) | `test_hostname_charset_boundaries`, `test_fuzz_against_oracle` |
| H20 | split: hostname check removed | `test_reject_userinfo_ipv6_trailing_dot_path`, `test_length_arguments_are_respected` |
| H21 | split: has_port always false | `test_accept_nonstandard_main_port`, `test_port_mismatch` |
| H22 | split: no-port early return removed | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H23 | split: port length limit 5 -> 10 | `test_reject_bad_ports` |
| H24 | split: port length limit 5 -> 4 | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H26 | split: leading zero check removed | `test_reject_bad_ports`, `test_fuzz_against_oracle` |
| H28 | split: digit lower bound removed | `test_reject_bad_ports` |
| H29 | split: digit upper bound removed | `test_reject_bad_ports`, `test_fuzz_against_oracle` |
| H30 | split: port 0 accepted | `test_reject_bad_ports`, `test_fuzz_against_oracle` |
| H31 | split: port 65536 accepted | `test_reject_bad_ports`, `test_fuzz_against_oracle` |
| H32 | split: port 65535 rejected | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H33 | split: port value not stored | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H34 | split: name length not stored | `test_origin_for_port:crash/rc=1` |
| H35 | main: NULL origin not checked | `test_origin_for_port:crash/rc=1` |
| H36 | main: NULL host not checked | `test_origin_for_port:crash/rc=1` |
| H40 | main: scheme compared case-sensitively | `test_case_insensitive`, `test_fuzz_against_oracle` |
| H41 | main: scheme check only first 7 chars | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H42 | main: scheme check skipped | `test_scheme_mismatch_at_every_position` |
| H44 | main: Host split failure ignored | `test_reject_bad_ports`, `test_length_arguments_are_respected` |
| H45 | main: default Origin port 80 | `test_accept_main_port_443`, `test_case_insensitive` |
| H46 | main: default Origin port 0 | `test_accept_main_port_443`, `test_case_insensitive` |
| H47 | main: default Origin port is page_port (no port always matches) | `test_port_mismatch`, `test_fuzz_against_oracle` |
| H48 | main: port comparison removed | `test_port_mismatch`, `test_fuzz_against_oracle` |
| H49 | main: Origin port compared with Host port instead of page_port | `test_accept_main_port_443`, `test_accept_nonstandard_main_port` |
| H50 | main: name length comparison removed | `test_origin_for_port:crash/rc=1` |
| H51 | main: name compared case-sensitively | `test_case_insensitive`, `test_hostname_charset_boundaries` |
| H52 | main: only Origin side lowercased | `test_case_insensitive`, `test_hostname_charset_boundaries` |
| H53 | main: only Host side lowercased | `test_case_insensitive`, `test_hostname_charset_boundaries` |
| H54 | main: name comparison skips the first character | `test_host_name_mismatch`, `test_fuzz_against_oracle` |
| H55 | main: name comparison skips the last character | `test_host_name_mismatch`, `test_fuzz_against_oracle` |
| H56 | main: name comparison removed | `test_host_name_mismatch`, `test_fuzz_against_oracle` |

## 4. 実行できなかった確認 / 未確定

- IDF のビルド(`web_server` への結線、`ws_video_core_config_t` の新しいメンバーを使う側)は、このタスクの範囲外で、実行していない。`ws_video_core_config_t` に足したメンバー(`queue_timeout_ms`、`frame_deadline_base_ms`、`frame_min_rate_bytes_per_s`)は、指定初期化子で書いている呼び出し側では、書かなければ 0(= 無効)になるので、既存の呼び出しは壊れない。
- 実機での測定(`slowclient.py` の `40 KB/s` など)は、`docs/VIDEO_TRANSPORT.md` 6 章の device-check の担当。
- 期限の既定(`base` = 200 ms、`min_rate` = 1,000,000 B/s)、`queue_timeout_ms` = 10000 は、契約どおり `TODO(hw-verify)`(IDF 側が渡す値。このコンポーネントは値を持たない)。

## 5. P7: H.264 のキーフレーム待ち(`ws_video_core`)と `frame_hub` の `codec` / `flags`

作成: 2026-11-16(P7。契約は `docs/VIDEO_TRANSPORT.md` 9 章、`docs/INTERFACES.md` 1.4)。純 C で、ホストでテストできる部分だけ。`web_server.c`(`accept=h264` の解析、`request_keyframe` の配線)、`csi_video_source.c`、`main.c` は変更していない。

### 5.1 変更の要点

- `frame_hub`: `video_frame_t` の末尾に `codec` / `flags` を足した(`video_codec_t`、`VIDEO_FRAME_FLAG_KEY`)。`frame_hub_commit_ex(hub, frame, info)` を足し、`frame_hub_commit()` はシグネチャを変えずに `codec = MJPEG`、`flags = KEY` で `commit_ex` に写す。`frame_hub_begin_publish()` が払い出すスロットは、`codec = MJPEG`、`flags = 0` に戻す。`size` の規則(0 と `max_frame_size` 超過は捨てて `dropped_frames`)、二重の commit・未知のフレームの無視は `commit` と同じ。`info == NULL` は、公開できないので `abort_publish` と同じ(スロットを「書き込み中」のまま残さない。数えない)。
- `video_source_t`: 末尾(`ctx` の後)に `supported_codecs` / `set_codec` / `request_keyframe` を足した。`test_pattern_source.c` と `csi_video_source.c` は、指示付き初期化(`.name = ...`)で変更なしに通る。
- `ws_video_core`: 設定に `request_keyframe`、`idr_min_interval_ms`、`key_wait_timeout_ms` を足した。クライアントごとに `need_key`(と、待ちの起点、最後に配ったコーデック)、全体に要求の状態を持つ。
  - `need_key` を真にする: (a) 接続したとき、(b) 配ろうとするフレームの `seq` が `last_seqs + 1` でないとき(戻った場合も含む)、(c) H.264 のフレームで、直前に配ったフレームが MJPEG(または初めて)のとき。すでに真なら何もしない(待ちの起点も要求も、やり直さない)。
  - `need_key` が真のクライアントには、H.264 で `KEY` の立っていないフレームを配らない(`hub_release` して次の tick)。MJPEG(H.264 以外の値も)と H.264 の IDR は配り、`need_key` を偽にする。
  - `request_keyframe`: `need_key` を偽 -> 真にしたとき、要求を 1 つ溜める。tick の**最後**に、溜まっていて、まだ待っているクライアントがいれば、`idr_min_interval_ms` が経っていれば 1 回呼ぶ(経っていなければ溜めたまま、経った最初の tick で 1 回だけ)。要求がまとまるのは、複数のクライアント、同じ tick の複数の印、間隔の間の印。誰も待たなくなったら(配れた、切断、閉じる要求)、溜めた要求は捨てる。`request_keyframe == NULL` なら溜めない(待ちと切断は動く)。
  - `key_wait_timeout_ms`: H.264 のフレームを**最初に配らなかった時刻**から数え、`need_key` が `key_wait_timeout_ms` 以上続いたクライアントを `io_request_close` で閉じる(フレームの送信中・状態のテキストの送信中でも)。0 = 閉じない。
- MJPEG だけのとき(`codec` がずっと MJPEG)は、フレームを配るたびに `need_key` が偽になり、待ちも切断も起きない。`request_keyframe` が `NULL` の既存の設定では、既存の 3 つのテストファイルは、変更なしで全件通る。

### 5.2 テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/frame_hub/test_frame_hub.c` | 既存 14 件 + P7: `commit` が MJPEG / KEY、`commit_ex` の `codec` / `flags` の受け渡し(`size == max` も)、`begin_publish` での初期化(使い回すスロット)、`commit` が公開側の書き込みを上書きすること、`size` 0 / 超過が `commit` と同じ、誤用(二重・未知・NULL)、`video_source_t` の位置指定・指示付き初期化で P7 の項目が 0 / NULL、P7 の項目、ファズ(20 シード x 1500 ステップ) | 24(+10) |
| `test/host/ws_video_core/test_ws_video_core_keyframe.c`(新規) | 接続直後(P を配らない / IDR から / IDR が先なら要求も不要)、seq の飛び(境界 +1 / +2、戻り、IDR 自身は配る)、MJPEG は影響されない(飛び、KEY なし、未知のコーデックの値、`KEY` 以外のビット)、MJPEG -> H.264 の切替と戻り、要求のまとめ(複数のクライアント、境界 -1 / ちょうど / +1、誰も待たなくなったら捨てる、間隔 0、`NULL`、`user_data`)、`key_wait_timeout_ms`(境界 -1 / ちょうど、起点の再設定がないこと、IDR で終わった後は閉じない、0 = 閉じない、フレームが来ない間は数えない、ほかのクライアントに影響しない、状態のテキスト・フレームの送信中でも数える)、C-6b との併用、複数のクライアントの独立、同じスロットでの再接続、`begin_send` の失敗、時刻の巻き戻り、ファズ | 46 |

`test/host/CMakeLists.txt` は変更していない(`test_*.c` の自動発見)。ctest の実行ファイルは 66 -> 67 個で、すべて成功。

**ファズ(`test_fuzz_keyframe_gating_matches_model_and_invariants`)**: 固定シード 400 個 x 3000 ステップ。設定(`idr_min_interval_ms` x `key_wait_timeout_ms` x `request_keyframe` の有無 x 状態の通知の有無 x 時刻の巻き戻りの有無)をシードごとに変え、接続・切断・閉じる要求・送信の完了(失敗を含む)・フレームの公開(MJPEG / H.264、IDR、1 回に複数、信号なし)・tick(`begin_send` の失敗を含む)をランダムな順で行う。次の 2 つを毎 tick で検査する。(1) 独立したモデル(クライアントごとに「配る / 配らない / 閉じる / テキスト」と、要求を呼ぶ tick を予測)と、実際の動作(配られた `seq` / `codec` / `flags`、`release` の数、閉じた回数、要求の回数、送信中の状態)が一致する。(2) モデルの規則を使わない不変条件: H.264 の P が配られるのは、直前に**実際に配られた**フレームが H.264 で `seq` が 1 つ前のときだけ(= 接続直後・飛び・コーデックの切替の後に、IDR なしで P が配られない)、1 tick に要求は高々 1 回、(巻き戻りなしのシードで)要求の間隔が `idr_min_interval_ms` 以上、保持と登録が残らない。実測(固定シードなので毎回同じ): 配らなかったフレーム 30474、待ちを IDR で終えた 4661、要求 21989、間隔で溜めた 4095、期限で閉じた 9711、飛びによる印 15688、切替による印 481、状態のテキスト 17270、巻き戻り 14512(いずれも下限を検査して、空振りでないことを確認している)。

### 5.3 ミューテーションテスト(P7 の追加分)

`tools/test/mutate_video_core.py` に `frame_hub.c` のグループを足した(`frame_hub.c` のミュータントには `test/host/frame_hub/` の全部を実行する)。追加したミュータントは、`ws_video_core.c` に 58 個(`K01`〜`K62`。欠番は、実装を簡単にして、冗長な分岐のミュータントが消えたもの)、`frame_hub.c` に 23 個(`F01`〜`F23`)。**最終の全体の実行(既存の 114 個を含む)**:

```
python3 tools/test/mutate_video_core.py <repo> <スクラッチ> tools/test/video_core_mutants.py <結果.json>
run: 195 killed: 173 survived: 22 build_error: 0      (ベースラインは 3 つのグループとも全テスト成功)
```

| グループ | 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|---|
| ws_video_core.c(既存 58 + P7 58) | 116 | 102 | 11(既存) + 3(P7) | 0 |
| http_guard.c | 56 | 48 | 8(既存) | 0 |
| frame_hub.c(P7) | 23 | 23 | 0 | 0 |
| 合計 | 195 | 173 | 22 | 0 |

P7 の追加分だけ見ると、81 個(ID `K` が 58 個、`F` が 23 個)のうち 78 個を殺し、3 個が同値で生き残った。

経緯: 最初の実行で、実装の冗長な分岐に由来する生き残りが出た。(1) `key_waiting` を配ったときに戻す分岐と、`need_key` を戻す条件(`!is_h264 || is_key`)は、`need_key` が真のまま配られるのは MJPEG と IDR だけなので、条件が常に真と同じだった。**テストを足すのではなく、冗長なコードを消して**(配ったら無条件に `need_key` を偽にする。`key_waiting` は `need_key` が偽の間は参照せず、`mark_need_key` が戻す)、その参照を外すと動作が変わるミュータントは、テストで殺した(`test_key_wait_is_not_applied_after_the_wait_ended` が `K28` を殺す)。(2) seq が戻る場合(`K37`)を殺すテスト `test_backward_seq_is_a_discontinuity` を足した。そのあと、**全体を最初から実行し直した**のが上の結果。(途中で、書き換えた箇所に合わなくなったミュータントの old 文字列が `BAD EDIT` で止めたので、直して、やり直した。)

#### 5.3.1 同値のミュータント(P7 の分。3 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| K19 | flush: `request_keyframe` の `NULL` の検査を外す | 要求を溜めるのは `mark_need_key` が `request_keyframe != NULL` のときだけで、`NULL` のまま `flush` に溜まった要求が届くことはない。`NULL` の検査は、設定が実行中に変わる(起こらない)ときのための二重の防御。 |
| K21 | connect: `last_codec` の初期値を H.264 にする | 接続時に `need_key` を真にしているので、最初に配るフレームの (c) の判定は、すでに真の `need_key` に対する何もしない操作になる。最初のフレームを配ると `last_codec` はそのフレームのもので上書きされる。初期値は、「初めて = MJPEG 扱い」という意味を明示する二重の防御。 |
| K24 | connect: `last_codec` を戻さない(前のクライアントの値のまま) | K21 と同じ理由(接続後の最初の配信で必ず上書きされ、それまでは `need_key` が真)。 |

(既存の 19 個の同値は、3.1 を参照。)

#### 5.3.2 殺したミュータントの一覧(P7 の分。78 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| K01 | mark: not idempotent (already-true need_key is marked again) | `test_key_wait_timeout_boundary`, `test_key_wait_does_not_restart_on_each_withheld_frame` |
| K02 | mark: need_key not set | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_without_frames_requests_once_at_end_of_first_tick` |
| K03 | mark: request is not queued | `test_connect_without_frames_requests_once_at_end_of_first_tick`, `test_seq_gap_makes_need_key_until_next_idr` |
| K04 | mark: request queued even without a request callback | `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K05 | flush: queued request not dropped when nobody needs a key | `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request`, `test_seq_gap_makes_need_key_until_next_idr` |
| K06 | flush: close_requested clients count as waiting | `test_failed_begin_send_of_idr_closes_and_does_not_count_as_delivered`, `test_close_requested_client_is_not_served_and_not_counted_as_waiting` |
| K07 | flush: non-ACTIVE clients count as waiting | `test_pending_request_is_dropped_when_the_waiting_client_is_closed`, `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K08 | flush: need_key ignored (any client counts) | `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request`, `test_seq_gap_makes_need_key_until_next_idr` |
| K09 | flush: interval boundary < becomes <= | `test_request_boundary_waits_idr_min_interval_then_one_call`, `test_request_boundary_one_ms_after_interval` |
| K10 | flush: interval boundary < becomes > (inverted) | `test_request_boundary_waits_idr_min_interval_then_one_call`, `test_request_boundary_one_ms_after_interval` |
| K11 | flush: interval ignored (always call) | `test_request_boundary_waits_idr_min_interval_then_one_call`, `test_request_boundary_one_ms_after_interval` |
| K12 | flush: pending not cleared after the call (repeats every tick) | `test_interval_zero_does_not_merge_across_ticks_but_merges_within_a_tick`, `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K13 | flush: idr_requested never set (first-call state lost, no throttling) | `test_request_boundary_waits_idr_min_interval_then_one_call`, `test_request_boundary_one_ms_after_interval` |
| K14 | flush: last request time not recorded | `test_request_boundary_one_ms_after_interval`, `test_clock_rollback_clamps_last_request_time` |
| K15 | flush: clock rollback clamp removed | `test_clock_rollback_clamps_last_request_time`, `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K16 | flush: throttle also applied before the first request | `test_connect_without_frames_requests_once_at_end_of_first_tick`, `test_seq_gap_makes_need_key_until_next_idr` |
| K17 | flush: request called with NULL user_data | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_without_frames_requests_once_at_end_of_first_tick` |
| K18 | tick: flush_keyframe_request never called | `test_connect_without_frames_requests_once_at_end_of_first_tick`, `test_seq_gap_makes_need_key_until_next_idr` |
| K20 | connect: need_key not marked (a) | `test_connect_without_frames_requests_once_at_end_of_first_tick`, `test_mjpeg_delivered_regardless_of_need_key_and_clears_it` |
| K22 | connect: stale need_key not reset (mark returns early) | `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K23 | connect: stale key_waiting / need_key / since all kept | `test_reconnect_after_timeout_close_has_a_new_wait`, `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K25 | key-wait: boundary >= becomes > | `test_request_keyframe_null_still_gates_and_never_requests`, `test_key_wait_timeout_boundary` |
| K26 | key-wait: timeout 0 does not disable | `test_key_wait_zero_never_closes`, `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K27 | key-wait: counted before any frame was withheld (key_waiting ignored) | `test_key_wait_does_not_count_while_no_frame_arrives`, `test_reconnect_after_timeout_close_has_a_new_wait` |
| K28 | key-wait: need_key ignored (key_waiting only) | `test_idr_before_timeout_prevents_close_and_new_wait_has_new_origin`, `test_key_wait_is_not_applied_after_the_wait_ended` |
| K29 | key-wait: never closes | `test_request_keyframe_null_still_gates_and_never_requests`, `test_key_wait_timeout_boundary` |
| K30 | key-wait: clock rollback clamp removed | `test_clock_rollback_clamps_key_wait_origin`, `test_clock_rollback_to_zero_does_not_underflow` |
| K31 | key-wait: no continue after the close request | `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K32 | key-wait: the check runs only for idle (not sending) clients | `test_key_wait_closes_even_while_a_send_is_in_flight` |
| K33 | key-wait: timeout shortened by 1 ms (>= timeout - 1) | `test_request_keyframe_null_still_gates_and_never_requests`, `test_key_wait_timeout_boundary` |
| K34 | gap (b): condition removed (seq gaps ignored) | `test_seq_gap_makes_need_key_until_next_idr`, `test_gap_boundary_seq_plus_one_is_continuous_plus_two_is_not` |
| K35 | gap (b): expects seq + 2 | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K36 | gap (b): compares with the last seq itself | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K37 | gap (b): only a forward gap counts (> instead of !=) | `test_backward_seq_is_a_discontinuity` |
| K38 | gap (b): a gap of exactly one frame is tolerated (> + 2) | `test_seq_gap_makes_need_key_until_next_idr`, `test_gap_boundary_seq_plus_one_is_continuous_plus_two_is_not` |
| K39 | codec (c): switch to H.264 not detected | `test_mjpeg_to_h264_switch_sets_need_key`, `test_h264_to_mjpeg_to_h264_sets_need_key_each_time` |
| K40 | codec (c): marks while the previous frame was H.264 | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K41 | codec (c): is_h264 dropped (marks on every frame after a non-H.264 one) | `test_fuzz_keyframe_gating_matches_model_and_invariants` |
| K42 | withhold: an IDR is also withheld (!is_key dropped) | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K43 | withhold: MJPEG is also withheld (is_h264 dropped) | `test_defer_unregister_during_send`, `test_send_failure_releases_and_closes` |
| K44 | withhold: need_key ignored (every P is withheld) | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K45 | withhold: never withholds | `test_connect_h264_p_is_not_delivered_until_idr`, `test_seq_gap_makes_need_key_until_next_idr` |
| K46 | withhold: frame not released | `test_connect_h264_p_is_not_delivered_until_idr`, `test_seq_gap_makes_need_key_until_next_idr` |
| K47 | withhold: end_send not called (client stays 'sending') | `test_connect_h264_p_is_not_delivered_until_idr`, `test_seq_gap_makes_need_key_until_next_idr` |
| K48 | withhold: wait origin not recorded (since stays 0) | `test_request_keyframe_null_still_gates_and_never_requests`, `test_key_wait_timeout_boundary` |
| K49 | withhold: key_waiting never set | `test_request_keyframe_null_still_gates_and_never_requests`, `test_key_wait_timeout_boundary` |
| K50 | withhold: wait origin restarts at every withheld frame | `test_key_wait_timeout_boundary`, `test_key_wait_does_not_restart_on_each_withheld_frame` |
| K51 | is_h264: any codec other than MJPEG counts as H.264 | `test_unknown_codec_value_is_not_gated` |
| K52 | is_key: any non-zero flags count as a key frame | `test_h264_flag_bits_other_than_key_do_not_count_as_key` |
| K53 | is_key: only flags == KEY counts (other bits break it) | `test_h264_flag_bits_other_than_key_do_not_count_as_key` |
| K54 | is_key: flags ignored (no frame is a key frame) | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K58 | clear: need_key never cleared on delivery | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K60 | delivered: last_codec not recorded | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| K61 | delivered: last_codec always H.264 | `test_mjpeg_to_h264_switch_sets_need_key`, `test_h264_to_mjpeg_to_h264_sets_need_key_each_time` |
| K62 | delivered: last_codec always MJPEG | `test_connect_h264_p_is_not_delivered_until_idr`, `test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request` |
| F01 | begin_publish: codec not reset | `test_frame_hub_begin_publish_resets_codec_and_flags` |
| F02 | begin_publish: flags not reset | `test_frame_hub_begin_publish_resets_codec_and_flags` |
| F03 | begin_publish: codec reset to H.264 | `test_frame_hub_begin_publish_resets_codec_and_flags` |
| F04 | begin_publish: flags reset to KEY | `test_frame_hub_begin_publish_resets_codec_and_flags` |
| F05 | commit: codec is H.264 | `test_frame_hub_commit_sets_mjpeg_key`, `test_frame_hub_commit_overrides_dirty_fields` |
| F06 | commit: flags 0 (not KEY) | `test_frame_hub_commit_sets_mjpeg_key`, `test_frame_hub_commit_overrides_dirty_fields` |
| F07 | commit: size not passed on | `test_frame_hub_basic_publish_subscribe`, `test_frame_hub_size_alternation` |
| F08 | commit: width and height swapped | `test_frame_hub_commit_sets_mjpeg_key` |
| F09 | commit: timestamp not passed on | `test_frame_hub_commit_sets_mjpeg_key` |
| F10 | commit_ex: codec not stored | `test_frame_hub_commit_ex_passes_codec_and_flags`, `test_frame_hub_commit_overrides_dirty_fields` |
| F11 | commit_ex: flags not stored | `test_frame_hub_commit_sets_mjpeg_key`, `test_frame_hub_commit_ex_passes_codec_and_flags` |
| F12 | commit_ex: KEY forced on | `test_frame_hub_commit_ex_passes_codec_and_flags`, `test_frame_hub_codec_flags_fuzz` |
| F13 | commit_ex: codec forced to MJPEG | `test_frame_hub_commit_ex_passes_codec_and_flags`, `test_frame_hub_codec_flags_fuzz` |
| F14 | commit_ex: size == max_frame_size rejected (> becomes >=) | `test_frame_hub_commit_ex_passes_codec_and_flags`, `test_frame_hub_commit_ex_size_rules_match_commit` |
| F15 | commit_ex: size 0 accepted | `test_frame_hub_publish_abort`, `test_frame_hub_commit_ex_size_rules_match_commit` |
| F16 | commit_ex: oversize accepted | `test_frame_hub_commit_oversize`, `test_frame_hub_commit_ex_size_rules_match_commit` |
| F17 | commit_ex: size not stored | `test_frame_hub_basic_publish_subscribe`, `test_frame_hub_size_alternation` |
| F18 | commit_ex: width not stored | `test_frame_hub_commit_sets_mjpeg_key`, `test_frame_hub_commit_ex_passes_codec_and_flags` |
| F19 | commit_ex: height not stored | `test_frame_hub_commit_sets_mjpeg_key`, `test_frame_hub_commit_ex_passes_codec_and_flags` |
| F20 | commit_ex: timestamp not stored | `test_frame_hub_commit_sets_mjpeg_key`, `test_frame_hub_commit_ex_passes_codec_and_flags` |
| F21 | commit_ex: NULL info not handled (dereferenced) | `test_frame_hub:crash/rc=1` |
| F22 | commit_ex: NULL info leaves the slot in 'writing' | `test_frame_hub_commit_ex_misuse` |
| F23 | commit_ex: oversize discard not counted | `test_frame_hub_commit_oversize`, `test_frame_hub_publish_abort` |

### 5.4 確認したこと / 未確認 / 契約の解釈

**確認したこと(ホスト)**
- `cmake` + `ctest`(`-Wall -Wextra -Werror`、ASan / UBSan)で、全 67 件が成功(前は 66 件)。既存のテストは 1 つも書き換えていない。
- 上のミューテーション(195 個を実行。P7 の追加分 81 個のうち、同値の 3 個を除いて殺した。既存の分も、前と同じ 19 個の同値だけが生き残る)。

**契約(`VIDEO_TRANSPORT.md` 9.6)からの解釈と判断(owner / 親の確認が要る)**
1. **要求は、`on_connect` ではなく tick の最後に呼ぶ**。接続したとき(a)に、最初の tick で状態のテキストを先に送る(C-6b)と、その tick にフレームを配れず、要求が出る。MJPEG のとき、`core` は、接続した時点ではコーデックを知らないので、`request_keyframe` を渡す設定では、**接続の直後に 1 回、要求が出ることがある**(MJPEG のフレームを同じ tick に配れたときは出ない)。`video_source_t.request_keyframe` が `NULL` の MJPEG だけのソースでは、呼ばれない。MJPEG の接続のたびに要求を出したくないなら、「要求は、H.264 のフレームを配らなかったときだけ」に変えられる(接続直後の IDR は、要求なしで届くので、IDR の無駄が減る)。この変更は、契約(「`need_key` を真にしたら要求」)に合わせて、していない。
2. **要求は、`need_key` が偽 -> 真に変わったときだけ**(待ちの間に、P を配らないたびには出さない)。要求が失われたときの保険は、契約どおり、映像ソースの周期的な IDR(`CSI_H264_GOP`)と `key_wait_timeout_ms` による再接続。
3. **溜めた要求は、誰も待たなくなったら捨てる**(IDR が届いた、切断、閉じる要求)。間隔が経っても、不要な IDR(最大 495 KB)は出さない。
4. **`key_wait_timeout_ms` の起点は、最初に H.264 のフレームを配らなかった時刻**(接続した時刻ではない)。接続した時刻から数えると、フレームが来ない間(信号なし、映像ソース未起動)に、MJPEG のクライアントまで `key_wait_timeout_ms` で閉じてしまい、従来の動作(フレームがなくても接続は続く)と変わるため。フレームが 1 つも来ない間は、閉じない。
5. `idr_min_interval_ms == 0`(まとめない)でも、同じ tick の要求は 1 回(tick に 1 回まで)。
6. `video_frame_t.codec` が `VIDEO_CODEC_H264` 以外の値(契約の外)は、MJPEG と同じに配る(配らない側には倒さない)。
7. `frame_hub_commit_ex(info == NULL)` は `abort_publish` と同じ(契約に規定なし)。
8. **`video_source_t` の位置指定の初期化 `{name, start, stop, ctx}` は、`-Wextra` の gcc では `-Wmissing-field-initializers` の警告が出る**(足した項目が未指定になるため)。ビルドは通り、足した項目は 0 / `NULL`(テストで確認)。`test_pattern_source.c` と `csi_video_source.c` は指示付き初期化なので、警告は出ない。位置指定で書く新しいコードがあれば、`-Werror` で止まる可能性がある(IDF のビルドは、この環境にない)。

**未確認**
- IDF のビルド(`web_server.c` への結線: `?accept=h264` の解析、`request_keyframe` を `video_source_t.request_keyframe` に包む関数(tick から、ロックの下で呼ばれるので、ブロックしないこと)、`ws_video_core_config_t` の新しい 3 項目の設定)。`csi_video_source.c` は IDF 専用で、ホストでは構文を見ていない(指示付き初期化の形は、目で確認した)。
- 既定値(`TODO(hw-verify)`、`core` は値を持たず、渡す側が決める): `idr_min_interval_ms`(契約の `VIDEO_CODEC_IDR_MIN_INTERVAL_MS` = 1000。IDR は 495 KB に達する)、`key_wait_timeout_ms`(契約の `VIDEO_CODEC_KEY_WAIT_MS` = 5000)、`CSI_H264_GOP`(HDMI 担当)。
- 実機での、IDR の要求から、IDR が届くまでの時間、遅いクライアントでの待ちの長さ、再接続の頻度。
- `video_codec_policy`(コーデックの方針)、ビューアの H.264 の経路は、このタスクの範囲外。

## 6. P7: `accept=h264` のクエリの解析(`http_guard_video_accept`)と `web_server.c` の結線

作成: 2026-11-17(P7。契約は `docs/VIDEO_TRANSPORT.md` 9.4・9.5・9.6・9.7)。純 C の部分(`http_guard_video_accept`)は、ホストでテスト・ファズ・ミューテーションまで行った。`web_server.c` と `main.c` の結線は、この環境に ESP-IDF がなく、**ビルドしていない**(6.4)。

### 6.1 変更の要点

- `http_guard_video_accept(query, query_len)`(`components/http_guard`、純 C): `/video` の URI のクエリ部(`?` の後ろ)から、受け取れる JPEG 以外のコーデックのビットマスク(`HTTP_GUARD_VIDEO_ACCEPT_H264 = 1u`)を返す。規則(`http_guard.h` に書いた。判断した点を含む):
  - `&` で区切った `key=value` のうち、key が `accept` と**完全に一致**するものだけを見る(`xaccept`、`accept2`、`Accept`、`?accept` は別のキー)。value は `,` 区切りで、各要素が `h264` と**完全に一致**するときだけ真(大文字小文字を区別する。`H264`、前後の空白は未知)。未知の値と空の要素は無視する。
  - **`accept` キーが複数あるときは、どれかに `h264` があれば真(OR。順序に依存しない)**。
  - パーセントエンコードと `+` は解釈しない(`%68264` は未知の値)。
  - `query == NULL`、長さ 0、**長さが `HTTP_GUARD_VIDEO_QUERY_MAX`(512)を超える**、**範囲の中に NUL がある**ときは 0(MJPEG だけ。安全側)。`query_len` を超えては 1 バイトも読まない。
- `main/Kconfig.projbuild`: `VIDEO_CODEC_H264_ALLOWED`(既定 `n`)、`VIDEO_CODEC_UPGRADE_DWELL_MS`(5000)、`VIDEO_CODEC_IDR_MIN_INTERVAL_MS`(1000)、`VIDEO_CODEC_KEY_WAIT_MS`(5000)。後ろの 3 つは `H264_ALLOWED` が `y` のときだけ現れる(範囲は 0〜60000。0 は、それぞれ `video_codec_policy` の「待たない」、`ws_video_core` の「まとめない」「閉じない」)。
- `web_server.c`: `web_server_set_video_source()`、`ws_video_post_handshake_cb` で `ws_video_core_on_connect()` の直後に、ロックの下で `ws_video_core_set_accepts_h264()`(申告は `ws_video_core` の `accepts_h264[]` に一本化した。7 章)、`ws_video_task` の各 tick で(`H264_ALLOWED = y` のときだけ)`video_codec_policy_update()` を、ロックの下で呼ぶ。コーデックが変わったら、**ロックの外で** `source->set_codec()`。`request_keyframe` は `source->request_keyframe` に包む(tick から、ロックの下で呼ばれる)。`H264_ALLOWED = n` では、方針も `video_status_get()` も `set_codec()` も呼ばず、`ws_video_core_config_t` の新しい 3 項目は 0 / `NULL`(**従来と同じ設定**)。
- `main.c`: 映像ソースを選んで `start` を呼ぶ箇所の後ろ(テストパターンは `web_server_start` の前、CSI は後)で `web_server_set_video_source()` を呼ぶ。

### 6.2 テスト

| ファイル | 内容 | 件数 |
|---|---|---|
| `test/host/http_guard/test_video_accept.c`(新規) | 基本形、`accept` なし・空・未知の値、大文字小文字、キーの完全一致(前後の 1 文字、`Accept`、`?accept` など)、複数のキーと組(OR)、パーセントエンコードを解釈しないこと、非 ASCII のバイト、NULL・長さ 0、長さの外を読まないこと(**入力は長さちょうどのヒープ領域に置く**。ASan が 1 バイトの範囲外の読みも検出する)、範囲の中の NUL、長さの上限の境界(512 と 513、`(size_t)-1`)、上限の中の長い値 | 15 |
| `test/host/http_guard/test_video_accept_fuzz.c`(新規) | 固定シードの疑似乱数(accept の綴り、`h264`、`= & , %`、NUL などの部品の列 20 万件、NUL を混ぜた 10 万件、全 256 値のランダムなバイト列 6 万件)を、**別の書き方のオラクル**(NUL 終端のコピーにして `strchr` / `strcmp` で分ける)と比べる。有効なクエリの、すべての長さの接頭辞も確認。ASan / UBSan | 4 |

`ctest`: **68 件 -> 70 件**(新しい実行ファイル 2 つ)、すべて成功(`-Wall -Wextra -Werror`、ASan / UBSan)。`node tools/test/check_video_viewer.js` も成功。

### 6.3 ミューテーションテスト

```
python3 tools/test/mutate_video_core.py <repo> <スクラッチ> tools/test/video_core_mutants.py <結果.json> H57 H58 ... H99
run: 43 killed: 38 survived: 5 build_error: 0      (ベースラインは 3 つのグループとも全テスト成功)
```

`http_guard.c` の `http_guard_video_accept` と `hg_accept_value` に、43 個(`H57`〜`H99`)。1 回の実行で、生き残りはすべて同値だった(テストを足して殺したものはない)。既存の 195 個(`V` / `H01`〜`H56` / `K` / `F`)は、コードを変えていないので、再実行していない(ベースラインの 3 グループは全テスト成功)。`H84` と `H98`(次の区間・要素の開始位置が進まない)は、無限ループになり、テストのタイムアウト(120 秒)で殺した。

| グループ | 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|---|
| http_guard.c(P7: `http_guard_video_accept`) | 43 | 38 | 5 | 0 |

#### 6.3.1 同値のミュータント(5 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| H58 | accept: query_len == 0 check removed (equivalent) | `query_len == 0` の検査を外しても、長さ 0 では NUL の走査も区間の走査も 1 回も回らず、`mask` は 0 のまま返る。`!query` の検査(H57)が、NULL を守る。冗長な二重の検査。 |
| H66 | accept: segment loop <= (equivalent) | 区間のループが `start <= query_len` になっても、`start == query_len` のときは空の区間 (`end == start`、`=` なし) で、何も起きず `start` が進んで終わる。 |
| H71 | accept: '=' search runs past the segment (equivalent) | `=` の探索が区間の外 (`query_len` まで) に伸びても、見つかった位置は `end` 以上になり、直後の `eq < end` の検査で捨てられる。結果は同じ (走査が長くなるだけ)。 |
| H96 | value: mask assigned instead of or-ed (equivalent) | `hg_accept_value` の `mask` は、0 から始まり、このビットしか立てない。or-assign でも assign でも同じ値。 |
| H97 | value: token loop <= (equivalent) | 値のループが `start <= n` になっても、`start == n` では空の要素 (長さ 0 は `h264` の長さ 4 と一致しない) で、何も起きず `start` が進んで終わる。 |

#### 6.3.2 殺したミュータントの一覧(38 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ。`:crash/rc=1` は、Unity の失敗行を解析できずに、終了コード 1 だった実行ファイル(アサーションの失敗、または ASan / UBSan)。`:timeout` は、120 秒で終わらなかったもの。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| H57 | accept: NULL query not checked | `test_video_accept:crash/rc=1` |
| H59 | accept: length limit removed | `test_video_accept:crash/rc=1` |
| H60 | accept: length limit > becomes >= | `test_video_accept:crash/rc=1` |
| H61 | accept: length limit allows one more | `test_video_accept:crash/rc=1` |
| H62 | accept: embedded NUL not rejected | `test_video_accept_fuzz:crash/rc=1` |
| H63 | accept: NUL scan reads one past the end | `test_basic_h264`, `test_case_sensitive` |
| H64 | accept: NUL scan skips the last byte | `test_video_accept_fuzz:crash/rc=1` |
| H65 | accept: NUL stops the scan but parsing goes on | `test_video_accept_fuzz:crash/rc=1` |
| H67 | accept: segment separator ';' instead of '&' | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H68 | accept: segment end not bounded by query_len | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H69 | accept: segment end bound <= query_len (reads one past the end) | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H70 | accept: key/value separator ':' instead of '=' | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H72 | accept: 'has =' check removed | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H73 | accept: key length check removed | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H74 | accept: key length == becomes >= (prefix key accepted) | `test_key_must_match_exactly`, `test_video_accept_fuzz:crash/rc=1` |
| H75 | accept: key length == becomes <= (shorter key compared) | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H76 | accept: key compared case-insensitively | `test_case_sensitive`, `test_video_accept_fuzz:crash/rc=1` |
| H77 | accept: key spelled differently ('accepx') | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H78 | accept: key compared over 5 bytes only (accept? matches) | `test_case_sensitive` |
| H79 | accept: value of a later accept key replaces the earlier one | `test_multiple_keys_and_pairs`, `test_video_accept_fuzz:crash/rc=1` |
| H80 | accept: only the first segment is used | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H81 | accept: value length one too long (includes the separator) | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H82 | accept: value starts at '=' (not after it) | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H83 | accept: value ignored (result always 0) | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H84 | accept: next segment starts at the '&' (hangs) | `test_video_accept:timeout`, `test_video_accept_fuzz:timeout` |
| H85 | accept: next segment skips one character | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H86 | value: token separator ';' instead of ',' | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H87 | value: token end not bounded by n | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H88 | value: token end bound <= n (reads one past the end) | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H89 | value: token length check removed | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H90 | value: token length == becomes >= (prefix match) | `test_unknown_values_ignored`, `test_multiple_keys_and_pairs` |
| H91 | value: token length == becomes <=  | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H92 | value: compared case-insensitively | `test_case_sensitive`, `test_video_accept_fuzz:crash/rc=1` |
| H93 | value: token spelled differently ('h265') | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H94 | value: compared over 3 bytes only (h26x matches) | `test_unknown_values_ignored`, `test_multiple_keys_and_pairs` |
| H95 | value: wrong bit | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |
| H98 | value: next token starts at the ',' (hangs) | `test_video_accept:timeout`, `test_video_accept_fuzz:timeout` |
| H99 | value: next token skips one character | `test_video_accept:crash/rc=1`, `test_video_accept_fuzz:crash/rc=1` |

### 6.4 確認したこと / 未確認

**確認したこと(ホスト)**
- 上のテストとミューテーション。
- **`web_server.c` の新しいコード(`ws_video_request_keyframe`、`ws_video_codec_update_locked`、`ws_video_codec_apply`、`ws_video_tick_once`、`ws_video_get_accept`、`web_server_set_video_source`)を、そのまま(ファイルから切り出して)、スタブの FreeRTOS / esp API と実物の純 C コンポーネント(`ws_video_core`、`video_codec_policy`、`video_status`、`http_guard`)に対して、`-Wall -Wextra -Werror`、ASan / UBSan でコンパイルして実行した**(スクラッチの確認用で、コミットしていない)。確認した項目: 5 秒の待ちの後の昇格、`accept` のないクライアントが来たときの即時の降格、閉じる要求中のクライアントを数えないこと、`set_codec` の呼び出しがロックの外、`request_keyframe` がロックの下、`set_codec` の失敗(昇格の失敗は H.264 を止める、降格の失敗は毎 tick 再試行)、ソースが `NULL` / MJPEG だけ / `set_codec` が `NULL`、ソースを後から登録、解像度が上限を超えるとき、スロットの再利用で前の `accept` が残らないこと、`H264_ALLOWED = n` で `set_codec` も `video_status_get` も `request_keyframe` も呼ばれないこと。
- `http_guard` の Origin・Host・Cookie の検査は、URI を使っていない(ヘッダーの `Host`、`Origin`、`Cookie` と、`http_guard_decide` の列挙型の引数だけ)。クエリ付きの `/video?accept=h264` でも、検査の入力は変わらない。`web_headers_allow_for_uri` と `web_assets_find` は、`?` より前だけを見る。

**未確認**
- **IDF のビルド**(`web_server.c`、`main.c`、`Kconfig.projbuild`)。使った API は `httpd_req_get_url_query_str`、`ws_video_core` / `video_codec_policy` / `video_source_t` の既存の関数・フィールドで、`httpd_req_get_url_query_str` は、この環境にヘッダーがなく、IDF の公開 API の知識だけで書いた(`TODO(hw-verify)`)。
- `TODO(hw-verify)`: `/video?accept=h264` が、`/video` の URI ハンドラーに一致すること(esp_http_server が、URI の照合でクエリを除くこと)。`ws_post_handshake_cb` の中で、`req->uri` が有効なこと。
- `TODO(hw-verify)`: `csi_h264` の `set_codec()` が一時的に失敗しうるか(失敗した昇格は、再起動まで H.264 を止める)。
- 既定値(`VIDEO_CODEC_UPGRADE_DWELL_MS` = 5000、`VIDEO_CODEC_IDR_MIN_INTERVAL_MS` = 1000、`VIDEO_CODEC_KEY_WAIT_MS` = 5000)は、実測に基づかない(`TODO(hw-verify)`)。

## 7. P7: H.264 を申告していないクライアントには、H.264 のフレームを配らない(`ws_video_core` の `accepts_h264`)

作成: 2026-11-17(PR #124 のレビュー指摘への対応)。**問題**: H.264 の配信中に、`accept=h264` のない接続(古いビューア)が来ると、`ws_video_task` は `ws_video_core_tick` を先に呼ぶので、その接続にも、`frame_hub` の最新の H.264 のフレーム(IDR を含む)が配られた。降格の `set_codec` は非同期で、コーデックが切り替わるまでの間も、`frame_hub` に H.264 のフレームが残る。方針の判定を tick の前に移すだけでは直らない(`ws_video_core` は、クライアントが H.264 を受け取れるかを知らない)。

### 7.1 変更の要点

- `ws_video_core_t` にクライアントごとの `accepts_h264[]`(**接続したときは false**。`on_connect` のシグネチャは変えない)。`void ws_video_core_set_accepts_h264(core, fd, accepts)` を足した(`web_server.c` は `on_connect` の直後に、同じロックの下で呼ぶ。未知の fd・NULL・同じ値は何もしない)。`on_close` と `on_connect` で false に戻す(再利用されるスロットに、前のクライアントの値を残さない)。
- `ws_video_core_tick`: `frame->codec == H264` で、そのクライアントが `accepts_h264 == false` のとき、**そのフレームを配らず**(`hub_release`、`ws_clients_end_send`、`continue`。キーフレーム待ちの P の破棄と同じ作法)、`need_key`・`last_seqs`・`last_codec`・要求・待ちの起点に触れない。MJPEG(H.264 以外の値を含む)は、従来どおり配る。
- 申告なしのクライアントには、`need_key` も、`request_keyframe` の要求も、`key_wait_timeout_ms` の切断も働かない: `mark_need_key` は、申告なしでは何もしない。
- **申告なし -> あり**: 接続時の規則 (a)(`need_key` を真にして、要求を溜める)を、そのとき適用する(接続時の `mark_need_key` の呼び出しは、ここに移したので `on_connect` から消した。ミュータント K20 は、対象のコードがなくなったので、定義から外した)。**あり -> なし**: そのクライアントの `need_key` と待ちの起点を捨てる(溜まった要求は、誰も待たなくなれば、tick の最後に捨てられる)。
- `web_server.c`: `s_ws_video_accept[]` はやめて、`ws_video_core` の `accepts_h264[]` に一本化した(二重の状態を持たない)。`video_codec_policy` に渡す `h264_client_count` は、コアの `accepts_h264[]` から数える(ACTIVE かつ `close_requested` でないもの)。`ws_video_close_fn` での 0 戻しは不要(コアが `on_close` で戻す)。
- **既存のテスト**: `test_ws_video_core_keyframe.c` の接続の補助関数 `connect_fd` に、1 行 `ws_video_core_set_accepts_h264(&core, fd, true)` を足した(このファイルのテストは、H.264 を受け取れるクライアントを扱う。接続したときは申告なしになったため)。ほかの既存のテストは、書き換えていない(配るフレームがすべて MJPEG なので、従来と同じ)。

### 7.2 テスト

| ファイル | 内容 | 件数 |
|---|---|---|
| `test/host/ws_video_core/test_ws_video_core_accepts.c`(新規) | (a) 申告なしには、H.264 の IDR も P も配られず、MJPEG は配られる(接続の既定が申告なし、未知のコーデックの値、同じフレームの release は 1 回)、(b) 申告あり・混在(同じフレームを、申告ありには配り、なしには配らない)、(c) 申告なしでは、`request_keyframe` が呼ばれない・20 秒の H.264 のフレームと seq の飛びでも `key_wait_timeout_ms` で閉じられない、MJPEG の seq の飛びでも要求しない、要求は待っている申告ありのクライアントからだけ、(d) 途中で `set_accepts_h264(true)`(接続時の規則を適用、IDR から配る)、`false` に戻す(待ちと溜めた要求を捨てる、5 秒を超えても閉じない)、同じ値は何もしない、(e) スロットの再利用(`on_close`、送信中の `on_close`、`on_close` なしの再接続)で前の値を引き継がない、(f) NULL・未知の fd・閉じた後の fd・ほかのクライアントに触れない、(g) 不変条件ファズ | 20 |

ファズ(`test_fuzz_accepts_invariants`): 固定シード 300 個 x 1500 ステップ。クライアントの数、`idr_min_interval_ms`、`key_wait_timeout_ms`、`request_keyframe` の有無をシードごとに変え、接続(申告の有無)、申告の切替、切断、フレームの公開(MJPEG / P / IDR、seq の飛び)、tick(時刻を大きく進めることもある)、送信の完了をランダムに行う。各ステップで検査する不変条件(モデルとは別に、配られた中身を見る): **申告なしのクライアントに、H.264 のフレームが配られない**、H.264 の P は、直前に配ったフレームが H.264 の seq - 1 のときだけ、一度も申告していないクライアントが閉じられない、**要求が出た時点で、待っている申告ありのクライアントがいる**、申告の状態がモデルと一致、申告なしは `need_key` でない、ハブの保持・release の整合。実行結果の例: H.264 の配信 7012、申告なしへの MJPEG の配信 18477、要求 6014、申告の切替 7238、申告ありの待ちの期限切れ 135。

`ctest`: **70 件 -> 71 件**(新しい実行ファイル 1 つ)、すべて成功(`-Wall -Wextra -Werror`、ASan / UBSan)。

### 7.3 ミューテーションテスト

`ws_video_core.c` のグループ(`V`、`K`)を、最初から全部実行し直した(`ws_video_core.c` のミュータントには、`test/host/ws_video_core/` の実行ファイルを全部実行する)。`K63`〜`K85`(23 個)を足し、`K20` を外した。

```
python3 tools/test/mutate_video_core.py <repo> <スクラッチ> tools/test/video_core_mutants.py <結果.json> <ws_video_core のミュータントの ID>
ws_video_core.c のグループ: 137 個を実行、118 個を殺した、19 個が生き残った、ビルドエラー 0      (ベースラインは 3 つのグループとも全テスト成功)
```

(実行は 2 回に分けた。最初の実行が、ミュータント K65 の置換元の文字列が古く `BAD EDIT` で止まったため、K65 の定義を直して、K65〜K85 だけを実行し直した。前半は V01〜K64 の結果。)

| グループ | 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|---|
| ws_video_core.c 全体 | 137 | 118 | 19(既存 14 + 新規 5) | 0 |
| うち新しい K63〜K85 | 23 | 18 | 5 | 0 |

既存の 14 個(V11、V16、V17、V18、V19、V28、V29、V31、V35、V43、V57、K19、K21、K24)は、前と同じで、同値の理由も同じ(3.1、5.3.1)。K21 と K24 は、接続時の `need_key` を、申告時(`set_accepts_h264(true)`)に移したが、理由は変わらない(最初に配るフレームの (c) の判定は、すでに真の `need_key` に対する何もしない操作)。K21・K24 の置換元の文字列は、接続時のコードの変更に合わせて直した。`http_guard.c`(`H`)と `frame_hub.c`(`F`)は、コードを変えていないので、再実行していない。

#### 7.3.1 同値のミュータント(新規 5 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| K63 | accepts: mark_need_key also marks non-accepting clients | `mark_need_key` の「申告なしは何もしない」の検査は、いまの tick の構造では観測できない、二重の防御。申告なしのクライアントで `mark_need_key` が呼ばれるのは (b) の seq の飛びだけ (H.264 は、その前の検査で配らずに `continue` する。接続時の呼び出しは、7.1 で消した)。その直後に、MJPEG のフレームを必ず配って `need_key` を偽に戻す (配れなければ閉じる)。`flush_keyframe_request` は tick の最後に「待っているクライアントがいなければ、要求を捨てる」ので、要求は出ない。検査は、要求を溜める唯一の場所で『申告なしは要求しない』を明示するために残した。 |
| K67 | accepts: set(NULL) not handled | `ws_clients_find(&core->clients, fd)` の `clients` は `ws_video_core_t` の先頭のメンバーなので、`core == NULL` のとき `ws_clients_find` の引数が NULL になり、NULL を検査して -1 を返す。`core == NULL` の検査は、構造体の並びに依存しないための、二重の防御。 |
| K74 | accepts: true | `need_key_since_ms` は `key_waiting` が真のときだけ参照する (待ちが始まるときに設定し直す)。`key_waiting` を偽にした後の値は、誰も読まない。 |
| K84 | accepts: filter updates last_seqs (the later MJPEG frame looks continuou | 申告なしのクライアントの `last_seqs` は、(b) の判定 (`seq != last_seqs + 1`) にだけ使われ、(b) は申告なしでは何も起こさない (`mark_need_key` は何もしない)。申告ありになるときは、`set_accepts_h264` が `need_key` を真にし、最初に配ったフレームが `last_seqs` を更新する。 |
| K85 | accepts: filter sets last_codec to H.264 | 申告なしのクライアントの `last_codec` は、(c) の判定 (MJPEG から H.264 への切替) にだけ使われ、(c) は申告なしでは何も起こさない。申告ありになるときは、`set_accepts_h264` が `need_key` を真にし (既に真なら、(c) は何もしない)、最初に配ったフレームが `last_codec` を更新する。 |

#### 7.3.2 殺したミュータントの一覧(新規 18 個)

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| K64 | accepts: mark_need_key condition inverted (only non-accepting are marked | `test_accepting_client_is_unchanged`, `test_requests_come_only_from_accepting_clients` |
| K65 | accepts: connect does not reset the flag | `test_slot_reuse_without_close_notification_resets_on_connect` |
| K66 | accepts: on_close does not reset the flag | `test_slot_reuse_after_close_does_not_inherit_accept`, `test_close_while_sending_resets_accept_and_new_client_waits_for_the_slot` |
| K68 | accepts: set(unknown fd) writes slot -1 | `test_unknown_fd_and_null_do_nothing` |
| K69 | accepts: set with the same value is not a no-op | `test_set_same_value_is_a_noop` |
| K70 | accepts: set does not store the flag | `test_accepting_client_is_unchanged`, `test_mixed_clients_same_frames` |
| K71 | accepts: false | `true does not apply the connect rule  -> test_accept_set_later_applies_connect_rule`, `test_set_does_not_touch_other_clients` |
| K72 | accepts: true | `false keeps need_key  -> test_accept_revoked_drops_the_wait_and_the_pending_request`, `test_accept_revoked_drops_a_pending_request` |
| K73 | accepts: true | `false keeps key_waiting (the key-wait close stays armed  -> test_accept_revoked_drops_the_wait_and_the_pending_request` |
| K75 | accepts: set branch inverted | `test_accept_set_later_applies_connect_rule`, `test_accept_revoked_drops_the_wait_and_the_pending_request` |
| K76 | accepts: filter inverted (H.264 only to non-accepting) | `test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg`, `test_no_accept_h264_frame_with_same_seq_as_last_is_still_released_once` |
| K77 | accepts: filter also withholds MJPEG from non-accepting clients | `test_defer_unregister_during_send`, `test_send_failure_releases_and_closes` |
| K78 | accepts: filter withholds H.264 from everybody | `test_accepting_client_is_unchanged`, `test_mixed_clients_same_frames` |
| K79 | accepts: filter lets the IDR through | `test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg`, `test_mixed_clients_same_frames` |
| K80 | accepts: filter only withholds the IDR (P frames pass) | `test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg`, `test_no_accept_h264_frame_with_same_seq_as_last_is_still_released_once` |
| K81 | accepts: filter does not release the frame | `test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg`, `test_no_accept_h264_frame_with_same_seq_as_last_is_still_released_once` |
| K82 | accepts: filter does not end the send (client stays in 'sending') | `test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg`, `test_mixed_clients_same_frames` |
| K83 | accepts: filter does not skip the rest (the frame is delivered anyway) | `test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg`, `test_mixed_clients_same_frames` |

### 7.4 確認したこと / 未確認

- 上のテストとミューテーション。`web_server.c` の新しい部分(`s_ws_video_accept[]` をやめて、コアの `accepts_h264[]` を使う)も、前回の切り出しハーネス(スクラッチ。コミットしていない)で、`H264_ALLOWED` が 1 と 0 の両方で、再確認した(昇格・降格・失敗・スロットの再利用など、6.4 と同じ項目)。
- **未確認**: IDF のビルド(6.4 と同じ)。新しく使った IDF の API はない。
- 降格の後、`set_codec` が効くまで、申告なしのクライアントは、MJPEG のフレームが来るまで何も受け取らない(`frame_hub` の H.264 のフレームは捨てられる)。待ちの長さは、ソースが切り替える速さによる(`TODO(hw-verify)`: `csi_h264` の `set_codec` が効くまでの時間と、その間の申告なしのクライアントの画面の凍結)。
