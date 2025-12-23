# phoenix-sdr-net

**Version:** v0.1.0  
**Part of:** Phoenix Nest MARS Communications Suite  
**Developer:** Alex Pennington (KY4OLB)

---

## Overview

Network streaming and telemetry tools for the Phoenix Nest SDR system. Provides TCP-based I/Q streaming from SDR hardware and UDP telemetry logging for distributed processing.

---

## Components

| Tool | Description |
|------|-------------|
| `sdr_server` | TCP control + I/Q streaming with Phoenix Nest discovery |
| `telem_logger` | UDP telemetry listener and CSV logger with system tray support |
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
│   │              │     I/Q: 4536            │ (TCP/UDP)    │               │
│   │              │─────────────────────────►└──────────────┘               │
│   └──────┬───────┘     2 MHz I/Q                                            │
│          │                                                                  │
│          │  UDP Telemetry (optional)                                        │
│          │  Port 3005                                                       │
│          ▼                                                                  │
│   ┌──────────────┐                                                          │
│   │ telem_logger │                                                          │
│   │              │                                                          │
│   │ CSV Files:   │                                                          │
│   │ • CHAN       │                                                          │
│   │ • TICK       │                                                          │
│   │ • MARK       │                                                          │
│   │ • SYNC       │                                                          │
│   └──────────────┘                                                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Tools

### sdr_server

TCP control server for SDRplay RSP2 Pro hardware with Phoenix Nest service discovery.

**Ports:**
- 4535: Control commands (text protocol)
- 4536: I/Q streaming (binary protocol)
- 5400: UDP service discovery broadcasts

**Discovery:**
- Automatically announces service on LAN
- Default node ID: `SDR-SERVER-1`
- Broadcasts every 30-60 seconds
- Can be disabled with `--no-discovery`

**Usage:**
```bash
sdr_server.exe [-p control_port] [-i iq_port] [--node-id ID] [--no-discovery]
```

**Protocol:**
- Control: Text commands (SET_FREQ, GET_GAIN, START, STOP, etc.)
- I/Q: Binary frames with PHXI/IQDQ headers

---

### telem_logger

UDP telemetry listener and CSV logger. Captures telemetry broadcasts from processing tools and organizes them into channel-specific CSV files.

**Port:** 3005 (UDP, configurable)

**Features:**
- System tray operation (Windows)
- Per-channel CSV files with timestamps
- Pause/Resume logging
- Channel filtering
- Verbose console output mode

**Usage:**
```bash
telem_logger.exe [-p 3005] [-o logs/] [-c CHAN,TICK,MARK] [-v] [--no-tray]
```

**Output Files:**
```
telem_CHAN_YYYYMMDD_HHMMSS.csv  # Channel quality metrics
telem_TICK_YYYYMMDD_HHMMSS.csv  # Tick pulse detections
telem_MARK_YYYYMMDD_HHMMSS.csv  # Minute marker events
telem_SYNC_YYYYMMDD_HHMMSS.csv  # Sync state transitions
```

**System Tray:**
- Tooltip shows message count and active channels
- Right-click menu for pause/resume, open logs folder, exit
- Graceful shutdown with file footers

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

### Windows (CMake + MinGW)

```powershell
# Configure
cmake --preset msys2-ucrt64

# Build
cmake --build --preset msys2-ucrt64

# Deploy release
.\deploy-release.ps1 -IncrementPatch -Deploy
```

**Output:**
- `build/msys2-ucrt64/sdr_server.exe`
- `build/msys2-ucrt64/signal_splitter.exe`
- `build/msys2-ucrt64/wormhole.exe`

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
| [phoenix-discovery](https://github.com/Alex-Pennington/phoenix-discovery) | Service discovery (UDP broadcast) |
| [phoenix-waterfall](https://github.com/Alex-Pennington/phoenix-waterfall) | Waterfall display (client) |
| [phoenix-wwv](https://github.com/Alex-Pennington/phoenix-wwv) | WWV detection library |

---

## License

AGPL-3.0 — See [LICENSE](LICENSE)

---

*Phoenix Nest MARS Communications Suite*  
*KY4OLB*
