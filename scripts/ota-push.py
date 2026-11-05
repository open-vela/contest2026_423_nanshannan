#!/usr/bin/env python3
"""经网络把新的 openvela 镜像（AMP FIT）装进板上没在运行的 A/B 槽。

用法：
  python3 scripts/ota-push.py                 用 ../nuttx/nuttx.bin 现打一份 FIT
  python3 scripts/ota-push.py --fit X.itb     用现成的 FIT
  python3 scripts/ota-push.py --reboot        装完立刻重启试运行
  python3 scripts/ota-push.py --sha256 <值>   故意给错的值（负向测试：板子必须拒绝）

流程：主机起一个只服务这一个文件的 HTTP 服务，经 telnet 让板子执行
  ota install http://<主机>:<端口>/<文件名> <字节数> <sha256>
板子把它写进没在运行的槽，回读校验一致才设为下次启动（app/k7ota）。
以板上"完成："那一行作为成功判据。

重启后 U-Boot 校验槽内 FIT 再试启动；界面起来 15 秒后自动确认。
没确认（新镜像挂在启动途中，按 RESET 后）就回到原来的槽。
格式与布局见 amp/include/k7_bootab.h。
"""
import argparse
import hashlib
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WS = os.path.dirname(ROOT)
MKIMAGE_PATH = os.path.expanduser("~/rk3576-amp/u-boot/tools") + ":" + \
    os.path.expanduser("~/rk3576-amp/u-boot/scripts/dtc")
SLOT_MAX = 65536 * 512

_spec = importlib.util.spec_from_file_location(
    "lxroot_push", os.path.join(ROOT, "scripts", "lxroot-push.py"))
lx = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lx)


def build_fit(nuttx_bin, out_dir):
    """和 flash.sh 一样打包：amp.its + nuttx.bin → 外置数据的 FIT"""
    os.makedirs(out_dir, exist_ok=True)
    subprocess.run(["cp", nuttx_bin, os.path.join(out_dir, "openvela-amp.bin")],
                   check=True)
    subprocess.run(["cp", os.path.join(ROOT, "amp", "fit", "amp.its"), out_dir],
                   check=True)
    env = dict(os.environ, PATH=MKIMAGE_PATH + ":" + os.environ["PATH"])
    subprocess.run(["mkimage", "-f", "amp.its", "-E", "-p", "0xe00", "amp.itb"],
                   cwd=out_dir, env=env, check=True, stdout=subprocess.DEVNULL)
    return os.path.join(out_dir, "amp.itb")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fit", help="现成的 FIT；不给就用 nuttx.bin 现打")
    ap.add_argument("--bin", default=os.path.join(WS, "nuttx", "nuttx.bin"))
    ap.add_argument("--sha256", help="覆盖实际值（负向测试用）")
    ap.add_argument("--port", type=int, default=8089)
    ap.add_argument("--reboot", action="store_true", help="装完重启")
    a = ap.parse_args()

    fit = a.fit or build_fit(a.bin, "/tmp/k7-ota/build")
    data = open(fit, "rb").read()
    if data[:4] != bytes.fromhex("d00dfeed"):
        sys.exit(f"{fit} 不是 FIT")
    if len(data) > SLOT_MAX:
        sys.exit(f"{fit} 有 {len(data)} 字节，超过一个槽（{SLOT_MAX}）")

    sha = a.sha256 or hashlib.sha256(data).hexdigest()
    print(f"FIT {fit}: {len(data)} 字节 sha256 {hashlib.sha256(data).hexdigest()}")
    if a.sha256:
        print(f"★ 告诉板子的 sha256 是 {sha}（负向测试）")

    ip = lx.host_ip_for(lx.BOARD)
    srv, name = lx.serve_one(fit, a.port)
    budget = 120 + len(data) // (100 * 1024)
    out = lx.run_on_board(
        f"ota install http://{ip}:{a.port}/{name} {len(data)} {sha}", budget)
    srv.shutdown()

    ok = "完成：".encode() in out
    print("\n结果:", "成功" if ok else "失败")
    if ok and a.reboot:
        lx.run_on_board("reboot", 3)
        print("已发 reboot：看串口上 U-Boot 的 AMP: A/B 几行")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
