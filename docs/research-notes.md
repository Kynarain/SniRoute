# 审查测量与 QUIC 封锁：三篇论文的结论与设计约束

本文记录与 Sniroute 直接相关的三项研究，以及它们如何影响我们的设计决策。
所有"实测"数据都来自本机（中国移动家宽，Windows 10），只代表这一条链路。

---

## 1. Ensafi, *Ethical, Scale, and Continuity Concerns for Censorship Measurement*

IETF HRPC 演讲幻灯（`slides-102-hrpc-...`）。

### 核心问题

长期测量审查有三个绕不开的挑战：

| 挑战 | 含义 |
|---|---|
| **覆盖** coverage | 志愿者分布偏斜，没覆盖到的地方就是盲区 |
| **伦理** ethics | 志愿者可能被 ISP 观测甚至针对，必须充分告知 |
| **连续性** continuity | 研究者毕业、资金断供，测量就停止 |

### OONI 的五项应对

1. 详细且诚实的同意书——明确告诉用户"你的 ISP 能看出你在跑 OONI Probe"
2. 与当地社群建立长期关系
3. 维持志愿者社区
4. 专注 + 开源承诺
5. 稳定的资金（OTF、M-Lab、Free Press）

### 远程测量（不让志愿者冒险）

- **Spooky Scan**：用 TCP/IP 侧信道（IP ID 递增）判断两端能否通信、哪个方向被丢
- **Augur**：把同一套侧信道做成互联网级、可统计的持续检测
- 前提：测量机能伪造源 IP、反射主机有全局 IP ID

### 对 Sniroute 的约束

- 我们只做**单机、低量**探测（每一轮几个包），不触碰伦理红线
- 反过来说，我们的结论**只代表自己这条链路**，必须在文档里写明
- 不做大范围扫描，不实现"从第三方主机远程探测"

---

## 2. Xue et al., *Fingerprinting Obfuscated Proxy Traffic with Encapsulated TLS Handshakes*

USENIX Security 2024（`usenixsecurity24-xue-fingerprinting.pdf`）。

### 方法：协议无关的代理检测

```
任何代理/隧道都产生"嵌套协议栈"
        ↓
嵌套的 TLS 握手在加密流里留下可识别痕迹
        ↓
相似度分类器把它挑出来
```

### 关键结论

| 结论 | 含义 |
|---|---|
| 随机填充（padding）挡不住 | 握手包的大小与往返模式仍然可见 |
| 多层封装也挡不住 | 嵌套本身才是特征 |
| 流多路复用（multiplexing）是目前有效对策 | 但它只是"缓解" |
| 仅靠 padding + 多路复用的混淆**有结构性上限** | 减少不了流量突发大小与 RTT 数 |
| 实测部署在百万用户级 ISP，误伤很小 | 不是理论推演 |

### 对 Sniroute 的约束

- **新功能不做"隧道套 TLS"**：本机 loopback 是本地终结，线路上仍是单层 TLS；
  Worker 反代也是单层 TLS——这两类安全
- 任何"把用户的 TLS 再包进另一层 TLS"的设计，都要先假定它可被识别
- 这条约束写进了本项目的设计原则

---

## 3. Zohaib et al., *Exposing and Circumventing SNI-based QUIC Censorship of the Great Firewall of China*

USENIX Security 2025 正文 + artifact appendix（`usenixsecurity25-appendix-zohaib.pdf`）。

### 机制

QUIC Initial 的密钥由**明文 DCID + 固定盐**派生（RFC 9001），任何观察者都能解密：

```
GFW 解密 Initial → 读出 TLS ClientHello 里的 SNI → 查独立 QUIC 黑名单 → 丢包
```

### 关键数据

| 项 | 数值 |
|---|---|
| 起止 | 2024-04-07 开始 |
| 每周平均封锁 | 43.8K FQDN（Tranco） |
| 累计观测 | 58,207 个域名 |
| 黑名单规模 | 约为 DNS 黑名单的 60% |
| 过滤规则之一 | **源端口 > 目的端口** 时触发 |
| 方向 | 只封客户端→服务端 |

### 已落地的绕过：Initial 分片（SNI slicing）

GFW **不重组跨 UDP 数据报的 QUIC Initial**：

| 时间 | 事件 |
|---|---|
| 2025-01-27 | Neqo v0.12.0 加入 |
| 2025-04-30 | Firefox 137 默认启用 `network.http.http3.sni-slicing` |
| 2025-05-23 | quic-go v0.52.0 → sing-box / V2Ray / Xray / Hysteria / TUIC 默认支持 |

论文还指出 Chrome（v124+ 后量子密钥过大）天然把 Initial 拆成多个数据报，意外地绕过了该机制。

### 附带发现

- 中等规模 QUIC 流量突增即可压制其封锁效果（解密开销是瓶颈）
- 该机制可被利用阻断任意两台主机之间的 UDP（已披露）

### Artifact

- 代码与数据：`github.com/gfw-report/usenixsecurity25-quic-sni`
- 归档：DOI `10.5281/zenodo.15606165`

### 对 Sniroute 的约束

- QUIC 黑名单是**按域名**的，不能一概而论
- `site` 命令必须先查 HTTPS 记录是否声明 h3，再决定要不要打 UDP 探测
- 超时只能说明"没拿到 ServerHello"，**无法区分本地 UDP 阻断与 GFW 黑名单**——必须在输出里注明

---

## 我们的实测（2026-11-25，本机）

用 `tools/quic-sni-test.py`（aioquic 完整客户端，直连真实边缘）：

| 域名 | TCP 带 SNI | QUIC 握手 | 说明 |
|---|---|---|---|
| www.pixiv.net | 被重置 | 成功 0.2~0.7s | 不在 QUIC 黑名单 |
| www.google.com | 被重置 | 成功 0.5s（有时超时） | 网络状态有波动 |
| www.cloudflare.com | 成功 | 成功 0.2~1.0s（有时超时） | 对照 |
| www.reddit.com | — | 成功 0.1~0.7s | 不在黑名单 |
| www.youtube.com | 被重置 | 超时 | 在 QUIC 黑名单 |
| www.vrchat.com | 被重置 | 无 h3 | 服务器不支持 QUIC |

Chrome 实测（`--log-net-log`）：打开 pixiv 会建立 `QUIC_SESSION`，页面 DOM 约 74 KB。

### 一个容易踩的坑

用 `--origin-to-force-quic-on` 强制 Chrome 走 QUIC 时，所有站点都报
`ERR_QUIC_PROTOCOL_ERROR`——那是**强制标志与 ECH/NRPT 的交互问题**，不是网络封锁。
判断 QUIC 可用性必须用真实 QUIC 栈（aioquic）或让 Chrome 自然协商。

---

## 三篇合起来给我们的三条原则

| 原则 | 依据 |
|---|---|
| 不做大规模扫描，结论标注"仅代表本链路" | Ensafi |
| 新功能不做双层 TLS 封装 | Xue |
| QUIC 按域名判定，不预设"能用/不能用" | Zohaib |

---

## 引用

- R. Ensafi. *Ethical, Scale, and Continuity Concerns for Censorship Measurement.* IETF HRPC slides 102.
- D. Xue, M. Kallitsis, A. Houmansadr, et al. *Fingerprinting Obfuscated Proxy Traffic with Encapsulated TLS Handshakes.* USENIX Security 2024.
- A. Zohaib, Q. Zao, J. Sippe, A. Alaraj, A. Houmansadr, Z. Durumeric, E. Wustrow. *Exposing and Circumventing SNI-based QUIC Censorship of the Great Firewall of China.* USENIX Security 2025.
