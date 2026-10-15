# 作成計画 (PLAN)

`docs/HANDOFF.md` の決定事項に従う。本書は「誰が・何を・どの順で」を定める。
方針: **リスク順フェーズ（HANDOFF 5章）を維持しつつ、3トラックを並行**する。

## 1. 体制

| 役割 | 担当 | 内容 |
|---|---|---|
| 実装 | **Jules**（Jules API 経由） | 1タスク = 1セッション = 1 PR。`requirePlanApproval` は常に true |
| 監視・レビュー | Claude (Sonnet) | プラン評価、PR 差分レビュー、受入基準の照合、Issue/進捗管理 |
| 助言 | Claude (Opus) | 設計判断で迷ったときのみ相談 |
| 実機確認・書き込み | **人間（オーナー）** | Jules/Claude は実機に触れない。実機確認が要る項目は PR/Issue に `hw-verify` ラベルで残す |

Jules の VM から実機は見えない。したがって **実機が完了条件に含まれるフェーズでは、Jules の完了条件は「CI（ホストテスト + ESP-IDF ビルド）が緑」まで**とし、実機での動作確認はオーナーが行って結果を Issue に記録する。

## 2. トラック

- **Track A（ファーム本線）**: フェーズ 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7（HANDOFF 5章の順）
- **Track B（ホスト純ロジック）**: 切替/ATX 状態機械をハード非依存で書き、ホスト上でユニットテスト。フェーズ 6 の前提。
- **Track C（実機確認・部品選定）**: HANDOFF 6章の未確定事項 1〜4,6 をオーナーが確認し `docs/HW_VERIFY.md` に記録。

## 3. リポジトリ構成（フェーズ 0 で作る）

```
CMakeLists.txt / sdkconfig.defaults      # ESP-IDF プロジェクト (rev v1.3 向け)
main/                                    # app_main のみ。薄く保つ
components/
  kvm_logic/      # ★ハード非依存の純 C/C++。状態機械・タイムアウト・不変条件。IDF ヘッダを include しない
  kvm_board/      # GPIO/I2C の HAL。kvm_logic が使う小さな interface を実装 (実機専用)
  tc358743/ video_pipeline/ web/ hid/ ...  # 後続フェーズで追加
test/host/        # kvm_logic のホストテスト (CMake + Unity もしくは GoogleTest。macOS/Linux)
docs/             # HANDOFF.md, PLAN.md, HW_VERIFY.md, 各フェーズの結果記録
.github/workflows/ci.yml                 # ホストテスト + idf.py build
```

## 4. 仕様の追加決定（HANDOFF 3.3 への補足）

本節が HANDOFF 3.3 の補足の唯一の置き場（HANDOFF 側にはここへの参照だけを置く）。

**合意済み（オーナー承認）**
- **切替後の HDMI 再同期待ちにはタイムアウトを設ける。タイムアウト／無信号でも ATX のロックは解除する。**
  理由: 電源オフのターゲットは無信号のままであり、ATX で起動できなくなるのを防ぐ。映像状態は「無信号」として API で通知する。
  手動切替の追従でも同じ再同期待ち（上限あり）を行う。タイムアウト値はパラメータ化し `TODO(hw-verify)`。
  （不変条件 2 の「切替中」= ボタン押下〜LED 確定 + 再同期待ち（上限あり）。）

**Opus レビューによる安全側の追加（TB-1 の必須要件。異論があれば差し戻し可）**
- **不変条件 4（A0/A1 の変更順序）**: A0/A1 は 3 つのデコーダ区画で共有のため、いずれかの Ē が Low の間は A0/A1 を変えない。
  変更は「全 Ē を High → A0/A1 変更 → セットアップ時間を置く → 必要な Ē を Low」の順。
- **UNKNOWN 状態**: LED が全消灯・複数点灯・チャタリング・起動直後など「現在チャンネルを確定できない」間は UNKNOWN とし、ATX ロック・全 Ē High・A0/A1 保持。LED 安定判定時間はパラメータ（`TODO(hw-verify)`）。
- **競合シナリオの期待動作を表で定義しテストと 1 対 1 対応**: 切替中の手動切替、タイムアウト後に LED が遅れて n になる、ATX パルス中の LED 変化、切替要求の重複（Web とパネル同時／現在チャンネルへの切替）、切替中の ATX 要求。
- **パルス中の LED 監視**: パルス中も LED を周期監視し、不一致なら即座に Ē を High に戻す。「直後の再確認」は異常として記録・通知する定義に改める（出し終えた後では中断できないため）。
- **パルス長上限の多重防御**: ロジック側と `kvm_board` 側の双方で上限を clamp し、`kvm_board` はハードウェアタイマー（esp_timer one-shot 等）で自動解放する。

## 4.1 `kvm_logic` の設計方針（TB-1）

- 言語/テスト: **C99 + Unity + CTest に固定**（Unity はバージョン固定で vendoring か FetchContent）。`-Wall -Wextra -Werror` と ASan/UBSan を有効化。テストは自動発見し、テスト追加で `ci.yml` を編集しない。
- `kvm_logic` は IDF ヘッダを include しない。IDF ビルドにも含め、ターゲット(RISC-V)でコンパイルできることを保証する。
- **ブロッキング禁止の純関数**: `kvm_logic_step(state, event, now_ms) -> actions`。時刻は単調増加 `uint64_t` ms を外部注入（`uint32` を使う箇所はラップアラウンドのテストを追加）。
- HAL は `set_addr` / `set_enable(sel, level)` / `read_led_raw` / `video_sync_state` 程度の小さな interface。`video_sync_state` は `LOCKED / NO_SIGNAL / UNKNOWN`（実装は P5）。
- 実行モデル: 1 つの kvm タスクが状態を占有し、Web/パネルはキュー経由で `channel` 付きコマンドを投げる。**`channel` の照合（409 判定）はロジック内で行う**（TOCTOU 回避）。
- HID の入力変換は別の純粋コンポーネント（`hid_logic`）に置く（P4）。`kvm_logic` に混ぜない。

## 5. フェーズと Issue

各 Issue の「完了条件」を満たすことが受入基準。HW = オーナーの実機確認が必要。

| ID | タイトル | 依存 | Jules の完了条件 | HW |
|---|---|---|---|---|
| T0-1 | リポジトリ雛形 + sdkconfig(rev v1.3) + ホストテスト基盤 + CI（旧 P1 を統合） | なし | 3 章の構成で `idf.py build`（ブートログを出す `app_main`）とホストテスト（ダミー1件）が CI で緑。CI は全 PR と作業ブランチ push で走り、IDF イメージはバージョン固定。rev v1.3 の Kconfig は IDF ソースの該当箇所を引用して根拠を示す（示せなければ `TODO(hw-verify)`）。PR に Actions 実行リンクを貼る | — |
| P1 | ブートログ確認（オーナー作業） | T0-1 | 実機に書き込み、チップ rev v1.3 とブートログを Issue に記録 | 要 |
| TB-1 | `kvm_logic`: 切替/ATX 状態機械（TDD） | T0-1 | 4・4.1 章の要件すべて。HANDOFF 3.3 の全シナリオ＋4 章の競合表をホストテストで網羅し、不変条件 1〜4 に対応するテストが存在 | — |
| TC-1 | `docs/HW_VERIFY.md`（確認チェックリスト） | なし | HANDOFF 6章の各項目について「何を・どう測り・結果をどこに書くか」。**Claude が作成**しオーナーが記入 | オーナーが記入 |
| P2a | TC358743 I2C 初期化 | T0-1, TC-1 | 読み取り系（chip id 等）＋初期化シーケンスがビルド可。診断ログ（chip id、レジスタダンプ、CSI エラーカウンタ）を出力。`TODO(hw-verify)` を明示 | 要 |
| P2b | CSI/ISP でフレーム 1 枚取得 → JPEG 保存 | P2a | 同上。解像度変更追従のコールバック骨格を含む | 要（フェーズ 2 の完了条件） |
| P3 | MJPEG + HTTPS(自己署名) + WebSocket + 認証 | P2b | 認証なし拒否、初期パスワード強制、`WS /video` | 要 |
| P4 | HID（ブートKB + 絶対/相対ポインタ）＋ `hid_logic` | P3 | TinyUSB 複合デバイスがビルド可。`WS /ws` の入力変換は `hid_logic`（ホストテスト可）に置く | 要（BIOS） |
| P5 | KVM 経由の動作確認 | P3,P4 | 解像度/チャンネル変更時の再同期ロジック。`video_sync_state` の実装 | 要 |
| P6 | ATX 4ch + KVM 切替 + パネル + API | TB-1,P5 | `kvm_board` HAL 実装、API（`channel` 必須・不一致 409）。TB-1 のテストが引き続き緑 | 要 |
| P7 | H.264 低帯域モード | P3 | PPA 変換含むビルドが緑。設定で MJPEG/H.264 切替 | 要 |

各 Issue には「触ってよいディレクトリ」を明記し、並行タスク間のコンフリクトを避ける（例: TB-1 は `components/kvm_logic/` と `test/host/kvm_logic/` のみ、P2a は `components/tc358743/` のみ）。ルートの `CMakeLists.txt`・`ci.yml`・`sdkconfig.defaults` は T0-1 以外では原則変更しない。

## 6. 進め方（初手）

1. docs 配置・本計画を push（完了後に Issue 起票）。
2. Jules: **T0-1 を先行**（以降のタスクが依存するため単独で）。
3. 並行して Claude が TC-1（`docs/HW_VERIFY.md`）を作成し、オーナーに実機確認を依頼（P2a のクリティカルパス）。
4. T0-1 マージ後、**TB-1 と P2a を並行**で Jules に依頼（互いに別ディレクトリ）。オーナーは P1 を実施。
5. P2b 以降は実機ログをオーナーが Issue/既存セッションに戻すループで進める。GPIO・KVM 配線の回答（Track C）を受けて P6。

## 7. Jules 運用ルール（Claude が監視）

- 依頼文は `docs/HANDOFF.md`・`CLAUDE.md`・本書・対象 Issue を必読と明記する。
- プランは評価してから承認（受入基準との整合、スコープ逸脱、既存ファイルの削除がないか）。
- PR はサマリではなく**差分全体**を読む。スコープ外の削除、テストの削除/スキップは差し戻す。
- ハードウェアの電気的数値を Jules が推測で決めた場合は差し戻し、`TODO(hw-verify)` に置換させる。
- Apache-2.0 の ESP-KVM / p4kvm のコードを直接コピーしていないか確認する（コピーする場合は LICENSE/NOTICE 保持）。
- レビュー対応は既存セッションに `send-message`（新規セッションは作らない）。
- Jules の PR は `claude/brave-cori-5m9w1m`（現作業ブランチ）を base にする。最終的にこのブランチを main へ PR する。
