#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USBCOERCER_MAX_DHCP_DOMAIN_LEN CONFIG_USBCOERCER_DHCP_DOMAIN_MAXLEN
#define USBCOERCER_MAX_WPAD_URL_LEN    CONFIG_USBCOERCER_WPAD_URL_MAXLEN
#define USBCOERCER_MAX_STATIC_ROUTES   CONFIG_USBCOERCER_STATIC_ROUTE_MAX_COUNT

typedef struct {
    ip4_addr_t network;
    uint8_t prefix_length;
    ip4_addr_t gateway;
} usbc_static_route_t;

typedef struct {
    ip4_addr_t local_ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    uint8_t mac[6];
} usbc_interface_config_t;

typedef struct {
    ip4_addr_t pool_start;
    uint8_t pool_size;
    uint32_t lease_time;
    ip4_addr_t dns;
    char domain[USBCOERCER_MAX_DHCP_DOMAIN_LEN + 1];
} usbc_dhcp_config_t;

typedef struct {
    bool enabled;
    char url[USBCOERCER_MAX_WPAD_URL_LEN + 1];
} usbc_wpad_config_t;

typedef struct {
    size_t count;
    usbc_static_route_t routes[USBCOERCER_MAX_STATIC_ROUTES];
} usbc_route_list_t;

typedef struct {
    usbc_interface_config_t interface;
    usbc_dhcp_config_t dhcp;
    usbc_wpad_config_t wpad;
    usbc_route_list_t routes;
} usbc_app_config_t;

esp_err_t usbc_load_config(usbc_app_config_t *config);
void usbc_log_config(const usbc_app_config_t *config);

#ifdef __cplusplus
}
#endif

