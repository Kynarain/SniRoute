#!/usr/bin/env python3
"""DoH 服务的自检：发一个 wire 格式查询，把应答解出来看看对不对。

不依赖 dig / nslookup —— 它们不一定会说 DoH，也不一定在 Windows 上装好。

用法：
    python tools/doh_selftest.py --url http://127.0.0.1:5453/dns-query --name www.pixiv.net --type 65
"""

from __future__ import annotations

import argparse
import base64
import ipaddress
import ssl
import sys
import urllib.request


def encode_query(name: str, qtype: int, txid: int = 0x1234) -> bytes:
    header = txid.to_bytes(2, "big") + b"\x01\x00" + b"\x00\x01" + b"\x00" * 6
    qname = b"".join(bytes([len(p)]) + p.encode() for p in name.split(".") if p) + b"\x00"
    return header + qname + qtype.to_bytes(2, "big") + (1).to_bytes(2, "big")


def skip_name(data: bytes, off: int) -> int:
    while True:
        n = data[off]
        if n == 0:
            return off + 1
        if n & 0xC0:
            return off + 2
        off += 1 + n


def parse(data: bytes) -> tuple:
    ancount = int.from_bytes(data[6:8], "big")
    off = 12
    off = skip_name(data, off) + 4          # 跳过 question
    out = []
    for _ in range(ancount):
        off = skip_name(data, off)
        rtype = int.from_bytes(data[off:off + 2], "big")
        ttl = int.from_bytes(data[off + 4:off + 8], "big")
        rdlen = int.from_bytes(data[off + 8:off + 10], "big")
        rdata = data[off + 10:off + 10 + rdlen]
        off += 10 + rdlen
        out.append((rtype, ttl, rdata))
    return ancount, out


def describe(rtype: int, rdata: bytes) -> str:
    if rtype == 1:
        return str(ipaddress.IPv4Address(rdata))
    if rtype == 28:
        return str(ipaddress.IPv6Address(rdata))
    if rtype == 65:
        # priority(2) + target(1=根) + 若干 SVCB 参数
        prio = int.from_bytes(rdata[0:2], "big")
        off = 3
        parts = [f"priority={prio}"]
        names = {1: "alpn", 3: "port", 4: "ipv4hint", 5: "ech", 6: "ipv6hint"}
        while off + 4 <= len(rdata):
            key = int.from_bytes(rdata[off:off + 2], "big")
            ln = int.from_bytes(rdata[off + 2:off + 4], "big")
            val = rdata[off + 4:off + 4 + ln]
            key_name = names.get(key, f"key{key}")
            if key == 1:
                alpn, i = [], 0
                while i < len(val):
                    ln2 = val[i]
                    alpn.append(val[i + 1:i + 1 + ln2].decode("ascii", "replace"))
                    i += 1 + ln2
                parts.append(f"alpn={','.join(alpn)}")
            elif key == 4:
                parts.append("ipv4hint=" + ",".join(
                    str(ipaddress.IPv4Address(val[i:i + 4])) for i in range(0, len(val), 4)))
            elif key == 5:
                parts.append(f"ech={base64.b64encode(val).decode()} ({len(val)} 字节)")
            else:
                parts.append(f"{key_name}={val.hex()[:40]}")
            off += 4 + ln
        return " ".join(parts)
    return rdata.hex()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:5453/dns-query")
    ap.add_argument("--name", default="www.pixiv.net")
    ap.add_argument("--type", type=int, default=65)
    ap.add_argument("--cafile", help="用这个根证书校验证书链（自签场景用它，别用 -k）")
    ap.add_argument("--insecure", action="store_true", help="不校验证书（仅调试）")
    args = ap.parse_args()

    query = encode_query(args.name, args.type)
    req = urllib.request.Request(
        args.url, data=query,
        headers={"content-type": "application/dns-message", "accept": "application/dns-message"})

    ctx = None
    if args.cafile or args.insecure:
        ctx = ssl.create_default_context(cafile=args.cafile)
        if args.insecure:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx)) if ctx else None

    try:
        resp = opener.open(req, timeout=10) if opener else urllib.request.urlopen(req, timeout=10)
        with resp:
            data = resp.read()
    except Exception as exc:
        print(f"请求失败：{exc}", file=sys.stderr)
        raise SystemExit(1)

    ancount, answers = parse(data)
    print(f"查询 {args.name} type={args.type} → {ancount} 条应答")
    if not answers:
        print("  （空应答：NOERROR / 无记录）")
    for rtype, ttl, rdata in answers:
        print(f"  type={rtype} ttl={ttl}  {describe(rtype, rdata)}")


if __name__ == "__main__":
    main()
