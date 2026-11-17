#include "mu16tty_modem.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAPS 64u
#define PHASES 128u

struct mu16tty_resampler {
    uint32_t input_rate;
    uint32_t output_rate;
    uint64_t phase;
    size_t head;
    float history[TAPS];
    float coefficients[PHASES][TAPS];
};

int mu16tty_resampler_create(
    uint32_t input_rate, uint32_t output_rate, mu16tty_resampler **out
) {
    const double pi = 3.14159265358979323846;
    mu16tty_resampler *r;
    double cutoff;
    size_t phase;
    if (out == NULL || input_rate == 0u || output_rate == 0u) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    *out = NULL;
    r = (mu16tty_resampler *)calloc(1u, sizeof(*r));
    if (r == NULL) return MU16TTY_ERROR_NO_MEMORY;
    r->input_rate = input_rate;
    r->output_rate = output_rate;
    cutoff = 0.9 * (output_rate < input_rate ? (double)output_rate / input_rate : 1.0);
    for (phase = 0u; phase < PHASES; ++phase) {
        double sum = 0.0;
        size_t tap;
        for (tap = 0u; tap < TAPS; ++tap) {
            const double x = (double)tap - (double)(TAPS / 2u) + (double)phase / PHASES;
            const double angle = pi * x / (TAPS / 2u);
            const double window = 0.42 + 0.5 * cos(angle) + 0.08 * cos(2.0 * angle);
            const double sinc = x == 0.0 ? cutoff : sin(pi * cutoff * x) / (pi * x);
            r->coefficients[phase][tap] = (float)(window * sinc);
            sum += window * sinc;
        }
        for (tap = 0u; tap < TAPS; ++tap) {
            r->coefficients[phase][tap] = (float)(r->coefficients[phase][tap] / sum);
        }
    }
    *out = r;
    return MU16TTY_OK;
}

void mu16tty_resampler_destroy(mu16tty_resampler *r) {
    free(r);
}

void mu16tty_resampler_reset(mu16tty_resampler *r) {
    if (r != NULL) {
        r->phase = 0u;
        r->head = 0u;
        memset(r->history, 0, sizeof(r->history));
    }
}

int mu16tty_resampler_process(
    mu16tty_resampler *r, const float *input, size_t input_count,
    float *output, size_t capacity
) {
    uint64_t span, required;
    size_t in, out = 0u;
    if (r == NULL || (input == NULL && input_count != 0u) ||
        input_count > UINT64_MAX / r->output_rate) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    span = (uint64_t)input_count * r->output_rate;
    required = span > r->phase ? 1u + (span - r->phase - 1u) / r->input_rate : 0u;
    if (required > capacity || required > INT_MAX || (required != 0u && output == NULL)) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    for (in = 0u; in < input_count; ++in) {
        r->head = (r->head + 1u) % TAPS;
        r->history[r->head] = input[in];
        while (r->phase < r->output_rate) {
            const size_t phase = (size_t)(r->phase * PHASES / r->output_rate);
            float value = 0.0f;
            size_t tap;
            for (tap = 0u; tap < TAPS; ++tap) {
                value += r->coefficients[phase][tap] * r->history[(r->head + TAPS - tap) % TAPS];
            }
            output[out++] = value;
            r->phase += r->input_rate;
        }
        r->phase -= r->output_rate;
    }
    return (int)out;
}
