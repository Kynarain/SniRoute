import javax.swing.*;
import java.awt.*;
import java.awt.image.BufferedImage;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * sniroute tray application: a dot next to the clock, no window.
 *
 * <p>There is deliberately no JFrame. The tray menu is the whole interface, and
 * three things replace what a window used to give:
 * <ul>
 *   <li>results arrive as balloon notifications (including the exit code);</li>
 *   <li>full output is appended to {@code logs/gui.log}, reachable from the menu;</li>
 *   <li>the icon colour and tooltip carry live status (green = both services up).</li>
 * </ul>
 * The only dialog left is the yes/no confirmation before the two privileged or
 * destructive actions, because silently installing a root certificate or
 * removing it would be worse than showing one extra prompt.
 *
 * <p>Every action shells out to {@code sniroute.cmd}, the same code path the
 * command line uses, so the tray and the CLI cannot drift apart.
 *
 * <p>This file is ASCII only on purpose: user-visible text lives in
 * {@code data/gui-strings.json} and is read as UTF-8.
 */
public final class SnirouteGui {

    private static final int DOH_PORT = 5444;
    private static final String RUN_KEY = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    private static final String RUN_VALUE = "sniroute";

    private static final Color OK_COLOR = new Color(0, 120, 40);
    private static final Color BAD_COLOR = new Color(180, 40, 30);

    private final Path repo;
    private final Path actionLog;
    private final Path hostsPath = Paths.get(
            System.getenv().getOrDefault("SystemRoot", "C:\\Windows"),
            "System32", "drivers", "etc", "hosts");
    private final java.util.Map<String, String> text = new java.util.LinkedHashMap<>();
    private final CountDownLatch exitLatch = new CountDownLatch(1);

    private TrayIcon trayIcon;
    private boolean trayReady = false;
    private boolean quitting = false;
    private volatile boolean busy = false;
    private boolean notified = false;

    /** Created when the user declines the one-time setup, so we never nag twice. */
    private Path declinedMarker;

    public static void main(String[] args) throws Exception {
        Path repo = Paths.get(SnirouteGui.class.getProtectionDomain()
                .getCodeSource().getLocation().toURI()).getParent();
        while (repo != null && !Files.exists(repo.resolve("sniroute.cmd"))) {
            repo = repo.getParent();
        }
        if (repo == null) {
            System.err.println("Cannot locate sniroute.cmd from the class location.");
            System.exit(2);
        }
        boolean smoke = java.util.Arrays.stream(args)
                .anyMatch(a -> a.equalsIgnoreCase("--smoke"));

        final Path repoFinal = repo;
        SnirouteGui app = new SnirouteGui(repoFinal);

        // --dump-strings exists so the text pipeline can be checked without a GUI:
        // if this prints correct characters, the file was read as UTF-8 correctly.
        if (java.util.Arrays.stream(args).anyMatch(a -> a.equalsIgnoreCase("--dump-strings"))) {
            app.loadStrings();
            app.text.forEach((k, v) -> System.out.println(k + "=" + v));
            return;
        }

        // --font-probe answers "can THIS runtime draw Chinese at all?" - the one
        // question that separates a rendering bug from an encoding bug.
        if (java.util.Arrays.stream(args).anyMatch(a -> a.equalsIgnoreCase("--font-probe"))) {
            app.probeFonts();
            return;
        }

        app.init();

        if (!app.trayReady) {
            System.err.println("No system tray available; nothing to show. "
                    + "Use the command line: sniroute.cmd up");
            System.exit(3);
        }
        if (smoke) {
            System.out.println("smoke: tray = true, window = false");
            app.dispose();
            return;
        }
        // One-time setup, the way an installer-based tool does it: ask once,
        // elevate once, then never again. Declining is remembered.
        app.maybeFirstRunSetup();
        app.notifyFirstRun();
        app.exitLatch.await(); // keep the process alive while the tray icon exists
    }

    private SnirouteGui(Path repo) {
        this.repo = repo;
        this.actionLog = repo.resolve("logs/gui.log");
        this.declinedMarker = repo.resolve("data/.first-run-declined");
        ICON_OWNER = repo;
    }

    // ------------------------------------------------------------- first run
    /**
     * Ask once for the one-time setup, then elevate once.
     *
     * <p>This is the piece that makes the portable folder feel installed: without
     * it the user has to find the menu entry and read the docs before anything
     * works. Declining writes a marker so the prompt appears exactly once.
     */
    private void maybeFirstRunSetup() {
        if (caTrusted() && hostsReady()) {
            return;                     // already set up on this machine
        }
        // No confirmation and no elevation any more: the CA goes into the current
        // user's trust store, which needs no administrator rights. Installers that
        // silently do this are why "no UAC" tools feel effortless.
        log("[setup] not configured yet; installing the CA into the user store");
        runScript(Kind.INSTALL, "setup-browser-ech.ps1");
    }

    /** Either the user store has our CA, or the old hosts entry still exists. */
    private boolean hostsReady() {
        return hostsHasEntry();
    }

    // ------------------------------------------------------------------- init
    private void init() {
        loadStrings();
        if (!SystemTray.isSupported()) {
            return;
        }
        // AWT PopupMenu is a NATIVE Windows menu and does not do CJK font
        // fallback: every Chinese label rendered as a box. A Swing JPopupMenu is
        // painted by Java, which does fall back, so the labels are readable.
        // The native menu is still constructed as a last-resort fallback for the
        // rare platform where showing a Swing popup over the tray misbehaves.
        buildSwingMenu();

        trayIcon = new TrayIcon(trayImage(OK_COLOR),
                text.getOrDefault("tray_tip", "sniroute"), null);
        trayIcon.setImageAutoSize(true);
        trayIcon.addActionListener(e -> notifyStatus()); // double click = status
        trayIcon.addMouseListener(new java.awt.event.MouseAdapter() {
            @Override
            public void mouseReleased(java.awt.event.MouseEvent e) {
                if (e.isPopupTrigger()) {
                    showSwingMenu(e);
                }
            }

            @Override
            public void mousePressed(java.awt.event.MouseEvent e) {
                if (e.isPopupTrigger()) {
                    showSwingMenu(e);
                }
            }
        });
        try {
            SystemTray.getSystemTray().add(trayIcon);
            trayReady = true;
        } catch (AWTException ex) {
            log("[tray] cannot add icon: " + ex.getMessage());
            return;
        }

        // java.util.Timer, not javax.swing.Timer: no Swing component involved.
        java.util.Timer poll = new java.util.Timer("sniroute-status", true);
        poll.schedule(new java.util.TimerTask() {
            @Override
            public void run() {
                refresh();
            }
        }, 0, 3000);
    }

    private JPopupMenu swingMenu;

    /** Report font/runtime facts into logs/gui.log. Used to diagnose tofu boxes. */
    private void probeFonts() {
        StringBuilder sb = new StringBuilder();
        sb.append("[font-probe] java.version=").append(System.getProperty("java.version"));
        sb.append(" runtime=").append(System.getProperty("java.home"));
        sb.append(" file.encoding=").append(System.getProperty("file.encoding"));
        sb.append(" sun.jnu.encoding=").append(System.getProperty("sun.jnu.encoding"));
        sb.append(System.lineSeparator());
        GraphicsEnvironment ge = GraphicsEnvironment.getLocalGraphicsEnvironment();
        String[] families = ge.getAvailableFontFamilyNames();
        int cjk = 0;
        for (String n : families) {
            if (new Font(n, Font.PLAIN, 12).canDisplay('\u4e2d')) {
                cjk++;
            }
        }
        Font chosen = uiFont();
        sb.append("[font-probe] families=").append(families.length)
          .append(" cjkFamilies=").append(cjk)
          .append(" chosen=").append(chosen.getFamily())
          .append(" canDisplay=").append(chosen.canDisplay('\u4e2d'));
        String line = sb.toString();
        log(line);
        System.out.println(line);
    }

    /** Font with real CJK glyphs; the default Swing font may lack them too. */
    private static Font uiFont() {
        String[] candidates = {"Microsoft YaHei UI", "Microsoft YaHei", "SimSun", "Dialog"};
        for (String name : candidates) {
            Font f = new Font(name, Font.PLAIN, 12);
            if (f.getFamily().equalsIgnoreCase(name) || f.canDisplayUpTo("中") == -1) {
                return f;
            }
        }
        return new Font(Font.SANS_SERIF, Font.PLAIN, 12);
    }

    private void buildSwingMenu() {
        Font font = uiFont();
        swingMenu = new JPopupMenu();
        swingMenu.setFont(font);
        swingMenu.add(menuItemSwing(font, "tray_up", e -> runCli(Kind.UP, "up")));
        swingMenu.add(menuItemSwing(font, "tray_down", e -> runCli(Kind.DOWN, "down")));
        swingMenu.add(menuItemSwing(font, "tray_status", e -> notifyStatus()));
        swingMenu.addSeparator();
        swingMenu.add(menuItemSwing(font, "tray_browser",
                e -> runScript(Kind.BROWSER, "start-chrome-ech.ps1")));
        swingMenu.add(menuItemSwing(font, "tray_install",
                e -> runScript(Kind.INSTALL, "setup-browser-ech.ps1")));
        swingMenu.add(menuItemSwing(font, "tray_revert", e -> {
            if (confirm("confirm_revert")) {
                runScript(Kind.REVERT, "setup-browser-ech.ps1", "-Revert");
            }
        }));
        swingMenu.addSeparator();
        swingMenu.add(menuItemSwing(font, "tray_log", e -> openLog()));
        JCheckBoxMenuItem auto = new JCheckBoxMenuItem(
                text.getOrDefault("tray_autostart", "Start with Windows"), autostartEnabled());
        auto.setFont(font);
        auto.addItemListener(e -> toggleAutostartSwing(auto));
        swingMenu.add(auto);
        swingMenu.addSeparator();
        swingMenu.add(menuItemSwing(font, "tray_exit", e -> quit()));
    }

    private JMenuItem menuItemSwing(Font font, String key, java.awt.event.ActionListener action) {
        JMenuItem item = new JMenuItem(text.getOrDefault(key, key));
        item.setFont(font);
        item.addActionListener(action);
        return item;
    }

    private void showSwingMenu(java.awt.event.MouseEvent e) {
        if (swingMenu == null) {
            return;
        }
        try {
            // JPopupMenu needs an invoker WITH a native peer. Passing null (the
            // previous attempt) made Swing refuse to display it at all, which is
            // why right-clicking appeared to do nothing. A 1x1 transparent JWindow
            // gives the popup a real anchor we can position freely.
            if (menuInvoker == null) {
                menuInvoker = new JWindow();
                menuInvoker.setBackground(new Color(0, 0, 0, 0));
                menuInvoker.setAlwaysOnTop(true);
                swingMenu.addPopupMenuListener(new javax.swing.event.PopupMenuListener() {
                    @Override
                    public void popupMenuWillBecomeVisible(javax.swing.event.PopupMenuEvent ev) {
                    }

                    @Override
                    public void popupMenuWillBecomeInvisible(javax.swing.event.PopupMenuEvent ev) {
                        if (menuInvoker != null) {
                            menuInvoker.setVisible(false);
                        }
                    }

                    @Override
                    public void popupMenuCanceled(javax.swing.event.PopupMenuEvent ev) {
                        if (menuInvoker != null) {
                            menuInvoker.setVisible(false);
                        }
                    }
                });
            }
            swingMenu.pack();
            Dimension size = swingMenu.getPreferredSize();

            // Tray MouseEvent coordinates are unreliable on Windows: they are often
            // 0,0 or icon-relative, which is what put the menu in the wrong place.
            // Query the real cursor position instead.
            Point cursor;
            try {
                cursor = MouseInfo.getPointerInfo().getLocation();
            } catch (Exception ex) {
                cursor = new Point(e.getXOnScreen(), e.getYOnScreen());
            }

            GraphicsConfiguration gc = e.getComponent() != null
                    ? e.getComponent().getGraphicsConfiguration()
                    : GraphicsEnvironment.getLocalGraphicsEnvironment()
                            .getDefaultScreenDevice().getDefaultConfiguration();
            Rectangle screen = gc.getBounds();
            int x = cursor.x;
            int y = cursor.y - size.height;
            // above the cursor normally; below it if that would leave the screen
            if (y < screen.y) {
                y = cursor.y;
            }
            if (x + size.width > screen.x + screen.width) {
                x = screen.x + screen.width - size.width;
            }
            if (x < screen.x) {
                x = screen.x;
            }
            menuInvoker.setBounds(x, y, 1, 1);
            menuInvoker.setVisible(true);
            swingMenu.show(menuInvoker, 0, 0);
        } catch (Exception ex) {
            log("[tray] cannot show Swing menu: " + ex);
            fallbackNativeMenu();
        }
    }

    private JWindow menuInvoker;

    private PopupMenu nativeFallback;

    /** Last resort: the native menu always appears, even if its CJK glyphs may not. */
    private void fallbackNativeMenu() {
        try {
            if (nativeFallback == null) {
                nativeFallback = new PopupMenu();
                addNative("tray_up", () -> runCli(Kind.UP, "up"));
                addNative("tray_down", () -> runCli(Kind.DOWN, "down"));
                addNative("tray_status", () -> notifyStatus());
                addNative("tray_log", () -> openLog());
                addNative("tray_exit", () -> quit());
            }
            nativeFallback.show(null, 0, 0);
        } catch (Exception ex) {
            log("[tray] native fallback failed too: " + ex);
        }
    }

    private void addNative(String key, Runnable action) {
        MenuItem item = new MenuItem(text.getOrDefault(key, key));
        item.setFont(uiFont());          // may or may not be honoured by Win32
        item.addActionListener(ev -> action.run());
        nativeFallback.add(item);
    }

    private void toggleAutostartSwing(JCheckBoxMenuItem item) {
        boolean enable = item.isSelected();
        boolean ok = writeAutostart(enable);
        if (!ok) {
            item.setSelected(!enable);
        }
    }

    private boolean writeAutostart(boolean enable) {
        try {
            Process proc = enable
                    ? new ProcessBuilder("reg", "add", RUN_KEY, "/v", RUN_VALUE,
                            "/t", "REG_SZ", "/d", autostartCommand(), "/f")
                            .redirectErrorStream(true).start()
                    : new ProcessBuilder("reg", "delete", RUN_KEY, "/v", RUN_VALUE, "/f")
                            .redirectErrorStream(true).start();
            proc.waitFor(10, TimeUnit.SECONDS);
            log("[autostart] " + (enable ? "enabled: " + autostartCommand() : "disabled"));
            notifyNow(text.getOrDefault("title", "sniroute"),
                    enable ? text.getOrDefault("notify_autostart_on", "autostart on")
                            : text.getOrDefault("notify_autostart_off", "autostart off"),
                    TrayIcon.MessageType.INFO);
            return true;
        } catch (Exception ex) {
            log("[autostart] failed: " + ex.getMessage());
            return false;
        }
    }

    private void dispose() {
        if (trayIcon != null) {
            SystemTray.getSystemTray().remove(trayIcon);
        }
    }

    private void quit() {
        quitting = true;
        dispose();
        exitLatch.countDown();
    }

    // ------------------------------------------------------------------- i18n
    private void loadStrings() {
        Path p = repo.resolve("data/gui-strings.json");
        if (!Files.isRegularFile(p)) {
            return;
        }
        try {
            String json = Files.readString(p, StandardCharsets.UTF_8);
            for (String[] pair : TinyJson.flatObject(json)) {
                text.put(pair[0], pair[1]);
            }
        } catch (Exception ex) {
            log("[i18n] " + ex.getMessage());
        }
    }

    // ---------------------------------------------------------------- actions
    private void runCli(Kind kind, String... cliArgs) {
        List<String> cmd = new ArrayList<>();
        cmd.add(repo.resolve("sniroute.cmd").toString());
        cmd.addAll(List.of(cliArgs));
        runProcess(cmd, kind, "sniroute " + String.join(" ", cliArgs));
    }

    private void runScript(Kind kind, String scriptName, String... extra) {
        Path script = repo.resolve("scripts").resolve(scriptName);
        if (!Files.isRegularFile(script)) {
            notifyNow("sniroute", "missing " + script, TrayIcon.MessageType.ERROR);
            return;
        }
        List<String> cmd = new ArrayList<>(List.of(
                "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.toString()));
        cmd.addAll(List.of(extra));
        runProcess(cmd, kind, scriptName + " " + String.join(" ", extra));
    }

    /** Which user-facing sentence a finished action should produce. */
    private enum Kind { UP, DOWN, STATUS, INSTALL, BROWSER, REVERT, GENERIC }

    private void runProcess(List<String> cmd, Kind kind, String label) {
        if (busy) {
            notifyNow("sniroute", text.getOrDefault("notify_busy", "busy, ignored"),
                    TrayIcon.MessageType.INFO);
            return;
        }
        busy = true;
        log("=== " + label + " ===");
        Thread worker = new Thread(() -> {
            int code;
            try {
                ProcessBuilder pb = new ProcessBuilder(cmd);
                pb.redirectErrorStream(true);
                pb.directory(repo.toFile());
                Process proc = pb.start();
                try (BufferedReader r = new BufferedReader(
                        new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = r.readLine()) != null) {
                        log(line);
                    }
                }
                if (!proc.waitFor(180, TimeUnit.SECONDS)) {
                    proc.destroyForcibly();
                    log("[timeout] killed after 180s");
                    code = 124;
                } else {
                    code = proc.exitValue();
                }
            } catch (Exception ex) {
                log("[ERROR] " + ex.getMessage());
                code = 1;
            }
            log("--- " + label + " exit " + code + " ---");
            busy = false;
            refresh();
            notifyResult(kind, code, label);
        }, "sniroute-action");
        worker.setDaemon(true);
        worker.start();
    }

    private boolean confirm(String key) {
        return JOptionPane.showConfirmDialog(null,
                text.getOrDefault(key, key),
                text.getOrDefault("title_confirm", "Confirm"),
                JOptionPane.OK_CANCEL_OPTION) == JOptionPane.OK_OPTION;
    }

    private void openLog() {
        try {
            Files.createDirectories(actionLog.getParent());
            if (!Files.exists(actionLog)) {
                Files.writeString(actionLog, "");
            }
            new ProcessBuilder("notepad.exe", actionLog.toString()).start();
        } catch (Exception ex) {
            notifyNow("sniroute", "cannot open log: " + ex.getMessage(), TrayIcon.MessageType.ERROR);
        }
    }

    // ---------------------------------------------------------------- status
    private void refresh() {
        if (!trayReady || trayIcon == null || quitting) {
            return;
        }
        boolean doh = dohListening();
        boolean nginx = nginxRunning();
        trayIcon.setToolTip("sniroute  DoH:" + (doh ? "up" : "down")
                + "  nginx:" + (nginx ? "up" : "down"));
        trayIcon.setImage(trayImage(doh && nginx ? OK_COLOR : BAD_COLOR));
    }

    private void notifyStatus() {
        boolean doh = dohListening();
        boolean nginx = nginxRunning();
        boolean hosts = hostsHasEntry();
        boolean ca = caTrusted();
        String tpl = text.getOrDefault("notify_status_tpl", "DoH:%s nginx:%s hosts:%s ca:%s");
        String msg = String.format(tpl,
                doh ? text.getOrDefault("on", "up") : text.getOrDefault("off", "down"),
                nginx ? text.getOrDefault("on", "up") : text.getOrDefault("off", "down"),
                hosts ? text.getOrDefault("yes", "yes") : text.getOrDefault("no", "no"),
                ca ? text.getOrDefault("yes", "yes") : text.getOrDefault("no", "no"));
        notifyNow(text.getOrDefault("title", "sniroute"), msg, TrayIcon.MessageType.INFO);
        log("[status] " + msg);
    }

    /**
     * Turn an exit code into a sentence a user can act on.
     *
     * <p>Learned from SNIBypassGUI's release notes: report what happened in user
     * terms and say what to do next. A bare "exit 2" tells nobody anything, so
     * the code only ever appears as a parenthetical detail.
     */
    private void notifyResult(Kind kind, int code, String label) {
        String title = text.getOrDefault("title", "sniroute");
        String okKey;
        String failKey;
        switch (kind) {
            case UP -> {
                okKey = "notify_up_ok";
                failKey = "notify_up_fail";
            }
            case DOWN -> {
                okKey = "notify_down_ok";
                failKey = "notify_fail";
            }
            case INSTALL -> {
                okKey = "notify_install_ok";
                failKey = "notify_install_fail";
            }
            case BROWSER -> {
                okKey = "notify_browser_ok";
                failKey = "notify_fail";
            }
            case REVERT -> {
                okKey = "notify_revert_ok";
                failKey = "notify_fail";
            }
            default -> {
                okKey = "notify_done";
                failKey = "notify_fail";
            }
        }
        if (code == 0) {
            notifyNow(title, text.getOrDefault(okKey, label + ": done"), TrayIcon.MessageType.INFO);
        } else {
            String tpl = text.getOrDefault(failKey, label + " failed (exit %s)");
            notifyNow(title, String.format(tpl, code), TrayIcon.MessageType.WARNING);
        }
    }

    private void notifyFirstRun() {
        if (notified) {
            return;
        }
        notified = true;
        notifyNow(text.getOrDefault("title", "sniroute"),
                text.getOrDefault("tray_msg_first", "Running in the tray."),
                TrayIcon.MessageType.INFO);
    }

    private void notifyNow(String title, String message, TrayIcon.MessageType type) {
        if (trayReady && trayIcon != null) {
            trayIcon.displayMessage(title, message, type);
        }
    }

    private boolean dohListening() {
        try (java.net.Socket s = new java.net.Socket()) {
            s.connect(new java.net.InetSocketAddress("127.0.0.1", DOH_PORT), 300);
            return true;
        } catch (IOException ignored) {
            return false;
        }
    }

    private boolean nginxRunning() {
        Path pidFile = repo.resolve("logs/nginx.pid");
        try {
            long pid = Long.parseLong(Files.readString(pidFile).trim());
            return ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false);
        } catch (Exception ignored) {
            return false;
        }
    }

    private boolean hostsHasEntry() {
        try {
            return Files.readString(hostsPath, StandardCharsets.UTF_8)
                    .contains("# >>> sniroute doh >>>");
        } catch (Exception ignored) {
            return false;
        }
    }

    /**
     * Windows trust store is not readable from plain Java, so ask certutil.
     *
     * <p>Must be -user: we install into the CURRENT USER's root store precisely so
     * that no elevation is needed. Querying the machine store instead reports "not
     * trusted" on every start and re-runs the installer each time.
     */
    private boolean caTrusted() {
        if (!Files.isRegularFile(repo.resolve("ca/rootCA.crt"))) {
            return false;
        }
        try {
            Process p = new ProcessBuilder("certutil", "-user", "-store", "Root")
                    .redirectErrorStream(true).start();
            String out = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            p.waitFor(15, TimeUnit.SECONDS);
            String lower = out.toLowerCase();
            return lower.contains("kynarain") || lower.contains("sniroute");
        } catch (Exception ignored) {
            return false;
        }
    }

    // ------------------------------------------------------------- autostart
    private String autostartCommand() {
        Path exe = repo.resolve("Sniroute.exe");
        if (Files.isRegularFile(exe)) {
            return "\"" + exe + "\"";
        }
        String javaCmd = ProcessHandle.current().info().command().orElse("javaw.exe");
        String javaw = javaCmd.replaceAll("(?i)java\\.exe$", "javaw.exe");
        String cp = System.getProperty("java.class.path");
        return "\"" + javaw + "\" -cp \"" + cp + "\" SnirouteGui";
    }

    private boolean autostartEnabled() {
        try {
            Process p = new ProcessBuilder("reg", "query", RUN_KEY, "/v", RUN_VALUE)
                    .redirectErrorStream(true).start();
            String out = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            p.waitFor(5, TimeUnit.SECONDS);
            return p.exitValue() == 0 && out.contains(RUN_VALUE);
        } catch (Exception ignored) {
            return false;
        }
    }

    // ------------------------------------------------------------------ misc
    private void log(String line) {
        try {
            Files.createDirectories(actionLog.getParent());
            Files.writeString(actionLog, line + System.lineSeparator(), StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (Exception ignored) {
            // logging must never break an action
        }
    }

    private static Image trayImage(Color status) {
        int size = 32;
        // Prefer the shared icon so the tray matches the exe/MSI icon exactly.
        // A small status dot is composited on top because colour is the only
        // signal available when the window is gone.
        BufferedImage img = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BILINEAR);

        Image base = loadIconImage();
        if (base != null) {
            g.drawImage(base, 0, 0, size, size, null);
        } else {
            g.setColor(new Color(31, 111, 235));
            g.fillRoundRect(0, 0, size - 1, size - 1, 8, 8);
            g.setColor(Color.WHITE);
            g.setStroke(new BasicStroke(2.5f));
            g.drawPolygon(new int[]{8, 24, 24, 16, 8}, new int[]{7, 7, 17, 25, 17}, 5);
        }
        g.setColor(status);
        g.fillOval(size - 11, size - 11, 10, 10);
        g.setColor(Color.WHITE);
        g.setStroke(new BasicStroke(1.5f));
        g.drawOval(size - 11, size - 11, 10, 10);
        g.dispose();
        return img;
    }

    /** gui/icon.png is generated by tools/make_icon.py; missing is not fatal. */
    private static Image loadIconImage() {
        try {
            Path png = ICON_OWNER.resolve("gui/icon.png");
            if (Files.isRegularFile(png)) {
                return javax.imageio.ImageIO.read(png.toFile());
            }
        } catch (Exception ignored) {
            // fall through to the drawn shape
        }
        return null;
    }

    /** Set once at startup so the static helper above knows where the repo is. */
    private static Path ICON_OWNER = Paths.get(".");

    /**
     * Minimal JSON reader: pulls out the flat "key": "string" pairs this app needs
     * from gui-strings.json. No dependency, and it ignores nested values rather
     * than guessing at them.
     */
    static final class TinyJson {
        static List<String[]> flatObject(String json) {
            List<String[]> out = new ArrayList<>();
            int i = 0;
            int n = json.length();
            while (i < n) {
                int q = json.indexOf('"', i);
                if (q < 0) {
                    break;
                }
                int keyEnd = json.indexOf('"', q + 1);
                if (keyEnd < 0) {
                    break;
                }
                String key = json.substring(q + 1, keyEnd);
                int colon = json.indexOf(':', keyEnd);
                if (colon < 0) {
                    break;
                }
                int v = colon + 1;
                while (v < n && Character.isWhitespace(json.charAt(v))) {
                    v++;
                }
                if (v < n && json.charAt(v) == '"') {
                    StringBuilder sb = new StringBuilder();
                    int k = v + 1;
                    while (k < n) {
                        char c = json.charAt(k);
                        if (c == '\\' && k + 1 < n) {
                            char esc = json.charAt(k + 1);
                            switch (esc) {
                                case 'n' -> sb.append('\n');
                                case 't' -> sb.append('\t');
                                case 'r' -> sb.append('\r');
                                default -> sb.append(esc);
                            }
                            k += 2;
                            continue;
                        }
                        if (c == '"') {
                            break;
                        }
                        sb.append(c);
                        k++;
                    }
                    out.add(new String[]{key, sb.toString()});
                    i = k + 1;
                } else {
                    i = colon + 1;
                }
            }
            return out;
        }
    }
}
