idf_component_register(
    SRCS "app_main.c" "blynk_mqtt.c"
    PRIV_REQUIRES esp-tls esp_wifi esp_event nvs_flash esp_https_ota esp_http_client bootloader_support
    INCLUDE_DIRS "."
)
