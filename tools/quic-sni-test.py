"""用 aioquic 的完整 QUIC 栈测试 GFW 对 SNI 的处理。

为什么不用手拼包：手拼的 Initial 容易被服务端静默丢弃，导致
"无回包"无法区分是"包不合法"还是"被墙丢包"。aioquic 是成熟实现，
握手成功/超时能明确反映网络是否放行。

用法：
    python quic-sni-test.py                          # 默认实验集
    python quic-sni-test.py --sni www.pixiv.net --ip 104.18.42.239
"""
import argparse
import asyncio
import ssl
import sys
import time

from aioquic.asyncio import connect
from aioquic.quic.configuration import QuicConfiguration


async def try_connect(sni: str, ip: str, port: int = 443, timeout: float = 8.0):
    conf = QuicConfiguration(is_client=True, alpn_protocols=["h3"])
    conf.server_name = sni
    conf.verify_mode = ssl.CERT_NONE
    t0 = time.time()
    try:
        async with asyncio.timeout(timeout):
            async with connect(ip, port, configuration=conf) as client:
                dt = time.time() - t0
                proto = client._quic
                return f"[OK] 握手成功 ({dt:.2f}s, peer={ip})"
    except asyncio.TimeoutError:
        return "[TIMEOUT] 超时（无 ServerHello——被丢包）"
    except Exception as exc:
        name = type(exc).__name__
        msg = str(exc)[:80]
        return f"[FAIL] {name}: {msg}"


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sni", default="")
    ap.add_argument("--ip", default="")
    args = ap.parse_args()

    if args.sni and args.ip:
        print(f"  SNI={args.sni:<24} -> {await try_connect(args.sni, args.ip)}")
        return

    cases = [
        ("www.cloudflare.com", "104.16.124.96", "未封锁（对照）"),
        ("www.pixiv.net", "104.18.42.239", "已封锁（QUIC 黑名单）"),
        ("www.google.com", "142.250.72.14", "已封锁（对照）"),
        ("www.vrchat.com", "104.18.6.156", "不支持 h3（对照）"),
    ]
    for sni, ip, tag in cases:
        res = await try_connect(sni, ip)
        print(f"  {tag:<26} SNI={sni:<22} {res}")


if __name__ == "__main__":
    asyncio.run(main())
