/**
 * @file sdr_controller.c
 * @brief Simple SDR Controller for Phoenix Nest Network Stack Testing
 *
 * Finds sdr_server via UDP discovery (or direct IP) and sends START command.
 * Used to initiate I/Q streaming for network stack testing.
 *
 * Usage:
 *   sdr_controller.exe                    # Auto-discover sdr_server via UDP
 *   sdr_controller.exe -H 192.168.1.153   # Direct connect to specific IP
 *   sdr_controller.exe -f 15000000        # Set frequency (Hz) before START
 *   sdr_controller.exe -g 40              # Set gain reduction before START
 *
 * (c) 2024 Phoenix Nest LLC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define sleep_ms(ms) Sleep(ms)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#include "pn_discovery.h"
#include "version.h"

/*============================================================================
 * Defaults
 *============================================================================*/

#define DEFAULT_CTRL_PORT   4535
#define DEFAULT_FREQ_HZ     15000000    /* 15 MHz (WWV) */
#define DEFAULT_GAIN        40
#define MAX_LINE            256
#define DISCOVERY_WAIT_SEC  300  /* 5 minutes - keep waiting */

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static volatile bool g_service_found = false;
static pn_service_t g_found_service;

/* Configuration */
static char g_host[64] = "";
static int g_port = DEFAULT_CTRL_PORT;
static double g_freq_hz = DEFAULT_FREQ_HZ;
static int g_gain = DEFAULT_GAIN;
static bool g_discovery_enabled = true;
static bool g_interactive = false;

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nInterrupted.\n");
    g_running = false;
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

static int send_command(SOCKET sock, const char *cmd, char *response, int resp_size) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "%s\n", cmd);
    
    printf(">> %s", buf);
    
    if (send_line(sock, buf) < 0) {
        fprintf(stderr, "Send failed\n");
        return -1;
    }
    
    if (recv_line(sock, response, resp_size) < 0) {
        fprintf(stderr, "Receive failed\n");
        return -1;
    }
    
    printf("<< %s\n", response);
    
    return (strncmp(response, "OK", 2) == 0) ? 0 : -1;
}

/*============================================================================
 * Discovery Callback
 *============================================================================*/

static void on_service_discovered(const char *id, const char *service,
                                  const char *ip, int ctrl_port, int data_port,
                                  const char *caps, bool is_bye, void *userdata) {
    (void)userdata;
    (void)data_port;
    (void)caps;
    
    if (is_bye) {
        printf("[Discovery] Service '%s' (%s) left the network\n", id, service);
        return;
    }
    
    printf("[Discovery] Found: %s (%s) at %s:%d\n", id, service, ip, ctrl_port);
    
    /* Look for sdr_server only (not signal_splitter - that's a data forwarder, not control) */
    if (strcmp(service, PN_SVC_SDR_SERVER) == 0) {
        memset(&g_found_service, 0, sizeof(g_found_service));
        strncpy(g_found_service.id, id, sizeof(g_found_service.id) - 1);
        strncpy(g_found_service.service, service, sizeof(g_found_service.service) - 1);
        strncpy(g_found_service.ip, ip, sizeof(g_found_service.ip) - 1);
        g_found_service.ctrl_port = ctrl_port;
        g_service_found = true;
    }
}

/*============================================================================
 * Usage
 *============================================================================*/

static void print_usage(const char *prog) {
    printf("SDR Controller - Phoenix Nest Network Stack\n");
    printf("Version: %s\n\n", PHOENIX_VERSION_STRING);
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -H HOST     Connect directly to HOST (skip discovery)\n");
    printf("  -p PORT     Control port (default: %d)\n", DEFAULT_CTRL_PORT);
    printf("  -f FREQ     Set frequency in Hz (default: %.0f)\n", (double)DEFAULT_FREQ_HZ);
    printf("  -g GAIN     Set gain reduction in dB (default: %d)\n", DEFAULT_GAIN);
    printf("  -i          Interactive mode (stay connected for commands)\n");
    printf("  -h          Show this help\n\n");
    printf("Examples:\n");
    printf("  %s                       # Auto-discover and start streaming\n", prog);
    printf("  %s -H 192.168.1.153      # Direct connect to specific IP\n", prog);
    printf("  %s -f 10000000 -g 35     # Set 10 MHz, gain 35dB before start\n", prog);
}

/*============================================================================
 * Interactive Mode
 *============================================================================*/

static void interactive_loop(SOCKET sock) {
    char cmd[MAX_LINE];
    char response[MAX_LINE];
    
    printf("\n=== Interactive Mode ===\n");
    printf("Type commands (e.g., STATUS, STOP, SET_FREQ 10000000)\n");
    printf("Type 'quit' or 'exit' to disconnect\n\n");
    
    while (g_running) {
        printf("cmd> ");
        fflush(stdout);
        
        if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
            break;
        }
        
        /* Trim newline */
        char *nl = strchr(cmd, '\n');
        if (nl) *nl = '\0';
        
        /* Check for exit */
        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            send_command(sock, "QUIT", response, sizeof(response));
            break;
        }
        
        if (strlen(cmd) == 0) {
            continue;
        }
        
        send_command(sock, cmd, response, sizeof(response));
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    SOCKET sock = INVALID_SOCKET;
    char response[MAX_LINE];
    int ret = 1;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            strncpy(g_host, argv[++i], sizeof(g_host) - 1);
            g_discovery_enabled = false;
        }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            g_freq_hz = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            g_gain = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-i") == 0) {
            g_interactive = true;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    /* Install signal handler */
    signal(SIGINT, signal_handler);
    
    /* Initialize sockets */
    if (socket_init() != 0) {
        fprintf(stderr, "Failed to initialize sockets\n");
        return 1;
    }
    
    printf("======================================\n");
    printf("Phoenix SDR Controller v%s\n", PHOENIX_VERSION_STRING);
    printf("======================================\n\n");
    
    /* Find sdr_server via discovery or use direct connection */
    if (g_discovery_enabled) {
        printf("Initializing service discovery...\n");
        if (pn_discovery_init(0) != 0) {
            fprintf(stderr, "Failed to initialize discovery\n");
            socket_cleanup();
            return 1;
        }
        
        /* Announce ourselves so other services know to re-announce */
        pn_announce("CONTROLLER-1", PN_SVC_CONTROLLER, 0, 0, NULL);
        
        /* Start listening */
        if (pn_listen(on_service_discovered, NULL) != 0) {
            fprintf(stderr, "Failed to start discovery listener\n");
            pn_discovery_shutdown();
            socket_cleanup();
            return 1;
        }
        
        /* Wait for service - keep waiting until found or Ctrl+C */
        printf("Waiting for sdr_server... (Ctrl+C to quit)\n");
        int wait_count = 0;
        while (!g_service_found && g_running) {
            sleep_ms(100);
            wait_count++;
            if (wait_count % 100 == 0) {  /* Every 10 seconds */
                printf("  Still waiting... (%d sec)\n", wait_count / 10);
            }
        }
        
        if (!g_service_found) {
            printf("Discovery cancelled.\n");
            pn_discovery_shutdown();
            socket_cleanup();
            return 1;
        }
        
        strncpy(g_host, g_found_service.ip, sizeof(g_host) - 1);
        g_port = g_found_service.ctrl_port;
        printf("\nUsing discovered service: %s at %s:%d\n",
               g_found_service.id, g_host, g_port);
    } else {
        printf("Direct connection mode: %s:%d\n", g_host, g_port);
    }
    
    /* Connect to sdr_server */
    printf("\nConnecting to %s:%d...\n", g_host, g_port);
    
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "Failed to create socket\n");
        goto cleanup;
    }
    
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(g_port);
    server.sin_addr.s_addr = inet_addr(g_host);
    
    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", g_host, g_port);
        goto cleanup;
    }
    
    printf("Connected!\n\n");
    
    /* Get version */
    if (send_command(sock, "VER", response, sizeof(response)) == 0) {
        /* OK */
    }
    
    /* Get current status */
    send_command(sock, "STATUS", response, sizeof(response));
    
    /* Configure frequency */
    {
        char cmd[MAX_LINE];
        snprintf(cmd, sizeof(cmd), "SET_FREQ %.0f", g_freq_hz);
        if (send_command(sock, cmd, response, sizeof(response)) != 0) {
            fprintf(stderr, "Failed to set frequency\n");
            /* Continue anyway */
        }
    }
    
    /* Configure gain */
    {
        char cmd[MAX_LINE];
        snprintf(cmd, sizeof(cmd), "SET_GAIN %d", g_gain);
        if (send_command(sock, cmd, response, sizeof(response)) != 0) {
            fprintf(stderr, "Failed to set gain\n");
            /* Continue anyway */
        }
    }
    
    /* Start streaming! */
    printf("\n=== Starting I/Q Streaming ===\n");
    if (send_command(sock, "START", response, sizeof(response)) != 0) {
        fprintf(stderr, "START command failed: %s\n", response);
        goto cleanup;
    }
    
    printf("\n*** SDR streaming is now active! ***\n");
    printf("I/Q data flowing from sdr_server -> signal_splitter -> relay\n\n");
    
    if (g_interactive) {
        interactive_loop(sock);
    } else {
        /* Just verify and exit */
        send_command(sock, "STATUS", response, sizeof(response));
        printf("\nController task complete. SDR will continue streaming.\n");
        printf("Use -i for interactive mode, or run again with STOP to stop.\n");
    }
    
    ret = 0;
    
cleanup:
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    
    if (g_discovery_enabled) {
        pn_discovery_shutdown();
    }
    
    socket_cleanup();
    return ret;
}
