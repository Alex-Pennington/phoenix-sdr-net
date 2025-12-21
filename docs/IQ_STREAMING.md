# I/Q Streaming Protocol

Binary protocol for streaming I/Q samples over TCP.

---

## Connection Flow

1. Client connects to I/Q port (default 4536)
2. Server sends stream header (PHXI)
3. Server sends data frames (IQDQ) continuously
4. Server sends metadata updates (META) on parameter changes
5. Client disconnects when done

---

## Packet Types

### Stream Header (PHXI)

Sent once at connection start. Describes stream parameters.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       4     magic           0x50485849 ("PHXI")
4       4     version         Protocol version (1)
8       4     sample_rate     Samples per second (Hz)
12      4     sample_format   1=S16, 2=F32, 3=U8
16      4     center_freq_lo  Center frequency, low 32 bits
20      4     center_freq_hi  Center frequency, high 32 bits
24      4     gain_reduction  IF gain reduction (dB)
28      4     lna_state       LNA attenuation state (0-8)
```

Total: 32 bytes

### Data Frame (IQDQ)

Sent continuously during streaming. Contains I/Q sample pairs.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       4     magic           0x49514451 ("IQDQ")
4       4     sequence        Frame sequence number
8       4     num_samples     Number of I/Q pairs
12      4     flags           Bit 0: overload detected
16      N     samples         I/Q data (format per header)
```

Header: 16 bytes, followed by `num_samples * 2 * sample_size` bytes.

**Sample Formats:**
- S16: 2 bytes I, 2 bytes Q (little-endian signed)
- F32: 4 bytes I, 4 bytes Q (IEEE 754 float)
- U8: 1 byte I, 1 byte Q (unsigned, 128 = zero)

### Metadata Update (META)

Sent when parameters change during streaming.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       4     magic           0x4D455441 ("META")
4       4     sample_rate     New sample rate
8       4     sample_format   New format
12      4     center_freq_lo  New frequency, low 32 bits
16      4     center_freq_hi  New frequency, high 32 bits
20      4     gain_reduction  New IF gain
24      4     lna_state       New LNA state
28      4     reserved
```

Total: 32 bytes

---

## Flags Field

| Bit | Name | Description |
|-----|------|-------------|
| 0 | OVERLOAD | ADC overload detected in this frame |
| 1-31 | Reserved | Must be zero |

---

## Typical Frame Sizes

| Sample Rate | Frame Samples | Frame Size (S16) |
|-------------|---------------|------------------|
| 2 MHz | 8192 | 32,784 bytes |
| 50 kHz | 2048 | 8,208 bytes |
| 12 kHz | 512 | 2,064 bytes |

---

## Example Client (Python)

```python
import socket
import struct

def connect_iq(host, port=4536):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    
    # Read header
    header = sock.recv(32)
    magic, version, rate, fmt, freq_lo, freq_hi, gain, lna = \
        struct.unpack('<IIIIIIII', header)
    
    assert magic == 0x50485849, "Invalid header"
    freq = freq_lo | (freq_hi << 32)
    print(f"Stream: {rate} Hz, freq={freq}, gain={gain} dB")
    
    # Read frames
    while True:
        frame_hdr = sock.recv(16)
        if len(frame_hdr) < 16:
            break
        magic, seq, num_samples, flags = \
            struct.unpack('<IIII', frame_hdr)
        
        if magic == 0x49514451:  # IQDQ
            data_size = num_samples * 4  # S16 format
            data = sock.recv(data_size)
            # Process samples...
        elif magic == 0x4D455441:  # META
            # Read remaining 16 bytes of metadata
            meta_rest = sock.recv(16)
            # Update parameters...
    
    sock.close()
```

---

## Bandwidth Requirements

| Rate | Format | Bandwidth |
|------|--------|-----------|
| 2 MHz | S16 | ~8 MB/s |
| 50 kHz | F32 | ~400 KB/s |
| 12 kHz | F32 | ~100 KB/s |

---

## Error Handling

- **Connection reset:** Reconnect and await new header
- **Sequence gap:** Log warning, continue processing
- **Unknown magic:** Skip 4 bytes, resync on next magic
- **Overload flag:** Reduce gain or notify user
