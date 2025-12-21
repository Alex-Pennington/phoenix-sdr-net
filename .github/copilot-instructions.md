# Phoenix SDR Net - Copilot Instructions

## P0 - CRITICAL RULES

1. **NO unauthorized additions.** Do not add features, flags, modes, or files without explicit instruction. ASK FIRST.
2. **Minimal scope.** Fix only what's requested. One change at a time.
3. **Suggest before acting.** Explain your plan, wait for confirmation.
4. **Verify before modifying.** Run `git status`, read files before editing.

---

## Architecture Overview

**Phoenix SDR Net** provides network streaming and relay tools for distributed SDR processing.

### Network Topology
```
┌──────────────┐
│  SDRplay     │
│  RSP2 Pro    │
└──────┬───────┘
       │ USB
       ▼
┌──────────────┐     Control: 4535     ┌──────────────┐
│  sdr_server  │◄──────────────────────│   Clients    │
│              │     I/Q: 4536         │  (control)   │
└──────┬───────┘─────────────────────►└──────────────┘
       │ 2 MHz I/Q
       ▼
┌──────────────────────────────────┐
│         signal_splitter          │
│  Detector (50k)    Display (12k) │
└─────────┬───────────────┬────────┘
          │               │
          ▼               ▼
┌─────────────────────────────────┐
│        signal_relay             │
│   Port 4410      Port 4411      │
└─────────┬───────────────┬───────┘
          │               │
          ▼               ▼
    Detector Clients   Display Clients
```

### Key Files
| File | Purpose |
|------|---------|
| `sdr_server.c` | SDRplay TCP control + I/Q streaming |
| `signal_splitter.c` | Split 2 MHz → 50k detector + 12k display |
| `signal_relay.c` | Multi-client broadcast relay |
| `wormhole.c` | MIL-STD-188-110A constellation display |

---

## Build

### Windows
```powershell
gcc -O2 -I include src/sdr_server.c -lws2_32 -o sdr_server.exe
gcc -O2 -I include src/signal_splitter.c src/waterfall_dsp.c -lws2_32 -o signal_splitter.exe
```

### Linux (signal_relay)
```bash
gcc -O2 -I include src/signal_relay.c -lpthread -o signal_relay
```

---

## Protocol: I/Q Streaming (PHXI/IQDQ)

Binary protocol for high-rate I/Q transfer.

### Stream Header (PHXI) - 32 bytes
```c
struct {
    uint32_t magic;           // 0x50485849 "PHXI"
    uint32_t version;         // Protocol version (1)
    uint32_t sample_rate;     // Hz
    uint32_t sample_format;   // 1=S16, 2=F32, 3=U8
    uint32_t center_freq_lo;  // Low 32 bits
    uint32_t center_freq_hi;  // High 32 bits
    uint32_t gain_reduction;  // dB
    uint32_t lna_state;       // 0-8
};
```

### Data Frame (IQDQ) - 16 byte header + data
```c
struct {
    uint32_t magic;           // 0x49514451 "IQDQ"
    uint32_t sequence;        // Frame counter
    uint32_t num_samples;     // I/Q pairs
    uint32_t flags;           // Bit 0: overload
    // Followed by: num_samples * 2 * format_size bytes
};
```

### Metadata Update (META) - 32 bytes
```c
struct {
    uint32_t magic;           // 0x4D455441 "META"
    uint32_t sample_rate;
    uint32_t sample_format;
    uint32_t center_freq_lo;
    uint32_t center_freq_hi;
    uint32_t gain_reduction;
    uint32_t lna_state;
    uint32_t reserved;
};
```

---

## Protocol: Float32 Relay (FT32/DATA)

Simplified protocol for decimated float32 streams.

### Stream Header (FT32) - 16 bytes
```c
struct {
    uint32_t magic;           // 0x46543332 "FT32"
    uint32_t sample_rate;     // 50000 or 12000
    uint32_t reserved[2];
};
```

### Data Frame (DATA) - 16 byte header + data
```c
struct {
    uint32_t magic;           // 0x44415441 "DATA"
    uint32_t sequence;
    uint32_t num_samples;
    uint32_t flags;
    // Followed by: num_samples * 2 * sizeof(float) bytes
};
```

---

## Port Assignments

| Port | Service | Protocol |
|------|---------|----------|
| 4535 | SDR control | Text commands |
| 4536 | SDR I/Q stream | PHXI/IQDQ binary |
| 4410 | Detector relay | FT32/DATA binary |
| 4411 | Display relay | FT32/DATA binary |

---

## Connection Handling

### Reconnection Pattern
```c
while (running) {
    if (socket == INVALID) {
        socket = try_connect(host, port);
        if (socket == INVALID) {
            sleep(5);  // Retry in 5 seconds
            continue;
        }
        // Read header, validate magic
    }
    
    // Process frames...
    
    if (error) {
        close(socket);
        socket = INVALID;
    }
}
```

### Client Ring Buffer (signal_relay)
- 30 second buffer per client
- Drop slow clients that can't keep up
- Send header to new clients on connect

---

## Platform Differences

| Feature | Windows | Linux |
|---------|---------|-------|
| Socket type | `SOCKET` | `int` |
| Invalid socket | `INVALID_SOCKET` | `-1` |
| Close | `closesocket()` | `close()` |
| Error code | `WSAGetLastError()` | `errno` |
| Init | `WSAStartup()` | none |

Use the abstraction macros in each file:
```c
#ifdef _WIN32
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define socket_close closesocket
#else
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define socket_close close
#endif
```

---

## Dependencies

| Library | Purpose |
|---------|---------|
| Winsock2 | Windows networking |
| POSIX sockets | Linux networking |
| waterfall_dsp | DSP for signal_splitter |
