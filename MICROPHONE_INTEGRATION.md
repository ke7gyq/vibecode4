# Microphone Audio System Integration Guide

## Quick Start

The PDM microphone system is fully integrated and ready to use. Audio buffers are filled automatically by the microphone task and signaled via semaphore.

### Basic Consumer Pattern

```c
#include "microphone.h"

void my_audio_consumer_task(void *param) {
    (void)param;
    
    while (1) {
        // 1. Wait for audio buffer (timeout = 1 second)
        BaseType_t result = xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000));
        
        if (result != pdTRUE) {
            printf("Timeout waiting for audio\n");
            continue;
        }
        
        // 2. Get which buffer is ready (volatile, check immediately)
        uint8_t buffer_id = get_audio_ready();
        if (buffer_id == 0) {
            printf("Stale semaphore\n");
            continue;
        }
        
        // 3. Get audio buffer pointer (safe - microphone writing to OTHER buffer)
        int16_t *audio_buffer = (buffer_id == 1) ? g_audioBuffers.buffer1 
                                                   : g_audioBuffers.buffer2;
        
        // 4. Process 528 int16_t samples (11 ms of audio at 48 kHz)
        process_audio_chunk(audio_buffer, AUDIO_BUFFER_SIZE);
        
        // 5. Mark buffer consumed
        clear_audio_ready();
    }
}

// Create task in main.c:
// xTaskCreate(my_audio_consumer_task, "AudioProc", 2048, NULL, 2, NULL);
```

---

## System Architecture

### Audio Signal Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    VIBECODE4 AUDIO SYSTEM                       │
└─────────────────────────────────────────────────────────────────┘

Hardware Layer
├─ PDM Microphone (MEMS)
│  └─ Serial data (1-bit @ 3.072 MHz)
│
└─ RP2350 Microcontroller
   ├─ PIO0 SM2
   │  ├─ Output: GPIO7 (PDM clock, 3.072 MHz)
   │  └─ Input: GPIO6 (PDM data)
   │
   └─ DMA Channel 0 (Ping-Pong)
      ├─ Transfer: PDM FIFO → RAM (96 words = 384 bytes)
      ├─ Trigger: PIO SM2 data available
      ├─ ISR: Posts task notification + re-triggers OTHER buffer
      └─ Period: ~1 ms per buffer


Software Layer (FreeRTOS)
├─ Microphone Task (Priority 2)
│  ├─ Waits on DMA completion notification
│  ├─ Reads completed PDM buffer from DMA
│  ├─ Calls Open_PDM_Filter_64() for 48 PCM samples
│  ├─ Accumulates samples in output buffer
│  ├─ When full: Posts g_audioReadySemaphore
│  └─ Switches output buffer (ping-pong)
│
└─ Consumer Tasks
   ├─ Waits on g_audioReadySemaphore
   ├─ Gets audio pointer from g_audioBuffers
   ├─ Processes 528 samples (11 ms of audio)
   └─ Repeats
```

### Concurrency Model

**Key Feature: True DMA/Filter Pipelining**

```
Timeline:
─────────────────────────────────────────────────────────────

Time 0: DMA fills LUT using buffer_a
│
├─ DMA complete ISR fires
│  ├─ Clear IRQ
│  ├─ Swap pointers (g_current ↔ g_other)
│  ├─ Re-trigger DMA immediately for buffer_b ← PIPELINING!
│  └─ Notify task
│
├─ Microphone task wakes
│  ├─ Sync pointers from globals (already swapped by ISR)
│  ├─ Filter buffer_a [task doing work]
│  │
│  └─ DMA fills buffer_b [CONCURRENT with filtering!]
│
Time ~1ms: Filtering done
│
└─ Task loops: re-read globals, process buffer_b next

Result: ~1 ms DMA operation overlaps with filtering
        No idle time waiting for DMA between iterations
```

### Pin Configuration

| GPIO | Direction | Usage | Speed |
|------|-----------|-------|-------|
| 6 | Input | PDM Data | 3.072 MHz |
| 7 | Output | PDM Clock | 3.072 MHz |

Both routed through PIO0 State Machine 2

---

## API Reference

### Data Structures

```c
// ===== Audio Buffers =====
typedef struct {
    int16_t buffer1[AUDIO_BUFFER_SIZE];   // 528 samples (11 ms @ 48 kHz)
    int16_t buffer2[AUDIO_BUFFER_SIZE];   // 528 samples
} AudioBuffers_t;

extern AudioBuffers_t g_audioBuffers;
```

### Global Variables

```c
// ===== Status =====
extern volatile uint8_t g_audioReady;            // 0=none, 1=buffer1, 2=buffer2
extern AudioBuffers_t g_audioBuffers;            // Audio samples (int16_t)
extern SemaphoreHandle_t g_audioReadySemaphore;  // Binary semaphore

// ===== Configuration =====
extern uint8_t g_micDebug;  // 0=silent, 2+=verbose, 4+=stats
```

### Initialization

```c
bool microphone_init(void);
```
- Called automatically by microphone_task()
- Creates semaphore, initializes PIO, configures DMA
- Starts DMA channel
- Returns: true on success

### Status Functions

```c
uint8_t get_audio_ready(void);
```
- Returns which buffer has ready audio
- Return values: 0 (none), 1 (buffer1), 2 (buffer2)

```c
void clear_audio_ready(void);
```
- Marks current audio buffer as consumed
- Call after you finish processing

### Consumer Task Entry

```c
void microphone_task(void *parameters);
```
- Main audio capture and conversion task
- Created by application
- Runs indefinitely: capture → filter → fill → notify → repeat

---

## Consumer Task Patterns

### Pattern 1: Simple Linear Processing

```c
void audio_processor_task(void *param) {
    while (1) {
        if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
            uint8_t buf_id = get_audio_ready();
            int16_t *audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                            : g_audioBuffers.buffer2;
            
            // Process all 528 samples
            for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
                int16_t sample = audio[i];
                // ... do work on sample ...
            }
            
            clear_audio_ready();
        }
    }
}
```

### Pattern 2: Streamed Output (UDP)

```c
void udp_audio_streamer(void *param) {
    while (1) {
        if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
            uint8_t buf_id = get_audio_ready();
            int16_t *audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                            : g_audioBuffers.buffer2;
            
            // Send 528 samples over network
            send_audio_frame(audio, AUDIO_BUFFER_SIZE);
            
            clear_audio_ready();
        }
    }
}
```

### Pattern 3: Ring Buffer with Processing Lag

```c
#define RING_BUFFER_SIZE (3 * AUDIO_BUFFER_SIZE)  // 3 buffers
int16_t ring_buffer[RING_BUFFER_SIZE];
int ring_write_index = 0;

void ring_buffer_feeder(void *param) {
    while (1) {
        if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
            uint8_t buf_id = get_audio_ready();
            int16_t *audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                            : g_audioBuffers.buffer2;
            
            // Copy to ring buffer
            memcpy(&ring_buffer[ring_write_index], audio, 
                   AUDIO_BUFFER_SIZE * sizeof(int16_t));
            ring_write_index = (ring_write_index + AUDIO_BUFFER_SIZE) % RING_BUFFER_SIZE;
            
            // Signal processor
            xSemaphoreGive(ring_buffer_ready_semaphore);
            
            clear_audio_ready();
        }
    }
}
```

---

## Configuration

All audio parameters configured in [src/microphone_config.h](src/microphone_config.h):

```c
#define PDM_SAMPLE_RATE_HZ    3072000    // 3.072 MHz PDM clock
#define DECIMATION_RATIO      64         // 64:1 decimation
#define PCM_SAMPLE_RATE_HZ    48000      // Output rate
#define AUDIO_BUFFER_SIZE     528        // Samples per buffer (11 ms)
#define AUDIO_FILTER_LP_HZ    10000.0f   // Low-pass frequency
#define AUDIO_FILTER_HP_HZ    50.0f      // High-pass frequency
```

Modify these to change audio capture behavior.

---

## Debugging & Diagnostics

### Enable Verbose Output

```c
g_micDebug = 4;  // Maximum verbosity
```

**Output Examples:**
```
[Mic] Buffer 1 full (528 samples, 42 total)
[UDP] Sending buffer 1
[MicStats] Rate: 48000 Hz, Total: 22176 samples, Buffers: 42
[AudioRate] TX: 48000 Hz, DROP: 0 Hz (90.9 frames/sec)
```

### Monitor Statistics

```c
printf("Audio Ready: %d\n", g_audioReady);
printf("Buffer1[0]: %d (first sample)\n", g_audioBuffers.buffer1[0]);
```

### Check Semaphore

```c
// Inside your consumer task
if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(50)) == pdFAIL) {
    printf("No audio available (timeout)\n");
}
```

---

## Performance Characteristics

### Timing

| Operation | Time | Notes |
|-----------|------|-------|
| DMA Fill | ~1 ms | 384 bytes @ 3.072 MHz parallel input |
| Filter 48 samples | ~50-100 µs | Open_PDM_Filter_64 call |
| 11 filter calls | ~500-1100 µs | Per buffer accumulation |
| Buffer switch latency | <1 µs | Atomic pointer swap |
| Semaphore post | <1 µs | From ISR |
| Semaphore take/check | <10 µs | Kernel operation |

### Memory Usage

```
DMA Buffers:     768 bytes  (2 × 96 words)
PCM Buffers:   2,048 bytes  (2 × 1024 int16_t)
Filter State:  ~500 bytes   (FIR history)
Semaphore:     ~100 bytes   (FreeRTOS handle)
─────────────────────────────
Total:        ~3.5 KB
```

### CPU Usage (Approximate)

- **Microphone Task**: 5-10% when active, idle otherwise
- **Consumer Task**: Depends on your processing
- **Both Idle**: When no buffer ready (event-driven)

---

## Troubleshooting

### Issue: Semaphore Never Fires

**Cause:** Microphone task not reaching buffer-full condition

**Debug:**
```c
g_micDebug = 2;
// Look for: [Mic] Buffer X full
// If not appearing, check DMA completion
```

**Solution:**
- Verify DMA channel allocated
- Check GPIO6/7 connected to microphone
- Verify PDM clock running (oscilloscope on GPIO7)

### Issue: Partial Buffers

**Cause:** Consumer reads buffer mid-fill

**Fix (already in code):**
- Use semaphore (guarantees complete buffers only)
- Never read g_audioBuffers directly
- Always wait on g_audioReadySemaphore first

### Issue: Audio Glitches

**Cause:** Buffer overrun (consumer too slow)

**Debug:**
```c
extern uint32_t g_audio_samples_dropped;  // Check this
printf("Dropped samples: %lu\n", g_audio_samples_dropped);
```

**Solution:**
- Reduce consumer task processing time
- Increase consumer task priority
- Use ring buffer pattern for buffering

---

## References

- [microphone.h](src/microphone.h) - Full API
- [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) - System architecture
- [HARDWARE_WIRING.md](HARDWARE_WIRING.md) - GPIO pinouts

