"""把 ClientHello 拆成多个 TLS record 发送，看能否躲过 SNI 检测。

与"TCP 分段"的区别：
  * TCP 分段只是把字节流切开，重组后完全一样；
  * TLS record 分段是**协议层**的合法行为（RFC 8446 §5.1 允许 handshake 消息
    跨多个 record），但 shallow DPI 如果只解析第一个 record，就可能漏掉
    SNI（它被放到第二个 record 里）。

变体：
  A 对照：单 record，原样
  B 在 SNI 开始处切开（SNI 完整落在第二个 record）
  C 在 SNI 中间切开
  D 第一个 record 只放 1 字节（极端：逼 DPI 放弃）

每个变体跑 3 次，返回是否拿到 TLS 握手响应（首字节 0x16）。
"""
from __future__ import annotations

import socket
import ssl
import sys
import time

TARGETS = [
    ("www.pixiv.net", "104.18.42.239"),      # 已知 SNI 阻断
    ("discord.com", "162.159.128.233"),      # 已知 SNI 阻断
    ("www.google.com", "142.250.72.14"),     # 已知 SNI 阻断
    ("www.cloudflare.com", "104.16.132.229"),  # 对照：未被阻断
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


def split_records(hello: bytes, cut_payload: int) -> bytes:
    """把单条 record 的 payload 拆成两条 record。cut_payload 按 payload 偏移算。"""
    rec_len = int.from_bytes(hello[3:5], "big")
    payload = hello[5:5 + rec_len]
    tail = hello[5 + rec_len:]
    cut = max(1, min(len(payload) - 1, cut_payload))
    p1, p2 = payload[:cut], payload[cut:]
    r1 = b"\x16\x03\x01" + len(p1).to_bytes(2, "big") + p1
    r2 = b"\x16\x03\x01" + len(p2).to_bytes(2, "big") + p2
    return r1 + r2 + tail


def send(ip: str, blob: bytes, timeout: float = 6.0) -> str:
    try:
        s = socket.create_connection((ip, 443), timeout=timeout)
    except Exception as exc:
        return f"TCP失败({type(exc).__name__})"
    try:
        s.sendall(blob)
        s.settimeout(timeout)
        try:
            data = s.recv(16)
        except socket.timeout:
            return "超时"
        except ConnectionResetError:
            return "RST"
        if not data:
            return "对端关闭"
        if data[0] == 0x16:
            return "握手响应"
        if data[0] == 0x15:
            return "alert"
        return f"其他({data[0]:02x})"
    finally:
        s.close()


def main() -> None:
    for host, ip in TARGETS:
        hello = build_client_hello(host)
        off, nlen = find_sni(hello)
        # 把绝对偏移换算成 payload 偏移（payload 从 hello[5] 开始）
        payload_off = off - 5
        variants = [
            ("A 单record", hello),
            ("B SNI前切", split_records(hello, payload_off)),
            ("C SNI中切", split_records(hello, payload_off + nlen // 2)),
            ("D 首record1字节", split_records(hello, 1)),
        ]
        print(f"\n目标 {host} ({ip})  ClientHello {len(hello)}B  SNI@{off}+{nlen}")
        for label, blob in variants:
            res = [send(ip, blob) for _ in range(3)]
            ok = sum(1 for r in res if r == "握手响应")
            print(f"  {label:<16} 成功 {ok}/3   {res}")


if __name__ == "__main__":
    sys.exit(main())
