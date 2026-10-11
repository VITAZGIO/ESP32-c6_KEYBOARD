#pragma once
#include <stdint.h>

void battery_init(void);
uint32_t battery_mv(void);          // напряжение аккумулятора, мВ
uint8_t battery_pct(uint32_t mv);   // 0..100 %
