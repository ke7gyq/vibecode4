# VibeCode4 Persistent Memory Implementation (April 2026)

## Overview

VibeCode4 implements a unified persistent memory system for saving and loading WiFi credentials, waterfall display configuration, and gain settings to the Pico 2 W's flash storage. The system uses a single 4096-byte flash sector and a read-modify-write pattern to safely manage multiple configuration fields without data loss.

## Architecture

### Flash Storage Layout

**Location**: Last 4KB sector of flash (address: `PICO_FLASH_SIZE_BYTES - 4096`)

**Unified Structure** (`persistent_memory_t`):
```c
typedef struct {
    /* Header (8 bytes) */
    uint32_t magic;                    // 0x50455253 ('PERS')
    uint8_t version;                   // PERSISTENT_MEMORY_VERSION = 1
    uint8_t reserved[3];               // Padding

    /* WiFi Credentials (97 bytes) */
    char wan_ssid[33];                 // SSID max 32 chars + null
    char wan_password[64];             // Password max 63 chars + null

    /* Waterfall Configuration (14 bytes) */
    uint32_t waterfall_gain;           // Linear gain × GAIN_NORMALIZATION
    uint32_t waterfall_gain_squared;   // Precomputed: (gain / 10)²
    uint16_t waterfall_color;          // Colormap index (0=Jet, 1=Parula)
    uint8_t waterfall_mode;            // 0=off, 1=TEST, 2=LIVE_AUDIO
    uint8_t _mode_reserved;            // Padding

    /* Validation (4 bytes) */
    uint32_t crc32;                    // CRC32 of data before this field

    /* Reserved (3975 bytes) */
    uint8_t _padding[3975];            // Pad to exactly 4096 bytes
} __attribute__((packed)) persistent_memory_t;
```

Total structure: **exactly 4096 bytes** (compile-time assertion validates this)

### Memory Preservation Pattern: Read-Modify-Write

The critical pattern that prevents data loss when updating individual fields:

**Problem**: Flash sectors must be erased entirely before writing. If we only erase/write part of the sector, we lose all other data.

**Solution**: Read-modify-write sequence

```c
// 1. Allocate 4KB buffer on heap
persistent_memory_t *buffer = malloc(sizeof(persistent_memory_t));

// 2. Read ENTIRE sector from flash into buffer
const persistent_memory_t *flash_ptr = _get_persistent_from_flash();
memcpy(buffer, (const void *)flash_ptr, sizeof(persistent_memory_t));

// 3. Update ONLY the fields we want to change
buffer->waterfall_gain = new_gain;
buffer->waterfall_gain_squared = compute_squared(new_gain);

// 4. Recalculate CRC32 over the data
buffer->crc32 = crc32_calc((uint8_t *)buffer, offsetof(persistent_memory_t, crc32));

// 5. Erase ENTIRE sector + write back ENTIRE buffer
taskENTER_CRITICAL();
{
    flash_range_erase(PERSISTENT_MEMORY_OFFSET, 4096);
    flash_range_program(PERSISTENT_MEMORY_OFFSET, (uint8_t *)buffer, sizeof(persistent_memory_t));
}
taskEXIT_CRITICAL();

// 6. Free buffer
free(buffer);
```

**Why this works**: All other fields are preserved because they're copied in step 2 and written back in step 5.

**Why critical section is needed**: Prevents other tasks from accessing flash while we're erasing/writing.

### Implementation Functions

#### WiFi Credentials

**Save WiFi credentials**:
```c
int network_credentials_save(const char *ssid, const char *password);
```

1. Allocates 4KB buffer
2. Reads current flash → buffer
3. **Sets magic and version headers** ← *Critical fix (April 13)*
4. Copies ssid and password to buffer
5. Recalculates CRC32
6. Erases sector and writes buffer back
7. Returns 0 on success

**Load WiFi credentials**:
```c
int network_credentials_load(char *ssid, char *password);
```

1. Points to flash memory directly (XIP)
2. Validates magic number (0x50455253)
3. Validates CRC32 checksum
4. Copies ssid/password to output parameters
5. Returns 1 if valid, 0 if invalid

**Clear WiFi credentials**:
```c
int network_credentials_clear(void);
```

1. Allocates 4KB buffer
2. Clears wan_ssid and wan_password fields
3. Sets magic/version/CRC32
4. Writes back

#### Waterfall Configuration

**Save gain**:
```c
int waterfall_config_save_gain(uint32_t waterfall_gain);
```

1. Allocates 4KB buffer
2. Reads current flash → buffer
3. **Sets magic and version headers** ← *Critical fix*
4. Updates gain and gain_squared fields
5. Recalculates CRC32
6. Writes to flash
7. Prints debug message: `[FLASH] Waterfall gain saved: {gain}`

**Load gain**:
```c
int waterfall_config_load(uint32_t *waterfall_gain);
```

1. Validates magic and CRC32
2. Returns 1 if found, 0 if not

**Save display config** (color and mode):
```c
int waterfall_config_save_display(uint16_t waterfall_color, uint8_t waterfall_mode);
```

Similar pattern to gain save.

**Load display config**:
```c
int waterfall_config_load_display(uint16_t *waterfall_color, uint8_t *waterfall_mode);
```

## Data Integrity

### CRC32 Validation

Every read operation checks CRC32 before returning data:

```c
uint32_t expected_crc = crc32_calc((uint8_t *)mem, offsetof(persistent_memory_t, crc32));
if (expected_crc != mem->crc32) {
    // CRC mismatch - flash sector corrupted or uninitialized
    return 0;  // Invalid
}
```

### Flash Corruption Detection

The magic number (0x50455253) serves as a second validation layer:

```c
if (mem->magic != PERSISTENT_MEMORY_MAGIC) {
    // Flash never written, or corrupted
    return 0;  // Invalid
}
```

If flash is blank (0xFF), magic check fails before CRC check.

## Boot-Time Loading

### Phase 0: Infrastructure Initialization

When the system boots, `infrastructure_init()` (called from main.c before task scheduler starts) loads persistent configuration:

```c
// Load waterfall gain
extern int waterfall_config_load(uint32_t *waterfall_gain);
uint32_t saved_gain = 0;
if (waterfall_config_load(&saved_gain) && saved_gain > 0) {
    printf("✓ Waterfall gain from flash: %lu\n", saved_gain);
    setWaterfallGain(saved_gain);
} else {
    printf("✓ Waterfall gain: default (75)\n");
    setWaterfallGain(75);
}

// Load WiFi credentials
extern int network_credentials_load(char *ssid, char *password);
if (network_credentials_load(g_wifi_ssid, g_wifi_password)) {
    printf("✓ WiFi credentials loaded (SSID: %s)\n", g_wifi_ssid);
} else {
    printf("✓ No saved WiFi credentials\n");
}
```

**Note**: Waterfall display settings (color, mode) are not yet loaded at boot - this is a TODO.

## Critical Bug Fix (April 13, 2026)

### Problem

WiFi credentials saved with `wifiSet` command would succeed, but immediate `wifiGet` would return "No WiFi credentials saved".

**Root Cause**: The save functions were not initializing the magic and version fields before writing:

```c
// WRONG (old code):
persistent_memory_t *buffer = malloc(...);
memcpy(buffer, flash_ptr, ...);
// buffer->magic NOT SET!
// buffer->version NOT SET!
buffer->wan_ssid = ...;  // Write directly
```

When flash reads back the data:
1. CRC check calculates hash of uninitialized data
2. CRC doesn't match → validation fails
3. Returns "invalid" even though data was written

### Solution

All three save functions now explicitly initialize headers:

```c
// CORRECT (new code):
persistent_memory_t *buffer = malloc(...);
memcpy(buffer, flash_ptr, ...);

// Initialize headers FIRST
buffer->magic = PERSISTENT_MEMORY_MAGIC;
buffer->version = PERSISTENT_MEMORY_VERSION;

// Then update fields
buffer->waterfall_gain = new_gain;
buffer->waterfall_gain_squared = compute_squared(new_gain);

// Recalculate CRC over the initialized buffer
buffer->crc32 = crc32_calc((uint8_t *)buffer, offsetof(persistent_memory_t, crc32));

// Write to flash
```

### Files Modified (April 13)

1. **src/network.c** - `network_credentials_save()`
2. **src/network.c** - `waterfall_config_save_gain()`
3. **src/network.c** - `waterfall_config_save_display()`

## Test Results (April 13, 2026)

### Test 1: Basic Credential Save/Load
```
Command: wifiSet testnet mypass
Result: ✓ [FLASH] WiFi credentials saved successfully (SSID='testnet')

Command: wifiGet
Result: ✓ [FLASH] WiFi credentials loaded: SSID='testnet'
```

### Test 2: Cross-Field Preservation
```
Steps:
  1. wifiSet testnet mypass      → Saved
  2. wifiGet                      → testnet retrieved ✓
  3. gainWaterfall 50             → Gain updated + saved
  4. wifiGet (after gain change)  → testnet STILL there ✓

Result: Read-modify-write pattern works! Other fields preserved.
```

### Test 3: Power Cycle Persistence
```
Before power cycle:
  wifiSet testnet mypass
  gainWaterfall 50

After power cycle and boot:
  wifiGet → testnet loaded from flash ✓

Result: Credentials survive reboot! Flash is durable.
```

## Constants Defined

**In src/network.c**:
```c
#define PERSISTENT_MEMORY_MAGIC 0x50455253    /* 'PERS' */
#define PERSISTENT_MEMORY_VERSION 1
#define PERSISTENT_MEMORY_OFFSET (PICO_FLASH_SIZE_BYTES - 4096)
```

**In src/infrastructure.h**:
```c
#define GAIN_NORMALIZATION 10  /* user_input / 10 = linear gain */
```

## Memory Efficiency

- **Flash**: 4KB (last sector, unavailable for program code)
- **RAM on save**: 4KB heap allocation (temporary, freed after write)
- **RAM global**: ~120 bytes for g_wifi_ssid, g_wifi_password, gain values
- **Runtime overhead**: ~50 cycles for CRC calculation per read operation

## Thread Safety

### Race Condition Prevention

1. **Critical sections**: Flash read/write protected by `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`
2. **No mutual exclusion needed for reads**: XIP flash access is atomic at word level
3. **CRC validation**: Each read independently validates data before returned

### FreeRTOS SMP Considerations

- Critical sections are required because flash operations are slow (~100μs) and hardware resources are shared
- The SMP scheduler might switch cores during long operations without critical section protection
- Waterfall task and network task may both trigger saves simultaneously - critical section prevents corruption

## Future Improvements

1. **Load display settings at boot**: `waterfall_config_load_display()` not called during infrastructure_init()
2. **Wear leveling**: Pico flash doesn't support wear leveling natively - high-frequency updates could reduce flash life
3. **Redundant sectors**: Consider keeping backup copy of persistent memory in alternate sector
4. **Update frequency limiting**: Add debounce to prevent saving on every single user input

## Code Locations

- **Structure definition**: [src/network.c](src/network.c#L65-L85)
- **Save/load functions**: [src/network.c](src/network.c#L600-L800) 
- **CRC calculation**: [src/network.c](src/network.c#L720-L775)
- **Boot-time loading**: [src/infrastructure.c](src/infrastructure.c#L145-L165)
- **Configuration getters**: [src/infrastructure.c](src/infrastructure.c#L50-L95)

## Debugging Checklist

If persistent memory isn't working:

1. **Check magic number**: Print `mem->magic` when loading - should be `0x50455253`
2. **Check version**: Should be `1`
3. **Check CRC validation**: Print recalculated CRC vs stored CRC
4. **Check critical sections**: Ensure `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` are around erase/write
5. **Check header initialization**: Verify `buffer->magic` is set BEFORE calculating CRC
6. **Check free() call**: Memory leak if not freed after write
7. **Power cycle test**: Disconnect power for 5+ seconds before testing persistence

## References

- RP2350 Flash API: `pico/flash.h`
- FreeRTOS Critical Sections: FreeRTOS kernel documentation
- CRC32 algorithm: Standard polynomial reflection algorithm (0xEDB88320)
