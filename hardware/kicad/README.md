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
| [`quad_ipkvm.kicad_sym`](quad_ipkvm.kicad_sym) | プロジェクト用カスタムシンボルライブラリ (`ESP32-P4-ETH_P1`, `74HC139`, `Photorelay_SPST`, `Optocoupler_NPN` 等) |
| [`sym-lib-table`](sym-lib-table) | プロジェクトシンボルライブラリ参照定義 (`${KIPRJMOD}/quad_ipkvm.kicad_sym`) |

[`quad-ipkvm.kicad_sch`](quad-ipkvm.kicad_sch) 上では、中央ハブの **ESP32-P4-ETH (`P1` 40pin / `J3` 4pin)** から以下 5 ゾーンへすべての信号線を直接トレースできます。

1. **左上 — Target PC USB OTG HS & VBUS 分圧 (`J2` → `J3` / `P1 #4`)**: `D-`/`D+`/`GND` の 3 本直結に加え、#174 オーナー決定の **`R1=130 kΩ` / `R2=162 kΩ` ±0.5% E96** 分圧回路（ESD 素子なし）から `P1 #4 (GPIO18)` へ直結。
2. **左中 — Geekworm X630 V1.5 (`J4` 15p ↔ `J1` 22p CSI + `J5` 10p → `P1 #5/#6`)**: FFC 変換配線および `10pin #4 INT` (`GPIO17[TODO]`) / `#5 RESET` (`GPIO16`) を直結、`#2 HDET (4.8V)` は接続禁止 (`NC`) 明記。
3. **左下 — 共有 I2C バス (`GPIO7/8`) + `SSD1306` OLED + `MCP23017` (`SSCI-016506`)**: `P1 #16/#17` から X630 (`0x0F`)・OLED (`J6`, `0x3C/0x3D`)・MCP23017 (`U3`, `0x20`, パネルボタン×5 + LED×4) へ連続バス配線。
4. **右上/右中 — 4ch ATX & KVM 切替出力 (`P1` → プル抵抗 → `74HC139 ×2` → フォトリレー×12)**: `KVM_A0/A1` (`100 kΩ PD`) と `~E_PWR/~E_RST/~E_KVM` (`10 kΩ PU`) から `U1`/`U2` 出力を経て `K1..K12` へ直接配線。
5. **右下 — KVM LED 絶縁入力 (`OK1..OK4` → `10 kΩ PU` → `P1`)**: フォトカプラ×4 のコレクタ出力（外部 `10 kΩ PU` 付き）を直接 `P1 #26, #25, #22, #21` へ帰還配線。

## 反映済みの安全制約・オーナー決定

- **`J3` ピン 1 (`VCC_5V`)**: ターゲット PC VBUS へ接続禁止 (`NC`) [H3]。
- **`X630` 10pin `#2 HDET`**: HDMI 接続時 4.8 V 出力のため ESP32-P4 GPIO へ接続禁止 (`NC`)。
- **VBUS 分圧・ESD 方針（PR #174）**: `0.5% E96` の組（`R1=130 kΩ` / `R2=162 kΩ`）を使用、`J3`/VBUS 検出とも ESD 素子は追加しない（`R1` が直列制限抵抗を兼ねる）。電源投入順序は **ESP32-P4 (`H2`) → ターゲット PC**。
- **ATX/KVM 制御と KVM LED 入力**: `MCP23017` (`SSCI-016506`) は `/RESET` が内部固定のため出力ラッチ保持リスクがあり、**必ず直接 GPIO** に配線する。
- **KVM LED 極性の計測基準**: KVM 基板側の LED 端子 (`HW_VERIFY.md` `A6`) と **ESP32-P4 側 GPIO 端子（フォトカプラ出力：点灯時 Low・消灯時 High）** を区別して確認した上で、`CONFIG_KVM_BOARD_LED_POLARITY_ACTIVE_LOW`（既定 `UNSET`）を選択する。

## 未確定・暫定事項 (`TODO(hw-verify)`)

以下の項目は実機測定・部品選定前のため暫定表記となっています。実配線・PCB 化前に [docs/HW_VERIFY.md](../../docs/HW_VERIFY.md) で確定し、対応する Kconfig / 定数を更新してください。

- `P1` の ATX/KVM GPIO 割当（暫定候補: `GPIO20, 21, 22, 23, 46` を出力、`GPIO32, 33, 47, 48` を LED 入力）
- フォトリレー・フォトカプラの型番、駆動抵抗 `R21..R36`、`74HC139` シンク電流余裕（バッファ Tr 要否 `C1/C3`）
- パネル LED 制限抵抗 `R10..R13` (`G6`)、OLED モジュール I2C アドレス/PU (`G2/G3`)、`X630 #4 INT` High 電圧 (`B6`)、`J3` 実コネクタ形状/ピッチ (`H7`)
- `U3` (`MCP23017`, `SSCI-016506`) のピン番号 `1..20` は機能別論理番号であり、実基板ヘッダ対応は I2C 5pin (`1:3V3, 2:SDA, 3:SCL, 4:NC, 5:GND`), `JP1` (`GPA0..7`), `JP3` (`GPB0..7`)（`HW_VERIFY G4`）

## 再生成と検証

本プロジェクトのファイルは以下のスクリプトで再生成・構造検証できます。

```bash
python3 tools/kicad/generate_schematic.py
python3 tools/kicad/verify_kicad.py hardware/kicad/quad-ipkvm.kicad_pro
```

> [!NOTE]
> 生成環境に `kicad-cli` が未導入であったため、本ディレクトリのファイルは `verify_kicad.py` による S 式構文・`1.27 mm` グリッド・ピン/電源 ERC 整合・重なり検査およびレンダリング画像の目視確認を実施しています。KiCad 8/9 GUI で開いた際は、組み込みの ERC（Electrical Rules Checker）も実行して確認してください。
