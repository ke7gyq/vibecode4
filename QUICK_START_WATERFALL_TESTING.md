# Waterfall Testing - Quick Start Guide

**For Thursday April 10, 2026**

---

## Flash the Binary

```bash
# Build is already done, just flash
/home/doug/.pico-sdk/picotool/2.2.0-a4/picotool/picotool load build/vibecode4.elf -fx
```

Or hold BOOTSEL button and drag `build/vibecode4.elf` to USB drive.

---

## Connect Serial Terminal

```bash
# 115200 baud, 8N1
screen /dev/ttyUSB0 115200
# or
minicom -D /dev/ttyUSB0 -b 115200
```

---

## Test Commands (in order)

### 1. Verify Display Works
```
testWaterfallColors
```
**Expected:** All 16 colors display vertically, cycle through from blue to red/yellow  
**Duration:** ~2 seconds  
**Success:** All colors distinct and correct

### 2. Test Animation
```
testWaterfallScroll
```
**Expected:** Smooth horizontal scrolling, colors flow left→right, colormap changes every 32 bars  
**Duration:** Infinite (Ctrl+C to stop, or send another command)  
**Frame rate:** ~20 bars/second (should look smooth, not choppy)  
**Success:** Animation fluid, colormaps alternate cleanly

### 3. Manual Colormap Switching
While test running or after:
```
colorWaterfall 0
```
→ Display shows Jet colormap colors

```
colorWaterfall 1
```
→ Display shows Parula colormap colors

### 4. Individual Color Test
```
drawBar 0
drawBar 7
drawBar 15
```
**Expected:** Creates solid color bar from bottom to top  
**Success:** Colors match each other and colormap

---

## Expected Output

### testWaterfallColors
```
Testing waterfall colormap - displaying all 16 colors
Color test complete. Press 'testWaterfallScroll' to test animation.
```

### testWaterfallScroll
```
Testing waterfall scroll animation - Ctrl+C to stop
Alternating between Jet (0) and Parula (1) colormaps every 32 bars
Jet colormap - scrolling...
Parula colormap - scrolling...
Parula colormap - scrolling...
[continues indefinitely]
```

---

## Troubleshooting

### Nothing displays (blank/white screen)
1. Check pin connections (PIN_DIN=0, PIN_CLK=1, PIN_CS=2, PIN_DC=3, PIN_RESET=4, PIN_BL=5)
2. Verify `st7789_lcd_init()` returned 0 (check UART output from main startup)
3. Check backlight voltage (PIN_BL=5 should be HIGH)
4. Try `clearDisplay` (if implemented elsewhere or modify test)

### Colors wrong
1. Verify RGB565 byte order: should be `(R<<11) | (G<<5) | B`
2. Check colormap arrays in `waterfall.c` match reference
3. Verify `setColorMap()` is being called with correct index

### Animation jerky/stuttering
1. Measure with timing: How many bars per second?
2. Expected: ~20 bars/sec (50ms per bar)
3. If slower: CPU may be under load, check other tasks
4. If faster: DMA may complete before display reads pixels (unlikely)

### Colormap doesn't switch
1. Verify `colorWaterfall` command is recognized (try `help`)
2. Check `setColorMap()` is updating `g_colorPointer` global
3. Try `colorWaterfall 0` then `drawBar 5` to verify change took effect

---

## Performance Checks

**Measure with UART timestamps:**

```c
// In test function, add before/after:
printf("Test start: %u\n", xTaskGetTickCount());
testWaterfallColors();
printf("Test end: %u\n", xTaskGetTickCount());
```

Compare elapsed ticks to expected time.

**Expected:**
- testWaterfallColors: ~2000 ms (50ms × 32 bars + init overhead)
- Each bar: 50 ms ± 10 ms

---

## Success Criteria

✅ **Minimal (just verify hardware):**
1. Display initializes without error
2. testWaterfallColors shows 16 distinct colors
3. No visual glitches (torn lines, wrong colors)

✅ **Good (verify animation):**
1. testWaterfallScroll shows smooth scrolling
2. Animation frame rate is smooth (≥20 bars/sec)
3. Colormap switching works cleanly
4. No color corruption

✅ **Excellent (ready for integration):**
1. All above working perfectly
2. Performance meets target (20+ bars/sec)
3. Colormaps display with accurate colors
4. Ready to integrate `spectrogram_compute_waterfall_bins()` into audio pipeline

---

## Next Steps After Testing

If ✅ Excellent:
1. Integrate waterfall into spectrogram processing
2. Call `spectrogram_compute_waterfall_bins()` from microphone task
3. Real-time waterfall animation with live audio

If ⚠️ Issues:
1. Debug with oscilloscope (check SPI clock, data lines)
2. Review `st7789_lcd_init()` sequence
3. Check DMA configuration in `lcd_write_pixels_dma()`

---

## Reference

**Test function locations:**
- `fnTestWaterfallColors()` at line 249 in `src/parser.c`
- `fnTestWaterfallScroll()` at line 278 in `src/parser.c`

**Implementation files:**
- Display engine: `src/waterfall.c` (142 lines)
- Hardware interface: `src/st7789.c` (scroll functions added)
- FFT conversion: `src/spectrogram.c` (bins function declared)

**Command registration:**
- Last two entries in `aTokens[]` array in `src/parser.c`

---

**Good luck Thursday! The implementation is solid. Focus on verifying display output. 🎯**
