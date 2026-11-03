#!/usr/bin/env python3
"""回归重跑（2026-11-02，复制自 2026-09-20-btier/run.py）：按原文命令逐条执行，
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
    ('镜像',   'uname -a', 20),
    ('1.1.2',  'cmocka_sched_test', 120),
    ('1.1.3',  'cmocka_syscall_test', 180),
    ('1.1.5',  'getprime', 60),
    ('1.1.6',  'mm', 180),
    ('1.1.7',  'scanftest', 120),
    ('1.1.8',  'hello', 30),
    ('1.1.9',  'helloxx', 30),
    ('1.1.10', 'popen', 60),
    ('1.1.11', 'pipe', 90),
    ('1.1.12', 'echo xts-md5-test-file > /tmp/1.txt', 20),
    ('1.1.12', 'md5_test -f /tmp/1.txt -c 100', 120),
    ('1.1.13', 'cxxtest', 90),
    ('1.2.3',  'free', 30),
    ('1.2.3',  'ampctl exec free', 40),          # 原文：多核需每个核都输入
    ('1.2.4',  'df -h', 30),
    ('1.2.4',  'ampctl exec "df -h"', 40),
    ('1.3.2',  'fstest -n 10 -m /tmp', 600),
    ('1.3.3',  'ramtest -w -s {RAMTEST_SIZE}', 600),
    ('1.3.3',  'ramtest -h -s {RAMTEST_SIZE}', 600),
    ('1.3.3',  'ramtest -b -s {RAMTEST_SIZE}', 900),
    ('1.3.4',  'mkrd -m 10 -s 1000 1024', 30),   # RAM 盘，不碰 eMMC
    ('1.3.4',  'cmocka_driver_block -m /dev/ram10', 300),
    ('1.3.6',  'cmocka_driver_gpio -i /dev/gpio3 -o /dev/gpio2', 120),  # 40 针 5/7 脚短接
    ('1.3.7',  'cmocka_driver_i2c_spi', 120),    # I2C7 接 MPU-6050；不跑 spidev/i2cdev master
    ('1.3.10', 'cmocka_driver_uart -d /dev/ttyS0', 120),
    ('1.3.13', 'cmocka_driver_oneshot -d /dev/oneshot', 180),
    ('1.3.17', 'cmocka_des3cbc', 90),
    ('1.3.17', 'cmocka_aescbc', 90),
    ('1.3.17', 'cmocka_aesctr', 90),
    ('1.3.17', 'cmocka_aesxts', 90),
    ('1.3.17', 'cmocka_hmac', 90),
    ('1.3.17', 'cmocka_hash', 90),
    ('1.3.17', 'cmocka_crc32', 90),
    ('1.3.17', 'cmocka_ecdsa', 120),
    ('1.2.3',  'free', 30),                      # 跑完一轮后再取一次
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
