"""QUIC 握手测试：判断一个域名在当前网络下能不能走 QUIC（h3）。

为什么需要它：GFW 会解密 QUIC Initial（密钥由明文 DCID 派生），读出 SNI 后按
一份**独立的 QUIC 黑名单**丢包。所以同一个域名可能"TCP 被 SNI 封锁、QUIC 却通"
（实测 pixiv 就是这样），也可能反过来（YouTube 的 QUIC 被丢）。

用法：
    python quic-sni-test.py                          # 内置矩阵
    python quic-sni-test.py --sni www.pixiv.net --ip 104.18.42.239
    python quic-sni-test.py --sni www.youtube.com --ip 142.251.150.4 --src-port 9000
    python quic-sni-test.py --json

验证"源端口 > 目的端口"规则（Zohaib et al., USENIX Security '25）：
    python quic-sni-test.py --sni www.youtube.com --ip 142.251.150.4 --src-port 9000   # 源 9000 > 目的 443
    python quic-sni-test.py --sni www.youtube.com --ip 142.251.150.4 --src-port 80     # 源 80 < 目的 443
两次结果不同，说明该规则存在。

依赖：pip install aioquic（不是 Sniroute 的硬依赖，只有这个脚本和 `site --quic` 用到）
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import sys

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
import quic_probe  # noqa: E402

DEFAULT_CASES = [
    ("www.cloudflare.com", "104.16.124.96", "未封锁（对照）"),
    ("www.pixiv.net", "104.18.42.239", "TCP 被 SNI 封锁，QUIC 可能可用"),
    ("www.google.com", "142.250.72.14", "TCP 被 SNI 封锁"),
    ("www.reddit.com", "151.101.129.140", "对照"),
    ("www.youtube.com", "142.251.150.4", "已知 QUIC 黑名单"),
    ("www.vrchat.com", "104.18.6.156", "不支持 h3（对照）"),
]


def one(sni: str, ip: str, port: int, src_port: int, timeout: float, repeat: int) -> dict:
    results = []
    for _ in range(max(1, repeat)):
        with contextlib.redirect_stderr(io.StringIO()):
            results.append(quic_probe.quic_handshake(sni, ip, port, src_port, timeout))
    ok = sum(1 for r in results if r["ok"])
    best = min((r for r in results if r["ok"]), key=lambda r: r["elapsed"], default=results[0])
    out = dict(best)
    out["ok"] = ok > 0
    out["reason"] = best["reason"] if ok else results[-1]["reason"]
    out["success_rate"] = f"{ok}/{len(results)}"
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sni", default="")
    ap.add_argument("--ip", default="")
    ap.add_argument("--port", type=int, default=443)
    ap.add_argument("--src-port", type=int, default=0, help="固定本地源端口（0=随机）")
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    try:
        import aioquic  # noqa: F401
    except ImportError:
        print("缺少 aioquic：pip install aioquic", file=sys.stderr)
        return 2

    cases = ([(args.sni, args.ip, "自定义")] if args.sni and args.ip else DEFAULT_CASES)
    rows = []
    for sni, ip, tag in cases:
        r = one(sni, ip, args.port, args.src_port, args.timeout, args.repeat)
        rows.append({"sni": sni, "ip": ip, "tag": tag, **r})

    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=2))
        return 0

    for r in rows:
        print(f"  {r['tag']:<28} SNI={r['sni']:<22} "
              f"{quic_probe.describe(r)}  成功率={r['success_rate']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
