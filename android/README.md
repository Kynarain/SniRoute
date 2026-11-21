# Sniroute for Android

接管手机上的 TCP 流量，在 TLS 握手之前读出 SNI，把目标换成实测可达的 IP，
并对首个报文做分段。目标是修好「DNS 被污染、但 SNI 正常」的那一类站点。

## 它能做什么、不能做什么

| 情况 | 结果 | 依据 |
|---|---|---|
| DNS 污染 + SNI 正常（GitHub、Steam 商店、CurseForge、Modrinth 等） | ✅ 已实测可用 | GitHub 页面完整加载 |
| 未被封锁的普通站点（百度等） | ✅ 正常（走隧道） | 页面完整加载 |
| SNI 被阻断（pixiv 主站、Steam 社区、Discord） | ❌ 做不到 | 见下 |
| 需要地区限制突破（YouTube 视频、bilibili 地区、ChatGPT） | ❌ 做不到 | 需要海外出口 |

**为什么 SNI 被阻断的做不到**：拿桌面直连验证过——`104.18.42.145` 配真实 SNI
`www.pixiv.net` 同样被重置，封的就是 SNI 本身。要绕开只能靠 ECH 或本地 CA 做
TLS 中间人，而 Android 7 以后应用默认不信任用户 CA，非 root 设备无法把证书装进
系统信任库。这是架构限制，不是没写完。

## 它是怎么工作的

1. 建立 VPN，接管全部 TCP（`0.0.0.0/0`）。**只有全部接管，被污染的域名才会
   走进来**——按 IP 放行的话，投毒地址根本不会命中。
2. 收到 SYN 只回 SYN-ACK，**等第一段数据到达再连上游**。判定顺序：
   首字节不是 `0x16`（不是 TLS）→ 立刻连；能解析出 SNI → 立刻连；
   都不行 → 200ms 后兜底连。
3. 从 ClientHello 里取 SNI，命中实测表就把目标换成表里的 IP，
   再把首段数据拆成两段写出（1 字节 + 其余，中间停 10ms，开 TCP_NODELAY）。
4. 上游 socket 一律 `bindSocket` 到物理网络（WiFi/蜂窝），避免自己的连接
   被自己的 VPN 抓回去形成回环。

不做重传/拥塞控制之外的 TCP 特性：tun 这一侧是本地内存拷贝，几乎不丢包，
真丢包由应用自己重传；我们只补了 250ms 扫描重发，覆盖偶发丢段。

## 实测记录（模拟器 API 37）

逐个打开、截图确认（截图未随包分发）：

| 站点 | 结果 | 证据 |
|---|---|---|
| GitHub | ✅ 完整加载 | 搜索框、横幅、导航、图片全渲染 |
| CurseForge | ✅ 完整加载 | Hero、统计、图片、Cookie 提示全渲染 |
| Modrinth | ✅ 完整加载 | Logo、按钮、卡片全渲染 |
| 百度 | ✅ 完整加载 | 隧道对普通站点的透传正常 |
| Steam 商店 | ⚠️ 不可用（环境） | Akamai 的 `23.77.15.30` 等 IP 从桌面同样超时，非本应用问题 |
| pixiv / Steam 社区 / Discord | ❌ 做不到 | SNI 本身被封，见上 |

独立测试端（另一个 UID，真正走隧道）的原始输出：

```
[OK-TLS] 223.5.5.5 357B first=[HTTP/1.0 404 Not Found]
[OK]     223.5.5.5 157B first=[HTTP/1.0 404 Not Found]
```

## 关键实现细节（都是踩出来的）

| 现象 | 根因 | 修法 |
|---|---|---|
| 纯 HTTP 请求永久超时、上游从未被连接 | 触发条件只认 SNI，非 TLS 流量永远够不到 | 首字节判断 + 200ms 兜底 |
| 自己的连出连接被自己抓回去、无限回环 | `protect()` 返回 false | `Network.bindSocket()` 绑物理网络 |
| `ERR_SSL_BAD_RECORD_MAC_ALERT` | 写上游丢给线程池，两个数据段乱序入流 | 改为收包线程内同步顺序写 |
| 最后 410 字节永远发不出去 | TCP 校验和成对循环在奇数长度时越界 | 奇数尾字节按高字节+0 处理 |
| 连接建好后立刻 EOF | 数据在 connect 完成前到达，`c.out` 还是 null | `ready` 门闩，排队数据补写后才放行 |
| GitHub 一直握手超时 | 表里的 `57.155.223.240` 是 doh.pub 给的污染答案 | 改成阿里 DNS 的 `20.205.243.166`（已握手验证） |

## 安装

1. 把 `sniroute-3.9.6-android.apk` 传到手机（或 `adb install`）。
2. 允许「安装未知来源应用」。
3. 打开应用 → 点「启动」→ 允许 VPN 授权。
4. 状态行显示「运行中 · 已解析 N 次」即生效。

停止：点「停止」，或在系统设置里关掉这个 VPN。

## 签名

```
DN: CN=Kynarain, EMAILADDRESS=kynarain@kynarain.cn, O=Sniroute, C=CN
方案: APK Signature Scheme v2 + v3（minSdk 26，不需要 v1）
```

首次构建会在 `build\android\` 生成密钥库（密码在 `keystore.pass`）。**务必备份**：
丢了就无法给已安装旧版做覆盖升级。

## 构建

需要 Android SDK（`platforms/android-34` + `build-tools/34.0.0`）与 JDK 17。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-android.ps1
# 测试端（另一个 UID，用来验证隧道）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-android-test.ps1
```

不用 Gradle：应用无第三方依赖（连 AndroidX 都没有），直接用 SDK 自带的
`aapt2` / `d8` / `zipalign` / `apksigner`，链路短、可复现。
