# ISO の仮想メディア（アップロード + USB マウント）の設計 (P8)

ブラウザから ISO をアップロードして microSD に保存し、ターゲット PC に **USB の仮想 CD-ROM** として見せる機能。**この文書が契約**（API は `docs/WEB_API.md` §8、境界は `docs/INTERFACES.md` §7）。担当: 設計・純 C・API・ビューア = Claude、SD・USB・実機 = Antigravity。

> **版**: v4（v3 の独立レビュー: blocker 0、major 8 を反映。httpd のロック待ち、参照の解放の主体、`epoch` の持ち主、`rescan` の `remaining`、パス長、`msc_bot` のスレッドモデル、ワーカーのキュー、ロックと関数の対応表）。v3。v2 の独立レビュー（blocker 1、major 14）を反映した（ロックの階層、MSC の I/O タスク、`truncate` の定義、2 スロットの規則、状態遷移表、ワーカーのジョブ、スナップショットと `epoch`、`msc_bot` など）。初版（PR #135）の独立レビューで、`meta` のレイアウトの重なり、`rename` の原子性の誤り、TinyUSB の標準の MSC が契約を満たさないこと、判定の順序と表の食い違い、長い同期処理が httpd を止めること、などが見つかり、**全面的に直した**（この版）。

## 0. オーナーの決定（2026-11-18）と、この文書での扱い

| 項目 | 決定 |
|---|---|
| 機能 | ISO を**アップロード**し、**USB 経由でマウント**する。ISO は **microSD に保存**する |
| 対象の ISO | **Linux の ISO だけでなく、Windows の ISO も起動できる**こと → **仮想 CD-ROM のエミュレーションが必須**（ディスクとして見せるだけでは足りない。§5） |
| ISO のサイズ | **4 GiB を超える ISO も扱う**（Windows の ISO は約 5〜7 GB） |
| カード | 32GB の microSD。**FAT32 に再フォーマットして使う**（オーナーが PC で行う。デバイスはフォーマットしない。§3） |
| 保存方式 | **FAT32 に、固定サイズのパーツに分割して保存し、SD のセクタを直接読む**（保存方式の案 1。§11） |
| SD の用途 | 計画書の GPIO 割り当て（`PLAN.md` §4.4）に SD はない。**ISO 専用**（回路図で GPIO39〜45 を使うことを確認済み。§1） |

**実装上の判断（オーナー決定の範囲内）**: 案 1 の「連続確保」は、**`f_expand` を使わない**ことにした（§3.1）。理由: 1 GiB の確保で FAT の更新が数百 ms〜秒かかり、**単一スレッドの httpd が止まる**（キーボードとマウスの入力が止まる）。電源断で、どのファイルにも属さないクラスタの連鎖が残る。中断からの再開で、パーツを作り直す必要が出る。通常の追記なら、FAT32 は空きの先頭から順に割り当てるので、フォーマット直後は自然に連続に近くなり、**連続でなくても、エクステントの一覧（FAT のチェーンの走査。§3.1）で読める**。パーツに分割して直接セクタを読む、という案 1 の本質は変わらない。

**変更してよいもの**: 数値（パーツの大きさ、チャンクの大きさ、各上限）は、実機の測定（`HW_VERIFY.md` I 章）で変えてよい。保存方式は、§2 の「エクステントの一覧」の層が隠すので、後から別の方式（exFAT の有効化など）に切り替えても、上の層は作り直しにならない。

## 1. 確認した事実と、未確認のこと

**確認した（一次情報）**
- **microSD の結線**（回路図 `docs/ESP32-P4-ETH-schematic.pdf` 1 ページ「MicroSD Card」を目視。2026-11-18）: D0=GPIO39、D1=GPIO40、D2=GPIO41、D3(CD)=GPIO42、CLK=GPIO43、CMD=GPIO44（4 ビットのモード）。**データ線（D0〜D3）と CMD は 51kΩ（R8〜R12）で `ESP_LDO_VO4` にプルアップ**（CLK にはない）。CD2 も 51kΩ（R13）でプルアップされているだけで、GPIO には接続されていない。**カードの電源**は `ESP_3V3` から PMOS（Q1、SI2301CDS）経由で、ゲートは **GPIO45**（10kΩ で GND にプルダウン: GPIO45 が Low または未設定で**電源 ON**、High で OFF）。**カード検出の GPIO はない**。
- ESP-IDF v6.0.1 の FatFs（`components/fatfs/src/ffconf.h`、`Kconfig`）: R0.15。**`FF_FS_EXFAT 0` で固定**（menuconfig に項目がない）。`FF_USE_EXPAND 1`（使わない。§3.1）、`FF_USE_MKFS 1`（使わない）。**`FF_USE_FASTSEEK` は `CONFIG_FATFS_USE_FASTSEEK` に従い、既定は `n`**。**IDF の VFS 側の説明では、fast-seek と CLMT は読み取りモードのファイルだけ**（FatFs の `f_lseek` 自体にモードの検査はなく、呼び出し側が `fp->cltbl` を設定する。§3.1）。`f_lseek(CREATE_LINKMAP)` は、表が小さいと `FR_NOT_ENOUGH_CORE` を返し、`cltbl[0]` に必要な数を入れる。**`f_rename` は、宛先が存在すると `FR_EXIST` で失敗する**（上書きできない。原子的な置換ではない）。`FF_LBA64` は無効（`LBA_t` は 32 ビット）。`FF_FS_REENTRANT 1`。`fs->database` は絶対 LBA で、`クラスタのセクタ = database + csize × (クラスタ番号 − 2)`。
- **esp_tinyusb と TinyUSB の構成**（Managed Component キャッシュを一次情報で確認、2026-11-19、Antigravity）: **`espressif/esp_tinyusb 2.4.0`**（commit `7963dbeaddee8798cae953cbcd2f1dc48c7589bf`）は **`espressif/tinyusb 0.21.0~6`**（commit `ab2826e0a23c14545e82aba1f3d4c68828937d91`）に依存する。
  - **標準の `msc_device.c`（`0.21.0~6`）**: `0.18.0` 以降で `tud_msc_inquiry2_cb` と `tud_msc_async_io_done` が追加されたものの、**`proc_builtin_scsi()`（`msc_device.c:684-840`）が 8 コマンドを内蔵処理で捕捉する構造は変わらない**。具体的には、`SCSI_CMD_MODE_SENSE_6`（`:805`）が 4 バイトの固定ヘッダー（`data_len=3, block_descriptor_len=0`）のみ返して Mode Page（`2Ah` 等）を返せず `tud_msc_scsi_cb` を呼ばない、`SCSI_CMD_READ_FORMAT_CAPACITY`（`:756`）が `descriptor_type=2` の 12 バイト固定形式を返す、`proc_read_io_data()`（`:878-883`）が `TUD_MSC_RET_ERROR`（`-1`）時に無条件で `set_sense_medium_not_present(lun)`（`02h/3Ah/00h`）を上書きする、`SCSI_CMD_READ_12`（`0xA8`）がストリーミング経路（`:327, 637` は `READ_10`/`WRITE_10` のみ）に乗らず `CFG_TUD_MSC_EP_BUFSIZE` 制限の `tud_msc_scsi_cb` に落ちる。→ **最新の `0.21.0~6` でも仮想 CD-ROM の契約（§5）を満たさない**。
  - **アプリのクラスドライバのフックと共存**: `usbd.c:457` は弱シンボル `TU_ATTR_WEAK usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count)` を公開しており、`esp_tinyusb 2.4.0` 側はこれを定義しない（競合ゼロ）。`usbd.c:393-406, 584-593` で自前ドライバを内蔵ドライバの前に登録でき、`device/usbd_pvt.h:50-60` の `init`/`deinit`/`reset`/`open`/`control_xfer_cb`/`xfer_cb`/`xfer_isr`/`sof` を受けられる。また、`CONFIG_TINYUSB_MSC_ENABLED=n`（既定）で `CFG_TUD_MSC == 0`（`tusb_config.h:47-48, 261`、`CMakeLists.txt:38`、`tinyusb.c:17, 64, 88`）となり、`msc_device.c` / `tinyusb_msc.c` を無効化したまま、`CONFIG_TINYUSB_HID_COUNT=3`（`CFG_TUD_HID=3`）および `descriptors_control.c` の独自記述子（`usb_hid_logic.c`）と共存できる。さらに `usbd_defer_func(func, param, false)`（`usbd.c:1527`、`usbd_pvt.h:131`）の `USBD_EVENT_FUNC_CALL` は `usbd.c:801-806` で `rhport` 検査なしに `tud_task` で実行されるため、ESP32-P4 の `rhport=1`（OTG_HS）でも `iso_msc_task` からの非同期復帰に使える。
- **ESP32-P4 の USB OTG HS（`dwc2_esp32.h:89`, `dcd_dwc2.c:262-330`、TinyUSB `0.21.0~6`）**: Port 1（OTG_HS）は `ep_count=16`、`ep_in_count=8`、`otg_dfifo_depth=1024`（32-bit word = `4096 B`）。HID×3（`0x81..0x83`）+ MSC（`0x84`/`0x04`）+ EP0 で IN `5/8`、OUT `2/16`。Buffer DMA での FIFO 消費は epinfo `32`（`2*16`）+ 共有 `GRXFSIZ`（`calc_device_grxfsiz(512, 16) = 304`）+ TX FIFO（`16 [EP0 64B] + 2 [0x81 8B] + 2 [0x82 6B] + 1 [0x83 4B] + 128 [0x84 512B] = 149`）= **`485 / 1024` words**（残 **`539` words = `2,156 B`**、Bulk IN ダブルバッファ時でも `613 / 1024` words・残 `1,644 B`）で余裕がある（実機列挙は未確認）。
- **TinyUSB タスクの優先度とスタック（`esp_tinyusb 2.4.0`）**: `tinyusb_default_config.h:70-85` で既定の `TinyUSB` タスクは優先度 `5`、スタック `4096 B`、アフィニティ CPU1（マルチコア時）。`usb_vbus`（優先度 `5`）・`csi_video`（`5`）・`ws_input`/`kvm_service`（`4`）と整合するよう、`iso_msc_task` は `TinyUSB` より低い優先度 **`4`**（プロビジョナル、`CONFIG_ISO_MSC_*` で設定可）、スタック **`4096 B`**（暫定、実機 HWM で確認）とする。
- 32GB のカードは、SD の規格上 SDHC で、FAT32 で出荷されるのが標準。セクタは 512 バイト。
- `esp_http_server` は **1 つのタスク**で、`select` → 1 つの要求の処理 → 次、と回る（`httpd_main.c`）。**1 つの要求の処理中は、同じサーバーの他の要求（`/ws` の入力を含む）が待つ**。

**未確認（`TODO(hw-verify)` / 調査）**
- 複合デバイス（HID ×3 + 独自 BOT MSC）および OTG_HS FIFO 割り当ての実機列挙、`iso_msc_task` 稼働時の HWM と HID 遅延の実測（`HW_VERIFY.md` I7, I8, I10）。
- `ESP_LDO_VO4` の電圧（SD の IO のレベルに関わる。カードの VDD は 3.3V）。**電源 OFF（GPIO45 = High）のとき、VO4 のプルアップからカードに逆給電しうるか**（電源の入れ直しでカードが確実にリセットされるか）。実測が要る。
- microSD のバス速度（40 MHz の高速モードで使えるか）、書き込み・読み出しの実効速度。
- ターゲット PC の BIOS / UEFI が、複合デバイス（HID + 仮想 CD-ROM）を認識し、CD として起動できるか。

## 2. 全体の構成

```
ブラウザ ── HTTPS /api/v1/iso/* ──▶ iso_api（純 C: 要求の検査と状態遷移）
                                      │
                    ┌─────────────────┼──────────────────┐
                    ▼                 ▼                  ▼
              iso_catalog        iso_upload          iso_media
        （一覧・名前・メタデータ）（受信の状態機械・    （仮想ドライブの状態:
                                    SHA-256・タイムアウト）  メディア有無・変更通知）
                    │                 │                  │
                    └──── iso_fs_t ───┘                  ▼
                         （FAT32 のファイル操作。   mmc_scsi（純 C: CD-ROM の SCSI コマンド。
                          IDF = FatFs）                  **全コマンドを持つ**。§5.1）
   USB のターゲット PC ◀── MSC（自前の BOT または         │
                          パッチした TinyUSB）◀───────────┘
                                                          │
                                                   iso_extent（純 C: CD のブロック → SD のセクタ）
                                                          │
                                                    iso_blockdev_t
                                                  （SD の 512B セクタの直接読み出し。IDF）
```

- **純 C（ホストでテストする。Claude）**: `iso_extent`、`iso_catalog`（メタデータの符号化を含む）、`iso_upload`、`iso_media`、`mmc_scsi`、`msc_bot`（BOT の状態機械。C-13）、`iso_api`。IDF のヘッダーに依存せず、時刻・乱数・I/O は関数ポインタで注入する。
- **IDF の結合（Antigravity）**: SD（SDMMC、LDO、電源スイッチ）、FAT32（FatFs）、SD の I/O の直列化、USB の MSC クラス（**`iso_msc_task`**。§5.1）、HTTP のハンドラー、**ワーカータスク**（§4.2）、ロックの階層（§4.1）。シームの型は **`components/iso_seams/include/iso_seams.h`**（宣言だけ。この PR の成果物）。
- 境界は `docs/INTERFACES.md` §7。

## 3. SD の保存形式（FAT32）

オーナーが PC で FAT32 にフォーマットする（32GB のカードは FAT32 が標準。Windows 標準のツールで可）。デバイスは、フォーマットしない（誤って消すのを避ける）。**マウント時に検査する**: `fs->fs_type == FS_FAT32`（FatFs は FAT12/16 も開くので、FAT32 だけを受け付ける）、**`csize × 512 ≥ 2048`**（クラスタが CD の 1 ブロック以上。そうでないと、CD の 1 ブロックがクラスタの境界をまたぎうる）。合わないカードと、FatFs が開けないカード（exFAT は `FF_FS_EXFAT 0` で開けない）は、**`sd.state` が `unsupported_fs`**（API は 503 `not_ready`）。

```
/iso/                       ← ルート。無ければ作る
  import/                   ← PC で ISO を直接コピーする場所（§3.3）
  <id>/                     ← 1 つのイメージ。<id> は 8 桁の小文字の 16 進（乱数）
    meta.a, meta.b          ← メタデータの 2 つのスロット（256 バイト固定。§3.2）
    p000, p001, ...         ← データのパーツ（連番）
```

### 3.1 パーツと書き込み
- **パーツの大きさは 1 GiB（1,073,741,824 バイト = 524,288 個の CD ブロック）**。最後のパーツだけ、残りの大きさ。FAT32 の 1 ファイルの上限（4 GiB − 1 バイト）に余裕がある。イメージの最大は、パーツ 16 個 = **16 GiB**（`ISO_MAX_PARTS`。READ CAPACITY(10) の 32 ビットに収まる）。
- **2 つの大きさを区別する**: **`size_bytes`**（アップロードで宣言した実バイト数。`meta` の @8）と、**`capacity_blocks` = ⌈`size_bytes` / 2048⌉**（CD として見せるブロック数。READ CAPACITY が返す）。`size_bytes` が 2048 の倍数でないとき、**`size_bytes` 以降のバイトは 0 を返す**。**このゼロ埋めは `mmc_scsi_read_blocks` が、バイト単位で行う**（`iso_extent` は、セクタ単位の区間だけを返す。ファイルの最後のセクタの、EOF より後ろのバイトは、0 とは限らない）。
- **`f_expand` は使わない**。アップロードは、パーツを先頭から順に、**通常の `f_write` で追記**する。**1 つのチャンクは、全体をバッファに受信してから、書く**（接続が途中で切れたとき、何も書かない）。**チャンクがパーツの境界をまたぐときは、書き込みを分割する**（最初のパーツの残りまで書き、次のパーツを作って続きを書く）。
- **書き込みの失敗の取り消し（`truncate`）**: FatFs の `f_truncate` は、**`fp->err` が立っている（直前の `f_write` が失敗した）ハンドルでは、即エラーを返す**（`ff.c`）。また、現在の読み書き位置が EOF 未満のときだけ切る。→ **`iso_fs_t.truncate(path, size)` は、「ハンドルを `close`（`f_sync` はエラーを見ない。`close` が失敗しても、ハンドルは解放する）→ `stat` で実ファイルの大きさを見る（`size` より小さければ `ISO_ERR_INVALID`。`f_lseek` が、書き込みモードで、ファイルを伸ばしてしまうため）→ 開き直す → `f_lseek(size)` → `f_truncate`」と定義する**。ハンドル版は作らない。`size` は、**そのチャンクを書く前の、メモリ上のファイルの大きさ**（純 C が持つ）。**書き込みの失敗の仕方によっては、ディレクトリ上の大きさが元のままで、連鎖だけが伸びていることがある**（このとき `f_truncate` は何もしない）。**孤児の末尾クラスタが残りうるが、次の追記が既存の次クラスタを再利用し、読み出しは `iso_extent` が大きさでクリップするので、無害**。**`truncate` 自体が失敗したら、アップロードは `incomplete` になる。`received` は、書く前の値のまま（直前に成功した `PUT` までの、メモリ上の値。同期済みとは限らない。書こうとした分は、受理していないので数えない。`consumed_since` には、書こうとした分を足す）。取り消せなかったパーツは、純 C がメモリに覚えておき、次の再開の最初の `PUT` と `finish` の受付の前に、もう一度 `truncate` する（成功するまで、どちらも進めない。エラーはそのまま返す）。再開のときの `received` は、ディスク上のパーツの大きさから決め直す（§3.4。メモリ上の値と、同期済みパーツの大きさの合計は、別のもの）。再起動すると、この記憶は失われ、ディスクに残った分は、実ファイルの大きさとして `received` に数えられる**（残った分は、クライアントが送ったチャンクの先頭で、続きの `PUT` と同じデータ。`finish` は全パーツを読み直して SHA-256 を計算する）。
- **`f_sync` は、`ISO_SYNC_KIB`（既定 4096 = 4 MiB）ごと、パーツの終わり、アイドルのタイムアウト、`finish`、中止のとき**に行う。**同期したときの、パーツのファイルの大きさが、`received` の正**（§3.4）。
- **`begin` で空き容量（`size` + 予備 `ISO_FREE_RESERVE_MIB` = 64 MiB）を検査する**（足りなければ 507）。再開した最初の `PUT` でも、**残りの大きさ + 予備**を検査する（中断中に、別のイメージが空きを使ったとき）。書き込み中に FatFs が「空きなし」を返したときも、取り消して `incomplete` にし、**507** を返す。
- **連続は必須ではない**: 読み出しは、FAT32 のクラスタのチェーンから作る**エクステントの一覧**で行う（**FatFs の `CLMT` ではなく、A-13（#153）が実装した自前の FAT の走査**。下の `get_extents`）。**エクステントの数が `ISO_MAX_EXTENTS`（既定 4096。イメージ全体）を超える**ときは、そのイメージを **`invalid`（理由 `too_fragmented`）**にする（最終処理で検査する。§3.4）。**`too_fragmented` にするのは、エクステントの数の超過だけ**（FAT の走査は `cltbl` を使わないので、`CLMT` の `FR_NOT_ENOUGH_CORE` は起きない。`f_open` が LFN バッファの確保に失敗して返す `FR_NOT_ENOUGH_CORE` は別。再試行する）。エクステントの一覧は **PSRAM** に置く。**`iso_extent` は、一覧を、ファイルのセクタ数（`⌈ファイルの大きさ / 512⌉`）でクリップする**（FatFs の連鎖は、クラッシュ後の孤児の末尾クラスタまで列挙しうる）。
- **`get_extents`（A-13 で実装済み。#153）は、FatFs の `CLMT`（`f_lseek(CREATE_LINKMAP)`）を使わず、FAT32 のチェーンを自前で走査する**(当初の設計は `CLMT` だった。`cltbl` の確保と `FR_NOT_ENOUGH_CORE` の扱い、`PUT` の `f_write` を待たせる volume mutex の保持を避けるため、実装で変えた)。(1) `f_stat` と `f_open(FA_READ)` で、ファイルの開始クラスタ(`fil.obj.sclust`)と、FAT 領域の位置(`fatbase`、`database`、`csize`、`n_fatent`)を得て、`f_close` する(**FatFs の volume mutex は、この間だけ保持する**)。(2) FAT のセクタを、`iso_blockdev_t` と同じ経路(`sd_io_lock` の下、`epoch` を固定)で、数セクタずつ先読みして読み、クラスタのチェーンをたどる(**循環の検出(Brent)と、クラスタ数の上限つき**。壊れたチェーンは `ISO_ERR_IO`)。連続するクラスタを、1 つのエクステントにまとめ、`max` を超えるときは `ISO_ERR_TOO_FRAGMENTED`(**走査はファイルの大きさを知らないので、ファイルの末尾より後の孤児のクラスタも、この数に入り、`iso_extent` のクリップの前に判定する**。孤児が多数の不連続なクラスタに及ぶのは、クラッシュ後など、通常は起きず(`truncate` はクラスタを解放する)、起きたときは `too_fragmented` で `invalid` になる安全側の結果)。(3) **走査の間は、FatFs の volume mutex を持たない**ので、FatFs の呼び出し(`PUT` の `f_write` など)を、volume mutex では待たせない(ただし、`INDEX` のステップが保持する `iso_op_mutex` のため、その間の `PUT` は `503 busy` になりうる。SD の読み出しは `sd_io_lock` を MSC と共有する。§4.1)。(4) 対象のファイルは、走査の間、変更されない前提(`FINISH` は `sync` + `close` の後に呼び、`DELETE` とは `iso_op_mutex` で排他)。(5) FAT32 でないとき、または FAT の領域が不正なときは `ISO_ERR_UNSUPPORTED_FS`。`CONFIG_FATFS_USE_FASTSEEK=y` は、ビルドの検査(`#error`)が残っているが、`get_extents` は `cltbl` を使わない。

### 3.2 メタデータ（256 バイト固定、リトルエンディアン）
| オフセット | 大きさ | 内容 |
|---|---|---|
| 0 | 4 | マジック `ISO1` |
| 4 | 2 | 版（1） |
| 6 | 1 | 状態: 0 = `uploading`、1 = `ready`、2 = `incomplete`、3 = `invalid`、4 = `deleting` |
| 7 | 1 | `invalid` の理由: 0 = なし、1 = `checksum_mismatch`、2 = `not_iso`、3 = `too_fragmented`、4 = `part_mismatch`（パーツの欠落、またはサイズの不一致） |
| 8 | 8 | **`size_bytes`**（アップロードの宣言値。実バイト数） |
| 16 | 8 | 予約（0。`received` はここに持たない。§3.4） |
| 24 | 2 | パーツの数 |
| 26 | 2 | 予約（0） |
| 28 | 4 | パーツの大きさ（ブロック数。既定 524288。`import` では、そのファイルの大きさ（ブロック数、切り上げ）） |
| 32 | 32 | SHA-256（宣言値、または計算値。64 のフラグに従う） |
| 64 | 1 | SHA-256 のフラグ: 0 = なし、1 = **宣言値**（`begin` で受け取った。`finish` で、計算値と比べる）、2 = **計算値**（`ready` のとき。検証済み、または宣言なしで計算した） |
| 65 | 1 | 名前の長さ（バイト。1〜63） |
| 66 | 64 | 名前（UTF-8。残りは 0） |
| 130 | 4 | **世代番号**（書くたびに +1。下の規則） |
| 134 | 118 | 予約（0） |
| 252 | 4 | **先頭の 252 バイトの CRC-32（IEEE 802.3）** |

**2 つのスロット（`meta.a` / `meta.b`）の規則**（`rename` では置き換えない。FatFs の `f_rename` は、宛先が存在すると失敗し、`unlink` + `rename` は原子的でない）:
- **有効なスロット**: ファイルの長さがちょうど 256 バイト、マジックと版が正しく、CRC が正しい。**長さが足りないファイル（作成直後に電源が落ちた）は、無効**。
- **読む**: 有効なスロットのうち、**世代番号が新しい方**を採用する（比較は `(int32_t)(a - b) > 0`。巻き戻りを許す）。**同じ世代番号なら `meta.a`**。有効なスロットが 1 つもなければ、そのイメージは **`invalid`（実行時の理由 `meta_corrupt`。`meta` には保存しない）**。名前は `<id>`、`size` と `received` は `null`。削除はできるが、マウントはできない。
- **書く**: **採用していない方（無効、存在しない、または古い方）のスロットに、世代番号 + 1 の 256 バイトを書き（ファイルが無ければ作る。`iso_fs_t.overwrite` は「作成または先頭から上書き」）、`f_sync`** する。有効なスロットが 1 つもなければ、`meta.a` に世代番号 1 で書く。
- **作成（`begin`）の順序**: `mkdir <id>` → `meta.a`（世代番号 1、状態 `uploading`）を書いて `f_sync` → 201。`meta.b` は、最初の更新で作られる。`mkdir` と最初の `meta.a` の間で電源が落ちたディレクトリは、`meta_corrupt` として一覧に出る（削除できる）。
- **電源断のパターン**（`HW_VERIFY` I11 で確認）:
  | パターン | 結果 |
  |---|---|
  | スロットの書き込み前 | 変化なし |
  | スロットの書き込み中（途中のセクタだけ書けた） | CRC が合わず、他方のスロットが採用される。新しい状態が失われるだけ |
  | 書き込み後、`f_sync` の前 | どちらの内容でも整合している |
  | `f_sync` 中のディレクトリのセクタの書き込み途中 | `meta.a`、`meta.b`、`p000` のディレクトリエントリは、同じセクタにありうる。**2 スロットでは守れない**（FAT とディレクトリは、この方式の対象外） |
  | カード内部の消去ブロックの読み出し・書き換えの途中 | 隣り合うクラスタの 2 つのスロットが、同時に壊れうる。**2 スロットでは守れない** |
  | 両方のスロットが無効 | `meta_corrupt`。データのパーツは残るので、PC でカードを調べられる |
  保護されるのは **`meta` ファイルの中身だけ**で、FAT とディレクトリの破損は対象外（FatFs の `f_sync` は、FAT とディレクトリを書く順序を保証しない）。
- 状態が `uploading`（0）のまま読まれたものは、**`incomplete` として扱う**（電源断。§3.4）。

### 3.3 PC から直接コピーする（`import`）
`/iso/import/` に、PC で **4 GiB 弱までの ISO ファイル**（`*.iso`。拡張子は大文字小文字を区別しない）をコピーすると、`POST /api/v1/iso/rescan`（と起動時）で、イメージとして取り込める。**日本語などのファイル名を読むため、`CONFIG_FATFS_API_ENCODING_UTF_8`（`FF_LFN_UNICODE=2`）が必要**（A-13。既定の OEM（CP437）では、日本語名のファイルは `skipped` になる）。
- **取り込みは `rename` で `/iso/<id>/p000` に移す**（コピーしない。ディレクトリを作ってから移すので、宛先は存在しない）。名前はファイル名（拡張子を除く）。**名前の規則に合わない（63 バイトを超える、FatFs が名前を返せない（255 バイト超）を含む）、同名がある、`ISO_MAX_IMAGES` に達している、サイズが 34,816 バイト未満、セクタ 16 に署名（`CD001` / `BEA01`）がない**ファイルは、取り込まず `skipped` に数える（ファイルは `import/` に残す。`invalid` のイメージは作らない）。
- **`rename` の途中で電源が落ちると、同じクラスタの連鎖を、`import/foo.iso` と `<id>/p000` の 2 つのエントリが指す**（FatFs の `f_rename` は、新エントリの登録 → 旧エントリの削除の 2 段階。`ff.c` のコメントにもある）。**起動時と `rescan` で、`import/` のファイルの先頭クラスタ（`iso_fs_t.stat` の `first_cluster`）が、既存の `<id>/p000` と同じなら、そのファイルは取り込まず、警告をログに出す**（`skipped` に数える）。**このファイルを `f_unlink` してはならない**。**同じ理由で、`DELETE`（`<id>/p000` の削除）の前にも、`import/` に同じ先頭クラスタのファイルがないか確かめ、あれば `409 crosslinked` で拒否する**（`p000` を消すと、`import/` 側が、解放済みのクラスタを指して、次の `rescan` で取り込まれてしまう）（連鎖を解放してしまい、`<id>/p000` が壊れる）。PC で `chkdsk` などを使って直す（`HW_VERIFY` I11）。
- **削除の再開（`deleting` の `meta` が残っている。再起動、SD の再初期化、再度の `DELETE`）でも、同じ照合をする**（C-10a、Issue #152 の追記）。**削除のステップの最初（`meta` を書く前）で、`import/` に同じ先頭クラスタのファイルがあれば、何も消さず（`p000` も、ほかのパーツも、`meta` も残す）、`ERROR(crosslinked)` で止まる**。`deleting` のまま、PC の `chkdsk` で直すまで残す（再試行しても、同じ結果になる。ワーカーは自動で再試行しない）。**警告のログは、純 C にログの手段がないので、`ERROR` を受けたグルー（`iso_worker`）が出す**（イメージの `id` だけ。名前は出さない）。
- **`import` の `meta` の書き込みと `rename` の間の電源断**（C-10a の決定）: 順序は `mkdir <id>` → `meta.a`（`ready`、SHA-256 なし）→ `rename`。`meta` と `rename` の間で電源が落ちると、`p000` のない `ready` のイメージが残り、ファイルは `import/` に残る。次の `rescan` は、同名のイメージが残っているので、そのファイルを `skipped` にする。起動時の `INDEX` が `part_mismatch`（パーツの欠落）にするので、利用者が削除すれば（`p000` がないので、クロスリンクの照合は一致しない）、もう一度取り込める。`rename` を先にすると、ファイルが `<id>/p000` に移った後で `meta` がなく、`meta_corrupt` になる（データは残るが、`import/` から見えない）ので、`meta` を先にした。
- **SHA-256 は計算しない**（フラグ 0、`sha256: null`）。**`not_iso` の検査（セクタ 16 の署名）だけ行い、`ready` にする**。エクステントの作成は、通常の `ready` と同じく、ワーカーが行う（§4.2。それまで `finalizing`）。
- **`rescan` は、1 回の呼び出しで、ディレクトリのエントリを最大 `ISO_RESCAN_BATCH`（既定 4）個まで調べる**（取り込む（`rename` を伴う）ものも、`skipped` にするもの（署名の判定にファイルの読み出しが要る）も、同じ予算に数える。1 つのファイルで、`mkdir`、`meta` の作成と `f_sync`、`rename` があり、数百 ms〜数秒かかる。単一スレッドの httpd を長く止めない。§4.2）。**継続位置**: リクエストは `{}` または `{"cursor":C}`。**`C` は、`import/` の `*.iso` のエントリ（ディレクトリの順）のうち、先頭から読み飛ばす数**（`skipped` にして残したエントリの数。取り込んだエントリは、ディレクトリから消える）。応答は `{"imported":N,"skipped":M,"next_cursor":C'|null}`（**`C'` = `C` + `M`**。まだ調べていないエントリがなければ `null`）。**ビューアは、`next_cursor` が `null` になるまで、その値を `cursor` に入れて繰り返す**（各回で、取り込むか、位置が進むので、必ず終わる）。**書き込み途中のファイルを取り込む危険**を避けるため、`rescan` は、ユーザーの操作（と起動時）でだけ行う。
- **4 GiB を超える ISO は、FAT32 のファイルとして置けないので、ブラウザのアップロード（パーツに分割される）を使う**。
- **起動時のスキャン**（`iso_catalog` の仕事）: `/iso/` の直下の各ディレクトリを調べる。名前が `<id>` の形式でないものは無視する。**`ISO_MAX_IMAGES` を超える分は、一覧に出さない**（カードの空きは使う。`GET` の `hidden_images` に数を出す。**見える順はディレクトリの順。見えているイメージを `DELETE` すると、次のスキャン（`DELETE` の完了時に行う）で、隠れていたものが見えるようになる**）。

### 3.4 状態、遷移、`received`

**状態の遷移表**（`meta` に保存する状態は、§3.2 の 5 つ。`finalizing` は実行時の状態）:

| 現在の状態 | イベント | 次の状態 | 備考 |
|---|---|---|---|
| （なし） | `begin` | `uploading` | §3.2 の作成の順序 |
| `uploading` | `PUT`（正常） | `uploading` | |
| `uploading` | アイドルのタイムアウト、SD の書き込み失敗（`truncate` の後） | `incomplete` | ワーカーの `IDLE_CLOSE` が、`f_sync`、`f_close`、`meta` の書き込みを行う |
| `uploading` | 再起動 | `incomplete` | `received` は、同期済みのパーツのファイルの大きさ |
| `uploading` / `incomplete` | `finish`（`received == size_bytes`） | `finalizing`（`FINISH` ジョブ） | `meta` の状態は、`ready` になるまで変えない |
| `uploading` / `incomplete` | `finish`（`received < size_bytes`） | 変化なし | 409 `size_mismatch` |
| `incomplete` | `finish`（`received == size_bytes`、別のイメージが枠を使っている） | 変化なし | 409 `upload_in_progress` |
| `invalid` | `finish` | 変化なし | 422（`invalid_reason`） |
| `incomplete` | `PUT`（枠が空いている） | `uploading` | 再開 |
| `finalizing`（`FINISH`） | 検査に成功 | `ready` | パーツを同期して閉じ、`meta` を `ready` で書く（SHA-256 のフラグは 0（import）または 2。宣言値（1）は、検証の後に 2 に変わる） |
| `finalizing`（`FINISH`） | 検査に失敗（`checksum_mismatch`、`not_iso`、`too_fragmented`、`part_mismatch`） | `invalid` | 理由を `meta` に保存 |
| `finalizing`（`FINISH`） | SD の I/O エラー | `incomplete` | `received` は、実ファイルの大きさから決める。再試行は `finish` |
| `finalizing`（`FINISH`） | 再起動 | `incomplete` | `received == size_bytes` なので、**ビューアが、一覧を見て自動で `finish` を送る**（§C-12） |
| `ready` | 起動時・SD の再初期化後の `INDEX` ジョブ（表示は `finalizing`。**アップロードの枠は使わない**） | `ready` | エクステントの表を公開する。**各パーツのサイズを、`meta` の期待値と照合する** |
| `ready` | `INDEX` で、パーツの欠落やサイズの不一致、`too_fragmented` | `invalid` | 理由 `part_mismatch` / `too_fragmented`。`meta` に保存 |
| `ready` | `INDEX` で、SD の I/O エラー | `ready`（表なし） | 表示は `finalizing`。SD の再初期化の後に、もう一度行う |
| `invalid` | `DELETE` だけ | `deleting` | |
| 任意（マウント中を除く） | `DELETE` | `deleting`（`DELETE` ジョブ） | `finalizing` のジョブには、中止の印を立てる。ジョブは、ステップごとに印を見る |
| `deleting` | 完了 | （一覧から消える） | 手順は下 |
| `deleting` | 再起動 | `deleting` を続ける | `meta` の状態が 4 のものは、起動時にジョブを作り直す |

- **削除の手順**（`DELETE` ジョブ。**順序が重要**）: (1) `meta` を状態 `deleting`（4）で書いて同期（**これで、途中で電源が落ちても、パーツが欠けた `ready` が残らない**）。(2) ディレクトリ内の、`p*` のファイルを 1 つずつ `f_unlink`。(3) `meta.a`、`meta.b` を `f_unlink`（**採用していない（古い）スロットを先に、`deleting` を書いた方を最後に消す**。途中で電源が落ちても、古い `ready` が単独で採用されない）。(4) `rmdir`。**ディレクトリのファイルは、`meta` の内容に頼らず、一覧から消す**（`meta_corrupt` のものも、同じ手順で消える。(1) は、有効なスロットがあるときだけ）。
- **`received`（受信済みのバイト数）の正は、同期済みのパーツのファイルの大きさの合計**（先頭から順に数え、パーツの大きさに満たないパーツで止める）。メモリ上の値は、その時点の同期前の分を含むが、**再起動すると、同期済みの分に戻る**。クライアントは、`offset_mismatch` の `received`、または `GET` の `received` から続ける。
- **SHA-256 の逐次の状態は、メモリ上にだけある**。**再起動、SD のエラーによる巻き戻し（`truncate`）で失われる**。失われたときは、`FINISH` ジョブが、SD を読み直して計算する。アイドルのタイムアウトでは、状態を保つ（失わない）。**宣言値（フラグ 1）は、`meta` に保存されているので、再起動しても残る**。
- **アップロードの枠**: **`uploading` のイメージと、`FINISH` ジョブ中の `finalizing` のイメージが使う**。`INDEX` ジョブ（起動時など）は使わない（その間も、`begin` は受け付ける）。
- **マウントできるのは、エクステントの表が公開された `ready` だけ**（`INDEX` の間は 409 `image_not_ready`）。
- **マウント中のイメージは、削除も、変更もできない**（409 `mounted`）。
- **`ready` になるのは、全パーツを `f_sync` と `f_close` した後**。
- **`FINISH` の途中で SD が差し替わったら、`ready` にしない**: `FINISH` は、開始のときに `iso_blockdev_t.epoch` を記録し、**`meta` を `ready` で書く前に、もう一度読んで比べる**。違えば `ISO_ERR_EPOCH` で `incomplete` に戻す（表は引き渡さない。ディスク上の `meta` は `ready` にならない）。**SD の再初期化（`epoch_bump`）は、動いている `FINISH` に印を立てない（印は `DELETE` だけ）ので、この検査が、カードが替わった後に `ready` を書かないための守り**。
  **この検査だけでは、確認と `meta` の書き込みの間に ① の `epoch_bump` が入る窓が残る**（① は `iso_state_mutex` だけを取り、`iso_op_mutex` を取らないので、1 回のステップの途中にも入りうる）。**純 C では閉じられない。A-13 が、次の要件で閉じる（1 つに決めた）**:
  - **(ii)を採る**: ②〜⑦ は `iso_op_mutex` を取って始まり、ジョブのステップも `iso_op_mutex` の下で行われる（§4.1 の表）ので、**実行中の `FINISH`（などのジョブ）のステップが終わってから ② が始まり、② が、そのジョブを取り消す**。これは既存の設計のとおりで、追加の排他は要らない（`iso_op_mutex` を長く持たせない。ジョブはステップごとに放す）。
  - **(i)の「書いた `ready` を取り消す」は採らない**: 取り消しの `meta` の書き込みが、替わった後のカードに入りうる。再スキャンの再検査も、`ready` の `meta` が替わったカードの上に書かれた後では、手遅れ。
  - **A-13 の要件（書き込みの柵）**: **自前の diskio の書き込み系（`disk_write`、`CTRL_SYNC`。`iso_fs_t` の `write` / `sync` / `truncate` / `overwrite` / `rename` / `remove` / `mkdir` が通る）は、`sd_io_lock` の下で、⑤ の `f_mount` のときに記録した `epoch`（マウントの `epoch`）と、現在の `epoch` を比べ、違えば `ISO_ERR_EPOCH` で失敗する**（`read_sectors` と同じ。マウントの `epoch` は ⑤ で記録し直す）。これで、① の `epoch_bump` の後の書き込みは、替わった後のカードへ行かず、`FP_META` の `overwrite` は失敗して、`FINISH` は `incomplete` になる（`meta` は `ready` にならない）。純 C の `epoch` の確認は、その前に、無駄な書き込みを避ける早期の失敗と、ホストで確かめられる二重の守り。
    - **柵の保証の範囲(PR #155 のレビュー)**: 柵の確認と、その後の I/O は、**`sd_io_lock` を保持したまま続ける 1 つの区間**にする(ロックの階層: ① の `epoch_bump` は `iso_state_mutex` だけを取り、**`sd_io_lock` は取らない**。`iso_state_mutex` の保持中に `sd_io_lock` を取らない。`epoch_bump` は、実行中の I/O を待たず、止めない)。保証は次のとおり。(1) **`epoch_bump` が見える状態で始まった書き込みは、必ず `ISO_ERR_EPOCH` で拒否される**。(2) 確認が済んだ後に `epoch_bump` が入った書き込みは、**拒否されず、そのまま完了する**(確認の時点でささっていたカードへの書き込みとして)。(3) **物理的な差し替えと、その検出(CD 検出、I/O エラー)の間の書き込みは、柵では防げない**(差し替え → 検出 → `epoch_bump` の順なので、ソフトウェアでは限界がある)。(3) で古いカードの `ready` の `meta` が新しいカードに書かれるには、**新しいカードに、同じ id のディレクトリがあり、`meta` を置ける**必要があるので、通常は書き込みに失敗する。仮に書かれても、再初期化の後の `INDEX` が、`meta` のパーツの大きさと実ファイルの大きさを突き合わせ(不一致は `invalid`)、ISO の署名を確かめる。**`INDEX` は SHA-256 を計算し直さない**ので、同じ id・同じ大きさの別の内容は検出できない。この残りのリスクは、極小の窓として受け入れる(`FINISH` の最後の 1 回の `meta` の書き込みと、カードの差し替えが、同時であること)。
  - ホストでは確かめられない（`iso_fs_t` の実装の性質）。`HW_VERIFY` I11 に、**`FINISH` の最中の SD の差し替え（`epoch_bump` の直後の書き込みが `ISO_ERR_EPOCH` で失敗し、新しいカードに何も書かれないこと）** を足す。`INDEX` が作る表の `epoch` は、`INDEX` の開始のときの値（`FINISH` の記録より後なら、違いを検出する）。
- **`IDLE_CLOSE` と `PUT` の競合**: ワーカーの `IDLE_CLOSE`（タイムアウトで積まれる）が実行されるまでの間に、`PUT` が成功したら、**枠を閉じない**。`PUT` が成功したとき、**キューに待っている同じイメージの `IDLE_CLOSE` を取り除き、すでに選ばれて動き出す前のものには中止の印を立てる**（印のある `IDLE_CLOSE` は、何もせずに終わる）。`iso_upload_idle_close` を直接呼ぶ（強制）ときは、取り除かない（SD のエラーからの復帰など）。受理されなかった `PUT`（`offset_mismatch` など）では、取り除かない。
- **SD の再初期化の手順**（抜き取り、I/O エラーからの復帰。**起動時と同じ流れ**。**① は、`iso_state_mutex` だけを取り、1 つの区間で、(a) `sd.state` を `ready` 以外にして公開（`mount` と `begin` などは、`iso_op_mutex` を取る前に、このビューを見て 503 `not_ready` にする。`iso_media_mount` も、`iso_state_mutex` の中で、`sd.state` を再検査する）、(b) `force_eject`、(c) `epoch_bump` を行う。この後、`iso_op_mutex` を取って ②〜⑦ を行う**）: ① `iso_media_force_eject()`（カードの世代 `epoch` を進め、進行中の READ を失敗させる。§4.1）→ ② ワーカーのジョブを全て取り消す → ③ 開いているファイルハンドルを全て捨てる → ④ アップロードを `incomplete` にして、SHA-256 の状態を捨てる → ⑤ `f_mount` → ⑥ カタログを、起動時のスキャンで作り直す → ⑦ `ready` のイメージごとに `INDEX` ジョブ。**自動ではマウントしない**。

## 4. 排他、ワーカー、性能

### 4.1 ロックの階層（1 つの表。これ以外の入れ子を作らない）

| 順（外 → 内） | ロック | 守るもの | 保持の規則 |
|---|---|---|---|
| 1 | `iso_op_mutex` | **変更する操作の全体**: カタログの更新、アップロードの書き込み、ワーカーのステップ、`mount` / `eject` / `DELETE` の状態更新。**I/O を含めて直列化する** | httpd のハンドラー（`begin` / `PUT` / `finish` / `DELETE` / `rescan` / `mount` / `eject`）と、ワーカーが取る。**httpd のハンドラーは、`ISO_OP_LOCK_WAIT_MS`（既定 20 ms）以内に取れなければ、待たずに `503 {"error":"busy"}` + `Retry-After: 1` を返す**（単一スレッドの httpd を、ワーカーの長いステップで止めない。`/ws` の入力が止まる）。**MSC と `GET` は取らない**。ワーカーは、**ステップごとに放す**（`DELETE` や `mount` が割り込める） |
| 2 | `iso_state_mutex` | **公開ビュー**: メディアの状態（`iso_media`）、カタログの要約のコピー（`GET` 用）、エクステントの表への参照、`sd.state`、キャッシュした `free_bytes` | **短い区間だけ**（メモリの読み書きだけ。**I/O、ネットワーク、`sd_io_lock`、FatFs を呼ばない**）。純 C は、操作の最後に、ビューを更新する（グルーが渡すコールバックで公開）。**MSC と `GET` は、このロックだけ取る**。**`epoch` は、このロックの対象ではない**（下の「`epoch`」） |
| 3 | FatFs の volume mutex | FatFs の 1 回の API 呼び出しの全体 | `iso_op_mutex` を持つ側だけが、FatFs を呼ぶ |
| 4 | `sd_io_lock` | SD への 1 回のアクセス（diskio の 1 回の読み書き、または `read_sectors` の 1 回） | **シームの中が、自分で取る**。**呼び出し側（純 C、ワーカー、MSC）は、決して取らない**（取ると、自己デッドロックと、FatFs との ABBA が起きる） |

- **順序**: 1 → 2 は、取ってよい（2 は短い）。1 → 3 → 4 も、取ってよい。**2 を持ったまま、1・3・4 を取らない**。**MSC は、2 を取って放してから、4（`read_sectors`）を取る**（1 は取らない）。
- **MSC と `GET` は、変更する操作が I/O 中でも、止まらない**（ビューは、操作の途中の値ではなく、最後に公開された値を返す）。
- **`iso_media_force_eject()`、`iso_media_bus_reset()` は、2 だけを取る**。**`sd_io_lock` を持つ文脈（diskio の中）から呼ばない**。diskio は、エラーを**戻り値で上げ**、ロックを持たない文脈（SD の監視タスク）が、`force_eject()` を呼ぶ。
- **READ の経路と、スナップショットの寿命**: `mmc_scsi_execute` は、READ(10)/(12) のとき、`iso_state_mutex` の下で**エクステントの表への参照（参照カウント）** を取り（**表は、作られたとき（`INDEX`）の、カードの `epoch` を持つ。`read_sectors` には、この `epoch` を渡す**。コマンドの開始時の `epoch` ではない。SD の再初期化で `epoch` が進んだ後は、古いカードの表で、新しいカードを読むことが、必ず失敗する）。**`mount` は、表の `epoch` が現在の値と同じことも検査する**（違えば 409 `image_not_ready`）、**参照を放すのは `mmc_scsi_command_done(ctx)` だけ**（**BOT 層は、正常終了、STALL、リセット、切断のどの場合も、必ず 1 回だけ呼ぶ。取り消し済みでも冪等**）。**`iso_media_bus_reset()` と `force_eject()` は、参照を放さない**（`epoch` を進め、進行中のコマンドに中止の印を立てるだけ）。表の実体は、参照が 0 になった時点で解放する（遅延解放）。コマンドは、**参照を持っている間だけ**表を読む（`iso_extent` の引き直しも、その間）ので、表が解放済みになることはない。
  - **Web の `eject`**: 参照を落とすだけで、進行中の READ の完了を待たない。**進行中のコマンドは、取得済みのスナップショットで完了してよい**。次の**コマンド**からは、NOT READY（メディアなし）。
  - **`DELETE` のジョブは、エクステントの表の参照が、カタログの分だけになるまで待つ**（`eject` の直後でも、進行中の READ が、解放されたクラスタを読まない）。**待ちには上限を付ける**（`ISO_DELETE_REF_WAIT_S`、既定 30 秒）。ホストが IN を読まなくなっても、BOT のタイムアウトとリセットが `command_done` を呼ぶ。上限を超えたら、ジョブは `ERROR(ISO_ERR_BUSY)` で、`deleting` のまま、次の機会（SD の再初期化、起動時、再度の `DELETE`）に続ける。
  - **`epoch`（M6）**: **原子的な `uint32_t`。持ち主は `iso_blockdev_t` の実装（A-13）**。`iso_blockdev_t.epoch_bump(ctx)` で進め（**SD の再初期化と `force_eject` が呼ぶ**。`iso_state_mutex` の下から呼んでよい）、**`read_sectors` は、`sd_io_lock` の下で、ロックなしに（原子的に）読んで**、引数の `epoch` と違えば `ISO_ERR_EPOCH` で失敗する（別のカードの任意のセクタを、ISO の続きとして返さない。§5.5）。**`iso_state_mutex` は、`sd_io_lock` の中から取らない**（階層のため）。
- **関数とロックの対応**（グルーが、純 C の関数を、次のロックで包んで呼ぶ。**純 C の部品は、スレッドセーフではない**）:
  | 呼ぶ関数 | 呼ぶ側が取るロック |
  |---|---|
  | `iso_api_*`（`put_precheck` を除く）、`iso_catalog_*`、`iso_upload_*`、ジョブのステップ、`iso_upload_tick` | `iso_op_mutex`（純 C は、ビューの更新を、渡されたコールバックで行う。コールバックの中が `iso_state_mutex` を取る） |
  | `iso_api_put_precheck`、`GET` の組み立て | `iso_state_mutex`（ビューだけを読む） |
  | `iso_media_*`（`mount`、`eject`、`force_eject`、`bus_reset`、`prevent_removal` など）、`mmc_scsi_execute`、`mmc_scsi_command_done` | `iso_state_mutex`（短い） |
  | `mmc_scsi_read_blocks`、`iso_extent` の変換 | なし（参照を保持している間。`read_sectors` が `sd_io_lock` を取る） |
  | `iso_fs_t` / `iso_blockdev_t` の各関数 | 呼び出し側は取らない（FatFs の volume mutex と `sd_io_lock` は、中が取る）。`iso_fs_t` は、`iso_op_mutex` を持つ側だけが呼ぶ |
- **FatFs の volume mutex は、1 回の API 呼び出しの全体で保持される**（`ff.c` の `validate` から `LEAVE_FF` まで）。**FatFs 経由の呼び出し（`f_lseek`、`f_unlink`、`f_write`、`mkdir` など）は、互いに待たされる**。例: `f_unlink`（数百 ms〜秒）の間、`PUT` の `f_write` は待つ（`get_extents` は、FAT の走査の間、FatFs の volume mutex を持たないので、**volume mutex では待たせない**。ただし、`INDEX` のステップは、走査の間 `iso_op_mutex` を保持するので、その間の `PUT` は `ISO_OP_LOCK_WAIT_MS` の間に取れなければ `503 busy`、走査の SD 読み出しは `sd_io_lock` を MSC の読み出しと共有する。§3.1、§4.1）。**止められないのは、MSC の直接読み出し（`sd_io_lock` だけ）だけ**。→ ワーカーは、**パーツごと・ファイルごとにステップを分け、ステップの間に `iso_op_mutex` を放す**。それでも 1 ステップの時間は短くならない。**`HW_VERIFY` I10 で、ワーカーの稼働中の `PUT` の遅延を測る**。

### 4.2 ワーカータスク（A-13）と、HTTP の長さの制限

**`esp_http_server` は 1 つのタスクで動く**ので、**1 つの要求の処理は短く保つ**（数十 ms まで。**「数十 ms」は、LAN と SD が通常の速さのときの見込み**。受信はクライアントの速度に依存する。§6.1）。長い処理は、**専用のワーカータスク（`iso_worker`）**で行い、API は `202` と状態で返す。

| 処理 | 時間（推測。`TODO(hw-verify)`） | 扱い |
|---|---|---|
| `finish` の最終処理（`FINISH` ジョブ）: SHA-256 の再計算（SD の読み直し）、エクステントの作成（FAT のチェーンの走査）、ISO の署名の検査 | 数秒〜数分 | ワーカー。`finish` は `202 {"state":"finalizing"}` |
| 起動時・SD の再初期化後のエクステントの作成（`INDEX` ジョブ。`ready` のイメージごと） | 16 GiB で十数秒 | ワーカー。`begin` は、その間も受け付ける（枠を使わない） |
| `DELETE`（`DELETE` ジョブ） | 数秒 | ワーカー。`202 {"state":"deleting"}` |
| アイドルのタイムアウト（`IDLE_CLOSE` ジョブ: `f_sync`、`f_close`、`meta` の書き込み） | 数十〜数百 ms | ワーカー（ワーカーが 1 秒ごとに `iso_upload_tick` を呼んで判定） |
| `rescan` | 1 回に最大 `ISO_RESCAN_BATCH`（4）エントリ。各数百 ms〜数秒 | **同期**。httpd を、数秒止めうる（**この制約を許容し、上限で抑える**。`remaining` で続ける） |
| チャンクの `PUT`（受信 + `f_write`） | 128 KiB で数十 ms | 同期（httpd のスレッド） |

- **ジョブの契約**:
  - **ワーカーは 1 つのタスク**。ジョブは、**キュー**（長さ `ISO_WORKER_QUEUE`（既定 8）の FIFO）と、**`INDEX` が必要なイメージの集合**（キューに積まない。ワーカーが空いたときに 1 つずつ選ぶ）。種類は `FINISH(id)`、`INDEX(id)`、`DELETE(id)`、`IDLE_CLOSE(id)`。**優先順位: `DELETE`（先頭に割り込む。動いているジョブに中止の印を立てる）、`FINISH`、`IDLE_CLOSE`、`INDEX`**（利用者が待つ操作を、数分かかる `INDEX` の後ろに並べない）。**キューがあふれたら、投入は `ISO_ERR_BUSY`**（API は `503 busy`）。**同じイメージには、同時に 1 つのジョブだけ**（重複する投入は、すでに動いているジョブに合流する）。
  - **ステップ関数は、1 回に `ISO_WORKER_BUDGET_KIB`（既定 1024 KiB）以内の仕事を進め**、`CONTINUE`（続きがある）/ `DONE` / `ERROR(iso_err_t)` を返す。**中止の印は、ステップの先頭で見る**。**`ERROR` のとき**: `FINISH` は `incomplete`（§3.4）、`INDEX` は SD の I/O エラーなら表なしのまま、`DELETE` は、ログを出して、次の SD の再初期化か、起動時に続ける。
  - ステップ関数は、ステップごとに `iso_op_mutex` を取り、**シームが `sd_io_lock` を自分で取る**（§4.1）。
- **ステップの時間**: ステップ関数は、イベント待ちで長く待たない（I/O の待ちは除く）。**1 回のステップは、複数の同期 I/O を順に行うことがある**（例: `IDLE_CLOSE` は、パーツの `f_sync` と `f_close` の後に `meta` を書く。`FINISH` の 1 ステップは、`f_sync` + `f_close`、または `budget` バイトまでの読み出しと SHA-256 の更新、または `meta` の書き込み）。**`iso_op_mutex` の保持時間は、ステップ 1 回の中の I/O を、すべて数えて見積もる**。1 つの I/O は、FatFs の 1 回の呼び出し（`CLMT` の作成、`f_unlink` など）の時間で、数百 ms〜秒になりうる。その間、httpd は `iso_op_mutex` の try-lock が失敗して `503 busy` を返す（§4.1）。`HW_VERIFY` I10 で、**httpd が `iso_op_mutex` を待った時間と、`busy` の頻度**を測る。
- **アップロードを受信しながら、ターゲットがマウント中の別のイメージを読む**ことがある。`sd_io_lock` で直列化されるので、どちらも遅くなる（ターゲットの読み出しが遅くなっても、壊れない）。
- 1 つの CD ブロック（2048 バイト）= SD の 4 セクタ（512 バイト）。**クラスタが 2048 バイト以上（§3）なので、1 つの CD ブロックはクラスタの境界をまたがない**。
- 読み出しは、`iso_extent` が「連続した SD のセクタの区間（スパン）」の一覧を返し、グルーが区間ごとに `read_sectors` を呼ぶ。1 回の MSC の転送の単位で、先読みはしない。**性能の目標は `TODO(hw-verify)`**（Linux の ISO のブートに足りる速度）。
- **`GET /api/v1/iso` は、公開ビュー（RAM）から返す**（SD を読まない。`iso_op_mutex` も取らない）。`free_bytes` はキャッシュする。**更新するのは、マウント時と、ワーカー（`finish`、`DELETE` の完了）だけ**（FSInfo が無効なとき、FatFs は FAT 全体を数える。32GB のカードで数秒かかりうる）。**httpd のハンドラーからは更新しない**。**`begin` の空き検査は、`キャッシュ値 − 採取後に書いたバイト数（`consumed_since`）− 途中のイメージ（`uploading` と `incomplete`）の「`size_bytes` − `received`」の合計（これから使う分の予約）` で行う**。`consumed_since` は、キャッシュの採取後に、アップロードが書いたバイト数の合計（`write` のたびに加え、ワーカーがキャッシュを更新したら、0 に戻す）。**採取後に書いた分と、これから使う分を、別々に数える**（`10 GiB` で採取し、`6 GiB` のイメージを `5 GiB` まで受信して中断した場合: `10 − 5 − (6 − 5) = 4 GiB`。実際の空きは約 5 GiB で、安全側）。
- **diskio のラッパー（A-13）は、`disk_read` / `disk_write` だけでなく、`CTRL_SYNC`、`GET_SECTOR_COUNT`、`GET_SECTOR_SIZE`、`GET_BLOCK_SIZE`、`CTRL_TRIM`（`FF_USE_TRIM`）なども正しく処理する**。

## 5. USB: 仮想 CD-ROM

### 5.1 MSC の実装の方針（A-14 調査結果: 手段 1 を採用確定）
**一次情報（`espressif/esp_tinyusb 2.4.0` + `espressif/tinyusb 0.21.0~6`）をソースで確認した結果（2026-11-19、Antigravity）**:
`0.21.0~6` の `msc_device.c` では `tud_msc_inquiry2_cb`（`:50, 796`）と `tud_msc_async_io_done`（`:344`）が追加されたものの、依然として `proc_builtin_scsi()`（`:684-840`）が 8 コマンドを内蔵処理で捕捉するため仮想 CD-ROM の契約を満たせない。
- **`SCSI_CMD_MODE_SENSE_6`（`:805`）**: 4 バイト固定ヘッダー（`data_len=3, block_descriptor_len=0`）のみを返し、Mode Page（`2Ah` 等）を返せず `tud_msc_scsi_cb` も呼ばれない。
- **`SCSI_CMD_READ_FORMAT_CAPACITY`（`:756`）**: `descriptor_type=2` 固定の 12 バイト形式を返す。
- **`proc_read_io_data()`（`:878-883`）**: コールバックが `TUD_MSC_RET_ERROR`（`-1`）を返すと無条件で `set_sense_medium_not_present(lun)`（`02h/3Ah/00h`）を呼び、SD I/O エラーの `MEDIUM ERROR`（`03h/11h/00h`）を上書きする。
- **`SCSI_CMD_READ_12`（`A8h`）**: ストリーミング経路（`:327, 637` は `READ_10`/`WRITE_10` のみ）に乗らず、非 READ/WRITE コマンドは `CFG_TUD_MSC_EP_BUFSIZE` 制限の `tud_msc_scsi_cb` 経由になる。

→ **標準の MSC のままでは、仮想 CD-ROM の契約（デバイス種別 5、MODE SENSE のページ、READ のエラー、READ(12)、READ TOC など）を満たせない**。**契約: 「USB の MSC の層は、受け取った全ての CDB を `mmc_scsi_execute` に渡し、その応答（データ、センス、ステータス）を、そのまま返す」**。次の比較により **手段 1 を採用確定**とする（**C-13 `#143` の純 C `msc_bot` を使用**）。

| 順 | 手段 | 実装量 | 保守の負担 | 共存・技術根拠 | 判定 |
|---|---|---|---|---|---|
| 1 | **アプリのクラスドライバ（`usbd_app_driver_get_cb`）+ 純 C `msc_bot`（C-13）** | **中（約 300〜450 行）**（BOT 状態機械は C-13 で純 C 実装・単体検証、IDF 側はグルーと `iso_msc_task` のみ） | **小**（TinyUSB / `esp_tinyusb` を一切改変・フォークしない） | `usbd.c:457` は `TU_ATTR_WEAK usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t*)` を公開、`esp_tinyusb 2.4.0` 側は未定義で競合なし。`CONFIG_TINYUSB_MSC_ENABLED=n` で `CFG_TUD_MSC==0` となり内蔵 MSC を無効化したまま `HID×3`（`descriptors_control.c` の独自記述子）と共存可。`usbd_defer_func()`（`usbd.c:1527`）は `rhport` を絞らない `USBD_EVENT_FUNC_CALL`（`usbd.c:801-806`）を積むため ESP32-P4 OTG_HS（`rhport=1`）で `iso_msc_task` 完了通知にそのまま使える | **採用（確定）** |
| 2 | **TinyUSB の `msc_device.c` にパッチ**（`components/` 上書きまたは `override_path`） | 中〜大（8 個の内蔵ハンドラ改変、エラーセンス上書き除去、`READ(12)` ストリーミング追加、非同期 Reset/STALL 時 `command_done` 保証） | **大**（上流 `espressif/tinyusb` 更新のたびに全変更の追従・検証が必須、ビルド時も `override_path` / コンポーネント複製管理が要る） | TinyUSB の非 READ(10) IN コマンドが同期 `tud_msc_scsi_cb` 前提であり、`iso_msc_task` 非同期化・中断時 `command_done` 保証のために大規模改造が必要 | **不採用** |
| × | 標準の MSC + コールバックだけ | — | — | `MODE SENSE(6)` が 4 バイト固定、`READ_FORMAT_CAPACITY` が固定形式、`READ` エラー時に `02h/3Ah/00h` へ上書き、`READ(12)` 非ストリーミングのため契約違反 | **不採用** |

**MSC の読み出しを、USB のタスクで行わない（重要）**: `tud_task` は 1 つのタスクで、**HID の IN（キーボードとマウス）も同じタスクで処理する**。そこで `sd_io_lock` を待つ、または SD を読むと、アップロードの書き込みやカードの内部処理（数百 ms）の間、**キーボードとマウスが止まる**（本機の主機能）。→ **MSC 専用の I/O タスク（`iso_msc_task`）を置く**: USB のタスクは、CBW を受け取って `iso_msc_task` に渡し、**`mmc_scsi_execute` と `mmc_scsi_read_blocks`（ブロックする）は `iso_msc_task` が呼ぶ**。データは、2 つのバッファの交互（ダブルバッファ）で、USB のタスクが転送を発行し、`iso_msc_task` 完了時は `usbd_defer_func()` で `tud_task` を起こして CSW/次転送へ進む（**USB のタスクはブロックしない**）。
- **`iso_msc_task` の優先度とスタック**: `esp_tinyusb 2.4.0` の既定 `TinyUSB` タスクが優先度 `5`・スタック `4096 B`（`tinyusb_default_config.h:81-85`）であるため、`iso_msc_task` は `TinyUSB` より低い **優先度 `4`**（`ws_input` / `kvm_service` と同等、HID の `tud_task`・`usb_vbus` が常に優先される）、**スタック `4096 B`**（暫定。`mmc_scsi` + `iso_extent` + `sdmmc` + ログ分の余裕を `HW_VERIFY` I6 / I10 の実機 HWM で確定）とする。**`HW_VERIFY` I10 で、アップロードとターゲットの読み出しを同時にしたときの、HID の遅延を測る**。

**`msc_bot` のスレッドモデル**: **`msc_bot` の状態は、USB のタスクだけが触る**。`iso_msc_task` は「`mmc_scsi_execute` + `mmc_scsi_read_blocks`（ブロックする）」だけを担い、**完了を `usbd_defer_func()` / イベント経由で USB のタスクへ返す**。**BOT の Reset、`CLEAR_FEATURE(HALT)`、切断が、`iso_msc_task` の実行中に来たとき**: USB のタスクは、中止の印を立て、**`iso_msc_task` の完了イベントを待ってから**、`mmc_scsi_command_done` を呼ぶ（参照を放すのは、`command_done` だけ。§4.1）。C-13 のテストに「実行中の Reset」を含める。

（A-14 §1 の事前調査は 2026-11-19 完了・手段 1 確定。`C-13` `#143` の `msc_bot` 実装へ進む。）

### 5.2 デバイスの構成
- 今の複合デバイス（HID ×3、記述子 84 バイト）に、**MSC のインターフェースを 1 つ足す**。**インターフェース番号は、HID の後（3）に置く**（BIOS は、インターフェース 0 のブートキーボードを使う。今の動作を変えない）。クラス 0x08、サブクラス 0x06 = SCSI、プロトコル 0x50 = BOT。バルク IN/OUT の 2 本（HS は 512 バイト、FS は 64 バイト）。記述子は `usb_hid_logic`（純 C）で作っていて、ホストテストがある。MSC を足すと、**FS と HS の両方の設定記述子（+23 バイト）と、インターフェース数が変わる**ので、テストも直す。
- **LUN は 1 つ**。`USB_MSC_ENABLE`（既定 n）が有効なときだけ、記述子に入れる（無効なら、今の構成のまま）。
- **ターゲットから見えるのは、USB が接続されているとき**（`usb_vbus_ctrl` が接続させたとき）。マウント・取り外しは、USB が切断されていても受け付ける（メディアは、接続されたときに見える）。
- **ターゲットの BIOS が、HID と MSC の複合デバイスを扱えるか**は、実機で確認する（`TODO(hw-verify)`）。

### 5.3 INQUIRY
- **デバイス種別 5（CD/DVD）**、RMB = 1（取り外し可能）、ベンダー `QUADKVM `（8 文字）、製品 `Virtual CD-ROM  `（16 文字）、版 `0001`（4 文字）。INQUIRY の応答の全体（種別のバイトを含む）は、`mmc_scsi` が作る。

### 5.4 対応する SCSI コマンド（`mmc_scsi`。最小の集合。確定は C-11）
BIOS、UEFI、Linux（sr）、Windows が CD-ROM に対して発行する最小の集合。**文書（MMC / SPC）と照らして、C-11 で確定する**（照合した出典を、テストの説明に書く。確認できないものは `TODO(device-check)`）。

| コマンド（オペコード） | 動作 |
|---|---|
| TEST UNIT READY（00h） | メディアがあれば GOOD。なければ NOT READY（3Ah/00h: メディアなし） |
| REQUEST SENSE（03h） | 直近のセンスを返す |
| INQUIRY（12h） | §5.3。VPD ページは、未対応のページに ILLEGAL REQUEST（24h/00h） |
| MODE SENSE(6)（1Ah）/ (10)（5Ah） | 最小の応答（ヘッダー。ページ 2Ah のドライブの能力、ページ 3Fh のすべて）。MODE SELECT は未対応（ILLEGAL REQUEST） |
| START STOP UNIT（1Bh） | ロードは GOOD（何もしない）。**イジェクト（LoEj = 1、Start = 0）**: PREVENT が掛かっていれば CHECK CONDITION（05h/53h/02h: メディアの取り外しを防止中）。掛かっていなければメディアを取り外す（§5.5） |
| PREVENT ALLOW MEDIUM REMOVAL（1Eh） | 防止フラグを設定・解除 |
| READ CAPACITY(10)（25h） | 最終ブロックの LBA と、ブロック長 2048。メディアなしなら NOT READY |
| READ FORMAT CAPACITIES（23h） | 最小の応答（現在のメディアの大きさ） |
| READ(10)（28h）/ READ(12)（A8h） | 範囲外は ILLEGAL REQUEST（21h/00h: LBA が範囲外）。SD のエラーは MEDIUM ERROR（03h/11h/00h: 回復できない読み出しエラー）。**論理サイズ以降のバイトは 0** |
| READ TOC/PMA/ATIP（43h） | 形式 0（TOC）、1（マルチセッション情報）、2（生の TOC）。**1 セッション、1 データトラック**（トラック 1: 先頭 LBA 0、制御 = データ。リードアウト 0xAAh: 総ブロック数）。MSF のビットに従う（MSF が 8 ビットの範囲を超える大きなイメージの丸め規則は、MMC で確認する。`TODO(device-check)`） |
| GET CONFIGURATION（46h） | 現在のプロファイル（**DVD-ROM 0010h**。`TODO(hw-verify)`）と、最小のフィーチャー（プロファイル一覧、コア、取り外し可能メディア、ランダム読み出し） |
| GET EVENT STATUS NOTIFICATION（4Ah） | ポーリング形式。メディアのクラス: 新しいメディア、メディアの取り外し、取り外し要求 |
| READ DISC INFORMATION（51h）、READ TRACK INFORMATION（52h） | 最小の応答（完結したディスク、1 セッション、1 トラック） |
| READ SUB-CHANNEL（42h）、GET PERFORMANCE（ACh）、READ CD（BEh）など | 未対応（ILLEGAL REQUEST 20h/00h）。実機で必要と分かったものだけ足す |
| WRITE(10) など書き込み系 | DATA PROTECT（07h/27h/00h: 書き込み禁止）。**イメージは読み取り専用** |

センスキー/ASC/ASCQ の値は、標準の SPC の値（C-11 で文書と照合する）。

**呼び出しの形**: `mmc_scsi_execute(ctx, cdb, cdb_len, dir, expected_len, out, out_cap, result)`。**`dir`（IN / OUT / なし）と `expected_len` は、BOT の CBW の `bmCBWFlags` と `dCBWDataTransferLength`**。`result` は、ステータス、センス、`data_len`、READ の範囲。**データを受け取る（OUT）コマンド（WRITE、MODE SELECT など）は、データを受け取らず、DATA PROTECT / ILLEGAL REQUEST で失敗にする**。**CBW の 13 のケース（Hi > Dn など、`expected_len` と `data_len` の食い違い）の処理と、OUT のデータの読み捨て・STALL は、`msc_bot`（C-13）が行う**（`INTERFACES.md` §7.3）。

### 5.5 メディアの状態（`iso_media`）
- 状態: `EMPTY` / `LOADED(id)`。フラグ: `unit_attention`（メディア変更を 1 回だけ報告する）、`prevent_removal`（ターゲットが設定）、`event_pending`（GET EVENT STATUS で報告する）。
- **UNIT ATTENTION（28h/00h）は、INQUIRY と REQUEST SENSE を除く、次の最初のコマンドで 1 回だけ報告する**（SPC の規則。C-11 で文書と照合）。報告したら消す。
- **`prevent_removal` は、USB のバスリセット・切断・再接続、Web の `force` による取り外し、`force_eject`（SD の再初期化）で解除する**（ターゲットが再起動したとき、永久に `locked_by_target` にならないように）。**バスリセットの後は、UNIT ATTENTION（29h/00h: 電源投入またはリセット）を報告する**（C-11 で文書と照合）。
- **マウント**（Web）: `EMPTY` → `LOADED`、または `LOADED` → 別の `LOADED`（差し替え）。いずれも `unit_attention` と新メディアのイベントを立てる。**`prevent_removal` が立っているときの差し替えは 409 `locked_by_target`**（`force` で上書きできる）。
- **取り外し**（Web `eject`）: `prevent_removal` が立っていれば 409 `locked_by_target`（`force` で取り外す）。取り外すと、次のコマンドが NOT READY（メディアなし）になり、メディアの取り外しのイベントを立てる。
- **取り外し**（ターゲットの START STOP UNIT）: `prevent_removal` が掛かっていなければ、取り外す。Web の状態は `mounted: null`、`ejected_by_target: true`（**次の `mount` または `eject` まで残る**。`GET` では消えない）。
- **SD の再初期化（I/O エラー、抜き取り、`sd.state` が `ready` でなくなる）では、`prevent_removal` を無視して、強制的に `EMPTY` にする**（UNIT ATTENTION と取り外しのイベントを立てる。**`epoch` を進めて、進行中の READ を失敗させる**。§4.1）。**理由**: エクステントは、そのカードの生のセクタ番号。別のカードが入ったとき、マウントが残ると、ターゲットが別のカードの任意のセクタを ISO の続きとして読んでしまう。**再初期化の後は、自動でマウントしない**。`GET` は `mounted: null` を返す。
- **再起動後は、マウントしない**（`ISO_AUTOMOUNT_LAST` は、将来の選択肢。既定 n）。USB の再接続（ターゲットの電源の入れ直し）では、マウントを保つ（`prevent_removal` は解除）。

### 5.6 起動（ブート）
El Torito のブートは、ISO の中身（ブートカタログ）が持つ。デバイスは、**ブロックを正しく返すだけ**。ターゲット側の BIOS / UEFI で、起動順位を、この「USB CD-ROM」にするのは、オーナーの操作。UEFI の USB マスストレージのドライバが、デバイス種別 5 とブロック長 2048 の CD を扱えるかは、**一次情報で確認していない**（EDK2 のソースでの確認と、実機の UEFI での確認が要る。`TODO(device-check)`）。**実機で確認する**（`HW_VERIFY` I 章。Linux の ISO と Windows の ISO の両方）。

## 6. アップロードの方式

### 6.1 決定: 分割（チャンク）の PUT + 再開
- ブラウザは、ISO を `chunk_size` ごとに切り（`Blob.slice`）、**`PUT /api/v1/iso/<id>` を順に送る**（1 つの要求 = 1 つのチャンク）。要求のたびに `X-Upload-Offset`（その要求の先頭のバイト位置）を付ける。**サーバーは、`received` と一致するときだけ受け付ける**（不一致は 409 `offset_mismatch` と、現在の `received`）。再開は、`GET /api/v1/iso` の `received` から続ける。
- **`chunk_size` は、`begin` の応答で、サーバーが決める**（既定 128 KiB。`ISO_UPLOAD_CHUNK_KB`、16〜1024）。**`esp_http_server` は 1 つのタスクで動く**ので、1 つの要求の処理中は、同じサーバーの `/ws`（入力）と API が止まる。**チャンクは、全体をバッファに受信してから書く**（§3.1。バッファの確保元と、DMA の可否は A-15 が決める）。チャンクを小さくして、1 つの要求を短く（128 KiB で数十 ms の見込み）する。映像は専用のサーバー（`video_port`）なので、影響を受けない。**アップロード中の入力の遅延は、平均だけでなく最悪値も、実機で測る**（`HW_VERIFY` I10）。許容できなければ、アップロード専用のサーバー（別のポート）に移す（API はそのままで、ホストだけが変わる）。
- **`PUT` は、本体を読む前に、ヘッダーだけで判定する**（認証、Origin、`id`、状態、`X-Upload-Offset`、`Content-Length`、**空き容量（キャッシュ値。507）**。純 C の `iso_api_put_precheck`。`INTERFACES.md` 7.4）。**拒否するときは、本体を読まずに応答し、`Connection: close` を付ける**（残りの本体を、単一スレッドの httpd が読み捨てて止まるのを避ける）。
- **`iso_api_put_precheck`（ヘッダーだけ。ビューを読む）の後、本体を全部受信してから、`iso_op_mutex` を（try-lock で）取り、状態、`X-Upload-Offset`、`size` を、もう一度検査する**（受信の間に、ワーカーの `IDLE_CLOSE` や `DELETE` が状態を変えうる）。**1 つのチャンクの受信全体に期限を設ける**（`ISO_PUT_RECV_TIMEOUT_S`、既定 2 秒。ソケットの受信タイムアウトと合計の経過時間）。**超えたら、何も書かずに接続を閉じる**。帯域が細い経路（VPN など）で 128 KiB の受信が長引くと、その間 `/ws` の入力が止まるため。**クライアントは、`PUT` のネットワークエラーのとき、`GET /api/v1/iso` で `received` を取り直して続ける**（`Connection: close` で未読の本体を残して閉じると、TCP のリセットで、ブラウザが応答を読めず `TypeError` になりうる）。クライアントは、遅いときは、`chunk_size` より小さいチャンクを送ってよい（`Content-Length` は 1 以上 `chunk_size` 以下）。**専用サーバー（別のポート）へ移す条件**: `HW_VERIFY` I10 で、アップロード中の入力の遅延の最悪値が許容値（`TODO(hw-verify)`: オーナーが決める。目安 100 ms）を超えるとき。
- **SHA-256 は、受信しながら順に計算する**（mbedTLS）。`finish` で、宣言値（あれば）と比べる。**状態が失われたとき**（§3.4）は、`finish` の最終処理が SD を読み直して計算する（ワーカー）。
- **アップロードは、同時に 1 つだけ**（409 `upload_in_progress`）。**`finalizing` のイメージも、スロットを使っている扱い**（`begin` と `rescan` は 409）。アイドルが `ISO_UPLOAD_IDLE_S`（既定 300 秒）続くと、`incomplete` にする（ファイルを `f_sync`、`f_close` して、`meta` を書く。残す。SHA-256 の状態は保つ）。
- **`DELETE` は、`uploading` のイメージを削除できる**（アップロードの中止）。ファイルを閉じ、SHA-256 の状態を捨て、スロットを空ける。クライアントの `PUT` のループは、次の `PUT` で 404 を受ける。
- **途中で電源が落ちたとき**: 再起動後は、`meta` の状態が `uploading` のものを `incomplete` として扱い、`received` は同期済みのパーツのファイルの大きさから決める（§3.4）。

### 6.2 方式の選択肢（おすすめ順。オーナーが変更してよい）
| 順 | 方式 | 理由 |
|---|---|---|
| 1 | **分割の PUT + 再開（採用）** | 数 GB の転送で、途中の切断に強い。1 つの要求が短く、入力を止めない。サーバー側の状態は、`received`（ファイルの大きさ）だけ |
| 2 | 1 つの大きな PUT（ストリーミング） | 実装が最も単純。ただし、切れたら最初からやり直し。要求が数分続き、その間 `/ws` と API が止まる（採用しない） |
| 3 | multipart/form-data の POST | ブラウザの標準のフォームで送れる。ただし、境界の解析が要り、再開できず、1 つの要求が長い |
| 4 | WebSocket でバイナリを送る | 要求のオーバーヘッドが小さい。ただし、プロトコルを自前で作ることになり、既存の `/ws`（入力）と混ざる |
| 5 | デバイスが URL から取得 | PC を介さない。ただし、TLS の検証、メモリ、SSRF の対策が要る（後続の選択肢） |

## 7. セキュリティと安全

- **認証が必須**。変更する要求（POST / PUT / DELETE）は **Origin 必須**（Host と一致。無ければ 403）。既存の API（`WEB_API.md` §7）と同じ検査（`http_guard`）。
- **パスは、ユーザーの入力から作らない**。ディレクトリ名は `<id>`（サーバーが作る 8 桁の 16 進。乱数は `iso_random_t` で注入）。ユーザーの名前は、メタデータの中にだけ置く（表示用）。**名前の規則**: UTF-8 として正しい（不正なバイト列、過長な表現、サロゲートを拒否）、1〜63 バイト、**C0 制御文字（U+0000〜U+001F）、DEL（U+007F）、C1 制御文字（U+0080〜U+009F）を含まない**、`/` と `\` を含まない、前後に空白（U+0020）がない。**JSON の `\u` エスケープは、復号してから検査する**（サロゲートペアは 4 バイトの UTF-8 にする。対になっていないサロゲートは拒否）。**`sha256` の 16 進は、大文字小文字を受け付け、小文字にそろえて保存する**。**同名の判定は、バイト列の完全一致**（大文字小文字を区別する。Unicode の正規化はしない）で、409 `name_exists`。**ビューアは、名前を `textContent` で表示する**（`innerHTML` を使わない）。
- **上限**: イメージ 16 個（`ISO_MAX_IMAGES`）、1 つのイメージは 16 GiB まで（`ISO_MAX_PARTS` × 1 GiB）、**`size` は 34,816 バイト（= 2048 × 17。セクタ 0〜16。署名のセクタ 16 を含む）以上**。**署名の検査は、セクタ 16 の、バイト 1〜5 が `CD001`（ISO 9660）または `BEA01`（UDF）**、アップロードは同時に 1 つ、1 つのチャンクは `chunk_size` まで（超過は 413）、JSON のボディは 512 バイトまで（既存の API と同じ）。**カードの全体の大きさ（`total_bytes`）を超える `size` も、空き不足と同じ 507**。
- **イメージは読み取り専用**でターゲットに見せる（書き込み系は DATA PROTECT）。
- **SD の抜き取り・故障**: I/O のエラーを、MSC は MEDIUM ERROR で、API は 503 `sd_unavailable`（処理中の I/O エラー）または `not_ready`（処理の前に、`sd.state` が `ready` でない）で返す。SD が戻ったら、再初期化する（カード検出の GPIO がないので、定期的な再確認）。**書き込み中のエラーは、チャンクを `truncate` で取り消し、アップロードを `incomplete` にして、再開できるようにする**。**再初期化では、マウントを強制的に外す**（§5.5）。
- **`begin` と `DELETE` を繰り返すと、SD の書き込みが増える**（認証済みの利用者だけ。許容する）。
- **ログ**: 操作（`begin`、`finish`、`mount`、`eject`、`delete`）の結果と理由を、イメージの `id` と大きさだけ記録する。名前は、記録しない（個人の情報の可能性）。チャンクごとのログは出さない（数万回になる）。進捗は 5% ごと。

## 8. 担当と順序（Issue）

| ID | 内容 | 担当 | 備考 |
|---|---|---|---|
| **C-9** | この設計と契約（`ISO_MOUNT_DESIGN.md`、`INTERFACES.md` §7、`WEB_API.md` §8、`PLAN.md`、`HW_VERIFY.md` I 章）と、**シームの宣言（`components/iso_seams/include/iso_seams.h`）** | Claude | **この PR（#135）**。A-13 は、このヘッダーの合意の後に始める |
| **C-10**（#139） | 純 C: `iso_extent`、`iso_catalog`（メタデータ 256 バイトの符号化、2 スロット、名前の規則、起動時のスキャン、import）、`iso_upload`（受信の状態機械、再開、SHA-256 の注入、タイムアウト、`iso_upload_hash_step`）。ホストテスト、ファズ、変異テスト | Claude | `iso_extent` は、すぐ始めてよい |
| **C-11**（#140） | 純 C: `mmc_scsi`（§5.4。**全コマンド**）と `iso_media`（§5.5）。MMC / SPC の文書と照合。ホストテスト、ファズ、変異テスト | Claude | A-14 と並行 |
| **C-12**（#141） | 純 C: `iso_api`（要求の検査、`iso_api_put_precheck`、判定の順序、状態遷移）と `web_server` の結線の一部、ビューアの画面（一覧、アップロードの進捗と再開、マウント、取り外し、削除） | Claude | C-10、C-11 の後 |
| **A-13**（#137） | SD（**`iso_worker` の実装は、C-10 が `iso_core.h`（ジョブの型、ステップ関数、`iso_upload_tick`、ビュー更新のコールバック、`iso_media_force_eject` / `bus_reset` のプロトタイプ）を出した後の、A-13 の後半**。**前半は SD、FatFs、diskio、`epoch`、ロック**。**`CONFIG_FATFS_API_ENCODING_UTF_8`**、`epoch`、`iso_fs_t.truncate` の定義（§3.1）を含む）: SDMMC（スロット 0、GPIO39〜44、`ESP_LDO_VO4`、電源スイッチ GPIO45）、FAT32 のマウントと検査（§3）、**自前の diskio（`sd_io_lock`）**、`iso_blockdev_t`（`epoch` / `epoch_bump`。**SD の再初期化と `force_eject` が `epoch_bump` を呼ぶ。実行中の `FINISH` は、開始のときの `epoch` と、`ready` を書く前の `epoch` を比べ、違えば `ready` にせず `incomplete` にする**）と `iso_fs_t` の実装（`truncate`、**`read_at`**（`f_lseek` + `f_read`。読み取りのハンドルも `close` で閉じる。**同じハンドルで `read` と `read_at` を混ぜない。`read_at` の後の `read` の位置は未定義**: 位置を戻す実装は後ろ向きの seek で、1 GiB パーツのチェーンを毎回たどり直す）、**書き込み系の diskio は、マウントの `epoch` と現在の `epoch` が違えば `ISO_ERR_EPOCH` で失敗する（§3.4 の柵）**、`get_extents`、**【実装上のリスク: `read_at` のコスト】`FINISH` の再ハッシュは、ステップごとに読み取りハンドルを開き直す（ステップの間はハンドルを保持しない制約。`iso_job_step` は `iso_op_mutex` をステップごとに放すため）ので、`read_at` の `f_lseek` が、毎ステップ、1 GiB パーツのクラスタ連鎖（約 65536 クラスタ）を先頭から走査し直す。位置合わせの走査は、ステップのオフセットが大きいほど長く(そのステップの走査量はオフセットに比例)、1 つのパーツを読み切るまでの**走査量の合計は、おおむね `budget`（既定 1 MiB）に反比例して増える**(ステップ数が `budget` に反比例するため)。軽減策: (a) `FINISH` の `budget` を大きくする（例: 8 MiB。`iso_op_mutex` の保持時間が延びる）、(b) `read_at` で `CLMT`（`CREATE_LINKMAP`）を使う（開くたびに 1 回、連鎖を走査するので、同じ O(n)。単独では解決しない）、(c) ジョブの間だけ glue が読み取りハンドルを保持する（`iso_op_mutex` と `epoch` の扱いの設計し直しが要る）。実機で測る（`HW_VERIFY` I4）まで、方式を確定しない】**、`CONFIG_FATFS_USE_FASTSEEK=y`）、`iso_worker`（§4.2）。電圧・速度の実測 | Antigravity | |
| **A-14**（#136） | USB MSC: **最初に、§5.1 の調査（手段 1 か 2）**。**`iso_msc_task`（USB のタスクで SD を読まない）**。MSC の記述子（インターフェース 3）、`mmc_scsi` への結線（全コマンド）、`usb_hid_logic` のテストの更新。BIOS / UEFI での確認 | Antigravity | 調査の結果を、この文書 §5.1 に反映する |
| **C-13**（#143） | 純 C: **`msc_bot`**（BOT の状態機械: CBW の検証、13 のケース、データ IN / OUT / なし、STALL、CSW、リセット、`mmc_scsi_command_done` の確実な呼び出し）。ホストテスト、ファズ、変異テスト | Claude | A-14 の手段 1 のときに使う（手段 2 のときは、一部だけ） |
| **A-15**（#138） | HTTP: アップロードのハンドラー（受信 → バッファ → SD、mbedTLS の SHA-256、拒否時の `Connection: close`）、`/api/v1/iso*` の結線（**ワイルドカードの URI 照合、`max_uri_handlers`、`http_guard` に `PUT` / `DELETE` と `application/octet-stream`、ハンドラーのスタックの実測**）。入力の遅延と転送速度の実測 | Antigravity | C-12 と協調 |

**Kconfig の接頭辞**: `ISO_*` = Claude（API とロジック）、`SD_*` と `USB_MSC_*` = Antigravity。**`ISO_ENABLE` は、`SD_ENABLE` と `USB_MSC_ENABLE` に依存する**（どちらかが無効なら、ISO の機能は無効で、パスは登録しない）。`ISO_*` の項目: `ISO_ENABLE`、`ISO_MAX_IMAGES`（16）、`ISO_MAX_PARTS`（16）、`ISO_MAX_EXTENTS`（4096）、`ISO_FREE_RESERVE_MIB`（64）、`ISO_UPLOAD_CHUNK_KB`（128）、`ISO_UPLOAD_IDLE_S`（300）、`ISO_SYNC_KIB`（4096）、`ISO_AUTOMOUNT_LAST`（n）、`ISO_RESCAN_BATCH`（4）、`ISO_WORKER_QUEUE`（8）、`ISO_WORKER_BUDGET_KIB`（1024）、`ISO_OP_LOCK_WAIT_MS`（20）、`ISO_DELETE_REF_WAIT_S`（30）、`ISO_PUT_RECV_TIMEOUT_S`（2）。固定の定数: アップロードの `receiving` と `idle` の境（`ISO_UPLOAD_ACTIVE_S` = 5 秒）。SD の GPIO と LDO のチャンネルは、**回路図の固定の値**（Kconfig にしない。§1）。

## 9. 状態の正と、各部品のテスト（純 C）

| 部品 | 主な不変条件（テストで固定する） |
|---|---|
| `iso_extent` | LBA → (パーツ、オフセット) → SD セクタの変換が、パーツの境界・エクステントの境界・最終ブロック・パディングで正しい。範囲外は拒否。区間の併合が、順序と数を保つ |
| `iso_catalog` | `meta`（256 バイト）の符号化と復号が往復する（名前 63 バイト、CRC が名前と重ならない）。**2 つのスロットの選択**（世代番号、巻き戻り、一方が壊れている、両方が壊れている）。壊れた `meta` の拒否。名前の規則（UTF-8、長さ、禁止文字）。**状態の遷移表（§3.4）の全ての行**。2 スロットの規則（無効 / 存在しない / 同世代）。同名の拒否。上限。起動時のスキャン（`uploading` → `incomplete`、孤立ディレクトリ、不正な名前）。import（名前の衝突、上限、拡張子の大小） |
| `iso_upload` | `received` と一致しないオフセットの拒否。チャンクの上限。チャンクがパーツの境界をまたぐときの分割。書き込み失敗の `truncate` と `received`。SHA-256 の状態が失われたときの再計算。同時 1 つ（`finalizing` を含む）。アイドルのタイムアウト。`finish` の判定（サイズ、SHA-256、ISO の署名 `CD001` / `BEA01`、`too_fragmented`）。時刻の巻き戻り |
| `msc_bot` | **実行中の Reset（`iso_msc_task` の完了を待ってから `command_done`）**、CBW の検証（署名、LUN、長さ）、13 のケース（Hn / Hi / Ho と Dn / Di / Do の組み合わせ）、データ IN の途中の失敗、OUT のデータの読み捨て、STALL とリセットからの復帰、`mmc_scsi_command_done` を、どの終わり方でも 1 回だけ呼ぶこと |
| `iso_media` | メディアの変更が、1 回だけ UNIT ATTENTION になる。`prevent_removal` と取り外し・差し替えの規則。バスリセットで `prevent_removal` が解除される。SD の再初期化での強制取り外し。ターゲットの取り外しが、Web の状態に残る（次の `mount` / `eject` まで） |
| `mmc_scsi` | 各コマンドの応答のバイト列（手書きの期待値）。CDB の長さ・予約ビットの検査。メディアなし / ありの両方。範囲外、書き込み、未対応のコマンドのセンス。論理サイズ以降のゼロ埋め |
| `iso_api` | 判定の順序（`WEB_API.md` §8）。状態ごとのエラーの対応表。`iso_api_put_precheck`（本体を読む前の判定） |

## 10. 未確認一覧（`TODO(hw-verify)` / 調査。`HW_VERIFY.md` I 章と同じ番号）

| I 番号 | 内容 | 確認する人 |
|---|---|---|
| I2 | `ESP_LDO_VO4` の電圧と、IDF での有効化。電源 OFF（GPIO45 = High）のときの逆給電の有無 | A-13、オーナー（測定） |
| I3 | microSD の 4 ビットのモードの速度（40 MHz）、FAT32 の読み書きの実効速度 | A-13 |
| I4 | **FAT のチェーンの走査によるエクステントの取得**（`get_extents`。読み取りモードで開いて開始クラスタを得る）が動くこと。断片化したときの挙動（`too_fragmented`）。**`FINISH` の再ハッシュの全体の所要時間と、ステップごとの `f_lseek` の時間**（§8 の A-13 のリスク） | A-13 |
| I5 | SD の直列化（自前の diskio）で、アップロードと MSC の読み出しが同時に動くこと | A-13、A-15 |
| I6 | **MSC の実装の手段**（アプリのクラスドライバ、または TinyUSB のパッチ）。esp_tinyusb 2.4.0 が持つ TinyUSB の版。`iso_msc_task` の優先度とスタック | A-14（最初の調査） |
| I7 | USB OTG HS の FIFO の割り当て（HID ×3 の IN + MSC の IN/OUT。合計 4096 バイト）が足りること | A-14 |
| I8 | HID + 仮想 CD-ROM の複合デバイスを、BIOS / UEFI が認識すること。Linux の ISO と Windows の ISO の起動 | A-14、オーナー |
| I9 | READ TOC / GET CONFIGURATION のプロファイル（DVD-ROM）が、Windows、Linux、UEFI で問題ないこと | A-14 |
| I10 | アップロード中の入力（`/ws`）と **HID（キーボード・マウス）の遅延（平均と最悪値）**。アップロードの実効速度。**ワーカーの稼働中（`DELETE`、`finalizing`、`INDEX`）の `PUT` の遅延**。**アップロードとターゲットの読み出しを同時にしたときの HID の遅延**（`iso_msc_task`）。`PUT` の拒否時の `Connection: close` が効くこと | A-14、A-15 |
| I11 | 電源断・SD の抜き取りのとき、`incomplete` として復帰し、再開できること。壊れた `meta` の扱い（2 スロット）。**`import` の `rename` の途中の電源断（クロスリンクの検出）**。`DELETE` の途中の電源断（`deleting` の再開）。**`FINISH` の最中の SD の差し替え（書き込みの柵、§3.4）** | A-13、A-15 |
| I12 | `locked_by_target`、ターゲットの START STOP UNIT による取り外し | A-14 |

## 11. 検討した他の案（記録。変更してよい）

### 保存方式（オーナー決定: 案 1）
| 順 | 案 | 内容と理由 |
|---|---|---|
| 1 | **FAT32 + 分割 + 直接セクタ読み出し（採用）** | IDF を改造しない。32GB のカードは FAT32 が標準。変換は純 C。`f_expand` は使わず、追記 + FAT のチェーンの走査によるエクステント（§3.1） |
| 2 | exFAT を有効化し、ISO を 1 ファイルで保存 | カードをそのまま使え、1 ISO が 1 ファイル。ただし `ffconf.h` が `FF_FS_EXFAT 0` で固定で、IDF の `fatfs` の複製（6.0.1 に固定）の保守と、特許面の確認が要る。後から、§2 のエクステントの層の下だけを替えて切り替えられる |
| 3 | SD を専用の生領域として管理 | 読み出しは最も単純。ただし領域管理を自前で作る。PC で中身が見えない |
| 4 | 小さな FAT32 + 大きな生領域 | 設定は PC で見え、ISO は連続領域。両方の管理が要る |
| 5 | 4 GiB 超は非対応 | 最も簡単。ただしオーナーの要件（4 GiB 超）に合わない |

### 提示方式
| 順 | 案 | 内容と理由 |
|---|---|---|
| 1 | **仮想 CD-ROM（採用）** | Windows の ISO も起動できる（オーナーの要件）。El Torito をそのまま使える |
| 2 | 仮想ディスク（MSC のディスク型。TinyUSB の既定） | 実装が最も単純。Linux の isohybrid は起動できるが、Windows の ISO は、そのままでは起動できない |
| 3 | 仮想ディスク + 自前の変換（ISO から FAT のディスクイメージを動的に作る） | Windows も起動できる可能性があるが、複雑で、ISO の中身に依存する |
| 4 | 仮想 FD / ZIP ドライブ | 小さなイメージ向き。ISO には合わない |
| 5 | 複数の LUN（CD + ディスク） | 将来の拡張。今は LUN 1 つ |
