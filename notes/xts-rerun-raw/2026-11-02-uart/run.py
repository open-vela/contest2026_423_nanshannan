#!/usr/bin/env python3
"""xTS 1.3.11 UART 文件传输（2026-11-02）：控制台临时降到 115200 后双向 ymodem。

用法：python3 run.py <输出目录>

为什么降速：板 -> 主机方向在 1.5 Mbaud 下由主机侧 CH340 丢字（7~16%），115200 下
零丢失（见 ../2026-09-20-ymodem/README.md）。编译期改波特率不生效，这里用
`k7diag baud` 在运行时改，传完再改回 1.5M；不管中间成败，最后都改回去。

★ 改波特率的命令必须在串口控制台上敲，不能从 telnet 发：串口上的 nsh 在读一行
  之前保存整份 termios、读完写回（apps/system/readline/readline_fd.c），从别处改的
  波特率会在串口收到下一个回车时被改回去（attempt2-telnet-baud/ 就是这样失败的）。
  在串口上敲时上一行已读完，下一行开始保存的就是新波特率。telnet 只用来兜底恢复。

流程（每步原始记录都落盘）：
  串口 1.5M：  k7diag baud 115200
  串口 115200：rb -f /tmp      <- 主机 sb --ymodem <文件>
               md5_test -f /tmp/<文件> -c 1
               sb /tmp/<文件>  -> 主机 rb --ymodem（收进 recv/）
  主机：收回的文件与原文件逐字节比对
  串口 115200：k7diag baud 1500000，串口 1.5M 下确认提示符（不通则 telnet 兜底）
"""

import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

HERE = Path(__file__).resolve()
ROOT = HERE.parents[3]
PROMPT = re.compile(rb'nsh>\s*(?:\x1b\[[0-9;]*[A-Za-z])?\s*$')


def netsh(out, cmd):
    env = {**os.environ, 'K7_NETSH_OUT': str(out / '_netsh')}
    r = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'xts-netsh.py'),
                        cmd, '20'], capture_output=True, text=True, env=env,
                       timeout=60)
    return r.stdout


class Console:
    def __init__(self, baud, log):
        self.s = serial.Serial('/dev/ttyUSB0', baud, timeout=0.05,
                               exclusive=True)
        self.log = log

    def read_until_prompt(self, t):
        buf = bytearray()
        end = time.monotonic() + t
        while time.monotonic() < end:
            d = self.s.read(65536)
            if d:
                buf += d
                if PROMPT.search(buf[-200:]):
                    break
        self.log.write(bytes(buf))
        return bytes(buf)

    def type(self, cmd):
        self.s.reset_input_buffer()
        for c in cmd.encode():
            self.s.write(bytes((c,)))
            time.sleep(0.003)
        self.s.write(b'\r')
        self.log.write(f'\n>>> {cmd}\n'.encode())

    def lrzsz(self, argv, cwd, errlog):
        # pyserial 以 O_NONBLOCK 打开串口；lrzsz 继承这个描述符时每次 read 都
        # 立刻 EAGAIN，当成超时（attempt3-nonblock-fd/ 就是这样失败的）。
        # 交给它之前改回阻塞，用完再恢复。
        fd = self.s.fileno()
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
        try:
            with open(errlog, 'wb') as err:
                r = subprocess.run(argv, stdin=fd, stdout=fd, stderr=err,
                                   cwd=cwd, timeout=120)
        finally:
            fcntl.fcntl(fd, fcntl.F_SETFL, flags)
        return r.returncode

    def close(self):
        self.s.close()


def transfers(con, out, recv, files, ev):
    con.s.write(b'\r')
    ev['prompt_at_115200'] = bool(PROMPT.search(con.read_until_prompt(3)))
    if not ev['prompt_at_115200']:
        raise RuntimeError('115200 下没有 nsh 提示符')

    for name, data in files.items():
        r = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
             'md5': hashlib.md5(data).hexdigest()}

        # 主机 -> 板

        con.type('rb -f /tmp')
        time.sleep(0.5)
        r['host_sb_rc'] = con.lrzsz(['sb', '--ymodem', str(out / name)], out,
                                    out / f'{name}.host-sb.log')
        con.read_until_prompt(5)
        con.type(f'md5_test -f /tmp/{name} -c 1')
        txt = con.read_until_prompt(5)
        m = re.search(rb'md5 is ([0-9a-f]{32})', txt)
        r['board_md5'] = m.group(1).decode() if m else None

        # 板 -> 主机

        got = recv / name
        if got.exists():
            got.unlink()

        con.type(f'sb /tmp/{name}')
        time.sleep(0.3)
        r['host_rb_rc'] = con.lrzsz(['rb', '--ymodem'], recv,
                                    out / f'{name}.host-rb.log')
        con.read_until_prompt(5)
        back = got.read_bytes() if got.exists() else b''
        r['back_size'] = len(back)
        r['back_identical'] = back == data
        ev[name] = r
        print(name, json.dumps(r, ensure_ascii=False), flush=True)

        con.type(f'rm /tmp/{name}')
        con.read_until_prompt(3)


def main():
    out = Path(sys.argv[1]).resolve()
    recv = out / 'recv'
    recv.mkdir(parents=True, exist_ok=True)
    files = {
        'xts-uart-pattern.bin': bytes(range(256)) * 16,          # 与 09-19 相同
        'xts-uart-random.bin': os.urandom(65536),
    }
    for name, data in files.items():
        (out / name).write_bytes(data)

    ev = {'start': time.time()}
    try:
        with open(out / 'console-1500000-before.raw', 'wb') as log:
            con = Console(1500000, log)
            con.s.write(b'\r')
            con.read_until_prompt(3)
            con.type('k7diag baud 115200')
            time.sleep(0.5)
            con.close()
        with open(out / 'console-115200.raw', 'wb') as log:
            con = Console(115200, log)
            try:
                transfers(con, out, recv, files, ev)
                con.type('k7diag baud 1500000')
                time.sleep(0.5)
            finally:
                con.close()
    finally:
        with open(out / 'console-1500000-after.raw', 'wb') as log:
            con = Console(1500000, log)
            con.s.write(b'\r')
            ok = bool(PROMPT.search(con.read_until_prompt(3)))
            con.close()
        if not ok:
            # 兜底：从 telnet 改回；串口这边要再敲一次回车让它稳定下来
            (out / '99-telnet-restore.net.txt').write_text(
                netsh(out, 'k7diag baud 1500000'))
            with open(out / 'console-1500000-after.raw', 'ab') as log:
                con = Console(1500000, log)
                con.s.write(b'\r')
                ok = bool(PROMPT.search(con.read_until_prompt(3)))
                con.close()
        ev['prompt_back_at_1500000'] = ok
        ev['end'] = time.time()
        (out / 'events.json').write_text(json.dumps(ev, ensure_ascii=False,
                                                    indent=2))
        print(json.dumps({k: v for k, v in ev.items()
                          if not k.endswith('.bin')}, ensure_ascii=False))

if __name__ == '__main__':
    main()
