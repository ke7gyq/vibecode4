# PDM Microphone API Quick Reference

## Overview
Complete PDM-to-PCM microphone system with ping-pong buffers, PIO/DMA hardware acceleration, and FreeRTOS synchronization.

---

## Global Variables

### Audio Status Flag
```c
extern volatile uint8_t g_audioReady;
```
- **Value 0**: No buffer ready
- **Value 1**: Audio ready in `buffer1`
- **Value 2**: Audio ready in `buffer2`

**Always check this variable after `xSemaphoreTake()` to know which buffer to process.**

### Audio Buffers
```c
extern AudioBuffers_t g_audioBuffers;

// Structure definition:
typedef struct {
    int16_t buffer1[1024];
    int16_t buffer2[1024];
} AudioBuffers_t;
```

Each buffer: 1024 int16_t samples (~16-32 ms of audio at 62.5 kHz PCM rate)

### Synchronization Semaphore
```c
extern SemaphoreHandle_t g_audioReadySemaphore;
```

Binary semaphore posted by microphone task when buffer is ready.

---

## API Functions

### Initialize System
```c
bool microphone_init(void);
```
- Usually called from microphone_task automatically
- Sets up PIO state machine, DMA, buffers, and semaphore
- Returns `true` on success, `false` on failure
- Called once at startup

### Get Buffer Status
```c
uint8_t get_audio_ready(void);
```
- Returns 0, 1, or 2 (which buffer is ready)
- Safe from interrupt context
- Check immediately after `xSemaphoreTake()`

### Clear Buffer Status
```c
void clear_audio_ready(void);
```
- **MUST** call after consuming buffer
- Tells microphone task buffer has been processed
- Allows microphone to overwrite with new data

### Consumer Synchronization
```c
BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore,
                          TickType_t xTicksToWait);
```
FreeRTOS function to wait for audio buffer:
```c
if (xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
    // Buffer is ready - read g_audioReady
} else {
    // Timeout - no buffer available for 5 seconds
}
```

### PDM-to-PCM Conversion (Advanced)
```c
uint16_t process_pdm_to_pcm(const uint32_t *p_pdm_data,
                            uint16_t num_pdm_words,
                            int16_t *p_pcm_buffer,
                            uint16_t *p_pcm_index);
```

Process raw PDM data from DMA:
- Input: PDM data array (uint32_t, 32 bits per word)
- Output: PCM samples (int16_t) written to buffer
- Updates `*p_pcm_index` with new position
- Returns number of PCM samples generated

Example:
```c
uint16_t samples = process_pdm_to_pcm(dma_buffer, 32, pcm_out, &pcm_idx);
```

---

## Typical Consumer Pattern

### Minimal Lock-Free Pattern
```c
#include "microphone.h"
#include "FreeRTOS.h"
#include "semphr.h"

void my_audio_consumer(void *param) {
    while (1) {
        // 1. Block until audio ready (5 second timeout)
        if (xSemaphoreTake(g_audioReadySemaphore, 
                          pdMS_TO_TICKS(5000)) != pdTRUE) {
            printf("No audio (timeout)\n");
            continue;
        }
        
        // 2. Check which buffer is ready
        uint8_t buf_id = get_audio_ready();
        if (buf_id == 0) {
            printf("ERROR: No buffer marked ready\n");
            continue;
        }
        
        // 3. Get pointer to correct buffer
        int16_t *p_audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                          : g_audioBuffers.buffer2;
        
        // 4. Process audio (safe - microphone writes to OTHER buffer)
        for (int i = 0; i < 1024; i++) {
            int16_t sample = p_audio[i];
            // ... do something with sample ...
        }
        
        // 5. Signal done - CRITICAL!
        clear_audio_ready();
    }
}

// Create task in main():
xTaskCreate(my_audio_consumer, "Consumer", 2048, NULL, 2, NULL);
```

### With Mutex (For Multiple Consumers)
```c
static SemaphoreHandle_t g_audio_mutex = NULL;

void create_multi_consumer(void) {
    g_audio_mutex = xSemaphoreCreateMutex();
    xTaskCreate(consumer_task_1, "Consumer1", 2048, NULL, 2, NULL);
    xTaskCreate(consumer_task_2, "Consumer2", 2048, NULL, 2, NULL);
}

void consumer_task_1(void *param) {
    while (1) {
        xSemaphoreTake(g_audioReadySemaphore, pdMS_TO_TICKS(5000));
        
        // Lock buffer access
        xSemaphoreTake(g_audio_mutex, pdMS_TO_TICKS(100));
        {
            uint8_t buf_id = get_audio_ready();
            int16_t *p_audio = (buf_id == 1) ? g_audioBuffers.buffer1 
                                              : g_audioBuffers.buffer2;
            
            // Copy for independent processing
            int16_t local[1024];
            memcpy(local, p_audio, 2048);
        }
        xSemaphoreGive(g_audio_mutex);
        
        clear_audio_ready();
        
        // Now process local copy at own pace...
    }
}
```

---

## Configuration

### Change PDM Clock Rate
In `microphone.c`, modify `pio_init_pdm()`:
```c
// Default: ~4 MHz PDM clock
sm_config_set_clkdiv(&config, 31.25f);

// Other options:
// 62.5     → ~2 MHz PDM → 31.25 kHz PCM
// 15.625   → ~8 MHz PDM → 125 kHz PCM  
// 125.0    → ~1 MHz PDM → 15.6 kHz PCM
```

### Change Pin Assignments
In `microphone.h`:
```c
#define MIC_CLK_PIN     6      // PDM clock output here
#define MIC_DATA_PIN    7      // PDM data input here
```

Rebuild project after changing.

### Change Buffer Size
In `microphone.h`:
```c
#define AUDIO_BUFFER_SIZE 1024  // Samples per buffer
```

Rebuild project after changing.

---

## Error Handling

### Check for Timeouts
```c
BaseType_t result = xSemaphoreTake(g_audioReadySemaphore, 
                                   pdMS_TO_TICKS(1000));
if (result != pdTRUE) {
    printf("ERROR: Audio buffer timeout\n");
    // Microphone task might be stuck or CPU overloaded
}
```

### Verify Initialization
```c
if (g_audioReadySemaphore == NULL) {
    printf("ERROR: Semaphore not created\n");
}
```

### Check for Stuck Buffer
```c
// If g_audioReady stays non-zero for >200ms, it's stuck
if (g_audioReady != 0) {
    printf("WARNING: Buffer %d not consumed\n", g_audioReady);
}
```

---

## Real-Time Audio Processing Example

### Calculate RMS Level
```c
int32_t calculate_rms(const int16_t *p_samples, int count) {
    int64_t sum_sq = 0;
    for (int i = 0; i < count; i++) {
        int32_t s = p_samples[i];
        sum_sq += s * s;
    }
    return (int32_t)sqrt(sum_sq / count);
}
```

Usage:
```c
uint8_t buf_id = get_audio_ready();
int16_t *p = (buf_id == 1) ? g_audioBuffers.buffer1 
                            : g_audioBuffers.buffer2;
int32_t rms = calculate_rms(p, 1024);
printf("RMS = %d (range: 0-32767)\n", rms);
```

### Simple Low-Pass Filter
```c
#define FILTER_COEFF 0.9f  // 0.0-1.0, higher = more filtering

int16_t filtered = 0;
for (int i = 0; i < 1024; i++) {
    filtered = (int16_t)(FILTER_COEFF * filtered + 
                        (1.0f - FILTER_COEFF) * p_audio[i]);
    // Use filtered value...
}
```

### Peak Detection
```c
int16_t peak = 0;
for (int i = 0; i < 1024; i++) {
    int16_t abs_val = (p_audio[i] < 0) ? -p_audio[i] : p_audio[i];
    if (abs_val > peak) {
        peak = abs_val;
    }
}
printf("Peak = %d (0-32767 is full scale)\n", peak);
```

---

## Integration Checklist

- [x] System compiles
- [x] Microphone task created
- [x] PIO/DMA configured
- [x] Ping-pong buffers allocated
- [ ] Consumer task created
- [ ] Hardware connected (GPIO 6, GPIO 7)
- [ ] Output verified (audio levels make sense)
- [ ] Performance acceptable (no CPU overload)

---

## Signal Flow

```
Hardware
┌────────────────────────────────────────────┐
│ ATSAMD21 Microphone                        │
│ - Generates PDM clock                      │
│ - Outputs 1-bit PDM data                   │
└────────────┬────────────────────┬──────────┘
             │ CLK                │ DATA
             v                    v
          GPIO 6              GPIO 7 (RP2350)
             │                    │
             └────────────────────┘
                      │
             PIO State Machine 0
                      │
             ┌────────┴────────┐
             │ PIO RX FIFO     │
             └────────┬────────┘
                      │
                  DMA Ch. 0
                      │
             g_dma_buffer[256]
                      │
        ┌─────────────┴─────────────┐
        │ Microphone Task            │
        │ - process_pdm_to_pcm()    │
        │ - Fill buffer1 or buffer2 │
        │ - Post semaphore          │
        │ - Update g_audioReady     │
        └─────────────┬─────────────┘
                      │
             ┌────────┴────────┐
             │ xSemaphoreGive │
             └────────┬────────┘
                      │
        ┌─────────────┴─────────────┐
        │ Consumer Task (YOUR CODE)  │
        │ - xSemaphoreTake()         │
        │ - check g_audioReady       │
        │ - process buffer           │
        │ - clear_audio_ready()      │
        └────────────────────────────┘
```

---

## Performance Notes

### Latency
- **Capture to buffer**: ~16-32 ms (buffer fill time at 62.5 kHz)
- **Producer-consumer**: <1 ms jitter (PIO/DMA is hardware)
- **Total**: ~16-50 ms end-to-end depending on processing

### Memory Usage
```
Buffers:              2 × 1024 × 2 bytes = 4 KB
DMA intermediate:     256 × 4 bytes = 1 KB
Total audio memory:   ~5 KB
```

### CPU Usage
- **PIO + DMA**: ~0% (hardware handles it)
- **Microphone Task**: ~1-2% (PDM-to-PCM conversion)
- **Consumer Task**: Depends on your processing (typically 5-50%)

---

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| Consumer never wakes up | Microphone task not running | Check printf output; verify init succeeded |
| All zeros in buffer | Microphone not connected | Check GPIO 6 & 7 connections |
| Audio very noisy | Electrical noise on wires | Use shielded cable, shorter wires |
| Audio clipped | Signal too loud for PDM | Reduce microphone gain or add attenuator |
| CPU overload | Consumer processing too slow | Reduce processing, increase priority |
| Buffer stuck (g_audioReady != 0) | Consumer crashed or busy-waiting | Add timeout logic, fix consumer loop |

---

## Files

- [microphone.h](src/microphone.h) - Header with API
- [microphone.c](src/microphone.c) - Implementation
- [audio_consumer.c](src/audio_consumer.c) - Simple example
- [audio_dsp_example.c](src/audio_dsp_example.c) - Advanced example
- [MICROPHONE_README.md](MICROPHONE_README.md) - Full documentation

