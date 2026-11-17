#include "mu16tty_windows_audio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Public PortAudio V19 ABI, accessed through the already loaded audio DLL. */
typedef struct pa_device_info {
    int struct_version;
    const char *name;
    int host_api;
    int input_channels;
    int output_channels;
    double default_low_input_latency;
    double default_low_output_latency;
    double default_high_input_latency;
    double default_high_output_latency;
    double default_sample_rate;
} pa_device_info;

typedef struct pa_host_api_info {
    int struct_version;
    int type;
    const char *name;
    int device_count;
    int default_input;
    int default_output;
} pa_host_api_info;

typedef int (__cdecl *pa_get_index)(void);
typedef const pa_device_info *(__cdecl *pa_get_device_info)(int);
typedef const pa_host_api_info *(__cdecl *pa_get_host_api_info)(int);

typedef struct pa_stream_parameters {
    int device;
    int channels;
    unsigned long sample_format;
    double suggested_latency;
    void *host_api_stream_info;
} pa_stream_parameters;

typedef int (__cdecl *pa_stream_callback)(
    const void *, void *, unsigned long, const void *, unsigned long, void *);
typedef int (__cdecl *pa_format_supported)(
    const pa_stream_parameters *, const pa_stream_parameters *, double);
typedef int (__cdecl *pa_open_stream)(
    void **, const pa_stream_parameters *, const pa_stream_parameters *, double,
    unsigned long, unsigned long, pa_stream_callback, void *);
typedef int (__cdecl *pa_write_stream)(void *, const void *, unsigned long);
typedef int (__cdecl *pa_stream_control)(void *);
typedef void (__cdecl *pa_finished_callback)(void *);
typedef int (__cdecl *pa_set_finished_callback)(void *, pa_finished_callback);

#ifdef _MSC_VER
#pragma warning(push)
/* GetProcAddress returns a generic pointer. These signatures match portaudio.h. */
#pragma warning(disable: 4191)
#endif
MU16TTY_API int mu16tty_windows_audio_initialize(void *module, int initialize)
{
    pa_get_index control;
    if (module == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    control = (pa_get_index)GetProcAddress(
        module, initialize ? "Pa_Initialize" : "Pa_Terminate");
    return control != NULL ? control() : MU16TTY_ERROR_INVALID_ARGUMENT;
}

MU16TTY_API int mu16tty_windows_audio_devices(void *module, mu16tty_audio_device_callback callback)
{
    HMODULE audio = module;
    pa_get_index count_devices;
    pa_get_device_info get_device;
    pa_get_host_api_info get_host_api;
    int count;
    int index;

    if (audio == NULL || callback == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    count_devices = (pa_get_index)GetProcAddress(audio, "Pa_GetDeviceCount");
    get_device = (pa_get_device_info)GetProcAddress(audio, "Pa_GetDeviceInfo");
    get_host_api = (pa_get_host_api_info)GetProcAddress(audio, "Pa_GetHostApiInfo");
    if (count_devices == NULL || get_device == NULL || get_host_api == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    count = count_devices();
    if (count < 0)
        return count;
    for (index = 0; index < count; ++index) {
        const pa_device_info *device = get_device(index);
        const pa_host_api_info *host;
        if (device == NULL || device->struct_version != 2)
            return MU16TTY_ERROR_INVALID_ARGUMENT;
        host = get_host_api(device->host_api);
        if (host == NULL || host->struct_version != 1)
            return MU16TTY_ERROR_INVALID_ARGUMENT;
        callback(index, device->name, device->input_channels, device->output_channels,
                 device->default_sample_rate, host->name, host->type);
    }
    return count;
}

MU16TTY_API int mu16tty_windows_audio_default(void *module, int input)
{
    HMODULE audio = module;
    pa_get_index get_default;

    if (audio == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    get_default = (pa_get_index)GetProcAddress(
        audio, input ? "Pa_GetDefaultInputDevice" : "Pa_GetDefaultOutputDevice");
    return get_default != NULL ? get_default() : MU16TTY_ERROR_INVALID_ARGUMENT;
}

MU16TTY_API int mu16tty_windows_audio_output(void *module, int device, double rate, void **stream)
{
    HMODULE audio = module;
    pa_get_device_info get_device;
    const pa_device_info *info;
    pa_stream_parameters output = {0};

    if (audio == NULL || rate <= 0)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    get_device = (pa_get_device_info)GetProcAddress(audio, "Pa_GetDeviceInfo");
    if (get_device == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    info = get_device(device);
    if (info == NULL || info->output_channels < 1)
        return -9996; /* paInvalidDevice */
    output.device = device;
    output.channels = 1;
    output.sample_format = 1; /* paFloat32 */
    output.suggested_latency = info->default_high_output_latency;

    if (stream == NULL) {
        pa_format_supported check = (pa_format_supported)GetProcAddress(audio, "Pa_IsFormatSupported");
        return check != NULL ? check(NULL, &output, rate) : MU16TTY_ERROR_INVALID_ARGUMENT;
    } else {
        pa_open_stream open_stream = (pa_open_stream)GetProcAddress(audio, "Pa_OpenStream");
        *stream = NULL;
        return open_stream != NULL
            ? open_stream(stream, NULL, &output, rate, 0, 0, NULL, NULL)
            : MU16TTY_ERROR_INVALID_ARGUMENT;
    }
}

MU16TTY_API int mu16tty_windows_audio_write(
    void *module, void *stream, const float *samples, unsigned long frames)
{
    pa_write_stream write_stream;
    int result;
    if (module == NULL || stream == NULL || samples == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    write_stream = (pa_write_stream)GetProcAddress(module, "Pa_WriteStream");
    if (write_stream == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    result = write_stream(stream, samples, frames);
    return result == -9980 ? MU16TTY_OK : result; /* paOutputUnderflowed */
}

int mu16tty_windows_audio_input_with_context(
    void *module, int device, double rate, unsigned long block_frames,
    mu16tty_audio_stream_callback callback, mu16tty_audio_finished_callback finished,
    void *user_data, void **stream)
{
    HMODULE audio = module;
    pa_get_device_info get_device;
    pa_open_stream open_stream;
    pa_set_finished_callback set_finished;
    pa_stream_control close_stream;
    const pa_device_info *info;
    pa_stream_parameters input = {0};
    int result;

    if (audio == NULL || stream == NULL || callback == NULL ||
        finished == NULL || rate <= 0 || block_frames == 0)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    *stream = NULL;
    get_device = (pa_get_device_info)GetProcAddress(audio, "Pa_GetDeviceInfo");
    open_stream = (pa_open_stream)GetProcAddress(audio, "Pa_OpenStream");
    set_finished = (pa_set_finished_callback)GetProcAddress(audio, "Pa_SetStreamFinishedCallback");
    close_stream = (pa_stream_control)GetProcAddress(audio, "Pa_CloseStream");
    if (get_device == NULL || open_stream == NULL || set_finished == NULL || close_stream == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    info = get_device(device);
    if (info == NULL || info->input_channels < 1)
        return -9996; /* paInvalidDevice */
    input.device = device;
    input.channels = 1;
    input.sample_format = 1; /* paFloat32 */
    input.suggested_latency = info->default_high_input_latency;
    result = open_stream(stream, &input, NULL, rate, block_frames, 0, callback, user_data);
    if (result != MU16TTY_OK)
        return result;
    result = set_finished(*stream, finished);
    if (result != MU16TTY_OK) {
        close_stream(*stream);
        *stream = NULL;
    }
    return result;
}

MU16TTY_API int mu16tty_windows_audio_input(
    void *module, int device, double rate, unsigned long block_frames,
    pa_stream_callback callback, pa_finished_callback finished, void **stream)
{
    return mu16tty_windows_audio_input_with_context(
        module, device, rate, block_frames, callback, finished, NULL, stream);
}

MU16TTY_API int mu16tty_windows_audio_stream_control(void *module, void *stream, unsigned operation)
{
    const char *names[] = {"Pa_StartStream", "Pa_StopStream", "Pa_AbortStream", "Pa_CloseStream"};
    pa_stream_control control;
    int result;
    if (module == NULL || stream == NULL || operation >= ARRAYSIZE(names))
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    control = (pa_stream_control)GetProcAddress(module, names[operation]);
    if (control == NULL)
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    result = control(stream);
    return operation == 2u && result == -9983 ? MU16TTY_OK : result; /* paStreamIsStopped */
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
