/**
 * @file signal_relay.c
 * @brief Simple TCP relay - pairs connections and pipes bytes
 *
 * Architecture:
 *   Three ports, each a dumb pipe:
 *   - Port 3001: Control (bidirectional, SDR commands)
 *   - Port 3002: Detector stream (splitter → client, 50kHz I/Q)
 *   - Port 3003: Display stream (splitter → client, 12kHz I/Q)
 *
 *   Each port accepts exactly two connections:
 *   1. signal_splitter (outbound from LAN, first to connect)
 *   2. Remote client (waterfall, detector, controller)
 *
 *   Relay just forwards bytes between the pair.
 *   Start in any order, auto-reconnects.
 *
 * Target Platform: Linux (DigitalOcean droplet)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/*============================================================================
 * Configuration
 *============================================================================*/

#define CONTROL_PORT    3001
#define DETECTOR_PORT   3002
#define DISPLAY_PORT    3003

#define BUFFER_SIZE     65536
#define STATUS_INTERVAL 10

/*============================================================================
 * Pipe - pairs two connections on one port
 *============================================================================*/

typedef struct {
    const char *name;
    int port;
    int listen_fd;
    int fd_a;           /* First connection (typically splitter) */
    int fd_b;           /* Second connection (typically client) */
    char ip_a[64];
    char ip_b[64];
    uint64_t bytes_a_to_b;
    uint64_t bytes_b_to_a;
} pipe_t;

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static pipe_t g_control;
static pipe_t g_detector;
static pipe_t g_display;
static time_t g_start_time;
static time_t g_last_status;

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\nShutdown requested\n");
    g_running = false;
}

/*============================================================================
 * Socket Helpers
 *============================================================================*/

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 2) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    set_nonblocking(fd);
    return fd;
}

/*============================================================================
 * Pipe Operations
 *============================================================================*/

static bool pipe_init(pipe_t *p, const char *name, int port) {
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->port = port;
    p->fd_a = -1;
    p->fd_b = -1;
    
    p->listen_fd = create_listen_socket(port);
    if (p->listen_fd < 0) {
        fprintf(stderr, "[%s] Failed to create listen socket on port %d\n", name, port);
        return false;
    }
    
    fprintf(stderr, "[%s] Listening on port %d\n", name, port);
    return true;
}

static void pipe_accept(pipe_t *p) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(p->listen_fd, (struct sockaddr*)&addr, &addr_len);
    
    if (fd < 0) return;
    
    set_nonblocking(fd);
    char *ip = inet_ntoa(addr.sin_addr);
    
    if (p->fd_a < 0) {
        p->fd_a = fd;
        strncpy(p->ip_a, ip, sizeof(p->ip_a) - 1);
        fprintf(stderr, "[%s] Connection A from %s\n", p->name, ip);
    } else if (p->fd_b < 0) {
        p->fd_b = fd;
        strncpy(p->ip_b, ip, sizeof(p->ip_b) - 1);
        fprintf(stderr, "[%s] Connection B from %s - pipe active\n", p->name, ip);
    } else {
        fprintf(stderr, "[%s] Rejecting connection from %s (pipe full)\n", p->name, ip);
        close(fd);
    }
}

static void pipe_close_fd(pipe_t *p, int which) {
    if (which == 0 && p->fd_a >= 0) {
        fprintf(stderr, "[%s] Connection A closed (%s)\n", p->name, p->ip_a);
        close(p->fd_a);
        p->fd_a = -1;
        p->ip_a[0] = '\0';
    } else if (which == 1 && p->fd_b >= 0) {
        fprintf(stderr, "[%s] Connection B closed (%s)\n", p->name, p->ip_b);
        close(p->fd_b);
        p->fd_b = -1;
        p->ip_b[0] = '\0';
    }
}

/* Forward data from one fd to the other */
static void pipe_forward(pipe_t *p, int from_fd, int to_fd, int from_which) {
    static uint8_t buffer[BUFFER_SIZE];
    
    ssize_t received = recv(from_fd, buffer, sizeof(buffer), 0);
    
    if (received <= 0) {
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        pipe_close_fd(p, from_which);
        return;
    }
    
    if (to_fd >= 0) {
        ssize_t sent = send(to_fd, buffer, received, MSG_NOSIGNAL);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            pipe_close_fd(p, from_which == 0 ? 1 : 0);
            return;
        }
        
        if (from_which == 0) {
            p->bytes_a_to_b += received;
        } else {
            p->bytes_b_to_a += received;
        }
    }
    /* If to_fd not connected, data is dropped */
}

static void pipe_cleanup(pipe_t *p) {
    if (p->fd_a >= 0) close(p->fd_a);
    if (p->fd_b >= 0) close(p->fd_b);
    if (p->listen_fd >= 0) close(p->listen_fd);
}

/*============================================================================
 * Status
 *============================================================================*/

static void print_status(void) {
    time_t now = time(NULL);
    if (now - g_last_status < STATUS_INTERVAL) return;
    g_last_status = now;
    
    int uptime = (int)(now - g_start_time);
    
    fprintf(stderr, "\n=== Signal Relay Status (uptime: %dd %dh %dm) ===\n",
            uptime / 86400, (uptime % 86400) / 3600, (uptime % 3600) / 60);
    
    fprintf(stderr, "[CONTROL]  port %d: A=%s B=%s (A→B: %lu, B→A: %lu)\n",
            g_control.port,
            g_control.fd_a >= 0 ? g_control.ip_a : "---",
            g_control.fd_b >= 0 ? g_control.ip_b : "---",
            g_control.bytes_a_to_b, g_control.bytes_b_to_a);
    
    fprintf(stderr, "[DETECTOR] port %d: A=%s B=%s (A→B: %lu bytes)\n",
            g_detector.port,
            g_detector.fd_a >= 0 ? g_detector.ip_a : "---",
            g_detector.fd_b >= 0 ? g_detector.ip_b : "---",
            g_detector.bytes_a_to_b);
    
    fprintf(stderr, "[DISPLAY]  port %d: A=%s B=%s (A→B: %lu bytes)\n",
            g_display.port,
            g_display.fd_a >= 0 ? g_display.ip_a : "---",
            g_display.fd_b >= 0 ? g_display.ip_b : "---",
            g_display.bytes_a_to_b);
    
    fprintf(stderr, "\n");
}

/*============================================================================
 * Main Loop
 *============================================================================*/

static void run(void) {
    fd_set readfds;
    struct timeval tv;
    
    while (g_running) {
        FD_ZERO(&readfds);
        int max_fd = 0;
        
        /* Add all listen sockets */
        FD_SET(g_control.listen_fd, &readfds);
        FD_SET(g_detector.listen_fd, &readfds);
        FD_SET(g_display.listen_fd, &readfds);
        max_fd = g_control.listen_fd;
        if (g_detector.listen_fd > max_fd) max_fd = g_detector.listen_fd;
        if (g_display.listen_fd > max_fd) max_fd = g_display.listen_fd;
        
        /* Add connected sockets */
        pipe_t *pipes[] = { &g_control, &g_detector, &g_display };
        for (int i = 0; i < 3; i++) {
            if (pipes[i]->fd_a >= 0) {
                FD_SET(pipes[i]->fd_a, &readfds);
                if (pipes[i]->fd_a > max_fd) max_fd = pipes[i]->fd_a;
            }
            if (pipes[i]->fd_b >= 0) {
                FD_SET(pipes[i]->fd_b, &readfds);
                if (pipes[i]->fd_b > max_fd) max_fd = pipes[i]->fd_b;
            }
        }
        
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ready = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        /* Accept new connections */
        if (FD_ISSET(g_control.listen_fd, &readfds)) pipe_accept(&g_control);
        if (FD_ISSET(g_detector.listen_fd, &readfds)) pipe_accept(&g_detector);
        if (FD_ISSET(g_display.listen_fd, &readfds)) pipe_accept(&g_display);
        
        /* Forward data on each pipe */
        for (int i = 0; i < 3; i++) {
            pipe_t *p = pipes[i];
            
            if (p->fd_a >= 0 && FD_ISSET(p->fd_a, &readfds)) {
                pipe_forward(p, p->fd_a, p->fd_b, 0);
            }
            if (p->fd_b >= 0 && FD_ISSET(p->fd_b, &readfds)) {
                pipe_forward(p, p->fd_b, p->fd_a, 1);
            }
        }
        
        print_status();
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("Signal Relay - Simple TCP Pipe Server\n");
    printf("Ports: Control=%d, Detector=%d, Display=%d\n\n",
           CONTROL_PORT, DETECTOR_PORT, DISPLAY_PORT);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    g_start_time = time(NULL);
    g_last_status = 0;
    
    if (!pipe_init(&g_control, "CONTROL", CONTROL_PORT)) return 1;
    if (!pipe_init(&g_detector, "DETECTOR", DETECTOR_PORT)) return 1;
    if (!pipe_init(&g_display, "DISPLAY", DISPLAY_PORT)) return 1;
    
    fprintf(stderr, "\nRelay ready. Waiting for connections...\n\n");
    
    run();
    
    pipe_cleanup(&g_control);
    pipe_cleanup(&g_detector);
    pipe_cleanup(&g_display);
    
    fprintf(stderr, "Relay shutdown complete\n");
    return 0;
}
