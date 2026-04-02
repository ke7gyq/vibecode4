/**
 * Microphone Filter Configuration
 * 
 * This header defines the filter parameters for the OpenPDM2PCM filter.
 * These parameters are used by both:
 *   1. C code in microphone.c for filter initialization
 *   2. Python build script (generate_lut.py) to auto-generate LUT_Params.h
 */

#ifndef MICROPHONE_CONFIG_H
#define MICROPHONE_CONFIG_H

/* OpenPDM2PCM Filter Parameters */
#define AUDIO_FILTER_LP_HZ           10000.0f   /* Low-pass cutoff frequency (Hz) */
#define AUDIO_FILTER_HP_HZ             50.0f   /* High-pass cutoff frequency (Hz) */
#define AUDIO_FILTER_FS              48000     /* Output sample rate (Hz) */
#define AUDIO_FILTER_DECIMATION         64     /* PDM-to-PCM decimation ratio */
#define AUDIO_FILTER_MAX_VOLUME          16     /* Output volume scaling factor */
#define AUDIO_FILTER_GAIN                1      /* Additional gain multiplier */

/* Derived Constants */
#define AUDIO_FILTER_SINCN               3      /* Number of sinc³ filters */
#define AUDIO_FILTER_DECIMATION_MAX    128      /* Maximum supported decimation */

#endif /* MICROPHONE_CONFIG_H */
