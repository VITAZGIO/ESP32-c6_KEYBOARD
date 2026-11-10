/*
 * Вибромоторчик на транзисторе. Рисунок вибрации — массив длительностей
 * в мс: «вкл, пауза, вкл, ...». Играется цепочкой одноразовых esp_timer,
 * ничего не блокирует. Мотор выключен всегда, кроме самого импульса,
 * так что во сне плата его не ест.
 */
#include "esp_timer.h"
#include "driver/gpio.h"
#include "config.h"
#include "haptic.h"

#if HAPTIC_ENABLED

static esp_timer_handle_t s_timer;
static const uint16_t *s_seq;
static uint8_t s_len, s_pos;

static void motor(bool on)
{
    gpio_set_level(PIN_HAPTIC, (on == (HAPTIC_ACTIVE_LEVEL != 0)) ? 1 : 0);
}

static void step_cb(void *arg)
{
    if (s_pos >= s_len) { motor(false); return; }
    motor((s_pos & 1) == 0);                       // чётные — вкл, нечётные — пауза
    esp_timer_start_once(s_timer, (uint64_t)s_seq[s_pos++] * 1000);
}

void haptic_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_HAPTIC,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    motor(false);
    esp_timer_create_args_t a = { .callback = step_cb, .name = "haptic" };
    esp_timer_create(&a, &s_timer);
}

static const uint16_t P_FIRE[]    = { HAPTIC_FIRE_MS };
static const uint16_t P_ROW1[]    = { 30 };
static const uint16_t P_ROW2[]    = { 30, 70, 30 };
static const uint16_t P_SVC1[]    = { 250 };
static const uint16_t P_SVC2[]    = { 250, 100, 250 };
static const uint16_t P_FAIL[]    = { 40, 80, 40, 80, 40 };
static const uint16_t P_CHORD[]   = { 600 };

void haptic_play(haptic_t p)
{
    const uint16_t *seq; uint8_t len;
    switch (p) {
    case HAP_FIRE:        seq = P_FIRE;    len = sizeof(P_FIRE) / 2;    break;
    case HAP_ROW1:        seq = P_ROW1;    len = sizeof(P_ROW1) / 2;    break;
    case HAP_ROW2:        seq = P_ROW2;    len = sizeof(P_ROW2) / 2;    break;
    case HAP_SVC1:        seq = P_SVC1;    len = sizeof(P_SVC1) / 2;    break;
    case HAP_SVC2:        seq = P_SVC2;    len = sizeof(P_SVC2) / 2;    break;
    case HAP_FAIL:        seq = P_FAIL;    len = sizeof(P_FAIL) / 2;    break;
    case HAP_CHORD:       seq = P_CHORD;   len = sizeof(P_CHORD) / 2;   break;
    default: return;
    }
    esp_timer_stop(s_timer);        // новый рисунок перебивает старый
    s_seq = seq; s_len = len; s_pos = 0;
    step_cb(NULL);
}

#else
void haptic_init(void) {}
void haptic_play(haptic_t p) { (void)p; }
#endif
