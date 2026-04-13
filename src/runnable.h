#ifndef RUNNABLE_H
#define RUNNABLE_H

#include <FreeRTOS.h>
#include <task.h>
#include <stdio.h>

/**
 * Runnable Component Architecture
 * 
 * Type definition and utilities for module-based initialization pattern.
 * Supports null-terminated registry, error codes, and assertions.
 */

/**
 * Error codes returned by initialization functions
 */
#define INIT_SUCCESS      0   // Initialization successful
#define INIT_ERR_QUEUE    1   // Queue creation failed
#define INIT_ERR_SEMAPHORE 5  // Semaphore creation failed
#define INIT_ERR_MALLOC   2   // Memory allocation failed
#define INIT_ERR_HW       3   // Hardware initialization failed
#define INIT_ERR_CONFIG   4   // Configuration error

/**
 * FreeRTOS Core Affinity Masks (RP2350 dual-core)
 * 
 * CORE_NONE: Task floats across cores (uses xTaskCreate)
 * CORE_0/CORE_1: Task pinned to specific core (uses xTaskCreateAffinitySet)
 */
#define CORE_NONE  (-1)   // Floating - no core affinity
#define CORE_0     (1 << 0)  // Core 0 (ARM Cortex-M33 primary)
#define CORE_1     (1 << 1)  // Core 1 (ARM Cortex-M33 secondary)

/** * Assert macro for initialization errors
 * 
 * Prints error code to UART, flushes, and halts.
 * Used in main() after calling init functions.
 * 
 * Usage: INIT_ASSERT(ret, "module_name");
 */
#define INIT_ASSERT(ret, module) \
    do { \
        if ((ret) != INIT_SUCCESS) { \
            printf("ERROR: %s initialization failed (code %d)\n", (module), (ret)); \
            fflush(stdout); \
            while(1);  /* Halt */ \
        } \
    } while(0)

/**
 * Runnable component structure
 * 
 * Defines a module's initialization and task entry point.
 * Assembled into null-terminated registry in main.c.
 * 
 * Fields:
 *   name           - Task name for debugging (passed to xTaskCreateAffinitySet)
 *   stack_size     - Stack size in words for FreeRTOS task
 *   priority       - Task priority (1 to configMAX_PRIORITIES-1)
 *   affinity_mask  - Core affinity for SMP (e.g., 1<<0 for Core 0)
 *   initialize     - Phase 1: Module-specific initialization (before task create)
 *   run            - Phase 2: Task entry point (called by scheduler)
 *   pramInitialize - Optional context for initialize() function
 *   pramRunnable   - Optional context for run() function (typically module state)
 */
typedef struct {
    const char *name;
    uint32_t stack_size;
    UBaseType_t priority;
    UBaseType_t affinity_mask;
    
    int (*initialize)(const void *init_param, void *run_param);
    void (*run)(void *run_param);
    
    const void *pramInitialize;
    void *pramRunnable;
} runnable_t;

/**
 * Runnable Component Management Functions
 * 
 * Phase 1: Initialize all components (before task creation)
 * Phase 2: Create all FreeRTOS tasks with affinity
 * 
 * Should be called from main() in this order:
 *   1. runnable_initialize_all()
 *   2. runnable_create_all_tasks()
 *   3. vTaskStartScheduler()
 */
int runnable_initialize_all(void);
int runnable_create_all_tasks(void);

#endif /* RUNNABLE_H */
