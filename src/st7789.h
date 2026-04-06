#pragma once

#include <lvgl.h>
#include <stdint.h>

/**
 * Initialize LVGL and ST7789 display
 * 
 * Initializes the LVGL library, creates display buffers, and configures
 * the ST7789 controller for the connected display.
 * 
 * @return Pointer to the initialized lv_display_t object, or NULL on error
 */
lv_display_t *initialize_lvgl(void);

/**
 * Display flush callback for LVGL
 * 
 * Transfers the pixel data from the LVGL buffer to the ST7789 display
 * using DMA. This function is registered as the flush callback for
 * the display driver.
 * 
 * @param display Pointer to the display object
 * @param area Area of the display to update
 * @param px_map Pointer to the pixel map buffer containing pixel data
 */
void st7789_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map);

/**
 * Initialize ST7789 hardware
 * 
 * Sets up the SPI interface, GPIO pins, and initializes the ST7789
 * display controller. Initializes in landscape mode by default.
 * 
 * @return 0 on success, non-zero on failure
 */
int st7789_init_hw(void);

/**
 * Set display to portrait mode (240×320)
 * Used for waterfall spectrogram display with vertical scroll
 */
void st7789_set_portrait_mode(void);

/**
 * Set display to landscape mode (320×240)
 * Default mode for general UI/menu displays
 */
void st7789_set_landscape_mode(void);

/**
 * Set vertical scroll area definition
 * Defines which part of display can scroll (typically full screen)
 * 
 * @param lines_before Fixed lines at top (usually 0)
 * @param scroll_lines Lines that can scroll (usually SCREEN_HEIGHT)
 * @param lines_after Fixed lines at bottom (usually 0)
 */
void st7789_set_vertical_scroll_area(uint16_t lines_before, uint16_t scroll_lines, uint16_t lines_after);

/**
 * Set vertical scroll start address
 * Changes which line appears at the top of the display (hardware wrap-around)
 * 
 * @param start_line Line number to display at top (0 to SCREEN_HEIGHT-1)
 */
void st7789_set_vertical_scroll_offset(uint16_t start_line);

/**
 * Write a single vertical line (column) of pixels
 * Used by waterfall to add new frequency column on the left after scrolling
 * 
 * @param x Column position (typically 0 for left edge after scroll)
 * @param y Starting row
 * @param height Number of pixels to write
 * @param data Pixel data (16-bit color values)
 */
void st7789_write_column(uint16_t x, uint16_t y, uint16_t height, const uint16_t *data);
