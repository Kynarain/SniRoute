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

// WARP 闂傚倸鍊风粈渚€骞栭銈傚亾濮樺崬鍘寸€规洝顫夌€靛ジ寮堕幋鐘垫毎濠电偠鎻徊鑺ョ珶婵犲洤鐤柛娑樼摠閻撶娀鏌ｉ幘鎶筋€楁い锝呯－缁辨帡鎮╅懡銈囨毇闂佸搫鐬奸崰鏍嵁娓氣偓楠炴帡骞嬪┑鍡╂綑闂傚倷绶氶埀顒傚仜閼活垱鏅堕鈧弻娑欑節閸曨剚鐝梺鐟扮畭閸ㄤ粙鐛€ｎ喗鏅濋柍褜鍓熼幃?0=闂傚倸鍊风粈渚€骞栭锔藉亱婵犲﹤瀚々鍙夌節婵犲倻澧涢柛瀣ф櫊閺屾洘绻涜鐎氼剟寮?1=闂備浇顕у锕傦綖婢舵劖鍋ら柡鍥╁С閻掑﹥銇勮箛鎾搭棡鐎规洖寮剁换娑㈠箣濞嗗繒浠肩紓?2=闂傚倸鍊风粈渚€骞栭锔藉亱婵犲﹤瀚々鏌ユ煥濠靛棙澶勭€规洖寮剁换娑㈠箣濞嗗繒浠肩紓浣哄У婵炲﹪寮婚弴鐔风窞婵炴垶鑹鹃惌婵嬫⒑?
// 闂?warp-cli status 闂傚倸鍊风粈渚€骞夐敍鍕殰闁搞儺鍓欑壕褰掓煛閸ャ儱鐏柛瀣ф櫅铻栭柨婵嗘噹閺嗙偤鏌￠崪浣稿闁逞屽墮閸樻粓宕戦幘缁樼厱闁规澘澧庨崚鏉棵瑰鍫㈢暫闁诡喗锕㈤幃娆戞崉鏉炵増鐫忛梻浣藉吹閸犳劗鎹㈤崼銉ヨ摕闁跨喓濮寸粈瀣⒒閸喓銆掑ù鐙€鍨伴埞鎴︽偐椤愵澀澹曢梻浣告啞娓氭宕瑰畷鍥ь棜濡わ絽鍟悡娆撴倵濞戞瑯鐒界紒鐘虫尰椤ㄣ儵鎮欓鍕紙闂佸搫琚崝宀勫煡婢跺ň鏋庨柟瀵稿仦閸庢槐SQUE 闂傚倸鍊风粈渚€骞夐敓鐘偓鍐川椤栨稑搴婂┑掳鍊曢幏瀣极婵犲洦鐓曟繛鎴濆船閺嬨倝鏌嶇紒妯活棃闁诡喗顨婇弫鎰償閳ユ剚娼剧紓鍌欑椤︻垳绱炴繝鍥ц摕婵炴垶锕╁〒濠氭煟閹存梹娅呭ù婊堢畺閺屟嗙疀閿濆懍鍑介梺鍛婂嚬閸嬪懐鎹㈠☉娆愮秶闁告挆鍐ㄧ厒婵犵妲呴崑鍛存偡瑜忛崚鎺撶節閸ャ劍娅囬梺绋挎湰缁嬫捇宕㈤幒妤侇棅妞ゆ劑鍨烘径鍕煙缁嬫寧鎲搁柟宄邦儔瀵濡烽敂鎯у箻闂備焦鍎冲ù姘跺磻閸曨垱鍊跺┑鐘插绾惧吋淇婇妶鍌氫壕濡炪倧濡囬弫璇差嚕婵犳艾宸濇い鎾楀嫬鍏婇梻浣告啞閹稿棝宕担鍓?
// 闂傚倸鍊风粈渚€骞夐敓鐘冲仭妞ゆ牗绋撻々鍙夌節闂堟稒锛旈柤?status 闂?Connected 闂傚倸鍊风粈浣虹礊婵犲偆鐒界憸蹇曞垝閺冨牆绠柦妯侯槼閸╃偛顪冮妶鍡楃瑨闁稿﹤缍婂畷鎴濐潨閳ь剟寮婚弴鐔虹闁割煈鍠栨慨鍫曟⒑閸涘﹨澹橀柛濠傛健瀵鎮㈤悡搴ｎ啋濡炪倖娲栭幊蹇浰夐弽褜娓婚柕鍫濇噺缁傚鏌涢幘瀵告创妤犵偛绻掗埀顒婄秵閸犳牕娲块梻浣告啞濞诧箓宕板绔洦瀵肩€涙ǚ鎷婚梺绋挎湰绾板秹宕鑸电厱闁靛鍨抽悾杈ㄣ亜閵夛箑鍝烘慨濠冩そ瀹曘劍绻濋崟顒€娅ч梻浣规偠閸斿秵绻涙繝鍌ゅ殨妞ゆ劧绠戝洿婵犮垼娉涜癌?
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

// 闂傚倸鍊搁崐椋庢閿熺姴纾婚柛娑卞枤閳瑰秹鏌ц箛姘兼綈鐎?sniroute.cmd 闂傚倸鍊风粈浣革耿闁秵鍋￠柟鎯版楠炪垽鏌嶉崫鍕偓褰掑级?warp on/off闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟瀵稿仧闂勫嫰鏌￠崘銊モ偓鑽ょ不閺傛鐔嗛悹楦挎閻忓崬顪冮悷鏉挎毐妞ゎ叀娉曢幑鍕偖閺夋垵顫犵紓鍌欑劍閸斞呮崲濠靛钃熼柕濞垮劗濡插牊淇婇婵嗗惞濞村吋浜瀝p-cli 闂傚倷绀侀幖顐λ囬锕€鐤炬繝闈涱儏绾惧鏌ｉ幇顒備粵闁哄棙绮撻弻鐔虹磼閵忕姵鐏堥梺娲诲幗椤ㄥ棝濡甸崟顖氬唨闁靛ě鍛殼闂備礁鎲¤摫闁瑰憡濞婇獮鍐倻閽樺宓嗗┑顔斤耿绾危椤曗偓濮婃椽骞栭悙娴嬪亾閺嶎厼鍨傞柛锔诲幖閸ㄦ繈鏌￠崘銊у缂佺姵濞婇弻锝夊箛椤旇姤姣勫銈忓瘜閸嬪﹤顫?
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


// 闂?winget 闂傚倷娴囬褍霉閻戣棄绠犻柟鎯у殺閸ヮ剙绠柛?Cloudflare WARP闂傚倸鍊风欢姘焽瑜嶈灋婵°倕鎳庣壕鐟懊归悩宸剱闁瑰弶锕㈤幃妤呮晲鎼粹€冲箣闂佺顑嗛幐楣冨箟閹绢喖绀嬮柛鈩兠悘瀵糕偓娈垮枛椤兘骞冮姀顫剨濞达綀顫夐弳濠囨⒒娴ｇ瓔娼愭繝銏∩戦弲鑸垫償閿濆洨鐒兼繝銏ｆ硾鑹屾俊鎻掔墦閺岀喖骞嗚閿涘秹宕幖浣光拺闁硅偐鍋涢埀顒佺墱閸掓帒鐣濋崟鍨櫓闂婎偄娲︾粙鎴犳兜閳ь剟姊虹紒妯哄閻忓繑鐟╅崺鈧い鎺嗗亾妞ゎ厾鍏樺璇测槈濡攱鐎婚梺褰掑亰閸撴稒绂掗幘顔解拺濞村吋鐟х粊鐑芥煕閺傚潡鍙勬鐐差槸铻栭柛娑卞幖缁愭稑顪冮妶鍡樺暗闁稿鍠庤灒濞撴埃鍋撴慨?
// 濠电姷鏁搁崑鐐差焽濞嗘挸瑙﹂悗锝庡枟閸ゅ苯螖閿濆懎鏆為柛銈呮搐閳规垿鎮╅幓鎺撴闂佸磭绮Λ鍐蓟瀹ュ牜妾ㄩ梺鍛婃尰閻熲晠宕?UAC闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟瀛樼箰閼拌法绱掑Δ鍐╃彅P 闂傚倷娴囧畷鐢稿窗閹邦優娲箣濠㈡繂缍婂畷鍫曨敄閿曗偓椤︽壆鎹㈠┑瀣倞鐟滃秹鎮靛鈧幃妤呯嵁閸喖濮庨梺鐟板暱闁帮綁鏁愰悙鍝勎у璺侯儑閸樺崬鈹戦悩璇у伐闁哥喍鍗抽幃鐐哄箹娴ｅ湱鍘撻梺鑽ゅ枑婢瑰棙鏅ラ柣搴ゎ潐濞叉鎹㈤崼銉ョ伋闁挎洖鍊哥粻鎶芥煛閸愩劌浜炴俊鍙夋尦濮婄粯鎷呴悷閭﹀殝缂備礁顑嗛崹鍧楀箠閻旂⒈鏁嶉柣鎰綑娴犲ジ姊洪棃娴ゆ盯宕熼锝嗙彺闂傚倷绀侀幉鈩冪瑹濡ゅ懎鍌ㄥΔ锝呭暙濮瑰弶鎱ㄥΟ鍨厫闁绘挻娲熼弻鐔兼倻濮楀棙鐣烽梺杞扮窔娴滃爼寮婚敍鍕勃闁哄倶鍊涢崥顐︽⒑閸︻収鍔滅紒韫矙閳ワ箓濡搁埡浣侯槰闂侀潧臎鐏炴嫎褔姊婚崒娆掑厡缂侇噮鍨堕垾锕傚醇閵夈儲妲梺鍝勭▉閸樺ジ鎮為崹顐犱簻闁瑰瓨绻冮崑銉︿繆閸撗冨幋闁?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 濠电姷鏁搁崑娑㈡偤閵娾晛鍚归幖娣妼绾捐淇婇妶鍛殭闁?CLI闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟鐑橆殕閸ゅ苯螖閿濆懎鏆為柛瀣ф櫊閺岋絽螣閸濆嫭姣愬銈嗘礉妞存悂骞堥妸銉建闁糕剝顨呴～鈺呮煟閵忊晛鐏涢柛搴ｆ暬楠?winget闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈剧畱鐎氬銇勯幒鎴濐仾闁稿骸顦甸弻娑樷槈濞嗘劗绋囬柣?winget 闂傚倷娴囬褏鎹㈤幇顔藉床闁归偊鍨禍鐟邦熆鐠鸿櫣鐏辩紒鐘虫閺屾稑鈽夐崡鐐寸亶婵?Cloudflare 闂傚倷娴囬褍顫濋敃鍌︾稏濠㈣泛瀛╅幊宀勬⒒娴ｅ憡鎯堥柣顒€銈稿畷浼村箻鐠囪尙鍔﹀銈嗗笒閸婂綊宕甸埀顒€顪冮妶蹇涙濠电偛锕ㄥΛ銏＄節閻㈤潧孝婵炲眰鍊楃划鍫濈暆閸曨剛鍘卞┑鐐村灥瀹曨剚绂嶉幇鐗堢厱?
        // 濠电姷鏁搁崑鐐哄垂閸洖绠伴柛婵勫劤閻捇姊婚崼鐔峰幏闁挎繂娲ㄩ惌娆撴煕閻斿嘲鏆為悽顖滃枔缁鈽夐姀鐘殿啋闂佽鍎煎Λ鍕亹?Windows 闂傚倸鍊搁崐椋庢閿熺姴绐楁繛鎴炵懄閸欏繘鏌曢崼婵囧珔闁?winget闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟瀛樼箰閼版寧銇勯姀銏㈡C闂傚倸鍊风欢姘焽瑜嶈灋闁哄啫鐗婇崵鍕煕閹惧弶绁板┑顔藉▕閹綊骞侀幒鎴濐瀷婵炴垶鎸哥粔褰掑蓟瀹ュ牜妾ㄩ梺鍛婃尵閸犳牠鐛崘顓ф▌閻庤娲滈崢褔鍩為幋锕€閱囨繝闈涚墕楠炩偓闂傚倷娴囧畷鐢稿窗閹扮増鍋￠柕鍫濇处椤愪粙鏌曢崼婵囧妞ゎ偅娲橀幈銊ノ熼幐搴ｃ€愰弶?
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


// WARP 闂傚倸鍊风粈渚€骞栭锕€纾婚柛鈩冪☉閸屻劑鏌ゅù瀣珕妞ゎ偅娲熼弻鐔告綇閸撗呮殸闁荤喐鐟辩粻鎾荤嵁閺嶎灔搴敆閳ь剟鎮橀埡鍛畾闁绘柨鍚嬮埛鎴︽煕濠靛棗顏褔浜堕弻娑㈠箻鐎靛憡鍣ч梺?DNS 闂傚倸鍊风粈渚€骞栭锕€纾瑰ù鐘差儜缂嶆牕顭块懜闈涘闁逞屽厸缁€渚€顢橀崗鐓庣窞濠电姴瀚烽崬闈涒攽閻橆喖鐏辨繛澶嬬洴閺佸啴濡疯閻棝鏌嶈閸撶喎顫忓ú顏勪紶闁告洖鐏氳倴闂備胶顭堢€垫帡宕归崼鏇炴槬闁?
//
// 濠电姷鏁搁崑鐐哄垂閸洖绠插ù锝呭濞存牠鏌曟繛褍鍟悘濠囨⒑閸撴彃浜為柛鐘虫崌閸┿垽寮撮姀锛勫幈濡炪倖鍔楁慨鎾礉濠婂厾褰掓偐缁涚鈧潡鏌熼鎯у幋鐎殿喗娼欓～婵囶潙閺嶃剱婊堟⒒娴ｉ涓茬紒鍙夊缁绘盯鍩€椤掑嫭鐓涘ù锝呮憸瀛濆銈庡亖閸ㄤ粙鐛埀顒€鈹戞径瀣潔 闂傚倸鍊风粈渚€骞栭銈傚亾濮樺崬鍘寸€规洖缍婇弻鍡楊吋閸涱喛绶㈤梻浣瑰缁诲倿藝椤栨壕鍋撳顒夋Ч缂佺粯绋戦蹇涱敊閼姐倕鍤梻浣风串缁犳垿鎮ч幘鎰佹綎婵炲樊浜堕弫鍡椕归敐鍫綈闁稿孩鎸剧槐鎾存媴娴犲鎽电紓浣筋嚙鐎氫即宕洪埀顒併亜閹烘垵鈧綊宕甸埀顒勬⒑閸濄儱娅忛柛銊﹀▕瀵偊顢氶埀顒勫春閳ь剚銇勯幒鎴濃偓鑽ゅ婵傚憡鐓熼柟浼存涧婢ь喗銇勯弴鐔虹煁濞ｅ洤锕幃娆撳箵閹哄棗浜鹃柛褎顨呯粻鏍归悩宸剰閹喖姊洪幐搴㈢５闁稿鎹囬弻锝夊箻鐎涙顦梺閫炲苯澧柣蹇旂缁旂喓鈧綆鍠栫壕濠氭煙閹呬邯闁稿鎹囧畷褰掝敃閵忊晜顥撻梻浣侯焾椤戝棝骞愰幖浣圭畳闂備胶绮敃鈺呭窗閺嶎厼绀夋繛鍡樻尰閸?缂傚倸鍊搁崐鎼佸磹閹间礁鐤い鏍仜閸ㄥ倿鏌涢锝嗙闁藉啰鍠栭弻娑㈠Ψ閿濆懎顬嬪?闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈鹃檮閸庢銆掑锝呬壕闂佺硶鏂侀崑鎾愁渻閵堝棗鍧婇柛瀣崌閺屻倕煤鐠囪尙浠搁梺璇″枛濞尖€崇暦閻旂⒈鏁嶆繛鎴炲笚鐎垫牠姊绘担鍛婂暈濞撴碍顨婂畷婊冣槈閵忊€充画闂佽澹嗘晶妤呮偂濞戙垺鐓曟い鎰靛亜娴滄粌顭胯閸犳鎹㈠┑瀣劦?
// DoH/nginx 闂傚倸鍊风粈渚€骞夐敓鐘冲仭妞ゆ牜鍋涢崹鍌炴煟閵忋埄鐒剧紒鈧崒娑欏弿婵＄偠顕ф禍楣冩倵鐟欏嫭绀€闁绘牕銈搁獮鍐煛閸屾稓绉跺銈嗗姂閸娿倕危瑜版帗鈷掑ù锝呮啞閸熺偞銇勯妸銉﹀櫣閻撱倝鏌曟繝蹇撶槣婵炲牅绮欓弻娑㈠Ψ椤旂厧顫╅弶鈺傜箖缁绘稒娼忛崜褍鍩岄梺纭呭Г缁捇宕洪悙娴嬫婵炲棙鍨归惁鍫ユ偡濠婂啰效鐎规洦鍨舵俊鎼佸煛娴ｅ搫濮︽俊鐐€栭崹鐓庘枖閺囥垹绀夌€广儱顦伴悡鏇㈡煏婵犲繒鍒板┑锛勬櫕缁辨帡宕掑☉妯肩懖闂佽鍨卞Λ鍐╂叏閳ь剟鏌嶉妷銉э紞濠㈣泛瀚板缁樻媴閸涘﹤鏆堥梺鍛婂灥缂嶅﹤鐣疯ぐ鎺撳亱闁割偆鍠庡鍧楁⒑閸愬弶鎯堥柟鍐茬箻瀹曠敻鍩€椤掑嫭鍊甸柛蹇擃槸娴滈箖姊洪柅鐐茶嫰婢ь喗銇勯弬璺ㄧ婵炵厧绻樻俊鎼佸Ψ瑜忛悰顔尖攽閻愬瓨灏伴柤鐟板⒔閼洪亶鏌嗗鍛紱闂佽宕橀褏澹曢崗鑲╃瘈闂傚牊绋撴晶銏°亜閺冣偓閸旀瑥顫忕紒妯诲闁荤喐澹嗙粊鐑芥⒑閸忓吋銇熼柛銊ㄤ含缁骞掗幋鏃€顫嶅┑鐐叉閸ㄧ喖寮搁弽顓熲拺闂傚牊绋撴晶鏇㈡倵濮樼厧寮い銏℃礋閹稿﹥绔熷┑鍫熷殌妞ゎ厹鍔戝畷姗€骞嗚婵⊙呯磽?
//   1) 婵犵數濮烽。钘壩ｉ崨鏉戠；闁逞屽墴閺屾稓鈧綆鍋呭畷宀勬煛?5444(DoH 闂傚倸鍊风粈渚€骞夐敓鐘茬闁告縿鍎抽惌鎾绘煕椤愶絾绀冮柛? / 5454(DoH 闂傚倸鍊风粈渚€骞夐敓鐘冲殞闁诡垼鐏愯ぐ鎺撳€荤紒娑橆儐閺? / 443(nginx) 闂傚倸鍊风粈渚€骞栭銈傚亾濮樺崬鍘寸€规洝顫夌€靛ジ寮堕幋鐘垫毎濠电偞鎸婚崺鍐磻閹剧粯鐓涢悘鐐垫櫕婢э箓鏌熺粙鍖℃敾鐎垫澘瀚埀顒婄秵閸樺ジ骞嗛崼銉︹拻?
//   2) 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柦妯侯樈濞存牠鏌曟繛鐐珔缂佺姵鐓￠弻鏇＄疀鐎ｎ亖鍋撻弴鐘典笉婵炴垯鍨婚崣鎾绘煕閵夋垵瀚畷鍐差渻閵堝懐绠扮€规洦鍓熼崺銉﹀緞閹邦剛顔撻梺鍛婂壃閸涱垱鏅ㄥ┑鐘愁問閸犳褰犻梺鍛婃煥闁帮綁鐛幋锕€绀嬫い鏍ㄦ皑椤撳搫鈹戦悩璇у伐闁瑰嘲顑嗙€靛ジ宕惰閺€?sniroute.cmd up 闂傚倸鍊烽懗鍫曘€佹繝鍌楁灃婵炴垯鍨洪弲婵嬫煏閸繍妲归柛瀣ф櫊閺岋絽螣閾忕櫢绱炲銈傛櫊閺€閬嶅Φ閸曨垰绠崇€广儱娴傚Σ顔碱渻閵堝懎顒㈤柣鐔叉櫊楠炲啰娑甸崪浣光枌闂備浇銆€閸嬫捇鏌涘Δ鍐ㄥ壉闁?
// 闂傚倸鍊烽懗鍫曗€﹂崼銏″床闁割偁鍎辩粈澶愭煙鏉堝墽鐣辩痪鎯ф健閹鎮介崨濠冪彸闂佺粯甯掗敃顏勵嚕閸洖閱囨慨姗嗗幗閻濇棃鎮楃憴鍕婵炶尙鍠栧璇测槈濮楀棙鍍靛銈嗗笒鐎氱兘鍩€椤掍緡娈滈柡灞诲€濆鍫曞箣椤栨繂鎯堟俊銈囧Х閸嬬偤骞戦崶顒傚祦闁搞儺鍓﹂弫濠囧级閻愭潙顥嬪瑙勫▕濮婄粯鎷呴悷閭﹀殝缂備礁顑嗛崹鍧楀箚鐏炲墽鐟归柍褜鍓熼妴浣糕枎閹惧磭鍔﹀銈嗗笒鐎氼參鎮￠悢鎼炰簻妞ゆ劦鍋勬晶顔尖攽閳ヨ尙鐭欓柡灞剧洴閸╋繝宕掑鍕灡闂備浇顕栭崹浼存儗閸屾稐绻嗛柣鎴ｆ閻撴盯鏌嶈閸撴氨绮?闂備浇顕у锕傦綖婢舵劖鍋ら柡鍥╁С閻掑﹥绻涢崱妯虹仴闁搞劍绻堥弻銊╂偄閸濆嫅銏㈢磼閳ь剟宕卞☉娆戝幈闂侀潧顦伴崹鐢碘偓姘煎櫍钘濋柕澶嗘櫆閳锋垿鏌涘┑鍡楊伀濠⒀囦憾閺屾盯骞樼€靛憡鍣ч梺?DNS 闂傚倸鍊风粈渚€骞栭锕€纾瑰ù鐘差儜缂嶆牕顭块懜闈涘闁?闂?
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
        // up 濠电姷鏁搁崑鐐差焽濞嗘挸瑙﹂悗锝庡枟閸ゅ苯螖閿濆懎鏆欓柛灞诲妽缁绘盯骞嬪▎蹇曚患缂?闂傚倷娴囧畷鍨叏瀹ュ鐐婇柕濠忛檮濞堝憡绻濋悽闈浶涢柛瀣崌閺屾盯濡烽鐓庮潻缂備讲鍋撳鑸靛姈閻撳啴鏌曟径鍫濆姎闁哄棝浜堕弻锟犲川椤旂偓鍣伴梺鍝勬湰缁嬫捇鍩€椤掑﹦绉甸柛瀣槹缁旂喎螣婵傝棄缍婇幃鈺呮嚑椤掍焦鍠栭梻浣筋嚃閸ㄥ崬螞閸愵喖钃熼柕濞р偓閸嬫捇鏁愰崨顖欑驳闂佹娊鏀卞ú鐔奉潖濞差亜浼犻柛鏇ㄥ櫘濞煎爼姊虹粙鎸庡攭鐎规洟娼у嵄闁归偊鍏橀弨浠嬫煕椤愮姴鐏╃€规洏鍎遍—鍐Χ閸℃瑥顫ч梺鑲╂嚀婢х晫妲愰悙鍝勭闁哄倶鍎查～宥夋⒑鐟欏嫬鍔ょ痪缁㈠幗閻楀孩绻濈喊妯哄⒉鐟滄澘鍟撮獮妤€顭ㄩ崨顓炵亰闂佸啿鎼崯鐘诲磻閹剧粯鏅查幖瀛樼箘閻╁酣姊虹紒妯诲鞍缂佽鐗撳璇测槈濠婂懐鏉搁柣搴秵娴滄粓锝為崶顒佲拺缂佸娼￠妤併亜閺囥劌骞楃紒鍌氱Ч瀹曘劍绻濋崘顏喰ら梻浣哥枃濡椼劎绮堟担鍝勵棜闁秆勵殕閳锋垿姊婚崼鐔衡姇妞ゃ儲绮撻弻娑欑節娴ｈ櫣鐓撻梺璇″枛濞尖€崇暦閸楃偐鏋庨柟閭﹀墰瑜版挳姊绘笟鈧褏鎹㈤崱娑樼婵犻潧娲ㄩ々閿嬩繆椤栨瑨顒熼柣?30 缂傚倸鍊搁崐椋庣矆娓氣偓钘濋柟娈垮枟閺嗘粍銇勮箛鎾搭棡妞?
        // 濠电姷鏁搁崑娑㈩敋椤撶喐鍙忛悗闈涙憸閻挾鈧娲栧ú銊х不閺傛鐔嗛悹鍝勬惈椤ㄦ瑩鏌ｉ幒鎴含鐎殿喖鐖煎畷鐓庮潩椤撶喓褰呴柣搴ゎ潐濞叉ê煤閻旂厧绠栨俊顖濇硶閻も偓闂佽鍎抽悿鍥春閻愮儤鈷戦柛婵勫劚閺嗛亶鏌涚€ｎ偅宕岄柡宀嬬秮閹晠妫冨☉妯侯瀱闂備焦鎮堕崝宀€澹曢鐘插灊闁冲搫鎳庣痪褔鏌熺€涙ɑ鈷愰柣?up闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈鹃檮閸嬪鈹戦悩鎻掍喊闁兼澘娼￠弻娑㈠Ψ椤旂厧顫╃紓浣插亾濠㈣埖鍔栭悡鍐煏婢跺牆鍔氶柡鍡涗憾閹鎷呴崷顓炲绩闂佸搫琚崐妤呭焵椤掆偓濠€杈ㄦ叏閻㈡潌澶嬪緞閹邦厾鍘遍梺闈浤涢崘鐐瘔缂傚倷娴囧銊у垝濞嗗繒鏆﹂柟鎵閸嬪棗霉閿濆浂鏀伴柡鍛囧啠鏀介柣鎰綑閻忥箓鏌熼崨濠冨€愰柡浣稿暣閺佹劙宕堕埡浣歌Е闂備礁鎼ˇ浼村垂閻旂厧鍑犻幖娣妽閻撱儵鏌ｉ弬鎸庢儓鐎涙繂鈹戦悙鍙夊櫡闁搞劋绮欏璇测槈濮楀棙鍍甸梺缁樻尭濞撮攱绂掗鐔虹瘈闁冲皝鍋撶€广儱娲ㄩ弳銈夋倵濞堝灝娅橀柛鎾跺枛閵嗕礁顫滈埀顒佷繆濮濆矈妲惧銈忓瘜閸嬪﹤顫?
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
        // up 闂傚倸鍊烽悞锕傛儑瑜版帒绀夌€光偓閳ь剟鍩€椤掍礁鍤柛娆忓暣閵嗕礁螖閳ь剟鈥﹂妸鈺佸窛妞ゆ棁濮ら鍥⒒娴ｅ憡鍟炵紒鍝勬健瀹曟洟宕樺ù瀣亙闂侀潧绻堥崐鏍偂濞戞﹩鐔嗛悹娲細鐎氫即鏌＄€ｎ亜顒㈤柟渚垮妼铻ｇ紒瀣仢椤绱撴担绋库偓鍦暜閿熺姴鏋侀柟鐗堟緲闁卞洦绻涢幋鐐茬瑨濞存粓绠栭弻娑㈠箛闂堟稒鐏堥柣搴㈣壘椤﹂亶鍩€椤掆偓缁犲秹宕曢柆宥呯疇閹兼惌鐓夌紞鏍煏閸繃绀岄柛瀣崌瀹曠兘顢樿閸旀悂鎮楀▓鍨灕闁糕晜鐗犲畷姘節閸ャ劍鍎柣鐔哥懃鐎氼剟鎯冮锔解拺闁告稑锕ら悘鐔兼煕婵犲喚娈滈柣娑卞櫍婵偓闁靛繈鍨婚敍婊堟⒑閸濆嫬鏆為柛銊ф暬閹﹢鎮╅崹顐㈡瀾闂佺粯顨呴悧蹇撁虹€涙﹩娈介柣鎰綑濞搭喖鈹戦鐟颁壕闂備焦瀵х粙鎴犫偓姘ュ姂瀹曟垿鎮欓悜妯锋嫼闂佸憡绋戦敃锕傚箠閹邦優鐟邦煥閸涱厺妲愰悗娈垮櫘閸嬪﹥淇婇懜闈涚窞濠电姴瀚伴悰鎾绘⒒娓氣偓濞佳囁囬銏犵？闂侇剙绉跺畵渚€鏌熸潏鍓х暠缂佺姴缍婇幃妤€鈽夊▍铏灩缁氨鎹勯妸褏锛滈柡澶婄墑閸斿海绮婚弽顐ょ＜閻庯綆浜跺Σ鍛娿亜閹剧偨鍋㈢€规洖銈搁、鏇㈡晲閸モ晛骞€闂傚倸鍊风欢姘焽閼姐倖瀚婚柣鏃傚帶缁€澶屸偓鍏夊亾闁告洦鍓欐禒濂告⒑缂佹﹩鐒界紒顕呭灦閹繝宕掑┃鎯т壕妤犵偛鐏濋崝姘舵煙閸愯尙绠荤€?
        // 闂傚倸鍊风粈渚€骞栭锔藉亱闁糕剝鐟ч惌鎾绘倵濞戞鎴﹀矗韫囨稒鐓熼柡鍌氱仢閹垿鏌涙惔锛勑ч柡灞剧洴瀵挳濡搁妷銉ь啋闁诲氦顫夊ú鏍偉婵傜钃熸繛鎴炃氶弸搴㈢箾閸℃ê鐏╃悮锕傛⒒娓氣偓濞艰崵绱炴笟鈧棢闁规崘顕х粻顖炴煟濡粯銇熼柡浣哥У閹便劌顫滈崱妤€顫繛瀵稿帶閻楁挸顫?婵犵數濮烽弫鎼佸磻濞戙垺鍎戦柡灞诲劚鍥撮梺绯曞墲椤ㄥ﹤螞閻戣姤鈷?闂?
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


// WARP 闂傚倸鍊风粈渚€骞栭銈傚亾濮樺崬鍘寸€规洝顫夌€靛ジ寮堕幋鐘垫毎濠电偞鎸婚崺鍐磻閹剧粯鐓欐い鏂跨仢琚氬┑鐐靛帶缁绘﹢宕洪敓鐘茬＜婵炴垶鐟崥鍌炴⒒閸屾瑧顦︽繝鈧柆宓ュ骞囬閿亾閹烘鎹舵い鎾寸☉娴滈箖姊婚崼鐔恒€掗柣鎺戞憸閳ь剝顫夊ú妯兼崲閸喍绻嗛柟闂寸鍞梺鎸庢磵閸嬫捇鏌涢幘鎰佺吋婵﹤鎼叅缂備焦锚閳诲繘姊洪崨濠冪叆闁挎洦浜畷娲焵椤掍降浜滈柟鍝勬娴滈箖姊虹粙鍖″伐缂傚秴锕獮鍐Ψ閳哄倹娅滈梺鎼炲劘閸斿矂顢撳☉銏♀拺缂備焦锕╁▓鏃堟煟濡も偓閿曨亪骞冨Δ鍜佹晩婵?data\.warp-enabled闂?
//   闂傚倷娴囬褏鈧稈鏅濈划娆撳箳濡や焦娅斿┑鐘垫暩婵參宕?  -> 闂傚倸鍊烽悞锕€顪冮崹顕呯劷闁秆勵殔缁€澶屸偓骞垮劚椤︻垶寮伴妷锔剧闁瑰鍋熼。鏌ユ煛閸℃鐭掗柟顔斤耿閹瑩妫冨☉妤€顥氱紓?WARP 闂備浇顕х€涒晠顢欓弽顓炵獥闁圭儤顨呯壕濠氭煙閸撗呭笡闁绘挾鍠愰妵鍕箛椤斿吋鐎绘繛瀛樼矒缁犳牕顫忓ú顏勭閹艰揪绲哄Σ鍫ユ⒑缂佹﹩娈曢柟鐟版搐閻ｇ兘骞庨挊澶岊啇婵炶揪绲鹃悺鏇☆杺濠碉紕鍋戦崐鏍偋濡ゅ啫鏋堢€广儱顦壕褰掓⒒閸喕鍎愰幖娣妼閻愬﹪鏌曟繛褍鎳愰弳顐︽⒑鐠囨煡顎楅柣蹇旇壘铻為柛鎰靛枛閺?
//   濠电姷鏁搁崑鐐哄垂閸洖绠伴柛婵勫劤閻捇鏌熺紒銏犳灍闁稿鍊濋弻鏇熺箾閻愵剚鐝旈梺?-> 濠电姵顔栭崰妤冩暜濡ゅ啰鐭欓柟鐑樸仜閳ь剨绠撳畷濂稿Ψ椤旇姤娅嶅┑鐘垫暩婵敻鎳濋崜褏鐭嗛柛宀€鍋為悡娆撴煙椤栨稒绶茬悮姘舵⒑閹稿海鈯曟俊顐㈠暣瀵濡搁妷銏☆潔濠碘槅鍨抽崢褔銆傜憴鍕箚闁靛牆娲ゅ瓭濠电偛妯婇崢濂告偩閻戠瓔鏁冮柨婵嗘川閹虫繈姊洪柅鐐茶嫰婢т即鏌涢幒鎾虫诞妞ゃ垺绋戦…銊╁礋椤愵偀鍋撳☉銏♀拺闂傚牊绋撴晶鏇熺箾鐏炲倸鈧鎮疯濮婄粯鎷呯粵瀣異闂佺绻戠粙鎾斥枎閵忋倕围闁搞儺鐓堝鐔兼⒒娓氬洤澧紒澶婎嚟閹叉挳鏁傛慨鎰盎闂佸搫绉查崝蹇旂珶濡吋鍋栭柣鏂垮悑閳锋垿鏌涘┑鍡楊仼闁哄棛鍋ら弻娑欐償閵堝懍姹楅梺閫炲苯澧柛婵嗛叄瀹曞綊鎸婃径灞炬闂佹寧绻傞ˇ顖滅不閿濆鐓熸俊顖氱仢閻ㄦ椽鏌ｉ妶鍌氫壕 WARP 闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墐閳ь剨绠撳畷濂稿閳ュ啿绨ユ繝鐢靛仦閸垶宕瑰ú顏勭？闁瑰墽绮悡鍐喐濠婂牆绀堟繛鎴炵懅椤╂煡鏌涘☉姗堟敾缂傚秴娲弻鏇熺節韫囨搩娲梺鍛婃⒒閸忔﹢骞冨Δ鈧埢鎾诲垂椤旂晫浜愰梻浣侯焾椤戝棝骞戦崶顒€钃?
// 闂傚倸鍊烽悞锕€顪冮崹顕呯劷闁秆勵殔缁€澶屸偓骞垮劚椤︿即宕戞径鎰叆婵犻潧妫欐径鍕攽椤栨哎鍋㈤柡宀嬬到铻栭柍褜鍓熼幃褎绻濋崶銊モ偓鍨亜閺冣偓缁佺銇愰幒鎾充汗闂佸憡鍔栬ぐ鍐╂叏閹间焦鈷戦梺顐ゅ仜閼活垱鏅堕灏栨斀闁稿本绋掗埛鎺旂磼椤曞懎骞楃€垫澘瀚伴獮鍥敆閳ь剙顕ｅ畡閭︽富闁靛牆妫涙晶顒傜磼鐠囪尙效鐎?JSON闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈剧畱鐎氬銇勯幒鍡椾壕闁告浜堕弻娑樷槈濮楀牆濮涢梺宕囩帛濡啫顕ｉ崼鏇熷€烽柡澶嬪焾濡棝姊洪崫銉ユ瀻闁挎洦浜?.autostart-off 濠电姷鏁搁崕鎴犲緤閽樺娲晜閻愵剙搴婇梺鍛婂姦娴滄牠宕戦幘璇插瀭妞ゆ劧缍嗗Λ鍐倵鐟欏嫭纾搁柛搴ゆ珪缁傛帡鏁冮崒娑樻異闂佸啿鎼敃锝囨濡偐纾介柛灞剧懅椤︼附銇勯敂璇茬仯缂侇喗姊婚埀顒佺⊕椤洭銆呴柨瀣闁瑰鍊戝璺哄嚑闁哄稁鍘介悡娑㈡煕閵夛絽澧告慨妯挎硾绾?
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

// 闂傚倸鍊风粈渚€骞夐敓鐘茬鐟滅増甯掗崹鍌炴煟濡も偓閻楀﹪宕ｈ箛娑欑厓闁告繂瀚崳鍦磼椤愩垻效闁哄被鍊濋獮鏍ㄦ媴鐟欏嫰鏁梻浣瑰▕閺€鍗炍涢崘顔肩畺?濠电姵顔栭崰妤冩暜濡ゅ啰鐭欓柟鐑樸仜閳ь剨绠撳畷濂稿Ψ椤旇姤娅嶅┑鐘垫暩婵敻鎳濋崜褏鐭嗛柛宀€鍋為悡娆撴煙椤栨稒绶茬悮姘舵⒑?闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟鐑樺焾濞尖晠鏌曟繛鐐珔闁哄绶氶弻鏇㈠醇濠垫劖笑缂備胶濯寸紞浣割潖濞差亜鍨傛い鏇炴噹閸撳啿鈹戦纭疯含缂佺姵鐗犲璇测槈濮楀棙鍍甸梺缁樻尭濞撮攱绂掗鐐粹拺閺夌偘鍗冲鐑芥煕婵犲啰澧い顓炴处缁绘繂顫濋鍌氣偓鐐烘⒑闂堟侗妲堕柛銊潐缁傚秹骞嗛‖鈥崇秺閺佹劖寰勫Ο娲绘缂?WARP 闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墐閳ь剨绠撳畷濂稿閳ュ啿绨ユ繝鐢靛仦閸垶宕瑰ú顏勭？闁瑰墽绮悡?-> 闂傚倸鍊风粈渚€骞栭锕€纾婚柛鈩冪☉閸屻劑鏌ゅù瀣珕妞ゎ偅娲熼弻鐔告綇閸撗呮殸閺?
// 闂傚倸鍊风粈渚€骞夐敓鐘冲仭妞ゆ牗绋撻々鍙夌節婵犲倸鏋﹂柤鐗堝閵囧嫯绠涢幘璺侯暫闁荤喐鐟遍梽鍕Φ閸曨垰鍐€闁靛鍔岄ˉ婵嬫偠濮樺崬鏋涙慨濠冩そ瀹曨偊宕熼锝嗩啈婵犵數鍋涢ˇ鎵矙閹烘梻鐭夐柟鐑樺灍濡插牊绻涢崱妯虹亶闁哄鐗犲娲焻閻愯尪瀚板褜鍣ｉ弻宥囨嫚閺屻儱寮伴梺绯曟杺閸庨潧鐣烽妸鈺婃晬婵犲﹤瀛╁暩濠电姷鏁搁崑鐐哄垂閸洖绠伴柛婵勫劤閻捇鏌ｉ姀銏╃劸婵☆偅锚闇夐柨婵嗙墱濞兼劕霉濠у灝鈧牜鎹㈠☉銏犵闁绘垵妫涢崝鐑芥⒑閸濆嫭顥欓柛妤佸▕瀵鏁撻悩鑼槰濡炪倖鎸鹃崰鎾诲Υ閹邦兘鏀介柣鎰级閸ｆ椽鎮介銈囩瘈鐎殿噮鍋勯濂稿川椤栨稒鐣烽梻浣告啞濞诧附绂嶉悙鍨潟婵鍩栭埛鎴犵磼鐎ｎ厽纭剁紒鐘冲缁辨帗寰勭€ｎ偄鍞夊Δ鐘靛仜缁绘垹鈧絻鍋愰埀顒佺⊕閿氬ù鐘虫そ濮婃椽妫冨☉杈╁彋缂備胶濮甸崹鍧楀箖閿熺姴鍗抽柕蹇ョ磿閸樼敻姊婚崒姘卞濞撴碍顨婂畷鏇＄疀閹绢垱鏂€濡炪倖妫侀～澶娾枍閸℃绠鹃柣鎾冲瘨閻撳ジ鏌熼鐣屾噰鐎殿喖鐖奸獮瀣攽閹邦厜鈺呮⒒閸屾瑨鍏岄柟铏崌閹ê顫濈捄铏诡唶婵犵數濮电喊宥夊磹閸洘鐓ラ柡鍥╁仜閳ь剚鐡曠换?
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

// ---------------------------------------------------------------- WARP 闂傚倸鍊烽懗鍫曗€﹂崼銉晞闁告劦鍠栫壕鍧楁⒑椤掆偓缁夊绮婚弽顐熷亾楠炲灝鍔氭い锔诲灠椤斿繐鈹戦崱蹇旀杸闂佺粯锚瀵埖鍒婃總鍛婄厱?
// 濠电姷鏁搁崑鐐哄垂閸洖绠伴柛顐ｆ礀绾捐淇婇妶鍛珔缂?WARP 濠电姷鏁搁崑鐐哄垂閸洖绠伴柛婵勫劤閻捇鏌ｉ幋锝嗩棄缂佲偓閸屾稒鍙忔俊鐐额嚙娴滈箖姊?闂?
//   1) 闂傚倸鍊烽懗鍓佸垝椤栫偐鈧箓宕奸妷銉︽К闂佸搫绋侀崢濂告倿閸偁浜滈柟瀵稿仜椤曟粍銇勯敃鍌ゆ缂?connected 闂傚倸鍊风粈渚€骞夐敓鐘冲仭闁挎洑鐒﹂幊宀勬⒒?disconnected      -> 缂傚倸鍊搁崐鐑芥倿閿曞倸钃熼柕濞炬櫓閺佸嫰鏌涘☉娆愮稇闁哄嫨鍎甸弻鏇＄疀鐎ｎ亖鍋撻弽顓熷€跨紓浣股戦崣蹇斾繆椤栨哎浠掗柛姘煎亰閺屾稑鈻庣仦鎴掑
//   2) 闂傚倸鍊烽懗鍓佸垝椤栫偐鈧箓宕奸妷銉︽К闂佸搫绋侀崢濂告倿閸偁浜滈柟瀵稿仜椤曟粎绱掓担鍝勵暭闁?connected 濠电姷鏁搁崑鐘诲箵椤忓棗绶ら柦妯猴級濞戞鏃堝川椤撶偞鍎俊鐐€栭幐鍫曞垂瑜版帗鍎楁俊銈呮噺閻撱儲绻濋棃娑欘棡妞ゃ儲绮撻幃纭咁槻闁宦板妿閹广垹鈽夐姀鐘甸獓闂佽鎯岄崢濂稿礈椤斿墽纾藉ù锝堫嚃濞堟梹銇勯敂鐐毈鐎殿喖顭峰畷銊╊敇瑜庨崓鐢告⒑閸涘﹥瀵欓柛婊€鐒﹁ⅸ濠电姷鏁搁崑鐐哄垂閸洖绠伴柛顐ｆ礀閸ㄥ倿鏌涢敂璇插箰闁稿鎹囧畷褰掝敃閸℃ê鐏撮柕?缂傚倸鍊搁崐鐑芥倿閿曗偓铻為煫鍥ㄧ☉閸ㄥ倿鏌熼幆鐗堫棄缁炬儳顭烽弻銊モ攽閸℃ê顦╅梺绋款儐閻楁濡甸崟顖氱疀闁割偅娲橀宥咁渻閵堝懎顒㈤柣鐔叉櫊瀵?
//      -> 闂傚倷绀侀幖顐λ囬锕€鐤炬繝闈涱儏绾惧鏌ｉ弬鍨倯闁稿鍔庣槐鎾存媴婵埈浜炲▎銏ゆ倷閸濆嫮楠囬梺鍓插亽閸嬪嫭绂嶉婊勫仏?2 婵犵數濮烽弫鎼佸磻濞戞娑樼暆閸曨偆顦┑鐘绘涧濞诧箑鈻嶉悩缁樼厽婵☆垵鍋愮敮娑㈡煟閹烘梻甯涢柟渚垮妼铻ｅ鍓侇焾閹牓姊洪崨濠勬喛闁稿鎹囧缁樻媴閻熼偊鍤嬬紓浣割儐閸ㄥ墎缂撴禒瀣睄闁稿本绮庨悾?10 闂傚倸鍊风粈渚€骞夐敍鍕殰闁圭儤鍤﹀☉妯锋斀閻庯綆浜為鍥⒑鐟欏嫬绀冩い鏇嗗懐鐭嗛柛鎰靛枟閻撴洘绻涢幋婵嗚埞闁哄鍠庨湁闁绘绉鍫氣偓锕傚锤濡や礁娈濋梺姹囧灮閺佹悂宕ｅ┑鍫㈢＝濞达綀顫夐埛鎰箾閸忚偐鎳囬柛鈹垮灪閹棃濡搁妷褍寮ㄩ梻浣告啞娓氭宕归幎鍓垮洭顢氶埀顒€顫忕紒妯肩懝闁搞儜鍐炬交闂備浇顕х换鎴犳崲閸繄鏆﹂柕蹇嬪€曞洿婵犮垼娉涢敃锕偹囬锔藉€甸柣鐔告緲椤忣亜顭块悷鐗堫棞妞?
// 闂傚倸鍊烽悞锕€顪冮崹顕呯劷闁秆勵殔缁€澶屸偓骞垮劚椤︻垶寮伴妷锔剧闁瑰鍋熼幊鍛存煃缂佹ɑ顥堥柟顔肩秺瀹曞爼濡搁妷褏銈烽柣鐐存皑閸犳牕顫忓ú顏勪紶闁告洦鍋傜花浠嬫⒑閸涘鐒鹃柛瀣姇鍗遍柟鐗堟緲缁狅綁鏌ㄩ弴妤€浜剧紓?g_warpManualStop=true闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟瀵稿仧闂勫嫰鏌￠崘銊モ偓鑽ょ不閺傛鐔嗛柤鎼佹涧婵洨绱掗悩顔煎姦闁哄瞼鍠栭幃娆擃敆娴ｈ櫣鈻忔俊鐐€戦崕杈╃矓瑜版帒钃?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊搁崐椋庢閿熺姴纾婚柛娑卞枤閳瑰秹鏌ц箛姘兼綈鐎?WARP 闂傚倸鍊风粈渚€骞夐敓鐘冲仭闁挎洖鍊归崑瀣煛閸愩劌鈧宕″鑸靛€垫繛鎴炵懐閻掍粙鏌?204 缂傚倸鍊搁崐鐑芥倿閿曗偓铻為煫鍥ㄧ☉閸ㄥ倿鏌熼幆鐗堫棄缁炬儳顭烽弻銊モ攽閸♀晜笑閺夆晜绻堝铏圭矙閹稿孩鎷辩紓浣筋嚙鐎氼喚鍒掓繝姘摕闁靛濡囬崢?true 闂傚倷娴囧畷鐢稿磻閻愮數鐭欓煫鍥ㄧ☉缁€澶愬箹濞ｎ剙濡煎鍛攽椤旂瓔鐒炬繛澶嬬〒缁牓宕熼娑氬幈濠电娀娼уΛ妤咁敂閳哄啰纾界€广儱妫欏畷宀勬煛鐏炲墽鈽夐柍璇查铻ｉ柧蹇涒偓娑樺闂傚倷娴囧銊╂⒔瀹ュ纾规繝闈涙矗缁?
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
        if (st == 0) {                     // 婵犵數濮烽弫鎼佸磻濞戙垺鍎戦柡灞诲劚鍥撮柟鍏肩暘閸╁嫬螞閸涘瓨鈷戦悹鍥ㄧ叀椤庢绱掗悩鑼х€规洜鎳撶叅妞ゅ繐瀚Σ顒€鈹戦悙鏉戠仸婵ǜ鍔戝畷鈥愁潩椤撴粈绨婚梺鍝勭Р閸庨亶鍩€椤掍緡娈樼紒顔肩墛瀵板嫭绻涢幒鎴犵Ш闁诡喒鏅犻、鏇綖椤撶儐妫滃┑鐘垫暩閸嬬偤宕归崼鏇炶摕闁靛ň鏅滈崑瀣煙閹规劦鍤欑紒鈧崒娑楃箚妞ゆ牗绻傞崥褰掓煃缂佹ɑ鈷掗柍褜鍓欑粻宥夊磿闁秴绠犵€广儱娲ら崹?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倸鍊风粈渚€骞栭锕€纾婚柛鈩冪☉閸屻劑鏌ゅù瀣珕妞?
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
constexpr wchar_t kAppVersion[] = L"3.17.0";

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
    // 闂傚倸鍊风粈渚€宕ョ€ｎ兘鍋撶粭娑樻噹閸ㄦ繈鎮橀悙鍨珪闁哄棴绠撻弻锝夊籍閸屾艾浠樼紓浣哄У濠㈡﹢鈥﹂崸妤佸殝闂傚牊绋戣婵犵數鍋涢悺銊ッ洪悢鐓庣畺婵°倐鍋撻柍钘夘槸閳诲骸螣閼姐倖娈鹃梻鍌欐缁鳖噣姊介崟顓熷床闁割偁鍎冲畵渚€鏌熼悜姗嗘當缂佺姵濞婇弻鐔衡偓鐢殿焾鍟稿銈忕稻閻撯€愁潖濞差亜绠伴幖杈剧岛婵洨绱掗悙顒佺凡缂佸鎳撻锝夊箮閼恒儱浜滈梺缁樻尭濞寸兘顢欐径濠庢富闁靛牆妫欓ˉ鍡椻攽椤斿搫鈧繈骞冨Δ鍜佹晩婵炴潙楠哥紞濠囧箖閳哄懏鍊绘俊顖滎儠閳ь剚鍔欓幃宄邦煥閸涱収鏆梺缁樻惈缁绘繈鐛繝鍋芥棃宕ㄩ鍏肩枀闂備線娼ч¨鈧紒鐘虫尭椤﹪鎮滈懞銉㈡嫼闂佺粯鎸哥€垫帒鈻撻悩瑁佺懓顭ㄩ崟顓犵暤闁捐崵鍋為妵鍕箛閸洘顎嶉梺绋款儏椤戝寮诲☉銏╂晝闁靛牆鎳忛悗楣冩⒑閸︻厼鍘村ù婊冪埣瀵濡搁妷銏☆潔濠碘槅鍨抽崢褔路閳ь剚绻濋悽闈浶涢柛瀣尰閵囧嫰骞橀崡鐐典患缂備胶濮垫繛濠囧蓟閻斿吋鍊绘俊顖濇娴犺偐绱掗悙顒佺凡缂佸缍婂濠氬Ω閵夈垺顫嶅┑顔筋殔濡瑩顢撻崱娆戠＝濞达絼绮欓崫铏圭磼鐠囨彃鏆欐い顓炴川閹风姴霉鐎ｎ偆鈧姊虹憴鍕婵炲懏娲熼、鏃堟倷閻戞ǚ鎷洪梺鍛婄☉閿曘倖鎱ㄩ崶鈺冪＜濠㈣泛锕﹂敍宥夋煙楠炲灝鐏╂い顐ｇ箖濞煎繘濡搁妷褍閰辨繝鐢靛仩閹活亞绱炴笟鈧棢闁圭偓妞块弫鍌炲级閸稑濡跨紒鐘荤畺閺屾盯鎮╅銏犫拤婵犮垻鎳撻澶婄暦?
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
        // WARP 闂傚倷娴囬褏鈧稈鏅濈划娆撳箳濡も偓缁€澶嬩繆椤栨繐楠忓ù婊冩贡閳ь剙绠嶉崕閬嵥囬婊勫厹闁逞屽墴濮婃椽宕崟顒傘偐婵炲瓨绮庨崑鐔哄垝閿濆憘鏃堝礃椤忓棴绱?Sniroute 闂傚倸鍊风粈渚€骞夐垾鎰佹綎缂備焦蓱閸欏繒鈧娲栧ú銊╁础濮樿埖鐓欑紓浣靛灩閺嬫稓绱掗悪娆忔处閻撳啴鏌曟径娑氱暠濠⒀屽灦閺岋繝宕卞Ο鑽ゅ嚒濡炪値鍘煎锟犲箠濠婂牊鍋ㄩ梻鍫熺▓閸氬倿姊绘担瑙勫仩闁稿鍊濆畷锝夊礃閵娧勬闂佹寧绻傞ˇ浼村疾閺屻儳鍙撻柛銉ｅ妽缁€鍫熴亜閿旇　鍋撻埡鐜眊le/YouTube/Play 缂傚倸鍊搁崐鎼佸磹閹间礁鐤い鏍仜閸ㄥ倿鏌涜椤ㄥ懓绻氬┑鐘灱閸╂牠宕濋弽顓炵；閻庯綆鍠楅悡鏇熴亜閹伴潧浜滃褎澹嗙槐鎺楀焵椤掑嫬骞㈡繛鎴炵懅閸?
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

    // 闂傚倸鍊风粈渚€骞栭锔藉亱闁告劦鍠栫壕濠氭煙閹规劦鍤欑紒鐘茬秺閹鈽夊▍铏灩缁牏鈧綆鍠楅悡鏇㈠箹濞ｎ剙鐏悘蹇ョ畵閺岀喖顢涘顒佹閻庤娲滈崰鏍€佸鈧幃婊堝幢濡崵妲橀梻鍌氬€风粈渚€骞栭位鍥敍閻愯尙鏌堥梺纭呮硾椤洟鍩€椤戣法绐旀鐐差儔閺佸倿鎳為妷锔惧建濠电姵顔栭崰妤呮晪閻熸粍婢橀ˇ鍗炍ｉ幇顑╂棃宕ㄩ鎯у箻缂備胶铏庨崢濂稿箠鎼达絾顫曢柨鏂垮⒔绾惧吋淇婇妶鍌氫壕濡炪倧绠撳褔鎮鹃悜鑺ユ櫜濠㈣埖蓱閺咁剙鈹戦悩璇у伐闁哥啿鏅犲畷鐘诲冀椤撶啿鎷洪梺鍦焾濞寸兘鍩ユ径鎰厾婵炶尪顕ч悘銉╂煏閸ャ劌濮屾い锕€缍婇弻鐔碱敊閼姐倗鐓撴繝纰樺墲閹倿寮崒娑氼浄閻庯綆鍋掗埀顒佺墵濮婄粯鎷呴崨濠冨創缂備礁顦板娆戝弲闂佹寧娲嶉崑鎾诲础闁秵鐓欓弶鍫濆⒔閻ｈ京绱掗埀顒勫礋椤栨稓鍘卞┑鐘绘涧濡顢旂€涙ü绻嗛柡灞诲劙閹查箖鏌＄仦鍓р槈妞ゎ厹鍔戝畷姗€顢旈崱鏇犵婵犵數濮幏鍐礋閸偆鏉归柣搴㈩問閸犳岸寮繝姘モ偓渚€寮介鐔哄弳濡炪倖鐗楅惌顔界珶?
    // 婵犵數濮烽弫鍛婃叏瀹ュ绀嬫い鎰╁焺濞兼洟姊绘担铏瑰笡闁绘顨堥崚鎺戭吋閸モ晝鐓旈梺鍛婎殘閸嬫劙寮ㄦ禒瀣厱闁靛鍨甸幊蹇撴毄缂?ASCII闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈剧畱鐎氬銇勯幒宥囧妽婵炲懐濮烽埀顒€绠嶉崕鍗炍涘☉顫偓?U+00A9(濠? 闂?U+00B7(闂? 闂傚倸鍊烽悞锕€顪冮崹顕呯劷闁秆勵殔缁€澶愮叓閸ャ劍濯肩憸鐗堝笒楠炪垺淇婇姘辨癁闁哄鐟︾换婵嬪閿濆棛銆愰梺鎸庢磸閸婃繂顕ｉ弻銉ョ濞达絽婀遍崢?
    {
        // 闂傚倸鍊烽懗鍓佸垝椤栫偑鈧啴宕ㄧ€涙ê浜辨繝鐢靛Т閸嬪棝鎮?+ 闂傚倷娴囧畷鍨叏閹惰姤鍤勯柛顐ｆ磵閳ь剨绠撳畷濂告晲閸愌勭潖闂備礁鎼悮顐﹀磿鏉堚晝鐭嗛柛鈩冪⊕閻撴洟鏌嶉埡浣告灓婵炲牊顭囬惀顏堝箲閹伴潧娈梺瀹犳椤︾敻鐛鈧畷锟犳倷閳轰椒澹曞┑鐐村焾濞煎潡鍩€椤戣法绐旀鐐差儔閺佸倿鎳為妷锔惧建濠电姵顔栭崰妤呮晝閳哄懎纾婚柛鈩冧緱閺佸倿姊洪鈧粔鐢稿磻閻樺磭绠鹃柟瀵稿剱閻掗箖鎮楀顐㈠祮闁哄本娲熷畷鎯邦檪闂婎剦鍓氶幈銊︾節閸滀焦缍堥悗鍨緲鐎氭澘鐣烽锕€唯鐟滃繐鐣烽崼鏇熲拺闁煎鍊曟禒锕傛煕閹存繄绉虹€规洘鍨跺鍕償濞戞鐫忛柣搴＄畭閸庨亶藝椤栨粍鍏滈柍褜鍓熷娲传閸曨剙绐涢梺鍝ュТ闁帮綁骞冮悙瀵哥瘈婵﹩鍘鹃崢鎼佹⒑缁嬭儻顫﹂柛濠冩礀椤曪絿鎹勯妸褏锛滈柡澶婄墑閸斿海绮婚悙鐑樼厵闁惧浚鍋呯亸闈浢归悪鍛暤闁硅櫕鐗犻崺锟犲礃閳轰焦顏?All rights reserved"
        // 闂傚倸鍊风粈渚€骞夐敓鐘偓鍐幢濡炴洖鎼…銊╁川椤忓嫪澹曞┑顔筋焽閸樠冾嚕椤旂瓔娈介柣鎰▕閸庢梹顨ラ悙鍙夊枠妞ゃ垺鐩弫鎰板幢濡粯绶┑鐘垫暩閸嬬偤宕归崼鏇炵闁告縿鍎抽惌鎾绘煙缂併垹娅橀柡浣革躬閺岀喖鎮ч崼鐔哄嚒闂佸憡鐟ラ敃顏堝蓟閻旂厧绠查柟浼存涧濞堟劕鈹戦悙闈涘付闁绘牕銈搁獮鍐亹閹烘垹鍊為柣鐘荤細濞咃絾绂掗弶鎴富闁靛牆妫欑壕濠氭煕濡や礁鈻曢柣?闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墛閸嬶繝鏌曢崼婵愭Ч闁绘挸鍊圭换婵囩節閸屾凹浠存繛瀛樼矋缁捇寮诲☉銏℃櫆閻犲洦褰冪粻铏圭磽娴ｇ懓濮堟い銊ユ嚇閸┿儲寰勯幇顒傤啋閻庤娲栧▔锕傛晲閸℃洜绠氶梺鑲┾拡閸擄箓鎮為悙顑句簻闁哄浂浜炵粔顕€鏌涢埞鎯т壕婵＄偑鍊栧濠氬磻閹剧粯鐓冮柣鐔告緲閺嗭絾顨ラ悙鍙夊枠妞ゃ垺鐟╅幃鍓т沪閻愵剦鍟?闂?
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
            // 闂傚倷娴囬褍霉閻戣棄鏋侀柟闂寸閸屻劎鎲搁弬璺ㄦ殾闁汇垹澹婇弫鍥煟閺冨洢鈧偓闁哄鐗犻幃妤呯嵁閸喖濮庨柣搴㈠嚬閸犳岸骞忚ぐ鎺戠厸闁告侗鍠栨禒顖炴偡濠婂啴鍙勯柟顔ㄥ洤鍗抽柕蹇婃櫆閺咃綁鎮峰鍛暭閻㈩垱甯炵划濠氭晲閸滀焦瀵岄柣搴秵娴滄粓鎯侀妸銉冨綊鎮℃惔鈾€鍋撻崸妤€钃熼柕濞垮劗濡插牊绻涢崱妯忣亪宕滅猾鎬?濠电姷鏁搁崑鐐差焽濞嗘挸瑙﹂悗锝庡亞閳瑰秵绻涘顔荤盎闁告俺顫夌换婵囩節閸屾碍鐏撻梺鍝ュ枎閹虫ê顕ｉ崼鏇為唶婵炴垶锚椤姊洪崫鍕棞闁绘鎹囧?WARP 闂傚倷娴囧畷鍨叏閻㈢绀夌憸蹇曞垝婵犳艾绠ｉ柨婵嗗暕濮规姊虹粔鍡楀濞堟鈧鎸风欢姘跺箖瑜版帒浼犻柛鏇ㄥ墯閸庢捇姊?YAML闂?
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
        // winget 闂?闂備浇顕уù鐑藉箠閹捐绠熼梽鍥Φ閹版澘绀冩い鏃囨娴犲ジ姊洪棃娴ゆ盯宕熼锝嗙彺闂傚倷绀侀幉鈩冪瑹濡ゅ懎绀堟繛鎴欏灪閸嬧剝銇勯幒鎴濐仾闁绘挻鐩弻娑㈠箛閸忓摜鐩庨柡宥佲偓鏂ユ斀闁绘劖娼欑徊缁樼箾鐏忔牑鍋撻幇浣告?闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墕閽冪喖鏌曟繛鍨壄婵炲樊浜滈崘鈧銈嗘尵閸ｃ儱螞?0闂?1978335189闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟娆″眰鍔戦崺鈧い鎺戝€荤壕濂稿级閸稑濡跨紒鐘靛仱閺岋紕浠﹂懞銉ユ灎闂佽桨鐒﹂崝娆忕暦閻旂⒈鏁囬柣妯哄暱閺嬫捇姊婚崒娆戭槮闁圭⒈鍋嗛埀顒佹皑閸忔ê鐣风憴鍕嚤闁哄鍤﹂敃鍌涚厵閻庣數顭堟牎闁诲孩鑹鹃…鐑藉蓟閺囩喎绶炴繛鎴炶壘閻繈姊?
        // 濠电姷鏁搁崑娑㈩敋椤撶喐鍙忓ù鍏兼綑绾惧潡姊洪鈧粙渚€寮崶鈺傚枑闁绘鐗嗙粭鎺旂磼閻欏懐鍒版い顏勫暟閳ь剨缍嗘禍婊堟儊閵娿儍褰掓偂鎼粹檧鍋撻崸妤€钃熼柨鐔哄Т缁€瀣⒒閸喓銆掑ù鐘櫊濮婅櫣鎷犻垾宕囦户缂備浇鍩栧畝鎼佺嵁閸℃稑鐐婃い鎺嶇娴滄粓姊虹紒妯虹仸妞ゎ厼娲鎶芥偐缂佹ǚ鎷洪梺鍛婄☉閿曘倝鎮樻潏銊х闁告粌鍟扮粔顕€鏌ｅ☉鍗炴珝鐎规洖鐖兼俊姝岊槷闁哄鐗犲娲川婵犲嫮鐣甸柣搴㈠嚬閸撴瑩鍩㈠鍛瀻闁规儳顕崣鍡椻攽閻愭潙鐏︽い顓炴喘閹偤鎮滈懞銉у帗閻熸粍绮撳畷婊冣枎閹邦噣妾梺鍝勫暙閸婅崵绮婚弬娆剧唵闁兼悂娼ф慨鍥煕濞嗗骏鏀婚柕鍥у瀵粙濡搁妸銉婵犵數鍋為崵顏堝炊閵娧冨箺婵＄偑鍊曠换鎰偓姘卞厴瀹曟洟寮崼鐔哄帗閻熸粍绮撳畷妤€顫滈埀顒€鐣烽幋锕€绠荤紓浣诡焽閸橀亶姊虹涵鍛涧缂佹煡绠栭、鏃堟偐缂佹鍘介梺鍝勭Р閸斿酣鎯岄幒鎴旀斀?
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
            // 闂傚倸鍊风粈渚€骞栭锕€纾婚柛鈩冪☉閸屻劑鏌ゅù瀣珕妞ゎ偅娲熼弻鐔衡偓鐢殿焾娴犙囨煃缂佹ɑ鈷掗柍褜鍓欑粻宥夊磿闁秴绠犻幖绮规濡插牏鎲搁弮鍫濊摕?闂傚倸鍊风粈渚€骞夐敓鐘插瀭闁汇垻顭堢粻瑙勩亜閹扳晛鍔樺ù婊嗩潐缁绘盯骞嬮悙鍐╁哺瀵鏁冮崒娑氬帗闂侀潧顦崝宀勬儍濞差亝鐓?闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟鐑樺灩閺嗗棝鏌熼梻瀵割槮鏉╂繈鏌熼崗鑲╂殬闁告柨鐬肩划濠氬Ψ閳哄倸鈧敻鏌ㄥ┑鍡涱€楅柛妯绘尦閺屽秷顧侀柛鎾翠亢閹筋偆绱撴担姝屽鐎规洦鍓熼崺鐐哄箣閿曗偓閻愬﹪鏌曟繝蹇曠暠妞ゅ繋鍗冲?DoH/nginx 闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墛閺咁亝绻濋悽闈浶涢柛瀣尰閵囧嫯绠涢幘璺侯杸闂佸搫鎷嬮崜鐔煎箖濮椻偓閹瑩鎳滃▓鎸庮棄闂備胶绮敮鎺楀Χ閹间礁绠栭悷娆忓婵挳鏌涘▎蹇ｆЧ闁绘繃鐗犲鐑樻姜娴煎瓨顎栭梺绋匡攻缁诲牓鎮伴閿亾閿濆骸鏋熼柟灞傚劦閺屾稓浠﹂崜褎鍣梻鍌氼槸缁夌數鎹㈠☉銏犵闁绘劖娼欓弳妤呮⒑缁嬭法鏄傞柛濠冪箓椤曪絾绻濆顒傚€為梺鍐叉惈閸熶即鏁嶉悙鐑樷拺闁荤喐澹嗛幗鐘绘煟閻旀潙鐏茬€?
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


// 婵犵數濮烽。钘壩ｉ崨鏉戠；闁逞屽墴閺屾稓鈧綆鍋呭畷宀勬煛瀹€瀣？濞寸媴濡囬幏鐘诲箵閹烘埈娼ュ┑鐘垫暩閸嬬偠銇愰崘顭戠劷闁炽儲鍓氶崵鏇熴亜閹烘垵鈧綊寮冲鍕╀簻闁哄倸鐏濋埛鏃堟煟濞戞牕鍔﹂柡?sniroute.cmd update check闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈剧畱鐎氬銇勯幒宥嗩樂缂佽妫濋弻娑樷槈濮楀牊鏁剧紓鍌氱Т濞差參寮婚悢鐓庣畾鐟滃秹寮虫潏銊﹀弿闁挎繂鎷嬪Ο鈧梺璇″枛缂嶅﹪鐛崶顒夋晣闁绘棁娅ｉ埢蹇涙⒒娴ｈ櫣銆婇柡鍛洴閹崇喖顢涢悜鍥╃◤濠电娀娼ч鍛尵瀹ュ鐓冪憸婊堝礈閻旈鏆︾€瑰嫭鍣村ú顏嶆晜闁告侗鍨扮敮鎾绘⒑鐠囪尙绠抽柛瀣枛瀹曟洟鎮界粙璺紱?
// 闂傚倸鍊搁崐鎼佸磹缁嬫５娲偐鐠囪尙锛涢梺鐟板⒔缁垶宕戦敓鐘崇厸闁搞儯鍔庨。鑼磼娴ｅ搫顣奸柕鍥у瀵噣鍩€椤掑嫭鍋嬮柣妯烘▕濞?GitHub API闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈鹃檮閸嬪鐓崶銊ュ枙缂傚秵鐗犻弻娑㈠焺閸愵亖妲堥柣搴㈣壘椤︻垶鈥︾捄銊﹀磯濞撴凹鍨辨婵犵數鍋涢ˇ顓㈠垂娴犲宓侀柡宥庡亐閸嬫挸鈽夊▍铏灥閻ｇ兘宕熼鍙ョ盎闂佸啿鎼鍛淬€呴鍕厵鐎瑰嫰鍋婇崕鏂库攽閳ュ磭鍩ｇ€规洖銈告慨鈧柍钘夋缂嶅矂姊婚崒娆戝妽閻庣瑳鍛煓闁规灞傚姂閸┾偓妞ゆ帒鍊荤壕濂稿级閸稑濡跨紒鐘靛仦椤ㄣ儵鎮欓懠顑囨煕閳规儳浜炬俊鐐€栫敮濠囨嚄閸洏鈧懘鎮滈懞銉у帾婵犮垼鍩栭惄顖氼瀶椤曗偓閺屸剝寰勬繝鍕ㄩ梺鍝勬湰缁嬫垿锝炲┑瀣垫晢闁告洜顣介崑鎾澄旈崘鈺佹瀾闂佸搫顦伴娆撳吹閻斿吋鐓熼柕澶樺枙缁ㄤ粙鏌熼悷鏉款伃濠碘剝鎮傛慨鈧柨娑樺楠炴﹢姊婚崒娆戝妽閻庣瑳鍛煓闁瑰鍋熼梽鍕煛閸愩劌鈧崵绮婚弬娆剧唵闁兼悂娼ф慨鍫ユ煕閵堝懎顏柡宀嬬畱铻ｅ〒姘煎灡绗戞俊銈囧Х閸嬫垿宕硅ぐ鎺撶畳闂備焦瀵х换鍌炲箠閹邦喚涓嶉柟缁樺俯閻斿棛鎲稿鍫濈煑闁告劦鍠栫粻姘舵煕閺囥劌鈧娀鎮㈤崫鍕嚙闂佸搫娲ㄩ崑妯荤珶?
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
        // 婵犵數濮甸鏍窗濡ゅ嫭鎳屾繝娈垮枛閿曘倗绱炴繝鍌滄殾鐎瑰嫭鍣村ú顏嶆晜闁告洦鍘奸幗瀣⒒娴ｅ憡鍟為柣鐕傜秮瀹曟洟骞庨懞銉ヤ画濠电娀娼уΛ灞轿ｉ崼銉︾厵闁绘劦鍓欐晶顖炴煕鎼达紕绠查柕鍥у閺佸啯鎷呴崷顓溾偓濠囨⒑鐠団€虫灍婵犮垺锕㈤幃鎯р攽閸艾浜鹃柨婵嗛婢т即鏌ｅ☉鏍у姦婵﹥妞藉畷銊︾節閸曨剙娅у┑鐐茬摠缁秹鎮疯楠炴垿濮€閻欌偓濞尖晠鏌ら幖浣规锭濞寸姷鍘ч埞鎴︽倷閸欏妫炵紓浣虹帛閸旀瑨鐭鹃梺鍛婁緱閸樼危閸儲鐓欓柟顖嗗啳鍩為梺閫炲苯澧柣鈺婂灦婵″瓨绗熼埀顒勭嵁鐎ｎ喗鏅濋柍褜鍓涢埀顒佽壘椤︻垶鈥︾捄銊﹀磯濞撴凹鍨伴崜鍗炩攽閻愬弶鍣圭紒缁橈耿瀵鈽夐姀鐘电杸濡炪倖甯婄粈浣糕枍閿濆拋娓婚柕鍫濇閳锋劙鏌涢弮鈧悷锔界┍?
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


// ---------------------------------------------------------------- GHA 婵炴垶鎸搁鍫澝归崶鈹惧亾閻㈡鏀伴柣锕佹椤?
// 闂佸搫顑勯懗鍫曟偩濠靛洦缍囬柟鎯у暱瀵櫕淇婂Δ鈧Λ瀵告閹殿喚纾介柡宥庡墰鐢棙淇?+ 缂佺虎鍙庨崰鏍偩?闂佸憡鐟﹂悧妤冪矓閻戣姤鏅€光偓閳ь剟宕欓懞銉ь浄闁挎繂娲ㄩ崹鎶芥煕閹烘挾鈽夌紓鍌涙崌閺佸秹宕煎鍛喒闂傚倸娲犻崑鎾绘偡?.rc 闁荤姍鍐仾缂侇煈鍣ｉ弫宥嗗緞閸艾浜?
// Enter = 缂佺虎鍙庨崰鏍偩妤ｅ啯鏅悗娑樼倰c = 闂佸憡鐟﹂悧妤冪矓閻戣棄违?
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

// 闂佸憡鑹炬姝屻亹鐎靛憡宕?gha fetch闂佹寧绋戦張顒佷繆閹间礁瀚夐柍褜鍓熷畷銉︽償濠靛牜浼囬柣鐐寸☉閻胶娆㈤銏犵濡わ箑鐏濋崹閬嶆煕閹烘柨顣奸柣銊╂涧閳绘梹骞婇柍?
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
    // WARP 闂傚倸鍊烽懗鍫曗€﹂崼銉晞闁告劦鍠栫壕鍧楁⒑椤掆偓缁夊绮婚弽顐熷亾楠炲灝鍔氶柟鍙壝埥澶愬閻橀潧濮︽俊鐐€栫敮鎺斺偓姘煎墴閹锋垿鎮㈤崗鑲╁帾婵犮垼鍩栭敋闁诲骏绲跨槐?0 缂傚倸鍊搁崐椋庣矆娓氣偓钘濋柟娈垮枟閺嗘粌鈹戦悩鎻掝伀闁告宀搁弻鈩冨緞鐎ｎ亞浜堕梺鍝勵儏闁帮綁寮诲澶嬪癄濠㈣泛顑呭▍褏绱撴担鎻掍壕婵犮垼娉涢…顒勬偄閸℃稒鐓欐い鏍ㄧ☉椤ュ绱掗妸銉﹀仴闁诡喖鍢查埢搴♀枎閹炬惌鈧秶绱撴担鍓插剰妞わ箑鐏氭穱濠囧醇閺囩偛鑰块梺瀹犳〃濡炴帗娼诲鈧缁樻媴閸濄儳楔濠电偘鍖犻崨顖滅厠閻熸粎澧楃敮鎺楁倿閸偁浜滈柟鍝勭Ф閸斿秹鏌ｉ妸锕€鐏撮柡宀嬬秮楠炴瑩宕熼鐘垫濠?
    // 闂傚倸鍊风粈渚€骞夐敓鐘茬鐟滅増甯掗崹鍌炴煟濡も偓閻楀﹪宕ｈ箛娑欑厓闁告繂瀚崳鍦磼椤愩垻效闁诡喖缍婂畷顐﹀礋椤掍礁鍓甸梺姹囧焺閸剟宕曢悽绋胯摕闁绘梻鍎ゅ畷澶愭煟濮橆厽缍戝ù鍏煎姉缁辨捇宕掑姣欍垺銇勯幋婵愭Ц闁伙絽鍢查…銊╁礃閹勫€梻浣规偠閸庢椽鎮樺┑瀣櫇闁稿本绋撻崢闈涱渻閵堝棗鐏﹂悗绗涘懐鐭堥柣妤€鐗勬禍婊堟煛閸屾稑顕滃璺哄缁辨帞鈧綆浜跺Σ娲煃瑜滈崜銊х礊閸℃稑纾婚柛娑樼摠閳锋梻鈧箍鍎遍ˇ浼存偂閵夛妇绠鹃柟瀵稿仧閹冲棗顭跨憴鍕惰€块柡宀€鍠栭、娑樷枎閹寸姷宕查柣?闂傚倸鍊风粈渚€骞夐敓鐘茬鐟滅増甯掗崹鍌炴煟濡も偓閻楀﹪宕ｈ箛娑欑厓闁告繂瀚崳鍦磼椤愩垻效闁诡喖缍婂畷顐﹀礋椤掍礁鍓甸梻浣规偠閸婃牜鏁垾鎰佸殨闁割偅娲栭悡娑㈡煕閹扳晛濡烘繛宀婁邯閺岋綁濮€閳轰胶浠梺瑙勭摃濞呮洘绌辨繝鍕ㄥ亾濞戞瑯鐒介柣鐔风秺閺屽秷顧侀柛鎾寸懇閸┿垽骞樼拠宸綂闂侀潧鐗嗛幊鎾诡杺闂備胶鎳撻崥瀣偩椤忓牆鍨傛繛宸簼閸婂潡鏌ｉ敐鍛槵闁稿鎸搁埢鎾诲垂椤旂晫浜愰梻?闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墛閸嬶紕鎲搁弮鍫熸櫜闁绘劕鎼粻鎶芥煙閻愵剚缍戞い顐熲偓鎰佹富闁靛牆妫楃粭鍌滅磼閳ь剚鎷呯憴鍕妳婵犵數濮存导鍛般亹閹烘垹顦ч梻鍌氬€荤换婵堚偓娑掓櫅椤啴濡舵惔鈥插缂備胶濮甸崹鍧楀灳閿旈敮鍋撻崷顓炐ラ柣銈傚亾闂備礁鎼崐钘夆枖閺囥埄鏁?
    // 闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈鹃檮閸嬪鐓崶銊ょ繁闁轰礁瀚伴弻娑㈠Ψ閿濆懎顬嬮梺宕囩帛濡啴寮诲鍫闂佸憡鎸婚悷鈺呭春閵忊剝鍎熼柕濞垮劤閸旀挳姊洪崨濠傚闁稿鎹囬崺娑㈠即閵忊檧鎷洪梺鍛婄箓鐎氼垶锝為敃鍌涚厱閹肩补鈧櫕姣愰梺鎸庢磸閸ㄨ棄鐣烽妸鈺佺骇闁瑰濯Σ鎾⒒娴ｇ瓔娼愭い鏃€鐗犲畷鏉课旈崨顔间患闂備緡鍓欑粔鐢告偂濞戙垺鐓曢柡鍥ュ灪濞懷囨煟韫囥儳绡€闁哄矉绱曟禒锕傛嚍閵夛附鍊锋俊鐐€栧ú鈺冪礊娓氣偓瀹曟椽鍩€椤掍降浜滈柟鐑樺灥椤忣亪鏌￠崪浣稿闁逞屽墮閸樻粓宕戦幘缁樼厱闁规澘澧庨崚鏉棵瑰鍫㈢暫闁诡喗锕㈤幃娆戞崉鏉炵増鐫忛梻浣藉吹閸犳劙宕濋弴銏犵叀濠㈣埖鍔曠粻缁樸亜閺嶃劎鐓瑙勬礃缁绘繈鎮介棃娑楃捕濠碘槅鍋勯顓炍ｉ幇鏉垮嵆闁绘梻顭堝鍧楁⒑瑜版帗锛熺紒鈧担铏逛笉闁瑰瓨绶疯ぐ鎺撴櫜闁搞儯鍔嶉悵顏嗙磽娴ｅ搫啸闁稿繑锕㈠濠氭晸閻樿尙顦伴梻鍌氱墛缁嬫挻绂掗鐐╂斀闁绘﹩鍋勬禍鎯ь渻閵堝棙灏柛銊︽そ瀹曟洖螖閸涱喚鍘繝銏ｅ煐閿氶柣蹇ョ悼缁辨帡鍩€椤掍礁绶為柟閭﹀幘閸橀亶姊洪悷鐗堟儓妞ゆ垵鎳橀崺鈧い鎺嶇劍缁€瀣煙椤曗偓缁犳牠骞冨▎鎿冩晢闁稿本顨嗛悘鍐煟鎼达紕鐣柛搴ㄤ憾楠炲﹥鎯旈埍?濠电姷鏁搁崑娑㈩敋椤撶喐鍙忛柡澶嬪焾濞尖晠鏌ㄩ弮鍌氫壕鐎规洖寮剁换娑㈠箣濞嗗繒浠肩紓浣哄У婵炲﹪寮婚弴鐔风窞闁割偅绻傛慨搴ㄦ⒑缂佹ɑ灏柛濠傛健瀵鏁撻悩鑼紲闂侀潧鐗嗛幊搴ｇ不閸︻厾纾?闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟娆¤娲、姗€濮€閻橀潧濮?
    // 婵犵數濮甸鏍闯椤栨粌绶ら柣锝呮湰瀹曟煡鏌熸潏鎯х槣闁轰礁鍟埞鎴﹀磼濠婂海鍔搁柣鐔哥懕闂勫嫰濡甸崟顖氬唨闁靛鍔岄ˉ婵嬫偠濮樺崬鏋涙慨濠勭帛缁楃喖宕惰閻忓牓姊洪柅鐐茶嫰婢т即鏌涢悢宄邦劉鐎垫澘瀚禒锕傚箚瑜忛敍蹇涙⒒娴ｇ儤鍤€闁宦板妿閸掓帗鎯旈…鎴炴櫆闁哄鐗勯崝搴ｇ不?up闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟瀵稿仧闂勫嫰鏌￠崘銊モ偓鍝ユ?*闂備浇顕уù鐑藉磻閿濆纾归柡宥庡幖绾惧潡鏌ゅù瀣珕鐎?12 缂?*闂?
    // first-run-setup 闂傚倸鍊烽懗鍫曞储瑜旈妴鍐╂償閵忋埄娲稿┑鐘诧工閹冲繐鐣烽幓鎹楀綊鏁愰崨顔跨闂佸磭绮ú鐔煎蓟閿濆妫橀柛顭戝櫘閺嗐垻绱撴担鐟板妞ゃ劌锕濠氭偄妞嬪孩娈濋梻渚囧弿闂勫嫭鐡忛梻鍌欑濠€鍗炩枍閵忋垻绀婂ù锝呭閸ゆ洟姊婚崼鐔剁繁闁告艾顑夐弻娑樷槈閸楃偛瀛ｅ┑鐐插悑婵炲﹤顫忓ú顏勭閹艰揪绲哄Σ鍫ユ⒑濞茶澧柕鍫熸倐楠炲啴妫冨☉鎺擃潔闂侀潧绻掓慨鐑剿囬锔界厽闁绘ê寮舵径鍕喐閺夊灝鏆ｇ€殿喗褰冮埢搴ㄥ箻閺夋垳鍖?up 濠电姷鏁搁崑鐐差焽濞嗘挸瑙﹂悗锝庡枟閸ゅ苯螖閿濆懎鏆欑紒鐙€鍨堕弻鐔封枔閸喗鐏€闂佺顑嗛幑鍥箖閻戣棄绾ч柟绋垮椤斿鈹戦悩鎰佸晱闁哥姵纰嶇换娑㈠焵椤掑嫭鐓涘ù锝呮憸鏁堥悗瑙勬礈閸樠囧煘閹达箑閱囬柣鏂挎憸闁芥帒鈹戦悩鍨毄闁稿鍋涢…鍥р枎閹垮啯鏅為梺鍛婄⊕濞兼瑧绮婚弽銊ょ箚闁靛牆鎳庨弳鐔兼煕濡や胶顣查柕鍥у瀵粙濡歌椤撶厧鈹戦悙璺侯棈婵炲鐩﹢浣糕攽閻樿宸ラ悗姘煎墰缁叀顦崇紒?
    // 闂傚倸鍊烽懗鍫曗€﹂崼銉晞闁糕剝鐟ラ崹婵堚偓骞垮劚濞诧箓寮冲鍫熺厓鐟滄粓宕滈悢鐓庤摕闁靛鍎Σ鍫熶繆椤栨繂浜瑰ù鐓庢川缁辨挻绗熼崶褎鐏侀梺鍛婃⒐閸ㄧ敻顢?5454 婵犵數濮烽弫鎼佸磻濞戞瑥绶為柛銉墮閻掑灚銇勯幒鍡椾壕閻庢鍠栭悥濂稿箚閸ャ劎顩烽悗锝庡亞閸樺崬鈹戦绛嬬劷闁告鍥у偍闁汇垹鎲￠悡?444 闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墕閽冪喖鏌曟繛鍨壄?502闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟娆¤娲、姗€濮€閻橀潧濮︽俊鐐€栧濠氬磻閹剧粯鐓曞┑鐘插亞閻撳ジ鏌熼鑺ャ仢妤犵偛顑夐幃鈺咁敃椤厼顥氶梻浣藉吹閸犳劕顭垮鈧鎶芥偄閾忓湱锛滃銈嗘煥閹碱偅鎱ㄩ崒婧惧亾濞堝灝鏋涢柣鏍с偢楠炲啴濮€閵堝棛鍙嗛柣搴秵娴滄牠宕戦幘缁樺仺闁告稑锕﹂崢閬嶆⒑瑜版帒浜伴柛鎾寸☉閺嗏晠姊洪懞銉劷闁哥姴閰ｉ崺銉﹀緞閹邦剛顔掑銈嗙墬閻喗绔熼弴鐔虹瘈闁汇垽娼ф禒婊勪繆椤愩垻鐒搁柛鈹惧亾濡炪倖甯掗崯顖炴偟椤忓牊鐓熼柕濠忕畱閻忊晝绱掓潏銊ョ闁逞屽墾缂嶅棙绂嶅鍫濇辈闂侇剙绉甸悡鐔兼煥濠靛棙鎼愬ù婊冨⒔缁辨帡宕掑姣櫻呪偓瑙勬礋娴滃爼鐛崶顒€惟闁靛鍊曢ˉ?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 濠电姵顔栭崰妤冩暜濡ゅ啰鐭欓柟鐑樸仜閳ь剨绠撳畷濂稿Ψ椤旇姤娅嶅┑鐘垫暩婵敻鎳濋崜褏鐭嗛柛宀€鍋為悡娆撴煙椤栨稒绶茬悮姘舵⒑閹稿海鈯曟俊顐㈠暣瀵濡搁妷銏☆潔濠电偛妫欓崝妤€鈻撻妶澶嬧拺閺夌偘鍗冲鐑芥煕婵犲啰绠為柣娑卞枤閳ь剨缍嗛崰妤呮偂閵夆晜鍊甸柨婵嗛婢ь垶鏌嶉柨瀣缂佺粯鐩獮瀣倷閺夋垹锛夋俊鐐€栧▔锕傚礋椤掆偓瀵潡姊洪崘鍙夋儓闁瑰啿娴烽幉鎾晜婵劒绨婚梺鍝勭Р閸斿酣鎯岀€ｎ喗鐓涢柛鈩冾殘缁犺崵鈧鍠栭悥濂稿春閳╁啯濯撮悷娆忓閸婇攱绻濆▓鍨灍閼垦囨煕閺冣偓閸ㄥ潡鎮伴鑺ュ劅闁靛繈鍨婚惁鍫ユ⒑閸涘﹣绶遍柛銊﹀▕椤㈡瑩寮撮姀鈾€鎷哄┑鐐跺蔼椤曆勭閹扮増鐓曢悗锝庡亝瀹曞矂鏌?WARP 闂傚倸鍊风粈渚€骞栭锕€纾婚柛鈩冪☉閸屻劑鏌ｉ敐鍛拱闁哄棙绮撻弻宥堫檨闁告挾鍠栧濠氬Ω閵夈垺顫嶅┑鈽嗗灠閻忔繈顢旈銏♀拺闁告縿鍎遍弳閬嶆煕鐎ｎ偅宕岄柡宀嬬秮閹晜娼忛埡浣割瀴濠电姰鍨婚幊鎾澄涘▎鎾崇疄闁靛ň鏅涚粻鐟懊归敐鍛喐闁伙綀椴哥换娑欐綇閸撗勫仹濡炪倧绲肩划娆忕暦?
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
