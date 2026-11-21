sniroute 使用说明
========================================

这是什么
--------
一个本地小工具：把 pixiv 主站与图片站的访问接回本机处理，
主站用浏览器原生 ECH 绕过按域名（SNI）的封锁，图片站走本机代理。
**不需要管理员权限**：证书装进当前用户的受信任根，不改系统 hosts。

安装
----
两种形态，任选一种：

  Sniroute-3.0.5.msi   双击安装。可以自己选安装目录；只装给当前用户，
                       全程不会弹 UAC。装完桌面和开始菜单各有一个快捷方式，
                       控制面板里可正常卸载。
  Sniroute\ 文件夹      解压即用的绿色版，双击里面的 Sniroute.exe。

支持的网站
--------
  pixiv    主站、API 与图片站都由本工具接管。
  google   搜索、账号、Gmail、Gemini、YouTube 等，同样由本机接管。
  github   只修正解析：DNS 被污染时换用 DoH 的真实地址，并对给出的地址做
           一次 TCP 探活，剔除连不上的。证书仍是 GitHub 自己的，不做中间人。
  discord  页面、gateway、头像/表情/附件、更新检查都可用：
             · discord.com / gateway → 本机终结后转发（补齐了 Cloudflare 要求的导航头）
             · cdn.discordapp.com / media.discordapp.net / updates.discord.com
               → 走 ECH：本机 DNS 下发 Cloudflare 的共享 ECH 配置
                 （cloudflare-ech.com，每次启动现查，避免密钥轮换后失效），
                 浏览器自己加密 SNI，服务端仍能正确路由
             · 这条路不经过本机代理，只对使用本工具 DoH 的浏览器生效
           Discord 客户端的更新下载（stable.dl2.discordapp.net、dl.discordapp.net）
           无法支持：前者在 Google Cloud 上、带 SNI 被静默丢包，后者所在的
           Cloudflare zone 不支持 ECH。这两个域名被指到 127.0.0.2，让连接
           立刻被拒绝（约 0.5 秒），而不是对着被丢包的连接干等二三十秒。
  youtube  页面、搜索、缩略图、评论可用（youtube.com / youtu.be / i.ytimg.com /
           yt3.ggpht.com 都靠 Host 头路由到 142.251.x.2 这一族）。
           **视频播放不可用**：视频流在 rr*.googlevideo.com 上，这些服务器
           必须靠 SNI 选路，而带 SNI 的连接会被静默丢包（实测 TCP 通、TLS 无响应），
           它们也不支持 ECH，80 端口同样不通。所以播放器会停在转圈并反复 404。
  steam    商店（store.steampowered.com）用 dns 模式，只修解析：浏览器直连，
           证书仍是 Steam 的真证书。实测打开耗时 4.8-6.1 秒，标题「欢迎来到 Steam」；
           cdn.akamai.steamstatic.com 的图片实测加载成功（IMG_OK）。
           社区（steamcommunity.com）用 loopback：本机终结 TLS、上游不发 SNI。
           带 SNI 的连接会被重置，不发 SNI 返回 200；上游 104.89.103.51 /
           23.15.140.216 / 23.205.151.43 各测 3 次全部成功。
           已知限制：社区页会加载第三方资源（如 community.steamstatic.com），
           其中一部分在当前网络里会挂到超时；页面内容能出来，但「加载完成」
           可能很慢，这部分还在继续排查。
  curseforge
           用 dns 模式（13 个域名），只修解析：真实浏览器实测 200，标题
           「CurseForge - Mods & Addons Leading Community」2.16 秒；/minecraft
           200，标题「Minecraft Mods & Modpacks - CurseForge」1.65 秒。
           文件下载实测可用：mediafilez.forgecdn.net 返回 206、
           Content-Type: application/java-archive、Content-Range:
           bytes 0-131071/7091096，开头 504b0304。
           注意：
             · Cloudflare 的人机挑战页会让裸客户端拿到 403（cf-mitigated:
               challenge），这不是被墙——真实浏览器会自动通过，无头浏览器会
               一直停在「请稍候…」。
             · forgecdn 必须真 SNI（CloudFront 无 SNI 直接握手失败），不能放进 loopback。
             · api.curseforge.com 网络可达，但需要 API key（无 key 403）。
  cloudflare
           用 dns 模式（14 个域名），只修解析：www.cloudflare.com 实测 200，
           标题「Cloudflare: Build for ...」；cdnjs.cloudflare.com 的 JS 实际
           拉取成功（94 KB）。dash.cloudflare.com 稳定 403，是 Cloudflare
           自己的人机判定，不是被墙。one.one.one.one（1.1.1.1）与
           cloudflare-dns.com 在本网络不可用，未纳入；pages.dev 可直连（SNI 正常）。
  onedrive 用 loopback，本机终结 TLS、上游不带 SNI、靠 Host 路由（同 Steam 社区）。
           onedrive.live.com 实测 15.3 秒正常打开。
  mega     用 loopback（同上）。mega.nz 实测 10.3 秒正常打开；MEGA 边缘 TLS
           握手要 9-12 秒，所以这个站点单独放宽了超时。
  dropbox  用 loopback（同上）。www.dropbox.com 实测 13.2 秒正常。
           明确不可用：dl.dropboxusercontent.com（下载 CDN）——带 SNI 被重置，
           不带 SNI 只回兜底 404，且不认 Host。
  roblox   用 dns 模式，能连上，但服务端按地区限制：www.roblox.com 会 301 到
           www.roblox.cn/home，而该域名返回 404（Roblox 自己的中国站已失效）；
           腾讯 roblox.qq.com 是 200，但那是另一个产品。这不是网络问题。
  nexusmods
           用 dns 模式：带真 SNI 正常；www 的 403 是 Cloudflare 的人机挑战，
           不是被墙。staticdelivery 的静态图片能拉到真实字节。
           files.nexus-cdn.com 的 mod 文件下载需要 API key 签发的签名 URL，
           无 key 会被 302 到失效页（应用层限制）。
  modrinth 用 dns 模式：站点在 Cloudflare、下载 CDN 在 Fastly，都要求真 SNI。
           实测拉取一个真实 jar 共 1,211,839 字节，SHA-512 与 API 给的值一致；
           API 公开，不需要 key。
  mod.io   用 ech 模式——唯一需要 ECH 的站点：它的域名被针对性 SNI 阻断，
           同一 IP 上换别的域名正常，只有 mod.io 的 SNI 在 200 毫秒内被重置；
           不带 SNI 又会被 Cloudflare 拒绝。实测走共享 ECH 后页面 200、
           178 KB 正常。API 需要 key。
  ea       用 dns 模式，只修解析：www.ea.com 的 35 个域名里 18 个可用，全部带
           真 SNI 正常、没有 ECH，所以不需要本机代理。
           **解析已修好，但 Chrome 仍有协议错误**：实测报
           net::ERR_HTTP2_PROTOCOL_ERROR；强制 h1.1 会变成 ERR_CONNECTION_RESET，
           而 curl（schannel 栈）能拿到 200。这是客户端与 Akamai 边缘的协议
           协商问题，DNS 层改不了；也试过用本机 nginx 改写上游协议，结果 504，
           未采用。
  bilibili 国际版（bilibili.tv）用 dns 模式，只修解析：相关 15 个域名带真 SNI
           全部正常（没有 SNI 阻断、也没有 ECH），JS / PNG 都能拉到真字节。
           但 www.bilibili.tv/ 返回 200 + Content-Length: 0（空白页），播放接口
           返回 {"code":10003003,"message":"The area is inaccessible"}——本机
           出口 IP 的地区限制，网络层解决不了。结论：网络全通，视频因地区限制
           看不了。
  twitch   混合模式：直播可看，VOD / 剪辑 / 登录不可用。
           直播：*.ttvnw.net 在 CloudFront 上带真 SNI 正常，实测拉到真实直播
           分片 200 / 1,870,914 字节，走 dns 模式。
           页面/表情：www 等 *.twitch.tv 域名带 SNI 被重置、不带 SNI 反而通，所以走
           loopback（本机终结 TLS、上游不发 SNI）；static-cdn.jtvnw.net 走 dns。
           不可用：VOD / 剪辑（vod-secure / clips-media-assets2 所在的 CloudFront
           边缘 18.65.14.x 是 IP 级封锁，换任何 SNI 都重置）、登录相关的
           api / passport / id。
           **ttvnw.net 绝不能放进 loopback**：CloudFront 强制要求 SNI，去掉就
           握手失败，直播会全断。已知：页面里的 assets.twitch.tv 静态资源仍偶发
           上游超时。
  x        混合模式（原 Twitter）。
           前门：x.com / www.x.com / api.x.com / t.co 走 loopback（本机终结
           TLS），上游发**带根点的 SNI**（x.com.），Host 原样透传。为什么
           不用 ECH：x.com 的 Cloudflare zone 不接受共享 ECH 配置，实测回
           403（浏览器表现为 ERR_HTTP_RESPONSE_CODE_FAILURE）。为什么要带
           根点：GFW 拦的是精确域名，ClientHello 里写 "x.com." 不被拦（同 IP
           交错对照：x.com 被 RST / x.com. 回 200，7 个候选里 6 个 3/3 成功）；
           配置里就是 proxy_ssl_name $host.（末尾补根点）。twitter.com 等旧
           域名即使带根点也被重置，未纳入。
           twimg：pbs / abs / video.twimg.com 在 Fastly 上，精确 SNI 被重置、
           不发 SNI 被 400；解法是把上游 SNI 换成 Fastly 的通用映射名
           twimg.twitter.map.fastly.net，Host 透传（实测
           abs.twimg.com/favicons/twitter.ico 200、912 字节，video 的 m3u8
           以 #EXTM3U 开头）。三个 twimg 域名必须一起接管，否则 X 页面没有样式。
           重要坑：配置里**故意不配 upstream keepalive 并关掉
           proxy_ssl_session_reuse**，否则 nginx 会复用跨域名的 TLS 连接/会话，
           Cloudflare 约一半请求回 421 Misdirected Request。
  grok     loopback，和 X 同一套手法（带根点的 SNI）：grok.com 与 x.ai 的
           上游 SNI 也补根点，Host 保持原样。实测 grok.com 200、页面 235 KB、
           标题 Grok；x.ai 200。精确 SNI 在 27-40 毫秒内被重置，不带 SNI 又
           会被 Cloudflare 拒绝，所以不能 proxy_ssl_server_name off。上游硬编码
           104.18.28.234 / 104.18.29.234 / 104.18.18.80；104.18.19.80 是黑洞，
           必须排除。
  openai   dns 模式，只修解析：openai.com / www / platform / auth /
           cdn.oaistatic.com / cdn.openai.com 带真 SNI 正常。返回的 403 是
           Cloudflare 的 WAF / 人机挑战，不是被墙；静态资源 cdn.oaistatic.com
           实测能拿到真实 JS 字节。实测 openai.com 页面 200、标题
           「OpenAI | 研究与部署」。

  未加入 Spotify：音频 CDN 网络是通的（实测拉到真实 MP3 344,502 字节），
  但网页版返回 451 地区限制，加了也没用。
  hCaptcha / Arkose Labs 探测过，本网络没有被封，不需要接管；
  Arkose 服务端强制 SNI，绝不能走 loopback。
  未加入 ChatGPT：chatgpt.com 的 SNI 被封、不带 SNI 也被 Cloudflare 拒绝
  （该 IP 集强制 SNI），加上 DNS 污染，只有 ECH 可能。实测 ECH 握手确实成立
  （能拿到 Cloudflare 渲染的页面），但页面内容是「Unable to load site ...
  If you are using a VPN, try turning it off. [IP:<redacted-ip>]」——OpenAI
  按出口 IP 拦截大陆地址，网络层解决不了，所以没有加入菜单（避免放一个
  打不开的项）。


             pixiv 主站  走浏览器原生 ECH（只有加密 DNS 能携带 ECH 配置）
             pixiv 图片  由本机终结 TLS 后转发，因此对所有程序都生效
             google      本机终结 + 上游不发 SNI，靠 Host 头路由（实测搜索/账号/Gmail/Gemini 均可用）

  要增加别的网站，改 data\sites.json 后重新打包。是否需要本机代理取决于
  对方对"无 SNI 连接"的态度，不是纯配置问题。
第一次使用
----------
1. 程序启动后**立即接管**：自动拉起 DoH 与 nginx，并默认注册开机自启动
   （托盘菜单里可以取消）。任务栏时钟旁边会出现一个图标（绿=正常，红=有服务没起），不会弹窗口。
   首次启动会自动把根证书装进**当前用户**的受信任根存储，不需要管理员、不会弹 UAC。

2. 浏览器不需要手动设置。

   首次启动会自动把 Chrome 的安全 DNS 指向 https://127.0.0.1:5444/dns-query，
   通过注册表策略下发，重启浏览器生效，对所有 Chrome 配置都有效。

   如果这台机器的用户策略键被加固（HKCU\Software\Policies 只有读权限），
   程序会提示"需要一次管理员授权"：这时右键托盘图标 →「配置浏览器（DoH）」，
   点一次 UAC 即可。普通机器不会走到这一步。

3. 右键托盘图标 →「启动全部」。

日常使用
--------
右键托盘图标：

  启动全部 / 停止全部    起停后台服务（DoH + nginx）
  刷新状态               气泡显示 DoH / nginx / hosts / 证书 状态
  检测 NAT 类型          弹窗显示本机 NAT 类型（RFC 5780）
  打开浏览器             用独立的浏览器配置启动（带 ECH 参数）
  安装证书/hosts         重新执行一次证书安装（一般不需要）
  配置浏览器（DoH）      把 Chrome 安全 DNS 指向本机（策略下发，可撤销）
  全局模式（DNS 接管）   用 NRPT 让系统解析器把域名交给本机，所有程序生效（需一次 UAC）
  支持的网站（子菜单）   列出本工具接管的网站，点一下直接打开
  查看日志               出问题时看 logs\gui.log
  开机自动启动           写入当前用户的启动项，不需要管理员
  还原                   移除证书，并把浏览器安全 DNS 改回"关闭"后即可卸载
  退出                   结束托盘程序

检测 NAT 类型

上游地址自动刷新
----------------
loopback 模式的上游 IP 是实测后写死的（conf/upstreams/*.list），CDN 调度一变就可能失效。
现在它们会自动校准，不需要你知道有这回事：

  · 每次启动：后台跑一次检查，不拖慢启动
  · 每 45 分钟：托盘在后台再校准一次
  · 手动：托盘菜单「刷新上游地址」，或 sniroute.cmd refresh（--dry-run 只看不改）

判定标准是**真握手**而不是 HTTP 状态码：这些站点对陌生路径回 403/404 是常态，
拿状态码当标准会把好地址误判成坏的。同一地址连测 3 次、至少 2 次拿到 HTTP 响应
才算健康。

三种结果，处理方式不同：
  · 健康：保留
  · TCP 能连但 TLS 失败：降级保留、排到后面（可能是被 Fastly/Akamai 临时限速，
    删掉反而会把站点打挂）
  · 连 TCP 都不通：剔除

一个地址都不健康时保持原文件不动 —— 宁可留着旧地址让 nginx 慢慢试，
也不清空配置把站点打死。
-------------
  用途：判断当前网络属于哪种 NAT：公网直连、全锥、受限锥、端口受限锥或对称。

  原理：RFC 5780，两条行为相乘：
    映射行为  同一本地端口打不同目标，公网映射地址/端口是否变化
    过滤行为  外部能否主动从别的地址/端口打进来
  单看一条会误判，两条组合才能分出锥型细分、对称型与公网直连。

  用法：
    托盘菜单 →「检测 NAT 类型」，弹窗显示结论；
    sniroute.cmd nat，可加 --summary / --json / --server / --server2 /
    --timeout-ms。

  默认 STUN 服务器是 stun.miwifi.com:3478：Google 的 stun.l.google.com 在本网络
  解析到 127.0.0.1（DNS 被污染），不可用；Cloudflare 与 Nextcloud 作备选。

  本机实测：全锥型 NAT（Full Cone），映射 <redacted-ip> 稳定，过滤端点无关。

  注意：HTTP 反查的公网 IP 可能与 STUN 映射地址不同（本机实测 HTTP 走代理
  203.131.255.202、UDP 直连 <redacted-ip>）。报告里会写明「以 STUN 为准」。

命令行（可选，和托盘等价）
--------------------------
  sniroute.cmd up / down / status     起停与状态
  sniroute.cmd verify                 经代理发真实请求，判断站点能不能用
  sniroute.cmd stats                  看真实响应耗时分布
  sniroute.cmd check                  环境自检
  sniroute.cmd nat                    检测 NAT 类型（RFC 5780；可选 --summary / --json 等）
sniroute.cmd refresh                检查并刷新上游候选地址（可加 --dry-run / --quiet）
  sniroute.cmd site <域名>            判断某个站能不能被本工具支持，以及该用哪种模式
                                      （TCP 不通 = 无解；只封 SNI = 可做；有 ech = 走浏览器 ECH）

中继（可选，需要墙外服务器）
----------------------------
  默认关闭（data\relay.json）。用途：YouTube 视频、Twitch VOD/剪辑/登录、
  Discord 客户端更新——这些在同一条网络里没有直连出路。

  原理：nginx 发真实 SNI → tools\relay.py 从 ClientHello 读主机名 → SOCKS5
  （ssh -D）→ 目标 443 → 字节透传；SNI 只在隧道内部出现。

  **服务器必须在墙外：没有境外服务器时，装上本工具也不会让这些站点可用。**
  实测阿里云武汉 ECS（cn-wuhan-lr）拿到真实 IP 访问 Google 仍是 15 秒超时，
  不能当中继。拿到境外服务器后：改 data\relay.json 的 enabled=true 与
  ssh.host / key，再执行 sniroute.cmd relay start。

  命令：sniroute.cmd relay status / start / stop
  未启用时相关域名快速返回 502，其余站点不受影响。

卸载
----
安装目录里有 **uninstall.cmd**，双击即可，它会按顺序做三件事：

  1. 停掉后台服务（DoH + nginx）
  2. 把根证书从当前用户的受信任根里移除
  3. 调用 Windows 安装器卸载程序，并清掉运行时产生的证书与日志

也可以在「设置 → 应用 → 已安装的应用 → sniroute → 卸载」卸载
（但这条路径只删程序，证书需要再双击一次 uninstall.cmd 才会移除）。

绿色版：双击 uninstall.cmd 后直接删文件夹。

卸载完成后，记得在浏览器里把安全 DNS 改回"关闭"——否则那个浏览器配置会失去解析。

注意事项
--------
- 填了安全 DNS 之后，这个浏览器配置的所有域名解析都走本工具。
  托盘退出后浏览器会解析失败——这不是故障，把安全 DNS 改回"关闭"即可。
- 根证书与叶子证书都是**每台机器各自生成**的（首次启动自动签发），不随包分发。
- 需要 Windows 10/11 64 位。托盘程序是原生 C++（不依赖 Java），包内自带 Python、nginx、openssl。
- 若 Windows 提示"已保护你的电脑"（SmartScreen），因为安装包没有代码签名：
  点"更多信息 → 仍要运行"。
- Chrome 后台的 optimizationguide-pa.googleapis.com（优化指南）会通过代理拉 5 MB
  （实测 201 秒），拖慢所有网页的加载；现已改为快速失败。
- dns 模式曾把"探活失败"当成"域名不存在"、返回空应答，导致 github.com 整站
  解析失败（浏览器 ERR_NAME_NOT_RESOLVED）；现在探活全失败时回退为返回上游
  原始答案，不再回空应答。

许可证：GPL-3.0（见安装目录里的 LICENSE）。随包的 nginx / openssl / python 各自许可见 licenses\ 目录，汇总见 THIRD_PARTY_LICENSES.txt。本工具只做域名解析与代理，不收集也不上传任何数据；日志只留域名与状态码，
不记录完整网址。

文件校验与签名
--------------
  安装包和程序都带自签名（发布者 Kynarain, O=Sniroute）：

    scripts\sign-and-hash.ps1 -Path dist\Sniroute-x.y.z.msi      # 构建时自动调用

  同时会生成 SHA256SUMS.txt 和同名 .sha256 文件，用来核对下载是否完整：

    certutil -hashfile Sniroute-3.5.2.msi SHA256
    # 与自己计算的结果和 SHA256SUMS.txt 对照

  在**别人的机器**上要看到"签名已验证"，需要先导入随包附带的 codesign.cer：

    certutil -addstore -user Root codesign.cer
    certutil -addstore -user TrustedPublisher codesign.cer

  说明两点，免得误解：
    · 自签名证书不会让 SmartScreen 变绿灯 —— 它只证明文件没被改过、发布者是谁，
      不提供商业证书那种"全球通用信任"。首次运行仍可能提示未知发布者。
    · 构建时没有加时间戳（这台机器到公共 TSA 不通）。代价是证书在 2031 年
      过期后，已签名的文件也会一起失去有效性。需要时可用
      sign-and-hash.ps1 -TimestampServer <可用的 TSA 地址> 重新签。

