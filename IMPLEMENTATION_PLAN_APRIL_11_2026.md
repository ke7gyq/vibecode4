# Runnable Component Architecture Implementation Plan
**Date**: April 11, 2026  
**Status**: Design Complete, Ready for Implementation  
**Snapshot**: `vibecode4_20260411_pre_runnable_refactor.tgz` (132M)

---

## Architecture Overview

**Two-tier initialization pattern** with null-terminated runnable registry:

### Tier 1: Shared Infrastructure (main.c owns)
- Created in `infrastructure.c` before modules initialize
- Examples: `g_audioQueueUDP`, `g_audioQueueWaterfall`, `g_audioBuffer1/2`
- Pattern: All shared resources exist before Phase 1 module init

### Tier 2: Module-Specific Resources (modules own)
- Each module creates its own context struct
- Owns task state, internal buffers, configuration
- Initialized in `<module>_init()` function

---

## Type Definitions

### runnable.h
```c
// Error codes
#define INIT_SUCCESS      0
#define INIT_ERR_QUEUE    1
#define INIT_ERR_MALLOC   2
#define INIT_ERR_HW       3
#define INIT_ERR_CONFIG   4

// Assert macro
#define INIT_ASSERT(ret, module) \
    do { \
        if ((ret) != INIT_SUCCESS) { \
            printf("ERROR: %s initialization failed (code %d)\n", (module), (ret)); \
            fflush(stdout); \
            while(1); \
        } \
    } while(0)

// Runnable interface
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
```

---

## Three-Phase Initialization

### Phase 0: Infrastructure
```c
int infrastructure_init(void)
// Creates: all shared queues, buffers, hardware config
// Returns: error code
// Called: first thing in main()
```

### Phase 1: Module Init
```c
for each runnable:
    int ret = runnable->initialize(init_param, run_param)
    INIT_ASSERT(ret, name)
// Creates: module-specific state, task handles storage
// Assumes: infrastructure queues already exist
```

### Phase 2: Task Creation
```c
for each runnable:
    xTaskCreateAffinitySet(runnable->run, name, stack, run_param, priority, affinity, &handle)
// Creates: actual FreeRTOS tasks with pre-initialized state
// Assumes: all module state ready
```

---

## Implementation Checklist

### Step 1: Create Core Files
- [ ] `src/runnable.h` — Type definition + error codes + assert macro
- [ ] `src/infrastructure.h` — Extern declarations for shared resources
- [ ] `src/infrastructure.c` — Queue/buffer creation and initialization

### Step 2: Refactor Modules (One at a time, test after each)
**Module Order** (dependencies matter):
1. [ ] `src/microphone.c` — Audio capture (independent)
2. [ ] `src/waterfall.c` — Display (reads from microphone queue)
3. [ ] `src/spectrogram.c` — (if separate task, else skip)
4. [ ] `src/udp_audio_server.c` — UDP streaming (reads from microphone queue)
5. [ ] `src/network.c` — WiFi/DHCP (may startup other features)
6. [ ] `src/parser.c` — Command parsing (independent)

**Per-Module Pattern**:
```c
// Define module context
typedef struct {
    TaskHandle_t taskHandle;
    // ... module-specific state
} ModuleContext;

static ModuleContext g_<module>Context = {0};

// Init function (Phase 1)
int <module>_init(const void *init_param, void *run_param) {
    ModuleContext *ctx = (ModuleContext *)run_param;
    // Create module-internal state
    // Check return codes, return INIT_SUCCESS or error code
    return INIT_SUCCESS;
}

// Run function (Phase 2 entry point)
void <module>_run(void *run_param) {
    ModuleContext *ctx = (ModuleContext *)run_param;
    ctx->taskHandle = xTaskGetCurrentTaskHandle();
    while(1) { /* task loop */ }
}

// Export runnable
const runnable_t <module>Runnable = {
    .name = "<module>",
    .stack_size = XXX,
    .priority = X,
    .affinity_mask = (1 << 0),
    .initialize = <module>_init,
    .run = <module>_run,
    .pramRunnable = &g_<module>Context,
    .pramInitialize = NULL,
};
```

### Step 3: Update main.c
- [ ] Include `infrastructure.h` and `runnable.h`
- [ ] Declare extern for all module runnables
- [ ] Create `g_runnables[]` null-terminated array
- [ ] Implement `rtos_initialize_all()`
- [ ] Implement `rtos_create_all_tasks()`
- [ ] Replace old task creation code with new pattern

### Step 4: Build & Test
- [ ] CMakeLists.txt includes new files
- [ ] Compiles without errors/warnings
- [ ] Boot sequence runs error-free
- [ ] Audio still works (47 kHz, 0 loss)
- [ ] Display renders (waterfall visible)
- [ ] UDP streaming active

### Step 5: Commit
- [ ] Single unified commit with all refactoring
- [ ] Commit message: "Refactor: Implement runnable component architecture"
- [ ] All modules consolidated into runnable pattern

---

## Key Design Decisions

### ✅ Error Handling
- All `_init()` functions return `int` error code
- Main asserts on error with `INIT_ASSERT()` macro
- Prints error code and halts (embedded-appropriate panic)

### ✅ Null-Terminated Array
- `g_runnables[]` ends with `NULL` sentinel
- Loop: `while (g_runnables[i] != NULL)`
- Flexible: can add/remove modules without recounting

### ✅ Locality Preserved
- Modules only `#include` infrastructure they use
- Module state owned locally (not passed through main)
- `pramRunnable` used for `_run()` function context

### ✅ Core Affinity
- All tasks pinned to Core 0 (current design)
- `affinity_mask` field allows future per-module pinning
- Single line change to split across cores

### ✅ Phase Separation
- Phase 0 (infrastructure): Before modules
- Phase 1 (module init): Before scheduler
- Phase 2 (task create): Last step before `vTaskStartScheduler()`
- Result: Race-free initialization

---

## Reference Documents

- **RUNNABLE_COMPONENT_ARCHITECTURE.md** — Full design spec with examples
- **runnable_t_design.md** (session memory) — Design evolution notes

---

## Testing Strategy

After each module refactoring:
1. Compile: `ninja -C build` (should complete without errors)
2. Boot test: Flash and observe UART output (all 3 phases logged)
3. Functional test: Verify that module still works as before
   - Microphone: Samples captured at 48 kHz
   - Waterfall: Display updates, spectrum visible
   - UDP: Frames transmitted to client at 47 kHz
   - Network: WiFi connects, TCP/UDP ports ready
   - Parser: Commands accepted over TCP

---

## Current Code Status

**Now**: Code is in `vibecode4_20260411_pre_runnable_refactor.tgz`
- Full working system: 47.4 kHz sustained, 0 frame loss
- Waterfall rendering: All 128 FFT bins, full spectrum visible
- UDP streaming: PBUF_ROM optimized (single memcpy)
- Architecture: Ready for refactoring

**After Refactor**: Same performance, cleaner code organization
- Modular initialization pattern
- Clear resource ownership
- Extensible for future modules
- Better debugging (error codes, phase logging)

---

## Success Criteria

- ✅ All 6 modules refactored to runnable pattern
- ✅ Zero build errors/warnings
- ✅ Boot completes all 3 phases successfully
- ✅ Audio performance: 46-47 kHz (unchanged)
- ✅ UDP delivery: 0% loss (unchanged)
- ✅ Display: Waterfall rendering full spectrum
- ✅ Single commit with all changes
- ✅ Code review ready

---

## Timeline Estimate

| Phase | Task | Est. Time |
|-------|------|-----------|
| 1 | Create runnable.h, infrastructure.h/c | 30 min |
| 2 | Refactor microphone.c | 20 min |
| 3 | Refactor waterfall.c | 20 min |
| 4 | Refactor udp_audio_server.c | 20 min |
| 5 | Refactor network.c | 15 min |
| 6 | Refactor parser.c | 15 min |
| 7 | Update main.c | 20 min |
| 8 | Build, test, validate | 30 min |
| 9 | Commit and document | 10 min |
| **Total** | | **3.5 hours** |

---

## Rollback Plan

If issues arise:
```bash
tar -xzf /home/doug/rpi-pico/vibecode4_20260411_pre_runnable_refactor.tgz
# Restore working code, start over
```

---

## Next Steps

1. ✅ Design reviewed and approved
2. ⏳ Ready to create `src/runnable.h`
3. ⏳ Create `src/infrastructure.h/c`
4. ⏳ Begin module refactoring
5. ⏳ Comprehensive testing
6. ⏳ Single unified commit
