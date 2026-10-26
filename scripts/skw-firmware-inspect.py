#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate SKW6621S SDIO firmware structure and print a read-only boot plan.

Wire-format facts come from the supplied SeekWave SDK skw_boot.c. This is an
independent parser, with explicit bounds checks. It never accesses the board,
modifies input files, or chooses an ALONE/SHARE antenna configuration for you.
"""

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import struct
import sys

HEADER_SCAN = 512
MAX_IMAGE = 16 * 1024 * 1024
MODULE_NAMES = {1: "bsp", 2: "wifi", 3: "bluetooth"}


class FirmwareError(ValueError):
    """The input cannot safely be interpreted using the known SDK format."""


def require(condition, message):
    if not condition:
        raise FirmwareError(message)


def marker(data, tag):
    end = min(len(data), HEADER_SCAN)
    matches = [pos for pos in range(0, end - len(tag) + 1, 4)
               if data[pos:pos + len(tag)] == tag]
    require(len(matches) == 1, f"expected one aligned {tag!r} in first 512 bytes")
    return matches[0]


def region(base, length, label):
    require(base > 0 and length > 0 and base + length <= 1 << 32,
            f"{label}: empty or overflowing 32-bit address range")
    return base, base + length


def digest(data):
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "crc16_xmodem": f"{binascii.crc_hqx(data, 0):04x}"}


def nv_section(nv, offset, size, label, optional=False):
    if optional and offset == 0 and size == 0:
        return b""
    require(offset >= 32 and size > 0 and offset <= len(nv) and
            size <= len(nv) - offset, f"{label}: section outside NV file")
    return nv[offset:offset + size]


def prepare(iram, dram, nv):
    """Return a JSON-compatible manifest and an in-memory patched IRAM copy."""
    for name, data in (("IRAM", iram), ("DRAM", dram), ("NV", nv)):
        require(0 < len(data) <= MAX_IMAGE, f"{name}: unsupported file size")
    require(len(nv) >= 32, "NV: expected container header, not raw calibration")
    head = marker(iram, b"kees0616")
    tail = marker(iram, b"evaw0616")
    records = head + 16
    require(records < tail and (tail - records) % 12 == 0,
            "IRAM: invalid module table extent")
    ibase, dbase = struct.unpack_from("<II", iram, head + 8)
    irange = region(ibase, len(iram), "IRAM")
    drange = region(dbase, len(dram), "DRAM")
    require(irange[1] <= drange[0] or drange[1] <= irange[0],
            "IRAM and DRAM load ranges overlap")
    modules = []
    for pos in range(records, tail, 12):
        source, kind_size, destination = struct.unpack_from("<III", iram, pos)
        kind, size = kind_size >> 24, kind_size & 0xffffff
        require(kind in MODULE_NAMES, f"module at {pos:#x}: unknown type {kind}")
        source_range = region(source, size, "module source")
        region(destination, size, "module destination")
        candidates = [(name, base) for name, (base, end) in
                      (("iram", irange), ("dram", drange))
                      if base <= source_range[0] and source_range[1] <= end]
        require(len(candidates) == 1, f"module at {pos:#x}: source outside images")
        name, base = candidates[0]
        modules.append({"service": MODULE_NAMES[kind], "image": name,
                        "source_address": source, "file_offset": source - base,
                        "destination_address": destination, "size": size})

    nv_head = marker(iram, b"TSVN")
    nv_tail = marker(iram, b"DEVN")
    nv_start = nv_head + 4
    require(nv_start < nv_tail, "IRAM: reversed or empty NV slot")
    require(nv_tail + 4 <= head or nv_head >= tail + 8,
            "IRAM: NV slot overlaps firmware metadata")
    cmfg_off, cmfg_len, pnfg_off, pnfg_len = struct.unpack_from("<IIII", nv, 8)
    cmfg = nv_section(nv, cmfg_off, cmfg_len, "CMFG")
    pnfg = nv_section(nv, pnfg_off, pnfg_len, "PNFG", optional=True)
    require(len(pnfg) % 8 == 0 and len(pnfg) <= 20 * 8,
            "PNFG: expected at most twenty complete 8-byte pin records")
    require(len(cmfg) <= nv_tail - nv_start, "CMFG exceeds IRAM NV slot")
    if pnfg:
        require(cmfg_off + cmfg_len <= pnfg_off or
                pnfg_off + pnfg_len <= cmfg_off, "CMFG and PNFG overlap")
    merged = bytearray(iram)
    merged[nv_start:nv_start + len(cmfg)] = cmfg
    report = {
        "validation": "structure only; boot and authenticity are not verified",
        "images": {"iram": digest(iram), "dram": digest(dram), "nv": digest(nv)},
        "prepared_iram": digest(merged),
        "metadata": {"start": head, "end": tail, "modules": modules},
        "nv": {"slot_offset": nv_start, "slot_size": nv_tail - nv_start,
               "cmfg_size": len(cmfg), "pnfg_size": len(pnfg),
               "requires_post_ready_pin_config": bool(pnfg)},
        "download_plan": [],
        "completion": {"function": 0, "address": 0x160, "value": 1,
                       "ready_sequence": ["BSPREADY", "RDVERSION", "trunk_W"],
                       "requires_rx_transport": True},
    }
    for name, address, data in (("dram", dbase, dram), ("iram", ibase, merged)):
        report["download_plan"].append({
            "image": name, "address": address, "size": len(data),
            "function": 1, "sdio_window": 0x0f, "increment_address": True,
            "latch_once_at_start": True, "chunk_size": 512,
            "cmd53_count": (len(data) + 511) // 512,
            "last_chunk_size": (len(data) - 1) % 512 + 1})
    return report, bytes(merged)


def read_image(path):
    # Bound allocation even if the file changes after a stat call.
    with path.open("rb") as stream:
        data = stream.read(MAX_IMAGE + 1)
    require(0 < len(data) <= MAX_IMAGE, f"{path.name}: unsupported file size")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("iram", "dram", "nv"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        report, _ = prepare(read_image(args.iram), read_image(args.dram),
                            read_image(args.nv))
    except (OSError, FirmwareError) as error:
        print(f"SKW firmware rejected: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
