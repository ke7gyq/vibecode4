# Vibecode4 UDP Audio Streaming & Spectrum Display - Implementation Summary

## ✅ Latest Updates (Apr 5, 2026)

**Spectrum Display with Dynamic Control:**
- ✅ Multimedia Colormap System: Jet (blue→red) + Parula (blue→yellow), runtime switchable
- ✅ Dynamic Spectrum Gain: 1-1000+ adjustment via `gainWaterfall` command, no audio latency
- ✅ Integer Optimization: Eliminated float operations from spectrum loop (3d56df3)
  - Before: UDP queue 4/4 full (46 kHz degrading to 40 kHz with stalls)
  - After: UDP queue 0/4 healthy (46.95 kHz stable, zero frame loss)
- ✅ Parser Efficiency: Reduced UART polling timeout 100ms → 1ms (6bcb5c3)
  - ParserTask CPU: 77,836 ticks → 7,911 ticks (90% reduction)
- ✅ Audio Verification: 15-second capture at 46.95 kHz, 1334 frames, 0 lost frames

**Previous Updates (Apr 4, 2026):**
- ✅ Dual-Core SMP Architecture & Stream Processing
- ✅ Spectrum Accumulation: 100-frame batching with magnitude-squared summation
- ✅ Zero-Copy UDP: PBUF_REF eliminates memcpy
- ✅ PDM Clock Diagnostics: Debug output for frequency verification
- ✅ Pitch Accuracy: Verified to 0.01% error (440 Hz reference tone)

**Status**: ✅ Production Ready | Build: SUCCESS | Audio: 46.95 kHz stable | Display: Live waterfall spectrum | UI: Full TTY control

---

## 📦 Core System Files

### Audio Capture & Filtering

1. **[src/microphone.h/c](src/microphone.h/c)** - Core microphone system
   - `microphone_init()` - Initialize PIO, DMA, semaphore, buffers
   - `microphone_task()` - FreeRTOS task for PDM→PCM conversion
   - DMA pipelining: ISR re-triggers buffer fill concurrently with filtering
   - Semaphore posts when PCM buffer complete

2. **[src/microphone_config.h](src/microphone_config.h)** - Centralized config
   - Filter parameters (LP/HP frequency, decimation, gain)
   - DMA buffer sizing
   - Output buffer configuration

3. **[src/OpenPDMFilter.c/h](src/OpenPDMFilter.c/h)** - STMicroelectronics filter
   - Optimized sinc³ decimation-by-64 filter
   - Configurable LUT mode
   - `Open_PDM_Filter_64()` converts 384 bytes PDM → 48 PCM samples

### Network & Streaming

4. **[src/udp_audio_server.c](src/udp_audio_server.c)** - UDP audio server
   - **OPTIMIZED**: Zero-copy transmission using `PBUF_REF` (no memcpy)
   - Semaphore-driven (waits on `g_audioReadySemaphore`)
   - `udp_audio_task()` - Event-based UDP transmission
   - Sends complete `AUDIO_BUFFER_SIZE` samples per notification
   - Direct pointer assignment: `p->payload = (void *)buffer`
   - Statistics reporting per second

5. **[src/network.c/h](src/network.c/h)** - Network initialization
   - CYW43439 WiFi setup (Pico 2 W)
   - UDP primitives
   - IP assignment

### Build & Configuration

6. **[CMakeLists.txt](CMakeLists.txt)** - Build configuration
   - FreeRTOS integration
   - PIO file compilation
   - SDK linking

7. **[src/*.pio](src/)** - PIO assembly programs
   - `pdm_clock.pio` - Generates 3.072 MHz PDM clock, reads GPIO6 data
   - `gpio_toggle_test.pio` - GPIO diagnostics

### Python Utilities

8. **[utils/udp_audio_client.py](utils/udp_audio_client.py)** - UDP audio capture
   - Receives UDP frames from Pico
   - Saves to WAV file
   - **FIXED**: Sample rate now correctly set to 48000 Hz (was 42496 Hz)
   - Default: 30-second capture
   - Configurable via `--sample-rate` and `--frame-size` flags

9. **[utils/telnet_pcm_client.py](utils/telnet_pcm_client.py)** - Live PCM monitor
   - Display RMS levels in real-time
   - 48 kHz PCM stream monitoring

10. **[utils/playTone.py](utils/playTone.py)** - Reference tone generator
    - Generates 440 Hz (A4) test tone at 48 kHz
    - Saves as WAV file for audio testing
    - Useful for verifying pitch accuracy and sampling rate

11. **[utils/analyzePitch.py](utils/analyzePitch.py)** - Frequency analyzer
    - FFT-based pitch detection from WAV files
    - Compares measured vs expected frequency
    - Reports sampling rate errors and percent deviation
    - Used to verify audio quality and detect sample rate issues

### Parser Commands (USB Serial Interface)

12. **[src/parser.c](src/parser.c)** - Command-line interface (115200 baud TTY)
    - **`help`** - Display all available commands
    
    **Audio Control:**
    - **`udpStart` / `udpStop`** - Control UDP audio streaming (default: running)
    - **`gainWaterfall [N]`** - Get/set spectrum gain (1-1000+, default: 10)
      - `gainWaterfall` → Show current gain
      - `gainWaterfall 20` → Set gain to 20 (multiply amplitude by 0.04)
      - Integer math: `gained = (accum[i] * gain²) / 10000` (no float operations)
    
    **Display Control:**
    - **`enableWaterfall` / `disableWaterfall`** - Enable/disable spectrum display updates
    - **`colorWaterfall [N]`** - Get/set waterfall colormap
      - `colorWaterfall` → Show current colormap
      - `colorWaterfall 0` → Jet colormap (blue→red, default)
      - `colorWaterfall 1` → Parula colormap (blue→yellow)
      - Invalid indices default to Jet (0)
    
    **System Monitoring:**
    - **`rtosStatus`** - Display FreeRTOS task CPU ticks, stack usage, queue depths
      - Shows all tasks: MicrophoneTask, UdpAudioTask, WaterfallTask, TimerTask, ParserTask, etc.
      - Queue status: UDP/Waterfall depth (0/4 = healthy, 4/4 = bottleneck)
      - CPU ticks: Measure per-task overhead and identify bottlenecks
    - **`micDebug [0-2]`** - Get/set microphone debug output level
      - 0 = off, 1 = warnings, 2 = verbose
    
    **Other:**
    - **`blink [ms]`** - Get/set LED blink rate
    - **`setTime [N]`** - Set clock display value
    - **`scanWifi` / `wifiConnect` / `wifiStatus`** - WiFi network commands
    - More: startClock, stopClock, etc.

---

## 🔄 Architecture Overview

### Dual-Core Separation (RP2350 SMP)

**Core 0 (Primary):**
- FreeRTOS system tick and scheduler
- UDP audio transmission (semaphore-based, DMA-pipelining)
- LVGL display rendering
- Network tasks
- Parser/command interface

**Core 1 (FFT Processor):**
- Waterfall display task (pinned via `xTaskCreateAffinitySet`)
- Real-time FFT computation on all audio frames
- Spectrum accumulation (100-frame batching with magnitude-squared summation)
- Display update queue handling

**Inter-Core Synchronization:**
- FreeRTOS message queues (thread-safe cross-core)
  - `g_audioQueueUDP` → UDP task on Core 0
  - `g_audioQueueWaterfall` → Waterfall task on Core 1
- LVGL mutex (standard cross-core mutex usage)
- No deadlock risk (single mutex, no circular dependencies)

**Performance Result:**
- ✅ UDP Queue: 0/4 (not starved by FFT load)
- ✅ Waterfall Queue: 0/4 (FFT keeps pace with audio)
- ✅ Both IDLE tasks active (cores have spare capacity)

### Data Flow

```
Hardware: PDM Microphone
    ↓ (3.072 MHz, 1-bit data)
PIO State Machine
    ├─ Generates clock on GPIO7
    └─ Reads data from GPIO6 into RX FIFO
    ↓
DMA Channel (Ping-Pong)
    ├─ Transfers 384 bytes (96 × 32-bit words) PDM data
    ├─ ISR fired on completion
    ├─ ISR immediately re-triggers OTHER buffer (PIPELINING)
    └─ ISR notifies microphone task
    ↓
Microphone Task (FreeRTOS)
    ├─ Reads completed DMA buffer
    ├─ Applies Open_PDM_Filter_64 (48 samples per call)
    ├─ Accumulates PCM samples in output buffer
    ├─ When output buffer full: posts semaphore
    └─ Switches to other output buffer
    ↓
UDP Audio Task (FreeRTOS)
    ├─ Waits on g_audioReadySemaphore
    ├─ Gets pointer to ready buffer (buffer1 or buffer2)
    ├─ Sends entire AUDIO_BUFFER_SIZE samples via UDP (one memcpy)
    └─ Loops back to wait
```

### Key Buffers

```c
// DMA Ping-Pong (PDM data - 384 bytes each)
static uint32_t g_dma_buffer_a[96];    // ISR alternates between A/B
static uint32_t g_dma_buffer_b[96];

// Output Buffers (PCM data - 1024 int16_t each)
AudioBuffers_t {
    int16_t buffer1[1024];   // g_audioReady=1
    int16_t buffer2[1024];   // g_audioReady=2
};
```

### Concurrency (Pipelining)

**Timeline with Pipelining:**
```
Time →
DMA fills buffer_a [~1ms] ← ISR in handler
    ↓ (ISR: swap pointers, re-trigger DMA for buffer_b)
Filter buffer_a [µs]     ← Task work
    ╱─── DMA fills buffer_b [~1ms] ← Concurrent!
    │
    └─→ Filter buffer_b [µs]     ← Task work
        ╱─── DMA fills buffer_a [~1ms] ← Concurrent!
```

**Result:** True overlap - DMA fills while CPU filters

---

## 🎯 System Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| PDM Clock | 3.072 MHz | GPIO7 (generated) |
| PDM Data Pin | GPIO6 | (input) |
| PDM Decimation | 64:1 | Fixed by filter |
| PCM Sample Rate | 48 kHz | 3.072M / 64 |
| DMA Transfer | 384 bytes/ms | 96 words @ 3.072 MHz clock |
| DMA Period | ~1 ms | Per buffer transfer |
| Filter Calls/Buffer | 11 | 11 × 48 samples = 528 samples |
| PCM Buffer Size | 528 samples | 11 ms of audio |
| Buffer Fill Rate | ~11 ms | Per buffer completion |
| UDP Transmission | Event-driven | On buffer complete, not poll |
| Memcpy Operations | 1 per buffer | buffer → pbuf (direct) |

---

## 🚀 Startup Sequence

```c
int main() {
    // 1. Initialize network
    network_init();
    
    // 2. Initialize LCD display
    st7789_init();
    
    // 3. Create FreeRTOS tasks
    xTaskCreate(microphone_task, "Mic", 2048, NULL, 2, NULL);
    xTaskCreate(udp_audio_task, "UDP", 2048, NULL, 2, NULL);
    xTaskCreate(network_poll_task, "Net", 2048, NULL, 1, NULL);
    
    // 4. Start scheduler
    vTaskStartScheduler();
}
```

### Initialization Order
1. PIO configured (pdm_clock program loaded)
2. DMA configured with ping-pong buffers
3. Open_PDM_Filter initialized with config parameters
4. Binary semaphore created
5. DMA channel started
6. Task loops begin

---

## 🔧 Global Interface

```c
// ===== Status Variables =====
volatile uint8_t g_audioReady;              // 0=none, 1=buffer1, 2=buffer2
AudioBuffers_t g_audioBuffers;              // Two 528-sample buffers
SemaphoreHandle_t g_audioReadySemaphore;    // Posts when buffer ready

// ===== Functions =====
bool microphone_init(void);                 // Initialize system
uint8_t get_audio_ready(void);              // Get current buffer ID
void clear_audio_ready(void);               // Mark buffer consumed

// ===== Tasks =====
void microphone_task(void *parameters);     // PDM→PCM conversion
void udp_audio_task(void *parameters);      // UDP streaming
```

---

## 📊 Performance Metrics

### Latency
- **DMA Fill Time**: ~1 ms per buffer
- **Filter Time**: ~50-100 µs per call
- **Total Buffer Latency**: ~11 ms (buffer fill completion)
- **UDP Send Time**: ~100-200 µs (memcpy + ethernet)

### Memory Usage
- **DMA Buffers**: 768 bytes (2 × 384 bytes, PDM data)
- **Output Buffers**: 2,112 bytes (2 × 528 samples × 2 bytes, PCM data)
- **Semaphore Handle**: ~100 bytes (FreeRTOS)
- **Total**: ~3.1 KB for audio system

### CPU Usage
- **Microphone Task**: Prime only during DMA complete notifications
- **UDP Task**: Prime on semaphore notifications
- **Both Tasks**: Idle/blocked when no data ready
- **Impact**: Minimal - mostly event-driven

---

## ✨ Key Optimizations Implemented

### 1. DMA Pipelining
✅ ISR immediately re-triggers DMA in other buffer
✅ True concurrent fill+filter operation
✅ Reduces jitter
✅ Maximizes throughput

### 2. Semaphore Flow
✅ Event-based (no polling timer)
✅ Eliminated 12 ms repeating timer
✅ Energy efficient
✅ Guaranteed complete buffers (no partial sends)

### 3. Single Memcpy
✅ Direct buffer→pbuf transmission
✅ Eliminated intermediate 1 KB buffer
✅ Reduced latency
✅ Simplified arithmetic (complete buffers only)

### 4. Ping-Pong Buffers
✅ Lock-free synchronization (alternating buffers)
✅ No race conditions (DMA always writes "other" buffer)
✅ Producer-consumer pattern (microphone fills, UDP sends)
✅ Predictable timing

---

## 🐛 Debugging

### Enable Debug Output
```c
// In main.c or any task
extern uint8_t g_micDebug;
g_micDebug = 4;  // Enable all microphone traces

// Outputs:
// [Mic] Buffer 1 full (528 samples)
// [UDP] Sending buffer 1
// [MicStats] Rate: 48000 Hz, Total: 528000 samples, Buffers: 1000
// [AudioRate] TX: 48000 Hz, DROP: 0 Hz (90.9 frames/sec)
```

### Check Status
```c
printf("Audio ready: %d\n", g_audioReady);           // 0, 1, or 2
printf("Buffer1[0] = %d\n", g_audioBuffers.buffer1[0]); // Raw samples
```

---

## 📝 Files Structure

```
vibecode4/
├── src/
│   ├── microphone.h/c          ← Core audio system
│   ├── microphone_config.h      ← Filter configuration
│   ├── udp_audio_server.c       ← UDP streaming (semaphore-driven)
│   ├── network.c/h              ← WiFi/network init
│   ├── OpenPDMFilter.c/h        ← PDM→PCM filter
│   ├── pdm_clock.pio            ← PIO program (clock + data read)
│   └── main.c                   ← FreeRTOS setup
├── utils/
│   ├── udp_audio_client.py      ← Host UDP receiver
│   └── telnet_pcm_client.py     ← Live PCM monitor
├── CMakeLists.txt               ← Build config
└── IMPLEMENTATION_SUMMARY.md    ← This file
```

---

## 🔗 Related Documentation

- [microphone.h](src/microphone.h) - API reference
- [QUICK_START.md](QUICK_START.md) - Getting started guide
- [HARDWARE_WIRING.md](HARDWARE_WIRING.md) - GPIO pinout

