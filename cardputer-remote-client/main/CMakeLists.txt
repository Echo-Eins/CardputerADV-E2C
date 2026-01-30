idf_component_register(
    SRCS
        "../src/main.c"
        "../src/remote_crypto.c"
        "../src/remote_protocol.c"
        "../src/remote_network.c"
        "../src/remote_display.c"
        "../src/remote_input.c"
    INCLUDE_DIRS
        "../include"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        mdns
        mbedtls
        driver
        esp_lcd
        esp_timer
        freertos
        heap
)

# Enable hardware JPEG decoder for ESP32-S3
if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_IDF_TARGET_ESP32S3=1)
endif()
