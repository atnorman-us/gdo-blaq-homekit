idf_component_register(
    SRCS
        "gdo-blaq-homekit.cpp"
        "homekit.cpp"
        "wifi.cpp"
    INCLUDE_DIRS "." "include"
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        gdolib
        nvs_wifi_connect
        esp_hap_core
        esp_hap_platform
        esp_hap_extras
        esp_hap_apple_profiles
)

