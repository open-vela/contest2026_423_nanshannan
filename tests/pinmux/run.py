#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check production RK3576 pull writes against Linux/K7 register vectors."""
import os
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
CHIP = REPO / "chip/rk3576"

TEST = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "rk3576_pinmux.h"

static uintptr_t last_addr;
static uint32_t last_value;
static unsigned int writes;

void putreg32(uint32_t value, uintptr_t addr)
{
  last_addr = addr;
  last_value = value;
  writes++;
}

uint32_t getreg32(uintptr_t addr)
{
  assert(addr == last_addr);
  return last_value & 0xffff;
}

int main(void)
{
  /* Register vectors are independent of the driver's offset calculation.
   * Cover SDIO plus GPIO0/GPIO4 split register regions.
   */
  static const struct
  {
    int bank;
    int pin;
    uintptr_t addr;
    uint32_t up;
    uint32_t down;
  } cases[] =
  {
    {1, 12, 0x26046114, 0x03000300, 0x03000100},
    {1, 15, 0x26046114, 0xc000c000, 0xc0004000},
    {1, 16, 0x26046118, 0x00030003, 0x00030001},
    {1, 17, 0x26046118, 0x000c000c, 0x000c0004},
    {0, 11, 0x26040024, 0x00c000c0, 0x00c00040},
    {0, 12, 0x26042028, 0x03000300, 0x03000100},
    {2,  0, 0x26046120, 0x00030003, 0x00030001},
    {3,  0, 0x26046130, 0x00030003, 0x00030001},
    {4, 15, 0x26046144, 0xc000c000, 0xc0004000},
    {4, 16, 0x2604a148, 0x00030003, 0x00030001},
    {4, 24, 0x2604b14c, 0x00030003, 0x00030001}
  };
  unsigned int i;
  unsigned int before;

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
      assert(rk3576_pinmux_setpull(cases[i].bank, cases[i].pin,
                                 RK3576_PULL_UP) == 0);
      assert(last_addr == cases[i].addr && last_value == cases[i].up);
      assert(rk3576_pinmux_getpull(cases[i].bank, cases[i].pin) == 3);
      assert(rk3576_pinmux_setpull(cases[i].bank, cases[i].pin,
                                 RK3576_PULL_DOWN) == 0);
      assert(last_value == cases[i].down);
      assert(rk3576_pinmux_getpull(cases[i].bank, cases[i].pin) == 1);
      assert(rk3576_pinmux_setpull(cases[i].bank, cases[i].pin,
                                 RK3576_PULL_NONE) == 0);
      assert(last_value == (cases[i].up & 0xffff0000));
    }

  before = writes;
  assert(rk3576_pinmux_setpull(-1, 0, RK3576_PULL_UP) < 0);
  assert(rk3576_pinmux_setpull(5, 0, RK3576_PULL_UP) < 0);
  assert(rk3576_pinmux_setpull(1, 32, RK3576_PULL_UP) < 0);
  assert(rk3576_pinmux_setpull(1, 12, 2) < 0);
  assert(rk3576_pinmux_setpull(1, 12, 4) < 0);
  assert(writes == before);
  puts("PASS: RK3576 pulls, split banks, write masks and invalid arguments");
  return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="k7-pinmux-") as directory:
        tmp = Path(directory)
        for name in ("nuttx/config.h", "arch/chip/chip.h"):
            target = tmp / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("")
        (tmp / "arm64_internal.h").write_text(
            "#include <stdint.h>\n#define OK 0\n"
            "void putreg32(uint32_t, uintptr_t);\n"
            "uint32_t getreg32(uintptr_t);\n")
        (tmp / "test.c").write_text(TEST)
        binary = tmp / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
            "-Werror", "-fsanitize=undefined", "-fno-sanitize-recover=all",
            "-I", str(tmp), "-I", str(CHIP),
            str(CHIP / "rk3576_pinmux.c"), str(tmp / "test.c"),
            "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
