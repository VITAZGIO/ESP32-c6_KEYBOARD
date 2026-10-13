/*
 * Режим прошивки по воздуху.
 * Плата поднимает Wi-Fi и HTTP-сервер, PlatformIO заливает прошивку
 * командой curl на  http://<IP>/update  (см. platformio.ini).
 * Zigbee в этом режиме не запускается.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "config.h"
#include "secrets.h"
#include "ota.h"

static const char *TAG = "OTA";

#define NVS_NS  "aula"
#define NVS_KEY "ota"

// ------------------------------------------------------------
// Флаг «после перезагрузки уйти в OTA»
// ------------------------------------------------------------

static void set_flag(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool ota_requested(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    return v != 0;
}

void ota_reboot_into(void)
{
    ESP_LOGW(TAG, "перезагрузка в режим прошивки по воздуху");
    set_flag(1);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

// ------------------------------------------------------------
// Wi-Fi
// ------------------------------------------------------------

#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1
static EventGroupHandle_t s_ev;
static int s_retries = 0;

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "нет связи с роутером, причина %d", d->reason);
        if (++s_retries < 10) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static bool wifi_up(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

#if OTA_STATIC_IP
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ipaddr_addr(OTA_IP);
    ip.gw.addr = ipaddr_addr(OTA_GATEWAY);
    ip.netmask.addr = ipaddr_addr(OTA_NETMASK);
    esp_netif_set_ip_info(netif, &ip);
#endif

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL);

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, WIFI_SSID, sizeof(wcfg.sta.ssid));
    strncpy((char *)wcfg.sta.password, WIFI_PASS, sizeof(wcfg.sta.password));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();
    // от батареи пиковый ток Wi-Fi на полной мощности (~20 дБм) просаживает питание
    // и чип сбрасывается посреди заливки. 11 дБм на комнату хватает с запасом
    esp_wifi_set_max_tx_power(44);   // единицы 0.25 дБм

    ESP_LOGI(TAG, "подключаюсь к %s", WIFI_SSID);
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
#if OTA_STATIC_IP
    // со статическим IP событие GOT_IP приходит не всегда — проверяем связь сами
    if (!(b & BIT_GOT_IP) && !(b & BIT_FAILED)) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) b |= BIT_GOT_IP;
    }
#endif
    return (b & BIT_GOT_IP) != 0;
}

// ------------------------------------------------------------
// HTTP: приём прошивки
// ------------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "AulaKeys: жду прошивку на POST /update\n");
}

static esp_err_t update_post(httpd_req_t *req)
{
    char pass[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Password", pass, sizeof(pass)) != ESP_OK ||
        strcmp(pass, OTA_PASSWORD) != 0) {
        ESP_LOGW(TAG, "неверный пароль");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad password");
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!part || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "приём прошивки, %d байт -> %s", req->content_len, part->label);

    static char buf[4096];
    int left = req->content_len, done = 0, last_pct = -1, timeouts = 0;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0 || esp_ota_write(h, buf, n) != ESP_OK) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "обрыв приёма");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }
        timeouts = 0;
        left -= n;
        done += n;
        int pct = (int)((int64_t)done * 100 / req->content_len);
        if (pct / 10 != last_pct / 10) {
            ESP_LOGI(TAG, "%d%%", pct);
            last_pct = pct;
        }
    }

    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        ESP_LOGE(TAG, "прошивка битая, не ставлю");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK, перезагрузка\n");
    ESP_LOGI(TAG, "готово, перезагрузка");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ------------------------------------------------------------

void ota_run(void)
{
    ESP_LOGW(TAG, "=== РЕЖИМ ПРОШИВКИ ПО ВОЗДУХУ ===");
    // OTA_WITH_SEQUENTIAL_WRITES: флеш стирается по ходу записи, а не весь раздел
    // сразу (иначе 2 МБ стираются ~10 с, приём стоит и соединение рвётся)
    set_flag(0);   // снимаем сразу: если что-то сломается, следующий старт обычный

    if (!wifi_up()) {
        ESP_LOGE(TAG, "Wi-Fi не поднялся, возврат в Zigbee");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.recv_wait_timeout = 30;
    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &hcfg));
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t upd = {.uri = "/update", .method = HTTP_POST, .handler = update_post};
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &upd);

    ESP_LOGI(TAG, "жду прошивку 10 минут: Upload в окружении esp32-c6-ota");
    vTaskDelay(pdMS_TO_TICKS(OTA_TIMEOUT_MS));
    ESP_LOGW(TAG, "время вышло, возврат в Zigbee");
    esp_restart();
}
