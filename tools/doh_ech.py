#!/usr/bin/env python3
"""本地 DoH 服务：让浏览器用原生 ECH 绕过 SNI 阻断。

思路来自一篇实践记录：**不需要自己实现 ECH 客户端**。浏览器已经实现了，我们只要
把它需要的东西供给它——

  1. A 记录     → 指向 Cloudflare 边缘（而不是被污染/被封锁的地址）
  2. AAAA 记录  → 空（pixiv 的 IPv6 会返回 403 block_ip，必须过滤）
  3. HTTPS(65)  → 带 ech= 的 SVCB 记录，浏览器据此把真实 SNI 加密进内层

于是 GFW 在明文里只看到外层 SNI（cloudflare-ech.com），匹配不到 pixiv.net。

前提：浏览器要把安全 DNS 指向本服务，并且信任签发本服务证书的根 CA（本项目自签）。

不做的事：不实现 ECH（浏览器做）、不代理网页流量（浏览器直连 CF 边缘）。

用法：
    # 1) 给 DoH 端点签一张证书（用本项目自己的 CA）
    openssl req -new -nodes -newkey rsa:2048 -sha256 \
        -keyout conf/certs/doh.key -out temp/doh.csr -subj "/O=Sniroute/CN=doh.sniroute.lan"
    printf "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:doh.sniroute.lan\n" > temp/doh.ext
    openssl x509 -req -in temp/doh.csr -CA ca/rootCA.crt -CAkey ca/rootCA.key \
        -CAcreateserial -days 825 -sha256 -extfile temp/doh.ext -out conf/certs/doh.crt

    # 2) 启动（hosts 里加 127.0.0.1 doh.sniroute.lan）
    python tools/doh_ech.py --listen 127.0.0.1:5443

    # 3) 浏览器：chrome://flags 开启 Encrypted ClientHello，
    #    设置 → 隐私和安全 → 安全 → 使用安全 DNS → 自定义 → https://doh.sniroute.lan:5443/dns-query
"""

from __future__ import annotations

import argparse
import socket
import base64
import ipaddress
import json
import socketserver
import ssl
import sys
import urllib.request
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
SITES = ROOT / "data" / "sites.json"
ECH_CFG = ROOT / "data" / "cloudflare-ech.json"

TYPE_A, TYPE_AAAA, TYPE_HTTPS = 1, 28, 65

# 查询日志路径。排查"浏览器到底有没有连到我"时，这是唯一可靠的证据：
# 没有日志就只能靠猜，而猜错方向会白白怀疑一堆无关的东西。
LOG_PATH = None

# 兜底上游：不归我们管的域名原样转发，否则浏览器一旦用上这个 DoH 就哪里都上不去。
# 多个端点轮着试：实测 doh.pub 会偶发 RST（WinError 10054），单端点会导致
# 整个浏览器时好时坏，而症状看起来像"ECH 没生效"，极难定位。
FORWARD_DOHS = [
    # 阿里云放第一位：实测 0.11s（钉 IP 后），doh.pub 约 0.8s。
    "https://dns.alidns.com/dns-query",
    "https://doh.pub/dns-query",
    # dns.google 在 WARP 关闭时必然超时；放在最后，并靠下面的冷却机制跳过，
    # 否则每次查询都要白等一个超时才轮到它。
    "https://dns.google/dns-query",
]

# 端点失败后的冷却（秒）：连续失败 2 次就暂时不再尝试。
# 没有它，"默认关闭 WARP"时每次都先从被墙端点开始等，整体延迟被放大。
FORWARD_COOLDOWN = 600.0
_FORWARD_FAIL: dict = {}

# 上游域名 -> 固定 IP。
#
# 为什么必须写死：本后端自己也在 53 端口提供明文 DNS（全局模式下 NRPT 指向
# 127.0.0.1:53）。如果转发时用域名（doh.pub 等），urllib 会做一次系统解析，
# 这个解析又回到自己的 53 端口 -> 再次进入 forward() -> **递归**，只能等超时。
# 实测表现：每个新查询卡满 3×4 秒，之后靠缓存才快。
# 直接把上游域名钉到 IP，SNI 仍然是域名，TLS 校验不受影响。
_FORWARD_IPS = {
    "doh.pub": "120.53.53.53",
    "dns.alidns.com": "223.5.5.5",
    "dns.google": "8.8.8.8",
}

_orig_getaddrinfo = socket.getaddrinfo


def _forward_getaddrinfo(host, *args, **kwargs):
    ip = _FORWARD_IPS.get(host)
    if ip:
        host = ip
    return _orig_getaddrinfo(host, *args, **kwargs)


socket.getaddrinfo = _forward_getaddrinfo


# 简单应答缓存：浏览器会对同一域名反复查询，缓存能把上游压力降一个数量级
_FORWARD_CACHE: dict = {}
# 缓存 5 分钟：浏览器对同一域名会反复问，冷查询要付一次上游往返（约 0.1–3 秒）。
CACHE_TTL = 300.0
# 落盘缓存：重启后端（升级、换配置）后仍然有效，否则每次重启都要重新付一遍冷查询。
CACHE_PATH = ROOT / "data" / "doh-cache.json"
CACHE_MAX = 2000
_cache_dirty = False

# 明文 DNS（系统解析器）与 DoH（浏览器自己的解析器）必须给出不同的答案：
#   * 明文 DNS 无认证，ECH 配置不可信 -> 一律 127.0.0.1，交本机 nginx 终结
#   * DoH 有 TLS 保护 -> 按站点 dns_mode 返回，主站才拿得到 ECH
# 所以这个区别是**每次请求**的参数，不能是全局开关：一旦全局，DNS 监听器一启动
# 就会把 DoH 也变成 loopback，主站的 ECH 直接被抹掉。


# --------------------------------------------------------------------------
# 极简 DNS 报文处理（只做我们需要的部分）
# --------------------------------------------------------------------------


def read_name(data: bytes, offset: int) -> tuple:
    labels = []
    while True:
        length = data[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0:  # 压缩指针：我们生成的报文里不出现，遇到就放弃
            offset += 2
            break
        labels.append(data[offset + 1:offset + 1 + length].decode("ascii", "replace"))
        offset += 1 + length
    return ".".join(labels), offset


def parse_question(data: bytes) -> tuple:
    offset = 12
    name, offset = read_name(data, offset)
    qtype = int.from_bytes(data[offset:offset + 2], "big")
    qclass = int.from_bytes(data[offset + 2:offset + 4], "big")
    return name, qtype, qclass, offset + 4


def build_response(query: bytes, qend: int, answers: list) -> bytes:
    """answers: [(type, rdata_bytes, ttl)]；查询段原样保留。"""
    txid = query[:2]
    flags = (0x8000 | 0x0100 | 0x0080).to_bytes(2, "big")  # QR | RD | RA
    header = txid + flags + (1).to_bytes(2, "big") + len(answers).to_bytes(2, "big") + b"\x00\x00" + b"\x00\x00"
    body = query[12:qend]
    out = header + body
    for rtype, rdata, ttl in answers:
        out += b"\xc0\x0c"                      # name → 指向查询名
        out += rtype.to_bytes(2, "big")
        out += (1).to_bytes(2, "big")           # IN
        out += ttl.to_bytes(4, "big")
        out += len(rdata).to_bytes(2, "big")
        out += rdata
    return out


def svc_param(key: int, value: bytes) -> bytes:
    return key.to_bytes(2, "big") + len(value).to_bytes(2, "big") + value


def build_https_rdata(ipv4s: list, ech_b64: str, alpn: list) -> bytes:
    """SVCB/HTTPS RDATA：Priority | TargetName(根) | alpn | ipv4hint | ech。"""
    rdata = (1).to_bytes(2, "big") + b"\x00"     # priority=1, target=.
    alpn_wire = b"".join(bytes([len(a.encode())]) + a.encode() for a in alpn)
    rdata += svc_param(1, alpn_wire)             # key 1 = alpn
    if ipv4s:
        rdata += svc_param(4, b"".join(ipaddress.IPv4Address(ip).packed for ip in ipv4s))
    if ech_b64:
        rdata += svc_param(5, base64.b64decode(ech_b64))  # key 5 = ech
    return rdata


# --------------------------------------------------------------------------
# 站点数据
# --------------------------------------------------------------------------


def load_ech() -> dict:
    if not ECH_CFG.is_file():
        return {}
    return json.loads(ECH_CFG.read_text(encoding="utf-8"))


def load_upstreams() -> dict:
    """把站点清单摊平成「域名 → {ips, mode}」。

    两个模式必须分开，这是实测换来的结论：

      mode="ech"      目标在 Cloudflare 上 → 指向 CF 边缘 + 下发 ECH 配置，
                      浏览器自己把 SNI 加密。适用于 pixiv 主站。
      mode="loopback" 目标在自家机房、且发 SNI 会被切断 → 指向 127.0.0.1，
                      交给本地 nginx 处理（它向上游不发 SNI）。适用于 i.pximg.net。

    给后者塞 ECH 是错的：它根本不支持 ECH，握手会直接失败，表现为图片全挂。
    """
    table: dict = {}
    if not SITES.is_file():
        return table
    cfg = json.loads(SITES.read_text(encoding="utf-8"))
    for site in cfg.get("sites", []):
        if not (site.get("enabled", True) or site.get("dns_override", False)):
            continue
        mode = site.get("dns_mode", "ech")
        ips: list[str] = []
        if mode == "loopback":
            # loopback_ip 允许指到别的回环地址。127.0.0.2 上没有监听者，
            # 连接会立刻被拒绝 —— 用来让"无论如何都连不通"的域名快速失败，
            # 而不是让客户端对着被静默丢包的连接干等二十多秒。
            ips = [site.get("loopback_ip", "127.0.0.1")]
        elif mode == "dns":
            # 只修正解析：地址来自 forward()，这里不需要任何 IP。
            # 早期版本在这里读了 site["upstream_file"]，而 dns 模式的站点没有这个
            # 字段，KeyError 让整张表加载失败，所有域名都退化成转发（被污染）。
            pass
        elif site.get("candidates"):
            # ech 模式且没有 upstream_file 时，直接用手写的边缘地址（Discord CDN 用）。
            for cand in site["candidates"]:
                try:
                    ipaddress.IPv4Address(cand)
                except ValueError:
                    continue
                ips.append(cand)
        else:
            up = site.get("upstream_file")
            lst = ROOT / up if up else None
            if lst and lst.is_file():
                for line in lst.read_text(encoding="utf-8").splitlines():
                    line = line.strip()
                    if not line.startswith("server "):
                        continue
                    addr = line.split()[1].split(":")[0]
                    try:
                        ipaddress.IPv4Address(addr)
                    except ValueError:
                        continue
                    ips.append(addr)
        if not ips and mode != "dns":
            continue
        # ech_override：站点自己指定的 ECHConfigList（base64）。
        # 用于"服务商提供了共享 ECH 配置、但域名本身不发布 HTTPS 记录"的情况，
        # Discord 的 Cloudflare CDN 就是这种：Cloudflare 的共享配置名叫
        # cloudflare-ech.com，任何 CF 前置的站点都能拿来用。
        entry = {"ips": ips, "mode": mode,
                 "ech_override": site.get("ech_override", "")}
        for host in site["hosts"]:
            table[host.lower()] = entry
        for san in site.get("cert_san", []):
            if san.startswith("*.") and san[2:] not in table:
                table[san[2:]] = entry
    return table


# --------------------------------------------------------------------------
# DoH 服务
# --------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    server_version = "sniroute-doh"

    def log_message(self, fmt, *args):  # 安静一点
        sys.stderr.write("[doh] " + (fmt % args) + "\n")

    def _body(self) -> bytes:
        if self.command == "POST":
            length = int(self.headers.get("content-length") or 0)
            return self.rfile.read(length)
        qs = parse_qs(urlparse(self.path).query)
        if "dns" in qs:
            raw = qs["dns"][0]
            return base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4))
        return b""

    def do_POST(self):
        self._serve()

    def do_GET(self):
        self._serve()

    def _serve(self):
        query = self._body()
        if len(query) < 12:
            self.send_error(400, "bad dns message")
            return
        try:
            name, qtype, _cls, _qend = parse_question(query)
            reply = answer_query(query)
            kind = {1: "A", 28: "AAAA", 65: "HTTPS", 12: "PTR", 16: "TXT"}.get(qtype, f"type{qtype}")
            emit(f"[query] {name} {kind} -> {len(reply)}B")
        except Exception as exc:  # 任何解析异常都不该让服务倒下
            emit(f"[doh] 处理失败: {exc}")
            self.send_error(500, "dns error")
            return
        self.send_response(200)
        self.send_header("content-type", "application/dns-message")
        self.send_header("content-length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def emit(msg: str) -> None:
    sys.stderr.write(msg + "\n")
    if LOG_PATH:
        try:
            with open(LOG_PATH, "a", encoding="utf-8") as fh:
                fh.write(msg + "\n")
        except OSError:
            pass



def _tcp_ok(ip: str, port: int, timeout: float) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def _tls_ok(ip: str, host: str, timeout: float) -> bool:
    """对某个地址做一次真实 TLS 握手，SNI 用查询的域名。

    为什么不用 TCP 探活：Akamai/Fastly 的地址池里混着"TCP 秒连、TLS 握手却要
    8-15 秒甚至超时"的边缘（EA 的池子实测如此）。只测 TCP 会把这种地址判为可用，
    浏览器拿到它以后就卡住 —— 表现是页面时好时坏。
    """
    import ssl as _ssl
    if not host:
        return False
    ctx = _ssl.SSLContext(_ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = _ssl.CERT_NONE
    try:
        with socket.create_connection((ip, 443), timeout=timeout) as raw:
            with ctx.wrap_socket(raw, server_hostname=host):
                return True
    except (OSError, _ssl.SSLError):
        return False


def _a_records(answer: bytes) -> list:
    """从一条 DNS 响应里取出所有 A 记录（含 CNAME 链之后的最终地址）。"""
    import struct as _struct
    if len(answer) < 12:
        return []
    qd = _struct.unpack(">H", answer[4:6])[0]
    an = _struct.unpack(">H", answer[6:8])[0]
    i = 12
    for _ in range(qd):
        while i < len(answer) and answer[i]:
            i += answer[i] + 1
        i += 5
    out = []
    for _ in range(an):
        if i >= len(answer):
            break
        if answer[i] & 0xC0 == 0xC0:
            i += 2
        else:
            while i < len(answer) and answer[i]:
                i += answer[i] + 1
            i += 1
        if i + 10 > len(answer):
            break
        rtype, rclass, ttl, rdlen = _struct.unpack(">HHIH", answer[i:i + 10])
        i += 10
        rdata = answer[i:i + rdlen]
        i += rdlen
        if rtype == TYPE_A and rdlen == 4:
            out.append(ipaddress.IPv4Address(rdata))
    return out


def _filter_reachable(query: bytes, qend: int, answer: bytes) -> bytes:
    """把 DoH 给出的 A 记录做一次 TCP 443 探活，只保留能连上的。

    为什么需要：DNS 只说明"这个域名现在解析到这些地址"，不保证这些地址在你这条
    链路上可达。GitHub 尤其明显——真实地址有多个，其中部分在当前网络里 TCP 直接
    超时，浏览器拿到那样的地址就是 ERR_CONNECTION_TIMED_OUT。这里用很短超时并发
    探测，能把"解析对了但连不上"变成"解析对了且能连"。
    """
    ips = _a_records(answer)
    if not ips:
        return answer
    # 注意：只返回一条地址时也要探活。早期版本在这里直接返回，结果"上游给了一条
    # 黑洞地址"的情况完全没被处理 —— 客户端照样干等二三十秒。
    import concurrent.futures as _cf
    # 用查询的域名做 SNI 探活；拿不到域名就退回 TCP 探测。
    probe_host = ""
    try:
        probe_host = parse_question(query)[0].rstrip(".")
    except Exception:
        probe_host = ""
    with _cf.ThreadPoolExecutor(max_workers=len(ips)) as pool:
        results = list(pool.map(
            lambda ip: (ip, _tls_ok(str(ip), probe_host, 3.0) if probe_host
                        else _tcp_ok(str(ip), 443, 1.6)), ips))
    good = [ip for ip, ok in results if ok]
    if len(good) == len(ips):
        return answer
    if not good:
        # TLS 全部失败有两种可能，必须分开处理：
        #   a) 真是一批黑洞地址（被污染）→ 回空应答，让客户端立刻失败；
        #   b) 只是冷启动时握手慢（实测刚装完第一次访问会到这里）→ 用 TCP
        #      快速复查一次，能连上的仍然返回，避免出现 ERR_NAME_NOT_RESOLVED。
        import concurrent.futures as _cf2
        with _cf2.ThreadPoolExecutor(max_workers=len(ips)) as pool2:
            tcp_hits = [ip for ip, ok in pool2.map(
                lambda ip: (ip, _tcp_ok(str(ip), 443, 1.6)), ips) if ok]
        if tcp_hits:
            emit(f"[dns] TLS 全超时，但 {len(tcp_hits)} 个地址 TCP 可达，按冷启动处理返回")
            return build_response(query, qend, [(TYPE_A, ipaddress.IPv4Address(ip).packed, 60)
                                               for ip in tcp_hits])
        # 连 TCP 都不通时，仍然回退到上游给的原始答案，**不再回空应答**。
        # 教训：探活只是启发式，它失败的原因可能是地址被污染，也可能是探测那一刻
        # 网络抖动。把启发式失败当成"域名不存在"，会把一个本来能用的站点彻底打死
        # —— 实测 github.com 就因此变成 ERR_NAME_NOT_RESOLVED，连仓库都创建不了。
        # 真不可达时让客户端自己超时，至少域名还是活的。
        emit(f"[dns] {len(ips)} 个地址都没通过探活，回退为返回原始答案（不做剔除）")
        return answer
    emit(f"[dns] {len(ips)} 个地址中 {len(ips) - len(good)} 个不可达，已剔除")
    return build_response(query, qend, [(TYPE_A, ip.packed, 60) for ip in good])



# --------------------------------------------------------------------------
# Cloudflare 共享 ECH 配置
#
# 有些 Cloudflare 前置的站点（Discord 的 CDN）自己不在 HTTPS 记录里发布 ECH，
# 但 Cloudflare 提供了一个**共享配置**，其 public_name 是 cloudflare-ech.com。
# 用它就能把内层 SNI 加密送出去，让 GFW 只看到 cloudflare-ech.com。
#
# 这份配置由 Cloudflare 定期轮换，所以不能写死在代码里：启动后第一次需要时
# 现查一次，缓存一小时；查不到才回落到站点里写死的备份值。
# --------------------------------------------------------------------------
_SHARED_ECH = {"value": "", "ts": 0.0}


def build_query(name: str, qtype: int) -> bytes:
    import struct as _struct
    head = _struct.pack(">HHHHHH", 0x5A5A, 0x0100, 1, 0, 0, 0)
    labels = b"".join(bytes([len(x)]) + x.encode() for x in name.split(".")) + b"\x00"
    return head + labels + _struct.pack(">HH", qtype, 1)


def https_param(resp: bytes, want: int) -> bytes:
    """从 HTTPS(65) 响应里取出指定 key 的 svcparam 值。"""
    import struct as _struct
    if len(resp) < 12:
        return b""
    an = _struct.unpack(">H", resp[6:8])[0]
    i = 12
    while i < len(resp) and resp[i]:
        i += resp[i] + 1
    i += 5
    for _ in range(an):
        if i >= len(resp):
            break
        if resp[i] & 0xC0 == 0xC0:
            i += 2
        else:
            while resp[i]:
                i += resp[i] + 1
            i += 1
        if i + 10 > len(resp):
            break
        rtype, _rc, _ttl, rdlen = _struct.unpack(">HHIH", resp[i:i + 10])
        i += 10
        rdata = resp[i:i + rdlen]
        i += rdlen
        if rtype != TYPE_HTTPS:
            continue
        j = 2
        while j < len(rdata) and rdata[j]:
            j += rdata[j] + 1
        j += 1
        while j + 4 <= len(rdata):
            key, plen = _struct.unpack(">HH", rdata[j:j + 4])
            j += 4
            val = rdata[j:j + plen]
            j += plen
            if key == want:
                return val
    return b""


def shared_cloudflare_ech() -> str:
    import time as _time
    now = _time.monotonic()
    if _SHARED_ECH["value"] and now - _SHARED_ECH["ts"] < 3600:
        return _SHARED_ECH["value"]
    # 失败冷却：拿不到就 120 秒内不再试。
    # 没有它，每个 HTTPS(65) 查询都会重新走一遍上游（3 端点 × 4s），
    # 浏览器会看到十几秒的空白——WARP 关闭时尤其明显。
    if now < _SHARED_ECH.get("retry_after", 0.0):
        return _SHARED_ECH["value"]
    try:
        raw = https_param(forward(build_query("cloudflare-ech.com", TYPE_HTTPS)), 5)
        if raw:
            _SHARED_ECH["value"] = base64.b64encode(raw).decode()
            _SHARED_ECH["ts"] = now
            emit(f"[ech] 已从 cloudflare-ech.com 刷新共享配置（{len(raw)} 字节）")
    except Exception as exc:
        emit(f"[ech] 刷新共享配置失败: {exc}")
        _SHARED_ECH["retry_after"] = now + 120.0
    if not _SHARED_ECH["value"]:
        _SHARED_ECH["retry_after"] = now + 120.0
    return _SHARED_ECH["value"]


def answer_query(query: bytes, plain: bool = False) -> bytes:
    name, qtype, qclass, qend = parse_question(query)
    key = name.lower().rstrip(".")

    entry = UPSTREAM_TABLE.get(key)
    if entry is None:
        # 尝试后缀匹配（*.pximg.net → 任何子域）
        for suffix, mapped in UPSTREAM_TABLE.items():
            if key == suffix or key.endswith("." + suffix):
                entry = mapped
                break

    if entry is None:
        return forward(query)

    # dns 模式：只修正解析，不做代理。
    # 适用于"DNS 被污染、但带 SNI 的 TLS 本身没问题"的站点（GitHub 就是这样）：
    # 我们用 DoH 拿到真实地址，浏览器照常直连，证书是对方的真证书，
    # 因此既不需要本机终结 TLS，也不需要签发任何证书。
    if entry.get("mode") == "dns":
        resp = forward(query)
        # 只修解析，不做代理；但要保证给出的地址在本机确实能连上。
        return _filter_reachable(query, qend, resp)

    # 明文 DNS（全局模式）没有可信通道，ECH 配置不可信，因此一律把流量引回本机，
    # 由 nginx 终结 TLS；DoH 路径仍然按站点的 dns_mode 走 ECH。
    # 只有 loopback 模式才把明文 DNS 指向本机。ech 模式的客户端如果不会 ECH，
    # 指到本机只会拿到 Cloudflare 的 421 错误页；给它真实地址反而更清楚
    # （连接被重置，而不是在浏览器/客户端里看到误导性的 421）。
    if plain and entry["mode"] == "loopback":
        ips = ["127.0.0.1"]
    else:
        ips = entry["ips"]
    if qtype == TYPE_A:
        answers = [(TYPE_A, ipaddress.IPv4Address(ip).packed, 60) for ip in ips]
        return build_response(query, qend, answers)
    if qtype == TYPE_AAAA:
        # 空 AAAA：pixiv 的 IPv6 会 403 block_ip，且让浏览器不要绕开我们的 A 记录
        return build_response(query, qend, [])
    if qtype == TYPE_HTTPS:
        # loopback 模式绝不能带 ech：目标不支持 ECH，带了就是握手失败
        ech = ""
        if not plain and entry["mode"] != "loopback":
            override = entry.get("ech_override") or ""
            if override == "auto:cloudflare":
                # 自动模式：现查 Cloudflare 的共享配置，失败才用站点里的备份值。
                override = (shared_cloudflare_ech()
                            or entry.get("ech_fallback", ""))
            ech = override or ECH.get("ech_config", "")
        rdata = build_https_rdata(ips, ech, ECH.get("alpn", ["h2"]))
        return build_response(query, qend, [(TYPE_HTTPS, rdata, 60)])
    return build_response(query, qend, [])



def _load_cache() -> None:
    """启动时读回上次落盘的缓存；文件坏了就当没有，绝不因此启动失败。"""
    global _FORWARD_CACHE
    try:
        import json as _json
        raw = _json.loads(CACHE_PATH.read_text(encoding="utf-8"))
    except Exception:
        return
    now = __import__("time").monotonic()
    kept = 0
    for k, v in raw.items():
        try:
            ts, b64 = float(v[0]), v[1]
            blob = __import__("base64").b64decode(b64)
        except Exception:
            continue
        # 落盘里存的是"写入时的 monotonic"，跨进程没有可比性；
        # 所以落盘条目统一按短 TTL（120 秒）保守处理，避免给出过期答案。
        _FORWARD_CACHE[__import__("base64").b64decode(k)] = (now, blob)
        kept += 1
        if kept >= CACHE_MAX:
            break
    if kept:
        emit(f"[cache] 已载入 {kept} 条落盘缓存")


def _save_cache() -> None:
    """把缓存写盘。只保留未过期的条目，并做条数上限。"""
    global _cache_dirty
    if not _cache_dirty:
        return
    try:
        import json as _json, time as _time, base64 as _b64
        now = _time.monotonic()
        items = []
        for q, (ts, body) in _FORWARD_CACHE.items():
            if now - ts <= CACHE_TTL:
                items.append((ts, q, body))
        items.sort(key=lambda x: x[0], reverse=True)
        items = items[:CACHE_MAX]
        doc = {_b64.b64encode(q).decode(): [0.0, _b64.b64encode(body).decode()]
               for _ts, q, body in items}
        tmp = CACHE_PATH.with_suffix(".tmp")
        tmp.write_text(_json.dumps(doc), encoding="utf-8")
        tmp.replace(CACHE_PATH)
        _cache_dirty = False
        emit(f"[cache] 已写盘 {len(doc)} 条")
    except Exception as exc:
        emit(f"[cache] 写盘失败: {exc}")


def _cache_flush_loop() -> None:
    import time as _time
    while True:
        _time.sleep(60)
        _save_cache()

def forward(query: bytes) -> bytes:
    """转发给公共 DoH。多端点 + 缓存，任何单点抖动都不该让浏览器整体不可用。"""
    import time as _time
    now = _time.monotonic()
    cached = _FORWARD_CACHE.get(query)
    if cached and now - cached[0] < CACHE_TTL:
        return cached[1]

    last_err = None
    tried = 0
    for endpoint in list(FORWARD_DOHS):
        fails, until = _FORWARD_FAIL.get(endpoint, (0, 0.0))
        cooling = fails >= 2 and now < until
        # 所有端点都在冷却时，仍然试第一个 —— 宁可多等一次，也不要直接 SERVFAIL。
        if cooling and tried > 0:
            continue
        if cooling and tried == 0:
            emit(f"[forward] 全部端点冷却中，强制重试 {endpoint}")
        tried += 1
        req = urllib.request.Request(
            endpoint, data=query,
            headers={"content-type": "application/dns-message",
                     "accept": "application/dns-message"})
        try:
            with urllib.request.urlopen(req, timeout=3) as resp:
                body = resp.read()
            if len(body) >= 12:
                _FORWARD_CACHE[query] = (now, body)
                globals()["_cache_dirty"] = True
                if len(_FORWARD_CACHE) > CACHE_MAX:
                    # 超上限就砍掉最旧的一半，避免内存无界增长
                    for k in sorted(_FORWARD_CACHE, key=lambda q: _FORWARD_CACHE[q][0])[:CACHE_MAX // 2]:
                        _FORWARD_CACHE.pop(k, None)
                return body
        except Exception as exc:
            last_err = exc
            fails, _ = _FORWARD_FAIL.get(endpoint, (0, 0.0))
            _FORWARD_FAIL[endpoint] = (fails + 1, now + FORWARD_COOLDOWN)
            continue

    # 全部失败时返回 SERVFAIL，而不是抛 500 —— 浏览器至少能重试或回退，
    # 不会看到"DNS 服务挂了"这种一刀切的结果。
    emit(f"[forward] 所有上游都失败: {last_err}")
    txid = query[:2]
    flags = (0x8000 | 0x0100 | 0x0080 | 0x0002).to_bytes(2, "big")  # QR|RD|RA|SERVFAIL
    return txid + flags + b"\x00\x01" + b"\x00" * 8 + query[12:]


# --------------------------------------------------------------------------


def serve_dns_plain(host: str, port: int) -> None:
    """在 53 端口提供明文 DNS，供系统解析器使用（全局模式）。

    为什么需要它：NRPT 把域名交给"某个 DNS 服务器"，而系统解析器只会说明文
    DNS。于是同一条解析逻辑要能从两种传输进入 —— DoH（浏览器自己的解析器，
    可以带 ECH）和明文 DNS（系统栈，统一走本机 nginx）。

    UDP 和 TCP 都要开：系统解析器默认 UDP，响应被截断时会改用 TCP。
    """
    import threading

    def udp_loop() -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((host, port))
        except OSError as exc:
            emit(f"[dns] UDP {host}:{port} 绑定失败: {exc}")
            return
        emit(f"[dns] 明文 DNS 监听 udp://{host}:{port}")
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except OSError:
                continue
            try:
                resp = answer_query(data, plain=True)
            except Exception as exc:
                emit(f"[dns] 处理失败: {exc}")
                continue
            if resp:
                sock.sendto(resp, addr)

    def tcp_loop() -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            srv.bind((host, port))
            srv.listen(16)
        except OSError as exc:
            emit(f"[dns] TCP {host}:{port} 绑定失败: {exc}")
            return
        emit(f"[dns] 明文 DNS 监听 tcp://{host}:{port}")
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                continue
            with conn:
                try:
                    head = conn.recv(2)
                    if len(head) < 2:
                        continue
                    want = int.from_bytes(head, "big")
                    body = b""
                    while len(body) < want:
                        chunk = conn.recv(want - len(body))
                        if not chunk:
                            break
                        body += chunk
                    resp = answer_query(body, plain=True)
                    if resp:
                        conn.sendall(len(resp).to_bytes(2, "big") + resp)
                except Exception as exc:
                    emit(f"[dns] TCP 处理失败: {exc}")

    threading.Thread(target=udp_loop, name="dns-udp", daemon=True).start()
    threading.Thread(target=tcp_loop, name="dns-tcp", daemon=True).start()

def main() -> None:
    for _stream in (sys.stdout, sys.stderr):
        if not _stream.isatty() and hasattr(_stream, "reconfigure"):
            _stream.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(description="本地 DoH 服务（为浏览器原生 ECH 提供配置）")
    ap.add_argument("--listen", default="127.0.0.1:5443")
    ap.add_argument("--cert", default=str(ROOT / "conf/certs/doh.crt"))
    ap.add_argument("--key", default=str(ROOT / "conf/certs/doh.key"))
    ap.add_argument("--no-tls", action="store_true", help="明文 HTTP，仅本地调试用")
    ap.add_argument("--log", help="把每条查询写进这个文件（排查用）")
    ap.add_argument("--dns-listen", default="127.0.0.1", help="明文 DNS 监听地址（全局模式）")
    ap.add_argument("--dns-port", type=int, default=53, help="明文 DNS 端口；0 表示不启用")
    args = ap.parse_args()

    global ECH, UPSTREAM_TABLE, LOG_PATH
    ECH = load_ech()
    UPSTREAM_TABLE = load_upstreams()
    LOG_PATH = args.log

    # 启动时先把 Cloudflare 的共享 ECH 配置取好。懒加载会让重启后的第一个
    # 查询多等一次远端往返，浏览器有时等不及就直接失败（实测 5 次里第 1 次挂）。
    if any(e.get("ech_override") == "auto:cloudflare" for e in UPSTREAM_TABLE.values()):
        try:
            shared_cloudflare_ech()
        except Exception as _exc:
            emit(f"[ech] 预取失败: {_exc}")

    host, _, port = args.listen.rpartition(":")
    if not ECH.get("ech_config"):
        print("警告：data/cloudflare-ech.json 里没有 ech 配置，浏览器将无法启用 ECH。",
              file=sys.stderr)

    class Server(socketserver.ThreadingTCPServer):
        # Windows 上 SO_REUSEADDR 的语义和 Unix 不同：它允许**两个进程同时绑同一个端口**，
        # 后绑的那个会悄悄抢走流量。这个坑过一次：旧实例占着 5443，新实例看起来
        # "启动成功"，实际上一条查询都收不到，日志一直是空的。
        # 所以 Windows 下显式禁止复用，宁可启动失败也不要静默双开。
        allow_reuse_address = not sys.platform.startswith("win")
        daemon_threads = True

    httpd = Server((host or "127.0.0.1", int(port)), Handler)

    if not args.no_tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    scheme = "http" if args.no_tls else "https"
    print(f"DoH 监听 {scheme}://{host or '127.0.0.1'}:{port}/dns-query", file=sys.stderr)
    print(f"  接管域名 {len(UPSTREAM_TABLE)} 个；ECH 配置 {'已载入' if ECH.get('ech_config') else '缺失'}",
          file=sys.stderr)

    # 全局模式：同时说明文 DNS，供系统解析器（NRPT 指向这里）使用。
    _load_cache()
    import threading as _th
    _th.Thread(target=_cache_flush_loop, name="cache-flush", daemon=True).start()

    if args.dns_port:
        serve_dns_plain(args.dns_listen, args.dns_port)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("退出", file=sys.stderr)


if __name__ == "__main__":
    main()
