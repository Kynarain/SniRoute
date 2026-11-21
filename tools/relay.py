#!/usr/bin/env python3
"""relay.py -- 按 SNI 路由的本地 TCP 中继

为什么需要它
------------
有些目标域名在当前网络里没有可用的直连路径：带 SNI 会被封，不带 SNI 服务端又不认
（YouTube 的 googlevideo、Twitch 的 VOD、Discord 的更新域名都是这一类）。唯一的
出路是把上游连接放进隧道，让 SNI 只出现在隧道内部。

本机的 nginx 无法自己说"走隧道"，所以这里做一层：nginx 照常向本进程发起 TLS，
本进程从 ClientHello 里读出目标主机名，经 SOCKS5（由 `ssh -D` 提供）连到
目标 443，然后把字节双向透传。TLS 握手其实是 nginx 与目标之间端到端完成的，
本进程只负责"看一眼 SNI、选个出口"。

为什么按 SNI 而不是按端口
------------------------
googlevideo 的节点名是动态的（rr1---sn-xxxx 这种，每个视频都不一样），没法
预先为每个主机开一个监听端口。读 SNI 就只需要一个端口。

用法
----
    python relay.py --listen 127.0.0.1:9555 --socks 127.0.0.1:1080
    python relay.py --listen 127.0.0.1:9555 --direct      # 自测：不走隧道
"""

import argparse
import socket
import struct
import sys
import threading
import time

# TLS 记录类型 0x16 = handshake；握手子类型 0x01 = ClientHello
REC_HANDSHAKE = 0x16
HS_CLIENT_HELLO = 0x01


def _read_exact(sock, n, timeout):
    sock.settimeout(timeout)
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed while reading handshake")
        buf += chunk
    return buf


def peek_sni(conn, timeout=5.0):
    """读进一段 ClientHello，返回 (sni, 已读字节)。原始字节必须回放给目标端。

    只解析到 server_name 扩展就停，不消费后续数据；所有读到的字节原样返回，
    因为 TLS 握手是端到端的，我们只是偷看，不能吞掉任何字节。
    """
    head = _read_exact(conn, 5, timeout)
    if head[0] != REC_HANDSHAKE:
        # 不是 TLS：可能是明文探测或错误请求，交给上层直接断开
        return None, head
    rec_len = struct.unpack(">H", head[3:5])[0]
    body = _read_exact(conn, rec_len, timeout)
    data = head + body

    if not body or body[0] != HS_CLIENT_HELLO:
        return None, data

    # 跳过：handshake header(4) + version(2) + random(32)
    i = 4 + 2 + 32
    sid_len = body[i]
    i += 1 + sid_len
    cs_len = struct.unpack(">H", body[i:i + 2])[0]
    i += 2 + cs_len
    comp_len = body[i]
    i += 1 + comp_len
    if i + 2 > len(body):
        return None, data
    ext_total = struct.unpack(">H", body[i:i + 2])[0]
    i += 2
    end = min(len(body), i + ext_total)

    while i + 4 <= end:
        etype, elen = struct.unpack(">HH", body[i:i + 4])
        i += 4
        if i + elen > len(body):
            break
        if etype == 0:  # server_name
            # list_len(2) + type(1) + name_len(2) + name
            p = i + 2
            if p + 3 <= len(body):
                name_len = struct.unpack(">H", body[p + 1:p + 3])[0]
                name = body[p + 3:p + 3 + name_len]
                return name.decode("ascii", errors="ignore"), data
        i += elen
    return None, data


def socks5_connect(socks_host, socks_port, target_host, target_port, timeout=10.0):
    """通过 SOCKS5（ssh -D 提供）连到目标，返回已连接的 socket。"""
    s = socket.create_connection((socks_host, socks_port), timeout=timeout)
    s.settimeout(timeout)
    s.sendall(b"\x05\x01\x00")                      # VER=5, NMETHODS=1, NO AUTH
    if s.recv(2) != b"\x05\x00":
        s.close()
        raise ConnectionError("socks5: auth method rejected")

    host_b = target_host.encode("ascii", errors="ignore")
    req = b"\x05\x01\x00\x03" + bytes([len(host_b)]) + host_b
    req += struct.pack(">H", target_port)
    s.sendall(req)

    rep = _read_exact(s, 4, timeout)
    if rep[1] != 0:
        s.close()
        raise ConnectionError(f"socks5: connect failed (code {rep[1]})")
    atyp = rep[3]
    if atyp == 0x01:
        _read_exact(s, 4, timeout)
    elif atyp == 0x03:
        ln = _read_exact(s, 1, timeout)[0]
        _read_exact(s, ln, timeout)
    elif atyp == 0x04:
        _read_exact(s, 16, timeout)
    _read_exact(s, 2, timeout)
    return s


def pump(a, b):
    """双向透传，任一方向结束就关闭两端。"""
    def one_way(src, dst):
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                dst.sendall(data)
        except OSError:
            pass
        finally:
            for s in (src, dst):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    t = threading.Thread(target=one_way, args=(a, b), daemon=True)
    t.start()
    one_way(b, a)
    t.join(timeout=1.0)


class Relay:
    def __init__(self, listen, socks=None):
        self.listen = listen
        self.socks = socks          # (host, port) 或 None = 直连（自测用）
        self.stop_flag = threading.Event()
        self.server = None
        self.log_lock = threading.Lock()

    def log(self, msg):
        with self.log_lock:
            print(f"[relay] {msg}", flush=True)

    def handle(self, conn, addr):
        try:
            sni, first_bytes = peek_sni(conn)
            if not sni:
                self.log(f"{addr[0]} 没有 SNI，断开（不该走到这里）")
                conn.close()
                return

            if self.socks:
                upstream = socks5_connect(self.socks[0], self.socks[1], sni, 443)
                via = f"socks {self.socks[0]}:{self.socks[1]}"
            else:
                upstream = socket.create_connection((sni, 443), timeout=10)
                via = "direct"

            upstream.sendall(first_bytes)       # 回放偷看的字节
            self.log(f"{sni} -> {via}")
            pump(conn, upstream)
        except Exception as exc:
            self.log(f"{addr[0]} 失败: {type(exc).__name__}: {exc}")
            try:
                conn.close()
            except OSError:
                pass

    def serve(self):
        host, _, port = self.listen.rpartition(":")
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # Windows 上 SO_REUSEADDR 允许两个进程绑同一端口，宁可启动失败也不要静默双开
        if not sys.platform.startswith("win"):
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host or "127.0.0.1", int(port)))
        srv.listen(128)
        self.server = srv
        self.log(f"监听 {self.listen}" + (f"（出口 {self.socks[0]}:{self.socks[1]}）" if self.socks else "（直连，仅自测）"))
        while not self.stop_flag.is_set():
            try:
                conn, addr = srv.accept()
            except OSError:
                break
            threading.Thread(target=self.handle, args=(conn, addr), daemon=True).start()

    def stop(self):
        self.stop_flag.set()
        if self.server:
            try:
                self.server.close()
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser(description="按 SNI 路由的本地中继")
    ap.add_argument("--listen", default="127.0.0.1:9555")
    ap.add_argument("--socks", default="127.0.0.1:1080")
    ap.add_argument("--direct", action="store_true", help="不走隧道，直接连目标（自测用）")
    args = ap.parse_args()

    socks = None
    if not args.direct:
        h, _, p = args.socks.rpartition(":")
        socks = (h or "127.0.0.1", int(p))

    relay = Relay(args.listen, socks)
    try:
        relay.serve()
    except KeyboardInterrupt:
        relay.stop()


if __name__ == "__main__":
    main()
