# P2b の下調べ: TC358743 の PLL 設定と ESP-IDF v6.0.1 の CSI / ISP / JPEG / PPA

作成: 2026-10-25（オーナー側 Claude の調査）。**ハードウェアは操作していない（机上の調査）。**

次の機会（P2b: 「CSI でフレームを 1 枚取る」）で必要になる情報を、一次情報から洗い出した。**確認済み・推測・未確認を区別して書く。**

- **[出典]** 一次情報で確認: TC358743 データシート Rev 1.0（Toshiba、2017-10-26、Wiki 掲載の 18 ページ版）、ESP-IDF v6.0.1 のソース（`components/` 配下のヘッダと実装）。
- **[参照]** Linux の `tc358743.c` を**参照のみ**した事実（式・制約。**コードは複製していない**）。ライセンス上、実装はゼロから書く。
- **[計算]** 上の事実から、この文書の作者が計算した値。
- **[推測]** 推論。**[未確認]** 確認できていない。

**重要な制約**: 公開されている TC358743 のデータシートは、ピン配置と電気特性のみで、**レジスタ表を含まない**（HW_VERIFY の E3、D 章参照）。PLL・CSI のレジスタの意味と数値の一次情報（Toshiba の機能仕様書 Rev 0.60）は、**入手できていない**。

---

## 1. 結論（先に読む部分）

| 項目 | 内容 |
|---|---|
| 目標のモード | **1080p50（UYVY 16bpp）まで**が 2 レーンの上限。**1080p60 は実質不可**（レーン上限 1Gbps に対し、必要 995.3Mbps で余裕 0.4%、行バーストでは 1036.8Mbps が必要）[計算] |
| RGB888（24bpp） | **1080p30 まで**（746.5Mbps/レーン）。1080p50 以上は 1Gbps/レーンを超える [計算]。HANDOFF の「1080p30 上限」は RGB888 前提 |
| PLL（REFCLK 27MHz） | `pll_prd = 4`（PLL 入力 6.75MHz）、`pll_fbd = レーンレート / 6.75MHz`。例: 972Mbps → `fbd = 144`、594Mbps → `fbd = 88` [参照 + 計算] |
| rev v1.3 の制約 | **CSI ブリッジの色変換は使えない**（入力と出力の色形式を同じにする）。**入力側のバイト／ハーフワード入れ替えも使えない** [出典: IDF のソース] |
| 色形式（TC358743 → JPEG） | TC358743 の出力 UYVY と、JPEG エンコーダの YUV422 入力（`YVYU`）の**並びが一致しない**。PPA（SRM）でも `YVYU` は出力できない。**解決方法は未確認**（§3-3、§3-4、§3-6） |
| 未確認の最大のもの | **CSI のタイミング・PHY のレジスタ値**（`LINEINITCNT` 等）、**TC358743 の CSI ブロックの起動手順**（PLL 有効化との順序）。機能仕様書の入手、または Linux ドライバの該当表の参照（複製はしない）が必要 |

---

## 2. TC358743 の PLL 設定（REFCLK 27MHz、CSI 2 レーン）

### 2-1. 確認済みの制約 [出典: データシート Rev 1.0]

- REFCLK: **27MHz、26MHz、または 42MHz**（3 章、`REFCLK`）。X630 の水晶は「27.000」の刻印（HW_VERIFY E3。実周波数と REFCLK への接続は未確認）。
- CSI-2 送信: **最大 4 データレーン、1 レーンあたり最大 1Gbps**（2 章）。X630 は 15 ピン FPC のため、**2 レーン**と推定（HW_VERIFY E1。ピン表は未確認）。ESP32-P4 の CSI ホストも**最大 2 レーン** [出典: `MIPI_CSI_HOST_LL_LANE_NUM_MAX = 2`、`esp_hal_cam/esp32p4/include/hal/mipi_csi_host_ll.h`]。
- HDMI 側: HDMI 1.4、最大 1080p60、HDMI クロック最大 165MHz、RGB / YCbCr444 24bpp、YCbCr422 24bpp（2 章）。
- ESP32-P4 の CSI ホスト（D-PHY）が受けられるレーンレート: **90〜1500Mbps**（50Mbps 前後の区間表、`esp_hal_cam/esp32p4/mipi_csi_periph.c`）[出典]。TC358743 の最大 1Gbps は範囲内。

### 2-2. PLL の式と制約 [参照: Linux `tc358743.c`、コードは複製しない]

- **レーンレート = `refclk / pll_prd × pll_fbd`**（ドライバのコメントと、PLL 設定関数の内容）。
- **PLL の入力（`refclk / pll_prd`）は 6〜40MHz。低いほどよい**（コメント）。ドライバは `pll_prd = refclk / 6MHz`（整数）とし、対応する REFCLK は 26/27/42MHz のみ。
- `pll_fbd = レーンレート / refclk × pll_prd`。レーンレート（`link-frequencies` の 2 倍）は **62.5Mbps〜1Gbps** の範囲のみ受け付ける。
- レジスタには `pll_prd − 1`、`pll_fbd − 1` を書き、フィールドは 9 ビット（0x01ff でマスク）。PLL の `FRS` フィールドは、`refclk / prd × fbd`（HS クロック）が 500MHz 超で 0、250MHz 超で 1、125MHz 超で 2、それ以下で 3。
- **これらのレジスタの意味・ビット位置は、データシート（レジスタ表なし）では確認できない**。実装時は、Linux ドライバの該当部分を参照して再実装する（複製しない）か、機能仕様書を入手する。**[未確認]**

### 2-3. REFCLK 27MHz の場合の候補 [計算]

`pll_prd = 4`（入力 6.75MHz、範囲内）で、レーンレートは **6.75Mbps 刻み**（`fbd` は 9 ビットに収まる）。`fbd` は、必要レートを切り上げた整数。

| 目標 | 画素クロック | 形式 | 必要レート/レーン（有効画素のみ） | 行バーストの下限 | `fbd`（切り上げ） | 設定レート | 判定 |
|---|---|---|---|---|---|---|---|
| 1080p60 | 148.5MHz | UYVY 16bpp | 995.3Mbps | 1036.8Mbps | 148 | 999.0Mbps | **実質不可**（上限 1Gbps、余裕 0.4%、バースト不可） |
| 1080p60 | 148.5MHz | RGB888 | 1493.0Mbps | 1555.2Mbps | 222 | 1498.5Mbps | **不可**（> 1Gbps） |
| **1080p50** | 148.5MHz | **UYVY 16bpp** | **829.4Mbps** | 864.0Mbps | 123 | 830.25Mbps | **可**（Pi の overlay は 972Mbps を使用 → `fbd = 144`） |
| 1080p50 | 148.5MHz | RGB888 | 1244.2Mbps | 1296.0Mbps | 185 | 1248.75Mbps | 不可（> 1Gbps） |
| 1080p30 | 74.25MHz | UYVY 16bpp | 497.7Mbps | 518.4Mbps | 74 | 499.5Mbps | 可 |
| **1080p30** | 74.25MHz | **RGB888** | **746.5Mbps** | 777.6Mbps | 111 | 749.25Mbps | **可** |
| 1080p25 | 74.25MHz | UYVY 16bpp / RGB888 | 414.7Mbps / 622.1Mbps | 432.0 / 648.0Mbps | 62 / 93 | 418.5 / 627.75Mbps | 可 |
| 720p60 | 74.25MHz | UYVY 16bpp / RGB888 | 442.4Mbps / 663.6Mbps | 460.8 / 691.2Mbps | 66 / 99 | 445.5 / 668.25Mbps | 可 |
| 720p50 | 74.25MHz | UYVY 16bpp / RGB888 | 368.6Mbps / 553.0Mbps | 384.0 / 576.0Mbps | 55 / 82 | 371.25 / 553.5Mbps | 可 |

- 「必要レート」= `水平画素 × 垂直画素 × fps × bpp ÷ 2 レーン`（有効画素のみ。ブランキング期間にデータは送らない）。CSI-2 のパケットのヘッダ・フッタ（1 行あたり 6 バイト、約 0.16%）、フレーム開始・終了、LP/HS の遷移は含まない。
- 「行バーストの下限」= 行のデータを、1 行の時間（水平ブランキングを含む）に収めるのに必要なレート（`必要レート × 総ライン数 / 有効ライン数`）。TC358743 は内部に映像 FIFO を持つ（データシートのブロック図）ため、FIFO で平滑化できればこの下限より低いレートでも成立しうるが、**FIFO の深さは未確認**。
- 1080p50 を 972Mbps（`fbd = 144`）にすると、有効画素のみの必要レートに約 17% の余裕ができる（Raspberry Pi の `tc358743-overlay.dts` は `link-frequencies = 486000000`（= 972Mbps/レーン）[参照]）。
- HANDOFF の「2 レーン 972Mbit/s、RGB888 で約 81Mpix/s」と整合する（972 × 2 ÷ 24 = 81Mpix/s）。UYVY 16bpp なら 121.5Mpix/s で、1080p50（103.7Mpix/s）まで収まる。
- **Wiki が X630 を 1080p@50 としている**（HW_VERIFY E5）のも、この計算（UYVY で 1080p50 まで）と整合する。

### 2-4. 設定の流れの案 [推測]

1. HDMI を接続せずに、PLL（REFCLK 27MHz、`prd = 4`、目標レーンレートに応じた `fbd`）と CSI の PHY タイミングを設定する。
2. EDID に、目標のモード（1080p50 以下など）だけを広告する（`tc358743_edid.c`）。
3. CSI 側（ESP32-P4）の `lane_bit_rate_mbps` を、**TC358743 のレーンレートと一致**させる（§3）。
4. 最初のモードは、**720p60 の UYVY**（445.5Mbps、余裕が大きい）か、**1080p30 の RGB888**（749.25Mbps）で、1 フレームを取る。

### 2-5. 未確認（要調査）

- CSI のタイミング・PHY レジスタ（`LINEINITCNT`、`LPTXTIMECNT`、`TCLK_HEADERCNT` 等）の値と計算式。Linux ドライバは、特定のレーンレート（594Mbps と 972Mbps）の表を持つ。**値の抽出と、再実装の根拠にするかの判断は未実施**。
- TC358743 の CSI ブロックの起動手順（PLL → CSI のリセット解除 → 開始）の順序と待ち時間。第 1 報の発見（CSI レジスタの読み出し値が、PLL・CSI を開始するまで無効）は、この手順が正しいかの確認に使える。
- CSI 出力の形式の選択（UYVY か RGB888 か）を制御するレジスタ。rev v1.3 の ESP32-P4 は色変換ができないため、**TC358743 側で出力形式を決める必要がある**。
- 映像 FIFO の深さ（レーンレートの余裕を決める）。

---

## 3. ESP-IDF v6.0.1 の CSI / ISP / JPEG / PPA [出典: ソース、`components/` 配下]

### 3-1. CSI（`esp_driver_cam`）

| 項目 | 内容 |
|---|---|
| 作成 | `esp_cam_new_csi_ctlr(const esp_cam_ctlr_csi_config_t *, esp_cam_ctlr_handle_t *)`（`csi/include/esp_cam_ctlr_csi.h`） |
| 設定 | `ctlr_id`、`clk_src`、`h_res`、`v_res`、`data_lane_num`（**最大 2**）、`lane_bit_rate_mbps`（**TC358743 のレーンレートと一致させる。D-PHY の対応範囲は 90〜1500Mbps**）、`input_data_color_type`、`output_data_color_type`、`data_type`（独自のデータタイプ）、`queue_items`、フラグ（`input_8bit_swap_en`、`input_16bit_swap_en`、`byte_swap_en`、`bk_buffer_dis`） |
| 操作 | `esp_cam_ctlr_enable` → `esp_cam_ctlr_start` → `esp_cam_ctlr_receive(handle, trans, timeout_ms)`（トランザクション = バッファ + 長さ）、`esp_cam_ctlr_stop`、`esp_cam_ctlr_disable`、`esp_cam_ctlr_del` |
| コールバック | `esp_cam_ctlr_register_event_callbacks`: `on_get_new_trans`（次のバッファを渡す）、`on_trans_finished`（1 フレーム受信完了） |
| バッファ | `esp_cam_ctlr_alloc_buffer(handle, size, buf_caps)`（`MALLOC_CAP_SPIRAM \| MALLOC_CAP_DMA` または内蔵 RAM）。**ドライバ内部のバックアップバッファ**（フレーム全体）は、既定で **PSRAM に確保される**（`bk_buffer_dis = 1` で無効化）。`esp_cam_ctlr_get_frame_buffer` で内部バッファのアドレスを取得できる |
| サイズの制約 | `h_res × v_res × 出力の bpp` が **8 ビットの倍数**であること |

**rev v1.3（`CONFIG_ESP_REV_MIN_FULL < 300`）の制約** [出典: `esp_cam_ctlr_csi.c`]:
- **CSI ブリッジの色形式変換は使えない**: `input_data_color_type` と `output_data_color_type` が**同じ**（バイパス）でなければ `ESP_ERR_NOT_SUPPORTED`（rev v3.0 以上のみ対応）。
- **`input_8bit_swap_en` と `input_16bit_swap_en` は使えない**（`ESP_ERR_NOT_SUPPORTED`）。**`byte_swap_en`（出力側）は使える**。
- したがって、TC358743 の出力形式（UYVY か RGB888）が、そのまま CSI の出力形式になる。

### 3-2. ISP（`esp_driver_isp`）

- `esp_isp_new_processor(const esp_isp_processor_cfg_t *, isp_proc_handle_t *)`、`esp_isp_enable`。設定: `clk_src`、`clk_hz`、`input_data_source`（CSI など）、`input_data_color_type`、`output_data_color_type`、`yuv_range`、`yuv_std`、`has_line_start_packet`、`has_line_end_packet`、`h_res`、`v_res`、`bayer_order`、フラグ（**`bypass_isp`**、`byte_swap_en`）。
- ISP は、RAW 入力のカメラ向け（デモザイク、AWB、AE 等）。TC358743 は YUV422 / RGB888 を出力するため、**ISP の処理は通常不要**と推測される。**ISP が CSI の受信経路で必須か、`bypass_isp` で足りるか**は、**未確認**（CSI ブリッジは ISP と共有されるという記述がある: `SOC_ISP_SHARE_CSI_BRG`）。

### 3-3. JPEG エンコーダ（`esp_driver_jpeg`）

- `jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *, jpeg_encoder_handle_t *)`、`jpeg_encoder_process(engine, const jpeg_encode_cfg_t *, in_buf, in_size, out_buf, out_size, &out_len)`。設定: `width`、`height`、`src_type`、`sub_sample`、`image_quality`（1〜100）、`pixel_reverse`（入力の画素順の反転。詳細は TRM を参照とだけある）。
- **入力形式（`jpeg_enc_input_format_t`）**: `RGB888`（`BGR24`）、`RGB565`、`GRAY`、**`YUV422`（`YVYU`）**。`YUV444` と `YUV420` は、**rev v3.0 未満（`CONFIG_ESP_REV_MIN_FULL < 300`）の P4 では選択できない**（ヘッダで非公開）。
- **TC358743 の UYVY（バイト順 U0 Y0 V0 Y1）と、JPEG の `YVYU`（Y0 V0 Y1 U0）は並びが違う**。`pixel_reverse` で足りるか、PPA か CSI の `byte_swap_en`（出力）で並べ替えるか、RGB に変換してから JPEG に入れるかは、**未確認**（TRM が必要）。

### 3-4. PPA（`esp_driver_ppa`）

- 操作: SRM（拡大縮小・回転・反転）、ブレンド、塗りつぶし。`ppa_register_client`、SRM の設定は `ppa_srm_oper_config_t`（入出力の `srm_cm`、`yuv_range`、`yuv_std`、`rgb_swap`、`byte_swap`）。
- SRM の色形式: 入出力とも `ARGB8888`、`RGB888`、`RGB565`、`YUV420`、`YUV444`（限定的）、`YUV422_UYVY`、`GRAY8`。**`YUV422_VYUY`、`YUV422_YUYV`、`YUV422_YVYU` は入力のみ**。
- したがって、**PPA で UYVY → RGB888 / RGB565 への変換はできる**が、**`YVYU` には出力できない**。RGB に変換して JPEG に入れる経路は、メモリ帯域と時間が増える（コスト未測定）。

### 3-5. H.264

- ESP-IDF 本体（`components/`）には、H.264 エンコーダの API が**見当たらない**（コンポーネントレジストリの `esp_h264` 等になる見込みだが、**この調査では到達できず、未確認**）。HANDOFF の「rev v1.3 では H.264 が遅い」とも合わせて、MJPEG を主とする方針は妥当。

### 3-6. 経路の案 [推測]

| 経路 | 内容 | 懸念 |
|---|---|---|
| **A** | TC358743（UYVY）→ CSI（出力 UYVY）→ PSRAM のフレームバッファ → **PPA で RGB565 / RGB888 に変換** → JPEG → WebSocket | PPA の 1 回分の帯域と時間、バッファの増加 |
| **B** | TC358743（UYVY）→ CSI → PSRAM → **JPEG に直接入力**（`YUV422`、`pixel_reverse` や `byte_swap_en` で並びを合わせる） | 並びが合うか未確認 |
| **C** | TC358743（RGB888）→ CSI（出力 RGB888）→ PSRAM → JPEG（`RGB888`） | レーンレートの制約で **1080p30 まで**。帯域が大きい |

最初の 1 フレームの取得（P2b）では、**経路 C か A の 720p / 1080p30 から**始めるのが、確認項目が少ない。

---

## 4. 次の調査・実験の優先順位（提案）

1. **機能仕様書 Rev 0.60 の入手可否**（Toshiba / Wiki の別資料）。入手できなければ、Linux ドライバの表を**参照のみ**で読み、再実装の根拠にするかを決める。
2. PLL と CSI の起動手順の実装（コード）と、**CSI ステータスが実値になるか**の実機確認（第 1 報の仮説の検証にもなる）。
3. 720p60 UYVY（またはほかの低いモード）で、`esp_cam_ctlr_receive` による 1 フレームの受信確認。
4. JPEG の入力の並び（`YVYU` と UYVY）の解決。TRM の確認と、実機でのテスト画像による確認。
5. ISP を経由するかどうかの確認（`bypass_isp`）。

## 5. 出典

- TC358743XBG データシート Rev 1.0（2017-10-26、Wiki の File ページ）。
- ESP-IDF v6.0.1 のソース: `components/esp_driver_cam/`（`csi/include/esp_cam_ctlr_csi.h`、`csi/src/esp_cam_ctlr_csi.c`、`include/esp_cam_ctlr.h`、`include/esp_cam_ctlr_types.h`）、`esp_hal_cam/`（`esp32p4/mipi_csi_periph.c`、`esp32p4/include/hal/mipi_csi_host_ll.h`）、`esp_driver_isp/include/driver/isp_core.h`、`esp_driver_jpeg/include/driver/`（`jpeg_encode.h`、`jpeg_types.h`）、`esp_driver_ppa/include/driver/ppa.h`、`esp_hal_ppa/include/hal/ppa_types.h`。
- Linux の `drivers/media/i2c/tc358743.c`（参照のみ。式と制約の事実）、Raspberry Pi の `tc358743-overlay.dts`（`clock-frequency = <27000000>`、`link-frequencies = /bits/ 64 <486000000>`、2 レーン）。
- HW_VERIFY: E1（レーン数）、E3（水晶、チップ刻印）、E5（最大モード）、D4（PSRAM）。
