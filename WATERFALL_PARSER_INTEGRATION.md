# Waterfall Display Parser Integration Guide

## Overview

The waterfall display system has two main components:

1. **Waterfall Accumulator Pipeline** - Processes raw FFT data into colormap indices
   - Located in `spectrogram.c/h`
   - Handles frame accumulation, gain application, and log-to-colormap conversion
   
2. **Waterfall Display Driver** - Renders colormap indices to the LCD
   - Located in `waterfall.c/h` and `st7789.c/h`
   - Handles pixel expansion, hardware scrolling, and DMA transfers

This document explains:
- What parser commands are needed to initialize and control the waterfall
- How the program flow works from sensor → FFT → display
- Where parser functions fit into the data pipeline

---

## Part 1: Waterfall Initialization Flow

### Step 1: Initialize Display Hardware (Called Once at Startup)

**Function:** `waterfall_mode_init()`  
**Location:** `waterfall.c`  
**Purpose:** Set up ST7789 LCD in portrait mode with vertical hardware scrolling

```c
void waterfall_mode_init(void) {
    st7789_lcd_init();                  // Initialize SPI, GPIO, DMA
    st7789_setVerticalMode();           // Portrait mode (320×240)
    st7789_setScrollMargins(0, 0);      // No fixed margins
    clearDisplay();                      // Fill with white
}
```

**Parser Command for This:**
```c
static uint8_t fnInitWaterfall(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    printf("Initializing waterfall display...\n");
    waterfall_mode_init();
    printf("Waterfall display initialized (portrait 320×240, hardware scroll enabled)\n");
    return 0;
}

// Add to aTokens:
{"initWaterfall", "Initialize waterfall display mode", fnInitWaterfall},
```

---

### Step 2: Initialize Waterfall Accumulator (Called Once per Audio Session)

**Function:** `waterfall_accm_init()`  
**Location:** `spectrogram.c`  
**Purpose:** Reset accumulation buffers for a new audio stream

```c
int waterfall_accm_init(waterfall_accm_t *accm) {
    if (accm == NULL) return -1;
    memset(accm->accmArray, 0, sizeof(accm->accmArray));
    accm->frame_count = 0;
    accm->accumulating = 1;
    return 0;
}
```

**Parser Command for This:**
```c
// Global accumulator (at module scope, with display mutex)
static waterfall_accm_t g_waterfallAccumulator;

static uint8_t fnStartWaterfallFeed(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("Failed to acquire display mutex\n");
        return 1;
    }
    
    printf("Initializing waterfall accumulator (100 FFT frames per bar)...\n");
    waterfall_accm_init(&g_waterfallAccumulator);
    printf("Waterfall ready to receive FFT data\n");
    
    xSemaphoreGive(g_LvglMutex);
    return 0;
}

// Add to aTokens:
{"startWaterfallFeed", "Initialize waterfall for live audio stream", fnStartWaterfallFeed},
```

---

## Part 2: Real-Time Data Flow (Audio Processing Loop)

This is the **critical integration point** - where audio samples become waterfall bars.

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│ Audio Input                                                 │
│ (microphone PDM stream @ 16 kHz)                           │
└────────────────────┬────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────┐
│ Microphone Task                                             │
│ 1. Decimate PDM → PCM at 16 kHz                            │
│ 2. Assemble 256-sample frames                               │
│ 3. Emit to audio buffer queue (double-buffered)            │
└────────────────────┬────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────┐
│ Audio Processing Task (Where Waterfall Feeds)               │
│ 1. Pop audio frame from queue                               │
│ 2. Window audio (Hanning)                                   │
│ 3. Compute 256-point RFFT                                   │
│    → produces fft_output[256] complex (real, imag pairs)   │
│ 4. **Call waterfall_accm_add_fft(&accm, fft_output, 256)**│
│    → Computes |z|² for 512 frequency bins                  │
│    → Accumulates into array                                │
│    → Returns 1 when 100 frames accumulated                 │
│ 5. When ready (returns 1):                                  │
│    a. Call waterfall_accm_get_bar(&accm, colorIndices[24])│
│       → Batches 512→24 pixels                              │
│       → Applies gain                                        │
│       → Log-threshold lookup → colormap indices            │
│    b. Call addBar(colorIndices, 24)                        │
│       → Expands to 10×10 pixels                            │
│       → Hardware scroll + DMA write                        │
└─────────────────────────────────────────────────────────────┘
                     ▲
                     │
        Updates every 100 FFT frames (~1.6 seconds @ 16kHz)
```

### Example Integration in Audio Processing Task

This pseudocode shows where to integrate the accumulator calls:

```c
// In udp_audio_server.c or audio processing loop:

void audio_processing_task(void *parameters) {
    waterfall_accm_t waterfall_accm;
    waterfall_accm_init(&waterfall_accm);
    
    uint16_t colormap_indices[24];
    
    while (1) {
        // Pop audio frame from queue
        int16_t *audio_frame = queue_pop(audio_queue);
        if (audio_frame == NULL) continue;
        
        // Compute FFT
        // (assuming spectrogram_t spec already initialized)
        arm_rfft_q15(&spec.fft_instance, windowed_audio, fft_output);
        
        // ⭐ KEY INTEGRATION POINT:
        // Add FFT to waterfall accumulator
        int bar_ready = waterfall_accm_add_fft(
            &waterfall_accm,
            fft_output,
            256  // Number of complex outputs from RFFT
        );
        
        // When 100 frames accumulated, generate bar
        if (bar_ready == 1) {
            // Convert accumulated FFT to colormap indices
            waterfall_accm_get_bar(&waterfall_accm, colormap_indices);
            
            // Display the bar
            addBar(colormap_indices, 24);
            
            // Show progress
            static uint32_t bar_count = 0;
            if (++bar_count % 10 == 0) {
                printf("Waterfall: %lu bars displayed\n", bar_count);
            }
        }
    }
}
```

---

## Part 3: Parser Commands for Live Waterfall Control

### Gain Control

**Function:** `setWaterfallGain(gain_value)`  
**Location:** `spectrogram.c`  
**Purpose:** Set display brightness (multiplied by FFT magnitude)

```c
// Already implemented in parser.c:
static uint8_t fnGainWaterfall(char *rest, void *v) {
    while (*rest && isspace(*rest)) rest++;
    
    if (*rest == '\0') {
        // Get current gain - need adjustment since gain is now in spectrogram.c
        // Would be: printf("Current waterfall gain: %lu\n", getWaterfallGain());
        return 0;
    }
    
    uint32_t gain_value;
    if (sscanf(rest, "%lu", &gain_value) != 1) {
        printf("Error: gainWaterfall requires a numeric value (e.g., 10 = 1.0x)\n");
        return 1;
    }
    
    setWaterfallGain(gain_value);
    printf("Waterfall gain set to %lu (linear multiplication factor)\n", gain_value);
    return 0;
}

// Usage:
// > gainWaterfall        # Get current
// > gainWaterfall 10     # 1.0x gain
// > gainWaterfall 20     # 2.0x gain (2× brighter)
```

**Gain Normalization:**
- Input formula: `gain_linear = user_input / 10`
- Storage formula: `gain_squared = (user_input / 10)² = (user_input² / 100)`
- Default: `gain = 10` → `1.0x` linear
- Range: Try `5` (0.5x) to `50` (5.0x)

### Colormap Selection

```c
// Already implemented:
static uint8_t fnColorWaterfall(char *rest, void *v) {
    while (*rest && isspace(*rest)) rest++;
    
    if (*rest == '\0') {
        uint16_t current = getColorMap();
        printf("Current colormap: %u (0=Jet, 1=Parula)\n", current);
        return 0;
    }
    
    uint16_t map_index;
    if (sscanf(rest, "%hu", &map_index) != 1) {
        printf("Error: colorWaterfall requires colormap index (0=Jet, 1=Parula)\n");
        return 1;
    }
    
    setColorMap(map_index);
    printf("Colormap changed to %u\n", map_index);
    return 0;
}

// Usage:
// > colorWaterfall       # Get current
// > colorWaterfall 0     # Jet (blue→cyan→yellow→red)
// > colorWaterfall 1     # Parula (blue→purple→orange)
```

### Test/Debug Commands

```c
// Draw single test bar with all frequency bins at same color
static uint8_t fnDrawBar(char *rest, void *v) {
    while (*rest && isspace(*rest)) rest++;
    
    if (*rest == '\0') {
        printf("Usage: drawBar <0-15> (colormap index)\n");
        printf("  0=Highest energy (red),  15=Lowest (blue)\n");
        return 1;
    }
    
    uint16_t level;
    if (sscanf(rest, "%hu", &level) != 1 || level > 15) {
        printf("Error: level must be 0-15\n");
        return 1;
    }
    
    // Create a test array of all same color
    uint16_t test_bar[24];
    for (int i = 0; i < 24; i++) {
        test_bar[i] = level;
    }
    
    addBar(test_bar, 24);
    printf("Drew test bar with colormap index %u\n", level);
    return 0;
}

// Already implemented - draws color gradient test bars
static uint8_t fnTestWaterfallColors(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    waterfall_mode_init();
    drawTestBar(0);    // Red
    vTaskDelay(pdMS_TO_TICKS(200));
    drawTestBar(8);    // Mid-range
    vTaskDelay(pdMS_TO_TICKS(200));
    drawTestBar(15);   // Blue
    printf("Test bars drawn (red, mid, blue)\n");
    return 0;
}

// Already implemented - test RGB precision
static uint8_t fnDrawColorBars(char *rest, void *v) {
    waterfall_mode_init();
    drawColorBars();  // White, Blue, Green, Red
    printf("Color bars drawn: 8 white | 8 blue | 8 green | 8 red\n");
    return 0;
}
```

---

## Part 4: Recommended Parser Additions (Complete Checklist)

### Critical Integration Functions

Add these to `parser.c` (in addition to existing waterfall commands):

#### 1. Initialize Display Hardware
```c
{"initWaterfall", "Initialize waterfall display mode", fnInitWaterfall}
```
- Calls `waterfall_mode_init()`
- Required before any bar drawing
- Safe to call multiple times

#### 2. Initialize Accumulator
```c
{"startWaterfallFeed", "Initialize waterfall for audio stream", fnStartWaterfallFeed}
```
- Calls `waterfall_accm_init()`
- Required before audio pipeline starts feeding FFT data
- Resets frame counter

#### 3. Clear Display (Optional but Useful)
```c
{"clearWaterfall", "Clear waterfall display to white", fnClearWaterfall}
```
```c
static uint8_t fnClearWaterfall(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("Failed to acquire display mutex\n");
        return 1;
    }
    
    clearDisplay();
    printf("Waterfall display cleared\n");
    
    xSemaphoreGive(g_LvglMutex);
    return 0;
}
```

#### 4. Status Command (Optional but Useful)
```c
{"waterfallStatus", "Show waterfall accumulator state", fnWaterfallStatus}
```
```c
static uint8_t fnWaterfallStatus(char *rest, void *v) {
    (void)rest;
    (void)v;
    
    if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("Failed to acquire display mutex\n");
        return 1;
    }
    
    printf("Waterfall Status:\n");
    printf("  Gain: %lu / GAIN_NORMALIZATION (1.0x = 10)\n", g_waterfallAccumulator.frame_count == 0 ? 10 : 0);
    printf("  Colormap: %u (0=Jet, 1=Parula)\n", getColorMap());
    printf("  Frames accumulated: %lu / %d\n", 
           g_waterfallAccumulator.frame_count, WATERFALL_ACCM_FRAMES);
    printf("  Status: %s\n", 
           g_waterfallAccumulator.accumulating ? "Accumulating" : "Idle");
    
    xSemaphoreGive(g_LvglMutex);
    return 0;
}
```

---

## Part 5: Typical Usage Sequence

### At Startup (Called Once)

```bash
# Initialize display hardware
> initWaterfall
# Output: "Waterfall display initialized (portrait 320×240, hardware scroll enabled)"

# Test display is working
> drawColorBars
# Output: "Color bars drawn: 8 white | 8 blue | 8 green | 8 red"

# Set colormap preference
> colorWaterfall 0
# Output: "Colormap changed to 0"

# Set initial gain
> gainWaterfall 15
# Output: "Waterfall gain set to 15 (linear multiplication factor)"
```

### When Audio Stream Starts

```bash
# Initialize accumulator for new stream
> startWaterfallFeed
# Output: "Waterfall ready to receive FFT data"

# ⭐ At this point, audio processing task should be calling:
#    waterfall_accm_add_fft(&g_waterfallAccumulator, fft_output, 256)
#    Every new FFT output should feed into the accumulator

# After 100 FFT frames (~1.6 seconds at 16 kHz), first bar appears
# Continue calling waterfall_accm_add_fft() in audio loop
```

### Runtime Adjustments

```bash
# Adjust brightness in real-time
> gainWaterfall 30
# Output: "Waterfall gain set to 30 (linear multiplication factor)"

# Switch colormap
> colorWaterfall 1
# Output: "Colormap changed to 1"

# Check status
> waterfallStatus
# Output: Shows frame count, gain, colormap, etc.

# Draw test pattern
> drawBar 8
# Output: "Drew test bar with colormap index 8"
```

---

## Part 6: Key Data Structures and Functions

### Waterfall Accumulator Structure
```c
typedef struct {
    uint32_t accmArray[512];        // One 32-bit accumulator per frequency bin
    uint32_t frame_count;            // 0 to WATERFALL_ACCM_FRAMES
    uint8_t accumulating;            // Status flag
} waterfall_accm_t;
```

### Critical Functions

| Function | Location | Purpose | Called From |
|----------|----------|---------|-------------|
| `waterfall_mode_init()` | waterfall.c | Initialize ST7789 + clear | Parser command |
| `waterfall_accm_init()` | spectrogram.c | Reset accumulator | Parser command |
| `waterfall_accm_add_fft()` | spectrogram.c | Add 256 FFT outputs, accumulate | Audio task loop |
| `waterfall_accm_get_bar()` | spectrogram.c | Generate 24 colormap indices | Audio task (when ready) |
| `addBar()` | waterfall.c | Render bar to display | Audio task (when ready) |
| `setWaterfallGain()` | spectrogram.c | Configure gain multiplier | Parser command |
| `getWaterfallGain()` | spectrogram.c | Retrieve cached squared gain | Audio task |
| `setColorMap()` | waterfall.c | Switch color palette | Parser command |
| `getColorMap()` | waterfall.c | Retrieve current palette | Display operations |

### Constants (in spectrogram.h)

```c
#define WATERFALL_ACCM_FRAMES 100      // Frames per bar (configurable)
#define WATERFALL_FFT_BINS 512          // Output freq bins (fixed for 256-point RFFT)
#define GAIN_NORMALIZATION 10           // Scale factor for user input
```

---

## Part 7: Synchronization and Mutex Usage

### Display Mutex (g_LvglMutex)

All parser commands modifying waterfall state should acquire the mutex:

```c
if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    printf("Failed to acquire display mutex\n");
    return 1;
}

// Safe to call:
setWaterfallGain(value);
setColorMap(index);
clearDisplay();
addBar(colorIndices, 24);

xSemaphoreGive(g_LvglMutex);
```

### Audio Task (No Mutex Needed)

The audio processing task can call these functions safely **without** the mutex:
- `waterfall_accm_add_fft()` - Writes only to accmArray (not display state)
- `waterfall_accm_get_bar()` - Reads cached gain, not modified by parser
- `addBar()` - Assumes display is initialized and stable

---

## Part 8: Common Integration Pitfalls and Solutions

### Problem 1: First Bar Never Appears
**Cause:** `waterfall_accm_add_fft()` not being called  
**Solution:** Verify audio task is calling `waterfall_accm_add_fft()` after each FFT computation

### Problem 2: Bars Appear But Look Noisy
**Cause:** Gain too low, or accumulation frames too small  
**Solutions:**
- Increase gain: `gainWaterfall 50`
- Increase `WATERFALL_ACCM_FRAMES` (more smoothing, slower response)

### Problem 3: Parser Commands Don't Affect Display
**Cause:** Parser calling functions but audio task not running  
**Solution:** 
1. Start UDP audio: `udpStart`
2. Then initialize waterfall: `startWaterfallFeed`
3. Verify audio/FFT pipeline is running

### Problem 4: Display Freezes When Parser Commands Run
**Cause:** Missing mutex or blocking operations in audio loop  
**Solution:** 
- Ensure parser acquires `g_LvglMutex` before modifying display
- Ensure audio task doesn't block on parser operations
- Use `pdMS_TO_TICKS(500)` timeout on mutex acquisition

### Problem 5: Colormap Changes Don't Apply
**Cause:** `setColorMap()` called but `addBar()` not called after  
**Solution:** `setColorMap()` affects only newly drawn bars; existing bars unchanged

---

## Summary Checklist

- [ ] Add `initWaterfall` command (calls `waterfall_mode_init()`)
- [ ] Add `startWaterfallFeed` command (calls `waterfall_accm_init()`)
- [ ] Modify audio processing task to call `waterfall_accm_add_fft()`
- [ ] Modify audio processing task to call `waterfall_accm_get_bar()` + `addBar()` when ready
- [ ] Verify existing `gainWaterfall` command works with new gain API in spectrogram.c
- [ ] Verify existing `colorWaterfall` command still works
- [ ] Test initialization sequence: `initWaterfall` → `drawColorBars` → `gainWaterfall` → `startWaterfallFeed`
- [ ] Test live audio: Start UDP audio, watch bars appear and scroll

---

## Code Template for Audio Integration

```c
// At module scope in audio_processing_file.c:
static waterfall_accm_t g_waterfall_accm;
static uint16_t g_waterfall_colormap[24];

// In audio processing task setup:
void audio_task_init(void) {
    waterfall_accm_init(&g_waterfall_accm);
    printf("Waterfall accumulator initialized\n");
}

// In main audio processing loop (after FFT):
void process_audio_frame(int16_t *fft_output, uint32_t num_outputs) {
    // Add to waterfall accumulator
    int bar_ready = waterfall_accm_add_fft(&g_waterfall_accm, fft_output, num_outputs);
    
    if (bar_ready == 1) {
        // Convert accumulated data to colormap indices
        int status = waterfall_accm_get_bar(&g_waterfall_accm, g_waterfall_colormap);
        
        if (status == 0) {
            // Display the bar
            if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                addBar(g_waterfall_colormap, 24);
                xSemaphoreGive(g_LvglMutex);
            }
            // If mutex timeout, skip this bar (will sync on next one)
        }
    }
}
```

This template ensures safe, non-blocking integration with the waterfall display pipeline.
