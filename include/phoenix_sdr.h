/**
 * @file phoenix_sdr.h
 * @brief Phoenix Nest SDR Interface - RSP2 Pro Integration
 *
 * Public API for SDRplay RSP2 Pro integration with Phoenix Nest MARS Suite.
 * Provides device enumeration, streaming, and I/Q data access for
 * MIL-STD-188-110A modem receive testing.
 *
 * @author Phoenix Nest LLC
 * @date 2024-12-12
 */

#ifndef PHOENIX_SDR_H
#define PHOENIX_SDR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Version Info
 *============================================================================*/

#define PHOENIX_SDR_VERSION_MAJOR  0
#define PHOENIX_SDR_VERSION_MINOR  1
#define PHOENIX_SDR_VERSION_PATCH  0

/*============================================================================
 * Error Codes
 *============================================================================*/

typedef enum {
    PSDR_OK = 0,
    PSDR_ERR_API_OPEN,
    PSDR_ERR_API_VERSION,
    PSDR_ERR_NO_DEVICES,
    PSDR_ERR_DEVICE_SELECT,
    PSDR_ERR_DEVICE_PARAMS,
    PSDR_ERR_INIT,
    PSDR_ERR_UNINIT,
    PSDR_ERR_UPDATE,
    PSDR_ERR_INVALID_ARG,
    PSDR_ERR_NOT_INITIALIZED,
    PSDR_ERR_ALREADY_STREAMING,
    PSDR_ERR_NOT_STREAMING,
    PSDR_ERR_CALLBACK,
    PSDR_ERR_UNKNOWN
} psdr_error_t;

/*============================================================================
 * Configuration Structures
 *============================================================================*/

/** Bandwidth options (kHz) */
typedef enum {
    PSDR_BW_200  = 200,
    PSDR_BW_300  = 300,
    PSDR_BW_600  = 600,
    PSDR_BW_1536 = 1536,
    PSDR_BW_5000 = 5000,
    PSDR_BW_6000 = 6000,
    PSDR_BW_7000 = 7000,
    PSDR_BW_8000 = 8000
} psdr_bandwidth_t;

/** RSP2 Antenna selection */
typedef enum {
    PSDR_ANT_A = 0,
    PSDR_ANT_B,
    PSDR_ANT_HIZ   /* High-Z port for lower frequencies */
} psdr_antenna_t;

/** AGC mode */
typedef enum {
    PSDR_AGC_DISABLED = 0,
    PSDR_AGC_5HZ,
    PSDR_AGC_50HZ,
    PSDR_AGC_100HZ
} psdr_agc_mode_t;

/** IF type (Zero-IF vs Low-IF) */
typedef enum {
    PSDR_IF_ZERO = 0,   /**< Zero-IF (signal at DC) */
    PSDR_IF_LOW         /**< Low-IF (signal at IF offset) */
} psdr_if_mode_t;

/** SDR Configuration */
typedef struct {
    double       freq_hz;        /**< Center frequency in Hz */
    double       sample_rate_hz; /**< Sample rate (2e6 to 10e6) */
    psdr_bandwidth_t bandwidth;  /**< IF bandwidth */
    psdr_antenna_t   antenna;    /**< Antenna port */
    psdr_agc_mode_t  agc_mode;   /**< AGC mode (DISABLED recommended for modem) */
    int          gain_reduction; /**< Gain reduction in dB (20-59) */
    int          lna_state;      /**< LNA state (0-8 for RSP2) */
    uint8_t      decimation;     /**< Hardware decimation factor (1,2,4,8,16,32) */
    bool         bias_t;         /**< Enable bias-T */
    bool         rf_notch;       /**< Enable FM broadcast notch filter */

    /* Advanced settings */
    psdr_if_mode_t if_mode;      /**< IF mode (ZERO or LOW) */
    bool         dc_offset_corr; /**< Enable DC offset correction */
    bool         iq_imbalance_corr; /**< Enable IQ imbalance correction */
    int          agc_setpoint_dbfs; /**< AGC setpoint in dBFS (-72 to 0) */
} psdr_config_t;

/** Device info (from enumeration) */
typedef struct {
    char    serial[64];
    uint8_t hw_version;
    bool    available;
} psdr_device_info_t;

/*============================================================================
 * Callback Types
 *============================================================================*/

/**
 * @brief I/Q sample callback
 *
 * Called by streaming thread when samples are available.
 * WARNING: Called from API thread - keep processing minimal or copy & defer.
 *
 * @param xi        Pointer to I (real) samples, 16-bit signed
 * @param xq        Pointer to Q (imaginary) samples, 16-bit signed
 * @param count     Number of samples
 * @param reset     If true, internal state was reset (flush buffers)
 * @param user_ctx  User-provided context pointer
 */
typedef void (*psdr_sample_callback_t)(
    const int16_t *xi,
    const int16_t *xq,
    uint32_t count,
    bool reset,
    void *user_ctx
);

/**
 * @brief Gain change notification callback
 *
 * @param gain_db   Current system gain in dB
 * @param lna_db    LNA gain reduction in dB
 * @param user_ctx  User-provided context pointer
 */
typedef void (*psdr_gain_callback_t)(
    double gain_db,
    int lna_db,
    void *user_ctx
);

/**
 * @brief Power overload callback
 *
 * Called when ADC overload is detected or corrected.
 *
 * @param overloaded  True if overload detected, false if corrected
 * @param user_ctx    User-provided context pointer
 */
typedef void (*psdr_overload_callback_t)(
    bool overloaded,
    void *user_ctx
);

/** Callback set */
typedef struct {
    psdr_sample_callback_t   on_samples;
    psdr_gain_callback_t     on_gain_change;
    psdr_overload_callback_t on_overload;
    void                    *user_ctx;
} psdr_callbacks_t;

/*============================================================================
 * Opaque Handle
 *============================================================================*/

typedef struct psdr_context psdr_context_t;

/*============================================================================
 * API Functions
 *============================================================================*/

/**
 * @brief Get human-readable error string
 */
const char* psdr_strerror(psdr_error_t err);

/**
 * @brief Initialize default configuration
 *
 * Sets config to sensible defaults for HF narrowband work:
 * - 2 MSPS sample rate
 * - 200 kHz bandwidth
 * - Zero IF
 * - AGC disabled
 * - Antenna A
 */
void psdr_config_defaults(psdr_config_t *config);

/**
 * @brief Open SDRplay API and enumerate devices
 *
 * @param devices     Array to receive device info (can be NULL)
 * @param max_devices Size of devices array
 * @param num_found   Receives number of devices found
 * @return Error code
 */
psdr_error_t psdr_enumerate(
    psdr_device_info_t *devices,
    size_t max_devices,
    size_t *num_found
);

/**
 * @brief Create SDR context and select device
 *
 * @param ctx         Receives allocated context
 * @param device_idx  Device index from enumeration (0 for first)
 * @return Error code
 */
psdr_error_t psdr_open(psdr_context_t **ctx, unsigned int device_idx);

/**
 * @brief Configure device (before starting stream)
 *
 * @param ctx     SDR context
 * @param config  Configuration to apply
 * @return Error code
 */
psdr_error_t psdr_configure(psdr_context_t *ctx, const psdr_config_t *config);

/**
 * @brief Start streaming with callbacks
 *
 * @param ctx        SDR context
 * @param callbacks  Callback functions and user context
 * @return Error code
 */
psdr_error_t psdr_start(psdr_context_t *ctx, const psdr_callbacks_t *callbacks);

/**
 * @brief Update parameters while streaming
 *
 * Only certain parameters can be updated during streaming:
 * - Frequency
 * - Gain/LNA state
 * - AGC settings
 *
 * @param ctx     SDR context
 * @param config  Configuration with updated values
 * @return Error code
 */
psdr_error_t psdr_update(psdr_context_t *ctx, const psdr_config_t *config);

/**
 * @brief Stop streaming
 *
 * @param ctx  SDR context
 * @return Error code
 */
psdr_error_t psdr_stop(psdr_context_t *ctx);

/**
 * @brief Close device and release resources
 *
 * @param ctx  SDR context (will be freed)
 */
void psdr_close(psdr_context_t *ctx);

/**
 * @brief Get current actual sample rate
 *
 * May differ slightly from requested rate.
 *
 * @param ctx  SDR context
 * @return Actual sample rate in Hz, or 0 if not streaming
 */
double psdr_get_sample_rate(const psdr_context_t *ctx);

/**
 * @brief Check if currently streaming
 *
 * @param ctx  SDR context
 * @return True if streaming
 */
bool psdr_is_streaming(const psdr_context_t *ctx);

/**
 * @brief Print device parameters (defaults from API)
 *
 * Prints the raw device parameters returned by the SDRplay API
 * after device selection, before any configuration is applied.
 *
 * @param ctx  SDR context
 */
void psdr_print_device_params(const psdr_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PHOENIX_SDR_H */
