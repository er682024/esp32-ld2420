idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "http_server.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        ld2420
    PRIV_REQUIRES
        app_update
        esp_timer
        mbedtls
)

# Data/ora di configurazione CMake (quando lanci idf.py build)
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")
add_definitions(-DBIN_BUILD_DATE="${BUILD_DATE}")