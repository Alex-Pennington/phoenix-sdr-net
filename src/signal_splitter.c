/**
 * @file signal_splitter.c
 * @brief Split 2 MHz I/Q from SDR server into detector (50kHz) and display (12kHz) paths
 *
 * Architecture:
 *   TCP Client → sdr_server:4536 (receives 2 MHz I/Q)
 *   ↓
 *   Signal Divergence (exact copy of waterfall.c divergence)
 *   ├─ Detector Path: 5kHz lowpass → decimate 40:1 → 50kHz
 *   └─ Display Path:  5kHz lowpass → decimate 166:1 → 12kHz
 *   ↓
 *   TCP Client → relay_server:4410 (detector stream, float32 I/Q)
 *   TCP Client → relay_server:4411 (display stream, float32 I/Q)
 *
 * Connection Tolerance:
 *   - sdr_server disconnect: stop processing, retry every 5 sec
 *   - relay disconnect: buffer to ring (30 sec), retry every 5 sec
 *   - Graceful shutdown on SIGINT/SIGTERM
 *   - Status reporting every 5 seconds
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>

#include "waterfall_dsp.h"
#include "version.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define socket_close closesocket
#define socket_errno WSAGetLastError()
#define EWOULDBLOCK_VAL WSAEWOULDBLOCK
#define EINPROGRESS_VAL WSAEINPROGRESS
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define socket_close close
#define socket_errno errno
#define EWOULDBLOCK_VAL EWOULDBLOCK
#define EINPROGRESS_VAL EINPROGRESS
#define Sleep(ms) usleep((ms) * 1000)
#endif

/*============================================================================
 * Protocol Definitions
 *============================================================================*/

/* SDR Server Protocol (MAGIC_IQDQ from sdr_server.c) */
#define MAGIC_PHXI  0x50485849  /* "PHXI" - Stream header */
#define MAGIC_IQDQ  0x49514451  /* "IQDQ" - Data frame */
#define MAGIC_META  0x4D455441  /* "META" - Metadata */

typedef struct {
    uint32_t magic;           /* 0x50485849 = "PHXI" */
    uint32_t version;         /* Protocol version (1) */
    uint32_t sample_rate;     /* Hz (e.g., 2000000) */
    uint32_t sample_format;   /* 1=S16, 2=F32, 3=U8 */
    uint32_t center_freq_lo;  /* Low 32 bits of freq */
    uint32_t center_freq_hi;  /* High 32 bits of freq */
    uint32_t gain_reduction;  /* dB */
    uint32_t lna_state;       /* 0-8 */
} iq_stream_header_t;

typedef struct {
    uint32_t magic;           /* 0x49514451 = "IQDQ" */
    uint32_t sequence;        /* Frame counter */
    uint32_t num_samples;     /* I/Q pairs in frame */
    uint32_t flags;           /* Status flags */
} iq_data_frame_t;

/* Relay Server Protocol (float32 streams) */
#define MAGIC_FT32  0x46543332  /* "FT32" - Float32 stream header */
#define MAGIC_DATA  0x44415441  /* "DATA" - Float32 data frame */

typedef struct {
    uint32_t magic;           /* 0x46543332 = "FT32" */
    uint32_t sample_rate;     /* Hz (50000 or 12000) */
    uint32_t reserved1;
    uint32_t reserved2;
} relay_stream_header_t;

typedef struct {
    uint32_t magic;           /* 0x44415441 = "DATA" */
    uint32_t sequence;        /* Frame counter */
    uint32_t num_samples;     /* I/Q pairs in frame */
    uint32_t reserved;
} relay_data_frame_t;

/*============================================================================
 * Configuration
 *============================================================================*/

#define SDR_SAMPLE_RATE         2000000     /* 2 MHz from SDR */
#define DETECTOR_SAMPLE_RATE    50000       /* 50 kHz detector path */
#define DISPLAY_SAMPLE_RATE     12000       /* 12 kHz display path */
#define DETECTOR_DECIMATION     40          /* 2MHz / 50kHz */
#define DISPLAY_DECIMATION      166         /* 2MHz / 12kHz (approx) */
#define FILTER_CUTOFF           5000.0f     /* 5 kHz lowpass */

#define RELAY_FRAME_SIZE        2048        /* Samples per relay frame */
#define DETECTOR_BUFFER_SIZE    (50000 * 30)  /* 30 sec @ 50kHz = 1.5M samples */
#define DISPLAY_BUFFER_SIZE     (12000 * 30)  /* 30 sec @ 12kHz = 360k samples */

#define RECONNECT_DELAY_MS      5000        /* 5 seconds between reconnect attempts */
#define STATUS_INTERVAL_MS      5000        /* 5 seconds between status reports */

#define DEFAULT_SDR_HOST        "localhost"
#define DEFAULT_SDR_PORT        4536
#define DEFAULT_SDR_CTRL_PORT   4535
#define DEFAULT_RELAY_PORT_DET  4410
#define DEFAULT_RELAY_PORT_DISP 4411
#define DEFAULT_RELAY_CTRL_PORT 4409

/*============================================================================
 * Ring Buffer
 *============================================================================*/

typedef struct {
    float *data;        /* I/Q pairs (interleaved) */
    size_t capacity;    /* Total I/Q pairs */
    size_t write_idx;
    size_t read_idx;
    size_t count;       /* Number of I/Q pairs available */
    uint64_t overflows; /* Overflow counter */
} ring_buffer_t;

static ring_buffer_t* ring_buffer_create(size_t capacity) {
    ring_buffer_t *rb = (ring_buffer_t*)malloc(sizeof(ring_buffer_t));
    if (!rb) return NULL;

    rb->data = (float*)malloc(capacity * 2 * sizeof(float));  /* I and Q */
    if (!rb->data) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->write_idx = 0;
    rb->read_idx = 0;
    rb->count = 0;
    rb->overflows = 0;
    return rb;
}

static void ring_buffer_destroy(ring_buffer_t *rb) {
    if (rb) {
        free(rb->data);
        free(rb);
    }
}

static bool ring_buffer_write(ring_buffer_t *rb, float i, float q) {
    if (rb->count >= rb->capacity) {
        /* Overflow - discard oldest */
        rb->read_idx = (rb->read_idx + 1) % rb->capacity;
        rb->overflows++;
    } else {
        rb->count++;
    }

    rb->data[rb->write_idx * 2] = i;
    rb->data[rb->write_idx * 2 + 1] = q;
    rb->write_idx = (rb->write_idx + 1) % rb->capacity;
    return true;
}

static bool ring_buffer_read(ring_buffer_t *rb, float *i, float *q) {
    if (rb->count == 0) return false;

    *i = rb->data[rb->read_idx * 2];
    *q = rb->data[rb->read_idx * 2 + 1];
    rb->read_idx = (rb->read_idx + 1) % rb->capacity;
    rb->count--;
    return true;
}

static size_t ring_buffer_available(ring_buffer_t *rb) {
    return rb->count;
}

/*============================================================================
 * Global State
 *============================================================================*/

static volatile bool g_running = true;
static volatile bool g_shutdown_requested = false;

/* SDR connection */
static socket_t g_sdr_socket = SOCKET_INVALID;
static char g_sdr_host[256] = DEFAULT_SDR_HOST;
static int g_sdr_port = DEFAULT_SDR_PORT;
static bool g_sdr_connected = false;
static uint32_t g_sdr_sample_rate = SDR_SAMPLE_RATE;

/* Relay connections */
static socket_t g_relay_det_socket = SOCKET_INVALID;
static socket_t g_relay_disp_socket = SOCKET_INVALID;
static char g_relay_host[256] = "";
static int g_relay_det_port = DEFAULT_RELAY_PORT_DET;
static int g_relay_disp_port = DEFAULT_RELAY_PORT_DISP;
static bool g_relay_det_connected = false;
static bool g_relay_disp_connected = false;
static uint32_t g_relay_det_sequence = 0;
static uint32_t g_relay_disp_sequence = 0;

/* Control connections (text protocol passthrough) */
static socket_t g_sdr_ctrl_socket = SOCKET_INVALID;
static socket_t g_relay_ctrl_socket = SOCKET_INVALID;
static int g_sdr_ctrl_port = DEFAULT_SDR_CTRL_PORT;
static int g_relay_ctrl_port = DEFAULT_RELAY_CTRL_PORT;
static bool g_sdr_ctrl_connected = false;
static bool g_relay_ctrl_connected = false;

/* DSP State - exact copy of waterfall.c */
static wf_lowpass_t g_detector_lowpass_i;
static wf_lowpass_t g_detector_lowpass_q;
static wf_lowpass_t g_display_lowpass_i;
static wf_lowpass_t g_display_lowpass_q;
static int g_detector_decim_counter = 0;
static int g_display_decim_counter = 0;

/* Ring buffers for relay disconnect tolerance */
static ring_buffer_t *g_detector_ring = NULL;
static ring_buffer_t *g_display_ring = NULL;

/* Relay output buffers */
static float g_detector_frame[RELAY_FRAME_SIZE * 2];  /* I/Q pairs */
static float g_display_frame[RELAY_FRAME_SIZE * 2];
static int g_detector_frame_idx = 0;
static int g_display_frame_idx = 0;

/* Statistics */
static uint64_t g_samples_received = 0;
static uint64_t g_detector_samples_sent = 0;
static uint64_t g_display_samples_sent = 0;
static uint64_t g_detector_samples_dropped = 0;
static uint64_t g_display_samples_dropped = 0;
static time_t g_last_status_time = 0;

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n[SHUTDOWN] Received signal, shutting down gracefully...\n");
    g_shutdown_requested = true;
    g_running = false;
}

/*============================================================================
 * TCP Helpers
 *============================================================================*/

static bool tcp_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

static void tcp_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static socket_t tcp_connect(const char *host, int port) {
    struct addrinfo hints, *result, *rp;
    char port_str[16];
    socket_t sock = SOCKET_INVALID;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return SOCKET_INVALID;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == SOCKET_INVALID) continue;

        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            break;  /* Success */
        }

        socket_close(sock);
        sock = SOCKET_INVALID;
    }

    freeaddrinfo(result);
    return sock;
}

static bool tcp_recv_exact(socket_t sock, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = recv(sock, (char*)buf + total, (int)(len - total), 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

static bool tcp_send_exact(socket_t sock, const void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = send(sock, (const char*)buf + total, (int)(len - total), 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

/*============================================================================
 * SDR Connection
 *============================================================================*/

static bool connect_to_sdr(void) {
    if (g_sdr_connected) return true;

    fprintf(stderr, "[SDR] Connecting to %s:%d...\n", g_sdr_host, g_sdr_port);

    g_sdr_socket = tcp_connect(g_sdr_host, g_sdr_port);
    if (g_sdr_socket == SOCKET_INVALID) {
        fprintf(stderr, "[SDR] Connection failed\n");
        return false;
    }

    /* Receive stream header */
    iq_stream_header_t header;
    if (!tcp_recv_exact(g_sdr_socket, &header, sizeof(header))) {
        fprintf(stderr, "[SDR] Failed to receive header\n");
        socket_close(g_sdr_socket);
        g_sdr_socket = SOCKET_INVALID;
        return false;
    }

    if (header.magic != MAGIC_PHXI) {
        fprintf(stderr, "[SDR] Invalid header magic: 0x%08X\n", header.magic);
        socket_close(g_sdr_socket);
        g_sdr_socket = SOCKET_INVALID;
        return false;
    }

    g_sdr_sample_rate = header.sample_rate;

    fprintf(stderr, "[SDR] Connected: %u Hz, format=%u, freq=%llu Hz\n",
            header.sample_rate, header.sample_format,
            ((uint64_t)header.center_freq_hi << 32) | header.center_freq_lo);

    g_sdr_connected = true;
    return true;
}

static void disconnect_from_sdr(void) {
    if (g_sdr_socket != SOCKET_INVALID) {
        socket_close(g_sdr_socket);
        g_sdr_socket = SOCKET_INVALID;
    }
    g_sdr_connected = false;
}

/*============================================================================
 * Relay Connection
 *============================================================================*/

static bool connect_to_relay_detector(void) {
    if (g_relay_det_connected) return true;

    fprintf(stderr, "[RELAY-DET] Connecting to %s:%d...\n", g_relay_host, g_relay_det_port);

    g_relay_det_socket = tcp_connect(g_relay_host, g_relay_det_port);
    if (g_relay_det_socket == SOCKET_INVALID) {
        fprintf(stderr, "[RELAY-DET] Connection failed\n");
        return false;
    }

    /* Send stream header */
    relay_stream_header_t header = {
        .magic = MAGIC_FT32,
        .sample_rate = DETECTOR_SAMPLE_RATE,
        .reserved1 = 0,
        .reserved2 = 0
    };

    if (!tcp_send_exact(g_relay_det_socket, &header, sizeof(header))) {
        fprintf(stderr, "[RELAY-DET] Failed to send header\n");
        socket_close(g_relay_det_socket);
        g_relay_det_socket = SOCKET_INVALID;
        return false;
    }

    fprintf(stderr, "[RELAY-DET] Connected: %u Hz float32 I/Q\n", DETECTOR_SAMPLE_RATE);
    g_relay_det_connected = true;
    g_relay_det_sequence = 0;
    return true;
}

static bool connect_to_relay_display(void) {
    if (g_relay_disp_connected) return true;

    fprintf(stderr, "[RELAY-DISP] Connecting to %s:%d...\n", g_relay_host, g_relay_disp_port);

    g_relay_disp_socket = tcp_connect(g_relay_host, g_relay_disp_port);
    if (g_relay_disp_socket == SOCKET_INVALID) {
        fprintf(stderr, "[RELAY-DISP] Connection failed\n");
        return false;
    }

    /* Send stream header */
    relay_stream_header_t header = {
        .magic = MAGIC_FT32,
        .sample_rate = DISPLAY_SAMPLE_RATE,
        .reserved1 = 0,
        .reserved2 = 0
    };

    if (!tcp_send_exact(g_relay_disp_socket, &header, sizeof(header))) {
        fprintf(stderr, "[RELAY-DISP] Failed to send header\n");
        socket_close(g_relay_disp_socket);
        g_relay_disp_socket = SOCKET_INVALID;
        return false;
    }

    fprintf(stderr, "[RELAY-DISP] Connected: %u Hz float32 I/Q\n", DISPLAY_SAMPLE_RATE);
    g_relay_disp_connected = true;
    g_relay_disp_sequence = 0;
    return true;
}

static void disconnect_from_relay(void) {
    if (g_relay_det_socket != SOCKET_INVALID) {
        socket_close(g_relay_det_socket);
        g_relay_det_socket = SOCKET_INVALID;
    }
    g_relay_det_connected = false;

    if (g_relay_disp_socket != SOCKET_INVALID) {
        socket_close(g_relay_disp_socket);
        g_relay_disp_socket = SOCKET_INVALID;
    }
    g_relay_disp_connected = false;
}

/*============================================================================
 * Control Path Connections (Text Protocol Passthrough)
 *============================================================================*/

static bool connect_to_sdr_control(void) {
    if (g_sdr_ctrl_connected) return true;

    fprintf(stderr, "[SDR-CTRL] Connecting to %s:%d...\n", g_sdr_host, g_sdr_ctrl_port);

    g_sdr_ctrl_socket = tcp_connect(g_sdr_host, g_sdr_ctrl_port);
    if (g_sdr_ctrl_socket == SOCKET_INVALID) {
        fprintf(stderr, "[SDR-CTRL] Connection failed\n");
        return false;
    }

    fprintf(stderr, "[SDR-CTRL] Connected (text protocol)\n");
    g_sdr_ctrl_connected = true;
    return true;
}

static bool connect_to_relay_control(void) {
    if (g_relay_ctrl_connected) return true;

    fprintf(stderr, "[RELAY-CTRL] Connecting to %s:%d...\n", g_relay_host, g_relay_ctrl_port);

    g_relay_ctrl_socket = tcp_connect(g_relay_host, g_relay_ctrl_port);
    if (g_relay_ctrl_socket == SOCKET_INVALID) {
        fprintf(stderr, "[RELAY-CTRL] Connection failed\n");
        return false;
    }

    fprintf(stderr, "[RELAY-CTRL] Connected (text protocol passthrough)\n");
    g_relay_ctrl_connected = true;
    return true;
}

static void disconnect_from_control(void) {
    if (g_sdr_ctrl_socket != SOCKET_INVALID) {
        socket_close(g_sdr_ctrl_socket);
        g_sdr_ctrl_socket = SOCKET_INVALID;
    }
    g_sdr_ctrl_connected = false;

    if (g_relay_ctrl_socket != SOCKET_INVALID) {
        socket_close(g_relay_ctrl_socket);
        g_relay_ctrl_socket = SOCKET_INVALID;
    }
    g_relay_ctrl_connected = false;
}

static void forward_control_data(socket_t from_sock, socket_t to_sock, const char *label) {
    char buffer[4096];
    ssize_t received = recv(from_sock, buffer, sizeof(buffer) - 1, 0);

    if (received <= 0) {
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  /* No data available */
        }
        fprintf(stderr, "[CTRL-%s] Connection lost\n", label);
        disconnect_from_control();
        return;
    }

    buffer[received] = '\0';

    /* Forward to destination */
    if (send(to_sock, buffer, received, 0) < 0) {
        fprintf(stderr, "[CTRL-%s] Forward failed\n", label);
        disconnect_from_control();
    }
}

/*============================================================================
 * Relay Frame Transmission
 *============================================================================*/

static bool send_detector_frame(void) {
    if (!g_relay_det_connected || g_detector_frame_idx == 0) return true;

    relay_data_frame_t frame_hdr = {
        .magic = MAGIC_DATA,
        .sequence = g_relay_det_sequence++,
        .num_samples = (uint32_t)g_detector_frame_idx,
        .reserved = 0
    };

    /* Send frame header */
    if (!tcp_send_exact(g_relay_det_socket, &frame_hdr, sizeof(frame_hdr))) {
        fprintf(stderr, "[RELAY-DET] Send failed, disconnecting\n");
        socket_close(g_relay_det_socket);
        g_relay_det_socket = SOCKET_INVALID;
        g_relay_det_connected = false;
        return false;
    }

    /* Send float32 I/Q data */
    size_t data_len = g_detector_frame_idx * 2 * sizeof(float);
    if (!tcp_send_exact(g_relay_det_socket, g_detector_frame, data_len)) {
        fprintf(stderr, "[RELAY-DET] Send failed, disconnecting\n");
        socket_close(g_relay_det_socket);
        g_relay_det_socket = SOCKET_INVALID;
        g_relay_det_connected = false;
        return false;
    }

    g_detector_samples_sent += g_detector_frame_idx;
    g_detector_frame_idx = 0;
    return true;
}

static bool send_display_frame(void) {
    if (!g_relay_disp_connected || g_display_frame_idx == 0) return true;

    relay_data_frame_t frame_hdr = {
        .magic = MAGIC_DATA,
        .sequence = g_relay_disp_sequence++,
        .num_samples = (uint32_t)g_display_frame_idx,
        .reserved = 0
    };

    /* Send frame header */
    if (!tcp_send_exact(g_relay_disp_socket, &frame_hdr, sizeof(frame_hdr))) {
        fprintf(stderr, "[RELAY-DISP] Send failed, disconnecting\n");
        socket_close(g_relay_disp_socket);
        g_relay_disp_socket = SOCKET_INVALID;
        g_relay_disp_connected = false;
        return false;
    }

    /* Send float32 I/Q data */
    size_t data_len = g_display_frame_idx * 2 * sizeof(float);
    if (!tcp_send_exact(g_relay_disp_socket, g_display_frame, data_len)) {
        fprintf(stderr, "[RELAY-DISP] Send failed, disconnecting\n");
        socket_close(g_relay_disp_socket);
        g_relay_disp_socket = SOCKET_INVALID;
        g_relay_disp_connected = false;
        return false;
    }

    g_display_samples_sent += g_display_frame_idx;
    g_display_frame_idx = 0;
    return true;
}

/*============================================================================
 * Relay Output (with ring buffer fallback)
 *============================================================================*/

static void output_detector_sample(float i, float q) {
    if (g_relay_det_connected) {
        /* Try to send from ring buffer first (if any buffered) */
        while (ring_buffer_available(g_detector_ring) > 0) {
            float buf_i, buf_q;
            if (!ring_buffer_read(g_detector_ring, &buf_i, &buf_q)) break;

            g_detector_frame[g_detector_frame_idx * 2] = buf_i;
            g_detector_frame[g_detector_frame_idx * 2 + 1] = buf_q;
            g_detector_frame_idx++;

            if (g_detector_frame_idx >= RELAY_FRAME_SIZE) {
                if (!send_detector_frame()) {
                    /* Connection lost - buffer current sample */
                    ring_buffer_write(g_detector_ring, i, q);
                    return;
                }
            }
        }

        /* Add current sample to frame */
        g_detector_frame[g_detector_frame_idx * 2] = i;
        g_detector_frame[g_detector_frame_idx * 2 + 1] = q;
        g_detector_frame_idx++;

        if (g_detector_frame_idx >= RELAY_FRAME_SIZE) {
            send_detector_frame();
        }
    } else {
        /* Not connected - buffer to ring */
        if (!ring_buffer_write(g_detector_ring, i, q)) {
            g_detector_samples_dropped++;
        }
    }
}

static void output_display_sample(float i, float q) {
    if (g_relay_disp_connected) {
        /* Try to send from ring buffer first */
        while (ring_buffer_available(g_display_ring) > 0) {
            float buf_i, buf_q;
            if (!ring_buffer_read(g_display_ring, &buf_i, &buf_q)) break;

            g_display_frame[g_display_frame_idx * 2] = buf_i;
            g_display_frame[g_display_frame_idx * 2 + 1] = buf_q;
            g_display_frame_idx++;

            if (g_display_frame_idx >= RELAY_FRAME_SIZE) {
                if (!send_display_frame()) {
                    /* Connection lost - buffer current sample */
                    ring_buffer_write(g_display_ring, i, q);
                    return;
                }
            }
        }

        /* Add current sample to frame */
        g_display_frame[g_display_frame_idx * 2] = i;
        g_display_frame[g_display_frame_idx * 2 + 1] = q;
        g_display_frame_idx++;

        if (g_display_frame_idx >= RELAY_FRAME_SIZE) {
            send_display_frame();
        }
    } else {
        /* Not connected - buffer to ring */
        if (!ring_buffer_write(g_display_ring, i, q)) {
            g_display_samples_dropped++;
        }
    }
}

/*============================================================================
 * Signal Processing (exact copy from waterfall.c:2151-2238)
 *============================================================================*/

static void process_iq_samples(const int16_t *samples, uint32_t num_samples) {
    for (uint32_t s = 0; s < num_samples; s++) {
        /* Normalize to [-1, 1] (exact copy of waterfall.c) */
        float i_raw = (float)samples[s * 2] / 32768.0f;
        float q_raw = (float)samples[s * 2 + 1] / 32768.0f;

        g_samples_received++;

        /* ===== DETECTOR PATH (50 kHz) ===== */
        float det_i = wf_lowpass_process(&g_detector_lowpass_i, i_raw);
        float det_q = wf_lowpass_process(&g_detector_lowpass_q, q_raw);

        g_detector_decim_counter++;
        if (g_detector_decim_counter >= DETECTOR_DECIMATION) {
            g_detector_decim_counter = 0;
            output_detector_sample(det_i, det_q);
        }

        /* ===== DISPLAY PATH (12 kHz) ===== */
        float disp_i = wf_lowpass_process(&g_display_lowpass_i, i_raw);
        float disp_q = wf_lowpass_process(&g_display_lowpass_q, q_raw);

        g_display_decim_counter++;
        if (g_display_decim_counter >= DISPLAY_DECIMATION) {
            g_display_decim_counter = 0;
            output_display_sample(disp_i, disp_q);
        }
    }
}

/*============================================================================
 * Status Reporting
 *============================================================================*/

static void print_status(void) {
    time_t now = time(NULL);
    if (now - g_last_status_time < (STATUS_INTERVAL_MS / 1000)) return;
    g_last_status_time = now;

    fprintf(stderr, "\n[STATUS] Connections: SDR=%s DET=%s DISP=%s CTRL=%s/%s\n",
            g_sdr_connected ? "UP" : "DOWN",
            g_relay_det_connected ? "UP" : "DOWN",
            g_relay_disp_connected ? "UP" : "DOWN",
            g_sdr_ctrl_connected ? "SDR" : "---",
            g_relay_ctrl_connected ? "RLY" : "---");

    fprintf(stderr, "[STATUS] Samples: RX=%llu DET_TX=%llu DISP_TX=%llu\n",
            (unsigned long long)g_samples_received,
            (unsigned long long)g_detector_samples_sent,
            (unsigned long long)g_display_samples_sent);

    size_t det_buffered = ring_buffer_available(g_detector_ring);
    size_t disp_buffered = ring_buffer_available(g_display_ring);

    fprintf(stderr, "[STATUS] Buffers: DET=%zu/%zu (%.1f%%) DISP=%zu/%zu (%.1f%%)\n",
            det_buffered, (size_t)DETECTOR_BUFFER_SIZE,
            100.0f * det_buffered / DETECTOR_BUFFER_SIZE,
            disp_buffered, (size_t)DISPLAY_BUFFER_SIZE,
            100.0f * disp_buffered / DISPLAY_BUFFER_SIZE);

    if (g_detector_ring->overflows > 0 || g_display_ring->overflows > 0) {
        fprintf(stderr, "[STATUS] Overflows: DET=%llu DISP=%llu\n",
                (unsigned long long)g_detector_ring->overflows,
                (unsigned long long)g_display_ring->overflows);
    }

    if (g_detector_samples_dropped > 0 || g_display_samples_dropped > 0) {
        fprintf(stderr, "[STATUS] Dropped: DET=%llu DISP=%llu\n",
                (unsigned long long)g_detector_samples_dropped,
                (unsigned long long)g_display_samples_dropped);
    }
}

/*============================================================================
 * Main Loop
 *============================================================================*/

static void run(void) {
    time_t last_reconnect = 0;
    int16_t *sample_buffer = (int16_t*)malloc(8192 * 2 * sizeof(int16_t));
    if (!sample_buffer) {
        fprintf(stderr, "Failed to allocate sample buffer\n");
        return;
    }

    while (g_running) {
        time_t now = time(NULL);

        /* Try to connect/reconnect */
        if (!g_sdr_connected && (now - last_reconnect >= (RECONNECT_DELAY_MS / 1000))) {
            if (connect_to_sdr()) {
                last_reconnect = now;
            } else {
                last_reconnect = now;
                Sleep(1000);
                continue;
            }
        }

        if (!g_relay_det_connected && strlen(g_relay_host) > 0 &&
            (now - last_reconnect >= (RECONNECT_DELAY_MS / 1000))) {
            connect_to_relay_detector();
            last_reconnect = now;
        }

        if (!g_relay_disp_connected && strlen(g_relay_host) > 0 &&
            (now - last_reconnect >= (RECONNECT_DELAY_MS / 1000))) {
            connect_to_relay_display();
            last_reconnect = now;
        }

        /* Connect control paths */
        if (!g_sdr_ctrl_connected && g_sdr_connected &&
            (now - last_reconnect >= (RECONNECT_DELAY_MS / 1000))) {
            connect_to_sdr_control();
            last_reconnect = now;
        }

        if (!g_relay_ctrl_connected && strlen(g_relay_host) > 0 &&
            (now - last_reconnect >= (RECONNECT_DELAY_MS / 1000))) {
            connect_to_relay_control();
            last_reconnect = now;
        }

        /* Forward control data bidirectionally */
        if (g_sdr_ctrl_connected && g_relay_ctrl_connected) {
            /* Relay → SDR */
            forward_control_data(g_relay_ctrl_socket, g_sdr_ctrl_socket, "RELAY->SDR");
            /* SDR → Relay */
            forward_control_data(g_sdr_ctrl_socket, g_relay_ctrl_socket, "SDR->RELAY");
        }

        /* Receive and process data from SDR */
        if (g_sdr_connected) {
            iq_data_frame_t frame;
            if (!tcp_recv_exact(g_sdr_socket, &frame, sizeof(frame))) {
                fprintf(stderr, "[SDR] Connection lost\n");
                disconnect_from_sdr();
                continue;
            }

            if (frame.magic != MAGIC_IQDQ) {
                fprintf(stderr, "[SDR] Invalid frame magic: 0x%08X\n", frame.magic);
                disconnect_from_sdr();
                continue;
            }

            /* Receive sample data */
            size_t sample_bytes = frame.num_samples * 2 * sizeof(int16_t);
            if (!tcp_recv_exact(g_sdr_socket, sample_buffer, sample_bytes)) {
                fprintf(stderr, "[SDR] Failed to receive samples\n");
                disconnect_from_sdr();
                continue;
            }

            /* Process samples */
            process_iq_samples(sample_buffer, frame.num_samples);
        }

        /* Print status */
        print_status();
    }

    free(sample_buffer);

    /* Flush remaining frames on shutdown */
    if (g_shutdown_requested) {
        fprintf(stderr, "\n[SHUTDOWN] Flushing buffers...\n");
        send_detector_frame();
        send_display_frame();
    }
}

/*============================================================================
 * Main
 *============================================================================*/

static void print_usage(const char *prog) {
    printf("Signal Splitter - Split 2 MHz I/Q into detector and display streams\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --sdr-host HOST        SDR server hostname (default: %s)\n", DEFAULT_SDR_HOST);
    printf("  --sdr-port PORT        SDR I/Q port (default: %d)\n", DEFAULT_SDR_PORT);
    printf("  --sdr-ctrl-port PORT   SDR control port (default: %d)\n", DEFAULT_SDR_CTRL_PORT);
    printf("  --relay-host HOST      Relay server hostname (required)\n");
    printf("  --relay-det PORT       Relay detector port (default: %d)\n", DEFAULT_RELAY_PORT_DET);
    printf("  --relay-disp PORT      Relay display port (default: %d)\n", DEFAULT_RELAY_PORT_DISP);
    printf("  --relay-ctrl PORT      Relay control port (default: %d)\n", DEFAULT_RELAY_CTRL_PORT);
    printf("  -h, --help             Show this help\n\n");
    printf("Streams:\n");
    printf("  Input:  SDR server @ HOST:PORT (2 MHz I/Q, int16)\n");
    printf("  Output: Detector @ HOST:%d (50 kHz I/Q, float32)\n", DEFAULT_RELAY_PORT_DET);
    printf("  Output: Display @ HOST:%d (12 kHz I/Q, float32)\n", DEFAULT_RELAY_PORT_DISP);
    printf("  Relay:  Control @ HOST:%d (bidirectional text)\n\n", DEFAULT_RELAY_CTRL_PORT);
    printf("See: docs/SIGNAL_SPLITTER.md\n");
}

int main(int argc, char *argv[]) {
    print_version("Phoenix SDR - Signal Splitter");

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sdr-host") == 0 && i + 1 < argc) {
            strncpy(g_sdr_host, argv[++i], sizeof(g_sdr_host) - 1);
        } else if (strcmp(argv[i], "--sdr-port") == 0 && i + 1 < argc) {
            g_sdr_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--relay-host") == 0 && i + 1 < argc) {
            strncpy(g_relay_host, argv[++i], sizeof(g_relay_host) - 1);
        } else if (strcmp(argv[i], "--relay-det") == 0 && i + 1 < argc) {
            g_relay_det_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--relay-disp") == 0 && i + 1 < argc) {
            g_relay_disp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (strlen(g_relay_host) == 0) {
        fprintf(stderr, "Error: --relay-host is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    fprintf(stderr, "Signal Splitter Configuration:\n");
    fprintf(stderr, "  SDR:     %s:%d\n", g_sdr_host, g_sdr_port);
    fprintf(stderr, "  Relay:   %s:%d (detector), %s:%d (display)\n",
            g_relay_host, g_relay_det_port, g_relay_host, g_relay_disp_port);
    fprintf(stderr, "  Buffers: Detector=%d sec, Display=%d sec\n\n",
            DETECTOR_BUFFER_SIZE / DETECTOR_SAMPLE_RATE,
            DISPLAY_BUFFER_SIZE / DISPLAY_SAMPLE_RATE);

    /* Initialize */
    signal(SIGINT, signal_handler);
#ifdef SIGTERM
    signal(SIGTERM, signal_handler);
#endif

    if (!tcp_init()) {
        fprintf(stderr, "Failed to initialize TCP\n");
        return 1;
    }

    /* Initialize DSP (exact copy of waterfall.c initialization) */
    wf_lowpass_init(&g_detector_lowpass_i, FILTER_CUTOFF, (float)SDR_SAMPLE_RATE);
    wf_lowpass_init(&g_detector_lowpass_q, FILTER_CUTOFF, (float)SDR_SAMPLE_RATE);
    wf_lowpass_init(&g_display_lowpass_i, FILTER_CUTOFF, (float)SDR_SAMPLE_RATE);
    wf_lowpass_init(&g_display_lowpass_q, FILTER_CUTOFF, (float)SDR_SAMPLE_RATE);

    /* Create ring buffers */
    g_detector_ring = ring_buffer_create(DETECTOR_BUFFER_SIZE);
    g_display_ring = ring_buffer_create(DISPLAY_BUFFER_SIZE);
    if (!g_detector_ring || !g_display_ring) {
        fprintf(stderr, "Failed to allocate ring buffers\n");
        return 1;
    }

    fprintf(stderr, "[STARTUP] Ready to process signals\n\n");
    g_last_status_time = time(NULL);

    /* Main loop */
    run();

    /* Cleanup */
    fprintf(stderr, "\n[SHUTDOWN] Closing connections...\n");
    disconnect_from_sdr();
    disconnect_from_relay();
    disconnect_from_control();
    ring_buffer_destroy(g_detector_ring);
    ring_buffer_destroy(g_display_ring);
    tcp_cleanup();

    fprintf(stderr, "[SHUTDOWN] Done.\n");
    return 0;
}
