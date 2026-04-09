# Waterfall Display Parser Integration Guide (Updated with Mode Management)

## Overview

The waterfall display system has three main components:

1. **Waterfall Mode Management** - Controls whether test functions or audio FFT data have display access
   - Located in `parser.c` and `waterfall.h`
   - Three exclusive modes: OFF, TEST, LIVE_AUDIO
   
2. **Waterfall Accumulator Pipeline** - Processes raw FFT data into colormap indices
   - Located in `spectrogram.c/h`
   - Handles frame accumulation, gain application, and log-to-colormap conversion
   
3. **Waterfall Display Driver** - Renders colormap indices to the LCD
   - Located in `waterfall.c/h` and `st7789.c/h`
   - Handles pixel expansion, hardware scrolling, and DMA transfers

This document explains:
- The three waterfall modes and how they prevent conflicts
- Parser commands to control waterfall initialization and modes
- How the program flow works from sensor → FFT → display
- Where parser functions fit into the data pipeline

---

## Part 1: Waterfall Mode System

### The Three Waterfall Modes

The waterfall system uses three **mutually exclusive modes** to prevent conflicts between test functions and audio FFT feeding:

#### **WATERFALL_MODE_OFF (0)**
- **Purpose:** Waterfall disabled, both test functions and audio FFT are blocked
- **Use Case:** Startup state, before initialization
- **Audio Task:** Does NOT feed FFT data
- **Test Functions:** Cannot display (returns without drawing)
- **Display State:** Undefined

#### **WATERFALL_MODE_TEST (1)**
- **Purpose:** Test functions have exclusive control of the display
- **Use Case:** Testing display hardware, verifying colormaps, debugging
- **Audio Task:** Does NOT feed FFT data (`waterfall_should_feed_fft()` returns 0)
- **Test Functions:** CAN display cleanly without interference
- **Examples:** `drawColorBars`, `drawBar`, `testWaterfallColors`, `testWaterfallScroll`
- **Auto-Set By:** All test functions automatically switch to this mode when called

#### **WATERFALL_MODE_LIVE_AUDIO (2)**
- **Purpose:** Audio processing task feeds real-time FFT data to waterfall
- **Use Case:** Normal operation with live spectrogram display
- **Audio Task:** SHOULD feed FFT data (`waterfall_should_feed_fft()` returns 1)
- **Test Functions:** Blocked (will not interfere if called)
- **Display State:** Updates with scrolling waterfall bars
- **Set By:** `enableWaterfall` or `waterfallMode 2`

### Mode Management API

**Functions (all in parser.c, declared in waterfall.h):**

```c
// Get current mode
waterfall_mode_t waterfall_get_mode(void);

// Set new mode (WATERFALL_MODE_OFF, TEST, or LIVE_AUDIO)
void waterfall_set_mode(waterfall_mode_t mode);

// Check if audio task should feed FFT (used in audio loop)
int waterfall_should_feed_fft(void);

// Get pointer to accumulator (returns NULL if not in LIVE_AUDIO mode)
void* waterfall_get_accumulator(void);
```

**Parser Commands:**

```c
// Check current mode
waterfallMode
// Output: "Current waterfall mode: TEST (1)"

// Set specific mode
waterfallMode 0    # OFF - disable all
waterfallMode 1    # TEST - test functions have control
waterfallMode 2    # LIVE_AUDIO - audio FFT feeding enabled
```

---

## Part 2: Waterfall Initialization & Mode Transitions

### Typical Mode Transitions

```
Startup
  ↓
WATERFALL_MODE_OFF (initial state)
  ↓
initWaterfall or enableWaterfall (calls waterfall_mode_init())
  ↓
User runs test function (drawColorBars, drawBar, etc.)
  ↓
Automatically → WATERFALL_MODE_TEST
  ↓
User issues enableWaterfall command
  ↓
Switches → WATERFALL_MODE_LIVE_AUDIO
  ↓
Audio task checks waterfall_should_feed_fft() 
  ↓
Audio feeds FFT → bars appear on display
```

### Initialization Steps

#### Step 1: Initialize Display Hardware (called automatically by mode functions)

**Function:** `waterfall_mode_init()`  
**Called By:** `enableWaterfall`, test functions  
**Purpose:** Set up ST7789 LCD in portrait mode with vertical hardware scrolling

```c
void waterfall_mode_init(void) {
    st7789_lcd_init();                  // Initialize SPI, GPIO, DMA
    st7789_setVerticalMode();           // Portrait mode (320×240)
    st7789_setScrollMargins(0, 0);      // No fixed margins
    clearDisplay();                      // Fill with white
}
```

#### Step 2: Enable Live Audio Mode

**Function:** `fnEnableWaterfall()`  
**Location:** `parser.c`  
**Parser Command:** `enableWaterfall`  
**Purpose:** Switch to LIVE_AUDIO mode and initialize accumulator

```c
// What happens when user types: enableWaterfall

1. Acquire g_LvglMutex
2. Call waterfall_mode_init()           // Initialize display hardware
3. Call waterfall_accm_init(&g_waterfallAccumulator)  // Reset accumulator
4. Call waterfall_set_mode(WATERFALL_MODE_LIVE_AUDIO)  // Enable audio feeding
5. Release g_LvglMutex
6. Output: "Waterfall display enabled and ready for live audio (100 FFT frames per bar)"
```

#### Step 3: Audio Task Feeds FFT Data (after enableWaterfall)

**Functions:** `waterfall_accm_add_fft()`, `waterfall_accm_get_bar()`, `addBar()`  
**Location:** Audio processing task (in udp_audio_server.c or equivalent)  
**Prerequisites:** Mode must be LIVE_AUDIO, waterfall_mode_init() called

```c
// In audio processing loop (after FFT computation):

if (waterfall_should_feed_fft() == 1) {  // Check if in LIVE_AUDIO mode
    waterfall_accm_t *accm = (waterfall_accm_t*)waterfall_get_accumulator();
    
    if (accm != NULL) {
        int bar_ready = waterfall_accm_add_fft(accm, fft_output, 256);
        
        if (bar_ready == 1) {  // 100 frames accumulated
            waterfall_accm_get_bar(accm, colormap_indices);  // Batch 512→24 pixels
            if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                addBar(colormap_indices, 24);  // Display the bar
                xSemaphoreGive(g_LvglMutex);
            }
        }
    }
}
```

---

## Part 3: Test Functions (AUTO-SWITCH TO TEST MODE)

All test functions automatically switch to `WATERFALL_MODE_TEST` before displaying, ensuring clean operation without audio FFT interference.

### Available Test Commands

| Command | Function | Display | Audio FFT | Mode Auto-Set |
|---------|----------|---------|-----------|----------------|
| `drawColorBars` | Displays white, blue, green, red bars | RGB test pattern | Blocked | → TEST |
| `drawBar <0-15>` | Single bar with specified color level | Solid color bar | Blocked | → TEST |
| `testWaterfallColors` | Cycles through all 16 colormap colors | Color sweep | Blocked | → TEST |
| `testWaterfallScroll` | Continuous scrolling animation | Alternates Jet/Parula | Blocked | → TEST |
| `colorWaterfall <0-1>` | Switch colormap (no display update) | None | Not blocked | No change |
| `gainWaterfall <N>` | Change gain value (no display update) | None | Not blocked | No change |

### Usage of Test Commands

```bash
# Switch to TEST mode and draw color bars
> drawColorBars
# Output: "Drawing individual color bars: white | blue | green | red"
# Output: "Color bars drawn: 8 white | 8 blue | 8 green | 8 red"
# Mode is now: WATERFALL_MODE_TEST

# Draw a test bar while in TEST mode
> drawBar 8
# Output: "Drew waterfall test bar with color level 8"
# Mode stays: WATERFALL_MODE_TEST

# Check current mode
> waterfallMode
# Output: "Current waterfall mode: TEST (1)"

# Switch back to live audio
> enableWaterfall
# Output: "Enabling waterfall display - initializing for live audio..."
# Output: "Waterfall display enabled and ready for live audio (100 FFT frames per bar)"
# Mode is now: WATERFALL_MODE_LIVE_AUDIO
```

---

## Part 4: Live Audio Integration

### Audio Processing Loop Integration

The audio task **checks the waterfall mode before feeding FFT data** to prevent wasteful processing when in TEST or OFF modes.

**Safe Integration Pattern:**

```c
// At module scope in audio processing file:
static uint16_t g_waterfall_colormap[24];

// In main audio processing loop (after FFT):
void process_fft_output(int16_t *fft_output) {
    // Check if LIVE_AUDIO mode enabled
    if (waterfall_should_feed_fft() != 1) {
        // Not in LIVE_AUDIO mode, skip waterfall processing
        return;
    }
    
    // Get pointer to accumulator (NULL if not in LIVE_AUDIO mode - safety check)
    waterfall_accm_t *accm = (waterfall_accm_t*)waterfall_get_accumulator();
    if (accm == NULL) {
        return;  // Safety net - shouldn't happen if mode is correct
    }
    
    // Add FFT frame to accumulator
    int bar_ready = waterfall_accm_add_fft(accm, fft_output, 256);
    
    if (bar_ready == 1) {  // 100 frames accumulated
        // Convert to colormap indices
        waterfall_accm_get_bar(accm, g_waterfall_colormap);
        
        // Try to acquire mutex (don't block - skip frame if busy)
        if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            addBar(g_waterfall_colormap, 24);
            xSemaphoreGive(g_LvglMutex);
        }
        // If mutex timeout, skip this bar (next one will appear shortly)
    }
}
```

### Performance Notes

- **Mode Check is Extremely Cheap:** Just reading a single uint32_t - ~0.1μs
- **Accumulator Pointer Retrieval is Instant:** Returns cached pointer or NULL
- **No FFT Processing if Mode is TEST/OFF:** Early return prevents wasted computation
- **Mutex Acquisition is Non-Blocking:** Uses 50ms timeout, skips frame if display is busy

### Example: UDP Audio Server Integration

```c
// In udp_audio_server.c audio processing task:

while (audio_stream_running) {
    // Get next audio frame
    int16_t *audio_frame = dequeue_audio_buffer();
    if (audio_frame == NULL) continue;
    
    // Window and compute FFT
    apply_hanning_window(audio_frame);
    arm_rfft_q15(&g_spectrogram.fft_instance, windowed_audio, fft_output);
    
    // ⭐ Feed to waterfall if in LIVE_AUDIO mode
    if (waterfall_should_feed_fft() == 1) {
        waterfall_accm_t *accm = (waterfall_accm_t*)waterfall_get_accumulator();
        if (accm != NULL && waterfall_accm_add_fft(accm, fft_output, 256) == 1) {
            waterfall_accm_get_bar(accm, colormap_indices);
            
            if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                addBar(colormap_indices, 24);
                xSemaphoreGive(g_LvglMutex);
            }
        }
    }
    
    // Send audio to UDP clients
    udp_audio_send_frame(audio_frame);
}
```

---

## Part 5: Complete Usage Sequences

### Scenario 1: Test Display Hardware at Startup

**Goal:** Verify that the LCD is working and colors are accurate

```bash
# Step 1: Initialize display
> enableWaterfall
# Output: "Enabling waterfall display - initializing for live audio..."
# Note: This puts us in LIVE_AUDIO mode, we'll immediately test the display

# Step 2: Switch to TEST mode to test without audio interference
> waterfallMode 1
# Output: "Waterfall mode set to TEST"

# Step 3: Draw color bars for RGB verification
> drawColorBars
# Output: "Drawing individual color bars: white | blue | green | red"
# Output: "Color bars drawn: 8 white | 8 blue | 8 green | 8 red"
# Display shows: 8 white bars | 8 blue bars | 8 green bars | 8 red bars

# Step 4: Test individual color levels
> drawBar 0
# Output: "Drew waterfall test bar with color level 0"
# Display shows: Bar at color index 0 (blue, low energy)

> drawBar 7
# Display shows: Bar at color index 7 (yellow, moderate energy)

> drawBar 15
# Display shows: Bar at color index 15 (pale yellow, high energy)

# Step 5: Hardware test complete - prepare for live audio
> enableWaterfall
# Output: "Enabling waterfall display - initializing for live audio..."
# Mode switches to: WATERFALL_MODE_LIVE_AUDIO
```

### Scenario 2: Run Live Waterfall Display

**Goal:** Start audio processing and display real-time spectrogram

```bash
# Step 1: Initialize waterfall for live audio (sets LIVE_AUDIO mode)
> enableWaterfall
# Output: "Enabling waterfall display - initializing for live audio..."
# Output: "NOTE: Audio processing task must be calling waterfall_accm_add_fft()..."

# Step 2: Start audio processing (UDP server with FFT feeding)
> udpStart
# Output: "UDP Server: Started on port 5001"
# Audio processing task now calls waterfall_accm_add_fft() every FFT...
# After ~1.6 seconds (100 accumulated frames at 16 kHz), first bar appears

# Step 3: Adjust display in real-time
> gainWaterfall 20
# Output: "Waterfall gain set to 20 (linear multiplication factor)"
# Display becomes brighter (2.0x gain)

> colorWaterfall 1
# Output: "Colormap changed to 1"
# New bars use Parula colormap (blue→purple→orange)

# Step 4: Monitor performance
> waterfallMode
# Output: "Current waterfall mode: LIVE_AUDIO (2)"
# Output: "  0=OFF (disabled),  1=TEST (test functions),  2=LIVE_AUDIO (audio FFT)"

# Step 5: Stop audio without losing display
> udpStop
# Output: "UDP Server: Stopped"
# Waterfall stays in LIVE_AUDIO mode but stops receiving new bars
# Old bars remain visible (frozen display)

# Step 6: If needed, switch to test mode
> waterfallMode 1
# Mode switches to TEST, allows test functions to run
# No interference with display since we stopped audio

# Step 7: Restart audio feeding
> udpStart
# Output: "UDP Server: Started on port 5001"
# Waterfall still in TEST mode
# Audio task checks mode and does NOT feed FFT (waterfall_should_feed_fft() returns 0)

# To resume live display, enable waterfall again
> enableWaterfall
# Mode switches to LIVE_AUDIO, bars resume appearing
```

### Scenario 3: Quick Display Test (No Audio Processing)

**Goal:** Test display without starting audio pipeline

```bash
# Just run test functions - they auto-switch to TEST mode
> drawColorBars
# Output: "Drawing individual color bars: white | blue | green | red"
# Display shows test pattern, waterfall auto-switched to TEST mode
# Audio processing (if running) is NOT blocked - it just doesn't feed waterfall

> testWaterfallColors
# Output: "Testing waterfall colormap - displaying all 16 colors"
# Cycles through all 16 colors, then exits

> dataWaterfallScroll
# Output: "Testing waterfall scroll animation - Ctrl+C to stop"
# Continuously draws scrolling bars
# (Press Ctrl+C in the terminal to exit)
```

---

## Part 6: Parser Commands Reference

### Mode Management

```bash
waterfallMode                    # Get current mode
waterfallMode 0                  # Set to OFF (disable all)
waterfallMode 1                  # Set to TEST (test functions control)
waterfallMode 2                  # Set to LIVE_AUDIO (audio FFT feeds waterfall)
```

### Enable/Disable Live Mode

```bash
enableWaterfall                  # Initialize hardware, init accumulator, switch to LIVE_AUDIO mode
disableWaterfall                 # Disable all waterfall operations, switch to OFF mode
```

### Display Control (Work in Any Mode)

```bash
gainWaterfall                    # Get current gain
gainWaterfall 10                 # Set gain to 1.0x (default)
gainWaterfall 20                 # Set gain to 2.0x (brighter)
gainWaterfall 5                  # Set gain to 0.5x (dimmer)

colorWaterfall                   # Get current colormap
colorWaterfall 0                 # Set to Jet colormap (blue→cyan→yellow→red)
colorWaterfall 1                 # Set to Parula colormap (blue→purple→orange)
```

### Test Functions (Auto-Switch to TEST Mode)

```bash
drawColorBars                    # Draw RGB test bars (white, blue, green, red)
drawBar 0                        # Draw single bar at color level 0 (blue, low)
drawBar 8                        # Draw single bar at color level 8 (yellow, mid)
drawBar 15                       # Draw single bar at color level 15 (pale, high)

testWaterfallColors              # Cycle through all 16 colormap indices
testWaterfallScroll              # Continuous scrolling animation (Ctrl+C to exit)
```

### Audio Server (For feeding FFT data)

```bash
udpStart                         # Start UDP audio server on port 5001
udpStop                          # Stop UDP audio server
```

---

## Part 7: Key Data Structures and Functions

### Mode Type Definition (waterfall.h)

```c
typedef enum {
    WATERFALL_MODE_OFF = 0,           /* Waterfall disabled */
    WATERFALL_MODE_TEST = 1,          /* Test mode (test functions control display) */
    WATERFALL_MODE_LIVE_AUDIO = 2     /* Live audio mode (audio task feeds FFT data) */
} waterfall_mode_t;
```

### Critical Functions

| Function | Location | Purpose | Caller |
|----------|----------|---------|--------|
| `waterfall_mode_init()` | waterfall.c | Initialize ST7789 display hardware | enableWaterfall, test functions |
| `waterfall_accm_init()` | spectrogram.c | Reset FFT accumulator | enableWaterfall |
| `waterfall_accm_add_fft()` | spectrogram.c | Add single FFT output, accumulate | Audio task loop |
| `waterfall_accm_get_bar()` | spectrogram.c | Generate 24 colormap indices | Audio task (when ready) |
| `addBar()` | waterfall.c | Display 24-pixel bar | Audio task (when ready) |
| `waterfall_get_mode()` | parser.c | Get current mode | Audio task, test functions |
| `waterfall_set_mode()` | parser.c | Set current mode | Parser commands |
| `waterfall_should_feed_fft()` | parser.c | Check if mode allows FFT feeding | Audio task (efficiency check) |
| `waterfall_get_accumulator()` | parser.c | Get pointer to accumulator | Audio task (or NULL if not LIVE_AUDIO) |

### Constants (spectrogram.h)

```c
#define WATERFALL_ACCM_FRAMES 100      // FFT frames per bar (100 = ~1.6 sec @ 16 kHz)
#define WATERFALL_FFT_BINS 512          // Number of frequency bins (fixed for 256-point RFFT)
#define GAIN_NORMALIZATION 10           // User input scale factor (10 = 1.0x gain)
```

---

## Part 8: Synchronization & Thread Safety

### Mutex Usage

The display mutex `g_LvglMutex` protects all display operations. All mode switching and display commands use it:

```c
// Pattern used in all mode-switching functions:
if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    printf("Failed to acquire display mutex\n");
    return 1;
}

// Safe to modify display and mode here
waterfall_mode_init();
waterfall_accm_init(&g_waterfallAccumulator);
waterfall_set_mode(WATERFALL_MODE_LIVE_AUDIO);

xSemaphoreGive(g_LvglMutex);
```

### Audio Task Non-Blocking Pattern

The audio task uses a **non-blocking timeout** when acquiring the mutex (prevents audio jitter):

```c
// Audio task - non-blocking display access
if (xSemaphoreTake(g_LvglMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    addBar(colorIndices, 24);
    xSemaphoreGive(g_LvglMutex);
} else {
    // Mutex busy, skip this frame (next bar will appear shortly)
    // This prevents audio processing from blocking on display updates
}
```

### Mode Checking is Atomic

The mode is a simple enum (atomic read), so checking `waterfall_get_mode()` is thread-safe:

```c
// Safe even without mutex (single word read):
if (waterfall_should_feed_fft() == 1) {
    // Process FFT - mode won't change mid-frame
}
```

---

## Part 9: Common Issues & Solutions

### Issue: Test Function Doesn't Display
**Symptoms:** `drawColorBars` command output shows success but nothing on display  
**Causes:**
1. Display mutex stuck or timeout
2. Display hardware not initialized
3. SPI/DMA issue

**Solution:**
```bash
> waterfallMode
# Check the mode - should be TEST after test function runs
# If it's OFF, display might not be initialized

> enableWaterfall
# Re-initialize display hardware and try again
```

### Issue: Waterfall Doesn't Update During Audio
**Symptoms:** `enableWaterfall` succeeds but no bars appear on display  
**Causes:**
1. Audio task not calling `waterfall_accm_add_fft()`
2. Mode check failing in audio task
3. Accumulator pointer returning NULL

**Solution:**
```bash
> waterfallMode
# Should output "LIVE_AUDIO (2)"

> udpStart
# Start audio processing
# Watch for debug output from audio task showing FFT accumulation

# Verify in audio code:
if (waterfall_should_feed_fft() == 1) {  // Must be true
    waterfall_accm_t *accm = (waterfall_accm_t*)waterfall_get_accumulator();
    if (accm != NULL) {  // Must be non-NULL
        waterfall_accm_add_fft(accm, fft_output, 256);
    }
}
```

### Issue: Test Functions Interfere with Live Audio
**Symptoms:** Audio bars stop appearing when running test functions  
**Solution:** This should NOT happen because:
1. Test functions auto-switch to TEST mode
2. Audio task checks `waterfall_should_feed_fft()` and stops feeding
3. No conflict

If it still happens, verify:
```bash
> waterfallMode
# Should show TEST after test function
# Audio task should not feed waterfall in TEST mode

> enableWaterfall
# Switch back to LIVE_AUDIO to resume
```

---

## Summary of Changes from Previous Guide

**Previous Version:**
- Single initialization sequence
- No mode management
- Potential conflicts between test and audio

**New Version:**
- Three-mode system (OFF, TEST, LIVE_AUDIO)
- Test functions auto-switch to TEST mode
- Audio task respects mode before feeding FFT
- No conflicts - modes are mutually exclusive
- Audio task can check mode for efficiency (skip processing if not LIVE_AUDIO)
- Cleaner separation of concerns

**Migration Note:** Existing code that doesn't use the mode system still works - just call `waterfall_accm_add_fft()` unconditionally. But the new pattern with mode checking is recommended for efficiency and clean integration.
