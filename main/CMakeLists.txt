set(priv_requires esp_wifi nvs_flash)

if(CONFIG_USBCOERCER_STATUS_LED)
    list(APPEND priv_requires esp_driver_led_strip)
endif()

idf_component_register(SRCS "main.c"
                       "dhserver.c"
                       "usbc_config.c"
                       "wpad_http_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server lwip
                       PRIV_REQUIRES ${priv_requires}
                       )
