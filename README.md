# sniroute

本地 SNI 反向代理：把被解析污染的域名接在本机，由 nginx 用自己的证书终止 TLS，
再换一条路径连到真实站点。

思路借自 [Pixiv-Nginx](https://github.com/mashirozx/Pixiv-Nginx)（WTFPL），但**没有**复制它的
配置结构 —— 那是 2018 年的形状，22 个 conf 文件里有一半从没被引用过，改它等于在负资产上
加负资产。这里只保留三个仍然成立的想法，其余重写：

1. **把域名接回本机**（它用 hosts 写死，这里保留 hosts，并说明为什么 v0 够用）
2. **本地终止 TLS**（所以需要一张自己的证书；这里改用本机生成的窄证书）
3. **向上游隐藏 SNI**（`proxy_ssl_server_name off`，但做成可开关的，因为这台机器上
   到底哪种能通，得跑出来才知道）

至于 [SNIBypassGUI](https://github.com/racpast/SNIBypassGUI) 那样的成品，它的 IP 数据受专有
许可保护（第 6.4/6.5 条禁止提取与转格式），本项目一个字节都没从那里拿 —— 所有上游地址
都由 `probe` 自己测出来。

---

## 它解决了什么，没解决什么

**解决：DNS 污染。** 域名被指到本机，绕开污染解析，自己决定连哪个 IP。
**解决：写死 IP 会烂。** 上游地址是脚本测出来的，不是抄在配置里然后等人来修。
**解决：证书范围失控。** Pixiv-Nginx 用一张证书盖了 24 个域名（我实测 SAN 443 字节，
含 pixiv / wikipedia / google / steam / AO3），而这个项目为每个站点单独签一张窄证书。

**没解决：SNI 阻断。** 向上游隐藏 SNI 是一半的答案 —— 如果前端要求 SNI 与 Host 一致，
隐藏就会换来 421 或握手失败。配置里留了开关，两种都得实测（结果见下节）。
**没解决：ECH。** hosts 文件只管 A/AAAA 记录，管不了 SVCB/HTTPS 记录，浏览器照样可能
拿到 ECH 配置。要彻底压下这条，得有一个能干预 DNS 应答的本地解析器 —— 那是 v1 的事，
`data/sites.json` 与 `probe` 已经为此留好了位置。

---

## 已验证的可用配置（2026-11-19）

这一套在本机跑通了，实测证据：DoH 收到 469 次查询（`www.pixiv.net` 7、`i.pximg.net` 14、
`s.pximg.net` 15），nginx 访问日志里 `i.pximg.net` 连续返回 200 且带真实字节数（最大一张
781 KB）。主站 trace 页显示 `sni=encrypted`。

### 三个组件，谁都不能少

```
浏览器 ──DNS──> 本机 DoH (127.0.0.1:5444) ──┬─ pixiv.net   → Cloudflare 边缘 + ECH 配置
                     ↑                      │              （浏览器自己加密 SNI）
                     │                      └─ pximg.net   → 127.0.0.1
              hosts: doh.sniroute.lan                          ↓
                     ↓                                    本机 nginx (127.0.0.1:443)
              本机根证书（CA）                           向上游「不发 SNI」
```

| 组件 | 作用 | 起法 |
|---|---|---|
| **DoH** | 给浏览器发"正确的 IP"和"ECH 配置" | `scripts\doh-ech.cmd` |
| **nginx** | 接住图片站，用它不发 SNI 的能力 | `python scripts\sniroute.py service up` |
| **浏览器设置** | 安全 DNS 指向本机 DoH | 设置里填一次 |

### 最关键的一条：两种站点必须分开处理

| 站点类型 | DNS 应答 | 为什么 |
|---|---|---|
| 在 **Cloudflare** 上（pixiv 主站） | CF 边缘 IP + **带 `ech=` 的 HTTPS 记录** | 只有 ECH 能躲过按 SNI 值的切断 |
| 在**自家机房**（`i.pximg.net`） | **127.0.0.1**，**绝不能带 ech** | 它不支持 ECH，带上去就是握手失败 → 图片全挂 |

给不支持 ECH 的站点下发 ECH，症状是"页面能开但图全裂"——这个坑我们真踩了。
对应实现是 `data/sites.json` 里的 `dns_mode`：`"ech"` / `"loopback"`。
探活全部失败时，dns 模式现在回退为返回上游原始答案，而不是回空应答。早前把"探活失败"
当成"域名不存在"处理，曾导致 github.com 整站解析失败（浏览器 ERR_NAME_NOT_RESOLVED）。

### 两个容易反复踩的坑

1. **安全 DNS 是按 Chrome 配置（profile）存的。** 日常 profile 和 `--user-data-dir`
   独立实例是两份设置，填一边不会影响另一边。
2. **Chrome 157 忽略命令行的 DoH 参数**（`--dns-over-https-templates` 实测有效查询 0 条），
   也忽略手写进 profile 的 `dns_over_https` 偏好。**只有设置界面（或策略）生效**，
   而本机的 `HKCU\Software\Policies` 被加固（属主 SYSTEM、用户仅 ReadKey），策略写不进去。

### 已知不足

- **大图慢，且不是配置能解决的**。`sniroute.cmd stats` 的真实分布：

  | 响应大小 | 数量 | 中位 | p95 | 最大 |
  |---|---|---|---|---|
  | >500 KB | 7 | **17.07s** | 45.76s | 45.76s |
  | 100–500 KB | 9 | 8.59s | 13.61s | 13.61s |
  | 10–100 KB | 50 | 0.56s | 2.18s | 3.99s |
  | <10 KB | 193 | 0.31s | 2.59s | 24.51s |

  耗时与响应大小强相关 → 瓶颈是跨境吞吐，不是 nginx。已做的两项针对性调整：

  1. `conf/incl/proxy-params.conf`：`proxy_buffering off` → **on**（配 `proxy_buffers`），
     把上游读取与客户端发送解耦；`proxy_read_timeout` 60s → **180s**，避免长尾被误判超时；
     加 `proxy_socket_keepalive on`。
  2. `probe` 现在**剔除明显慢的离群地址**（超过中位 2.5 倍且另有 3 个以上快地址时），
     因为 nginx 默认轮询会把 1/N 请求分给最慢那个 —— 这是小请求偶发数秒延迟的来源。

  验证结果：小资源中位 0.255 → 0.251s，最大 0.307 → 0.313s（无回归，属噪声范围）。
  大文件是否改善需要真实浏览后对比，用 `sniroute.cmd stats` 看"按响应大小"那一段。

- 依赖三件套同时在线；DoH 一停，该浏览器配置下所有域名都解析不了（`secure` 模式的固有行为）。
- `logs/access.log` 里 `www.pixiv.net` 的失败记录是 ECH 打通**之前**的：现在主站走浏览器
  直连 Cloudflare，不再经过本机 nginx，所以 `verify --site pixiv` 对它没有意义。

---

## 实测记录（2026-11-18，Windows 11 + nginx 1.31.8）

这不是设计推演，是在一台真实机器上跑出来的。**结论比预期更具体：图片站通，主站不通。**

### 1. 上游对 SNI 的态度（22 个候选地址，逐个测）

| 探测方式 | 结果 |
|---|---|
| 带真实 SNI（`server_hostname=域名`） | **22/22 全部失败**，0.1 秒内被重置 |
| 不发 SNI（`server_hostname=None`） | **22/22 全部成功**，证书链有效，`CN=pixiv.net`，签发者 Google Trust Services WR1 |

延迟区间 140ms – 3.4s，按 ms 排序写进了 `conf/upstreams/*.list`。

这条差异就是整个方案成立的前提，也是我第一次写 probe 时踩的坑：**按浏览器的方式探测，
会把所有能用的地址判成死的**。所以 `probe` 现在默认先按代理的方式（不发 SNI）测。

### 2. 经代理的实际结果

| 请求 | 结果 | 说明 |
|---|---|---|
| `https://i.pximg.net/` | **200** | 图片站端到端可用 |
| `https://i.pximg.net/robots.txt` | **200 / 75B / text/plain** | 拿到上游真实文件（`User-agent: Googlebot-Image` …） |
| `https://i.pximg.net/img/` | 404 | 上游正常回"路径不存在"，说明 vhost 认了这个 Host |
| `https://i.pximg.net/favicon.ico` | 403 | 资源级 ACL，不是 vhost 拒绝 |
| `https://pximg.net/` | 200 / 110B | pixiv 给 apex 域名的占位页（任何路径都一样） |
| `https://pixiv.net/` | 301 → `https://www.pixiv.net/` | 上游默认 vhost 的跳转 |
| `https://www.pixiv.net/` | **403** | 该 vhost 在无 SNI 时拒绝服务 |

### 3. 这三行说明什么

> **本节的结论已被第 5 节更正。** 当时的判断是"主站要 SNI、而 SNI 被墙，所以无解"。
> 后续发现真实原因不同：主站根本不在那批源站 IP 上，而在 Cloudflare。保留本节是为了
> 留下判断是怎样一步步被推翻的 —— 这比一个干净但来路不明的结论有用。

pixiv 的源站在无 SNI 时只服务 `pixiv.net` 这个默认 vhost：它对任何路径都回 301 指向
`www.pixiv.net`，而对 `Host: www.pixiv.net` 直接 403。于是：

- **图片站**（`i.pximg.net`）的 vhost 接受无 SNI 的请求 → 通
- **主站**（`www.pixiv.net`）必须让源站按 SNI 选中 vhost → 而任何 SNI 都被重置 → 不通

也就是说，"隐藏 SNI"这一招对**按 Host 路由**的站点有效，对**按 SNI 路由**的站点无效。
这不是配置错误，改 `proxy_ssl_server_name on` 只会立刻被阻断。

### 4. 主站还有什么路可走（已试过，都不通）

顺着"找一条按 Host 路由的边缘"的思路实测了一轮：

| 目标 | TCP | 不带 SNI + `Host: www.pixiv.net` | 结论 |
|---|---|---|---|
| Cloudflare `104.16.0.1` / `104.18.32.1` / `172.64.0.1` / `162.159.0.1` | 通 | 000（TLS 失败） | 不可用 |
| Cloudflare `104.21.0.1` | 通 | 000 | 不可用 |
| Cloudflare `172.67.0.1` / `2.16.168.1` | **不通** | — | 不可达 |
| Fastly `151.101.1.1` | 通 | **200，但只有 100 字节 `text/plain`**，`Via: 1.1 varnish` | 是 Fastly 自己的"未知域名"应答，不是 pixiv 内容 |
| Fastly `199.232.69.194` | 通 | 000 | 不可用 |
| Akamai `23.215.0.136` | 通 | 000 | 不可用 |
| 域前置：SNI=`cloudflare.com` + `Host: www.pixiv.net` | 通 | 403 | Cloudflare 已封禁 SNI/Host 不一致 |

另外确认了上游的 vhost 行为（同一 IP，不发 SNI，换 Host）：

| Host | 状态 | 含义 |
|---|---|---|
| `pixiv.net` | 301 | 默认 vhost，跳 www |
| `www.pixiv.net` | 403 | 明确拒绝无 SNI 请求 |
| `i.pximg.net` | 421 | 这是网页集群，不是图片集群 |
| `api.pixiv.net` / `accounts.pixiv.net` | 421 / 403 | 同上 |

换浏览器 UA 不改变结果（403 是 nginx 自己的错误页，不是 UA 过滤）。

**结论：主站从当前网络没有可用的入口。** 剩下能想到的路：

1. **找到一个真正按 Host 路由、且本机可达的边缘** —— 目前试过的 CF/Fastly/Akamai 都不是。
2. **v1 的 DNS 抑制 ECH** —— 能解决 ECH，但解决不了"源站拒绝无 SNI 请求"这件事。
3. **接受现状**：本代理只负责图片这类按 Host 路由的静态资源，主站走别的通道。
   图片往往是流量大头，先解决它不是妥协 —— 所以 `data/sites.json` 里 **pixiv 主站
   已标记 `enabled: false`，`hosts install` 不会拦它的域名**。把打不通的域名指到本机，
   只会把用户从"慢"推进"完全打不开"。

---


---

### 5. 更正：主站不在那批 IP 上，而在 Cloudflare

第 3、4 节的结论错了，原因很蠢也很典型：**我们一直在打 pixiv 2018 年的老源站。**

用国内可达的 DoH（`doh.pub`）查真实解析：

| 查询 | 结果 |
|---|---|
| `doh.pub` → `www.pixiv.net` A | **104.18.42.239 / 172.64.145.17** ← Cloudflare anycast |
| `dns.alidns.com` → 同一条 | 168.143.162.58 ← 污染答案 |
| `210.140.139.151`（旧配置里的地址） | pixiv 的**老源站**，仍然活着，但已不是对外入口 |

所以之前那两个现象（无 SNI → 301/403、带 SNI → 被重置）解释的是"旧源站不再服务普通请求"，
而不是"这条路对主站无效"。

**进而对 CF 边缘做了三组测试（openssl，不是 curl）：**

| 测试 | 结果 |
|---|---|
| TCP 到 104.18.42.239 等 8 个 CF 边缘 | 全部可达 |
| `SNI=cloudflare.com` → CF 边缘 | 握手成功 |
| `SNI=www.pixiv.net` → CF 边缘（首轮） | **握手成功，证书校验通过（code 0）** |
| `SNI=www.pixiv.net` → 同一批 IP（几分钟后） | **全部被重置**，而 `SNI=cloudflare.com` 依然成功 |

也就是说：**阻断是按 SNI 值、而且是行为式的** —— 前几次握手能过，之后这个 SNI 就被封了。
这与文献里描述的行为式 ClientHello 评分一致（见 `docs/references.md` 里的 M3 论文）。

**这正好解释了 SNIBassGUI 的设计**：它把 `.pixiv.net` 放进 `type = "ech"` 的 route，
配 `require_ech = true`。因为只有把 SNI 加密（ECH），这个值才根本不会出现在可被观察的位置。

**而我们做不到这一点**：

- nginx 没有 ECH 客户端能力（没有 `proxy_ssl_ech` 一类指令），1.31.8 亦无；
- Windows 自带 curl 8.13 是 Schannel 后端，`--ech` 直接报 "the installed libcurl version
  does not support this"。

结论：主站要在 nginx 之外再加一个支持 ECH 的网关（自己的 Go/Rust 组件），否则只能保持
`enabled: false`。当前就是这个状态。

**顺带记一个方法论教训**：Windows 的 Schannel 对某些 CF 边缘会直接失败（`curl` 报 `000`），
而同一地址用 `openssl s_client` 却握手成功。**所以所有 TLS 判断都必须用 openssl 交叉验证，
不能只信 curl**——我们差点因此把"Schannel 不行"记成"被墙"。

---

### 6. 三条后续实验的结果（2026-11-18 夜）

调研完别人的五种打法后，把不额外依赖的三条挨个试了。

#### 实验一：TLS 分片 —— **无效**

按 [mirage](https://github.com/PoriyaVali/mirage) 的做法写了中继（[tools/frag_relay.py](tools/frag_relay.py)）：
把 ClientHello 拆成两个 TLS record，两个 record 放在同一次 TCP 写里。

| 配置 | 结果 |
|---|---|
| 不分片（对照组） | 失败 |
| 第一段 1 字节 | 失败 |
| 第一段 2 字节 | 失败 |
| 第一段 4 字节 | 失败 |
| 第一段 5 字节 | 失败 |

**对照组证明中继本身是对的**：同一个中继、同样的分片参数，`SNI=cloudflare.com` 握手成功。
所以结论是"分片对当前这套阻断无效"，不是"工具坏了"。另外换过 8 个全新的 CF 边缘 IP 直连，
`SNI=www.pixiv.net` 全部失败 —— 不是"某些 IP 被烧了"，是这个 SNI 值现在过不去。

#### 实验二：QUIC 上游 —— **无法测试**

QUIC 的 Initial 包是加密的，SNI 不在明文里，理论上最干净。但本机装不上 `aioquic`
（无 PyPI 访问），Windows 与 Git 自带的 curl 都不带 h3。需要 Go 或 Rust 工具链才能验证。

#### 实验三：DoH + 浏览器原生 ECH —— **服务端完成并验证通过**

关键认知：**不需要自己实现 ECH**，浏览器已经实现了；我们只要供给它
"A 记录 + 空 AAAA + 带 ech= 的 HTTPS 记录"。

[tools/doh_ech.py](tools/doh_ech.py) 的实测输出：

```
type=A     → 6 条，全部是 Cloudflare 边缘（104.18.42.239 …）
type=AAAA  → 空应答（NOERROR）
type=HTTPS → priority=1 alpn=h2
             ipv4hint=104.18.42.239,172.64.145.17,…
             ech=AEX+DQBB1wAgACCfWWcLbPp+38JDF…（71 字节）
example.com→ 转发给 doh.pub，正常返回
```

TLS 层用本项目 CA 签了 `doh.sniroute.lan` + `IP:127.0.0.1` 双 SAN，`openssl verify` 通过，
`tools/doh_selftest.py --cafile ca/rootCA.crt` 端到端成功。ECH 配置的来源也解决了：
用 `dns.alidns.com` 查 `cloudflare-ech.com` 的 type=65 即可拿到
（见 [data/cloudflare-ech.json](data/cloudflare-ech.json)），**不需要直连 Cloudflare**。

**剩下的步骤需要管理员权限，只能由使用者来做**：

1. `python scripts/sniroute.py install-ca` —— 把本机根证书装进受信任根
2. hosts 加 `127.0.0.1 doh.sniroute.lan`
3. Chrome/Edge：`chrome://flags` 开启 `Encrypted ClientHello`
4. 设置 → 隐私和安全 → 安全 → 使用安全 DNS → 自定义 → `https://doh.sniroute.lan:5444/dns-query`
5. 验证：访问 `https://crypto.cloudflare.com/cdn-cgi/trace`，输出里出现 `sni=encrypted`

### 自动化：两条路，一条被本机挡住了

**注册表策略（本机不可用）**。[scripts/setup-browser-ech.ps1](scripts/setup-browser-ech.ps1) 用
`DnsOverHttpsMode` / `DnsOverHttpsTemplates` / `EncryptedClientHelloEnabled` 三个策略代替点
flags（三个名字已在 Chrome 157 的 `chrome.dll` 里核对存在）。但本机实测写不进去：

```
HKCU\Software\SnirouteTest           -> 可写
HKCU\Software\Policies\SnirouteTest  -> 拒绝（属主 NT AUTHORITY\SYSTEM，当前账号仅 ReadKey）
```

策略键被加固过。**改它的 ACL 属于动安全配置，本项目不这么做**，所以这条路留作参考 ——
在策略未被加固的机器上可直接用（脚本支持 `-Browsers Chrome|Edge|Both` 与 `-Revert`）。

**启动参数（本机可用，零权限）**。[scripts/start-chrome-ech.ps1](scripts/start-chrome-ech.ps1) 用一个
**独立的 Chrome 实例**带齐参数，不动系统任何设置：

```powershell
scripts\doh-ech.cmd                 # 终端 A：起 DoH（必须一直开着），默认 5444
scripts\start-chrome-ech.cmd        # 终端 B：拉起带 ECH 的独立 Chrome 实例
scripts\start-chrome-ech.cmd -DryRun        # 只看命令，不启动
```

`.cmd` 包装器解决了两件事：一是从任意目录调用都不会因为相对路径失败（`%~dp0`），
二是自动找可用的 Python（`py -3` → `python` → DSH 自带运行时）。
**它们只用 ASCII 注释**——cmd.exe 按 ANSI 代码页读 `.cmd`，中文注释会被当成命令执行。

关键是 `--ignore-certificate-errors-spki-list=<SPKI>`：它按**公钥指纹**放行这一张 DoH 证书，
而不是像装根 CA 那样信任一大片 —— 所以**不需要管理员，也没有扩大信任面**。
再配 `--host-resolver-rules` 让 DoH 域名指向本机，连 hosts 都不用改。退出实例即恢复。

> 这一步会让浏览器把 `pixiv.net` 解析到 Cloudflare 并启用 ECH，**完全绕过本项目的 nginx**。
> 所以 `data/sites.json` 里 pixiv 是 `enabled: false` + `dns_override: true`：前者表示 nginx
> 不代理它，后者表示 DNS 层要接管。两个开关分开，是因为它们是两件事。

---

## 为什么每个站点要一张证书

这不是洁癖，是功能需要。

浏览器在「同一 IP、同一张证书对两个域名都有效」的情况下，会把 HTTP/2 连接**合并复用**。
于是上游收到「连接 SNI 是 `pixiv.net`、`:authority` 是 `i.pximg.net`」这种请求，Cloudflare
一类的前端直接回 **421 Misdirected Request**。

Pixiv-Nginx 那张「一张证书盖 24 个域名」的设计，恰好就是给浏览器发的合并通行证。
所以 `conf/certs/pixiv.crt` 的 SAN 里**只有** `pixiv.net` / `*.pixiv.net`，
`conf/certs/pximg.crt` 里**只有** `pximg.net` / `*.pximg.net`。浏览器没有合并的理由。

> 路径细节：证书必须放在 `conf/` 下。nginx 把 `ssl_certificate certs/x.crt` 解析到
> `conf/`，而把 `error_log logs/x.log` 解析到仓库根 —— 两条规则不一样，放错位置报的是
> `[emerg] BIO_new_file() failed`，而不是"文件找不到"。

---

## 前置条件

- Windows（脚本用了 `certutil`、`ipconfig`、`tasklist`）
- **nginx for Windows ≥ 1.25.1**：<https://nginx.org/en/download.html>
  （`http2 on;` 这种写法从 1.25.1 起才有；官方 Windows 构建自带 `ssl_preread` 等模块）
  把路径写进仓库根的 `nginx.path`，或每次用 `--nginx` 指定
- Python 3.10+，**只用标准库**，不需要 pip 装任何东西
- openssl：装 Git for Windows 就有了（脚本会自动去 `C:\Program Files\Git\usr\bin` 找）
- 改 hosts、装根证书、监听 443 都需要**管理员权限**

---

## 上手：只有一个入口

仓库根的 `sniroute.cmd` 是唯一入口，**从任何目录都能调用**（它自己解析仓库位置、
自己找可用的 Python）：

```powershell
sniroute.cmd up       # 拉起 DoH + nginx 并做健康检查
sniroute.cmd status   # 组件、站点、最近查询
sniroute.cmd down     # 全停
```

不带参数运行会打印所有可用子命令。

### 首次使用：三件事，只需各做一次

```powershell
# ① 证书（本机自签，私钥留在 ca\，不入库）
sniroute.cmd gen-ca
sniroute.cmd gen-cert

# ② 装根证书 + 写 hosts（需要管理员：用「以管理员身份运行」的 PowerShell）
powershell -ExecutionPolicy Bypass -File scripts\setup-browser-ech.ps1 -Elevated

# ③ 浏览器里填一次安全 DNS
#    Chrome > 设置 > 隐私和安全 > 安全 > 使用安全 DNS > 自定义
#    https://doh.sniroute.lan:5444/dns-query
```

之后每次只要 `sniroute.cmd up`。

### 图形界面（可选）

不想记命令就用界面。它和命令行**共用同一套实现**（界面只调 `sniroute.cmd`，不重复写任何
逻辑），所以两边行为不会分叉。

```powershell
scripts\build-gui.cmd        # 只需一次：编译 Java 界面
scripts\sniroute-gui.cmd     # 打开窗口
```

窗口里：四个状态灯（DoH / nginx / hosts / 根证书，每 3 秒自刷）、六个按钮（启动全部 /
停止全部 / 刷新 / 安装证书hosts / 打开浏览器 / 还原）、站点表、以及可滚动的输出框。

### 托盘图标（唯一形态：没有窗口）

Java 版是一个**纯托盘程序**：启动后不出现任何窗口，只在任务栏通知区域（时钟旁边）放一个
圆点。所有操作都在右键菜单里。

**首次启动会自己完成安装**：检测到根证书不在信任库、或 hosts 没有条目时，弹一次确认 →
自动提权装好 → 之后不再询问（选「取消」会记下来，右键菜单仍可手动执行）。这样便携包发给
别人时，对方双击一次、点一次 UAC 就能用，不必先去读文档。

| 菜单项 | 作用 |
|---|---|
| 启动全部 / 停止全部 | 等价于 `sniroute.cmd up` / `down` |
| 刷新状态 | 气泡显示 DoH / nginx / hosts / 证书 四项状态（左键双击图标同效） |
| 检测 NAT 类型 | 弹窗显示本机 NAT 类型（`sniroute.cmd nat --summary`） |
| 刷新上游地址 | 重新检查 loopback 站点的上游候选地址（`sniroute.cmd refresh`） |
| 打开浏览器 | 用独立配置启动带 ECH 的 Chrome |
| 安装证书/hosts | 弹 UAC 做一次性安装（首次启动已自动做过的话无需再点） |
| 查看日志 | 用记事本打开 `logs\gui.log` |
| 开机自动启动 | 写入 `HKCU\...\CurrentVersion\Run`（**不需要管理员**） |
| 退出 | 结束进程 |

| 图标 | 含义 |
|---|---|
| 绿点 | DoH 与 nginx 都在跑 |
| 红点 | 有服务没起 |
| 悬停 | `sniroute  DoH:up  nginx:up` |

**气泡文案是"说结果 + 说下一步"，不是抛退出码。** 例如启动成功是
「已启动。DoH 与 nginx 都在运行，可以直接访问目标站点了」，安装成功是
「安装完成。接下来把 Chrome 的「使用安全 DNS」设为 …」，失败才在括号里带上退出码并指向
「查看日志」。措辞全部放在 `data/gui-strings.json` 的 `notify_*` 键里，**改文案不用重新编译**。

没有窗口之后，三件事取代了原来界面上的位置：

- **结果用气泡通知**回报；
- **完整输出写进 `logs\gui.log`**，菜单里一键打开；
- **状态靠图标颜色与悬停提示**，每 3 秒自刷。

代码里只剩一处 Swing：首次安装与「还原」前的**确认对话框**。静默装根证书、静默删根证书都
比多一个弹窗更糟，所以保留了它——但它不是应用窗口。

```powershell
scripts\sniroute-gui.cmd     # 唯一启动方式
```

开机自启注册的命令：便携包里是 `Sniroute.exe`，开发目录里是
`javaw -cp <classes> SnirouteGui`。

> PowerShell 兜底版（`scripts\sniroute-gui.ps1`，没装 JDK 的机器用）仍然是窗口形态，
> 它没有托盘能力。便携包自带 Java，不会用到它。



两个前端，行为一致：

| 前端 | 文件 | 依赖 |
|---|---|---|
| **Java / Swing**（默认） | `gui\SnirouteGui.java` | 需 JDK 11+，`build-gui.cmd` 编译一次 |
| PowerShell WinForms（兜底） | `scripts\sniroute-gui.ps1` | 无依赖，Windows 自带 |

`sniroute-gui.cmd` 优先用 Java；没编译过就自动退回 PowerShell 版。

**为什么源码都是纯 ASCII**：界面文案统一放在 `data/gui-strings.json`，由两者各自以 UTF-8
显式读取。Windows PowerShell 5.1 会按 ANSI 解码没有 BOM 的 `.ps1`，中文会吞引号导致整份
脚本解析失败 —— 这一点真踩过。把文案抽出去之后，脚本与 Java 源码都能保持纯 ASCII，
不再依赖任何编码猜测。

### 打包成便携版（双击 EXE 就能跑）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-package.ps1
# → dist\sniroute-portable-win64.zip   （约 62 MB）
```

解压后是一个自包含目录，双击 `Sniroute.exe` 就开窗口，**不需要安装 Java、Python 或 nginx**：

```
Sniroute\
  Sniroute.exe          双击入口（jpackage app-image）
  runtime\              自带 Java 运行时
  app\sniroute.jar      GUI
  python\               裁剪后的 CPython 3.12（37 MB，PSF 许可）
  nginx\nginx.exe
  openssl\mingw64\...   openssl 3.2.1 + 必要 DLL/配置（签本机证书用）
  scripts\ tools\ conf\ data\
  sniroute.cmd          命令行入口，包里也能用
  README.txt
```

三处为"可携带"做的适配：

| 适配 | 原因 |
|---|---|
| `find_openssl` / `find_nginx` 先找包内路径 | 不依赖系统里恰好装了什么 |
| `sniroute.cmd` 优先用 `python\python.exe` | 便携包自带解释器 |
| `up` 发现证书缺失时**自动签发** | 证书每台机器各自签，不能随包分发；否则用户点"启动"只会看到"请先跑 gen-cert" |

打包脚本里每一步都带验证：编译后检查 `Sniroute.exe` 存在、裁剪后的 Python 能 `import ssl,socketserver,http.server...`、
包内 openssl 能真的签出证书 —— 任何一步失败就中止，不会产出半成品 zip。

> 注意：包内 openssl 必须是 Git 的 `mingw64` **目录布局**（含 `lib\ossl-modules`、`etc\ssl\openssl.cnf`、
> `zlib1.dll`）。OpenSSL 3 按 exe 自身路径推算 prefix，只复制 `openssl.exe` 会在签证书时报
> "No store loader found"。这一条是在打包测试里踩出来的。

### 打包：MSI 安装包 与 绿色版 ZIP

托盘程序是**原生 C++（Win32）**，不依赖 Java：

```powershell
# 只编译托盘程序（zig c++，免安装工具链）
powershell -ExecutionPolicy Bypass -File scripts\build-tray.ps1
# → build\tray\Sniroute.exe   （约 480 KB）

# MSI：按用户安装，无 UAC，可自选目录（WiX 直接打包）
powershell -ExecutionPolicy Bypass -File scripts\build-msi-wix.ps1
# → dist\Sniroute-2.0.0.msi   （17 MB）

# 绿色版 ZIP
powershell -ExecutionPolicy Bypass -File scripts\build-zip.ps1
# → dist\sniroute-portable-win64.zip  （16.8 MB）
```

编译用 `zig c++`：Zig 自带 clang、MinGW 头文件和 Win32 库，`pip install ziglang`
即可拿到，**不需要装 Visual Studio，也不需要管理员权限**（`build\zig\zig.exe`）。

为什么从 Java 换成 C++：Java 版的原生托盘菜单不做 CJK 字体回退，中文全变成方块；
改用 Swing 弹菜单又无法可靠地定位到通知区域旁边。Win32 的 `TrackPopupMenu` 两个问题
都不存在，而且产物从 40 MB 的运行时降到 480 KB 的单文件。

| | MSI | 绿色版 ZIP |
|---|---|---|
| 安装 | 双击，**不弹 UAC**，可选安装目录 | 解压即用 |
| 卸载 | 设置 → 应用 → 卸载 | 删文件夹 |
| 快捷方式 | 桌面 + 开始菜单 | 无（双击 exe） |
| 体积 | 64.7 MB | 62.5 MB |

**为什么能做到不弹 UAC**（三条一起才成立）：

1. `jpackage --win-per-user-install` —— 只装到当前用户，Windows 不要求提权；
2. 根证书装进**当前用户**的受信任根（`certutil -user -addstore Root`）——
   Chrome / Edge 同样校验用户存储，所以够用；
3. DoH 端点用 IP 形式 `https://127.0.0.1:5444/dns-query` —— 不需要改系统 hosts，
   因此不碰受保护文件。DoH 证书同时带 `DNS:doh.sniroute.lan` 与 `IP:127.0.0.1`
   两个 SAN，两种寻址方式都能用（之前缺 IP SAN 时浏览器会以"证书与地址不匹配"整段拒绝）。

安装完成后首次启动会**自动**装证书：不弹窗、不提权，一次到位。

**WiX 依赖**：jpackage 打 MSI 需要 WiX 3.x 的 `candle.exe` / `light.exe` 在 PATH 上。
WiX 官方安装器要求管理员权限（还要 .NET 3.5），所以本项目用它的免安装二进制包：
把 `wix314-binaries.zip` 解压到 `build\wix\` 即可（`build-msi.ps1` 会自动把它加进 PATH）。

**验证过的安装行为**（本机实测）：

```
msiexec /i Sniroute-1.0.0.msi /qn INSTALLDIR="..."   → 退出码 0，无提权
安装后目录                                           → Sniroute.exe / python\ / nginx\ / openssl\ / gui\icon.png
Sniroute.exe --smoke                                 → tray = true, window = false
msiexec /x                                           → 卸载干净，目录不残留
非管理员执行 setup-browser-ech.ps1                   → 证书进 CurrentUser\Root，LocalMachine\Root 未被触碰
```

### 其余子命令（按需）

| 命令 | 用途 |
|---|---|
| `verify` | 经代理发真实请求，判断每个站点到底能不能用 |
| `stats` | 从访问日志统计真实表现（按域名 / 按响应大小，用来看调参有没有效果） |
| `probe` | 实测上游 IP，写出 `conf/upstreams/*.list`（会自动剔除慢地址） |
| `discover` | 找候选 IP（DoH 查询 / 同网段扩展） |
| `doh` | 单独启停 DoH（`start` / `stop` / `status`） |
| `hosts` | 增删 hosts 区块（`install` / `remove` / `show`） |
| `service` | 单独控制 nginx（`up` / `down` / `reload` / `status` / `test`） |
| `check` | 环境自检 |
| `relay` | 起停中继（`status` / `start` / `stop`；默认关闭，需墙外服务器） |
| `nat` | 检测 NAT 类型（RFC 5780；`--summary` / `--json` / `--server` / `--server2` / `--timeout-ms`） |

### 还原

```powershell
sniroute.cmd down
sniroute.cmd hosts remove                     # 撤掉站点 hosts 区块
powershell -ExecutionPolicy Bypass -File scripts\setup-browser-ech.ps1 -Revert -Elevated
# 浏览器里把安全 DNS 改回「关闭」
```

---

## probe 在做什么（以及为什么不能省）

它**按代理真正会用的方式**握手，而不是按浏览器的方式 —— 这两者的差别大到能得出相反结论：

1. 先**不发 SNI**（`server_hostname=None`）。这就是 nginx 在 `proxy_ssl_server_name off`
   时的行为。握手成功说明这条链路真的能用；随后自己比对对方证书的 SAN/CN 是否覆盖目标
   域名（不发 SNI 时 SSLContext 不会替我们查主机名）。
2. 只有第 1 步失败，才**发真实 SNI** 再试一次，用来发现"这里没被阻断"的地址。

每个结果都带 `mode`（SNI / 无SNI）与 `identity`（`verified` / `selfsigned-match` /
`mismatch`），排序时把身份可信的放前面。所以"活着"不等于"可信任"，两者分开标注。

这比「443 端口开着」强得多：一个开着 443 但不是目标站点的地址会在这里失败，而不是被写进
上游列表。**一条都不通时，它不会覆盖现有列表** —— IP 全失效和本地断网看起来一样，
把一份还能用的列表清空是最糟的自动化行为。

顺带一个反例，说明为什么这一步不能省：最初这版只按浏览器方式探测，结果 22 个地址全部
报"被重置"，而同一批地址经代理却能正常回包。工具告诉你的结论，取决于它模仿的是谁。

---

## verify 在做什么

`probe` 证明"上游能握手"，`verify` 证明"端到端能用"——两者不是一回事。它经本机代理发一个
真实 HTTPS 请求，并用 `ca/rootCA.crt` 校验本地那张证书，因此同时验证了：代理在监听、
它为该域名出示的是我们自己的证书、上游确实回了内容。

结论按 HTTP 状态分成四种，对应四种完全不同的原因，别混着看：

| 结论 | 含义 | 该怎么办 |
|---|---|---|
| 可用 | 200 / 204 | 正常 |
| 跳转后失败 | 3xx 但跟一跳后到不了内容 | 看括号里的终点结论，通常就是"被拒" |
| **被拒**（403） | 源站要求 SNI 选中 vhost，而我们没发 SNI | 这条路对该站不适用，设 `enabled: false` |
| **错向**（421） | 这个地址不服务这个域名（集群用错了） | probe 选错了上游，检查 `upstreams/*.list` |
| 失败 | 本地代理没跑 / 证书不对 / 连接不通 | 先看 `service status` 与 `logs/error.log` |

`verify --site pixiv` 可以验停用的站点 —— 那正是想知道"它现在通了没有"的时刻。它对 3xx
会**多跟一跳**：pixiv 主站是 301 到 `www.pixiv.net` 再 403，如果在 301 处停下，结论会被
写成"可用"，把人送进一个打不开的页面。跳一次，才看得到终点。

---

## 目录

```
sniroute/
├─ sniroute.cmd             ★ 唯一入口：up / down / status / 其余子命令
├─ conf/
│  ├─ nginx.conf            主配置：日志格式、80→443（可选）、include 站点
│  ├─ incl/proxy-params.conf 所有站点共用的上游请求参数
│  ├─ sites/pixiv.conf      站点定义：证书、上游、SNI 开关
│  ├─ sites/pximg.conf
│  └─ upstreams/*.list      上游地址（probe 生成）
├─ data/
│  ├─ sites.json            站点清单：域名、SAN、dns_mode —— 唯一的事实来源
│  └─ cloudflare-ech.json   Cloudflare 公开的 ECH 配置（会轮换）
├─ scripts/
│  ├─ sniroute.py           全部命令的实现
│  ├─ setup-browser-ech.ps1 装根证书 + 写 hosts（需管理员；纯 ASCII）
│  ├─ start-chrome-ech.ps1  以独立配置启动带 ECH 的 Chrome
│  ├─ start-chrome-ech.cmd  上面那个的包装器（任意目录可调）
│  └─ doh-ech.cmd           单独起 DoH 的包装器
├─ tools/
│  ├─ doh_ech.py            本地 DoH：A / AAAA / HTTPS(ech) 记录
│  ├─ doh_selftest.py       DoH 自检（发 wire 查询并解码应答）
│  └─ frag_relay.py         TLS 分片中继（实验，结论：对本网络无效）
├─ docs/references.md       参考清单，每条注明它改变了哪个判断
├─ ca/                      根证书与私钥（生成物，不入库）
├─ conf/certs/              叶子证书（生成物，不入库）
├─ logs/                    运行日志（生成物，不入库）
├─ chromeprofile/           Chrome 独立配置（生成物，不入库）
└─ nginx.path               可选：把 nginx.exe 路径写在这里
```

改站点只需要动 `data/sites.json`（`dns_mode` 决定走 ECH 还是走本地 nginx），
然后重跑 `gen-cert` 与 `sniroute.cmd up`。

---

## 安全须知（别跳过）

- **根证书私钥 `ca/rootCA.key` 是本机最高机密。** 泄露它的人可以为这张根签发的任何域名
  伪造证书。它已经在 `.gitignore` 里，请不要手动提交、不要贴进对话、不要上传。
- **只监听回环。** 所有 `listen` 都写死了 `127.0.0.1` / `[::1]`。改成 `0.0.0.0` 等于把
  一个不需要密码的代理端口开给整个局域网。
- **这是有意的本地中间人。** 装了自己的根 CA，就意味着本机信任它签的一切。这也是本方案
  和「装别人的根 CA」的根本区别：信任面只在你自己的机器上。
- **日志不记录完整 URL**（不含 `$request`），只有域名与状态码 —— 代理日志不该变成浏览历史。
- **卸载三件事要做全**：`hosts remove`、`certutil -delstore`、删掉 `ca/`。

---

## 已知限制（v0 的诚实清单）

| 限制 | 后果 | 计划 |
|---|---|---|
| **pixiv 主站在 Cloudflare 上，SNI 被行为式阻断** | `enabled: false`；要通必须有 ECH，而 nginx 做不到 | 需要额外的 ECH 网关组件（Go/Rust） |
| hosts 不拦 SVCB/HTTPS 记录 | 浏览器可能拿到 ECH 配置，届时连接绕过本代理 | v1：本地 DNS 拦截 |
| 隐藏 SNI 是否被上游接受，因站而异 | 图片站接受（老源站按 Host 路由） | 每加一个站点都要实测 |
| 无 GUI、无开机自启、无崩溃重启 | 手动启停 | 需要再说 |
| 上游 IP 需要自己找候选 | 候选全失效时 probe 无能为力 | `discover` 提供两条来源，但仍需人工兜底 |
| **中继需要一台墙外服务器** | 没有时 YouTube 视频、Twitch 与 Discord 客户端更新仍不可用；武汉 ECS 实测 15 秒超时 | 自备境外 VPS |

---

## 为什么不做 GUI

界面的价值远低于 IP 数据本身 —— 真正让别人拿不走的是「哪些地址现在还能用」，
不是窗口长什么样。所以这里的重心全在 `probe` 与 `data/sites.json` 上。

## License

```
Copyright (C) 2026 Kynarain. All rights reserved.
Licensed under the GNU General Public License v3.0.
```



GPL-3.0-only (see `LICENSE`). This covers this project's own code: the Win32 tray
app, the local DoH server, and the scripts.

Two obligations worth stating plainly, since binaries get handed to other people:

* **Source offer.** GPL asks that whoever receives the binaries can get the
  corresponding source. This repository is that source; if you received only a
  ZIP/MSI, the person who gave it to you must be able to hand over the source too.
* **Bundled third-party runtimes** (`python`, `nginx`, `openssl`) keep their own
  upstream licenses, listed in their own `LICENSE` files inside the install
  directory. They run as separate processes, invoked but never linked, so they
  are aggregated with - not combined into - this project.

The installer has **no license page**: the flow is
Welcome -> install folder -> confirm -> progress -> finish. The license text
ships as `LICENSE` next to the executables.

### Browser DoH without manual setup

Chrome's "Secure DNS" can be changed only by a human in `chrome://settings` or by
policy. Command-line switches for DoH were removed (Chrome 157 ignores
`--dns-over-https-*`), and editing the profile file does not work because those
preferences are MAC-protected. So the tool writes policy:

  DnsOverHttpsMode      = secure
  DnsOverHttpsTemplates = https://127.0.0.1:5444/dns-query

Two tiers, so ordinary machines never see a UAC prompt:

  1. `HKCU\Software\Policies\Google\Chrome` - per-user policy, no elevation.
     This is attempted automatically on the first launch.
  2. `HKLM\SOFTWARE\Policies\Google\Chrome` - machine policy, one elevation.
     Used only when the user policy key is not writable (a hardened machine
     where `HKCU\Software\Policies` is owned by SYSTEM and merely readable).

The tray menu exposes it as "configure browser (DoH)"; reverting removes exactly
those two values and nothing else. This is also why the manual instruction is
gone from the readme: it is a fallback, not the normal path.

### Global DNS mode (default)

Instead of teaching each browser to use DoH, the default setup redirects the
system resolver: one NRPT rule tells the Windows DNS Client service that the
proxied namespaces are answered by sniroute's local plain-DNS listener. Every
program that resolves through the system stack then gets the redirect - Edge,
Firefox, curl, updaters - so images work everywhere without per-browser setup.

  HKLM\SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DnsPolicyConfig\{GUID}
    Version            = 2
    ConfigOptions      = 8        (GenericDNSServers present)
    GenericDNSServers  = 127.0.0.1
    Name               = .pximg.net, i.pximg.net, pximg.net

Details that cost real time to discover:

* The server field must be a bare address. `127.0.0.1:5335` looks like it should
  work (the registry spells other endpoints as address:port), but the DNS Client
  then reports the policy as invalid (Get-DnsClientNrptPolicy fails with 9572)
  and silently falls back to normal resolution. Port 53 only.
* Port 53 on 0.0.0.0 is usually taken by Internet Connection Sharing (svchost).
  Binding 127.0.0.1:53 still works and is the specific binding the loopback
  query hits, so no conflict.
* Global mode puts the CA in the machine store and sets machine-wide Chrome/Edge
  DoH policy, so it needs one elevation. The main site cannot go through plain
  DNS: those answers are unauthenticated, no browser trusts an ECH config from
  them, and without ECH the upstream SNI is the thing being blocked. So the main
  site keeps using the browser's DoH (which does carry ECH), and images use the
  local TLS termination.

`scripts\nrpt.ps1 -Install|-Revert|-Check` manages it; `-IncludePixiv` routes the
main site here too (experimental, expected to be unreliable).

## Supported sites

**pixiv** - main site, API and image host are all handled by this tool, by two
different paths:

| Path | What it covers | Why |
|---|---|---|
| browser DoH + ECH | main site and API | only an encrypted DNS answer can carry an ECH config |
| NRPT -> local TLS termination | image host | no ECH needed, so every program can be covered, not just browsers |

**steam** - the store and the community take different paths:

| Path | What it covers | Why |
|---|---|---|
| `dns` (resolution only) | store and image CDN | browser connects directly; the certificate stays Steam's own |
| `loopback` (local TLS termination, no SNI upstream) | community | with SNI the connection is reset; without SNI it returns 200 |

Measured: the store opens in 4.8-6.1s (title "Welcome to Steam"),
`cdn.akamai.steamstatic.com` images load (IMG_OK), and each community
upstream (104.89.103.51 / 23.15.140.216 / 23.205.151.43) succeeded 3 times.

Known limitation: community pages load third-party resources (e.g.
`community.steamstatic.com`); some of them stall until timeout on this network.
The page content renders, but "load complete" can be very slow. This part is
still being investigated.

**curseforge** - `dns` only (13 domains). A real browser gets 200 for
`www.curseforge.com` (title "CurseForge - Mods & Addons Leading Community",
2.16s) and for `/minecraft` (title "Minecraft Mods & Modpacks - CurseForge",
1.65s). File downloads work: `mediafilez.forgecdn.net` returns 206 with
`Content-Type: application/java-archive`, `Content-Range: bytes
0-131071/7091096`, and ZIP magic `504b0304`.

Worth knowing:

* Cloudflare's challenge page returns 403 to bare clients
  (`cf-mitigated: challenge`). That is not blocking - a real browser passes
  automatically, while a headless browser stays on "please wait...".
* `forgecdn` must keep the real SNI (CloudFront fails the handshake without
  SNI), so it cannot be moved to `loopback`.
* `api.curseforge.com` is reachable but needs an API key (403 without one).

**cloudflare** - `dns` only (14 domains). `www.cloudflare.com` returns 200
(title "Cloudflare: Build for ...") and `cdnjs.cloudflare.com` JS is actually
fetched (94 KB). `dash.cloudflare.com` returns a stable 403 - that is
Cloudflare's own human check, not blocking. `one.one.one.one` (1.1.1.1) and
`cloudflare-dns.com` are not reachable on this network and are not included;
`pages.dev` connects directly (SNI is normal).

**onedrive** - `loopback` (local TLS termination, no SNI upstream, routing by
Host - same as Steam Community): `onedrive.live.com` opened in 15.3s.

**mega** - `loopback` (same): `mega.nz` opened in 10.3s. MEGA edges take 9-12s
just to finish the TLS handshake, so this site runs with its own relaxed
timeout.

**dropbox** - `loopback` (same): `www.dropbox.com` opened in 13.2s. One host is
explicitly unusable: `dl.dropboxusercontent.com` (the download CDN) - with SNI
the connection is reset, without SNI it returns only a generic 404 and does not
honour Host.

**roblox** - `dns`, but the service is region-restricted: `www.roblox.com`
301s to `www.roblox.cn/home`, which returns 404 (Roblox's own China site is
dead). Tencent's `roblox.qq.com` returns 200 but is a different product. This
is a server-side restriction, not a network problem.

**nexusmods** - `dns`: works with the real SNI; the 403 on `www` is
Cloudflare's human check, not blocking. `staticdelivery` serves real image
bytes. `files.nexus-cdn.com` mod downloads need a signed URL issued with an API
key; without one the request is 302'd to an expired page (application-layer
limit).

**modrinth** - `dns`: the site is on Cloudflare and the download CDN is on
Fastly, both require the real SNI. A real jar (1,211,839 bytes) was fetched and
its SHA-512 matched the API value. The API is public, no key needed.

**mod.io** - the only `ech` site: its domains are SNI-blocked selectively - on
the same IP other hostnames work, but the `mod.io` SNI is reset within 200ms;
without SNI Cloudflare rejects it. With the shared ECH config the page returns
200 (178 KB). Its API needs a key.

**ea** - `dns` only. 18 of the 35 `www.ea.com` domains are usable, all with the
real SNI and none needing ECH, so no local proxy is involved. **Resolution is
fixed, but Chrome still hits a protocol error**: loading reports
`net::ERR_HTTP2_PROTOCOL_ERROR`, forcing HTTP/1.1 turns it into
`ERR_CONNECTION_RESET`, while curl (Schannel stack) gets 200. This is a protocol
negotiation problem between the client and the Akamai edge - DNS cannot fix it.
Rewriting the upstream protocol through local nginx was tried and returned 504;
not used.

**bilibili** (international, bilibili.tv) - `dns` only: all 15 related domains
work with the real SNI (no SNI blocking, no ECH), and JS / PNG assets return
real bytes. But `www.bilibili.tv/` returns 200 with `Content-Length: 0` (blank
page), and the playback API returns
`{"code":10003003,"message":"The area is inaccessible"}` - a region restriction
on this network's egress IP that cannot be fixed at the network layer. In
short: the network works, the video does not.

**twitch** - mixed modes: live streams work; VODs / clips / login do not.
Live: `*.ttvnw.net` is on CloudFront and works with the real SNI - a real live
segment was fetched (200, 1,870,914 bytes) - so it uses `dns`.
Pages / emotes: `*.twitch.tv` (www etc.) reset with SNI and work without it, so
they use `loopback`; `static-cdn.jtvnw.net` uses `dns`.
Unusable: VODs / clips (the CloudFront edge `18.65.14.x` behind vod-secure /
clips-media-assets2 is IP-blocked - any SNI is reset) and the login services
(api / passport / id).
Critical: **`ttvnw.net` must never be put in `loopback`** - CloudFront requires
SNI, and without it the handshake fails, which would break all live playback.
Known issue: static assets from `assets.twitch.tv` still occasionally time out
upstream.

**x** (formerly Twitter) - mixed modes:
Front door: `x.com` / `www.x.com` / `api.x.com` / `t.co` use `loopback` (local
TLS termination) and send a **root-dotted SNI** (`x.com.`) upstream, passing
Host through. Why not ECH: the `x.com` Cloudflare zone does not accept the
shared ECH config - measured 403 (browser shows
`ERR_HTTP_RESPONSE_CODE_FAILURE`). Why the trailing dot: the block targets the
exact hostname, and a ClientHello carrying `"x.com."` is not blocked
(interleaved same-IP control: `x.com` gets RST / `x.com.` gets 200; 6 of 7
candidates succeeded 3/3). In the config this is `proxy_ssl_name $host.`.
Legacy domains such as `twitter.com` are reset even with the trailing dot and
are not included.
twimg: `pbs` / `abs` / `video.twimg.com` are on Fastly; the exact SNI is reset
and no-SNI gets 400. The fix is to send Fastly's generic mapping name
`twimg.twitter.map.fastly.net` upstream while passing Host through (measured
`abs.twimg.com/favicons/twitter.ico` 200, 912 bytes; the video m3u8 starts
with `#EXTM3U`). All three twimg domains must be captured together, otherwise
the X page has no styling.
Important trap: the config **deliberately sets no upstream keepalive and turns
off `proxy_ssl_session_reuse`**; otherwise nginx reuses TLS connections /
sessions across hostnames and Cloudflare answers roughly half the requests
with 421 Misdirected Request.

**grok** - `loopback`, the same trick as X (root-dotted SNI): `grok.com` and
`x.ai` get the trailing dot upstream while Host stays unchanged. Measured:
`grok.com` 200, page 235 KB, title Grok; `x.ai` 200. The exact SNI is reset
within 27-40ms, and without SNI Cloudflare rejects it, so
`proxy_ssl_server_name off` is not an option. Upstreams are hardcoded to
104.18.28.234 / 104.18.29.234 / 104.18.18.80; 104.18.19.80 is a black hole and
must be excluded.

**openai** - `dns`, resolution only: `openai.com` / `www` / `platform` /
`auth` / `cdn.oaistatic.com` / `cdn.openai.com` all work with the real SNI.
The 403s are Cloudflare's WAF / human check, not blocking; `cdn.oaistatic.com`
serves real JS bytes. Measured `openai.com` 200 with title
"OpenAI | 研究与部署".

Not included: **Spotify** - the audio CDN is reachable (a real MP3 of 344,502
bytes was fetched), but the web player returns 451 (regional restriction), so
proxying it would not help. **hCaptcha / Arkose Labs** were probed and are not
blocked on this network, so they are not proxied; Arkose requires the real SNI
on the server side and must never go through `loopback`.

Also not added: **ChatGPT** - `chatgpt.com` has its SNI blocked, and without SNI
Cloudflare rejects it too (that IP set forces SNI); with DNS poisoning, only
ECH can work. ECH does complete - a Cloudflare-rendered page comes back - but
the page reads "Unable to load site ... If you are using a VPN, try turning it
off. [IP:<public-egress>]": OpenAI blocks mainland egress IPs, which the network
layer cannot fix. It is left out of the menu rather than shipping an item that
cannot open.

The concrete hostnames live in `data\sites.json`. `scripts\gen-sites-list.ps1`
flattens the `service` field into `data\sites-tray.txt` for the tray menu, so the
menu shows the service name ("pixiv") rather than a hostname list, and cannot
drift from the configuration. Adding another site means editing `sites.json` and
re-running the build - whether it works depends on how that origin treats a
connection without SNI, which is a research question, not a setting.

## Upstream address refresh

The loopback sites pin their upstream IPs in `conf/upstreams/*.list`. Those addresses
drift whenever a CDN reshuffles its fleet, so they are now re-checked automatically:
once in the background at startup (never delaying it), every 45 minutes from the tray,
and on demand via the tray item or `sniroute.cmd refresh` (`--dry-run` to preview).

Health is judged by a real TLS handshake, not by HTTP status: these origins answer 403
or 404 on unknown paths all day, so status codes would flag good addresses as broken.
An address must produce an HTTP response at least 2 times out of 3.

TCP-reachable but TLS-failing addresses are kept and moved to the end rather than
dropped - that is usually Fastly/Akamai rate limiting, and dropping them would take the
site down. Only addresses that cannot even open a TCP connection are removed. If no
address stays healthy the file is left untouched.

## NAT type detection

`sniroute.cmd nat` reports the NAT kind (RFC 5780). Options: `--summary`,
`--json`, `--server`, `--server2`, `--timeout-ms`. The tray menu runs the same
check and shows the conclusion in a popup.

The verdict combines two behaviours: mapping behaviour (send from the same
local port to different targets - does the public mapping change?) and
filtering behaviour (can an outside host reach back?). The combination
distinguishes public address / full-cone / restricted-cone /
port-restricted-cone / symmetric.

Default STUN server is `stun.miwifi.com:3478`. Google's `stun.l.google.com`
resolves to 127.0.0.1 on this network (poisoned DNS) and is unusable;
Cloudflare and Nextcloud are fallbacks.

Measured on this machine: **Full Cone**, mapping stable at a single public address,
filtering independent of endpoint.

One caveat: the public IP from an HTTP echo can differ from the STUN mapped
address (measured here: HTTP via proxy `203.131.255.202`, UDP direct
`<your-public-ip>`). The report states that STUN is authoritative.

## Relay (optional)

Off by default (`data\relay.json`). It covers the traffic that has no direct
route on this network: YouTube video, Twitch VODs/clips/login, and Discord
client updates.

Flow: nginx sends the real SNI -> `tools\relay.py` reads the hostname from the
ClientHello -> SOCKS5 (`ssh -D`) -> target port 443 -> byte passthrough. The
SNI appears only inside the tunnel.

**It needs a server outside the Great Firewall.** The tool cannot supply one.
Measured counter-example: an Alibaba Cloud Wuhan ECS instance (`cn-wuhan-lr`)
resolving to a real IP still timed out after 15s when reaching Google, so it
cannot serve as a relay. With a working overseas server, set `enabled: true`
and `ssh.host` / `key` in `data\relay.json`, then run
`sniroute.cmd relay start`.

`sniroute.cmd relay status|start|stop`. While relay is off, the affected
domains fail fast with 502; other sites are unaffected.

## Chrome background request

Chrome's background request to `optimizationguide-pa.googleapis.com` (the
optimization guide) used to pull 5 MB through the proxy - 201s measured - which
slowed down every page load. It now fails fast instead.

## 版本记录

**3.9.6**

- 修复 `sniroute check` 的两个崩溃：dns / ech 模式的站点没有 `upstream_file`、
  `candidates` 字段，旧代码直接取键会 `KeyError` 退出。
- 修复 `sniroute status` 误报：只认仓库目录下的 `logs\nginx.pid`，安装版由托盘
  启动时 pid 在 `%LOCALAPPDATA%\Sniroute\nginx\logs`，导致 443 明明在监听却报
  "nginx 未运行"。现在按 pid → 安装目录 pid → 443 端口监听 三级判定。
- `check` 对只占位、不托管域名的站点（youtube、relaycert）不再打印空域名。

**3.9.5**

- 全量站点扩展到 21 个可托管站点（含 EA、bilibili 国际、Twitch、Modrinth、mod.io、
  x/twimg、grok）。
- 托盘新增 NAT 类型检测与上游 IP 定时刷新；MSI 详细说明写入版本信息。

## Android 版（独立目录 android/）

除了 Windows，同一套思路还有一个 Android 应用：用 VpnService 接管 TCP，
读 TLS ClientHello 里的 SNI，把目标换成实测可达的 IP，并对首段做分段。

**实测可用**（模拟器 API 37，走本机隧道，页面完整渲染）：
GitHub、CurseForge、Modrinth、百度。

**做不到**：SNI 被阻断的站点（pixiv 主站、Steam 社区、Discord）——
那需要 ECH 或本地 CA 做 TLS 中间人，而 Android 7 以后应用默认不信任用户 CA，
非 root 设备装不进系统信任库。这是架构限制，不是没写完。

产物：`dist\sniroute-3.9.6-android.apk`（与桌面版同一签名身份）。
细节、构建方式、踩过的坑都写在 `android\README.md`。

## 手机 / 浏览器：用 ECH 绕过 SNI 封锁（新）

不开 VPN、不装证书、不用海外服务器，只用一个域名 + 一个 Cloudflare Worker。
原理是让浏览器自己把 SNI 加密（ECH），GFW 只看到外层 `cloudflare-ech.com`。

```
浏览器 ──DoH──> doh.你的域名 ──> Cloudflare 自己的 DoH（拿真实 IP + ech=）
浏览器 ──ECH──> 目标站点（SNI 加密，只有 Cloudflare 能读）
```

**实测（Android 模拟器，无 VPN、无 CA）**：pixiv.net ✅ 完整加载；
store.steampowered.com ✅；GitHub ✅。discord / x.com 未成功。

部署步骤、验证命令、已知限制都写在 `worker/README.md`；
Worker 源码 `worker/doh-worker.js`，自检工具 `worker/probe-doh.mjs`。

## WARP 出口（3.9.7）

Sniroute 能解决 DNS 污染、以及接受"无 SNI"的上游；但 Google、YouTube、Play 网页版
这类 IP 层封锁的站点，必须有墙外出口。3.9.7 把 Cloudflare WARP 接成了可选出口。

**安装客户端**（一次性，会弹管理员确认）：

```powershell
winget install --id Cloudflare.Warp -e
```

**命令**：

```powershell
sniroute.cmd warp status    # 连接状态 + 协议 + 端点 + 延迟/丢包
sniroute.cmd warp on        # 切到 WireGuard 协议并连接
sniroute.cmd warp off       # 断开
sniroute.cmd warp endpoint 162.159.192.2:2408
```

托盘菜单里也有 **WARP (exit)** 子菜单，显示状态并提供连接/断开。

**为什么默认切 WireGuard 协议**：WARP 客户端默认走 MASQUE（HTTP/3），实测在国内
经常"客户端显示 Connected，但流量完全不通"（curl 全部 000、丢包 20%+）。换成
WireGuard（UDP 2408）后同一台机器立刻可用，所以 `warp on` 会先把协议切过去。

**两者共存**：WARP 连上后，Sniroute 的 nginx 上游会自动经 WARP 出墙，NRPT 那条
DNS 路径不变。实测同样请求，走 Sniroute（NRPT → nginx → WARP）比直连 WARP 快
3–5 倍（1.0s vs 5.2s），因为 nginx 复用了连接。


### WARP 不可用时自动通知（3.9.9）

托盘每 60 秒检查一次，两种"不能用"都会弹通知：

| 情况 | 判定 | 通知 |
|---|---|---|
| 客户端从已连接变为断开 | `warp-cli status` 不再是 Connected | **立即**弹「WARP 出口已断开」 |
| 显示已连接但实际不通 | 连续 2 次取不到 `gstatic.com/generate_204` | 弹「WARP 出口不通」，10 分钟内最多一次 |

主动在托盘里点「断开 WARP」不会告警（那是你自己的意图）。日志见
`%LOCALAPPDATA%\Sniroute\logs\tray.log`，会记 `[warp] alert: ...`。

实测记录：

```
[15:15:45] [warp] recovered
[15:16:45] [warp] alert: disconnected
```


### 一键安装 WARP 客户端（3.9.10）

没装 WARP 时，托盘菜单会显示：

```
WARP 出口 ▸ 未安装 WARP 客户端
            安装 WARP 客户端（winget，需一次管理员确认）
```

点第二项即执行：

```powershell
winget install --id Cloudflare.Warp -e --accept-package-agreements --accept-source-agreements
```

**为什么不把 WARP 打进我们的安装包**：WARP 客户端是专有软件，再分发需要授权；
而 winget 指向 Cloudflare 官方源，属于"替用户执行一次官方安装"，不涉及再分发。
代价是需要一次 UAC（WARP 要装服务和驱动），这次确认由用户自己点。

装完菜单立即变成"连接 / 断开"。已安装时会提示已存在，不会报错。


### 没有 winget 的机器怎么办（3.10.0）

LTSC、精简版 Win10、没装 App Installer 的系统没有 `winget`。一键安装改成三级回退：

1. **winget**（有就用，能拿最新版）
2. **Cloudflare 官方安装包直链**：`downloads.cloudflareclient.com`（没有 latest 端点，
   钉版本号，实测可用；下载后交给系统安装，仍会弹一次 UAC）
3. 都失败 → 打印 `https://cloudflarewarp.com/` 让用户手动装

也可以直接跑：

```powershell
sniroute.cmd warp install              # 自动：先 winget，不行再下载官方包
sniroute.cmd warp install --no-winget  # 强制走官方安装包
```

**踩到的坑**：Cloudflare 的下载端点会拒绝 Python-urllib 的默认 UA（403），
必须带浏览器 UA 才能下；文件头校验为 MSI（`d0cf11e0a1b11ae1`），实测 38.8 MB。

**为什么不打包进 MSI**：WARP 客户端是专有软件，再分发需授权。上述做法都是
"让用户从官方源安装"，不涉及再分发。


### WARP 断开时自动切回本地 DNS（3.10.1）

WARP 掉线后，托盘会：

1. 弹提示「WARP 已断开，正在切回本地 DNS 方案」
2. 检查 443(nginx) / 5444(DoH 前置) / 5454(DoH 后端)
3. 缺任何一个就跑一次 `sniroute.cmd up` 把链路拉起来；以 `up` 的退出码为准，
   失败时再补一次
4. 成功后弹「已切回本地 DNS 方案（DoH + nginx）」；仍失败则提示手动 `sniroute.cmd up`

为什么需要：WARP 是整机隧道，它掉线时最容易出现的错觉是"网断了"。实际上本地
DoH/nginx 可能因为升级、崩溃或从未启动而缺失，这一步把"出口没了"和"链路也没了"
分开处理。

实测日志：

```
[15:57:54] [warp] recovered
[15:58:54] [warp] alert: disconnected
[15:58:56] [dns] local chain incomplete, running up
```



### 上游连接超时与端口占用清理（3.10.2）

**`proxy_connect_timeout 8s`**：原来只设了读写超时（180s），连接超时用 nginx 默认的
60 秒。WARP 抖动时 worker 全挂在 `connect()` 上，连接队列被占满，现象是
**「nginx 在监听，但所有 NRPT 域名一起卡死」**（实测每个请求 20 秒后 000）。
现在连接阶段 8 秒就放弃，worker 不会被长期占住。

**端口被外部实例占用时自动清理**：`sniroute.cmd up` 以前遇到 5444 被其它目录启动的
DoH 占着会直接 `die`，用户只看到"起不来"。现在先 `taskkill` 掉、重新探测，清不掉才报错。

实测（修复后）：`www.google.com 302 1.6s` / `www.youtube.com 200 4.9s` / `i.pximg.net 200 1.8s`。



### 端口占用与陈旧 pid 的修正（3.10.4）

`5444` 有**两个持有者**：本工具的 nginx 前置（h2）和 Python DoH 后端。早期的清理逻辑
按端口猜进程，把后端误杀后端口仍被 nginx 占着，于是报"无法结束"——实测直接把 DoH 搞挂，
浏览器安全 DNS 全部 502。

现在的规则：

1. 先按**自己的 pid 文件**停旧后端，不按端口猜
2. 停完仍被占用且进程是 `nginx` → 属于正常，继续启动
3. 是其它进程 → 报错并说明，不擅自杀
4. `doh_running()` 改成**看 5454 端口**，并在端口没人听时清掉陈旧 `doh.pid`，
   避免"已在运行，跳过"把坏掉的后端当成好的

实测：`5444 -> 200`、`5454 -> 200`、`www.google.com 302 1.2s`、`i.pximg.net 200 1.5s`。



### 启动顺序修正（3.10.5）

停掉占用 5444 的旧后端之后，代码仍拿**旧 pid** 继续判断，于是打印
「DoH 已在运行，配置未变，跳过」并返回——后端根本没起来，表现为 `5454 FAIL`、
浏览器安全 DNS 502。现在停掉后会把 pid 清空，确保真正走到启动分支。

配合 3.10.4 的规则，`up` 现在的次序是：按 pid 文件停旧后端 → 端口仍被 nginx
占用则视为正常 → 真正启动后端 → 健康检查。

干净复位后实测：`5444 -> 200`、`5454 -> 200`、
`google 302 1.7s`、`pximg 200 2.8s`、`youtube 200 5.0s`。



### WARP 默认关闭（3.10.6 / 3.10.7）

产品默认状态改成 **WARP 关闭**：

- 用户没主动启用过时，托盘启动会断开仍在连着的 WARP
- 托盘里点「连接 WARP」会写入 `data\.warp-enabled` 标记，重启后保持
- 点「断开 WARP」删除标记，回到默认关闭
- 未启用时不再弹「WARP 断开」告警（避免自己关掉还被通知）

**顺带修掉 DoH 的两个坑**（关闭 WARP 后才暴露，因为此时没有墙外出口兜底）：

1. **上游域名解析递归**：后端自己在 53 端口提供明文 DNS，转发时用域名会让
   urllib 做一次系统解析，又回到自己的 53 端口 → 递归 → 每个新查询卡满 12 秒。
   现在把上游域名钉到固定 IP（`dns.alidns.com`→223.5.5.5、`doh.pub`→120.53.53.53、
   `dns.google`→8.8.8.8），SNI 仍是域名。
2. **失败端点反复重试**：加了 10 分钟冷却（连续失败 2 次就跳过）与 ECH 刷新失败
   的 120 秒冷却，避免每个查询都先去等被墙的端点。

实测（WARP 关闭）：冷查询约 3 秒、缓存后 0.02 秒，`5444 -> 200`。



### DoH 应答缓存（3.10.9）

冷查询要付一次上游往返（实测 0.1–1.4 秒），浏览器对同一域名会反复问，所以：

- **内存缓存 5 分钟**（`CACHE_TTL = 300`）
- **落盘到 `data/doh-cache.json`**，每 60 秒写一次；重启后端（升级/换配置）后
  仍然有效，不必重新付冷查询
- 上限 2000 条，超出砍掉最旧的一半；文件坏了就当没有，不影响启动
- 所有端点都在冷却时仍强制试第一个，避免整段时间直接 SERVFAIL

实测：

```
冷查询 cache-test-1.example.net -> 1.37s
重复查询同一名字            -> 0.02s
重启后端后再查              -> 0.03s（载入 135 条落盘缓存）
```



### 系统 DNS 卡死修复（3.11.1）

明文 DNS（53 端口，系统解析器用）原来是**单线程串行**处理，而且每个 dns 模式
查询都会对上游返回的每个地址做一次 TLS/TCP 探活（8 个地址 = 8 次）。一条慢查询
会把后面所有查询一起堵住——实测表现是**所有域名解析超时**，游戏（如战地 1）
报"无法连接到 EA"。

现在：

1. 每条查询交给 8 线程的线程池，互不阻塞
2. 探活并行进行，系统解析器用 1 秒超时（浏览器仍用 3 秒）
3. dns 模式优先用站点表里已验证过的 IP；没有时才走上游 + 过滤

实测（明文 53）：

```
github.com      0.24s -> 20.247.166.64      （修之前返回投毒 IP 57.155.223.240）
accounts.ea.com 1.47s -> 44.194.199.215
api1.origin.com 0.40s -> 23.47.146.135
```



### NAT 类型检测的修正（3.11.2）

`sniroute.cmd nat` 原来会给出**互相矛盾**的结论：同一台机器连跑几次，得到
"全锥型 / 受限锥型 / 无法判定"三种结果。原因有三个：

1. **把"没有响应"当成"地址端口相关过滤（APDF）"**。服务器不支持 CHANGE-REQUEST
   或丢包时也会没响应，两者不该混为一谈。现在只认正面证据。
2. **忽略 CHANGE-REQUEST 的回包被当成"换了源地址"**。不支持该扩展的服务器会从
   原地址回一个正常的 BINDING_SUCCESS，旧逻辑据此判成"全锥型"。现在要求回包
   地址**正好等于服务器通告的 OTHER-ADDRESS** 才算 EIF。
3. **单点采样**。这条链路（<your-public-ip>）实测过滤行为会随目的地变化，属于
   多网关 CGNAT 的典型表现。现在做 **5 轮取证，只有 5/5 一致才下结论**，
   否则明确报"行为不一致（可能位于多网关 CGNAT）"。

另外：绑定探测加了重试；STUN 服务器支持多服务器回退（`stun.miwifi.com` →
`stun.cloudflare.com` → …）；映射地址与本地出口相同时判定为"公网直连"。


**3.11.3 的修正（把判据改回来）**：3.11.2 要求"回包地址必须等于服务器通告的
OTHER-ADDRESS"才算全锥，这是错的——首次绑定丢包时 `other` 为 `None`，判据直接
失效，于是全锥型被报成"无法判定/不一致"。正确的判据是：

> **只要收到来自"我们从未发过包的地址"的合法回包，就是全锥型（EIF）。**

成功是强证据，失败是弱证据：CHANGE-REQUEST 丢包很常见，不能"多数轮次说了算"，
只要有一轮拿到不同 IP 的回包就足以定论。

实测（这台机器，server 111.206.174.3:3478）：

```
change=IP+PORT -> 回包来自 111.206.174.2:3479   -> 全锥型
```

连跑 5 次结论一致：`全锥型 NAT（Full Cone）`，映射行为端点无关。



**注意**：WARP 在国内时通时不通；不通时先试换端点，仍不行再考虑自建 VPS。
Windows 上 WARP 与 Sniroute 可以同时开；Android 上只能二选一（系统限制一个 VPN）。



## 一条命令生成 Clash 的 WARP 配置（3.11.0）

不用装 wgcf：

```powershell
sniroute.cmd warp export                       # 自动注册账号并生成 dist\warp-clash.yaml
sniroute.cmd warp export -o D:\warp.yaml      # 指定输出
sniroute.cmd warp export --renew               # 重新注册一个账号
sniroute.cmd warp export --endpoint 162.159.193.10:2408
```

它做的事：

1. 用 `tools/x25519.py`（纯 Python，RFC 7748 测试向量已验证）生成 Curve25519 密钥对
2. 向 `api.cloudflareclient.com` 注册，拿到 `172.16.0.2/32` 隧道地址与对端公钥
3. 把账号存到 `data/warp-account.json`（**已 gitignore，且打包时会被剔除**）
4. 生成完整 Clash 配置（含 fake-ip DNS、fallback 组、`MATCH,PROXY`）

**传输层为什么用 curl**：实测 Python 的 urllib 请求会被直接 RST（TLS 指纹差异），
curl 与 Go 客户端都能过，所以注册走 `curl.exe`（Win10 1803+ 自带）。

托盘菜单里也有：`WARP 出口 -> 导出 Clash YAML (WARP)`。

**安全**：生成的文件含 WARP 私钥，等同账号凭据。构建脚本会在打包前剔除
`data/warp-account.json` 与 `data/doh-cache.json`，所以安装包里不会带别人的账号。

## WARP 配置转 Clash YAML

Cloudflare 官方客户端**不导出配置**。要拿到 WARP 的 WireGuard 参数，只能用
[wgcf](https://github.com/ViRb3/wgcf) 注册一个真实 WARP 账号：

```powershell
winget install ViRb3.wgcf
wgcf register --accept-tos    # 生成 wgcf-account.toml
wgcf generate                 # 生成 wgcf-profile.conf
```

`.conf` 是标准 WireGuard 格式，Clash 用的是自己的 YAML 结构，字段名不同，
手抄容易漏（漏 `remote-dns-resolve` 就会出现"已连接但域名解析错"）。仓库里带了一个转换脚本：

```powershell
python tools\wgcf-to-clash.py wgcf-profile.conf -o warp-clash.yaml
# 或自动找 %%TEMP%%\wgcf\wgcf-profile.conf
python tools\wgcf-to-clash.py --auto -o warp-clash.yaml
# 换端点（可重复）
python tools\wgcf-to-clash.py --auto --endpoint 162.159.193.10:2408 -o warp-clash.yaml
```

输出是**完整可导入**的 Clash 配置（含 DNS、fallback 组、MATCH 规则），不是片段。

字段映射关系：

| wgcf-profile.conf | Clash YAML |
|---|---|
| `PrivateKey` | `private-key` |
| `PublicKey` | `public-key` |
| `Address` 的 IPv4/IPv6 | `ip` / `ipv6` |
| `Endpoint` 的 host:port | `server` / `port` |
| `MTU` | `mtu` |
| `DNS` | `dns` + `remote-dns-resolve: true` |

**注意**：生成的 YAML 里有 WARP 私钥，等同账号凭据，不要提交到仓库或发给别人。

## 关于哈希文件（3.10.5 起）

不再随产物生成 `.sha256` / `.sha1` / `.md5` 与 `SHA256SUMS.txt` / `SHA1SUMS.txt` / `MD5SUMS.txt`。

需要校验时：

```powershell
sniroute.cmd verify Sniroute-3.10.5.msi
# 或
Get-FileHash Sniroute-3.10.5.msi -Algorithm SHA256
```

MSI 本身有 Authenticode 签名，双击属性里的"数字签名"即可看到签名者与时间戳。
