#ifndef CONFIG_CONSTANTS_H
#define CONFIG_CONSTANTS_H

/**
 * Auto-generated configuration constants header
 * Generated: 2026-04-08T21:17:00.174448
 * Source: configuration.json
 * DO NOT EDIT MANUALLY - regenerate using scripts/generate_headers.py
 */

/* ============== DISPLAY CONFIGURATION ============== */

#define DISPLAY_CONTROLLER      "ST7789"
#define SCREEN_WIDTH            320
#define SCREEN_HEIGHT           240
#define DISPLAY_ORIENTATION     "portrait"

/* Display pins */
#define PIN_DIN                 0
#define PIN_CLK                 1
#define PIN_CS                  2
#define PIN_DC                  3
#define PIN_RESET               4
#define PIN_BL                  5

#define SERIAL_CLK_DIV          1.0

/* ============== AUDIO FILTER CONFIGURATION ============== */

#define AUDIO_FILTER_FS         48000
#define AUDIO_FILTER_LP_HZ      10000
#define AUDIO_FILTER_HP_HZ      50
#define AUDIO_FILTER_DECIMATION 64
#define AUDIO_FILTER_DECIMATION_MAX 128
#define AUDIO_FILTER_SINCN      3
#define AUDIO_FILTER_MAX_VOLUME 16
#define AUDIO_FILTER_GAIN       1

/* ============== SPECTROGRAM CONFIGURATION ============== */

#define SPECTROGRAM_FFT_SIZE    256
#define SPECTROGRAM_SAMPLE_RATE 6000
#define SPECTROGRAM_DOWNSAMPLE_FACTOR 8
#define SPECTROGRAM_NUM_BINS    16
#define SPECTROGRAM_BIN_SIZE    16
#define SPECTROGRAM_BINS_SKIP   5
#define SPECTROGRAM_MAG_MAX     2000.0
#define GAIN_NORMALIZATION      10

/* ============== WATERFALL CONFIGURATION ============== */

#define WATERFALL_MAX_FREQ_HZ   3000
#define WATERFALL_ACCM_FRAMES   9
#define WATERFALL_PIXELS_PER_BAR 24
#define WATERFALL_BINS_PER_BAR  5
#define WATERFALL_FFT_BINS      128
#define WATERFALL_USED_BINS     120

#endif /* CONFIG_CONSTANTS_H */
