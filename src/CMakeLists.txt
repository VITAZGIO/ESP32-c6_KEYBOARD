idf_component_register(
    SRCS "main.c" "zb.c" "battery.c" "ota.c"
    INCLUDE_DIRS "."
    REQUIRES espressif__esp-zigbee-lib nvs_flash esp_driver_gpio esp_adc esp_pm esp_timer
             esp_wifi esp_netif esp_event esp_http_server app_update esp_system lwip
)
