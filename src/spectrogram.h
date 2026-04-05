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

/* ============== FFT Configuration ============== */

/* Sample rate (must match microphone audio rate) */
#define SPECTROGRAM_SAMPLE_RATE    16000   /* Hz */

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

#endif /* SPECTROGRAM_H */
