#pragma once

/*
 * kvm_service: kvm_ctrl（純 C）を FreeRTOS のタスク + ミューテックスで動かす IDF 専用の接着。docs/INTERFACES.md 6。
 *
 *   - kvm_ctrl_t を 1 つ持ち、tick / select / power / get_status を 1 つのミューテックスで直列化する。
 *   - 周期タスク（Kconfig KVM_TICK_MS、既定 20 ms）が kvm_ctrl_tick() を回す。
 *   - kvm_api（HTTP API の純 C）に渡す backend を返す（kvm_service_backend）。
 *
 * 起動: **kvm_service_start() の呼び出し元は A-9（Antigravity、実機の kvm_board_hal_t の実装）**。
 * 実機の HAL が無い間は main.c から呼ばない。呼ばれていない間、web_server の KVM / ATX の API は
 * 503 {"error":"not_ready"} を返す（kvm_service_backend() の ready が false）。
 */

#include "esp_err.h"
#include "kvm_board.h"
#include "kvm_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * KVM / ATX のサービスを起動する（1 回だけ）。
 *  - ミューテックスを作り、kvm_ctrl_init(hal) を呼び（release_all -> 全線 High のまま set_addr）、tick のタスクを作る。
 *  - hal の中身（関数ポインタ）はコピーされる。hal->ctx が指すものは、起動後もずっと有効であること。
 *  - 設定は Kconfig（KVM_*、ATX_*）から作る。パルス長や間隔はクライアントが決められない。
 * @return ESP_OK: 起動した（kvm_ctrl_init が失敗して**フォールトがラッチされた場合も ESP_OK**: API は 200 の status で
 *         "fault":true を返し、操作は 503 hw_fault になる。再起動まで解除しない）。
 *         ESP_ERR_INVALID_ARG: hal が NULL、関数ポインタの欠落、設定の不備（Kconfig の値がハード上限を超えている等）。
 *         ESP_ERR_INVALID_STATE: すでに起動している。
 *         ESP_ERR_NO_MEM: ミューテックスかタスクを作れなかった。
 * 失敗したときは、サービスは起動していない（API は not_ready のまま）。
 */
esp_err_t kvm_service_start(const kvm_board_hal_t *hal);

/**
 * kvm_api_handle() に渡す backend。常に有効なポインタを返す（起動前でも）。起動前は ready() が false を返し、
 * lock も失敗する（API は 503 not_ready）。ロックは Kconfig KVM_LOCK_TIMEOUT_MS までしか待たない。
 */
const kvm_api_backend_t *kvm_service_backend(void);

#ifdef __cplusplus
}
#endif
