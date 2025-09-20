/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 by Sergey Fetisov <fsenok@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * version: 1.0 demo (7.02.2015)
 * brief:   tiny dhcp ipv4 server using lwip (pcb)
 * ref:     https://lists.gnu.org/archive/html/lwip-users/2012-12/msg00016.html
 */

#ifndef DHSERVER_H
#define DHSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lwip/err.h"
#include "lwip/udp.h"
#include "netif/etharp.h"

typedef struct dhcp_entry
{
	uint8_t    mac[6];
	ip4_addr_t addr;
	uint32_t   lease;
} dhcp_entry_t;

typedef struct dhcp_route_option
{
        uint8_t    prefix_length;
        ip4_addr_t network;
        ip4_addr_t gateway;
} dhcp_route_option_t;

typedef struct dhcp_option_settings
{
        bool                       enable_routes;
        size_t                     route_count;
        const dhcp_route_option_t *routes;
        bool                       enable_wpad;
        const char                *wpad_url;
} dhcp_option_settings_t;

typedef struct dhcp_config
{
        ip4_addr_t                router;
        uint16_t                  port;
        ip4_addr_t                dns;
        const char               *domain;
        int                       num_entry;
        dhcp_entry_t             *entries;
        const dhcp_option_settings *options;
} dhcp_config_t;

#ifdef __cplusplus
extern "C" {
#endif
err_t dhserv_init(const dhcp_config_t *c);
void dhserv_free(void);
#ifdef __cplusplus
}
#endif

#endif /* DHSERVER_H */
