#!/bin/bash
# Integrated tone test: play tone through speakers, capture from Pico, analyze

set -e

PICO_HOST="192.168.12.200"
PICO_PORT="5001"
TONE_FREQ=${1:-1000}      # Default 1000 Hz
TONE_DURATION=${2:-5}     # Default 5 seconds
CAPTURE_DURATION=$((TONE_DURATION + 2))  # Capture 2 seconds before/after
SAMPLE_RATE=48000
FRAME_SIZE=528

OUTPUT_RAW="/tmp/tone_test_raw.raw"
OUTPUT_WAV="/tmp/tone_test.wav"

echo "========================================"
echo "VibeCode4 Tone Test"
echo "========================================"
echo "Tone frequency: ${TONE_FREQ} Hz"
echo "Tone duration: ${TONE_DURATION} seconds"
echo "Capture duration: ${CAPTURE_DURATION} seconds"
echo "Output WAV: ${OUTPUT_WAV}"
echo ""

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Start audio capture in background
echo "[1/3] Starting UDP audio capture..."
"${SCRIPT_DIR}/udp_audio_client" \
    --host "${PICO_HOST}" \
    --port "${PICO_PORT}" \
    --duration "${CAPTURE_DURATION}" \
    --output "${OUTPUT_RAW}" &

CAPTURE_PID=$!
sleep 1  # Let capture start

# Play tone
echo "[2/3] Playing ${TONE_FREQ} Hz tone for ${TONE_DURATION} seconds..."
cd "${SCRIPT_DIR}"
python3 playTone.py --frequency "${TONE_FREQ}" --duration "${TONE_DURATION}"

# Wait for capture to finish
wait $CAPTURE_PID
echo ""

# Convert raw to WAV
echo "[3/3] Converting raw audio to WAV format..."
python3 << 'PYTHON_EOF'
import struct
import wave
import sys

# Read parameters
frame_size = 528
sample_rate = 48000
input_raw = "/tmp/tone_test_raw.raw"
output_wav = "/tmp/tone_test.wav"

# Count frames in raw file
import os
raw_size = os.path.getsize(input_raw)
num_frames = raw_size // (frame_size * 2)
print(f"  Raw file size: {raw_size} bytes ({num_frames} frames)")

# Create WAV file
with wave.open(output_wav, 'wb') as wav_file:
    wav_file.setnchannels(1)  # Mono
    wav_file.setsampwidth(2)  # 16-bit
    wav_file.setframerate(sample_rate)
    
    # Read raw data and write to WAV
    with open(input_raw, 'rb') as raw_file:
        data = raw_file.read()
        wav_file.writeframes(data)

print(f"  Saved WAV: {output_wav}")
print(f"  Duration: {num_frames * frame_size / sample_rate:.2f} seconds")
PYTHON_EOF

# Analyze the captured tone
echo ""
echo "========================================"
echo "ANALYSIS RESULTS"
echo "========================================"
cd "${SCRIPT_DIR}"
python3 analyzePitch.py "${OUTPUT_WAV}"

echo ""
echo "✓ Test complete!"
echo "  Raw audio: ${OUTPUT_RAW}"
echo "  WAV file: ${OUTPUT_WAV}"
