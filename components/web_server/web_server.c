#include "sdkconfig.h"
#include "web_server.h"
#include "esp_log.h"
#include "esp_https_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "web_cert.h"
#include "auth_logic.h"
#include "http_guard.h"
#include "web_status_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <string.h>
#include "auth_crypto_mbedtls.h"

static const char *TAG = "web_server";

static auth_logic_t s_auth_ctx;
static SemaphoreHandle_t s_auth_mutex;
static SemaphoreHandle_t s_pbkdf2_mutex; // For try-lock behavior

typedef struct {
    char **cert_pem;
    size_t *cert_len;
    char **key_pem;
    size_t *key_len;
    esp_err_t result;
    SemaphoreHandle_t done_sem;
} cert_task_args_t;

static void cert_task(void *pvParameters) {
    cert_task_args_t *args = (cert_task_args_t *)pvParameters;
    args->result = web_cert_load_or_create(args->cert_pem, args->cert_len, args->key_pem, args->key_len);
    ESP_LOGI(TAG, "cert task stack high water mark: %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(args->done_sem);
    vTaskDelete(NULL);
}

static esp_err_t load_auth_state(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("auth", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = 0;
    err = nvs_get_blob(handle, "state", NULL, &len);
    if (err == ESP_OK && len > 0) {
        uint8_t *buf = malloc(len);
        if (buf) {
            err = nvs_get_blob(handle, "state", buf, &len);
            if (err == ESP_OK) {
                if (auth_logic_load_state(&s_auth_ctx, buf, len) != 0) {
                    err = ESP_FAIL;
                }
            }
            free(buf);
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_auth_state(void) {
    uint8_t buf[128]; // Enough for AUTH_LOGIC_SERIALIZED_LEN
    size_t len = 0;
    if (auth_logic_serialize_state(&s_auth_ctx, buf, sizeof(buf), &len) != 0) {
        return ESP_FAIL;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("auth", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, "state", buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void add_security_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

static uint32_t get_client_ip(httpd_req_t *req) {
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_INET) {
            return ntohl(((struct sockaddr_in *)&addr)->sin_addr.s_addr);
        } else if (addr.ss_family == AF_INET6) {
            // Fold IPv4-mapped IPv6
            struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&addr;
            if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
                uint32_t v4;
                memcpy(&v4, &in6->sin6_addr.s6_addr[12], 4);
                return ntohl(v4);
            }
            // Fallback fold using FNV-1a
            uint32_t hash = 2166136261u;
            for (int i=0; i<16; i++) {
                hash ^= in6->sin6_addr.s6_addr[i];
                hash *= 16777619u;
            }
            return hash;
        }
    }
    return 0;
}

static bool check_host_origin(httpd_req_t *req, bool is_post) {
    char host_buf[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host_buf, sizeof(host_buf)) != ESP_OK) {
        return false;
    }
    char config_hosts[] = CONFIG_WEB_SERVER_ALLOWED_HOSTS;
    const char *allowed_hosts[8]; // Max 8 hosts
    size_t allowed_count = 0;

    char *saveptr;
    char *token = strtok_r(config_hosts, ",", &saveptr);
    while (token != NULL && allowed_count < 8) {
        // Skip leading spaces
        while (*token == ' ') token++;
        allowed_hosts[allowed_count++] = token;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (!http_guard_host_ok(host_buf, strlen(host_buf), allowed_hosts, allowed_count)) {
        return false;
    }

    if (is_post) {
        char origin_buf[128] = {0};
        if (httpd_req_get_hdr_value_str(req, "Origin", origin_buf, sizeof(origin_buf)) == ESP_OK) {
            if (!http_guard_origin_ok(origin_buf, strlen(origin_buf), host_buf, strlen(host_buf))) {
                return false;
            }
        } else {
            return false; // POST requires Origin for CSRF protection
        }
    }
    return true;
}

static bool get_session_token(httpd_req_t *req, uint8_t token[32]) {
    char cookie_buf[512] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, sizeof(cookie_buf)) == ESP_OK) {
        return (http_guard_extract_session_cookie(cookie_buf, strlen(cookie_buf), token) == 0);
    }
    return false;
}



static esp_err_t send_action_error(httpd_req_t *req, http_guard_action_e action) {
    int code = web_status_map_action_code(action);
    const char *msg = web_status_map_action_message(action);
    char buf[96];
    if (msg) {
        http_guard_format_error(msg, buf, sizeof(buf));
    } else {
        http_guard_format_error("error", buf, sizeof(buf));
    }

    const char *status_str = "500 Internal Server Error";
    switch(code) {
        case 401: status_str = "401 Unauthorized"; break;
        case 403: status_str = "403 Forbidden"; break;
        case 405:
            status_str = "405 Method Not Allowed";
            httpd_resp_set_hdr(req, "Allow", "GET, POST");
            break;
        case 409: status_str = "409 Conflict"; break;
        case 415: status_str = "415 Unsupported Media Type"; break;
        case 429: status_str = "429 Too Many Requests"; break;
        case 503: status_str = "503 Service Unavailable"; break;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status_str);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
    add_security_headers(req);

    uint8_t token[32];
    bool has_token = get_session_token(req, token);

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    bool valid = false;
    if (has_token) {
        valid = (auth_logic_validate_session(&s_auth_ctx, token, sizeof(token), esp_timer_get_time() / 1000) == 0);
    }
    auth_state_e state = s_auth_ctx.state;
    xSemaphoreGive(s_auth_mutex);

    bool host_ok = check_host_origin(req, false);
    http_guard_action_e action = http_guard_decide(state, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_STATUS, valid, host_ok, false, host_ok);

    if (action == HTTP_GUARD_ACTION_ALLOW) {
        char buf[256];
        http_guard_format_status(state, valid, buf, sizeof(buf));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, buf);
    } else {
        return send_action_error(req, action);
    }
    return ESP_OK;
}

static esp_err_t setup_handler(httpd_req_t *req) {
    add_security_headers(req);
    bool host_ok = check_host_origin(req, true);
    char ctype[64];
    bool is_json = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) == ESP_OK) {
        if (strncmp(ctype, "application/json", 16) == 0) is_json = true;
    }


    if (req->content_len == 0 || req->content_len > 512) {
        if (req->content_len > 512) {
            httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Payload Too Large");
        } else {
            httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "Length Required");
        }
        return ESP_OK;
    }

    char buf[513] = {0};
    size_t recv_all = 0;
    while (recv_all < req->content_len) {
        int ret = httpd_req_recv(req, buf + recv_all, req->content_len - recv_all);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        recv_all += ret;
    }
    int ret = recv_all;

    http_guard_json_setup_t parsed;
    if (http_guard_parse_json_password(buf, ret, &parsed) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_OK;
    }

    // Acquire PBKDF2 lock
    if (xSemaphoreTake(s_pbkdf2_mutex, 0) != pdTRUE) {
        http_guard_clear_json_setup(&parsed);
        httpd_resp_set_hdr(req, "Retry-After", "1");
        char buf[96];
        http_guard_format_error("server_busy", buf, sizeof(buf));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, buf);
        return ESP_OK;
    }

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);

    http_guard_action_e action = http_guard_decide(s_auth_ctx.state, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_SETUP, false, host_ok, is_json, host_ok);

    if (action != HTTP_GUARD_ACTION_ALLOW) {
        xSemaphoreGive(s_auth_mutex);
        xSemaphoreGive(s_pbkdf2_mutex);
        http_guard_clear_json_setup(&parsed);
        return send_action_error(req, action);
    }

    int auth_ret = auth_logic_setup_password(&s_auth_ctx, parsed.password, parsed.password_len);
    int status_code = web_status_map_setup(auth_ret, &s_auth_ctx);

    if (auth_ret == 0) {
        if (save_auth_state() != ESP_OK) {
            auth_logic_init(&s_auth_ctx, auth_crypto_mbedtls_get_interface(), &s_auth_ctx.config); // Reset to NEEDS_SETUP
            auth_ret = -1;
            status_code = 500;
        }
    }

    xSemaphoreGive(s_auth_mutex);
    xSemaphoreGive(s_pbkdf2_mutex);
    http_guard_clear_json_setup(&parsed);

    if (auth_ret == 0) {
        httpd_resp_sendstr(req, "{}");
    } else {
        if (status_code == 403) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Already setup");
        } else if (status_code == 400) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Error");
        }
    }
    return ESP_OK;
}

static esp_err_t login_handler(httpd_req_t *req) {
    add_security_headers(req);
    bool host_ok = check_host_origin(req, true);
    char ctype[64];
    bool is_json = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) == ESP_OK) {
        if (strncmp(ctype, "application/json", 16) == 0) is_json = true;
    }


    if (req->content_len == 0 || req->content_len > 512) {
        if (req->content_len > 512) {
            httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Payload Too Large");
        } else {
            httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "Length Required");
        }
        return ESP_OK;
    }

    char buf[513] = {0};
    size_t recv_all = 0;
    while (recv_all < req->content_len) {
        int ret = httpd_req_recv(req, buf + recv_all, req->content_len - recv_all);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        recv_all += ret;
    }
    int ret = recv_all;

    http_guard_json_setup_t parsed; // reusing setup struct for parsing single password
    if (http_guard_parse_json_password(buf, ret, &parsed) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_OK;
    }

    // Acquire PBKDF2 lock
    if (xSemaphoreTake(s_pbkdf2_mutex, 0) != pdTRUE) {
        http_guard_clear_json_setup(&parsed);
        httpd_resp_set_hdr(req, "Retry-After", "1");
        char buf[96];
        http_guard_format_error("server_busy", buf, sizeof(buf));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, buf);
        return ESP_OK;
    }

    uint32_t ip = get_client_ip(req);
    uint64_t now = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);

    http_guard_action_e action = http_guard_decide(s_auth_ctx.state, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_LOGIN, false, host_ok, is_json, host_ok);

    if (action != HTTP_GUARD_ACTION_ALLOW) {
        xSemaphoreGive(s_auth_mutex);
        xSemaphoreGive(s_pbkdf2_mutex);
        http_guard_clear_json_setup(&parsed);
        return send_action_error(req, action);
    }

    uint8_t token[32];
    int auth_ret = auth_logic_login(&s_auth_ctx, parsed.password, parsed.password_len, ip, now, token);
    int status_code = web_status_map_login(auth_ret, &s_auth_ctx, ip, now);

    // We must read retry-after while we have the lock if we wanted a precise one,
    // but 10s fallback or generic Retry-After is sufficient for now.

    xSemaphoreGive(s_auth_mutex);
    xSemaphoreGive(s_pbkdf2_mutex);
    http_guard_clear_json_setup(&parsed);

    if (auth_ret == 0) {
        char token_b64[64];
        size_t b64_len;
        http_guard_base64url_encode(token, sizeof(token), token_b64, sizeof(token_b64), &b64_len);

        char cookie_str[256];
        snprintf(cookie_str, sizeof(cookie_str), "%s=%s; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=%llu",
                 HTTP_GUARD_COOKIE_NAME, token_b64, (unsigned long long)(s_auth_ctx.config.session_absolute_timeout_ms / 1000));
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_str);
        httpd_resp_sendstr(req, "{}");
    } else {
        char buf[96];
        if (status_code == 429) {
            http_guard_format_error("rate_limited", buf, sizeof(buf));
            httpd_resp_set_hdr(req, "Retry-After", "10");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "429 Too Many Requests");
            httpd_resp_sendstr(req, buf);
        } else {
            http_guard_format_error("unauthorized", buf, sizeof(buf));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_sendstr(req, buf);
        }
    }
    return ESP_OK;
}

static esp_err_t logout_handler(httpd_req_t *req) {
    add_security_headers(req);
    bool host_ok = check_host_origin(req, true);

    uint8_t token[32];
    bool has_token = get_session_token(req, token);

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    http_guard_action_e action = http_guard_decide(s_auth_ctx.state, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_LOGOUT, has_token, host_ok, false, host_ok);

    if (action != HTTP_GUARD_ACTION_ALLOW) {
        xSemaphoreGive(s_auth_mutex);
        return send_action_error(req, action);
    }

    if (has_token) {
        auth_logic_logout(&s_auth_ctx, token, sizeof(token));
    }
    xSemaphoreGive(s_auth_mutex);

    // Expire cookie
    char cookie_str[128];
    snprintf(cookie_str, sizeof(cookie_str), "%s=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0", HTTP_GUARD_COOKIE_NAME);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_str);
    httpd_resp_sendstr(req, "{}");

    return ESP_OK;
}


esp_err_t web_server_start(void) {
    ESP_LOGI(TAG, "Starting web server...");

    s_auth_mutex = xSemaphoreCreateMutex();
    s_pbkdf2_mutex = xSemaphoreCreateMutex();

    auth_logic_config_t config;
    auth_logic_config_default(&config);
    config.pbkdf2_iterations = CONFIG_WEB_SERVER_PBKDF2_ITERATIONS;

    if (auth_logic_init(&s_auth_ctx, auth_crypto_mbedtls_get_interface(), &config) != 0) {
        ESP_LOGE(TAG, "Failed to initialize auth_logic");
        return ESP_FAIL;
    }

    esp_err_t err = load_auth_state();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to load auth state (fail-closed): %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.max_open_sockets = 7; // Max clients + 1 for status, etc.
    conf.httpd.lru_purge_enable = true;
    conf.httpd.stack_size = 10240; // 10KB to ensure PBKDF2/mbedTLS have enough space

    char *cert_pem = NULL;
    size_t cert_len = 0;
    char *key_pem = NULL;
    size_t key_len = 0;

    cert_task_args_t cert_args = {
        .cert_pem = &cert_pem,
        .cert_len = &cert_len,
        .key_pem = &key_pem,
        .key_len = &key_len,
        .result = ESP_FAIL,
        .done_sem = xSemaphoreCreateBinary()
    };

    if (!cert_args.done_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore for cert task");
        return ESP_FAIL;
    }

    if (xTaskCreate(cert_task, "web_cert", CONFIG_WEB_SERVER_CERT_TASK_STACK_SIZE, &cert_args, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create web_cert task");
        vSemaphoreDelete(cert_args.done_sem);
        return ESP_FAIL;
    }

    // Wait indefinitely for the task to finish to ensure it does not access out-of-scope variables
    xSemaphoreTake(cert_args.done_sem, portMAX_DELAY);
    esp_err_t cert_ret = cert_args.result;
    vSemaphoreDelete(cert_args.done_sem);

    if (cert_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load/create web certificate");
        return ESP_FAIL;
    }

    conf.servercert = (const uint8_t *)cert_pem;
    conf.servercert_len = cert_len;
    conf.prvtkey_pem = (const uint8_t *)key_pem;
    conf.prvtkey_len = key_len;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_ssl_start(&server, &conf);

    free(cert_pem);
    if (key_pem) {
        mbedtls_platform_zeroize(key_pem, key_len);
        free(key_pem);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTPS server!");
        return ret;
    }

    httpd_uri_t status_uri = { .uri = "/api/v1/auth/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    httpd_uri_t setup_uri = { .uri = "/api/v1/auth/setup", .method = HTTP_POST, .handler = setup_handler, .user_ctx = NULL };
    httpd_uri_t login_uri = { .uri = "/api/v1/auth/login", .method = HTTP_POST, .handler = login_handler, .user_ctx = NULL };
    httpd_uri_t logout_uri = { .uri = "/api/v1/auth/logout", .method = HTTP_POST, .handler = logout_handler, .user_ctx = NULL };

    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &setup_uri);
    httpd_register_uri_handler(server, &login_uri);
    httpd_register_uri_handler(server, &logout_uri);

    return ESP_OK;
}
