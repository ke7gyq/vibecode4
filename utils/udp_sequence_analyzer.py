#!/usr/bin/env python3
"""
UDP Sequence Number Analyzer for VibeCode4 Audio Streaming

Listens to UDP audio packets and analyzes sequence number patterns to diagnose frame loss.
Outputs statistics on loss distribution, timing, and patterns.

Usage:
    python3 udp_sequence_analyzer.py --host 192.168.12.200 --port 5001 --duration 30
"""

import socket
import struct
import time
import argparse
from collections import defaultdict


def analyze_udp_sequences(host, port, duration):
    """
    Listen to UDP packets, extract sequence numbers, and analyze loss patterns.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(2.0)

    print(f"[INFO] Listening on 0.0.0.0:{port} for {duration} seconds")
    print(f"[INFO] Sending registration to {host}:{port}...")
    
    # Send HELLO registration packet to request audio stream
    try:
        sock.sendto(b'HELLO\x00', (host, port))
        print(f"[INFO] Registration sent, waiting for stream...")
    except Exception as e:
        print(f"[WARNING] Could not send registration: {e}")
    
    time.sleep(0.5)  # Give RP2350 time to start sending
    print(f"[INFO] Extracting sequence numbers from UDP packets...")
    print()

    start_time = time.time()
    last_seq = None
    sequences = []
    gaps = []
    gap_sizes = defaultdict(int)
    consecutive_losses = []
    current_loss_streak = []
    receive_times = []
    
    packet_count = 0
    bytes_received = 0

    try:
        while time.time() - start_time < duration:
            try:
                data, (src_ip, src_port) = sock.recvfrom(2048)
                current_time = time.time() - start_time
                bytes_received += len(data)
                packet_count += 1
                receive_times.append(current_time)

                if len(data) < 4:
                    print(f"[ERROR] Packet too small ({len(data)} bytes)")
                    continue

                # Extract 4-byte LE sequence number
                seq_num = struct.unpack('<I', data[0:4])[0]
                sequences.append((seq_num, current_time))

                if last_seq is not None:
                    gap = seq_num - last_seq - 1
                    if gap > 0:
                        gaps.append((last_seq, seq_num, gap, current_time))
                        gap_sizes[gap] += 1
                        current_loss_streak.append(gap)
                    else:
                        if current_loss_streak:
                            consecutive_losses.append(sum(current_loss_streak))
                            current_loss_streak = []
                else:
                    print(f"[INFO] First packet received at t={current_time:.3f}s with seq={seq_num}")

                last_seq = seq_num

            except socket.timeout:
                continue

    except KeyboardInterrupt:
        print("\n[INFO] Interrupted by user")

    finally:
        sock.close()

    elapsed = time.time() - start_time

    # =======================
    # Analysis and Reporting
    # =======================
    print("\n" + "=" * 70)
    print("SEQUENCE NUMBER ANALYSIS RESULTS")
    print("=" * 70)

    total_seq_span = sequences[-1][0] - sequences[0][0] + 1 if sequences else 0
    total_expected_frames = total_seq_span
    total_received_frames = len(sequences)
    total_lost_frames = sum(g[2] for g in gaps)

    print(f"\n[SUMMARY]")
    print(f"  Duration:           {elapsed:.2f}s")
    print(f"  Packets received:   {packet_count}")
    print(f"  Bytes received:     {bytes_received:,}")
    print(f"  Avg packet rate:    {packet_count/elapsed:.1f} pkt/s")
    print(f"  Avg bandwidth:      {bytes_received/elapsed/1024:.1f} KB/s")

    print(f"\n[SEQUENCE COVERAGE]")
    print(f"  First sequence:     {sequences[0][0] if sequences else 'N/A'}")
    print(f"  Last sequence:      {sequences[-1][0] if sequences else 'N/A'}")
    print(f"  Total span:         {total_seq_span} frames")
    print(f"  Frames received:    {total_received_frames}")
    print(f"  Frames lost:        {total_lost_frames}")
    if total_expected_frames > 0:
        print(f"  Loss rate:          {100*total_lost_frames/total_expected_frames:.2f}%")
    else:
        print(f"  Loss rate:          N/A (no data received)")

    if gaps:
        print(f"\n[GAP STATISTICS]")
        print(f"  Total gap events:   {len(gaps)}")
        print(f"  Average gap size:   {total_lost_frames/len(gaps):.2f} frames")
        print(f"  Min gap size:       {min(g[2] for g in gaps)} frames")
        print(f"  Max gap size:       {max(g[2] for g in gaps)} frames")

        print(f"\n[GAP DISTRIBUTION]")
        for size in sorted(gap_sizes.keys()):
            count = gap_sizes[size]
            pct = 100 * count / len(gaps)
            bar = "█" * min(40, int(pct / 2.5))
            print(f"  Loss of {size:2d} frame(s): {count:3d} events ({pct:5.1f}%) {bar}")

        print(f"\n[LOSS CLUSTERING]")
        if consecutive_losses:
            total_consecutive_lost = sum(consecutive_losses)
            print(f"  Consecutive loss events:  {len(consecutive_losses)}")
            print(f"  Total frames in clusters: {total_consecutive_lost}")
            print(f"  Avg frames per cluster:   {total_consecutive_lost/len(consecutive_losses):.2f}")
            print(f"  Max frames in one cluster:{max(consecutive_losses)}")
        else:
            print(f"  No consecutive loss events")

        print(f"\n[TEMPORAL ANALYSIS]")
        if receive_times:
            # Calculate inter-arrival times
            inter_arrivals = [receive_times[i+1] - receive_times[i] for i in range(len(receive_times)-1)]
            avg_interval = sum(inter_arrivals) / len(inter_arrivals)
            expected_frame_time = 1.0 / 46  # ~46 kHz frame rate

            print(f"  Expected frame interval: {expected_frame_time*1000:.2f} ms")
            print(f"  Avg packet interval:     {avg_interval*1000:.2f} ms")
            print(f"  Min packet interval:     {min(inter_arrivals)*1000:.2f} ms")
            print(f"  Max packet interval:     {max(inter_arrivals)*1000:.2f} ms")

            # Detect stalls (gap > 2x expected interval)
            stall_threshold = expected_frame_time * 3
            stalls = [t for t in inter_arrivals if t > stall_threshold]
            if stalls:
                print(f"  Stall events (>{stall_threshold*1000:.1f}ms): {len(stalls)}")
                print(f"  Max stall duration:      {max(stalls)*1000:.1f} ms")

    print("\n[DETAILED GAP LOG (first 20)]")
    for i, (seq_before, seq_after, gap_size, gap_time) in enumerate(gaps[:20]):
        print(f"  Gap {i+1:2d}: seq {seq_before} → {seq_after} (lost {gap_size} frame(s)) at t={gap_time:.3f}s")

    if len(gaps) > 20:
        print(f"  ... and {len(gaps) - 20} more gaps")

    print("\n" + "=" * 70)


def main():
    parser = argparse.ArgumentParser(
        description='Analyze UDP sequence numbers to diagnose frame loss patterns'
    )
    parser.add_argument('--host', default='192.168.12.200', help='RP2350 IP address')
    parser.add_argument('--port', type=int, default=5001, help='UDP port')
    parser.add_argument('--duration', type=int, default=30, help='Listen duration in seconds')

    args = parser.parse_args()

    try:
        analyze_udp_sequences(args.host, args.port, args.duration)
    except Exception as e:
        print(f"[ERROR] {e}")
        import traceback
        traceback.print_exc()


if __name__ == '__main__':
    main()
