idf_component_register(
    SRCS
        "gdo-blaq-homekit.cpp"
        "homekit.cpp"
        "wifi.cpp"
        "diag_webserver.cpp"
        "clock_sync.cpp"
        "activity_history.cpp"
        "log_ring_buffer.cpp"
        "pre_close_warning.c"
        "diagnostics_counters.c"
    INCLUDE_DIRS "." "include"
    PRIV_REQUIRES
        bootloader_support
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
        esp_http_server
        driver
        app_update
        mdns
        mbedtls
        json
        lwip
)

# A panic skips shutdown handlers. Wrap the panic entry point so it can release
# the GDO TX pin first (see __wrap_esp_panic_handler).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
