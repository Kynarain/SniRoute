"""Sniroute 云通道：AWS Lambda（Function URL）版。

部署要点：
  1. 新建 Python 3.12 函数，把本文件内容贴进代码（handler 名为 lambda_handler）
  2. 配置 → 函数 URL → 认证类型选 NONE（否则调用方需要 SigV4 签名）
  3. 环境变量设 TOKEN=<随机串>（强烈建议，公开 URL 不加校验会被刷额度）
  4. 在 Sniroute 里注册：

       sniroute.cmd cloud add --name aws \
           --template "https://<url-id>.lambda-url.<region>.on.aws/?url={url}" \
           --token <同一个随机串>

免费额度：每月 100 万次请求 + 40 万 GB·秒。注意超时上限 15 分钟、
响应缓冲模式 6 MB（要传大文件需开启响应流式模式）。

这不是代理/隧道：只有 HTTP(S) 转发，SSH/游戏 UDP 不经过它。
"""
import base64
import os
import urllib.error
import urllib.request


def lambda_handler(event, context):
    params = event.get("queryStringParameters") or {}
    token = os.environ.get("TOKEN", "")
    if token:
        headers = event.get("headers") or {}
        got = headers.get("x-relay-token") or headers.get("X-Relay-Token") or ""
        if got != token:
            return {"statusCode": 403, "body": "forbidden"}

    target = params.get("url")
    if not target:
        return {"statusCode": 400, "body": "missing ?url="}
    if not target.lower().startswith(("http://", "https://")):
        return {"statusCode": 400, "body": "only http(s) targets"}

    req = urllib.request.Request(target, headers={"User-Agent": "sniroute-lambda"})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            body = resp.read()
            ctype = resp.headers.get("content-type", "application/octet-stream")
            status = resp.status
    except urllib.error.HTTPError as exc:
        body = exc.read() or b""
        ctype = exc.headers.get("content-type", "text/plain") if exc.headers else "text/plain"
        status = exc.code
    except Exception as exc:  # 网络错误
        return {"statusCode": 502, "body": f"upstream error: {exc}"}

    # 二进制内容必须 base64 返回，否则 Lambda 会按 UTF-8 破坏字节
    try:
        text = body.decode("utf-8")
        return {
            "statusCode": status,
            "headers": {"content-type": ctype, "access-control-allow-origin": "*"},
            "body": text,
        }
    except UnicodeDecodeError:
        return {
            "statusCode": status,
            "headers": {"content-type": ctype, "access-control-allow-origin": "*"},
            "body": base64.b64encode(body).decode(),
            "isBase64Encoded": True,
        }
