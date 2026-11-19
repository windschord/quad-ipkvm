# msc_bot のテストとミューテーションテストの結果

作成: 2026-11-19（Issue #143、C-13）。`components/msc_bot`（USB Mass Storage の Bulk-Only Transport の状態機械。純 C）のホスト単体テストの構成と、`msc_bot.c` に対するミューテーションテストの結果。仕様と状態遷移表は `components/msc_bot/include/msc_bot.h` の先頭のコメント、設計は `docs/ISO_MOUNT_DESIGN.md` §4.1・§5.1・§9、境界は `docs/INTERFACES.md` §7.3。

## 1. テストの構成

| ファイル | 内容 | Unity のテストケース |
|---|---|---|
| `test/host/msc_bot/test_msc_bot_cases.c` | 13 のケース。(A) 純関数 `msc_bot_decide` / `msc_bot_resolve` の全ケース（42 行の表。手書きの期待値）、(B) 状態機械を通した 13 のケース全部（同期のモック、完了を遅らせるモック、OUT の読み捨てと STALL、バッファ 2048 と 8192）、(C) READ の転送長（`read.blocks x 2048`。`data_len` を見ない） | 20 |
| `test/host/msc_bot/test_msc_bot_data.c` | データ IN のダブルバッファ（2 通りの順序を 1 手ずつ）、転送の大きさの上限、端数、1.2 MB の READ、IN の途中の失敗（MEDIUM ERROR 相当）、OUT の読み捨て・短い OUT・OUT の STALL 方針、注入した関数の拒否（内部エラー）、契約外のイベント、`command_done` のタイミング（最後の SCSI 依頼の完了が、次の `xfer_in` より先）、宣言長に満たない OUT の STALL、転送の世代 | 36 |
| `test/host/msc_bot/test_msc_bot_cbw.c` | CBW の検証（署名、長さ、LUN、CDB 長、予約ビット）と、BOT 6.6.1 の STALL、Reset Recovery（Reset、CLEAR_FEATURE の順序と、回復の前の解除） | 17 |
| `test/host/msc_bot/test_msc_bot_lifecycle.c` | 実行中の Reset / 切断（中止の印、完了を待ってから `command_done`）、再接続、再入（ops の中からのイベント）、完全に同期した環境、`command_done` の回数（15 の終わり方）、`reset_done` / `reset_failed`（CBW の受信の発行の拒否、再接続の後の Reset）、転送の世代（取り消し前の完了を新しい転送に使わない）、`msc_bot_init` の検査（バッファの重なり） | 43 |
| `test/host/msc_bot/test_msc_bot_table.c` | 状態遷移表（10 状態 x 11 イベント）の全てのセルと、状態ごとの派生（EXEC の結果の種類、データの終わり、失敗の後の終わり、実行中の依頼がある RECOVERY）。計 132 行。`CMDf` の行は、保留した READ_BLOCKS の失敗を実際に完了させる（依頼の種類まで検査） | 4 |
| `test/host/msc_bot/test_msc_bot_fuzz.c` | 固定シードのファズ（300 シード x 6000 ステップ + 依頼が必ず完了する 100 シード。各反復で、CBW の受信の発行、CSW が 1 つ増えること、tag の一致を必須にする。乱数は 1 文に 1 回）  | 2 |
| `test/host/msc_bot/mb_common.h` | 共通部品: USB のハードウェアと `iso_msc_task` のモック、ホスト（PC）の役、違反フラグ、検査の道具 | - |

合計 122 件（`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルは 6 つ増える）。全て `-Wall -Wextra -Werror`、ASan/UBSan（`-fno-sanitize-recover=undefined`。`components/msc_bot` のホスト側のコンパイルにも付ける）で実行する。

**モックが検査する、`msc_bot` の義務（違反フラグ）**: 同じ向きの USB の転送を 2 つ同時に発行しない / STALL 中の Bulk に転送を発行しない / 転送が残っている Bulk に STALL を発行しない / CSW は 13 バイト、CBW の受信は 31 バイト、データはバッファからだけで、バッファの大きさ以下 / `iso_msc_task` への依頼は同時に 1 つ / `command_done` は、EXECUTE を依頼した数だけ、実行中の依頼がないときに / READ_BLOCKS は、`blocks x 2048` がバッファに収まり、生きているコマンドの間だけ（スナップショットの参照を放した後に読まない）/ CDB 長 1〜16。`mb_assert_clean()` が全テストの最後に確かめる。

## 2. 出典

BOT の仕様は、一次情報（"Universal Serial Bus Mass Storage Class, Bulk-Only Transport, Revision 1.0", usb.org, 1999-09。`usbmassbulk_10.pdf`）を、この作業の中で取得して読んだ。テストの説明（各ファイルの先頭と、表の各行）に、節を書いてある。

| 節 | 内容 | テスト |
|---|---|---|
| 3.1 | Bulk-Only Mass Storage Reset: STALL の状態とデータトグルを保つ。ステータスステージは、Reset が終わるまで NAK | `test_reset_recovery_*`、`test_reset_alone_does_not_clear_halts`、`reset_done` の呼び出し |
| 5.1 | CBW の形（署名 43425355h、31 バイト、方向のビット、予約、LUN、CDB の長さ 1〜16）。`dCBWDataTransferLength` = 0 のとき方向を無視 | `test_msc_bot_cbw.c`、`test_pure_hn_ignores_direction_bit` |
| 5.2 | CSW の形（署名 53425355h、tag、residue <= H、status 0/1/2） | `mb_assert_csw`、ファズの F5 |
| 5.3.3、5.3.4 | Phase Error の後の Reset Recovery（Reset、CLEAR_FEATURE(HALT) IN、同 OUT） | `test_reset_recovery_conforming_order` ほか |
| 6.2.1、6.2.2 | valid CBW（31 バイト + 署名）、meaningful CBW（予約ビット = 0、LUN が有効、CDB の長さ） | `test_invalid_*` |
| 6.6.1 | CBW が不正: Bulk-IN を STALL、Bulk-OUT も STALL（または読み捨て）、Reset Recovery まで保つ | `expect_invalid`、`test_invalid_cbw_keeps_state_until_reset_recovery` |
| 6.6.2 | 内部エラー: STALL して Phase Error、または Reset Recovery まで両 Bulk を STALL（後者を採用） | 注入した関数の拒否のテスト |
| 6.7.1〜6.7.3 | 13 のケース | `test_msc_bot_cases.c` |

SCSI / MMC の規則（センス、READ のコマンド自体）は、`mmc_scsi`（C-11）の担当。`msc_bot` は、CHECK CONDITION を `bCSWStatus = 1` にするだけで、センスの中身を見ない。

## 3. 設計の判断（BOT の仕様が「してもよい（may）」とする箇所）

仕様が選択を許すところは、次のとおり決めた。**どれも、仕様に違反しない**。ホストの実装（BIOS / UEFI / Windows / Linux）での確認は、`TODO(device-check)`（8 章）。

| 箇所 | 仕様 | 決めたこと |
|---|---|---|
| ケース 2、3（Hn < Di / Do） | 6.7.1: Bulk-IN を STALL してもよい | STALL しない。CSW（Phase Error）をそのまま返す |
| ケース 7（Hi < Di） | 6.7.2: H バイトを送ったなら STALL してもよい | STALL しない。H バイト送って、CSW（Phase Error） |
| ケース 4、5、8、IN の途中の失敗 | 6.7.2: 送った量が H に満たなければ、STALL する（shall） | 「送った量 < H」で動的に決める。STALL した後は、ホストの CLEAR_FEATURE(HALT, IN) を待ってから CSW |
| ケース 9〜13（Ho） | 6.7.3: H バイトを受け取る、または Bulk-OUT を STALL して打ち切る | 既定は、受け取って読み捨てる（`out_stall = false`）。`out_stall = true` で STALL（residue = H）。両方をテストで固定 |
| meaningful でない CBW | 6.4: 応答は未規定 | 不正な CBW と同じ（両 Bulk を STALL）。issue #143 の指示 |
| Reset Recovery の前の CLEAR_FEATURE | 6.6.1: 回復まで、この状態を保つ | RECOVERY の間の CLEAR_FEATURE は無効（再度 STALL。TinyUSB の `MSC_STAGE_NEED_RESET` と同じ）。Reset の後に、IDLE に戻り、STALL が残る間は、CBW の受信を発行しない |
| Phase Error の CSW の後 | 5.3.3.1: ホストが Reset Recovery する | デバイスは、CSW の後に IDLE へ戻り、次の CBW を受ける（拒否しない） |
| 宣言長に満たない OUT | 6.7.3: 宣言長を受け取る、または Bulk-OUT を STALL して打ち切る | 短い受信で打ち切るときは、Bulk-OUT を STALL してから CSW（STALL したので、ホストが OUT の halt を解除するまで、次の CBW の受信は発行しない）。宣言長を受け取ったときは STALL しない |
| Reset の通知 | 3.1: ステータスステージは、Reset が完了するまで NAK | 実行中の依頼の完了を待ち終え、CBW の受信を発行した（または既存の halt の解除を待つ）なら `reset_done`。xfer_out が発行を拒否したら、RECOVERY に入り、`reset_failed`（`reset_done` は呼ばない） |
| 再接続の後に受けた Reset | （BOT の範囲外） | DETACH_WAIT で、再接続（ATT）の後に受けた Reset は記録し、旧い依頼の解放と再接続の準備の後で通知する。再接続の前の Reset と、通知の前の再度の切断は、破棄する |

その他の設計: ダブルバッファ（`buf[0]`、`buf[1]`）。1 回の転送の最大 = バッファの大きさ（2048 の倍数）。読み出しは先読みしない（バッファ 2 つが埋まれば止まる）。`command_done` は、「そのコマンドのために `mmc_scsi` が呼ばれることが、もうない」直後（最後の READ_BLOCKS の完了、または EXECUTE の完了）に呼ぶ。CSW の送信は待たない（ISO の DELETE が待つ参照を、早く放すため）。

## 4. 状態遷移表の固定（`test_msc_bot_table.c`）

表（`msc_bot.h`）の 10 状態 x 11 イベント（ATT、DET、RST、HIN、HOUT、OUTok、OUTer、INok、INer、CMDok、CMDfail）の全てのセルを、1 つ以上の行で固定する（`test_table_covers_every_state_event_cell` が、セルの欠けを検出する）。各行は、標準の設定（状態ごとに、本物の経路で作る）でイベントを 1 つ与え、次の状態、`ops` の呼び出しの記録（例: `done out31 rdone`）、`command_done` の回数の増分、「対応する依頼がないので捨てた」イベントの数の増分、モックのハードウェアの STALL と bot の記録の一致、を確かめる。1 つのセルが状況で複数の遷移を持つもの（IDLE の CBW が有効 / 不正、EXEC の結果の種類、データの終わり、失敗の後の終わり、実行中の依頼がある RECOVERY）は、複数の行で固定する。期待値は、表から手で書いた。

## 5. ファズ（`test_msc_bot_fuzz.c`）

乱数（xorshift、固定シード）で、有効 / 不正な CBW、IN / OUT の完了（成功、失敗、短い、大きい）、依頼の完了（成功、失敗、契約違反の結果）、Reset、切断、接続、CLEAR_FEATURE(HALT)、対応する依頼がない完了、注入した関数の拒否、同期 / 遅延のモック、バッファの大きさ（2048、4096、8192）、OUT の方針を与える。検査（全ステップの後）:

- F1: モックの違反フラグが立たない。イベントの FIFO があふれない
- F2: `command_done` の回数: 依頼した EXECUTE の数 - `command_done` の数 = 生きているコマンド（0 か 1）。`msc_bot` の記録と、bot の `cmd_live` とも一致。**最後に、依頼の完了と切断で片付けた後、数が一致する**
- F3: bot の転送・依頼・halt の記録が、モックのハードウェアの状態と一致
- F4: 状態ごとの形（IDLE は CBW の受信を発行済み、STATUS_SEND は CSW が転送中、RECOVERY は両 Bulk が STALL、RESET_WAIT / DETACH_WAIT は依頼が実行中、DETACHED は何も保持しない、など）
- F5: CSW の署名・tag・status <= 2・residue <= H。status != 2 の IN では、受け取った量 + residue = H。H に満たなければ、CBW の後に Bulk-IN の STALL がある
- F6: 対応する依頼がない完了は、状態も `ops` の呼び出しも変えない

空振りでないことの確認（固定シードなので毎回同じ件数。下限だけを検査）。実測: 有効な CBW 47021、不正な CBW 6409、CSW 22137（成功 9578、失敗 6861、Phase Error 5698）、Reset 54055、切断 35996、接続 89999、対応する依頼がない完了 66395、転送の失敗の注入 2040、IN のデータ転送 9345、OUT のデータ転送 4509、失敗させた READ_BLOCKS 489。全 10 状態を、それぞれ 1.4 万回以上通る（最も少ないのは DETACH_WAIT の 14998）。

## 6. ミューテーションテスト

方法: `tools/test/mutate_msc_bot.py` が、`components/` と `test/` をスクラッチにコピーし、`msc_bot.c` の 1 か所を置換 -> 再ビルド -> `test_msc_bot_table` / `_cbw` / `_cases` / `_lifecycle` / `_data` / `_fuzz` を実行、を繰り返す（ミュータントの定義は `tools/test/msc_bot_mutants.py`。`--check-only` で、`old` の文字列が 1 回だけ現れることを、実行前に確かめられる）。1 つでも失敗（アサーション、ASan/UBSan、クラッシュ、タイムアウト）すれば「殺した」。速さのため、最初に失敗した実行ファイルで止める。

```
UNITY_SRC=<取得済みの Unity> python3 tools/test/mutate_msc_bot.py <repo> <スクラッチ> tools/test/msc_bot_mutants.py <結果.json> [id ...]
```

**結果**（PR #149 のレビュー指摘の修正の後、**ミュータント 387 個を全数**実行した最終の状態）:

| 実行した数 | 殺した数 | 生き残った数（全て同値と判断） | ビルドエラー |
|---|---|---|---|
| 387 | 353 | 34 | 0 |

経緯:
1. 最初の実装（PR #149 の初版）: 356 個を全数実行し、後からテストを足して、殺 327、同値の生き残り 29（最初の全数実行は、殺 306、生き残り 33、ビルドエラー 17。ビルドエラーは、ミュータントの書き方の問題（-Werror の未使用警告）で、`(void)` を足して書き直した）。
2. レビュー指摘の修正（`reset_failed`、転送の世代、再接続後の Reset、バッファの重なり、短い OUT の STALL、`command_done` の順）で、`msc_bot.c` が変わったので、**ミュータントの `old` 文字列を更新し、新しい修正に対するミュータント 31 個（B450〜B480）を足して、387 個を全数、最初から実行した**。ビルドエラーになった 4 個（B390、B451、B467 と、修正前の B463 の書き方）は、書き直して再実行し、殺した。
3. 生き残りのうち、同値でない 1 個（B463: 再接続の前に受けた Reset を、再接続の後に通知する）を殺すテスト `test_reset_before_reattach_is_not_reported_after_it` を足し、再実行して殺した（最後の全数の実行の後に足したテストは、このテストだけ。加算なので、他のミュータントの結果は変わらない）。

レビュー指摘で足したミュータントと、それを殺すテスト（抜粋）:

| ミュータント | 殺すテスト |
|---|---|
| B450〜B456（`reset_failed` の判定、通知の取り違え、NULL） | `test_reset_failed_when_cbw_arm_refused_*`、`test_reset_waiting_for_halt_clear_is_success`、`test_reset_failed_is_optional` |
| B457〜B461（転送の世代） | `test_stale_out_completion_does_not_complete_the_new_cbw_receive`、`test_stale_in_completion_after_cancel_is_dropped` |
| B462〜B468（再接続の後の Reset） | `test_detach_wait_then_attach_reattaches_after_completion`、`test_reset_in_detach_wait_without_attach_is_dropped`、`test_pending_reset_*`、`test_reset_before_reattach_is_not_reported_after_it` |
| B469〜B472、B421（バッファの重なり） | `test_init_rejects_overlapping_buffers` |
| B473〜B475（短い OUT の STALL） | `test_out_short_transfer_ends_data_phase`、`test_out_short_second_chunk_stalls_out`、`test_out_exact_length_does_not_stall_out` |
| B477（`command_done` が `xfer_in` より先） | `test_every_row`、`test_msc_bot_cases.c` のログ |

### 6.1 同値のミュータント（34 個）

多くは、「防御的な二重の検査」（呼び出し側がすでに保証していることを、関数の中でも検査している）を外したもので、到達できる全ての入力で、結果が同じ。

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| B053 | resolve: Hn residue = H | H is always 0 for Hn |
| B072 | reset_data_state: din_failed not cleared | exec_done and din_begin clear it |
| B073 | reset_data_state: cmd_good not set | exec_done sets it |
| B074 | reset_data_state: buffer states not reset | din_begin resets them |
| B080 | enter_recovery: transfers not cancelled | callers have already cleared the pending flag of the failed transfer and the other pipe never has one |
| B084 | enter_recovery: stalls even if already halted (IN) | IN is never halted when recovery is entered |
| B090 | arm_cbw: state check dropped | callers set IDLE first |
| B094 | arm_cbw: out_req_len not set | only DATA_OUT reads it |
| B110 | status_phase: finish_cmd dropped | callers finish earlier |
| B113 | status_phase: data state not reset | next command resets it |
| B114 | status_phase: STALL OUT also when already halted | CBW not accepted while halted |
| B115 | status_phase: STALL IN also when already halted | CBW not accepted while halted |
| B163 | din_pump: sends after a failure | a READY buffer never exists at a failure |
| B164 | din_pump: sends while an IN transfer is pending | the sending buffer is SENDING, not READY |
| B172 | din_pump: command_done even if a command is outstanding | finish_cmd checks it |
| B175 | din_pump: issued >= total | issued never exceeds total |
| B179 | din_pump: end condition >= | sent never exceeds total |
| B198 | din_on_cmd_done: failed buffer kept filling | no read follows a failure |
| B238 | exec_done: invalid dev_dir not normalized here | decide normalizes |
| B239 | exec_done: zero length not normalized here | decide normalizes |
| B254 | exec_done: no-data branch does not finish the command | status_phase finishes |
| B301 | cbw: data state not reset | exec_done sets everything it uses |
| B313 | detach: DETACHED detach not ignored | idempotent |
| B315 | detach: DETACH_WAIT early return dropped | same result |
| B328 | reset: RESET_WAIT reset not ignored | same result |
| B340 | halt: early return for DETACHED/DETACH_WAIT dropped | no effect |
| B358 | xfer IN: completion in other states not counted as stale | in_pending is never set in other states |
| B363 | xfer OUT: completion in other states not counted as stale | out_pending is never set in other states |
| B383 | cmd: DETACH_WAIT re-attach flag kept | on_detach clears it before the next wait |
| B465 | detach: pending reset not discarded (main path) | reset_pending is only ever set in DETACH_WAIT, so it is false there |
| B468 | cmd done in DETACH_WAIT: pending reset notified even without a re-attach | reset_pending implies then_attach |
| B476 | dout_on_done: STALL already halted Bulk-OUT again | OUT is never halted in DATA_OUT |
| B478 | din_pump: command_done before the last read is complete | finish_cmd checks the outstanding command |
| B479 | arm_cbw: already-armed CBW reported as waiting | both are success |

### 6.2 殺したミュータントの一覧（353 個）

「殺したテスト」は、最初に失敗した実行ファイルの、失敗したテストのうち先頭の 2 つ。`test_every_row` は、状態遷移表の全行のテスト（`test_msc_bot_table.c`）。

| ID | 変更箇所 | 殺したテスト（先頭の 2 つ） |
|---|---|---|
| B001 | rd32le: byte 1 shift 8 -> 9 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B002 | rd32le: byte 3 dropped | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B003 | wr32le: byte 1 written from v >> 7 | `test_reset_recovery_conforming_order`, `test_reset_recovery_after_phase_error_without_stalls` |
| B004 | wr32le: byte 3 written from v >> 16 | `test_reset_recovery_conforming_order`, `test_reset_recovery_after_phase_error_without_stalls` |
| B005 | min_u32 returns the max | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B010 | decide: host_len == 1 instead of 0 is Hn | `test_reset_recovery_after_phase_error_without_stalls` |
| B011 | decide: direction flag inverted | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B012 | decide: L = 0 no longer means Dn | `test_pure_all_rows` |
| B013 | decide: invalid dev_dir value not normalized to Dn | `test_pure_all_rows` |
| B014 | decide: case 2 (Hn<Di) not a phase error | `test_reset_recovery_after_phase_error_without_stalls` |
| B015 | decide: case 3 (Hn<Do) not a phase error | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B016 | decide: case 4 (Hi>Dn) a phase error | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B017 | decide: case 7 (Hi<Di) not a phase error | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B018 | decide: case 8 (Hi<>Do) not a phase error | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B019 | decide: case 10 (Ho<>Di) not a phase error | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B020 | decide: case 13 (Ho<Do) not a phase error | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B021 | decide: Hi: L <= H is case 5 | `test_pure_all_rows`, `test_pure_every_case_number_is_reachable` |
| B022 | decide: Hi: L >= H is case 6 | `test_pure_all_rows`, `test_pure_every_case_number_is_reachable` |
| B023 | decide: case 5 sends H instead of L | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B024 | decide: case 6 sends nothing | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B025 | decide: case 7 sends L (truncated) instead of H | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B026 | decide: case 7 sends no data | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B027 | decide: case 9 processed = H | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B028 | decide: case 11 processed = 0 | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B029 | decide: case 12 processed = 0 | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B030 | decide: Ho: L <= H is case 11 | `test_pure_all_rows`, `test_pure_every_case_number_is_reachable` |
| B031 | decide: Ho: L >= H is case 12 | `test_pure_all_rows`, `test_pure_every_case_number_is_reachable` |
| B032 | decide: out_stall policy inverted | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B033 | decide: OUT data length L instead of H | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B034 | decide: OUT data phase has no direction | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B035 | decide: case 1 reported for Hn<Di (case_no) | `test_pure_all_rows`, `test_pure_every_case_number_is_reachable` |
| B036 | decide: case 10 reported as 9 | `test_pure_all_rows`, `test_pure_every_case_number_is_reachable` |
| B040 | resolve: phase error ignored | `test_reset_recovery_after_phase_error_without_stalls` |
| B041 | resolve: phase error reported as Failed | `test_reset_recovery_after_phase_error_without_stalls` |
| B042 | resolve: good/failed swapped | `test_reset_recovery_conforming_order`, `test_reset_recovery_after_phase_error_without_stalls` |
| B043 | resolve: sent not clamped to H | `test_pure_all_rows`, `test_pure_residue_never_exceeds_h` |
| B044 | resolve: IN residue = H | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B045 | resolve: STALL IN also when sent == H | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B046 | resolve: no STALL IN when nothing was sent | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B047 | resolve: never STALL IN | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B048 | resolve: received not clamped to H | `test_pure_residue_never_exceeds_h` |
| B049 | resolve: OUT processed = received always | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B050 | resolve: OUT processed ignores the received amount | `test_pure_all_rows`, `test_all_13_cases_sync_out_stall` |
| B051 | resolve: OUT residue = processed | `test_pure_all_rows`, `test_all_13_cases_sync_discard` |
| B052 | resolve: OUT never STALLs | `test_pure_all_rows`, `test_all_13_cases_sync_out_stall` |
| B060 | finish_cmd: ignores an outstanding command | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B061 | finish_cmd: ignores cmd_live (double done) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B062 | finish_cmd: cmd_live not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B063 | finish_cmd: done count not kept | `test_command_done_exactly_once_for_every_ending` |
| B064 | finish_cmd: command_done not called | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B065 | cancel_xfers: needs both transfers pending | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B066 | cancel_xfers: pending flags not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B067 | cancel_xfers: callback not called | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B068 | stall_ep: IN halt flag not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B069 | stall_ep: OUT halt flag not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B070 | stall_ep: stall_in not called | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B071 | stall_ep: stall_out not called | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B081 | enter_recovery: Bulk-IN not stalled | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B082 | enter_recovery: Bulk-OUT not stalled | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B083 | enter_recovery: command_done not called | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B085 | enter_recovery: state set to IDLE | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B086 | enter_recovery: stalls even if already halted (OUT) | `test_recovery_does_not_stall_an_already_halted_pipe` |
| B091 | arm_cbw: arms twice | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B092 | arm_cbw: arms while Bulk-IN is halted | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B093 | arm_cbw: arms while Bulk-OUT is halted | `test_reset_recovery_conforming_order`, `test_invalid_cbw_between_valid_commands_keeps_counts` |
| B095 | arm_cbw: refusal check inverted | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B096 | arm_cbw: receive length 32 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B097 | arm_cbw: refusal ignored | `test_pending_reset_reports_failure_if_cbw_arm_refused`, `test_reset_failed_when_cbw_arm_refused_immediate_path` |
| B098 | arm_cbw: out_pending not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B100 | csw: signature = CBW signature | `test_reset_recovery_conforming_order`, `test_reset_recovery_after_phase_error_without_stalls` |
| B101 | csw: tag = 0 | `test_reset_recovery_conforming_order`, `test_reset_recovery_after_phase_error_without_stalls` |
| B102 | csw: residue = 0 | `test_all_13_cases_sync_discard`, `test_all_13_cases_delayed_discard` |
| B103 | csw: status = 0 | `test_reset_recovery_after_phase_error_without_stalls` |
| B104 | csw: in_pending not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B105 | csw: in_req_len not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B106 | csw: refusal check inverted | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B107 | csw: length 12 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B108 | csw: refusal ignored | `test_xfer_in_refused_for_csw` |
| B109 | csw: state not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B111 | status_phase: din_failed ignored in the CSW status | `test_in_mid_failure_third_read`, `test_in_mid_failure_first_read` |
| B112 | status_phase: cmd_good ignored | `test_all_13_cases_sync_discard`, `test_all_13_cases_delayed_discard` |
| B116 | status_phase: STALL OUT regardless of the policy | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B117 | status_phase: STALL IN regardless of the need | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B118 | status_phase: does not wait for the unstall | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B119 | status_phase: stalls OUT after the CSW wait (IN stall not applied) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B120 | status_phase: STALL OUT stalls IN | `test_all_13_cases_sync_out_stall`, `test_all_13_cases_delayed_out_stall` |
| B130 | din_begin: non-READ data counted as not issued | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B131 | din_begin: non-READ buffer not ready | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B132 | din_begin: non-READ buffer length 0 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B133 | din_begin: lba not recorded | `test_read10_blocks1_expected2048_datalen0_completes_as_data_in`, `test_read_ignores_data_len_even_if_nonzero` |
| B134 | din_begin: total not recorded | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B135 | din_begin: state not set to DATA_IN | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B136 | din_begin: READ counted as issued | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B140 | din_pump: reads continue after a failure | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B141 | din_pump: reads overlap (no check of outstanding command) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B142 | din_pump: reads past the total | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B143 | din_pump: reads into a buffer that is not free | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B144 | din_pump: chunk not limited by the buffer | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B145 | din_pump: chunk always the buffer size | `test_read_hi_less_than_di_case7` |
| B146 | din_pump: blocks rounded down | `test_read_hi_less_than_di_case7` |
| B147 | din_pump: blocks of 512 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B148 | din_pump: READ lba always 0 | `test_read10_blocks1_expected2048_datalen0_completes_as_data_in`, `test_read_ignores_data_len_even_if_nonzero` |
| B149 | din_pump: READ blocks + 1 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B150 | din_pump: READ into buf[0] always | `test_reset_during_read_with_transfer_in_flight`, `test_fully_synchronous_environment_large_read` |
| B151 | din_pump: READ cap 2048 | `test_read12_two_blocks_exact`, `test_read_hi_greater_than_di_case5` |
| B152 | din_pump: READ kind is EXECUTE | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B153 | din_pump: READ seq not advanced | `test_read_blocks_has_its_own_seq` |
| B154 | din_pump: buffer marked ready at submit | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B155 | din_pump: buffer length = buffer size | `test_read_hi_less_than_di_case7` |
| B156 | din_pump: current buffer index not recorded | `test_read12_huge_blocks_does_not_overflow` |
| B157 | din_pump: lba advances by 1 | `test_fully_synchronous_environment_large_read` |
| B158 | din_pump: issued advances by the buffer size | `test_tail_not_multiple_of_block` |
| B159 | din_pump: fill buffer not alternated | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B160 | din_pump: cmd_outstanding not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B161 | din_pump: READ refusal leaves the command outstanding | `test_command_done_exactly_once_for_every_ending` |
| B162 | din_pump: READ refusal ignored | `test_command_done_exactly_once_for_every_ending` |
| B165 | din_pump: sends a buffer that is not ready | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B166 | din_pump: sent buffer marked free at once | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B167 | din_pump: in_pending not set for data | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B168 | din_pump: in_req_len not set for data | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B169 | din_pump: sends from buf[0] always | `test_reset_during_read_with_transfer_in_flight`, `test_fully_synchronous_environment_large_read` |
| B170 | din_pump: sends the buffer size | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B171 | din_pump: data refusal ignored | `test_xfer_in_refused_for_data_waits_for_outstanding_read` |
| B173 | din_pump: command_done not called on failure at once | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B174 | din_pump: command_done only when failed | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B176 | din_pump: CSW while an IN transfer is in flight after a failure | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B177 | din_pump: failure CSW with sent = 0 | `test_in_mid_failure_third_read`, `test_in_failure_waits_for_inflight_transfer` |
| B178 | din_pump: normal end with sent = 0 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B180 | din_pump: end condition never | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B190 | din_on_in_done: transfer error ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B191 | din_on_in_done: short transfer accepted | `test_in_short_completion_is_error` |
| B192 | din_on_in_done: sent += 1 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B193 | din_on_in_done: buffer not freed | `test_fully_synchronous_environment_large_read`, `test_command_done_exactly_once_for_every_ending` |
| B194 | din_on_in_done: send index not advanced | `test_read12_huge_blocks_does_not_overflow` |
| B195 | din_on_in_done: no pump | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B196 | din_on_cmd_done: failure and success swapped | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B197 | din_on_cmd_done: failure not recorded | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B199 | din_on_cmd_done: buffer not marked ready | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B200 | din_on_cmd_done: cmd_outstanding not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B201 | din_on_cmd_done: no pump | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B210 | dout_next: remaining == 1 is the end | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B211 | dout_next: end with received = 0 | `test_all_13_cases_sync_discard`, `test_all_13_cases_delayed_discard` |
| B212 | dout_next: chunk not limited by the buffer | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B213 | dout_next: out_req_len not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B214 | dout_next: out_pending not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B215 | dout_next: receives into buf[1] | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B216 | dout_next: refusal ignored | `test_xfer_out_refused_for_data_out` |
| B217 | dout_on_done: transfer error ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B218 | dout_on_done: oversized completion accepted | `test_out_oversized_completion_is_error` |
| B219 | dout_on_done: received += requested | `test_out_short_transfer_ends_data_phase` |
| B220 | dout_on_done: full chunk treated as short | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B221 | dout_on_done: short transfer not detected | `test_out_short_transfer_ends_data_phase`, `test_out_short_second_chunk_stalls_out` |
| B222 | dout_on_done: short end with received = 0 | `test_out_short_transfer_ends_data_phase` |
| B223 | dout_on_done: no next chunk | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B230 | exec_done: READ recognized even if the command failed | `test_read_execute_check_condition_no_data` |
| B231 | exec_done: READ length from data_len | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B232 | exec_done: READ length computed in 32 bits | `test_read12_huge_blocks_does_not_overflow` |
| B233 | exec_done: READ length uses 512-byte blocks | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B234 | exec_done: READ is not marked as READ | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B235 | exec_done: READ direction is none | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B236 | exec_done: non-READ data_len ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B237 | exec_done: non-READ direction ignored | `test_all_13_cases_sync_discard`, `test_all_13_cases_delayed_discard` |
| B240 | exec_done: out buffer limit off by one | `test_result_larger_than_out_buffer_is_internal_error` |
| B241 | exec_done: limit applies to READ too | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B242 | exec_done: limit applies to OUT too | `test_device_out_length_larger_than_buffer_is_not_an_error` |
| B243 | exec_done: no limit | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B244 | exec_done: limit violation not an error | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B245 | exec_done: host direction inverted in decide | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B246 | exec_done: out_stall ignored | `test_all_13_cases_sync_out_stall`, `test_all_13_cases_delayed_out_stall` |
| B247 | exec_done: cmd_good always true | `test_all_13_cases_sync_discard`, `test_all_13_cases_delayed_discard` |
| B248 | exec_done: data IN and OUT branches swapped | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B249 | exec_done: READ lba lost | `test_read10_blocks1_expected2048_datalen0_completes_as_data_in`, `test_read_ignores_data_len_even_if_nonzero` |
| B250 | exec_done: IN length = device length | `test_all_13_cases_sync_discard`, `test_all_13_cases_delayed_discard` |
| B251 | exec_done: OUT branch does not finish the command | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B252 | exec_done: OUT state not entered | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B253 | exec_done: OUT total lost | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B255 | exec_done: no-data branch skips the status phase | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B256 | exec_done: cmd_outstanding not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B257 | exec_done: din_pump not called after din_begin | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B260 | cbw: transfer error ignored | `test_invalid_transfer_error_of_the_cbw` |
| B261 | cbw: length not checked | `test_invalid_length` |
| B262 | cbw: signature not checked | `test_invalid_signature_every_byte_and_values`, `test_invalid_cbw_keeps_state_until_reset_recovery` |
| B263 | cbw: length check allows longer | `test_invalid_length` |
| B264 | cbw: length check allows shorter | `test_invalid_length` |
| B265 | cbw: invalid (signature) not counted | `test_invalid_signature_every_byte_and_values`, `test_invalid_length` |
| B266 | cbw: invalid (meaningful) not counted | `test_invalid_lun`, `test_invalid_cdb_length` |
| B267 | cbw: obsolete flag bit 6 not checked | `test_invalid_reserved_flag_bits` |
| B268 | cbw: flag bit 0 not checked | `test_invalid_reserved_flag_bits`, `test_reserved_flag_bits_checked_even_when_length_zero` |
| B269 | cbw: reserved flag bits not checked | `test_invalid_reserved_flag_bits`, `test_reserved_flag_bits_checked_even_when_length_zero` |
| B270 | cbw: reserved LUN bits not checked | `test_invalid_lun` |
| B271 | cbw: reserved LUN bit 4 not checked | `test_invalid_lun` |
| B272 | cbw: LUN 1 accepted | `test_invalid_lun` |
| B273 | cbw: LUN mask 3 bits | `test_invalid_lun` |
| B274 | cbw: LUN not checked | `test_invalid_lun` |
| B275 | cbw: reserved bits of the CDB length byte not checked | `test_invalid_cdb_length` |
| B276 | cbw: reserved bit 5 of the CDB length byte not checked | `test_invalid_cdb_length` |
| B277 | cbw: CDB length 0 accepted | `test_invalid_cdb_length` |
| B278 | cbw: CDB length 1 rejected | `test_valid_cbw_all_cdb_lengths_pass_the_cdb_through` |
| B279 | cbw: CDB length 16 rejected | `test_valid_cbw_all_cdb_lengths_pass_the_cdb_through` |
| B280 | cbw: CDB length 17 accepted | `test_invalid_cdb_length` |
| B281 | cbw: CDB length mask 4 bits | `test_valid_cbw_all_cdb_lengths_pass_the_cdb_through`, `test_invalid_cdb_length` |
| B282 | cbw: tag not recorded | `test_reset_recovery_conforming_order`, `test_reset_recovery_after_phase_error_without_stalls` |
| B283 | cbw: host length not recorded | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B284 | cbw: direction bit inverted | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B285 | cbw: CDB copied from offset 16 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B286 | cbw: CDB length passed as 16 | `test_valid_cbw_all_cdb_lengths_pass_the_cdb_through` |
| B287 | cbw: CDB copied with length 16 | `test_valid_cbw_all_cdb_lengths_pass_the_cdb_through` |
| B288 | cbw: command direction for H=0 not none | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B289 | cbw: command direction swapped | `test_execute_command_fields` |
| B290 | cbw: expected_len not passed | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B291 | cbw: out buffer is buf[1] | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B292 | cbw: out_cap 2048 | `test_execute_command_fields` |
| B293 | cbw: seq not advanced | `test_duplicate_old_completion_after_reset_is_dropped` |
| B294 | cbw: state EXEC not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B295 | cbw: cmd_outstanding not set | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B296 | cbw: cmd_live not set (done never called) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B297 | cbw: refusal leaves cmd_live set | `test_submit_refused_for_execute` |
| B298 | cbw: refusal leaves the command outstanding | `test_submit_refused_for_execute` |
| B299 | cbw: refusal not an error | `test_submit_refused_for_execute` |
| B300 | cbw: exec_submitted not counted | `test_command_done_exactly_once_for_every_ending` |
| B310 | attach: DETACHED does not become IDLE | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B311 | attach: CBW not armed | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B312 | attach: DETACH_WAIT attach not remembered | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B314 | detach: DETACH_WAIT detach keeps a pending re-attach | `test_detach_wait_attach_detach_stays_detached` |
| B316 | detach: transfers not cancelled | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B317 | detach: IN halt flag not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B318 | detach: OUT halt flag not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B319 | detach: IN unstall not called | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B320 | detach: OUT unstall not called | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B321 | detach: unstall skipped when provided | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B322 | detach: the outstanding command is not waited for | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B323 | detach: state stays after detach without a command | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B324 | detach: command_done not called without an outstanding command | `test_fuzz_invariants` |
| B325 | detach: re-attach flag set instead of cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B326 | reset: DETACH_WAIT reset not handled (falls to the default path) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B327 | reset: DETACHED reset not ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B329 | reset: IDLE reset does not notify | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B330 | reset: IDLE reset is handled like the others (CBW receive cancelled) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B331 | reset: transfers not cancelled | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B332 | reset: the outstanding command is not waited for | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B333 | reset: RESET_WAIT not entered | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B334 | reset: command_done not called when nothing is outstanding | `test_reset_in_data_in_with_live_command_but_nothing_outstanding` |
| B335 | reset: CBW not armed after reset | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B336 | reset: reset_done not called after reset | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B337 | reset: halts cleared by reset (Reset must preserve STALL) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B341 | halt: RECOVERY does not re-stall | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B342 | halt: RECOVERY halt cleared (no re-stall, flags cleared) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B343 | halt: RECOVERY re-stalls the other pipe | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B344 | halt: IN halt flag not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B345 | halt: CSW not sent after the unstall | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B346 | halt: OUT halt flag not cleared | `test_reset_recovery_conforming_order`, `test_reset_recovery_clears_in_either_order` |
| B347 | halt: CBW not armed after the unstall | `test_reset_recovery_conforming_order`, `test_reset_recovery_clears_in_either_order` |
| B348 | halt: the unstall of OUT sends the CSW | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B350 | xfer IN: stale completion not detected | `test_fuzz_invariants` |
| B351 | xfer IN: in_pending not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B352 | xfer IN: stale count not kept | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B353 | xfer IN: data completion ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B354 | xfer IN: CSW completion error ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B355 | xfer IN: short CSW accepted | `test_short_csw_completion_is_error` |
| B356 | xfer IN: CSW done does not arm the CBW | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B357 | xfer IN: CSW done does not leave STATUS_SEND | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B359 | xfer OUT: stale completion not detected | `test_fuzz_invariants` |
| B360 | xfer OUT: out_pending not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B361 | xfer OUT: CBW completion ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B362 | xfer OUT: data completion ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B364 | xfer OUT: stale count not kept | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B370 | cmd: seq not checked | `test_duplicate_old_completion_after_reset_is_dropped` |
| B371 | cmd: outstanding not checked | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B372 | cmd: stale count not kept | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B373 | cmd: EXEC completion ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B374 | cmd: DATA_IN completion ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B375 | cmd: RESET_WAIT completion does not finish the command | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B376 | cmd: RESET_WAIT completion keeps cmd_outstanding | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B377 | cmd: RESET_WAIT completion does not return to IDLE | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B378 | cmd: RESET_WAIT completion does not arm the CBW | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B379 | cmd: RESET_WAIT completion does not notify the reset | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B380 | cmd: DETACH_WAIT completion does not finish the command | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B381 | cmd: DETACH_WAIT completion does not detach | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B382 | cmd: DETACH_WAIT re-attach ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B384 | cmd: RECOVERY completion does not finish the command | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B385 | cmd: RECOVERY completion keeps cmd_outstanding | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B390 | dispatch: attach event ignored | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B391 | dispatch: detach event runs reset | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B392 | dispatch: halt event uses IN always | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B393 | dispatch: xfer event uses IN always | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B394 | dispatch: xfer event ok always true | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B395 | dispatch: xfer event len 0 | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B396 | post: queue accepts one too many | `test_event_fifo_overflow_is_counted_not_fatal` |
| B397 | post: overflow not counted | `test_event_fifo_overflow_is_counted_not_fatal` |
| B398 | post: tail computed one off | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B399 | post: nested events processed at once (no FIFO) | `test_stale_out_completion_does_not_complete_the_new_cbw_receive`, `test_fully_synchronous_environment_large_read` |
| B400 | post: in_pump not set | `test_stale_out_completion_does_not_complete_the_new_cbw_receive`, `test_fully_synchronous_environment_large_read` |
| B401 | post: in_pump not cleared | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B402 | post: head advances by 2 | `test_stale_out_completion_does_not_complete_the_new_cbw_receive`, `test_fully_synchronous_environment_large_read` |
| B403 | post: count not decremented | `test_msc_bot_table:timeout` |
| B404 | post: uninitialized bot accepted | `test_msc_bot_lifecycle:crash/rc=1` |
| B410 | init: xfer_in not required | `test_init_validates_config` |
| B411 | init: xfer_out not required | `test_init_validates_config` |
| B412 | init: stall_in not required | `test_init_validates_config` |
| B413 | init: stall_out not required | `test_init_validates_config` |
| B414 | init: cancel_xfers not required | `test_init_validates_config` |
| B415 | init: submit_command not required | `test_init_validates_config` |
| B416 | init: command_done not required | `test_init_validates_config` |
| B417 | init: cbw_buf not required | `test_init_validates_config` |
| B418 | init: csw_buf not required | `test_init_validates_config` |
| B419 | init: buf[0] not required | `test_init_validates_config` |
| B420 | init: buf[1] not required | `test_init_validates_config` |
| B421 | init: overlapping / identical buffers accepted | `test_init_validates_config`, `test_init_rejects_overlapping_buffers` |
| B422 | init: buffer smaller than a block accepted | `test_init_validates_config` |
| B423 | init: buffer not a multiple of the block accepted | `test_init_validates_config` |
| B424 | init: one block buffer rejected | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B425 | init: bot not marked initialized | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B426 | init: NULL bot accepted | `test_msc_bot_lifecycle:crash/rc=1` |
| B427 | init: NULL cfg accepted | `test_msc_bot_lifecycle:crash/rc=1` |
| B428 | init: config not copied | `test_msc_bot_table:crash/rc=1` |
| B429 | api: halt event endpoint lost | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B430 | api: xfer event endpoint lost | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B431 | api: xfer event length lost | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B432 | api: xfer event ok lost | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B433 | api: command result lost | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B434 | api: NULL result accepted | `test_msc_bot_lifecycle:crash/rc=1` |
| B435 | api: halted() swapped | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B436 | api: cmd_live returns outstanding | `test_reset_in_data_in_with_live_command_but_nothing_outstanding` |
| B437 | api: cmd_outstanding returns live | `test_reset_in_data_in_with_live_command_but_nothing_outstanding` |
| B438 | api: state name of IDLE changed | `test_all_13_cases_sync_discard`, `test_all_13_cases_delayed_discard` |
| B439 | api: state name of DATA_IN changed | `test_state_names_exact` |
| B440 | api: unknown state name changed | `test_state_names_exact` |
| B450 | arm_cbw: refusal reported as armed | `test_pending_reset_reports_failure_if_cbw_arm_refused`, `test_reset_failed_when_cbw_arm_refused_immediate_path` |
| B451 | notify_reset: failure never reported | `test_pending_reset_reports_failure_if_cbw_arm_refused`, `test_reset_failed_when_cbw_arm_refused_immediate_path` |
| B452 | notify_reset: waiting for a halt clear reported as failure | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B453 | notify_reset: failure reported through reset_done | `test_pending_reset_reports_failure_if_cbw_arm_refused`, `test_reset_failed_when_cbw_arm_refused_immediate_path` |
| B454 | notify_reset: success reported through reset_failed | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B455 | notify_reset: reset_failed called without a NULL check (crash when omitted) | `test_msc_bot_lifecycle:crash/rc=1` |
| B456 | arm_cbw: halt wait reported as refused | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B457 | cancel_xfers: generation not advanced | `test_stale_out_completion_does_not_complete_the_new_cbw_receive` |
| B458 | post: completion event not stamped with the generation | `test_reset_during_read_with_transfer_in_flight`, `test_stale_out_completion_does_not_complete_the_new_cbw_receive` |
| B459 | dispatch: old-generation completion applied | `test_stale_out_completion_does_not_complete_the_new_cbw_receive` |
| B460 | dispatch: old-generation completion not counted as stale | `test_stale_out_completion_does_not_complete_the_new_cbw_receive` |
| B461 | dispatch: every completion dropped as old | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B462 | reset in DETACH_WAIT: not recorded | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B463 | reset in DETACH_WAIT: recorded even without a re-attach | `test_reset_before_reattach_is_not_reported_after_it` |
| B464 | detach: pending reset not discarded (DETACH_WAIT) | `test_pending_reset_is_discarded_by_detach` |
| B466 | cmd done in DETACH_WAIT: pending reset not notified | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B467 | cmd done in DETACH_WAIT: pending reset always reported as success | `test_pending_reset_reports_failure_if_cbw_arm_refused` |
| B469 | init: adjacent buffers rejected | `test_all_13_cases_large_buffer` |
| B470 | init: only identical buffers rejected | `test_init_rejects_overlapping_buffers` |
| B471 | init: overlap check uses half the size | `test_init_rejects_overlapping_buffers` |
| B472 | init: overlap check applies only when buf[0] is above buf[1] | `test_init_validates_config`, `test_init_rejects_overlapping_buffers` |
| B473 | dout_on_done: short receive not STALLed | `test_out_short_transfer_ends_data_phase`, `test_out_short_second_chunk_stalls_out` |
| B474 | dout_on_done: short receive STALLs Bulk-IN | `test_out_short_transfer_ends_data_phase`, `test_out_short_second_chunk_stalls_out` |
| B475 | dout_on_done: STALL even when the declared length was received | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B477 | din_pump: command_done after xfer_in (early finish removed) | `test_every_row`, `test_cmdf_rows_exercise_the_real_completion_kind` |
| B480 | attach_now: result discarded (arm refusal reported as success after re-attach) | `test_pending_reset_reports_failure_if_cbw_arm_refused` |

## 7. 実行

```
cmake -S test/host -B <scratch>/build && cmake --build <scratch>/build && ctest --test-dir <scratch>/build
```

（`.github/workflows/ci.yml` の host-tests と同じ。msc_bot は ctest が 6 つ増える。）

## 8. TODO(device-check)（実機・実ホストでの確認が要る項目。推測で確定していない）

1. `TODO(device-check)`: **仕様が「してもよい」とする選択**（3 章の表）が、BIOS / UEFI / Windows / Linux のホストで問題ないこと。特に、ケース 2・3・7 で Bulk-IN を STALL しない、ケース 9〜13 で OUT を読み捨てる（既定）、の 2 点。問題があれば、`out_stall = true` に切り替えるか、STALL を足す。
2. `TODO(device-check)`: **Reset Recovery の前に CLEAR_FEATURE(HALT) だけを送るホスト**があるか。`msc_bot` は、RECOVERY の間、再度 STALL する（BOT 6.6.1。TinyUSB の `MSC_STAGE_NEED_RESET` と同じ）ので、Reset を送らないホストは、回復できない。
3. `TODO(device-check)`: **Reset だけを送り、CLEAR_FEATURE を送らないホスト**（BIOS の INT 13h など）。Reset は STALL を解かない（BOT 3.1）ので、`msc_bot` は、STALL が残る間は CBW の受信を発行しない。そのようなホストでは、デッドロックしうる。
4. `TODO(device-check)`: **`cancel_xfers`（発行済みの Bulk の転送の取り消し）を、手段 1（アプリのクラスドライバ）で実現できるか**。TinyUSB は、発行済みの転送を取り消す公開 API を持たない（`usbd_edpt_close` + 開き直しなど）。A-14 の調査項目（`HW_VERIFY` I6）。
5. `TODO(device-check)`: Bulk-Only Mass Storage Reset のコントロール転送のステータスステージを、`reset_done` まで NAK し、`reset_failed` で STALL できるか（BOT 3.1）。できなければ、Reset 直後に CBW が届きうる（`msc_bot` は、完了まで CBW の受信を発行しないので、ホストの CBW は NAK される）。
