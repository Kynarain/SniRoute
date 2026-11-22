#!/usr/bin/env python3
"""把 wgcf 生成的 WireGuard 配置转成 Clash Meta 的 YAML 片段。

为什么需要这个脚本：
  Cloudflare WARP 官方客户端**不导出**任何配置；能拿到参数的途径只有 wgcf
  （它注册一个真实的 WARP 账号，输出标准 WireGuard 的 wgcf-profile.conf）。
  而 Clash Meta 用的是自己的 YAML 结构，字段名和 .conf 不一样，手抄容易漏字段
  （漏了 reserved/remote-dns-resolve 就会"显示已连接但没网"）。

用法：
    # 1) 注册并生成 WireGuard 配置（只需一次）
    winget install ViRb3.wgcf
    wgcf register --accept-tos
    wgcf generate                 # -> wgcf-profile.conf

    # 2) 转成 Clash YAML
    python tools/wgcf-to-clash.py wgcf-profile.conf -o warp-clash.yaml

    # 也可以直接读 wgcf 账号目录里的默认文件
    python tools/wgcf-to-clash.py --auto

输出是一个**可直接导入**的完整 Clash 配置（含 DNS 与规则），不是片段。
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# 国内可用的 WARP 入口。Cloudflare 的 anycast 会挑一个就近的 colo，
# 这几个只是不同的入口 IP；出口机房不由我们决定（实测通常在 LAX/SJC/HKG 之间）。
DEFAULT_ENDPOINTS = [
    ("162.159.192.1", 2408),
    ("162.159.193.10", 2408),
]


def parse_conf(text: str) -> dict:
    """从 wgcf-profile.conf 里取 Interface/Peer 字段。"""
    def grab(key: str) -> str:
        m = re.search(rf"^\s*{key}\s*=\s*(.+)$", text, re.MULTILINE)
        return m.group(1).strip() if m else ""

    addr = grab("Address")
    ipv4 = ""
    ipv6 = ""
    for part in addr.split(","):
        part = part.strip()
        if not part:
            continue
        if ":" in part:
            ipv6 = part.split("/")[0]
        else:
            ipv4 = part.split("/")[0]

    return {
        "private_key": grab("PrivateKey"),
        "public_key": grab("PublicKey"),
        "ipv4": ipv4,
        "ipv6": ipv6,
        "endpoint": grab("Endpoint"),
        "mtu": grab("MTU") or "1280",
        "dns": grab("DNS"),
    }


def build_yaml(cfg: dict, endpoints: list[tuple[str, int]]) -> str:
    if not cfg["private_key"] or not cfg["public_key"]:
        raise SystemExit("配置里缺 PrivateKey / PublicKey，确认这是 wgcf-profile.conf")

    proxies = []
    for idx, (host, port) in enumerate(endpoints, 1):
        name = "WARP" if idx == 1 else f"WARP-{idx}"
        block = [
            f"  - name: {name}",
            "    type: wireguard",
            f"    server: {host}",
            f"    port: {port}",
            f"    ip: {cfg['ipv4']}",
        ]
        if cfg["ipv6"]:
            block.append(f"    ipv6: {cfg['ipv6']}")
        block += [
            f'    private-key: "{cfg["private_key"]}"',
            f'    public-key: "{cfg["public_key"]}"',
            "    udp: true",
            f"    mtu: {cfg['mtu']}",
            # 关键：DNS 必须走隧道内解析，否则 Clash 会用本地（可能被污染的）DNS，
            # 表现为"能连上但域名解析错"。
            "    remote-dns-resolve: true",
            "    dns: [1.1.1.1, 1.0.0.1]",
        ]
        proxies.append("\n".join(block))

    proxy_names = ", ".join(
        ("WARP" if i == 1 else f"WARP-{i}") for i in range(1, len(endpoints) + 1)
    )
    return f"""# Cloudflare WARP for Clash Meta —— 由 tools/wgcf-to-clash.py 生成
# 账号来源：wgcf register（真实 WARP 账号，私钥只在本文件里，别外传）
mixed-port: 7890
allow-lan: false
mode: rule
log-level: info
ipv6: false

dns:
  enable: true
  ipv6: false
  enhanced-mode: fake-ip
  fake-ip-range: 198.18.0.1/16
  nameserver:
    - 223.5.5.5
    - 119.29.29.29
  # 解析 WARP 端点用的解析器；不能走代理，否则会绕回自己
  proxy-server-nameserver:
    - 223.5.5.5

proxies:
{chr(10).join(proxies)}

proxy-groups:
  - name: PROXY
    type: fallback
    proxies: [{proxy_names}]
    url: https://www.gstatic.com/generate_204
    interval: 300

rules:
  - MATCH,PROXY
"""


def main() -> int:
    ap = argparse.ArgumentParser(
        description="wgcf-profile.conf -> Clash Meta YAML",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("conf", nargs="?", help="wgcf-profile.conf 路径（或用 --auto）")
    ap.add_argument("--auto", action="store_true",
                    help="自动找 %%TEMP%%\\wgcf\\wgcf-profile.conf 或当前目录")
    ap.add_argument("-o", "--output", default="warp-clash.yaml")
    ap.add_argument("--endpoint", action="append", default=[],
                    metavar="IP:PORT", help="可重复；不给则用内置的两个默认端点")
    args = ap.parse_args()

    if args.auto:
        candidates = [
            Path.home() / "AppData" / "Local" / "Temp" / "wgcf" / "wgcf-profile.conf",
            Path.cwd() / "wgcf-profile.conf",
        ]
        found = next((p for p in candidates if p.is_file()), None)
        if not found:
            print("找不到 wgcf-profile.conf，先跑：wgcf register --accept-tos && wgcf generate")
            return 1
        conf_path = found
    elif args.conf:
        conf_path = Path(args.conf)
    else:
        ap.print_help()
        return 2

    if not conf_path.is_file():
        print(f"文件不存在：{conf_path}")
        return 1

    endpoints = []
    for raw in args.endpoint:
        host, _, port = raw.partition(":")
        try:
            endpoints.append((host, int(port or 2408)))
        except ValueError:
            print(f"端点格式不对：{raw}（应为 IP:PORT）")
            return 1
    if not endpoints:
        endpoints = DEFAULT_ENDPOINTS

    cfg = parse_conf(conf_path.read_text(encoding="utf-8", errors="replace"))
    yaml = build_yaml(cfg, endpoints)
    out = Path(args.output)
    out.write_text(yaml, encoding="utf-8")
    print(f"已生成 {out}  ({len(yaml)} 字节)")
    print(f"  端点   : {', '.join(f'{h}:{p}' for h, p in endpoints)}")
    print(f"  隧道 IP: {cfg['ipv4']}")
    print(f"  MTU    : {cfg['mtu']}")
    print("导入方式：Clash 客户端 -> 配置 -> 从文件导入")
    return 0


if __name__ == "__main__":
    sys.exit(main())
