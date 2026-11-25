"""QUIC 握手探测共享库。

为什么单独成库：`site` 子命令和 `tools/quic-sni-test.py` 都要做同一件事——
用真实的 QUIC 栈去和服务器握一次手，看能不能拿到 ServerHello。
把实现放这里，避免两处逻辑漂移。

关键背景（Zohaib et al., USENIX Security '25）：
  GFW 会解密 QUIC Initial 包（密钥由明文 DCID 派生），读出 SNI 后按
  一份**独立的 QUIC 黑名单**丢包。所以 QUIC 并不是天然的"SNI 隐身衣"，
  能不能用取决于该域名是否在黑名单里。

返回值的 reason 只有四种：
  ok          握手成功
  timeout     没有 ServerHello —— 可能是 GFW 黑名单，也可能是本地 UDP 阻断
  unsupported 服务器不支持 h3（与网络无关）
  error       其它异常（参数错误、aioquic 版本问题等）
"""
from __future__ import annotations

import asyncio
import ssl
import time


class QuicUnavailable(RuntimeError):
    """aioquic 未安装 —— 调用方应当降级而不是报错。"""


def _load_aioquic():
    try:
        from aioquic.asyncio import connect  # noqa: F401
        from aioquic.quic.configuration import QuicConfiguration  # noqa: F401
    except ImportError as exc:  # pragma: no cover - 依赖缺失时才会走到
        raise QuicUnavailable("缺少 aioquic（pip install aioquic）") from exc
    return connect, QuicConfiguration


async def _handshake(sni: str, ip: str, port: int, src_port: int, timeout: float) -> dict:
    connect, QuicConfiguration = _load_aioquic()
    conf = QuicConfiguration(is_client=True, alpn_protocols=["h3"])
    conf.server_name = sni
    conf.verify_mode = ssl.CERT_NONE
    t0 = time.time()
    try:
        async with asyncio.timeout(timeout):
            # local_port 用来固定源端口，验证"源端口 > 目的端口"这条过滤规则
            async with connect(ip, port, configuration=conf, local_port=src_port,
                               wait_connected=True):
                return {"ok": True, "reason": "ok", "elapsed": round(time.time() - t0, 3)}
    except (asyncio.TimeoutError, TimeoutError):
        return {"ok": False, "reason": "timeout", "elapsed": round(time.time() - t0, 3)}
    except ssl.SSLError:
        return {"ok": False, "reason": "unsupported", "elapsed": round(time.time() - t0, 3)}
    except BaseException as exc:  # aioquic 的连接错误在不同版本里类型不同
        name = type(exc).__name__
        if name in ("ConnectionError", "QuicConnectionError"):
            return {"ok": False, "reason": "unsupported",
                    "elapsed": round(time.time() - t0, 3), "error": name}
        return {"ok": False, "reason": "error",
                "elapsed": round(time.time() - t0, 3), "error": f"{name}: {str(exc)[:80]}"}


def quic_handshake(sni: str, ip: str, port: int = 443, src_port: int = 0,
                   timeout: float = 8.0) -> dict:
    """同步入口：跑一次 QUIC 握手并返回结果字典。

    aioquic 未安装时抛 QuicUnavailable，调用方应捕获并降级。
    """
    loop = asyncio.new_event_loop()
    # aioquic 在连接被丢弃时会有未被取回的 Future，默认处理器会往 stderr 吐噪音。
    # 这些消息对使用者没有意义，统一吞掉。
    loop.set_exception_handler(lambda _loop, _ctx: None)
    try:
        return loop.run_until_complete(_handshake(sni, ip, port, src_port, timeout))
    finally:
        loop.close()


def quic_handshake_best(sni: str, ips: list[str], port: int = 443, src_port: int = 0,
                        timeout: float = 8.0, limit: int = 2) -> dict:
    """对前 limit 个 IP 依次尝试，返回最快的成功结果（都失败则返回最后一个）。"""
    last: dict = {"ok": False, "reason": "error", "elapsed": 0.0}
    for ip in ips[:limit]:
        result = quic_handshake(sni, ip, port, src_port, timeout)
        result["ip"] = ip
        if result["ok"]:
            return result
        last = result
    return last


def describe(result: dict) -> str:
    """把结果转成一句人话，供命令行输出。"""
    reason = result.get("reason")
    elapsed = result.get("elapsed", 0.0)
    if reason == "ok":
        return f"成功 {elapsed:.2f}s"
    if reason == "timeout":
        return f"超时（{elapsed:.1f}s 无 ServerHello，疑似 QUIC 黑名单）"
    if reason == "unsupported":
        return "服务器不支持 h3"
    return f"探测失败（{result.get('error', '未知原因')}）"
