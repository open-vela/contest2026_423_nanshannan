#!/usr/bin/env python3
"""灵感速记联调（2026-11-02）：经调试入口 /tmp/k7-memo 记两条，等后台打标签，
核对 /data/memo/memo.jsonl；截图；重启后确认还在。

用法：python3 run.py <输出目录> [--after-reboot]
"""
import json, os, re, subprocess, sys, time
from pathlib import Path

OUT = Path(sys.argv[1]).resolve(); OUT.mkdir(parents=True, exist_ok=True)
ROOT = Path(__file__).resolve().parents[2]
PULL = Path('/tmp/claude-1000/-home-dministrator-openvela-amlogic-src/3069be32-7076-4e98-bf4e-8cf84b8fc52e/scratchpad/acoustic/pull.py')
t0 = time.monotonic()
log_f = open(OUT / 'steps.log', 'a')

def log(s):
    line = f'{time.monotonic() - t0:7.2f} {s}'
    print(line, flush=True); log_f.write(line + '\n')

def netsh(cmd, wait='15'):
    env = {**os.environ, 'K7_NETSH_OUT': str(OUT / '_netsh')}
    r = subprocess.run([sys.executable, str(ROOT / 'scripts/xts-netsh.py'), cmd, wait],
                       capture_output=True, text=True, env=env, timeout=90)
    meta = json.loads(r.stdout.strip().splitlines()[-1])
    return Path(meta['text']).read_text(errors='replace')

def memos():
    txt = netsh('cat /data/memo/memo.jsonl')
    out = []
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try: out.append(json.loads(line))
            except ValueError: pass
    return out

def shot(name):
    netsh('echo 2 > /tmp/k7-shot'); time.sleep(2.5)
    subprocess.run([sys.executable, str(PULL), '/tmp/k7-shot.jpg', str(OUT / name)], capture_output=True)
    log(f'截图 {name}')

checks = []
def check(name, ok, detail=''):
    checks.append({'name': name, 'ok': bool(ok), 'detail': str(detail)[:600]})
    log(('PASS ' if ok else 'FAIL ') + name + ('' if ok else f'  {detail}'))

SAMPLES = ['下期视频讲一下 K7 开发板的双系统是怎么分工的',
           '明天下午三点给王总回电话，确认第二批样品的报价']

if '--after-reboot' not in sys.argv:
    before = memos(); log(f'已有 {len(before)} 条')
    for s in SAMPLES:
        netsh(f'echo {s} > /tmp/k7-memo'); log(f'记一条：{s}')
        time.sleep(1)
    got = []
    for _ in range(30):                       # 打标签最多等 90 秒
        time.sleep(3)
        got = [m for m in memos() if m['text'] in SAMPLES]
        if len(got) == 2 and all(m['state'] != 0 for m in got):
            break
    log('memo.jsonl 里的这两条：\n' + '\n'.join(json.dumps(m, ensure_ascii=False) for m in got))
    check('两条都存进 /data/memo/memo.jsonl', len(got) == 2, got)
    check('两条都打上标签（state=1）', len(got) == 2 and all(m['state'] == 1 for m in got), got)
    types = {m['text']: m['type'] for m in got}
    check('第一条归"选题"', types.get(SAMPLES[0]) == '选题', types)
    check('第二条归"待办"或"客户"', types.get(SAMPLES[1]) in ('待办', '客户'), types)
    check('标题、标签非空', all(m['title'] and m['tags'] for m in got), got)
    shot('shot-list.jpg')
else:
    got = [m for m in memos() if m['text'] in SAMPLES]
    log('重启后：\n' + '\n'.join(json.dumps(m, ensure_ascii=False) for m in got))
    check('重启后两条还在、标签还在', len(got) == 2 and all(m['state'] == 1 for m in got), got)
    shot('shot-after-reboot.jpg')

(OUT / 'checks.json').write_text(json.dumps(checks, ensure_ascii=False, indent=2))
print(f"{sum(c['ok'] for c in checks)}/{len(checks)} 通过")
