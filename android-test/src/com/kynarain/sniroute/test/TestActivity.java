package com.kynarain.sniroute.test;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * 独立测试端。
 *
 * 为什么需要它：Android 的 VPN 只排除「提供 VPN 的那个应用」自己的流量，
 * 所以主应用内部开 socket 测不到自己的隧道。这个包是另一个 UID，
 * 它的连接会真正进入 Sniroute 的 tun，从而验证用户态 TCP 栈。
 *
 * 用纯 HTTP（不做 TLS），把 TLS 因素排除掉：如果这里能拿到 HTTP 响应，
 * 就说明栈正确；如果拿不到，问题就在栈本身而不是 TLS。
 *
 * 注意：这里不用 lambda —— 构建用 -source 8 + android.jar 作 bootclasspath，
 * java.lang.invoke.LambdaMetafactory 不在 android.jar 里，lambda 会编译失败。
 */
public class TestActivity extends Activity {

    private static final String TAG = "SnirouteTest";
    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 96, 48, 48);
        root.setGravity(Gravity.CENTER_HORIZONTAL);

        TextView title = new TextView(this);
        title.setText("Sniroute 测试端");
        title.setTextSize(24);
        root.addView(title);

        status = new TextView(this);
        status.setText("未测试");
        status.setTextSize(16);
        status.setPadding(0, 32, 0, 32);
        root.addView(status);

        Button run = new Button(this);
        run.setText("运行测试");
        run.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                runTest();
            }
        });
        root.addView(run);

        setContentView(root);
    }

    private void runTest() {
        status.setText("测试中…");
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                doTest();
            }
        }, "test");
        t.start();
    }

    private void doTest() {
        String report = testTls("223.5.5.5", 443, "223.5.5.5")
                + testHttp("223.5.5.5", 80, "223.5.5.5");
        Log.i(TAG, report.trim());
        final String shown = report;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                status.setText(shown);
            }
        });
    }

    /** 走 TLS（443）：验证隧道对真实 TLS 流的字节透传是否正确。 */
    private String testTls(String ip, int port, String host) {
        long t0 = System.currentTimeMillis();
        try {
            javax.net.ssl.SSLContext ctx = javax.net.ssl.SSLContext.getInstance("TLS");
            ctx.init(null, new javax.net.ssl.TrustManager[]{
                    new javax.net.ssl.X509TrustManager() {
                        public void checkClientTrusted(java.security.cert.X509Certificate[] c, String a) {
                        }
                        public void checkServerTrusted(java.security.cert.X509Certificate[] c, String a) {
                        }
                        public java.security.cert.X509Certificate[] getAcceptedIssuers() {
                            return new java.security.cert.X509Certificate[0];
                        }
                    }
            }, new java.security.SecureRandom());
            Socket raw = new Socket();
            raw.connect(new InetSocketAddress(ip, port), 8000);
            raw.setSoTimeout(12000);
            javax.net.ssl.SSLSocket ss = (javax.net.ssl.SSLSocket)
                    ctx.getSocketFactory().createSocket(raw, host, port, true);
            ss.startHandshake();
            ss.getOutputStream().write(("GET / HTTP/1.0\r\nHost: " + host
                    + "\r\nConnection: close\r\n\r\n").getBytes("ISO-8859-1"));
            ss.getOutputStream().flush();
            byte[] buf = new byte[512];
            int n = ss.getInputStream().read(buf);
            String head = n > 0 ? new String(buf, 0, Math.min(n, 100), "ISO-8859-1") : "";
            String first = head.split("\r\n")[0];
            long ms = System.currentTimeMillis() - t0;
            String line = "[OK-TLS] " + host + " " + n + "B " + ms
                    + "ms first=[" + first + "]\n";
            Log.i(TAG, line.trim());
            ss.close();
            return line;
        } catch (Exception e) {
            long ms = System.currentTimeMillis() - t0;
            String line = "[FAIL-TLS] " + host + " " + e.getClass().getSimpleName()
                    + ": " + String.valueOf(e.getMessage()) + " " + ms + "ms\n";
            Log.i(TAG, line.trim());
            return line;
        }
    }

    private String testHttp(String ip, int port, String host) {
        long t0 = System.currentTimeMillis();
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress(ip, port), 8000);
            s.setSoTimeout(10000);
            OutputStream out = s.getOutputStream();
            out.write(("GET / HTTP/1.0\r\nHost: " + host
                    + "\r\nConnection: close\r\n\r\n").getBytes("ISO-8859-1"));
            out.flush();
            InputStream in = s.getInputStream();
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            byte[] tmp = new byte[2048];
            int n;
            while ((n = in.read(tmp)) > 0 && buf.size() < 65536) {
                buf.write(tmp, 0, n);
            }
            byte[] body = buf.toByteArray();
            String head = new String(body, 0, Math.min(body.length, 120), "ISO-8859-1");
            String first = head.split("\r\n")[0];
            long ms = System.currentTimeMillis() - t0;
            String line = "[OK] " + host + " " + body.length + "B " + ms
                    + "ms first=[" + first + "]\n";
            Log.i(TAG, line.trim());
            return line;
        } catch (Exception e) {
            long ms = System.currentTimeMillis() - t0;
            String line = "[FAIL] " + host + " " + e.getClass().getSimpleName()
                    + ": " + String.valueOf(e.getMessage()) + " " + ms + "ms\n";
            Log.i(TAG, line.trim());
            return line;
        } finally {
            try {
                s.close();
            } catch (Exception ignored) {
            }
        }
    }
}
