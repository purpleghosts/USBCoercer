idf_component_register(SRCS "main.c"
                       "dhserver.c"
                       "usbc_config.c"
                       INCLUDE_DIRS "."
                       REQUIRES lwip
                       PRIV_REQUIRES esp_wifi nvs_flash led_strip
                       )
