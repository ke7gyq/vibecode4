#!/usr/bin/env python3
"""
Analyze the dominant frequency/pitch in a WAV audio file.
Uses FFT to detect the fundamental frequency.
"""

import numpy as np
import wave
import sys
from scipy import signal

def analyze_frequency(wav_file, verbose=True):
    """Analyze WAV file and return dominant frequency."""
    
    try:
        with wave.open(wav_file, 'rb') as wav:
            # Get audio properties
            n_channels = wav.getnchannels()
            sample_width = wav.getsampwidth()
            frame_rate = wav.getframerate()
            n_frames = wav.getnframes()
            
            # Read audio data
            audio_data = wav.readframes(n_frames)
            audio_array = np.frombuffer(audio_data, dtype=np.int16)
            
            # Handle stereo by taking first channel
            if n_channels == 2:
                audio_array = audio_array[::2]
            
            duration = n_frames / frame_rate
            
            if verbose:
                print(f"Audio file: {wav_file}")
                print(f"  Channels: {n_channels}")
                print(f"  Sample rate: {frame_rate} Hz")
                print(f"  Duration: {duration:.2f} seconds")
                print(f"  Frames: {n_frames}")
        
        # Skip first and last 10% to avoid edge artifacts
        skip_start = int(len(audio_array) * 0.1)
        skip_end = int(len(audio_array) * 0.9)
        audio_array = audio_array[skip_start:skip_end]
        
        # Apply Hann window to reduce spectral leakage
        windowed = audio_array * signal.windows.hann(len(audio_array))
        
        # Compute FFT
        fft_result = np.fft.rfft(windowed)
        magnitude = np.abs(fft_result)
        frequencies = np.fft.rfftfreq(len(windowed), 1/frame_rate)
        
        # Find peak frequency (skip DC component)
        peak_idx = np.argmax(magnitude[1:]) + 1
        peak_freq = frequencies[peak_idx]
        
        if verbose:
            print(f"\n{'='*50}")
            print(f"Detected frequency: {peak_freq:.2f} Hz")
            print(f"{'='*50}")
        
        return peak_freq
        
    except Exception as e:
        print(f"Error reading WAV file: {e}")
        return None

def compare_frequencies(measured_freq, expected_freq=440):
    """Compare measured vs expected frequency and suggest sampling rate."""
    if measured_freq is None:
        return
    
    ratio = measured_freq / expected_freq
    percent_off = (ratio - 1) * 100
    
    print(f"\nExpected frequency: {expected_freq} Hz")
    print(f"Measured frequency: {measured_freq:.2f} Hz")
    print(f"Ratio (measured/expected): {ratio:.4f}")
    print(f"Percent difference: {percent_off:+.2f}%")
    
    # If playback is slower than expected, sampling rate is probably lower
    if ratio < 0.98:
        print(f"\n⚠ Audio appears SLOWER/DEEPER than expected")
        print(f"  → Pico may be sampling at {48000 * ratio:.0f} Hz instead of 48000 Hz")
        print(f"  → Or Pico is treating samples as coming from {48000 / ratio:.0f} Hz")
    elif ratio > 1.02:
        print(f"\n⚠ Audio appears FASTER/HIGHER than expected")
        print(f"  → Pico may be sampling at {48000 * ratio:.0f} Hz instead of 48000 Hz")
    else:
        print(f"\n✓ Frequency matches expected value within 2%")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: analyzePitch.py <wav_file> [expected_frequency]")
        print("Example: analyzePitch.py audio_from_pico.wav 440")
        sys.exit(1)
    
    wav_file = sys.argv[1]
    expected_freq = 440
    
    if len(sys.argv) > 2:
        expected_freq = float(sys.argv[2])
    
    measured_freq = analyze_frequency(wav_file)
    if measured_freq:
        compare_frequencies(measured_freq, expected_freq)
