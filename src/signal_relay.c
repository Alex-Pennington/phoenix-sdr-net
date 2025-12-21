/**
 * @file signal_relay.c
 * @brief Relay server for detector (50kHz) and display (12kHz) streams
 *
 * Architecture:
 *   Listen on port 4410 (detector stream from signal_splitter)
 *   Listen on port 4411 (display stream from signal_splitter)
 *   Broadcast received data to multiple clients on each port
 *
 * Client Management:
 *   - Ring buffer per client (30 sec)
 *   - Drop slow clients that can't keep up
 *   - Continue broadcasting if splitter disconnects
 *   - Send stream header to new clients
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
 * Protocol Definitions (must match signal_splitter.c)
 *============================================================================*/

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

#define DETECTOR_PORT       4410
#define DISPLAY_PORT        4411
#define CONTROL_PORT        4409
#define MAX_CLIENTS         100
#define CLIENT_BUFFER_SIZE  (50000 * 30)  /* 30 sec @ 50kHz (worst case) */
#define STATUS_INTERVAL_SEC 5

/*============================================================================
 * Client Ring Buffer
 *============================================================================*/

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t write_idx;
    size_t read_idx;
    size_t count;
    uint64_t overflows;
    uint64_t bytes_sent;
} client_buffer_t;

static client_buffer_t* client_buffer_create(size_t capacity) {
    client_buffer_t *cb = (client_buffer_t*)malloc(sizeof(client_buffer_t));
    if (!cb) return NULL;

    cb->data = (uint8_t*)malloc(capacity);
    if (!cb->data) {
        free(cb);
        return NULL;
    }

    cb->capacity = capacity;
    cb->write_idx = 0;
    cb->read_idx = 0;
    cb->count = 0;
    cb->overflows = 0;
    cb->bytes_sent = 0;
    return cb;
}

static void client_buffer_destroy(client_buffer_t *cb) {
    if (cb) {
        free(cb->data);
        free(cb);
    }
}

static size_t client_buffer_write(client_buffer_t *cb, const uint8_t *data, size_t len) {
    size_t written = 0;

    while (written < len) {
        if (cb->count >= cb->capacity) {
            /* Overflow - discard oldest byte */
            cb->read_idx = (cb->read_idx + 1) % cb->capacity;
            cb->overflows++;
        } else {
            cb->count++;
        }

        cb->data[cb->write_idx] = data[written];
        cb->write_idx = (cb->write_idx + 1) % cb->capacity;
        written++;
    }

    return written;
}

static size_t client_buffer_read(client_buffer_t *cb, uint8_t *data, size_t len) {
    size_t to_read = (len < cb->count) ? len : cb->count;
    size_t read_count = 0;

    while (read_count < to_read) {
        data[read_count] = cb->data[cb->read_idx];
        cb->read_idx = (cb->read_idx + 1) % cb->capacity;
        read_count++;
    }

    cb->count -= read_count;
    cb->bytes_sent += read_count;
    return read_count;
}

/*============================================================================
 * Client Management
 *============================================================================*/

typedef struct {
    int fd;
    struct sockaddr_in addr;
    client_buffer_t *buffer;
    bool header_sent;
    time_t connected_time;
    uint64_t frames_sent;
} client_t;

typedef struct {
    client_t clients[MAX_CLIENTS];
    int count;
    relay_stream_header_t stream_header;
    uint64_t total_clients_served;
    uint64_t total_bytes_relayed;
    uint64_t total_frames_relayed;
} client_list_t;

static void client_list_init(client_list_t *list, uint32_t sample_rate) {
    memset(list, 0, sizeof(client_list_t));
    list->stream_header.magic = MAGIC_FT32;
    list->stream_header.sample_rate = sample_rate;
    list->stream_header.reserved1 = 0;
    list->stream_header.reserved2 = 0;
}

static int client_list_add(client_list_t *list, int fd, struct sockaddr_in *addr) {
    if (list->count >= MAX_CLIENTS) return -1;

    client_t *client = &list->clients[list->count];
    client->fd = fd;
    client->addr = *addr;
    client->buffer = client_buffer_create(CLIENT_BUFFER_SIZE);
    if (!client->buffer) {
        close(fd);
        return -1;
    }

    client->header_sent = false;
    client->connected_time = time(NULL);
    client->frames_sent = 0;

    list->count++;
    list->total_clients_served++;

    fprintf(stderr, "[CLIENT] New connection from %s:%d (total: %d)\n",
            inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), list->count);

    return list->count - 1;
}

static void client_list_remove(client_list_t *list, int idx) {
    if (idx < 0 || idx >= list->count) return;

    client_t *client = &list->clients[idx];

    fprintf(stderr, "[CLIENT] Disconnecting %s:%d (sent: %llu bytes, %llu frames)\n",
            inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port),
            (unsigned long long)client->buffer->bytes_sent,
            (unsigned long long)client->frames_sent);

    close(client->fd);
    client_buffer_destroy(client->buffer);

    /* Shift remaining clients */
    for (int i = idx; i < list->count - 1; i++) {
        list->clients[i] = list->clients[i + 1];
    }
    list->count--;
}

static void client_list_broadcast(client_list_t *list, const uint8_t *data, size_t len) {
    for (int i = 0; i < list->count; i++) {
        client_buffer_write(list->clients[i].buffer, data, len);
    }
    list->total_bytes_relayed += len;
}

static void client_list_send_pending(client_list_t *list) {
    for (int i = list->count - 1; i >= 0; i--) {
        client_t *client = &list->clients[i];

        /* Send header if not sent yet */
        if (!client->header_sent) {
            ssize_t sent = send(client->fd, &list->stream_header, sizeof(list->stream_header), MSG_NOSIGNAL);
            if (sent == sizeof(list->stream_header)) {
                client->header_sent = true;
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                client_list_remove(list, i);
                continue;
            }
        }

        /* Send buffered data */
        if (client->buffer->count > 0) {
            uint8_t chunk[8192];
            size_t to_send = (client->buffer->count < sizeof(chunk)) ? client->buffer->count : sizeof(chunk);
            size_t read_count = client_buffer_read(client->buffer, chunk, to_send);

            ssize_t sent = send(client->fd, chunk, read_count, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    client_list_remove(list, i);
                    continue;
                } else {
                    /* Put data back in buffer */
                    client_buffer_write(client->buffer, chunk, read_count);
                }
            } else if (sent < (ssize_t)read_count) {
                /* Partial send - put remainder back */
                client_buffer_write(client->buffer, chunk + sent, read_count - sent);
            }
        }
    }
}

/*============================================================================
 * Global State
 *============================================================================*/

static volatile bool g_running = true;
static int g_detector_listen_fd = -1;
static int g_display_listen_fd = -1;
static int g_control_listen_fd = -1;
static int g_detector_source_fd = -1;
static int g_display_source_fd = -1;
static int g_control_source_fd = -1;  /* signal_splitter connection */
static int g_control_client_fd = -1;  /* remote client connection */
static client_list_t g_detector_clients;
static client_list_t g_display_clients;
static client_list_t g_display_clients;
static time_t g_start_time;
static time_t g_last_status_time;

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n[SHUTDOWN] Received signal, shutting down...\n");
    g_running = false;
}

/*============================================================================
 * Socket Helpers
 *============================================================================*/

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Set reuse address */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* Listen */
    if (listen(fd, 10) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    fprintf(stderr, "[LISTEN] Port %d ready\n", port);
    return fd;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*============================================================================
 * Stream Source Handling
 *============================================================================*/

static bool handle_source_connection(int *source_fd, int listen_fd, const char *stream_name) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept(listen_fd, (struct sockaddr*)&addr, &addr_len);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
        return false;
    }

    /* If we already have a source, close the old one */
    if (*source_fd >= 0) {
        fprintf(stderr, "[SOURCE-%s] Replacing connection from %s:%d\n",
                stream_name, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        close(*source_fd);
    } else {
        fprintf(stderr, "[SOURCE-%s] New connection from %s:%d\n",
                stream_name, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    }

    set_nonblocking(fd);
    *source_fd = fd;
    return true;
}

static bool receive_and_relay(int source_fd, client_list_t *clients, const char *stream_name) {
    uint8_t buffer[65536];
    ssize_t received = recv(source_fd, buffer, sizeof(buffer), 0);

    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[SOURCE-%s] Connection lost\n", stream_name);
            return false;
        }
        return true;  /* No data available */
    }

    if (received == 0) {
        fprintf(stderr, "[SOURCE-%s] Connection closed\n", stream_name);
        return false;
    }

    /* Broadcast to all clients */
    client_list_broadcast(clients, buffer, received);

    return true;
}

/*============================================================================
 * Control Path (Bidirectional Text Forwarding)
 *============================================================================*/

static void forward_control_data(int from_fd, int to_fd, const char *label) {
    char buffer[4096];
    ssize_t received = recv(from_fd, buffer, sizeof(buffer), 0);

    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[CTRL-%s] Connection lost\n", label);
            if (from_fd == g_control_source_fd || to_fd == g_control_source_fd) {
                if (g_control_source_fd >= 0) close(g_control_source_fd);
                g_control_source_fd = -1;
            }
            if (from_fd == g_control_client_fd || to_fd == g_control_client_fd) {
                if (g_control_client_fd >= 0) close(g_control_client_fd);
                g_control_client_fd = -1;
            }
        }
        return;
    }

    if (received == 0) {
        fprintf(stderr, "[CTRL-%s] Connection closed\n", label);
        if (from_fd == g_control_source_fd || to_fd == g_control_source_fd) {
            if (g_control_source_fd >= 0) close(g_control_source_fd);
            g_control_source_fd = -1;
        }
        if (from_fd == g_control_client_fd || to_fd == g_control_client_fd) {
            if (g_control_client_fd >= 0) close(g_control_client_fd);
            g_control_client_fd = -1;
        }
        return;
    }

    /* Forward to destination */
    ssize_t sent = send(to_fd, buffer, received, MSG_NOSIGNAL);
    if (sent < 0) {
        fprintf(stderr, "[CTRL-%s] Forward failed\n", label);
    }
}

/*============================================================================
 * Status Reporting
 *============================================================================*/

static void print_status(void) {
    time_t now = time(NULL);
    if (now - g_last_status_time < STATUS_INTERVAL_SEC) return;
    g_last_status_time = now;

    time_t uptime = now - g_start_time;

    fprintf(stderr, "\n[STATUS] Uptime: %ld sec\n", (long)uptime);

    fprintf(stderr, "[STATUS] Detector: source=%s clients=%d (total_served=%llu)\n",
            g_detector_source_fd >= 0 ? "UP" : "DOWN",
            g_detector_clients.count,
            (unsigned long long)g_detector_clients.total_clients_served);

    fprintf(stderr, "[STATUS]   Relayed: %llu bytes, %llu frames\n",
            (unsigned long long)g_detector_clients.total_bytes_relayed,
            (unsigned long long)g_detector_clients.total_frames_relayed);

    fprintf(stderr, "[STATUS] Display: source=%s clients=%d (total_served=%llu)\n",
            g_display_source_fd >= 0 ? "UP" : "DOWN",
            g_display_clients.count,
            (unsigned long long)g_display_clients.total_clients_served);

    fprintf(stderr, "[STATUS]   Relayed: %llu bytes, %llu frames\n",
            (unsigned long long)g_display_clients.total_bytes_relayed,
            (unsigned long long)g_display_clients.total_frames_relayed);

    fprintf(stderr, "[STATUS] Control: source=%s client=%s\n",
            g_control_source_fd >= 0 ? "UP" : "DOWN",
            g_control_client_fd >= 0 ? "CONNECTED" : "---");
}

/*============================================================================
 * Main Loop
 *============================================================================*/

static void run(void) {
    fd_set readfds;
    struct timeval tv;

    while (g_running) {
        FD_ZERO(&readfds);
        int max_fd = -1;

        /* Listen sockets */
        FD_SET(g_detector_listen_fd, &readfds);
        FD_SET(g_display_listen_fd, &readfds);
        FD_SET(g_control_listen_fd, &readfds);
        max_fd = g_detector_listen_fd;
        if (g_display_listen_fd > max_fd) max_fd = g_display_listen_fd;
        if (g_control_listen_fd > max_fd) max_fd = g_control_listen_fd;

        /* Source sockets */
        if (g_detector_source_fd >= 0) {
            FD_SET(g_detector_source_fd, &readfds);
            if (g_detector_source_fd > max_fd) max_fd = g_detector_source_fd;
        }
        if (g_display_source_fd >= 0) {
            FD_SET(g_display_source_fd, &readfds);
            if (g_display_source_fd > max_fd) max_fd = g_display_source_fd;
        }
        if (g_control_source_fd >= 0) {
            FD_SET(g_control_source_fd, &readfds);
            if (g_control_source_fd > max_fd) max_fd = g_control_source_fd;
        }
        if (g_control_client_fd >= 0) {
            FD_SET(g_control_client_fd, &readfds);
            if (g_control_client_fd > max_fd) max_fd = g_control_client_fd;
        }

        /* Select with 100ms timeout */
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ready = select(max_fd + 1, &readfds, NULL, NULL, &tv);

        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Accept new source connections */
        if (FD_ISSET(g_detector_listen_fd, &readfds)) {
            handle_source_connection(&g_detector_source_fd, g_detector_listen_fd, "DETECTOR");
        }
        if (FD_ISSET(g_display_listen_fd, &readfds)) {
            handle_source_connection(&g_display_source_fd, g_display_listen_fd, "DISPLAY");
        }

        /* Accept control connections */
        if (FD_ISSET(g_control_listen_fd, &readfds)) {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            int fd = accept(g_control_listen_fd, (struct sockaddr*)&addr, &addr_len);

            if (fd >= 0) {
                /* Source = signal_splitter (single connection) */
                if (g_control_source_fd < 0) {
                    set_nonblocking(fd);
                    g_control_source_fd = fd;
                    fprintf(stderr, "[CTRL-SOURCE] Connected from %s:%d\n",
                            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                }
                /* Client = remote user (single connection, reject if occupied) */
                else if (g_control_client_fd < 0) {
                    set_nonblocking(fd);
                    g_control_client_fd = fd;
                    fprintf(stderr, "[CTRL-CLIENT] Connected from %s:%d\n",
                            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                } else {
                    fprintf(stderr, "[CTRL] Rejecting connection (already occupied)\n");
                    close(fd);
                }
            }
        }

        /* Receive from sources and relay */
        if (g_detector_source_fd >= 0 && FD_ISSET(g_detector_source_fd, &readfds)) {
            if (!receive_and_relay(g_detector_source_fd, &g_detector_clients, "DETECTOR")) {
                close(g_detector_source_fd);
                g_detector_source_fd = -1;
            }
        }
        if (g_display_source_fd >= 0 && FD_ISSET(g_display_source_fd, &readfds)) {
            if (!receive_and_relay(g_display_source_fd, &g_display_clients, "DISPLAY")) {
                close(g_display_source_fd);
                g_display_source_fd = -1;
            }
        }

        /* Forward control data bidirectionally */
        if (g_control_source_fd >= 0 && g_control_client_fd >= 0) {
            /* Client → Source (commands from remote user to SDR) */
            if (FD_ISSET(g_control_client_fd, &readfds)) {
                forward_control_data(g_control_client_fd, g_control_source_fd,
                                     &g_control_client_fd, "CLIENT→SOURCE");
            }
            /* Source → Client (responses from SDR to remote user) */
            if (FD_ISSET(g_control_source_fd, &readfds)) {
                forward_control_data(g_control_source_fd, g_control_client_fd,
                                     &g_control_source_fd, "SOURCE→CLIENT");
            }
        }

        /* Send pending data to clients */
        client_list_send_pending(&g_detector_clients);
        client_list_send_pending(&g_display_clients);

        /* Status reporting */
        print_status();
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Phoenix SDR Signal Relay\n");
    printf("Detector stream: port %d (50 kHz float32 I/Q)\n", DETECTOR_PORT);
    printf("Display stream:  port %d (12 kHz float32 I/Q)\n", DISPLAY_PORT);
    printf("Control relay:   port %d (text commands)\n\n", CONTROL_PORT);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  /* Ignore broken pipe */

    /* Initialize client lists */
    client_list_init(&g_detector_clients, 50000);
    client_list_init(&g_display_clients, 12000);

    /* Create listen sockets */
    g_detector_listen_fd = create_listen_socket(DETECTOR_PORT);
    g_display_listen_fd = create_listen_socket(DISPLAY_PORT);
    g_control_listen_fd = create_listen_socket(CONTROL_PORT);

    if (g_detector_listen_fd < 0 || g_display_listen_fd < 0 || g_control_listen_fd < 0) {
        fprintf(stderr, "Failed to create listen sockets\n");
        return 1;
    }

    set_nonblocking(g_detector_listen_fd);
    set_nonblocking(g_display_listen_fd);
    set_nonblocking(g_control_listen_fd);

    g_start_time = time(NULL);
    g_last_status_time = g_start_time;

    fprintf(stderr, "[STARTUP] Ready to relay signals\n\n");

    /* Main loop */
    run();

    /* Cleanup */
    fprintf(stderr, "\n[SHUTDOWN] Closing all connections...\n");

    if (g_detector_source_fd >= 0) close(g_detector_source_fd);
    if (g_display_source_fd >= 0) close(g_display_source_fd);
    if (g_control_source_fd >= 0) close(g_control_source_fd);
    if (g_control_client_fd >= 0) close(g_control_client_fd);

    close(g_detector_listen_fd);
    close(g_display_listen_fd);
    close(g_control_listen_fd);

    for (int i = 0; i < g_detector_clients.count; i++) {
        close(g_detector_clients.clients[i].fd);
        client_buffer_destroy(g_detector_clients.clients[i].buffer);
    }
    for (int i = 0; i < g_display_clients.count; i++) {
        close(g_display_clients.clients[i].fd);
        client_buffer_destroy(g_display_clients.clients[i].buffer);
    }

    fprintf(stderr, "[SHUTDOWN] Done.\n");
    return 0;
}
