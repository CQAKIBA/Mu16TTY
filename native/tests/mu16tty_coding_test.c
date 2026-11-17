#include "mu16tty_internal.h"

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define TEST_API __declspec(dllexport)
#else
#define TEST_API
#endif

TEST_API uint32_t mu16tty_test_crc32c(const uint8_t *data, size_t length) {
    return mu16tty_crc32c(data, length);
}

TEST_API int mu16tty_test_scramble(const uint8_t *input, size_t count, uint8_t *output) {
    return mu16tty_scramble_bits(input, count, output);
}

TEST_API int mu16tty_test_convolutional_encode(const uint8_t *input, size_t count, uint8_t *output, size_t output_capacity) {
    return mu16tty_convolutional_encode(input, count, output, output_capacity);
}

TEST_API int mu16tty_test_viterbi_decode(const double *llrs, size_t llr_count, size_t expected_bits, uint8_t *output, size_t output_capacity) {
    return mu16tty_viterbi_decode(llrs, llr_count, expected_bits, output, output_capacity);
}

TEST_API int mu16tty_test_interleave(const uint8_t *input, size_t count, size_t depth, uint8_t *output, size_t output_capacity) {
    return mu16tty_interleave_bits(input, count, depth, output, output_capacity);
}

TEST_API int mu16tty_test_deinterleave(const uint8_t *input, size_t input_count, size_t original_length, size_t depth, uint8_t *output, size_t output_capacity) {
    return mu16tty_deinterleave_bits(input, input_count, original_length, depth, output, output_capacity);
}

TEST_API int mu16tty_test_interleave_blocks(const uint8_t *input, size_t count, size_t block_size, size_t depth, uint8_t *output, size_t output_capacity) {
    return mu16tty_interleave_blocks(input, count, block_size, depth, output, output_capacity);
}

TEST_API int mu16tty_test_deinterleave_blocks(const uint8_t *input, size_t input_count, size_t original_length, size_t block_size, size_t depth, uint8_t *output, size_t output_capacity) {
    return mu16tty_deinterleave_blocks(input, input_count, original_length, block_size, depth, output, output_capacity);
}

TEST_API int mu16tty_test_validate_header(const uint8_t *data, size_t length) {
    return mu16tty_validate_packet_header(data, length);
}

static mu16tty_mid_tracker mid_tracker;

TEST_API void mu16tty_test_mid_reset(void) {
    mu16tty_mid_reset(&mid_tracker);
}

TEST_API uint32_t mu16tty_test_mid_observe(
    uint64_t start,
    double score,
    double frequency,
    uint64_t interval,
    uint64_t now
) {
    (void)now;
    return mu16tty_mid_observe(&mid_tracker, start, score, frequency, interval);
}

TEST_API uint32_t mu16tty_test_mid_count(void) {
    return mid_tracker.candidate_count;
}

TEST_API int mu16tty_test_mid_take(uint64_t now) {
    mu16tty_mid_candidate candidate;
    return mu16tty_mid_take_recovery_candidate(
        &mid_tracker, now, 15000u, &candidate
    );
}
