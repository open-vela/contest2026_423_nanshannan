#!/usr/bin/env python3
"""xTS 1.3.5 Flash 功能（2026-11-02，复制自 2026-11-02-regress/run.py）：按原文命令逐条执行，
同时留网络与串口两份原始记录。ramtest 的 -s 取环境变量 RAMTEST_SIZE（free 的
maxfree − 64KB，见 2026-09-19-ramtest-final）。

用法：python3 run.py <输出目录> [只跑某几个用例号，空格分隔]

为什么两份记录：cmocka 系列把结果打到 syslog（串口），普通命令的输出回到
telnet。只留一份都会缺东西——09-19 的 RTC 复测就是因为只看 telnet 而看不到
结果。这里对每条用例同时抓：
  <用例>-<命令>.net.txt     telnet 会话（含命令回显与直接输出）
  <用例>-<命令>.serial.raw  执行期间的串口原始字节
  events.jsonl              每条的命令、耗时、两侧字节数

不做判定，只取证；判定在 README 里人工写，保持与其它目录一致。
"""

import importlib.util
import json
import sys
import threading
import time
from pathlib import Path

import serial

HERE = Path(__file__).resolve()
ROOT = HERE.parents[3]
spec = importlib.util.spec_from_file_location('netsh', ROOT / 'scripts' / 'xts-netsh.py')
netsh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(netsh)

# (用例号, 命令, 超时秒)；命令照 docs/refs/openvela_xts_test_cases.md 原文
CASES = [
    ('镜像',  'uname -a', 20),
    ('1.3.5', 'k7diag sdpart', 30),                       # SD 卡末尾 64MB -> /dev/sdtest
    ('1.3.5', 'ls -l /dev/sdtest', 20),
    ('1.3.5', 'cmocka_driver_block -m /dev/sdtest', 3600),
]


class SerialTap:
    """执行期间独占串口收字节。"""

    def __init__(self, path):
        self.path = path
        self.buf = bytearray()
        self.run = True
        self.t = threading.Thread(target=self._loop, daemon=True)

    def _loop(self):
        with serial.Serial('/dev/ttyUSB0', 1500000, timeout=0.05,
                           exclusive=True) as s:
            while self.run:
                self.buf += s.read(65536)

    def __enter__(self):
        self.t.start()
        time.sleep(0.6)
        return self

    def __exit__(self, *a):
        time.sleep(0.8)
        self.run = False
        self.t.join(timeout=5)
        Path(self.path).write_bytes(bytes(self.buf))


def slug(cmd):
    return ''.join(c if c.isalnum() else '-' for c in cmd)[:40].strip('-')


def main():
    out = Path(sys.argv[1])
    only = set(sys.argv[2:])
    out.mkdir(parents=True, exist_ok=True)
    for case, cmd, timeout in CASES:
        cmd = cmd.replace('{RAMTEST_SIZE}', __import__('os').environ.get('RAMTEST_SIZE', '0'))
        if only and case not in only:
            continue
        base = out / f'{case}-{slug(cmd)}'
        t0 = time.monotonic()
        with SerialTap(f'{base}.serial.raw') as tap:
            import subprocess
            env = {**__import__('os').environ, 'K7_NETSH_OUT': str(out / '_netsh')}
            r = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'xts-netsh.py'),
                                cmd, str(timeout)], capture_output=True, text=True,
                               env=env, timeout=timeout + 60)
        el = time.monotonic() - t0
        Path(f'{base}.net.txt').write_text(r.stdout)
        ev = {'case': case, 'cmd': cmd, 'elapsed_s': round(el, 1),
              'net_bytes': len(r.stdout), 'serial_bytes': Path(f'{base}.serial.raw').stat().st_size,
              'rc': r.returncode}
        with (out / 'events.jsonl').open('a') as f:
            f.write(json.dumps(ev, ensure_ascii=False) + '\n')
        print(json.dumps(ev, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
