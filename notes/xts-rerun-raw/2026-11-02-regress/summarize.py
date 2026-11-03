"""把 run.py 的两份原始记录汇总成判读表（只提取，不改判据）。"""
import json, re
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAT = [
    (re.compile(rb'\[\s*PASSED\s*\]\s*(\d+) test\(s\)'), 'PASSED {0} 项'),
    (re.compile(rb'\[\s*FAILED\s*\]\s*(\d+) test\(s\)'), '★FAILED {0} 项'),
    (re.compile(rb'(\d+) test\(s\) run'), '共 {0} 项'),
    (re.compile(rb'OK:\s*(\d+),?\s*FAILED:\s*(\d+)', re.I), 'OK {0} / FAILED {1}'),
    (re.compile(rb'TEST COMPLETE'), 'TEST COMPLETE'),
    (re.compile(rb'user_main: Exiting'), 'ostest 正常退出'),
    (re.compile(rb'Hello, World!!'), 'Hello, World!!'),
]

for ev in [json.loads(l) for l in (HERE / 'events.jsonl').open()]:
    base = HERE / f"{ev['case']}-{''.join(c if c.isalnum() else '-' for c in ev['cmd'])[:40].strip('-')}"
    blob = b''
    for suf in ('.serial.raw', '.net.txt'):
        f = Path(f'{base}{suf}')
        if f.exists():
            blob += f.read_bytes()
    hits = []
    for pat, fmt in PAT:
        m = pat.search(blob)
        if m:
            hits.append(fmt.format(*[g.decode() for g in m.groups()]))
    bad = len(re.findall(rb'ERROR|FAILED|Assertion', blob))
    print(f"{ev['case']:<7}{ev['cmd']:<38}{ev['elapsed_s']:>7.1f}s  "
          f"{' / '.join(hits) if hits else '（无结果关键字）':<28} 疑似异常行 {bad}")
