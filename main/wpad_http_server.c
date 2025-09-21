#include "wpad_http_server.h"

#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WPAD_HTTP";

static httpd_handle_t s_http_server = NULL;

static esp_err_t handle_wpad_request(httpd_req_t *req)
{
    const usbc_wpad_config_t *wpad_cfg = (const usbc_wpad_config_t *)req->user_ctx;
    if (!wpad_cfg || !wpad_cfg->inline_enabled || wpad_cfg->pac[0] == '\0') {
        ESP_LOGW(TAG, "WPAD inline request rejected (feature disabled)");
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Inline WPAD disabled");
    }

    httpd_resp_set_type(req, "application/x-ns-proxy-autoconfig");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    return httpd_resp_send(req, wpad_cfg->pac, HTTPD_RESP_USE_STRLEN);
}

esp_err_t wpad_http_server_start(const usbc_wpad_config_t *config)
{
    if (!config || !config->enabled) {
        ESP_LOGI(TAG, "WPAD HTTP server not started (disabled)");
        return ESP_OK;
    }

    if (s_http_server) {
        ESP_LOGW(TAG, "WPAD HTTP server already running");
        return ESP_OK;
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_http_server, &httpd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t wpad_uri = {
        .uri = "/wpad.dat",
        .method = HTTP_GET,
        .handler = handle_wpad_request,
        .user_ctx = (void *)config,
    };

    err = httpd_register_uri_handler(s_http_server, &wpad_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WPAD handler: %s", esp_err_to_name(err));
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WPAD HTTP server started (inline %s, PAC length %u bytes)",
             config->inline_enabled ? "enabled" : "disabled",
             (unsigned)strlen(config->pac));

    return ESP_OK;
}

