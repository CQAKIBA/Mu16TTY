#include "mu16tty_modem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int main(int argc, char **argv)
{
    uint8_t expected[320], packet[320], symbols[2048];
    const uint8_t message[] = {0xe7, 0x8c, 0xab};
    const unsigned rates[] = {40, 20, 10, 4};
    mu16tty_chat_packet_fields fields;
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file != NULL);
    size_t expected_size = fread(expected, 1, sizeof(expected), file);
    CHECK(!ferror(file));
    fclose(file);
    int size = mu16tty_pack_chat_packet(0, UINT64_C(0x0102030405060708), 9, 1700000000,
                                      (const uint8_t *)"JA1TEST", 7, message, sizeof(message), packet, sizeof(packet));
    CHECK(size > 0 && (size_t)size == expected_size);
    CHECK(memcmp(expected, packet, expected_size) == 0);
    CHECK(mu16tty_unpack_chat_packet(packet, expected_size, &fields) == MU16TTY_OK);
    packet[24] ^= 1;
    CHECK(mu16tty_unpack_chat_packet(packet, expected_size, &fields) != MU16TTY_OK);
    CHECK(mu16tty_inspect_chat_packet(packet, expected_size, &fields) == MU16TTY_OK);

    for (unsigned mode = 0; mode < 4; ++mode) {
        size = mu16tty_pack_chat_packet((uint8_t)mode, 1, mode, 0,
                                       (const uint8_t *)"JA1TEST", 7, message, sizeof(message), packet, sizeof(packet));
        CHECK(size > 0);
        int count = mu16tty_encode_tx_symbols(packet, (size_t)size, symbols, sizeof(symbols));
        CHECK(count > 0);
        int samples = mu16tty_render_tx_pcm_f32(symbols, (size_t)count, 12000, rates[mode], 7200, NULL, 0);
        CHECK(samples > 0);
        size_t total = (size_t)samples + 24000;
        float *pcm = calloc(total, sizeof(*pcm));
        CHECK(pcm != NULL);
        CHECK(mu16tty_render_tx_pcm_f32(symbols, (size_t)count, 12000, rates[mode], 7200, pcm, (size_t)samples) == samples);
        mu16tty_config config = {MU16TTY_ABI_VERSION, sizeof(config), 12000, 1u << mode, 64, 0};
        mu16tty_context *modem = NULL;
        CHECK(mu16tty_create(&config, &modem) == MU16TTY_OK);
        int received = 0;
        for (size_t offset = 0; offset < total; ) {
            size_t chunk = total - offset < 257 ? total - offset : 257;
            CHECK(mu16tty_push_pcm_f32(modem, pcm + offset, chunk) == MU16TTY_OK);
            offset += chunk;
            mu16tty_event event = {0};
            event.abi_version = MU16TTY_ABI_VERSION;
            event.struct_size = sizeof(event);
            while (mu16tty_poll_event(modem, &event) == MU16TTY_OK) {
                if (event.event_type == MU16TTY_EVENT_PACKET) {
                    CHECK(event.payload_size == (uint32_t)size);
                    CHECK(memcmp(event.payload, packet, (size_t)size) == 0);
                    received = 1;
                }
            }
        }
        mu16tty_destroy(modem);
        free(pcm);
        CHECK(received);
        printf("%u sym/s UTF-8 round trip passed\n", rates[mode]);
    }
    return 0;
}
