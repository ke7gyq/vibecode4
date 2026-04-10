/**
 * Waterfall Display - Optimized Hardware Scrolling Version
 * 
 * Ultra-fast waterfall spectrogram using ST7789 hardware scroll
 * No LVGL, no accumulators - processes FFT bins directly to display
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "st7789.h"
#include "waterfall.h"
#include "spectrogram.h"
#include "microphone.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pico/time.h"

#define COLORMAP_SIZE 16

/* Jet colormap: RGB565 format, blue to red spectrum */
static const uint16_t RGB_jet_colormap_16[COLORMAP_SIZE] = {
    0x0004, 0x0006, 0x00C7, 0x04D7, 0x0AB7, 0x0EFF, 
    0x1FDF, 0x1FDD, 0x1FDB, 0x1FD9, 0xFD80, 0x9200, 
    0x4D00, 0x0800, 0x0000, 0xF800
};

/* Parula colormap: RGB565 format */
static const uint16_t parula_colormap_16[COLORMAP_SIZE] = {
    0x400A, 0x484B, 0x48CC, 0x494C, 0x400C, 0x3BEB, 
    0x3049, 0x2288, 0x1EC6, 0x1206, 0x27E8, 0x420B, 
    0x728C, 0x41E8, 0x77C5, 0x7BE4
};

/* Colormap array and active colormap pointer */
static const uint16_t * const aColorPointers[] = {
    RGB_jet_colormap_16,
    parula_colormap_16
};

static const uint16_t *g_colorPointer = RGB_jet_colormap_16;
static uint16_t g_colormapIndex = 0;

/* Pixel buffer: 24 frequency bins × 10 pixels wide × 10 pixels tall */
pixel_buffer_t pixBuf;

typedef uint16_t pixelColors_t[PIXELS_PER_BAR];

/**
 * Set active colormap by index
 * @param selectedColormapIndex 0=Jet, 1=Parula, or modulo of available colormaps
 */
void setColorMap(uint16_t selectedColormapIndex) {
    uint16_t num_colormaps = sizeof(aColorPointers) / sizeof(aColorPointers[0]);
    uint16_t idx = selectedColormapIndex % num_colormaps;
    g_colorPointer = aColorPointers[idx];
    g_colormapIndex = idx;
}

/**
 * Get current colormap index
 */
uint16_t getColorMap(void) {
    return g_colormapIndex;
}

/**
 * Map frequency colormap indices to pixel buffer
 * Expands 24 frequency bins into 10×10 pixel blocks
 * @param freqData Array of 24 RGB565 color values
 */
static void pxArrayToPixBuf(const pixelColors_t freqData) {
    uint16_t color, xPos, yPos, colorIdx;
    for (colorIdx = 0; colorIdx < PIXELS_PER_BAR; colorIdx++) {
        color = freqData[colorIdx];
        for (xPos = 0; xPos < PIXEL_WIDTH; xPos++) {
            for (yPos = 0; yPos < PIXEL_HEIGHT; yPos++) {
                pixBuf.pixels[colorIdx][yPos][xPos] = color;
            }
        }
    }
}

/**
 * Convert colormap indices to RGB565 colors and fill pixel buffer
 * @param colormapIndexArray Array of 24 colormap indices (0-15)
 * @param noOfEntries Number of valid entries (should be PIXELS_PER_BAR=24)
 */
void fillPixelsToBar(const uint16_t *colormapIndexArray, uint16_t noOfEntries) {
    pixelColors_t freqData;

    for (uint16_t idx = 0; idx < PIXELS_PER_BAR; idx++) {
        if (idx >= noOfEntries) {
            freqData[idx] = 0;  // Black for unused entries
        } else {
            uint16_t idx2 = colormapIndexArray[idx];
            // Clamp to valid colormap range, 0 (black) if out of bounds
            uint16_t color = (idx2 < COLORMAP_SIZE) ? g_colorPointer[idx2] : 0;
            freqData[idx] = color;
        }
    }
    pxArrayToPixBuf(freqData);
}

/* =====================================================================
 * Global color bar definitions for display testing
 * =====================================================================
 */

/* RGB565 color values */
#define COLOR_WHITE 0xFFFF  /* 11111 11111 11111 */
#define COLOR_BLUE  0x001F  /* 00000 00000 11111 */
#define COLOR_GREEN 0x07E0  /* 00000 11111 00000 */
#define COLOR_RED   0xF800  /* 11111 00000 00000 */

/**
 * Static color bar buffers initialized with RGB565 values
 * Each bar is PIXEL_COUNT pixels filled with a solid color
 */
static const uint16_t whiteBar[PIXEL_COUNT] = {[0 ... (PIXEL_COUNT - 1)] = COLOR_WHITE};
static const uint16_t blueBar[PIXEL_COUNT] = {[0 ... (PIXEL_COUNT - 1)] = COLOR_BLUE};
static const uint16_t greenBar[PIXEL_COUNT] = {[0 ... (PIXEL_COUNT - 1)] = COLOR_GREEN};
static const uint16_t redBar[PIXEL_COUNT] = {[0 ... (PIXEL_COUNT - 1)] = COLOR_RED};


/**
 * Clear display with white color
 */
void clearDisplay(void) {
    /* Fill entire screen with white bars (32 bars × 10 pixels each) */
    for (int idx = 0; idx < NO_OF_BARS; idx++) {
        drawRegion(PIXEL_WIDTH * idx, 0, PIXEL_WIDTH, BAR_HEIGHT, whiteBar);
    }
}

/* Horizontal scroll offset for waterfall animation */
static uint16_t pixelOffset = 0;

/**
 * Add a new waterfall bar with hardware scroll
 * Scrolls display right by PIXEL_WIDTH pixels, writes new bar on left edge
 * @param colormapIndexArray Array of 24 colormap indices (0-15)
 * @param noOfEntries Number of valid entries (should be PIXELS_PER_BAR=24)
 */
void addBar(const uint16_t *colormapIndexArray, uint16_t noOfEntries) {
    fillPixelsToBar(colormapIndexArray, noOfEntries);
    
    // Draw the bar at current offset position
    drawRegion(SCREEN_WIDTH - PIXEL_WIDTH - pixelOffset, 0, PIXEL_WIDTH, BAR_HEIGHT, pixBuf.data);
    
    // Update hardware scroll address for next iteration
    st7789_scrollAddress(pixelOffset + PIXEL_WIDTH);
    
    // Advance offset for next call
    pixelOffset += PIXEL_WIDTH;
    if (pixelOffset >= SCREEN_WIDTH) {
        pixelOffset = 0;
    }
}

/**
 * Initialize waterfall mode
 * Sets up display in portrait mode with vertical scroll
 */
void waterfall_mode_init(void) {
    st7789_lcd_init();
    st7789_setVerticalMode();
    st7789_setScrollMargins(0, 0);
    clearDisplay();
}

/**
 * FreeRTOS Waterfall Task - Processes audio FFT and displays spectrogram
 * 
 * Waits on g_audioQueueWaterfall for audio buffer messages from microphone,
 * computes FFT using spectrogram processor, accumulates frames, and displays
 * when a full bar is ready.
 * 
 * Runs on Core 1 to isolate display I/O from real-time audio on Core 0.
 */
extern SemaphoreHandle_t g_LvglMutex;  /* Display mutex - protect LVGL/display access */
extern spectrogram_t g_spectrogram;    /* Global spectrogram context */

void waterfall_task(void *parameters) {
    (void)parameters;
    
    printf("[Waterfall] Task started on Core 1\n");
    fflush(stdout);
    
    /* Local accumulator for FFT frame integration */
    waterfall_accm_t accm;
    waterfall_accm_init(&accm);
    
    /* Local buffer for colormap indices (24 bins) */
    uint16_t logAccmPower[24];
    
    /* Statistics */
    uint32_t frame_count = 0;
    uint32_t bar_count = 0;
    uint32_t last_report = 0;
    
    while (1) {
        /* Wait forever for audio buffer from queue */
        AudioBufferMessage_t msg;
        
        if (xQueueReceive(g_audioQueueWaterfall, &msg, portMAX_DELAY) != pdTRUE) {
            printf("[Waterfall] Queue receive failed\n");
            continue;
        }
        
        /* Check waterfall mode - only process in LIVE_AUDIO mode */
        if (waterfall_get_mode() != WATERFALL_MODE_LIVE_AUDIO) {
            continue;  /* Silently skip if not in LIVE_AUDIO mode */
        }
        
        /* Validate message */
        if (msg.buffer_ptr == NULL || msg.sample_count == 0) {
            printf("[Waterfall] Invalid message: NULL buffer or 0 samples\n");
            continue;
        }
        
        /* Process audio samples through spectrogram (FFT + binning) */
        int spec_result = spectrogram_process_samples(&g_spectrogram, msg.buffer_ptr, msg.sample_count);
        if (spec_result != 0) {
            printf("[Waterfall] Spectrogram processing failed: %d\n", spec_result);
            continue;
        }
        
        frame_count++;
        
        /* Get FFT output and add to accumulator */
        /* CRITICAL FIX: Pass fft_output (complex: 256 real/imag pairs) not fft_magnitude (magnitude: 128 values) */
        int accm_result = waterfall_accm_add_fft(&accm, g_spectrogram.fft_output, SPECTROGRAM_FFT_SIZE / 2);
        
        /* If bar is ready (accm_result == 1), extract and display */
        if (accm_result == 1) {
            /* Extract accumulated bar data */
            if (waterfall_accm_get_bar(&accm, logAccmPower) == 0) {
                bar_count++;
                
                /* Take display mutex before updating screen */
                if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    /* Render the bar to screen */
                    fillPixelsToBar(logAccmPower, 24);
                    addBar(logAccmPower, 24);
                    
                    xSemaphoreGive(g_LvglMutex);
                } else {
                    printf("[Waterfall] Display mutex timeout\n");
                    fflush(stdout);
                }
            }
        }
        
        /* Periodic reporting */
        // Frame counting still active, but reporting disabled for cleaner console
    }
}

/**
 * Draw a test bar with all frequency bins set to the same colormap index
 * Useful for testing display and colormap functionality
 * @param colormapIndex Index into the current colormap (0-15)
 */
void drawTestBar(uint8_t colormapIndex) {
    /* Create bar with all frequency bins set to the same color index */
    uint16_t testBar[PIXELS_PER_BAR];
    
    /* Clamp index to valid range */
    if (colormapIndex >= COLORMAP_SIZE) {
        colormapIndex = COLORMAP_SIZE - 1;
    }
    
    /* Fill array with same colormap index for all frequencies */
    for (uint16_t i = 0; i < PIXELS_PER_BAR; i++) {
        testBar[i] = colormapIndex;
    }
    
    /* Add this bar to the waterfall display with hardware scroll */
    addBar(testBar, PIXELS_PER_BAR);
}

/**
 * Draw individual color bars for testing
 * Draws white, blue, green, and red bars in sequence across the display
 * Useful for verifying color accuracy and display functionality
 */
void drawColorBars(void) {
    /* Each color is displayed as 8 bars (32 total bars / 4 colors) */
    const uint16_t bars_per_color = 8;
    
    /* Draw white bars */
    for (uint16_t i = 0; i < bars_per_color; i++) {
        drawRegion(PIXEL_WIDTH * i, 0, PIXEL_WIDTH, BAR_HEIGHT, whiteBar);
    }
    
    /* Draw blue bars */
    for (uint16_t i = bars_per_color; i < bars_per_color * 2; i++) {
        drawRegion(PIXEL_WIDTH * i, 0, PIXEL_WIDTH, BAR_HEIGHT, blueBar);
    }
    
    /* Draw green bars */
    for (uint16_t i = bars_per_color * 2; i < bars_per_color * 3; i++) {
        drawRegion(PIXEL_WIDTH * i, 0, PIXEL_WIDTH, BAR_HEIGHT, greenBar);
    }
    
    /* Draw red bars */
    for (uint16_t i = bars_per_color * 3; i < bars_per_color * 4; i++) {
        drawRegion(PIXEL_WIDTH * i, 0, PIXEL_WIDTH, BAR_HEIGHT, redBar);
    }
}

/**
 * Get waterfall queue depth
 * @return Queue depth (0 since FreeRTOS task is removed)
 */
uint32_t getWaterfallQueueDepth(void) {
    return 0;  /* No queue in new hardware-scroll implementation */
}

/**
 * Check if waterfall server is running
 * @return false (waterfall is now part of spectrogram processing)
 */
bool waterfallServerIsRunning(void) {
    /* Waterfall task is running if mode is LIVE_AUDIO (mode 2) */
    return (waterfall_get_mode() == WATERFALL_MODE_LIVE_AUDIO);
}
