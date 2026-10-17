# ESP-IDF v6.0.1 の注意点（このプロジェクトで実際に踏んだもの）

CI の `espressif/idf:v6.0.1` イメージで確認した事実と、IDF 公式の移行ガイド
（`docs/en/migration-guides/release-6.x/6.0/` 配下）の要点。**IDF 5.x 向けの情報（ブログ・サンプル・学習済みの知識）をそのまま使わないこと。**
推測せず、`raw.githubusercontent.com/espressif/esp-idf/v6.0.1/<path>` の該当ヘッダ・ドキュメント、または CI のログで確認する。

## 暗号 / Mbed TLS 4 / PSA Crypto（`security.rst`）
- IDF 6.0 の Mbed TLS は **4.x**（ビルドフラグに `-DMBEDTLS_MAJOR_VERSION=4`、TF-PSA-Crypto）。旧来の暗号 API
  （`mbedtls_sha256_*`、`mbedtls_md_*` の HMAC、`mbedtls/pkcs5.h` の PBKDF2 など）の**大半が削除**され、**PSA Crypto API が主**。
  `mbedtls/pkcs5.h` は存在しない（CI で `fatal error: mbedtls/pkcs5.h: No such file` を確認済み）。
- 暗号操作の前に `psa_crypto_init()` が必要（証明書・鍵のパース、TLS ハンドシェイクを含む。IDF は通常の起動で初期化する。冪等）。
- X.509 の書き出し・SSL cookie/ticket の設定など、**RNG コールバック（`f_rng` / `p_rng`）を取っていた API は、PSA の RNG を使う新しいプロトタイプに変更**。
- `mbedtls/private/` 等の private ヘッダには依存しない。`MBEDTLS_THREADING_C` は既定で有効。
- TLS 1.2 は、有限体 DHE / 前方秘匿のない RSA 鍵交換 / 静的 ECDH が削除されている（古いクライアントで接続できない場合がある）。
- 詳細: IDF リポジトリの `components/mbedtls/mbedtls/docs/4.0-migration-guide.md`、`.../tf-psa-crypto/docs/1.0-migration-guide.md`、`.../psa-transition.md`。

## Ethernet（`networking.rst`、`esp_eth.rst`）
- 個別 PHY ドライバ（`esp_eth_phy_new_ip101()` ほか）は **IDF から削除**され、外部リポジトリ（esp-eth-drivers、コンポーネントレジストリ）に移動。
  IEEE 802.3 準拠の PHY には **`esp_eth_phy_new_generic()`** を使う。`CONFIG_ETH_PHY_IP101` のような Kconfig はない。
- RMII クロックの Kconfig は**削除**。設定は EMAC の構造体（`eth_esp32_emac_config_t`、`ETH_ESP32_EMAC_DEFAULT_CONFIG()`）だけ。
  ESP32-P4 のデフォルトは本基板の結線と一致（MDC=31, MDIO=52, REF_CLK=50 外部入力, TX_EN=49, TXD0=34, TXD1=35, CRS_DV=28, RXD0=29, RXD1=30）。
- `ETH_PHY_DEFAULT_CONFIG()` の `reset_gpio_num` の既定は 5。**本基板では 51 に上書きが必要**。`phy_addr` は `ESP_ETH_PHY_ADDR_AUTO`。
- `ETH_ENABLED` はユーザーが設定できない非表示の項目。`ETH_USE_ESP32_EMAC` は ESP32-P4 で既定 y（`sdkconfig.defaults` への追加は不要）。
- `ETH_DMA_BUFFER_SIZE` は ESP32-P4 では 64 バイト境界が必要（既定 512 のまま）。

## 開発・CI の運用メモ
- `idf-build` は約 3 分。ホストテストは数十秒。ローカルで IDF のビルドができない環境では、**CI のログが一次情報**。PR を作って CI を回し、ログを読んで直す。
- `components/` 配下のコンポーネントは、`main` が `REQUIRES` していなくても全部ビルドされる（未使用でもコンパイルエラーは CI で検出される）。
- コンポーネントが別コンポーネントのヘッダを include するなら、`idf_component_register` の `REQUIRES`（または `PRIV_REQUIRES`）に書く。
- ホストテストの UBSan は致命的（`-fno-sanitize-recover=undefined`）。未定義動作は即失敗する。
- この環境（レビュー側）から `raw.githubusercontent.com` は読めるが、`api.github.com` 経由の esp-idf リポジトリの閲覧と、`components.espressif.com`（レジストリ）は到達できない。
