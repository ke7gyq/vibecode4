#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WiFi network information structure
 */
typedef struct {
    char ssid[33];           // SSID (max 32 chars + null terminator)
    uint8_t bssid[6];        // BSSID
    int8_t rssi;             // Signal strength (dBm)
    uint8_t channel;         // Channel number
    uint8_t security;        // Security type
} wifi_ap_t;

/**
 * Scan for available WiFi networks
 * Non-blocking initiation of WiFi scan
 */
void network_scan_start(void);

void wifi_init ( void);

/**
 * Check if WiFi scan is in progress
 * @return true if scan is active, false otherwise
 */
bool network_scan_is_active(void);

/**
 * Get number of APs found in last scan
 * @return Number of access points discovered
 */
uint32_t network_scan_get_count(void);

/**
 * Get AP info from scan results
 * @param index Index of AP (0 to count-1)
 * @param ap Pointer to wifi_ap_t structure to fill
 * @return 0 on success, non-zero on error
 */
int network_scan_get_ap(uint32_t index, wifi_ap_t *ap);

/**
 * Print scan results to console
 */
void network_scan_print_results(void);

/**
 * Attempt non-blocking WiFi connection
 * @param ssid WiFi network SSID
 * @param password WiFi network password
 * @return 0 on initiation success, non-zero on error
 */
int network_connect(const char *ssid, const char *password);

/**
 * Check WiFi connection status
 * @return 1 if connected, 0 if not connected, <0 for error
 */
int network_is_connected(void);

/**
 * Get current WiFi connection RSSI (signal strength)
 * @return RSSI in dBm (negative value), or -999 if not connected
 */
int network_get_rssi(void);

/**
 * Get current IP address assigned by DHCP
 * @param ip_str Buffer to store IP address string (recommend at least 16 bytes)
 * @param len Length of ip_str buffer
 * @return 1 if valid IP assigned, 0 if not connected or no IP yet
 */
int network_get_ip_address(char *ip_str, int len);

/**
 * Print detailed network interface diagnostics
 * Shows netif flags, RX/TX stats, link status, etc.
 */
void network_print_netif_status(void);

/**
 * WiFi network task - call periodically to process WiFi events
 * @param parameters Unused FreeRTOS parameter
 */
void network_task(void *parameters);

/**
 * Save WiFi credentials to flash (persistent storage)
 * @param ssid WiFi network name (max 32 chars)
 * @param password WiFi password (max 63 chars)
 * @return 0 on success, non-zero on error
 */
int network_credentials_save(const char *ssid, const char *password);

/**
 * Load WiFi credentials from flash
 * @param ssid Buffer to fill with SSID (must be at least 33 bytes)
 * @param password Buffer to fill with password (must be at least 64 bytes)
 * @return 1 if credentials found, 0 if not found/invalid
 */
int network_credentials_load(char *ssid, char *password);

/**
 * Clear saved WiFi credentials from flash
 * @return 0 on success, non-zero on error
 */
int network_credentials_clear(void);

/**
 * Check if WiFi credentials are saved
 * @return 1 if valid credentials exist, 0 otherwise
 */
int network_credentials_exist(void);

/* ==================== Waterfall Configuration Functions ==================== */

/**
 * Save waterfall gain configuration to flash
 * @param waterfall_gain Linear gain × GAIN_NORMALIZATION
 * @return 0 on success, non-zero on error
 */
int waterfall_config_save_gain(uint32_t waterfall_gain);

/**
 * Load waterfall gain configuration from flash
 * @param waterfall_gain Pointer to variable to fill with waterfall gain
 * @return 1 if valid config found, 0 if not found or invalid
 */
int waterfall_config_load(uint32_t *waterfall_gain);

/**
 * Save waterfall display configuration (color and mode) to flash
 * @param waterfall_color Color palette index (uint16_t)
 * @param waterfall_mode Display mode (0=off, 1=TEST, 2=LIVE_AUDIO)
 * @return 0 on success, non-zero on error
 */
int waterfall_config_save_display(uint16_t waterfall_color, uint8_t waterfall_mode);

/**
 * Load waterfall display configuration from flash
 * @param waterfall_color Pointer to variable to fill with color
 * @param waterfall_mode Pointer to variable to fill with mode
 * @return 1 if valid config found, 0 if not found or invalid
 */
int waterfall_config_load_display(uint16_t *waterfall_color, uint8_t *waterfall_mode);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */
