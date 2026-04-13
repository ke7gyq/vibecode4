# VibeCode4 LVGL Status (Persistent Documentation)

**Date**: April 13, 2026  
**Status**: LVGL Disabled - Ruled Out as Cause of Packet Loss  
**Duration**: This determination is permanent until LVGL is re-enabled in code

---

## Critical Finding

**LVGL is NOT the cause of the 4.15% packet loss in vibecode4.**

### Evidence
- ✅ `initialize_lvgl()` is **NOT called** (disabled in `#if 0` block, main.c lines 91-201)
- ✅ All LVGL function calls are in disabled code blocks - never executed
- ✅ No LVGL tasks running (timer_task disabled)
- ✅ No LVGL memory allocation at runtime (only pointer declaration)
- ✅ No LVGL DMA operations or interrupt handlers
- ✅ No LVGL mutex acquisition by any running code

### Runtime Impact: ZERO
| Resource | Impact | Reason |
|----------|--------|--------|
| CPU Time | ❌ None | No LVGL code executed |
| RAM | ❌ None | Not initialized |
| DMA Channels | ❌ None | No DMA ops |
| Interrupts | ❌ None | No handlers |
| Mutex Contention | ❌ None | Never acquired by LVGL |
| Heap | ❌ None | No allocation |

---

## What Actually Causes the 4.15% Packet Loss

Comparative testing results:

**VibeCode4** (full audio + FFT + waterfall):
- Packet rate: 60.8 pkt/s
- **Frame loss: 4.15%** (79 frames out of 1903)
- Loss events: 68 gaps
- Max stall: 3,034 ms (3 seconds)
- Stall events: 14 occurrences

**VibeCode4b** (stripped network only - no audio/FFT):
- Packet rate: 254.1 pkt/s (4.2x faster)
- **Frame loss: 0.37%** (28 frames out of 7651)
- Loss events: 13 gaps (5.2x fewer)
- Max stall: 320.8 ms (9.5x shorter)
- Stall events: 2 occurrences

**Root cause**: Audio pipeline contention from:
1. Microphone DMA capture (Core 0)
2. 256-point FFT processing (Core 0)  
3. Waterfall display task (Core 1)
4. Cross-core synchronization delays through mutex

---

## Rule Going Forward

**DO NOT** discuss LVGL as a potential cause of packet loss, stalls, frame drops, or timing issues until LVGL is actually re-enabled in the code.

If re-enabling LVGL causes performance regression later, that will be a legitimate finding at that point.

---

## Where LVGL Code Exists (Reference)

- `src/main.c` - Disabled timer_task (lines 91-201) with all LVGL calls
- `src/widgets.c` - Completely unused functions (never called)
- `src/st7789.h` - Function signatures using LVGL types (never called)
- `src/lv_conf.h` - LVGL configuration file
- Binary includes ~100 KB of LVGL code (Flash only, not RAM)

All disabled code remains in codebase for easy re-enablement without editing.

---

## When This Note Loses Validity

This finding expires when:
1. LVGL code is uncommented/enabled in main.c
2. `initialize_lvgl()` is actually called at runtime
3. timer_task or widget functions are instantiated and running

At that point, LVGL becomes a legitimate performance suspect again, and any new testing should include LVGL as a variable.

---

## Testing Summary (April 13, 2026)

| Aspect | Finding |
|--------|---------|
| LVGL initialization | ❌ Not called |
| LVGL tasks | ❌ None running |
| LVGL memory | ❌ Zero allocated |
| LVGL performance impact | ❌ None |
| Actual bottleneck | ✅ Audio + FFT + Waterfall contention |
| Confidence level | ✅ 99% (proven by vibecode4b comparison) |

---

**File created**: April 13, 2026  
**Last updated**: April 13, 2026  
**Status**: Locked - do not reopen LVGL debugging until code changes confirm re-enablement
