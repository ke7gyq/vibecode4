#!/usr/bin/env python3
"""
Telnet PCM Data Client with WAV File Capture

Connects to a telnet server and provides interactive commands for monitoring
PCM (Pulse Code Modulation) audio data from the vibecode4 device.

Usage:
    python3 telnet_pcm_client.py [--host HOST] [--port PORT] [--command COMMAND ...]

Arguments:
    --host HOST           Target server IP address (default: 192.168.12.200)
    --port PORT           Target server port (default: 5001)
    --command COMMAND     Execute command(s) non-interactively
                          If not specified, runs in interactive mode

Commands (when in interactive mode or with --command):
    help                  Show list of available commands
    showPcm               Display PCM data as 16-bit integers (16 per line)
    showAscii             Display raw data as ASCII characters
    filename <name>       Set output filename for WAV capture (default: audio.wav)
    captureWav            Capture audio stream to WAV file
    exit / quit           Disconnect and exit
    <Enter>               Stop current operation and return to prompt

Examples:
    # Interactive mode on port 5001
    python3 telnet_pcm_client.py --port 5001

    # Show PCM data and exit
    python3 telnet_pcm_client.py --port 5001 --command showPcm

    # Capture audio to ding.wav and exit
    python3 telnet_pcm_client.py --port 5001 --command filename ding.wav --command captureWav

    # Capture with custom host
    python3 telnet_pcm_client.py --host 192.168.1.100 --port 5001 \\
        --command filename myaudio.wav --command captureWav
"""

import socket
import sys
import argparse
import struct
import os
from typing import Optional


class TelnetPCMClient:
    """Telnet client for monitoring PCM audio data"""
    
    # WAV file parameters
    SAMPLE_RATE = 44100  # Hz
    CHANNELS = 1         # Mono
    BIT_DEPTH = 16       # bits
    
    def __init__(self, host: str = "192.168.12.200", port: int = 5001):
        """Initialize telnet client
        
        Args:
            host: Server IP address
            port: Server port number
        """
        self.host = host
        self.port = port
        self.socket: Optional[socket.socket] = None
        self.connected = False
        self.output_filename = "audio.wav"
        self.bytes_captured = 0
    
    def connect(self) -> bool:
        """Connect to telnet server
        
        Returns:
            True if connection successful, False otherwise
        """
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(5.0)
            print(f"[STATUS] Connecting to {self.host}:{self.port}...")
            self.socket.connect((self.host, self.port))
            self.connected = True
            print(f"[STATUS] ✓ Connected to {self.host}:{self.port}")
            return True
        except socket.timeout:
            print(f"[STATUS] ✗ Connection timeout to {self.host}:{self.port}")
            return False
        except ConnectionRefusedError:
            print(f"[STATUS] ✗ Connection refused by {self.host}:{self.port}")
            return False
        except Exception as e:
            print(f"[STATUS] ✗ Connection error: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from server"""
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
            self.connected = False
            print("[STATUS] Disconnected")
    
    def show_help(self):
        """Display available commands"""
        help_text = """
Available Commands:
  help                   - Show this help message
  showPcm                - Display PCM audio data as 16-bit hexadecimal integers
                           (16 values per line, press Enter to stop)
  showAscii              - Display raw data as ASCII characters
                           (press Enter to stop)
  filename <name>        - Set output filename for WAV capture
  captureWav             - Capture audio stream to WAV file
                           Shows byte count during capture
  exit / quit            - Disconnect and exit
  <Enter>                - Return to command prompt
"""
        print(help_text)
    
    def create_wav_header(self, num_samples: int) -> bytes:
        """Create a WAV file header for the given number of samples
        
        Args:
            num_samples: Number of 16-bit samples
            
        Returns:
            44-byte WAV header
        """
        bytes_per_sample = self.BIT_DEPTH // 8
        byte_rate = self.SAMPLE_RATE * self.CHANNELS * bytes_per_sample
        block_align = self.CHANNELS * bytes_per_sample
        data_size = num_samples * bytes_per_sample
        file_size = 36 + data_size  # Total size - 8 bytes (for RIFF header)
        
        # RIFF header
        header = b"RIFF"
        header += struct.pack("<I", file_size)
        header += b"WAVE"
        
        # fmt subchunk
        header += b"fmt "
        header += struct.pack("<I", 16)  # Subchunk1 size (16 for PCM)
        header += struct.pack("<H", 1)   # Audio format (1 = PCM)
        header += struct.pack("<H", self.CHANNELS)
        header += struct.pack("<I", self.SAMPLE_RATE)
        header += struct.pack("<I", byte_rate)
        header += struct.pack("<H", block_align)
        header += struct.pack("<H", self.BIT_DEPTH)
        
        # data subchunk
        header += b"data"
        header += struct.pack("<I", data_size)
        
        return header
    
    def update_wav_header(self, filepath: str, num_samples: int):
        """Update the WAV file header with correct data size
        
        Args:
            filepath: Path to WAV file
            num_samples: Number of samples written to file
        """
        try:
            header = self.create_wav_header(num_samples)
            with open(filepath, "r+b") as f:
                f.seek(0)
                f.write(header)
            print(f"[STATUS] ✓ WAV header updated ({num_samples} samples)")
        except Exception as e:
            print(f"[STATUS] ✗ Error updating WAV header: {e}")
    
    def capture_wav(self):
        """Capture audio stream to WAV file
        
        Reads raw 16-bit PCM samples from the server and writes them to
        a WAV file with proper header and metadata.
        """
        if not self.connected or not self.socket:
            print("[ERROR] Not connected. Use 'connect' command first.")
            return
        
        print(f"[STATUS] Capturing audio to '{self.output_filename}'...")
        print("[INFO] Press Enter to stop capture")
        
        wav_file = None
        import time
        try:
            # Set socket to non-blocking for interactive input
            self.socket.setblocking(False)
            
            # Open WAV file for writing (start with placeholder header)
            wav_file = open(self.output_filename, "wb")
            
            # Write placeholder header (will be updated at the end)
            placeholder_header = self.create_wav_header(0)
            wav_file.write(placeholder_header)
            
            buffer = b""
            samples_written = 0
            bytes_read_total = 0
            last_report_samples = 0
            start_time = time.time()
            
            while True:
                try:
                    # Try to receive data from socket
                    chunk = self.socket.recv(4096)
                    if not chunk:
                        print("[STATUS] Connection closed by server")
                        break
                    
                    buffer += chunk
                    bytes_read_total += len(chunk)
                    
                    # Process complete 2-byte pairs (16-bit samples)
                    while len(buffer) >= 2:
                        # Extract 2 bytes
                        pcm_bytes = buffer[:2]
                        buffer = buffer[2:]
                        
                        # Write to WAV file
                        wav_file.write(pcm_bytes)
                        samples_written += 1
                        
                        # Display status periodically (every 4410 samples = ~0.1 sec at 44.1kHz)
                        if samples_written % 4410 == 0:
                            elapsed = time.time() - start_time
                            sample_rate = (samples_written / elapsed) if elapsed > 0 else 0
                            print(f"[CAPTURE] {samples_written} samples ({bytes_read_total} bytes), "
                                  f"rate: {sample_rate:.0f} Hz ({elapsed:.1f}s)")
                
                except BlockingIOError:
                    # No data available on socket yet
                    pass
                
                # Check for user input (Enter key)
                try:
                    import select
                    rlist, _, _ = select.select([sys.stdin], [], [], 0.1)
                    if rlist:
                        user_input = sys.stdin.readline()
                        print("[STATUS] Capture stopped by user")
                        break
                except:
                    # select not available on all platforms, just keep trying
                    pass
        
        except Exception as e:
            print(f"[ERROR] Error during WAV capture: {e}")
        
        finally:
            # Return to blocking mode
            try:
                self.socket.setblocking(True)
            except:
                pass
            
            # Close the file and update header
            if wav_file:
                try:
                    wav_file.close()
                    elapsed = time.time() - start_time
                    actual_rate = (samples_written / elapsed) if elapsed > 0 else 0
                    print(f"[STATUS] Closing file...")
                    self.update_wav_header(self.output_filename, samples_written)
                    print(f"[STATUS] ✓ Captured {samples_written} samples ({bytes_read_total} bytes)")
                    print(f"[STATUS] ✓ Capture time: {elapsed:.2f}s, actual rate: {actual_rate:.0f} Hz")
                    print(f"[STATUS] ✓ Expected rate: 48000 Hz")
                    print(f"[STATUS] ✓ File saved: {self.output_filename}")
                    self.bytes_captured = samples_written
                except Exception as e:
                    print(f"[ERROR] Error closing file: {e}")
    
    def show_pcm(self):
        """Display PCM data from server
        
        Reads data in 2-byte chunks, converts to 16-bit integers,
        displays 16 values per line. User can press Enter to stop.
        """
        if not self.connected or not self.socket:
            print("[ERROR] Not connected.")
            return
        
        print("[STATUS] PCM Data (press Enter to stop):")
        print("-" * 80)
        
        try:
            self.socket.setblocking(False)
            
            pcm_values = []
            buffer = b""
            bytes_read = 0
            
            while True:
                try:
                    chunk = self.socket.recv(4096)
                    if not chunk:
                        print("\n[STATUS] Connection closed by server")
                        break
                    buffer += chunk
                    bytes_read += len(chunk)
                    
                    while len(buffer) >= 2:
                        pcm_bytes = buffer[:2]
                        buffer = buffer[2:]
                        pcm_value = struct.unpack('<H', pcm_bytes)[0]
                        pcm_values.append(pcm_value)
                        
                        if len(pcm_values) == 16:
                            print("  ".join(f"0x{v:04x}" for v in pcm_values))
                            pcm_values = []
                
                except BlockingIOError:
                    pass
                
                try:
                    import select
                    rlist, _, _ = select.select([sys.stdin], [], [], 0.1)
                    if rlist:
                        user_input = sys.stdin.readline()
                        print("-" * 80)
                        print(f"[INFO] {bytes_read} bytes read")
                        if pcm_values:
                            print("  ".join(f"0x{v:04x}" for v in pcm_values))
                        break
                except:
                    pass
        
        except Exception as e:
            print(f"[ERROR] Error receiving PCM data: {e}")
        
        finally:
            try:
                self.socket.setblocking(True)
            except:
                pass
    
    def show_ascii(self):
        """Display raw data as ASCII characters"""
        if not self.connected or not self.socket:
            print("[ERROR] Not connected.")
            return
        
        print("[STATUS] ASCII Data (press Enter to stop):")
        print("-" * 80)
        
        try:
            self.socket.setblocking(False)
            
            buffer = b""
            line = ""
            bytes_read = 0
            
            while True:
                try:
                    chunk = self.socket.recv(4096)
                    if not chunk:
                        print("\n[STATUS] Connection closed by server")
                        break
                    buffer += chunk
                    bytes_read += len(chunk)
                    
                    for byte in buffer:
                        if 32 <= byte <= 126:
                            char = chr(byte)
                        else:
                            char = '.'
                        
                        line += char
                        
                        if len(line) >= 80:
                            print(line)
                            line = ""
                    
                    buffer = b""
                
                except BlockingIOError:
                    pass
                
                try:
                    import select
                    rlist, _, _ = select.select([sys.stdin], [], [], 0.1)
                    if rlist:
                        user_input = sys.stdin.readline()
                        print("-" * 80)
                        print(f"[INFO] {bytes_read} bytes read")
                        if line:
                            print(line)
                        break
                except:
                    pass
        
        except Exception as e:
            print(f"[ERROR] Error receiving ASCII data: {e}")
        
        finally:
            try:
                self.socket.setblocking(True)
            except:
                pass
    
    def run_command(self, command: str) -> bool:
        """Execute a single command non-interactively
        
        Args:
            command: Command to execute
            
        Returns:
            True if command executed successfully
        """
        parts = command.strip().split(None, 1)
        cmd = parts[0].lower() if parts else ""
        arg = parts[1] if len(parts) > 1 else None
        
        if cmd in ("help", "h", "?"):
            self.show_help()
            return True
        
        elif cmd == "showpcm":
            if not self.connected:
                print("[ERROR] Not connected")
                return False
            self.show_pcm()
            return True
        
        elif cmd == "showascii":
            if not self.connected:
                print("[ERROR] Not connected")
                return False
            self.show_ascii()
            return True
        
        elif cmd == "filename":
            if not arg:
                print(f"[INFO] Output filename is: {self.output_filename}")
                print("[USAGE] filename <name>")
                return False
            self.output_filename = arg
            print(f"[STATUS] Output filename set to: {self.output_filename}")
            return True
        
        elif cmd == "capturewav":
            if not self.connected:
                print("[ERROR] Not connected")
                return False
            self.capture_wav()
            return True
        
        elif cmd in ("exit", "quit", "q"):
            print("[STATUS] Exiting...")
            return True
        
        else:
            print(f"[ERROR] Unknown command: '{cmd}'")
            print("Available: help, showPcm, showAscii, filename, captureWav, exit")
            return False
    
    def run_interactive(self):
        """Main command loop"""
        if not self.connect():
            return
        
        print("\n[STATUS] Telnet PCM Client")
        print("[INFO] Type 'help' for available commands\n")
        
        try:
            while self.connected:
                try:
                    user_input = input("pcm> ").strip()
                    
                    if not user_input:
                        continue
                    
                    self.run_command(user_input)
                
                except KeyboardInterrupt:
                    print("\n[STATUS] Exiting...")
                    break
                except Exception as e:
                    print(f"[ERROR] {e}")
        
        finally:
            self.disconnect()


def main():
    """Parse arguments and run client"""
    parser = argparse.ArgumentParser(
        description="Telnet client for PCM audio monitoring and WAV file capture",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Interactive mode
  python3 telnet_pcm_client.py --port 5001

  # Show PCM data
  python3 telnet_pcm_client.py --port 5001 --command showPcm

  # Capture WAV file
  python3 telnet_pcm_client.py --port 5001 --command filename ding.wav --command captureWav

  # Multiple commands
  python3 telnet_pcm_client.py --host 192.168.1.100 --port 5001 \\
      --command filename myaudio.wav --command captureWav
"""
    )
    
    parser.add_argument(
        "--host",
        type=str,
        default="192.168.12.200",
        help="Target server IP address (default: 192.168.12.200)"
    )
    parser.add_argument(
        "--port",
        type=int,
        default=5001,
        help="Target server port (default: 5001)"
    )
    parser.add_argument(
        "--command",
        nargs='+',
        action="append",
        dest="commands",
        help="Command and its arguments (can specify multiple times)"
    )
    
    args = parser.parse_args()
    
    # Validate port
    if not (1 <= args.port <= 65535):
        print(f"[ERROR] Invalid port number {args.port}. Must be between 1 and 65535.")
        sys.exit(1)
    
    # Create client
    client = TelnetPCMClient(host=args.host, port=args.port)
    
    # If commands specified, run them non-interactively
    if args.commands:
        if not client.connect():
            sys.exit(1)
        try:
            # Each cmd is now a list of argument parts; join them to form command string
            for cmd_parts in args.commands:
                cmd_str = ' '.join(cmd_parts)
                client.run_command(cmd_str)
        finally:
            client.disconnect()
    else:
        # Otherwise, run interactive mode
        client.run_interactive()


if __name__ == "__main__":
    main()