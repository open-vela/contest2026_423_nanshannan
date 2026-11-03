#!/usr/bin/env python3
"""主机侧 SNTP：量出主机时钟相对公网 NTP 的偏差，给 capture.py 当基准。

WSL 的墙钟会跳（watchdog 那次回跳过 46 s；11-03 试跑时一次请求内跳了约 50 s），
不能拿 time.time() 当"PC 时间"，也不能拿它量往返。这里全部用 time.monotonic()：
  真实时间 = time.monotonic() + K，K 每次取样重新量。
往返为负或超过 0.5 s 的回复说明请求期间时钟不稳，直接丢弃。

  python3 sntp.py        # 打印各服务器的 offset / delay
"""

import socket
import struct
import time

SERVERS = ["ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org"]
NTP_EPOCH = 2208988800                    # 1900-01-01 → 1970-01-01


def _ts(b):
    sec, frac = struct.unpack("!II", b)
    return sec - NTP_EPOCH + frac / 2 ** 32


def query(server, timeout=2.0, clock=time.monotonic):
    """返回 (offset, delay)：offset = 服务器时间 − clock()。"""
    pkt = bytearray(48)
    pkt[0] = 0x23                          # LI=0 VN=4 Mode=3（客户端）
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        t1 = clock()
        s.sendto(pkt, (server, 123))
        data, _ = s.recvfrom(512)
        t4 = clock()
    if len(data) < 48 or (data[0] & 0x07) != 4 or data[1] == 0:
        raise RuntimeError("bad reply from %s" % server)
    t2, t3 = _ts(data[32:40]), _ts(data[40:48])
    return ((t2 - t1) + (t3 - t4)) / 2, (t4 - t1) - (t3 - t2)


def best(tries=3, clock=time.monotonic):
    """几台服务器各问几次，取往返最短的一次。全失败返回 None。"""
    got = []
    for srv in SERVERS:
        for _ in range(tries):
            try:
                off, dly = query(srv, clock=clock)
                if 0 <= dly <= 0.5:
                    got.append((dly, off, srv))
            except Exception:
                pass
    if not got:
        return None
    dly, off, srv = min(got)
    return {"offset": off, "delay": dly, "server": srv, "n": len(got)}


if __name__ == "__main__":
    for srv in SERVERS:
        try:
            print(srv, "wall offset %+.3f s  delay %.3f s" % query(srv, clock=time.time))
        except Exception as e:
            print(srv, "fail", e)
    print(best())
