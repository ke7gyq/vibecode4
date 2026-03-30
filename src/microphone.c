#include "microphone.h"
#include "OpenPDMFilter.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "pdm_clock.pio.h"
#include "gpio_toggle_test.pio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * Compile-time switch between real microphone and test data
 * 
 * Set to 1 for real microphone:
 *   - Reads PDM data from DMA buffer (g_dma_buffer)
 *   - Processes actual ATSAMD21 microphone data
 *   - Converts PDM to PCM using process_pdm_to_pcm()
 * 
 * Set to 0 for test mode:
 *   - Generates synthetic 1 kHz sine wave tone
 *   - Useful for testing software without physical microphone
 *   - Fills buffers at expected rate (48 kHz)
 *
 * Change this value, rebuild, and deploy to switch modes:
 *   #define USE_REAL_MICROPHONE 0  // 0 = test, 1 = real
 */
#define USE_REAL_MICROPHONE 1

/* ==================== PDM Library State (from microphone-library-for-pico) ==================== */
#define PDM_DECIMATION       64
#define PDM_RAW_BUFFER_COUNT 2

/* PDM microphone library state */
static struct {
    struct {
        uint gpio_data;
        uint gpio_clk;
        PIO pio;
        uint pio_sm;
        uint sample_rate;
        uint sample_buffer_size;
    } config;
    int dma_channel;
    uint8_t* raw_buffer[PDM_RAW_BUFFER_COUNT];  /* Raw PDM data buffers */
    volatile int raw_buffer_write_index;        /* Index being written by DMA */
    volatile int raw_buffer_read_index;         /* Index being read by consumer */
    uint raw_buffer_size;
    uint dma_irq;
    TPDMFilter_InitStruct filter;               /* OpenPDM filter configuration */
    uint16_t filter_volume;
    pdm_samples_ready_handler_t samples_ready_handler;  /* Callback when buffer ready */
} g_pdm_mic = {0};

/* Forward declaration for DMA handler */
static void pdm_dma_handler(void);

/* ==================== Global Application State ==================== */
/* Global state */
volatile uint8_t g_audioReady = 0;
AudioBuffers_t g_audioBuffers;
SemaphoreHandle_t g_audioReadySemaphore = NULL;
uint8_t g_micDebug = 0;  /* Microphone debug level: 0=OFF, 2+=ON */

/* Microphone state */
static bool g_microphone_initialized = false;
static PIO g_pio = NULL;
static uint g_sm = 0;
static uint g_dma_ch = 0;

/* DMA buffers - ping-pong pair for alternating fills */
static uint32_t g_dma_buffer_a[DMA_BUFFER_WORDS];  /* 96 words = 384 bytes */
static uint32_t g_dma_buffer_b[DMA_BUFFER_WORDS];  /* 96 words = 384 bytes */

/* PDM filter configuration using ST OpenPDM2PCM library */
static TPDMFilter_InitStruct g_pdm_filter_config = {0};

/**
 * Computed filter parameters (determined at compile-time)
 */
#define SAMPLES_PER_FILTER_CALL  (PCM_SAMPLE_RATE_HZ / 1000)  /* 48 samples @ 48kHz */
#define PDM_BYTES_PER_FILTER_CALL (DMA_BUFFER_WORDS * 4)      /* 384 bytes */

/*
 * Configuration for OpenPDM2PCM filter
 * - Decimation: 64:1 (PDM 3.072 MHz → PCM 48 kHz)
 * - Channels: 1 (mono)
 * - Output rate (Fs): 48000 Hz
 * - Low-pass filter: 10 kHz (remove quantization noise)
 * - High-pass filter: 50 Hz (remove DC offset)
 * - Volume: 16 (standard gain)
 */
void pdm_filter_config_init(void)
{
    g_pdm_filter_config.LP_HZ = 10000.0f;      /* Low-pass cutoff: 10 kHz */
    g_pdm_filter_config.HP_HZ = 50.0f;         /* High-pass cutoff: 50 Hz (removes DC) */
    g_pdm_filter_config.Fs = PCM_SAMPLE_RATE_HZ; /* Output sample rate from compile-time config */
    g_pdm_filter_config.In_MicChannels = 1;    /* Mono input */
    g_pdm_filter_config.Out_MicChannels = 1;   /* Mono output */
    g_pdm_filter_config.Decimation = DECIMATION_RATIO; /* Decimation from compile-time config */
    g_pdm_filter_config.MaxVolume = 16;        /* Standard gain */
#ifdef PICO_BUILD
    g_pdm_filter_config.Gain = 1;              /* Additional gain multiplier */
#endif
    
    /* Initialize the filter (builds sinc³ coefficients and LUT) */
    Open_PDM_Filter_Init(&g_pdm_filter_config);
    
    printf("OpenPDM2PCM Filter initialized:\n");
    printf("  PDM Clock: %lu Hz\n", PDM_SAMPLE_RATE_HZ);
    printf("  Decimation: %d:1\n", g_pdm_filter_config.Decimation);
    printf("  Output rate: %d Hz\n", g_pdm_filter_config.Fs);
    printf("  LP: %.1f Hz, HP: %.1f Hz\n", g_pdm_filter_config.LP_HZ, g_pdm_filter_config.HP_HZ);
}

/* ==================== PDM Microphone Library API Functions ==================== */
/**
 * DMA completion interrupt handler
 * Swaps buffer indices and calls samples_ready callback
 */
static void pdm_dma_handler() {
    // Clear IRQ
    if (g_pdm_mic.dma_irq == DMA_IRQ_0) {
        dma_hw->ints0 = (1u << g_pdm_mic.dma_channel);
    } else if (g_pdm_mic.dma_irq == DMA_IRQ_1) {
        dma_hw->ints1 = (1u << g_pdm_mic.dma_channel);
    }

    // Get the current buffer index
    g_pdm_mic.raw_buffer_read_index = g_pdm_mic.raw_buffer_write_index;

    // Get the next capture index
    g_pdm_mic.raw_buffer_write_index = (g_pdm_mic.raw_buffer_write_index + 1) % PDM_RAW_BUFFER_COUNT;

    // Give the channel a new buffer to write to and re-trigger it
    dma_channel_transfer_to_buffer_now(
        g_pdm_mic.dma_channel,
        g_pdm_mic.raw_buffer[g_pdm_mic.raw_buffer_write_index],
        g_pdm_mic.raw_buffer_size
    );

    if (g_pdm_mic.samples_ready_handler) {
        g_pdm_mic.samples_ready_handler();
    }
}

/**
 * Start PDM microphone capture
 * Enables DMA interrupts and PIO state machine
 */
int pdm_microphone_start() {
    g_pdm_mic.dma_irq = DMA_IRQ_0;

    irq_set_enabled(g_pdm_mic.dma_irq, true);
    irq_set_exclusive_handler(g_pdm_mic.dma_irq, pdm_dma_handler);

    if (g_pdm_mic.dma_irq == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(g_pdm_mic.dma_channel, true);
    } else if (g_pdm_mic.dma_irq == DMA_IRQ_1) {
        dma_channel_set_irq1_enabled(g_pdm_mic.dma_channel, true);
    } else {
        return -1;
    }

    Open_PDM_Filter_Init(&g_pdm_mic.filter);

    pio_sm_set_enabled(
        g_pdm_mic.config.pio,
        g_pdm_mic.config.pio_sm,
        true
    );

    g_pdm_mic.raw_buffer_write_index = 0;
    g_pdm_mic.raw_buffer_read_index = 0;

    dma_channel_transfer_to_buffer_now(
        g_pdm_mic.dma_channel,
        g_pdm_mic.raw_buffer[0],
        g_pdm_mic.raw_buffer_size
    );

    printf("PDM microphone started (DMA IRQ%u, PIO SM%u)\n", 
           g_pdm_mic.dma_irq, g_pdm_mic.config.pio_sm);

    return 0;
}

/**
 * Stop PDM microphone capture
 */
void pdm_microphone_stop() {
    pio_sm_set_enabled(
        g_pdm_mic.config.pio,
        g_pdm_mic.config.pio_sm,
        false
    );

    dma_channel_abort(g_pdm_mic.dma_channel);

    if (g_pdm_mic.dma_irq == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(g_pdm_mic.dma_channel, false);
    } else if (g_pdm_mic.dma_irq == DMA_IRQ_1) {
        dma_channel_set_irq1_enabled(g_pdm_mic.dma_channel, false);
    }

    irq_set_enabled(g_pdm_mic.dma_irq, false);
    printf("PDM microphone stopped\n");
}

/**
 * Set callback function called when samples are ready
 */
void pdm_microphone_set_samples_ready_handler(pdm_samples_ready_handler_t handler) {
    g_pdm_mic.samples_ready_handler = handler;
}

/**
 * Set filter maximum volume
 */
void pdm_microphone_set_filter_max_volume(uint8_t max_volume) {
    g_pdm_mic.filter.MaxVolume = max_volume;
}

/**
 * Set filter gain
 */
void pdm_microphone_set_filter_gain(uint8_t gain) {
    g_pdm_mic.filter.Gain = gain;
}

/**
 * Set filter volume for next read operations
 */
void pdm_microphone_set_filter_volume(uint16_t volume) {
    g_pdm_mic.filter_volume = volume;
}

/**
 * Read decoded PCM samples from PDM buffer
 * Applies OpenPDM filter to convert PDM -> PCM
 * Returns: number of samples read, or 0 if no data available
 */
int pdm_microphone_read(int16_t* buffer, size_t samples) {
    int filter_stride = (g_pdm_mic.filter.Fs / 1000);
    samples = (samples / filter_stride) * filter_stride;

    if (samples > g_pdm_mic.config.sample_buffer_size) {
        samples = g_pdm_mic.config.sample_buffer_size;
    }

    if (g_pdm_mic.raw_buffer_write_index == g_pdm_mic.raw_buffer_read_index) {
        return 0;  /* No new data available */
    }

    uint8_t* in = g_pdm_mic.raw_buffer[g_pdm_mic.raw_buffer_read_index];
    int16_t* out = buffer;

    g_pdm_mic.raw_buffer_read_index++;

    for (int i = 0; i < samples; i += filter_stride) {
#if PDM_DECIMATION == 64
        Open_PDM_Filter_64(in, out, g_pdm_mic.filter_volume, &g_pdm_mic.filter);
#elif PDM_DECIMATION == 128
        Open_PDM_Filter_128(in, out, g_pdm_mic.filter_volume, &g_pdm_mic.filter);
#else
        #error "Unsupported PDM_DECIMATION value!"
#endif

        in += filter_stride * (PDM_DECIMATION / 8);
        out += filter_stride;
    }

    return samples;
}

/**
 * Initialize PIO for PDM clock generation and data sampling
 */
static bool pio_init_pdm(void)
{
    /* SM2 is loaded by initialize_pdm_clock() with pdm_clock.pio */
    /* This function used to initialize SM1 (pdm_microphone.pio) but that's now redundant */
    /* SM2 already reads GPIO6 data into its RX FIFO */
    
    g_pio = pio0;
    g_sm = 2;  /* Use SM2 (pdm_clock) which handles both clock and data reading */
    
    printf("PDM data source: State Machine 2 (pdm_clock.pio)\n");
    printf("  SM2 reads GPIO6 data and generates GPIO7 clock\n");
    printf("  DMA will capture from SM2's RX FIFO\n");
    
    return true;
}

/**
 * Initialize DMA for transferring PDM data from PIO
 * Uses a single channel with manual restart after completion
 */
static bool dma_init_pdm(void)
{
    /* Claim one DMA channel */
    g_dma_ch = dma_claim_unused_channel(true);
    if (g_dma_ch < 0) {
        printf("ERROR: No DMA channel available\n");
        return false;
    }
    
    printf("Allocated DMA channel %d\n", g_dma_ch);
    
    /* Configure DMA channel */
    dma_channel_config config = dma_channel_get_default_config(g_dma_ch);
    
    /* Read from PIO RX FIFO, write to buffer */
    channel_config_set_read_increment(&config, false);  /* Don't increment read (always FIFO) */
    channel_config_set_write_increment(&config, true);  /* Increment write address */
    channel_config_set_dreq(&config, pio_get_dreq(g_pio, g_sm, false));  /* Use PIO DREQ */
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);  /* Transfer 32-bit words */
    channel_config_set_bswap(&config, true);  /* Enable byte swap for endianness */
    
    /* Start with buffer A */
    dma_channel_configure(
        g_dma_ch,
        &config,
        g_dma_buffer_a,                 /* Destination: Buffer A */
        &g_pio->rxf[g_sm],              /* Source: PIO RX FIFO */
        DMA_BUFFER_WORDS,               /* Transfer count: 96 words = 384 bytes */
        true                            /* Start immediately */
    );
    
    printf("DMA channel configured: %d words per transfer (%.1f ms)\n", 
           DMA_BUFFER_WORDS,
           (DMA_BUFFER_WORDS * 1000.0f) / (PDM_SAMPLE_RATE_HZ / DECIMATION_RATIO));
    
    return true;
}

/**
 * Microphone task function
 * Polls single DMA channel, alternating between two buffers
 */
void microphone_task(void *parameters)
{
    (void)parameters;
    
    printf("Microphone Task Started\n");
    
    if (!microphone_init()) {
        printf("ERROR: Failed to initialize microphone\n");
        vTaskDelete(NULL);
        return;
    }
    
    printf("Microphone initialized successfully\n");
    
    /* Initialize OpenPDM2PCM filter */
    pdm_filter_config_init();
    printf("PDM Clock: %lu Hz, Decimation: %d:1, PCM Rate: %u Hz\n", 
           PDM_SAMPLE_RATE_HZ, DECIMATION_RATIO, PCM_SAMPLE_RATE_HZ);
    printf("DMA -> Filter: %d words (%d bytes) -> %d samples\n",
           DMA_BUFFER_WORDS, PDM_BYTES_PER_FILTER_CALL, SAMPLES_PER_FILTER_CALL);
    
    /* Output buffer state */
    uint16_t pcm_index = 0;
    uint8_t current_buffer = 1;
    int16_t *p_current_buffer = g_audioBuffers.buffer1;
    
    /* Filter call accumulation */
    uint16_t filter_calls_accumulated = 0;
    
    /* Statistics */
    uint32_t sample_count = 0;
    uint32_t buffer_count = 0;
    
    /* DMA state */
    uint32_t *current_dma_buffer = g_dma_buffer_a;
    uint32_t *other_dma_buffer = g_dma_buffer_b;
    bool dma_last_complete = false;
    
#if USE_REAL_MICROPHONE
    printf("[Mic] Starting REAL microphone mode...\n");
#else
    printf("[Mic] Starting TEST mode (synthetic audio)...\n");
#endif
    
    while (1) {
#if USE_REAL_MICROPHONE
        if (!dma_channel_is_busy(g_dma_ch) && !dma_last_complete) {
            /* DMA finished filling current buffer */
            dma_last_complete = true;
            
            /* IMMEDIATELY restart DMA on the other buffer (producer-consumer overlap) */
            dma_channel_config config = dma_channel_get_default_config(g_dma_ch);
            channel_config_set_read_increment(&config, false);
            channel_config_set_write_increment(&config, true);
            channel_config_set_dreq(&config, pio_get_dreq(g_pio, g_sm, false));
            channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
            
            dma_channel_configure(
                g_dma_ch,
                &config,
                other_dma_buffer,           /* Start filling the other buffer */
                &g_pio->rxf[g_sm],
                DMA_BUFFER_WORDS,
                true
            );
            
            /* Now process the completed buffer with filter (while DMA fills other buffer) */
            uint8_t *pdm_bytes = (uint8_t *)current_dma_buffer;
            
            if ((pcm_index + SAMPLES_PER_FILTER_CALL) <= AUDIO_BUFFER_SIZE) {
                Open_PDM_Filter_64(
                    pdm_bytes,
                    &p_current_buffer[pcm_index],
                    1,
                    &g_pdm_filter_config
                );
                
                pcm_index += SAMPLES_PER_FILTER_CALL;
                sample_count += SAMPLES_PER_FILTER_CALL;
                filter_calls_accumulated++;
                
            } else {
                /* Output buffer full */
                g_audioReady = current_buffer;
                if (g_audioReadySemaphore != NULL) {
                    xSemaphoreGive(g_audioReadySemaphore);
                }
                
                buffer_count++;
                if (g_micDebug >= MIC_TRACE) {
                    printf("[Mic] Buffer %d full (%u samples, %lu total)\n", 
                           current_buffer, pcm_index, buffer_count);
                }
                
                current_buffer = (current_buffer == 1) ? 2 : 1;
                p_current_buffer = (current_buffer == 1) ? 
                                  g_audioBuffers.buffer1 : g_audioBuffers.buffer2;
                pcm_index = 0;
                filter_calls_accumulated = 0;
                
                /* Wait for consumer */
                uint32_t timeout_ms = 100;
                uint32_t start_time = to_ms_since_boot(get_absolute_time());
                while (g_audioReady != 0 && (to_ms_since_boot(get_absolute_time()) - start_time) < timeout_ms) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                
                /* Process the buffer after switching */
                if ((pcm_index + SAMPLES_PER_FILTER_CALL) <= AUDIO_BUFFER_SIZE) {
                    Open_PDM_Filter_64(
                        pdm_bytes,
                        &p_current_buffer[pcm_index],
                        1,
                        &g_pdm_filter_config
                    );
                    pcm_index += SAMPLES_PER_FILTER_CALL;
                    sample_count += SAMPLES_PER_FILTER_CALL;
                    filter_calls_accumulated++;
                }
            }
            
            dma_last_complete = false;
            
            /* Swap buffer pointers for next iteration */
            uint32_t *temp = current_dma_buffer;
            current_dma_buffer = other_dma_buffer;
            other_dma_buffer = temp;
        }
#else
        /* Test mode */
        if (pcm_index < AUDIO_BUFFER_SIZE) {
            int32_t sine = (int32_t)(10000.0f * 
                __builtin_sinf(2.0f * 3.14159f * sample_count / PCM_SAMPLE_RATE_HZ));
            p_current_buffer[pcm_index] = (int16_t)sine;
            pcm_index++;
            sample_count++;
        } else {
            g_audioReady = current_buffer;
            if (g_audioReadySemaphore != NULL) {
                xSemaphoreGive(g_audioReadySemaphore);
            }
            
            buffer_count++;
            if (g_micDebug >= MIC_TRACE) {
                printf("[Mic] Buffer %d full (TEST, %lu total)\n", current_buffer, buffer_count);
            }
            
            current_buffer = (current_buffer == 1) ? 2 : 1;
            p_current_buffer = (current_buffer == 1) ? 
                              g_audioBuffers.buffer1 : g_audioBuffers.buffer2;
            pcm_index = 0;
            
            uint32_t timeout_ms = 100;
            uint32_t start_time = to_ms_since_boot(get_absolute_time());
            while (g_audioReady != 0 && (to_ms_since_boot(get_absolute_time()) - start_time) < timeout_ms) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
#endif
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * Initialize the microphone system
 */
bool microphone_init(void)
{
    if (g_microphone_initialized) {
        return true;
    }
    
    printf("Initializing microphone system...\n");
    
    /* Create binary semaphore if not exists */
    if (g_audioReadySemaphore == NULL) {
        g_audioReadySemaphore = xSemaphoreCreateBinary();
        if (g_audioReadySemaphore == NULL) {
            printf("ERROR: Failed to create audio semaphore\n");
            return false;
        }
    }
    
    /* Initialize buffers */
    memset(&g_audioBuffers, 0, sizeof(g_audioBuffers));
    g_audioReady = 0;
    
    /* Initialize PIO */
    if (!pio_init_pdm()) {
        printf("ERROR: Failed to initialize PIO\n");
        return false;
    }
    printf("PIO initialized\n");
    
    /* Initialize DMA */
    if (!dma_init_pdm()) {
        printf("ERROR: Failed to initialize DMA\n");
        return false;
    }
    printf("DMA initialized\n");
    
    g_microphone_initialized = true;
    
    return true;
}

/**
 * Get current audio ready status
 */
uint8_t get_audio_ready(void)
{
    return g_audioReady;
}

/**
 * Clear audio ready status after consuming
 */
void clear_audio_ready(void)
{
    g_audioReady = 0;
}

/* ==================== GPIO Toggle Test ==================== */

/**
 * GPIO Toggle Test - toggles GPIO6 using PIO state machine
 * Runs for 2 seconds, then stops and cleans up
 */
void gpio_toggle_test(void)
{
    printf("\n[GPIO Test] Starting GPIO6 toggle test...\n");
    
    /* Use PIO0, state machine 3 (reserved for tests) */
    PIO pio = pio0;
    uint sm = 3;
    uint gpio = 6;
    
    /* Load the program */
    uint offset = pio_add_program(pio, &gpio_toggle_test_program);
    printf("[GPIO Test] PIO program loaded at offset %u\n", offset);
    
    /* Initialize and start */
    gpio_toggle_test_program_init(pio, sm, offset, gpio);
    pio_sm_set_enabled(pio, sm, true);
    printf("[GPIO Test] GPIO%u toggling at ~1 MHz (150 MHz / 148 cycles per toggle)\n", gpio);
    printf("[GPIO Test] Use oscilloscope on GPIO%u to verify\n", gpio);
    
    /* Run for 2 seconds */
    sleep_ms(2000);
    
    /* Stop the state machine */
    pio_sm_set_enabled(pio, sm, false);
    printf("[GPIO Test] GPIO%u toggle test complete\n\n", gpio);
}

/**
 * Initialize PDM Clock - generates 3.061 MHz PDM clock permanently on GPIO6
 * Runs on GPIO6 via PIO0 state machine 2 (doesn't conflict with microphone SM1 or display SM0)
 * Clock runs continuously for microphone capture
 */
void initialize_pdm_clock(void)
{
    printf("\n[PDM Clock Init] Starting integrated PDM clock + data reader...\n");
    
    /* Use PIO0, state machine 2 (SM0=display, SM1=microphone reader, SM2=this) */
    PIO pio = pio0;
    uint sm = 2;
    uint data_pin = 6;  /* GPIO6 - PDM data input (reading) */
    uint clk_pin = 7;   /* GPIO7 - PDM clock output (generation) */
    
    /* CRITICAL: Disable SM1 to prevent conflicts with GPIO6/7 */
    pio_sm_set_enabled(pio, 1, false);
    printf("[PDM Clock Init] SM1 disabled (was using GPIO6/7)\n");
    
    /* Load the PDM clock program */
    /* Clock divider: sys_clk / (PDM_freq * cycles_per_period)
     * PDM_freq = PCM_SAMPLE_RATE_HZ * DECIMATION_RATIO = 3.072 MHz
     * cycles_per_period = 4 (2 low + 2 high) */
    float clk_div = clock_get_hz(clk_sys) / (float)(PCM_SAMPLE_RATE_HZ * DECIMATION_RATIO * 4);
    uint offset = pio_add_program(pio, &pdm_clock_program);
    printf("[PDM Clock Init] PIO program loaded at offset %u\n", offset);

    /* Initialize and start with both GPIO pins */
    pdm_clock_program_init(pio, sm, offset, clk_div,data_pin, clk_pin);
    pio_sm_set_enabled(pio, sm, true);
    
    printf("[PDM Clock Init] GPIO6 reads data (input)\n");
    printf("[PDM Clock Init] GPIO7 generates PDM clock at 3.06 MHz (clk_div=%.4f)\n", clk_div);
    printf("[PDM Clock Init] Clock runs continuously - no shutdown needed\n");
    printf("[PDM Clock Init] Running initialization test for 5 seconds...\n");
    
    /* Run initialization test for 5 seconds to allow settling */
    // sleep_ms(5000);
    
    /* Clock continues running - microphone_task will handle GPIO reading */
    printf("[PDM Clock Init] Initialization complete - clock running continuously\n\n");
}




