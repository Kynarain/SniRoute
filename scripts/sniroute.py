#!/usr/bin/env python3
"""sniroute 命令行工具。

一个纯标准库的 Windows 小工具，负责这座代理的四件杂事：

    discover   找候选 IP：DoH 查真实记录，或扩展已验证地址的同网段
    probe      按代理真正会用的方式实测每个地址，写出 upstreams/*.list
    gen-ca     / gen-cert / install-ca   本机证书体系
    hosts      / service                 接上域名、启停 nginx

为什么要有它：Pixiv-Nginx 那种项目最后都死在同一件事上 —— 写死的 IP 烂了，
而没人知道该换成哪个。把这些动作脚本化，是让这套东西能活过三个月的唯一办法。

用法见 README.md；每个子命令也有 --help。
"""

from __future__ import annotations

import argparse
import concurrent.futures
import ipaddress
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONF = ROOT / "conf"
CA_DIR = ROOT / "ca"

# 证书必须放在 conf/ 下面。nginx 把 `ssl_certificate certs/x.crt` 这类相对路径
# 解析到 conf_prefix（即 conf/），而 `error_log logs/x.log` 解析到 prefix（仓库根）。
# 两条规则不一样，是这套配置里最容易踩的坑：放错位置报的是
# [emerg] BIO_new_file() failed，而不是"文件找不到"。
CERT_DIR = CONF / "certs"

TEMP_DIR = ROOT / "temp"
DATA = ROOT / "data" / "sites.json"

# DoH 是本方案的中枢：浏览器所有解析都问它，它再决定哪个域名走 ECH、哪个交给 nginx。
# 端口不用 443/53，避免和系统里已有的东西抢。
# 5444 是浏览器看到的端点（nginx 前置，支持 h2）；5454 是 Python 后端，只说
# 明文 HTTP。为什么这么拆：Chrome 的 DoH 必须走 HTTP/2，而 Python 标准库的 TLS
# 不提供 ALPN，浏览器会直接判定该端点不可用（表现为所有域名解析失败）。TLS+h2
# 交给 nginx，解析逻辑仍然在 Python 里，两边各做自己擅长的事。
DOH_PORT = 5444
DOH_INTERNAL_PORT = 5454
# Global mode: NRPT points names at our plain DNS listener (the system resolver
# speaks plain DNS only). Port 53 is what NRPT accepts.
PLAIN_DNS_PORT = 53
DOH_LOG = ROOT / "logs" / "doh.log"
DOH_SCRIPT = ROOT / "tools" / "doh_ech.py"

# discover 的产出。和 sites.json 分开：前者是机器测出来的临时结果，后者是人写的
# 长期事实。混在一起的话，一次失败的自动发现就会覆盖掉手工维护的清单。
CANDIDATES = ROOT / "data" / "candidates.json"

# 公共 DoH 端点。用它们拿没被污染的 A/AAAA 记录 —— 只要本机到其中任意一个的
# HTTPS 是通的，结果就可信；不通就一个都拿不到，此时只能靠 expand 或手工。
# 可达性优先：cloudflare / google 的 DoH 端点在这个网络里本身就是 TCP 不通的，
# 排在前面会让所有查询直接失败。doh.pub 与 alidns 在国内可达，放前面；其余留作
# 海外环境的后备。
DOH_ENDPOINTS = [
    ("doh.pub", "https://doh.pub/dns-query"),
    ("alidns", "https://dns.alidns.com/resolve"),
    ("cloudflare", "https://cloudflare-dns.com/dns-query"),
    ("google", "https://dns.google/resolve"),
]

HOSTS_PATH = (Path(os.environ.get("SystemRoot", r"C:\Windows"))
              / "System32" / "drivers" / "etc" / "hosts")
BLOCK_BEGIN = "# >>> sniroute >>>"
BLOCK_END = "# <<< sniroute <<<"
# DoH 端点那条 hosts 由 setup-browser-ech.ps1 写入，用的是另一个标记。
# 两处标记不同，检查时必须用对应的那个 —— 用错就会出现"明明写了却说没有"。
DOH_BLOCK_BEGIN = "# >>> sniroute doh >>>"

# 候选地址的解析顺序。Git for Windows 自带 openssl，是这台机器上最可能存在的那个。
OPENSSL_CANDIDATES = [
    r"C:\Program Files\Git\usr\bin\openssl.exe",
    r"C:\Program Files\Git\mingw64\bin\openssl.exe",
    r"C:\Program Files (x86)\Git\usr\bin\openssl.exe",
]


# --------------------------------------------------------------------------
# 基础设施
# --------------------------------------------------------------------------


def die(msg: str) -> None:
    print(f"错误：{msg}", file=sys.stderr)
    raise SystemExit(2)


def find_openssl() -> str:
    # 便携包：openssl 随包放在 openssl\mingw64\bin\ 下，并且必须是这个目录布局
    # （OpenSSL 3 会按自身路径推算 prefix，去找 etc\ssl\openssl.cnf 与
    #  lib\ossl-modules）。所以优先用它，而不是系统里碰巧存在的那个。
    for relative in ("openssl/mingw64/bin/openssl.exe", "../openssl/mingw64/bin/openssl.exe"):
        candidate = (ROOT / relative).resolve()
        if candidate.is_file():
            return str(candidate)
    exe = shutil.which("openssl")
    if exe:
        return exe
    for cand in OPENSSL_CANDIDATES:
        if Path(cand).is_file():
            return cand
    die("找不到 openssl。装 Git for Windows，或把它放进 PATH。")


def find_nginx(explicit: str | None) -> str:
    exe = explicit or os.environ.get("SNIROUTE_NGINX")
    if not exe:
        marker = ROOT / "nginx.path"
        if marker.is_file():
            # 用 utf-8-sig 读：Windows 上存 UTF-8 很容易带 BOM，而带 BOM 的路径会让
            # Path.is_file() 永远为假，报出来的却是"找不到 nginx"。
            exe = marker.read_text(encoding="utf-8-sig").strip()
    if exe and Path(exe).is_file():
        return exe
    # 便携包：nginx.exe 随包放在 nginx\ 下，不依赖系统安装
    for relative in ("nginx/nginx.exe", "../nginx/nginx.exe"):
        candidate = (ROOT / relative).resolve()
        if candidate.is_file():
            return str(candidate)
    die("找不到 nginx.exe。从 https://nginx.org/en/download.html 下载 Windows 版"
        "（1.25.1 以上），用 --nginx 指定路径，或把路径写进仓库根的 nginx.path。")


def load_sites() -> dict:
    if not DATA.is_file():
        die(f"缺少站点清单：{DATA}")
    return json.loads(DATA.read_text(encoding="utf-8"))


def select_sites(cfg: dict, name: str | None) -> list:
    sites = cfg["sites"]
    if not name:
        return sites
    picked = [s for s in sites if s["name"] == name]
    if not picked:
        die(f"站点清单里没有 {name!r}；可选：{', '.join(s['name'] for s in sites)}")
    return picked


def run(cmd: list, *, quiet: bool = True, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(cwd or ROOT), capture_output=quiet, text=True,
                          encoding="utf-8", errors="replace")


def is_admin() -> bool:
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


# --------------------------------------------------------------------------
# probe：按代理真正会用的方式探测
# --------------------------------------------------------------------------


def _cert_names(cert: dict) -> list:
    names = [v for k, v in cert.get("subjectAltName", ()) if k == "DNS"]
    if not names:
        names = [v for rdn in cert.get("subject", ()) for k, v in rdn if k == "commonName"]
    return [n.lower() for n in names]


def cert_covers(cert: dict, hostname: str) -> bool:
    """对方出示的证书是否覆盖目标域名。通配只匹配一级，和浏览器规则一致。"""
    host = hostname.lower().rstrip(".")
    for name in _cert_names(cert):
        if name == host:
            return True
        if name.startswith("*.") and host.count(".") >= 2 and host.split(".", 1)[1] == name[2:]:
            return True
    return False


def _handshake(ip: str, port: int, hostname: str, timeout: float,
               with_sni: bool, verify: bool) -> dict:
    ctx = ssl.create_default_context()
    ctx.check_hostname = with_sni and verify
    ctx.verify_mode = ssl.CERT_REQUIRED if verify else ssl.CERT_NONE
    started = time.monotonic()
    with socket.create_connection((ip, port), timeout=timeout) as raw:
        # server_hostname=None 时 Python 不发 SNI 扩展 —— 这正是代理的行为
        with ctx.wrap_socket(raw, server_hostname=hostname if with_sni else None) as tls:
            cert = tls.getpeercert() or {}
            issuer = ""
            for rdn in cert.get("issuer", ()):
                for key, value in rdn:
                    if key == "commonName":
                        issuer = value
            return {"tls": tls.version(), "cert": cert, "issuer": issuer,
                    "ms": round((time.monotonic() - started) * 1000)}


def probe_one(ip: str, hostname: str, port: int, timeout: float,
              mode: str = "auto") -> dict:
    """按代理真正会用的方式探测一个地址。

    这里有一次实测换来的教训：最初这版只按浏览器的方式探测（带真实 SNI、校验证书），
    结果在一台被 SNI 阻断的机器上它把所有地址判死；而同一个地址经 nginx
    （proxy_ssl_server_name off，握手不发 SNI）却正常回包：
        带 SNI   → 0.1 秒被重置
        不带 SNI → 握手成功、证书链有效、对方出示 CN=pixiv.net

    所以探测必须复刻使用方式：默认先试"不发 SNI"，只有它失败时才试发 SNI。
    """
    order = {"auto": (False, True), "nosni": (False,), "sni": (True,)}[mode]
    errors: list[str] = []

    for with_sni in order:
        label = "SNI" if with_sni else "无SNI"
        try:
            info = _handshake(ip, port, hostname, timeout, with_sni, verify=True)
            # 发 SNI 时主机名由 SSLContext 校验；不发 SNI 时它没法替我们查（对方给的
            # 可能是默认证书），所以这里得自己比对 SAN/CN。
            identity = "verified" if (with_sni or cert_covers(info["cert"], hostname)) else "mismatch"
            return {"ip": ip, "ok": True, "mode": label, "identity": identity, **info}
        except ssl.SSLCertVerificationError as exc:
            errors.append(f"{label}: 证书链无效({exc.reason})")
            # 链无效不等于不可用：很多站点在无 SNI 时给默认证书，链路本身是通的
            try:
                info = _handshake(ip, port, hostname, timeout, with_sni, verify=False)
                identity = "selfsigned-match" if info["cert"] and cert_covers(
                    info["cert"], hostname) else "mismatch"
                return {"ip": ip, "ok": True, "mode": label, "identity": identity, **info}
            except (socket.timeout, TimeoutError):
                errors.append(f"{label}: 超时")
            except (ssl.SSLError, OSError) as exc2:
                errors.append(f"{label}: {getattr(exc2, 'reason', None) or exc2}")
        except (socket.timeout, TimeoutError):
            errors.append(f"{label}: 超时")
        except ssl.SSLError as exc:
            # 被重置通常落在这里，且很快返回 —— 正是 SNI 阻断的典型特征
            errors.append(f"{label}: {exc.reason or exc}")
        except OSError as exc:
            errors.append(f"{label}: {exc.strerror or exc}")

    return {"ip": ip, "ok": False, "error": " / ".join(errors)}


def write_upstream_file(path: Path, results: list, site: dict, mode: str) -> None:
    """写上游列表：按延迟排序，并剔除明显慢的离群值。

    为什么剔除：nginx 的 upstream 默认轮询，会把 1/N 的请求分给最慢的那个 IP。
    实测 pximg 的候选里最快的 140ms、最慢 2344ms，混在一起时小图请求也会偶发
    几秒甚至二十几秒的延迟 —— 那种长尾最伤体验，而它完全可以靠"别把慢的写进去"
    消掉。至少保留 3 个，避免剔到没有冗余。
    """
    kept = [r for r in results if r["identity"] != "signed-mismatch"]
    dropped: list = []
    if len(kept) > 3:
        times = sorted(r["ms"] for r in kept)
        median = times[len(times) // 2]
        threshold = max(median * 2.5, median + 800)
        fast = [r for r in kept if r["ms"] <= threshold]
        if len(fast) >= 3:
            dropped = [r for r in kept if r["ms"] > threshold]
            kept = fast

    weak = [r for r in kept if r["identity"] != "verified"]
    lines = [
        "# 由 scripts/sniroute.py probe 生成 —— 手工改动会在下次 probe 时被覆盖。",
        f"# 站点：{site['name']}   探测：{site['probe']['hostname']}:{site['probe']['port']}",
        f"# 生成时间：{time.strftime('%Y-%m-%d %H:%M:%S')}   探测模式：{mode}",
    ]
    if dropped:
        lines.append(f"# 已剔除 {len(dropped)} 个明显慢的地址（>{int(threshold)}ms），"
                     "避免轮询把请求分给它们。")
    if weak:
        lines.append(f"# 注意：其中 {len(weak)} 条无法确认身份（无 SNI 时对方给的是默认证书），"
                     "链路可用但请自行评估。")
    lines.append("")
    for r in sorted(kept, key=lambda x: x["ms"]):
        lines.append(f"server {r['ip']}:{site['probe']['port']} max_fails=1 fail_timeout=10s;"
                     f"   # {r['ms']}ms {r['mode']} {r['identity']} {r.get('issuer', '')}".rstrip())
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if dropped:
        print(f"    剔除慢地址 {len(dropped)} 个："
              + ", ".join(f"{r['ip']}({r['ms']}ms)" for r in dropped[:5]))


def cmd_probe(args) -> None:
    sites = select_sites(load_sites(), args.site)
    found = load_candidates()
    failures = 0

    for site in sites:
        hostname = site["probe"]["hostname"]
        port = site["probe"]["port"]
        prepared = merge_candidates(site["candidates"], found.get(site["name"], []))
        cands = prepared["ips"]
        out = ROOT / site["upstream_file"]

        if prepared["auto"]:
            print(f"\n[{site['name']}] 并入 discover 找到的 {prepared['auto']} 个候选")
        print(f"\n=== {site['name']} → {hostname}:{port}（{len(cands)} 个候选，模式 {args.mode}）===")

        with concurrent.futures.ThreadPoolExecutor(max_workers=min(16, max(1, len(cands)))) as pool:
            futures = [pool.submit(probe_one, ip, hostname, port, args.timeout, args.mode)
                       for ip in cands]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        rank = {"verified": 0, "selfsigned-match": 1, "mismatch": 2}
        good = sorted((r for r in results if r["ok"]),
                      key=lambda r: (rank.get(r["identity"], 3), r["ms"]))
        bad = [r for r in results if not r["ok"]]

        for r in good:
            print(f"    OK   {r['ip']:<18} {r['ms']:>5}ms  {r['mode']:<5} "
                  f"{r['identity']:<16} {r['tls']}  {r.get('issuer', '')[:30]}")
        for r in bad[:5]:
            print(f"    --   {r['ip']:<18} {r['error']}")
        if len(bad) > 5:
            print(f"    --   …另有 {len(bad) - 5} 个失败")

        if not good:
            failures += 1
            # 一条都不通时绝不覆盖：现有列表可能还有效，只是这次网络不通
            print(f"    没有可用地址；保留 {out.name} 原样不动。")
            continue

        write_upstream_file(out, good, site, args.mode)
        weak = sum(1 for r in good if r["identity"] != "verified")
        print(f"    已写入 {out.relative_to(ROOT)}（{len(good)} 条"
              f"{f'，其中 {weak} 条身份不可确认' if weak else ''}）")

    if failures:
        print(f"\n{failures} 个站点没有探到任何可用地址。这通常意味着当前网络到目标不可达，"
              f"或候选 IP 全部失效 —— 需要补充新的候选地址。")


# --------------------------------------------------------------------------
# discover：候选 IP 从哪来
#
# probe 只能回答"这些地址里哪些还能用"，回答不了"我该试哪些"。Pixiv-Nginx 死在
# 第二次上：地址是抄来的，抄的那天就是它开始腐烂的那天。
#
# 这里给两条不依赖第三方数据的路：
#   doh     用加密 DNS 拿真实 A/AAAA。污染绕得过去，这步就成立。
#   expand  从一个已验证活着的地址出发，探测它同网段的邻居。目标站点通常整段部署。
# --------------------------------------------------------------------------


def load_candidates() -> dict:
    if not CANDIDATES.is_file():
        return {}
    try:
        raw = json.loads(CANDIDATES.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
    return {k: v for k, v in raw.items() if not k.startswith("_") and isinstance(v, list)}


def save_candidates(data: dict) -> None:
    payload = {"_comment": "discover 的产出；probe 会并入 sites.json 的 candidates 一起测。",
               "_updated": time.strftime("%Y-%m-%d %H:%M:%S")}
    for name in sorted(data):
        payload[name] = sorted(data[name])
    CANDIDATES.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                          encoding="utf-8")


def merge_candidates(manual: list, auto: list) -> dict:
    seen, ips = set(), []
    for ip in list(manual) + list(auto):
        if ip not in seen:
            seen.add(ip)
            ips.append(ip)
    return {"ips": ips, "auto": len(set(auto) - set(manual))}


def doh_lookup(name: str, timeout: float) -> tuple:
    """问公共 DoH 要 A/AAAA。返回 (端点名, [ip...]) 或 (None, [])。"""
    for label, base in DOH_ENDPOINTS:
        ips: list[str] = []
        for rtype in ("A", "AAAA"):
            url = f"{base}?name={urllib.parse.quote(name)}&type={rtype}"
            req = urllib.request.Request(url, headers={"Accept": "application/dns-json"})
            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    doc = json.loads(resp.read().decode("utf-8", errors="replace"))
            except Exception:
                ips = []
                break
            for answer in doc.get("Answer", []):
                # 1 = A, 28 = AAAA
                if answer.get("type") in (1, 28) and isinstance(answer.get("data"), str):
                    try:
                        ipaddress.ip_address(answer["data"])
                    except ValueError:
                        continue
                    ips.append(answer["data"])
        if ips:
            return label, ips
    return None, []


def expand_neighbors(ips: list, mask: int) -> list:
    """把每个地址扩展成它所在网段里的其他主机地址。"""
    out: set = set()
    for ip in ips:
        try:
            net = ipaddress.ip_network(f"{ip}/{mask}", strict=False)
        except ValueError:
            continue
        # 只扩展 IPv4：IPv6 的 /64 有 2^64 个地址，扫它没有意义
        if net.version != 4:
            continue
        for host in net.hosts():
            out.add(str(host))
    return sorted(out)


def cmd_discover(args) -> None:
    sites = select_sites(load_sites(), args.site)
    known = load_candidates()
    changed = False

    if args.method in ("doh", "both"):
        for site in sites:
            hostname = site["probe"]["hostname"]
            print(f"\n=== DoH 查询 {hostname} ===")
            label, ips = doh_lookup(hostname, args.timeout)
            if not ips:
                print("    所有 DoH 端点都不可达或没有返回答案。")
                print("    这通常意味着当前网络到公网 DoH 不通 —— 只能靠 expand 或手工补候选。")
                continue
            fresh = [ip for ip in ips if ip not in set(known.get(site["name"], []))]
            print(f"    {label} 返回 {len(ips)} 条：{', '.join(ips)}")
            if fresh:
                print(f"    新增 {len(fresh)} 条")
            known[site["name"]] = sorted(set(known.get(site["name"], [])) | set(ips))
            changed = True

    if args.method in ("expand", "both"):
        for site in sites:
            hostname = site["probe"]["hostname"]
            port = site["probe"]["port"]
            pool = merge_candidates(site["candidates"], known.get(site["name"], []))["ips"]
            print(f"\n=== 网段扩展 {site['name']}（先找出活着的起点）===")

            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
                seeds = [r for r in ex.map(
                    lambda ip: probe_one(ip, hostname, port, args.timeout, "auto"), pool)
                    if r["ok"]]

            if not seeds:
                print("    当前没有任何候选是活的，无法扩展。先跑 doh，或手工补几个候选。")
                continue

            seeds.sort(key=lambda r: r["ms"])
            print(f"    活着的起点 {len(seeds)} 个：{', '.join(s['ip'] for s in seeds[:5])}"
                  f"{' …' if len(seeds) > 5 else ''}")

            neighbors = [ip for ip in expand_neighbors([s["ip"] for s in seeds], args.mask)
                         if ip not in set(pool)]
            print(f"    /{args.mask} 邻居 {len(neighbors)} 个待测")
            if not neighbors:
                print("    没有可扩展的邻居。")
                continue

            with concurrent.futures.ThreadPoolExecutor(max_workers=32) as ex:
                hits = [r for r in ex.map(
                    lambda ip: probe_one(ip, hostname, port, args.timeout, "auto"), neighbors)
                    if r["ok"]]

            hits.sort(key=lambda r: r["ms"])
            for r in hits[:10]:
                print(f"    OK   {r['ip']:<18} {r['ms']:>5}ms  {r['mode']} {r['identity']}")
            if len(hits) > 10:
                print(f"    …另外 {len(hits) - 10} 个")
            if hits:
                known[site["name"]] = sorted(set(known.get(site["name"], []))
                                             | {r["ip"] for r in hits})
                changed = True

    if changed:
        save_candidates(known)
        print(f"\n已写入 {CANDIDATES.relative_to(ROOT)}。下一步：python scripts/sniroute.py probe")
    else:
        print("\n没有发现任何新候选，文件未改动。")


# --------------------------------------------------------------------------
# 证书
# --------------------------------------------------------------------------


def cmd_gen_ca(args) -> None:
    openssl = find_openssl()
    cfg = load_sites()["ca"]
    CA_DIR.mkdir(parents=True, exist_ok=True)
    key, crt = CA_DIR / "rootCA.key", CA_DIR / "rootCA.crt"

    if key.exists() and not args.force:
        die(f"{key} 已存在。换根证书会让已安装的旧根失效，确认重来请加 --force。")

    res = run([openssl, "req", "-x509", "-new", "-nodes", "-newkey", "rsa:3072", "-sha256",
               "-days", str(cfg["days"]), "-keyout", str(key), "-out", str(crt),
               "-subj", f"/O={cfg['organization']}/CN={cfg['common_name']}",
               "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
               "-addext", "keyUsage=critical,keyCertSign,cRLSign",
               "-addext", "subjectKeyIdentifier=hash"])
    if res.returncode != 0:
        die(f"openssl 失败：\n{res.stderr}")
    print(f"根证书已生成：{crt}")
    print(f"私钥：{key}（不要提交、不要外传、不要丢）")
    print("\n下一步：以管理员身份运行 `python scripts/sniroute.py install-ca`。")


def gen_doh_cert() -> None:
    """给本地 DoH 端点签一张证书。

    必须同时带 DNS 与 IP 两个 SAN：

      DNS:doh.sniroute.lan   —— 老办法（需要 hosts 条目，要管理员）
      IP:127.0.0.1           —— 免管理员方案：浏览器直接把
                                https://127.0.0.1:5444/dns-query 当作 DoH 端点，
                                完全不需要改 hosts

    没有 IP SAN 的话，浏览器会以"证书与地址不匹配"为由拒绝整个 DoH，表现为
    什么域名都解析不了——这个坑踩过一次。
    """
    openssl = find_openssl()
    crt_ca, key_ca = CA_DIR / "rootCA.crt", CA_DIR / "rootCA.key"
    if not crt_ca.is_file() or not key_ca.is_file():
        die("根证书不存在，先跑 gen-ca。")
    CERT_DIR.mkdir(parents=True, exist_ok=True)
    TEMP_DIR.mkdir(parents=True, exist_ok=True)

    ext = TEMP_DIR / "doh.ext"
    ext.write_text("basicConstraints=critical,CA:FALSE\n"
                   "keyUsage=critical,digitalSignature,keyEncipherment\n"
                   "extendedKeyUsage=serverAuth\n"
                   "subjectAltName=DNS:doh.sniroute.lan,IP:127.0.0.1\n",
                   encoding="utf-8")
    key, csr, crt = CERT_DIR / "doh.key", TEMP_DIR / "doh.csr", CERT_DIR / "doh.crt"
    for cmd in ([openssl, "req", "-new", "-nodes", "-newkey", "rsa:2048", "-sha256",
                 "-keyout", str(key), "-out", str(csr), "-subj", f"/O={load_sites()['ca']['organization']}/CN=doh.sniroute.lan"],
                [openssl, "x509", "-req", "-in", str(csr), "-CA", str(crt_ca),
                 "-CAkey", str(key_ca), "-CAcreateserial", "-days", "825",
                 "-sha256", "-extfile", str(ext), "-out", str(crt)]):
        res = run(cmd)
        if res.returncode != 0:
            die(f"openssl 失败（doh）：\n{res.stderr}")
    print(f"已签发 {crt.name:<14} SAN: doh.sniroute.lan, 127.0.0.1")


def cmd_gen_cert(args) -> None:
    openssl = find_openssl()
    cfg = load_sites()
    sites = select_sites(cfg, args.site)
    crt_ca, key_ca = CA_DIR / "rootCA.crt", CA_DIR / "rootCA.key"
    if not crt_ca.is_file() or not key_ca.is_file():
        die("根证书不存在，先跑 gen-ca。")

    CERT_DIR.mkdir(parents=True, exist_ok=True)
    TEMP_DIR.mkdir(parents=True, exist_ok=True)

    for site in sites:
        name, sans = site["name"], site["cert_san"]
        # CN 用第一个非通配的名字：通配符放 CN 不合法，也过时
        if not sans:
            # dns 模式只修正解析，不需要证书；没有 SAN 的站点直接跳过。
            print(f"跳过 {site['name']:<10} 无 cert_san（dns 模式不需要证书）")
            continue
        cn = next((s for s in sans if not s.startswith("*.")), sans[0])
        ext = TEMP_DIR / f"{name}.ext"
        ext.write_text("basicConstraints=critical,CA:FALSE\n"
                       "keyUsage=critical,digitalSignature,keyEncipherment\n"
                       "extendedKeyUsage=serverAuth\n"
                       f"subjectAltName={','.join('DNS:' + s for s in sans)}\n",
                       encoding="utf-8")

        key, csr, crt = CERT_DIR / f"{name}.key", TEMP_DIR / f"{name}.csr", CERT_DIR / f"{name}.crt"
        for cmd in ([openssl, "req", "-new", "-nodes", "-newkey", "rsa:2048", "-sha256",
                     "-keyout", str(key), "-out", str(csr),
                     "-subj", f"/O={cfg['ca']['organization']}/CN={cn}"],
                    [openssl, "x509", "-req", "-in", str(csr), "-CA", str(crt_ca),
                     "-CAkey", str(key_ca), "-CAcreateserial", "-days", str(args.days),
                     "-sha256", "-extfile", str(ext), "-out", str(crt)]):
            res = run(cmd)
            if res.returncode != 0:
                die(f"openssl 失败（{name}）：\n{res.stderr}")
        print(f"已签发 {crt.name:<14} SAN: {', '.join(sans)}")

    # DoH 端点证书一并签发：新机器上浏览器要靠它连本机 DoH（含 IP SAN）
    gen_doh_cert()


def cmd_install_ca(args) -> None:
    crt = CA_DIR / "rootCA.crt"
    if not crt.is_file():
        die("根证书不存在，先跑 gen-ca。")
    if not is_admin():
        die("需要管理员权限才能写入本机根证书存储。")
    res = run(["certutil", "-addstore", "-f", "Root", str(crt)])
    if res.returncode != 0:
        die(f"certutil 失败：\n{res.stdout}\n{res.stderr}")
    print("已安装到「受信任的根证书颁发机构」（本机）。")
    print('卸载：certutil -delstore Root "Sniroute Local CA"')


# --------------------------------------------------------------------------
# hosts
# --------------------------------------------------------------------------


def read_hosts() -> str:
    raw = HOSTS_PATH.read_bytes()
    for enc in ("utf-8-sig", "mbcs"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def write_hosts(text: str) -> None:
    # 留一个备份，出问题时不用凭记忆修
    backup = HOSTS_PATH.with_suffix(".sniroute.bak")
    if HOSTS_PATH.is_file() and not backup.exists():
        shutil.copy2(HOSTS_PATH, backup)
    HOSTS_PATH.write_bytes(text.encode("utf-8"))


def extract_block(text: str) -> str:
    match = re.search(rf"{re.escape(BLOCK_BEGIN)}.*?{re.escape(BLOCK_END)}", text, re.S)
    return match.group(0) if match else ""


def strip_block(text: str) -> str:
    return re.sub(rf"\n*{re.escape(BLOCK_BEGIN)}.*?{re.escape(BLOCK_END)}\n?", "\n",
                  text, flags=re.S)


def flush_dns() -> None:
    run(["ipconfig", "/flushdns"])


def cmd_hosts(args) -> None:
    if args.action == "show":
        block = extract_block(read_hosts())
        print(block if block else "（hosts 里没有 sniroute 区块）")
        return

    if not is_admin():
        die("需要管理员权限才能修改 hosts。")

    text = strip_block(read_hosts())
    if args.action == "install":
        names: list[str] = []
        skipped: list[str] = []
        for site in select_sites(load_sites(), args.site):
            # 默认不拦 enabled=false 的站点：把打不通的域名指到本机，等于把
            # 用户从"慢"直接推进"完全打不开"，比什么都不做更糟。
            if not site.get("enabled", True) and not args.include_disabled:
                skipped.append(f"{site['name']}（{site.get('disabled_reason', '未启用')[:40]}…）")
                continue
            for host in site["hosts"]:
                if host not in names:
                    names.append(host)
        for note in skipped:
            print(f"跳过 {note}")
        if not names:
            die("没有可拦截的域名。用 --include-disabled 可以强制包含未启用的站点。")
        block = [BLOCK_BEGIN, "# 由 sniroute 管理；删除本区块即可还原"]
        block += [f"127.0.0.1\t{name}" for name in names]
        block.append(BLOCK_END)
        text = text.rstrip("\r\n") + "\n\n" + "\n".join(block) + "\n"
        print(f"写入 {len(names)} 个域名 → 127.0.0.1")

    write_hosts(text)
    flush_dns()
    if args.action == "remove":
        print("已移除 sniroute 区块，并刷新 DNS 缓存。")


# --------------------------------------------------------------------------
# nginx 进程
# --------------------------------------------------------------------------


def nginx_cmd(nginx: str, *args: str) -> list:
    return [nginx, "-p", str(ROOT) + os.sep, "-c", "conf/nginx.conf", *args]


def pid_is_running(pid_file: Path) -> bool:
    """pid 文件存在不等于 nginx 在跑：`nginx -t` 和异常退出都会留下残留文件。"""
    if not pid_file.is_file():
        return False
    pid = pid_file.read_text(encoding="utf-8", errors="replace").strip()
    if not pid:
        return False
    return "nginx" in (run(["tasklist", "/FI", f"PID eq {pid}", "/NH"]).stdout or "").lower()


def cmd_service(args) -> None:
    nginx = find_nginx(args.nginx)
    pid_file = ROOT / "logs" / "nginx.pid"

    if args.action == "test":
        res = run(nginx_cmd(nginx, "-t"))
        print(res.stdout.strip() or res.stderr.strip())
        # `nginx -t` 自己也会写出 pid 文件。进程并不存在时把它清掉，
        # 否则下一条命令会被一个说谎的 pid 文件误导。
        if res.returncode == 0 and not pid_is_running(pid_file):
            pid_file.unlink(missing_ok=True)
        raise SystemExit(res.returncode)

    if args.action == "status":
        if not pid_file.is_file():
            print("未运行（没有 pid 文件）")
            return
        pid = pid_file.read_text(encoding="utf-8", errors="replace").strip()
        print(f"pid {pid} {'运行中' if pid_is_running(pid_file) else '已退出（残留 pid 文件）'}")
        return

    if args.action == "up":
        if pid_is_running(pid_file):
            die("已经在运行；用 reload，或先 down。")
        if pid_file.is_file():
            print("清理残留 pid 文件")
            pid_file.unlink()
        (ROOT / "logs").mkdir(parents=True, exist_ok=True)
        # nginx 在 Windows 上会占住控制台，所以脱离启动
        subprocess.Popen(nginx_cmd(nginx), cwd=str(ROOT),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=0x00000008)  # DETACHED_PROCESS
        time.sleep(1.2)
        print("已启动。" if pid_is_running(pid_file) else
              "启动命令已发出，但没看到运行中的 pid —— 去看 logs/error.log。")
        return

    sig = {"down": "-s stop", "reload": "-s reload"}[args.action]
    res = run(nginx_cmd(nginx, *sig.split()))
    if res.returncode != 0:
        die(f"nginx {sig} 失败：\n{res.stderr}")

    if args.action == "down":
        # 信号发出 ≠ 已经退出。等它真的结束再回话，否则下一条命令会拿到
        # "已执行 stop" 但进程仍在的错觉 —— 刚才就是这么被自己骗了一次。
        for _ in range(50):
            if not pid_is_running(pid_file):
                pid_file.unlink(missing_ok=True)
                print("已停止。")
                return
            time.sleep(0.1)
        die("发了 stop 信号，但进程 5 秒内没有退出（可能有长连接）。"
            "看 logs/error.log，或用 taskkill /F /IM nginx.exe。")

    print(f"已执行 nginx {sig}")


def cmd_stats(args) -> None:
    """从 nginx 访问日志里统计真实表现。

    日志格式（conf/nginx.conf）：远端地址 [时间] "域名" 状态码 字节数 耗时s
    刻意不含 $request —— 代理日志不该变成浏览历史，所以这里也统计不到 URL。
    """
    log = ROOT / "logs" / "access.log"
    if not log.is_file():
        die("没有 logs/access.log，先跑起来并浏览一会儿。")
    pattern = re.compile(r'"([^"]+)"\s+(\d{3})\s+(\d+)\s+([\d.]+)s')
    rows = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pattern.search(line)
        if not m:
            continue
        rows.append({"host": m.group(1), "status": int(m.group(2)),
                     "bytes": int(m.group(3)), "time": float(m.group(4))})

    if not rows:
        die("日志里没有可解析的记录。")

    def pct(values: list, q: float) -> float:
        if not values:
            return 0.0
        ordered = sorted(values)
        idx = min(len(ordered) - 1, int(len(ordered) * q))
        return ordered[idx]

    print(f"样本 {len(rows)} 条")
    print("\n按域名")
    print(f"  {'域名':<24}{'次数':>6}{'失败':>6}{'中位耗时':>10}{'p95':>9}{'最大':>9}")
    by_host: dict = {}
    for r in rows:
        by_host.setdefault(r["host"], []).append(r)
    for host, items in sorted(by_host.items(), key=lambda kv: -len(kv[1]))[:12]:
        times = [i["time"] for i in items]
        fails = sum(1 for i in items if i["status"] >= 400)
        print(f"  {host:<24}{len(items):>6}{fails:>6}"
              f"{pct(times, 0.5):>9.2f}s{pct(times, 0.95):>8.2f}s{max(times):>8.2f}s")

    print("\n按响应大小（耗时与大小强相关，分开看才有意义）")
    buckets = [(">500KB", 500_000), ("100KB-500KB", 100_000), ("10KB-100KB", 10_000), ("<10KB", 0)]
    for label, floor in buckets:
        subset = [r for r in rows if r["bytes"] >= floor and
                  (floor != 100_000 or r["bytes"] < 500_000) and
                  (floor != 10_000 or r["bytes"] < 100_000) and
                  (floor != 0 or r["bytes"] < 10_000)]
        if not subset:
            continue
        times = [r["time"] for r in subset]
        slow = sum(1 for t in times if t > 3)
        print(f"  {label:<14}{len(subset):>4} 个   中位 {pct(times, 0.5):>6.2f}s   "
              f"p95 {pct(times, 0.95):>6.2f}s   最大 {max(times):>6.2f}s   "
              f"超 3s: {slow}")

    print("\n调参前后对比就看这一段：大响应的中位与 p95 是否下降。")





# --------------------------------------------------------------------------
# DoH 子进程
# --------------------------------------------------------------------------


def port_listener_pid(port: int) -> int | None:
    """找出监听某端口的进程。返回 None 表示没人监听。"""
    res = run(["netstat", "-ano"])
    for line in (res.stdout or "").splitlines():
        if "LISTENING" not in line or ":" not in line:
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        local = parts[1]
        if local.rsplit(":", 1)[-1] == str(port):
            try:
                return int(parts[-1])
            except ValueError:
                continue
    return None


def doh_running() -> bool:
    """后端 Python 是否真的在跑（pid 文件可能陈旧，必须看端口）。"""
    pid = port_listener_pid(DOH_INTERNAL_PORT)
    if pid is None:
        # 端口没人听 -> 顺手清掉陈旧 pid 文件，否则 start 会误判"已在运行"而跳过。
        try:
            if DOH_PID.is_file():
                DOH_PID.unlink()
        except Exception:
            pass
        return False
    return True


def doh_front_running() -> bool:
    """浏览器实际访问的 5444 是否有人听（正常是 nginx 前置）。"""
    return port_listener_pid(DOH_PORT) is not None

DOH_PID = ROOT / "logs" / "doh.pid"
SITES_JSON = ROOT / "data" / "sites.json"


def cmd_doh(args) -> None:
    action = args.action

    # 后端在 5454；5444 是 nginx 前置，不是我们管的进程。
    pid = port_listener_pid(DOH_INTERNAL_PORT)

    if action == "export":
        cmd_warp_export(args)
        return

    if action == "status":
        print(f"DoH 后端 :{DOH_INTERNAL_PORT} {"running (pid " + str(pid) + ")" if pid else "not running"}")
        print(f"DoH 前置 :{DOH_PORT} {"listening" if doh_front_running() else "not listening"} (needs nginx)")
        if DOH_LOG.is_file():
            last = DOH_LOG.read_text(encoding="utf-8", errors="replace").splitlines()[-3:]
            for line in last:
                print(f"  {line}")
        return

    if action == "restart":
        if doh_running():
            cmd_doh(argparse.Namespace(action="stop"))
        return cmd_doh(argparse.Namespace(action="start"))

    if action == "stop":
        if not pid:
            print("DoH 未运行")
            return
        run(["taskkill", "/F", "/PID", str(pid)])
        for _ in range(30):
            if not doh_running():
                print("DoH 已停止。")
                return
            time.sleep(0.1)
        die(f"无法结束 pid {pid}（可能被其它会话托管）")
        return

    # start
    #
    # 判断顺序很关键，这里踩过两次：
    #   1) 先看 5454（后端自己的端口）。它在听就说明后端活着，别的都不用管。
    #   2) 5454 没听时才谈启动。此时 5444 可能由 nginx 前置占着 —— 那是正常的，
    #      不能当成"残留进程"去杀；杀了 ngixn 前置会让 5444 直接 502。
    if doh_running():
        try:
            mine = DOH_PID.is_file() and int(DOH_PID.read_text().strip()) == pid
        except Exception:
            mine = False
        stale = False
        try:
            pid_mtime = DOH_PID.stat().st_mtime
            newest = max(SITES_JSON.stat().st_mtime, DOH_SCRIPT.stat().st_mtime)
            stale = newest > pid_mtime
        except Exception:
            pass
        if stale:
            print(f"DoH 已在运行 (pid {pid})，但站点配置在那之后改过——正在重启以加载新配置。")
            cmd_doh(argparse.Namespace(action="stop"))
        else:
            print(f"DoH 已在运行 (pid {pid})，配置未变，跳过。")
            return
    if not DOH_SCRIPT.is_file():
        die(f"缺少 {DOH_SCRIPT}")
    (ROOT / "logs").mkdir(parents=True, exist_ok=True)
    subprocess.Popen(
        [sys.executable, str(DOH_SCRIPT), "--listen", f"127.0.0.1:{DOH_INTERNAL_PORT}",
         "--no-tls", "--log", str(DOH_LOG), "--dns-port", str(PLAIN_DNS_PORT)],
        cwd=str(ROOT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=0x00000008)  # DETACHED_PROCESS
    for _ in range(40):
        if doh_running():
            try:
                DOH_PID.write_text(str(port_listener_pid(DOH_PORT) or ""), encoding="utf-8")
            except Exception:
                pass
            print(f"DoH 后端已启动 (127.0.0.1:{DOH_INTERNAL_PORT}，明文；"
                  f"浏览器走 nginx 前置 :{DOH_PORT} 的 h2)")
            return
        time.sleep(0.1)
    die("DoH 启动后没有监听，去看 logs/ 或手动跑 tools\\doh_ech.py")


# --------------------------------------------------------------------------
# 一键启停：DoH + nginx
# --------------------------------------------------------------------------


def hosts_has_doh_entry() -> bool:
    try:
        text = HOSTS_PATH.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return DOH_BLOCK_BEGIN in text


# --------------------------------------------------------------------------
# 中继（relay）
#
# 有些目标域名在当前网络里没有直连出路（YouTube 视频、Twitch VOD、Discord 更新）。
# 做法：本机 nginx 向 tools/relay.py 发真实 SNI，relay 从 ClientHello 里读出目标
# 主机名，经 SOCKS5（ssh -D 提供）连到目标 443，再透传字节。SNI 只在隧道内部出现。
#
# 服务器必须在墙外：墙内的机器自己也被封锁（实测武汉 ECS 拿到真实 IP 仍超时）。
# --------------------------------------------------------------------------
RELAY_CFG = ROOT / "data" / "relay.json"
RELAY_SCRIPT = ROOT / "tools" / "relay.py"


def load_relay_cfg() -> dict:
    if not RELAY_CFG.is_file():
        return {}
    try:
        return json.loads(RELAY_CFG.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _port_of(addr: str) -> int:
    return int(addr.rpartition(":")[2])


def relay_status() -> dict:
    cfg = load_relay_cfg()
    listen = cfg.get("listen", "127.0.0.1:18955")
    socks = cfg.get("socks", "127.0.0.1:18080")
    return {
        "enabled": bool(cfg.get("enabled")),
        "relay_pid": port_listener_pid(listen),
        "socks_pid": port_listener_pid(socks),
        "listen": listen,
        "socks": socks,
    }


def cmd_relay(args) -> None:
    cfg = load_relay_cfg()
    action = args.action
    st = relay_status()

    if action == "status":
        print(f"中继配置 : {'已启用' if st['enabled'] else '未启用'}（data/relay.json）")
        print(f"隧道 SOCKS : {st['socks']}  {'运行中 pid ' + str(st['socks_pid']) if st['socks_pid'] else '未运行'}")
        print(f"中继监听   : {st['listen']}  {'运行中 pid ' + str(st['relay_pid']) if st['relay_pid'] else '未运行'}")
        ssh_cfg = cfg.get("ssh") or {}
        if ssh_cfg:
            print(f"服务器     : {ssh_cfg.get('user')}@{ssh_cfg.get('host')}:{ssh_cfg.get('port', 22)}")
        return

    if action == "stop":
        if st["relay_pid"]:
            run(["taskkill", "/F", "/PID", str(st["relay_pid"])])
        if st["socks_pid"]:
            run(["taskkill", "/F", "/PID", str(st["socks_pid"])])
        print("中继与隧道已停止。")
        return

    # start
    if not st["enabled"]:
        die("中继未启用：把 data/relay.json 的 enabled 改成 true 并填好 ssh.host/key。")
    ssh_cfg = cfg.get("ssh") or {}
    if not ssh_cfg.get("host") or not ssh_cfg.get("key"):
        die("data/relay.json 缺少 ssh.host 或 ssh.key")

    if not st["socks_pid"]:
        socks_host, _, socks_port = st["socks"].rpartition(":")
        logs = ROOT / "logs"
        logs.mkdir(parents=True, exist_ok=True)
        subprocess.Popen(
            ["ssh", "-N", "-D", f"{socks_host}:{socks_port}",
             "-i", str(ssh_cfg["key"]),
             "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
             "-o", "ServerAliveInterval=30", "-o", "ExitOnForwardFailure=yes",
             "-p", str(ssh_cfg.get("port", 22)),
             f"{ssh_cfg.get('user', 'root')}@{ssh_cfg['host']}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=0x00000008)  # DETACHED_PROCESS
        for _ in range(30):
            if port_listener_pid(st["socks"]):
                break
            time.sleep(0.2)
        if port_listener_pid(st["socks"]):
            print(f"隧道已建立（SOCKS {st['socks']}）")
        else:
            die(f"隧道没有起来：ssh 连不上 {ssh_cfg.get('host')}，检查密钥权限与网络")

    if not st["relay_pid"]:
        listen_host, _, listen_port = st["listen"].rpartition(":")
        subprocess.Popen(
            [sys.executable, str(RELAY_SCRIPT), "--listen", st["listen"], "--socks", st["socks"]],
            cwd=str(ROOT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=0x00000008)
        for _ in range(30):
            if port_listener_pid(st["listen"]):
                break
            time.sleep(0.2)
        if port_listener_pid(st["listen"]):
            print(f"中继已启动（监听 {st['listen']}，出口 {st['socks']}）")
        else:
            die("中继没有起来，检查 tools/relay.py 与端口是否被系统保留")




def cmd_hash(args) -> None:
    """计算文件的 SHA-256 / SHA-1 / MD5，并与同名 sidecar 比对。

    为什么需要这个命令：安装包的"详细信息"里放不了它自己的哈希（写进去哈希就变了），
    所以校验值以同名 .sha256 / .sha1 / .md5 的形式放在旁边，这里负责算并核对。
    """
    import hashlib
    target = Path(args.file)
    if not target.is_file():
        die(f"文件不存在: {target}")

    data = target.read_bytes()
    digests = {
        "SHA256": hashlib.sha256(data).hexdigest(),
        "SHA1": hashlib.sha1(data).hexdigest(),
        "MD5": hashlib.md5(data).hexdigest(),
    }
    print(f"文件: {target}")
    print(f"大小: {len(data):,} 字节")
    for name, value in digests.items():
        print(f"  {name:6}: {value}")

    print()
    ok = True
    for name, suffix in (("SHA256", ".sha256"), ("SHA1", ".sha1"), ("MD5", ".md5")):
        side = Path(str(target) + suffix)
        if not side.is_file():
            print(f"  {name}: 没有 {side.name}，跳过比对")
            continue
        text = side.read_text(encoding="utf-8", errors="ignore").strip()
        expected = text.split()[0].lower() if text else ""
        match = expected == digests[name].lower()
        ok = ok and match
        print(f"  {name}: {'一致' if match else '不一致'}（{side.name}）")
    sys.exit(0 if ok else 1)


def cmd_refresh(args) -> None:
    """候选地址健康检查与刷新：转调 tools/refresh-upstreams.py。"""
    script = ROOT / "tools" / "refresh-upstreams.py"
    if not script.is_file():
        die(f"缺少 {script}")
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    cmd = [sys.executable, str(script)]
    if getattr(args, "dry_run", False):
        cmd.append("--dry-run")
    if getattr(args, "quiet", False):
        cmd.append("--quiet")
    sys.exit(subprocess.call(cmd, env=env))


def cmd_nat(args) -> None:
    """NAT 类型检测：转调 tools/natcheck.py，保持输出原样。"""
    script = ROOT / "tools" / "natcheck.py"
    if not script.is_file():
        die(f"缺少 {script}")
    cmd = [sys.executable, str(script)]
    if args.server:
        cmd += ["--server", args.server]
    if args.server2:
        cmd += ["--server2", args.server2]
    if args.timeout_ms:
        cmd += ["--timeout", str(args.timeout_ms)]
    if args.json:
        cmd += ["--json"]
    if args.summary:
        cmd += ["--summary"]
    # 子进程输出要强制 UTF-8：Windows 下 Python 接管道时默认用本地代码页（cp936），
    # 托盘那边按 UTF-8 解码就会得到乱码。
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    sys.exit(subprocess.call(cmd, env=env))




# ---------------------------------------------------------------- 检查更新
REPO_SLUG = "Kynarain/SniRoute"
VERSION_FILE = ROOT / "data" / "version.txt"


def local_version() -> str:
    try:
        return VERSION_FILE.read_text(encoding="utf-8").strip() or "unknown"
    except Exception:
        return "unknown"


def _remote_version_via_gh() -> tuple[str | None, str]:
    """用 gh CLI 读仓库里的 data/version.txt。私有仓库需要认证，
    gh 已登录时最省事。返回 (version, 错误说明)。"""
    import subprocess as _sp
    try:
        proc = _sp.run(["gh", "api", f"repos/{REPO_SLUG}/contents/data/version.txt",
                        "-H", "Accept: application/vnd.github.raw"],
                       capture_output=True, text=True, timeout=25)
        if proc.returncode == 0 and proc.stdout.strip():
            return proc.stdout.strip().splitlines()[0], ""
        return None, (proc.stderr or "").strip()[:120] or f"gh 退出码 {proc.returncode}"
    except FileNotFoundError:
        return None, "未安装 gh"
    except Exception as exc:
        return None, f"{type(exc).__name__}: {exc}"


def _remote_version_via_token() -> tuple[str | None, str]:
    """没有 gh 时，用环境变量里的 GITHUB_TOKEN/GH_TOKEN 直接调 API。"""
    import os as _os
    import urllib.request as _ur
    token = _os.environ.get("GITHUB_TOKEN") or _os.environ.get("GH_TOKEN")
    if not token:
        return None, "未设置 GITHUB_TOKEN/GH_TOKEN"
    url = f"https://api.github.com/repos/{REPO_SLUG}/contents/data/version.txt"
    req = _ur.Request(url, headers={
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github.raw",
        "User-Agent": "sniroute-update-check",
    })
    try:
        with _ur.urlopen(req, timeout=25) as resp:
            return resp.read().decode("utf-8", "replace").strip(), ""
    except Exception as exc:
        return None, f"{type(exc).__name__}: {exc}"


def _semver_tuple(v: str) -> tuple:
    parts = []
    for piece in str(v).strip().lstrip("v").split("."):
        digits = "".join(ch for ch in piece if ch.isdigit())
        parts.append(int(digits) if digits else 0)
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])



# ---------------------------------------------------------------- GitHub Actions
#
# 用途：把 GitHub 的 runner（墙外）当成"取文件"的通道。
#   gha fetch <url>   触发仓库里的 fetch.yml 下载，然后把 artifact 取回本地
#   gha status        看认证来源、仓库、最近几次运行
#   gha auth --token  保存一个 PAT（没有 gh CLI 时用）
#
# 认证顺序：
#   1) 已登录的 gh CLI（最省事，token 由它管理，不落到我们文件里）
#   2) data/github-token.json 里的 PAT（本机保存，文件权限由用户自己负责）
GH_SLUG = "Kynarain/SniRoute"
GH_TOKEN_FILE = ROOT / "data" / "github-token.json"


def _gh_cfg() -> dict:
    try:
        import json as _json
        return _json.loads((ROOT / "data" / "github.json").read_text(encoding="utf-8"))
    except Exception:
        return {}


def _gh_login() -> str | None:
    """当前 gh CLI 登录账号（不依赖网络：gh auth token 只读本地 keyring）。"""
    if not gh_cli_ok():
        return None
    try:
        proc = _gh_run(["gh", "api", "user", "--jq", ".login"], timeout=25)
        login = (proc.stdout or "").strip()
        if login and proc.returncode == 0:
            return login
    except Exception:
        pass
    # 网络不通时用 auth status 的文本兜底：它会把账号名打出来
    try:
        proc = _gh_run(["gh", "auth", "status"], timeout=20)
        import re as _re
        m = _re.search(r"account (\S+)", (proc.stdout or "") + (proc.stderr or ""))
        if m:
            return m.group(1)
    except Exception:
        pass
    return None


def gh_slug() -> str:
    """workflow 跑在哪个仓库。

    顺序：
      1) data/github.json 里显式配置
      2) **当前 gh 登录账号**下的同名仓库 —— 别人 fork 后就是这个形态，
         不需要改配置、更不需要作者的 token
      3) 作者仓库（仅兜底）
    """
    cfg = _gh_cfg().get("repo")
    if cfg:
        return cfg
    login = _gh_login()
    if login:
        return f"{login}/SniRoute"
    return GH_SLUG


def gh_token() -> str | None:
    import os as _os
    tok = _os.environ.get("GITHUB_TOKEN") or _os.environ.get("GH_TOKEN")
    if tok:
        return tok
    try:
        import json as _json
        return _json.loads(GH_TOKEN_FILE.read_text(encoding="utf-8")).get("token")
    except Exception:
        return None



def _gh_run(cmd, timeout=60, **_ignored):
    """跑外部命令并用 UTF-8 解码输出（gh 输出是 UTF-8，Windows 默认 GBK 会炸）。

    **_ignored：兼容调用处残留的 capture_output/text 参数——本函数始终捕获并解码。
    """
    return subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def gh_cli_ok() -> bool:
    """本机是否有可用的 gh 登录。

    注意：`gh auth status` 会连 api.github.com，网络抖动时返回非 0，但凭据仍在本地。
    所以网络失败时退一步用 `gh auth token`（纯本地读 keyring）判断是否登录过；
    网络错误应该在真正发请求时报，而不是被误判成"没登录"。
    """
    import shutil as _sh
    if not _sh.which("gh"):
        return False
    try:
        if _gh_run(["gh", "auth", "status"], timeout=15).returncode == 0:
            return True
    except Exception:
        pass
    try:
        proc = _gh_run(["gh", "auth", "token"], timeout=15)
        return proc.returncode == 0 and bool((proc.stdout or "").strip())
    except Exception:
        return False


def gh_api(method: str, path: str, body=None, raw: bool = False):
    """走 REST API（用户 token）。返回 (status, data)。"""
    import json as _json
    import urllib.request as _ur
    tok = gh_token()
    if not tok:
        raise RuntimeError("没有 GitHub 凭据（先 gh auth login，或用 gha auth --token）")
    url = path if path.startswith("http") else f"https://api.github.com{path}"
    data = _json.dumps(body).encode() if body is not None else None
    req = _ur.Request(url, data=data, method=method, headers={
        "Authorization": f"Bearer {tok}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "sniroute-gha",
        "Content-Type": "application/json",
    })
    with _ur.urlopen(req, timeout=60) as resp:
        payload = resp.read()
        return resp.status, (payload if raw else _json.loads(payload or b"{}"))



def _gh_repo_exists(slug: str) -> bool | None:
    """仓库是否存在。None = 无法判断（网络问题）。"""
    try:
        if gh_cli_ok():
            proc = _gh_run(["gh", "api", f"repos/{slug}", "--jq", ".full_name"], timeout=25)
            if proc.returncode == 0:
                return True
            blob = (proc.stderr or "") + (proc.stdout or "")
            if "404" in blob or "Not Found" in blob:
                return False
            return None
        try:
            gh_api("GET", f"/repos/{slug}")
            return True
        except Exception as exc:
            if "404" in str(exc):
                return False
            return None
    except Exception:
        return None


def _gh_ensure_repo(slug: str) -> None:
    """确认目标仓库存在；不存在就停下来说明怎么做（**不自动 fork**）。

    为什么不自作主张：fork 会在用户账号下创建仓库，属于对外可见的操作，
    必须由用户明确同意。这里只给指引，真正的 fork 由 `gha fork` 完成，
    而 `gha fork` 自己会先问一次。
    """
    exists = _gh_repo_exists(slug)
    if exists:
        return
    if exists is None:
        print(f"  无法确认 {slug} 是否存在（网络问题），继续尝试")
        return

    login = _gh_login() or ""
    if login and slug == f"{login}/SniRoute":
        die(f"你的账号下还没有 {slug}。\n"
            f"      运行下面的命令来 fork（会先征求你的确认）：\n"
            f"        sniroute.cmd gha fork")
    die(f"仓库 {slug} 不存在。\n"
        f"      用 gha config 指定一个已有仓库，或先 fork：gh repo fork {GH_SLUG} --clone=false")


def cmd_gha_fork(args) -> None:
    """显式 fork 作者仓库到当前账号（需要确认）。"""
    import time as _t

    login = _gh_login()
    if not login:
        die("没有 gh 登录，无法确定 fork 到哪个账号（先 gh auth login）")
    target = f"{login}/SniRoute"
    if _gh_repo_exists(target):
        print(f"{target} 已存在，无需 fork。")
        return

    if not args.yes:
        print(f"即将把 {GH_SLUG} fork 到你的账号：{target}")
        print("（这会在你的 GitHub 账号下创建一个公开仓库）")
        try:
            answer = input("继续？[y/N] ").strip().lower()
        except EOFError:
            answer = ""
        if answer not in ("y", "yes"):
            print("已取消，未做任何改动。")
            return

    if gh_cli_ok():
        proc = _gh_run(["gh", "repo", "fork", GH_SLUG, "--clone=false"], timeout=120)
        if proc.returncode != 0:
            die(f"fork 失败：{((proc.stderr or proc.stdout).strip())[:200]}")
    else:
        try:
            gh_api("POST", f"/repos/{GH_SLUG}/forks", {})
        except Exception as exc:
            die(f"fork 失败：{exc}")

    for _ in range(20):
        _t.sleep(3)
        if _gh_repo_exists(target):
            print(f"fork 完成：{target}")
            return
    print(f"fork 已发起，但 {target} 还没出现。稍等片刻后运行 gha status 确认。")


def _gha_dispatch(url: str, name: str) -> None:
    slug = gh_slug()
    _gh_ensure_repo(slug)
    if gh_cli_ok():
        cmd = ["gh", "workflow", "run", "fetch.yml", "-R", slug, "-f", f"url={url}"]
        if name:
            cmd += ["-f", f"filename={name}"]
        proc = _gh_run(cmd, capture_output=True, text=True, timeout=60)
        if proc.returncode != 0:
            raise RuntimeError((proc.stderr or proc.stdout).strip()[:200])
        print(f"  已触发（gh CLI）：{slug} fetch.yml")
        return
    gh_api("POST", f"/repos/{slug}/actions/workflows/fetch.yml/dispatches",
           {"ref": "main", "inputs": {"url": url, "filename": name or ""}})
    print(f"  已触发（REST）：{slug} fetch.yml")


def _gha_wait_run(since_iso: str, timeout_s: int = 600) -> dict:
    import time as _t
    slug = gh_slug()
    deadline = _t.time() + timeout_s
    while _t.time() < deadline:
        if gh_cli_ok():
            proc = _gh_run(
                ["gh", "run", "list", "-R", slug, "--workflow=fetch.yml", "--limit", "5",
                 "--json", "databaseId,status,conclusion,createdAt"],
                capture_output=True, text=True, timeout=60)
            if proc.returncode == 0:
                runs = __import__("json").loads(proc.stdout or "[]")
                for r in runs:
                    if r.get("createdAt", "") >= since_iso and r.get("status") == "completed":
                        return {"id": r["databaseId"], "conclusion": r.get("conclusion")}
        else:
            _, data = gh_api("GET", f"/repos/{slug}/actions/runs?per_page=10")
            for r in data.get("workflow_runs", []):
                if r.get("created_at", "") >= since_iso and r.get("status") == "completed":
                    return {"id": r["id"], "conclusion": r.get("conclusion")}
        _t.sleep(5)
    raise RuntimeError("等待运行超时")


def _gha_download(run_id: int, out_dir: Path) -> Path | None:
    import json as _json
    import shutil as _sh
    import tempfile as _tf
    import urllib.request as _ur
    import zipfile as _zf
    slug = gh_slug()
    if gh_cli_ok():
        proc = _gh_run(["gh", "run", "download", str(run_id), "-R", slug,
                               "-n", "fetch", "-D", str(out_dir)],
                              capture_output=True, text=True, timeout=300)
        if proc.returncode != 0:
            raise RuntimeError((proc.stderr or "").strip()[:200])
        files = [f for f in out_dir.rglob("*") if f.is_file()]
        return files[0] if files else None

    _, data = gh_api("GET", f"/repos/{slug}/actions/runs/{run_id}/artifacts")
    arts = [a for a in data.get("artifacts", []) if a.get("name") == "fetch"]
    if not arts:
        return None
    tmp_zip = Path(_tf.gettempdir()) / f"gha-{run_id}.zip"
    tok = gh_token()
    req = _ur.Request(arts[0]["archive_download_url"], headers={
        "Authorization": f"Bearer {tok}", "User-Agent": "sniroute-gha"})
    with _ur.urlopen(req, timeout=300) as resp, open(tmp_zip, "wb") as fh:
        _sh.copyfileobj(resp, fh)
    with _zf.ZipFile(tmp_zip) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        if not names:
            return None
        first = names[0]
        target = out_dir / Path(first).name
        out_dir.mkdir(parents=True, exist_ok=True)
        with z.open(first) as src, open(target, "wb") as dst:
            _sh.copyfileobj(src, dst)
    tmp_zip.unlink(missing_ok=True)
    return target



# ---------------------------------------------------------------- GitHub 登录（设备码）
#
# 为什么需要它：gha 依赖 GitHub 凭据。gh CLI 能登录，但不是每台机器都装了 gh；
# 手填 PAT 又需要用户自己去 Setting 里点。设备码流程是"最像正常 App 登录"的方式：
#   1. 我们向 GitHub 请求一个设备码（user_code）
#   2. 用户在浏览器打开 github.com/login/device 输入这个码并授权
#   3. 我们在后台轮询，拿到 token
#
# 前提：需要一个**已注册的 OAuth App**（client_id）。GitHub 要求所有设备码流程
# 都绑定一个应用，不能凭空发起。注册步骤见 README，一次就够。
GH_OAUTH_CLIENT_ID = ""


def _gh_device_code_poll(client_id: str) -> str | None:
    """完成设备码登录，返回 token（失败返回 None）。"""
    import json as _json
    import time as _t
    import urllib.parse as _up
    import urllib.request as _ur

    def post(url: str, fields: dict) -> dict:
        data = _up.urlencode(fields).encode()
        req = _ur.Request(url, data=data, headers={
            "Accept": "application/json",
            "Content-Type": "application/x-www-form-urlencoded",
            "User-Agent": "sniroute-gha",
        })
        with _ur.urlopen(req, timeout=30) as resp:
            return _json.loads(resp.read() or b"{}")

    start = post("https://github.com/login/device/code",
                 {"client_id": client_id, "scope": "repo"})
    if "device_code" not in start:
        print(f"  请求设备码失败: {start}")
        return None
    print(f"  浏览器打开: {start.get('verification_uri', 'https://github.com/login/device')}")
    print(f"  输入代码  : {start['user_code']}")
    print(f"  有效期    : {start.get('expires_in', 900)} 秒")
    interval = int(start.get("interval", 5))
    deadline = _t.time() + int(start.get("expires_in", 900))
    while _t.time() < deadline:
        _t.sleep(interval)
        res = post("https://github.com/login/oauth/access_token", {
            "client_id": client_id,
            "device_code": start["device_code"],
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
        })
        if res.get("access_token"):
            return res["access_token"]
        err = res.get("error", "")
        if err == "authorization_pending":
            continue
        if err == "slow_down":
            interval += 5
            continue
        print(f"  登录失败: {err} {res.get('error_description', '')}")
        return None
    print("  设备码已过期")
    return None


def cmd_gha_login(args) -> None:
    import json as _json
    import os as _os

    client_id = args.client_id or _os.environ.get("GH_OAUTH_CLIENT_ID") or GH_OAUTH_CLIENT_ID
    if not client_id:
        die("缺少 OAuth App 的 client_id。\n"
            "      1) GitHub → Settings → Developer settings → OAuth Apps → New OAuth App\n"
            "      2) 勾选 Enable Device Flow，把 Client ID 记下来\n"
            "      3) 运行: sniroute.cmd gha login --client-id <那串 Client ID>\n"
            "\n"
            "      注意：设备码流程走的是 github.com（不是 api.github.com）。\n"
            "      实测这边 github.com 经常不可达，若登录卡住，改用 PAT：\n"
            "        - 用 Tor Browser / 其它出口打开 github.com 生成 PAT（scope: repo）\n"
            "        - 运行: sniroute.cmd gha auth --token <PAT>")
    token = _gh_device_code_poll(client_id)
    if not token:
        die("登录未完成")
    GH_TOKEN_FILE.write_text(_json.dumps({"token": token, "via": "device-flow"}, indent=2),
                             encoding="utf-8")
    print(f"已保存凭据：{GH_TOKEN_FILE}")
    print("提示：想更安全的话，装 gh CLI 并 gh auth login，然后把该文件删掉。")



# ---------------------------------------------------------------- 云通道（自带账号）
#
# 设计目标：每个用户用自己的账号和额度，不在工具里内置任何共享凭据。
#
# 通道分两类：
#   基础设施型（自动探测）：tor / warp / github-actions
#   URL 模板型（用户自建）：任何能"接收 ?url= 并返回内容"的 HTTP 端点
#     Cloudflare Worker / AWS Lambda Function URL / Deno Deploy / Vercel...
#
# 模板里用 {url} 占位（会被 urlencode），{url_raw} 表示不编码。
CLOUD_CFG = ROOT / "data" / "cloud.json"


def _cloud_load() -> dict:
    import json as _json
    try:
        return _json.loads(CLOUD_CFG.read_text(encoding="utf-8"))
    except Exception:
        return {"providers": {}}


def _cloud_save(doc: dict) -> None:
    import json as _json
    CLOUD_CFG.write_text(_json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")


def cloud_detect() -> dict:
    """探测本机可用的基础设施型通道。"""
    found = {}
    # Tor：SOCKS 9150（Tor Browser）或 9050（系统 tor）
    for port in (9150, 9050):
        if port_listener_pid(port):
            found["tor"] = {"type": "socks5", "host": "127.0.0.1", "port": port}
            break
    # WARP：warp-cli 存在且已注册
    if find_warp_cli() is not None and warp_state() != "missing":
        found["warp"] = {"type": "system-tunnel"}
    # GitHub Actions
    if gh_cli_ok() or gh_token():
        found["github-actions"] = {"type": "gha", "repo": gh_slug()}
    return found


def _urlencode(s: str) -> str:
    import urllib.parse as _up
    return _up.quote(s, safe="")


def cmd_cloud(args) -> None:
    import json as _json
    import os as _os
    import time as _t
    import shutil as _sh
    import urllib.request as _ur

    doc = _cloud_load()
    providers = doc.setdefault("providers", {})
    action = args.action

    if action == "list":
        print("基础设施型（自动探测）：")
        det = cloud_detect()
        for name, info in det.items():
            print(f"  {name:<16} {info}")
        if not det:
            print("  （无：没有 Tor / WARP / gh 登录）")
        print("\n自建 URL 通道：")
        if not providers:
            print("  （无：用 cloud add --name ... --template ... 添加）")
        for name, info in providers.items():
            tok = "有" if info.get("token") else "无"
            print(f"  {name:<16} {info.get('template','')}  token={tok}")
        return

    if action == "add":
        if not args.name or not args.template:
            die("用法：sniroute.cmd cloud add --name cf --template \"https://x/proxy?url={url}\" [--token T]")
        if "{url}" not in args.template and "{url_raw}" not in args.template:
            die("模板里必须有 {url} 或 {url_raw} 占位符")
        providers[args.name] = {
            "template": args.template,
            "token": args.token or "",
            "added": _t.strftime("%Y-%m-%dT%H:%M:%S"),
        }
        _cloud_save(doc)
        print(f"已添加通道 {args.name}")
        print("凭据存在 data/cloud.json（已 gitignore）。")
        return

    if action == "remove":
        if args.name in providers:
            providers.pop(args.name)
            _cloud_save(doc)
            print(f"已移除 {args.name}")
        else:
            die(f"没有 {args.name}")
        return

    if action == "fetch":
        if not args.url:
            die("用法：sniroute.cmd cloud fetch <url> [--provider name] [-o 文件]")
        name = args.provider or ""
        det = cloud_detect()
        if not name:
            for cand in ("tor", "warp", "github-actions"):
                if cand in det:
                    name = cand
                    break
            if not name and providers:
                name = next(iter(providers))
        if not name:
            die("没有可用通道。先 cloud list 看看，或 cloud add 一个。")

        out = Path(args.output) if args.output else (Path.cwd() / "cloud-download.bin")

        if name == "tor":
            info = det.get("tor")
            if not info:
                die("Tor 未运行（SOCKS 9150/9050 都没监听）")
            cmd = ["curl.exe", "-s", "--ssl-no-revoke", "-L", "-m", "180",
                   "--socks5-hostname", f"{info['host']}:{info['port']}",
                   "-o", str(out), "-w", "%{http_code}", args.url]
            proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            code = (proc.stdout or "").strip()
            if code == "200" and out.is_file():
                print(f"已通过 Tor 保存 {out}（{out.stat().st_size} 字节）")
            else:
                die(f"Tor 通道失败（HTTP {code}）: {(proc.stderr or '')[:150]}")
            return

        if name == "warp":
            if warp_state() != "connected":
                die("WARP 未连接：sniroute.cmd warp on")
            cmd = ["curl.exe", "-s", "--ssl-no-revoke", "-L", "-m", "180",
                   "-o", str(out), "-w", "%{http_code}", args.url]
            proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            code = (proc.stdout or "").strip()
            print(f"已通过 WARP 保存 {out}" if code == "200" and out.is_file()
                  else f"WARP 通道失败（HTTP {code}）")
            return

        if name == "github-actions":
            ns = __import__("argparse").Namespace(
                action="fetch", url=args.url, output=str(out), name="", token="", yes=False)
            cmd_gha(ns)
            return

        info = providers.get(name)
        if not info:
            die(f"未知通道 {name}（cloud list 看可用的）")
        url = info["template"].replace("{url}", _urlencode(args.url)).replace("{url_raw}", args.url)
        headers = {"User-Agent": "sniroute-cloud"}
        if info.get("token"):
            headers["X-Relay-Token"] = info["token"]
            headers["Authorization"] = f"Bearer {info['token']}"
        print(f"通过 {name} 请求…")
        req = _ur.Request(url, headers=headers)
        with _ur.urlopen(req, timeout=300) as resp:
            data = resp.read()
        # 如果响应是 JSON 且含 base64 字段，按约定解出来
        try:
            j = _json.loads(data)
            if isinstance(j, dict) and j.get("encoding") == "base64":
                import base64 as _b64
                data = _b64.b64decode(j["body"])
        except Exception:
            pass
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print(f"已通过 {name} 保存 {out}（{len(data)} 字节）")
        return

    if action == "test":
        name = args.provider or ""
        det = cloud_detect()
        targets = [name] if name else list(det) + list(providers)
        if not targets:
            die("没有可测的通道")
        for t in targets:
            print(f"  {t:<16} {'探测到' if (t in det or t in providers) else '未配置'}")
        return

    die(f"未知动作 {action!r}")


def cmd_gha(args) -> None:
    import datetime
    import shutil as _sh
    import tempfile as _tf
    action = args.action

    if action == "fork":
        cmd_gha_fork(args)
        return

    if action == "config":
        # 别人用这份工具时，必须把仓库改成他们自己的 fork，否则会往作者仓库发
        # workflow_dispatch（需要写权限，对方也不该有）。默认值只适合本机作者。
        import json as _json
        cfg_path = ROOT / "data" / "github.json"
        if not args.repo:
            print(f"当前仓库: {gh_slug()}")
            print(f"配置文件: {cfg_path}（不存在则用默认 {GH_SLUG}）")
            print("用法: sniroute.cmd gha config --repo <owner/name>")
            return
        slug = args.repo.strip()
        if "/" not in slug:
            die("仓库格式应为 owner/name")
        cfg_path.write_text(_json.dumps({"repo": slug}, indent=2), encoding="utf-8")
        print(f"已设置仓库: {slug}")
        print("该仓库需要有 .github/workflows/fetch.yml（fork 本仓库即可）。")
        return

    if action == "login":
        cmd_gha_login(args)
        return

    if action == "auth":
        if not args.token:
            die("用法：sniroute.cmd gha auth --token <PAT>（scope 至少 repo）")
        import json as _json
        GH_TOKEN_FILE.write_text(_json.dumps({"token": args.token.strip()}, indent=2), encoding="utf-8")
        print(f"已保存凭据：{GH_TOKEN_FILE}")
        print("注意：这是明文保存的 PAT，别把该文件同步到别处。")
        return

    if action == "status":
        print(f"仓库     : {gh_slug()}")
        if gh_cli_ok():
            print("认证来源 : gh CLI（已登录）")
        elif gh_token():
            print("认证来源 : 本地 PAT（data/github-token.json）")
        else:
            print("认证来源 : 无（先 gh auth login，或 gha auth --token）")
            return
        try:
            if gh_cli_ok():
                proc = _gh_run(["gh", "run", "list", "-R", gh_slug(),
                                       "--workflow=fetch.yml", "--limit", "5",
                                       "--json", "databaseId,status,conclusion,createdAt"],
                                      capture_output=True, text=True, timeout=60)
                runs = __import__("json").loads(proc.stdout or "[]")
            else:
                _, data = gh_api("GET", f"/repos/{gh_slug()}/actions/runs?per_page=5")
                runs = [{"databaseId": r["id"], "status": r["status"],
                         "conclusion": r["conclusion"], "createdAt": r["created_at"]}
                        for r in data.get("workflow_runs", [])]
            print("\n最近运行：")
            for r in runs:
                print(f"  {r['databaseId']}  {r['status']:<10} {str(r.get('conclusion')):<10} {r.get('createdAt','')}")
        except Exception as exc:
            print(f"  读取运行列表失败：{exc}")
        return

    if action == "fetch":
        if not args.url:
            die("用法：sniroute.cmd gha fetch <url> [-o 输出文件] [--name 文件名]")
        if not (gh_cli_ok() or gh_token()):
            die("没有 GitHub 凭据：先 gh auth login，或 sniroute.cmd gha auth --token <PAT>")
        name = args.name or ""
        since = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        _gha_dispatch(args.url, name)
        print("  等待运行完成…")
        run = _gha_wait_run(since)
        if run.get("conclusion") != "success":
            die(f"运行失败（conclusion={run.get('conclusion')}，run id {run['id']}）")
        tmp = Path(_tf.gettempdir()) / f"gha-out-{run['id']}"
        _sh.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True, exist_ok=True)
        got = _gha_download(run["id"], tmp)
        if not got:
            die("没有拿到 artifact")
        # -o 的语义：指向已存在的目录（或以斜杠结尾）就放进去，否则当作文件名。
        # 托盘传的是 <Downloads> 目录，这样文件名仍然是 artifact 自己的名字。
        if args.output:
            out = Path(args.output)
            if out.is_dir() or str(args.output).endswith(("\\", "/")):
                out = out / got.name
        else:
            out = Path.cwd() / got.name
        out.parent.mkdir(parents=True, exist_ok=True)
        _sh.copy2(got, out)
        print(f"已保存 {out}  （{out.stat().st_size} 字节）")
        print(f"  run id {run['id']}，源 {args.url}")
        return

    if action == "runs":
        if not (gh_cli_ok() or gh_token()):
            die("没有 GitHub 凭据")
        if gh_cli_ok():
            proc = _gh_run(["gh", "run", "list", "-R", gh_slug(), "--limit", "10"],
                                  capture_output=True, text=True, timeout=60)
            print(proc.stdout)
        else:
            _, data = gh_api("GET", f"/repos/{gh_slug()}/actions/runs?per_page=10")
            for r in data.get("workflow_runs", []):
                print(f"  {r['id']}  {r['status']:<10} {r.get('conclusion')}  {r['created_at']}")
        return

    die(f"未知动作 {action!r}")


def cmd_update(args) -> None:
    local = local_version()
    remote, err = _remote_version_via_gh()
    via = "gh"
    if not remote:
        remote, err2 = _remote_version_via_token()
        via = "token"
        if not remote:
            err = err or err2

    if args.json:
        print(json.dumps({"local": local, "remote": remote, "via": via, "error": err},
                         ensure_ascii=False, indent=2))
        return
    if not remote:
        print(f"本地版本 : {local}")
        print(f"无法检查   : {err}")
        print("该仓库是私有的，需要：")
        print("  - 已登录的 gh CLI（gh auth login），或")
        print("  - 环境变量 GITHUB_TOKEN（最小 scope: repo 或 contents:read）")
        return

    print(f"本地版本 : {local}")
    print(f"远端版本 : {remote}   （来源：{via}）")
    if _semver_tuple(remote) > _semver_tuple(local):
        print(f"有新版本 : {remote}（当前 {local}）")
        print(f"下载页   : https://github.com/{REPO_SLUG}/releases")
        print("（若仓库没有 Release，直接拉 main 分支的 dist 目录，或用自己的构建产物）")
    elif _semver_tuple(remote) == _semver_tuple(local):
        print("已是最新版本。")
    else:
        print(f"本地版本比远端新（{local} > {remote}），可能是未推送的开发版。")

def cmd_up(args) -> None:
    """把这条链路需要的组件按依赖顺序拉起来，并做一次健康检查。"""
    problems: list[str] = []

    print("[1/4] 检查前置条件")
    sites = load_sites()["sites"]
    need_ca = not (CA_DIR / "rootCA.crt").is_file()
    # 只有 loopback 模式（本机终结 TLS）才需要叶子证书。
    # dns / ech / 快速失败的站点不经过本机代理，给它们要证书是错的 ——
    # 这个误判会让每次启动都报"缺少证书"并让 up 以失败退出。
    need_leaf = any(
        s.get("dns_mode", "ech") == "loopback" and s.get("cert_san")
        and not (CERT_DIR / f"{s['name']}.crt").is_file()
        for s in sites
    )
    if need_ca or need_leaf:
        # 证书是每台机器各自签的，不能随包分发；缺失时自动补，免得用户
        # 点一下"启动"却只得到一句"请先跑 gen-cert"。
        print("  证书缺失，自动签发（本机自签）")
        try:
            if need_ca:
                cmd_gen_ca(argparse.Namespace(force=False))
            cmd_gen_cert(argparse.Namespace(site=None, days=825))
        except SystemExit:
            problems.append("证书签发失败，见上面的输出")

    for site in sites:
        if not (site.get("enabled", True) or site.get("dns_override")):
            continue
        # 同上：只有本机终结 TLS 的站点需要证书。
        if site.get("dns_mode", "ech") != "loopback" or not site.get("cert_san"):
            continue
        crt = CERT_DIR / f"{site['name']}.crt"
        if not crt.is_file():
            problems.append(f"缺少证书 {crt.name}（跑 gen-cert）")
    if not (CA_DIR / "rootCA.crt").is_file():
        problems.append("缺少根证书（跑 gen-ca）")
    if not hosts_has_doh_entry():
        problems.append("hosts 里没有 DoH 条目（跑 setup-browser-ech.ps1 -Elevated）")
    for p in problems:
        print(f"  [!] {p}")
    if not problems:
        print("  OK")

    print("[2/4] 启动 DoH")
    cmd_doh(argparse.Namespace(action="start"))

    print("[3/4] 启动 nginx")
    if pid_is_running(ROOT / "logs" / "nginx.pid"):
        print("  nginx 已在运行")
    else:
        try:
            cmd_service(argparse.Namespace(action="up", nginx=args.nginx))
        except SystemExit:
            problems.append("nginx 启动失败，看 logs/error.log")

    # 启动时后台跑一次上游健康检查：地址失效时自动修复，用户不必知道有这回事。
    # 不能阻塞启动（51 个地址要十几秒），所以 detached 进程 + 静默输出。
    try:
        refresh_py = ROOT / "tools" / "refresh-upstreams.py"
        if refresh_py.is_file():
            env = dict(os.environ)
            env["PYTHONIOENCODING"] = "utf-8"
            subprocess.Popen([sys.executable, str(refresh_py), "--quiet"],
                             cwd=str(ROOT), env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             creationflags=0x00000008)
    except Exception:
        pass

    # 中继是可选的：启用了才拉，失败只提示、不影响主链路
    relay_cfg = load_relay_cfg()
    if relay_cfg.get("enabled"):
        print("[+] 启动中继（可选）")
        try:
            cmd_relay(argparse.Namespace(action="start"))
        except SystemExit:
            print("  [!] 中继没起来，相关域名会快速 502（其余站点不受影响）")

    print("[4/4] 健康检查")
    # 后端在 5454 解析，浏览器看到的 5444 由 nginx 前置提供。两个都通才算就绪：
    # 只检查其中一个，就会出现"报 OK 但浏览器什么都解析不了"的情况。
    backend_pid = port_listener_pid(DOH_INTERNAL_PORT)
    front_ok = port_listener_pid(DOH_PORT) is not None
    nginx_ok = pid_is_running(ROOT / "logs" / "nginx.pid")
    print(f"  DoH 后端 :{DOH_INTERNAL_PORT}  {'OK' if backend_pid else 'FAIL'}")
    print(f"  DoH 前置 :{DOH_PORT}  {'OK' if front_ok else 'FAIL'}（需要 nginx）")
    print(f"  nginx :443     {'OK' if nginx_ok else 'FAIL'}")
    if not backend_pid:
        problems.append(f"DoH 后端（:{DOH_INTERNAL_PORT}）没起来，看 logs/doh.log")
    if not front_ok:
        problems.append(f"DoH 前置（:{DOH_PORT}）没起来，浏览器会无法解析任何域名")
    if not nginx_ok:
        problems.append("nginx 没起来，看 logs/error.log")

    print()
    if problems:
        print(f"有 {len(problems)} 个问题需要先解决，见上面 [!] 行。")
        raise SystemExit(1)
    print("链路就绪。浏览器里把「使用安全 DNS」填成：")
    print(f"  https://doh.sniroute.lan:{DOH_PORT}/dns-query")
    print("然后用 scripts\\start-chrome-ech.cmd 启动浏览器。")


def cmd_down(args) -> None:
    print("停止 nginx")
    try:
        cmd_service(argparse.Namespace(action="down", nginx=args.nginx))
    except SystemExit:
        print("  nginx 未运行或已在停止")
    print("停止 DoH")
    cmd_doh(argparse.Namespace(action="stop"))


def cmd_status(args) -> None:
    sites = load_sites()["sites"]
    pid = port_listener_pid(DOH_PORT)
    # nginx 的 pid 文件位置取决于谁启动的它：
    #   · 仓库里直接跑：<仓库>/logs/nginx.pid
    #   · 安装版由托盘/服务启动：%LOCALAPPDATA%\Sniroute\nginx 下的 pid
    # 只认仓库那份会误报"未运行"（实测 443 明明被占用却说没跑）。
    # 所以除了 pid 文件，再兜一层"443 有没有人监听"。
    nginx_ok = pid_is_running(ROOT / "logs" / "nginx.pid")
    if not nginx_ok:
        installed_pid = (Path(os.environ.get("LOCALAPPDATA", "")) / "Sniroute"
                         / "nginx" / "logs" / "nginx.pid")
        nginx_ok = pid_is_running(installed_pid)
    if not nginx_ok:
        nginx_ok = port_listener_pid(443) is not None

    print("组件")
    print(f"  DoH    :{DOH_PORT}  {'运行中 pid ' + str(pid) if pid else '未运行'}")
    print(f"  nginx  :443    {'运行中' if nginx_ok else '未运行'}")
    print(f"  hosts  DoH 条目 {'有' if hosts_has_doh_entry() else '无'}")
    _w = warp_state()
    print(f"  WARP   出口    {'已连接' if _w == 'connected' else ('未安装' if _w == 'missing' else '未连接')}")
    ca = (CA_DIR / "rootCA.crt").is_file()
    print(f"  根证书 文件 {'有' if ca else '无'}（是否已装入信任库需管理员视角）")

    print("\n站点")
    for site in sorted(sites, key=lambda s: s.get("name", "").lower()):
        mode = site.get("dns_mode", "ech")
        state = "启用" if site.get("enabled", True) else ("仅DNS" if site.get("dns_override") else "停用")
        print(f"  {site['name']:<8} [{state}] mode={mode:<8} 域名:{','.join(site['hosts'])}")

    if DOH_LOG.is_file():
        lines = DOH_LOG.read_text(encoding="utf-8", errors="replace").splitlines()
        print(f"\nDoH 最近查询（共 {len(lines)} 条）")
        for line in lines[-5:]:
            print(f"  {line}")


# --------------------------------------------------------------------------
# 自检
# --------------------------------------------------------------------------


def http_via_proxy(
host: str, path: str = "/", timeout: float = 20.0) -> dict:
    """经本机代理发一个真实 HTTPS 请求，返回状态行与字节数。

    这里刻意用本机根证书去校验：如果握手能过并且主机名对得上，就同时证明了两件事
    —— 代理确实在监听，且它为该域名出示的是我们自己签的那张证书（而不是把请求
    转发去别处）。证书没装进系统信任库也没关系，因为我们直接指向 ca/rootCA.crt。
    """
    hostname = host.split(":")[0]
    port = int(host.split(":")[1]) if ":" in host else 443
    ctx = ssl.create_default_context()
    ca = CA_DIR / "rootCA.crt"
    if ca.is_file():
        ctx.load_verify_locations(cafile=str(ca))
        ctx.check_hostname = True
        ctx.verify_mode = ssl.CERT_REQUIRED
    else:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    request = (f"GET {path} HTTP/1.1\r\n"
               f"Host: {hostname}\r\n"
               "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
               "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36\r\n"
               "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
               "Connection: close\r\n\r\n")

    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as raw:
            with ctx.wrap_socket(raw, server_hostname=hostname) as tls:
                tls.sendall(request.encode())
                buf = b""
                while len(buf) < 262144:
                    chunk = tls.recv(16384)
                    if not chunk:
                        break
                    buf += chunk
    except ssl.SSLCertVerificationError as exc:
        return {"ok": False, "error": f"本地证书不对：{exc.reason}"}
    except (socket.timeout, TimeoutError):
        return {"ok": False, "error": "本地代理超时（在跑吗？）"}
    except OSError as exc:
        return {"ok": False, "error": f"连不上本机代理：{exc.strerror or exc}"}

    head, _, body = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin-1", "replace").split("\r\n")
    status = lines[0] if lines else ""
    code = 0
    if len(status.split()) >= 2 and status.split()[1].isdigit():
        code = int(status.split()[1])
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    return {"ok": True, "code": code, "status": status, "size": len(body),
            "location": headers.get("location", ""), "server": headers.get("server", "")}


def verdict_of(result: dict) -> tuple:
    """把一个 HTTP 结果翻成人能读的结论。这几种情形对应完全不同的原因。"""
    if not result.get("ok"):
        return "失败", result.get("error", "")
    code = result["code"]
    if code in (200, 204):
        return "可用", f"{result['size']} 字节"
    if code in (301, 302, 307, 308):
        return "可跳转", f"→ {result.get('location', '?')}"
    if code == 403:
        return "被拒", "上游拒绝：源站要求 SNI 选中 vhost，而我们没发 SNI"
    if code == 421:
        return "错向", "来源集群与 Host 不匹配（该地址不用来服务这个域名）"
    if code >= 500:
        return "上游故障", f"HTTP {code}"
    return "异常", f"HTTP {code}"


def describe(site: dict, result: dict, timeout: float) -> tuple:
    """把一次请求的结果翻成结论；3xx 再跟一跳。

    只报"可跳转"是不够的：pixiv 主站就是 301 到 www.pixiv.net 然后 403，如果在这里
    停下，结论会写成"可用"——把用户送进一个打不开的页面。跳一次，才看得到终点。
    """
    label, detail = verdict_of(result)
    if result.get("ok") and 300 <= result["code"] < 400 and result.get("location"):
        target = urllib.parse.urlsplit(result["location"])
        if target.hostname:
            follow = http_via_proxy(target.hostname, target.path or "/", timeout)
            flabel, fdetail = verdict_of(follow)
            detail = f"→ {target.hostname}（{flabel}：{fdetail}）"
            if flabel not in ("可用",):
                label = "跳转后失败"
    return label, detail


def cmd_verify(args) -> None:
    pid_file = ROOT / "logs" / "nginx.pid"
    if not pid_is_running(pid_file):
        die("nginx 没在跑。先 `service up`，或用 service test 只校验配置。")

    sites = select_sites(load_sites(), args.site)
    # 显式指定 --site 时连停用的也验：那正是想确认"它现在通了没有"的场景。
    if not args.site:
        sites = [s for s in sites if s.get("enabled", True)]
    if not sites:
        die("没有可验证的站点。")

    rows = []
    for site in sites:
        host = site["hosts"][0] if site["hosts"] else site["probe"]["hostname"]
        result = http_via_proxy(host, args.path, args.timeout)
        label, detail = describe(site, result, args.timeout)
        rows.append((site["name"], host, label, detail))

    width = max(len(r[0]) for r in rows)
    for name, host, label, detail in rows:
        print(f"  {name:<{width}}  {host:<22} {label:<8} {detail}")

    bad = [r for r in rows if r[2] not in ("可用", "可跳转")]
    print()
    if bad:
        print(f"{len(bad)}/{len(rows)} 个站点当前不可用。若上游刚变过，跑一次 probe；"
              f"若是 '被拒'，说明该站点要求 SNI —— 这条路对它不适用，"
              f"把它在 data/sites.json 里设为 enabled: false。")
        raise SystemExit(1)
    print(f"{len(rows)}/{len(rows)} 个站点可用。")


def tcp_reachable(ip: str, port: int, timeout: float) -> bool:
    """TCP 能不能连上。这一条不过，后面所有 TLS 手段都无意义。"""
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def tls_handshake(ip: str, port: int, server_name, timeout: float) -> bool:
    """只关心握手能不能完成，不校验证书（我们测的是阻断，不是信任链）。

    server_name 为 None 即不发 SNI —— 这正是"本机终结后转发"要走的路径。
    """
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        raw = socket.create_connection((ip, port), timeout=timeout)
    except OSError:
        return False
    try:
        with ctx.wrap_socket(raw, server_hostname=server_name):
            return True
    except (ssl.SSLError, OSError):
        return False
    finally:
        try:
            raw.close()
        except Exception:
            pass


def find_ech_config(name: str, timeout: float) -> bool:
    """查 HTTPS/SVCB（type 65）里有没有 ech=。

    只有带 ech 的域名，浏览器原生 ECH 这条路才存在。
    """
    for _label, base in DOH_ENDPOINTS:
        url = f"{base}?name={urllib.parse.quote(name)}&type=HTTPS"
        req = urllib.request.Request(url, headers={"Accept": "application/dns-json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                doc = json.loads(resp.read().decode("utf-8", errors="replace"))
        except Exception:
            continue
        for answer in doc.get("Answer", []):
            if answer.get("type") == 65 and "ech=" in str(answer.get("data", "")):
                return True
    return False


def cmd_check_site(args) -> None:
    """判断一个域名能不能被本工具支持，并说明该用哪种模式。

    判定只看三条事实，顺序也不能换：
      1. TCP 443 能不能连上        —— 连不上就是 IP 级封锁，无解
      2. 带 SNI 握手是否成功        —— 成功说明本来就不需要本工具
      3. 不带 SNI 握手是否成功      —— 这决定 loopback 模式可不可行
    再补一条 type=65 里有没有 ech，决定能不能走浏览器原生 ECH。
    """
    domain = args.domain
    timeout = args.timeout
    print(f"检查 {domain}\n")

    endpoint, ips = doh_lookup(domain, timeout)
    ips = [ip for ip in ips if ":" not in ip]
    if not ips:
        die("解析失败：没有拿到任何 A 记录")
    print(f"  解析（经 {endpoint}）：{', '.join(ips[:4])}")

    candidates = ips[:4]
    reachable = [ip for ip in candidates if tcp_reachable(ip, 443, timeout)]
    print(f"  TCP 443：{len(reachable)}/{len(candidates)} 个地址可连")

    if not reachable:
        print("\n结论：不支持。")
        print("  这些地址在 TCP 层就连不上（IP/端口级封锁）。")
        print("  SNI/ECH 只能改变握手内容，无法建立一条被阻断的连接；")
        print("  这类站点需要中转（代理/VPN），不属于本工具解决的范围。")
        return

    ip = reachable[0]
    sni_ok = tls_handshake(ip, 443, domain, timeout)
    nosni_ok = tls_handshake(ip, 443, None, timeout)
    ech = find_ech_config(domain, timeout)

    print(f"  TLS 带 SNI：{'握手成功' if sni_ok else '被重置或失败'}")
    print(f"  TLS 不带 SNI：{'握手成功' if nosni_ok else '被重置或失败'}")
    print(f"  HTTPS 记录里的 ech：{'有' if ech else '无'}")

    print("\n结论：")
    if ech:
        print("  支持，建议 ech 模式：浏览器原生 ECH，本机不做 TLS 终结。")
        print(f"  需要 DNS 层把 {domain} 的 HTTPS 记录补上 ech（data/cloudflare-ech.json）。")
    elif nosni_ok:
        print("  支持，用 loopback 模式：DNS 指向本机，nginx 终结后无 SNI 转发。")
        print(f"  需要为该域名签发叶子证书（sites.json 的 cert_san 要包含它）。")
    elif sni_ok:
        print("  不需要本工具：带 SNI 就能握手，阻断不在这一层。")
    else:
        print("  不支持：带/不带 SNI 都被拒，且没有可用的 ECH 配置。")

def cmd_check(args) -> None:
    print(f"仓库根目录 : {ROOT}")
    print(f"管理员     : {'是' if is_admin() else '否（hosts / install-ca 需要）'}")

    # 这两个一律走 find_*，这样"包里自带的"与"系统里的"不会报出两套不同答案
    try:
        print(f"openssl    : {find_openssl()}")
    except SystemExit:
        print("openssl    : 未找到")
    try:
        print(f"nginx      : {find_nginx(args.nginx)}")
    except SystemExit:
        print("nginx      : 未配置（用 --nginx 或写 nginx.path）")
    print(f"根证书     : {'已生成' if (CA_DIR / 'rootCA.crt').is_file() else '未生成'}")

    discovered = load_candidates()
    for site in load_sites()["sites"]:
        crt = CERT_DIR / f"{site['name']}.crt"
        # dns / ech 模式的站点没有 upstream_file（它们不经过 nginx 转发上游），
        # 这里以前直接取 site["upstream_file"] 会 KeyError 崩溃。
        upstream_file = site.get("upstream_file")
        lst = (ROOT / upstream_file) if upstream_file else None
        count = sum(1 for line in lst.read_text(encoding="utf-8").splitlines()
                    if line.strip().startswith("server ")) if lst and lst.is_file() else 0
        state = "启用" if site.get("enabled", True) else "停用"
        # 同样是 dns / ech 模式的站点：没有 candidates 字段（它们不做上游转发）。
        cand = len(site.get("candidates") or [])
        hosts = site.get("hosts") or []
        print(f"  {site['name']:<8} [{state}] 证书:{'有' if crt.is_file() else '无':<2} "
              f"上游:{count} 条  候选:{cand}+"
              f"{len(discovered.get(site['name'], []))}(discover)  "
              f"域名:{','.join(hosts) if hosts else '—（不托管域名，仅占位）'}")


# --------------------------------------------------------------------------




# ---------------------------------------------------------------- WARP 账号与导出
#
# 官方 WARP 客户端不导出配置，wgcf 能导出但要额外装一个 Go 工具。
# 这里直接自己注册：Cloudflare 的注册接口就是一个 POST + 我们生成的 Curve25519 公钥。
# 密钥用 tools/x25519.py（纯 Python，RFC 7748 测试向量已通过）。
WARP_ACCOUNT = ROOT / "data" / "warp-account.json"


def _load_x25519():
    import importlib.util
    spec = importlib.util.spec_from_file_location("sniroute_x25519", ROOT / "tools" / "x25519.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def warp_register() -> dict | None:
    """注册一个 WARP 账号并返回账号信息；失败返回 None。

    传输层用 curl.exe：实测 Python 的 urllib 会被直接 RST（TLS 指纹差异），
    而 curl 与 Go 客户端都能过。curl 在 Win10 1803+ 是系统自带。
    """
    import base64
    import datetime
    import json
    import tempfile

    x = _load_x25519()
    priv, pub = x.generate_keypair()
    body = {
        "key": base64.b64encode(pub).decode(),
        "install_id": "",
        "fcm_token": "",
        "tos": datetime.datetime.now(datetime.timezone.utc)
                .isoformat(timespec="milliseconds").replace("+00:00", "Z"),
        "model": "PC",
        "type": "Windows",
        "locale": "en_US",
    }
    tmp = Path(tempfile.gettempdir())
    body_file = tmp / "sniroute-warp-reg.json"
    resp_file = tmp / "sniroute-warp-reg-resp.json"
    body_file.write_text(json.dumps(body), encoding="utf-8")

    for ver in ("v0a2158", "v0i1909051800"):
        url = f"https://api.cloudflareclient.com/{ver}/reg"
        try:
            proc = subprocess.run(
                ["curl.exe", "-s", "-m", "25", "--ssl-no-revoke",
                 "-o", str(resp_file), "-w", "%{http_code}",
                 "-X", "POST", "-H", "Content-Type: application/json",
                 "-H", "User-Agent: okhttp/3.12.1",
                 "--data-binary", f"@{body_file}", url],
                capture_output=True, text=True, timeout=40)
        except Exception:
            continue
        if (proc.stdout or "").strip() != "200":
            continue
        try:
            doc = json.loads(resp_file.read_text(encoding="utf-8"))
            peer = doc["config"]["peers"][0]
            iface = doc["config"]["interface"]
        except Exception:
            continue
        acct = {
            "private_key": base64.b64encode(priv).decode(),
            "peer_public_key": peer["public_key"],
            "client_public_key": base64.b64encode(pub).decode(),
            "addresses": iface.get("addresses", {}),
            "client_id": doc.get("client_id", ""),
            "registered_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        }
        WARP_ACCOUNT.write_text(json.dumps(acct, indent=2), encoding="utf-8")
        return acct
    return None


def _warp_yaml(acct: dict, endpoints: list[tuple[str, int]]) -> str:
    addrs = acct.get("addresses") or {}
    v4 = addrs.get("v4", "172.16.0.2")
    v6 = addrs.get("v6", "")
    blocks = []
    for i, (host, port) in enumerate(endpoints, 1):
        name = "WARP" if i == 1 else f"WARP-{i}"
        block = [
            f"  - name: {name}",
            "    type: wireguard",
            f"    server: {host}",
            f"    port: {port}",
            f"    ip: {v4}",
        ]
        if v6:
            block.append(f"    ipv6: {v6}")
        block += [
            f'    private-key: "{acct["private_key"]}"',
            f'    public-key: "{acct["peer_public_key"]}"',
            "    udp: true",
            "    mtu: 1280",
            "    remote-dns-resolve: true",
            "    dns: [1.1.1.1, 1.0.0.1]",
        ]
        blocks.append("\n".join(block))
    names = ", ".join(("WARP" if i == 1 else f"WARP-{i}") for i in range(1, len(endpoints) + 1))
    joined = "\n".join(blocks)
    return f"""# Cloudflare WARP for Clash Meta —— 由 sniroute.cmd warp export 生成
# 账号是本机注册的真实 WARP 账号；文件含私钥，别外传，别提交到仓库。
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
  proxy-server-nameserver:
    - 223.5.5.5

proxies:
{joined}

proxy-groups:
  - name: PROXY
    type: fallback
    proxies: [{names}]
    url: https://www.gstatic.com/generate_204
    interval: 300

rules:
  - MATCH,PROXY
"""


def cmd_warp_export(args) -> None:
    import json

    acct = None
    if WARP_ACCOUNT.is_file() and not args.renew:
        try:
            acct = json.loads(WARP_ACCOUNT.read_text(encoding="utf-8"))
            print(f"复用本机 WARP 账号：{WARP_ACCOUNT}")
        except Exception:
            acct = None
    if acct is None:
        print("向 Cloudflare 注册新的 WARP 账号…")
        acct = warp_register()
        if acct is None:
            die("注册失败：无法访问 api.cloudflareclient.com（可能被网络拦截）。\n"
                "      可改用 wgcf：wgcf register --accept-tos && wgcf generate\n"
                "      再用 tools/wgcf-to-clash.py 转换。")
        print(f"  已保存账号：{WARP_ACCOUNT}")

    endpoints = []
    for raw in (args.endpoint or []):
        host, _, port = raw.partition(":")
        try:
            endpoints.append((host, int(port or 2408)))
        except ValueError:
            die(f"端点格式应为 IP:PORT，收到 {raw!r}")
    if not endpoints:
        endpoints = [("162.159.192.1", 2408), ("162.159.193.10", 2408)]

    out = Path(args.output or (ROOT / "dist" / "warp-clash.yaml"))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(_warp_yaml(acct, endpoints), encoding="utf-8")
    print(f"已生成 {out}")
    print("  导入方式：Clash 客户端 -> 配置 -> 从文件导入")
    print("  注意：文件含 WARP 私钥，等同账号凭据，不要外传")

# ---------------------------------------------------------------- WARP
# Cloudflare WARP 客户端（warp-cli）不装在我们目录里，位置是固定的 Program Files。
# 它默认走 MASQUE(HTTP/3)，国内经常握手成功但流量不通；实测换成 WireGuard 协议
# （UDP 2408）反而能通，所以 `warp on` 会顺手把协议切过去。
WARP_CLI_CANDIDATES = [
    Path(r"C:\Program Files\Cloudflare\Cloudflare WARP\warp-cli.exe"),
    Path(r"C:\Program Files (x86)\Cloudflare\Cloudflare WARP\warp-cli.exe"),
]


def find_warp_cli() -> Path | None:
    for p in WARP_CLI_CANDIDATES:
        if p.is_file():
            return p
    return None


def warp_run(*args: str, timeout: int = 20) -> tuple[int, str]:
    """跑一条 warp-cli 命令，返回 (退出码, 输出)。找不到客户端返回 (-1, "")。"""
    cli = find_warp_cli()
    if cli is None:
        return -1, ""
    proc = subprocess.run([str(cli), "--accept-tos", *args],
                          capture_output=True, text=True, timeout=timeout,
                          encoding="utf-8", errors="replace")
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def warp_state() -> str:
    """返回 connected / disconnected / missing。"""
    if find_warp_cli() is None:
        return "missing"
    try:
        _, out = warp_run("status", timeout=15)
    except Exception:
        return "disconnected"
    low = out.lower()
    if "connected" in low and "disconnected" not in low:
        return "connected"
    return "disconnected"



# WARP 官方安装包的固定版本直链。没有 "latest" 端点（实测 404），
# 所以这里钉一个已知版本；它过期了也仍然能装，只是不是最新。
WARP_PINNED_URL = ("https://downloads.cloudflareclient.com/v1/download/windows/"
                   "version/2026.8.2115.0")


def winget_available() -> bool:
    return shutil.which("winget") is not None


def warp_install(use_winget: bool = True) -> bool:
    """装 WARP。返回 True 表示装好了（或本来就有）。

    三级回退，因为不是每台 Windows 都有 winget（LTSC、精简版、没装 App Installer）：
      1. winget（能拿最新版）
      2. 官方直链下载固定版本，再交给系统安装（仍会弹 UAC）
      3. 打印官方页面，让用户手动装
    """
    if warp_state() != "missing":
        print("WARP 客户端已安装，无需重复安装。")
        return True

    if use_winget and winget_available():
        print("[1/3] 用 winget 安装（会弹一次管理员确认）…")
        try:
            proc = subprocess.run(
                ["winget", "install", "--id", "Cloudflare.Warp", "-e",
                 "--accept-package-agreements", "--accept-source-agreements"],
                capture_output=True, text=True, timeout=900,
                encoding="utf-8", errors="replace")
            if find_warp_cli() is not None:
                print("  已安装。")
                return True
            print(f"  winget 退出码 {proc.returncode}，尝试下一条路。")
        except Exception as e:
            print(f"  winget 失败：{e}，尝试下一条路。")
    elif use_winget:
        print("[1/3] 这台机器没有 winget（精简版/LTSC 常见），跳过。")

    # 2) 官方直链
    print("[2/3] 从 Cloudflare 官方地址下载安装包…")
    dest = Path(tempfile.gettempdir()) / "cloudflare-warp-setup.msi"
    try:
        import urllib.request
        print(f"  {WARP_PINNED_URL}")
        # Cloudflare 的下载端点会拒绝 Python-urllib 的默认 UA（实测 403），
        # 必须带一个普通浏览器 UA。
        req = urllib.request.Request(WARP_PINNED_URL, headers={
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                          "AppleWebKit/537.36 (KHTML, like Gecko) "
                          "Chrome/124.0 Safari/537.36",
        })
        with urllib.request.urlopen(req, timeout=120) as resp, open(dest, "wb") as fh:
            shutil.copyfileobj(resp, fh)
        size = dest.stat().st_size
        print(f"  已下载 {size / 1048576:.1f} MB -> {dest}")
        if size < 1024 * 1024:
            print("  文件太小，可能不是安装包；放弃。")
            return False
        print("  安装（会弹管理员确认）…")
        subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"Start-Process msiexec.exe -ArgumentList '/i \"{dest}\" /qb' -Verb RunAs -Wait"],
            timeout=900)
        if find_warp_cli() is not None:
            print("  已安装。")
            return True
        print("  安装未完成（可能取消了 UAC）。")
    except Exception as e:
        print(f"  下载/安装失败：{e}")

    # 3) 手动
    print("[3/3] 请手动安装：")
    print("  浏览器打开 https://cloudflarewarp.com/ ，下载 Windows 版安装。")
    print("  装好后运行 sniroute.cmd warp status 确认。")
    return False

def cmd_warp(args) -> None:
    action = args.action

    # export 不依赖本地客户端（自己注册账号 + 生成 YAML），放在最前面。
    if action == "export":
        cmd_warp_export(args)
        return

    # install 必须在"客户端未安装就早退"之前处理 —— 它的目的正是装客户端。
    if action == "install":
        ok = warp_install(use_winget=not args.no_winget)
        print("结果：成功" if ok else "结果：未成功")
        return

    cli = find_warp_cli()

    if cli is None:
        print("WARP 客户端未安装。")
        print("  安装命令（会弹一次管理员确认）：")
        print("    winget install --id Cloudflare.Warp -e")
        print("  装好后重新运行 sniroute.cmd warp on")
        return

    print(f"WARP 客户端: {cli}")

    if action == "status":
        state = warp_state()
        print(f"连接状态  : {'已连接' if state == 'connected' else '未连接'}")
        try:
            _, proto = warp_run("tunnel", "stats", timeout=15)
            for line in proto.splitlines():
                line = line.strip()
                if any(k in line for k in ("Tunnel Protocol", "Endpoints", "Sent:", "latency", "loss")):
                    print(f"  {line}")
        except Exception:
            pass
        return

    if action == "on":
        # 先切协议：MASQUE 在这边常常"连上但不通"。
        try:
            code, out = warp_run("tunnel", "protocol", "set", "WireGuard", timeout=20)
            if code == 0:
                print("协议已切换为 WireGuard（MASQUE 在国内常连上但流量不通）")
            else:
                print(f"协议切换返回 {code}: {out.strip()[:80]}")
        except Exception as e:
            print(f"协议切换失败（继续尝试连接）: {e}")
        code, out = warp_run("connect", timeout=30)
        if code == 0:
            print("已发起连接。用 sniroute.cmd warp status 看结果。")
            print("提示：连接成功后，Sniroute 的 nginx 会自动经 WARP 出墙，")
            print("      Google / YouTube / Play 网页版都可用，不必停掉本工具。")
        else:
            print(f"连接失败（退出码 {code}）: {out.strip()[:120]}")
        return

    if action == "off":
        code, out = warp_run("disconnect", timeout=20)
        print("已断开 WARP。" if code == 0 else f"断开失败: {out.strip()[:100]}")
        return

    if action == "endpoint":
        if not args.endpoint:
            print("用法: sniroute.cmd warp endpoint 162.159.192.2:2408")
            return
        code, out = warp_run("tunnel", "endpoint", args.endpoint, timeout=20)
        print(f"端点已设为 {args.endpoint}" if code == 0 else f"设置失败: {out.strip()[:100]}")
        return

def main() -> None:
    # 输出被重定向（管道、日志、CI）时用 UTF-8 写，否则中文会按 ANSI 代码页变成乱码。
    # 连着真实控制台时不动它 —— 那边 Windows 自己有 Unicode 通道。
    for stream in (sys.stdout, sys.stderr):
        if not stream.isatty() and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(prog="sniroute", description="sniroute")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("up", help="一键拉起整条链路（DoH + nginx）并做健康检查")
    p.add_argument("--nginx")
    p.set_defaults(func=cmd_up)

    p = sub.add_parser("down", help="停止整条链路")
    p.add_argument("--nginx")
    p.set_defaults(func=cmd_down)

    p = sub.add_parser("status", help="看组件与站点的当前状态")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("stats", help="从访问日志统计真实表现：按域名 / 按响应大小")
    p.set_defaults(func=cmd_stats)

    p = sub.add_parser("doh", help="单独控制 DoH 服务")
    p.add_argument("action", choices=["start", "stop", "restart", "status"])
    p.set_defaults(func=cmd_doh)

    p = sub.add_parser("refresh", help="上游候选地址健康检查与刷新")
    p.add_argument("--dry-run", dest="dry_run", action="store_true", help="只报告不写入")
    p.add_argument("--quiet", action="store_true", help="只在有变化时输出")
    p.set_defaults(func=cmd_refresh)

    p = sub.add_parser("nat", help="检测 NAT 类型（RFC 5780：映射行为 × 过滤行为）")
    p.add_argument("--server", default="", help="STUN 服务器，默认 stun.miwifi.com:3478")
    p.add_argument("--server2", default="", help="判定映射行为用的第二个 STUN 服务器")
    p.add_argument("--timeout-ms", dest="timeout_ms", type=int, default=0, help="单次探测超时（毫秒）")
    p.add_argument("--json", action="store_true")
    p.add_argument("--summary", action="store_true")
    p.set_defaults(func=cmd_nat)

    p = sub.add_parser("relay", help="中继：YouTube 视频 / Twitch VOD / Discord 更新等无直连出路的域名")
    p.add_argument("action", choices=["start", "stop", "status"])
    p.set_defaults(func=cmd_relay)

    p = sub.add_parser("warp", help="Cloudflare WARP 开关（给 Sniroute 加一条墙外出口）")
    p.add_argument("action", choices=["status", "on", "off", "endpoint", "install", "export"])
    p.add_argument("endpoint", nargs="?", default="", help="action=endpoint 时的 IP:端口")
    p.add_argument("--no-winget", action="store_true",
                   help="install 时跳过 winget，直接用 Cloudflare 官方安装包")
    p.add_argument("-o", "--output", default="", help="export 时输出路径")
    p.add_argument("--renew", action="store_true", help="export 时重新注册 WARP 账号")
    p.set_defaults(func=cmd_warp)

    p = sub.add_parser("cloud", help="云通道：用各自的账号额度取文件（tor / warp / gha / 自建 URL）")
    p.add_argument("action", choices=["list", "add", "remove", "fetch", "test"])
    p.add_argument("url", nargs="?", default="", help="fetch 的目标 URL")
    p.add_argument("--name", default="", help="add/remove 的通道名")
    p.add_argument("--template", default="", help="add 时的 URL 模板（含 {url}）")
    p.add_argument("--token", default="", help="add 时的共享密钥")
    p.add_argument("--provider", default="", help="fetch 用哪个通道")
    p.add_argument("-o", "--output", default="", help="保存路径")
    p.set_defaults(func=cmd_cloud)

    p = sub.add_parser("gha", help="用 GitHub Actions 的 runner 下载文件（墙外取文件通道）")
    p.add_argument("action", choices=["status", "fetch", "auth", "runs", "login", "config", "fork"])
    p.add_argument("url", nargs="?", default="", help="fetch 时必填")
    p.add_argument("-o", "--output", default="", help="保存到哪个文件")
    p.add_argument("--name", default="", help="目标文件名（留空由 URL 决定）")
    p.add_argument("--token", default="", help="auth 时保存的 PAT")
    p.add_argument("--client-id", default="", help="login 时的 OAuth App Client ID")
    p.add_argument("--repo", default="", help="config 时的 owner/name")
    p.add_argument("--yes", action="store_true", help="fork 时跳过确认")
    p.set_defaults(func=cmd_gha)

    p = sub.add_parser("update", help="检查是否有新版本")
    p.add_argument("action", choices=["check"], nargs="?", default="check")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_update)

    p = sub.add_parser("site", help="判断一个域名能不能被本工具支持，并给出该用的模式")
    p.add_argument("domain")
    p.add_argument("--timeout", type=float, default=6.0)
    p.set_defaults(func=cmd_check_site)
    p = sub.add_parser("check", help="环境自检")
    p.add_argument("--nginx")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("hash", help="校验文件的 SHA-256 / SHA-1 / MD5 并与 sidecar 比对")
    p.add_argument("file", help="要校验的文件")
    p.set_defaults(func=cmd_hash)

    p = sub.add_parser("verify", help="经代理发真实请求，判断每个站点到底能不能用")
    p.add_argument("--site")
    p.add_argument("--path", default="/")
    p.add_argument("--timeout", type=float, default=20.0)
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("discover", help="找候选 IP：DoH 查真实记录，或扩展已验证地址的同网段")
    p.add_argument("--site")
    p.add_argument("--method", choices=["doh", "expand", "both"], default="both")
    p.add_argument("--mask", type=int, default=24, help="expand 时扩展的网段前缀长度，默认 /24")
    p.add_argument("--timeout", type=float, default=5.0)
    p.set_defaults(func=cmd_discover)

    p = sub.add_parser("probe", help="实测上游 IP 并写出 upstreams/*.list")
    p.add_argument("--site")
    p.add_argument("--timeout", type=float, default=5.0)
    p.add_argument("--mode", choices=["auto", "nosni", "sni"], default="auto",
                   help="auto=先按代理方式(不发SNI)再退到发SNI；nosni=只不发；sni=只发")
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("gen-ca", help="生成根证书")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_gen_ca)

    p = sub.add_parser("gen-cert", help="签发叶子证书")
    p.add_argument("--site")
    p.add_argument("--days", type=int, default=825)
    p.set_defaults(func=cmd_gen_cert)

    p = sub.add_parser("install-ca", help="把根证书装进本机受信任根（需管理员）")
    p.set_defaults(func=cmd_install_ca)

    p = sub.add_parser("hosts", help="改写 hosts 中的 sniroute 区块（需管理员）")
    p.add_argument("action", choices=["install", "remove", "show"])
    p.add_argument("--site")
    p.add_argument("--include-disabled", action="store_true",
                   help="连 enabled=false 的站点也一起拦截（不推荐，那个站点打不通）")
    p.set_defaults(func=cmd_hosts)

    p = sub.add_parser("service", help="启停 nginx")
    p.add_argument("action", choices=["up", "down", "reload", "status", "test"])
    p.add_argument("--nginx")
    p.set_defaults(func=cmd_service)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
