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
 * display controller.
 * 
 * @return 0 on success, non-zero on failure
 */
int st7789_init_hw(void);
