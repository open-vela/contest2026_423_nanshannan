#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile real SDIO helper code and selected host functions against mock MMIO.

Only platform headers and hardware access are replaced. No driver algorithm is
reimplemented. Generated files and binaries stay in a temporary directory.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
WORKSPACE = REPO.parent
HOST = REPO / "chip/rk3576/rk3576_dwmmc.c"


def function(source, name):
    # Keep the complete production function body, without a second algorithm.
    match = re.search(r"^static [^;{}]*\b" + name + r"\([^;{}]*\)\n\{", source, re.M)
    if not match:
        raise ValueError(name)
    start = match.start()
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true",
                        help="Test HEAD before local fixes; expected to fail")
    args = parser.parse_args()

    def read_source(repo, path):
        if args.baseline:
            return subprocess.check_output(
                ["git", "show", "HEAD:" + path], cwd=repo, text=True)
        return (repo / path).read_text()

    source = read_source(REPO, "chip/rk3576/rk3576_dwmmc.c")
    sdio = read_source(WORKSPACE / "nuttx", "drivers/mmcsd/sdio.c")
    skw = read_source(REPO, "chip/rk3576/skw6621s/skw6621s_sdio.c")
    functions = [
        "dw_set_clock_hz", "rk3576_dwmmc_status",
        "rk3576_dwmmc_sendcmd", "rk3576_dwmmc_waitresponse",
        "rk3576_dwmmc_recvshort", "rk3576_dwmmc_blocksetup",
        "rk3576_dwmmc_recvsetup", "rk3576_dwmmc_sendsetup",
        "rk3576_dwmmc_cancel", "dw_pio_transfer",
        "rk3576_dwmmc_waitenable", "rk3576_dwmmc_eventwait",
    ]
    hardware = source[source.index("#define DWMMC_GATE_CON"):
                      source.index("#define RESET_TIMEOUT_US")]
    start = source.index("struct rk3576_dwmmc_dev_s\n")
    state = source[start:source.index("\n};", start) + 3]
    with tempfile.TemporaryDirectory(prefix="k7-sdio-") as tmp:
        tmp = Path(tmp)
        for header in ["debug.h", "nuttx/config.h", "nuttx/compiler.h",
                       "nuttx/arch.h", "nuttx/sdio.h", "nuttx/signal.h",
                       "nuttx/clock.h"]:
            target = tmp / header
            target.parent.mkdir(parents=True, exist_ok=True)
            target.touch()
        (tmp / "host.inc").write_text(
            hardware + "\n" + state + "\n" +
            "\n".join(function(source, name) for name in functions))
        (tmp / "sdio.inc").write_text(sdio)
        (tmp / "skw.inc").write_text(
            "\n".join(re.findall(r"^#define .*$", skw, re.M)) + "\n" +
            function(skw, "skw_write_f0") + "\n" +
            function(skw, "skw_read_identity"))
        for dma in (False, True):
            binary = tmp / ("test-dma" if dma else "test-pio")
            command = [os.environ.get("CC", "cc"), "-std=c11", "-g",
                       "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                       "-fsanitize=undefined", "-fno-sanitize-recover=all",
                       "-I", str(tmp), "-I", str(WORKSPACE),
                       "-I", str(REPO / "chip/rk3576"),
                       str(HERE / "test.c"), "-o", str(binary)]
            if dma:
                command.insert(1, "-DCONFIG_SDIO_DMA=1")
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
