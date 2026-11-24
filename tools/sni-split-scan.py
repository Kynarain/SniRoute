"""在 SNI 内部按不同偏移切分 ClientHello，看哪些切法能过。

背景：项目早期用 tools/tls-fragment-probe.py 测过 3 种切法（整包 / 首字节+剩余 /
SNI 对半），当时结论是"都不行"。后来重跑时发现 x.com 在对半切分下能握手，
说明结论不该一概而论——切点位置可能影响检测器的匹配。

本脚本对每个目标，把 ClientHello 在 SNI 内部的不同偏移切成两段发送：
  0   = SNI 之前（等价于把 SNI 放到第二段开头）
  k   = SNI 中间第 k 个字节后
  -1  = SNI 结束后
每个偏移跑 2 次，记录是否拿到 TLS 握手响应（0x16）。
"""
from __future__ import annotations

import socket
import ssl
import sys
import time

TARGETS = [
    ("www.pixiv.net", "104.18.42.239"),
    ("x.com", "162.159.140.229"),
    ("www.google.com", "142.250.72.14"),
    ("discord.com", "162.159.128.233"),
]


def build_client_hello(host: str) -> bytes:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    inb, outb = ssl.MemoryBIO(), ssl.MemoryBIO()
    obj = ctx.wrap_bio(inb, outb, server_hostname=host)
    try:
        obj.do_handshake()
    except ssl.SSLWantReadError:
        pass
    return outb.read()


def find_sni(hello: bytes) -> tuple[int, int]:
    """返回 (SNI 主机名的起始偏移, 长度)。"""
    i = 43
    sid_len = hello[i]
    i += 1 + sid_len
    cs_len = int.from_bytes(hello[i:i + 2], "big")
    i += 2 + cs_len
    comp_len = hello[i]
    i += 1 + comp_len
    ext_total = int.from_bytes(hello[i:i + 2], "big")
    i += 2
    end = i + ext_total
    while i + 4 <= end:
        etype = int.from_bytes(hello[i:i + 2], "big")
        elen = int.from_bytes(hello[i + 2:i + 4], "big")
        body = i + 4
        if etype == 0:
            nlen = int.from_bytes(hello[body + 3:body + 5], "big")
            return body + 5, nlen
        i = body + elen
    raise ValueError("no SNI")


def send_split(ip: str, hello: bytes, cut: int, timeout: float = 6.0) -> str:
    try:
        s = socket.create_connection((ip, 443), timeout=timeout)
    except Exception as exc:
        return f"TCP失败({type(exc).__name__})"
    try:
        try:
            s.sendall(hello[:cut])
            time.sleep(0.08)
            if cut < len(hello):
                s.sendall(hello[cut:])
        except ConnectionResetError:
            return "RST(发送时)"
        except OSError as exc:
            return f"发送失败({type(exc).__name__})"
        s.settimeout(timeout)
        try:
            data = s.recv(16)
        except socket.timeout:
            return "超时"
        except ConnectionResetError:
            return "RST"
        if not data:
            return "对端关闭"
        return "握手响应" if data[0] == 0x16 else f"alert({data[1] if len(data) > 1 else 0:02x})"
    finally:
        s.close()


def main() -> None:
    for host, ip in TARGETS:
        hello = build_client_hello(host)
        off, nlen = find_sni(hello)
        cuts = [off, off + 1, off + nlen // 2, off + nlen - 1, off + nlen]
        print(f"\n目标 {host} ({ip})  ClientHello {len(hello)}B  SNI@{off} 长度{nlen}")
        print(f"  {'切点':<12} {'结果（2 次）'}")
        whole = [send_split(ip, hello, len(hello)) for _ in range(2)]
        print(f"  {'整包(对照)':<12} {whole}")
        for cut in cuts:
            label = "SNI前" if cut == off else ("SNI后" if cut == off + nlen else f"SNI+{cut - off}")
            res = [send_split(ip, hello, cut) for _ in range(2)]
            print(f"  {label:<12} {res}")


if __name__ == "__main__":
    sys.exit(main())
