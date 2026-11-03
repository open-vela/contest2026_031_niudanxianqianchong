/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* PCM16 little endian mono, 24kHz -> 16kHz, duration preserved to one output
 * sample. Non-overlapping caller-owned buffers; returns 0 or -errno.
 * Filter attenuates energy above the output Nyquist band before decimation.
 */
size_t smart_home_voice_resample_bytes(size_t input_bytes);
int smart_home_voice_resample_24k_16k(const uint8_t *input, size_t input_bytes,
    uint8_t *output, size_t capacity, size_t *output_bytes);
