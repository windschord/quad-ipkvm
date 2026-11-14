# P7 H.264 低帯域モード評価レポート (Issue #93 / A-12)

作成日: 2026-11-14  
対象環境: ESP32-P4 rev v1.3 / ESP-IDF v6.0.1 / `espressif/esp_h264` v1.4.1 / Geekworm X630 V1.5 (TC358743)  
位置づけ: Issue #93 (A-12) に基づく評価結果。本ドキュメントでは **ソース・実機実測で確認した事実（`[確認]`）** と **未実装の設計案・見通し（`[推測]`）** を明確に分離する。

---

## 0. 結論サマリー

1. **HW H.264 エンコーダは利用可能 `[確認]`**: ESP32-P4 rev v1.3 + `espressif/esp_h264` v1.4.1 で Baseline Profile の HW エンコーダが動作し、1フレーム目から SPS/PPS 付きの Annex B バイトストリーム（`00 00 00 01 67 42 c0 ...`）を生成できる。一方、SW エンコーダ（OpenH264）は `640x480` で `235.20 ms`（`4.3 fps` 上限）・`1280x720` で `586.38 ms`（`1.7 fps` 上限）・PSRAM `+10.3 MiB` となり実運用不可 `[確認]`。
2. **rev v1.3 では PPA による `RGB888 -> O_UYY_E_VYY`（YUV420）色空間変換が必須 `[確認]`**: `esp_h264` v1.4.1 は `CONFIG_ESP_REV_MIN_FULL < 300` において入力形式を `ESP_H264_RAW_FMT_O_UYY_E_VYY` のみに制限している（`BGR888` / `UYVY` 直接入力は `>= 300` 専用）。さらに ESP-IDF v6.0.1 の `ppa_ll.h` を確認したところ、PPA SRM の `YUV422` 入力自体も `CHIP_SUPPORT_MIN_REV >= 300` 専用（rev v1.3 では非対応）であり、rev v1.3 で HW 変換できるのは `RGB888 -> YUV420` 経路のみとなる。
3. **1080p のフレームレートは MJPEG より下がる（720p でも同様） `[確認]`**: PPA 変換が `1280x720` で `33.86〜33.93 ms`、`1920x1080` で `76.35〜76.37 ms` を要するため、合計フレーム間隔（PPA + HW H.264）は **`720p` で約 `48.7 ms`（上限 `20.5〜20.6 fps`）**、**`1080p` で約 `109.3〜109.6 ms`（上限 `9.1〜9.2 fps`）** となる。現行 HW JPEG（`RGB888` 直結: `720p 30.00 fps` / `1080p 15.00 fps`）より高速にはならず、**H.264 は完全に「低帯域モード」** である。
4. **静止画〜カーソル移動時の帯域削減効果は極めて大きい `[確認]`**: 静止 P フレームは `93〜592 B`（同 JPEG 比約 `1/200〜1/4,000`）、小カーソル移動時 P フレームも 1080p `2000/4000 kbps` 設定で `251〜1,385 B` に収まる。一方、1080p 高情報量 IDE 画面の **IDR フレームは最大 `507,122 B`（約 `495 KiB`）** に達し、現行の `FRAME_HUB_MAX_FRAME_KB = 256` 既定値を超えるケースがある `[確認]`。
5. **推奨判断 `[推測]`**: まずは MJPEG（P2b/P3）+ P4 HID + P5/P6 を完成させ、P7 は要望通り **副モード（低帯域モード）** として実装するのが妥当。実装フェーズへ進む場合は `frame_hub`（サイズ上限と P フレーム欠落耐性/`force_idr`）・`video_frame_t`（コーデック種別/IDR フラグ）・`WS /video`（コーデック切替またはメッセージ識別）について **`contract` ラベルの Issue で Claude 側と合意を取る必要がある `[推測]`**。

---

## 1. ESP32-P4 rev v1.3 / ESP-IDF v6.0.1 の H.264 エンコーダ仕様（ソース確認）

対象コンポーネント: `espressif/esp_h264` `1.4.1`（`interface/include/esp_h264_version.h`, `interface/include/esp_h264_types.h`, `interface/include/esp_h264_enc_single.h`, `interface/include/esp_h264_enc_param.h`, `hw/include/esp_h264_enc_single_hw.h`, `hw/src/esp_h264_enc_hw_param.h`, `hw/src/esp_h264_enc_hw_param.c`, `hw/src/esp_h264_enc_single_hw.c`, `sw/include/esp_h264_enc_single_sw.h`）

### 1.1 HW エンコーダ API と仕様 `[確認]`

- **初期化・制御 API（単一ストリーム）**:
  - `esp_h264_enc_hw_new(const esp_h264_enc_cfg_hw_t *cfg, esp_h264_enc_handle_t *out_enc)`（`esp_h264_enc_cfg_hw_t` は `esp_h264_enc_cfg_t` の `typedef`）
  - `esp_h264_enc_open(enc)` → `esp_h264_enc_process(enc, &in_frame, &out_frame)` → `esp_h264_enc_close(enc)` → `esp_h264_enc_del(enc)`
  - パラメータ取得・動的変更: `esp_h264_enc_hw_get_param_hd(enc, &param_hd)` 経由で `esp_h264_enc_set_fps` / `esp_h264_enc_set_gop` / `esp_h264_enc_set_bitrate` / **`esp_h264_enc_force_idr`** を呼び出せる。
  - `esp_h264_enc_process` は IDR フレーム先頭に SPS と PPS の NAL ユニットを自動付与する（`esp_h264_enc_single.h` 註記および実機ベンチで先頭バイト `00 00 00 01 67 ...` を確認）。`out_frame.frame_type` は `ESP_H264_FRAME_TYPE_IDR` / `ESP_H264_FRAME_TYPE_I` / `ESP_H264_FRAME_TYPE_P`、`out_frame.length` に出力長が入る。
- **解像度・プロファイル・パラメータ上限（ソース定数・チェックより）**:
  - プロファイル: **Baseline Profile**（最大 36,864 マクロブロック/フレーム、`esp_h264_version.h` 記載）
  - 解像度（`hw/src/esp_h264_enc_hw_param.h`・`esp_h264_enc_hw_res_check`）:
    - 幅: `ESP_H264_MIN_WIDTH = 80` 〜 `ESP_H264_MAX_WIDTH = 1920`（16 の倍数アライン要件あり、`esp_h264_enc_single.h`）
    - 高さ: `ESP_H264_MIN_HEIGHT = 80` 〜 `ESP_H264_MAX_HEIGHT = 2032`（16 の倍数アライン要件; 1080p は `1920x1088` に 16 の倍数アラインまたはパディングが必要になる点に注意、後述）
  - QP: `0 .. ESP_H264_QP_MAX (51)` かつ `qp_max >= qp_min`
  - `fps` / `gop`: `uint8_t`、`esp_h264_enc_hw_new` で `cfg->fps > 0` かつ `cfg->gop > 0`（`1 .. 255`）
  - `bitrate`: `uint32_t`（`esp_h264_enc_hw_new` にハードコードの上限チェックはなくレート制御パラメータへ渡される）
- **ESP32-P4 rev v1.3 (`CONFIG_ESP_REV_MIN_FULL < 300`) の入力フォーマット制約 (`esp_h264_types.h:848-880`)**:
  - `#if !defined(CONFIG_ESP_REV_MIN_FULL) || CONFIG_ESP_REV_MIN_FULL < 300` のとき、`ESP_H264_HW_IS_SUPPORTED_PIC_TYPE(pic_type)` は **`(pic_type) == ESP_H264_RAW_FMT_O_UYY_E_VYY` のみ true**。
  - `BGR888` / `RGB565_LE` / `VUY` / `UYVY` の HW 直接入力サポートは `CONFIG_ESP_REV_MIN_FULL >= 300`（rev v3.0 以降）向けブロックに限定されている（`esp_h264_enc_hw_param.c:51` の `h264_ori_color_space_t` 変換も同様）。

### 1.2 SW エンコーダ（OpenH264）が不採用となる根拠 `[確認]`

- 実装は OpenH264 ベース（`esp_h264_enc_sw_new`、実行時ログ `H264_ENC.SW: openh264 version: 9ead944`）。入力は `YUYV` / `I420` のみで、`esp_h264_enc_force_idr` は **`ESP_H264_ERR_UNSUPPORTED`** を返す（新規視聴者・パケット欠落時の即時 IDR 復旧ができない）。
- 実機ベンチ（ESP32-P4 @ 360 MHz、`a7_a12_bench.log`）:
  - `640x480`: 平均 **`235.20 ms`/frame（上限 `4.3 fps`）**、`int_delta = 93,116 B`、`psram_delta = 3,651,320 B`
  - `1280x720`: 平均 **`586.38 ms`/frame（上限 `1.7 fps`）**、`int_delta = 193,304 B`、`psram_delta = 10,275,072 B`
- `720p` で既に `1.7 fps` かつ PSRAM 約 `9.8 MiB` を占有するため、`1080p` や HTTP/WS/HID との並行動作には実用的でない。

---

## 2. PPA での色変換と入力経路（`RGB888` vs `UYVY`）

### 2.1 PPA が必須となる理由 `[確認]`

- 前項の通り ESP32-P4 rev v1.3 の HW H.264 エンコーダは **`ESP_H264_RAW_FMT_O_UYY_E_VYY`（PPA 出力の `PPA_SRM_COLOR_MODE_YUV420` = `ESP_COLOR_FOURCC_OUYY_EVYY` に一致）しか受け付けない**。
- 一方、TC358743 + CSI ブリッジ経路は `RGB888` または `UYVY`（rev v1.3 では CSI DMA の 32-bit ワード内エンディアン反転補正が必要）を出力し、`O_UYY_E_VYY` を直接生成できない。したがって rev v1.3 では **HW H.264 の前段に PPA SRM が必須**。
- さらに ESP-IDF v6.0.1 の `components/esp_hal_ppa/esp32p4/include/hal/ppa_ll.h:214-295` を確認したところ、`PPA_SRM_COLOR_MODE_YUV422_UYVY` 等は `#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) >= 300` で囲われており、rev `< 300` では `"YUV422 not supported by PPA SRM hardware before P4 ECO5"` として非対応になっている。

### 2.2 `RGB888` 入力と `UYVY` 入力の比較（ソース + P2b-8 / 今回ベンチの実測より）

| 項目 | `RGB888` 入力 → PPA `RGB888 -> YUV420` | `UYVY` 入力 → `YUV420` 経路（rev v1.3 での可否） |
|---|---|---|
| CSI 実機実績 (`docs/HW_VERIFY.md` §E P2b-8) | `720p60` / `1080p30` で安定（**rev v1.3 既定**） | `UYVY` 自体は `972/594 Mbps` で受信可能。ただし CSI DMA 4B 反転のため、P2b-8 では `ARGB8888` 偽装による PPA 4B 反転を要した |
| PPA 色変換対応 (`ppa_ll.h`) / 実測時間 | `RGB888 -> YUV420` **対応 `[確認]`**。<br>A-12 実測:<br>- `1280x720`: **`33.86〜33.93 ms`** (max `34.26 ms`)<br>- `1920x1080`: **`76.35〜76.37 ms`** (max `77.08 ms`) | PPA SRM の `YUV422` 入力は **`CHIP_SUPPORT_MIN_REV >= 300` 限定で rev v1.3 非対応 `[確認]`**。<br>（参考: P2b-8 の `ARGB8888` 偽装 4B 反転のみでも `1280x720`: `ppa 24.04 ms` / `1920x1080`: `ppa 54.91 ms` を要した。加えて CPU 等で `UYVY -> YUV420` 変換が必要になり非現実的） |
| MJPEG とのモード切替コスト | CSI を停止・再初期化せず、**フレーム単位で「HW JPEG」と「PPA → HW H.264」を切り替え可能 `[推測]`** | CSI / TC358743 レジスタの両方をピクセル形式ごとに再初期化する必要がある `[推測]` |
| PSRAM 使用量（CSI バッファ分） | `1080p` 2 面 = `12,441,600 B` | `1080p` 2 面 = `8,294,400 B` |

- **結論**:
  - **rev v1.3 では現行既定の `RGB888` 入力から PPA 1 パス（`PPA_SRM_COLOR_MODE_RGB888` → `PPA_SRM_COLOR_MODE_YUV420`）で `O_UYY_E_VYY` を生成する経路一択 `[確認]`**。
  - `UYVY` 直接入力または `UYVY -> YUV420` の HW 変換はいずれも rev v3.0 以降（`CHIP_SUPPORT_MIN_REV >= 300`）向けの機能である `[確認]`。

> [!NOTE]
> `1920x1080` で `YUV420` 入力を使う場合、`1080` は 16 の倍数ではない（`1080 = 67 * 16 + 8`）ため、実機ベンチ（`a7_a12_bench`）では `esp_h264_enc_cfg_hw_t.res = {1920, 1080}` を指定したうえで、入力 `O_UYY_E_VYY` バッファを 16px アライン高さ `1088` 分（`1920 * 1088 * 3 / 2 = 3,133,440 B`）確保してゼロ初期化し、PPA からは `pic_w = 1920, pic_h = 1080`（`3,110,400 B`）で先頭へ出力した。生成された SPS（§3.2）は `68 MB`（`1088 px`）＋ `frame_cropping_flag = 1`・`frame_crop_bottom_offset = 4`（下端 `8 px` クロップ）を自動付与して `1920x1080` としてデコードされるため、本番実装時もこの `3,133,440 B` バッファ確保とパディング整合を維持する必要がある `[確認・注意点]`。

---

## 3. 実機ベンチマーク結果（720p / 1080p: 時間・帯域・メモリ・CPU）

実測ログ: `~/quad-ipkvm-hw-artifacts/logs/a7_a12_bench.log`（ESP32-P4 rev v1.3、CPU `360000000 Hz`、ESP-IDF v6.0.1、`Heap before alloc: free_int=545155 free_spiram=32763576`）。  
各テストケースで **静的（static P）/ カーソル小領域変化（cursor P）/ 全体パン変化（pan P）** の 3 種類のフレーム挙動を測定し、同一画像に対する HW JPEG（q=50〜80）と比較した。

### 3.1 1280x720（720p）結果 `[確認]`

*(参考: 現行のライブ MJPEG 実機配信実績（`docs/HW_VERIFY.md` P2b-8、Ubuntu デスクトップ `q=70`）: **`30.00 fps`**, エンコード平均 `22.49 ms`)*

#### (a) 同一ソースでの JPEG vs HW H.264 比較

| シーン | 形式・設定 | PPA 平均 / 最大 | エンコード / 合計平均 (上限 fps) | 静的サイズ | 小変化 (cursorP) | 大変化 (panP / forced IDR) |
|---|---|---|---|---|---|---|
| `live_hdmi` | JPEG q=80 | — | `17.34 ms` | `133,468 B` | — | — |
| `live_hdmi` | JPEG q=70 | — | `16.62 ms` | `112,068 B` | — | — |
| `live_hdmi` | JPEG q=60 | — | `16.60 ms` | `98,138 B` | — | — |
| `live_hdmi` | JPEG q=50 | — | `16.59 ms` | `88,593 B` | — | — |
| `live_hdmi` | H.264 `1500 kbps` | `33.86 ms` / `34.26 ms` | enc `14.88 ms` / **total `48.74 ms` (`20.5 fps`)** | IDR `72,248 B` (`13.89 ms`), **staticP `117 B`** (`14.56 ms`) | **`289 B`** (`14.96 ms`) | panP **`36,341 B`** (`15.02 ms`), forceIDR `49,117 B` |
| `live_hdmi` | H.264 `3000 kbps` | `33.87 ms` / `34.08 ms` | enc `14.88 ms` / **total `48.76 ms` (`20.5 fps`)** | IDR `72,248 B` (`13.89 ms`), **staticP `159 B`** (`14.56 ms`) | **`7,759 B`** (`14.97 ms`) | panP **`48,938 B`** (`15.02 ms`), forceIDR `121,220 B` |
| `photo_sonoma_720p_crop` | JPEG q=70 | — | `16.65 ms` | `136,514 B` | — | — |
| `photo_sonoma_720p_crop` | H.264 `1500 kbps` | `33.87 ms` / `34.08 ms` | enc `14.85 ms` / **total `48.72 ms` (`20.5 fps`)** | IDR `96,909 B` (`12.44 ms`), **staticP `93 B`** | **`490 B`** | panP `17,361 B`, forceIDR `21,297 B` |
| `photo_sonoma_720p_crop` | H.264 `3000 kbps` | `33.89 ms` / `34.12 ms` | enc `14.83 ms` / **total `48.72 ms` (`20.5 fps`)** | IDR `96,909 B` (`12.44 ms`), **staticP `167 B`** | `2,380 B` | panP `36,515 B`, forceIDR `192,522 B` |
| `desktop_ide_720p_crop` | JPEG q=80 | — | `17.68 ms` | **`372,976 B`** | — | — |
| `desktop_ide_720p_crop` | JPEG q=70 | — | `16.88 ms` | **`298,258 B`** (> 256 KiB) | — | — |
| `desktop_ide_720p_crop` | JPEG q=60 | — | `16.80 ms` | `251,137 B` | — | — |
| `desktop_ide_720p_crop` | JPEG q=50 | — | `16.75 ms` | `219,542 B` | — | — |
| `desktop_ide_720p_crop` | H.264 `1500 kbps` | `33.89 ms` / `34.13 ms` | enc `14.75 ms` / **total `48.63 ms` (`20.6 fps`)** | IDR **`253,403 B`** (`10.91 ms`), **staticP `103 B`** | **`1,769 B`** | panP `103,100 B`, forceIDR `116,987 B` |
| `desktop_ide_720p_crop` | H.264 `3000 kbps` | `33.93 ms` / `34.15 ms` | enc `14.75 ms` / **total `48.68 ms` (`20.5 fps`)** | IDR **`253,403 B`** (`10.86 ms`), **staticP `123 B`** | **`1,768 B`** | panP `103,100 B`, forceIDR `116,987 B` |

- NALU 先頭確認（`live_hdmi` 等 720p）: `00 00 00 01 67 42 c0 1f 95 a0 14 01 6c 84 00 00 03 00 04 00 00 03 00 f2`（Annex B + Baseline Profile Level 3.1 = **`avc1.42c01f`**）。
- 720p メモリ増分: `int_delta = 103,804〜103,808 B`（約 `101.4 KiB`）、`psram_delta = 1,382,732 B`（約 `1.32 MiB`）。

### 3.2 1920x1080（1080p）結果 `[確認]`

*(参考: 現行のライブ MJPEG 実機配信実績（`docs/HW_VERIFY.md` P2b-8、Ubuntu デスクトップ `q=70`）: **`15.00 fps`**, エンコード平均 `52.70 ms`)*

- **1080p 測定条件 `[確認]`（`a7_a12_bench` 実装および出力 SPS NALU 解析より）**:
  - **エンコーダに渡した高さ（`esp_h264_enc_cfg_hw_t.res`）**: `width = 1920`, `height = 1080`（`esp_h264_enc_hw_new()` へ直接指定）。
  - **PPA の出力サイズ（`ppa_srm_oper_config_t.out`）**: `pic_w = 1920`, `pic_h = 1080`（`PPA_SRM_COLOR_MODE_YUV420`、`buffer_size = 1920 * 1080 * 3 / 2 = 3,110,400 B`）。
  - **パディング方法（PPA 出力バッファの末尾ゼロパディング＋エンコーダ SPS クロップ）**: `O_UYY_E_VYY` 入力バッファを 16px アライン高さ `mb_h = 1088` 分（`enc_yuv_bytes = 1920 * 1088 * 3 / 2 = 3,133,440 B`）確保して `memset(0)` でゼロ初期化し、先頭 `3,110,400 B` に PPA 出力（`1920x1080`）を書き込んで（末尾 8 ライン相当 `23,040 B` はゼロパディング）`esp_h264_enc_process()` へ `in_frame.raw_data.len = 3,133,440 B` を渡した。生成された SPS（下掲 `67 42 c0 29 95 a0 1e 00 89 79 61 ...`）のビット解析では `pic_width_in_mbs_minus1 = 119`（`1920 px`）、`pic_height_in_map_units_minus1 = 67`（`68 MB = 1088 px`）、`frame_cropping_flag = 1`、`frame_crop_bottom_offset = 4`（YUV420 の `CropUnitY = 2` により下端 `8 px` クロップ）が自動設定されており、デコーダ表示解像度は `1920x1080` となる。

| シーン | 形式・設定 | PPA 平均 / 最大 | エンコード / 合計平均 (上限 fps) | 静的サイズ | 小変化 (cursorP) | 大変化 (panP / forced IDR) |
|---|---|---|---|---|---|---|
| `synth_natural_1f` | JPEG q=70 | — | `37.40 ms` | `187,842 B` | — | — |
| `synth_natural_1f` | H.264 `4000 kbps` | `76.36 ms` / `76.55 ms` | enc `33.25 ms` / **total `109.61 ms` (`9.1 fps`)** | IDR `92,571 B` (`29.88 ms`), **staticP `592 B`** | `21,976 B` | panP `55,655 B`, forceIDR `275,264 B` |
| `photo_sonoma_horizon_scale` | JPEG q=80 | — | `38.17 ms` | **`307,815 B`** (> 256 KiB) | — | — |
| `photo_sonoma_horizon_scale` | JPEG q=70 | — | `37.46 ms` | `237,644 B` | — | — |
| `photo_sonoma_horizon_scale` | JPEG q=60 | — | `37.39 ms` | `194,547 B` | — | — |
| `photo_sonoma_horizon_scale` | JPEG q=50 | — | `37.37 ms` | `166,735 B` | — | — |
| `photo_sonoma_horizon_scale` | H.264 `2000 kbps` | `76.35 ms` / `76.54 ms` | enc `33.18 ms` / **total `109.53 ms` (`9.1 fps`)** | IDR `147,066 B` (`29.48 ms`), **staticP `167 B`** | **`251 B`** | panP `28,283 B`, forceIDR `32,614 B` |
| `photo_sonoma_horizon_scale` | H.264 `4000 kbps` | `76.36 ms` / `76.55 ms` | enc `33.18 ms` / **total `109.54 ms` (`9.1 fps`)** | IDR `147,066 B` (`29.47 ms`), **staticP `419 B`** | **`420 B`** | panP `48,045 B`, forceIDR `190,483 B` |
| `photo_sonoma_horizon_scale` | H.264 `6000 kbps` | `76.36 ms` / `76.56 ms` | enc `33.18 ms` / **total `109.54 ms` (`9.1 fps`)** | IDR `147,066 B` (`29.47 ms`), **staticP `424 B`** | `39,219 B` | panP `55,556 B`, forceIDR `411,951 B` |
| `photo_sonoma_horizon_crop` | JPEG q=80/70/60/50 | — | `38.34 / 37.59 / 37.49 / 37.45 ms` | `400,103 / 317,819 / 265,821 / 231,569 B` | — | — |
| `photo_sonoma_trees_crop` | JPEG q=80/70/60/50 | — | `37.77 / 37.22 / 37.20 / 37.21 ms` | `61,917 / 45,376 / 41,191 / 39,768 B` | — | — |
| `desktop_ide_terminal` | JPEG q=80 | — | `38.63 ms` | **`748,226 B`** (> 512 KiB) | — | — |
| `desktop_ide_terminal` | JPEG q=70 | — | `37.96 ms` | **`599,710 B`** (> 512 KiB) | — | — |
| `desktop_ide_terminal` | JPEG q=60 | — | `37.80 ms` | **`505,732 B`** (> 256 KiB) | — | — |
| `desktop_ide_terminal` | JPEG q=50 | — | `37.72 ms` | **`441,657 B`** (> 256 KiB) | — | — |
| `desktop_ide_terminal` | H.264 `2000 kbps` | `76.35 ms` / `76.55 ms` | enc `32.94 ms` / **total `109.29 ms` (`9.2 fps`)** | IDR **`507,122 B`** (`25.06 ms`), **staticP `189 B`** | **`1,385 B`** | panP `204,350 B`, forceIDR `234,567 B` |
| `desktop_ide_terminal` | H.264 `4000 kbps` | `76.37 ms` / `77.08 ms` | enc `32.94 ms` / **total `109.31 ms` (`9.1 fps`)** | IDR **`507,122 B`** (`25.06 ms`), **staticP `189 B`** | **`1,385 B`** | panP `204,350 B`, forceIDR `234,567 B` |
| `desktop_ide_terminal` | H.264 `6000 kbps` | `76.35 ms` / `76.55 ms` | enc `32.94 ms` / **total `109.30 ms` (`9.1 fps`)** | IDR **`507,122 B`** (`25.06 ms`), **staticP `244 B`** | **`1,346 B`** | panP `204,350 B`, forceIDR `234,567 B` |
| `synth_white_noise` (JPEG 最悪値参考) | JPEG q=80/70/60/50 | — | `40.05 / 38.80 / 38.68 / 38.44 ms` | `1,539,130 / 1,295,882 / 1,124,750 / 994,850 B` | — | — |

- NALU 先頭確認（`synth_natural_1f` 等 1080p）: `00 00 00 01 67 42 c0 29 95 a0 1e 00 89 79 61 00 00 03 00 01 00 00 03 00`（Annex B + Baseline Profile Level 4.1 = **`avc1.42c029`**）。
- 1080p メモリ増分: `int_delta = 155,004〜155,008 B`（約 `151.4 KiB`）、`psram_delta = 3,133,772 B`（約 `2.99 MiB`）。
  - 備考: `desktop_ide_terminal` の `IDR` が `2000/4000/6000 kbps` でいずれも `507,122 B`、`panP_avg` が `204,350 B` に揃っているのは、初期フレーム/高詳細場面で QP 範囲下限〜RC 追従の上限近傍に張り付くためと考えられる `[推測]`。

### 3.3 実測から判明した重要な数値整理

1. **「1080p で JPEG（15 fps）より fps が上がるか？」→ 上がらない（`15.0 fps` → `9.1 fps` に低下） `[確認]`**:
   - ボトルネックは `esp_h264` 単体（`33 ms` 級）ではなく、その前段で必須になる **PPA の `RGB888 -> YUV420` 変換（`76.36 ms`）**。
   - パイプライン並列化（CSI 取り込み + フレーム N-1 の PPA / フレーム N-2 の H.264 を別タスクで重複実行）を組んでも、1 段の最大時間 `ppa_avg = 76.36 ms` が律速となり、理論上限は `1000 / 76.36 ≒ 13.1 fps` まで（直列時は `1000 / 109.54 = 9.13 fps`）`[推測]`。
2. **PSRAM / 内部 RAM の余力 `[確認]`**:
   - HW H.264 エンコーダ単体の増分は 1080p でも内部 RAM `~151 KiB`、PSRAM `~3.0 MiB`（`free_spiram` 初期 `32,763,576 B`〔`31.25 MiB`〕に対し、CSI + `frame_hub` 搭載後も十分余裕あり `[推測]`）。
   - ※ `desktop_ide_terminal` の 1080p IDR が **`507,122 B`（約 `495 KiB`）** に達したため、P7 で 1080p IDR を安全に流すなら `FRAME_HUB_MAX_FRAME_KB` は `512` 以上、あるいは IDR 分割・`max_frame_size` 設計が必要。
3. **CPU 負荷 `[確認 + 未実測]`**:
   - 単体ベンチログ（`a7_a12_bench.log`）では `uxTaskGetSystemState` 等による `%` 表示の CPU 使用率は計測していない（**CPU 使用率 % は `[未実測]`**）。
   - 観測された挙動: HW JPEG・PPA SRM・HW H.264 はいずれも専用 HW/DMA と完了割り込み待機で動作するため、`ppa + enc = 109.6 ms` の連続実行でも `task_wdt` 発火なしで完了した。一方 SW OpenH264 は 1 コア（360 MHz）の演算時間を `235〜586 ms`/frame 消費した `[確認]`。

---

## 4. 配信方法の候補と契約・設計への影響

### 4.1 配信方式: `WS /video` + ブラウザ `WebCodecs (VideoDecoder)` `[確認 + 推測]`

`docs/HANDOFF.md` §3 の決定（**RTSP は使わない、HTTPS 必須**）に沿う構成を検証した。

- **ストリーム形式**: `esp_h264_enc_hw_new` の出力が先頭スタートコード `00 00 00 01`（IDR 時は SPS+PPS 同梱）を持つ **Annex B** であるため、`VideoDecoderConfig` において **`description` を省略（`undefined`）し、codec 文字列のみ指定** して `EncodedVideoChunk`（`type: "key"` / `"delta"`）へ直接渡す構成が最も単純である（`[確認]`: W3C WebCodecs 仕様では `description` 省略時は Annex B インバンド SPS/PPS として扱われる）。
- **コーデック文字列**:
  - 実機生成 NAL の SPS 先頭 3 バイト（`67 <profile> <compat> <level>`）から機械生成可能 `[確認]`:
    - `1280x720`: `67 42 c0 1f` → **`avc1.42c01f`**（Baseline, Level 3.1）
    - `1920x1080`: `67 42 c0 29` → **`avc1.42c029`**（Baseline, Level 4.1）
- **ブラウザ対応状況（WebCodecs `VideoDecoder`）`[確認]`**:
  - **Chrome / Edge（Chromium 系）**: Chrome 94 以降対応。
  - **Safari（macOS / iOS）**: Safari 16.4 以降対応。
  - **Firefox**: Firefox 130 以降（デスクトップ）で既定有効。
  - **制約**: **Secure Context（HTTPS）必須**（本機は自己署名 HTTPS 前提のため満たせる `[確認]`）。ただし端末・OS・ドライバによって H.264 デコーダ有効性が異なるため、ブラウザ側で **`VideoDecoder.isConfigSupported({ codec })` を事前確認し、非対応なら MJPEG へフォールバック** する設計が安全 `[推測]`。

### 4.2 `frame_hub` / `video_source_t` / `WS /video` 契約への影響

現行契約（`docs/INTERFACES.md` §1、`components/frame_hub/include/frame_hub.h`、`components/frame_hub/frame_hub.c`、`docs/WEB_API.md` §5）を確認したところ、P7 実装時には次の **5 点で契約・定数設計の見直しが必要 `[確認]`** となる。

1. **最新フレームスキップ（P フレーム欠落）およびエンコード後公開失敗と H.264 の非互換 (`components/frame_hub/frame_hub.c:177-178, 266-279`) `[確認]`**:
   - 現行 `frame_hub_get_latest_frame()` は読み取り時に無条件で `hub->latest_slot` に飛び、中間の `seq` を飛ばす（MJPEG では仕様通り正しい）。また `frame_hub` は `seq` を **`frame_hub_commit()` の成功時にのみ進める**（`components/frame_hub/frame_hub.c:177-178`）。
   - しかし H.264 は P フレームが直前フレームに依存するため、**1 フレームでも飛ばすと IDR まで映像が破綻する**。さらに、`esp_h264_enc_process()` が成功して HW エンコーダの参照状態だけが進んだ後に `frame_hub_begin_publish()` 等の公開処理が失敗した場合、**`seq` 自体は欠番にならず連続して見える**ため、受信側の `seq` 不連続検査だけでは欠落を検出できない。
   - **対策案 `[推測]`**:
     - (案 A・推奨) 配信契約として次の **3 条件すべて**で `video_source` に `request_keyframe()`（内部で `esp_h264_enc_force_idr`）を要求し、次の IDR が到着するまで該当クライアント（および公開欠落後の全クライアント）への P フレーム配信を抑止する（既存の「遅延クライアントを最新へ追いつかせる」設計を保ちつつ H.264 を壊さない）:
       1. 配信タスク側でクライアントごとの `frame->seq != expected_seq`（リング上書き等による `seq` 不連続）を検出したとき、
       2. 新規 `WS /video` 接続が参加したとき、
       3. **`video_source` 側で `esp_h264_enc_process()` 実行後の `frame_hub` 公開処理（`frame_hub_begin_publish()` 等）が失敗したとき**（`seq` が連続に見えてしまうため、`video_source` 自身が即座に次回フレームの `force_idr` フラグを立てて次公開フレームを必ず IDR にする）。
     - (案 B) リング全フレーム読み出し API を足すが、遅いクライアントがプール（既定 3 スロット）を枯渇させる問題が出るため案 A が優位。
2. **`video_frame_t` へのメタデータ追加 (`components/frame_hub/include/frame_hub.h`) `[確認]`**:
   - 現在 `video_frame_t` には `data, size, width, height, timestamp_ms, seq` しかなく、フレームが **JPEG か H.264 か、また H.264 のとき IDR (key) か P (delta) か** を判別できない。
   - 最低限 `codec`（例: `JPEG` / `H264_ANNEXB`）と `flags`（`KEYFRAME` 等。WebCodecs の `EncodedVideoChunk({ type })` に必須）の追加が要る。
3. **`video_source_t` へのモード切替・IDR 要求インタフェース (`components/frame_hub/include/video_source.h`) `[確認]`**:
   - 現在 `start` / `stop` のみ。「設定で MJPEG / H.264 切替」「新規クライアント接続時・`seq` 不連続検知時・エンコード後公開失敗時の `request_keyframe()` / `force_idr()` 要求」を扱う拡張（コールバック追加または制御関数）が必要。
4. **`WS /video` プロトコルと複数クライアント同時接続時のコーデック方針 (`docs/WEB_API.md` §5 / `docs/INTERFACES.md` §1) `[確認 + 推測]`**:
   - 現在は「1 バイナリメッセージ = JPEG 1 フレーム」固定。
   - P7 では「URL クエリ（例: `/video?codec=h264`）＋バイナリメッセージ先頭の数バイトヘッダ（コーデック・keyframe フラグ・タイムスタンプまたは codec string 情報）」や制御メッセージの導入が必要。
   - **異種コーデック同時要求時の方針（未決定・契約フェーズで確定）**: §2.2 / §3 の実測通り `video_source` の生成ストリームは単一コーデックに制約され、1080p で JPEG（`~37.5 ms`）と PPA＋H.264（`~109.6 ms`）を同時生成すると `~147 ms`/frame（`< 7 fps`）となりメモリも二重化するため **両形式の同時生成・同時配信は行わない**。複数接続で異なるコーデックが要求された場合（例: 一方が H.264 視聴中に他方が MJPEG フォールバックを要求）の扱いは §5.1 の契約フェーズで次のいずれかを選択し、`docs/WEB_API.md` と `docs/INTERFACES.md` に明記する（本レポート段階は実装前評価のため共有契約文書は未変更）:
     - **選択肢 A（互換優先・MJPEG 自動フォールバック共有）**: MJPEG のみを再生可能なクライアントが 1 台でも接続中の間は映像ソースを MJPEG に降格して全クライアントへ配信（フレームヘッダの codec 識別子で既存接続も自動追従）し、全クライアントが H.264 対応時のみ H.264 で生成する。
     - **選択肢 B（現行モード維持・追従または明示拒否）**: サーバ設定（または先着接続）でアクティブな生成コーデックに後続接続を合わせるか、一致しないコーデックを要求した接続を WebSocket Close `1008`（または HTTP `409 Conflict`）で明示拒否する。
5. **`FRAME_HUB_MAX_FRAME_KB` の配慮 (`docs/HW_VERIFY.md`・今回ベンチ) `[確認]`**:
   - 1080p 高情報量 IDE 画面の HW H.264 IDR が **`507,122 B`**（`495.2 KiB`）を実測しており、現行 `256 KiB` 枠では起動直後や `forceIDR` 時にドロップされる。H.264 モード有効時は `FRAME_HUB_MAX_FRAME_KB >= 512`（あるいは 768〜1024 KiB）の確保が必要（`1920x1080` パディング/バッファ確保も考慮）。

---

## 5. 実装する場合の計画案（`[推測]`）

P7 に進むと決定した場合、Claude 側と合意のうえ以下の段階構成で実装するのが最小リスクである。

1. **契約フェーズ（`contract` ラベル Issue 作成 → 合意）**:
   - 上記 §4.2 の項目（`video_frame_t` 拡張、`video_source_t` の `request_keyframe` / コーデック設定、エンコード後公開失敗時の自己 `force_idr` 要求、`WS /video` の H.264 フレーミング、**単一エンコーダ制約下での複数クライアント異種コーデック同時要求ポリシー（自動 MJPEG 降格共有 vs 既存形式追従/不一致拒否）**、`FRAME_HUB_MAX_FRAME_KB` 上限方針）を決定し、`docs/INTERFACES.md` と `docs/WEB_API.md` に固定する。
   - 新 Kconfig 接頭辞（例: `VIDEO_H264_*` または `CSI_H264_*`）と `CONFIG_*` 既定（既定は MJPEG のまま、または実行時設定で切替）を明確化。
2. **エンコーダモジュール作成（Antigravity: `components/csi_capture` / `components/csi_video_source`）**:
   - `csi_h264` ラッパー（PPA `RGB888 -> YUV420` + `esp_h264_enc_hw_new`、`1080p` 時の `1920x1088` バッファ確保・末尾ゼロパディング維持、内部にホストテスト可能な純 C 制御ロジック分離）。
   - `csi_video_source` にコーデック切替と `force_idr` スロット（ロックフリー要求フラグ＋`frame_hub_begin_publish()` 等公開失敗時の自己セット）を追加し、MJPEG と同様に `frame_hub` へ publish。
   - **重要**: `slowclient` や解像度変更時に H.264 コンテキストの再初期化（PPS/SPS 再発行）をテスト。
3. **配信・ビューア対応（Claude: `web_server` / `ws_video_core` / フロントエンド）**:
   - `WS /video` でのキーフレーム待ち制御（新規接続・`seq` 不連続検知時に `request_keyframe()`）、確定した同時接続コーデック方針の適用、WebCodecs `VideoDecoder` + `isConfigSupported()` による自動フォールバック再生 UI。
4. **実機検証（オーナー協力）**:
   - Chrome / Safari / Firefox（HTTPS 経由）での長時間再生・チャンネル切替復帰・低帯域（例: WAN / SSH トンネル相当）での体感遅延と実測ビットレートの確認（`docs/HW_VERIFY.md` 記録）。
