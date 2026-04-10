#ifndef UDP_SERVER_H
#define UDP_SERVER_H

#include <stdint.h>

/**
 * Start the UDP server that sends audio frames to connected clients
 * Server listens on port 5001 and accepts UDP datagrams
 * Returns 0 on success, non-zero on error
 */
int udp_server_start(void);

/**
 * Stop the UDP server
 */
void udp_server_stop(void);

/**
 * UDP Audio Task - FreeRTOS task that sends audio frames when notified
 * Create this as a FreeRTOS task in main:
 *   xTaskCreate(udp_audio_task, "UdpAudio", 2048, NULL, 2, NULL);
 * The task waits for timer interrupt notifications and sends frames to UDP clients
 * Requires: udp_server_start() must be called first to initialize the timer
 */
void udp_audio_task(void *parameters);

/**
 * Get server status
 * Returns 1 if running, 0 if stopped
 */
int udp_server_is_running(void);

/**
 * Print UDP server diagnostics
 * Shows connected clients, packets received, and audio transmission stats
 */
void udp_server_print_diagnostics(void);

/**
 * Write audio frame to shared buffer and signal all connected clients
 * Called from microphone ISR context
 * Returns number of samples written (may be less if buffer full)
 */
uint32_t udp_server_write_audio_frame(const int16_t *audio_data, uint32_t sample_count);

#endif
