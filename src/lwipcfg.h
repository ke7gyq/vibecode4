#ifndef LWIPCFG_H
#define LWIPCFG_H

/*
 * lwIP configuration for Pico W
 * This file customizes lwIP behavior for our specific application needs
 */

// Enable features we need
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_RAW 1

// DHCP configuration
#define LWIP_DHCP 1
#define LWIP_AUTOIP 0

// DNS resolution
#define LWIP_DNS 1
#define DNS_MAX_SERVERS 2

// Socket API
#define LWIP_SOCKET 1
#define LWIP_COMPAT_SOCKETS 1
#define LWIP_POSIX_SOCKETS_IO_NAMES 1

// Network interfaces
#define LWIP_NETIF_API 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1

// Memory configuration
#define MEM_SIZE (8 * 1024)  // Reduced heap for embedded system
#define PBUF_POOL_SIZE 16
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define TCP_WND (4*TCP_MSS)

// Reduce default wait times for embedded systems
#define TCP_MSL 60000U  // 60 seconds
#define TCP_SYNMAXRTX 6
#define TCP_MAXRTX 12

// Debug - set to 0 to disable debug output
#define LWIP_DEBUG 0

#endif /* LWIPCFG_H */
