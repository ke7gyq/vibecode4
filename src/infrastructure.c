#include "infrastructure.h"
#include "microphone.h"
#include <stdio.h>
#include <string.h>
#include "runnable.h"

/**
 * Shared Infrastructure Implementation
 * 
 * Initializes and owns all resources needed by multiple modules.
 * Created during Phase 0 (before module initialization).
 */

/* Audio Queues */
QueueHandle_t g_audioQueueUDP = NULL;
QueueHandle_t g_audioQueueWaterfall = NULL;

/* Audio Buffers and Status */
AudioBuffers_t g_audioBuffers = {0};
volatile uint8_t g_audioReady = 0;

/* Audio Synchronization Primitives */
SemaphoreHandle_t g_audioReadySemaphore = NULL;
SemaphoreHandle_t g_audioSemaphoreUDP = NULL;

/* Debugging */
uint8_t g_micDebug = 0;

/* Waterfall Gain Configuration */
uint32_t g_waterfallGainLinear = 75;      /* Linear gain value (default 75) */
uint32_t g_waterfallGainSquared = 56;    /* Precomputed squared gain: (75/10)² = 56.25 → 56 */

/* WiFi Credentials Configuration */
char g_wifi_ssid[33] = {0};       /* WiFi SSID (max 32 chars + null) */
char g_wifi_password[64] = {0};   /* WiFi password (max 63 chars + null) */

/**
 * Compute squared gain from linear gain value
 * @param gain Linear gain × GAIN_NORMALIZATION
 * @return Squared gain = (gain / GAIN_NORMALIZATION)²
 */
static uint32_t compute_gain_squared(uint32_t gain) {
    if (gain == 0) return 0;
    uint64_t gain_sq = (uint64_t)gain * gain;
    uint64_t divisor = (uint64_t)GAIN_NORMALIZATION * GAIN_NORMALIZATION;
    uint64_t result = gain_sq / divisor;
    return (result > UINT32_MAX) ? UINT32_MAX : (uint32_t)result;
}

/**
 * Set waterfall gain and precompute squared value
 * @param gain Linear gain × GAIN_NORMALIZATION
 */
void setWaterfallGain(uint32_t gain) {
    if (gain > 0) {
        g_waterfallGainLinear = gain;
        g_waterfallGainSquared = compute_gain_squared(gain);
    }
}

/**
 * Get current waterfall gain (squared, precomputed)
 * @return Squared gain for bar generation
 */
uint32_t getWaterfallGain(void) {
    return g_waterfallGainSquared;
}

/**
 * Get current waterfall gain (linear, user-facing)
 * @return Linear gain × GAIN_NORMALIZATION
 */
uint32_t getWaterfallGainLinear(void) {
    return g_waterfallGainLinear;
}

/**
 * Get WiFi SSID
 * @return Pointer to SSID string (null-terminated)
 */
const char* getWifiSSID(void) {
    return g_wifi_ssid;
}

/**
 * Get WiFi password
 * @return Pointer to password string (null-terminated)
 */
const char* getWifiPassword(void) {
    return g_wifi_password;
}

/**
 * Initialize all shared infrastructure
 * 
 * Phase 0 of FreeRTOS initialization.
 * Must complete successfully before module init phase begins.
 */
int infrastructure_init(void) {
    printf("\n=== Phase 0: Shared Infrastructure ===\n");
    
    /* Create audio queues */
    printf("  Creating audio queues...\n");
    
    g_audioQueueUDP = xQueueCreate(4, sizeof(AudioBufferMessage_t));
    if (g_audioQueueUDP == NULL) {
        printf("    ERROR: Failed to create UDP audio queue\n");
        return INIT_ERR_QUEUE;
    }
    printf("    ✓ UDP audio queue (depth=4, msg_size=%u bytes)\n",
           (unsigned)sizeof(AudioBufferMessage_t));
    
    g_audioQueueWaterfall = xQueueCreate(4, sizeof(AudioBufferMessage_t));
    if (g_audioQueueWaterfall == NULL) {
        printf("    ERROR: Failed to create Waterfall audio queue\n");
        return INIT_ERR_QUEUE;
    }
    printf("    ✓ Waterfall audio queue (depth=4, msg_size=%u bytes)\n",
           (unsigned)sizeof(AudioBufferMessage_t));
    
    /* Create audio synchronization semaphores */
    printf("  Creating audio semaphores...\n");
    
    g_audioReadySemaphore = xSemaphoreCreateBinary();
    if (g_audioReadySemaphore == NULL) {
        printf("    ERROR: Failed to create audio ready semaphore\n");
        return INIT_ERR_QUEUE;
    }
    printf("    ✓ Audio ready semaphore (binary)\n");
    
    g_audioSemaphoreUDP = xSemaphoreCreateBinary();
    if (g_audioSemaphoreUDP == NULL) {
        printf("    ERROR: Failed to create UDP audio semaphore\n");
        return INIT_ERR_QUEUE;
    }
    printf("    ✓ UDP audio semaphore (binary)\n");
    
    /* Initialize buffers and status */
    printf("  Initializing audio buffers...\n");
    memset(&g_audioBuffers, 0, sizeof(g_audioBuffers));
    g_audioReady = 0;
    printf("    ✓ Audio buffers cleared (2 × %d samples)\n", AUDIO_BUFFER_SIZE);
    
    /* Load persistent configuration from flash */
    printf("  Loading persistent configuration from flash...\n");
    
    /* Load waterfall gain */
    extern int waterfall_config_load(uint32_t *waterfall_gain);
    uint32_t saved_gain = 0;
    if (waterfall_config_load(&saved_gain) && saved_gain > 0) {
        printf("    ✓ Waterfall gain from flash: %lu\n", saved_gain);
        setWaterfallGain(saved_gain);
    } else {
        printf("    ✓ Waterfall gain: default (75)\n");
        setWaterfallGain(75);
    }
    
    /* Load WiFi credentials */
    extern int network_credentials_load(char *ssid, char *password);
    if (network_credentials_load(g_wifi_ssid, g_wifi_password)) {
        printf("    ✓ WiFi credentials loaded (SSID: %s)\n", g_wifi_ssid);
    } else {
        printf("    ✓ No saved WiFi credentials\n");
    }
    
    printf("✓ Infrastructure ready\n\n");
    return INIT_SUCCESS;
}
