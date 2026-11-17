#ifndef MU16TTY_STATION_SETTINGS_H
#define MU16TTY_STATION_SETTINGS_H

#include <stddef.h>

typedef struct mu16tty_timing {
    unsigned delay;
    unsigned wake;
    unsigned tail;
} mu16tty_timing;

int mu16tty_settings_load(void);
int mu16tty_settings_callsign(wchar_t callsign[13]);
int mu16tty_settings_my(wchar_t callsign[13], int save);
int mu16tty_settings_ptt(wchar_t port[8], int *dtr, int save);
int mu16tty_settings_audio_device(void *audio, int input, int fallback);
int mu16tty_settings_save_audio_device(int input, int index, int automatic);
mu16tty_timing mu16tty_settings_timing(void);
/* 1 success, 0 invalid command, -1 save failure. */
int mu16tty_settings_command(const wchar_t *command, char *message, size_t capacity, int *changed);

#endif
