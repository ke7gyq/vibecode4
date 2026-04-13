# Runnable Component Architecture - Refactoring Template

## Overview
This document provides a step-by-step template for converting existing FreeRTOS tasks into the new runnable component architecture. Each module follows the same pattern: extract state → split init/run → declare runnable → test.

## Pattern Summary

### Before (Traditional FreeRTOS)
```c
// Global state scattered across module
static SomeState_t g_state = {...};
static QueueHandle_t g_taskQueue = NULL;

void task_name(void *parameters) {
    // Initialization code here
    // ...setup...
    
    while (1) {
        // Work loop
        // ...work...
    }
}

// In main.c
xTaskCreateAffinitySet(task_name, "TaskName", STACK_SIZE, NULL, PRIORITY, CORE_0, NULL);
```

### After (Runnable Component)
```c
// Structured state in context
typedef struct {
    SomeState_t state;
    QueueHandle_t taskQueue;
} TaskNameContext_t;

// Init function - runs BEFORE scheduler
static int task_name_init(void *pramInitialize, void *pramRunnable) {
    (void)pramInitialize;  // Reserved for config injection
    
    TaskNameContext_t *ctx = malloc(sizeof(TaskNameContext_t));
    if (ctx == NULL) return INIT_ERR_MALLOC;
    
    memset(ctx, 0, sizeof(TaskNameContext_t));
    
    // Initialize all context fields
    ctx->state = {...};
    ctx->taskQueue = xQueueCreate(4, sizeof(Message_t));
    if (ctx->taskQueue == NULL) {
        free(ctx);
        return INIT_ERR_QUEUE;
    }
    
    *(TaskNameContext_t **)pramRunnable = ctx;
    return INIT_SUCCESS;
}

// Run function - runs as FreeRTOS task
static void task_name_run(void *pramRunnable) {
    TaskNameContext_t *ctx = (TaskNameContext_t *)pramRunnable;
    
    printf("Task started\n");
    
    while (1) {
        // Work loop - same as before, but using ctx->field instead of g_field
        // ...work using ctx->...
    }
}

// Runnable declaration - goes in module
const runnable_t task_name_runnable = {
    .name = "TaskName",
    .stack_size = STACK_SIZE,
    .priority = PRIORITY,
    .affinity_mask = CORE_0,
    .initialize = task_name_init,
    .run = task_name_run,
    .pramInitialize = NULL,
    .pramRunnable = NULL
};
```

## Refactoring Checklist (Per Module)

### Step 1: Analyze Current State
- [ ] List ALL static globals used by the task function
- [ ] Identify which come from:
  - [ ] Module-specific (owns them, move to context)
  - [ ] Shared infrastructure (extern from infrastructure.h, don't copy)
- [ ] Example: `g_audioReady` (extern/shared) vs `g_scan_active` (module-specific)

### Step 2: Create Context Structure
- [ ] Name it: `<ModuleName>Context_t`
- [ ] Include ALL module-specific state
- [ ] Do NOT include shared infrastructure (those are extern)
- [ ] Example context for network.c:
```c
typedef struct {
    wifi_state_t wifi_state;
    char target_ssid[33];
    char target_password[64];
    uint32_t connection_start_time;
    bool dhcp_started;
    bool udp_auto_started;
    uint32_t udp_startup_time;
    uint32_t last_check;
    uint32_t last_diagnostic;
    uint32_t poll_count;
} NetworkContext_t;
```

### Step 3: Create Init Function
```c
static int network_init(void *pramInitialize, void *pramRunnable) {
    (void)pramInitialize;  // Reserved, not used yet
    
    // Allocate context
    NetworkContext_t *ctx = malloc(sizeof(NetworkContext_t));
    if (ctx == NULL) return INIT_ERR_MALLOC;
    
    // Zero-initialize entire context
    memset(ctx, 0, sizeof(NetworkContext_t));
    
    // Initialize fields from old static initializers
    ctx->wifi_state = WIFI_STATE_IDLE;
    ctx->dhcp_started = false;
    ctx->udp_auto_started = false;
    ctx->udp_startup_time = 0;
    ctx->last_check = 0;
    ctx->last_diagnostic = 0;
    ctx->poll_count = 0;
    
    // Store pointer in pramRunnable for task to retrieve
    *(NetworkContext_t **)pramRunnable = ctx;
    
    // Init-time checks (e.g., hardware init)
    wifi_init();  // Called once, safe to call from init
    
    // Auto-load credentials if they exist (init-time, before scheduler runs)
    if (network_credentials_exist()) {
        char ssid[33], password[64];
        if (network_credentials_load(ssid, password)) {
            printf("[NETWORK] Auto-loading credentials (SSID: %s)\n", ssid);
            strncpy(ctx->target_ssid, ssid, 32);
            strncpy(ctx->target_password, password, 63);
        }
    }
    
    return INIT_SUCCESS;
}
```

Key Points:
- Allocate context on heap with proper error checking
- Initialize ALL fields (even to 0/NULL)
- Return error codes, not void
- Store context pointer via `*(Type**)pramRunnable = ctx`

### Step 4: Create Run Function
Rename existing `task_name()` to `task_name_run()` and replace ALL `g_` references with `ctx->`:

```c
static void network_run(void *pramRunnable) {
    NetworkContext_t *ctx = (NetworkContext_t *)pramRunnable;
    
    printf("Network task started\n");
    
    // Periodic polling loop
    ctx->last_check = to_ms_since_boot(get_absolute_time());
    ctx->last_diagnostic = to_ms_since_boot(get_absolute_time());
    
    while (1) {
        // Poll WiFi driver
        cyw43_arch_poll();
        ctx->poll_count++;
        
        // Every 1 second, check connection status
        uint32_t now = to_ms_since_boot(get_absolute_time());
        
        if ((now - ctx->last_check) >= 1000) {
            ctx->last_check = now;
            // ... rest of loop using ctx->field instead of g_field
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

Key Points:
- Rename `task_name()` → `task_name_run()`
- Replace ALL `g_variable` → `ctx->variable`
- Keep all logic exactly the same
- Cast `pramRunnable` to context type immediately

### Step 5: Declare Runnable Instance
```c
// At module scope (after functions defined)
const runnable_t network_runnable = {
    .name = "Network",
    .stack_size = 2048,           // From xTaskCreateAffinitySet(..., 2048, ...)
    .priority = 2,                 // From xTaskCreateAffinitySet(..., 2, ...)
    .affinity_mask = (1 << 0),    // CORE_0 = (1 << 0)
    .initialize = network_init,
    .run = network_run,
    .pramInitialize = NULL,
    .pramRunnable = NULL
};
```

Stack size and priority come from current main.c xTaskCreateAffinitySet call.

### Step 6: Update Module Includes
- [ ] Add `#include "runnable.h"` at top
- [ ] Add `#include <stdlib.h>` for malloc/free (if not present)
- [ ] Verify `#include "infrastructure.h"` exists (for extern globals)

### Step 7: Export Runnable in Header
In module.h, add (before final #endif):
```c
// Runnable component interface
extern const runnable_t <module>_runnable;
```

### Step 8: Create Module Shortcuts (Optional but Helpful)
For modules with multiple globals that other modules access:
```c
// network.h
extern const wifi_ap_t *network_get_scan_results(uint32_t *count);
extern bool network_is_connected(void);
```

This encapsulates context access without exposing internal structure.

### Step 9: Build & Test
```bash
cd /home/doug/rpi-pico/vibecode4
rm -rf build && cmake -B build
make -j4
```

Expected Result:
- ✅ No compile errors
- ✅ No duplicate symbol errors
- ✅ Module's `<module>_runnable` symbol defined and exported

### Step 10: Update main.c (After All Modules Converted)
```c
// Extern declarations for all module runnables
extern const runnable_t parser_runnable;
extern const runnable_t network_runnable;
extern const runnable_t waterfall_runnable;
extern const runnable_t udp_audio_runnable;
extern const runnable_t microphone_runnable;

// Registry array (null-terminated)
static const runnable_t *const g_runnables[] = {
    &parser_runnable,
    &network_runnable,
    &waterfall_runnable,
    &udp_audio_runnable,
    &microphone_runnable,
    NULL  // Sentinel
};

// Phase 1: Initialize all modules (before scheduler)
static void rtos_initialize_all(void) {
    const runnable_t *const *p = g_runnables;
    while (*p != NULL) {
        const runnable_t *r = *p;
        printf("[INIT] Initializing %s...\n", r->name);
        int ret = r->initialize(r->pramInitialize, &r->pramRunnable);
        INIT_ASSERT(ret, r->name);
        p++;
    }
}

// Phase 2: Create tasks (before scheduler starts)
static void rtos_create_all_tasks(void) {
    const runnable_t *const *p = g_runnables;
    while (*p != NULL) {
        const runnable_t *r = *p;
        printf("[TASK] Creating %s (%s, priority %d, stack %d)\n", 
               r->name, r->affinity_mask == 1 ? "Core0" : "Core1", r->priority, r->stack_size);
        xTaskCreateAffinitySet(
            r->run,
            r->name,
            r->stack_size,
            (void *)r->pramRunnable,  // Pass context
            r->priority,
            r->affinity_mask,
            NULL
        );
        p++;
    }
}

// In main()
int main(void) {
    // ... early init ...
    
    // Phase 0: Create shared infrastructure
    printf("[INIT] Phase 0: Creating shared infrastructure\n");
    int ret = infrastructure_init();
    INIT_ASSERT(ret, "infrastructure");
    
    // Phase 1: Initialize all modules
    printf("[INIT] Phase 1: Initializing modules\n");
    rtos_initialize_all();
    
    // Phase 2: Create all tasks
    printf("[INIT] Phase 2: Creating FreeRTOS tasks\n");
    rtos_create_all_tasks();
    
    // Phase 3: Start scheduler (no return from here)
    printf("[INIT] Phase 3: Starting FreeRTOS scheduler\n");
    vTaskStartScheduler();
    
    // Should never reach here
    while (1);
}
```

## Reference: Task Analysis

### Task 1: parser_task (main.c, line 220)
- Type: Command loop, no dynamic state
- State: None (stateless command parser)
- **Complexity: LOW** - Start here!
- Action: Create parser_init() that just returns INIT_SUCCESS, rename parser_task to parser_run

### Task 2: network_task (network.c, line 423)
- Type: WiFi scanning, connection state machine
- State: `g_scan_active`, `g_scan_start_time`, `g_wifi_state`, `g_target_ssid/password`, `g_connection_start_time`, `g_dhcp_started`, `g_udp_auto_started`, `g_udp_startup_time`
- **Complexity: MEDIUM** - Good second choice
- Action: Extract 8 variables into NetworkContext_t

### Task 3: udp_audio_task (udp_audio_server.c, line 231)
- Type: Audio transmission loop, queue-based
- State: Possibly socket/UDP state variables
- **Complexity: MEDIUM** - Can do in parallel with network

### Task 4: waterfall_task (waterfall.c, line 186)
- Type: FFT accumulation and display rendering
- State: Display state, FFT bin accumulation state
- **Complexity: MEDIUM-HIGH** - Many variables likely

### Task 5: microphone_task (Actually in main.c, not found separately)
- Type: Audio capture, PDM interrupt handling, queue submission
- State: PDM state, interrupt handlers, buffer management
- **Complexity: HIGH** - Most complex, do last

## Conversion Order (Recommended)
1. ✅ **parser.c** (0 state) - 10 minutes
2. ✅ **network.c** (8 state vars) - 25 minutes
3. ⏳ **udp_audio_server.c** (? state vars) - 20 minutes
4. ⏳ **waterfall.c** (? state vars) - 30 minutes
5. ⏳ **microphone.c** (complex, DMA+ISR) - 45 minutes
6. ⏳ **main.c** (orchestration) - 20 minutes

**Total Estimated Time: 2.5 hours for complete conversion + validation**

## Troubleshooting

### Issue: Compile error "g_variable declared in multiple files"
**Cause:** Variable still global static in source, conflicting with context
**Fix:** Remove `static` declaration, move to context struct

### Issue: "undefined reference to module_runnable"
**Cause:** Runnable declared as `static` or not exported in header
**Fix:** Remove `static`, add `extern const runnable_t module_runnable;` in module.h

### Issue: "Task receives NULL pramRunnable"
**Cause:** Context pointer not stored via `*(Type**)pramRunnable = ctx` in init
**Fix:** Ensure init function stores pointer and doesn't return error

### Issue: "Segfault when task accesses ctx field"
**Cause:** Context structure definition mismatch between init and run
**Fix:** Verify typedef is same in both functions, use consistent naming

## Validation After Each Module

After converting each module:
```bash
# Step 1: Rebuild
cd /home/doug/rpi-pico/vibecode4
rm -rf build && cmake -B build
make -j4

# Step 2: Check for errors
echo "Exit code: $?"

# Step 3: Quick sanity check (if device present)
# Flash and test one specific task's functionality
# E.g., after network.c refactor, test WiFi scan
```

## Example Success Output (Per Module)
```
[INIT] Initializing Network...
[INIT] Network initialized successfully
[TASK] Creating Network (Core0, priority 2, stack 2048)
[TASK] Task created: Network
Network task started
[NETWORK] Poll frequency: 50 polls/5s (every 100.0ms avg)
```

---

**Next Step:** Apply this template to each module in order. Start with [parser.c], then [network.c], etc.
