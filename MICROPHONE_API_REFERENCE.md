# Microphone Audio API Reference

## Overview

Complete API for PDM microphone audio capture and PCM conversion on the Pico 2 W.

**Feature Summary:**
- 3.072 MHz PDM input → 48 kHz PCM output
- Ping-pong buffering (528 samples = 11 ms per buffer)
- Semaphore-based producer/consumer flow
- Lock-free, interrupt-driven architecture
- Optimized sinc³ decimation filter

---

## Data Types

### AudioBuffers_t

```c
typedef struct {
    int16_t buffer1[AUDIO_BUFFER_SIZE];
    int16_t buffer2[AUDIO_BUFFER_SIZE];
} AudioBuffers_t;
```

**Description:**
- Ping-pong pair of audio buffers
- Each contains PCM samples at 48 kHz sample rate
- `AUDIO_BUFFER_SIZE = 528` samples (11 ms of audio)
- Microphone task fills alternately

**Example:**
```c
extern AudioBuffers_t g_audioBuffers;

int16_t sample = g_audioBuffers.buffer1[0];  // First sample of buffer1
```

---

## Status Variables

### g_audioReady

```c
extern volatile uint8_t g_audioReady;
```

**Values:**
- `0` - No buffer ready
- `1` - buffer1 has ready audio
- `2` - buffer2 has ready audio

**Usage:**
```c
if (g_audioReady == 1) {
    // Process buffer1
    int16_t *audio = g_audioBuffers.buffer1;
}
```

**Thread Safety:** Atomic read (volatile) due to small size

---

### g_audioBuffers

```c
extern AudioBuffers_t g_audioBuffers;
```

**Contains:** Two 528-sample audio buffers

**Note:** 
- Do NOT access directly
- Always wait on semaphore first
- Guaranteed safe only after semaphore taken

---

### g_audioReadySemaphore

```c
extern SemaphoreHandle_t g_audioReadySemaphore;
```

**Type:** FreeRTOS binary semaphore

**Posted By:** Microphone task when buffer fills completely

**Behavior:**
- Guarantees complete, ready buffer
- Prevents partial buffer reads
- Causes consumer task to wake

**Example:**
```c
if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
    // Buffer ready, safe to read
}
```

---

### g_micDebug

```c
extern uint8_t g_micDebug;
```

**Values:**
- `0` - Silent (default)
- `2` - Show buffer fill messages
- `4` - Add statistics per second

**Default:** 0

**Example:**
```c
g_micDebug = 4;  // Enable stats output
// Outputs:
// [Mic] Buffer 1 full (528 samples, 1 total)
// [MicStats] Rate: 48000 Hz, Total: 528 samples, Buffers: 1
```

---

## Initialization

### microphone_init()

```c
bool microphone_init(void);
```

**Description:**
Creates and configures the audio system. Called automatically by microphone_task().

**Parameters:** None

**Returns:**
- `true` - Success, system ready
- `false` - Failure (PIO, DMA, or semaphore creation failed)

**Side Effects:**
- Creates `g_audioReadySemaphore`
- Initializes PIO0 state machine 2
- Claims DMA channel 0
- Starts DMA capture
- Clears g_audioBuffers

**Thread Safety:** Called once during initialization

**Example:**
```c
if (!microphone_init()) {
    printf("ERROR: Microphone init failed\n");
    return;
}
printf("Microphone ready\n");
```

**Note:** Do not call directly—microphone_task calls this on startup.

---

## Audio Status Functions

### get_audio_ready()

```c
uint8_t get_audio_ready(void);
```

**Description:**
Returns which buffer (if any) currently has ready audio.

**Parameters:** None

**Returns:**
- `0` - No audio ready
- `1` - buffer1 ready
- `2` - buffer2 ready

**Thread Safety:** Safe to call anytime (volatile read)

**Example:**
```c
uint8_t buf = get_audio_ready();
if (buf > 0) {
    int16_t *audio = (buf == 1) ? g_audioBuffers.buffer1 
                                 : g_audioBuffers.buffer2;
}
```

**Typical Use:**
```c
xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000));
uint8_t which = get_audio_ready();  // Check which one
```

---

### clear_audio_ready()

```c
void clear_audio_ready(void);
```

**Description:**
Marks the current audio buffer as consumed. Microphone task can then overwrite it.

**Parameters:** None

**Returns:** None

**Thread Safety:** Safe to call from any task

**Important:** 
- Call only after you finish processing audio
- Don't hold the buffer longer than necessary
- Prevents buffer overflow

**Example:**
```c
void consumer_task(void *param) {
    while (1) {
        xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000));
        
        uint8_t buf_id = get_audio_ready();
        int16_t *audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                        : g_audioBuffers.buffer2;
        
        process_audio(audio, AUDIO_BUFFER_SIZE);  // Do your work
        
        clear_audio_ready();  // Mark consumed - IMPORTANT!
    }
}
```

---

## Task Interface

### microphone_task()

```c
void microphone_task(void *parameters);
```

**Description:**
Main microphone capture and PCM conversion task. Runs indefinitely, filling audio buffers.

**Parameters:**
- `parameters` - Unused (pass NULL)

**Returns:** Never returns (FreeRTOS task)

**Behavior:**
1. Initializes microphone system
2. Starts DMA capture
3. Waits for DMA completion notification
4. Applies PDM→PCM filter
5. Accumulates samples in output buffer
6. When buffer full: posts semaphore, switches buffers
7. Repeats

**Creation:**
```c
xTaskCreate(microphone_task, "Microphone", 2048, NULL, 2, NULL);
```

**Stack Size:** 2048 bytes recommended

**Priority:** 2 (normal)

**Thread Safety:** Do not call directly—create as FreeRTOS task only

---

## Consumer Task Pattern

### Complete Example

```c
void audio_logger_task(void *param) {
    printf("Audio Logger started\n");
    
    uint32_t samples_logged = 0;
    
    while (1) {
        // 1. WAIT for buffer
        BaseType_t result = xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(2000));
        
        if (result != pdTRUE) {
            printf("[Logger] Timeout waiting for audio\n");
            continue;
        }
        
        // 2. GET buffer pointer
        uint8_t buf_id = get_audio_ready();
        if (buf_id == 0) {
            printf("[Logger] Invalid buffer?\n");
            continue;
        }
        
        int16_t *audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                        : g_audioBuffers.buffer2;
        
        // 3. PROCESS (work with audio)
        printf("[Logger] Buffer %d: ", buf_id);
        int32_t sum = 0;
        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
            int16_t sample = audio[i];
            if (sample < 0) sample = -sample;
            sum += sample;
        }
        uint16_t avg = sum / AUDIO_BUFFER_SIZE;
        printf("Average amplitude: %u\n", avg);
        
        samples_logged += AUDIO_BUFFER_SIZE;
        
        // 4. MARK consumed (critical!)
        clear_audio_ready();
    }
}

// Create in main:
// xTaskCreate(audio_logger_task, "Logger", 2048, NULL, 2, NULL);
```

---

## Global Configuration

Via [src/microphone_config.h](src/microphone_config.h):

```c
#define AUDIO_BUFFER_SIZE              528     // Samples per buffer (11 ms)
#define PCM_SAMPLE_RATE_HZ            48000   // Output rate
#define PDM_SAMPLE_RATE_HZ          3072000   // Input clock
#define DECIMATION_RATIO                64    // PDM→PCM decimation

#define AUDIO_FILTER_LP_HZ          10000.0f  // Low-pass frequency
#define AUDIO_FILTER_HP_HZ             50.0f  // High-pass frequency
#define AUDIO_FILTER_MAX_VOLUME        200    // Volume scaling
#define AUDIO_FILTER_GAIN               16    // Additional gain
```

**Modify these to customize audio capture behavior.**

---

## Hardware Configuration

Via microphone_task() → initialize_pdm_clock():

```c
#define MIC_CLK_PIN     7      // GPIO7 output (PDM clock generation)
#define MIC_DATA_PIN    6      // GPIO6 input  (PDM data)
#define MIC_PIO_NUM     0      // PIO0
#define MIC_SM          2      // State machine 2
```

---

## Synchronization Mechanism

### Producer (Microphone Task)

```
DMA complete
  ↓
ISR fires
  ├─ Swap buffer pointers
  ├─ Re-trigger DMA (pipelining)
  └─ Notify task
  ↓
Task wakes
  ├─ Filter PDM buffer
  ├─ Accumulate PCM samples
  ├─ When full: xSemaphoreGive(g_audioReadySemaphore)
  └─ Loop
```

### Consumer (Your Task)

```
xSemaphoreTake(g_audioReadySemaphore, timeout)
  ↓
Blocked until semaphore posted
  ↓
Wakes when audio ready
  ├─ Read g_audioReady → which buffer
  ├─ Get pointer → g_audioBuffers.bufferN
  ├─ Process audio
  └─ clear_audio_ready()
  ↓
Repeat
```

---

## Error Handling

### Common Issues

**Issue: Timeout waiting for audio**
```c
if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
    printf("Audio not ready\n");
    // Microphone task may not be running
    // Check: Is microphone_task() created?
    // Check: Are GPIO6/7 wired to microphone?
}
```

**Issue: Reading stale buffer**
```c
// Wrong - buffer might be partially filled
int16_t sample = g_audioBuffers.buffer1[0];

// Right - wait for notification first
xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000));
if (get_audio_ready() == 1) {
    int16_t sample = g_audioBuffers.buffer1[0];  // Safe
}
```

**Issue: Buffer overrun**
```c
// Slow consumer - microphone fills both buffers before consumer processes
// Solution: Optimize consumer or reduce processing time
// Check: g_audio_samples_dropped > 0  (in udp_audio_server.c)
```

---

## Performance Characteristics

| Metric | Value |
|--------|-------|
| **Sample Rate** | 48,000 Hz |
| **Buffer Size** | 528 samples (11 ms) |
| **Buffer Fill Rate** | ~11 ms |
| **Semaphore Latency** | <1 µs |
| **Memory (Audio)** | 2 KB (both buffers) |
| **CPU (Mic Task)** | ~5-10% when active |

---

## Code Examples

### Simple RMS Calculator

```c
#include "microphone.h"
#include <math.h>

void audio_meter_task(void *param) {
    while (1) {
        if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000)) != pdTRUE) 
            continue;
        
        uint8_t buf_id = get_audio_ready();
        int16_t *audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                        : g_audioBuffers.buffer2;
        
        // Calculate RMS
        int64_t sum_sq = 0;
        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
            int32_t s = audio[i];
            sum_sq += s * s;
        }
        float rms = sqrt((float)sum_sq / AUDIO_BUFFER_SIZE);
        printf("RMS: %.1f\n", rms);
        
        clear_audio_ready();
    }
}
```

### Circular Buffer (Ring Buffer)

```c
#define RING_SIZE (3 * AUDIO_BUFFER_SIZE)
int16_t ring[RING_SIZE];
int ring_head = 0;

void ring_feeder_task(void *param) {
    while (1) {
        if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue;
        
        uint8_t buf_id = get_audio_ready();
        int16_t *audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                        : g_audioBuffers.buffer2;
        
        // Copy to ring
        memcpy(&ring[ring_head], audio, AUDIO_BUFFER_SIZE * sizeof(int16_t));
        ring_head = (ring_head + AUDIO_BUFFER_SIZE) % RING_SIZE;
        
        // Signal consumer
        xSemaphoreGive(ring_ready_sem);
        clear_audio_ready();
    }
}
```

---

## Related Files

- [microphone.h](src/microphone.h) - Header with declarations
- [microphone.c](src/microphone.c) - Implementation
- [microphone_config.h](src/microphone_config.h) - Configuration
- [MICROPHONE_INTEGRATION.md](MICROPHONE_INTEGRATION.md) - Integration guide
- [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) - System architecture
