#!/usr/bin/env python3
"""
Generate and play a constant 440 Hz (A4) test tone.
Useful for testing audio sampling rates and pitch accuracy.
"""

import numpy as np
import wave
import os

# Audio parameters
FREQUENCY = 440  # Hz (A4 note)
DURATION = 5     # seconds
SAMPLE_RATE = 48000  # Match your Pico's sampling rate
AMPLITUDE = 0.8  # 0-1 range to avoid clipping

def generate_tone(freq, duration, sample_rate, amplitude):
    """Generate a sine wave at specified frequency."""
    num_samples = int(duration * sample_rate)
    t = np.linspace(0, duration, num_samples)
    wave_data = amplitude * np.sin(2 * np.pi * freq * t)
    # Convert to 16-bit PCM
    wave_data = np.int16(wave_data * 32767)
    return wave_data

def save_and_play_tone():
    """Generate, save, and play the tone."""
    print(f"Generating {FREQUENCY} Hz tone ({DURATION}s at {SAMPLE_RATE} Hz)...")
    
    # Generate tone
    tone = generate_tone(FREQUENCY, DURATION, SAMPLE_RATE, AMPLITUDE)
    
    # Save as WAV file
    output_file = "test_tone_440hz.wav"
    with wave.open(output_file, 'w') as wav_file:
        wav_file.setnchannels(1)  # Mono
        wav_file.setsampwidth(2)  # 16-bit
        wav_file.setframerate(SAMPLE_RATE)
        wav_file.writeframes(tone.tobytes())
    
    print(f"✓ Saved: {output_file}")
    print(f"  Frequency: {FREQUENCY} Hz")
    print(f"  Duration: {DURATION} seconds")
    print(f"  Sample rate: {SAMPLE_RATE} Hz")
    
    # Try to play the tone
    try:
        import subprocess
        # Try paplay first (PipeWire), then aplay (ALSA), then ffplay
        players = ['paplay', 'aplay', 'ffplay -nodisp -autoexit']
        
        for player in players:
            try:
                print(f"\nAttempting to play with: {player}")
                subprocess.run(f"{player} {output_file}", shell=True, timeout=DURATION + 2)
                print(f"✓ Playback complete")
                break
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue
        else:
            print("\n⚠ Could not find audio player (paplay, aplay, or ffplay)")
            print(f"  You can manually play: aplay {output_file}")
            print(f"  Or use: ffplay {output_file}")
    except Exception as e:
        print(f"⚠ Playback error: {e}")
        print(f"  Play the file manually: aplay {output_file}")

if __name__ == "__main__":
    try:
        save_and_play_tone()
    except ImportError as e:
        print(f"Error: Missing required package: {e}")
        print("Install with: pip install numpy scipy")
    except Exception as e:
        print(f"Error: {e}")
