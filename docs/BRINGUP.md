# 実機ブリングアップ手順 (BRINGUP)

ESP32-P4-ETH を初めて動かす手順です。**段階ごとに進め、各段階の結果を `docs/HW_VERIFY.md` の該当項目に記入してください。** うまく動かない段階があれば、その段階の「貼ってほしい情報」を Claude に貼ってください（原因の切り分けをします）。

- このファームは、マージ済みの `claude/brave-cori-5m9w1m`（`net_eth`、`tc358743`、`frame_hub` とテストパターン、`auth_logic` を含む）を対象にしています。
- **電気的な値（電圧・電流）は、この文書では確定しません。** 測定が必要な項目は `docs/HW_VERIFY.md` に測定値と条件を書いてください。
- 手元の接続物に応じて、段階 0 から順に必要な分だけ進めれば構いません（段階 2 は X630 と変換ケーブルが必要です）。

## 0. 準備

### 0-1. 開発環境（ESP-IDF 6.0.1）
**ESP-IDF は 6.0.1 を使ってください**（5.x の手順やサンプルは、このプロジェクトでは使えない部分があります。`docs/IDF6_NOTES.md` 参照）。

```sh
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4
. ./export.sh           # 新しいシェルのたびに実行
idf.py --version        # v6.0.1 と表示されること
```

Windows は、ESP-IDF 公式のインストーラ（ESP-IDF Tools Installer）で 6.0.1 を選んでください。CI は Docker イメージ `espressif/idf:v6.0.1` でビルドしています（Linux では `--device=/dev/ttyUSB0` を渡してコンテナから書き込むこともできますが、macOS / Windows では、ホストに入れた方が簡単です）。

（Python は 3.13 で導入を確認しました。3.15 では pip が `setuptools` を解決できず、導入に失敗した報告があります。）

### 0-2. ソースの取得
```sh
git clone https://github.com/windschord/quad-ipkvm.git
cd quad-ipkvm
git checkout claude/brave-cori-5m9w1m
```

### 0-3. 基板の接続
- 基板の **USB-to-UART 側の Type-C**（回路図の `USB to UART` / CH343P）を PC に接続します。シリアルポート名（Linux: `/dev/ttyACM*` または `/dev/ttyUSB*`、macOS: `/dev/cu.usb*`、Windows: `COM*`）を控えます。
- 書き込みに失敗するときは、**BOOT ボタンを押しながら RESET を押して離す**（ダウンロードモード）と成功することがあります。
- **ATX リレー、KVM、PC のフロントパネルなど、まだ電気的に確認していないものは、まだ何も接続しないでください。**

## 段階 0: ビルド・書き込み・起動（HW_VERIFY D1）

> **設定を変えたら `sdkconfig` を作り直す**: `sdkconfig.defaults` は、`sdkconfig` が**まだ無いとき**にしか読まれません。すでに `sdkconfig` があると、`sdkconfig.defaults` の変更（フラッシュサイズなど）は反映されません。リポジトリを更新したあとは、次のようにしてください。
> ```sh
> rm -f sdkconfig
> idf.py set-target esp32p4        # sdkconfig.defaults から sdkconfig を作り直す
> idf.py menuconfig                # 段階ごとの有効化（NET_ETH_INIT など）は、作り直すと消えるので設定し直す
> ```

```sh
idf.py set-target esp32p4
idf.py build
idf.py -p <シリアルポート> flash monitor      # monitor の終了は Ctrl+]
```

**期待するログ**（抜粋）:
```
ESP32-P4 chip revision: v1.3        ← app_main のログ。チップ rev が v1.3 であること
```

**この段階で出ると予想されるメッセージ（異常ではありません）**:
- `Failed to create frame hub`（`app_main`）。理由: `main.c` の `frame_hub` は 256KiB × 3 のバッファを `malloc` で確保しますが、**PSRAM を有効にしていない**ため、チップ内蔵の SRAM（約 768KB）にはほぼ収まらず、確保に失敗すると考えられます（`TODO(hw-verify)` のまま。実際の結果を教えてください）。**起動は続き、他の動作に影響はありません。** PSRAM の有無・設定が確認できたら、`frame_hub` のサイズと確保元を設定可能にする作業を Jules に依頼します。
- ブート時に `Flash size` や `PSRAM` に関する警告が出たら、そのまま貼ってください（`sdkconfig.defaults` のフラッシュ/PSRAM 設定は未確認です）。

**合否**: チップ rev が表示され、`app_main` まで到達し、リブートを繰り返さなければ合格。

**貼ってほしい情報（失敗時）**: 電源投入から `app_main` の最初のログ（または異常終了のスタックトレース/リブートのループ）までのシリアル出力全体。

**記入**: HW_VERIFY D1 に「チップ rev、IDF バージョン、日付」。

### 初回起動（NVS が空）の確認
`sdkconfig` が無いクリーンな状態で、Web サーバーの証明書生成とネットワーク初期化が正しく行われるか確認します（CI では見つからなかった、メインタスクのスタック不足や初期化順序の不具合を修正しました）。

```sh
rm -f sdkconfig
idf.py set-target esp32p4
# 必要に応じて idf.py erase-flash を実行して NVS をクリア
idf.py -p <シリアルポート> flash monitor
```

**期待するログ**:
- 初回起動時: `Generated new certificate in N ms` が表示され、再起動せずに `Web server started successfully` となること。既定（`CONFIG_NET_ETH_INIT=n`）でも起動すること。
- 初回起動時: `cert task stack high water mark: ... bytes free` が出力され、スタック空き量が確認できること。
- 2 回目以降の起動: `Loaded certificate and key from NVS.` が表示されること。

**貼ってほしい情報（失敗時）**: `Stack protection fault`（スタック不足）や `Invalid mbox`（初期化順序エラー）、その他のエラーログを含むシリアル出力全体。

## 段階 0b: PSRAM とフラッシュサイズの確認（任意・段階 1 以降と並行可）

- フラッシュは、実機では 32MB ですが、現在のイメージヘッダは 2MB のままです（`sdkconfig.defaults` に設定が必要。Jules に依頼予定）。
- PSRAM は有効になっていません。確認するには、次を試して、ブートログ（`PSRAM` / `spiram` / `heap` に関する行）を貼ってください。
```sh
idf.py menuconfig
#   Component config → ESP PSRAM → [*] Support for external, SPI-connected RAM
#   （モード・周波数などは、**既定値のまま**で一度試し、結果を貼ってください。推測で変更しないでください）
idf.py -p <シリアルポート> flash monitor
```
- 期待する結果: PSRAM の容量が表示されて起動する。起動に失敗する、または容量が表示されない場合は、そのログを貼ってください（`TODO(hw-verify)`）。

## 段階 1: Ethernet（HW_VERIFY F1〜F6）

LAN ケーブル（DHCP のあるルーター）を基板の RJ45 に接続します。

```sh
idf.py menuconfig
#   Net Ethernet Configuration  →  [*] Initialize Ethernet (net_eth)
idf.py -p <シリアルポート> flash monitor
```
（`menuconfig` を使わず、`echo CONFIG_NET_ETH_INIT=y >> sdkconfig && idf.py reconfigure` でも構いません。）

**期待するログ**（`net_eth` タグ）:
```
Starting Ethernet initialization...
Ethernet initialized successfully
Ethernet Started
Ethernet Link Up
Ethernet HW Addr xx:xx:xx:xx:xx:xx
Ethernet Got IP Address
ETHIP:192.168.x.x
ETHMASK:255.255.255.0
ETHGW:192.168.x.x
```

**確認すること**:
| 項目 | 見るところ |
|---|---|
| F2 PHY アドレス | `esp_eth` / PHY のログに出るアドレス（自動検出の結果） |
| F3 REF_CLK でリンクが張れる | `Ethernet Link Up` が出るか |
| F4 DHCP・速度 | `Got IP Address` と、ルーター側または `esp_eth` のログで 100M / Full |
| F6 汎用 PHY ドライバ（IP101GRI） | 上記が全部通れば合格 |
| F1 結線 | リンクが張れれば、RMII の結線は一致していると判断できる |

**うまくいかないとき**（例）:
- `Ethernet Link Up` が出ない: LAN ケーブル・ルーターのポート・リンク LED を確認。PHY の初期化エラー（`esp_eth` / `emac` のログ）が出ていれば、それを貼ってください。
- `Link Up` だが `Got IP` が出ない: DHCP サーバの有無、ルーターの接続端末一覧に基板の MAC があるかを確認。
- どれでも、**電源投入から 30 秒ぶんのログ全体**を貼ってください。

**記入**: HW_VERIFY F1〜F6（取得した IP アドレスは書かなくて構いません）。

## 段階 2: TC358743（X630）の I2C 診断（HW_VERIFY E1〜E4）

**必要なもの**: X630 v1.5、22 ピン(0.5mm) → 15 ピン(1.0mm) 変換ケーブル（E2）。

**接続の注意（重要）**:
- **電源を切ってから** FPC ケーブルを抜き差ししてください。**ケーブルの接点面の向きは、必ず確認してから**差し込みます（向きを間違えると電源ピンとデータピンが誤接続になるおそれがあります。E2 の「未確認」項目です）。不安なときは、導通チェックでピン対応を確認し、結果を E2 に記入してください。
- **最初は HDMI ケーブルを接続しないで**起動します（E7: HDMI からの back-power の有無が未確認のため）。HDMI を接続する場合は、段階 2 が通ってから、次の起動で行ってください。

```sh
idf.py menuconfig
#   Quad IP-KVM Configuration  →  [*] Initialize TC358743 on startup
idf.py -p <シリアルポート> flash monitor
```

**期待するログ**（`tc358743` タグ）:
```
Initializing TC358743...
Found TC358743, Chip ID: 0x00xx (Revision 0x..)   ← 上位バイトが 0x00 であること
Generating and writing 256-byte EDID to RAM...
EDID programmed successfully
...
----- TC358743 Diagnostics -----
Chip ID: ...  SYSCTL: ...  HDMI SYS_STATUS: ... (TMDS detected: no, SYNC: no)
CSI_STATUS: ...  CSI_ERR: ...
```
`UNVERIFIED PLL parameters ... Skipping`、`UNVERIFIED CSI timings ...`、`UNVERIFIED: ...` の警告が出ますが、**これらは未実装部分の意図した警告で、異常ではありません**（PLL / CSI / PHY の設定は P2b の実機確認後に実装します）。

**確認すること**: `Chip ID validation failed`（I2C で応答がない、またはアドレス違い）が出ないか。I2C は SDA=GPIO7、SCL=GPIO8、アドレス 0x0F（回路図と Linux ドライバの既定値。E3）。

**HDMI を接続した場合**: `TMDS detected: yes` になるかを確認してください（映像信号があるか。EDID が相手に認識されたかの目安）。

**診断ログの読み方（2026-10-21 の実機結果から）**:
- `Chip ID`、`SYSCTL`、`HDMI SYS_STATUS` は実値として読んでよいです。`SYS_STATUS=0x09` は、DDC5V（相手が 5V を出している）と PHY_SCDT（信号の検出）が立っている状態で、`TMDS detected: no` のままでも、HDMI を接続したことは検出されています。
- **`CSI_STATUS` / `CSI_ERR` は、CSI を開始する前は無効な値です。** 現状では PLL と CSI の設定をスキップしているため、CSI ブロック（0x04xx）の読み出し値が、直前に読んだレジスタの値の複製になることが確認されています（例: 直前が `SYS_STATUS=0x09` なら `0x0909`）。CSI 開始後に読み直して判断します。

**記入**: HW_VERIFY E1（ピン表）、E3（アドレス・I2C 速度）、E4（RESET/INT が見えるか）。

**貼ってほしい情報（失敗時）**: ログ全体と、ケーブルの接続（向き、使ったケーブルの型番）。

## 段階 3: テストパターン供給（ログのみ）

段階 0 で `Failed to create frame hub` が出ている間は、テストパターンは開始しません。PSRAM 対応後に、`Starting test pattern source...` が出ることだけ確認します。**画面に映像が出るのは P3-3b 以降です**（ブラウザで見るための HTTPS / WebSocket がまだありません）。

## 結果のまとめ

次の表をコピーして、結果を貼ってください。

| 段階 | 結果（OK / NG / 未実施） | メモ（ログの該当行など） |
|---|---|---|
| 0 ビルド・起動（D1） | | チップ rev: |
| 1 Ethernet（F1〜F6） | | PHY アドレス / 速度 / リンクまでの時間: |
| 2 TC358743（E1〜E4） | | Chip ID: / HDMI 接続の有無: |

## 次の作業（結果に応じて）
- 段階 0 の結果から、`sdkconfig.defaults` のフラッシュ/PSRAM 設定を決め、`frame_hub` の確保サイズ・確保元（PSRAM 優先）を設定可能にします（Jules に依頼）。
- 段階 1 が通れば、P3-3 の実機確認（HW_VERIFY F5）に進めます。
- 段階 2 が通れば、P2b（CSI でフレームを 1 枚取る）の実機デバッグに進めます。

## 段階 4: Web サーバーと認証画面の確認 (P3-3b-2a)

Ethernet (段階 1) が通った状態で、Web ブラウザからアクセスして確認します。

1. **アクセスと証明書の承認**
   - 同じ LAN にある PC のブラウザから `https://<ESP の IP アドレス>/` を開きます。
   - 初回起動時（自己署名証明書）のため、ブラウザがセキュリティ警告を出しますが、無視して進めてください。
   - 開発者ツール (F12) のコンソールを開き、CSP 違反 (Content-Security-Policy 関連のエラー) が出ていないことを確認します。
2. **初回設定とログイン**
   - 「初期設定」画面が表示されるので、パスワードを入力して「設定する」を押します。
   - 続けてログインし、「ログイン済み」と「ログアウト」ボタンが表示されることを確認します。
3. **ログアウトと再ログイン**
   - 「ログアウト」ボタンを押してログイン画面に戻ることを確認し、再度設定したパスワードでログインできることを確認します。
4. **コマンドラインからの確認 (curl)**
   - CSP と Cache-Control の確認: `curl -k -i https://<ESP の IP アドレス>/` を実行し、`Content-Security-Policy` ヘッダと `Cache-Control: no-store` ヘッダが付与されていることを確認します。また、`Content-Security-Policy` ヘッダが 1 つだけであることを確認します。
   - 存在しないパスの確認: `curl -k -i https://<ESP の IP アドレス>/nonexistent` を実行し、ステータスが `404` であることを確認します。
   - メソッド不許可の確認: `curl -k -i -X POST https://<ESP の IP アドレス>/` を実行し、ステータスが `405` であることを確認します。
   - Host ヘッダ不一致の確認: `curl -k -i -H "Host: wrong-host" https://<ESP の IP アドレス>/` を実行し、ステータスが `403` であることを確認します。

## 実施記録
- **2026-10-21**（オーナー側 Claude の報告）: 段階 0 OK（チップ rev v1.3、ESP-IDF v6.0.1）、段階 1 OK（Link Up は起動後約 3.4 秒、DHCP 取得、ping 3/3。PHY アドレスと 100M/全二重はログに出ず未確認）、段階 2 OK（I2C 0x0F で応答、Chip ID 0x0000、EDID 書き込み成功。HDMI 接続時 `SYS_STATUS=0x09`）、段階 3 未実施（`frame_hub` の作成に失敗するため）。詳細は `docs/HW_VERIFY.md` の D1・E3・E4・E7・F1〜F6。
- **2026-10-22**（オーナー側 Claude の報告）: 段階 0b OK（PSRAM を**既定値のまま**有効化。HEX・200MHz・32MB、メモリテスト OK、`frame_hub` の作成と `Starting test pattern source...` を確認）、段階 1 の抜き差し・300 回 ping OK、PBKDF2 の動作と所要時間（`docs/HW_VERIFY.md` D5）。シリアルの取得は、RTS でリセットしてから読む方法が確実（`monitor` は TTY が必要）。F2・F4（PHY アドレス、100M/全二重）は、P-BOOT-1 のログで確認する。
