# ISO の仮想メディア（アップロード + USB マウント）の設計 (P8)

ブラウザから ISO をアップロードして microSD に保存し、ターゲット PC に **USB の仮想 CD-ROM** として見せる機能。**この文書が契約**（API は `docs/WEB_API.md` §8、境界は `docs/INTERFACES.md` §7）。担当: 設計・純 C・API・ビューア = Claude、SD・USB・実機 = Antigravity。

> **版**: 初版（PR #135）の独立レビューで、`meta` のレイアウトの重なり、`rename` の原子性の誤り、TinyUSB の標準の MSC が契約を満たさないこと、判定の順序と表の食い違い、長い同期処理が httpd を止めること、などが見つかり、**全面的に直した**（この版）。

## 0. オーナーの決定（2026-11-18）と、この文書での扱い

| 項目 | 決定 |
|---|---|
| 機能 | ISO を**アップロード**し、**USB 経由でマウント**する。ISO は **microSD に保存**する |
| 対象の ISO | **Linux の ISO だけでなく、Windows の ISO も起動できる**こと → **仮想 CD-ROM のエミュレーションが必須**（ディスクとして見せるだけでは足りない。§5） |
| ISO のサイズ | **4 GiB を超える ISO も扱う**（Windows の ISO は約 5〜7 GB） |
| カード | 32GB の microSD。**FAT32 に再フォーマットして使う**（オーナーが PC で行う。デバイスはフォーマットしない。§3） |
| 保存方式 | **FAT32 に、固定サイズのパーツに分割して保存し、SD のセクタを直接読む**（保存方式の案 1。§11） |
| SD の用途 | 計画書の GPIO 割り当て（`PLAN.md` §4.4）に SD はない。**ISO 専用**（回路図で GPIO39〜45 を使うことを確認済み。§1） |

**実装上の判断（オーナー決定の範囲内）**: 案 1 の「連続確保」は、**`f_expand` を使わない**ことにした（§3.1）。理由: 1 GiB の確保で FAT の更新が数百 ms〜秒かかり、**単一スレッドの httpd が止まる**（キーボードとマウスの入力が止まる）。電源断で、どのファイルにも属さないクラスタの連鎖が残る。中断からの再開で、パーツを作り直す必要が出る。通常の追記なら、FAT32 は空きの先頭から順に割り当てるので、フォーマット直後は自然に連続に近くなり、**連続でなくても、エクステントの一覧（CLMT）で読める**。パーツに分割して直接セクタを読む、という案 1 の本質は変わらない。

**変更してよいもの**: 数値（パーツの大きさ、チャンクの大きさ、各上限）は、実機の測定（`HW_VERIFY.md` I 章）で変えてよい。保存方式は、§2 の「エクステントの一覧」の層が隠すので、後から別の方式（exFAT の有効化など）に切り替えても、上の層は作り直しにならない。

## 1. 確認した事実と、未確認のこと

**確認した（一次情報）**
- **microSD の結線**（回路図 `docs/ESP32-P4-ETH-schematic.pdf` 1 ページ「MicroSD Card」を目視。2026-11-18）: D0=GPIO39、D1=GPIO40、D2=GPIO41、D3(CD)=GPIO42、CLK=GPIO43、CMD=GPIO44（4 ビットのモード）。**データ線（D0〜D3）と CMD は 51kΩ（R8〜R12）で `ESP_LDO_VO4` にプルアップ**（CLK にはない）。CD2 も 51kΩ（R13）でプルアップされているだけで、GPIO には接続されていない。**カードの電源**は `ESP_3V3` から PMOS（Q1、SI2301CDS）経由で、ゲートは **GPIO45**（10kΩ で GND にプルダウン: GPIO45 が Low または未設定で**電源 ON**、High で OFF）。**カード検出の GPIO はない**。
- ESP-IDF v6.0.1 の FatFs（`components/fatfs/src/ffconf.h`、`Kconfig`）: R0.15。**`FF_FS_EXFAT 0` で固定**（menuconfig に項目がない）。`FF_USE_EXPAND 1`（使わない。§3.1）、`FF_USE_MKFS 1`（使わない）。**`FF_USE_FASTSEEK` は `CONFIG_FATFS_USE_FASTSEEK` に従い、既定は `n`**。**fast-seek と CLMT は、読み取りモードで開いたファイルだけ**。`f_lseek(CREATE_LINKMAP)` は、表が小さいと `FR_NOT_ENOUGH_CORE` を返し、`cltbl[0]` に必要な数を入れる。**`f_rename` は、宛先が存在すると `FR_EXIST` で失敗する**（上書きできない。原子的な置換ではない）。`FF_LBA64` は無効（`LBA_t` は 32 ビット）。`FF_FS_REENTRANT 1`。`fs->database` は絶対 LBA で、`クラスタのセクタ = database + csize × (クラスタ番号 − 2)`。
- ESP32-P4 の USB OTG HS（TinyUSB の `dwc2_esp32.h`）: エンドポイント 16 本（IN は 8 本）、FIFO の合計 4096 バイト。HID ×3 に MSC のバルク IN/OUT を足す本数の余裕はある（FIFO の割り当ては §10 の確認項目）。
- **TinyUSB 0.18.0 の `msc_device.c`（Claude がソースで確認）**: 標準の MSC は、**TEST UNIT READY、START STOP UNIT、PREVENT ALLOW MEDIUM REMOVAL、READ CAPACITY(10)、READ FORMAT CAPACITY、INQUIRY、MODE SENSE(6)、REQUEST SENSE を内蔵の処理で先に処理し、アプリのコールバックに回さない**。アプリの `tud_msc_scsi_cb` に回るのは、内蔵が処理しなかった（負を返した）コマンドだけ。INQUIRY は 36 バイト固定で、種別は 0（ディスク）。MODE SENSE(6) はヘッダーだけ。**READ(10) のコールバックが負を返すと、センスは無条件に「NOT READY / メディアなし（3Ah）」に上書きされる**（READ エラーのセンスを返せない）。非 READ/WRITE のコマンドは、データの大きさが `CFG_TUD_MSC_EP_BUFSIZE` に制限される。→ **標準の MSC のままでは、仮想 CD-ROM の契約（§5）を満たせない**（§5.1）。
- 32GB のカードは、SD の規格上 SDHC で、FAT32 で出荷されるのが標準。セクタは 512 バイト。
- `esp_http_server` は **1 つのタスク**で、`select` → 1 つの要求の処理 → 次、と回る（`httpd_main.c`）。**1 つの要求の処理中は、同じサーバーの他の要求（`/ws` の入力を含む）が待つ**。

**未確認（`TODO(hw-verify)` / 調査）**
- esp_tinyusb 2.4.0 が持つ TinyUSB の版と、**アプリのクラスドライバ（`usbd_app_driver_get_cb`）で MSC を自前実装できるか**、esp_tinyusb 側の MSC を無効にする方法（A-14 の最初の調査。§5.1）。
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

- **純 C（ホストでテストする。Claude）**: `iso_extent`、`iso_catalog`（メタデータの符号化を含む）、`iso_upload`、`iso_media`、`mmc_scsi`、`iso_api`。IDF のヘッダーに依存せず、時刻・乱数・I/O は関数ポインタで注入する。
- **IDF の結合（Antigravity）**: SD（SDMMC、LDO、電源スイッチ）、FAT32（FatFs）、SD の I/O の直列化、USB の MSC クラス、HTTP のハンドラー、**ワーカータスク**（§4.2）、ロックの方針（§4.1）。
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
- **論理サイズは 2048 バイトの倍数に切り上げる**。ISO の大きさが 2048 の倍数でないとき、**論理サイズ以降のバイトは 0 を返す**。**このゼロ埋めは `mmc_scsi_read_blocks` が、バイト単位で行う**（`iso_extent` は、セクタ単位の区間しか返さない。ファイルの最後のセクタの、EOF より後ろのバイトは、0 とは限らない）。
- **`f_expand` は使わない**。アップロードは、パーツを先頭から順に、**通常の `f_write` で追記**する。**1 つのチャンクは、全体をバッファに受信してから、書く**（接続が途中で切れたとき、何も書かない）。**書き込みに失敗したら、そのパーツのファイルを、チャンクを書く前の大きさに `truncate` する**（`iso_fs_t.truncate`）。**チャンクがパーツの境界をまたぐときは、書き込みを分割する**（最初のパーツの残りまで書き、次のパーツを作って続きを書く）。
- **`f_sync` は、`ISO_SYNC_KIB`（既定 4096 = 4 MiB）ごと、パーツの終わり、アイドルのタイムアウト、`finish`、中止のとき**に行う。**同期したときの、パーツのファイルの大きさが、`received` の正**（§3.4）。
- **`begin` で空き容量（`size` + 予備 `ISO_FREE_RESERVE_MIB` = 64 MiB）を検査する**（足りなければ 507）。再開した最初の `PUT` でも、**残りの大きさ + 予備**を検査する（中断中に、別のイメージが空きを使ったとき）。
- **連続は必須ではない**: 読み出しは、`CLMT`（クラスタのリンクマップ）から作る**エクステントの一覧**で行う。**エクステントの数が `ISO_MAX_EXTENTS`（既定 4096。イメージ全体）を超える**、または `FR_NOT_ENOUGH_CORE` のときは、そのイメージを **`invalid`（理由 `too_fragmented`）**にする（`finish` の最終処理で検査する。§3.4）。エクステントの一覧は **PSRAM** に置く。**`get_extents` は、読み取りモードで開いたファイルで行う**（fast-seek / CLMT は、読み取りモードだけ）。

### 3.2 メタデータ（256 バイト固定、リトルエンディアン）
| オフセット | 大きさ | 内容 |
|---|---|---|
| 0 | 4 | マジック `ISO1` |
| 4 | 2 | 版（1） |
| 6 | 1 | 状態: 0 = `uploading`、1 = `ready`、2 = `incomplete`、3 = `invalid` |
| 7 | 1 | `invalid` の理由: 0 = なし、1 = `checksum_mismatch`、2 = `not_iso`、3 = `too_fragmented` |
| 8 | 8 | 論理サイズ（バイト。アップロードの宣言値。2048 に切り上げる前の実サイズ） |
| 16 | 8 | 予約（0。`received` はここに持たない。§3.4） |
| 24 | 2 | パーツの数 |
| 26 | 2 | 予約（0） |
| 28 | 4 | パーツの大きさ（ブロック数。既定 524288。`import` では、そのファイルの大きさ（ブロック数、切り上げ）） |
| 32 | 32 | SHA-256（`ready` のとき） |
| 64 | 1 | SHA-256 が有効か（0 / 1） |
| 65 | 1 | 名前の長さ（バイト。1〜63） |
| 66 | 64 | 名前（UTF-8。残りは 0） |
| 130 | 4 | **世代番号**（書くたびに +1。下の読み書き） |
| 134 | 118 | 予約（0） |
| 252 | 4 | **先頭の 252 バイトの CRC-32（IEEE 802.3）** |

- **読み書き（2 つのスロット `meta.a` / `meta.b`）**: **`rename` では置き換えない**（FatFs の `f_rename` は、宛先が存在すると失敗する。`unlink` + `rename` は原子的でない）。書くときは、**古い方のスロットに、世代番号 +1 の内容を上書きして `f_sync`** する。読むときは、**マジック・版・CRC が正しい方のうち、世代番号が新しい方**を採用する（世代番号の比較は、巻き戻り（`UINT32_MAX` の次）を考慮した差の符号で行う）。電源が途中で落ちても、少なくとも一方は有効なまま残る。
- **両方のスロットが無効**、またはスロットがない `<id>/`（`mkdir` と最初の `meta` の書き込みの間で電源が落ちた）は、**一覧に `invalid` として出す**（名前は `<id>`、理由は `meta_corrupt`。**`meta` には保存しない実行時の理由**）。削除はできるが、マウントはできない。
- 状態が `uploading`（0）のまま読まれたものは、**`incomplete` として扱う**（電源断。§3.4）。

### 3.3 PC から直接コピーする（`import`）
`/iso/import/` に、PC で **4 GiB 弱までの ISO ファイル**（`*.iso`。拡張子は大文字小文字を区別しない）をコピーすると、`POST /api/v1/iso/rescan`（と起動時）で、イメージとして取り込める。
- **取り込みは `rename` で `/iso/<id>/p000` に移す**（コピーしない。ディレクトリを作ってから移すので、宛先は存在しない）。名前はファイル名（拡張子を除く）。**名前の規則に合わない、または同名がある、または `ISO_MAX_IMAGES` に達している**ファイルは、取り込まず `skipped` に数える（ファイルは `import/` に残す）。
- **SHA-256 は計算しない**（`sha256: null`）。**`not_iso` の検査（セクタ 16 の署名）だけ行い、すぐ `ready` にする**。エクステントの作成は、通常の `ready` と同じく、ワーカーが行う（§4.2。それまで `finalizing`）。`rescan` は**同期**で返る（読むのは、各ファイルのセクタ 16 の 1 回だけ）。**書き込み途中のファイルを取り込む危険**を避けるため、`rescan` は、ユーザーの操作（または起動時）でだけ行う。
- **4 GiB を超える ISO は、FAT32 のファイルとして置けないので、ブラウザのアップロード（パーツに分割される）を使う**。
- **起動時のスキャン**（`iso_catalog` の仕事）: `/iso/` の直下の各ディレクトリを調べる。名前が `<id>` の形式でないものは無視する。`ISO_MAX_IMAGES` を超える分は、一覧に出さず、ログに 1 行。

### 3.4 状態と `received`

```
begin ──▶ uploading ──(全バイト受信 + finish)──▶ finalizing ──▶ ready
              │ アイドルのタイムアウト / 電源断 / 再起動 / SD のエラー
              ▼
         incomplete ──(再開: 同じ id に続きを PUT)──▶ uploading
 finalizing の検査に失敗 ──▶ invalid（理由つき。削除のみ）
```
- **`received`（受信済みのバイト数）の正は、同期済みのパーツのファイルの大きさの合計**（先頭から順に数え、パーツの大きさに満たないパーツで止める）。メモリ上の値は、その時点の同期前の分を含むが、**再起動すると、同期済みの分に戻る**。クライアントは、`offset_mismatch` の `received`、または `GET` の `received` から続ける。
- **SHA-256 の逐次の状態は、メモリ上にだけある**。**再起動、SD のエラーによる巻き戻し（`truncate`）で失われる**。失われたときは、`finish` の最終処理（`finalizing`）が、SD を読み直して計算する。アイドルのタイムアウトでは、状態を保つ（失わない）。
- **`finalizing`**（SHA-256 の計算、エクステントの作成、ISO の署名の検査）と **`deleting`** は、**メタデータに保存しない実行時の状態**（API の `state` にだけ出る。再起動すると、`ready` のイメージは、起動時にワーカーが再び `finalizing`（エクステントの作成）にする）。
- **マウントできるのは `ready` だけ**（エクステントが作られているもの。`finalizing` の間は 409 `image_not_ready`）。
- **マウント中のイメージは、削除も、変更もできない**（409 `mounted`）。
- **`ready` になるのは、全パーツを `f_sync` と `f_close` した後**。
- **`invalid` の理由**は、`meta` に保存する（`checksum_mismatch`、`not_iso`、`too_fragmented`）。`GET` の `invalid_reason` に出る。

## 4. 排他、ワーカー、性能

### 4.1 ロックの方針
- **純 C の部品は、スレッドセーフではない**。IDF のラッパーが、**1 つのミューテックス（`iso_state_mutex`）**で、`iso_catalog` / `iso_upload` / `iso_media` / `iso_api` の呼び出しを直列化する。**このミューテックスは、短い区間だけ取り、SD の I/O やネットワークの I/O の間は持たない**。
- **SD の I/O は、別のロック（`sd_io_lock`）で直列化する**。FatFs の読み書き（diskio）と、MSC の直接読み出し（`iso_blockdev_t`）の**両方**が、同じロックを取る。IDF の `esp_vfs_fat` の標準の diskio は、このロックを持たないので、**自前の diskio のラッパー**（FatFs の `ff_diskio_register` で登録するもの）で、ロックを取ってから `sdmmc_*` を呼ぶ（A-13）。FatFs の `f_write` は 1 回の `disk_write` をクラスタの境界で分けるので、**ロックの保持は、1 回の `disk_write`（クラスタ以下）に限る**。**`iso_state_mutex` を持ったまま `sd_io_lock` を取らない**（デッドロックの防止。順序は `sd_io_lock` が内側）。
- **READ の経路**: `mmc_scsi` は、`iso_state_mutex` の下で、**エクステントの表のスナップショット（参照カウント付き）を取り**、ミューテックスを放してから、SD を読む。**`eject` は、参照を落とすだけ**で、進行中の READ の完了を待たない（その READ は、古いスナップショットで完了してよい。表は、参照カウントが 0 になったら解放する）。次のコマンドからは、NOT READY（メディアなし）。

### 4.2 ワーカータスク（A-13 / A-15）
**`esp_http_server` は 1 つのタスクで動く**ので、**1 つの要求の処理は短く保つ**（数十 ms まで）。長い処理は、**専用のワーカータスク（`iso_worker`）**で行い、API は `202` と状態で返す。

| 処理 | 時間（推測。`TODO(hw-verify)`） | 扱い |
|---|---|---|
| `finish` の最終処理: SHA-256 の再計算（SD の読み直し）、エクステントの作成（`CLMT`: FAT の連鎖を読む。1 GiB のパーツで数百 ms）、ISO の署名の検査 | 数秒〜数分 | ワーカー。`finish` は `202 {"state":"finalizing"}` |
| 起動時のエクステントの作成（`ready` のイメージごと） | 16 GiB で十数秒 | ワーカー（バックグラウンド）。その間、そのイメージは `finalizing` |
| `DELETE`（最大 16 個のパーツの `f_unlink`。1 GiB で FAT の連鎖の解放が数百 ms） | 数秒 | ワーカー。`DELETE` は `202 {"state":"deleting"}`。終わると一覧から消える |
| `rescan` | 短い（イメージごとに 1 セクタ） | 同期 |
| チャンクの `PUT`（受信 + `f_write`） | 128 KiB で数十 ms | 同期（httpd のスレッド） |

- ワーカーは、**1 ステップずつ進める純 C の関数**（`iso_upload_hash_step`、`iso_index_step`。`INTERFACES.md` 7.2）を、`sd_io_lock` を 1 回の I/O ごとに取りながら呼ぶ。進行中も、他の SD の I/O（MSC の読み出し、アップロード）を、極端に止めない。
- アップロードを受信しながら、ターゲットがマウント中の別のイメージを読むことがある。ロックで直列化されるので、どちらも遅くなる（ターゲットの読み出しが遅くなっても、壊れない）。
- 1 つの CD ブロック（2048 バイト）= SD の 4 セクタ（512 バイト）。**クラスタが 2048 バイト以上（§3）なので、1 つの CD ブロックはクラスタの境界をまたがない**（4 セクタが、同じクラスタの中の連続したセクタに入る）。
- 読み出しは、`iso_extent` が「連続した SD のセクタの区間（スパン）」の一覧を返し、グルーが区間ごとに `read_sectors` を呼ぶ。1 回の MSC の転送の単位で、先読みはしない。**性能の目標は `TODO(hw-verify)`**（Linux の ISO のブートに足りる速度）。
- **`GET /api/v1/iso` は、カタログを RAM から返す**（SD を毎回読まない）。`free_bytes` はキャッシュする（アップロードのたび、`finish`、`DELETE` で更新）。ポーリングされても、`sd_io_lock` を取り合わない。

## 5. USB: 仮想 CD-ROM

### 5.1 MSC の実装の方針（標準の TinyUSB の MSC では足りない）
§1 のとおり、**標準の TinyUSB の MSC は、INQUIRY（種別 5）、MODE SENSE のページ、REQUEST SENSE とセンスの制御、READ のエラーのセンス、READ FORMAT CAPACITY を、内蔵の処理で決めてしまう**。仮想 CD-ROM（Windows の ISO を含む）に必要なものを、アプリが決められない。したがって、**契約は「USB の MSC の層は、受け取った CDB を、全て `mmc_scsi_execute` に渡し、その応答（データ、センス、ステータス）を、そのまま返す」とする**。次のどちらかの手段で実現する（**A-14 の最初の調査で決める**）。

| 順 | 手段 | 内容 | 理由 |
|---|---|---|---|
| 1 | **アプリのクラスドライバ（`usbd_app_driver_get_cb`）で、BOT（CBW / CSW）を自前で実装**（推奨候補） | TinyUSB の MSC クラスを使わず、MSC のインターフェース（バルク IN/OUT）を自前のクラスドライバが持つ。HID は esp_tinyusb のまま | TinyUSB と esp_tinyusb を改造しない。全コマンドと全センスを `mmc_scsi` が持てる。BOT の状態機械（CBW → データ → CSW、STALL、リセット）は、純 C に分けて、ホストでテストできる。**実装量は大きい** |
| 2 | **TinyUSB の `msc_device.c` にパッチ**（内蔵の処理を外す / 全てを `tud_msc_scsi_cb` に回す、READ のエラーのセンスを返せるようにする） | `components/` での上書き、または `idf_component.yml` の `override_path` | 実装量は小さい。ただし、TinyUSB の更新のたびに、パッチの追従が要る |
| × | 標準の MSC + コールバック（`tud_msc_inquiry_cb` など）に分解 | — | **採用しない**: INQUIRY の種別が 0 のまま、MODE SENSE のページが返せず、READ のエラーのセンスが上書きされる。Windows の ISO の起動に足りない |

**A-14 が調査すること**: (a) esp_tinyusb 2.4.0 が持つ TinyUSB の版（`tud_msc_inquiry2_cb` の有無を含む）。(b) `usbd_app_driver_get_cb` が使えるか、esp_tinyusb の複合デバイスの記述子・HID と共存できるか（esp_tinyusb の MSC は `CONFIG_TINYUSB_MSC_ENABLED` で無効にできるか）。(c) 手段 1 と 2 の、実装量と保守の負担。結果を、この表と §5.2 に反映する。

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

### 5.5 メディアの状態（`iso_media`）
- 状態: `EMPTY` / `LOADED(id)`。フラグ: `unit_attention`（メディア変更を 1 回だけ報告する）、`prevent_removal`（ターゲットが設定）、`event_pending`（GET EVENT STATUS で報告する）。
- **UNIT ATTENTION（28h/00h）は、INQUIRY と REQUEST SENSE を除く、次の最初のコマンドで 1 回だけ報告する**（SPC の規則。C-11 で文書と照合）。報告したら消す。
- **`prevent_removal` は、USB のバスリセット・切断・再接続で解除する**（ターゲットが再起動したとき、永久に `locked_by_target` にならないように）。
- **マウント**（Web）: `EMPTY` → `LOADED`、または `LOADED` → 別の `LOADED`（差し替え）。いずれも `unit_attention` と新メディアのイベントを立てる。**`prevent_removal` が立っているときの差し替えは 409 `locked_by_target`**（`force` で上書きできる）。
- **取り外し**（Web `eject`）: `prevent_removal` が立っていれば 409 `locked_by_target`（`force` で取り外す）。取り外すと、次のコマンドが NOT READY（メディアなし）になり、メディアの取り外しのイベントを立てる。
- **取り外し**（ターゲットの START STOP UNIT）: `prevent_removal` が掛かっていなければ、取り外す。Web の状態は `mounted: null`、`ejected_by_target: true`（**次の `mount` または `eject` まで残る**。`GET` では消えない）。
- **SD の再初期化（I/O エラー、抜き取り、`sd.state` が `ready` でなくなる）では、`prevent_removal` を無視して、強制的に `EMPTY` にする**（UNIT ATTENTION と取り外しのイベントを立てる）。**理由**: エクステントは、そのカードの生のセクタ番号。別のカードが入ったとき、マウントが残ると、ターゲットが別のカードの任意のセクタを ISO の続きとして読んでしまう。**再初期化の後は、自動でマウントしない**。`GET` は `mounted: null` を返す。
- **再起動後は、マウントしない**（`ISO_AUTOMOUNT_LAST` は、将来の選択肢。既定 n）。USB の再接続（ターゲットの電源の入れ直し）では、マウントを保つ（`prevent_removal` は解除）。

### 5.6 起動（ブート）
El Torito のブートは、ISO の中身（ブートカタログ）が持つ。デバイスは、**ブロックを正しく返すだけ**。ターゲット側の BIOS / UEFI で、起動順位を、この「USB CD-ROM」にするのは、オーナーの操作。UEFI の USB マスストレージのドライバが、デバイス種別 5 とブロック長 2048 の CD を扱えるかは、**一次情報で確認していない**（EDK2 のソースでの確認と、実機の UEFI での確認が要る。`TODO(device-check)`）。**実機で確認する**（`HW_VERIFY` I 章。Linux の ISO と Windows の ISO の両方）。

## 6. アップロードの方式

### 6.1 決定: 分割（チャンク）の PUT + 再開
- ブラウザは、ISO を `chunk_size` ごとに切り（`Blob.slice`）、**`PUT /api/v1/iso/<id>` を順に送る**（1 つの要求 = 1 つのチャンク）。要求のたびに `X-Upload-Offset`（その要求の先頭のバイト位置）を付ける。**サーバーは、`received` と一致するときだけ受け付ける**（不一致は 409 `offset_mismatch` と、現在の `received`）。再開は、`GET /api/v1/iso` の `received` から続ける。
- **`chunk_size` は、`begin` の応答で、サーバーが決める**（既定 128 KiB。`ISO_UPLOAD_CHUNK_KB`、16〜1024）。**`esp_http_server` は 1 つのタスクで動く**ので、1 つの要求の処理中は、同じサーバーの `/ws`（入力）と API が止まる。**チャンクは、全体をバッファに受信してから書く**（§3.1。バッファの確保元と、DMA の可否は A-15 が決める）。チャンクを小さくして、1 つの要求を短く（128 KiB で数十 ms の見込み）する。映像は専用のサーバー（`video_port`）なので、影響を受けない。**アップロード中の入力の遅延は、平均だけでなく最悪値も、実機で測る**（`HW_VERIFY` I10）。許容できなければ、アップロード専用のサーバー（別のポート）に移す（API はそのままで、ホストだけが変わる）。
- **`PUT` は、本体を読む前に、ヘッダーだけで判定する**（認証、Origin、`id`、状態、`X-Upload-Offset`、`Content-Length`。純 C の `iso_api_put_precheck`。`INTERFACES.md` 7.4）。**拒否するときは、本体を読まずに応答し、`Connection: close` を付ける**（残りの本体を、単一スレッドの httpd が読み捨てて止まるのを避ける）。
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
- **パスは、ユーザーの入力から作らない**。ディレクトリ名は `<id>`（サーバーが作る 8 桁の 16 進。乱数は `iso_random_t` で注入）。ユーザーの名前は、メタデータの中にだけ置く（表示用）。**名前の規則**: UTF-8 として正しい（不正なバイト列、過長な表現、サロゲートを拒否）、1〜63 バイト、**C0 制御文字（U+0000〜U+001F）、DEL（U+007F）、C1 制御文字（U+0080〜U+009F）を含まない**、`/` と `\` を含まない、前後に空白（U+0020）がない。**同名の判定は、バイト列の完全一致**（大文字小文字を区別する。Unicode の正規化はしない）で、409 `name_exists`。**ビューアは、名前を `textContent` で表示する**（`innerHTML` を使わない）。
- **上限**: イメージ 16 個（`ISO_MAX_IMAGES`）、1 つのイメージは 16 GiB まで（`ISO_MAX_PARTS` × 1 GiB）、**`size` は ISO の署名が置かれるセクタ 17 までを含む 34,816 バイト以上**、アップロードは同時に 1 つ、1 つのチャンクは `chunk_size` まで（超過は 413）、JSON のボディは 512 バイトまで（既存の API と同じ）。**カードの全体の大きさ（`total_bytes`）を超える `size` も、空き不足と同じ 507**。
- **イメージは読み取り専用**でターゲットに見せる（書き込み系は DATA PROTECT）。
- **SD の抜き取り・故障**: I/O のエラーを、MSC は MEDIUM ERROR で、API は 503 `sd_unavailable`（処理中の I/O エラー）または `not_ready`（処理の前に、`sd.state` が `ready` でない）で返す。SD が戻ったら、再初期化する（カード検出の GPIO がないので、定期的な再確認）。**書き込み中のエラーは、チャンクを `truncate` で取り消し、アップロードを `incomplete` にして、再開できるようにする**。**再初期化では、マウントを強制的に外す**（§5.5）。
- **`begin` と `DELETE` を繰り返すと、SD の書き込みが増える**（認証済みの利用者だけ。許容する）。
- **ログ**: 操作（`begin`、`finish`、`mount`、`eject`、`delete`）の結果と理由を、イメージの `id` と大きさだけ記録する。名前は、記録しない（個人の情報の可能性）。チャンクごとのログは出さない（数万回になる）。進捗は 5% ごと。

## 8. 担当と順序（Issue）

| ID | 内容 | 担当 | 備考 |
|---|---|---|---|
| **C-9** | この設計と契約（`ISO_MOUNT_DESIGN.md`、`INTERFACES.md` §7、`WEB_API.md` §8、`PLAN.md`、`HW_VERIFY.md` I 章） | Claude | **この PR（#135）** |
| **C-10**（#139） | 純 C: `iso_extent`、`iso_catalog`（メタデータ 256 バイトの符号化、2 スロット、名前の規則、起動時のスキャン、import）、`iso_upload`（受信の状態機械、再開、SHA-256 の注入、タイムアウト、`iso_upload_hash_step`）。ホストテスト、ファズ、変異テスト | Claude | `iso_extent` は、すぐ始めてよい |
| **C-11**（#140） | 純 C: `mmc_scsi`（§5.4。**全コマンド**）と `iso_media`（§5.5）。MMC / SPC の文書と照合。ホストテスト、ファズ、変異テスト | Claude | A-14 と並行 |
| **C-12**（#141） | 純 C: `iso_api`（要求の検査、`iso_api_put_precheck`、判定の順序、状態遷移）と `web_server` の結線の一部、ビューアの画面（一覧、アップロードの進捗と再開、マウント、取り外し、削除） | Claude | C-10、C-11 の後 |
| **A-13**（#137） | SD: SDMMC（スロット 0、GPIO39〜44、`ESP_LDO_VO4`、電源スイッチ GPIO45）、FAT32 のマウントと検査（§3）、**自前の diskio（`sd_io_lock`）**、`iso_blockdev_t` と `iso_fs_t` の実装（`truncate`、`get_extents`、`CONFIG_FATFS_USE_FASTSEEK=y`）、`iso_worker`（§4.2）。電圧・速度の実測 | Antigravity | |
| **A-14**（#136） | USB MSC: **最初に、§5.1 の調査（手段 1 か 2）**。MSC の記述子（インターフェース 3）、`mmc_scsi` への結線（全コマンド）、`usb_hid_logic` のテストの更新。BIOS / UEFI での確認 | Antigravity | 調査の結果を、この文書 §5.1 に反映する |
| **A-15**（#138） | HTTP: アップロードのハンドラー（受信 → バッファ → SD、mbedTLS の SHA-256、拒否時の `Connection: close`）、`/api/v1/iso*` の結線（**ワイルドカードの URI 照合、`max_uri_handlers`、`http_guard` に `PUT` / `DELETE` と `application/octet-stream`、ハンドラーのスタックの実測**）。入力の遅延と転送速度の実測 | Antigravity | C-12 と協調 |

**Kconfig の接頭辞**: `ISO_*` = Claude（API とロジック）、`SD_*` と `USB_MSC_*` = Antigravity。**`ISO_ENABLE` は、`SD_ENABLE` と `USB_MSC_ENABLE` に依存する**（どちらかが無効なら、ISO の機能は無効で、パスは登録しない）。`ISO_*` の項目: `ISO_ENABLE`、`ISO_MAX_IMAGES`（16）、`ISO_MAX_PARTS`（16）、`ISO_MAX_EXTENTS`（4096）、`ISO_FREE_RESERVE_MIB`（64）、`ISO_UPLOAD_CHUNK_KB`（128）、`ISO_UPLOAD_IDLE_S`（300）、`ISO_SYNC_KIB`（4096）、`ISO_AUTOMOUNT_LAST`（n）。SD の GPIO と LDO のチャンネルは、**回路図の固定の値**（Kconfig にしない。§1）。

## 9. 状態の正と、各部品のテスト（純 C）

| 部品 | 主な不変条件（テストで固定する） |
|---|---|
| `iso_extent` | LBA → (パーツ、オフセット) → SD セクタの変換が、パーツの境界・エクステントの境界・最終ブロック・パディングで正しい。範囲外は拒否。区間の併合が、順序と数を保つ |
| `iso_catalog` | `meta`（256 バイト）の符号化と復号が往復する（名前 63 バイト、CRC が名前と重ならない）。**2 つのスロットの選択**（世代番号、巻き戻り、一方が壊れている、両方が壊れている）。壊れた `meta` の拒否。名前の規則（UTF-8、長さ、禁止文字）。同名の拒否。上限。起動時のスキャン（`uploading` → `incomplete`、孤立ディレクトリ、不正な名前）。import（名前の衝突、上限、拡張子の大小） |
| `iso_upload` | `received` と一致しないオフセットの拒否。チャンクの上限。チャンクがパーツの境界をまたぐときの分割。書き込み失敗の `truncate` と `received`。SHA-256 の状態が失われたときの再計算。同時 1 つ（`finalizing` を含む）。アイドルのタイムアウト。`finish` の判定（サイズ、SHA-256、ISO の署名 `CD001` / `BEA01`、`too_fragmented`）。時刻の巻き戻り |
| `iso_media` | メディアの変更が、1 回だけ UNIT ATTENTION になる。`prevent_removal` と取り外し・差し替えの規則。バスリセットで `prevent_removal` が解除される。SD の再初期化での強制取り外し。ターゲットの取り外しが、Web の状態に残る（次の `mount` / `eject` まで） |
| `mmc_scsi` | 各コマンドの応答のバイト列（手書きの期待値）。CDB の長さ・予約ビットの検査。メディアなし / ありの両方。範囲外、書き込み、未対応のコマンドのセンス。論理サイズ以降のゼロ埋め |
| `iso_api` | 判定の順序（`WEB_API.md` §8）。状態ごとのエラーの対応表。`iso_api_put_precheck`（本体を読む前の判定） |

## 10. 未確認一覧（`TODO(hw-verify)` / 調査。`HW_VERIFY.md` I 章と同じ番号）

| I 番号 | 内容 | 確認する人 |
|---|---|---|
| I2 | `ESP_LDO_VO4` の電圧と、IDF での有効化。電源 OFF（GPIO45 = High）のときの逆給電の有無 | A-13、オーナー（測定） |
| I3 | microSD の 4 ビットのモードの速度（40 MHz）、FAT32 の読み書きの実効速度 | A-13 |
| I4 | **`CLMT` によるエクステントの取得**（`CONFIG_FATFS_USE_FASTSEEK=y`、読み取りモード）が動くこと。断片化したときの挙動（`FR_NOT_ENOUGH_CORE`、`too_fragmented`） | A-13 |
| I5 | SD の直列化（自前の diskio）で、アップロードと MSC の読み出しが同時に動くこと | A-13、A-15 |
| I6 | **MSC の実装の手段**（アプリのクラスドライバ、または TinyUSB のパッチ）。esp_tinyusb 2.4.0 が持つ TinyUSB の版 | A-14（最初の調査） |
| I7 | USB OTG HS の FIFO の割り当て（HID ×3 の IN + MSC の IN/OUT。合計 4096 バイト）が足りること | A-14 |
| I8 | HID + 仮想 CD-ROM の複合デバイスを、BIOS / UEFI が認識すること。Linux の ISO と Windows の ISO の起動 | A-14、オーナー |
| I9 | READ TOC / GET CONFIGURATION のプロファイル（DVD-ROM）が、Windows、Linux、UEFI で問題ないこと | A-14 |
| I10 | アップロード中の入力（`/ws`）の遅延（**平均と最悪値**）と、アップロードの実効速度。`DELETE`、`finalizing` の最中の遅延 | A-15 |
| I11 | 電源断・SD の抜き取りのとき、`incomplete` として復帰し、再開できること。壊れた `meta` の扱い（2 スロット） | A-13、A-15 |
| I12 | `locked_by_target`、ターゲットの START STOP UNIT による取り外し | A-14 |

## 11. 検討した他の案（記録。変更してよい）

### 保存方式（オーナー決定: 案 1）
| 順 | 案 | 内容と理由 |
|---|---|---|
| 1 | **FAT32 + 分割 + 直接セクタ読み出し（採用）** | IDF を改造しない。32GB のカードは FAT32 が標準。変換は純 C。`f_expand` は使わず、追記 + CLMT のエクステント（§3.1） |
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
