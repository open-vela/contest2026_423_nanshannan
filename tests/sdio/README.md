# SDIO host regression

Run from the workspace root:

```sh
python3 contest2026_423_nanshannan/tests/sdio/run.py
```

Requires Python 3 and a native C compiler with UndefinedBehaviorSanitizer.
The runner compiles the complete live `nuttx/drivers/mmcsd/sdio.c`, selected
complete functions from `rk3576_dwmmc.c`, and the SKW address-latch/identity
functions. It replaces platform headers, MMIO and clocks with deterministic
mocks. There is no second implementation of the driver algorithms.
Temporary includes and executables are removed automatically.

Both `CONFIG_SDIO_DMA` off and on are compiled with warnings as errors and UBSan.
Coverage includes:

- PIO fallback even when DMA is compiled in but the host lacks DMA capability.
- DMA setup selection when the host advertises it (mock DMA, not real hardware).
- CMD53 read/write lengths 1, 2, 3, 4, 5, 15, 16, 255, 511 and 512 with unaligned
  buffers, boundary sentinels and zero-filled transmit padding.
- CMD_DONE consumption, command timeout before FIFO access, and ordinary SD
  transfers whose command response has already been consumed.
- All five R5 error flags surviving a subsequent successful CMD52 response.
- DATA_OVER before FIFO drain; simultaneous DATA_OVER/CRC or timeout error.
- Stalled FIFO respecting one PIO deadline; setup/send/lock error propagation.
- CMD52 send/lock failures not being hidden by an earlier cached response.
- SDIO/SDMMC0 CRU instance selection and nonremovable SDIO card status.
- CMD5 delayed READY, never-ready timeout, invalid OCR and response error.
- Invalid CMD52/CMD53 arguments rejected before touching the bus.
- SKW identity address latch, function 1, incrementing address 0x0f and 16 bytes.

`run.py --baseline` builds the repositories' HEAD versions instead of local
changes. With the original 2026-09-20 baseline it fails the first CMD53 transfer
assertion. This option is intended to demonstrate the regression and returns a
nonzero exit status; it will cease to be a failing baseline after fixes are
committed. Use `ulimit -c 0` before this optional run to disable core dumps.

The mock clock is logical, not calibrated hardware time. These tests do not
establish electrical timing, actual DMA operation, real SD-card stability,
firmware boot, HCI behavior or WiFi connectivity. The production PIO path still
polls under a deadline; IRQ/DMA support remains future work. Its deadline covers
FIFO movement and DATA_OVER, not the preceding command-response wait.
