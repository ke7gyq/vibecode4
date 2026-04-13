#ifndef INFRASTRUCTURE_H
#define INFRASTRUCTURE_H

#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <stdint.h>
#include "runnable.h"
#include "microphone.h"  /* For AudioBuffers_t and AUDIO_BUFFER_SIZE */

/**
 * Shared Infrastructure Definitions
 * 
 * This module owns and initializes all shared resources that multiple
 * modules depend on. Infrastructure initialization happens BEFORE module
 * initialization, so all modules can assume these resources exist.
 */

/**
 * Audio Queues - Message-based distribution
 * 
 * Two independent queues allow decoupled flow control:
 * - UDP task can drain at its own rate
 * - Waterfall task can drain at its own rate
 * - Microphone produces to both simultaneously
 */
extern QueueHandle_t g_audioQueueUDP;       // Queue for UDP audio transmission
extern QueueHandle_t g_audioQueueWaterfall; // Queue for waterfall display

/**
 * Audio Status and Buffers
 * 
 * g_audioBuffers    - Ping-pong pair of PCM buffers (528 samples each)
 * g_audioReady      - Status volatile: 0=none, 1=buffer1 ready, 2=buffer2 ready
 * g_audioReadySemaphore   - Binary semaphore signaled when buffer ready
 * g_audioSemaphoreUDP     - Semaphore for UDP task signaling
 */
extern AudioBuffers_t g_audioBuffers;
extern volatile uint8_t g_audioReady;
extern SemaphoreHandle_t g_audioReadySemaphore;
extern SemaphoreHandle_t g_audioSemaphoreUDP;

/* ============== Configuration Constants ============== */

/** Gain normalization factor: user_input / GAIN_NORMALIZATION = linear_gain */
#define GAIN_NORMALIZATION 10

/**
 * Microphone Debug Level
 * 0 = Silent (no output)
 * 2+ = Verbose (show status)
 * 4+ = Extra verbose (show statistics)
 */
extern uint8_t g_micDebug;

/* ============== Waterfall Gain Configuration ============== */

/**
 * Waterfall Gain Configuration
 * Linear gain value * GAIN_NORMALIZATION (e.g., 10 = 1.0x, 50 = 5.0x)
 * Owned by infrastructure, loaded from flash on startup
 */
extern uint32_t g_waterfallGainLinear;
extern uint32_t g_waterfallGainSquared;

/* ============== WiFi Credentials Configuration ============== */

/**
 * WiFi SSID and password storage
 * Owned by infrastructure, loaded from flash on startup
 * Max sizes: SSID 32 chars, password 63 chars (plus null terminator)
 */
extern char g_wifi_ssid[33];
extern char g_wifi_password[64];

/**
 * Get WiFi SSID
 * @return Pointer to SSID string (null-terminated)
 */
const char* getWifiSSID(void);

/**
 * Get WiFi password
 * @return Pointer to password string (null-terminated)
 */
const char* getWifiPassword(void);

/**
 * Set waterfall gain (linear value)
 * Computes and caches squared gain for efficient bar generation
 * @param gain Linear gain × GAIN_NORMALIZATION
 */
void setWaterfallGain(uint32_t gain);

/**
 * Get current waterfall gain (squared, precomputed for rendering)
 * @return Squared gain for bar generation
 */
uint32_t getWaterfallGain(void);

/**
 * Get current waterfall gain (linear, user-facing value)
 * @return Linear gain as set by user
 */
uint32_t getWaterfallGainLinear(void);

/**
 * Initialize all shared infrastructure
 * 
 * Creates:
 *   - Audio queues (g_audioQueueUDP, g_audioQueueWaterfall)
 *   - Audio semaphores (g_audioReadySemaphore, g_audioSemaphoreUDP)
 *   - Initializes audio buffers (g_audioBuffers)
 * 
 * Must be called BEFORE module initialization phase.
 * 
 * @return INIT_SUCCESS on success, error code on failure
 */
int infrastructure_init(void);

#endif /* INFRASTRUCTURE_H */
