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

/**
 * Clear display with white color
 */
void clearDisplay(void) {
    // Create white bar (all pixels set to white)
    uint16_t whiteBar[PIXEL_COUNT];
    for (uint32_t i = 0; i < PIXEL_COUNT; i++) {
        whiteBar[i] = 65535;  // White in RGB565
    }
    
    // Fill entire screen with white bars (32 bars × 10 pixels each)
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
