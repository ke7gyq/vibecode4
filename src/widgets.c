#include <stdio.h>
#include <lvgl.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <pico/multicore.h>
#include "widgets.h"

/* External display pointer */
extern lv_display_t *display;

/* Static variables for main scale widgets */
// static lv_obj_t *main_screen_widgets = NULL;

/* Static variables for clock widget */
static lv_obj_t *needle_line = NULL;
static lv_obj_t *scale_line = NULL;
static lv_anim_t anim_scale_line;

/**
 * Animation callback for clock scale needle line
 */
static void set_clock_needle_value_callback(void * obj, int32_t v)
{
    if (scale_line && needle_line) {
        lv_scale_set_line_needle_value(scale_line, needle_line, 60, v);
    }
}

/**
 * Animation callback for left scale needle line (in initial screen)
 */
static void set_needle_line_value(void * obj, int32_t v)
{
    lv_scale_set_line_needle_value((lv_obj_t *)obj, obj, 60, v);
}

/**
 * Animation callback for right scale needle line (in initial screen)
 */
static void set_needle_right_value(void * obj, int32_t v)
{
    lv_scale_set_line_needle_value((lv_obj_t *)obj, obj, 60, v);
}

/**
 * Create the initial screen with scale widgets and animations
 */
void create_initial_screen(void)
{
    // Get the active screen (created in initialize_lvgl)
    lv_obj_t *screen = lv_screen_active();
    
    if (screen == NULL) {
        printf("ERROR: No active screen in create_initial_screen\n");
        return;
    }
    
    // Create "Hello World" label
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "Hello World");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

/**
 * Create the clock widget
 */
void create_clock_widget(void)
{
    printf ("In StartClock\n");
    if (scale_line != NULL) {
        printf("Clock widget already exists\n");
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    
    scale_line = lv_scale_create(screen);
    
    lv_obj_set_size(scale_line, 150, 150);
    lv_scale_set_mode(scale_line, LV_SCALE_MODE_ROUND_INNER);
    lv_obj_set_style_bg_opa(scale_line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scale_line, lv_palette_lighten(LV_PALETTE_RED, 5), 0);
    lv_obj_set_style_radius(scale_line, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(scale_line, true, 0);
    lv_obj_align(scale_line, LV_ALIGN_CENTER, 0, 0);
    
    lv_scale_set_label_show(scale_line, true);
    
    lv_scale_set_total_tick_count(scale_line, 31);
    lv_scale_set_major_tick_every(scale_line, 5);
    
    lv_obj_set_style_length(scale_line, 5, LV_PART_ITEMS);
    lv_obj_set_style_length(scale_line, 10, LV_PART_INDICATOR);
    lv_scale_set_range(scale_line, 0, 60);
    
    lv_scale_set_angle_range(scale_line, 270);
    lv_scale_set_rotation(scale_line, 135);
    
    // Create needle line
    needle_line = lv_line_create(scale_line);
    lv_obj_set_style_line_width(needle_line, 6, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(needle_line, true, LV_PART_MAIN);
    
    printf("Clock widget created\n");

   
}

/**
 * Remove the clock widget
 */
void remove_clock_widget(void)
{
    
    if (scale_line != NULL) {
        lv_obj_del(scale_line);
        scale_line = NULL;
        needle_line = NULL;
        printf("Clock widget removed\n");
    }

}

/**
 * Clear the screen and remove all widgets
 */
void clear_screen(void) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    scale_line = NULL;
    needle_line = NULL;

}

/**
 * Set the clock needle value
 */
void set_clock_needle_value(int32_t value) {
    if (scale_line && needle_line) {
        // Clamp value to 0-59
        if (value < 0) value = 0;
        if (value > 59) value = 59;
        lv_scale_set_line_needle_value(scale_line, needle_line, 60, value);
    }

}

/**
 * LVGL Timer Update Task
 * 
 * Periodically calls lv_timer_handler() to process LVGL timers.
 * Runs on Core 1 (isolated from audio processing on Core 0).
 * Prevents lv_timer_handler() from returning 0xFFFFFFFF when no timers are scheduled.
 * 
 * @param parameters Task parameters (unused)
 */
void timer_update_task(void *parameters)
{
    (void)parameters;  /* Suppress unused parameter warning */
    
    printf("Timer update task started on Core %d\n", get_core_num());
    fflush(stdout);
    
    /* Main task loop: periodically update LVGL timers */
    while (1) {
        /* Process LVGL timers */
        uint32_t delay_ms = lv_timer_handler();
        
        /* If no timers ready (0xFFFFFFFF), use a safe default delay */
        if (delay_ms == 0xFFFFFFFF) {
            delay_ms = 10;
        }
        
        /* Ensure minimum delay to prevent busy-loop */
        if (delay_ms == 0) {
            delay_ms = 1;
        }
        
        /* Sleep for the recommended delay */
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
