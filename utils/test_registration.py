#!/usr/bin/env python3
"""
Test UDP registration by sending HELLO packet multiple times
"""

import socket
import time
import sys

def send_hello(host, port, count=5, delay=0.5):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    for i in range(count):
        try:
            sock.sendto(b'HELLO', (host, port))
            print(f"[{i+1}] Sent HELLO to {host}:{port}")
            time.sleep(delay)
        except Exception as e:
            print(f"[ERROR] Failed to send: {e}")
            return False
    
    sock.close()
    return True

if __name__ == '__main__':
    host = sys.argv[1] if len(sys.argv) > 1 else '192.168.12.200'
    port = 5001
    
    print(f"Sending HELLO packets to {host}:{port}")
    send_hello(host, port, count=5, delay=0.3)
    print("Done. Now try UDP audio client.")
