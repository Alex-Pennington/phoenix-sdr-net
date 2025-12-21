/**
 * @file tcp_server.h
 * @brief Phoenix SDR TCP Control Server
 *
 * Text-based TCP protocol for remote SDR control.
 * See docs/SDR_TCP_CONTROL_INTERFACE.md for protocol specification.
 */

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "phoenix_sdr.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Constants
 *============================================================================*/

#define TCP_DEFAULT_PORT        4535
#define TCP_MAX_LINE_LENGTH     256
#define TCP_MAX_ARGS            8

/*============================================================================
 * Command Types
 *============================================================================*/

typedef enum {
    CMD_UNKNOWN = 0,

    /* Frequency */
    CMD_SET_FREQ,
    CMD_GET_FREQ,

    /* Gain */
    CMD_SET_GAIN,
    CMD_GET_GAIN,
    CMD_SET_LNA,
    CMD_GET_LNA,
    CMD_SET_AGC,
    CMD_GET_AGC,

    /* Sample Rate / Bandwidth */
    CMD_SET_SRATE,
    CMD_GET_SRATE,
    CMD_SET_BW,
    CMD_GET_BW,

    /* Hardware */
    CMD_SET_ANTENNA,
    CMD_GET_ANTENNA,
    CMD_SET_BIAST,
    CMD_SET_NOTCH,
    CMD_SET_DECIM,
    CMD_GET_DECIM,
    CMD_SET_IFMODE,
    CMD_GET_IFMODE,
    CMD_SET_DCOFFSET,
    CMD_GET_DCOFFSET,
    CMD_SET_IQCORR,
    CMD_GET_IQCORR,
    CMD_SET_AGC_SETPOINT,
    CMD_GET_AGC_SETPOINT,

    /* Streaming */
    CMD_START,
    CMD_STOP,
    CMD_STATUS,

    /* Utility */
    CMD_PING,
    CMD_VER,
    CMD_CAPS,
    CMD_HELP,
    CMD_QUIT,

    CMD_COUNT  /* Number of commands */
} tcp_cmd_type_t;

/*============================================================================
 * Error Codes
 *============================================================================*/

typedef enum {
    TCP_OK = 0,
    TCP_ERR_SYNTAX,     /* Malformed command */
    TCP_ERR_UNKNOWN,    /* Unknown command */
    TCP_ERR_PARAM,      /* Invalid parameter value */
    TCP_ERR_RANGE,      /* Value out of valid range */
    TCP_ERR_STATE,      /* Invalid state for operation */
    TCP_ERR_BUSY,       /* SDR busy / another client */
    TCP_ERR_HARDWARE,   /* SDR hardware error */
    TCP_ERR_TIMEOUT     /* Operation timed out */
} tcp_error_t;

/*============================================================================
 * Parsed Command Structure
 *============================================================================*/

typedef struct {
    tcp_cmd_type_t type;
    int            argc;                       /* Number of arguments */
    char           argv[TCP_MAX_ARGS][64];     /* Argument strings */

    /* Parsed values (filled by parse_command_args) */
    union {
        double      freq_hz;
        int         gain_db;
        int         lna_state;
        int         sample_rate;
        int         bandwidth_khz;
        bool        on_off;
        int         decimation;
        int         agc_setpoint;
        char        if_mode[16];
        struct {
            char    mode[16];
        } agc;
        struct {
            char    port[8];
        } antenna;
    } value;
} tcp_command_t;

/*============================================================================
 * Response Structure
 *============================================================================*/

typedef struct {
    tcp_error_t     error;
    char            message[TCP_MAX_LINE_LENGTH];
} tcp_response_t;

/*============================================================================
 * SDR State (for commands to query/modify)
 *============================================================================*/

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET tcp_socket_t;
typedef CRITICAL_SECTION tcp_mutex_t;
#define TCP_INVALID_SOCKET INVALID_SOCKET
#else
typedef int tcp_socket_t;
typedef pthread_mutex_t tcp_mutex_t;
#define TCP_INVALID_SOCKET (-1)
#endif

typedef struct {
    /* Actual SDR hardware context (NULL if not connected) */
    psdr_context_t *sdr_ctx;
    psdr_config_t   sdr_config;
    psdr_callbacks_t sdr_callbacks;

    /* Cached state (mirrors hardware or used when hardware not connected) */
    bool        streaming;
    double      freq_hz;
    int         gain_reduction;
    int         lna_state;
    char        agc_mode[16];
    int         sample_rate;
    int         bandwidth_khz;
    char        antenna[8];
    bool        bias_t;
    bool        notch;
    bool        overload;

    /* Advanced settings */
    int         decimation;         /* 1,2,4,8,16,32 */
    char        if_mode[16];        /* ZERO, LOW */
    bool        dc_offset_corr;     /* DC offset correction */
    bool        iq_imbalance_corr;  /* IQ imbalance correction */
    int         agc_setpoint;       /* AGC setpoint in dBFS (-72 to 0) */

    /* Hardware integration flags */
    bool        hardware_connected;  /* True if SDR hardware is available */

    /* Async notification support */
    tcp_socket_t client_socket;      /* Current client socket for notifications */
    tcp_mutex_t  notify_mutex;       /* Protects socket access from callback thread */
    bool         notify_enabled;     /* True when client is connected */
} tcp_sdr_state_t;

/*============================================================================
 * Notification Functions
 *============================================================================*/

/**
 * @brief Initialize notification mutex
 */
void tcp_notify_init(tcp_sdr_state_t *state);

/**
 * @brief Cleanup notification mutex
 */
void tcp_notify_cleanup(tcp_sdr_state_t *state);

/**
 * @brief Set client socket for async notifications
 */
void tcp_notify_set_client(tcp_sdr_state_t *state, tcp_socket_t client);

/**
 * @brief Clear client socket (client disconnected)
 */
void tcp_notify_clear_client(tcp_sdr_state_t *state);

/**
 * @brief Send async notification to client (thread-safe)
 *
 * @param state     SDR state with client socket
 * @param format    printf-style format string
 * @return          0 on success, -1 if no client or send failed
 */
int tcp_send_notification(tcp_sdr_state_t *state, const char *format, ...);

/*============================================================================
 * Command Parsing Functions
 *============================================================================*/

/**
 * @brief Parse a command line into structured command
 *
 * @param line      Input line (null-terminated, may include \n)
 * @param cmd       Output command structure
 * @return          TCP_OK on success, error code on failure
 */
tcp_error_t tcp_parse_command(const char *line, tcp_command_t *cmd);

/**
 * @brief Get command name from type
 */
const char* tcp_cmd_name(tcp_cmd_type_t type);

/**
 * @brief Get error code string
 */
const char* tcp_error_name(tcp_error_t err);

/*============================================================================
 * Command Execution Functions
 *============================================================================*/

/**
 * @brief Execute a parsed command
 *
 * @param cmd       Parsed command
 * @param state     SDR state (queried/modified)
 * @param response  Output response
 * @return          TCP_OK on success
 */
tcp_error_t tcp_execute_command(
    const tcp_command_t *cmd,
    tcp_sdr_state_t *state,
    tcp_response_t *response
);

/*============================================================================
 * Response Formatting Functions
 *============================================================================*/

/**
 * @brief Format a success response
 */
void tcp_response_ok(tcp_response_t *resp, const char *value);

/**
 * @brief Format an error response
 */
void tcp_response_error(tcp_response_t *resp, tcp_error_t err, const char *msg);

/**
 * @brief Format response as string for sending
 *
 * @param resp      Response structure
 * @param buf       Output buffer
 * @param buf_size  Size of output buffer
 * @return          Number of bytes written
 */
int tcp_format_response(const tcp_response_t *resp, char *buf, size_t buf_size);

/*============================================================================
 * Server Functions (implemented in sdr_server.c)
 *============================================================================*/

/**
 * @brief Initialize default SDR state
 */
void tcp_state_defaults(tcp_sdr_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SERVER_H */
