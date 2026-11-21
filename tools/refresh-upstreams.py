#!/usr/bin/env python3
"""refresh-upstreams.py -- 候选地址健康检查与刷新

为什么需要它
------------
loopback 模式的上游地址是实测后硬编码的（写在 conf/upstreams/*.list）。这些地址
会随 CDN 调度变化而失效：某个 IP 挂了，nginx 只能靠 max_fails 挨个试，用户在等的
就是那几秒。这里把"谁来发现地址失效"从人手动重测，改成每次启动自动做一遍。

判定用的是真握手，不看 HTTP 状态码
----------------------------------
上游站点对自己不认识的路径返回 403/404 是常态（Cloudflare/Fastly 都这样），拿
状态码当健康标准会把好地址误判成坏的。所以这里只要求"TLS 握手成功且拿到任意
HTTP 响应"，并且同一地址连测 3 次、至少 2 次成功才算健康。

两种上游 SNI
------------
* nosni  : 故意不发 SNI（上游站点靠 Host 路由），握手成功即可
* sni    : 发真实 SNI；X / Grok 要用带根点的写法（$host.），twimg 要用 Fastly 的
           通用映射名，都由 sites.json 的 probe_sni 字段声明

失败时不破坏现有配置
--------------------
一个地址都没通过时保持原文件不动（宁可留着旧地址让 nginx 慢慢试，也不要一键清空
把站点打死）。这个教训来自 dns 模式那次"探活失败就当域名不存在"。
"""

import json
import os
import re
import socket
import ssl
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITES = os.path.join(ROOT, "data", "sites.json")
NGINX = os.path.join(ROOT, "nginx", "nginx.exe")
ATTEMPTS = 3
NEED_OK = 2
TIMEOUT = 15.0


def resolve_sni(site, host):
    """决定探活时发什么 SNI。返回 None 表示故意不发。

    以站点自己的 nginx 配置为准，不要靠 sites.json 里的字段去猜 —— 早期版本就是
    因为猜错，把"配置里根本不发 SNI 的站点"按发 SNI 去探，结果全部误判为不可用。
        proxy_ssl_server_name off          -> 不发 SNI
        proxy_ssl_name $host.              -> 发带根点的域名
        proxy_ssl_name <固定名字>          -> 发那个名字（例如 Fastly 的 map 名）
    """
    rule = site.get("probe_sni")
    if rule == "$host.":
        return host + "."
    if rule:
        return rule

    conf = os.path.join(ROOT, "conf", "sites", site["name"] + ".conf")
    if os.path.isfile(conf):
        try:
            text = open(conf, encoding="utf-8").read()
        except OSError:
            text = ""
        for line in text.splitlines():
            line = line.strip()
            if line.startswith("proxy_ssl_server_name off"):
                return None
            m = re.match(r"proxy_ssl_name\s+(\S+?);", line)
            if m:
                name = m.group(1)
                if name == "$host.":
                    return host + "."
                if name in ("$host", "$http_host"):
                    return host
                return name
    if site.get("upstream_mode") == "nosni":
        return None
    return host


def probe(ip, sni, host, port=443):
    """一次探测：TLS 握手 + 一个 GET。只看"拿到了 HTTP 响应没有"。"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        sock = socket.create_connection((ip, port), timeout=TIMEOUT)
    except OSError:
        return False, "tcp"
    try:
        with ctx.wrap_socket(sock, server_hostname=sni) as ss:
            req = (f"GET / HTTP/1.1\r\nHost: {host}\r\n"
                   "User-Agent: Mozilla/5.0\r\nAccept: */*\r\nConnection: close\r\n\r\n")
            ss.sendall(req.encode())
            data = ss.recv(64)
        if data.startswith(b"HTTP/"):
            return True, "ok"
        return False, "no-http"
    except ssl.SSLError:
        return False, "tls"
    except OSError:
        return False, "tcp"
    except Exception:
        return False, "err"
    finally:
        try:
            sock.close()
        except Exception:
            pass


def rewrite_upstream_file(path, healthy_ips):
    """只替换文件里的 server 行，保留注释与说明（那些是给人看的原因记录）。"""
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()

    head = [ln for ln in lines if not re.match(r"\s*server\s+[^\s]+:443", ln)]
    body = [f"server {ip}:443  max_fails=2 fail_timeout=15s;" for ip in healthy_ips]

    # 注释块与 server 行之间补一个空行，保持原来的排版
    while head and not head[-1].strip():
        head.pop()
    text = "\n".join(head + ["", *body]) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def nginx_test_and_reload():
    try:
        test = subprocess.run([NGINX, "-p", ROOT + os.sep, "-c", "conf/nginx.conf", "-t"],
                              capture_output=True, timeout=30)
        if test.returncode != 0:
            return False, "nginx -t 失败"
        subprocess.run([NGINX, "-p", ROOT + os.sep, "-c", "conf/nginx.conf", "-s", "reload"],
                       capture_output=True, timeout=30)
        return True, "reloaded"
    except Exception as exc:
        return False, f"reload 异常 {type(exc).__name__}"


def main():
    dry = "--dry-run" in sys.argv
    quiet = "--quiet" in sys.argv

    with open(SITES, encoding="utf-8") as f:
        cfg = json.load(f)

    total_checked = total_kept = total_dropped = 0
    changed_files = []

    for site in cfg["sites"]:
        if site.get("dns_mode") != "loopback":
            continue
        candidates = site.get("candidates") or []
        upstream_file = site.get("upstream_file")
        if not candidates or not upstream_file:
            continue
        path = os.path.join(ROOT, upstream_file.replace("/", os.sep))
        if not os.path.isfile(path):
            continue

        host = (site.get("probe") or {}).get("hostname") or (site.get("hosts") or [""])[0]
        if not host:
            continue
        sni = resolve_sni(site, host)

        healthy, suspect, dropped = [], [], []
        for ip in candidates:
            oks = 0
            tcp_ok = False
            reasons = []
            for attempt in range(ATTEMPTS):
                ok, why = probe(ip, sni, host)
                if why != "tcp":
                    tcp_ok = True          # TCP 连上过，说明对端是活的
                oks += 1 if ok else 0
                if not ok:
                    reasons.append(why)
                if oks >= NEED_OK:
                    break
                time.sleep(0.25)           # 连测太快会被 Akamai/Fastly 临时限速
            total_checked += 1
            if oks >= NEED_OK:
                healthy.append(ip)
            elif tcp_ok:
                # TCP 通但 TLS 失败：可能是限速、也可能是这台边缘对我们不通。
                # 不直接删 —— 删错会把整站打挂；标记为"降级保留"，排在健康地址后面。
                suspect.append((ip, ",".join(sorted(set(reasons)))))
            else:
                dropped.append((ip, ",".join(sorted(set(reasons))) or "tcp"))
                total_dropped += 1

        if not healthy and not suspect:
            if not quiet:
                print(f"  {site['name']:16} 全部不可用，保持原文件不动")
            continue

        if not quiet:
            for ip, why in suspect:
                print(f"  {site['name']:16} {ip} TCP 通但 TLS 失败（{why}），降级保留")

        total_kept += len(healthy) + len(suspect)
        # 顺序：健康地址在前，降级保留的在后（nginx 会优先命中前面的）
        keep = set(healthy) | {ip for ip, _ in suspect}
        healthy_ordered = [ip for ip in candidates if ip in healthy]
        healthy_ordered += [ip for ip in candidates if ip in {x for x, _ in suspect} and ip not in healthy]
        if healthy_ordered != candidates:
            changed_files.append(site["name"])
            if not dry:
                rewrite_upstream_file(path, healthy_ordered)
        if not quiet:
            detail = "，".join(f"{ip}({why})" for ip, why in dropped[:6])
            print(f"  {site['name']:16} 健康 {len(healthy)}/{len(candidates)}"
                  + (f"  剔除: {detail}" if dropped else ""))

    summary = f"检查 {total_checked} 个地址，保留 {total_kept}，剔除 {total_dropped}"
    if changed_files:
        if dry:
            summary += f"；将更新 {len(changed_files)} 个清单（dry-run 未写入）"
        else:
            ok, note = nginx_test_and_reload()
            summary += f"；更新 {len(changed_files)} 个清单（{note}）" if ok else \
                       f"；清单已改但 {note}，请手工检查"
    if not quiet:
        print("  " + summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
