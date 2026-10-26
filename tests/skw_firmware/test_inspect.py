# SPDX-License-Identifier: Apache-2.0
"""Synthetic format tests; no vendor firmware is embedded in the repository."""

import importlib.util
from pathlib import Path
import struct
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/skw-firmware-inspect.py"
SPEC = importlib.util.spec_from_file_location("skw_firmware", SCRIPT)
FW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FW)


def fixture():
    iram = bytearray(1024)
    dram = bytes(513)
    nv = bytearray(60)
    iram[32:36] = b"TSVN"
    iram[64:68] = b"DEVN"
    iram[128:136] = b"kees0616"
    struct.pack_into("<II", iram, 136, 0x100000, 0x20200000)
    for idx in range(3):
        struct.pack_into("<III", iram, 144 + idx * 12,
                         0x100200 + idx * 4, ((idx + 1) << 24) | 4,
                         0x100200 + idx * 4)
    iram[180:188] = b"evaw0616"
    struct.pack_into("<IIII", nv, 8, 32, 28, 0, 0)
    nv[32:60] = bytes(range(28))
    return iram, dram, nv


class FirmwareTests(unittest.TestCase):
    def test_valid_merge_and_plan(self):
        iram, dram, nv = fixture()
        before = bytes(iram)
        report, merged = FW.prepare(iram, dram, nv)
        self.assertEqual(bytes(iram), before)
        self.assertEqual(merged[:36], before[:36])
        self.assertEqual(merged[36:64], nv[32:60])
        self.assertEqual(merged[64:], before[64:])
        self.assertEqual([m["service"] for m in report["metadata"]["modules"]],
                         ["bsp", "wifi", "bluetooth"])
        self.assertEqual([x["image"] for x in report["download_plan"]],
                         ["dram", "iram"])
        self.assertEqual(report["download_plan"][0]["last_chunk_size"], 1)
        self.assertEqual(report["download_plan"][1]["last_chunk_size"], 512)
        self.assertTrue(report["completion"]["requires_rx_transport"])

    def test_crc_vector(self):
        self.assertEqual(FW.digest(b"123456789")["crc16_xmodem"], "31c3")

    def test_truncated_images(self):
        iram, dram, nv = fixture()
        for size in (0, 1, 31, 135, 143, 181, 187):
            with self.subTest(size=size), self.assertRaises(FW.FirmwareError):
                FW.prepare(iram[:size], dram, nv)

    def test_bad_module(self):
        for offset, value in ((148, 0xff000004), (144, 0x30000000),
                              (144, 0x1003ff), (152, 0xffffffff), (148, 0)):
            iram, dram, nv = fixture()
            struct.pack_into("<I", iram, offset, value)
            with self.subTest(offset=offset, value=value):
                with self.assertRaises(FW.FirmwareError):
                    FW.prepare(iram, dram, nv)

    def test_bad_nv(self):
        for field, value in ((8, 0), (8, 0xfffffff0), (12, 29),
                             (12, 0), (16, 32), (20, 4)):
            iram, dram, nv = fixture()
            struct.pack_into("<I", nv, field, value)
            with self.subTest(field=field, value=value):
                with self.assertRaises(FW.FirmwareError):
                    FW.prepare(iram, dram, nv)
        iram, dram, nv = fixture()
        with self.assertRaises(FW.FirmwareError):
            FW.prepare(iram, dram, nv[:28])

    def test_markers_and_overlap(self):
        for tag, offset in ((b"kees0616", 200), (b"TSVN", 220),
                            (b"DEVN", 160)):
            iram, dram, nv = fixture()
            iram[offset:offset + len(tag)] = tag
            with self.subTest(tag=tag), self.assertRaises(FW.FirmwareError):
                FW.prepare(iram, dram, nv)
        iram, dram, nv = fixture()
        struct.pack_into("<I", iram, 140, 0x100200)
        with self.assertRaises(FW.FirmwareError):
            FW.prepare(iram, dram, nv)

    def test_truncated_nv_container(self):
        iram, dram, nv = fixture()
        for size in range(len(nv)):
            with self.subTest(size=size), self.assertRaises(FW.FirmwareError):
                FW.prepare(iram, dram, nv[:size])

    def test_pin_config_is_not_silently_ignored(self):
        iram, dram, nv = fixture()
        nv.extend(bytes(8))
        struct.pack_into("<II", nv, 16, 60, 8)
        report, _ = FW.prepare(iram, dram, nv)
        self.assertTrue(report["nv"]["requires_post_ready_pin_config"])

    def test_bad_pin_records(self):
        for size in (4, 7, 9, 168):
            iram, dram, nv = fixture()
            nv.extend(bytes(size))
            struct.pack_into("<II", nv, 16, 60, size)
            with self.subTest(size=size), self.assertRaises(FW.FirmwareError):
                FW.prepare(iram, dram, nv)


if __name__ == "__main__":
    unittest.main()
