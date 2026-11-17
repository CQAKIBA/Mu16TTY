#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include "mu16tty_station_http_config.inc"

#include "mu16tty_station_http.h"
#include "mu16tty_station_settings.h"
#include "mu16tty_windows_audio.h"
#include "mu16tty_windows_ptt.h"

static volatile LONG log_all = 1;
static void record_damaged_packet(const mu16tty_event *event);

/* The HTTP dispatcher handles one settings request at a time. */
static struct {
    char text[8192];
    size_t used;
    int input;
    int output;
    int direction;
    int all;
    int has_mme;
    int candidate;
    double candidate_rate;
} device_listing;

static void list_audio_device(int index, const char *name, int inputs, int outputs,
                              double rate, const char *host, int host_type)
{
    int selected = device_listing.direction == 1 ? index == device_listing.input
                                                   : index == device_listing.output;
    wchar_t wide[512];
    char utf8[2048];
    int written;
    if (index == device_listing.candidate &&
        (device_listing.direction == 1 ? inputs : outputs) > 0)
        device_listing.candidate_rate = rate;
    if (device_listing.direction == 0)
        selected = index == device_listing.input || index == device_listing.output;
    if ((device_listing.direction == 1 && !inputs) || (device_listing.direction == 2 && !outputs))
        return;
    if (device_listing.direction == 0 && !selected)
        return;
    if (!device_listing.all && !selected && device_listing.has_mme &&
        (host_type != 2 || strncmp(name, "Microsoft Sound Mapper", 22) == 0))
        return;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, ARRAYSIZE(wide)) &&
        MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide)) &&
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), NULL, NULL))
        name = utf8;
    written = snprintf(device_listing.text + device_listing.used,
                       sizeof(device_listing.text) - device_listing.used,
                       "%s%s%3d  %s [%s] / %.0f Hz\n",
                       device_listing.direction == 0 ? (index == device_listing.input ? "IN  " : "OUT ") : "",
                       selected ? ">> " : "   ",
                       index, name, host, rate);
    if (written > 0 && (size_t)written < sizeof(device_listing.text) - device_listing.used)
        device_listing.used += (size_t)written;
    else
        device_listing.text[device_listing.used] = '\0';
}

static void find_mme(int index, const char *name, int inputs, int outputs,
                     double rate, const char *host, int host_type)
{
    (void)index;
    (void)rate;
    (void)host;
    if (host_type == 2 && (device_listing.direction == 1 ? inputs : outputs) > 0 &&
        strncmp(name, "Microsoft Sound Mapper", 22) != 0)
        device_listing.has_mme = 1;
}

static void list_ptt_device(const wchar_t *port, const wchar_t *name,
                            const wchar_t *product, const wchar_t *instance)
{
    wchar_t line[1100];
    int written;
    (void)instance;
    if (swprintf_s(line, ARRAYSIZE(line), L"%ls  %ls%ls%ls\n", port, *name ? name : port,
                   *product ? L" | " : L"", product) < 0)
        return;
    written = WideCharToMultiByte(CP_UTF8, 0, line, -1, device_listing.text + device_listing.used,
                                  (int)(sizeof(device_listing.text) - device_listing.used), NULL, NULL);
    if (written > 0)
        device_listing.used += (size_t)written - 1;
    else
        device_listing.text[device_listing.used] = '\0';
}

static int hardware_settings(const wchar_t *command, void *audio, mu16tty_http_input *receiver,
                             volatile LONG *output_device,
                             char *message, size_t capacity, int *changed)
{
    int input = receiver != NULL ? receiver->device : -1;
    int output = output_device != NULL ? InterlockedCompareExchange(output_device, 0, 0) : -1;
    int sound = _wcsicmp(command, L"sound") == 0;
    int input_list = _wcsicmp(command, L"sound in") == 0 || _wcsicmp(command, L"sound in all") == 0;
    int output_list = _wcsicmp(command, L"sound out") == 0 || _wcsicmp(command, L"sound out all") == 0;
    int ptt_list = _wcsicmp(command, L"ptt list") == 0;
    if (_wcsnicmp(command, L"loglevel", 8) == 0 &&
        (command[8] == L'\0' || command[8] == L' ' || command[8] == L'\t')) {
        const wchar_t *value = command + 8;
        value += wcsspn(value, L" \t");
        if (*value != L'\0') {
            int all = _wcsicmp(value, L"all") == 0;
            if (!all && _wcsicmp(value, L"valid") != 0) {
                snprintf(message, capacity, "Use loglevel [valid|all].");
                return 0;
            }
            *changed = InterlockedExchange(&log_all, all) != all;
        }
        snprintf(message, capacity, "Log level: %s%s",
            InterlockedCompareExchange(&log_all, 0, 0) ? "all" : "valid",
            *value ? "" : "\n\nloglevel all   : Include CRC failures\nloglevel valid : Only log CRC-valid receptions");
        return 1;
    }
    int input_setting = _wcsnicmp(command, L"sound in ", 9) == 0;
    int output_setting = _wcsnicmp(command, L"sound out ", 10) == 0;
    if (!input_list && !output_list && (input_setting || output_setting)) {
        const wchar_t *argument = command + (input_setting ? 9 : 10);
        argument += wcsspn(argument, L" \t\u3000");
        int automatic = _wcsicmp(argument, L"auto") == 0 || _wcsicmp(argument, L"default") == 0;
        wchar_t *end = NULL;
        long index = automatic && audio != NULL ? mu16tty_windows_audio_default(audio, input_setting)
                                                : wcstol(argument, &end, 10);
        if (audio == NULL || index < 0 || index >= INT_MAX ||
            (!automatic && (end == argument || *end != L'\0')) ||
            (input_setting ? receiver == NULL || receiver->change == NULL : output_device == NULL)) {
            snprintf(message, capacity, "Use sound in/out with an available device index or auto.");
            return 0;
        }
        memset(&device_listing, 0, sizeof(device_listing));
        device_listing.direction = input_setting ? 1 : 2;
        device_listing.all = 1;
        device_listing.candidate = (int)index;
        if (mu16tty_windows_audio_devices(audio, list_audio_device) < 0 ||
            device_listing.candidate_rate <= 0 ||
            (!input_setting && (device_listing.candidate_rate < 8000 ||
             mu16tty_windows_audio_output(audio, (int)index, device_listing.candidate_rate, NULL) != 0))) {
            snprintf(message, capacity, "Audio device %ld is unavailable for this direction.", index);
            return 0;
        }
        int saved = input_setting ? receiver->change(receiver->context, (int)index, automatic)
                                  : mu16tty_settings_save_audio_device(0, (int)index, automatic) ? 1 : -1;
        if (saved == 0) {
            snprintf(message, capacity, "Could not open input device %ld. Current input retained.", index);
            return 0;
        }
        if (saved < 0) {
            snprintf(message, capacity, "Could not save config/settings.json.");
            return -1;
        }
        *changed = 1;
        if (input_setting)
            receiver->device = (int)index;
        else
            InterlockedExchange(output_device, (LONG)index);
        snprintf(message, capacity, "Saved sound %s %s%ld. %s",
                 input_setting ? "in" : "out", automatic ? "auto -> " : "", index,
                 input_setting ? "Input switched." : "Applies to the next transmission.");
        return 1;
    }
    if (!sound && !input_list && !output_list && !ptt_list)
        return -2;
    memset(&device_listing, 0, sizeof(device_listing));
    device_listing.input = input;
    device_listing.output = output;
    device_listing.direction = input_list ? 1 : output_list ? 2 : 0;
    device_listing.all = _wcsicmp(command, L"sound in all") == 0 ||
                         _wcsicmp(command, L"sound out all") == 0;
    if (ptt_list) {
        uint32_t result = mu16tty_windows_ptt_list(list_ptt_device);
        if (result != 0) {
            snprintf(message, capacity, "Could not list COM ports (Windows error %u).", result);
            return 0;
        }
    } else if (audio != NULL) {
        mu16tty_windows_audio_devices(audio, find_mme);
        if (mu16tty_windows_audio_devices(audio, list_audio_device) < 0) {
            snprintf(message, capacity, "Could not list audio devices.");
            return 0;
        }
    }
    snprintf(message, capacity, "%s%s", device_listing.used ? device_listing.text :
             ptt_list ? "No serial ports available." : "No active audio devices.",
             sound ? "\n\nsound in|out        : Show devices\nsound in|out all    : List all devices\n"
                     "sound in|out index  : Set audio port\nsound in|out auto   : Set system default" : "");
    return 1;
}

enum { HISTORY_JSON_CAPACITY = 2 * 1024 * 1024 };
enum { WAVEFORM_POINTS = 128, SPECTRUM_BINS = 96 };

typedef struct monitor_snapshot {
    uint64_t revision;
    double levels[2];
    unsigned queued, queue_peak, queue_capacity;
    double waveform[WAVEFORM_POINTS];
    double spectrum[SPECTRUM_BINS];
} monitor_snapshot;

static SRWLOCK monitor_lock = SRWLOCK_INIT;
static volatile LONG queue_reading;

void mu16tty_http_observe_queue(unsigned used, unsigned peak, unsigned capacity)
{
    InterlockedExchange(&queue_reading, (LONG)(used | (peak << 8) | (capacity << 16)));
}
static monitor_snapshot monitor;

enum { DECODER_RATES = 4, DECODER_ROWS = 8, DECODER_LOG_CAPACITY = 64 };
static const uint32_t decoder_rates[DECODER_RATES] = {40, 20, 10, 4};
static const char *const decoder_names[DECODER_RATES] = {"Normal 40", "Robust 20", "Slow 10", "Music 4"};

typedef struct decoder_state {
    const char *phase;
    const char *detail;
    double started_at;
    ULONGLONG until;
} decoder_state;

typedef struct decoder_transition {
    unsigned row;
    const char *phase;
    const char *detail;
    double at;
} decoder_transition;

typedef struct decoder_preview {
    ULONGLONG until;
    size_t size;
    uint8_t bytes[MU16TTY_MAX_PACKET_BYTES];
} decoder_preview;

static SRWLOCK decoder_lock = SRWLOCK_INIT;
static decoder_state decoders[DECODER_ROWS];
static decoder_transition decoder_log[DECODER_LOG_CAPACITY];
static unsigned decoder_log_next;
static unsigned decoder_log_size;
static decoder_preview previews[DECODER_RATES];

static void set_decoder(unsigned row, const char *phase, const char *detail,
                        double timestamp, ULONGLONG now, ULONGLONG duration)
{
    decoder_state *state = &decoders[row];
    if (state->until <= now || strcmp(state->phase, phase) != 0 || strcmp(state->detail, detail) != 0) {
        decoder_transition *entry = &decoder_log[decoder_log_next];
        entry->row = row;
        entry->phase = phase;
        entry->detail = detail;
        entry->at = timestamp;
        decoder_log_next = (decoder_log_next + 1) % DECODER_LOG_CAPACITY;
        if (decoder_log_size < DECODER_LOG_CAPACITY)
            ++decoder_log_size;
        state->started_at = timestamp;
    }
    state->phase = phase;
    state->detail = detail;
    state->until = now + duration;
}

void mu16tty_http_observe_decoder(const mu16tty_event *event)
{
    static const char *const rejected[] = {
        "header_rejected", "header_expired", "header_decode_failed", "header_invalid", "mode_mismatch"
    };
    ULONGLONG now = GetTickCount64();
    double timestamp = (double)time(NULL);
    unsigned index;
    unsigned row;

    AcquireSRWLockExclusive(&decoder_lock);
    if (event == NULL) {
        memset(decoders, 0, sizeof(decoders));
        memset(previews, 0, sizeof(previews));
    } else if (event->event_type == MU16TTY_EVENT_WAKE) {
        for (index = 0; index < DECODER_RATES; ++index) {
            decoder_state *state = &decoders[2 * index];
            if (state->until <= now || strcmp(state->phase, "acquire") == 0)
                set_decoder(2 * index, "acquire", "", timestamp, now, 10000);
        }
    } else {
        for (index = 0; index < DECODER_RATES; ++index)
            if (decoder_rates[index] == event->symbol_rate)
                break;
        if (index < DECODER_RATES) {
            row = 2 * index + (event->metrics.decode_path == MU16TTY_DECODE_PATH_MID ? 1u : 0u);
            switch (event->event_type) {
            case MU16TTY_EVENT_HEAD_SYNC:
                previews[index].until = 0;
                set_decoder(row, "header", "", timestamp, now, 4000 + 100000 / event->symbol_rate);
                break;
            case MU16TTY_EVENT_MID_SYNC:
                if (decoders[row].until <= now || strcmp(decoders[row].phase, "receive") != 0)
                    set_decoder(row, "found", "mid", timestamp, now, 3000);
                break;
            case MU16TTY_EVENT_HEADER:
                previews[index].until = 0;
                set_decoder(row, "receive", "", timestamp, now, 5000 + 1600000 / event->symbol_rate);
                break;
            case MU16TTY_EVENT_PREVIEW:
                if (event->payload_size <= sizeof(previews[index].bytes)) {
                    previews[index].size = event->payload_size;
                    memcpy(previews[index].bytes, event->payload, event->payload_size);
                    previews[index].until = now + 2000 + 136000 / event->symbol_rate;
                }
                break;
            case MU16TTY_EVENT_PACKET:
            case MU16TTY_EVENT_DAMAGED_PACKET:
                previews[index].until = 0;
                if (event->event_type == MU16TTY_EVENT_PACKET)
                    set_decoder(row, "success", "received", timestamp, now, 3000);
                else
                    set_decoder(row, "rejected", "crc", timestamp, now, 3000);
                break;
            case MU16TTY_EVENT_HEAD_REJECTED: {
                unsigned reason = event->payload_size != 0 ? event->payload[0] : 0;
                previews[index].until = 0;
                set_decoder(2 * index, "rejected",
                            rejected[reason < ARRAYSIZE(rejected) ? reason : 0], timestamp, now, 3000);
                break;
            }
            }
        }
    }
    ReleaseSRWLockExclusive(&decoder_lock);
    if (event != NULL && event->event_type == MU16TTY_EVENT_DAMAGED_PACKET)
        record_damaged_packet(event);
}

void mu16tty_http_observe_audio(const float *samples, size_t count, uint32_t sample_rate,
                                unsigned queued, unsigned peak, unsigned capacity)
{
    static ULONGLONG next_update;
    ULONGLONG now = GetTickCount64();
    monitor_snapshot next = {0};
    if (now < next_update)
        return;
    next_update = now + 250;
    if (mu16tty_analyze_audio_monitor(samples, count, sample_rate, next.levels,
                                    next.waveform, WAVEFORM_POINTS,
                                    next.spectrum, SPECTRUM_BINS) != MU16TTY_OK)
        return;
    AcquireSRWLockExclusive(&monitor_lock);
    next.revision = monitor.revision + 1;
    next.queued = queued;
    next.queue_peak = peak;
    next.queue_capacity = capacity;
    monitor = next;
    ReleaseSRWLockExclusive(&monitor_lock);
}

static SRWLOCK history_lock = SRWLOCK_INIT;
static sqlite3 *history_db;
static ULONGLONG history_started_at;

int mu16tty_http_open_history(void)
{
    static wchar_t path[32768];
    static const char schema[] =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,identity_key TEXT NOT NULL UNIQUE,"
        "observed_at INTEGER NOT NULL,direction TEXT NOT NULL,sender TEXT NOT NULL,"
        "message TEXT NOT NULL,protocol TEXT NOT NULL,quality REAL,session_id TEXT,"
        "sequence INTEGER,metadata_json TEXT NOT NULL);";
    DWORD length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    wchar_t *name;
    size_t remaining;
    if (length == 0 || length >= ARRAYSIZE(path) || (name = wcsrchr(path, L'\\')) == NULL) {
        fputs("Could not resolve the history directory.\n", stderr);
        return 0;
    }
    ++name;
    remaining = ARRAYSIZE(path) - (size_t)(name - path);
    if (wcscpy_s(name, remaining, L"data") != 0 ||
        (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        wcscpy_s(name, remaining, L"data\\history.sqlite3") != 0) {
        fputs("Could not create the history directory.\n", stderr);
        return 0;
    }
    if (sqlite3_open16(path, &history_db) != SQLITE_OK ||
        sqlite3_busy_timeout(history_db, 1500) != SQLITE_OK ||
        sqlite3_exec(history_db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Could not open history database: %s\n",
                history_db != NULL ? sqlite3_errmsg(history_db) : "out of memory");
        mu16tty_http_close_history();
        return 0;
    }
    return 1;
}

void mu16tty_http_close_history(void)
{
    if (history_db != NULL) {
        sqlite3_close(history_db);
        history_db = NULL;
    }
}

static size_t render_packet_text(uint8_t *out, const uint8_t *bytes, size_t count, int ascii)
{
    static const uint8_t hex[] = "0123456789ABCDEF";
    size_t index = 0, size = 0;
    while (index < count) {
        uint8_t byte = bytes[index];
        size_t width = byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        int valid = ascii ? byte >= 0x20 && byte <= 0x7e :
            width <= count - index &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                (const char *)bytes + index, (int)width, NULL, 0);
        if (valid) {
            memcpy(out + size, bytes + index, width);
            size += width;
            index += width;
        } else {
            out[size++] = '<';
            out[size++] = hex[byte >> 4u];
            out[size++] = hex[byte & 15u];
            out[size++] = '>';
            ++index;
        }
    }
    return size;
}

static void store_packet(const uint8_t *packet, const mu16tty_chat_packet_fields *fields,
                         unsigned rate, int outbound, int delivered, size_t damaged_size)
{
    static const char sql[] =
        "INSERT INTO messages (identity_key,observed_at,direction,sender,message,protocol,"
        "session_id,sequence,metadata_json) VALUES (?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(identity_key) DO UPDATE SET observed_at=excluded.observed_at,"
        "direction=CASE WHEN messages.direction='outbound' THEN 'outbound' ELSE excluded.direction END,"
        "metadata_json=json_patch(json_patch(messages.metadata_json,excluded.metadata_json),"
        "CASE WHEN messages.direction<>excluded.direction OR "
        "json_extract(messages.metadata_json,'$.delivery')='loopback_received' "
        "THEN '{\"delivery\":\"loopback_received\",\"crc\":\"ok\"}' ELSE '{}' END)";
    sqlite3_stmt *statement = NULL;
    const char *sender = (const char *)packet + fields->callsign_offset;
    char protocol[24], session[17], sequence[12], identity[80 + MU16TTY_MAX_PACKET_BYTES * 2];
    char metadata[256 + MU16TTY_MAX_PACKET_BYTES * 2];
    char raw_hex[MU16TTY_MAX_PACKET_BYTES * 2 + 1];
    uint8_t displayed_sender[48], displayed_body[1120];
    const char *body = sender + fields->callsign_size;
    size_t sender_size = fields->callsign_size, body_size = fields->message_size;
    size_t identity_size;
    time_t now = time(NULL);
    int result;
    snprintf(protocol, sizeof(protocol), "mu16tty-%u%s", rate, damaged_size ? "-damaged" : "");
    snprintf(session, sizeof(session), "%016llx", (unsigned long long)fields->session_id);
    snprintf(sequence, sizeof(sequence), "%u", fields->sequence);
    identity_size = strlen(protocol) + 1;
    memcpy(identity, protocol, identity_size);
    memcpy(identity + identity_size, sender, fields->callsign_size);
    identity_size += fields->callsign_size;
    identity[identity_size++] = '\0';
    memcpy(identity + identity_size, session, sizeof(session));
    identity_size += sizeof(session);
    memcpy(identity + identity_size, sequence, strlen(sequence));
    identity_size += strlen(sequence);
    if (damaged_size) {
        static const char hex[] = "0123456789ABCDEF";
        size_t index;
        for (index = 0; index < damaged_size; ++index) {
            raw_hex[2 * index] = hex[packet[index] >> 4u];
            raw_hex[2 * index + 1] = hex[packet[index] & 15u];
        }
        raw_hex[2 * damaged_size] = '\0';
        identity[identity_size++] = '\0';
        memcpy(identity + identity_size, raw_hex, 2 * damaged_size);
        identity_size += 2 * damaged_size;
        sender_size = render_packet_text(displayed_sender, (const uint8_t *)sender, sender_size, 1);
        body_size = render_packet_text(displayed_body, (const uint8_t *)body, body_size, 0);
        sender = (const char *)displayed_sender;
        body = (const char *)displayed_body;
        snprintf(metadata, sizeof(metadata),
            "{\"symbol_rate\":%u,\"crc\":\"fail\",\"verification\":\"damaged\",\"raw_packet_hex\":\"%s\"}",
            rate, raw_hex);
    } else {
        snprintf(metadata, sizeof(metadata),
             "{\"symbol_rate\":%u,\"packet_timestamp\":%u,\"robust\":%s,\"crc\":%s,\"delivery\":\"%s\"}",
             rate, fields->timestamp, rate < 40 ? "true" : "false",
             delivered ? "\"ok\"" : "null", outbound ? (delivered ? "sent" : "failed") : "received");
    }
    AcquireSRWLockExclusive(&history_lock);
    result = sqlite3_prepare_v2(history_db, sql, -1, &statement, NULL);
    if (result == SQLITE_OK) {
        if (sqlite3_bind_text(statement, 1, identity, (int)identity_size, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_int64(statement, 2, now < 0 ? 0 : (sqlite3_int64)now) == SQLITE_OK &&
            sqlite3_bind_text(statement, 3, outbound ? "outbound" : "inbound", -1, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_text(statement, 4, sender, (int)sender_size, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_text(statement, 5, body, (int)body_size, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_text(statement, 6, protocol, -1, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_text(statement, 7, session, -1, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_int64(statement, 8, fields->sequence) == SQLITE_OK &&
            sqlite3_bind_text(statement, 9, metadata, -1, SQLITE_STATIC) == SQLITE_OK)
            result = sqlite3_step(statement);
        else
            result = sqlite3_errcode(history_db);
    }
    if (result != SQLITE_DONE)
        fprintf(stderr, "Could not save message history: %s\n", sqlite3_errmsg(history_db));
    sqlite3_finalize(statement);
    ReleaseSRWLockExclusive(&history_lock);
}

void mu16tty_http_record_packet(const mu16tty_event *event, const mu16tty_chat_packet_fields *fields)
{
    store_packet(event->payload, fields, event->symbol_rate, 0, 1, 0);
}

static void record_damaged_packet(const mu16tty_event *event)
{
    mu16tty_chat_packet_fields fields;
    if (InterlockedCompareExchange(&log_all, 0, 0) != 0 &&
        event->payload_size <= sizeof(event->payload) &&
        mu16tty_inspect_chat_packet(event->payload, event->payload_size, &fields) == MU16TTY_OK)
        store_packet(event->payload, &fields, event->symbol_rate, 0, 0, event->payload_size);
}

void mu16tty_http_record_transmission(const uint8_t *packet, size_t length,
                                     unsigned rate, int delivered)
{
    mu16tty_chat_packet_fields fields;
    if (mu16tty_unpack_chat_packet(packet, length, &fields) == MU16TTY_OK)
        store_packet(packet, &fields, rate, 1, delivered, 0);
}

static void send_all(SOCKET client, const char *data, int size)
{
    while (size > 0) {
        int sent = send(client, data, size, 0);
        if (sent <= 0)
            return;
        data += sent;
        size -= sent;
    }
}

static void reply(SOCKET client, const char *status, const char *type, const char *body,
                  const char *additional_headers)
{
    char header[512];
    int size = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
        status, type, strlen(body), additional_headers);
    if (size > 0 && (size_t)size < sizeof(header)) {
        send_all(client, header, size);
        send_all(client, body, (int)strlen(body));
    }
}

static char *append_numbers(char *out, char *end, const double *values, size_t count)
{
    size_t index;
    *out++ = '[';
    for (index = 0; index < count; ++index)
        out += sprintf_s(out, (size_t)(end - out), "%s%.5f", index ? "," : "", values[index]);
    *out++ = ']';
    return out;
}

static char *append_json_string(char *out, const uint8_t *text, size_t length);

static char *append_preview(char *out, const decoder_preview *preview)
{
    static const uint8_t hex[] = "0123456789ABCDEF";
    uint8_t text[MU16TTY_MAX_PACKET_BYTES * 4];
    size_t index = 0, size = 0;
    while (index < preview->size) {
        uint8_t byte = preview->bytes[index];
        size_t sequence = byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        if (sequence <= preview->size - index &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                (const char *)preview->bytes + index, (int)sequence, NULL, 0)) {
            memcpy(text + size, preview->bytes + index, sequence);
            size += sequence;
            index += sequence;
        } else {
            text[size++] = '<';
            text[size++] = hex[byte >> 4u];
            text[size++] = hex[byte & 15u];
            text[size++] = '>';
            ++index;
        }
    }
    return append_json_string(out, text, size);
}

static char *append_decoder_json(char *out, char *end)
{
    ULONGLONG now = GetTickCount64();
    unsigned row, index;
    int comma = 0;
    AcquireSRWLockShared(&decoder_lock);
    out += sprintf_s(out, (size_t)(end - out), ",\"decoders\":[");
    for (row = 0; row < DECODER_ROWS; ++row) {
        const decoder_state *state = &decoders[row];
        int active = state->until > now;
        out += sprintf_s(out, (size_t)(end - out),
            "%s{\"symbol_rate\":%u,\"lane\":\"%c\",\"name\":\"%s%c\","
            "\"phase\":\"%s\",\"detail\":\"%s\",\"started_at\":%.0f}",
            row ? "," : "", decoder_rates[row / 2], row % 2 ? 'M' : 'R',
            decoder_names[row / 2], row % 2 ? 'M' : 'R',
            active ? state->phase : "idle", active ? state->detail : "",
            active ? state->started_at : 0);
    }
    out += sprintf_s(out, (size_t)(end - out), "],\"decoder_log\":[");
    for (index = 0; index < decoder_log_size; ++index) {
        const decoder_transition *entry =
            &decoder_log[(decoder_log_next + DECODER_LOG_CAPACITY - decoder_log_size + index)
                         % DECODER_LOG_CAPACITY];
        char lane = entry->row % 2 ? 'M' : 'R';
        out += sprintf_s(out, (size_t)(end - out),
            "%s{\"at\":%.0f,\"name\":\"%s%c\",\"phase\":\"%s\",\"detail\":\"%s\"}",
            index ? "," : "", entry->at, decoder_names[entry->row / 2], lane,
            entry->phase, entry->detail);
    }
    out += sprintf_s(out, (size_t)(end - out), "],\"receive_previews\":[");
    for (index = 0; index < DECODER_RATES; ++index) {
        if (previews[index].until <= now)
            continue;
        out += sprintf_s(out, (size_t)(end - out), "%s{\"symbol_rate\":%u,\"text\":",
                         comma ? "," : "", decoder_rates[index]);
        out = append_preview(out, &previews[index]);
        *out++ = '}';
        comma = 1;
    }
    ReleaseSRWLockShared(&decoder_lock);
    *out++ = ']';
    return out;
}

static void reply_monitor(SOCKET client)
{
    static const char json[] = "application/json; charset=utf-8";
    monitor_snapshot snapshot;
    char body[32768];
    char *out;
    char *end = body + sizeof(body);
    AcquireSRWLockShared(&monitor_lock);
    snapshot = monitor;
    ReleaseSRWLockShared(&monitor_lock);
    if (snapshot.revision == 0) {
        out = body + sprintf_s(body, sizeof(body), "{\"available\":false,\"revision\":0");
    } else {
        out = body + sprintf_s(body, sizeof(body),
        "{\"available\":true,\"revision\":%llu,\"rms_dbfs\":%.5f,\"peak_dbfs\":%.5f,"
        "\"clipped\":%s,\"spectrum_min_hz\":0,\"spectrum_max_hz\":3000,\"waveform\":",
        (unsigned long long)snapshot.revision, snapshot.levels[0], snapshot.levels[1],
        snapshot.levels[1] >= -0.00869024 ? "true" : "false"); /* 20 log10(0.999) */
        out = append_numbers(out, end, snapshot.waveform, WAVEFORM_POINTS);
        out += sprintf_s(out, (size_t)(end - out), ",\"spectrum_dbfs\":");
        out = append_numbers(out, end, snapshot.spectrum, SPECTRUM_BINS);
    }
    unsigned reading = (unsigned)InterlockedCompareExchange(&queue_reading, 0, 0);
    if (reading != 0)
        out += sprintf_s(out, (size_t)(end - out),
            ",\"input_queue\":{\"used\":%u,\"peak\":%u,\"capacity\":%u}",
            reading & 255u, (reading >> 8) & 255u, reading >> 16);
    out = append_decoder_json(out, end);
    *out++ = '}';
    *out = '\0';
    reply(client, "200 OK", json, body, "");
}

static void reply_web_asset(SOCKET client, const wchar_t *filename, const char *type)
{
    static const char json[] = "application/json; charset=utf-8";
    static wchar_t path[32768];
    wchar_t *name;
    DWORD path_length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    FILE *file = NULL;
    long size;
    char *body;
    if (path_length == 0 || path_length >= ARRAYSIZE(path) ||
        (name = wcsrchr(path, L'\\')) == NULL ||
        swprintf_s(name + 1, ARRAYSIZE(path) - (size_t)(name + 1 - path),
                   L"web\\%ls", filename) < 0 ||
        _wfopen_s(&file, path, L"rb") != 0) {
        reply(client, "404 Not Found", json, "{\"error\":\"web_asset_missing\"}", "");
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        size > 256 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        reply(client, "500 Internal Server Error", json, "{\"error\":\"web_asset_unavailable\"}", "");
        return;
    }
    body = malloc((size_t)size + 1);
    if (body == NULL || fread(body, 1, (size_t)size, file) != (size_t)size) {
        free(body);
        fclose(file);
        reply(client, "500 Internal Server Error", json, "{\"error\":\"web_asset_unavailable\"}", "");
        return;
    }
    fclose(file);
    body[size] = '\0';
    reply(client, "200 OK", type, body, "X-Content-Type-Options: nosniff\r\n");
    free(body);
}

static int serve_web_asset(SOCKET client, const char *path)
{
    static const struct {
        const char *route;
        const wchar_t *file;
        const char *type;
    } assets[] = {
        {"/", L"index.html", "text/html; charset=utf-8"},
        {"/assets/styles.css", L"styles.css", "text/css; charset=utf-8"},
        {"/assets/app.js", L"app.js", "text/javascript; charset=utf-8"},
        {"/assets/state.js", L"state.js", "text/javascript; charset=utf-8"},
        {"/assets/monitor.js", L"monitor.js", "text/javascript; charset=utf-8"},
        {"/assets/settings-dialog.js", L"settings-dialog.js", "text/javascript; charset=utf-8"},
    };
    size_t index;
    for (index = 0; index < ARRAYSIZE(assets); ++index) {
        if (strcmp(path, assets[index].route) == 0) {
            reply_web_asset(client, assets[index].file, assets[index].type);
            return 1;
        }
    }
    return 0;
}

static char *append_json_string(char *out, const uint8_t *text, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t index;
    *out++ = '"';
    for (index = 0; index < length; ++index) {
        uint8_t byte = text[index];
        if (byte == '"' || byte == '\\') {
            *out++ = '\\';
            *out++ = (char)byte;
        } else if (byte < 0x20) {
            memcpy(out, "\\u00", 4);
            out += 4;
            *out++ = hex[byte >> 4u];
            *out++ = hex[byte & 15u];
        } else {
            *out++ = (char)byte;
        }
    }
    *out++ = '"';
    return out;
}

static int history_query(char *query, uint64_t *after, unsigned *limit, int *latest)
{
    while (query != NULL && *query != '\0') {
        char *next = strchr(query, '&');
        const char *value = NULL;
        uint64_t number = 0;
        int is_limit = strncmp(query, "limit=", 6) == 0;
        if (next != NULL)
            *next++ = '\0';
        if (strcmp(query, "latest=1") == 0)
            *latest = 1;
        else if (is_limit)
            value = query + 6;
        else if (strncmp(query, "after_id=", 9) == 0)
            value = query + 9;
        if (value != NULL) {
            if (*value == '\0')
                return 0;
            for (; *value != '\0'; ++value) {
                unsigned digit = (unsigned)(*value - '0');
                if (digit > 9 || number > (UINT64_MAX - digit) / 10)
                    return 0;
                number = number * 10 + digit;
            }
            if (is_limit) {
                if (number == 0 || number > 500)
                    return 0;
                *limit = (unsigned)number;
            } else {
                *after = number;
            }
        }
        query = next;
    }
    return 1;
}

static char *append_history_column(char *out, sqlite3_stmt *row, int column)
{
    const uint8_t *value = sqlite3_column_text(row, column);
    if (value != NULL)
        return append_json_string(out, value, (size_t)sqlite3_column_bytes(row, column));
    memcpy(out, "null", 4);
    return out + 4;
}

static void reply_history(SOCKET client, char *query)
{
    static const char json[] = "application/json; charset=utf-8";
    static const char sql[] =
        "SELECT id,observed_at,direction,sender,message,protocol,quality,session_id,sequence,"
        "CASE WHEN json_valid(metadata_json) THEN "
        "CASE WHEN json_type(metadata_json)='object' THEN json(metadata_json) ELSE '{}' END "
        "ELSE '{}' END FROM messages WHERE id>? ORDER BY id LIMIT ?";
    sqlite3_stmt *row = NULL;
    uint64_t after = 0;
    unsigned limit = 200;
    unsigned count = 0;
    int latest = 0;
    int revision;
    char *body, *out;
    int result;
    if (!history_query(query, &after, &limit, &latest)) {
        reply(client, "422 Unprocessable Content", json, "{\"error\":\"invalid_history_query\"}", "");
        return;
    }
    body = malloc(HISTORY_JSON_CAPACITY);
    if (body == NULL) {
        reply(client, "503 Service Unavailable", json, "{\"error\":\"out_of_memory\"}", "");
        return;
    }
    memcpy(body, "{\"messages\":[", 13);
    out = body + 13;
    AcquireSRWLockExclusive(&history_lock);
    if (latest && after == 0) {
        if (sqlite3_prepare_v2(history_db, "SELECT id FROM messages ORDER BY id DESC LIMIT 1 OFFSET ?",
                               -1, &row, NULL) == SQLITE_OK &&
            sqlite3_bind_int(row, 1, (int)limit) == SQLITE_OK &&
            sqlite3_step(row) == SQLITE_ROW)
            after = (uint64_t)sqlite3_column_int64(row, 0);
        sqlite3_finalize(row);
        row = NULL;
    }
    result = sqlite3_prepare_v2(history_db, sql, -1, &row, NULL);
    if (result == SQLITE_OK)
        result = sqlite3_bind_int64(row, 1, after > INT64_MAX ? INT64_MAX : (sqlite3_int64)after);
    if (result == SQLITE_OK)
        result = sqlite3_bind_int(row, 2, (int)limit);
    if (result == SQLITE_OK) {
        while ((result = sqlite3_step(row)) == SQLITE_ROW) {
            static const int text_columns[] = {2, 3, 4, 5, 7};
            size_t required = 512;
            unsigned column;
            const unsigned char *metadata = sqlite3_column_text(row, 9);
            size_t metadata_length = (size_t)sqlite3_column_bytes(row, 9);
            double quality = sqlite3_column_double(row, 6);
            sqlite3_int64 id = sqlite3_column_int64(row, 0);
            for (column = 0; column < ARRAYSIZE(text_columns); ++column)
                required += 6 * (size_t)sqlite3_column_bytes(row, text_columns[column]);
            required += metadata_length;
            if (required > (size_t)(body + HISTORY_JSON_CAPACITY - out)) {
                result = count == 0 ? SQLITE_TOOBIG : SQLITE_DONE;
                break;
            }
            if (count++ != 0)
                *out++ = ',';
            out += sprintf_s(out, 192, "{\"id\":%lld,\"observed_at\":%lld,\"direction\":",
                             (long long)id, (long long)sqlite3_column_int64(row, 1));
            out = append_history_column(out, row, 2);
            out += sprintf_s(out, 32, ",\"sender\":");
            out = append_history_column(out, row, 3);
            out += sprintf_s(out, 32, ",\"message\":");
            out = append_history_column(out, row, 4);
            out += sprintf_s(out, 32, ",\"protocol\":");
            out = append_history_column(out, row, 5);
            out += sprintf_s(out, 64, ",\"quality\":");
            if (sqlite3_column_type(row, 6) != SQLITE_NULL && isfinite(quality))
                out += sprintf_s(out, 64, "%.17g", quality);
            else
                out += sprintf_s(out, 16, "null");
            out += sprintf_s(out, 32, ",\"session_id\":");
            out = append_history_column(out, row, 7);
            out += sprintf_s(out, 32, ",\"sequence\":");
            if (sqlite3_column_type(row, 8) == SQLITE_NULL)
                out += sprintf_s(out, 16, "null");
            else
                out += sprintf_s(out, 32, "%lld", (long long)sqlite3_column_int64(row, 8));
            out += sprintf_s(out, 32, ",\"metadata\":");
            if (metadata != NULL) {
                memcpy(out, metadata, metadata_length);
                out += metadata_length;
            } else {
                memcpy(out, "{}", 2);
                out += 2;
            }
            *out++ = '}';
            after = (uint64_t)id;
        }
    }
    sqlite3_finalize(row);
    revision = sqlite3_total_changes(history_db);
    ReleaseSRWLockExclusive(&history_lock);
    if (result != SQLITE_DONE) {
        free(body);
        reply(client, "500 Internal Server Error", json, "{\"error\":\"history_unavailable\"}", "");
        return;
    }
    sprintf_s(out, 128, "],\"last_id\":%llu,\"history_generation\":\"%lu-%llu\",\"history_revision\":%d}",
              (unsigned long long)after, GetCurrentProcessId(),
              (unsigned long long)history_started_at, revision);
    reply(client, "200 OK", json, body, "");
    free(body);
}

static int readable(SOCKET socket, long milliseconds)
{
    fd_set set;
    struct timeval timeout;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return select(0, &set, NULL, NULL, &timeout);
}

static void skip_json_space(const wchar_t **cursor)
{
    while (**cursor != L'\0' && wcschr(L" \r\n\t", **cursor) != NULL)
        ++*cursor;
}

static int read_json_string(const wchar_t **cursor, wchar_t *out, size_t capacity)
{
    const wchar_t *p = *cursor;
    size_t length = 0;
    if (*p++ != L'"')
        return 0;
    while (*p != L'"') {
        wchar_t value = *p++;
        if (value < 0x20 || length + 1 >= capacity)
            return 0;
        if (value == L'\\') {
            value = *p++;
            switch (value) {
            case L'"': case L'\\': case L'/': break;
            case L'b': value = L'\b'; break;
            case L'f': value = L'\f'; break;
            case L'n': value = L'\n'; break;
            case L'r': value = L'\r'; break;
            case L't': value = L'\t'; break;
            case L'u': {
                unsigned digits = 0;
                unsigned index;
                for (index = 0; index < 4; ++index) {
                    wchar_t digit = *p++;
                    if (digit >= L'0' && digit <= L'9')
                        digits = digits * 16 + (unsigned)(digit - L'0');
                    else if (digit >= L'a' && digit <= L'f')
                        digits = digits * 16 + (unsigned)(digit - L'a' + 10);
                    else if (digit >= L'A' && digit <= L'F')
                        digits = digits * 16 + (unsigned)(digit - L'A' + 10);
                    else
                        return 0;
                }
                if (digits == 0)
                    return 0;
                value = (wchar_t)digits;
                break;
            }
            default: return 0;
            }
        }
        out[length++] = value;
    }
    out[length] = L'\0';
    *cursor = p + 1;
    return 1;
}

static int parse_transmit_request(const char *body, size_t length, wchar_t message[281], unsigned *rate)
{
    wchar_t json[4097];
    const wchar_t *cursor = json;
    unsigned fields = 0;
    int robust = 0;
    int count;
    if (length == 0 || memchr(body, '\0', length) != NULL)
        return 0;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, (int)length,
                                json, ARRAYSIZE(json) - 1);
    if (count <= 0)
        return 0;
    json[count] = L'\0';
    skip_json_space(&cursor);
    if (*cursor++ != L'{')
        return 0;
    for (;;) {
        wchar_t key[16];
        skip_json_space(&cursor);
        if (!read_json_string(&cursor, key, ARRAYSIZE(key)))
            return 0;
        skip_json_space(&cursor);
        if (*cursor++ != L':')
            return 0;
        skip_json_space(&cursor);
        if (wcscmp(key, L"message") == 0 && (fields & 1u) == 0) {
            if (!read_json_string(&cursor, message, 281))
                return 0;
            fields |= 1u;
        } else if (wcscmp(key, L"symbol_rate") == 0 && (fields & 2u) == 0) {
            if (wcsncmp(cursor, L"40", 2) == 0 || wcsncmp(cursor, L"20", 2) == 0 ||
                wcsncmp(cursor, L"10", 2) == 0) {
                *rate = (unsigned)(cursor[0] - L'0') * 10;
                cursor += 2;
            } else if (*cursor == L'4') {
                *rate = 4;
                ++cursor;
            } else {
                return 0;
            }
            fields |= 2u;
        } else if (wcscmp(key, L"robust") == 0 && (fields & 4u) == 0) {
            if (wcsncmp(cursor, L"true", 4) == 0) {
                robust = 1;
                cursor += 4;
            } else if (wcsncmp(cursor, L"false", 5) == 0) {
                cursor += 5;
            } else {
                return 0;
            }
            fields |= 4u;
        } else {
            return 0;
        }
        skip_json_space(&cursor);
        if (*cursor != L',')
            break;
        ++cursor;
    }
    if (*cursor++ != L'}')
        return 0;
    skip_json_space(&cursor);
    if ((fields & 2u) == 0)
        *rate = robust ? 20 : 40;
    count = (fields & 1u) != 0
        ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, message, -1, NULL, 0, NULL, NULL) : 0;
    return *cursor == L'\0' && count > 1 && count <= 281;
}

static int parse_post_headers(char *headers, unsigned port, size_t *body_size)
{
    char host_ip[32], host_local[32];
    char *host = NULL, *origin = NULL;
    int content_type = 0, content_length = 0;
    snprintf(host_ip, sizeof(host_ip), "127.0.0.1:%u", port);
    snprintf(host_local, sizeof(host_local), "localhost:%u", port);
    while (*headers != '\0') {
        char *line_end = strstr(headers, "\r\n");
        char *value;
        if (line_end != NULL)
            *line_end = '\0';
        value = strchr(headers, ':');
        if (value == NULL)
            return 0;
        *value++ = '\0';
        while (*value == ' ' || *value == '\t')
            ++value;
        if (_stricmp(headers, "Host") == 0 && host == NULL)
            host = value;
        else if (_stricmp(headers, "Origin") == 0 && origin == NULL)
            origin = value;
        else if (_stricmp(headers, "Content-Type") == 0 && !content_type) {
            if (_strnicmp(value, "application/json", 16) != 0 ||
                (value[16] != '\0' && value[16] != ';'))
                return 0;
            content_type = 1;
        } else if (_stricmp(headers, "Content-Length") == 0 && !content_length) {
            size_t size = 0;
            for (; *value != '\0'; ++value) {
                unsigned digit = (unsigned)(*value - '0');
                if (digit > 9 || size > 409)
                    return 0;
                size = size * 10 + digit;
            }
            if (size == 0 || size > 4096)
                return 0;
            *body_size = size;
            content_length = 1;
        } else if (_stricmp(headers, "Transfer-Encoding") == 0 ||
                   _stricmp(headers, "Host") == 0 || _stricmp(headers, "Origin") == 0 ||
                   _stricmp(headers, "Content-Type") == 0 || _stricmp(headers, "Content-Length") == 0)
            return 0;
        if (line_end == NULL)
            break;
        headers = line_end + 2;
    }
    if (!content_type || !content_length || host == NULL ||
        (_stricmp(host, host_ip) != 0 && _stricmp(host, host_local) != 0 &&
         !(http_lan && *http_id && *http_pass && strspn(host, "0123456789.:") == strlen(host))))
        return 0;
    return origin == NULL || (_strnicmp(origin, "http://", 7) == 0 && _stricmp(origin + 7, host) == 0);
}

static void reply_transmit(SOCKET client, const mu16tty_http_transmitter *tx,
                           const char *body, size_t size)
{
    static const char json[] = "application/json; charset=utf-8";
    wchar_t message[281];
    unsigned rate, depth;
    uint32_t sequence;
    char response[160];
    if (tx == NULL) {
        reply(client, "503 Service Unavailable", json, "{\"detail\":{\"message\":\"Transmission disabled\"}}", "");
    } else if (strcmp(tx->callsign, "NOCALL") == 0) {
        reply(client, "422 Unprocessable Content", json,
              "{\"detail\":{\"message\":\"Set your callsign with my [callsign] before transmitting.\"}}", "");
    } else if (!parse_transmit_request(body, size, message, &rate)) {
        reply(client, "422 Unprocessable Content", json, "{\"detail\":{\"message\":\"Invalid message or symbol rate\"}}", "");
    } else if (!tx->enqueue(tx->context, message, rate, &sequence, &depth)) {
        reply(client, "503 Service Unavailable", json, "{\"detail\":{\"message\":\"Transmit queue unavailable\"}}", "");
    } else {
        snprintf(response, sizeof(response),
            "{\"session_id\":\"%016llx\",\"sequence\":%u,\"queue_depth_after_accept\":%u}",
            (unsigned long long)tx->session_id, sequence, depth);
        reply(client, "202 Accepted", json, response, "");
    }
}

static void reply_settings_help(SOCKET client)
{
    static const char help[] =
        "show     : Show all settings\n"
        "my       : My callsign setting\n"
        "frame    : Frame sequence setting\n"
        "sound    : Audio interface setting\n"
        "ptt      : PTT control setting\n"
        "loglevel : Receive damaged packets setting\nhttp     : HTTP server settings";
    char body[1024] = "{\"help\":";
    char *out;
    out = append_json_string(body + strlen(body), (const uint8_t *)help, strlen(help));
    *out++ = '}';
    *out = '\0';
    reply(client, "200 OK", "application/json; charset=utf-8", body, "");
}

static int station_settings(const wchar_t *command, void *audio, mu16tty_http_input *input,
                            const mu16tty_http_transmitter *tx, char *message, size_t capacity,
                            int *changed)
{
    const wchar_t *args;
    if (_wcsnicmp(command, L"http", 4) == 0 && wcschr(L" \t", command[4]) != NULL)
        return http_command(command + 4 + wcsspn(command + 4, L" \t"), message, capacity, changed);
    if (_wcsicmp(command, L"show") == 0) {
        char sound[8192], ptt[256];
        mu16tty_timing timing = mu16tty_settings_timing();
        int ignored = 0;
        hardware_settings(L"sound", audio, input, tx ? tx->output_device : NULL,
                          sound, sizeof(sound), &ignored);
        char *help = strstr(sound, "\n\n");
        if (help != NULL)
            *help = '\0';
        if (tx)
            tx->ptt_command(tx->context, L"", ptt, sizeof(ptt), &ignored);
        else
            strcpy_s(ptt, sizeof(ptt), "PTT disabled");
        snprintf(message, capacity, "My callsign: %s\nFrame: Delay %u ms / Wake %u ms / Tail %u ms\n"
                 "Audio:\n%s\n%s\nLog level: %s", tx ? tx->callsign : "-",
                 timing.delay, timing.wake, timing.tail, sound, ptt,
                 InterlockedCompareExchange(&log_all, 0, 0) ? "all" : "valid");
        return 1;
    }
    if (_wcsnicmp(command, L"my", 2) == 0 && wcschr(L" \t", command[2]) != NULL) {
        args = command + 2;
        args += wcsspn(args, L" \t");
        if (tx)
            return tx->my_command(tx->context, args, message, capacity, changed);
        snprintf(message, capacity, "Transmission disabled.");
        return 0;
    }
    if (_wcsnicmp(command, L"frame", 5) == 0 && wcschr(L" \t", command[5]) != NULL) {
        args = command + 5;
        args += wcsspn(args, L" \t");
        if (*args == L'\0') {
            mu16tty_timing timing = mu16tty_settings_timing();
            snprintf(message, capacity, "Delay %u ms\nWake %u ms\nTail %u ms\n\n"
                     "frame delay|wake|tail [0..5000] : Show or set milliseconds",
                     timing.delay, timing.wake, timing.tail);
            return 1;
        }
        if (_wcsicmp(args, L"show") != 0)
            return mu16tty_settings_command(args, message, capacity, changed);
        snprintf(message, capacity, "Use frame, or frame delay|wake|tail [0..5000].");
        return 0;
    }
    if (_wcsicmp(command, L"ptt") == 0 && tx) {
        int result = tx->ptt_command(tx->context, L"", message, capacity, changed);
        size_t length = strlen(message);
        snprintf(message + length, capacity - length,
                 "\n\nptt list          : List COM ports\nptt open|close    : Open or close port\n"
                 "ptt COMx [rts|dtr]: Select port and line\nptt rts|dtr       : Select line\nptt none          : Disable PTT");
        return result;
    }
    return -2;
}

static void reply_settings_command(SOCKET client, const char *body, size_t size,
                                   void *audio, mu16tty_http_input *input,
                                   const mu16tty_http_transmitter *tx)
{
    wchar_t json[4097], key[16], command[257];
    const wchar_t *cursor = json;
    char message[8192], response[sizeof(message) * 6 + 128];
    char *out;
    int changed = 0;
    int result = 0;
    int count = memchr(body, '\0', size) == NULL ?
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, (int)size, json, ARRAYSIZE(json) - 1) : 0;
    if (count > 0) {
        json[count] = L'\0';
        skip_json_space(&cursor);
        if (*cursor++ == L'{') {
            skip_json_space(&cursor);
            if (read_json_string(&cursor, key, ARRAYSIZE(key)) && wcscmp(key, L"command") == 0) {
                skip_json_space(&cursor);
                if (*cursor++ == L':') {
                    skip_json_space(&cursor);
                    if (read_json_string(&cursor, command, ARRAYSIZE(command))) {
                        skip_json_space(&cursor);
                        if (*cursor++ == L'}') {
                            skip_json_space(&cursor);
                            if (*cursor == L'\0') {
                                const wchar_t *whitespace = L" \t\r\n\v\f\u3000";
                                wchar_t *trimmed = command + wcsspn(command, whitespace);
                                size_t length = wcslen(trimmed);
                                while (length > 0 && wcschr(whitespace, trimmed[length - 1]) != NULL)
                                    trimmed[--length] = L'\0';
                                result = station_settings(trimmed, audio, input, tx, message, sizeof(message), &changed);
                                if (result == -2)
                                    result = hardware_settings(trimmed, audio, input,
                                    tx != NULL ? tx->output_device : NULL, message, sizeof(message), &changed);
                                if (result == -2 && _wcsnicmp(trimmed, L"ptt", 3) == 0 &&
                                    (trimmed[3] == L'\0' || trimmed[3] == L' ' || trimmed[3] == L'\t')) {
                                    const wchar_t *args = trimmed + 3;
                                    args += wcsspn(args, L" \t");
                                    if (tx != NULL)
                                        result = tx->ptt_command(tx->context, args, message, sizeof(message), &changed);
                                    else {
                                        snprintf(message, sizeof(message), "Transmission disabled.");
                                        result = 0;
                                    }
                                }
                                if (result == -2)
                                    snprintf(message, sizeof(message), "Unknown command. Use show, my, frame, sound, ptt, loglevel or http.");
                            }
                            else
                                snprintf(message, sizeof(message), "Invalid settings command.");
                            goto respond;
                        }
                    }
                }
            }
        }
    }
    snprintf(message, sizeof(message), "Invalid settings command.");

respond:
    out = response + sprintf_s(response, sizeof(response), "{\"success\":%s,\"changed\":%s,\"message\":",
                               result > 0 ? "true" : "false", changed ? "true" : "false");
    out = append_json_string(out, (const uint8_t *)message, strlen(message));
    *out++ = '}';
    *out = '\0';
    reply(client, result > 0 ? "200 OK" : result < 0 ? "503 Service Unavailable" : "422 Unprocessable Content",
          "application/json; charset=utf-8", response, "");
}

static void serve_client(SOCKET client, mu16tty_http_input *input, unsigned port,
                         const mu16tty_http_transmitter *tx, void *audio, int (*stop_requested)(void))
{
    static const char json[] = "application/json; charset=utf-8";
    char request[4097];
    size_t length = 0;
    ULONGLONG deadline = GetTickCount64() + 1500;
    DWORD send_timeout = 1000;
    char *end, *path, *version, *query, *header_end;

    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout));
    while (length < sizeof(request) - 1 && !stop_requested()) {
        int result = readable(client, 100);
        int received;
        if (result < 0 || GetTickCount64() >= deadline)
            return;
        if (result == 0)
            continue;
        received = recv(client, request + length, (int)(sizeof(request) - 1 - length), 0);
        if (received <= 0 || memchr(request + length, '\0', (size_t)received) != NULL)
            return;
        length += (size_t)received;
        request[length] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL)
            break;
    }
    if (stop_requested())
        return;
    if (length == 0 || strstr(request, "\r\n\r\n") == NULL) {
        reply(client, "431 Request Header Fields Too Large", json, "{\"error\":\"headers_too_large\"}", "");
        return;
    }
    header_end = strstr(request, "\r\n\r\n");
    end = strstr(request, "\r\n");
    if (!http_authorized(end == header_end ? end : end + 2, header_end)) {
        reply(client, "401 Unauthorized", json, "{}", "WWW-Authenticate: Basic realm=\"Mu16TTY\", charset=\"UTF-8\"\r\n");
        return;
    }
    *end = '\0';
    path = strchr(request, ' ');
    if (path != NULL)
        *path++ = '\0';
    version = path != NULL ? strchr(path, ' ') : NULL;
    if (version != NULL)
        *version++ = '\0';
    query = path != NULL ? strchr(path, '?') : NULL;
    if (query != NULL)
        *query++ = '\0';
    if (version == NULL || (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)) {
        reply(client, "400 Bad Request", json, "{\"error\":\"bad_request\"}", "");
    } else if (strcmp(request, "POST") == 0 && query == NULL &&
               (strcmp(path, "/api/v1/messages") == 0 || strcmp(path, "/api/v1/settings/command") == 0)) {
        char body[4096];
        size_t expected = 0;
        size_t available = length - (size_t)(header_end + 4 - request);
        *header_end = '\0';
        if (end == header_end || !parse_post_headers(end + 2, port, &expected) || available > expected) {
            reply(client, "400 Bad Request", json, "{\"detail\":{\"message\":\"Invalid local JSON request\"}}", "");
            return;
        }
        memcpy(body, header_end + 4, available);
        while (available < expected && !stop_requested()) {
            int pending = readable(client, 100);
            int received;
            if (pending < 0 || GetTickCount64() >= deadline)
                return;
            if (pending == 0)
                continue;
            received = recv(client, body + available, (int)(expected - available), 0);
            if (received <= 0)
                return;
            available += (size_t)received;
        }
        if (!stop_requested()) {
            if (strcmp(path, "/api/v1/messages") == 0)
                reply_transmit(client, tx, body, available);
            else
                reply_settings_command(client, body, available, audio, input, tx);
        }
    } else if (strcmp(request, "GET") != 0) {
        reply(client, "405 Method Not Allowed", json, "{\"error\":\"method_not_allowed\"}", "Allow: GET\r\n");
    } else if (strcmp(path, "/api/v1/status") == 0) {
        char body[768];
        mu16tty_timing timing = mu16tty_settings_timing();
        char *out = body + sprintf_s(body, sizeof(body),
            "{\"state\":\"running\",\"modem_backend\":\"native\",\"history_polling\":true,"
            "\"settings_commands\":true,\"tx_delay_ms\":%u,\"tx_wake_ms\":%u,\"tx_tail_ms\":%u,"
            "\"rx\":{\"state\":\"%s\"},\"tx\":{\"state\":\"%s\"},\"input_device\":%d",
            timing.delay, timing.wake, timing.tail,
            input == NULL ? "disabled" : input->running(input->context) ? "running" : "failed",
            tx != NULL ? "running" : "disabled",
            input != NULL ? input->device : -1);
        if (input != NULL)
            out += sprintf_s(out, (size_t)(body + sizeof(body) - out),
                             ",\"input_rate\":%u", input->sample_rate(input->context));
        if (tx != NULL) {
            char port_name[8];
            int dtr, open;
            out += sprintf_s(out, (size_t)(body + sizeof(body) - out),
                ",\"output_device\":%d,\"session_id\":\"%016llx\",\"callsign\":",
                (int)InterlockedCompareExchange(tx->output_device, 0, 0),
                (unsigned long long)tx->session_id);
            out = append_json_string(out, (const uint8_t *)tx->callsign, strlen(tx->callsign));
            memset(&device_listing, 0, sizeof(device_listing));
            device_listing.direction = 2;
            device_listing.candidate = (int)InterlockedCompareExchange(tx->output_device, 0, 0);
            mu16tty_windows_audio_devices(audio, list_audio_device);
            if (device_listing.candidate_rate > 0)
                out += sprintf_s(out, (size_t)(body + sizeof(body) - out),
                                 ",\"output_rate\":%.0f", device_listing.candidate_rate);
            out += sprintf_s(out, (size_t)(body + sizeof(body) - out),
                             ",\"queue_depth\":%u,\"queue_capacity\":%u",
                             tx->queue_depth(tx->context), tx->queue_capacity);
            tx->ptt_status(tx->context, port_name, &dtr, &open);
            out += sprintf_s(out, (size_t)(body + sizeof(body) - out), ",\"ptt\":{\"port\":");
            if (*port_name)
                out = append_json_string(out, (const uint8_t *)port_name, strlen(port_name));
            else
                out += sprintf_s(out, (size_t)(body + sizeof(body) - out), "null");
            out += sprintf_s(out, (size_t)(body + sizeof(body) - out),
                ",\"line\":\"%s\",\"is_open\":%s}", dtr ? "dtr" : "rts",
                open < 0 ? "null" : open ? "true" : "false");
        }
        *out++ = '}';
        *out = '\0';
        reply(client, "200 OK", json, body, "");
    } else if (strcmp(path, "/api/v1/messages") == 0) {
        reply_history(client, query);
    } else if (strcmp(path, "/api/v1/monitor") == 0) {
        reply_monitor(client);
    } else if (strcmp(path, "/api/v1/settings/help") == 0) {
        reply_settings_help(client);
    } else if (!serve_web_asset(client, path)) {
        reply(client, "404 Not Found", json, "{\"error\":\"not_found\"}", "");
    }
}

int mu16tty_serve_local_http(unsigned short port, mu16tty_http_input *input,
                            const mu16tty_http_transmitter *tx, void *audio,
                            int (*stop_requested)(void), int open_browser)
{
    WSADATA data;
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address = {0};
    int length = sizeof(address);
    BOOL exclusive = TRUE;
    int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        fprintf(stderr, "Winsock initialization failed (%d).\n", result);
        return 1;
    }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD path_length = GetModuleFileNameW(NULL, http_ini, ARRAYSIZE(http_ini));
    wchar_t *directory = path_length && path_length < ARRAYSIZE(http_ini) ? wcsrchr(http_ini, L'\\') : NULL;
    if (!directory || wcscpy_s(directory + 1, ARRAYSIZE(http_ini) - (directory + 1 - http_ini), L"config") != 0) {
        result = ERROR_INVALID_NAME;
        goto cleanup;
    }
    CreateDirectoryW(http_ini, NULL);
    if (wcscat_s(http_ini, ARRAYSIZE(http_ini), L"\\http.ini") != 0) {
        result = ERROR_INVALID_NAME;
        goto cleanup;
    }
    GetPrivateProfileStringW(L"http", L"id", L"", http_id, ARRAYSIZE(http_id), http_ini);
    GetPrivateProfileStringW(L"http", L"pass", L"", http_pass, ARRAYSIZE(http_pass), http_ini);
    GetPrivateProfileStringW(L"http", L"allow", L"", http_allow, ARRAYSIZE(http_allow), http_ini);
    http_lan = *http_id && *http_pass && *http_allow;
    if (port == 0)
        port = (unsigned short)GetPrivateProfileIntW(L"http", L"port", 0, http_ini);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(http_lan ? INADDR_ANY : INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (listener == INVALID_SOCKET ||
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive)) != 0 ||
        bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, SOMAXCONN) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &length) != 0) {
        result = WSAGetLastError();
        goto cleanup;
    }
    history_started_at = GetTickCount64();
    if (open_browser && port == 0) {
        wchar_t assigned[6];
        swprintf_s(assigned, ARRAYSIZE(assigned), L"%u", (unsigned)ntohs(address.sin_port));
        if (!http_save(L"port", assigned))
            fputs("Could not save HTTP port to config/http.ini.\n", stderr);
    }
    unsigned bound_port = ntohs(address.sin_port);
    printf("Port: %u\nID: %ls\nPassword: %s\nAllowed IPv4: %ls\n\n"
           "AT YOUR OWN RISK.\n\nhttp://127.0.0.1:%u/\n\nReady.\n",
           bound_port, *http_id ? http_id : L"(unset)",
           *http_pass ? "************" : "(unset)",
           http_lan ? http_allow : L"(loopback only)", bound_port);
    fflush(stdout);
    ULONGLONG next_status = GetTickCount64() + 30000;
    while (!stop_requested()) {
        int pending = readable(listener, 100);
        if (GetTickCount64() >= next_status) {
            unsigned rx_queue = (unsigned)InterlockedCompareExchange(&queue_reading, 0, 0);
            printf("HTTP :%u | RX %s | RX buffer %u/%u (peak %u) | TX queue %u/%u\n",
                   bound_port, input == NULL ? "disabled" : input->running(input->context) ? "running" : "failed",
                   rx_queue & 255u, rx_queue >> 16, (rx_queue >> 8) & 255u,
                   tx ? tx->queue_depth(tx->context) : 0, tx ? tx->queue_capacity : 0);
            fflush(stdout);
            next_status = GetTickCount64() + 30000;
        }
        if (pending == SOCKET_ERROR) {
            result = WSAGetLastError();
            break;
        }
        if (pending > 0) {
            struct sockaddr_in peer;
            int peer_length = sizeof(peer);
            SOCKET client = accept(listener, (struct sockaddr *)&peer, &peer_length);
            if (client != INVALID_SOCKET) {
                if (http_allowed(ntohl(peer.sin_addr.s_addr)))
                    serve_client(client, input, ntohs(address.sin_port), tx, audio, stop_requested);
                shutdown(client, SD_SEND);
                closesocket(client);
            }
        }
    }

cleanup:
    if (listener != INVALID_SOCKET)
        closesocket(listener);
    WSACleanup();
    if (result != 0)
        fprintf(stderr, "Local HTTP server failed (%d).\n", result);
    return result != 0;
}
