/**
 * @file sdr_server.c
 * @brief Phoenix SDR TCP Control Server
 *
 * TCP server for remote control of SDRplay RSP2 Pro.
 * Implements protocol defined in docs/SDR_TCP_CONTROL_INTERFACE.md
 * I/Q streaming on separate port per docs/SDR_IQ_STREAMING_INTERFACE.md
 *
 * Usage: sdr_server.exe [-p port] [-i iq_port] [-T addr]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <math.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include "tcp_server.h"
#include "phoenix_sdr.h"
#include "version.h"
#include <stdarg.h>

/*============================================================================
 * I/Q Streaming Protocol Definitions
 *============================================================================*/

#define IQ_DEFAULT_PORT 4536
#define IQ_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4 MB ring buffer */
#define IQ_FRAME_SAMPLES 8192                   /* Samples per frame */

/* Magic numbers */
#define IQ_MAGIC_HEADER 0x50485849  /* "PHXI" */
#define IQ_MAGIC_DATA   0x49514451  /* "IQDQ" */
#define IQ_MAGIC_META   0x4D455441  /* "META" */

/* Sample format codes */
#define IQ_FORMAT_S16 1
#define IQ_FORMAT_F32 2
#define IQ_FORMAT_U8  3

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           /* 0x50485849 = "PHXI" */
    uint32_t version;         /* Protocol version (1) */
    uint32_t sample_rate;     /* Current sample rate in Hz */
    uint32_t sample_format;   /* Format code */
    uint32_t center_freq_lo;  /* Center frequency low 32 bits */
    uint32_t center_freq_hi;  /* Center frequency high 32 bits */
    uint32_t gain_reduction;  /* IF gain reduction in dB */
    uint32_t lna_state;       /* LNA state (0-8) */
} iq_stream_header_t;

typedef struct {
    uint32_t magic;           /* 0x49514451 = "IQDQ" */
    uint32_t sequence;        /* Frame sequence number */
    uint32_t num_samples;     /* Number of I/Q pairs in this frame */
    uint32_t flags;           /* Bit flags */
} iq_data_frame_t;

typedef struct {
    uint32_t magic;           /* 0x4D455441 = "META" */
    uint32_t sample_rate;     /* New sample rate in Hz */
    uint32_t sample_format;   /* New format code */
    uint32_t center_freq_lo;  /* New center frequency low 32 bits */
    uint32_t center_freq_hi;  /* New center frequency high 32 bits */
    uint32_t gain_reduction;  /* IF gain reduction in dB */
    uint32_t lna_state;       /* LNA state (0-8) */
    uint32_t reserved;        /* Future use (0) */
} iq_metadata_update_t;
#pragma pack(pop)

/* I/Q frame flags */
#define IQ_FLAG_OVERLOAD    (1 << 0)
#define IQ_FLAG_FREQ_CHANGE (1 << 1)
#define IQ_FLAG_GAIN_CHANGE (1 << 2)

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static bool g_log_to_file = false;
static bool g_start_minimized = false;
static SOCKET g_listen_socket = INVALID_SOCKET;
static SOCKET g_client_socket = INVALID_SOCKET;
static tcp_sdr_state_t g_sdr_state;

/* I/Q streaming globals */
static SOCKET g_iq_listen_socket = INVALID_SOCKET;
static SOCKET g_iq_client_socket = INVALID_SOCKET;
static volatile bool g_iq_connected = false;
static volatile bool g_iq_streaming = false;

/* Ring buffer for I/Q samples */
static int16_t *g_iq_ring_buffer = NULL;
static volatile size_t g_iq_write_pos = 0;
static volatile size_t g_iq_read_pos = 0;
static volatile uint32_t g_iq_sequence = 0;
static volatile uint32_t g_iq_frames_sent = 0;
static volatile uint32_t g_iq_frames_dropped = 0;
static volatile uint32_t g_iq_current_flags = 0;
static volatile bool g_iq_config_changed = false;

/* Rate limiting for notifications */
static DWORD g_last_overload_notify_time = 0;
#define OVERLOAD_NOTIFY_COOLDOWN_MS 500  /* Min time between overload notifications */

/* SDR recovery tracking */
static DWORD g_last_stop_time = 0;
#define SDR_RESTART_COOLDOWN_MS 200  /* Min time between stop and start */

#ifdef _WIN32
static CRITICAL_SECTION g_iq_mutex;

/* System tray globals */
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
static HWND g_tray_hwnd = NULL;
static NOTIFYICONDATAA g_nid;
static bool g_tray_active = false;
#else
static pthread_mutex_t g_iq_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/*============================================================================
 * Notification Functions (thread-safe async notifications to client)
 *============================================================================*/

void tcp_notify_init(tcp_sdr_state_t *state) {
#ifdef _WIN32
    InitializeCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_init(&state->notify_mutex, NULL);
#endif
    state->client_socket = TCP_INVALID_SOCKET;
    state->notify_enabled = false;
}

void tcp_notify_cleanup(tcp_sdr_state_t *state) {
#ifdef _WIN32
    DeleteCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_destroy(&state->notify_mutex);
#endif
}

void tcp_notify_set_client(tcp_sdr_state_t *state, tcp_socket_t client) {
#ifdef _WIN32
    EnterCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_lock(&state->notify_mutex);
#endif
    state->client_socket = client;
    state->notify_enabled = true;
#ifdef _WIN32
    LeaveCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_unlock(&state->notify_mutex);
#endif
}

void tcp_notify_clear_client(tcp_sdr_state_t *state) {
#ifdef _WIN32
    EnterCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_lock(&state->notify_mutex);
#endif
    state->client_socket = TCP_INVALID_SOCKET;
    state->notify_enabled = false;
#ifdef _WIN32
    LeaveCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_unlock(&state->notify_mutex);
#endif
}

int tcp_send_notification(tcp_sdr_state_t *state, const char *format, ...) {
    if (!state) return -1;

    char buf[TCP_MAX_LINE_LENGTH];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf) - 1, format, args);
    va_end(args);

    if (len <= 0) return -1;

    /* Ensure newline termination */
    if (buf[len-1] != '\n') {
        buf[len++] = '\n';
        buf[len] = '\0';
    }

    int result = -1;

#ifdef _WIN32
    EnterCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_lock(&state->notify_mutex);
#endif

    if (state->notify_enabled && state->client_socket != TCP_INVALID_SOCKET) {
        int sent = send(state->client_socket, buf, len, 0);
        result = (sent == len) ? 0 : -1;
    }

#ifdef _WIN32
    LeaveCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_unlock(&state->notify_mutex);
#endif

    return result;
}

/*============================================================================
 * I/Q Ring Buffer Functions
 *============================================================================*/

static bool iq_buffer_init(void) {
    g_iq_ring_buffer = (int16_t*)malloc(IQ_RING_BUFFER_SIZE);
    if (!g_iq_ring_buffer) {
        fprintf(stderr, "Failed to allocate I/Q ring buffer\n");
        return false;
    }
    g_iq_write_pos = 0;
    g_iq_read_pos = 0;
    g_iq_sequence = 0;
    g_iq_frames_sent = 0;
    g_iq_frames_dropped = 0;
#ifdef _WIN32
    InitializeCriticalSection(&g_iq_mutex);
#endif
    printf("I/Q ring buffer: %d KB allocated\n", IQ_RING_BUFFER_SIZE / 1024);
    return true;
}

static void iq_buffer_cleanup(void) {
    if (g_iq_ring_buffer) {
        free(g_iq_ring_buffer);
        g_iq_ring_buffer = NULL;
    }
#ifdef _WIN32
    DeleteCriticalSection(&g_iq_mutex);
#endif
}

/* Get available space in ring buffer (in samples, I/Q pairs) */
static size_t iq_buffer_space(void) {
    size_t max_samples = IQ_RING_BUFFER_SIZE / (2 * sizeof(int16_t));
    size_t write = g_iq_write_pos;
    size_t read = g_iq_read_pos;
    if (write >= read) {
        return max_samples - (write - read) - 1;
    } else {
        return read - write - 1;
    }
}

/* Get available data in ring buffer (in samples, I/Q pairs) */
static size_t iq_buffer_available(void) {
    size_t max_samples = IQ_RING_BUFFER_SIZE / (2 * sizeof(int16_t));
    size_t write = g_iq_write_pos;
    size_t read = g_iq_read_pos;
    if (write >= read) {
        return write - read;
    } else {
        return max_samples - read + write;
    }
}

/* Write interleaved I/Q samples to ring buffer (called from SDR callback) */
static void iq_buffer_write(const int16_t *xi, const int16_t *xq, uint32_t count) {
    if (!g_iq_ring_buffer || !g_iq_connected) return;

    size_t max_samples = IQ_RING_BUFFER_SIZE / (2 * sizeof(int16_t));
    size_t space = iq_buffer_space();

    if (count > space) {
        /* Buffer overflow - drop oldest data */
        size_t drop = count - space;
        g_iq_read_pos = (g_iq_read_pos + drop) % max_samples;
        g_iq_frames_dropped++;
    }

    /* Write interleaved I/Q data */
    for (uint32_t i = 0; i < count; i++) {
        size_t pos = g_iq_write_pos * 2;
        g_iq_ring_buffer[pos] = xi[i];
        g_iq_ring_buffer[pos + 1] = xq[i];
        g_iq_write_pos = (g_iq_write_pos + 1) % max_samples;
    }
}

/* Read interleaved I/Q samples from ring buffer (called from I/Q thread) */
static size_t iq_buffer_read(int16_t *buffer, size_t max_samples) {
    if (!g_iq_ring_buffer) return 0;

    size_t available = iq_buffer_available();
    size_t to_read = (available < max_samples) ? available : max_samples;

    size_t buffer_max = IQ_RING_BUFFER_SIZE / (2 * sizeof(int16_t));

    for (size_t i = 0; i < to_read; i++) {
        size_t pos = g_iq_read_pos * 2;
        buffer[i * 2] = g_iq_ring_buffer[pos];
        buffer[i * 2 + 1] = g_iq_ring_buffer[pos + 1];
        g_iq_read_pos = (g_iq_read_pos + 1) % buffer_max;
    }

    return to_read;
}

/*============================================================================
 * I/Q Streaming Thread
 *============================================================================*/

static void send_iq_header(SOCKET sock) {
    iq_stream_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = IQ_MAGIC_HEADER;
    header.version = 1;
    header.sample_rate = (uint32_t)g_sdr_state.sample_rate;
    header.sample_format = IQ_FORMAT_S16;

    uint64_t freq = (uint64_t)g_sdr_state.freq_hz;
    header.center_freq_lo = (uint32_t)(freq & 0xFFFFFFFF);
    header.center_freq_hi = (uint32_t)(freq >> 32);
    header.gain_reduction = (uint32_t)g_sdr_state.gain_reduction;
    header.lna_state = (uint32_t)g_sdr_state.lna_state;

    send(sock, (const char*)&header, sizeof(header), 0);
    printf("[IQ] Sent stream header: %u Hz, format S16, GR=%u, LNA=%u\n",
           header.sample_rate, header.gain_reduction, header.lna_state);
}

static void send_iq_metadata(SOCKET sock) {
    iq_metadata_update_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = IQ_MAGIC_META;
    meta.sample_rate = (uint32_t)g_sdr_state.sample_rate;
    meta.sample_format = IQ_FORMAT_S16;

    uint64_t freq = (uint64_t)g_sdr_state.freq_hz;
    meta.center_freq_lo = (uint32_t)(freq & 0xFFFFFFFF);
    meta.center_freq_hi = (uint32_t)(freq >> 32);
    meta.gain_reduction = (uint32_t)g_sdr_state.gain_reduction;
    meta.lna_state = (uint32_t)g_sdr_state.lna_state;

    send(sock, (const char*)&meta, sizeof(meta), 0);
}

#ifdef _WIN32
static DWORD WINAPI iq_stream_thread_func(void *arg)
#else
static void *iq_stream_thread_func(void *arg)
#endif
{
    (void)arg;
    int16_t *frame_buffer = (int16_t*)malloc(IQ_FRAME_SAMPLES * 2 * sizeof(int16_t));
    if (!frame_buffer) {
        fprintf(stderr, "[IQ] Failed to allocate frame buffer\n");
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    /* Track last known config to detect changes */
    double last_freq = 0;
    int last_sample_rate = 0;
    int last_gain = 0;
    int last_lna = 0;

    printf("[IQ] Streaming thread started\n");

    while (g_running) {
        /* Accept I/Q client connection if none connected */
        if (!g_iq_connected || g_iq_client_socket == INVALID_SOCKET) {
            if (g_iq_listen_socket == INVALID_SOCKET) {
#ifdef _WIN32
                Sleep(100);
#else
                usleep(100000);
#endif
                continue;
            }

            /* Set non-blocking accept with timeout */
            fd_set read_fds;
            struct timeval tv;
            FD_ZERO(&read_fds);
            FD_SET(g_iq_listen_socket, &read_fds);
            tv.tv_sec = 0;
            tv.tv_usec = 100000;  /* 100ms timeout */

            int ready = select((int)(g_iq_listen_socket + 1), &read_fds, NULL, NULL, &tv);
            if (ready <= 0) {
                continue;
            }

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            SOCKET new_client = accept(g_iq_listen_socket, (struct sockaddr*)&client_addr, &client_len);
            if (new_client == INVALID_SOCKET) {
                continue;
            }

            /* Accept the new I/Q client */
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            printf("[IQ] Client connected from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

            g_iq_client_socket = new_client;
            g_iq_connected = true;
            g_iq_sequence = 0;
            g_iq_frames_sent = 0;
            g_iq_frames_dropped = 0;

            /* Reset config tracking */
            last_freq = g_sdr_state.freq_hz;
            last_sample_rate = g_sdr_state.sample_rate;
            last_gain = g_sdr_state.gain_reduction;
            last_lna = g_sdr_state.lna_state;

            /* Send stream header to new client */
            send_iq_header(g_iq_client_socket);
            continue;
        }

        /* Wait for streaming to be active */
        if (!g_sdr_state.streaming) {
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
            continue;
        }

        /* Send metadata update if config changed */
        if (g_iq_config_changed ||
            last_freq != g_sdr_state.freq_hz ||
            last_sample_rate != g_sdr_state.sample_rate ||
            last_gain != g_sdr_state.gain_reduction ||
            last_lna != g_sdr_state.lna_state) {

            g_iq_config_changed = false;
            last_freq = g_sdr_state.freq_hz;
            last_sample_rate = g_sdr_state.sample_rate;
            last_gain = g_sdr_state.gain_reduction;
            last_lna = g_sdr_state.lna_state;
            send_iq_metadata(g_iq_client_socket);
            printf("[IQ] Config changed, sent metadata update\n");
        }

        /* Read samples from ring buffer */
        size_t samples = iq_buffer_read(frame_buffer, IQ_FRAME_SAMPLES);

        /* If no samples and no hardware, generate test signal */
        if (samples == 0 && !g_sdr_state.hardware_connected) {
            /* Generate test signal: 1kHz tone at ~half scale */
            static double phase = 0.0;
            double freq_hz = 1000.0;
            double sample_rate = (double)g_sdr_state.sample_rate;
            double phase_inc = 2.0 * 3.14159265358979 * freq_hz / sample_rate;

            for (size_t i = 0; i < IQ_FRAME_SAMPLES; i++) {
                double val = sin(phase) * 16000.0;
                frame_buffer[i * 2] = (int16_t)val;       /* I */
                frame_buffer[i * 2 + 1] = (int16_t)(val * 0.5);  /* Q (phase shift for complex signal) */
                phase += phase_inc;
                if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
            }
            samples = IQ_FRAME_SAMPLES;

            /* Simulate sample rate timing */
            double frame_time_ms = (1000.0 * IQ_FRAME_SAMPLES) / sample_rate;
#ifdef _WIN32
            Sleep((DWORD)(frame_time_ms + 0.5));
#else
            usleep((useconds_t)(frame_time_ms * 1000));
#endif
        } else if (samples == 0) {
#ifdef _WIN32
            Sleep(1);
#else
            usleep(1000);
#endif
            continue;
        }

        /* Build frame header */
        iq_data_frame_t frame;
        frame.magic = IQ_MAGIC_DATA;
        frame.sequence = g_iq_sequence++;
        frame.num_samples = (uint32_t)samples;
        frame.flags = g_iq_current_flags;
        g_iq_current_flags = 0;  /* Clear flags after sending */

        /* Send frame header */
        int sent = send(g_iq_client_socket, (const char*)&frame, sizeof(frame), 0);
        if (sent != sizeof(frame)) {
            printf("[IQ] Client disconnected (header send failed)\n");
            closesocket(g_iq_client_socket);
            g_iq_client_socket = INVALID_SOCKET;
            g_iq_connected = false;
            continue;
        }

        /* Send sample data */
        size_t data_size = samples * 2 * sizeof(int16_t);
        sent = send(g_iq_client_socket, (const char*)frame_buffer, (int)data_size, 0);
        if (sent != (int)data_size) {
            printf("[IQ] Client disconnected (data send failed)\n");
            closesocket(g_iq_client_socket);
            g_iq_client_socket = INVALID_SOCKET;
            g_iq_connected = false;
            continue;
        }

        g_iq_frames_sent++;
    }

    /* Cleanup */
    if (g_iq_client_socket != INVALID_SOCKET) {
        closesocket(g_iq_client_socket);
        g_iq_client_socket = INVALID_SOCKET;
    }
    g_iq_connected = false;

    free(frame_buffer);
    printf("[IQ] Streaming thread stopped\n");

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/*============================================================================
 * SDR Callbacks
 *============================================================================*/

static void on_samples(const int16_t *xi, const int16_t *xq,
                      uint32_t count, bool reset, void *user_ctx) {
    (void)reset; (void)user_ctx;

    /* Write samples to I/Q ring buffer for TCP streaming */
    if (g_iq_connected && g_sdr_state.streaming) {
        iq_buffer_write(xi, xq, count);
    }
}

static void on_gain_change(double gain_db, int lna_gr_db, void *user_ctx) {
    tcp_sdr_state_t *state = (tcp_sdr_state_t*)user_ctx;
    if (state) {
        /* Note: lna_gr_db is LNA gain reduction in dB (0-24), NOT state index (0-8) */
        /* We DON'T update state->gain_reduction here - it reflects what was SET by user */
        /* The callback just reports what the hardware is actually using */

        /* Log the hardware-reported values for debugging */
        printf("[SDR] Gain changed: GR=%.0f dB, LNA_GR=%d dB\n", gain_db, lna_gr_db);

        /* Send notification with actual hardware values (informational only) */
        tcp_send_notification(state, "! GAIN_CHANGE GR_ACTUAL=%d LNA_GR=%d",
                              (int)gain_db, lna_gr_db);
    }
}

static void on_overload(bool overloaded, void *user_ctx) {
    tcp_sdr_state_t *state = (tcp_sdr_state_t*)user_ctx;
    if (state) {
        bool was_overloaded = state->overload;
        state->overload = overloaded;

        /* Set I/Q stream flag */
        if (overloaded) {
            g_iq_current_flags |= IQ_FLAG_OVERLOAD;
        }

        /* Only notify if state changed AND rate limit not exceeded */
        if (was_overloaded != overloaded) {
            DWORD now = GetTickCount();
            DWORD elapsed = now - g_last_overload_notify_time;

            if (elapsed >= OVERLOAD_NOTIFY_COOLDOWN_MS) {
                printf("[SDR] %s\n", overloaded ? "OVERLOAD DETECTED" : "Overload cleared");
                tcp_send_notification(state, "! OVERLOAD %s",
                                      overloaded ? "DETECTED" : "CLEARED");
                g_last_overload_notify_time = now;
            }
        }
    }
}

/*============================================================================
 * System Tray Functions (Windows only)
 *============================================================================*/

#ifdef _WIN32
static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                /* Show context menu */
                POINT pt;
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit SDR Server");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_EXIT) {
                g_running = false;
                PostQuitMessage(0);
            }
            return 0;

        case WM_DESTROY:
            if (g_tray_active) {
                Shell_NotifyIconA(NIM_DELETE, &g_nid);
                g_tray_active = false;
            }
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static bool init_tray_icon(void) {
    /* Register window class for tray message handling */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = tray_wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "PhoenixSDRTrayClass";
    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "Failed to register tray window class\n");
        return false;
    }

    /* Create hidden message window */
    g_tray_hwnd = CreateWindowA("PhoenixSDRTrayClass", "Phoenix SDR Server",
                                 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                 GetModuleHandle(NULL), NULL);
    if (!g_tray_hwnd) {
        fprintf(stderr, "Failed to create tray window\n");
        return false;
    }

    /* Set up notification icon data */
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = g_tray_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "Phoenix SDR Server v%s", PHOENIX_VERSION_STRING);

    /* Add the icon to system tray */
    if (!Shell_NotifyIconA(NIM_ADD, &g_nid)) {
        fprintf(stderr, "Failed to add tray icon\n");
        DestroyWindow(g_tray_hwnd);
        return false;
    }

    g_tray_active = true;
    return true;
}

static void cleanup_tray_icon(void) {
    if (g_tray_active) {
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        g_tray_active = false;
    }
    if (g_tray_hwnd) {
        DestroyWindow(g_tray_hwnd);
        g_tray_hwnd = NULL;
    }
    UnregisterClassA("PhoenixSDRTrayClass", GetModuleHandle(NULL));
}

/* Process tray messages - call periodically from main loop */
static void process_tray_messages(void) {
    if (!g_tray_hwnd) return;
    MSG msg;
    while (PeekMessage(&msg, g_tray_hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
#endif

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    g_running = false;

    /* Send DISCONNECT notification to client before closing */
    if (g_client_socket != INVALID_SOCKET) {
        const char *disconnect_msg = "! DISCONNECT server shutdown\n";
        send(g_client_socket, disconnect_msg, (int)strlen(disconnect_msg), 0);
    }

    /* Stop SDR streaming if active */
    if (g_sdr_state.streaming && g_sdr_state.sdr_ctx) {
        printf("Stopping SDR streaming...\n");
        psdr_stop(g_sdr_state.sdr_ctx);
        g_sdr_state.streaming = false;
    }

    /* Close I/Q sockets */
    if (g_iq_client_socket != INVALID_SOCKET) {
        closesocket(g_iq_client_socket);
        g_iq_client_socket = INVALID_SOCKET;
        g_iq_connected = false;
    }
    if (g_iq_listen_socket != INVALID_SOCKET) {
        closesocket(g_iq_listen_socket);
        g_iq_listen_socket = INVALID_SOCKET;
    }

    /* Close control sockets to unblock accept/recv */
    if (g_client_socket != INVALID_SOCKET) {
        closesocket(g_client_socket);
        g_client_socket = INVALID_SOCKET;
    }
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }
}

/*============================================================================
 * Socket Helpers
 *============================================================================*/

static int socket_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    return 0;
#endif
}

static void socket_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static int send_line(SOCKET sock, const char *line) {
    size_t len = strlen(line);
    int sent = send(sock, line, (int)len, 0);
    return (sent == (int)len) ? 0 : -1;
}

static int recv_line(SOCKET sock, char *buf, int buf_size) {
    int total = 0;
    while (total < buf_size - 1) {
        char c;
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) {
            /* Connection closed or error */
            return -1;
        }
        if (c == '\n') {
            buf[total] = '\0';
            return total;
        }
        if (c != '\r') {
            buf[total++] = c;
        }
    }
    buf[total] = '\0';
    return total;
}

/*============================================================================
 * Forward Declarations
 *============================================================================*/

static bool init_sdr(tcp_sdr_state_t *state, int device_idx);
static void cleanup_sdr(tcp_sdr_state_t *state);
static bool reinit_sdr(tcp_sdr_state_t *state);

/*============================================================================
 * Client Handler
 *============================================================================*/

static void handle_client(SOCKET client, tcp_sdr_state_t *state) {
    char line[TCP_MAX_LINE_LENGTH];
    char response[TCP_MAX_LINE_LENGTH + 16];
    tcp_command_t cmd;
    tcp_response_t resp;

    printf("Client connected\n");

    /* Enable async notifications for this client */
    tcp_notify_set_client(state, client);

    while (g_running) {
        /* Receive command */
        int len = recv_line(client, line, sizeof(line));
        if (len < 0) {
            printf("Client disconnected\n");
            break;
        }
        if (len == 0) {
            continue;  /* Empty line */
        }

        printf("< %s\n", line);

        /* Parse command */
        tcp_error_t err = tcp_parse_command(line, &cmd);

        if (err == TCP_OK) {
            /* Suppress notifications during command execution to ensure
             * the command response is sent before any async notifications.
             * This prevents ! GAIN_CHANGE from arriving before OK. */
            state->notify_enabled = false;

            /* Handle START cooldown - SDR needs time to reset after STOP */
            if (cmd.type == CMD_START && g_last_stop_time != 0) {
                DWORD now = GetTickCount();
                DWORD elapsed = now - g_last_stop_time;
                if (elapsed < SDR_RESTART_COOLDOWN_MS) {
                    DWORD wait_ms = SDR_RESTART_COOLDOWN_MS - elapsed;
                    printf("[SDR] Waiting %lu ms for SDR cooldown...\n", wait_ms);
                    Sleep(wait_ms);
                }
            }

            /* Execute command */
            tcp_execute_command(&cmd, state, &resp);

            /* If START failed with hardware error, try recovery and retry once */
            if (cmd.type == CMD_START && resp.error == TCP_ERR_HARDWARE) {
                printf("[SDR] START failed, attempting recovery...\n");
                if (reinit_sdr(state)) {
                    /* Retry the START command */
                    tcp_execute_command(&cmd, state, &resp);
                }
            }

            /* Track STOP time for cooldown */
            if (cmd.type == CMD_STOP && resp.error == TCP_OK) {
                g_last_stop_time = GetTickCount();
            }
        } else {
            /* Parse error */
            const char *msg = NULL;
            switch (err) {
                case TCP_ERR_SYNTAX:   msg = "malformed command"; break;
                case TCP_ERR_UNKNOWN:  msg = "unknown command"; break;
                case TCP_ERR_RANGE:    msg = "value out of range"; break;
                case TCP_ERR_PARAM:    msg = "invalid parameter"; break;
                default:               msg = NULL; break;
            }
            tcp_response_error(&resp, err, msg);
        }

        /* Format and send response */
        tcp_format_response(&resp, response, sizeof(response));
        printf("> %s", response);  /* Already has \n */

        if (send_line(client, response) < 0) {
            printf("Send failed\n");
            break;
        }

        /* Re-enable async notifications after response is sent */
        state->notify_enabled = true;

        /* Handle QUIT */
        if (cmd.type == CMD_QUIT) {
            break;
        }
    }

    /* Stop streaming if client disconnects */
    if (state->streaming) {
        printf("Stopping streaming (client disconnect)\n");
        if (state->hardware_connected && state->sdr_ctx) {
            psdr_stop(state->sdr_ctx);
            g_last_stop_time = GetTickCount();  /* Track stop time for restart cooldown */
        }
        state->streaming = false;
    }

    /* Reset overload state for next client */
    state->overload = false;
    g_last_overload_notify_time = 0;

    /* Disable async notifications */
    tcp_notify_clear_client(state);

    printf("Client session cleanup complete\n");
}

/*============================================================================
 * Usage
 *============================================================================*/

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -p PORT    Control port (default: %d)\n", TCP_DEFAULT_PORT);
    printf("  -i PORT    I/Q stream port (default: %d)\n", IQ_DEFAULT_PORT);
    printf("  -T ADDR    Listen address (default: 127.0.0.1)\n");
    printf("  -I         Disable I/Q streaming port\n");
    printf("  -d INDEX   Select SDR device index (default: 0)\n");
    printf("  -l         Log output to file (sdr_server_<version>.log)\n");
    printf("  -m         Start minimized (or hidden if -l also set)\n");
    printf("  -h         Show this help\n");
    printf("\nProtocol: See docs/SDR_TCP_CONTROL_INTERFACE.md\n");
    printf("I/Q Stream: See docs/SDR_IQ_STREAMING_INTERFACE.md\n");
}

/*============================================================================
 * SDR Initialization
 *============================================================================*/

static bool init_sdr(tcp_sdr_state_t *state, int device_idx) {
    printf("Initializing SDR hardware...\n");

    /* Enumerate devices */
    psdr_device_info_t devices[4];
    size_t num_devices = 0;

    psdr_error_t err = psdr_enumerate(devices, 4, &num_devices);
    if (err != PSDR_OK) {
        fprintf(stderr, "SDR enumeration failed: %s\n", psdr_strerror(err));
        return false;
    }

    if (num_devices == 0) {
        fprintf(stderr, "No SDR devices found\n");
        return false;
    }

    printf("Found %zu SDR device(s):\n", num_devices);
    for (size_t i = 0; i < num_devices; i++) {
        printf("  [%zu] %s (HW v%d)%s\n", i,
               devices[i].serial,
               devices[i].hw_version,
               devices[i].available ? "" : " [in use]");
    }

    if (device_idx >= (int)num_devices) {
        fprintf(stderr, "Device index %d out of range (0-%zu)\n",
                device_idx, num_devices - 1);
        return false;
    }

    if (!devices[device_idx].available) {
        fprintf(stderr, "Device %d is in use by another application\n", device_idx);
        return false;
    }

    /* Open selected device */
    err = psdr_open(&state->sdr_ctx, (unsigned int)device_idx);
    if (err != PSDR_OK) {
        fprintf(stderr, "Failed to open SDR: %s\n", psdr_strerror(err));
        return false;
    }

    /* Set up callbacks */
    state->sdr_callbacks.on_samples = on_samples;
    state->sdr_callbacks.on_gain_change = on_gain_change;
    state->sdr_callbacks.on_overload = on_overload;
    state->sdr_callbacks.user_ctx = state;

    /* Configure with defaults from state */
    psdr_config_defaults(&state->sdr_config);
    state->sdr_config.freq_hz = state->freq_hz;
    state->sdr_config.sample_rate_hz = (double)state->sample_rate;
    state->sdr_config.bandwidth = (psdr_bandwidth_t)state->bandwidth_khz;
    state->sdr_config.gain_reduction = state->gain_reduction;
    state->sdr_config.lna_state = state->lna_state;

    err = psdr_configure(state->sdr_ctx, &state->sdr_config);
    if (err != PSDR_OK) {
        fprintf(stderr, "Failed to configure SDR: %s\n", psdr_strerror(err));
        psdr_close(state->sdr_ctx);
        state->sdr_ctx = NULL;
        return false;
    }

    state->hardware_connected = true;
    printf("SDR initialized: %s\n", devices[device_idx].serial);
    printf("  Frequency:    %.3f MHz\n", state->freq_hz / 1e6);
    printf("  Sample Rate:  %.3f MSPS\n", state->sample_rate / 1e6);
    printf("  Bandwidth:    %d kHz\n", state->bandwidth_khz);
    printf("  Gain Red:     %d dB\n", state->gain_reduction);
    printf("  LNA State:    %d\n", state->lna_state);

    return true;
}

static void cleanup_sdr(tcp_sdr_state_t *state) {
    if (state->sdr_ctx) {
        if (state->streaming) {
            printf("Stopping SDR streaming...\n");
            psdr_stop(state->sdr_ctx);
            state->streaming = false;
        }
        printf("Closing SDR...\n");
        psdr_close(state->sdr_ctx);
        state->sdr_ctx = NULL;
        state->hardware_connected = false;
    }
}

/* Track device index for reinit */
static int g_sdr_device_idx = 0;

/**
 * @brief Attempt to reinitialize SDR after a failure
 *
 * Called when hardware operations fail to try to recover.
 */
static bool reinit_sdr(tcp_sdr_state_t *state) {
    printf("[SDR] Attempting hardware recovery...\n");

    /* Clean up existing context */
    cleanup_sdr(state);

    /* Wait a moment for hardware to reset */
    Sleep(500);

    /* Try to reinitialize */
    if (init_sdr(state, g_sdr_device_idx)) {
        printf("[SDR] Hardware recovery successful\n");
        return true;
    }

    printf("[SDR] Hardware recovery failed\n");
    return false;
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    print_version("Phoenix SDR - TCP Control Server");

    int port = TCP_DEFAULT_PORT;
    int iq_port = IQ_DEFAULT_PORT;
    const char *bind_addr = "127.0.0.1";
    bool iq_enabled = true;
    int device_idx = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iq_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-I") == 0) {
            iq_enabled = false;
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device_idx = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0) {
            g_log_to_file = true;
        } else if (strcmp(argv[i], "-m") == 0) {
            g_start_minimized = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Set up file logging if enabled */
    if (g_log_to_file) {
        char log_filename[512];
        char exe_dir[512];

        /* Get directory of the executable */
        strncpy(exe_dir, argv[0], sizeof(exe_dir) - 1);
        exe_dir[sizeof(exe_dir) - 1] = '\0';

        /* Find last path separator and truncate to get directory */
        char *last_sep = strrchr(exe_dir, '\\');
        if (!last_sep) last_sep = strrchr(exe_dir, '/');
        if (last_sep) {
            *(last_sep + 1) = '\0';  /* Keep the trailing slash */
            snprintf(log_filename, sizeof(log_filename), "%ssdr_server_%s.log", exe_dir, PHOENIX_VERSION_STRING);
        } else {
            /* No path separator, use current directory */
            snprintf(log_filename, sizeof(log_filename), "sdr_server_%s.log", PHOENIX_VERSION_STRING);
        }

        if (freopen(log_filename, "w", stdout) == NULL) {
            fprintf(stderr, "Failed to open log file: %s\n", log_filename);
            return 1;
        }
        freopen(log_filename, "a", stderr);  /* Append stderr to same file */
    }

    /* Handle system tray / minimize mode */
    if (g_start_minimized) {
#ifdef _WIN32
        /* Initialize system tray icon */
        if (!init_tray_icon()) {
            fprintf(stderr, "Warning: Failed to create system tray icon\n");
        }

        /* Hide the console window - tray icon is now visible */
        HWND console = GetConsoleWindow();
        if (console) {
            ShowWindow(console, SW_HIDE);
        }
#endif
    }

    /* Initialize SDR state */
    tcp_state_defaults(&g_sdr_state);

    /* Initialize notification mutex */
    tcp_notify_init(&g_sdr_state);

    /* Initialize SDR hardware */
    g_sdr_device_idx = device_idx;  /* Save for reinit */
    if (!init_sdr(&g_sdr_state, device_idx)) {
        fprintf(stderr, "Warning: Running without SDR hardware\n");
    }

    /* Initialize sockets */
    if (socket_init() != 0) {
        fprintf(stderr, "Socket initialization failed\n");
        cleanup_sdr(&g_sdr_state);
        return 1;
    }

    /* Initialize I/Q ring buffer */
    if (iq_enabled) {
        if (!iq_buffer_init()) {
            fprintf(stderr, "Failed to allocate I/Q ring buffer\n");
            cleanup_sdr(&g_sdr_state);
            socket_cleanup();
            return 1;
        }
    }

    /* Set up signal handler */
    signal(SIGINT, signal_handler);
#ifndef _WIN32
    signal(SIGTERM, signal_handler);
#endif

    /* Create control listen socket */
    g_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "Failed to create control socket\n");
        iq_buffer_cleanup();
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    /* Allow address reuse */
    int optval = 1;
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));

    /* Bind control socket */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_addr);
        closesocket(g_listen_socket);
        iq_buffer_cleanup();
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    if (bind(g_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Bind failed on %s:%d\n", bind_addr, port);
        closesocket(g_listen_socket);
        iq_buffer_cleanup();
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    /* Listen on control socket */
    if (listen(g_listen_socket, 1) == SOCKET_ERROR) {
        fprintf(stderr, "Listen failed on control socket\n");
        closesocket(g_listen_socket);
        iq_buffer_cleanup();
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    printf("Control port: %s:%d\n", bind_addr, port);

    /* Create I/Q stream listen socket */
    HANDLE iq_thread = NULL;
    if (iq_enabled) {
        g_iq_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (g_iq_listen_socket == INVALID_SOCKET) {
            fprintf(stderr, "Failed to create I/Q socket\n");
            closesocket(g_listen_socket);
            iq_buffer_cleanup();
            cleanup_sdr(&g_sdr_state);
            socket_cleanup();
            return 1;
        }

        setsockopt(g_iq_listen_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));

        struct sockaddr_in iq_addr;
        memset(&iq_addr, 0, sizeof(iq_addr));
        iq_addr.sin_family = AF_INET;
        iq_addr.sin_port = htons((unsigned short)iq_port);
        inet_pton(AF_INET, bind_addr, &iq_addr.sin_addr);

        if (bind(g_iq_listen_socket, (struct sockaddr*)&iq_addr, sizeof(iq_addr)) == SOCKET_ERROR) {
            fprintf(stderr, "Bind failed on I/Q port %s:%d\n", bind_addr, iq_port);
            closesocket(g_iq_listen_socket);
            closesocket(g_listen_socket);
            iq_buffer_cleanup();
            cleanup_sdr(&g_sdr_state);
            socket_cleanup();
            return 1;
        }

        if (listen(g_iq_listen_socket, 1) == SOCKET_ERROR) {
            fprintf(stderr, "Listen failed on I/Q socket\n");
            closesocket(g_iq_listen_socket);
            closesocket(g_listen_socket);
            iq_buffer_cleanup();
            cleanup_sdr(&g_sdr_state);
            socket_cleanup();
            return 1;
        }

        printf("I/Q stream port: %s:%d\n", bind_addr, iq_port);

        /* Start I/Q streaming thread */
        iq_thread = CreateThread(NULL, 0, iq_stream_thread_func, NULL, 0, NULL);
        if (!iq_thread) {
            fprintf(stderr, "Failed to create I/Q streaming thread\n");
        }
    } else {
        printf("I/Q streaming: DISABLED\n");
    }

    printf("Hardware: %s\n", g_sdr_state.hardware_connected ? "CONNECTED" : "NOT CONNECTED");

    printf("Press Ctrl+C to stop\n\n");

    /* Accept loop */
    while (g_running) {
#ifdef _WIN32
        /* Process system tray messages if in minimized mode */
        if (g_start_minimized) {
            process_tray_messages();
        }
#endif

        /* Use select with timeout so we can process tray messages */
        fd_set read_fds;
        struct timeval tv;
        FD_ZERO(&read_fds);
        FD_SET(g_listen_socket, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms timeout */

        int ready = select((int)(g_listen_socket + 1), &read_fds, NULL, NULL, &tv);
        if (ready <= 0) {
            continue;  /* Timeout or error, loop to check g_running and process messages */
        }

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        g_client_socket = accept(g_listen_socket, (struct sockaddr*)&client_addr, &client_len);
        if (g_client_socket == INVALID_SOCKET) {
            if (g_running) {
                fprintf(stderr, "Accept failed\n");
            }
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("Connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        /* Handle this client (blocking - single client only) */
        handle_client(g_client_socket, &g_sdr_state);

        closesocket(g_client_socket);
        g_client_socket = INVALID_SOCKET;
        printf("Client session ended\n\n");
    }

    /* Wait for I/Q thread to finish */
    if (iq_thread) {
        WaitForSingleObject(iq_thread, 2000);
        CloseHandle(iq_thread);
    }

    /* Cleanup */
#ifdef _WIN32
    if (g_start_minimized) {
        cleanup_tray_icon();
    }
#endif
    cleanup_sdr(&g_sdr_state);
    tcp_notify_cleanup(&g_sdr_state);
    iq_buffer_cleanup();

    if (g_iq_listen_socket != INVALID_SOCKET) {
        closesocket(g_iq_listen_socket);
    }
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
    }
    socket_cleanup();

    printf("Server stopped\n");
    return 0;
}
