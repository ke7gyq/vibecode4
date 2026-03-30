#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "parser.h"
#include "widgets.h"

/* External globals */
extern SemaphoreHandle_t g_LvglMutex;
extern uint32_t g_blinkRateMs;
extern uint8_t g_micDebug;

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

/**
 * Token function for the "help" command
 * Displays help information for available commands
 * 
 * @param rest Remainder of the command string (unused)
 * @param v Void pointer for context (unused in this function)
 * @return 0 on success
 */
static uint8_t fnHelp(char *rest, void *v) {
    printf("\nAvailable commands:\n");
    printf("  blink [ms]     - Get/set LED blink duration (no args shows current value)\n");
    printf("  micDebug [val] - Get/set microphone debug level (0=off, 2+=on)\n");
    printf("  startClock     - Start and display clock widget\n");
    printf("  stopClock      - Stop and remove clock widget\n");
    printf("  setTime <val>  - Set clock needle value (0-59)\n");
    printf("  help           - Display this help message\n\n");
    return 0;
}

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
 * Static array of available tokens
 * Terminated with a null entry to mark the end of the array
 */
static const struct s_tokens aTokens[] = {
    {"help",       "Display available commands",           fnHelp},
    {"blink",      "Get/set LED blink rate (ms)",          fnBlink},
    {"micDebug",   "Get/set microphone debug level",       fnMicDebug},
    {"startClock", "Start and display clock widget",       fnStartClock},
    {"stopClock",  "Stop and remove clock widget",         fnStopClock},
    {"setTime",    "Set clock needle value (0-59)",        fnSetTime},
    {NULL,         NULL,                                    NULL}
};

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
