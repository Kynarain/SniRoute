"""纯 Python 的 X25519（RFC 7748）。

为什么自己写：
  打包的 Python 运行时不带 cryptography / nacl，而 WARP 注册必须提交
  Curve25519 公钥。引入第三方库会让"裁剪过的运行时"重新变大，所以用
  标准参考实现（RFC 7748 §5 的伪代码）落成 40 行纯 Python。

性能：一次密钥生成约 1–3 ms，只用于注册，完全够。
"""
from __future__ import annotations

import os

P = 2 ** 255 - 19
A24 = 121665


def _cswap(swap: int, a: int, b: int) -> tuple[int, int]:
    dummy = (-swap) & (a ^ b)
    return a ^ dummy, b ^ dummy


def x25519(scalar: bytes, u_bytes: bytes) -> bytes:
    """标量乘法。scalar 32 字节私钥，u_bytes 32 字节 u 坐标。"""
    k = bytearray(scalar)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64

    k_int = int.from_bytes(k, "little")
    u_int = int.from_bytes(u_bytes, "little") & ((1 << 255) - 1)

    x1 = u_int
    x2, z2 = 1, 0
    x3, z3 = u_int, 1
    swap = 0

    for t in range(254, -1, -1):
        k_t = (k_int >> t) & 1
        swap ^= k_t
        x2, x3 = _cswap(swap, x2, x3)
        z2, z3 = _cswap(swap, z2, z3)
        swap = k_t

        a = (x2 + z2) % P
        aa = (a * a) % P
        b = (x2 - z2) % P
        bb = (b * b) % P
        e = (aa - bb) % P
        c = (x3 + z3) % P
        d = (x3 - z3) % P
        da = (d * a) % P
        cb = (c * b) % P
        x3 = pow((da + cb) % P, 2, P)
        z3 = (x1 * pow((da - cb) % P, 2, P)) % P
        x2 = (aa * bb) % P
        z2 = (e * ((aa + (A24 * e) % P) % P)) % P

    x2, x3 = _cswap(swap, x2, x3)
    z2, z3 = _cswap(swap, z2, z3)

    result = (x2 * pow(z2, P - 2, P)) % P
    return result.to_bytes(32, "little")


BASE_POINT = (9).to_bytes(32, "little")


def generate_keypair() -> tuple[bytes, bytes]:
    """返回 (private, public)，各 32 字节。"""
    private = os.urandom(32)
    return private, x25519(private, BASE_POINT)


if __name__ == "__main__":
    import base64
    import time

    t0 = time.time()
    priv, pub = generate_keypair()
    dt = (time.time() - t0) * 1000
    print(f"耗时 {dt:.1f} ms")
    print("private:", base64.b64encode(priv).decode())
    print("public :", base64.b64encode(pub).decode())
