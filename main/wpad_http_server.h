#pragma once

#include "esp_err.h"

#include "usbc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wpad_http_server_start(const usbc_wpad_config_t *config);

#ifdef __cplusplus
}
#endif

