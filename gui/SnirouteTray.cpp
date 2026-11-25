// SnirouteTray.cpp - Win32 tray application for sniroute.
//
// Replaces the Java GUI. Why C++: the Java build rendered Chinese as boxes in the
// native tray menu and its Swing popup could not be positioned reliably next to
// the notification area. A native Win32 menu uses the system font, so Chinese
// renders correctly, and TrackPopupMenu positions itself at the cursor.
//
// Design notes:
//   * No window is ever shown; the tray icon is the whole interface.
//   * Every action shells out to sniroute.cmd so the tray and the CLI cannot drift.
//   * All user-visible text lives in data\tray-strings.txt (UTF-8, key=value), so
//     the wording can be changed without recompiling. This file stays ASCII only.
//   * Passwords/keys are never touched here; the app only orchestrates.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <algorithm>
#include <vector>
#include <map>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr UINT WM_TRAY_CB = WM_APP + 1;
constexpr UINT WM_NAT_DONE = WM_APP + 2;   // NAT result, lParam = new std::wstring
constexpr UINT TIMER_STATUS = 1;
constexpr UINT TIMER_REFRESH = 2;   // upstream health sweep
constexpr UINT TIMER_WARP = 3;      // WARP health probe
constexpr UINT TIMER_STARTUP_UP = 4; // one-shot: run up after first-run setup
constexpr UINT WM_WARP_ALERT = WM_APP + 21;
constexpr UINT_PTR ID_TRAY = 1;
constexpr UINT IDM_UP = 1001;
constexpr UINT IDM_DOWN = 1002;
constexpr UINT IDM_STATUS = 1003;
constexpr UINT IDM_BROWSER = 1004;
constexpr UINT IDM_INSTALL = 1005;
constexpr UINT IDM_POLICY = 1010;
constexpr UINT IDM_GLOBAL = 1011;
constexpr UINT IDM_SITE_FIRST = 3000;   // + index into g_siteUrls
constexpr UINT IDM_REFRESH = 1013;  // refresh upstream candidates
constexpr UINT IDM_NAT = 1012;      // NAT type check
constexpr UINT IDM_WARP_ON = 1016;      // connect Cloudflare WARP
constexpr UINT IDM_WARP_OFF = 1017;     // disconnect Cloudflare WARP
constexpr UINT IDM_WARP_INSTALL = 1018; // one-click winget install
constexpr UINT IDM_WARP_EXPORT = 1019;  // export Clash YAML
constexpr UINT IDM_UPDATE = 1020;       // check for updates
constexpr UINT IDM_GHA_FETCH = 1021;    // download via GitHub Actions
constexpr UINT WM_GHA_DONE = WM_APP + 24;
constexpr UINT WM_UPDATE_DONE = WM_APP + 23;
constexpr UINT WM_WARP_INSTALL_DONE = WM_APP + 22;
constexpr UINT IDM_REVERT = 1006;
constexpr UINT IDM_LOG = 1007;
constexpr UINT IDM_AUTOSTART = 1008;
constexpr UINT IDM_EXIT = 1009;

struct Strings {
    std::map<std::wstring, std::wstring> kv;

    const wchar_t* get(const wchar_t* key, const wchar_t* fallback) const {
        auto it = kv.find(key);
        return it == kv.end() ? fallback : it->second.c_str();
    }
};

std::wstring g_root;           // install / repo root
std::wstring g_logPath;
std::wstring g_stringsPath;
std::wstring g_iconPath;
std::wstring g_cliPath;
Strings g_str;
HWND g_hwnd = nullptr;
HICON g_iconOk = nullptr;
HICON g_iconBad = nullptr;
UINT g_taskbarCreated = 0;
CRITICAL_SECTION g_logCs;
std::atomic<bool> g_busy{false};
std::vector<std::wstring> g_siteUrls;    // submenu id -> url, rebuilt with the menu

// ---------------------------------------------------------------- utilities
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

void Log(const std::wstring& line) {
    EnterCriticalSection(&g_logCs);
    FILE* f = nullptr;
    _wfopen_s(&f, g_logPath.c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(stamp, L"[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    std::string bytes = WideToUtf8(std::wstring(stamp) + line + L"\r\n");
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    LeaveCriticalSection(&g_logCs);
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    size_t e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
}

// ---------------------------------------------------------------- strings
void LoadStrings() {
    FILE* f = nullptr;
    _wfopen_s(&f, g_stringsPath.c_str(), L"rb");
    if (!f) {
        Log(L"[strings] missing " + g_stringsPath);
        return;
    }
    std::string raw;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF) {
        raw = raw.substr(3);
    }
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        // UTF-16LE BOM: convert the rest
        std::wstring w((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
        raw = WideToUtf8(w);
    }

    size_t start = 0;
    while (start <= raw.size()) {
        size_t nl = raw.find('\n', start);
        std::string line = raw.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? raw.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::wstring key = Utf8ToWide(line.substr(0, eq));
        std::wstring val = Utf8ToWide(line.substr(eq + 1));
        g_str.kv[key] = Trim(val);
    }
    Log(L"[strings] loaded " + std::to_wstring(g_str.kv.size()) + L" keys");
}

std::wstring FormatWithNumber(const wchar_t* tpl, int number) {
    std::wstring out;
    for (const wchar_t* p = tpl; *p; ++p) {
        if (p[0] == L'%' && p[1] == L'd') {
            out += std::to_wstring(number);
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}

// ---------------------------------------------------------------- status
bool PortListening(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(s, (sockaddr*)&addr, sizeof(addr)) == 0;
    closesocket(s);
    return ok;
}

bool NginxRunning() {
    std::wstring pidFile = g_root + L"\\logs\\nginx.pid";
    FILE* f = nullptr;
    _wfopen_s(&f, pidFile.c_str(), L"rb");
    if (!f) return false;
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    DWORD pid = (DWORD)_strtoui64(buf, nullptr, 10);
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

bool HostsHasEntry() {
    wchar_t win[MAX_PATH];
    GetEnvironmentVariableW(L"SystemRoot", win, MAX_PATH);
    std::wstring path = std::wstring(win) + L"\\System32\\drivers\\etc\\hosts";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return false;
    std::string raw;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);
    return raw.find("sniroute") != std::string::npos;
}

// ---------------------------------------------------------------- icons
HICON MakeStatusIcon(bool ok) {
    const int size = 32;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HGDIOBJ oldColor = SelectObject(mem, color);
    HGDIOBJ oldMask = SelectObject(mem, mask);

    PatBlt(mem, 0, 0, size, size, BLACKNESS);

    HICON base = (HICON)LoadImageW(nullptr, g_iconPath.c_str(), IMAGE_ICON, size, size,
                                   LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (base) {
        DrawIconEx(mem, 0, 0, base, size, size, 0, nullptr, DI_NORMAL);
        DestroyIcon(base);
    }

    // status dot, bottom-right
    HBRUSH brush = CreateSolidBrush(ok ? RGB(0, 160, 60) : RGB(200, 40, 30));
    HGDIOBJ oldBrush = SelectObject(mem, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(mem, pen);
    Ellipse(mem, size - 12, size - 12, size, size);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    SelectObject(mem, oldColor);
    SelectObject(mem, oldMask);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------- tray
void SetTooltip(const std::wstring& text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring BuildTooltip(bool doh, bool nginx);

void UpdateStatus(bool notifyBalloon) {
    bool doh = PortListening(5444);
    bool nginx = NginxRunning();
    bool ok = doh && nginx;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = ok ? g_iconOk : g_iconBad;
    std::wstring tip = BuildTooltip(doh, nginx);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    if (notifyBalloon) {
        std::wstring msg = g_str.get(L"label_doh", L"DoH");
        msg += L":";
        msg += doh ? g_str.get(L"on", L"up") : g_str.get(L"off", L"down");
        msg += L"  ";
        msg += g_str.get(L"label_nginx", L"nginx");
        msg += L":";
        msg += nginx ? g_str.get(L"on", L"up") : g_str.get(L"off", L"down");
        msg += L"  ";
        msg += g_str.get(L"label_hosts", L"hosts");
        msg += L":";
        msg += HostsHasEntry() ? g_str.get(L"yes", L"yes") : g_str.get(L"no", L"no");

        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(nid.szInfoTitle, g_str.get(L"tip", L"sniroute"), _TRUNCATE);
        wcsncpy_s(nid.szInfo, msg.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        Log(L"[status] " + msg);
    }
}

void Balloon(const std::wstring& title, const std::wstring& msg, DWORD flags) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = flags;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, msg.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------- actions
struct ActionSpec {
    const wchar_t* arg;       // sniroute.cmd argument (empty when running a script)
    const wchar_t* script;    // script file name under scripts\, or empty
    const wchar_t* scriptArg; // extra argument passed to the script, or empty
    const wchar_t* okKey;
    const wchar_t* failKey;
    const wchar_t* okFallback;
    const wchar_t* failFallback;
    // Optional message for exit code 2. Only the first-run setup uses it: it means
    // "certificate done, browser policy needs one elevation", not a failure.
    const wchar_t* needKey;
};

// NAT type check: runs tools/natcheck.py --summary and shows the conclusion.
// It takes 5-10 seconds (three STUN bindings plus mapping and filtering probes),
// so it runs on a worker thread and posts the text back with WM_NAT_DONE.
void RunNatProbe() {
    if (g_busy.exchange(true)) {
        Balloon(g_str.get(L"tip", L"sniroute"), g_str.get(L"notify_busy", L"busy"), NIIF_INFO);
        return;
    }
    HANDLE worker = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        std::wstring cmdline = L"cmd.exe /c \"\"" + g_cliPath + L"\" nat --summary 2>&1\"";
        Log(L"[nat] " + cmdline);

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        CreatePipe(&readEnd, &writeEnd, &sa, 0);
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;
        PROCESS_INFORMATION pi{};
        std::wstring wcmd = cmdline;
        BOOL started = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi);
        CloseHandle(writeEnd);
        if (!started) {
            CloseHandle(readEnd);
            PostMessageW(g_hwnd, WM_NAT_DONE, 0, (LPARAM)(new std::wstring(
                g_str.get(L"nat_failed", L"NAT check failed to start"))));
            g_busy = false;
            return 1;
        }
        std::string raw;
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            raw.append(buf, got);
        }
        CloseHandle(readEnd);
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        std::wstring text;
        if (!raw.empty()) {
            int need = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
            if (need > 0) {
                text.resize((size_t)need);
                MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), text.data(), need);
            }
        }
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
        if (text.empty()) text = g_str.get(L"nat_empty", L"NAT check returned no output");
        PostMessageW(g_hwnd, WM_NAT_DONE, 0, (LPARAM)(new std::wstring(text)));
        g_busy = false;
        return 0;
    }, nullptr, 0, nullptr);
    if (worker) CloseHandle(worker);
}

void RunAction(const ActionSpec& spec) {
    if (g_busy.exchange(true)) {
        Balloon(g_str.get(L"tip", L"sniroute"), g_str.get(L"notify_busy", L"busy"), NIIF_INFO);
        return;
    }
    struct Ctx { ActionSpec spec; };
    Ctx* ctx = new Ctx{spec};
    HANDLE worker = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        ActionSpec spec = ((Ctx*)p)->spec;
        delete (Ctx*)p;
        std::wstring cmdline;
        if (spec.script && *spec.script) {
            cmdline = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"";
            cmdline += g_root + L"\\scripts\\" + spec.script + L"\"";
            if (spec.scriptArg && *spec.scriptArg) {
                cmdline += L" ";
                cmdline += spec.scriptArg;
            }
        } else {
            cmdline = L"cmd.exe /c \"\"" + g_cliPath + L"\" " + spec.arg + L"\"";
        }

        Log(L"[action] " + cmdline);

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        CreatePipe(&readEnd, &writeEnd, &sa, 0);
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;
        si.hStdInput = nullptr;
        PROCESS_INFORMATION pi{};

        std::wstring wcmd = cmdline;
        BOOL started = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi);
        CloseHandle(writeEnd);
        if (!started) {
            CloseHandle(readEnd);
            Log(L"[action] CreateProcess failed " + std::to_wstring(GetLastError()));
            Balloon(g_str.get(L"tip", L"sniroute"),
                    FormatWithNumber(g_str.get(L"notify_fail", L"failed (%d)"), (int)GetLastError()),
                    NIIF_WARNING);
            g_busy = false;
            return 1;
        }

        std::string raw;
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            raw.append(buf, got);
        }
        CloseHandle(readEnd);
        WaitForSingleObject(pi.hProcess, 240000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // keep the raw output for diagnosis; it may be any codepage, so store bytes
        Log(L"[action] exit " + std::to_wstring(code));
        if (!raw.empty()) {
            EnterCriticalSection(&g_logCs);
            FILE* f = nullptr;
            _wfopen_s(&f, g_logPath.c_str(), L"ab");
            if (f) {
                fwrite(raw.data(), 1, raw.size(), f);
                fclose(f);
            }
            LeaveCriticalSection(&g_logCs);
        }

        if (code == 2 && spec.needKey) {
            // exit code 2 from first-run setup: certificate is done, the browser
            // policy needs one elevation - not a failure
            Balloon(g_str.get(L"tip", L"sniroute"), g_str.get(spec.needKey, L"Needs admin once"),
                    NIIF_WARNING);
        } else if (code == 0) {
            Balloon(g_str.get(L"tip", L"sniroute"), g_str.get(spec.okKey, spec.okFallback), NIIF_INFO);
        } else {
            Balloon(g_str.get(L"tip", L"sniroute"),
                    FormatWithNumber(g_str.get(spec.failKey, spec.failFallback), (int)code),
                    NIIF_WARNING);
        }
        g_busy = false;
        PostMessageW(g_hwnd, WM_TIMER, TIMER_STATUS, 0);
        return 0;
    }, ctx, 0, nullptr);
    if (worker) CloseHandle(worker);
}

const ActionSpec kUp{L"up", L"", L"", L"notify_up_ok", L"notify_up_fail",
                     L"Started", L"Start failed (%d)", nullptr};
const ActionSpec kDown{L"down", L"", L"", L"notify_down_ok", L"notify_fail",
                       L"Stopped", L"Failed (%d)", nullptr};
const ActionSpec kBrowser{L"", L"start-chrome-ech.ps1", L"", L"notify_browser_ok", L"notify_fail",
                          L"Browser launched", L"Failed (%d)", nullptr};
const ActionSpec kInstall{L"", L"setup-browser-ech.ps1", L"", L"notify_install_ok",
                          L"notify_install_fail", L"Certificate installed", L"Install failed (%d)", nullptr};
const ActionSpec kRevert{L"", L"revert-all.ps1", L"", L"notify_revert_ok",
                         L"notify_fail", L"Reverted", L"Failed (%d)", nullptr};
// Points the browser at the local DoH endpoint through policy. Elevates only when
// the per-user policy key is unwritable; on a normal install it never prompts.
const ActionSpec kPolicy{L"", L"set-browser-policy.ps1", L"", L"notify_policy_ok",
                         L"notify_policy_fail", L"Browser configured", L"Failed (%d)", nullptr};
// Runs once, when the CA is not yet trusted: installs the CA and sets the policy.
const ActionSpec kGlobal{L"", L"nrpt.ps1", L"-Install", L"notify_global_ok",
                       L"notify_global_fail", L"Global mode on", L"Failed (%d)", nullptr};
const ActionSpec kFirstRun{L"", L"first-run-setup.ps1", L"", L"notify_first_ok",
                           L"notify_first_fail", L"Setup complete", L"Setup failed (%d)",
                           L"notify_first_need_admin"};

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

// Ask the OS whether our CA is trusted for the CURRENT USER. Querying the machine
// store instead would report "not trusted" forever, because we install per-user.
bool CaTrusted() {
    std::wstring cmd = L"certutil.exe -user -store Root";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = cmd;
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        return false;
    }
    std::string raw;
    char buf[2048];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) raw.append(buf, got);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::wstring wide = Utf8ToWide(raw);
    for (auto& ch : wide) ch = (wchar_t)towlower(ch);
    return wide.find(L"kynarain") != std::wstring::npos ||
           wide.find(L"sniroute") != std::wstring::npos;
}

// WARP 闂佸搫瀚烽崹浼村箚娴ｅ壊鍟呴柤鐓庡缁犻箖鏌熼幁鎺戝姌闁逞屽墮閸婄晫鎹㈤幋锕€鐐?0=闂佸搫鐗滄禍婊堟偩閵娧勫晳?1=閻庤鐡曠亸顏嗘崲濞戙垹绠?2=闂佸搫鐗滄禍锝囨崲濞戙垹绠抽柕澶涚岛閸?
// 闂?warp-cli status 闂佸憡甯囬崐鏇㈡偩妤ｅ啯鏅€光偓閸愶絽浜鹃悘鐐跺亹閻熸繈鏌￠崟闈涚仯婵犫偓閸涱垳纾鹃柟瀛樼箓楠炪垽鏌ㄥ☉娆戞惒ASQUE 闂佸憡顨呯换妤咁敊閸涱喚鈻旈悗锝庡墰缁夊綊鏌涘鐑╁亾閾忣偉鍚繛杈剧到濡瑧绮婇敃鍌涚厒闊洦鎸哥拋鏌ユ煠閾忣偄鐏婄紒妤€顦甸弻鍛潩閸撗咁槷
// 闂佸憡鐟禍婵嗭耿?status 闁?Connected 闂佸綊娼х粔鎶藉船椤掑倹鍋橀柕濞垮労閸氬倿鏌熼幘顕呮妞ゅ浚鍓涢幏瀣Χ閸曨喚顦伴梻鍌氭啚閸ワ箑顥氶梺鍛婄懐閸ㄦ娊寮妶澶娢?
int WarpState() {
    static const wchar_t* kCandidates[] = {
        L"C:\\Program Files\\Cloudflare\\Cloudflare WARP\\warp-cli.exe",
        L"C:\\Program Files (x86)\\Cloudflare\\Cloudflare WARP\\warp-cli.exe",
    };
    std::wstring cli;
    for (auto c : kCandidates) {
        if (FileExists(c)) { cli = c; break; }
    }
    if (cli.empty()) return 0;

    std::wstring cmd = L"\"" + cli + L"\" --accept-tos status";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return 2;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = cmd;
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) { CloseHandle(readEnd); return 2; }
    std::string raw;
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) raw.append(buf, got);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    for (auto& ch : raw) ch = (char)tolower((unsigned char)ch);
    if (raw.find("connected") != std::string::npos &&
        raw.find("disconnected") == std::string::npos) return 1;
    return 2;
}

// 闂備緡鍋呮穱铏规崲?sniroute.cmd 闂佸湱鐟抽崱鈺傛杸 warp on/off闂佹寧绋戞總鏃傜箔婢跺瞼椹冲璺猴功缁愶繝鏌ㄥ☉妯荤伐arp-cli 闁哄鏅濋崑鐐垫暜閹绢喖鐭楁い鏍ㄧ箓閸樻挳鎮烽弴姘鳖槮闁搞倖鍨圭划鏃堝箳閻愮數顦梺?
void RunWarpAction(const wchar_t* action) {
    std::wstring cmdline = L"cmd.exe /c \"\"" + g_cliPath + L"\" warp " + action + L"\"";
    Log(L"[warp] " + cmdline);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = cmdline;
    if (CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, g_root.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        Balloon(g_str.get(L"tip", L"sniroute"), g_str.get(L"warp_fail", L"WARP action failed"),
                NIIF_WARNING);
    }
}


// 闂?winget 闁诲海鎳撻ˇ鎶剿?Cloudflare WARP闂侀潧妫楅崐浠嬫憘鐎ｎ兘鍋撶憴鍕嗘垿寮婚悢灏佹敠闁规澘澧庣粈澶娾槈閹惧磭啸闁搞倖绮岄蹇涘础閻愯尙鈧鏌涘▎鎰仴闁伙絻鍔庨幉妤呭川椤撶偟妯侀梺?
// 婵炴潙鍚嬮懝楣冩嚇婵犲啰鈻旈柍褜鍓欓埢?UAC闂佹寧绋戝﹢绗癛P 闁荤喐娲戝ù鍥夋繝鍥у珘鐎广儱鎳庨～銈夋煕濠婂啰鐒搁柍鐟扮Ч瀹曟繈鈥﹂幒鏃傤槴闂佹寧绋戦懟顖炴偩閵娧勫晳闁告侗鍘鹃弳姘舵煕閹烘鏁遍柡灞斤躬閹筹絽顭ㄩ崘銊ь槹闂佺粯顭堥崺鏍焵椤戝潡妾烽柍?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 婵炲瓨鍤庨崐妤冨垝?CLI闂佹寧绋掗懝楣冩偩閻樺啿顕辨慨姗嗗墮鐢儵鎮?winget闂佹寧绋戦張顒勬儉閸涙潙瀚?winget 闁诲繐绻愯ぐ澶岀箔閸涱喗濮?Cloudflare 闁诲氦顫夎摫闁哄瞼鍠撻埀顒傛嚀椤︽娊藟婵犲洤绀岄柛娑卞亐閸?
        // 婵炴垶鎸哥粔闈浳ｇ粙鍟冩帟绠涢幘鎶藉彙 Windows 闂備緡鍠涙慨銈咃耿?winget闂佹寧绋戝﹢顢篠C闂侀潧妫旈懗鍓佽姳鐠恒劎涓嶉柍褜鍓熼幃褔宕煎┑鍫濈厬闁荤喐鐟ょ欢銈囨濮樿泛违?
        std::wstring cmd = L"cmd.exe /c \"\"" + g_cliPath + L"\" warp install\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring wcmd = cmd;
        if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            PostMessageW(g_hwnd, WM_WARP_INSTALL_DONE, 0, 0);
            return 1;
        }
        WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Log(L"[warp] winget install exit " + std::to_wstring(code));
        PostMessageW(g_hwnd, WM_WARP_INSTALL_DONE, (WPARAM)code, 0);
        return 0;
    }, nullptr, 0, nullptr);
}


// WARP 闂佸搫鍋嗛崢鑲╂閹达箑瑙﹂幖娣灩鎯熼梺鍝勭墱閸撴瑥锕?DNS 闂佸搫鍊介～澶愩€佸澶婄濠㈣泛锕ョ粈鈧梺鍝勵槶閸庮垶鍩€?
//
// 婵炴垶鎹佸銊у垝閸喓鈻曢柛顐犲劜娴犳﹢鎮烽弴姘鳖槮闁伙絿濞€閺佸秴顫㈤幀濉昉 闂佸搫瀚烽崹閬嶅汲閿濆瀚夋繛宸簼閻﹀牓姊洪浣规儓缂佽鲸绻勯埀顒傛嚀閸燁垳鏁埀顒傜磼閻愵剙顕滄俊鐐插€块幃浠嬪Ω閿曗偓閻撴洜鈧灚澹嬮崑鎾垛偓鍨閸嬫挸霉閻橆喖鍔欓悹?缂傚倸鍟崹鍫曞蓟閸ャ劎顩?闂佹寧绋戦惌渚€鍩€椤掆偓閼活垶鎮块崟顖涒挃闁告侗鍓涢悷鎰版煛閸偄澧繝鈧?
// DoH/nginx 闂佸憡鐟崹鐢稿礂濮椻偓瀹曞爼鎮╂潏顐ゎ槹闂佸憡顨呭ú銊︻殽閸ヮ剙违濞达絽鎽滈埞娑欑節瑜嶉崯銊╁焵椤戣法鍔嶉柛銊ｅ妽缁傛帗鎯旈姀鈥崇处闂佸憡鍑归崹鐗堟叏閳哄懏鍤€鐎光偓閳ь剙顔忔潏銊ョ窞濞撴艾绨遍崑鎾诲磼濠靛牏顔愰梻浣瑰絻閼活垱绂掑☉娆戔枖闁靛牆瀚钘壝瑰鍐惧劀缂?
//   1) 濠碘槅鍋€閸嬫捇鏌?5444(DoH 闂佸憡鎸哥粔鍫曟偪? / 5454(DoH 闂佸憡鑹惧ù鐑筋敂? / 443(nginx) 闂佸搫瀚烽崹浼村箚娓氣偓閺屽牓鎸婃径瀣惈闂?
//   2) 缂傚倸鍊藉銊╁箲閵忋倕绠涢煫鍥ф捣椤忓崬鈽夐幙鍐х盎婵ǜ鍔庨幑鍕箣濠婂拋浼囧┑?sniroute.cmd up 闂佺娉涢敃銈夋偩閻樺磭顩锋い鎺嶇椤庢瑩鎮硅閺€鍗烆焽?
// 闂佺绻堥崝鎴﹀磿鐎甸晲鐒婇弶鍫氭櫅瀹曟洟鏌涘顒勵€楅柤鏉戯躬楠炴捇骞囬杞扮驳闂佹寧绋戦惌渚€顢欓埀顒勬煟椤剙濡介柛鈺傜洴閹矂濡烽埡鈧竟?閻庤鐡曠亸娆戝垝閿熺姴绀嗛柛銉戝嫮顦ラ梺鍝勭墱閸撴瑥锕?DNS 闂佸搫鍊介～澶愩€?闂?
void RestoreLocalDns() {
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        bool ok = PortListening(5444) && PortListening(5454) && PortListening(443);
        if (ok) {
            Log(L"[dns] local chain already up after WARP drop");
            PostMessageW(g_hwnd, WM_WARP_ALERT, 3, 0);
            return 0;
        }
        Log(L"[dns] local chain incomplete, running up");
        std::wstring cmd = L"cmd.exe /c \"\"" + g_cliPath + L"\" up\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring wcmd = cmd;
        bool started = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi) != 0;
        DWORD upExit = 1;
        if (started) {
            WaitForSingleObject(pi.hProcess, 90000);
            GetExitCodeProcess(pi.hProcess, &upExit);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            Log(L"[dns] up exit " + std::to_wstring(upExit));
        }
        // up 婵炴潙鍚嬮懝楣冨储濞戙垹纾?闁荤姍鍥ㄦ暠婵犫偓閸ヮ剙绀夐柍銉ュ暱閺呮瑩鏌涚€ｎ偉澹樺ù鐘茬摠閹棃鏌ㄧ€ｅ灚鈷曢梺鍝勮閸庤崵妲愬┑鍫㈠崥妞ゆ牗鑹剧紞鎾斥槈閹惧磭校婵℃彃鎽滅划鈺冣偓锝庝簻閻撴繈鏌涜箛瀣闁稿骸顕禍鍛婃綇閳轰礁纾块梺闈涙閸婁粙鎮块崱娆愬珰闁靛繆鍓濇禒妯侯熆?30 缂備礁顦扮敮顏嗘?
        // 婵炲濮寸粔宕囩箔婢跺奔鐒婇弶鍫氭櫅瀹曟洟鎮樿箛鎾宠埞闁哥啿鍋撻柣鐘靛劋閸ㄥ磭绮╅弶鎴旀瀻?up闂佹寧绋戦悧濠傦耿閸ヮ剙绀夐柍銉ュ暱鐠佹煡鏌ょ€圭姵顥夐柛銊у缁轰粙宕愰悤浣硅晧婵炴垶鎸搁敃锝囩博閺夋垟鏋庨柨鐔哄Т濞呫垽鏌涘鐓庣仭濠⒀嶇畵瀵剟顢橀妸褏顦梺?
        auto allUp = [](int *miss) -> bool {
            bool a = PortListening(5444), b = PortListening(5454), c = PortListening(443);
            if (miss) *miss = a ? (b ? (c ? 0 : 443) : 5454) : 5444;
            return a && b && c;
        };
        auto waitUp = [&](int seconds) -> bool {
            for (int i = 0; i < seconds; i++) {
                Sleep(1000);
                if (allUp(nullptr)) return true;
            }
            return allUp(nullptr);
        };
        // up 闂佹眹鍔岀€氼參顢楀鍛殰闁告縿鍎冲▔銏ゆ煛婢跺苯鏋庢慨妯稿妽缁傛帡鏁愰崼娑掑亾閸愵喖瀚夌€广儱鎳庨～銈団偓鍦焾瀵埖鍒婇搹瑙勫皫闁告垯鍊楅獮銏㈢磼閺冩垵鐏犵憸鐗堢洴楠炴帡濮€閿涘嫨鍋為梺鍛婄懐娴滅偤寮妶澶婄骇闁靛鍎遍獮鎴﹀箹鐎涙ɑ绌跨紒杈ㄧ箖缁嬪顓奸崼顐ｇ煑闂侀潻璐熼崝宀勬偩閻樼粯鐓傜€广儱鎳忛崕?
        // 闂佸搫鐗嗙粔瀛樻叏閻斿吋鍎嶉柛鏇ㄥ幘瀹曞爼鏌涘▎娆戠М闁革綁顥撻幏鐘活敍濮橆剛浼堥梺?濠电偛鐪查妶鍡楅棷闂?闂?
        bool now = (started && upExit == 0);
        if (!now) now = waitUp(20);
        if (!now) {
            Log(L"[dns] first up did not settle, retrying once");
            std::wstring again = L"cmd.exe /c \"\"" + g_cliPath + L"\" up\"";
            STARTUPINFOW si2{};
            si2.cb = sizeof(si2);
            si2.dwFlags = STARTF_USESHOWWINDOW;
            si2.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi2{};
            std::wstring w2 = again;
            if (CreateProcessW(nullptr, w2.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, g_root.c_str(), &si2, &pi2)) {
                WaitForSingleObject(pi2.hProcess, 90000);
                CloseHandle(pi2.hProcess);
                CloseHandle(pi2.hThread);
            }
            now = waitUp(20);
        }
        int miss = 0;
        allUp(&miss);
        if (!now) Log(L"[dns] missing port " + std::to_wstring(miss));
        Log(now ? L"[dns] local chain restored" : L"[dns] local chain still incomplete");
        PostMessageW(g_hwnd, WM_WARP_ALERT, now ? 3 : 4, 0);
        return 0;
    }, nullptr, 0, nullptr);
}


// WARP 闂佸搫瀚烽崹浼村箚娓氣偓閹姤娼忛埡鍌涙闂佸湱顥愬〒鍦偓闈涚焸瀹曟繈濡搁妷锕€鍓婚梺娲绘娇閸斿﹪鍩€椤掆偓閸婂綊鎮ラ敐鍥ㄥ闁绘鐗婇悗顔济?data\.warp-enabled闂?
//   闁诲孩绋掗敋婵犫偓?  -> 闂佹椿娼块崝宥夊春濞戞碍鏆滈悘鐐靛亾缁?WARP 閻庢鍠掗崑鎾绘煟椤愮喎浜鹃梺鎸庣☉閻楁捇宕抽幖浣歌Е妞ゆ牗纰嶉崐闈浨庨崶銊х畺閻庡灚妫冮弫?
//   婵炴垶鎸哥粔鎾偤閵娾晛鎹?-> 婵帗绋掗…鍫ヮ敇婵犳艾绀傞柟鎯板Г閿涙棃鏌ㄥ☉妯煎濡ょ姴娲畷顐ｇ瑹閳ь剛鍒掗婊勫闁靛牆娲ょ瑧闂佽鍏涚欢銈囨闁秴瑙︽い鏍ㄨ壘琚熼梺鍝勫暞閸庤偐鈧哎鍨藉畷锝夊箣閻樿尙鐤€ WARP 闁哄鏅滈…鍫㈡崲濞戙垺鍎撻柍褜鍓涙禍鍛婃綇閵婏妇鍔烽悗娈垮枓閸嬫捇鏌?
// 闂佹椿娼块崝宥夋偉閿濆洦濯奸柣妤€鐗婇悗顔济归悩鍐插姸闁逞屽墮婵傛梻绮径鎰強妞ゆ牗绻嶉崵?JSON闂佹寧绋戦張顒€危閸涘﹦鈻旈弶鐐村閸熷﹪鏌?.autostart-off 婵烇絽娲︾换鍐偓鍨瀹曘儳浠﹂懖鈺婁紘缂備礁顦粔瀛樼濞戞ǚ鏋栭柡鍥ㄇ氶崑?
bool WarpIntentEnabled() {
    std::wstring marker = g_root + L"\\data\\.warp-enabled";
    return FileExists(marker);
}

void SetWarpIntent(bool enabled) {
    std::wstring marker = g_root + L"\\data\\.warp-enabled";
    if (enabled) {
        HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    } else {
        DeleteFileW(marker.c_str());
    }
}

// 闂佸憡鍑归崹鐗堟叏閳哄懎绫嶉柤鎼佹涧閳锋棃鎮?婵帗绋掗…鍫ヮ敇婵犳艾绀傞柟鎯板Г閿?闂佹寧绋掑銊╁极閵堝绠ｉ梺鍨儑濮婇箖鏌涘鐓庣仯闁轰降鍊栧濠氬炊閵夈垹浜惧ù锝夘棑缁?WARP 闁哄鏅滈…鍫㈡崲濞戙垺鍎撻柍?-> 闂佸搫鍋嗛崢鑲╂閹达箑违?
// 闂佸憡鐟禍婊冿耿椤忓牆瑙︽い鏍ㄨ壘琚熼梺鍝勫暢濞夋稒绂掑☉娆戔枖闁逞屽墮閳诲酣鍩ラ崨顖滎槷婵炴垶鎸哥粔鐢稿棘娴ｈ浜ゆ繛鎴烆焽閺嗕即鏌￠崘顓熺【婵炲懏甯￠弫宥呯暆閸曨亞绱氶梺绋跨箰缁夋挳骞忔导瀛樺仺闁靛绠戦悡鏇㈡煙闂堟侗鍎忓┑顔规櫆濞煎鎮欓弶鎴濐槻闂佺懓鐏氶幐濠氭倶閿曞倸违?
void EnforceWarpDefaultOff() {
    if (WarpIntentEnabled()) return;
    if (WarpState() != 1) return;
    Log(L"[warp] default off: disconnecting");
    std::wstring cmd = L"cmd.exe /c \"\"" + g_cliPath + L"\" warp off\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = cmd;
    if (CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, g_root.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// ---------------------------------------------------------------- WARP 闂佺顑冮崕閬嶅箖瀹ュ宸濆┑鐘叉媼閸?
// 婵炴垶鎸堕崐妤咃綖?WARP 婵炴垶鎸哥粔鐑藉礂濮椻偓閹?闂?
//   1) 闂佺粯顭堥崺鏍焵椤戞寧顦风紒?connected 闂佸憡鐟﹁摫闁?disconnected      -> 缂備焦鏌ㄩ鍛村春閵忋倖鐒绘慨妯夸含閸欌偓
//   2) 闂佺粯顭堥崺鏍焵椤戞寧绁版い?connected 婵炶揪绲藉Λ妤呭吹椤撱垹鐭楅柨婵嗘鐠у瓨绻涢弶鎴剮缂佹顦甸弻鍛潩閸撗咁槱婵炴垶鎸堕崹鍦偓鍨戦〃?缂備焦妫忛崹鎶藉磻閿濆洦鍋栨い鎰堕檮椤庢瑩鏌?
//      -> 闁哄鏅濋崑鐔兼偨缂佹ê绶為弶鍫亯琚?2 濠电偛妫岄崜婵囨櫠閻樼粯鐒绘慨妯夸含閸欌偓闂佹寧绋戞總鏃傜箔?10 闂佸憡甯掑Λ婵嬪箰閹捐绀冮柛娑卞枟娴犳ê顭块懜鐢电叝缂佹梹娼欓埢搴ㄥ煡閸涱垳顦梻渚囧墮閻忔繈宕㈤妶澶婄鐎瑰嫭澹嗗?
// 闂佹椿娼块崝宥夊春濞戞瑧鈻旈悹鍥ㄥ絻琚熼梺鍝勫亞閸樿偐妲愰幋锕€绫?g_warpManualStop=true闂佹寧绋戞總鏃傜箔婢舵劕绠ラ柟鐐綑椤ュ秹鏌?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂備緡鍋呮穱铏规崲?WARP 闂佸憡鐟﹂悧鏃傜博鐎涙鈻?204 缂備焦妫忛崹鎶藉磻閿濆违闁稿本绻勭粻鏌ユ煕?true 闁荤偞绋忛崝搴ㄥΦ濮樿泛绀勯柛婵嗗缂嶆捇鏌涘▎妯虹仯闁轰降鍊濇俊?
bool WarpProbeOk() {
    std::wstring cmd =
        L"curl.exe -s --ssl-no-revoke -m 8 -o NUL -w %{http_code} "
        L"https://www.gstatic.com/generate_204";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = cmd;
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) { CloseHandle(readEnd); return false; }
    std::string raw;
    char buf[256];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) raw.append(buf, got);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, 12000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return raw.find("204") != std::string::npos;
}

void RunWarpHealthCheck() {
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        int st = WarpState();
        if (st == 0) {                     // 濠电偛鐪查妶搴㈩棅闁诲骸绠嶉崹娲春濞戞氨鍗氭い鏍ュ€楃粈澶娒归悩顐壕婵炴垶鏌ㄩ悧鎾诲礂濡崵鈻旂€广儱鎳嶇划?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂佸搫鍋嗛崢鑲╂?
            if (g_warpLastState == 1 && !g_warpManualStop && WarpIntentEnabled()) {
                Log(L"[warp] alert: disconnected");
                PostMessageW(g_hwnd, WM_WARP_ALERT, 1, 0);
            }
            g_warpLastState = st;
            g_warpFailStreak = 0;
            return 0;
        }
        if (g_warpLastState == 2) {
            Log(L"[warp] recovered");
        }
        g_warpLastState = 1;
        if (WarpProbeOk()) {
            g_warpFailStreak = 0;
            return 0;
        }
        g_warpFailStreak++;
        ULONGLONG now = GetTickCount64();
        if (g_warpFailStreak >= 2 && (now - g_warpLastNotify) > 10ULL * 60 * 1000) {
            g_warpLastNotify = now;
            Log(L"[warp] alert: connected but probe failed");
            PostMessageW(g_hwnd, WM_WARP_ALERT, 2, 0);
        }
        return 0;
    }, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------- status rows
constexpr wchar_t kAppVersion[] = L"3.14.2";

// NRPT rule installed? Same key the installer writes, so status cannot lie.
bool NrptInstalled() {
    HKEY key = nullptr;
    std::wstring path =
        L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DnsPolicyConfig\\"
        L"{6E1F2C74-9A3B-4D2E-8F51-0C7A5B3D9E48}";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

// Trusted for this user OR machine-wide (global mode puts it in the machine store).
bool CaTrustedAnywhere() {
    if (CaTrusted()) return true;
    std::wstring cmd = L"certutil.exe -store Root";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = cmd;
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) { CloseHandle(readEnd); return false; }
    std::string raw;
    char buf[2048];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) raw.append(buf, got);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::wstring wide = Utf8ToWide(raw);
    for (auto& ch : wide) ch = (wchar_t)towlower(ch);
    return wide.find(L"kynarain") != std::wstring::npos || wide.find(L"sniroute") != std::wstring::npos;
}

// "DNS: (filled circle) Running" - the shape SNIBypassGUI uses, for the same reason: a
// disabled menu row is the only place the tray can put a status line.
std::wstring StatusLabelEx(const wchar_t* labelKey, bool up, const wchar_t* upKey,
                           const wchar_t* downKey, const wchar_t* upFallback,
                           const wchar_t* downFallback) {
    std::wstring out = g_str.get(labelKey, labelKey);
    out += L": ";
    out += up ? L"\u25CF " : L"\u25CB ";          // filled / hollow circle
    out += up ? g_str.get(upKey, upFallback) : g_str.get(downKey, downFallback);
    return out;
}

// Tooltip: name + version + the two service states. Version included because a
// screenshot of the tray is the usual way a problem gets reported back.
std::wstring BuildTooltip(bool doh, bool nginx) {
    std::wstring tip = g_str.get(L"tip", L"sniroute");
    tip += L" ";
    tip += kAppVersion;
    tip += L"  DoH:";
    tip += doh ? g_str.get(L"status_on_short", L"up") : g_str.get(L"status_off_short", L"down");
    tip += L" nginx:";
    tip += nginx ? g_str.get(L"status_on_short", L"up") : g_str.get(L"status_off_short", L"down");
    return tip;
}

std::wstring StatusLabel(const wchar_t* labelKey, bool up) {
    return StatusLabelEx(labelKey, up, L"status_running", L"status_stopped",
                         L"running", L"stopped");
}

std::wstring TrustLabel(const wchar_t* labelKey, bool trusted) {
    return StatusLabelEx(labelKey, trusted, L"status_trusted", L"status_untrusted",
                         L"trusted", L"not trusted");
}
// Supported sites, flattened at build time from data\sites.json into "label|url"
// lines. The tray has no JSON parser and a hand-kept duplicate would drift.
struct SiteEntry {
    std::wstring label;
    std::wstring url;
};

std::vector<SiteEntry> LoadSites() {
    std::vector<SiteEntry> out;
    std::wstring path = g_root + L"\\data\\sites-tray.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return out;
    std::string raw;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);

    std::wstring all = Utf8ToWide(raw);
    size_t start = 0;
    while (start < all.size()) {
        size_t nl = all.find(L'\n', start);
        std::wstring line = all.substr(start, nl == std::wstring::npos ? std::wstring::npos
                                                                      : nl - start);
        start = (nl == std::wstring::npos) ? all.size() : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        size_t bar = line.find(L'|');
        if (bar == std::wstring::npos) continue;
        SiteEntry e;
        e.label = line.substr(0, bar);
        e.url = line.substr(bar + 1);
        if (!e.label.empty() && !e.url.empty()) out.push_back(e);
    }
    // 闂佸吋瀵х划灞界暦閻旂厧绠板鑸靛姦濞存洟鎮楀☉娆樻畷闁伙附绻堥獮鎺楀箳閹寸姷顣查梺鎸庣⊕绾板秹寮抽悢鐓庣妞ゆ棁濮ら悗顔济归悩鐑樼【鐟滅増鐓￠幊妤呮嚍閵夛箓妾烽梺鐓庮殠娴滄繂螞椤愶附鍋嬮柛顐ゅ枑閹烽亶鏌ㄥ☉妯煎婵犫偓椤撱垹绠抽柟鐑樺灩绾板秹鏌ㄥ☉姗嗘缂佽鲸绻冨璇测枎閹捐櫕顏為梺鍛婂姇缁夊磭鎹㈠杈ㄥ珰濞达綁顥撻杈ㄧ箾閸犫剝澹嬮崑?
    std::sort(out.begin(), out.end(), [](const SiteEntry& a, const SiteEntry& b) {
        std::wstring x = a.label, y = b.label;
        for (auto& ch : x) ch = (wchar_t)towlower(ch);
        for (auto& ch : y) ch = (wchar_t)towlower(ch);
        return x < y;
    });
    return out;
}
// ---------------------------------------------------------------- autostart
const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* RUN_VALUE = L"sniroute";

bool AutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    bool found = RegQueryValueExW(key, RUN_VALUE, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

void SetAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t exe[MAX_PATH * 4];
        GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        std::wstring value = std::wstring(L"\"") + exe + L"\"";
        RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        Balloon(g_str.get(L"tip", L"sniroute"),
                g_str.get(L"notify_autostart_on", L"Autostart on"), NIIF_INFO);
    } else {
        RegDeleteValueW(key, RUN_VALUE);
        Balloon(g_str.get(L"tip", L"sniroute"),
                g_str.get(L"notify_autostart_off", L"Autostart off"), NIIF_INFO);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------- menu
void ShowMenu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();

    // Read-only status block, disabled rows: DNS redirect (NRPT), DNS service,
    // nginx, certificate - then the version. Same layout idea as SNIBypassGUI.
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, StatusLabel(L"status_dns_redirect", NrptInstalled()).c_str());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, StatusLabel(L"status_dns", PortListening(5444)).c_str());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, StatusLabel(L"status_nginx", NginxRunning()).c_str());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, TrustLabel(L"status_ca", CaTrustedAnywhere()).c_str());
    // "Supported sites" submenu: one clickable entry per service, opening the
    // site itself. A submenu beats a text row because the interesting action is
    // "go there", not "read a hostname list".
    {
        g_siteUrls.clear();
        std::vector<SiteEntry> sites = LoadSites();
        HMENU sitesMenu = CreatePopupMenu();
        for (size_t i = 0; i < sites.size(); ++i) {
            AppendMenuW(sitesMenu, MF_STRING, IDM_SITE_FIRST + (UINT)i, sites[i].label.c_str());
            g_siteUrls.push_back(sites[i].url);
        }
        UINT flags = MF_POPUP;
        if (sites.empty()) flags |= MF_GRAYED;
        AppendMenuW(menu, flags, reinterpret_cast<UINT_PTR>(sitesMenu),
                    g_str.get(L"menu_supported", L"Supported sites"));
    }
    {
        std::wstring ver = g_str.get(L"status_version", L"version");
        ver += L" ";
        ver += kAppVersion;
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, ver.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_UP, g_str.get(L"menu_up", L"Start all"));
    AppendMenuW(menu, MF_STRING, IDM_DOWN, g_str.get(L"menu_down", L"Stop all"));
    AppendMenuW(menu, MF_STRING, IDM_STATUS, g_str.get(L"menu_status", L"Status"));
    AppendMenuW(menu, MF_STRING, IDM_UPDATE, g_str.get(L"menu_update", L"Check for updates"));
    AppendMenuW(menu, MF_STRING, IDM_NAT, g_str.get(L"menu_nat", L"NAT type check"));
    AppendMenuW(menu, MF_STRING, IDM_GHA_FETCH,
                g_str.get(L"menu_gha", L"Download via GitHub Actions..."));
    {
        // WARP 闁诲孩绋掗崝妯裤亹瀹ュ纭€闁哄浂浜炵粣妤冪磽?Sniroute 闂佸憡姊绘慨宕囩博閹绢喖绾ч柍銉﹀墯閺嗘澘顭块懜鐢靛ⅹ闁搞値鍙冨畷锝夋晲韫囨洜顦〨oogle/YouTube/Play 缂傚倸鍟崹鍧楀Υ婢舵劖鍋嬮柛顐墰缁€鍡涙煏?
        int ws = WarpState();
        HMENU warpMenu = CreatePopupMenu();
        if (ws == 0) {
            AppendMenuW(warpMenu, MF_STRING | MF_GRAYED, 0,
                        g_str.get(L"warp_missing", L"WARP not installed"));
            AppendMenuW(warpMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(warpMenu, MF_STRING, IDM_WARP_INSTALL,
                        g_str.get(L"warp_install", L"Install WARP client"));
        } else {
            std::wstring stateLine = g_str.get(L"warp_state", L"WARP: ");
            stateLine += (ws == 1) ? g_str.get(L"warp_on", L"connected")
                                   : g_str.get(L"warp_off_state", L"disconnected");
            if (!WarpIntentEnabled() && ws == 2) {
                stateLine += g_str.get(L"warp_default_off", L" (off by default)");
            }
            AppendMenuW(warpMenu, MF_STRING | MF_GRAYED, 0, stateLine.c_str());
            AppendMenuW(warpMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(warpMenu, MF_STRING, IDM_WARP_ON, g_str.get(L"warp_connect", L"Connect WARP"));
            AppendMenuW(warpMenu, MF_STRING, IDM_WARP_OFF, g_str.get(L"warp_disconnect", L"Disconnect WARP"));
            AppendMenuW(warpMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(warpMenu, MF_STRING, IDM_WARP_EXPORT,
                        g_str.get(L"warp_export", L"Export Clash YAML (WARP)"));
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(warpMenu),
                    g_str.get(L"menu_warp", L"WARP (exit)"));
    }
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, g_str.get(L"menu_refresh", L"Refresh upstream IPs"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_BROWSER, g_str.get(L"menu_browser", L"Open browser"));
    AppendMenuW(menu, MF_STRING, IDM_INSTALL, g_str.get(L"menu_install", L"Install certificate"));
    AppendMenuW(menu, MF_STRING, IDM_POLICY, g_str.get(L"menu_policy", L"Configure browser (DoH)"));
    AppendMenuW(menu, MF_STRING, IDM_GLOBAL, g_str.get(L"menu_global", L"Global DNS mode (admin)"));
    AppendMenuW(menu, MF_STRING, IDM_REVERT, g_str.get(L"menu_revert", L"Revert"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_LOG, g_str.get(L"menu_log", L"View log"));
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART,
                g_str.get(L"menu_autostart", L"Start with Windows"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, g_str.get(L"menu_exit", L"Exit"));

    // 闂佸搫鐗冮崑鎾诲箹鐎涙ɑ绀嬮柛搴㈡尦閹啴宕熼鐑嗘毉闂佸搫顦崯鎶姐€侀幋锔芥櫖婵ê褰夌槐妤呮煠绾懎绱︾紒妤€顦靛畷锝夘敍濠婂啩鍖栭梺鎸庣☉閼活垵銇愯閹粙濡搁敂浠嬫闂佸搫绉村ú锕€危閹达箑绀勯柛婵嗗濡查亶鏌涘鍐伇濠⒀勵殜瀵敻顢栭柨顖氫壕?
    // 濠电姍鍕闁绘牗绮嶇粚鍗炩攽閸パ呮Х缂?ASCII闂佹寧绋戦張顒佹櫠瀹ュ棛顩?U+00A9(婵? 闂?U+00B7(閻? 闂佹椿娼块崝蹇撁归崱妯尖枙濠㈣泛锕ら弲鎼佹煏?
    {
        // 闂佺粯顨呴悧濠傤焽?+ 闁荤姳鑳堕…鍫ｃ亹閺屻儱绀嗛柛鈩冾殘绾捐鈽夐幘鍗炩偓娑€侀幋锔芥櫖婵﹩鍋嗛閬嶆偠濞戞瀚伴柕鍥э躬濮婅寰勯崫銉х崶闁荤偞鍑归崑澶庛亹瀹ュ纭€闁哄洨鍠庨悞濠氭煛閸屻倕寮跨紒杈ㄧ箞閹虫挾浠﹂懖鈺冩噸"All rights reserved"
        // 闂佸憡顨嗗ú婊呪偓姘煎弮楠炲骞囬锝嗘杸婵炴垶鎸哥粔鎾敊閹版澘鍙婇柟鎹愵嚃濞煎爼鎮归崶璺虹仴妞ゆ洑鍗抽獮?闁哄鏅滈悷銈夋煂濠婂嫨浜滈柛锔诲幗缁愭鈽夐幘宕囆ｆ俊鑼焾椤曪綁鍩€椤掆偓閳瑰啴骞囬鐣屾殸"闂?
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    L"Copyright \u00A9 2026 Kynarain. All rights reserved.");
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    L"Licensed under the GNU General Public License v3.0.");
    }

    // Required so the menu closes when clicking elsewhere.
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd,
                   nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------- window proc
void RunUpdateCheck();   // defined later in this namespace

std::wstring PromptForUrl();          // defined later in this namespace
void RunGhaFetch(const std::wstring& url);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        // Explorer restarted: re-add the icon or it silently disappears.
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = ID_TRAY;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY_CB;
        nid.hIcon = g_iconOk;
        wcsncpy_s(nid.szTip, g_str.get(L"tip", L"sniroute"), _TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &nid);
        UpdateStatus(false);
        return 0;
    }

    switch (msg) {
    case WM_TRAY_CB:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu();
            return 0;
        case WM_LBUTTONDBLCLK:
            UpdateStatus(true);
            return 0;
        default:
            break;
        }
        break;

    case WM_COMMAND:
        // Submenu entries open the site; ids are contiguous from IDM_SITE_FIRST.
        if (LOWORD(wp) >= IDM_SITE_FIRST &&
            LOWORD(wp) < IDM_SITE_FIRST + g_siteUrls.size()) {
            const std::wstring url = g_siteUrls[LOWORD(wp) - IDM_SITE_FIRST];
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Log(L"[site] open " + url);
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDM_UP:
            RunAction(kUp);
            return 0;
        case IDM_DOWN:
            RunAction(kDown);
            return 0;
        case IDM_STATUS:
            UpdateStatus(true);
            return 0;
        case IDM_BROWSER:
            RunAction(kBrowser);
            return 0;
        case IDM_INSTALL:
            RunAction(kInstall);
            return 0;
        case IDM_POLICY:
            RunAction(kPolicy);
            return 0;
        case IDM_REVERT:
            RunAction(kRevert);
            return 0;
        case IDM_LOG: {
            std::wstring cmd = L"notepad.exe \"" + g_logPath + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::wstring w = cmd;
            if (CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                               &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            return 0;
        }
        case IDM_AUTOSTART: {
            bool enable = !AutostartEnabled();
            SetAutostart(enable);
            std::wstring marker = g_root + L"\\data\\.autostart-off";
            if (enable) {
                DeleteFileW(marker.c_str());
            } else {
                HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
            return 0;
        }
        case IDM_REFRESH:
            RunAction({L"refresh", nullptr, nullptr,
                       L"refresh_ok", L"refresh_fail",
                       L"Upstream addresses refreshed", L"Refresh failed (%d)",
                       nullptr});
            return 0;
        case IDM_GHA_FETCH: {
            std::wstring url = PromptForUrl();
            if (!url.empty()) RunGhaFetch(url);
            return 0;
        }
        case IDM_UPDATE:
            RunUpdateCheck();
            return 0;
        case IDM_NAT:
            RunNatProbe();
            return 0;
        case IDM_WARP_EXPORT: {
            // 闁诲海鏁搁崢褔宕甸鐔衡枖鐎广儱瀚拹鈺呮偣瑜庨悧鏇㈩敇瑜版帒绠ｅ瀣瘨娴煎倿鏌ㄥ☉娆樺剻LI 婵炴潙鍚嬫穱娲吹濠婂喚鍟呴弶鍫涘妽閺嗗牓鏌?WARP 闁荤姵鍔х粻鎴ｃ亹閸ф宓侀悹鍝勬惈閺?YAML闂?
            std::wstring cmd = L"cmd.exe /c \"\"" + g_cliPath + L"\" warp export\"";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            std::wstring w = cmd;
            if (CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, g_root.c_str(), &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                Balloon(g_str.get(L"tip", L"sniroute"),
                        g_str.get(L"warp_export_msg",
                                  L"Exporting WARP config to: (install dir)\dist\warp-clash.yaml"),
                        NIIF_INFO);
            }
            return 0;
        }
        case IDM_WARP_INSTALL:
            RunWarpInstall();
            return 0;
        case IDM_WARP_ON:
            SetWarpIntent(true);
            g_warpManualStop = false;
            g_warpFailStreak = 0;
            RunWarpAction(L"on");
            Balloon(g_str.get(L"tip", L"sniroute"),
                    g_str.get(L"warp_connecting", L"Connecting WARP..."),
                    NIIF_INFO);
            return 0;
        case IDM_WARP_OFF:
            SetWarpIntent(false);
            g_warpManualStop = true;
            g_warpLastState = 2;
            RunWarpAction(L"off");
            Balloon(g_str.get(L"tip", L"sniroute"),
                    g_str.get(L"warp_disconnected_msg", L"WARP disconnected"),
                    NIIF_INFO);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_WARP_INSTALL_DONE: {
        // winget 闁?閻庣懓鎲¤ぐ鍐偩閵娧勫晳闁告侗鍓涢悷顒勬煛閸愵亜校婵炴彃娼″?闁哄鏅滈弻銊ッ洪弽顓燁棃?0闂?1978335189闂佹寧绋戦¨鈧紒杈ㄧ箞閺屽洭鏁愰崟顓犳喒闂佸搫瀚烽崹浼村Φ閹寸姵瀚婚柕澶涚岛閸?
        // 婵炲濮伴崕閬嶎敇瑜版帒绠ｅ瀣瘨娴煎倿鏌￠崟闈涚仩闁诡垯绶氶幆鍥偄閻戞鏆犻梺鍛婂灱濞咃綁鐛崶銊р枖闁告繂瀚▍娆撴煥濞戞鐒烽柍褜鍓欐總鏃傜箔婢舵劕鍙婃い鏍ㄨ壘濞懷囨煟椤忓嫷鍎旈柍褜鍏橀崑鎾绘煕閹达絾顏犻柣鏍ㄧ矒婵?
        if (wp == 0 && WarpState() != 0) wp = 0;
        if (wp == 0) {
            Balloon(g_str.get(L"tip", L"sniroute"),
                    g_str.get(L"warp_installed",
                              L"WARP installed. Use WARP (exit) -> Connect WARP."),
                    NIIF_INFO);
        } else {
            Balloon(g_str.get(L"tip", L"sniroute"),
                    g_str.get(L"warp_install_fail",
                              L"WARP install failed. Run manually: winget install Cloudflare.Warp"),
                    NIIF_WARNING);
        }
        return 0;
    }

    case WM_GHA_DONE: {
        std::wstring* p = reinterpret_cast<std::wstring*>(lp);
        if (p) {
            Balloon(g_str.get(L"tip", L"sniroute"), *p, NIIF_INFO);
            delete p;
        }
        return 0;
    }

    case WM_UPDATE_DONE: {
        std::wstring* p = reinterpret_cast<std::wstring*>(lp);
        if (p) {
            Balloon(g_str.get(L"tip", L"sniroute"), *p, NIIF_INFO);
            delete p;
        }
        return 0;
    }

    case WM_WARP_ALERT: {
        if (wp == 1) {
            Balloon(g_str.get(L"warp_lost_title", L"WARP exit lost"),
                    g_str.get(L"warp_lost_msg",
                              L"WARP is no longer connected; switching back to the local DNS plan."),
                    NIIF_WARNING);
            // 闂佸搫鍋嗛崢鑲╂閹寸偟鈻旂€广儱鎳庡☉褔鏌?闂佸憡鍨甸幖顐ャ亹濞戞ǚ鏌﹂柍銉ㄥ皺閸?闂佹寧绋掑畝鎼佸Υ鎼淬劌绠ラ悗锝庡厴閳ь剚蓱缁屽崬鈹戦崶銊﹀闂?DoH/nginx 闁哄鏅滈敋婵犫偓椤忓牊鏅悘鐐舵閻掑ジ鎮规担鍙夋珖闁轰降鍊濋獮瀣憥閸屾瑧闉嶆繛鎴炴尵閸庛倝寮婚崶鈺冪＞闁瑰瓨鐟㈤崑?
            RestoreLocalDns();
        } else if (wp == 3) {
            Balloon(g_str.get(L"tip", L"sniroute"),
                    g_str.get(L"dns_restored",
                              L"Switched back to the local DNS plan (DoH + nginx). WARP is off."),
                    NIIF_INFO);
        } else if (wp == 4) {
            Balloon(g_str.get(L"tip", L"sniroute"),
                    g_str.get(L"dns_restore_fail",
                              L"WARP is off and the local DNS plan is not running. "
                              L"Run sniroute.cmd up."),
                    NIIF_WARNING);
        } else {
            Balloon(g_str.get(L"warp_dead_title", L"WARP exit unreachable"),
                    g_str.get(L"warp_dead_msg",
                              L"WARP reports connected but the exit probe fails. "
                              L"Try reconnect or another endpoint."),
                    NIIF_WARNING);
        }
        return 0;
    }

    case WM_NAT_DONE: {
        // worker thread hands over a heap string; show it then free it
        std::wstring* text = (std::wstring*)lp;
        if (text) {
            MessageBoxW(g_hwnd, text->c_str(), g_str.get(L"tip", L"sniroute"),
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            delete text;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_REFRESH) {
            RunAction({L"refresh", nullptr, nullptr,
                       L"refresh_ok", L"refresh_fail",
                       L"Upstream addresses refreshed", L"Refresh failed (%d)",
                       nullptr});
            return 0;
        }
        if (wp == TIMER_STATUS) {
            UpdateStatus(false);
            return 0;
        }
        if (wp == TIMER_STARTUP_UP) {
            KillTimer(g_hwnd, TIMER_STARTUP_UP);
            std::wstring cmd = L"cmd.exe /c \"\"" + g_cliPath + L"\" up\"";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            std::wstring w = cmd;
            if (CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, g_root.c_str(), &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                Log(L"[start] up spawned (delayed)");
            }
            return 0;
        }
        if (wp == TIMER_WARP) {
            RunWarpHealthCheck();
            return 0;
        }
        break;

    case WM_DESTROY: {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = ID_TRAY;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (g_iconOk) DestroyIcon(g_iconOk);
        if (g_iconBad) DestroyIcon(g_iconBad);
        PostQuitMessage(0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


// 濠碘槅鍋€閸嬫捇鏌＄仦璇插姕婵炴彃娼″顒傛暜椤斿墽鐛ラ柣?sniroute.cmd update check闂佹寧绋戦張顒佷繆閸涘﹥缍囬柟鎯у暱濮ｅ鎮归幇顓熺稇闁糕晛鑻～婵嬪籍閳ь剟宕峰ú顏呯厒閻忕偟鍎甸崑?
// 闂傚倸娲犻崑鎾绘偡閺囨碍绁版い鏃€鐗犲?GitHub API闂佹寧绋戦悧蹇涳綖閸℃稑瀚夊璺侯槺濞夈垽骞栫€涙ɑ宕勬い鈺嬪閹峰濡堕崼銏☆潔闂佹寧绋戦¨鈧紒杈ㄧ箞楠炲秹鍩€椤掍胶顩烽柕澶涢檮閺夊綊鏌涘顒勵€楃憸鏉款嚟閻ヮ亞鎹勯妸銏＄厾闂佹寧绋戞總鏃傜箔婢舵劖鍤勯柦妯侯槸楠炪垹霉閿濆懐绠插褜鍙冮幆鍕熺化鏇炰壕?
void RunUpdateCheck() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"update_checking", L"Checking for updates..."), NIIF_INFO);
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        std::wstring cmdline = L"cmd.exe /c \"\"" + g_cliPath + L"\" update check 2>&1\"";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            PostMessageW(g_hwnd, WM_UPDATE_DONE, 0,
                         (LPARAM)new std::wstring(L"Update check failed"));
            return 1;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;
        PROCESS_INFORMATION pi{};
        std::wstring wcmd = cmdline;
        BOOL started = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi);
        CloseHandle(writeEnd);
        if (!started) {
            CloseHandle(readEnd);
            PostMessageW(g_hwnd, WM_UPDATE_DONE, 0,
                         (LPARAM)new std::wstring(L"Update check failed"));
            return 1;
        }
        std::string raw;
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) raw.append(buf, got);
        CloseHandle(readEnd);
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        std::wstring text;
        if (!raw.empty()) {
            int need = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
            if (need > 0) {
                text.resize((size_t)need);
                MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), text.data(), need);
            }
        }
        // 濠殿喗蓱濮婂綊宕峰ú顏勭哗闁告瑣鍎抽悷婵嗏槈閹炬剚鍎忔い锕佹硶閹澘鐣濈€ｎ剛鐛ラ梺鍛婄懐娴滄瑧鎹㈠鑸靛仼婵炲棗绻愰·鍛槈閹惧崬鈧盯銆侀幋锕€瀚夊璺侯儐濞呭繘鏌涢幇顒佸櫣妞ゆ梹鍔欐俊?
        std::wstring brief;
        int lines = 0;
        size_t pos = 0;
        while (pos < text.size() && lines < 2) {
            size_t nl = text.find(L'\n', pos);
            std::wstring one = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
            while (!one.empty() && (one.back() == L'\r' || one.back() == L' ')) one.pop_back();
            if (!one.empty()) {
                if (!brief.empty()) brief += L"\n";
                brief += one;
                ++lines;
            }
            if (nl == std::wstring::npos) break;
            pos = nl + 1;
        }
        if (brief.empty()) brief = L"Update check failed";
        Log(L"[update] " + brief);
        PostMessageW(g_hwnd, WM_UPDATE_DONE, 0, (LPARAM)new std::wstring(brief));
        return 0;
    }, nullptr, 0, nullptr);
}


// ---------------------------------------------------------------- GHA 涓嬭浇瀵硅瘽妗?
// 鏋佺畝杈撳叆妗嗭細缂栬緫妗?+ 纭畾/鍙栨秷锛岀函浠ｇ爜鍒涘缓锛堜笉闇€瑕?.rc 璧勬簮锛夈€?
// Enter = 纭畾锛孍sc = 鍙栨秷銆?
static std::wstring g_promptValue;
static bool g_promptDone = false;

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HWND edit = nullptr;
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"URL (https://...)",
                        WS_CHILD | WS_VISIBLE, 14, 10, 512, 20, hwnd, nullptr, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        14, 34, 512, 26, hwnd, (HMENU)1001, nullptr, nullptr);
        edit = GetDlgItem(hwnd, 1001);
        SetFocus(edit);
        CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        350, 70, 80, 28, hwnd, (HMENU)IDOK, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                        440, 70, 86, 28, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[2048]{};
            GetWindowTextW(edit, buf, 2048);
            g_promptValue = buf;
            g_promptDone = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            g_promptDone = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { PostMessageW(hwnd, WM_COMMAND, IDOK, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0); return 0; }
        break;
    case WM_CLOSE:
        g_promptDone = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring PromptForUrl() {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"SniroutePrompt";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    g_promptValue.clear();
    g_promptDone = false;
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"SniroutePrompt",
                             L"Download via GitHub Actions",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 560, 140,
                             nullptr, nullptr, inst, nullptr);
    if (!w) return L"";
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    MSG msg;
    while (!g_promptDone && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return g_promptValue;
}

// 鍚庡彴璺?gha fetch锛屾妸鏈€鍚庝竴琛岃緭鍑鸿创鍒版皵娉°€?
void RunGhaFetch(const std::wstring& url) {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"gha_started", L"Downloading via GitHub Actions..."), NIIF_INFO);
    CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        std::wstring* p = reinterpret_cast<std::wstring*>(param);
        std::wstring target = *p;
        delete p;

        wchar_t profile[MAX_PATH]{};
        SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile);
        std::wstring outDir = std::wstring(profile) + L"\\Downloads";
        std::wstring cmdline = L"cmd.exe /c \"\"" + g_cliPath + L"\" gha fetch \"" +
                               target + L"\" -o \"" + outDir + L"\" 2>&1\"";

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            PostMessageW(g_hwnd, WM_GHA_DONE, 0, (LPARAM)new std::wstring(L"pipe failed"));
            return 1;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;
        PROCESS_INFORMATION pi{};
        std::wstring wcmd = cmdline;
        BOOL started = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi);
        CloseHandle(writeEnd);
        if (!started) {
            CloseHandle(readEnd);
            PostMessageW(g_hwnd, WM_GHA_DONE, 0, (LPARAM)new std::wstring(L"CreateProcess failed"));
            return 1;
        }
        std::string raw;
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) raw.append(buf, got);
        CloseHandle(readEnd);
        WaitForSingleObject(pi.hProcess, 15 * 60 * 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        std::wstring text;
        if (!raw.empty()) {
            int need = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
            if (need > 0) {
                text.resize((size_t)need);
                MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), text.data(), need);
            }
        }
        std::wstring brief;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find(L'\n', pos);
            std::wstring one = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
            while (!one.empty() && (one.back() == L'\r' || one.back() == L' ')) one.pop_back();
            if (!one.empty()) brief = one;
            if (nl == std::wstring::npos) break;
            pos = nl + 1;
        }
        if (brief.empty()) brief = L"GitHub Actions download failed";
        Log(L"[gha] " + brief);
        PostMessageW(g_hwnd, WM_GHA_DONE, 0, (LPARAM)new std::wstring(brief));
        return 0;
    }, new std::wstring(url), 0, nullptr);
}

bool CommandLineHas(const wchar_t* flag) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 0; i < argc; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    InitializeCriticalSection(&g_logCs);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_root = ExeDir();
    g_iconPath = g_root + L"\\gui\\icon.ico";
    g_stringsPath = g_root + L"\\data\\tray-strings.txt";
    g_logPath = g_root + L"\\logs\\tray.log";
    g_cliPath = g_root + L"\\sniroute.cmd";

    CreateDirectoryW((g_root + L"\\logs").c_str(), nullptr);
    LoadStrings();

    if (CommandLineHas(L"--status")) {
        // Prints the exact rows the menu will show, so the text can be checked
        // without a human reading the popup.
        std::wstring out;
        out += StatusLabel(L"status_dns_redirect", NrptInstalled()) + L"\n";
        out += StatusLabel(L"status_dns", PortListening(5444)) + L"\n";
        out += StatusLabel(L"status_nginx", NginxRunning()) + L"\n";
        out += TrustLabel(L"status_ca", CaTrustedAnywhere()) + L"\n";
        {
            std::vector<SiteEntry> sites = LoadSites();
            for (const auto& s : sites) {
                out += g_str.get(L"status_sites", L"sites");
                out += L": " + s.label + L"  (" + s.url + L")\n";
            }
        }
        out += g_str.get(L"status_version", L"version");
        out += L" " + std::wstring(kAppVersion) + L"\n";
        out += L"tooltip: " + BuildTooltip(PortListening(5444), NginxRunning()) + L"\n";
        Log(out);
        return 0;
    }

    if (CommandLineHas(L"--smoke")) {
        Log(L"[smoke] strings=" + std::to_wstring(g_str.kv.size()) + L" icon=" +
            (FileExists(g_iconPath) ? L"yes" : L"no"));
        return 0;
    }

    // One instance per install path. A single global name looked safer, but a
    // process left behind by an uninstall-while-running keeps owning it forever
    // (its files are gone, so it is also unkillable), and every later launch then
    // exits silently. Scoping the name to the install folder makes a new install
    // immune to such a leftover, at the cost of two installs being able to run
    // side by side - which is visible and harmless.
    unsigned int hash = 2166136261u;
    for (wchar_t ch : g_root) {
        hash ^= (unsigned int)ch;
        hash *= 16777619u;
    }
    wchar_t mutexName[64];
    swprintf_s(mutexName, L"Local\\sniroute-tray-%08x", hash);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"[start] another instance already running for this folder");
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SnirouteTrayWindow";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"sniroute", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                             inst, nullptr);
    if (!g_hwnd) {
        Log(L"[fatal] cannot create message window");
        return 1;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_iconOk = MakeStatusIcon(true);
    g_iconBad = MakeStatusIcon(false);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CB;
    nid.hIcon = g_iconOk;
    wcsncpy_s(nid.szTip, BuildTooltip(PortListening(5444), NginxRunning()).c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        Log(L"[fatal] Shell_NotifyIcon failed");
        return 1;
    }

    SetTimer(g_hwnd, TIMER_STATUS, 3000, nullptr);
    // Upstream addresses drift as the CDN reshuffles its fleet, so re-check them
    // every 45 minutes in the background instead of waiting for a user report.
    SetTimer(g_hwnd, TIMER_REFRESH, 45 * 60 * 1000, nullptr);
    // WARP 闂佺顑冮崕閬嶅箖瀹ュ憘娑㈠焵椤掑嫬钃熼柕澶樺灣缁?0 缂備礁顦扮敮濠勭博閺夋垟鏋庨柍钘夋噽缁€澶婎熆閹壆绨块悷娆欓檮缁嬪濡堕崱鈺佷还闂佸綊娼х粔褰掑焵椤掍焦鐨戦柣鎿勭節婵?
    // 闂佸憡鍑归崹鐗堟叏閳哄懎绫嶉悹鍝勬惈鐢儵鏌熺捄鐚村伐缂傚秴顑夊畷婊冾吋閸ヮ灝锕傛煙椤戞寧绁版い鏂挎处缁嬪鈧綆鍋呴梽宥夋煥濞戞瑨澹橀柟顖欑窔瀹?闂佸憡鍑归崹鐗堟叏閳哄懎绫嶉悹鍝勬惈閸ゆ帡寮堕埡鍐ㄤ沪閻㈩垱鎸虫俊瀛樼瑹閳ь剙鈻撻姀銈呰Е閹兼番鍨洪悞鐣屸偓娈垮枓閸?闁哄鏅滈悷褔锝為幒鎾村妞ゆ帊绀佹惔濠偯归崗闂翠孩妞ゎ偓绠戦…瀹犵疀閺傝法顏?
    // 闂佹寧绋戦悧蹇涱敄閸ャ劎鈻旈柍褜鍓欓埢搴ㄥ焺閸愮偓鈷曢梺鍝勮閸庡啿锕㈤崨鏉戠闁规鍠楅悾閬嶆煛閸曢潧鐏￠柡灞芥搐椤曪綁鍩€椤掑嫭鏅€光偓閸愶絽浜鹃悘鐐跺亹閻熸劕鈽夐幘顖氫壕濠电偛妫寸槐鏇熸叏閹间礁绠戝ù锝囨櫕缁犵兘鏌￠崟闈涚仭婵犫偓椤撱垺鍎楅柕澶樺灣缁€澶愭煕閹哄鈧洟鎮鹃悙顒傗枖鐎广儱鎳庡В?婵炲濮村锔炬崲濞戙垹绠抽柕澶堝劚閻撳倿鏌￠崒銈呭箹缂?闂佹寧绋戦ˇ顓㈠焵?
    // 濠殿噯绲界换鎴︻敃婵傜瑙︽い鏍ㄨ壘琚熼梻渚囧枛閳ь剛鍟庢径灞惧磯闁瑰瓨绮庨杈ㄧ箾?up闂佹寧绋戞總鏃傚緤?*閻庣偣鍊栭崕鑲╂崲?12 缂?*闂?
    // first-run-setup 闂佺厧顨庢禍婊呮崲娴ｅ湱鈻曢柣鏂垮缁愭鏌熷畡閭︽Ъ闁圭櫢缍佸闈涱吋閸涱収娼抽梺鎸庣☉閻ジ鎮靛☉銏犵閻犲洦褰冮弲娆撴偣?up 婵炴潙鍚嬮懝楣冨箯閹殿喒鍋撻悷鏉挎殲濠殿喚濞€閺佸秹宕煎┑鍫熸緰濠电偞娼欓鍛村箖濡ゅ啰鍗氭い鏍ㄧ箥濞艰泛霉濠婂嫬绗ф繛?
    // 闂佺顑嗙划宥囨暜閳ь剟鏌ㄥ☉妯肩伇缂侇喓鍔戝?5454 濠电偛澶囬埀顒€寮堕惇浠嬫煕濮樼厧鍘甸柍?444 闁哄鏅滈弻銊ッ?502闂佹寧绋戦ˇ顓㈠焵椤掆偓閸婂鎮洪幋鐘亾閻熸澘鏆熺紒顔哄姂瀵爼鎮㈤柨瀣偓鐢告煕閹邦剛啸閽樼喎鈽夐幘顖氫壕濠电偛妫岄埀顒冩珪閻ㄦ垹绱掔€ｎ亜鏆遍柣锝囧亾缁傚秹宕遍幇銊ヤ壕?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 婵帗绋掗…鍫ヮ敇婵犳艾绀傞柟鎯板Г閿涙棃鏌ㄥ☉娆愮殤闁轰降鍊濋獮瀣煥鐎ｎ剚鈷曟繛鎴炴崄椤曆勬叏閳哄懎瑙︽い鏍ㄧ矋閺嗗繘寮堕埡浣规悙婵ǜ鍔戦獮搴㈢節閸涱垳顔曢梺娲诲亐閸嬫捇鏌?WARP 闂佸搫鍋嗛崢鐣屾暜閳ь剟鏌ㄥ☉妯垮闁哥啿鍋撻柣鐘辩劍婢瑰棙鎱ㄩ幖浣哥畱濞达絿顣介崑?
    EnforceWarpDefaultOff();
    g_warpLastState = WarpState();
    SetTimer(g_hwnd, TIMER_WARP, 60 * 1000, nullptr);
    UpdateStatus(false);
    Balloon(g_str.get(L"tip", L"sniroute"), g_str.get(L"notify_first", L"Running in the tray"),
            NIIF_INFO);
    Log(L"[start] tray up");

    // First launch on this machine: trust the CA and point the browser at local
    // DoH, so nobody has to paste a URL into chrome://settings. Runs without
    // elevation; if the policy key is locked down, the balloon says so and the
    // tray menu offers the one-time elevation.
    // Always run the setup script, never a name-based guess in C++.
    //
    // The previous version asked "is a certificate whose subject contains
    // Kynarain trusted?" - which is TRUE for the CA of an older install. Every
    // install generates its own CA, so after an upgrade the trusted CA and the CA
    // that signed the served certificate differ; Chrome then rejects the DoH
    // endpoint (log: CertVerifyProcBuiltin ... failed) and every lookup dies.
    // first-run-setup.ps1 compares THUMBPRINTS and is idempotent, and it also
    // installs the browser policy and starts the services, so calling it on each
    // start is both safe and the only place that can answer correctly.
    Log(L"[setup] running first-run setup (CA thumbprint, policy, services)");
    RunAction(kFirstRun);

    // Default to "take over at logon". A marker file records an explicit opt-out,
    // so unchecking the tray item is respected across upgrades.
    {
        std::wstring marker = g_root + L"\\data\\.autostart-off";
        bool optedOut = FileExists(marker);
        if (!AutostartEnabled() && !optedOut) {
            SetAutostart(true);
            Log(L"[setup] autostart registered by default");
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
