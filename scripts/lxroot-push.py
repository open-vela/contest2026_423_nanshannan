#!/usr/bin/env python3
"""把 A72 上 Linux 的根文件系统镜像经网络写进板上 eMMC 的 backup 分区。

用法：python3 scripts/lxroot-push.py <镜像文件> [--sha256 <故意给的值>]

流程：主机起一个只服务这一个文件的 HTTP 服务，经 telnet 让板子执行
  lxroot write http://<主机>:<端口>/<文件名> <字节数> <sha256>
板子边下边写、回读校验一致后才写头部（格式见 amp/include/k7_lxroot.h）。
本脚本把板上输出原样打出来，以板上"完成："那一行作为成功判据。

--sha256 用来做负向测试：给一个错的值，板子必须拒绝写头部。

★ WSL 镜像网络下，默认 MSS 的 HTTP 服务传大文件会卡住（板子 recv 超时），
  监听套接字设 TCP_MAXSEG=536 后正常（notes/wireless-research-2026-10-26.md）。
"""
import argparse
import hashlib
import http.server
import os
import socket
import subprocess
import sys
import threading
import time

BOARD = os.environ.get("K7_HOST", "192.168.1.50")
TELNET_PORT = int(os.environ.get("K7_TELNET_PORT", "2323"))


def host_ip_for(board):
    """主机上哪个地址能被板子访问到：问路由表去板子走哪个源地址。"""
    out = subprocess.run(["ip", "route", "get", board], capture_output=True,
                         text=True, check=True).stdout.split()
    return out[out.index("src") + 1]


class OneFile(http.server.BaseHTTPRequestHandler):
    path_on_disk = None
    url_name = None

    def do_GET(self):
        if self.path != "/" + self.url_name:
            self.send_error(404)
            return
        size = os.path.getsize(self.path_on_disk)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(self.path_on_disk, "rb") as f:
            while True:
                b = f.read(64 * 1024)
                if not b:
                    break
                self.wfile.write(b)

    def log_message(self, fmt, *args):
        print("[http] " + fmt % args)


class SmallMssServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True

    def server_bind(self):
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 536)
        super().server_bind()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--sha256", help="覆盖实际值（负向测试用）")
    ap.add_argument("--port", type=int, default=8088)
    a = ap.parse_args()

    size = os.path.getsize(a.image)
    h = hashlib.sha256()
    with open(a.image, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    sha = a.sha256 or h.hexdigest()
    ip = host_ip_for(BOARD)
    print(f"镜像 {a.image}: {size} 字节 sha256 {h.hexdigest()}")
    if a.sha256:
        print(f"★ 告诉板子的 sha256 是 {sha}（负向测试）")

    srv, name = serve_one(a.image, a.port)

    # 总时限按体积算：最慢按 100KB/s 计，再加回读和余量
    budget = 120 + size // (100 * 1024)
    out = run_on_board(f"lxroot write http://{ip}:{a.port}/{name} {size} {sha}",
                       budget)
    srv.shutdown()

    ok = "完成：".encode() in out
    print("\n结果:", "成功" if ok else "失败")
    sys.exit(0 if ok else 1)


def serve_one(path, port):
    """起一个只服务这一个文件的 HTTP 服务，返回 (服务, URL 里的文件名)"""
    OneFile.path_on_disk = os.path.abspath(path)
    OneFile.url_name = os.path.basename(path)
    srv = SmallMssServer(("0.0.0.0", port), OneFile)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, OneFile.url_name


def run_on_board(cmd, budget):
    """经 telnet NSH 发一条命令，把输出原样打出来，等到提示符或超时"""
    s = socket.create_connection((BOARD, TELNET_PORT), timeout=5)
    s.settimeout(1)
    time.sleep(1.5)
    try:
        s.recv(65536)
    except socket.timeout:
        pass
    s.sendall((cmd + "\r\n").encode())

    out = b""
    end = time.time() + budget
    while time.time() < end:
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            break
        sys.stdout.write(d.replace(b"\xff", b"").decode("utf-8", "replace"))
        sys.stdout.flush()
        out += d
        if b"nsh> " in out[-200:]:
            break
    else:
        print(f"\n超过 {budget} 秒没结束")
    s.close()
    return out


if __name__ == "__main__":
    main()
