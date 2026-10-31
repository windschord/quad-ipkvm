# 境界の契約 (INTERFACES)

作成: 2026-10-31。`docs/ANTIGRAVITY_HANDOFF.md` の分担（HDMI 周り = Antigravity、それ以外 = Claude）の**境界**を固定する。
この契約を変えるときは、PR の本文に「契約の変更」の節を設け、**両方の担当が合意してから**マージする（合意の記録は PR のコメント）。

---

## 1. 映像: HDMI 担当 → 配信側（Claude 担当）

**HDMI 担当（Antigravity）が作るもの**は、JPEG のフレームを `frame_hub` に publish する `video_source_t` の実装（`csi_video_source`）まで。**配信（`WS /video`）、認証、ビューアは Claude 担当**で、この契約の外側にある。

### 1.1 `video_source_t`（`components/frame_hub/include/video_source.h`。すでにある）

```c
typedef struct video_source_t video_source_t;
struct video_source_t {
    const char *name;
    int  (*start)(const video_source_t *source, frame_hub_t *hub);  // 0 = 成功
    void (*stop)(const video_source_t *source);
    void *ctx;
};
```

- `start` は、publish するタスクを起動して**すぐに戻る**（ブロックしない）。`stop` は、タスクを止め、確保した資源を解放する。
- 実装の例: `components/test_pattern_source`（`test_pattern_source_get()`）。HDMI 担当は、同じ形で `csi_video_source_get()` を提供する。
- `app_main`（`main/main.c`）は、Kconfig の選択に応じて、使う `video_source_t` を 1 つ選んで `start` を呼ぶ（P2b-6 で `CONFIG_VIDEO_SOURCE_*` の選択を足す）。

### 1.2 `frame_hub` の publish の規則（`components/frame_hub/include/frame_hub.h`）

- publish は **1 つのスレッド**から行う（`frame_hub_begin_publish` → データを書く → `frame_hub_commit`）。書けなかったときは `frame_hub_abort_publish`。
- `frame_hub_commit(hub, frame, size, width, height, timestamp_ms)`: `size` は JPEG のバイト数、`width` / `height` は画素数、`timestamp_ms` は単調増加のミリ秒。
- `frame_hub_begin_publish(hub, required_size)` が `NULL` を返したら、**そのフレームを捨てる**。待たない。`NULL` の理由は 2 つある。(a) プールが埋まっている（クライアントが遅い）: `frame_hub` の統計 `dropped_frames` に**数えられる**。(b) `required_size` が 0、または `max_frame_size` を超える: **`dropped_frames` には数えられない**（実装は黙って `NULL` を返す）。(b) は HDMI 担当側で数えて記録する（ログか自前の統計）。
- フレームのバッファは `frame_hub` が管理する（`alloc_cb`）。`max_frame_size` を超える JPEG は publish しない（上の (b)。`frame_hub_commit` に超過した `size` を渡した場合は、`frame_hub` が捨てて `dropped_frames` に数えるが、先に `begin_publish` が `NULL` を返すので通常は届かない）。`max_frame_size` / `pool_size` の既定は `main/Kconfig.projbuild` の `FRAME_HUB_*`。**1080p の JPEG が収まらないときは、HDMI 担当が `FRAME_HUB_*` の値の変更を PR で提案する**（共有ファイルの規則は `ANTIGRAVITY_HANDOFF.md` の 5 章）。
- JPEG は、**ブラウザの `createImageBitmap` が解釈できる、通常のベースライン JPEG**（SOI `FFD8` で始まり EOI `FFD9` で終わる）。

### 1.3 映像の状態（P5 で使う。いまは任意）

KVM の切替（`kvm_logic`）は、映像の同期状態を `kvm_video_sync_state_t`（`UNKNOWN` / `NO_SIGNAL` / `LOCKED`。`components/kvm_logic/include/kvm_logic.h`）として受け取る。HDMI 担当は、**P5 の前に**、次の関数を `csi_video_source` に用意する（名前と意味を固定する。実装は P5）。

```c
// 現在の映像の同期状態。ブロックしない。どのタスクからも呼べる。
kvm_video_sync_state_t csi_video_source_get_sync_state(void);
// 取り込み中の解像度。LOCKED のときだけ有効（それ以外は 0）。
void csi_video_source_get_mode(uint32_t *width, uint32_t *height);
```

**一貫した読み出し**: `get_sync_state()` と `get_mode()` は別の呼び出しなので、2 回の間に状態が変わりえる（`LOCKED` を読んだあとに幅・高さが 0 になる、など）。KVM 側が状態と解像度を**同じ時点の値**で必要とするときは、次の 1 回の呼び出しを使う（実装は、タスク間の共有値を、ロックなしで一貫して読める形（シーケンス番号つきのコピー、またはクリティカルセクション）にする。ブロックしない規則は同じ）。

```c
typedef struct {
    kvm_video_sync_state_t state;
    uint32_t width;   // state が LOCKED のときだけ有効（それ以外は 0）
    uint32_t height;
} csi_video_status_t;
// 状態・幅・高さを同じ時点で返す。ブロックしない。どのタスクからも呼べる。
void csi_video_source_get_status(csi_video_status_t *out);
```

## 2. Kconfig の名前空間

| 接頭辞 | 担当 | 例 |
|---|---|---|
| `TC358743_*`、`MIPI_PHY_*`、`CSI_*`、`VIDEO_SOURCE_*`、`JPEG_*` | **HDMI 担当** | `TC358743_RESET_GPIO`、`MIPI_PHY_POWER_ENABLE`、`CSI_*` |
| `WEB_SERVER_*`、`NET_ETH_*`、`OLED_*`、`KVM_*`、`ATX_*`、`HID_*`、`FRAME_HUB_*` | **Claude** | `WEB_SERVER_WS_MAX_CLIENTS`、`NET_ETH_INIT` |

- 他の担当の接頭辞の項目は、**変更しない**（必要なときは、その担当に依頼する）。
- `FRAME_HUB_*` は Claude 担当だが、映像の大きさに依存するので、HDMI 担当が値の変更を提案してよい（上の 1.2）。
- 無効な Kconfig の bool は未定義になる。`main.c` で `if (CONFIG_X)` を使うときは、`#ifndef CONFIG_X / #define CONFIG_X 0 / #endif` を足す（既存の書き方に合わせる）。

## 3. 共有ファイル

| ファイル | 規則 |
|---|---|
| `main/main.c` | 自分の担当の初期化のブロックを、**小さな差分で**足す（`#if` か `if (CONFIG_X)` で囲む）。ほかの担当のブロックは変更しない |
| `main/Kconfig.projbuild` | 自分の接頭辞の項目だけを足す。既存の項目の並べ替えをしない |
| `main/CMakeLists.txt` | `REQUIRES` に、自分のコンポーネントを足すだけ（**足し忘れると idf-build が落ちる**） |
| `test/host/CMakeLists.txt` | 自分のコンポーネントの登録だけを、既存の書き方に合わせて足す |
| `sdkconfig.defaults` | 自分の接頭辞の既定だけを足す。`CONFIG_ESP_REV_*` などの全体の設定は、Claude に相談 |
| `docs/HW_VERIFY.md` | 自分の担当の章（HDMI 担当: E、D の映像関連）だけを書き換える。実機の測定は、「測定値 + 条件 + 日付」まで書く |
| `docs/PLAN.md`、`docs/HANDOFF.md`、`CLAUDE.md` | **Claude が更新する**。変更が必要なときは、Issue か PR のコメントで依頼 |

## 4. 変更のしかた

1. 契約を変えたい担当は、Issue を立てる（ラベル `contract`）。
2. 双方が合意したら、変更の PR を出す（本文に「契約の変更」の節）。
3. マージは、オーナーの指示で行う。
