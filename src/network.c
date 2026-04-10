#include <stdio.h>
#include <string.h>

// lwIP configuration must be included before any other lwIP headers
#include "lwipcfg.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "FreeRTOS.h"
#include "task.h"
#include "network.h"
#include "microphone.h"  /* For g_micDebug flag */
#include "tcp_server.h"  /* For udp_server_start() on WiFi connect */

// lwIP includes
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/sys.h"

// WiFi scan results storage
#define MAX_APS 20
/*static */wifi_ap_t g_scan_results[MAX_APS];
/*static */uint32_t g_scan_count = 0;
static bool g_scan_active = false;
static uint32_t g_scan_start_time = 0;          // Track when scan started
static const uint32_t WIFI_SCAN_TIMEOUT_MS = 20000;  // 20 second scan timeout

// WiFi connection state
typedef enum {
    WIFI_STATE_IDLE,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED
} wifi_state_t;

static wifi_state_t g_wifi_state = WIFI_STATE_IDLE;
static char g_target_ssid[33] = {0};
static char g_target_password[64] = {0};
static uint32_t g_connection_start_time = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;  // 30 second timeout (lwIP DHCP takes time)
static bool g_dhcp_started = false;
static bool g_udp_auto_started = false;  /* Track if UDP was auto-started on WiFi connect */
static uint32_t g_udp_startup_time = 0;  /* When to start UDP (non-blocking delay) */
static const uint32_t UDP_START_DELAY_MS = 2000;  /* 2-second delay for netif initialization */

/* ==================== WiFi Credentials Flash Storage ==================== */

#define WIFI_CREDENTIALS_MAGIC 0x57494649  /* 'WIFI' in ASCII */
#define WIFI_CREDENTIALS_VERSION 1

/* Credentials structure stored in flash */
typedef struct {
    uint32_t magic;              /* Magic number for validation */
    uint8_t version;             /* Structure version */
    uint8_t reserved[3];         /* Padding */
    char ssid[33];               /* SSID (max 32 chars + null) */
    char password[64];           /* Password (max 63 chars + null) */
    uint32_t crc32;              /* CRC32 checksum */
} wifi_credentials_t;

/* Flash storage: use last 4KB of flash for credentials (before end of storage) */
/* RP2350 has 8MB or 16MB flash depending on variant */
#define WIFI_CREDENTIALS_OFFSET (PICO_FLASH_SIZE_BYTES - 4096)

/**
 * Simple CRC32 calculation
 */
static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/**
 * Callback for WiFi scan results
 * Called by cyw43 driver when scan process encounters an AP or completes
 */
static int scan_result_callback(void *env, const cyw43_ev_scan_result_t *result) {
    if (result == NULL) {
        // Scan complete
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - g_scan_start_time;
        printf("WiFi scan complete. Found %u networks (elapsed: %ums)\n", g_scan_count, elapsed);
        g_scan_active = false;
        return 0;
    }
    
    // Store result if we have room
    if (g_scan_count < MAX_APS) {
        wifi_ap_t *ap = &g_scan_results[g_scan_count];
        strncpy(ap->ssid, (const char *)result->ssid, sizeof(ap->ssid) - 1);
        ap->ssid[sizeof(ap->ssid) - 1] = '\0';
        memcpy(ap->bssid, result->bssid, 6);
        ap->rssi = result->rssi;
        ap->channel = result->channel;
        ap->security = result->auth_mode;
        printf("  AP found: %s (rssi: %d, ch: %u)\n", ap->ssid, ap->rssi, ap->channel);
        g_scan_count++;
    } else {
        printf("WARNING: Max APs reached (%d), ignoring additional results\n", MAX_APS);
    }
    
    return 0;
}

/**
 * Initialize WiFi hardware
 */
void wifi_init(void) {
    static bool initialized = false;
    if (initialized) return;
    
    printf("Initializing cyw43 with lwIP...\n");
    if (cyw43_arch_init()) {
        printf("Failed to initialize cyw43\n");
        return;
    }
    
    printf("Enabling WiFi STA mode...\n");
    cyw43_arch_enable_sta_mode();
    
    initialized = true;
    printf("WiFi initialized with lwIP support\n");
}

/**
 * Start a WiFi scan
 */
void network_scan_start(void) {
    if (g_scan_active) {
        printf("Scan already in progress\n");
        return;
    }
    
    wifi_init();  // Ensure WiFi is initialized
    
    printf("Starting WiFi scan...\n");
    g_scan_count = 0;
    g_scan_active = true;
    g_scan_start_time = to_ms_since_boot(get_absolute_time());  // Record start time
    
    // Initiate scan using the cyw43 API
    cyw43_wifi_scan_options_t scan_opts = {0};
    int result = cyw43_wifi_scan(&cyw43_state, &scan_opts, NULL, scan_result_callback);
    if (result != 0) {
        printf("WiFi scan failed to start (error: %d)\n", result);
        g_scan_active = false;
    } else {
        printf("WiFi scan initiated successfully. Waiting for results...\n");
    }
}

/**
 * Check if scan is active
 */
bool network_scan_is_active(void) {
    return g_scan_active;
}

/**
 * Get number of APs found
 */
uint32_t network_scan_get_count(void) {
    return g_scan_count;
}

/**
 * Get AP info by index
 */
int network_scan_get_ap(uint32_t index, wifi_ap_t *ap) {
    if (index >= g_scan_count || ap == NULL) {
        return -1;
    }
    memcpy(ap, &g_scan_results[index], sizeof(wifi_ap_t));
    return 0;
}

/**
 * Print scan results
 */
void network_scan_print_results(void) {
    printf("\n=== WiFi Networks Found ===\n");
    printf("%-32s RSSI Ch Auth\n", "SSID");
    printf("--------------------------------\n");
    
    for (uint32_t i = 0; i < g_scan_count; i++) {
        wifi_ap_t *ap = &g_scan_results[i];
        printf("%-32s %4d %2u %u\n", ap->ssid, ap->rssi, ap->channel, ap->security);
    }
    printf("\nTotal: %u networks\n\n", g_scan_count);
}

/**
 * Attempt WiFi connection (non-blocking with lwIP DHCP)
 */
int network_connect(const char *ssid, const char *password) {
    if (ssid == NULL || password == NULL) {
        printf("Invalid SSID or password\n");
        return -1;
    }
    
    wifi_init();  // Ensure WiFi hardware is initialized
    
    strncpy(g_target_ssid, ssid, sizeof(g_target_ssid) - 1);
    g_target_ssid[sizeof(g_target_ssid) - 1] = '\0';
    
    strncpy(g_target_password, password, sizeof(g_target_password) - 1);
    g_target_password[sizeof(g_target_password) - 1] = '\0';
    
    printf("Attempting WiFi connection to: %s\n", g_target_ssid);
    g_wifi_state = WIFI_STATE_CONNECTING;
    g_connection_start_time = to_ms_since_boot(get_absolute_time());
    g_dhcp_started = false;
    g_udp_auto_started = false;  /* Reset UDP flag for this connection attempt */
    
    // Initiate the connection with password
    int result = cyw43_arch_wifi_connect_async(g_target_ssid, g_target_password, CYW43_AUTH_WPA2_AES_PSK);
    
    if (result == 0) {
        printf("Connection initiated. Polling for status and DHCP...\n");
        return 0;
    } else {
        printf("Connection async initiation returned: %d\n", result);
        g_wifi_state = WIFI_STATE_FAILED;
        return result;
    }
}

/**
 * Check WiFi connection status with IP address
 */
int network_is_connected(void) {
    if (g_wifi_state != WIFI_STATE_CONNECTING) {
        return (g_wifi_state == WIFI_STATE_CONNECTED) ? 1 : 0;
    }
    
    // Check timeout
    uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - g_connection_start_time;
    if (elapsed > WIFI_CONNECT_TIMEOUT_MS) {
        printf("WiFi connection timeout\n");
        g_wifi_state = WIFI_STATE_FAILED;
        return 0;
    }
    
    // Check actual WiFi link status from cyw43 driver
    int link_status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    
    // Log status periodically
    static uint32_t last_status_check = 0;
    if ((elapsed - last_status_check) >= 1000 || last_status_check == 0) {
        last_status_check = elapsed;
        const char *status_str = "UNKNOWN";
        switch (link_status) {
            case CYW43_LINK_DOWN:    status_str = "DOWN"; break;
            case CYW43_LINK_JOIN:    status_str = "JOINED"; break;
            case CYW43_LINK_NOIP:    status_str = "NOIP"; break;
            case CYW43_LINK_UP:      status_str = "UP"; break;
            case CYW43_LINK_FAIL:    status_str = "FAIL"; break;
            case CYW43_LINK_NONET:   status_str = "NONET"; break;
            case CYW43_LINK_BADAUTH: status_str = "BADAUTH"; break;
        }
        
        // Get the network interface - try multiple names
        struct netif *netif = netif_find("cyw");
        if (netif == NULL) {
            // Try default interface
            netif = netif_list;
        }
        
        if (netif != NULL) {
            // If we're JOINED and haven't started DHCP, do it now
            if (link_status == CYW43_LINK_JOIN && !g_dhcp_started) {
                printf("WiFi AP joined. netif: %c%c (flags: 0x%x), starting DHCP\n", 
                       netif->name[0], netif->name[1], netif->flags);
                
                // Start DHCP client on this interface
                dhcp_start(netif);
                printf("DHCP client started\n");
                g_dhcp_started = true;
            }
            
            // Check if we have an IP address via DHCP
            if (!ip4_addr_isany_val(netif->ip_addr)) {
                printf("DHCP successful - IP: %s\n", ip4addr_ntoa(&netif->ip_addr));
                printf("WiFi connected successfully\n");
                g_wifi_state = WIFI_STATE_CONNECTED;
                return 1;
            }
        } else {
            // Network interface not yet created
            if ((elapsed - last_status_check) >= 1000 && link_status == CYW43_LINK_JOIN) {
                printf("  (waiting for netif...)\n");
            }
        }
        
        printf("Connection attempt in progress... (%ums elapsed, status=%s)\n", elapsed, status_str);
    }
    
    // Check for connection errors
    if (link_status == CYW43_LINK_FAIL) {
        printf("WiFi connection failed\n");
        g_wifi_state = WIFI_STATE_FAILED;
        return 0;
    }
    if (link_status == CYW43_LINK_NONET) {
        printf("WiFi SSID not found\n");
        g_wifi_state = WIFI_STATE_FAILED;
        return 0;
    }
    if (link_status == CYW43_LINK_BADAUTH) {
        printf("WiFi authentication failed\n");
        g_wifi_state = WIFI_STATE_FAILED;
        return 0;
    }
    
    return 0;  // Still attempting connection
}

/**
 * Get current WiFi connection RSSI (signal strength)
 */
int network_get_rssi(void) {
    // Only return RSSI if we are connected
    if (network_is_connected() != 1) {
        return -999;  // Not connected
    }
    
    // Query RSSI from cyw43 driver
    // Note: This is a simple implementation; actual RSSI may require 
    // additional cyw43 API calls or ioctl commands
    int rssi = -999;
    
    // Try to get RSSI using cyw43 - may not work on all platforms
    // For RP2350 with CYW43, we might need to use a dedicated IOCTL
    // For now, return -999 to indicate not available, but we can improve this
    // if the cyw43 driver has a public RSSI query function
    
    // TODO: Check cyw43 documentation for RSSI query method
    // Common approaches:
    // 1. cyw43_wifi_get_rssi() - if available
    // 2. ioctl(CYW43_GET_RSSI, ...) - if available  
    // 3. Status from WiFi scan results - if connected to recently scanned AP
    
    return rssi;
}

/**
 * Get current IP address assigned by DHCP
 * Copies IP address string to provided buffer
 */
int network_get_ip_address(char *ip_str, int len) {
    if (ip_str == NULL || len < 16) {
        return 0;  // Invalid buffer
    }
    
    // Only return IP if we are connected
    if (network_is_connected() != 1) {
        snprintf(ip_str, len, "NOT CONNECTED");
        return 0;
    }
    
    // Get the network interface
    struct netif *netif = netif_find("cyw");
    if (netif == NULL) {
        netif = netif_list;
    }
    
    if (netif == NULL) {
        snprintf(ip_str, len, "NO INTERFACE");
        return 0;
    }
    
    // Check if we have a valid IP address
    if (ip4_addr_isany_val(netif->ip_addr)) {
        snprintf(ip_str, len, "WAITING FOR DHCP");
        return 0;
    }
    
    // Copy IP address string
    const char *ip_cstr = ip4addr_ntoa(&netif->ip_addr);
    snprintf(ip_str, len, "%s", ip_cstr);
    return 1;
}

/**
 * Print detailed network interface diagnostics
 * Shows netif flags, link status, etc.
 */
void network_print_netif_status(void) {
    struct netif *netif = netif_find("cyw");
    if (netif == NULL) {
        netif = netif_list;
    }
    
    if (netif == NULL) {
        printf("[NETIF] No network interface found\n");
        return;
    }
    
    printf("\n[NETIF] Network Interface: %c%c\n", netif->name[0], netif->name[1]);
    printf("[NETIF] IP Address: %s\n", ip4addr_ntoa(&netif->ip_addr));
    printf("[NETIF] Gateway: %s\n", ip4addr_ntoa(&netif->gw));
    printf("[NETIF] Netmask: %s\n", ip4addr_ntoa(&netif->netmask));
    printf("[NETIF] Flags: 0x%x\n", netif->flags);
    printf("[NETIF]   - UP: %s\n", (netif->flags & NETIF_FLAG_UP) ? "YES" : "NO");
    printf("[NETIF]   - BROADCAST: %s\n", (netif->flags & NETIF_FLAG_BROADCAST) ? "YES" : "NO");
    printf("[NETIF]   - LINK_UP: %s\n", (netif->flags & NETIF_FLAG_LINK_UP) ? "YES" : "NO");
    printf("[NETIF] MTU: %u\n", netif->mtu);
    printf("[NETIF] hwaddr_len: %u\n", netif->hwaddr_len);
    printf("\n");
}

/**
 * Polls WiFi status and lwIP timers in background
 */
void network_task(void *parameters) {
    (void)parameters;  // Unused
    
    printf("Network task started\n");
    
    /* Try to auto-connect with saved credentials */
    static bool auto_connect_attempted = false;
    if (!auto_connect_attempted) {
        auto_connect_attempted = true;
        if (network_credentials_exist()) {
            char ssid[33], password[64];
            if (network_credentials_load(ssid, password)) {
                printf("[NETWORK] Auto-connecting with saved credentials (SSID: %s)\n", ssid);
                network_connect(ssid, password);
            }
        }
    }
    
    // Periodic polling loop
    uint32_t last_check = to_ms_since_boot(get_absolute_time());
    uint32_t poll_count = 0;
    uint32_t last_diagnostic = to_ms_since_boot(get_absolute_time());
    
    while (1) {
        // Poll WiFi driver to process events (includes lwIP integration)
        // In pico_cyw43_arch_lwip_poll mode, cyw43_arch_poll() handles both 
        // WiFi driver and lwIP timer updates automatically
        cyw43_arch_poll();
        poll_count++;
        
        // Every 1 second, check connection status and scan timeout
        uint32_t now = to_ms_since_boot(get_absolute_time());
        
        if ((now - last_check) >= 1000) {
            last_check = now;
            
            // Check scan timeout
            if (g_scan_active) {
                uint32_t scan_elapsed = now - g_scan_start_time;
                if (scan_elapsed > WIFI_SCAN_TIMEOUT_MS) {
                    printf("WiFi scan timeout (elapsed: %ums). Stopping scan.\n", scan_elapsed);
                    g_scan_active = false;
                }
            }
            
            if (g_wifi_state == WIFI_STATE_CONNECTING) {
                // Check if connection succeeded
                int connected = network_is_connected();
                
                // When WiFi first connects, set a timer for UDP startup
                if (connected && g_udp_startup_time == 0) {
                    g_udp_startup_time = now + UDP_START_DELAY_MS;
                    printf("[NETWORK] WiFi connected! UDP will auto-start in %lu ms\n", UDP_START_DELAY_MS);
                    fflush(stdout);
                }
            }
            
            // Check if it's time to start UDP (independent of WiFi state)
            // This runs every 1 second after a timer was set
            if (g_udp_startup_time > 0 && !g_udp_auto_started && now >= g_udp_startup_time) {
                g_udp_auto_started = true;
                g_udp_startup_time = 0;  // Reset for next time
                
                printf("[NETWORK] Network interface ready. Auto-starting UDP server...\n");
                fflush(stdout);
                if (udp_server_start() == 0) {
                    printf("[NETWORK] UDP server auto-started successfully\n");
                } else {
                    printf("[NETWORK] WARNING: UDP server failed to auto-start\n");
                }
                fflush(stdout);
            }
        }
        
        // Every 5 seconds, print poll diagnostics
        if ((now - last_diagnostic) >= 5000) {
            last_diagnostic = now;
            if (g_micDebug >= 2) {
                printf("[NETWORK] Poll frequency: %lu polls/5s (every %.1fms avg)\n", 
                       poll_count, poll_count > 0 ? 5000.0f / poll_count : 0.0f);
            }
            poll_count = 0;
        }
        
        // Small delay - 20ms for responsive lwIP/DHCP polling
        // lwIP needs regular polling for timers and DHCP state machine
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ==================== WiFi Credentials Flash Storage Implementation ==================== */

int network_credentials_save(const char *ssid, const char *password) {
    if (ssid == NULL || password == NULL) {
        printf("ERROR: Invalid SSID or password pointer\n");
        return -1;
    }
    
    if (strlen(ssid) > 32 || strlen(password) > 63) {
        printf("ERROR: SSID too long (max 32) or password too long (max 63)\n");
        return -1;
    }
    
    wifi_credentials_t creds;
    memset(&creds, 0xFF, sizeof(creds));  /* Flash erase pattern */
    
    /* Fill structure */
    creds.magic = WIFI_CREDENTIALS_MAGIC;
    creds.version = WIFI_CREDENTIALS_VERSION;
    strncpy(creds.ssid, ssid, 32);
    creds.ssid[32] = '\0';
    strncpy(creds.password, password, 63);
    creds.password[63] = '\0';
    
    /* Calculate CRC over data before CRC field */
    creds.crc32 = crc32_calc((uint8_t*)&creds, offsetof(wifi_credentials_t, crc32));
    
    /* Write to flash using FreeRTOS critical section */
    printf("[FLASH] Saving WiFi credentials to flash at offset 0x%x\n", WIFI_CREDENTIALS_OFFSET);
    
    /* Use FreeRTOS critical section instead of raw interrupt disable */
    taskENTER_CRITICAL();
    {
        flash_range_erase(WIFI_CREDENTIALS_OFFSET, 4096);  /* Erase 4KB sector */
        flash_range_program(WIFI_CREDENTIALS_OFFSET, (uint8_t*)&creds, sizeof(creds));
    }
    taskEXIT_CRITICAL();
    
    printf("[FLASH] WiFi credentials saved successfully\n");
    return 0;
}

int network_credentials_load(char *ssid, char *password) {
    if (ssid == NULL || password == NULL) {
        printf("ERROR: Invalid output buffer pointer\n");
        return 0;
    }
    
    /* Read from flash (XIP region, direct memory access) */
    const wifi_credentials_t *creds = (const wifi_credentials_t *)(XIP_BASE + WIFI_CREDENTIALS_OFFSET);
    
    /* Validate magic and version */
    if (creds->magic != WIFI_CREDENTIALS_MAGIC) {
        if (creds->magic != 0xFFFFFFFF) {  /* Not erased, but wrong magic */
            printf("[FLASH] Invalid credentials magic: 0x%x (expected 0x%x)\n", 
                   creds->magic, WIFI_CREDENTIALS_MAGIC);
        }
        return 0;  /* Not found */
    }
    
    if (creds->version != WIFI_CREDENTIALS_VERSION) {
        printf("[FLASH] Unsupported credential version: %u\n", creds->version);
        return 0;
    }
    
    /* Validate CRC */
    uint32_t expected_crc = crc32_calc((uint8_t*)creds, offsetof(wifi_credentials_t, crc32));
    if (expected_crc != creds->crc32) {
        printf("[FLASH] Credentials CRC mismatch: got 0x%x, expected 0x%x\n", 
               creds->crc32, expected_crc);
        return 0;  /* Corrupted */
    }
    
    /* Extract credentials */
    strncpy(ssid, creds->ssid, 32);
    ssid[32] = '\0';
    strncpy(password, creds->password, 63);
    password[63] = '\0';
    
    printf("[FLASH] WiFi credentials loaded: SSID='%s'\n", ssid);
    return 1;  /* Success */
}

int network_credentials_clear(void) {
    printf("[FLASH] Clearing WiFi credentials from flash\n");
    
    /* Use FreeRTOS critical section instead of raw interrupt disable */
    taskENTER_CRITICAL();
    {
        flash_range_erase(WIFI_CREDENTIALS_OFFSET, 4096);  /* Erase 4KB sector */
    }
    taskEXIT_CRITICAL();
    
    printf("[FLASH] WiFi credentials cleared\n");
    return 0;
}

int network_credentials_exist(void) {
    const wifi_credentials_t *creds = (const wifi_credentials_t *)(XIP_BASE + WIFI_CREDENTIALS_OFFSET);
    
    return (creds->magic == WIFI_CREDENTIALS_MAGIC && creds->version == WIFI_CREDENTIALS_VERSION);
}
