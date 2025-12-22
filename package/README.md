# phoenix-sdr-net

**Version:** v0.1.0  
**Part of:** Phoenix Nest MARS Communications Suite  
**Developer:** Alex Pennington (KY4OLB)

---

## Overview

Network streaming and relay tools for the Phoenix Nest SDR system. Enables distributed SDR processing with TCP-based I/Q streaming, signal splitting, and multi-client broadcast.

---

## Components

| Tool | Description |
|------|-------------|
| `sdr_server` | TCP control server for SDRplay RSP2 Pro with I/Q streaming |
| `signal_splitter` | Splits 2 MHz I/Q into detector (50 kHz) and display (12 kHz) paths |
| `signal_relay` | Broadcast relay for multi-client distribution |
| `wormhole` | MIL-STD-188-110A constellation display |
| `test_iq_client.py` | Python test client for I/Q streaming |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Phoenix SDR Network                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌──────────────┐                                                          │
│   │  SDRplay     │                                                          │
│   │  RSP2 Pro    │                                                          │
│   └──────┬───────┘                                                          │
│          │ USB                                                              │
│          ▼                                                                  │
│   ┌──────────────┐     Control: 4535        ┌──────────────┐               │
│   │  sdr_server  │◄─────────────────────────│   Clients    │               │
│   │              │     I/Q: 4536            │  (control)   │               │
│   └──────┬───────┘─────────────────────────►└──────────────┘               │
│          │ 2 MHz I/Q                                                        │
│          ▼                                                                  │
│   ┌──────────────────────────────────┐                                     │
│   │         signal_splitter          │                                     │
│   │  ┌─────────────┐ ┌─────────────┐ │                                     │
│   │  │ Detector    │ │ Display     │ │                                     │
│   │  │ Path (50k)  │ │ Path (12k)  │ │                                     │
│   │  └──────┬──────┘ └──────┬──────┘ │                                     │
│   └─────────┼───────────────┼────────┘                                     │
│             │               │                                               │
│             ▼               ▼                                               │
│   ┌─────────────────────────────────┐                                      │
│   │        signal_relay             │                                      │
│   │   Port 4410      Port 4411      │                                      │
│   └─────────┬───────────────┬───────┘                                      │
│             │               │                                               │
│             ▼               ▼                                               │
│   ┌──────────────┐ ┌──────────────┐                                        │
│   │  Detector    │ │  Waterfall   │                                        │
│   │  Clients     │ │  Displays    │                                        │
│   └──────────────┘ └──────────────┘                                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Tools

### sdr_server

TCP control server for SDRplay RSP2 Pro hardware.

**Ports:**
- 4535: Control commands (text protocol)
- 4536: I/Q streaming (binary protocol)

**Usage:**
```bash
sdr_server.exe [-p control_port] [-i iq_port] [-T telemetry_addr]
```

**Protocol:**
- Control: Text commands (SET_FREQ, GET_GAIN, START, STOP, etc.)
- I/Q: Binary frames with PHXI/IQDQ headers

---

### signal_splitter

Splits high-rate I/Q stream into multiple lower-rate streams for different consumers.

**Input:** 2 MHz I/Q from sdr_server:4536

**Output:**
- Port 4410: 50 kHz detector stream (float32 I/Q)
- Port 4411: 12 kHz display stream (float32 I/Q)

**Usage:**
```bash
signal_splitter.exe [--sdr host:port] [--relay host]
```

**Processing:**
- 5 kHz lowpass filter on both paths
- 40:1 decimation for detector path
- 166:1 decimation for display path

---

### signal_relay

Multi-client broadcast relay server. Accepts one producer connection and broadcasts to multiple consumers.

**Target Platform:** Linux (DigitalOcean droplet)

**Ports:**
- 4410: Detector stream relay
- 4411: Display stream relay

**Features:**
- Ring buffer per client (30 seconds)
- Automatic slow client disconnection
- Producer reconnection handling
- Stream header forwarding to new clients

**Usage:**
```bash
./signal_relay [-d detector_port] [-p display_port]
```

---

### wormhole

MIL-STD-188-110A constellation display for PSK signal visualization.

**Parameters:**
- Sample Rate: 48 kHz
- Symbol Rate: 2400 baud
- Center Frequency: 1800 Hz

**Usage:**
```bash
wormhole.exe [pcm_directory]
```

---

### test_iq_client.py

Python test client for I/Q streaming protocol verification.

**Usage:**
```bash
python test_iq_client.py [--host localhost] [--port 4536]
```

---

## Protocols

### Control Protocol (Port 4535)

Text-based, one command per line. See [phoenix-sdr-core TCP_PROTOCOL.md](https://github.com/Alex-Pennington/phoenix-sdr-core/blob/main/docs/TCP_PROTOCOL.md).

### I/Q Streaming Protocol (Port 4536)

Binary protocol with frame headers:

**Stream Header (PHXI):**
```c
struct {
    uint32_t magic;           // 0x50485849 "PHXI"
    uint32_t version;         // Protocol version
    uint32_t sample_rate;     // Hz
    uint32_t sample_format;   // 1=S16, 2=F32, 3=U8
    uint32_t center_freq_lo;  // Frequency low 32 bits
    uint32_t center_freq_hi;  // Frequency high 32 bits
    uint32_t gain_reduction;  // IF gain dB
    uint32_t lna_state;       // LNA state 0-8
};
```

**Data Frame (IQDQ):**
```c
struct {
    uint32_t magic;           // 0x49514451 "IQDQ"
    uint32_t sequence;        // Frame counter
    uint32_t num_samples;     // I/Q pairs in frame
    uint32_t flags;           // Status flags
    // Followed by num_samples * 2 * sizeof(format) bytes
};
```

### Relay Protocol (Ports 4410/4411)

Float32 I/Q streaming:

**Stream Header (FT32):**
```c
struct {
    uint32_t magic;           // 0x46543332 "FT32"
    uint32_t sample_rate;     // 50000 or 12000
    uint32_t reserved[2];
};
```

**Data Frame (DATA):**
```c
struct {
    uint32_t magic;           // 0x44415441 "DATA"
    uint32_t sequence;        // Frame counter
    uint32_t num_samples;     // I/Q pairs
    uint32_t flags;
    // Followed by num_samples * 2 * sizeof(float) bytes
};
```

---

## Building

### Windows (sdr_server, signal_splitter, wormhole)

```powershell
gcc -O2 -I include src/sdr_server.c -lws2_32 -o sdr_server.exe
gcc -O2 -I include src/signal_splitter.c src/waterfall_dsp.c -lws2_32 -o signal_splitter.exe
gcc -O2 -I include src/wormhole.c -lSDL2 -o wormhole.exe
```

### Linux (signal_relay)

```bash
gcc -O2 -I include src/signal_relay.c -lpthread -o signal_relay
```

---

## Related Repositories

| Repository | Description |
|------------|-------------|
| [mars-suite](https://github.com/Alex-Pennington/mars-suite) | Phoenix Nest MARS Suite index |
| [phoenix-sdr-core](https://github.com/Alex-Pennington/phoenix-sdr-core) | SDR hardware interface |
| [phoenix-waterfall](https://github.com/Alex-Pennington/phoenix-waterfall) | Waterfall display (client) |
| [phoenix-wwv](https://github.com/Alex-Pennington/phoenix-wwv) | WWV detection library |

---

## License

AGPL-3.0 — See [LICENSE](LICENSE)

---

*Phoenix Nest MARS Communications Suite*  
*KY4OLB*
