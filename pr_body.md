MIPI D-PHYの電源(`VDD_MIPI_DPHY`)をLDOから供給するための `mipi_phy_power` コンポーネントを追加します。

- `mipi_phy_power` コンポーネントを新規作成。
  - 純Cの `mipi_phy_power_validate` 関数を実装し、LDO チャンネル（3番のみ）と電圧（2250〜2750mV）の範囲を検証。
  - `esp_ldo_acquire_channel()` で LDO を有効化し、`esp_ldo_release_channel()` で無効化する処理を実装。
- Kconfig (`MIPI_PHY_POWER_ENABLE`, `MIPI_PHY_LDO_CHAN`, `MIPI_PHY_LDO_MV`) を追加し、既定は無効、チャンネル3、2500mVに設定。
- `main.c` にて、設定が有効な場合にのみTC358743の初期化前に `mipi_phy_power_enable()` を呼び出すように変更。
- ホストテストを作成し、境界値を含む検証ロジックをテスト。

## 根拠
- `esp_ldo_acquire_channel` などのAPI構造体および挙動は、ESP-IDFのソース(`components/esp_hw_support/ldo/include/esp_ldo_regulator.h`, `components/esp_hw_support/ldo/esp_ldo_regulator.c`)で確認。同一チャンネルを複数ユーザーが異なる設定で取得しようとしたり、調節可能フラグ(`adjustable`)が異なるとエラーになる仕様であることが確認済み。
- 戻り値もESP-IDFソースコードに準拠。

## オーナー向け実機確認手順
1. `MIPI_PHY_POWER_ENABLE` を `n` のままファームウェアを書き込む。
2. `VDD_MIPI_DPHY` のノード（U6 のピン 41 付近の C63 / C64）の電圧を測り、0V 付近であることを確認する。
3. Kconfig で `MIPI_PHY_POWER_ENABLE` を `y` にしてファームウェアをビルド・書き込む。
4. 同じノードの電圧を測り、2.5V 付近（2.25〜2.75V）であることを確認する。
5. 電圧が範囲外または不安定な場合は、すぐに Kconfig で設定を無効に戻す。
※ケーブル（CSI）は接続しない状態で行うこと。

## 未確認一覧
- rev v1.3 への電圧の適用 (2.25/2.5/2.75Vが正しいか) `TODO(hw-verify)`
- `ESP_LDO_VO3` に他の負荷が無いこと (回路図上はU6ピン41とピン73のみ) `TODO(hw-verify)`
- LDOの最大電流 (データシート上50mA) に対して問題ないか `TODO(hw-verify)`
- LDOを有効にした後の立ち上がり時間の待ちが必要か `TODO(hw-verify)`

## 変異テスト結果
| 変更 | 落ちたテスト名 |
|---|---|
| `chan_id != 3` を `chan_id != 2` に変更 | `test_mipi_phy_power_validate_invalid_chan`, `test_mipi_phy_power_validate_valid` |
| `voltage_mv < 2250` を `<= 2250` に変更 | `test_mipi_phy_power_validate_valid` |
| `voltage_mv > 2750` の上限チェックを削除 | `test_mipi_phy_power_validate_invalid_voltage` |
| 戻り値を `return 0;` から `return 1;` に変更 | すべてのテスト |
