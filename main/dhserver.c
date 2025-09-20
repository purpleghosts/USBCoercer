/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 by Sergey Fetisov
 * Modificado por ChatGPT para incluir Rutas estáticas /32 y Opción WPAD.
 */

#include "dhserver.h"

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"

/* DHCP message type */
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_RELEASE        7
#define DHCP_INFORM         8

/* DHCP options */
enum DHCP_OPTIONS
{
    DHCP_PAD                    = 0,
    DHCP_SUBNETMASK             = 1,
    DHCP_ROUTER                 = 3,
    DHCP_DNSSERVER              = 6,
    DHCP_HOSTNAME               = 12,
    DHCP_DNSDOMAIN              = 15,
    DHCP_LEASETIME              = 51,
    DHCP_MESSAGETYPE            = 53,
    DHCP_SERVERID               = 54,
    DHCP_END                    = 255,

    /* Opciones que nos interesan */
    DHCP_CLASSLESSROUTE         = 121, /* RFC3442: Classless Static Route */
    DHCP_WPAD                   = 252, /* WPAD (en Windows) */
};

typedef struct
{
    uint8_t  dp_op;           /* packet opcode type */
    uint8_t  dp_htype;        /* hardware addr type */
    uint8_t  dp_hlen;         /* hardware addr length */
    uint8_t  dp_hops;         /* gateway hops */
    uint32_t dp_xid;          /* transaction ID */
    uint16_t dp_secs;         /* seconds since boot began */
    uint16_t dp_flags;
    uint8_t  dp_ciaddr[4];    /* client IP address */
    uint8_t  dp_yiaddr[4];    /* 'your' IP address */
    uint8_t  dp_siaddr[4];    /* server IP address */
    uint8_t  dp_giaddr[4];    /* gateway IP address */
    uint8_t  dp_chaddr[16];   /* client hardware address */
    uint8_t  dp_legacy[192];
    uint8_t  dp_magic[4];
    uint8_t  dp_options[275]; /* options area */
} DHCP_TYPE;

static DHCP_TYPE dhcp_data;
static struct udp_pcb *pcb = NULL;
static const dhcp_config_t *config = NULL;

static char magic_cookie[] = {0x63,0x82,0x53,0x63};

/* Rutas /32 -> 192.168.7.1 */
static const char *TAG = "DHCP_SERVER";

/* Estructuras helper */
static ip4_addr_t get_ip(const uint8_t *pnt)
{
    uint32_t value = ((uint32_t)pnt[0] << 24) |
                     ((uint32_t)pnt[1] << 16) |
                     ((uint32_t)pnt[2] << 8) |
                     ((uint32_t)pnt[3]);
    ip4_addr_t result;
    result.addr = lwip_htonl(value);
    return result;
}

static void set_ip(uint8_t *pnt, ip4_addr_t value)
{
    uint32_t host = lwip_ntohl(ip4_addr_get_u32(&value));
    pnt[0] = (host >> 24) & 0xFF;
    pnt[1] = (host >> 16) & 0xFF;
    pnt[2] = (host >> 8) & 0xFF;
    pnt[3] = host & 0xFF;
}

/* Buscar la IP en la tabla */
static dhcp_entry_t *entry_by_ip(ip4_addr_t ip)
{
    for (int i = 0; i < config->num_entry; i++)
        if (config->entries[i].addr.addr == ip.addr)
            return &config->entries[i];
    return NULL;
}

/* Buscar la MAC en la tabla */
static dhcp_entry_t *entry_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < config->num_entry; i++)
        if (memcmp(config->entries[i].mac, mac, 6) == 0)
            return &config->entries[i];
    return NULL;
}

static __inline bool is_vacant(const dhcp_entry_t *entry)
{
    static const uint8_t zero[6] = {0};
    return memcmp(entry->mac, zero, sizeof(entry->mac)) == 0;
}

static dhcp_entry_t *vacant_address(void)
{
    for (int i = 0; i < config->num_entry; i++)
        if (is_vacant(&config->entries[i]))
            return &config->entries[i];
    return NULL;
}

static __inline void free_entry(dhcp_entry_t *entry)
{
    memset(entry->mac, 0, 6);
}

/* Opciones extra: busca un “tag” */
static uint8_t *find_dhcp_option(uint8_t *attrs, int size, uint8_t attr)
{
    int i = 0;
    while ((i + 1) < size)
    {
        int next = i + attrs[i + 1] + 2;
        if (next > size) return NULL;
        if (attrs[i] == attr)
            return attrs + i;
        i = next;
    }
    return NULL;
}

/*
   fill_options(...):
   - Añade las opciones mínimas (server ID, lease time, subnet mask, router)
   - Añade classless route (Op121)
   - Añade WPAD (Op252)
*/
static int fill_options(
    void *dest,
    uint8_t msg_type,
    const dhcp_config_t *cfg,
    uint32_t lease_time,
    ip4_addr_t serverid,
    ip4_addr_t router,
    ip4_addr_t subnet)
{
    uint8_t *ptr = (uint8_t *)dest;

    // 1) Mensaje DHCP (Offer / Ack)
    *ptr++ = DHCP_MESSAGETYPE;
    *ptr++ = 1;
    *ptr++ = msg_type;

    // 2) Server ID
    *ptr++ = DHCP_SERVERID;
    *ptr++ = 4;
    set_ip(ptr, serverid);
    ptr += 4;

    // 3) Lease time
    *ptr++ = DHCP_LEASETIME;
    *ptr++ = 4;
    *ptr++ = (lease_time >> 24) & 0xFF;
    *ptr++ = (lease_time >> 16) & 0xFF;
    *ptr++ = (lease_time >> 8) & 0xFF;
    *ptr++ = (lease_time >> 0) & 0xFF;

    // 4) Subnet mask
    *ptr++ = DHCP_SUBNETMASK;
    *ptr++ = 4;
    set_ip(ptr, subnet);
    ptr += 4;

    // 5) Router (si != 0)
    if (router.addr != 0)
    {
        *ptr++ = DHCP_ROUTER;
        *ptr++ = 4;
        set_ip(ptr, router);
        ptr += 4;
    }

    // 6) Domain name
    if (cfg->domain && cfg->domain[0])
    {
        size_t len = strlen(cfg->domain);
        if (len > 255) len = 255;
        *ptr++ = DHCP_DNSDOMAIN;
        *ptr++ = (uint8_t)len;
        memcpy(ptr, cfg->domain, len);
        ptr += len;
    }

    // 7) DNS server
    if (cfg->dns.addr != 0)
    {
        *ptr++ = DHCP_DNSSERVER;
        *ptr++ = 4;
        set_ip(ptr, cfg->dns);
        ptr += 4;
    }

    // 8) Opción 121: Classless Static Routes
    if (cfg->options && cfg->options->enable_routes &&
        cfg->options->route_count > 0 && cfg->options->routes)
    {
        uint8_t *option_start = ptr;
        *ptr++ = DHCP_CLASSLESSROUTE;
        uint8_t *len_ptr = ptr++;
        uint16_t option_len = 0;

        for (size_t i = 0; i < cfg->options->route_count; ++i)
        {
            const dhcp_route_option_t *route = &cfg->options->routes[i];
            uint8_t prefix = route->prefix_length;
            if (prefix > 32)
            {
                ESP_LOGW(TAG, "Ignoring route with invalid prefix length %u", prefix);
                continue;
            }

            uint8_t prefix_bytes = (prefix + 7) / 8;
            if (prefix_bytes > 4)
            {
                prefix_bytes = 4;
            }

            if ((size_t)option_len + 1 + prefix_bytes + 4 > 255)
            {
                ESP_LOGW(TAG, "Classless routes option full, skipping remaining entries");
                break;
            }

            *ptr++ = prefix;
            option_len++;

            uint32_t network_host = lwip_ntohl(ip4_addr_get_u32(&route->network));
            for (uint8_t j = 0; j < prefix_bytes; ++j)
            {
                uint8_t shift = 24 - (j * 8);
                *ptr++ = (network_host >> shift) & 0xFF;
            }
            option_len += prefix_bytes;

            uint32_t gateway_host = lwip_ntohl(ip4_addr_get_u32(&route->gateway));
            for (uint8_t j = 0; j < 4; ++j)
            {
                uint8_t shift = 24 - (j * 8);
                *ptr++ = (gateway_host >> shift) & 0xFF;
            }
            option_len += 4;
        }

        if (option_len == 0)
        {
            ptr = option_start;
        }
        else
        {
            *len_ptr = (uint8_t)option_len;
        }
    }

    // 9) Opción 252: WPAD
    if (cfg->options && cfg->options->enable_wpad &&
        cfg->options->wpad_url && cfg->options->wpad_url[0])
    {
        size_t wlen = strlen(cfg->options->wpad_url);
        if (wlen > 255) wlen = 255;
        *ptr++ = DHCP_WPAD; // 252
        *ptr++ = (uint8_t)wlen;
        memcpy(ptr, cfg->options->wpad_url, wlen);
        ptr += wlen;
    }

    // 10) Fin
    *ptr++ = DHCP_END;
    return ptr - (uint8_t *)dest;
}

/*-----------------------------------------
   Procesar paquetes (Discover / Request)
-----------------------------------------*/
static void udp_recv_proc(void *arg, struct udp_pcb *upcb,
                          struct pbuf *p,
                          const ip_addr_t *addr, u16_t port)
{
    if (!config) {
        pbuf_free(p);
        return;
    }
    struct netif *netif = netif_get_by_index(p->if_idx);
    if (!netif) {
        pbuf_free(p);
        return;
    }

    unsigned n = p->len;
    if (n > sizeof(dhcp_data)) n = sizeof(dhcp_data);

    memcpy(&dhcp_data, p->payload, n);

    // Buscar la Opción 53 (Message Type)
    uint8_t *opt_ptr = find_dhcp_option(dhcp_data.dp_options, 
                                        sizeof(dhcp_data.dp_options),
                                        DHCP_MESSAGETYPE);
    if (!opt_ptr) {
        pbuf_free(p);
        return;
    }

    switch (opt_ptr[2])
    {
        case DHCP_DISCOVER:
        {
            dhcp_entry_t *entry = entry_by_mac(dhcp_data.dp_chaddr);
            if (!entry) entry = vacant_address();
            if (!entry) break;

            // Preparamos Offer
            dhcp_data.dp_op = 2; // reply
            dhcp_data.dp_secs = 0;
            dhcp_data.dp_flags = 0;
            set_ip(dhcp_data.dp_yiaddr, entry->addr);
            memcpy(dhcp_data.dp_magic, magic_cookie, 4);
            memset(dhcp_data.dp_options, 0, sizeof(dhcp_data.dp_options));

            fill_options(dhcp_data.dp_options,
                         DHCP_OFFER,
                         config,
                         entry->lease,
                         *netif_ip4_addr(netif),
                         config->router,
                         *netif_ip4_netmask(netif));

            struct pbuf *pp = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_data), PBUF_POOL);
            if (pp) {
                memcpy(pp->payload, &dhcp_data, sizeof(dhcp_data));
                udp_sendto(upcb, pp, IP_ADDR_BROADCAST, port);
                pbuf_free(pp);
            }
        }
        break;

        case DHCP_REQUEST:
        {
            // 1) Buscar requested IP (opción 50)
            uint8_t *ipreq = find_dhcp_option(dhcp_data.dp_options,
                                              sizeof(dhcp_data.dp_options),
                                              50 /* DHCP_IPADDRESS */);
            if (!ipreq) break;
            if (ipreq[1] != 4) break;
            ipreq += 2;

            // 2) Buscar en la tabla
            ip4_addr_t requested = get_ip(ipreq);
            dhcp_entry_t *entry = entry_by_ip(requested);
            if (!entry) break;
            if (!is_vacant(entry) && memcmp(entry->mac, dhcp_data.dp_chaddr, 6) != 0)
                break;

            // Preparamos ACK
            memcpy(dhcp_data.dp_yiaddr, ipreq, 4);
            dhcp_data.dp_op = 2; // reply
            dhcp_data.dp_secs = 0;
            dhcp_data.dp_flags = 0;
            memcpy(dhcp_data.dp_magic, magic_cookie, 4);
            memset(dhcp_data.dp_options, 0, sizeof(dhcp_data.dp_options));

            fill_options(dhcp_data.dp_options,
                         DHCP_ACK,
                         config,
                         entry->lease,
                         *netif_ip4_addr(netif),
                         config->router,
                         *netif_ip4_netmask(netif));

            struct pbuf *pp = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_data), PBUF_POOL);
            if (pp) {
                // Asociar la MAC
                memcpy(entry->mac, dhcp_data.dp_chaddr, 6);
                memcpy(pp->payload, &dhcp_data, sizeof(dhcp_data));
                udp_sendto(upcb, pp, IP_ADDR_BROADCAST, port);
                pbuf_free(pp);
            }
        }
        break;

        case DHCP_RELEASE:
        {
            dhcp_entry_t *entry = entry_by_mac(dhcp_data.dp_chaddr);
            if (entry) {
                free_entry(entry);
            }
        }
        break;

        default:
            break;
    }

    pbuf_free(p);
}

/*-------------------
   API de dhserver
--------------------*/
err_t dhserv_init(const dhcp_config_t *c)
{
    if (!c || !c->entries || c->num_entry <= 0) {
        return ERR_ARG;
    }

    dhserv_free(); // Limpia pcb previo
    pcb = udp_new();
    if (!pcb) return ERR_MEM;

    uint16_t port = c->port ? c->port : 67;
    err_t err = udp_bind(pcb, IP_ADDR_ANY, port);
    if (err != ERR_OK)
    {
        dhserv_free();
        return err;
    }

    config = c;
    udp_recv(pcb, udp_recv_proc, NULL);
    return ERR_OK;
}

void dhserv_free(void)
{
    if (pcb)
    {
        udp_remove(pcb);
        pcb = NULL;
    }
    config = NULL;
}
