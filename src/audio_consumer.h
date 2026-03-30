#ifndef AUDIO_CONSUMER_H
#define AUDIO_CONSUMER_H

#include "microphone.h"

/**
 * Audio consumer task - waits for and processes audio buffers
 * This is an example task that demonstrates how to use the microphone system
 * 
 * @param parameters - unused
 */
void audio_consumer_task(void *parameters);

/**
 * Initialize the audio consumer system
 * Creates the consumer task
 */
void audio_consumer_init(void);

#endif // AUDIO_CONSUMER_H
