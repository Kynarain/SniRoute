# 参考

按"它解决了我们哪个具体问题"排列。每条都注明**它改变了本项目的什么判断**——没有这句的
链接不值得留。

---

## 一、同类实现（这套做法的先例）

| 参考 | 关系 |
|---|---|
| [racpast/SNIBypassGUI](https://github.com/racpast/SNIBypassGUI) | 思路来源：sni-gate（443，TLS 外观层）+ nginx（22222，HTTP 代理）+ 本机 CA + 私有签名更新。**主站靠 ECH 连 Cloudflare**，这是本项目做不到的部分 |
| [Watt Toolkit（Steam++）证书导入](https://steampp.net/liunxSetupCer) | 同模式的成熟商业实现：装自己的根 CA、改 hosts、本地反代。说明"装本机 CA"是这类工具的标准做法 |
| [mashirozx/Pixiv-Nginx](https://github.com/mashirozx/Pixiv-Nginx) | 原始思路（写死 IP + 隐藏 SNI）。本项目保留了想法，丢掉了它的配置结构 |

**改变了什么**：确认"本地终止 TLS + 自签 CA"是正路；也确认了它的天花板——当目标走 ECH 时，
纯 nginx 就到头了（见下面第四节）。

---

## 二、域前置到底还能不能用

[**Measuring CDNs susceptible to Domain Fronting**](https://ar5iv.labs.arxiv.org/html/2310.17851)
（Subramani, Perdisci, Skafidas；Georgia Tech）

测了 30 个 CDN：**22 个仍可域前置**，其中 **Fastly 100%、Akamai 52%** 的测试域名成功；
**Cloudflare、CloudFront 全线拦截**。机制是 CDN 用 Host 选源站、用 SNI 发证书，当两个域名
在同一 CDN 后面时才能"借道"。

**改变了什么**：我曾根据一次 CF 测试就断言"域前置已死"，这不准确。准确的表述是——
域前置仍可用，但**要求目标站点自己也在那个 CDN 后面**。这条结论让我们把主站问题从
"换个边缘再试"改判为"必须换机制"。

---

## 三、SNI 阻断的行为特征

- [**M3: Behavioral ClientHello scoring**（2026-06）](https://zenodo.org/records/22112161/files/tspu-research-2026-en.pdf?download=1)
  —— 把封锁设备对 ClientHello 的打分行为量化。
- [**MB-RA / MB-R 对比**](https://core.ac.uk/download/534239234.pdf) —— 关键描述："MB-R 观察到
  被禁 SNI 后不会立刻拆连接"。

**改变了什么**：我们的实测正好落在这个模式里——`SNI=www.pixiv.net` 到 CF 边缘**前几次握手
成功**，几分钟后同一 IP 上该 SNI 全部被重置，而 `SNI=cloudflare.com` 依然正常。看到这段
文献之前，我以为这是"某次配置改坏了"。

---

## 四、ECH：主站唯一剩下的路

- [**Encrypted Client Hello (ECH) in Censorship Circumvention**（FOCI 2025）](https://www.petsymposium.org/foci/2025/foci-2025-0016.pdf)
- nginx 官方：[`ngx_stream_ssl_preread_module`](https://nginx.org/ru/docs/stream/ngx_stream_ssl_preread_module.html)

**改变了什么**：解释了 SNIBypassGUI 为什么必须自研 `sni-gate.exe`——它的
`[listener.ech] require_ech = true` 把 SNI 加密后交给 Cloudflare 解密，这个值不再出现在
可观察位置。nginx 没有 ECH 客户端指令，Windows 的 curl 8.13（Schannel）也不支持 `--ech`，
所以本项目要跨过这道坎只能再加一个 Go/Rust 网关。

---

## 五、另一条路：把问题外包

- [PixivNow](https://github.com/iris1598/PixivNow) —— serverless 后端代理。
- [pixiv-proxy](https://github.com/vmoranv/pixiv-proxy) —— 第三方免费反代。

**改变了什么**：提醒代价——`i.pixiv.cat` 就是这么消失的（见 Pixiv-Nginx
[#62](https://github.com/mashirozx/Pixiv-Nginx/issues/62)）。可用性掌握在别人手里。

---

## 六、别人的实现（按机制分类，2026-11 调研）

| 项目 | 机制 | 对我们意味着什么 |
|---|---|---|
| [SniShaper](https://github.com/SnishaperTeam/SniShaper)（AGPL-3.0，Go） | **ECH 注入** + **TLS 分片** + **QUIC 转换** + 会话迁移 + TUN；带 Cloudflare IP 优选池、NAT64、"进化模式"（自动试多种规则组合挑最优） | 正是 SNIBypassGUI 许可证里点名的竞品，但它是**开源可读**的；它的多模式清单可以当作我们的候选清单 |
| [mirage](https://github.com/PoriyaVali/mirage) | 把一个 ClientHello 拆成两个 TLS record，第一段 1/2/4/5 字节有效，3 字节反而被丢。结论来自 680 次实测 | 分片是**不依赖 ECH** 的一条路，几十行代码，可以直接插在 nginx 和上游之间试 |
| [byedpi](https://github.com/hufrea/byedpi) / [GreenTunnel](https://github.com/SadeghHayeri/GreenTunnel) | 同类 DPI 规避（分片、乱序） | 同上，成熟参考实现 |
| [《使用 AdGuard 作为 DoH/DoT…通过 ECH 突破 SNI 封锁》](https://mhdy2233.github.io/archives/shi-yong-adguardzuo-wei-doh-dotlai-dui-kang-dnswu-ran-yi-ji-tong-guo-echtu-po-snifeng-suo/) | **不写客户端**：自建 DoH（AdGuard Home）+ 把 A 记录重写到 CF IP + 合成 HTTPS/ECH 记录，让**浏览器原生 ECH** 生效 | 关键结论：*"只要托管于 Cloudflare 的网站都能通过 ECH 突破，即使网站没开 ECH"*；明确点名 pixiv 适用，且 pixiv 的 IPv6 会返回 `403 block_ip`，必须过滤 AAAA |

**改变了什么**：ECH 不一定要自己实现客户端。浏览器已经支持，我们只需要给它
"指向 CF 的 A 记录 + 带 ech= 的 SVCB 记录 + 一个它信任的 DoH 端点"。
这比写 Go/Rust 网关轻得多。

验证 ECH 是否生效：访问 `https://crypto.cloudflare.com/cdn-cgi/trace`，
输出里 `sni=encrypted` 即为成功。

---

## 七、方法论教训（不是链接，但同样重要）

1. **探测必须复刻使用方式。** 按浏览器方式探测，会把一整套可用地址判死（`probe` 的
   默认模式就是这么改过来的）。
2. **所有 TLS 判断都要用 openssl 交叉验证。** Windows 的 Schannel 对部分 Cloudflare 边缘
   直接失败（curl 报 `000`），而 `openssl s_client` 同一地址握手成功。我们差点把
   "Schannel 不行"记成"被墙"。
3. **状态码不是结论。** Fastly 曾对不存在的 Host 返回 200（100 字节的未知域名页）；
   pixiv 主站对旧源站返回 301 然后 403。`verify` 因此对 3xx 多跟一跳才下判断。
