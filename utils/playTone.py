#!/usr/bin/env python3
"""
Generate and play a test tone at specified frequency and duration.
Default: plays to speakers. Optional: save to WAV file.

Usage:
    python3 playTone.py                          # 440 Hz, 5 seconds, play only
    python3 playTone.py --frequency 1000         # 1000 Hz tone
    python3 playTone.py --duration 3             # 3 second duration
    python3 playTone.py --frequency 220 --duration 2  # 220 Hz, 2 seconds
    python3 playTone.py --save output.wav        # Play AND save to file
    python3 playTone.py --save-only test.wav     # Save only, don't play
"""

import numpy as np
import wave
import os
import sys
import argparse

# Default audio parameters
DEFAULT_FREQUENCY = 440  # Hz (A4 note)
DEFAULT_DURATION = 5     # seconds
SAMPLE_RATE = 48000      # Match your Pico's sampling rate
AMPLITUDE = 0.8          # 0-1 range to avoid clipping

def generate_tone(freq, duration, sample_rate, amplitude):
    """Generate a sine wave at specified frequency."""
    num_samples = int(duration * sample_rate)
    t = np.linspace(0, duration, num_samples)
    wave_data = amplitude * np.sin(2 * np.pi * freq * t)
    # Convert to 16-bit PCM
    wave_data = np.int16(wave_data * 32767)
    return wave_data

def save_wav(tone, output_file, sample_rate=SAMPLE_RATE):
    """Save tone as a WAV file."""
    with wave.open(output_file, 'w') as wav_file:
        wav_file.setnchannels(1)  # Mono
        wav_file.setsampwidth(2)  # 16-bit
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(tone.tobytes())
    print(f"✓ Saved: {output_file}")

def play_audio_pygame(tone, sample_rate=SAMPLE_RATE):
    """Play audio using pygame mixer."""
    try:
        import pygame
        pygame.mixer.init(frequency=sample_rate, size=-16, channels=1, buffer=512)
        
        # Create sound from array
        sound = pygame.sndarray.make_sound(tone)
        sound.play()
        
        # Wait for playback to finish
        duration_ms = int((len(tone) / sample_rate) * 1000)
        pygame.time.delay(duration_ms)
        
        print(f"✓ Playback complete (pygame)")
    except Exception as e:
        print(f"⚠ Pygame playback failed: {e}")
        return False
    return True

def play_audio_sounddevice(tone, sample_rate=SAMPLE_RATE):
    """Play audio using python-sounddevice."""
    try:
        import sounddevice as sd
        
        # Normalize to -1 to 1 range for sounddevice
        audio_data = tone.astype(np.float32) / 32768.0
        
        sd.play(audio_data, sample_rate, blocking=True)
        print(f"✓ Playback complete (sounddevice)")
    except Exception as e:
        print(f"⚠ Sounddevice playback failed: {e}")
        return False
    return True

def play_audio(tone, sample_rate=SAMPLE_RATE):
    """Play audio to speakers - tries multiple backends."""
    print(f"\nPlaying tone to speakers...")
    
    # Try pygame first (most reliable)
    if play_audio_pygame(tone, sample_rate):
        return True
    
    # Fallback to sounddevice
    if play_audio_sounddevice(tone, sample_rate):
        return True
    
    # Last resort: system commands
    print("⚠ Python audio libraries not available, falling back to system players...")
    try:
        import subprocess
        # Create temp file for system playback
        temp_file = "/tmp/playTone_temp.wav"
        save_wav(tone, temp_file, sample_rate)
        
        players = ['paplay', 'aplay']
        for player in players:
            try:
                subprocess.run(f"{player} {temp_file}", shell=True, timeout=60)
                os.remove(temp_file)
                print(f"✓ Playback complete ({player})")
                return True
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue
        
        os.remove(temp_file)
        print("⚠ Could not find system audio player (paplay or aplay)")
        return False
    except Exception as e:
        print(f"⚠ System playback failed: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(
        description='Generate and play test tones',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                           # 440 Hz, 5 sec, play to speakers
  %(prog)s --frequency 1000          # 1000 Hz tone
  %(prog)s --duration 3              # 3 second duration  
  %(prog)s --frequency 220 --duration 2  # 220 Hz, 2 seconds
  %(prog)s --save tone.wav           # Play AND save
  %(prog)s --save-only tone.wav      # Save only, no playback
        """
    )
    
    parser.add_argument('-f', '--frequency', type=float, default=DEFAULT_FREQUENCY,
                       help=f'Frequency in Hz (default: {DEFAULT_FREQUENCY})')
    parser.add_argument('-d', '--duration', type=float, default=DEFAULT_DURATION,
                       help=f'Duration in seconds (default: {DEFAULT_DURATION})')
    parser.add_argument('--save', type=str, metavar='FILE',
                       help='Save to WAV file AND play')
    parser.add_argument('--save-only', type=str, metavar='FILE',
                       help='Save to WAV file only (no playback)')
    
    args = parser.parse_args()
    
    # Validate arguments
    if args.frequency <= 0:
        print("Error: Frequency must be positive")
        sys.exit(1)
    if args.duration <= 0:
        print("Error: Duration must be positive")
        sys.exit(1)
    
    print(f"Generating {args.frequency} Hz tone ({args.duration}s at {SAMPLE_RATE} Hz)...")
    
    # Generate tone
    tone = generate_tone(args.frequency, args.duration, SAMPLE_RATE, AMPLITUDE)
    
    # Handle save-only mode
    if args.save_only:
        save_wav(tone, args.save_only, SAMPLE_RATE)
        print(f"  Frequency: {args.frequency} Hz")
        print(f"  Duration: {args.duration} seconds")
        print(f"  Sample rate: {SAMPLE_RATE} Hz")
        return
    
    # Save if requested
    if args.save:
        save_wav(tone, args.save, SAMPLE_RATE)
        print(f"  Frequency: {args.frequency} Hz")
        print(f"  Duration: {args.duration} seconds")
        print(f"  Sample rate: {SAMPLE_RATE} Hz")
    
    # Play to speakers (default)
    play_audio(tone, SAMPLE_RATE)

if __name__ == "__main__":
    try:
        main()
    except ImportError as e:
        print(f"Error: Missing required package: {e}")
        print("\nRequired packages:")
        print("  pip install numpy")
        print("\nOptional (for audio playback):")
        print("  pip install pygame          # Recommended, most reliable")
        print("  pip install python-sounddevice  # Alternative if pygame unavailable")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        sys.exit(0)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)
