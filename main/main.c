#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_NAMESPACE "wifi"
#define WIFI_KEY_SSID "ssid"
#define WIFI_KEY_PASS "pass"

#define DEFAULT_AP_SSID "ESP32C2-SETUP"
#define DEFAULT_AP_PASS "12345678"

#define CONFIG_BUTTON_GPIO GPIO_NUM_0

#define UART_PORT UART_NUM_1
#define UART_TX_GPIO GPIO_NUM_4
#define UART_RX_GPIO GPIO_NUM_5
#define UART_BAUD_RATE 115200

#define TCP_SERVER_PORT 12345
#define UDP_DISCOVERY_PORT 3333

static const char *TAG = "uart_tcp_bridge";

static httpd_handle_t http_server;
static esp_netif_t *sta_netif;
static TaskHandle_t udp_task_handle;
static TaskHandle_t tcp_task_handle;

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t i = 0; src[i] != '\0' && di + 1 < dst_len; i++) {
        if (src[i] == '+') {
            dst[di++] = ' ';
            continue;
        }
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
            continue;
        }
        dst[di++] = src[i];
    }
    dst[di] = '\0';
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, WIFI_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool load_wifi_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }
    size_t saved_ssid_len = ssid_len;
    size_t saved_pass_len = pass_len;
    err = nvs_get_str(nvs, WIFI_KEY_SSID, ssid, &saved_ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    err = nvs_get_str(nvs, WIFI_KEY_PASS, pass, &saved_pass_len);
    nvs_close(nvs);
    return err == ESP_OK;
}

static void erase_wifi_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static esp_err_t http_root_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'/>"
        "<title>ESP32-C2 配网</title></head><body>"
        "<h2>ESP32-C2 配网</h2>"
        "<form method='POST' action='/configure'>"
        "SSID: <input name='ssid' length=32 /><br/>"
        "密码: <input name='password' type='password' length=64 /><br/>"
        "<button type='submit'>保存</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_configure_handler(httpd_req_t *req)
{
    char content[128] = { 0 };
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
    }
    char *ssid_key = strstr(content, "ssid=");
    char *pass_key = strstr(content, "password=");
    if (!ssid_key || !pass_key) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
    }
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    char *ssid_end = strchr(ssid_key, '&');
    if (ssid_end) {
        *ssid_end = '\0';
    }
    url_decode(ssid, sizeof(ssid), ssid_key + 5);
    url_decode(pass, sizeof(pass), pass_key + 9);
    if (ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid empty");
    }
    esp_err_t err = save_wifi_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save wifi failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    httpd_resp_sendstr(req, "saved, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    if (httpd_start(&http_server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = http_root_handler,
            .user_ctx = NULL
        };
        httpd_uri_t configure = {
            .uri = "/configure",
            .method = HTTP_POST,
            .handler = http_configure_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(http_server, &root);
        httpd_register_uri_handler(http_server, &configure);
    }
}

static void stop_http_server(void)
{
    if (http_server) {
        httpd_stop(http_server);
        http_server = NULL;
    }
}

static void start_softap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = DEFAULT_AP_SSID,
            .password = DEFAULT_AP_PASS,
            .ssid_len = strlen(DEFAULT_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4
        }
    };
    if (strlen(DEFAULT_AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    start_http_server();
    ESP_LOGI(TAG, "AP started: SSID=%s", DEFAULT_AP_SSID);
}

static void udp_discovery_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(UDP_DISCOVERY_PORT)
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "UDP bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UDP discovery server started on %d", UDP_DISCOVERY_PORT);
    while (true) {
        char rx_buf[64];
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "UDP recv failed: %d", errno);
            continue;
        }
        rx_buf[len] = '\0';
        if (strcmp(rx_buf, "DISCOVER") == 0) {
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(sta_netif, &ip_info);
            char payload[64];
            snprintf(payload, sizeof(payload), "IP:%s", ip4addr_ntoa(&ip_info.ip));
            sendto(sock, payload, strlen(payload), 0, (struct sockaddr *)&source_addr, socklen);
        }
    }
}

static void tcp_bridge_task(void *arg)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "TCP socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TCP_SERVER_PORT)
    };
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "TCP bind failed: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    listen(listen_sock, 1);
    ESP_LOGI(TAG, "TCP server listening on %d", TCP_SERVER_PORT);
    while (true) {
        struct sockaddr_in6 source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "TCP accept failed: %d", errno);
            continue;
        }
        ESP_LOGI(TAG, "TCP client connected");
        while (true) {
            uint8_t uart_data[256];
            int uart_len = uart_read_bytes(UART_PORT, uart_data, sizeof(uart_data), pdMS_TO_TICKS(20));
            if (uart_len > 0) {
                int sent = send(sock, uart_data, uart_len, 0);
                if (sent < 0) {
                    ESP_LOGE(TAG, "TCP send failed: %d", errno);
                    break;
                }
            }
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(sock, &read_fds);
            struct timeval tv = { 0, 20000 };
            int sel = select(sock + 1, &read_fds, NULL, NULL, &tv);
            if (sel > 0 && FD_ISSET(sock, &read_fds)) {
                uint8_t rx_buf[256];
                int len = recv(sock, rx_buf, sizeof(rx_buf), 0);
                if (len <= 0) {
                    ESP_LOGI(TAG, "TCP client disconnected");
                    break;
                }
                uart_write_bytes(UART_PORT, rx_buf, len);
            }
        }
        shutdown(sock, 0);
        close(sock);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, starting servers");
        if (!udp_task_handle) {
            xTaskCreate(udp_discovery_task, "udp_discovery", 4096, NULL, 5, &udp_task_handle);
        }
        if (!tcp_task_handle) {
            xTaskCreate(tcp_bridge_task, "tcp_bridge", 4096, NULL, 5, &tcp_task_handle);
        }
    }
}

static void button_task(void *arg)
{
    const int hold_time_ms = 3000;
    int pressed_ms = 0;
    while (true) {
        if (gpio_get_level(CONFIG_BUTTON_GPIO) == 0) {
            pressed_ms += 100;
            if (pressed_ms >= hold_time_ms) {
                ESP_LOGI(TAG, "Factory reset triggered");
                erase_wifi_credentials();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            pressed_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, UART_TX_GPIO, UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    init_uart();

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    xTaskCreate(button_task, "factory_reset", 2048, NULL, 5, NULL);

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t sta_config = { 0 };
        strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
        strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());
        ESP_LOGI(TAG, "Connecting to SSID=%s", ssid);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials, entering AP config mode");
        start_softap();
    }
}
