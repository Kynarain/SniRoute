"""QUIC Initial 分片（sni-slicing）：把 ClientHello 拆到两个 UDP 数据报里。

依据：Zohaib et al., USENIX Security '25 —— GFW 会解密 QUIC Initial 读 SNI，
但**不重组跨数据报的 Initial**。所以把 ClientHello 切成两半、分别装进两个
Initial 包，审查者解析不到完整 SNI，服务器却能靠 CRYPTO 帧的 offset 重组。

用法：
    python quic-slicing.py --sni www.twitter.com --ip 69.171.235.22
    python quic-slicing.py --sni www.twitter.com --ip 69.171.235.22 --local-test

--local-test 会先在 127.0.0.1 起一个 aioquic 服务端，确认分片能被服务器重组，
再去打真实目标。这样"无回包"才能归因于网络封锁而不是包写错。
"""
from __future__ import annotations

import argparse
import asyncio
import os
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quic_initial_ref as q  # noqa: E402


def varint(n: int) -> bytes:
    if n < 64:
        return bytes([n])
    if n < 16384:
        return struct.pack(">H", 0x4000 | n)
    return struct.pack(">I", 0x80000000 | n)


def split_crypto(payload: bytes) -> tuple[bytes, bytes]:
    """把 Initial 载荷里的 CRYPTO 帧数据切成两半，返回两个新载荷。"""
    if payload[0] != 0x06:
        raise ValueError(f"不是 CRYPTO 帧（首字节 0x{payload[0]:02x}）")
    pos = 1
    # offset
    first = payload[pos]
    if first & 0x80 == 0 and first & 0x40:
        offset = int.from_bytes(payload[pos:pos + 2], "big") & 0x3FFF
        pos += 2
    elif first & 0xC0 == 0x80:
        offset = int.from_bytes(payload[pos:pos + 4], "big") & 0x3FFFFFFF
        pos += 4
    else:
        offset = first
        pos += 1
    # length
    first = payload[pos]
    if first & 0x80 == 0 and first & 0x40:
        ln = int.from_bytes(payload[pos:pos + 2], "big") & 0x3FFF
        pos += 2
    elif first & 0xC0 == 0x80:
        ln = int.from_bytes(payload[pos:pos + 4], "big") & 0x3FFFFFFF
        pos += 4
    else:
        ln = first
        pos += 1
    data = payload[pos:pos + ln]
    mid = len(data) // 2
    p1 = b"\x06" + varint(offset) + varint(mid) + data[:mid]
    p2 = b"\x06" + varint(offset + mid) + varint(len(data) - mid) + data[mid:]
    return p1, p2


def pad(payload: bytes, total: int = 1200) -> bytes:
    """补齐到 total 字节（PADDING 帧就是 0x00）。"""
    if len(payload) < total:
        payload += b"\x00" * (total - len(payload))
    return payload


async def local_server(port: int):
    from aioquic.asyncio import QuicConnectionProtocol, serve
    from aioquic.quic.configuration import QuicConfiguration
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    import datetime

    key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.datetime.now(datetime.timezone.utc)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=30))
            .sign(key, hashes.SHA256()))
    d = tempfile.mkdtemp()
    cp, kp = os.path.join(d, "c.pem"), os.path.join(d, "k.pem")
    open(cp, "wb").write(cert.public_bytes(serialization.Encoding.PEM))
    open(kp, "wb").write(key.private_bytes(serialization.Encoding.PEM,
                                           serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
    conf = QuicConfiguration(is_client=False, alpn_protocols=["h3"])
    conf.load_cert_chain(cp, kp)

    class P(QuicConnectionProtocol):
        def quic_event_received(self, ev):
            print(f"    [local] {type(ev).__name__}", flush=True)

    await serve("127.0.0.1", port, configuration=conf, create_protocol=P)


def make_sliced(sni: str, host: str, port: int) -> list[bytes]:
    ref = q.aioquic_reference(sni, host, port)
    dcid, scid, _, _, _ = q.parse_long_header(ref)
    plain, _, pn_len, pn = q.decrypt(ref, dcid)
    p1, p2 = split_crypto(plain)
    pkt1 = q.build(dcid, scid, pad(p1), pn=int.from_bytes(pn, "big"), pn_len=pn_len)
    pkt2 = q.build(dcid, scid, pad(p2), pn=int.from_bytes(pn, "big") + 1, pn_len=pn_len)
    return [pkt1, pkt2]


def send(ip: str, port: int, packets: list[bytes], wait: float = 5.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(wait)
    try:
        for p in packets:
            s.sendto(p, (ip, port))
            time.sleep(0.05)
        data, addr = s.recvfrom(4096)
        return f"有回包 {len(data)}B from {addr[0]}:{addr[1]}"
    except socket.timeout:
        return "无回包（被丢或服务端拒绝）"
    finally:
        s.close()


async def run_local_test(sni: str):
    await local_server(4439)
    await asyncio.sleep(0.8)
    pkts = make_sliced(sni, "127.0.0.1", 4439)
    print(f"  分片包: {len(pkts)} 个，各 {len(pkts[0])} 字节", flush=True)
    print(f"  本地服务端: {send('127.0.0.1', 4439, pkts)}", flush=True)
    await asyncio.sleep(0.5)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sni", required=True)
    ap.add_argument("--ip", required=True)
    ap.add_argument("--port", type=int, default=443)
    ap.add_argument("--local-test", action="store_true")
    ap.add_argument("--timeout", type=float, default=6.0)
    args = ap.parse_args()

    if args.local_test:
        asyncio.run(run_local_test(args.sni))

    pkts = make_sliced(args.sni, args.ip, args.port)
    print(f"  目标 {args.ip}:{args.port}  SNI={args.sni}", flush=True)
    print(f"  整包（对照）: {send(args.ip, args.port, pkts[:1], args.timeout)}", flush=True)
    print(f"  分片（绕过）: {send(args.ip, args.port, pkts, args.timeout)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
