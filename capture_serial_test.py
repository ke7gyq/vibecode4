#!/usr/bin/env python3
"""
Capture serial output while running UDP test
Usage: python3 capture_serial_test.py [duration_seconds]
"""

import serial
import threading
import subprocess
import time
import sys

def capture_serial(port="/dev/ttyACM0", baud=115200, duration=20):
    """Capture serial output for specified duration"""
    try:
        ser = serial.Serial(port, baud, timeout=1)
        print(f"[INFO] Connected to {port} at {baud} baud")
        print(f"[INFO] Capturing output for {duration} seconds...\n")
        print("=" * 80)
        
        start_time = time.time()
        while time.time() - start_time < duration:
            try:
                if ser.in_waiting:
                    line = ser.readline().decode('utf-8', errors='ignore')
                    if line:
                        print(line, end='')
                        sys.stdout.flush()
            except Exception as e:
                print(f"[ERROR] Reading serial: {e}")
                break
        
        print("=" * 80)
        print("[INFO] Capture complete\n")
        ser.close()
        
    except Exception as e:
        print(f"[ERROR] Failed to open serial port: {e}")
        sys.exit(1)

def main():
    duration = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    
    # Give a moment for user to start the test
    print("[INFO] Starting serial capture...")
    print("[INFO] Run UDP test in another terminal now:")
    print("[INFO]   python3 utils/udp_audio_client.py --host 192.168.12.200 --port 5001 --duration", duration)
    print()
    
    capture_serial(duration=duration)

if __name__ == "__main__":
    main()
