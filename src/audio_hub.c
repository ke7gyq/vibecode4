/**
 * Audio Hub Implementation - Broadcast Semaphore + Shared Buffer
 */

#include <string.h>
#include "audio_hub.h"
#include "microphone.h"

/* Shared audio buffer protected by mutex */
typedef struct {
    AudioBufferMessage_t buffer;
    SemaphoreHandle_t mutex;           /* Protects reads/writes to buffer */
    SemaphoreHandle_t broadcast_sem;   /* Signals new audio available to all consumers */
    int initialized;
} AudioHub;

static AudioHub g_audio_hub = {0};

/**
 * Initialize the audio hub
 */
int audio_hub_init(void) {
    if (g_audio_hub.initialized) {
        return 0;  /* Already initialized */
    }
    
    /* Create mutex to protect shared buffer */
    g_audio_hub.mutex = xSemaphoreCreateMutex();
    if (g_audio_hub.mutex == NULL) {
        return -1;
    }
    
    /* Create binary semaphore for broadcast (initially blocked for consumers) */
    g_audio_hub.broadcast_sem = xSemaphoreCreateBinary();
    if (g_audio_hub.broadcast_sem == NULL) {
        vSemaphoreDelete(g_audio_hub.mutex);
        return -1;
    }
    
    /* Initialize shared buffer to zero */
    memset(&g_audio_hub.buffer, 0, sizeof(g_audio_hub.buffer));
    
    g_audio_hub.initialized = 1;
    return 0;
}

/**
 * Broadcast audio to all consumers
 * Producer (microphone task) calls this
 */
int audio_hub_broadcast(const AudioBufferMessage_t *msg) {
    if (!g_audio_hub.initialized || msg == NULL) {
        return -1;
    }
    
    /* Take mutex (blocks if any consumer is reading) */
    if (xSemaphoreTake(g_audio_hub.mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    
    /* Copy audio to shared buffer */
    memcpy(&g_audio_hub.buffer, msg, sizeof(AudioBufferMessage_t));
    
    /* Release mutex */
    xSemaphoreGive(g_audio_hub.mutex);
    
    /* Wake all consumers (they all get the same buffer) */
    xSemaphoreGive(g_audio_hub.broadcast_sem);
    
    return 0;
}

/**
 * Consumer receives next audio buffer
 */
int audio_hub_receive(AudioBufferMessage_t *msg, uint32_t timeout_ms) {
    if (!g_audio_hub.initialized || msg == NULL) {
        return -1;
    }
    
    /* Wait for producer to broadcast new audio */
    if (xSemaphoreTake(g_audio_hub.broadcast_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;  /* Timeout or error */
    }
    
    /* Take mutex to read shared buffer safely */
    if (xSemaphoreTake(g_audio_hub.mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    
    /* Copy buffer to consumer */
    memcpy(msg, &g_audio_hub.buffer, sizeof(AudioBufferMessage_t));
    
    /* Release mutex */
    xSemaphoreGive(g_audio_hub.mutex);
    
    return 0;
}

/**
 * Get latest buffer without blocking
 */
const AudioBufferMessage_t* audio_hub_get_latest(void) {
    if (!g_audio_hub.initialized) {
        return NULL;
    }
    
    /* Try to acquire mutex without blocking */
    if (xSemaphoreTake(g_audio_hub.mutex, 0) != pdTRUE) {
        return NULL;  /* Can't acquire safely */
    }
    
    /* Give it back immediately - caller just gets a snapshot reference */
    xSemaphoreGive(g_audio_hub.mutex);
    
    return &g_audio_hub.buffer;
}

/**
 * Cleanup audio hub resources
 */
void audio_hub_cleanup(void) {
    if (!g_audio_hub.initialized) {
        return;
    }
    
    if (g_audio_hub.mutex) {
        vSemaphoreDelete(g_audio_hub.mutex);
    }
    
    if (g_audio_hub.broadcast_sem) {
        vSemaphoreDelete(g_audio_hub.broadcast_sem);
    }
    
    g_audio_hub.initialized = 0;
}
