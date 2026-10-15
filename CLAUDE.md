# CLAUDE.md

ESP32-P4 (Waveshare ESP32-P4-ETH, チップ rev v1.3) で作る、ベアメタル(FreeRTOS)の 4ch IP-KVM。
**最初に必ず `docs/HANDOFF.md` を読むこと。** 決定事項・実装順序・未確定事項はすべてそこにある。

## 要点

- ESP-IDF (C/C++)。第一候補は 6.0.1。
- 映像: TC358743 (HDMI→MIPI-CSI, 2レーン) → MJPEG を主、H.264 は低帯域用の副。配信は WebSocket + **HTTPS 必須**（RTSP は使わない）。
- HID: TinyUSB 複合デバイス（ブートキーボード + 絶対ポインタ + 相対ポインタ）。
- 4台管理: 前段に 4ch HDMI KVM。**KVM の LED を状態の真実とし、ATX の選択先はそれに追従する。**
- ATX: 74HC139 ×2 + フォトリレー×8。API は `channel` 必須。現在チャンネルと違えば 409。
- 実装順序は HANDOFF.md の 5 章に従う。**最初は「CSI でフレームを1枚取る」**。

## ルール

- ESP-KVM / p4kvm は**参考にするが、コードはゼロから書く**。直接コピーする場合は Apache-2.0 の LICENSE/NOTICE を保持する。
- ハードウェアの電気的な数値は推測で確定しない。`TODO(hw-verify)` を残して人間に確認する。
- ATX は安全側に倒す（Ē プルアップ前提、パルス長上限、`channel` 不一致は拒否）。
- 状態機械はホスト上でユニットテストできるようにする。
- 不明点・食い違いは、実装を進める前に報告する。
- 使用モデルは Sonnet または Haiku。
