idf_component_register(SRCS "konnected-homekit.cpp" "homekit.cpp" "wifi.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES gdolib esp_hap_core esp_hap_apple_profiles esp_wifi nvs_flash wifi_provisioning nvs_wifi_connect
                )
