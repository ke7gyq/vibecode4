/**
 * Audio Hub - Broadcast Audio Buffer to Multiple Consumers
 * 
 * Implements broadcast semaphore + shared buffer pattern for distributing
 * audio data to multiple independent consumers (UDP, Waterfall, etc.)
 * 
 * All consumers wake simultaneously and read from a single shared buffer.
 * Natural backpressure: if any consumer is slow, producer waits.
 */

#ifndef AUDIO_HUB_H
#define AUDIO_HUB_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "microphone.h"

/**
 * Initialize the audio hub
 * Must be called once during system startup before any producer/consumer tasks
 * 
 * @return 0 on success, -1 on failure
 */
int audio_hub_init(void);

/**
 * Broadcast audio buffer to all registered consumers
 * Producer (microphone task) calls this to send audio to all consumers
 * Blocks if any consumer hasn't consumed the previous buffer yet
 * 
 * @param msg Pointer to audio buffer message to broadcast
 * @return 0 on success, -1 on error
 */
int audio_hub_broadcast(const AudioBufferMessage_t *msg);

/**
 * Receive next audio buffer (for consumer tasks)
 * Blocks until producer broadcasts new audio data
 * 
 * @param msg Pointer to buffer where audio message will be copied
 * @param timeout_ms Timeout in milliseconds (or portMAX_DELAY for infinite)
 * @return 0 on success, -1 on timeout/error
 */
int audio_hub_receive(AudioBufferMessage_t *msg, uint32_t timeout_ms);

/**
 * Get the current shared audio buffer without waiting
 * Useful for consumers that want to read the latest without blocking
 * 
 * @return Pointer to current audio buffer, or NULL if not initialized
 */
const AudioBufferMessage_t* audio_hub_get_latest(void);

/**
 * Cleanup audio hub resources
 * Call before shutdown
 */
void audio_hub_cleanup(void);

#endif /* AUDIO_HUB_H */
