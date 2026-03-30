#include "audio_dsp_example.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <math.h>

/**
 * Audio DSP Processor - Noise Gate Example
 * 
 * This example demonstrates a real-world audio processing scenario:
 * 1. Consume audio buffers from microphone
 * 2. Apply a noise gate (gate opens only when signal exceeds threshold)
 * 3. Track RMS level and gate state
 * 4. Output statistics for monitoring
 */

typedef struct {
    int32_t rms;            /* Current RMS level */
    uint8_t gate_open;      /* Gate state: 1=open (sound passing), 0=closed (muted) */
    uint32_t samples_in;    /* Total samples processed */
    uint32_t samples_gated; /* Samples that passed the gate */
} DSPState_t;

static DSPState_t dsp_state = {0};
static NoiseGateConfig_t *g_gate_config = NULL;
static int32_t gate_counter = 0;  /* Counter for attack/release timing */

/**
 * Calculate RMS (Root Mean Square) of audio samples
 * RMS is commonly used to measure audio level/loudness
 * 
 * @param p_samples - pointer to int16_t audio samples
 * @param num_samples - number of samples to process
 * @return RMS value (0 to 32767 for full-scale int16)
 */
static int32_t calculate_rms(const int16_t *p_samples, uint16_t num_samples)
{
    if (!p_samples || num_samples == 0) {
        return 0;
    }
    
    int64_t sum_sq = 0;
    
    /* Calculate sum of squares */
    for (uint16_t i = 0; i < num_samples; i++) {
        int32_t sample = p_samples[i];
        sum_sq += (sample * sample);
    }
    
    /* RMS = sqrt(sum_sq / N) */
    int32_t rms = (int32_t)sqrt((double)sum_sq / (double)num_samples);
    
    return rms;
}

/**
 * Apply noise gate to audio samples
 * Gate opens when RMS > threshold, closes after release time expires
 * 
 * @param p_samples - pointer to int16_t audio samples (modified in-place)
 * @param num_samples - number of samples to process
 * @param rms - current RMS level
 * @return number of samples that passed the gate (non-zero)
 */
static uint16_t apply_noise_gate(int16_t *p_samples, uint16_t num_samples, int32_t rms)
{
    if (!g_gate_config) {
        return num_samples;  /* Gate disabled */
    }
    
    uint16_t samples_passed = 0;
    
    /* Simple gate logic with attack/release */
    if (rms > g_gate_config->threshold) {
        /* Signal above threshold - open gate */
        dsp_state.gate_open = 1;
        gate_counter = 0;
    } else if (gate_counter > 0) {
        /* Release phase - gradually close gate */
        gate_counter--;
        dsp_state.gate_open = 1;
    } else {
        /* Gate is closed */
        dsp_state.gate_open = 0;
    }
    
    /* Apply gate: zero out samples if gate is closed */
    if (dsp_state.gate_open) {
        /* Count all samples (gate is open) */
        samples_passed = num_samples;
    } else {
        /* Mute all samples (gate is closed) */
        for (uint16_t i = 0; i < num_samples; i++) {
            p_samples[i] = 0;
        }
        samples_passed = 0;
    }
    
    return samples_passed;
}

/**
 * Audio DSP Processor Task
 * 
 * This task:
 * 1. Waits for microphone buffers to become available
 * 2. Calculates RMS level of the audio
 * 3. Applies noise gate processing
 * 4. Prints statistics
 * 
 * In a real application, this could:
 * - Stream processed audio to speaker or transmission
 * - Feed into ML models (speech recognition, etc.)
 * - Perform adaptive filtering, AGC, etc.
 */
void audio_dsp_task(void *parameters)
{
    (void)parameters;
    
    printf("Audio DSP Processor Task Started\n");
    
    if (!g_gate_config) {
        printf("ERROR: DSP config not initialized\n");
        vTaskDelete(NULL);
        return;
    }
    
    printf("Noise Gate: threshold=%d, attack=%dms, release=%dms\n",
           g_gate_config->threshold, 
           g_gate_config->attack_ms,
           g_gate_config->release_ms);
    
    uint32_t buffer_count = 0;
    
    while (1) {
        /* Wait for microphone to provide audio buffer */
        if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(5000)) != pdTRUE) {
            printf("DSP: Timeout waiting for audio\n");
            continue;
        }
        
        /* Get the ready buffer */
        uint8_t buffer_id = get_audio_ready();
        if (buffer_id == 0) {
            printf("ERROR: No buffer marked ready\n");
            continue;
        }
        
        /* Get pointer to the audio buffer */
        int16_t *p_audio = (buffer_id == 1) ? g_audioBuffers.buffer1 
                                             : g_audioBuffers.buffer2;
        
        /**
         * Processing Pipeline
         */
        
        /* Step 1: Calculate RMS level */
        int32_t rms = calculate_rms(p_audio, AUDIO_BUFFER_SIZE);
        dsp_state.rms = rms;
        
        /* Step 2: Apply noise gate */
        uint16_t samples_passed = apply_noise_gate(p_audio, AUDIO_BUFFER_SIZE, rms);
        
        /* Step 3: Update statistics */
        dsp_state.samples_in += AUDIO_BUFFER_SIZE;
        dsp_state.samples_gated += samples_passed;
        buffer_count++;
        
        /**
         * Step 4: Output statistics
         * In production, you might log less frequently (every 10 or 100 buffers)
         */
        if ((buffer_count % 10) == 0) {
            char gate_state = dsp_state.gate_open ? 'O' : 'X';
            float gate_ratio = 100.0f * (float)dsp_state.samples_gated / 
                              (float)(dsp_state.samples_in + 1);
            
            printf("[DSP-%lu] RMS=%5d Gate=%c (%5.1f%%) threshold=%d\n",
                   buffer_count,
                   (int)rms,
                   gate_state,
                   gate_ratio,
                   g_gate_config->threshold);
        }
        
        /**
         * Step 5: Signal that we're done with the buffer
         * This allows the microphone task to overwrite it with new data
         */
        clear_audio_ready();
        
        /**
         * Optional: Additional processing steps could go here
         * - Stream processed audio to DAC/speaker
         * - Send to wireless peripheral
         * - Store to SD card
         * - Feed to ML model
         * - Real-time visualization
         */
    }
}

/**
 * Initialize the audio DSP processor
 * @param config - pointer to NoiseGateConfig_t configuration
 */
void audio_dsp_init(const NoiseGateConfig_t *config)
{
    if (!config) {
        printf("ERROR: Invalid DSP config pointer\n");
        return;
    }
    
    /* Store config */
    g_gate_config = (NoiseGateConfig_t *)config;
    
    /* Calculate gate counter from release time
     * Assuming ~62.5kHz PCM rate with 1024 samples per buffer (~16ms per buffer)
     * release_ms / 16ms = number of buffers before gate closes
     */
    gate_counter = 0;
    
    /* Create DSP processor task */
    BaseType_t result = xTaskCreate(
        audio_dsp_task,                        /* Task function */
        "AudioDSP",                            /* Task name */
        configMINIMAL_STACK_SIZE + 2048,       /* Stack size (DSP uses more memory) */
        NULL,                                  /* Parameters */
        2,                                     /* Priority */
        NULL                                   /* Task handle */
    );
    
    if (result != pdPASS) {
        printf("ERROR: Failed to create audio DSP task\n");
    } else {
        printf("Audio DSP task created successfully\n");
    }
}
