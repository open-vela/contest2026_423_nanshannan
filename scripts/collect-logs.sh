#!/usr/bin/env bash
# 归集本项目的 AI Coding 日志到 logs/。
#
# 为什么需要这个脚本
# ------------------
# 官方的 contest-log-collector 用"当前目录向上能找到 .repo/"作隐私门控，
# 会话结束时自动入仓。但从工作区**外**启动的会话（例如 cwd 是
# /home/dministrator/linux）不会被采集 —— 本项目的主力会话正是这种情况。
#
# 官方给的补救手段是 export-session.py --backfill，但它扫描整个
# ~/.claude/projects/，会把**所有**个人项目的对话一并导入。参赛仓默认
# 是 public，那样等于公开无关的私人对话。实测一次 --backfill 会带进
# 39 个无关会话共 23.8MB（论文排版、面试题整理、其它项目……）。
#
# 所以这里改用**会话白名单**：只导出确属本项目的会话，其余一律不碰。
#
# 用法
# ----
#   ./scripts/collect-logs.sh            # 导出并校验
#   ./scripts/collect-logs.sh --check    # 只校验，不导出
#
# 新增会话时，把 session id 追加到下面的 SESSIONS 里。
# 查 id：python3 <skill>/tools/export-session.py --list
#        或看 ~/.claude/projects/<项目目录>/<sid>.jsonl 的文件名。
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"
TOOLS="$WS/.claude/skills/contest-log-collector/tools"

# ★ 白名单：只有这里列出的会话会被导出
#
#   ~/.claude/projects/ 下的目录是按**工作目录**分的，不是按项目分的。
#   本项目的会话 cwd 在 /home/dministrator/linux，那个目录下还跑过论文
#   排版、面试题整理、算法题等完全无关的会话 —— 一次 --backfill 会把它们
#   一并导入。参赛仓是 public 的，那等于公开私人对话。
#
#   实测曾有 6 个无关会话（共 19MB）混进 logs/，已清除。新增会话时务必
#   确认它确属本项目再往下面加。
SESSIONS=(
  # RK3576 / KICKPI-K7 BSP 适配 —— 摄像头/音频/PL330 等（当前会话）
  6da36939-72ed-45a4-8311-5cc9b1b96e52
  # RK3576 / KICKPI-K7 BSP 适配 —— 显示调试（VOP2 / DSI / LCD）等前期工作
  6396523f-2861-4bec-8d27-a68bbd63cfc3
  # 09-19/20：Linux 用户态、AI 助手与语音唤醒、开机 logo、xTS 严格复测
  a6fadf52-fde8-4859-b800-e334901c58df
  # 10-24 ~ 11-05：SMP/eMMC、SKW6621S WiFi/BT、外设划分与 UI、OTA、蓝牙与媒体框架
  3069be32-7076-4e98-bf4e-8cf84b8fc52e
)

[ -f "$TOOLS/export-session.py" ] || {
  echo "找不到 $TOOLS/export-session.py —— 是否已 repo sync？" >&2; exit 1; }

if [ "${1:-}" != "--check" ]; then
  # ★ 先把新会话扫进**本地暂存区**（~/.claude/contest-collector-staging）。
  #   export-session.py --session 只认暂存区里已有的会话；钩子没装或会话
  #   从工作区外启动时，新会话不在里面，直接导出会报 "No sessions matched"。
  python3 "$TOOLS/export-session.py" --backfill --source claude --dest "$ROOT" \
    --dry-run >/dev/null 2>&1 || true

  for sid in "${SESSIONS[@]}"; do
    python3 "$TOOLS/export-session.py" --session "$sid" --dest "$ROOT" --confirm \
      | sed "s/^/  [${sid:0:8}] /"
  done

  # ★ 清掉白名单之外的会话。
  #
  #   --backfill 虽然只往暂存区写，实测它**同时**会把扫到的会话落进
  #   logs/（2026-09-20 实测多出 6 个，其中包含校招、论文等私人对话）。
  #   本仓是 public，所以导出之后必须按白名单再裁一遍，manifest 同步裁剪。
  python3 - "$ROOT" "${SESSIONS[@]}" <<'PY'
import json, sys
from pathlib import Path

root = Path(sys.argv[1])
keep = set(sys.argv[2:])
logs = root / 'logs' / 'zhouyllll'
for f in sorted(logs.glob('*/*.jsonl')):
    sid = f.stem.split('__', 1)[-1]
    if sid not in keep:
        print(f'  ✂ 白名单外，删除 {f.relative_to(root)}')
        f.unlink()
mf = logs / 'manifest.json'
if mf.exists():
    d = json.loads(mf.read_text())
    before = len(d.get('sessions', []))
    d['sessions'] = [x for x in d.get('sessions', []) if x['session_id'] in keep]
    if len(d['sessions']) != before:
        mf.write_text(json.dumps(d, ensure_ascii=False, indent=2) + '\n')
        print(f'  ✂ manifest 会话 {before} -> {len(d["sessions"])}')
for d in sorted(logs.glob('*/')):
    if d.is_dir() and not any(d.iterdir()):
        d.rmdir()
PY
fi

# 校验器返回 2 = 只有警告（例如按天分段的会话，manifest 只登记最新一段，
# 其余各段报 orphan）。不能让 set -e 在这里退出，否则下面的脱敏不会执行。
python3 "$TOOLS/validate-log.py" "$ROOT/logs/zhouyllll" || [ $? -eq 2 ]

# ★ 脱敏：会话日志里会出现运行时配置过的密钥
# ------------------------------------------------
# 白名单只解决"哪些会话该进仓"，解决不了"会话里说了什么"。本仓是 public，
# 而调试期在板子上 `set_llm` 配过的 MiMo API key 会**原样出现在对话记录里**
# —— 实测一份日志里出现 7 次，并且已经随一次提交进了历史（未推送，已用
# git-filter-repo 全量重写清掉）。
#
# 所以导出之后、提交之前必须再过一遍脱敏。放在这里而不是 .gitignore：
# .gitignore 管的是"哪些文件不进仓"，而日志是**必须**进仓的，要处理的是
# 文件内容。
#
# 新增密钥模式时往下面加一行 sed。

scrub() {
  local n
  n=$(grep -roE 'tp-[a-z0-9]{40,}|sk-[A-Za-z0-9]{20,}|ghp_[A-Za-z0-9]{30,}' \
        "$ROOT/logs" 2>/dev/null | wc -l)
  if [ "$n" -gt 0 ]; then
    grep -rlE 'tp-[a-z0-9]{40,}|sk-[A-Za-z0-9]{20,}|ghp_[A-Za-z0-9]{30,}' \
        "$ROOT/logs" 2>/dev/null |
      while read -r f; do
        sed -i -E 's/tp-[a-z0-9]{40,}/***REDACTED-API-KEY***/g;
                   s/sk-[A-Za-z0-9]{20,}/***REDACTED-API-KEY***/g;
                   s/ghp_[A-Za-z0-9]{30,}/***REDACTED-TOKEN***/g' "$f"
        echo "  脱敏: $f"
      done
    echo "  ★ 共处理 $n 处密钥；请确认 git diff 之后再提交"
  else
    echo "  脱敏检查: 未发现密钥"
  fi
}

scrub

# ★ 无线相关脱敏：WiFi 密码、MAC 地址、扫描到的 SSID
# ------------------------------------------------
# 调 WiFi 时在板上敲过 `wifi connect <SSID> <密码>`，密码原样留在对话里；
# 扫描结果里是周围别人的网络名和 BSSID，蓝牙扫描里是别人设备的地址。
#
# 密码不写进本脚本（脚本本身要入仓）：从日志里的 `wifi connect` 命令自动
# 认出来，再全文替换。MAC 一律遮掉；扫描结果行（"ch N  -xx dBm  名字"）
# 里的网络名遮掉。
scrub_wireless() {
  python3 - "$ROOT/logs" <<'PY'
import re, sys
from pathlib import Path

files = sorted(Path(sys.argv[1]).rglob('*.jsonl'))
cmd = re.compile(r'wifi connect +(?:\\"|")?[^\s"\\<\[]+(?:\\"|")? +([^\s"\\<\[`]{8,64})')
pw = set()
for f in files:
    pw.update(cmd.findall(f.read_text(errors='replace')))
mac = re.compile(r'(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b')
scan = re.compile(r'(\bch +\d+ +-\d+ dBm +)(?!<hidden>)([^\s"\\][^"\\\n]{0,31}?)(?=\\n|\\r|\n|"|$)')
total = 0
for f in files:
    t = f.read_text(errors='replace')
    o = t
    for p in pw:
        t = t.replace(p, '***REDACTED-WIFI-PSK***')
    t = mac.sub('xx:xx:xx:xx:xx:xx', t)
    t = scan.sub(lambda m: m.group(1) + '***SSID***', t)
    if t != o:
        f.write_text(t)
        total += 1
        print(f'  无线脱敏: {f}')
print(f'  无线脱敏: 识别出 {len(pw)} 个 WiFi 密码，改动 {total} 个文件')
PY
}

scrub_wireless

echo
echo "提交： git add logs/ && git commit -s -m 'logs: 更新 AI Coding 日志'"
