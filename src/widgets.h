#ifndef WIDGETS_H
#define WIDGETS_H

#include <stdint.h>

/**
 * Create the initial screen with scale widgets and animations
 */
void create_initial_screen(void);

/**
 * Create the clock widget
 */
void create_clock_widget(void);

/**
 * Remove the clock widget
 */
void remove_clock_widget(void);

/**
 * Clear the screen and remove all widgets
 */
void clear_screen(void);

/**
 * Set the clock needle value
 * @param value Value to set (0-59)
 */
void set_clock_needle_value(int32_t value);

/**
 * LVGL Timer Update Task
 * Periodically calls lv_timer_handler() to process LVGL timers.
 * Runs on Core 1 (isolated from audio processing on Core 0).
 * @param parameters Task parameters (unused)
 */
void timer_update_task(void *parameters);

#endif // WIDGETS_H
