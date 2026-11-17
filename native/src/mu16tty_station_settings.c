#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mu16tty_station_settings.h"
#include "mu16tty_windows_audio.h"

static wchar_t settings_directory[32768];
static wchar_t settings_path[32768];
static SRWLOCK settings_lock = SRWLOCK_INIT;
static mu16tty_timing current = {250, 600, 250};

static char *read_json_file(const wchar_t *path)
{
    FILE *file = NULL;
    errno_t error = _wfopen_s(&file, path, L"rb");
    long size;
    char *text;
    if (error != 0)
        return error == ENOENT ? _strdup("{}") : NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        size > 65536 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (text == NULL || fread(text, 1, (size_t)size, file) != (size_t)size ||
        memchr(text, '\0', (size_t)size) != NULL) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[size] = '\0';
    fclose(file);
    return text;
}

static char *read_settings_file(void)
{
    return read_json_file(settings_path);
}

int mu16tty_settings_callsign(wchar_t callsign[13])
{
    static wchar_t path[32768];
    sqlite3 *db = NULL;
    sqlite3_stmt *row = NULL;
    char *text, *line;
    int result = 0;
    if (swprintf_s(path, ARRAYSIZE(path), L"%ls\\station.json", settings_directory) < 0)
        return 0;
    text = read_json_file(path);
    if (text == NULL)
        return 0;
    /* The Python launcher emits full-line comments above its JSON template. */
    for (line = text; *line; ) {
        char *next = strchr(line, '\n');
        char *start = line + strspn(line, " \t\r");
        if (strncmp(start, "//", 2) == 0)
            memset(line, ' ', next != NULL ? (size_t)(next - line) : strlen(line));
        if (next == NULL)
            break;
        line = next + 1;
    }
    if (sqlite3_open(":memory:", &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
            "SELECT COALESCE(json_extract(?1,'$.callsign'),'NOCALL') "
            "WHERE json_valid(?1)", -1, &row, NULL) == SQLITE_OK &&
        sqlite3_bind_text(row, 1, text, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(row) == SQLITE_ROW && sqlite3_column_type(row, 0) == SQLITE_TEXT) {
        const char *value = (const char *)sqlite3_column_text(row, 0);
        int size = sqlite3_column_bytes(row, 0);
        int index;
        if (value != NULL && size >= 1 && size <= 12) {
            for (index = 0; index < size && value[index] >= 0x20 && value[index] <= 0x7e; ++index)
                callsign[index] = (wchar_t)value[index];
            callsign[index] = L'\0';
            result = index == size;
        }
    }
    sqlite3_finalize(row);
    sqlite3_close(db);
    free(text);
    return result;
}

static void load_timing(const char *text)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *row = NULL;
    static const char query[] =
        "SELECT CASE WHEN json_type(?1,'$.delay_ms')='integer' THEN json_extract(?1,'$.delay_ms') END,"
        "CASE WHEN json_type(?1,'$.wake_ms')='integer' THEN json_extract(?1,'$.wake_ms') END,"
        "CASE WHEN json_type(?1,'$.tail_ms')='integer' THEN json_extract(?1,'$.tail_ms') END "
        "WHERE json_valid(?1)";
    if (sqlite3_open(":memory:", &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, query, -1, &row, NULL) == SQLITE_OK &&
        sqlite3_bind_text(row, 1, text, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(row) == SQLITE_ROW) {
        unsigned *const fields[] = {&current.delay, &current.wake, &current.tail};
        int index;
        for (index = 0; index < (int)ARRAYSIZE(fields); ++index) {
            sqlite3_int64 value = sqlite3_column_int64(row, index);
            if (sqlite3_column_type(row, index) == SQLITE_INTEGER && value >= 0 && value <= 5000)
                *fields[index] = (unsigned)value;
        }
    }
    sqlite3_finalize(row);
    sqlite3_close(db);
}

int mu16tty_settings_load(void)
{
    DWORD length = GetModuleFileNameW(NULL, settings_directory, ARRAYSIZE(settings_directory));
    wchar_t *filename;
    char *text;
    if (length == 0 || length >= ARRAYSIZE(settings_directory) ||
        (filename = wcsrchr(settings_directory, L'\\')) == NULL ||
        wcscpy_s(filename + 1, ARRAYSIZE(settings_directory) -
                 (size_t)(filename + 1 - settings_directory), L"config") != 0 ||
        swprintf_s(settings_path, ARRAYSIZE(settings_path), L"%ls\\settings.json", settings_directory) < 0)
        return 0;
    text = read_settings_file();
    if (text != NULL) {
        load_timing(text);
        free(text);
    }
    return 1;
}

mu16tty_timing mu16tty_settings_timing(void)
{
    mu16tty_timing snapshot;
    AcquireSRWLockShared(&settings_lock);
    snapshot = current;
    ReleaseSRWLockShared(&settings_lock);
    return snapshot;
}

static int replace_settings_file(const char *text, DWORD length)
{
    static wchar_t temporary[32768];
    HANDLE file;
    DWORD written;
    int success;
    if ((!CreateDirectoryW(settings_directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        swprintf_s(temporary, ARRAYSIZE(temporary), L"%ls.%lu.tmp",
                   settings_path, GetCurrentProcessId()) < 0)
        return 0;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    success = WriteFile(file, text, length, &written, NULL) && written == length && FlushFileBuffers(file);
    CloseHandle(file);
    if (success)
        success = MoveFileExW(temporary, settings_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!success)
        DeleteFileW(temporary);
    return success;
}

int mu16tty_settings_ptt(wchar_t port[8], int *dtr, int save)
{
    char *text = read_settings_file();
    sqlite3 *db = NULL;
    sqlite3_stmt *row = NULL;
    char name[8];
    int result = 0;
    const char *query = save
        ? "SELECT json_set(?1,'$.ptt_port',?2,'$.ptt_line',?3) WHERE CASE WHEN json_valid(?1) THEN json_type(?1)='object' ELSE 0 END"
        : "SELECT json_extract(?1,'$.ptt_port'),json_extract(?1,'$.ptt_line') WHERE json_valid(?1) AND json_type(?1,'$.ptt_port') IN ('text','null')";
    if (text != NULL && sqlite3_open(":memory:", &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, query, -1, &row, NULL) == SQLITE_OK &&
        sqlite3_bind_text(row, 1, text, -1, SQLITE_STATIC) == SQLITE_OK &&
        (!save || (WideCharToMultiByte(CP_UTF8, 0, port, -1, name, sizeof(name), NULL, NULL) &&
         (*port ? sqlite3_bind_text(row, 2, name, -1, SQLITE_STATIC) : sqlite3_bind_null(row, 2)) == SQLITE_OK &&
         sqlite3_bind_text(row, 3, *dtr ? "dtr" : "rts", -1, SQLITE_STATIC) == SQLITE_OK)) &&
        sqlite3_step(row) == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(row, 0);
        if (save && value != NULL) {
            result = replace_settings_file(value, (DWORD)sqlite3_column_bytes(row, 0));
        } else if (!save) {
            const char *line = (const char *)sqlite3_column_text(row, 1);
            port[0] = L'\0';
            if (value == NULL || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, port, 8)) {
                *dtr = line != NULL && strcmp(line, "dtr") == 0;
                result = 1;
            } else {
                port[0] = L'\0';
            }
        }
    }
    sqlite3_finalize(row);
    sqlite3_close(db);
    free(text);
    return result;
}

int mu16tty_settings_my(wchar_t callsign[13], int save)
{
    char *text = read_settings_file();
    sqlite3 *db = NULL;
    sqlite3_stmt *row = NULL;
    char ascii[13];
    int result = 0;
    const char *sql = save
        ? "SELECT json_set(?1,'$.callsign',?2) WHERE json_valid(?1) AND json_type(?1)='object'"
        : "SELECT json_extract(?1,'$.callsign') WHERE json_valid(?1)";
    if (text != NULL && sqlite3_open(":memory:", &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &row, NULL) == SQLITE_OK &&
        sqlite3_bind_text(row, 1, text, -1, SQLITE_STATIC) == SQLITE_OK &&
        (!save || (WideCharToMultiByte(CP_UTF8, 0, callsign, -1, ascii, sizeof(ascii), NULL, NULL) &&
                   sqlite3_bind_text(row, 2, ascii, -1, SQLITE_STATIC) == SQLITE_OK)) &&
        sqlite3_step(row) == SQLITE_ROW && sqlite3_column_type(row, 0) == SQLITE_TEXT) {
        const char *value = (const char *)sqlite3_column_text(row, 0);
        int size = sqlite3_column_bytes(row, 0);
        if (save) {
            result = replace_settings_file(value, (DWORD)size);
        } else if (size >= 1 && size <= 12 && strspn(value,
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/") == (size_t)size) {
            MultiByteToWideChar(CP_UTF8, 0, value, -1, callsign, 13);
            result = 1;
        }
    }
    sqlite3_finalize(row);
    sqlite3_close(db);
    free(text);
    return result;
}

static int audio_device_setting(void *audio, int input, int index, int automatic, int save)
{
    char *text = read_settings_file();
    sqlite3 *db = NULL;
    sqlite3_stmt *row = NULL;
    int result = save ? 0 : index;
    const char *query = save
        ? "SELECT json_set(?1,?2,?3) WHERE CASE WHEN json_valid(?1) THEN json_type(?1)='object' ELSE 0 END"
        : "SELECT json_extract(?1,?2),json_type(?1,?2) WHERE json_valid(?1)";
    if (text != NULL && sqlite3_open(":memory:", &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, query, -1, &row, NULL) == SQLITE_OK &&
        sqlite3_bind_text(row, 1, text, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(row, 2, input ? "$.input_device" : "$.output_device", -1, SQLITE_STATIC) == SQLITE_OK &&
        (!save || (automatic ? sqlite3_bind_text(row, 3, "auto", -1, SQLITE_STATIC)
                             : sqlite3_bind_int(row, 3, index)) == SQLITE_OK) &&
        sqlite3_step(row) == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(row, 0);
        if (save && value != NULL) {
            result = replace_settings_file(value, (DWORD)sqlite3_column_bytes(row, 0));
        } else if (!save) {
            const char *type = (const char *)sqlite3_column_text(row, 1);
            if (type != NULL && strcmp(type, "integer") == 0) {
                sqlite3_int64 device = sqlite3_column_int64(row, 0);
                if (device >= 0 && device <= INT_MAX)
                    result = (int)device;
            } else if (value != NULL && strcmp(value, "auto") == 0) {
                result = mu16tty_windows_audio_default(audio, input);
            }
        }
    }
    sqlite3_finalize(row);
    sqlite3_close(db);
    free(text);
    return result;
}

int mu16tty_settings_audio_device(void *audio, int input, int fallback)
{
    return audio_device_setting(audio, input, fallback, 0, 0);
}

int mu16tty_settings_save_audio_device(int input, int index, int automatic)
{
    return audio_device_setting(NULL, input, index, automatic, 1);
}

static int save_timing(mu16tty_timing timing)
{
    static const char query[] =
        "SELECT json_set(?1,'$.delay_ms',?2,'$.wake_ms',?3,'$.tail_ms',?4) "
        "WHERE CASE WHEN json_valid(?1) THEN json_type(?1)='object' ELSE 0 END";
    char *text = read_settings_file();
    sqlite3 *db = NULL;
    sqlite3_stmt *row = NULL;
    int success = 0;
    if (text != NULL && sqlite3_open(":memory:", &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, query, -1, &row, NULL) == SQLITE_OK &&
        sqlite3_bind_text(row, 1, text, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int(row, 2, (int)timing.delay) == SQLITE_OK &&
        sqlite3_bind_int(row, 3, (int)timing.wake) == SQLITE_OK &&
        sqlite3_bind_int(row, 4, (int)timing.tail) == SQLITE_OK &&
        sqlite3_step(row) == SQLITE_ROW) {
        const unsigned char *json = sqlite3_column_text(row, 0);
        if (json != NULL)
            success = replace_settings_file((const char *)json, (DWORD)sqlite3_column_bytes(row, 0));
    }
    sqlite3_finalize(row);
    sqlite3_close(db);
    free(text);
    return success;
}

int mu16tty_settings_command(const wchar_t *command, char *message, size_t capacity, int *changed)
{
    wchar_t key[16];
    size_t key_length, value_length;
    mu16tty_timing timing = mu16tty_settings_timing();
    unsigned *field;
    const char *label;
    unsigned value = 0;
    size_t index;

    *changed = 0;
    command += wcsspn(command, L" \t\r\n");
    key_length = wcscspn(command, L" \t\r\n");
    if (key_length == 0 || key_length >= ARRAYSIZE(key))
        goto invalid;
    memcpy(key, command, key_length * sizeof(*key));
    key[key_length] = L'\0';
    command += key_length;
    command += wcsspn(command, L" \t\r\n");
    if (_wcsicmp(key, L"show") == 0 && *command == L'\0') {
        snprintf(message, capacity, "Delay %u ms\nWake %u ms\nTail %u ms",
                 timing.delay, timing.wake, timing.tail);
        return 1;
    }
    if (_wcsicmp(key, L"delay") == 0) {
        field = &timing.delay;
        label = "Delay";
    } else if (_wcsicmp(key, L"wake") == 0) {
        field = &timing.wake;
        label = "Wake";
    } else if (_wcsicmp(key, L"tail") == 0) {
        field = &timing.tail;
        label = "Tail";
    } else {
        goto invalid;
    }
    if (*command != L'\0') {
        value_length = wcsspn(command, L"0123456789");
        if (value_length == 0 || value_length > 4 ||
            command[value_length + wcsspn(command + value_length, L" \t\r\n")] != L'\0')
            goto invalid;
        for (index = 0; index < value_length; ++index)
            value = value * 10 + (unsigned)(command[index] - L'0');
        if (value > 5000)
            goto invalid;
        if (*field != value) {
            *field = value;
            if (!save_timing(timing)) {
                snprintf(message, capacity, "Could not save config/settings.json.");
                return -1;
            }
            AcquireSRWLockExclusive(&settings_lock);
            current = timing;
            ReleaseSRWLockExclusive(&settings_lock);
            *changed = 1;
        }
    }
    snprintf(message, capacity, "%s %u ms", label, *field);
    return 1;

invalid:
    snprintf(message, capacity, "Use frame delay|wake|tail with an optional value from 0 to 5000 ms.");
    return 0;
}
