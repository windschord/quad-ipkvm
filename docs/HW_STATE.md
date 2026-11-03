# 実機の現状 (HW_STATE)

作成: 2026-11-01（実機担当 Claude）。`docs/ANTIGRAVITY_HANDOFF.md` に基づく、HDMI 担当（Antigravity）への引き継ぎ。
**この文書は、実機担当が 2026-10-21〜2026-11-01 に行った確認の記録の要約**で、詳細と根拠は `docs/HW_VERIFY.md`（特に E 章と E.1）にある。食い違いがあれば、`HW_VERIFY.md` の測定の記録を正とし、分からないことは実機担当（オーナー経由）に確認する。

## 書き方の約束
- **[確認]** = 実測または実機のログで確認したもの。測定値は「値 + 条件（どこで・何と何の間・機材・日付）」の形。
- **[推測]** = 状況証拠からの推測。**[未確認]** = 分かっていない。**電気的な値を推測で確定していない。**
- 機材: テスター = オーナーのデジタルマルチメータ（表示は 2〜3 桁。細いプローブあり）。
- **秘密は書かない。** パスワード（オーナーが保管）、証明書の秘密鍵・パスワードのハッシュ（ボードの NVS 内）は、この文書にない。

---

## 1. ハードウェア構成

### 1.1 部品

| 項目 | 内容 |
|---|---|
| ボード | Waveshare ESP32-P4-ETH、**チップ rev v1.3**（`ESP32-P4NRW32`。PSRAM 32MB〔HEX、200MHz〕、フラッシュ 32MB）。Ethernet は IP101GRI（RMII）。MAC `80:f1:b2:d2:e2:c4`。IP は DHCP で **192.168.11.39**（変わりうる）。回路図: `docs/ESP32-P4-ETH-schematic.pdf` |
| HDMI キャプチャ | **Geekworm X630 V1.5**（TC358743XBG。DIP スイッチなし。V1.5 はソフトリセット切替を廃止）。I2C アドレス 0x0F。水晶の表記 "27.000"（REFCLK への接続は **[未確認]**） |
| HDMI の経路 | **PC（オーナーのもの）→ HDMI → X630 → MIPI CSI（2 レーンとみられる [推測]）→ 15 ピン FFC（X630 付属、15 ピン 1.0mm ↔ 22 ピン 0.5mm）→ ESP32-P4-ETH の J1（22 ピン CSI コネクタ）→ ESP32-P4 の CSI**。いまは **PC 直結（KVM なし）**。映像は、まだ 1 フレームも取れていない |
| 前段の KVM | 4ch HDMI KVM は未接続（このフェーズでは使わない） |

### 1.2 配線表

**接続済み: X630 の 15 ピン ↔ ケーブル ↔ J1（22 ピン）**。**X630 の 15 ピンの番号は、C779 型の逆順と推測している（X630 固有のピン表は未確認。GND が 6・9・12・15 番であることだけが実測）。** 以下の表で、**[確認]** はケーブルの導通と X630 の GND の位置の実測、**[推測]** は X630 側の信号の割り当て。J1 の番号は回路図（左端が 1 番。USB-C コネクタの側から見て）。

| X630 15 ピン | 信号 | J1 | 行き先（回路図） | 確認の状況 |
|---|---|---|---|---|
| 1 | 3V3 | 1 | `ESP_3V3` | **[確認]** ケーブル単体の導通（1 → 1）。X630 の 1 番が 3V3 であることは、GND の実測（下）、C779 の画像、写真との整合による **[推測]**（X630 自体の資料はない） |
| 2 | SDA | 2 | `ESP_I2C_SDA`（GPIO7） | **[確認]** 導通（2 → 2）と、I2C の応答（0x0F） |
| 3 | SCL | 3 | `ESP_I2C_SCL`（GPIO8） | **[確認]** 導通（3 → 3）と、I2C の応答 |
| 4 | 基準クロックの位置（C779 の画像で REFCLK）| 5 | `CSI_IO1`（ESP のどの GPIO にも未接続。R43 未実装） | **[推測]**（写真と標準配線による） |
| 5 | 不明（C779 の画像は「記載なし」） | 6 | `CSI_IO0`（GPIO 未接続。**R42 10kΩ で `ESP_3V3` にプルアップ**） | **[推測]**。X630 の 5 番の機能は **[未確認]** |
| 6 / 9 / 12 / 15 | **GND** | 13 / 16 / 19 / 22 | GND | X630 の GND の位置は **[確認]**（15 番と導通するのは 6・9・12 番だけ。下の測定）。**15 → 22 は [確認]（導通）。6・9・12 → 13・16・19 は [推測]（導通を測っていない）** |
| 7 / 8 | CK_P / CK_N | 14 / 15 | `CSI_CLKP` / `CSI_CLKN` | **[推測]** |
| 10 / 11 | D1_P / D1_N | 17 / 18 | `CSI_DATAP1` / `CSI_DATAN1` | **[推測]** |
| 13 / 14 | D0_P / D0_N | 20 / 21 | `CSI_DATAP0` / `CSI_DATAN0` | **[推測]** |

- 根拠（写真）: J1 に挿したケーブルは、左から 1・2・3・5・6 番と、13〜22 番の 10 本に配線が届き、4 番と 7〜12 番は配線なし。標準の Raspberry Pi 用 15 → 22 ピンのケーブルの配線と一致する。
- 安全上の確認（ケーブル）: ケーブル単体で、1 番から他の 14 本、隣り合う接点どうしの導通なし（2026-10-31）。ケーブルを X630 と J1 に挿した状態で、40 ピンヘッダの 3V3 と GND の間は短絡なし（数 kΩ 以上、緩やかに上昇。**数値は記録なし**）。
- **ケーブルの向きの危険**: 22 ピン側の接点面を間違えると、X630 の GND と ESP の 3V3 が入れ替わりうる。通電前に導通と短絡を確認する（`HW_VERIFY` E.1）。

**X630 の 10 ピンコネクタ（SH1.0）**: **ESP 側は、すべて未配線**（HDMI 担当の作業で配線する）。

| 10 ピン | 信号 | 扱い |
|---|---|---|
| 1 | 3V3 **出力** | **接続禁止**（ESP の 3V3 に繋がない） |
| 2 | **HDET** | **接続禁止**（ESP の GPIO に繋がない。HDMI 接続で 4.8V が出る） |
| 3 | IR | 使わない |
| 4 | **INT**（High のとき割り込み） | **High の電圧を測るまで未接続**。ESP 側の予定は GPIO17（入力）。High の電圧は **[未測定]** |
| 5 | **RESET**（active low） | ESP 側の予定は **GPIO16（オープンドレイン出力。出力を High にドライブしない）**。**まだ未配線**（配線は準備中。順序は `HW_VERIFY` E.1 の 5 の依頼元の回答） |
| 6〜9 | 音声（A_SCK など） | 使わない |
| 10 | GND | RESET と一緒に配線する予定（ハーネスの GND の導通は、配線後に確認する） |

- GPIO16 / 17 のヘッダ上の位置: 回路図（`PICO-2*20` のヘッダ）の **5 番が GPIO17、6 番が GPIO16**。基板上の物理位置との照合は **[未実施]**（1 番ピンの印から標準の数え方で特定し、シルクの GPIO 名があれば照合する）。GPIO16 / 17 の I/O 電源ドメインは `VDD_IO_0`（= 回路図の `VDDPST_2`）で、回路図上は `ESP_3V3` に接続されている [確認: 回路図。ピンの対応はデータシート v0.7 の Table 2-1]。**`ESP_3V3` の実測値、データシート v0.7 の rev v1.3 への適用（入力・出力の定格、駆動能力）、ROM・ブートローダ段階の起動時の状態は [未確認]**（データシート v0.7 の値は、「VIH 最小 0.75 × VDD」などで、preliminary）。
- **現在の物理状態（最後に確認できたもの）**: ボードの USB は**抜かれている**（通電していない）。HDMI も抜く指示を出した。**CSI ケーブルを J1 から外す依頼（`VDD_MIPI_DPHY` の測定の準備）を出した直後で、実施したかは [未確認]**。再開するときは、オーナーに現状を確認する。

## 2. 電源
- **給電: PC → 電流制限つきの給電ハブ・電源 → ESP32-P4-ETH の Type-C（`H2`、UART 用）**。データ通信あり（PC からシリアルが見える）。**電流制限は「500mA 前後（USB 2.0 相当）」とオーナーが回答**。**型番、制限の実際の値、過電流時の動作は [未記録]**。
- **電流計はない**（USB の電流計・電流を測れる電源は、手元にない）。電流の値と推移は見えない。基板全体の消費電流は **[未測定]**。**CSI の受信（P2b-4）を始める前に、電流計を用意する**（依頼元の指示）。
- 給電経路（回路図）: Type-C の VBUS（`USB0_5V`）→ TVS1（`LTVS16H5.0ET5G`）→ Q2（`AO3401`）・U2（`MMDT3906DW`）→ D2（`MBR230LSFT1G`）→ `VCC_5V` → U1（`MP1658GTF-Z`）→ **`ESP_3V3`**（回路図の帰還抵抗から **約 3.27V と計算。実測なし**。「3A MAX」・L1 2.8A）。**ヒューズ・PTC はこの経路に見当たらない**。入力の電流制限は、給電側（PC・ハブ）の保護だけ。
- X630 の電源: ケーブルの 3V3（J1 の 1 番 = `ESP_3V3`）から給電される。X630 の消費電流は **[未測定]**（TC358743 単体のデータシート代表値は 480〜543mW。**[推測]** 3.3V で約 150〜165mA）。
- 通電の止める基準（依頼元）: ハブのトリップ、触れないほど熱い、異臭、PWR LED の消灯・ちらつき、PC 側での USB 切断、X630 の 3V3 が 3.0V を下回る。電流が 400〜450mA を超える（電流計がないので監視できない）。

## 3. 測定ログ
すべて、オーナーがテスターで測定（2026-10-24〜2026-10-31）。HDMI は、記載がない限り未接続。

### 3.1 電圧（通電中）

| 値 | どこで・何と何の間 | 条件・日付 |
|---|---|---|
| **3.2〜3.4V** | X630 の 10 ピンの 1 番（3V3 出力）と 10 番（GND）の間 | 初回の通電（HDMI なし、ケーブル接続済み）、2026-10-31 |
| **3.3V** | 同上 | HDMI 接続の前と後（PC 直結）、2026-10-31（小数 1 桁） |
| **RESET: 通常 3.3V** | 10 ピンの 5 番 – GND（10 番） | ESP 側未配線、2026-10-24（B6）。10kΩ を接続すると 1.1V（X630 側に約 20kΩ のプルアップとみられる [推測]） |
| **INT: 0V** | 10 ピンの 4 番 – GND（10 番） | 待機（HDMI なし）2026-10-24 と 2026-10-31。**HDMI 接続後も 0V**（2026-10-31） |
| **HDET: 0.8V** | 10 ピンの 2 番 – GND（10 番） | HDMI 未接続、2026-10-30 |
| **HDET: 4.8V** | 同上 | HDMI を PC に接続後、2026-10-30。**HDMI の +5V を検出する信号とみられる [推測]**。Wiki は「3.3V」と書く（食い違い） |
| `ESP_3V3` ≈ 3.27V | （計算） | 回路図の帰還抵抗（R18 40.2kΩ、R24 13kΩ、FB 0.8V の注記）。**実測なし** |

### 3.2 抵抗（電源なし）

| 値 | どこで・何と何の間 | 条件・日付 |
|---|---|---|
| **540kΩ** | X630 の 10 ピンの INT（4 番）– 3V3（1 番） | X630 に ESP 未接続、HDMI なし、2026-10-31 |
| **158kΩ** | INT（4 番）– GND（10 番） | 同上 |
| **37kΩ** | INT（4 番）– HDET（2 番） | 同上（原因は **[未確認]**） |
| 約 30kΩ | X630 の 15 ピンの 1 番（3V3）– 15 番（GND） | X630 単体、2026-10-31 |
| 0Ω | 15 ピンの 12 番 – 15 番（GND）。**GND は 6・9・12・15 番** | X630 単体、2026-10-31（6・9・12 だけが 15 番と導通。1・4・7・10・13・14 は導通なし） |
| 11 番: GND に 1.4MΩ、3V3（1 番）に 30kΩ | 15 ピンの 11 番 | X630 単体、2026-10-31（30kΩ は電源系統の抵抗が見えているだけとみられる [推測]） |

- INT: 3V3 へのプルアップ（数 kΩ〜数十 kΩ）は見えない。**INT はチップが駆動する出力（プッシュプル）とみられる [推測]**。High の電圧は、TC358743 の I/O 電源（1.8V か 3.3V か **[未確認]**）で決まる。RESET が 3.3V、I2C が 3.3V のバスで動くことは、3.3V 方向の状況証拠。
- `VDD_MIPI_DPHY`: **[未測定]**（下の 6 章）。**ESP_3V3 や X630 の消費電流も未測定**。
- 温度・異臭: 通電中に指で触れて、レギュレータと TC358743 に異常なし（2026-10-31。温度の数値はない）。

## 4. 今ボードに書き込まれているファームウェア

| 項目 | 内容 |
|---|---|
| バージョン表示 | **`e6891e3-dirty`**（コンパイル 2026-10-31 09:46:39、ELF SHA256 `988f871d5…`） |
| ソース | 統合ブランチ `claude/brave-cori-5m9w1m` の **`681bb46`（PR #60 のマージ）のコード** + **一時パッチ（コミットしない）**: `main.c` の `TC358743_INIT` ブロックに、HDMI の確認用の 180 秒ループ（1 秒ごとに `INTSTATUS` を読んで出力、5 秒ごとに診断を出力）。パッチ: `~/quad-ipkvm-hw-artifacts/firmware/temp_poll_loop.patch`。`e6891e3` は docs だけの差 |
| sdkconfig | リポジトリ直下の未追跡の `sdkconfig`（コピー: `~/quad-ipkvm-hw-artifacts/firmware/sdkconfig_repo_local`）: **`CONFIG_NET_ETH_INIT=y`、`CONFIG_TC358743_INIT=y`、`CONFIG_MIPI_PHY_POWER_ENABLE` 無効**、`FRAME_HUB_POOL_SIZE=3`・`MAX_FRAME_KB=256`・`PREFER_SPIRAM=y`、`TEST_PATTERN_FPS=10`、フラッシュ 32MB、パーティションは single app（`nvs` 0x9000、`factory` 0x10000 = 1MB。ブートログで app を offset 0x10000 から読む） |
| 書き込み範囲 | **アプリ（0x10000）だけ**を書き込んでいる。ブートローダ（0x2000）とパーティションテーブル（0x8000）は、今回の作業では書き込んでいない（以前の書き込みのまま。**フラッシュ上の内容と、`firmware/` のコピーが同一かのハッシュ確認はしていない**。ビルドのパーティションテーブルのデコード結果: `nvs` 0x9000 24K、`phy_init` 0xf000 4K、`factory` 0x10000 1M）。**NVS（証明書、パスワードのハッシュ）は、そのまま残っている** |
| 動作 | 起動 → Ethernet（192.168.11.39）→ テストパターン → HTTPS サーバ（443）→ TC358743 の初期化（I2C、EDID 書き込み、PLL・CSI は省略）→ 診断 → 180 秒ループ |
| **MIPI PHY の電源** | **無効**（`VDD_MIPI_DPHY` は未測定のため） |

**バックアップ**
- **フラッシュ全体（32MB）のバックアップは、まだ取っていない。** Antigravity が、**最初に**取る（`~/quad-ipkvm-hw-artifacts/scripts/backup_flash.sh`。手順は 5 章。**NVS の秘密を含むため、リポジトリや共有の場所に置かない**）。
- アプリだけの退避（`~/quad-ipkvm-hw-artifacts/firmware/`）: `quad-ipkvm_af3bf37_backup.bin`（af3bf37 時点のアプリ。**書き込み済みだったものと同一かのハッシュ確認はしていない**）、`quad-ipkvm_e6891e3-dirty_TEMPloop_MIPIoff.bin`（今書き込まれているもの）、`quad-ipkvm_MIPIon_TC358743init_notflashed.bin`（`MIPI_PHY_POWER_ENABLE=y` のビルド。**書き込んでいない**）。参考: `bootloader_from_build_tc.bin`、`partition-table_from_build_tc.bin`、`flasher_args_build_tc.json`。
- **ESP-KVM を参照機として動かすときは、元のファームを書き戻す**（`ANTIGRAVITY_HANDOFF.md` 7 章）。そのために、フラッシュ全体のバックアップが要る。

## 5. ビルド環境と操作

| 項目 | 内容 |
|---|---|
| ESP-IDF | **v6.0.1**: `~/.espressif/v6.0.1/esp-idf`。ツール: `~/.espressif/idf601-tools`（`IDF_TOOLS_PATH`）。CI も v6.0.1。master や 6.1 のソースを見て書かない |
| 環境の読み込み | `source ~/quad-ipkvm-hw-artifacts/scripts/idf_env.sh`（中身: `export IDF_TOOLS_PATH=$HOME/.espressif/idf601-tools` → `source $HOME/.espressif/v6.0.1/esp-idf/export.sh`） |
| ビルド | `~/quad-ipkvm-hw-artifacts/scripts/build.sh [ビルドディレクトリ [sdkconfig]]`（中身: `idf.py -B <dir> [-D SDKCONFIG=<path>] build`）。**リポジトリ内の `build/` は使わない**（前のセッションの Python 環境〔`.../scratchpad/esp/tools/...py3.13_env`〕で作られており、`idf.py` が「currently active ... while the project was configured with ...」で失敗する。`idf.py fullclean` で作り直すか、`-B` で別のディレクトリを使う） |
| 書き込み（アプリのみ） | `~/quad-ipkvm-hw-artifacts/scripts/flash_app.sh <app.bin>`（`esptool --chip esp32p4 -p /dev/cu.usbmodem5B5F0916061 -b 460800 write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m 0x10000 <bin>`） |
| **フラッシュ全体のバックアップ** | `~/quad-ipkvm-hw-artifacts/scripts/backup_flash.sh`（`esptool --chip esp32p4 -p <port> -b 921600 read-flash 0 0x2000000 <out>`。戻すときは `write-flash 0 <out>`） |
| シリアル | **ポート `/dev/cu.usbmodem5B5F0916061`、115200bps**。コンソールは UART（GPIO37/38）で、Type-C の `H2` 経由 |
| ログの取得 | `<venv>/bin/python ~/quad-ipkvm-hw-artifacts/scripts/capture.py <出力ファイル> [秒数]`。**実行するとボードがリセットされる**（RTS）。出力に NUL が混じることがある（`grep -a`）。対話モニタは `idf.py -p <port> monitor`（TTY が要る） |
| Python 環境 | スクラッチの venv は永続化していない。`python3 -m venv ~/quad-ipkvm-hw-artifacts/venv && ~/quad-ipkvm-hw-artifacts/venv/bin/pip install pyserial pymupdf requests websockets`（使った版: pyserial 3.5、pymupdf 1.28.2、requests 2.34.2、websockets 17.3） |
| ホストテスト | `cmake -S test/host -B b && cmake --build b -j8 && ctest --test-dir b --timeout 30` |

- 実機で HTTPS に到達するには `CONFIG_NET_ETH_INIT=y` が必要（既定は n）。`sdkconfig` は未追跡（コミットしない）。
- **`main.c` では、MIPI PHY の電源の有効化は `if (CONFIG_TC358743_INIT)` の中にある**（`f1028e9` 時点）。`MIPI_PHY_POWER_ENABLE=y` だけでは、電源は有効にならない。`TC358743_INIT=y` も要る。X630 を繋がない測定でも、`TC358743_INIT=y` では、I2C の初期化に失敗するログ（`Failed to initialize TC358743`）が出るが、LDO の有効化は、その前に行われる。
- Kconfig（`f1028e9` 時点）: `TC358743_START_CSI`（既定 n）、`TC358743_RESET_GPIO`（既定 16）、`TC358743_CSI_CONTINUOUS_CLOCK`（既定 n）、`TC358743_INIT`（既定 n）、`MIPI_PHY_POWER_ENABLE`（既定 n）、`MIPI_PHY_LDO_CHAN`（3）、`MIPI_PHY_LDO_MV`（2500）。
- エージェントのサンドボックスによっては、シリアルポートや `~/.espressif` への書き込みに、権限の許可が要る。

## 6. 実機で最後に確認した結果（2026-10-31）

### 6.1 TC358743 の I2C と初期化（ローカルのビルド、HDMI なし）
ログ（`~/quad-ipkvm-hw-artifacts/logs/powerup2.log`）:
- `tc358743: Found TC358743, Chip ID: 0x0000 (Revision 0x00)`（**I2C の 0x0F が応答**。Linux のドライバでは `CHIPID` の上位バイトが 0 であることが TC358743 の判定条件で、`0x0000` は矛盾しない）
- `Lowering HPD...` → `Generating and writing 256-byte EDID to RAM...` → **`EDID programmed successfully`**（EDID は 1080p30 を含む骨格のもの。最初のモードを 720p60 だけにする変更は、P2b-3）→ `Bringing up HPD...`
- `SYSCTL: 0x0c00 (Sleep: 0, HDMIRST: 0, CTXRST: 0)`、`HDMI SYS_STATUS: 0x00 (TMDS detected: no, SYNC: no)`
- 警告（ドライバが省略したもの）: HDMI PHY・Audio・Infoframe の設定、PLL の設定（`UNVERIFIED PLL parameters`）、CSI の開始（`UNVERIFIED CSI timings`）

### 6.2 HDMI を接続した通電（PC 直結、約 170 秒、ログ `logs/hdmi1.log`）
- **`SYS_STATUS`（0x8520）: 起動後 約 50 秒まで 0x00、HDMI を接続した後は 0x01**（ドライバの表示は「TMDS detected: no, SYNC: no」のまま）。Linux のドライバの `MASK_S_DDC5V`（HDMI の +5V 検出）に当たる **[推測。レジスタ定義のヘッダが未入手]**。
- **`INTSTATUS`（0x0014）: 164 回の読み取りのすべてで 0x0000**。INT（10 ピンの 4 番）は 0V のまま。`INTMASK` は変更していない（初期値は **[未確認]**）。
- **PC は、ディスプレイを認識しなかった。** 依頼元の判断: 今のビルドでは**想定内**（`DDC_CTL` の `DDC5V_MODE` が 0 のまま〔公開実装は 2〕、`EDID_MODE` の設定〔公開実装は E-DDC の 0x02〕がなく、HDMI RX PHY・基準クロック依存の値を省略しているため）。HPD の配線の問題かの切り分けは、**P2b-3 のビルドで行う**（`HW_VERIFY` E.1 の 7）。
- 3V3 は 3.3V のまま、温度・異臭に異常なし。ハブの電流制限のトリップ・LED のちらつきの有無は、回答に含まれなかった。

### 6.3 再現手順
1. ボードの USB（ハブ経由）を繋ぎ、`ls /dev/cu.usbmodem*` でポートを確認する。
1a. **書き込みの前に、フラッシュ全体のバックアップを取る**: `~/quad-ipkvm-hw-artifacts/scripts/backup_flash.sh`。**出力が 32MB（0x2000000 バイト）で、SHA-256 が表示されたことを確認してから、先に進む**（取れなければ、書き込まない）。
2. `source ~/quad-ipkvm-hw-artifacts/scripts/idf_env.sh`。リポジトリの `sdkconfig` で `CONFIG_TC358743_INIT=y`、`CONFIG_NET_ETH_INIT=y` を確認する。
3. （HDMI 接続時の `INTSTATUS` と `SYS_STATUS` の観測〔手順 5〕を再現するときだけ）一時パッチを適用する: `git apply ~/quad-ipkvm-hw-artifacts/firmware/temp_poll_loop.patch`（`main/main.c` に 180 秒のループを足す。**コミットしない**。終わったら `git checkout main/main.c`）。パッチを使わず、保存済みのバイナリ `~/quad-ipkvm-hw-artifacts/firmware/quad-ipkvm_e6891e3-dirty_TEMPloop_MIPIoff.bin` を書き込んでもよい。
3a. `~/quad-ipkvm-hw-artifacts/scripts/build.sh ~/quad-ipkvm-hw-artifacts/build_a` → `flash_app.sh ~/quad-ipkvm-hw-artifacts/build_a/quad-ipkvm.bin`。
4. `capture.py <out> 35` → ログに `Found TC358743, Chip ID: 0x0000` と `EDID programmed successfully` が出る。
5. HDMI（PC 直結）を接続して、`SYS_STATUS` の変化（0x00 → 0x01）を診断のログで見る（手順 3 の一時パッチ、またはそのバイナリが要る）。**HDET（10 ピンの 2 番）には触れない。**

### 6.4 ほかに確認済みのこと（HDMI 以外）
ブラウザ（Chrome）でのログインと `/video` の wss でのテストパターンの表示、フレームレート約 9.9 fps（Python クライアント）、`/status` の遅いクライアントでの遅延（最大 5.1 秒。要判断）など。`HW_VERIFY` の D・F・Z を参照。

## 7. 使ったスクリプト・コマンド
置き場所: **`~/quad-ipkvm-hw-artifacts/`**（リポジトリの外。**コミットしない**）。

| 場所 | 内容 |
|---|---|
| `scripts/idf_env.sh`、`build.sh`、`flash_app.sh`、`backup_flash.sh`、`capture.py` | 5 章のとおり |
| `scripts/dstext.py <pdf> <正規表現>` | PDF の行を正規表現で検索（pymupdf。データシートの調査に使った） |
| `scripts/pdftext.py`、`pdfclip.py <out.png> x0 y0 x1 y1 倍率` | 回路図の PDF（`docs/ESP32-P4-ETH-schematic.pdf` の 1 ページ目）の語の位置の検索と、矩形の画像化 |
| `scripts/soak.py`、`slowclient.py`、`d10check.py`、`firstpost.py`、`bootpost.py`、`summ.py` | Web / WS の確認用（使い捨てのパスワードを自動生成する。パスワードは埋め込んでいない）。Claude 担当の範囲 |
| `firmware/` | 5・4 章のとおり（バイナリ、sdkconfig、パッチ） |
| `logs/` | `powerup1.log`（通電後の起動）、`powerup2.log`（TC358743 の初期化）、`hdmi1.log`（HDMI 接続の約 170 秒）、`soak_*`、`cap_q4.txt` |
| `reports/` | 依頼元向けの報告 8〜16（実機担当が書いたもの。時系列の詳細） |
| `refs/` | `p4_datasheet.pdf`（**ESP32-P4 Datasheet Pre-release v0.7**。rev v3.x 向けで、rev v1.3 への適用は **[未確認]**）、`ref/tc358743_ds.pdf`（TC358743 のデータシート）、`ref/tc358743.c`（Linux ドライバ。**GPL-2.0、参照のみ**）、`geekworm/`（X630 の Wiki の保存） |
| `schematic-crops/` | 回路図の抜粋の画像（J1、`VDD_MIPI_DPHY`、電源、GPIO16/17 など） |

## 8. `TODO(hw-verify)` と、オーナーの手元の機材

### 8.1 HDMI 周りの `TODO(hw-verify)`（詳細は `HW_VERIFY` E 章・E.1）
- **`VDD_MIPI_DPHY` の測定**（未測定。CSI ケーブルを外して。無効 → 有効の順。**2.75V 超でただちに無効に戻す**）。専用のテストポイントはなく、**C63（0.1µF）、C64（1µF）、C66（10nF）、C46（1µF）の端子**で測る（E14）。基板上の位置は資料にない。rev v1.3 の許容範囲は未確認（データシート v0.7 の 2.25〜2.75V は、合否の基準にしない）。
- **INT の High の電圧**（未測定。P2b-3 のビルドで測る）。X630 の TC358743 の I/O 電源（1.8V か 3.3V か）。INT – HDET の 37kΩ の原因。
- **RESET → GPIO16 の配線**（未実施）: 配線の直前に、X630 通電中の RESET の待機電圧を再測定（3.6V 以下）。配線後、電源を切った状態で、10 ピンの GND の導通と、GPIO16 への導通を確認。配線後の初回の通電で、GPIO16 の待機電圧が約 3.3V であること、I2C が応答することを確認。GPIO16 のヘッダ上の位置の特定。
- **PC がディスプレイを認識しない原因**（HPD の配線・EDID。P2b-3 のビルドで）。X630 の HPD の配線は未確認。
- **基準クロック**: 27MHz の REFCLK の接続（X630 の 4 番〔C779 では REFCLK〕↔ J1 の `CSI_IO1`。ESP は供給しない）。PLL の設定値。
- **X630 の最大モード**（Wiki は 1080p@50、商品ページは 1080p。2 レーンの上限）、**レーン数**（2 レーンとみられる [推測]）、15 ピン・10 ピンのピン表（X630 自体の資料なし）、HPD の配線。
- **15 ピンの中間の導通**（GND の 6・9・12 番、CSI の信号）: 測っていない。CSI の受信（P2b-4）の前に、通電後の動作で間接的に確認する。測れるなら測る（接点の位置を、拡大した写真などで特定する）。
- **電流**: 電流計の用意（P2b-4 の前）、ハブの型番・制限値・過電流時の動作の記録、基板全体と X630 の消費電流。
- **TC358743 の I2C の速度**（現在 100kHz。400kHz は `TODO(hw-verify)`）、`TC358743_EDID_MAX_PIXEL_CLOCK_KHZ`（81000）。
- データシート v0.7 の rev v1.3 への適用（GPIO16/17 の定格と起動時の状態など）。ROM・ブートローダ段階の GPIO16/17 の状態。
- そのほかの `TODO(hw-verify)`（`kvm_logic`、`auth_logic`、`net_eth` など）は、ソースを `grep -rn 'TODO(hw-verify)' components main` で確認する（Claude 担当の範囲）。

### 8.2 オーナーの手元の機材
- **テスター**（デジタルマルチメータ。細いプローブあり）。
- **PC**（HDMI 出力あり）、**HDMI ケーブル**、X630 付属の **15 ピン ↔ 22 ピンの FFC**、**10 ピンのケーブル**（SH1.0）。
- **電流制限つきの給電ハブ・電源**（500mA 前後、型番は未記録）。
- **ない・未確認のもの**: USB の電流計、オシロスコープ（E3 の機材の回答にない）、ロジックアナライザ。ジャンパ線などの手持ち（RESET の配線用）は **[未確認]**。
- 作業の分担: 物理的な作業（配線、挿抜、テスターでの測定、電源）は**オーナー**。Antigravity は手順を書き、結果を記録する。

## 9. ハマりどころ・未解決の症状
1. **X630 の 15 ピンは、C779 型の逆順と推測している**（GND が 6・9・12・15 番であることは実測。1 = 3V3 とする対応と、CSI 信号の割り当ては、X630 固有の資料がなく **[推測]**）。Raspberry Pi 標準（1 = GND、15 = 3V3）や、Arducam の表の番号は、X630 の実測（GND の位置）と合わないため、使わない。最初に、Raspberry Pi 標準の前提で作った対応表が誤りだった。
2. **X630 の `HDET`（10 ピンの 2 番）は 5V 系**（HDMI 接続で 4.8V）。ESP の GPIO に繋がない。HPD の判定にも使えない。
3. **X630 の 5 番は、J1 の `CSI_IO0` に繋がり、R42（10kΩ）で ESP の 3V3 にプルアップされると推測している**（J1 の `CSI_IO0` の R42 は回路図で確認。X630 の 5 番が J1 の 6 番に繋がることは、写真と標準配線による **[推測]**。X630 の 5 番の機能は未確認）。
4. **INT – HDET の 37kΩ**: 通電時には導通していないとみられる（HDET が 4.8V の状態で INT が 0V だったため [推測]）が、原因は未確認。
5. **PC がディスプレイを認識しない**: 想定内（DDC・EDID の設定の骨格の抜け）。P2b-3 のビルドで再確認する。
6. **`capture.py` はボードをリセットする**。実行中のログの取得や、HDMI を繋いだ状態の長時間の観測のときは、リセットのタイミングに注意する。出力に NUL が混じる（`grep -a`）。
7. **リポジトリ内の `build/` は壊れている**（別の Python 環境で作られた）。`-B` で別のディレクトリを使う。
8. **`main.c` の MIPI PHY の電源は、`TC358743_INIT` の中**にある。
9. **内蔵ブラウザは、自己署名証明書の HTTPS に到達できない**。オーナーが自分のブラウザで証明書を承認してログインすれば、Claude in Chrome で続きを確認できる。
10. **TC358743 の Chip ID `0x0000` は、正常**（Linux のドライバの判定条件）。
11. **アプリだけの書き込み（0x10000）では、NVS は消えない**（証明書、パスワードのハッシュ）。**フラッシュの消去（`erase-flash`、NVS の消去）、または NVS を含まないイメージでフラッシュ全体を書き込むと、NVS が失われ、初回のパスワード設定からやり直しになる**。`backup_flash.sh` で取った**完全バックアップ（NVS を含む）を `write-flash 0` で書き戻すと、NVS も元に戻る**。
12. **東芝の機能仕様書は入手できない**（流出物は取得・参照しない）。レジスタは、公開実装（p4kvm、ESP-KVM、Linux ドライバ）にあるものに限る。**Linux のドライバは GPL-2.0、参照のみ。コードを複製しない。**
13. 未解決: `/status` が、遅い WS クライアントで最大 5.1 秒止まる（`HW_VERIFY` Z。Claude 担当、Issue #69）。
