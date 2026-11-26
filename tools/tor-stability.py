"""Tor 稳定性采样：定时检查经 Tor 的出口与延迟，给出成功率与统计。

用途：解决"Tor 到底能不能日常用"这个靠感觉的问题。每 N 秒经 SOCKS 探一次
check.torproject.org，记录延迟、出口 IP、失败原因，最后输出汇总。

用法：
    python tor-stability.py --duration 180 --interval 10
    python tor-stability.py --duration 60 --interval 5 --json out.json
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

CHECK_URL = "https://check.torproject.org/api/ip"


def curl_via_socks(url: str, socks: str, timeout: float) -> tuple[int, str, float]:
    """返回 (http_code, body, 耗时秒)。"""
    t0 = time.time()
    proc = subprocess.run(
        ["curl.exe", "-s", "--ssl-no-revoke", "-m", str(int(timeout)),
         "--socks5-hostname", socks, "-w", "\n%{http_code}", url],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        timeout=timeout + 5,
    )
    out = proc.stdout or ""
    lines = out.rstrip("\n").split("\n")
    code = 0
    try:
        code = int(lines[-1].strip()) if lines else 0
    except ValueError:
        code = 0
    body = "\n".join(lines[:-1]) if len(lines) > 1 else ""
    return code, body, round(time.time() - t0, 2)


def sample(socks: str, timeout: float) -> dict:
    code, body, elapsed = curl_via_socks(CHECK_URL, socks, timeout)
    is_tor = False
    ip = ""
    if code == 200:
        try:
            doc = json.loads(body)
            is_tor = bool(doc.get("IsTor"))
            ip = str(doc.get("IP", ""))
        except Exception:
            pass
    ok = code == 200 and is_tor
    return {"ts": datetime.now().isoformat(timespec="seconds"),
            "ok": ok, "code": code, "elapsed": elapsed, "ip": ip}


def summarize(rows: list[dict]) -> dict:
    ok_rows = [r for r in rows if r["ok"]]
    lat = [r["elapsed"] for r in ok_rows]
    exits = sorted({r["ip"] for r in ok_rows if r["ip"]})
    codes = {}
    for r in rows:
        if not r["ok"]:
            codes[str(r["code"])] = codes.get(str(r["code"]), 0) + 1
    return {
        "samples": len(rows),
        "ok": len(ok_rows),
        "ok_rate": round(len(ok_rows) / len(rows), 3) if rows else 0,
        "latency_median": round(statistics.median(lat), 2) if lat else None,
        "latency_max": max(lat) if lat else None,
        "exits": exits,
        "fail_codes": codes,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=180, help="总时长（秒）")
    ap.add_argument("--interval", type=int, default=10, help="采样间隔（秒）")
    ap.add_argument("--socks", default="127.0.0.1:9150")
    ap.add_argument("--timeout", type=float, default=25.0)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    rows = []
    deadline = time.time() + args.duration
    n = 0
    while time.time() < deadline:
        n += 1
        r = sample(args.socks, args.timeout)
        rows.append(r)
        mark = "OK " if r["ok"] else "FAIL"
        print(f"  [{n:02d}] {mark} {r['elapsed']:>6.2f}s  http={r['code']}  exit={r['ip'] or '-'}",
              flush=True)
        time.sleep(max(1, args.interval))

    summary = summarize(rows)
    print("\n=== 汇总 ===")
    print(f"  样本 {summary['samples']}  成功 {summary['ok']}  "
          f"成功率 {summary['ok_rate'] * 100:.1f}%")
    if summary["latency_median"] is not None:
        print(f"  延迟 中位数 {summary['latency_median']}s  最大 {summary['latency_max']}s")
    print(f"  出口 IP: {', '.join(summary['exits']) or '无'}")
    print(f"  失败码: {summary['fail_codes'] or '无'}")

    if args.json:
        Path(args.json).write_text(
            json.dumps({"summary": summary, "samples": rows}, ensure_ascii=False, indent=2),
            encoding="utf-8")
        print(f"  报告: {args.json}")
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
