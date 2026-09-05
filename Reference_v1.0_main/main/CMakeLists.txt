idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES bt esp_event esp_lcd esp_netif esp_psram esp_wifi driver nvs_flash)
