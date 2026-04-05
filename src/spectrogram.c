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
 * Uses log-magnitude scaling for better perceptual representation
 */
static uint8_t magnitude_to_amplitude_level(q15_t magnitude)
{
    /* Convert Q15 to floating point */
    /* Q15: range is [0, 32767] representing [0, ~1] normalized */
    float32_t mag_float = (float32_t)magnitude / 32767.0f;
    
    /* Scale by magnitude max for proper range */
    mag_float = mag_float * SPECTROGRAM_MAG_MAX;
    
    /* Use logarithmic scaling for perceptual effect */
    /* Avoid log(0) - use small epsilon */
    if (mag_float < 1.0f) {
        mag_float = 1.0f;
    }
    
    float32_t log_mag = logf(mag_float);
    
    /* Map to 0-15 amplitude level */
    /* Empirically tuned range for good visualization */
    float32_t min_log = logf(1.0f);        /* ~0 */
    float32_t max_log = logf(SPECTROGRAM_MAG_MAX);  /* ~7.6 */
    
    float32_t normalized = (log_mag - min_log) / (max_log - min_log);
    
    /* Clamp to [0, 1] */
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    
    /* Convert to 0-15 range */
    uint8_t level = (uint8_t)(normalized * 15.0f + 0.5f);
    
    return (level > 15) ? 15 : level;
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
 */
int spectrogram_process_samples(spectrogram_t *spec, int16_t *samples, uint32_t num_samples)
{
    if (spec == NULL || samples == NULL || num_samples == 0) {
        printf("ERROR: Invalid input to spectrogram_process_samples\n");
        return -1;
    }
    
    /* Update input buffer with new samples */
    spectrogram_update_input_buffer(spec, samples, num_samples);
    
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
