#ifndef MU16TTY_MODEM_H
#define MU16TTY_MODEM_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(MU16TTY_MODEM_BUILD)
#define MU16TTY_API __declspec(dllexport)
#else
#define MU16TTY_API __declspec(dllimport)
#endif
#else
#define MU16TTY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MU16TTY_ABI_VERSION 1u
#define MU16TTY_MAX_PACKET_BYTES 320u

typedef struct mu16tty_context mu16tty_context;

typedef enum mu16tty_result {
    MU16TTY_OK = 0,
    MU16TTY_NO_EVENT = 1,
    MU16TTY_ERROR_INVALID_ARGUMENT = -1,
    MU16TTY_ERROR_ABI_VERSION = -2,
    MU16TTY_ERROR_STRUCT_SIZE = -3,
    MU16TTY_ERROR_NO_MEMORY = -4
} mu16tty_result;

typedef enum mu16tty_event_type {
    MU16TTY_EVENT_NONE = 0,
    MU16TTY_EVENT_WAKE = 1,
    MU16TTY_EVENT_HEAD_SYNC = 2,
    MU16TTY_EVENT_MID_SYNC = 3,
    MU16TTY_EVENT_HEADER = 4,
    MU16TTY_EVENT_PACKET = 5,
    MU16TTY_EVENT_DAMAGED_PACKET = 6,
    MU16TTY_EVENT_HEAD_REJECTED = 7, /* payload[0] is a mu16tty_head_rejection */
    MU16TTY_EVENT_PREVIEW = 8 /* payload is provisional message bytes */
} mu16tty_event_type;

typedef enum mu16tty_head_rejection {
    MU16TTY_HEAD_EXPIRED = 1,
    MU16TTY_HEAD_DECODE_FAILED = 2,
    MU16TTY_HEAD_INVALID = 3,
    MU16TTY_HEAD_MODE_MISMATCH = 4
} mu16tty_head_rejection;

typedef enum mu16tty_decode_path {
    MU16TTY_DECODE_PATH_NONE = 0,
    MU16TTY_DECODE_PATH_HEAD = 1,
    MU16TTY_DECODE_PATH_MID = 2
} mu16tty_decode_path;

typedef enum mu16tty_metric_validity {
    MU16TTY_METRIC_PATH = UINT64_C(1) << 0,
    MU16TTY_METRIC_SYNC = UINT64_C(1) << 1,
    MU16TTY_METRIC_FREQUENCY = UINT64_C(1) << 2,
    MU16TTY_METRIC_CONFIDENCE = UINT64_C(1) << 3,
    MU16TTY_METRIC_BER_FEC = UINT64_C(1) << 4,
    MU16TTY_METRIC_SNR = UINT64_C(1) << 5,
    MU16TTY_METRIC_REFINEMENT = UINT64_C(1) << 6,
    MU16TTY_METRIC_MID = UINT64_C(1) << 7,
    MU16TTY_METRIC_CRC = UINT64_C(1) << 8
} mu16tty_metric_validity;

typedef struct mu16tty_rx_metrics {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t valid_fields;
    uint32_t decode_path;
    uint32_t sync_matches;
    uint32_t sync_symbols;
    uint32_t pre_fec_bit_errors_est;
    uint32_t pre_fec_bits;
    uint32_t refinement_invocations;
    uint32_t refinement_improvements;
    uint32_t mid_marker_count;
    uint32_t crc_ok;
    double sync_score;
    double frequency_offset_hz;
    double symbol_confidence_mean;
    double symbol_confidence_min;
    double pre_fec_ber_est;
    double snr_estimate_db;
    double snr_2500hz_estimate_db;
    double rewind_seconds;
    double mid_score;
} mu16tty_rx_metrics;

typedef struct mu16tty_config {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t sample_rate;
    uint32_t enabled_rate_mask;
    uint32_t event_capacity;
    uint32_t reserved;
} mu16tty_config;

typedef struct mu16tty_event {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t event_type;
    uint32_t symbol_rate;
    uint64_t sequence;
    uint32_t payload_size;
    uint32_t reserved;
    double frequency_hz;
    double score;
    mu16tty_rx_metrics metrics;
    uint8_t payload[MU16TTY_MAX_PACKET_BYTES];
} mu16tty_event;

typedef struct mu16tty_stats {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t samples_consumed;
    uint64_t pushes;
    uint64_t events_dropped;
    uint64_t ring_capacity;
    uint64_t ring_used;
    uint64_t allocation_count;
    float peak_abs;
    uint32_t reserved;
} mu16tty_stats;

MU16TTY_API int mu16tty_create(
    const mu16tty_config *config,
    mu16tty_context **out_context
);
MU16TTY_API void mu16tty_destroy(mu16tty_context *context);
MU16TTY_API int mu16tty_reset(mu16tty_context *context);
MU16TTY_API int mu16tty_push_pcm_f32(
    mu16tty_context *context,
    const float *samples,
    size_t sample_count
);
MU16TTY_API int mu16tty_poll_event(
    mu16tty_context *context,
    mu16tty_event *event
);
MU16TTY_API int mu16tty_get_stats(
    const mu16tty_context *context,
    mu16tty_stats *stats
);

/* With a NULL output, return the required element count. Otherwise return
 * the number written, or a negative mu16tty_result on invalid input. */
/* The message is already encoded as UTF-8. rate_mode is the wire value
 * 0 through 3 for 40, 20, 10, and 4 symbols/s respectively. */
MU16TTY_API int mu16tty_pack_chat_packet(
    uint8_t rate_mode, uint64_t session_id, uint32_t sequence, uint32_t timestamp,
    const uint8_t *callsign, size_t callsign_size,
    const uint8_t *message, size_t message_size,
    uint8_t *output, size_t capacity
);

typedef struct mu16tty_chat_packet_fields {
    uint64_t session_id;
    uint32_t sequence;
    uint32_t timestamp;
    uint32_t rate_mode;
    uint32_t callsign_offset;
    uint32_t callsign_size;
    uint32_t message_size;
} mu16tty_chat_packet_fields;

/* Check header, exact length, and CRC. Text remains in the original buffer. */
MU16TTY_API int mu16tty_unpack_chat_packet(
    const uint8_t *packet, size_t packet_size, mu16tty_chat_packet_fields *fields
);
/* Check the packet layout without requiring a valid CRC. For diagnostics only. */
MU16TTY_API int mu16tty_inspect_chat_packet(
    const uint8_t *packet, size_t packet_size, mu16tty_chat_packet_fields *fields
);
MU16TTY_API int mu16tty_encode_tx_symbols(
    const uint8_t *packet, size_t packet_size, uint8_t *output, size_t capacity
);
MU16TTY_API int mu16tty_render_tx_pcm_f32(
    const uint8_t *symbols, size_t symbol_count, uint32_t sample_rate,
    uint32_t symbol_rate, size_t wake_samples, float *output, size_t capacity
);

typedef struct mu16tty_resampler mu16tty_resampler;
MU16TTY_API int mu16tty_resampler_create(
    uint32_t input_rate, uint32_t output_rate, mu16tty_resampler **out
);
MU16TTY_API void mu16tty_resampler_destroy(mu16tty_resampler *resampler);
MU16TTY_API void mu16tty_resampler_reset(mu16tty_resampler *resampler);
/* Return samples written. Too little output capacity leaves state intact. */
MU16TTY_API int mu16tty_resampler_process(
    mu16tty_resampler *resampler, const float *input, size_t input_count,
    float *output, size_t capacity
);

/* Compute RMS/peak dBFS, a clipped waveform and a 0 to 3000 Hz spectrum. */
MU16TTY_API int mu16tty_analyze_audio_monitor(
    const float *samples, size_t count, uint32_t sample_rate, double levels[2],
    double *waveform, size_t waveform_points, double *spectrum, size_t spectrum_bins
);

#ifdef __cplusplus
}
#endif

#endif
