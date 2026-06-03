idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_http_server esp_websocket_client esp_timer esp_sntp mdns json vfs esp_http_client driver)
