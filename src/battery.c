/*
 * Замер аккумулятора через делитель 20k/20k на GPIO1.
 */
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "config.h"
#include "battery.h"

static const char *TAG = "BAT";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_ok = false;

void battery_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(PIN_BAT_ADC, &unit, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d не АЦП", PIN_BAT_ADC);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return;

    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, s_chan, &ccfg);

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);
    if (!s_ok) ESP_LOGE(TAG, "калибровка АЦП не создалась");
}

uint32_t battery_mv(void)
{
    if (!s_ok) return 0;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += mv;
            n++;
        }
    }
    return n ? (uint32_t)(sum / n) * 2 : 0;   // делитель 1:2
}

uint8_t battery_pct(uint32_t mv)
{
    if (mv <= BAT_MIN_MV) return 0;
    if (mv >= BAT_MAX_MV) return 100;
    return (uint8_t)((mv - BAT_MIN_MV) * 100 / (BAT_MAX_MV - BAT_MIN_MV));
}
