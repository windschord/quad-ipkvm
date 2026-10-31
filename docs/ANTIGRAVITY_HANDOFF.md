# Antigravity との作業分担と、現状の整理 (ANTIGRAVITY_HANDOFF)

作成: 2026-10-31。オーナーの決定: **HDMI 周りの検証と実装は、ローカルで動く Antigravity が担当し、それ以外は Claude（クラウド）が担当する。**
連携方式は、**GitHub の PR を中心にし、所有権を決め、Issues をタスク盤にする**。

関連: `CLAUDE.md`（プロジェクトの規則）、`docs/HANDOFF.md`（決定事項）、`docs/PLAN.md`（フェーズ）、`docs/INTERFACES.md`（境界の契約）、`docs/HW_STATE.md`（実機の現状。実機担当が作成）、`docs/P2B_PLAN.md` と `docs/P2B_TC358743_REFERENCE.md`（HDMI の計画と、公開実装から集めた事実）、`docs/HW_VERIFY.md`（実機の確認記録）。

---

## 1. 体制

| 役割 | 担当 | 何をするか |
|---|---|---|
| オーナー | 人間 | 物理的な作業（配線、挿抜、テスターでの測定、電源）、最終判断、**マージの指示** |
| **HDMI 担当** | **Antigravity**（ローカル、実機と同じ PC） | TC358743、MIPI PHY 電源、CSI 受信、JPEG 化、`csi_video_source`、HDMI の実機検証 |
| **それ以外の担当** | **Claude**（クラウド） | 下の 3 章。計画、docs、レビュー、CI、Jules への依頼 |
| 実装の補助 | Jules（クラウド、Claude が依頼・監視） | Claude 担当の範囲の実装。HDMI 担当の範囲には出さない（例外: 下の 2.3） |
| 実機担当（Claude） | 別のローカルの Claude | **引き継ぎ（`docs/HW_STATE.md` の作成）のあと、HDMI の実機作業は Antigravity に移る。** HDMI 以外の実機確認の依頼先は、オーナーが指定する |

**Claude は実機に触れない。** Claude 担当の機能に実機確認が必要なときは、Claude が依頼文を書き、オーナーが、実機担当か Antigravity に渡す。

## 2. 現状（2026-10-31）

統合ブランチ `claude/brave-cori-5m9w1m`（`a2ffafc` 時点）。ホストテスト 36 件が通り、CI（`host-tests`、`idf-build`）が緑。

### 2.1 できていて、実機でも確認したもの
- ビルドと起動: ESP-IDF v6.0.1、チップ rev v1.3、PSRAM（HEX、200MHz、32MB）、Ethernet（IP101GRI）。
- **Web と認証（Claude 担当）**: HTTPS（自己署名、NVS に保存）、初回パスワード設定、PBKDF2 のワーカー、セッション Cookie、`WS /video`、最小ビューア。**ブラウザ（Chrome）でログインし、`/video` の wss でテストパターン（8 色のカラーバー）が表示される**ことを確認済み。フレームレートは Python クライアントで約 9.9 fps を実測した（`TEST_PATTERN_FPS=10`）。**Chrome 上の表示 fps は未測定**（`HW_VERIFY` D10、Z）。
- X630（TC358743）: **I2C（0x0F）が応答**、Chip ID 0x0000、EDID の書き込みが成功、電源（USB、電流制限つきのハブ）で異常なし。

### 2.2 できていて、ホストテストまで（実機は未確認）
- `kvm_logic`（切替と ATX の状態機械）、`mcp23017_logic`、`oled_ui`、`oled_render`、`auth_logic`、`http_guard`、`frame_hub`、`ws_video_core`、`tc358743_pll_calc`（P2b-1）、`mipi_phy_power`（P2b-2。**既定で無効。電圧は未測定**）、OLED-3a のアクセサ（IP・リンク速度・`needs_setup`・WS クライアント数）。

### 2.3 HDMI の状況（HDMI 担当が引き継ぐ）
- **映像は、まだ 1 フレームも取れていない。** `tc358743_init` は骨格で、HDMI RX の PHY・DDC・EDID の読み出し設定と、PLL・CSI・出力形式の起動手順が未実装（PC がディスプレイを認識しない原因）。
- **P2b-3（起動手順）は、Jules が実装中**（セッション `10919686634635935434`）。PR が出たら、Claude が一次レビュー（ホストテスト、CI、変異テスト）を行い、**HDMI 担当に渡す**。HDMI 担当は、それを土台にしても、書き直してもよい（実機で動くことが優先）。
- 決定事項（オーナー）: 最初のモードは **972Mbps・2 レーン・RGB888**（JPEG に直結）。**EDID は最初は 720p60 だけ**。**TC358743 の GPIO リセットを必須にする**（10 ピンの RESET → GPIO16。配線はオーナー）。MIPI PHY の電源は、LDO ch3（`ESP_LDO_VO3`）、既定 2500mV、**実機で電圧を測ってから有効にする**。
- 東芝の機能仕様書は**入手できない**。公開実装（p4kvm、ESP-KVM、Linux ドライバ）を参考に進める。事実のノートは `docs/P2B_TC358743_REFERENCE.md`。
- 計画: `docs/P2B_PLAN.md`（P2b-3 〜 P2b-8）。

### 2.4 実機の状態（詳細は `docs/HW_STATE.md`）
- ESP32-P4-ETH（rev v1.3）と X630 V1.5 を、22 ピン ↔ 15 ピンのケーブルで接続。HDMI は、PC（オーナー）から X630 に接続できる。
- X630 の 10 ピンコネクタは、**ESP 側が未配線**（RESET → GPIO16 は準備中。INT は配線しない）。
- 書き込み済みのファーム: ローカルのビルド（`CONFIG_TC358743_INIT=y`、`CONFIG_NET_ETH_INIT=y`、MIPI PHY の電源は無効）。
- 電流計なし（電流制限つきのハブで給電）。15 ピンの中間 GND は導通を測っていない。

### 2.5 Claude の作業（Antigravity は触れない）
- **未着手・進行中**: スモークテスト `tools/https_smoke_auth.sh`、遅い WS クライアントで `/status` が止まる問題（`HW_VERIFY` Z）、OLED-3b、MCP23017 / OLED-4、HID（P4）、KVM 切替と ATX（P5・P6）、H.264（P7）、mDNS、UI のエラーメッセージ。

## 3. 分担（所有権）

| 範囲 | 担当 |
|---|---|
| `components/tc358743/`、`components/mipi_phy_power/`、新規の `components/csi_capture/`・`components/csi_video_source/`、HDMI の EDID | **HDMI 担当** |
| `main/` の HDMI 関連のブロック、Kconfig の `TC358743_*` / `MIPI_PHY_*` / `CSI_*` / `VIDEO_SOURCE_*` / `JPEG_*` | **HDMI 担当** |
| `docs/HW_VERIFY.md` の E 章（X630）と D の映像関連、`docs/P2B_*.md` の更新 | **HDMI 担当**（`P2B_PLAN.md` の大きな変更は Claude に相談） |
| `components/web_server`、`auth_*`、`http_guard`、`ws_*`、`frame_hub`、`net_eth`、`oled_*`、`mcp23017_logic`、`kvm_*`、`test_pattern_source`、`tools/`、`.github/`、`CLAUDE.md`、`docs/PLAN.md`・`HANDOFF.md` ほか | **Claude** |
| 境界（`video_source_t`、`frame_hub` の使い方、`kvm_video_sync_state_t`） | **`docs/INTERFACES.md` の契約**（変更は両者の合意） |

## 4. ブランチと PR の規約

- **統合ブランチは `claude/brave-cori-5m9w1m`**。直接 push しない。**すべての変更は PR で、このブランチ向けに出す。**
- Antigravity のブランチ名: `antigravity/<トピック>`（例: `antigravity/p2b3-start-csi`）。
- **マージはオーナーの指示で、Claude が行う**（squash）。担当が自分の PR をマージしない。
- PR の本文に必ず書く: (1) 概要と、変更したファイル、(2) **確認したこと**（実行したコマンドと結果。実機で確認したものと、していないものを区別）、(3) **未確認一覧**（`TODO(hw-verify)` の項目）、(4) オーナー向けの実機確認手順、(5) 契約を変えたなら「契約の変更」。**確認していないものを「確認した」と書かない。**
- CI（`host-tests`、`idf-build`）が緑であること。Claude と CodeRabbit のレビューの指摘に対応する。
- コミットの規約: `feat` / `fix` / `docs` / `test` の接頭辞。コミットメッセージと PR の本文は日本語（技術用語とパスは英語のままでよい）。
- 共有ファイルの規則は `docs/INTERFACES.md` の 3 章。**`main/CMakeLists.txt` の `REQUIRES` の足し忘れ**は、CI でしか見つからない（過去に起きた）。
- 履歴を動かす操作（rebase、force push、`reset --hard`）をしない。
- スクラッチのファイル（パッチ、ログ、バックアップ）をリポジトリに入れない。

## 5. タスク盤（GitHub Issues）

- ラベル: `owner:antigravity`、`owner:claude`、`hw-verify`（実機確認が要る）、`blocked`、`contract`（境界の契約）。
- 1 タスク = 1 Issue。PR の本文で `Closes #番号`。担当は、Issue にコメントで進捗を書く。
- 相手の担当に依頼したいことは、**Issue を立てる**（担当のラベルを付ける）。

## 6. 実機と電気の安全（必ず守る）

- **電気的な数値（電圧、極性、タイミング）を推測で確定しない。** 根拠のない値は `TODO(hw-verify)` を残す。
- **MIPI PHY の電源（`VDD_MIPI_DPHY`）**: 絶対最大 2.75V。**有効にする前に、CSI ケーブルを外した状態で電圧を測る**。2.75V 超、範囲外、不安定なら、すぐに無効にする（`docs/HW_VERIFY.md` E14、PR #59）。
- **通電の手順**: 電流計がない間は、電流制限つきのハブを使い、温度・異臭・LED を見る。止める基準は `HW_VERIFY` E.1。**CSI の受信（P2b-4）の前に、電流計を用意する**。
- **ケーブルの向き**: 22 ピン側の接点面を間違えると、X630 の GND と ESP の 3V3 が繋がる。通電前に導通と短絡を確認する（`HW_VERIFY` E.1）。
- **X630 の `HDET`（10 ピンの 2 番）は ESP の GPIO に繋がない**（HDMI 接続で 4.8V が出る）。INT は、High の電圧を測るまで配線しない。
- **ATX（P6）は安全側に倒す**（Ē のプルアップ前提、パルス長の上限、`channel` の不一致は拒否）。これは Claude 担当だが、HDMI 担当が触る箇所には関係しない。
- 物理的な作業（配線、挿抜、テスター）は、**オーナーが行う**。Antigravity は、手順を書き、結果を記録する。

## 7. ライセンスと参照実装

- **コードはゼロから書く**（`CLAUDE.md`）。p4kvm・ESP-KVM・Linux ドライバは**参考**にするが、コードを複製しない。使うのは、レジスタ名、アドレス、値、式、順序などの**事実**。出典を `// Source:` で残す。
- **Linux の `tc358743.c` は GPL-2.0**。**参照のみ**。p4kvm・ESP-KVM（Apache-2.0）は、Linux のドライバに従うと自称している。そのコードを取り込むと、GPL の影響を受ける可能性がある（法的な判断はしない）。**コードを取り込むときは、先にオーナーと Claude に相談する**（LICENSE と NOTICE の保持が要る）。
- **ESP-KVM を「動く参照機」として動かすことは、許可されている**（公開済みのファームを書き込み、動作中のレジスタを読む）。読み出した値は事実として使える。**終わったら、元のファームを書き戻す。**
- **東芝の機能仕様書の流出物（TC358749XBG の V074 など）は、取得も参照もしない。**

## 8. 環境（実機と同じ PC）

- ESP-IDF v6.0.1（実機担当が導入: `~/.espressif/v6.0.1`、ツールは `~/.espressif/idf601-tools`）。**CI のイメージも v6.0.1**。master や 6.1 のソースを見て書かない（過去に `esp_driver_ldo` で idf-build が落ちた。v6.0.1 では LDO は `esp_hw_support` の中）。
- 実機で HTTPS に到達するには `CONFIG_NET_ETH_INIT=y` が必要（既定は n）。`sdkconfig` は未追跡（コミットしない）。
- ホストテスト: `cmake -S test/host -B b && cmake --build b -j8 && ctest --test-dir b --timeout 30`（ASan/UBSan 付き）。**IDF のビルドは、ローカルで行う**（Claude のクラウド環境では行えない）。
- シリアルのポート名、書き込みの手順、スクリプトの場所は `docs/HW_STATE.md`。
- ブラウザでの確認: 内蔵ブラウザは自己署名証明書で使えない。オーナーが証明書の承認とログインを行えば、Claude in Chrome で続きを確認できる。

## 9. 報告

- 実機の測定と確認は、`docs/HW_VERIFY.md` に「測定値 + 条件（何と何の間、機材、日付）」で書く（PR で）。
- 分からないことは「未確認」と書く。推測は [推測] と書く。
- Claude への依頼・質問は、Issue か PR のコメントで。
