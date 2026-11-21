#!/usr/bin/env python3
"""sniroute 图形界面。

定位：一层薄壳，**不重复实现任何逻辑**。所有动作都调用 scripts/sniroute.py 里
已有的函数（cmd_up / cmd_down / cmd_status / cmd_doh ...），所以命令行和界面
永远一致 —— 不会出现"界面能做的事命令行做不了"或者两处行为不一致。

只用标准库 tkinter，不需要 pip 装任何东西。这与 SNIBypassGUI 那种用 Wails +
React 的做法不同：那边是为了跨平台与外观，这里是为了零依赖与可审计。

启动：
    scripts\\sniroute-gui.cmd      （无控制台窗口）
    python tools\\gui.py           （带控制台，便于看异常）
"""

from __future__ import annotations

import contextlib
import io
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import sniroute as core  # noqa: E402  —— 必须先把 scripts/ 加进 sys.path

DOH_URL = f"https://doh.sniroute.lan:{core.DOH_PORT}/dns-query"
REFRESH_MS = 3000


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("sniroute — 本地 SNI 反向代理")
        self.geometry("880x620")
        self.minsize(760, 540)

        self._out = queue.Queue()      # 后台线程 → UI 的消息队列
        self._busy = False

        self._build()
        self._pump()
        self.refresh()

    # ------------------------------------------------------------------ UI
    def _build(self) -> None:
        pad = {"padx": 8, "pady": 4}

        # 顶部：四个状态灯
        top = ttk.LabelFrame(self, text="组件状态")
        top.pack(fill="x", **pad)
        self._lamps: dict[str, ttk.Label] = {}
        row = ttk.Frame(top)
        row.pack(fill="x", padx=8, pady=6)
        for i, name in enumerate(("DoH", "nginx", "hosts", "根证书")):
            lab = ttk.Label(row, text=f"{name}: ?", width=22)
            lab.grid(row=0, column=i, sticky="w")
            self._lamps[name] = lab

        # 操作按钮
        ops = ttk.LabelFrame(self, text="操作")
        ops.pack(fill="x", **pad)
        btn_row = ttk.Frame(ops)
        btn_row.pack(fill="x", padx=8, pady=6)
        buttons = [
            ("启动全部", self.on_up, "启动 DoH 与 nginx，并做健康检查"),
            ("停止全部", self.on_down, "停止两个服务"),
            ("刷新状态", self.refresh, "重新读取状态"),
            ("安装证书/hosts", self.on_install, "需要管理员：会弹 UAC"),
            ("打开浏览器", self.on_browser, "用独立配置启动带 ECH 的 Chrome"),
            ("还原一切", self.on_revert, "移除根证书、hosts 条目，并停止服务"),
        ]
        for i, (text, cmd, tip) in enumerate(buttons):
            b = ttk.Button(btn_row, text=text, command=cmd, width=14)
            b.grid(row=0, column=i, padx=3)
            _Tooltip(b, tip)
        self._buttons = {w.cget("text"): w for w in btn_row.winfo_children()}

        # 站点表
        sites_box = ttk.LabelFrame(self, text="站点")
        sites_box.pack(fill="x", **pad)
        self._sites = ttk.Treeview(
            sites_box, columns=("state", "mode", "domains", "upstreams"),
            show="headings", height=4)
        for col, title, w in (("state", "状态", 80), ("mode", "DNS 模式", 100),
                              ("domains", "域名", 420), ("upstreams", "上游", 70)):
            self._sites.heading(col, text=title)
            self._sites.column(col, width=w, anchor="w")
        self._sites.pack(fill="x", padx=8, pady=6)

        # 日志区
        log_box = ttk.LabelFrame(self, text="日志")
        log_box.pack(fill="both", expand=True, **pad)
        self._notebook = ttk.Notebook(log_box)
        self._notebook.pack(fill="both", expand=True, padx=6, pady=6)

        self._op_log = self._make_text(self._notebook)
        self._notebook.add(self._op_log.master, text="操作")

        self._doh_log = self._make_text(self._notebook)
        self._notebook.add(self._doh_log.master, text="DoH 查询")

        # 底部提示
        self._hint = ttk.Label(
            self, text=f"浏览器里安全 DNS 填：{DOH_URL}",
            foreground="#444")
        self._hint.pack(fill="x", padx=12, pady=(0, 8))

    def _make_text(self, parent) -> tk.Text:
        frame = ttk.Frame(parent)
        text = tk.Text(frame, height=10, wrap="none", font=("Consolas", 9))
        scroll = ttk.Scrollbar(frame, orient="vertical", command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        text.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        return text

    # --------------------------------------------------------------- 后台执行
    def _run(self, func, label: str, done=None) -> None:
        """在后台线程跑一个动作，把它的 stdout 收进"操作"日志。"""
        if self._busy:
            self._append_op(f"[跳过] {label}：上一步还在执行")
            return
        self._busy = True
        self._append_op(f"=== {label} ===")

        def worker() -> None:
            buf = io.StringIO()
            code = 0
            try:
                with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
                    func()
            except SystemExit as exc:
                code = int(exc.code or 0)
            except Exception as exc:  # 任何异常都要显示出来，不能静默
                buf.write(f"\n[异常] {exc!r}\n")
                code = 1
            self._out.put(("log", buf.getvalue()))
            self._out.put(("done", (label, code, done)))

        threading.Thread(target=worker, daemon=True).start()

    def _pump(self) -> None:
        """把后台消息搬进 UI。tkinter 只能在主线程碰控件。"""
        try:
            while True:
                kind, payload = self._out.get_nowait()
                if kind == "log":
                    self._append_op(payload.rstrip())
                elif kind == "done":
                    label, code, done = payload
                    self._busy = False
                    self._append_op(f"--- {label} 结束（exit {code}）---")
                    if done:
                        done(code)
                    self.refresh()
        except queue.Empty:
            pass
        self.after(120, self._pump)

    def _append_op(self, text: str) -> None:
        self._op_log.insert("end", text + "\n")
        self._op_log.see("end")

    def _append_doh(self, text: str) -> None:
        self._doh_log.delete("1.0", "end")
        self._doh_log.insert("end", text)
        self._doh_log.see("end")

    # ---------------------------------------------------------------- 状态刷新
    def refresh(self) -> None:
        doh = core.port_listener_pid(core.DOH_PORT)
        nginx = core.pid_is_running(ROOT / "logs" / "nginx.pid")
        hosts = core.hosts_has_doh_entry()
        ca_file = (core.CA_DIR / "rootCA.crt").is_file()

        def lamp(name: str, ok: bool, extra: str = "") -> None:
            self._lamps[name].configure(
                text=f"{name}: {'运行中' if ok else '未运行'}{extra}",
                foreground="#0a7d26" if ok else "#b3261e")

        lamp("DoH", bool(doh), f" (pid {doh})" if doh else "")
        lamp("nginx", nginx)
        lamp("hosts", hosts)
        lamp("根证书", ca_file)

        self._sites.delete(*self._sites.get_children())
        try:
            for site in core.load_sites()["sites"]:
                enabled = site.get("enabled", True)
                state = "启用" if enabled else ("仅 DNS" if site.get("dns_override") else "停用")
                count = 0
                lst = ROOT / site["upstream_file"]
                if lst.is_file():
                    count = sum(1 for line in lst.read_text(encoding="utf-8").splitlines()
                                if line.strip().startswith("server "))
                self._sites.insert("", "end", values=(
                    state, site.get("dns_mode", "ech"),
                    ", ".join(site["hosts"]), f"{count} 条"))
        except Exception as exc:
            self._append_op(f"[状态] 读取站点失败: {exc}")

        if core.DOH_LOG.is_file():
            lines = core.DOH_LOG.read_text(encoding="utf-8", errors="replace").splitlines()
            self._append_doh(f"（共 {len(lines)} 条，显示最后 200 条）\n"
                             + "\n".join(lines[-200:]))

        self.after(REFRESH_MS, self.refresh)

    # ------------------------------------------------------------------ 动作
    def on_up(self) -> None:
        self._run(lambda: core.cmd_up(_Ns(nginx=None)), "启动全部")

    def on_down(self) -> None:
        self._run(lambda: core.cmd_down(_Ns(nginx=None)), "停止全部")

    def on_install(self) -> None:
        if not messagebox.askokcancel(
                "需要管理员", "这会弹出 UAC 提权窗口，用来装本机根证书并写 hosts。\n继续？"):
            return
        script = SCRIPTS / "setup-browser-ech.ps1"
        self._run(lambda: _run_powershell(script, ["-Elevated"]), "安装证书/hosts")

    def on_browser(self) -> None:
        script = SCRIPTS / "start-chrome-ech.ps1"
        self._run(lambda: _run_powershell(script, []), "打开浏览器")

    def on_revert(self) -> None:
        if not messagebox.askokcancel("还原", "将移除根证书、hosts 条目并停止服务。\n继续？"):
            return

        def action() -> None:
            core.cmd_down(_Ns(nginx=None))
            _run_powershell(SCRIPTS / "setup-browser-ech.ps1", ["-Revert", "-Elevated"])

        self._run(action, "还原一切")


def _run_powershell(script: Path, extra: list) -> None:
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", str(script), *extra]
    res = subprocess.run(cmd, capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    if res.stdout:
        print(res.stdout.rstrip())
    if res.stderr:
        print(res.stderr.rstrip())
    if res.returncode != 0:
        raise SystemExit(res.returncode)


class _Ns:
    """把参数包成 argparse.Namespace 的样子，好直接喂给 core.cmd_* 。"""

    def __init__(self, **kw) -> None:
        self.__dict__.update(kw)


class _Tooltip:
    """极简悬浮提示：按钮上不写长说明，但鼠标停一下要能看到。"""

    def __init__(self, widget, text: str) -> None:
        self.widget, self.text, self.tip = widget, text, None
        widget.bind("<Enter>", self.show)
        widget.bind("<Leave>", self.hide)

    def show(self, _event=None) -> None:
        x = self.widget.winfo_rootx() + 12
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        self.tip = tk.Toplevel(self.widget)
        self.tip.wm_overrideredirect(True)
        self.tip.wm_geometry(f"+{x}+{y}")
        tk.Label(self.tip, text=self.text, background="#ffffe0",
                 relief="solid", borderwidth=1).pack()

    def hide(self, _event=None) -> None:
        if self.tip:
            self.tip.destroy()
            self.tip = None


def main() -> None:
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()
