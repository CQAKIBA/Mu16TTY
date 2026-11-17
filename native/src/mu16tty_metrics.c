#include "mu16tty_internal.h"

#include <float.h>
#include <math.h>
#include <string.h>

void mu16tty_metrics_reset(mu16tty_metric_workspace *workspace) {
    if (workspace == NULL) {
        return;
    }
    memset(workspace, 0, sizeof(*workspace));
    workspace->metrics.abi_version = MU16TTY_ABI_VERSION;
    workspace->metrics.struct_size = (uint32_t)sizeof(workspace->metrics);
    workspace->confidence_min = DBL_MAX;
}

void mu16tty_metrics_set_head(
    mu16tty_metric_workspace *workspace,
    uint32_t sync_matches,
    uint32_t sync_symbols,
    double sync_score,
    double frequency_offset_hz
) {
    if (workspace == NULL) {
        return;
    }
    workspace->metrics.valid_fields |=
        MU16TTY_METRIC_PATH | MU16TTY_METRIC_SYNC | MU16TTY_METRIC_FREQUENCY |
        MU16TTY_METRIC_REFINEMENT;
    workspace->metrics.decode_path = MU16TTY_DECODE_PATH_HEAD;
    workspace->metrics.sync_matches = sync_matches;
    workspace->metrics.sync_symbols = sync_symbols;
    workspace->metrics.sync_score = sync_score;
    workspace->metrics.frequency_offset_hz = frequency_offset_hz;
    workspace->metrics.refinement_invocations = 0u;
    workspace->metrics.refinement_improvements = 0u;
}

void mu16tty_metrics_set_mid(
    mu16tty_metric_workspace *workspace,
    double frequency_offset_hz,
    double rewind_seconds,
    double mid_score,
    uint32_t marker_count
) {
    if (workspace == NULL) {
        return;
    }
    workspace->metrics.valid_fields |=
        MU16TTY_METRIC_PATH | MU16TTY_METRIC_FREQUENCY | MU16TTY_METRIC_MID |
        MU16TTY_METRIC_REFINEMENT;
    workspace->metrics.decode_path = MU16TTY_DECODE_PATH_MID;
    workspace->metrics.frequency_offset_hz = frequency_offset_hz;
    workspace->metrics.rewind_seconds = rewind_seconds;
    workspace->metrics.mid_score = mid_score;
    workspace->metrics.mid_marker_count = marker_count;
    workspace->metrics.refinement_invocations = 0u;
    workspace->metrics.refinement_improvements = 0u;
}

void mu16tty_metrics_observe_symbol(
    mu16tty_metric_workspace *workspace,
    const double likelihoods[16]
) {
    double strongest = 0.0;
    double runner_up = 0.0;
    double confidence;
    size_t index;
    if (workspace == NULL || likelihoods == NULL) {
        return;
    }
    for (index = 0u; index < 16u; ++index) {
        const double value = likelihoods[index];
        if (value >= strongest) {
            runner_up = strongest;
            strongest = value;
        } else if (value > runner_up) {
            runner_up = value;
        }
    }
    confidence = strongest > DBL_MIN ? (strongest - runner_up) / strongest : 0.0;
    workspace->confidence_sum += confidence;
    if (confidence < workspace->confidence_min) {
        workspace->confidence_min = confidence;
    }
    workspace->confidence_count += 1u;
    if (workspace->observation_count >= MU16TTY_MAX_METRIC_SYMBOLS) {
        return;
    }
    for (index = 0u; index < 16u; ++index) {
        double observed = likelihoods[index];
        if (observed < 0.0 || !isfinite(observed)) observed = 0.0;
        workspace->tone_likelihoods[workspace->observation_count][index] =
            (float)observed;
    }
    workspace->observation_count += 1u;
}

void mu16tty_metrics_finish(
    const mu16tty_metric_workspace *workspace,
    mu16tty_rx_metrics *metrics
) {
    if (workspace == NULL || metrics == NULL) {
        return;
    }
    *metrics = workspace->metrics;
    if (workspace->confidence_count != 0u) {
        metrics->valid_fields |= MU16TTY_METRIC_CONFIDENCE;
        metrics->symbol_confidence_mean =
            workspace->confidence_sum / (double)workspace->confidence_count;
        metrics->symbol_confidence_min = workspace->confidence_min;
    }
}

static uint8_t gray_to_tone(uint8_t gray) {
    uint8_t tone = gray;
    tone ^= (uint8_t)(tone >> 1u);
    tone ^= (uint8_t)(tone >> 2u);
    return (uint8_t)(tone & 0x0Fu);
}

static uint32_t count_nibble_bits(uint8_t value) {
    uint32_t count = 0u;
    value &= 0x0Fu;
    while (value != 0u) {
        count += (uint32_t)(value & 1u);
        value >>= 1u;
    }
    return count;
}

int mu16tty_metrics_finish_packet(
    mu16tty_metric_workspace *workspace,
    const uint8_t *packet,
    size_t packet_size,
    uint32_t symbol_rate,
    int crc_ok
) {
    uint8_t transmitted[5200];
    int header_count;
    int body_count;
    size_t total_bits;
    size_t symbol_count;
    size_t symbol;
    uint32_t bit_errors = 0u;
    double signal_energy = 0.0;
    double other_energy = 0.0;
    if (workspace == NULL || packet == NULL || packet_size < 24u || symbol_rate == 0u) {
        return -1;
    }
    header_count = mu16tty_encode_segment(packet, 24u, transmitted, 400u);
    body_count = mu16tty_encode_segment(
        packet + 24u, packet_size - 24u, transmitted + 400u, 4800u
    );
    if (header_count != 400 || body_count < 0) {
        return -1;
    }
    total_bits = 400u + (size_t)body_count;
    symbol_count = total_bits / 4u;
    if (symbol_count > workspace->observation_count) {
        return -1;
    }
    for (symbol = 0u; symbol < symbol_count; ++symbol) {
        uint8_t expected_label = 0u;
        uint8_t expected_tone;
        uint8_t strongest_tone = 0u;
        float strongest = 0.0f;
        double total = 0.0;
        size_t bit;
        size_t tone;
        for (bit = 0u; bit < 4u; ++bit) {
            expected_label = (uint8_t)(
                (expected_label << 1u) | transmitted[symbol * 4u + bit]
            );
        }
        expected_tone = gray_to_tone(expected_label);
        for (tone = 0u; tone < 16u; ++tone) {
            const float value = workspace->tone_likelihoods[symbol][tone];
            total += (double)value;
            if (value > strongest) {
                strongest = value;
                strongest_tone = (uint8_t)tone;
            }
        }
        bit_errors += count_nibble_bits(
            (uint8_t)(expected_label ^ (strongest_tone ^ (strongest_tone >> 1u)))
        );
        signal_energy += (double)workspace->tone_likelihoods[symbol][expected_tone];
        other_energy += total - (double)workspace->tone_likelihoods[symbol][expected_tone];
    }
    workspace->metrics.valid_fields |= MU16TTY_METRIC_BER_FEC | MU16TTY_METRIC_CRC;
    workspace->metrics.pre_fec_bit_errors_est = bit_errors;
    workspace->metrics.pre_fec_bits = (uint32_t)total_bits;
    workspace->metrics.pre_fec_ber_est = (double)bit_errors / (double)total_bits;
    workspace->metrics.crc_ok = crc_ok ? 1u : 0u;
    if (signal_energy > 0.0 && other_energy > 0.0) {
        const double snr = 10.0 * log10(signal_energy / (other_energy / 15.0));
        workspace->metrics.valid_fields |= MU16TTY_METRIC_SNR;
        workspace->metrics.snr_estimate_db = snr;
        workspace->metrics.snr_2500hz_estimate_db =
            snr - 10.0 * log10(2500.0 / (1.5 * (double)symbol_rate));
    }
    return 0;
}
