/*
 * AULA F75 — Zigbee-кнопки внутри клавиатуры
 * ESP32-C6, ESP-IDF, спящий Zigbee End Device с light sleep.
 *
 * Плата спит и остаётся в сети. End будит её мгновенно, пока End зажат —
 * читаем цифры с матрицы клавиатуры. Отпустил, всё отправлено — спит.
 *
 * Три режима — всё решает то, что было с End ДО рабочего зажатия.
 * Цифры везде срабатывают мгновенно.
 *
 *   просто зажал End + цифра 1..6                 -> Btn 1..6
 *   тап End (коротко), отпустил, зажал + цифра    -> Btn 1..6 dbl
 *   держал End 2 с, отпустил, нажал End + цифру   -> сервисный режим:
 *       любая из цифр 1,2,3    -> сброс сети Zigbee
 *       любая из цифр 4,5,6    -> режим OTA (Wi-Fi, прошивка по воздуху)
 *
 * Флаг взводится в момент ОТПУСКАНИЯ End и живёт 3 секунды. Если во время
 * удержания нажимались цифры — это была обычная работа, флаг не взводится.
 *
 * VITAZGIO / AulaKeys
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#include "config.h"
#include "battery.h"
#include "zb.h"
#include "ota.h"

static const char *TAG = "AULA";

static const int DIGIT_PINS[N_DIGITS] = DIGIT_PINS_INIT;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ============================================================
// Матрица: прерывание на сканирующей линии цифр
// ============================================================
// На каждый скан (спад линии PIN_SCAN) смотрим линии чтения.
// Цифра нажата, когда DIGIT_PRESS_SCANS сканов подряд её линия в нуле.

static volatile uint8_t s_low_run[N_DIGITS];
static volatile uint8_t s_high_run[N_DIGITS];
static volatile uint8_t s_digit_down;             // бит i = цифра i зажата
static volatile uint32_t s_last_scan_ms;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR scan_isr(void *arg)
{
    uint32_t in = REG_READ(GPIO_IN_REG);
    portENTER_CRITICAL_ISR(&s_mux);
    for (int i = 0; i < N_DIGITS; i++) {
        bool low = !(in & (1UL << DIGIT_PINS[i]));
        if (low) {
            s_high_run[i] = 0;
            if (s_low_run[i] < 255) s_low_run[i]++;
            if (s_low_run[i] >= DIGIT_PRESS_SCANS) s_digit_down |= (1 << i);
        } else {
            s_low_run[i] = 0;
            if (s_high_run[i] < 255) s_high_run[i]++;
            if (s_high_run[i] >= DIGIT_RELEASE_SCANS) s_digit_down &= ~(1 << i);
        }
    }
    s_last_scan_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL_ISR(&s_mux);
}

static uint8_t digits_read(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // клава перестала сканировать — значит, ничего не зажато
    if (s_digit_down && (int32_t)(now - s_last_scan_ms) > SCAN_SILENCE_MS) {
        s_digit_down = 0;
        memset((void *)s_low_run, 0, sizeof(s_low_run));
    }
    uint8_t d = s_digit_down;
    portEXIT_CRITICAL(&s_mux);
    return d;
}

static void digits_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    s_digit_down = 0;
    memset((void *)s_low_run, 0, sizeof(s_low_run));
    memset((void *)s_high_run, 0, sizeof(s_high_run));
    portEXIT_CRITICAL(&s_mux);
}

// ============================================================
// End: пробуждение и антидребезг
// ============================================================

static SemaphoreHandle_t s_wake;   // будит главную задачу: End, сеть, таймер

// --- End по матрице: прерывание на скане столбца End
static volatile uint8_t s_end_low_run, s_end_high_run;
static volatile bool s_end_matrix;                // End зажат (по сканам)
static volatile uint32_t s_last_end_scan_ms;      // последний скан столбца End

static void IRAM_ATTR end_scan_isr(void *arg)
{
    bool low = !(REG_READ(GPIO_IN_REG) & (1UL << PIN_END));
    portENTER_CRITICAL_ISR(&s_mux);
    if (low) {
        s_end_high_run = 0;
        if (s_end_low_run < 255) s_end_low_run++;
        if (s_end_low_run >= END_PRESS_SCANS) s_end_matrix = true;
    } else {
        s_end_low_run = 0;
        if (s_end_high_run < 255) s_end_high_run++;
        if (s_end_high_run >= END_RELEASE_SCANS) s_end_matrix = false;
    }
    s_last_end_scan_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL_ISR(&s_mux);
}

// End зажат? Сканов нет дольше SCAN_SILENCE_MS — клава уснула, значит отпущен.
static bool end_raw(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(now - s_last_end_scan_ms) > SCAN_SILENCE_MS) {
        s_end_matrix = false;
        s_end_low_run = 0;
    }
    bool d = s_end_matrix;
    portEXIT_CRITICAL(&s_mux);
    return d;
}

// клава ещё сканирует (что-то зажато) — не засыпать, иначе разбудит снова
static bool matrix_busy(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (int32_t)(now - s_last_end_scan_ms) <= SCAN_SILENCE_MS;
}

// после пробуждения: даём клаве время начать сканировать
static void end_watch_start(void)
{
    portENTER_CRITICAL(&s_mux);
    s_last_end_scan_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_end_low_run = s_end_high_run = 0;
    s_end_matrix = false;
    portEXIT_CRITICAL(&s_mux);
    gpio_intr_enable(PIN_END_SCAN);
}

// пробуждение: линия чтения ряда End ушла в ноль
static void IRAM_ATTR end_isr(void *arg)
{
    gpio_intr_disable(PIN_END);   // дальше работаем по сканам
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_wake, &hp);
    if (hp) portYIELD_FROM_ISR();
}

// ============================================================
// Логика режимов и кнопок
// ============================================================

typedef enum { PEND_NONE, PEND_DBL, PEND_SERVICE } pending_t;
typedef enum { MODE_BASE, MODE_DBL, MODE_SERVICE } keys_mode_t;

static pending_t s_pending = PEND_NONE;
static uint32_t s_pending_at;
static keys_mode_t s_mode = MODE_BASE;

static bool s_end_down = false;
static uint8_t s_end_low_samples = 0;
static uint32_t s_end_high_since = 0;
static uint32_t s_end_down_at = 0;
static bool s_session_used = false;

static bool s_combo[N_DIGITS];

typedef struct {
    uint32_t off_at;      // когда слать OFF после импульса (0 — не надо)
    uint8_t retries;      // сколько ещё повторных OFF
    uint32_t retry_at;
    uint32_t last_fire;
} btn_t;
static btn_t s_btn[2][N_DIGITS];   // [0] — Btn, [1] — Btn dbl

static volatile bool s_joined_event = false;
static volatile uint32_t s_wakes, s_end_presses;   // для лога: пробуждения и нажатия End

void app_on_zb_joined(void)
{
    s_joined_event = true;
    xSemaphoreGive(s_wake);
}

static void fire(int idx, bool alt)
{
    btn_t *b = &s_btn[alt][idx];
    uint32_t now = now_ms();
    if (now - b->last_fire < REFIRE_MS) return;
    b->last_fire = now;
    zb_set_button(idx, alt, true);
    b->off_at = now + PULSE_MS;
    b->retries = 0;
    ESP_LOGI(TAG, ">>> Btn %d%s", idx + 1, alt ? " dbl" : "");
}

// возвращает true, если ещё есть что отправлять
static bool pulses_service(uint32_t now)
{
    bool busy = false;
    for (int a = 0; a < 2; a++) {
        for (int i = 0; i < N_DIGITS; i++) {
            btn_t *b = &s_btn[a][i];
            if (b->off_at && (int32_t)(now - b->off_at) >= 0) {
                zb_set_button(i, a, false);
                b->off_at = 0;
                b->retries = OFF_RETRIES;
                b->retry_at = now + OFF_RETRY_MS;
            } else if (!b->off_at && b->retries && (int32_t)(now - b->retry_at) >= 0) {
                zb_set_button(i, a, false);
                b->retries--;
                b->retry_at = now + OFF_RETRY_MS;
            }
            if (b->off_at || b->retries) busy = true;
        }
    }
    return busy;
}

static void report_battery(void)
{
    uint32_t mv = battery_mv();
    uint8_t pct = battery_pct(mv);
    zb_set_battery(pct);
    ESP_LOGI(TAG, "батарея %lu мВ, %u %%", (unsigned long)mv, pct);
}

static void end_pressed(uint32_t now)
{
    s_end_down = true;
    s_end_down_at = now;
    s_end_presses++;
    s_session_used = false;
    digits_clear();
    gpio_intr_enable(PIN_SCAN);   // слушаем матрицу только пока End зажат

    if (s_pending == PEND_DBL)          { s_mode = MODE_DBL;     ESP_LOGI(TAG, "[MODE] второй ряд"); }
    else if (s_pending == PEND_SERVICE) { s_mode = MODE_SERVICE; ESP_LOGI(TAG, "[MODE] НАСТРОЙКИ"); }
    else                                  s_mode = MODE_BASE;
    s_pending = PEND_NONE;
}

static void end_released(uint32_t released_at)
{
    s_end_down = false;
    gpio_intr_disable(PIN_SCAN);  // линия скана в покое может «шуметь» и будить чип
    uint32_t held = released_at - s_end_down_at;

    if (s_session_used) {
        s_pending = PEND_NONE;
    } else if (held < TAP_MAX_MS) {
        s_pending = PEND_DBL;
        s_pending_at = released_at;
        ESP_LOGI(TAG, "[MODE] тап -> жду второй ряд");
    } else if (held >= SERVICE_HOLD_MS) {
        s_pending = PEND_SERVICE;
        s_pending_at = released_at;
        ESP_LOGI(TAG, "[MODE] удержание -> жду настройки");
    } else {
        s_pending = PEND_NONE;
    }
    s_mode = MODE_BASE;
    for (int i = 0; i < N_DIGITS; i++) s_combo[i] = false;
}

// Один шаг опроса (раз в END_POLL_MS). Возвращает true, пока надо бодрствовать.
static bool step(void)
{
    uint32_t now = now_ms();

    // ---- End с антидребезгом
    bool raw = end_raw();
    if (!s_end_down) {
        s_end_low_samples = raw ? s_end_low_samples + 1 : 0;
        if (s_end_low_samples >= END_PRESS_SAMPLES) {
            s_end_low_samples = 0;
            end_pressed(now);
        }
    } else {
        if (raw) {
            s_end_high_since = 0;
        } else {
            if (!s_end_high_since) s_end_high_since = now ? now : 1;
            if (now - s_end_high_since >= END_RELEASE_MS) {
                end_released(s_end_high_since);
                s_end_high_since = 0;
            }
        }
    }

    // ---- окно флага закрылось
    if (!s_end_down && s_pending != PEND_NONE) {
        uint32_t win = (s_pending == PEND_DBL) ? ALT_WINDOW_MS : SERVICE_WINDOW_MS;
        if (now - s_pending_at >= win) {
            s_pending = PEND_NONE;
            ESP_LOGI(TAG, "[MODE] окно закрылось");
        }
    }

    // ---- цифры
    uint8_t d = s_end_down ? digits_read() : 0;
    int n_down = __builtin_popcount(d);

    if (s_end_down && s_mode == MODE_SERVICE) {
        uint8_t low = d & 0x07, high = d & 0x38;
        if (low && !high) {
            s_session_used = true;
            ESP_LOGW(TAG, "аккорд: сброс сети Zigbee");
            vTaskDelay(pdMS_TO_TICKS(300));
            zb_factory_reset();
        }
        if (high && !low) {
            s_session_used = true;
            ota_reboot_into();
        }
        if (d) s_session_used = true;
    } else if (s_end_down) {
        if (d) s_session_used = true;   // жали цифры — это работа, а не тап/удержание
        bool sane = n_down <= MAX_SIMULTANEOUS;
        for (int i = 0; i < N_DIGITS; i++) {
            bool on = (d & (1 << i)) && sane;
            if (on && !s_combo[i]) {
                s_combo[i] = true;
                s_session_used = true;
                fire(i, s_mode == MODE_DBL);
            }
            if (!on) s_combo[i] = false;
        }
    }

    bool sending = pulses_service(now);

    // бодрствуем, пока End зажат или отпущен недавно (идёт проверка дребезга),
    // пока открыто окно режима, пока не ушли все OFF и пока клава сканирует
    return s_end_down || s_end_low_samples || s_pending != PEND_NONE || sending
           || matrix_busy();
}

// ============================================================
// Главная задача
// ============================================================

static void keys_task(void *arg)
{
    esp_pm_lock_handle_t awake;
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "keys", &awake));

    // дошли сюда без падения — прошивка рабочая, откат не нужен
    esp_ota_mark_app_valid_cancel_rollback();

    uint32_t last_bat = now_ms();
    bool holding = false;

    for (;;) {
        // --- активная фаза: опрос каждые 2 мс, чип не спит
        if (!holding) { esp_pm_lock_acquire(awake); holding = true; }
        end_watch_start();
        s_wakes++;

        if (s_joined_event) {
            s_joined_event = false;
            ESP_LOGI(TAG, "в сети: гашу все кнопки, отправляю заряд");
            for (int a = 0; a < 2; a++)
                for (int i = 0; i < N_DIGITS; i++) zb_set_button(i, a, false);
            report_battery();
            last_bat = now_ms();
        }

        while (step()) vTaskDelay(pdMS_TO_TICKS(END_POLL_MS));

        if (now_ms() - last_bat >= BAT_REPORT_MS) {
            report_battery();
            last_bat = now_ms();
        }
        // отпускаем чип спать — дальше всё должно успеть
        // до следующего нажатия или до отчёта батареи

        // --- спячка: ждём End (прерывание по низкому уровню) или таймер батареи
        gpio_intr_disable(PIN_END_SCAN);
        esp_pm_lock_release(awake);
        holding = false;

        xSemaphoreTake(s_wake, 0);          // сбросить старое пробуждение
        if (s_joined_event) {
            xSemaphoreGive(s_wake);          // уже есть работа
        } else if (gpio_get_level(PIN_END) == 0) {
            // линия ряда в нуле, а сканов нет — подождать, не крутиться вхолостую
            vTaskDelay(pdMS_TO_TICKS(50));
            xSemaphoreGive(s_wake);
        } else {
            gpio_intr_enable(PIN_END);
        }

        uint32_t elapsed = now_ms() - last_bat;
        uint32_t left = elapsed >= BAT_REPORT_MS ? 0 : BAT_REPORT_MS - elapsed;
        // тик = 1 мс; pdMS_TO_TICKS считает в 32 битах и на 6 часах переполняется
        xSemaphoreTake(s_wake, (TickType_t)(left / portTICK_PERIOD_MS));
        gpio_intr_disable(PIN_END);
    }
}

// ============================================================
// Диагностика сна: раз в 30 с — кто держит чип и сколько он спал
// ============================================================

#define DEBUG_PM 1

#if DEBUG_PM
#include "hal/pmu_ll.h"
#include "soc/pmu_struct.h"
#include "esp_attr.h"
// Диагностика сна. Окна по 30 с, последние 10 окон лежат в RTC-памяти:
// переживают отключение USB и перезагрузку (но не пропадание питания).
// Так видно, что было, пока плата работала от батареи без USB.
#define DBG_WIN   30000
#define DBG_HIST  10
typedef struct {
    uint32_t t_s;          // секунда от старта, конец окна
    uint32_t sleeps;       // сколько раз уснул
    uint32_t slept[4];     // сколько проспал: <1мс, 1-5мс, 5-50мс, >50мс
    uint32_t bit[16];      // сырые биты причины пробуждения PMU
    uint32_t zero;         // пробуждение без единого бита причины
    uint32_t sleep_ms;     // всего проспал за окно, мс
} dbg_win_t;
typedef struct {
    uint32_t magic;
    uint32_t boot;
    uint32_t head;         // сколько окон записано всего
    dbg_win_t w[DBG_HIST];
} dbg_hist_t;
#define DBG_MAGIC 0xA11AF75u
static RTC_NOINIT_ATTR dbg_hist_t s_hist;
static dbg_win_t s_cur;    // текущее окно (пишется из колбэка сна)

static esp_err_t IRAM_ATTR pm_exit_cb(int64_t slept_us, void *arg)
{
    if (slept_us <= 0) return ESP_OK;
    s_cur.sleeps++;
    s_cur.sleep_ms += (uint32_t)(slept_us / 1000);
    int b = slept_us < 1000 ? 0 : slept_us < 5000 ? 1 : slept_us < 50000 ? 2 : 3;
    s_cur.slept[b]++;
    uint32_t raw = pmu_ll_hp_get_wakeup_cause(&PMU);
    if (!raw) s_cur.zero++;
    for (int i = 0; i < 16; i++) if (raw & (1u << i)) s_cur.bit[i]++;
    return ESP_OK;
}

static void dbg_print_win(const char *pfx, const dbg_win_t *w)
{
    char line[160]; int n = 0;
    for (int i = 0; i < 16; i++)
        if (w->bit[i]) n += snprintf(line + n, sizeof(line) - n, " b%d=%lu", i, (unsigned long)w->bit[i]);
    line[n] = 0;
    ESP_LOGI(TAG, "%s t=%lus снов %lu спал %lu мс | <1мс %lu 1-5 %lu 5-50 %lu >50 %lu | без причины %lu |%s",
             pfx, (unsigned long)w->t_s, (unsigned long)w->sleeps, (unsigned long)w->sleep_ms,
             (unsigned long)w->slept[0], (unsigned long)w->slept[1],
             (unsigned long)w->slept[2], (unsigned long)w->slept[3],
             (unsigned long)w->zero, n ? line : " бит нет");
}

static void dbg_print_hist(const char *title)
{
    uint32_t cnt = s_hist.head < DBG_HIST ? s_hist.head : DBG_HIST;
    ESP_LOGI(TAG, "==== %s (запуск #%lu), окон %lu ====", title, (unsigned long)s_hist.boot, (unsigned long)cnt);
    for (uint32_t k = s_hist.head - cnt; k < s_hist.head; k++)
        dbg_print_win("  ", &s_hist.w[k % DBG_HIST]);
}

static void pm_debug_task(void *arg)
{
    // что было до перезагрузки (например, на батарее без USB)
    if (s_hist.magic == DBG_MAGIC && s_hist.head) {
        dbg_print_hist("ПРОШЛЫЙ ЗАПУСК");
        s_hist.boot++;
    } else {
        s_hist.boot = 1;
    }
    s_hist.magic = DBG_MAGIC;
    s_hist.head = 0;

    esp_pm_sleep_cbs_register_config_t cbs = { .exit_cb = pm_exit_cb };
    esp_pm_light_sleep_register_cbs(&cbs);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DBG_WIN));
        dbg_win_t w;
        portDISABLE_INTERRUPTS();
        w = s_cur;
        memset(&s_cur, 0, sizeof(s_cur));
        portENABLE_INTERRUPTS();
        w.t_s = (uint32_t)(esp_timer_get_time() / 1000000);
        s_hist.w[s_hist.head % DBG_HIST] = w;
        s_hist.head++;
        dbg_print_hist("СОН, последние окна по 30 с");
        ESP_LOGI(TAG, "за 30 с: пробуждений %lu, нажатий End %lu",
                 (unsigned long)s_wakes, (unsigned long)s_end_presses);
        s_wakes = s_end_presses = 0;
    }
}
#endif

// ============================================================

static void gpio_setup(void)
{
    // линия чтения ряда End: только вход, без подтяжек (тянет сама клава),
    // будит из light sleep низким уровнем
    gpio_config_t end_cfg = {
        .pin_bit_mask = 1ULL << PIN_END,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,   // включаем только в keys_task
    };
    gpio_config(&end_cfg);
    gpio_wakeup_enable(PIN_END, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    // линии матрицы — только вход, без подтяжек: в клавиатуру ничего не тянем
    uint64_t mask = (1ULL << PIN_SCAN) | (1ULL << PIN_END_SCAN);
    for (int i = 0; i < N_DIGITS; i++) {
        mask |= 1ULL << DIGIT_PINS[i];
    }
    gpio_config_t m_cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&m_cfg);
    gpio_set_intr_type(PIN_SCAN, GPIO_INTR_NEGEDGE);
    gpio_set_intr_type(PIN_END_SCAN, GPIO_INTR_NEGEDGE);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_SCAN, scan_isr, NULL);
    gpio_isr_handler_add(PIN_END, end_isr, NULL);
    gpio_isr_handler_add(PIN_END_SCAN, end_scan_isr, NULL);
    gpio_intr_disable(PIN_END_SCAN);
    gpio_intr_disable(PIN_END);
    gpio_intr_disable(PIN_SCAN);   // включается на время зажатого End
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (ota_requested()) {
        ota_run();   // не возвращается
    }

    ESP_LOGI(TAG, "старт, ESP-IDF, light sleep");

    s_wake = xSemaphoreCreateBinary();
    gpio_setup();
    battery_init();

    uint32_t mv = battery_mv();
    ESP_LOGI(TAG, "батарея %lu мВ", (unsigned long)mv);

    // light sleep: частота постоянная, чип спит, когда все задачи ждут
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));

#if TEST_NO_ZIGBEE
    ESP_LOGW(TAG, "ТЕСТ: Zigbee выключен (TEST_NO_ZIGBEE 1 в config.h)");
#else
    zb_start(battery_pct(mv));
#endif
    xTaskCreate(keys_task, "keys", 4096, NULL, 6, NULL);
#if DEBUG_PM
    xTaskCreate(pm_debug_task, "pmdbg", 3072, NULL, 1, NULL);
#endif
}
