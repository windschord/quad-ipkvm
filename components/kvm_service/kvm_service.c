#include "kvm_service.h"
#include "sdkconfig.h"
#include "kvm_ctrl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "kvm_service";

/* Kconfig の範囲は main/Kconfig.projbuild。ここは組み合わせの検査だけ */
_Static_assert(CONFIG_KVM_ASSERT_MARGIN_MS > CONFIG_KVM_TICK_MS,
               "KVM_ASSERT_MARGIN_MS must exceed KVM_TICK_MS, or the board timer may cut a normal pulse");

static kvm_ctrl_t s_ctrl;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static volatile bool s_ready; /* start が全部済んでから true にする。backend の ready() が読む */
static bool s_fault_logged;

/* ------------------------------------------------------------------------------------------------
 * backend（kvm_api が使う）。lock を持っている間だけ kvm_ctrl を触る
 * ---------------------------------------------------------------------------------------------- */

static bool be_ready(void *ctx) {
    (void)ctx;
    return s_ready;
}

/* HTTP のハンドラ(httpd のタスク)から呼ばれる。長くブロックしない。取れなければ false（-> 503 not_ready） */
static bool be_lock(void *ctx) {
    (void)ctx;
    if (!s_ready || s_mutex == NULL) return false;
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(CONFIG_KVM_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void be_unlock(void *ctx) {
    (void)ctx;
    xSemaphoreGive(s_mutex);
}

static kvm_ctrl_result_t be_get_status(void *ctx, kvm_ctrl_status_t *status) {
    (void)ctx;
    return kvm_ctrl_get_status(&s_ctrl, status);
}

static kvm_ctrl_result_t be_select(void *ctx, uint8_t channel, uint32_t *op_id) {
    (void)ctx;
    return kvm_ctrl_select(&s_ctrl, channel, op_id);
}

static kvm_ctrl_result_t be_power(void *ctx, uint8_t channel, kvm_ctrl_power_action_t action, uint32_t *op_id) {
    (void)ctx;
    return kvm_ctrl_power(&s_ctrl, channel, action, op_id);
}

static const kvm_api_backend_t s_backend = {
    .ctx = NULL,
    .lock = be_lock,
    .unlock = be_unlock,
    .ready = be_ready,
    .get_status = be_get_status,
    .select = be_select,
    .power = be_power,
};

const kvm_api_backend_t *kvm_service_backend(void) {
    return &s_backend;
}

/* ------------------------------------------------------------------------------------------------
 * tick のタスク
 * ---------------------------------------------------------------------------------------------- */

static void kvm_task(void *arg) {
    (void)arg;
    TickType_t period = pdMS_TO_TICKS(CONFIG_KVM_TICK_MS);
    if (period == 0) period = 1; /* FreeRTOS の tick が粗い設定でも止まらないように */
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, period);

        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        kvm_ctrl_result_t r = kvm_ctrl_tick(&s_ctrl, now_ms);
        kvm_ctrl_status_t st;
        bool have_status = false;
        if (r == KVM_CTRL_ERR_FAULT && !s_fault_logged) {
            have_status = (kvm_ctrl_get_status(&s_ctrl, &st) == KVM_CTRL_OK);
        }
        xSemaphoreGive(s_mutex);

        /* フォールトのラッチは 1 回だけ記録する（以後の tick で溢れさせない）。ログはミューテックスの外 */
        if (r == KVM_CTRL_ERR_FAULT && !s_fault_logged) {
            s_fault_logged = true;
            ESP_LOGE(TAG, "board fault latched (reason=%d): all lines released, KVM/ATX requests now answer hw_fault until reboot",
                     have_status ? (int)st.fault_reason : -1);
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * 起動
 * ---------------------------------------------------------------------------------------------- */

esp_err_t kvm_service_start(const kvm_board_hal_t *hal) {
    if (hal == NULL) return ESP_ERR_INVALID_ARG;
    if (s_ready || s_mutex != NULL) return ESP_ERR_INVALID_STATE;

    /* 既定値から始めて、Kconfig の値で上書きする。すべて TODO(hw-verify)。
     * 範囲（ハード上限）の検査は kvm_ctrl_init の中（上限を超えるとき INVALID_PARAM）。 */
    kvm_ctrl_config_t cfg;
    kvm_ctrl_config_default(&cfg);
    cfg.press_ms = CONFIG_ATX_PRESS_MS;
    cfg.reset_ms = CONFIG_ATX_RESET_MS;
    cfg.force_ms = CONFIG_ATX_FORCE_MS;
    cfg.atx_min_interval_ms = CONFIG_ATX_MIN_INTERVAL_MS;
    cfg.assert_margin_ms = CONFIG_KVM_ASSERT_MARGIN_MS;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    /* タスクを作る前なので、ロックなしで初期化してよい（ready は false のまま） */
    kvm_ctrl_result_t r = kvm_ctrl_init(&s_ctrl, &cfg, hal);
    if (r == KVM_CTRL_ERR_INVALID_PARAM) {
        ESP_LOGE(TAG, "kvm_ctrl_init rejected the configuration or the HAL");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_INVALID_ARG;
    }
    if (r == KVM_CTRL_ERR_FAULT) {
        /* ctrl は使える状態でフォールトがラッチされている。API は fault を報告し、操作は hw_fault になる */
        ESP_LOGE(TAG, "board initialization failed: fault latched at start, all lines released");
        s_fault_logged = true;
    }

    if (xTaskCreate(kvm_task, "kvm_service", CONFIG_KVM_TASK_STACK_SIZE, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the kvm_service task");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_fault_logged = false;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "started (tick %d ms, press %d / reset %d / force %d ms, cooldown %d ms)", CONFIG_KVM_TICK_MS,
             CONFIG_ATX_PRESS_MS, CONFIG_ATX_RESET_MS, CONFIG_ATX_FORCE_MS, CONFIG_ATX_MIN_INTERVAL_MS);
    return ESP_OK;
}
