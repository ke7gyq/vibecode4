# Task Function Analysis for Runnable Component Refactoring

## Overview
Analysis of 4 remaining task functions to be refactored into `runnable_t` pattern. Each section describes global/static variables accessed, identifies task-specific state (should become context), identifies external dependencies (must remain global), and notes any complications.

---

## 1. Parser Task (parser.c)

### Current Implementation
**File**: [src/parser.c](src/parser.c)
**Task Function**: `parse()` at line 1235 (NOT a task function, called from TCP server handler)

### What It Does
```c
uint8_t parse(char *buffer, void *v)
```
- Entry point: Command string from TCP network input
- Extracts first token from buffer
- Searches `aTokens[]` array for matching command function
- Calls matching token function with remainder of string
- **NOT a FreeRTOS task** - called sequentially by network stack

### Global/Static Variables Accessed

#### **Globals (External, Must Stay Global)**
- `g_LvglMutex` - LVGL display mutex (extern from main.c)
- `g_blinkRateMs` - LED blink rate (extern from main.c)
- `g_micDebug` - Microphone debug level (extern from infrastructure.h)

#### **Static Module State (Local to parser.c)**
- `g_waterfallMode` - Waterfall display mode (READ in token functions)
  - Set by `fnWaterfallMode()`, `fnEnableWaterfall()`, `fnDisableWaterfall()`
  - Read by `waterfall_should_feed_fft()` helper
  
#### **Token Function Array (Immutable)**
- `aTokens[]` - Static const array at line 1163
  - ~33 command definitions
  - Points to token handler functions (all static, all in parser.c)

### State to Move to Context (ParserContext)
```c
typedef struct {
    waterfall_mode_t waterfallMode;     // Current waterfall display mode
} ParserContext;
```
- **Only `g_waterfallMode`** needs to be in context
- Everything else is either immutable (aTokens) or accessed externally

### External Dependencies (Stay Global)
✅ `g_LvglMutex` - Used by waterfall/display token functions
✅ `g_blinkRateMs` - Used by blink token function
✅ `g_micDebug` - Used by micDebug token function
✅ ALL display/infrastructure state - Token functions interact with global systems

### Special Considerations

**CRITICAL**: Parser is NOT a task function
- It's called from TCP server handler (network.c) when command arrives
- **No runnable_t needed for parser itself**
- However, `g_waterfallMode` should move to context IF parser is refactored
- For now: Continue calling `parse()` from network task (no change needed)

**Future Option**: If want to refactor parser as message-driven run loop:
- Wrap parse() in a task that reads from command queue
- Move `g_waterfallMode` to `ParserContext`
- Keep all token functions static (internal helpers)
- Expose context via const `parserRunnable`

### Refactoring Complexity
🟡 **MEDIUM** - Parser is not task-based currently
- No task loop to refactor
- Would require creating queue-based command pipeline
- Currently integrated into network receive flow (tight coupling)
- **Recommendation**: Leave parser.c as-is for now
  - Can refactor later if command flow changes
  - Currently well-designed as synchronous request handler

---

## 2. UDP Audio Task (src/udp_audio_server.c)

### Current Implementation
**File**: [src/udp_audio_server.c](src/udp_audio_server.c)
**Task Function**: `udp_audio_task()` at line 231

### What It Does
```c
void udp_audio_task(void *parameters)
```
1. **Infinite loop** waits on `g_audioQueueUDP` for audio buffer messages
2. Extracts buffer from message and sends to all active UDP clients
3. Tracks frame sequence, dropped buffers, and statistics
4. Timestamps and detects send latency (buffer pressure)

**Core Loop**:
```
xQueueReceive(g_audioQueueUDP, ...) → validate message → udp_audio_send_frame() → stats
```

### Global/Static Variables Accessed

#### **Shared Infrastructure (extern from infrastructure.h)**
- `g_audioQueueUDP` - Queue for audio messages (READ from queue)
- `g_audioBuffers` - Audio sample buffers (READ: buffer1/buffer2)
- `g_micDebug` - Debug level (READ for logging)

#### **Static Module State (Local to udp_audio_server.c)**
- `audio_pcb` (struct udp_pcb) - UDP socket
- `g_server_running` - Server active flag
- `g_udp_clients[]` - Array of 4 connected clients (tracking IP, port, activity)
- `g_udp_audio_task_handle` - This task's handle (for ISR notifications)
- `g_current_buffer_being_sent` - Which buffer currently being transmitted
- `g_udp_send_buffer[]` - Static copy of audio (for safe lwIP transmission)
- `g_audio_frames_sent` - Frame count
- `g_audio_samples_sent` - Sample count
- `g_audio_samples_dropped` - Dropped sample count
- `g_audio_last_stats_time` - Last stats report timestamp
- `g_audio_frames_sent_attempts` - Send attempt counter
- `g_audio_sendto_errors` - UDP send error counter
- `g_udp_last_sequence` - Last received sequence number
- `g_udp_frame_count` - Total frames processed
- `g_udp_frames_skipped` - Skipped frames due to backpressure
- `g_udp_last_report_time` - Last stats report time
- `g_udp_packets_received` - Incoming registration packets

#### **Helper Functions (All Static)**
- `udp_audio_send_frame()` - Sends frame to all active clients
- `udp_client_init()` - Client registration handler

### State to Move to Context (UdpAudioContext)
```c
typedef struct {
    TaskHandle_t taskHandle;
    
    // Client management
    udp_client_t clients[MAX_CLIENTS];
    int8_t sendBuffer[AUDIO_BUFFER_SIZE];      // Static TX buffer
    
    // Sequence tracking
    uint32_t lastSequence;
    uint32_t frameCount;
    
    // Statistics
    uint32_t framesDropped;
    uint32_t lastReportTime;
    uint32_t lastStatsTime;
    uint32_t frameAttempts;
    uint32_t sendErrors;
    
    // Server state
    struct udp_pcb *serverPcb;
    int serverRunning;
} UdpAudioContext;
```

### External Dependencies (Stay Global)
✅ `g_audioQueueUDP` - Must remain global (shared by microphone producer)
✅ `g_audioBuffers` - Must remain global (shared audio memory)
✅ `g_micDebug` - Must remain global (external config)

**Network stack state**:
- lwIP PCB structures - Owned by network.c, accessed via API calls
- Client registration/hello mechanism - Handled externally

### Special Considerations

1. **Circular Buffer State**: Audio buffers (buffer1/buffer2) are owned by microphone
   - **Stay global**: Context only holds references via message
   
2. **Socket Lifecycle**: UDP socket created during initialization
   - **Move to context**: Store `struct udp_pcb *` in UdpAudioContext
   - Already scoped to module, safe to move

3. **Queue Message Flow**: 
   - Microphone → g_audioQueueUDP → UDP task
   - Queue handle **stays global** (shared producer/consumer)
   - Message content is independent per task invocation

4. **Statistics Tracking**: All stats are task-local
   - Safe to move entirely to context
   - No external code reads UDP stats directly

5. **lwIP Thread Safety**:
   - All UDP operations from single UDP task (good)
   - No locking needed (already serialized)
   - `pbuf_free()` called from task context (safe)

### Refactoring Concerns
🟢 **LOW COMPLEXITY** - Straightforward state migration
1. Create `UdpAudioContext` struct
2. Move all `g_udp_*` and `g_audio_*` static vars → context
3. Add `UdpAudioContext g_udpContext = {0}` static instance
4. Update `udp_audio_task()` signature: `void udp_audio_task(void *param)` → extract context
5. Update `udp_audio_send_frame()` to take context parameter
6. Export `const runnable_t udpAudioRunnable`

**Helpers Needing Changes**:
- `udp_audio_send_frame()` - Add context parameter
- No other public API functions to update

---

## 3. Waterfall Task (src/waterfall.c)

### Current Implementation
**File**: [src/waterfall.c](src/waterfall.c)
**Task Function**: `waterfall_task()` at line 186

### What It Does
```c
void waterfall_task(void *parameters)
```
1. **Infinite loop** waits on `g_audioQueueWaterfall` for audio buffer messages
2. Checks waterfall mode (`WATERFALL_MODE_LIVE_AUDIO`?) - skips if off
3. Processes through spectrogram FFT (256→128 complex bins)
4. Accumulates 9 FFT frames into power accumulator
5. When bar ready: extracts colormap indices, applies display mutex, renders bar

**Core Loop**:
```
xQueueReceive(g_audioQueueWaterfall, ...) → 
  waterfall_get_mode() → 
  spectrogram_process_samples() → 
  waterfall_accm_add_fft() → 
  (if bar complete) waterfall_accm_get_bar() → 
  fillPixelsToBar() → addBar()
```

### Global/Static Variables Accessed

#### **Shared Infrastructure (extern)**
- `g_audioQueueWaterfall` - Queue for audio messages (READ from queue)
- `g_spectrogram` - FFT processor context (from main.c) (READ)
- `g_LvglMutex` - Display mutex for screen updates (READ: xSemaphoreTake/Release)

#### **Module Functions Accessed**
- `waterfall_get_mode()` - Defined in parser.c (reads `g_waterfallMode`)
- `waterfall_should_feed_fft()` - Query if feedi FFT data
- `setColorMap()` - Set active colormap
- `fillPixelsToBar()` - Convert colormap indices → pixel buffer
- `addBar()` - Write bar to display via ST7789

#### **Static Module State (Local to waterfall.c)**
- `g_colorPointer` - Pointer to active colormap (Jet/Parula)
- `g_colormapIndex` - Index of active colormap (0 or 1)
- `pixBuf` - Pixel buffer (24 freq bins × 10×10 pixels)
- `RGB_jet_colormap_16[]` - const colormap data
- `parula_colormap_16[]` - const colormap data
- `aColorPointers[]` - const array of colormap pointers

### State to Move to Context (WaterfallContext)
```c
typedef struct {
    TaskHandle_t taskHandle;
    
    // Accumulator for 9 frames
    waterfall_accm_t accm;
    
    // Display buffers
    uint16_t logAccmPower[24];          // Colormap indices for current bar
    pixel_buffer_t pixBuf;              // Pixel buffer (24 freq bins)
    
    // Colormap management
    const uint16_t *colorPointer;
    uint16_t colormapIndex;
    
    // Statistics
    uint32_t frameCount;
    uint32_t barCount;
    uint32_t lastReportTime;
} WaterfallContext;
```

### External Dependencies (Stay Global)
✅ `g_audioQueueWaterfall` - Must remain global (shared by microphone producer)
✅ `g_spectrogram` - Must remain global (shared FFT processor)
✅ `g_LvglMutex` - Must remain global (shared display lock, used by LVGL)

### Special Considerations

1. **Global Colormap State**:
   - `setColorMap()`, `getColorMap()` functions are PUBLIC API
   - External code calls `setColorMap(idx)` from parser commands
   - **Solution**: 
     - Keep `setColorMap()` public
     - Have it write to a per-context instance
     - Add init parameter to set initial colormap

2. **Display Mutex**:
   - `g_LvglMutex` created in main.c (not here)
   - Used for LVGL thread safety
   - **Stay global**: Mutex ownership is main.c, not waterfall

3. **Spectrogram FFT Output**:
   - `g_spectrogram.fft_output` - 512 q15 values (256 complex pairs)
   - Owned by main spectrogram instance (global)
   - Task just reads it per frame
   - **Stay global**: Shared by audio pipeline

4. **Pixel Buffer Lifecycle**:
   - `pixBuf` is currently static module-level
   - Used by `fillPixelsToBar()` (called from task)
   - **Move to context**: Task-specific, no external access

5. **Helper Functions** (called from task):
   - `waterfall_accm_init()` - Takes `waterfall_accm_t *`
   - `waterfall_accm_add_fft()` - Takes accm reference
   - **Already designed for context!** Just pass context's accm

### Refactoring Concerns
🟢 **LOW-MEDIUM COMPLEXITY** - Good modularity
1. Create `WaterfallContext` struct
2. Add `waterfall_accm_t` and local buffers to context
3. Create static `WaterfallContext g_waterfallContext = {0}`
4. Move `g_colorPointer`, `g_colormapIndex`, `pixBuf` → context
5. Update `waterfall_task()` to extract context from parameter
6. Update `setColorMap()`, `getColorMap()` to use context
7. Helpers already accept pointers (waterfall_accm_add_fft) - no change needed
8. Export `const runnable_t waterfallRunnable`

**Helpers Needing Changes**:
- `setColorMap()` - Add context parameter OR keep as-is and write global (less ideal)
- `getColorMap()` - Same decision
- `fillPixelsToBar()` - Already takes pointer, no change needed
- `addBar()` - Already takes pointer, no change needed

**Decision**: `setColorMap()`/`getColorMap()` can stay global for now
- Low overhead to check both global and context
- Simplifies parser command handlers (don't need context parameter)

---

## 4. Microphone Task (src/microphone.c)

### Current Implementation
**File**: [src/microphone.c](src/microphone.c)
**Task Function**: `microphone_task()` at line 392

### What It Does
```c
void microphone_task(void *parameters)
```
1. **Initialization**: Calls `microphone_init()` to set up PDM/DMA hardware
2. **Infinite loop**:
   - **If real microphone**: Waits for DMA completion notification from ISR
   - **If test mode**: Generates synthetic 1kHz sine wave
3. Processes DMA/synthetic PDM data through `Open_PDM_Filter_64()` 
4. Accumulates PCM samples into output buffers (buffer1/buffer2)
5. When buffer full (528 samples): 
   - Sends message to `g_audioQueueUDP` and `g_audioQueueWaterfall`
   - Gives semaphore `g_audioReadySemaphore`
   - Switches to other buffer

**Core Loop (Real Hardware)**:
```
ulTaskNotifyTake() [DMA ISR] →
  Open_PDM_Filter_64() [PDM→PCM] →
  (if buffer full) xQueueSend() → switch buffers
```

### Global/Static Variables Accessed

#### **Shared Infrastructure (extern from infrastructure.h)**
- `g_audioQueueUDP` - **Message OUTPUT to UDP consumer**
- `g_audioQueueWaterfall` - **Message OUTPUT to waterfall consumer**
- `g_audioReadySemaphore` - Legacy binary semaphore (OUTPUT: signal when buffer ready)
- `g_audioBuffers` - Audio sample buffers (WRITE: buffer1/buffer2)
- `g_micDebug` - Debug level (READ for logging)
- `g_audioReady` - Status volatile (WRITE: 0=none, 1=buffer1, 2=buffer2)

#### **Static Module State (Local to microphone.c)**
**PDM Library State**:
- `g_pdm_mic` - PDM library context struct (config, DMA channel, buffers, filter)

**Microphone Hardware State**:
- `g_microphone_initialized` - Initialization flag
- `g_pio`, `g_sm`, `g_dma_ch` - PIO/state machine/DMA channel handles
- `g_dma_buffer_a[]`, `g_dma_buffer_b[]` - Raw PDM ping-pong buffers
- `g_current_dma_buffer`, `g_other_dma_buffer` - Pointers to active DMA buffers
- `g_microphone_task_handle` - This task's handle (for DMA ISR notification)

**Filter Configuration**:
- `g_pdm_filter_config` - OpenPDM2PCM filter parameters

**Task Local State**:
```c
// In microphone_task() stack (reset each invocation):
uint16_t pcm_index = 0;              // Index into output buffer
uint8_t current_buffer = 1;          // Which output buffer (1 or 2)
int16_t *p_current_buffer = NULL;    // Pointer to output buffer
uint32_t filter_calls_accumulated;   // DMA→Filter accumulation counter
uint32_t sample_count = 0;           // Total samples processed
uint32_t buffer_count = 0;           // Total buffers output
uint32_t last_sample_count = 0;      // Last sample count (for stats)
uint32_t stats_counter = 0;          // Check time every N iterations
uint32_t last_stats_time = 0;        // Last stats report timestamp

// DMA state
uint32_t *current_dma_buffer = NULL; // Current buffer being filled
uint32_t *other_dma_buffer = NULL;   // Next buffer to fill
```

**Message Sequence**:
- `g_audioMessageSequence` - Sequence counter (incremented per buffer ready)

### State to Move to Context (MicrophoneContext)
```c
typedef struct {
    TaskHandle_t taskHandle;
    
    // Hardware state
    struct {
        PIO pio;
        uint pio_sm;
        uint dma_channel;
        bool initialized;
    } hardware;
    
    // DMA buffers
    uint32_t dmaBufferA[DMA_BUFFER_WORDS];
    uint32_t dmaBufferB[DMA_BUFFER_WORDS];
    uint32_t *currentDmaBuffer;
    uint32_t *otherDmaBuffer;
    
    // Output buffer state
    uint8_t outputBufferIndex;         // 1 or 2
    int16_t *pCurrentOutputBuffer;
    uint16_t outputBufferSampleIndex;
    
    // Filter configuration & state
    TPDMFilter_InitStruct filterConfig;
    uint16_t filterCallsAccumulated;
    
    // Statistics
    uint32_t totalSamplesProcessed;
    uint32_t totalBuffersOutput;
    uint32_t lastStatsTime;
    uint32_t statsCheckInterval;
    
    // Mode
    bool useRealMicrophone;
} MicrophoneContext;
```

### External Dependencies (Stay Global)
✅ `g_audioQueueUDP` - Queue handle (shared with producer/consumer)
✅ `g_audioQueueWaterfall` - Queue handle (shared)
✅ `g_audioReadySemaphore` - Semaphore handle (legacy, shared)
✅ `g_audioBuffers` - Audio buffer memory (shared output destination)
✅ `g_audioReady` - Status volatile (shared ready flag)
✅ `g_micDebug` - Debug level (external config)

**Why these stay global**:
- **Queues/semaphores**: Handles are obtained at infrastructure init, shared between components
- **Audio buffers**: Physical memory allocated for input/output, multiple components read them
- **Status volatile**: External code polls `g_audioReady` for buffer availability

### Special Considerations

1. **DMA ISR Handler**:
   ```c
   static void microphone_dma_irq_handler(void)
   ```
   - Runs in ISR context (not task context)
   - Accesses: `g_current_dma_buffer`, `g_other_dma_buffer`, `g_dma_ch`, `g_microphone_task_handle`
   - **Must have access to context!**
   - **Solution**: Register ISR with context as parameter
   - Need to change ISR to take context and call task via handle stored in context

2. **DMA ISR Registration**:
   - Called during `microphone_init()` (called from task)
   - `irq_set_exclusive_handler(dma_irq, microphone_dma_irq_handler)`
   - **Problem**: IRQ handler is static function with no parameter
   - **Solution**: Store context in global for ISR access, OR refactor ISR to be generic with context
   - **Current approach**: Keep context state in static variable that ISR can access

3. **Buffer Ownership**:
   - Microphone fills `g_audioBuffers.buffer1` and `buffer2`
   - UDP and waterfall tasks read from these buffers
   - **These stay global**: Physical memory, multiple owners
   - **Context only tracks which buffer is "current"**: index + pointer

4. **PDM Library Integration**:
   - `g_pdm_mic` struct from microphone-library-for-pico
   - Complex nested state (DMA config, filter state, callback handlers)
   - **Move to context**: Task-specific, not accessed elsewhere
   - **Filter config**: Initialize once, use from context

5. **Compile-Time Switch**:
   - `#define USE_REAL_MICROPHONE 1` - Real vs test mode
   - Affects loop behavior (`ulTaskNotifyTake()` vs sine generation)
   - **Option 1**: Keep as compile-time switch (simpler, no context needed)
   - **Option 2**: Move to context (more flexible)
   - **Recommendation**: Keep as compile-time (no runtime cost)

6. **Sequence Counter**:
   - `g_audioMessageSequence` - Incremented per buffer ready
   - Used to number messages for seq tracking
   - **Could move to context** but is also accessed by infrastructure
   - **Decision**: Keep global for now (stateless counter, no contention)
   - OR: Move to context and have microphone pass via message

### Refactoring Concerns
🟠 **MEDIUM-HIGH COMPLEXITY** - ISR integration
1. Create `MicrophoneContext` struct
2. Move all task-local state → context
3. Create static `MicrophoneContext g_micContext = {0}`
4. Update `microphone_task()` to extract context
5. **CRITICAL**: Update DMA ISR handler
   - ISR needs access to context (buffer pointers, task handle)
   - Options:
     a. Keep static context global for ISR, task extracts from it (current approach, no change)
     b. Register ISR with context closure (more complex, requires ISR wrapper)
     c. Use helper function in ISR that accesses context
   - **Recommendation**: Option (a) - Context via static variable for ISR access
6. Update `microphone_init()` to initialize context
7. Export `const runnable_t microphoneRunnable`

**Helpers Needing Changes**:
- `microphone_init()` - Initialize context hardware state
- `pdm_filter_config_init()` - Use context filterConfig
- `microphone_dma_irq_handler()` - Already accesses via global context

**ISR Access Pattern**:
```c
// In microphone_dma_irq_handler():
// Global context available (same as before, no change to ISR code!)
// Just swap buffer pointers from context:
uint32_t *temp = g_micContext.currentDmaBuffer;
g_micContext.currentDmaBuffer = g_micContext.otherDmaBuffer;
g_micContext.otherDmaBuffer = temp;
```

---

## Summary Table

| Task | Entry Point | Type | Complexity | Context State | External Deps | Issues |
|------|-------------|------|-----------|---------------|---------------|--------|
| **Parser** | `parse()` | Function | N/A | N/A | Extensive | NOT a task - leave as-is |
| **UDP Audio** | `udp_audio_task()` | Task (line 231) | 🟢 Low | Client state, stats, socket | Queue, buffers, debug | None |
| **Waterfall** | `waterfall_task()` | Task (line 186) | 🟢 Low | Accum, colormap, pixbuf | Queue, FFT, mutex | None |
| **Microphone** | `microphone_task()` | Task (line 392) | 🟠 Medium | DMA buffers, filter config, PCM index | Queues, buffers, status | **ISR handler must access context** |

---

## Refactoring Order Recommendation

1. **Waterfall** (easiest) - No ISR, simple queue loop, self-contained state
2. **UDP Audio** (simple) - Straightforward state migration, queue-driven
3. **Microphone** (complexity) - ISR integration, PDM library integration
4. **Parser** (skip for now) - Not a task, low refactoring value

## Next Steps

1. Create context structs for each task
2. Add static context instances to each module
3. Refactor task functions to extract context and use it
4. Update initialize functions to set up context state
5. Export runnable_t definitions
6. Update main.c to assemble runnables array
7. Test audio pipeline (verify same performance)
8. Commit with clear message about pattern

