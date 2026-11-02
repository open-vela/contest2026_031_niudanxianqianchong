#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inject ALLOCBUFFER failures into audio_smoke's real allocation functions."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


STUBS = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FAR
#define OK 0
#define LOG_DEBUG 7
#define syslog(...) ((void)0)
#define AUDIOIOC_ALLOCBUFFER 1
#define AUDIOIOC_FREEBUFFER 2
struct ap_buffer_s { int value; };
struct audio_buf_desc_s {
    unsigned numbytes;
    union { struct ap_buffer_s **pbuffer; struct ap_buffer_s *buffer; } u;
};
struct audio_smoke_state_s {
    struct ap_buffer_s **buffers;
    struct { unsigned nbuffers, buffer_size; } buffer_info;
    int audio_fd;
};
static int mode, calls, allocated, released;
static int test_ioctl(int fd, int command, unsigned long arg)
{
    struct audio_buf_desc_s *desc = (void *)(uintptr_t)arg;
    (void)fd;
    if (command == AUDIOIOC_FREEBUFFER) {
        assert(desc->u.buffer != NULL);
        free(desc->u.buffer);
        released++;
        return 0;
    }
    assert(command == AUDIOIOC_ALLOCBUFFER);
    assert(desc->numbytes == 4096);
    assert(desc->u.pbuffer != NULL && *desc->u.pbuffer == NULL);
    calls++;
    if (mode == 1 || (mode == 4 && calls == 2)) return 0;
    if (mode == 2) return sizeof(*desc);
    if (mode == 3) { errno = ENOMEM; return -1; }
    *desc->u.pbuffer = calloc(1, sizeof(struct ap_buffer_s));
    assert(*desc->u.pbuffer);
    allocated++;
    return sizeof(*desc);
}
#define ioctl test_ioctl
"""

MAIN = r"""
int main(void)
{
    for (mode = 0; mode <= 4; mode++) {
        struct audio_smoke_state_s state;
        memset(&state, 0, sizeof(state));
        state.buffer_info.nbuffers = 2;
        state.buffer_info.buffer_size = 4096;
        calls = allocated = released = 0;
        int result = audio_smoke_allocate_buffers(&state);
        assert(result == (mode == 0 ? OK : -ENOMEM));
        if (mode == 0) assert(allocated == 2);
        if (mode == 4) assert(allocated == 1);
        audio_smoke_free_buffers(&state);
        assert(allocated == released && state.buffers == NULL);
        printf("PASS: allocation mode=%d\n", mode);
    }
    return 0;
}
"""


def extract_function(source, signature):
    start = source.index(signature)
    end = re.search(r"^}", source[start:], re.MULTILINE)
    if end is None:
        raise ValueError(signature)
    return source[start:start + end.end()] + "\n"


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=root / "demos/smart_home/src/audio/audio_smoke_main.c")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = args.source.read_text()
    code = STUBS
    for name in ("audio_smoke_allocate_buffers", "audio_smoke_free_buffers"):
        code += extract_function(source, "static " +
                                 ("int " if "allocate" in name else "void ") +
                                 name + "(")
    with tempfile.TemporaryDirectory(prefix="audio-smoke-buffers-") as tmp:
        test = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        test.write_text(code + MAIN)
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
