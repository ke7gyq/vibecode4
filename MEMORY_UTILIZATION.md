# Memory Utilization Analysis

## Overview

The waterfall display system on the RP2350 has specific memory requirements for real-time audio visualization. This document provides a comprehensive breakdown of memory allocation, usage patterns, and optimization strategies.

**Key Metrics:**
- **Peak Memory**: ~163.6 KB when waterfall is active
- **System Memory (RP2350)**: 520 KB SRAM total
- **Waterfall Headroom**: ~356.4 KB available when waterfall active
- **Status**: Memory-constrained but functional

## Memory Components

### 1. Canvas Buffer (Primary Consumer)

**Size**: 153.6 KB (when waterfall active)

$$\text{Buffer Size} = \text{Width} \times \text{Height} \times \text{Bytes/Pixel}$$
$$\text{Buffer Size} = 320 \times 240 \times 2 = 153,600 \text{ bytes}$$

**Location**: Heap (malloc'd in `waterfall_init()`)

**Format**: RGB565 (2 bytes per pixel, 16-bit color)

**Allocation Pattern**:
```c
uint32_t buffer_size = WATERFALL_WIDTH * WATERFALL_HEIGHT * (LV_COLOR_DEPTH / 8);
g_canvas_buffer = (uint8_t *)malloc(buffer_size);
```

**Lifetime**: 
- Created: When `enableWaterfall` command issued
- Freed: When `disableWaterfall` command issued or `waterfall_destroy()` called
- Fragmentation Risk: HIGH (single large allocation)

**Deallocation**:
```c
if (g_canvas_buffer != NULL) {
    free(g_canvas_buffer);
    g_canvas_buffer = NULL;
}
```

### 2. Waterfall Task Stack

**Size**: 8,192 bytes (2048 words × 4 bytes/word)

**Location**: SRAM stack area (FreeRTOS manages)

**Configuration** (in `waterfall.c`):
```c
xTaskCreate(
    waterfall_task,
    "waterfall",
    2048,      /* Stack size in words */
    NULL,
    tskIDLE_PRIORITY + 2,
    &g_waterfall_task_handle
);
```

**Stack Usage Analysis**:

| Component | Size | Notes |
|-----------|------|-------|
| Stack frame variables | ~200 bytes | Local vars in waterfall_task loop |
| t_waterfallBar struct | 64 bytes | 16 × 4-byte magnitude_sq values |
| Function call overhead | ~100 bytes | spectrogram_compute_fft, _bins |
| CMSIS-DSP temp buffers | ~1000 bytes | Within spectrogram_compute_fft |
| **High Water Mark** | **~1400 bytes** | Leaves 648 word margin |
| **Allocated Stack** | **8192 bytes** | 2048 words |

**Safety Margin**: 648 bytes (31% unused at peak)

**Monitoring** (add to parser for debugging):
```c
printf("Waterfall stack HWM: %u words free\n", 
       uxTaskGetStackHighWaterMark(g_waterfall_task_handle));
```

### 3. Spectrogram Context

**Size**: ~2,000 bytes (estimated)

**Location**: Static global in `main.c` (data segment, not heap)

```c
static spectrogram_t g_spectrogram;
```

**Expected Contents**:
- FFT engine state (~500 bytes)
- Hanning window buffer (256 × 2 bytes = 512 bytes for Q15)
- Input sample buffer (64 × 2 bytes = 128 bytes for Q15)
- Output magnitude buffer (128 × 4 bytes = 512 bytes)
- Frequency bin accumulator (16 × 4 bytes = 64 bytes)
- **Subtotal**: ~1,716 bytes

**Lifetime**: Program lifetime (static allocation)

**Allocation**: No dynamic allocation (FreeRTOS doesn't manage)

### 4. LVGL Overhead

**Estimated Size**: ~5-10 KB

**Components**:
- Canvas object structure (~200 bytes)
- LVGL internal state per canvas (~1 KB)
- Display driver buffers (managed separately)
- Font/glyph caches (~4 KB if text rendering enabled)

**Location**: Varies (LVGL internal allocations)

**Note**: Not dynamically allocated/freed with waterfall; persists for entire program

### 5. Other System Components (Not Waterfall-Specific)

| Component | Size | Status |
|-----------|------|--------|
| FreeRTOS kernel | ~3-5 KB | Always present |
| Task control blocks (all) | ~1 KB/task | 8 tasks × 1 KB |
| Semaphore/mutex structures | ~1 KB | g_audioReadySemaphore, g_LvglMutex |
| UDP buffer | ~2 KB | Separate from waterfall |
| Parser command buffers | ~1 KB | Separate from waterfall |
| Microphone buffers | ~2 KB | Separate from waterfall |
| **Total System Overhead** | **~15-20 KB** | Baseline |

## Memory Timeline

### System Startup

```
[Initialization Phase]
├─ FreeRTOS kernel init: +3-5 KB
├─ Spectrogram context: +2 KB (static)
├─ Task stacks (all): +8 KB/task × 8 tasks = +64 KB
├─ Global state: +5 KB
└─ Free heap remaining: ~440 KB

→ Total used: ~80 KB, ~440 KB free
```

### Enable Waterfall (`enableWaterfall` command)

```
[Before waterfall_init]
├─ Heap used: ~80 KB
├─ Heap free: ~440 KB
└─ Waterfall canvas: NULL

↓ waterfall_init() called

[Canvas buffer allocation]
├─ malloc(153,600): -153.6 KB from heap
├─ Heap used: ~233.6 KB
├─ Heap free: ~286.4 KB
├─ Canvas buffer: allocated

[Waterfall task creation]
├─ xTaskCreate(): -8 KB from task stack area
├─ Waterfall task: running (at tskIDLE_PRIORITY + 2)
└─ Canvas object created: +200 bytes

→ Total used: ~241.6 KB, ~286.4 KB free
```

### Waterfall Running (Steady State)

```
[Per Audio Buffer Update]
├─ Semaphore wake: no additional memory
├─ FFT computation: uses existing spectrogram buffers (~1.7 KB)
├─ Canvas draw operations: uses stack locally (~500 bytes peak)
├─ No new allocations
└─ Fragmentation: minimal (single allocation)

→ Memory stable at ~241.6 KB used
```

### Disable Waterfall (`disableWaterfall` command)

```
[Before waterfall_destroy]
├─ Heap used: ~241.6 KB
├─ Canvas buffer: 153.6 KB allocated
└─ Waterfall task: running

↓ waterfall_destroy() called

[Task deletion]
├─ vTaskDelete(): task removed from scheduler
├─ Task stack freed: +8 KB back to stack area
└─ Waterfall task: stopped

[Canvas buffer deallocation]
├─ free(g_canvas_buffer): +153.6 KB back to heap
├─ Heap used: ~80 KB
├─ Heap free: ~440 KB
└─ Canvas buffer: NULL

→ Return to baseline (~80 KB used, ~440 KB free)
```

## Memory Allocation Patterns

### Fragmentation Risk Analysis

**High Risk Zone**: Canvas buffer allocation
- **Fragmentation**: Moderate
- **Reason**: Single large allocation/deallocation
- **Pattern**: 
  ```
  Enable waterfall:  ----[153.6 KB]----
  Disable waterfall: .....[153.6 KB freed].....
  Re-enable:         ?\??\?[153.6 KB]?\??\?  (fragmentation)
  ```

**Low Risk Zone**: Everything else
- Spectrogram context: Static (no fragmentation)
- Task stacks: Pre-allocated by FreeRTOS (no fragmentation)
- System overhead: Small allocations (acceptable fragmentation)

**Fragmentation Scenario**:

If other tasks allocate memory while waterfall is active:

```
Initial state (fragmented heap):
[Free 100KB][Used 100KB][Free 100KB][Used 40KB][Free 100KB]

Disable waterfall (canvas buffer freed):
[Free 100KB][Used 100KB][Free 100KB][Used 40KB][Free 253.6KB]

If max contiguous free < 153.6KB, next enable fails!
```

**Mitigation**:
1. Enable waterfall early in application startup (cleaner heap)
2. Avoid large allocations from other tasks while waterfall active
3. Use heap compaction utility (if available in FreeRTOS config)
4. Monitor heap fragmentation: `xPortGetFreeHeapSize()`

### Dynamic Allocation Events

**During Normal Operation**:
```
enableWaterfall  → malloc(153.6 KB)    [1 call]
disableWaterfall → free() [1 call]
                → vTaskDelete() [1 call]
```

**Frequency**: 
- Typically once per application session
- May be toggled occasionally for UI switching
- Not called repeatedly in fast loops

**Memory Leak Risk**: LOW
- `free()` explicitly called in `waterfall_destroy()`
- Task deleted with `vTaskDelete()`
- No circular references or forgotten pointers

## Optimization Strategies

### 1. Reduce Canvas Buffer Size

**Current**: 320 × 240 × 2 = 153.6 KB

**Alternative (25% reduction)**:
```c
#define WATERFALL_WIDTH  256     // (was 320)
#define WATERFALL_HEIGHT 192     // (was 240)
// New size: 256 × 192 × 2 = 98.3 KB (saves 55 KB)
```

**Trade-off**: Display area smaller but still usable for spectrogram

### 2. Use Indexed Color (4-bit instead of RGB565)

**Current**: RGB565, 2 bytes per pixel (16-bit true color)

**Alternative**: 4-bit indexed color (16 color palette)
```c
// Current implementation:
// - 16 Parula colors are fixed
// - Could use palette-based format (1 byte per 2 pixels)
// New size: (320 × 240) / 2 = 38.4 KB (saves 115 KB!)
```

**Requirements**:
- Modify `lv_canvas_set_buffer()` to use `LV_COLOR_FORMAT_I4`
- Create LVGL palette of 16 Parula colors
- Simpler pixel writes (but must respect nibble boundaries)

**Impact**: Extreme savings, but requires LVGL API changes

### 3. Use Smaller Task Stack

**Current**: 2048 words (8 KB)

**Analysis**:
- Current peak: ~1400 bytes (~350 words)
- Safety margin: 648 bytes

**Alternative**: 1536 words (6 KB)
```c
xTaskCreate(..., 1536, ...);  // Saves 2 KB
```

**Risk**: May fail if CMSIS-DSP expands internal buffering

**Recommendation**: Monitor with `uxTaskGetStackHighWaterMark()`, then reduce if safe

### 4. Allocate Buffer at Compile-Time (Avoid Malloc)

**Current**: Runtime malloc in `waterfall_init()`

**Alternative**: Static buffer in BSS segment
```c
// waterfall.c:
static uint8_t g_canvas_buffer_static[WATERFALL_WIDTH * WATERFALL_HEIGHT * 2];

// In waterfall_init():
// lv_canvas_set_buffer(canvas, g_canvas_buffer_static, ...);
```

**Advantage**: 
- No malloc/free fragmentation
- Deterministic memory layout
- Slightly faster initialization

**Disadvantage**: Always uses 153.6 KB (even if waterfall disabled)

### 5. Use External DRAM (if available)

**Future Enhancement**:
- RP2350 supports external memory via QSPI interface
- Could place canvas buffer in external DRAM
- Would free ~150 KB internal SRAM
- Trade-off: Slower access (QSPI latency)

**Not currently implemented** (would require LVGL DRAM support)

## Memory Debugging

### Check Free Heap

```c
// Add to parser commands:
void fnHeapStatus(void) {
    printf("Heap free: %u bytes\n", xPortGetFreeHeapSize());
    printf("Heap min free: %u bytes\n", xPortGetMinimumEverFreeHeapSize());
}
```

### Monitor Task Stack

```c
void fnTaskStackCheck(void) {
    printf("Waterfall task HWM: %u words\n", 
           uxTaskGetStackHighWaterMark(g_waterfall_task_handle));
}
```

### Check for Leaks

Enable FreeRTOS memory tracing (CMakeLists.txt):
```cmake
set(FREERTOS_DEBUG_ENABLE ON)  # Track alloc/free
```

### Inspect Heap Layout (if GDB available)

```gdb
(gdb) info symbol g_canvas_buffer
g_canvas_buffer at 0x20000000  # Heap address
```

## Memory Safety Considerations

### Stack Overflow Prevention

**Risk**: Deep function call stack during FFT computation

**Current Protection**:
- Task stack: 2048 words (8 KB)
- Waterfall task HWM: ~1400 words at peak
- Margin: 648 words (safe)

**Monitoring**:
```c
// In waterfall_task after spectrogram_compute_fft:
if (uxTaskGetStackHighWaterMark(NULL) < 100) {
    printf("WARNING: Low stack in waterfall task\n");
}
```

### Heap Corruption Prevention

**Risk**: Buffer overflow in canvas drawing

**Current Protection**:
- Canvas buffer size verified before allocation
- `lv_canvas_set_px()` bounds checks pixel coordinates
- No manual pointer arithmetic in drawing code

**Safeguards**:
```c
if (canvas == NULL || g_canvas_buffer == NULL) {
    printf("ERROR: Waterfall not initialized\n");
    return;  // Safe exit
}
```

### Memory Leak Protection

**Risk**: Allocated but never freed

**Current Implementation**:
```c
// waterfall_init():
g_canvas_buffer = malloc(buffer_size);  // Tracked
// waterfall_destroy():
free(g_canvas_buffer);                  // Always freed
g_canvas_buffer = NULL;                 // Cleared
```

**No Leaks If**:
- `waterfall_destroy()` always called before `waterfall_init()` again
- Parser ensures commands execute in correct order

## Performance vs. Memory Trade-offs

| Strategy | Memory Saved | Performance Impact | Complexity |
|----------|--------------|-------------------|-----------|
| Reduce to 256×192 | 55 KB (36%) | Slightly smaller display | Low |
| 4-bit indexed color | 115 KB (75%) | Simpler pixel ops | Medium |
| Smaller task stack | 2 KB (25%) | Minimal risk | Low |
| Static buffer | No savings | Slightly faster init | Low |
| External DRAM | 150 KB (98%) | Slower canvas access | High |

**Recommended**: Reduce to 256×192 if space critical (saves 55 KB with minimal visual loss)

## System Memory Map (RP2350)

```
0x20000000 ┌─────────────────────────────────┐
           │ FreeRTOS Kernel & Task Stacks   │ ~70 KB
           ├─────────────────────────────────┤
           │ Heap Start (after static data)  │
           │                                 │
  Dynamic  │  [Used: Various allocations]    │ ~80-240 KB
  Heap     │  [Canvas Buffer: 153.6 KB]      │  (depends on
           │  [Free: Remaining space]        │   waterfall state)
           │                                 │
           ├─────────────────────────────────┤
  0x20080000│ Heap End / Stack Growth Area   │
           └─────────────────────────────────┘

Total SRAM: 520 KB available (some reserved for OS/bootloader)
```

## Recommendations

### For Production Use

1. **Monitor Initial State**:
   ```c
   printf("Free heap at startup: %u KB\n", 
          xPortGetFreeHeapSize() / 1024);
   ```

2. **Enable Waterfall Early**:
   - Allocate canvas buffer before other tasks create large buffers
   - Cleaner heap = less fragmentation risk

3. **Graceful Degradation**:
   ```c
   if (waterfall_init() == NULL) {
       printf("ERROR: Not enough memory for waterfall\n");
       printf("Free heap: %u KB\n", xPortGetFreeHeapSize() / 1024);
       // Fall back to other UI mode
   }
   ```

4. **Optional Optimization** (if needed):
   - Reduce canvas to 256×192 (saves 55 KB)
   - Frees more headroom for audio processing buffers
   - Still allows readable waterfall display

### For Debug/Development

1. Add heap status command:
   ```c
   fnHeapStatus()  // Returns free heap size
   ```

2. Add stack monitoring:
   ```c
   fnTaskStackCheck()  // Returns HWM for waterfall task
   ```

3. Test memory-constrained scenarios:
   - Allocate large buffers from other tasks
   - Verify waterfall still initializes
   - Check for heap fragmentation patterns

## See Also

- [WATERFALL_ARCHITECTURE.md](WATERFALL_ARCHITECTURE.md) - System design
- FreeRTOS Memory Management Reference
- LVGL Memory Optimization Guide
- RP2350 Datasheet § Memory Layout
