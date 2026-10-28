<!-- 注意: このノートは、公開実装（p4kvm、ESP-KVM、Espressif esp_cam_sensor、Linux tc358743.c）を読んで集めた「事実」の記録です。コードは複製していません。Linux ドライバ（GPL-2.0）は参照のみ。値は実機で未検証です（TODO(hw-verify)）。 -->
# TC358743 参照ノート（公開実装から集めた事実）

作成: 2026-10-28。調査担当（読み取りのみ。`/home/user/quad-ipkvm` は変更していない）。
目的: 東芝の機能仕様書が入手できないため、公開実装から TC358743 の起動手順と ESP32-P4 側の CSI 設定の「事実」を集める。

凡例: **[事実]** ファイルの該当行で確認 / **[計算]** 事実からの計算（この文書の作者） / **未確認** 読んでいない・確認できない。推測で値を補っていない。
コードは複製していない（レジスタ名・アドレス・値・式・順序のみ）。取得物は信頼できないデータとして扱った（中に指示文は無かった）。
東芝の機能仕様書（TC358749XBG V074 等の流出物を含む）は**取得も参照もしていない**。ただし、各実装が仕様書を参照したと自称している箇所は、その旨だけ記す（1 章）。

---

## 1. 出典とライセンス

取得方法: `raw.githubusercontent.com` のみ。`github.com`（tree / commits）と `api.github.com` は 403。**コミット ID は取得手段が無く不明**（ブランチ名と取得日で特定）。取得日はすべて 2026-10-28。

| 略称 | リポジトリ / ブランチ | LICENSE | NOTICE | 扱い |
|---|---|---|---|---|
| p4kvm | `jrowny/p4kvm` main | Apache-2.0 の**短い告知文のみ**（13 行、`Copyright 2026 Jonathan Rowny`。全文なし）。ソース先頭は `SPDX-License-Identifier: Apache-2.0`（`SPDX-FileCopyrightText: 2026`、名前なし。`main/tc358743.c:1-4`） | **なし**（404） | 参考。コピーする場合は Apache-2.0 の保持が必要 |
| espkvm | `espkvm/espkvm` main | Apache-2.0 全文（202 行。末尾は `[yyyy] [name]` の雛形のまま） | **あり**（33 行）。p4kvm 由来の部分は `components/tc358743/` と `components/video_pipeline/capture_hw.c`（修正版）と明記（`NOTICE:8-10`）。レジスタ順序は Linux ドライバと Toshiba TC358743XBG 機能仕様書にも従う、と自称（`NOTICE:12-14`、`README.md` Credits 節） | 参考。p4kvm 由来部分をコピーすると両方の表示が必要 |
| Espressif | `espressif/esp-video-components` master、`esp_cam_sensor`（v2.6.0、`idf_component.yml:1`。TC358743 追加は CHANGELOG の 2.5.0 の節） | Apache-2.0（`esp_cam_sensor/LICENSE`、ルート `README.md` の表） | ルートに無し（404） | 参考。`tc358743.c:9-11` が REF_01 に「Toshiba 機能仕様書 Rev 1.1」、REF_03 に p4kvm を挙げる（**4 つ目の公開実装。指示書の対象外だが追加で読んだ**） |
| Linux | `torvalds/linux` master: `drivers/media/i2c/tc358743.c`、`tc358743_regs.h`、`include/media/i2c/tc358743.h` | **GPL-2.0-only**（SPDX 行 1） | — | **参照のみ。コード複製禁止** |
| Linux DT binding | 同 `Documentation/devicetree/bindings/media/i2c/toshiba,tc358743.yaml` | `GPL-2.0-only OR BSD-2-Clause`（1 行目） | — | 参照のみ |
| RPi overlay | `raspberrypi/linux` rpi-6.6.y: `arch/arm/boot/dts/overlays/tc358743-overlay.dts`、`drivers/media/i2c/tc358743.c`（照合用） | GPL-2.0-only（dts 1 行目） | — | **参照のみ** |
| ESP-IDF | `espressif/esp-idf` v6.0.1 と master（`esp_driver_cam/csi/`、`esp_hal_cam/`、`esp_hw_support/ldo/`） | Apache-2.0（ヘッダの SPDX） | — | API・既定動作の確認用 |

注意（法的判断はしない）: p4kvm のヘッダは「Register sequence from drivers/media/i2c/tc358743.c in the Linux kernel」（`main/tc358743.h:6`）とし、自身は Apache-2.0 を名乗る。CLAUDE.md の方針（ゼロから書く）のとおり、本プロジェクトでは値・順序（事実）だけを使い、コードは書き下ろす。

---

## 2. 起動手順

### 2-1. I2C のアドレスとアクセス方法

| 項目 | 内容 | 出典 |
|---|---|---|
| 7 ビットアドレス | **0x0F**（8 ビット表記 0x1E） | p4kvm `main/tc358743.h:19-20`; espkvm `include/tc358743.h:24`; Espressif `include/tc358743.h:15`; RPi overlay `tc358743-overlay.dts:16,18`; Linux binding yaml の例 `reg = <0x0f>` |
| レジスタアドレス | **16 ビット、ビッグエンディアン**（上位バイトを先に送る） | p4kvm `tc358743.c:288-301`; Linux `tc358743.c:160-212,131-158` |
| データ | **リトルエンディアン**、幅はレジスタごとに 8 / 16 / 32 ビット（1 / 2 / 4 バイトを連続書き込み） | p4kvm `tc358743.c:303-333`; Linux `tc358743.c:227-237`（`cpu_to_le32`） |
| 読み出し | アドレス 2 バイトを書いてから再スタートで読む | p4kvm `tc358743.c:297-301` |
| 一括書き込み | EDID は 128 バイト単位（アドレス 2 + 128 = 130 バイト）。Linux の上限 `I2C_MAX_XFER_SIZE = 130` | Linux `tc358743.c:66,168-172`; p4kvm `tc358743.c:620-622` |
| バス速度 | 100 kHz | p4kvm `tc358743.c:640`; espkvm `tc358743.c:757` |
| タイムアウト | p4kvm は無期限（`-1`）。espkvm は 200 ms に変更（「`-1` では 1 回の詰まりでロックが永久に残った」） | p4kvm `tc358743.c:294`; espkvm `tc358743.c:32-34` |
| チップ ID 確認 | `CHIPID`(0x0000) を 16 ビット読み、**上位 8 ビットが 0** ならば TC358743（Linux・espkvm・Espressif は確認する。p4kvm は読むだけで判定しない） | Linux `tc358743.c:2212-2217`; espkvm `tc358743.c:777-804`; Espressif `tc358743.c:1110-1116`; p4kvm `tc358743.c:661-667` |

### 2-2. 順序（p4kvm の実行順。espkvm は同じで、差は括弧内）

| # | 処理 | 内容（待ち時間） | 出典 |
|---|---|---|---|
| 0 | （P4 側）MIPI D-PHY の LDO | `esp_ldo_acquire_channel`（chan 3、2500 mV）。**リセットより前** | p4kvm `capture_hw.c:191-196`; espkvm `capture_hw.c:582-587` |
| 1 | ハードウェアリセット | RESETN（GPIO 23）を Low 10 ms → High → **200 ms 待つ**。未配線なら 500 ms 待つ（espkvm は極性を選べる） | p4kvm `capture_hw.c:45-66`; espkvm `capture_hw.c:128-165`; Kconfig 既定 GPIO 23（p4kvm `Kconfig.projbuild:5-8`、espkvm `main/Kconfig.projbuild:163-166`） |
| 2 | I2C バス作成・デバイス追加 | I2C_NUM_0、SDA=GPIO 7、SCL=GPIO 8、内部プルアップ有効、glitch_ignore_cnt=7（espkvm はここで CHIPID を最大 3 回、5 ms 間隔で確認） | p4kvm `capture_hw.c:200-212`、`p4kvm_hw_defaults.h:9-10`; espkvm `capture_hw.c:75-91`、`tc358743.c:777-804` |
| 3a | HPD を Low | `HPD_CTL.HPD_OUT0 = 0`、**20 ms** | p4kvm `tc358743.c:681-682` |
| 3b | `initial_setup` | `SYSCTL` の IRRST/CECRST を立てる → `SYSCTL` の CTXRST/HDMIRST を立てて即戻す（ブロックリセット）→ SLEEP=0 → `FIFOCTL`=374 → 基準クロック設定 → `DDC_CTL.DDC5V_MODE`=2 → `EDID_MODE`=E-DDC(2) → HDMI RX PHY 設定 → HDCP 無効 → 音声設定 → InfoFrame 設定 → `VI_MODE.RGB_DVI` クリア → `VOUT_SET2.VOUTCOLORMODE`=AUTO(1) → `VOUT_SET3`=EXTCNT(0x08) | p4kvm `tc358743.c:496-517`; espkvm `tc358743.c:582-606`; Linux `tc358743.c:937-974` |
| 3c | EDID 書き込み | `EDID_LEN1/2`=256（0x00 / 0x01）→ `EDID_RAM`(0x8C00) に 128 バイト × 2 → **10 ms**。**HPD は Low のまま**（「HPD を上げるのは PLL/CSI の後」とコメント） | p4kvm `tc358743.c:611-624` |
| 3d | ストリーム停止 | `VI_MUTE`=AUTO_MUTE\|VI_MUTE（0xD0）、`CONFCTL.VBUFEN/ABUFEN` クリア | p4kvm `tc358743.c:366-376` |
| 3e | PLL | SLEEP=1 → `PLLCTL0` → `PLLCTL1`(FRS\|RESETB\|PLL_EN) → **1 ms**（Linux は `udelay(10)`、コメント「REF_02」） → `PLLCTL1.CKEN` → SLEEP=0。`PLLCTL0` と PLL_EN が既に同じなら書かない | p4kvm `tc358743.c:407-435`; Linux `tc358743.c:668-705` |
| 3f | CSI 送信側 | `SYSCTL.CTXRST` ブロックリセット → 使わないレーンを `D?W_CNTRL` で LANEDISABLE → タイミング 9 レジスタ → `HSTXVREGEN` → `TXOPTIONCNTRL`=0 → `STARTCNTRL`=1 → `CSI_START`=1 → `CSI_CONFW` を 4 回 | p4kvm `tc358743.c:554-604`; Linux `tc358743.c:803-865` |
| 3g | 出力色形式 | 既定は RGB888（3 章参照）。UYVY は後から切り替え | p4kvm `tc358743.c:519-534,694-695` |
| 3h | 割り込み | `INTSTATUS`=0xFFFF、`INTMASK`=0xFCFF（HDMI と CSI のみ許可） | p4kvm `tc358743.c:697-698` |
| 4 | （P4 側、p4kvm）バッファ確保 → `esp_cam_new_csi_ctlr` → コールバック登録 → `esp_cam_ctlr_enable` → ISP プロセッサ作成 → `ISP.cntl.isp_en=0` → MIPI_CSI_BRIDGE 設定。**HDMI 出力を有効にする前**に行う | p4kvm `capture_hw.c:215-285` |
| 5 | HDMI 出力有効化 | `enable_stream(true)`（`VI_MUTE`=AUTO_MUTE(0xC0)、`CONFCTL.VBUFEN/ABUFEN` セット）→ **150 ms** → `HPD_OUT0`=1 → **50 ms** → `CSI_START`=1 を**再書き込み**（「CONFCTL が映像を有効にした後で書き直さないと CSI TX が idle のままの基板がある」とコメント） | p4kvm `tc358743.c:705-717,366-376` |
| 6 | HDMI ロック待ち | `SYS_STATUS`(0x8520) を 50 ms 刻みで最大 **5000 ms**。TMDS(0x02) と SYNC(0x80) の両方が立つまで。**タイムアウトしても続行** | p4kvm `capture_hw.c:68-82,287` |
| 7 | CSI 受信開始 | MIPI_CSI_BRIDGE を再設定 → `esp_cam_ctlr_start` | p4kvm `capture_hw.c:289-290` |

**espkvm の差**: 4 と 7 の順序が逆。ロック待ち（6）の**後**で、実測した解像度（`DE_WIDTH`/`V_SIZE` 等）で `esp_cam_new_csi_ctlr` を作る。源が範囲外（320x200 未満、1920x1080 超）なら 1920x1080 で作って後で拒否する（`capture_hw.c:703-745`）。ストリーム OFF→ON の `TXOPTIONCNTRL` 書き込み等は同じ（`tc358743.c:842-883`）。

**復旧手順（映像が 2 秒止まった時）**: p4kvm: `esp_cam_ctlr_stop` → `enable_stream(false)` → HPD Low → **150 ms** → `enable_stream(true)` → 150 ms → HPD High → 50 ms → `CSI_START` → ロック待ち（最大 5 s）→ 色空間再設定 → `set_csi` を再実行 → `CSI_START` → ブリッジ再設定 → `esp_cam_ctlr_start`。連続復旧は 8 秒空ける（`capture_hw.c:305-348`、`tc358743.c:719-739`、`capture_mjpeg.c:66-85`）。espkvm は `esp_cam_ctlr` を作り直す（`capture_hw.c:833-884`）。待ち 2000 ms・復旧間隔 8 s は `capture_loop.c:382,408-410`。

### 2-3. Linux ドライバの順序（参照のみ。p4kvm と異なる点を太字）

1. リセット GPIO: 5〜10 ms 待つ → アサート 1〜2 ms → 解除 → **20 ms**（`tc358743.c:2000-2007`）。
2. CHIPID 確認 → `initial_setup` → `s_dv_timings` 相当（`enable_stream(false)` → `set_pll` → `set_csi`）→ 色空間 → 割り込みの初期化（`tc358743.c:2212-2275`）。
3. **EDID は上記の後、ユーザー空間の `s_edid` で書く**: HPD Low → `EDID_LEN` → `EDID_RAM` 書き込み → 5 V（DDC5V）があれば **143 ms 後に HPD High**（`tc358743.c:429-460,1881-1933`）。
4. ストリーム ON は `enable_stream(true)`。**この中で `TXOPTIONCNTRL` を 0 → `CONTCLKMODE`(1) の順に書く**（LP11→HS 遷移を作るため、コメントあり。`tc358743.c:640-666`）。
5. 解像度変更時: SYNC 変化割り込みで、信号が無ければ **PHY リセット**（`PHY_RST` を 0→1）と BKSV 消去 → `format_change` が `enable_stream(false)`。ユーザー空間が `s_dv_timings`（`enable_stream(false)` → `set_pll` → `set_csi`）→ `enable_stream(true)` を呼ぶ（`tc358743.c:618-624,1202-1215,1109-1135,1658-1682`）。

Espressif `esp_cam_sensor` の順序は p4kvm と同一（`init_streaming` → `enable_hdmi_output` → ロック待ち最大 6000 ms。`tc358743.c:537-587,909-934`、`TC358743_HDMI_LOCK_WAIT_MS` は `:65`）。

---

## 3. レジスタ設定値の表

特に断りがなければ p4kvm / espkvm / Espressif は同値。値は 16 進。「レーン」は設定したレーンレート。

### 3-1. PLL（基準クロック 27 MHz）

| レジスタ | 値・式 | レーン | 出典 |
|---|---|---|---|
| `pll_prd` | `refclk / 6 MHz`（整数）= **4**。PLL 入力 6.75 MHz。Linux は 26 / 27 / 42 MHz のみ許可 | 全て | p4kvm `tc358743.c:268`; Linux `tc358743.c:717-719,2073-2087` |
| `pll_fbd` | `bps_per_lane / refclk × pll_prd`（**整数除算**。27 MHz の倍数でないレートは切り捨て）。972 → **144**、594 → **88**、648 → 96 | — | p4kvm `tc358743.c:270-271`; Linux `tc358743.c:2101-2103` |
| レーンレート | `refclk / pll_prd × pll_fbd`。Linux は 62.5 Mbps 〜 1 Gbps のみ受け付ける | — | Linux `tc358743.c:798,2089-2099` |
| `PLLCTL0`(0x0020) | `((prd-1) << 12) \| ((fbd-1) & 0x1FF)`。972 → **0x308F** [計算]、594 → 0x3057 [計算] | 972 / 594 | p4kvm `tc358743.c:41-45` |
| `PLLCTL1`(0x0022) の `FRS`(bit 11:10) | HS クロック（= 上式のレーンレート）が **> 500 MHz で 0、> 250 MHz で 1、> 125 MHz で 2、それ以下で 3**。972・594・648 はすべて 0。1 → 445.5 Mbps など 500 未満で初めて非 0（公開実装で**実際に使われた例は無い**） | — | p4kvm `tc358743.c:415-425`; Linux `tc358743.c:682-693` |
| `PLLCTL1` その他 | `RESETB`(0x02) と `PLL_EN`(0x01) を FRS と同時に、少し待ってから `CKEN`(0x10) | — | p4kvm `tc358743.c:427-433` |

### 3-2. CSI 送信タイミング（レーンレート別）

**Linux の表は 594 Mbps と 972 Mbps のみ**（コメント: Toshiba のスプレッドシート REF_02 由来。「原理的には周波数と解像度から計算できるはず」の FIXME。**計算式は無い**。`tc358743.c:2105-2110`）。表に無いレートでは **警告を出して 594 Mbps の値を使う**（`default:` が 594 に落ちる。`tc358743.c:2111-2115`）。RPi 版の同ドライバも同じ値（`rpi-6.6.y` `tc358743.c:1986-2007`）。

| レジスタ | 972 Mbps | 594 Mbps | 出典（行） |
|---|---|---|---|
| `LINEINITCNT`(0x0210) | **0x1B58** | 0x0E80 | Linux 2116 / 2129 |
| `LPTXTIMECNT`(0x0214) | **0x0007** | 0x0003 | 2117 / 2130 |
| `TCLK_HEADERCNT`(0x0218) | **0x2806**（Linux コメント: tclk-preparecnt 6、zerocnt 40） | 0x1403（prepare 3、zero 20） | 2119 / 2132 |
| `TCLK_TRAILCNT`(0x021C) | **0x00** | 0x00 | 2120 / 2133 |
| `THS_HEADERCNT`(0x0220) | **0x0806**（ths-prepare 6、zero 8） | 0x0103（3、1） | 2122 / 2135 |
| `TWAKEUP`(0x0224) | **0x4268** | 0x4882 | 2123 / 2136 |
| `TCLK_POSTCNT`(0x0228) | **0x0008** | 0x0008 | 2124 / 2137 |
| `THS_TRAILCNT`(0x022C) | **0x0005** | 0x0002 | 2125 / 2138 |
| `HSTXVREGCNT`(0x0230) | **0** | 0 | 2126 / 2139 |

- **p4kvm・espkvm が動かしているレーンレート: 972 Mbps、2 レーン**（`P4KVM_MIPI_LANE_MBPS 972`、`p4kvm_hw_defaults.h:17`。espkvm の Kconfig 既定 972、範囲 100〜1500、`main/Kconfig.projbuild:202-209`）。値は 972 の列と完全に一致（p4kvm `tc358743.c:272-285`）。
- RPi overlay: `link-frequencies = 486000000`（= 972 Mbps/レーン）、2 レーン、`clock-noncontinuous`、基準 27 MHz（`tc358743-overlay.dts:25-30,53,82`）。Linux のコメント: 「594 Mbps は 4 レーン 1080p60 または 2 レーン 720p60 の既定、**972 Mbps は 2 レーンの 1080p50 UYVY を可能にする**」（`tc358743.c:2089-2093`）。
- **Espressif `esp_cam_sensor`**: RGB888 1080p30 は 972 Mbps。**YUV422 1080p30 は 648 Mbps**（`pll_fbd`=96 [計算]）だが、タイミング 9 レジスタは**上の 972 の値のまま**（`tc358743.c:63-64,794-818`）。この組み合わせが動くかの記述は無い（**未確認**）。
- 他のレーンレート（445.5、749.25 など）で動かした公開実装は**見つからなかった**。

### 3-3. CSI 送信の制御

| レジスタ | 値 | 備考 | 出典 |
|---|---|---|---|
| `CLW_CNTRL`(0x0140) / `D0W_CNTRL`(0x0144) / `D1W_CNTRL`(0x0148) / `D2W_CNTRL`(0x014C) / `D3W_CNTRL`(0x0150) | レーン数 N に対し、**使わないレーンに LANEDISABLE(0x01) を書く**（2 レーンなら D2W と D3W） | クロック・D0 は `lanes < 1` のときだけ | p4kvm `tc358743.c:560-574` |
| `HSTXVREGEN`(0x0234) | 使うレーンの bit を立てる: CLM(0x01) + D0M(0x02) + D1M(0x04)… 2 レーン = **0x07** [計算] | | p4kvm `tc358743.c:586-589` |
| `TXOPTIONCNTRL`(0x0238) | **0**（非連続クロック）を `set_csi` で書く。`enable_stream` では**書かない** | **Linux との差**（9 章） | p4kvm `tc358743.c:591-592`; Linux `tc358743.c:651-653,843-844` |
| `STARTCNTRL`(0x0204) | 1（START） | | p4kvm `tc358743.c:593` |
| `CSI_START`(0x0518) | 1（STRT）。`set_csi` で 1 回、`enable_hdmi_output` の最後で**もう 1 回** | 2 回目は Linux に無い | p4kvm `tc358743.c:594,714` |
| `CSI_CONFW`(0x0500) 1 回目 | MODE_SET(0xA0000000) \| ADDRESS_CSI_CONTROL(0x03000000) \| CSI_MODE(0x8000) \| TXHSMD(0x0080) \| NOL | **2 レーン = 0xA3008082** [計算]。NOL: 1L=0, 2L=0x02, 3L=0x04, 4L=0x06。`CSI_CONTROL`(0x040C) への間接書き込み | p4kvm `tc358743.c:596-598` |
| 〃 2 回目 | MODE_SET \| ADDRESS_CSI_ERR_INTENA(0x14000000) \| TXBRK(0x02) \| QUNK(0x10) \| WCER(0x100) \| INER(0x200) = 0xB4000312 [計算] | エラー割り込み許可 | 599 |
| 〃 3 回目 | MODE_CLEAR(0xC0000000) \| ADDRESS_CSI_ERR_HALT(0x15000000) \| TXBRK \| QUNK = 0xD5000012 [計算] | | 601 |
| 〃 4 回目 | MODE_SET \| ADDRESS_CSI_INT_ENA(0x06000000) \| INTER(0x04) = 0xA6000004 [計算] | | 603 |

**`CSI_CONFW` は色形式のレジスタではない**。`CSI_CONTROL` / `CSI_ERR_INTENA` / `CSI_ERR_HALT` / `CSI_INT_ENA` への書き込み口で、レーン数は NOL に入る（Linux `tc358743_regs.h:137-175`）。

### 3-4. 出力色形式（VI_MODE / VOUT 系）

| 設定 | RGB888（DT 0x24、24bpp） | UYVY 4:2:2（DT 0x1E、16bpp） | 出典 |
|---|---|---|---|
| `VOUT_SET2`(0x8573) の `SEL422`(0x80) と `VOUT_422FIL_100`(0x40) | 両方**クリア** | 両方**セット** | p4kvm `tc358743.c:519-534` |
| `VI_REP`(0x8576) の `COLOR_SEL`(0xE0) | **0x00**（RGB フルレンジ） | **0x60**（BT.601 YCbCr リミテッド）。Espressif は **0x40**（BT.601 フル）（9 章） | p4kvm 〃; Espressif `tc358743.c:425` |
| `CONFCTL`(0x0004) の `YCBCRFMT`(0x00C0) | **0**（4:4:4） | **0xC0**（4:2:2 8 ビット）。値 0x80 は「COLORBAR」と Linux のヘッダに定義（`tc358743_regs.h:43`）。どの実装も使っていない | 〃 |
| 共通 | `VI_MODE.RGB_DVI`(0x08) クリア、`VOUT_SET2.VOUTCOLORMODE`(0x03)=AUTO(1)、`VOUT_SET3`=EXTCNT(0x08) | | `tc358743.c:514-516` |

- UYVY を選ぶ関数は `tc358743_set_csi_uyvy422()`。p4kvm では既定 RGB888 で、UYVY は呼ばれない（`tc358743.c:545-552`）。
- 出力形式の選択は `set_csi` の**後**に別途行う（Linux も `set_csi` → `set_csi_color_space` の順。`tc358743.c:1842-1845`）。

### 3-5. FIFO

| 項目 | 値 | 出典 |
|---|---|---|
| `FIFOCTL`(0x0006) | **374**（32 ビットワード単位）。Linux のコメント: 「16 は 594 Mbps で 720p60（2 レーン）・1080p60（4 レーン）に使えるが、レートを上げる・画素クロックを下げると CSI の読み出しがデータ到着より速くなって失敗する。374 は 594 のその 2 モードと、972 のほとんどのモードで動く」。Toshiba の計算式は「スプレッドシートの中」で使えない、とも | Linux `tc358743.c:2060-2072`; p4kvm `tc358743.c:272` |
| espkvm の動的調整 | 1 行のワード数（`hact × bpp / 32`）が 374 以下なら `× 7/8`。入力の 1 行が CSI の送出より速く来るなら **32** にする。720x480 UYVY は 360 ワードで、既定 374 のままでは**フレームが全く来なかった**。1080p50 で 374 のままだと FIFO があふれ、各行に右側の画素が混ざった（Function EV、2026-10-24、rev 3.x） | espkvm `tc358743.c:924-967`、`docs/HARDWARE-NOTES.md:66-88` |
| FIFO 容量 | **未確認**（どの資料にも無い） | — |

### 3-6. HPD / EDID / DDC / ストリーム制御

| レジスタ | 値 | 出典 |
|---|---|---|
| `HPD_CTL`(0x8544) の `HPD_OUT0`(0x01) | 初期化の最初に 0、`enable_hdmi_output` で 1 | p4kvm `tc358743.c:606-609` |
| `DDC_CTL`(0x8543) の `DDC5V_MODE`(0x03) | **0x02**（100 ms。Linux の `DDC5V_DELAY_100_MS` はコード上 2、`include/media/i2c/tc358743.h:17-21`、`tc358743.c:2058`） | p4kvm `tc358743.c:274,506` |
| `EDID_MODE`(0x85C7) の `EDID_MODE`(0x03) | **0x02**（E-DDC） | p4kvm `tc358743.c:507` |
| `EDID_LEN1/2`(0x85CA/CB)、`EDID_RAM`(0x8C00) | 256 バイト（2 ブロック） | p4kvm `tc358743.c:617-622` |
| `VI_MUTE`(0x857F) | ON: `AUTO_MUTE`(0xC0) \| `VI_MUTE`(0x10) = 0xD0、OFF: 0xC0 | p4kvm `tc358743.c:366-376` |
| `CONFCTL` の `VBUFEN`(0x01) / `ABUFEN`(0x02) | ストリーム ON でセット | 〃 |
| `SYS_STATUS`(0x8520) のビット | DDC5V=0x01、**TMDS=0x02**、PHY_PLL=0x04、PHY_SCDT=0x08、HDMI=0x10、HDCP=0x20、AVMUTE=0x40、**SYNC=0x80** | Linux `tc358743_regs.h:390-398`; espkvm `include/tc358743.h:131-135` |
| `HDCP_MODE`(0x8560) の `MANUAL_AUTHENTICATION`(0x02) | セット（HDCP を使わない） | p4kvm `tc358743.c:442` |

### 3-7. HDMI RX PHY・基準クロック依存の値（27 MHz の場合）

| レジスタ | 値 | 出典 |
|---|---|---|
| `PHY_EN`(0x8534) | 設定中は 0、設定後に 1 | p4kvm `tc358743.c:449,462` |
| `PHY_CTL1`(0x8532) | `((1600/200) << 4) \| (1-1)` = **0x80** [計算] | 450 |
| `PHY_CTL2`(0x8533) の AUTO_RST 2〜4 | 既定の cfg ではすべて 0（クリア） | 451-454 |
| `PHY_BIAS`(0x8536) | 0x40 | 455 |
| `PHY_CSQ`(0x853F) | 0x0A | 456 |
| `AVM_CTL`(0x8546) | 45 | 457 |
| `HDMI_DET`(0x8552) の `DET_V`(0x30) | `hdmi_detection_delay << 4` = 0 | 458 |
| `HV_RST`(0x85AF) | p4kvm/espkvm/Espressif は 0x01 / 0x02 をクリア。Linux は 0x20 / 0x10（9 章）。既定ではどのフラグも 0 | p4kvm `tc358743.c:166-168,459-461`; Linux `tc358743_regs.h:567-569` |
| `SYS_FREQ0/1`(0x8540/41) | `refclk/10000` = 2700 = 0x0A8C → 0x8C / 0x0A [計算] | 381-383 |
| `PHY_CTL0`(0x8531) `PHY_SYSCLK_IND` | 42 MHz のときだけセット。27 MHz では 0 | 385 |
| `FH_MIN0/1`(0x85AA/AB) | `refclk/100000` = 270 (0x010E) [計算] | 387-389 |
| `FH_MAX0/1`(0x85AC/AD) | `fh_min × 66 / 10` = 1782 (0x06F6) [計算] | 391-393 |
| `LOCKDET_REF0/1/2`(0x8630〜32) | `refclk/100` = 270000 (0x41EB0) → 0xB0 / 0x1E / 0x04 [計算] | 395-398 |
| `NCO_F0_MOD`(0x8670) の下位 2 ビット | 27 MHz のとき **0x01** | 400 |
| `CECHCLK/CECLCLK`(0x0028 / 0x002A) | `656 × sys_freq / 4200` = 421 (0x01A5) [計算]（Linux のコメント: 「既定値 656 は 42 MHz 用と試行錯誤で推測」） | 402-404; Linux `tc358743.c:746-753` |

音声関連（`FORCE_MUTE`、`AUTO_CMD0〜2`、`BUFINIT_START`、`FS_IMODE`、`ACR_*`、`SDO_MODE1`、`DIV_MODE`）は映像に不要。**p4kvm のアドレスは Linux と違う**（`FORCE_MUTE`=0x8602 など。espkvm は Linux に合わせて直した。`espkvm tc358743.c:255-293` のコメント、`HARDWARE-NOTES.md:441-444`）。

---

## 4. ESP32-P4 側の CSI

### 4-1. 公式 API で足りているか

結論（事実）: **`esp_cam_ctlr_csi` の公式 API を使いつつ、p4kvm / espkvm は `MIPI_CSI_BRIDGE` と ISP のレジスタも直接書いている**。なぜ必要かを述べた文は少ない。

**公式 API に渡している設定**（p4kvm `capture_hw.c:246-273`、espkvm `capture_hw.c:447-480`。同値）:

| 項目 | 値 | 備考 |
|---|---|---|
| `ctlr_id` | 0 | |
| `clk_src` | `MIPI_CSI_PHY_CLK_SRC_DEFAULT` | |
| `h_res` / `v_res` | p4kvm: 1920 × 1080 固定。espkvm: ロック後に測定した値 | |
| `data_lane_num` | 2 | P4 の上限も 2（P2B_PREP 引用の `MIPI_CSI_HOST_LL_LANE_NUM_MAX`） |
| `lane_bit_rate_mbps` | **972**（TC358743 の PLL と一致させる、とコメント。p4kvm `tc358743.c:269`） | IDF はこれから D-PHY の `hs_freq_sel` を引く（`esp_hal_cam/mipi_csi_hal.c:43-51`） |
| `input_data_color_type` = `output_data_color_type` | RGB888（rev < 3）／ YUV422_UYVY（rev ≥ 3） | 入出力が同じなら色変換 bypass（`esp_cam_ctlr_csi.c:635-637`） |
| `queue_items` | 2（espkvm は ring 2、rev ≥ 3 は 3） | |
| `byte_swap_en` | **false** | |
| `bk_buffer_dis` | **true**（自前バッファ。espkvm の理由: ドライバのバックアップバッファは 6 MB で、確保・解放の繰り返しで PSRAM が断片化して失敗した。`capture_hw.c:458-461`） | |
| コールバック | `on_get_new_trans`（バッファを渡す）、`on_trans_finished`（完了） | |

**ISP（bypass）**: `esp_isp_new_processor` を `bypass_isp = true`、`input_data_source = CSI`、`clk_hz = 80 MHz`、`has_line_start_packet = false`、`has_line_end_packet = false`、`bayer_order = BGGR`、`byte_swap_en = false` で作成し、**`ISP.cntl.isp_en = 0` を直接書く**（p4kvm `capture_hw.c:257-270,282-283`、espkvm `capture_hw.c:464-477,494-495`）。ISP を作る理由・作らなくても動くか: **記述なし（未確認）**。

**直接書いているレジスタ**（rev < 3 でも同じ。p4kvm `capture_hw.c:150-159`、espkvm `capture_hw.c:292-301`）:

| レジスタ | 値 | 意味 | 公式 API が自動で設定するか（IDF v6.0.1 のソースを読んだ事実） |
|---|---|---|---|
| `MIPI_CSI_BRIDGE.frame_cfg.hadr_num` | 水平画素数 | 1 行の画素数 | **設定される**（`mipi_csi_hal.c:71`。`esp_cam_ctlr_csi.c:199-200` で名前が入れ替わって渡されるが、結果は h_res）→ 二重設定 |
| `.frame_cfg.vadr_num` | 垂直ライン数 | 1 フレームの行数 | 〃（`mipi_csi_hal.c:72`） |
| `.frame_cfg.has_hsync_e` | 0 | 行同期パケット（Line Start/End）を使わない | 自動設定では**見当たらない**（`mipi_csi_brg_ll_enable_has_hsync` は存在するが、コンストラクタは呼ばない） |
| `.frame_cfg.vadr_num_check` | 0 | 行数チェックを無効 | 〃 |
| `.data_type_cfg.data_type_min/max` | **0x24**（RGB888）、UYVY は **0x1E** | 受け取る CSI-2 データタイプを 1 つに絞る | IDF の既定は **0x12〜0x2F**（`esp_cam_ctlr_csi.c:206-209`）で 0x24 も 0x1E も含む。`data_type` で独自指定も可能（`esp_cam_ctlr_csi_config_t.data_type`、`bits_per_pixel` を非 0 にすると有効） |
| `.int_clr` | 0x3F | 6 つの割り込み要因をクリア | 自動設定なし |
| `.csi_en.csi_brg_en` | 0 / 1 | ブリッジ（DMA 入口）の開閉 | `start` / `stop` が設定（`:553,565`）。espkvm は「フレームゲート」として自前で開閉（rev ≥ 3 のみ、`capture_hw.c:328-371,426-431`） |
| `.host_cm_ctrl.csi_host_cm_en` / `_bypass` / `_8bit_swap` | 1 / 1 / 1 | UYVY のバイト順を戻す。**rev ≥ 3.0 限定**（espkvm `capture_hw.c:303-325`） | rev ≥ 3 では `input_8bit_swap_en` で同等に見える（ソースの読みのみ。**未検証**） |
| `ISP.cntl.isp_en` | 0 | ISP 無効 | — |

- p4kvm は上の `capture_configure_p4_csi_bridge` を **2 回**呼ぶ（CSI 作成直後と、HDMI ロック後の `esp_cam_ctlr_start` 直前。`capture_hw.c:284,289`）。2 回目の理由は**記述なし**。
- 理由の記述として見つかったもの: espkvm の `README.md` Credits 節に「`MIPI_CSI_BRIDGE` の直接設定。`esp_cam_ctlr` API は公開しない。Espressif の例は普通のカメラセンサー向けで HDMI ブリッジ向けではない」（`README.md:336-338`）。rev ≥ 3 の host_cm の理由は `capture_hw.c:303-313` のコメントに具体的（入出力の色タイプが同じだと `esp_cam` ドライバが早期 return し、host_cm が「入力 RGB888、YUV422 並び YVYU、bypass」の既定のまま残り、UYVY が YVYU/RGB として扱われて色が崩れる）。**frame_cfg / data_type / int_clr を v6.0.1 でも書く必要があるか: 記述なし（未確認）**。
- 公式 API の制約（IDF ソース）: rev < 3.0 では `input_8bit_swap_en` / `input_16bit_swap_en` が `ESP_ERR_NOT_SUPPORTED`（`esp_cam_ctlr_csi.c:118`）、色変換は入出力が違うと `ESP_ERR_NOT_SUPPORTED`（`:639-645`）。**転送サイズは `h_res × v_res × 入力 bpp / 64` で作成時に固定**（`:251`）。→ 解像度が変わったら `esp_cam_ctlr` を作り直す（espkvm `capture_hw.c:441-446`、`HARDWARE-NOTES.md:529-530`）。
- IDF のバージョン: p4kvm は **6.0.1**（`README.md:28`、チップ rev < 3 を選ぶ）。espkvm は **6.1**（`README.md` バッジと手順）。

### 4-2. データタイプ・バイト順

| 項目 | 内容 | 出典 |
|---|---|---|
| データタイプ | RGB888 = **0x24**、YUV422 8 ビット = **0x1E**。HDMI の YCbCr444 24bpp も RGB888 と同じ 0x24（ログ文） | p4kvm `capture_hw.c:41,98-103`; espkvm `capture_pixfmt.c:17-60` |
| メモリ上の並び | RGB888 は **BGR** の順で DRAM に入る（p4kvm の起動ログ「CSI 24bpp BGR ring」）。espkvm: 「この CSI の DMA は 32 ビットワード内でバイトを逆順に詰める（RGB888 が BGR になるのと同じ理由）。ワイヤ上の U Y V Y は Y V Y U で着き、色が崩れる」 | p4kvm `capture_hw.c:237`; espkvm `capture_hw.c:316-320` |
| Espressif の記述 | UYVY のとき `ESP_CAM_SENSOR_DATA_SEQ_WORD_INTERNAL_SWAPPED` を返す。RGB888 は `DATA_SEQ_NONE`。コメント「todo, fix yuv seq with `input_8bit_swap_en = true`」 | `tc358743.c:113,865-883` |
| `byte_swap_en`（出力側）| 使っている実装は**見つからなかった**（全て false）。espkvm の測定: ブリッジの `endian_mode` は 64 ビットワード全体の反転で他の状態を持たない | `HARDWARE-NOTES.md:250-252` |

---

## 5. PHY の電源（MIPI D-PHY）

| 実装 | `esp_ldo_acquire_channel` | チャンネル | 電圧 | どのボード向けか | 出典 |
|---|---|---|---|---|---|
| p4kvm | **使っている** | **3** | **2500 mV** | README に特定ボード名なし（「RPi 互換 CSI + Ethernet の ESP32-P4 モジュール」）。同じ GPIO 既定（I2C 7/8、RESETN 23）を espkvm は Waveshare ESP32-P4-ETH の値と明記 | `p4kvm_hw_defaults.h:7-8`、`capture_hw.c:191-196`; espkvm `main/Kconfig.projbuild:83-85` |
| espkvm | **使っている** | **3** | **2500 mV** | 全ボード共通の定数（`KVM_BOARD_MIPI_LDO_CHAN_ID/VOLTAGE_MV`、「chip- or capture-level で基板ごとに変わらない」）。Waveshare ESP32-P4-ETH（rev v1.3、既定）、Function EV、NANO、Guition、PoE 系ほか 10 種以上を「tested」としている。Kconfig では変更不可（`docs/PORTING.md:131-134`: 変える場合はコードの定数） | `kvm_board.h:15-18`、`capture_hw.c:582-587`; `README.md` ボード表 |
| Espressif `esp_cam_sensor` | **TC358743 ドライバには無い**（センサー側で P4 の LDO を扱わない） | — | — | — | `tc358743.c` 全体（grep 結果） |
| ESP-IDF v6.0.1 `esp_cam_ctlr_csi` | **LDO・電源の記述が無い**（`esp_cam_ctlr_csi.c`、`.h`、`mipi_csi_hal.c` を `ldo\|regulator\|power` で検索して該当なし） | — | — | アプリ側で給電する前提 | — |

- 取得順序: LDO 取得 → リセットパルス → I2C → TC358743 の初期化 → CSI 作成（ハンドルは解放しない）。
- `esp_ldo_acquire_channel` の説明: 「`chan_id` は回路図で決めよ（例: LDO_VO1 なら 1）」（`esp_ldo_regulator.h:26,40`）。
- **本プロジェクトの突き合わせ**: ボードの `VDD_MIPI_DPHY` は ESP_LDO_VO3（chan_id 3）、候補は 2500 mV。→ **p4kvm / espkvm の値（chan 3、2500 mV）と一致**。ただし 2 つの実装は同じ系統（espkvm は p4kvm の派生）で、**独立した確認ではない**。電圧の最終確認は回路図とチップのデータシート（`TODO(hw-verify)` のまま）。
- 参考（別用途）: microSD の IO 電源は LDO_VO4（P4-ETH と Function EV）。MIPI とは別チャンネル（espkvm `docs/PORTING.md:141-146`）。

---

## 6. 色の経路

| 構成 | キャプチャ | JPEG | H.264 | 出典 |
|---|---|---|---|---|
| **p4kvm（rev v1.3）** | RGB888、0x24、24bpp、バッファ 2 面 | `cache_msync (M2C)` → `jpeg_encoder_process`、`src_type = RGB888`、`sub_sample = YUV420`、既定品質 70 | **なし**（rev 3+ で「希望的に」可能と書くだけ）。README: 「**YUV は JPEG エンコーダとうまく動かないので、CSI 経路は RGB888 固定**」 | `capture_mjpeg.c:95-125`; `README.md:68-69` |
| **espkvm、rev < 3.0（Waveshare P4-ETH など）** | RGB888、0x24 | 〃（`capture_pixfmt.c:17-29`、`capture_mjpeg.c:246-250,262`） | **PPA（SRM）で RGB888 → YUV420**（BT.601 リミテッド、拡大縮小なし、出力は 16 の倍数に切り上げた高さ）→ H.264 エンコーダ（入力形式 `O_UYY_E_VYY`）。PPA で YUV420 に変換しながら拡大縮小すると**ブロッキング呼び出しが戻らない** | `capture_h264.c:1100-1161`、`capture_pixfmt.c:17-29`、`HARDWARE-NOTES.md:493-519` |
| espkvm、rev ≥ 3.0 | UYVY、0x1E、16bpp、host_cm の 8 ビットスワップ | `src_type = YUV422`、**`sub_sample = YUV422`**（YUV420 にすると色が崩れる） | `ESP_H264_RAW_FMT_UYVY` 直結（PPA なし） | `capture_pixfmt.c:32-61` |
| espkvm、rev < 3.0 + LT6911D（M5Stack、チップ rev v1.3） | YUV422、0x1E | **PPA を「ARGB8888 と偽る」ことで 4 バイトを逆順に並べ替え**（`rgb_swap`）→ JPEG（YUV422）。MJPEG のみ。H.264 は CPU で並べ替え、1280x720 まで | `capture_pixfmt.c:63-88`、`HARDWARE-NOTES.md:223-298,707-733` |
| Espressif | RGB888 または UYVY（上記 4-2 の注意つき） | — | — | — |

- チップ rev v1.3 で動くか: **p4kvm と espkvm は rev v1.3 実機で動作**と自称（p4kvm `README.md:68`、espkvm `HARDWARE-NOTES.md:15`「ESP32-P4 rev v1.3」、`sdkconfig.defaults:7-11` で `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` / `REV_MIN_100`）。rev < 3 と ≥ 3 のイメージは**互換性が無い**（`HARDWARE-NOTES.md:567-574`）。
- 事実（espkvm `HARDWARE-NOTES.md:493-519`）: **rev < 3.0 では H.264 エンコーダは `O_UYY_E_VYY` しか受けない**（`esp_h264_enc_hw_new(): Un-supported h264 picture type` で UYVY を拒否）。**PPA の YUV422 入力モードも rev 3.0 以上限定**。色変換できるのは PPA だけ（CSI ブリッジは rev 3.0 未満で拒否、ISP は RAW 専用）。
- **TC358743 の UYVY を rev v1.3 で JPEG に渡した公開実装は見つからなかった**。最も近い実測は LT6911D（別チップ）での PPA 並べ替え経路（上表）。

---

## 7. ハマりどころ

| # | 症状・注意 | 内容 | 出典 |
|---|---|---|---|
| 1 | 黒画面（レジスタ 1 つの誤りで起きる） | espkvm は「PLL 分周、MIPI タイミングカウンタ、HPD と CSI 送信器を立ち上げる順序」が最も難しい部分で、「レジスタを 1 つ間違えると黒画面でデバッグ手段が無い」と記す | espkvm `README.md:333-335` |
| 2 | CSI が idle のまま | TMDS ロックの前に CSI を設定すると MIPI が idle のまま残ることがある → ロック後に `set_csi` と `CSI_START` を再実行。`CSI_START` は `CONFCTL` が映像を有効にした後にもう一度書く | p4kvm `tc358743.c:713,728-739` |
| 3 | EDID | HPD Low の間に EDID を書く。ソースに DDC させない。広告しすぎると黒画面（次項） | `tc358743.c:611-613` |
| 4 | **EDID でレーン帯域を超えるモードを広告すると黒画面** | 2 レーン × 972 Mbps で RGB888 なら **81 Mpixel/s**（`1.944e9 / 24`）。1080p30（74.25 MHz）が実質上限。1080p60（148.5）や 1280x1024@60（108）は黒画面になる。「フレームが減る」のではなく**フレームが 1 枚も来ず**、ログにタイムアウトが溜まる | espkvm `tools/gen_edid.py:10-31`、`HARDWARE-NOTES.md:525-528`、`capture_hw.c:551-557` |
| 5 | EDID と HDMI スプリッタ | モニターを接続した C792 では、ソースが TV 側の EDID を読んで 1080p60 を選び、絵が出ない | espkvm `docs/BOARDS.md:420-428` |
| 6 | 解像度変更 | `esp_cam_ctlr` は転送長を作成時に固定するので、新しい解像度ごとに**コントローラを作り直す**。バッファは最大モード用に 1 回確保（再確保で PSRAM が断片化し、BIOS → デスクトップの切り替えで失敗する） | espkvm `capture_hw.c:441-446,763-793`、`capture_priv.h:39-44` |
| 7 | 解像度の読み取りが揺れる | モード変化の数 ms は値が不定。espkvm は 200 ms ごとに読み、**同じ値が 2 回続いたら**採用 | espkvm `capture_hw.c:1101-1115,956-961` |
| 8 | FIFO 開始レベル | 3-5 参照（1 行のワード数が 374 未満だとフレームが来ない／高速入力でバンド状の崩れ） | espkvm `tc358743.c:924-967` |
| 9 | ソースがスリープ・無信号 | p4kvm: 「ソースがスリープから戻らないことがある（WIP）」「EDID が合わない機器がある」「KVM より先にターゲットを起動しておく」「ケーブルを替える」「診断: SYS_STATUS の TMDS / HDMI / SYNC / DDC5V が全部 1」。espkvm: HPD は EDID 読み込みとキャプチャ開始まで Low で、電源投入から約 10 秒かかる。その間に POST が走ると「外部モニター無し」と判断する機器がある → KVM を先に起動するか、HDMI を挿し直す。信号が無いまま **10 s → 20 s → 40 s** で自動的に HPD を再接続する（`HDMI_NUDGE_*`）。DDC5V があるときだけ実行 | p4kvm `README.md:58-64`; espkvm `docs/TROUBLESHOOTING.md:31-48`、`capture_hw.c:886-934,1055-1096` |
| 10 | 「何も来ない」の切り分け | `MIPI_CSI_HOST.phy_rx.phy_rxclkactivehs`（クロックレーンが HS か）が 1 なら、レーン上にデータはあり、受信側の解釈が違う。0 ならソース（TC358743）が送っていない | espkvm `HARDWARE-NOTES.md:201-206`、`capture_hw.c:251-264` |
| 11 | I2C | 1 回の転送の詰まりがロックを握り続け、映像・リセット・同じバスの RTC まで止まった → タイムアウト必須 | espkvm `tc358743.c:32-34` |
| 12 | I2C のプルアップ | 内部プルアップを有効にしても外部プルアップの有無は分からない。内部プルダウンを短時間かけて High が保たれるかで判別（espkvm の診断） | espkvm `capture_hw.c:104-126` |
| 13 | warm restart / OTA | CSI の DMA は PSRAM へほぼ常時書き込むため、`esp_restart()` で AXI の途中書き込みが切れ、**次の起動が無言でハング**（OTA 後に「新イメージが動かない」と見えてロールバックされる）。再起動前に CSI を止める | espkvm `capture_hw.c:500-520` |
| 14 | バッファ超過 | ブリッジは DMA の流れを自分で制御する。期待よりバイトが多いとバッファを超えて PSRAM に書き込み、**ブートループ**（esptool で両方の app スロットを書き直す必要）。rev 3.x で YUV420 を試した際の話 | espkvm `HARDWARE-NOTES.md:114-116` |
| 15 | 入力が範囲外 | 最大 1920x1080 のバッファに 1920x1200 などを書くと溢れる → 範囲外なら最大で開始してモード検査で拒否 | espkvm `capture_hw.c:706-728` |
| 16 | 速度が足りない入力 | 画素レートがレーン容量（`2 × レーン速度 / bpp`）を超えると**「減る」のではなく来ない**。監視タスクがログに残して待つ | espkvm `capture_hw.c:1117-1141` |
| 17 | PHY リセット | Linux は SYNC 変化かつ無信号のときに PHY をリセット（`PHY_RST` 0→1）。p4kvm・espkvm は定義だけで**使っていない** | Linux `tc358743.c:1202-1209`; p4kvm `tc358743.c:162-163` |
| 18 | 色が「ひどい」 | UYVY のバイト順（4-2、6 章）。判定は画面を撮った参照画像との比較、列の偶奇別の誤差で順列が特定できる | espkvm `HARDWARE-NOTES.md:740-755` |
| 19 | CPU が待たない処理 | 純 CPU の並べ替えは、ハードウェア待ちが無くアイドルタスクが走らず、5 秒後に WDT 再起動（LT6911D 経路の話） | espkvm `HARDWARE-NOTES.md:281-286` |

---

## 8. 実測値

すべて espkvm の自己申告（`docs/HARDWARE-NOTES.md`、`docs/HOW-IT-WORKS.md`、`README.md`）。p4kvm の README には**数値なし**（動画へのリンクのみ）。

**ボード・チップ**: Waveshare ESP32-P4-ETH、ESP32-P4 **rev v1.3**、360 MHz デュアルコア、PSRAM 32 MB @ 200 MHz（HEX モード）、キャプチャは Geekworm C790（TC358743、2 レーン、972 Mbps）（`HARDWARE-NOTES.md:10-23`、`sdkconfig.defaults:64-67`）。

| 項目 | 値 | 条件 | 出典 |
|---|---|---|---|
| MJPEG 1920x1080 | **20 fps**、17.5 Mbit/s | 品質 70、公開されたフレームのみ | `HARDWARE-NOTES.md:27-33`、`HOW-IT-WORKS.md:70` |
| MJPEG 1280x720 | 45.5 fps、16.8 Mbit/s | 同上 | 〃 |
| MJPEG 1024x768 | 56 fps、17.6 Mbit/s | 同上 | 〃 |
| 静止画面 | 0 kbit/s（同一フレームを送らない）、カーソル移動で 1080p 8.5 Mbit/s・13 fps | | `HARDWARE-NOTES.md:35-36` |
| H.264（PPA 経由）1080p | **約 5〜7 fps**。単体測定: PPA 76.3 ms + エンコード 30.9 ms。**キャプチャ稼働中は 1 フレーム 145 ms**（CSI の DMA が同じ PSRAM に 6 MB/フレーム書くため）。別の記述では「150 ms のうち 104 ms が変換」 | | `HARDWARE-NOTES.md:38-58`、`HOW-IT-WORKS.md:89-92` |
| H.264 720p | 約 17 fps（PPA 57〜60 ms） | | `HARDWARE-NOTES.md:51-54` |
| 遅延 | 約 60 ms（H.264、WebRTC、1080p50）は **rev 3.x**（Function EV）。**rev v1.3 の遅延は記載なし** | | `HOW-IT-WORKS.md:153-160` |
| 温度 | 1080p フル負荷で 46 ℃（rev v1.3、開放空気） | | `HOW-IT-WORKS.md:471-473` |
| M5Stack（LT6911D、チップ rev v1.3）MJPEG | 1080p 8.7〜9.4 fps、720p 21〜23.6 fps（バイト並べ替え 75〜79 ms / 24〜28 ms） | 別チップ | `HARDWARE-NOTES.md:325-330` |
| PPA の並べ替え単体 | 1280x720・16bpp で約 27 ms | | `HARDWARE-NOTES.md:722-724` |
| rev 3.x の参考 | H.264 1080p 25〜27 fps、MJPEG 23 fps（Function EV rev v3.2）。**rev v1.3 の値ではない** | | `HOW-IT-WORKS.md:70-74` |

**PSRAM の使い方**:
- p4kvm: CSI バッファ 2 面 × 1920×1080×3 = 6,220,800 B（計 12,441,600 B）[計算]、JPEG 出力 3 スロット × (1920×1080 + 384 KiB) = 2,466,816 B [計算]（`capture_hw.c:217-237`、`capture_mjpeg.c:30`、`jpeg_frame.h:14`）。DMA 可能・キャッシュアライメント付きで確保。
- espkvm（rev < 3）: ring 2 面 + `drop_fb` 1 面 = 3 × 6,220,800 B（計 18,662,400 B）[計算]（`capture_priv.h:45-69`、`capture_hw.c:667-684`）。MJPEG 出力は 1 面 2.4 MB と記述（`capture_mjpeg.c:50`）。H.264 実行中の rev < 3 は PSRAM の空きが約 3 MB（rev 3.x は 11 MB）（`HARDWARE-NOTES.md:512-513`）。
- 設定: `CONFIG_SPIRAM_MODE_HEX`、`SPEED_200M`、`SPIRAM_MALLOC_ALWAYSINTERNAL=4096`（小さい割り当ても PSRAM へ。H.264 の参照フレームが内蔵 RAM に 155 KB の連続領域を要するため）（`sdkconfig.defaults:64-79`）。

---

## 9. 食い違いと未確認

### 9-1. 実装間・Linux との食い違い（どれが正しいかは判断しない）

| # | 項目 | p4kvm | espkvm | Espressif | Linux（参照） |
|---|---|---|---|---|---|
| 1 | **`TXOPTIONCNTRL`**（クロックの連続性） | `set_csi` で 0（非連続）。`enable_stream` では書かない。コメント「Linux と一致」 | 同じ（`tc358743.c:441,680-681`） | 同じ（`:246,478`） | `set_csi` は DT の `clock-noncontinuous` が無ければ **CONTCLKMODE(1)**。さらに **`enable_stream(true)` が 0 → 1 の順に書く**（LP11→HS 遷移を作るため）。RPi 版も同じ。RPi の overlay は `clock-noncontinuous` を指定しているが、`enable_stream` 側で結果的に連続クロックになるように読める（`tc358743.c:651-653,843-844`；rpi-6.6.y `:544-546,736-737`）。**これが P4 の CSI ホストで効くかは不明** |
| 2 | `HV_RST`(0x85AF) のビット | H_PI_RST=0x01、V_PI_RST=0x02 | 同じ | 同じ | **0x20 / 0x10**（`tc358743_regs.h:568-569`）。既定ではフラグが全て 0 なので、書き込まれるのは「該当ビットのクリア」（実効値が異なりうる） |
| 3 | UYVY の `VI_REP.COLOR_SEL` | 0x60（601 リミテッド） | 0x60 | **0x40**（601 フル）`tc358743.c:425` | 0x60（`:766-767`） |
| 4 | `VI_STATUS1` のインターレースビット | 読まない | **0x10**（コメント「Linux では VI_STATUS3 ではなく VI_STATUS1」）`tc358743.c:146,994` | 読まない | **0x01**（`tc358743_regs.h:410`、`tc358743.c:361`） |
| 5 | 音声レジスタのアドレス | `FORCE_MUTE`=0x8602、`FS_IMODE`=0x8608 など（Linux と別） | Linux と同じに修正（`FORCE_MUTE`=0x8600、`FS_IMODE`=0x8620） | p4kvm と同じ（0x8602 / 0x8608） | 0x8600 / 0x8620 |
| 6 | `BUFINIT_START` | `ms & 0xFF` | `ms / 100` | p4kvm と同じ | `ms / 100` |
| 7 | PLL 後の待ち | 1 ms | 1 ms | 1 ms | `udelay(10)` |
| 8 | EDID を書く位置 | `initial_setup` の直後（PLL/CSI より前） | 同じ | 同じ | PLL/CSI 設定の**後**、HPD 遅延 143 ms を挟む |
| 9 | 972 以外のレーンレート | 972 のみ | 972 のみ（Kconfig で変更可） | 648 でも**972 のタイミング値**を使う | 594 の値を警告付きで流用 |
| 10 | `pll_fbd` 計算 | 整数除算（27 MHz の倍数に丸まる） | 同じ | 同じ | 同じ |
| 11 | 復旧時のハードウェア手段 | HPD 再接続のみ | HPD 再接続。ほか（LT6911D）はリセットピン | HPD 再接続 | PHY リセット + BKSV 消去 |
| 12 | 1 フレームの画素レート上限 | 1080p30 のみ広告 | EDID プロファイルで制御 | 1080p30 のみ | `tc358743_timings_cap`: 640〜1920 × 350〜1200、画素クロック 13〜165 MHz、プログレッシブ、`tc358743.c:71-82` |

### 9-2. 未確認

- `frame_cfg.has_hsync_e` / `vadr_num_check` / `data_type` 絞り込み / `int_clr` を、IDF v6.0.1 の公式 API だけで済ませられるか（既定値を決める TRM を読んでいない）。
- ISP プロセッサを作らなくても CSI が動くか。`CSI 作成直後` と `ロック後` の 2 回呼ぶ理由。
- `TXOPTIONCNTRL` を連続クロックにする必要があるか（P4 側のホストで）。
- 972 以外（445.5、648、749.25 Mbps など）でタイミング値がどう変わるか。FRS ≠ 0 のレート。
- TC358743 のテストパターン（`CONFCTL.YCBCRFMT = 0x80` の COLORBAR）が実際に出るか。どの実装も使っていない。
- TC358743 の UYVY を rev v1.3 で JPEG に渡す経路（PPA で 4 バイト反転してから JPEG の `YUV422` 入力へ）が TC358743 のバイト順で成立するか。
- X630 の基準クロック（27.000 MHz の刻印）とレーン数。参照実装の基板は Geekworm C790 / Waveshare（27 MHz）。Linux は 26 / 27 / 42 MHz のみ許可。
- LDO の電圧を回路図・データシートで確認していない（5 章）。
- Espressif の `esp_cam_sensor` のレジスタ表の一部（`tc358743_types.h`、EDID ヘッダ）は取得できず（404）、**Toshiba 機能仕様書 Rev 1.1** を参照したと自称するが読んでいない。

### 9-3. 取得できなかったもの

- `github.com` の tree / commits は **403**（ファイル名は推測して `raw.githubusercontent.com` を試した）。`api.github.com` も不可。コミット ID は不明。
- 404: espkvm `components/tc358743/tc358743.h`（実体は `include/`）、`edid.c`、`tc358743_regs.h`、`kvm_board.c`、各コンポーネントの Kconfig（実体は `main/Kconfig.projbuild`）。p4kvm `NOTICE`（存在しない）、`capture_hw.h`、`docs/README.md`。
- 404: Linux `include/dt-bindings/media/tc358743.h`、IDF `components/hal/esp32p4/include/hal/mipi_csi_brg_ll.h` の旧パス（実体は `esp_hal_cam/esp32p4/include/hal/`）、`mipi_csi_bridge_struct.h`（レジスタのビット配置は未取得）、Espressif の `tc358743_types.h` / EDID ヘッダ / Kconfig / NOTICE。
- Linux `include/media/i2c/tc358743.h` は最初の取得が HTTP 429（レート制限）で、再取得で成功。
- 東芝の機能仕様書は方針どおり**取得していない**。

---

## 10. 本プロジェクトの計画書との差分

読んだのは `origin/docs/p2b-prep:docs/P2B_PREP.md`（2〜3 章）、`origin/docs/p2b-plan:docs/P2B_PLAN.md`（作業ツリーには無く、他ブランチの `git show` で読み取り）、`docs/HANDOFF.md`、`docs/HW_VERIFY.md`、`docs/PLAN.md`。

| # | 計画書の記述 | 公開実装で分かった事実 | 影響 |
|---|---|---|---|
| 1 | P2B_PLAN §5 案 1: **最初のモードは 720p60 UYVY（445.5 Mbps、`pll_fbd=66`）** | 公開実装で TC358743 を動かしたのは **972 Mbps・RGB888（rev v1.3）** のみ（Espressif に 648 Mbps・UYVY の定義はあるが動作の記述なし）。UYVY は espkvm では **rev ≥ 3 限定**。445.5 Mbps は未使用で、FRS ≠ 0 になる唯一のパス | 最初に動かすなら、実績のある **972 Mbps・RGB888（fbd=144）+ 594 / 972 の表**のほうが参照できる値が多い。445.5 / 749.25 は公開実績なし |
| 2 | P2B_PREP §2-2 / §2-3: `pll_fbd = レーンレート / 6.75 MHz`（例 445.5 → 66、749.25 → 111） | Linux・p4kvm・espkvm の式は **`bps / refclk × prd`（整数除算）**。445.5 Mbps だと 64（= 432 Mbps）に丸まる。6.75 Mbps 刻みの fbd がハードウェア上有効かは**どの実装も検証していない** | `tc358743_pll_calc`（P2b-1）の仕様を、実装の式に合わせるか、6.75 刻みを実機で確認するか決める |
| 3 | P2B_PREP §2-5「CSI のタイミング・PHY レジスタの値と計算式」「起動手順」「出力形式の選択を制御するレジスタ」は未確認 | **594 / 972 Mbps の値**（3-2）、起動順（2 章）、`CONFCTL.YCBCRFMT` / `VOUT_SET2` / `VI_REP`（3-4）が判明。**計算式は Linux にも無い** | 未確認の一部が解消。式は依然として無い |
| 4 | P2B_PLAN §2/§4 Q4/§6 P2b-3: 「`CSI_CONFW`（色形式・レーン数）」 | `CSI_CONFW` は **CSI 制御レジスタへの間接書き込み口**で、色形式は含まない。色形式は `CONFCTL` / `VOUT_SET2` / `VI_REP`。レーン数は `CSI_CONFW` の NOL と、`D?W_CNTRL`、`HSTXVREGEN` | 計画書の記述を直す |
| 5 | P2B_PLAN §3-1: PHY 電源の LDO は**アプリ側で有効化**、チャンネルと電圧は Q1 で確認（標準的には VO3、2.5 V 前後と記憶） | IDF v6.0.1 の CSI ドライバに LDO の記述は**無い**（確認）。p4kvm / espkvm とも `esp_ldo_acquire_channel(chan 3, 2500 mV)` を **リセットの前**に呼ぶ | 候補（VO3 / 2500 mV）と**一致**。ただし同系統の実装で独立した確認ではない。電圧は引き続き回路図・データシートで確認 |
| 6 | P2B_PLAN §3-2: 「`MIPI_CSI_BRIDGE` の直接設定が v6.0.1 でも必要か未確認。まず公式 API」 | p4kvm（IDF 6.0.1）も espkvm（6.1）も直接書いている（4 章の表）。ただし frame_cfg の幅・高さと DT 範囲は v6.0.1 が既に設定する。**追加分の必要性の記述は無い** | 「まず公式 API → 足りなければ直接」の方針は妥当。**差分は 4 項目**（`has_hsync_e`、`vadr_num_check`、DT の絞り込み、`int_clr`）＋ ISP の `isp_en`。bypass + `ISP.cntl.isp_en=0` を実装は使う |
| 7 | P2B_PREP §3-1: 「ドライバ内部のバックアップバッファ（PSRAM）」は既定で確保 | 両実装とも `bk_buffer_dis = true`、`on_get_new_trans` で自前バッファ。espkvm は「バックアップ 6 MB の確保・解放で PSRAM が断片化し失敗」と記す | 注意点として追加 |
| 8 | P2B_PREP §3-2: 「ISP が必須か `bypass_isp` で足りるか未確認」 | 両実装は ISP プロセッサを **bypass_isp=true** で作る（さらに `ISP.cntl.isp_en=0`） | 実績ある構成が分かった。不要かの検証は無い |
| 9 | P2B_PREP §3-3 / §3-6 / P2B_PLAN Q5: UYVY ↔ JPEG `YVYU` の並びは**未解決**。経路 A（PPA で RGB へ）／B（`pixel_reverse` / `byte_swap_en`）／C（RGB888 直結） | **経路 C は p4kvm・espkvm（rev < 3）の実績あり**（RGB888 → JPEG 直結、`sub_sample=YUV420`、1080p で約 20 fps）。UYVY は DMA が 32 ビットワード内でバイトを逆順に書く（rev ≥ 3 は 8 ビットスワップで戻す）。rev < 3 では**PPA を「ARGB8888」と偽って 4 バイト反転**する経路が LT6911D で動いた（別チップ）。`byte_swap_en`（出力側）の使用例は無い。espkvm の測定では出力側 `endian_mode` は 64 ビット反転のみ、JPEG の `pixel_reverse` は 16 ビット内の 2 バイト交換 | 経路 B の候補に「PPA による 4 バイト反転 → JPEG `YUV422`」が加わる。**TC358743 の UYVY で成立するかは未検証** |
| 10 | P2B_PLAN §3 項 3: 「1080p50 を既定の EDID にして、エンコーダが 1 フレームおきに取る運用もしている」 | これは **rev 3.x（16bpp キャプチャ）のみ**。rev v1.3 の RGB888 では、1080p50 のプロファイルは使われず `full`（1080p30 まで）になる（`capture_hw.c:631-638`、`gen_edid.py`） | rev v1.3 に当てはめない |
| 11 | P2B_PLAN §3 項 3 / HANDOFF 108: 「H.264 は PPA 変換が約 104 ms」「1 フレーム 145 ms のうち 76 ms」 | espkvm の文書内で数値が違う: 単体測定 PPA 76.3 ms、実稼働で 1 フレーム 145 ms、別の記述では 150 ms のうち 104 ms | どちらも出所は espkvm だが条件が違う。引用時に条件を併記 |
| 12 | P2B_PLAN §2 / §6 P2b-3: 「CSI のステータスが実値になるか」 | `SYS_STATUS` の TMDS / SYNC を 50 ms 刻みで待つ（最大 5〜6 s）。`CSI_STATUS` のビット（`Hlt`、`RxAct`、`TxAct`、`WSync`）は Linux の定義 `0x0410`（`tc358743_regs.h:400-404`）。**P4 側ホストの `phy_rx.phy_rxclkactivehs`** が「ソースがレーンを駆動しているか」の切り分けに使える | 診断の追加候補 |
| 13 | P2B_PLAN §4 Q7: TC358743 の内蔵テストパターンの有無 | Linux のヘッダに `YCBCRFMT = 0x80`「COLORBAR」の定義あり（`tc358743_regs.h:43`）。使った実装は無い | 実機で試す価値あり（**未確認**） |
| 14 | P2B_PLAN §6: `tc358743_start_csi` の順序は「PLL → CSI のリセット解除 → 開始」（P2B_PREP §2-5） | 公開実装の順序: HPD Low → 初期設定 → EDID → ストリーム停止 → PLL → CSI（CTXRST 含む）→ 色形式 → 割り込み、その後に `enable_stream(true)` → 150 ms → HPD High → 50 ms → `CSI_START`。EDID の位置が Linux と違う（9 章） | 順序の根拠を 2 章から引ける |
| 15 | HW_VERIFY E5 / PLAN 80: 「YUV422 なら 1080p50 が収まる計算（未検証）」 | 帯域は espkvm も同じ計算（121.5 Mpixel/s）で、**実際の 1080p50 は rev 3.x（FIFO 調整つき）で確認**。rev v1.3 は RGB888 + 972 Mbps で 81 Mpixel/s（1080p30 まで）が確認済みの上限 | rev v1.3 で 1080p50 を狙う場合は、UYVY 経路（上の 9 番）が前提で、公開実績なし |
| 16 | P2B_PLAN §8: 「ESP-KVM のコードは読んでいない」 | 今回読んだ（p4kvm・espkvm・Espressif）。**3 つとも同一の系統**（Espressif は p4kvm を参照元に明記）。値の独立した確認にはならない | 引用時は「同系統」と明記 |
| 17 | CLAUDE.md「ESP-KVM / p4kvm はゼロから書く」 | p4kvm のソースは Linux の GPL ドライバに従うと自称しつつ Apache-2.0。espkvm の NOTICE は p4kvm 由来部分を明記 | コピーしない方針は妥当。値・順序は事実として使える |
