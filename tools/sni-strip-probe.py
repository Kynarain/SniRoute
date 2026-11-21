"""
验证「删掉 ClientHello 里的 SNI 再转发」这条路：

1. 用 MemoryBIO 生成带 SNI 的真实 ClientHello
2. 从字节里把 server_name 扩展整段删掉，修好长度字段
3. 发给 s.pximg.net，完成握手
4. 真的发一个 HTTP 请求，看能不能拿到内容

如果成功，说明代理层"改写握手"能救 pixiv 的样式/图片域名 —— 不需要证书、不需要 root。
"""

import socket
import ssl
import struct
import sys

HOST = "s.pximg.net"
PATH = "/css/pixiv.css"


def build_hello(host):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    rb, wb = ssl.MemoryBIO(), ssl.MemoryBIO()
    obj = ctx.wrap_bio(rb, wb, server_hostname=host)
    try:
        obj.do_handshake()
    except ssl.SSLWantReadError:
        pass
    return wb.read(), obj, rb, wb


def strip_sni(hello):
    """删掉 server_name 扩展，返回 (新字节, 是否删成功)。"""
    data = bytearray(hello)
    # 5B 记录头 + 4B 握手头 + 2B 版本 + 32B 随机
    i = 5 + 4 + 2 + 32
    sid_len = data[i]
    i += 1 + sid_len
    cs_len = struct.unpack(">H", data[i:i + 2])[0]
    i += 2 + cs_len
    comp_len = data[i]
    i += 1 + comp_len
    ext_total_off = i
    ext_total = struct.unpack(">H", data[i:i + 2])[0]
    i += 2
    ext_end = i + ext_total
    while i + 4 <= ext_end:
        etype, elen = struct.unpack(">HH", data[i:i + 4])
        if etype == 0:  # server_name
            del data[i:i + 4 + elen]
            new_total = ext_total - (4 + elen)
            data[ext_total_off:ext_total_off + 2] = struct.pack(">H", new_total)
            # 握手头长度（去掉 4B 握手头后的长度）
            hs_len = struct.unpack(">I", bytes([0]) + data[6:9])[0]
            data[6:9] = struct.pack(">I", hs_len - (4 + elen))[1:]
            # 记录长度
            rec_len = struct.unpack(">H", data[3:5])[0]
            data[3:5] = struct.pack(">H", rec_len - (4 + elen))
            return bytes(data), True
        i += 4 + elen
    return bytes(data), False


def run(host, ip):
    print(f"  === {host} ({ip}) ===")
    hello, obj, rb, wb = build_hello(host)
    print(f"    原始 ClientHello: {len(hello)} 字节（含 SNI）")
    stripped, ok = strip_sni(hello)
    print(f"    删除 SNI 后     : {len(stripped)} 字节（删除{'成功' if ok else '失败'}）")

    # 用删好的字节，但需要保留原来用于解析服务器响应的 ssl 对象；
    # 这里重新建一个"无 SNI"的对象来消费响应。
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    rb2, wb2 = ssl.MemoryBIO(), ssl.MemoryBIO()
    obj2 = ctx.wrap_bio(rb2, wb2)  # 无 server_hostname

    s = socket.create_connection((ip, 443), timeout=10)
    s.settimeout(10)
    try:
        s.sendall(stripped)
        for _ in range(12):
            d = s.recv(16384)
            if not d:
                print("    连接被关闭"); return
            rb2.write(d)
            try:
                obj2.do_handshake()
                break
            except ssl.SSLWantReadError:
                out = wb2.read()
                if out:
                    s.sendall(out)
                continue
        cert = obj2.getpeercert(binary_form=True)
        print(f"    握手成功，证书 {len(cert) if cert else 0} 字节")

        req = (f"GET {PATH} HTTP/1.1\r\nHost: {host}\r\n"
               "User-Agent: Mozilla/5.0\r\nConnection: close\r\nAccept: */*\r\n\r\n")
        ss = obj2
        # 用 BIO 对象发数据
        ss.write(req.encode())
        out = wb2.read()
        if out:
            s.sendall(out)
        buf = b""
        while True:
            try:
                d = s.recv(16384)
            except socket.timeout:
                break
            if not d:
                break
            rb2.write(d)
            try:
                chunk = ss.read(16384)
                if chunk:
                    buf += chunk
            except ssl.SSLWantReadError:
                continue
            except ssl.SSLZeroReturnError:
                break
            if len(buf) > 4000:
                break
        first = buf.split(b"\r\n")[0].decode("latin-1", "replace") if buf else "(无响应)"
        print(f"    HTTP 响应: {first}  共 {len(buf)} 字节")
    except Exception as e:
        print(f"    失败: {type(e).__name__}: {str(e)[:70]}")
    finally:
        s.close()


if __name__ == "__main__":
    run(HOST, "210.140.139.137")
    run("i.pximg.net", "210.140.139.135")
