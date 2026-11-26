"""用 aioquic 生成的合法 Initial 当参照，校准我们自己构造 QUIC Initial 的代码。

背景：手拼 Initial 很容易在长度字段/头部保护采样上出错，服务端会静默丢弃，
于是"无回包"无法区分"包不合法"与"被墙丢包"。这里先拿 aioquic 的包做基准，
把我们的构造器对齐，之后才敢用它去测 sni-slicing。
"""
import os
import struct
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF, HKDFExpand

SALT_V1 = bytes.fromhex("38762cf7f55934b34d179ae6a4c80cadccbb7f0a")


def hkdf_expand_label(secret, label, length):
    full = b"tls13 " + label.encode()
    info = struct.pack(">H", length) + bytes([len(full)]) + full + b"\x00"
    return HKDFExpand(algorithm=hashes.SHA256(), length=length, info=info).derive(secret)


def initial_keys(dcid):
    # 注意：cryptography 的 HKDF(...).derive() 返回的是 Extract+Expand 的结果，
    # 不是原始 PRK。QUIC 需要的是 RFC 8446 的 HKDF-Extract（= HMAC(salt, ikm)），
    # 再用 Expand-Label 派生 key/iv/hp。用 HKDF 会得到完全不同的密钥。
    import hmac as _hmac
    prk = _hmac.new(SALT_V1, dcid, "sha256").digest()
    client = hkdf_expand_label(prk, "client in", 32)
    return (hkdf_expand_label(client, "quic key", 16),
            hkdf_expand_label(client, "quic iv", 12),
            hkdf_expand_label(client, "quic hp", 16))


def varint(n):
    if n < 64:
        return bytes([n])
    if n < 16384:
        return struct.pack(">H", 0x4000 | n)
    return struct.pack(">I", 0x80000000 | n)


def parse_long_header(pkt):
    """解析 Initial 头，返回 (dcid, scid, pn_offset, length_field, payload_len)。"""
    assert pkt[0] & 0x80, "不是长头"
    pos = 1
    version = pkt[pos:pos + 4]
    pos += 4
    dcid_len = pkt[pos]
    pos += 1
    dcid = pkt[pos:pos + dcid_len]
    pos += dcid_len
    scid_len = pkt[pos]
    pos += 1
    scid = pkt[pos:pos + scid_len]
    pos += scid_len
    # token
    token_len = pkt[pos]
    pos += 1
    if token_len & 0xC0 == 0xC0:
        token_len = int.from_bytes(pkt[pos - 1:pos + 1], "big") & 0x3FFF
        pos += 1
    pos += token_len
    # length varint (2 bytes for our sizes)
    first = pkt[pos]
    if first & 0x80 == 0 and first & 0x40:
        ln = int.from_bytes(pkt[pos:pos + 2], "big") & 0x3FFF
        pos += 2
    elif first & 0xC0 == 0x80:
        ln = int.from_bytes(pkt[pos:pos + 4], "big") & 0x3FFFFFFF
        pos += 4
    else:
        ln = first
        pos += 1
    return dcid, scid, version, pos, ln


def remove_hp(pkt, dcid):
    key, iv, hp = initial_keys(dcid)
    _, _, _, pn_offset, ln = parse_long_header(pkt)
    sample = pkt[pn_offset + 4: pn_offset + 4 + 16]
    mask = Cipher(algorithms.AES(hp), modes.ECB()).encryptor().update(sample)
    first = pkt[0] ^ (mask[0] & 0x0F)
    pn_len = (first & 0x03) + 1
    pn = bytes(pkt[pn_offset + i] ^ mask[1 + i] for i in range(pn_len))
    return first, pn_len, pn, key, iv


def decrypt(pkt, dcid):
    first, pn_len, pn, key, iv = remove_hp(pkt, dcid)
    _, _, _, pn_offset, ln = parse_long_header(pkt)
    ct = pkt[pn_offset + pn_len: pn_offset + ln]
    nonce = bytes(a ^ b for a, b in zip(iv, pn.rjust(12, b"\x00")))
    # AAD = 还原后的头部（首字节和包号都要去掉头部保护）
    aad = bytes([first]) + pkt[1:pn_offset] + pn
    return AESGCM(key).decrypt(nonce, ct, aad), first, pn_len, pn


def build(dcid, scid, payload, pn=0, pn_len=4):
    """按 RFC 9001 构造一个 Initial 数据包（AAD = 完整头部）。"""
    key, iv, hp = initial_keys(dcid)
    pn_bytes = pn.to_bytes(pn_len, "big")
    ct_len = len(payload) + 16
    first = 0xC0 | (pn_len - 1)
    header = (bytes([first]) + struct.pack(">I", 1) +
              bytes([len(dcid)]) + dcid + bytes([len(scid)]) + scid +
              b"\x00" + varint(pn_len + ct_len) + pn_bytes)
    nonce = bytes(a ^ b for a, b in zip(iv, pn_bytes.rjust(12, b"\x00")))
    ct = AESGCM(key).encrypt(nonce, payload, header)
    pkt = bytearray(header + ct)
    pn_offset = len(header) - pn_len
    sample = bytes(pkt[pn_offset + 4: pn_offset + 4 + 16])
    mask = Cipher(algorithms.AES(hp), modes.ECB()).encryptor().update(sample)
    pkt[0] ^= mask[0] & 0x0F
    for i in range(pn_len):
        pkt[pn_offset + i] ^= mask[1 + i]
    return bytes(pkt)


def aioquic_reference(sni, host, port):
    """让 aioquic 生成一个真实 Initial，返回字节。"""
    from aioquic.quic.configuration import QuicConfiguration
    from aioquic.quic.connection import QuicConnection
    conf = QuicConfiguration(is_client=True, alpn_protocols=["h3"])
    conf.server_name = sni
    conn = QuicConnection(configuration=conf)
    conn.connect((host, port), now=0.0)
    datagrams, _ = conn.datagrams_to_send(now=0.0)
    return datagrams[0] if datagrams else b""


if __name__ == "__main__":
    ref = aioquic_reference("www.twitter.com", "69.171.235.22", 443)
    print(f"  aioquic 参考包: {len(ref)} 字节")
    dcid, scid, version, pn_offset, ln = parse_long_header(ref)
    print(f"  DCID={dcid.hex()} SCID={scid.hex()} pn_offset={pn_offset} length={ln}")
    plain, first, pn_len, pn = decrypt(ref, dcid)
    print(f"  first=0x{first:02x} pn_len={pn_len} pn={pn} 明文={len(plain)} 字节")
    # 用同样的 payload 重新构造，看是否与参考一致
    rebuilt = build(dcid, scid, plain, pn=int.from_bytes(pn, "big"), pn_len=pn_len)
    print(f"  重建包: {len(rebuilt)} 字节  与参考一致: {rebuilt == ref}")
    if rebuilt != ref:
        for i, (a, b) in enumerate(zip(rebuilt, ref)):
            if a != b:
                print(f"  第一个差异在第 {i} 字节: 重建=0x{a:02x} 参考=0x{b:02x}")
                break
