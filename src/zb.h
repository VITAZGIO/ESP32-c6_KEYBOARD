#pragma once
#include <stdbool.h>
#include <stdint.h>

// Запускает Zigbee-стек в отдельной задаче (sleepy end device, light sleep).
void zb_start(uint8_t battery_pct);

// Плата в сети.
bool zb_joined(void);

// Состояние кнопки: idx 0..5, alt = второй ряд.
void zb_set_button(int idx, bool alt, bool on);

// Заряд батареи в процентах.
void zb_set_battery(uint8_t pct);

// Забыть сеть и перезагрузиться (после этого — Permit Join в z2m).
void zb_factory_reset(void);

// Вызывается из Zigbee-задачи, когда плата подключилась к сети.
// Определена в main.c.
void app_on_zb_joined(void);
