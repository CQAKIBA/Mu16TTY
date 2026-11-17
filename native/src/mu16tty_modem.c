#include "mu16tty_modem.h"
#include "mu16tty_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct mu16tty_context {
    uint32_t sample_rate;
    uint32_t enabled_rate_mask;
    uint32_t event_capacity;
    float *ring;
    size_t ring_capacity;
    size_t ring_used;
    size_t ring_write;
    uint64_t allocation_count;
    mu16tty_event *events;
    size_t event_head;
    size_t event_count;
    uint64_t next_analysis_at;
    unsigned int wake_hits;
    double wake_frequency;
    uint64_t total_samples_written;
    uint64_t sync_search_until;
    uint64_t next_sync_scan[4];
    uint64_t last_sync_event[4];
    uint64_t pending_sync_start[4];
    double pending_sync_score[4];
    uint8_t pending_sync[4];
    uint64_t next_mid_scan[4];
    uint64_t last_mid_event[4];
    uint8_t mid_event_seen[4];
    uint64_t candidate_sync_start[4];
    uint8_t candidate_active[4];
    uint64_t candidate_payload_start[4];
    uint32_t candidate_body_bytes[4];
    uint32_t candidate_body_bits[4];
    uint32_t candidate_physical_index[4];
    uint32_t candidate_body_llr_count[4];
    double candidate_body_llrs[4][4800];
    uint8_t candidate_header[4][24];
    double candidate_confidence[4];
    mu16tty_metric_workspace candidate_metrics[4];
    mu16tty_mid_tracker mid_trackers[4];
    mu16tty_stats stats;
};

static const double MU16TTY_PI = 3.14159265358979323846;
static const unsigned int MU16TTY_RATES[4] = {40u, 20u, 10u, 4u};
static const uint8_t MU16TTY_SYNC_SYMBOLS[12] = {15u, 0u, 12u, 3u, 10u, 5u, 9u, 6u, 1u, 14u, 2u, 13u};
static const uint8_t MU16TTY_MID_SYMBOLS[4] = {16u, 15u, 0u, 10u};
static const int MU16TTY_WHITE_KEY_SEMITONES[16] = {2, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19, 20, 22, 24, 26, 27};

static double symbol_frequency(uint8_t tone);
static void try_decode_header(mu16tty_context *context, size_t rate_index);

static void queue_event(mu16tty_context *context, const mu16tty_event *event) {
    size_t index;
    if (context->event_capacity == 0u || context->event_count >= context->event_capacity) {
        context->stats.events_dropped += 1u;
        return;
    }
    index = (context->event_head + context->event_count) % context->event_capacity;
    context->events[index] = *event;
    context->event_count += 1u;
}

static float recent_sample(const mu16tty_context *context, size_t window, size_t index) {
    const size_t start = (context->ring_write + context->ring_capacity - window) % context->ring_capacity;
    return context->ring[(start + index) % context->ring_capacity];
}

static float absolute_sample(const mu16tty_context *context, uint64_t sample_index) {
    const uint64_t oldest = context->total_samples_written - (uint64_t)context->ring_used;
    const size_t oldest_position = (context->ring_write + context->ring_capacity - context->ring_used) % context->ring_capacity;
    const size_t offset = (size_t)(sample_index - oldest);
    return context->ring[(oldest_position + offset) % context->ring_capacity];
}

static double tone_segment_score(
    const mu16tty_context *context,
    uint64_t start,
    size_t count,
    double frequency
) {
    const double coefficient = 2.0 * cos((2.0 * MU16TTY_PI * frequency) / (double)context->sample_rate);
    double previous = 0.0;
    double previous2 = 0.0;
    double energy = 0.0;
    size_t index;
    if (count < 2u) {
        return 0.0;
    }
    for (index = 0u; index < count; ++index) {
        const double value = (double)absolute_sample(
            context, start + (uint64_t)index
        );
        const double current = value + coefficient * previous - previous2;
        energy += value * value;
        previous2 = previous;
        previous = current;
    }
    if (energy <= 1.0e-20) {
        return 0.0;
    }
    return (2.0 * (previous * previous + previous2 * previous2 - coefficient * previous * previous2)) / ((double)count * energy);
}

static double sync_score_at(
    const mu16tty_context *context,
    uint64_t start,
    size_t samples_per_symbol,
    double frequency_offset_hz
) {
    double total = 0.0;
    size_t symbol;
    for (symbol = 0u; symbol < 12u; ++symbol) {
        const uint8_t tone = MU16TTY_SYNC_SYMBOLS[symbol];
        const double frequency = 440.0 * pow(2.0, (double)MU16TTY_WHITE_KEY_SEMITONES[tone] / 12.0) + frequency_offset_hz;
        total += tone_segment_score(
            context,
            start + (uint64_t)(symbol * samples_per_symbol),
            samples_per_symbol,
            frequency
        );
    }
    return total / 12.0;
}

static uint32_t sync_rank_at(
    const mu16tty_context *context,
    uint64_t start,
    size_t samples_per_symbol,
    double frequency_offset_hz,
    double *relative_score
) {
    uint32_t matches = 0u;
    double score_sum = 0.0;
    size_t symbol;
    for (symbol = 0u; symbol < 12u; ++symbol) {
        double best_score = -1.0;
        uint8_t best_tone = 0u;
        uint8_t tone;
        double tone_sum = 0.0;
        double expected_score = 0.0;
        for (tone = 0u; tone < 16u; ++tone) {
            const double score = tone_segment_score(
                context,
                start + (uint64_t)(symbol * samples_per_symbol),
                samples_per_symbol,
                symbol_frequency(tone) + frequency_offset_hz
            );
            tone_sum += score;
            if (tone == MU16TTY_SYNC_SYMBOLS[symbol]) {
                expected_score = score;
            }
            if (score > best_score) {
                best_score = score;
                best_tone = tone;
            }
        }
        if (best_tone == MU16TTY_SYNC_SYMBOLS[symbol]) {
            matches += 1u;
        }
        if (tone_sum > DBL_MIN) {
            score_sum += expected_score / tone_sum;
        }
    }
    if (relative_score != NULL) {
        *relative_score = score_sum / 12.0;
    }
    return matches;
}

static void analyze_head_sync(mu16tty_context *context, size_t rate_index) {
    const unsigned int rate = MU16TTY_RATES[rate_index];
    const size_t samples_per_symbol = (size_t)context->sample_rate / rate;
    const size_t pattern_samples = samples_per_symbol * 12u;
    const uint64_t end = context->total_samples_written;
    const uint64_t oldest = end - (uint64_t)context->ring_used;
    uint64_t start;
    double total;
    double best_offset = 0.0;
    double best_score = -1.0;
    double best_rank_score = -1.0;
    uint32_t best_matches = 0u;
    double soft_score = -1.0;
    double soft_offset = 0.0;
    uint32_t soft_matches = 0u;
    int offset;
    int64_t timing_delta;
    const int64_t timing_step = (int64_t)(samples_per_symbol / 20u) > 0
        ? (int64_t)(samples_per_symbol / 20u)
        : 1;
    if (context->candidate_active[rate_index] != 0u ||
        (context->enabled_rate_mask & (1u << rate_index)) == 0u ||
        pattern_samples > context->ring_used || end < pattern_samples) {
        return;
    }
    start = end - (uint64_t)pattern_samples;
    if (start < oldest) {
        return;
    }
    total = sync_score_at(context, start, samples_per_symbol, 0.0);
    if (total >= 0.12) {
        if (!context->pending_sync[rate_index] ||
            total > context->pending_sync_score[rate_index]) {
            context->pending_sync[rate_index] = 1u;
            context->pending_sync_start[rate_index] = start;
            context->pending_sync_score[rate_index] = total;
            return;
        }
    }
    if (context->pending_sync[rate_index]) {
        start = context->pending_sync_start[rate_index];
        total = context->pending_sync_score[rate_index];
        context->pending_sync[rate_index] = 0u;
    } else {
        return;
    }
    {
        double prefilter_score = 0.0;
        const uint32_t prefilter_matches = sync_rank_at(
            context, start, samples_per_symbol, 0.0, &prefilter_score
        );
        /* Random payload tones often pass the deliberately permissive
         * energy gate.  Reject them before the expensive timing and
         * frequency sweeps; a real weak HEAD only needs a minority of
         * its twelve positions to rank first at the coarse offset. */
        if (prefilter_matches < 4u) {
            return;
        }
    }
    if (context->last_sync_event[rate_index] == 0u ||
        start - context->last_sync_event[rate_index] >= (uint64_t)pattern_samples) {
        const uint64_t coarse_start = start;
        double timing_score = -1.0;
        for (
            timing_delta = -(int64_t)(samples_per_symbol / 2u);
            timing_delta <= (int64_t)(samples_per_symbol / 2u);
            timing_delta += timing_step
        ) {
            const int64_t candidate_start = (int64_t)coarse_start + timing_delta;
            double score;
            if (candidate_start < (int64_t)oldest ||
                (uint64_t)candidate_start + (uint64_t)pattern_samples > end) {
                continue;
            }
            score = sync_score_at(
                context, (uint64_t)candidate_start, samples_per_symbol, 0.0
            );
            if (score > timing_score) {
                timing_score = score;
                start = (uint64_t)candidate_start;
            }
        }
        mu16tty_event event;
        memset(&event, 0, sizeof(event));
        event.abi_version = MU16TTY_ABI_VERSION;
        event.struct_size = (uint32_t)sizeof(event);
        event.event_type = MU16TTY_EVENT_HEAD_SYNC;
        event.symbol_rate = rate;
        event.sequence = start;
        event.score = total;
        for (offset = -20; offset <= 20; ++offset) {
            double rank_score = 0.0;
            double adjusted_rank_score;
            const uint32_t matches = sync_rank_at(
                context,
                start,
                samples_per_symbol,
                (double)offset,
                &rank_score
            );
            /* Prefer the smallest correction when broad, clean peaks are
             * effectively tied.  Real offsets still win by tone evidence. */
            adjusted_rank_score = rank_score -
                (rate <= 20u ? 0.001 * fabs((double)offset) : 0.0);
            if (matches >= 6u && adjusted_rank_score > soft_score) {
                soft_score = adjusted_rank_score;
                soft_offset = (double)offset;
                soft_matches = matches;
            }
            if (matches > best_matches ||
                (matches == best_matches && adjusted_rank_score > best_rank_score)) {
                best_matches = matches;
                best_rank_score = adjusted_rank_score;
                best_offset = (double)offset;
            }
        }
        if (best_matches < 8u) {
            /* A narrow loss at several positions can hide a real HEAD.
             * Require energy across the pattern before trying the header. */
            if (soft_score < 0.33) {
                return;
            }
            best_offset = soft_offset;
            best_matches = soft_matches;
        }
        best_score = sync_score_at(
            context, start, samples_per_symbol, best_offset
        );
        event.score = best_score;
        queue_event(context, &event);
        context->last_sync_event[rate_index] = start;
        context->candidate_sync_start[rate_index] = start;
        context->candidate_active[rate_index] = 1u;
        mu16tty_metrics_reset(&context->candidate_metrics[rate_index]);
        mu16tty_metrics_set_head(
            &context->candidate_metrics[rate_index],
            best_matches,
            12u,
            best_score,
            best_offset
        );
    }
}

static void try_start_mid_recovery(mu16tty_context *context, size_t rate_index) {
    const unsigned int rate = MU16TTY_RATES[rate_index];
    const uint64_t samples_per_symbol = (uint64_t)context->sample_rate / rate;
    const uint64_t oldest = context->total_samples_written - (uint64_t)context->ring_used;
    mu16tty_mid_candidate candidate;
    uint64_t marker_ordinal;
    if (context->candidate_active[rate_index] != 0u ||
        !mu16tty_mid_take_recovery_candidate(
            &context->mid_trackers[rate_index],
            context->total_samples_written,
            (uint64_t)context->sample_rate * 15u,
            &candidate
        )) {
        return;
    }
    for (marker_ordinal = 0u; marker_ordinal < 32u; ++marker_ordinal) {
        const uint64_t logical_before_marker = 100u + marker_ordinal * 256u;
        const uint64_t physical_before_marker = logical_before_marker + marker_ordinal * 4u;
        const uint64_t rewind_symbols = physical_before_marker + 12u;
        uint64_t header_start;
        if (candidate.start_sample < physical_before_marker * samples_per_symbol) {
            break;
        }
        header_start = candidate.start_sample - physical_before_marker * samples_per_symbol;
        if (header_start < 12u * samples_per_symbol ||
            header_start - 12u * samples_per_symbol < oldest) {
            continue;
        }
        context->candidate_sync_start[rate_index] =
            header_start - 12u * samples_per_symbol;
        context->candidate_active[rate_index] = 1u;
        mu16tty_metrics_reset(&context->candidate_metrics[rate_index]);
        mu16tty_metrics_set_mid(
            &context->candidate_metrics[rate_index],
            candidate.frequency_offset_hz,
            (double)(rewind_symbols * samples_per_symbol) /
                (double)context->sample_rate,
            candidate.score,
            candidate.marker_count
        );
        try_decode_header(context, rate_index);
        if (context->candidate_active[rate_index] == 2u) {
            return;
        }
    }
    context->candidate_active[rate_index] = 0u;
}

static int deinterleave_header_soft(const double input[400], double output[396]) {
    size_t input_start = 0u;
    size_t output_start = 0u;
    size_t remaining = 396u;
    while (remaining != 0u) {
        const size_t chunk = remaining < 256u ? remaining : 256u;
        const size_t encoded = ((chunk + 15u) / 16u) * 16u;
        const size_t rows = encoded / 16u;
        size_t index;
        for (index = 0u; index < chunk; ++index) {
            const size_t row = index / 16u;
            const size_t column = index % 16u;
            output[output_start + index] = input[input_start + column * rows + row];
        }
        input_start += encoded;
        output_start += chunk;
        remaining -= chunk;
    }
    return input_start == 400u ? 0 : -1;
}

static size_t transmitted_length(size_t coded_length) {
    const size_t full = coded_length / 256u;
    const size_t tail = coded_length % 256u;
    return full * 256u + (tail == 0u ? 0u : ((tail + 15u) / 16u) * 16u);
}

static double demodulate_symbol_llrs(
    const mu16tty_context *context,
    uint64_t start,
    size_t samples_per_symbol,
    double frequency_offset_hz,
    double output[4],
    double observed_likelihoods[16]
) {
    double likelihoods[16];
    double maximum = 0.0;
    size_t tone;
    size_t bit_index;
    for (tone = 0u; tone < 16u; ++tone) {
        likelihoods[tone] = tone_segment_score(
            context,
            start,
            samples_per_symbol,
            symbol_frequency((uint8_t)tone) + frequency_offset_hz
        );
        if (likelihoods[tone] > maximum) maximum = likelihoods[tone];
    }
    if (observed_likelihoods != NULL) {
        memcpy(observed_likelihoods, likelihoods, sizeof(likelihoods));
    }
    for (bit_index = 0u; bit_index < 4u; ++bit_index) {
        const unsigned int shift = 3u - (unsigned int)bit_index;
        double zero = DBL_MIN;
        double one = DBL_MIN;
        for (tone = 0u; tone < 16u; ++tone) {
            const unsigned int label = (unsigned int)tone ^ ((unsigned int)tone >> 1u);
            if (((label >> shift) & 1u) == 0u) {
                if (likelihoods[tone] > zero) zero = likelihoods[tone];
            } else if (likelihoods[tone] > one) {
                one = likelihoods[tone];
            }
        }
        output[bit_index] = log(zero) - log(one);
    }
    return maximum;
}

static void report_head_rejection(
    mu16tty_context *context, size_t rate_index, mu16tty_head_rejection reason
) {
    mu16tty_event event;
    /* MID probes try several positions internally. Only HEAD has announced HEADER. */
    if (context->candidate_metrics[rate_index].metrics.decode_path != MU16TTY_DECODE_PATH_HEAD) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.abi_version = MU16TTY_ABI_VERSION;
    event.struct_size = (uint32_t)sizeof(event);
    event.event_type = MU16TTY_EVENT_HEAD_REJECTED;
    event.symbol_rate = MU16TTY_RATES[rate_index];
    event.sequence = context->candidate_sync_start[rate_index];
    event.payload_size = 1u;
    event.payload[0] = (uint8_t)reason;
    queue_event(context, &event);
}

static void try_decode_header(mu16tty_context *context, size_t rate_index) {
    const unsigned int rate = MU16TTY_RATES[rate_index];
    const size_t samples_per_symbol = (size_t)context->sample_rate / rate;
    const uint64_t oldest = context->total_samples_written - (uint64_t)context->ring_used;
    uint64_t sync_start = context->candidate_sync_start[rate_index];
    uint64_t payload_start;
    double transmitted[400];
    double ordered[396];
    uint8_t scrambled[192];
    uint8_t bits[192];
    uint8_t header[24];
    double confidence = 0.0;
    size_t symbol;
    size_t bit_index;
    double likelihoods[16];
    const double frequency_offset_hz =
        context->candidate_metrics[rate_index].metrics.frequency_offset_hz;

    if (context->candidate_active[rate_index] != 1u) {
        return;
    }
    if (sync_start < oldest) {
        context->candidate_active[rate_index] = 0u;
        report_head_rejection(context, rate_index, MU16TTY_HEAD_EXPIRED);
        return;
    }
    payload_start = sync_start + (uint64_t)(12u * samples_per_symbol);
    if (context->total_samples_written < payload_start + (uint64_t)(100u * samples_per_symbol)) {
        return;
    }

    for (symbol = 0u; symbol < 100u; ++symbol) {
        confidence += demodulate_symbol_llrs(
            context,
            payload_start + (uint64_t)(symbol * samples_per_symbol),
            samples_per_symbol,
            frequency_offset_hz,
            &transmitted[symbol * 4u],
            likelihoods
        );
        mu16tty_metrics_observe_symbol(
            &context->candidate_metrics[rate_index], likelihoods
        );
    }
    context->candidate_active[rate_index] = 0u;
    if (deinterleave_header_soft(transmitted, ordered) != 0 ||
        mu16tty_viterbi_decode(ordered, 396u, 192u, scrambled, 192u) != 192 ||
        mu16tty_scramble_bits(scrambled, 192u, bits) != 0) {
        report_head_rejection(context, rate_index, MU16TTY_HEAD_DECODE_FAILED);
        return;
    }
    memset(header, 0, sizeof(header));
    for (bit_index = 0u; bit_index < 192u; ++bit_index) {
        header[bit_index / 8u] |= (uint8_t)(bits[bit_index] << (7u - bit_index % 8u));
    }
    if (mu16tty_validate_packet_header(header, sizeof(header)) != 0) {
        report_head_rejection(context, rate_index, MU16TTY_HEAD_INVALID);
        return;
    }
    if (header[1] != rate_index) {
        report_head_rejection(context, rate_index, MU16TTY_HEAD_MODE_MISMATCH);
        return;
    }
    {
        const size_t message_length = ((size_t)header[3] << 8u) | header[4];
        const size_t body_bytes = (size_t)header[5] + message_length + 4u;
        const size_t body_coded = 2u * (body_bytes * 8u + 6u);
        mu16tty_event event;
        memset(&event, 0, sizeof(event));
        event.abi_version = MU16TTY_ABI_VERSION;
        event.struct_size = (uint32_t)sizeof(event);
        event.event_type = MU16TTY_EVENT_HEADER;
        event.symbol_rate = rate;
        event.sequence = payload_start;
        event.payload_size = (uint32_t)((400u + transmitted_length(body_coded)) / 4u);
        event.score = confidence / 100.0;
        mu16tty_metrics_finish(&context->candidate_metrics[rate_index], &event.metrics);
        queue_event(context, &event);
        memcpy(context->candidate_header[rate_index], header, sizeof(header));
        context->candidate_payload_start[rate_index] = payload_start;
        context->candidate_body_bytes[rate_index] = (uint32_t)body_bytes;
        context->candidate_body_bits[rate_index] = (uint32_t)transmitted_length(body_coded);
        context->candidate_physical_index[rate_index] = 100u;
        context->candidate_body_llr_count[rate_index] = 0u;
        context->candidate_confidence[rate_index] = confidence;
        context->candidate_active[rate_index] = 2u;
    }
}

static void queue_body_preview(mu16tty_context *context, size_t rate_index) {
    const size_t count = context->candidate_body_llr_count[rate_index];
    const size_t bit_count = count / 2u;
    const size_t callsign_bytes = context->candidate_header[rate_index][5];
    const size_t text_end = context->candidate_body_bytes[rate_index] - 4u;
    /* Leave four bytes of traceback margin at the unconfirmed end. */
    const size_t stable_bytes = bit_count / 8u - 4u;
    const size_t end = stable_bytes < text_end ? stable_bytes : text_end;
    double ordered[4768];
    uint8_t scrambled[2368];
    uint8_t bits[2368];
    size_t index;
    mu16tty_event *pending = NULL;
    mu16tty_event event;
    if (end <= callsign_bytes) return;
    for (index = 0u; index < context->event_count; ++index) {
        mu16tty_event *queued = &context->events[
            (context->event_head + index) % context->event_capacity
        ];
        if (queued->event_type == MU16TTY_EVENT_PREVIEW &&
            queued->symbol_rate == MU16TTY_RATES[rate_index] &&
            queued->sequence == context->candidate_payload_start[rate_index]) {
            pending = queued;
            break;
        }
    }
    /* Preview is optional. Leave room for the packet and status events. */
    if (pending == NULL && context->event_count + 2u >= context->event_capacity) return;
    for (index = 0u; index < count; ++index) {
        const size_t in_block = index % 256u;
        ordered[index] = context->candidate_body_llrs[rate_index][
            index - in_block + (in_block % 16u) * 16u + in_block / 16u
        ];
    }
    if (mu16tty_viterbi_prefix(ordered, count, scrambled, sizeof(scrambled)) != (int)bit_count ||
        mu16tty_scramble_bits(scrambled, bit_count, bits) != 0) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.abi_version = MU16TTY_ABI_VERSION;
    event.struct_size = (uint32_t)sizeof(event);
    event.event_type = MU16TTY_EVENT_PREVIEW;
    event.symbol_rate = MU16TTY_RATES[rate_index];
    event.sequence = context->candidate_payload_start[rate_index];
    event.payload_size = (uint32_t)(end - callsign_bytes);
    for (index = callsign_bytes * 8u; index < end * 8u; ++index) {
        event.payload[index / 8u - callsign_bytes] |= (uint8_t)(bits[index] << (7u - index % 8u));
    }
    if (pending != NULL) {
        *pending = event;
    } else {
        queue_event(context, &event);
    }
}

static void finish_body(mu16tty_context *context, size_t rate_index) {
    const size_t body_bytes = context->candidate_body_bytes[rate_index];
    const size_t information_bits = body_bytes * 8u;
    const size_t coded_bits = 2u * (information_bits + 6u);
    const size_t transmitted_bits = context->candidate_body_bits[rate_index];
    double ordered[4768];
    uint8_t scrambled[2368];
    uint8_t bits[2368];
    uint8_t body[296];
    uint8_t packet[MU16TTY_MAX_PACKET_BYTES];
    size_t input_start = 0u;
    size_t output_start = 0u;
    size_t remaining = coded_bits;
    size_t bit_index;
    uint32_t received_crc;
    uint32_t calculated_crc;
    const size_t packet_size = 24u + body_bytes;

    while (remaining != 0u) {
        const size_t chunk = remaining < 256u ? remaining : 256u;
        const size_t encoded = ((chunk + 15u) / 16u) * 16u;
        const size_t rows = encoded / 16u;
        size_t index;
        for (index = 0u; index < chunk; ++index) {
            const size_t row = index / 16u;
            const size_t column = index % 16u;
            ordered[output_start + index] =
                context->candidate_body_llrs[rate_index][input_start + column * rows + row];
        }
        input_start += encoded;
        output_start += chunk;
        remaining -= chunk;
    }
    context->candidate_active[rate_index] = 0u;
    if (input_start != transmitted_bits ||
        mu16tty_viterbi_decode(ordered, coded_bits, information_bits, scrambled, sizeof(scrambled)) != (int)information_bits ||
        mu16tty_scramble_bits(scrambled, information_bits, bits) != 0) {
        return;
    }
    memset(body, 0, sizeof(body));
    for (bit_index = 0u; bit_index < information_bits; ++bit_index) {
        body[bit_index / 8u] |= (uint8_t)(bits[bit_index] << (7u - bit_index % 8u));
    }
    memcpy(packet, context->candidate_header[rate_index], 24u);
    memcpy(packet + 24u, body, body_bytes);
    received_crc = ((uint32_t)packet[packet_size - 4u] << 24u) |
                   ((uint32_t)packet[packet_size - 3u] << 16u) |
                   ((uint32_t)packet[packet_size - 2u] << 8u) |
                   (uint32_t)packet[packet_size - 1u];
    calculated_crc = mu16tty_crc32c(packet, packet_size - 4u);
    if (mu16tty_metrics_finish_packet(
            &context->candidate_metrics[rate_index],
            packet,
            packet_size,
            MU16TTY_RATES[rate_index],
            received_crc == calculated_crc
        ) != 0) {
        return;
    }
    {
        mu16tty_event event;
        memset(&event, 0, sizeof(event));
        event.abi_version = MU16TTY_ABI_VERSION;
        event.struct_size = (uint32_t)sizeof(event);
        event.event_type = received_crc == calculated_crc
            ? MU16TTY_EVENT_PACKET
            : MU16TTY_EVENT_DAMAGED_PACKET;
        event.symbol_rate = MU16TTY_RATES[rate_index];
        event.sequence = context->candidate_payload_start[rate_index];
        event.payload_size = (uint32_t)packet_size;
        event.score = context->candidate_confidence[rate_index] /
                      (100.0 + (double)(transmitted_bits / 4u));
        mu16tty_metrics_finish(&context->candidate_metrics[rate_index], &event.metrics);
        memcpy(event.payload, packet, packet_size);
        queue_event(context, &event);
        mu16tty_mid_reset(&context->mid_trackers[rate_index]);
    }
}

static void collect_candidate_body(mu16tty_context *context, size_t rate_index) {
    const size_t samples_per_symbol = (size_t)context->sample_rate / MU16TTY_RATES[rate_index];
    uint32_t physical_index;
    uint32_t relative;
    uint32_t position;
    double llrs[4];
    double likelihoods[16];
    uint64_t symbol_end;
    if (context->candidate_active[rate_index] != 2u) return;
    physical_index = context->candidate_physical_index[rate_index];
    symbol_end = context->candidate_payload_start[rate_index] +
                 (uint64_t)(physical_index + 1u) * samples_per_symbol;
    if (context->total_samples_written < symbol_end) return;
    relative = physical_index - 100u;
    position = relative % 260u;
    if (position >= 4u) {
        const uint32_t count = context->candidate_body_llr_count[rate_index];
        size_t bit;
        context->candidate_confidence[rate_index] += demodulate_symbol_llrs(
            context,
            symbol_end - samples_per_symbol,
            samples_per_symbol,
            context->candidate_metrics[rate_index].metrics.frequency_offset_hz,
            llrs,
            likelihoods
        );
        mu16tty_metrics_observe_symbol(
            &context->candidate_metrics[rate_index], likelihoods
        );
        for (bit = 0u; bit < 4u; ++bit) {
            context->candidate_body_llrs[rate_index][count + bit] = llrs[bit];
        }
        context->candidate_body_llr_count[rate_index] += 4u;
    }
    context->candidate_physical_index[rate_index] += 1u;
    if (context->candidate_body_llr_count[rate_index] >= context->candidate_body_bits[rate_index]) {
        finish_body(context, rate_index);
    } else if (position >= 4u && context->candidate_body_llr_count[rate_index] % 256u == 0u) {
        queue_body_preview(context, rate_index);
    }
}

static double symbol_frequency(uint8_t tone) {
    if (tone == 16u) {
        return 440.0;
    }
    return 440.0 * pow(2.0, (double)MU16TTY_WHITE_KEY_SEMITONES[tone] / 12.0);
}

int mu16tty_encode_tx_symbols(
    const uint8_t *packet, size_t packet_size, uint8_t *output, size_t capacity
) {
    uint8_t coded[5200];
    size_t body_bytes, body_symbols, required, symbol, position;
    int count;
    if (mu16tty_validate_packet_header(packet, packet_size) != 0) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    body_bytes = (size_t)packet[5] + (((size_t)packet[3] << 8u) | packet[4]) + 4u;
    if (packet_size != 24u + body_bytes) return MU16TTY_ERROR_INVALID_ARGUMENT;
    body_symbols = transmitted_length(2u * (8u * body_bytes + 6u)) / 4u;
    required = 12u + 100u + body_symbols + 4u * ((body_symbols + 255u) / 256u);
    if (output == NULL) return (int)required;
    if (capacity < required || mu16tty_encode_segment(packet, 24u, coded, 400u) != 400) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    count = mu16tty_encode_segment(packet + 24u, body_bytes, coded + 400u, sizeof(coded) - 400u);
    if (count != (int)(4u * body_symbols)) return MU16TTY_ERROR_INVALID_ARGUMENT;
    memcpy(output, MU16TTY_SYNC_SYMBOLS, sizeof(MU16TTY_SYNC_SYMBOLS));
    position = sizeof(MU16TTY_SYNC_SYMBOLS);
    for (symbol = 0u; symbol < 100u + body_symbols; ++symbol) {
        const uint8_t *bits = coded + 4u * symbol;
        uint8_t tone = (uint8_t)(8u * bits[0] + 4u * bits[1] + 2u * bits[2] + bits[3]);
        if (symbol >= 100u && (symbol - 100u) % 256u == 0u) {
            memcpy(output + position, MU16TTY_MID_SYMBOLS, sizeof(MU16TTY_MID_SYMBOLS));
            position += sizeof(MU16TTY_MID_SYMBOLS);
        }
        tone ^= (uint8_t)(tone >> 1u);
        tone ^= (uint8_t)(tone >> 2u);
        output[position++] = tone;
    }
    return (int)position;
}

int mu16tty_render_tx_pcm_f32(
    const uint8_t *symbols, size_t symbol_count, uint32_t sample_rate,
    uint32_t symbol_rate, size_t wake_samples, float *output, size_t capacity
) {
    size_t total, index, segment, position = 0u;
    double increments[17];
    double phase = 0.0;
    const double turn = 2.0 * MU16TTY_PI;
    if ((symbols == NULL && symbol_count != 0u) || sample_rate < 8000u ||
        (symbol_rate != 40u && symbol_rate != 20u && symbol_rate != 10u && symbol_rate != 4u) ||
        wake_samples > (size_t)INT_MAX) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    if (symbol_count > ((uint64_t)INT_MAX - wake_samples) * symbol_rate / sample_rate) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    total = wake_samples +
        (size_t)(((uint64_t)symbol_count * sample_rate + symbol_rate - 1u) / symbol_rate);
    for (index = 0u; index < symbol_count; ++index) {
        if (symbols[index] > 16u) return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    if (output == NULL) return (int)total;
    if (capacity < total) return MU16TTY_ERROR_INVALID_ARGUMENT;
    for (index = 0u; index < 17u; ++index) {
        increments[index] = turn * symbol_frequency((uint8_t)index) / sample_rate;
    }
    for (segment = 0u; segment <= symbol_count; ++segment) {
        const size_t end = wake_samples +
            (size_t)(((uint64_t)segment * sample_rate + symbol_rate - 1u) / symbol_rate);
        const double step = increments[segment == 0u ? 16u : symbols[segment - 1u]];
        while (position < end) {
            output[position++] = (float)(0.8 * sin(phase));
            phase += step;
            if (phase >= turn) phase -= turn;
        }
    }
    return (int)total;
}

static void analyze_mid_sync(mu16tty_context *context, size_t rate_index) {
    const unsigned int rate = MU16TTY_RATES[rate_index];
    const size_t samples_per_symbol = (size_t)context->sample_rate / rate;
    const size_t pattern_samples = samples_per_symbol * 4u;
    const uint64_t end = context->total_samples_written;
    const uint64_t oldest = end - (uint64_t)context->ring_used;
    uint64_t start;
    double probe_energy = 0.0;
    double total;
    double best_score = -1.0;
    double best_offset = 0.0;
    uint32_t marker_count;
    int offset;
    size_t probe;
    size_t symbol;
    if ((context->enabled_rate_mask & (1u << rate_index)) == 0u ||
        pattern_samples > context->ring_used || end < pattern_samples) {
        return;
    }
    start = end - (uint64_t)pattern_samples;
    if (start < oldest) {
        return;
    }

    /* A sparse activity probe keeps long silent monitoring inexpensive. */
    for (probe = 0u; probe < 16u; ++probe) {
        const size_t sample_offset = (probe * samples_per_symbol) / 16u;
        const double value = (double)absolute_sample(
            context, start + (uint64_t)sample_offset
        );
        probe_energy += value * value;
    }
    if (probe_energy <= 1.0e-10) {
        return;
    }

    total = tone_segment_score(context, start, samples_per_symbol, 440.0);
    if (total < 0.12) {
        return;
    }
    for (symbol = 1u; symbol < 4u; ++symbol) {
        total += tone_segment_score(
            context,
            start + (uint64_t)(symbol * samples_per_symbol),
            samples_per_symbol,
            symbol_frequency(MU16TTY_MID_SYMBOLS[symbol])
        );
    }
    total /= 4.0;
    if (total < 0.12) {
        return;
    }
    for (offset = -20; offset <= 20; ++offset) {
        double offset_score = 0.0;
        for (symbol = 0u; symbol < 4u; ++symbol) {
            offset_score += tone_segment_score(
                context,
                start + (uint64_t)(symbol * samples_per_symbol),
                samples_per_symbol,
                symbol_frequency(MU16TTY_MID_SYMBOLS[symbol]) + (double)offset
            );
        }
        offset_score /= 4.0;
        if (offset_score > best_score) {
            best_score = offset_score;
            best_offset = (double)offset;
        }
    }
    marker_count = mu16tty_mid_observe(
        &context->mid_trackers[rate_index],
        start,
        best_score,
        best_offset,
        (uint64_t)(260u * samples_per_symbol)
    );
    if (marker_count != 0u &&
        (!context->mid_event_seen[rate_index] ||
         start - context->last_mid_event[rate_index] >= (uint64_t)pattern_samples)) {
        mu16tty_event event;
        memset(&event, 0, sizeof(event));
        event.abi_version = MU16TTY_ABI_VERSION;
        event.struct_size = (uint32_t)sizeof(event);
        event.event_type = MU16TTY_EVENT_MID_SYNC;
        event.symbol_rate = rate;
        event.sequence = start;
        event.score = best_score;
        event.metrics.abi_version = MU16TTY_ABI_VERSION;
        event.metrics.struct_size = (uint32_t)sizeof(event.metrics);
        event.metrics.valid_fields = MU16TTY_METRIC_PATH | MU16TTY_METRIC_FREQUENCY |
                                     MU16TTY_METRIC_MID;
        event.metrics.decode_path = MU16TTY_DECODE_PATH_MID;
        event.metrics.frequency_offset_hz = best_offset;
        event.metrics.mid_score = best_score;
        event.metrics.mid_marker_count = marker_count;
        queue_event(context, &event);
        context->last_mid_event[rate_index] = start;
        context->mid_event_seen[rate_index] = 1u;
    }
}

static void analyze_wake(mu16tty_context *context, uint64_t sample_index) {
    const size_t window = (size_t)context->sample_rate / 10u;
    double energy = 0.0;
    double best_score = 0.0;
    double best_frequency = 0.0;
    unsigned int bin;
    size_t index;
    if (window < 2u || context->ring_used < window) {
        return;
    }
    for (index = 0u; index < window; ++index) {
        const double hann = 0.5 - 0.5 * cos((2.0 * MU16TTY_PI * (double)index) / (double)(window - 1u));
        const double value = (double)recent_sample(context, window, index) * hann;
        energy += value * value;
    }
    if (energy <= 1.0e-20) {
        context->wake_hits = 0u;
        return;
    }
    for (bin = 19u; bin <= 69u; ++bin) {
        const double frequency = (double)bin * 10.0;
        const double coefficient = 2.0 * cos((2.0 * MU16TTY_PI * frequency) / (double)context->sample_rate);
        double previous = 0.0;
        double previous2 = 0.0;
        double power;
        double score;
        for (index = 0u; index < window; ++index) {
            const double hann = 0.5 - 0.5 * cos((2.0 * MU16TTY_PI * (double)index) / (double)(window - 1u));
            const double value = (double)recent_sample(context, window, index) * hann;
            const double current = value + coefficient * previous - previous2;
            previous2 = previous;
            previous = current;
        }
        power = previous * previous + previous2 * previous2 - coefficient * previous * previous2;
        score = (2.0 * power) / ((double)window * energy);
        if (score > best_score) {
            best_score = score;
            best_frequency = frequency;
        }
    }
    if (best_score < 0.20) {
        context->wake_hits = 0u;
        return;
    }
    if (context->wake_hits == 0u || fabs(best_frequency - context->wake_frequency) <= 15.0) {
        context->wake_hits += 1u;
    } else {
        context->wake_hits = 1u;
    }
    context->wake_frequency = best_frequency;
    if (context->wake_hits >= 3u) {
        mu16tty_event event;
        memset(&event, 0, sizeof(event));
        event.abi_version = MU16TTY_ABI_VERSION;
        event.struct_size = (uint32_t)sizeof(event);
        event.event_type = MU16TTY_EVENT_WAKE;
        event.sequence = sample_index;
        event.frequency_hz = best_frequency;
        event.score = best_score;
        queue_event(context, &event);
        context->sync_search_until = sample_index + (uint64_t)context->sample_rate * 10u;
        for (bin = 0u; bin < 4u; ++bin) {
            if (context->next_sync_scan[bin] < sample_index) {
                context->next_sync_scan[bin] = sample_index;
            }
        }
        context->wake_hits = 0u;
    }
}

static int validate_header(uint32_t abi_version, uint32_t struct_size, size_t expected) {
    if (abi_version != MU16TTY_ABI_VERSION) {
        return MU16TTY_ERROR_ABI_VERSION;
    }
    if ((size_t)struct_size < expected) {
        return MU16TTY_ERROR_STRUCT_SIZE;
    }
    return MU16TTY_OK;
}

static void clear_stats(mu16tty_context *context) {
    memset(&context->stats, 0, sizeof(context->stats));
    context->stats.abi_version = MU16TTY_ABI_VERSION;
    context->stats.struct_size = (uint32_t)sizeof(context->stats);
    context->stats.ring_capacity = (uint64_t)context->ring_capacity;
    context->stats.ring_used = (uint64_t)context->ring_used;
    context->stats.allocation_count = context->allocation_count;
}

int mu16tty_create(const mu16tty_config *config, mu16tty_context **out_context) {
    mu16tty_context *context;
    int result;

    if (config == NULL || out_context == NULL || config->sample_rate == 0u) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    *out_context = NULL;
    result = validate_header(config->abi_version, config->struct_size, sizeof(*config));
    if (result != MU16TTY_OK) {
        return result;
    }
    context = (mu16tty_context *)calloc(1u, sizeof(*context));
    if (context == NULL) {
        return MU16TTY_ERROR_NO_MEMORY;
    }
    context->sample_rate = config->sample_rate;
    context->enabled_rate_mask = config->enabled_rate_mask;
    context->event_capacity = config->event_capacity;
    context->ring_capacity = (size_t)config->sample_rate * 360u;
    context->ring = (float *)calloc(context->ring_capacity, sizeof(*context->ring));
    if (context->ring == NULL) {
        free(context);
        return MU16TTY_ERROR_NO_MEMORY;
    }
    if (context->event_capacity != 0u) {
        context->events = (mu16tty_event *)calloc(context->event_capacity, sizeof(*context->events));
        if (context->events == NULL) {
            free(context->ring);
            free(context);
            return MU16TTY_ERROR_NO_MEMORY;
        }
    }
    context->allocation_count = context->event_capacity == 0u ? 2u : 3u;
    context->next_analysis_at = (uint64_t)config->sample_rate / 10u;
    {
        unsigned int rate_index;
        for (rate_index = 0u; rate_index < 4u; ++rate_index) {
            context->next_sync_scan[rate_index] =
                ((uint64_t)config->sample_rate / MU16TTY_RATES[rate_index]) / 4u;
            context->next_mid_scan[rate_index] =
                4u * ((uint64_t)config->sample_rate / MU16TTY_RATES[rate_index]);
        }
    }
    clear_stats(context);
    *out_context = context;
    return MU16TTY_OK;
}

void mu16tty_destroy(mu16tty_context *context) {
    if (context != NULL) {
        free(context->ring);
        free(context->events);
    }
    free(context);
}

int mu16tty_reset(mu16tty_context *context) {
    if (context == NULL) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    context->ring_used = 0u;
    context->ring_write = 0u;
    context->event_head = 0u;
    context->event_count = 0u;
    context->next_analysis_at = (uint64_t)context->sample_rate / 10u;
    context->wake_hits = 0u;
    context->wake_frequency = 0.0;
    context->total_samples_written = 0u;
    context->sync_search_until = 0u;
    memset(context->last_sync_event, 0, sizeof(context->last_sync_event));
    memset(context->pending_sync_start, 0, sizeof(context->pending_sync_start));
    memset(context->pending_sync_score, 0, sizeof(context->pending_sync_score));
    memset(context->pending_sync, 0, sizeof(context->pending_sync));
    memset(context->last_mid_event, 0, sizeof(context->last_mid_event));
    memset(context->mid_event_seen, 0, sizeof(context->mid_event_seen));
    memset(context->candidate_sync_start, 0, sizeof(context->candidate_sync_start));
    memset(context->candidate_active, 0, sizeof(context->candidate_active));
    memset(context->mid_trackers, 0, sizeof(context->mid_trackers));
    {
        unsigned int rate_index;
        for (rate_index = 0u; rate_index < 4u; ++rate_index) {
            context->next_sync_scan[rate_index] =
                ((uint64_t)context->sample_rate / MU16TTY_RATES[rate_index]) / 4u;
            context->next_mid_scan[rate_index] =
                4u * ((uint64_t)context->sample_rate / MU16TTY_RATES[rate_index]);
        }
    }
    clear_stats(context);
    return MU16TTY_OK;
}

int mu16tty_push_pcm_f32(
    mu16tty_context *context,
    const float *samples,
    size_t sample_count
) {
    size_t index;
    float peak;

    if (context == NULL || (samples == NULL && sample_count != 0u)) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    peak = context->stats.peak_abs;
    for (index = 0u; index < sample_count; ++index) {
        const float magnitude = fabsf(samples[index]);
        if (magnitude > peak) {
            peak = magnitude;
        }
        context->ring[context->ring_write] = samples[index];
        context->ring_write = (context->ring_write + 1u) % context->ring_capacity;
        if (context->ring_used < context->ring_capacity) {
            context->ring_used += 1u;
        }
        context->total_samples_written += 1u;
        {
            unsigned int rate_index;
            for (rate_index = 0u; rate_index < 4u; ++rate_index) {
                const uint64_t samples_per_symbol =
                    (uint64_t)context->sample_rate / MU16TTY_RATES[rate_index];
                const uint64_t step = samples_per_symbol / 4u;
                if (context->total_samples_written >= context->next_mid_scan[rate_index]) {
                    analyze_mid_sync(context, rate_index);
                    try_start_mid_recovery(context, rate_index);
                    context->next_mid_scan[rate_index] += step == 0u ? 1u : step;
                }
                try_decode_header(context, rate_index);
                collect_candidate_body(context, rate_index);
            }
        }
        if (context->stats.samples_consumed + (uint64_t)index + 1u >= context->next_analysis_at) {
            analyze_wake(context, context->stats.samples_consumed + (uint64_t)index + 1u);
            context->next_analysis_at += (uint64_t)context->sample_rate / 20u;
        }
        {
            unsigned int rate_index;
            for (rate_index = 0u; rate_index < 4u; ++rate_index) {
                const uint64_t step = ((uint64_t)context->sample_rate / MU16TTY_RATES[rate_index]) / 4u;
                if (context->total_samples_written >= context->next_sync_scan[rate_index]) {
                    analyze_head_sync(context, rate_index);
                    context->next_sync_scan[rate_index] = context->total_samples_written + (step == 0u ? 1u : step);
                }
            }
        }
    }
    context->stats.samples_consumed += (uint64_t)sample_count;
    context->stats.pushes += 1u;
    context->stats.peak_abs = peak;
    context->stats.ring_used = (uint64_t)context->ring_used;
    return MU16TTY_OK;
}

int mu16tty_poll_event(mu16tty_context *context, mu16tty_event *event) {
    int result;

    if (context == NULL || event == NULL) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    result = validate_header(event->abi_version, event->struct_size, sizeof(*event));
    if (result != MU16TTY_OK) {
        return result;
    }
    if (context->event_count == 0u) {
        return MU16TTY_NO_EVENT;
    }
    *event = context->events[context->event_head];
    context->event_head = (context->event_head + 1u) % context->event_capacity;
    context->event_count -= 1u;
    return MU16TTY_OK;
}

int mu16tty_get_stats(const mu16tty_context *context, mu16tty_stats *stats) {
    int result;

    if (context == NULL || stats == NULL) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    result = validate_header(stats->abi_version, stats->struct_size, sizeof(*stats));
    if (result != MU16TTY_OK) {
        return result;
    }
    *stats = context->stats;
    return MU16TTY_OK;
}
