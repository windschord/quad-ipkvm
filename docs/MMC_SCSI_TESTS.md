# mmc_scsi のテストとミューテーションテストの結果

作成: 2026-11-19（Issue #140、C-11）。`components/mmc_scsi`（仮想 CD-ROM の SCSI / MMC コマンド。純 C）のホスト単体テスト、ファズ、`mmc_scsi.c` に対するミューテーションテストの結果。契約は `docs/ISO_MOUNT_DESIGN.md` 5.3〜5.5、`docs/INTERFACES.md` 7.3。メディアの状態（`iso_media`）のテストは `docs/ISO_MEDIA_TESTS.md`。

## 1. 出典と、照合の状況（重要）

**一次情報（T10 の MMC-6 / SPC-4 / SBC-3 の本文）は、この環境では読めなかった**（`www.t10.org` などがネットワークの制限で許可されていない。許可されれば、下の「未照合」の項目を、本文で確認し直すこと）。そのため、値は次の 3 種類の根拠で決めた。**条項の番号は、確認できていないので書かない**（文書名とコマンド名・表の名前だけを書く）。

| 記号 | 意味 |
|---|---|
| ◎ | 公開されている実装のソースで、**同じ値を確認した**（下の一覧）。仕様の本文での確認ではない |
| ○ | 仕様（MMC-6 / SPC-4 / SBC-3）の記憶による。実装では確認できていない |
| △ | 確認できなかった、または実装の間で食い違う。`TODO(device-check)`（5 章） |

**照合に使った公開資料**（いずれも 2026-11-19 に取得）

- QEMU `hw/ide/atapi.c`、`hw/scsi/scsi-disk.c`（`master`）: READ TOC の形式 1（`00 0A 01 01` の 12 バイト）、MSF = LBA + 150、GET CONFIGURATION の見出し（データ長 = 全体 − 4、現在のプロファイル、プロファイル一覧のフィーチャー）、モードページ 2Ah（ページ長 0x14）、GET EVENT STATUS NOTIFICATION（見出し 4 バイト、長さの欄 = 記述子の長さ、メディア記述子 4 バイト）、READ DISC INFORMATION（34 バイト、`0E`、URU = `20`）。
- Linux `drivers/scsi/sr.c`: GET EVENT STATUS NOTIFICATION の CDB（ポーリング、クラスは `1 << 4` = メディア、割り当て長 8）、応答の検査（長さの欄 ≥ 4、NEA = 0、通知クラス = 4、イベント符号 1 = 取り外し要求 / 2・3 = メディア変更）、UNIT ATTENTION の扱い、ASC 3Ah = メディアなし。READ TOC の形式を byte 9 のビット 7-6 で指定する古い形式（`sr_cd_check`）を使うこと。
- edk2 `MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassBoot.c`（UEFI の USB マスストレージ）: CD-ROM（デバイス種別 5）に対して、INQUIRY、TEST UNIT READY、READ CAPACITY(10)、READ(10)（ブロック長 2048）、失敗時の REQUEST SENSE だけを使い、MODE SENSE は使わない。UNIT ATTENTION をメディア変更、NOT READY のメディアなしをメディアなし、DATA PROTECT を書き込み禁止として扱う。**最低限の集合で UEFI が動くことの根拠（実機での確認は別）**。READ CAPACITY の最終 LBA が 0 のときは、空のディスクとして扱う（本機は 34,816 バイト以上のイメージだけをマウントするので当たらない）。
- libcdio-devel のメーリングリスト（2020-05、`msg00030`）: MMC-5 の、単一セッションの DVD の TOC で、MSF 形式のリードアウトの上限は FF:3B:4A（255:59:74）。LBA 形式の 1151849 に当たる。**これは、メールが MMC-5 の表を引用している二次情報**。

**コマンドごとの根拠**

| コマンド（オペコード） | 値 | 根拠 |
|---|---|---|
| センスデータ（固定形式、応答コード 70h、18 バイト、追加の長さ 10） | `70 00 key 00×4 0A 00×4 ASC ASCQ 00×4` | ○ SPC-4 の固定形式のセンスデータ |
| センスキーと ASC / ASCQ | NOT READY 02 / 3A 00（メディアなし）、MEDIUM ERROR 03 / 11 00（回復できない読み出しエラー）、ILLEGAL REQUEST 05 / 20 00（無効なコマンド）、24 00（CDB の無効な欄）、21 00（LBA が範囲外）、39 00（保存値は未対応）、53 02（取り外しの防止）、DATA PROTECT 07 / 27 00、UNIT ATTENTION 06 / 28 00 と 06 / 29 00、ABORTED COMMAND 0B / 00 00 | ○ SPC-4 の ASC / ASCQ の表（edk2 と Linux が、3Ah と 28h、書き込み禁止を同じ意味で扱うことは ◎） |
| INQUIRY（12h） | 36 バイト、種別 05h、RMB = 1、SPC-3、ベンダー `QUADKVM `、製品 `Virtual CD-ROM  `、版 `0001`。VPD は 24h/00h | ○ SPC-4 の標準 INQUIRY データ。種別 5 を UEFI が受け付けることは ◎（edk2） |
| TEST UNIT READY（00h）、REQUEST SENSE（03h） | メディアなしは NOT READY 3Ah/00h。DESC = 1 は未対応 | ○ SPC-4。edk2 / Linux の使い方は ◎ |
| UNIT ATTENTION の規則 | INQUIRY と REQUEST SENSE を除く最初のコマンドで 1 回だけ。REQUEST SENSE は、保留中の UNIT ATTENTION を報告せず、消さない | ○ SPC-4（設計 5.5 の指示）。REQUEST SENSE の扱いは △ |
| MODE SENSE(6)/(10)（1Ah / 5Ah） | ヘッダー（4 / 8 バイト、ブロック記述子なし）+ ページ 2Ah（22 バイト、ページ長 0x14）。ページ 3Fh は 2Ah だけ。PC = 3 は 39h/00h | ページ長は ◎（QEMU）。ヘッダーとページの欄の位置は ○（MMC-6 の MM 能力とメカニカルステータスのページ）。ページの中身（DVD-ROM の読み出し、ローディング機構 = トレイ、イジェクト可、ロック可、ロックの状態）は △ |
| START STOP UNIT（1Bh） | LoEj = bit 1、Start = bit 0。イジェクトは防止が掛かっていれば 05h/53h/02h | ○ MMC-6 |
| PREVENT ALLOW MEDIUM REMOVAL（1Eh） | PREVENT = bit 0、PERSISTENT = bit 1（未対応） | ○ MMC-6 / SPC-4 |
| READ CAPACITY(10)（25h） | 最終 LBA（BE32）+ ブロック長 2048（BE32）。4 GiB 超は最終 LBA = 0xFFFFFFFF | ○ SBC-3 / MMC-6。ブロック長 2048 と最終 LBA の使い方は ◎（edk2） |
| READ FORMAT CAPACITIES（23h） | 4 バイトの見出し + 8 バイトの記述子（ブロック数、種別、ブロック長 24 ビット）。メディアなしは 0xFFFFFFFF、種別 3 | ○ MMC-6。メディアなしの記述子は △ |
| READ(10) / (12)（28h / A8h） | 範囲外は 21h/00h。0 ブロックは GOOD（転送なし）。RDPROTECT 非 0、ストリーミングは 24h/00h | ○ SBC-3 / MMC-6 |
| READ TOC/PMA/ATIP（43h） | 形式 0（4 + 8 × 2 バイト。トラック 1 と 0xAA）、形式 1（12 バイト）、形式 2（4 + 11 × 4 バイト。A0、A1、A2、トラック 1）。ADR/CONTROL = 0x14。MSF = LBA + 150。形式の指定は byte 2 の下位 4 ビット（0 のときは byte 9 のビット 7-6）| 形式 1 の 12 バイトと +150 は ◎（QEMU）。byte 9 の古い形式は ◎（Linux の sr）。形式 0 / 2 の欄の位置は ○（MMC-6 の READ TOC の応答）。MSF の上限（255:59:74）は ◎（二次情報）。形式 2 の中身は △ |
| GET CONFIGURATION（46h） | 見出し 8 バイト（データ長 = 全体 − 4、現在のプロファイル 0010h = DVD-ROM）+ フィーチャー 0000h（プロファイル一覧）、0001h（コア）、0003h（取り外し可能メディア）、0010h（ランダム読み出し）。RT = 0 / 1 / 2 | 見出しとプロファイル一覧の形は ◎（QEMU）。コア、取り外し可能メディア、ランダム読み出しの欄は ○ で、バージョンなどが △ |
| GET EVENT STATUS NOTIFICATION（4Ah） | ポーリング形式（POLLED = 1 のみ）。メディアクラス（0x10）。見出し: 長さ = 記述子の長さ（4）、NEA / 通知クラス（04h）、サポートするクラス（10h）。記述子: イベント符号（0 なし、1 取り外し要求、2 新メディア、3 取り外し）、メディアの状態（bit 1 = メディアあり）、0、0 | 見出しの形と「長さ = 全体 − 4」は ◎（QEMU）。Linux の sr が受け付けることは ◎。長さの欄の定義は △ |
| READ DISC INFORMATION（51h） | 34 バイト、`00 20 0E 01 01 01 01 20 00 …` | ◎（QEMU） |
| READ TRACK INFORMATION（52h） | 36 バイト（長さ 0x22）、トラック 1、セッション 1、データ（トラックモード 4、データモード 1）、トラックの大きさ = capacity_blocks | ○ MMC-3 / MMC-6。実装では確認できていない。△ |
| 書き込み系 | DATA PROTECT 07h/27h/00h（WRITE(6/10/12/16)、WRITE AND VERIFY(10/12/16)、WRITE SAME(10/16)、FORMAT UNIT、BLANK） | ○（設計 5.4 の指示。edk2 が DATA PROTECT を書き込み禁止として扱うことは ◎） |
| CDB の長さ | オペコードのグループから決まる長さ（6 / 10 / 12 / 16）以上を受け付ける（パディングを許す）。足りなければ 24h/00h | ○ SPC-4 の CDB の形式 |

## 2. テストの構成

| ファイル | 内容 | 件数（Unity のテストケース） |
|---|---|---|
| `test/host/mmc_scsi/test_mmc_scsi_cmds.c` | メディアの内容に依存しないコマンド（INQUIRY、TEST UNIT READY、REQUEST SENSE、UNIT ATTENTION、MODE SENSE、START STOP UNIT、PREVENT、書き込み系、未対応、CDB の長さと予約ビット） | 40 |
| `test/host/mmc_scsi/test_mmc_scsi_data.c` | READ CAPACITY、READ FORMAT CAPACITIES、READ(10)/(12)、`mmc_scsi_read_blocks`（ゼロ埋め、I/O エラー、`ISO_ERR_EPOCH`、リセットによる中止、64 ビットのオフセット）、参照のリーク、READ TOC、GET CONFIGURATION、GET EVENT STATUS NOTIFICATION、READ DISC / TRACK INFORMATION、全コマンド共通の予約ビット・コントロールバイト・割り当て長 | 61 |
| `test/host/mmc_scsi/test_mmc_scsi_fuzz.c` | 固定シードのファズ（12 シード × 4 万回） | 1 |
| `test/host/mmc_scsi/mm_common.h` | 共通の部品（メモリ上のモックのバックエンド。参照の取得 / 釈放を数え、生きていない参照の使用を検出。イメージ A: 204,900 バイト、B: 102,400 バイト、BIG: 2.5 GiB、BIGP: 5 GiB + 100 バイト、HUGE: 約 18 TiB、C: 5000 ブロック） | - |
| `test/host/iso_media/test_iso_media.c` | `docs/ISO_MEDIA_TESTS.md` | 27 |

**応答のバイト列は、手書きの期待値**（`mmc_scsi.c` の定数や関数を使わない）で比べる。INQUIRY、REQUEST SENSE のセンス、MODE SENSE（6 / 10、ロックの状態あり / なし、変更可能な値）、READ CAPACITY（A、B、BIG、HUGE）、READ FORMAT CAPACITIES（あり / なし）、READ TOC（形式 0 の LBA / MSF、開始トラック AAh、形式 1、形式 2、大きなイメージでの MSF の飽和、分・秒・フレームの計算）、GET CONFIGURATION（メディアあり / なし、RT 0 / 1 / 2、開始フィーチャー）、GET EVENT STATUS NOTIFICATION（新メディア、取り外し、取り外し要求、NEA）、READ DISC INFORMATION、READ TRACK INFORMATION。

**ファズの検査**（全コマンドの後）: (1) out の前後の番兵が壊れない。`data_len <= out_cap` (2) status は GOOD か CHECK CONDITION で、CHECK CONDITION は `data_len = 0`、`read.is_read = 偽`、センスは許可した組だけ。GOOD のセンスは 0 (3) `read.is_read` は、READ(10)/(12) の GOOD のときだけ、`data_len = 0`、`lba + blocks <= capacity_blocks`（64 ビット） (4) 参照は、`blocks > 0` の READ が GOOD のときだけ、ちょうど 1 つ。バックエンドの生きた参照の数と一致。`command_done` の後は 0（冪等）。生きていない参照での read / release が 0 回。実行の途中の失敗でもリークしない。`command_done` を呼び損ねても、次の `execute` が放す (5) **切り詰めの性質**: 同じ状態から、大きな `out_cap` で実行した結果の、先頭の `min(data_len, out_cap)` バイトと一致 (6) `mmc_scsi_read_blocks`: 実データはバックエンドの内容、`size_bytes` 以降は 0。バックエンドの失敗は MEDIUM ERROR で、バッファを 0 にする。バスリセット（実行中を含む）の後は ABORTED COMMAND。固定シードの実測: 実行 48 万回（READ の転送 約 5000 回、ゼロ埋めのバイト 約 18 万、中止 約 300 回、バックエンドの失敗 約 80 回）。

**未初期化の読み出しの検査**: このコンテナーには MemorySanitizer の実行時ライブラリーがないので、(a) `valgrind --track-origins=yes` で `cmds`、`data`、`fuzz` の全てを実行（エラー 0）、(b) `-ftrivial-auto-var-init=pattern` と `=zero` の 2 通りでビルドして、全テストが成功すること、で代えた。

## 3. 設計との食い違い、設計に書かれていない決定

1. **参照を放さない**: Issue #140 の v3 コメントには「`iso_media_bus_reset()` と `force_eject()` も、保持中の参照を放す」とあるが、設計 v4（4.1）と依頼の指示に従い、**放すのは `mmc_scsi_command_done()` だけ**にした。バスリセット / `force_eject` は、世代（`iso_media_abort_generation`）を進め、`mmc_scsi_read_blocks` が、それを見て ABORTED COMMAND（0Bh/00h/00h）で中止する（参照は保持したまま）。
2. **`execute` の先頭で、前のコマンドの参照が残っていれば放す**（`command_done` の呼び損ねの保険）。放すのは `mmc_scsi_command_done` の処理そのもの。BOT 層が必ず呼ぶ契約は変わらない。
3. **バックエンド**: `size_bytes(ctx, id, *out)`、`acquire(ctx, id, *handle)`、`read(ctx, handle, lba, blocks, buf)`、`release(ctx, handle)`（`mmc_scsi.h`）。`iso_media` は id だけを持ち、大きさはバックエンドから取る（`iso_extent` と結線するのは親）。`read` は `size_bytes` 以降の内容を保証しなくてよく、ゼロ埋めは `mmc_scsi_read_blocks`。`acquire` の失敗は、`ISO_ERR_NOT_FOUND` / `ISO_ERR_NO_CARD` なら NOT READY（メディアなし）、それ以外（`ISO_ERR_EPOCH` を含む）は MEDIUM ERROR。
4. **`dir` と `expected_len` は使わない**（13 のケースは `msc_bot`）。OUT のデータを受け取るコマンド（WRITE 系）は、データを受け取らず DATA PROTECT。MODE SELECT は未対応（ILLEGAL REQUEST 20h/00h）。
5. **READ の範囲**: `lba + blocks == capacity_blocks` かつ `blocks == 0` は GOOD（転送なし）。`lba > capacity_blocks`（`blocks == 0` でも）は 21h/00h。`lba + blocks` は 64 ビットで計算する（32 ビットだと巻き戻る）。
6. **PREVENT（防止）はメディアがないとき NOT READY（3Ah/00h）**、解除は GOOD。START STOP UNIT の Start = 1（ロード）は、メディアの有無によらず GOOD。
7. **REQUEST SENSE は、保留中の UNIT ATTENTION を報告せず、消さない**（依頼の「INQUIRY と REQUEST SENSE を除く、次の最初のコマンドで」に従った）。SPC-4 が、REQUEST SENSE で UNIT ATTENTION を返して消す、と定めているかは、本文で確認できていない。
8. **バスリセット / `force_eject` の後は、保持中のセンスを捨てる**（SAM の「リセットでセンスが消える」の記憶による）。
9. **GET EVENT STATUS NOTIFICATION の長さの欄 = 記述子の長さ（4）**（QEMU と Linux の sr に合わせた。`TODO(device-check)`）。
10. **READ TOC の byte 9（SFF-8020i の形式の指定）に対応**（Linux の `sr_cd_check` が使う）。形式 2（フル TOC）は、MSF ビットによらず MSF。
11. **`iso_seams.h` は変更していない**。

## 4. 未実装の項目

- VPD ページ、REPORT LUNS、SYNCHRONIZE CACHE、READ CD、READ SUB-CHANNEL、GET PERFORMANCE、SET CD SPEED、READ DVD STRUCTURE など（ILLEGAL REQUEST 20h/00h）。実機で必要と分かったものだけ足す。
- GET CONFIGURATION のフィーチャー（DVD 読み出し 001Fh、マルチリード 001Dh、CD 読み出し 001Eh、電源管理など）。プロファイルは DVD-ROM（0010h）だけ。
- READ TOC の形式 3（PMA）、4（ATIP）、5（CD-TEXT）。
- 取り外し要求のイベントを立てる呼び出し側（`iso_media_request_eject()` を呼ぶグルー）。
- MODE SENSE のブロック記述子（`DBD` は受け付け、記述子は常に返さない）。

## 5. `TODO(device-check)` の一覧（実機または一次情報で確認する）

| # | 内容 | 場所 |
|---|---|---|
| 1 | GET EVENT STATUS NOTIFICATION の「イベント記述子の長さ」の定義（記述子だけの 4 か、見出しの 2 バイトを含む 6 か） | `cmd_get_event_status` |
| 2 | READ TOC の MSF で、大きなイメージ（リードアウト + 150 が 1,151,999 を超える）を FF:3B:4A に飽和させる規則。LBA 形式のリードアウトが、同じ上限になる実機がある（libcdio のメール）。本機は LBA 形式を飽和させない | `msf_of`、`toc_track_desc` |
| 3 | READ TOC 形式 2（フル TOC）の記述子（A0 のディスク種別 = 0、時刻の欄 = 0）と、MSF ビットによらず MSF にすること。DVD-ROM でのフル TOC の扱い | `cmd_read_toc` |
| 4 | READ TOC の byte 2 と byte 9 の、形式の優先順位（byte 2 が 0 でなければ byte 2） | `cmd_read_toc` |
| 5 | GET CONFIGURATION: コアのバージョン（2）、物理インターフェース（USB = 8）、DBE、取り外し可能メディアのバージョン、ランダム読み出しのブロック化（16）と PP、DVD 読み出しなどのフィーチャーの有無 | `cmd_get_configuration` |
| 6 | モードページ 2Ah の長さ（MMC-3 の 0x14 と、MMC-6 のより長い形）と、各欄（DVD-ROM の読み出し、ローディング機構、ロックの状態） | `build_caps_page` |
| 7 | READ DISC INFORMATION / READ TRACK INFORMATION の、DVD-ROM メディアでの扱い。READ TRACK INFORMATION の長さ（36 か 48）と欄 | `cmd_read_disc_info`、`cmd_read_track_info` |
| 8 | READ FORMAT CAPACITIES の、メディアなしの記述子（種別 3、0xFFFFFFFF） | `cmd_read_format_capacities` |
| 9 | PREVENT のメディアなしでの応答。START STOP UNIT の、メディアなしの Start = 1 と、LoEj = 1 + Start = 1 | `cmd_prevent_allow`、`cmd_start_stop_unit` |
| 10 | REQUEST SENSE と、保留中の UNIT ATTENTION（返して消すか） | `cmd_request_sense` |
| 11 | UNIT ATTENTION の優先順位（29h/00h を 28h/00h より先に報告） | `iso_media_take_unit_attention` |
| 12 | 実機の UEFI / BIOS / Windows / Linux での、最小の集合で足りること（edk2 と Linux の sr のソースでは、足りる見込み） | 全体（`HW_VERIFY` I8、I9） |

## 6. ミューテーションテスト

方法: `tools/test/mutate_mmc_scsi.py` が、`components/` と `test/` をスクラッチにコピーし、`mmc_scsi.c` の 1 か所を置換 -> 再ビルド -> `test_mmc_scsi_cmds` / `_data` / `_fuzz` を実行、を繰り返す（ミュータントの定義は `tools/test/mmc_scsi_mutants.py`）。1 つでも失敗（アサーション、ASan/UBSan、クラッシュ、タイムアウト）すれば「殺した」。

```
python3 tools/test/mutate_mmc_scsi.py <repo> <スクラッチ> tools/test/mmc_scsi_mutants.py <結果.json> [ID ...]
```

**結果**（全数: 439 個を、3 つに分けて実行。ベースラインは全テスト成功）:

| 実行した数 | 殺した数 | 生き残った数（すべて同値と判断） | ビルドエラー |
|---|---|---|---|
| 439 | 437 | 2 | 0 |

経緯: (1) 最初の全数の実行で、26 個が生き残り（多くは、コントロールバイト、予約バイト、2 バイトの割り当て長の上位バイト、`read_blocks` のセンス、4 GiB を超えるイメージの部分的な最後のブロックの検査が、コマンドごとに揃っていなかった）、24 個がビルドエラー（ミュータントが -Werror の未使用の警告になった。書き直した）だった。(2) 殺すテストを足した: 全コマンド共通の表（`test_every_command_rejects_each_reserved_control_bit_and_accepts_vendor_bits`、`test_every_command_rejects_each_reserved_byte`、`test_two_byte_allocation_length_is_honoured_by_every_command_that_has_one`）、`test_read_blocks_zero_fill_of_a_partial_last_block_beyond_4_gib`（5 GiB + 100 バイトのイメージ BIGP）、`test_read_blocks_misuse_senses`、`test_read_toc_msf_minutes_seconds_and_frames`、`test_read_toc_two_byte_allocation_length_and_huge_lba_form` ほか。(3) テストを足した後に、全数を最初から実行し直した（これが上の結果。ビルドエラーだった 3 個は、書き直して、その 3 個だけ実行し直して殺した）。

### 6.1 同値のミュータント（2 個）

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| S15 | `clamp32`: `v > 0xFFFFFFFF` -> `>=` | `v == 0xFFFFFFFF` のとき、どちらの分岐も 0xFFFFFFFF を返す |
| O04 | `msf_of`: `v > 上限` -> `>=` | `v == 上限` のとき、`v = 上限` の代入は同じ値 |

### 6.2 殺したミュータントの代表

全ての ID、説明、殺したテストは、`mutate_mmc_scsi.py` の結果の JSON（`<結果.json>`）に出る。ミュータントの種類: 小さな部品（`zero_bytes`、`control_ok`、`reply` の切り詰め、`capacity_blocks` の切り上げ、`clamp32`）、センスの値とバイト列（キー、ASC、ASCQ、固定形式の欄）、INQUIRY の各バイト、MODE SENSE の各欄とヘッダー、START STOP UNIT / PREVENT の分岐と予約ビット、READ CAPACITY / READ FORMAT CAPACITIES の各欄、READ(10)/(12) の欄の幅・範囲検査・参照の取得と保持、READ TOC の形式 0/1/2・MSF の計算と飽和、GET CONFIGURATION のフィーチャーと RT、GET EVENT STATUS NOTIFICATION の符号と消費、READ DISC / TRACK INFORMATION、ディスパッチ（対応するオペコードと書き込み系を 1 つずつ外す）、`execute` の UNIT ATTENTION とセンスの保持、`command_done` の冪等性、`mmc_scsi_read_blocks` の範囲・中止・失敗・ゼロ埋めの境界。

