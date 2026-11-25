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

// WARP 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢妶鍌氫壕婵ê宕崢瀵糕偓瑙勬礉椤鈧潧銈稿鍫曞箣閻樺灚姣庢繝鐢靛仩閹活亞寰婇懞銉х彾濠电姴娲ら悿顕€鏌涘☉妯兼憼闁绘挾濞€閺岋綁骞橀幎绛嬧偓妤併亜閿濆懐锛嶇紒杈ㄥ浮閹晠鎳￠妶鍥ㄦ瘒闂備礁鎼惉濂稿窗閺嶎厾宓佸〒姘ｅ亾妤犵偞甯￠獮瀣攽閸♀晜缍戦梻鍌氬€风欢姘跺焵椤掑倸浠滈柤娲诲灡閺呭爼顢氶埀顒勫蓟濞戞瑧绡€闁告洦鍓氶悵顏堟⒑閻熸壆鐣柛銊ょ矙閻涱喚鈧綆鍠楅弲婵嬫煃瑜滈崜鐔煎箖?0=闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敂钘変罕濠电姴锕ょ€氼噣銆呴崣澶岀瘈濠电姴鍊绘晶娑㈡煕鐎Ｑ勬珚闁哄本娲樼换娑滎槻閻庢凹鍓熷?1=闂傚倷娴囬褍顫濋敃鍌︾稏濠㈣埖鍔栭崑銈夋煛閸モ晛小闁绘帒锕ラ妵鍕疀閹炬惌妫￠悗瑙勬礀瀵墎鎹㈠☉銏犵婵炲棗绻掓禒鑲╃磽?2=闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敂钘変罕濠电姴锕ょ€氼噣銆呴弻銉︾叆婵犻潧妫欐径鍕偓瑙勬礀瀵墎鎹㈠☉銏犵婵炲棗绻掓禒鑲╃磽娴ｅ搫校濠电偛锕濠氬即閻旈绐炲┑鐐村灦閼归箖鎯屽┑瀣拺?
// 闂?warp-cli status 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁嶉崟顒佹闂佹悶鍎洪崜娆戝瑜版帗鐓涢柛銉ｅ劚閻忣亪鏌涚€Ｑ勬珔閾绘牠鏌ㄥ┑鍡樺櫣闁哄棛鍋ら弻锟犲椽娴ｇ顫梺閫炲苯澧柛妯荤矒瀹曟垿骞樼紒妯煎幈闂佽婢樻晶搴ㄥ礆閺夋５鐟邦煥閸垻鏆梺璇″枟閿曘垽骞冨▎鎴炲磯閺夌偟澧楅惈蹇涙⒒娴ｈ棄鍚归柛鐘冲姉閹广垽宕奸妷銉ㄦ憰闂佽法鍠撴慨瀵哥矆鐎ｎ喗鈷掗柛顐ゅ枔閵嗘帒霉閻欌偓閸ㄤ即鍩為幋锔藉亹妞ゆ劦婢€婢规洟姊绘担鍛婂暈濞撴碍顨婂畷鐟扮暦閸パ屾婵°倧绲介崯顖炴偂濞嗘挻鍊垫繛鎴炵懐閻掔晫绱掗悩铏鞍妞ゃ劊鍎甸幃娆擃敂閸曨厺绱欓梻浣告惈鐞氼偊宕濆畝鍕叀濠㈣泛艌閺嬪酣鏌熺€电浠﹂柛搴㈡SQUE 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩顔瑰亾閸愵喖宸濇い鏍ㄧ☉鎼村﹤鈹戞幊閸婃洟骞忕€ｎ喖鏋佸┑鐘叉处閻撴洘绻涢幋婵嗚埞闁哄鍊濋弻宥囩磼濡椿妫冮梺璇″枟椤ㄥ﹪寮幇顓熷劅闁炽儲鍓氬鍓х磽閸屾瑧顦︽い锔诲灣缁辩偞绻濋崶褑鎽曞┑鐐村灦閿曗晛銆掓繝姘厽闁瑰瓨姊瑰▍鍛瑰鍫㈢暫闁哄睙鍡欑杸闁挎繂鎳嶉崙浠嬫⒑閸涘﹤鍤柛瀣噽閹广垹鈽夊▎鎰Ф闂佸憡鎸嗛崘銊у帓濠电姷顣藉Σ鍛村磻閸涘瓨鍋＄憸蹇涘礆閹烘挾绡€闁搞儯鍔嶅▍鍥⒑缁嬫寧婀扮紒瀣崌瀹曘垽骞掑Δ渚囨濡炪倖鍔戦崹鐑樺緞閸曨垱鐓欑紒瀣閹叉悂鏌熷畡閭﹀剶鐎殿喖顭锋俊鐑芥晜閹冪闂傚倷鐒﹂崕鍐裁瑰璺虹；闁告洦鍨遍崐璺衡攽閻樻彃顏痪鎯у悑娣囧﹪濡堕崒姘婵＄偑鍊ф俊鍥极鐠囧樊鍤曞┑鐘宠壘瀹告繃銇勯幘妤€瀚崗濠囨⒒娴ｅ憡鍟為柟绋挎瀹曨亝鎷呴崜?
// 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐蹭画濡炪倖鐗楃粙鎾汇€呴崣澶岀瘈闂傚牊绋掗敍鏃堟煠?status 闂?Connected 闂傚倸鍊搁崐椋庣矆娴ｈ櫣绀婂┑鐘插亞閻掔晫鎲歌箛鏇炲灊闁哄啫鐗嗙粻顕€鏌﹀Ο渚Ъ闁糕晝鍋涢—鍐Χ閸℃鐟ㄩ梺绋匡工缂嶅﹤鐣烽幋婵愭建闁逞屽墴瀵寮撮悢铏诡啎闂佸壊鐓堥崰鏍ㄦ叏閸洘鈷戦柛娑橈龚婢规﹢鏌涙繝鍌涘仴鐎殿喖顭烽幃銏ゆ偂鎼达綆鍟嬫俊鐐€栧ú鏍箠韫囨蛋澶愬冀瑜滃〒濠氭煏閸繃鍣虹紒鍌氼儔閺屾盯骞樼€靛憡鍒涘Δ鐘靛仜缁绘帡鍩€椤掑﹦绉甸柛鐘崇墪濞插潡姊绘担鍛婂暈婵炶绠撳畷鏉款潩缁旑垰娲︾€佃偐鈧稒菤閹峰姊虹粙鎸庢拱缁炬澘绉瑰畷顖氼潩閼哥數鍘遍梺闈涱樈閸ㄦ娊鎮炬潏銊ｄ簻闁靛绠戦崫鐑樻叏婵犲啯銇濈€规洏鍔嶇换婵嬪礋椤掆偓濞呇囨⒒娴ｈ鍋犻柛鏂跨У缁绘稒绻濋崒銈呮濡炪倖鍔х粻鎴濇纯濠电姰鍨煎▔娑滅檶?
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

// 闂傚倸鍊搁崐鎼佸磹妞嬪孩顐介柨鐔哄Т绾惧鏌涘☉鍗炴灓闁崇懓绉归弻褑绠涘鍏肩秷閻?sniroute.cmd 闂傚倸鍊搁崐椋庣矆娴ｉ潻鑰块梺顒€绉甸崑锟犳煙閹増顥夋鐐灲閺屽秹宕崟顐熷亾瑜版帒绾?warp on/off闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熺€电浠ч梻鍕閺岋繝宕橀妸銉㈠亾閼姐倗涓嶉柡鍌涱儥閻斿棝鎮规ウ鎸庮仩闁诲繐宕—鍐偡閺夋寧姣愬銈庡弨濞夋洟骞戦崟顖涘仏闁哄鍨甸～鐘电磽閸屾瑧鍔嶉柛鏂炲懏宕叉繝闈涱儏閽冪喖鏌曟繛鍨姉婵℃彃鐗婃穱濠囶敍濠靛棗鎯炴繛鏉戝悑娴滅€漰-cli 闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濋棃娑卞剰缁炬儳顭烽弻锝夊箛椤掑倷绮甸梺鍝勬缁捇寮婚悢铏圭＜闁靛繒濮甸悘鍫ユ⒑濞茶骞楁い銊ユ婵＄敻宕熼姘敤闂侀潧臎閸涱垱娈奸梻鍌欑閹猜ゆ懌闂佺懓鎲℃繛濠囩嵁閸愵喗鍊婚柦妯侯槸瀹撳棗鈹戦鏂よ€跨痪顓狀焾鍗辨い鏇楀亾婵﹥妞介獮鏍倷濞村浜鹃柡宥庡幖閸ㄥ倿鏌涢敂璇插箹闁搞劍绻堥弻锟犲礃閵娧冾暫缂備胶濮垫繛濠囧蓟閿濆绠涙い鏃囧Г濮ｅ嫬顪冮妶蹇撶槣闁稿锕ら～?
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


// 闂?winget 闂傚倸鍊峰ù鍥敋瑜嶉湁闁绘垼妫勭粻鐘绘煙閹冩闁搞儺鍓欑粻顕€鏌?Cloudflare WARP闂傚倸鍊搁崐椋庢濮橆剦鐒界憸宥堢亱濠德板€曢幊搴ｅ閻熸噴褰掓偐瀹割喖鍓遍梺鐟板级閿曘垽骞冨Δ鍛櫜閹肩补鈧啿绠ｉ梻浣侯焾椤戝棝骞愭ィ鍐ㄧ疅闁圭虎鍠栫粈瀣煕閳╁厾顏堟倶鐎电硶鍋撳▓鍨灈妞ゎ厾鍏橀獮鍐椤厾鍓ㄦ繛杈剧秬椤寮虫繝鍥ㄢ拻濞达絿鐡斿鎰節閵忊埄鎴﹀疾閼稿灚鍎熼柨婵嗘川閻掑吋绻濋姀锝嗙【閼瑰本淇婇幓鎺斿ⅵ闁哄瞼鍠栭獮鍡氼槾闁挎稑绉瑰畷顖炲箹娴ｅ厜鎷洪梺纭呭亹閸嬫盯鍩€椤掍胶澧遍柛鎺撳笒閻ｆ繈宕熼崹顐ｆ珦闂傚鍋勫ú锔剧矙閹寸姵鍏滈柍褜鍓熷铏圭磼濡搫顫戦柣蹇撶箲閻熲晠宕洪埀顒併亜閹哄棗浜惧銈庡幘閸忔ê顕ｇ拠娴嬫婵☆垶鏀遍悗濠氭⒑瑜版帒浜伴柛鎾寸⊕缁傛帡骞橀瑙ｆ嫼婵炴潙鍚嬮悷褏绮婇悜鑺ョ厱闁哄倸娼￠崣鍕殽閻愬樊妲搁摶鏍煕濞戝崬骞栫紒鎰☉椤啴濡堕崱妯烘殫闂佺顑囬崰搴ょ亽婵炴挻鍩冮崑鎾存叏?
// 婵犵數濮烽弫鎼佸磻閻愬樊鐒芥繛鍡樻尭鐟欙箓鎮楅敐搴℃灍闁搞倕鑻灃闁挎繂鎳庨弳鐐烘煕閵堝懏鎼愰柍瑙勫灴閹晠骞撻幒鎾搭啀闂備礁纾划顖毼涢崘顔艰摕鐎广儱鐗滃銊╂⒑閸涘﹥灏伴柣鐔叉櫊瀹?UAC闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熺€涙绠伴柤鎷屾硶缁辨帒螖閸愨晝褰匬 闂傚倸鍊峰ù鍥х暦閻㈢绐楅柟閭﹀劒濞差亜绠ｆ繝銏＄箓缂嶅﹤鐣烽崼鏇ㄦ晞闁挎洍鍋撴い锔藉閹广垹鈹戠€ｎ亞鍊為悷婊冪Ч閹潧顫滈埀顒勫箖濡ゅ懐宓侀柛顭戝枛婵酣姊洪悷鏉挎毐闂佸府缍侀弫鎰版倷閸濆嫀褍顭跨捄渚剳闁告ê宕埞鎴︽偐鐠囇冧紣闂佸摜鍠嶉崡鎶藉箖閻愬搫绠瑰ù锝呮贡閸樻捇姊洪懡銈呮瀾濠㈢懓妫欓弲銉╂煟鎼淬値娼愭繛鍙夘焽閹广垽宕奸妷銉т紜闂佹寧娲栭崐鍝ョ不閹惰姤鐓涢柛鎰╁妼娴滅偞淇婇崣澶嬪唉婵﹦绮幏鍛存偡闁箑娈濈紓鍌欑椤戝棝宕归崸妤€绠犻柣鏃傗拡閺佸秹鏌ｉ幇顓熺稇濞寸姴銈稿娲濞淬倖鐩畷鐔碱敃閿濆棛褰洪梻鍌氬€风粈渚€骞夐埄鍐懝婵°倕鎳庨崒銊ノ旈敐鍛殭婵懓寮堕幈銊ノ熼崹顔惧帿闂佺粯鎸诲ú鐔煎蓟閻斿吋鍊绘慨妤€妫欓悾鐑芥⒑鏉炴壆绐斿ù婊冪埣瀵鏁嶉崟顏呭媰闂佸搫鍊堕崐娑㈠触椤愶附鈷戦柛锔诲弾閸旀粎绱掗煫顓犵煓闁炽儻绠撴俊鎼佸煛娴ｄ警妲伴梻渚€娼ц噹閻忕偞瀚庤濮婂宕掑▎鎺戝帯缂備緡鍣崹鍫曞灳閿曞倸閱囬柕澶堝劜濡差剟姊洪崫鍕枆闁告ê銈搁幃鐐哄垂椤愮姳绨婚梺鐟扮摠缁诲啴宕戦妷锔跨箚闁告挆鍐ㄥ箣闂?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 婵犵數濮烽弫鎼佸磻濞戙垺鍋ら柕濞炬櫅閸氬綊骞栧ǎ顒€濡肩痪鎹愵潐娣囧﹪濡堕崨顓熸闂?CLI闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熼悜姗嗘畷闁搞倕鑻灃闁挎繂鎳庨弳鐐烘煕鐎Ｑ勬珚闁哄矉绲借灒闁告繂瀚В鎰渻閵堝棙绀夊瀛樻倐楠炲牓濡搁妷顔藉缓闂佺硶鍓濋〃鍛达綖閳哄懏鐓熼柕蹇婃櫅閻忔盯鏌涙惔锝嗘毈妤?winget闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏鍓х暠閻庢艾顦伴妵鍕箳閹存繍浠鹃梺绋块椤︾敻寮诲☉妯锋婵炲棙鍔楃粙鍥煟?winget 闂傚倸鍊峰ù鍥敋瑜忛幑銏ゅ箛椤旇棄搴婇梺褰掑亰閸剚绂嶉悷閭︾唵閻犻缚娅ｉ悘杈╃磼閻樿櫕顥堥柡灞剧☉閳藉宕￠悙瀵镐憾濠?Cloudflare 闂傚倸鍊峰ù鍥敋瑜嶉～婵嬫晝閸岋妇绋忔繝銏ｆ硾鐎涒晠骞婂畝鍕拻濞达絽鎲￠幆鍫ユ煟椤掆偓閵堢鐣锋导鏉戠閻犲洩灏欓崝锕€顪冮妶鍡楃瑨闁稿﹤缍婂畷鐢稿焵椤掆偓椤啴濡惰箛娑欘€嶆繝鐢靛仜閿曘劌螞閵忥紕绡€闁汇垽娼у瓭濠电偛鐪伴崐妤冨垝閸繄鏆嗛柛鏇ㄥ墰閸樺崬鈹戦悙鏉戠仴鐎规洦鍓氱粋宥夊箛閻楀牏鍘?
        // 婵犵數濮烽弫鎼佸磻閻愬搫鍨傞柛顐ｆ礀缁犱即鏌涘┑鍕姢闁活厽鎹囧濠氬醇閻斿嘲骞忛梺鎸庣箓濞层劑鎯屽▎鎾寸厱闁绘柨鍢查弳鐐烘偨椤栨粌鏋旂紒顔款嚙閳藉濮€閻樻鍟嬮梻浣筋潐閸庣厧螞閸曨喓浜?Windows 闂傚倸鍊搁崐鎼佸磹妞嬪孩顐介柨鐔哄Т缁愭绻涢幋鐐垫噭闁告瑥绻橀弻鏇㈠醇濠靛洤鐝旈梺?winget闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熺€涙绠伴柤鐗堝閵囧嫰濮€閵忋垺顎岰闂傚倸鍊搁崐椋庢濮橆剦鐒界憸宥堢亱闂佸搫鍟悧濠囧吹閸曨垱鐓曢柟鎯у级缁佹澘鈹戦钘夆枙闁诡喖缍婇獮渚€骞掗幋婵愮€峰┑鐐村灦閹稿摜绮旇ぐ鎺戣摕鐎广儱鐗滃銊╂⒑閸涘﹥灏甸柛鐘崇墵閻涱噣宕橀褎鈻岄柣搴ゎ潐濞叉粓宕㈣閸╃偤骞嬮敃鈧柋鍥ㄧ節闂堟稓澧曟鐐╁亾闂傚倸鍊峰ù鍥х暦閻㈢绐楅柟鎵閸嬶繝鏌曢崼婵囧妞ゆ劒绮欓弻鏇㈠醇濠靛洤顦╁銈庡亝濞叉﹢骞堥妸銉庣喖骞愭惔锝冣偓鎰板级?
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


// WARP 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧壕濠氭煕閳╁啰鈽夐柛灞诲姂閺屻倕霉鐎ｎ偅鐝曞銈庡亝濞茬喖寮婚悢鍛婄秶闁告挆鍛闂佽崵鍠愰悷杈╃不閹捐崵宓侀柡宥庣仈鎼搭煈鏁嗛柍褜鍓熼幃姗€鍩￠崨顓犵暰闂佺粯鏌ㄩ崥瀣煕閹达附鐓曟繝闈涙椤忣偄顭胯娴滃爼寮诲☉銏犵閻庨潧鎲￠崳褔姊?DNS 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧壕鐟懊归悩宸劀缂傚秵鐗曢…鍧楁嚋闂堟稑顫囬梺閫炲苯鍘哥紒鈧笟鈧、姗€宕楅悡搴ｇ獮婵犵數濮寸€氱兘宕棃娑掓斀闁绘﹩鍠栭悘杈ㄧ箾婢跺娲撮柡浣稿暣婵＄柉顦撮柣顓炴閺屽秷顧侀柛鎾跺枎椤繐煤椤忓嫪绱堕梺鍛婃礀閻忔俺鍊撮梻鍌欒兌椤牏鈧灚甯″畷褰掑醇閺囩偞妲梺?
//
// 婵犵數濮烽弫鎼佸磻閻愬搫鍨傞柛顐ｆ礀缁犳彃霉閿濆懎顥忔繛瀛樼墵閺屾洘绻涜閸燁偊鎮樻繝鍥ㄢ拺闁告挻褰冩禍鐐烘煕閻樿櫕宕岄柛鈹垮灲瀵挳濮€閿涘嫬骞堟俊鐐€栭崝妤佹叏閹绢喖绀夋繝濠傚幘瑜版帗鍋愮紒娑氼攰閳ь剙娼￠弻鐔碱敊閹冨箣閻庢鍠楀娆擄綖濠靛浂娼欓柡宥冨壉濠婂牊鈷掑ù锝夘棑娑撹尙绱掗崣澶婎嚋缂佺粯鐩崺鈧い鎺戝閻撴稑霉閿濆懏鎲哥€涙繂顪冮妶搴′簴闁搞劋绮欓悰顕€鍩€椤掆偓閳规垶寰勭€ｎ偒娼?闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢妶鍌氫壕婵ê宕崢瀵糕偓瑙勬礀缂嶅﹪寮婚崱妤婂悑闁告侗鍠涚欢銏ゆ⒒娴ｇ懓顕滅紒璇插€胯棟妞ゆ牗澹曢崑鎾愁潩椤掑效缂備胶绮粙鎴︻敊韫囨侗鏁婇柤濮愬€曢崵顒勬⒒娴ｉ涓茬紒鐘冲灴閹囧箻閹颁焦缍庡┑鐐叉▕娴滃爼寮崱妞曞綊鏁愰崼顐ｇ秷闂佺瀛╅幐鍓ф閹惧瓨濯村ù鐘差儏閹界數绱撴担绛嬪殭閻庢矮鍗冲畷娲焵椤掍降浜滈柟鐑樺灥閳ь剙缍婂畷鐢稿焵椤掑嫭鈷戦柛婵勫劚濞呭繘鏌涢妸锕€鈻曠€殿噮鍋婇、姘跺焵椤掑嫬鏄ラ柍褜鍓氶妵鍕箳閹存績鍋撻懡銈咁棜濠靛倸鎲￠悡鐔兼煙娴煎瓨娑у褜鍠楅妵鍕即閻旇櫣鐓佹繛锝呮搐閿曨亪骞冨▎鎾崇闁瑰搫妫楁禍楣冩煕瑜庨〃鍛不閺嶎灐褰掓偐瀹割喖鍓伴柟顖滃枛濮婃椽骞愭惔銏紩闂佺顑嗛幑鍥蓟閿濆绠婚悗娑欘焽椤︻參姊洪柅鐐茶嫰婢ь噣鏌ｈ箛鏃傤暡缂佹梻鍠撻埀顒婄秵閸犳牜澹曟繝姘厵闁诡垎鍛偗闂佺顑嗛幑鍥х暦瑜版帩鏁冮柕蹇婃櫆椤ユ捇姊绘担渚劸妞ゆ垵妫濋獮鎰板箹娴ｅ湱鐣抽梻鍌欒兌缁垶鏁冮埡鍛獥闁哄稁鍘肩粈澶嬬箾閸℃ɑ灏伴柛?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸閻ゎ喗銇勯弽顐粶闁搞劌鍊块弻娑㈩敃閿濆棛顦ラ梺钘夊暟閸犳牠寮诲☉銏犖ㄩ柨婵嗘噹椤顪?闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏楣冩闁稿孩顨呴妴鎺戭潩閿濆懍澹曢梻浣虹《閺備線宕戦幘鎰佹富闁靛牆妫楅崸濠囨煕鐎ｎ偅宕岄柡灞诲€曠叅閻犲洩灏欐禒鎼佹⒑鐠団€虫灈婵炲皷鈧磭鏆﹂柣鏃傗拡閺佸秵绻涢幋鐐茬瑲閻庡灚鐗犲缁樻媴閸涘﹤鏆堟繛鎾寸椤ㄥ﹤鐣峰鍐ｆ闁靛繆鈧厖鐢婚梻浣筋潐婢瑰棙鏅跺Δ鍛亗婵炴垯鍨洪悡鏇熴亜閹伴潧浜滃ù婊勭矊椤儻顧侀柛鐘愁殘閹广垹鈹戠€ｎ亞鍔?
// DoH/nginx 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐蹭画濡炪倖鐗滈崑娑㈠垂閸岀偞鐓熼柕蹇嬪焺閻掑墽绱掗埀顒勫磼濞戞瑥寮垮┑锛勫仩椤曆勭妤ｅ啯鍊甸悷娆忓缁€鈧梺缁樼墪閵堟悂鐛崘顔肩厸闁稿本绋撶粔璺侯渻閵堝棗濮傞柛濞垮€曞嵄鐟滅増甯楅埛鎺懨归敐鍛暈闁哥喓鍋為妵鍕Ω閵夛箑娅ｉ柣鎾卞€濋弻鏇熺節韫囨挾妲ｅ┑鐐茬墔缁瑩寮诲☉銏犖ㄦい鏃傚帶椤晠寮堕埡鍌滅畺缂佺粯绋掑蹇涘礈瑜嶉崺宀勬⒑绾懎袚缂侇喗鎹囧畷娲倷濞村顫嶅┑鐐叉閸ㄥ綊鎯侀崼銉﹀仭婵犲﹤鍟版晥閻庤娲﹂崹鑸典繆閹间礁鐓涘ù锝呮惈婵附淇婇悙顏勨偓鏍垂閻撳簶鏋栭柡鍥ュ灩缁€澶屸偓骞垮劚椤︿即鎮￠弴銏＄厪濠电姴绻掗崚鏉库攽閿涘嫭娅曠紒杈ㄥ浮瀹曟帒鈽夊Ο鑲╂嚃闂備浇顕栭崹鍗炍涢崘鈺傚弿闁逞屽墴閺屽秹濡烽妷褝绱炴繝銏ｆ硾鐎氭澘顫忕紒妯诲闁告稑锕ら弳鍫ユ⒑閸涘﹤鐏ョ紓宥咃工閻ｇ柉銇愰幒鎾充罕闂佸壊鍋嗛崰搴☆嚕閸ф鈷戦柛鎰级閹牓鏌熼崘鑼鐎规洜鏁婚崺鈧い鎺戝閸婄敻鏌涜箛鎿冩Ц濞存粓绠栧娲焻閻愯尪瀚板褜鍠楅妵鍕棘鐠恒劎顔夊┑鐐靛帶缁绘ɑ淇婇幖浣肝ㄧ憸蹇涙偘椤斿皷鏀介柣鎰摠鐏忎即鏌ら悷鏉库挃闁兼椽浜堕弻鍡楊吋閸涱垽绱遍梻浣筋潐瀹曟﹢顢氳婢规洟宕楅懖鈺冪槇闂傚倸鐗婄粙鎾存櫠閵徛颁簻闁哄啠鍋撻柛鏃€鐟ラ～蹇曠磼濡顎撻梺鑽ゅ枑婢瑰棛绮婇悜鑺モ拺闁稿繐鍚嬮妵鐔兼煕閵娿劋鍚紒顔碱儔楠炴帡骞嬮弮鈧～宥呪攽閻愬弶顥為柛銊у枛瀵悂寮介鐔叉嫼闂傚倸鐗婄粙鎾存櫠閺囥垺鍊垫慨妯煎帶瀵喗銇勯姀鈩冪闁圭锕ョ粩鐔封攽閸喎娈屽銈庡幑閸旀垵鐣峰鈧獮鍡氼槻濠碘姍鍛＝?
//   1) 濠电姷鏁告慨鐑姐€傞挊澹╋綁宕ㄩ弶鎴狅紱闂侀€炲苯澧撮柡灞剧〒閳ь剨缍嗛崑鍛暦瀹€鍕厸?5444(DoH 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鑼唶闂佸憡绺块崕鎶芥儗閹剧粯鐓曟い鎰剁稻缁€鍐煕? / 5454(DoH 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐叉疄闂佽鍨奸悘鎰亹閹烘挸鈧崵绱掑☉姗嗗剱闁? / 443(nginx) 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢妶鍌氫壕婵ê宕崢瀵糕偓瑙勬礉椤鈧潧銈稿鍫曞箣閻樺灚姣庢繝鐢靛仦閹稿宕洪崘顔肩；闁瑰墽绮悡娑㈡倶閻愬灚娅曞褝绠撻弻鐔虹矙閸栤剝鏁鹃悗鍨緲鐎氼噣鍩€椤掑﹦绉甸柛妯恒偢楠炲棝宕奸妷锔规嫽?
//   2) 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌﹀Ο渚▓婵炲瓨鐗犻弻鏇熺箾閻愵剚鐝旂紓浣哄У閻擄繝寮婚弴锛勭杸閻庯綆浜栭崑鎾诲即閻樺吀绗夊┑鐐村灟閸ㄥ宕ｉ幘缁樼厱闁靛鍨电€氼厼鐣烽崘宸富闁靛牆鎳愮粻鎵偓瑙勬处閸撶喖宕洪妷锕€绶為柟閭﹀墰椤旀捇姊洪崨濠傚闁告侗鍨遍弲銊モ攽閻樻剚鍟忛柛鐘愁殙瑜扮娀姊洪崨濠冪叆闂佸府缍侀悰顕€骞嬮敃鈧粈瀣亜閺嶃劍鐨戞い鎾虫惈閳规垿鎮╃拠褍浼愰梺鐟板槻椤戝棛鈧潧銈稿畷鎯邦槷闁衡偓?sniroute.cmd up 闂傚倸鍊搁崐鐑芥嚄閸洏鈧焦绻濋崒妤佺亙濠电偞鍨崹娲疾濠靛鐓忛柛顐ｇ箥濡插綊鏌涚€Ｑ勬珚闁哄矉绲借灒闁惧繒娅㈢槐鐐差渻閵堝倹娅婇柡鈧柆宥呂﹂柛鏇ㄥ灠缁犲磭鈧箍鍎卞ù鍌毼ｉ纰辨富闁靛牆鎳庨銏ゆ煟閻斿弶娅婃鐐插暟濞戠敻宕担鍏夋瀸闂傚倷娴囬妴鈧柛瀣崌閺屾稑螖閸愩劌澹夐梺?
// 闂傚倸鍊搁崐鐑芥嚄閸洍鈧箓宕奸姀鈥冲簥闂佸壊鍋侀崕杈╃矆婢舵劖鐓欓弶鍫濆⒔閻ｈ京鐥幆褎鍋ラ柟顔筋殜閹粙宕ㄦ繝鍐礁闂備胶绮敮鎺楁晝椤忓嫷鍤曢柛顐ｆ礀闁卞洦鎱ㄥ鍡楀箺闁绘繃妫冮幃妤冩喆閸曨剛顦ㄥ┑鐐跺皺閸犳牕顕ｇ拠娴嬫婵妫欓崓闈涱渻閵堝棗绗掗悗姘卞厴閸┾偓妞ゆ帊绶″▓婊堟煛鐏炶鈧繂顕ｉ崼鏇炵妞ゆ牗绻傞幆鍫熶繆閵堝洤啸闁稿鍋ら獮鎴﹀炊椤掑倸绁﹂梺鎼炲労閸擄箓寮繝鍥х骇闁绘劖娼欓ˉ瀣渻鐟欏嫬鈻曟慨濠勭帛閹峰懘鎮烽柇锕€娈濈紓鍌欑椤戝棝宕归崸妤€绠氶悘鐐插⒔閻熷綊鏌嶈閸撶喖濡存担绯曟瀻闁规儳纾崝锕€顪冮妶鍡楃瑨閻庢凹鍙冮幃锟犳偄閹肩偘绨诲銈嗗姦閸嬪嫭鏅堕灏栨斀闁炽儴灏欓惌娆撴煛鐏炲墽娲撮柛鈺嬬節瀹曟帒顭ㄩ崟顐㈢仭闂傚倷娴囬鏍垂娴煎瓨鍎楅柛灞剧◥缁诲棝鏌ｉ幋锝嗩棄闁绘挻鐩弻宥堫檨闁告挻姘ㄧ划?闂傚倷娴囬褍顫濋敃鍌︾稏濠㈣埖鍔栭崑銈夋煛閸モ晛小闁绘帒锕ョ换娑㈠幢濡櫣浠撮梺鎼炲妽缁诲牓寮婚妸鈺傚亜闁告繂瀚呴姀銏㈢＜闁逞屽墴瀹曞崬鈽夊▎鎴濆箞闂備線娼чˇ浼村垂閻㈢鍋撳鐓庢珝閽樻繈鏌曟径鍡樻珕闁抽攱鍨块弻娑樷攽閸℃浼€婵犫拃鍥︽喚闁哄本鐩獮妯尖偓闈涙啞閸Ｑ囨⒑?DNS 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧壕鐟懊归悩宸劀缂傚秵鐗曢…鍧楁嚋闂堟稑顫囬梺?闂?
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
        // up 婵犵數濮烽弫鎼佸磻閻愬樊鐒芥繛鍡樻尭鐟欙箓鎮楅敐搴℃灍闁搞倕鑻灃闁挎繂鎳庨弳娆撴煕鐏炶濡界紒缁樼洴楠炲鈻庤箛鏇氭偅缂?闂傚倸鍊峰ù鍥х暦閸偅鍙忕€广儱顦悙濠囨煏婵犲繘妾繛鍫濇啞缁绘繈鎮介棃娴舵盯鏌涚€ｎ偅宕岄柡灞剧洴婵＄兘顢欓悡搴交缂傚倷璁查崑鎾愁熆閼搁潧濮堥柣鎾冲暣閺屾洘寰勯崼婵嗗闂佸搫妫濇禍鍫曞蓟閿熺姴宸濇い鏃傚亾閸ｄ即姊洪崫鍕拱缂佸鎹囬崺鈧い鎺戯功缁夌敻鏌涚€ｎ亶妲圭紒鏃傚枎铻ｅ┑鍌濇缂嶅﹪骞冮埡鍛殤妞ゆ帊鐒﹂崰鏍⒒娴ｇ瓔鍤冮柛銊ュ船铻為柛鎰靛枛閽冪喖鏌曟繛褉鍋撻柛瀣崌閺佹劙宕ㄩ娆戦┏闂備焦濞婇弨鍗灻洪悢濂夋綎婵炲樊浜滄导鐘绘煕閺囥劌娅樻繛鐓庣埣濮婅櫣绮欓幐搴℃敪閻庤娲熷褍宓勯梺褰掑亰閸忔﹢寮ㄦ禒瀣厱妞ゆ劗濮撮悘鈺冣偓瑙勬磸閸庨亶鈥旈崘顔嘉ч柛鈩冪懃椤囨⒑閼测晜鍤€濠⒀呮櫕濡叉劙鎮欓崫鍕啋闂佸搫鍊堕崕鏌ワ綖瀹ュ鈷戦悷娆忓閸斻倗鐥紒銏犲箺闁绘瀛╃换婵堝枈濡搫鈷夐悷婊勬緲閸熸挳鐛Δ鈧…銊╁川椤撶偟浜伴梻浣稿暱閹碱偊宕悩璇茬；闁瑰墽绮弲鏌ュ箹鐎涙绠橀柣鈺侀叄濮婅櫣绱掑Ο璇查瀺缂備浇顕ч悧鎾愁嚕鐠囨祴妲堟繝濠傛噽閺夋悂鏌ｆ惔顖滅У濞存粍绮撻敐鐐哄炊椤掍讲鎷虹紓浣割儏濞硷繝顢撳Δ浣典簻闁哄洢鍔岄獮妤冪磼閸屾氨效鐎规洏鍔嶇换婵嬪礃椤忓柊銈夋⒒娴ｅ摜鏋冩俊妞煎妿缁牊鎷呴崫鍕垫闂佺鍕垫畷闁抽攱鍨垮濠氬醇閻旇　濮囧銈冨劜缁捇寮诲☉娆戠瘈濞达綀娅ｉ悡鎾绘⒑鐠団€虫灈婵炲皷鈧磭鏆﹂柛妤冨亹閺嬪酣鏌熼柇锕€澧扮憸鐗堟尦濮婄粯绗熼埀顒€顭囪閹广垽宕卞☉妯碱槶濠电娀娼уú銊┿€呴柨瀣╃箚妞ゆ牗鐟ㄩ鐔兼煟?30 缂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾閽樻繈鏌熷▓鍨灍闁哄棙绮嶉妵鍕疀閹炬惌妫″?
        // 婵犵數濮烽弫鎼佸磻濞戙埄鏁嬫い鎾跺枑閸欏繘鎮楅棃娑欐喐闁活厽鎸鹃埀顒冾潐濞叉牕煤閵娧呬笉闁哄倹顑欓悢鍡涙偣閸濆嫭鎯堟い銊︾懇閺岋綁骞掗幋顖濆惈閻庢鍠栭悥鐓庣暦閻撳寒娼╂い鎾跺枔瑜板懘鏌ｆ惔銈庢綈婵炲弶锚鐓ら柣鏃傚帶缁犳牗淇婇婵囩《闁汇倐鍋撻梻浣筋潐閸庢娊鎮块崶顒€鏄ラ柣鎰劋閳锋垿鏌涘┑鍕姎闁哄棝浜堕弻娑氣偓锝庡亝瀹曞矂鏌″畝瀣М闁诡喒鏅犲Λ鍐ㄢ槈濡警鐎遍梻鍌欑劍閹爼宕濆畝鈧竟鏇㈩敇閻樻彃鐏婇梺鍐叉惈閹冲海鐥閺岀喓鈧稒蓱閳锋劙鏌?up闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏楣冩闁稿顑呴埞鎴︽偐閹绘帊鍠婇梺鍏兼緲濞硷繝寮诲☉銏犖ㄦい鏃傚帶椤晝绱撴担鎻掍壕婵犮垼鍩栭崝鏍偂閸愵喗鐓忓璺虹墕閸旀岸鏌￠崱娑楁喚闁诡喛顫夐幏鍛村捶椤撶偛缁╅梻浣告惈鐞氼偊宕愬Δ鍛劦妞ゆ巻鍋撴繝鈧潏銊﹀弿闁汇垺娼屾径瀣窞闁归偊鍘鹃崢閬嶆⒑闂堟丹娑㈠礃閻愵剚鐦旂紓鍌氬€峰ù鍥ь浖閵娧冨灊婵炲棗绻掗弳锕傛煙閹殿喖顣奸柛瀣闇夐柨婵嗘祩閺€浼存煛閸涘洤鍟犻弨浠嬫煟閹邦厽缍戦柣蹇ョ畵閺岀喖宕ㄦ繝鍐ㄢ偓鎰版煛娴ｇ鏆ｉ柡浣瑰姍瀹曞爼鍩℃担姝屝曢梻鍌欑閹碱偊藝娴兼潙鍨傞柣鏃傚帶閸戠娀骞栧ǎ顒€濡介柣鎾卞劦閺岋綁寮幐搴㈠創閻庢稒绻傞埞鎴︽倷閸欏娅￠梺鎼炲妺缁瑥顕ｇ拠娴嬫婵妫欓崓鐢告⒑缂佹ɑ灏繛鎾敱缁傛帡顢橀悢铏圭槇闂佸啿鐨濋崑鎾垛偓骞垮劚濞层劑寮抽妶澶嬪€垫繛鍫濈仢濞呮﹢鏌涢幘璺烘灈闁靛棔绀侀～婊堝焵椤掍椒绻嗘慨婵嗙焾濡叉儳顪冮妶蹇撶槣闁稿锕ら～?
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
        // up 闂傚倸鍊搁崐鐑芥倿閿曞倹鍎戠憸鐗堝笒缁€澶屸偓鍏夊亾闁逞屽墴閸┾偓妞ゆ帊绀侀崵顒勬煕濞嗗繐鏆ｉ柕鍡曠铻栭柍褜鍓熼垾锕傚Ω閳轰礁绐涘銈嗘婵倝顢欓崶顒佲拻濞达絽鎲￠崯鐐电磼閸濆嫭鍋ョ€规洘娲熷畷妯好圭€ｎ偆浜欓梻渚€娼х换鍫ュ磹閺嶎厽鍋傛繛鎴烇供閻斿棝鎮瑰ú顏嗙窗閻庢矮鍗抽弻锛勨偓锝庝簻椤掋垽鏌熸笟鍨閾伙絿绱掔€ｎ亞浠㈡い顒€顦辩槐鎾存媴缁嬪簱鍋撻崷顓熸殰闁跨喓濮撮弸渚€鏌熼悧鍫熺凡闂佸崬娲︾换娑㈠箣閻愯尙鐟ㄦ繛瀛樼矒缁犳牠寮诲☉銏犵疀闂傚牊绋掗悘鍫ユ煟鎼淬垼澹樻い锕備憾閸┾偓妞ゆ巻鍋撶紒鐘茬Ч瀹曟洟鏌嗗鍛枃闁瑰吋鎯岄悡澶岀礊閺嶎厽鐓忛柛顐ｇ箖缁€宀勬煕鐎ｎ偅宕岀€规洜鍏橀、妯款槾闁告梹鎮傞幃妤€鈻撻崹顔界仌闂佺硶鏅滈悧鐘茬暦濮橆厾绡€闁搞儯鍔嶉崕顏堟煟閻斿摜鎳冮悗姘煎墴閹啴顢曢敂瑙ｆ嫼闂佸憡绋戦敃銈夋倶閻斿吋鐓曞┑鐘插枤濞堟粓鏌ｅ☉鍗炴珝濠殿喒鍋撻梺闈涚箞閸ㄥ鏁嶅鍫熲拺闁告繂瀚弳鐐烘煕閵娧勬毈闁诡喗锕㈤幃鈺呭垂椤愩垺鐎鹃梻浣虹帛椤ㄥ懘鎮ц箛鎾佽櫣鈧稒锕╁▓浠嬫煟閹邦厽缍戞繛鎼枛閳规垿顢欓悷棰佸闂傚倷鐒︾€笛呯矙閹寸姭鍋撳銉ュ鐎规洘鍨块幃娆撴倻濡攱瀚奸梻浣告啞缁嬫垿鏁冮敃鍌氱疇闁归偊鍎悷閭︾叆闁告侗鍘哄Σ鎰版倵濞堝灝娅橀柛瀣攻娣囧﹪鎳滈棃娑氱獮婵犵數濮寸€氫即鎮伴幘缁樷拻濞撴埃鍋撴繛浣冲泚鍥敇閵忕姷锛熼梻渚囧墮缁夎泛鐣垫笟鈧弻鐔告綇閸撗呮殸缂備胶濮寸紞濠囧箖濡も偓閳藉鈻嶉搹顐㈢仼缂侇喗姘ㄩ幑鍕Ω瑜忛敍婊堟煛婢跺﹦澧戦柛鏂挎捣缁寮介銈囷紲闁诲函缍嗘禍璺何ｉ崨濞夸簻闁瑰墽鍋ㄩ崑銏⑩偓瑙勬礀閵堟悂銆侀弴銏℃櫜闁搞儮鏅涢獮鈧梻鍌氬€搁崐椋庢濮橆剦鐒介柤濮愬€栫€氬鏌ｉ弮鍌氬付缂佲偓婢跺备鍋撻崗澶婁壕闂佸憡娲﹂崜娆愮婵傚憡鈷戠紓浣癸供閻掔晫绱掗鍛仸闁诡噯绻濆畷鎺戔攦閹傚濡ょ姷鍋涢悘婵嬪礉濮樿埖鐓欓柛鎰皺缁犺崵鈧?
        // 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敂钘変罕闂佺硶鍓濋悷褔鎯岄幘缁樺€垫繛鎴烆伆閹达箑鐭楅煫鍥ㄧ⊕閻撶喖鏌￠崒姘变虎闁诡喗鍨块弻娑欐償閿涘嫅褔鏌＄仦鍓ф创鐎殿喗鎸虫俊鎼佸Ψ閵壯屽晪闂佽姘﹂～澶娒洪弽顓熷亯濠靛倻顭堥拑鐔哥箾閹寸們姘跺几鎼淬垻绠鹃柛鈩兠悘鈺冩偖閿曞倹鈷掑〒姘ｅ亾婵炶壈宕电槐鐐寸瑹閳ь剝妫㈤梺瑙勫礃椤曆呯不椤栫偞鐓熸俊顖滅帛閵囩喖鏌℃担鍝バｉ柟渚垮妼椤粓宕卞Δ鈧～顐ｇ箾鐎电甯堕柣妤佹尭椤?濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴欏灪閸庢垿鏌＄仦璇插姎閸ユ挳姊虹化鏇炲⒉妞ゃ劌锕よ灋闁绘垼濮ら埛?闂?
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


// WARP 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢妶鍌氫壕婵ê宕崢瀵糕偓瑙勬礉椤鈧潧銈稿鍫曞箣閻樺灚姣庢繝鐢靛仦閹稿宕洪崘顔肩；闁瑰墽绮悡娆愩亜閺傝法浠㈢悮姘攽閻愰潧甯剁紒缁橈耿瀹曟椽鏁撻悩鑼紲濠电偞鍨堕悷顖炲触閸岀偞鈷掗柛灞剧懅椤︼附绻濋埀顒勬焼瀹撱儱顦甸獮鍥敄闁款垰浜鹃柟鐑橆殔閹硅埖銇勯幘瀵糕槈濞存粓绠栧濠氬醇閻旀亽鈧帡鏌ｉ幒鎴炴喐闁逞屽墲椤煤濡吋宕查柛顐ゅ枍缁诲棝鏌熼梻瀵割槮閸烆垶姊洪幐搴㈢５闁稿鎹囬弻娑㈠箻閹颁胶鍚嬪┑顔硷工閹碱偆鍙呯紓鍌欑劍閿氶柍璇茬箻濮婃椽宕ㄦ繝鍐弳闂佹寧娲︽禍顏勭暦濞差亜鐒垫い鎺嶉檷娴滄粓鏌熼崫鍕棞濞存粓绠栧铏圭矙閸栤€充紣缂傚倸绉撮敃顏堢嵁閸愵喖唯闁冲搫鍊瑰▍婊堟⒑閹肩偛鍔橀柛鏂跨焸椤㈡挸鈽夐姀鈾€鎷虹紓鍌欑劍閿曗晛鈻撻弮鍫熺厽婵°倐鍋撻柨鏇ㄤ邯楠炲啫螖閸滀焦鏅╁┑?data\.warp-enabled闂?
//   闂傚倸鍊峰ù鍥敋瑜忛埀顒佺▓閺呮繄鍒掑▎鎾崇婵°倓鐒﹀▍鏂库攽閻樺灚鏆╁┑顕呭弮瀹?  -> 闂傚倸鍊搁崐鐑芥倿閿曗偓椤啴宕归鍛姺闂佺鍕垫當缂佲偓婢跺备鍋撻獮鍨姎妞わ富鍨跺浼村Ψ閿斿墽顔曢梺鐟邦嚟閸嬬喖銆傞弻銉︾厸闁糕剝顭囬惌鎺楁煙椤旀枻鑰块柟顔界懇濡啫鈽夊Δ鈧ˉ姘辩磽?WARP 闂傚倷娴囬褏鈧稈鏅犻、娆撳冀椤撶偟鐛ラ梺鍦劋椤ㄥ懐澹曟繝姘厵闁告挆鍛闂佺粯鎸鹃崰鎰板Φ閸曨垰绠涙い鏂垮悑閻庣粯绻涚€涙鐭掔紒鐘崇墪椤繐煤椤忓嫮顔囬柟鑹版彧缁插搫危閸儲鈷戠紓浣癸供濞堟洟鏌熼悷鐗堟悙闁伙絿鍏橀獮搴ㄦ寠婢跺矈鍟囧┑鐐舵彧缁查箖鎮洪弴鈽嗘澓婵犵绱曢崑鎴﹀磹閺嶎厽鍋嬫俊銈呭暙閺嬪牏鈧箍鍎遍ˇ顖滃瑜版帗鈷掗柛顐ゅ枙閸庢劙骞栧ǎ顒€濡奸柣鎰躬閺屾洘绻涜閹虫劙寮抽锔解拺閻犲洦鐓￠妤呮煟韫囨棁澹橀摶鐐烘煕閹伴潧鏋涢柡?
//   婵犵數濮烽弫鎼佸磻閻愬搫鍨傞柛顐ｆ礀缁犱即鏌涘┑鍕姢闁活厽鎹囬弻鐔虹磼閵忕姵鐏嶉梺绋款儍閸婃繈寮婚弴鐔虹闁绘劦鍓氶悵鏃堟⒑?-> 婵犵數濮甸鏍窗濡ゅ啯鏆滄俊銈呭暟閻瑩鏌熼悜妯镐粶闁逞屽墾缁犳挸鐣锋總绋课ㄦい鏃囧Г濞呭秴鈹戦悩鍨毄濠殿喚鏁婚幊婵嬪礈瑜忛惌鍡涙煕瀹€鈧崑鐐烘偂濞嗘挻鐓欐い鏍ㄧ⊕缁惰尙鎮鑸碘拺闁圭娴烽埊鏇熶繆椤愩垹鏆ｇ€殿喖顭锋俊鎼佸Ψ閵忊槅娼旀繝纰樻閸ㄦ娊宕㈣閵嗗倻鎲撮崟顏嗙畾闂侀潧鐗嗗ú銈呯摥婵犵數鍋涘Ο濠囧储婵傚憡鍋╅柣鎴犵摂閺佸啴鏌ㄥ┑鍡樺窛闁硅櫕绻堝娲焻閻愯尪瀚板褌鍗抽弻娑㈠箳閹捐櫕璇炲銈冨灪缁嬫垿鈥﹂妸鈺佺妞ゆ劦鍋€閸嬫挸鈽夐姀鈾€鎷洪梻鍌氱墛缁嬫挻鏅堕弴鐔虹閻忕偛鍊搁埀顒侇殜閹柉顦规慨濠勭帛閹峰懐绮电€ｎ亝鐣伴梻浣侯焾缁绘垹绮欓幘鏂ユ瀻闁靛繈鍊曞洿闂佹悶鍎洪悡鍫濐焽閻斿吋鈷掑〒姘搐婢ь喚绱掓径濠庡殶闁瑰弶鎸抽弫鍌涙叏閹邦亞鐩庨梻浣告惈缁夋煡宕濊箛鏃傜彾婵☆垰鍚嬮崑鏍煟閺傚灝鎮戦柍閿嬪灴閺屾稑鈹戦崱妤婁患闂佸搫妫涢崑銈夊蓟濞戞瑦鍎熼柕鍫濇噸濮规姊洪柅鐐茶嫰婢ь垶鏌涘┑鍡涘弰鐎规洖缍婇幐濠冨緞鐏炵偓顔曢梻浣瑰缁诲倿藝椤栨粎涓嶉柨婵嗩槹閻撶喐淇婇姘变虎闁汇劍妞介弻锝夊Χ閸屾矮澹?WARP 闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濇繝鍌滃闁逞屽墾缁犳挸鐣锋總绋款潊闁炽儱鍟跨花銉︾節閻㈤潧浠﹂柛顭戝灦瀹曠懓煤椤忓嫮锛熼梺鐟板⒔缁垶鎮￠崘顏呭枑婵犲﹤鐗嗙粈鍫熺箾閹寸偟鎳呮い鈺傜叀閺屾稑鈽夊鍫熸暰缂傚倸绉村ú顓㈠蓟閺囩喓绡€闊洦鎼╁ú顓㈡⒑閸涘﹥鈷掗柛蹇旓耿楠炲啫螖閳ь剟鍩㈤幘璇插瀭妞ゆ梻鏅禍鎰版⒒娴ｄ警鐒炬い鎴濇楠炴垿宕堕鈧拑?
// 闂傚倸鍊搁崐鐑芥倿閿曗偓椤啴宕归鍛姺闂佺鍕垫當缂佲偓婢跺备鍋撻獮鍨姎妞わ缚鍗冲畷鎴炲緞閹邦厾鍙嗗┑鐘绘涧濡瑦寰勯崟顐熸斀妞ゆ牗鍝庨崑銏ゆ煛瀹€瀣埌閾绘牠鏌嶈閸撶喖骞冭缁绘繈宕堕妸銉㈠亾閸偁浜滈柡鍐ｅ亾缂佷胶顢婇妵鎰板箳閹惧厖姹楅梻浣告啞閸旀牞銇愰崘鈺傚弿闁归棿鐒﹂埛鎴︽⒑椤愩倕浠滈柤娲诲灡閺呭爼顢欑亸鏍ㄦ杸闂佺鏈粙鎺楀煕閹烘梻纾兼い鏇炴噹楠炴鈧灚婢樼€氫即鐛崶顒夋晢闁逞屽墮椤曪絽鐣￠柇锔藉瘜闂侀潧鐗嗗Λ娑欐櫠椤掑倻纾奸悹鍥皺鏁堥悗?JSON闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏鍓х暠閻庢艾顦伴妵鍕箳閸℃ぞ澹曢梺鍛婎殜娴滃爼寮诲☉妯锋婵鐗嗘慨娑㈡⒑瀹曞洨甯涙俊顐㈠暙椤曪綁宕奸弴鐔封偓鐑芥煛婢跺鐒炬俊顐㈡濮婃椽宕妷銉︾€婚梺鎸庢处娴滎亜顕?.autostart-off 婵犵數濮烽弫鎼佸磿閹寸姴绶ら柦妯侯棦濞差亝鏅滈柣鎰靛墮鎼村﹪姊洪崨濠傚Е濞存粍鐗犲畷鎴﹀箻鐠囨彃鐎銈嗗姧缂嶅棗螞閸愵喗鍊甸悷娆忓绾炬悂鏌涙惔銈嗙彧缂佸倹甯￠弫鍐磼濞戞ɑ鐣伴梻浣稿暱閹碱偊鏁冮敐鍥潟婵☆垵鍋愮壕浠嬫煕鐏炲墽鎳呮い锔奸檮閵囧嫰鏁傜拠鑼化缂備緡鍠楀濠氬焵椤掍胶鈯曟い顓炴喘閵嗗懘鏌ㄧ€ｎ剛顔曢梺鐟邦嚟閸婃垵顫濈捄鍝勫殤闂佸搫绋侀崢浠嬫偂濞戙垺鐓曢柕澶涚到婢у憡鎱ㄥΟ鎸庣【缁?
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

// 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鑼槱閻熸粎澧楃敮鎺楀垂閸岀偞鐓熸俊銈傚亾闁绘锕畷锝堢疀濞戞瑧鍘撻梺鍛婄箓鐎氼參宕抽崷顓犵＜妞ゆ劑鍨绘晥闂佸搫琚崐婵嬬嵁閺嶃劍濯撮悷娆忓閺侇亪姊绘担鐟扳枙闁衡偓閸楃倣娑㈠礃椤旇偐鐣?婵犵數濮甸鏍窗濡ゅ啯鏆滄俊銈呭暟閻瑩鏌熼悜妯镐粶闁逞屽墾缁犳挸鐣锋總绋课ㄦい鏃囧Г濞呭秴鈹戦悩鍨毄濠殿喚鏁婚幊婵嬪礈瑜忛惌鍡涙煕瀹€鈧崑鐐烘偂濞嗘挻鐓欐い鏍ㄧ⊕缁惰尙鎮鑸碘拺?闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熼悜妯虹劸婵炲皷鏅犻弻鏇熺箾閻愵剚鐝旈梺鍝勵儎缁舵岸寮婚弴銏犻唶婵犲灚鍔栫瑧缂傚倷鑳舵刊瀵哥礊娴ｅ壊娼栨繛宸簻閸ㄥ倹銇勯弴鐐村櫣闁告挸鍟块埞鎴︻敊绾柉鍚紓浣哄У閻楃姴顕ｇ拠娴嬫婵妫欓崓鐢告⒑缂佹ɑ灏繛鎾敱缁傛帡顢涢悙绮规嫼闁哄鍋橀崡鍐差瀶閻戣姤鐓曞┑鐘插暟婢ь亝銇勯鐐村缂佺粯绻傞～婵嬵敆閸屾埃鍋撻悙鐑樷拺闂傚牊渚楀Σ鍫曟煕閵婎煈娼愮紒鍌氱Ч楠炲棝鈥栭垾宕囩Ш闁轰焦鍔栧鍕熷ú缁橆棝缂?WARP 闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濇繝鍌滃闁逞屽墾缁犳挸鐣锋總绋款潊闁炽儱鍟跨花銉︾節閻㈤潧浠﹂柛顭戝灦瀹曠懓煤椤忓嫮锛熼梺鐟板⒔缁垶鎮?-> 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧壕濠氭煕閳╁啰鈽夐柛灞诲姂閺屻倕霉鐎ｎ偅鐝曞銈庡亝濞茬喖寮婚悢鍛婄秶闁告挆鍛闁?
// 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐蹭画濡炪倖鐗楃粙鎾汇€呴崣澶岀瘈濠电姴鍊搁弸锕傛煠閻楀牆顕滈柕鍥у缁犳盯骞樼捄渚毇闂佽崵鍠愰悷閬嶆⒔閸曨垰桅闁告洦鍨伴崘鈧梺闈涱焾閸斿矂藟濠靛鍋犳慨妯哄船閺嬫稒鎱ㄦ繝鍐┿仢鐎规洦鍋婂畷鐔碱敃閿濆棭鍟堝┑鐘垫暩閸嬫盯藝閹殿喚鐭欓柟鐑樻⒒閻鏌熼悜妯虹亶婵℃彃鐗婄换娑㈠幢濡櫣浜堕梺鍝勵儐閻楃姴顫忓ú顏呯劵闁绘劘灏€氭澘顭胯閸ｏ綁寮诲鍥ㄥ珰闁哄被鍎卞浼存⒑缁洘鏉洪柛搴ㄦ涧閻ｇ兘濡搁埡濠冩櫖濠电姴锕ょ€涒晛鏆╂繝鐢靛Х閺佹悂宕戦悙鍝勫瀭闁割偅娲栫粻浼存煕濠靛嫬鍔ら柣顓熸崌閺岋綁濮€閵忊晝鍔稿┑鈽嗗亝閿氶棁澶愭煥濠靛棛澧辨繛鍏煎姇闇夋繝褍鐏濋埀顒佺墱閹广垹鈽夐姀鐘殿吅闂佺粯鍨靛Λ娑㈠礉閻戣姤鈷戦柛婵嗗椤ユ瑩鏌涘Δ浣糕枙鐎殿喖顭烽弫鎾绘偐閼碱剦妲版俊鐐€栭幐楣冨窗閹捐违闁归偊鍏橀弨浠嬫煟閹邦剙绾ч柛锝嗘そ閹粙顢涢妶鍥╃槇閻庢鍣崑鍕敇婵傜宸濇い鏍ㄧ⊕閻ｇ兘姊绘担鍛婂暈婵炶闄勭粋宥夋倷閸偒娼熷┑顔筋焾閸╂牠鍩涢幋鐘电＜閻庯綆鍘界涵鍓佺磼閻樺啿顥嬬紒杈ㄥ笚瀵板嫮鈧綆鍋勯崬澶娢旈悩闈涗粶缂佺粯鍨归埀顑跨祷閸嬫劙鍩€椤掍胶鈯曢柨姘归悩铏仢婵﹥妞藉Λ鍐ㄢ槈鏉堚晛褰嬬紓鍌欒兌婵敻宕归崸妤€绠栭柨鐔哄Т閸楁娊鏌曡箛銉х？闁告鏁诲濠氬磼濮樺崬顤€婵炴挻纰嶉〃濠傜暦閺囷紕鐤€闁圭虎鍨遍弬鈧俊鐐€栧Λ渚€锝炴径濞炬瀺闁糕剝顭囩粻楣冩煟閹惧啿鐦ㄩ柣鎾炽偢閺岀喖顢欓悾灞惧櫚閻庢鍠栭悥濂哥嵁鐎ｎ亖鏀介柟閭﹀帨閳哄懏鈷掗柛灞剧懆閸忓矂鏌熼搹顐ｅ磳闁诡喗锚椤繄鎹勯搹璇″敹濠电姷鏁告慨鐢靛枈瀹ュ纾归柛顐ｆ礃閻撱儵鏌￠崶鈺佷粶闁逞屽墯閻℃洜鎹?
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

// ---------------------------------------------------------------- WARP 闂傚倸鍊搁崐鐑芥嚄閸洍鈧箓宕奸妷顔芥櫈闂佸憡鍔﹂崰鏍閸ф鈷戞い鎺嗗亾缂佸顕划濠氬冀椤愮喎浜炬鐐茬仢閸旀碍銇勯敂璇茬仩妞ゆ柨绻愰埞鎴﹀幢韫囨梹鏉搁梻浣虹帛閿氱€殿喛鍩栭崚濠冪附閸涘﹦鍘?
// 婵犵數濮烽弫鎼佸磻閻愬搫鍨傞柛顐ｆ礀缁犱即鏌涢锝嗙缁炬崘顫夋穱濠囧Χ閸涱喖鐝旂紓?WARP 婵犵數濮烽弫鎼佸磻閻愬搫鍨傞柛顐ｆ礀缁犱即鏌涘┑鍕姢闁活厽鎹囬弻锝夊箣閿濆棭妫勭紓浣插亾闁稿本绋掗崣蹇斾繆閻愰鍤欏ù婊堢畺濮?闂?
//   1) 闂傚倸鍊搁崐鐑芥嚄閸撲礁鍨濇い鏍亹閳ь剨绠撳畷濂稿Ψ閵夛附袣闂備礁鎼粙渚€宕㈡總鍛婂€块柛顭戝亖娴滄粓鏌熺€电浠滄い鏇熺矋閵囧嫰鏁冮崒銈嗩棖缂?connected 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐蹭画闂佹寧娲戦悞锕傚箠瀹€鍕拻?disconnected      -> 缂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇炲€搁拑鐔兼煏婵炵偓娅撻柡浣稿閺屾稑鈽夊▎鎰▏闂佸搫瀚ㄩ崕鐢稿蓟閺囷紕鐤€閻庯綆浜栭崑鎾诲冀椤撶喎鈧法绱撴担鑲℃垿宕ｈ箛鏂剧箚妞ゆ牗鍝庢禒鎺楁煕濮樼厧浜伴柡灞剧☉閳诲海浠﹂幋鎺戭棜
//   2) 闂傚倸鍊搁崐鐑芥嚄閸撲礁鍨濇い鏍亹閳ь剨绠撳畷濂稿Ψ閵夛附袣闂備礁鎼粙渚€宕㈡總鍛婂€块柛顭戝亖娴滄粓鏌熺€电浠滄い鏇熺矌缁辨帗鎷呴崫鍕垫毉闂?connected 婵犵數濮烽弫鎼佸磻閻樿绠垫い蹇撴缁躲倝鏌﹀Ο鐚寸礆婵炴垶顭傞弮鍫濆窛妞ゆ挾鍋為崕顏呬繆閻愵亜鈧牠骞愰崼鏇炲瀭鐟滅増甯楅崕妤佷繆閵堝懏鍣洪柣鎾卞劜缁绘繈妫冨☉娆樻！濡炪們鍎茬划鎾诲箖绾拋妲婚梺瀹︽澘濡块柟骞垮灩閳藉濮€閻樼敻鐛撻梻浣筋潐閹矂宕㈡總绋跨妞ゆ柨澧界壕钘壝归敐鍫殐婵炲牊姊归妵鍕晜閻愵剚姣堥悗娈垮枛椤嘲鐣烽妸鈺婃晣鐟滃酣宕撻悽鍛娾拺闁告稑锕ョ€垫瑩鏌涘鈧悞锕佲吀婵犵數濮烽弫鎼佸磻閻愬搫鍨傞柛顐ｆ礀缁犱即鏌涢锝嗙闁搞劌鍊块弻娑㈡晜鐠囨彃绠伴梺绋款儐閹瑰洤鐣疯ぐ鎺濇晝闁糕剝锚閻忔挳鏌?缂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇楀亾閾荤偤鐓崶銊р槈闁搞劌鍊块弻鐔煎箚閻楀牜妫勭紒鐐劤椤兘寮婚妸銉㈡斀闁糕剝锚椤︹晠姊虹粙娆惧剱闁绘顨婃俊鐢稿礋椤栨氨鐤€闂佸壊鍋呭ú姗€顢撳鍜佹富闁靛牆鎳庨銏ゆ煟閻斿弶娅婄€?
//      -> 闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濋棃娑卞剰缁炬儳顭烽弻锝夊棘閸喗鍊梺绋款儍閸斿海妲愰幘瀛樺濠殿喗鍩堟禍鐐测枎閵忋倖鍊烽柛婵嗗妤犲洭姊洪崜鎻掍航闁稿瀚粋宥夘敍濠婂嫬浠?2 濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴烆焸濞戞鏆嗛柛鏇ㄥ亞椤︻厼鈹戦悩缁樻锭婵炶绠戦埢宥夋偐缂佹鍘藉┑鈽嗗灥閸嬫劗鏁☉銏＄厽闁圭儤姊荤敮娑㈡煙娓氬灝濡奸摶锝咁熆閸撲緡鐒鹃柟顖氱墦濮婃椽宕ㄦ繝鍕枦闂佺顑嗛幑鍥ь潖缂佹ɑ濯撮柣鐔煎亰閸ゅ绱撴担鍓插剱闁搞劌澧庣紓鎾寸鐎ｎ亞鐫勯梺绋挎湰缁酣鎮?10 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁嶉崟顒佹闂佸湱鍎ら崵锕€鈽夊Ο閿嬫杸闁诲函缍嗘禍鐐侯敊閸ヮ剚鈷戦悷娆忓缁€鍐┿亜閺囧棗鎳愰惌鍡涙煕閹伴潧鏋熼柣鎾存礃缁绘盯骞嬪┑鍡氬煘闂佸搫顑囬崰搴ㄦ箒闂佺粯顭堢粔顕€顢旈崼姘ｅ亾閿曞倸閿ゆ俊銈勭濞堟繈姊哄Ч鍥х伄闁轰焦鎮傚畷锝呪攽閸垻锛濇繛杈剧秬椤鍩涢幇顓犵闁稿繗鍋愰幊鍥煕閳瑰灝鐏柟顖涙婵℃悂濡疯瀵劑姊绘担鍛婂暈濞撴碍顨婂畷褰掑箮閸撳灝娲、姘跺焵椤掆偓椤繒绱掑Ο鑲╂嚌闂佹悶鍎滈崘鐐氦闂傚倷娴囬褏鎹㈤幋鐘冲床闁割偅绻勯弳锕傛煏韫囧鈧洖娲垮┑鐘灱濞夋盯鏁冮敃鍋瑰洭顢曢敂钘夆偓鐢告煟閻斿憡绶叉い蹇ｄ簻椤潡鎮烽悧鍫濡?
// 闂傚倸鍊搁崐鐑芥倿閿曗偓椤啴宕归鍛姺闂佺鍕垫當缂佲偓婢跺备鍋撻獮鍨姎妞わ富鍨跺浼村Ψ閿斿墽顔曢梺鐟邦嚟閸嬬喖骞婇崨瀛樼厓缂備焦蓱椤ュ牓鏌熼鑲╃Ш鐎规洖鐖兼俊鎼佸Ψ瑜忛妶鐑芥煟閻愬瓨鐨戦柛鐘崇墪椤繐煤椤忓嫪绱堕梺鍛婃处閸嬪倻鑺辨禒瀣拺闁告稑顭悞楣冩煕鐎ｎ偄濮囬崡閬嶆煙閻楀牊绶茬紒鐙呯秮閺屻劑寮村Δ鈧禍鍓х磽?g_warpManualStop=true闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熺€电浠ч梻鍕閺岋繝宕橀妸銉㈠亾閼姐倗涓嶉柡鍌涱儥閻斿棝鏌ら幖浣规锭濠殿喖娲ㄧ槐鎺楁偐椤旂厧濮﹂梺鍝勭灱閸犳牠骞冨▎鎿冩晢濞达綀娅ｉ埢蹇斾繆閻愵亜鈧垿宕曟潏鈺冪煋鐟滅増甯掗拑?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊搁崐鎼佸磹妞嬪孩顐介柨鐔哄Т绾惧鏌涘☉鍗炴灓闁崇懓绉归弻褑绠涘鍏肩秷閻?WARP 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐蹭画闂佹寧娲栭崐褰掑磻鐎ｎ喗鐓涢柛鎰╁妼閳ь剙顭峰畷鈥愁潩閼搁潧鈧灚绻涢幋鐐垫噽闁绘帊绮欓弻?204 缂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇楀亾閾荤偤鐓崶銊р槈闁搞劌鍊块弻鐔煎箚閻楀牜妫勭紒鐐劤椤兘寮婚妸銉㈡斀闁糕檧鏅滅瑧闁哄鏅滅换鍫濐潖閾忓湱鐭欓柟绋垮閹疯京绱撴担绛嬪殭閻庢凹鍠氶崚鎺撶節濮橆剝鎽曢梺闈涱檧婵″洭宕?true 闂傚倸鍊峰ù鍥х暦閻㈢纾婚柣鎰暩閻瑩鐓崶銊р槈缂佲偓婢舵劕绠规繛锝庡墮婵＄厧顩奸崨顓涙斀妞ゆ梻鐡旈悞鐐箾婢跺銆掔紒顔肩墦瀹曠喖顢涘☉姘箞婵犵數濞€濞佳兾涘Δ鍜佹晜闁冲搫鍟扮壕鐣屸偓骞垮劚濡瑥鐣峰畝鍕厸閻忕偛澧介埥澶愭煃鐠囨煡顎楅摶锝夋煣韫囨稈鍋撳☉妯侯嚙闂傚倸鍊峰ù鍥ь浖閵娾晜鈷旂€广儱顦壕瑙勭節闂堟稒鐭楃紒?
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
        if (st == 0) {                     // 濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴欏灪閸庢垿鏌＄仦璇插姎閸ユ挳鏌熼崗鑲╂殬闁糕晛瀚灋闁告稑鐡ㄩ埛鎴︽偣閸ャ劎鍙€妞ゅ孩顨堢槐鎺楁偐閼碱儷褏鈧娲滈幊鎾跺弲濡炪倕绻愮€氼厼危椤掆偓閳规垿鎮欓弶鎴犱桓濠殿喗菧閸旀垵鐣烽垾鎰佹僵妞ゆ挻绮堢花濠氭⒑閸濆嫮袪闁稿酣浜堕崺鈧い鎺嶇贰濞堟绱掗鑲╁鐎垫澘瀚换娑㈠箳閹寸姷楔闂佽鍠掗弲鐘汇€侀弴顫稏妞ゆ挾鍎愬Λ婊冣攽閻樺灚鏆╅柛瀣仱瀹曞綊宕奸弴鐐舵憰闂侀潧艌閺呮粓宕戠€ｎ喗鐓欓柟瑙勫姦閸ゆ瑧绱掗埀顒勫磼濞戞绠氬銈嗙墬缁诲倿宕ヨぐ鎺撶厓缂備焦蓱閳锋帡鏌嶈閸撴瑧绮诲澶婄？闂侇剙绉寸粻鐘碘偓骞垮劚濞层倝宕?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧壕濠氭煕閳╁啰鈽夐柛灞诲姂閺屻倕霉鐎ｎ偅鐝曞?
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
constexpr wchar_t kAppVersion[] = L"3.18.0";

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
    // 闂傚倸鍊搁崐椋庣矆娓氣偓瀹曘儳鈧綆鍏橀崑鎾剁箔濞戞ɑ鍣归柛銊︾箞閹﹢鎮欓崹顐ｇ彧闂佸搫妫寸粻鎾诲蓟閿濆绫嶉柛灞捐壘娴犳绱撴担鍝勑ｆ繝銏★耿閳ワ箓宕稿Δ浣告疂闂傚倸鐗婄粙鎴ｎ杺濠电姷鏁搁崑娑㈡偤閵娿儍娲偄閻撳海鐣哄┑掳鍊愰崑鎾绘煃閽樺妲搁柍璇查铻ｉ柤濮愬€栧▓楣冩⒒閸屾瑦顦风紒槌栧櫍濮婁粙宕熼鐔峰簥闂佸壊鍋侀崕鍐茬暤娓氣偓閺岀喖鎮滃鍡樼暥缂備胶濮垫繛濠囧蓟閻旇　鍋撻悽娈跨劸閸熺顪冮妶蹇曠ɑ闁绘挴鈧剚娼栨繛宸簻缁犱即骞栨潏鍓у矝濠殿喖娲ㄧ槐鎺楁倷椤掍胶鍑＄紓浣割槺閹虫捇顢氶敐澶婄闁兼亽鍎辨禍婊堟⒑缂佹ɑ灏繛瀵稿厴椤㈡瑦寰勬繝搴㈠瘜闂侀潧鐗嗗Λ娆撍夐崱妞绘斀妞ゆ柨鎼埀顒佺箞楠炲啫螖閸滀焦鏅╁┑鐐存綑妤犲摜绱炴繝鍥х畺闁冲搫鎳忛崐缁樹繆椤栨粠鍎犻柍褜鍓氶崝娆撳箖瀹勯偊鐓ラ柛娑卞弾閺嗩參姊虹紒妯绘儓缂佺粯绻堥悰顔界節閸嬭姤妫冨畷銊╊敊閸忚偐鏋€闂傚倷绶氬褔篓閳ь剛绱掗悩铏碍妞ゎ剙锕幃婊堟嚍閵夈垺瀚奸梻浣虹帛閹稿摜鈧灚甯掗埢鎾绘偐鐟佷胶鎳撻…銊╁礋椤撶姷鏆ら梺鎹愬吹閸嬬偤濡甸崟顖氱疀闁割偅娲橀宥夋⒑缁嬫鍎忔い鎴濐樀瀵鈽夐姀鈺傛櫇闂侀潧鐗嗛幊蹇涙倵妤ｅ啯鈷戦柛锔诲幖閸樻潙霉濠婂啰鍩ｇ€殿喖顭锋俊鎼佸Ψ閵忊槅娼旀繝纰樻閸ㄦ娊宕㈣璺柍褜鍓氱换婵嬫偨闂堟刀娑㈡煕鐎ｎ偅灏伴柕鍥у楠炴﹢宕￠悙鍏告偅缂傚倷鑳舵慨鍨箾婵犲洤钃熼柣鏂垮悑閸婄粯淇婇婵囶仩濞寸姾鍋愮槐鎺楁倷椤掍胶鍑＄紓浣割槸缂嶅﹤顕ｆ繝姘╅柕澶堝灪椤秴鈹戦绛嬫當婵☆偅鐟╅、鎾诲幢濞嗘垹锛濇繛杈剧导缁瑩宕搹鍦＜閻犲洦褰冮弳娆愩亜椤撶偞宸濋柟椋庡Т闇夐悗锝庡亞閳ь剙顭峰铏规喆閸曨偆顦ㄥ┑鐐叉噺濞茬喖銆侀弮鍫熷€烽柣鎴炃氶幏娲⒑閸涘﹦鈽夐柨鏇樺€栭幈銊╁炊閳哄啰锛滄繝銏ｆ硾閿曪箓鏁嶅澶嬬厵妤犵偛鐏濋悘鈺傘亜椤愶絿绠栨繛鐓庣箻婵℃悂濡疯闁拌鲸绻濋悽闈涗哗闁规椿浜炵槐鐐寸瑹閳ь剝妫㈤梺鍦亾濡炲潡寮崒鐐茬骇闁割偅绋戞俊璺ㄧ磼閻樿崵鐣洪柡灞剧洴閹晠顢曢姀鐘嫟濠电姰鍨婚幊鎾活敄婢跺﹦鏆?
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
        // WARP 闂傚倸鍊峰ù鍥敋瑜忛埀顒佺▓閺呮繄鍒掑▎鎾崇婵°倐鍋撶紒鈧径瀣╃箚妞ゆ牗绻愭蹇撁瑰鍐╄础闁逞屽墮缁犲秹宕曢柆宓ュ洭顢涘鍕幑闂侀€炲苯澧存慨濠冩そ瀹曨偊宕熼鍌樺亹濠电偛鐡ㄧ划搴ㄥ磻閻斿搫鍨濋柨婵嗘啒閺冨牆绀冩い蹇撴４缁?Sniroute 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鍨鹃幇浣圭稁缂傚倷鐒﹁摫闁告瑥绻掗埀顒冾潐濞叉牕煤閵娾晛纭€婵鍩栭悡娆戠磽娴ｉ潧鐏╅柡瀣〒缁辨帡鎮▎蹇斿闁绘挸鍟撮弻鏇熷緞濞戞氨鏆犳繝鈷€灞界仸闁哄矉绻濆畷鍗炍熼懡銈呭殥婵＄偑鍊ら崢鐓庮焽閿熺姴绠犳繝濠傜墛閸嬨劑姊婚崼鐔衡枔闁告艾鍊垮缁樻媴鐟欏嫬浠╅梺绋款焾閸婃繂鐣烽敐澶婄闁靛ě鍕啎闂備焦瀵х换鍌炈囨导鏉戠柧闁哄被鍎抽崣鎾绘煕閵夛絽濡界紒鈧崼鐔翠簻闁挎棁銆€閸嬫捇鍩￠悳鐪妉e/YouTube/Play 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸閻ゎ喗銇勯弽顐粶闁搞劌鍊块弻娑滎槼妞ゃ劌鎳撶换姘攽閻橆喖鐏遍柛鈺傜墵瀹曟繈寮介鐐碉紱闁诲函缍嗛崰妤呮偂閺囩喆浜滈柟浼存涧娴滄粌顭胯婢瑰棛妲愰幒妤€鐒垫い鎺戝楠炪垺绻涢幋鐐垫噮闁?
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

    // 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敂钘変罕闂佸憡鍔﹂崰鏍婵犳碍鐓欓柟瑙勫姦閸ゆ瑧绱掗悩鑼Ш闁诡喗顨呴埥澶娾枍閾忣偄鐏╃紒顔肩墢閳ь剨缍嗛崰妤呮偂閺囥垹绠规繛锝庡墮閻忣亪鎮樿箛銉х暤闁哄瞼鍠栭、娑橆潩椤掍焦顔掗柣搴ゎ潐濞叉粓宕伴弽顓溾偓浣割潨閳ь剟骞冨鍫濆耿婵☆垵宕靛Σ姗€姊婚崒姘偓椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁嶉柣鎰皺閺屽牓姊虹涵鍛【妞ゎ偅娲熼崺鈧い鎴ｆ硶缁愭梹顨ラ悙宸剶闁轰礁鍊块幊鐐哄Ψ閿旀儳寤烘繝鐢靛У椤旀牠宕板Δ鍛櫔闁荤喐绮嶅姗€藝閸楃倣锝夊箛椤戔晜妫冨畷銊╊敊閹冪缂傚倷鑳堕搹搴ㄥ储婵傜绠犻幖杈剧稻椤洟鏌ㄩ弬鍨挃缁炬儳鍚嬫穱濠囧Χ閸屾矮澹曟俊鐐€х粻鎾愁焽瑜旈幃楣冩倻閼恒儲娅滄繝銏ｅ煐钃遍柡鍜佸墮閳规垿鎮╃拠褍浼愰梺鍝ュ暱閺呯姴鐣烽悩璇插唨妞ゆ挾鍟块幏娲⒑閸︻収鐒炬繛瀵稿厴閸┿儲寰勯幇顓犲幘濠电偠灏褔鎮橀妷鈺傜厪闁搞儯鍔屾慨灞俱亜閿曗偓缂嶅﹪寮婚悢纰辨晩闁煎鍊楅悡鎾寸節绾版ê澧查柟顔煎€垮顐﹀磼濞戞凹娴勯柣搴秵閸嬫帡鍩€椤掍胶澧垫慨濠勭帛閹峰懘宕ㄦ繝鍐ㄥ壍缂傚倷绀侀ˇ鏉款焽濞嗘垵寮查梻浣瑰濞插秹宕戦幘璇茬闂侇剙绉甸悡娆撳级閸繂鈷旈柣锝堜含缁辨帡鍩€椤掑嫬绀嬫い鏍ㄧ〒閸樺崬鈹戦悩缁樻锭婵☆偅顨婇、鏃傗偓娑櫭肩换鍡涙煛鐏炶鍔欓柟鏌ョ畺閺岋紕浠﹂崜褉妲堝銈庡幑閸旀垵鐣峰鈧、鏃堝幢閺囩姷顦ㄥ┑鐘垫暩婵參骞忛崘顔肩闁割煈鍋嗛弶褰掓煟鎼淬埄鍟忛柛鐘冲哺瀵偅绻濆銉㈠亾娓氣偓瀵粙顢橀悢鍝勫汲婵＄偑鍊栭悧妤呮儗椤旂晫鐝?
    // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忕€广儱顦粈瀣亜閹扳晛鐒烘繛鍏兼礋濮婄粯鎷呴搹鐟扮闂佺粯顨嗛〃鍫ュ礆閹烘埈鍚嬮柛銉㈡櫇閻撴棃姊洪崨濠庢畼闁稿鍔欏銊︾鐎ｎ偆鍘遍梺闈涱槹閸ㄧ敻骞婅箛鎾存瘎缂?ASCII闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏鍓х暠閻庢艾顦伴妵鍕箳瀹ュ洤濡藉┑鐐叉噽婵兘鍩€椤掆偓缁犲秹宕曢崡鐐嶆稑鈽夐～顑藉亾?U+00A9(婵? 闂?U+00B7(闂? 闂傚倸鍊搁崐鐑芥倿閿曗偓椤啴宕归鍛姺闂佺鍕垫當缂佲偓婢舵劗鍙撻柛銉ｅ妽婵偐鎲搁悧鍫濈瑨妤犵偑鍨烘穱濠囶敍濮樿鲸鐧侀梺鍝勵儐閻燂妇鎹㈠┑瀣潊闁挎繂妫涢妴鎰版⒑閹稿孩纾搁柛濠冪箓椤曪綁寮婚妷銉ь啇婵炶揪绲藉﹢閬嶅储?
    {
        // 闂傚倸鍊搁崐鐑芥嚄閸撲礁鍨濇い鏍亼閳ь剙鍟村畷銊р偓娑櫭禍杈ㄧ節閻㈤潧孝闁稿妫濋幃?+ 闂傚倸鍊峰ù鍥х暦閸偅鍙忛柟鎯板Г閸ゅ嫰鏌涢锝嗙５闁逞屽墾缁犳挸鐣锋總鍛婃櫜闁告剬鍕綎闂傚倷绀侀幖顐ゆ偖椤愶箑纾块弶鍫氭櫇閻棝鏌涢埄鍐姇闁绘挻娲熼弻宥夊煛娴ｅ憡鐏撳┑鐐茬墛椤洭鎯€椤忓牆绠查柟浼存涧濞堫參姊虹€圭姵顥夋い锔炬暬閻涱喖顫滈埀顒€鐣烽敓鐘冲€烽柍杞版婢规洖鈹戦悙鏉戠劸婵炵厧娼￠崺鈧い鎴ｆ硶缁愭梹顨ラ悙宸剶闁轰礁鍊块幊鐐哄Ψ閿旀儳寤烘繝鐢靛У椤旀牠宕板Δ鍛櫇闁冲搫鎳庣壕濠氭煕閳╁啩绶遍柡浣稿€垮娲敆閳ь剛绮旈悽绋跨；闁绘ê纾粻楣冩煙鐎电鍓遍柣鎺楃畺閹顫濋銏犵ギ闂佸搫鏈ú鐔风暦閹偊妾梻濠庡墻閸撴岸骞堥妸锔剧瘈闁告粈鐒︾紞鍫ユ倵閸偅绶查悗姘緲閻ｇ兘顢曢敃鈧敮閻熸粌绻愰悾鐑藉醇閺囩啿鎷洪梺鐓庮潟閸婃洘绂掗敃鍌涚厱闁瑰瓨绻勭粔铏光偓瑙勬礃閸ㄨ泛顕ラ崟顒佸劅婵炴垶顨嗛惈蹇涙煟鎼达紕鐣柛搴ㄤ憾钘濇い鏍ㄧ矋閸忔粓鏌嶈閸撶喎顫忓ú顏勪紶闁告洦鍓欑粣娑㈡⒑閸濄儱孝闂佸府缍侀獮鍐倷鐎靛摜鐦堝┑顔斤供閸橀箖宕㈤幖浣光拺缂佸鍎婚～锕傛煕婵犲啯绀€妞ゆ洩绲块幑鍕Ω瑜忛敍婊堟煛婢跺﹦澧戦柛鏂挎捣缁鎮欓悜妯煎幍闂佹儳娴氶崑鍛焊闂堟耽褰掓偑閸涱垳鏆ら梺纭呮珪閻楃娀宕洪敓鐘茬闁宠桨鐒﹂?All rights reserved"
        // 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩顔瑰亾閸愵喖骞㈡俊鐐存礀閹碱偊鈥﹂妸鈺佸窛妞ゅ繐瀚竟鏇炩攽椤旂瓔鐒介柛妯犲喚鍤曟い鏃傜摂濞堜粙鏌ｉ幇顒佲枙闁稿孩姊归〃銉╂倷閸欏鏋犲銈冨灪閻╊垶寮幇鏉垮耿婵☆垳绮欢顐⑩攽閻樺灚鏆╅柛瀣仱瀹曞綊宕奸弴鐐殿唶闂佸憡绺块崕鎶芥儗閹剧粯鐓欑紓浣靛灩濞呮﹢鏌℃担闈╄含闁哄瞼鍠栭幃褔宕奸悢鍝勫殥闂備礁鎲￠悷銉╂晝椤忓牆钃熼柣鏃傚帶缁犳煡鏌熸导瀛樻锭婵炲牊鍔曢埞鎴︽倷闂堟稑浠橀梺缁樼墪閵堟悂鐛崘顓滀汗闁圭儤鍨归崐鐐烘煟閻樿崵绱版繛鍜冪稻缁傛帡寮堕幋顓熷瘜闂侀潧鐗嗗Λ娆戝婵犳碍鐓曟俊銈勭閳绘洟鏌?闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濇繝鍌滃闁稿绻濋弻鏇㈠醇濠垫劖效闂佺粯鎸搁崐鍦崲濠靛洨绡€闁稿本鍑规禒瀛樼箾鐎涙鐭嬬紒顔芥崌瀵鈽夐姀鈩冩珕闁荤姴娲﹁ぐ鍐不閾忓湱纾藉ù锝囨嚀婵牊銇勯妸銉﹀殗闁糕斂鍎插鍕箛椤掑偆鍟嬮柣搴ゎ潐濞叉牕鈻旈敃鍌涙櫜闁糕剝娲滅粻姘舵⒑閼测斁鎷￠柛鎿勭畵閹偤鎮欓鍙ョ盎闂佸搫娴傛禍鐐电矓椤曗偓閺屾盯鍩為幆褌澹曞┑锛勫亼閸婃牕顔忔繝姘；闁瑰墽绮悡鍐煟閻斿憡绶查柡鍡稻椤ㄣ儵鎮欓崣澶婃灎濡炪們鍨洪悷鈺呭箖閸撗傛勃闁绘劦鍓﹂崯?闂?
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
            // 闂傚倸鍊峰ù鍥敋瑜嶉湁闁绘垼妫勯弸渚€鏌熼梻瀵割槮闁稿被鍔庨幉鎼佸棘鐠恒劍娈鹃梺姹囧灩婢瑰﹪寮崶顒佺厽闁哄啫娲㈤埀顑藉亾闂佸搫顑嗛悧鐘诲箖濡ゅ懐宓侀柛顭戝枛婵酣鏌ｆ惔銏犲毈闁哥姵宀搁獮蹇氥亹閹烘垹鍘搁梺鍛婁緱閸犳牗绂掗鐐村仭婵犲﹤鍟撮崣鍕煙椤斻劌娲ら崡鎶芥煏韫囧﹥娅嗛柡鍜冪秮閹嘲顭ㄩ崨顓ф毉闁汇埄鍨辩敮鐐靛垝婵犳碍鏅查柛婊€鐒︾€靛矂鏌ｆ惔顖滅У濞存粍绮撻幆渚€濡搁妷鍐ㄧ秺閹剝鎯旈埦鈧崑鎾诲锤濡も偓閽冪喖鏌曟繛鍨姉婵℃彃鐗婄换娑㈠幢濡浚浜畷婊呯尵閹?婵犵數濮烽弫鎼佸磻閻愬樊鐒芥繛鍡樻尭鐟欙箓鎮楅敐搴′簽闁崇懓绉电换娑橆啅椤旇崵鐩庨梺鍛婁亢椤鎹㈠┑鍥╃瘈闁稿本纰嶉悘鎾绘⒑閸濄儱鏋庨柟铏锝夊醇閺囩偤鍞跺┑鐐村灦閿氭い顒€顦靛娲传閸曨剦妫為梺缁橆殕閹瑰洤顕?WARP 闂傚倸鍊峰ù鍥х暦閸偅鍙忛柣銏㈩焾缁€澶屾喐韫囨洖鍨濆┑鐘宠壘缁狅綁鏌ㄥ┑鍡楁殨婵顨婂铏圭矓閸℃顏存繛鍫燁焽閳ь剝顫夐幐椋庢濮樿泛绠栫憸鐗堝笒娴肩娀鏌涢弴銊ュ闁稿孩鎹囧?YAML闂?
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
        // winget 闂?闂傚倷娴囬褍霉閻戣棄绠犻柟鎹愵嚙缁犵喖姊介崶顒€桅闁圭増婢樼粈鍐┿亜閺冨洦顥夊ù鐘层偢濮婃椽妫冨ù銈嗙洴瀹曠喖顢曢敐鍡欏胶闂傚倸鍊风粈渚€骞夐埄鍐懝婵°倕鎳庣粈鍫熺箾閹存瑥鐏柛瀣у墲閵囧嫰骞掗幋婵愪痪闂佺粯鎸婚惄顖炲蓟濞戙垹绠涢柛蹇撴憸閻╁酣鏌″浣插亾閺傘儲鏂€闂佺粯鍔栧娆戝緤缂佹绠鹃悘蹇旂墤閸嬫捇骞囨担鍛婎吙?闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濇繝鍌滃闁藉啰鍠栭弻鏇熺箾閸喖澹勫┑鐐叉▕娴滄粓宕橀埀顒€顪冮妶鍡樺暗闁革絻鍎辫灋?0闂?1978335189闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熷▎鈥崇湴閸旀垿宕洪埀顒併亜閹烘垵鈧崵澹曟總绋跨骇闁割偅绋戞俊璺ㄧ磼閻橀潧浠遍柡宀嬬磿娴狅箓鎳為妷銉︾亷闂備浇妗ㄩ悞锕傚礉濞嗗繒鏆﹂柣鏃傗拡閺佸洭鏌ｅΟ鍝勬毐闁哄鎹囧濠氬磼濞嗘埈妲梺鍦拡閸嬪棝鍩€椤掍焦鐨戦柛蹇斆悾椋庢喆閸曨偒鍤ら梺鍝勵槹閸わ箓鏁冮崒娑氬幍闁诲海鏁搁…鍫熺墡闂佽瀛╅懝楣冣€﹂悜钘夎摕闁哄洨鍠庣欢鐐寸箾閹寸偠澹橀柣顓熺箞濮?
        // 婵犵數濮烽弫鎼佸磻濞戙埄鏁嬫い鎾跺枑閸欏繐霉閸忓吋缍戠痪鎯ф健濮婃椽顢楅埀顒傜矙娓氣偓瀵偊宕堕埡鍌氭瀾闂佺粯顨呴悧鍡欑箔閹烘梻纾奸柣娆忔噽閸掔増銇勯鍕殶闁逞屽墾缂嶅棙绂嶅鍫熷剨闁靛鍎嶈ぐ鎺撳亗閹肩补妾ч崑鎾诲锤濡も偓閽冪喖鏌ㄩ悢鍝勑㈢紒鈧€ｎ喗鈷掗柛顐ゅ枔閵嗘帒霉閻橆偅娅婃慨濠呮閹风娀鍨惧畷鍥︽埛缂傚倷娴囬崺鏍х暆閹间胶宓侀柛鈩冪☉閻愬﹥銇勯幒宥囶槮濞存粍绮撳铏圭磼濡櫣浠稿銈庡幖濞差厼顕ｉ幎鑺ュ亹缂備焦菤閹锋椽姊洪崨濠勨槈闁挎洏鍊濋幃妯绘綇閵娧咁啎闂佸憡绮岄崯鎵矓椤曗偓閺岋絽鈽夐崡鐐寸彎閻庤娲栭悥鍏间繆濮濆矈妲烽梺鍝勵儐閻楃姴顫忓ú顏勫窛濠电姴瀚悾鐢告煟鎼淬垹鍤柛鎾寸懇閸┿垹顓奸崨顏呯€婚梺瑙勫劤椤曨參宕ｉ崱妞绘斀闁绘劖娼欓悘锔姐亜椤撶偞鍠橀柟顔惧仱閹粓鎳為妷褍甯楅柣鐔哥矋缁挸鐣峰鍐ｆ瀻闁归偊鍣ｅ顕€姊洪崫鍕殭闁稿﹨宕电划濠氬棘濞嗗墽鍞甸梺鍏兼倐濞佳勬叏閸ヮ剚鐓曟繛鍡楅獜閺€濠氭煏閸パ冾伃鐎殿喕绮欐俊鎼佸Ω閵夘喗顥ゅ┑鐘垫暩閸嬬偤宕甸鍫濈倞闁靛ě鍐ㄧ濠碉紕鍋戦崐鏇犳崲閹邦喒鍋撳鍗炲幋鐎规洘娲熷顕€宕奸悢鍝勫笚闁荤喐绮嶇划鎾崇暦濡も偓椤粓鍩€椤掆偓閻ｇ兘骞嬮敃鈧粻鑽ょ磽娴ｈ鐒介柛姗€浜跺铏规兜閸涱喖娑х紓浣圭叀缁犳牠銆侀弮鍫熷亹缂備焦顭囬崢浠嬫⒑閸濆嫮袪闁告柨閰ｉ幆宀勫箳閹存梹鏂€?
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
            // 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧壕濠氭煕閳╁啰鈽夐柛灞诲姂閺屻倕霉鐎ｎ偅鐝曞銈庡亝濞茬喖寮婚悢琛″亾閻㈡鐒惧ù鐘欏洦鐓冪紓浣股戦埛鎺楁煃瑜滈崜娆戠不瀹ュ纾块梺顒€绉寸粻鐘诲箹缁顎嗘俊鎻掔墢閹叉悂寮崼婵婃憰?闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鎻掔€梺姹囧灮椤牏绮荤憴鍕╀簻闁规壋鏅涢崝妯好瑰鍡╂綈缂佺粯鐩獮瀣倷閸愨晛鍝虹€殿喖顭烽弫鍐磼濞戞艾甯楅梻渚€娼чˇ顓㈠礉瀹€鍕剭婵炲樊浜濋悡?闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熼悜妯虹仼闁哄棗妫濋弻鐔兼⒒鐎靛壊妲弶鈺傜箞閺岀喖宕楅懖鈺傛闂佸憡鏌ㄩ惉鑲╁垝婵犳艾唯闁冲搫鍊搁埀顒傛暬閺屻劌鈹戦崱娑扁偓妤呮煕濡粯灏﹂柡灞界Х椤т線鏌涢幘缈犱孩闁圭瓔鍋嗙槐鎾存媴濮濆苯顏悗瑙勬处閸撶喖宕洪悙鍝勭闁挎洍鍋撻柣鎰躬閺屾洘绻濊箛鏇犳殸濡炪倕绻嬮崡鍐差潖?DoH/nginx 闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濇繝鍌滃闁哄拋浜濈换婵嬫偨闂堟刀娑㈡煕鐎ｎ偅灏伴柕鍥у缁犳盯骞樼捄渚澑闂備礁鎼幏瀣礈閻旂厧绠栨慨妞诲亾闁诡喗鐟╅幊婊冣枔閹稿寒妫勯梻鍌欒兌缁垳鏁幒妤€围闁归棿绀佺粻鏍偡濞嗗繐顏┑顖涙尦閺屾稑鈻庤箛锝喰ч梺缁樼箖閻楃姴顫忛悜妯诲濞寸厧鐡ㄩ鏍⒑缁嬪尅鏀荤紒璇茬墦閹即顢欓柨顖氫壕闁挎繂楠搁弸鐔兼煙鐏炲倸鍔﹂柡灞剧〒娴狅箓宕滆閸ｎ噣姊婚崒姘兼Ц缂佸鏁搁幑銏犫槈閵忕姷顓洪梺缁樺姈濞兼瑩寮冲Δ鍛拺缂佸娉曢弰鍌炴煕婵犲啰绠撴い鏇稻缁绘繂顫濋鍌氣偓鐐烘⒑閸愬弶鎯堥柛鐔跺嵆閺佸秹鎮欓悜妯锋嫼闂佽崵鍠愭竟鍡涘箺閻樼粯鐓熼柣鏃€娼欓悘鑼偓?
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


// 濠电姷鏁告慨鐑姐€傞挊澹╋綁宕ㄩ弶鎴狅紱闂侀€炲苯澧撮柡灞剧〒閳ь剨缍嗛崑鍛暦瀹€鍕厸鐎光偓鐎ｎ剛锛熸繛瀵稿婵″洭骞忛悩璇茬闁圭儤鍩堝銉モ攽閻樺灚鏆╅柛瀣仩閵囨劙宕橀…鎴犲姺闂佺偨鍎查崜姘跺吹閺囩喆浜滈柟鐑樺灥閳ь剙缍婂鍐差煥閸曗晙绨婚梺鍝勫€搁悘婵嬪煕閺冨牊鐓熸繛鎴炵墪閸旓箓鏌?sniroute.cmd update check闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏鍓х暠閻庢艾顦伴妵鍕箳瀹ュ棭妯傜紓浣筋嚙濡繈寮诲☉妯锋婵鐗婇弫鍓х磽閸屾氨孝婵炲樊鍙冨濠氭偄閻撳海鐣鹃悷婊冪Ч瀵櫕娼忛妸锕€寮块梺鎸庣箓閹峰螣閳ь剟姊虹拠鈥虫灈缂傚秴锕悰顕€宕堕澶嬫櫍闂佺粯妫佸▍锝夊煝韫囨稒鈷掑ù锝堟閵嗗﹪鏌￠崨顔炬创闁瑰磭鍠栭、娑㈡倻閸モ晝鈼ゆ繝鐢靛█濞佳囶敄閸涱垳灏电€广儱顦伴悡鍐喐濠婂牆绀堥柣鏃堫棑閺嗭妇鈧懓瀚崳鏉懨洪宥嗘櫆闂佸憡渚楅崹鎵暜閹剧粯鈷戦悹鍥皺缁犳娊鏌涚€ｎ剙鏋涚€规洘娲熼幃鐣岀矙鐠侯煉绱?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭紒瀣紩濞差亝鍋愰悹鍥皺閿涙盯姊洪悷鏉库挃缂侇噮鍨跺畷鎴︽晸閻樺磭鍘搁梺鎼炲劘閸斿酣銆傞懠顒傜＜濞达絽鎼。濂告煏閸パ冾伃鐎殿噮鍣ｉ崺鈧い鎺戝閸嬪鏌ｅΟ鐑樷枙婵?GitHub API闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏楣冩闁稿顑夐悡顐﹀炊閵娿儱鏋欑紓鍌氱У閻楃娀寮诲☉銏犵労闁告劦浜栧Σ鍫ユ煟鎼淬垼澹樻い锔诲灦閳ワ妇鎹勯妸锕€纾繛鎾村嚬閸ㄨ鲸顨欏┑鐘垫暩閸嬫盯藝椤撱垹鍨傚ù鐘差儑瀹撲線鏌″搴′簮闁稿鎸搁埥澶娾枍閾忣偄鐏ラ柣锝囧厴瀹曠喖顢栭崣銉х泿闂備礁鍟块幖顐︻敄閸涙番鈧懘顢楅崟顒傚幍閻庣懓瀚伴崑濠囧磿閺傚簱鏀介柍銉ョ－閸╋絿鈧娲栭妶鍛婃叏閳ь剟鏌嶉挊澶嬵棡缂傚秴鐭傚濠氬磼濞嗘垵濡介柣搴ｇ懗閸涱垳鐓撻梺瑙勵偧鐏炲倸濮傞柛鈹惧亾濡炪倖甯掗崐鑽ゅ婵傜绾ч柛顐ｇ☉婵¤法绱掗悩闈涗沪妞ゃ劊鍎甸幃娆撴嚑椤戭値鍥ㄧ厱闁宠鍎虫禍鐐繆閻愵亜鈧牜鏁繝鍥ㄥ殑闁割偅娲忛埀顒€鎳橀幃婊堟嚍閵壯冨妇濠电姰鍨奸崺鏍儎椤栨凹鐎舵い鏇楀亾闁哄备鍓濆鍕節閸曨厜銊╂⒑閸濆嫭婀扮紒瀣灴閿濈偛鈹戠€ｅ灚鏅㈤梺鍛婃礈椤ｄ粙宕戦幘婢勬棃宕橀埡浣圭€鹃梻浣告惈椤︿即顢栧▎鎾冲惞闁绘柨鍚嬮悡鐔兼煏婢舵ê鏋欑紒銊ょ矙閺岀喖鎮烽弶娆句純婵犵鍓濋幃鍌涙叏閳ь剟鏌ㄥ☉妯侯仼妤犵偞锕㈠濠氬磼濞嗘垵濡介柣搴ｇ懗閸涱垳鐓撻梺鐟邦嚟閸嬬喖姊介崟顖涚厸闁告劑鍔岄埀顒冨吹缁寮▎鍓у數闂佸吋鎮傚褎鎱ㄩ崼銉︾厱闁靛牆鎳庨顓㈡煛瀹€瀣暠閾伙絽銆掑鐓庣仭缁楁垶淇婇妶鍥ラ柛瀣灴瀹曠銇愰幒鎾剁暢闂傚倷鐒︾€笛呮崲閸岀偛绠犻柟閭﹀枤娑撳秹鏌熺紒妯轰刊闁绘柨妫涢幉绋款煥閸繄鐓戦梺鍛婂姦閸犳牜绮诲鑸电厱闁哄洢鍔岄埀顒傚█閹垽宕崟顐殭闂備礁鎼ú銊╁磻濡崵鐝?
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
        // 濠电姷鏁告慨鐢割敊閺嶎厼绐楁俊銈呭閹冲本绻濆▓鍨灈闁挎洏鍊楃槐鐐寸節閸屾粍娈鹃悗鐟板閸ｆ潙煤椤忓秵鏅滈梺鍛婃处閸樺ジ骞楃€ｎ喗鈷掑ù锝呮啞閸熺偤鏌ｉ悤鍌滅М鐎规洘娲熼獮搴ㄦ嚍閵夈儰鐢绘繝鐢靛█濞佳兾涚仦杞匡綁宕奸妷锔惧幍闂佺粯鍔﹂崜娆愭櫠椤栫偞鐓曢幖杈剧磿缁犳煡鏌曢崶褍顏柡浣稿暞閹峰懘宕烽婧惧亾婵犲洦鈷戦悹鍥ｂ偓铏亶濠电姰鍨洪敃銏ゅ箖閹€鏀介柛顐ｈ壘娴滈箖鏌ㄥ┑鍡涱€楀褌鍗抽弻锝呪槈閺嵮冨Е濠殿喖锕ュ钘夌暦閵婏妇绡€闁告洦鍓欏▍褍鈹戦悙鑼憼缂侇喖绉归幃鐤樄妤犵偞鍨挎慨鈧柣娆屽亾婵炲皷鏅犻弻銈夊箹娴ｈ閿繛瀵稿Х閸樠囧煘閹达附鍊烽柛娆忣樈濡偟绱撴担铏瑰笡闁告梹鐟ㄩ惌楣冩⒑閸涘﹣绶遍柛妯碱焾鍗遍柛顐犲劜閻撴瑩鏌熼鍡楀暢閸╃偤姊洪柅鐐茶嫰婢ь垶鏌ｉ埡濠傜仸濠碘€崇摠缁楃喖鍩€椤掑嫮宓侀悗锝庡枟閺呮繈鏌嶈閸撴盯鍩€椤掍浇澹樻い锔诲灦閳ワ妇鎹勯妸锕€纾繛鎾村嚬閸ㄤ即宕滈崡鐐╂斀闁绘劕寮堕崳鍦磼缂佹﹫鑰跨€殿喛顕ч埥澶愬閻樼數鏉告俊鐐€栫敮濠勭矆娴ｇ硶鏋嶉柨婵嗘媼濞撳鏌曢崼婵囶棡闁抽攱鍔欓弻娑㈠籍閳ь剟鎮烽敂鐣屸攳?
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


// ---------------------------------------------------------------- GHA 濠电偞鍨堕幐鎼侇敄閸緷褰掑炊閳规儳浜鹃柣銏☆問閺€浼存煟閿曚焦顥夋い?
// 闂備礁鎼鍕嚄閸洘鍋╂繝闈涙处缂嶅洭鏌熼幆褍鏆辩€殿喛娅曟穱濠偽旈埀顒€螞鐎靛憡顫曢柟娈垮枤绾句粙鏌″搴″閻㈩垰妫欐穱?+ 缂備胶铏庨崣搴ㄥ窗閺嶎厽鍋?闂備礁鎲￠悷锕傛偋濡ゅ啰鐭撻柣鎴ｅГ閺咁剛鈧厜鍋撻柍褜鍓熷畷娆撴嚍閵壯屾祫闂佹寧绻傚ú銊╁垂閹惰姤鐓曢柟鐑樻尵閳藉绱撻崒娑欏磳闁轰礁绉瑰畷鐓庮煥閸涱垱鍠掗梻鍌氬€稿ú鐘诲磻閹剧粯鍋?.rc 闂佽崵濮嶉崘顭戜痪缂備緡鐓堥崳锝夊极瀹ュ棗绶為柛顐ｈ壘娴?
// Enter = 缂備胶铏庨崣搴ㄥ窗閺嶎厽鍋╁Δ锝呭暞閺咁剟鎮楀☉妯煎€癱 = 闂備礁鎲￠悷锕傛偋濡ゅ啰鐭撻柣鎴ｆ杩?
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

// 闂備礁鎲￠懝鐐殽濮濆被浜归悗闈涙啞瀹?gha fetch闂備焦瀵х粙鎴﹀嫉椤掍椒绻嗛柟闂寸鐎氬鏌嶈閸撶喎鐣烽妷锔藉劅婵犻潧鐗滄导鍥煟閻愬鈽夐柣顓濊兌濞嗐垽顢橀姀鐘殿槱婵°倧绠戦悘婵嬪垂闁秵鐓曢柟鐑樻煥椤ｅジ鏌ｉ妸鈺傛锭闁崇粯姊归獮濠囨煃?
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
    // WARP 闂傚倸鍊搁崐鐑芥嚄閸洍鈧箓宕奸妷顔芥櫈闂佸憡鍔﹂崰鏍閸ф鈷戞い鎺嗗亾缂佸顕划濠氬冀椤愮喎浜炬鐐茬仢閸旀岸鏌熼崣澹濐亪鍩ユ径鎰潊闁绘﹢娼ф慨锔戒繆閻愵亜鈧牜鏁幒鏂哄亾濮樼厧澧撮柟閿嬪灴閹垽宕楅懖鈺佸妇濠电姰鍨奸崺鏍晪闂佽楠忕徊璺ㄦ?0 缂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾閽樻繈鏌熷▓鍨灍闁哄棙绮岄埞鎴︽偐閹绘帩浼€闂佸憡顨嗗畝鎼佸蓟閳╁啫绶為悗锝庝簽娴滃爼姊洪崫鍕靛剰闂佸府缍佸璇差吋婢跺鐧勬繝銏ｆ硾椤戝懎鈻嶈缁辨挻鎷呴幓鎺嶅濠电姰鍨煎▔娑⑩€﹂鍕亜闁糕剝绋掗悡娆愩亜閺嶃劎鈽夋い銉ヮ儑缁辨帡濡搁妷锕€浠撮梺璇″枛閸㈡煡鍩㈡惔鈾€鏋庨柟鐐儗閳ь剙绉剁槐鎾存媴閸撴彃鍓板銈忕畱閻忔碍绌辨繝鍥ч唶闁哄洨鍋涢懓鍧楁⒑鐎圭姵銆冩俊鐐村笚濞艰顫滈埀顒€顫忕紒妯诲闁告縿鍎虫婵犵數鍋橀崠鐘诲川椤栨粎鍘犻柣鐔哥矌婢ф鏁幒妤佸€块柛顭戝亖娴滄粓鏌熼崫鍕ら柛鏂跨Ч閺岋綁濡搁敃鈧悘鎾煛瀹€瀣М妤犵偞鐟╁畷鐔碱敇閻樺灚顫屾繝?
    // 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鑼槱閻熸粎澧楃敮鎺楀垂閸岀偞鐓熸俊銈傚亾闁绘锕畷锝堢疀濞戞瑧鍘撻梺鍛婄箓鐎氼參宕抽崷顓犵＜妞ゆ劑鍨绘晥闂佽鍠栫紞濠傜暦椤愶箑绀嬫い鎺嶇閸撶敻姊哄Ч鍥х労闁割煈鍓熷畷鏇㈡偨缁嬭儻鎽曢梺缁樻⒒閸庛倕鐣锋径鎰厽婵﹩鍘界紞鎴澝归崗鐓庡缂佽鲸鎹囧畷鎺戭潩濮ｆ瑣鍨洪妵鍕箣濠垫劖笑闂佷紮绲介崲鏌モ€﹂妸鈺佺闁诡垎鍕偓顖炴⒒娴ｈ鍋犻柛搴㈡そ閹ê鈹戠€ｎ偅娅囬梺绋挎湰缁嬫捇宕㈤棃娑辨富闁靛牆妫楅悘锕傛倵缁楁稑鎳愰惌鍫ユ煟濡も偓閻楀嫭绂嶅鍫熺厸闁稿本绋戦婊冾熆鐠哄搫顏紒杈ㄥ笧閳ь剨缍嗘禍璺何ｅú顏呯厓鐟滄粓宕滈妸褏绀婇柛鈩冪☉绾惧鏌涘☉妯兼憼闁抽攱姊婚埀顒€绠嶉崕閬嵥囨导瀛樺亗闁靛濡囩粻楣冩煙鐎电浠ч柟鍐叉椤法鎲撮崟鎯扳偓鍧楁煛瀹€鈧崰鏍€佸☉妯锋瀻闁瑰濮峰畷鏌ユ煟?闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鑼槱閻熸粎澧楃敮鎺楀垂閸岀偞鐓熸俊銈傚亾闁绘锕畷锝堢疀濞戞瑧鍘撻梺鍛婄箓鐎氼參宕抽崷顓犵＜妞ゆ劑鍨绘晥闂佽鍠栫紞濠傜暦椤愶箑绀嬫い鎺嶇閸撶敻姊绘担瑙勫仩闁稿﹥鐗滈弫顕€鍨鹃幇浣告闂佸壊鍋呭ú鏍偂濞戙垺鐓曢柟鎵虫櫅婵＄儤绻涘畝濠侀偗闁哄矉缍佹慨鈧柍杞拌兌娴狀參姊虹憴鍕憙婵炲懏娲樼粚杈ㄧ節閸曘劌浜炬繛鎴炵懐閻掍粙鏌ｉ悢椋庣Ш闁哄苯绉烽¨渚€鏌涢幘瀵告噰闁糕斂鍨介獮妯兼嫚瀹割喗缍傞梻渚€娼ч悧鍡涘箠閹捐鏉洪梻鍌欒兌閹虫捇宕ョ€ｎ喗鍋╂い蹇撶墕閸ㄥ倹绻涘顔荤凹闁稿﹤娼￠弻锝夋晲閸涱収妲甸梺绋款儐閹告悂鍩㈤幘璇插瀭妞ゆ梻鏅禍鎰版⒒?闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濇繝鍌滃闁稿绱曢幉鎼佸籍閸喐娅滈梺缁樺姇閹碱偆绮婚幎鑺ョ厵闁绘劦鍓氱紞鎴炪亜椤愮啿鍋撻幇浣瑰瘜闂侀潧鐗嗗Λ妤冪箔閸屾粎纾奸柍褜鍓氶幏鍛喆閸曨偄濡冲┑鐘垫暩婵瓨瀵奸崨鑸汗闁圭儤鍨归ˇ褔姊婚崒姘偓鑽ゆ崲濠靛牃鍋撳☉鎺撴珔妞ゎ亜鍟存俊鑸垫償閳ユ彃顥氱紓鍌欒兌婵敻宕归崸妤€鐏抽柨鏃堟暜閸嬫捇宕烽鐐愩儵鏌ｉ妶鍌氫壕闂傚倷绀侀幖顐﹀磹閽樺鏋栭柡鍥ュ焺閺?
    // 闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏楣冩闁稿顑夐悡顐﹀炊閵娿倗绻侀梺杞扮鐎氫即寮诲☉銏犖ㄩ柨婵嗘噹椤姊哄畷鍥╁笡婵☆偄鍟村璇差吋閸偅顎囬梻浣告啞閹稿鎮烽埡鍛槬闁靛繆鍓濋崕鐔兼煏婵炲灝鍔ら柛鏃€鎸冲娲川婵犲倸顫庨梺绋款儐閹瑰洭宕哄☉銏犲嵆闁靛繆妾ч幏娲⒑閸涘﹦绠撻悗姘煎灦閿濈偤鏁冮崒娑氬幈闁硅偐琛ラ埀顒冩珪濮ｆ劙姊洪幐搴㈢８闁搞劏妫勯悾鐑藉Ω閳轰胶楠囬梺鐟邦嚟婵厼危閹绢喗鈷掑ù锝囩摂濞兼劖銇勯弮鈧悧鐘茬暦閺夎鏃堝川椤旈棿鎮ｉ梻鍌欑贰閸撴瑧绮旈悽鍛婂亗婵炴垯鍨洪悡鏇㈡煛閸ャ儱鐏繛鎳峰洦鐓熼煫鍥ュ劤缁♀偓闂佸搫鐭夌槐鏇熺閿曞倹鍤嶉柕澶涢檮閸婇攱淇婇悙顏勨偓鏍洪埡鍐濞撴埃鍋撶€规洘妞介崺鈧い鎺嶉檷娴滄粓鏌熼悜妯虹仴妞ゅ浚浜弻锟犲椽娴ｇ顫梺閫炲苯澧柛妯荤矒瀹曟垿骞樼紒妯煎幈闂佽婢樻晶搴ㄥ礆閺夋５鐟邦煥閸垻鏆梺璇″枟閿曘垽骞冨▎鎴炲磯閺夌偟澧楅惈蹇涙⒒娴ｈ棄鍚归柛鐘冲姍瀹曟繈寮撮姀鐘靛弨婵犮垼鍩栭崝鏇犵不缂佹ǜ浜滈柡宥冨妿閻擃垰顭跨憴鍕缂佺粯绻堥幃浠嬫濞戞鎹曟繝纰樻閸嬪嫰顢氶鐐嶏綁骞囬弶鍨祮闂佺粯姊婚…鍫濐嚕閸ф鈷戠憸鐗堝笚閿涚喓绱掗埀顒佹媴閾忛€涚瑝闂佺懓鐡ㄧ欢鐤亹閹烘挻娅滈梺鎼炲劘閸斿秹鎮甸鍡欑＝濞达絽鎼暩闂佺绻戦敃銏狀嚕婵犳碍鏅搁柣妯垮皺椤︿即姊婚崒姘卞缂佸鎸荤粋鎺楊敇閻愨晜鏂€闂佺粯锕╅崑鍕閹屾富闁靛牆妫欑亸顏堟煕閵婏附銇濈€规洘娲栬灃闁告侗鍠氶崢顏呯節閵忥絽鐓愰柨姘舵煟韫囥儳鎮肩紒杈ㄥ浮閸┾偓妞ゆ帊绀佺欢鐐烘煙闁箑骞橀柛姗€浜跺娲偡閻楀牊鍎撳銈嗗灥閹虫﹢宕洪埀顒併亜閹哄秶鍔嶇紒鈧€ｎ喗鐓欐い鏇楀亾缂佺姵鐗犻獮鍐ㄢ枎閹垮啯鏅㈤梺绋挎湰椤ㄥ棝鎮橀崘顔界厽閹艰揪绱曢悾顓㈡煕鎼淬劋鎲炬鐐诧攻閹棃鍩?婵犵數濮烽弫鎼佸磻濞戙埄鏁嬫い鎾跺枑閸欏繘鏌℃径瀣劸婵炲皷鏅犻弻銊╁籍閸屾矮澹曢悗瑙勬礀瀵墎鎹㈠☉銏犵婵炲棗绻掓禒鑲╃磽娴ｅ搫校濠电偛锕濠氬即閻旈绐為梺鍓插亝缁诲倹鎱ㄦ惔銊︹拺缂備焦蓱鐏忣參鏌涙繝鍌涘仴鐎殿喖顭烽弫鎾绘偐閼碱剨绱查梻渚€娼ч悧鍡涘箠鎼达絿涓嶉柛锔诲幘绾?闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熷▎陇顕уú顓€佸鈧慨鈧柣姗€娼ф慨?
    // 濠电姷鏁告慨鐢割敊閺嶎厼闂い鏍ㄧ矊缁躲倝鏌ｉ敐鍛拱鐎规洘鐓￠弻鐔告綇閹呮В闂佽桨绀侀崯顐﹀煘閹达箑纾兼繝濠傛捣閸旀悂鏌ｉ悢鍝ユ嚂闂傚嫬瀚版俊鐢稿礋椤栨艾鍞ㄩ梺闈涱焾閸斿矂藟濠靛鍋犳慨妯哄船閺嬫稒鎱ㄦ繝鍕笡缂佹鍠栧畷鎯邦槼闁诲繐鐗撳娲焻閻愯尪瀚板褌鍗抽弻娑㈡偄瀹勯偊鍔夐悗鍨緲鐎氼喗绂掗敃鍌氱畾鐟滃繘鏁嶈箛娑欌拻濞达絿鍎ら崵鈧梺瀹︽澘濡块柛鎺撳笚閹棃鈥﹂幋鐐存珕闂佸搫顦悧鍕礉鎼达絿涓?up闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熺€电浠ч梻鍕閺岋繝宕橀妸銉㈠亾閸濄儲顐?*闂傚倷娴囬褍霉閻戣棄纾婚柨婵嗩槸绾惧綊鏌″搴″箹缁炬儳娼￠弻銈吤圭€ｎ偅鐝曢悗?12 缂?*闂?
    // first-run-setup 闂傚倸鍊搁崐鐑芥嚄閸洖鍌ㄧ憸鏃堝Υ閸愨晜鍎熼柕蹇嬪焺濞茬鈹戦悩璇у伐闁瑰啿绻愰悾鐑藉箵閹规缍婇弫鎰板川椤旇法顢呴梻浣哥－缁垰煤閻旂厧钃熼柨婵嗩槸濡﹢鏌涢…鎴濇珮闁哄棎鍨荤槐鎾存媴閻熸澘顫嶅銈冨妼閿曨亜顕ｆ繝姘亜濡炲瀛╁▓婵嬫⒒娓氬洤寮块梻鍕閻″繘姊婚崒娆戭槮婵犫偓閸楃偐鏋嶉柕蹇嬪灮缁€濠偯归敐鍛棌闁搞倖娲熷濠氬醇閻斿墎绻侀梺鍛婅壘椤戝寮诲☉妯锋闁告鍋涚€涳絽鈹戦悙鎻掓倯濠电偛锕ら～蹇撁洪鍕唶闁硅壈鎻徊鍝勎ｉ崼銉︹拺婵炶尪顕ф晶顕€鏌曢崼鐔稿€愭鐐插暣濡啫鈽夐幒鎿冩綌闂備線娼х换鎺撴叏閻戝壙鍥敃閿旂晫鍘介梺缁樏鑸靛緞閸曨厽鍠愰柡澶婄仢閺嗭絿鈧鍠楄ぐ鍐煝鎼淬劌绠婚柡澶嬪灣閸?up 婵犵數濮烽弫鎼佸磻閻愬樊鐒芥繛鍡樻尭鐟欙箓鎮楅敐搴℃灍闁搞倕鑻灃闁挎繂鎳庨弳娆戠磼閻欌偓閸ㄥ爼寮婚悢灏佹灁闁割煈鍠楅悘鈧梻浣侯焾椤戝棝骞戦崶顒€绠栭柣鎴ｆ缁狙囨煙缁嬪灝顒㈡い鏂款槸閳规垿鎮╅幇浣告櫛闂佸摜濮电喊宥囨崲濞戙垹鐒垫い鎺戝閻撴稑霉閿濆懏鎲搁弫鍫ユ倵鐟欏嫭绀堥柛妯犲洤鐓橀柟杈剧畱闁卞洭鏌ｉ弬鎸庢喐闂佽姤甯掗埞鎴︽偐閸偅姣勯梺绋款儑閸嬫盯鈥﹂崶褉鏋庨柟鍨暞閺呯偤姊洪崨濠勨姇婵炲吋鐟х划濠氬冀閵娿倗绠氶梺闈涚墕閹冲酣寮抽悢鍏肩厱婵°倓鑳堕。鏌ユ煏閸パ冾伃鐎殿喕绮欐俊姝岊槾妞ゆ挾鍘ч埞鎴︽倷鐠轰警妫堝┑鐐差槹閻╊垰锕㈡担绯曟斀闁绘顕滃銉╂倵濮樼厧澧扮紒顕呭弨椤﹀磭绱?
    // 闂傚倸鍊搁崐鐑芥嚄閸洍鈧箓宕奸妷顔芥櫈闂佺硶鍓濋悷銉╁垂濠靛牃鍋撻獮鍨姎婵炶绠撳鍐差煥閸喓鍘撻悷婊勭矒瀹曟粓鎮㈤悡搴ゆ憰闂侀潧顭堥崕顔嘉ｉ崼鐔剁箚妞ゆ牗绻傛禍鐟懊归悡搴㈠窛缂佽鲸鎸荤粭鐔煎炊瑜庨悘渚€姊洪崨濠冣拹闁搞劎鏁婚、?5454 濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴炵懃缁剁偤鏌涢妷顔煎闁绘帒鐏氶妵鍕箳閸℃ぞ澹曢柣搴㈩問閸犳牠鎮ユ總绋跨畾闁搞儯鍔庨々鐑芥倵閿濆骸浜為柛妯哄船閳规垿顢欑粵瀣姺闂佸憡顭嗛崶褍鍋嶉梺姹囧灩閹诧繝鎮?444 闂傚倸鍊风粈渚€骞栭位鍥敃閿曗偓閻ょ偓绻濇繝鍌滃闁藉啰鍠栭弻鏇熺箾閸喖澹?502闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熷▎陇顕уú顓€佸鈧慨鈧柣姗€娼ф慨锔戒繆閻愵亜鈧牕顔忔繝姘；闁瑰墽绮悡鏇炩攽閻樻彃浜為柣鎾炽偢閺岀喖顢欓懞銉ｄ虎濡ょ姷鍋涢澶愬箖閳哄拋鏁冩い顐幖椤ユ岸姊绘担钘夊惞闁哥姵鍔曢…鍨潨閳ь剙顕ｉ幎鑺ュ亜闁惧繐婀遍敍婊冾渻閵堝棙鐓ラ柟纰卞亝閹便劑宕掑┃鎯т壕婵炲牆鐏濋弸娑㈡煟閺嵮佸仮妤犵偛鍟存慨鈧柕鍫濇閸欏棝鏌ｆ惔顖滅У濞存粍鐗犲畷鎴﹀箻缂佹ê浠洪梺鍛婄☉閿曪箓宕㈤柆宥嗏拺鐟滅増甯掓禍浼存煕閹惧鈽夐柡鍡忔櫊濮婃椽鎳為妷顔惧姺闂佸摜濮撮柊锝夊春閵夛箑绶為柟閭﹀墰椤旀帒顪冮妶鍡欏闁活収鍠楃粩鐔煎即閻旇櫣鐦堥梺姹囧灲濞佳勭濠婂嫪绻嗘い鎰╁灮閻掓悂鏌涢埞鎯т壕婵＄偑鍊栫敮鎺楀疮椤栫偞鍋熸い蹇撶墛閻撶喖鏌曟繝蹇曠暠闁诲繆鏅濈槐鎺撴綇閵娿儳顑傞梺閫炲苯澧剧紓宥呮缁傚秴顭ㄩ崼婵囪緢闂備緡鍓欑粔鐢告偂閻斿吋鐓ユ繝闈涙閹兼劕霉濠婂啫鈷旂紒杈ㄥ浮瀹曟帒顫濆В娅诲應鍋撶憴鍕濞存粌鐖奸悰顕€宕堕鈧儫闂侀潧顦崐鏇⑺?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 婵犵數濮甸鏍窗濡ゅ啯鏆滄俊銈呭暟閻瑩鏌熼悜妯镐粶闁逞屽墾缁犳挸鐣锋總绋课ㄦい鏃囧Г濞呭秴鈹戦悩鍨毄濠殿喚鏁婚幊婵嬪礈瑜忛惌鍡涙煕瀹€鈧崑鐐烘偂濞嗘挻鐓欐い鏍ㄧ⊕缁惰尙鎮鑸碘拺闁圭娴烽埊鏇熶繆椤愩垹鏆ｇ€殿喖顭锋俊鎼佸Ψ閵忊槅娼旀繝鐢靛仜濡瑩宕濆Δ鈧埢鎾诲Χ婢跺鎷洪柡澶屽仒閸楀啿顬婇悜鑺ョ厱濠电姴鍟扮粻鐐烘煟濞戝崬鏋ら柍褜鍓ㄧ紞鍡涘窗濡ゅ懏鍋傞柕澶嗘櫆閸婄敻鏌ㄥ┑鍡涱€楀褜鍨堕弻宥夋煥鐎ｎ偆顦ㄧ紓浣虹帛閻╊垶鐛€ｎ喗鍊烽柡澶嬪灩閿涘淇婇悙顏勨偓鏍р枖閿曞倸绀嬫い鎺嗗亾鐎殿喖娼″娲礃閸欏鍎撻梺鐟板暱濞寸兘骞夐幘顔芥櫆濠殿喗鍔掔花濠氭⒑閸濆嫮袪闁告柨閰ｉ幆宀€鈧綆鍠楅悡娑㈡煕閳╁喚娈樼紒鐘哄吹閳ь剚顔栭崰鏍偉婵傜鏄ラ柍鈺佸暞婵挳鎮峰▎蹇擃仾闁稿﹪鏀辩换婵嗏枔閸喗鐏嶉柤鍨﹀洦鐓曢柡鍐ｅ亾闁搞劌娼￠幃浼搭敊閼恒儱鍔呴梺闈涚箞閸ㄥ鎯侀崼銉︹拺闁告稑锕ｇ欢閬嶆煕閵婏箑鈻曟い銏＄懇瀵挳濮€閳锯偓閹峰搫鈹戦悙璺鸿敿妞ゆ泦鍕洸闁规壆澧楅悡鏇㈡倵閿濆骸浜濈€规洖鐭傞弻?WARP 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧壕濠氭煕閳╁啰鈽夐柛灞诲姂閺岋綁鏁愰崨顔芥嫳闂佸搫妫欑划鎾诲蓟瀹ュ牜妾ㄩ梺鍛婃尵閸犳牕顕ｆ繝姘╅柕澶堝灪椤秴鈹戦埥鍡楃仩闁诲繑绻堥、鏃堫敇閵忊檧鎷洪梺鍛婄缚閸庨亶寮抽柆宥嗙厱閻庯綆鍋呭畷宀勬煛瀹€瀣М闁诡喒鏅滃蹇涘煛娴ｅ壊鐎存繝鐢靛О閸ㄥ骞婇幘婢勬稑鈻庨幘宕囩杽闂侀潧艌閺呮稓绮婚悷鎳婂綊鏁愰崨顔藉枑闂佷紮缍€妞村摜鎹㈠☉娆愮秶闁告挆鍕还婵＄偑鍊х徊鑲╁垝濞嗗繒鏆?
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
