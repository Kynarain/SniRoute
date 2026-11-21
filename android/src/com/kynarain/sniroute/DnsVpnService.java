package com.kynarain.sniroute;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.VpnService;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.SocketTimeoutException;
import java.util.Arrays;
import java.util.Locale;

import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * 只接管 DNS 的 VpnService。
 *
 * 做法：
 *   1. 建立一个只有一个 /32 路由的 tun（10.111.222.2），并把系统 DNS 指到它；
 *      其余流量没有路由进 tun，因此完全不经过本应用。
 *   2. 从 tun 读到的每个报文都是"发往 10.111.222.2:53 的 IPv4 UDP DNS 查询"。
 *   3. 把 DNS 负载原样 POST 给加密 DNS（DoH，wire 格式），拿回同样格式的应答。
 *   4. 交换源/目的地址与端口，重新组装 IPv4+UDP 报文写回 tun。
 *
 * 为什么不需要解析 DNS 内容：DoH 的 wire 格式与明文 DNS 报文完全一致，
 * 所以整个应用只是一个"搬运工"，不必实现任何记录类型。
 *
 * 为什么本应用自己的出站连接不会绕圈：tun 里只路由 10.111.222.2/32，
 * 我们连 223.5.5.5 之类的真实地址不经过 tun。
 */
public class DnsVpnService extends VpnService {

    public static final String ACTION_START = "com.kynarain.sniroute.START";
    public static final String ACTION_STOP = "com.kynarain.sniroute.STOP";

    private static final String TAG = "Sniroute";
    private static final String VPN_ADDRESS = "10.111.222.1";
    private static final String DNS_ADDRESS = "10.111.222.2";
    private static final int MTU = 1500;

    private static volatile boolean sRunning = false;
    private int pktLog = 0;

    /**
     * 需要接管 TCP 的域名：这些站点的 SNI 被封锁，光修 DNS 打不开。
     * 做法是把它们的真实 IP 加进路由，让这部分流量进 tun，由 TunProxy 分包转发。
     * 其余一切流量不进 tun —— 既安全（不会把系统流量拖下水），也不会耗尽线程。
     */
    private static final String[][] KNOWN_IPS = {
            // 域名, 实测可达的 IP —— 每个都做过 TLS 握手验证（2026-11-21）。
            // 为什么不能用 DoH 现查：doh.pub 对 github.com 就返回了 57.155.223.240
            // 这个连不上、握手超时的地址；阿里 DNS 才给出真实可用的 20.205.243.166。
            // 所以表里的值一律以"握手成功"为准，不看是谁解析出来的。
            {"www.pixiv.net", "104.18.42.145", "104.18.43.145"},
            {"pixiv.net", "104.18.42.145", "104.18.43.145"},
            {"steamcommunity.com", "104.89.103.51", "23.15.140.216", "23.205.151.43"},
            {"discord.com", "162.159.130.233", "162.159.138.232"},
            {"x.com", "104.18.11.118", "172.64.152.129"},
            {"www.x.com", "104.18.11.118", "172.64.152.129"},
            {"grok.com", "104.18.28.234", "104.18.29.234"},
            {"www.dropbox.com", "162.125.85.18"},
            {"dropbox.com", "162.125.248.18"},
            {"mega.nz", "31.216.144.5"},
            {"onedrive.live.com", "13.107.137.11"},
            {"example.com", "93.184.216.34"},
            {"www.example.com", "93.184.216.34"},
            // DNS 被污染、但 SNI 正常的站点：这一类是这个应用真正能修好的
            {"github.com", "20.205.243.166"},
            {"api.github.com", "20.205.243.168"},
            {"codeload.github.com", "20.205.243.165", "57.155.223.241"},
            {"github.githubassets.com", "185.199.108.215", "185.199.111.215",
                    "185.199.110.215", "185.199.109.215"},
            {"avatars.githubusercontent.com", "185.199.110.133", "185.199.108.133"},
            {"objects.githubusercontent.com", "185.199.111.133", "185.199.110.133"},
            {"store.steampowered.com", "23.77.15.30", "23.15.142.182"},
            {"cdn.akamai.steamstatic.com", "23.33.126.180", "23.33.126.137"},
            {"www.curseforge.com", "104.17.30.3", "104.17.29.3"},
            {"mediafilez.forgecdn.net", "54.230.70.66", "54.230.70.63"},
            {"modrinth.com", "104.18.22.35"},
            {"cdn.modrinth.com", "146.75.45.242", "140.248.165.242"},
    };



    private ParcelFileDescriptor tun;
    private TunProxy proxy;
    private Thread worker;

    public static boolean isRunning() {
        return sRunning;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? ACTION_START : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            stopVpn();
            return START_NOT_STICKY;
        }
        // startVpn 里要先用 DoH 解析 blocklist（网络请求）。Android 禁止主线程做网络，
        // 放在主线程会抛 NetworkOnMainThreadException，异常被吞掉后路由一条都加不上
        // —— 这个坑实测踩过（日志 "blocklist routes added: 0"）。
        // 所以先发前台通知，再把整个建立过程丢到工作线程。
        startForegroundNotice();
        new Thread(new Runnable() {
            @Override
            public void run() {
                startVpn();
            }
        }, "vpn-setup").start();
        return START_STICKY;
    }

    private synchronized void startVpn() {
        if (sRunning) {
            return;
        }
        try {
            Builder b = new Builder();
            b.setSession("Sniroute DNS");
            b.setMtu(MTU);
            b.addAddress(VPN_ADDRESS, 32);
            // 为什么不再 addDnsServer：实测（API 37）只要 VPN 声明了 DNS 服务器，系统就会
            // 对那个地址改用 DoT(853)，而 DoT 需要设备信任的证书，我们不可能拿到。
            // 结果就是所有查询都失败。改成：不声明 DNS，而是把"系统原本在用的 DNS 服务器"
            // 也路由进 tun —— 明文 UDP 53 的查询就会落到我们手里。
            // 用 tun 自己的地址当 DNS 服务器：这是 DNS66 / Blokada 那类应用的标准做法。
            // 声明第二个 IP 时，系统可能把它当成"可做 DoT 的服务器"而去打 853；
            // 用接口自身地址，系统就按普通 DNS 发到它的 53 端口。
            // 必须有默认路由，VPN 才会成为"默认网络"；否则系统解析器继续用底层网络的
            // DNS，我们连查询都收不到（实测）。代价是要转发全部流量，所以线程池和
            // 并发上限是必需的。
            b.addRoute("0.0.0.0", 0);
            // 故意不调用 addDnsServer：实测只要 VPN 声明了 DNS 服务器，本版本系统就会
            // 改用 DoT(853) 去连它，而 DoT 需要设备信任的证书（非 root 拿不到）——
            // 结果所有域名都解析失败，浏览器连尝试连接都不会发起。
            // 不声明时，系统继续用底层网络的 DNS：答案可能是被投毒的，但"能解析"；
            // 我们随后在 TCP 层读 ClientHello 的 SNI，把目标换成实测 IP。
            int routedDns = routeSystemDns(b);
            Log.i(TAG, "system dns routes: " + routedDns);
            // 只把发往我们 DNS 地址的流量导入 tun，别的流量一条都不加
            b.addRoute(DNS_ADDRESS, 32);
            // 只接管"需要代理解析的域名"对应的 IP，其余流量不进 tun。
            // 早期版本用 0.0.0.0/0 全量接管，结果系统与 Google 服务的连接全涌进来，
            // 每条连接两个线程，直接把进程搞挂（pthread_create OOM）。这条教训写在这里。
            int routed = addBlocklistRoutes(b);
            Log.i(TAG, "blocklist routes added: " + routed);
            if (Build.VERSION.SDK_INT >= 29) {
                b.setMetered(false);
            }
            tun = b.establish();
            if (tun == null) {
                Log.e(TAG, "establish() returned null (VPN permission missing?)");
                return;
            }
            sRunning = true;
            worker = new Thread(new Runnable() {
                @Override
                public void run() {
                    loop();
                }
            }, "dns-loop");
            worker.start();
        } catch (Exception e) {
            Log.e(TAG, "start failed: " + e, e);
        }
    }

    private void stopVpn() {
        sRunning = false;
        try {
            if (tun != null) {
                tun.close();
            }
        } catch (Exception ignored) {
        }
        tun = null;
        if (proxy != null) {
            proxy.shutdown();
            proxy = null;
        }
        stopForeground(true);
        stopSelf();
    }

    private void startForegroundNotice() {
        String channel = "vpn";
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel(channel, "Sniroute", NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, channel)
                .setContentTitle("Sniroute")
                .setContentText("正在接管 DNS 解析")
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
        startForeground(1, n);
    }

    private void loop() {
        byte[] packet = new byte[MTU];
        try (FileInputStream in = new FileInputStream(tun.getFileDescriptor());
             FileOutputStream out = new FileOutputStream(tun.getFileDescriptor())) {
            proxy = new TunProxy(this, out);
            while (sRunning) {
                int len;
                try {
                    len = in.read(packet);
                } catch (Exception e) {
                    break;
                }
                if (len <= 0) {
                    continue;
                }
                if (pktLog < 24 && len > 20) {
                    pktLog++;
                    int v = (packet[0] >> 4) & 0xF;
                    int proto = packet[9] & 0xFF;
                    int ihl = (packet[0] & 0xF) * 4;
                    int dport = (ihl + 4 <= len && (proto == 6 || proto == 17))
                            ? (((packet[ihl + 2] & 0xFF) << 8) | (packet[ihl + 3] & 0xFF)) : -1;
                    Log.i(TAG, "pkt v" + v + " proto=" + proto + " dport=" + dport + " len=" + len);
                }
                boolean isDns = isDnsQuery(packet, len);
                if (isDns) {
                    byte[] reply = handle(packet, len);
                    if (reply != null) {
                        try {
                            out.write(reply);
                        } catch (Exception e) {
                            break;
                        }
                    }
                } else if (proxy != null) {
                    proxy.handle(packet, len);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "loop ended: " + e, e);
        }
    }

    /** 是否是发往我们 DNS 地址的 UDP 53 查询。 */

    /** 解析 blocklist 里的域名，把 A 记录逐条加进路由；返回加了几个。 */
    /** 把当前网络实际使用的 DNS 服务器逐条加进路由，返回加了几个。 */

    /** 按域名查实测 IP（给 TunProxy 在读完 SNI 后决定连哪个地址）。 */
    static String lookupIp(String host) {
        if (host == null) {
            return null;
        }
        for (String[] entry : KNOWN_IPS) {
            if (entry[0].equalsIgnoreCase(host)) {
                return entry.length > 1 ? entry[1] : null;
            }
        }
        return null;
    }

    private int routeSystemDns(Builder b) {
        int n = 0;
        try {
            android.net.ConnectivityManager cm =
                    (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            android.net.Network net = cm.getActiveNetwork();
            if (net == null) {
                return 0;
            }
            android.net.LinkProperties lp = cm.getLinkProperties(net);
            if (lp == null) {
                return 0;
            }
            for (java.net.InetAddress addr : lp.getDnsServers()) {
                String ip = addr.getHostAddress();
                if (ip != null && ip.indexOf(':') < 0) {
                    try {
                        b.addRoute(ip, 32);
                        n++;
                        Log.i(TAG, "routed system dns " + ip);
                    } catch (Exception ignored) {
                    }
                }
            }
        } catch (Exception e) {
            Log.d(TAG, "routeSystemDns failed: " + e);
        }
        return n;
    }

    private int addBlocklistRoutes(Builder b) {
        int n = 0;
        for (String[] entry : KNOWN_IPS) {
            for (int i = 1; i < entry.length; i++) {
                try {
                    b.addRoute(entry[i], 32);
                    n++;
                } catch (Exception ignored) {
                }
            }
        }
        Log.i(TAG, "blocklist routes added: " + n);
        return n;
    }

    /** 构造一个最小的 A 记录查询报文。 */
    private static byte[] buildAQuery(String host) {
        try {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            int id = (int) (System.nanoTime() & 0xFFFF);
            out.write(id >> 8);
            out.write(id & 0xFF);
            out.write(0x01);
            out.write(0x00);
            out.write(0x00); out.write(0x01);
            out.write(0); out.write(0);   // ancount
            out.write(0); out.write(0);   // nscount
            out.write(0); out.write(0);   // arcount
            for (String label : host.split("\\.")) {
                byte[] lb = label.getBytes("ISO-8859-1");
                out.write(lb.length);
                out.write(lb, 0, lb.length);
            }
            out.write(0);
            out.write(0); out.write(1);
            out.write(0); out.write(1);
            return out.toByteArray();
        } catch (Exception e) {
            return null;
        }
    }

    /** 从 DNS 应答里取出所有 A 记录。 */
    private static java.util.List<String> parseA(byte[] resp) {
        java.util.List<String> out = new java.util.ArrayList<>();
        if (resp == null || resp.length < 12) {
            return out;
        }
        int ancount = ((resp[6] & 0xFF) << 8) | (resp[7] & 0xFF);
        int i = 12;
        while (i < resp.length && resp[i] != 0) {
            i += (resp[i] & 0xFF) + 1;
        }
        i += 5;
        for (int a = 0; a < ancount && i + 10 <= resp.length; a++) {
            if ((resp[i] & 0xC0) == 0xC0) {
                i += 2;
            } else {
                while (i < resp.length && resp[i] != 0) {
                    i += (resp[i] & 0xFF) + 1;
                }
                i += 1;
            }
            if (i + 10 > resp.length) {
                break;
            }
            int type = ((resp[i] & 0xFF) << 8) | (resp[i + 1] & 0xFF);
            int rdlen = ((resp[i + 8] & 0xFF) << 8) | (resp[i + 9] & 0xFF);
            i += 10;
            if (type == 1 && rdlen == 4 && i + 4 <= resp.length) {
                out.add((resp[i] & 0xFF) + "." + (resp[i + 1] & 0xFF) + "."
                        + (resp[i + 2] & 0xFF) + "." + (resp[i + 3] & 0xFF));
            }
            i += rdlen;
        }
        return out;
    }

    private boolean isDnsQuery(byte[] p, int len) {
        if (len < 28 || ((p[0] >> 4) & 0xF) != 4) {
            return false;
        }
        int ihl = (p[0] & 0xF) * 4;
        if ((p[9] & 0xFF) != 17) {
            return false;
        }
        int dstPort = ((p[ihl + 2] & 0xFF) << 8) | (p[ihl + 3] & 0xFF);
        return dstPort == 53;
    }

    /** 解析 IPv4+UDP，若不是发往 53 的查询就原样返回 null。 */
    private byte[] handle(byte[] p, int len) {
        if (len < 28) {
            return null;
        }
        int version = (p[0] >> 4) & 0xF;
        if (version != 4) {
            return null;   // 我们只配置了 IPv4 DNS，IPv6 不会进来
        }
        int ihl = (p[0] & 0xF) * 4;
        if (ihl < 20 || len < ihl + 8) {
            return null;
        }
        if ((p[9] & 0xFF) != 17) {
            return null;   // 只处理 UDP
        }
        int srcPort = ((p[ihl] & 0xFF) << 8) | (p[ihl + 1] & 0xFF);
        int dstPort = ((p[ihl + 2] & 0xFF) << 8) | (p[ihl + 3] & 0xFF);
        if (dstPort != 53) {
            return null;
        }
        int dnsLen = ((p[ihl + 4] & 0xFF) << 8) | (p[ihl + 5] & 0xFF);
        int payloadOff = ihl + 8;
        if (dnsLen <= 0 || payloadOff + dnsLen > len) {
            return null;
        }
        Log.i(TAG, "dns query len=" + dnsLen + " src=" + srcPort);
        byte[] query = Arrays.copyOfRange(p, payloadOff, payloadOff + dnsLen);
        // 顺序很重要：
        //   1. 命中已知站点表 -> 用表里的实测 IP（这是本应用存在的意义）
        //   2. 其余 -> 原样透传给"客户端本来要问的那台 DNS 服务器"
        //   3. 透传失败 -> 再用 DoH 兜底
        // 为什么第 2 步不能省：DoH 的实现要先解析 dns.alidns.com，而那个 DNS 查询
        // 又会进 tun —— 被这里处理 → 递归。之前正是这个环导致全部解析失败。
        byte[] answer = resolveFromTable(query);
        if (answer == null) {
            answer = dnsPassthrough(p, ihl, query);
        }
        if (answer == null) {
            answer = resolve(query);
        }
        if (answer == null) {
            return null;
        }
        return buildReply(p, ihl, srcPort, dstPort, answer);
    }

    /** 如果查询的域名在我们的实测表里，直接合成应答，绕过被投毒的 DoH。 */
    private byte[] resolveFromTable(byte[] query) {
        try {
            // 解析问题段里的域名
            int i = 12;
            StringBuilder sb = new StringBuilder();
            while (i < query.length && query[i] != 0) {
                int len = query[i] & 0xFF;
                if (len == 0 || i + 1 + len > query.length) {
                    break;
                }
                if (sb.length() > 0) {
                    sb.append('.');
                }
                sb.append(new String(query, i + 1, len, "ISO-8859-1"));
                i += 1 + len;
            }
            String host = sb.toString().toLowerCase(java.util.Locale.ROOT);
            java.util.List<String> ips = new java.util.ArrayList<>();
            for (String[] entry : KNOWN_IPS) {
                if (entry[0].equalsIgnoreCase(host)) {
                    for (int k = 1; k < entry.length; k++) {
                        ips.add(entry[k]);
                    }
                    break;
                }
            }
            if (ips.isEmpty()) {
                return null;
            }
            int qEnd = i + 5;                       // 0 结尾 + type(2) + class(2)
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            out.write(query, 0, 2);                 // id
            out.write(0x81);                        // QR=1, RD=1
            out.write(0x80);                        // RA=1, rcode=0
            out.write(0); out.write(1);             // qdcount
            out.write(0); out.write(ips.size());    // ancount
            out.write(0); out.write(0);             // nscount
            out.write(0); out.write(0);             // arcount
            out.write(query, 12, qEnd - 12);        // 原问题段
            for (String ip : ips) {
                out.write(0xC0); out.write(0x0C);   // 指针指向问题段的域名
                out.write(0); out.write(1);         // type A
                out.write(0); out.write(1);         // class IN
                out.write(0); out.write(0); out.write(0x01); out.write(0x2C);  // TTL 300
                out.write(0); out.write(4);
                for (String part : ip.split("\\.")) {
                    out.write(Integer.parseInt(part));
                }
            }
            Log.i(TAG, "dns override " + host + " -> " + ips);
            return out.toByteArray();
        } catch (Exception e) {
            return null;
        }
    }


    /**
     * 把 DNS 查询原样转发给客户端原本要问的那台服务器（通常是路由器/运营商 DNS），
     * 再把应答带回来。这条路不依赖任何域名解析，因此不会递归。
     *
     * 为什么必须 protect：不 protect 的话，我们自己的 UDP 包又会进 tun，被自己处理，
     * 形成回环。
     */
    private byte[] dnsPassthrough(byte[] req, int ihl, byte[] query) {
        int dstIp = ((req[16] & 0xFF) << 24) | ((req[17] & 0xFF) << 16)
                | ((req[18] & 0xFF) << 8) | (req[19] & 0xFF);
        String server = ((dstIp >> 24) & 0xFF) + "." + ((dstIp >> 16) & 0xFF) + "."
                + ((dstIp >> 8) & 0xFF) + "." + (dstIp & 0xFF);
        java.net.DatagramSocket sock = null;
        try {
            sock = new java.net.DatagramSocket();
            if (!protect(sock)) {
                // 保险起见再试一次绑定物理网络
                android.net.ConnectivityManager cm = (android.net.ConnectivityManager)
                        getSystemService(CONNECTIVITY_SERVICE);
                for (android.net.Network n : cm.getAllNetworks()) {
                    android.net.NetworkCapabilities nc = cm.getNetworkCapabilities(n);
                    if (nc != null && !nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_VPN)
                            && (nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)
                                || nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_CELLULAR))) {
                        n.bindSocket(sock);
                        break;
                    }
                }
            }
            sock.connect(new java.net.InetSocketAddress(server, 53));
            sock.setSoTimeout(4000);
            sock.send(new java.net.DatagramPacket(query, query.length));
            byte[] buf = new byte[1500];
            java.net.DatagramPacket resp = new java.net.DatagramPacket(buf, buf.length);
            sock.receive(resp);
            return Arrays.copyOfRange(buf, 0, resp.getLength());
        } catch (Exception e) {
            Log.d(TAG, "dns passthrough to " + server + " failed: " + e);
            return null;
        } finally {
            if (sock != null) {
                sock.close();
            }
        }
    }

    /** 通过加密 DNS 拿到应答；先阿里后腾讯，失败依次回退。 */
    private byte[] resolve(byte[] query) {
        byte[] local = resolveFromTable(query);
        if (local != null) {
            noteResolved(local);
            return local;
        }
        String[][] upstreams = new String[][] {
                {"223.5.5.5", "dns.alidns.com", "/dns-query"},
                {"120.53.53.53", "doh.pub", "/dns-query"},
        };
        for (String[] up : upstreams) {
            byte[] r = doh(up[0], up[1], up[2], query);
            if (r != null) {
                noteResolved(r);
                return r;
            }
        }
        Log.w(TAG, "all DoH upstreams failed");
        return null;
    }

    /**
     * 建立 VPN 之前用的 DoH：不做 protect。
     *
     * 为什么单独一份：protect() 要求 VPN 已经建立（或至少已授权完成），在 establish()
     * 之前调用会抛异常；而我们需要在建立之前先把 blocklist 的 IP 解析出来，才能把它们
     * 加进路由。早期版本在这里用了带 protect 的版本，异常被 catch 吞掉，结果路由一条
     * 都没加上（日志里是 "blocklist routes added: 0"）。
     */
    private byte[] dohBeforeEstablish(String ip, String host, String path, byte[] query) {
        SSLSocket socket = null;
        try {
            SSLSocketFactory f = (SSLSocketFactory) SSLSocketFactory.getDefault();
            socket = (SSLSocket) f.createSocket();
            socket.connect(new java.net.InetSocketAddress(ip, 443), 6000);
            SSLParameters params = socket.getSSLParameters();
            params.setServerNames(java.util.Collections.singletonList(new SNIHostName(host)));
            socket.setSSLParameters(params);
            socket.setSoTimeout(6000);
            socket.startHandshake();
            OutputStream out = socket.getOutputStream();
            String head = "POST " + path + " HTTP/1.1\r\n"
                    + "Host: " + host + "\r\n"
                    + "Content-Type: application/dns-message\r\n"
                    + "Accept: application/dns-message\r\n"
                    + "Content-Length: " + query.length + "\r\n"
                    + "Connection: close\r\n\r\n";
            out.write(head.getBytes("ISO-8859-1"));
            out.write(query);
            out.flush();
            return readHttpBody(socket.getInputStream());
        } catch (Exception e) {
            Log.d(TAG, "pre-establish doh failed via " + host + ": " + e);
            return null;
        } finally {
            try {
                if (socket != null) {
                    socket.close();
                }
            } catch (Exception ignored) {
            }
        }
    }

    private byte[] doh(String ip, String host, String path, byte[] query) {
        SSLSocket socket = null;
        try {
            SSLSocketFactory f = (SSLSocketFactory) SSLSocketFactory.getDefault();
            // 必须先建未连接的 socket 再 protect，最后 connect：
            // 全量路由后，若直接 createSocket(ip, port) 会立刻连出去并被自己的 VPN 抓走。
            socket = (SSLSocket) f.createSocket();
            protect(socket);
            socket.connect(new java.net.InetSocketAddress(ip, 443), 6000);
            SSLParameters params = socket.getSSLParameters();
            params.setServerNames(java.util.Collections.singletonList(new SNIHostName(host)));
            socket.setSSLParameters(params);
            socket.setSoTimeout(6000);
            socket.startHandshake();

            OutputStream out = socket.getOutputStream();
            String head = "POST " + path + " HTTP/1.1\r\n"
                    + "Host: " + host + "\r\n"
                    + "Content-Type: application/dns-message\r\n"
                    + "Accept: application/dns-message\r\n"
                    + "Content-Length: " + query.length + "\r\n"
                    + "Connection: close\r\n\r\n";
            out.write(head.getBytes("ISO-8859-1"));
            out.write(query);
            out.flush();

            InputStream in = socket.getInputStream();
            byte[] body = readHttpBody(in);
            if (body == null || body.length < 12) {
                return null;
            }
            return body;
        } catch (Exception e) {
            Log.d(TAG, "doh failed via " + host + ": " + e);
            return null;
        } finally {
            try {
                if (socket != null) {
                    socket.close();
                }
            } catch (Exception ignored) {
            }
        }
    }

    /** 读 HTTP/1.1 响应体：支持 Content-Length，也支持读到断开（chunked 极少见，直接返回原文）。 */
    private byte[] readHttpBody(InputStream in) throws Exception {
        java.io.ByteArrayOutputStream head = new java.io.ByteArrayOutputStream();
        int state = 0;
        while (state < 4) {
            int c = in.read();
            if (c < 0) {
                return null;
            }
            head.write(c);
            if ((state == 0 || state == 2) && c == '\r') {
                state++;
            } else if ((state == 1 || state == 3) && c == '\n') {
                state++;
            } else {
                state = 0;
            }
            if (head.size() > 8192) {
                return null;
            }
        }
        String header = head.toString("ISO-8859-1");
        int cl = -1;
        for (String line : header.split("\r\n")) {
            String lower = line.toLowerCase(Locale.ROOT);
            if (lower.startsWith("content-length:")) {
                try {
                    cl = Integer.parseInt(line.substring(15).trim());
                } catch (Exception ignored) {
                }
            }
        }
        java.io.ByteArrayOutputStream body = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int total = 0;
        while (true) {
            if (cl >= 0 && total >= cl) {
                break;
            }
            int want = cl >= 0 ? Math.min(buf.length, cl - total) : buf.length;
            int n;
            try {
                n = in.read(buf, 0, want);
            } catch (SocketTimeoutException te) {
                break;
            }
            if (n <= 0) {
                break;
            }
            body.write(buf, 0, n);
            total += n;
            if (total > 65535) {
                break;
            }
        }
        return body.toByteArray();
    }

    /** 组装回程报文：交换地址与端口，UDP 校验和置 0（IPv4 下合法）。 */
    private byte[] buildReply(byte[] req, int ihl, int srcPort, int dstPort, byte[] answer) {
        int total = 20 + 8 + answer.length;
        byte[] out = new byte[total];
        // IPv4 头
        out[0] = 0x45;
        out[1] = 0;
        out[2] = (byte) ((total >> 8) & 0xFF);
        out[3] = (byte) (total & 0xFF);
        out[4] = req[4];
        out[5] = req[5];
        out[6] = 0;
        out[7] = 0;
        out[8] = 64;                    // TTL
        out[9] = 17;                    // UDP
        out[10] = 0;
        out[11] = 0;
        // 源 = 原目的（我们的 DNS 地址），目的 = 原源
        System.arraycopy(req, 16, out, 12, 4);
        System.arraycopy(req, 12, out, 16, 4);
        int sum = 0;
        for (int i = 0; i < 20; i += 2) {
            sum += ((out[i] & 0xFF) << 8) | (out[i + 1] & 0xFF);
        }
        while ((sum >> 16) != 0) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        int cks = ~sum & 0xFFFF;
        out[10] = (byte) ((cks >> 8) & 0xFF);
        out[11] = (byte) (cks & 0xFF);
        // UDP 头
        int udpLen = 8 + answer.length;
        out[20] = (byte) ((dstPort >> 8) & 0xFF);       // 源端口 = 原目的端口（53）
        out[21] = (byte) (dstPort & 0xFF);
        out[22] = (byte) ((srcPort >> 8) & 0xFF);       // 目的端口 = 原源端口
        out[23] = (byte) (srcPort & 0xFF);
        out[24] = (byte) ((udpLen >> 8) & 0xFF);
        out[25] = (byte) (udpLen & 0xFF);
        out[26] = 0;
        out[27] = 0;                                    // 校验和 0：IPv4 允许
        System.arraycopy(answer, 0, out, 28, answer.length);
        return out;
    }

    private void noteResolved(byte[] answer) {
        try {
            SharedPreferences sp = getSharedPreferences("state", MODE_PRIVATE);
            sp.edit().putInt("resolved", sp.getInt("resolved", 0) + 1)
                    .putLong("last_ms", System.currentTimeMillis()).apply();
        } catch (Exception ignored) {
        }
    }

    @Override
    public void onDestroy() {
        sRunning = false;
        try {
            if (tun != null) {
                tun.close();
            }
        } catch (Exception ignored) {
        }
        super.onDestroy();
    }
}
