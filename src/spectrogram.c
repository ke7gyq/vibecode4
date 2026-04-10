/**
 * Spectrogram Implementation - FFT Processing
 * 
 * CMSIS ARM FFT-based frequency domain analysis
 * Processes audio samples using Q15 fixed-point arithmetic
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arm_math.h>
#include "spectrogram.h"

/* ============== Waterfall Gain Control ============== */

/**
 * Static gain values for waterfall display control
 * User inputs linear gain (e.g., 10 = 1.0x), stored as g_waterfallGain
 * Squared gain is precomputed and cached for efficiency: (gain / GAIN_NORMALIZATION)²
 * Squared gain is what's actually applied during waterfall bar generation
 */
static uint32_t g_waterfallGain = 300;           /* Linear gain value (user input) - reduced for accumulated power ~1600 */

/* Helper function to compute squared gain from linear gain value */
static uint32_t compute_gain_squared(uint32_t gain) {
    if (gain == 0) return 0;
    uint64_t gain_sq = (uint64_t)gain * gain;
    uint64_t divisor = (uint64_t)GAIN_NORMALIZATION * GAIN_NORMALIZATION;
    uint64_t result = gain_sq / divisor;
    return (result > UINT32_MAX) ? UINT32_MAX : (uint32_t)result;
}

/* Initialize squared gain at compile time: (300 / 10)² = 30² = 900 */
static uint32_t g_waterfallGain_squared = (uint32_t)((300ULL * 300ULL) / (10ULL * 10ULL));  /* 900 */

/**
 * Get current waterfall gain (squared, precomputed)
 * This is the value to apply during bar generation
 * @return Squared gain = (user_gain / GAIN_NORMALIZATION)²
 */
uint32_t getWaterfallGain(void) {
    return g_waterfallGain_squared;
}

/**
 * Set waterfall gain value
 * Computes squared gain once to avoid repeated squaring during bar generation
 * @param gain Linear gain value * GAIN_NORMALIZATION (e.g., 10 = 1.0x, 20 = 2.0x)
 */
void setWaterfallGain(uint32_t gain) {
    if (gain > 0) {
        g_waterfallGain = gain;
        g_waterfallGain_squared = compute_gain_squared(gain);
    }
}

/* ============== Waterfall Bandwidth Control ============== */

/**
 * Static bandwidth value for waterfall frequency range
 * Controls maximum frequency displayed (Hz)
 */
static uint32_t g_waterfallBandwidth_Hz = 3000;  /* Default: 3 kHz */

/**
 * Get current waterfall bandwidth (max frequency to display)
 * @return Maximum frequency in Hz
 */
uint32_t getWaterfallBandwidth(void) {
    return g_waterfallBandwidth_Hz;
}

/**
 * Set waterfall bandwidth (max frequency to display)
 * @param bandwidth_hz Maximum frequency to display (Hz) - clamped to 1 Hz minimum
 */
void setWaterfallBandwidth(uint32_t bandwidth_hz) {
    if (bandwidth_hz > 0) {
        g_waterfallBandwidth_Hz = bandwidth_hz;
    }
}

/* ============== FFT Debug Control ============== */

/**
 * Static debug flag for FFT logging
 * When enabled, logs FFT bin values and power levels
 */
static int g_fft_debug = 0;

/**
 * Get FFT debug flag
 * @return 1 if debug enabled, 0 otherwise
 */
int getFFTDebug(void) {
    return g_fft_debug;
}

/**
 * Set FFT debug flag
 * @param enable 1 to enable debug output, 0 to disable
 */
void setFFTDebug(int enable) {
    g_fft_debug = (enable != 0) ? 1 : 0;
}

/* ============== Static Helper Functions ============== */

/**
 * Initialize Hanning window function
 * w(n) = 0.5 * (1 - cos(2*pi*n / (N-1)))
 * 
 * Store in Q15 fixed-point format
 */
static void hanning_window_init_q15(q15_t *window, uint32_t size)
{
    const float32_t pi = 3.14159265358979f;
    
    for (uint32_t n = 0; n < size; n++) {
        /* Calculate Hanning window value in floating point */
        float32_t w = 0.5f * (1.0f - cosf(2.0f * pi * n / (size - 1)));
        
        /* Convert to Q15: value in range [0, 1) -> [-32768, 32767] */
        /* Q15: multiply by 32767 and round */
        int32_t q15_val = (int32_t)(w * 32767.0f + 0.5f);
        
        /* Clamp to Q15 range */
        if (q15_val > 32767) {
            q15_val = 32767;
        }
        
        window[n] = (q15_t)q15_val;
    }
}

/**
 * Convert Q15 magnitude to amplitude level (0-15)
 * Uses optimized integer-based log2 calculation (right-shift counting)
 * 
 * Squares the magnitude and counts right-shifts until value becomes zero.
 * This efficiently approximates logarithmic scaling without floating-point math.
 */
static uint8_t magnitude_to_amplitude_level(q15_t magnitude)
{
    /* Square the magnitude: mag_squared = magnitude * magnitude */
    int32_t mag_i32 = (int32_t)magnitude;
    uint64_t mag_squared = (uint64_t)mag_i32 * mag_i32;
    
    /* Extract upper 16 bits where significant bits reside */
    uint32_t resultant = (uint32_t)(mag_squared >> 16);
    
    /* If resultant is 0, return index 0 */
    if (resultant == 0) {
        return 0;
    }
    
    /* Count right-shifts by 1 bit until resultant becomes 0 */
    /* This computes floor(log2(resultant)) efficiently */
    uint8_t index = 0;
    do {
        resultant >>= 1;
        index++;
        if (resultant == 0) {
            break;
        }
    } while (index < 15);  /* Clamp to maximum colormap index */
    
    return index;
}

/* ============== Audio Downsampling (48 kHz → 6 kHz) ============== */

/**
 * Downsample audio by factor of 8 (48 kHz → 6 kHz)
 * Uses simple averaging over 8 consecutive samples for basic anti-aliasing
 * 
 * @param input Input samples at 48 kHz
 * @param input_count Number of input samples (must be multiple of 8)
 * @param output Output downsampled buffer (caller must allocate at least input_count/8 samples)
 * @return Number of output samples produced
 */
static uint32_t downsample_audio_8x(int16_t *input, uint32_t input_count, int16_t *output)
{
    if (input == NULL || output == NULL || input_count == 0) {
        return 0;
    }
    
    uint32_t output_count = 0;
    
    /* Average every 8 consecutive input samples */
    for (uint32_t i = 0; i + 8 <= input_count; i += 8) {
        /* Sum 8 samples */
        int32_t sum = 0;
        for (int j = 0; j < 8; j++) {
            sum += input[i + j];
        }
        /* Divide by 8 (right shift by 3) for anti-aliasing */
        int16_t downsampled = (int16_t)(sum >> 3);
        output[output_count++] = downsampled;
    }
    
    return output_count;
}

/* ============== Public Functions ============== */

/**
 * Initialize spectrogram processor
 */
int spectrogram_init(spectrogram_t *spec)
{
    if (spec == NULL) {
        printf("ERROR: Invalid spectrogram context pointer\n");
        return -1;
    }
    
    printf("Initializing spectrogram processor...\n");
    printf("  FFT Size: %u\n", SPECTROGRAM_FFT_SIZE);
    printf("  Sample Rate: %u Hz\n", SPECTROGRAM_SAMPLE_RATE);
    printf("  Frequency Bands: %u\n", SPECTROGRAM_NUM_BINS);
    printf("  Bin Size: %u\n", SPECTROGRAM_BIN_SIZE);
    
    /* Initialize FFT instance for Q15 RFFT */
    /* Parameters: instance, fftLen, ifftFlag, bitReverseFlag */
    arm_status status = arm_rfft_init_q15(&spec->fft_instance, SPECTROGRAM_FFT_SIZE, 0, 1);
    
    if (status != ARM_MATH_SUCCESS) {
        printf("ERROR: Failed to initialize FFT (status: %d)\n", status);
        return -1;
    }
    
    /* Initialize Hanning window */
    hanning_window_init_q15(spec->window_buffer, SPECTROGRAM_FFT_SIZE);
    
    /* Clear buffers */
    memset(spec->input_buffer, 0, sizeof(spec->input_buffer));
    memset(spec->windowed_input, 0, sizeof(spec->windowed_input));
    memset(spec->fft_output, 0, sizeof(spec->fft_output));
    memset(spec->fft_magnitude, 0, sizeof(spec->fft_magnitude));
    memset(spec->amplitude_bins, 0, sizeof(spec->amplitude_bins));
    
    printf("Spectrogram processor initialized successfully\n");
    
    return 0;
}

/**
 * Deinitialize spectrogram processor
 */
void spectrogram_deinit(spectrogram_t *spec)
{
    if (spec == NULL) {
        return;
    }
    
    /* No dynamic allocation, but clear buffers for safety */
    memset(spec, 0, sizeof(spectrogram_t));
    
    printf("Spectrogram processor deinitialized\n");
}

/**
 * Update input buffer with new samples (sliding window)
 * Shifts existing samples left, appends new samples on the right
 */
void spectrogram_update_input_buffer(spectrogram_t *spec, int16_t *new_samples, uint32_t num_samples)
{
    if (spec == NULL || new_samples == NULL || num_samples == 0) {
        return;
    }
    
    /* Shift existing samples left by num_samples */
    /* Use Q15 copy with shift for efficiency */
    uint32_t shift_amount = num_samples;
    uint32_t remaining = SPECTROGRAM_FFT_SIZE - shift_amount;
    
    if (shift_amount >= SPECTROGRAM_FFT_SIZE) {
        /* New data completely replaces buffer */
        if (num_samples > SPECTROGRAM_FFT_SIZE) {
            /* Truncate to buffer size */
            num_samples = SPECTROGRAM_FFT_SIZE;
        }
        /* Copy new samples directly, converting from int16_t to q15_t (same format) */
        arm_copy_q15((q15_t *)new_samples, spec->input_buffer, num_samples);
        
        /* Zero pad the rest */
        memset(&spec->input_buffer[num_samples], 0, 
               (SPECTROGRAM_FFT_SIZE - num_samples) * sizeof(q15_t));
    } else {
        /* Slide buffer left */
        arm_copy_q15(&spec->input_buffer[shift_amount], spec->input_buffer, remaining);
        
        /* Append new samples at the end */
        arm_copy_q15((q15_t *)new_samples, &spec->input_buffer[remaining], num_samples);
    }
}

/**
 * Process samples and compute FFT and amplitude bins
 * Downsamples input by 8x before processing (48 kHz → 6 kHz)
 */
int spectrogram_process_samples(spectrogram_t *spec, int16_t *samples, uint32_t num_samples)
{
    if (spec == NULL || samples == NULL || num_samples == 0) {
        printf("ERROR: Invalid input to spectrogram_process_samples\n");
        return -1;
    }
    
    /* Downsample by factor of 8 (48 kHz → 6 kHz) before FFT */
    /* Allocate temporary buffer for downsampled samples (max 66 samples from 528) */
    int16_t downsampled[66];  /* 528 / 8 = 66 samples max */
    uint32_t downsampled_count = downsample_audio_8x(samples, num_samples, downsampled);
    
    if (downsampled_count == 0) {
        /* Not enough samples to downsample */
        return 0;  /* Silent return - will accumulate on next call */
    }
    
    /* Update input buffer with downsampled samples */
    spectrogram_update_input_buffer(spec, downsampled, downsampled_count);
    
    /* Compute FFT */
    if (spectrogram_compute_fft(spec) != 0) {
        printf("ERROR: FFT computation failed\n");
        return -1;
    }
    
    /* Bin the magnitude spectrum */
    if (spectrogram_compute_bins(spec) != 0) {
        printf("ERROR: Amplitude binning failed\n");
        return -1;
    }
    
    return 0;
}

/**
 * Compute FFT of input buffer
 * Apply Hanning window, compute RFFT, and magnitude
 */
int spectrogram_compute_fft(spectrogram_t *spec)
{
    if (spec == NULL) {
        return -1;
    }
    
    /* Apply Hanning window: windowed_input[n] = input[n] * window[n] */
    arm_mult_q15(spec->input_buffer, spec->window_buffer, 
                 spec->windowed_input, SPECTROGRAM_FFT_SIZE);
    
    /* Compute RFFT: complex output with real/imag interleaved */
    arm_rfft_q15(&spec->fft_instance, spec->windowed_input, spec->fft_output);
    
    /* Compute magnitude of complex FFT output */
    /* Input: fft_output[2n] = real, fft_output[2n+1] = imag */
    /* Output: fft_magnitude[n] = sqrt(real^2 + imag^2) */
    arm_cmplx_mag_q15(spec->fft_output, spec->fft_magnitude, SPECTROGRAM_FFT_SIZE / 2);
    
    return 0;
}

/**
 * Bin FFT magnitude into 16 amplitude levels
 * Divides frequency spectrum into 16 equal bands
 */
int spectrogram_compute_bins(spectrogram_t *spec)
{
    if (spec == NULL) {
        return -1;
    }
    
    /* Initialize amplitude bins */
    memset(spec->amplitude_bins, 0, sizeof(spec->amplitude_bins));
    
    /* For each frequency band */
    for (uint32_t bin = 0; bin < SPECTROGRAM_NUM_BINS; bin++) {
        /* Calculate range of FFT bins for this frequency band */
        uint32_t bin_start = bin * SPECTROGRAM_BIN_SIZE + SPECTROGRAM_BINS_SKIP;
        uint32_t bin_end = bin_start + SPECTROGRAM_BIN_SIZE;
        
        /* Clamp to valid FFT magnitude range */
        if (bin_start >= SPECTROGRAM_FFT_SIZE / 2) {
            spec->amplitude_bins[bin] = 0;
            continue;
        }
        if (bin_end > SPECTROGRAM_FFT_SIZE / 2) {
            bin_end = SPECTROGRAM_FFT_SIZE / 2;
        }
        
        /* Find maximum magnitude in this frequency band */
        q15_t max_magnitude = 0;
        
        for (uint32_t i = bin_start; i < bin_end; i++) {
            if (spec->fft_magnitude[i] > max_magnitude) {
                max_magnitude = spec->fft_magnitude[i];
            }
        }
        
        /* Convert magnitude to amplitude level (0-15) */
        spec->amplitude_bins[bin] = magnitude_to_amplitude_level(max_magnitude);
    }
    
    return 0;
}

/**
 * Get amplitude bin for specific frequency band
 */
uint8_t spectrogram_get_bin(const spectrogram_t *spec, uint32_t bin_index)
{
    if (spec == NULL || bin_index >= SPECTROGRAM_NUM_BINS) {
        return 0xFF;  /* Error indicator */
    }
    
    return spec->amplitude_bins[bin_index];
}

/* REMOVED: spectrogram_compute_waterfall_bins() and wrapper
 * 
 * These were alternate implementations that used floating-point logf() scaling.
 * The actual waterfall rendering uses the accumulation method with integer log
 * lookup table (waterfall_accm_get_bar), which is more efficient.
 * 
 * See waterfall_accm_get_bar() below for the active implementation.
 */

/* ============== Waterfall Accumulation Algorithm ============== */

/* Number of entries in log accumulation lookup table */
#define LOGACCM_TABLE_SIZE 17

/**
 * Log accumulation lookup table for power-to-index conversion
 * Powers of 2 shifted left by 16 bits: 0, 1, 2, 4, 8, 16, 32, ..., 32768
 * Used to map accumulated squared magnitudes to colormap indices 0-15
 */
static const uint32_t logAccmLookup[LOGACCM_TABLE_SIZE] = {
    0 << 16,           /* Index 0:  threshold 0 */
    1 << 16,           /* Index 1:  threshold 65536 */
    2 << 16,           /* Index 2:  threshold 131072 */
    4 << 16,           /* Index 3:  threshold 262144 */
    8 << 16,           /* Index 4:  threshold 524288 */
    16 << 16,          /* Index 5:  threshold 1048576 */
    32 << 16,          /* Index 6:  threshold 2097152 */
    64 << 16,          /* Index 7:  threshold 4194304 */
    128 << 16,         /* Index 8:  threshold 8388608 */
    256 << 16,         /* Index 9:  threshold 16777216 */
    512 << 16,         /* Index 10: threshold 33554432 */
    1024 << 16,        /* Index 11: threshold 67108864 */
    2048 << 16,        /* Index 12: threshold 134217728 */
    4096 << 16,        /* Index 13: threshold 268435456 */
    8192 << 16,        /* Index 14: threshold 536870912 */
    16384 << 16,       /* Index 15: threshold 1073741824 */
    32768 << 16        /* Index 16: sentinel for overflow */
};

/**
 * Convert 32-bit power value to 4-bit colormap index using log lookup table
 * Searches for the threshold that powerSquared exceeds
 * 
 * @param powerSquared 32-bit accumulated power squared value
 * @return colormap index 0-15
 */
static uint8_t power_to_colormap_index(uint32_t powerSquared) {
    /* Extract upper 16 bits where the threshold values reside */
    uint32_t resultant = powerSquared >> 16;
    
    /* If resultant is 0, return index 0 */
    if (resultant == 0) {
        return 0;
    }
    
    /* Count right-shifts by 1 bit until resultant becomes 0 */
    uint8_t index = 0;
    do {
        resultant >>= 1;
        index++;
        if (resultant == 0) {
            break;
        }
    } while (index < 15);  /* Clamp to maximum colormap index */
    
    return index;
}

/**
 * Initialize waterfall accumulation context
 */
int waterfall_accm_init(waterfall_accm_t *accm)
{
    if (accm == NULL) {
        return -1;
    }
    
    memset(accm->accmArray, 0, sizeof(accm->accmArray));
    accm->frame_count = 0;
    accm->accumulating = 1;
    
    return 0;
}

/**
 * Add FFT complex output to accumulation array
 * Computes magnitude squared for each bin and accumulates
 * 
 * Returns 1 when WATERFALL_ACCM_FRAMES have been accumulated (bar ready)
 * Returns 0 otherwise
 */
int waterfall_accm_add_fft(waterfall_accm_t *accm, const q15_t *fft_output, uint32_t num_outputs)
{
    if (accm == NULL || fft_output == NULL) {
        return -1;
    }
    
    /* On first call of new cycle, reset accumulation array */
    if (accm->frame_count == 0) {
        memset(accm->accmArray, 0, sizeof(accm->accmArray));
    }
    
    /* Process FFT complex output (real, imag, real, imag, ...) */
    uint32_t fft_idx = 0;
    uint32_t max_mag_sq = 0;  /* Track max power across all bins */
    
    for (uint32_t bin = 0; bin < num_outputs && fft_idx < 2 * num_outputs; bin++) {
        /* Extract real and imaginary parts */
        int32_t real = (int32_t)fft_output[fft_idx++];
        int32_t imag = (fft_idx < 2 * num_outputs) ? (int32_t)fft_output[fft_idx++] : 0;
        
        /* TODO: Handle FFT bin 0 specially
         * FFT[0] contains both DC component (real part) and Nyquist frequency (imag part)
         * Zero only bin 0 (DC + Nyquist), but keep bins 1-3 to observe CIC3 filter response
         * Bins 1-2 (~23-47 Hz) are in the CIC3 high-pass cutoff region and useful for validation
         */
        if (bin == 0) {  /* Skip DC component and Nyquist only */
            real = 0;
            imag = 0;
        }
        
        /* Compute magnitude squared: |z|^2 = real^2 + imag^2 */
        uint64_t mag_sq = (uint64_t)real * real + (uint64_t)imag * imag;
        
        /* Track max magnitude */
        if ((uint32_t)mag_sq > max_mag_sq) {
            max_mag_sq = (uint32_t)mag_sq;
        }
        
        /* Accumulate raw magnitude squared with saturation protection to prevent overflow */
        uint64_t new_accm = (uint64_t)accm->accmArray[bin] + mag_sq;
        if (new_accm > UINT32_MAX) {
            accm->accmArray[bin] = UINT32_MAX;
        } else {
            accm->accmArray[bin] = (uint32_t)new_accm;
        }
    }
    
    /* DEBUG: Check if this is silent (max_mag_sq very low) */
    if (getFFTDebug()) {
        if (max_mag_sq < 100) {  /* Threshold for "silent" - below ~0.003 of max q15 value */
            static int silent_count = 0;
            silent_count++;
            if (silent_count == 1) {
                /* DISABLED: Multiple printf calls cause stack overflow
                printf("[FFT] *** SILENT INPUT *** (max_mag_sq=%lu)\n", max_mag_sq);
                printf("[FFT] Bins 4+ accumulated energy:\n");
                for (uint32_t bin = 4; bin < 16; bin++) {
                    printf("  Bin %u: accum=%lu\n", bin, accm->accmArray[bin]);
                }
                */
            }
        } else {
            static int silent_count = 0;
            silent_count = 0;  /* Reset counter when signal returns */
        }
    }
    
    /* Increment frame counter */
    accm->frame_count++;
    
    /* Check if we have accumulated enough frames */
    if (accm->frame_count >= WATERFALL_ACCM_FRAMES) {
        accm->frame_count = 0;  /* Reset for next cycle */
        return 1;  /* Signal that bar is ready */
    }
    return 0;  /* Still accumulating */
}

/**
 * Generate waterfall bar colormap indices from accumulated data
 * Batches accumulated FFT bins into 24 pixels with frequency limit (max 3 kHz)
 * Anything above 3 kHz clips to topmost bar
 */
int waterfall_accm_get_bar(waterfall_accm_t *accm, uint16_t *logAccmPower)
{
    if (accm == NULL || logAccmPower == NULL) {
        return -1;
    }
    
    #define PIXELS_PER_BAR 24
    #define BINS_PER_BAR 5  /* Each bar gets exactly 5 FFT bins for even frequency distribution */
    
    /* Calculate frequency mapping: 24 bars × 5 bins = 120 bins used (0-119)
     * Audio is downsampled 8x before FFT: 48 kHz → 6 kHz
     * FFT has 128 magnitude bins from 256-point FFT, covering 0-3 kHz (Nyquist at 6 kHz)
     * Resolution: 6000 Hz / 128 bins = 46.875 Hz per bin
     * Bar 0: bins 0-4 (0-117 Hz)
     * Bar 23: bins 115-119 (2695-2790 Hz)
     * Unused: bins 120-127 (2813-3000 Hz - high freq noise)
     */
    const uint32_t FFT_BINS_AVAILABLE = 128;  /* SPECTROGRAM_FFT_SIZE / 2 */
    const uint32_t fft_bins_used = PIXELS_PER_BAR * BINS_PER_BAR;  /* 24 × 5 = 120 bins */
    
    uint32_t powerIndex = 0;
    uint32_t gain_squared = getWaterfallGain();  /* Precomputed squared gain */
    
    /* Map 128 FFT bins to 24 display bars with constant 5 bins per bar (bins 120-127 unused) */
    for (uint16_t pixelNumber = 0; pixelNumber < PIXELS_PER_BAR; pixelNumber++) {
        uint64_t batchedPower = 0;
        
        /* Sum exactly 5 bins for this bar */
        for (uint32_t cnt = 0; cnt < BINS_PER_BAR && powerIndex < fft_bins_used; cnt++) {
            batchedPower += accm->accmArray[powerIndex++];
        }
        
        /* Apply squared gain to the batched power value (compute as 64-bit to prevent overflow) */
        uint64_t powerSquared_64 = (uint64_t)batchedPower * (uint64_t)gain_squared;
        
        /* Clamp to 32-bit range to prevent overflow in lookup table */
        uint32_t powerSquared = (powerSquared_64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)powerSquared_64;
        
        /* DEBUG: DISABLED - Set breakpoint here to inspect variables in debugger */
        /* if (getFFTDebug() && pixelNumber < 6) {
            printf("[BAR] Pixel %u: batchedPower=%lu, gain²=%lu, powerSquared=%lu (capped=%s)\n",
                   pixelNumber, batchedPower, gain_squared, powerSquared, 
                   (powerSquared_64 > UINT32_MAX) ? "YES" : "NO");
        } */
        
        /* Convert to colormap index using log threshold lookup */
        /* Search from highest threshold down to find which range powerSquared falls into */
        uint16_t colorIndex = 0;  /* uint16_t for consistency with logAccmPower[24] array */
        for (uint16_t idx = LOGACCM_TABLE_SIZE - 1; idx >= 0; idx--) {
            if (logAccmLookup[idx] <= powerSquared) {
                if ( idx > 15 ) {
                    /* If index exceeds 15, cap to 15 to fit colormap range */
                    colorIndex = 15;
                } else {
                    colorIndex = idx;
                }
                //colorIndex = (uint16_t)(idx > 15 ? 15 : idx);
                break;
            }
        }
        
        /* DEBUG: DISABLED - Set breakpoint here to inspect colorIndex in debugger */
        /* if (getFFTDebug() && pixelNumber < 6) {
            printf("[BAR] Color index for pixel %u: %u\n", pixelNumber, colorIndex);
        } */
        
        logAccmPower[pixelNumber] = colorIndex;
    }
    
    return 0;
}
