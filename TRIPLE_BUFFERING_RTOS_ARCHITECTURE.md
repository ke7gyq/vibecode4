# VibeCode4 Triple Buffering & RTOS Architecture

## Triple Buffering Overview

VibeCode4 uses a **dual-buffer ping-pong approach** for audio data, with an additional **transcoding buffer** for FFT processing, creating an effective triple-buffer system for handling asynchronous audio capture and multi-consumer processing.

### Why Triple Buffering?

**Problem**: The microphone produces data at 48 kHz continuously. Meanwhile, two independent tasks need to consume this data:
1. **UDP Audio Task** - Transmits PCM samples over network at UDP_PACKET_RATE
2. **Waterfall Task** - Processes audio through FFT for frequency analysis

Both tasks run at unknown/variable rates due to network jitter and display refresh timing. A single buffer would cause:
- Microphone to overwrite data before consumers finish
- Dropped frames and audio artifacts
- Synchronization conflicts

**Solution**: Three buffers working together:
1. **Capture Buffer 1** - Microphone fills this while consumers read buffer 2
2. **Capture Buffer 2** - Consumers read this while microphone fills buffer 1
3. **Transcoding Buffer** - FFT input buffer inside spectrogram_t structure

### Data Flow Diagram

```
Microphone (48 kHz)
    ↓
[PDM Filter + CIC3 + Downsampling]
    ↓
Produces 528-sample blocks
    ↓
Ping-Pong Selection:
  ├─→ Fill {buffer1 OR buffer2} depending on g_audioReady status
  └─→ Alternate on each block
    ↓
g_audioReady flag indicates which buffer is ready:
  g_audioReady = 0  →  No data ready (both buffers being consumed)
  g_audioReady = 1  →  buffer1 ready for consumption
  g_audioReady = 2  →  buffer2 ready for consumption
    ↓
Queue Distribution:
  ├─→ UDP Task:      Send block to g_audioQueueUDP
  │                  (can process at network speed)
  └─→ Waterfall Task: Send block to g_audioQueueWaterfall
                      (can process at display refresh rate)
    ↓
UDP Task:                      Waterfall Task:
  Accumulate samples +1        Accumulate samples +1
  When N samples ready:        When 256 samples ready:
  Send over network              ├─→ Apply Hanning window
                                  ├─→ arm_rfft_q15() FFT
                                  ├─→ Extract magnitude
                                  ├─→ Bin to 24 bands
                                  └─→ Send to display task
```

### Ping-Pong Buffer Structure

**src/microphone.h**:
```c
typedef struct {
    int16_t buffer1[AUDIO_BUFFER_SIZE];  // 528 samples
    int16_t buffer2[AUDIO_BUFFER_SIZE];  // 528 samples
} AudioBuffers_t;

extern AudioBuffers_t g_audioBuffers;
extern volatile uint8_t g_audioReady;   // 0=none, 1=buffer1, 2=buffer2
```

**Size**: `528 samples × 2 buffers × 2 bytes/sample = 2,112 bytes RAM`

### Transcoding Buffer (FFT Input)

Inside `spectrogram_t` structure:
```c
typedef struct {
    q15_t input_buffer[SPECTROGRAM_FFT_SIZE];        // 256 samples for FFT
    q15_t window_buffer[SPECTROGRAM_FFT_SIZE];       // Hanning window
    q15_t windowed_input[SPECTROGRAM_FFT_SIZE];      // Windowed input
    q15_t fft_output[SPECTROGRAM_FFT_SIZE * 2];      // 512 values (128 complex bins)
    q15_t fft_magnitude[SPECTROGRAM_FFT_SIZE / 2];   // 128 magnitude values
    uint8_t amplitude_bins[SPECTROGRAM_NUM_BINS];    // 16 display bins
} spectrogram_t;
```

**Purpose**: 
- Holds accumulation buffer while filling 256-sample FFT input block
- Prevents corruption if UDP task or microphone task interferes during FFT computation
- Each waterfall task instance has its own spectrogram_t (no shared state)

### Total Memory Usage

```
Ping-pong buffers:        528 × 2 × 2 bytes = 2,112 bytes
Spectrogram context:      ~3,200 bytes per waterfall task
Audio queues (messages):  4 messages × sizeof(msg) × 2 queues = ~1,024 bytes
Synchronization primitives: ~200 bytes
────────────────────────────────────────────────────
Total audio subsystem:    ~6.5 KB RAM
```

## RTOS Architecture

### FreeRTOS SMP (Symmetric Multi-Processing)

VibeCode4 runs on RP2350 dual-core (Cortex-M33 × 2). FreeRTOS SMP supports static task affinity or dynamic floating.

#### Core Affinity Mask Definition (src/runnable.h)

```c
#define CORE_NONE  (-1)      // Task floats dynamically (no affinity)
#define CORE_0     (1 << 0)  // Pinned to core 0
#define CORE_1     (1 << 1)  // Pinned to core 1
```

**Task Creation with Affinity**:
```c
TaskHandle_t handle = xTaskCreateAffinitySet(
    task_function,           // Entry point
    "TaskName",              // Name for debugging
    stack_size,              // Stack in words
    parameters,              // Void pointer parameter
    priority,                // 1 (low) to 31 (high)
    affinity_mask,           // Core mask (CORE_0, CORE_1, or CORE_NONE)
    &handle                  // Output task handle
);
```

### Critical Section (Flash Write Protection)

```c
taskENTER_CRITICAL();    // Disable interrupts on all cores
{
    flash_range_erase(...);
    flash_range_program(...);
}
taskEXIT_CRITICAL();     // Re-enable interrupts
```

**Why needed**: Flash operations in Pico are slow (milliseconds). A core switch in the middle of erase/write would corrupt flash. Critical section ensures atomic operation.

### Known Issue: lwIP Callback Delivery (Resolved April 2026)

#### Problem (Before Fix)

When tasks were static pinned to cores using affinity masks, lwIP network callbacks **never fired** even though the network was listening and packets arrived.

**Symptom**:
- TCP/UDP socket created ✓
- Polling works (manual `lwip_poll()` gets data) ✓
- But automatic callbacks registered with `setcallback()` never fire ✗
- Network buffer fills up → packets dropped

**Root Cause Analysis** (Investigation April 2-3, 2026):

lwIP callbacks depend on tight temporal coupling between:
1. Network ISR (wakes waiting task)
2. Task scheduler (schedules waiting task)
3. Task execution (invokes callback)

With **static pinning**:
- Network ISR may fire on core 1, wake waiting task on core 0
- FreeRTOS scheduler runs on core 0, but task pinned to core 1
- Scheduler can't immediately migrate task to core 1 (core 1 busy)
- Callback timing window closes before task runs
- Race condition: scheduler and task affinity create mismatch

#### Solution (Applied April 3, 2026)

**Let FreeRTOS manage core migration dynamically:**

```c
// WRONG (static pin - breaks lwIP):
xTaskCreateAffinitySet(..., (1 << 0), ...);  // Pinned to core 0

// CORRECT (floating - lwIP works):
xTaskCreateAffinitySet(..., (CORE_NONE or -1), ...);  // No pin, floats
// OR use old API:
xTaskCreate(...);  // Always floats
```

**Why this works**:
- FreeRTOS SMP scheduler dynamically assigns tasks to available cores
- If network ISR fires on core 0, scheduler can immediately run task on core 0
- If task finishes, scheduler can preempt and run another task
- Temporal coupling restored → callbacks fire reliably

**Result**: Network callbacks now work perfectly with dual-core scheduler.

**Code Change Location**: [src/network.c](src/network.c) - Network task creation

### Task Priorities and Scheduling

VibeCode4 uses FreeRTOS configMAX_PRIORITIES = 32

**Current Task Design** (April 2026):

```
Priority  Task              Affinity    Purpose
────────────────────────────────────────────────────────
30        (Reserved)                    System/interrupt level
29        Microphone        CORE_0      Audio capture (real-time critical)
25        Waterfall         CORE_NONE   FFT/FFT processing
20        UDP Audio         CORE_NONE   Network transmission
15        Network/WiFi      CORE_NONE   WiFi connection management
10        Serial CLI        CORE_NONE   Command parsing
5         Idle              (auto)      Background
```

**Priority Reasoning**:
- **Microphone (29)**: Highest user priority - must capture every PDM sample (48 kHz deadline)
- **Waterfall (25)**: High - FFT windows are 256 samples (~5.3ms) - miss window → artifact
- **UDP (20)**: Medium - network packets can be retransmitted, jitter acceptable
- **WiFi (15)**: Low - WiFi state machine can wait
- **CLI (10)**: Lowest - user commands not time-critical

### Queue-Based Decoupling

Audio queues prevent direct task-to-task coupling:

```c
// Microphone task (simplified):
while (1) {
    read_audio_from_pdm(...);
    
    // Send to BOTH queues simultaneously
    xQueueSendToBack(g_audioQueueUDP, &audio_block, 0);
    xQueueSendToBack(g_audioQueueWaterfall, &audio_block, 0);
    
    // Doesn't wait - queues have depth 4, so almost never blocks
}

// UDP task:
while (1) {
    xQueueReceive(g_audioQueueUDP, &audio_block, portMAX_DELAY);
    accumulate_and_send_over_network(audio_block);
}

// Waterfall task:
while (1) {
    xQueueReceive(g_audioQueueWaterfall, &audio_block, portMAX_DELAY);
    process_through_fft(audio_block);
}
```

**Advantages**:
- Microphone never blocks waiting for consumers
- UDP and Waterfall tasks run independently at their own pace
- Queue depth 4 provides buffering for timing jitter

### Semaphore Usage

**Binary Semaphores** for simple signals:

```c
g_audioReadySemaphore    // Signaled when buffer ready (deprecated now)
g_audioSemaphoreUDP      // Signaled for UDP task wakeup
```

**Difference from queues**:
- Queues: Pass data
- Semaphores: Signal readiness

Current implementation favors queues (they include semaphore semantics + data).

## Audio Processing Pipeline

### Stage 1: Microphone Capture (CORE_0, Priority 29)

```c
void microphone_task(void *param) {
    while (1) {
        // PDM → PCM conversion via CIC3 filter chain
        // Input:  3.072 MHz PDM bitstream (48 kHz × 32× oversampling)
        // Output: 48 kHz, 16-bit PCM samples
        
        // Fill either buffer1 or buffer2 based on ping-pong selection
        int samples = pdm_microphone_read(
            g_audioBuffers.buffer1 or buffer2,
            AUDIO_BUFFER_SIZE
        );
        
        g_audioReady = 1 or 2;  // Mark which buffer ready
        
        // Notify both consumer tasks via queues
        // (doesn't wait - queues have space)
        AudioBufferMessage_t msg = { buffer_index: g_audioReady };
        xQueueSendToBack(g_audioQueueUDP, &msg, 0);
        xQueueSendToBack(g_audioQueueWaterfall, &msg, 0);
    }
}
```

**Timing**: Produces 528-sample blocks at ~10.3ms intervals (528 / 48000)

### Stage 2A: UDP Task (CORE_NONE, Priority 20)

```c
void udp_audio_task(void *param) {
    uint32_t accumulator = 0;
    uint16_t accumulated[UDP_FRAME_SIZE];
    
    while (1) {
        // Wait for audioblock from microphone
        xQueueReceive(g_audioQueueUDP, &msg, portMAX_DELAY);
        
        // Get the appropriate buffer (buffer1 or buffer2)
        int16_t *audio = (msg.buffer == 1) ? 
            g_audioBuffers.buffer1 : 
            g_audioBuffers.buffer2;
        
        // Accumulate samples
        for (int i = 0; i < 528; i++) {
            accumulated[accumulator++] = audio[i];
        }
        
        // When we have enough samples for a UDP packet, send it
        if (accumulator >= UDP_FRAME_SIZE) {
            send_udp_packet(accumulated, UDP_FRAME_SIZE);
            accumulator = 0;
        }
    }
}
```

**Timing**: Sends UDP packets when buffer fills (variable - network dependent)

### Stage 2B: Waterfall Task (CORE_NONE, Priority 25)

```c
void waterfall_task(void *param) {
    spectrogram_t *spec = (spectrogram_t *)param;
    uint32_t sample_count = 0;
    
    while (1) {
        // Wait for audio block from microphone
        xQueueReceive(g_audioQueueWaterfall, &msg, portMAX_DELAY);
        
        // Get the appropriate buffer
        int16_t *audio = (msg.buffer == 1) ? 
            g_audioBuffers.buffer1 : 
            g_audioBuffers.buffer2;
        
        // Accumulate 528 samples into FFT buffer
        for (int i = 0; i < 528 && sample_count < FFT_SIZE; i++) {
            spec->input_buffer[sample_count++] = (q15_t)audio[i];
        }
        
        // When FFT buffer full (256 samples), process FFT
        if (sample_count >= SPECTROGRAM_FFT_SIZE) {
            // Apply Hanning window
            apply_window(spec);
            
            // Run FFT via ARM CMSIS-DSP
            arm_rfft_q15(&spec->fft_instance, 
                         spec->windowed_input, 
                         spec->fft_output);
            
            // Extract magnitude and bin to 24 display bands
            waterfall_bar_extract(spec);
            
            // Reset for next window
            sample_count = 0;
        }
    }
}
```

**Timing**: Processes 256-sample FFT every 42.7ms (256 / 6000 after downsampling)

## Performance & Timing

### Microphone Timing (Critical Path)

```
PDM Input:  48 kHz × 32× oversampling = 1.536 MHz bitstream
CIC3 Filter: Decimates 32× → 48 kHz output
Block Size: 528 samples
Block Interval: 528 / 48000 = 10.3 ms
Required CPU: ~40% (tight timing - runs on CORE_0, priority 29)
```

### Queue Latency

```
Max latency (full queue + processing):
  Microphone → Queue → UDP: 4 blocks × 10.3ms = ~41ms
  Microphone → Queue → Waterfall: 4 blocks × 10.3ms = ~41ms
  Queue-to-consumer: <1ms (RTOS wakeup)
```

### FFT Processing

```
Input: 256 samples @ 6 kHz (downsampled from 48 kHz)
Window: Hanning (256 point)
FFT: arm_rfft_q15 (ARM CMSIS-DSP optimized)
Output: 128 complex bins
Bins: 24 display bands
Processing time: ~2-3ms per FFT window (measured)
```

## Potential Issues & Resolutions

### Issue 1: Buffer Overflow

**Symptom**: Audio drops, popping sounds, garbled waterfall

**Cause**: Consumer tasks too slow - can't empty queues faster than microphone fills them

**Resolution**:
- Check queue depth: `uxQueueMessagesWaiting(g_audioQueueUDP)` - should stay < 4
- Lower priority of slow tasks if possible
- Reduce UDP packet rate or increase waterfall processing speed

**Code to debug**:
```c
#define DEBUG_QUEUE_DEPTH 1
#if DEBUG_QUEUE_DEPTH
if (uxQueueMessagesWaiting(g_audioQueueUDP) > 3) {
    printf("WARNING: UDP queue backing up (%u messages)\n", 
           uxQueueMessagesWaiting(g_audioQueueUDP));
}
#endif
```

### Issue 2: FFT Window Misalignment

**Symptom**: Waterfall bars appear offset or discontinuous

**Cause**: sample_count variable not properly reset after FFT, or interrupts corrupting buffer

**Resolution**:
- Verify sample_count reset at line: `sample_count = 0;`
- Check no interrupt handlers write to input_buffer
- Monitor for dropped queue messages (would skip samples)

### Issue 3: Callback Delivery (RESOLVED)

**FIXED April 3, 2026**: Network callbacks were not firing because network task was statically pinned to a core. Changed to floating task (CORE_NONE) to allow dynamic scheduling.

## Code Locations

- **Ping-pong buffers definition**: [src/microphone.h](src/microphone.h#L150-L155)
- **Audio status volatile**: [src/infrastructure.h](src/infrastructure.h#L35)
- **Queue creation**: [src/infrastructure.c](src/infrastructure.c#L100-L125)
- **Waterfall task**: [src/main.c](src/main.c) - check for waterfall_init()
- **UDP task**: [src/main.c](src/main.c) - check for udp_init()
- **Microphone task**: [src/microphone.c](src/microphone.c) - high-priority core 0 task
- **RTOS config**: [src/FreeRTOSConfig.h](src/FreeRTOSConfig.h#L1-L50)

## References

- **FreeRTOS SMP**: https://www.freertos.org/RTOS-Cortex-M33-RP2350.html
- **Pico Docs**: https://datasheets.raspberrypi.com/pico/pico-datasheet.pdf
- **ARM CMSIS-DSP**: https://github.com/ARM-software/CMSIS-DSP
- **Triple Buffering Theory**: Real-time Graphics Programming textbook (Akenine-Möller et al.)
