#!/usr/bin/env python3
"""
Generate and play pure sinusoidal tones at specified frequencies.

Usage:
    ./playTones.py 440              # Play 440 Hz tone for 10 seconds
    ./playTones.py 440 5            # Play 440 Hz tone for 5 seconds
    ./playTones.py 440 10 --save    # Play and save tone_440.wav
    ./playTones.py 1000 8 --save    # Play and save tone_1000.wav

Output WAV files are saved as tone_<frequency>.wav in the same directory.
Uses aplay (ALSA) for audio playback.
"""

import sys
import argparse
import wave
import struct
import math
import subprocess
import tempfile
import os


def generate_tone(frequency_hz, duration_seconds, sample_rate=48000):
    """
    Generate a sinusoidal tone.
    
    Args:
        frequency_hz: Frequency of the tone in Hz
        duration_seconds: Duration of the tone in seconds
        sample_rate: Sample rate in Hz (default 48000)
    
    Returns:
        List of audio samples (16-bit signed integers)
    """
    num_samples = int(duration_seconds * sample_rate)
    samples = []
    
    # Generate sine wave
    for i in range(num_samples):
        # Calculate sample value using sine function
        # Amplitude: 32767 (max 16-bit signed int)
        t = i / sample_rate
        phase = 2 * math.pi * frequency_hz * t
        amplitude = 32767 * 0.8  # Use 0.8 to prevent clipping
        sample_value = int(amplitude * math.sin(phase))
        samples.append(sample_value)
    
    return samples


def save_wav(filename, samples, sample_rate=48000, num_channels=1, sample_width=2):
    """
    Save audio samples to a WAV file.
    
    Args:
        filename: Output filename
        samples: List of audio samples
        sample_rate: Sample rate in Hz
        num_channels: Number of audio channels (1 for mono)
        sample_width: Sample width in bytes (2 for 16-bit)
    """
    try:
        with wave.open(filename, 'wb') as wav_file:
            wav_file.setnchannels(num_channels)
            wav_file.setsampwidth(sample_width)
            wav_file.setframerate(sample_rate)
            
            # Convert samples to bytes
            for sample in samples:
                # Pack as 16-bit signed little-endian
                wav_file.writeframes(struct.pack('<h', sample))
        
        print(f"Saved: {filename}")
    except Exception as e:
        print(f"Error saving WAV file: {e}", file=sys.stderr)
        return False
    
    return True


def play_tone_aplay(samples, sample_rate=48000):
    """
    Play audio samples using aplay (ALSA audio player).
    
    Args:
        samples: List of audio samples (16-bit signed integers)
        sample_rate: Sample rate in Hz
    
    Returns:
        True if playback succeeded, False otherwise
    """
    try:
        # Create temporary WAV file
        with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as tmp:
            tmp_filename = tmp.name
        
        # Save to temporary file
        if not save_wav(tmp_filename, samples, sample_rate):
            return False
        
        try:
            print(f"Playing tone using aplay... (press Ctrl+C to stop)")
            # Call aplay to play the file
            subprocess.run(['aplay', tmp_filename], check=True)
            print("Playback complete")
            return True
        except FileNotFoundError:
            print("Error: aplay not found. Install with: sudo apt-get install alsa-utils", file=sys.stderr)
            return False
        except subprocess.CalledProcessError:
            print("Error: aplay playback failed", file=sys.stderr)
            return False
        except KeyboardInterrupt:
            print("\nPlayback stopped")
            return True
        finally:
            # Clean up temporary file
            try:
                os.unlink(tmp_filename)
            except:
                pass
    
    except Exception as e:
        print(f"Playback error: {e}", file=sys.stderr)
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Generate and play pure sinusoidal tones',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s 440              # Play 440 Hz (A4 note) for 10 seconds
  %(prog)s 440 5            # Play 440 Hz for 5 seconds
  %(prog)s 1000 10 --save   # Play 1000 Hz for 10 seconds and save tone_1000.wav
  %(prog)s 261.63 8 --save  # Generate 8-second C4 tone and save
        """
    )
    
    parser.add_argument('frequency', type=float, help='Tone frequency in Hz')
    parser.add_argument('duration', type=float, nargs='?', default=10,
                        help='Tone duration in seconds (default: 10)')
    parser.add_argument('--save', action='store_true',
                        help='Save tone as WAV file (tone_<freq>.wav)')
    parser.add_argument('--sample-rate', type=int, default=48000,
                        help='Sample rate in Hz (default: 48000)')
    parser.add_argument('--no-play', action='store_true',
                        help='Skip audio playback (useful with --save)')
    
    args = parser.parse_args()
    
    # Validate frequency
    if args.frequency <= 0:
        print("Error: Frequency must be positive", file=sys.stderr)
        return 1
    
    if args.duration <= 0:
        print("Error: Duration must be positive", file=sys.stderr)
        return 1
    
    print(f"Generating {args.frequency} Hz tone for {args.duration} seconds...")
    
    # Generate tone
    samples = generate_tone(args.frequency, args.duration, args.sample_rate)
    
    # Save WAV if requested
    if args.save:
        # Format frequency string (remove decimal if it's .0)
        freq_str = f"{args.frequency:.0f}" if args.frequency == int(args.frequency) else f"{args.frequency:.2f}"
        wav_filename = f"tone_{freq_str}.wav"
        if not save_wav(wav_filename, samples, args.sample_rate):
            return 1
    
    # Play tone if not skipped
    if not args.no_play:
        if not play_tone_aplay(samples, args.sample_rate):
            # Non-fatal error - still return success if file was saved
            if args.save:
                return 0
            return 1
    
    return 0


if __name__ == '__main__':
    sys.exit(main())



def generate_tone(frequency_hz, duration_seconds, sample_rate=48000):
    """
    Generate a sinusoidal tone.
    
    Args:
        frequency_hz: Frequency of the tone in Hz
        duration_seconds: Duration of the tone in seconds
        sample_rate: Sample rate in Hz (default 48000)
    
    Returns:
        List of audio samples (16-bit signed integers)
    """
    num_samples = int(duration_seconds * sample_rate)
    samples = []
    
    # Generate sine wave
    for i in range(num_samples):
        # Calculate sample value using sine function
        # Amplitude: 32767 (max 16-bit signed int)
        t = i / sample_rate
        phase = 2 * math.pi * frequency_hz * t
        amplitude = 32767 * 0.8  # Use 0.8 to prevent clipping
        sample_value = int(amplitude * math.sin(phase))
        samples.append(sample_value)
    
    return samples


def save_wav(filename, samples, sample_rate=48000, num_channels=1, sample_width=2):
    """
    Save audio samples to a WAV file.
    
    Args:
        filename: Output filename
        samples: List of audio samples
        sample_rate: Sample rate in Hz
        num_channels: Number of audio channels (1 for mono)
        sample_width: Sample width in bytes (2 for 16-bit)
    """
    try:
        with wave.open(filename, 'wb') as wav_file:
            wav_file.setnchannels(num_channels)
            wav_file.setsampwidth(sample_width)
            wav_file.setframerate(sample_rate)
            
            # Convert samples to bytes
            for sample in samples:
                # Pack as 16-bit signed little-endian
                wav_file.writeframes(struct.pack('<h', sample))
        
        print(f"Saved: {filename}")
    except Exception as e:
        print(f"Error saving WAV file: {e}", file=sys.stderr)
        return False
    
    return True


def main():
    parser = argparse.ArgumentParser(
        description='Generate and play pure sinusoidal tones',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s 440              # Play 440 Hz (A4 note) for 10 seconds
  %(prog)s 440 5            # Play 440 Hz for 5 seconds
  %(prog)s 1000 10 --save   # Play 1000 Hz for 10 seconds and save tone_1000.wav
  %(prog)s 261.63 8 --save  # Generate 8-second C4 tone and save
        """
    )
    
    parser.add_argument('frequency', type=float, help='Tone frequency in Hz')
    parser.add_argument('duration', type=float, nargs='?', default=10,
                        help='Tone duration in seconds (default: 10)')
    parser.add_argument('--save', action='store_true',
                        help='Save tone as WAV file (tone_<freq>.wav)')
    parser.add_argument('--sample-rate', type=int, default=48000,
                        help='Sample rate in Hz (default: 48000)')
    parser.add_argument('--no-play', action='store_true',
                        help='Skip audio playback (useful with --save)')
    
    args = parser.parse_args()
    
    # Validate frequency
    if args.frequency <= 0:
        print("Error: Frequency must be positive", file=sys.stderr)
        return 1
    
    if args.duration <= 0:
        print("Error: Duration must be positive", file=sys.stderr)
        return 1
    
    print(f"Generating {args.frequency} Hz tone for {args.duration} seconds...")
    
    # Generate tone
    samples = generate_tone(args.frequency, args.duration, args.sample_rate)
    
    # Save WAV if requested
    if args.save:
        # Format frequency string (remove decimal if it's .0)
        freq_str = f"{args.frequency:.0f}" if args.frequency == int(args.frequency) else f"{args.frequency:.2f}"
        wav_filename = f"tone_{freq_str}.wav"
        if not save_wav(wav_filename, samples, args.sample_rate):
            return 1
    
    # Play tone if not skipped
    if not args.no_play:
        if not play_tone(samples, args.sample_rate):
            # Non-fatal error - still return success if file was saved
            if args.save:
                return 0
            return 1
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
