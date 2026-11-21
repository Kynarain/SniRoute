#!/usr/bin/env python3
"""TLS ClientHello 分片中继。

用途：在被 SNI 阻断的网络上，把发往上游的第一个 TLS record **拆成两个**，
让路径上的 DPI 无法在一次记录里匹配到服务器名，而真服务器会照常重组握手。

为什么值得先试这个：它不需要 ECH、不需要改浏览器、不需要证书，只是转发时
把首包换个形状。成本极低，能立刻回答"今天这套还灵不灵"。

参数不是猜的：mirage（github.com/PoriyaVali/mirage）用 680 次真实探测测出，
第一段 1/2/4/5 字节有效，**3 字节会被丢弃**，6~64 字节也会被丢弃；两个 record
必须放在同一次 TCP 写里。而且同一套参数在不同网络上会反转 —— 所以本工具把
第一段长度做成参数，并且建议你在自己的网络上逐个实测，别抄别人的值。

用法：
    python tools/frag_relay.py --listen 127.0.0.1:8443 \
        --upstream 104.18.42.239:443 --first-len 2
然后让 nginx 的 upstream 指向 127.0.0.1:8443，或直接用
    openssl s_client -connect 127.0.0.1:8443 -servername www.pixiv.net
验证。
"""

from __future__ import annotations

import argparse
import socket
import sys
import threading

# TLS record 头：type(1) + version(2) + length(2)
RECORD_HEADER = 5
HANDSHAKE = 0x16


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """读满 n 字节；对端提前关闭时返回已读到的部分。"""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf


def split_record(record: bytes, first_len: int) -> bytes:
    """把一个 TLS record 改写成两个 record，握手负载不变。

    record = header(5) + payload
    -> header(5, len=first_len) + payload[:first_len]
     + header(5, len=rest)      + payload[first_len:]
    """
    payload = record[RECORD_HEADER:]
    if len(payload) <= first_len:
        return record  # 负载太短，拆了没意义，原样发出

    head = record[:RECORD_HEADER]
    v2 = head[1:3]  # 版本字段原样保留

    first = bytes([head[0]]) + v2 + len(payload[:first_len]).to_bytes(2, "big") + payload[:first_len]
    rest_payload = payload[first_len:]
    second = bytes([head[0]]) + v2 + len(rest_payload).to_bytes(2, "big") + rest_payload
    return first + second


def pump(src: socket.socket, dst: socket.socket) -> None:
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def handle(client: socket.socket, upstream_addr, first_len: int, mode: str) -> None:
    peer = client.getpeername()
    try:
        upstream = socket.create_connection(upstream_addr, timeout=15)
    except OSError as exc:
        log(f"[{peer}] 连不上上游 {upstream_addr}: {exc}")
        client.close()
        return

    try:
        # 1) 取客户端的第一个 TLS record（可能是握手的第一个 flight）
        head = recv_exact(client, RECORD_HEADER)
        if len(head) < RECORD_HEADER or head[0] != HANDSHAKE:
            # 不是 TLS 握手首包（例如明文 HTTP），原样转发，不做处理
            log(f"[{peer}] 首包不是 TLS handshake，原样透传")
            upstream.sendall(head)
            threading.Thread(target=pump, args=(client, upstream), daemon=True).start()
            pump(upstream, client)
            return

        body_len = int.from_bytes(head[3:5], "big")
        body = recv_exact(client, body_len)
        record = head + body
        log(f"[{peer}] 收到 ClientHello record：{len(record)} 字节（负载 {len(body)}）")

        # 2) 改形状后一次性写出（mirage：两个 record 必须在同一次 TCP 写里）
        if mode == "split":
            shaped = split_record(record, first_len)
            upstream.sendall(shaped)
            log(f"[{peer}] 已分片发送：第一段 {first_len} 字节，第二段 {len(record) - RECORD_HEADER - first_len} 字节")
        else:
            upstream.sendall(record)
            log(f"[{peer}] 未分片，原样发送（对照组）")

        # 3) 之后双向透传
        threading.Thread(target=pump, args=(upstream, client), daemon=True).start()
        pump(client, upstream)
    except OSError as exc:
        log(f"[{peer}] 转发中断: {exc}")
    finally:
        for s in (client, upstream):
            try:
                s.close()
            except OSError:
                pass


def parse_hostport(text: str) -> tuple:
    host, _, port = text.rpartition(":")
    return host, int(port or 443)


def main() -> None:
    ap = argparse.ArgumentParser(description="TLS ClientHello 分片中继")
    ap.add_argument("--listen", default="127.0.0.1:8443", help="监听地址，默认 127.0.0.1:8443")
    ap.add_argument("--upstream", required=True, help="上游地址，例如 104.18.42.239:443")
    ap.add_argument("--first-len", type=int, default=2,
                    help="第一段字节数。实测有效值 1/2/4/5；3 会被丢弃。默认 2")
    ap.add_argument("--mode", choices=["split", "pass"], default="split",
                    help="split=分片；pass=原样转发（做对照实验用）")
    args = ap.parse_args()

    listen = parse_hostport(args.listen)
    upstream = parse_hostport(args.upstream)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(listen)
    srv.listen(64)

    log(f"监听 {listen[0]}:{listen[1]} → 上游 {upstream[0]}:{upstream[1]}，"
        f"模式={args.mode}，第一段={args.first_len} 字节")

    try:
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=handle,
                             args=(conn, upstream, args.first_len, args.mode),
                             daemon=True).start()
    except KeyboardInterrupt:
        log("退出")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
