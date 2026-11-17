#include "mu16tty_windows_audio.h"
#include "mu16tty_windows_ptt.h"
#include "mu16tty_station_http.h"
#include "mu16tty_station_settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <limits.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

static HMODULE load_portaudio(void)
{
    static wchar_t path[32768];
    const wchar_t *relative = L"runtime\\portaudio\\libportaudio64bit.dll";
    wchar_t *filename;
    DWORD size = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    if (size == 0 || size >= ARRAYSIZE(path))
        return NULL;
    filename = wcsrchr(path, L'\\');
    if (filename == NULL || (size_t)(filename + 1 - path) + wcslen(relative) >= ARRAYSIZE(path))
        return NULL;
    memcpy(filename + 1, relative, (wcslen(relative) + 1) * sizeof(*relative));
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static void print_table_field(const char *text)
{
    for (; *text != '\0'; ++text) {
        unsigned char ch = (unsigned char)*text;
        putchar(ch < 0x20 ? ' ' : ch);
    }
}

static void print_device_name(const char *name, int host_type)
{
    wchar_t wide[512];
    char utf8[2048];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, ARRAYSIZE(wide))) {
        print_table_field(name);
    } else if ((host_type == 1 || host_type == 2 || host_type == 3) &&
               MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide)) &&
               WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, ARRAYSIZE(utf8), NULL, NULL)) {
        print_table_field(utf8);
    } else {
        fputs("?", stdout);
    }
}

static void print_device(int index, const char *name, int inputs, int outputs,
                         double rate, const char *host, int host_type)
{
    printf("%d\t%d\t%d\t%.0f\t", index, inputs, outputs, rate);
    print_table_field(host);
    putchar('\t');
    print_device_name(name, host_type);
    putchar('\n');
}

typedef struct ptt_port_description {
    wchar_t name[512];
    wchar_t product[512];
} ptt_port_description;

static ptt_port_description *ptt_ports[10000];
static int ptt_list_allocation_failed;

static const wchar_t *usb_id(const wchar_t *instance, const wchar_t *prefix)
{
    for (; *instance != L'\0'; ++instance) {
        if (_wcsnicmp(instance, prefix, 4) == 0 &&
            wcslen(instance) >= 8 &&
            wcsspn(instance + 4, L"0123456789ABCDEFabcdef") >= 4)
            return instance + 4;
    }
    return NULL;
}

static void collect_ptt_port(const wchar_t *port, const wchar_t *name,
                             const wchar_t *product, const wchar_t *instance)
{
    unsigned long number = wcstoul(port + 3, NULL, 10);
    ptt_port_description *description;
    if (number == 0 || number >= ARRAYSIZE(ptt_ports) || ptt_ports[number] != NULL)
        return;
    description = calloc(1, sizeof(*description));
    if (description == NULL) {
        ptt_list_allocation_failed = 1;
        return;
    }
    wcsncpy_s(description->name, ARRAYSIZE(description->name), *name ? name : port, _TRUNCATE);
    if (_wcsnicmp(instance, L"USB\\", 4) == 0 ||
        _wcsnicmp(instance, L"FTDIBUS\\", 8) == 0) {
        if (*product != L'\0') {
            wcsncpy_s(description->product, ARRAYSIZE(description->product), product, _TRUNCATE);
        } else {
            const wchar_t *vendor = usb_id(instance, L"VID_");
            const wchar_t *device = usb_id(instance, L"PID_");
            if (vendor != NULL && device != NULL)
                swprintf_s(description->product, ARRAYSIZE(description->product),
                           L"USB %.4ls:%.4ls", vendor, device);
        }
    }
    ptt_ports[number] = description;
}

static void print_wide_field(const wchar_t *text)
{
    char utf8[2048];
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL))
        print_table_field(utf8);
}

static int list_ptt_ports(void)
{
    size_t index;
    size_t count = 0;
    UINT previous_code_page = GetConsoleOutputCP();
    uint32_t result = mu16tty_windows_ptt_list(collect_ptt_port);
    if (previous_code_page != 0)
        SetConsoleOutputCP(CP_UTF8);
    for (index = 1; index < ARRAYSIZE(ptt_ports); ++index) {
        ptt_port_description *description = ptt_ports[index];
        if (description == NULL)
            continue;
        if (result == ERROR_SUCCESS && !ptt_list_allocation_failed) {
            printf("COM%zu  ", index);
            print_wide_field(description->name);
            if (description->product[0] != L'\0') {
                fputs(" | ", stdout);
                print_wide_field(description->product);
            }
            putchar('\n');
            ++count;
        }
        free(description);
    }
    if (count == 0 && result == ERROR_SUCCESS && !ptt_list_allocation_failed)
        puts("No serial ports available.");
    fflush(stdout);
    if (previous_code_page != 0)
        SetConsoleOutputCP(previous_code_page);
    if (result != ERROR_SUCCESS) {
        fprintf(stderr, "Could not list PTT ports (Windows error %u).\n", result);
        return 1;
    }
    if (ptt_list_allocation_failed) {
        fputs("Could not allocate the PTT port list.\n", stderr);
        return 1;
    }
    return 0;
}

/* Buffer short decoder stalls while the PortAudio callback keeps receiving. */
enum { QUEUE_BLOCKS = 128, MODEM_RATE = 12000 };
enum {
    INPUT_UNDERFLOW = 1, INPUT_OVERFLOW = 2,
    INPUT_BLOCK_TOO_LARGE = 4, INPUT_QUEUE_FULL = 8
};

typedef struct receive_queue {
    float *samples;
    unsigned long block_frames;
    unsigned long frames[QUEUE_BLOCKS];
    volatile LONG head;
    volatile LONG tail;
    volatile LONG gap;
    volatile LONG finished;
    volatile LONG peak_blocks;
    int report_queue;
    volatile LONG *paused;
    HANDLE ready;
    HANDLE first_pcm;
} receive_queue;

static volatile LONG stop_requested;
static HANDLE station_shutdown;
static __declspec(thread) int selected_device;
static __declspec(thread) uint32_t selected_rate;

static BOOL WINAPI stop_audio(DWORD event)
{
    if (event == CTRL_CLOSE_EVENT && station_shutdown != NULL) {
        InterlockedExchange(&stop_requested, 1);
        WaitForSingleObject(station_shutdown, 4000);
        return TRUE;
    }
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    InterlockedExchange(&stop_requested, 1);
    return TRUE;
}

static HANDLE lock_station_folder(void)
{
    static wchar_t path[32768];
    wchar_t name[64], *filename;
    DWORD length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    uint64_t hash = UINT64_C(14695981039346656037);
    HANDLE lock;
    wchar_t *cursor;
    if (length == 0 || length >= ARRAYSIZE(path) ||
        (filename = wcsrchr(path, L'\\')) == NULL)
        return NULL;
    *filename = L'\0';
    CharLowerBuffW(path, (DWORD)(filename - path));
    for (cursor = path; *cursor; ++cursor) {
        hash ^= (uint64_t)*cursor;
        hash *= UINT64_C(1099511628211);
    }
    swprintf_s(name, ARRAYSIZE(name), L"Local\\Mu16TTY-native-%016llx", (unsigned long long)hash);
    lock = CreateMutexW(NULL, FALSE, name);
    if (lock != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(lock);
        MessageBoxW(NULL, L"Mu16TTY is already running from this folder.",
                    L"Mu16TTY", MB_OK | MB_ICONERROR);
        return NULL;
    }
    return lock;
}

static int should_stop(void)
{
    return InterlockedCompareExchange(&stop_requested, 0, 0) != 0;
}

static void find_input_rate(int index, const char *name, int inputs, int outputs,
                            double rate, const char *host, int host_type)
{
    (void)name;
    (void)outputs;
    (void)host;
    (void)host_type;
    if (index == selected_device && inputs > 0 && rate >= 1 && rate <= 384000)
        selected_rate = (uint32_t)(rate + 0.5);
}

static int capture_block(const void *input, void *output, unsigned long frames,
                         const void *timing, unsigned long flags, void *user)
{
    receive_queue *queue = user;
    LONG head = InterlockedCompareExchange(&queue->head, 0, 0);
    LONG next = (head + 1) % QUEUE_BLOCKS;
    LONG tail = InterlockedCompareExchange(&queue->tail, 0, 0);
    LONG used = (head - tail + QUEUE_BLOCKS) % QUEUE_BLOCKS;
    LONG peak = queue->peak_blocks;
    float *destination = queue->samples + (size_t)head * queue->block_frames;
    (void)output;
    (void)timing;
    LONG gap = (LONG)(flags & (INPUT_UNDERFLOW | INPUT_OVERFLOW));
    if (frames > queue->block_frames)
        gap |= INPUT_BLOCK_TOO_LARGE;
    if (next == tail)
        gap |= INPUT_QUEUE_FULL;
    if (gap != 0) {
        InterlockedOr(&queue->gap, gap);
    } else if (frames != 0) {
        if (input != NULL)
            memcpy(destination, input, frames * sizeof(float));
        else
            memset(destination, 0, frames * sizeof(float));
        queue->frames[head] = frames;
        InterlockedExchange(&queue->head, next);
        ++used;
        if (input != NULL && queue->first_pcm != NULL)
            SetEvent(queue->first_pcm);
    }
    if (used > peak) {
        InterlockedExchange(&queue->peak_blocks, used);
        peak = used;
    }
    if (queue->report_queue &&
        (queue->paused == NULL || InterlockedCompareExchange(queue->paused, 0, 0) == 0))
        mu16tty_http_observe_queue((unsigned)used, (unsigned)peak, QUEUE_BLOCKS - 1);
    SetEvent(queue->ready);
    return 0; /* paContinue */
}

static void capture_finished(void *user)
{
    receive_queue *queue = user;
    InterlockedExchange(&queue->finished, 1);
    SetEvent(queue->ready);
}

typedef void (*packet_sink)(const mu16tty_event *, const mu16tty_chat_packet_fields *);
typedef void (*audio_sink)(const float *, size_t, uint32_t, unsigned, unsigned, unsigned);
typedef void (*decoder_sink)(const mu16tty_event *);

static void print_packet(const mu16tty_event *event, const mu16tty_chat_packet_fields *fields)
{
    const uint8_t *callsign = event->payload + fields->callsign_offset;
    const uint8_t *message = callsign + fields->callsign_size;
    size_t index;

    printf("[%u sym/s] ", event->symbol_rate);
    for (index = 0; index < fields->callsign_size; ++index)
        putchar(callsign[index] < 0x20 ? ' ' : callsign[index]);
    fputs(": ", stdout);
    for (index = 0; index < fields->message_size; ++index)
        putchar(message[index] < 0x20 ? ' ' : message[index]);
    putchar('\n');
    fflush(stdout);
}

static void deliver_packet(const mu16tty_event *event, packet_sink sink)
{
    mu16tty_chat_packet_fields fields;
    const uint8_t *callsign;
    const uint8_t *message;
    size_t index;
    if (mu16tty_unpack_chat_packet(event->payload, event->payload_size, &fields) != MU16TTY_OK)
        return;
    callsign = event->payload + fields.callsign_offset;
    message = callsign + fields.callsign_size;
    for (index = 0; index < fields.callsign_size; ++index) {
        if (callsign[index] > 0x7f)
            return;
    }
    if (fields.message_size != 0 &&
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)message,
                             (int)fields.message_size, NULL, 0))
        return;

    sink(event, &fields);
}

static int drain_modem_events(mu16tty_context *modem, packet_sink sink, decoder_sink decoder)
{
    mu16tty_event event = {0};
    int result;
    event.abi_version = MU16TTY_ABI_VERSION;
    event.struct_size = sizeof(event);
    while ((result = mu16tty_poll_event(modem, &event)) == MU16TTY_OK) {
        if (decoder != NULL)
            decoder(&event);
        if (event.event_type == MU16TTY_EVENT_PACKET)
            deliver_packet(&event, sink);
    }
    return result == MU16TTY_NO_EVENT ? MU16TTY_OK : result;
}

static int process_input(receive_queue *queue, mu16tty_context *modem,
                         mu16tty_resampler *resampler, float *converted, size_t capacity,
                         packet_sink sink, audio_sink monitor, decoder_sink decoder,
                         volatile LONG *retire, volatile LONG *paused)
{
    while (!should_stop() && (retire == NULL || InterlockedCompareExchange(retire, 0, 0) == 0)) {
        if (paused != NULL && InterlockedCompareExchange(paused, 0, 0) != 0) {
            if (InterlockedCompareExchange(&queue->finished, 0, 0) != 0)
                return MU16TTY_ERROR_INVALID_ARGUMENT;
            Sleep(10);
            continue;
        }
        LONG tail = InterlockedCompareExchange(&queue->tail, 0, 0);
        LONG head = InterlockedCompareExchange(&queue->head, 0, 0);
        LONG gap = InterlockedExchange(&queue->gap, 0);
        if (gap != 0) {
            InterlockedExchange(&queue->tail, head);
            mu16tty_resampler_reset(resampler);
            mu16tty_reset(modem);
            if (decoder != NULL)
                decoder(NULL);
            fprintf(stderr, "Audio input %d at %u Hz; decoder reset:%s%s%s%s\n",
                    selected_device, selected_rate,
                    gap & INPUT_UNDERFLOW ? " PortAudio input underflow;" : "",
                    gap & INPUT_OVERFLOW ? " PortAudio input overflow;" : "",
                    gap & INPUT_BLOCK_TOO_LARGE ? " callback block exceeds buffer;" : "",
                    gap & INPUT_QUEUE_FULL ? " receiver queue full;" : "");
        } else if (tail != head) {
            const float *samples = queue->samples + (size_t)tail * queue->block_frames;
            if (monitor != NULL)
                monitor(samples, queue->frames[tail], selected_rate,
                        (unsigned)((head - tail - 1 + QUEUE_BLOCKS) % QUEUE_BLOCKS),
                        (unsigned)InterlockedCompareExchange(&queue->peak_blocks, 0, 0),
                        QUEUE_BLOCKS - 1);
            int written = mu16tty_resampler_process(
                resampler, samples, queue->frames[tail], converted, capacity);
            InterlockedExchange(&queue->tail, (tail + 1) % QUEUE_BLOCKS);
            if (written < 0)
                return written;
            if (mu16tty_push_pcm_f32(modem, converted, (size_t)written) != MU16TTY_OK ||
                drain_modem_events(modem, sink, decoder) != MU16TTY_OK)
                return MU16TTY_ERROR_INVALID_ARGUMENT;
        } else if (InterlockedCompareExchange(&queue->finished, 0, 0) != 0) {
            fputs("Audio input stopped.\n", stderr);
            return MU16TTY_ERROR_INVALID_ARGUMENT;
        } else {
            WaitForSingleObject(queue->ready, 100);
        }
    }
    return MU16TTY_OK;
}

static int listen_audio(HMODULE audio, int device, HANDLE ready, packet_sink sink,
                  audio_sink monitor, decoder_sink decoder, volatile LONG *retire,
                  volatile LONG *paused, int verify_pcm, unsigned *opened_rate)
{
    mu16tty_config config = {MU16TTY_ABI_VERSION, sizeof(config), MODEM_RATE, 0x0f, 64, 0};
    mu16tty_context *modem = NULL;
    mu16tty_resampler *resampler = NULL;
    receive_queue queue = {0};
    void *stream = NULL;
    float *converted = NULL;
    size_t capacity;
    int result;
    int close_result;

    selected_device = device < 0 ? mu16tty_windows_audio_default(audio, 1) : device;
    selected_rate = 0;
    result = mu16tty_windows_audio_devices(audio, find_input_rate);
    if (result < 0 || selected_rate == 0) {
        fprintf(stderr, "Input device %d is unavailable.\n", selected_device);
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    queue.block_frames = (selected_rate + 39u) / 40u;
    queue.report_queue = monitor != NULL;
    queue.paused = paused;
    if (verify_pcm)
        queue.first_pcm = ready;
    capacity = ((size_t)queue.block_frames * MODEM_RATE + selected_rate - 1u) / selected_rate;
    queue.samples = malloc((size_t)queue.block_frames * QUEUE_BLOCKS * sizeof(float));
    converted = malloc(capacity * sizeof(float));
    queue.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (queue.samples == NULL || converted == NULL || queue.ready == NULL) {
        result = MU16TTY_ERROR_NO_MEMORY;
        goto cleanup;
    }
    result = mu16tty_create(&config, &modem);
    if (result != MU16TTY_OK)
        goto cleanup;
    result = mu16tty_resampler_create(selected_rate, MODEM_RATE, &resampler);
    if (result != MU16TTY_OK)
        goto cleanup;
    result = mu16tty_windows_audio_input_with_context(
        audio, selected_device, selected_rate, queue.block_frames,
        capture_block, capture_finished, &queue, &stream);
    if (result != MU16TTY_OK)
        goto cleanup;
    if (opened_rate != NULL)
        *opened_rate = selected_rate;
    result = mu16tty_windows_audio_stream_control(audio, stream, 0); /* Start */
    if (result == MU16TTY_OK) {
        fprintf(stderr, "Listening on input %d at %u Hz. Press Ctrl+C to stop.\n",
                selected_device, selected_rate);
        if (ready != NULL && !verify_pcm)
            SetEvent(ready);
        result = process_input(&queue, modem, resampler, converted, capacity, sink, monitor, decoder,
                               retire, paused);
        mu16tty_windows_audio_stream_control(audio, stream, 2); /* Abort */
    }

cleanup:
    if (stream != NULL) {
        close_result = mu16tty_windows_audio_stream_control(audio, stream, 3); /* Close */
        if (result == MU16TTY_OK)
            result = close_result;
    }
    mu16tty_resampler_destroy(resampler);
    mu16tty_destroy(modem);
    if (queue.ready != NULL)
        CloseHandle(queue.ready);
    free(converted);
    free(queue.samples);
    return result;
}

static void find_output_rate(int index, const char *name, int inputs, int outputs,
                             double rate, const char *host, int host_type)
{
    (void)name;
    (void)inputs;
    (void)host;
    (void)host_type;
    if (index == selected_device && outputs > 0 && rate >= 8000 && rate <= 384000)
        selected_rate = (uint32_t)(rate + 0.5);
}

static int play_samples(HMODULE audio, int device, uint32_t rate, const float *pcm, size_t count)
{
    void *stream = NULL;
    unsigned long chunk = rate / 10;
    size_t offset;
    int result;
    int close_result;

    if (InterlockedCompareExchange(&stop_requested, 0, 0) != 0)
        return MU16TTY_OK;
    result = mu16tty_windows_audio_output(audio, device, rate, &stream);
    if (result != MU16TTY_OK)
        return result;
    result = mu16tty_windows_audio_stream_control(audio, stream, 0); /* Start */
    if (result == MU16TTY_OK) {
        for (offset = 0; offset < count; offset += chunk) {
            unsigned long frames = count - offset < chunk ? (unsigned long)(count - offset) : chunk;
            if (InterlockedCompareExchange(&stop_requested, 0, 0) != 0)
                break;
            result = mu16tty_windows_audio_write(audio, stream, pcm + offset, frames);
            if (result != MU16TTY_OK)
                break;
        }
        if (result == MU16TTY_OK && InterlockedCompareExchange(&stop_requested, 0, 0) == 0)
            result = mu16tty_windows_audio_stream_control(audio, stream, 1); /* Drain */
        else
            mu16tty_windows_audio_stream_control(audio, stream, 2); /* Abort */
    }
    close_result = mu16tty_windows_audio_stream_control(audio, stream, 3); /* Close */
    return result != MU16TTY_OK ? result : close_result;
}

typedef struct station_ptt {
    wchar_t name[8];
    int dtr;
    void *port;
} station_ptt;

static int transmit(HMODULE audio, int device, uint32_t rate, const float *pcm, size_t count,
                    const wchar_t *ptt_name, int dtr, station_ptt *persistent)
{
    void *port = NULL;
    uint32_t ptt_error;
    int result = MU16TTY_OK;

    if (persistent != NULL) {
        ptt_name = persistent->name[0] ? persistent->name : NULL;
        dtr = persistent->dtr;
        port = persistent->port;
    }
    if (ptt_name != NULL) {
        ptt_error = port != NULL ? ERROR_SUCCESS :
            mu16tty_windows_ptt_open(ptt_name, wcslen(ptt_name), &port);
        if (ptt_error != ERROR_SUCCESS) {
            fprintf(stderr, "Could not open PTT port (Windows error %u).\n", ptt_error);
            return MU16TTY_ERROR_INVALID_ARGUMENT;
        }
        if (InterlockedCompareExchange(&stop_requested, 0, 0) == 0) {
            ptt_error = mu16tty_windows_ptt_set(port, dtr, 1);
            if (ptt_error != ERROR_SUCCESS) {
                fprintf(stderr, "Could not assert PTT (Windows error %u).\n", ptt_error);
                result = MU16TTY_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (result == MU16TTY_OK)
        result = play_samples(audio, device, rate, pcm, count);
    if (port != NULL) {
        if (persistent != NULL) {
            ptt_error = mu16tty_windows_ptt_set(port, dtr, 0);
            if (ptt_error == ERROR_SUCCESS)
                persistent->port = port;
            else {
                mu16tty_windows_ptt_close(port);
                persistent->port = NULL;
            }
        } else {
            ptt_error = mu16tty_windows_ptt_close(port);
        }
        if (ptt_error != ERROR_SUCCESS) {
            fprintf(stderr, "Could not release PTT (Windows error %u).\n", ptt_error);
            if (result == MU16TTY_OK)
                result = MU16TTY_ERROR_INVALID_ARGUMENT;
        }
    }
    return result;
}

typedef struct tx_identity {
    uint64_t session_id;
    uint64_t next_sequence;
} tx_identity;

static int send_message(HMODULE audio, int device, unsigned mode, uint32_t symbol_rate,
                        const wchar_t *sender, const wchar_t *text,
                        const wchar_t *ptt_name, int dtr, tx_identity *identity,
                        station_ptt *ptt)
{
    char callsign[13];
    char message[281];
    uint8_t packet[MU16TTY_MAX_PACKET_BYTES];
    uint8_t *symbols = NULL;
    float *pcm = NULL;
    time_t timestamp = time(NULL);
    size_t wake, delay, tail;
    mu16tty_timing timing = {250, 600, 250};
    int callsign_size, message_size, packet_size, symbol_count, signal_frames, result;

    callsign_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, sender, -1,
                                      callsign, sizeof(callsign), NULL, NULL);
    message_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                     message, sizeof(message), NULL, NULL);
    if (callsign_size <= 1 || message_size <= 0) {
        fputs("Use a callsign of 1-12 ASCII bytes and a message of up to 280 UTF-8 bytes.\n", stderr);
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    if (timestamp < 0 || (uint64_t)timestamp > UINT32_MAX ||
        identity->next_sequence > UINT32_MAX)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    packet_size = mu16tty_pack_chat_packet(
        (uint8_t)mode, identity->session_id, (uint32_t)identity->next_sequence, (uint32_t)timestamp,
        (const uint8_t *)callsign, (size_t)(callsign_size - 1),
        (const uint8_t *)message, (size_t)(message_size - 1), packet, sizeof(packet));
    if (packet_size < 0) {
        fputs("Invalid callsign or message.\n", stderr);
        return packet_size;
    }

    selected_device = device;
    selected_rate = 0;
    result = mu16tty_windows_audio_devices(audio, find_output_rate);
    if (result < 0 || selected_rate == 0) {
        fprintf(stderr, "Output device %d is unavailable.\n", device);
        result = MU16TTY_ERROR_INVALID_ARGUMENT;
        goto cleanup;
    }
    result = mu16tty_windows_audio_output(audio, device, selected_rate, NULL);
    if (result != MU16TTY_OK)
        goto cleanup;
    symbol_count = mu16tty_encode_tx_symbols(packet, (size_t)packet_size, NULL, 0);
    if (symbol_count <= 0) {
        result = MU16TTY_ERROR_INVALID_ARGUMENT;
        goto cleanup;
    }
    symbols = malloc((size_t)symbol_count);
    if (symbols == NULL) {
        result = MU16TTY_ERROR_NO_MEMORY;
        goto cleanup;
    }
    result = mu16tty_encode_tx_symbols(packet, (size_t)packet_size, symbols, (size_t)symbol_count);
    if (result != symbol_count) {
        result = MU16TTY_ERROR_INVALID_ARGUMENT;
        goto cleanup;
    }

    if (ptt != NULL)
        timing = mu16tty_settings_timing();
    wake = ((size_t)selected_rate * timing.wake + 500) / 1000;
    delay = ((size_t)selected_rate * timing.delay + 500) / 1000;
    tail = ((size_t)selected_rate * timing.tail + 500) / 1000;
    signal_frames = mu16tty_render_tx_pcm_f32(
        symbols, (size_t)symbol_count, selected_rate, symbol_rate, wake, NULL, 0);
    if (signal_frames <= 0) {
        result = MU16TTY_ERROR_INVALID_ARGUMENT;
        goto cleanup;
    }
    pcm = calloc((size_t)signal_frames + delay + tail, sizeof(float));
    if (pcm == NULL) {
        result = MU16TTY_ERROR_NO_MEMORY;
        goto cleanup;
    }
    result = mu16tty_render_tx_pcm_f32(
        symbols, (size_t)symbol_count, selected_rate, symbol_rate, wake,
        pcm + delay, (size_t)signal_frames);
    if (result != signal_frames) {
        result = MU16TTY_ERROR_INVALID_ARGUMENT;
        goto cleanup;
    }
    fprintf(stderr, "Sending on output %d at %u sym/s. Press Ctrl+C to stop.\n", device, symbol_rate);
    ++identity->next_sequence;
    result = transmit(audio, device, selected_rate, pcm,
                      (size_t)signal_frames + delay + tail, ptt_name, dtr, ptt);
    if (InterlockedCompareExchange(&stop_requested, 0, 0) != 0)
        fputs("Playback cancelled.\n", stderr);

cleanup:
    if (ptt != NULL)
        mu16tty_http_record_transmission(packet, (size_t)packet_size, symbol_rate,
                                         result == MU16TTY_OK && !should_stop());
    free(pcm);
    free(symbols);
    return result;
}

typedef struct chat_transmitter {
    HMODULE audio;
    HANDLE receiver_ready;
    int device;
    unsigned mode;
    uint32_t rate;
    const wchar_t *sender;
    const wchar_t *ptt;
    int dtr;
    tx_identity identity;
} chat_transmitter;

static unsigned __stdcall read_and_send(void *data)
{
    chat_transmitter *tx = data;
    wchar_t line[512];
    while (WaitForSingleObject(tx->receiver_ready, 100) == WAIT_TIMEOUT) {
        if (InterlockedCompareExchange(&stop_requested, 0, 0) != 0)
            return 0;
    }
    fputs("Enter a message to send. Press Ctrl+C to exit.\n", stderr);
    while (InterlockedCompareExchange(&stop_requested, 0, 0) == 0 &&
           fgetws(line, ARRAYSIZE(line), stdin) != NULL) {
        size_t length = wcslen(line);
        int result;
        if (length == ARRAYSIZE(line) - 1 && line[length - 1] != L'\n') {
            wint_t next;
            do {
                next = fgetwc(stdin);
            } while (next != WEOF && next != L'\n');
            fputs("Message exceeds 280 UTF-8 bytes.\n", stderr);
            continue;
        }
        while (length > 0 && (line[length - 1] == L'\n' || line[length - 1] == L'\r'))
            line[--length] = L'\0';
        if (length == 0 || InterlockedCompareExchange(&stop_requested, 0, 0) != 0)
            continue;
        result = send_message(tx->audio, tx->device, tx->mode, tx->rate,
                              tx->sender, line, tx->ptt, tx->dtr, &tx->identity, 0);
        if (result != MU16TTY_OK)
            fprintf(stderr, "Message could not be sent (%d).\n", result);
    }
    InterlockedExchange(&stop_requested, 1);
    return 0;
}

static int chat(chat_transmitter *tx, int input_device)
{
    HANDLE worker;
    int result;
    if (_setmode(_fileno(stdin), _O_U8TEXT) < 0)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    tx->receiver_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (tx->receiver_ready == NULL)
        return MU16TTY_ERROR_NO_MEMORY;
    worker = (HANDLE)_beginthreadex(NULL, 0, read_and_send, tx, 0, NULL);
    if (worker == NULL) {
        CloseHandle(tx->receiver_ready);
        return MU16TTY_ERROR_NO_MEMORY;
    }
    result = listen_audio(tx->audio, input_device, tx->receiver_ready, print_packet, NULL, NULL, NULL, NULL, 0, NULL);
    InterlockedExchange(&stop_requested, 1);
    /* Repeat cancellation if the worker started a read just as RX exited. */
    while (WaitForSingleObject(worker, 100) == WAIT_TIMEOUT)
        CancelSynchronousIo(worker);
    CloseHandle(worker);
    CloseHandle(tx->receiver_ready);
    return result;
}

typedef struct http_receiver {
    HMODULE audio;
    HANDLE ready;
    int device;
    int result;
    volatile LONG retire;
    volatile LONG paused;
    HANDLE thread;
    int verify_pcm;
    unsigned sample_rate;
} http_receiver;

typedef struct http_input_controller {
    HMODULE audio;
    http_receiver *active;
} http_input_controller;

enum { HTTP_TX_CAPACITY = 16 };
static const uint32_t symbol_rates[] = {40, 20, 10, 4};

typedef struct http_tx_job {
    wchar_t message[281];
    uint32_t sequence;
    unsigned rate;
} http_tx_job;

typedef struct http_tx_queue {
    HMODULE audio;
    volatile LONG device;
    wchar_t callsign[13];
    char ascii_callsign[13];
    station_ptt ptt;
    SRWLOCK ptt_lock;
    uint64_t session_id;
    uint64_t next_sequence;
    HANDLE ready;
    SRWLOCK lock;
    unsigned head;
    unsigned count;
    http_tx_job jobs[HTTP_TX_CAPACITY];
} http_tx_queue;

static int normalize_station_callsign(const wchar_t *value, wchar_t name[13])
{
    size_t length = wcslen(value);
    int letter = 0, digit = 0;
    if (length == 0 || length > 12)
        return 0;
    for (size_t index = 0; index < length; ++index) {
        wchar_t ch = value[index];
        if (ch >= 0xff01 && ch <= 0xff5e)
            ch -= 0xfee0;
        if (ch >= L'a' && ch <= L'z')
            ch -= L'a' - L'A';
        if (ch >= L'A' && ch <= L'Z')
            letter = 1;
        else if (ch >= L'0' && ch <= L'9')
            digit = 1;
        else if (wcschr(L"/-+=._", ch) == NULL)
            return 0;
        name[index] = ch;
    }
    name[length] = 0;
    return letter && digit;
}

static int my_settings(void *context, const wchar_t *value, char *message, size_t capacity, int *changed)
{
    http_tx_queue *queue = context;
    wchar_t name[13];
    size_t length = wcslen(value);
    if (length != 0 && !normalize_station_callsign(value, name)) {
        snprintf(message, capacity, "Use my [callsign]: up to 12 characters (A-Z, 0-9, /-+=._), including a letter and a digit.");
        return 0;
    }
    if (length != 0 && wcscmp(name, queue->callsign) != 0) {
        if (!mu16tty_settings_my(name, 1)) {
            snprintf(message, capacity, "Could not save config/settings.json.");
            return -1;
        }
        AcquireSRWLockExclusive(&queue->lock);
        wcscpy_s(queue->callsign, ARRAYSIZE(queue->callsign), name);
        WideCharToMultiByte(CP_UTF8, 0, name, -1, queue->ascii_callsign,
                            sizeof(queue->ascii_callsign), NULL, NULL);
        ReleaseSRWLockExclusive(&queue->lock);
        *changed = 1;
    }
    snprintf(message, capacity, "My callsign: %s%s", queue->ascii_callsign,
             length ? "" : "\n\nmy [callsign] : Set callsign");
    return 1;
}

static int valid_com_name(const wchar_t *name)
{
    size_t length = wcslen(name);
    return length >= 4 && length <= 7 && _wcsnicmp(name, L"COM", 3) == 0 &&
        name[3] >= L'1' && name[3] <= L'9' &&
        wcsspn(name + 4, L"0123456789") == length - 4;
}

static void ptt_status(void *context, char port[8], int *dtr, int *open)
{
    http_tx_queue *queue = context;
    station_ptt *ptt = &queue->ptt;
    /* Configuration changes run on the same HTTP thread as this snapshot. */
    *port = '\0';
    if (valid_com_name(ptt->name))
        WideCharToMultiByte(CP_UTF8, 0, ptt->name, -1, port, 8, NULL, NULL);
    *dtr = ptt->dtr;
    if (TryAcquireSRWLockShared(&queue->ptt_lock)) {
        *open = ptt->port != NULL;
        ReleaseSRWLockShared(&queue->ptt_lock);
    } else {
        *open = -1;
    }
}

static int ptt_settings(void *context, const wchar_t *args, char *message, size_t capacity, int *changed)
{
    http_tx_queue *queue = context;
    station_ptt *ptt = &queue->ptt;
    wchar_t name[8], token[8];
    int dtr, configure = 0, open_port = 0, close_port = 0, result = 1;
    uint32_t error = ERROR_SUCCESS;
    if (!TryAcquireSRWLockExclusive(&queue->ptt_lock)) {
        if (*args == L'\0') {
            snprintf(message, capacity, "PTT / TX");
            return 1;
        }
        snprintf(message, capacity, "PTT settings are locked during transmission.");
        return 0;
    }
    wcscpy_s(name, ARRAYSIZE(name), ptt->name);
    dtr = ptt->dtr;
    if (_wcsicmp(args, L"open") == 0) {
        open_port = 1;
    } else if (_wcsicmp(args, L"close") == 0) {
        close_port = 1;
    } else if (_wcsicmp(args, L"none") == 0) {
        name[0] = L'\0';
        configure = 1;
    } else if (*args != L'\0') {
        size_t length = wcscspn(args, L" \t");
        if (length >= ARRAYSIZE(token))
            goto invalid;
        wmemcpy(token, args, length);
        token[length] = L'\0';
        args += length;
        args += wcsspn(args, L" \t");
        if (valid_com_name(token)) {
            size_t index;
            for (index = 0; index <= length; ++index)
                name[index] = (wchar_t)towupper(token[index]);
        } else if (*args == L'\0') {
            args = token;
        } else {
            goto invalid;
        }
        if (_wcsicmp(args, L"dtr") == 0)
            dtr = 1;
        else if (_wcsicmp(args, L"rts") == 0)
            dtr = 0;
        else if (*args != L'\0')
            goto invalid;
        configure = 1;
        open_port = name[0] != L'\0';
    }
    if (configure && (wcscmp(name, ptt->name) != 0 || dtr != ptt->dtr)) {
        if (!mu16tty_settings_ptt(name, &dtr, 1)) {
            snprintf(message, capacity, "Could not save config/settings.json.");
            result = -1;
            goto done;
        }
        close_port = 1;
        wcscpy_s(ptt->name, ARRAYSIZE(ptt->name), name);
        ptt->dtr = dtr;
        *changed = 1;
    }
    if (close_port && ptt->port != NULL) {
        error = mu16tty_windows_ptt_close(ptt->port);
        ptt->port = NULL;
        *changed = 1;
    }
    if (error == ERROR_SUCCESS && open_port && ptt->port == NULL) {
        if (ptt->name[0] == L'\0')
            goto invalid;
        error = mu16tty_windows_ptt_open(ptt->name, wcslen(ptt->name), &ptt->port);
        if (error == ERROR_SUCCESS)
            *changed = 1;
    }
    if (error == ERROR_SUCCESS)
        snprintf(message, capacity, "PTT %ls %s%s", *ptt->name ? ptt->name : L"none",
                 ptt->dtr ? "DTR" : "RTS", ptt->port ? " / OPEN" : *ptt->name ? " / CLOSED" : "");
    else {
        snprintf(message, capacity, "PTT %ls / CLOSED (Windows error %u).", ptt->name, error);
        result = 0;
    }
    goto done;

invalid:
    snprintf(message, capacity, "Use ptt [open|close|none|COMx [rts|dtr]|rts|dtr].");
    result = 0;
done:
    ReleaseSRWLockExclusive(&queue->ptt_lock);
    return result;
}

static int enqueue_http_message(void *context, const wchar_t *message, unsigned rate,
                                uint32_t *sequence, unsigned *depth)
{
    http_tx_queue *queue = context;
    int accepted = 0;
    AcquireSRWLockExclusive(&queue->lock);
    if (!should_stop() && queue->count < HTTP_TX_CAPACITY && queue->next_sequence <= UINT32_MAX) {
        http_tx_job *job = &queue->jobs[(queue->head + queue->count) % HTTP_TX_CAPACITY];
        wcscpy_s(job->message, ARRAYSIZE(job->message), message);
        job->rate = rate;
        job->sequence = (uint32_t)queue->next_sequence++;
        *sequence = job->sequence;
        *depth = ++queue->count;
        accepted = 1;
    }
    ReleaseSRWLockExclusive(&queue->lock);
    if (accepted)
        SetEvent(queue->ready);
    return accepted;
}

static unsigned __stdcall send_for_http(void *context)
{
    http_tx_queue *queue = context;
    while (!should_stop()) {
        http_tx_job job = {0};
        wchar_t sender[13];
        int available;
        unsigned mode;
        AcquireSRWLockExclusive(&queue->lock);
        available = queue->count != 0;
        if (available) {
            wcscpy_s(sender, ARRAYSIZE(sender), queue->callsign);
            job = queue->jobs[queue->head];
            queue->head = (queue->head + 1) % HTTP_TX_CAPACITY;
            --queue->count;
        }
        ReleaseSRWLockExclusive(&queue->lock);
        if (!available) {
            WaitForSingleObject(queue->ready, 100);
            continue;
        }
        for (mode = 0; mode < ARRAYSIZE(symbol_rates); ++mode) {
            if (job.rate == symbol_rates[mode]) {
                tx_identity identity = {queue->session_id, job.sequence};
                int device = InterlockedCompareExchange(&queue->device, 0, 0);
                AcquireSRWLockExclusive(&queue->ptt_lock);
                send_message(queue->audio, device, mode, job.rate, sender,
                             job.message, NULL, 0, &identity, &queue->ptt);
                ReleaseSRWLockExclusive(&queue->ptt_lock);
                break;
            }
        }
    }
    return 0;
}

static unsigned __stdcall receive_for_http(void *data)
{
    http_receiver *receiver = data;
    receiver->result = listen_audio(receiver->audio, receiver->device, receiver->ready,
                              mu16tty_http_record_packet, mu16tty_http_observe_audio,
                              mu16tty_http_observe_decoder, &receiver->retire, &receiver->paused,
                              receiver->verify_pcm, &receiver->sample_rate);
    return 0;
}

static int close_http_receiver(http_receiver *receiver)
{
    int result;
    InterlockedExchange(&receiver->retire, 1);
    if (receiver->thread != NULL) {
        WaitForSingleObject(receiver->thread, INFINITE);
        CloseHandle(receiver->thread);
    }
    if (receiver->ready != NULL)
        CloseHandle(receiver->ready);
    result = receiver->result;
    free(receiver);
    return result;
}

static http_receiver *open_http_receiver(HMODULE audio, int device, int verify_pcm)
{
    http_receiver *receiver = calloc(1, sizeof(*receiver));
    HANDLE waiting[2];
    if (receiver == NULL)
        return NULL;
    receiver->audio = audio;
    receiver->device = device;
    receiver->paused = 1;
    receiver->verify_pcm = verify_pcm;
    receiver->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (receiver->ready != NULL) {
        receiver->thread = (HANDLE)_beginthreadex(NULL, 0, receive_for_http, receiver, 0, NULL);
        waiting[0] = receiver->thread;
        waiting[1] = receiver->ready;
        if (receiver->thread != NULL &&
            WaitForMultipleObjects(2, waiting, FALSE, 3000) == WAIT_OBJECT_0 + 1 &&
            (!verify_pcm || WaitForSingleObject(receiver->thread, 250) == WAIT_TIMEOUT) &&
            !should_stop())
            return receiver;
    }
    close_http_receiver(receiver);
    return NULL;
}

static int change_http_input(void *context, int device, int automatic)
{
    http_input_controller *input = context;
    http_receiver *replacement = NULL;
    if (device != input->active->device ||
        WaitForSingleObject(input->active->thread, 0) == WAIT_OBJECT_0) {
        replacement = open_http_receiver(input->audio, device, 1);
        if (replacement == NULL)
            return 0;
    }
    if (!mu16tty_settings_save_audio_device(1, device, automatic)) {
        if (replacement != NULL)
            close_http_receiver(replacement);
        return -1;
    }
    if (replacement != NULL) {
        close_http_receiver(input->active);
        input->active = replacement;
        mu16tty_http_observe_decoder(NULL);
        InterlockedExchange(&replacement->paused, 0);
    }
    return 1;
}

static unsigned http_input_rate(void *context)
{
    http_input_controller *input = context;
    return input->active->sample_rate;
}

static unsigned http_tx_depth(void *context)
{
    http_tx_queue *queue = context;
    unsigned count;
    AcquireSRWLockShared(&queue->lock);
    count = queue->count;
    ReleaseSRWLockShared(&queue->lock);
    return count;
}

static int http_input_running(void *context)
{
    const http_input_controller *input = context;
    return WaitForSingleObject(input->active->thread, 0) == WAIT_TIMEOUT;
}

static int serve_with_receiver(unsigned short port, int device, int output,
                               const wchar_t *callsign, const wchar_t *ptt, int dtr,
                               int open_browser)
{
    http_receiver receiver = {0};
    http_input_controller input = {0};
    mu16tty_http_input input_api = {0};
    int result = 1;
    HANDLE tx_thread = NULL;
    http_tx_queue tx = {0};
    char ascii_callsign[13];
    mu16tty_http_transmitter tx_api = {0};
    receiver.audio = load_portaudio();
    if (receiver.audio == NULL) {
        fputs("Could not load PortAudio.\n", stderr);
        return 1;
    }
    receiver.result = mu16tty_windows_audio_initialize(receiver.audio, 1);
    if (receiver.result != MU16TTY_OK) {
        fprintf(stderr, "PortAudio initialization failed (%d).\n", receiver.result);
        FreeLibrary(receiver.audio);
        return 1;
    }
    device = mu16tty_settings_audio_device(receiver.audio, 1, device);
    if (open_browser)
        output = mu16tty_windows_audio_default(receiver.audio, 0);
    if (open_browser || output >= 0)
        output = mu16tty_settings_audio_device(receiver.audio, 0, output);
    if (open_browser && output < 0)
        goto cleanup;
    if (output >= 0) {
        size_t index;
        size_t length = wcslen(callsign);
        if (length == 0 || length >= sizeof(ascii_callsign)) {
            fputs("Use a callsign of 1-12 ASCII bytes.\n", stderr);
            goto cleanup;
        }
        for (index = 0; index < length; ++index) {
            if (callsign[index] < 0x20 || callsign[index] > 0x7e) {
                fputs("Use a printable ASCII callsign.\n", stderr);
                goto cleanup;
            }
            ascii_callsign[index] = (char)callsign[index];
        }
        ascii_callsign[length] = '\0';
        selected_device = output;
        selected_rate = 0;
        if (mu16tty_windows_audio_devices(receiver.audio, find_output_rate) < 0 ||
            selected_rate == 0 ||
            mu16tty_windows_audio_output(receiver.audio, output, selected_rate, NULL) != MU16TTY_OK) {
            fprintf(stderr, "Output device %d is unavailable.\n", output);
            goto cleanup;
        }
        if (BCryptGenRandom(NULL, (PUCHAR)&tx.session_id, sizeof(tx.session_id),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            goto cleanup;
        tx.audio = receiver.audio;
        tx.device = output;
        wcscpy_s(tx.callsign, ARRAYSIZE(tx.callsign), callsign);
        strcpy_s(tx.ascii_callsign, sizeof(tx.ascii_callsign), ascii_callsign);
        mu16tty_settings_my(tx.callsign, 0);
        wchar_t normalized[13];
        if (normalize_station_callsign(tx.callsign, normalized))
            wcscpy_s(tx.callsign, ARRAYSIZE(tx.callsign), normalized);
        else
            wcscpy_s(tx.callsign, ARRAYSIZE(tx.callsign), L"NOCALL");
        WideCharToMultiByte(CP_UTF8, 0, tx.callsign, -1, tx.ascii_callsign,
                            sizeof(tx.ascii_callsign), NULL, NULL);
        tx.ptt.dtr = dtr;
        if (ptt != NULL)
            wcscpy_s(tx.ptt.name, ARRAYSIZE(tx.ptt.name), ptt);
        else
            mu16tty_settings_ptt(tx.ptt.name, &tx.ptt.dtr, 0);
        if (tx.ptt.name[0] != L'\0') {
            uint32_t error = mu16tty_windows_ptt_open(tx.ptt.name, wcslen(tx.ptt.name), &tx.ptt.port);
            if (error != ERROR_SUCCESS)
                fprintf(stderr, "Could not open PTT port (Windows error %u).\n", error);
        }
        InitializeSRWLock(&tx.lock);
        InitializeSRWLock(&tx.ptt_lock);
        tx.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (tx.ready == NULL)
            goto cleanup;
        tx_thread = (HANDLE)_beginthreadex(NULL, 0, send_for_http, &tx, 0, NULL);
        if (tx_thread == NULL)
            goto cleanup;
        tx_api.output_device = &tx.device;
        tx_api.callsign = tx.ascii_callsign;
        tx_api.session_id = tx.session_id;
        tx_api.context = &tx;
        tx_api.queue_depth = http_tx_depth;
        tx_api.queue_capacity = HTTP_TX_CAPACITY;
        tx_api.enqueue = enqueue_http_message;
        tx_api.ptt_command = ptt_settings;
        tx_api.my_command = my_settings;
        tx_api.ptt_status = ptt_status;
    }
    input.audio = receiver.audio;
    input.active = open_http_receiver(receiver.audio, device, 0);
    if (input.active != NULL) {
        input_api.device = input.active->device;
        input_api.context = &input;
        input_api.change = change_http_input;
        input_api.sample_rate = http_input_rate;
        input_api.running = http_input_running;
        InterlockedExchange(&input.active->paused, 0);
        result = mu16tty_serve_local_http(port, &input_api, tx_thread ? &tx_api : NULL,
                                          receiver.audio, should_stop, open_browser);
        InterlockedExchange(&stop_requested, 1);
        close_http_receiver(input.active);
    }
cleanup:
    InterlockedExchange(&stop_requested, 1);
    if (tx_thread != NULL) {
        SetEvent(tx.ready);
        WaitForSingleObject(tx_thread, INFINITE);
        CloseHandle(tx_thread);
    }
    if (tx.ready != NULL)
        CloseHandle(tx.ready);
    if (tx.ptt.port != NULL)
        mu16tty_windows_ptt_close(tx.ptt.port);
    if (mu16tty_windows_audio_initialize(receiver.audio, 0) != MU16TTY_OK)
        result = 1;
    FreeLibrary(receiver.audio);
    return result;
}

static int parse_device(const wchar_t *text)
{
    wchar_t *end;
    long parsed = wcstol(text, &end, 10);
    if (end == text || *end != L'\0' || parsed < 0 || parsed >= INT_MAX)
        return -1;
    return (int)parsed;
}

int wmain(int argc, wchar_t **argv)
{
    HMODULE audio;
    int result;
    int shutdown;
    int listen_mode = argc >= 2 && wcscmp(argv[1], L"--listen") == 0;
    int send_mode = argc >= 2 && wcscmp(argv[1], L"--send") == 0;
    int chat_mode = argc >= 2 && wcscmp(argv[1], L"--chat") == 0;
    int serve_mode = argc >= 2 && wcscmp(argv[1], L"--serve") == 0;
    int transmit_mode = send_mode || chat_mode;
    unsigned mode = 0;
    int device = -1;
    int chat_output = -1;
    chat_transmitter transmitter = {0};
    wchar_t ptt_name[8] = {0};
    const wchar_t *ptt = NULL;
    int dtr = 0;
    int send_argc = argc;
    UINT previous_code_page;

    if (argc == 2 && wcscmp(argv[1], L"--ptt-list") == 0)
        return list_ptt_ports();
    if (argc == 1) {
        wchar_t callsign[13];
        HANDLE lock = lock_station_folder();
        if (lock == NULL)
            return 1;
        station_shutdown = CreateEventW(NULL, TRUE, FALSE, NULL);
        result = 1;
        if (station_shutdown != NULL && mu16tty_settings_load() && mu16tty_settings_callsign(callsign) &&
            SetConsoleCtrlHandler(stop_audio, TRUE)) {
            if (mu16tty_http_open_history()) {
                result = serve_with_receiver(0, -1, -2, callsign, NULL, 0, 1);
                mu16tty_http_close_history();
            }
        }
        CloseHandle(lock);
        if (station_shutdown != NULL)
            SetEvent(station_shutdown);
        if (result != 0 && !should_stop()) {
            fputs("Station startup failed. Check config/station.json and config/settings.json.\n", stderr);
            if (GetConsoleWindow() != NULL)
                WaitForSingleObject(GetStdHandle(STD_INPUT_HANDLE), 30000);
        }
        return result;
    }
    if ((transmit_mode || serve_mode) && argc >= 8 && wcscmp(argv[argc - 3], L"--ptt") == 0) {
        const wchar_t *name = argv[argc - 2];
        const wchar_t *line = argv[argc - 1];
        size_t length = wcslen(name);
        size_t index;
        if (length < 4 || length >= ARRAYSIZE(ptt_name) ||
            (_wcsicmp(line, L"rts") != 0 && _wcsicmp(line, L"dtr") != 0)) {
            fputs("Use --ptt COM<number> rts or --ptt COM<number> dtr.\n", stderr);
            return 2;
        }
        for (index = 0; index < length; ++index)
            ptt_name[index] = (wchar_t)towupper(name[index]);
        dtr = _wcsicmp(line, L"dtr") == 0;
        ptt = ptt_name;
        send_argc -= 3;
    }
    if (serve_mode) {
        int port = send_argc >= 3 ? parse_device(argv[2]) : 8766;
        int input = send_argc >= 4 ? parse_device(argv[3]) : -1;
        int output = send_argc == 6 ? parse_device(argv[4]) : -1;
        if (send_argc == 5 || send_argc > 6 || port < 0 || port > 65535 ||
            (send_argc >= 4 && input < 0) || (send_argc == 6 && output < 0) ||
            (ptt != NULL && output < 0)) {
            fputs("Use --serve [port] [input-index [output-index callsign]] [--ptt COMx rts|dtr].\n",
                  stderr);
            return 2;
        }
        if (!SetConsoleCtrlHandler(stop_audio, TRUE))
            return 1;
        result = 1;
        if (mu16tty_settings_load() && mu16tty_http_open_history()) {
            result = input >= 0
                ? serve_with_receiver((unsigned short)port, input, output,
                                       output >= 0 ? argv[5] : NULL, ptt, dtr, 0)
                : mu16tty_serve_local_http((unsigned short)port, NULL, NULL, NULL, should_stop, 0);
            mu16tty_http_close_history();
        }
        SetConsoleCtrlHandler(stop_audio, FALSE);
        return result;
    }
    if (transmit_mode && (send_argc == 5 || send_argc == 6)) {
        int rate = send_argc == 6 ? parse_device(argv[5]) : 40;
        while (mode < ARRAYSIZE(symbol_rates) && rate != (int)symbol_rates[mode])
            ++mode;
        if (mode == ARRAYSIZE(symbol_rates)) {
            fputs("Symbol rate must be 40, 20, 10 or 4.\n", stderr);
            return 2;
        }
    } else if (!(listen_mode && (argc == 2 || argc == 3)) &&
               !(argc == 2 && wcscmp(argv[1], L"--devices") == 0)) {
        fputs("Usage: Mu16TTY-native --devices | --ptt-list | --listen [input-index]\n"
              "       Mu16TTY-native --serve [port] [input-index [output-index callsign]] [--ptt COMx rts|dtr]\n"
              "       Mu16TTY-native --send output-index callsign message [symbol-rate] [--ptt COMx rts|dtr]\n"
              "       Mu16TTY-native --chat input-index output-index callsign [symbol-rate] [--ptt COMx rts|dtr]\n",
              stderr);
        return 2;
    }
    if (transmit_mode || (listen_mode && argc == 3)) {
        device = parse_device(argv[2]);
        if (device < 0) {
            fputs("Device must be a nonnegative device index.\n", stderr);
            return 2;
        }
    }
    if (chat_mode && (chat_output = parse_device(argv[3])) < 0) {
        fputs("Output device must be a nonnegative device index.\n", stderr);
        return 2;
    }
    if (transmit_mode && BCryptGenRandom(NULL, (PUCHAR)&transmitter.identity.session_id,
                                        sizeof(transmitter.identity.session_id),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        fputs("Could not generate a transmit session ID.\n", stderr);
        return 1;
    }
    if ((transmit_mode || listen_mode) && !SetConsoleCtrlHandler(stop_audio, TRUE)) {
        fputs("Could not install the console stop handler.\n", stderr);
        return 1;
    }
    audio = load_portaudio();
    if (audio == NULL) {
        fputs("Could not load PortAudio from runtime\\portaudio next to the executable.\n", stderr);
        return 1;
    }
    result = mu16tty_windows_audio_initialize(audio, 1);
    if (result < 0) {
        fprintf(stderr, "PortAudio initialization failed (%d).\n", result);
        FreeLibrary(audio);
        return 1;
    }

    previous_code_page = GetConsoleOutputCP();
    if (previous_code_page != 0)
        SetConsoleOutputCP(CP_UTF8);
    if (chat_mode) {
        transmitter.audio = audio;
        transmitter.device = chat_output;
        transmitter.mode = mode;
        transmitter.rate = symbol_rates[mode];
        transmitter.sender = argv[4];
        transmitter.ptt = ptt;
        transmitter.dtr = dtr;
        result = chat(&transmitter, device);
    } else if (send_mode) {
        result = send_message(audio, device, mode, symbol_rates[mode], argv[3], argv[4],
                              ptt, dtr, &transmitter.identity, 0);
    } else if (listen_mode) {
        result = listen_audio(audio, device, NULL, print_packet, NULL, NULL, NULL, NULL, 0, NULL);
    } else {
        puts("Index\tInputs\tOutputs\tRate\tHost API\tDevice");
        result = mu16tty_windows_audio_devices(audio, print_device);
        if (result >= 0) {
            printf("Default input\t%d\n", mu16tty_windows_audio_default(audio, 1));
            printf("Default output\t%d\n", mu16tty_windows_audio_default(audio, 0));
        }
    }
    fflush(stdout);
    if (previous_code_page != 0)
        SetConsoleOutputCP(previous_code_page);

    shutdown = mu16tty_windows_audio_initialize(audio, 0);
    FreeLibrary(audio);
    if (transmit_mode || listen_mode)
        SetConsoleCtrlHandler(stop_audio, FALSE);
    if (result < 0 || shutdown < 0) {
        fprintf(stderr, "Audio operation failed (%d, %d).\n", result, shutdown);
        return 1;
    }
    return 0;
}
