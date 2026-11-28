# `quad-ipkvm` KiCad 回路図プロジェクト

Waveshare **ESP32-P4-ETH (rev v1.3)** ベースの 4ch IP-KVM 全体回路図（KiCad 8 / 9 対応・**A2 単一統合シート構成**）です。一次仕様は [docs/WIRING.md](../../docs/WIRING.md) および [docs/HW_VERIFY.md](../../docs/HW_VERIFY.md) に準拠しています。

## 開き方

KiCad 8 または KiCad 9 で [`quad-ipkvm.kicad_pro`](quad-ipkvm.kicad_pro) を開いてください。
カスタムシンボルは同梱の [`quad_ipkvm.kicad_sym`](quad_ipkvm.kicad_sym)（[`sym-lib-table`](sym-lib-table) で `${KIPRJMOD}` 参照）および `.kicad_sch` 内の `(lib_symbols ...)` の双方に収録されているため、外部ライブラリ設定なしでそのまま開けます。

## ファイル構成・シート上のゾーン分け（A2 横 1 枚統合）

| ファイル | 役割 |
|---|---|
| [`quad-ipkvm.kicad_pro`](quad-ipkvm.kicad_pro) | KiCad 8/9 プロジェクトファイル |
| [`quad-ipkvm.kicad_sch`](quad-ipkvm.kicad_sch) | **全体統合回路図（A2 横・1 枚）** — 下記 5 ゾーンをすべて直接配線で接続 |
| [`quad_ipkvm.kicad_sym`](quad_ipkvm.kicad_sym) | プロジェクト用カスタムシンボルライブラリ (`ESP32-P4-ETH_P1`, `74HC139`, `Photorelay_SPST`, `Photorelay_SPST_M`, `74HC4052` 等) |
| [`sym-lib-table`](sym-lib-table) | プロジェクトシンボルライブラリ参照定義 (`${KIPRJMOD}/quad_ipkvm.kicad_sym`) |

[`quad-ipkvm.kicad_sch`](quad-ipkvm.kicad_sch) 上では、中央ハブの **ESP32-P4-ETH (`P1` 40pin / `J3` 4pin)** から以下 5 ゾーンへすべての信号線を直接トレースできます。

1. **左上 — Target PC USB OTG HS & VBUS 分圧 (`J2` → `J3` / `P1 #4`)**: `D-`/`D+`/`GND` の 3 本直結に加え、#178 オーナー決定の **`R1=120 kΩ` / `R2=150 kΩ` ±1% E24**（手元の部品。0.5% E96 の 130 kΩ / 162 kΩ は代替）の分圧回路（ESD 素子なし）から `P1 #4 (GPIO18)` へ直結。
2. **左中 — Geekworm X630 V1.5 (`J4` 15p ↔ `J1` 22p CSI + `J5` 10p → `P1 #5/#6`)**: FFC 変換配線および `10pin #4 INT` (`GPIO17[TODO]`) / `#5 RESET` (`GPIO16`) を直結、`#2 HDET (4.8V)` は接続禁止 (`NC`) 明記。
3. **左下 — 共有 I2C バス (`GPIO7/8`) + `SSD1306` OLED + `MCP23017` (`SSCI-016506`)**: `P1 #16/#17` から X630 (`0x0F`)・OLED (`J6`, `0x3C/0x3D`)・MCP23017 (`U3`, `0x20`, パネルボタン×5 + LED×4) へ連続バス配線。
4. **右上/右中 — ATX 電源・リセット出力 & KVM の直結 GPIO (Issue #180、オーナーの手書き回路図 2026-11-28)**: `KVM_A0/A1` (`100 kΩ PD`) と `~E_PWR/~E_RST` (`10 kΩ PU`) から `74HC139 ×1`（`U1`）を経て、フォトリレー `K1..K8`（「61A1」と書かれている。G3VM-61A1 と思われる。リレー入力は `+3V3 → 390 Ω → LED → デコーダ出力`）へ直接配線。**KVM のボタン ×4・LED ×4 は、9 ピンのハーネス `J9` から GPIO に直結**（ボタンは ESP が**オープンドレイン**で Low にして押す、LED は **Active Low**。絶縁部品なし）。
5. **右下 — PC のフロントパネルの PWR LED / HDD LED の読み出し (Issue #180)**: チャンネルごとに「PC の LED+ → 390 Ω → フォトリレー入力」、出力側は「3.3 V → 接点 → 4.7 kΩ で GND」。8 本（`K9..K16`）を **`74HC4052`**（`U2`。2 回路の 4 入力アナログ・マルチプレクサ。`VEE`・`~E` は GND）で選び、`A/B` は `74HC139` と共通（`KVM_A0/KVM_A1`）。出力 `PC_PWR_L` / `PC_HDD_L` を `P1` の GPIO 入力 2 本へ。

## 反映済みの安全制約・オーナー決定

- **`J3` ピン 1 (`VCC_5V`)**: ターゲット PC VBUS へ接続禁止 (`NC`) [H3]。
- **`X630` 10pin `#2 HDET`**: HDMI 接続時 4.8 V 出力のため ESP32-P4 GPIO へ接続禁止 (`NC`)。
- **VBUS 分圧・ESD 方針（PR #174、#178）**: 手元の `1% E24` の組（`R1=120 kΩ` / `R2=150 kΩ`）を使用（V2 の実測 `ESP_3V3` = 3.34 V で余裕あり）。`J3`/VBUS 検出とも ESD 素子は追加しない（`R1` が直列制限抵抗を兼ねる）。電源投入順序は **ESP32-P4 (`H2`) → ターゲット PC**。
- **ATX/KVM 制御と KVM LED 入力**: `MCP23017` (`SSCI-016506`) は `/RESET` が内部固定のため出力ラッチ保持リスクがあり、**必ず直接 GPIO** に配線する。
- **KVM のボタンと LED（Issue #180、オーナー申告 2026-11-28。`HW_VERIFY.md` A2〜A8）**: ボタンは 4 個独立で、KVM 側で 3.3 V にプルアップされ、押すと 0 V。**ESP はオープンドレインで駆動し、High を出さない**。LED は 3.3 V 共通で、KVM の IC がシンクして点灯する **Active Low**（点灯 = 0 V）。自動スキャンなし。GPIO に直結するため、**逆給電**（KVM 通電・ESP 無電源で `ESP_3V3` が上がらないか）と共通 GND を、KVM に繋ぐ前に確認する。

## 未確定・暫定事項 (`TODO(hw-verify)`)

以下の項目は実機測定・部品選定前のため暫定表記となっています。実配線・PCB 化前に [docs/HW_VERIFY.md](../../docs/HW_VERIFY.md) で確定し、対応する Kconfig / 定数を更新してください。

- `P1` の ATX/KVM/PC LED の GPIO 割当（暫定: `A0/A1` = `GPIO20/21`、`~E_PWR/~E_RST` = `GPIO22/23`、`KVM_BTN1` = `GPIO46`、`KVM_LED1..4` = `GPIO32/33/47/48`、`KVM_BTN2..4` = `GPIO19/15/14`、`PC_PWR_L` = `GPIO6`、`PC_HDD_L` = `GPIO54`。**空き GPIO は残らない**）
- フォトリレー（61A1）の型番・`If/Vf`、`74HC139` シンク電流余裕（リレー LED 電流は約 5 mA の概算。バッファ Tr 要否 `C1/C3`）、`74HC4052` の 3.3 V 単一電源での条件（ピン配置、`Ron`）、PC のフロントパネルヘッダの極性・電圧（`C4`）
- KVM の LED の消灯時の電圧とプルアップの要否（`A6`）、逆給電の確認
- パネル LED 制限抵抗 `R10..R13` (`G6`)、OLED モジュール I2C アドレス/PU (`G2/G3`)、`X630 #4 INT` High 電圧 (`B6`)

## 再生成と検証

本プロジェクトのファイルは以下のスクリプトで再生成・構造検証できます。

```bash
python3 tools/kicad/generate_schematic.py
python3 tools/kicad/verify_kicad.py hardware/kicad/quad-ipkvm.kicad_pro
```

> [!NOTE]
> 生成環境に `kicad-cli` が未導入であったため、本ディレクトリのファイルは `verify_kicad.py` による S 式構文・`1.27 mm` グリッド・ピン/電源 ERC 整合・重なり検査およびレンダリング画像の目視確認を実施しています。KiCad 8/9 GUI で開いた際は、組み込みの ERC（Electrical Rules Checker）も実行して確認してください。
