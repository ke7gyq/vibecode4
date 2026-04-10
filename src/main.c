#include <pico/stdlib.h>
#include <pico/time.h>
#include <pico/status_led.h>
#include <hardware/uart.h>
#include <hardware/regs/uart.h>
#include <stdio.h>
#include <lvgl.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "st7789.h"
#include "parser.h"
#include "widgets.h"
#include "microphone.h"
#include "waterfall.h"
#include "network.h"
#include "tcp_server.h"
#if 0
#include "waterfall.h"
#endif
#include "spectrogram.h"



/**
 * Global variables
 */
/* Character input queue - populated by UART RX interrupt */
static QueueHandle_t g_CharQueue = NULL;

/* LVGL display handle */
static lv_display_t *display;

/* Spectrogram processor context for FFT and frequency analysis */
spectrogram_t g_spectrogram;

/* FreeRTOS synchronization */
SemaphoreHandle_t g_LvglMutex;

/* LED blink rate in milliseconds */
uint32_t g_blinkRateMs = 1000;

/* High-frequency timer counter for FreeRTOS runtime statistics */
/* Incremented by configTICK_HOOK_FUNCTION for per-tick precision */
volatile uint32_t ulHighFrequencyTimerTicks = 0;

/**
 * FreeRTOS Stack Overflow Hook - called when a task overflows its stack
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    printf("ERROR: Stack overflow in task %s\n", pcTaskName);
    while (1) {
        sleep_ms(100);
    }
}

/**
 * FreeRTOS Malloc Failed Hook - called when malloc fails
 */
void vApplicationMallocFailedHook(void) {
    printf("ERROR: Memory allocation failed\n");
    while (1) {
        sleep_ms(100);
    }
}

/**
 * FreeRTOS Tick Hook - called every system tick
 * Used to increment the high-frequency timer counter for runtime statistics
 */
void vApplicationTickHook(void) {
    ulHighFrequencyTimerTicks++;
}


/**
 * LVGL tick callback - returns current time in milliseconds
 */
static uint32_t lvgl_tick_cb(void){
    uint32_t curTime = to_ms_since_boot(get_absolute_time());
    return curTime;
}

/**
 * Timer Task - handles LVGL timers and rendering
 * Event-driven: sleeps when idle, wakes when timers are scheduled
 * Timers are created on-demand by parser/widgets, not continuously running
 */
#if 0
static uint32_t last_tick_time = 0;

static void timer_task(void *parameters)
{
    int32_t delay_ms ;
    printf("Timer Task Started\n");
  
    // Set LVGL tick callback for time tracking
    //lv_tick_set_cb(lvgl_tick_cb);

    // Initialize LVGL
    display = initialize_lvgl();
    printf("After initialize_lvgl: display = %p\n", (void*)display);
    
    printf("Setting tick callback...\n");
    lv_tick_set_cb(lvgl_tick_cb);

    if (!display) {
        printf("Failed to initialize LVGL\n");
        vTaskDelete(NULL);
        return;
    }
    printf("LVGL initialized\n");
    fflush(stdout);

    printf("About to create initial screen...\n");
    fflush(stdout);
    
    // Create initial screen with widgets
    create_initial_screen();
    
    printf("Initial screen created\n");
    fflush(stdout);

    // Force an invalidation of the screen to trigger display refresh
    printf("Invalidating screen...\n");
    fflush(stdout);
    
    lv_obj_invalidate(lv_screen_active());
    
    printf("Forcing immediate refresh...\n");
    fflush(stdout);
    
    // Try to force a refresh immediately
    lv_refr_now(NULL);
    
    printf("Screen setup complete\n");
    fflush(stdout);


    if(g_LvglMutex == NULL) {
        printf("LVGL mutex not initialized\n");
        vTaskDelete(NULL);
        return;
    }


    
    // Main timer loop - event-driven
    while (1) {
        delay_ms = 200; // Default: long sleep when idle (no timers)
        if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Calculate actual time delta and update LVGL's tick
            uint32_t current_time = to_ms_since_boot(get_absolute_time());
            if (last_tick_time == 0) {
                last_tick_time = current_time;
            }
            uint32_t time_delta = current_time - last_tick_time;
            if (time_delta > 0) {
                lv_tick_inc(time_delta);
                last_tick_time = current_time;
            }
            
            // Handle LVGL timers and tasks
            int32_t handler_result = (int32_t)lv_timer_handler();
            if (handler_result == -1) {
                // No timers scheduled - sleep longer to save power
                delay_ms = 200;  
            } else if (handler_result == 1) {
                // Handler already running or system disabled
                delay_ms = 10;  
            } else if (handler_result > 0) {
                // Active timers scheduled - wake up at next timer interval
                delay_ms = handler_result;
                if (delay_ms > 100) {
                    delay_ms = 100;  // Cap to 100ms max
                }
            }
            xSemaphoreGive(g_LvglMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * Blinker Task - blinks the LED at configurable rate
 */
static void blinker_task(void *parameters)
{
    printf("Blinker Task Started\n");
    // status_led_init();
    int state = false;
    while(1){
        status_led_set_state(state);
        vTaskDelay(pdMS_TO_TICKS(g_blinkRateMs / 2));  // On time
        state = !state;
    }
}
#endif  // #ifdef(0) - Timer and Blinker tasks disabled

/**
 * UART RX Callback - called from interrupt context
 * Reads all available characters from UART FIFO and feeds them into the character queue
 */
static void uart_rx_callback(void) {
    /* Read all available characters from UART RX FIFO */
    while (uart_is_readable(uart0)) {
        int ch = uart_getc(uart0);  /* Read directly from FIFO (non-blocking) */
        if (ch != PICO_ERROR_TIMEOUT) {
            /* Send character to queue from ISR context */
            xQueueSendFromISR(g_CharQueue, &ch, NULL);
        }
    }
}

/**
 * USB Command Task - reads commands from USB and parses them
 * Blocks on character queue - yields CPU when no input available
 */
static void parser_task(void *parameters)
{
    char buffer[64];
    int buffer_idx = 0;
    int ch;
    
    printf("Parser Task Started\n");
    printf("Type 'help' for available commands\n");
    printf("> ");
    fflush(stdout);
    
    while (1) {
        // Block indefinitely until a character arrives (yields CPU to other tasks)
        // Uses interrupt-driven input via UART RX callback
        if (xQueueReceive(g_CharQueue, &ch, portMAX_DELAY) == pdTRUE) {
            // Character received from queue
            if (ch == '\n' || ch == '\r') {
                // End of line - process command
                if (buffer_idx > 0) {
                    buffer[buffer_idx] = '\0';
                    
                    // Parse and execute command
                    parse(buffer, NULL);
                }
                // Reset buffer for next command
                buffer_idx = 0;
                printf("> ");
                fflush(stdout);
            } else if (ch == '\b' || ch == 127) {
                // Backspace handling
                if (buffer_idx > 0) {
                    buffer_idx--;
                    printf("\b \b");
                    fflush(stdout);
                }
            } else if (ch >= 32 && ch < 127 && buffer_idx < sizeof(buffer) - 1) {
                // Printable character - add to buffer
                buffer[buffer_idx++] = (char)ch;
                printf("%c", ch);
                fflush(stdout);
            }
        }
    }
}



int main(void)
{
    // Initialize UART first before any printf calls

    stdio_init_all();
    sleep_ms(100);  // Give UART time to initialize
    st7789_lcd_init();

    printf("Vibecode4 - FreeRTOS with LVGL\n");
    printf("Initializing FreeRTOS...\n");
    
    // Create character input queue for UART RX
    // Populated by interrupt handler, consumed by parser_task
    g_CharQueue = xQueueCreate(64, sizeof(int));
    if (g_CharQueue == NULL) {
        printf("Failed to create character input queue\n");
        return 1;
    }
    printf("Character input queue created\n");
    
    // Setup UART RX interrupt to feed characters into queue
    // Enable RX interrupt in UART0 hardware (stdio doesn't do this by default)
    uart_set_irq_enables(uart0, true, false);  /* Enable RX IRQ, disable TX IRQ */
    irq_set_exclusive_handler(UART0_IRQ, uart_rx_callback);
    irq_set_enabled(UART0_IRQ, true);
    printf("UART RX interrupt configured for interrupt-driven input\n");
    
    wifi_init();

    // **CRITICAL**: PDM microphone GPIO handled by PIO (SM2)
    // GPIO6: Data input (from microphone) - configured by pdm_clock_program_init()
    // GPIO7: Clock output (to microphone) - configured by pdm_clock_program_init()
    // DO NOT initialize these with regular GPIO API - PIO needs exclusive control!
    printf("PDM microphone GPIO (6=data, 7=clock) will be configured by PIO\n");
    
    // Initialize integrated PDM clock generator + data reader (SM2)
    initialize_pdm_clock();
    
    
    // Create FreeRTOS tasks
    BaseType_t result;
    TaskHandle_t timer_handle = NULL;
    TaskHandle_t blinker_handle = NULL;
    TaskHandle_t parser_handle = NULL;
    
    // Create binary semaphore (mutex) for LVGL protection
    g_LvglMutex = xSemaphoreCreateMutex();
    if (g_LvglMutex == NULL) {
        printf("Failed to create LVGL mutex\n");
    }

    /* Initialize spectrogram processor for FFT analysis */
    printf("Initializing spectrogram processor...\n");
    if (spectrogram_init(&g_spectrogram) != 0) {
        printf("ERROR: Failed to initialize spectrogram\n");
        return 1;
    }
    printf("Spectrogram initialized successfully\n");

    /* Initialize waterfall display on startup (independent of WiFi) */
    printf("Initializing waterfall display in LIVE_AUDIO mode...\n");
    fflush(stdout);
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        waterfall_mode_init();
        waterfall_set_mode(WATERFALL_MODE_LIVE_AUDIO);
        printf("Waterfall display initialized and ready (mode=LIVE_AUDIO)\n");
        xSemaphoreGive(g_LvglMutex);
    } else {
        printf("WARNING: Could not acquire display mutex for waterfall init\n");
    }
    fflush(stdout);

    /* Pre-create separate audio queues BEFORE tasks start to avoid race conditions */
    /* Two independent queues - each task has its own for independent flow control */
    printf("Initializing audio queues...\n");
    
    g_audioQueueUDP = xQueueCreate(4, sizeof(AudioBufferMessage_t));
    if (g_audioQueueUDP == NULL) {
        printf("ERROR: Failed to create UDP audio queue\n");
        return 1;
    }
    printf("UDP audio queue created (depth=4, msg_size=%u bytes)\n",
           (unsigned)sizeof(AudioBufferMessage_t));
    
    g_audioQueueWaterfall = xQueueCreate(4, sizeof(AudioBufferMessage_t));
    if (g_audioQueueWaterfall == NULL) {
        printf("ERROR: Failed to create Waterfall audio queue\n");
        return 1;
    }
    printf("Waterfall audio queue created (depth=4, msg_size=%u bytes)\n",
           (unsigned)sizeof(AudioBufferMessage_t));
    printf("  - Independent queues: UDP and Waterfall each drain at own rate\n");

    /* Pre-create audio ready semaphore BEFORE tasks start (microphone_init will skip if exists) */
    /* Used by audio_dsp_example.c and potentially other consumers */
    printf("Initializing audio ready semaphore...\n");
    g_audioReadySemaphore = xSemaphoreCreateBinary();
    if (g_audioReadySemaphore == NULL) {
        printf("ERROR: Failed to create audio ready semaphore\n");
        return 1;
    }
    printf("Audio ready semaphore created (binary, legacy compatibility)\n");

#if 0
    // Timer task - LVGL display management (SPI I/O heavy)
    // Pin to Core 1 to prevent SPI transfers from blocking audio on Core 0
    const UBaseType_t core_1_affinity_timer = (1 << 1);  /* Core 1 only */
    result = xTaskCreateAffinitySet(
        timer_task,           // Task function
        "TimerTask",          // Task name
        1536,                 // Stack size in words
        NULL,                 // Parameters
        2,                    // Priority: lower than microphone
        core_1_affinity_timer,// Core affinity: Core 1 (isolated display I/O)
        &timer_handle         // Task handle
    );
    if (result != pdPASS) {
        printf("Failed to create timer task\n");
        return 1;
    }
    printf("Timer task created on Core 1\n");
#endif  // #ifdef(0) - Timer task disabled

    // Blinker task - medium priority
    #if(0)
    result = xTaskCreate(
        blinker_task,
        "BlinkerTask",
        // 1024,
        256,
        NULL,
        2,
        &blinker_handle
    );
    if (result != pdPASS) {
        printf("Failed to create blinker task\n");
        return 1;
    }
    #endif  // #ifdef(0) - Blinker task disabled
    #if(1)
    // Parser task - lower priority, pinned to Core 0 for audio/display separation
    const UBaseType_t core_0_parser = (1 << 0);  /* Core 0 only */
    result = xTaskCreateAffinitySet(
        parser_task,
        "ParserTask",
        512,
        // 2048,
        NULL,
        2,  /* Priority: handle keyboard input */
        core_0_parser,  /* Core affinity: Core 0 (isolated from LVGL on Core 1) */
        &parser_handle
    );
    if (result != pdPASS) {
        printf("Failed to create parser task\n");
        return 1;
    }
    #endif

    #if(1)
    // Microphone task - captures PDM audio and converts to PCM
    // **HIGHEST PRIORITY** - Real-time audio capture cannot be delayed
    // Clock is already running continuously from initialize_pdm_clock()
    result = xTaskCreate(
        microphone_task,
        "MicrophoneTask",
        512,
        NULL,
        3,  /* Priority: highest - real-time audio capture */
        NULL
    );
    if (result != pdPASS) {
        printf("Failed to create microphone task\n");
        return 1;
    }
    #endif

    #if(1)
    // Network task - WiFi scanning and connection
    result = xTaskCreate(
        network_task,         // Task function
        "NetworkTask",        // Task name
        512,
        // 2048,                 // Stack size in words
        NULL,                 // Parameters
        2,                    // Priority (between timer/blinker and low tasks)
        NULL                  // Task handle (not needed)
    );
    if (result != pdPASS) {
        printf("Failed to create network task\n");
        return 1;
    }
    #endif

    #if(1)
    // UDP Audio Task - sends audio frames when timer notifies it
    // Stack: 2048 words = 8KB (large to prevent stack overflow with frame buffer)
    // Priority 2: Medium-high, below microphone (3) to avoid starving parser task
    // Core Affinity: Core 0 (separate from waterfall FFT on Core 1)
    const UBaseType_t core_0_affinity = (1 << 0);  /* Core 0 only */
    result = xTaskCreateAffinitySet(
        udp_audio_task,       // Task function
        "UdpAudioTask",       // Task name
        2048,                 // Stack size in words (8KB - much safer margin)
        NULL,                 // Parameters
        2,                    // Priority: medium-high (not higher than needed)
        core_0_affinity,      // Core affinity: Core 0
        NULL                  // Task handle (not needed)
    );
    if (result != pdPASS) {
        printf("Failed to create UDP audio task\n");
        return 1;
    }
    #endif

    /* Attach spectrogram processor to waterfall display */
#if 0
    printf("Attaching spectrogram to waterfall task...\n");
    fflush(stdout);
    waterfall_set_spectrogram(&g_spectrogram);
#endif  // #ifdef(0) - Waterfall disabled

    /* Create Waterfall Task (Core 1 - isolated display rendering) */
    const UBaseType_t core_1_waterfall = (1 << 1);  /* Core 1 only */
    result = xTaskCreateAffinitySet(
        waterfall_task,       // Task function
        "WaterfallTask",      // Task name
        8192,                 // Stack size in words (FFT + signal processing + printf buffers)
        NULL,                 // Parameters
        2,                    // Priority: medium (below microphone, same as display)
        core_1_waterfall,     // Core affinity: Core 1
        NULL                  // Task handle (not needed)
    );
    if (result != pdPASS) {
        printf("Failed to create waterfall task\n");
        return 1;
    }
    printf("Waterfall task created on Core 1\n");

    /* Create Timer update task on Core 1 (isolated from audio) */
    const UBaseType_t core_1_affinity = (1 << 1);  /* Core 1 only */
    result = xTaskCreateAffinitySet(
        timer_update_task,    // Task function
        "TimerUpdateTask",    // Task name
        512,                  // Stack size in words (2KB - small, simple task)
        NULL,                 // Parameters
        2,                    // Priority: medium-high (match waterfall task)
        core_1_affinity,      // Core affinity: Core 1
        NULL                  // Task handle (not needed)
    );
    if (result != pdPASS) {
        printf("Failed to create timer update task\n");
        return 1;
    }
    
    printf("Starting FreeRTOS scheduler...\n");
    fflush(stdout);
    vTaskStartScheduler();
    
    // Should never reach here
    printf("ERROR: FreeRTOS scheduler failed to start\n");
    return 1;
}
