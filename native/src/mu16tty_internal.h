#ifndef MU16TTY_INTERNAL_H
#define MU16TTY_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "mu16tty_modem.h"

#define MU16TTY_CONSTRAINT_LENGTH 7u
#define MU16TTY_TAIL_BITS 6u
#define MU16TTY_STATE_COUNT 64u
#define MU16TTY_MAX_METRIC_SYMBOLS 1300u
#define MU16TTY_MID_CANDIDATE_CAPACITY 32u
#define MU16TTY_MID_MAX_ATTEMPTS 12u

typedef struct mu16tty_mid_candidate {
    uint64_t start_sample;
    double score;
    double frequency_offset_hz;
    uint32_t marker_count;
    uint8_t promoted;
    uint8_t attempted;
} mu16tty_mid_candidate;

typedef struct mu16tty_mid_tracker {
    mu16tty_mid_candidate candidates[MU16TTY_MID_CANDIDATE_CAPACITY];
    uint32_t candidate_count;
    uint32_t recovery_attempts;
    uint64_t cooldown_until;
} mu16tty_mid_tracker;

void mu16tty_mid_reset(mu16tty_mid_tracker *tracker);
uint32_t mu16tty_mid_observe(
    mu16tty_mid_tracker *tracker,
    uint64_t start_sample,
    double score,
    double frequency_offset_hz,
    uint64_t interval_samples
);
int mu16tty_mid_take_recovery_candidate(
    mu16tty_mid_tracker *tracker,
    uint64_t now_sample,
    uint64_t cooldown_samples,
    mu16tty_mid_candidate *candidate
);

typedef struct mu16tty_metric_workspace {
    mu16tty_rx_metrics metrics;
    double confidence_sum;
    double confidence_min;
    uint32_t confidence_count;
    uint32_t observation_count;
    float tone_likelihoods[MU16TTY_MAX_METRIC_SYMBOLS][16];
} mu16tty_metric_workspace;

void mu16tty_metrics_reset(mu16tty_metric_workspace *workspace);
void mu16tty_metrics_set_head(
    mu16tty_metric_workspace *workspace,
    uint32_t sync_matches,
    uint32_t sync_symbols,
    double sync_score,
    double frequency_offset_hz
);
void mu16tty_metrics_set_mid(
    mu16tty_metric_workspace *workspace,
    double frequency_offset_hz,
    double rewind_seconds,
    double mid_score,
    uint32_t marker_count
);
void mu16tty_metrics_observe_symbol(
    mu16tty_metric_workspace *workspace,
    const double likelihoods[16]
);
void mu16tty_metrics_finish(
    const mu16tty_metric_workspace *workspace,
    mu16tty_rx_metrics *metrics
);
int mu16tty_metrics_finish_packet(
    mu16tty_metric_workspace *workspace,
    const uint8_t *packet,
    size_t packet_size,
    uint32_t symbol_rate,
    int crc_ok
);
int mu16tty_encode_segment(
    const uint8_t *data,
    size_t data_size,
    uint8_t *output,
    size_t output_capacity
);

uint32_t mu16tty_crc32c(const uint8_t *data, size_t length);
int mu16tty_scramble_bits(const uint8_t *input, size_t count, uint8_t *output);
int mu16tty_interleave_bits(
    const uint8_t *input,
    size_t count,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
);
int mu16tty_deinterleave_bits(
    const uint8_t *input,
    size_t input_count,
    size_t original_length,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
);
int mu16tty_interleave_blocks(
    const uint8_t *input,
    size_t count,
    size_t block_size,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
);
int mu16tty_deinterleave_blocks(
    const uint8_t *input,
    size_t input_count,
    size_t original_length,
    size_t block_size,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
);
int mu16tty_convolutional_encode(
    const uint8_t *input,
    size_t count,
    uint8_t *output,
    size_t output_capacity
);
int mu16tty_viterbi_prefix(
    const double *llrs, size_t llr_count, uint8_t *output, size_t output_capacity
);
int mu16tty_viterbi_decode(
    const double *llrs,
    size_t llr_count,
    size_t expected_bits,
    uint8_t *output,
    size_t output_capacity
);
int mu16tty_validate_packet_header(const uint8_t *data, size_t length);

#endif
