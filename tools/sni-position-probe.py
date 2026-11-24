"""SNI 位置实验：把 server_name 扩展挪到扩展列表最后，看 GFW 还认不认。

动机：有人会想"SNI 一定要在第一个扩展吗？挪到后面是不是就绕过了？"

TLS 规范里扩展按 type 解析，与顺序无关；所以**真正的服务器照样能握手**。
问题只剩一个：GFW 的解析器是按 type 匹配，还是只看固定偏移/第一个扩展。

本脚本构造两个 ClientHello 发往同一个目标：
  A. 原样（server_name 在最前）
  B. 把 server_name 扩展整块移到扩展列表末尾（长度字段同步修正）

对照：对"被 SNI 封锁的域名"与"未被封锁的域名"各跑一次，
看 B 是否改变结果。全是原始 socket，不经过系统 TLS。
"""
from __future__ import annotations

import socket
import ssl
import struct
import sys

TARGETS = [
    ("www.google.com", "142.250.72.14"),   # SNI 被阻断
    ("www.baidu.com", "39.156.66.10"),     # 对照：未被阻断
]


def build_client_hello(host: str):
    """借 ssl.MemoryBIO 生成真实 ClientHello。"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    in_bio = ssl.MemoryBIO()
    out_bio = ssl.MemoryBIO()
    sslobj = ctx.wrap_bio(in_bio, out_bio, server_hostname=host)
    try:
        sslobj.do_handshake()
    except ssl.SSLWantReadError:
        pass
    hello = out_bio.read()
    return hello, sslobj, in_bio, out_bio


def split_extensions(hello: bytes):
    """切出 [0..ext_start) 与 (ext_start..end) 两段，以及扩展区长度字段的位置。"""
    # TLS record(5) + handshake(4) + version(2) + random(32) + session_id
    if hello[0] != 0x16:
        raise ValueError("not a TLS handshake record")
    hs = 5
    body = hs + 4
    pos = body + 2 + 32            # version + random
    sid_len = hello[pos]
    pos += 1 + sid_len
    cs_len = struct.unpack(">H", hello[pos:pos + 2])[0]
    pos += 2 + cs_len
    comp_len = hello[pos]
    pos += 1 + comp_len
    ext_total_pos = pos
    ext_total_len = struct.unpack(">H", hello[pos:pos + 2])[0]
    ext_start = pos + 2
    ext_end = ext_start + ext_total_len
    return ext_total_pos, ext_start, ext_end, ext_total_len


def parse_exts(buf: bytes):
    out = []
    i = 0
    while i + 4 <= len(buf):
        etype, elen = struct.unpack(">HH", buf[i:i + 4])
        out.append((etype, buf[i + 4:i + 4 + elen], i))
        i += 4 + elen
    return out


def move_sni_last(hello: bytes) -> bytes:
    """把 type=0（server_name）的扩展移到扩展列表末尾。"""
    total_pos, ext_start, ext_end, total_len = split_extensions(hello)
    # head 必须停在"扩展区长度字段"之前（total_pos），否则会把旧长度字段
    # 也带进去，再写一次新长度 → 报文里出现两个长度字段，服务器直接 alert。
    head = hello[:total_pos]
    ext_blob = hello[ext_start:ext_end]
    tail = hello[ext_end:]
    exts = parse_exts(ext_blob)
    sni = [e for e in exts if e[0] == 0]
    if not sni:
        raise ValueError("no server_name extension")
    others = [e for e in exts if e[0] != 0]
    ordered = others + sni               # SNI 放到最后
    new_blob = b"".join(struct.pack(">HH", t, len(v)) + v for t, v, _ in ordered)
    out = bytearray(head)
    out += struct.pack(">H", len(new_blob))
    out += new_blob
    out += tail
    # 修正长度：record 头是 5 字节（type+ver+len），handshake 头是 4 字节
    # （type 1 字节 + 长度 3 字节）。之前用 pack_into(">I", 6, ...) 会覆盖
    # handshake type 字节，把 ClientHello 写坏（服务器回 alert 0x15）。
    # 布局：record(5) = type|ver(2)|len(2)；handshake(4) = type|len(3)
    # 也就是 handshake type 在偏移 5，长度在偏移 6..8。
    struct.pack_into(">H", out, 3, len(out) - 5)        # record length
    hs_len = len(out) - 9                               # handshake body length
    out[5] = 0x01                                       # ClientHello
    out[6] = (hs_len >> 16) & 0xFF
    out[7] = (hs_len >> 8) & 0xFF
    out[8] = hs_len & 0xFF
    return bytes(out)


def probe(ip: str, hello: bytes, timeout: float = 6.0) -> str:
    try:
        s = socket.create_connection((ip, 443), timeout=timeout)
    except Exception as exc:
        return f"TCP 失败({type(exc).__name__})"
    try:
        s.sendall(hello)
        s.settimeout(timeout)
        try:
            data = s.recv(64)
        except socket.timeout:
            return "超时（无响应）"
        except ConnectionResetError:
            return "RST（连接被重置）"
        if not data:
            return "对端关闭"
        return "有响应: " + data[:2].hex()
    finally:
        s.close()


def main() -> None:
    for host, ip in TARGETS:
        print(f"\n目标 {host}  ({ip})")
        hello, sslobj, in_bio, out_bio = build_client_hello(host)
        try:
            moved = move_sni_last(hello)
        except Exception as exc:
            print(f"  改序失败: {exc}")
            continue
        print(f"  原始 ClientHello {len(hello)}B，SNI 在第 {parse_exts(hello[split_extensions(hello)[1]:split_extensions(hello)[2]])[0][0]} 号扩展")
        print(f"  A. 原样        -> {probe(ip, hello)}")
        print(f"  B. SNI 移到最后 -> {probe(ip, moved)}")
        # 校验改序后服务器是否仍能握上（证明改序对服务器无影响）
        for label, blob in (("A", hello), ("B", moved)):
            try:
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                inb, outb = ssl.MemoryBIO(), ssl.MemoryBIO()
                obj = ctx.wrap_bio(inb, outb, server_hostname=host)
                outb.write(blob)
                try:
                    obj.do_handshake()
                    ok = "握手成功"
                except ssl.SSLWantReadError:
                    ok = "已发出 ClientHello（等服务器）"
                except Exception as exc:
                    ok = f"{type(exc).__name__}"
                print(f"  {label} 本地 TLS 引擎接受该报文: {ok}")
            except Exception as exc:
                print(f"  {label} 校验失败: {exc}")


if __name__ == "__main__":
    sys.exit(main())
