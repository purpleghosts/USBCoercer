/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 by Sergey Fetisov
 * Modificado por ChatGPT para incluir Rutas estáticas /32 y Opción WPAD.
 */

#include "dhserver.h"

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
static const uint8_t g_classless_routes[] =
{
    // Estructura: [prefijo] [dest] [gateway]
    // 1) 3.121.6.180/32 -> 192.168.7.1
    32, 3, 121, 6, 180,    192,168,7,1,
    // 2) 3.121.187.176/32 -> 192.168.7.1
    32, 3, 121,187,176,    192,168,7,1,
    // 3) 3.121.238.86/32 -> 192.168.7.1
    32, 3, 121,238, 86,    192,168,7,1,
    // 4) 3.125.15.130/32 -> 192.168.7.1
    32, 3, 125, 15,130,    192,168,7,1,
    // 5) 18.158.187.80/32 -> 192.168.7.1
    32, 18,158,187, 80,    192,168,7,1,
    // 6) 18.198.53.88/32 -> 192.168.7.1
    32, 18,198, 53, 88,    192,168,7,1,
};

/* WPAD URL */
static const char g_wpad_url[] = "http://192.168.1.175/wpad.dat";

/* Estructuras helper */
static ip4_addr_t get_ip(const uint8_t *pnt)
{
    ip4_addr_t result;
    memcpy(&result, pnt, sizeof(result));
    return result;
}

static void set_ip(uint8_t *pnt, ip4_addr_t value)
{
    memcpy(pnt, &value.addr, sizeof(value.addr));
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
static dhcp_entry_t *entry_by_mac(uint8_t *mac)
{
    for (int i = 0; i < config->num_entry; i++)
        if (memcmp(config->entries[i].mac, mac, 6) == 0)
            return &config->entries[i];
    return NULL;
}

static __inline bool is_vacant(dhcp_entry_t *entry)
{
    return memcmp("\0\0\0\0\0", entry->mac, 5) == 0;
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
    const char *domain,
    ip4_addr_t dns,
    int lease_time,
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
    if (domain && domain[0])
    {
        uint8_t len = (uint8_t)strlen(domain);
        *ptr++ = DHCP_DNSDOMAIN;
        *ptr++ = len;
        memcpy(ptr, domain, len);
        ptr += len;
    }

    // 7) DNS server
    if (dns.addr != 0)
    {
        *ptr++ = DHCP_DNSSERVER;
        *ptr++ = 4;
        set_ip(ptr, dns);
        ptr += 4;
    }

    // 8) Opción 121: Classless Static Routes
    //    (si quieres siempre enviarlo)
    {
        const uint8_t routes_len = sizeof(g_classless_routes);
        *ptr++ = DHCP_CLASSLESSROUTE; // 121
        *ptr++ = routes_len; 
        memcpy(ptr, g_classless_routes, routes_len);
        ptr += routes_len;
    }

    // 9) Opción 252: WPAD
    {
        uint8_t wlen = (uint8_t)strlen(g_wpad_url);
        *ptr++ = DHCP_WPAD; // 252
        *ptr++ = wlen;
        memcpy(ptr, g_wpad_url, wlen);
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
            // Buscar si la MAC ya está en la tabla
            // (no implementado “entry_by_mac” en tu snippet, corrige si lo deseas)
            dhcp_entry_t *entry = NULL;
            //entry = entry_by_mac(dhcp_data.dp_chaddr);
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
                         config->domain,
                         config->dns,
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
            if (!is_vacant(entry)) break;

            // Preparamos ACK
            memcpy(dhcp_data.dp_yiaddr, ipreq, 4);
            dhcp_data.dp_op = 2; // reply
            dhcp_data.dp_secs = 0;
            dhcp_data.dp_flags = 0;
            memcpy(dhcp_data.dp_magic, magic_cookie, 4);
            memset(dhcp_data.dp_options, 0, sizeof(dhcp_data.dp_options));

            fill_options(dhcp_data.dp_options,
                         DHCP_ACK,
                         config->domain,
                         config->dns,
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
    dhserv_free(); // Limpia pcb previo
    pcb = udp_new();
    if (!pcb) return ERR_MEM;

    err_t err = udp_bind(pcb, IP_ADDR_ANY, c->port);
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
}
