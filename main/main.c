#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"


// TinyUSB / ESP-IDF
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb.h"

// lwIP
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/ethip6.h"

// (Si quieres un HTTP server local)
#include "lwip/apps/httpd.h"

// Nuestro DHCP server minimal (incluye O121 + O252)
#include "dhserver.h"

static const char *TAG = "NCM_DHCP_WPAD";

// Ajustes de IP local
#define USB_NET_IP   "192.168.7.1"
#define USB_NET_MASK "255.255.255.0"
#define USB_NET_GW   "0.0.0.0"

// Interfaz lwIP
static struct netif ncm_netif;

/* -------------------------------------------------------------------------
   Recibir paquetes USB -> inyectarlos a lwIP
------------------------------------------------------------------------- */
static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (!buffer || len == 0) {
        return ESP_OK;
    }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) {
        ESP_LOGW(TAG, "No mem for pbuf");
        return ESP_FAIL;
    }
    memcpy(p->payload, buffer, len);

    err_t err = ncm_netif.input(p, &ncm_netif);
    if (err != ERR_OK) {
        pbuf_free(p);
        ESP_LOGW(TAG, "lwIP input error %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
   Transmitir paquetes lwIP -> USB
------------------------------------------------------------------------- */
static err_t ncm_linkoutput_fn(struct netif *netif, struct pbuf *p)
{
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

/* -------------------------------------------------------------------------
   Inicializar la interfaz lwIP
------------------------------------------------------------------------- */
static err_t ncm_netif_init_cb(struct netif *netif)
{
    netif->name[0] = 'N';
    netif->name[1] = 'C';

    netif->mtu = 1500; // normal en Ethernet
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                 | NETIF_FLAG_UP        | NETIF_FLAG_LINK_UP;
    netif->hwaddr_len = 6;

    netif->linkoutput = ncm_linkoutput_fn;
    netif->output = ncm_netif_output_fn;
#if LWIP_IPV6
    netif->output_ip6 = ncm_netif_output_ip6_fn;
#endif
    return ERR_OK;
}

/* -------------------------------------------------------------------------
   app_main: configuración principal
------------------------------------------------------------------------- */
void app_main(void)
{
    // 1) NVS (para PHY calibration, etc.)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2) TinyUSB driver
    tinyusb_config_t tusb_cfg = { .external_phy = false };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "TinyUSB installed, starting NCM...");

    // 3) Inicializar NCM
    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer   = NULL,
        .user_context     = NULL,
    };
    // MAC arbitraria
    net_cfg.mac_addr[0] = 0x02;
    net_cfg.mac_addr[1] = 0x12;
    net_cfg.mac_addr[2] = 0x34;
    net_cfg.mac_addr[3] = 0x56;
    net_cfg.mac_addr[4] = 0x78;
    net_cfg.mac_addr[5] = 0x9A;

    ESP_ERROR_CHECK(tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg));
    ESP_LOGI(TAG, "NCM device init. MAC %02x:%02x:%02x:%02x:%02x:%02x",
             net_cfg.mac_addr[0], net_cfg.mac_addr[1],
             net_cfg.mac_addr[2], net_cfg.mac_addr[3],
             net_cfg.mac_addr[4], net_cfg.mac_addr[5]);

    // 4) lwIP init
    tcpip_init(NULL, NULL);

    // 5) Añadir la netif “ncm_netif” con IP 192.168.7.1
    ip4_addr_t ipaddr, netmask, gw;
    ip4addr_aton(USB_NET_IP, &ipaddr);
    ip4addr_aton(USB_NET_MASK, &netmask);
    ip4addr_aton(USB_NET_GW, &gw);

    netif_add(&ncm_netif, &ipaddr, &netmask, &gw,
              NULL, ncm_netif_init_cb, tcpip_input);
    netif_set_default(&ncm_netif);
    netif_set_up(&ncm_netif);

    // 6) Servidor DHCP
    static dhcp_entry_t dhcp_entries[] = {
        // Asignamos IP .2, .3, .4
        {{0}, { PP_HTONL(LWIP_MAKEU32(192,168,7,2)) }, 24*3600 },
        {{0}, { PP_HTONL(LWIP_MAKEU32(192,168,7,3)) }, 24*3600 },
        {{0}, { PP_HTONL(LWIP_MAKEU32(192,168,7,4)) }, 24*3600 },
    };
    const dhcp_config_t dhcp_cfg = {
        .router = {0},   // sin gateway real
        .port   = 67,   
        .dns    = { PP_HTONL(LWIP_MAKEU32(192,168,27,191)) }, // DNS = 192.168.1.247
        .domain = "badnet", // sufijo domain
        .num_entry = 3,
        .entries  = dhcp_entries
    };
    while (dhserv_init(&dhcp_cfg) != ERR_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "DHCP server ready (classless routes + WPAD).");
    ESP_LOGI(TAG, "Local IP: %s", USB_NET_IP);

    // 8) Bucle principal
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Si no hay tarea interna de TinyUSB, llama: tud_task();
        // Y si no hay tcpip_thread, llama: sys_check_timeouts();
    }
}
