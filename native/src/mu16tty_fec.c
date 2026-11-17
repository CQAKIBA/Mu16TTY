#include "mu16tty_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

static uint8_t parity(uint32_t value) {
    value ^= value >> 16u;
    value ^= value >> 8u;
    value ^= value >> 4u;
    value &= 0x0Fu;
    return (uint8_t)((UINT16_C(0x6996) >> value) & 1u);
}

static int binary_input(const uint8_t *input, size_t count) {
    size_t index;
    if (input == NULL && count != 0u) {
        return 0;
    }
    for (index = 0u; index < count; ++index) {
        if (input[index] > 1u) {
            return 0;
        }
    }
    return 1;
}

int mu16tty_convolutional_encode(
    const uint8_t *input,
    size_t count,
    uint8_t *output,
    size_t output_capacity
) {
    const size_t steps = count + MU16TTY_TAIL_BITS;
    const size_t encoded_length = 2u * steps;
    uint32_t state = 0u;
    size_t step;
    if (!binary_input(input, count) || output == NULL || output_capacity < encoded_length || encoded_length > (size_t)INT_MAX) {
        return -1;
    }
    for (step = 0u; step < steps; ++step) {
        const uint8_t bit = step < count ? input[step] : 0u;
        state = ((state << 1u) | bit) & UINT32_C(0x7F);
        output[2u * step] = parity(state & UINT32_C(0171));
        output[2u * step + 1u] = parity(state & UINT32_C(0133));
    }
    return (int)encoded_length;
}

static int viterbi_decode(
    const double *llrs,
    size_t llr_count,
    size_t expected_bits,
    uint8_t *output,
    size_t output_capacity,
    int terminated
) {
    const size_t steps = expected_bits + (terminated ? MU16TTY_TAIL_BITS : 0u);
    double metrics[MU16TTY_STATE_COUNT];
    double next_metrics[MU16TTY_STATE_COUNT];
    uint8_t *predecessors;
    uint8_t *bits;
    size_t state;
    size_t step;

    if (llrs == NULL || output_capacity < expected_bits || (output == NULL && expected_bits != 0u) || llr_count != 2u * steps || expected_bits > (size_t)INT_MAX) {
        return -1;
    }
    for (step = 0u; step < llr_count; ++step) {
        if (!isfinite(llrs[step])) {
            return -1;
        }
    }
    predecessors = (uint8_t *)malloc(steps * MU16TTY_STATE_COUNT);
    bits = (uint8_t *)malloc(steps * MU16TTY_STATE_COUNT);
    if (predecessors == NULL || bits == NULL) {
        free(predecessors);
        free(bits);
        return -1;
    }
    for (state = 0u; state < MU16TTY_STATE_COUNT; ++state) {
        metrics[state] = state == 0u ? 0.0 : DBL_MAX;
    }
    for (step = 0u; step < steps; ++step) {
        const double first = llrs[2u * step];
        const double second = llrs[2u * step + 1u];
        size_t previous;
        for (state = 0u; state < MU16TTY_STATE_COUNT; ++state) {
            next_metrics[state] = DBL_MAX;
        }
        for (previous = 0u; previous < MU16TTY_STATE_COUNT; ++previous) {
            uint8_t input_bit;
            if (metrics[previous] == DBL_MAX) {
                continue;
            }
            for (input_bit = 0u; input_bit <= 1u; ++input_bit) {
                const uint32_t reg = (uint32_t)((previous << 1u) | input_bit);
                const size_t next = reg & (MU16TTY_STATE_COUNT - 1u);
                const uint8_t encoded0 = parity(reg & UINT32_C(0171));
                const uint8_t encoded1 = parity(reg & UINT32_C(0133));
                const double branch = (encoded0 == 0u ? -first : first) + (encoded1 == 0u ? -second : second);
                const double candidate = metrics[previous] + branch;
                if (candidate < next_metrics[next]) {
                    next_metrics[next] = candidate;
                    predecessors[step * MU16TTY_STATE_COUNT + next] = (uint8_t)previous;
                    bits[step * MU16TTY_STATE_COUNT + next] = input_bit;
                }
            }
        }
        for (state = 0u; state < MU16TTY_STATE_COUNT; ++state) {
            metrics[state] = next_metrics[state];
        }
    }
    state = 0u;
    if (!terminated) {
        size_t candidate;
        for (candidate = 1u; candidate < MU16TTY_STATE_COUNT; ++candidate) {
            if (metrics[candidate] < metrics[state]) {
                state = candidate;
            }
        }
    }
    for (step = steps; step-- > 0u;) {
        const uint8_t bit = bits[step * MU16TTY_STATE_COUNT + state];
        if (step < expected_bits) {
            output[step] = bit;
        }
        state = predecessors[step * MU16TTY_STATE_COUNT + state];
    }
    free(predecessors);
    free(bits);
    return (int)expected_bits;
}

int mu16tty_viterbi_decode(
    const double *llrs, size_t llr_count, size_t expected_bits,
    uint8_t *output, size_t output_capacity
) {
    return viterbi_decode(llrs, llr_count, expected_bits, output, output_capacity, 1);
}

int mu16tty_viterbi_prefix(
    const double *llrs, size_t llr_count, uint8_t *output, size_t output_capacity
) {
    return viterbi_decode(llrs, llr_count, llr_count / 2u, output, output_capacity, 0);
}
