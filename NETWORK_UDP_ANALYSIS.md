# Network & UDP Audio Server Architecture Analysis

**Date**: April 13, 2026  
**Analysis Focus**: Relationship, redundancy, and optimization opportunities  

---

## Current Architecture

### Two-Task Model

**NetworkTask** (network.c:423)
- **Purpose**: WiFi connectivity + lwIP polling
- **Loop**: `while(1) { cyw43_arch_poll(); vTaskDelay(10ms); }`
- **Key responsibility**: 
  - Poll WiFi driver every 10ms
  - Monitor connection state
  - Call `udp_server_start()` after WiFi + 2-second delay
  - Re-register UDP callback on each poll (workaround for -O3 optimization)
- **Priority**: 2
- **Affinity**: CORE_NONE (floating)
- **Stack**: 512 words

**UdpAudioTask** (udp_audio_server.c:331)
- **Purpose**: Audio transmission when microphone data arrives
- **Initialization**: Blocks in 100ms polls until `g_server_running=1` set by network_task
- **Loop**: Waits on `g_audioQueueUDP`, sends per-client on arrival
- **Key responsibility**:
  - Block until server initialized
  - Receive audio buffer messages from microphone task
  - Transmit to connected UDP clients
  - Track transmission statistics
- **Priority**: 2
- **Affinity**: CORE_NONE (floating)
- **Stack**: 512 words

---

## Data Flow & Synchronization Points

```
network_task                          udp_audio_task
    ↓                                     ↓
cyw43_arch_poll()                  Wait for g_server_running
    ↓ (WiFi connects)                    ↓ (after 2s delay)
udp_server_start()                 xQueueReceive(g_audioQueueUDP)
    ├─ Sets g_server_running=1      ↓
    └─ Creates UDP PCB              udp_audio_send_frame()
         ↓                               ↓
                                    (statistics)
```

### Synchronization Mechanisms

| Mechanism | Purpose | Issue |
|-----------|---------|-------|
| **g_server_running** (volatile int) | Signal when UDP is ready | Polled with sleep loop (100ms),not event-driven |
| **g_audioQueueUDP** (FreeRTOS queue) | Audio buffer messages | Proper blocking, efficient |
| **__dmb()** memory barrier | Ensure lwIP callback visibility | Added per-poll, possible contention |
| **udp_refresh_callback_registration()** | Re-register callback per poll | Workaround for -O3 caching, overhead |

---

## Identified Optimization Opportunities

### 1. **Inefficient UDP Startup Handshake** 🔴 HIGH

**Current Problem**:
- network_task must wait for WiFi + 2 second delay
- Then calls udp_server_start() which sets g_server_running
- udp_audio_task polls g_server_running in 100ms sleep loops until it becomes 1

**Optimization**:
- Replace g_server_running volatile int with **binary semaphore**
- network_task calls `xSemaphoreGive()` after UDP initialization
- udp_audio_task calls `xSemaphoreTake()` with timeout
- **Saves**: ~20 wake cycles x 100ms = ~2 seconds startup latency

**Code impact**: Minimal (just change one synchronization primitive)

---

### 2. **Per-Poll Callback Re-registration Overhead** 🔴 HIGH

**Current Problem**:
```c
// In network_task loop (runs every 10ms):
cyw43_arch_poll();
extern void udp_refresh_callback_registration(void);
udp_refresh_callback_registration();
vTaskDelay(pdMS_TO_TICKS(10));
```

- Callback is re-registered **300 times per second** (every 10ms)
- This is a **workaround for -O3 optimization caching** in GCC
- Suggests: callback pointer being cached in register, not re-fetched from memory

**Optimization Options**:
1. **Fix root cause**: Mark callback function pointer as `volatile` in UDP PCB struct → eliminates need for re-registration
2. **Reduce frequency**: Register only on state changes (WiFi connect) instead of every poll
3. **Use memory barrier**: Replace re-registration with `__asm volatile("": : :"memory")`

**Impact**: Each registration call likely ~100-200 cycles × 300/sec = ~3% CPU overhead

---

### 3. **Separate Initialization Responsibility** 🟡 MEDIUM

**Current Problem**:
- UDP server initialization split: udp_server_start() called from network.c
- UDP server state (g_server_running) owned by udp_audio_server.c
- udp_audio_task doesn't initialize itself (waits for network_task to do it)

**Fragmentation**:
- If network_task fails to call udp_server_start() → silent failure
- No clear owner of initialization sequence
- Hard to consolidate tasks later

**Optimization**:
- Move `udp_server_start()` call into udp_audio_server module
- Have network_task send **initialization event** (semaphore) when network is ready
- udp_audio_task receives event, then calls udp_server_start() itself

**Benefit**: Clear ownership, easier to merge tasks later

---

### 4. **Memory Barrier Overhead** 🟡 MEDIUM

**Current Problem**:
```c
// In network_task loop (every 10ms):
__dmb();  /* Flush stale reads of lwIP PCB callback pointers */
cyw43_arch_poll();
```

- Data Memory Barrier flushes CPU pipeline
- Called every 10ms = 100 times/second
- Likely unnecessary if callbacks are accessed via volatile pointers

**Optimization**:
- Move barrier out of poll loop (only after WiFi state changes)
- Or remove if callback pointers marked volatile

**Impact**: ~2-5 cycles × 100/sec = <0.1% CPU

---

### 5. **Floating Task Affinity on SMP** 🟡 MEDIUM

**Current State**:
```c
.affinity_mask = CORE_NONE  // Both network_task and udp_audio_task
```

**Issue**:
- Both tasks float freely between cores
- WiFi polling + UDP transmission might thrash cache between cores
- From user memory: "Static task pinning breaks lwIP callback delivery in SMP"

**Recommendation**:
- Keep floating for now (as noted in comments and user memory)
- Only revisit if packet loss persists after other optimizations

---

## Consolidation Opportunity (Future)

**Feasibility**: HIGH ✅

Once optimizations above are in place, consolidating into single task:

```c
// Pseudocode:
void network_and_audio_task() {
    while (1) {
        cyw43_arch_poll();      // WiFi + lwIP every 10ms
        xQueueReceive(...);     // Non-blocking check for audio buffer
        if (audio_available) {
            udp_audio_send_frame();  // Send if data ready
        }
    }
}
```

**Benefits**:
- Single control flow → easier to reason about timing
- Reduced context switches (1 task instead of 2)
- Single priority level (currently both = 2)
- Shared stack space

**Risks**:
- WiFi polling might block UDP transmission if iwIP operations take >10ms
- Would need careful profiling

---

## Recommended Action Plan (In Order)

### Phase 1: Quick Wins (No code changes, just research)
1. ✅ Identify root cause of callback caching (GCC -O3 issue)
2. ✅ Measure actual overhead of per-poll re-registration
3. ✅ Profile __dmb() impact

### Phase 2: Optimization (Code changes)
1. 🔧 Replace g_server_running volatile int → binary semaphore
2. 🔧 Fix callback registration efficiency (volatile pointer or reduce frequency)
3. 🔧 Move udp_server_start() ownership to udp_audio_server module

### Phase 3: Consolidation (If warranted)
1. 🗂️ Merge network_task + udp_audio_task into single task

---

## Open Questions

1. **Where is udp_refresh_callback_registration() defined?** 
   - Need to see its implementation to understand the workaround
   - Likely in tcp_server.c or similar

2. **Why does -O3 cache callback pointers?**
   - Is it a GCC bug or expected behavior with non-volatile memory?

3. **What is the actual frame loss impact of duplicate callback registration?**
   - Could the real issue be callback not being invoked due to stale cache?

---

## Initial Assessment

**Current system is correct but has efficiency gaps**:
- ✅ Synchronization is safe (FreeRTOS queue, volatile variables)
- ✅ WiFi polling is frequent enough (10ms)
- ✅ Audio delivery is event-driven (no busy loops)
- ❌ Unnecessary per-poll overhead (callback re-registration)
- ❌ Inefficient startup handshake (100ms sleep loops)
- ❌ Fuzzy ownership of network initialization

**Estimated CPU savings from optimizations**: 3-5% (from callback re-registration alone)

**Will this fix the 4.15% packet loss?** Unlikely (different layer), but will reduce jitter and improve scheduler efficiency.

