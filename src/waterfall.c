/**
 * Waterfall Display Implementation
 * 
 * LVGL Canvas-based real-time frequency waterfall display
 * Uses Matlab Parula colormap (256-level) downsampled to 16 discrete colors
 * 
 * Includes a FreeRTOS task for real-time spectrogram processing and display update
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pico/time.h"
#include "waterfall.h"
#include "microphone.h"

/* Debug flag - 0=OFF, 1+=ON */
extern uint8_t g_micDebug;

/* Forward declare the queue from microphone */
extern QueueHandle_t g_audioQueueWaterfall;

/* ============== Parula Colormap (16-level discrete version) ============== */

/**
 * Matlab Parula colormap sampled at 16 discrete levels
 * RGB values that will be converted by LVGL to the correct format
 */
typedef struct {
    uint8_t r, g, b;
} ParulaColor;

static const ParulaColor parula_colormap_16[] = {
    {31,   69,  139},   /* 0:  Deep Blue */
    {0,    143, 150},   /* 1:  Teal */
    {0,    166, 147},   /* 2:  Cyan */
    {47,   167, 111},   /* 3:  Cyan-Green */
    {94,   159, 79},    /* 4:  Green-Cyan */
    {140,  154, 48},    /* 5:  Yellow-Green */
    {176,  158, 29},    /* 6:  Yellow-Green 2 */
    {201,  163, 26},    /* 7:  Yellow */
    {220,  174, 29},    /* 8:  Yellow-Orange */
    {235,  189, 42},    /* 9:  Orange */
    {244,  206, 58},    /* 10: Orange-Yellow */
    {245,  223, 76},    /* 11: Light Orange */
    {242,  240, 97},    /* 12: Yellow */
    {246,  246, 120},   /* 13: Bright Yellow */
    {255,  255, 142},   /* 14: Pale Yellow */
    {255,  255, 164},   /* 15: Very Pale Yellow */
};

/* ============== Log Magnitude to Amplitude Level Lookup Table ============== */
/**
 * Lookup table for converting summed magnitude-squared values to amplitude levels 0-15
 * This table maps the log scale to perceptually uniform amplitude levels
 * The thresholds are empirically tuned for good visualization
 * 
 * Each entry represents the power threshold for that amplitude level
 * magnitude_sq >= power_threshold[level] produces that level
 */
static const uint32_t power_threshold_log_lut[16] = {
    1,           /* Level 0: threshold = 1 (log ≈ 0) */
    2,           /* Level 1: threshold = 2 (log ≈ 0.3) */
    5,           /* Level 2: threshold = 5 (log ≈ 0.7) */
    10,          /* Level 3: threshold = 10 (log ≈ 1.0) */
    20,          /* Level 4: threshold = 20 (log ≈ 1.3) */
    50,          /* Level 5: threshold = 50 (log ≈ 1.7) */
    100,         /* Level 6: threshold = 100 (log ≈ 2.0) */
    200,         /* Level 7: threshold = 200 (log ≈ 2.3) */
    500,         /* Level 8: threshold = 500 (log ≈ 2.7) */
    1000,        /* Level 9: threshold = 1000 (log ≈ 3.0) */
    2000,        /* Level 10: threshold = 2000 (log ≈ 3.3) */
    5000,        /* Level 11: threshold = 5000 (log ≈ 3.7) */
    10000,       /* Level 12: threshold = 10000 (log ≈ 4.0) */
    20000,       /* Level 13: threshold = 20000 (log ≈ 4.3) */
    50000,       /* Level 14: threshold = 50000 (log ≈ 4.7) */
    100000,      /* Level 15: threshold = 100000 (log ≈ 5.0) */
};

/* ============== Static Variables ============== */

/* Current waterfall canvas object */
static lv_obj_t *g_waterfall_canvas = NULL;

/* Canvas pixel buffer (managed by LVGL) */
static uint8_t *g_canvas_buffer = NULL;

/* Current column position (0 to WATERFALL_WIDTH-1) */
static uint16_t g_current_column = 0;

/* FreeRTOS task handle for waterfall processor */
static TaskHandle_t g_waterfall_task_handle = NULL;

/* Pointer to spectrogram context (set by main.c via waterfall_set_spectrogram) */
static spectrogram_t *g_spectrogram_ctx = NULL;

/* External semaphore for audio ready notifications */
extern SemaphoreHandle_t g_audioReadySemaphore;

/* External LVGL mutex */
extern SemaphoreHandle_t g_LvglMutex;

/* ============== Waterfall Configuration & Statistics ============== */

/* Spectrum accumulation: sum 100 frames' FFT magnitudes for averaging */
#define WATERFALL_ACCUMULATION_COUNT 100

/* Accumulator buffer: uint64_t to avoid overflow when summing 100 q15_t values */
static uint64_t g_waterfall_magnitude_accum[WATERFALL_FREQ_BANDS] = {0};

/* Frame counter for accumulation (0-99, resets after averaging) */
static uint32_t g_waterfall_accum_frame_count = 0;

/* Statistics tracking */
static uint32_t g_waterfall_frames_accumulated = 0;  /* Total frames processed */
static uint32_t g_waterfall_spectra_displayed = 0;   /* Total averaged spectra sent to display */
static uint32_t g_waterfall_frames_dropped = 0;      /* Frames lost due to queue overflow */

/* ============== Implementation ============== */

/**
 * FreeRTOS task for waterfall display update
 * Receives audio buffer messages from queue, processes through spectrogram,
 * and updates the waterfall display in real-time
 * 
 * Implements smart frame dropping and queue flushing to prevent backup
 */
static void waterfall_task(void *pvParameters)
{
    (void)pvParameters;
    
    printf("Waterfall task started\n");
    
    static uint32_t frame_count = 0;
    static uint32_t last_sequence = 0;
    static uint32_t last_depth_report = 0;
    static uint32_t peak_queue_depth = 0;
    AudioBufferMessage_t msg;
    
    /* Task main loop */
    while (1) {
        /* Monitor queue depth */
        UBaseType_t queue_depth = uxQueueMessagesWaiting(g_audioQueueWaterfall);
        if (queue_depth > peak_queue_depth) {
            peak_queue_depth = queue_depth;
        }
        
        /* Smart queue flushing: if queue backed up, drain it to get latest message */
        if (queue_depth >= 4) {
            /* Queue is full - drain old messages, keep only the newest */
            AudioBufferMessage_t temp_msg;
            uint32_t drained = 0;
            
            /* Drain all but one message from queue */
            while (uxQueueMessagesWaiting(g_audioQueueWaterfall) > 1) {
                if (xQueueReceive(g_audioQueueWaterfall, &temp_msg, pdMS_TO_TICKS(0)) == pdTRUE) {
                    msg = temp_msg;  /* Keep the last one we read */
                    drained++;
                }
            }
            
            g_waterfall_frames_dropped += drained - 1;  /* Count dropped frames via module-level global */
            
            if (drained > 0 && g_micDebug >= 1) {
                printf("[Waterfall] Queue FLUSHED: drained %lu messages, kept newest seq=%lu\n",
                       drained, msg.sequence);
            }
        } else if (xQueueReceive(g_audioQueueWaterfall, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* No message available - timeout */
            if (g_micDebug >= 1) {
                printf("[Waterfall] Queue timeout (no audio for 1 second)\n");
            }
            continue;
        }
        
        /* Periodic reporting */
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        if (current_time - last_depth_report >= 5000) {  /* Report every 5 seconds */
            last_depth_report = current_time;
            if (g_micDebug >= 1) {
                printf("[Waterfall] Stats: frames_accumulated=%lu, spectra_displayed=%lu, frames_dropped=%lu, peak_queue_depth=%u/4\n",
                       g_waterfall_frames_accumulated, g_waterfall_spectra_displayed, g_waterfall_frames_dropped, peak_queue_depth);
            }
            peak_queue_depth = 0;  /* Reset peak after reporting */
        }
        
        /* Check for missed buffers (sequence gap) */
        if (last_sequence > 0 && msg.sequence != last_sequence + 1) {
            if (g_micDebug >= 1) {
                printf("[Waterfall] Frame %lu: MISSED %lu buffers (seq %lu → %lu)\n",
                       frame_count, msg.sequence - last_sequence - 1, last_sequence, msg.sequence);
            }
        }
        last_sequence = msg.sequence;
        
        if (g_micDebug >= 1) {
            printf("[Waterfall] Frame %lu: Processing seq=%lu buf=%u (%u samples)\n",
                   frame_count, msg.sequence, msg.buffer_id, msg.sample_count);
        }
        
        /* If spectrogram context is configured */
        if (g_spectrogram_ctx != NULL) {
            lv_obj_t *canvas = waterfall_get_canvas();
            if (canvas != NULL) {
                /* === Load audio data into spectrogram input buffer === */
                if (g_micDebug >= 2) {
                    printf("[Waterfall] Frame %lu: Loading %u samples into spectrogram\n",
                           frame_count, msg.sample_count);
                }
                
                /* Feed audio data to spectrogram processor */
                spectrogram_update_input_buffer(g_spectrogram_ctx, 
                                               msg.buffer_ptr, 
                                               msg.sample_count);
                
                /* Compute FFT from buffered audio (but not bins yet) */
                int fft_result = spectrogram_compute_fft(g_spectrogram_ctx);
                
                if (g_micDebug >= 2) {
                    printf("[Waterfall] Frame %lu: FFT result=%d\n", frame_count, fft_result);
                }
                
                if (fft_result == 0) {
                    /* === Accumulate magnitude-squared spectrum === */
                    /* For each frequency band, sum the magnitude-squared of constituent FFT bins */
                    for (uint32_t bin = 0; bin < WATERFALL_FREQ_BANDS; bin++) {
                        /* Calculate range of FFT bins for this frequency band (same as spectrogram_compute_bins) */
                        uint32_t bin_start = bin * SPECTROGRAM_BIN_SIZE + SPECTROGRAM_BINS_SKIP;
                        uint32_t bin_end = bin_start + SPECTROGRAM_BIN_SIZE;
                        
                        /* Clamp to valid FFT magnitude range */
                        if (bin_start >= SPECTROGRAM_FFT_SIZE / 2) {
                            continue;
                        }
                        if (bin_end > SPECTROGRAM_FFT_SIZE / 2) {
                            bin_end = SPECTROGRAM_FFT_SIZE / 2;
                        }
                        
                        /* Sum magnitude-squared across all FFT bins in this display band */
                        for (uint32_t i = bin_start; i < bin_end; i++) {
                            int32_t mag = (int32_t)g_spectrogram_ctx->fft_magnitude[i];
                            g_waterfall_magnitude_accum[bin] += (uint64_t)(mag * mag);
                        }
                    }
                    
                    g_waterfall_accum_frame_count++;
                    g_waterfall_frames_accumulated++;
                    
                    if (g_micDebug >= 2) {
                        printf("[Waterfall] Frame %lu: Accumulated frame %u/100\n",
                               frame_count, g_waterfall_accum_frame_count);
                    }
                    
                    /* === Check if we have 100 frames accumulated === */
                    if (g_waterfall_accum_frame_count >= WATERFALL_ACCUMULATION_COUNT) {
                        /* Create waterfall bar from accumulated magnitudes */
                        t_waterfallBar bar;
                        memset(&bar, 0, sizeof(bar));
                        
                        /* Pass accumulated magnitude_sq directly (let log function handle scaling) */
                        for (uint8_t i = 0; i < WATERFALL_FREQ_BANDS; i++) {
                            /* Clamp accumulated values to uint32_t range for display */
                            bar.magnitude_sq[i] = (g_waterfall_magnitude_accum[i] > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)g_waterfall_magnitude_accum[i];
                            
                            if (g_micDebug >= 2) {
                                printf("[Waterfall] Band %u: accum=%llu display=%u\n",
                                       i, g_waterfall_magnitude_accum[i], bar.magnitude_sq[i]);
                            }
                        }
                        
                        /* Update waterfall display with mutex protection */
                        if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            waterfall_add_column(canvas, &bar);
                            xSemaphoreGive(g_LvglMutex);
                            g_waterfall_spectra_displayed++;
                            
                            if (g_micDebug >= 1) {
                                printf("[Waterfall] Spectrum %lu: Displayed averaged spectrum (100 frames)\n", g_waterfall_spectra_displayed);
                            }
                        } else if (g_micDebug >= 1) {
                            printf("[Waterfall] Frame %lu: LVGL mutex timeout\n", frame_count);
                        }
                        
                        /* Reset accumulator for next batch */
                        memset(g_waterfall_magnitude_accum, 0, sizeof(g_waterfall_magnitude_accum));
                        g_waterfall_accum_frame_count = 0;
                    }
                } else {
                    if (g_micDebug >= 1) {
                        printf("[Waterfall] Frame %lu: FFT/Bins computation failed\n", frame_count);
                    }
                }
                
                frame_count++;
            }
        }
    }
}

/**
 * Initialize the waterfall display canvas
 * Creates a new LVGL canvas widget that fills the entire display
 * Also spawns the waterfall processing task
 */
lv_obj_t * waterfall_init(void)
{
    /* Destroy any existing waterfall */
    if (g_waterfall_canvas != NULL) {
        waterfall_destroy(g_waterfall_canvas);
    }
    
    /* Get the active screen */
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        printf("ERROR: No active screen for waterfall display\n");
        return NULL;
    }
    
    /* Clear the screen first */
    lv_obj_clean(screen);
    
    /* Create canvas widget - fills entire display */
    lv_obj_t *canvas = lv_canvas_create(screen);
    if (canvas == NULL) {
        printf("ERROR: Failed to create LVGL canvas\n");
        return NULL;
    }
    
    /* Set canvas size to fill entire display */
    lv_obj_set_size(canvas, WATERFALL_WIDTH, WATERFALL_HEIGHT);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
    
    /* Allocate canvas buffer */
    /* For RGB565 (2 bytes per pixel): WATERFALL_WIDTH * WATERFALL_HEIGHT * 2 bytes */
    uint32_t buffer_size = WATERFALL_WIDTH * WATERFALL_HEIGHT * (LV_COLOR_DEPTH / 8);
    
    g_canvas_buffer = (uint8_t *)malloc(buffer_size);
    if (g_canvas_buffer == NULL) {
        printf("ERROR: Failed to allocate canvas buffer (%u bytes)\n", buffer_size);
        lv_obj_del(canvas);
        return NULL;
    }
    
    /* Initialize buffer to black (clear) */
    memset(g_canvas_buffer, 0, buffer_size);
    
    /* Set canvas buffer */
    lv_canvas_set_buffer(canvas, g_canvas_buffer, WATERFALL_WIDTH, WATERFALL_HEIGHT, LV_COLOR_FORMAT_RGB565);
    
    /* Store reference and reset column counter */
    g_waterfall_canvas = canvas;
    g_current_column = 0;
    
    printf("Waterfall display initialized (%u x %u pixels)\n", WATERFALL_WIDTH, WATERFALL_HEIGHT);
    printf("Canvas buffer allocated: %u bytes\n", buffer_size);
    
    /* Create waterfall processing task if not already created */
    if (g_waterfall_task_handle == NULL) {
        /* Pin waterfall task to Core 0 (moved from Core 1 to join UDP audio processing) */
        /* This allows TimerTask (Core 1) to do SPI display I/O without interfering */
        const UBaseType_t core_0_affinity = (1 << 0);  /* Core 0 only */
        
        BaseType_t result = xTaskCreateAffinitySet(
            waterfall_task,                    /* Task function */
            "waterfall",                       /* Task name */
            2048,                              /* Stack size in words */
            NULL,                              /* Task parameter */
            tskIDLE_PRIORITY + 2,              /* Priority */
            core_0_affinity,                   /* Core affinity: Core 0 (with UDP audio) */
            &g_waterfall_task_handle           /* Task handle */
        );
        
        if (result != pdPASS) {
            printf("ERROR: Failed to create waterfall task\n");
            waterfall_destroy(canvas);
            return NULL;
        }
        printf("Waterfall task created on Core 0\n");
    }
    
    return canvas;
}

/**
 * Helper function: Convert magnitude-squared to amplitude level (0-15)
 * Uses logarithmic lookup table for perceptually uniform levels
 * 
 * @param magnitude_sq Raw magnitude-squared value from FFT
 * @return Amplitude level 0-15
 */
static uint8_t magnitude_sq_to_amplitude_level(uint32_t magnitude_sq)
{
    /* Find the appropriate level by comparing against thresholds */
    for (uint8_t level = 15; level > 0; level--) {
        if (magnitude_sq >= power_threshold_log_lut[level]) {
            return level;
        }
    }
    return 0;  /* Below lowest threshold */
}

/**
 * Convert amplitude level to magnitude_sq value
 * Used by parser for manual control - maps color_index to corresponding magnitude_sq
 * The mapping follows the log thresholds: color n requires magnitude_sq >= threshold[n]
 */
uint32_t waterfall_color_index_to_magnitude_sq(uint8_t color_index)
{
    if (color_index == 0) {
        return 0;  /* Level 0: magnitude_sq < threshold[0] */
    } else if (color_index < 16) {
        /* Return the threshold value for this color level */
        return power_threshold_log_lut[color_index];
    } else {
        /* Clamp to maximum color level */
        return power_threshold_log_lut[15];
    }
}

/**
 * Add a waterfall bar to the display
 * Shifts existing display left by WATERFALL_X_GRANULARITY pixels and draws new data on the right
 * Processes all 16 frequency bins, applies log scaling, and maps to colors
 */
void waterfall_add_column(lv_obj_t *canvas, const t_waterfallBar *bar)
{
    if (canvas == NULL || g_canvas_buffer == NULL) {
        printf("ERROR: Waterfall not initialized\n");
        return;
    }
    
    if (bar == NULL) {
        printf("ERROR: Invalid waterfall bar pointer\n");
        return;
    }
    
    /* Calculate pixel position and size */
    uint16_t bytes_per_pixel = LV_COLOR_DEPTH / 8;
    uint16_t row_stride = WATERFALL_WIDTH * bytes_per_pixel;
    uint16_t col_start = WATERFALL_WIDTH - WATERFALL_X_GRANULARITY;  /* Starting column for new data */
    
    /* First, shift entire canvas left by WATERFALL_X_GRANULARITY pixels */
    for (uint16_t row = 0; row < WATERFALL_HEIGHT; row++) {
        uint8_t *row_ptr = &g_canvas_buffer[row * row_stride];
        
        /* Shift left by WATERFALL_X_GRANULARITY pixels */
        for (uint16_t col = 0; col < WATERFALL_WIDTH - WATERFALL_X_GRANULARITY; col++) {
            uint32_t from = (col + WATERFALL_X_GRANULARITY) * bytes_per_pixel;
            uint32_t to = col * bytes_per_pixel;
            
            for (uint16_t b = 0; b < bytes_per_pixel; b++) {
                row_ptr[to + b] = row_ptr[from + b];
            }
        }
    }
    
    /* Clear the rightmost WATERFALL_X_GRANULARITY columns to black */
    for (uint16_t row = 0; row < WATERFALL_HEIGHT; row++) {
        for (uint16_t col = col_start; col < WATERFALL_WIDTH; col++) {
            uint32_t offset = row * row_stride + col * bytes_per_pixel;
            uint8_t *pixel = &g_canvas_buffer[offset];
            pixel[0] = 0x00;  /* Black (RGB565) */
            pixel[1] = 0x00;
        }
    }
    
    /* Process each of the 16 frequency bins */
    for (uint8_t freq_bin = 0; freq_bin < WATERFALL_FREQ_BANDS; freq_bin++) {
        /* Convert magnitude-squared to amplitude level using log lookup table */
        uint8_t amplitude_level = magnitude_sq_to_amplitude_level(bar->magnitude_sq[freq_bin]);
        
        /* Get RGB color for this amplitude level and convert using LVGL */
        ParulaColor rgb = parula_colormap_16[amplitude_level];
        lv_color_t color = lv_color_make(rgb.r, rgb.g, rgb.b);
        
        /* Calculate which rows correspond to this frequency band */
        uint16_t row_start = freq_bin * WATERFALL_BAR_HEIGHT;
        uint16_t row_end = row_start + WATERFALL_BAR_HEIGHT;
        
        /* Draw this frequency band across all WATERFALL_X_GRANULARITY columns */
        for (uint16_t row = row_start; row < row_end; row++) {
            for (uint16_t col = col_start; col < WATERFALL_WIDTH; col++) {
                /* Use LVGL's lv_canvas_set_px with full opacity */
                lv_canvas_set_px(canvas, col, row, color, LV_OPA_COVER);
            }
        }
    }
    
    /* Invalidate canvas to trigger redraw */
    lv_obj_invalidate(canvas);
    
    g_current_column++;
}

/**
 * Destroy the waterfall canvas and free resources
 * Stops the waterfall task and frees allocated memory
 */
void waterfall_destroy(lv_obj_t *canvas)
{
    if (canvas == NULL) {
        return;
    }
    
    /* Delete the waterfall task if it exists */
    if (g_waterfall_task_handle != NULL) {
        vTaskDelete(g_waterfall_task_handle);
        g_waterfall_task_handle = NULL;
        printf("Waterfall task deleted\n");
    }
    
    /* Free canvas buffer */
    if (g_canvas_buffer != NULL) {
        free(g_canvas_buffer);
        g_canvas_buffer = NULL;
    }
    
    /* Delete canvas object */
    lv_obj_del(canvas);
    
    g_waterfall_canvas = NULL;
    g_current_column = 0;
    
    printf("Waterfall display destroyed\n");
}

/**
 * Get the current waterfall canvas
 */
lv_obj_t * waterfall_get_canvas(void)
{
    return g_waterfall_canvas;
}

/**
 * Clear the waterfall display while maintaining the canvas
 */
void waterfall_clear(lv_obj_t *canvas)
{
    if (canvas == NULL || g_canvas_buffer == NULL) {
        printf("ERROR: Waterfall not initialized\n");
        return;
    }
    
    /* Clear buffer to black (0x0000) */
    uint32_t buffer_size = WATERFALL_WIDTH * WATERFALL_HEIGHT * (LV_COLOR_DEPTH / 8);
    memset(g_canvas_buffer, 0x00, buffer_size);
    
    /* Invalidate to trigger redraw */
    lv_obj_invalidate(canvas);
    
    printf("Waterfall display cleared\n");
}

/**
 * Get accumulation progress for current batch
 * Returns how many frames have been accumulated toward the next display update
 * 
 * @return Number of frames accumulated (0-99), or 100 when batch complete
 */
uint32_t waterfall_get_accumulation_progress(void)
{
    return g_waterfall_accum_frame_count;
}

/**
 * Set frame skip rate (deprecated - kept for compatibility)
 * In accumulation mode, this function does nothing
 * Frame rate is fixed at 1 display update per 100 audio frames
 * 
 * @param skip_rate Ignored
 */
void waterfall_set_frame_skip_rate(uint8_t skip_rate)
{
    (void)skip_rate;  /* Parameter ignored in accumulation mode */
    
    printf("[Waterfall] Frame skip rate setter deprecated - system uses spectrum accumulation\n");
    printf("[Waterfall] Mode: Accumulate 100 frames (~1 Hz display)\n");
}

/**
 * Get the current frame skip rate (deprecated)
 * In accumulation mode, always returns WATERFALL_ACCUMULATION_COUNT (100)
 * 
 * @return WATERFALL_ACCUMULATION_COUNT (100 frames per display update)
 */
uint8_t waterfall_get_frame_skip_rate(void)
{
    /* In accumulation mode, return the accumulation count clamped to uint8_t */
    return (WATERFALL_ACCUMULATION_COUNT > 255) ? 255 : (uint8_t)WATERFALL_ACCUMULATION_COUNT;
}

/**
 * Set the spectrogram context for the waterfall task to use
 * Called by main.c to attach the spectrogram processor
 * 
 * @param spec pointer to initialized spectrogram_t context
 */
void waterfall_set_spectrogram(spectrogram_t *spec)
{
    g_spectrogram_ctx = spec;
    if (spec != NULL) {
        printf("Waterfall spectrogram context configured\n");
    } else {
        printf("Waterfall spectrogram context cleared\n");
    }
}

/**
 * Get the current waterfall queue depth
 * Used for diagnostics and performance monitoring
 * 
 * @return Number of messages currently in waterfall audio queue (0-4)
 */
UBaseType_t waterfall_get_queue_depth(void)
{
    if (g_audioQueueWaterfall == NULL) {
        return 0;
    }
    return uxQueueMessagesWaiting(g_audioQueueWaterfall);
}
