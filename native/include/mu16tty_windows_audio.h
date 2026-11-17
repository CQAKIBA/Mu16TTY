#ifndef MU16TTY_WINDOWS_AUDIO_H
#define MU16TTY_WINDOWS_AUDIO_H

#include "mu16tty_modem.h"

typedef void (*mu16tty_audio_device_callback)(
    int index, const char *name, int inputs, int outputs, double rate,
    const char *host_api, int host_type
);

MU16TTY_API int mu16tty_windows_audio_initialize(void *module, int initialize);
MU16TTY_API int mu16tty_windows_audio_devices(void *module, mu16tty_audio_device_callback callback);
MU16TTY_API int mu16tty_windows_audio_default(void *module, int input);

typedef int (*mu16tty_audio_stream_callback)(
    const void *, void *, unsigned long, const void *, unsigned long, void *
);
typedef void (*mu16tty_audio_finished_callback)(void *);
MU16TTY_API int mu16tty_windows_audio_input_with_context(
    void *module, int device, double rate, unsigned long block_frames,
    mu16tty_audio_stream_callback callback, mu16tty_audio_finished_callback finished,
    void *user_data, void **stream
);
MU16TTY_API int mu16tty_windows_audio_stream_control(void *module, void *stream, unsigned operation);
MU16TTY_API int mu16tty_windows_audio_output(void *module, int device, double rate, void **stream);
MU16TTY_API int mu16tty_windows_audio_write(
    void *module, void *stream, const float *samples, unsigned long frames
);

#endif
