#!/usr/bin/env python3
"""xTS 1.3.14 时间一致性：关掉板上 NTP 后设时，静置 24h，按 6/12/18/24h 比对。

用法：python3 capture.py <输出目录> [总时长秒，默认 24h+10min]

接 09-20 的 capture.py（那一轮时间不够、中途被别的用例改过钟，作废）。改了四处：

1. **基准时间用公网 NTP，不用主机墙钟**：WSL 墙钟会跳（试跑时一次请求内跳了约
   50 s）。所有计时都用 time.monotonic()，基准 = monotonic + K，K 每次取样
   用 sntp.py 重新量；同时记下主机墙钟相对 NTP 的偏差，只作参考。
2. **设时卡在整秒上发**：`date -s` 只收整秒、纳秒清零。命令在基准时间正好走到
   那一秒时发出，起点误差是毫秒级，而不是 time.gmtime() 截断带来的最多 1 s。
3. **每次取样同时查 ps 与 /tmp 标记文件**：确认 NTP_daemon 没有回来、板子没有重启
   （一重启界面就会重新起 NTP，整轮作废）。
4. 检查点按基准时间算，不按 time.monotonic()（WSL 休眠时它会停）。

取样走网络 NSH（192.168.1.50:2323），不走 minicom 时间戳，理由同 09-20：
上一轮串口长测在约 15h 断连。
"""

import importlib.util
import json
import math
import re
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


netsh = _load("netsh", ROOT / "scripts" / "xts-netsh.py")
sntp = _load("sntp", HERE / "sntp.py")

OUT = Path(sys.argv[1])
TOTAL = float(sys.argv[2]) if len(sys.argv) > 2 else 24 * 3600 + 600
STEP = 600.0
CHECKPOINTS = [6 * 3600, 12 * 3600, 18 * 3600, 24 * 3600]
OUT.mkdir(parents=True, exist_ok=True)


def log(ev):
    with (OUT / "events.jsonl").open("a") as f:
        f.write(json.dumps(ev, ensure_ascii=False) + "\n")
    print(json.dumps(ev, ensure_ascii=False), flush=True)


MONO = time.monotonic


def ref_offset():
    """K = NTP 时间 − monotonic（s）。量不到就沿用上一次，并标出来。"""
    b = sntp.best()
    if b:
        ref_offset.last = b
        return b["offset"], b
    return ref_offset.last["offset"] if ref_offset.last else 0.0, None


ref_offset.last = None


def session():
    c = socket.create_connection((netsh.HOST, netsh.PORT), timeout=8)
    c.settimeout(0.1)
    filt = netsh.TelnetFilter()
    raw = open(OUT / "session.raw", "ab")
    greet = netsh.receive(c, filt, raw, 10)
    if not netsh.PROMPT.search(greet[-400:]):
        raise RuntimeError("no prompt")
    return c, filt, raw


def run(cmd, timeout=20, at=None):
    """跑一条命令。at = monotonic 的发送时刻（先连好再等）。返回的时刻也是 monotonic。"""
    c, filt, raw = session()
    try:
        if at is not None:
            while MONO() < at - 0.02:
                time.sleep(0.01)
            while MONO() < at:
                pass
        t0 = MONO()
        c.sendall(cmd.encode() + b"\r\n")
        out = netsh.receive(c, filt, raw, timeout)
        return out.decode("utf-8", "replace"), t0, MONO()
    finally:
        c.close()
        raw.close()


def board_epoch(text):
    m = re.findall(r"\b(1[0-9]{9})\b", text)
    return int(m[-1]) if m else None


def edge(off):
    """轮询捕捉板子秒数跳变，偏差 = 板子 − NTP 基准。"""
    prev = None
    for _ in range(40):
        text, t0, t1 = run("date -u +%s")
        cur = board_epoch(text)
        if cur is None:
            time.sleep(0.2)
            continue
        if prev is not None and cur != prev[0]:
            ref_at_tick = (prev[1] + t0) / 2 + off
            return {"mode": "edge", "board_epoch": cur, "ref_at_board_tick": round(ref_at_tick, 3),
                    "offset_s": round(cur - ref_at_tick, 3), "edge_window_s": round(t0 - prev[1], 3)}
        prev = (cur, t1)
        time.sleep(0.25)
    return {"mode": "coarse", "board_epoch": prev[0] if prev else None,
            "offset_s": round(prev[0] - (prev[1] + off), 3) if prev else None}


MARK = "/tmp/xts1314.mark"


def health():
    """NTP_daemon 是否还在、板子有没有重启过。

    /proc/uptime 在这版固件上读出来是溢出的大数，不能用；改在 tmpfs 上放一个
    标记文件：重启后 /tmp 是空的，标记就没了（重启同时也会让界面重新起 NTP）。
    """
    ps, _, _ = run("ps", 15)
    mk, _, _ = run("cat " + MARK, 10)
    return {"ntp_daemon": "NTP_daemon" in ps, "mark_present": "xts1314" in mk.split(MARK, 1)[-1]}


def wall_minus_ntp():
    """主机墙钟相对 NTP 的偏差，只作记录（看 WSL 有没有跳）。"""
    w = sntp.best(1, clock=time.time)
    return round(-w["offset"], 3) if w else None


def sample(tag):
    off, b = ref_offset()
    ev = {"tag": tag, "ntp_server": b["server"] if b else "stale",
          "ntp_delay_s": round(b["delay"], 3) if b else None, "wall_minus_ntp_s": wall_minus_ntp()}
    ev.update(edge(off))
    ev.update(health())
    ev["ref_epoch"] = round(MONO() + off, 3)
    return ev


# 1) 设时：基准时间走到整秒 S 时发 date -u -s "S"
off, b = ref_offset()
assert b, "NTP 基准量不到，不开始"
S = math.floor(MONO() + off) + 3
stamp = time.strftime("%b %d %H:%M:%S %Y", time.gmtime(S))
text, t0, t1 = run('date -u -s "%s"' % stamp, at=S - off)
log({"tag": "set", "cmd": 'date -u -s "%s"' % stamp, "target_epoch": S,
     "sent_ref": round(t0 + off, 4), "reply_ms": round((t1 - t0) * 1000, 1),
     "ntp_server": b["server"], "ntp_delay_s": round(b["delay"], 3), "reply": text.strip()[-120:]})
start_ref = S
run('echo xts1314 > %s' % MARK, 10)
log(sample("t0"))

# 2) 静置采样
done = set()
while True:
    now_ref = MONO() + ref_offset.last["offset"]
    el = now_ref - start_ref
    if el >= TOTAL:
        break
    time.sleep(max(1.0, STEP - (el % STEP)))
    el = MONO() + ref_offset.last["offset"] - start_ref
    tag = "%.1fh" % (el / 3600)
    for cp in CHECKPOINTS:
        if cp not in done and el >= cp:
            tag = "CHECK-%dh" % (cp // 3600)
            done.add(cp)
    try:
        ev = sample(tag)
        ev["elapsed_h"] = round(el / 3600, 3)
        log(ev)
    except Exception as exc:                 # 网络抖动不终止长测
        log({"tag": tag, "error": str(exc), "elapsed_h": round(el / 3600, 3)})

log({"tag": "end", "elapsed_h": round((MONO() + ref_offset.last["offset"] - start_ref) / 3600, 3)})
