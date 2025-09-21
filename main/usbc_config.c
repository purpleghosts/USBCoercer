#include "usbc_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "USBC_CFG";

static esp_err_t parse_ipv4(const char *text, ip4_addr_t *out, bool allow_empty)
{
    if (!text || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (allow_empty && text[0] == '\0') {
        out->addr = 0;
        return ESP_OK;
    }
    if (!ip4addr_aton(text, out)) {
        ESP_LOGE(TAG, "Invalid IPv4 address: '%s'", text);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t parse_mac_address(const char *text, uint8_t mac[6])
{
    if (!text || !mac) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *ptr = text;
    for (int i = 0; i < 6; ++i) {
        while (*ptr == ' ' || *ptr == '\t') {
            ++ptr;
        }
        if (*ptr == '\0') {
            ESP_LOGE(TAG, "MAC address too short: '%s'", text);
            return ESP_ERR_INVALID_ARG;
        }
        char *endptr = NULL;
        long value = strtol(ptr, &endptr, 16);
        if (endptr == ptr || value < 0 || value > 0xFF) {
            ESP_LOGE(TAG, "Invalid MAC component in '%s'", text);
            return ESP_ERR_INVALID_ARG;
        }
        mac[i] = (uint8_t)value;
        ptr = endptr;
        if (i < 5) {
            if (*ptr == ':' || *ptr == '-') {
                ++ptr;
            } else if (!isspace((unsigned char)*ptr)) {
                ESP_LOGE(TAG, "Expected ':' or '-' in MAC '%s'", text);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    while (*ptr) {
        if (!isspace((unsigned char)*ptr)) {
            ESP_LOGE(TAG, "Trailing characters in MAC '%s'", text);
            return ESP_ERR_INVALID_ARG;
        }
        ++ptr;
    }
    return ESP_OK;
}

static char *trim(char *text)
{
    if (!text) {
        return text;
    }
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static bool extract_ipv4_from_url(const char *url, ip4_addr_t *out)
{
    if (!url || !out || url[0] == '\0') {
        return false;
    }

    const char *host_start = strstr(url, "://");
    host_start = host_start ? host_start + 3 : url;

    const char *host_end = host_start;
    while (*host_end && *host_end != '/' && *host_end != ':') {
        ++host_end;
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= IP4ADDR_STRLEN_MAX) {
        return false;
    }

    char host_buf[IP4ADDR_STRLEN_MAX];
    memcpy(host_buf, host_start, host_len);
    host_buf[host_len] = '\0';

    ip4_addr_t parsed = {0};
    if (!ip4addr_aton(host_buf, &parsed)) {
        return false;
    }

    *out = parsed;
    return true;
}

static esp_err_t parse_single_route(char *spec, usbc_static_route_t *route)
{
    char *separator = strchr(spec, ',');
    if (!separator) {
        ESP_LOGE(TAG, "Route entry must use '<network>/<prefix>,<gateway>': '%s'", spec);
        return ESP_ERR_INVALID_ARG;
    }
    *separator = '\0';
    char *network_spec = trim(spec);
    char *gateway_spec = trim(separator + 1);

    char *slash = strchr(network_spec, '/');
    if (!slash) {
        ESP_LOGE(TAG, "Route entry missing prefix length: '%s'", network_spec);
        return ESP_ERR_INVALID_ARG;
    }
    *slash = '\0';
    char *prefix_spec = trim(slash + 1);

    char *endptr = NULL;
    long prefix = strtol(prefix_spec, &endptr, 10);
    if (endptr == prefix_spec || prefix < 0 || prefix > 32) {
        ESP_LOGE(TAG, "Invalid prefix length in route '%s/%s'", network_spec, prefix_spec);
        return ESP_ERR_INVALID_ARG;
    }

    ip4_addr_t network = {0};
    if (!ip4addr_aton(network_spec, &network)) {
        ESP_LOGE(TAG, "Invalid network address in route '%s'", network_spec);
        return ESP_ERR_INVALID_ARG;
    }

    ip4_addr_t gateway = {0};
    if (!ip4addr_aton(gateway_spec, &gateway)) {
        ESP_LOGE(TAG, "Invalid gateway address in route '%s'", gateway_spec);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t network_host = lwip_ntohl(ip4_addr_get_u32(&network));
    uint32_t mask = 0;
    if (prefix > 0) {
        mask = 0xFFFFFFFFu << (32 - prefix);
    }
    network_host &= mask;

    route->network.addr = lwip_htonl(network_host);
    route->prefix_length = (uint8_t)prefix;
    route->gateway = gateway;
    return ESP_OK;
}

static esp_err_t parse_routes(usbc_app_config_t *config)
{
#if CONFIG_USBCOERCER_ENABLE_STATIC_ROUTES
    const char *raw_routes = CONFIG_USBCOERCER_STATIC_ROUTES;
    if (!raw_routes || raw_routes[0] == '\0') {
        config->routes.count = 0;
        return ESP_OK;
    }
    char *mutable_copy = strdup(raw_routes);
    if (!mutable_copy) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(mutable_copy, ";\n", &saveptr);
    while (token) {
        char *entry = trim(token);
        if (*entry != '\0') {
            if (count >= USBCOERCER_MAX_STATIC_ROUTES) {
                ESP_LOGW(TAG, "Ignoring extra static route entries beyond %d", USBCOERCER_MAX_STATIC_ROUTES);
                break;
            }
            esp_err_t err = parse_single_route(entry, &config->routes.routes[count]);
            if (err != ESP_OK) {
                free(mutable_copy);
                return err;
            }
            ++count;
        }
        token = strtok_r(NULL, ";\n", &saveptr);
    }
    config->routes.count = count;
    free(mutable_copy);
#else
    config->routes.count = 0;
#endif
    return ESP_OK;
}

esp_err_t usbc_load_config(usbc_app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));

    esp_err_t err = parse_ipv4(CONFIG_USBCOERCER_LOCAL_IP, &config->interface.local_ip, false);
    if (err != ESP_OK) {
        return err;
    }
    err = parse_ipv4(CONFIG_USBCOERCER_SUBNET_MASK, &config->interface.netmask, false);
    if (err != ESP_OK) {
        return err;
    }
    err = parse_ipv4(CONFIG_USBCOERCER_GATEWAY, &config->interface.gateway, true);
    if (err != ESP_OK) {
        return err;
    }
    err = parse_mac_address(CONFIG_USBCOERCER_MAC_ADDRESS, config->interface.mac);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_ipv4(CONFIG_USBCOERCER_DHCP_POOL_START, &config->dhcp.pool_start, false);
    if (err != ESP_OK) {
        return err;
    }
    config->dhcp.pool_size = (uint8_t)CONFIG_USBCOERCER_DHCP_POOL_SIZE;
    if (config->dhcp.pool_size == 0) {
        ESP_LOGE(TAG, "DHCP pool size must be greater than zero");
        return ESP_ERR_INVALID_SIZE;
    }
    config->dhcp.lease_time = CONFIG_USBCOERCER_DHCP_LEASE_TIME;

    err = parse_ipv4(CONFIG_USBCOERCER_DHCP_DNS, &config->dhcp.dns, true);
    if (err != ESP_OK) {
        return err;
    }

    size_t domain_len = strlen(CONFIG_USBCOERCER_DHCP_DOMAIN);
    if (domain_len > USBCOERCER_MAX_DHCP_DOMAIN_LEN) {
        ESP_LOGE(TAG, "DHCP domain exceeds maximum length (%d)", USBCOERCER_MAX_DHCP_DOMAIN_LEN);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(config->dhcp.domain, CONFIG_USBCOERCER_DHCP_DOMAIN, domain_len);
    config->dhcp.domain[domain_len] = '\0';

#if CONFIG_USBCOERCER_ENABLE_WPAD
    config->wpad.enabled = true;
    size_t wpad_len = strlen(CONFIG_USBCOERCER_WPAD_URL);
    if (wpad_len > USBCOERCER_MAX_WPAD_URL_LEN) {
        ESP_LOGE(TAG, "WPAD URL exceeds maximum length (%d)", USBCOERCER_MAX_WPAD_URL_LEN);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(config->wpad.url, CONFIG_USBCOERCER_WPAD_URL, wpad_len);
    config->wpad.url[wpad_len] = '\0';
#else
    config->wpad.enabled = false;
#endif

    if (config->wpad.enabled && config->dhcp.dns.addr == 0) {
        ip4_addr_t inferred_dns = {0};
        if (extract_ipv4_from_url(config->wpad.url, &inferred_dns)) {
            config->dhcp.dns = inferred_dns;
            char dns_buf[IP4ADDR_STRLEN_MAX];
            ESP_LOGW(TAG,
                     "No DHCP DNS configured; defaulting to WPAD host %s for compatibility",
                     ip4addr_ntoa_r(&config->dhcp.dns, dns_buf, sizeof(dns_buf)));
        } else {
            ESP_LOGW(TAG,
                     "WPAD enabled but no DHCP DNS configured; Windows clients may ignore option 252");
        }
    }

    err = parse_routes(config);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

void usbc_log_config(const usbc_app_config_t *config)
{
    if (!config) {
        return;
    }
    char ip_buf[IP4ADDR_STRLEN_MAX];
    char mask_buf[IP4ADDR_STRLEN_MAX];
    char gw_buf[IP4ADDR_STRLEN_MAX];
    ESP_LOGI(TAG, "Interface IP %s / Netmask %s",
             ip4addr_ntoa_r(&config->interface.local_ip, ip_buf, sizeof(ip_buf)),
             ip4addr_ntoa_r(&config->interface.netmask, mask_buf, sizeof(mask_buf)));
    ESP_LOGI(TAG, "Gateway %s", ip4addr_ntoa_r(&config->interface.gateway, gw_buf, sizeof(gw_buf)));
    ESP_LOGI(TAG, "MAC %02X:%02X:%02X:%02X:%02X:%02X", config->interface.mac[0], config->interface.mac[1],
             config->interface.mac[2], config->interface.mac[3], config->interface.mac[4], config->interface.mac[5]);

    ESP_LOGI(TAG, "DHCP pool start %s (%u leases)",
             ip4addr_ntoa_r(&config->dhcp.pool_start, ip_buf, sizeof(ip_buf)), config->dhcp.pool_size);
    ESP_LOGI(TAG, "DHCP lease time %u seconds", (unsigned)config->dhcp.lease_time);
    if (config->dhcp.dns.addr != 0) {
        ESP_LOGI(TAG, "DHCP DNS %s", ip4addr_ntoa_r(&config->dhcp.dns, mask_buf, sizeof(mask_buf)));
    }
    if (config->dhcp.domain[0] != '\0') {
        ESP_LOGI(TAG, "DHCP domain '%s'", config->dhcp.domain);
    }
    if (config->wpad.enabled) {
        ESP_LOGI(TAG, "WPAD URL %s", config->wpad.url);
    } else {
        ESP_LOGI(TAG, "WPAD disabled");
    }
    if (config->routes.count > 0) {
        ESP_LOGI(TAG, "Static routes (%u)", (unsigned)config->routes.count);
        for (size_t i = 0; i < config->routes.count; ++i) {
            const usbc_static_route_t *route = &config->routes.routes[i];
            ESP_LOGI(TAG, "  %s/%u -> %s",
                     ip4addr_ntoa_r(&route->network, ip_buf, sizeof(ip_buf)),
                     route->prefix_length,
                     ip4addr_ntoa_r(&route->gateway, gw_buf, sizeof(gw_buf)));
        }
    } else {
        ESP_LOGI(TAG, "Static routes disabled");
    }
}

