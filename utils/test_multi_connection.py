#!/usr/bin/env python3
"""
Robust UDP Audio Connection Test
Tests multiple sequential connections with optional server restart
"""

import socket
import subprocess
import time
import sys
import os

def run_single_test(host, port, duration=10, test_num=1):
    """Run a single UDP audio test"""
    print(f"\n{'='*60}")
    print(f"Test {test_num}: {duration}s capture")
    print(f"{'='*60}")
    
    try:
        result = subprocess.run(
            ['python3', 'utils/udp_audio_client.py', 
             '--host', host, 
             '--port', str(port),
             '--duration', str(duration)],
            cwd='/home/doug/rpi-pico/vibecode4',
            capture_output=True,
            text=True,
            timeout=duration + 10
        )
        
        # Parse output
        output = result.stdout + result.stderr
        lines = output.split('\n')
        
        rate_hz = 0
        frames = 0
        for line in lines:
            if 'Average rate:' in line:
                try:
                    rate_hz = float(line.split('Average rate:')[1].split('Hz')[0].strip())
                except:
                    pass
            if 'Total frames:' in line:
                try:
                    frames = int(line.split('Total frames:')[1].strip())
                except:
                    pass
        
        print(f"Result: {rate_hz:.0f} Hz ({frames} frames)")
        return rate_hz, frames
        
    except subprocess.TimeoutExpired:
        print("TIMEOUT - test took too long")
        return 0, 0
    except Exception as e:
        print(f"ERROR: {e}")
        return 0, 0

def main():
    host = '192.168.12.200'
    port = 5001
    
    print("UDP Audio Multi-Connection Stability Test")
    print(f"Target: {host}:{port}")
    print("Running 10-second tests...\n")
    
    # Test sequence: 5 connections back-to-back, 10 seconds each
    results = []
    for i in range(1, 6):
        rate, frames = run_single_test(host, port, duration=10, test_num=i)
        results.append((i, rate, frames))
        
        # Wait between tests
        if i < 5:
            print(f"Waiting 5 seconds before test {i+1}...")
            time.sleep(5)
    
    # Summary
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print(f"{'Test':<6} {'Rate (Hz)':<15} {'Frames':<10} {'Status':<20}")
    print("-" * 60)
    
    all_good = True
    for test_num, rate, frames in results:
        status = "✓ PASS" if rate > 40000 else "✗ FAIL" if rate == 0 else f"⚠ DEGRADE ({rate:.0f})"
        print(f"{test_num:<6} {rate:>12.0f}    {frames:>8}    {status:<20}")
        if rate < 40000:
            all_good = False
    
    print("-" * 60)
    if all_good:
        print("✓ ALL TESTS PASSED - UDP stable for multiple connections")
    else:
        print("✗ FAILURES DETECTED - Need to debug reconnection logic")
    
    return 0 if all_good else 1

if __name__ == '__main__':
    sys.exit(main())
