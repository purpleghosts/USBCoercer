idf_component_register(SRCS "main.c" 
                       "dhserver.c"
                       INCLUDE_DIRS "."
		       REQUIRES lwip
                       PRIV_REQUIRES esp_wifi nvs_flash
                       )
