#ifndef MICROPHONE_H
#define MICROPHONE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "hardware/pio.h"

/**
 * ===== Hardware Configuration =====
 */
#define MIC_CLK_PIN     6      // GPIO6 - PDM clock output
#define MIC_DATA_PIN    7      // GPIO7 - PDM data input
#define MIC_PIO_NUM     0      // Use PIO0 (shares with st7789 LCD)
#define MIC_SM          1      // State machine 1 (st7789 uses SM 0)

/**
 * ===== PDM/PCM Configuration =====
 * These parameters define the audio chain:
 * - PDM_SAMPLE_RATE_HZ: PDM clock frequency (must match PIO clock divider)
 * - DECIMATION_RATIO: PDM to PCM decimation (typically 64:1)
 * - PCM_SAMPLE_RATE_HZ: Output PCM sample rate (PDM / DECIMATION)
 * 
 * NOTE: PIO instruction timing limits clock speed:
 * With minimal 4-instruction loop: PDM_max = SysClock / 4 / 2 dividers
 * For RP2350 (150 MHz): realistic PDM ~1.5 MHz with PIO
 * To reach 3.072 MHz: need external clock or PWM instead of PIO
 */
#define PDM_SAMPLE_RATE_HZ    3072000    // 3.072 MHz (original design target)
#define DECIMATION_RATIO      64         // 64:1 decimation (restored to maintain 48 kHz output rate)
#define PCM_SAMPLE_RATE_HZ    48000      // Output rate: 3.072M / 64 = 48 kHz

/**
 * ===== DMA Configuration =====
 * MIC_NUM_DMA_CHANNELS: Number of DMA channels for ping-pong operation (2)
 * DMA_BUFFER_WORDS: Size of each DMA buffer in 32-bit words
 *   - 96 words = 384 bytes = exactly 1 filter call = 48 PCM samples
 */
#define DMA_BUFFER_WORDS      96        // 96 * 4 bytes = 384 bytes per DMA transfer
#define MIC_NUM_DMA_CHANNELS  2         // Ping-pong pair

/**
 * ===== Filter & Output Buffer Configuration =====
 * FILTER_CALLS_PER_OUTPUT_BUFFER: How many filter calls (each 48 samples) 
 *   before switching output buffers
 *   - 11 calls × 48 samples = 528 samples = 11 ms of audio
 * AUDIO_BUFFER_SIZE: Total PCM samples in each ping-pong output buffer
 */
#define FILTER_CALLS_PER_OUTPUT_BUFFER  11    // 11 × 48 = 528 samples = 11ms
#define AUDIO_BUFFER_SIZE               (FILTER_CALLS_PER_OUTPUT_BUFFER * (PCM_SAMPLE_RATE_HZ / 1000))
#define AUDIO_BUFFER_SAMPLES            AUDIO_BUFFER_SIZE

/**
 * ===== Debug Configuration =====
 * MIC_TRACE: Minimum g_micDebug level to enable trace output
 */
#define MIC_TRACE 5  /* Raised from 2: buffer full only at verbose mode */

/* ==================== PDM Microphone Library API ==================== */

/**
 * Configuration structure for PDM microphone library
 */
struct pdm_microphone_config {
    uint gpio_data;           // GPIO pin for PDM data input
    uint gpio_clk;            // GPIO pin for PDM clock output
    PIO pio;                  // PIO instance (pio0 or pio1)
    uint pio_sm;              // PIO state machine (0-3)
    uint sample_rate;         // Output PCM sample rate in Hz
    uint sample_buffer_size;  // Size of samples buffer (samples at output rate)
};

/**
 * Callback type: called when samples buffer is ready
 */
typedef void (*pdm_samples_ready_handler_t)(void);

/**
 * Initialize PDM microphone subsystem
 * Allocates buffers, configures DMA, sets up filter
 * 
 * @param config Pointer to PDM configuration structure
 * @return 0 on success, -1 on error
 */
int pdm_microphone_init(const struct pdm_microphone_config* config);

/**
 * Clean up PDM microphone resources
 * Frees allocated buffers and releases DMA channel
 */
void pdm_microphone_deinit(void);

/**
 * Start PDM microphone capture
 * Enables DMA interrupts and starts PIO state machine
 * 
 * @return 0 on success, -1 on error
 */
int pdm_microphone_start(void);

/**
 * Stop PDM microphone capture
 * Disables DMA and PIO
 */
void pdm_microphone_stop(void);

/**
 * Set callback function invoked when samples buffer is ready
 * Callback is called from DMA interrupt context
 * 
 * @param handler Function pointer or NULL to disable
 */
void pdm_microphone_set_samples_ready_handler(pdm_samples_ready_handler_t handler);

/**
 * Set maximum volume level for filter
 * 
 * @param max_volume Maximum volume value (0-255)
 */
void pdm_microphone_set_filter_max_volume(uint8_t max_volume);

/**
 * Set filter gain for next read operations
 * 
 * @param gain Gain multiplier (typically 1-16)
 */
void pdm_microphone_set_filter_gain(uint8_t gain);

/**
 * Set filter volume for next read operations
 * Controls output level
 * 
 * @param volume Volume level
 */
void pdm_microphone_set_filter_volume(uint16_t volume);

/**
 * Read decoded PCM samples from PDM buffer
 * Blocks until buffer is available, applies PDM-to-PCM conversion
 * 
 * @param buffer Output buffer for PCM samples (int16_t)
 * @param samples Maximum number of samples to read
 * @return Number of samples read, or 0 if no data available
 */
int pdm_microphone_read(int16_t* buffer, size_t samples);

/* ==================== Application Interface ==================== */

/* Audio buffer structures */
typedef struct {
    int16_t buffer1[AUDIO_BUFFER_SIZE];
    int16_t buffer2[AUDIO_BUFFER_SIZE];
} AudioBuffers_t;

/**
 * Global audio status variable
 * 0 = no audio ready
 * 1 = audio ready in buffer 1
 * 2 = audio ready in buffer 2
 */
extern volatile uint8_t g_audioReady;

/**
 * Global microphone debug level
 * 0 = OFF (no status output)
 * 1 = Reserved
 * 2+ = ON (show buffer processing status)
 */
extern uint8_t g_micDebug;

/**
 * Global audio buffers - ping-pong pair
 */
extern AudioBuffers_t g_audioBuffers;

/**
 * Binary semaphore for audio data ready
 * Posted by microphone task when buffer is filled
 * Waited by consumer task
 */
extern SemaphoreHandle_t g_audioReadySemaphore;

/** * Binary semaphore for UDP task event-driven signaling
 * Posted by microphone when new audio buffer ready for UDP
 */
extern SemaphoreHandle_t g_audioSemaphoreUDP;

/**
 * Current buffer data for UDP task (updated by microphone, read by UDP)
 */
extern int16_t *g_current_udp_buffer;
extern uint32_t g_current_udp_sample_count;
extern uint32_t g_current_udp_sequence;

/**
 * UDP task ready flag - set when UDP task is waiting on semaphore for next frame
 * Microphone checks this before giving semaphore (prevents race condition)
 */
/**
 * Message queues for audio buffer distribution
 * Each consumer (UDP, Waterfall) gets independent queue for independent flow control
 * FreeRTOS queue internals handle all memory barriers (no explicit fences needed)
 */
extern QueueHandle_t g_audioQueueUDP;
extern QueueHandle_t g_audioQueueWaterfall;

typedef struct {
    uint8_t buffer_id;              // Buffer ID: 1 or 2
    uint32_t sequence;              // Incrementing sequence number (detects missed buffers)
    int16_t *buffer_ptr;            // Pointer to actual audio buffer
    uint32_t sample_count;          // Number of samples in this buffer
    uint32_t timestamp_ms;          // Timestamp when buffer completed (ms)
} AudioBufferMessage_t;

/**
 * Message queues for audio buffer distribution
 * Each consumer (UDP, Waterfall) gets independent queue
 * This prevents race conditions and lost notifications
 */

/**
 * Initialize the PDM microphone system
 * Sets up PIO, DMA, and FreeRTOS task
 * 
 * @return true on success, false on failure
 */
bool microphone_init(void);

/**
 * Get the current audio ready status
 * @return 0 (no buffer), 1 (buffer1 ready), or 2 (buffer2 ready)
 */
uint8_t get_audio_ready(void);

/**
 * Clear the audio ready status after consuming buffer
 */
void clear_audio_ready(void);

/**
 * Microphone task - runs PDM to PCM conversion and DMA transfer
 * This is called by FreeRTOS task creation system
 * @param parameters - unused
 */
void microphone_task(void *parameters);

/**
 * Process PDM data from DMA buffer and convert to PCM
 * 
 * This function takes PDM data (1-bit samples at high rate) and
 * converts it to PCM (16-bit samples at lower rate) using a
 * simple decimation-by-64 CIC filter.
 * 
 * @param p_pdm_data - pointer to PDM data words (uint32_t)
 * @param num_pdm_words - number of 32-bit PDM words to process
 * @param p_pcm_buffer - pointer to output PCM buffer (int16_t)
 * @param p_pcm_index - pointer to current index in PCM buffer (updated by function)
 * @return number of PCM samples generated
 */
uint16_t process_pdm_to_pcm(const uint32_t *p_pdm_data, uint16_t num_pdm_words,
                            int16_t *p_pcm_buffer, uint16_t *p_pcm_index);

/* ==================== GPIO Toggle Test ==================== */

/**
 * GPIO Toggle Test - runs before RTOS starts
 * Toggles GPIO6 at high frequency using PIO state machine
 * Useful for testing PIO functionality and verifying timing
 * 
 * This test runs for 2 seconds, then stops and cleans up
 */
void gpio_toggle_test(void);

/**
 * PDM Clock Test - generates 3.072 MHz PDM clock for microphone testing
 * Generates clock signal on GPIO6 using PIO state machine
 * Useful for verifying PDM timing before connecting to microphone
 * 
 * This test runs for 5 seconds, then stops and cleans up
 * Current frequency: ~3.125 MHz (1.7% higher than 3.072 MHz target)
 * For exact 3.072 MHz, modify clk_div to 1.0031
 */
void initialize_pdm_clock(void);

#endif // MICROPHONE_H
