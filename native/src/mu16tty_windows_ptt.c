#include "mu16tty_windows_ptt.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <wchar.h>

static int valid_port_name(const wchar_t *name, size_t length)
{
    size_t index;

    if (name == NULL || length < 4 || length > 7 ||
        name[0] != L'C' || name[1] != L'O' || name[2] != L'M' ||
        name[3] < L'1' || name[3] > L'9')
        return 0;
    for (index = 4; index < length; ++index) {
        if (name[index] < L'0' || name[index] > L'9')
            return 0;
    }
    return 1;
}

static DWORD clear_lines(HANDLE handle)
{
    DWORD error = ERROR_SUCCESS;

    if (!EscapeCommFunction(handle, CLRRTS))
        error = GetLastError();
    if (!EscapeCommFunction(handle, CLRDTR) && error == ERROR_SUCCESS)
        error = GetLastError();
    return error;
}

/* The station accepts only COM1 through COM9999. Build the device path
 * here so neither a configuration value nor a caller can open a file. */
MU16TTY_API uint32_t mu16tty_windows_ptt_open(
    const wchar_t *name, size_t length, void **port)
{
    wchar_t path[12] = L"\\\\.\\";
    HANDLE handle;
    DCB settings = {0};
    DWORD error;
    size_t index;

    if (port == NULL)
        return ERROR_INVALID_PARAMETER;
    *port = NULL;
    if (!valid_port_name(name, length))
        return ERROR_INVALID_NAME;
    for (index = 0; index < length; ++index)
        path[4 + index] = name[index];

    handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError();

    settings.DCBlength = sizeof(settings);
    if (GetCommState(handle, &settings)) {
        settings.BaudRate = CBR_9600;
        settings.ByteSize = 8;
        settings.Parity = NOPARITY;
        settings.StopBits = ONESTOPBIT;
        settings.fBinary = TRUE;
        settings.fParity = FALSE;
        settings.fRtsControl = RTS_CONTROL_DISABLE;
        settings.fDtrControl = DTR_CONTROL_DISABLE;
        settings.fOutxCtsFlow = FALSE;
        settings.fOutxDsrFlow = FALSE;
        settings.fInX = FALSE;
        settings.fOutX = FALSE;
        if (SetCommState(handle, &settings)) {
            error = clear_lines(handle);
            if (error == ERROR_SUCCESS) {
                *port = handle;
                return ERROR_SUCCESS;
            }
            CloseHandle(handle);
            return error;
        }
    }
    error = GetLastError();
    CloseHandle(handle);
    return error;
}

MU16TTY_API uint32_t mu16tty_windows_ptt_set(void *port, int dtr, int active)
{
    DWORD operation = dtr ? (active ? SETDTR : CLRDTR) :
                           (active ? SETRTS : CLRRTS);
    if (!EscapeCommFunction(port, operation))
        return GetLastError();
    return ERROR_SUCCESS;
}

MU16TTY_API uint32_t mu16tty_windows_ptt_close(void *port)
{
    DWORD error = clear_lines(port);
    if (!CloseHandle(port) && error == ERROR_SUCCESS)
        error = GetLastError();
    return error;
}

static void report_port(HDEVINFO devices, SP_DEVINFO_DATA *device, mu16tty_ptt_port_callback callback)
{
    HKEY key = SetupDiOpenDevRegKey(devices, device, DICS_FLAG_GLOBAL, 0,
                                   DIREG_DEV, KEY_QUERY_VALUE);
    wchar_t port[8] = {0};
    wchar_t name[512] = {0};
    wchar_t product[512] = {0};
    wchar_t instance[512] = {0};
    DWORD port_bytes = sizeof(port);
    DEVPROPTYPE product_type;
    LSTATUS result;

    if (key == INVALID_HANDLE_VALUE)
        return;
    result = RegGetValueW(key, NULL, L"PortName", RRF_RT_REG_SZ, NULL, port, &port_bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || !valid_port_name(port, wcslen(port)))
        return;

    if (!SetupDiGetDeviceRegistryPropertyW(devices, device, SPDRP_FRIENDLYNAME,
                                           NULL, (BYTE *)name, sizeof(name), NULL) &&
        !SetupDiGetDeviceRegistryPropertyW(devices, device, SPDRP_DEVICEDESC,
                                           NULL, (BYTE *)name, sizeof(name), NULL))
        name[0] = L'\0';
    if (!SetupDiGetDevicePropertyW(devices, device, &DEVPKEY_Device_BusReportedDeviceDesc,
                                    &product_type, (BYTE *)product, sizeof(product), NULL, 0) ||
        product_type != DEVPROP_TYPE_STRING)
        product[0] = L'\0';
    if (!SetupDiGetDeviceInstanceIdW(devices, device, instance, ARRAYSIZE(instance), NULL))
        instance[0] = L'\0';
    name[ARRAYSIZE(name) - 1] = L'\0';
    product[ARRAYSIZE(product) - 1] = L'\0';
    callback(port, name, product, instance);
}

uint32_t mu16tty_windows_ptt_list(mu16tty_ptt_port_callback callback)
{
    const GUID *classes[] = {&GUID_DEVCLASS_PORTS, &GUID_DEVCLASS_MODEM};
    size_t index;

    if (callback == NULL)
        return ERROR_INVALID_PARAMETER;
    for (index = 0; index < ARRAYSIZE(classes); ++index) {
        HDEVINFO devices = SetupDiGetClassDevsW(classes[index], NULL, NULL, DIGCF_PRESENT);
        SP_DEVINFO_DATA device = {0};
        DWORD device_index;
        DWORD error;

        if (devices == INVALID_HANDLE_VALUE)
            return GetLastError();
        device.cbSize = sizeof(device);
        for (device_index = 0; SetupDiEnumDeviceInfo(devices, device_index, &device); ++device_index)
            report_port(devices, &device, callback);
        error = GetLastError();
        SetupDiDestroyDeviceInfoList(devices);
        if (error != ERROR_NO_MORE_ITEMS)
            return error;
    }
    return ERROR_SUCCESS;
}
