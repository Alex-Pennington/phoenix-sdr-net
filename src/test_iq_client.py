#!/usr/bin/env python3
"""Simple test client for I/Q streaming port."""

import socket
import struct
import sys

IQ_PORT = 4536
CONTROL_PORT = 4535

# Magic numbers
MAGIC_HEADER = 0x50485849  # 'PHXI'
MAGIC_DATA = 0x49514451    # 'IQDQ'
MAGIC_META = 0x4D455441    # 'META'

def main():
    print("Connecting to control port...")
    ctrl = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ctrl.connect(("127.0.0.1", CONTROL_PORT))

    print("Connecting to I/Q port...")
    iq = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    iq.connect(("127.0.0.1", IQ_PORT))

    # Read stream header (32 bytes)
    print("Reading stream header...")
    header = iq.recv(32)
    if len(header) < 32:
        print(f"Short header: {len(header)} bytes")
        return 1

    magic, version, sample_rate, sample_format = struct.unpack("<IIII", header[:16])
    freq_lo, freq_hi = struct.unpack("<II", header[16:24])
    center_freq = freq_lo | (freq_hi << 32)

    print(f"  Magic: 0x{magic:08X} {'(PHXI)' if magic == MAGIC_HEADER else '(UNKNOWN)'}")
    print(f"  Version: {version}")
    print(f"  Sample Rate: {sample_rate} Hz")
    print(f"  Sample Format: {sample_format} (0=S16)")
    print(f"  Center Freq: {center_freq} Hz ({center_freq/1e6:.3f} MHz)")

    # Start streaming via control port
    print("\nStarting stream via control port...")
    ctrl.send(b"START\n")
    resp = ctrl.recv(256)
    print(f"Control response: {resp.decode().strip()}")

    # Read a few data frames
    print("\nReading I/Q data frames...")
    for i in range(5):
        # Read frame header (16 bytes)
        frame_hdr = iq.recv(16)
        if len(frame_hdr) < 16:
            print(f"  Frame {i}: short header ({len(frame_hdr)} bytes)")
            break

        magic, seq, num_samples, flags = struct.unpack("<IIII", frame_hdr)
        if magic != MAGIC_DATA:
            print(f"  Frame {i}: bad magic 0x{magic:08X}")
            break

        # Read sample data
        data_size = num_samples * 4  # 2 bytes I + 2 bytes Q per sample
        data = b""
        while len(data) < data_size:
            chunk = iq.recv(data_size - len(data))
            if not chunk:
                break
            data += chunk

        print(f"  Frame {i}: seq={seq}, samples={num_samples}, flags=0x{flags:X}, data={len(data)} bytes")

    # Stop streaming
    print("\nStopping stream...")
    ctrl.send(b"STOP\n")
    resp = ctrl.recv(256)
    print(f"Control response: {resp.decode().strip()}")

    ctrl.send(b"QUIT\n")
    ctrl.close()
    iq.close()

    print("\nTest complete!")
    return 0

if __name__ == "__main__":
    sys.exit(main())
