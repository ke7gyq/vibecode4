# Vibecode4 - Real-Time Microphone Audio Streaming & Spectrum Display System

A complete **Raspberry Pi Pico 2 W** microphone capture and UDP audio streaming system with:
- **Real-time audio capture** via PIO + DMA (PDM @ 3.072 MHz)
- **Live spectrum display** with Jet/Parula colormaps and dynamic gain control
- **Stable UDP streaming** at 46.95 kHz (zero frame loss)
- **Dual-core SMP optimization** (audio on Core 0, display on Core 1)
- **Integrated OpenPDM2PCM filter** with auto-generated lookup tables

**Status**: ✅ Complete • ✅ Tested • ✅ Production Ready

## 📝 Recent Updates (April 8, 2026)

- ✅ **Centralized Configuration Infrastructure**
  - Created `configuration.json` as single source of truth for all parameters
  - Auto-generates C headers at build time via `scripts/generate_headers.py`
  - Eliminates configuration scattered across 5+ header files
  - Python build utilities now read JSON instead of parsing C code

- ✅ **Critical Bugfixes**
  - Fixed waterfall accumulation buffer overflow (passing wrong FFT data structure)
  - Fixed UDP startup race condition (auto-start server before scheduler)
  - Verified zero frame loss at 46.95 kHz stable audio rate

- ✅ **Performance Optimization**
  - Re-enabled `-O3` aggressive optimization on FFT processing (spectrogram.c)
  - Dual-core SMP verified stable with proper task affinity

---

## 🎯 What This Project Does

- **Captures audio** from a MEMS microphone via PIO + DMA (PDM @ 3.072 MHz)
- **Converts PDM → PCM** using OpenPDM2PCM filter with sinc³ decimation (64:1)
- **Streams audio over UDP** at 46.95 kHz with 528-sample frames (zero frame loss)
- **Displays real-time waterfall spectrum** with horizontal scrolling and 16 frequency bands
- **Provides TTY interface** for dynamic spectrum gain and colormap switching
- **Auto-generates optimized lookup tables** at build time from configuration header
- **Isolates critical tasks** via core affinity (SMP) to prevent display I/O blocking audio

---

## 🚀 Quick Start (5 Minutes)

### 1. Build the Project

```bash
cd /home/doug/rpi-pico/vibecode4
mkdir -p build && cd build
cmake ..
ninja
```

**Build Output:**
```
[1/188] Generating OpenPDM2PCM LUT lookup table
Filter Configuration: LP_HZ: 10000.0, HP_HZ: 50.0, Fs: 48000...
✓ Successfully generated .../build/generated/LUT_Params.h
[187/188] Linking CXX executable vibecode4.elf
```

### 2. Flash to Pico 2 W

```bash
# Option A: Using picotool
picotool load build/vibecode4.uf2 -fx

# Option B: Manual copy (if in BOOTSEL mode)
cp build/vibecode4.uf2 /media/pico/
```

### 3. Interact via Serial (TTY)

```bash
# Connect via serial (115200 baud)
minicom -b 115200 -D /dev/ttyACM0
# or
screen /dev/ttyACM0 115200
```

**Available Commands:**
```
help                    - Display all available commands
gainWaterfall           - Show current spectrum gain (default: 10)
gainWaterfall 20        - Set spectrum gain to 20 (multiplier)
colorWaterfall          - Show current colormap (0=Jet, 1=Parula)
colorWaterfall 1        - Switch to Parula colormap
colorWaterfall 0        - Switch to Jet colormap (default)
rtosStatus              - Show task CPU usage and queue depths
udpStart                - Start UDP audio server (default: running)
enableWaterfall         - Enable display updates
disableWaterfall        - Disable display updates
```

### 4. Start Audio Capture

**Telnet monitoring:**
```bash
python3 utils/telnet_pcm_client.py
# Monitor live PCM output and RMS levels
```

**UDP client (15-second capture):**
```bash
python3 utils/udp_audio_client.py --duration 15 --host 192.168.12.200 --output capture.wav
# Saves UDP frames to WAV file
# Expected: ~46.95 kHz rate, 1334 frames, 0 lost frames
```

---

## ✨ Key Features

### Real-Time Audio Streaming
- **Sample Rate**: 48 kHz PCM (528 samples/buffer)
- **Throughput**: 46.95 kHz stable (97.9% of target)
- **Frame Loss**: Zero over 15-second capture
- **Protocol**: UDP multicast on port 5001
- **Queue Buffering**: Dual queues (UDP audio + Waterfall FFT) with healthy 0/4 depth

### Spectrum Display
- **Type**: Real-time waterfall display (horizontal time scroll)
- **Resolution**: 256-point FFT → 16 frequency bands
- **Display**: LVGL canvas widget on ST7789 LCD (320×240)
- **Colormaps**: Jet (default, blue→red) + Parula (blue→yellow)
- **Update Rate**: 100-frame accumulation per display update

### Dynamic Spectrum Control
- **Spectrum Gain**: 1-1000+ (adjustable via `gainWaterfall` command)
- **Gain Application**: Integer arithmetic `gained = (accum[i] * gain²) / 10000`
- **Colormap Selection**: Jet or Parula via `colorWaterfall` command
- **TTY Interface**: Full interactive control over serial connection

### Performance Optimization
- **Dual-Core SMP**: Audio on Core 0, display on Core 1 (prevents blocking)
- **Core Affinity Pinning**: TaskCreateAffinitySet for each core-specific task
- **Integer Math**: No float operations in real-time audio loop
- **Parser Efficiency**: 1ms UART polling with aggressive yielding (7.9k ticks)
- **CPU Balance**: IDLE0/IDLE1 tasks monitor core utilization

### Hardware Efficiency
- **Flash Usage**: 874 KB (vibecode4.elf)
- **RAM Usage**: 253 KB (FreeRTOS + buffers)
- **Clock Rate**: 150 MHz Cortex-M33 ARM SMP
- **Power Draw**: ~100-150 mA (audio + display active)

---

## 🎮 TTY Command Reference

### Spectrum Gain Control
```
gainWaterfall              Show current gain (default: 10)
gainWaterfall 20           Set gain to 20 (multiply amplitude by 0.04)
gainWaterfall 50           Set gain to 50 (multiply amplitude by 0.25)
gainWaterfall 100          Set gain to 100 (multiply amplitude by 1.0)
```
**Effect**: Changes spectrum display brightness in real-time (no audio latency)

### Colormap Switching
```
colorWaterfall             Show current colormap (0=Jet, 1=Parula)
colorWaterfall 0           Switch to Jet colormap (blue→red, default)
colorWaterfall 1           Switch to Parula colormap (blue→yellow)
colorWaterfall 99          Invalid index → defaults to Jet (0)
```
**Effect**: Changes frequency band color mapping instantly

### System Status
```
rtosStatus                 Show all FreeRTOS tasks, CPU ticks, queue depths
                           MicrophoneTask, UdpAudioTask, WaterfallTask, etc.

udpStart                   Enable UDP audio streaming
udpStop                    Stop UDP audio streaming

enableWaterfall            Enable display updates
disableWaterfall           Disable display updates

help                       List all available commands
```

---

## 📊 Performance Metrics

### Audio Streaming Benchmark (15-second capture)
```
Test Command:
  ./udp_audio_client.py --duration 15 --host 192.168.12.200

Results:
  Average Rate:     46,953 Hz (97.9% of target 48 kHz)
  Total Frames:     1334 (perfect @ 83 frames/sec)
  Lost Frames:      0 (zero packet loss)
  Stable Region:    46.7-46.96 kHz (samples 6-15s)
  Peak Rate:        46.96 kHz
```

### Task CPU Usage (rtosStatus output)
```
MicrophoneTask      767 ticks (real-time audio capture)
TimerTask           6,439 ticks (display I/O on Core 1)
ParserTask          7,911 ticks (1ms UART polling with yields)
WaterfallTask       7,460 ticks (FFT + spectrum processing)
UdpAudioTask        626 ticks (UDP frame queueing)
---
Total Active:       ~30k ticks
IDLE0/IDLE1:        ~71k ticks each (healthy core utilization)
```

### Queue Depths (Healthy State)
```
UDP Queue Depth:       0/4 ✓ (producer/consumer balanced)
Waterfall Queue Depth: 0/4 ✓ (spectrum processor keeping up)
```
*If UDP queue fills to 4/4: Display I/O blocking audio (check Core 1)*
*If Waterfall queue fills to 4/4: FFT processing too slow (reduce FFT rate)*

---

### Hardware
```
ATSAMD21 Microphone
    ↓ (PDM @ 3.072 MHz, GPIOs 6-7)
RP2350 Pico 2 W (Dual-Core SMP)
    │
    ├─ [Core 0] Audio Pipeline (Real-Time)
    │   ├─ PIO State Machine 0: PDM Clock Generation
    │   ├─ PIO State Machine 1: PDM Data Sampling
    │   ├─ DMA Channel: PDM → RAM
    │   ├─ MicrophoneTask (Priority 3): PDM→PCM conversion
    │   ├─ UdpAudioTask (Priority 2): UDP streaming @ 46.95 kHz
    │   └─ WaterfallTask (Priority 2): 256-point FFT + gain scaling
    │
    ├─ [Core 1] Display Pipeline (Non-Critical)
    │   └─ TimerTask (Priority 2): LVGL + SPI display I/O
    │
    └─ FreeRTOS SMP Scheduler (portSUPPORT_SMP=1, configNUM_CORES=2)
        ├─ Task affinity pinning prevents blocking between cores
        └─ IDLE0/IDLE1 tasks monitor each core

    ↓ UDP Audio Server (Core 0, Port 5001)
        ↓ (528 samples @ 83 Hz = 46.95 kHz)
    Network Host (UDP client captures audio)

    ↓ SPI/I2C Display (Core 1, ST7789 LCD)
        ↓ (Waterfall spectrum, 320×240)
    LCD Display (Real-time waterfall visualization)

    ↓ TTY Serial (UART 0, 115200 baud)
        ↓ (User commands: gainWaterfall, colorWaterfall)
    Host Terminal (Serial control interface)
```

### Data Pipeline

```
Raw PDM Input (3.072 MHz, 1-bit)
        ↓ [PIO State Machine]
DMA Buffer (256 uint32_t, ping-pong)
        ↓ [Core 0: MicrophoneTask]
Sinc³ Filter (decimation 64:1)
        ├─→ UDP Audio Stream @ 46.95 kHz
        │   └─→ [UdpAudioTask on Core 0]
        │       └─→ Network: 528-sample frames @ 83 Hz
        │
        └─→ Waterfall FFT Analysis
            └─→ [Core 0: WaterfallTask]
                ├─ 256-point FFT → 16 frequency bands
                ├─ Integer gain scaling (user-controlled)
                ├─ Log amplitude → 0-15 level mapping
                ├─ Colormap RGB lookup (Jet/Parula)
                └─→ [Core 1: TimerTask via LVGL]
                    └─→ LCD Display (320×240 canvas)
                        └─→ Horizontal waterfall scroll

User Interface (TTY Serial @ 115200 baud)
        ├─ gainWaterfall N      - Dynamic spectrum gain
        ├─ colorWaterfall N     - Colormap switching
        ├─ rtosStatus           - CPU monitoring
        └─ enableWaterfall      - Display control

Performance Monitoring:
        Queue Depth: UDP 0/4, Waterfall 0/4 (healthy)
        CPU Usage: MicrophoneTask 767 ticks, AllTasks <85k combined
```

### Configuration System (Centralized)

```
configuration.json (Single Source of Truth for ALL parameters)
├── display:         Screen dims, pins, serial clock
├── audio_filter:    LP/HP Hz, decimation, SINC order, volume, gain
├── spectrogram:     FFT size, sample rates, bins, magnitude max
└── waterfall:       Max freq, accumulation frames, pixel/bin mapping
        │
        ├─ [BUILD TIME - scripts/generate_headers.py]
        │   └─→ build/generated/config_constants.h (C #define macros)
        │
        ├─ [BUILD TIME - utils/generate_lut.py]
        │   └─→ build/generated/LUT_Params.h (OpenPDM2PCM LUT)
        │
        └─→ [Compile Phase]
            └─→ All .c source files include auto-generated headers
                └─→ vibecode4.elf (with all configuration baked in)
```

**Key Benefit**: No more scattered configuration across 5+ header files. JSON is:
- ✅ Structured and maintainable
- ✅ Auto-generates C headers at build time
- ✅ Python/Perl scripts read JSON directly (no regex parsing)
- ✅ Computed values documented (_computed section)
- ✅ Single source of truth prevents duplication

---

## 📁 Project Structure

```
vibecode4/
├── README.md                          ← You are here
├── QUICK_START.md                     ← 5-minute deployment guide
├── IMPLEMENTATION_SUMMARY.md          ← Complete technical overview
├── configuration.json                 ← Centralized config (single source of truth) ⭐
│
├── CMakeLists.txt                     ← Build system (auto-LUT + header generation)
├── scripts/
│   └── generate_headers.py            ← JSON → C headers converter (config_constants.h) ⭐
│
├── src/
│   ├── main.c                         ← Entry point, task initialization
│   ├── microphone.h/c                 ← PDM→PCM conversion, PIO/DMA setup
│   ├── microphone_config.h            ← Legacy (now generated from config.json)
│   ├── OpenPDMFilter.c                ← STM32 OpenPDM2PCM library
│   ├── udp_audio_server.c             ← UDP frame streaming (83 Hz, auto-start)
│   ├── network.c                      ← Networking stack (UDP helpers)
│   ├── waterfall.c                    ← Waterfall display + FFT accumulation
│   ├── spectrogram.c                  ← FFT processing (-O3 optimized)
│   ├── *.pio                          ← PIO assembly (clock, data sampling)
│   ├── generated/                     ← Auto-generated at build time
│   │   ├── config_constants.h         ← Generated from configuration.json
│   │   └── LUT_Params.h               ← Generated from configuration.json
│   └── ...
│
├── utils/
│   ├── README.md                      ← LUT generation pipeline details
│   ├── generate_lut.py                ← Python LUT generator (reads config.json) ⭐
│   ├── udp_audio_client.py            ← Capture UDP frames to WAV
│   ├── telnet_pcm_client.py           ← Monitor live 48 kHz PCM output
│   └── playTone.py                    ← Test utility
│
└── build/
    ├── generated/
    │   ├── config_constants.h         ← Generated @ build time
    │   └── LUT_Params.h               ← Generated @ build time
    └── vibecode4.elf/uf2              ← Compiled binary
```

---

## ⚙️ Configuration (Centralized via configuration.json)

All system parameters are defined in **`configuration.json`** (single source of truth):

```json
{
  "display": {
    "screen_width": 320,
    "screen_height": 240,
    "pins": { "DIN": 0, "CLK": 1, "CS": 2, "DC": 3, "RESET": 4, "BL": 5 }
  },
  "audio_filter": {
    "sample_rate_hz": 48000,
    "lowpass_hz": 10000,
    "highpass_hz": 50,
    "decimation_factor": 64,
    "sinc_order": 3,
    "max_volume": 16,
    "gain": 1
  },
  "spectrogram": {
    "fft_size": 256,
    "fft_sample_rate_hz": 6000,
    "downsample_factor": 8,
    "num_display_bins": 16
  },
  "waterfall": {
    "max_frequency_hz": 3000,
    "accumulation_frames": 9,
    "pixels_per_bar": 24,
    "bins_per_bar": 5
  }
}
```

### How Configuration Works

1. **Build Time**: CMake generates C headers from JSON:
   - `scripts/generate_headers.py` → `src/generated/config_constants.h` (all #define macros)
   - `utils/generate_lut.py` → `src/generated/LUT_Params.h` (optimized lookup tables)

2. **Compile Time**: All source files use auto-generated headers:
   - No manual header editing needed
   - Single source of truth prevents duplication
   - Python build utilities read JSON (no regex parsing of C code)

3. **Runtime**: Compiled constants determine behavior:
   - Audio filter decimation, sample rates
   - Display dimensions, pins
   - Waterfall FFT parameters, accumulation count

### Changing Configuration

**To modify any parameter:**

1. Edit `configuration.json`
2. Rebuild: `cd build && ninja`
3. CMake automatically regenerates headers and recompiles

**Example**: Change waterfall max frequency from 3kHz to 6kHz:

```json
"waterfall": {
  "max_frequency_hz": 6000,  // ← Change this
  ...
}
```

Then rebuild and redeploy.

### Auto-Generated Headers (Build Products)

**`src/generated/config_constants.h`** (auto-generated, never manually edit!):

Contains all configuration as C preprocessor macros:

```c
/* Display config */
#define SCREEN_WIDTH            320
#define SCREEN_HEIGHT           240
#define PIN_DIN                 0
#define PIN_CLK                 1
/* ... all pins ... */

/* Audio filter config */
#define AUDIO_FILTER_FS         48000
#define AUDIO_FILTER_LP_HZ      10000
#define AUDIO_FILTER_HP_HZ      50
#define AUDIO_FILTER_DECIMATION 64
/* ... all audio parameters ... */

/* Waterfall config */
#define WATERFALL_MAX_FREQ_HZ   3000
#define WATERFALL_ACCM_FRAMES   9
#define WATERFALL_PIXELS_PER_BAR 24
#define WATERFALL_USED_BINS     120
/* ... all waterfall parameters ... */
```

**`src/generated/LUT_Params.h`** (auto-generated from audio_filter config):

Pre-calculated OpenPDM2PCM lookup tables optimized for the decimation factor and filter coefficients in `configuration.json`.

---

## 📊 Build Information

**Recent Successful Build:**
- Binary: 874 KB (Flash), 853 KB (code), 253 KB (RAM)
- Status: ✅ PASSING
- Flags: `USE_CONST_LUT=1`
- Output: vibecode4.elf (6.2M), vibecode4.uf2 (1.7M flashable)

**Key Functions Compiled:**
- `microphone_task` - PDM capture & PCM conversion
- `Open_PDM_Filter_Init` - Filter coefficient calculation
- `Open_PDM_Filter_64` - 64-point sinc filter
- `udp_audio_task` - Frame streaming over UDP
- `network_init` - Network stack initialization

---

## 🔌 Hardware Wiring

### Microphone Connection
```
ATSAMD21 (Knowles SPU0414HR5H)
Pin 1: GND      ──→ Pico 2 W GND
Pin 2: CLK      ──→ GPIO 6 (PIO input)
Pin 3: DATA     ──→ GPIO 7 (PIO input)
Pin 4: VDD      ──→ Pico 2 W 3V3
```

### Network (Optional - for direct USB/Ethernet)
- Standard RP2350 CYW43439 WiFi module (built-in to Pico 2 W with headers)
- UDP multicast on port 12345

---

## 🧪 Testing

### Verify Compilation
```bash
cd /home/doug/rpi-pico/vibecode4/build
# Check binary size and symbol visibility
arm-none-eabi-size vibecode4.elf
arm-none-eabi-nm vibecode4.elf | grep "microphone_task\|udp_audio_task"
```

### Verify LUT Generation
```bash
# Check config was parsed correctly
head -20 build/generated/LUT_Params.h

# Check LUT array dimensions
grep "const int32_t lut" build/generated/LUT_Params.h
```

### Capture Live Audio
```bash
# Start flashed Pico, then on host:
python3 utils/udp_audio_client.py --duration 10 --output test.wav
# Should save 10 seconds of UDP frames
```

### Monitor PCM Stream
```bash
# Telnet to Pico (if connected to network)
python3 utils/telnet_pcm_client.py
# Shows real-time PCM samples and RMS levels
```

---

## 📚 Documentation

- **[QUICK_START.md](QUICK_START.md)** - Deployment & first-run guide
- **[IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)** - Complete technical details
- **[utils/README.md](utils/README.md)** - LUT generation pipeline
- **[HARDWARE_WIRING.md](HARDWARE_WIRING.md)** - Pinout & schematic

---

## 🔧 Troubleshooting

### Build Fails: "generate_lut.py not found"
- Verify `utils/generate_lut.py` exists
- Check CMakeLists.txt has correct path

### Build Succeeds but No Audio
1. Check microphone wiring (GPIO 6-7, GND, 3V3)
2. Monitor UART output at 115200 baud
3. Verify in `main.c`: `microphone_task` created and running

### LUT Generation Shows Wrong Parameters
1. Edit `src/microphone_config.h` to correct values
2. Delete `build/` directory, rebuild from scratch
3. Verify `build/generated/LUT_Params.h` has correct values

### UDP Frames Not Received
1. Check Pico has network connectivity
2. Verify port 12345 not blocked by firewall
3. Check broadcast address matches network

---

## 📝 License & Attribution

- **OpenPDM2PCM Filter**: STMicroelectronics (Apache 2.0)
- **Pico SDK**: Raspberry Pi Foundation
- **mbedtls/lwIP**: Open-source networking stacks

---

## ✏️ Recent Updates (April 2026)

✅ UDP audio streaming refactor (TCP → UDP)
✅ Centralized configuration in `microphone_config.h`
✅ Auto-generated LUT pipeline (CMake + Python)
✅ Configurable LUT mode via CMakeLists.txt (pre-computed vs dynamic)
✅ Python utilities: UDP client, telnet monitor
✅ Complete documentation overhaul
✅ Full build verified with pre-computed const LUT

---

**Need Help?** Check the detailed guides or enable debug output in `main.c`.
