#!/usr/bin/env python3
"""status-report.py -- 逐个站点实测连通性，输出 markdown 表格

两层判定，缺一不可：

1) 网络层：直接对本机服务/上游做一次 TLS 请求，看 TCP、证书、HTTP 状态码。
   dns 模式 -> 直连真实地址（带真 SNI）
   loopback -> 连 127.0.0.1（本机 nginx 终结，上游由配置决定）
   ech  -> 原生 TLS 测不了（ECH 只有浏览器会做），标注"需浏览器"

2) 浏览器层：用普通 UA 跑一次无头 Chrome，取标题与 net::ERR。
   为什么必须带真实浏览器 UA：Cloudflare 会把 HeadlessChrome 判成机器人回 403，
   那是反爬不是被墙，只看状态码会把好站点误判成故障。

判据：
   浏览器拿到标题且无 net::ERR -> 可用
   浏览器停在 CF 挑战页（标题含 Just a moment / 请稍候）-> 可用但有挑战
   有 net::ERR -> 需要看网络层区分"网络不通"还是"服务端拒绝"
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
TRAY = os.path.join(ROOT, "data", "sites-tray.txt")
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36")

# 额外抽测：这些子资源决定页面"能不能真正用起来"，只有主文档 200 不算数
EXTRAS = [
    ("steam 图片", "cdn.akamai.steamstatic.com", "/steam/apps/730/header.jpg", "dns"),
    ("twimg 图标", "abs.twimg.com", "/favicons/twitter.ico", "loopback"),
    ("cdnjs JS", "cdnjs.cloudflare.com", "/ajax/libs/jquery/3.7.1/jquery.min.js", "dns"),
    ("modrinth API", "api.modrinth.com", "/v2/project/sodium", "dns"),
]


def load_menu_sites():
    """菜单里展示的站点 = 有 service 与 url 的那些（与托盘菜单同源）。"""
    cfg = json.load(open(SITES, encoding="utf-8"))
    out = []
    for s in cfg["sites"]:
        if s.get("service") and s.get("url") and s.get("enabled", True):
            out.append(s)
    return out


def native_probe(host, path="/", mode="dns", timeout=12):
    """网络层：返回 (状态, 说明)。ECH 站点原生测不了，直接标注。"""
    if mode == "ech":
        return "ech", "需浏览器（原生 TLS 不会做 ECH）"

    # loopback 站点：请求本机 nginx；dns 站点：直接连真实地址
    if mode == "loopback":
        target_ip = "127.0.0.1"
    else:
        target_ip = None

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    t0 = time.time()
    try:
        if target_ip:
            sock = socket.create_connection((target_ip, 443), timeout=timeout)
        else:
            sock = socket.create_connection((host, 443), timeout=timeout)
        with ctx.wrap_socket(sock, server_hostname=host) as ss:
            req = (f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: {UA}\r\n"
                   f"Accept: */*\r\nConnection: close\r\n\r\n")
            ss.sendall(req.encode())
            data = b""
            while len(data) < 2000:
                chunk = ss.recv(4096)
                if not chunk:
                    break
                data += chunk
        line = data.split(b"\r\n")[0].decode("latin-1", "replace")
        m = re.search(r"HTTP/[\d.]+ (\d{3})", line)
        code = m.group(1) if m else "?"
        # 顺带把证书主体取出来，用于确认"解析对了且真连到了目标"
        cert = ss.getpeercert()
        subject = ""
        if cert:
            for rdn in cert.get("subject", ()):
                for k, v in rdn:
                    if k == "commonName":
                        subject = v
        return code, f"{int((time.time()-t0)*1000)}ms{(' cert=' + subject) if subject else ''}"
    except Exception as exc:
        return "ERR", f"{type(exc).__name__}: {str(exc)[:60]}"


def browser_probe(url, timeout=60):
    """浏览器层：普通 UA 跑一次无头 Chrome，取标题、长度、net::ERR。"""
    prof = os.path.join(os.environ.get("TEMP", "."), "status-prof")
    args = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run",
            "--no-default-browser-check", f"--user-data-dir={prof}",
            f"--user-agent={UA}", "--virtual-time-budget=20000", "--dump-dom", url]
    t0 = time.time()
    try:
        p = subprocess.run(args, capture_output=True, timeout=timeout,
                           text=True, encoding="utf-8", errors="replace")
        dom = p.stdout or ""
    except subprocess.TimeoutExpired:
        return "超时", "", int((time.time() - t0) * 1000)
    except Exception as exc:
        return f"启动失败 {type(exc).__name__}", "", 0

    m = re.search(r"<title[^>]*>(.*?)</title>", dom, re.S | re.I)
    title = re.sub(r"\s+", " ", m.group(1)).strip() if m else ""
    err = ""
    for line in dom.splitlines():
        if "net::ERR" in line:
            err = re.search(r"net::ERR[A-Z_0-9]+", line).group(0)
            break
    return (err or title), err, int((time.time() - t0) * 1000)


def summarize(title_or_err, err):
    t = (title_or_err or "").lower()
    if err:
        return "不可用", err
    if not title_or_err:
        return "无标题", ""
    if "just a moment" in t or "请稍候" in t or "attention required" in t:
        return "挑战页", "Cloudflare 人机验证（真实浏览器会自动通过）"
    return "可用", ""


def main():
    sites = load_menu_sites()
    rows = []
    print(f"共 {len(sites)} 个菜单站点，开始实测（每个都要跑一次浏览器，约 5-10 分钟）", flush=True)

    for i, s in enumerate(sites, 1):
        name = s["service"]
        url = s["url"]
        host = (s.get("probe") or {}).get("hostname") or url.split("://")[-1].strip("/")
        mode = s.get("dns_mode", "ech")
        print(f"  [{i}/{len(sites)}] {name} ({mode}) ...", flush=True)

        code, note = native_probe(host, mode=mode)
        title_or_err, err, ms = browser_probe(url)
        verdict, detail = summarize(title_or_err, err)
        rows.append({
            "name": name, "mode": mode, "url": url, "host": host,
            "http": code, "native_note": note,
            "browser": title_or_err[:60], "title": title_or_err[:60],
            "err": err, "ms": ms, "verdict": verdict, "detail": detail,
        })
        print(f"        -> {verdict} | {code} | {title_or_err[:40]}", flush=True)

    # 子资源抽测
    extra_rows = []
    for label, host, path, mode in EXTRAS:
        code, note = native_probe(host, path=path, mode=mode)
        extra_rows.append({"label": label, "host": host, "code": code, "note": note})
        print(f"  额外 {label}: {code} {note}", flush=True)

    out = os.path.join(ROOT, "temp", "status-report.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"rows": rows, "extras": extra_rows}, f, ensure_ascii=False, indent=2)

    print("\n=== 汇总 ===")
    ok = sum(1 for r in rows if r["verdict"] == "可用")
    chal = sum(1 for r in rows if r["verdict"] == "挑战页")
    bad = sum(1 for r in rows if r["verdict"] == "不可用")
    print(f"  可用 {ok} / 挑战页 {chal} / 不可用 {bad} / 共 {len(rows)}")
    print(f"  明细已写入 {out}")


if __name__ == "__main__":
    main()
