# P2b の計画: 実際の HDMI 映像を CSI で取り込み、JPEG にして配信する

作成: 2026-10-28。`docs/P2B_PREP.md`（下調べ）、`docs/PLAN.md`（P2b の行、4.2〜4.3 章）、`docs/HANDOFF.md`（5 章の実装順序、4 章の技術的な前提）をもとに、P2b を **作業単位に分割**し、**判断が必要な点を選択肢として整理**する。**ハードウェアは操作していない（机上の整理）。**

事実の区別は `P2B_PREP.md` と同じ: **[出典]** 一次情報で確認、**[参照]** 他のプロジェクトを読んで確認（コードは複製しない）、**[計算]** 計算値、**[推測]**、**[未確認]**。電気的な数値は、確認するまで確定しない（`TODO(hw-verify)`）。

---

## 1. 目標と完了条件

- **目標**: P3 で作った配信の経路（`frame_hub` → `WS /video` → ブラウザ）に、**テストパターンの代わりに、TC358743 が取り込んだ実際の HDMI 映像の JPEG** を流す。
- **完了条件（HANDOFF 5 章のフェーズ 2 + P3 との結合）**:
  1. PC 直結（KVM なし）で、**ブラウザに PC の画面が映る**（MJPEG、`wss`）。
  2. 取り込んだ 1 フレームを、JPEG として確認できる（ブラウザ、または保存）。
  3. **PC 側の解像度を変えても追従する**（少なくとも、再取り込みでき、黒画面のまま固まらない）。
- **範囲外**: H.264（P7）、KVM 経由（P5）、HID（P4）、音声。

## 2. 現状（すでにあるもの）

| もの | 状態 |
|---|---|
| `components/tc358743`（P2a） | I2C 0x0F で応答、チップ ID 検証、EDID 書き込み（成功）、診断ログ。**PLL（`tc_set_pll`）と CSI の設定（`tc_set_csi`）は、`pll_prd` / `pll_fbd` / CSI タイミングが未指定のとき「UNVERIFIED」の警告を出してスキップする骨格のみ**。`CSI_CONFW`（色形式・レーン数）は未実装（`TODO(hw-verify)`） |
| `frame_hub` / `video_source_t`（`start` / `stop`） | あり。`test_pattern_source` が `video_source_t` を実装して、JPEG を一定の fps で publish している |
| 配信（`WS /video` + ビューア） | あり（`#51`）。実機での確認は、これから |
| 実機（X630 + ESP32-P4-ETH、rev v1.3） | I2C は応答。**CSI のケーブル（22pin ↔ 15pin）の接続と、CSI の物理的な受信は、まだ試していない**（`HW_VERIFY` E1・E2、`BRINGUP` 段階 2 までが完了） |

## 3. 新しく分かったこと（この計画の作成時に確認）

1. **ESP-IDF v6.0.1 の `esp_cam_ctlr_csi` のドライバは、MIPI の PHY の電源を扱わない** [出典: `components/esp_driver_cam/csi/src/esp_cam_ctlr_csi.c` と `esp_cam_ctlr_csi.h` に、LDO・電源に関する記述がない]。ESP32-P4 は MIPI の D-PHY を内蔵の LDO（`esp_ldo_regulator`）から給電でき、`esp_ldo_acquire_channel` の `chan_id` は「**回路図とチップのデータシートで決める**」と IDF のドキュメントにある [出典: `docs/en/api-reference/peripherals/ldo_regulator.rst`]。**アプリ側で、PHY の電源を有効にする必要がある**見込み。
   - 本ボードの回路図（`docs/ESP32-P4-ETH-schematic.pdf`）には `VDD_MIPI_DPHY` と `ESP_LDO_VO1`〜`VO4` のネットが載っている。**`VDD_MIPI_DPHY` がどの LDO に繋がるかは、テキスト抽出では確認できなかった** [未確認]。標準的な構成は VO3（電圧は 2.5V 前後）と記憶しているが、**電圧は推測で確定しない**。→ **P2b-0 で確認**。
2. **ESP-KVM の README は、「TC358743 の起動手順（PLL の分周、MIPI のタイミングカウンタ、HPD と CSI 送信器を立ち上げる順序）」と「`esp_cam_ctlr` API が公開しない `MIPI_CSI_BRIDGE` レジスタの直接設定」が、最も難しい部分だったと記している** [参照: ESP-KVM の README の Credits]。ただし、それが当時の ESP-IDF のバージョンの話か、v6.0.1 でも必要かは **[未確認]**。v6.0.1 の `esp_cam_ctlr_csi_config_t` には、`data_type`（独自のデータタイプ）、入出力の色形式、`byte_swap_en` などの設定がある [出典: `P2B_PREP.md` §3-1]。**まず公式 API で試し、足りなければレジスタを直接設定する**（§5 の選択肢 1 と 4）。
3. **他の人の実測（参考値）**: 同じ ESP32-P4-ETH（rev v1.3）で、1080p の MJPEG は約 20 fps、H.264 は約 7 fps（PPA の色変換が約 104 ms 掛かるため）[参照: ESP-KVM の `docs/HOW-IT-WORKS.md`]。MJPEG を主とする方針（HANDOFF 4 章）と整合する。1080p50 を既定の EDID にして、エンコーダが 1 フレームおきに取る、という運用もしている [参照]。**自機では未検証**。
4. **`video_source_t`（`start` / `stop`）はすでにある**。実ソースは、これを実装して `frame_hub` に JPEG を publish すれば、P3 の配信がそのまま使える（P3 の 4.2 章の「映像ソースの抽象化」の狙いどおり）。

## 4. 未確定の点（P2b の着手前に決める・確かめる）

| # | 点 | 確かめる方法 | 担当 |
|---|---|---|---|
| Q1 | `VDD_MIPI_DPHY` の給電元（LDO のチャンネル番号と電圧） | 回路図の該当ページを目視（PDF を画像にして読む）、チップのデータシートの `VDD_MIPI_DPHY` の推奨電圧 | 実機担当（机上） |
| Q2 | 22pin ↔ 15pin ケーブルの入手と、向き・ピン対応（`HW_VERIFY` E1・E2） | 導通確認、ケーブルを実際に挿す | オーナー |
| Q3 | TC358743 の CSI のタイミング値（`LINEINITCNT` 等）と、起動手順の順序 | §5 の選択肢 | — |
| Q4 | **TC358743 の出力形式の選択**（UYVY か RGB888）と、`CSI_CONFW` の値 | Linux ドライバの該当部分の参照（複製しない）、実機で確認 | — |
| Q5 | **JPEG への入力経路**（`P2B_PREP.md` §3-6 の A / B / C） | 実機での実験（取り込んだ生フレームを使う） | — |
| Q6 | CSI から ISP を通す必要があるか（`bypass_isp` で足りるか） | 実機での実験 | — |
| Q7 | HDMI を接続しない状態の検証手段（TC358743 の内蔵テストパターン）があるか | データシート（レジスタ表は公開版に無い）、Linux ドライバの該当の有無 | 実機担当（机上） |
| Q8 | Toshiba の機能仕様書（Rev 0.60）の入手可否 | オーナー | オーナー |

## 5. 判断が必要な点: P2b の進め方（選択肢。おすすめ順）

「CSI のタイミング値と起動手順（Q3）、色の経路（Q4・Q5）が未確認のまま、どう進めるか」。

| 順位 | 案 | 内容 | 理由 |
|---|---|---|---|
| **1（おすすめ）** | **公式 API を使った段階実行 + 値は Linux ドライバを参照して導く** | (1) PHY の電源 → (2) TC358743 の PLL と CSI の起動（レーンレート 972Mbps = `pll_prd=4`、`pll_fbd=144`。最初のモードは 720p60 UYVY）→ (3) `esp_cam_ctlr` で 1 フレームを PSRAM に取り込み、生データの統計で検証 → (4) JPEG（経路 B → A の順に実験）→ (5) `video_source_t` として `frame_hub` に publish。タイミング値は、Linux ドライバの 594Mbps / 972Mbps の値を**参照のみ**で読み、MIPI D-PHY の仕様と突き合わせて**自分で書き直す**（根拠を文書に残す） | 実績のある値（Linux、Raspberry Pi）に基づくので、試行錯誤が少ない。公式 API が使えれば、レジスタ直接設定のリスクを避けられる。段階ごとに、I2C のエラーカウンタや生データで**失敗箇所を特定できる** |
| 2 | 1 の方針で、**最初のモードを 1080p30 RGB888（経路 C）** にする | 色の並べ替えの問題（UYVY ↔ YVYU）を避け、JPEG の `RGB888` 入力へ直接渡す | 色の経路の未確認（Q5）が最小。ただし 1080p30 の RGB888 は 749Mbps/レーンで余裕が小さく、フレームも大きい（約 6MB）。fps が出ない見込み |
| 3 | 1 の方針で、**最初から PPA で RGB に変換する経路 A** にする | UYVY → PPA（RGB565/RGB888）→ JPEG | PPA の UYVY 入力は公式に対応しており、確実。ただし他の人の実測で 1080p で約 104 ms の変換が掛かっており、fps が下がる |
| 4 | **レジスタ直接設定（`MIPI_CSI_BRIDGE`）から始める** | ESP-KVM / p4kvm が行った方法を読み、自分で書く | v6.0.1 の公式 API で足りるか未確認のまま、非公開の領域に手を出すことになる。**公式 API が動かなかったときの退路**として取っておく |
| 5 | **実機でタイミング値を探索する** | LINEINITCNT などを変えながら、CSI のエラーカウンタが 0 になる組を探す | 参照できる値が無いときの最後の手段。時間が掛かり、再現性の根拠が弱い |
| 6 | **Toshiba の機能仕様書の入手を待つ** | 入手できれば、レジスタの意味から導く | 最も確実だが、入手できる見込みが立たない。待つ間、作業が止まる。**並行して依頼しておく**（Q8） |

**おすすめは 1**。案 2・3 は、1 の (4) の色の経路の実験で試す候補。4 は 1 が行き詰まったときの退路、5・6 はそれぞれ補助の手段。

## 6. 作業の分割

依存関係: P2b-0 / P2b-1 / P2b-2 は並行できる。P2b-3 は P2b-1・P2b-2 のあと。P2b-4 以降は実機での確認とセット。

| ID | 内容 | 担当 | 依存 | 触るところ | 完了条件 |
|---|---|---|---|---|---|
| **P2b-0** | 机上の確認: Q1（`VDD_MIPI_DPHY` の給電元）、Q7（TC358743 のテストパターンの有無）、Q8 の依頼、E1・E2 のケーブル | 実機担当 + オーナー | なし | `docs/HW_VERIFY.md` | 各項目の結果が `HW_VERIFY` に記録される |
| **P2b-1** | `tc358743_pll_calc`（純 C）: `refclk` とレーンレートから、`pll_prd` / `pll_fbd` / `FRS` を求める。範囲外・不整合は失敗を返す | Jules | なし | `components/tc358743/`（新規ファイル）、`test/host/tc358743/` | `P2B_PREP.md` §2-3 の表（720p60 / 1080p30 / 1080p50 の `fbd`）をホストテストで再現。変異テスト |
| **P2b-2** | PHY 電源 `mipi_phy_power`（IDF）: `esp_ldo_acquire_channel` で PHY を給電。**チャンネル番号と電圧は Kconfig（既定値は Q1 の確認後に決める。それまでは無効）** | Jules | P2b-0（Q1） | `components/mipi_phy_power/`（新規）、`main/Kconfig.projbuild` | `idf-build` が緑。実機で `VDD_MIPI_DPHY` の電圧を測れる状態 |
| **P2b-3** | `tc358743_start_csi(mode)`: PLL と CSI の起動手順（P2b-1 の計算 + 参照して導いたタイミング値）、`CSI_CONFW`、出力形式、HPD。診断ログ（CSI のステータス・エラー）が実値になるか確認できる | Jules → 実機 | P2b-1, P2b-2 | `components/tc358743/` | `idf-build` が緑。実機で、CSI のステータスが「受信中」になる（`BRINGUP` 段階 3） |
| **P2b-4** | `csi_capture`（IDF）: `esp_cam_ctlr_csi` で 2 レーン・UYVY の 1 フレームを PSRAM に取り込む。受信完了のコールバック、統計（フレーム数・エラー数）、解像度変更の検出の骨格 | Jules → 実機 | P2b-3 | `components/csi_capture/`（新規） | `idf-build` が緑。実機で、1 フレームが取れ、生データの統計（ゼロ以外の画素など）が妥当 |
| **P2b-5** | JPEG 化の経路の実験（経路 B → A → C）。結果を `HW_VERIFY` に記録。確定した経路を `csi_capture` に組み込む | Jules → 実機 | P2b-4 | `components/csi_capture/` | 取り込んだ画面が JPEG として正しい色・向きで保存/表示できる |
| **P2b-6** | `csi_video_source`（`video_source_t` の実装）: 取り込み → JPEG → `frame_hub` への publish。`Kconfig` で `test_pattern_source` と切り替え | Jules → 実機 | P2b-5 | `components/csi_video_source/`（新規）、`main/main.c`、`main/Kconfig.projbuild` | ブラウザ（`#51` のビューア）に PC の画面が映る |
| **P2b-7** | 解像度変更への追従（再同期）、`HW_VERIFY` の更新 | Jules → 実機 | P2b-6 | 同上 | PC 側で解像度を変えても、映像が復帰する |

### 6.1 実機担当の作業（P2b-0 と、各段階の確認）
- P2b-0: 上の表。Q1 は **回路図の PDF を画像にして、`VDD_MIPI_DPHY` のネットがどの LDO（`ESP_LDO_VO?`）に繋がるかを、目視で確認**し、チップのデータシートの `VDD_MIPI_DPHY` の推奨電圧を記録する。
- P2b-2 以降は、各 PR のマージ後に、`BRINGUP` の手順に従って確認し、結果を `HW_VERIFY` に記録する。

### 6.2 Jules に出すときの注意（過去の失敗から）
- **値・API は、一次情報で確認する**（IDF のソース、MIPI D-PHY の仕様）。**Linux ドライバは参照のみ**（式・制約の事実だけを使い、コードを複製しない）。出典を `// Source:` で残す。
- **レジスタの値を推測で埋めない**。根拠が示せない値は `TODO(hw-verify)`。
- 変異テストは、一時ディレクトリで行う（作業ツリーを直接壊さない。提出前に `git diff` と `grep` で確認）。

## 7. リスク

- **CSI のタイミング・起動手順が合わず、黒画面のまま**になる（最大のリスク。ESP-KVM も最難関と記している）。→ 段階実行と、エラーカウンタによる切り分け（§5 の 1）。
- **rev v1.3 の制約**: CSI の色変換と入力側の入れ替えが使えない [出典: `P2B_PREP.md` §3-1]。色の経路が限定され、fps が下がる可能性（他の人の実測では 1080p MJPEG が約 20 fps）。
- **PSRAM の帯域**: 1080p の生フレーム（UYVY で約 4MB）を CSI が書き込み、JPEG エンコーダと PPA が読む。`frame_hub`、HTTPS の送信と競合する。→ 実測が必要。
- **PHY の電源の誤り**: 電圧を間違えると、動かないだけでなく、チップに負荷を掛けうる。→ Q1 の確認前は、Kconfig で無効にし、既定値を推測で入れない。
- **I2C バスの共有**（OLED、MCP23017 の追加。`PLAN.md` 4.4）: TC358743 のポーリングと競合しないこと。

## 8. 出典

- `docs/P2B_PREP.md`（PLL の式と候補、ESP-IDF v6.0.1 の CSI / ISP / JPEG / PPA の API と rev v1.3 の制約）。
- ESP-IDF v6.0.1: `components/esp_driver_cam/csi/`（`esp_cam_ctlr_csi.c` / `.h` に電源の記述がないこと）、`docs/en/api-reference/peripherals/ldo_regulator.rst`（`chan_id` は回路図で決める）。
- ESP-KVM の README と `docs/HOW-IT-WORKS.md`（公開されている説明のみ。コードは読んでいない）。
- `docs/ESP32-P4-ETH-schematic.pdf`（`VDD_MIPI_DPHY` と `ESP_LDO_VO1`〜`VO4` のネット名の存在のみ確認）。
