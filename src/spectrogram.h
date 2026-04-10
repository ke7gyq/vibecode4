/**
 * Spectrogram - Frequency Domain Analysis using CMSIS DSP FFT
 * 
 * Converts PDM microphone audio to frequency domain using CMSIS ARM FFT routines.
 * Bins the log-magnitude spectrum into 16 amplitude levels for waterfall display.
 */

#ifndef SPECTROGRAM_H
#define SPECTROGRAM_H

#include <stdint.h>
#include <arm_math.h>
#include "generated/config_constants.h"  /* Get SPECTROGRAM_SAMPLE_RATE from configuration.json */

/* ============== FFT Configuration ============== */

/* Sample rate (use value from generated config, fallback to 16000 if not defined) */
#ifndef SPECTROGRAM_SAMPLE_RATE
#define SPECTROGRAM_SAMPLE_RATE    16000   /* Hz - fallback default */
#endif

/* FFT size (must be power of 2) */
#define SPECTROGRAM_FFT_SIZE       256     /* Must be 128, 256, 512, 1024, etc. */

/* Input buffer size - samples captured before FFT processing */
#define SPECTROGRAM_INPUT_BUFFER_SIZE  64

/* Number of frequency bins to display (matches waterfall bands) */
#define SPECTROGRAM_NUM_BINS       16

/* Frequency resolution after binning */
#define SPECTROGRAM_BIN_SIZE       (SPECTROGRAM_FFT_SIZE / 2 / SPECTROGRAM_NUM_BINS)

/* Skip low frequency bins (DC and very low freq) */
#define SPECTROGRAM_BINS_SKIP      5

/* ============== Waterfall Frequency Limiting ============== */

/** Maximum frequency to display in waterfall (Hz) */
#define WATERFALL_MAX_FREQ_HZ      3000

/** Sample rate for audio processing (before downsampling) */
#define WATERFALL_SAMPLE_RATE_HZ   48000

/** Downsampling factor applied before FFT (48 kHz / 8 = 6 kHz) */
#define AUDIO_DOWNSAMPLE_FACTOR    8

/** Effective sample rate after downsampling for FFT */
#define WATERFALL_FFT_SAMPLE_RATE  (WATERFALL_SAMPLE_RATE_HZ / AUDIO_DOWNSAMPLE_FACTOR)

/* ============== Waterfall Gain Configuration ============== */

/** Gain normalization factor: user_input / GAIN_NORMALIZATION = linear_gain */
#define GAIN_NORMALIZATION 10

/* ============== Amplitude to Color Mapping ============== */

/* Maximum magnitude value before saturation */
#define SPECTROGRAM_MAG_MAX        2000.0f

/* ============== Data Types ============== */

/**
 * Q15 fixed-point data type used by CMSIS DSP
 * Represents values in range [-1, 1) with 16-bit resolution
 */
typedef int16_t q15_t;

/**
 * Spectrogram processing context
 * Maintains FFT state and working buffers
 */
typedef struct {
    /* FFT instance for Q15 fixed-point RFFT */
    arm_rfft_instance_q15 fft_instance;
    
    /* FFT input buffer (time domain samples) */
    q15_t input_buffer[SPECTROGRAM_FFT_SIZE];
    
    /* Hanning window for windowing function */
    q15_t window_buffer[SPECTROGRAM_FFT_SIZE];
    
    /* Windowed input (input * window) */
    q15_t windowed_input[SPECTROGRAM_FFT_SIZE];
    
    /* FFT output (complex: real, imag, real, imag, ...) */
    q15_t fft_output[SPECTROGRAM_FFT_SIZE * 2];
    
    /* FFT magnitude output */
    q15_t fft_magnitude[SPECTROGRAM_FFT_SIZE / 2];
    
    /* Binned amplitude levels (0-15 for each frequency band) */
    uint8_t amplitude_bins[SPECTROGRAM_NUM_BINS];
    
} spectrogram_t;

/* ============== Function Prototypes ============== */

/**
 * Initialize the spectrogram processor
 * Allocates FFT instance and creates Hanning window
 * 
 * @param spec pointer to spectrogram context structure
 * @return 0 on success, non-zero on failure
 */
int spectrogram_init(spectrogram_t *spec);

/**
 * Process a buffer of time-domain audio samples
 * Performs FFT and magnitude calculation
 * 
 * @param spec pointer to spectrogram context
 * @param samples pointer to input audio samples (PCM int16_t)
 * @param num_samples number of samples to process
 * @return 0 on success, non-zero on failure
 */
int spectrogram_process_samples(spectrogram_t *spec, int16_t *samples, uint32_t num_samples);

/**
 * Compute FFT and magnitude spectrum
 * Must call spectrogram_update_input_buffer() first to fill input buffer
 * 
 * @param spec pointer to spectrogram context
 * @return 0 on success, non-zero on failure
 */
int spectrogram_compute_fft(spectrogram_t *spec);

/**
 * Bin the magnitude spectrum into amplitude levels (0-15)
 * Divides frequency domain into 16 equal bands and computes log-magnitude
 * 
 * @param spec pointer to spectrogram context
 * @return 0 on success, non-zero on failure
 */
int spectrogram_compute_bins(spectrogram_t *spec);

/**
 * Shift input buffer left by INPUT_BUFFER_SIZE and append new samples
 * Used for sliding window processing of continuous audio stream
 * 
 * @param spec pointer to spectrogram context
 * @param new_samples pointer to new input samples
 * @param num_samples number of new samples to add
 */
void spectrogram_update_input_buffer(spectrogram_t *spec, int16_t *new_samples, uint32_t num_samples);

/**
 * Get the amplitude bin level for a specific frequency band
 * 
 * @param spec pointer to spectrogram context
 * @param bin_index frequency band index (0-15)
 * @return amplitude level 0-15, or 0xFF on error
 */
uint8_t spectrogram_get_bin(const spectrogram_t *spec, uint32_t bin_index);

/**
 * Deinitialize the spectrogram processor
 * Frees allocated resources
 * 
 * @param spec pointer to spectrogram context
 */
void spectrogram_deinit(spectrogram_t *spec);

/* ============== Waterfall Gain Control ============== */

/**
 * Get current waterfall gain value (squared, precomputed)
 * @return Squared gain = (user_gain / GAIN_NORMALIZATION)²
 */
uint32_t getWaterfallGain(void);

/**
 * Set waterfall gain value
 * @param gain Gain value * GAIN_NORMALIZATION (e.g., 10 = 1.0x, 20 = 2.0x)
 */
void setWaterfallGain(uint32_t gain);

/* ============== Waterfall Bandwidth Control ============== */

/**
 * Get current waterfall bandwidth (maximum frequency to display)
 * @return Maximum frequency in Hz
 */
uint32_t getWaterfallBandwidth(void);

/**
 * Set waterfall bandwidth (maximum frequency to display)
 * Frequencies above this limit are clipped to the topmost bar
 * @param bandwidth_hz Maximum frequency in Hz (must be > 0)
 */
void setWaterfallBandwidth(uint32_t bandwidth_hz);

/* ============== FFT Debug Control ============== */

/**
 * Get FFT debug flag (enables verbose logging of FFT bin values)
 * @return 1 if debug enabled, 0 otherwise
 */
int getFFTDebug(void);

/**
 * Set FFT debug flag (enables verbose logging of FFT bin values)
 * @param enable 1 to enable, 0 to disable
 */
void setFFTDebug(int enable);

/**
 * Compute 24-bin waterfall colormap indices from FFT magnitude
 * Divides 128 FFT magnitude values into 24 evenly-spaced frequency bins
 * Applies log-magnitude scaling and converts to colormap indices (0-15)
 * 
 * Used by waterfall display to generate frequency spectrum for each frame
 * 
 * @param spec pointer to spectrogram context (must have fft_magnitude populated)
 * @param waterfall_indices output array to store 24 colormap indices (0-15)
 * @param gain_squared microphone gain squared (from waterfall control)
 * @return 0 on success, non-zero on failure
 */
int spectrogram_compute_waterfall_bins(const spectrogram_t *spec, uint16_t *waterfall_indices, uint32_t gain_squared);

/**
 * Compute waterfall bins with automatic gain application
 * 
 * Convenience wrapper for spectrogram_compute_waterfall_bins() that
 * automatically retrieves the waterfall gain from the display control.
 * 
 * This is the preferred function to use in the audio processing pipeline.
 * It ensures the gain setting is properly applied during FFT processing.
 * 
 * Flow: FFT magnitude → squared → gain applied → log magnitude → colormap index
 * 
 * @param spec pointer to spectrogram context (must have fft_magnitude populated)
 * @param waterfall_indices output array to store 24 colormap indices (0-15)
 * @return 0 on success, -1 on error
 */
int spectrogram_compute_waterfall_bins_with_gain(const spectrogram_t *spec, uint16_t *waterfall_indices);

/* ============== Waterfall Frame Accumulation (New Algorithm) ============== */

/** Number of FFT frames to accumulate before generating waterfall bar (configurable) */
#define WATERFALL_ACCM_FRAMES 9

/** Number of frequency bins from FFT output (512 complex pairs -> 512 magnitude outputs) */
#define WATERFALL_FFT_BINS 512

/**
 * Waterfall accumulation context
 * Maintains running accumulators for waterfall display generation
 */
typedef struct {
    /* Accumulation array: one entry per FFT output bin */
    uint32_t accmArray[WATERFALL_FFT_BINS];
    
    /* Frame counter - when reaches WATERFALL_ACCM_FRAMES, output a bar */
    uint32_t frame_count;
    
    /* Frame accumulation in progress (for tracking state) */
    uint8_t accumulating;
    
} waterfall_accm_t;

/**
 * Initialize waterfall accumulation context
 * Zeros the accumulation array and frame counter
 * 
 * @param accm pointer to waterfall accumulation context
 * @return 0 on success
 */
int waterfall_accm_init(waterfall_accm_t *accm);

/**
 * Add FFT complex output to accumulation array
 * Computes magnitude squared for each complex FFT bin and adds to accumulator
 * 
 * Called after each FFT computation on the FFT output (512 complex values)
 * 
 * @param accm pointer to waterfall accumulation context
 * @param fft_output FFT complex output array (real, imag, real, imag, ...)
 * @param num_outputs number of complex values (typically 256 for 512-point RFFT)
 * @return 0 normally, 1 when bar is ready to output
 */
int waterfall_accm_add_fft(waterfall_accm_t *accm, const q15_t *fft_output, uint32_t num_outputs);

/**
 * Generate waterfall bar colormap indices from accumulated FFT data
 * Batches the 512 accumulated bins into 24 pixels, applies gain, and generates log colormap indices
 * 
 * Call this when waterfall_accm_add_fft() returns 1 (bar ready)
 * 
 * @param accm pointer to waterfall accumulation context
 * @param logAccmPower output array of 24 colormap indices (0-15)
 * @return 0 on success, -1 on error
 */
int waterfall_accm_get_bar(waterfall_accm_t *accm, uint16_t *logAccmPower);

#endif /* SPECTROGRAM_H */
