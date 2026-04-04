# Vibecode4 Audio Utilities

## Overview

This directory contains Python utilities for audio capture, testing, and analysis in the vibecode4 project.

### Files

**Audio Capture & Streaming:**
- **udp_audio_client.py** - Receives audio from Pico via UDP, saves to WAV file
- **telnet_pcm_client.py** - Real-time PCM level monitoring via Telnet

**Audio Testing & Analysis:**
- **playTone.py** - Generates 440 Hz reference tone (useful for pitch testing)
- **analyzePitch.py** - FFT-based pitch detection (verifies audio quality and sampling rate)

**Filter Configuration:**
- **generate_lut.py** - Python script that generates Look-Up Table (LUT) values for the OpenPDM2PCM filter
- **LUT_Params.h** - Auto-generated C header file containing the pre-calculated LUT values (regenerated during build)

## ⚠️ Important: Auto-Generated During Build

**LUT_Params.h is automatically generated during the CMake build process.** You should NOT manually edit it. Instead, modify filter parameters in [../src/microphone_config.h](../src/microphone_config.h), and the LUT will be regenerated automatically.

## Filter Configuration

All filter parameters are now centralized in a single configuration file:

**[../src/microphone_config.h](../src/microphone_config.h)**

This file is read by:
1. **C code** ([../src/microphone.c](../src/microphone.c)) - for filter initialization
2. **Python build script** (generate_lut.py) - to auto-generate LUT_Params.h

### Parameters

| Parameter | Header Macro | Value | Purpose |
|-----------|------|-------|---------|
| Low-pass cutoff | `AUDIO_FILTER_LP_HZ` | 10000.0 Hz | Removes quantization noise above 10 kHz |
| High-pass cutoff | `AUDIO_FILTER_HP_HZ` | 50.0 Hz | Removes DC offset below 50 Hz |
| Output sample rate | `AUDIO_FILTER_FS` | 48000 | PCM output rate (Hz) |
| Decimation ratio | `AUDIO_FILTER_DECIMATION` | 64 | PDM 3.072 MHz → 48 kHz |
| Volume scaling | `AUDIO_FILTER_MAX_VOLUME` | 16 | Output gain |
| Additional gain | `AUDIO_FILTER_GAIN` | 1 | Extra multiplier |

## How It Works

### Build-Time Generation

When you run CMake/ninja:

```
[1/NNN] Generating OpenPDM2PCM LUT lookup table
Generating LUT_Params.h...
✓ Successfully generated /path/to/build/generated/LUT_Params.h
```

The build system automatically:

1. Reads `src/microphone_config.h` to get filter parameters
2. Runs `utils/generate_lut.py` to calculate LUT values
3. Outputs to `build/generated/LUT_Params.h`
4. Compiler includes the generated file

### Modifying Filter Parameters

To change filter parameters:

1. **Edit** [src/microphone_config.h](../src/microphone_config.h)
   ```c
   #define AUDIO_FILTER_MAX_VOLUME    32    /* Increase volume to 32 */
   ```

2. **Rebuild** your project
   ```bash
   cd build
   ninja
   ```

3. **Automatic regeneration** - LUT_Params.h is automatically regenerated with new parameters

## What is LUT?

The Look-Up Table (LUT) is a pre-computed array that accelerates the PDM-to-PCM conversion process. Instead of computing filter coefficients on-the-fly during audio processing, the LUT provides pre-calculated values indexed by:

1. **Byte value** (0-255): The input PDM byte being processed
2. **Decimation index** (0-7): Position within the decimation window  
3. **Sinc filter** (0-2): Which of the three sinc³ filters

**LUT Dimensions**: `[256][8][3]` where:
- 256: All possible byte values
- 8: Decimation/8 (for decimation=64)
- 3: Number of sinc filters (SINCN=3)

## Python Script Usage

### Manual Execution

If you need to manually regenerate the LUT (usually not needed):

```bash
cd utils/
python3 generate_lut.py <path_to_config.h> <output_file.h>

# Examples:
python3 generate_lut.py ../src/microphone_config.h LUT_Params.h
python3 generate_lut.py  # Uses defaults if config not found
```

**Output**:
- Reads `AUDIO_FILTER_*` macros from the config header
- Generates pre-calculated LUT values
- Prints filter configuration and calculated constants
- File size: ~108 KB (contains 256×8×3 = 6144 LUT entries + coefficient arrays)

## CMake Integration

The CMake build system handles LUT generation automatically. Here's what happens in [../CMakeLists.txt](../CMakeLists.txt):

```cmake
# Generate LUT_Params.h from microphone_config.h
add_custom_command(
    OUTPUT "${LUT_PARAMS_H}"
    COMMAND python3 generate_lut.py
            "${MICROPHONE_CONFIG_H}"
            "${LUT_PARAMS_H}"
    DEPENDS "${MICROPHONE_CONFIG_H}" "generate_lut.py"
)
add_custom_target(generate_lut ALL DEPENDS "${LUT_PARAMS_H}")
add_dependencies(vibecode4 generate_lut)
```

- **Automatic**: Runs before compiling vibecode4
- **Smart dependencies**: Only regenerates when config changes
- **Clean generated files**: Located in `build/generated/LUT_Params.h` (out-of-source)

## Implementation Details

### Script Operation

The generate_lut.py script replicates the `Open_PDM_Filter_Init` logic from [../src/OpenPDMFilter.c](../src/OpenPDMFilter.c):

1. **Parse Configuration**: Extracts `AUDIO_FILTER_*` macros from header file
2. **Sinc³ Construction**: Builds sinc³ coefficients through convolution
3. **Coefficient Calculation**: Creates filter coefficients for each sinc filter
4. **LUT Generation**: Pre-calculates all 6144 LUT entries

### Mathematical Basis

The LUT computation extracts individual bits from a byte (c) and combines them with filter coefficients:

```
lut[c][d][s] = Sum(((c >> bit) & 0x01) * coef[s][d*8 + bit] for bit in 0..7)
```

This bit-extraction technique allows fast computation during real-time audio processing without expensive multiplication operations.

## References

- **Source Filter**: ST Microelectronics OpenPDM2PCM library
- **Filter Implementation**: [../src/OpenPDMFilter.c](../src/OpenPDMFilter.c)
- **Configuration Header**: [../src/microphone_config.h](../src/microphone_config.h)
- **Filter API**: [../src/OpenPDMFilter.h](../src/OpenPDMFilter.h)
- **Microphone Driver**: [../src/microphone.c](../src/microphone.c)

## Notes

- ✅ LUT_Params.h is automatically regenerated if you change microphone_config.h
- ✅ The LUT is specific to the current filter configuration and decimation ratio
- ✅ Pre-calculated LUT trades ~108 KB Flash memory for faster real-time audio processing
- ✅ Configuration is centralized (single source of truth)
- ✅ Both C code and Python tools stay in sync automatically

## Troubleshooting

### LUT didn't regenerate

If you modified `microphone_config.h` but the LUT didn't update:

```bash
cd build
rm -rf generated/LUT_Params.h  # Force regeneration
ninja
```

### Python script fails

Ensure:
- Python 3 is installed: `python3 --version`
- Script is executable: `chmod +x utils/generate_lut.py`
- Config file exists: `ls src/microphone_config.h`

### Wrong filter parameters in output

Check that macro names match exactly (case-sensitive):
```c
#define AUDIO_FILTER_LP_HZ      10000.0f   /* Correct */
#define Audio_Filter_LP_HZ      10000.0f   /* Wrong case */
#define AUDIO_LPFILTER_HZ       10000.0f   /* Wrong name */
```

