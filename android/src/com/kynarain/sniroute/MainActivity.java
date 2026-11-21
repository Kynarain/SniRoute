package com.kynarain.sniroute;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.VpnService;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 主界面：只有一个开关和一行状态。
 *
 * 这个应用做的是"只接管 DNS"：启动后建立一个仅包含一条 /32 路由的 VPN，
 * 把系统的 DNS 服务器指向我们自己，其余流量完全不经过 VPN —— 所以不会拖慢上网，
 * 也不会影响任何需要直连的应用。
 */
public class MainActivity extends Activity {

    private static final int REQ_VPN = 1001;

    private TextView status;
    private Button toggle;
    private final Handler ui = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(48, 96, 48, 48);

        TextView title = new TextView(this);
        title.setText("Sniroute");
        title.setTextSize(28);
        root.addView(title);

        TextView desc = new TextView(this);
        desc.setText("接管所有 TCP 流量：读到 TLS ClientHello 里的 SNI 后，\n"
                + "把目标换成实测可达的 IP，并对首个报文做分段。\n\n"
                + "已实测可用（端到端，走本机隧道）：GitHub、CurseForge、\n"
                + "Modrinth、百度，页面均完整加载。\n"
                + "同类可修：DNS 被污染、SNI 正常的站点（Steam 商店、CurseForge、\n"
                + "Modrinth 等，IP 表已逐个握手验证）。\n\n"
                + "修不了：SNI 被阻断的站点（pixiv 主站、Steam 社区、Discord）——\n"
                + "那需要证书中间人或 ECH，非 root 设备做不到。");
        desc.setTextSize(15);
        desc.setPadding(0, 24, 0, 32);
        root.addView(desc);

        Button selftestBtn = new Button(this);
        selftestBtn.setText("自测");
        selftestBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                runSelfTest();
            }
        });
        root.addView(selftestBtn);

        status = new TextView(this);
        status.setTextSize(16);
        root.addView(status);

        toggle = new Button(this);
        toggle.setText("启动");
        toggle.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (DnsVpnService.isRunning()) {
                    stopVpn();
                } else {
                    startVpn();
                }
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 40;
        root.addView(toggle, lp);

        setContentView(root);
        refreshUi();

        // 自测入口：adb shell am start -n com.kynarain.sniroute/.MainActivity --es selftest 1
        // 目的：用一个不受 protect 保护的 socket 访问纯 HTTP 站点，让流量自己走一遍
        // 我们的 TCP 栈，从而判定"栈本身是否正确"，把浏览器因素排除掉。
        if (getIntent() != null && getIntent().hasExtra("selftest")) {
            runSelfTest();
        }
    }

    /** 自测：纯 HTTP 走我们的 TCP 栈，结果只写 logcat（TAG = SnirouteSelftest）。 */
    private void runSelfTest() {
        android.util.Log.i("SnirouteSelftest", "selftest entered (thread will start)");
        android.widget.Toast.makeText(this, "自测开始", android.widget.Toast.LENGTH_SHORT).show();
        new Thread(new Runnable() {
            @Override
            public void run() {
                final String TAG = "SnirouteSelftest";
                try {
                    Thread.sleep(3000);
                    java.net.Socket s = new java.net.Socket();
                    s.connect(new java.net.InetSocketAddress("93.184.216.34", 80), 8000);
                    s.setSoTimeout(10000);
                    android.util.Log.i(TAG, "connected to example.com:80 (via our tun)");
                    String req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
                    s.getOutputStream().write(req.getBytes("ISO-8859-1"));
                    s.getOutputStream().flush();
                    android.util.Log.i(TAG, "request sent " + req.length() + "B");
                    java.io.InputStream in = s.getInputStream();
                    byte[] buf = new byte[2048];
                    int n = in.read(buf);
                    if (n <= 0) {
                        android.util.Log.i(TAG, "RESULT FAIL: no response (" + n + ")");
                        return;
                    }
                    String head = new String(buf, 0, Math.min(n, 200), "ISO-8859-1");
                    String firstLine = head.split("\r\n")[0];
                    android.util.Log.i(TAG, "RESULT OK: read " + n + "B, first line=[" + firstLine + "]");
                    s.close();
                } catch (Exception e) {
                    android.util.Log.i(TAG, "RESULT FAIL: " + e);
                }
            }
        }, "selftest").start();
    }

    private void startVpn() {
        Intent prepare = VpnService.prepare(this);
        if (prepare != null) {
            // 第一次需要用户授权（系统弹窗规定得非常明确，无法绕过）
            startActivityForResult(prepare, REQ_VPN);
            return;
        }
        launchService();
    }

    private void launchService() {
        Intent svc = new Intent(this, DnsVpnService.class);
        svc.setAction(DnsVpnService.ACTION_START);
        startService(svc);
        ui.postDelayed(new Runnable() {
            @Override
            public void run() {
                refreshUi();
            }
        }, 600);
    }

    private void stopVpn() {
        Intent svc = new Intent(this, DnsVpnService.class);
        svc.setAction(DnsVpnService.ACTION_STOP);
        startService(svc);
        ui.postDelayed(new Runnable() {
            @Override
            public void run() {
                refreshUi();
            }
        }, 600);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_VPN) {
            if (resultCode == RESULT_OK) {
                launchService();
            } else {
                status.setText("未获得 VPN 授权，无法接管 DNS");
            }
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        // 应用已在运行时再发一次 am start --es selftest，走这里（不会重走 onCreate）
        if (intent != null && intent.hasExtra("selftest")) {
            runSelfTest();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshUi();
    }

    private void refreshUi() {
        boolean running = DnsVpnService.isRunning();
        toggle.setText(running ? "停止" : "启动");
        SharedPreferences sp = getSharedPreferences("state", MODE_PRIVATE);
        int count = sp.getInt("resolved", 0);
        long last = sp.getLong("last_ms", 0);
        String lastText = last == 0 ? "尚未解析" : ("最近一次解析 " + (System.currentTimeMillis() - last) / 1000 + " 秒前");
        status.setText(running
                ? "运行中 · 已解析 " + count + " 次 · " + lastText
                : "未启动");
        ui.removeCallbacksAndMessages(null);
        ui.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!isFinishing()) {
                    refreshUi();
                }
            }
        }, 2000);
    }
}
