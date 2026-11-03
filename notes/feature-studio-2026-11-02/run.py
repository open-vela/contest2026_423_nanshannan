#!/usr/bin/env python3
"""录制模式联调（2026-11-02）：主机假扮家居设备，经 telnet 的调试入口开始 / 结束录制，
记录板上 broker 上的全部消息，逐项判定。

用法：python3 run.py [输出目录]   依赖 mosquitto-clients；家居模拟器 scripts/home-sim.sh。
"""
import json, os, subprocess, sys, time
from pathlib import Path

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
ROOT = Path(__file__).resolve().parents[2]
B = '192.168.1.50'
t0 = time.monotonic()

def netsh(cmd, wait='10'):
    env = {**os.environ, 'K7_NETSH_OUT': str(OUT / '_netsh')}
    r = subprocess.run([sys.executable, str(ROOT / 'scripts/xts-netsh.py'), cmd, wait],
                       capture_output=True, text=True, env=env, timeout=60)
    return r.stdout

def pub(topic, msg, retain=False):
    subprocess.run(['mosquitto_pub', '-h', B, '-t', topic, '-m', msg] + (['-r'] if retain else []), check=True)

def log(s):
    line = f'{time.monotonic() - t0:7.2f} {s}'
    print(line, flush=True)
    steps.write(line + '\n')

steps = open(OUT / 'steps.log', 'w')
sim = subprocess.Popen(['bash', str(ROOT / 'scripts/home-sim.sh'), B], stdout=open(OUT / 'home-sim.log', 'w'),
                       stderr=subprocess.STDOUT)
sub = subprocess.Popen(['mosquitto_sub', '-h', B, '-v', '-t', 'home/#'], stdout=subprocess.PIPE, text=True, bufsize=1)
msgs = []
import threading
def reader():
    for line in sub.stdout:
        msgs.append((time.monotonic() - t0, line.rstrip('\n')))
threading.Thread(target=reader, daemon=True).start()

def state():
    st = {}
    for _, l in msgs:
        tp, _, pl = l.partition(' ')
        if tp.endswith('/state'):
            st[tp.split('/')[1]] = pl
        if tp == 'home/studio/onair':
            st['onair'] = pl
    return st

def mark():
    return len(msgs)

def since(i):
    return [l for _, l in msgs[i:]]

checks = []
def check(name, ok, detail=''):
    checks.append({'name': name, 'ok': bool(ok), 'detail': detail})
    log(('PASS ' if ok else 'FAIL ') + name + ('' if ok else f'  {detail}'))

try:
    time.sleep(2.5)                                  # 模拟器发完初始状态
    pub('home/ac/set', 'ON'); pub('home/fresh_air/set', 'ON')
    time.sleep(1.5)
    st = state(); log(f'开始前 {st}')
    check('开始前：空调、新风开，台灯关', st.get('ac') == 'ON' and st.get('fresh_air') == 'ON' and st.get('desk_lamp') == 'OFF', st)

    i = mark(); log('>>> echo 1 > /tmp/k7-studio')
    netsh('echo 1 > /tmp/k7-studio'); time.sleep(2.5)
    got = since(i); st = state(); log(f'开始后消息 {got}')
    check('开始：下发 台灯 ON、空调 OFF、新风 OFF', {'home/desk_lamp/set ON', 'home/ac/set OFF', 'home/fresh_air/set OFF'} <= set(got), got)
    check('开始：电视、饮水机本来就关，不发指令', not any(l.startswith(('home/tv/set', 'home/kettle/set')) for l in got), got)
    check('开始：门口状态 ON（retain）', st.get('onair') == 'ON', st)

    i = mark(); log('>>> 门铃'); pub('home/doorbell/event', 'ring'); time.sleep(1.5)
    got = since(i); log(f'门铃后消息 {got}')
    check('门铃：录制中代答', any(l.startswith('home/doorbell/reply ') for l in got), got)

    log('>>> 录制中手动打开新风'); pub('home/fresh_air/set', 'ON'); time.sleep(1.5)
    shot = netsh('echo 2 > /tmp/k7-shot'); time.sleep(2)

    i = mark(); log('>>> echo 0 > /tmp/k7-studio')
    netsh('echo 0 > /tmp/k7-studio'); time.sleep(2.5)
    got = since(i); st = state(); log(f'结束后消息 {got}')
    check('结束：台灯恢复 OFF、空调恢复 ON', {'home/desk_lamp/set OFF', 'home/ac/set ON'} <= set(got), got)
    check('结束：录制中被手动打开的新风不去动', not any(l.startswith('home/fresh_air/set') for l in got), got)
    check('结束：门口状态 OFF', st.get('onair') == 'OFF', st)
    log(f'结束后状态 {st}')

    txt = netsh('cat /data/studio/marks.txt')
    (OUT / 'marks.txt.net.txt').write_text(txt)
    check('打点清单写进 /data/studio/marks.txt', '起录' in txt and '门铃' in txt, txt[-300:])
finally:
    sub.terminate(); sim.terminate()
    subprocess.run(['pkill', '-f', 'mosquitto_sub -h ' + B + ' -v -t home/\\+/set'])
    (OUT / 'mqtt.log').write_text(''.join(f'{t:7.2f} {l}\n' for t, l in msgs))
    (OUT / 'checks.json').write_text(json.dumps(checks, ensure_ascii=False, indent=2))
    print(f"{sum(c['ok'] for c in checks)}/{len(checks)} 通过")
