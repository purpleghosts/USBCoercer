#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb.h"

#if CONFIG_USBCOERCER_STATUS_LED
#include "led_strip.h"
#endif

#include "lwip/err.h"
#include "lwip/ethip6.h"
#include "lwip/init.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

#include "dhserver.h"
#include "usbc_config.h"

static const char *TAG = "USBCoercer";

static struct netif s_usb_netif;
static usbc_app_config_t s_app_config;
static dhcp_entry_t s_dhcp_entries[CONFIG_USBCOERCER_DHCP_POOL_SIZE];
static dhcp_route_option_t s_dhcp_route_options[CONFIG_USBCOERCER_STATIC_ROUTE_MAX_COUNT];
static dhcp_option_settings_t s_dhcp_options;
static dhcp_config_t s_dhcp_server_cfg;
static const usbc_app_config_t *s_netif_config = NULL;

#if CONFIG_USBCOERCER_STATUS_LED
static led_strip_handle_t s_status_led;
static bool s_dhcp_request_seen;
static bool s_dhcp_discover_seen;
#endif

static esp_err_t init_status_led(void);
static void set_status_led_color(uint8_t red, uint8_t green, uint8_t blue);
#if CONFIG_USBCOERCER_STATUS_LED
static void on_dhcp_request(void *ctx);
static void on_dhcp_discover(void *ctx);
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Reinitialising NVS flash");
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            return erase_err;
        }
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!buffer || len == 0) {
        return ESP_OK;
    }
    if (!netif_is_up(&s_usb_netif) || s_usb_netif.input == NULL) {
        return ESP_OK;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) {
        ESP_LOGW(TAG, "No memory for inbound frame");
        return ESP_FAIL;
    }
    memcpy(p->payload, buffer, len);

    err_t err = s_usb_netif.input(p, &s_usb_netif);
    if (err != ERR_OK) {
        ESP_LOGW(TAG, "lwIP input error %d", err);
        pbuf_free(p);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static err_t ncm_linkoutput_fn(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (tinyusb_net_send_sync(p->payload, p->tot_len, p, portMAX_DELAY) != ESP_OK) {
        return ERR_IF;
    }
    return ERR_OK;
}

static err_t ncm_netif_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr)
{
    return etharp_output(netif, p, addr);
}

#if LWIP_IPV6
static err_t ncm_netif_output_ip6_fn(struct netif *netif, struct pbuf *p, const ip6_addr_t *addr)
{
    return ethip6_output(netif, p, addr);
}
#endif

static err_t ncm_netif_init_cb(struct netif *netif)
{
    netif->name[0] = 'N';
    netif->name[1] = 'C';
    netif->mtu = 1500;
    netif->hwaddr_len = 6;

    if (s_netif_config) {
        memcpy(netif->hwaddr, s_netif_config->interface.mac, sizeof(s_netif_config->interface.mac));
    } else {
        memset(netif->hwaddr, 0, sizeof(netif->hwaddr));
    }

    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->linkoutput = ncm_linkoutput_fn;
    netif->output = ncm_netif_output_fn;
#if LWIP_IPV6
    netif->output_ip6 = ncm_netif_output_ip6_fn;
#endif
    return ERR_OK;
}

static esp_err_t init_tinyusb(const usbc_app_config_t *config)
{
    tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer = NULL,
        .user_context = NULL,
    };
    memcpy(net_cfg.mac_addr, config->interface.mac, sizeof(net_cfg.mac_addr));

    err = tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "tinyusb_net_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "TinyUSB NCM ready (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
             config->interface.mac[0], config->interface.mac[1], config->interface.mac[2],
             config->interface.mac[3], config->interface.mac[4], config->interface.mac[5]);
    return ESP_OK;
}

static esp_err_t init_network_interface(const usbc_app_config_t *config)
{
    s_netif_config = config;
    if (!netif_add(&s_usb_netif,
                   &config->interface.local_ip,
                   &config->interface.netmask,
                   &config->interface.gateway,
                   NULL,
                   ncm_netif_init_cb,
                   tcpip_input)) {
        ESP_LOGE(TAG, "Failed to add USB network interface");
        return ESP_FAIL;
    }

    netif_set_default(&s_usb_netif);
    netif_set_up(&s_usb_netif);
    netif_set_link_up(&s_usb_netif);
    return ESP_OK;
}

static esp_err_t start_dhcp_server(const usbc_app_config_t *config)
{
    size_t lease_count = config->dhcp.pool_size;
    if (lease_count > CONFIG_USBCOERCER_DHCP_POOL_SIZE) {
        ESP_LOGW(TAG, "Truncating DHCP pool (%u -> %u)", (unsigned)lease_count,
                 (unsigned)CONFIG_USBCOERCER_DHCP_POOL_SIZE);
        lease_count = CONFIG_USBCOERCER_DHCP_POOL_SIZE;
    }

    uint32_t start_host = lwip_ntohl(ip4_addr_get_u32(&config->dhcp.pool_start));
    for (size_t i = 0; i < lease_count; ++i) {
        uint32_t addr_host = start_host + i;
        ip4_addr_t lease_ip;
        lease_ip.addr = lwip_htonl(addr_host);
        s_dhcp_entries[i].addr = lease_ip;
        s_dhcp_entries[i].lease = config->dhcp.lease_time;
        memset(s_dhcp_entries[i].mac, 0, sizeof(s_dhcp_entries[i].mac));
    }

    s_dhcp_options.enable_routes = (config->routes.count > 0);
    if (s_dhcp_options.enable_routes) {
        size_t route_count = config->routes.count;
        if (route_count > CONFIG_USBCOERCER_STATIC_ROUTE_MAX_COUNT) {
            ESP_LOGW(TAG, "Truncating static route list (%u -> %u)",
                     (unsigned)route_count, (unsigned)CONFIG_USBCOERCER_STATIC_ROUTE_MAX_COUNT);
            route_count = CONFIG_USBCOERCER_STATIC_ROUTE_MAX_COUNT;
        }
        for (size_t i = 0; i < route_count; ++i) {
            s_dhcp_route_options[i].prefix_length = config->routes.routes[i].prefix_length;
            s_dhcp_route_options[i].network = config->routes.routes[i].network;
            s_dhcp_route_options[i].gateway = config->routes.routes[i].gateway;
        }
        s_dhcp_options.route_count = route_count;
        s_dhcp_options.routes = s_dhcp_route_options;
    } else {
        s_dhcp_options.route_count = 0;
        s_dhcp_options.routes = NULL;
    }
    s_dhcp_options.enable_wpad = config->wpad.enabled;
    s_dhcp_options.wpad_url = config->wpad.enabled ? config->wpad.url : NULL;

    s_dhcp_server_cfg.router = config->interface.gateway;
    s_dhcp_server_cfg.port = 67;
    s_dhcp_server_cfg.dns = config->dhcp.dns;
    s_dhcp_server_cfg.domain = config->dhcp.domain[0] ? config->dhcp.domain : NULL;
    s_dhcp_server_cfg.num_entry = (int)lease_count;
    s_dhcp_server_cfg.entries = s_dhcp_entries;
    s_dhcp_server_cfg.options = &s_dhcp_options;

    err_t err;
    do {
        err = dhserv_init(&s_dhcp_server_cfg);
        if (err != ERR_OK) {
            ESP_LOGW(TAG, "DHCP server init failed (%d), retrying", err);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } while (err != ERR_OK);

    return ESP_OK;
}

static esp_err_t init_status_led(void)
{
#if CONFIG_USBCOERCER_STATUS_LED
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_USBCOERCER_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = CONFIG_USBCOERCER_STATUS_LED_RMT_RESOLUTION,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_status_led);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialise status LED: %s", esp_err_to_name(err));
        return err;
    }

    err = led_strip_clear(s_status_led);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear status LED: %s", esp_err_to_name(err));
    }
#endif
    return ESP_OK;
}

static void set_status_led_color(uint8_t red, uint8_t green, uint8_t blue)
{
#if CONFIG_USBCOERCER_STATUS_LED
    if (!s_status_led) {
        return;
    }

    esp_err_t err = led_strip_set_pixel(s_status_led, 0, red, green, blue);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set status LED colour: %s", esp_err_to_name(err));
        return;
    }

    err = led_strip_refresh(s_status_led);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to refresh status LED: %s", esp_err_to_name(err));
    }
#else
    (void)red;
    (void)green;
    (void)blue;
#endif
}

#if CONFIG_USBCOERCER_STATUS_LED
static void on_dhcp_request(void *ctx)
{
    (void)ctx;
    s_dhcp_request_seen = true;
    set_status_led_color(0, 0, CONFIG_USBCOERCER_STATUS_LED_BRIGHTNESS);
}

static void on_dhcp_discover(void *ctx)
{
    (void)ctx;
    s_dhcp_discover_seen = true;
    if (!s_dhcp_request_seen) {
        set_status_led_color(CONFIG_USBCOERCER_STATUS_LED_BRIGHTNESS,
                             CONFIG_USBCOERCER_STATUS_LED_BRIGHTNESS,
                             0);
    }
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());

    ESP_ERROR_CHECK(usbc_load_config(&s_app_config));
    usbc_log_config(&s_app_config);

    ESP_ERROR_CHECK(init_tinyusb(&s_app_config));

    tcpip_init(NULL, NULL);
    ESP_ERROR_CHECK(init_network_interface(&s_app_config));
    ESP_ERROR_CHECK(start_dhcp_server(&s_app_config));
#if CONFIG_USBCOERCER_STATUS_LED
    dhserv_register_discover_callback(on_dhcp_discover, NULL);
    dhserv_register_request_callback(on_dhcp_request, NULL);
#endif
    ESP_ERROR_CHECK(init_status_led());

    char ip_buf[IP4ADDR_STRLEN_MAX];
    ESP_LOGI(TAG, "USB interface up at %s",
             ip4addr_ntoa_r(&s_app_config.interface.local_ip, ip_buf, sizeof(ip_buf)));
#if CONFIG_USBCOERCER_STATUS_LED
    if (s_dhcp_request_seen) {
        set_status_led_color(0, 0, CONFIG_USBCOERCER_STATUS_LED_BRIGHTNESS);
    } else if (s_dhcp_discover_seen) {
        set_status_led_color(CONFIG_USBCOERCER_STATUS_LED_BRIGHTNESS,
                             CONFIG_USBCOERCER_STATUS_LED_BRIGHTNESS,
                             0);
    } else {
        set_status_led_color(0, CONFIG_USBCOERCER_STATUS_LED_BRIGHTNESS, 0);
    }
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
