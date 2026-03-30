#include "audio_consumer.h"
#include "microphone.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/**
 * Audio consumer task
 * 
 * This task demonstrates the consumer side of the ping-pong buffer pattern:
 * 1. Waits on g_audioReadySemaphore (posted by microphone task)
 * 2. Checks g_audioReady to see which buffer has data
 * 3. Processes the audio data from the appropriate buffer
 * 4. Clears g_audioReady to signal ready for next buffer
 * 
 * In a real application, this would:
 * - Pass audio to DSP/ML pipeline (noise suppression, etc.)
 * - Stream to speaker or wireless transmission
 * - Perform real-time analysis
 */
void audio_consumer_task(void *parameters)
{
    (void)parameters;
    
    printf("Audio Consumer Task Started\n");
    
    uint32_t sample_count = 0;
    const TickType_t wait_timeout = pdMS_TO_TICKS(10000);  /* 10 second timeout */
    
    while (1) {
        /**
         * Wait for microphone task to signal buffer ready
         * This blocks until g_audioReadySemaphore is posted
         */

        while ( g_audioReadySemaphore == NULL ) {
            printf("Waiting for audio semaphore to be created...\n");
            vTaskDelay(pdMS_TO_TICKS(100));
        }


        if (xSemaphoreTake(g_audioReadySemaphore, wait_timeout) != pdTRUE) {
            printf("TIMEOUT: No audio buffer received\n");
            continue;
        }
        
        /* Check which buffer has data */
        uint8_t buffer_ready = get_audio_ready();
        
        if (buffer_ready == 0) {
            printf("ERROR: Semaphore received but no buffer marked ready\n");
            continue;
        }
        
        /* Get pointer to the ready buffer */
        int16_t *p_audio_buffer = NULL;
        if (buffer_ready == 1) {
            p_audio_buffer = g_audioBuffers.buffer1;
            if (g_micDebug >= 2) printf("Processing buffer 1: ");
        } else if (buffer_ready == 2) {
            p_audio_buffer = g_audioBuffers.buffer2;
            if (g_micDebug >= 2) printf("Processing buffer 2: ");
        } else {
            printf("ERROR: Invalid g_audioReady value: %d\n", buffer_ready);
            clear_audio_ready();
            continue;
        }
        
        /**
         * Example audio processing:
         * 1. Calculate RMS level (simple energy estimate)
         * 2. Check for silence
         * 3. In real app: process through filters, ML models, etc.
         */
        
        int64_t sum_sq = 0;
        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
            int32_t sample = p_audio_buffer[i];
            sum_sq += (sample * sample);
        }
        
        /* RMS = sqrt(sum_sq / N) */
        uint32_t rms = (uint32_t)__builtin_sqrtl(sum_sq / AUDIO_BUFFER_SIZE);
        
        if (g_micDebug >= 2) printf("RMS=%u, samples_processed=%lu\n", rms, sample_count++);
        
        /**
         * IMPORTANT: Clear the audio ready flag when done processing
         * This signals the microphone task that the buffer has been consumed
         * and the buffer can be reused for the next capture
         */
        clear_audio_ready();
        
        /**
         * Optional: Add processing delay if needed for synchronization
         * In real applications, this depends on DSP pipeline latency
         */
        // vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * Initialize audio consumer
 * Creates the consumer task
 */
void audio_consumer_init(void)
{
    /* Create consumer task with priority lower than microphone task */
    BaseType_t result = xTaskCreate(
        audio_consumer_task,              /* Task function */
        "AudioConsumer",                  /* Task name */
        configMINIMAL_STACK_SIZE + 1024,  /* Stack size */
        NULL,                             /* Parameters */
        3,                                /* Priority (0-24, higher = more important) */
        NULL                              /* Task handle */
    );
    
    if (result != pdPASS) {
        printf("ERROR: Failed to create audio consumer task\n");
    } else {
        printf("Audio consumer task created successfully\n");
    }
}
