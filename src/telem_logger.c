/**
 * @file telem_logger.c
 * @brief UDP telemetry listener and CSV logger for Phoenix SDR
 *
 * Listens on UDP port 3005 (or specified) for telemetry broadcasts from
 * waterfall.exe and writes received data to CSV files organized by channel.
 * Runs in the system tray on Windows for background operation.
 *
 * Usage:
 *   telem_logger.exe                      # Listen on default port 3005
 *   telem_logger.exe -p 3005              # Specify port
 *   telem_logger.exe -o logs/             # Specify output directory
 *   telem_logger.exe -c TICK,MARK,SYNC    # Filter to specific channels
 *   telem_logger.exe -v                   # Verbose mode (print to console)
 *   telem_logger.exe --no-tray            # Disable system tray (console only)
 *
 * Output Files:
 *   <outdir>/telem_CHAN_YYYYMMDD_HHMMSS.csv
 *   <outdir>/telem_TICK_YYYYMMDD_HHMMSS.csv
 *   <outdir>/telem_MARK_YYYYMMDD_HHMMSS.csv
 *   etc.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <shellapi.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "shell32.lib")
    typedef int socklen_t;
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <errno.h>
    #define SOCKET int
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR (-1)
    #define closesocket close
#endif

#include "version.h"

/*============================================================================
 * Configuration
 *============================================================================*/

#define DEFAULT_PORT        3005
#define MAX_MESSAGE_LEN     512
#define MAX_CHANNELS        16
#define MAX_CHANNEL_NAME    8
#define MAX_PATH_LEN        260

/* Known channel prefixes */
static const char *KNOWN_CHANNELS[] = {
    "CHAN", "TICK", "MARK", "CARR", "SYNC", "SUBC",
    "CORR", "T500", "T600", "BCDE", "BCDS", "SYM",
    "STATE", "STATUS", "CONS", NULL
};

/*============================================================================
 * System Tray (Windows)
 *============================================================================*/

#ifdef _WIN32
#define WM_TRAYICON         (WM_USER + 1)
#define ID_TRAY_EXIT        1001
#define ID_TRAY_STATUS      1002
#define ID_TRAY_OPEN_LOGS   1003
#define ID_TRAY_PAUSE       1004

static HWND g_tray_hwnd = NULL;
static NOTIFYICONDATAA g_nid;
static bool g_tray_active = false;
static bool g_tray_enabled = true;
static bool g_paused = false;
#endif

/*============================================================================
 * Types
 *============================================================================*/

typedef struct {
    char name[MAX_CHANNEL_NAME];
    FILE *csv_file;
    char csv_path[MAX_PATH_LEN];
    uint64_t message_count;
} channel_log_t;

typedef struct {
    int port;
    char output_dir[MAX_PATH_LEN];
    bool verbose;
    bool filter_enabled;
    char filter_channels[MAX_CHANNELS][MAX_CHANNEL_NAME];
    int filter_count;
    channel_log_t channels[MAX_CHANNELS];
    int channel_count;
    SOCKET sock;
    bool running;
    uint64_t total_messages;
    time_t start_time;
} telem_logger_t;

static telem_logger_t g_logger = {0};

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    g_logger.running = false;
}

/*============================================================================
 * System Tray Functions (Windows)
 *============================================================================*/

#ifdef _WIN32
static void update_tray_tooltip(void);

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                /* Show context menu */
                POINT pt;
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();

                /* Status item (disabled, just for display) */
                char status[128];
                snprintf(status, sizeof(status), "Messages: %llu | Channels: %d",
                         (unsigned long long)g_logger.total_messages, g_logger.channel_count);
                AppendMenuA(menu, MF_STRING | MF_DISABLED, ID_TRAY_STATUS, status);
                AppendMenuA(menu, MF_SEPARATOR, 0, NULL);

                /* Pause/Resume */
                AppendMenuA(menu, MF_STRING | (g_paused ? MF_CHECKED : 0),
                           ID_TRAY_PAUSE, g_paused ? "Resume Logging" : "Pause Logging");

                /* Open logs folder */
                if (g_logger.output_dir[0] || g_logger.channel_count > 0) {
                    AppendMenuA(menu, MF_STRING, ID_TRAY_OPEN_LOGS, "Open Logs Folder");
                }

                AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit Telemetry Logger");

                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_EXIT:
                    g_logger.running = false;
                    PostQuitMessage(0);
                    break;
                case ID_TRAY_PAUSE:
                    g_paused = !g_paused;
                    update_tray_tooltip();
                    break;
                case ID_TRAY_OPEN_LOGS: {
                    /* Open the logs folder in Explorer */
                    char path[MAX_PATH_LEN];
                    if (g_logger.output_dir[0]) {
                        strncpy(path, g_logger.output_dir, sizeof(path) - 1);
                    } else if (g_logger.channel_count > 0) {
                        /* Get directory from first log file */
                        strncpy(path, g_logger.channels[0].csv_path, sizeof(path) - 1);
                        char *last_sep = strrchr(path, '\\');
                        if (!last_sep) last_sep = strrchr(path, '/');
                        if (last_sep) *last_sep = '\0';
                        else GetCurrentDirectoryA(sizeof(path), path);
                    } else {
                        GetCurrentDirectoryA(sizeof(path), path);
                    }
                    ShellExecuteA(NULL, "explore", path, NULL, NULL, SW_SHOWNORMAL);
                    break;
                }
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

static void update_tray_tooltip(void) {
    if (!g_tray_active) return;

    if (g_paused) {
        snprintf(g_nid.szTip, sizeof(g_nid.szTip),
                 "Telemetry Logger - PAUSED (%llu msgs)",
                 (unsigned long long)g_logger.total_messages);
    } else {
        snprintf(g_nid.szTip, sizeof(g_nid.szTip),
                 "Telemetry Logger - %llu msgs, %d channels",
                 (unsigned long long)g_logger.total_messages, g_logger.channel_count);
    }
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

static bool init_tray_icon(void) {
    if (!g_tray_enabled) return true;

    /* Register window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = tray_wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "PhoenixTelemLoggerClass";
    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "[telem_logger] Failed to register tray window class\n");
        return false;
    }

    /* Create hidden message window */
    g_tray_hwnd = CreateWindowA("PhoenixTelemLoggerClass", "Phoenix Telemetry Logger",
                                 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                 GetModuleHandle(NULL), NULL);
    if (!g_tray_hwnd) {
        fprintf(stderr, "[telem_logger] Failed to create tray window\n");
        return false;
    }

    /* Set up notification icon */
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = g_tray_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "Telemetry Logger v%s - Starting...", PHOENIX_VERSION_STRING);

    if (!Shell_NotifyIconA(NIM_ADD, &g_nid)) {
        fprintf(stderr, "[telem_logger] Failed to add tray icon\n");
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
    UnregisterClassA("PhoenixTelemLoggerClass", GetModuleHandle(NULL));
}

static void process_tray_messages(void) {
    if (!g_tray_hwnd) return;
    MSG msg;
    while (PeekMessage(&msg, g_tray_hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
#endif /* _WIN32 */

/*============================================================================
 * Helper Functions
 *============================================================================*/

static void print_usage(const char *prog) {
    printf("Phoenix SDR Telemetry Logger v%s\n", PHOENIX_VERSION_STRING);
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -p <port>       UDP port to listen on (default: %d)\n", DEFAULT_PORT);
    printf("  -o <dir>        Output directory for CSV files (default: current dir)\n");
    printf("  -c <channels>   Comma-separated list of channels to log (default: all)\n");
    printf("                  Available: CHAN,TICK,MARK,CARR,SYNC,SUBC,T500,T600,BCDS\n");
    printf("  -v              Verbose mode (print messages to console)\n");
#ifdef _WIN32
    printf("  --no-tray       Disable system tray icon (console only mode)\n");
#endif
    printf("  -h              Show this help\n\n");
    printf("Examples:\n");
    printf("  %s                          # Log all channels to current directory\n", prog);
    printf("  %s -o logs/ -v              # Log to logs/ with console output\n", prog);
    printf("  %s -c TICK,MARK,SYNC        # Log only tick, marker, and sync channels\n", prog);
}

static void get_timestamp_str(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(buf, len, "%04d%02d%02d_%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static bool extract_channel_prefix(const char *message, char *prefix, size_t prefix_len) {
    /* Find the first comma to extract prefix */
    const char *comma = strchr(message, ',');
    if (!comma) {
        /* Some messages like "STATE,..." don't have timestamp first */
        /* Check if message starts with a known prefix */
        for (int i = 0; KNOWN_CHANNELS[i]; i++) {
            size_t plen = strlen(KNOWN_CHANNELS[i]);
            if (strncmp(message, KNOWN_CHANNELS[i], plen) == 0 &&
                (message[plen] == ',' || message[plen] == '\0')) {
                strncpy(prefix, KNOWN_CHANNELS[i], prefix_len - 1);
                prefix[prefix_len - 1] = '\0';
                return true;
            }
        }
        return false;
    }

    size_t len = (size_t)(comma - message);
    if (len >= prefix_len || len == 0) {
        return false;
    }

    strncpy(prefix, message, len);
    prefix[len] = '\0';
    return true;
}

static bool is_channel_filtered(const char *channel) {
    if (!g_logger.filter_enabled) {
        return false;  /* No filter, allow all */
    }

    for (int i = 0; i < g_logger.filter_count; i++) {
        if (strcmp(channel, g_logger.filter_channels[i]) == 0) {
            return false;  /* In filter list, allow */
        }
    }
    return true;  /* Not in filter list, reject */
}

static channel_log_t *find_or_create_channel(const char *channel) {
    /* Search existing channels */
    for (int i = 0; i < g_logger.channel_count; i++) {
        if (strcmp(g_logger.channels[i].name, channel) == 0) {
            return &g_logger.channels[i];
        }
    }

    /* Create new channel */
    if (g_logger.channel_count >= MAX_CHANNELS) {
        fprintf(stderr, "[telem_logger] Warning: Max channels reached, ignoring %s\n", channel);
        return NULL;
    }

    channel_log_t *ch = &g_logger.channels[g_logger.channel_count];
    strncpy(ch->name, channel, sizeof(ch->name) - 1);
    ch->name[sizeof(ch->name) - 1] = '\0';
    ch->message_count = 0;

    /* Create CSV file */
    char timestamp[32];
    get_timestamp_str(timestamp, sizeof(timestamp));

    if (strlen(g_logger.output_dir) > 0) {
        snprintf(ch->csv_path, sizeof(ch->csv_path), "%s\\telem_%s_%s.csv",
                 g_logger.output_dir, channel, timestamp);
    } else {
        snprintf(ch->csv_path, sizeof(ch->csv_path), "telem_%s_%s.csv",
                 channel, timestamp);
    }

    ch->csv_file = fopen(ch->csv_path, "w");
    if (!ch->csv_file) {
        fprintf(stderr, "[telem_logger] Error: Cannot create %s\n", ch->csv_path);
        return NULL;
    }

    /* Write CSV header comment */
    time_t now = time(NULL);
    fprintf(ch->csv_file, "# Phoenix SDR Telemetry Log - Channel: %s\n", channel);
    fprintf(ch->csv_file, "# Started: %s", ctime(&now));
    fprintf(ch->csv_file, "# Source: UDP port %d\n", g_logger.port);
    fflush(ch->csv_file);

    printf("[telem_logger] Created log: %s\n", ch->csv_path);

    g_logger.channel_count++;

#ifdef _WIN32
    update_tray_tooltip();
#endif

    return ch;
}

/*============================================================================
 * Initialization
 *============================================================================*/

static bool init_socket(void) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[telem_logger] WSAStartup failed\n");
        return false;
    }
#endif

    g_logger.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_logger.sock == INVALID_SOCKET) {
        fprintf(stderr, "[telem_logger] Failed to create socket\n");
        return false;
    }

    /* Allow socket reuse */
    int reuse = 1;
    setsockopt(g_logger.sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    /* Bind to port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_logger.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_logger.sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[telem_logger] Failed to bind to port %d\n", g_logger.port);
        closesocket(g_logger.sock);
        return false;
    }

    /* Set receive timeout (100ms) for responsive tray updates */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  /* 100ms */
    setsockopt(g_logger.sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    printf("[telem_logger] Listening on UDP port %d\n", g_logger.port);
    return true;
}

static void cleanup(void) {
    /* Close all CSV files */
    for (int i = 0; i < g_logger.channel_count; i++) {
        if (g_logger.channels[i].csv_file) {
            /* Write footer */
            time_t now = time(NULL);
            fprintf(g_logger.channels[i].csv_file, "# Ended: %s", ctime(&now));
            fprintf(g_logger.channels[i].csv_file, "# Messages logged: %llu\n",
                    (unsigned long long)g_logger.channels[i].message_count);
            fclose(g_logger.channels[i].csv_file);
        }
    }

    /* Close socket */
    if (g_logger.sock != INVALID_SOCKET) {
        closesocket(g_logger.sock);
    }

#ifdef _WIN32
    cleanup_tray_icon();
    WSACleanup();
#endif

    /* Print summary */
    double elapsed = difftime(time(NULL), g_logger.start_time);
    printf("\n[telem_logger] Summary:\n");
    printf("  Total messages: %llu\n", (unsigned long long)g_logger.total_messages);
    printf("  Runtime: %.0f seconds\n", elapsed);
    printf("  Channels logged: %d\n", g_logger.channel_count);
    for (int i = 0; i < g_logger.channel_count; i++) {
        printf("    %s: %llu messages -> %s\n",
               g_logger.channels[i].name,
               (unsigned long long)g_logger.channels[i].message_count,
               g_logger.channels[i].csv_path);
    }
}

/*============================================================================
 * Main Loop
 *============================================================================*/

static void process_message(const char *message, size_t len) {
#ifdef _WIN32
    /* Skip if paused */
    if (g_paused) return;
#endif

    /* Extract channel prefix */
    char channel[MAX_CHANNEL_NAME];
    if (!extract_channel_prefix(message, channel, sizeof(channel))) {
        if (g_logger.verbose) {
            printf("[telem_logger] Unknown format: %.*s\n", (int)len, message);
        }
        return;
    }

    /* Check filter */
    if (is_channel_filtered(channel)) {
        return;
    }

    /* Find or create channel log */
    channel_log_t *ch = find_or_create_channel(channel);
    if (!ch) {
        return;
    }

    /* Write to CSV (the message IS the CSV line) */
    fprintf(ch->csv_file, "%.*s\n", (int)len, message);
    fflush(ch->csv_file);

    ch->message_count++;
    g_logger.total_messages++;

    /* Verbose output */
    if (g_logger.verbose) {
        printf("[%s] %.*s\n", channel, (int)len, message);
    }

    /* Periodic status update */
    if (g_logger.total_messages % 100 == 0) {
        if (g_logger.verbose) {
            printf("[telem_logger] %llu messages logged...\r",
                   (unsigned long long)g_logger.total_messages);
            fflush(stdout);
        }
#ifdef _WIN32
        update_tray_tooltip();
#endif
    }
}

static void run_listener(void) {
    char buffer[MAX_MESSAGE_LEN];
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);

    g_logger.running = true;
    g_logger.start_time = time(NULL);

#ifdef _WIN32
    if (g_tray_enabled) {
        printf("[telem_logger] Running in system tray. Right-click icon to exit.\n\n");
    } else {
        printf("[telem_logger] Waiting for telemetry data...\n");
        printf("[telem_logger] Press Ctrl+C to stop\n\n");
    }
#else
    printf("[telem_logger] Waiting for telemetry data...\n");
    printf("[telem_logger] Press Ctrl+C to stop\n\n");
#endif

    while (g_logger.running) {
#ifdef _WIN32
        /* Process tray messages */
        process_tray_messages();
#endif

        int recv_len = recvfrom(g_logger.sock, buffer, sizeof(buffer) - 1, 0,
                                (struct sockaddr*)&sender, &sender_len);

        if (recv_len == SOCKET_ERROR) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                continue;  /* Timeout, process tray messages */
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
#endif
            if (g_logger.running) {
                fprintf(stderr, "[telem_logger] Receive error\n");
            }
            break;
        }

        if (recv_len > 0) {
            /* Null-terminate and strip trailing newline */
            buffer[recv_len] = '\0';
            while (recv_len > 0 && (buffer[recv_len-1] == '\n' || buffer[recv_len-1] == '\r')) {
                buffer[--recv_len] = '\0';
            }

            if (recv_len > 0) {
                process_message(buffer, (size_t)recv_len);
            }
        }
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    /* Initialize defaults */
    g_logger.port = DEFAULT_PORT;
    g_logger.output_dir[0] = '\0';
    g_logger.verbose = false;
    g_logger.filter_enabled = false;
    g_logger.filter_count = 0;
    g_logger.channel_count = 0;
    g_logger.sock = INVALID_SOCKET;
    g_logger.total_messages = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_logger.port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            strncpy(g_logger.output_dir, argv[++i], sizeof(g_logger.output_dir) - 1);
            /* Remove trailing slash */
            size_t len = strlen(g_logger.output_dir);
            if (len > 0 && (g_logger.output_dir[len-1] == '/' || g_logger.output_dir[len-1] == '\\')) {
                g_logger.output_dir[len-1] = '\0';
            }
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            g_logger.filter_enabled = true;
            char *channels = argv[++i];
            char *token = strtok(channels, ",");
            while (token && g_logger.filter_count < MAX_CHANNELS) {
                strncpy(g_logger.filter_channels[g_logger.filter_count], token, MAX_CHANNEL_NAME - 1);
                g_logger.filter_channels[g_logger.filter_count][MAX_CHANNEL_NAME - 1] = '\0';
                g_logger.filter_count++;
                token = strtok(NULL, ",");
            }
        }
        else if (strcmp(argv[i], "-v") == 0) {
            g_logger.verbose = true;
        }
#ifdef _WIN32
        else if (strcmp(argv[i], "--no-tray") == 0) {
            g_tray_enabled = false;
        }
#endif
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Setup signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Print configuration */
    print_version("telem_logger");
    printf("============================\n");
    printf("Port: %d\n", g_logger.port);
    printf("Output: %s\n", g_logger.output_dir[0] ? g_logger.output_dir : "(current directory)");
    printf("Verbose: %s\n", g_logger.verbose ? "yes" : "no");
#ifdef _WIN32
    printf("System tray: %s\n", g_tray_enabled ? "yes" : "no");
#endif
    if (g_logger.filter_enabled) {
        printf("Filter: ");
        for (int i = 0; i < g_logger.filter_count; i++) {
            printf("%s%s", g_logger.filter_channels[i],
                   i < g_logger.filter_count - 1 ? "," : "");
        }
        printf("\n");
    } else {
        printf("Filter: (all channels)\n");
    }
    printf("\n");

#ifdef _WIN32
    /* Initialize system tray */
    if (g_tray_enabled && !init_tray_icon()) {
        fprintf(stderr, "[telem_logger] Warning: Failed to create tray icon, continuing without\n");
        g_tray_enabled = false;
    }
#endif

    /* Initialize socket */
    if (!init_socket()) {
        cleanup();
        return 1;
    }

    /* Run main loop */
    run_listener();

    /* Cleanup */
    cleanup();

    return 0;
}
