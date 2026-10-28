# OLED-3 の計画: SSD1306 の駆動と状態表示（ESP-IDF との結合）

作成: 2026-10-28。`docs/PLAN.md` 4.4（OLED と I/O エキスパンダの仕様）、Issue #44（タスクの分割）、マージ済みの `oled_ui`（状態機械）・`oled_render`（描画）・`mcp23017_logic`（ボタンと LED）をもとに、IDF との結合（OLED-3）の設計と、ESP-IDF v6.0.1 のソースで確認した事実を整理する。**ハードウェアは操作していない（机上の調査）。** 電気的な数値（I2C アドレス、電源電圧、プルアップ）は確認するまで確定しない（`TODO(hw-verify)`、`docs/HW_VERIFY.md` G 章）。

事実の区別: **[出典]** ESP-IDF v6.0.1 のソースで確認、**[確認]** このリポジトリのコードで確認、**[推測]**、**[未確認]**。

---

## 1. 結論

- **SSD1306 のドライバは、ESP-IDF v6.0.1 に標準で入っている**（`esp_lcd_new_panel_ssd1306`）。外部コンポーネントも、自前の初期化コマンド列も要らない [出典: `components/esp_lcd/include/esp_lcd_panel_ssd1306.h`、`src/esp_lcd_panel_ssd1306.c`]。
- 描画データの形式は、`oled_render` の `oled_fb_t`（ページ形式、1 バイト = 縦 8 画素、最下位ビットが上）と**そのまま一致**する。**`oled_dirty_t`（ページごとの変化した列の範囲）を、ページごとの `draw_bitmap` に直接対応させられる**（§3）。
- I2C のバス（`i2c_master_bus_handle_t`）は、**複数のタスクで共有できる**（送受信ごとにバスのロックを取る）[出典: `components/esp_driver_i2c/i2c_master.c`]。TC358743 と同じバスに並列に繋ぐ方針（4.4）と合う。
- **OLED-3 の障害は、ハードではなくソフト側**: OLED に渡す**状態の情報源がまだ揃っていない**（§4）。ボタンは MCP23017（OLED-4）が来るまで物理的には使えない（§5）。

## 2. ESP-IDF v6.0.1 で確認した事実 [出典]

| 項目 | 内容 |
|---|---|
| ドライバ | `esp_lcd_new_panel_ssd1306(io, &panel_config, &panel)`。既定は 128x64、`vendor_config`（`esp_lcd_panel_ssd1306_config_t.height`）で 32 に変更可。`bits_per_pixel` は **1 のみ** |
| 接続 | `esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t, &esp_lcd_panel_io_i2c_config_t, &io)`。`dev_addr`、`scl_speed_hz`、`control_phase_bytes`、`dc_bit_offset`、`lcd_cmd_bits`、`lcd_param_bits`。**SSD1306 用の値は、IDF の公式の例 `examples/peripherals/lcd/i2c_oled` にある**: `control_phase_bytes = 1`、`lcd_cmd_bits = 8`、`lcd_param_bits = 8`、`dc_bit_offset = 6`（SSD1306 のデータシートによる、と例にある）、パネル側は `bits_per_pixel = 1`、`dev_addr = 0x3C`。`esp_lcd` コンポーネントは `esp_driver_i2c` に依存 |
| 初期化 `esp_lcd_panel_init` | マルチプレックス比（`height - 1`）、COM ピン構成（64 行は 0x12）、表示オフ、**水平アドレッシングモード（0x00）**、**チャージポンプ有効（0x14）**、左右・上下ミラーなし、を送る。**コントラスト・クロック分周・プリチャージなどは送らない**（既定値） |
| 描画 `draw_bitmap(x_start, y_start, x_end, y_end, data)` | 列の範囲とページの範囲を指定して送る。**`y` はページ境界（8 の倍数）**。データ長 = `(y_end - y_start) × (x_end - x_start) / 8` バイト。1 ページ（8 行）× 幅 W なら W バイト |
| 表示の ON/OFF `esp_lcd_panel_disp_on_off(panel, on)` | `true` = 表示オン。**内部で 100ms 待つ（`vTaskDelay`）** → **タスクの中で呼ぶ。httpd のコールバックなどで呼ばない** |
| 向き | `esp_lcd_panel_mirror(panel, mirror_x, mirror_y)`（実機の向きの確認は G5）、`esp_lcd_panel_invert_color` |
| リセット | `panel_dev_config.reset_gpio_num`（-1 なら使わない。**モジュールが RST を引き出していて、GPIO に繋ぐかは未確認**） |
| I2C のロック | `i2c_master_transmit` などは、バスのミューテックス（`bus_lock_mux`）を取る → **同じバスのハンドルを複数のタスクで使える**。ただし、1 回の転送の間、他のタスクは待たされる |
| 速度 | I2C マスターの SCL は **400kHz 以下**（IDF のドキュメントの推奨）。プルアップは 2〜5kΩ が推奨 |

## 3. 描画と I2C の負荷

- 128x64 の全画面は 1024 バイト。400kHz では、アドレスとコマンドを含めて約 **25〜30ms**（1 バイト = 9 クロックとして、1024×9÷400k ≈ 23ms + オーバーヘッド）[計算]。これが TC358743 のポーリング（P2b）の I2C を待たせる最長の時間。
- **`oled_render_diff` の結果（ページごとの最小〜最大の列）を使い、変化したページだけを、そのページ 1 回ずつの `draw_bitmap` で送る**: `draw_bitmap(panel, min_x, page*8, max_x+1, page*8+8, &fb->data[page][min_x])`（データは 1 ページの中で連続している）。1 回の転送が最大でも 128 バイト（約 3ms）になり、他のタスクの待ちが短くなる。
- 表示内容の更新は 1Hz 程度（`oled_ui` の再描画の間隔）。ほとんどの更新で変化するのは、稼働時間などの数ページ。

## 4. OLED に渡す状態の情報源（現状と不足）

`oled_status_t`（`oled_render.h`）の各項目:

| 項目 | 情報源 | 現状 |
|---|---|---|
| IP アドレス | `net_eth`（`IP_EVENT_ETH_GOT_IP`） | **取得するアクセサがない**（`net_eth.h` は状態と通知のみ）。`esp_netif_get_ip_info` で取れる見込み → **`net_eth_get_ip_str()` を足す** |
| リンク速度 | `esp_eth_ioctl(ETH_CMD_G_SPEED)` | **ログに出すだけ**。**`net_eth_get_speed_mbps()` を足す** |
| ホスト名 | — | **どこにも設定していない**（mDNS も未導入）。当面は `CONFIG_WEB_SERVER_ALLOWED_HOSTS` の先頭、または固定の名前。**判断が必要**（§6） |
| KVM チャンネル | `kvm_logic` | 未結合（P6）。当面 `-1`（不明）→ `CH -` |
| 映像ソースの状態 | `video_source`（P2b） | なし。当面 `OLED_VIDEO_STATE_UNKNOWN` |
| クライアント数 | `ws_clients_count`（`web_server` の内部） | **`web_server_ws_client_count()` を足す** |
| 稼働時間 | `esp_timer_get_time()` | あり |
| ファームのバージョン | `esp_app_get_description()->version` | あり（`app_update` コンポーネント） |
| 空きメモリ | `esp_get_free_heap_size()`（または PSRAM を含む） | あり |
| `needs_setup` | `web_server` の認証状態 | **`web_server_needs_setup()` を足す**（`s_auth_mutex` の下で読む） |

→ OLED-3 は、**情報源のアクセサを足す**作業を含む。`oled_display` 自体は、状態を `oled_status_provider_t`（関数ポインタ 1 本: `void (*fill)(oled_status_t *out, void *user)`）で受け取り、`net_eth` や `web_server` に直接依存しない（結合は `main.c` で行う）。

## 5. 設計（案）

### 5.1 構成

- **`components/oled_display`（IDF）**: タスク 1 本（`oled_display`、優先度は低め、スタックは Kconfig）が、約 100ms ごとに `oled_ui_tick`、イベント（ボタン、IP 取得）を受けて、`TURN_ON` / `TURN_OFF` / `REDRAW` に従う。
  - `TURN_ON`: `esp_lcd_panel_disp_on_off(on)`（100ms の待ちを含む）+ 描画。`TURN_OFF`: `disp_on_off(off)`。`REDRAW`: `oled_render_page` → `oled_render_diff` → ページごとの `draw_bitmap`。
  - 外部への API: `oled_display_start(bus, &cfg)`、`oled_display_on_button()`（任意のタスク/ISR 外から呼べる。内部でイベントキューに積む）、`oled_display_on_ip_acquired()`。
- **I2C バスの共有**: 現状 `main.c` の `if (CONFIG_TC358743_INIT)` の中で I2C バスを作っている。**バスの作成を `main.c` の先頭に出し（`board_i2c` のような関数）、TC358743 と OLED（と、のちの MCP23017）に同じハンドルを渡す**。バスの設定は既存のまま（ポート 0、SDA=GPIO7、SCL=GPIO8、内蔵プルアップなし。ボード上の 2.2kΩ）。
- **OLED が接続されていなくても起動を妨げない**: `i2c_master_probe` でアドレスの応答を確認し、応答がなければログを出して無効化（**10 秒ごとに再確認**して、後から接続されたら有効化）。
- **Kconfig**: `OLED_ENABLE`（既定 `n`）、`OLED_I2C_ADDR`（既定 0x3C。`TODO(hw-verify)`: G2）、`OLED_MIRROR_X` / `OLED_MIRROR_Y`（向き。G5）、`OLED_I2C_SPEED_HZ`（既定 400000 以下）、`OLED_TASK_STACK_SIZE`。
- **表示の消灯中は、I2C を触らない**（`oled_ui` が OFF のとき `REDRAW` を返さない仕様）。

### 5.2 OLED-3 の作業単位

| ID | 内容 | 担当 | 備考 |
|---|---|---|---|
| OLED-3a | アクセサの追加: `net_eth_get_ip_str` / `net_eth_get_speed_mbps`、`web_server_needs_setup` / `web_server_ws_client_count`。ホストでテストできる部分（文字列の整形など）は純 C にして、ホストテスト | Jules | 既存のテスト・API を壊さない |
| OLED-3b | `board_i2c`（バスの作成を `main.c` から切り出す）+ `oled_display`（上の設計）+ Kconfig | Jules | `idf-build` が緑。ホストでは、`oled_display` の「ページごとの転送範囲の計算」（`oled_dirty_t` → `draw_bitmap` の引数）を純 C にしてテスト |
| OLED-3c | 実機確認: 表示、30 秒で消灯、IP 取得で点灯、向き、I2C バスの共有（TC358743 の診断ログと同時に動かす） | 実機担当 | `HW_VERIFY` G2・G5・G8 |

OLED-4（MCP23017 からのボタン入力、パネル LED）は、MCP23017 の実機の確認（G4: `INTA` / `INTB` / `/RESET` の引き出し、G1〜G3）が済んでからでよい。

## 6. 判断が必要な点

### 6.1 ボタン入力が物理的に来るまで、どうするか（選択肢。おすすめ順）

| 順位 | 案 | 内容 | 理由 |
|---|---|---|---|
| **1（おすすめ）** | **API だけ用意して待つ** | `oled_display_on_button()` を用意し、OLED-3 のあいだは、起動時と IP 取得時の自動点灯だけで動かす。物理ボタンは OLED-4（MCP23017）で結合 | 捨てるコードがない。GPIO を消費しない。自動点灯で表示内容の確認（OLED-3c）は足りる |
| 2 | ボード上の **BOOT ボタン**を実行時に流用 | 起動後に入力として読む | 追加の部品が要らない。ただし、ストラップピンであることと、配線を**回路図で確認する必要がある**（`HW_VERIFY` B1・B2）。後で捨てることになる |
| 3 | **空いている GPIO に仮のボタン**を付ける | 4〜5 本の予備のうち 1 本 | 動作確認はしやすいが、配線の手間と、後で外す作業が増える |
| 4 | **シリアルコンソールのコマンド**で擬似的に押す | 開発用のみ | 実機では使えない（TTY が必要）。コードが残る |
| 5 | **認証付きのデバッグ API**（HTTP）で押す | `POST /api/v1/debug/oled/button` | 攻撃面が増える。**推奨しない** |
| 6 | OLED-3 を、ボタンと一緒に（OLED-4 の後に）実装する | 全部そろってから | 一度に確認する項目が増え、問題の切り分けが難しい |

### 6.2 ホスト名の表示

`oled_status_t.hostname` に何を出すか。案: (1) `CONFIG_WEB_SERVER_ALLOWED_HOSTS` の先頭（既定 `quad-ipkvm.local`）を表示するだけ（おすすめ。mDNS は別の機能）、(2) mDNS（`mdns` コンポーネント）を導入して実際に名前解決できるようにする（機能追加。別のタスク）、(3) ホスト名の行を表示しない。**(1) を推奨**し、(2) は別の Issue に切り出す。

## 7. 未確認（`TODO(hw-verify)`）

- OLED の I2C アドレス（0x3C / 0x3D）、電源電圧（3.3V）、向き、見え方（G2・G5）。
- I2C バスの共有時の干渉（OLED の 1 ページの転送と、TC358743 のポーリング）（G8）。
- IDF の公式の例（`examples/peripherals/lcd/i2c_oled`）の設定値を、この基板のモジュールで使って問題ないか（例は 0x3C、400kHz）。モジュールが SSD1306 でなく SH1106 などの互換品だと、表示がずれる（G5 で確認）。

## 8. 出典

- ESP-IDF v6.0.1: `examples/peripherals/lcd/i2c_oled/main/i2c_oled_example_main.c`、`components/esp_lcd/include/esp_lcd_panel_ssd1306.h`、`src/esp_lcd_panel_ssd1306.c`、`include/esp_lcd_io_i2c.h`、`src/esp_lcd_panel_ops.c`、`components/esp_driver_i2c/i2c_master.c`、`docs/en/api-reference/peripherals/i2c.rst`。
- このリポジトリ: `components/oled_ui`、`components/oled_render`、`components/mcp23017_logic`、`components/net_eth`、`main/main.c`、`docs/PLAN.md` 4.4、`docs/HW_VERIFY.md` G 章。
