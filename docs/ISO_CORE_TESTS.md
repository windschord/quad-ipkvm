# iso_core のテストとミューテーションテストの結果

作成: 2026-11-19(Issue #139、C-10 の後半)。`components/iso_core`(ワーカーのジョブの契約: キューと INDEX の集合、優先順位、同じイメージのジョブの合流、`iso_core_pick` / `iso_job_step`、`DELETE` の参照待ち、
公開ビューの更新、`iso_upload_tick`、SD の再初期化 `iso_core_sd_reinit`、起動 `iso_core_boot`。純 C。**ロックを持たない、スレッドを作らない、ブロックしない**)のホスト単体テストの構成と、`iso_core.c` に対するミューテーションテストの結果。
契約は `docs/ISO_MOUNT_DESIGN.md` §3.4(状態の遷移、SD の再初期化の手順)、§4.1(ロック)、§4.2(ワーカー)、§6.1、`docs/INTERFACES.md` §7.5。受信の状態機械と `FINISH` / `INDEX` のステップは `docs/ISO_UPLOAD_TESTS.md`。

## 1. 構成と、グルーへの契約

`components/iso_core/include/iso_core.h` の先頭のコメントが、**グルー(A-13 の `iso_worker`、A-15 の HTTP ハンドラー)への契約**(ロックの取り方、ワーカーのループ、SD の再初期化の呼び出し順、ビューのコールバックの規則)。要点:

- `iso_core_*`、`iso_upload_*`(`iso_core_upload()` が返すもの)、`iso_job_step`、`iso_upload_tick` は、`iso_op_mutex` を取って呼ぶ。`hooks.view_update` は `iso_op_mutex` の下で呼ばれ、実装は `iso_state_mutex` を取る(短く、I/O なし、`iso_core` / `iso_upload` を呼ばない)。ビューの `images[].table` は、保持するなら、コールバックの中で `iso_extent_table_ref` する。
- **SD の再初期化**: ① グルーが `iso_state_mutex` だけを取って(`sd.state` を ready 以外に、`iso_media_force_eject()`、`epoch_bump`)、放してから、`iso_op_mutex` を取って `iso_core_sd_reinit(core, remount, ctx)` を呼ぶ。中で、② ジョブの全取り消し → ③ 開いているファイルを捨てる → ④ アップロードを `incomplete` に、SHA-256 の状態を捨てる → ⑤ `remount`(グルーの `f_mount`)→ ⑥ カタログの再構築 → ⑦ `ready` のイメージごとに `INDEX`、`deleting` のイメージの `DELETE`。自動ではマウントしない。
- 戻り値は、`iso_res_t`(`iso_upload.h`。0 = 受理、`ISO_RES_JOINED` = 合流、正 = 拒否)と `iso_err_t`(負。キューのあふれは `ISO_ERR_BUSY`)。

## 2. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_core/test_core_queue.c` | 優先順位(`DELETE`、`FINISH`、`IDLE_CLOSE`、`INDEX`。同じ種類は入れた順)、後から入れた高い優先順位が先に動く、動いているジョブは `pick` が返し続ける、`INDEX` はキューに積まない(入れた順に 1 つずつ)、**キューは 8 でいっぱい、9 つ目は `ISO_ERR_BUSY`**(`DELETE` / `FINISH` は状態を変えずに取り消す。待っているジョブを置き換える分は数えない)、同じイメージの重複する投入の合流、`FINISH` と待っている `IDLE_CLOSE`、**`DELETE` が動いている `FINISH` / `INDEX` に中止の印**を立てて枠を空ける(別のイメージには立てない)、待っているジョブの取り消し、アップロード中の `DELETE`(ファイルを閉じ、SHA-256 を捨て、枠を空ける)、マウント中は拒否(何も変えない)、`meta_corrupt` の削除 | 31 |
| `test/host/iso_core/test_core_jobs.c` | 各ジョブのステップ: `FINISH`(完了、I/O エラーで `incomplete` + 再試行、署名の誤りは DONE で `invalid`)、`INDEX`(表の公開、`part_mismatch` で `invalid` と古い表の解放(READ の参照は残る)、I/O エラーは `ready` の表なし、`meta` 書き込みの失敗)、`IDLE_CLOSE`、**`DELETE`**(`free_bytes` の更新、表なしは待たない、**参照待ち: 待つ間は何も消さない、30 秒(29999 ms / 30000 ms)で `ERROR(ISO_ERR_BUSY)` で `deleting` のまま、時刻の巻き戻りは経過 0**、I/O エラーと続き)、ビューの内容(状態の表示: `ready` で表がなければ `finalizing`、`sha256`、`hidden`、`meta_corrupt`、`sd_state`)、フックなしでも動く | 25 |
| `test/host/iso_core/test_core_reinit.c` | **SD の再初期化の順序**: remount の時点で、キュー・INDEX の集合・動いているジョブ・表・ファイルハンドル・枠・SHA-256 の状態が片づいていて、カタログは古いまま、`uploading` は `incomplete`、READ の参照は残る(遅延解放)。その後、カタログを作り直し、`ready` は INDEX の集合へ、`deleting` は `DELETE` へ、`free_bytes` を採り直し、ビューを更新する。remount / scan の失敗(空のカタログ)、二度の呼び出し、動いている `FINISH` / `DELETE` の取り消し。起動(`uploading` → `incomplete`、`deleting` の再開は最大 8 つ、前の状態を捨てる) | 12 |
| `test/host/iso_core/test_core_tick.c` | `iso_upload_tick`: **経過 299999 ms は積まず、300000 ms で `IDLE_CLOSE`**、重複しない、最後の `PUT` から数える、`finalizing` はタイムアウトしない、**時刻の巻き戻りは経過 0 で基準を引き直す**、キューがあふれたら次の `tick` で再試行、`receiving` → `idle` の境(5000 ms ちょうどは receiving、5001 ms で idle。変わったときだけビューを更新) | 11 |
| `test/host/iso_core/test_core_fuzz.c` | **固定シードのファズ**(下記) | 1 |
| `test/host/iso_core/core_common.h` | 共通の部品(`iso_core` の設定、フック、ワーカーのループの代わりの `drive()` / `run_job()`) | - |

合計 80 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルが 5 個増える)。期待値は手書き。

### ファズ(`test_core_fuzz.c`。固定シード、ASan / UBSan)

40 シード × 150 操作。`PUT`、`begin`、送り切って `finish`、`submit`(`FINISH` / `DELETE` / `IDLE_CLOSE` / `INDEX`)、**ワーカーのステップ(途中で止める。他の操作と交わる)**、`tick`(時刻の飛びと巻き戻り)、MSC の READ の参照の取得と解放、
書き込みの失敗・エラーの注入、電源断の予約、即時の電源断、SD の再初期化、起動。操作のたびに、不変条件を調べる:

- キュー: 長さ <= 8、同じイメージのジョブは 1 つ(動いているジョブと待っている `DELETE`、`IDLE_CLOSE` と `FINISH` の組だけ例外)、`INDEX` の集合に重複がない。
- エクステントの表: カタログの `ready`(または削除待ちの `deleting`)のイメージにだけあり、重複せず、参照 >= 1。
- 枠: `uploading` / `finalizing` は 1 つ以下、`iso_upload_t` の枠と一致、ハンドルは枠の持ち主のものだけ。
- ディスク(実装を使わずに読んだ基準): パーツの中身 = ソースの先頭、`ready` の SHA-256 = ソースから別に計算した値、`invalid` の理由は宣言値の食い違いと署名だけ(電源断や I/O の失敗で `part_mismatch` にならない)。
- ビュー(`iso_core_publish` の後): カタログと表の状態と一致する(`ready` で表がなければ `finalizing` の表示)。
- 最後に、注入と電源断を止め、参照を放して全部のジョブを流し切り、**静止した状態**(`ready` は全部、表がある。`deleting` は残らない。枠は空き、キューは空)を確かめる。

## 3. ミューテーションテスト

方法: `tools/test/mutate_iso_core.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_core.c` の 1 か所を置換 → 再ビルド → 5 個のテストの実行ファイルを順に実行、を繰り返す。ミュータントの定義(`tools/test/iso_core_mutants.py`)は、
`iso_upload` と同じく `tools/test/gen_iso_mutants.py` がソースから機械的に作る(`python3 tools/test/gen_iso_mutants.py core . > tools/test/iso_core_mutants.py`)。

```
python3 tools/test/mutate_iso_core.py <repo> <スクラッチ> tools/test/iso_core_mutants.py <結果.json> -j4
```

RESULTS_PLACEHOLDER

## 4. 設計に書かれていないこと、設計と違うことを、実装で決めた箇所(レビューで確認してほしい)

1. **`DELETE` の「動いているジョブに中止の印」は、同じイメージのジョブだけ**。別のイメージの `FINISH` / `INDEX` を中止しない(設計の文面は、どのジョブとも読める)。同じイメージの待っている `FINISH` / `IDLE_CLOSE` / `INDEX` は取り消す。
2. **ジョブは最後まで動く(横取りしない)**: 数分かかる `INDEX`(16 パーツで十数秒)が動いている間に `FINISH` が入っても、`INDEX` が終わるまで待つ(パーツごとのステップの間で、`pick` は同じジョブを返し続ける)。横取りが必要なら、`INDEX` を集合に戻す形に変える(提案)。
3. **キューのあふれの数え方**: 同じイメージの待っているジョブを置き換える `DELETE` / `FINISH` は、その分を引いて数える。**あふれたら、状態を変えない**(`FINISH` は受付を取り消す(`iso_upload_finish_undo`))。
4. **`iso_upload_tick`(INTERFACES 7.5 の名前)は `iso_core.h` にあり、`iso_core_t` を取る**。時刻は引数 `now_ms`(注入の時計ではなく、呼び出し側が渡す)。**時刻の巻き戻りは、基準(最後の `PUT` の時刻)を `now_ms` に引き直す**(巻き戻った分だけ、タイムアウトが延びるのを避ける)。`DELETE` の参照待ちの基準も同じ。
5. **`iso_core_sd_reinit` は、`remount` のコールバックを引数に取る**(設計 3.4 の ⑤ `f_mount` が、④ と ⑥ の間にあるため)。① はグルーの仕事で、`iso_core` は触らない。remount か scan が失敗したら、カタログは空、ジョブなしで返る(グルーが `sd.state` を `error` にして、後でもう一度呼ぶ)。
6. **起動時の `deleting` の再開は、最大 `ISO_WORKER_QUEUE`(8)個**。あふれた分は `deleting` のまま残り、再度の `DELETE` か次の起動で続ける(自動の再試行は、ワーカーが `ERROR` のイメージを際限なく試すのを避けるため、しない)。
7. **ビューに、マウント中のイメージ(`iso_media`)は含まない**(`iso_media` が持つ。`GET` は、両方を `iso_state_mutex` の下で組み立てる)。`ready` でエクステントの表がないイメージは、`finalizing` と表示する(`INDEX` 待ち・中。枠は使わない)。ビューの `images[].table` は、コールバックの間だけ有効。
8. **ビューの更新のタイミング**: 状態が変わったとき(`submit`、`iso_upload` の遷移、ジョブの終わり、`boot` / `sd_reinit` の終わり、`tick` で `receiving` ↔ `idle` が変わったとき)。ジョブの途中のステップ(`CONTINUE`)では更新しない。`PUT` のたびに(`received` が進むので)更新する。
9. **`DELETE` の完了と `FINISH` の完了で、`free_bytes` のキャッシュを採り直す**(`mount_info`。失敗したら、古い値のまま)。**`INDEX` の完了では採り直さない**。
10. **未実装**: `DELETE` の前の `crosslinked` の照合(`import/` の同じ先頭クラスタ。設計 3.3)は、`iso_catalog_delete_precheck`(C-10a)が持つ。**`iso_core_submit_delete` は呼ばない**(C-12 の `iso_api` が、`iso_core_submit_delete` の前に呼ぶ。呼ばなくても、削除のステップの最初が、同じ照合をして `ERROR(crosslinked)` で止まる)。`rescan` の呼び出しと、その後の `iso_core_mark_unindexed` は、C-12。
