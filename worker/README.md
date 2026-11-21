# 手机 / 浏览器：用 ECH 绕过 SNI 封锁

不需要 root、不需要装证书、不需要 VPN、不需要海外服务器 —— 只用一个域名和一个
Cloudflare Worker。

## 原理

```
浏览器 ──DoH(可信证书)──> doh.你的域名  ──> Cloudflare 自己的 DoH
                                │
                                └─ 返回：真实 A 记录 + 带 ech= 的 HTTPS 记录
浏览器 ──ECH(外层 SNI=cloudflare-ech.com)──> 目标站点
                                GFW 只看到 cloudflare-ech.com，匹配不到目标域名
```

两件事缺一不可：

1. **解析要真实**。国内 DoH（阿里、腾讯）对被封锁域名会返回投毒的答案 ——
   实测 `www.pixiv.net` 的 A 记录被指到 Facebook 段，HTTPS 记录里连 `ech` 都没有。
2. **HTTPS 记录必须带 `ech=`**。pixiv / discord 这些站点自己**没有**发布 ECH 配置
   （它们的 HTTPS 记录里没有 ech 字段），但 Cloudflare 有一份全网共享的
   greeting 配置（`cloudflare-ech.com`），Worker 需要把它补进应答。

## 部署

1. 域名接入 Cloudflare（免费套餐即可）
2. 建一个 Worker，贴 `worker/doh-worker.js` 的内容，部署
3. 给 Worker 绑自定义域，例如 `doh.你的域名`（**不要用 `*.workers.dev`**，墙内常打不开）
4. 若绑的是"路由"而不是"自定义域"，还需要一条 DNS 记录：
   `A` / 名字 `doh` / 值 `192.0.2.1` / **代理开启（橙色云）**

## 浏览器设置

Chrome（Android / 桌面）：

```
设置 → 隐私和安全 → 使用安全 DNS → 选择服务提供方 → 自定义 → 填入
https://doh.你的域名/dns-query
```

Chrome 可能会显示 **"Please verify that this is a valid provider"**，并且页面顶部有
**"No internet connection"** 提示。这是 Chrome 拿 Google 的连通性探测做判断，
墙内必然失败；**它并不影响 DoH 实际生效**（实测页面能正常加载）。

如果反复失效，重进这个页面重新点一下自定义框即可。

## 验证

```bash
# 1. Worker 自诊断：ech_source 应为 wire（从 Cloudflare 现取），不是 fallback
curl https://doh.你的域名/debug

# 2. 用真实 DNS 线格式（浏览器就是这么发的）查 ECH
node worker/probe-doh.mjs
```

预期：

```
www.pixiv.net    ech=有 (AEX+DQBBegAg...) ipv4=104.18.42.239,172.64.145.17
discord.com      ech=有 (...)
带 ECH: 3/3
```

## 实测结果（Android 模拟器 API 37，无 VPN、无 CA）

| 站点 | 结果 |
|---|---|
| **pixiv.net** | ✅ 完整加载（两次复测，正文/导航/登录入口都在） |
| **store.steampowered.com** | ✅ 完整加载（这一条不需要 ECH：拿到真实 DNS 即可） |
| GitHub | ✅ 完整加载（同上） |
| discord.com | ❌ 空白（ECH 配置取自其自有记录，页面未渲染） |
| x.com | ❌ 空白（重型 SPA + 地区限制） |
| steamcommunity.com | ❌ 未测试；它不在 Cloudflare 上，本方案管不到 |


## 第二条通道：QUIC / HTTP/3（实测有效）

QUIC 把整个 TLS 握手（含 SNI）放进加密载荷，中间设备只能看到版本号和连接 ID。
所以**不需要 ECH**，SNI 本来就不可见——只要目标 CDN 支持 HTTP/3、且 UDP 443 没被封。

实测（桌面 Chrome，关闭 ECH）：

| 测试 | 结果 |
|---|---|
| pixiv 强制 QUIC（UDP 443） | 3/3 成功，页面内容完整 |
| pixiv 同 IP 走 TCP 443 | ERR_CONNECTION_RESET |
| x.com QUIC | 0/2（该 IP 未走通） |
| Fastly（pbs/ton.twimg）QUIC | 失败 |

因此 Worker 合成的 HTTPS 记录里 `alpn=h3,h2`，让浏览器优先尝试 QUIC。
ECH 与 QUIC 是两条独立通道：ECH 配置轮换失效时，QUIC 还能顶上去。

## 实测结果（补充）

模拟器 API 37，无 VPN、无 CA，只设置 Chrome 安全 DNS：

- **pixiv.net**：完整加载，含横幅广告与 Collections 缩略图（`i.pximg.net` 的图片也出来了）
- **store.steampowered.com**、**GitHub**：完整加载
- **x.com**：HTML 能到，样式/JS 域（abs.twimg.com）已在白名单内，但人机验证页与图片仍受限
- **discord.com**：空白

## 已知限制

- **只对 Cloudflare 前置的站点有效**（ECH 是 CF 的能力）。Steam 社区、以及大量
  未上 CF 的站点不适用。
- **`cloudflare-ech.com` 是被重点观察的名字**。一旦被针对性封锁，这条路会一起失效。
- **ECH 配置会轮换**。Worker 里每 30 分钟从 Cloudflare 现取一次，取不到才用兜底常量；
  兜底常量过期后 ECH 会失败（表现为页面打不开），此时看 `/debug` 的 `ech_source`。
- Worker 免费额度：每天 10 万次请求。日常浏览足够，别拿它跑大流量。

## 排查

| 现象 | 检查 |
|---|---|
| 页面打不开、`/debug` 里 `ech_source=fallback` | 取配置的通道断了，看 Worker 日志 |
| `ech=有` 但页面仍打不开 | Chrome 可能没启用 ECH；确认版本 ≥ 117 且未禁用 |
| Chrome 拒绝填这个 URL | 先访问 `https://doh.你的域名/` 确认返回 `sniroute doh ok` |
| 只有部分站点能用 | 那些站点不在 Cloudflare 上，本方案不覆盖 |
