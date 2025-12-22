/**
 * @file signal_relay.c
 * @brief Relay server for detector (50kHz) and display (12kHz) streams
 *
 * Architecture:
 *   Listen on port 4410 (detector stream from signal_splitter)
 *   Listen on port 4411 (display stream from signal_splitter)
 *   Listen on port 5401 (discovery coordinator - TCP registry)
 *   Broadcast received data to multiple clients on each port
 *
 * Client Management:
 *   - Ring buffer per client (30 sec)
 *   - Drop slow clients that can't keep up
 *   - Continue broadcasting if splitter disconnects
 *   - Send stream header to new clients
 *
 * Discovery Coordinator (TCP):
 *   - Edge nodes (signal_splitters) connect via TCP
 *   - Register their services in central registry
 *   - Query available services across all edge nodes
 *   - Hub-and-spoke topology for NAT traversal
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

#define RENDEZVOUS_PORT     3000        /* Initial handshake port */
#define SPLITTER_PORT_BASE  3001        /* First assigned splitter port */
#define SPLITTER_PORT_MAX   3100        /* Last assigned splitter port */
#define DETECTOR_PORT       4410
#define DISPLAY_PORT        4411
#define DISCOVERY_PORT      5401        /* TCP discovery coordinator */
#define MAX_SPLITTERS       32          /* Max signal_splitters connected */
#define MAX_CLIENTS         100
#define MAX_EDGE_NODES      32          /* Max signal_splitters connected */
#define MAX_SERVICES        128         /* Total services across all edges */
#define CLIENT_BUFFER_SIZE  (50000 * 30)  /* 30 sec @ 50kHz (worst case) */
#define STATUS_INTERVAL_SEC 5
#define EDGE_TIMEOUT_SEC    120         /* Remove edge if no heartbeat */

/* Control channel message types */
#define MSG_TYPE_RELAY  'r'     /* Relay command */
#define MSG_TYPE_SDR    's'     /* SDR passthrough */

/*============================================================================
 * Discovery Protocol (JSON over TCP, newline-delimited)
 *============================================================================*/

/* Message types matching pn_discovery.h */
#define DISC_CMD_HELO   "helo"      /* Edge node announces service */
#define DISC_CMD_BYE    "bye"       /* Edge node removes service */
#define DISC_CMD_LIST   "list"      /* Query all services */
#define DISC_CMD_FIND   "find"      /* Find specific service type */

/* Edge node tracking */
typedef struct {
    int fd;                         /* TCP socket */
    char ip[64];                    /* Edge node's public IP */
    time_t last_seen;               /* Last message timestamp */
    int service_count;              /* Number of services registered */
} edge_node_t;

/* Service registry entry */
typedef struct {
    char id[64];                    /* e.g., "KY4OLB-SDR1" */
    char service[32];               /* e.g., "sdr_server" */
    char ip[64];                    /* Edge node's IP */
    int ctrl_port;
    int data_port;
    char caps[128];                 /* Capabilities */
    int edge_idx;                   /* Which edge_node owns this */
    time_t registered;
} service_entry_t;

/* Splitter connection (control channel) */
typedef struct {
    int ctrl_fd;                    /* Control channel socket */
    int listen_fd;                  /* Listening socket for control */
    int assigned_port;              /* Control port assigned */
    int client_fd;                  /* Remote client connected for control */
    int det_listen_fd;              /* Listening socket for detector stream */
    int det_source_fd;              /* Splitter detector stream connection */
    int det_port;                   /* Detector port assigned */
    int disp_listen_fd;             /* Listening socket for display stream */
    int disp_source_fd;             /* Splitter display stream connection */
    int disp_port;                  /* Display port assigned */
    char node_id[64];               /* Splitter's node ID */
    char ip[64];                    /* Splitter's IP */
    time_t last_seen;               /* Last message timestamp */
    char recv_buf[8192];            /* Buffer for partial JSON messages */
    int recv_len;
    bool active;                    /* Slot in use */
    bool has_sdr;                   /* Splitter has SDR connected */
} splitter_conn_t;

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
static int g_rendezvous_listen_fd = -1;    /* Port 3000 for handshakes */
static int g_discovery_listen_fd = -1;
static int g_detector_source_fd = -1;
static int g_display_source_fd = -1;
static client_list_t g_detector_clients;
static client_list_t g_display_clients;
static time_t g_start_time;
static time_t g_last_status_time;

/* Splitter connections */
static splitter_conn_t g_splitters[MAX_SPLITTERS];
static int g_splitter_count = 0;
static int g_next_splitter_port = SPLITTER_PORT_BASE;

/* Discovery coordinator state */
static edge_node_t g_edge_nodes[MAX_EDGE_NODES];
static int g_edge_count = 0;
static service_entry_t g_services[MAX_SERVICES];
static int g_service_count = 0;

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n[SHUTDOWN] Received signal, shutting down...\n");
    g_running = false;
}

/*============================================================================
 * JSON Helpers
 *============================================================================*/

/* Simple JSON string extractor: finds "key":"value" and copies value */
static int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return -1;
    
    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return (int)len;
}

/* Simple JSON int extractor: finds "key":123 and returns value */
static int json_get_int(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    
    start += strlen(pattern);
    return atoi(start);
}

/* Unescape JSON string in-place, returns new length */
static int json_unescape(char *str, int len) {
    int r = 0, w = 0;
    while (r < len) {
        if (str[r] == '\\' && r + 1 < len) {
            r++;
            switch (str[r]) {
                case 'n': str[w++] = '\n'; break;
                case 'r': str[w++] = '\r'; break;
                case 't': str[w++] = '\t'; break;
                case '"': str[w++] = '"'; break;
                case '\\': str[w++] = '\\'; break;
                default: str[w++] = str[r]; break;
            }
            r++;
        } else {
            str[w++] = str[r++];
        }
    }
    str[w] = '\0';
    return w;
}

/*============================================================================
 * Splitter Management
 *============================================================================*/

static int allocate_splitter_port(void) {
    int port = g_next_splitter_port++;
    if (g_next_splitter_port > SPLITTER_PORT_MAX) {
        g_next_splitter_port = SPLITTER_PORT_BASE;
    }
    return port;
}

static splitter_conn_t* find_free_splitter_slot(void) {
    for (int i = 0; i < MAX_SPLITTERS; i++) {
        if (!g_splitters[i].active) {
            return &g_splitters[i];
        }
    }
    return NULL;
}

static void close_splitter(splitter_conn_t *sp) {
    if (!sp || !sp->active) return;
    
    fprintf(stderr, "[SPLITTER] Closing %s (port %d)\n", sp->node_id, sp->assigned_port);
    
    if (sp->ctrl_fd >= 0) close(sp->ctrl_fd);
    if (sp->client_fd >= 0) close(sp->client_fd);
    if (sp->listen_fd >= 0) close(sp->listen_fd);
    if (sp->det_listen_fd >= 0) close(sp->det_listen_fd);
    if (sp->det_source_fd >= 0) close(sp->det_source_fd);
    if (sp->disp_listen_fd >= 0) close(sp->disp_listen_fd);
    if (sp->disp_source_fd >= 0) close(sp->disp_source_fd);
    
    memset(sp, 0, sizeof(*sp));
    sp->ctrl_fd = -1;
    sp->client_fd = -1;
    sp->listen_fd = -1;
    sp->det_listen_fd = -1;
    sp->det_source_fd = -1;
    sp->disp_listen_fd = -1;
    sp->disp_source_fd = -1;
    sp->active = false;
    g_splitter_count--;
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
 * Splitter Control Channel
 *============================================================================*/

/* Handle rendezvous connection - assign port and send to splitter */
static void handle_rendezvous_accept(void) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(g_rendezvous_listen_fd, (struct sockaddr*)&addr, &addr_len);
    
    if (fd < 0) return;
    
    char ip_str[64];
    strncpy(ip_str, inet_ntoa(addr.sin_addr), sizeof(ip_str) - 1);
    
    fprintf(stderr, "[RENDEZVOUS] Connection from %s\n", ip_str);
    
    /* Read hello message (blocking, short timeout) */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    char buf[512];
    ssize_t received = recv(fd, buf, sizeof(buf) - 1, 0);
    if (received <= 0) {
        fprintf(stderr, "[RENDEZVOUS] No hello received\n");
        close(fd);
        return;
    }
    buf[received] = '\0';
    
    /* Parse hello: {"t":"r","c":"hello","id":"SPLITTER-1"} */
    char cmd[32], node_id[64];
    if (json_get_string(buf, "c", cmd, sizeof(cmd)) < 0 || strcmp(cmd, "hello") != 0) {
        fprintf(stderr, "[RENDEZVOUS] Expected 'hello', got: %s\n", buf);
        close(fd);
        return;
    }
    if (json_get_string(buf, "id", node_id, sizeof(node_id)) < 0) {
        strncpy(node_id, "UNKNOWN", sizeof(node_id));
    }
    
    /* Find a free splitter slot */
    splitter_conn_t *sp = find_free_splitter_slot();
    if (!sp) {
        fprintf(stderr, "[RENDEZVOUS] No free splitter slots\n");
        close(fd);
        return;
    }
    
    /* Allocate port and create listen socket */
    int assigned_port = allocate_splitter_port();
    int listen_fd = create_listen_socket(assigned_port);
    if (listen_fd < 0) {
        fprintf(stderr, "[RENDEZVOUS] Failed to create listen socket on port %d\n", assigned_port);
        close(fd);
        return;
    }
    set_nonblocking(listen_fd);
    
    /* Send port assignment */
    char response[128];
    int resp_len = snprintf(response, sizeof(response), 
                            "{\"t\":\"r\",\"c\":\"assign\",\"p\":%d}\n", assigned_port);
    if (send(fd, response, resp_len, 0) < 0) {
        fprintf(stderr, "[RENDEZVOUS] Failed to send assignment\n");
        close(fd);
        close(listen_fd);
        return;
    }
    
    /* Close rendezvous connection (splitter will reconnect to assigned port) */
    close(fd);
    
    /* Initialize splitter slot */
    memset(sp, 0, sizeof(*sp));
    sp->active = true;
    sp->assigned_port = assigned_port;
    sp->listen_fd = listen_fd;
    sp->ctrl_fd = -1;
    sp->client_fd = -1;
    strncpy(sp->node_id, node_id, sizeof(sp->node_id) - 1);
    strncpy(sp->ip, ip_str, sizeof(sp->ip) - 1);
    sp->last_seen = time(NULL);
    sp->recv_len = 0;
    g_splitter_count++;
    
    fprintf(stderr, "[RENDEZVOUS] Assigned port %d to %s, waiting for reconnect\n", 
            assigned_port, node_id);
}

/* Accept splitter connection on assigned port */
static void handle_splitter_accept(splitter_conn_t *sp) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(sp->listen_fd, (struct sockaddr*)&addr, &addr_len);
    
    if (fd < 0) return;
    
    set_nonblocking(fd);
    
    if (sp->ctrl_fd < 0) {
        /* First connection is the splitter itself */
        sp->ctrl_fd = fd;
        sp->last_seen = time(NULL);
        fprintf(stderr, "[SPLITTER] %s connected on port %d\n", sp->node_id, sp->assigned_port);
    } else if (sp->client_fd < 0) {
        /* Second connection is a remote client */
        sp->client_fd = fd;
        fprintf(stderr, "[SPLITTER] Client connected to %s\n", sp->node_id);
    } else {
        /* No more slots */
        fprintf(stderr, "[SPLITTER] Rejecting extra connection to %s\n", sp->node_id);
        close(fd);
    }
}

/* Send SDR passthrough wrapped in JSON */
static bool splitter_send_sdr(splitter_conn_t *sp, const char *data, int len) {
    char buf[8192];
    int pos = 0;
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"t\":\"s\",\"d\":\"");
    
    /* Escape the data */
    for (int i = 0; i < len && pos < (int)sizeof(buf) - 10; i++) {
        char c = data[i];
        if (c == '"') { buf[pos++] = '\\'; buf[pos++] = '"'; }
        else if (c == '\\') { buf[pos++] = '\\'; buf[pos++] = '\\'; }
        else if (c == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; }
        else if (c == '\r') { buf[pos++] = '\\'; buf[pos++] = 'r'; }
        else if (c >= 32 && c < 127) { buf[pos++] = c; }
    }
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"}\n");
    return send(sp->ctrl_fd, buf, pos, MSG_NOSIGNAL) == pos;
}

/* Process a single JSON message from splitter */
static void process_splitter_message(splitter_conn_t *sp, const char *msg) {
    char type[8];
    if (json_get_string(msg, "t", type, sizeof(type)) < 0) return;
    
    if (type[0] == MSG_TYPE_RELAY) {
        /* Relay command from splitter */
        char cmd[32];
        if (json_get_string(msg, "c", cmd, sizeof(cmd)) >= 0) {
            if (strcmp(cmd, "pong") == 0) {
                sp->last_seen = time(NULL);
            } else if (strcmp(cmd, "ready") == 0) {
                /* Splitter is ready - check has_sdr and assign data ports */
                char has_sdr_str[16];
                sp->has_sdr = (json_get_string(msg, "has_sdr", has_sdr_str, sizeof(has_sdr_str)) >= 0 
                               && strcmp(has_sdr_str, "true") == 0);
                
                /* Allocate detector and display ports */
                sp->det_port = allocate_splitter_port();
                sp->disp_port = allocate_splitter_port();
                
                /* Create listen sockets */
                sp->det_listen_fd = create_listen_socket(sp->det_port);
                sp->disp_listen_fd = create_listen_socket(sp->disp_port);
                
                if (sp->det_listen_fd < 0 || sp->disp_listen_fd < 0) {
                    fprintf(stderr, "[SPLITTER] Failed to create data listen sockets\n");
                    close_splitter(sp);
                    return;
                }
                
                set_nonblocking(sp->det_listen_fd);
                set_nonblocking(sp->disp_listen_fd);
                
                /* Send port assignment */
                char response[128];
                int len = snprintf(response, sizeof(response),
                                   "{\"t\":\"r\",\"c\":\"ports\",\"det\":%d,\"disp\":%d}\n",
                                   sp->det_port, sp->disp_port);
                send(sp->ctrl_fd, response, len, MSG_NOSIGNAL);
                
                fprintf(stderr, "[SPLITTER] %s assigned data ports: det=%d, disp=%d (has_sdr=%d)\n",
                        sp->node_id, sp->det_port, sp->disp_port, sp->has_sdr);
            }
            /* Add more relay commands as needed */
        }
    } else if (type[0] == MSG_TYPE_SDR) {
        /* SDR response - forward to client */
        if (sp->client_fd < 0) return;
        
        char data[4096];
        int len = json_get_string(msg, "d", data, sizeof(data));
        if (len > 0) {
            len = json_unescape(data, len);
            send(sp->client_fd, data, len, MSG_NOSIGNAL);
        }
    }
}

/* Read and process control data from splitter */
static void handle_splitter_data(splitter_conn_t *sp) {
    int space = sizeof(sp->recv_buf) - sp->recv_len - 1;
    if (space <= 0) {
        sp->recv_len = 0;
        return;
    }
    
    ssize_t received = recv(sp->ctrl_fd, sp->recv_buf + sp->recv_len, space, 0);
    
    if (received <= 0) {
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        fprintf(stderr, "[SPLITTER] %s disconnected\n", sp->node_id);
        close_splitter(sp);
        return;
    }
    
    sp->recv_len += received;
    sp->recv_buf[sp->recv_len] = '\0';
    sp->last_seen = time(NULL);
    
    /* Process complete messages */
    char *start = sp->recv_buf;
    char *newline;
    while ((newline = strchr(start, '\n')) != NULL) {
        *newline = '\0';
        if (newline > start) {
            process_splitter_message(sp, start);
        }
        start = newline + 1;
    }
    
    /* Move incomplete message to front */
    if (start > sp->recv_buf) {
        sp->recv_len = strlen(start);
        memmove(sp->recv_buf, start, sp->recv_len + 1);
    }
}

/* Handle data from client connected to splitter */
static void handle_client_to_splitter(splitter_conn_t *sp) {
    char buffer[4096];
    ssize_t received = recv(sp->client_fd, buffer, sizeof(buffer), 0);
    
    if (received <= 0) {
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        fprintf(stderr, "[SPLITTER] Client disconnected from %s\n", sp->node_id);
        close(sp->client_fd);
        sp->client_fd = -1;
        return;
    }
    
    /* Forward to splitter as SDR command */
    splitter_send_sdr(sp, buffer, received);
}

/*============================================================================
 * Discovery Coordinator (TCP Registry)
 *============================================================================*/

/* Find edge node by fd */
static int find_edge_by_fd(int fd) {
    for (int i = 0; i < g_edge_count; i++) {
        if (g_edge_nodes[i].fd == fd) return i;
    }
    return -1;
}

/* Add or update edge node */
static int add_edge_node(int fd, const char *ip) {
    int idx = find_edge_by_fd(fd);
    if (idx >= 0) {
        g_edge_nodes[idx].last_seen = time(NULL);
        return idx;
    }
    
    if (g_edge_count >= MAX_EDGE_NODES) {
        fprintf(stderr, "[DISCOVERY] Max edge nodes reached, rejecting\n");
        return -1;
    }
    
    edge_node_t *edge = &g_edge_nodes[g_edge_count];
    edge->fd = fd;
    strncpy(edge->ip, ip, sizeof(edge->ip) - 1);
    edge->last_seen = time(NULL);
    edge->service_count = 0;
    
    fprintf(stderr, "[DISCOVERY] Edge node connected: %s (idx=%d)\n", ip, g_edge_count);
    return g_edge_count++;
}

/* Remove edge node and its services */
static void remove_edge_node(int idx) {
    if (idx < 0 || idx >= g_edge_count) return;
    
    edge_node_t *edge = &g_edge_nodes[idx];
    fprintf(stderr, "[DISCOVERY] Edge node disconnected: %s\n", edge->ip);
    
    /* Remove all services from this edge */
    for (int i = g_service_count - 1; i >= 0; i--) {
        if (g_services[i].edge_idx == idx) {
            fprintf(stderr, "[DISCOVERY] Removing service: %s/%s\n", 
                    g_services[i].service, g_services[i].id);
            /* Shift remaining services */
            for (int j = i; j < g_service_count - 1; j++) {
                g_services[j] = g_services[j + 1];
            }
            g_service_count--;
        }
    }
    
    /* Update edge_idx for services from higher-indexed edges */
    for (int i = 0; i < g_service_count; i++) {
        if (g_services[i].edge_idx > idx) {
            g_services[i].edge_idx--;
        }
    }
    
    close(edge->fd);
    
    /* Shift remaining edges */
    for (int i = idx; i < g_edge_count - 1; i++) {
        g_edge_nodes[i] = g_edge_nodes[i + 1];
    }
    g_edge_count--;
}

/* Register a service from an edge node */
static int register_service(int edge_idx, const char *id, const char *service,
                            int ctrl_port, int data_port, const char *caps) {
    /* Check if service already exists (update it) */
    for (int i = 0; i < g_service_count; i++) {
        if (strcmp(g_services[i].id, id) == 0 && 
            strcmp(g_services[i].service, service) == 0) {
            /* Update existing */
            g_services[i].ctrl_port = ctrl_port;
            g_services[i].data_port = data_port;
            strncpy(g_services[i].caps, caps, sizeof(g_services[i].caps) - 1);
            g_services[i].registered = time(NULL);
            return i;
        }
    }
    
    if (g_service_count >= MAX_SERVICES) {
        fprintf(stderr, "[DISCOVERY] Max services reached\n");
        return -1;
    }
    
    service_entry_t *svc = &g_services[g_service_count];
    strncpy(svc->id, id, sizeof(svc->id) - 1);
    strncpy(svc->service, service, sizeof(svc->service) - 1);
    strncpy(svc->ip, g_edge_nodes[edge_idx].ip, sizeof(svc->ip) - 1);
    svc->ctrl_port = ctrl_port;
    svc->data_port = data_port;
    strncpy(svc->caps, caps, sizeof(svc->caps) - 1);
    svc->edge_idx = edge_idx;
    svc->registered = time(NULL);
    
    g_edge_nodes[edge_idx].service_count++;
    
    fprintf(stderr, "[DISCOVERY] Registered: %s/%s at %s:%d/%d caps=%s\n",
            service, id, svc->ip, ctrl_port, data_port, caps);
    
    return g_service_count++;
}

/* Unregister a service */
static void unregister_service(const char *id, const char *service) {
    for (int i = 0; i < g_service_count; i++) {
        if (strcmp(g_services[i].id, id) == 0 &&
            (service == NULL || strcmp(g_services[i].service, service) == 0)) {
            
            fprintf(stderr, "[DISCOVERY] Unregistered: %s/%s\n", 
                    g_services[i].service, g_services[i].id);
            
            int edge_idx = g_services[i].edge_idx;
            if (edge_idx >= 0 && edge_idx < g_edge_count) {
                g_edge_nodes[edge_idx].service_count--;
            }
            
            /* Shift remaining */
            for (int j = i; j < g_service_count - 1; j++) {
                g_services[j] = g_services[j + 1];
            }
            g_service_count--;
            return;
        }
    }
}

/* Build JSON response listing services */
static int build_service_list(char *buf, size_t buf_size, const char *filter_service) {
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, 
                    "{\"m\":\"PNSD\",\"v\":1,\"cmd\":\"list\",\"services\":[");
    
    bool first = true;
    for (int i = 0; i < g_service_count; i++) {
        if (filter_service && strcmp(g_services[i].service, filter_service) != 0) {
            continue;
        }
        
        if (!first) pos += snprintf(buf + pos, buf_size - pos, ",");
        first = false;
        
        pos += snprintf(buf + pos, buf_size - pos,
                        "{\"id\":\"%s\",\"svc\":\"%s\",\"ip\":\"%s\","
                        "\"port\":%d,\"data\":%d,\"caps\":\"%s\"}",
                        g_services[i].id, g_services[i].service, g_services[i].ip,
                        g_services[i].ctrl_port, g_services[i].data_port,
                        g_services[i].caps);
    }
    
    pos += snprintf(buf + pos, buf_size - pos, "]}\n");
    return pos;
}

/* Process discovery message from edge node */
static void process_discovery_message(int edge_idx, const char *msg) {
    char cmd[32] = {0};
    char id[64] = {0};
    char svc[32] = {0};
    char caps[128] = {0};
    
    json_get_string(msg, "cmd", cmd, sizeof(cmd));
    
    if (strcmp(cmd, DISC_CMD_HELO) == 0) {
        json_get_string(msg, "id", id, sizeof(id));
        json_get_string(msg, "svc", svc, sizeof(svc));
        json_get_string(msg, "caps", caps, sizeof(caps));
        int port = json_get_int(msg, "port");
        int data = json_get_int(msg, "data");
        
        register_service(edge_idx, id, svc, port, data, caps);
        g_edge_nodes[edge_idx].last_seen = time(NULL);
        
    } else if (strcmp(cmd, DISC_CMD_BYE) == 0) {
        json_get_string(msg, "id", id, sizeof(id));
        json_get_string(msg, "svc", svc, sizeof(svc));
        
        unregister_service(id, svc[0] ? svc : NULL);
        
    } else if (strcmp(cmd, DISC_CMD_LIST) == 0 || strcmp(cmd, DISC_CMD_FIND) == 0) {
        char filter[32] = {0};
        if (strcmp(cmd, DISC_CMD_FIND) == 0) {
            json_get_string(msg, "svc", filter, sizeof(filter));
        }
        
        char response[4096];
        int len = build_service_list(response, sizeof(response), 
                                     filter[0] ? filter : NULL);
        
        send(g_edge_nodes[edge_idx].fd, response, len, MSG_NOSIGNAL);
    }
}

/* Handle incoming data from edge node */
static void handle_edge_data(int edge_idx) {
    char buffer[4096];
    edge_node_t *edge = &g_edge_nodes[edge_idx];
    
    ssize_t received = recv(edge->fd, buffer, sizeof(buffer) - 1, 0);
    
    if (received <= 0) {
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        remove_edge_node(edge_idx);
        return;
    }
    
    buffer[received] = '\0';
    
    /* Process each line (newline-delimited JSON) */
    char *line = strtok(buffer, "\n");
    while (line) {
        if (line[0] == '{') {
            process_discovery_message(edge_idx, line);
        }
        line = strtok(NULL, "\n");
    }
}

/* Check for timed-out edge nodes */
static void check_edge_timeouts(void) {
    time_t now = time(NULL);
    
    for (int i = g_edge_count - 1; i >= 0; i--) {
        if (now - g_edge_nodes[i].last_seen > EDGE_TIMEOUT_SEC) {
            fprintf(stderr, "[DISCOVERY] Edge timeout: %s\n", g_edge_nodes[i].ip);
            remove_edge_node(i);
        }
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

    fprintf(stderr, "[STATUS] Discovery: edges=%d services=%d\n",
            g_edge_count, g_service_count);
    
    /* Check for edge timeouts */
    check_edge_timeouts();
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
        FD_SET(g_rendezvous_listen_fd, &readfds);
        FD_SET(g_discovery_listen_fd, &readfds);
        max_fd = g_detector_listen_fd;
        if (g_display_listen_fd > max_fd) max_fd = g_display_listen_fd;
        if (g_rendezvous_listen_fd > max_fd) max_fd = g_rendezvous_listen_fd;
        if (g_discovery_listen_fd > max_fd) max_fd = g_discovery_listen_fd;

        /* Source sockets */
        if (g_detector_source_fd >= 0) {
            FD_SET(g_detector_source_fd, &readfds);
            if (g_detector_source_fd > max_fd) max_fd = g_detector_source_fd;
        }
        if (g_display_source_fd >= 0) {
            FD_SET(g_display_source_fd, &readfds);
            if (g_display_source_fd > max_fd) max_fd = g_display_source_fd;
        }
        
        /* Splitter sockets */
        for (int i = 0; i < MAX_SPLITTERS; i++) {
            splitter_conn_t *sp = &g_splitters[i];
            if (!sp->active) continue;
            
            if (sp->listen_fd >= 0) {
                FD_SET(sp->listen_fd, &readfds);
                if (sp->listen_fd > max_fd) max_fd = sp->listen_fd;
            }
            if (sp->ctrl_fd >= 0) {
                FD_SET(sp->ctrl_fd, &readfds);
                if (sp->ctrl_fd > max_fd) max_fd = sp->ctrl_fd;
            }
            if (sp->client_fd >= 0) {
                FD_SET(sp->client_fd, &readfds);
                if (sp->client_fd > max_fd) max_fd = sp->client_fd;
            }
            if (sp->det_listen_fd >= 0) {
                FD_SET(sp->det_listen_fd, &readfds);
                if (sp->det_listen_fd > max_fd) max_fd = sp->det_listen_fd;
            }
            if (sp->det_source_fd >= 0) {
                FD_SET(sp->det_source_fd, &readfds);
                if (sp->det_source_fd > max_fd) max_fd = sp->det_source_fd;
            }
            if (sp->disp_listen_fd >= 0) {
                FD_SET(sp->disp_listen_fd, &readfds);
                if (sp->disp_listen_fd > max_fd) max_fd = sp->disp_listen_fd;
            }
            if (sp->disp_source_fd >= 0) {
                FD_SET(sp->disp_source_fd, &readfds);
                if (sp->disp_source_fd > max_fd) max_fd = sp->disp_source_fd;
            }
        }
        
        /* Edge node sockets */
        for (int i = 0; i < g_edge_count; i++) {
            FD_SET(g_edge_nodes[i].fd, &readfds);
            if (g_edge_nodes[i].fd > max_fd) max_fd = g_edge_nodes[i].fd;
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

        /* Accept rendezvous connections (new splitters) */
        if (FD_ISSET(g_rendezvous_listen_fd, &readfds)) {
            handle_rendezvous_accept();
        }
        
        /* Handle splitter connections */
        for (int i = 0; i < MAX_SPLITTERS; i++) {
            splitter_conn_t *sp = &g_splitters[i];
            if (!sp->active) continue;
            
            /* Accept on splitter's control listen socket */
            if (sp->listen_fd >= 0 && FD_ISSET(sp->listen_fd, &readfds)) {
                handle_splitter_accept(sp);
            }
            
            /* Accept on splitter's detector listen socket */
            if (sp->det_listen_fd >= 0 && FD_ISSET(sp->det_listen_fd, &readfds)) {
                struct sockaddr_in addr;
                socklen_t addr_len = sizeof(addr);
                int fd = accept(sp->det_listen_fd, (struct sockaddr*)&addr, &addr_len);
                if (fd >= 0) {
                    set_nonblocking(fd);
                    sp->det_source_fd = fd;
                    fprintf(stderr, "[SPLITTER] %s detector stream connected (port %d)\n", 
                            sp->node_id, sp->det_port);
                }
            }
            
            /* Accept on splitter's display listen socket */
            if (sp->disp_listen_fd >= 0 && FD_ISSET(sp->disp_listen_fd, &readfds)) {
                struct sockaddr_in addr;
                socklen_t addr_len = sizeof(addr);
                int fd = accept(sp->disp_listen_fd, (struct sockaddr*)&addr, &addr_len);
                if (fd >= 0) {
                    set_nonblocking(fd);
                    sp->disp_source_fd = fd;
                    fprintf(stderr, "[SPLITTER] %s display stream connected (port %d)\n", 
                            sp->node_id, sp->disp_port);
                }
            }
            
            /* Data from splitter control channel */
            if (sp->ctrl_fd >= 0 && FD_ISSET(sp->ctrl_fd, &readfds)) {
                handle_splitter_data(sp);
            }
            
            /* Data from client connected to splitter */
            if (sp->client_fd >= 0 && FD_ISSET(sp->client_fd, &readfds)) {
                handle_client_to_splitter(sp);
            }
            
            /* Detector stream data - relay to clients */
            if (sp->det_source_fd >= 0 && FD_ISSET(sp->det_source_fd, &readfds)) {
                if (!receive_and_relay(sp->det_source_fd, &g_detector_clients, "DETECTOR")) {
                    close(sp->det_source_fd);
                    sp->det_source_fd = -1;
                }
            }
            
            /* Display stream data - relay to clients */
            if (sp->disp_source_fd >= 0 && FD_ISSET(sp->disp_source_fd, &readfds)) {
                if (!receive_and_relay(sp->disp_source_fd, &g_display_clients, "DISPLAY")) {
                    close(sp->disp_source_fd);
                    sp->disp_source_fd = -1;
                }
            }
        }

        /* Accept discovery connections (edge nodes) */
        if (FD_ISSET(g_discovery_listen_fd, &readfds)) {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            int fd = accept(g_discovery_listen_fd, (struct sockaddr*)&addr, &addr_len);

            if (fd >= 0) {
                set_nonblocking(fd);
                char ip_str[64];
                strncpy(ip_str, inet_ntoa(addr.sin_addr), sizeof(ip_str) - 1);
                
                int edge_idx = add_edge_node(fd, ip_str);
                if (edge_idx < 0) {
                    close(fd);
                }
            }
        }
        
        /* Handle data from edge nodes */
        for (int i = g_edge_count - 1; i >= 0; i--) {
            if (FD_ISSET(g_edge_nodes[i].fd, &readfds)) {
                handle_edge_data(i);
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
                forward_control_data(g_control_client_fd, g_control_source_fd, "CLIENT->SOURCE");
            }
            /* Source → Client (responses from SDR to remote user) */
            if (FD_ISSET(g_control_source_fd, &readfds)) {
                forward_control_data(g_control_source_fd, g_control_client_fd, "SOURCE->CLIENT");
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
    printf("Detector stream:  port %d (50 kHz float32 I/Q)\n", DETECTOR_PORT);
    printf("Display stream:   port %d (12 kHz float32 I/Q)\n", DISPLAY_PORT);
    printf("Control relay:    port %d (text commands)\n", CONTROL_PORT);
    printf("Discovery coord:  port %d (TCP service registry)\n\n", DISCOVERY_PORT);

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
    g_discovery_listen_fd = create_listen_socket(DISCOVERY_PORT);

    if (g_detector_listen_fd < 0 || g_display_listen_fd < 0 || 
        g_control_listen_fd < 0 || g_discovery_listen_fd < 0) {
        fprintf(stderr, "Failed to create listen sockets\n");
        return 1;
    }

    set_nonblocking(g_detector_listen_fd);
    set_nonblocking(g_display_listen_fd);
    set_nonblocking(g_control_listen_fd);
    set_nonblocking(g_discovery_listen_fd);

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

    /* Close edge nodes */
    for (int i = 0; i < g_edge_count; i++) {
        close(g_edge_nodes[i].fd);
    }

    close(g_detector_listen_fd);
    close(g_display_listen_fd);
    close(g_control_listen_fd);
    close(g_discovery_listen_fd);

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
