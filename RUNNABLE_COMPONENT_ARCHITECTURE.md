# Runnable Component Architecture

## Philosophy

**Two-tier initialization pattern** that maintains module locality while managing shared resources cleanly:

### Tier 1: Shared Infrastructure
- **Owner**: `main.c` (via `infrastructure.c`)
- **Responsibility**: Create and manage resources used by multiple modules
- **Examples**: Audio queues (`g_audioQueueUDP`, `g_audioQueueWaterfall`), shared buffers
- **Pattern**: Created in infrastructure before tasks spawn → modules assume they exist

### Tier 2: Module-Specific Resources
- **Owner**: Individual modules (waterfall.c, microphone.c, etc.)
- **Responsibility**: Own and initialize their task state, module-internal buffers, configuration
- **Pattern**: Each module defines its context struct, creates in `<moduleName>_init()`, stores in `pramRunnable`

**Result**: Clean separation of concerns while preserving module locality. Modules don't need to know about infrastructure they don't use.

---

## Error Codes

```c
// src/runnable.h - Common error codes for all init functions
#define INIT_SUCCESS      0
#define INIT_ERR_QUEUE    1  // Queue creation failed
#define INIT_ERR_MALLOC   2  // Memory allocation failed
#define INIT_ERR_HW       3  // Hardware init failed
#define INIT_ERR_CONFIG   4  // Configuration error

// Assert macro: prints error and halts
#define INIT_ASSERT(ret, module) \
    do { \
        if ((ret) != INIT_SUCCESS) { \
            printf("ERROR: %s initialization failed (code %d)\n", (module), (ret)); \
            fflush(stdout); \
            while(1); \
        } \
    } while(0)
```

## Type Definition

```c
// src/runnable.h
typedef struct {
    const char *name;                    // Task name for debugging
    uint32_t stack_size;                 // Stack in words
    UBaseType_t priority;                // Task priority (1-configMAX_PRIORITIES)
    UBaseType_t affinity_mask;           // Core affinity for SMP (e.g., 1<<0 for Core 0)
    
    int (*initialize)(const void *init_param, void *run_param);  // Returns error code
    void (*run)(void *run_param);
    
    const void *pramInitialize;          // Module-specific init context (optional)
    void *pramRunnable;                  // Module-specific runtime context (optional)
} runnable_t;
```

---

## Initialization Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ main()                                                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ Infrastructure Init Phase                                       │
│ (src/infrastructure.c)                                          │
│                                                                 │
│ • Create shared queues: g_audioQueueUDP, g_audioQueueWaterfall │
│ • Allocate shared buffers                                       │
│ • Initialize core clocks, hardware                              │
│                                                                 │
│ RESULT: All shared globals exist, ready for module use          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ Module Initialize Phase (Phase 1)                               │
│ for each runnable: runnable[i]->initialize(...)                │
│                                                                 │
│ waterfall_init()     ─→ Create: accumulators, display context   │
│ microphone_init()    ─→ Create: PDM config, gain state          │
│ udp_init()           ─→ Create: client list, packet buffer      │
│ network_init()       ─→ Create: WiFi state, DHCP config         │
│ parser_init()        ─→ Create: command buffer, parser state    │
│                                                                 │
│ RESULT: All module-specific state ready                         │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ Task Creation Phase (Phase 2)                                   │
│ for each runnable: xTaskCreateAffinitySet(runnable[i]->run, ...) │
│                                                                 │
│ Creates: waterfall_task, microphone_task, udp_task,  ...       │
│                                                                 │
│ RESULT: All tasks created, waiting on scheduler                 │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ vTaskStartScheduler()                                           │
│                                                                 │
│ Tasks begin running, using pre-initialized state                │
└─────────────────────────────────────────────────────────────────┘
```

---

## File Structure

```
src/
├── main.c                    # Minimal: call infrastructure, runnables phases
├── infrastructure.h          # Extern declarations for shared resources
├── infrastructure.c          # Create/init all shared infrastructure
├── runnable.h               # Type definition + registration macros
├── waterfall.c              # Exports waterfallRunnable (owns display state)
├── waterfall.h
├── microphone.c             # Exports microphoneRunnable (owns PDM state)
├── microphone.h
├── udp_audio_server.c       # Exports udpRunnable (owns client list)
├── udp_audio_server.h
├── network.c                # Exports networkRunnable (owns WiFi state)
├── network.h
├── parser.c                 # Exports parserRunnable (owns command state)
└── parser.h
```

---

## Shared Infrastructure Example

**src/infrastructure.h**
```c
#ifndef INFRASTRUCTURE_H
#define INFRASTRUCTURE_H

#include <FreeRTOS.h>
#include <queue.h>
#include "runnable.h"

/**
 * Shared Audio Queues
 * 
 * Created during infrastructure_init() before modules initialize.
 * Modules assume these exist.
 */
extern QueueHandle_t g_audioQueueUDP;      // Microphone → UDP path
extern QueueHandle_t g_audioQueueWaterfall; // Microphone → Waterfall path

/**
 * Shared Audio Buffers
 * 
 * Pre-allocated during infrastructure_init().
 * Owned by microphone, read by waterfall.
 */
extern uint16_t *g_audioBuffer1;
extern uint16_t *g_audioBuffer2;
#define AUDIO_BUFFER_SIZE 528

/**
 * Initialize all shared infrastructure (Phase 0)
 * 
 * @return INIT_SUCCESS on success, error code on failure
 * Called in main() before runnables initialize.
 */
int infrastructure_init(void);

#endif
```

**src/infrastructure.c**
```c
#include "infrastructure.h"
#include "runnable.h"

// Shared queues
QueueHandle_t g_audioQueueUDP;
QueueHandle_t g_audioQueueWaterfall;

// Shared buffers
uint16_t *g_audioBuffer1 = NULL;
uint16_t *g_audioBuffer2 = NULL;

int infrastructure_init(void) {
    printf("Initializing shared infrastructure...\n");
    
    // Create queues
    g_audioQueueUDP = xQueueCreate(4, sizeof(AudioFrame));
    if (!g_audioQueueUDP) {
        printf("  ERROR: Failed to create g_audioQueueUDP\n");
        return INIT_ERR_QUEUE;
    }
    
    g_audioQueueWaterfall = xQueueCreate(4, sizeof(AudioFrame));
    if (!g_audioQueueWaterfall) {
        printf("  ERROR: Failed to create g_audioQueueWaterfall\n");
        return INIT_ERR_QUEUE;
    }
    
    // Allocate buffers
    g_audioBuffer1 = pvPortMalloc(AUDIO_BUFFER_SIZE * sizeof(uint16_t));
    g_audioBuffer2 = pvPortMalloc(AUDIO_BUFFER_SIZE * sizeof(uint16_t));
    if (!g_audioBuffer1 || !g_audioBuffer2) {
        printf("  ERROR: Failed to allocate audio buffers\n");
        return INIT_ERR_MALLOC;
    }
    
    printf("  ✓ Queues and buffers ready\n");
    return INIT_SUCCESS;
}
```

---

## Module Example: Waterfall

**src/waterfall.c (excerpt)**
```c
#include "runnable.h"
#include "infrastructure.h"  // Access shared queues

// Module-specific context (owned by waterfall module)
typedef struct {
    TaskHandle_t taskHandle;
    uint32_t accumulatedPower[WATERFALL_DISPLAY_HEIGHT];
    uint8_t currentRow;
    // ... other display state
} WaterfallContext;

static WaterfallContext g_waterfallContext = {0};

/**
 * Phase 1: Initialize module-specific state
 * Shared resources (queues, buffers) already exist via infrastructure_init()
 * 
 * @return INIT_SUCCESS on success, error code on failure
 */
int waterfall_init(const void *init_param, void *run_param) {
    (void)init_param;
    WaterfallContext *ctx = (WaterfallContext *)run_param;
    
    printf("  Waterfall: Initializing display state...\n");
    
    // Initialize accumulators
    memset(ctx->accumulatedPower, 0, sizeof(ctx->accumulatedPower));
    ctx->currentRow = 0;
    
    // Initialize display hardware (SPI, GPIO, etc.)
    if (st7789_init() != 0) {
        printf("    ERROR: Display hardware init failed\n");
        return INIT_ERR_HW;
    }
    
    st7789_clear();
    
    printf("    ✓ Waterfall ready\n");
    return INIT_SUCCESS;
}

/**
 * Phase 2: Task entry point
 * Assumes infrastructure and initialization already complete
 */
void waterfall_run(void *run_param) {
    WaterfallContext *ctx = (WaterfallContext *)run_param;
    ctx->taskHandle = xTaskGetCurrentTaskHandle();
    
    AudioFrame frame;
    
    while(1) {
        // Wait for audio frame from microphone
        if (xQueueReceive(g_audioQueueWaterfall, &frame, portMAX_DELAY)) {
            // Process FFT, accumulate power
            process_waterfall_frame(&frame, ctx);
            
            // Update display every N frames
            if (ctx->currentRow >= WATERFALL_ACCUMULATION_FRAMES) {
                render_waterfall_row(ctx);
                ctx->currentRow = 0;
            }
        }
    }
}

/**
 * Export runnable for main to register
 */
const runnable_t waterfallRunnable = {
    .name = "waterfall",
    .stack_size = 2048,
    .priority = 2,
    .affinity_mask = (1 << 0),           // Core 0
    .initialize = waterfall_init,
    .run = waterfall_run,
    .pramRunnable = &g_waterfallContext,
    .pramInitialize = NULL,
};
```

---

## Module Example: Microphone

**src/microphone.c (excerpt)**
```c
#include "runnable.h"
#include "infrastructure.h"  // Access shared queues

typedef struct {
    TaskHandle_t taskHandle;
    uint16_t gain;
    uint8_t currentBuffer;  // Index: 0=buffer1, 1=buffer2
    // ... PDM config
} MicrophoneContext;

static MicrophoneContext g_microphoneContext = {0};

void microphone_init(const void *init_param, void *run_param) {
    (void)init_param;
    MicrophoneContext *ctx = (MicrophoneContext *)run_param;
    
    printf("  Microphone: Initializing PDM capture...\n");
    
    ctx->gain = MICROPHONE_DEFAULT_GAIN;
    ctx->currentBuffer = 0;
    
    // Initialize PDM hardware (DMA, GPIO, clock)
    if (pdm_init() != 0) {
        printf("    ERROR: PDM hardware init failed\n");
        return INIT_ERR_HW;
    }
    
    printf("    ✓ Microphone ready\n");
    return INIT_SUCCESS;
}

void microphone_run(void *run_param) {
    MicrophoneContext *ctx = (MicrophoneContext *)run_param;
    ctx->taskHandle = xTaskGetCurrentTaskHandle();
    
    while(1) {
        // Capture 528 samples from PDM
        uint16_t *buffer = (ctx->currentBuffer == 0) ? g_audioBuffer1 : g_audioBuffer2;
        pdm_capture_blocking(buffer, AUDIO_BUFFER_SIZE);
        
        // Create frame
        AudioFrame frame = {
            .data = buffer,
            .length = AUDIO_BUFFER_SIZE,
            .timestamp = xTaskGetTickCount(),
        };
        
        // Send to both queues (UDP and Waterfall)
        xQueueSend(g_audioQueueUDP, &frame, 0);
        xQueueSend(g_audioQueueWaterfall, &frame, 0);
        
        // Swap buffers
        ctx->currentBuffer ^= 1;
    }
}

const runnable_t microphoneRunnable = {
    .name = "microphone",
    .stack_size = 512,
    .priority = 3,
    .affinity_mask = (1 << 0),
    .initialize = microphone_init,
    .run = microphone_run,
    .pramRunnable = &g_microphoneContext,
    .pramInitialize = NULL,
};
```

---

## Main Orchestration

**src/main.c**
```c
#include "infrastructure.h"
#include "runnable.h"

// Extern: Each module exports its runnable
extern const runnable_t waterfallRunnable;
extern const runnable_t microphoneRunnable;
extern const runnable_t udpRunnable;
extern const runnable_t networkRunnable;
extern const runnable_t parserRunnable;

// Registry: Null-terminated array of runnables
static const runnable_t *const g_runnables[] = {
    &waterfallRunnable,
    &microphoneRunnable,
    &udpRunnable,
    &networkRunnable,
    &parserRunnable,
    NULL  // Sentinel
};

/**
 * Phase 1: Initialize all module-specific state
 * Assumes infrastructure already initialized.
 */
static void rtos_initialize_all(void) {
    printf("\n=== Phase 1: Module Initialization ===\n");
    for (size_t i = 0; g_runnables[i] != NULL; i++) {
        const runnable_t *r = g_runnables[i];
        printf("[%zu] Initializing %s...\n", i, r->name);
        int ret = r->initialize(r->pramInitialize, r->pramRunnable);
        INIT_ASSERT(ret, r->name);  // Assert and print on error
    }
    printf("✓ All modules initialized\n\n");
}

/**
 * Phase 2: Create and start all tasks
 */
static void rtos_create_all_tasks(void) {
    printf("=== Phase 2: Task Creation ===\n");
    for (size_t i = 0; g_runnables[i] != NULL; i++) {
        const runnable_t *r = g_runnables[i];
        TaskHandle_t handle = NULL;
        
        BaseType_t result = xTaskCreateAffinitySet(
            r->run,
            r->name,
            r->stack_size,
            r->pramRunnable,
            r->priority,
            r->affinity_mask,
            &handle
        );
        
        if (result != pdPASS) {
            printf("ERROR: Failed to create task %s\n", r->name);
            while(1);  // Panic
        }
        
        printf("[%zu] Created task '%s' (pri=%d, stack=%d, core=0x%x)\n",
               i, r->name, r->priority, r->stack_size, r->affinity_mask);
    }
    printf("✓ All tasks created\n\n");
}

int main(void) {
    // Hardware setup (clock, GPIO, UART for debugging)
    board_init();
    printf("VibeCode4 Boot Sequence\n");
    printf("========================\n\n");
    
    // FreeRTOS initialization
    printf("=== Phase 0: Infrastructure ===\n");
    int ret = infrastructure_init();
    INIT_ASSERT(ret, "infrastructure");  // Assert and print on error
    
    // Module initialization (before scheduler)
    rtos_initialize_all();
    
    // Task creation and scheduler start
    rtos_create_all_tasks();
    
    printf("Starting FreeRTOS scheduler...\n");
    vTaskStartScheduler();
    
    return 0;  // Never reached
}
```
```

---

## Adding a New Module with Shared Resources

**Scenario**: Add a new "temperature monitor" module that needs to log to a shared `g_logQueue`.

**Step 1**: Update infrastructure.h
```c
extern QueueHandle_t g_logQueue;  // NEW: Shared logging queue
```

**Step 2**: Update infrastructure.c
```c
QueueHandle_t g_logQueue;

void infrastructure_init(void) {
    // ... existing code ...
    
    g_logQueue = xQueueCreate(8, sizeof(LogMessage));  // NEW
    if (!g_logQueue) {
        printf("ERROR: Failed to create g_logQueue\n");
        while(1);
    }
}
```

**Step 3**: Create temperature.c with runnable
```c
#include "runnable.h"
#include "infrastructure.h"

typedef struct {
    TaskHandle_t taskHandle;
    float lastTemp;
} TempContext;

static TempContext g_tempContext = {0};

/**
 * Create temperature module with shared infrastructure dependency
 * 
 * Needs to use g_logQueue (shared infrastructure)
 */

void temperature_init(const void *init, void *run) {
    TempContext *ctx = (TempContext *)run;
    ctx->lastTemp = 0.0f;
    
    if (temp_sensor_init() != 0) {
        printf("    ERROR: Temperature sensor init failed\n");
        return INIT_ERR_HW;
    }
    
    printf("    ✓ Temperature sensor ready\n");
    return INIT_SUCCESS;
}

void temperature_run(void *run) {
    TempContext *ctx = (TempContext *)run;
    
    while(1) {
        float temp = read_temperature();
        
        LogMessage msg = {.value = (int)(temp * 100)};
        xQueueSend(g_logQueue, &msg, 0);  // Uses shared infrastructure
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

const runnable_t temperatureRunnable = {
    .name = "temperature",
    .stack_size = 256,
    .priority = 1,
    .affinity_mask = (1 << 0),
    .initialize = temperature_init,
    .run = temperature_run,
    .pramRunnable = &g_tempContext,
};
```

**Step 4**: Update main.c
```c
extern const runnable_t temperatureRunnable;  // NEW

static const runnable_t *const g_runnables[] = {
    &waterfallRunnable,
    &microphoneRunnable,
    &temperatureRunnable,      // NEW
    &udpRunnable,
    &networkRunnable,
    &parserRunnable,
    NULL
};
```

That's it. Three changes to existing code, one new module file.

---

## Advantages of This Design

1. **Clear Ownership**
   - Shared infrastructure: owned by `main.c` via `infrastructure.c`
   - Module state: owned by individual modules
   - No ambiguity about who allocates what

2. **Module Locality Preserved**
   - Modules only `#include` the infrastructure they use
   - Don't need to know about unrelated modules
   - Easy to debug: "This module uses g_audioQueueUDP and my microphone context"

3. **Safe Initialization Order**
   - Phase 0: Infrastructure (guarantees all shared resources exist)
   - Phase 1: Modules initialize (assume infrastructure ready)
   - Phase 2: Tasks create (assume all state ready)
   - No race conditions from initialization order

4. **Extensibility**
   - Add new shared resource: update infrastructure.h/c + module includes
   - Add new module: create runnable, declare it in main.c, add to array
   - Remove module: remove from array + extern declaration (no other changes)

5. **Testability**
   - Call `infrastructure_init()` and `module_init()` independently
   - Mock or substitute modules for unit testing
   - Easy to test initialization without scheduler

6. **Performance**
   - All allocation before scheduler → no fragmentation
   - Queue handles known at compile time → inline-friendly
   - Single pass through runnables array

---

## Error Handling

All init functions return error codes. Main asserts and prints on failure.

**Error codes** (defined in runnable.h):
```c
#define INIT_SUCCESS      0
#define INIT_ERR_QUEUE    1  // Queue creation failed
#define INIT_ERR_MALLOC   2  // Memory allocation failed
#define INIT_ERR_HW       3  // Hardware init failed
#define INIT_ERR_CONFIG   4  // Configuration error

// Assert macro: prints error, flushes stdout, halts
#define INIT_ASSERT(ret, module) \
    do { \
        if ((ret) != INIT_SUCCESS) { \
            printf("ERROR: %s initialization failed (code %d)\n", (module), (ret)); \
            fflush(stdout); \
            while(1); \
        } \
    } while(0)
```

**Usage in main:**
```c
int ret = infrastructure_init();
INIT_ASSERT(ret, "infrastructure");

// In rtos_initialize_all():
for (size_t i = 0; g_runnables[i] != NULL; i++) {
    int ret = g_runnables[i]->initialize(...);
    INIT_ASSERT(ret, g_runnables[i]->name);
}
```

---

1. Create `src/runnable.h` with type definition
2. Create `src/infrastructure.{h,c}` with queue/buffer allocation
3. Refactor modules one-by-one:
   - Extract module context struct
   - Create `<module>_init()` and `<module>_run()` functions
   - Create `<module>Runnable` export
   - Add `extern` declarations for shared infrastructure
4. Update `src/main.c`:
   - Call `infrastructure_init()`
   - Call `rtos_initialize_all()`
   - Call `rtos_create_all_tasks()`
   - Add `g_runnables[]` array
5. Test and validate

---

## Summary

This architecture achieves the best of both worlds:
- **Shared resources** managed centrally (infrastructure.c) for safety
- **Module-specific state** owned locally (each .c file) for clarity
- **Clean interfaces** via extern declarations and runnable contracts
- **Safe initialization** through three explicit phases
- **Extensibility** through simple registry pattern
