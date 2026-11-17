#ifndef MU16TTY_WINDOWS_PTT_H
#define MU16TTY_WINDOWS_PTT_H

#include "mu16tty_modem.h"

MU16TTY_API uint32_t mu16tty_windows_ptt_open(const wchar_t *name, size_t length, void **port);
MU16TTY_API uint32_t mu16tty_windows_ptt_set(void *port, int dtr, int active);
MU16TTY_API uint32_t mu16tty_windows_ptt_close(void *port);
typedef void (*mu16tty_ptt_port_callback)(
    const wchar_t *port, const wchar_t *name, const wchar_t *product, const wchar_t *instance
);
MU16TTY_API uint32_t mu16tty_windows_ptt_list(mu16tty_ptt_port_callback callback);

#endif
