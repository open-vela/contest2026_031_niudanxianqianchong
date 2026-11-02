/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_voice_resample.h"
#include <errno.h>
#include <limits.h>

/* 33-tap, 7.2kHz low-pass Hamming-windowed sinc at 24kHz. Two phases
 * (0, 0.5 sample delay); Q15 coefficients sum to 32768. No runtime floats.
 */
static const int16_t g_filter[2][33] = {
    {
        -51, 0, 93, -84, -122, 280, 0, -524,
        431, 569, -1211, 0, 2169, -1898, -2969, 9854,
        19694, 9854, -2969, -1898, 2169, 0, -1211, 569,
        431, -524, 0, 280, -122, -84, 93, 0,
        -51
    },
    {
        -16, -48, 66, 37, -173, 77, 281, -384,
        -197, 843, -343, -1183, 1579, 831, -3963, 2111,
        16858, 16858, 2111, -3963, 831, 1579, -1183, -343,
        843, -197, -384, 281, 77, -173, 37, 66,
        -48
    },
};

size_t smart_home_voice_resample_bytes(size_t input_bytes)
{
    size_t samples = input_bytes / 2;
    return (samples / 3 * 2 + samples % 3 * 2 / 3) * 2;
}

static int32_t read_sample(const uint8_t *data, size_t index)
{
    uint32_t value = data[index * 2] | ((uint32_t)data[index * 2 + 1] << 8);
    return value < 32768 ? (int32_t)value : (int32_t)value - 65536;
}

int smart_home_voice_resample_24k_16k(const uint8_t *input, size_t input_bytes,
    uint8_t *output, size_t capacity, size_t *output_bytes)
{
    size_t samples = input_bytes / 2;
    size_t bytes = smart_home_voice_resample_bytes(input_bytes);
    if (!output_bytes) return -EINVAL;
    *output_bytes = 0;
    if (!input || !output || input == output || (input_bytes & 1) || samples < 2)
        return -EINVAL;
    if (capacity < bytes) return -ENOSPC;
    for (size_t n = 0; n < bytes / 2; n++) {
        size_t center = n / 2 * 3 + (n & 1);
        const int16_t *filter = g_filter[n & 1];
        int64_t acc = 0;
        for (int k = -16; k <= 16; k++) {
            size_t index;
            if (k < 0 && center < (size_t)(-k)) index = 0;
            else {
                index = k < 0 ? center - (size_t)(-k) : center + (size_t)k;
                if (index >= samples) index = samples - 1;
            }
            acc += (int64_t)read_sample(input, index) * filter[k + 16];
        }
        int64_t value = acc >= 0 ? (acc + 16384) / 32768 : -((-acc + 16384) / 32768);
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        uint16_t raw = (uint16_t)(int16_t)value;
        output[n * 2] = raw & 255;
        output[n * 2 + 1] = raw >> 8;
    }
    *output_bytes = bytes;
    return 0;
}
