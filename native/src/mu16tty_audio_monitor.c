#include "mu16tty_modem.h"

#include <math.h>
#include <stdlib.h>

static const double TAU = 6.28318530717958647692;

typedef struct spectrum_sample {
    double real;
    double imag;
} spectrum_sample;

static void fft(spectrum_sample *samples, size_t count) {
    size_t index, reverse = 0u, width;
    for (index = 1u; index < count; ++index) {
        size_t bit = count / 2u;
        while ((reverse & bit) != 0u) {
            reverse ^= bit;
            bit >>= 1u;
        }
        reverse ^= bit;
        if (index < reverse) {
            const spectrum_sample temp = samples[index];
            samples[index] = samples[reverse];
            samples[reverse] = temp;
        }
    }
    for (width = 2u; width <= count; width *= 2u) {
        const size_t half = width / 2u;
        const double cos_step = cos(-TAU / (double)width);
        const double sin_step = sin(-TAU / (double)width);
        size_t base;
        for (base = 0u; base < count; base += width) {
            double c = 1.0, s = 0.0;
            for (index = 0u; index < half; ++index) {
                spectrum_sample *low = &samples[base + index];
                spectrum_sample *high = low + half;
                const double real = c * high->real - s * high->imag;
                const double imag = c * high->imag + s * high->real;
                const double next_c = c * cos_step - s * sin_step;
                high->real = low->real - real;
                high->imag = low->imag - imag;
                low->real += real;
                low->imag += imag;
                s = s * cos_step + c * sin_step;
                c = next_c;
            }
        }
    }
}

static double clean(float sample) {
    return isfinite(sample) ? (double)sample : 0.0;
}

static double dbfs(double amplitude) {
    return amplitude > 1.0e-6 ? 20.0 * log10(amplitude) : -120.0;
}

int mu16tty_analyze_audio_monitor(
    const float *samples, size_t count, uint32_t sample_rate, double levels[2],
    double *waveform, size_t waveform_points, double *spectrum, size_t spectrum_bins
) {
    size_t index, fft_count = 1u;
    double peak = 0.0, energy = 0.0, window_sum = 0.0, scale;
    spectrum_sample *buffer;
    if (samples == NULL || count == 0u || count > 65536u || sample_rate == 0u ||
        levels == NULL || waveform == NULL || waveform_points == 0u ||
        spectrum == NULL || spectrum_bins == 0u) {
        return MU16TTY_ERROR_INVALID_ARGUMENT;
    }
    while (fft_count < count) fft_count *= 2u;
    buffer = (spectrum_sample *)calloc(fft_count, sizeof(*buffer));
    if (buffer == NULL) return MU16TTY_ERROR_NO_MEMORY;
    for (index = 0u; index < count; ++index) {
        const double sample = clean(samples[index]);
        const double window = count > 1u
            ? 0.5 - 0.5 * cos(TAU * (double)index / (double)(count - 1u)) : 1.0;
        buffer[index].real = sample * window;
        window_sum += window;
        energy += sample * sample;
        if (fabs(sample) > peak) peak = fabs(sample);
    }
    levels[0] = dbfs(sqrt(energy / (double)count));
    levels[1] = dbfs(peak);
    for (index = 0u; index < waveform_points; ++index) {
        const double pos = waveform_points > 1u
            ? (double)index * (double)(count - 1u) / (double)(waveform_points - 1u) : 0.0;
        const size_t start = (size_t)pos;
        double value = clean(samples[start]);
        if (start + 1u < count) {
            value += (pos - (double)start) * (clean(samples[start + 1u]) - value);
        }
        waveform[index] = fmax(-1.0, fmin(1.0, value));
    }
    fft(buffer, fft_count);
    scale = fmax(1.0, window_sum / 2.0);
    for (index = 0u; index < spectrum_bins; ++index) spectrum[index] = 0.0;
    for (index = 0u; index <= fft_count / 2u; ++index) {
        const double frequency = (double)index * sample_rate / (double)fft_count;
        const double real = buffer[index].real, imag = buffer[index].imag;
        size_t bin;
        if (frequency > 3000.0) break;
        bin = (size_t)(frequency / 3000.0 * (double)spectrum_bins);
        if (bin == spectrum_bins) --bin;
        spectrum[bin] = fmax(spectrum[bin], sqrt(real * real + imag * imag) / scale);
    }
    free(buffer);
    for (index = 0u; index < spectrum_bins; ++index) spectrum[index] = dbfs(spectrum[index]);
    return MU16TTY_OK;
}
