#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "parser.h"
#include "widgets.h"
#include "network.h"
#include "tcp_server.h"

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
    
    free(task_status_array);
    return 0;
}

/**
 * Token function for the "tcpStart" command
 * Starts the TCP server (sends "Hello X" to telnet clients on port 5000)
 */
static uint8_t fnTcpStart(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (tcp_server_is_running()) {
        printf("TCP Server: Already running\n");
        return 0;
    }
    
    int result = tcp_server_start();
    if (result == 0) {
        printf("TCP Server: Started (connect via: telnet <IP> 5000)\n");
    } else {
        printf("TCP Server: Failed to start\n");
    }
    return result;
}

/**
 * Token function for the "tcpStop" command
 * Stops the TCP server
 */
static uint8_t fnTcpStop(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (!tcp_server_is_running()) {
        printf("TCP Server: Not running\n");
        return 0;
    }
    
    tcp_server_stop();
    printf("TCP Server: Stopped\n");
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
    {"rtosStatus",  "Display FreeRTOS task stack usage",    fnRtosStatus},
    {"tcpStart",    "Start TCP server (telnet port 5000)",  fnTcpStart},
    {"tcpStop",     "Stop TCP server",                      fnTcpStop},
    {NULL,          NULL,                                    NULL}
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
