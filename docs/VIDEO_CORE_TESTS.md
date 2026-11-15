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

方法: `tools/test/mutate_video_core.py` が、`components/` と `test/` をスクラッチにコピーし、`ws_video_core.c` または `http_guard.c` の 1 か所を置換 -> 再ビルド -> テストを実行、を繰り返す(ミュータントの定義は `tools/test/video_core_mutants.py`)。`ws_video_core.c` のミュータントには `test/host/ws_video_core/` の 2 つの実行ファイル、`http_guard.c` のミュータントには `test/host/http_guard/` の 8 つの実行ファイル(`http_guard` の既存のテストを含む)を実行する。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。

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
