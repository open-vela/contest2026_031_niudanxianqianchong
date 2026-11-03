#!/usr/bin/env python3
"""Host checks of the production geometry validator; no DMA hardware model."""

import ctypes as C
import errno
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class Buffer(C.Structure):
    _fields_ = [
        ("data", C.c_void_p),
        ("size", C.c_size_t),
        ("stride", C.c_uint32),
        ("height", C.c_uint16),
        ("format", C.c_int),
    ]


class GraphicsBounds(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="p4-graphics-bounds-")
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        (path / "nuttx").mkdir()
        (path / "arch/chip").mkdir(parents=True)
        (path / "nuttx/config.h").write_text("#define OK 0\n")
        (path / "nuttx/compiler.h").write_text("#define FAR\n")
        (path / "arch/chip/esp_graphics.h").symlink_to(
            ROOT / "chips/esp32p4/include/esp_graphics.h"
        )
        subprocess.run(
            [
                "cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-I", str(path),
                str(ROOT / "chips/esp32p4/common/espressif/esp_graphics_validate.c"),
                "-o", str(path / "bounds.so"),
            ],
            check=True,
        )
        cls.lib = C.CDLL(str(path / "bounds.so"))
        cls.validate = cls.lib.esp_graphics_buffer_bounds
        cls.validate.argtypes = [C.POINTER(Buffer), *([C.c_uint32] * 4)]
        cls.validate.restype = C.c_int

    def check(self, buffer=None, rect=(4, 2, 32, 132), expected=0):
        buffer = buffer or Buffer(0x100000, 49152, 128, 192, 0)
        self.assertEqual(self.validate(C.byref(buffer), *rect), expected)

    def test_different_strides_and_exact_edges(self):
        self.check()
        self.check(Buffer(0x200000, 49152, 192, 192, 0), (8, 3, 32, 132))
        self.check(rect=(0, 0, 64, 192))
        self.check(rect=(62, 191, 2, 1))

    def test_each_format_and_pixel_alignment(self):
        for fmt, stride in [(0, 128), (1, 192), (2, 256), (3, 128)]:
            with self.subTest(format=fmt):
                self.check(Buffer(0x100000, 49152, stride, 192, fmt))
        self.check(rect=(1, 0, 32, 32), expected=-errno.EINVAL)
        self.check(rect=(0, 0, 1, 32), expected=-errno.EINVAL)
        self.check(Buffer(0x100000, 49152, 256, 192, 2), (1, 0, 1, 32))

    def test_invalid_allocations(self):
        for address, size in [(0, 0), (0, 49152), (0x100002, 49152),
                              (0x100000, 49151), (0x100000, 64)]:
            with self.subTest(address=address, size=size):
                self.check(Buffer(address, size, 128, 192, 0),
                           expected=-errno.EINVAL)
        maximum = C.c_size_t(-1).value
        self.check(Buffer(maximum - 63, 128, 128, 1, 0),
                   (0, 0, 2, 1), -errno.EINVAL)

    def test_rect_overflow_and_out_of_bounds(self):
        for rect in [(0, 0, 0, 1), (0, 0, 2, 0), (64, 0, 2, 1),
                     (0, 192, 2, 1), (0xFFFFFFFF, 0, 2, 1),
                     (0, 0xFFFFFFFF, 2, 1), (0, 0, 0xFFFFFFFF, 1)]:
            with self.subTest(rect=rect):
                self.check(rect=rect, expected=-errno.EINVAL)

    def test_descriptor_field_limits(self):
        for stride, height, fmt in [(65536, 1, 0), (128, 16384, 0),
                                    (130, 192, 0), (128, 192, 99),
                                    (0, 192, 0), (128, 0, 0)]:
            with self.subTest(stride=stride, height=height, format=fmt):
                self.check(Buffer(0x100000, 4194304, stride, height, fmt),
                           expected=-errno.EINVAL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
