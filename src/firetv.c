/*
 * Fire TV remote client. Port of firetv_ctrl's firetv.c: the same
 * undocumented remote API (port 8080, DIAL wake on 8009, X-Api-Key), but
 * HTTP is done with esp_http_client instead of curl.
 *
 * TLS certificate verification is skipped (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
 * in sdkconfig.defaults). The token is kept in NVS instead of a file.
 */

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "firetv.h"
#include "secrets.h"
#include "wifi_sta.h"

#define API_KEY        "0987654321"   /* fixed key used by the Fire TV remote app */
#define TOKEN_MAX      128
#define RESP_MAX       512
#define ERR_MAX        160
#define QUEUE_LEN      16
#define NVS_NAMESPACE  "firetv"
#define NVS_KEY        "token"

static const char *TAG = "firetv";

static QueueHandle_t s_queue;
static char          s_token[TOKEN_MAX];
static bool          s_have_token = false;

/* Sends one request. Returns 0 on HTTP 2xx, -1 otherwise. The response body (if any)
 * is copied to resp; err gets a message on failure. */
/* Persistent client for authenticated control requests. Reusing it keeps the TLS
 * session open between key presses, so only the first press (or the first one
 * after the TV closes the idle connection) pays for a TLS handshake. */
static esp_http_client_handle_t s_ctl;

static void ctl_reset(void)
{
    if (s_ctl) {
        esp_http_client_cleanup(s_ctl);
        s_ctl = NULL;
    }
}

static int ctl_request(const char *url, const char *body, char *err, size_t err_size)
{
    if (!s_ctl) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 5000,
            .keep_alive_enable = true,
            .skip_cert_common_name_check = true,
        };
        s_ctl = esp_http_client_init(&cfg);
        if (!s_ctl) {
            snprintf(err, err_size, "http client init failed");
            return -1;
        }
        esp_http_client_set_header(s_ctl, "X-Api-Key", API_KEY);
        esp_http_client_set_header(s_ctl, "user-agent", "okhttp/4.10.0");
        esp_http_client_set_header(s_ctl, "X-Client-Token", s_token);
    }
    esp_http_client_set_url(s_ctl, url);
    esp_http_client_set_method(s_ctl, HTTP_METHOD_POST);
    if (body) {
        esp_http_client_set_header(s_ctl, "Content-Type", "application/json; charset=utf-8");
        esp_http_client_set_post_field(s_ctl, body, (int) strlen(body));
    } else {
        esp_http_client_delete_header(s_ctl, "Content-Type");
        esp_http_client_set_post_field(s_ctl, NULL, 0);
    }
    esp_err_t e = esp_http_client_perform(s_ctl);
    int status = esp_http_client_get_status_code(s_ctl);
    if (e != ESP_OK) {
        snprintf(err, err_size, "request failed: %s", esp_err_to_name(e));
        ctl_reset();   /* connection is stale; next call reconnects */
        return -1;
    }
    if (status < 200 || status >= 300) {
        snprintf(err, err_size, "HTTP %d from %s", status, url);
        ctl_reset();
        return -1;
    }
    return 0;
}

static int http_request(const char *method, const char *url, const char *token,
                        const char *body, char *resp, size_t resp_size,
                        char *err, size_t err_size)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .skip_cert_common_name_check = true,   /* see sdkconfig.defaults */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(err, err_size, "http client init failed for %s", url);
        return -1;
    }

    esp_http_client_set_method(client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST
                                                                    : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Api-Key", API_KEY);
    esp_http_client_set_header(client, "user-agent", "okhttp/4.10.0");
    if (token) {
        esp_http_client_set_header(client, "X-Client-Token", token);
    }
    int body_len = body ? (int) strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    }

    if (esp_http_client_open(client, body_len) != ESP_OK) {
        snprintf(err, err_size, "no response from %s (is the TV awake?)", url);
        esp_http_client_cleanup(client);
        return -1;
    }
    if (body_len > 0 && esp_http_client_write(client, body, body_len) < 0) {
        snprintf(err, err_size, "write failed to %s", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char buf[RESP_MAX];
    int  n = esp_http_client_read_response(client, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (resp && resp_size > 0) {
        snprintf(resp, resp_size, "%s", buf);
    }
    if (status < 200 || status >= 300) {
        snprintf(err, err_size, "HTTP %d from %s: %.200s", status, url, buf);
        return -1;
    }
    return 0;
}

static int ftv_wake(char *err, size_t err_size)
{
    char url[96];
    snprintf(url, sizeof(url), "http://%s:8009/apps/FireTVRemote", FIRETV_HOST);
    return http_request("POST", url, NULL, NULL, NULL, 0, err, err_size);
}

static int ftv_pin_display(char *err, size_t err_size)
{
    char url[96], body[96];
    snprintf(url, sizeof(url), "https://%s:8080/v1/FireTV/pin/display", FIRETV_HOST);
    snprintf(body, sizeof(body), "{\"friendlyName\":\"%s\"}", "arcx ring");
    return http_request("POST", url, NULL, body, NULL, 0, err, err_size);
}

static int ftv_pin_verify(const char *pin, char *token, size_t token_size,
                          char *err, size_t err_size)
{
    char url[96], body[64], resp[RESP_MAX];
    snprintf(url, sizeof(url), "https://%s:8080/v1/FireTV/pin/verify", FIRETV_HOST);
    snprintf(body, sizeof(body), "{\"pin\":\"%s\"}", pin);
    if (http_request("POST", url, NULL, body, resp, sizeof(resp), err, err_size) != 0) {
        return -1;
    }

    /* Response: {"description":"<token>"} */
    const char *p = strstr(resp, "\"description\"");
    p = p ? strchr(p + 13, '"') : NULL;
    const char *q = p ? strchr(p + 1, '"') : NULL;
    if (!q || q == p + 1 || (size_t)(q - p - 1) >= token_size) {
        snprintf(err, err_size, "unexpected pin/verify response: %.200s", resp);
        return -1;
    }
    memcpy(token, p + 1, (size_t)(q - p - 1));
    token[q - p - 1] = '\0';
    return 0;
}

static int ftv_send(const char *action, char *err, size_t err_size)
{
    char url[200];

    if (strcmp(action, "play") == 0) {
        snprintf(url, sizeof(url), "https://%s:8080/v1/media?action=play", FIRETV_HOST);
        return ctl_request(url, NULL, err, err_size);
    }
    if (strcmp(action, "scan_forward") == 0 || strcmp(action, "scan_back") == 0) {
        snprintf(url, sizeof(url), "https://%s:8080/v1/media?action=scan", FIRETV_HOST);
        const char *body = strcmp(action, "scan_forward") == 0
            ? "{\"direction\":\"forward\",\"keyAction\":{\"keyActionType\":\"keyDown\"}}"
            : "{\"direction\":\"back\",\"keyAction\":{\"keyActionType\":\"keyDown\"}}";
        return ctl_request(url, body, err, err_size);
    }
    snprintf(url, sizeof(url), "https://%s:8080/v1/FireTV?action=%s", FIRETV_HOST, action);
    return ctl_request(url, NULL, err, err_size);
}

/* ---- token storage (NVS) ---- */

static bool token_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_token);
    esp_err_t e = nvs_get_str(h, NVS_KEY, s_token, &len);
    nvs_close(h);
    return e == ESP_OK && s_token[0] != '\0';
}

static esp_err_t token_save(const char *token)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = nvs_set_str(h, NVS_KEY, token);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

/* ---- pairing: PIN is typed on the serial console ---- */

/* Read a line straight from the console UART. stdin/fgets returns EOF at once
 * when the UART driver is not installed, so install it and echo characters
 * ourselves (terminals like picocom do not echo locally). Accepts CR or LF. */
static int console_read_line(const char *prompt, char *buf, size_t size)
{
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    size_t n = 0;

    if (!uart_is_driver_installed(port) &&
        uart_driver_install(port, 256, 0, 0, NULL, 0) != ESP_OK) {
        return -1;
    }
    uart_flush_input(port);
    printf("\r\n%s", prompt);
    fflush(stdout);

    for (;;) {
        uint8_t c;
        if (uart_read_bytes(port, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n > 0) {
                break;
            }
            continue;   /* ignore empty lines / the LF of a CRLF */
        }
        if ((c == 0x7f || c == '\b') && n > 0) {
            n--;
            printf("\b \b");
        } else if (c >= ' ' && c < 0x7f && n + 1 < size) {
            buf[n++] = (char)c;
            putchar(c);
        }
        fflush(stdout);
    }
    buf[n] = '\0';
    printf("\r\n");
    return (int)n;
}

static void pair_interactive(void)
{
    char err[ERR_MAX], pin[16], token[TOKEN_MAX];

    ESP_LOGI(TAG, "no token stored; pairing with Fire TV at %s", FIRETV_HOST);
    ftv_wake(err, sizeof(err));   /* best effort; the port may already be open */

    /* The remote service can take a few seconds to start listening after wake. */
    int rc = -1;
    for (int i = 0; i < 8 && rc != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        rc = ftv_pin_display(err, sizeof(err));
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "could not start pairing: %s", err);
        return;
    }

    if (console_read_line("Enter the PIN shown on the TV: ", pin, sizeof(pin)) <= 0) {
        ESP_LOGE(TAG, "no PIN entered");
        return;
    }

    if (ftv_pin_verify(pin, token, sizeof(token), err, sizeof(err)) != 0) {
        ESP_LOGE(TAG, "pairing failed: %s", err);
        return;
    }
    if (token_save(token) != ESP_OK) {
        ESP_LOGE(TAG, "paired, but could not save the token to NVS");
    }
    snprintf(s_token, sizeof(s_token), "%s", token);
    s_have_token = true;
    ESP_LOGI(TAG, "paired with Fire TV; token saved");
}

static void firetv_task(void *arg)
{
    /* Nothing can be sent until WiFi has an address. */
    while (!wifi_sta_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!s_have_token) {
        pair_interactive();
    }

    for (;;) {
        const char *action = NULL;
        if (xQueueReceive(s_queue, &action, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_have_token || !wifi_sta_connected()) {
            ESP_LOGW(TAG, "not ready (paired=%d wifi=%d), dropping %s",
                     s_have_token, wifi_sta_connected(), action);
            continue;
        }

        char err[ERR_MAX];
        if (ftv_send(action, err, sizeof(err)) != 0) {
            /* The remote service may have stopped while the TV idled: wake and retry. */
            ftv_wake(err, sizeof(err));
            vTaskDelay(pdMS_TO_TICKS(500));
            if (ftv_send(action, err, sizeof(err)) != 0) {
                ESP_LOGE(TAG, "%s failed: %s", action, err);
                continue;
            }
        }
        ESP_LOGI(TAG, "sent %s", action);
    }
}

esp_err_t firetv_init(void)
{
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(const char *));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    s_have_token = token_load();
    if (s_have_token) {
        ESP_LOGI(TAG, "token loaded from NVS");
    }
    BaseType_t ok = xTaskCreate(firetv_task, "firetv", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void firetv_queue_action(const char *action)
{
    /* Dropped if the queue is full; BLE callbacks must not block. */
    xQueueSend(s_queue, &action, 0);
}
