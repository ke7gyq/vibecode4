#!/usr/bin/env python3
"""Simple UDP audio recorder for testing."""

import socket
import struct
import wave
import time
import sys

PICO_IP = '192.168.12.200'
PICO_PORT = 5001
OUTPUT_FILE = 'test_recording.wav'
DURATION_SECONDS = 20
SAMPLE_RATE = 48000
FRAME_SIZE = 528  # Samples per frame

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
sock.settimeout(1.0)

print(f"[INFO] Opening {OUTPUT_FILE} for writing...")
wav_file = wave.open(OUTPUT_FILE, 'wb')
wav_file.setnchannels(1)  # Mono
wav_file.setsampwidth(2)  # 16-bit
wav_file.setframerate(SAMPLE_RATE)

# Send registration
print(f"[INFO] Sending REGISTER packet to {PICO_IP}:{PICO_PORT}...")
sock.sendto(b'REGISTER:48000:1', (PICO_IP, PICO_PORT))

start_time = time.time()
frames_received = 0
total_samples = 0

print(f"[INFO] Listening for audio (duration: {DURATION_SECONDS}s)...")

try:
    while time.time() - start_time < DURATION_SECONDS:
        try:
            data, addr = sock.recvfrom(4096)
            
            # Expect at least frame_size * 2 bytes (16-bit samples)
            frame_bytes = FRAME_SIZE * 2
            if len(data) >= frame_bytes:
                # Write raw audio data to WAV file
                wav_file.writeframes(data[:frame_bytes])
                frames_received += 1
                total_samples += FRAME_SIZE
                
                if frames_received % 10 == 0:
                    elapsed = time.time() - start_time
                    rate = total_samples / elapsed
                    print(f"[INFO] {frames_received} frames ({total_samples} samples) - Rate: {rate:.0f} Hz")
        
        except socket.timeout:
            print(f"[WARNING] No data received (timeout)")
            continue

except KeyboardInterrupt:
    print(f"\n[INFO] Interrupted by user")

wav_file.close()
sock.close()

elapsed = time.time() - start_time
print(f"\n[DONE] Recorded {frames_received} frames ({total_samples} samples) in {elapsed:.1f}s")
print(f"[INFO] Saved to: {OUTPUT_FILE}")
print(f"[INFO] Expected duration: {total_samples / SAMPLE_RATE:.1f}s")
