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
 * WiFi network task - call periodically to process WiFi events
 * @param parameters Unused FreeRTOS parameter
 */
void network_task(void *parameters);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */
