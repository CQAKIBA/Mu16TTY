#include "mu16tty_internal.h"

#include <limits.h>
#include <string.h>

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

int mu16tty_scramble_bits(const uint8_t *input, size_t count, uint8_t *output) {
    uint16_t state = UINT16_C(0x1FF);
    size_t index;
    if (!binary_input(input, count) || (output == NULL && count != 0u)) {
        return -1;
    }
    for (index = 0u; index < count; ++index) {
        const uint8_t whitening = (uint8_t)((state >> 8u) & 1u);
        const uint16_t feedback = (uint16_t)(((state >> 8u) ^ (state >> 4u)) & 1u);
        output[index] = (uint8_t)(input[index] ^ whitening);
        state = (uint16_t)(((state << 1u) & UINT16_C(0x1FF)) | feedback);
    }
    return 0;
}

int mu16tty_interleave_bits(
    const uint8_t *input,
    size_t count,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
) {
    size_t rows;
    size_t encoded_length;
    size_t column;
    size_t row;
    size_t output_index = 0u;

    if (!binary_input(input, count) || depth == 0u) {
        return -1;
    }
    rows = count == 0u ? 0u : (count + depth - 1u) / depth;
    encoded_length = rows * depth;
    if ((output == NULL && encoded_length != 0u) || output_capacity < encoded_length || encoded_length > (size_t)INT_MAX) {
        return -1;
    }
    for (column = 0u; column < depth; ++column) {
        for (row = 0u; row < rows; ++row) {
            const size_t input_index = row * depth + column;
            output[output_index++] = input_index < count ? input[input_index] : 0u;
        }
    }
    return (int)encoded_length;
}

int mu16tty_deinterleave_bits(
    const uint8_t *input,
    size_t input_count,
    size_t original_length,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
) {
    size_t rows;
    size_t index;
    if (!binary_input(input, input_count) || depth == 0u || input_count % depth != 0u || original_length > input_count || output_capacity < original_length || (output == NULL && original_length != 0u) || original_length > (size_t)INT_MAX) {
        return -1;
    }
    rows = input_count / depth;
    for (index = 0u; index < original_length; ++index) {
        const size_t row = index / depth;
        const size_t column = index % depth;
        output[index] = input[column * rows + row];
    }
    return (int)original_length;
}

int mu16tty_interleave_blocks(
    const uint8_t *input,
    size_t count,
    size_t block_size,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
) {
    size_t input_start = 0u;
    size_t output_start = 0u;
    if (block_size == 0u || depth == 0u || block_size % depth != 0u) {
        return -1;
    }
    while (input_start < count) {
        const size_t remaining = count - input_start;
        const size_t chunk_length = remaining < block_size ? remaining : block_size;
        const int written = mu16tty_interleave_bits(
            input + input_start,
            chunk_length,
            depth,
            output + output_start,
            output_capacity - output_start
        );
        if (written < 0) {
            return -1;
        }
        input_start += chunk_length;
        output_start += (size_t)written;
    }
    return output_start <= (size_t)INT_MAX ? (int)output_start : -1;
}

int mu16tty_deinterleave_blocks(
    const uint8_t *input,
    size_t input_count,
    size_t original_length,
    size_t block_size,
    size_t depth,
    uint8_t *output,
    size_t output_capacity
) {
    size_t input_start = 0u;
    size_t output_start = 0u;
    size_t remaining = original_length;
    if (block_size == 0u || depth == 0u || block_size % depth != 0u || output_capacity < original_length) {
        return -1;
    }
    while (remaining != 0u) {
        const size_t chunk_length = remaining < block_size ? remaining : block_size;
        const size_t encoded_length = ((chunk_length + depth - 1u) / depth) * depth;
        int written;
        if (encoded_length > input_count - input_start) {
            return -1;
        }
        written = mu16tty_deinterleave_bits(
            input + input_start,
            encoded_length,
            chunk_length,
            depth,
            output + output_start,
            output_capacity - output_start
        );
        if (written < 0) {
            return -1;
        }
        input_start += encoded_length;
        output_start += chunk_length;
        remaining -= chunk_length;
    }
    if (input_start != input_count || output_start > (size_t)INT_MAX) {
        return -1;
    }
    return (int)output_start;
}

int mu16tty_encode_segment(
    const uint8_t *data,
    size_t data_size,
    uint8_t *output,
    size_t output_capacity
) {
    uint8_t information[2368];
    uint8_t scrambled[2368];
    uint8_t coded[4768];
    size_t information_count;
    size_t byte_index;
    size_t bit_index;
    int coded_count;
    if ((data == NULL && data_size != 0u) || output == NULL || data_size > 296u) {
        return -1;
    }
    information_count = data_size * 8u;
    for (byte_index = 0u; byte_index < data_size; ++byte_index) {
        for (bit_index = 0u; bit_index < 8u; ++bit_index) {
            information[byte_index * 8u + bit_index] =
                (uint8_t)((data[byte_index] >> (7u - bit_index)) & 1u);
        }
    }
    if (mu16tty_scramble_bits(information, information_count, scrambled) != 0) {
        return -1;
    }
    coded_count = mu16tty_convolutional_encode(
        scrambled, information_count, coded, sizeof(coded)
    );
    if (coded_count < 0) {
        return -1;
    }
    return mu16tty_interleave_blocks(
        coded, (size_t)coded_count, 256u, 16u, output, output_capacity
    );
}
