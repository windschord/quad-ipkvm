# iso_media のテストとミューテーションテストの結果

作成: 2026-11-19（Issue #140、C-11）。`components/iso_media`（仮想 CD-ROM のメディアの状態。純 C）のホスト単体テストの構成と、`iso_media.c` に対するミューテーションテストの結果。契約は `docs/ISO_MOUNT_DESIGN.md` 5.5、`docs/WEB_API.md` 8.5 / 8.6、`docs/INTERFACES.md` 7.3。`mmc_scsi` との結合のテストと出典は `docs/MMC_SCSI_TESTS.md`。

## 1. 規則と、出典

`iso_media` は SCSI の応答を作らない（それは `mmc_scsi`）。メディアの状態と、通知（UNIT ATTENTION、イベント）の「いつ立てて、いつ消すか」だけを持つ。規則の出典は、次の 2 つ。

- **この設計の文書**（`ISO_MOUNT_DESIGN.md` 5.5、`WEB_API.md` 8.5 / 8.6、Issue #140 とそのコメント）。Web の `mount` / `eject`（`force`）、ターゲットの START STOP UNIT、`prevent_removal` の解除（バスリセット、Web の `force`、`force_eject`）、`ejected_by_target` が次の `mount` / `eject` まで残ること。
- **SCSI の規則**（SPC-4 の UNIT ATTENTION の節、MMC-6 の GET EVENT STATUS NOTIFICATION のメディアクラス）。UNIT ATTENTION を 1 回だけ報告すること、28h/00h（NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED）と 29h/00h（POWER ON, RESET, OR BUS DEVICE RESET OCCURRED）の値、メディアのイベントの符号（1 = 取り外し要求、2 = 新しいメディア、3 = メディアの取り外し）。**これらは、仕様の本文（T10 の MMC-6 / SPC-4）を、この環境のネットワーク制限で読めず、記憶と、実装（QEMU `hw/scsi/scsi-disk.c`、Linux `drivers/scsi/sr.c`）で照合した**（詳細は `MMC_SCSI_TESTS.md` 1 章）。

設計との食い違いと、設計に書かれていない決定は 3 章。

## 2. テストの構成

| ファイル | 内容 | 件数（Unity のテストケース） |
|---|---|---|
| `test/host/iso_media/test_iso_media.c` | 規則ごとの単体テストと、独立に書いた参照モデルとの比較（固定シード、30 万回の操作） | 27 |

`mmc_scsi` のテスト（`test/host/mmc_scsi/`）も、`iso_media` を実物で使う（UNIT ATTENTION、イベント、START STOP UNIT、PREVENT、バスリセットの結合）。

**単体テストが固定すること**

| 規則 | テスト |
|---|---|
| 初期状態は EMPTY、全てのフラグが偽、世代 0。NULL の引数で壊れない | `test_init_is_empty_with_all_flags_clear`、`test_null_arguments_are_safe` |
| EMPTY への `mount`: UNIT ATTENTION（28h/00h）を 1 回だけ、新メディアのイベント、`prevent_removal` は偽 | `test_mount_from_empty_sets_unit_attention_and_new_media_event` |
| 同じイメージの `mount` は `already_mounted`（何も変えない。`force` があっても、防止も保つ） | `test_mount_same_image_is_already_mounted_and_changes_nothing` |
| 差し替え: 新しい id、UNIT ATTENTION が 1 回、新メディアのイベント | `test_mount_replaces_the_image_and_reports_the_change_once` |
| 防止中の差し替えは `locked`（何も立てない）、`force` なら差し替えて防止を解除 | `test_mount_replacement_is_refused_while_prevented_unless_forced` |
| `mount` は `ejected_by_target` を戻す（同じイメージでも、EMPTY なら `mount`） | `test_mount_clears_ejected_by_target` |
| Web の `eject`: 取り外しイベントだけ（UNIT ATTENTION は立てない）。EMPTY の `eject` は冪等で `ejected_by_target` を戻す | `test_eject_removes_media_with_removal_event_but_without_unit_attention`、`test_eject_of_empty_is_idempotent_and_clears_ejected_by_target` |
| 防止中の `eject` は `locked`、`force` なら取り外して防止を解除 | `test_eject_is_refused_while_prevented_unless_forced` |
| ターゲットの取り外し: 防止中は拒否（Web の状態に印を付けない）。取り外すと `ejected_by_target`（次の `mount` / `eject` まで残る。バスリセット、`force_eject`、取り外し要求、PREVENT の操作では消えない）。EMPTY では何も変えない | `test_target_eject_*`（3 件） |
| PREVENT: EMPTY で防止は拒否、解除は受理。Web のビューの `locked` はメディアがあるときだけ | `test_set_prevent_on_empty_is_refused_and_allow_is_accepted`、`test_locked_in_the_web_view_requires_media` |
| バスリセット: 防止の解除、29h/00h の予約、世代 +1、メディアは保つ、コールバックは呼ばない。EMPTY でも 29h/00h。29h が 28h より先 | `test_bus_reset_*`（2 件）、`test_unit_attention_order_is_reset_before_media_change` |
| `force_eject`: 防止を無視して EMPTY、28h/00h と取り外しイベント、世代 +1、コールバックを 1 回（状態の更新後、登録した ctx で）。EMPTY のときは、コールバックと世代だけ（UNIT ATTENTION とイベントは立てない）。コールバックなしでも安全 | `test_force_eject_*`（4 件） |
| 取り外し要求のイベント: メディアがあり、新メディア / 取り外しが報告待ちでないときだけ。取り外し・新メディアが後から上書き | `test_request_eject_*`、`test_event_priority_latest_media_event_wins` |
| 参照モデルとの比較: 11 種類の操作（mount / eject / ターゲットの取り外し / PREVENT / バスリセット / force_eject / 取り外し要求 / UA の取得 / イベントの取得 / 読み出し / 一括の消去）を 30 万回。毎回、全てのフィールドと不変条件（`ejected_by_target` なら EMPTY、EMPTY なら防止は偽）が一致 | `test_random_operations_match_an_independent_model` |

## 3. 設計との食い違い、設計に書かれていない決定

1. **参照を放さない**: Issue #140 の v3 コメントには「`iso_media_bus_reset()` と `force_eject()` も、保持中の参照を放す」とあるが、設計 v4（4.1）と依頼の指示（「参照は放さない」。参照を放すのは `mmc_scsi_command_done()` だけ）に従った。`iso_media` は参照を一切持たない。
2. **`iso_media_mount` は id だけを持つ**（サイズは持たない）。メディアの大きさは `mmc_scsi` のバックエンド（`size_bytes(id)`）から取る。
3. **Web の `eject` と、ターゲットの START STOP UNIT による取り外しでは UNIT ATTENTION を立てない**（設計 5.5: 次のコマンドが NOT READY（メディアなし）になり、取り外しのイベントを立てる、とあるだけ）。`mount`（差し替えを含む）と `force_eject`（LOADED だったとき）は UNIT ATTENTION（28h/00h）。
4. **PREVENT は、メディアがないときに「防止」を受け付けない**（設計に記述なし）。理由: Web の GET は `mounted == null` のとき `locked = false` で、`mount` が EMPTY から防止付きで始まるのを避けるため。`mmc_scsi` は NOT READY（メディアなし）を返す（`TODO(device-check)`）。
5. **世代 `abort_generation`**: 「進行中のコマンドへの中止の印」を、フラグではなく**世代（カウンター）**にした（印を消す手順が要らず、複数のコマンドで取りこぼさない）。`mmc_scsi` が、コマンドの開始時の値と比べる。
6. **`iso_media_request_eject()` を足した**（設計 5.4 の GET EVENT STATUS NOTIFICATION に「取り外し要求」のクラスがあるため）。呼ぶ側（グルー）は、今は決まっていない（`locked_by_target` のときに、ターゲットへ取り外しを促す用途の候補）。**呼ばれなければ、取り外し要求のイベントは出ない**。
7. **UNIT ATTENTION の順序**: 29h/00h（リセット）を先、28h/00h（メディア変更）を後に報告する（SPC-4 の優先順位の記述は、本文で確認できていない。`TODO(device-check)`）。

## 4. ミューテーションテスト

方法: `tools/test/mutate_iso_media.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_media.c` の 1 か所を置換 -> 再ビルド -> `test_iso_media`、`test_mmc_scsi_cmds`、`test_mmc_scsi_data`、`test_mmc_scsi_fuzz` を実行、を繰り返す（ミュータントの定義は `tools/test/iso_media_mutants.py`）。1 つでも失敗（アサーション、ASan/UBSan、クラッシュ、タイムアウト）すれば「殺した」。

```
python3 tools/test/mutate_iso_media.py <repo> <スクラッチ> tools/test/iso_media_mutants.py <結果.json>
```

**結果**（全数: 106 個。ベースラインは全テスト成功）:

| 実行した数 | 殺した数 | 生き残った数（すべて同値と判断） | ビルドエラー |
|---|---|---|---|
| 106 | 101 | 5 | 0 |

経緯: 最初の実行（91 個を殺し、7 個が生き残り、8 個がビルドエラー: ミュータントが -Werror の未使用の警告になった。書き直した）の後に、`test_removal_paths_clear_the_stored_id`（取り外しの全経路で `id` が 0 に戻る。E12）と `test_take_unit_attention_accepts_null_output_pointers`（出力ポインターが NULL でもよい。U10）を足し、全数を最初から実行し直したのが上の結果。

### 4.1 同値のミュータント（5 個）

`iso_media` は「`prevent_removal` が真ならメディアがある」「`MEDIA_REMOVAL` が報告待ちなら、メディアがない（`mount` は `NEW_MEDIA` で上書きする）」「メディアがなければ `id` は 0」という不変条件を、全ての経路で保つ（参照モデルとの比較のテストが、毎回、不変条件を検査する）。次の 5 個は、その不変条件の下で、観測できない。

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| B06 | `mount`: 防止の検査から `m->loaded` を外す | `prevent_removal` が真なら必ず LOADED。条件は同じ |
| B07 | `mount`: 同上（`loaded || prevent`） | 同上 |
| R03 | `request_eject`: 報告待ちの `MEDIA_REMOVAL` を上書きする | `request_eject` はメディアがあるときだけ。メディアがあるとき、報告待ちが `MEDIA_REMOVAL` になることはない |
| G04 | `get_status`: EMPTY でも `id` を報告する | EMPTY のとき `id` は 0（上の `test_removal_paths_clear_the_stored_id` が、構造体を直接見る）。報告しても 0 |
| G06 | `get_status`: `locked` が `loaded` を見ない | `prevent_removal` が真なら必ず LOADED |

これらの防御（`loaded` の検査、`id` の 0 クリア）は、不変条件が将来壊れたときのために残す。

全ての ID、説明、殺したテストは、`mutate_iso_media.py` の結果の JSON に出る。ミュータントの種類: `init`、`mount`（同じイメージ、差し替え、防止、`force`、フラグ、イベント）、`eject`、ターゲットの取り外し、PREVENT、取り外し要求、バスリセット、`force_eject`（コールバック、世代、UNIT ATTENTION、イベント）、ビューの getter、UNIT ATTENTION の取り出し（順序、値）、イベントの取り出し。

