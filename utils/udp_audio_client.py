#!/usr/bin/env python3
"""
UDP Audio Client - Receives audio from Raspberry Pi Pico via UDP
Saves audio to WAV file and displays statistics
"""

import socket
import struct
import wave
import time
import argparse
import sys

def main():
    parser = argparse.ArgumentParser(description='UDP Audio Client - Receive audio from RP2350')
    parser.add_argument('--host', default='192.168.12.200', help='Pico IP address')
    parser.add_argument('--port', type=int, default=5001, help='UDP audio port')
    parser.add_argument('--output', default='audio.wav', help='Output WAV file')
    parser.add_argument('--duration', type=int, default=15, help='Capture duration in seconds')
    parser.add_argument('--sample-rate', type=int, default=48000, help='Expected sample rate (Hz)')
    parser.add_argument('--frame-size', type=int, default=528, help='Samples per frame')
    
    args = parser.parse_args()
    
    print(f"[INFO] UDP Audio Client")
    print(f"[INFO] Target: {args.host}:{args.port}")
    print(f"[INFO] Expected sample rate: {args.sample_rate} Hz")
    print(f"[INFO] Frame size: {args.frame_size} samples")
    print(f"[INFO] Output: {args.output}")
    print(f"[INFO] Duration: {args.duration} seconds")
    print(f"[INFO] Press Ctrl+C to stop capture\n")
    
    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)  # 1MB receive buffer
    sock.settimeout(0.5)  # Shorter timeout for better responsiveness
    
    # Send initial packet to register with server
    try:
        sock.sendto(b'HELLO', (args.host, args.port))
        print(f"[INFO] Sent registration packet to {args.host}:{args.port}")
    except Exception as e:
        print(f"[ERROR] Failed to send registration: {e}")
        return 1
    
    # Open WAV file for writing
    wav_file = wave.open(args.output, 'wb')
    wav_file.setnchannels(1)  # Mono
    wav_file.setsampwidth(2)  # 16-bit
    wav_file.setframerate(args.sample_rate)
    
    # Statistics
    start_time = time.time()
    total_samples = 0
    total_frames = 0
    last_stats_time = start_time
    frame_size_bytes = args.frame_size * 2  # 16-bit samples
    lost_frames = 0
    last_seq = None
    timeout_count = 0
    max_timeouts = 0
    
    try:
        print(f"[INFO] Listening for audio packets (Frame size: {frame_size_bytes} bytes)...")
        while True:
            # Check if we've exceeded duration
            elapsed = time.time() - start_time
            if elapsed > args.duration:
                print(f"\n[INFO] Duration limit reached ({args.duration}s)")
                break
            
            try:
                # Receive UDP packet with timeout
                data, (src_ip, src_port) = sock.recvfrom(65536)
                timeout_count = 0  # Reset timeout counter on successful receive
                
                if len(data) >= frame_size_bytes:
                    # Extract audio samples (16-bit signed, little-endian)
                    samples = struct.unpack(f'<{args.frame_size}h', data[:frame_size_bytes])
                    
                    # Write to WAV file
                    wav_file.writeframes(struct.pack(f'<{args.frame_size}h', *samples))
                    
                    # **CRITICAL FIX**: Flush the underlying file buffer after each write
                    # This prevents buffer overflow issues at ~12 seconds
                    # wave.Wave_write uses _file internally, so flush that
                    try:
                        if hasattr(wav_file, '_file'):
                            wav_file._file.flush()
                    except (AttributeError, ValueError):
                        pass
                    
                    total_samples += args.frame_size
                    total_frames += 1
                    
                    # Print stats every second
                    now = time.time()
                    if now - last_stats_time >= 1.0:
                        elapsed_seconds = now - start_time
                        sample_rate = total_samples / elapsed_seconds if elapsed_seconds > 0 else 0
                        print(f"[CAPTURE] {total_samples} samples ({total_samples * 2} bytes), "
                              f"frames: {total_frames}, rate: {sample_rate:.0f} Hz ({elapsed_seconds:.1f}s)")
                        last_stats_time = now
                        max_timeouts = max(max_timeouts, timeout_count)
                else:
                    # Frame too small, skip with warning
                    print(f"[WARNING] Received {len(data)} bytes, expected at least {frame_size_bytes} bytes")
            
            except socket.timeout:
                # No data available within timeout period
                timeout_count += 1
                
                # Alert if timeouts are happening frequently (indicates network issues)
                if timeout_count >= 3:
                    elapsed = time.time() - start_time
                    print(f"[WARNING] No data for {timeout_count * 0.5:.1f}s at elapsed {elapsed:.1f}s - "
                          f"Network issue or Pico not sending? "
                          f"(Received {total_frames} frames so far)")
                    timeout_count = 0  # Reset to avoid spam
                
                pass  # Continue waiting
    
    except KeyboardInterrupt:
        print(f"\n[INFO] Capture stopped by user")
    
    except Exception as e:
        print(f"[ERROR] Receive error: {e}")
        import traceback
        traceback.print_exc()
    
    finally:
        # Flush and close file to ensure all data is written
        try:
            if hasattr(wav_file, '_file'):
                wav_file._file.flush()
        except (AttributeError, ValueError):
            pass
        wav_file.close()
        sock.close()
        
        # Final statistics
        elapsed = time.time() - start_time
        if elapsed > 0:
            avg_rate = total_samples / elapsed
            print(f"\n[INFO] Capture complete!")
            print(f"[INFO] Total samples: {total_samples}")
            print(f"[INFO] Total frames: {total_frames}")
            print(f"[INFO] Average rate: {avg_rate:.0f} Hz")
            print(f"[INFO] Elapsed time: {elapsed:.2f}s")
            print(f"[INFO] File: {args.output}")
            print(f"[INFO] Lost frames: {lost_frames}")
        
        return 0

if __name__ == '__main__':
    sys.exit(main())

