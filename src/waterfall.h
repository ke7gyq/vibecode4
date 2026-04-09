/**
 * Waterfall Display - Optimized Hardware Scrolling
 * 
 * Ultra-fast waterfall spectrogram display using ST7789 hardware scroll
 * 24 frequency bins displayed with 10×10 pixel blocks per frequency
 */

#ifndef WATERFALL_H
#define WATERFALL_H

#include <stdint.h>
#include <stdbool.h>

/* Display geometry constants */
#define BAR_HEIGHT          240     /* Total vertical pixels */
#define PIXEL_WIDTH         10      /* Pixels wide per frequency bin */
#define PIXEL_HEIGHT        10      /* Pixels tall per frequency bin */
#define PIXELS_PER_BAR      (BAR_HEIGHT / PIXEL_HEIGHT)  /* 24 frequency bins */
#define NO_OF_BARS          (320 / PIXEL_WIDTH) /* 32 bars across */
#define PIXEL_COUNT         (BAR_HEIGHT * PIXEL_WIDTH)   /* 2400 pixels per bar */

/* Screen dimensions */
#define SCREEN_WIDTH        320
#define SCREEN_HEIGHT       240

/**
 * Pixel buffer structure
 * Union allows access as flat array or 3D indexed array
 * 24 frequency bins × 10 pixels wide × 10 pixels tall = 2400 pixels
 */
typedef union {
    uint16_t data[PIXEL_COUNT];                          /* Flat array (2400 elements) */
    uint16_t pixels[PIXELS_PER_BAR][PIXEL_HEIGHT][PIXEL_WIDTH];  /* 3D array [color][height][width] */
} pixel_buffer_t;

/* ============== Public API ============== */

/**
 * Set the active colormap
 * @param selectedColormapIndex 0=Jet, 1=Parula (wraps modulo available)
 */
void setColorMap(uint16_t selectedColormapIndex);

/**
 * Get the current colormap index
 * @return Current colormap index (0=Jet, 1=Parula)
 */
uint16_t getColorMap(void);

/**
 * Clear the entire display with white color
 * Fills all 32 bars with white pixels
 */
void clearDisplay(void);

/**
 * Add a new waterfall bar with hardware scroll
 * Applies colormap to 24 frequency indices, expands to 10×10 pixel blocks,
 * scrolls the display, and writes the new bar
 * 
 * Performs >100 updates per second using DMA and hardware scroll
 * 
 * @param colormapIndexArray Array of 24 colormap indices (0-15)
 * @param noOfEntries Number of valid entries (should be 24/PIXELS_PER_BAR)
 */
void addBar(const uint16_t *colormapIndexArray, uint16_t noOfEntries);

/**
 * Convert colormap indices to pixel buffer
 * Internal function that maps colormap indices to RGB565 colors
 * and expands to 10×10 pixel blocks
 * 
 * @param colormapIndexArray Array of 24 colormap indices (0-15)
 * @param noOfEntries Number of valid entries
 */
void fillPixelsToBar(const uint16_t *colormapIndexArray, uint16_t noOfEntries);

/**
 * Initialize waterfall display mode
 * Initializes ST7789 in portrait mode with vertical scroll hardware
 * Clears the screen to white
 */
void waterfall_mode_init(void);

/**
 * FreeRTOS Waterfall Task - Processes audio FFT and displays spectrogram
 * 
 * Waits on g_audioQueueWaterfall for audio buffer messages, computes FFT,
 * accumulates frames, and displays waterfall bars. Runs on Core 1.
 * 
 * @param parameters Unused (FreeRTOS task parameter)
 */
void waterfall_task(void *parameters);

/* ============== Display Driver Functions (from st7789.c) ============== */

/**
 * Initialize ST7789 LCD hardware
 * Sets up PIO, GPIO, DMA, and initializes controller
 * @return 0 on success
 */
int st7789_lcd_init(void);

/**
 * Set display to vertical mode for waterfall (portrait)
 */
void st7789_setVerticalMode(void);

/**
 * Set scroll margins
 * @param top Fixed lines at top
 * @param bottom Fixed lines at bottom
 */
void st7789_setScrollMargins(uint16_t top, uint16_t bottom);

/**
 * Set hardware vertical scroll address
 * @param vsp Vertical scroll position (0-319)
 */
void st7789_scrollAddress(uint16_t vsp);

/**
 * Draw a rectangular region
 * @param x0 X start coordinate
 * @param y0 Y start coordinate
 * @param width Width in pixels
 * @param height Height in pixels
 * @param data Pointer to RGB565 pixel data
 */
void drawRegion(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height, const uint16_t *data);

/**
 * Get current waterfall gain value
 * Defined in spectrogram.c - applies linear gain before log scaling
 * @return Gain value * GAIN_NORMALIZATION (e.g., 10 = 1.0x default)
 */
uint32_t getWaterfallGain(void);

/**
 * Set waterfall gain value
 * Defined in spectrogram.c - applies to magnitude before logarithmic scaling
 * @param gain Gain value * GAIN_NORMALIZATION (e.g., 10 = 1.0x, 20 = 2.0x)
 */
void setWaterfallGain(uint32_t gain);

/**
 * Draw a test bar with all frequency bins set to the same colormap index
 * Useful for testing display and colormap functionality
 * @param colormapIndex Index into the current colormap (0-15)
 */
void drawTestBar(uint8_t colormapIndex);

/**
 * Draw individual color bars for testing
 * Displays white, blue, green, and red bars in sequence
 * Useful for verifying color accuracy and display functionality
 */
void drawColorBars(void);

/**
 * Get waterfall queue depth
 * @return Queue depth (0 since FreeRTOS task is removed)
 */
uint32_t getWaterfallQueueDepth(void);

/**
 * Check if waterfall server is running
 * @return false (waterfall is now part of spectrogram processing)
 */
bool waterfallServerIsRunning(void);

/* ============== Waterfall Display Mode Management ============== */

/**
 * Waterfall mode enum (defined in parser.c)
 * Controls whether audio FFT data feeds the waterfall or display test functions have exclusive control
 */
typedef enum {
    WATERFALL_MODE_OFF = 0,           /* Waterfall disabled */
    WATERFALL_MODE_TEST = 1,          /* Test mode (test functions have exclusive control) */
    WATERFALL_MODE_LIVE_AUDIO = 2     /* Live audio mode (audio task feeds FFT data) */
} waterfall_mode_t;

/**
 * Get current waterfall display mode
 * @return Current mode (OFF, TEST, or LIVE_AUDIO)
 */
waterfall_mode_t waterfall_get_mode(void);

/**
 * Set waterfall display mode
 * When switching to TEST mode, audio task will stop feeding FFT data.
 * When switching to LIVE_AUDIO mode, audio task will resume feeding FFT data.
 * @param mode New mode (OFF, TEST, or LIVE_AUDIO)
 */
void waterfall_set_mode(waterfall_mode_t mode);

/**
 * Check if audio task should feed FFT data to waterfall
 * Called by audio processing pipeline before calling waterfall_accm_add_fft()
 * @return 1 if audio should feed waterfall, 0 otherwise
 */
int waterfall_should_feed_fft(void);

/**
 * Get pointer to global waterfall accumulator (for audio task)
 * Called by audio processing pipeline to feed FFT data
 * Defined in parser.c - returns NULL if not in LIVE_AUDIO mode (prevents unintended data feeding)
 * @return Pointer to waterfall accumulator (or NULL if not in LIVE_AUDIO mode)
 */
void* waterfall_get_accumulator(void);

/* ============== Compatibility Aliases ============== */
#define waterfall_get_gain() getWaterfallGain()
#define waterfall_set_gain(g) setWaterfallGain(g)
#define waterfall_get_colormap() getColorMap()
#define waterfall_set_colormap(idx) setColorMap((uint16_t)(idx))
#define waterfall_draw_bar(idx) drawTestBar((uint8_t)(idx))
#define waterfall_get_queue_depth() getWaterfallQueueDepth()
#define waterfall_server_is_running() waterfallServerIsRunning()

#endif /* WATERFALL_H */
