#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pico/time.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "parser.h"
#include "widgets.h"
#include "network.h"
#include "tcp_server.h"
#include "waterfall.h"
#include "microphone.h"
#include "spectrogram.h"
#include "st7789.h"

/* External globals */
extern SemaphoreHandle_t g_LvglMutex;
extern uint32_t g_blinkRateMs;
extern uint8_t g_micDebug;

/* ============== Waterfall Mode Management ============== */

/** Global waterfall mode - controls whether audio task feeds FFT data to waterfall */
static waterfall_mode_t g_waterfallMode = WATERFALL_MODE_OFF;

/**
 * Get current waterfall display mode
 * @return Current mode (OFF, TEST, or LIVE_AUDIO)
 */
waterfall_mode_t waterfall_get_mode(void) {
    return g_waterfallMode;
}

/**
 * Set waterfall display mode
 * @param mode New mode (OFF, TEST, or LIVE_AUDIO)
 * 
 * When switching to TEST mode, audio task will stop feeding FFT data.
 * When switching to LIVE_AUDIO mode, audio task will resume feeding FFT data.
 */
void waterfall_set_mode(waterfall_mode_t mode) {
    g_waterfallMode = mode;
}

/**
 * Check if audio task should feed FFT data to waterfall
 * Called by audio processing pipeline before calling waterfall_accm_add_fft()
 * @return 1 if audio should feed waterfall, 0 otherwise
 */
int waterfall_should_feed_fft(void) {
    return (g_waterfallMode == WATERFALL_MODE_LIVE_AUDIO) ? 1 : 0;
}

/**
 * Token function for the "micDebug" command
 * Gets or sets the microphone debug level
 * 
 * @param rest Remainder of the command string
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnMicDebug(char *rest, void *v) {
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // If no arguments, return current value
    if (*rest == '\0') {
        printf("Current mic debug level: %d\n", g_micDebug);
        return 0;
    }
    
    // Parse the debug level value
    uint32_t debug_level;
    if (sscanf(rest, "%lu", &debug_level) != 1) {
        printf("Error: micDebug command requires a numeric value\n");
        return 1;
    }
    
    // Set the debug level
    g_micDebug = (uint8_t)debug_level;
    printf("Microphone debug level set to: %d\n", g_micDebug);
    return 0;
}

/* Forward declaration of fnHelp - implemented after aTokens definition */
static uint8_t fnHelp(char *rest, void *v);

/**
 * Token function for the "blink" command
 * Gets or sets the blink rate
 * 
 * @param rest Remainder of the command string
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnBlink(char *rest, void *v) {
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // If no arguments, return current value
    if (*rest == '\0') {
        printf("Current blink rate: %lu ms\n", g_blinkRateMs);
        return 0;
    }
    
    // Parse the frequency value from the remainder of the command
    uint32_t frequency;
    if (sscanf(rest, "%lu", &frequency) == 1) {
        // Validate frequency (100ms to 10000ms)
        if (frequency >= 100 && frequency <= 10000) {
            g_blinkRateMs = frequency;
            printf("LED blink rate changed to %lu ms\n", frequency);
            return 0;
        } else {
            printf("Invalid frequency: %lu ms. Must be between 100 and 10000 ms\n", frequency);
            return 1;
        }
    } else {
        printf("Error: blink command requires a frequency in milliseconds (100-10000)\n");
        return 1;
    }
}

/**
 * Token function for the "startClock" command
 * Creates and displays the clock widget
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success
 */
static uint8_t fnStartClock(char *rest, void *v) {
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        create_clock_widget();
        xSemaphoreGive(g_LvglMutex);
        printf("Clock widget started\n");
    } else {
        printf("Failed to acquire LVGL mutex\n");
        return 1;
    }
    return 0;
}

/**
 * Token function for the "stopClock" command
 * Removes the clock widget and clears the screen
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success
 */
static uint8_t fnStopClock(char *rest, void *v) {
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        remove_clock_widget();
        clear_screen();
        xSemaphoreGive(g_LvglMutex);
        printf("Clock widget stopped\n");
    } else {
        printf("Failed to acquire LVGL mutex\n");
        return 1;
    }
    return 0;
}

/**
 * Token function for the "setTime" command
 * Sets the clock needle value
 * 
 * @param rest Remainder of the command string containing the value
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnSetTime(char *rest, void *v) {
    uint32_t needleValue;
    
    if (sscanf(rest, "%lu", &needleValue) != 1) {
        printf("Error: setTime command requires a value (0-59)\n");
        return 1;
    }
    
    needleValue = needleValue % 60;  // Ensure value is within 0-59
    
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set_clock_needle_value((int32_t)needleValue);
        xSemaphoreGive(g_LvglMutex);
        printf("Clock needle set to: %lu\n", needleValue);
    } else {
        printf("Failed to acquire LVGL mutex\n");
        return 1;
    }
    return 0;
}

/**
 * Token function for the "gainWaterfall" command
 * Gets or sets the waterfall display gain
 * Formats: 'gainWaterfall' (get), 'gainWaterfall 10' (set to 10)
 * 
 * @param rest Remainder of the command string (gain value or empty)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnGainWaterfall(char *rest, void *v) {
    (void)v;
    
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // If no arguments, return current value
    if (*rest == '\0') {
        printf("Current waterfall gain: %lu\n", waterfall_get_gain());
        return 0;
    }
    
    // Parse the gain value
    uint32_t gain_value;
    if (sscanf(rest, "%lu", &gain_value) != 1) {
        printf("Error: gainWaterfall requires a numeric value\n");
        return 1;
    }
    
    // Set the gain (setter validates and computes squared gain)
    waterfall_set_gain(gain_value);
    return 0;
}

/**
 * Token function for the "waterfallBandwidth" command
 * Gets or sets the maximum frequency to display in the waterfall
 * 
 * Formats: 'waterfallBandwidth' (get), 'waterfallBandwidth 600' (set to 600 Hz)
 * Valid range: 1 Hz to 24000 Hz (Nyquist for 48 kHz audio)
 * 
 * @param rest Remainder of the command string (bandwidth in Hz or empty)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnWaterfallBandwidth(char *rest, void *v) {
    (void)v;
    
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // If no arguments, return current value
    if (*rest == '\0') {
        printf("Current waterfall bandwidth: %lu Hz\n", getWaterfallBandwidth());
        return 0;
    }
    
    // Parse the bandwidth value
    uint32_t bandwidth_hz;
    if (sscanf(rest, "%lu", &bandwidth_hz) != 1) {
        printf("Error: waterfallBandwidth requires a numeric value (Hz)\n");
        return 1;
    }
    
    // Validate range
    if (bandwidth_hz < 1 || bandwidth_hz > 24000) {
        printf("Error: bandwidth must be between 1 Hz and 24000 Hz (Nyquist)\n");
        return 1;
    }
    
    // Set the bandwidth
    setWaterfallBandwidth(bandwidth_hz);
    printf("Waterfall bandwidth set to %lu Hz\n", bandwidth_hz);
    return 0;
}

/**
 * Token function for the "debugFFT" command
 * Enables/disables FFT debug output (logs bin values and power levels)
 * 
 * Formats: 'debugFFT' (get), 'debugFFT 0' (disable), 'debugFFT 1' (enable)
 * 
 * @param rest Remainder of the command string (0/1 or empty)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnDebugFFT(char *rest, void *v) {
    (void)v;
    
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // If no arguments, return current value
    if (*rest == '\0') {
        int enabled = getFFTDebug();
        printf("FFT debug: %s\n", enabled ? "ENABLED" : "DISABLED");
        return 0;
    }
    
    // Parse the enable value
    int enable_value;
    if (sscanf(rest, "%d", &enable_value) != 1) {
        printf("Error: debugFFT requires 0 (disable) or 1 (enable)\n");
        return 1;
    }
    
    // Set the debug flag
    setFFTDebug(enable_value);
    printf("FFT debug: %s\n", enable_value ? "ENABLED" : "DISABLED");
    return 0;
}

/**
 * Token function for the "colorWaterfall" command
 * Gets or sets the waterfall colormap by index
 * Available: 0=Jet (default), 1=Parula
 * 
 * Formats: 'colorWaterfall' (get), 'colorWaterfall 1' (set to 1)
 * Invalid indices default to Jet (0)
 * 
 * @param rest Remainder of the command string (colormap index or empty)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnColorWaterfall(char *rest, void *v) {
    (void)v;
    
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // If no arguments, return current colormap index
    if (*rest == '\0') {
        uint16_t index = getColorMap();
        const char *names[] = {"Jet", "Parula"};
        printf("Current colormap: %s (index %u)\n", names[index], index);
        return 0;
    }
    
    // Parse the colormap index
    uint32_t colormap_index;
    if (sscanf(rest, "%lu", &colormap_index) != 1) {
        printf("Error: colorWaterfall requires a numeric index\n");
        return 1;
    }
    
    // Set the colormap (setter wraps modulo to valid range)
    setColorMap((uint16_t)colormap_index);
    return 0;
}

/**
 * Token function for the "drawBar" command
 * Draws a test bar on the waterfall display with the specified color level
 * Scrolls display left by 1 pixel and writes new colored column
 * 
 * Formats: 'drawBar 0' (draw blue), 'drawBar 7' (draw yellow), 'drawBar 15' (draw pale yellow)
 * Valid range: 0-15 (0=low/cool colors, 15=high/warm colors)
 */
static uint8_t fnDrawBar(char *rest, void *v) {
    (void)v;
    
    // Switch to TEST mode to prevent audio FFT interference
    waterfall_set_mode(WATERFALL_MODE_TEST);
    
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // drawBar requires a color level argument
    if (*rest == '\0') {
        printf("Error: drawBar requires a color level (0-15)\n");
        printf("  0 = blue (lowest), 7 = yellow (mid), 15 = pale yellow (highest)\n");
        return 1;
    }
    
    // Parse the amplitude level (0-15)
    uint32_t amplitude_level;
    if (sscanf(rest, "%lu", &amplitude_level) != 1) {
        printf("Error: drawBar requires a numeric color level (0-15)\n");
        return 1;
    }
    
    // Bounds check: clamp to valid range (function also does this, but validate here for feedback)
    if (amplitude_level > 15) {
        printf("Warning: Color level %lu out of range, clamping to 15\n", amplitude_level);
        amplitude_level = 15;
    }
    
    // Draw the test bar on the waterfall display
    waterfall_draw_bar((uint8_t)amplitude_level);
    printf("Drew waterfall test bar with color level %lu\n", amplitude_level);
    
    return 0;
}

/**
 * Token function for the "testWaterfallColors" command
 * Displays all 16 colors from current colormap as vertical bars
 * Fills entire screen with gradient from color 0-15
 * Useful for verifying colormap and display hardware
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success
 */
static uint8_t fnTestWaterfallColors(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    // Switch to TEST mode to prevent audio FFT interference
    waterfall_set_mode(WATERFALL_MODE_TEST);
    
    printf("Testing waterfall colormap - displaying all 16 colors\n");
    
    /* Initialize waterfall display in portrait mode */
    waterfall_mode_init();
    
    /* Create test colormap index array - all indices from 0-15 repeated */
    uint16_t testIndices[24] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15,
        1, 2, 3, 4, 5, 6, 7, 8
    };
    
    /* Draw the colormap test bars (2 full cycles across screen) */
    for (uint16_t bar_idx = 0; bar_idx < 32; bar_idx++) {
        waterfall_draw_bar(bar_idx % 16);  /* Cycle through all 16 colors */
        sleep_ms(50);  /* Brief pause between bars */
    }
    
    printf("Color test complete. Press 'testWaterfallScroll' to test animation.\n");
    return 0;
}

/**
 * Token function for the "drawColorBars" command
 * Displays individual color bars: white, blue, green, red
 * Each color occupies 8 vertical bars across the full display width
 * Useful for verifying color accuracy of the display
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success
 */
static uint8_t fnDrawColorBars(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    // Switch to TEST mode to prevent audio FFT interference
    waterfall_set_mode(WATERFALL_MODE_TEST);
    
    printf("Drawing individual color bars: white | blue | green | red\n");
    
    /* Initialize waterfall display in portrait mode */
    waterfall_mode_init();
    
    /* Draw the four basic color bars */
    drawColorBars();
    
    printf("Color bars drawn: 8 white | 8 blue | 8 green | 8 red\n");
    return 0;
}

/**
 * Token function for the "testWaterfallScroll" command
 * Tests waterfall animation by continuously scrolling colored bars
 * Press Ctrl+C or send another command to stop
 * Alternates between Jet and Parula colormaps every 32 bars
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success (runs indefinitely)
 */
static uint8_t fnTestWaterfallScroll(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    // Switch to TEST mode to prevent audio FFT interference
    waterfall_set_mode(WATERFALL_MODE_TEST);
    
    printf("Testing waterfall scroll animation - Ctrl+C to stop\n");
    printf("Alternating between Jet (0) and Parula (1) colormaps every 32 bars\n");
    
    /* Initialize waterfall display in portrait mode */
    waterfall_mode_init();
    
    /* Run scrolling animation indefinitely */
    while (1) {
        /* Jet colormap: 32 bars with cycling colors */
        setColorMap(0);
        printf("Jet colormap - scrolling...\n");
        for (uint16_t cnt = 0; cnt < 32; cnt++) {
            /* Cycle through colormap (0-15) */
            waterfall_draw_bar(cnt % 16);
            sleep_ms(50);  /* 50ms per bar ≈ 20 bars/second */
        }
        
        /* Parula colormap: 32 bars with cycling colors */
        setColorMap(1);
        printf("Parula colormap - scrolling...\n");
        for (uint16_t cnt = 0; cnt < 32; cnt++) {
            /* Cycle through colormap (0-15) */
            waterfall_draw_bar(cnt % 16);
            sleep_ms(50);
        }
    }
    
    return 0;
}

/**
 * Token function for the "fillColor" command
 * Fills the entire waterfall screen with a single color from the colormap
 * Useful for testing individual colormap indices (0-15)
 * 
 * @param rest Remainder of the command string (color index 0-15)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnFillColor(char *rest, void *v) {
    (void)v;
    
    // Switch to TEST mode to prevent audio FFT interference
    waterfall_set_mode(WATERFALL_MODE_TEST);
    
    // Skip whitespace
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // fillColor requires a color index argument
    if (*rest == '\0') {
        printf("Error: fillColor requires a colormap index (0-15)\n");
        printf("  Usage: fillColor <index>\n");
        printf("  Example: fillColor 7 (middle of colormap)\n");
        return 1;
    }
    
    // Parse the color index (0-15)
    uint32_t color_index;
    if (sscanf(rest, "%lu", &color_index) != 1) {
        printf("Error: fillColor requires a numeric colormap index (0-15)\n");
        return 1;
    }
    
    // Bounds check
    if (color_index > 15) {
        printf("Warning: Color index %lu out of range, clamping to 15\n", color_index);
        color_index = 15;
    }
    
    // Initialize waterfall display
    waterfall_mode_init();
    
    // Fill entire screen (32 bars × 10 pixels wide = 320 pixels total width)
    printf("Filling screen with color index %lu...\n", color_index);
    for (uint16_t bar_count = 0; bar_count < 32; bar_count++) {
        waterfall_draw_bar((uint8_t)color_index);
    }
    
    printf("Screen filled with color index %lu\n", color_index);
    return 0;
}

/**
 * Token function for the "scanWifi" command
 * Initiates a WiFi network scan
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnScanWifi(char *rest, void *v) {
    (void)rest;
    (void)v;
    printf("Starting WiFi scan...\n");
    network_scan_start();
    return 0;
}

/**
 * Token function for the "wifiList" command
 * Prints the results of the last WiFi scan
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnWifiList(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (network_scan_is_active()) {
        printf("WiFi scan in progress...\n");
        return 0;
    }
    
    network_scan_print_results();
    return 0;
}

/**
 * Token function for the "wifiConnect" command
 * Attempts to connect to a WiFi network
 * 
 * Usage: wifiConnect SSID password
 * 
 * @param rest Remainder of the command string containing SSID and password
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnWifiConnect(char *rest, void *v) {
    (void)v;
    
    if (rest == NULL || *rest == '\0') {
        printf("Usage: wifiConnect <SSID> <password>\n");
        return 1;
    }
    
    // Parse SSID and password
    char ssid[33] = {0};
    char password[64] = {0};
    
    // Extract SSID (up to first space)
    char *space = strchr(rest, ' ');
    if (space == NULL) {
        printf("Usage: wifiConnect <SSID> <password>\n");
        return 1;
    }
    
    int ssid_len = space - rest;
    if (ssid_len > 32) ssid_len = 32;
    strncpy(ssid, rest, ssid_len);
    ssid[ssid_len] = '\0';
    
    // Extract password (after space)
    space++;  // Skip space
    while (*space == ' ') space++;  // Skip additional spaces
    strncpy(password, space, sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    
    // Trim trailing whitespace from password
    int pwd_len = strlen(password);
    while (pwd_len > 0 && (password[pwd_len-1] == ' ' || password[pwd_len-1] == '\n' || password[pwd_len-1] == '\r')) {
        password[--pwd_len] = '\0';
    }
    
    if (strlen(password) == 0) {
        printf("Usage: wifiConnect <SSID> <password>\n");
        return 1;
    }
    
    printf("Connecting to WiFi: %s\n", ssid);
    int result = network_connect(ssid, password);
    
    if (result == 0) {
        printf("Connection initiated. Check status with: wifiStatus\n");
    } else {
        printf("Failed to initiate connection (error code: %d)\n", result);
    }
    
    return (result == 0) ? 0 : 1;
}

/**
 * Token function for the "wifiStatus" command
 * Checks WiFi connection status
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnWifiStatus(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    int status = network_is_connected();
    if (status == 1) {
        printf("WiFi Status: CONNECTED\n");
    } else if (status == 0) {
        printf("WiFi Status: NOT CONNECTED\n");
    } else {
        printf("WiFi Status: ERROR (%d)\n", status);
    }
    
    return 0;
}

/**
 * Token function for the "wifiRssi" command
 * Gets the current WiFi signal strength (RSSI) in dBm
 *
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnWifiRssi(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    int rssi = network_get_rssi();
    if (rssi == -999) {
        printf("WiFi Signal: NOT CONNECTED or UNAVAILABLE\n");
    } else {
        printf("WiFi Signal Strength (RSSI): %d dBm\n", rssi);
    }
    
    return 0;
}

/**
 * Token function for the "rtosStatus" command
 * Displays stack usage information for all running FreeRTOS tasks
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnRtosStatus(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    printf("\n=== FreeRTOS Task Status ===\n");
    printf("System Tick Count: %u\n\n", xTaskGetTickCount());
    
    // Get number of tasks
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        printf("No tasks found\n\n");
        return 0;
    }
    
    // Allocate array for task status structures
    TaskStatus_t *task_status_array = (TaskStatus_t *)malloc(task_count * sizeof(TaskStatus_t));
    if (task_status_array == NULL) {
        printf("ERROR: Failed to allocate memory for task status array\n");
        return 1;
    }
    
    // Get task information from kernel
    // The total runtime is returned only if configGENERATE_RUN_TIME_STATS is enabled
    uint32_t total_runtime = 0;
    UBaseType_t num_tasks = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);
    
    // Print header
    printf("Task Name               State       HWM (words)  Priority  Ticks\n");
    printf("---------------------------------------------------------------------\n");
    
    // Iterate through each task and print information
    for (UBaseType_t i = 0; i < num_tasks; i++) {
        TaskStatus_t *task = &task_status_array[i];
        
        // Convert state to string
        const char *state_str = "Unknown";
        switch (task->eCurrentState) {
            case eRunning:
                state_str = "Running";
                break;
            case eReady:
                state_str = "Ready";
                break;
            case eBlocked:
                state_str = "Blocked";
                break;
            case eSuspended:
                state_str = "Suspended";
                break;
            case eDeleted:
                state_str = "Deleted";
                break;
            case eInvalid:
            default:
                state_str = "Invalid";
                break;
        }
        
        // Print task information in columns
        printf("%-23s %-11s %11u  %8u  %5u\n",
               task->pcTaskName ? task->pcTaskName : "Unknown",
               state_str,
               task->usStackHighWaterMark,
               (unsigned)task->uxCurrentPriority,
               (unsigned)task->ulRunTimeCounter);
    }
    
    printf("\n");
    printf("Total tasks: %u\n", num_tasks);
    printf("Stack HWM in words. Multiply by %zu for bytes.\n", sizeof(StackType_t));
    printf("\n");
    
    /* Display queue status */
    printf("=== Audio Queue Status ===\n");
    printf("Waterfall Queue Depth: %u/4\n", waterfall_get_queue_depth());
    printf("\n");
    
    free(task_status_array);
    return 0;
}

/**
 * Token function for the "udpStart" command
 * Starts the UDP server (sends audio frames to UDP clients on port 5001)
 */
static uint8_t fnUdpStart(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (udp_server_is_running()) {
        printf("UDP Server: Already running\n");
        return 0;
    }
    
    int result = udp_server_start();
    if (result == 0) {
        printf("UDP Server: Started (send UDP packet to port 5001 to register)\n");
    } else {
        printf("UDP Server: Failed to start\n");
    }
    return result;
}

/**
 * Token function for the "udpStop" command
 * Stops the UDP server
 */
static uint8_t fnUdpStop(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (!udp_server_is_running()) {
        printf("UDP Server: Not running\n");
        return 0;
    }
    
    udp_server_stop();
    printf("UDP Server: Stopped\n");
    return 0;
}

/* ============== Waterfall Accumulator (Global for Audio Task Access) ============== */
/**
 * Global waterfall accumulator - used by audio processing task when in LIVE_AUDIO mode
 * Maintained here to coordinate between parser and audio pipeline
 */
static waterfall_accm_t g_waterfallAccumulator;

/**
 * Get pointer to global waterfall accumulator (for audio task)
 * Called by audio processing pipeline to feed FFT data
 * @return Pointer to waterfall accumulator (or NULL if not in LIVE_AUDIO mode)
 */
void* waterfall_get_accumulator(void) {
    if (waterfall_get_mode() == WATERFALL_MODE_LIVE_AUDIO) {
        return (void*)&g_waterfallAccumulator;
    }
    return NULL;  // Not in LIVE_AUDIO mode
}

/**
 * Token function for the "enableWaterfall" command
 * Switches to LIVE_AUDIO mode and initializes the accumulator
 * The audio processing task will begin feeding FFT data to the waterfall
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnEnableWaterfall(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("Failed to acquire display mutex\n");
        return 1;
    }
    
    printf("Enabling waterfall display - initializing for live audio...\n");
    
    /* Initialize display hardware */
    waterfall_mode_init();
    
    /* Initialize accumulator for FFT data */
    waterfall_accm_init(&g_waterfallAccumulator);
    
    /* Switch to LIVE_AUDIO mode - audio task will now feed FFT data */
    waterfall_set_mode(WATERFALL_MODE_LIVE_AUDIO);
    
    printf("Waterfall display enabled and ready for live audio (100 FFT frames per bar)\n");
    printf("NOTE: Audio processing task must be calling waterfall_accm_add_fft() for bars to appear\n");
    
    xSemaphoreGive(g_LvglMutex);
    return 0;
}

/**
 * Token function for the "disableWaterfall" command
 * Switches to OFF mode, stopping the audio task from feeding FFT data
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnDisableWaterfall(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("Failed to acquire display mutex\n");
        return 1;
    }
    
    printf("Disabling waterfall display - audio task will stop feeding FFT data\n");
    waterfall_set_mode(WATERFALL_MODE_OFF);
    
    xSemaphoreGive(g_LvglMutex);
    return 0;
}

/**
 * Token function for the "waterfallMode" command
 * Gets or sets the waterfall display mode
 * 
 * Usage:
 *  waterfallMode       - Get current mode
 *  waterfallMode 0     - OFF mode (no FFT feeding, no test mode)
 *  waterfallMode 1     - TEST mode (exclusive display access for test functions)
 *  waterfallMode 2     - LIVE_AUDIO mode (audio task feeds FFT data)
 * 
 * @param rest Remainder of the command string
 * @param v Void pointer for context (unused)
 * @return 0 on success, non-zero on failure
 */
static uint8_t fnWaterfallMode(char *rest, void *v) {
    (void)v;
    
    while (*rest && isspace(*rest)) {
        rest++;
    }
    
    // If no arguments, return current mode
    if (*rest == '\0') {
        const char *modeNames[] = {"OFF", "TEST", "LIVE_AUDIO"};
        waterfall_mode_t mode = waterfall_get_mode();
        printf("Current waterfall mode: %s (%d)\n", modeNames[mode], mode);
        printf("  0=OFF (disabled),  1=TEST (test functions),  2=LIVE_AUDIO (audio FFT)\n");
        return 0;
    }
    
    // Parse the mode value
    uint32_t mode_value;
    if (sscanf(rest, "%lu", &mode_value) != 1) {
        printf("Error: waterfallMode requires 0 (OFF), 1 (TEST), or 2 (LIVE_AUDIO)\n");
        return 1;
    }
    
    if (mode_value > 2) {
        printf("Error: Invalid mode %lu. Valid modes: 0=OFF, 1=TEST, 2=LIVE_AUDIO\n", mode_value);
        return 1;
    }
    
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("Failed to acquire display mutex\n");
        return 1;
    }
    
    waterfall_mode_t new_mode = (waterfall_mode_t)mode_value;
    waterfall_set_mode(new_mode);
    
    const char *modeNames[] = {"OFF", "TEST", "LIVE_AUDIO"};
    printf("Waterfall mode set to: %s\n", modeNames[new_mode]);
    
    xSemaphoreGive(g_LvglMutex);
    return 0;
}

/**
 * Static array of available tokens
 * Terminated with a null entry to mark the end of the array
 */
static const struct s_tokens aTokens[] = {
    {"help",        "Display available commands",           fnHelp},
    {"blink",       "Get/set LED blink rate (ms)",          fnBlink},
    {"micDebug",    "Get/set microphone debug level",       fnMicDebug},
    {"startClock",  "Start and display clock widget",       fnStartClock},
    {"stopClock",   "Stop and remove clock widget",         fnStopClock},
    {"setTime",     "Set clock needle value (0-59)",        fnSetTime},
    {"scanWifi",    "Start WiFi network scan",              fnScanWifi},
    {"wifiList",    "List WiFi scan results",               fnWifiList},
    {"wifiConnect", "Connect to WiFi (usage: wifiConnect SSID pass)", fnWifiConnect},
    {"wifiStatus",  "Check WiFi connection status",         fnWifiStatus},
    {"wifiRssi",    "Get current WiFi signal strength (dBm)",  fnWifiRssi},
    {"rtosStatus",  "Display FreeRTOS task stack usage",    fnRtosStatus},
    {"udpStart",      "Start UDP server (audio on port 5001)",  fnUdpStart},
    {"udpStop",       "Stop UDP server",                      fnUdpStop},
    {"enableWaterfall","Enable waterfall display",           fnEnableWaterfall},
    {"disableWaterfall","Disable waterfall display",         fnDisableWaterfall},
    {"waterfallMode", "Get/set waterfall mode (0=OFF, 1=TEST, 2=LIVE_AUDIO)", fnWaterfallMode},
    {"gainWaterfall", "Get/set waterfall gain (1-N)",        fnGainWaterfall},
    {"waterfallBandwidth", "Get/set max frequency (Hz)",     fnWaterfallBandwidth},
    {"debugFFT",       "Get/set FFT debug output (0=off, 1=on)", fnDebugFFT},
    {"colorWaterfall", "Get/set waterfall colormap (0=Jet, 1=Parula)", fnColorWaterfall},
    {"drawBar",       "Draw test bar (0-15 color level)",    fnDrawBar},
    {"fillColor",     "Fill screen with color (0-15 index)", fnFillColor},
    {"testWaterfallColors", "Test colormap display (all colors)", fnTestWaterfallColors},
    {"drawColorBars", "Draw RGB color bars (white|blue|green|red)", fnDrawColorBars},
    {"testWaterfallScroll", "Test scroll animation (infinite)", fnTestWaterfallScroll},
    {NULL,            NULL,                                    NULL}
};

/**
 * Token function for the "help" command
 * Displays help information by iterating over aTokens array
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused in this function)
 * @return 0 on success
 */
static uint8_t fnHelp(char *rest, void *v) {
    (void)rest;  /* Unused */
    (void)v;     /* Unused */
    
    printf("\n");
    printf("%-16s %s\n", "Token", "Function");
    printf("%-16s %s\n", "-----", "--------");
    
    /* Iterate over aTokens array until NULL terminator */
    for (int i = 0; aTokens[i].tokenText != NULL; i++) {
        printf("%-16s %s\n", aTokens[i].tokenText, aTokens[i].helpText);
    }
    
    printf("\n");
    return 0;
}

/**
 * Parse a command string and execute the corresponding token function
 * Extracts the first non-whitespace token from the input buffer and
 * searches for a matching entry in aTokens. If found, calls the associated
 * function with the remainder of the string.
 * 
 * @param buffer The input command string to parse
 * @param v Void pointer to pass context to token functions
 * @return 0 if token was found and executed, non-zero otherwise
 */
uint8_t parse(char *buffer, void *v) {
    char token[64];
    int token_idx = 0;
    int buffer_idx = 0;
    
    if (buffer == NULL) {
        return 1;
    }
    
    // Extract the first non-whitespace token
    while (buffer[buffer_idx] != '\0' && isspace(buffer[buffer_idx])) {
        buffer_idx++;  // Skip leading whitespace
    }
    
    // Copy token characters until whitespace or end of string
    while (buffer[buffer_idx] != '\0' && !isspace(buffer[buffer_idx]) && token_idx < sizeof(token) - 1) {
        token[token_idx++] = buffer[buffer_idx++];
    }
    token[token_idx] = '\0';  // Null-terminate the token
    
    // Skip whitespace between token and arguments
    while (buffer[buffer_idx] != '\0' && isspace(buffer[buffer_idx])) {
        buffer_idx++;
    }
    
    // If no token was found, return error
    if (token_idx == 0) {
        return 1;
    }
    
    // Search for matching token in aTokens array
    for (int i = 0; aTokens[i].tokenText != NULL; i++) {
        if (strcmp(token, aTokens[i].tokenText) == 0) {
            // Token found - call the associated function
            return aTokens[i].fn(&buffer[buffer_idx], v);
        }
    }
    
    // Token not found
    printf("Unknown command '%s'. Type 'help' for available commands.\n", token);
    return 1;
}
