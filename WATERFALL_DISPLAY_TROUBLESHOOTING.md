# Waterfall Display Troubleshooting Guide
**Date**: April 8, 2026  
**Issue**: Display initializes (white screen), but no waterfall bars appear after `enableWaterfall` and `mode 2`

---

## Current Status

### ✅ What's Working
- Display initializes (white screen visible)
- Audio is transmitting (UDP client receiving data)
- Microphone task running
- `enableWaterfall` command changes mode to LIVE_AUDIO
- `waterfallMode 2` sets correct mode

### ❌ What's Not Working
- **Waterfall bars not appearing** on screen
- FFT/spectrogram rendering pipeline not pushing pixels to display

---

## Diagnostic Steps (Next Session)

### 1. Enable Debug Output
Rebuild with debug logging enabled in [src/waterfall.c:207-270](src/waterfall.c#L207-L270):

```c
if (frame_count == 0 || frame_count % 100 == 0) {
    printf("[Waterfall] Message received: seq=%lu, samples=%lu\n", msg.sequence, msg.sample_count);
}
printf("[Waterfall] BAR READY! frames accumulated, generating...\n");
printf("[Waterfall] Bar %lu rendered to screen\n", bar_count);
```

**Expected output sequence**:
```
[Waterfall] Task started on Core 1
[Waterfall] Message received: seq=1, samples=2880
[Waterfall] Message received: seq=2, samples=2880
... (100 frames)
[Waterfall] BAR READY! frames accumulated, generating...
[Waterfall] Bar 1 rendered to screen
```

### 2. Verify Audio Messages Reaching Queue

Check microphone debug output:
```c
// In src/microphone.c around line 527
if (g_micDebug >= 6) {
    printf("[Mic] WTF← seq=%lu buf=%u\n", msg.sequence, msg.buffer_id);
}
```

Enable with: `micDebug 6`

**Expected**: Should see "WTF←" messages every ~60ms

### 3. Check Bottleneck Points

| Component | Check | Expected |
|-----------|-------|----------|
| **Queue Send** | microphone logs "WTF←" | Every buffer (~60ms) |
| **Queue Receive** | waterfall logs "Message received" | After queue send |
| **FFT Processing** | spectrogram_process_samples() | No errors returned |
| **Accumulation** | accm_result == 1 every ~100 frames | Bar ready signals |
| **Display Render** | "Bar N rendered to screen" | Output appears |
| **Display Mutex** | Check for timeout message | Should not timeout |

---

## Likely Issues & Solutions

### Issue 1: waterfall_server_is_running() Returns False

**Symptom**: Microphone logs show no "WTF←" messages

**Root Cause**: `waterfall_server_is_running()` check blocking queue sends

**Check**: 
```c
// src/microphone.c line 515
if (g_audioQueueWaterfall != NULL && waterfall_server_is_running()) {
```

**Solution**: Override this guard or ensure function returns `true` in LIVE_AUDIO mode

---

### Issue 2: Accumulation Never Reaches Ready State

**Symptom**: "BAR READY!" never logged

**Root Cause**: `WATERFALL_ACCM_FRAMES` might be too high

**Check**: 
```c
// src/spectrogram.h
#define WATERFALL_ACCM_FRAMES  ???
```

**Solution**: Reduce to 5-10 for testing (was 100)

---

### Issue 3: Display Mutex Timeout

**Symptom**: "[Waterfall] Display mutex TIMEOUT" logged frequently

**Root Cause**: Display I/O or LVGL operations are too slow or deadlocked

**Check**: Monitor mutex timeout rate - if >10%, display pipeline is bottle-necked

**Solution**: Increase timeout to 500ms, or move LVGL operations off Core 1

---

### Issue 4: Stack Overflow in waterfall_task

**Symptom**: Random hangs or corrupted output

**Root Cause**: 2048-word stack not enough for FFT + local buffers

**Fix**: Increase stack in [src/main.c:456](src/main.c#L456)
```c
2048,  // ← increase to 4096
```

---

## Quick Debug Commands

```
# Enable microphone debug level 6
micDebug 6

# Monitor waterfall queue and bar generation
# (logs should show frame counts incrementing)

# Test display directly (no waterfall)
fillScreen 255  # Fill with color

# Check audio queue depth
# (should never be 0 = queue is flowing)
```

---

## Code Locations for Next Investigation

| Component | File | Lines | Purpose |
|-----------|------|-------|---------|
| Waterfall Task | [src/waterfall.c](src/waterfall.c#L186-L270) | 186-270 | Main processing loop |
| Queue Send (Audio) | [src/microphone.c](src/microphone.c#L515-L527) | 515-527 | Send to waterfall queue |
| Spectrogram Process | [src/spectrogram.c](src/spectrogram.c#L219-L241) | 219-241 | FFT + binning |
| Accum Add FFT | [src/spectrogram.c](src/spectrogram.c#L511-L549) | 511-549 | Frame accumulation logic |
| Display Render | [src/waterfall.c](src/waterfall.c#L147-L165) | 147-165 | addBar() and fillPixelsToBar() |
| Hardware Scroll | [src/st7789.c](src/st7789.c#L211-L222) | 211-222 | ST7789 scroll command |

---

## Initialization Checklist (For Next Session)

- [ ] Boot firmware, monitor console
- [ ] Run `enableWaterfall` - should initialize display (white screen)
- [ ] Run `micDebug 6` - enable detailed logging
- [ ] Run `waterfallMode 2` - switch to LIVE_AUDIO
- [ ] Watch console for "[Waterfall] Message received..." logs
- [ ] Check if "[Waterfall] BAR READY!" appears
- [ ] Verify "[Waterfall] Bar N rendered to screen" output
- [ ] Check display for bars updating

---

## Test Sequence for Isolation

1. **Test 1: Display Pipeline Only** (no audio)
   - Run `fillScreen 255` (white)
   - Run `fillScreen 0` (black)
   - Verify screen updates instantly → Display SPI works

2. **Test 2: Audio Only** (no display)
   - Monitor UDP client receiving packets
   - Check microphone task logs
   - Verify queues flowing → Audio pipeline works

3. **Test 3: Integration** (both)
   - Enable waterfall + live audio mode
   - Check console logs at each stage
   - Identify where pipeline breaks

---

**Last Known Working State**: Apr 7 - Audio streaming at 47 kHz, display can fill with color  
**Current blockers**: Waterfall bars not rendering (pipeline broken between queue and display)  
**Next action**: Deploy updated firmware with debug logging, trace data flow
