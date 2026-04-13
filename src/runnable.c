#include "runnable.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* ============== Runnable Registry ============== */

/**
 * Forward declarations for runnable components
 * Each module exports its runnable descriptor
 */
extern const runnable_t runnable_parser;
extern const runnable_t runnable_microphone;
extern const runnable_t runnable_network;
extern const runnable_t runnable_waterfall;
extern const runnable_t runnable_udp_audio_server;
//extern const runnable_t runnable_timer_update_task;  // From widgets.c

/**
 * Null-terminated array of all runnable components
 * Defines initialization and task creation order: Microphone → Network → Waterfall
 * Parser handled separately with direct xTaskCreateAffinitySet in main.c
 * 
 * DISABLED (April 13, 2026 - network performance debugging):
 * - runnable_parser: TTY command interface (not needed for performance baseline)
 * - runnable_waterfall: FFT + display rendering (Core 1 load causing scheduler contention)
 */
static const runnable_t *const aRunnable[] = {
    &runnable_parser,                 // ENABLED (April 13, 2026 - diagnostic access)
    &runnable_microphone,
    &runnable_network,
    &runnable_waterfall,              // ENABLED: Core 0 pinning (SMP optimization)
    &runnable_udp_audio_server,
    // &runnable_timer_update_task,
    NULL  /* Sentinel - marks end of array */
};

/* ============== Runnable Initialization and Task Creation ============== */

/**
 * Initialize all runnable components
 * 
 * Phase 1: Calls initialize() function for each runnable
 * Must complete successfully before task creation phase
 * 
 * @return INIT_SUCCESS on success, error code on failure
 */
int runnable_initialize_all(void) {
    printf("[RUNNABLE] ===== Phase 1: Component Initialization =====\n");
    fflush(stdout);
    
    for (size_t i = 0; aRunnable[i] != NULL; i++) {
        const runnable_t *r = aRunnable[i];
        
        if (r->initialize == NULL) {
            printf("[RUNNABLE] - %s (no init required)\n", r->name);
            continue;
        }
        
        int ret = r->initialize(r->pramInitialize, (void *)&r->pramRunnable);
        if (ret != INIT_SUCCESS) {
            printf("[FATAL] %s initialization failed (code %d)\n", r->name, ret);
            return ret;
        }
        printf("[RUNNABLE] ✓ %s initialized\n", r->name);
    }
    
    printf("[RUNNABLE] ✓ All components initialized\n\n");
    fflush(stdout);
    return INIT_SUCCESS;
}

/**
 * Create all FreeRTOS tasks from runnable components
 * 
 * Phase 2: Creates tasks using xTaskCreate (if CORE_NONE) or xTaskCreateAffinitySet (if pinned)
 * All component state initialized by Phase 1
 * 
 * @return INIT_SUCCESS on success, error code on failure
 */
int runnable_create_all_tasks(void) {
    printf("[RUNNABLE] ===== Phase 2: Task Creation =====\n");
    fflush(stdout);
    
    for (size_t i = 0; aRunnable[i] != NULL; i++) {
        const runnable_t *r = aRunnable[i];
        
        if (r->run == NULL) {
            printf("[RUNNABLE] - %s (no task required)\n", r->name);
            continue;
        }
        
        TaskHandle_t handle = NULL;
        BaseType_t result;
        
        /* Single-core mode: all tasks use xTaskCreate regardless of affinity specification */
        result = xTaskCreate(
            r->run,
            r->name,
            r->stack_size,
            (void *)r->pramRunnable,  /* Pass component context to task */
            r->priority,
            &handle
        );
        
        if (result != pdPASS) {
            printf("[FATAL] Failed to create task: %s\n", r->name);
            return INIT_ERR_HW;
        }
        printf("[RUNNABLE] ✓ %s task created (priority=%d, single-core)\n",
               r->name, r->priority);
    }
    
    printf("[RUNNABLE] ✓ All tasks created\n\n");
    fflush(stdout);
    return INIT_SUCCESS;
}
