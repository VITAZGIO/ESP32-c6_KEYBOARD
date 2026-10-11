/*
 * Zigbee: 12 эндпоинтов «датчик присутствия» + батарея.
 * Устройство — спящий End Device: между событиями чип в light sleep,
 * но остаётся в сети, поэтому кнопки уходят без переподключения.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "config.h"
#include "zb.h"

static const char *TAG = "ZB";

static volatile bool s_joined = false;

// Обход из Arduino-обёртки: чтобы z2m видел устройство как батарейное.
extern void zb_set_ed_node_descriptor(bool power_src, bool rx_on_when_idle, bool alloc_addr);

// Опрос роутера (спящее устройство само забирает адресованные ему пакеты).
extern void zb_zdo_pim_set_long_poll_interval(uint64_t ms);

#define LONG_POLL_MS     3000     // опрос роутера во сне
#define AWAKE_AFTER_BOOT 10000    // после перезагрузки: 10 с радио не спит
#define AWAKE_AFTER_PAIR 10000    // после нового подключения: 10 с —
                                  // z2m успевает опросить и настроить плату

static volatile bool s_sleep_allowed = false;

static void allow_sleep_cb(uint8_t unused)
{
    // Честно становимся «спящим» устройством: приёмник выключен между опросами
    // роутера. Иначе радио ловит эфир и будит чип сотни раз в секунду.
    esp_zb_set_rx_on_when_idle(false);
    s_sleep_allowed = true;
    ESP_LOGI(TAG, "настройка из z2m позади — радио теперь спит между событиями");
}

static void on_joined(bool fresh_pairing)
{
    zb_zdo_pim_set_long_poll_interval(LONG_POLL_MS);
    s_sleep_allowed = false;
    esp_zb_scheduler_alarm((esp_zb_callback_t)allow_sleep_cb, 0,
                           fresh_pairing ? AWAKE_AFTER_PAIR : AWAKE_AFTER_BOOT);
    s_joined = true;
    app_on_zb_joined();
}

// ------------------------------------------------------------
// Описание эндпоинтов
// ------------------------------------------------------------

// ZCL-строка: первый байт — длина
static char *zcl_str(const char *s)
{
    static char buf[2][34];
    static int n = 0;
    char *b = buf[n++ & 1];
    size_t len = strlen(s);
    b[0] = (char)len;
    memcpy(b + 1, s, len);
    b[len + 1] = 0;
    return b;
}

static esp_zb_cluster_list_t *make_clusters(bool with_battery, uint8_t battery_pct)
{
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x03,   // батарея
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, zcl_str(ZB_MANUFACTURER));
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, zcl_str(ZB_MODEL));

    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_occupancy_sensing_cluster_cfg_t occ_cfg = {
        .occupancy = ESP_ZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_UNOCCUPIED,
        .sensor_type = ESP_ZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PIR,
        .sensor_type_bitmap = (1 << ESP_ZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PIR),
    };

    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(cl, esp_zb_identify_cluster_create(&identify_cfg),
                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, esp_zb_occupancy_sensing_cluster_create(&occ_cfg),
                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    if (with_battery) {
        uint8_t half_pct = (battery_pct > 100 ? 100 : battery_pct) * 2;   // ZCL: шаг 0.5 %
        esp_zb_attribute_list_t *pwr = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
        esp_zb_power_config_cluster_add_attr(pwr, ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                             &half_pct);
        esp_zb_cluster_list_add_power_config_cluster(cl, pwr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    }
    return cl;
}

static void add_ep(esp_zb_ep_list_t *list, uint8_t ep, bool with_battery, uint8_t battery_pct)
{
    esp_zb_endpoint_config_t cfg = {
        .endpoint = ep,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(list, make_clusters(with_battery, battery_pct), cfg);
}

// ------------------------------------------------------------
// Сигналы стека
// ------------------------------------------------------------

static void commissioning_cb(uint8_t mode)
{
    esp_zb_bdb_start_top_level_commissioning(mode);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    esp_err_t st = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = *signal_struct->p_app_signal;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "старт стека");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (st == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "сети нет — ищу (включи Permit Join в z2m)");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "в сети (после перезагрузки)");
                on_joined(false);
            }
        } else {
            ESP_LOGW(TAG, "не могу вернуться в сеть (%s), повтор. Не проходит долго — "
                          "сброс сети аккордом и Permit Join", esp_err_to_name(st));
            esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_cb, ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (st == ESP_OK) {
            ESP_LOGI(TAG, "подключилась к сети, PAN 0x%04hx, канал %d, адрес 0x%04hx",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            on_joined(true);
        } else {
            ESP_LOGI(TAG, "сеть не найдена, повтор через 3 с");
            esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 3000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE: {
        esp_zb_zdo_signal_leave_params_t *lp = esp_zb_app_signal_get_params(signal_struct->p_app_signal);
        if (lp && lp->leave_type == ESP_ZB_NWK_LEAVE_TYPE_RESET) {
            // удалили в z2m: забываем сеть и сразу ищем новую (Permit Join)
            ESP_LOGW(TAG, "плату удалили из сети — сброс и поиск сети");
            s_joined = false;
            esp_zb_factory_reset();
        } else {
            ESP_LOGW(TAG, "выход с повторным подключением");
        }
        break;
    }

    case ESP_ZB_NLME_STATUS_INDICATION: {
        esp_zb_zdo_signal_nwk_status_indication_params_t *p =
            esp_zb_app_signal_get_params(signal_struct->p_app_signal);
        if (p) ESP_LOGW(TAG, "сеть: статус 0x%02x, адрес 0x%04x", p->status, p->network_addr);
        break;
    }

    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE: {
        esp_zb_zdo_device_unavailable_params_t *p = esp_zb_app_signal_get_params(signal_struct->p_app_signal);
        if (p) ESP_LOGW(TAG, "недоступен адресат 0x%04x", p->short_addr);
        break;
    }

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        // пока подключаемся и пока z2m настраивает плату — не спим, иначе
        // пропустим ответы сети. Потом радио спит, чип уснёт, когда все задачи отпустят его
        if (s_sleep_allowed) esp_zb_sleep_now();
        break;

    default:
        ESP_LOGI(TAG, "сигнал %s (0x%x), статус %s", esp_zb_zdo_signal_to_string(sig), sig,
                 esp_err_to_name(st));
        break;
    }
}

// Итог каждой отправки: дошло до сети или нет (диагностика)
static void send_status_cb(esp_zb_zcl_command_send_status_message_t m)
{
    if (m.status == ESP_OK) ESP_LOGI(TAG, "  ушло: ep %u, tsn %u", m.src_endpoint, m.tsn);
    else ESP_LOGW(TAG, "  НЕ ушло: ep %u, tsn %u, %s", m.src_endpoint, m.tsn, esp_err_to_name(m.status));
}

// ------------------------------------------------------------
// Задача стека
// ------------------------------------------------------------

static uint8_t s_boot_battery = 0;

static void zb_task(void *arg)
{
    esp_zb_cfg_t cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = ZB_KEEP_ALIVE_MS,
        },
    };
    esp_zb_sleep_enable(true);
    esp_zb_init(&cfg);

    esp_zb_ep_list_t *eps = esp_zb_ep_list_create();
    for (int i = 0; i < N_DIGITS; i++) add_ep(eps, EP_BASE_FIRST + i, i == 0, s_boot_battery);
    for (int i = 0; i < N_DIGITS; i++) add_ep(eps, EP_ALT_FIRST + i, false, 0);
    esp_zb_device_register(eps);
    esp_zb_zcl_command_send_status_handler_register(send_status_cb);

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    esp_zb_bdb_set_scan_duration(3);

    ESP_ERROR_CHECK(esp_zb_start(false));
    zb_set_ed_node_descriptor(0, true, 1);    // питание: батарея (как в Arduino-версии)
    esp_zb_stack_main_loop();
}

void zb_start(uint8_t battery_pct)
{
    s_boot_battery = battery_pct;
    esp_zb_platform_config_t pcfg = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE},
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&pcfg));
    xTaskCreate(zb_task, "zigbee", 6144, NULL, 5, NULL);
}

bool zb_joined(void)
{
    return s_joined;
}

// ------------------------------------------------------------
// Отправка
// ------------------------------------------------------------

static void report(uint8_t ep, uint16_t cluster, uint16_t attr)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {0};
    // напрямую координатору (адрес 0x0000, эндпоинт 1) — не зависим от таблицы привязок
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.dis_defalut_resp = 1;
    cmd.attributeID = attr;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.clusterID = cluster;
    cmd.zcl_basic_cmd.src_endpoint = ep;
    cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    esp_err_t e = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (e != ESP_OK) ESP_LOGW(TAG, "  отчёт не принят стеком: ep %u, %s", ep, esp_err_to_name(e));
}

void zb_set_button(int idx, bool alt, bool on)
{
    if (!s_joined) { ESP_LOGW(TAG, "  не в сети — кнопку не шлю"); return; }
    uint8_t ep = (alt ? EP_ALT_FIRST : EP_BASE_FIRST) + idx;
    uint8_t val = on ? 1 : 0;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID, &val, false);
    report(ep, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID);
    esp_zb_lock_release();
}

void zb_set_battery(uint8_t pct)
{
    if (!s_joined) return;
    uint8_t half_pct = (pct > 100 ? 100 : pct) * 2;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(EP_BASE_FIRST, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &half_pct, false);
    report(EP_BASE_FIRST, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
           ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);
    esp_zb_lock_release();
}

void zb_factory_reset(void)
{
    ESP_LOGW(TAG, "СБРОС СЕТИ");
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_factory_reset();   // стирает zb_storage и перезагружает
    esp_zb_lock_release();
}
