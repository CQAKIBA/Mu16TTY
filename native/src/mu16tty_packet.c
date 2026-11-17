#include "mu16tty_internal.h"
#include <string.h>

#define MU16TTY_PACKET_VERSION 1u
#define MU16TTY_HEADER_SIZE 24u
#define MU16TTY_MAX_CALLSIGN_BYTES 12u
#define MU16TTY_MAX_MESSAGE_BYTES 280u

static void write_big_endian(uint8_t *destination, uint64_t value, size_t size) {
    while (size > 0u) {
        destination[--size] = (uint8_t)value;
        value >>= 8u;
    }
}

int mu16tty_pack_chat_packet(
    uint8_t rate_mode, uint64_t session_id, uint32_t sequence, uint32_t timestamp,
    const uint8_t *callsign, size_t callsign_size,
    const uint8_t *message, size_t message_size,
    uint8_t *output, size_t capacity
) {
    size_t index;
    size_t protected_size;
    if (rate_mode > 3u || callsign == NULL || callsign_size == 0u ||
        callsign_size > MU16TTY_MAX_CALLSIGN_BYTES ||
        message_size > MU16TTY_MAX_MESSAGE_BYTES ||
        (message == NULL && message_size != 0u)) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < callsign_size; ++index) {
        if (callsign[index] > 0x7fu) {
            return MU16TTY_ERROR_INVALID_ARGUMENT;
        }
    }
    protected_size = MU16TTY_HEADER_SIZE + callsign_size + message_size;
    if (output == NULL) {
        return (int)(protected_size + 4u);
    }
    if (capacity < protected_size + 4u) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    output[0] = MU16TTY_PACKET_VERSION;
    output[1] = rate_mode;
    output[2] = MU16TTY_HEADER_SIZE;
    write_big_endian(output + 3u, message_size, 2u);
    output[5] = (uint8_t)callsign_size;
    write_big_endian(output + 6u, session_id, 8u);
    write_big_endian(output + 14u, sequence, 4u);
    write_big_endian(output + 18u, timestamp, 4u);
    output[22] = 0u;
    output[23] = 0u;
    memcpy(output + MU16TTY_HEADER_SIZE, callsign, callsign_size);
    if (message_size != 0u) {
        memcpy(output + MU16TTY_HEADER_SIZE + callsign_size, message, message_size);
    }
    write_big_endian(output + protected_size, mu16tty_crc32c(output, protected_size), 4u);
    return (int)(protected_size + 4u);
}

int mu16tty_validate_packet_header(const uint8_t *data, size_t length) {
    uint16_t message_length;
    uint16_t reserved;
    if (data == NULL || length < MU16TTY_HEADER_SIZE) {
        return -1;
    }
    if (data[0] != MU16TTY_PACKET_VERSION) {
        return -2;
    }
    if ((data[1] & UINT8_C(0xF8)) != 0u || (data[1] & UINT8_C(0x07)) > 3u) {
        return -3;
    }
    if (data[2] != MU16TTY_HEADER_SIZE) {
        return -4;
    }
    message_length = (uint16_t)(((uint16_t)data[3] << 8u) | data[4]);
    if (message_length > MU16TTY_MAX_MESSAGE_BYTES) {
        return -5;
    }
    if (data[5] == 0u || data[5] > MU16TTY_MAX_CALLSIGN_BYTES) {
        return -6;
    }
    reserved = (uint16_t)(((uint16_t)data[22] << 8u) | data[23]);
    if (reserved != 0u) {
        return -7;
    }
    return 0;
}

static uint64_t read_big_endian(const uint8_t *source, size_t size) {
    uint64_t value = 0u;
    size_t index;
    for (index = 0u; index < size; ++index) {
        value = (value << 8u) | source[index];
    }
    return value;
}

int mu16tty_inspect_chat_packet(
    const uint8_t *packet, size_t packet_size, mu16tty_chat_packet_fields *fields
) {
    size_t message_size;
    size_t protected_size;
    if (fields == NULL || mu16tty_validate_packet_header(packet, packet_size) != 0) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    message_size = (size_t)read_big_endian(packet + 3u, 2u);
    protected_size = MU16TTY_HEADER_SIZE + packet[5] + message_size;
    if (packet_size != protected_size + 4u) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    fields->session_id = read_big_endian(packet + 6u, 8u);
    fields->sequence = (uint32_t)read_big_endian(packet + 14u, 4u);
    fields->timestamp = (uint32_t)read_big_endian(packet + 18u, 4u);
    fields->rate_mode = packet[1];
    fields->callsign_offset = MU16TTY_HEADER_SIZE;
    fields->callsign_size = packet[5];
    fields->message_size = (uint32_t)message_size;
    return MU16TTY_OK;
}

int mu16tty_unpack_chat_packet(
    const uint8_t *packet, size_t packet_size, mu16tty_chat_packet_fields *fields
) {
    mu16tty_chat_packet_fields inspected;
    size_t protected_size;
    if (fields == NULL || mu16tty_inspect_chat_packet(packet, packet_size, &inspected) != MU16TTY_OK) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    protected_size = packet_size - 4u;
    if (read_big_endian(packet + protected_size, 4u) != mu16tty_crc32c(packet, protected_size)) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    *fields = inspected;
    return MU16TTY_OK;
}
