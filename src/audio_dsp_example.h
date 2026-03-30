#ifndef AUDIO_DSP_EXAMPLE_H
#define AUDIO_DSP_EXAMPLE_H

#include "microphone.h"

/**
 * Example: Noise Gate Processor
 * 
 * Demonstrates a real-world audio processing task that:
 * - Consumes audio from the ping-pong buffers
 * - Applies a simple noise gate (suppress audio below threshold)
 * - Computes RMS level for monitoring
 * - Could stream to output or process further
 */

/**
 * Configuration for noise gate processor
 */
typedef struct {
    int16_t threshold;      /* Gate threshold in PCM units (-32768 to 32767) */
    uint16_t attack_ms;     /* Time to open gate (milliseconds) */
    uint16_t release_ms;    /* Time to close gate (milliseconds) */
} NoiseGateConfig_t;

/**
 * Initialize and start the noise gate audio processor
 * @param config - pointer to configuration structure
 */
void audio_dsp_init(const NoiseGateConfig_t *config);

/**
 * DSP processor task function
 * This is called by FreeRTOS when the task is created
 * @param parameters - pointer to NoiseGateConfig_t
 */
void audio_dsp_task(void *parameters);

#endif // AUDIO_DSP_EXAMPLE_H
