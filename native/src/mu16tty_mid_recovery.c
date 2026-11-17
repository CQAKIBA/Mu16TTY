#include "mu16tty_internal.h"

#include <math.h>
#include <string.h>

void mu16tty_mid_reset(mu16tty_mid_tracker *tracker) {
    if (tracker != NULL) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

static int near_interval(uint64_t delta, uint64_t interval) {
    const uint64_t tolerance = interval / 20u + 1u;
    return delta + tolerance >= interval && delta <= interval + tolerance;
}

uint32_t mu16tty_mid_observe(
    mu16tty_mid_tracker *tracker,
    uint64_t start_sample,
    double score,
    double frequency_offset_hz,
    uint64_t interval_samples
) {
    mu16tty_mid_candidate candidate;
    uint32_t index;
    uint32_t marker_count = 0u;
    if (tracker == NULL || interval_samples == 0u || score < 0.15) {
        return 0u;
    }
    for (index = 0u; index < tracker->candidate_count; ++index) {
        mu16tty_mid_candidate *existing = &tracker->candidates[index];
        const uint64_t distance = start_sample >= existing->start_sample
            ? start_sample - existing->start_sample
            : existing->start_sample - start_sample;
        if (distance <= interval_samples / 20u + 1u) {
            if (score > existing->score) {
                existing->start_sample = start_sample;
                existing->score = score;
                existing->frequency_offset_hz = frequency_offset_hz;
                existing->attempted = 0u;
                if (existing->promoted) {
                    return existing->marker_count;
                }
            }
            if (!existing->promoted && existing->score >= 0.30) {
                existing->marker_count = 1u;
                existing->promoted = 1u;
                return 1u;
            }
            return 0u;
        }
    }
    if (score >= 0.30) {
        marker_count = 1u;
    } else {
        for (index = tracker->candidate_count; index > 0u; --index) {
            const mu16tty_mid_candidate *previous = &tracker->candidates[index - 1u];
            if (start_sample > previous->start_sample &&
                near_interval(start_sample - previous->start_sample, interval_samples) &&
                fabs(frequency_offset_hz - previous->frequency_offset_hz) <= 5.0) {
                marker_count = previous->marker_count + 1u;
                if (marker_count < 2u) marker_count = 2u;
                if (marker_count > 3u) marker_count = 3u;
                break;
            }
        }
    }
    if (tracker->candidate_count == MU16TTY_MID_CANDIDATE_CAPACITY) {
        memmove(
            &tracker->candidates[0],
            &tracker->candidates[1],
            sizeof(tracker->candidates[0]) * (MU16TTY_MID_CANDIDATE_CAPACITY - 1u)
        );
        tracker->candidate_count -= 1u;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.start_sample = start_sample;
    candidate.score = score;
    candidate.frequency_offset_hz = frequency_offset_hz;
    candidate.marker_count = marker_count;
    candidate.promoted = marker_count != 0u;
    tracker->candidates[tracker->candidate_count++] = candidate;
    return marker_count;
}

int mu16tty_mid_take_recovery_candidate(
    mu16tty_mid_tracker *tracker,
    uint64_t now_sample,
    uint64_t cooldown_samples,
    mu16tty_mid_candidate *candidate
) {
    uint32_t index;
    if (tracker == NULL || candidate == NULL) {
        return 0;
    }
    if (tracker->recovery_attempts >= MU16TTY_MID_MAX_ATTEMPTS) {
        if (now_sample < tracker->cooldown_until) {
            return 0;
        }
        tracker->recovery_attempts = 0u;
    }
    for (index = 0u; index < tracker->candidate_count; ++index) {
        mu16tty_mid_candidate *current = &tracker->candidates[index];
        if (current->promoted && !current->attempted) {
            current->attempted = 1u;
            tracker->recovery_attempts += 1u;
            if (tracker->recovery_attempts == MU16TTY_MID_MAX_ATTEMPTS) {
                tracker->cooldown_until = now_sample + cooldown_samples;
            }
            *candidate = *current;
            return 1;
        }
    }
    return 0;
}
