#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Update only the K7 AMP FIT region after backing it up and verifying reads.
#
# 布局（LBA、上限）来自 amp/layout.sh。写之前必须过 amp_check_write，
# 保证不会压到 dtb / U-Boot / Linux 内核。

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../amp/layout.sh
source "$root/amp/layout.sh"

rkdev=${RKDEV:-/home/dministrator/rkdeveloptool/rkdeveloptool}
fit=${K7_FIT:-${K7_NETSH_FIT:-/tmp/k7-netsh-fit/amp.itb}}
maskrom_loader=${K7_MASKROM_LOADER:-/home/dministrator/rk3576-uboot/u-boot/rk3576_spl_loader_v1.09.108.bin}
fit_lba=$AMP_FIT_LBA
region_sectors=$AMP_FIT_MAX

[[ -f "$fit" ]] || { echo "missing FIT: $fit" >&2; exit 1; }
fit_bytes=$(stat -c %s "$fit")
(( fit_bytes > 0 && fit_bytes % 512 == 0 )) || {
  echo "FIT must have a whole number of 512-byte sectors" >&2
  exit 1
}
fit_sectors=$(( fit_bytes / 512 ))
amp_check_write "$fit_lba" "$fit_sectors" fit
python3 - "$fit" <<'PY'
from pathlib import Path
import sys
if Path(sys.argv[1]).read_bytes()[:4] != bytes.fromhex('d00dfeed'):
    raise SystemExit('new image has no FIT header; refusing write')
PY

echo "FIT: $fit"
echo "FIT bytes: $fit_bytes; LBA $fit_lba..$(( fit_lba + fit_sectors - 1 ))" \
     "（区域上限 $region_sectors 扇区，余 $(( region_sectors - fit_sectors ))）"
sha256sum "$fit"

if [[ ${1:-} == --prepare ]]; then
  exit 0
fi

[[ -x "$rkdev" ]] || { echo "missing rkdeveloptool: $rkdev" >&2; exit 1; }
state=$("$rkdev" ld)
# ★ db 下载的引导器跑起来以后，ld 仍然报 Maskrom（USB 描述符没变），
#   这时再 db 会失败。先用 rfi 探一下：能读出 Flash 信息就说明引导器
#   已经在跑，可以直接读写。
# （不能写成 rfi | grep -q：pipefail 下 grep 提前退出会让管道判失败）
loader_alive() { local o; o=$("$rkdev" rfi 2>/dev/null || true); [[ "$o" == *"Flash Size"* ]]; }
if [[ "$state" == *Maskrom* ]] && loader_alive; then
  state="Loader (downloaded; ld still says maskrom)"
fi
if [[ "$state" == *Maskrom* ]]; then
  [[ -f "$maskrom_loader" ]] || { echo "missing MASKROM loader" >&2; exit 1; }
  "$rkdev" db "$maskrom_loader"
  # db 之后设备以 Loader 身份重新枚举；WSL 下 usbipd 的绑定随之失效，
  # 要重新 attach，否则 ld 报 not found。
  usbipd=${USBIPD:-/mnt/c/Program Files/usbipd-win/usbipd.exe}
  state=""
  for _ in $(seq 1 20); do
    sleep 1
    if [[ -x "$usbipd" ]]; then
      busid=$("$usbipd" list 2>/dev/null | tr -d '\r' |
              awk '/2207:/ && !/Attached/ {print $1; exit}')
      [[ -n "$busid" ]] && "$usbipd" attach --wsl --busid "$busid" >/dev/null 2>&1 && sleep 2
    fi
    if loader_alive; then
      state="Loader (downloaded)"
      break
    fi
  done
fi
[[ "$state" == *Loader* ]] || {
  echo "RockUSB Loader not available: $state" >&2
  exit 1
}

run_dir="/tmp/k7-amp-fit/flash-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$run_dir"
backup="$run_dir/lba${fit_lba}-${region_sectors}-before.bin"
readback="$run_dir/lba${fit_lba}-new-readback.bin"
rollback_readback="$run_dir/lba${fit_lba}-${region_sectors}-rollback.bin"

echo "Reading current AMP FIT region to $backup"
"$rkdev" rl "$fit_lba" "$region_sectors" "$backup" >/dev/null
[[ $(stat -c %s "$backup") -eq $(( region_sectors * 512 )) ]] || {
  echo "backup has wrong length; refusing write" >&2
  exit 1
}
# 原内容必须是 FIT，或者是空白（首次部署到 trust 分区时）。
# 其他内容说明这块被别的东西占着，需要显式 K7_ALLOW_OVERWRITE=1。
python3 - "$backup" "${K7_ALLOW_OVERWRITE:-${K7_ALLOW_INITIAL_SECURITY:-0}}" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
if data[:4] == bytes.fromhex('d00dfeed'):
    print('current FIT header verified')
elif len(set(data)) == 1:
    print(f'region is blank (all 0x{data[0]:02x})')
elif sys.argv[2] == '1':
    print(f'unknown content {data[:4]!r} explicitly allowed')
else:
    raise SystemExit(f'region holds unknown content {data[:4]!r}; '
                     'set K7_ALLOW_OVERWRITE=1 after checking the backup')
PY
sha256sum "$backup"

rollback() {
  echo "New FIT verification failed; restoring backed-up region" >&2
  "$rkdev" wl "$fit_lba" "$backup"
  "$rkdev" rl "$fit_lba" "$region_sectors" "$rollback_readback" >/dev/null
  cmp "$backup" "$rollback_readback"
  echo "Backed-up AMP region restored and verified" >&2
}

echo "Writing new FIT"
if ! "$rkdev" wl "$fit_lba" "$fit" >/dev/null; then
  rollback
  exit 1
fi
echo "Reading back new FIT"
if ! "$rkdev" rl "$fit_lba" "$fit_sectors" "$readback" >/dev/null ||
   ! cmp "$fit" "$readback"; then
  rollback
  exit 1
fi
sha256sum "$readback"
echo "FIT write verified. Current region backup: $backup"

# ★ 清 A/B 控制块（amp/include/k7_bootab.h）。
#   刚写的是救援 FIT；控制块若还指着 OTA 装过的槽，U-Boot 会启动那个槽，
#   这次烧写等于没生效。清零后两份都无效 = 没有 A/B，U-Boot 启动救援 FIT。
#   K7_KEEP_ABCTL=1 保留（只想更新救援 FIT、继续跑槽里的镜像时）。
if [[ ${K7_KEEP_ABCTL:-0} != 1 ]]; then
  amp_check_write "$AMP_ABCTL_LBA" "$AMP_ABCTL_CNT" abctl
  abctl_backup="$run_dir/lba${AMP_ABCTL_LBA}-abctl-before.bin"
  abctl_zero="$run_dir/abctl-zero.bin"
  "$rkdev" rl "$AMP_ABCTL_LBA" "$AMP_ABCTL_CNT" "$abctl_backup" >/dev/null
  head -c $(( AMP_ABCTL_CNT * 512 )) /dev/zero > "$abctl_zero"
  if cmp -s "$abctl_backup" "$abctl_zero"; then
    echo "A/B control block already clear"
  else
    "$rkdev" wl "$AMP_ABCTL_LBA" "$abctl_zero" >/dev/null
    "$rkdev" rl "$AMP_ABCTL_LBA" "$AMP_ABCTL_CNT" "$run_dir/abctl-readback.bin" >/dev/null
    cmp "$abctl_zero" "$run_dir/abctl-readback.bin"
    echo "A/B control block cleared (backup: $abctl_backup)"
  fi
fi
echo "Board remains in Loader mode; reset and bootamp only after review."
