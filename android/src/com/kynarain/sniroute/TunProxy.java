package com.kynarain.sniroute;

import android.net.VpnService;
import android.util.Log;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.HashMap;
import java.util.Map;

/**
 * 用户态 TCP 转发器（IPv4），按 SNI 决定连接目标。
 *
 * 为什么按 SNI 而不是靠 DNS：
 *   实测这台设备（API 37）上，系统解析器不肯把明文 DNS 发到 VPN 声明的服务器 ——
 *   它只用 DoT(853)，而 DoT 需要设备信任的证书（非 root 拿不到）。所以"修 DNS"
 *   这条路在标准权限下走不通。
 *
 *   但是 VPN 已经接管了 TCP 流量本身。TLS 的 ClientHello 里带着 SNI，
 *   我们在转发之前读一下它，就能知道用户到底要访问哪个域名 —— 于是可以：
 *     · 命中已知站点：连到我们实测过的真实 IP（不用管 DNS 给的毒地址）
 *     · 其余域名：照原样转发到原目的 IP
 *   这样 DNS 是否被污染都不再重要。
 *
 * 连接时机：收到 SYN 只回 SYN-ACK，等第一段数据到达、解析出 SNI 之后再向上游建连。
 *
 * 分包：把 ClientHello 拆成两段写出去（1 字节 + 其余，中间停 10ms，开 TCP_NODELAY），
 * 让 DPI 在第一个报文里看不到完整 SNI。会重组流的 DPI 依然能识别，效果不保证。
 *
 * 不做：重传、拥塞控制、窗口缩放、SACK、乱序重组。tun 这一侧是本地内存拷贝，
 * 几乎不丢包，真丢包由应用自己重传。
 */
public class TunProxy {

    private static final String TAG = "Sniroute";
    private static final int MSS = 1200;
    private static final int MAX_ACTIVE = 96;
    private static final int MAX_SNI_BYTES = 8192;

    private static final int FIN = 0x01;
    private static final int SYN = 0x02;
    private static final int RST = 0x04;
    private static final int PSH = 0x08;
    private static final int ACK = 0x10;

    private final VpnService vpn;
    private final OutputStream tunOut;
    private final Map<String, Conn> conns = new HashMap<>();
    private volatile boolean running = true;
    private final java.util.concurrent.ExecutorService pool =
            java.util.concurrent.Executors.newFixedThreadPool(24);
    private final java.util.concurrent.atomic.AtomicInteger active =
            new java.util.concurrent.atomic.AtomicInteger(0);
    private int connLog = 0;
    private int ipId = 1;

    private static final class Conn {
        Socket socket;
        OutputStream out;
        int srcIp, srcPort, dstIp, dstPort;
        long irs;
        long iss;
        long sndNxt;
        long sndUna;   // 客户端确认到哪
        // 已发出但未被确认的段（seq -> {data, sentAtMs, tries}）。
        // 之前完全没有重传：实测有一个 412 字节的段没被确认，握手就永久卡住。
        final java.util.LinkedHashMap<Long, Object[]> inflight = new java.util.LinkedHashMap<>();
        long rcvNxt;
        final java.io.ByteArrayOutputStream pending = new java.io.ByteArrayOutputStream();
        long bytesIn = 0;
        long bytesOut = 0;
        long bytesFromServer = 0;
        String closeReason = "?";
        boolean connected = false;
        volatile boolean ready = false;
        volatile long firstDataAt = 0;
        boolean firstData = true;
        volatile boolean closed;
    }

    public TunProxy(VpnService vpn, OutputStream tunOut) {
        this.vpn = vpn;
        this.tunOut = tunOut;
        startRetransmit();
    }

    public void shutdown() {
        running = false;
        pool.shutdownNow();
        synchronized (conns) {
            for (Conn c : conns.values()) {
                closeQuietly(c);
            }
            conns.clear();
        }
    }

    public boolean handle(byte[] p, int len) {
        if (len < 20 || ((p[0] >> 4) & 0xF) != 4) {
            return false;
        }
        int ihl = (p[0] & 0xF) * 4;
        if (ihl < 20 || len < ihl + 20) {
            return false;
        }
        int proto = p[9] & 0xFF;
        if (proto != 6) {
            return false;                      // UDP 由 DNS 那条路处理
        }
        return handleTcp(p, len, ihl);
    }

    private boolean handleTcp(byte[] p, int len, int ihl) {
        int srcPort = u16(p, ihl);
        int dstPort = u16(p, ihl + 2);
        long seq = u32(p, ihl + 4);
        long ack = u32(p, ihl + 8);
        int dataOff = ((p[ihl + 12] >> 4) & 0xF) * 4;
        int flags = p[ihl + 13] & 0x3F;
        int payloadOff = ihl + dataOff;
        int payloadLen = len - payloadOff;
        if (payloadLen < 0) {
            payloadLen = 0;
        }
        int srcIp = i32(p, 12);
        int dstIp = i32(p, 16);

        // DNS 端口不代理：53 = DNS over TCP，853 = DoT。让它们失败即可。
        if (dstPort == 53 || dstPort == 853) {
            if ((flags & SYN) != 0 && (flags & ACK) == 0) {
                sendTcp(p, ihl, srcIp, srcPort, dstIp, dstPort,
                        7777, (seq + 1) & 0xFFFFFFFFL, SYN | ACK, null, 0);
            } else {
                sendTcp(p, ihl, srcIp, srcPort, dstIp, dstPort, 7778, seq, RST | ACK, null, 0);
            }
            return true;
        }

        String key = key(srcIp, srcPort, dstIp, dstPort);
        Conn c0;
        synchronized (conns) {
            c0 = conns.get(key);
        }
        final Conn c = c0;
        if (c != null && (flags & ACK) != 0 && ack != 0) {
            c.sndUna = ack;
            synchronized (c.inflight) {
                java.util.Iterator<Long> it = c.inflight.keySet().iterator();
                while (it.hasNext()) {
                    long s = it.next();
                    Object[] v = c.inflight.get(s);
                    int seglen = ((byte[]) v[0]).length;
                    if (isAcked(s, seglen, ack)) {
                        it.remove();
                    }
                }
            }
        }

        if ((flags & SYN) != 0 && (flags & ACK) == 0) {
            Conn n = new Conn();
            n.srcIp = srcIp;
            n.srcPort = srcPort;
            n.dstIp = dstIp;
            n.dstPort = dstPort;
            n.irs = seq;
            n.rcvNxt = (seq + 1) & 0xFFFFFFFFL;
            n.iss = (System.nanoTime() & 0xFFFFFF) + 1;
            n.sndNxt = n.iss;
            n.sndUna = n.iss;
            synchronized (conns) {
                conns.put(key, n);
            }
            sendTcp(p, ihl, srcIp, srcPort, dstIp, dstPort,
                    n.iss, n.rcvNxt, SYN | ACK, null, 0);
            n.sndNxt = (n.iss + 1) & 0xFFFFFFFFL;
            return true;
        }

        if (c == null) {
            sendTcp(p, ihl, srcIp, srcPort, dstIp, dstPort,
                    (flags & ACK) != 0 ? ack : 0, 0, RST | ACK, null, 0);
            return true;
        }

        if ((flags & RST) != 0) {
            c.closeReason = "client RST";
            closeConn(key, c);
            return true;
        }
        if ((flags & FIN) != 0) {
            c.closeReason = "client FIN";
            if (payloadLen == 0 && c.rcvNxt == seq) {
                c.rcvNxt = (c.rcvNxt + 1) & 0xFFFFFFFFL;
            }
            closeConn(key, c);
            return true;
        }

        if (payloadLen > 0) {
            if (seq == c.rcvNxt) {
                if (c.pending.size() < 64) {
                    Log.i(TAG, "data in " + payloadLen + "B key=" + key);
                }
                c.rcvNxt = (c.rcvNxt + payloadLen) & 0xFFFFFFFFL;
                synchronized (c.pending) {
                    if (!c.ready) {
                        // 上游还没连上：无论 SNI 是否已解析，先排队，别丢数据
                        if (c.pending.size() == 0) {
                            c.firstDataAt = System.currentTimeMillis();
                        }
                        c.pending.write(p, payloadOff, payloadLen);
                        if (!c.connected && shouldStartUpstream(c.pending.toByteArray())) {
                            startUpstream(key, c);
                        }
                    } else {
                        writeUpstream(c, p, payloadOff, payloadLen);
                    }
                }
            }
            sendTcp(p, ihl, srcIp, srcPort, dstIp, dstPort,
                    c.sndNxt, c.rcvNxt, ACK, null, 0);
        }
        return true;
    }

    /** 缓冲里是否已经能解析出 SNI（或确定没有）。 */
    private boolean sniReady(byte[] buf) {
        return extractSni(buf) != null || buf.length >= 5 + 2048;
    }

    /** 从 ClientHello 里取 SNI；拿不到返回 null。 */
    static String extractSni(byte[] d) {
        try {
            if (d.length < 5 + 4 || (d[0] & 0xFF) != 0x16) {
                return null;
            }
            int recLen = ((d[3] & 0xFF) << 8) | (d[4] & 0xFF);
            int end = Math.min(d.length, 5 + recLen);
            if (end < 5 + 4 + 2 + 32 + 1) {
                return null;
            }
            int i = 5 + 4 + 2 + 32;           // handshake header + version + random
            int sidLen = d[i] & 0xFF;
            i += 1 + sidLen;
            if (i + 2 > end) {
                return null;
            }
            int csLen = ((d[i] & 0xFF) << 8) | (d[i + 1] & 0xFF);
            i += 2 + csLen;
            if (i + 1 > end) {
                return null;
            }
            int compLen = d[i] & 0xFF;
            i += 1 + compLen;
            if (i + 2 > end) {
                return null;
            }
            int extTotal = ((d[i] & 0xFF) << 8) | (d[i + 1] & 0xFF);
            i += 2;
            int extEnd = Math.min(end, i + extTotal);
            while (i + 4 <= extEnd) {
                int type = ((d[i] & 0xFF) << 8) | (d[i + 1] & 0xFF);
                int len2 = ((d[i + 2] & 0xFF) << 8) | (d[i + 3] & 0xFF);
                i += 4;
                if (i + len2 > extEnd) {
                    return null;
                }
                if (type == 0x00 && len2 > 5) {
                    int p2 = i + 2;           // 跳过 server_name_list 长度
                    int nameLen = ((d[p2 + 1] & 0xFF) << 8) | (d[p2 + 2] & 0xFF);
                    if (p2 + 3 + nameLen <= i + len2) {
                        return new String(d, p2 + 3, nameLen, "ISO-8859-1");
                    }
                }
                i += len2;
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    /**
     * 什么时候可以连上游。
     *
     * 之前只认「解析出 SNI」，结果纯 HTTP 请求（几十字节、没有 TLS 握手头）
     * 永远达不到条件：数据一直卡在缓冲里，客户端等到超时，上游根本没被连过。
     *
     * 现在的判定：
     *   · 首字节不是 0x16（不是 TLS 握手）-> 立刻连
     *   · 已经能解析出 SNI -> 立刻连
     *   · 缓冲达到上限 -> 连
     *   · 其余（TLS 但握手还没凑齐）-> 交给扫描线程，200ms 后兜底连
     */
    private static boolean shouldStartUpstream(byte[] pending) {
        if (pending.length == 0) {
            return false;
        }
        if ((pending[0] & 0xFF) != 0x16) {
            return true;
        }
        if (pending.length >= MAX_SNI_BYTES) {
            return true;
        }
        return extractSni(pending) != null;
    }

    /** 解析出 SNI 之后才真正向上游建连，并决定连哪个 IP。 */
    private void startUpstream(final String key, final Conn c) {
        final byte[] initial = c.pending.toByteArray();
        c.pending.reset();
        c.connected = true;
        if (active.get() >= MAX_ACTIVE) {
            Log.d(TAG, "too many active connections, rejecting");
            sendTcp(null, 20, c.srcIp, c.srcPort, c.dstIp, c.dstPort, 0, 0, RST | ACK, null, 0);
            closeConn(key, c);
            return;
        }
        active.incrementAndGet();
        pool.execute(new Runnable() {
            @Override
            public void run() {
                try {
                    String sni = extractSni(initial);
                    String target = sni == null ? null : DnsVpnService.lookupIp(sni);
                    String host = target != null ? target : ipStr(c.dstIp);
                    int port = c.dstPort;
                    if (target != null) {
                        // 命中实测表的连接一定要记下来：它是"分包是否生效"的直接证据
                        Log.i(TAG, "override " + sni + " -> " + host + " (was "
                                + ipStr(c.dstIp) + ")");
                    } else {
                        synchronized (TunProxy.this) {
                            if (connLog < 8) {
                                connLog++;
                                Log.i(TAG, "connect " + host + ":" + port + " sni=" + sni);
                            }
                        }
                    }
                    Socket s = new Socket();
                    // protect() 在本机实测返回 false —— 于是我们自己的出站连接又被自己的
                    // VPN 抓了回去，形成回环：日志里能看到"连到 104.18.42.145、SNI 又是
                    // pixiv"的无限重复，TLS 永远握不上手。
                    // 改法：显式把 socket 绑定到物理网络（WiFi/蜂窝），绕开 VPN。
                    boolean protected_ = vpn.protect(s);
                    if (!protected_) {
                        android.net.Network under = underlyingNetwork();
                        if (under != null) {
                            under.bindSocket(s);
                            protected_ = true;
                        }
                    }
                    s.setTcpNoDelay(true);
                    s.connect(new InetSocketAddress(InetAddress.getByName(host), port), 8000);
                    c.socket = s;
                    c.out = s.getOutputStream();
                    // 首次数据分包写出
                    if (initial.length > 8) {
                        c.out.write(initial, 0, 1);
                        c.out.flush();
                        try {
                            Thread.sleep(10);
                        } catch (InterruptedException ignored) {
                        }
                        c.out.write(initial, 1, initial.length - 1);
                    } else {
                        c.out.write(initial);
                    }
                    c.out.flush();
                    Log.i(TAG, "connected " + host + ":" + port + " sni=" + sni
                            + " protected=" + protected_ + " local=" + s.getLocalAddress());
                    {
                        StringBuilder hex = new StringBuilder();
                        for (int i = 0; i < Math.min(8, initial.length); i++) {
                            hex.append(String.format("%02x", initial[i]));
                        }
                        Log.i(TAG, "initial len=" + initial.length + " hex=" + hex
                                + " sni=" + extractSni(initial));
                    }
                    // 关键：把"建立过程中新到"的数据补写完之后才置 ready。
                    // 之前是先置 c.out 再 flush，这段窗口里新数据会走另一条写路径，
                    // 导致同一个字节被写两次、TLS 流被打乱，Chrome 直接断开。
                    synchronized (c.pending) {
                        if (c.pending.size() > 0) {
                            byte[] extra = c.pending.toByteArray();
                            c.pending.reset();
                            c.out.write(extra);
                            c.out.flush();
                        }
                        c.ready = true;
                    }
                    pool.execute(new Runnable() {
                        @Override
                        public void run() {
                            readUpstream(key, c);
                        }
                    });
                } catch (Exception e) {
                    Log.d(TAG, "connect failed: " + e);
                    sendTcp(null, 20, c.srcIp, c.srcPort, c.dstIp, c.dstPort, 0, 0, RST | ACK, null, 0);
                    closeConn(key, c);
                } finally {
                    active.decrementAndGet();
                }
            }
        });
    }


    /** 找一个非 VPN 的物理网络（WiFi/蜂窝），用来绕开自己的 VPN。 */
    private android.net.Network underlyingNetwork() {
        try {
            android.net.ConnectivityManager cm =
                    (android.net.ConnectivityManager) vpn.getSystemService(
                            android.content.Context.CONNECTIVITY_SERVICE);
            for (android.net.Network n : cm.getAllNetworks()) {
                android.net.NetworkCapabilities nc = cm.getNetworkCapabilities(n);
                if (nc == null) {
                    continue;
                }
                if (nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_VPN)) {
                    continue;
                }
                if (nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)
                        || nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_CELLULAR)
                        || nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_ETHERNET)) {
                    return n;
                }
            }
        } catch (Exception e) {
            Log.d(TAG, "underlyingNetwork failed: " + e);
        }
        return null;
    }

    private void writeUpstream(Conn c, byte[] p, int off, int len) {
        // 必须在收到包的同一个线程里按到达顺序写！
        // 之前这里丢给线程池，两个数据段可能以任意顺序写入 socket，
        // 于是服务端收到的字节流被打乱，表现为 ERR_SSL_BAD_RECORD_MAC_ALERT
        // （客户端与服务端的 TLS transcript 不一致）。这是本轮的核心修复。
        OutputStream out = c.out;
        if (out == null) {
            return;
        }
        try {
            out.write(p, off, len);
            out.flush();
        } catch (Exception e) {
            Log.d(TAG, "upstream write failed: " + e);
            c.closeReason = "upstream write failed";
            closeConn(keyOf(c), c);
        }
    }

    /** 重传：每 250ms 扫一遍没被确认的段，超时就重发。 */
    private static long nowMs() {
        return System.currentTimeMillis();
    }

    private void startRetransmit() {
        pool.execute(new Runnable() {
            @Override
            public void run() {
                while (running) {
                    try {
                        Thread.sleep(250);
                    } catch (InterruptedException e) {
                        return;
                    }
                    java.util.List<Conn> snapshot;
                    synchronized (conns) {
                        snapshot = new java.util.ArrayList<>(conns.values());
                    }
                    for (Conn c : snapshot) {
                        if (c.closed) {
                            continue;
                        }
                        // 兜底：TLS 握手迟迟凑不齐（或客户端只发了半包）时，
                        // 200ms 后带着已有字节直接连，别让客户端白等。
                        if (!c.connected && !c.ready) {
                            byte[] pendingNow = null;
                            synchronized (c.pending) {
                                if (c.pending.size() > 0 && c.firstDataAt > 0
                                        && nowMs() - c.firstDataAt > 200) {
                                    pendingNow = c.pending.toByteArray();
                                }
                            }
                            if (pendingNow != null) {
                                startUpstream(keyOf(c), c);
                            }
                        }
                        synchronized (c.inflight) {
                            long now = System.currentTimeMillis();
                            for (java.util.Map.Entry<Long, Object[]> e : c.inflight.entrySet()) {
                                Object[] v = e.getValue();
                                long sentAt = (Long) v[1];
                                int tries = (Integer) v[2];
                                if (now - sentAt > 400 && tries < 5) {
                                    v[1] = now;
                                    v[2] = tries + 1;
                                    byte[] data = (byte[]) v[0];
                                    sendTcp(null, 20, c.srcIp, c.srcPort, c.dstIp, c.dstPort,
                                            e.getKey(), c.rcvNxt, PSH | ACK, data, data.length);
                                    Log.d(TAG, "retransmit seq=" + e.getKey()
                                            + " len=" + data.length + " try=" + (tries + 1));
                                }
                            }
                        }
                    }
                }
            }
        });
    }

    private static boolean isAcked(long seq, int len, long ack) {
        long end = (seq + len) & 0xFFFFFFFFL;
        return ((ack - seq) & 0xFFFFFFFFL) > 0 && ((ack - end) & 0xFFFFFFFFL) >= 0x80000000L;
    }

    private void readUpstream(String key, Conn c) {
        byte[] buf = new byte[MSS];
        try {
            InputStream in = c.socket.getInputStream();
            while (running && !c.closed) {
                int n = in.read(buf);
                if (n <= 0) {
                    break;
                }
                if (c.sndNxt == c.iss + 1) {
                    Log.i(TAG, "data out first " + n + "B key=" + key);
                }
                byte[] payload = new byte[n];
                System.arraycopy(buf, 0, payload, 0, n);
                c.bytesOut += n;
                c.bytesFromServer += n;
                long seq = c.sndNxt;
                c.sndNxt = (c.sndNxt + n) & 0xFFFFFFFFL;
                synchronized (c.inflight) {
                    c.inflight.put(seq, new Object[]{payload, System.currentTimeMillis(), 0});
                }
                sendTcp(null, 20, c.srcIp, c.srcPort, c.dstIp, c.dstPort,
                        seq, c.rcvNxt, PSH | ACK, payload, n);
            }
        } catch (Exception e) {
            Log.d(TAG, "upstream read ended: " + e);
        } finally {
            if ("?".equals(c.closeReason)) {
                c.closeReason = "upstream EOF";
            }
            sendTcp(null, 20, c.srcIp, c.srcPort, c.dstIp, c.dstPort,
                    c.sndNxt, c.rcvNxt, FIN | ACK, null, 0);
            c.sndNxt = (c.sndNxt + 1) & 0xFFFFFFFFL;
            closeConn(key, c);
        }
    }

    private void sendTcp(byte[] req, int ihl, int srcIp, int srcPort,
                         int dstIp, int dstPort, long seq, long ack, int flags,
                         byte[] payload, int payloadLen) {
        int total = 20 + 20 + payloadLen;
        byte[] p = new byte[total];
        p[0] = 0x45;
        p[2] = (byte) ((total >> 8) & 0xFF);
        p[3] = (byte) (total & 0xFF);
        // IP 标识必须递增：恒为 0 时，某些接收栈会把连续报文当重复包丢掉。
        synchronized (this) {
            ipId = (ipId + 1) & 0xFFFF;
            p[4] = (byte) (ipId >> 8);
            p[5] = (byte) ipId;
        }
        p[6] = 0x40;                       // DF：不要分片
        p[8] = 64;
        p[9] = 6;
        putInt(p, 12, dstIp);
        putInt(p, 16, srcIp);
        int sum = 0;
        for (int i = 0; i < 20; i += 2) {
            sum += u16(p, i);
        }
        while ((sum >> 16) != 0) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        int cks = (~sum) & 0xFFFF;
        p[10] = (byte) (cks >> 8);
        p[11] = (byte) cks;

        int o = 20;
        p[o] = (byte) (dstPort >> 8);
        p[o + 1] = (byte) dstPort;
        p[o + 2] = (byte) (srcPort >> 8);
        p[o + 3] = (byte) srcPort;
        putInt(p, o + 4, seq);
        putInt(p, o + 8, ack);
        p[o + 12] = (byte) 0x50;
        p[o + 13] = (byte) flags;
        p[o + 14] = (byte) 0xFF;
        p[o + 15] = (byte) 0xFF;
        if (payload != null && payloadLen > 0) {
            System.arraycopy(payload, 0, p, o + 20, payloadLen);
        }
        int tcpSum = 0;
        tcpSum += (dstIp >> 16) & 0xFFFF;
        tcpSum += dstIp & 0xFFFF;
        tcpSum += (srcIp >> 16) & 0xFFFF;
        tcpSum += srcIp & 0xFFFF;
        tcpSum += 6;
        int tcpLen = 20 + payloadLen;
        tcpSum += tcpLen;
        // 注意：tcpLen 可能是奇数（20 + 奇数字节载荷）。按 2 字节步进时，
        // 最后一个字节后面没有配对字节，直接读 p[i+1] 会数组越界 ——
        // 这正是之前"最后 410 字节永远发不出去、握手卡死"的根因。
        for (int i = 0; i < tcpLen; i += 2) {
            int hi = p[o + i] & 0xFF;
            int lo = (i + 1 < tcpLen) ? (p[o + i + 1] & 0xFF) : 0;
            tcpSum += (hi << 8) | lo;
        }
        while ((tcpSum >> 16) != 0) {
            tcpSum = (tcpSum & 0xFFFF) + (tcpSum >> 16);
        }
        int tcpCks = (~tcpSum) & 0xFFFF;
        p[o + 16] = (byte) (tcpCks >> 8);
        p[o + 17] = (byte) tcpCks;

        try {
            synchronized (tunOut) {
                tunOut.write(p);
                tunOut.flush();
            }
        } catch (Exception e) {
            Log.d(TAG, "tun write failed: " + e);
        }
    }

    private void closeConn(String key, Conn c) {
        if (c.closed) {
            return;
        }
        Log.i(TAG, "close " + key + " in=" + c.bytesIn + " out=" + c.bytesOut
                + " reason=" + c.closeReason + " clientAck=" + c.sndUna
                + " ourNextSeq=" + c.sndNxt + " unacked=" + (c.sndNxt - c.sndUna)
                + " serverSent=" + c.bytesFromServer);
        c.closed = true;
        synchronized (conns) {
            conns.remove(key);
        }
        closeQuietly(c);
    }

    private void closeQuietly(Conn c) {
        try {
            if (c.socket != null) {
                c.socket.close();
            }
        } catch (Exception ignored) {
        }
    }

    private static String keyOf(Conn c) {
        return key(c.srcIp, c.srcPort, c.dstIp, c.dstPort);
    }

    private static String key(int sIp, int sPort, int dIp, int dPort) {
        return sIp + ":" + sPort + ">" + dIp + ":" + dPort;
    }

    private static int u16(byte[] p, int o) {
        return ((p[o] & 0xFF) << 8) | (p[o + 1] & 0xFF);
    }

    private static long u32(byte[] p, int o) {
        return ((long) (p[o] & 0xFF) << 24) | ((p[o + 1] & 0xFF) << 16)
                | ((p[o + 2] & 0xFF) << 8) | (p[o + 3] & 0xFF);
    }

    private static int i32(byte[] p, int o) {
        return ((p[o] & 0xFF) << 24) | ((p[o + 1] & 0xFF) << 16)
                | ((p[o + 2] & 0xFF) << 8) | (p[o + 3] & 0xFF);
    }

    private static void putInt(byte[] p, int o, long v) {
        p[o] = (byte) ((v >> 24) & 0xFF);
        p[o + 1] = (byte) ((v >> 16) & 0xFF);
        p[o + 2] = (byte) ((v >> 8) & 0xFF);
        p[o + 3] = (byte) (v & 0xFF);
    }

    static String ipStr(int ip) {
        return ((ip >> 24) & 0xFF) + "." + ((ip >> 16) & 0xFF) + "."
                + ((ip >> 8) & 0xFF) + "." + (ip & 0xFF);
    }
}
