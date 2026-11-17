#ifndef MU16TTY_STATION_HTTP_H
#define MU16TTY_STATION_HTTP_H

#include "mu16tty_modem.h"
#include <windows.h>

typedef struct mu16tty_http_transmitter {
    volatile LONG *output_device;
    const char *callsign;
    uint64_t session_id;
    void *context;
    int (*ptt_command)(void *context, const wchar_t *args, char *message, size_t capacity, int *changed);
    void (*ptt_status)(void *context, char port[8], int *dtr, int *open);
    int (*my_command)(void *context, const wchar_t *value, char *message, size_t capacity, int *changed);
    unsigned (*queue_depth)(void *context);
    unsigned queue_capacity;
    int (*enqueue)(void *context, const wchar_t *message, unsigned rate,
                   uint32_t *sequence, unsigned *depth);
} mu16tty_http_transmitter;

typedef struct mu16tty_http_input {
    int device;
    void *context;
    int (*change)(void *context, int device, int automatic);
    unsigned (*sample_rate)(void *context);
    int (*running)(void *context);
} mu16tty_http_input;

int mu16tty_serve_local_http(unsigned short port, mu16tty_http_input *input,
                            const mu16tty_http_transmitter *tx, void *audio,
                            int (*stop_requested)(void), int open_browser);
int mu16tty_http_open_history(void);
void mu16tty_http_close_history(void);
void mu16tty_http_record_packet(const mu16tty_event *event, const mu16tty_chat_packet_fields *fields);
void mu16tty_http_record_transmission(const uint8_t *packet, size_t length,
                                     unsigned rate, int delivered);
void mu16tty_http_observe_audio(const float *samples, size_t count, uint32_t sample_rate,
                                unsigned queued, unsigned peak, unsigned capacity);
void mu16tty_http_observe_decoder(const mu16tty_event *event);
void mu16tty_http_observe_queue(unsigned used, unsigned peak, unsigned capacity);

#endif
