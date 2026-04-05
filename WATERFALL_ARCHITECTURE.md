# Waterfall Audio Visualization Architecture

## Overview

The waterfall display is a real-time audio spectrum visualization system for the RP2350 microcontroller. It processes live audio from the PDM microphone, performs FFT analysis, and displays a continuously-scrolling frequency spectrum using a Parula colormap.

**Key Features:**
- Real-time 256-point RFFT processing (16 frequency bins)
- FreeRTOS task-based architecture with event-driven updates
- LVGL canvas-based pixel-perfect rendering
- Logarithmic amplitude scaling (perceptually uniform levels 0-15)
- Thread-safe LVGL display updates via mutex

## System Architecture

### High-Level Data Flow

```
PDM Microphone
     ↓
Audio Buffer (64 samples)
     ↓
microphone_task
     ↓ (sets g_audioReadySemaphore)
     ├─→ UDP Audio Task (streams audio over network)
     └─→ Waterfall Task (processes audio for display)
             ↓
         Spectrogram (FFT Processor)
             ↓ (256-point RFFT → 16 bins)
         Amplitude Levels (0-15)
             ↓
         Parula Colormap (16 colors)
             ↓
         LVGL Canvas (320×240 display)
             ↓
         Screen Update
```

### Component Responsibilities

#### `waterfall.c/h` - Display Module
- **Canvas Initialization**: Creates LVGL canvas widget (320×240)
- **Task Management**: Creates, maintains, and destroys FreeRTOS waterfall task
- **Display Update**: Implements horizontal scroll and column rendering
- **Color Mapping**: Converts amplitude levels to RGB565 colors

**Key Data Structures:**
```c
typedef struct {
    uint32_t magnitude_sq[16];    // FFT magnitudes for 16 frequency bins
} t_waterfallBar;

typedef struct {
    uint8_t r, g, b;             // RGB color components
} ParulaColor;
```

#### `spectrogram.c/h` - FFT Processor
- **FFT Initialization**: Sets up CMSIS-DSP 256-point RFFT engine
- **Window Application**: Applies Hanning window to input samples
- **Frequency Binning**: Converts 256 FFT output bins to 16 frequency bands
- **Amplitude Extraction**: Returns 0-15 amplitude levels for each bin

**Key Functions:**
- `spectrogram_init()` - Initialize FFT engine
- `spectrogram_compute_fft()` - Perform 256-point RFFT
- `spectrogram_compute_bins()` - Bin outputs to 16 frequency levels
- `spectrogram_get_bin()` - Retrieve amplitude for specific frequency band

#### `microphone.c` - Audio Input
- **PDM Capture**: Reads audio samples from PDM microphone
- **Buffer Management**: Maintains 64-sample sliding window
- **Semaphore Signaling**: Sets `g_audioReadySemaphore` when buffer ready

#### `parser.c` - Command Interface
- **Enable/Disable Commands**: `enableWaterfall`, `disableWaterfall`
- **Task Control**: Triggers display initialization and cleanup
- **Manual Testing**: Optional test command for development (disabled in production)

#### `main.c` - System Initialization
- **Spectrogram Setup**: Initializes FFT processor once at startup
- **Waterfall Attachment**: Connects spectrogram context to waterfall module
- **Task Creation**: Starts FreeRTOS scheduler with all tasks

## Task Synchronization

### Audio Ready Semaphore

The waterfall task is event-driven, waking when the microphone has a new audio buffer ready:

```
microphone_task:
    [Capture 64 audio samples from PDM]
    xSemaphoreGive(g_audioReadySemaphore)
    // Both UDP and waterfall tasks wake independently

waterfall_task:
    xSemaphoreTake(g_audioReadySemaphore, 1000ms_timeout)
    [Process audio through spectrogram]
    [Update display]
    [Loop back to wait]
```

**Synchronization Characteristics:**
- **Binary Semaphore**: One signal per audio buffer
- **Non-Blocking**: Multiple tasks can wait on same semaphore
- **Priority Inversion Safe**: UDP task (priority 2) and waterfall task (priority 2) have equal priority
- **Timeout Protection**: 1-second timeout prevents indefinite blocking

### LVGL Mutex Protection

All LVGL display updates from the waterfall task are protected by a mutex to prevent corruption from concurrent access with the LVGL timer task:

```c
waterfall_task:
    xSemaphoreTake(g_LvglMutex, 100ms_timeout)
    {
        waterfall_add_column(canvas, &bar)  // LVGL canvas operations
    }
    xSemaphoreGive(g_LvglMutex)
```

### Task Configuration

| Task | Priority | Stack | Wake Condition |
|------|----------|-------|---|
| timer_task | tskIDLE_PRIORITY + 4 | 1024 | 33ms interval (LVGL handler) |
| **waterfall_task** | **tskIDLE_PRIORITY + 2** | **2048** | **g_audioReadySemaphore** |
| udp_audio_task | tskIDLE_PRIORITY + 2 | 2048 | g_audioReadySemaphore |
| microphone_task | tskIDLE_PRIORITY + 3 | 2048 | DMA complete (PDM) |
| parser_task | tskIDLE_PRIORITY + 1 | 2048 | USB data available |

## Amplitude and Color Mapping

### Logarithmic Scaling

Audio signals span many orders of magnitude (quietest to loudest). To display this range on a 16-level display, a logarithmic mapping is used:

```c
// Power thresholds (log scale from 1 to 100,000)
static const uint32_t power_threshold_log_lut[16] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000,
    2000, 5000, 10000, 20000, 50000, 100000
};

// Conversion function
uint8_t amplitude_level = 0;
for (level = 15; level > 0; level--) {
    if (magnitude_sq >= power_threshold_log_lut[level]) {
        amplitude_level = level;
        break;
    }
}
```

**Perceptual Benefit**: Each step represents approximately 3dB (log₁₀ ratio of 2), matching human hearing perception.

### Parula Colormap

16-level discrete Matlab Parula colormap provides intuitive color progression:

| Level | Color | Hex RGB | Description |
|-------|-------|---------|---|
| 0 | Deep Blue | 1F458B | Silence/noise floor |
| 1-3 | Teal→Cyan | - | Low amplitude |
| 4-6 | Green→Yellow-Green | - | Mid amplitude |
| 7-9 | Yellow→Orange | - | Strong signal |
| 10-15 | Orange→Pale Yellow | - | Peak signal |

**Advantages:**
- Perceptually uniform color progression
- Blue (low) to Yellow (high) matches video spectrogram conventions
- Accessible to color-blind viewers (blue-yellow-red contrasts)

## Display Update Cycle

### Per-Audio-Buffer Processing Flow

```
1. microphone_task signals g_audioReadySemaphore
   ↓
2. waterfall_task wakes from xSemaphoreTake()
   ↓
3. Check if spectrogram context is configured
   ↓
4. Call spectrogram_compute_fft(g_spectrogram_ctx)
   - Input: 64 new audio samples (from sliding window)
   - Processing: 256-point RFFT on windowed data
   - Output: 128 complex FFT bins (only 128 needed for real input)
   ↓
5. Call spectrogram_compute_bins(g_spectrogram_ctx)
   - Input: 128 FFT magnitude bins
   - Processing: Sum adjacent bins to create 16 frequency bands
   - Output: 16 amplitude levels (0-15)
   ↓
6. Extract amplitude levels for all 16 bands
   ↓
7. Convert amplitude levels → magnitude_sq values
   ↓
8. Create t_waterfallBar structure with magnitude_sq[16]
   ↓
9. Acquire g_LvglMutex (with 100ms timeout)
   ↓
10. Call waterfall_add_column(canvas, &bar)
    a. Shift canvas pixels left by 10 pixels
    b. Clear rightmost 10 columns to black
    c. For each of 16 frequency bands:
       - Get Parula color for amplitude level
       - Draw 15×10 pixel rectangle
    d. Invalidate canvas for LVGL redraw
   ↓
11. Release g_LvglMutex
   ↓
12. Loop back to wait for next g_audioReadySemaphore
```

### Visual Display Progression

```
Initial State (all black):
┌─────────────────────────────┐
│                             │
│                             │
│  (240 pixels tall)          │
│  16 bands × 15 pixels       │
│                             │
│                             │
│        (320 pixels wide)    │
└─────────────────────────────┘

After 1st Audio Buffer (column 0-10):
┌─────────────────────────────┐ High Freq
│[FREQ 15]                    │
│[FREQ 14]                    │
│...                          │
│[FREQ 1]                     │ Low Freq
│[FREQ 0]                     │
└─────────────────────────────┘
     ↑ 10 pixels of new data

After 32 Audio Buffers (scrolled 320 pixels):
┌─────────────────────────────┐
│        [oldest data now     │ Scrolled off left
│         falling off screen] │ as new data
│                             │ enters from right
│                             │
│   [newest spectrogram]      │
│        at right edge        │
└─────────────────────────────┘
    ← Data flows left →
```

## Configuration and Tuning

### Adjustable Parameters

**In `waterfall.h`:**
```c
#define WATERFALL_WIDTH              320     // Display width
#define WATERFALL_HEIGHT             240     // Display height
#define WATERFALL_FREQ_BANDS         16      // Number of frequency bins displayed
#define WATERFALL_BAR_HEIGHT         15      // Pixels per frequency band (240/16)
#define WATERFALL_X_GRANULARITY      10      // Pixels scrolled per audio buffer
```

**Impact of WATERFALL_X_GRANULARITY:**
- 10 pixels: Default, smooth animation, fills screen in 32 buffers (~1 second at 32 fps)
- Smaller (e.g., 5): Smoother scroll but more CPU load (more frequent redraws)
- Larger (e.g., 20): Faster scroll but choppier animation

**In `waterfall.c`:**
```c
power_threshold_log_lut[16]:    // Log amplitude thresholds (1 to 100,000)
// Adjust these to tune color sensitivity for your audio levels
```

**In `spectrogram.h` (if available):**
```c
#define SPECTROGRAM_FFT_SIZE         256     // FFT points
#define SPECTROGRAM_INPUT_SIZE       64      // Audio samples per buffer
#define SPECTROGRAM_NUM_BINS         16      // Output bins
```

## Usage

### Enable Waterfall Display

```bash
enableWaterfall
```

This command:
1. Calls `waterfall_init()` which:
   - Clears the current LVGL screen
   - Creates a 320×240 LVGL canvas
   - Allocates pixel buffer (76.8 KB)
   - Spawns `waterfall_task` (priority 2, stack 2048 words)
   - Links with pre-initialized spectrogram processor
2. Waterfall task begins waiting for audio ready signal
3. Audio processing starts on first semaphore signal

### Disable Waterfall Display

```bash
disableWaterfall
```

This command:
1. Calls `waterfall_destroy()` which:
   - Stops `waterfall_task` via `vTaskDelete()`
   - Frees pixel buffer (76.8 KB)
   - Deletes LVGL canvas
   - Clears static variables
2. Display stops updating
3. Resources returned to heap

## Performance Characteristics

### CPU Load Per Audio Buffer
- **FFT Computation**: ~5-10ms (CMSIS-DSP optimized)
- **Binning**: ~1ms
- **Display Update**: ~2-3ms (LVGL canvas operations)
- **Total per buffer**: ~10-15ms
- **At 32 buffers/sec**: ~30-50% CPU utilization (one core)

### Memory Usage
- **Canvas Buffer**: 320 × 240 × 2 bytes = 153.6 KB (RGB565)
  - Allocated by `malloc()` during `waterfall_init()`
  - Freed during `waterfall_destroy()`
- **Waterfall Task Stack**: 2048 words = 8 KB
- **Spectrogram Context**: ~2 KB (FFT engine state)
- **Total**: ~163 KB when display is active

### Real-Time Guarantees
- Waterfall task wakes within 1-2 task ticks (~1ms)
- LVGL mutex timeout prevents missed audio buffers
- No priority inversion with UDP task (same priority)
- Graceful handling if task blocked (100ms timeout on mutex)

## Troubleshooting

### Waterfall Display Not Updating

**Symptom:** `enableWaterfall` succeeds but display stays black.

**Diagnosis Steps:**
1. Check console output: Should see "Waterfall display initialized" message
2. Check if audio is flowing: Run `enableWaterfall` while audio is being input
3. Check spectrogram initialization: Verify main() called `spectrogram_init()`

**Solutions:**
- Verify microphone input is working (check UDP audio streaming)
- Ensure `waterfall_set_spectrogram()` was called in main.c
- Check g_audioReadySemaphore is being set by microphone_task

### Display Corruption or Tearing

**Symptom:** Garbage pixels or flickering regions in waterfall.

**Likely Cause:** LVGL mutex contention or timing issue.

**Solutions:**
- Increase LVGL mutex timeout in waterfall_task:
  ```c
  // From 100ms to 200ms
  if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(200)) == pdTRUE)
  ```
- Reduce waterfall task frequency by increasing semaphore timeout
- Check for other tasks modifying LVGL display concurrently

### Task Stack Overflow

**Symptom:** Crashes or erratic behavior shortly after `enableWaterfall`.

**Diagnosis:**
- Add stack usage monitoring:
  ```c
  printf("Waterfall task stack: %u words\n", uxTaskGetStackHighWaterMark(g_waterfall_task_handle));
  ```
- Expected high water mark: ~1000 words (leaving ~1000 word margin)

**Solutions:**
- Increase task stack size in `waterfall_init()`:
  ```c
  xTaskCreate(..., 3072, ...)  // Increased from 2048
  ```
- Reduce local variable usage in waterfall_task

### Incorrect Colors or Amplitude Levels

**Symptom:** Waterfall displays but colors don't match audio intensity.

**Cause:** Log thresholds tuned for different audio level ranges.

**Solutions:**
1. Measure actual FFT magnitude_sq values:
   - Add debug output in waterfall_task:
     ```c
     printf("Bin 0: mag_sq=%lu, level=%u\n", 
            bar.magnitude_sq[0], 
            magnitude_sq_to_amplitude_level(bar.magnitude_sq[0]));
     ```
2. Adjust power_threshold_log_lut[] based on observed ranges
3. Levels should be roughly uniform across frequency bands

## Advanced: Custom Colormap

To change the Parula colormap to a different color scheme:

1. Edit `parula_colormap_16[]` array in waterfall.c:
   ```c
   static const ParulaColor parula_colormap_16[] = {
       {R1, G1, B1},   /* 0:  Rename color 0 */
       {R2, G2, B2},   /* 1:  Rename color 1 */
       ...
   };
   ```

2. Recompile: `ninja -C build`

3. Color progression should flow perceptually (blue→green→red or similar)

## Related Files

- **Source**: `src/waterfall.c`, `src/waterfall.h`
- **Dependencies**: `src/spectrogram.c/h`, `src/microphone.h`, `src/parser.c`
- **Initialization**: `src/main.c`
- **Display**: LVGL library (via CMakeLists.txt dependency)
- **Build**: `CMakeLists.txt`, `build/build.ninja`

## See Also

- [MICROPHONE_INTEGRATION.md](MICROPHONE_INTEGRATION.md) - Audio capture details
- [MICROPHONE_API_REFERENCE.md](MICROPHONE_API_REFERENCE.md) - Microphone function reference
- [HARDWARE_WIRING.md](HARDWARE_WIRING.md) - Pin configuration for PDM microphone
- [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) - Overall project architecture
