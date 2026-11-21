"""
ClientHello 分段实验：判断 SNI 阻断是"只看第一个报文"还是"整条流重组"。

做法：用 ssl.MemoryBIO 自己生成/消费 TLS 握手字节，TCP 上按指定策略切开发送。
对比三种发送方式：
  A. 一次性整包发送（对照组，应该被重置）
  B. 首字节 + 剩余（常见的"TLS 分片"技巧）
  C. 把 SNI 字段切成两半（更针对性的切法）

如果 B/C 能握手成功而 A 失败，说明防火墙只看第一个报文；
如果三者都失败，说明它会重组整条流，分段没用。
"""

import socket
import ssl
import struct
import sys
import time

TIMEOUT = 8


def build_client_hello(host):
    """借 ssl.MemoryBIO 生成一个真实的 ClientHello，返回 (bytes, sslobj, read_bio, write_bio)。"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    read_bio = ssl.MemoryBIO()
    write_bio = ssl.MemoryBIO()
    sslobj = ctx.wrap_bio(read_bio, write_bio, server_hostname=host)
    try:
        sslobj.do_handshake()
    except ssl.SSLWantReadError:
        pass
    hello = write_bio.read()
    return hello, sslobj, read_bio, write_bio


def find_sni_offset(hello):
    """在 ClientHello 里定位 SNI 主机名的字节偏移和长度。"""
    # 结构：5 字节记录头 + 4 字节握手头 + 2 版本 + 32 随机数 + 1 会话ID长度 ...
    i = 5 + 4 + 2 + 32
    sid_len = hello[i]
    i += 1 + sid_len
    cs_len = struct.unpack(">H", hello[i:i + 2])[0]
    i += 2 + cs_len
    comp_len = hello[i]
    i += 1 + comp_len
    ext_total = struct.unpack(">H", hello[i:i + 2])[0]
    i += 2
    end = min(len(hello), i + ext_total)
    while i + 4 <= end:
        etype, elen = struct.unpack(">HH", hello[i:i + 4])
        i += 4
        if etype == 0 and elen > 5:
            # server_name 扩展：list_len(2) + name_type(1) + name_len(2) + name
            name_len = struct.unpack(">H", hello[i + 3:i + 5])[0]
            return i + 5, name_len
        i += elen
    return None


def try_handshake(ip, host, plan, port=443):
    """plan: 'whole' | 'split1' | 'splitsni'"""
    hello, sslobj, read_bio, write_bio = build_client_hello(host)
    t0 = time.time()
    try:
        s = socket.create_connection((ip, port), timeout=TIMEOUT)
    except Exception as e:
        return f"TCP失败 {type(e).__name__}"
    s.settimeout(TIMEOUT)
    try:
        if plan == "whole":
            s.sendall(hello)
        elif plan == "split1":
            s.sendall(hello[:1])
            time.sleep(0.01)
            s.sendall(hello[1:])
        elif plan == "splitsni":
            loc = find_sni_offset(hello)
            if not loc:
                return "找不到 SNI 偏移"
            off, nlen = loc
            mid = off + nlen // 2
            s.sendall(hello[:mid])
            time.sleep(0.01)
            s.sendall(hello[mid:])
        # 收响应并推进握手
        for _ in range(12):
            try:
                data = s.recv(4096)
            except socket.timeout:
                return "读超时（多半被静默丢包）"
            if not data:
                return f"连接被关闭 {(time.time()-t0)*1000:.0f}ms"
            read_bio.write(data)
            try:
                sslobj.do_handshake()
                return f"握手成功 {(time.time()-t0)*1000:.0f}ms"
            except ssl.SSLWantReadError:
                out = write_bio.read()
                if out:
                    s.sendall(out)
                continue
            except Exception as e:
                return f"TLS失败 {type(e).__name__}: {str(e)[:40]}"
        return "握手未完成"
    except (ConnectionResetError, BrokenPipeError) as e:
        return f"被重置 {(time.time()-t0)*1000:.0f}ms"
    except Exception as e:
        return f"异常 {type(e).__name__}: {str(e)[:40]}"
    finally:
        try:
            s.close()
        except Exception:
            pass


TARGETS = [
    ("104.18.42.239", "www.pixiv.net", "pixiv / Cloudflare"),
    ("162.159.140.229", "x.com", "x.com / Cloudflare"),
    ("199.232.144.159", "pbs.twimg.com", "pbs.twimg / Fastly"),
]

if __name__ == "__main__":
    for ip, host, label in TARGETS:
        print(f"\n=== {label}  ({ip}, SNI={host}) ===")
        for plan, desc in (("whole", "A 整包发送"),
                           ("split1", "B 首字节+剩余"),
                           ("splitsni", "C SNI 切成两半")):
            print(f"  {desc:14} -> {try_handshake(ip, host, plan)}")
