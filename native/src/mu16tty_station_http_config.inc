#include <ws2tcpip.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

static wchar_t http_ini[MAX_PATH];
static wchar_t http_id[128], http_pass[128], http_allow[1024];
static int http_lan;

static int http_save(const wchar_t *key, const wchar_t *value)
{
    return WritePrivateProfileStringW(L"http", key, value, http_ini) != 0;
}

static int http_cidr(const wchar_t *value, ULONG *network, ULONG *mask)
{
    wchar_t ip[16];
    const wchar_t *slash = wcschr(value, L'/');
    size_t length = slash ? (size_t)(slash - value) : wcslen(value);
    long bits = 32;
    IN_ADDR address;
    if (!length || length >= ARRAYSIZE(ip))
        return 0;
    if (slash) {
        wchar_t *end;
        bits = wcstol(slash + 1, &end, 10);
        if (end == slash + 1 || *end || bits < 0 || bits > 32)
            return 0;
    }
    wmemcpy(ip, value, length);
    ip[length] = 0;
    if (InetPtonW(AF_INET, ip, &address) != 1)
        return 0;
    *mask = bits ? 0xffffffffUL << (32 - bits) : 0;
    *network = ntohl(address.s_addr) & *mask;
    return 1;
}

static int http_allowed(ULONG address)
{
    wchar_t list[ARRAYSIZE(http_allow)], *state, *item;
    if ((address >> 24) == 127)
        return 1;
    if (!http_lan || !*http_id || !*http_pass)
        return 0;
    wcscpy_s(list, ARRAYSIZE(list), http_allow);
    for (item = wcstok_s(list, L",", &state); item; item = wcstok_s(NULL, L",", &state)) {
        ULONG network, mask;
        if (http_cidr(item, &network, &mask) && (address & mask) == network)
            return 1;
    }
    return 0;
}

static int http_authorized(const char *headers, const char *end)
{
    wchar_t credentials[256];
    char utf8[1024], encoded[1400];
    DWORD size = sizeof(encoded);
    if (!*http_id || !*http_pass)
        return 1;
    swprintf_s(credentials, ARRAYSIZE(credentials), L"%ls:%ls", http_id, http_pass);
    if (!WideCharToMultiByte(CP_UTF8, 0, credentials, -1, utf8, sizeof(utf8), NULL, NULL) ||
        !CryptBinaryToStringA((const BYTE *)utf8, (DWORD)strlen(utf8),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded, &size))
        return 0;
    while (headers < end) {
        const char *next = strstr(headers, "\r\n");
        if (!next || next > end)
            break;
        if (next - headers > 20 && _strnicmp(headers, "Authorization: Basic ", 21) == 0)
            return (size_t)(next - headers - 21) == strlen(encoded) &&
                   memcmp(headers + 21, encoded, strlen(encoded)) == 0;
        headers = next + 2;
    }
    return 0;
}

static int http_command(const wchar_t *args, char *message, size_t capacity, int *changed)
{
    wchar_t updated[ARRAYSIZE(http_allow)], value[128];
    wchar_t summary[1600];
    char utf8[6400];
    const wchar_t *key = NULL;
    const wchar_t *saved = value;
    if (!*args || _wcsicmp(args, L"allow") == 0) {
        if (_wcsicmp(args, L"allow") == 0)
            swprintf_s(summary, ARRAYSIZE(summary), L"Allowed IPv4: %ls", *http_allow ? http_allow : L"(loopback only)");
        else
            swprintf_s(summary, ARRAYSIZE(summary), L"Port: %u\nID: %ls\nPassword: %ls\nAllowed IPv4: %ls\n\n"
                       L"http port <1..65535>\nhttp allow\nhttp allow add|del <IPv4/CIDR>\nhttp id <ID>\nhttp pass <password>",
                       GetPrivateProfileIntW(L"http", L"port", 0, http_ini),
                       *http_id ? http_id : L"(unset)", *http_pass ? L"(set)" : L"(unset)",
                       *http_allow ? http_allow : L"(loopback only)");
        WideCharToMultiByte(CP_UTF8, 0, summary, -1, utf8, sizeof(utf8), NULL, NULL);
        snprintf(message, capacity, "%s", utf8);
        return 1;
    }
    if (_wcsnicmp(args, L"port ", 5) == 0) {
        wchar_t *end;
        long port = wcstol(args + 5, &end, 10);
        if (*end || port < 1 || port > 65535)
            goto invalid;
        swprintf_s(value, ARRAYSIZE(value), L"%ld", port);
        key = L"port";
    } else if (_wcsnicmp(args, L"id ", 3) == 0 || _wcsnicmp(args, L"pass ", 5) == 0) {
        int id = _wcsnicmp(args, L"id ", 3) == 0;
        const wchar_t *argument = args + (id ? 3 : 5);
        if (!*argument || wcslen(argument) >= ARRAYSIZE(value) ||
            wcspbrk(argument, L"\r\n\"") || (id && wcschr(argument, L':')))
            goto invalid;
        wcscpy_s(value, ARRAYSIZE(value), argument);
        key = id ? L"id" : L"pass";
    } else if (_wcsnicmp(args, L"allow add ", 10) == 0 || _wcsnicmp(args, L"allow del ", 10) == 0) {
        ULONG network, mask;
        wchar_t list[ARRAYSIZE(http_allow)], *state, *item;
        int add = _wcsnicmp(args, L"allow add ", 10) == 0;
        if (!http_cidr(args + 10, &network, &mask))
            goto invalid;
        wcscpy_s(list, ARRAYSIZE(list), http_allow);
        updated[0] = 0;
        for (item = wcstok_s(list, L",", &state); item; item = wcstok_s(NULL, L",", &state)) {
            ULONG old_network, old_mask;
            if (http_cidr(item, &old_network, &old_mask) && old_network == network && old_mask == mask)
                continue;
            if (*updated)
                wcscat_s(updated, ARRAYSIZE(updated), L",");
            wcscat_s(updated, ARRAYSIZE(updated), item);
        }
        if (add) {
            if (wcslen(updated) + wcslen(args + 10) + 2 >= ARRAYSIZE(updated) - 80)
                goto invalid;
            if (*updated)
                wcscat_s(updated, ARRAYSIZE(updated), L",");
            wcscat_s(updated, ARRAYSIZE(updated), args + 10);
        }
        key = L"allow";
        saved = updated;
    } else {
        goto invalid;
    }
    if (!http_save(key, saved)) {
        snprintf(message, capacity, "Could not save config/http.ini.");
        return -1;
    }
    if (wcscmp(key, L"id") == 0)
        wcscpy_s(http_id, ARRAYSIZE(http_id), value);
    else if (wcscmp(key, L"pass") == 0)
        wcscpy_s(http_pass, ARRAYSIZE(http_pass), value);
    else if (wcscmp(key, L"allow") == 0)
        wcscpy_s(http_allow, ARRAYSIZE(http_allow), updated);
    *changed = 1;
    snprintf(message, capacity, "Saved HTTP settings. Restart to apply port or LAN listener changes.");
    return 1;

invalid:
    snprintf(message, capacity, "Invalid HTTP setting. Enter http for usage.");
    return 0;
}
