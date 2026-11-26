# `quad-ipkvm` KiCad 回路図プロジェクト

Waveshare **ESP32-P4-ETH (rev v1.3)** ベースの 4ch IP-KVM 全体回路図（KiCad 8 / 9 対応・階層シート構成）です。一次仕様は [docs/WIRING.md](../../docs/WIRING.md) および [docs/HW_VERIFY.md](../../docs/HW_VERIFY.md) に準拠しています。

## 開き方

KiCad 8 または KiCad 9 で [`quad-ipkvm.kicad_pro`](quad-ipkvm.kicad_pro) を開いてください。すべてのシンボル定義は各 `.kicad_sch` の `(lib_symbols ...)` に埋め込まれているため、外部ライブラリのパス設定なしで開けます。

## シート構成（全 5 枚・A3 横）

| Page | ファイル | 内容 |
|---|---|---|
| 1 (Root) | [`quad-ipkvm.kicad_sch`](quad-ipkvm.kicad_sch) | `P1` 40pin ヘッダ、`J3` USB OTG HS 4pin ヘッダ、4 サブシート相互接続、基板内固定 I/F 境界・安全注記 |
| 2 | [`usb_vbus.kicad_sch`](usb_vbus.kicad_sch) | ターゲット PC USB 3本線 (`D-`/`D+`/`GND`) + VBUS 検出分圧回路（#174 決定: `R1=130 kΩ` / `R2=162 kΩ` ±0.5% E96、ESD 素子追加なし） |
| 3 | [`x630_csi.kicad_sch`](x630_csi.kicad_sch) | Geekworm X630 V1.5 (`TC358743XBG`): `J1` 22pin ↔ `X630` 15pin CSI FFC + `10pin SH1.0` (`RESET=GPIO16`, `INT=GPIO17[TODO]`, `HDET` 接続禁止) |
| 4 | [`i2c_oled_panel.kicad_sch`](i2c_oled_panel.kicad_sch) | 共有 I2C バス（`GPIO7/8`, 基板内 `R44/R46 2.2 kΩ`）、`SSD1306` OLED (`0x3C/0x3D`)、`MCP23017` (`SSCI-016506`, `0x20`, ボタン×5 + パネル LED×4) |
| 5 | [`atx_kvm_ctrl.kicad_sch`](atx_kvm_ctrl.kicad_sch) | 4ch ATX/KVM 切替（`74HC139 ×2` + フォトリレー×12 + 保護プル抵抗）、KVM LED 絶縁入力（フォトカプラ×4 + 外付け `10 kΩ PU`） |

## 反映済みの安全制約・オーナー決定

- **`J3` ピン 1 (`VCC_5V`)**: ターゲット PC VBUS へ接続禁止（NC）[H3]。
- **`X630` 10pin `#2 HDET`**: HDMI 接続時 4.8 V 出力のため ESP32-P4 GPIO へ接続禁止。
- **VBUS 分圧・ESD 方針（PR #174）**: `0.5% E96` の組（`R1=130 kΩ` / `R2=162 kΩ`）を使用、`J3`/VBUS 検出とも ESD 素子は追加しない（`R1` が直列制限抵抗を兼ねる）。電源投入順序は **ESP32-P4 (`H2`) → ターゲット PC**。
- **ATX/KVM 制御と KVM LED 入力**: `MCP23017` (`SSCI-016506`) は `/RESET` が内部固定のため出力ラッチ保持リスクがあり、**必ず直接 GPIO** に配線する。

## 未確定・暫定事項 (`TODO(hw-verify)`)

以下の項目は実機測定・部品選定前のため暫定表記となっています。実配線・PCB 化前に [docs/HW_VERIFY.md](../../docs/HW_VERIFY.md) で確定し、対応する Kconfig / 定数を更新してください。

- `P1` の ATX/KVM GPIO 割当（暫定候補: `GPIO20, 21, 22, 23, 46` を出力、`GPIO32, 33, 47, 48` を LED 入力）
- フォトリレー・フォトカプラの型番、駆動抵抗 `R21..R36`、`74HC139` シンク電流余裕（バッファ Tr 要否 `C1/C3`）
- パネル LED 制限抵抗 `R10..R13` (`G6`)、OLED モジュール I2C アドレス/PU (`G2/G3`)、`X630 #4 INT` High 電圧 (`B6`)

## 再生成と検証

本プロジェクトの `.kicad_pro` / `.kicad_sch` は以下のスクリプトで再生成・構造検証できます。

```bash
python3 tools/kicad/generate_schematic.py
python3 tools/kicad/verify_kicad.py hardware/kicad/quad-ipkvm.kicad_pro
```

> [!NOTE]
> 生成環境に `kicad-cli` が未導入であったため、本ディレクトリのファイルは `verify_kicad.py` による S 式構文・1.27 mm グリッド・接続/階層ピン整合・重なり検査およびレンダリング画像の目視確認を実施しています。KiCad 8/9 GUI で開いた際は、組み込みの ERC（Electrical Rules Checker）も実行して確認してください。
