#pragma once
#include <stdbool.h>

bool ota_requested(void);     // стоит флаг «загрузиться в OTA»
void ota_reboot_into(void);   // поставить флаг и перезагрузиться
void ota_run(void);           // Wi-Fi + приём прошивки, не возвращается
