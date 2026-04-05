/**
 * Waterfall Display using LVGL Canvas
 * 
 * Implements a real-time frequency waterfall display with 16 frequency bands.
 * The display scrolls horizontally (time axis) with vertical frequency bins.
 * Uses LVGL canvas widget for efficient drawing.
 * 
 * The waterfall includes a background task that processes audio from the
 * microphone through the spectrogram FFT and updates the display in real-time.
 */

#ifndef WATERFALL_H
#define WATERFALL_H

#include <stdint.h>
#include <lvgl.h>
#include "spectrogram.h"

/* Display dimensions */
#define WATERFALL_WIDTH    320      /* Horizontal pixels (time axis) */
#define WATERFALL_HEIGHT   240      /* Vertical pixels (frequency axis) */
#define WATERFALL_FREQ_BANDS 16     /* Number of frequency bins */
#define WATERFALL_BAR_HEIGHT (WATERFALL_HEIGHT / WATERFALL_FREQ_BANDS)  /* Pixels per band (15) */

/* X-axis granularity: pixels shifted per addWaterfall call (tunable) */
#define WATERFALL_X_GRANULARITY 10  /* Shift 10 pixels per call */

/* ============== Data Types ============== */

/**
 * Waterfall bar - contains raw magnitude-squared values from FFT for each frequency bin
 * Waterfall display will apply log scaling and color mapping
 */
typedef struct {
    uint32_t magnitude_sq[WATERFALL_FREQ_BANDS];  /* Magnitude squared for each frequency bin */
} t_waterfallBar;

/* Canvas buffer configuration for LVGL canvas widget */
#define WATERFALL_CANVAS_BIT_DEPTH LV_COLOR_DEPTH

/**
 * Initialize the waterfall display canvas
 * Creates a new LVGL canvas and clears the screen
 * Should be called before adding waterfall data
 * 
 * @return pointer to the canvas object, or NULL on failure
 */
lv_obj_t * waterfall_init(void);

/**
 * Destroy the waterfall canvas and clear the screen
 * 
 * @param canvas pointer to the canvas object
 */
void waterfall_destroy(lv_obj_t *canvas);

/**
 * Add a waterfall bar to the display
 * Shifts the display left and adds a new bar on the right
 * Applies log scaling and color mapping to the magnitude-squared values
 * 
 * @param canvas pointer to the canvas object
 * @param bar pointer to t_waterfallBar containing magnitude-squared values for 16 frequency bins
 */
void waterfall_add_column(lv_obj_t *canvas, const t_waterfallBar *bar);

/**
 * Get the current waterfall canvas
 * 
 * @return pointer to the current canvas object, or NULL if not initialized
 */
lv_obj_t * waterfall_get_canvas(void);

/**
 * Clear the waterfall display while maintaining the canvas
 * 
 * @param canvas pointer to the canvas object
 */
void waterfall_clear(lv_obj_t *canvas);

/**
 * Convert amplitude level (color index) to magnitude_sq value
 * Used by parser/external code for manual control - maps color_index to corresponding magnitude_sq
 * 
 * @param color_index amplitude level (0-15, where 0=dark blue, 15=bright yellow)
 * @return magnitude_sq value that maps to the specified color via log scaling
 */
uint32_t waterfall_color_index_to_magnitude_sq(uint8_t color_index);

/**
 * Set the spectrogram context for the waterfall task to use
 * Called by main.c to attach the spectrogram processor
 * 
 * @param spec pointer to initialized spectrogram_t context
 */
void waterfall_set_spectrogram(spectrogram_t *spec);

/**
 * Get the current waterfall queue depth for diagnostics
 * Returns the number of pending audio buffers in the waterfall processing queue
 * 
 * @return Number of messages in queue (0-4)
 */
UBaseType_t waterfall_get_queue_depth(void);

/**
 * Set the frame skip rate for waterfall display updates
 * Process 1 of every N buffers to control display update rate
 * 
 * @param skip_rate Number of buffers to skip (1=every buffer, 2=every other, etc.)
 */
void waterfall_set_frame_skip_rate(uint8_t skip_rate);

/**
 * Get accumulation progress for current spectrum batch
 * In accumulation mode, returns how many frames have been accumulated (0-99)
 * When this reaches 100, the spectrum is averaged and displayed
 * 
 * @return Number of frames accumulated toward next display update (0-99)
 */
uint32_t waterfall_get_accumulation_progress(void);

/**
 * Get the current frame skip rate
 * 
 * @return Current skip rate
 */
uint8_t waterfall_get_frame_skip_rate(void);

/**
 * Set the waterfall spectrum gain
 * Gain is a percentage value (1-1000+)
 * Internally computed as gainSquared = (gain/100)^2 for efficient spectrum scaling
 * 
 * @param gain New gain value (1 or higher)
 */
void waterfall_set_gain(uint32_t gain);

/**
 * Get the current waterfall spectrum gain
 * 
 * @return Current gain value
 */
uint32_t waterfall_get_gain(void);

/**
 * Get the squared gain value used internally for spectrum scaling
 * Computed as (gain/100)^2 for efficient per-bin multiplication
 * 
 * @return Current gain squared value (float32_t)
 */
float32_t waterfall_get_gain_squared(void);

#endif /* WATERFALL_H */
