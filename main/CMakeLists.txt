set(priv_requires esp_wifi nvs_flash)

if(CONFIG_USBCOERCER_STATUS_LED)
    list(APPEND priv_requires esp_driver_led_strip)
endif()

idf_component_register(SRCS "main.c"
                       "dhserver.c"
                       "usbc_config.c"
                       INCLUDE_DIRS "."
                       REQUIRES lwip
                       PRIV_REQUIRES ${priv_requires}
                       )
