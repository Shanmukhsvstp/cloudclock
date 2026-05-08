# idf_component_register(SRCS "CloudClock.c"
#                     INCLUDE_DIRS ".")
idf_component_register(SRCS "CloudClock.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_timer nvs_flash)