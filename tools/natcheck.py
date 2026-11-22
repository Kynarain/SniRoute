#!/usr/bin/env python3
"""natcheck.py -- NAT 类型检测（RFC 5780）

判定思路与 I:\\test\\ip-nat-cs 的 C# 实现一致：

    映射行为（同一本地端口打不同目标，公网映射是否变化）
      × 过滤行为（外部能否主动从别的地址/端口打进来）
      → 锥型细分 / 对称型 / 公网直连

为什么用这三个维度而不是"猜一个 NAT 类型"：NAT 类型（全锥/受限锥/端口受限锥/对称）
是这两条行为组合出来的结果，单独看映射或单独看过滤都会误判。

STUN 服务器默认用小米的（stun.miwifi.com），因为 Google 那个在本网络解析到 127.0.0.1
（DNS 被污染），Cloudflare 与 Nextcloud 作为备选。

用法：
    python natcheck.py                        # 人类可读报告
    python natcheck.py --summary              # 一行结论（托盘气泡用）
    python natcheck.py --json                 # 机器可读
    python natcheck.py --server stun.cloudflare.com:3478
"""

import argparse
import json
import os
import socket
import ssl
import struct
import sys
import time
import urllib.request

MAGIC = 0x2112A442
BINDING_REQUEST = 0x0001
BINDING_SUCCESS = 0x0101
BINDING_ERROR = 0x0111

ATTR_MAPPED = 0x0001
ATTR_CHANGE_REQUEST = 0x0003
ATTR_ERROR = 0x0009
ATTR_XOR_MAPPED = 0x0020
ATTR_RESPONSE_ORIGIN = 0x802B
ATTR_OTHER_ADDRESS = 0x802C

CHANGE_IP = 0x04
CHANGE_PORT = 0x02

DEFAULT_SERVER = "stun.miwifi.com:3478"
FALLBACK_SERVERS = ["stun.cloudflare.com:3478", "stun.nextcloud.com:3478", "stun.miwifi.com:3478"]

# 公网 IP 反查：多源容错。这些域名在本网络未必都能解析，失败就跳过（不影响 STUN 主流程）。
IP_SOURCES = [
    ("api.ipify.org", "/"),
    ("ipv4.icanhazip.com", "/"),
    ("checkip.amazonaws.com", "/"),
    ("ifconfig.me", "/ip"),
    ("api.ip.sb", "/ip"),
]

CA_FILE = r"C:\Users\kynar\AppData\Local\Sniroute\ca\rootCA.crt"


# --------------------------------------------------------------------------
# STUN
# --------------------------------------------------------------------------

def new_txn() -> bytes:
    return os.urandom(12)


def build_binding(txn: bytes, change: int = None) -> bytes:
    body = b""
    if change is not None:
        body += struct.pack(">HHI", ATTR_CHANGE_REQUEST, 4, change)
    head = struct.pack(">HH", BINDING_REQUEST, 0) + struct.pack(">I", MAGIC) + txn
    msg = head + body
    return msg[:2] + struct.pack(">H", len(msg) - 20) + msg[4:]


def parse_attrs(data: bytes):
    """返回 [(type, value_bytes)]；同时返回消息类型。"""
    if len(data) < 20 or struct.unpack(">I", data[4:8])[0] != MAGIC:
        return None, []
    mtype = struct.unpack(">H", data[0:2])[0]
    mlen = struct.unpack(">H", data[2:4])[0]
    body = data[20:20 + mlen]
    out = []
    i = 0
    while i + 4 <= len(body):
        atype, alen = struct.unpack(">HH", body[i:i + 4])
        i += 4
        val = body[i:i + alen]
        i += alen + ((4 - alen % 4) % 4)
        out.append((atype, val))
    return mtype, out


def decode_endpoint(val: bytes, xor: bool, txn: bytes):
    if len(val) < 8:
        return None
    family = val[1]
    port = struct.unpack(">H", val[2:4])[0]
    if xor:
        port ^= (MAGIC >> 16)
    if family == 0x01:
        raw = val[4:8]
        if xor:
            key = struct.pack(">I", MAGIC)
            raw = bytes(a ^ b for a, b in zip(raw, key))
        return socket.inet_ntop(socket.AF_INET, raw), port
    if family == 0x02 and len(val) >= 20:
        raw = val[4:20]
        if xor:
            key = struct.pack(">I", MAGIC) + txn
            raw = bytes(a ^ b for a, b in zip(raw, key))
        return socket.inet_ntop(socket.AF_INET6, raw), port
    return None


def probe(sock, server, timeout, change=None):
    """发一次 Binding，返回 dict 或 None。复用同一个 socket 以保持本地端口不变。"""
    txn = new_txn()
    try:
        sock.sendto(build_binding(txn, change), server)
    except OSError as exc:
        return {"error": f"send: {exc}"}

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sock.settimeout(max(0.2, deadline - time.monotonic()))
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            return {"error": "timeout"}
        except OSError as exc:
            return {"error": f"recv: {exc}"}
        if data[8:20] != txn:      # 事务 ID 不符，不是我们的回复
            continue
        mtype, attrs = parse_attrs(data)
        if mtype is None:
            continue
        res = {"type": mtype, "from": addr, "mapped": None, "other": None, "origin": None}
        for atype, val in attrs:
            if atype == ATTR_XOR_MAPPED:
                res["mapped"] = decode_endpoint(val, True, txn)
            elif atype == ATTR_MAPPED and res["mapped"] is None:
                res["mapped"] = decode_endpoint(val, False, txn)
            elif atype == ATTR_OTHER_ADDRESS:
                res["other"] = decode_endpoint(val, False, txn)
            elif atype == ATTR_RESPONSE_ORIGIN:
                res["origin"] = decode_endpoint(val, False, txn)
            elif atype == ATTR_ERROR:
                if len(val) >= 4:
                    code = (val[2] & 0x07) * 100 + val[3]
                    res["error_code"] = code
        return res
    return {"error": "timeout"}


def resolve_host(name: str):
    """系统解析优先；失败或得到不可用结果时，用本机 DoH 再解析一次。"""
    try:
        return socket.gethostbyname(name), "system"
    except OSError:
        pass
    if not os.path.isfile(CA_FILE):
        return None, None
    try:
        ctx = ssl.create_default_context(cafile=CA_FILE)
        q = b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\x00"
        req = urllib.request.Request(
            "https://127.0.0.1:5444/dns-query",
            data=struct.pack(">HHHHHH", 0x4242, 0x0100, 1, 0, 0, 0) + q + struct.pack(">HH", 1, 1),
            headers={"Content-Type": "application/dns-message", "Accept": "application/dns-message"})
        with urllib.request.urlopen(req, timeout=6, context=ctx) as resp:
            d = resp.read()
        an = struct.unpack(">H", d[6:8])[0]
        i = 12
        while d[i]:
            i += d[i] + 1
        i += 5
        for _ in range(an):
            if d[i] & 0xC0 == 0xC0:
                i += 2
            else:
                while d[i]:
                    i += d[i] + 1
                i += 1
            rtype, _cls, _ttl, rlen = struct.unpack(">HHIH", d[i:i + 10])
            i += 10
            rdata = d[i:i + rlen]
            i += rlen
            if rtype == 1 and rlen == 4:
                return socket.inet_ntop(socket.AF_INET, rdata), "doh"
    except Exception:
        pass
    return None, None


def parse_target(spec: str):
    s = (spec or "").strip()
    if s.lower().startswith("stun:"):
        s = s[5:]
    host, _, port = s.rpartition(":")
    if not host:
        host, port = s, "3478"
    return host, int(port)


# --------------------------------------------------------------------------
# 公网 IP 反查（可选）
# --------------------------------------------------------------------------

def http_public_ip(timeout: float):
    for host, path in IP_SOURCES:
        try:
            ctx = ssl.create_default_context()
            req = urllib.request.Request(f"https://{host}{path}", headers={"User-Agent": "curl/8"})
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
                text = resp.read(64).decode("ascii", "ignore").strip()
            if text and text[0].isdigit():
                return text, host
        except Exception:
            continue
    return None, None


# --------------------------------------------------------------------------
# 判定
# --------------------------------------------------------------------------

def judge_mapping(a, b):
    if not a or not b:
        return "unknown"
    if a == b:
        return "EIM"          # endpoint-independent
    if a[0] == b[0]:
        return "APDM"         # address-port dependent
    return "ADM"              # address dependent


def classify(mapping, filtering, mapped, outbound):
    if not mapped:
        return "unknown"
    if outbound and mapped[0] == outbound:
        return "open"          # 公网直连（映射地址就是本机地址）
    if mapping in ("ADM", "APDM"):
        return "symmetric"
    if filtering == "EIF":
        return "full-cone"
    if filtering == "ADF":
        return "restricted-cone"
    if filtering == "APDF":
        return "port-restricted"
    if filtering == "inconsistent":
        return "inconsistent"
    # 过滤行为测不出来（服务器不支持 CHANGE-REQUEST / 丢包）时，不要假装知道。
    # 映射行为已经能确定是锥型还是对称，但"哪一种锥型"需要过滤行为的证据。
    # 早期版本在这里硬猜 port-restricted，结果同一台机器换个 STUN 服务器
    # 就变成 full-cone —— 用户看到的"结果不稳定"就是这么来的。
    return "cone-unknown"


LABELS = {
    "full-cone": "全锥型 NAT（Full Cone）",
    "restricted-cone": "受限锥型 NAT（Restricted Cone）",
    "port-restricted": "端口受限锥型 NAT（Port-Restricted Cone）",
    "symmetric": "对称型 NAT（Symmetric）",
    "cone-unknown": "锥型 NAT（具体类型无法判定：服务器不支持 CHANGE-REQUEST 或丢包）",
    "inconsistent": "锥型 NAT（过滤行为多轮不一致：可能位于多网关 CGNAT）",
    "open": "公网直连（无 NAT 或已完全映射）",
    "unknown": "无法判定",
}

MAP_LABELS = {"EIM": "端点无关（映射稳定）", "APDM": "地址端口相关", "ADM": "地址相关（对称）", "unknown": "未知", "inconsistent": "多轮不一致"}
FILT_LABELS = {"EIF": "端点无关（全锥）", "ADF": "地址相关", "APDF": "地址端口相关", "unsupported": "服务器不支持 RFC 5780", "unknown": "未知"}


def detect(server_spec, server2, timeout, do_turn=False):
    report = {"server": server_spec, "timeout_ms": timeout, "logs": [], "bindings": []}

    def log(msg):
        report["logs"].append(msg)

    host, port = parse_target(server_spec)
    ip, how = resolve_host(host)
    if not ip:
        report.update(kind="unknown", reason=f"无法解析 STUN 服务器 {host}")
        log(report["reason"])
        return report
    report["server_ip"] = ip
    report["resolved_by"] = how
    log(f"STUN 服务器 {host}:{port} -> {ip}（{how}）")

    server = (ip, port)

    # 本地出口地址：连一次 UDP 让系统选路
    outbound = None
    try:
        tmp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tmp.connect(server)
        outbound = tmp.getsockname()[0]
        tmp.close()
    except OSError:
        pass
    report["local_outbound"] = outbound
    log(f"本地出口地址 {outbound or '未知'}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    local_port = sock.getsockname()[1]
    report["local_port"] = local_port

    # 1) 三次绑定，拿映射地址与 OTHER-ADDRESS
    mapped_a = None
    other = None
    for i in range(3):
        r = probe(sock, server, timeout)
        if r is None or "error" in r:
            # 单次超时可能只是丢包：再试两次再决定是不是真的不通。
            for _retry in range(2):
                r = probe(sock, server, timeout)
                if r and "error" not in r:
                    break
        if r is None or "error" in r:
            log(f"绑定 {i + 1}: 失败（{r.get('error') if r else 'unknown'}）")
            break
        report["bindings"].append({"mapped": r["mapped"], "rtt": None})
        mapped_a = mapped_a or r["mapped"]
        other = other or r["other"]
        log(f"绑定 {i + 1}: 映射 = {r['mapped']}")

    if not mapped_a:
        report.update(kind="blocked", reason=f"STUN 无响应（{host}:{port}）→ UDP 出不去或被阻断")
        log(report["reason"])
        sock.close()
        return report

    # 2) 同一本地端口打第二个目标 -> 判断映射行为
    alts = []
    if other and other != server:
        alts.append(other)
        log(f"服务器通告 OTHER-ADDRESS: {other}")
    if server2:
        h2, p2 = parse_target(server2)
        ip2, _ = resolve_host(h2)
        if ip2:
            alts.append((ip2, p2))
    else:
        for spec in FALLBACK_SERVERS:
            h2, p2 = parse_target(spec)
            ip2, _ = resolve_host(h2)
            if ip2 and (ip2, p2) != server:
                alts.append((ip2, p2))

    mapped_b = None
    for target in alts[:3]:
        r = probe(sock, target, timeout)
        if r and "error" not in r and r.get("mapped"):
            mapped_b = r["mapped"]
            log(f"对比目标 {target} 的映射 = {mapped_b}")
            break
        log(f"对比目标 {target} 无响应（{(r or {}).get('error')}）")

    mapping = judge_mapping(mapped_a, mapped_b)
    report["mapped_a"] = list(mapped_a) if mapped_a else None
    report["mapped_b"] = list(mapped_b) if mapped_b else None
    report["mapping"] = mapping
    log(f"映射行为：{MAP_LABELS[mapping]}")

    # 3) 过滤行为：CHANGE-REQUEST（RFC 5780 Test III / Test II）
    #
    # 判据的核心是收到来自“我们从未发过包的地址”的回包。这在 NAT 语义里只有
    # 一个解释：该地址的入向包被放行了 —— 即过滤行为是端点无关（全锥）。
    #
    # 之前的版本要求“回包地址必须等于服务器通告的 OTHER-ADDRESS”，这是错的：
    #   1) 首次绑定时服务器可能没回（丢包），other 为 None，判据直接失效；
    #   2) 服务器回包端口与通告端口未必一致，比较端口会让全锥被判成无法判定。
    # 实测这台机器：server 111.206.174.3:3478，change=IP+PORT 后从
    # 111.206.174.2:3479 收到合法回包 —— 明显是全锥型。
    #
    # 另一个关键点：成功是强证据，失败是弱证据。
    # CHANGE-REQUEST 丢包很常见，所以不能“多数轮次说了算”；只要有一轮拿到
    # 不同 IP 的合法回包，就足以定 EIF。
    eif = False
    adf = False
    rounds = []
    for _round in range(5):
        r3 = probe(sock, server, timeout, change=CHANGE_IP | CHANGE_PORT)
        if r3 and "error" not in r3 and r3["type"] == BINDING_SUCCESS:
            src = r3["from"]
            rounds.append(f"III->{src[0]}:{src[1]}")
            if src[0] != server[0]:
                eif = True
                log(f"第 {_round + 1} 轮：收到来自 {src[0]}:{src[1]} 的回包"
                    f"（服务器是 {server[0]}:{server[1]}）→ 过滤行为端点无关")
                break
            continue
        r2 = probe(sock, server, timeout, change=CHANGE_PORT)
        if r2 and "error" not in r2 and r2["type"] == BINDING_SUCCESS:
            src = r2["from"]
            rounds.append(f"II->{src[0]}:{src[1]}")
            if src[0] != server[0]:
                eif = True
                log(f"第 {_round + 1} 轮：换端口后从 {src[0]}:{src[1]} 回包 → 端点无关")
                break
            if src[1] != server[1]:
                adf = True
        else:
            rounds.append("无响应")

    if eif:
        filtering = "EIF"
    elif adf:
        filtering = "ADF"
    else:
        filtering = "unknown"
    log(f"过滤行为：{FILT_LABELS.get(filtering, filtering)}（取样 {', '.join(rounds) or '无'}）")

    report["filtering"] = filtering
    log(f"过滤行为：{FILT_LABELS.get(filtering, filtering)}"
        + ("" if filtering != "unknown" else "（服务器不回 CHANGE-REQUEST 或丢包，无法判定）"))

    # 公网映射与本地出口相同、且端口一致 -> 没有 NAT（或已 DMZ/直连）。
    if outbound and mapped_a and str(mapped_a[0]) == str(outbound):
        kind = "open"
    else:
        kind = classify(mapping, filtering, mapped_a, outbound)
    report["kind"] = kind
    report["label"] = LABELS[kind]
    report["reason"] = (f"映射行为 {MAP_LABELS[mapping]}，过滤行为 {FILT_LABELS.get(filtering, filtering)}"
                        f"，公网映射 {mapped_a[0]}:{mapped_a[1]}")
    log(f"结论：{report['label']}")

    sock.close()

    # 4) 可选：HTTP 反查公网 IP，用于交叉验证映射地址
    http_ip, src = http_public_ip(min(3.0, timeout))
    report["http_ip"] = http_ip
    report["http_source"] = src
    if http_ip:
        log(f"HTTP 反查公网 IP：{http_ip}（{src}）")
        report["ip_consistent"] = (http_ip == mapped_a[0])
        if not report["ip_consistent"]:
            # 实测本机就遇到过：HTTP 走系统代理、UDP 直连，两个出口不同。
            # 这不算错误，但要在报告里说清楚，否则读者会以为映射地址取错了。
            log(f"注意：HTTP 出口（{http_ip}）与 STUN 映射地址（{mapped_a[0]}）不同，"
                "说明 HTTP 走了代理/VPN，而 UDP 是直连；NAT 结论以 STUN 为准")
    else:
        report["ip_consistent"] = None

    return report


def main():
    ap = argparse.ArgumentParser(description="NAT 类型检测（RFC 5780）")
    ap.add_argument("--server", default=DEFAULT_SERVER, help=f"STUN 服务器，默认 {DEFAULT_SERVER}")
    ap.add_argument("--server2", default=None, help="用于判定映射行为的第二个 STUN 服务器")
    ap.add_argument("--timeout", type=int, default=3000, help="单次探测超时（毫秒）")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--summary", action="store_true", help="只输出一行结论")
    ap.add_argument("--quiet", action="store_true", help="不打印过程日志")
    args = ap.parse_args()

    timeout_s = max(500, args.timeout) / 1000.0

    # 多服务器回退：公共 STUN 经常时通时不通（实测 stun.miwifi 两次进不去），
    # 只试一个会把"服务器抽风"报成"UDP 被阻断"。
    candidates = [args.server]
    if args.server == DEFAULT_SERVER:
        for spec in FALLBACK_SERVERS:
            if spec not in candidates:
                candidates.append(spec)

    report = None
    tried = []
    for spec in candidates:
        r = detect(spec, args.server2, timeout_s)
        tried.append(spec)
        report = r
        if r.get("kind") not in (None, "blocked", "unknown"):
            break
        if len(candidates) > 1:
            r["logs"].append(f"（{spec} 没拿到结果，换下一个 STUN 服务器）")

    if report is None:
        report = {"kind": "unknown", "label": "无法判定", "logs": ["没有可用的 STUN 服务器"], "reason": "没有可用的 STUN 服务器"}
    report["servers_tried"] = tried

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0 if report.get("kind") not in (None, "unknown") else 1

    if args.summary:
        print(f"{report.get('label', '无法判定')}  [{report.get('reason', '')}]")
        return 0 if report.get("kind") not in (None, "unknown") else 1

    if not args.quiet:
        for line in report["logs"]:
            print(f"  {line}")
    print()
    print(f"  NAT 类型 : {report.get('label', '无法判定')}")
    print(f"  映射地址 : {report.get('mapped_a')}")
    if report.get("mapped_b"):
        print(f"  对比映射 : {report.get('mapped_b')}")
    if report.get("http_ip"):
        print(f"  HTTP 反查: {report['http_ip']}（{report['http_source']}）")
    print(f"  依据     : {report.get('reason', '')}")
    return 0 if report.get("kind") not in (None, "unknown") else 1


if __name__ == "__main__":
    sys.exit(main())
