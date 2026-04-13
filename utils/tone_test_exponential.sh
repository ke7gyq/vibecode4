#!/bin/bash

# Exponential Frequency Sweep Tone Test
# Tests waterfall frequency bin mapping with exponentially-spaced tones
# Tones: 50, 100, 200, 400, 600, 1600 Hz
# Duration: 5 seconds per tone, 30 seconds total

set -e

REPO_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
SCRIPTS_DIR="$REPO_DIR/utils"

echo "=========================================="
echo "Exponential Frequency Sweep Test"
echo "=========================================="
echo ""
echo "Frequencies: 50, 100, 200, 400, 600, 1600 Hz"
echo "Duration per tone: 5 seconds"
echo "Total duration: 30 seconds"
echo ""

FREQS=(50 100 200 400 600 1600)
DURATION_PER_TONE=5
OUTPUT_FILE="$REPO_DIR/tone_test_exponential.raw"

# Start UDP audio client to capture audio
echo "[START] Starting audio capture to $OUTPUT_FILE..."
timeout 35 "$REPO_DIR/build/udp_audio_client" \
    --host 192.168.12.200 \
    --port 5001 \
    --duration 35 \
    --output "$OUTPUT_FILE" \
    &
UDPCLIENT_PID=$!

# Give network a moment to establish
sleep 2

echo ""
echo "[TONE] Starting frequency sweep..."
echo ""

# Play each frequency
for freq in "${FREQS[@]}"; do
    echo "[FREQ] Playing $freq Hz for $DURATION_PER_TONE seconds..."
    python3 "$SCRIPTS_DIR/playTones.py" "$freq" "$DURATION_PER_TONE"
    echo "[DONE] $freq Hz complete"
    sleep 1  # Brief pause between tones
done

echo ""
echo "[CAPTURE] Waiting for UDP client to finish..."
wait $UDPCLIENT_PID 2>/dev/null || true

if [ ! -f "$OUTPUT_FILE" ]; then
    echo "[ERROR] Output file not created!"
    exit 1
fi

echo "[SUCCESS] Audio captured: $OUTPUT_FILE"
echo ""

# Analyze the captured audio
echo "[ANALYZE] Running frequency analysis..."
if command -v python3 &> /dev/null; then
    python3 << 'PYTHON_ANALYSIS'
import sys
import struct
import math
import numpy as np
from scipy import signal
from pathlib import Path

output_file = "tone_test_exponential.raw"
if not Path(output_file).exists():
    print(f"[ERROR] {output_file} not found")
    sys.exit(1)

# Read raw audio (16-bit signed, mono, 48kHz)
with open(output_file, 'rb') as f:
    audio_data = f.read()

# Convert to numpy array
samples = np.frombuffer(audio_data, dtype=np.int16).astype(np.float32) / 32768.0
sample_rate = 48000
duration = len(samples) / sample_rate

print(f"[FILE] Audio file: {output_file}")
print(f"[FILE] Duration: {duration:.2f} seconds ({len(samples)} samples)")
print(f"[FILE] Sample rate: {sample_rate} Hz")
print()

# Analyze each frequency section
freqs = [50, 100, 200, 400, 600, 1600]
tone_duration = 5  # seconds
samples_per_tone = int(tone_duration * sample_rate)

print("========================================")
print("FREQUENCY ANALYSIS BY TONE")
print("========================================")
print()

for idx, freq in enumerate(freqs):
    start_idx = (idx + 1) * samples_per_tone  # Skip first 5s (settling)
    end_idx = start_idx + samples_per_tone
    
    if end_idx > len(samples):
        end_idx = len(samples)
    
    if start_idx >= len(samples):
        print(f"[SKIP] Tone {freq} Hz: Not enough data")
        continue
    
    segment = samples[start_idx:end_idx]
    
    # Apply Hann window for FFT
    windowed = segment * signal.hann(len(segment))
    
    # Compute FFT
    fft_result = np.fft.rfft(windowed, n=256)
    magnitude = np.abs(fft_result)
    
    # Find peak frequency
    peak_idx = np.argmax(magnitude)
    peak_freq = (peak_idx * sample_rate) / 256  # Bin spacing: 48000 Hz / 256 = 187.5 Hz per bin @ 48kHz
    peak_magnitude = magnitude[peak_idx]
    
    # Find magnitude at expected bin
    expected_bin = int((freq * 256) / sample_rate)
    expected_magnitude = magnitude[expected_bin] if expected_bin < len(magnitude) else 0
    
    print(f"[FREQ {freq:4d} Hz]")
    print(f"  Peak detected:  {peak_freq:7.1f} Hz (bin {peak_idx:3d})")
    print(f"  Expected bin:   {expected_bin:3d} ({(expected_bin * 48000 / 256):7.1f} Hz)")
    print(f"  Peak magnitude: {peak_magnitude:.1f}")
    print(f"  Expected mag:   {expected_magnitude:.1f}")
    
    if abs(peak_freq - freq) < freq * 0.1:  # Within 10%
        print(f"  ✓ PASS - Detected frequency matches")
    else:
        print(f"  ✗ FAIL - Frequency mismatch!")
    
    print()

print("========================================")
print("NOTES")
print("========================================")
print("At 48 kHz sample rate with 256-point FFT:")
print("  Bin spacing: 48000 / 256 = 187.5 Hz/bin")
print("  Valid bins: 0-128 (conjugate symmetry)")
print("  Bin 0: DC (0 Hz)")
print("  Bin 128: Nyquist (24 kHz)")
print()

PYTHON_ANALYSIS
else
    echo "[WARN] Python3 not available for analysis"
fi

echo ""
echo "=========================================="
echo "Test Complete"
echo "=========================================="
echo "Raw audio saved to: $OUTPUT_FILE"
echo "for manual analysis if needed"
