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

// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閻樻爠鍥ㄧ厱閻忕偛澧介悡顖氼熆鐟欏嫭绀€闁宠鍨块、娆撴儗椤愵偂绨婚柣锝囧厴椤㈡宕熼銏犱憾闂佽娴烽弫鍝ユ兜閸洖纾婚柟鎹愬煐閸犲棝鏌涢弴銊ュ妞わ富鍠栭—鍐Χ鎼粹€茬盎濡炪倧绠撴禍鍫曞春閳ь剚銇勯幒鎴濐仾婵炴嚪鍥ㄧ厪闁割偁鍩勯悞鐐亜閺囶亞绉€规洖銈稿鎾倷濞堝灝鎮堥梻鍌欒兌缁垶鏁嬮梺璇茬箲閼规崘顣鹃柟鍏肩暘閸ㄨ崵寮ч埀顒勬⒑濮瑰洤鐏叉繛浣冲嫮顩烽柍鍝勬噺閻撴瑦绻涢懠棰濆敽缂併劎鏅埀顒€鐏氬姗€鏁冮鍫濈疇闁绘劕鎼敮閻熸粌绻楅妵鎰版倷椤掑倻鐦堥梺姹囧灲濞佳冪摥婵犵數鍋涢惇浼村磹濠靛绠栭柛褎顨忛弫瀣煃瑜滈崜鐔奉嚕鐠囨祴妲堟俊顖炴敱椤秴鈹戦埥鍡楃仩闁告艾顑夐獮澶嬬附閸涘ň鎷虹紓鍌欑劍钃遍悘蹇涙涧閳规垿鎮欓埡浣峰闂傚倷绀侀幖顐︽儗婢跺瞼绀婂〒姘ｅ亾闁绘侗鍠栭～婊堝焵椤掑嫮宓侀幖杈剧悼妞规娊鎮楅敍鍗炲暕婢规洖鈹戞幊閸婃捇姊藉鍡欘洸婵犲﹤鐗婇悡銉︾節闂堟稒顥為柛锝勭矙閺屻劌鈽夊Ο澶广倗绱掓潏銊ョ瑲婵炵厧绻樻俊鎼佸Ψ閿曚胶妾ㄩ梻鍌欐祰椤曟牠宕伴幒妤€鐤鹃柣妯款嚙缁犵娀鏌ㄩ悢鍝勑㈤崶鎾⒑閹肩偛鍔橀柛鏂跨Ч閹線骞掑Δ浣叉嫽闂佺鏈悷褔藝閿曞倹鐓欑痪鏉垮船娴滀即鏌熼姘兼Ч缂佽櫣鏅划娆戞崉閵娧冪倞闂傚倷绀侀幖顐ょ矙娓氣偓瀹曘垽骞嗚閺嗐倕霉閿濆棙鍋犻柍褜鍓氱敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劕鐏氶弳婊堟煥閻斿搫啸鐎规挷绶氶幃妤呮晲鎼存繄鎸夐梺琛″亾濞寸姴顑嗛悡鏇㈡煃闁款垰浜鹃梺鍝勬噺缁海妲愰悙鍝勭闁挎棁袙閹峰姊虹粙鎸庢拱闁煎綊绠栭崺鈧い鎺戝濡垶绻涢崱鎰仼妞ゎ偅绻勯幑鍕惞閻熸澘袝濠碉紕鍋戦崐鏍暜閹烘纾归柛娑橈功椤╅攱绻濇繝鍌滃闁绘挾濮电换娑㈡嚑妫版繂娈梺璇查獜缁绘繈寮婚敓鐘插窛妞ゆ挾濮撮悡鐔奉渻閵堝啫鐏€光偓閹间礁鏄ラ柍褜鍓氶妵鍕箳閹存繍浠奸梺钘夊暟閸犳劗鎹㈠☉銏犵闁绘劕鐏氶崳顔剧磽娴ｅ弶顎嗛柛瀣崌濮婄粯鎷呴崨濠傛殘濠电偠顕滅粻鎾崇暦濠婂喚娼╅柤鎼佹涧娴犙冾渻閵堝懐绠伴柣妤€锕崺娑㈠箣閿旂晫鍘介梺缁樻⒒椤牓藟鐎ｎ喗鐓熼柨婵嗛瀵噣鏌＄仦鍓ф创濠碘剝鎮傛俊鍫曞炊閳哄啫骞嗛梻鍌欒兌缁垱绗熷Δ鍛獥婵娉涢悞鍨亜閹烘埊鍔熺紒澶屾暬閺屾盯鎮╅幇浣圭杹閻庢鍠楄ぐ鍐煘閹寸姭鍋撻敐搴′簽闁告ɑ鎹囬幃宄邦煥閸曨厾鐓夐悗瑙勬礃缁捇骞冮姀銈嗗亗閹艰揪绲块弳?0=闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝曢梻浣藉Г閿氭い锔诲枤缁辨棃寮撮悢铏圭槇闂佹眹鍨藉褍鐡梻浣瑰濞插繘宕愬Δ鍛劦妞ゆ帊绀侀崵顒勬煕閿濆繒绉┑鈩冩尦瀹曘劑寮堕幋鐘靛幀濠电姰鍨煎▔娑㈡儔婵傚憡鍎婇柛顐ゅ枔缁♀偓闂佹眹鍨藉褍鐡梻浣侯焾閿曪妇鍒掗鐐茬柧閻犻缚銆€濡插牓鏌曡箛銉х？闁告﹢浜堕弻锝嗘償椤栨稈鍋撻妷鈺佺妞ゆ帒鍊哥紞鍐⒒閸屾瑧顦﹂柟纰卞亰瀵敻顢楅埀顒傚弲濠碘槅鍨伴崥瀣暦閸欏绠鹃柟瀵稿仧閻倖淇婇幓鎺濈吋闁哄矉绲鹃幆鏃堝閻樺弶鐦撻梻浣告啞閹搁箖宕版惔顭戞晪?1=闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰效闁挎繄鍋涢埞鎴犫偓锝庡亝濞呭洭姊虹粙璺ㄧ効濠碘€虫川缁瑨绠涢弮鍌滅槇闂侀潧楠忕徊浠嬫偂閹扮増鐓曢柡鍐ｅ亾闁绘濮撮悾鐑藉箣閿曗偓鍥存繝銏ｆ硾椤戝洭宕㈤幖浣光拺闁硅偐鍋涢崝妤呮煛閸涱喚鈽夐柣顭戝墴濮婄粯鎷呴搹鐟扮闁汇埄鍨辩敮锟犲极閸愵喖唯闁宠桨绲奸敃鍌涚厱闁哄洢鍔岄弸銈夋煟閵堝倸浜鹃梻鍌欑濠€閬嶅磻閹捐绠氶悘鐐跺▏濞戙垺鏅搁柣妯哄暱閳ь剛鏁婚幃宄扳枎韫囨搩浠剧紓浣插亾闁逞屽墴閹鈻撻崹顔界亪婵犫拃鍐弰妤犵偞鍨挎慨鈧柣姗嗗亝椤秹姊洪棃娑氱濠殿喚鍏橀、姗€宕崟銊︽杸闂佺粯鍔曞鍫曀夊鍛＜缂備焦锚缁楁氨绱掗崒娑樻诞闁硅櫕鐗犻崺锟犲礃椤忓海闂?2=闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝曢梻浣藉Г閿氭い锔诲枤缁辨棃寮撮悢铏圭槇闂佹眹鍨藉褍鐡梻浣瑰濞插繘宕愬Δ鍛劦妞ゆ帊绀侀崵顒勬煕閿濆繒绉┑鈩冩尦瀹曘劑寮堕幋锝嗘啺闂備線娼ч¨鈧┑鈥虫喘瀹曪綁宕卞缁樻杸闂佺粯顭囩划顖氣槈瑜庢穱濠囶敃閿濆洦鍣悗鍨緲鐎氭澘鐣烽悢纰辨晣婵炴垶眉婢规洟鏌ｉ悢鍝ユ噧閻庢凹鍘剧划鍫ュ焵椤掑嫭鍊垫繛鍫濈仢閺嬫稑顭胯闁帮綁鐛幋锕€顫呴柣姗嗗亝椤秹姊洪棃娑氱濠殿喚鍏橀、姗€宕崟銊︽杸闂佺粯鍔曞鍫曀夊鍛＜缂備焦锚缁楁氨绱掗崒娑樻诞闁硅櫕鐗犻崺锟犲礃椤忓海闂繝鐢靛仩閹活亞寰婃禒瀣妞ゆ劧绲挎晶鈩冪節閻㈤潧啸闁轰焦鎮傚畷鎴濃槈閵忊剝娅囧銈呯箰鐎氱兘宕甸弴鐐╂斀闁绘ê鐤囨竟姗€鏌涘Δ浣糕枙闁哄矉缍佸顕€宕奸锝庢缂傚倷鐒﹂崝妤呭磻濞戙垹鐓橀柟杈鹃檮閸婄兘鏌℃径瀣仼濞寸姵鎮傚娲偂鎼达絿楔缂備胶濮甸悧鐘荤嵁閸℃瑤娌悷娆欑稻閺傗偓闂佽鍑界紞鍡涘磻閳ь剟鏌?
// 闂?warp-cli status 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎悗骞垮劚椤︻垳绮堢€ｎ偁浜滈柟鍝勭Ф閸斿秵銇勯弬鎸庡枠婵﹦绮幏鍛喆閸曨偂鍝楅梻浣侯焾鐎涒晛顪冮挊澶屾殾婵犲﹤鍟犻弸搴ㄦ煙鐎涙绠ユ俊顐ｇ矒閹嘲顭ㄩ崨顓ф毉闁汇埄鍨遍〃濠囧箖閳ユ枼妲堥柕蹇ョ磿閸橀亶姊洪棃娑辨缂佽弓绮欏畷婵嗩潩閼哥數鍘介棅顐㈡祫缁插ジ鏌囬鐐寸厸閻忕偛澧介妴鎺楁煃瑜滈崜銊╁箑閵夆晛绀嬫い鎺戝€哥徊浠嬫⒒娴ｇ鈷旂紒顕呭灦閹囨偐鐠囪尪鎽曢梺闈涱焾閸庮噣寮ㄦ禒瀣厱闁斥晛鍘鹃鍕ㄦ瀺闁挎繂顦伴埛鎴︽煕濠靛棗顏い銉у█閺屾稓鈧絻鍔岄埀顒佺箓椤曪絾绻濆顓熸闂佺粯锚瀵爼濡靛┑鍥╃閺夊牆澧介幃鍏笺亜椤撶偟澧﹂柨婵堝仱椤㈡洟鏁傜憴锝嗗闂傚倷绶￠崑鍡涘磻濞戙垺鍤愭い鏍ㄧ⊕濞呯姵銇勯弴妤€浜鹃梺鍝勬湰缁嬫垼鐏掗梺鐓庮潟閸婃鎮℃径鎰€甸悷娆忓缁€鍐煕閵娿儲鍋ラ柣娑卞枛铻ｉ柤娴嬫櫇閿涙粌鈹戦埥鍡楃仯闁告鍥╁祦闁割偁鍎查埛鎺懨归敐鍥╂憘婵炲吋鍔栫换婵嬪閳晜鍨垮鏌ユ嚋閸偄鍔呴梺闈涱焾閸庢娊藝椤愶附鈷戦柛婵嗗濡插綊鏌ㄥ☉娆愮闁诡喚鍏橀弻鍥晜閹冩辈闂傚倷绀佹竟濠囧磻娓氣偓瀹曡绻濆顒佽緢濡炪倖鍔ч梽鍕煕閹达附鍋ｉ柛銉岛閸嬫捇鎼归銈勬喚闂傚倷鐒︾€笛兠鸿箛娑樺瀭濞寸姴顑囧畵渚€鏌涢幇鈺佸Ψ闁哄閰ｉ弻鐔衡偓鐢殿焾娴犳粎鐥紒銏犵仸闁哄备鍓濆鍕沪閹存帗鍩涘┑鐘媰閸曞灚鐤侀梺璇″灠閻楁劗绮诲☉妯锋婵炲棗鏈€氳棄鈹戦悙鑸靛涧缂傚秮鍋撳┑鈽嗗亜鐎氭澘鐣烽妷銊ｄ汗闁圭儤鎸鹃崢閬嶆⒑缂佹◤顏堝疮閹稿孩鍙忓鑸靛姈閻撴瑦顨ラ悙鑼虎闁诲繗灏欓埀顒冾潐濞叉牠濡堕崨濠佺箚闁绘垼妫勫敮闂侀潧顭梽鍕疮濞差亝鈷掗柛灞剧懄缁佺増绻涙径瀣鐎规洑鍗抽獮妯尖偓娑櫭鍧楁煟鎼淬劍娑ч柟鑺ョ矒閹矂宕卞☉娆忊偓鐢告煥濠靛棝顎楅柡瀣枛閺屽秹鏌ㄧ€ｎ亝璇為梺鍝勬湰閻╊垶銆侀弴銏犖ч柛娑卞枤娴滄牠姊绘笟鈧褍螞濞嗘垶鏆滈柟鐑橆殕缁犳帡姊绘担铏瑰笡闁荤喆鍨藉畷鎴﹀箻缂佹鍘遍梺闈涱焾閸婃洖鐣甸崱娑欑厱闁冲搫鍟禒杈殽閻愬樊鍎旈柡浣稿€块幐濠冨緞婢跺娼栨繝纰夌磿閸嬫垿宕愰弽顓炵闁挎洍鍋撴い顓炴健閸┾偓妞ゆ巻鍋撴い顓℃硶閹叉挳宕熼鍌ゆЧ婵犳鍠栭敃锔惧垝椤栫偛绠柛娑欐綑瀹告繂鈹戦悩鎻掆偓鐟扳枔濡　鏀介柣鎰摠鐏忔壆鐥弶璺ㄐч柕鍡楀暞缁绘繈宕掑鍕啎闂備浇宕甸崰鎰珶閸℃稑姹查柨鏇炲€归悡鏇㈡煏婢跺鐏ラ柡鍫墴钘熼柍銉︽灱閺€浠嬪箳閹惰棄纾圭憸蹇擃嚗婵犲啰顩烽悗锝庝簽閺屽牆顪冮妶鍡欏⒈闁稿鍋ゅ畷鎴﹀磼濞戞氨顔曢梺绯曞墲钃遍悘蹇庡嵆閺屾稒绻濋崒婊€绮电紓浣虹帛閻╊垶鐛€ｎ喗鍊烽柛顭戝亜濞呇囨⒒娴ｈ櫣甯涢悽顖滃仱瀵煡顢曢妶鍡╂綗闂佸湱鍎ら〃鍡涘磻閹扮増鐓ｉ煫鍥风到娴滄粓姊绘径濠勑㈡い顏勫暣婵″爼宕橀妸銉庘晠姊虹粙鎸庢崳闁轰礁顭烽悰顕€宕橀妸銏＄€婚梺鐟扮仢閸燁垶寮插鍫熲拺闁告稑锕ユ径鍕煕濡寧顏犳俊鍙夊姇閳诲酣骞樼划瑙勫濠电偠鎻徊浠嬪箟閿熺姴绠氶柛顐犲劜閸嬬姵鎱ㄥΟ鐓庡付濞存粍澹嗛埀顒冾潐濞叉鎹㈤崒鐐村仼婵犻潧顑呯粈瀣亜閹扳晛鈧洖顕ｉ妸銉㈡斀闁绘﹩鍠栭悘杈ㄧ箾婢跺顬煎褎绻堝娲传閸曢潧鍓伴梻浣稿簻缂嶄礁顕ｆ繝姘亜閻炴稈鈧厖澹曢梺姹囧灪椤旀牠鎮為幆顬″綊鎮崨顖滄殼闂佸搫鏈粙鎺楀箚閺冨牆顫呴柍钘夋缂嶆〗QUE 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇顓滀簻闁哄啫鍊归崵鈧繛瀛樼矒缁犳牠寮诲☉銏犵疀闂傚牊绋掗悘鍫ユ倵閻熺増鍟炵紒璇插暣婵＄敻宕熼姘敤闂侀潧臎閳ь剙危閸儲鐓欓柛蹇撳悑缂嶆垿鏌ㄩ弴妯衡偓婵嬪春閻愬搫绠ｉ柣鎰皺閻ゅ洭姊绘担渚劸闁挎洏鍎辩叅闁绘棁鍋愬畵渚€鐓崶銊︾５闁稿鎹囬弫鎰償閳╁啰浜梻浣告惈椤戝嫮绮堟笟鈧崺鐐哄箣閿旇棄浜归梺鍛婄懃椤︻垶藝閳哄懏鈷戠紓浣股戠亸鐗堢箾閸欏鐒介柟骞垮灩閳藉濮€閻樻鍚呮繝鐢靛█濞佳兾涘鍫濇槬闁绘劗鍎ら埛鎴︽煕濠靛棗顏撮柛搴＄Ч閺屾稒绻濋崘顏嗙杽閻庢鍠涢褔顢樻總绋跨倞闁冲搫鍠涚槐鍙夌節绾版ɑ顫婇柛銊ュ船椤啴宕归銉︾洴瀹曟﹢顢欑憴锝嗗闂佽崵濮村ú鐘诲焵椤掑啯鐝柣蹇撶Т椤啴濡堕崘銊ヮ瀳闂佹寧娲︽禍婊堫敋閿濆鏁冮柨鏇楀亾缂佺姵绋掗妵鍕箻鐠虹儤鐎婚梺鍛婃煥閹虫ê顫忓ú顏呭€烽柦妯侯槸婵洟姊洪崨濠冨鞍闁煎綊绠栭垾锕傚炊椤掆偓缁€鍫㈡喐韫囨洖顥氬ù鐘差儐閻撴洟鎮橀悙鎻掆挃闁瑰啿鎳忛妵鍕晜婵傚憡顎嶉梺闈涙搐鐎氫即寮崒婊勫珰闁肩⒈鍏涢崠鏍磽閸屾瑦绁板ù婊庡墴瀹曟垿鎮欓崹顐㈠簥濠电娀娼ч鍛存倷婵犲嫭鍠愰柟杈剧畱缁犳椽鏌￠崶鈺佷汗闁衡偓娴犲鐓熼柟閭﹀幗缂嶆垿鏌ｈ箛銉х暤闁哄被鍔戝鏉懳旀担鍦絽闂備線娼уΛ娆戞暜濡ゅ啯宕叉繝闈涙－濞尖晜銇勯幘璺盒㈡鐐搭殜濮婄粯鎷呴悜妯烘畬闂佸鏉垮妞ゎ偄绻橀幃鐑藉箥閾忣偆鈧椽姊洪崨濠佺繁闁告柨鐖奸幃鐑芥焽閿旀儳寮抽梻浣稿閸嬩線宕规繝姘辈妞ゆ劧闄勯埛鎴︽煕濠靛棗顏繛绗哄€濋弻娑㈠焺閸愵亝鍣梺鍝勵槹閹倸顫忓ú顏勭閹艰揪绲块悾鐢告⒑閻熸澘鏆辩紒澶屾嚀閻ｅ嘲鈻庨幇顕呮祫闁诲函缍嗛崑鈧柟閿嬫そ濮婃椽宕ㄦ繝鍕ㄦ闂佹寧娲忛崐婵嗙暦閻㈠壊鏁嗛柛鏇ㄥ墰閸橀亶鏌ｆ惔顖滅У濞存粍绻堝畷铏鐎ｎ偆鍘靛Δ鐘靛仜閻忔繈鎮橀埡鍛厓闁芥ê顦藉Σ鎼佹煃鐠囨煡鍙勬鐐叉喘椤㈡瑩骞忓Ο绋库偓婵嗩潖缂佹ɑ濯撮柛娑橈工閺嗗牓姊洪崷顓х劸婵☆偅绻傞悾宄邦潨閳ь剚淇婂宀婃Щ闁汇埄鍨遍幐鑽ゆ崲濠靛鍋ㄩ梻鍫熷垁閵夛负浜滈柨婵嗙墛椤ュ宕￠柆宥嗙厱婵炴垶鐟︾紞鎴犵棯閹岀吋闁哄本绋戦埥澶娢熼崗鍏煎枛闂備胶顭堥鎴犳閺囥垹绠熸慨婵嗙灱閻も偓濠电偞鍨堕…鍥囬锔解拺闁革富鍘奸崝瀣煙缁嬪灝鈷旂€殿啫鍥х劦妞ゆ帒瀚埛鎴︽煙閼测晛浠滈柛鏃€锕㈤弻娑㈠棘鐠恒剱銏ゆ煃鐠囪尙效鐎规洖鐖奸、妤佹媴閸欏顏圭紓鍌氬€风粈渚€顢栭崱娆屽亾缁楁稑鍠涢懓鍧楁煙濞堝灝鏋ょ痪鎯с偢閹鏁愭惔鈥茬凹閻庤娲栭惉濂稿焵椤掑喚娼愭繛鎻掔箻瀹曡绂掔€ｎ剙绁﹂梺鍦劋閸わ箓寮埀顒傜箔閻旂厧鐐婃い顓熷灦椤ュ﹥绻濈喊澶岀？闁稿鍨垮畷鎰板冀椤撶偟顦梺纭呮彧缁犳垿宕归崒鐐寸厽闁瑰浼濋鍡橆偨闁绘劗鍎ら悡鏇㈡煛閸ャ儱濡奸柣蹇ョ節閺岋綁骞樼€靛憡鍣紓浣介哺鐢偤鍩€椤掑﹦绉甸柛瀣椤㈡艾顭ㄩ崼鐔哄幍闂佸憡鐟ラˇ浼村磹閹扮増鐓涚€光偓閳ь剟宕版惔銊﹀仼闁跨喓濮甸悞浠嬫煥閺囨浜鹃梺鍛婃尭閻栧ジ骞冨Δ鈧埢鎾诲垂椤旂晫浜俊鐐€ら崣鍐绩鏉堛劎鈹嶅┑鐘叉处閸婂鏌ら幁鎺戝姕婵炲懎妫濆铏规嫚閳ュ磭鈧鏌涢幇鍏告喚闁哥喎顑夊濠氬磼濮橆兘鍋撻幖浣哥９闁绘垼濮ら崐鍧楁煥閺囩偛鈧摜澹曢崸妤佺厱闁归偊鍨煎鍧楁煟閻旂厧浜版繛灏栨櫊閹銈﹂幐搴哗闂佹寧绋掗惄顖氼潖濞差亜宸濆┑鐘插暙椤︹晠姊洪幖鐐插濞存粏娉涢悾鐑藉箛椤旇棄纾柣鐐寸▓閸撴繈寮埀顒勬⒒娴ｈ櫣甯涢拑杈╂喐閺夊灝鏆ｆい銏℃椤㈡洟濡堕崶顭掔础闂備浇顕栭崹搴ㄥ礃閳轰讲鍋撻娑氱闁挎繂鎳忛幖鎰版煥閺囥劋绨煎ǎ鍥э躬瀹曞ジ寮撮悙纰夌床婵犳鍠楅敃鈺呭磻閸曨垁澶婎潩椤戣姤鏂€闂佹寧绋戠€氼剚绂嶆總鍛婄厱濠电倯鍐仹缂佽妫濋弻娑㈠Ψ椤旂厧顫╅梺鍝勵儎缁舵岸骞冭ぐ鎺戠倞鐟滃酣鍩㈤弴銏＄厱闁靛牆妫欑粈鈧梺瀹狀潐閸ㄥ潡骞冮埡鍐ｅ亾閸︻厼孝妞ゃ儲绻堥幃妤呮偡閻楀牆鏆堢紓浣筋嚙閸熷瓨淇婇悽绋跨妞ゆ牗姘ㄩ鎺戔攽鎺抽崐鎰板磻閹剧粯鍊垫慨妯煎帶婢у鈧娲橀〃鍡欐崲濠靛鐐婇柕濞垮灪鐎氳棄鈹戦悙鑸靛涧缂佽弓绮欓獮澶愭晸閻樿尙鏌堥梺缁樺姉閸庛倝鎮″▎鎴犵＜閻庯綆鍋勯婊勩亜閵夛絽鐏柍褜鍓濋～澶娒洪敂鐣岊洸婵犻潧顑呯粻顖炴煕濞戞瑦缍戠紒鈧?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厱闁归偊鍨扮槐锕傛煟閵忋垻甯涘ǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘介崑瀣繆閵堝懎鏆熼柣顓熺懇閺岀喖鎮欓鍌涜弴闂侀€炲苯澧柟铏悾鐑芥晲閸℃绐為悗鍏夊亾闁逞屽墴閹線宕奸妷锔规嫽闂佺鏈銊︽櫠濞戞氨纾奸悗锝庡亜濞搭噣鏌℃担鍝バх€殿噮鍣ｅ畷濂告偄閸濆嫬绠?status 闂?Connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闂囧鏌ㄥ┑鍡欏妞ゅ繒濮风槐鎺楀焵椤掍胶绡€闁稿本顨嗛弬鈧梻浣虹帛钃辩憸鏉垮暞缁傚秹鎮欓悜妯煎幗闂佸湱鍎ゅ鐟扳枍閺囥垺鐓欓柛娆忣槺閹藉啰绱掗悩宕囨创鐎殿喗鎸抽幃娆撴嚑椤戝灝鏋堥梻鍌氬€风粈渚€骞栭锕€鐤い鎰堕檮閸嬪鏌涢埄鍐噮濞戞挸绉甸妵鍕即濡も偓娴滈箖姊洪崫鍕効缂佺姵鍨奸悘鎺撶箾閹炬潙鍤柛銊ㄩ哺閻″繘姊婚崒娆掑厡缁绢厼鐖煎鎻掆攽鐎ｎ亞锛涘┑鐐村灍閹冲洭鍩€椤掍焦顥堢€规洘锕㈤、娆撳床婢诡垰娲﹂悡鏇㈡煃閳轰礁鏋ゆ繛鍫燂耿閺岋綁鎮㈤弶鎴濐潎濠殿喖锕ㄥ▍锝囧垝濞嗘挸绀岄柍銉ュ帠缁憋絿绱撻崒姘偓鍝ョ矓閹绢喗鏅濋柕鍫濐槹閸嬧晠鏌ｉ幋锝嗩棄缂佺媴绲剧换婵嬪垂椤愩垹顫嶉悗鐐瑰€栧钘夘潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯椤曨參鍩€椤掍緡鍟忛柛鐘崇墵閳ワ箑鐣￠柇锕€娈ㄩ梺鍦檸閸犳宕戦崟顖涚叄闊洦鍑归崵鍐煕閻旂兘鍙勬慨濠勭帛閹峰懐绮欏▎鐐棏闂備胶绮幐鎼佹偋閹惧磭鏆﹀ù鍏兼綑閸愨偓闂侀潧顭粻鎴濐嚕閸ф鈷戦柛鎾村絻娴滄繃绻涢崣澶涜€块柛鈺傜洴楠炲鏁傞悾灞藉箞婵犵數鍋為崹鍫曟晝椤愶籍娑橆潩椤掑鍞甸柣鐔哥懃鐎氭悂鎳撻崸妤佺厸閻忕偛澧介埊鏇犵磼缂佹绠炵€规洘甯掗埥澶娢熺憴鍕枙闂備浇顕х€涒晠顢欓弽顓炵獥闁哄稁鍘搁埀顒婄畵閹粓鎸婃径宀€鏆梻渚€娼х换鍫ュ磹閺嶎厼纾婚柛宀€鍋涚粻褰掑级閸繂鈷旂紒澶屾暬閺屾盯鎮㈡總澶婂壉婵烇絽娲ら敃顏堝箖濞嗘搩鏁傞柛鏇樺妼娴滈箖鏌″搴ｄ汗婵炲樊浜滈崘鈧銈嗘尵閸嬬喖鎮块崶顒侇棅妞ゆ劑鍨烘径鍕煣閺傛鍎愮€垫澘瀚伴獮鍥敆閳ь剟宕妸鈺傚仭婵犲﹤瀚惌濠囨煏閸℃ê绗氱紒缁樼箓椤繈顢橀悩鎻掑闂傚倷绀佹竟濠囧磻閸涱垱宕查柛鎰靛枛闂傤垶鏌ц箛鎾磋础缁炬儳銈搁弻娑氫沪閹冩瘓闂佺瀛╁Λ鍐蓟閿涘嫪娌柛鎾楀嫬鍨辨俊銈囧Х閸嬬偤鈥﹂崶顒€鐒垫い鎺戝濞懷囨煙鐠囇呯瘈鐎规洘鑹鹃埥澶娢熷鍕棃闁诡喒鏅犲Λ鍐ㄢ槈濡ゅ啯宕熺紓鍌氬€烽懗鍓佸垝椤栨粍鏆滈柨鐔哄Т閺勩儵鏌嶈閸撴岸濡甸崟顖氱闁规惌鍨版慨娑氱磽娴ｅ壊妲洪柡浣割煼瀵鈽夐姀鈥充汗閻庤娲栧ú銈夊焻閻熸壋鏀介柣鎰綑缁茬粯绻涚亸鏍ゅ亾閹颁焦缍庣紓鍌欑劍钃卞┑顖涙尦閺屾稑鈽夊鍫濅紣闂佸搫妫楅悧鍡涘煘閹达附鍊峰Λ鐗堢箓濞堟繄绱撴担鍝勑ｉ柟鍛婃倐閹箖鎮滈挊澶岄獓濠电偛妫涢崑鎾诲磽閻㈢數纾藉ù锝堫潐椤ユ棃鏌涢妸锔界凡閻撱倝鏌ㄩ弮鍫熸殰闁稿鎸婚幏鍛村礃閵娿倖鐫忛梻渚€鈧偛鑻晶顖滅磼鐎ｎ偄绗╃憸鐗堢矒濮婃椽鎮烽弶鎸幮╁銈庡幖閸㈡煡锝炶箛娑欐優闁革富鍘鹃悡鎾绘⒑閸︻収鐒鹃悗娑掓櫊楠炲繑娼忛埡鍐紲闂佺粯鍔﹂崜姘辨閼碱剛纾煎璺烘湰閸も偓闂佷紮绲块崗姗€銆佸☉姗嗘僵闁稿繒鍘ч崗濠囨⒒娴ｅ搫甯堕柟鑺ョ矒瀵偊骞栨担绋垮亶闂備緡鍓欑粔鐢稿煕閹达附鈷掗柛顐ゅ枍缁惰鲸淇婇姘卞ⅵ闁哄本鐩俊鎼佹晝閳ь剟鍩㈤弴銏＄厵妞ゆ梹鍎虫禍鐐箾閻撳海绠诲┑鈩冩倐閺佹劙宕熼鐘虫櫒闂傚倸鍊风欢姘焽瑜旂瘬闁逞屽墰缁辨帡鎮╅崘鑼紝閻庤娲﹂崑濠囧箖濠婂吘鐔烘嫚閹绘帊绱熸繝鐢靛Х閺佸憡鎱ㄩ幘顔肩柈妞ゆ牜鍋涙濠电娀娼ч悧鍐磻閹捐埖鍠嗛柛鏇ㄥ墰閿涙﹢姊洪崨濠冪叆缂佸鐖奸獮鎴﹀閻橆偅鏂€闁诲函缍嗘禍鐐核囬弶娆炬富闁靛牆妫楅崸濠囨煕鐎ｎ偅灏扮紒缁樼洴瀹曘劑宕惰婢跺嫰鏌嶉柨瀣瑨闂囧鏌ㄥ┑鍡欏⒈婵炲吋鍔欓弻娑氣偓锝庡墮閳诲牓鏌＄仦鍓ф创鐎殿噮鍓涢幑鍕Ω閹板墎鎮肩紒杈ㄥ笧缁辨帒螣鐏忔牕顫岀紓鍌欒兌缁垳鎹㈤崼婵堟殾闁圭儤顨呭洿闂佸憡渚楅崳顕€宕ュ▎蹇婃斀闁挎稑瀚禍濂告煕婵犲啰澧电€规洘绻嗛ˇ瀵哥磼椤旂⒈鐓兼鐐差儏閳规垿宕堕埡浣告瘣濠电姷鏁告慨鐑藉极閸涘﹥鍙忓ù鍏兼綑閸ㄥ倿鏌ｉ幘宕囧哺闁哄鐗楃换娑㈠箣閻愮數鐓傚┑?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨绘い鎺嬪灪閵囧嫰骞囬鍡欑厯闂佸搫琚崝鎴﹀箖閵忋倕浼犻柛鏇熷灟閸ㄥ鎯€椤忓牆绠氱憸婊堟偂婵傚憡鐓涢悘鐐插⒔閳藉鏌嶉挊澶樻█鐎规洩绻濋幃娆戔偓鐢告櫜婢规洟姊婚崒娆戭槮缁剧虎鍙冮獮濠囧箻閸撲胶鐓嬮悷婊呭鐢帡鎷戦悢鐑樺枑闁瑰濮锋稉宥嗙箾閹寸儑渚涙繛灏栨櫊閺屾稖绠涘顑挎睏缂備礁顦遍崗姗€寮?sniroute.cmd 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闂囧鏌ㄥ┑鍡╂Ч濞存嚎鍊濋弻銈夊级閹稿骸浠村┑顔硷攻濡炰粙銆佸Δ鍛劦妞ゆ帒鍊婚惌鎾绘煟閵忋埄鐒剧痪鎯ь煼閺屻劑鎮㈤崫鍕戯綁鏌ｉ幘瀵告噰闁哄瞼鍠栭、娑橆煥閸愮偓姣夋俊鐐€戦崕閬嵥囨导娣偓鍐Ψ閳哄倸鈧攱銇勮箛鎾愁伀濞寸厧鐗撳娲传閸曨喖顏紓浣割槺閹虫捇鎮鹃悜绛嬫晝闁挎洍鍋撶紒鈧€ｎ偁浜滈柟閭﹀枛閺嬪骸霉濠婂啫鈷旈柟鍙夋倐閹囧醇濠靛牏鎳嗙紓?warp on/off闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏㈡偘閳ュ厖澹曢梺姹囧灪椤旀牠鎮為幆顬″綊鎮╁▎蹇斿闁抽攱甯￠弻娑㈠即閵娿儰绨诲銈呮禋閸欏啴寮婚垾宕囨殕閻庯綆鍓涢惁鍫ユ倵鐟欏嫭绀冮柨鏇樺灪娣囧﹪骞栨担鑲濄劑鏌曡箛鏇炐″瑙勬礋濮婃椽鎮℃惔顔界稐闂佺锕ラ〃鍡欑矙婢跺鍚嬪璺侯儑閸樻悂姊虹粙鎸庢拱濞撴碍顨婂畷鏇㈠Ψ閳哄倻鍘介梺鍝勫€圭€笛囁夐姀銈嗙厵妞ゆ梹顑欏鎰版煏閸剛鐣垫鐐村姈閹棃顢欑拠鍙夋啟闂傚倸鍊峰ù鍥敋瑜忛幑銏ゅ箛椤掑倹娈惧銈嗗笒鐎氀囧焵椤掍焦顥堢€规洘锕㈤、娆撴寠婢跺鏁鹃梻鍌欑閹碱偊藝閹殿喒鍋撶粭娑樺敪閹烘绠涙い鎺戭槹鐎靛矂姊洪棃娑氬濡ょ姷顭堥锝夊Ω閿斿墽顔曟繝銏ｆ硾椤戝棛绮堢€ｎ偒娈介柣鎰綑閻忓鈧娲滈崰鏍€佸☉妯锋婵☆垰婀遍惀鍛存⒒閸屾瑧顦﹂柟纰卞亰钘濆ù鍏兼綑閸ㄥ倿鏌ｉ姀銈嗕氦缂傚秵鐗犻弻锝呂熼崗鍏兼瘎缂佹儳褰炵划娆撳蓟濞戞矮娌柛鎾楀嫬娅橀梻浣告啞閺屻劎绮旇ぐ鎺戣摕婵炴垯鍨瑰Λ姗€鏌ｉ幇顒€绾ч柛锝堟閳ь剝顫夊ú鏍ь嚕閸撲焦宕叉繝闈涱儐椤ュ牊绻涢幋婵嗚埞闁告挷鍗冲娲寠婢跺﹥娈堕梺鍝ュУ閻楃姴顕ｆ繝姘嵆闁绘棁娅ｉ鏇㈡⒑閸撹尙鍘涢柛鐘崇墬閹便劍寰勬繛鐐杸闂佹枼鏅涢崯顐ゅ緤婵犳碍鐓熸俊銈傚亾闁挎洏鍎崇划姘綇閵娧呯槇闂佹悶鍎撮崺鏍疾椤掆偓閳规垿鎮╃紒妯婚敪濠碘槅鍋呴〃濠囩嵁閸℃稒鍊烽悗闈涙憸椤旀洟姊洪崫鍕垫Ч闁搞劎鏁婚獮鍐箣濠㈡繄鎳撻…銊╁川椤忓嫪澹曞┑?cli 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟侗鍎愭い銉ョ墛缁绘盯骞嬪┑鍡氬煘闂佸憡鎸绘竟鍡欐閹烘鍊锋い鎺嗗亾闁告柣鍊栭妵鍕敇閻樻彃骞嬮悗娈垮枛椤兘寮幇顓炵窞濠电姴瀚弶鍛婁繆閻愵亜鈧牜鏁幒妤€纾瑰瀣閸ㄦ繈鏌ｉ姀鐘冲暈闁抽攱鍨块弻娑樷攽閸℃浠惧銈冨劤閸嬬喓妲愰幒妤婃晩闁兼亽鍎辩壕鎶芥倵濞堝灝鏋涙い顓犲厴楠炲啴濮€閵堝棙鍎梺闈╁瘜閸欏繒妲愰弻銉︹拻濞撴埃鍋撴繛浣冲懏宕查柟鐑樻尰閸欏繘鏌ｉ姀鐘冲暈闁稿﹤鐖奸弻娑㈩敃閻樻彃濮曢梺鎶芥敱閸ㄥ墎鎹㈠☉銏″殤濡増绻傚▓顓炍旈悩闈涗沪妞ゃ劌妫濇俊鐢稿礋椤栨鈺呮煏婢舵稑鐦滄俊顐檮缁绘繄鍠婃径宀€锛熼梺杞版祰椤曆囨偩閻戣姤鍋勭痪鎷岄哺閺咁剙鈹戦鏂や緵闁告挻鐟╁顐﹀Χ婢跺鎷绘繛鎾村焹閸嬫挻绻涙担鍐叉濞咃綁姊绘担鍛婂暈濞撴碍顨婂畷褰掑础閻愨晜鐏佹繝闈涘€搁幉锟犲煕閹烘鐓曢悘鐐插⒔閹冲懏銇勯敂鑲╃暤闁哄瞼鍠栭幃顔尖攦閸喒鍋撻弽顓炵疇鐎光偓閸曨兘鎷绘繛杈剧悼閸庛倝宕甸埀顒勬⒑鐟欏嫮銈撮柛鎾寸箘閹广垹鈽夊▎鎴犵槇闂佹悶鍎弲娑氱矈閿曞倹鈷戦柛娑橈工婵箓鏌涢悩鎰佹疁鐎规洘鍔栫换婵嗩潩椤撶姴寮繝纰樻閸垳鎷冮敃鈧嵄闁告縿鍎崑鎾舵喆閸曨剙纰嶅┑鈽嗗亝椤ㄥ﹪宕洪悙鍝勭闁挎梻绮弲鈺呮⒑閸濆嫬鈧爼宕愬☉銏犵劦妞ゆ帒鍟涵鍫曟煟閵夘喕閭い銏★耿閹亪鍩€椤掑嫭鍊垮Δ锝呭暙绾偓闁哄鐗勯崝濠冪濠婂牊鐓涢柛銉厛濞堟柨霉濠婂嫬鍔ら柍瑙勫灴椤㈡瑧娑甸柨瀣毎婵犵绱曢崑妯煎垝濞嗘挻鍋樻い鏇楀亾鐎殿喕绮欓、姗€鎮㈢亸浣镐壕闂佸灝顑囩粻楣冩煛婢跺鐒鹃柛搴＄箰闇夋繝濠傜墢閻ｆ椽鏌＄仦绯曞亾閹颁礁鎮戦柟鑲╄ˉ閳ь剙纾鎰版⒒娴ｈ鍋犻柛鏂跨焸瀹曟劙宕稿Δ鈧拑鐔哥箾閹存瑥鐏柡鍛矒閹綊宕堕妸銉хシ缂備胶濮寸紞濠傤潖濞差亜绠归柣鎰絻椤绱撴担绛嬪殭闁绘鎸搁锝嗙節濮橆厽娅㈤梺缁橆焾鐏忔瑩藝閵娾晜鈷戦梻鍫熶腹濞戙垹绀冮柣鎰靛墯閻︽帞绱撻崒姘偓鎼佸磹閻戣姤鍤勯柤绋跨仛閸欏繘鏌涢…鎴濅簽缂佺姵鍎抽埞鎴︽偐閸欏鎮欓梺钘夊暟閸犳牠寮婚妸銉㈡斀闁糕檧鏅滅瑧缂傚倷鑳舵慨鐢电矙閹烘桅闁告洦鍨扮猾宥夋煕閵夘喚浠㈤柟鎯х摠閸欏繘鏌ㄥ┑鍡樺櫣妞ゅ繆鏅滈妵鍕敂閸曨偅娈绘繝纰樺墲閸庤偐绮诲☉銏犵闁煎憡鏅幒妤佲拻濞达絼璀﹂悞鐐亜閹存繃鍤囬柡浣稿暣婵¤埖寰勫☉姘扁棨?
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


// 闂?winget 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟版晥濠电姭鍋撳〒姘ｅ亾婵﹨娅ｇ槐鎺懳熼搹閫涚礃婵犵妲呴崑鍕偓姘煎枤閸掓帗绻濆顓炰汗缂傚倷鐒﹂…鍥储閻㈠憡鈷戦悹鍥ｂ偓宕団偓濠氭煕閹般劍娅撻柛鐔风墦濮婄粯鎷呯憴鍕哗闂佺瀛╁钘夌暦濠婂啠鏋庨柟瀵稿С缁楀顪冮妶鍡樼５闁稿鎹囬弻?Cloudflare WARP闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簻椤掋垺銇勯幇顖毿撻柟渚垮妼椤粓宕卞Δ鈧埛鎺楁⒑缂佹ê绗ч柡鍜佸亰楠炲绮欏▎鎯ф倯闂佸壊鍋掑鈧紓鍐╂礀閳规垿鎮╅懠顒侇棄闂佸搫顦花閬嶅磻閹捐鍗抽柕蹇曞Х閻ゅ洭鏌熼懝鐗堝涧缂佽弓绮欒棢婵鍩栭悡鐔兼煟閺冨偆鐒炬繛鍛仧閹叉悂鎮ч崼婵堢懖闂佺顑嗛崝妤呭焵椤掑倹鍤€濠㈢懓锕畷浼村冀椤撶偟顦梻渚囧墮缁夌敻鍩涢幋锔界厽闁绘柨鎲＄欢鑼磼閻欐瑥娲﹂悡銉╂煛閸ヮ煁顏堟倶閸欏娈介柣鎰絻閺嗘瑩鎽堕弽顓熺厱婵炴垵宕弸鐔哥箾閸涱喚鐭掗柡宀€鍠栭幊鏍煛閸曞﹤顦甸弻宥堫檨闁告挻鐟╁畷顖滄崉閵娧傜瑝闂佹寧绻傞ˇ浼村煕閹烘嚚褰掓晲閸曨噮鍔呴梺缁樺笧閸嬫捇濡甸崟顖氱婵犻潧娴傚Λ锕€螖閻橀潧浠﹂柛鏃€鐗犳俊鍫曟晲婢跺﹦顦ㄩ梺闈浤涢崨顔锯偓顐︽⒒閸屾瑧顦︽繝鈧柆宥嗘優閹兼番鍔岀粣妤呮煛瀹ュ骸浜為柣顓炴閹鏁愭惔鈩冪亶闂佺粯鎸诲ú鐔煎蓟瀹ュ鐓涘ù锝囶焾缁犺顪冮妶鍛闁绘锕畷鎰板醇閺囩喎鈧敻鏌ｉ姀銏℃毄闁靛棗锕弻娑氣偓锝庡亝鐏忣參鏌嶇紒妯诲磳鐎规洖缍婇、娆撴偩鐏炲ジ鍋楁繝纰夌磿閸嬫垿宕愰妶澶婂偍濡わ絽鍟粈鍌涙叏濡炶浜鹃梺缁樹緱閸ｏ絽鐣峰鈧、娆戝枈鏉堛劎绉卞┑锛勫亼閸娿倝宕戦幒妤€鍌ㄥΔ锝呭暙缁€鍫ユ煏婵犲繐顩紒鐘荤畺瀵爼宕煎┑鍡忔寖缂備礁顦介崜姘跺Φ閸曨垼鏁冮柕蹇婃櫆閳诲牓鎮楀▓鍨灕婵炲懏娲滈幑銏犫攽鐎ｎ亞鍊為梺闈涱焾閸ㄨ櫣鈧俺妫勯埞鎴︽倷閼搁潧娑х紓浣瑰絻濞硷繝骞冨ú顏勬婵炲棗澧介崝鐑芥⒑瑜版帒浜伴柛妯哄⒔瀵囧焵椤掑嫭鈷戦梻鍫熺〒缁犳岸鏌涢埡浣割伃妤犵偛顑夐弫鍐焵椤掑嫭鍋Δ锝呭暞閻撶姷绱掔€ｎ厽纭跺ù鐘崇洴閺屾稒鎯旈敐鍛亪闂佸搫琚崝宀勫煘閹达箑骞㈡俊顖濇閻涒晠姊绘担铏瑰笡閻㈩垱甯″畷銉р偓锝庡墯瀹曞弶绻濋棃娑卞剰妞ゎ偄鎳橀弻銊モ攽閸℃﹩妫ら梺闈涙閸旀瑩寮婚妶澶嬪仭闁绘鐗婇幏杈ㄧ箾閿濆懏鎼愰柨鏇ㄤ邯閻涱噣骞樼拠鑼唺闂佸搫鍊哥花閬嶅矗濞差亝鈷掑ù锝呮啞閹牓鏌ｉ鈧妶绋跨暦娴兼潙鍐€妞ゆ挾濮寸粊锕傛⒑缁洖澧查柛鎴犳嚀鍗卞Δ锝呭暞閳锋垿鏌熺粙鎸庢崳闁宠棄顦辩槐鎺撳緞鐏炴儳娈岄梺浼欑悼閸忔﹢銆佸☉銏″€烽柤纰卞墻閸熷洤鈹戦悙鑸靛涧缂佽弓绮欏畷銏狀煥閸繄顔嗗┑鐐叉▕娴滄繈鍩涢幋鐘垫／妞ゆ挾鍋為崳鐟懊瑰鍐Ш闁哄本绋撻埀顒婄秵閸嬪棗煤閹绢喗鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍暜婵犲洦鍤勯柤绋跨仛濞呯娀姊洪鈧粔鐢告偂閺囥垺鐓欓柣鎴炆戠亸顓犵磼濡や礁娴柡宀嬬秮閺佹劙宕卞▎鎴犳澖闁诲氦顫夊ú婊堝窗閺嵮呮殾鐟滅増甯╅弫鍐煥濠靛棙顥犵憸鎵枛濮婂宕掑顑藉亾閹间讲鈧箓顢楅崟顐わ紱闂佸憡娲﹂崐瀣洪鍛珕闂佸憡鎸嗛崨顖氬箚闂傚倷鑳堕幊鎾活敋椤撶喐鍙忛柣銏㈩焾缁€鍌涚節婵犲倻澧涢柣鎾冲暟閹茬顭ㄩ崼婵堫槶闂佺粯鏌ㄩ〃搴☆焽閺嶎厽鐓ｉ煫鍥ㄦ尰鐠愶紕鐥幆褍鎮戝ǎ鍥э躬椤㈡稑顫濋鐔峰壍婵＄偑鍊ら崑鍛村垂閸ф钃熼柨婵嗙墢閻も偓闂佽鍎虫晶搴敊婢舵劖鈷戦柤濮愬€曢弳閬嶆煛閸涱垰孝闁伙絽鍢茬叅妞ゅ繐瀚崝锕€顪冮妶鍡楃瑐闂傚嫬绉电粋宥咁煥閸喓鍘甸梺鍛婄箓鐎氼參藟濠婂厾鐟邦煥閸曨偄濮㈠銈庝簻閸熷瓨淇婇懜鍨劅闁炽儱纾鎺楁⒒娴ｄ警鐒剧紒缁樺灴閺佸顪冮妶鍡樼┛缂佹彃娼￠獮蹇涙偐鐟佷礁婀遍埀顒婄秵閸嬫帒顭囬弮鈧换婵堝枈濡椿娼戦梺鎼炲妼閻栫厧顕ｉ妸鈺傜劶鐎广儱妫楅埀顒勬涧閳规垿鎮╁畷鍥舵殹闂佹娊鏀遍崹褰掑箟閹间焦鍋嬮柛顐ｇ箘閻熴劍绻涚€涙鐭嗛柛妤佸▕瀵鈽夐姀鐘殿啋闁诲海鏁哥涵鑸垫叏閸モ晝纾藉ù锝囶焾閳ь剙婀遍弫顕€鏁撻悩闈涚ウ濠殿喗銇涢崑鎾淬亜閵忥紕鎳呴柟鍙夋尦閺佹劙宕卞Δ濠佺椽婵犵數濮烽弫鎼佸磻閻愬樊鐒芥繛鍡樻尭鐟欙箓鎮楅敐搴℃灍闁稿绱曢幉姝岀疀閺囩偛鐏婃繝鐢靛Т濞诧箓宕愭繝姘厾闁诡厽甯掗崝銈夋煕濮椻偓娴滆泛顫忓ú顏勪紶闁告洦鍋€閸嬫挾绮欏▎鍓у姺闂佽法鍠撴慨鎾儗濡ゅ懏鐓曢柡鍥ュ妼婢х粯绻涢崼婊呯煓闁哄矉缍侀獮鍥敆娴ｅ弶鐏嗘繝纰樷偓鍐茬骇闁诡喖鍊块獮妤呭醇閺囩偛鍞ㄥ銈嗘尵閸犳捇宕㈤崡鐐╂斀闁绘劖娼欓悘銉р偓瑙勬处閸撶喖鎮伴鈧浠嬵敃閵堝浄绱查梻浣哥秺閸嬪﹪宕㈡ィ鍐ㄧ叀濠㈣泛顑冩禍婊勩亜韫囨挸顏╅柡鍡到閳规垿鍨惧畷鍥х厽閻庤娲栧畷顒冪亽闂佺粯鍨惰摫妞ゆ柨鍊垮濠氬磼濞嗘帒鍘″銈庡幖閻楁捇銆侀弽顓炵倞妞ゆ巻鍋撶紒鎰殜楠炴牕菐椤掆偓閳ь剚顨嗙粋宥夋寠婢跺棙鏂€闂佺粯鍔曢悺銊т焊娴煎瓨鐓曢柍杞扮閺嗭絿鈧娲橀崹鍧楃嵁濡皷鍋撳☉娅亜顕?
// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊璁查弸娆撴⒑缂佹ê绗╁┑顔哄€楅幑銏犫槈閵忕姴鑰垮┑鈽嗗灥椤曆呭枈瀹ュ鐓熼柣鏂挎憸閹虫洜绱掗悩铏磳妤犵偛鍟灃闁告侗鍠楀▍婊堟煙閼测晞藟闁告挻绻堥幃妯侯吋婢跺鎷洪梺纭呭亹閸嬫盯鍩€椤掍焦绀嬮柟宕囧枛椤㈡稑鈹戦崼婊呰兒闂傚倸鍊烽悞锔锯偓绗涘懏宕查柛宀€鍋涚粻鐘诲箹濞ｎ剙濡兼慨瑙勭叀閺岋綁骞囬鐓庡闂佺粯鎸诲ú鐔煎蓟閺囥垹閱囨繝闈涙搐濞呫倝姊洪悷鏉挎倯闁告梹鐟╁璇差吋閸ャ劌鏋傞梺鍛婃处閸嬪棙瀵煎畝鍕拺閻犲洠鈧櫕鐏€闂佸搫鎳愭繛鈧柣娑卞櫍楠炴鎷犻懠顒夊敽闂備礁婀遍崑鎾诲箚瀹€鍕柈闁逞屽墴濮婂宕掑顑藉亾妞嬪海鐭嗗ù锝呮惈椤ユ碍銇勯幘璺盒㈤柛銊︾箖閵囧嫰寮介…鎴濇濠电偞鍨崹鍦矆閸愨斂浜滈柡鍐ㄦ搐椤ュ秹鏌熼懞銉︾闁诡喗顨婇悰顕€宕归鐓庮潛闂備胶绮〃鍡欏垝瀹€鍕ㄢ偓鏍ㄧ節閸ヮ灛鈺呮煃閸濆嫬鈧敻骞忓ú顏呪拺闁告稑锕﹂埥澶愭煥閺囨ê鍔ゅù婊冩啞鐎佃偐鈧稒顭囬崢浠嬫⒑缂佹ɑ鐓ョ€殿喛娉涢埢宥嗙節閸曘劌浜?UAC闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏㈡偘閳ュ厖澹曞┑鐐村灦閻燁垶鎮為悾宀€纾奸柣妯哄船瀹撳棝鏌＄仦璇测偓婵嬬嵁韫囨洑娌柛鎾椾讲鍋撴繝鍕＝濞达絼绮欓崫娲偨椤栨稑绗╅柣蹇撳暣濮婃椽宕ㄦ繝鍌氼潊闂佸搫鎳忕换鍡涘Φ閹版澘绀?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊為悷婊冪箻瀵娊鏁冮崒娑氬幗闂侀潧绻堥崺鍕倿閸撗呯＜闁归偊鍙庡▓婊堟煛瀹€鈧崰鏍蓟閸ヮ剚鏅濋柍褜鍓熷畷婵嬪箳閹惧墎顔曢悗鐟板閸犳洜鑺遍懡銈囩＜闁绘瑥鎳愮粙鑽ょ磼缂佹绠炲┑顔瑰亾闂佹寧绋戠€氼噣鎮鹃悽鐢电＝闁稿本姘ㄥ瓭闂佹寧娲忛崐婵嬪箖妤ｅ啯鍊婚柦妯侯樀閸炲爼姊洪崫鍕偍闁告柨绉瑰鏌ユ倷閻戞ǚ鎷洪梺鍦焾鐎涒晝澹曢悽鍛婄厱閻庯綆鍋呯亸浼存煏閸パ冾伃闁轰礁鍊块幐濠冨緞濡椿妫嗛梻鍌欒兌閹虫挾绮诲澶婂瀭鐟滅増甯掗悡姗€鏌熺€电袥闁稿鎹囬弫鎰償濠靛牊鏅奸梻浣侯焾閿曪箓宕戦妶澶婄畺鐎瑰嫰鍋婇悡銉╂煕閹扮鍋撻鍡楁处閻撴瑦銇勯弴妤€浜惧┑鐐板尃閸ラ绋忓┑鐘诧工閻楀棝鎮為崹顐犱簻闁圭儤鍨甸鈺冪磼閻樺磭澧ǎ鍥э躬婵″爼宕ㄩ鍏碱仱闁诲氦顫夐幐椋庢濮樿泛钃熸繛鎴旀噰閳ь剨绠撻獮瀣攽閸モ晙閭繝鐢靛У椤旀牠宕伴弽顓熺厐闁挎繂鎷嬪鏍ㄧ箾瀹割喕绨奸柛瀣剁節閺屸剝寰勭€ｎ亶鍤嬪┑顕嗙稻閸旀瑥顫忕紒妯诲缂佸娉曢惄搴ｇ磽閸屾氨小缂佽埖鑹鹃锝夘敃閿曗偓缁犳盯鏌ｅΔ鈧悧鍡涘焵椤掑倸鍘撮柡灞剧☉閳规垿宕卞Δ鍐ㄥ嚑闁荤喐绮岀粔褰掑灳閿旂偓宕夐柕濠忕畱绾绢垱绻涢幘鏉戝毈闁搞劋鍗冲畷婊堝箮閼恒儮鎷洪梺鍛婄☉閳洟顢旈崟顓熸濡炪倖甯掔€氼剟鎮″鈧弻鐔告綇妤ｅ啯顎嶉梺绋款儐閸旀瑩骞冭ぐ鎺戠倞闁搞儜鍐╂缂備焦鍞荤紞浣割潖缂佹ɑ濯撮柛娑橈攻閸犳劙姊洪崫銉バ㈢紒澶屾嚀閻ｇ兘鏁撻悩鑼唴闂佽姤锚椤﹂亶鎮惧ú顏呪拺缂備焦锚婵洭鏌熺喊鍗炰簽缂侇喖顭烽幃鐑藉箠婵犲倻绉洪柡浣瑰姍瀹曘劑顢欑捄顭戞敤闂傚倷绀侀幉陇鎽梺鍦嚀濞层倝锝炶箛鎾佹椽顢旈崨顓燁吋闂備線娼ч悧鍡涘箠韫囨稒鍊甸柣鎰靛墰缁♀偓闂侀潧绻堥崹娲吹閳ь剙鈹戦埥鍡椾簼闁荤啿鏅涢锝夊蓟閵夈儙銊╂煃閸濆嫬鈧宕㈡禒瀣厵闁稿繗鍋愰弳姗€鏌涙繛鍨偓鏇⑩€﹂崶顒€绠涙い鎾跺Х椤旀洟姊洪崨濠勬噧妞わ缚绮欓幃鐐寸鐎ｎ偆鍘靛Δ鐘靛仜閻忔繈鎮橀懠顑藉亾鐟欏嫭绀€闁靛牆鎲℃穱濠囨倻閽樺）銊ф喐瀹€鈧Σ鎰煥閸啿鎷绘繛杈剧悼閹虫捇顢氬鍕闁告瑥顦辨晶鐢碘偓瑙勬礃閸旀瑥鐣烽锕€唯鐟滃瞼绮径鎰拺閻熸瑥瀚崝璺衡攽椤斿搫鈧繈骞冮垾鏂ユ闁靛骏绱曢崢閬嶆⒑鐟欏嫬顥愰柡鍛⊕娣囧﹪鎮￠獮鐔烘嚀椤劑宕ㄩ婵囧媰濠电儑绲藉ú銈夋晝椤忓嫮鏆﹂柨婵嗘缁剁偤鎮楅敐搴濈盎闁衡偓椤撶喓绡€婵炲牆鐏濋弸娑㈡煥閺囶亜顩柛鎺撳浮椤㈡盯鎮欓懠顒夊數闂備礁鎲＄粙鎴︽偤閵娾晛纾婚柨鐔哄У閳锋垶銇勯幘鍗炲闁伙綀椴哥换娑㈠醇閻旇櫣鐣肩紓浣介哺閹瑰洤鐣烽幒鎴旀瀻闁规惌鍘借ⅵ濠碉紕鍋戦崐鏍垂闂堟党娑樷攽閸℃瑦娈鹃悷婊呭鐢帡鏌ㄩ妶鍡曠箚闁靛牆鍊告禍鍓х磽娴ｅ搫啸濠电偐鍋撻梺鍝勭焿缂嶄礁顕ｉ鈧畷鎺懳旀担鍝ュ降闂傚倷娴囧銊х矆娴ｈ櫣鐭撶憸鐗堝笒閽冪喖鏌ㄥ┑鍡╂Ц缂佺姵绋掗妵鍕箻鐠虹儤鐏佺紓浣割儏濞诧妇鎹㈠☉姘ｅ亾閻㈡鐒剧€涙繈姊洪棃娑氬ⅱ妞ゎ厼鐗撻敐鐐剁疀閹句焦妞介、鏃堝椽娴ｈ妫冨┑鐘垫暩閸嬬偤鎮鹃鍫濈９闁哄稁鍘介崕搴亜閺嶃劍鐨戦柡鍡畵閺岋綁寮崒妤佸珱闂佽桨绀侀崯鎾蓟閵娿儮鏀介柛鈩冿供濡倝鎮峰鍛暭婵＄偠妫勯～蹇旂節濮橆剛锛滃┑顔斤供閸忔﹢宕戦幘鎼Ч閹艰揪绲块悞鍏肩箾閹炬潙鐒归柛瀣尰椤ㄣ儵鎮欏顔解枅闂佺硶鏅涚€氭澘鐣峰鈧、鏃堝幢濡儤鐤勬繝鐢靛Т閻ュ寮舵惔鎾充壕闁哄洢鍨圭粻鐘诲箹濞ｎ剙濡肩痪鎯с偢閺屾洘绻涢崹顔煎闂佸搫鍟悧濠囧疾閺屻儲鐓曟繛鎴濆船楠炴绻涢崼顐㈠箹闁宠鍨块、娆撴倷椤掆偓濞堝矂鎮楀▓鍨灈闁绘牕銈搁悰顕€宕奸妷銉庘晠鏌曟径濠冩崳闁哥姵鐗曢悾鐤亹閹烘繃鏅╅梺璇″瀻閸愵亞绐楅梻鍌氬€搁崐鐑芥嚄閸撲礁鍨濇い鏍仜缁犱即鎮归崶顏嶆⒖婵炲樊浜濋崑鍕煟閹捐櫕鍞夐柟鑺ユ礋濮婅櫣绱掑Ο鑽ゅ弳闂佸憡鑹鹃澶婄暦閹邦喚纾兼俊顖濐嚙瀵灝鈹戞幊閸婃劙宕戦幘缈犵箚妞ゆ劧绲块幊鍥煙椤曗偓缁犳牠骞冨鍫熷殟闁靛鍨虹€氬ジ姊洪崫鍕垫Ш闁稿鍋炵粩鐔煎幢濞嗘垹骞撻梺鍝勫€归敍鏇㈡偡闁妇鍙嗛梺鍛婂姦娴滄粓鎮￠悢鍏煎€垫繛鍫濈仢閺嬫稒銇勯鐘插幋鐎殿噮鍋勯鍏煎緞婵犲嫷妲规俊鐐€曠换鎰板箠鎼淬垹鍨斿ù鐓庣摠閳锋帒霉閿濆懏鍟為柟顖氱墦閺屾稒绻濋崒婊冪厽閻庤娲橀崝鏇㈠煡婢舵劕顫呴柣妯兼磪閺囩儐娓婚柕鍫濇婵囨⒒閸曨偄顏柛鈺傜洴楠炲鏁傞悾灞藉箞闂備焦瀵уΛ渚€顢氳椤曪絾銈ｉ崘鈺冨幈闂佸搫鍟犻崑鎾剁磼椤斿吋鎹ｆ俊鍙夊姍楠炴帒螖閸愩劌绲兼俊鐐€栭幐鐐叏閻戣姤鍋傞柟鍓х帛閳锋垿鏌ｉ幇顖涱棄闁告梻鏁哥槐鎺楁偐閾忣偄闉嶆繛锝呮搐閿曨亪鐛弽銊﹀闁告縿鍎遍獮鎰攽閻愯埖褰х紒鑼跺亹閹峰啴鏁冮埀顒€宓勬繛瀵稿帶閻°劑鍩涢幒鎴欌偓鎺戭潩閿濆懍澹曟繝鐢靛仒閸栫娀宕卞Δ渚囦哗闂傚倷娴囬褏鎹㈤幒妤€纾婚柣鎰暯閸嬫挸顫濋浣告敪濡炪倧绠掗～澶愬箞閵婏妇绡€闁稿被鍊楅崥瀣倵鐟欏嫭绀冮悽顖涘浮閸┿垺鎯旈妸銉ь唺闂佺懓鐡ㄧ换鍌炴嚈閹邦喚纾介柛灞捐壘閳ь剟顥撳▎銏ゆ晸閻樻煡妫峰銈嗘磵閸嬫挾鈧娲栫紞濠傜暦婵傜鍗抽柣鎰悁缁辨粓姊绘担鐟邦嚋婵炲弶鐗犲畷鎰板箹娴ｅ湱鍘戦梺鎼炲労閸撴岸鎮¤箛鎿冪唵閻犻缚娅ｆ晶鏇㈡煕閺冣偓缁秵绌辨繝鍌ゅ晠妞ゆ梻鍘ч埛瀣攽椤旂》鏀绘俊鐐舵閻ｇ兘顢曢敃鈧粈瀣亜閹惧鈯曢柣搴ㄤ憾濮婄粯鎷呴崨濠傛殘闂佹寧宀搁弻鏇㈠醇閻旂鈧劖顨ラ悙鎻掓殻闁诡喗鐟╁畷顐﹀礋椤愩倐鍋撻鐑嗘富闁靛牆妫楁慨澶娾攽椤旇偐锛嶉柤楦块哺缁绘繂顫濋娑欏闂備浇宕甸崰鎰珶閸℃稑绠洪柣妯兼暩绾惧ジ鎮归崶銊ョ祷闁哄棴绲块埀顒冾潐濞叉﹢宕归幐搴濈箚闁绘垼濮ら弲婊堟偣閹帒濡块柣锝夌畺濮婄粯鎷呴崨濠傛殘闂佸湱顭堝Λ婵嗙暦濮椻偓婵℃悂濡疯閸氬懘姊?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵婵炲棗绻掓禒楣冩⒑缁嬫鍎嶉柛濠冪箞瀵寮撮悢铏诡啎闂佺粯鍔﹂崜姘舵偟閺冨牊鈷戦柛娑橈龚婢规绱撳鍜冭含闁绘侗鍣ｅ浠嬧€栭妷銉╁弰妞ゃ垺顨婇崺鈧い鎺嶆缁诲棝鏌ゅù瀣珖婵☆偒鍨堕弻鐔烘喆閸曨偄袝濠电偛鍚嬮崝鏇″絹闂佹悶鍎滃鍫濇儓濠电姷顣介埀顒€纾崺锝団偓瑙勬磸閸旀垿銆佸鈧幃銏ゅ礈閹绘帗鏆㈤梻?CLI闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ礂閻撳簶鍋撶紒妯诲弿婵°倐鍋撴俊顐ｇ懇閹箖鎮滈懞銉㈡嫼闂佺鍋愰崑娑㈠焵椤掍焦绀嬮柟宕囧枛椤㈡稑鈹戦崼婊呰兒闂傚倸鍊烽悞锔锯偓绗涘懏宕查柛宀€鍋涚粻鐘诲箹濞ｎ剙濡兼慨瑙勭叀閺岋綁骞囬鐓庡闂佺粯鎸诲ú鐔煎箖濡ゅ懏鏅柛妤冨仜椤忣亪鏌ｅ┑鍥у摵婵﹥妞藉畷顐﹀礋椤撶姴濮界紓鍌欑劍瑜板啴鍩€椤掑倸鏋斿ù婊€绮欏缁樻媴閸涘﹤鏆堢紓浣筋嚙閸婂鍩€椤掍礁鍤柛妯垮椤ｉ箖姊鸿ぐ鎺戜喊闁告搫绠戦妴鎺撶節濮橆厾鍘梺鍓插亝缁诲啴藟閻愮數纾奸柍褜鍓氬鍕節鎼淬垹鐦滈梺璇插缁嬫帟鎽梺绋匡攻閸旀牠銆冮妷鈺傚€烽柤纰卞墮閳灚绻濈喊妯哄⒉闁诡喖鍊规穱濠囨倻缁涘鏅╅梺鑺ッˇ杈╂閿曞倹鈷掗柛灞剧懆閸忓瞼鐥鐐靛煟鐎规洘绮岄埞鎴犫偓锝冨妷閸嬫捇宕橀鐓庣獩闁哄鐗嗘晶鐣岀矙韫囨稒鈷戦柛鎰级閹牓鏌熼崘鑼闁诡噯绻濋幃銏ゅ礂閼测晛甯鹃梻濠庡亜濞层倝鏁冮妷銉㈡瀺闁告稑鐡ㄩ悡鐔肩叓閸ャ劍鈷掓繛鍙夋尦閺岋紕浠﹂崜褉濮囬梺璇″灡濡啴寮幇鏉垮耿婵☆垵宕靛Σ妤€鈹?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖闂備礁鎲￠幐鍡涘川椤旂瓔鍟呴梻鍌欐祰椤曆呪偓娑掓櫊閹虫繃銈ｉ崗顖氭处鐎靛ジ寮堕幊绛圭畵閺屾盯寮撮妸銉т哗缂備胶濮垫繛濠囧蓟閻旇　鍋撳☉娆樼劷缂佺姰鍎遍湁闁绘宕甸悾鐑樻叏婵犲懏顏犵紒顔界懇瀹曠螖婵炴儳浜炬俊銈勯檷娴滄粓鏌ㄩ弮鍌氫壕闁哄棭鍓涢埀顒侇問閸犳盯顢氳閸┿儲寰勬繛銏㈠枛閺屻劎鈧絽鐏氱紞灞解攽閻樺灚鏆╅柛瀣☉铻炴繛鍡樻尭缁€澶嬩繆閵堝懎鏆熼柣顓熺懇閺屾盯濡烽鐓庮潽闂?winget 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟扮粻缁橆殽閻愭潙鐏村┑顔瑰亾闂侀潧鐗嗛幊鎰版偪閳ь剚淇婇悙顏勨偓鏍涙担鑲濇盯宕熼浣稿妳婵犵數濮村ú锕傚煕閹寸姵鍠愰柣妤€鐗嗙粭鎺懨瑰鈧崡鎶藉蓟濞戔懇鈧箓骞嬪┑鍡╂浇缂傚倷绀侀崐鍝ョ矓瑜版帒绠栭柣鎴ｅГ閻掍粙鏌ㄩ弮鈧崕鎶藉极閹间焦鈷戦柤濮愬€曢埢鍫㈢磽閸屾稖澹橀柍璇茬Ч閺佹劖寰勭€ｎ亖鍋撻崼鏇炵骇闁割偅纰嶅▍鍥╃棯閹佸仮闁哄矉绲借灒闁圭娴疯ぐ褍顪冮妶鍐ㄥ姎闁绘鎹囧濠氭晬閸曨亝鍕冮梺鍛婃寙閳ь剙危閸儲鈷戦悗鍦У椤ュ銇勯敃鍌涙锭闁伙絿鍏橀弫鎾绘偐閸愭祴鍋撻悜鑺ュ€甸柣銏犳啞濞呮粓鏌熼懠顒€浜剧紒?Cloudflare 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟版晥闂佹寧绻勯崑娑㈠煘閹寸姭鍋撻敐搴′簼婵炲懎娲铏圭矙鐠恒劎鍔规繝纰樷偓铏窛缂侇喗鐟ㄧ粻娑㈠籍閸屾粎妲囬梻渚€娼ф蹇曞緤閸撗勫厹濡わ絽鍟崐鍨箾閹寸偛绗氭繛鍛攻椤ㄣ儵鎮欑拠褍浼愰梺浼欑稻缁诲牆鐣烽悢纰辨晞闁兼亽鍎遍鐑樼節閻㈤潧浠╅柟娲讳簽瀵板﹥绂掔€ｎ亞鐤呴梺璺ㄥ枔婵绮诲鑸电厱妞ゆ劗濮撮崝婊堟煟閹捐泛鏋戦柕鍥у楠炴帡宕卞鎯ь棜闂傚倷绶氬褔鎮ц箛娑掆偓锕傚醇閵夛箑浠奸梻浣哥仢椤戝懘顢氶柆宥嗙厸濠㈣泛顑呴悘鈺傘亜閺冣偓鐢繝寮婚敐澶嬪亹闁告瑥顦遍埞娑㈡煟韫囨挾绠查柣鐔叉櫅閻ｅ嘲鈹戠€ｎ偅娅囬梺閫炲苯澧撮柍銉︽瀹曟﹢顢欓崲澹洦鐓曢柍鈺佸枤濞堟﹢鏌ｉ悢鏉戝婵﹨娅ｇ划娆撳礌閳ュ厖绱ｇ紓鍌氬€哥粔鎾晝閵堝鍋╅柣鎴ｅГ閸嬨劎绱掔€ｎ厽纭堕柛鏃撶畱椤啴濡堕崱妤€娼戦梺绋款儐閹稿濡甸崟顖ｆ晜闁告洦鍋呭▓缁樼節绾版ê澧查柛搴ゅ亹缁鈽夊Ο閿嬵潔濠电偛妫欒彜闁稿鎸搁濂稿幢濞嗘垹妲囬梻浣圭湽閸ㄨ棄顭囪缁傛帒顭ㄩ崼鐔哄弳闂佸搫娲﹂敋濠碘€炽偢閹藉爼鎮欓悜妯煎帾闂婎偄娲㈤崕宕囧鐠恒劎纾奸柍閿亾闁稿鎹囧缁樻媴閸︻厽鑿囬梺鎼炲姀濡嫰鈥﹂崶顏嶆Щ闂佺儵妲呴崢鐣屾崲濠靛鍋ㄩ梻鍫熺◥缁爼姊哄ú璁崇胺闁告鍟块悾鐑藉箛椤戔晜妫冨畷姗€濡搁妷褌鍠婇梻鍌欑婢瑰﹪宕戦崨顖涘床闁告洦鍨板Ч鏌ユ煕閳╁啰鈯曢柣鎾存礋閺岋繝宕堕妷銉ヮ瀳婵犫拃鍌氬祮闁哄本绋戣灒闁告繂瀚崺鐐烘⒑鐎圭媭娼愰柛銊ユ健楠炲啫鈻庨幘宕囬獓闂佺懓顕慨鍐睬庨鈧弻锝嗘償閵堝孩缍堝┑鐐额嚋缁犳挸鐣峰鍐炬僵閺夊牃鏅濋悞鍧楁倵楠炲灝鍔氭い锔诲灣缁鈽夐姀锛勫幗濠德板€愰崑鎾绘煟濡も偓缁绘ê鐣?
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊鐒﹂崕鎾绘⒑閹肩偛濡奸柛濠傛健瀵鈽夐姀鈺傛櫇闂佹寧绻傚Λ娑⑺囬妷褏纾藉ù锝呮惈灏忛梺鍛婎殕婵炲﹤顕ｇ拠娴嬫婵☆垶鏀遍弬鈧梻浣告啞濞诧箓宕戦崟顒佸弿闁靛繈鍊栭埛鎴炵箾閼奸鍤欐鐐搭殜閺岀喖鎮烽悧鍫熸倷濡炪値鍋勭换鎴犳崲濠靛棭娼╂い鎺戝閺佸綊姊绘担铏瑰笡闁哄被鍔戝畷鏌ュ蓟閵壯冪ウ闂婎偄娲︾粙鎺楀煕閹达附鐓欑紒瀣閹癸絿绱掗悩铏鞍缂佺粯绋撴禒锕傚磼濮橆剦娼庨梻浣筋嚃閸ㄤ即鎳熼鐐茬厺閹兼番鍔岀粻濠氭倵閻㈡鐒炬鐐茬墦濮婄粯鎷呴搹鐟扮闂佸搫琚崝鎴濈暦閺屻儱钃熼柕澶堝劜鏉堝牓姊虹紒妯哄闁稿簺鍊濆畷鎴﹀Ω閿旇桨绨婚梺鍝勭Р閸斿酣鎯屾繝鍥ㄧ厸閻庯綆鍋呴埛鎺旂磼鏉堛劌娴い銏＄懃閳诲酣骞嗚椤斿嫰姊绘担鍝ユ瀮婵☆偄瀚拌棟闁割煈鍋呴崣蹇涙煃瑜滈崜鐔煎蓟閿濆憘鐔访虹拠鍙夋珱闂備礁鎽滈崰搴敄婢跺娼栫紓浣股戞刊瀵哥磼濞戞﹩鍎愮紓宥呯墦濮婅櫣绮欓幐搴㈠闯闂佸憡顭嗛崱娆庣綍闂傚倷绀侀幉鈥趁洪敃鍌氱獥闁规儳顕々?Windows 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨绘い鎺嬪灪閵囧嫰骞囬鍡欑厯闂佸搫琚崝鎴﹀箖閵忋倕浼犻柛鏇熷灟閸ㄨ崵妲愰幒妤€绠涙い鎾楀嫮鏆︾紓鍌欒兌缁垶鎯勯鐐靛祦閻庯綆鍠楅崐鐑芥煕椤垵浜滄繛鍛殜濮婄粯鎷呴崨濠傛殘闂佽崵鍠嗛崕鎶藉箲閵忕媭娼ㄩ柍褜鍓欓锝嗙節濮橆剙宓嗛梺闈涚箳婵炩偓闁哥偠娉涢埞鎴︽偐缂佹ɑ閿┑鐐茬湴閸婃繈骞冮悽绋跨睄闁割偆鍟块幏?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏㈡偘閳ュ厖澹曞┑鐐村灦閻燁垶鎮為悾宀€纾奸柣妯哄船瀹撳棝鏌＄仦璇测偓婵嬪箖瑜斿畷鐓庘攽閹邦剚鐤傞梻鍌欑窔濞佳兠洪妶鍥ｅ亾濮樺崬顣奸柟渚垮姂閸┾偓妞ゆ帒瀚悡蹇涚叓閸パ屽剰闁诲浚浜濋妵鍕償闄囬崺妤呮⒒閸屾艾鈧悂宕愰幖浣哥９闁归棿绀佺壕褰掓煙闂傚顦︾痪鍓ф嚀椤啰鈧綆浜滈銏°亜閹邦垰袚闁逛究鍔岄～婊堝幢濡も偓閳锋帡姊虹紒妯虹闁哄拋鍋婇獮澶岀矙濞嗘儳鎮戦梺鍓插亽濠⑩偓缂傚啯娲熷濠氬磼濞嗘埈妲梺鍦拡閸嬪﹤鐣烽鐑嗘晝闁挎棁妫勬禍杈ㄧ節閻㈤潧孝婵炲眰鍊濆畷銉ㄣ亹閹烘挾鍘遍梺鍝勬储閸斿矂鎮橀敓鐘崇厽闁规儳鐡ㄧ粈瀣煛瀹€鈧崰鏍嵁閸℃凹妲荤紓浣哄珡閸涱垳锛滄繛杈剧秬椤娆㈤弻銉︾厓閻熸瑥瀚悘瀛樸亜閵忥紕鎳囬柟绋匡攻瀵板嫰宕卞Ο闂寸敖闂傚倸鍊峰ù鍥敋閺嶎厼绐楅柡宥庡亞缁€濠傗攽閻樺弶宸濈€规挷绀侀妴鎺戭潩閿濆懍澹曟俊銈囧Х閸嬬偟鏁敓鐘靛祦閻庯綆浜栭弨浠嬫煙闁箑鏋撻柛瀣尭閻ｏ繝骞嶉搹顐ｆ澑闂備胶绮崝鏍ь焽濞嗘挻鍊堕柨鏇炲€归悡娆戠磼鐎ｎ偄顕滈柟鎻掓憸缁辨帡顢欓懞銉⒖闂侀潧娲﹂崝娆撶嵁閹烘绠ｉ柨婵嗘閸熸椽姊虹拠鏌ヮ€楃紒鐘茬Ч瀹曟洟鏌嗗鍡椾罕濠电姴锕ら崰姘舵倵閸洘鐓忓┑鐐靛亾濞呭棝鏌嶉柨瀣伌闁哄本绋戦埥澶婎潨閸喐鏆伴梺璇茬箰缁绘帡寮繝姘摕婵炴垯鍨洪崑鎰偓瑙勬礀濞层倝鍩涘畝鍕拺缂備焦眉缁堕亶鏌涢敐蹇曞埌闁伙絽鍢查～婊堝焵椤掑嫨鈧礁螣濞嗙偓歇闂備礁鐤囬褔鎯岄崒鐐茶摕闁挎繂妫欓崕鐔兼煏閸繂顒㈢紓宥呯墕閳规垿鎮欓幓鎺旈獓缂備緡鍠楅幐鎼佹偩閻戣棄顫呴柨婵嗗閳诲矂姊绘担铏广€婇柡鍛箞瀹曟垿濡舵径灞界ウ闁诲函缍嗛崰妤呭疾濠靛鐓冪憸婊堝礈閻旂厧钃熼悗锝庡枛閻愬﹪鏌曟繛褍鎳愬Σ鍥⒒閸屾艾鈧悂鎮ф繝鍕煓闁规儳鐡ㄥ▍鐘绘煛閸モ晛娅忔繛鍫滅矙閺岋綁骞囬妸锔界彅婵炲瓨绮撶粻鏍ь潖婵犳艾纾兼慨姗嗗厴閸嬫捇骞栨担鍝ワ紮閻庣懓瀚崳纾嬨亹閹烘垹鍊為悷婊冪箻瀵娊鏁冮崒娑氬幗闂侀潧绻堥崺鍕倿閸撗呯＜闁归偊鍙庡▓婊堟煛瀹€鈧崰鏍嵁瀹ュ鏁婄痪鎷岄哺濮ｅ姊绘担渚劸妞ゆ垶鍨归幑銏犫攽鐎ｎ亣鎽曢梺鍝勬储閸ㄥ綊鏌嬮崶銊х瘈闂傚牊绋掗幖鎰亜閿旇娅呮い顏勫暣婵″爼宕卞Δ鈧～鍫㈢磽娓氬洤鏋熼柣鐔叉櫅椤曪絾绻濆顒€宓嗛梺闈涚箳婵炩偓闁哥偠娉涢埞鎴︽偐缂佹ɑ閿┑鐐茬湴閸婃繆妫熼梺瀹犳〃缁垛€炽€掓繝姘厪闁割偅绻堥妤€霉濠婂嫮绠栫紒缁樼洴瀹曪絾寰勫☉婧惧亾閹稿寒娈介柣鎰皺婢э箑鈹戦埄鍐╁€愬┑锟犳敱閹棃鏁愰崨顔句邯婵°倗濮烽崑鐐哄礉閺嶎厼绠氶柡鍐ㄧ墛閺呮煡鏌涢幇鍓佸埌濞存粓绠栭弻鐔兼偋閸喓鍑＄紓?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶鏉款熆鐟欏嫭绀冪紒缁樼箓椤繈顢橀悩鎻掔疄闂傚倷鐒﹂弸濂稿疾濞戙垹鐤ù鍏兼綑閻撯€愁熆閼搁潧濮堥柣鎾存礋閹﹢鎮欐０婵嗘婵犳鍠栭崐鍧楀蓟閿涘嫪娌悹鍥ㄧゴ閸嬫捇寮撮姀锛勭暢闂備浇顕ч崙鐣岀礊閸℃稑纾婚柛娑樼摠閸嬬喖鏌￠崶鈺佸壉婵炴挸顭烽弻鏇㈠醇濠靛鈧霉濠婂嫮绠栫紒缁樼洴閹崇娀顢楅埀顒勫几濞戞埃鍋撳▓鍨灈妞ゎ厾鍏橀獮鍐閵堝懎绐涙繝鐢靛Т鐎氼喛鍊撮梻鍌氬€风粈渚€骞夐敓鐘茬闁糕剝绋戝婵囥亜閺冨倻鎽傞柛鐔锋嚇濮婂宕掑▎鎰偘閻庤娲滈弫璇茬暦娴兼潙绠涢柣妤€鐗嗘禒顓㈡煛婢跺﹦澧愰柡鍛箖缁嬪顓兼径瀣幍闂佺顫夐崝鏇㈠触閸︻厸鍋撶憴鍕矮缂佽埖宀稿濠氬灳閹颁礁鎮戦柟鑹版彧缁茶偐鍒掗幘缁樼厵闁稿繐鍚嬮崕妤呮煟閹惧磭澧︾€殿噮鍋婂畷鍗炩槈濞嗘垵甯楅柣鐔哥矋缁挸鐣峰鍫熷亜闁绘挸娴烽悾鍝勨攽椤斿浠滈柛瀣崌閺屾盯鍩＄€ｎ剛鐦堥悗瑙勬磸閸旀垿銆佸鈧幃鈺呭垂椤愶絿妲ラ梻鍌氬€搁崐鐑芥嚄閸撲礁鍨濇い鏍仜閽冪喖鏌曟繛鐐珔閻熸瑱绠撻幃妤呮晲鎼粹剝鐏堥梺缁樻尰濞茬喖寮婚悢鍛婄秶濡わ絽鍟宥夋⒑缂佹ɑ灏版繛鍙夘焽閹广垹鈹戠€ｎ偒妫冨┑鐐村灦閻燁垰螞閻愬樊娓婚柕鍫濇噺缁傚鏌涚€ｎ亜顏柍銉畵閹虫顢涢敐鍡忔瀸婵犵數鍋涢悺銊у垝瀹€鍕亯闁绘挸瀵掗崵鏇㈡偣閸ャ劎銈存俊鎻掔墦閺屾洝绠涢弴鐐愩儲銇勯弮鈧敮锟犲蓟閿濆棙鍎熼柕濠忛檮闁款參姊虹涵鍜佸殝缂佺粯绻傞悾椋庘偓鍦濡嫬鈹?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶鏉款熆鐟欏嫭绀嬮柟顔惧厴楠炲﹥绻濋崒婊呅ㄩ梺璇″枓閺呮盯顢欒箛娑辨晩闁绘挸楠歌灇缂傚倸鍊搁崐鎼佸磹閸濄儳鐭撻柣銏犳啞閸嬪鏌￠崶锝嗗櫚闁逞屽墾缁犳挸鐣风粙璇炬梹鎷呴崨濠勬澓闂傚倸鍊搁崐鎼佹偋婵犲嫮鐭欓柟杈捐缂嶆牠鏌涢妷顔煎闁抽攱鍨垮娲敃閵堝懍绮堕梺鍏兼た閸ㄨ泛鐣烽姀銈呬紶闁靛／鍜冪床闂備線鈧偛鑻晶顖滅磼濡ゅ啫鏋涢柛鈹惧亾濡炪倖甯掔€氥劑鍩€椤戣法顦︽い顐ｇ矒閸┾偓妞ゆ巻鍋撻柣锝呭槻铻栭柛娑卞枛娴滄鏌熼懝鐗堝涧缂佹彃娼￠幃浼搭敊鐏忔牗鏂€闂佺粯锚閻忔岸寮抽埡浣叉斀妞ゆ梻鈷堥崵鐔兼煃瑜滈崜姘潩閵娾晛绀傛俊顖濇閺嗭附銇勯幒鎴濐仾妞ゃ儱鐗婄换娑㈠箣閻愯尙鐟查梺鍝勫€甸崑鎾绘⒒閸屾瑨鍏岀紒顕呭灦閺佸啴鍩￠崨顓犵崶闂佸搫绋侀崢浠嬪磹閸洖绾ч柛顐ｇ☉婵¤法绱掗悩鍐插姢妞ゎ叀娉曢幑鍕瑹椤栨艾澹嬫繝鐢靛仜瀵爼骞愰幎钘夎摕闁斥晛鍟刊瀵哥磼鐎ｎ偄顕滄繛鍫㈢帛缁绘繄鍠婃径宀€锛熼梺鍝勮嫰椤︿絻妫熼梺鍦檸閸犳鎮￠悢鎼炰簻闁规崘娉涙禒锔姐亜閵夛絽鐏查柡宀嬬磿娴狅箓鎮剧仦婵勫劜閵囧嫯顦辩紒鑸靛哺瀵鈽夐姀鐘殿啋闁荤姾娅ｉ崕銈夋倵鐠囧樊娓婚柕鍫濋娴滄粎绱掔紒妯哄闁告瑥鎳庨—鍐Χ閸涱垳顔囬柣搴㈠嚬娴滅偛危閹版澘閱囬柡鍥╁暱閹锋椽姊洪崨濠勨槈闁挎洏鍎崇划鍫ュ焵椤掑嫭鈷戦悹鍥皺缁犳彃菐閸ャ劍銇濈€规洘鍔欓獮姗€顢欑憴锝嗗闂備胶顭堥張顒勬偡閵娾晛绀傞悘鐐板嫎娴滄粍銇勯幘璺轰户濠⒀佸灲閺屽秷顧侀柛鎾寸懇閹ê顫濇潏銊︽闂佺偨鍎辩壕顓㈠汲閿旂偓鍠愰柣妤€鐗嗙粭鎺楁⒑閸楃偞鍠橀柡宀嬬節瀹曞爼鍩℃担閿嬪媰婵犵鈧櫕鈻曢柛鎾跺枎椤?
//
// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊鐒﹂崕鎾绘⒑閹肩偛濡奸柛濠傛健瀵鈽夐姀鈺傛櫇闂佹寧绻傚Λ娑⑺囬妷褏纾藉ù锝呮惈鏍￠悷婊勬緲閸熸潙锕㈡笟鈧娲箰鎼达絿鐣甸梺鐟板暱闁帮綀妫㈤棅顐㈡处濮婅崵绮绘ィ鍐╁€垫繛鎴炵懐閻掕棄顭胯閺佸寮婚敍鍕勃闁绘劦鍓涢ˇ銊х磽娴ｈ櫣甯涚紒瀣墵钘濋柡灞诲劜閻撴洟鏌ｉ弴姘鳖槮濞存粈鍗抽弻鐔碱敊缂傚瓨鍨归幑銏犫攽鐎ｎ亞鍊為梺闈涱焾閸ㄨ櫣鈧艾銈稿缁樻媴閸涘﹤鏆堥梺鐟版啞濡叉帡濡甸幇鏉跨闁冲搫鍠氬ú鎼佹⒑缂佹ê濮夐柛搴涘€濋幃锟犲即閵忥紕鍘藉┑鈽嗗灡鐎笛囨偟椤忓棌鍋撶憴鍕闁活厼鍊垮璇测槈閵忕姷鍘撮梺鍓茬厛閸犳碍瀵奸崼銉﹀€垫繛鍫濈仢閺嬬喖鏌熷灞剧彧闁逛究鍔戦崺鈧い鎺戝閻撱儲绻涢幋鐏活亪顢旈埡鍌ゆ闁绘劘灏欐晶娑樓庨崶褝韬柟顔界懇椤㈡棃宕熼妸銉ゅ闂佸搫绋侀崢鑲╃矆婢跺绻嗛柕鍫濆閸斿秹鏌涘▎蹇曠闁哄瞼鍠撶槐鎺楀閻樺磭浜紓鍌欒閸嬫挸顭跨捄渚剳缂佲檧鍋撳┑鐘垫暩婵挳宕愰幖浣哄祦婵☆垰鍚嬮崰鎰版煟濡も偓閻楀棛绮鑸电厱閻庯綆鍋勬慨澶岀磼鏉堛劌绗掗柍钘夘槸椤繈宕楅崗鍏兼闂傚倷绶氶埀顒傚仜閼活垱鏅堕鐣岀缁绢參顥撶弧鈧悗娈垮枛椤兘骞冮姀銏犳瀳閺夊牄鍔嶅▍鎾绘⒒娴ｇ瓔鍤冮柛銊ラ叄瀹曟﹢濡歌閸氬懘姊绘担绛嬪殭閻庢稈鏅犻、娆撳冀椤撶偟鐛ュ┑掳鍊愰崑鎾淬亜椤撶偞鎼愰柍璇查叄楠炴瑩宕熼鐘（濠电姷鏁告繛鈧繛浣冲厾褰掑磼濮樿偐鍔峰┑鐐叉閹稿鎮″☉姘ｅ亾楠炲灝鍔氶柛鐕佸亝缁旂喐寰勭€ｎ剛鐦堟繝鐢靛Т閸婄粯鏅跺☉銏＄厓闂佸灝顑呯粭鎺楁婢舵劖鐓ユ繝闈涙－濡牊淇婇锝呯仴闁宠棄顦甸獮姗€鎮烽幍顔筋嚄缂傚倷娴囨ご鍝ユ暜閳ュ磭鏆﹂柨婵嗘缁剁偛鈹戦悩鏌ヮ€楁繛鍫熺箘缁辨捇宕掑▎鎺戝帯缂備緡鍣崹鍫曞箚閸曨垼鏁嶉柣鎰级濡差剟姊洪柅鐐茶嫰婢ь垶鏌曢崶褍顏鐐村浮楠炲鈹戦幇顏嗘／闂傚倷鑳剁划顖炲箰閼姐倗鐭欓柟杈鹃檮缁犳帡姊绘担鐟邦嚋缂佽鍊块獮濠呯疀濞戞鐤呴梺鍛婄缚閸庮噣宕戦幘鏂ユ婵炲棙鍨归悾闈涱渻閵堝簼绨婚柛鐔风摠娣囧﹪鎳滈崹顐㈠妳闂佺偨鍎寸亸娆撳箰婢舵劖鈷掑ù锝堫潐閸嬬娀鏌涢弬娆惧剳闁告帗甯掗埢搴ㄥ箻閺夋垳绮ф俊鐐€栧ú宥夊磻閹剧粯鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍ь啅婵犳艾纾婚柟鍓х帛閻撳啴鎮峰▎蹇擃仼闁诲骏濡囬埀顒€鐏氬妯尖偓姘煎櫍閸┾偓妞ゆ帒锕︾粔鐢告煕鐎ｎ偄绗掓い?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閻樻爠鍥ㄧ厱閻忕偛澧介悡顖氼熆鐟欏嫭绀€闁宠鍨块、娆撴儗椤愵偂绨婚柣锝囧厴椤㈡宕熼銏犱憾闂佽娴烽弫鍝ユ兜閸洖纾婚柟鎹愬煐閸犲棝鏌涢弴銊ュ妞わ负鍎崇槐鎾诲磼濮樻瘷銏ゆ煥閺囥劋绨绘い鏇稻缁绘繂顫濋鈧懓鍨攽鎺抽崐鏇㈡晝閵堝绠栭柟杈鹃檮閳锋垿鏌涘☉姗堟缂佸爼浜堕弻娑㈡偐瀹曞洢鈧帗绻涢崱鎰伈濠殿喒鍋撻梺闈涚墕濡矂骞忕紒妯肩閺夊牆澧介幃濂告煙閾忣偅宕屾い銏¤壘椤劑宕ㄩ婊愮床闂佽崵濮村ú锕併亹閸愵喖鐒垫い鎺戝暙閸斿灚淇婇銈呮瀻妞ゎ亜鍟存俊鍫曞幢濞嗗浚娼峰┑鐘灱椤煤閻旈鏆﹂柟杈剧畱缁犲鏌熸０浣哄妽闁稿秹娼ч—鍐Χ閸℃鐟ㄥ銈忓婢ф寮查妷褏纾介柛灞捐壘閳ь剛鍏橀幊妤呭礈娴ｇ鐏婂銈嗙墬缁酣鎯屽▎鎾寸厵閺夊牊鍎抽惁婊堟煛娴ｉ潻韬柣鎿冨亰瀹曞爼濡搁妶鍥╃畳闂備浇妗ㄧ粈渚€鏁冮鍫濊摕闁靛牆妫欓崣蹇涙煙闁箑鍘撮柛瀣崌瀵挳濮€閻樺弶鍎俊鐐€栫敮鎺斺偓姘煎弮閸╂盯骞掗幊銊ョ秺閺佹劖寰勫Ο娲绘濠电偞鍨堕幑浣轰焊濞嗘垶顫曢柟鐑橆殕閸嬫劙鏌涢幇顖氱毢濞寸厧鐭傚铏规嫚閳ヨ櫕娈梺鎼炲劀閸滀礁鏅梻鍌欑缂嶅﹪銆傞敃鍌涘€块柨鏂垮⒔缁€濠囧箹缁顎嗛柡鈧禒瀣厽闁归偊鍓欑痪褔鏌嶇紒妯荤闂囧绻濇繝鍌氭殶闁糕晪绲块埀顒侇問閸ｎ噣宕戞繝鍌滄殾闁告鍎愬〒濠氭煛閸モ晛浠х紒澶嬫そ閺屸剝鎷呴悷鏉款潚閻庤娲栭妶鎼併€侀弴銏℃櫜闁搞儮鏁╅妷鈺傗拻闁稿本鐟ㄩ崗灞俱亜椤撶偟澧㈤柍褜鍓氱粙鍫ュ疾閻樿崵宓侀柟鐗堟緲缁€鍫㈡喐鎼搭煉缍栭柡鍥ュ灪閻撴瑩鏌熼鍡楁噺閹插吋绻濆▓鍨仭婵炲吋鐟╁﹢渚€姊虹紒妯忣亜螣婵犲洤纾块煫鍥ㄧ⊕閻撴瑩鏌ｉ敐鍛拱闁哄棌鏅濈槐鎺撴綇閵婏箑闉嶉梺鐟板槻閹虫劗鍒掗悽纰樺亾閿濆簼绨兼い鏂垮濮婅櫣鎷犻幓鎺濆妷闂佹椿鍙庨崳锝呯暦閿熺姴绀冮柤纰卞墯濞堟儳鈹戦悙鍙夆枙濞存粍绮撻幃鐐哄垂椤愮姳绨婚梺鐟版惈缁夌懓顬婂畡鎳婄懓顭ㄩ崟顓犵厜闂佸搫鐬奸崰鏍箖濠婂吘鐔兼惞闁稒妯婇梻鍌欑窔閳ь剛鍋涢懟顖涙櫠椤斿墽纾界€广儱鎷戝銉╂煟閿濆洤鍘撮柟顔炬櫕缁瑧鎹勯妸銉バ曞┑锛勫亼閸婃牜鏁幒鏂哄亾濮樼厧寮柛鈺傜洴楠炲鏁傞悾灞藉箞婵犵數濞€濞佳呪偓姘煎墴瀹曟繂顫濋鐘殿啎闂佸憡渚楅崢鍓х不婵犳碍鐓涢悘鐐插⒔濞叉潙鈹戦埄鍐╁€愰柡浣稿暣閸┾偓妞ゆ帒瀚悡婵嬫煛閸モ晛袥闁稿鎸搁埢鎾诲垂椤旂晫褰梻浣侯焾椤戝懘鏁冮鍫澪ュù锝呭濞尖晠鎮规ウ鎸庮仩闁告棑绠戦—鍐Χ閸℃鐟ㄩ柣搴㈠嚬閸撶喎顕ｆ导鏉懳ㄩ柍鍝勫€婚崢鎾绘偡濠婂嫮鐭掔€规洘绮岄～婵堟崉娴ｆ洩绠撻弻娑㈠即閵娿儳浠╃紓浣哄У婵炲﹪寮婚悢琛″亾濞戞瑯鐒藉┑鈥虫喘閺屾稓鈧綆鍋呭畷宀勬煙缁涘湱绡€濠碘€崇埣瀹曨亝鎷呯拠鈩冿紖濠电姷鏁告繛鈧繛浣冲洤纾归柟闂寸缁犵喖鏌ㄩ悢鍝勑ｉ柛瀣€块弻锝夊棘閸喗鍊梺缁樻尰閻熴儵婀侀梺缁樻尭濞寸兘骞楅悩宕囩闁瑰啿鍢查崰姘舵偄閸℃瑦鍠愭繝濠傜墕缁愭淇婇妶鍛櫣濡楀懘姊洪崨濠冨闁搞劋鍗冲畷锟犲箮閼恒儳鍘介梺鍝勫暙濞诧箓顢旈悩缁樼厽闁规崘灏欓崝宥囩磼缂佹娲撮柡浣瑰姍瀹曘劑顢欓懞銉モ偓鐑芥⒒娴ｇ懓顕滄繛璇ч檮缁傚秹顢旈崼銏犵ウ闂佸憡鍔戦崝澶愬几鎼淬劍鐓欓柟顖嗗拑绱炲銈嗘煥閻倸顫忓ú顏呭仭闁绘鐗婇崕鎾斥攽閳藉棗浜濇い銊ユ缁顓奸崱鎰簼闂佸憡鍔忛弲婵嬪储闁秵鍋℃繝濠傚暣閸欏嫰鏌曢崱妤€鏆ｇ€规洏鍔戦、娑橆煥閳ь剛绮径鎰拺闁告繂瀚埢澶愭煟韫囨柨濮岄柕鍥ㄥ姍楠炴帡骞橀崗鍛潖闂佽娴烽幊鎾寸珶婵犲洤绐楅柡宥庡幖缁€鍫澝归悡搴ｆ憼闁绘挻鐟﹂妵鍕冀閵娧呯厐闂佸搫顑嗙粙鎺楀箞閵婏妇绡€闁告劏鏂傛禒銏犖旈悩闈涗沪闁告梹鐗犻獮鍡涘籍閸繍娼婂銈庡亽閸犳氨绮氶崸妤佲拻闁稿本鐟ㄩ崗灞俱亜椤撶偟澧︽い銏＄墵瀹曞崬鈽夊Ο纰卞敼闂備焦鎮堕崕娲礈濞嗘挻瀚呴柣鏂垮悑閻撱儲绻濋棃娑欙紞婵℃彃鎽滅槐鎺楁偐瀹割喚鍚嬮梺璇″枟椤ㄥ懘鍩ユ径濞炬瀻婵☆垳鍘ф慨鐑樹繆閻愵亜鈧呪偓闈涚焸瀹曪綁宕橀…鎴炵稁濠电偛妯婃禍婵嬫倿婵犳碍鐓熼柟閭﹀灠閽勫ジ鎮楀顓熺凡妞ゎ叀鍎婚¨渚€鏌涢敐蹇曠М鐎殿喖顭烽弫鎰板醇閵忋垺婢戦梻浣告惈閸燁偊宕戦崱娑樻辈闁挎稑瀚壕钘壝归敐鍫綈闁抽攱甯￠弻娑㈡偐閾忣偄纾抽梺绯曟杹閸嬫挸顪冮妶鍡楃瑨闁挎洩濡囩划鏃堟偨閸涘﹦鍘遍梺缁樏壕顓熸櫠閸楃儐鐔嗛悷娆忓缁€鍐磼缂佹绠撴い顐ｇ箞椤㈡﹢鎮╅幓鎺旑吋闂傚倸鍊峰ù鍥敋閺嶎厼鍨傞幖娣妼瀹告繃銇勯幒鎴濐仼濞寸姵鍔欏濠氬磼濞嗘帒鍘″銈庡幖閻楁捇銆侀弽顓炲耿婵炴垶顭囬澶愭⒑閹肩偛鍔€闁告侗鍠楅鏇㈡煟閻斿摜鐭屽褎顨堥弫顕€鍩￠崨顓熺€梺鍛婂姦閸犳鎮¤箛娑氬彄闁搞儜宥嗘暰濠电偛鎳庡Λ娆撳Φ閸曨垰唯闁挎棁妫勭壕鍐测攽椤旂》鍔熺紒顕呭灦楠炲繘宕ㄩ鍓ф嚌濡炪倖鐗楃划灞炬叏閸忛棿绻嗛柣鎰典簻閳ь剚鐗犲畷褰掓濞磋櫕绋戦埞鎴犫偓锝庡墮缁侊箓姊洪崷顓烆暭婵犮垺顭囩划濠氭偡閹冲﹤缍婇弫鎰板川椤旇法顢呴梻浣风串缁茬偓绻涙繝鍌ゆ綎濠电姵鑹剧壕鍏肩箾閸℃ê绗掗柛姗堢磿缁辨挻绗熼崶褎鐝梺鎼炲妼閻栫厧顕ｉ锕€绀冩い鏃囧亹閸樻悂姊洪崨濠佺繁闁告﹢绠栭幃浼村Ψ閳哄倵鎷洪梺鍛婄箓鐎氼噣鍩㈡径鎰厱婵☆垰鐏濇禍鍦磼椤旇偐澧涚€垫澘瀚埀顒婄秵閸撴盯顢欓弴銏♀拺缂侇垱娲栨晶鏌ユ嫅闁秵鍊堕煫鍥ч瀹撳棝鏌?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鈧鍝勑ч崶褉鍋撻妶澶婄獥婵°倕鎳庢闂佸憡娲﹂崹浼村礃閳ь剙顪冮妶鍡楀Е婵犫懇鍋撶紓渚囧枛閻栫厧顫忓ú顏勭闁绘劖褰冮‖鍫ユ⒑缁嬪尅宸ユ繛灏栤偓鎰佸殨閻犲洦绁村Σ鍫ユ煏韫囧﹥娅嗛柡鍛箞濮婃椽骞愭惔锝囩暤濠碘槅鍋呴惄顖濇闂侀潧顦弲婊堝煕閹达附鐓㈡俊顖欒濡叉悂鏌￠崱娆忔灈闁哄本鐩幃鈺佺暦閸パ€鎷伴柣搴㈩問閸犳盯顢氳閸┿儲寰勯幇顒夋綂闂佺粯蓱閻楁捇宕濋幋锕€钃熼柕濞垮劗閺€浠嬫煕閳╁厾顏勨枍閿濆拋娓婚柕鍫濋婢ь喗銇勯幋婵囧殗闁?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖濠德板€х徊浠嬪疮椤栫儐鏁佺€广儱顦伴埛鎴犵磼鐎ｎ亞浠㈡い鎺嬪灪閵囧嫰濡搁妷锕€娈楁繝纰樺墲閹告娊鐛幒妤€绠ｆい鎾跺枎閸忓﹪姊绘担鐟邦嚋缂佽鍊块獮濠傤吋閸℃瑧褰鹃梺鍝勬储閸ㄦ椽鍩涢幒鎳ㄥ綊鏁愰崼鐕佷哗闂侀潧妫楅敃顏堝蓟閿熺姴纾兼繛鍡樺灩閻涖垽鎮楃憴鍕闁搞劌娼￠悰顔碱潨閳ь剟鐛崶銊﹀闁荤喐澹嗗Σ锝夋⒒閸屾瑧绐旀繛浣冲洦鍋嬮柛鈩冦亗濞戞鏃堝川椤撱垺鏆呭┑鐘垫暩婵潙煤閿曞倹鍋傞柡鍥ュ灪閸婄敻鏌ㄥ┑鍡涱€楀ù婊勭箘閳ь剝顫夊ú鏍儗閸岀偛钃熼柨娑樺濞岊亪鎮归崶銊с偞闁稿鎹囧鎾偐閹绘帞鏌ч梻鍌欐祰瀹曞灚鎱ㄩ弶鎳ㄦ椽鍩￠崘褏鍓ㄥ┑鐐叉閸旀銆呴悜鑺ョ厵闂傚倸顕崝宥夋煃闁垮鐏撮柟顔肩秺瀹曞爼鏁愰崒姘闂佹儳娴氶崑鍡樼椤忓懐绡€闁汇垽娼ф禒婊堟煟閵娧冨幋闁糕斁鍋撳銈嗗坊閸嬫挾鐥紒銏犲籍鐎殿喗濞婇弫鍐磼濞戞艾骞楅梻浣告惈閸燁偊宕愰悽绋跨闁跨喓濮甸悡娑樏归敐鍛喐閺嶏紕绱撴担铏瑰笡闁烩晩鍨堕悰顔锯偓锝庡枟閸婄兘鏌ょ喊鍗炲闁硅櫕鐗犲铏规嫚閺屻儺鈧鏌ｈ箛鏂垮摵闁诡啫鍥ㄥ亹鐎瑰壊鍠楃紞搴ｇ磽閸屾瑧鍔嶉拑鍗炩攽椤栨稒灏﹂柡灞剧☉閳藉顫滈崼婵呯矗闂備礁鎼Λ妤呮偋婵犲嫭宕叉繛鎴欏灩缁犲鎮楅棃娑橆棌闁哄棌鈧剚娓婚柕鍫濆暙婵″ジ鏌ㄩ弴妯衡偓婵嬪箖閹呮殝闂侇叏濡囬崣鍡涙⒑閸涘﹤濮х紒鏌ョ畺閿濈偛顓兼径瀣ф嫼闂傚倸鐗婄粙鎾剁不濮樿埖鐓冪憸婊堝礈濞嗘挸鍌ㄩ柡宥庡幗閸嬨劌鈹戦悩宕囶暡闁抽攱甯掗湁闁挎繂娲ㄩ幗鍌涚箾閸忕厧濮囨い顓炴健閹兘鎮ч崼顐ｏ紒闂備礁鎼幊鎾斥枖濞戞粎浜欓梻浣告啞娓氭宕伴弽銊ь洸婵°倐鍋撻柍瑙勫灴閹瑩寮堕幋鐘辨埛闂備焦鎮堕崝宥咁渻娴犲绠栭柨鐔哄Т閸楁娊鏌ｉ弮鍌滅瘈缂併劌顭峰娲焻濞戞ê绁┑鐐板尃閸曨収娲稿┑鐘诧工閸橀箖鎮烽柇锔惧弳闂佸憡娲﹂崢楣冩偂閳ь剚淇婇悙顏勨偓銈夊矗閳ь剟鏌涙惔鈽嗙吋缁℃挸銆掑锝呬壕闂佸搫鏈惄顖炲箖閳哄懎绠涘ù锝呮贡閺嗩參姊绘担鐑樺殌缂佺姴绉瑰畷纭呫亹閹烘垹鍘撮梺鐟邦嚟婵參宕戦幘缁樻櫜閹煎瓨绻勯弫鏍⒑?
// DoH/nginx 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厱闁归偊鍨扮槐锕傛煟閵忋垻甯涘ǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘介崑瀣節婵犲倻澧曠痪鍓х帛缁绘盯骞嬪▎蹇曚患闂佹悶鍔岄崐鍧楀蓟濞戞鏆嗛柍褜鍓熷畷鎴︽倷閻戞ê浜楅梺缁樻煥閸氬鎮¤箛娑氬彄闁搞儜灞藉壈闂佺粯甯楀浠嬪蓟閿濆绠抽柟瀛樼妇閹稿啰绱撴担浠嬪摵閻㈩垽绻濆畷娲焵椤掍降浜滈柟鐑樺灥椤忊晝鐥幆褍鎮戠紒缁樼洴楠炲鎮欓崹顐㈡珣闁诲孩顔栭崰鏍倶濠靛鐓橀柟杈鹃檮閺咁剟鏌涢弴銊ヤ簵闁告繃顨呴—鍐Χ閸℃鍋侀梺鍛婃处閸欏骸煤閸涘﹣绻嗛柕鍫濈箳閸掍即鏌涢悤浣哥仸鐎规洘鍔欓幃浠嬪川婵犲倷鐢绘繝鐢靛仜濡鎹㈤幇閭︽晜妞ゅ繐妫涚壕濂告煃瑜滈崜鐔煎春閳ь剚銇勯幒鎴濐仾闁抽攱鍨圭槐鎾存媴鐠囷紕鍔锋繝鈷€灞奸偗闁哄本娲熷畷濂告偄缁嬪簱鍋撻幇鐗堢厽婵炴垵宕▍宥団偓瑙勬礃閿曘垽銆佸▎鎾村殐闁宠桨绀佺粻浼存⒒閸屾瑨鍏屾い顓炵墦瀵敻顢楅崒婊呯厯闂佽鍎虫晶搴ㄦ儗閹剧粯鍋ｉ弶鐐村椤掔喐绻涢幘鎰佺吋闁哄本娲熷畷鐓庘攽閸ヨ埖顥ｅ┑鐘愁問閸犳牠宕愰崸妤€钃熸繛鎴炵懅缁犻箖鏌涢…鎴濇灀闁稿鎹囧鎾倷閼碱兙鍋掗梻浣藉吹閸犳劗鍒掓惔銏℃珷婵°倕鍟弳婊勪繆閵堝懏鍣归柣鎾存礋閺岀喖鎮滃Ο鐑橆唴閻熸粎澧楃敮妤呭疾閺屻儲鐓曟繛鎴濆船閺嬫盯鏌￠崱妤冨ⅵ婵﹥妞藉畷顐﹀Ψ閵夛妇浜栭梻浣侯焾椤戝洭宕戦妶鍡曠箚闁汇垻顭堢粈瀣亜閺嶎煈鍤ら柍鍝勬噺閻撳繐顭块懜鐢点€掗柣锝堥哺缁绘盯宕崘顏喰滈梺鍝勭焿缂嶄線鐛鈧畷锟犳倷閳轰椒澹曞┑鐘绘涧椤戝懘鎷戦悢鍏肩厸闁搞儮鏅涢弸鏃傜磼閳锯偓閸嬫捇姊绘繝搴′簻婵炶绠撻獮鎰板箚椤剨缍侀弫鎰板川椤忓懏鏉搁梻浣虹帛閸旀浜稿▎鎰珷闁哄啫鍊荤壕鍏笺亜閺冨倹娅曢柟鍐叉川閳ь剚顔栭崰娑㈩敋瑜旈崺銉﹀緞閹邦剦娼婇梺缁樕戦悧鎾诲礉瀹ュ桅闁告洦鍨扮猾宥夋煕鐏炴崘澹樺ù鐘趁—鍐Χ鎼粹€茬爱闂佸搫鎳愭慨鎾敋閿濆閱囬柡鍥╁仧閸樻悂姊虹粙鎸庢拱缂侇喖閰ｉ幃楣冩惞閸︻厾锛濇繛杈剧秬濞咃絿鏁☉姘辩＜閻庯綆鍋勭粭鎺撱亜椤撶偞绌跨紒鐘崇☉閳藉螣绾拌鲸肖闂佽崵鍠愮划宀€绮旈悷鎵殾闁告繂瀚弳鍡涙煕閺囥劌鍘撮柟鐤缁辨帞绱掗姀鐘茬闂佺懓鍟垮锕傘€傜捄銊х＝闁稿本鐟х拹浼存煕閻樻剚娈滄鐐村灴瀹曟儼顦撮柡鍡檮缁绘稑顔忛鑽ゅ嚬闂佺锕弨杈╂崲濞戙垹绾ч柟鎼幖閸撳崬顪冮妶搴′簻缂佸鎸抽崺鐐哄箣閿旇棄鈧兘鏌涘▎蹇ｆ▓婵☆偓绻濆娲箹閻愭彃顬堢紓鍌氱Т閿曨亪鐛崱妯肩懝闁逞屽墮閻ｉ攱绺界粙璇俱劑鏌ㄩ弴妤€浜炬繛瀵稿閸樺ジ鍩為幋锔藉亹閻犲泧鍐х矗闂備礁鎽滈崰鎾存櫠濡ゅ懎鐤鹃柕澶嗘櫆閻撶喖骞栧ǎ顒€鈧倕顭囬幇顓犵閻犲泧鍛殼閻庤娲栫紞濠囧箠濠婂牆绀傞柛婵勫劤閻ｉ箖姊绘担鍝ョШ婵☆偉娉曠划鍫熺瑹閳ь剟骞冮垾鏂ユ婵☆垰绻愮紞濠囧极閹版澘宸濇い鏃囨閸撻潧鈹戦悩鎰佸晱闁革綆鍨跺浠嬪礋椤掑倵鏀虫繝鐢靛Т濞诧箓宕愰柨瀣ㄤ簻闊洦鎸搁銈夋煕鐎ｎ偅宕岀€殿喕绮欓、姗€鎮欓懠顑藉亾椤撱垺鈷戠紓浣股戠亸顓犵磼閵娿儳鍩ｇ€殿喗鎮傚浠嬵敇閻斿搫骞愰梻浣规偠閸庮垶宕曢柆宥嗗€堕柍鍝勫暟绾惧ジ鏌嶈閸撴艾顕ラ崟顓濇勃缂佸銇樻竟鏇炍旈悩闈涗粶闁诲繑绻堝畷婵嗩潩椤撴粈绨婚梺鎸庢煥閻忔繂鐣甸崱娑欑厵妞ゆ洖妫涚弧鈧悗娈垮枟閹歌櫕鎱ㄩ埀顒勬煥濞戞ê顏╅悽顖涘劤閳规垿鎮╅崹顐ｆ瘎婵犳鍠栭顓㈠箲閵忋倕绠虫俊銈傚亾缂佲偓閸℃稒鐓涘璺猴功濮樸劑鏌￠埀顒佺鐎ｎ偆鍙嗗┑鐐村灦閿氭い蹇ｄ簼缁绘盯宕ㄩ鐣岊槶缂備浇椴哥敮妤€顭囪箛娑樜╅柕澶涚畳閻т線鏌ｆ惔銈庢綈婵炲弶顭囬弫顕€骞掑Δ鈧悡鈥愁熆鐠哄ソ锟犳偄閸忕厧娈愰梺鍐叉惈閸婄鈻撻幇鐗堚拻闁稿本鑹鹃埀顒€鍢查湁闁搞儺鍓﹂弫瀣煛瀹ュ骸骞栭柛銊ュ€块弻娑樜旈崘銊㈠亾閿濆應妲堥柕蹇曞Х椤︺劑姊洪崘鍙夋儓闁稿﹦顭堥锝夊锤濡や讲鎷洪梻渚囧亞閸嬫盯鎳熼娑欐珷闁圭虎鍠楅悡娑㈡倶閻愬灚娅曢弫鍫熺節绾板纾块柛蹇斏戞穱濠囨偪椤栵絾顎夌紓鍌欐祰娴滎剟宕戦悙鍨床婵犻潧顑呴～鍛存煥濠靛棙顥犻柕鍡樺姍閺岋絾鎯旈姀銏╂殹濠电姰鍨洪敃銏ゆ晲閻愬墎鐤€闁哄洨濮烽敍婊冣攽閳藉棗鐏ユい鏇嗗浂鏁侀柟鍓х帛閳锋垿鏌涘☉姗堝姛闁宠棄顦甸弻銊╁即濡　鍋撳┑鍡╁殨闁规儼妫勯柋鍥煏婢舵稖鍚傞柟閿嬫そ濮婃椽宕ㄦ繝鍕ㄦ闂佹寧娲忛崐婵嬪箖濮椻偓婵＄柉顦寸痪鍓ф櫕閳ь剙绠嶉崕閬嶅箯閹存繍鍟呴柕澶嗘櫆閻撶喖鏌ㄥ┑鍡樺闁绘帒娼￠弻鏇㈠炊閵娿儱顫掑Δ鐘靛仦鐢繝鐛鈧畷妤呭川椤栨粎协闂傚倸鍊搁崐椋庣矆娴ｅ湱鐝跺┑鐘叉搐绾惧鏌涢埄鍐槈缂佹劖顨婇獮鏍垝鐟欏嫷娼戦梺鎼炲€栧ú鐔煎蓟濞戞粠妲煎銈冨妼濡繈宕哄☉銏犵闁挎梻鏅崢閬嶆⒑瑜版帒浜伴柛鎾寸懅閻ヮ亣顦归柡宀€鍠栭、娑橆潩閸楃偐鍙洪梻浣告惈閻ジ宕伴弽褏鏆︽慨妞诲亾闁瑰磭濞€椤㈡宕掗妶鍛珨闂備浇顕у锕傦綖婢跺⊕鍝勵煥閸繃鐎繝鐢靛У閼归箖寮伴妷鈺傜厓鐟滄粓宕滃璺何﹂柛鏇ㄥ灠缁犳娊鏌熺€涙绠ュù鐘荤畺濮婅櫣绮欓崠鈥冲闂佽桨绀侀…宄邦嚕鐠囨祴妲堟繝濠傛噽閺夋悂姊洪悷鎵虎闁哥噥鍨抽悷褎绻濋悽闈涗粶闁宦板妿閸掓帡骞橀崜浣风瑝闂佸搫璇炵仦鍓х▉婵犵數鍋涘Ο濠冪濠靛鍊垮┑鐘崇閻撶喖鏌熼柇锕€鐏犻柦鍕偢閹顫濋鐔哄嚒濡炪値鍙€閸庡篓娓氣偓閺屾盯鎮╅幇浣圭杹婵犵绱曢弫璇茬暦閻旂⒈鏁嶆慨姗嗗弾濡喖姊洪懡銈呅ｅù婊€绮欏畷婵囨償椤垶鏅╂繝銏ｆ硾鑹岄柡鈧禒瀣厽闁规壆澧楀☉褔鎮介姘暢缂佽鲸甯炵槐鎺懳熼弴鐔风仸缂佸矁椴哥换婵嬪炊瑜忛鎰箾鏉堝墽绉柛濠佺矙婵℃瓕顦抽柛鐘冲姍閻擃偊宕堕妸褉濮囬梺绋款儐钃遍柕鍥у瀵剛鎹勯妸锔炬闂備浇銆€閸嬫挸霉閻樺樊鍎愰柣鎾跺枛閺岀喖鏌囬敃鈧晶浼存煙閻ｅ苯鈻堥柟顔筋焾缁犳稒鎯旈垾宕囥偖闂備礁鎼悮顐﹀磹濠靛绠栭柣鎴ｆ缁狙囧箹鐎涙ɑ灏伴悗鍨墵濮婄粯鎷呯粙鎸庡€┑锛勫仩婵倗绮嬪澶婇唶闁哄洨鍋犻幗鏇㈡⒑缁洖澧叉い銊ョ箻瀹曘儳鈧綆鍠楅悡鏇熺箾閹存繂鑸归柣蹇氬皺缁辨帡鏌嗗鍕枤濠殿喖锕︾划顖炲箯閸涱垳椹抽悗锝庡亞缁夌兘鏌ｆ惔銈庢綈婵炲弶锕㈤弫鍐閵堝啠鍋撴担鍓叉僵闁归鐒﹂瀷婵犵數濮伴崹濂革綖婢跺⊕娲偄妞嬪孩娈惧┑掳鍊曢幊蹇涘吹閸愵喗鐓冮柛婵嗗閺嗙喐淇婇幓鎺撳枠婵﹤顭峰畷鎺戭潩椤戣棄浜鹃柟闂寸劍閸嬪鈹戦悩鎻掝伀闁活厽鐟╅弻鐔煎箚瑜忛幗鐘崇箾閸涱垰鈻堥柡灞炬礀椤曘儱鈽夐姀鈺傛缂備降鍔岄…宄邦潖濞差亜浼犻柛鏇ㄥ亝濞堟粓姊虹粙娆惧剱闁圭懓娲璇测槈閵忕姷鐫勯梺閫炲苯澧撮柟顔惧厴婵＄兘鍩＄€ｎ剛鈼ら梻濠庡亜濞诧箑煤濠婂懎顥氶柛蹇涙？缁诲棙銇勯弽銊х闁搞倖鐟﹂妵鍕償閵忊€崇３濠殿喖锕ュ浠嬪箠閿熺姴围闁告侗鍠氶埀顒傚亾缁绘繈濮€閵忊€虫畬濠碘槅鍋呯粙鎾诲礆閹烘挾绡€闁搞儯鍔岄埀顒€顭烽弻銈夊箒閹烘垵濮夐梺褰掓敱濡炶棄顫忛搹鍦煓閻犳亽鍔庨鍥⒑閸涘﹨澹樻い鎴濐槹娣囧﹪鎮介崨濠備簻闂佺绻楅崑鎰板储闁秵鈷戦梻鍫熶緱閻掗箖鏌涢弬娆惧剶鐎规洏鍎甸、鏇㈠Χ閸モ晪绱叉俊鐐€栧濠氬煕閸儱纾块柡鍐ｅ亾妞ゃ劊鍎甸幃娆撳级閹寸偠鐧佹俊銈囧Х閸嬫盯顢栨径濠庡殨妞ゆ洍鍋撻柛鈹惧亾濡炪倖甯掔€氬摜绱為弽顐熷亾楠炲灝鍔氶柟鍐差樀瀵劍绂掔€ｎ偆鍘介梺褰掑亰閸撴瑧鐥閵囧嫰濡疯娴犻亶鏌＄仦鍓ф创濠碘剝鐡曢ˇ鏌ユ煛鐎ｎ偆銆掗柍褜鍓氶鏍窗濡ゅ懎绠栭柛灞惧嚬閸ゆ洖霉閻樺樊鍎涢柡浣告喘閺岋綁寮崹顔鹃獓闁诲孩鑹鹃妶绋款潖婵犳艾纾兼慨姗嗗厴閸嬫捇骞栨担鐟颁罕婵犵數濮寸€氼噣鎯屽▎鎾寸厵闁诡垎鍛喖濠电偛鎳愭繛鈧柡宀嬬節瀹曞爼濡烽妷褌鐥梻浣侯焾閿曘倝鎮樺璺虹疄闁靛鍎欓悢鍏煎亗閹肩补鈧尙甯涢梺璇插椤旀牠宕抽鈧畷鏉课旈崨顓囷箓鏌涢弴銊ョ仩妞ゎ偄鎳橀弻宥夊煛娴ｅ憡娈跺銈忕細閸楀啿顫忓ú顏勬嵍妞ゆ挆鍛Ъ闂備焦瀵уú蹇涘磿瀹曞洤鍨濋柍鍝勬噺閸嬪嫰鎮樿箛搴ｅ妽闁哄倵鍋撻梻鍌欑婢瑰﹪宕戦崱娑樼獥閹肩补妾ч弸鏃堟倶閻愮數鎽傛繛鎾愁煼閺屾洟宕煎┑瀣碘偓妤侇殽閻愭潙鐏撮柡灞剧洴瀵噣鍩€椤掑嫬鍨傞柣銏犳啞閸嬧晝鈧懓瀚伴崑濠傤焽閵娾晜鐓冪憸婊堝礈閻斿吋鍋樻い鏇楀亾鐎规洖宕～婵嬪礂婢跺厜鏅涢埞鎴︽偐濞堟寧姣屾繝娈垮枛缁夌懓鐣烽妸鈺婃晬婵犲﹥鍔楅梻?
//   1) 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣濠垫劖缍楅梺閫炲苯澧柛濠傛健楠炴劖绻濋崘顏嗗骄闂佸啿鎼鍥╃矓椤旈敮鍋撶憴鍕８闁告梹鍨甸锝夊醇閺囩偟顓洪梺缁樼懃閹虫劙鐛姀銈嗏拻闁稿本鐟х粣鏃堟煃瑜滈崜娑㈠磻濞戙垺鍤愭い鏍ㄧ⊕濞呯娀鏌熺紒銏犳灍闁绘挻鐩幃姗€鎮欓幓鎺嗘寖闂侀潧妫欑敮锟犲蓟瀹ュ牜妾ㄩ梺鍛婃尪閸斿海妲愰悙鍝勫耿婵炴垶顭囬敍娑㈡⒑閸涘﹣绶遍柛姗€绠栧鎶芥晜闁款垰浜鹃柛蹇擃槸娴滈箖姊洪崨濠冨闁告挻鐩畷?5444(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厾缁炬澘宕晶濠氭煕閻愯尙鍩ｆ慨濠勭帛閹峰懘宕ㄦ繝鍌涙畼缂傚倷绀侀幖顐㈩焽閳ュ磭鏆﹂柡鍥ュ灩缁狀噣鏌ら幁鎺戝姎闁告挸缍婂娲偡閺夊簱鎸冪紓渚囧櫘閸ㄥ爼骞冮敓鐘插嵆闁绘梻绻濈花濠氭⒑鐟欏嫭绶插褍娴风划娆撴嚒閵堝洨锛滈梺閫炲苯澧寸€规洘锕㈤、娆撴偩鐏炶棄绠? / 5454(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厱闁归偊鍓欑痪褔鏌ｉ妶鍛伃婵﹦绮幏鍛驳鐎ｎ亝鐣伴梻浣规偠閸斿矂濡堕幖浣哥畺婵せ鍋撴鐐叉喘椤㈡稑螣鐟欏嫮娼栭梻鍌欑濠€閬嶅磿閵堝绠伴柟闂寸閻掑灚銇勯幒鎴濇殭闁告艾婀辩槐鎺撴綇閵娿儳鐟ㄩ梺瀹犳椤︻垶锝炲鍫濆耿婵°倐鍋撳鐟邦樀濮? / 443(nginx) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閻樻爠鍥ㄧ厱閻忕偛澧介悡顖氼熆鐟欏嫭绀€闁宠鍨块、娆撴儗椤愵偂绨婚柣锝囧厴椤㈡宕熼銏犱憾闂佽娴烽弫鍝ユ兜閸洖纾婚柟鎹愬煐閸犲棝鏌涢弴銊ュ妞わ富鍠栭—鍐Χ鎼粹€茬盎濡炪倧绠撴禍鍫曞春閳ь剚銇勯幒鎴濐仾婵炴嚪鍥ㄧ厪闁割偁鍩勯悞鐐亜閺囶亞绉€规洖銈稿鎾倷濞堝灝鎮堥梻鍌欒兌缁垶鏁嬮梺璇茬箲閼规崘顣鹃柟鍏肩暘閸ㄨ崵寮ч埀顒勬⒑濮瑰洤鐏叉繛浣冲嫮顩烽柨鏇炲€归悡娆戠磼鐎ｎ偒鍎ラ柣鎾瑰亹閳ь剝顫夊ú妯侯渻閽樺鏆︽慨妞诲亾妞ゃ垺鐟╅幊鏍煛鐎ｎ剙鑰块梻鍌氬€烽懗鍫曞箠閹惧瓨娅犻柣锝呰嫰閸ㄦ繃銇勯弽顐沪闁稿鍊圭换娑㈠箣濞嗗繒浠鹃梺绋匡工閻栧ジ寮婚敐澶婄疀妞ゆ帒鍊风划闈涒攽閻愬弶鍣烘繛鍙夌墵閳ワ箓鎳楅锝嗩€夌紓鍌欒兌婵敻骞戦崶褜鍤曞┑鐘崇閸嬪嫰鏌ц箛鎾磋础闁绘挻鎹囧娲川婵犲嫬鈪抽梺鍛婃尰缁诲牆顕ｉ锝冨亝闁告劏鏅涢埀顒傛暬閺屾盯鈥﹂幋婵呯凹缂備浇鍩栭悡锟犲箖濡も偓椤繈鎮℃惔銏㈠綆闂備胶纭堕弬渚€宕戦幘鎰佹富闁靛牆妫楃粭鎺楁煥閺囶亜顩紒顔芥閹粙宕ㄦ繝鍕箞婵犵妲呴崹杈ㄧ娴犲纾婚柕蹇婃濞堜粙鏌ｉ幇顒€绾фい銉ｅ灮閳ь剝顫夊ú鏍Χ閸涘﹣绻嗛柣鎴ｅГ閺呮粓鎮峰▎蹇擃仼妞?
//   2) 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁绘劦鍓欓崝銈囩磽瀹ュ拑韬€殿喖顭烽弫鍐焵椤掑啰浜藉┑鐐存尰閸戝綊宕归柆宥呯厺闁圭偓鏋奸弨浠嬫煟閹邦垰鐨洪柟鐣屽█閺岋絽螖閸愩劉鏋呴悗娈垮枛椤嘲顕ｉ幘顔藉亜闁惧繗顕栭崯搴ㄦ⒒娴ｈ櫣甯涢柛鏃撶畵瀹曟粌顫濋懜闈涗户闂佸搫鍟崐鑽ゅ閽樺褰掓晲閸涱喗鍎撻柡宥忕節濮婅櫣绱掑Ο缁樺創缂備胶绮换鍌烆敋閿濆棛绡€婵﹩鍓欏畵鍡涙⒑閹稿海鈽夐悗姘煎櫍瀵爼骞栨担鍦幗闁瑰吋鎯岄崹宕囩矓闂堟耽鐟邦煥閸曨厼鈷岄悗瑙勬礃閸ㄥ潡鐛Ο鍏煎珰闁圭粯甯掔粊顕€姊绘担铏瑰笡闁挎岸鏌涘锝呬壕缂傚倷鐒﹂〃鍛此囬鐐茬厴闁硅揪闄勯崐鐑芥煛婢跺鐏╁ù鐘欏洦鈷戦柟鑲╁仜婵″吋銇勯鐐叉Щ闁伙絿鍏橀弫鎰緞婵犲嫷鍟嬬紓鍌氬€烽悞锕傗€﹂崶顒€鍌ㄩ梺顒€绉甸埛鎴︽⒒閸喍绶辨俊鎻掔秺閺屾盯濡歌閺嗏晠鏌嶈閸撴岸宕欒ぐ鎺戝偍濠靛倸鎲￠崑鈺呮煟閹达絾顥夌紒鈧崘顏佸亾閻熸澘顥忛柛鐘愁殘閳ь剟娼ч惌鍌氼潖濞差亝顥堟繛鎴炶壘椤ｆ椽姊虹涵鍛彧闁告梹顨堥崚鎺撶節濮橆剛顓煎銈嗘⒒閹虫挻绂嶆ィ鍐╁仭婵炲棗绻愰顏勵熆鐠哄搫顏柡灞剧洴楠炴鎹勯悜妯间邯闁诲氦顫夊ú妯侯渻閼恒儰绻嗛柣鎴ｅГ閺呮繈鏌嶈閸撴稑鈻庨姀銈嗗€烽柣鎴烆焽閸樼敻姊绘笟鍥у伎缂佺姵鍨堕弲鍫曟偋閸稐绨婚梺鍝勫暊閸嬫捇鏌熼悷鐗堟悙妞ゎ偄绻愮叅妞ゅ繐瀚粣娑欑節閻㈤潧孝闁稿﹥鎮傝棢闁割偁鍎查埛鎴︽煕濞戞﹫姊楃紒鍫曚憾閺屾盯濡搁埡鈧幉鍓р偓娈垮枟閻撯剝淇婇幖浣肝ㄩ柕蹇婂墲閺夋悂姊绘担铏瑰笡閽冮亶鏌涢幘鏉戝摵鐎规洦鍨电粻娑樷槈濞嗘垵骞堥梻浣虹帛钃遍柛鎾磋壘閳绘挸鈻庨幋鐐叉瀾闂佽顔栭崰鏍煝閸喐鍙忓┑鐘插暞閵囨繄鈧娲忛崝宥囨崲濠靛绀冩い蹇撳濮规煡姊婚崒姘偓椋庣矆娴ｈ鍎熸繝濠傛噽缁€濠傘€掑锝呬壕闂佽鍠曢崡鎶姐€佸璺虹劦妞ゆ帒瀚畵渚€鎮楅敐搴℃灍闁哄懏绻堥弻宥堫檨闁告挻绋撻崚鎺楀醇閳垛晛浜鹃柨婵嗛娴滄牕霉濠婂嫮鐭掗柡灞糕偓鎰佸悑闁告劑鍔岄‖澶愭⒑濮瑰洤濡介柛銊ョ埣婵＄敻宕熼姘鳖啋闂佹儳娴氶崑鍛村磿閹剧粯鈷戦悗鍦濞兼劙鏌涢妸銉﹀仴妤犵偛鍟撮崺锟犲礃閳轰胶褰撮柣鐔哥矊缁夌數鈧潧銈搁獮鍥偋閸碍瀚奸梻浣藉吹閸犳劕顭垮Ο浣曪絿绱掑Ο闀愮盎闂佺懓鐡ㄧ换鍐夐悩缁樼厓鐟滄粓宕滈悢濂夌劷鐟滄梹淇婇崜浣虹煓閻犳亽鍔嶅▓楣冩⒑鐠団€崇仸闁稿锕ゅ嵄闁绘垼濮ら埛鎴︽偠濞戞巻鍋撻崗鍛棜?sniroute.cmd up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴椤㈡洟鏁愰崱娆欑喘闂備線鈧偛鑻崢鎼佹煟閹虹偛顩柟骞垮灩閳规垹鈧綆浜為敍婊冣攽鎺抽崐鎾绘嚄閼哥數顩叉繛鍡樺姉缁♀偓闂佹眹鍨藉褎绂掗敃鍌涚厱妞ゎ厽鍨甸悘锔锯偓瑙勬礈閹虫挾鍙呭銈呯箰鐎氼噣寮昏閳规垿鎮╃紒妯婚敪濡炪倖鍨靛Λ婵嬪箖闄囩粻娑樷槈濞嗘垵骞堟俊鐐€栭崝褏寰婄捄銊т笉闁靛缂氱换鍡涙煙缂佹ê绗х紒澶嬫そ閺岋紕浠﹂崜褋鈧帡鏌嶈閸撱劑骞€閵夆晛绀嬫い鎺戝€哥紞鍐⒒閸屾瑧顦﹂柟纰卞亰閹本寰勭仦鐣岀瓘闂佺锕﹂崰鎾寸娴犲鈷掑ù锝囨嚀閸斻倗绱掔紒妯虹瑨闁宠绉规慨鈧柕蹇婃濞肩喖姊虹紒妯哄婵☆垰锕ら妴鎺撶節濮橆厾鍘梺鍓插亝缁诲倿鍩€椤掑倹鍤€闁宠绉电换婵嬪炊閵娧冨箰闂備線鈧偛鑻晶鎾煛鐏炶姤顥滄い鎾炽偢瀹曘劑宕掗敂鍓ф殼闂佸搫鏈惄顖氼嚕閹绢喖惟闁靛／鍌氭憢缂傚倸鍊烽懗鑸垫叏鐎电顥氭い鎾卞灩閻掑灚銇勯幒鍡椾壕缂備胶濮寸粔鐟扮暦閺囥垹纭€闁诲繗顕х紞濠傜暦閹烘嚩鎺旀兜妞嬪海袦濡炪們鍨洪悷褔宕版繝鍐╃秶闁靛鍎冲Σ锝夋⒒閸屾瑧绐旀繛浣冲洦鍋嬮柛鈩冪☉缁犵娀骞栭幖顓犲帥闁轰礁妫濋弻鏇＄疀鐎ｎ亖鍋撻弽顓熷亗闁绘柨鍚嬮悡鐔兼煛閸屾氨浠㈢紒鐙欏嫮绠鹃柛娑卞弿閺€缁樸亜閵婏絽鍔﹂柟顔界懇楠炴捇骞掗幘鎼晙濠电姷鏁搁崑鐐哄垂瑜版帒鏋侀悹鍥皺閺嗭附銇勮箛鎾村櫤濠殿垱鎸抽弻娑滅疀閹垮啯效闂佽瀛╅幃鍌氼潖婵犳艾纾兼慨姗嗗厴閸嬫挸鐣￠幊濠傜秺瀹曞爼顢楅埀顒冪箽闂備線鈧偛鑻晶鎾煛鐏炲墽銆掗柍褜鍓ㄧ紞鍡涘磻閸涱厾鏆︾€光偓閸曨剛鍘搁柣蹇曞仜婢т粙鍩ユ径鎰嚉闁哄稁鍘介悡鏇㈡煙閹佃櫕娅呭┑锛勫帶椤潡鎮烽柇锔解枅濠?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴椤㈡洟鏁愰崱娆欑穿闂備線鈧偛鑻晶鍓х磼閻樿櫕灏柣锝夋敱缁虹晫绮欏▎鐐秱闂備胶鍋ㄩ崕閬嶅疮鐠恒劏濮抽柕澶嗘櫆閳锋帒霉閿濆浂鐒炬い銉ョ箻閺屾稓鈧絺鏅濈粣鏃傗偓瑙勬礃濞叉ê顭囪箛娑樼厸闁告劦浜為崝璺衡攽閻橆喖鐏遍柛鈺傜墵瀹曟繈寮介鐔蜂簵濠电偛妫欓幐鍝ョ棯瑜旈弻娑㈩敃閿濆洨鐣甸梺鎶芥敱濡啴寮婚敐澶嬫櫜闁割偅绮岄幆鍫ユ⒑濮瑰洤鍔村ù婊庝邯閻涱噣宕卞鍏夹梻浣侯焾椤戝啴宕曢悽绋胯摕闁绘梻鈷堥弫濠勭磼濞戞﹩鍎愰柣銈勭窔濮婅櫣鎷犻垾铏亞缂備緡鍠楅悷銉╂偩閻戣棄惟闁挎柨澧介惁鍫ユ⒑閸涘﹤濮﹀ù婊勭矌缁牊绗熼埀顒€顫忔繝姘＜婵炲棙甯掗崢锛勭磽娓氬洤娅橀柛銊﹀▕瀵偊顢氶埀顒勭嵁閹烘嚦鏃€鎷呯憴鍕絿濠碉紕鍋戦崐鏇犳崲閹邦喒鍋撳鍗炲幋鐎规洜鏁诲鎾閿涘嫬骞堟俊鐐€栭崝褏寰婇崜褏鐭嗛柍褜鍓熷缁樻媴缁嬭儻鍩炲┑鐐额嚋缁犳捇鐛崼銉ノ╅柕澶樺枟瀵ゆ椽姊虹化鏇炲⒉妞ゃ劌鎳愮划璇裁洪鍛嫼缂傚倷鐒﹂…鍥╃不閺嶃劋绻嗘い鎰╁灩閺嗭絾顨ラ悙鎻掓殭閾绘牠鏌涢幇銊︽珔闁哄鍊垮娲川婵犲啫顦╅梺鍛婃尰閻╊垵妫熼梺闈涱焾閸庮噣寮ㄦ禒瀣厽闁归偊鍨辩涵楣冩煟閵婏附銇濋柡灞剧洴閹晛鐣烽崶褉鍚傛俊鐐€栧ú鈺冨緤閸ф绠柣妯款梿閸︻厸鍋撻敐搴′航婵炲吋姊圭换婵嗏枔閸喗鐏嶅銈冨妼閹冲氦鐏嬪┑鐐叉閹稿摜澹曟禒瀣拻闁割偆鍠嶇欢杈ㄧ箾閹炬剚鐓奸柡灞炬礋瀹曠厧鈹戦崶鑸殿棧缂傚倷鐒﹂〃鍡欐暜閿熺姴绠栨俊銈傚亾妞ゎ偅绻堝畷锟犳倷閹绘帞顓洪梻鍌欒兌绾爼寮笟鈧畷鎴﹀箻閹颁焦瀵岄梺闈涚墕濡绮崒婊呯＜闁搞劍绋堥崑鎾诲棘閵夛妇浜栨繝鐢靛Т閿曘倝鎮ф繝鍥ㄥ亗婵炴垯鍨洪崐鍫曟煟閹伴偊鏉洪柛銈嗙懇閺屽秷顧侀柛鎾存皑閹广垽宕掗悜鍡樻櫔闂佹寧绻傞ˇ顖炴煁閸ヮ剚鐓涢柛銉㈡櫅娴犙勩亜閹捐櫕鎯堟い顏勫暣婵″爼宕卞▎蹇ｆ椒缂傚倷娴囬褔宕愰崸妤冨祦闁糕剝绋戦柋鍥煟閺傚灝顣崇紒鐘冲哺濮婃椽妫冨☉姘暫濠电偛鐪伴崐鏇㈠汲閳哄懏鈷掑ù锝勮閻掔偓銇勯幋婵囧枠鐎规洘鍨挎俊鑸靛緞鐏炵晫銈﹂梻浣侯攰閹活亞绮婚幋锔藉€峰┑鐘插暔娴滄粓鏌熼悜妯虹仴闁逞屽墮閹芥粎鍒掓繝鍥ㄦ櫇闁稿本绋堥幏娲⒑绾懎浜归柛瀣洴瀹曟繈骞橀鐣屽幈闂佺懓鐏濈€氼噣鎮鹃悽纰樺亾濞堝灝娅橀柛瀣噽閹广垹鈹戠€ｎ亞鍊為悷婊冪箲椤ㄣ儵宕堕浣叉嫼缂傚倷鐒﹂敋濠殿喖鐗婄换娑㈠礂閼测晜鍣伴柦妯煎枛閹鏁愭惔婵囧枤濠电偞鎸搁…鐑藉箖瀹勬壋鏋庨煫鍥ㄦ惄娴犲ジ姊哄畷鍥╁笡婵炲弶锕㈤獮鍫ュΩ閿斿墽鐦堥梺鍛婃处閸樿偐绮敓鐘斥拺闁荤喐婢樺Σ濠氭煙閾忣偓鑰挎鐐插暣閹粓鎳為妷褍濮洪梻浣瑰濞插秹宕戦幘瀵哥闁割偆鍠撶粻妯肩磼鏉堛劍宕岀€规洘甯掗埢搴ㄥ箣椤撶啘婊勪繆閻愵亜鈧牠宕归棃娴虫稑鈹戦崱娆愭閻熸粎澧楃敮鎺楁煥閵堝棔绻嗛柕鍫濆€告禍鍓х磽娴ｅ搫校鐎光偓閹间礁绠栨慨妞诲亾闁诡喗鐟╅獮鎾诲箳閸℃ê鐦遍梻鍌欐祰瀹曠敻宕版惔锝囩濠电姴娲ら拑鐔衡偓骞垮劚閻楁粌顬婇妸鈺傗拺闁告稑锕ョ亸鐢告煕閻樺磭澧ǎ鍥э功閳ь剚绋掕摫濠殿垰顕槐鎺楊敍濞戞瑧顦ㄩ梺璇查椤嘲顫忛搹瑙勫枂闁告洦鍋勬慨銏㈢磼閸撗嗘闁告瑥鍟悾宄扳攽鐎ｎ偅娅囬梺閫炲苯澧撮柍銉︽瀹曟﹢顢欓崲澹洦鐓曢柍鈺佸枤濞堟﹢鏌ｉ悢鏉戝闁哄矉绲鹃幆鏃堝閻樺弶鐦撻梻浣告啞閻熴儵宕幘顔惧祦闁告劦鍠楅弲鎼佹煟濡搫鏆卞ù婊勭矒濮婅櫣娑甸崨顓濇睏闂佺顑嗛…鍥嚍鏉堚晜瀚氱€瑰壊鍠楃€靛矂姊洪棃娑氬婵☆偅顨嗛幈銊╂晝閸屾稓鍘遍柣搴秵娴滄粓顢旈鍫熺厸闁告侗鍘鹃崺锝嗐亜閵忥紕鎳呭ù婊勬倐瀵粙濡搁敂鑺ョ帆闂傚倸鍊烽懗鍫曞磻閵娾晛纾块悗鐢告櫜閻掑﹥绻涢崱妯诲鞍闁稿孩顨嗙换娑㈠幢濡闉嶉梺缁樻尰閻╊垶骞冨鈧幃娆撳箵閹哄棙瀵栨繝鐢靛仜瀵爼骞愰幎钘夎摕婵炴垯鍨归悡娑㈡倵閿濆骸澧伴柣锕€鐗撻幃妤冩喆閸曨剛顦ㄩ柣銏╁灡鐢繝鍨鹃敃鍌毼╅柍杞拌兌椤︽澘顪冮妶鍡楀闁搞劎绮粋鎺楊敇閵忊檧鎷婚梺绋挎湰閼归箖鍩€椤掆偓閸㈡煡婀侀梺鎼炲労閸擄箓寮€ｎ喗鐓涚€广儱楠搁獮鏍煕閵娿儱鈧悂婀侀梺缁樻尭濞寸兘骞楅悩缁樼厱閹兼番鍩勫▓婊堟煛鐏炲墽銆掑ù鐙呯畵瀹曟粏顦俊顐熸櫇缁辨挻鎷呯粵瀣濠碘槅鍋呯换鍫濐嚕婵犳碍鏅插璺猴功椤撴椽姊洪幐搴ｇ畵婵☆偒鍘艰灋闁告洦鍨遍埛鎴犵磽娴ｈ偂鎴犱焊娴煎瓨鐓熼柍鍝勶工閻忥妇鈧鍠涢褔顢樻總绋块唶妞ゆ劧缍嗛埀顒€娲缁樻媴閸涘﹤鏆堥梺瑙勭摃椤曆囷綖濠靛惟鐟滃秹宕?闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰效闁挎繄鍋涢埞鎴犫偓锝庡亝濞呭洭姊虹粙璺ㄧ効濠碘€虫川缁瑨绠涢弮鍌滅槇闂侀潧楠忕徊浠嬫偂閹扮増鐓曢柡鍐ｅ亾闁绘濮撮悾鐑藉箣閿曗偓鍥存繝銏ｆ硾椤戝洭宕㈤幖浣光拺闁硅偐鍋涢崝妤呮煛閸涱喚鈽夐柣顭戝墴濮婄粯鎷呴搹鐟扮闁汇埄鍨辩敮锟犲极閸愵喖唯鐟滃骸鐣烽崣澶岀闁瑰瓨鐟ラ悘鈺傤殽閻愵亜鐏ǎ鍥э躬椤㈡稑鈹戦崶鏈靛摋濠电偛顕慨鐢稿箰閸愬樊娼栨繛宸簻缁犲綊鏌ｉ幇顓炵祷妞ゎ剙顦辩槐鎾存媴缁涘娈梺缁橆殕閹告悂顢氶敐鍡欑瘈婵﹩鍎甸埡鍛厓闁告繂瀚埀顒€鎲＄粋宥咁煥閸啿鎷洪梺鍛婄☉閿曪妇绮婚幘缁樺€垫慨姗嗗墯閸ゅ洤鈹戦鐟颁壕闂備線娼х换鍫ュ垂婵犳碍鏅繝濠傜墛閳锋垿姊洪銈呬粶闁兼椿鍨遍弲鍓佲偓鐢电《閸嬫挾鎲撮崟顒傤槬閻庤娲﹂崜鐔煎春閵夛箑绶炲┑鐐靛亾閻庡姊洪悷鎵憼缂佽鍊荤划濠氭倷閻戞ǚ鎷婚梺绋挎湰閻熝呯玻閺冣偓缁绘稒鎷呴崘鍙夌〗闁搞儯鍔岄閬嶆煛婢跺鐏ラ柣搴弮濮婂搫效閸パ呬紕濡炪倧绲肩划娆忕暦閹达箑绠婚柟棰佺劍閸嶇敻姊虹紒妯诲碍閻庢稈鏅犻幃闈涒攽鐎ｎ偆鍙冨┑鈽嗗灥瀹曠數绮婚悙鐑樼厸閻忕偟鍋撶粈鍐偓鍨緲鐎氭澘鐣烽崡鐐嶇喖鎳￠妶鍛ɑ闂傚倸鍊烽悞锔界箾婵犲洤缁╅梺顒€绉撮崹鍌炴煕瑜庨〃鍛存嫅閻斿摜绠鹃柟瀵稿€戝璺虹哗濞寸姴顑嗛悡鏇㈡煃閳轰礁鏋ゆ繛鍫涘灩闇夐悘蹇旂墪娴滄儳鈹戦悩鍨毄濠殿噮鍙冮獮蹇涘礃椤旇偐鍊為梺鎸庢濡嫰寮搁妶澶嬧拻濞达絽鎲￠幆鍫ユ煛閸偄澧撮柟顖氬椤㈡盯鎮欓棃娑氥偊婵犵妲呴崹浼存儍閻戣棄纾婚柟鍓х帛椤ュ牊绻涢幋鐐垫噧闁哄棗鐗撳娲閳哄倵鍋撻妷鈺佺倞闁靛鍨虹€?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶鏉款熆鐟欏嫭绀嬮柟顔惧厴楠炲﹥绻濋崒婊呅ㄩ梺璇″枓閺呮盯顢欒箛娑辨晩闁绘挸楠歌灇缂傚倸鍊搁崐鎼佸磹閸濄儳鐭撻柣銏犳啞閸嬪鏌￠崶锝嗗櫚闁逞屽墾缁犳挸鐣风粙璇炬梹鎷呴崨濠勬澓闂傚倸鍊搁崐鎼佹偋婵犲嫮鐭欓柟杈捐缂嶆牠鏌涢妷顔煎闁?闂?
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
        // up 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊璁查弸娆撴⒑缂佹ê绗╁┑顔哄€楅幑銏犫槈閵忕姴鑰垮┑鈽嗗灥椤曆呭枈瀹ュ鐓熼柣鏂挎憸閹虫洜绱掗悩铏磳妤犵偛鍟灃闁告侗鍠楀▍婊堟煙閼测晞藟闁告挻绻堥幃妯侯吋婢跺鎷洪梺纭呭亹閸嬫盯鍩€椤掍焦绀嬮柟宕囧枛椤㈡稑鈹戦崼婊呰兒闂傚倸鍊烽悞锔锯偓绗涘懏宕查柛宀€鍋涚粻鐘诲箹濞ｎ剙濡兼慨鐟板级缁绘盯宕卞Ο璇查瀺闂佺粯鎸诲ú鐔煎箖濮椻偓閹瑦锛愬┑鍡橆唲濠电姷顣槐鏇㈠疾椤愩倖顫曢柟鐑樻⒒绾惧吋淇婇姘倯闁告帗绋戣灃闁绘﹢娼ф禒婊勩亜閹存繃鍤囬柛鈹垮灪閹棃濡堕崶鈺傛緫闂備礁鎼ú锔惧枈瀹ュ纾婚柛娑卞灣绾?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊為悷婊冪箻瀵娊鏁冮崒娑氬幈濡炪値鍘介崹鍨濠靛鐓曟繛鍡楃箳缁犳娊鏌嶈閸撴瑧绮诲澶婄？闂侇剙鍗曢崶顒夋晬闁绘劖娼欓埀顒€鍢查埞鎴︽偐閸欏顦╅梺缁樻尭缁绘劙鍩為幋锔藉亹闁肩⒈鍓涢鎺戔攽閿涘嫯妾搁柛鐘愁殘閹广垹鈽夐姀鐘诲敹濠电娀娼уú銈夊汲閸垻纾藉ù锝堟鐢稓绱掔拠鑼ⅵ妤犵偛鍟粋鎺斺偓锝庡亽濡啫鈹戦悙娈挎闁糕晜鐗犻幆鍕敍閻愯尪鎽曞┑鐐村灦閸╁啴宕戦幘缁樻櫜閹肩补鈧尙鍑归柣搴ゎ潐濞叉牠鎯岄崒鐐茶摕闁挎稑瀚▽顏堟煕閹炬瀚崹鍗炩攽閻樻鏆滅紒杈ㄦ礋瀹曟顫滈埀顒勫Υ娴ｇ硶鏋庨柟鐐綑娴滄鏌熼崗鍏煎剹闁哥姵顨嗙粋宥夊Χ閸モ晝锛濋梺绋挎湰閼归箖鍩€椤掑嫷妫戦柟宄邦儔瀵濡烽妷褝绱遍梻浣告贡閸嬫捇宕滃鑸靛亜闁糕剝绋掗悡鐘绘煙闂傚鍔嶆繛鎳峰啠鏀芥い鏃囧亹婢э箓鏌″畝瀣М妤犵偞锕㈠畷姗€宕ｆ径瀣靛晪闂傚倷绀侀幖顐﹀嫉椤掆偓鐓ゆ俊顖欒閸ゆ鏌涢弴銊ョ仩闂佸崬娲︾换婵嬫濞戞瑯妫炲銈庡亝閹瑰洤顫忕紒妯诲闁告稑锕ラ崕鎾斥攽閳藉棗浜濈紒璇插暟缁顓兼径濠囧敹闂佸搫娲ㄩ崑鐘诲箺閺囥垺鈷戦悹鎭掑妼閺嬫柨鈹戦鑺ョ稇妞ゆ柨绻愰埞鎴﹀炊閵夈倗鐩庨梻浣告惈閸燁偊宕愰崨濠勵洸濡わ絽鍟悡鏇㈡煥濠靛棗顏╃€规挸妫欓幈銊︾節閸愨斂浠㈤悗瑙勬处閸嬪﹤鐣烽悢纰辨晢濞达絿鎳撻鎵磽閸屾艾鈧绮堟笟鈧、鏍幢濞戞顔夐梺鎼炲労閸撴瑩寮伴妷鈺傜厓鐟滄粓宕滃璺何﹂柛鏇ㄥ灠缁犳娊鏌熼幖顓炵仩濠殿喗绋撶槐鎾存媴閸撳弶楔闂佽桨绀侀…宄邦嚕鐠囨祴妲堟慨姗堢到娴滈箖鏌ㄥ┑鍡欏嚬闁硅尙鍋撴穱濠囧箵閹烘捁纭€缂備浇椴哥敮鈥愁嚕椤曗偓瀹曟帒顫濋鐘亾婵犳碍鐓ｅù鍏肩懅閸掍即鏌嶇憴鍕伌鐎规洘甯掗埞鎴﹀炊閼告妫勭紓鍌氬€搁崐鍝ョ矓閹绢喗鏅濇い蹇撶墢瀹撲線鏌涢幇闈涙灈闁绘挻鐩弻娑樷槈閸楃偞鐏堝┑鐐茬墑閸婃洟鈥旈崘顔嘉ч柛鈩冾殘閻熸劙姊虹紒妯洪嚋缂佺姵鎸搁悾閿嬪閺夋垵鍞ㄥ銈嗘尵閸熷潡骞忕紒妯肩閺夊牆澧介幃濂告煟閳╁啯顥堢€规洜鏁诲畷姗€顢欓悾灞藉箞闂備線娼ч…鍫ュ磿闁秵鍤嬬憸鐗堝笚閹虫岸鏌ｉ幇顔煎妺闁绘挻娲熼弻鐔稿鐎涙ɑ閿梺鍝勵儐閻╊垶寮婚敃鍌氱妞ゅ繐瀚悘鍫ユ⒑閸濆嫭婀版繛鍙夘焽閹广垹鈽夊搴⑿ラ梻浣侯焾椤戝棝骞戦崶顒€钃熸繛鎴炲焹閸嬫捇鏁愭惔鈥茬凹閻庤娲栭惌鍌炲蓟閳╁啯濯撮柣鐔告緲椤帡鎮楃憴鍕８闁告梹鍨块妴浣糕槈濡攱鐎婚梺纭呮彧閼靛綊寮ㄥú顏呪拻闁稿本鑹鹃埀顒傚厴閹偤鏁傞悾宀€顔曟繝鐢靛Т濞层倗绱掗埡鍛厱婵°倕鍟╃槐铏箾瀹割喕绨奸柛瀣閳规垿宕掑鍏夹х紓鍌氱Т妤犳悂鍩為幋锔藉€烽柛娆忣槴閺嬫瑦绻涚€涙鐭嬬紒顔肩Ф閳ь剟娼ч妶鎼佸箖閳哄啰纾兼俊顖滃帶楠炲牓姊绘担鍛婃儓婵炲眰鍨藉畷婵堜沪鐟欙絾鐎哄┑鈽嗗灥瀹曠數绮绘ィ鍐╃厽闁规崘娅曢幑锝夋煕閳哄绋婚柟渚垮妽缁绘繈宕ㄩ鍛摋缂傚倷绶￠崰妤呮偡閳哄懐宓侀柟閭﹀幗閸庣喖鏌嶉埡浣告殲闁哄棙锕㈠铏规嫚閸欏鏀銈庡亜椤︻垳鍙呴梺缁樻煥椤ㄥ酣鎮為悾灞惧枑鐎广儱鎳愰弳銈夋煕閺囥劌鐏￠柍閿嬪灩閹叉悂鎮ч崼婵堢懆婵炲瓨绮堥崡鎶藉蓟濞戞粎鐤€闁哄啯鎹侀埀顒冩硶閳ь剚顔栭崰鎾诲礉瀹ュ洨鐭夐柟鐑樻煛閸嬫捇鏁愭惔鈥茬敖闂佸憡锚瀹曨剟鈥旈崘顔嘉ч柛鈩冾殔椤孩绻濆▓鍨灍闁瑰憡濞婇獮鍐潨閳ь剟宕洪敓鐘茬闁挎繂鎳嶆竟鏇㈡煟閻斿摜鎳冮悗姘煎幘婢规洟骞栨担鍦幈闁瑰吋鐣崝瀣箟妤ｅ啯鐓冮柕澶涢檮椤ュ牏鈧娲橀敃銏ゃ€佸▎鎾冲簥濠㈣鍨板ú锕傛偂閺囥垺鐓冮柍杞扮閺嗙喖鏌熼崘鎻掓殲闁靛洤瀚伴、鏇熸償閳╁啫顦╅梺鎶芥敱閸ㄥ潡寮婚妶鍚ゅ湱鈧綆鍋呴悵鏇㈡⒑閹肩偛鈧劙宕戦幘鏂ユ斀闁绘﹩鍋呮刊浼存煕濞戝崬鏋熼柣婵愪簻閳规垿鍨鹃崘鑼獓闂佸憡姊归悷鈺呯嵁閸愩劉鏋庨柟鎯у帠缁卞爼姊洪崨濠冨闁告瑥閰ｅ畷顖溾偓锝庡枟閳锋帒霉閿濆懏鍟為柟顖氱墦閺屾稒绻濋崒婊冪厽閻庤娲樺ú鐔奉嚕婵犳艾唯妞ゆ牗绮庨埣銈夋煟鎼达紕鐣柛搴ㄤ憾钘濆ù鍏兼綑閻撴洟鏌熸潏楣冩闁稿鍓濈换娑㈠幢濡ゅ啰顔囧銈呯箚閺呯娀寮诲☉銏犳閻犲洦绁撮崑鎾澄旈崨顔煎墻濡炪倕绻愬Λ娑氬婵傚憡鐓忛煫鍥ㄦ礀瀛濈紓浣哄У濡炶棄顫忛搹鍦＜婵妫欓悾鍫曟煟鎼淬垻鈻撻柡鍛箞楠炴垿濮€閻橆偅鏂€闂佸壊鍋呯换鍐倵閸愭祴鏀介柍钘夋閻忕娀鏌熺喊鍗炰喊闁糕晜绋掑鍕箛椤撶偘鐢绘繝鐢靛Т閿曘倗鈧凹鍘惧▎銏ゅ蓟閵夛妇鍘遍梺缁樻煥閹诧紕浜搁敃鍌涚厽婵炴垵宕弸娑㈡懚閺嶎厽鐓冪憸婊堝礈閻旂厧鐏抽柨鏇炲€告儫闂佸疇妗ㄧ粈浣虹玻濞戞﹩娓婚柕鍫濇鐏忕敻鏌涚€ｎ剙鏋戠紒鍌氱У鐎佃偐鈧稒菤閹风粯绻涙潏鍓ф偧妞ゎ厼鐗撳鎶芥焼瀹ュ棛鍘电紒鐐緲瀹曨剚绂嶉幍顔瑰亾鐟欏嫭澶勯柛銊ョ埣楠炲啴鍩￠崘鈺佺彴闂佽偐鈷堥崜銊ф闁秵鈷掑ù锝囧劋閸も偓婵犫拃鍛珪闁告帗甯￠、娑㈡倷閼碱剚鐓ｉ梻浣告惈鐞氼偊宕曟潏鈺冧笉鐟滅増甯楅崐鍨箾閹寸偟鎳愰柣鎺旀櫕缁辨帡鎮╃粵纭呪偓鍧楁煛瀹€瀣М闁糕斁鍓濈粭鐔煎焵椤掑嫬鐭楅柛鏇ㄥ墯閸欏繐鈹戦悩鍙夊櫤妞ゅ繒濮风槐鎺撴綇閵娿儳鐟ㄩ柧浼欑秮閹綊宕堕妸褍寮ㄩ梺璇叉唉濞咃絿妲愰幘璇茬＜婵炲棙鍔楅妶鏉款渻閵堝棙鑲犻柛銉ｅ妼娴滈亶姊洪崷顓炰壕闁告挻宀稿畷鐢稿即閵忊€充化闂佹悶鍎崝搴ㄥΧ鐎涙ü绻嗛柟缁樺笧婢ф稓绱掔紒妯肩畺缂佺粯绻堝畷鎺戔槈濡椿浠遍梻鍌欑閹碱偊藝娴兼潙绠栭柛宀€鍋涢拑鐔兼煥濠靛棙顥為柛搴ｅ枑閵囧嫰寮介妸褏鐓傞柡宥佲偓鏂ユ斀闁绘劘灏欓幗鐘电磼椤旇偐肖闁告帗甯￠獮妯肩磼濡粯鐝抽梻浣虹帛閸旀洟骞栭锔藉€峰┑鐘插暔娴滄粓鏌熼崫鍕ユい鏂款樀閺岀喖鎮块娑变哗缂備浇椴搁幐濠氬箯閸涙潙绀堥柟缁樺笒婢瑰牊绻濋悽闈涗户闁靛棗顑囬幑銏犫攽鐎ｃ劉鍋撴笟鈧獮妯侯熆閸曨剚鈷愰柟宄版嚇瀹曟宕妷褎鏅ㄩ梻鍌氬€风粈渚€骞栭鈷氭椽濮€閵堝懐顦悗鍏夊亾闁告洦鍘肩粊锕€鈹戞幊閸婃洟宕幍顔碱棜濠靛倸鎲￠悡鏇㈡倶閻愰潧浜鹃柣銊﹀灴瀵偊宕奸妷锕€鈧敻鎮峰▎蹇擃仾缁剧偓鎮傞弻娑㈠棘鐠恒剱銈夋煙楠炲灝鐏╅柍瑙勫灩閳ь剨缍嗘禍鐐核囬妸銉富闁靛牆鎳愮粻浼存煛鐏炴枻韬┑陇鍩栧鍕偓锛卞嫬顏烘繝鐢靛仩閹活亞寰婃禒瀣婵犲﹤鐗嗛崙鐘绘煕閹般劍娅冪紒璇叉娣囧﹪鎮欓棃娑樹粯婵犲痉銈嗙《缂佽鲸甯￠、娆撴嚃閳轰緡鏉搁梻浣烘嚀閸㈡煡骞婂Ο铏规殾妞ゆ劧绠戠粈瀣煕椤垵浜芥俊顐犲劦濮婂宕掑▎鎺戝帯濡炪値鍋勯澶婄暦閻旂厧鍨傛い鎰╁€栧▓楣冩⒒閸屾瑧鍔嶅┑鐐诧躬瀵劑鏌嗗鍛€梺鍓茬厛閸ｎ喖顭囬弽褉鏀介柣妯虹枃婢规﹢姊洪崡鐐村枠闁哄矉缍佸顕€宕堕妷銏犱壕闁逞屽墮閳规垿顢欓崗鍏兼倷濡炪値浜滈崯瀛樹繆閸洖绀冮柕濞垮劚椤氨绱撻崒娆戣窗闁哥姵顨婇獮鎴﹀炊瑜滈崵鏇㈡偣閸ャ劎銈存俊鎻掔墛缁绘盯宕卞Ο铏逛患闂佷紮绠戦悧濠勬崲濞戞瑦缍囬柛鎾楀憛姘攽閻愬弶瀚呯紓宥勭窔楠炲啴鏁撻悩鑼啋缂傚倷鐒﹁彜闁归攱妞介弻锝夋偐閸欏顦遍梺閫炲苯澧叉繛鍛礋閹﹢宕煎┑鍐╂杸闂佺粯鍔樼亸娆徯掗悙鐑樼厓鐟滄粓宕滃▎鎴濐棜妞ゆ挶鍨瑰Ч鏌ユ煥閺囩偛鈧綊鎮￠弴鐔剁箚闁靛牆鎳庨弳娆徝瑰鍐Ш闁哄矉绱曢埀顒婄秵娴滎亪宕ｉ崟顖涚厸鐎光偓閳ь剟宕伴弽顓炶摕闁搞儺鍓氶弲婵嬫煃瑜滈崜姘跺疾閸洖绠ユい鏃囨閺嬫垿姊虹紒姗嗘當闁绘锕︽禍鎼佹晜閼恒儱寮挎繝鐢靛Т鐎氼喚鏁☉姘辩＜婵°倕鍟弸娑㈡煕閳规儳浜炬俊鐐€栧濠氬磻閹炬番浜滄い鎾跺仦缁屾寧銇勯敂鑺ュ唉闁哄瞼鍠栭悰顕€宕归鍙ョ礄闁诲氦顫夊ú鏍х暦椤掑嫬鐓″璺好￠悢鍝勬瀳閺夊牄鍓遍妶鍛斀闁绘ê鐏氶弳鈺佲攽閻愨晛浜炬繝鐢靛仒閸栫娀宕楅悩铏仢濠碘剝鎮傞崺鐐存償閹惧厖澹曢梺鍛婄☉閻°劑鎮￠妷鈺傚€甸柨婵嗗€瑰▍鍥╃磼閻樺疇澹樻い顏勫暣婵″爼宕卞▎蹇ｆ椒闂備浇宕甸崰鍡涘礉閹达负鈧礁螖閸涱喖浜滈梺绋跨箺閸嬫劙宕?30 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ岸姊绘担钘壭撻拑杈╃磼鐠囪尙澧︾€殿喖顭烽幃銏焊娴ｅ湱浜栭梻浣稿閻撳牓宕板Δ鍛€剁€广儱顦伴埛鎴︽煕濠靛棗顏い銉у仧缁辨帡顢欓懖鈹冣攽閿涘嫭鏆€规洜鍠栭、娆撳礈瑜庡鎴︽⒒娴ｅ憡璐￠柛瀣崌楠炲棛浠︽潪鎸庣洴閸ㄩ箖骞囨担鐟扮槣?
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵婵炲棗绻掗崝鎾⒑鏉炴壆顦︽い鎴濇婵＄敻宕熼姘鳖啋闁荤姾娅ｉ崕銈夋倵妤ｅ啯鈷戦柛娑橈功閹冲啰绱掔紒姗堣€挎鐐插暙铻栭柛娑卞櫘濡啫鈹戦悙鏉戠仸闁荤噦绠撳畷浼村箛閻楀牃鎷哄┑鐐跺蔼椤曆囩嵁濡ゅ懏鐓欓柟闂磋兌閻ｆ椽鏌涢埞鎯т壕婵＄偑鍊栫敮鎺楀窗濮橆剦鐒介柟閭﹀幘缁犻箖鏌涘▎蹇ｆШ濠⒀屼邯閹繝濡舵径瀣帾婵犵數鍊崘鈺佹缂備焦顨呴ˇ闈涱潖濞差亜浼犻柛鏇ㄥ亐閸嬫捁銇愰幒婵囨櫈濠电偛妫欓幐濠氬磻閸曨垱鐓曢柍鈺佸暟閳洟鏌涚€ｅ墎绉柡灞剧☉閳规垿宕卞Δ濠佺棯闂備浇顕栭崹浼存偋婵犲洤桅闁告洦鍨版儫闂佹寧姊婚崑鎾诲闯娴煎瓨鈷戦柛婵嗗閻掕法绱撳鍕獢闁绘侗鍣ｉ獮鎺懳旈埀顒傜不閿濆棎浜滈柡宥冨妿缁犳捇鏌熼姘卞ⅵ闁哄矉绲鹃幆鏃堝閳轰焦娅涢梻浣告憸婵敻鎮ч悩璇茬畺闁靛鏅滈崑鍌炲箹鏉堝墽绋绘繛鍫ョ畺濮婅櫣绱掑Ο璇茬闁诲骸鐏氱敮鎺椻€﹂崶顒€鐓涢柛灞久肩花濠氭⒑鐟欏嫬绀冩繛澶嬬洴瀵悂寮崒娑樻瀾闂佸搫顦扮€笛囧吹閸ヮ剚鐓涚€光偓鐎ｎ剛蓱闂佽鍨卞Λ鍐╀繆閼稿灚鍎熼柕蹇嬪灮鍟告繝鐢靛Х閺佹悂宕戝☉姗嗗殨闁割偅娲橀弲顏堟⒑缂佹ɑ灏柛濠冪箞瀵鏁愭径濠勭潉闂佺鏈懝鐐濡偐纾藉ù锝呮惈鏍￠梺缁橆殕椤ㄥ棛绮氭潏銊х瘈闁搞儴鍩栭弲顒€鈹戦悩缁樻锭婵炲眰鍔戞俊瀛樼節閸ャ劉鎷哄┑顔炬嚀濞层倝鍩€椤掍礁濮嶇€规洘鍨块獮妯肩磼濡攱瀚藉┑鐐舵彧缁插潡鎮洪弬娆剧劷闁规壆澧楅悡鏇㈠箹濞ｎ剙鐏╅柍缁樻礋閺岀喖顢欓崗鐓庝淮閻庤娲栭悥濂搞€佸Δ鍛劦妞ゆ帒瀚崣濠囨煏婢跺棙娅嗛柣鎾跺枛閺岀喖鏌囬敃鈧弸娑㈡煕閺傛鍎旈柡灞界Ч閺屻劎鈧綆浜炴导宀勬⒑閸濆嫭婀扮紒瀣灴閸╃偤骞嬮敃鈧粈瀣亜閹扳晛鐏╂い顐ｆ崌濮婄粯鎷呴崨濠冨創濠碘槅鍋呯换鍐矉瀹ュ閱囬柡鍥╁仩閹芥洖鈹戦悙鏉戠仸闁糕晛鍟村畷鎴﹀箻缂佹ɑ娅滈柟鑲╄ˉ閳ь剝灏欓崙褰掓煟鎼淬値娼愭繛鍙夌墵閹矂宕掗悙鑼舵憰闂佺偨鍎辩壕顓㈠汲閸℃稒鍊甸柨婵嗛婢ф壆鎮敃鍌涒拻濞达絿鐡旈崵鍐煕閻樺啿娴€殿喗鐓￠幃鈺呭矗閸屾稓娲寸€规洘锕㈡俊鎼佸閳藉棙缍屽┑鐘殿暯濡插懘宕归婊勫闁挎洖鍊归崐鐢告⒑椤掆偓缁夌敻鍩涢幒妤佺厱閻忕偛澧介幊鍛存煕閺傝法效闁哄瞼鍠栭、娆撴嚃閳轰胶鍘介柣搴ゎ潐濞叉鎹㈤崒鐐村仼婵犻潧顑呴悞鍨亜閹哄秷鍏岄柣婵嗙埣閺岋繝宕堕妷銉у姶闂佽桨绀佸ú顓㈠蓟閿濆憘鐔烘嫚閼碱剛銈烽梻浣筋嚙缁绘劙鏁冮鍕垫綎婵炲樊浜滅粈鍐煕濞嗗浚妲归柛搴㈡崌濮婃椽鎮烽弶鎸庢瘣濠电偛顕崗姗€骞嗛埀顒併亜韫囨挻绁╂俊顖氬濮婃椽宕崟顓犱紘闂佸摜濮甸幑鍥春閳ь剚銇勯幒宥堝厡闁愁垱娲熼幏鎴︽焼瀹ュ棛鍘撻梻浣哥仢椤戝懏鎱ㄥ澶嬬厸?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖濠德板€х徊浠嬪疮椤栫儐鏁佺€广儱顦伴埛鎴犵磼鐎ｎ偒鍎ラ柛搴＄箲閵囧嫰骞嬪┑鍥ф畻闂佺硶鏅濋崑銈夌嵁鐎ｎ喗鏅滈柦妯侯槷濮规姊绘担鍛婅础缂侇噮鍨抽弫顔界節閸パ呯崶婵犵數濮村ú锕傚煕閹达附鐓曢煫鍥ㄦ尭閹垹绱掔捄鍝勭骇缂佺粯绋撳☉鍨槹鎼达絿妲囬柣搴㈩問閸犳盯顢氳閸┿儲寰勯幇顒夋綂闂佺粯蓱閻楁捇宕濆澶娢﹂柛鏇ㄥ灠缁秹鏌涚仦鎹愬濞寸姵锚椤啴濡舵惔鈥茬爱闂佸搫鎳忕换鍕ｉ幇鏉跨閻庢稒锚椤庢捇姊洪崷顓犲笡閻㈩垰锕ョ粩鐔煎即閻愨晜鏂€闂佺粯锕╅崰鏍倶闁秵鐓曢柍鍝勫€绘晶鐢碘偓瑙勬礃缁诲牆顕ｆ禒瀣垫晣闁绘棁娓圭花鐢告⒒娴ｅ憡鍟為柛鏃撶畵瀹曟澘螖閸涱喖浜楅棅顐㈡处閹尖晠鎮㈤崱娑欏仯濡わ附瀵ч鐘差熆瑜庡ú鐔煎蓟濞戙垹绫嶉柍褜鍓欓悾婵嬪箹娴ｆ瓕鎽曢梺璺ㄥ枔婵挳鐛姀锛勭闁瑰鍎愬▓鏇㈡煕閻樺啿鍝烘慨濠呮閹风娀鍨鹃搹顐や粚婵＄偑鍊ら崑鍕囬悽鍝ュ祦闊洦绋戝婵嬫煛婢跺鐏︾€规挸绉撮—鍐Χ閸℃ê鏆楅梺绋款儐缁嬫挾妲愰幒妤€鐓涢柛娑卞枓閹风粯绻涙潏鍓у埌闁硅绻濋獮鍡涘醇閵夛箑浠╁┑顔筋焽閸樠勭閹殿喒鍋撶憴鍕闁告梹娲濋悘鍐⒑閸涘﹣绶遍柛姗€绠栧畷婵嬫晝閳ь剟鈥旈崘顔嘉ч柛鈩冾殔濞兼垿姊虹粙娆惧剱闁规瓕宕甸幑銏犫攽鐎ｎ亞鍔﹀銈嗗笂閻掞妇绱為崶顒佺厪濠电姴绻掗悾杈┾偓娈垮枛濞尖€愁潖閸濆嫅褔宕惰娴犳儳鈹戦悙闈涘付闁哄牜鍓欓銉╁礋椤撴稑浜鹃柨婵嗙凹缁ㄧ晫绱掗幇顔间槐婵﹪缂氶妵鎰板箳閹存粌鏋堥梻浣告啞閿曘垻绮婚弽褏鏆﹂柕蹇嬪€栭悞鑲┾偓骞垮劚濡矂骞忓ú顏呪拻闁稿本姘ㄦ晶娑樷槈閹捐埖鍤€闁宠棄顦垫慨鈧柣妯活問濡蹭即姊绘担铏瑰笡闁告棑绠撳畷婊冾潩閼搁潧鍓ㄩ梺鍝勫暙閸婅崵澹曟禒瀣厱閻忕偞宕樻竟姗€鏌嶈閸撴瑩宕查弻銉︾畳闂備焦鎮堕崕婊呬沪閸撗屾船闂傚倷绶氬褑鍣归梺鍛婂姂閸斿孩绂嶉悧鍫㈢瘈闁汇垽娼ф禒婊勪繆椤愶綆娈橀柟骞垮灲楠炴帒螖閳ь剙效閺屻儲鐓ラ柡鍥╁仜閳ь剙缍婇幃鈥斥枎閹惧鍘靛┑鐐茬墕閻忔繈寮稿☉娆嶄簻闁挎繂鎳岄崑銏ゆ煛鐏炲墽銆掗柍褜鍓ㄧ紞鍡樼濠靛瑤澶婎煥閸噥姊挎繝銏ｅ煐閸旀牠鎮￠妷锔剧瘈闂傚牊绋掗ˉ鐐电磼娴ｅ弶娅婇柡宀嬬節閸┾偓妞ゆ帊绀侀閬嶆倵濞戞瑯鐒介柛妯绘倐濮婃椽宕ㄦ繝鍕ㄦ濠电偛鐪伴崐婵嗙暦椤栫偞鍋愰悹鍥皺閸濇绻涚€电孝妞ゆ垵妫濋幃锟犳偄閸忚偐鍘甸梻渚囧弿缁犳垿鐛Δ鍐＝鐎广儱妫楅悘鎾煛瀹€瀣К缂佺姵鐩俊鐤槾闁哄棎鍊濆娲传閸曨厾浼囬梺鍝ュУ閻楁洟鎮鹃悜钘壩╅柨鏂垮⒔閻﹀牓姊洪崨濠傚闁告柨顦靛畷鎴﹀箻鐠囪尙鐤€闂佺粯顨呴悧蹇涘储閸涘﹦绠鹃弶鍫濆⒔閹吋銇勯鐐靛ⅵ鐎殿喗濞婇弫鎰緞鐎ｎ剙骞愬┑鐐舵彧缁插潡骞婇幘瀛樺弿鐎广儱娲犻崑鎾舵喆閸曨剛顦ラ梺缁樼墪閵堢鐣峰ú顏勭労闁告劏鏅涢鎾斥攽椤旂晫绠撻柣顐ｎ殜瀵挳濮€閳锯偓閹峰姊虹粙鎸庢拱闁荤啙鍥佸洭鏁冮崒娑氬幍缂佺偓婢樺畷顒佺娴煎瓨瀵犳繝闈涙灩瑜版帒绀傞柛蹇氬亹缁嬪洭姊洪幖鐐插闁稿﹤娼″濠氭晲婢跺﹦鐫勯梺绋挎湰閼圭偓绂掑鈧娲川婵犲倻浠兼繝鐢靛亹閸嬫挸螖閻橀潧浠﹂柣妤佹礉瑜颁礁顪冮妶鍡楀潑闁稿鎸搁埞鎴﹀灳閼碱剛鐓撻梺鍝勭焿缂嶄線鐛鈧畷锟犳倷閹绘崼鏇㈡⒒娴ｅ憡鎯堥柣顓烆槺缁辩偞鎷呴崜鎻掓濡炪倖甯掔€氼剛绮绘导鏉戠婵烇綆鍓欓悘鈺呮煕閹捐埖鏆柡宀嬬稻閹棃濮€閳垛晛顫岀紓鍌欐祰椤曆囧磹閸ф鏄ラ柣鎰惈缁狅綁鏌ㄩ弮鍥棄闁逞屽墰閸忔﹢寮诲☉妯锋瀻闊浄绲剧瑧婵犵數鍋涢幏鎴犳崲濠靛棭娼栨繛宸簻缁犲綊鏌ｉ幇顓炵祷妞ゎ剙妫涚槐鎾寸瑹閸パ勭亶闂佽崵鍠嗛崕鐢搞€佸鑸垫櫜闁搞儯鍔岄悵鏉库攽閻愬瓨缍戞い鎴濇閿濈偛顓奸崨顏呮杸濡炪倖姊婚悺鏂库枔閺冣偓娣囧﹪顢曢敐鍥ㄥ櫚閻庤娲橀幑鍥箖娴犲宸濆┑鐐靛亾鐎氳偐绱撻崒姘偓鐑芥倿閿曚焦鎳岄梺璇茬箰濞存岸宕㈤悾灞惧床婵炴垯鍨圭粻缁樸亜閺冨洤袚闁哄棛濮风槐鎾存媴缁嬪簱鍋撻崷顓熸殰闁跨喓濮伴埀顑跨椤粓鍩€椤掑嫬绠栭柕蹇嬪€栭幆鐐烘煕閿旇骞楁い顐㈢焸濮婂宕掑▎鎴М闂佸摜鍠庡锟犲箚閸愩劉鏀介悗锝庝簽閿涙盯姊虹憴鍕姢闁诲繐鐗撳畷鎴﹀箻閼姐倕绁﹂梺鍓茬厛閸犳牗鎱ㄦ惔鈾€鏀介柣鎰皺濠€鎾煕閺冣偓閸ㄧ敻顢氶敐澶婄闁芥ê锛夎椤法鎹勬笟顖氬壈闂侀€炲苯澧柣蹇旀皑閹广垹鈽夐姀鐘诲敹濠电娀娼ч悧鍛存惞鎼粹檧鏀介柣鎰级閸ｆ椽鏌ㄩ弴妯虹伈鐎殿喛顕ч埥澶愬閻樻鍟嬮梺鑽ゅЬ濞咃綁宕曢妶澶嬪€堕柛顐犲劜閳锋垿姊婚崼鐔衡姇妞ゃ儲鐟х槐鎺楀焵椤掍胶鐟归柍褜鍓熼弫鎰版倷瀹割喚鍙嗛梺鍓插亝缁诲嫰宕濋敃鈧—鍐Χ閸℃鐟愬┑锛勫仦鐢偤骞戦姀銈呭耿婵☆垵顕у鍨攽閻樼粯娑ф俊顐ｇ洴閹偤鎮欓璺ㄧ畾闂佸憡鐟ラˇ顖炲礉閵堝棎浜滄い蹇撳閺嗭絽鈹戦埄鍐ㄥ姸缂佺姵鐩獮姗€鎳滈弫灞惧浮濮婄粯鎷呮笟顖滃姼濡炪倖鍨甸幊姗€寮崘顔嘉у鑸电〒缁?
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
        // up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵挳鎮欏ù瀣壕鐟滅増甯掔壕鍧楁煙鐎电校闁哥姵鍔欓弻锝呂旈埀顒勬偋閸℃瑧绠旈柟鐑樻⒒绾惧ジ鏌嶈閸撴艾顕ラ崟顓濇勃缂佸銇樻竟鏇㈡⒑缁嬭法绠版い锔诲灡缁傚秵銈ｉ崘鈹炬嫼闂備緡鍋嗛崑娑㈡嚐椤栨稒娅犻柟缁㈠枟閻撴洟鏌嶇憴鍕姢濞存粎鍋撴穱濠囨倷椤忓嫧鍋撻弽顐ｆ殰濠电姴瀚惌鍡椼€掑锝呬壕閻庤娲滈弫濠氥€佸Δ鍛妞ゆ帒鍊搁獮鍫熺節閻㈤潧浠滄俊顐ｎ殘閹广垽骞囬弶鎸庤緢闂佹寧绻傞ˇ浼存偂韫囨稒鐓曢柍鈺佸暞缁€鍫熴亜閿旇偐鐣甸柟钘夌埣瀵粙顢橀悢鍝勫笚闁荤喐绮嶇划鎾崇暦濠婂牊鍋勯柣鎾虫捣娴煎姊洪幐搴㈢闁稿﹤鎲￠崕顐︽⒒娴ｅ摜鏋冩俊妞煎妿缁牊鎷呴搹鍦箵濠电偞鍨堕敃鈺併€掓繝姘厪闁割偅绻冮ˉ鐐淬亜閵夈儲顥滈柍瑙勫灴椤㈡瑩宕崟銊ヤ壕婵犻潧顑冮埀顑跨閳诲酣骞樺畷鍥р偓鐐差渻閵堝棗绗傞悹浣圭叀楠炲繒绱掑Ο鑲╊啎闁哄鐗嗘晶浠嬪礆娴煎瓨鐓欓悹鍥囧懐鐦堥悗瑙勬处閸ㄥ爼骞冨▎鎾村仺闁汇垻顣槐鏌ユ⒒娴ｅ憡鍟炵紒璇插€婚埀顒佸嚬閸欏啫鐣烽幋锕€唯鐟滃宕戦幘鑸靛枂闁告洦鍓涢ˇ銊モ攽閻愯泛鐨洪柛鐘查叄閹儳鐣￠幏鏃傚枑缁轰粙鎸婃径瀣闂侀€炲苯澧剧紓宥呮瀹曟垿宕卞Ο纰辨锤濠电偛妫欓幐濠氬煕閹烘垯鈧帒顫濋敐鍛婵犵數鍋橀崠鐘诲川椤旂厧绨ラ梻浣稿閸嬩線宕曟潏鈺侇棜鐟滅増甯楅悡娑氣偓骞垮劚妤犳悂鐛Δ鍛厱閻庯綆浜滈埀顒€婀遍幑銏犫槈閵忕姷顓洪梺缁樺灥濞诧絿娆㈤悽鍛娾拺缂備焦蓱閻撱儲淇婇锝囩疄妤犵偛鍟撮幃鐑藉箠婵犲啯銇濇い銏℃瀹曞崬鈻庨幋鐘茬倞闂傚倷娴囬褏鈧稈鏅犻幆宀勵敋閳ь剙鐣烽敓鐘茬闁芥ê顦抽幗鏇㈡⒑閹稿海鈽夐悗姘€鍥х；闁瑰墽绮弲鏌ュ箹鐎涙绠橀柣鈺侀椤啴濡堕崱妤冾伝闂佹悶鍔嬬划娆忣嚕婵犳碍鍋勯柛婵勫劤閹虫繈姊洪崜鑼帥闁哥姵鐗犻垾鏍醇閵夛妇鍙勬繛鏉戝悑閻熝囧箖閼测晝纾奸弶鍫涘妼缁椦囨煃瑜滈崜銊х礊閸℃顩查柣鎰嚟椤╃兘鏌曞娑㈩暒缁ㄨ顪冮妶鍡楀潑闁稿鎸婚妵鍕晝娴ｅ嘲鎯堝┑鈥冲级閸旀瑩鐛Ο灏栧亾濞戞顏堫敁閹惧墎纾藉ù锝呭閸庡繒绱掗妸鈺€鎲剧€规洘鍨块獮妯兼嫚閸欏绁舵俊鐐€栭幐楣冨窗閹邦兘鏋斿ù鐓庣摠閳锋垿鎮归幁鎺戝闁哄鏌ㄩ埞鎴︻敊閼恒儱鍞夐悗娈垮枛閹诧紕绮悢鐓庣劦妞ゆ帒瀚拑鐔兼煟閺傚灝鎮戦柛瀣儔閺屾盯顢曢妶鍛亖闂佸憡鍩婄换婵嗩潖缂佹ɑ濯寸紒瀣劵閸╃偛鈹戦悙棰濆殝濠碘€虫喘楠炴垿濮€閻橆偅顫嶉梺闈涚箳婵兘顢樺ú顏呪拺缂備焦锚婵矂鎮樿箛鏃傛噰闁诡喚鍏樻俊鎼佹晜閸撗屽晭闂佽瀛╃粙鎺椻€﹂崶顒佸剹闁圭儤姊荤壕濂告煟濡櫣锛嶅褏鏁搁埀顒侇問閸犳盯顢氳閸┿儲寰勯幇顒夋綂闂佺粯顭囬弫鎼佸级閹间焦鈷掗柛灞捐壘閳ь剚鎮傞幃褎绻濋崟顓犵厯闂佸湱鍎ら〃鍡涘磹閸洘鐓曟い鎰Т閸旀粓鏌ｉ幘璺烘灈妤犵偞鐗曡彁妞ゆ巻鍋撻柣蹇撳船椤法鎲撮崟鍡樺灴婵＄敻宕熼娑欐珖闂佺鏈悷锕傚窗濮樿埖鈷戠紒顖涙礃閺夊綊鏌涚€ｎ偅灏い顏勫暣婵″爼宕卞Δ鈧鎴︽⒑缁嬫鍎愰柟绋垮⒔濡叉劙骞掑Δ浣镐汗闂佽偐鈷堥崕鐤槾闁逞屽墲椤煤濡ソ娲偄閻撳氦鎽曢梺绯曞墲椤ㄥ繘宕ョ€ｎ喗鐓曟繛鎴濆船閻忥綁鏌＄€ｎ亜鏆ｆ慨濠冩そ閹兘寮堕幐搴♀偓顖炴⒑鐠団€崇仩闁活厼鍊块獮鍐焺閸愩劎绐為悗鍏夊亾闁逞屽墰缁牊绻濋崶銊у幐閻庡箍鍎辨鎼佺嵁濡ゅ懏鐓熼柟鎯х－缁犲鏌＄仦鍓ф创妞ゃ垺娲熼弫鎰板炊閳哄啯姣囩紓鍌氬€风拋鏌ュ磻閹惧灈鍋撻崗澶婁壕闂佸憡娲﹂崜娑㈠储闁秵鐓熼幖鎼灣缁夌敻鏌涚€ｎ亝鍣归柣锝呭槻閻ｆ繈鍩€椤掑嫬鐒垫い鎺戝枤濞兼劖绻涢崣澶岀煉鐎规洘顨呴～婊堝焵椤掑嫬违濞达綀鍊介悢灏佹瀻闁绘劦鍎烽鍕拻濞达絽鎲￠崯鐐存叏婵犲倻绉烘鐐插暣瀹曟帡鎮欓懠顒傛毇婵犲痉鏉库偓鎰板磻閹剧粯鐓冪紓浣股戝畷宀€鈧娲栫紞濠囥€佸▎鎾村仼閻忕偠鍋愰惌姘舵⒒閸屾艾鈧兘鎳楅崜浣瑰厹闁割偅娲栭悿鐐節婵犲倻澧涢柛瀣儔閺岋絽螣閸喚姣㈤梺鍝勬４闂勫嫰骞堥妸銉建闁糕剝顨呯粻鑽ょ磽娴ｅ弶顎嗛柛瀣崌濮婄粯鎷呯憴鍕哗闂佺瀵掗崹璺虹暦濠靛棭鍚嬪璺猴功閿涚喎顪冮妶鍛闁绘锕幃锟犳偄閸忚偐鍘介梺鍝勫€圭€笛囧箟閹间焦鐓欓柛鎰絻閺嗭綁鏌熼绛嬫當妞ゎ偅绻堥幃鈩冩償閳锯偓閹奉偊姊绘担绛嬪殐闁搞劋鍗冲畷顖炲箻椤旇　鍋撴笟鈧鎾閳╁啯鐝曢梺鑽ゅ枑閻熻京寰婇崜褉鍋撳顑惧仮婵﹦绮幏鍛村川婵犲倹娈樼紓鍌欑椤戝棝宕归崸妤€鏋侀柛鎰靛枛鍥存繝銏ｆ硾椤戝懘鍩€椤掆偓閻栧ジ寮婚敐澶婃闁割煈鍠楅崐顖炴⒑缂佹ɑ灏版繛鍙夌墵閸╃偤骞嬮敂钘変汗闂佸湱绮敮妤呮倶閹炬枼鏀介柣鎰皺婢ф稓绱掗鑺ュ磳鐎殿喖顭烽弫鎰板川椤忓拋娼旈梻浣告啞椤ㄥ牓宕戦悙鐑樺仧婵犻潧娲ㄧ弧鈧┑鐐茬墕閻忔繈寮搁幘缁樼厱閻庯綆鍋呭畷灞炬叏婵犲啯銇濇俊顐㈠暙閳藉顫濇潏鈺傛緰闂傚倷鐒﹂幃鍫曞礉瀹€鍕ㄢ偓锕傛倻閽樺鐎悗骞垮劚閹峰鎮￠妷鈺傜厱妞ゆ劑鍊曢弳閬嶆煙闁垮銇濇慨濠冩そ瀹曘劍绻濋崟顓犳殼闂佽瀛╅崙褰掑礈閻斿鍤曢柟鎯板Г閸婄兘鏌ｉ幋鐑嗙劷闁告﹢浜跺娲濞淬倖鐩畷鐔碱敆娴ｇ儤顫滈梻鍌氬€峰ù鍥敋閺嶎厼绐楁俊銈呮噺閺呮繈鏌曡箛瀣偓鏇犵不閺嶎厽鐓冮柛婵嗗閸ｅ綊鏌涢妸銉モ偓褰掑Φ閸曨垰绠涢柍杞拌兌娴犻箖姊虹拠鏌ュ弰缂佺姵鐗曢～蹇曠磼濡顎撻梺鎯х箳閹虫挾绮垾鎰佹富闁靛牆鍟俊濂告煙閾忣偓鑰挎鐐插暢椤﹀墎绱掗悩宕囨创妤犵偞蓱閹峰懘鎳為妷顔诲摋闂備線鈧偛鑻晶顖滅磼鐎ｎ偄娴柡浣稿暣閸┾剝鎷呴崣澶屼簴濠电偛顕慨鎾敄閸℃稒鍋傞柣鏂垮悑閻撴瑩姊洪銊х暠妤犵偞顨堢槐鎾愁吋閸℃浠哥紓浣虹帛閻╊垶鐛弽顓ф晣闁绘劕顕禍顏堟⒒娴ｅ摜鏋冩い顐㈩樀瀹曞綊宕稿灏栧亾娴ｇ硶鏋庨柟鐐綑娴狀厽淇婇锔绘殥缂佲偓娓氣偓钘熸慨妯垮煐閳锋帡鏌涚仦鎹愬闁逞屽墰閸忔﹢骞冮悙鐑樻櫆闂佹鍨版禍鍓х磼濡や胶鈽夐柟铏姍閹苯鈻庨幘瀵稿幍闁诲海鏁搁…鍫ｂ叴闂備胶顭堥鍡涘箰閹间緤缍栨繝闈涱儏鍞梺闈涱槶閸庡搫顭囨惔銊︾厽閹兼番鍊ゅ鎰箾閸欏鑰跨€规洖缍婂畷绋课旈埀顒傜不閺嶃劎绠鹃柛鈩兩戠亸浼存煕婵犲嫮甯涘ǎ鍥э躬椤㈡盯鎮欑€涙ɑ娈搁梺璇插閼归箖濡剁粙娆炬綎缂備焦蓱婵挳鏌涘☉姗堝伐闁哄棗鐗忕槐鎾存媴閸撴彃鍓遍梺鎼炲妼濞尖€愁嚕椤愶箑绀冩い鏃囧Г濞呭洭姊虹粙鎸庢拱婵ǜ鍔戦幃鍧楀炊椤掍讲鎷洪悷婊呭鐢帗绂嶆导瀛樼厱閹兼惌鍠曢柇顖炴煙椤旀儳鍘撮柡灞芥喘閺佸倿骞嗚濮规煡姊婚崒娆戭槮闁规祴鍓濈粭鐔肺旈崨顓炲亶闂佸憡绻傜€氣偓闁挎繂顦粻娑㈡煟濡も偓閻楀棝鍩€椤掆偓閻忔氨鎹㈠☉銏犻唶婵犻潧鐗呴搹搴♀攽閻愬弶鍣洪柨鏇樺灲瀵鈽夊锝呬壕闁挎繂绨奸幃宥夋煛閳ь剛鎷犻惈顒€閰ｅ畷鎯邦檪闂婎剦鍓熼弻鐔兼惞椤愩倗鐓夊┑鈽嗗亜閸燁偊鍩ユ径濠庢僵閺夊牃鏅涚粊锕€鈹戦悩鍨毄濠殿喚鍏樺顐﹀川婵犲啫寮块柣搴ｆ暩绾爼宕戦幘鎰佹僵妞ゆ劑鍨圭粊顕€姊虹拠鈥虫灆闁告濞婇悰顔嘉熼懖鈺冿紲濠碘槅鍨堕弨杈┾偓姘虫閳规垿鎮欑€涙ê闉嶉梺绯曟櫅閸熸潙鐣烽幋锕€绠婚悗闈涙憸椤旀洘绻涙潏鍓у埌闁哥喎鐏濋埢鏂款潩鐠鸿櫣鍊炲銈嗗笒椤︿即寮查敐澶嬧拺闂傚牊绋撶粻鍐测攽椤旀儳鍘撮柡浣哥Ч閹垽宕楃亸鏍ㄥ濠电偞鎸婚懝鎯洪妶鍡樻珷妞ゆ柨澧界壕鐓庮熆鐠哄搫顒㈢痪顓㈢畺閺岋綁鏁愰崨顖欑驳缂備焦顨堥崰鏍春閳ь剚銇勯幒鎴濐仼闁藉啰鍠栭弻锝夊棘閹稿骸鏆堢紓鍌氱Т濞差參寮诲☉銏犵婵°倐鍋撻柟鍐茬箰閳绘捇宕妷褏锛濋梺绋挎湰閻熴劑宕楃仦瑙ｆ斀妞ゆ梻鈷堥崵鐔虹磼鏉堚晛浠︾紒缁樼箞瀹曟儼顦撮柣锝呮惈閳规垶骞婇柛濠冨姍瀹曟垿骞樼紒妯煎帗闂佽姤锚椤﹁棄危閹间焦鐓冮悹鍥皺鏁堥梺鍦帶缂嶅﹪銆侀弴銏狀潊妞ゎ偒鍘介崑鍛磽閸屾艾鈧娆㈤敓鐘茬獥婵°倐鍋撴い顐ｇ箞婵℃悂鍩℃担渚敼闂備礁鎲″ú锕傚垂閻楀牆鍔旈梺鑽ゅ枑缁瞼鎹㈠鈧顐㈩吋閸滀胶鍙嗛梺鍓插亞閸犳捇宕㈤崨濠勭瘈闁靛骏绲剧涵楣冩煥閺囶亪妾柡鍛埣楠炲鏁傞悾灞藉箞闂備礁鎼崐鍦偓绗涘洤绠查柨婵嗘川绾惧吋銇勯弮鍌楁嫛闁绘捁鍋愰埀顒侇問閸犳洜鍒掑▎鎾扁偓渚€寮撮姀鐘栄囨煕閵夆晩妫戦幖鏉戯躬濮婄粯鎷呯粵瀣闂佸憡鍨归弲顐ゆ閻愬搫骞㈡俊顖溾拡濞叉悂鏌ｉ悩鐑樸€冪紓宥呯Ч閺佹劖寰勬繝鍌滅崺濠电姷鏁搁崑娑㈠Φ濡壈濮冲┑鐘崇閳锋垿鏌ｉ悢鍛婄凡闁抽攱妫冮弻娑氣偓锝冨妼婵倻鈧娲橀崹鎸庢叏閳ь剟鏌嶉崹娑欐珔濞存粓绠栭幃宄扳枎韫囨搩浠剧紓浣插亾闁逞屽墴濮婃椽妫冨☉姘杸闂佽鍠栭崐鎸庝繆鐎涙鐟归柍褜鍓欓锝夊箻椤旂⒈娼婇梺鏂ユ櫅閸燁垶鎮甸幎鑺モ拻濞达綀顫夐崑鐘绘煕鎼搭喖娅嶇€殿喗褰冮埥澶愬閵忕姷銈﹂梻渚€鈧偛鑻晶瀛樻叏婵犲偆鐓肩€规洘甯掗～婵嬵敄閽樺澹曢梺褰掓？缁€浣哄閻熼偊娓婚悗锝庝簻椤掋垺銇勯幇顖毿撻柟渚垮妼椤粓宕卞Δ鈧埛鎺楁⒑缂佹ê绗傜紒顔界懇瀵濡堕崱妯哄伎闂佸綊鍋婇崗姗€宕戦幘璇插唨妞ゆ劗鍠庢禍鐐叏濮楀棗澧婚柣鎾炽偢閺屸€崇暆鐎ｎ剛袦閻庢鍣崳锝呯暦閹烘埈娼╂い鎴ｆ硶鐢稓绱撻崒姘偓椋庢媼閺屻儱纾婚柟鐐墯閻斿棝鎮归崫鍕儓妞わ富鍓熼弻娑氣偓锝庡亝瀹曞瞼鈧娲橀〃鍡楊嚗閸曨剛绡€濞达絽澹婂Λ婊堟⒒閸屾艾鈧绮堟笟鈧獮澶愬灳鐡掍焦妞介弫鍐磼濮橀硸妲舵繝鐢靛仜濡瑩宕濆Δ鍛偓鍛暋閹佃櫕鏂€闂佺鏈喊宥夊疮閻愮儤鐓冮梺鍨儏閻忊晝绱掓潏銊﹀鞍闁瑰嘲鎳橀幆鍌炴⒐閹邦喛绻戦梻鍌欒兌缁垳鏁悙鍝勭柧妞ゆ劑鍊栭～鏇㈡煙閻戞﹩娈㈤柡浣割儔閺屾稑鈽夐崡鐐茬婵炲濮甸幃鍌氼潖閾忚瀚氶柍銉ㄦ珪閻濐垳绱撴担铏瑰笡缂佽鍊块幃楣冩倻閽樺顔婇梺鐟扮摠濮婂綊寮悩缁樷拺閻犲洠鈧磭鈧鏌涚仦鎯у毈婵☆偅绮岄埞鎴炲箠闁稿﹥顨嗛幈銊╂倻閽樺锛涘┑鐐村灟閸ㄦ椽宕愰懜鐢电瘈闂傚牊渚楅崕蹇曠磼閳ь剚寰勭€ｎ偄寮垮┑鈽嗗灠閻忔繈鎮￠幇鐗堢厽闁规儳顕幊鍥煛鐏炲墽娲存鐐叉喘椤㈡稑顫濇潏顭戝殑缂傚倸鍊烽懗鑸垫叏閹呮殾闁汇垻顭堥悞?
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝曢梻浣藉Г閿氭い锔诲枤缁辨棃寮撮姀鈾€鎷绘繛杈剧秬濞咃綁濡存繝鍥ㄧ厱闁规儳顕粻鐐烘煙椤旂偓鐒挎い锕€缍婇弻鐔碱敍濮樿京鍔Δ鐘靛仦椤洨妲愰幒鎳崇喓鎮伴垾鍏呭闂佸壊鐓堥崑鍡欑不妤ｅ啯鐓欓悗鐢殿焾閸撳崬霉閻撳孩顥㈤柡宀€鍠愬蹇涘礈瑜忛弳鐘绘⒑濞茶骞楁い銊ワ躬閹繝顢曢敃鈧悙濠囨煏婵炑€鍋撳┑顔兼喘濮婅櫣绱掑Ο璇叉殫闂佸摜濮甸悧鐘差嚕婵犳碍鏅搁柣妯垮皺閿涙粌鈹戦鐭亣銇愭径鎰優閹兼番鍔嶉埛鎴︽偣閸ワ絺鍋撻搹顐や憾闂備焦鎮堕崝灞筋焽閳ユ剚鍤曢悹鍥ㄧゴ濡插牊绻涢崱妯哄闁告柨鎳樺娲箰鎼达絺妲堥柣搴㈣壘閹虫捇藝瑜版帗鐓涚€光偓鐎ｎ剛锛熸繛瀵稿缁犳挸鐣峰鍡╂Ъ闂佸憡甯楅惄顖炲箖濡も偓閳绘捇宕归鐣屼憾闂備浇顫夐悺鏇炩枍閺囩喓鈹嶅┑鐘叉搐缁犵懓霉閿濆牆鈧粙濡搁埡鍌滃帾婵犮垼顕栭崹浼存嚐椤栫偛鐤炬い蹇撶墛閳锋帒霉閿濆洨鎽傞柛銈嗙懄閹便劌顫滈崼銏㈡殼闂佹寧绻勯崑鐐差嚗閸曨剛绠鹃柟鐑樻尰閵囨繄鈧鍠曠划娆撱€佸鈧幃銏ゆ憥閸屾艾姣愬┑鐘垫暩婵炩偓婵炰匠鍥х９濠电姵鍩冮埀顒婄畵瀹曞ジ濡烽敃鈧悵鏃堟⒑缂佹ɑ鐓ラ柟鑺ョ矌缁粯銈ｉ崘鈺冨幍闁诲海鏁搁…鍫ュ焵椤掍礁鐏ユい顐ｇ箘閹瑰嫰宕崟顐ｎ吇闂備胶鎳撻崥瀣偩椤忓牆鍨傞柤濮愬€楁稉宥呂旈敐鍛殲闁绘挻娲熼弻宥夊煛娴ｇ绠诲銈呯箰閻楀﹪宕愰崼鏇熺厓闁告繂瀚弳鐐烘煕鐎ｎ偆澧甸柡灞诲姂瀵挳鎮欏ù瀣壕鐟滅増甯掗悡鏇㈡煙閻戞ê鐏熼柍褜鍓氱敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戦悩鍨毄闁稿濮电粩鐔煎醇濠靛牊娈鹃梺姹囧灩閻忔岸锝為弴銏＄厽闁归偊鍓﹂崵鐔兼煟閻旈绉洪柡灞界Х椤т線鏌涢幘瀵哥畵閾荤偤鏌曡箛瀣偓鏍煕閹达附鍋℃繛鍡楃箰椤忊晝绱掗埀顒勫礃閳瑰じ绨婚梺鍝勬搐濡骞婇幇顓犫枖鐎广儱妫庢禍婊堟煛瀹ュ骸浜滃ù鐙呯畵閺岋綁骞樼捄鐑樼€炬繛锝呮搐閿曨亪銆佸☉姗嗙叆闁告侗鍘鹃濂告⒒娓氣偓濞佳兠洪妸鈺佺獥闁哄稁鍘奸拑鐔兼煃閳轰礁鏆炲┑顖涙尦閺屾稑鈹戦崟顐㈠闂佹寧绻傞ˇ浼存偂濞嗗繈鈧帒顫濋鍌欒檸婵犵鈧啿鎮戦柕鍥у椤㈡洟鏁愰崶鈺冨帨闁诲氦顫夊ú鏍х暦椤掑啰浜欓梻渚€鈧偛鑻晶鎾煥濠靛牆浠辨い銏℃礋閺佹劙宕堕埡濠傛櫗闂備浇顕уù鐑藉极閹间讲鈧箓宕奸姀鈩冩闂佸壊鍋呭ú姗€鎮￠悢闀愮箚闁靛牆瀚崝宥夋倶韫囷絽骞楅柕?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帗绂掓ィ鍐┾拺缂佸鐏濋銏ゆ煕閵娿儲鍋ョ€殿喖顭烽弫宥夊礋椤忓懎濯伴梺鑽ゅТ濞诧箒銇愰崘鈺傚弿閹兼番鍔嶉悡鏇㈡煏婢舵盯妾悘蹇ラ檮閹便劍绻濋崨顕呬哗闂佸憡鐗楅悧鐘差嚕閹绢喗鍊锋繛鏉戭儐鐎氱喎鈹戦敍鍕杭闁稿﹥鍨垮畷婵嗙暆閸曨剚娅囬梺闈涚墕閻楀繑绂嶉柆宥嗏拻濞达綀娅ｇ敮娑㈡煕閵娿儱鎮戦柟渚垮姂婵¤埖寰勬繝鍕箞?闂?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閻樻爠鍥ㄧ厱閻忕偛澧介悡顖氼熆鐟欏嫭绀€闁宠鍨块、娆撴儗椤愵偂绨婚柣锝囧厴椤㈡宕熼銏犱憾闂佽娴烽弫鍝ユ兜閸洖纾婚柟鎹愬煐閸犲棝鏌涢弴銊ュ妞わ富鍠栭—鍐Χ鎼粹€茬盎濡炪倧绠撴禍鍫曞春閳ь剚銇勯幒鎴濐仾婵炴嚪鍥ㄧ厪闁割偁鍩勯悞鐐亜閺囶亞绉€规洖銈稿鎾倷濞堝灝鎮堥梻鍌欒兌缁垶鏁嬮梺璇茬箲閼规崘顣鹃柟鍏肩暘閸ㄨ崵寮ч埀顒勬⒑濮瑰洤鐏叉繛浣冲嫮顩烽柨鏇炲€归悡娆戠磼鐎ｎ偒鍎ラ柣鎾瑰亹閳ь剝顫夊ú妯侯渻閽樺鏆︽慨妞诲亾妞ゃ垺鐟╅幊鏍煛鐎ｎ剙鑰块梻鍌氬€烽懗鍫曞箠閹惧瓨娅犻柣锝呰嫰閸ㄦ繃銇勯弽顐沪闁稿鍊圭换娑㈠幢濡搫顫囨繛瀛樼矋缁捇寮婚敓鐘茬＜婵犻潧娲ㄩ妴濠冪箾鐎电袨闁搞劌顭烽獮鍐敊绾拌鲸鍍靛銈嗗灱婵倝寮埀顒勬⒒娴ｈ櫣甯涢柛鏃€鐟ラ…鍥槾闁轰緡鍣ｅ畷婊勬媴閾忕櫢绱茬紓鍌氬€烽悞锕傛晝椤愶附鍤€閻犳亽鍔夐崑鎾舵喆閸曨剛顦ㄥ┑锛勫仒缁瑥顕ｉ锕€绠荤紓浣姑禍褰掓⒑閼测斁鎷￠柛鎾卞妿濡叉劙宕ｆ径宀€鐦堥梺姹囧灲濞佳勭閿曞倹鐓曢柕濞垮劤閸╋綁鏌熼鎯у幋妞ゃ垺鐩幃娆撴嚑缂傚孩娲熷铏圭矙鐠恒劎浼囬梺绋款儑閸嬨倝宕哄☉銏犵婵°倓鑳堕崢閬嶆煟韫囨洖浠滃褌绮欓獮濠囧川椤旇桨绨婚梺鎸庢煥閵堢顬婇鍓х＜缂備焦顭囩粻鐐烘煕閳规儳浜炬俊鐐€栫敮鎺斺偓姘煎弮閹偤鎳栭埞鎯т壕閻熸瑥瀚亸顏堟煕鎼淬劋鎲鹃挊婵嬫煟閵忕姵鎲哥€规挷绶氶弻娑㈠Ψ椤旂厧鈷掗梺杞扮鐎氭澘顫忛崫鍕ㄦ瀻闁诡垎鍛摋濠电偛鐡ㄧ划鎾剁不閺嶎厼钃熼柣鏂垮悑閸婄粯鎱ㄥΔ鈧Λ娆撴偩閸洘鈷戦柛锔诲弾閺€浼存煕閳哄倻澧靛┑锛勬暬瀹曠喖顢涘槌栧晪闂佽娴烽弫鍝ユ兜鐠轰綍锝夊醇閻旇櫣顔曢柣搴㈢⊕椤洭鎯岄幒鏃傜＜闁绘ê纾晶顏呫亜椤愩垻绠茬紒缁樼箓椤繈顢楅崟顐ｆ毆闂傚倷鑳剁划顖氼潖鐟欏嫮顩插ù鐘差儏閻掑灚銇勯幒宥囶槮婵炴惌鍣ｉ弻鈥崇暆鐎ｎ剛袦濡ょ姷鍋炵敮锟犵嵁鐎ｎ喗鍊烽柡澶嬪灦閻庨箖姊婚崒娆戠獢闁逞屽墰閸嬫盯鎳熼娑欐珷闁告瑥顦禍婊勩亜閹扳晛鐒烘俊鍙夋倐閹繝濡堕崱鏇犵畾濡炪倖鐗楅懝楣冨箖閹寸姭鍋撶憴鍕闁绘搫绻濆璇测槈閵忊晜鏅濋梺闈涚墕閹冲繘鎮楅搹鍦＝濞达絿鐡旈崵娆愪繆椤愶絿绠炵€殿喖顭烽幃銏ゅ礂鐏忔牗瀚介梺璇查叄濞佳勭珶婵犲伣锝夘敋閳ь剟寮诲☉銏″€婚柛鈩冾殘娴犵鈹戦纭锋敾婵＄偘绮欓悰顕€骞囬鐔峰妳闂侀潧娴氬浣烘閳哄懏鈷掑ù锝勮閻掔偓銇勯幋婵囶棦妤犵偞鍨垮畷鍫曨敆閳ь剟鎷戦悢鍝ョ闁瑰瓨鐟ラ悘鈺冪磼閻樺樊鐓奸柡宀€鍠愰敍鎰媴閽樺甯梻浣告啞閼归箖顢栭崨鏉戠厴闁硅揪瀵岄弫濠勭棯椤撶偛妲诲ù鍏煎姍濮婃椽宕烽娑欏珱闂佺顑呭Λ婵嗙暦閿濆宸濇い鏍ㄧ矌閿涙繈姊虹粙鎸庢拱闁荤啙鍥х鐎广儱顦伴悡銉︽叏濡灝鐓愰柣鎾冲暣閹綊宕堕鍕缂備胶濮风划顖炲箞閵婏妇绡€闁告劏鏂傛禒銏ゆ倵鐟欏嫭纾搁柛鏂跨Ф閹广垹鈹戠€ｎ亞顦ㄥ銈呯箰鐎氼剙效閺屻儲鈷掗柛灞剧懅閸斿秹鎮楃粭娑樺幘濞差亝鏅滈柣锝呯焾濞茬顪冮妶鍛閻庢凹鍣ｅ鎶芥晜閻ｅ瞼顔曢悗鐟板閸犳洜鑺辨繝姘厽闁圭儤鍨规禒娑㈡煏閸パ冾伃妤犵偞甯掗鍏煎緞鐎ｇ鍋撻弽銊х閻庢稒顭囬惌瀣磼椤旇姤宕岀€殿喖顭烽幃銏ゆ偂鎼存繄鐐婇梻浣告啞濞诧箓宕滈敃鈧灋闁绘劕顕粻楣冩倵濞戞瑯鐒介柣顓熷笧缁辨帡鎮╁畷鍥р吂濡炪値鍋勭换姗€骞栬ぐ鎺戞嵍妞ゆ挾濮烽崢顖炴⒒娴ｅ憡鍟為柣妤佸灴閸┾偓妞ゆ巻鍋撻柛妯荤墱濡叉劙鏁愰崶鈺冿紳闂佺鏈懝楣冨焵椤掆偓閹芥粎鍒掗弮鍫濈妞ゆ棁濮ゅ▍鍥ь渻閵堝懐绠伴柣妤€绻愰悾鍨瑹閳ь剟寮诲☉銏犵疀闂傚牊绋掗悘鍫ユ煕閻戝棗鐏︽慨濠冩そ瀹曟﹢宕ｆ径瀣壍闂備胶顭堥敃锕傚箠閹捐鐓濈€广儱鎷嬮悡銉╂煕椤愩倕鏆遍柟閿嬫そ濮婅櫣娑甸崨顓濇睏闂佺顑嗛惄顖氱暦濠靛棭娼ㄩ柍褜鍓熷璇测槈閵忕姵顥濋柣鐘充航閸斿酣宕濋姘兼富闁靛牆鍟悘顏堟煙閸涘﹥鍊愰柛鈹垮劜瀵板嫰骞囬澶嬬秱闂備焦濞婇弫顕€宕戦幘缁樼厵闁汇値鍨遍鐘电磼鏉堛劍宕岀€规洘甯掗埢搴ㄥ箣濠靛棭鐎撮梻鍌欑劍鐎笛兠哄澶婄柧婵炴垯鍨归悡婵嬫煙閹规劦鍤欑紒顐㈢Ч閺屾盯顢曢妶鍛亖闂佸憡顭堝Λ鍕煘閹寸偛绠犻梺绋匡攻閸旀瑥鐣烽幋锕€绠荤紓浣诡焽閸欏棝姊洪崫鍕偍闁告柨顑夐弻鍥敍濞戣鲸顔旈梺缁樺姈瑜板啴寮冲▎鎾存嚉闁哄稁鍘介悡鏇熺節婵炴儳浜鹃梺缁樺釜缁犳挸顕ｉ弻銉ョ厸濞达絽寮堕弬鈧?data\.warp-enabled闂?
//   闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟扮粻濠氭煕閳规儳浜炬俊鐐€栫敮濠囨嚄閸洖鐓濋柟鍓х帛閻撴盯鏌涘☉鍗炴灓缂佺姵锕㈤弻娑㈠箳閹惧磭鐟ㄩ梺瀹狀嚙闁帮綁鐛Ο铏规殾闁搞儴娉涢弫钘夆攽閻樿尙妫勯柡澶婄氨閸嬫捇骞樼紒妯衡偓鍧楁煥閺囨浜鹃梺瀹狀嚙缁夌懓顕ｉ浣瑰劅闁规儳鐡ㄩ弶鎼佹⒒娴ｈ櫣甯涢柨姘舵煟韫囨柨鍝虹€殿喗濞婇崺鈩冩媴閸欏鏉告俊鐐€栧ú鏍箠鎼粹槅鍤曟い鏃堟暜閸?  -> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵潙螖閳ь剚绂嶉幆褜娓婚柕鍫濈凹缁ㄥ鏌涢悢鍛婄稇闁伙絿鏌夐妵鎰板箳濠靛洦娅旈梻浣告啞娓氭宕归悧鍫熷弿婵炲樊浜濋埛鎺懨归敐鍕劅婵炲吋甯￠弻娑㈠即閻愬吀绮甸梺浼欑到婢у海妲愰幘瀛樺闁圭粯甯婃竟鏇炩攽閻橆喖鐏辨繛澶嬬⊕瀵板嫰宕堕鈧壕濠氭煙閹规劕鍚圭€规挷绶氶弻娑⑩€﹂幋婵囩亪婵犳鍠栨鎼佲€旈崘顔嘉ч煫鍥ㄦ尵濡诧綁姊洪幖鐐插婵炲鐩、姘舵晲閸℃鍤ら梺鍝勵槹閸わ箓濡搁埡鍌滃弳闂佸搫鍊搁悘婵嬪煕閺冣偓閵囧嫰寮埀顒€煤閻斿娼栨繛宸簼閸嬶繝姊洪銊х暠婵炲牆鐖煎铏圭矙濞嗘儳鍓抽梺鍝ュУ閻楃姵淇婇幘顔肩＜闁绘劘灏幗鏇㈡⒑闂堟侗妾у┑鈥虫喘瀹曘垽骞栨担鍏夋嫼缂備緡鍨卞ú鏍ㄦ櫠閸欏浜滄い鎾跺仦閸犳﹢鏌熼姘辩劯妤犵偞甯掕灃濞达絽鎼獮宥嗕繆閻愵亜鈧牕顫忚ぐ鎺戝嚑濠电姵纰嶉崵鈧梺绉嗗嫷娈曢柣鎾寸懄閵囧嫰寮崒娑欑彧闂佺懓鍟垮ú锔界┍婵犲浂鏁冮柕蹇曞У濞堫參姊虹€圭姵顥夋い锔诲灥閻忓啴姊洪柅鐐茶嫰婢ф挳鎸婇悢鍛婂弿婵☆垰銇橀崥顐ょ棯?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰绠婚柛鈹惧亾濡炪倖甯婇懗鍫曞煝閹剧粯鐓涢柛娑卞灠閳诲牓鏌曢崱鏇狀槮闁宠閰ｉ獮姗€宕橀幓鎺撴殢濠碉紕鍋戦崐鏍箰妤ｅ啫纾婚柣鏂垮悑閸嬫﹢鏌曟径鍡樻珕闁抽攱鍨块弻娑㈡晜鐠囨彃绠归梺鍛婃煥椤戝棝濡甸崟顖毼╅柕澶涚畱濞呇勭節閵忥綆娼愭繛鍙夘焽閹广垹鈹戦崱鈺傚兊濡炪倖鎸炬慨鎾嵁瀹ュ鈷掑ù锝呮啞閸熺偤鏌熺粙鎸庮棦鐎规洏鍔戦、姗€鎮埀顒勫磻瑜斿濠氬磼濞嗘帒鍘＄紓渚囧櫘閸ㄥ爼鐛幇顓滃亝闁告劑鍔庨悰銉╂⒑閸︻厼顣兼繝銏∩戦、濠囨⒒娴ｅ憡鍟炴繛璇х畵瀹曞綊骞嶉鍙ョ瑝濠电偞鍨堕悷锝嗙濠婂牊鐓涢柛灞炬皑娴犮垽鏌熼钘夌伌闁哄矉绲鹃幆鏃堟晲閸パ咁偧缂傚倷鑳剁划顖滅矙閹捐鐒垫い鎺嶈兌閳洘銇勯鐐村枠闁诡垰鐭傞獮鎺楀籍閸屾粣绱查梻浣虹帛閿氱痪缁㈠幗閺呭爼顢旈崨顐＄盎濡炪倖鍔戦崹鑽ょ不娴煎瓨鍋傞柕鍫濇娴滄粓鐓崶銊﹀碍妞ゅ浚鍋呴妵鍕棘閹稿骸鏋犻梺鍝勭灱閸犳牠骞婂┑瀣亱闁割偅绻冮柨顓犵磽閸屾瑧鍔嶇憸鏉垮暣楠炴牠顢曢姀鐘点偒闂傚倷绀佹竟濠囧磻娓氣偓瀹曟洟寮婚妷銉у幈闂佺懓顕慨椋庡閽樺褰掓晲閸ヮ煈浠ф繛鎾寸缁嬫帞鎹㈠☉銏犻唶闁绘柨鎽滈ˇ浼存⒑閸濆嫮鐏遍柛鐘崇墵楠炲啴鎮滈懞銉ヤ罕闂佸壊鍋嗛崰鎰板磹閻戣姤鈷掑ù锝夘棑娑撹尙绱掗幓鎺撳仴鐎规洘顨呴～婊堝焵椤掑嫭鍋樻い鏃囧Г閸庣喖鏌曟繝蹇曠暠闁谎冨缁绘繈濮€閿濆棛銆愰梺娲诲幖閻楁挸鐣烽鐐茬倞鐟滃寮ㄦ禒瀣厽闁归偊鍨伴悡鎰喐閹跺﹤鎳愮壕濂告煛鐏炶鍔ら柣锝嗘そ閺岀喖顢欓懞銉ｄ虎閻庢鍠楅幐鎶藉春閵夆晛骞㈡俊顖濆吹缂堣鈹戦悩鍨毄濠殿喚鍏橀妴鍌烆敊閻愵剦娼熼梺鍝勬储閸ㄥ湱澹曟繝姘厵閺夊牆澧介悾杈╃棯閹呯Ш闁哄备鈧剚鍚嬮幖绮光偓宕囶啈闂備胶顭堥鍛搭敄閸℃鈹嶅┑鐘叉搐鍥撮梺鍛婁緱閸樿棄鈻撻銏♀拺闁告繂瀚烽崕蹇涙煟濡も偓缁绘﹢宕洪埀顒併亜閹哄棗浜剧紓浣哄Т缁夌懓鐣烽弴銏＄劶鐎规挶鍎卞ú顓€佸☉姗嗙叆闁告劗鍋撻ˉ锟犳煟閻斿摜鐭屽褎顨堥弫顔嘉旈崨顓犲幈闂佸湱鍎ら〃鍡涙偂閺囩喆浜滈柟鎵虫櫅閳ь剚娲熷绋库枎閹惧鍘遍柟鍏肩暘閸ㄨ鎱ㄥ鍡╂闁绘劕顕晶顏堟嚕閹邦厹浜滈柟鍝勬娴滅偓绻濈喊妯峰亾閻愯棄浠梺鍝勭焿缂嶄線鐛崶顒夋晢濠电姴鎳庨幆鍫ユ⒒娴ｅ憡鎯堥柡鍫墮鐓ゆ俊顖氬悑瀹曞弶绻涢幋鐐电煠婵℃彃顭峰铏规兜閸涱喚褰ч梺鍛婃⒐閻熴儵顢氶敐澶婄缂佹稑顑嗛弲婵嬫⒑閹稿孩顥嗙悮娆撴煙闁垮銇濋柡宀嬬秮閹晠宕ｆ径濠庢П闂備胶绮幖顐ゆ崲濠靛鈧線骞樺鍕瀹曘劑顢欓幆褍绠洪梻鍌氼煬閸嬪嫬煤閿曗偓铻炲ù锝呮贡閻濊埖鎱ㄥ璇蹭壕闂佺妫勯悥濂稿箖濞嗘挻鍊绘俊顖滃帶楠炲牓姊绘笟鈧埀顒佺☉瀹撳棙绻涙担鍐叉搐閸戠姵绻涢幋娆忕仾闁?
//   婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊鐒﹂崕鎾绘⒑閹肩偛濡奸柛濠傛健瀵鈽夐姀鈺傛櫇闂佹寧绻傚Λ娑⑺囬妷褏纾藉ù锝呮惈灏忛梺鍛婎殕婵炲﹤顕ｇ拠娴嬫婵☆垶鏀遍弬鈧梻浣告啞濞诧箓宕戦崟顒佸弿闁靛繈鍊栭埛鎴炵箾閼奸鍤欐鐐搭殜閺岀喖鎮烽悧鍫濇灎閻庢鍠栭…鐑藉箖閵忋倖鎯為柛锔诲弿缁辨煡姊绘笟鈧褏鎹㈤幒鎾村弿闁汇垹鎲￠崐鍓佲偓骞垮劚椤︿即鍩涢幋鐘电＜閻庯綆鍋掗崕銉╂煕鎼达紕效闁哄本绋掔换婵嬪礃椤忓棛鏉介柣搴㈩問閸犳牠鈥﹀畡閭﹀殨闁圭虎鍠楅崑鍕煣韫囨凹鍤冮柛鐔烽叄濮婄粯鎷呴搹鐟扮闂佸憡妫戠粻鎾崇暦濠婂喚娼╅柤鎼佹涧娴犙囨⒑閸濆嫬鏆欓柣妤€锕崺?-> 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ姀鐘差棌闁轰礁锕弻鈥愁吋鎼粹€崇缂備焦鍔栭〃鍡樼┍婵犲洤围闁告侗鍘藉▓鏌ユ⒑閸濆嫭顥炵紒顔肩У缁岃鲸绻濋崶顬囨煕濞戝崬骞楁繛鍫濈埣濮婂搫煤鐠囨彃绠洪梺鑽ゅ暱閺呯姴顕ｆ繝姘亜闁稿繒鍘ч埀顒傜帛娣囧﹪顢涢悙瀛樼彸缂備緡鍠栭悥鐓庮潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯婢х晫妲愰幒妤佸亹闁惧浚鍋勭喊宥夋⒑娴兼瑧绉柡鈧潏鈺冪处闁伙絽鐬奸惌娆撴偣閸ワ箑瀚庨柛鏂跨Ч婵＄敻宕熼姘辩潉闂佹悶鍎洪悘婵嬪箻鐎靛摜顔曢梺鍛婁緱閸樻儳鐡梻浣哥枃椤宕归崸妤€绠栭柍鍝勬噹閸ㄥ倹銇勯幇鍓佺ɑ闁伙箑楠搁埞鎴︽偐閹颁礁鏅遍梺鍝ュУ瀹€绋款嚕椤愶絿绡€婵﹩鍓涢悿鍥р攽閻樼粯娑фい鎴濇噽缁牓宕奸姀鈥虫瀾闂婎偄娲︾粙鎺楀磿濡ゅ懏鐓曢柍鈺佸暟閳洟鏌ｉ幘瀛樼闁逞屽墮閸樻粓宕戦幘缁樼厓鐟滄粓宕滈悢椋庢殾闁硅揪闄勯崐鐑芥煟閹寸伝顏呯濡も偓閳规垿鎮欓崣澶樻！闂佸湱顭堥…鐑藉箖閳ユ枼鏋庨柟鎵虫櫃缁ㄥ姊洪崫鍕犻柛鏂块叄閸╁﹪寮撮悩鐢碉紲闂佽鍨卞姗€鎯冨ú顏呯厵妞ゆ洖鎳庨弸娑欍亜椤愶絿绠為柟铏矌閸犲﹤螣鐠囧樊浼栭梻鍌氬€风粈浣革耿闁秮鈧箓宕煎婵嗙秺閹粓鎸婃径灞藉笌闂備礁鎼ú銊╁窗瀹ュ洦宕查柛鈩兦滄禍婊堟煙閹佃櫕娅呴柣蹇婃櫊閺岋繝宕遍鐘冲仹闂侀€炲苯澧い鏃€鐗犲畷浼村冀椤撴稈鍋撻敃鍌涚叆閻庯絺鏅濈粻姘舵⒑瑜版帗锛熺紒鈧担鍛婃殰闂傚倷绶氬褏鎹㈤崱妞綁宕ㄩ褏鍔烽梺鍝勫暊閸嬫挾绱掔紒妯肩畼闁哥姴锕よ灒婵炶尙绮紞澶愭⒒娴ｈ鍋犻柛鏂跨У缁绘稒绻濋崒婊勬闂侀潧楠忕紞鍡椢熼崟顖涒拺闂傚牊绋掗ˉ婊堟煕婵犲偆鐓兼鐐差樀楠炴﹢顢欓懖鈺婃Ч婵＄偑鍊曠换鎰涘☉銏″仼濡わ絽鍟埛鎺戙€掑锝呬壕濠电偘鍖犻崶銊ヤ罕闂佺硶鍓濋ˇ鍝劽洪鍕幯囨煕濞戝崬鐏￠柟鍐插暞缁绘繈鎮介棃娴躲儵鏌℃担瑙勫€愮€规洘鍨甸埥澶娢熷鍛瀾缂佺粯绻堝畷鎯邦槻闁稿绉电换婵嬫濞戞ǚ鍋撻幖浣哥疅闁跨喓濮寸壕濠氭煃閸濆嫭鍣洪柣鎾跺枛閺岀喓鈧數顭堝暩闂佺妫勯崐鍧楀蓟閳╁啯濯寸紒瀣濞堝姊洪崫鍕潶闁告柨鐭傞崺鐐哄箣閿曗偓楠炪垺淇婇婵囶仩闁绘稈鏅犲缁樻媴娓氼垱鏁┑鐐叉噺濞叉粓骞戦姀銈呴唶婵犲灚鍔栫紞搴♀攽閻愬弶鈻曞ù婊勭箞閹偞绻濆顓犲幗闂佸綊鍋婇崹浼存嫊婵傚憡鍊垫慨姗嗗幗缁跺弶銇勯鈥冲姷妞わ箓浜堕弻娑樜旀担绯曟灆閻庢鍠涢褔鍩ユ径鎰潊闁绘ɑ顔栭崬娲⒒娴ｇ瓔娼愮€规洘锚閳诲秹寮撮悩鍐茬彴闂佺粯鍔曞Ο濠傘€掓繝姘厪闁割偅绻傞弳娆撴煟韫囷絼绨界紒杈ㄥ笧閳ь剨缍嗛崑鍕倶閹绢喗鐓冮柕澶樺灣閻ｉ潧鈹戦埄鍐╁€愰柛鈹惧墲閹峰懏绗熼娴堆冣攽閿涘嫬浜奸柛濠冪墵瀹曟繈鏁冮崒姘憋紱闂侀€炲苯澧撮柡灞剧〒閳ь剨缍嗛崑鍛焊椤撱垺鐓冮柦妯侯樈濡偓婵犳鍠掗崑鎾绘⒑閹惰姤鏆滈柛瀣崌閺岀喖鏌ㄧ€ｎ偁浠㈠┑顔硷工椤嘲鐣烽幒鎴僵闁告鍎愰弶鍝ョ磽閸屾瑧顦︽い鎴濇楠炴劖绻濆顒傛煣闂佸壊鍋呭ú鏍础閹惰姤鐓熼柡鍐ㄦ处椤忕姵銇勯弮鈧ú鐔煎蓟閿濆牏鐤€闁哄洨鍋樼划鍫曟⒑缁嬪尅鏀婚柟顔煎€垮畷娲焵椤掍降浜滈柟鍝勭Ф鐠愮増绻涢崼鐔虹煉闁哄瞼鍠栭、娆撴倷椤掑倸濮芥俊鐐€х粻鎺楋綖婢舵劕绠為柕濠忓缁♀偓闂佸憡娲﹂崢鑲╃箔閿熺姵鈷戦柣鐔告緲濡插鏌熼搹顐㈠闁告帗甯￠幃鐣屽枈濡桨澹曢梺鎸庣箓缁ㄨ偐鑺辨總鍛婄厽闁挎繂瀚畵鍡樻叏婵犲嫮甯涢柟宄版噺缁楃喖顢涘鍐ㄐ紓鍌氬€烽懗鍓佸垝椤栫偛鍨傞柛锔诲幖閸ㄦ繃绻涢崱妯诲碍缂佺姴顭烽弻锟犲磼濡搫濮曢梺璇查椤嘲顫忓ú顏咁棃婵炴垶姘ㄩ悿鍕⒑缁嬪尅鏀绘繛鍙夌墪鐓ら柛褎顨嗛埛鎺懨归敐鍫綈闁稿濞€閺屾稒鎯旈姀掳浠㈤梺璇″櫙缁绘繂鐣烽悡搴樻斀闁归偊鍠栨慨娲⒒娴ｈ櫣甯涢柡灞诲姂瀹曘儳鈧綆鍠栭悡鏇㈡煙閻戞ê鐏熼柍褜鍓氱敮鎺楋綖濠靛鏁嗗ù锝堫潐閸婂嘲鈹戦悩顐ｅ閻忕偠濮ら悘鎾剁磽娴ｄ粙鍝洪悽顖涱殔椤曘儵宕熼鍌滅槇闁硅偐琛ラ崜婵嬵敊瀹ュ鈷掑ù锝囧劋閸も偓閻庢鍠栭悥濂哥嵁閹版澘绠柦妯侯槹閻庮剟姊虹粙鎸庢拱缂佸甯″畷锝堢疀濞戞瑧鍘甸梻渚囧弿缂傛氨鑺辨禒瀣厽闁宠桨绶氶崣鍕叏婵犲嫮甯涢柟宄版嚇瀹曘劍绻濋崘銊ュ缂傚倸鍊风粈渚€藝娴煎瓨鍋傞柍銉ョ－閺嗭附绻濋棃娑冲姛闁汇倐鍋撻梻浣告惈閸燁偊宕愰弰蹇嬩汗闁诡垎鈧弨浠嬫煃閽樺顥滈柣蹇曞█閺屾稑顫濋澶婂壎閻庤娲橀崹鍨嚕娴犲鏁囬柣鏂挎惈楠炴姊绘担鍛婃儓闁稿﹥鎮傞幃妯衡攽鐎ｎ亞鐣洪梺纭呮彧闂勫嫰鎮￠崘顔解拺闁割煈鍣崕蹇涙煟韫囨梹灏﹂柡宀嬬磿娴狅箓宕滆濡插牓姊虹€圭媭娼愰柛銊ョ仢閻ｇ兘宕￠悙宥嗘⒐缁绘繃鎷呴悷棰佺凹闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢埗鑺ョ☉閳藉濮€閻樼绱遍梻渚€娼ч悧鍡浰囬鐐村珔闁绘柨鎽滅粻楣冩煙鐎电浠ч柟鍐插閺屾稒鎯旈敐鍛亪闂佸搫鏈ú鐔风暦閻撳簶鏀介柛銉戝嫷浠у┑鐘愁問閸犳盯锝炴径鎰┾偓鍐╃節閸屻倖缍庡┑鐐叉▕娴滄繈鎮炴繝姘厽闁归偊鍨伴拕濂告倵濮橆厽绶叉い顓″劵椤т線鏌涢妸銉у煟鐎殿喛顕ч埥澶娢熼柨瀣澑闂備胶纭堕崜婵堢矙閹存繍鍤曞ù鐓庣摠閸婄敻鎮峰▎蹇擃仾缂佲偓閳ь剛绱撻崒姘毙㈤柨鏇ㄤ邯閻涱噣骞囬鐘电槇闂佸憡鍔戦崝搴ｇ玻閻愮儤鐓熼煫鍥ㄦ礀娴犙囨煕鐎ｎ偅宕屾い銏＄懇瀵挳濮€閳锯偓閹风粯绻涙潏鍓ф偧闁硅櫕鎹囬、姘煥閸涱垳锛滈柣鐘叉处瑜板啴鍩€椤掑啫鍔垫俊顐ゅ枎椤啴濡堕崱娆忣潷缂備緡鍠栫€涒晝绮欐径濠庡悑濠㈣泛顑囬崣鍡椻攽閻樼粯娑ф俊顐幖鍗辩憸鐗堝笚閻撶喖鏌熼幑鎰【闁哄鍨佃彁闁搞儜宥堝惈濡炪們鍨虹粙鎴︼綖濠靛鐭楁俊顖涘椤庢姊婚崒娆戝妽闁活亜缍婂畷婵嗩吋閸滀焦瀵屾繛瀵稿Т椤戝懘鎷戦悢鍏肩叆婵犻潧妫Σ鎼佸炊閹绢喗鈷戠紒瀣濠€浼存煟椤撗冩灍缂?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟稒宸濈紒鈾€鍋撻梻浣侯焾閺堫剛鍒掑畝鍕┾偓鍌炴偨閸涘ň鎷洪梻渚囧亞閸嬫盯鎳熼娑欐珷闁告挆鍛紲闂佺粯顭堝▍鏇犱焊椤撱垺鐓熼柨婵嗩槹閺佽京绱掗搹瑙勬珪缂侇喗鐟ラ埢搴ㄦ倷椤掑倻袩闂傚倸鍊烽懗鍫曞磻閵娾晛纾块梺顒€绉撮悿顕€鎮归幁鎺戝濠殿喗濞婇弻鏇熷緞濞戞﹩娲紓浣叉閸嬫捇姊绘担鍦菇闁搞劏妫勯…鍥槼缂佸倹甯￠弫鍐磼濞戞艾骞堟俊鐐€ら崢浠嬪垂閻㈠憡鍊堕柨鏃堟暜閸嬫挾鎲撮崟顒傤槶闂佺娅曢幐楣冨矗閸涱収娓婚柕鍫濇噽缁犱即鎮楀鐓庢珝闁轰礁绉归幃銏ゅ礂鐏忔牗瀚奸梻浣藉吹閸犳劕顭垮鈧崺娑㈠籍閸屾粎锛滃銈嗘礀閹冲酣鎮橀敃鍌涚厵妞ゆ洖妫涚弧鈧悗瑙勬礃閿曘垽銆侀弮鍫濆窛妞ゆ挾鍠撻埀顒傚亾缁绘繈鎮介棃娴躲垽鏌ㄩ弴妯衡偓婵嬪箖瑜斿畷鍗炩枎閹寸姷鍔堕梻浣稿閸嬪棝宕伴幘鍓佷笉濡わ絽鍟悡娆撴倵閻㈡鐒惧ù鐘欏洦鐓欓柛鎰级閸ｆ椽鏌曢崶褍顏柛鈺嬬節瀹曟帒顭ㄩ崘銊у摋闂傚倷绀侀幖顐﹀嫉椤掑倻鐭欓柟杈剧畱閻撯€愁熆鐠轰警鐓繛绗哄姂閺屾盯顢曢妶鍛€鹃梺鍝勬婢瑰棛妲愰幘璇茬＜婵﹩鍏橀崑鎾诲传閵壯呯厠闂佸搫顦伴崵姘洪宥嗘櫍闂侀潧绻掓慨楣冨箺閺囥垺鈷戦柛婵嗗瀹告繈鏌涢悩铏暗鐎殿啫鍥х劦妞ゆ帒瀚ˉ濠冦亜閹扳晛鐏璺哄閺岀喓浜搁弽銊︾彅闁绘挶鍊栭妵鍕箻閸楃偟浠鹃梺鎶芥敱閸ㄥ潡寮诲☉妯锋婵鐗婇弫楣冩⒑閹惰姤鏁遍悽顖ょ節瀵鈽夊鍛澑闂佸搫鍟幑渚€鍩€椤掍礁顕滄い銊ｅ劦閹瑩骞撻幒鎾搭唲闂佸墽绮悧鐘诲蓟瀹ュ牜妾ㄩ梺鍛婃尵閸犳牕鐣峰ú顏勵潊闁靛牆鎳愰鎺楁煟閻樺弶澶勭憸鏉垮暣閹顢橀悜鍡樺瘜闂侀潧鐗嗗Λ娆撳煕閹烘鐓涢柛婊€绀佹禍婵堢磼閸屾氨效妤犵偛娲幃褔宕煎顓烆伜婵犵數鍋犻幓顏嗗緤閼测晜濯伴柨鏇炲€归崐鍧楁煟閹伴潧鍘靛ù婊勭矒閺岀喓鈧稒顭囩粻鏍ㄣ亜閵夛絽鐏い銊ｅ劦閹瑩寮堕幋鐘辩礉闁诲氦顫夊ú鏍偉婵傛悶鈧礁螖閸涱厾鍔﹀銈嗗笒鐎氼厾鈧?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵潙螖閳ь剚绂嶉幆褜娓婚柕鍫濈凹缁ㄥ鏌涢悢鍛婄稇闁伙絿鏌夐妵鎰板箳濠靛洦娅旈梻浣告啞娓氭宕归悧鍫熷弿婵炲樊浜濋埛鎺懨归敐鍕劅婵炲吋甯￠弻娑㈠即閻愬吀绮甸梺浼欑到婢у海妲愰幘瀛樺闁圭粯甯婃竟鏇炩攽閻橆喖鐏辨繛澶嬬⊕瀵板嫰宕堕鈧壕濠氭煙閹规劕鍚圭€规挷绶氶弻娑⑩€﹂幋婵囩亪婵犳鍠栨鎼佲€旈崘顔嘉ч煫鍥ㄦ礈缁愭姊洪崨濠庢當闁哥喎娼￠幃楣冩倻閽樺顓洪梺缁樺姌鐏忔瑧绮ｉ悙鐑樷拺鐟滅増甯掓禍浼存煕濮椻偓缁犳牕鐣烽敐澶婂耿婵＄偟绮弬鈧梻浣虹帛椤洨鍒掗姘ｆ鐟滃孩绌辨繝鍥舵晝闁挎繂娲﹂崳顓㈡倵閸偅绶查悗姘緲閻ｇ兘鎮㈢喊杈ㄦ櫇闂佺粯鏌ㄩ幉锟犲级閸涘﹣绻嗛柣鎰典簻閳ь剚鐗犻幃褍螖閸涱厺绱堕柟鍏肩暘閸斿瞼澹曟繝姘厪闊洤顑呴埀顒佺墵閹€斥槈濮橈絽浜鹃柛蹇擃槸娴滈箖鏌ｆ惔顖滅У闁告挻鐟╁畷鍝勭暆閸曨剛鍙勭紓鍌欑劍椤洦鏅堕悽鍛婄厸閻忕偟鏅暩濡炪伇鍌滅獢闁哄本鐩獮妯兼崉閻戞浜俊銈囧Х閸嬫盯宕幎鍓垮洤鐣烽崶鈺冿紲缂傚倷鐒﹂…鍥╃不閻愮鍋撶憴鍕闁绘牕鍚嬫穱濠囧箹娴ｈ倽銊╂煏韫囨洖小濠㈣娲熷娲礈閼碱剙甯ラ梺绋款儎閻掞妇绮嬪鍜佺叆闁割偆鍠撻崢鎼佹⒑閸涘﹤濮х紒鑼跺Г缁傚秹宕滆绾捐棄霉閿濆洦鐒块柛蹇撴湰閵囧嫰濮€閻欏懓鍚繝纰夌磿閺佸鐛崶顒€绾ч悹渚厛閸炴椽姊绘担绛嬫綈闁稿孩濞婂畷銏ゅ冀椤啫鍏炬棃宕ㄩ瑙勫濠电偠鎻徊浠嬪箟閿熺姴鐤柣鎰劋閻撴洟鏌￠崘锝呬壕闂佺粯顨堥崑銈嗕繆閻㈢绠涢柡澶庢硶椤︺劑姊洪崘鍙夋儓闁稿﹦顭堥锝夊锤濡や讲鎷洪悷婊呭鐢鏁嶉悢鍏肩厽闁硅櫣鍋熼悾鐢告煕閳哄倻娲存鐐差儔閺佸倻鎲撮敐鍡楊伖濠碉紕鍋戦崐鏍礉閹达箑纾归柡鍥╁枔椤╅攱绻濇繝鍌滃闁绘挾濮电换娑㈡嚑妫版繂娈梺璇查獜缁绘繈寮婚敓鐘插窛妞ゆ挾濮撮悡鐔奉渻閵堝啫鐏柣鐔濆嫮顩查柟闂寸閸愨偓闂侀潧顭梽鍕熆閹达附鈷掗柛灞剧懆閸忓本銇勯鐐靛ⅵ鐎殿喚鏁婚、妤呭磼濠婂懐鍘梻浣告贡閸庛倝銆冮崨瀛樺亗闁哄洢鍨洪悡娆撴煟閹寸倖鎴﹀煕閹烘梻妫柟顖嗗啯鍊梺闈涙搐鐎氭澘顕ｉ幘顔藉€烽柡澶嬪灦椤忕姵淇婇妶鍥ラ柛瀣仱閵嗗啴宕奸妷銉у姦濡炪倖宸婚崑鎾绘煟韫囨棁澹樻い顓炵仢铻ｉ悘蹇旂墪娴滅偓鎱ㄥ鍡椾簻鐎规挸妫濋弻锝呪槈閸楃偞鐝濋悗瑙勬礀閻栧ジ銆佸Δ浣哥窞閻庯綆鍋呴悵婊堟⒒閸屾瑧绐旈柍褜鍓涢崑娑㈡嚐椤栨稒娅犳い鏂款潟娴滄粓鏌￠崶鏈电敖闁告帊鍗抽弻锝夋晲韫囨洜鐦堥梺鍝勮嫰濞差參寮崒鐐村癄濠㈣泛鏈闂傚倸鍊搁崐椋庢濮橆剦鐒界憸鏃堝箖瑜斿畷鍗烆渻閵忥紕鈽夐柍钘夘槸閳诲酣骞囬鐘插▏濠碉紕鍋戦崐鏍暜閹烘纾归柤濮愬€曢ˉ姘攽閸屾碍鍟為柛瀣剁秮閺屾盯濡烽鑽ょ泿闂佹眹鍔嶅钘夘嚕椤愶箑閱囬柕澶涚畱閳?JSON闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖闂備礁鎲￠幐鍡涘川椤旂瓔鍟呴梻鍌欐祰椤曆呪偓娑掓櫊閹虫繃銈ｉ崗顖氭处鐎靛ジ寮堕幊绛圭畵閺屾盯寮撮妸銉т哗缂備胶濮垫繛濠囧蓟濞戙垹鐒洪柛鎰剁細濞岊亝绻濋姀锝庢綈婵炶尙鍠庨～蹇撁洪鍕獩婵犵數濮寸€涒晠鎮挎担鍦閻庢稒顭囬惌濠囨煟閺嶎偄甯舵い鏇悼閹风姴霉鐎ｎ偒娼旀繝纰樻閸ㄥ爼寮ㄩ柆宥忕稏鐎广儱鎳夐弨鑺ャ亜閺冨倻鎽傛繛鍫熸閺岋絽螖閳ь剙螞濞嗘挸绠為柕濞垮劗濡插牓鏌曡箛濠冾€嗛柟閿嬫そ閹鎲撮崟顒傤槬濠电偛鐪伴崝搴ㄥ极椤斿皷妲堟繛鍡楁捣缁犳艾顪冮妶鍡楀闁搞劌缍婂鍐测枎閹扳晙绨婚梺鍝勬祩娴滅偟绮旈閿亾鐟欏嫭绀€闁靛牏顭堥锝夊箻椤旇棄浜滈柣蹇撶箣缁€浣圭妤ｅ啯鐓熼柟杈剧到琚氶梺缁樻尰缁嬫垿鈥︾捄銊﹀磯濞撴凹鍨伴崜浼存⒑缂佹ê绗ч柛瀣尰缁岃鲸绻濋崶鑸垫櫇闂侀潧绻嗗Σ鍛涢敃鈧埞鎴︻敊绾攱鏁惧┑锛勫仩濡嫰鎮鹃悜绛嬫晝闁挎洍鍋撻崬顖炴⒑闂堟侗妾у┑鈥虫喘閸┾偓妞ゆ巻鍋撴い顓炲槻椤繐煤椤忓嫮顔囬柟鍏肩暘閸ㄥ綊藝閳哄倻绠鹃悗娑欘焽閻倕霉濠婂嫮鐭掓い?.autostart-off 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤濞硷繝寮婚悢琛″亾閻㈡鐒剧€涙繄绱撴担鐣屽牚闁稿﹥绻堝濠氭晝閳ь剝鐏掓繛鎾村嚬閸ㄩ亶鏁嶉崱妞绘斀闁绘劕寮堕埢鏇灻瑰鍕疄鐎殿喗褰冮…銊╁醇閻斿搫骞楅梻渚€鈧稑宓嗘繛浣冲嫭娅犳い鏇楀亾妤犵偞鐗犲鍫曞箣椤栨繂鎯堟繝娈垮枛閿曘儱顪冮挊澶屾殾闁靛濡囩弧鈧梺绋挎湰椤曟挳寮撮悢铏诡啎闁诲孩绋掗…鍥儗鐎ｎ喗鐓熸俊銈呭暙瀛濋梺浼欑悼閸忔﹢鐛€ｎ喗鏅濋柍褜鍓涚划缁樼節濮橆厼浠梺鎼炲劘閸斿瞼寰婃繝姘厽閹兼惌鍠栭獮鏍ㄣ亜椤忓嫬鏆ｅ┑鈥崇埣瀹曞崬螖閳ь剝銆栫紓鍌氬€搁崐鍝ョ矓閺夋嚦娑樜旈崟顓濈綍闂傚倷绀侀幉锟犲礉閿曞倸绐楁俊銈呮噹绾惧綊鏌ｉ姀鐘冲暈闁稿鍓濈换娑㈠幢濡ゅ啰顔囧銈呮禋娴滅偤鎯€椤忓牊鍊锋い鎺嶇閳ь剚鍔欓弻锛勪沪閸撗€濮囬梺璇″灡濡啯淇婇崼鏇炲耿婵炲棗绻戦柨顓犵磽閸屾艾鈧绮堟笟鈧畷鎰板捶椤撶喐娈伴梺璺ㄥ枔婵挳寮告笟鈧弻娑㈠箛闂堟稒鐏堢紒鐐劤閸氬鎹㈠☉銏犵闁绘劕妫欓崹鍧楀箖閻愵剙顕遍悗娑櫱氶幏缁樼箾鏉堝墽鎮兼い顓炵墦瀵娊鏌嗗鍡欏幍缂佺偓婢樺畷顒佺娴煎瓨鐓涘ù锝囶焾閺嗭綁鏌℃担鐟板鐎规洖鐖奸垾锕傚箣閻愮數鈼ユ繝鐢靛Х椤ｈ棄危閸涙潙鍨傞柣銏㈩焾绾惧鏌熼柇锕€鏋涙い銉﹁壘闇夐柣妯烘▕閸庢劙鏌ｉ幘瀛樼闁诡喗锕㈤幃娆撳箵閹哄棙瀵栭梻浣烘嚀閸熷潡骞婅箛娑樜﹂柛鏇ㄥ灡閺呮粌鈹戦崒姘暈妞ゅ骸绉瑰娲濞戞瑦鎮欓柣搴㈢煯閸楀啿顕ｉ锕€纾兼繝濠傚閻濅即姊洪懖鈹炬嫛闁稿繑鐩畷鐘诲冀椤愩倗锛濋梺绋挎湰閻熝呮嫻娴煎瓨鐓曢柣妯诲墯濞堟梹銇勯銏㈢缂佺粯绻傞～婵嬵敆閸屾艾袝濠碉紕鍋戦崐鏍暜婵犲洦鍤勯柛顐ｆ礀閻撯偓闂佸搫娲ㄩ崰鍡樼濠婂嫨浜滈柟鎹愭硾娴狅箓鏌涢悩绛硅€块柡灞炬礋瀹曞崬螖娴ｅ憡鐣婚梻浣告惈鐞氼偊宕濇惔銊ョ劦妞ゆ帒锕︾粔鐢告煕閹惧娲存い銏＄懇瀵挳濮€閳锯偓閹锋椽姊洪懡銈呮灈闁稿锕畷鐢告偄閸忚偐鍘辨繝鐢靛Т閸燁偊鎮橀妷锔轰簻妞ゆ劧绲跨粻姗€鏌熼悷鏉款伃鐎规洦鍋婂畷鐔碱敃閿濆骸鎮嬮梻鍌氬€搁崐椋庣矆娓氣偓楠炴牠顢曢妶鍥╃厯婵炴挻鍩冮崑鎾垛偓瑙勬磸閸ㄨ櫣绮嬮幒鏂哄亾閿濆骸浜滃ù婊勵殔閳规垿鎮欓弶鎴犱户闂佹悶鍔嶅浠嬪箖閿熺姴鍗抽柕蹇ョ磿閸橆亝绻濋姀锝呯厫缂佸鎹囧畷姘跺级濞嗗墽鍞甸悷婊冪Ч楠炲鏁撻悩鑼杽闂侀潧顭堥崕娆撴偄閻撳海顔囬柟鑹版彧缁插鍩€椤掍礁濮夌紒?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厾缁炬澘宕晶缁樹繆閺屻儰鎲鹃柡宀嬬秮閹垽宕妷褏鍘掑┑鐘媰閸曞灚鐣奸梺杞扮贰閸ｏ綁鐛幒鎳虫棃鍩€椤掑嫬鍨傞柛宀€鍋為悡鏇犫偓鍏夊亾闁逞屽墴瀹曟垿鎮欓悜妯轰簵闂佺粯妫冮弲鑼崲閸℃稒鐓忛柛顐ｇ箓閳ь剙鎲＄粋宥嗐偅閸愨斁鎷虹紓鍌欑劍閿曗晛鈻撻弮鍫熺叆闁哄洢鍔嬬花濠氭煟閿濆洤鍘撮柡浣瑰姍瀹曞ジ濮€閵忊剝绶┑鐘垫暩閸嬬偤宕归崼鏇熷仭鐟滄柨鐣烽姀銈呯缂備焦菤閹锋椽姊洪崨濠勨槈闁挎洩濡囩划濠氬箻缂佹ê鈧灚鎱ㄥΟ鐓庡付鐎殿噮鍠氶埀顒冾潐濞叉ɑ绻涙繝鍌滄殾闁绘垹鐡旈弫鍥煟濡⒈鏆滅紒鍗炲级娣囧﹪鎮欓鍕ㄥ亾閺嶎厼绀夐柟杈剧畱閸ㄥ倻绱撴担鑲℃垵鈻嶈箛娑欌拻闁稿本鐟чˇ锕傛煙绾板崬浜為柟顕呭櫍椤㈡洟鏁冮埀顒傚鐟欏嫮绡€闂傚牊渚楅崕鎴犫偓瑙勬尫缁舵岸寮婚垾鎰佸悑闁告劑鍔岄‖澶嬬節濞堝灝鐏￠柟鍛婂▕楠炲啫鐣￠幍铏€婚棅顐㈡处閹尖晜瀵奸崘顔解拺閺夌偞澹嗙拹鏉棵瑰搴濈盎妞ゎ偄绻掔槐鎺懳熺拠宸偓鎾绘⒑閼姐倕鏋戝鐟邦儔瀵鈻庨幘绮规嫼闁荤偞绋堥埀顒€鍘栨竟鏇㈡⒒娴ｅ憡鍟炴い銊ョ墦瀹曟劙鏁愰崨顏咁潔闂侀潧绻掓慨鐑剿囬妸銉富闁靛牆妫欓ˉ鍡涙煕鐎ｎ偄濮嶉柟?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ姀鐘差棌闁轰礁锕弻鈥愁吋鎼粹€崇缂備焦鍔栭〃鍡樼┍婵犲洤围闁告侗鍘藉▓鏌ユ⒑閸濆嫭顥炵紒顔肩У缁岃鲸绻濋崶顬囨煕濞戝崬骞楁繛鍫濈埣濮婂搫煤鐠囨彃绠洪梺鑽ゅ暱閺呯姴顕ｆ繝姘亜闁稿繒鍘ч埀顒傜帛娣囧﹪顢涢悙瀛樼彸缂備緡鍠栭悥鐓庮潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯婢х晫妲愰幒妤佸亹闁惧浚鍋勭喊宥夋⒑娴兼瑧绉柡鈧潏鈺冪处闁伙絽鐬奸惌娆撴偣閸ワ箑瀚庨柛鏂跨Ч婵＄敻宕熼姘辩潉闂佹悶鍎洪悘婵嬪箻鐎靛摜顔曢梺鍛婁緱閸樻儳鐡梻浣哥枃椤宕归崸妤€绠栭柍鍝勬噹閸ㄥ倹銇勯幇鍓佺ɑ闁伙箑楠搁埞鎴︽偐閹颁礁鏅遍梺鍝ュУ瀹€绋款嚕椤愶絿绡€婵﹩鍓涢悿鍥р攽閻樼粯娑фい鎴濇噽缁牓宕奸姀鈥虫瀾闂婎偄娲︾粙鎺楀磿濡ゅ懏鐓曢柍鈺佸暟閳洟鏌ｉ幘瀛樼闁逞屽墮閸樻粓宕戦幘缁樼厓鐟滄粓宕滈悢椋庢殾闁硅揪闄勯崐鐑芥煟閹寸伝顏呯濡も偓閳规垿鎮欓崣澶樻！闂佸湱顭堥…鐑藉箖閳ユ枼鏋庨柟鎵虫櫃缁ㄥ姊洪崫鍕犻柛鏂块叄閸╁﹪寮撮悩鐢碉紲闂佽鍨卞姗€鎯冨ú顏呯厵妞ゆ洖鎳庨弸娑欍亜椤愶絿绠為柟铏矌閸犲﹤螣鐠囧樊浼?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ礂閻撳簶鍋撶紒妯圭箚妞ゆ牗绻冮鐘绘煕閺冨偆鐒鹃柍瑙勫灴閹瑩鎳犻浣烘闂備礁鎼幊鎰箾閳ь剛鈧鍠栭…宄邦嚕閹绢喗鍋勯柧蹇氼嚃閸熷酣姊绘担铏瑰笡闁告棑绠撳畷婊冾潩閼搁潧浠ч梺鍝勫暙閻楀﹪鍩涢幋锔界厱婵犻潧妫楅鎾煕鎼粹剝鎹ｇ紒杈ㄥ浮閹虫粓妫冨☉妯绘嚈闁诲孩顔栭崰鏍€﹀畡閭﹀殨闁圭虎鍠栭～鍛存煟濡鍤欓柡浣筋潐缁绘繈鎮介棃娴躲垽鏌ｈ箛鏂垮摵鐎规洘绻堝浠嬵敃閵堝棗娅樼紓鍌氬€搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌ら幖浣规锭闁搞劍妫冮幃妤呮濞戞瑦鍠愮紓浣插亾濠电姴浼呰ぐ鎺撴櫜闁告侗鍙庡Λ灞解攽閻愭彃鎮戦柣妤侇殘閹广垹鈽夊鍡楁櫊濡炪倖妫佸畷鐢告儎鎼淬劍鈷戦柟鑲╁仜婵″ジ鏌涙繝鍌滅Ш濠碉紕鏁诲畷鐔碱敍濮橆剙绁梻浣虹帛閸旀牕顭囧▎蹇婃瀺闁挎繂顦伴埛鎴︽煕濞戞﹫鏀婚悘蹇ｅ弮閺屾盯鎮㈤柨瀣淮闂佺硶鏅濋崑銈夌嵁鐎ｎ喗鏅滅紓浣股戝▍鎾剁磽娴ｉ缚妾搁柛姗€绠栧缁樺緞閹邦剝袝濡炪倖鍔戦崐妤冨閽樺褰掓晲閸涱喗鍎撻柡宥忕節濮婅櫣绱掑鍡樼暭婵犳鍠楅幐鎶姐€佸鑸垫櫜闁搞儯鍔岄悵鏉库攽閻愬瓨缍戞い鎴濇閿濈偛顓奸崨顏呮杸濡炪倖姊婚悺鏂库枔閺冣偓娣囧﹪顢曢敐鍥ㄥ櫚閻庤娲橀幑鍥箖娴犲宸濆┑鐐靛亾鐎氳偐绱撻崒姘偓鐑芥倿閿曚焦鎳岄梺璇茬箰濞存岸宕㈤悾灞惧床婵炴垯鍨圭粻缁樸亜閺冨洤袚闁哄棛濮风槐鎾存媴缁嬪簱鍋撻崷顓熸殰闁跨喓濮伴埀顑跨閳藉濮€閳ユ枼鍋撴搴ｇ＜妞ゆ梹顑欏鎰版倵濮橆兙鍋㈡慨濠冩そ瀹曨偊宕熼崹顐嶃劑姊虹粙娆惧剱闁挎洏鍨归悾鐑芥晸閻樿尙顦ㄩ悗鐟板閸犳岸鍩€椤掆偓閻栧ジ寮婚敐澶婄闁挎繂鎲涢幘缁樼厽闁规儳鐡ㄧ粈鈧梺瀹狀潐閸ㄥ潡骞冮埡鍛闁圭儤鎸婚宥呪攽閻橆偅濯伴悘鐐跺亹閸戝綊姊洪棃娑欐悙閻庢矮鍗抽妴浣割潨閳ь剟骞冨▎鎾崇骇闁规惌鍘借ⅸ缂傚倸鍊搁崐鐑芥嚄閸撲礁鍨濇い鏍ㄧ矋瀹曟煡鏌涘畝鈧崕鎴犵礊閺嶃劎绡€闂傚牊渚楅崕娑㈡煛娴ｅ憡顥㈤柡灞剧〒娴狅箓骞嗚閸斾即姊虹粙娆惧剱闁圭懓娲獮鍐ㄢ枎閹炬潙鈧粯淇婇鐐存暠閻庢艾銈稿濠氬磼濮橆兘鍋撻幖浣瑰亱濠电姴鍟伴悷瑙勪繆閵堝倸浜鹃柛妤呬憾閺屾盯顢曢敐鍡欘槰闂佺粯鎸诲ú鐔煎蓟閺囩喓绡€閹艰揪绲界敮銉モ攽閻愭彃鎮戦柛鏃€顨堝Σ鎰板箳濡も偓绾惧吋鎱ㄥΟ鑽ゆ▊鐟滃酣銆冮妷鈺傚€烽柛娆忣樈濡箓姊洪悙钘夊姕闁绘濞€瀹曞綊骞嗚閺嗭箓鏌涢妷銏℃殨闁靛ň鏅滈埛鎴﹀级閻愭潙顥嬮柛鏂跨Ч閺屾盯寮埀顒勬偋閺囥埄鏁嬮柕澶嗘櫅缁€瀣亜閹邦剦鍎戦柛鐘叉捣閸欏懐绱撻崒娆戝妽闁挎洍鏅涜灋婵犻潧鐗忕壕?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟稒宸濈紒鈾€鍋撻梻浣侯焾閺堫剛鍒掑畝鍕┾偓鍌炴偨閸涘ň鎷洪梻渚囧亞閸嬫盯鎳熼娑欐珷闁告挆鍛紲闂佺粯顭堝▍鏇犱焊椤撱垺鐓熼柨婵嗩槹閺佽京绱掗搹瑙勬珪缂侇喗鐟ラ埢搴ㄦ倷椤掑倻袩闂傚倸鍊烽懗鍫曞磻閵娾晛纾块梺顒€绉撮悿顕€鎮归幁鎺戝濠殿喗濞婇弻鏇熷緞濞戞﹩娲紓浣叉閸嬫捇姊绘担鍦菇闁搞劏妫勯…鍥槼缂佸倹甯￠弫鍐磼濞戞艾骞堟俊鐐€ら崢浠嬪垂閻㈠憡鍊堕柨鏃堟暜閸嬫挾鎲撮崟顒傤槶闂佺娅曢幐楣冨矗閸涱収娓婚柕鍫濇噽缁犱即鎮楀鐓庢珝闁轰礁绉归幃銏ゅ礂鐏忔牗瀚奸梻浣藉吹閸犳劕顭垮鈧崺娑㈠籍閸屾粎锛滃銈嗘礀閹冲酣鎮橀敃鍌涚厵?-> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶鏉款熆鐟欏嫭绀冪紒缁樼箓椤繈顢橀悩鎻掔疄闂傚倷鐒﹂弸濂稿疾濞戙垹鐤ù鍏兼綑閻撯€愁熆閼搁潧濮堥柣鎾存礋閹﹢鎮欐０婵嗘婵犳鍠栭崐鍧楀蓟閿涘嫪娌悹鍥ㄧゴ閸嬫捇寮撮姀锛勭暢闂備浇顕ч崙鐣岀礊閸℃稑纾婚柛娑樼摠閸嬬喖鏌￠崶鈺佸壉婵炴挸顭烽弻鏇㈠醇濠靛鈧霉濠婂嫮绠栫紒缁樼洴閹崇娀顢楅埀顒勫几濞戞埃鍋撳▓鍨灈妞ゎ厾鍏橀獮鍐閵堝懎绐涙繝鐢靛Т鐎氼喛鍊撮梻鍌氬€风粈渚€骞夐敓鐘茬闁糕剝绋戝婵囥亜閺冨倻鎽傞柛鐔锋嚇濮?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厱闁归偊鍨扮槐锕傛煟閵忋垻甯涘ǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘介崑瀣繆閵堝懎鏆熼柣顓熺懇閺岀喖鎮欓鍌涜弴闂侀€炲苯澧柟铏悾鐑芥晲閸℃绐為悗鍏夊亾闁逞屽墴閹線宕奸悢铏圭槇闂佹眹鍨藉褍鐡梻浣侯焾閿曘儵骞冮崒姘煎殨闁归棿鐒﹂弲婵嬫煕鐏炲墽銆掗柛姗€绠栧铏圭磼濮楀棙鐣堕梺缁橆殔濡繈銆佸顒夌叆闁割偆鍠撻崢顏堟⒑閹肩偛鍔€闁告劕褰炵槐鎾剁磽閸屾瑨鍏屽┑顔炬暬閹嫰顢涢悙闈涚ウ濠碘槅鍨伴崥瀣暦婢跺鈧帒顫濋悡搴ｄ画濠殿噯绲藉ú顓烆潖缂佹ɑ濯撮柦妯侯槸閹偤姊洪崫銉バｉ柛鏃€鐟╅獮鍐倻閼恒儳鍔堕悗骞垮劚濡矂骞愰崘顔解拺闁告稑锕ユ径鍕煕閵娧冾暭妞ゃ垹锕缁樻媴閸涘﹤鏆堝┑鐐额嚋缁犳挸鐣风憴鍕嚤闁哄鍨归ˇ銊╂⒑闁偛鑻晶瀛樻叏婵犲啯銇濇俊顐㈠暙閳藉娼忛埡浣感梻鍌欑閹诧繝寮婚妸鈺傚剹闁稿本绻冮敍鍌涚節閻㈤潧鈻堟繛浣冲浂鏁勯柛鈩冪☉绾惧鏌ｅΟ鎸庣彧闁告瑥绻戞穱濠囶敍濮橆厽鍎撻梺鑲╊焾缂嶅﹪寮婚敍鍕ㄥ亾閿濆骸浜為柍顖涙礋閺岀喐绗熼崹顔碱瀴缂備胶绮换鍫濈暦濮椻偓閸╃偞寰勫灞绢€楅梻浣筋嚙濮橈箓锝炴径濞掓椽鏁冮崒姘憋紱婵犵數濮撮崐濠氬汲閿曞倹鐓熼柡鍐ㄥ€甸幏锟犳煛娴ｇ鏆ｉ柡灞诲妼閳规垿宕卞鍫㈢闂備礁鎽滈崰搴ㄦ偋閸℃稑鐓橀柟杈鹃檮閸嬫劙鏌涢…鎴濅簼婵絽瀚板铏圭矙濞嗘儳鍓遍梺鐑╂櫓閸ㄩ潧螞閻斿吋鈷戦悷娆忓閸熷繘鏌涢悩鍐插摵闁诡垰鑻埢搴ㄥ箻鐎电骞嶉梻浣虹帛閸ㄦ儼鎽梺鎶芥敱鐢繝寮婚敐澶樻晣闁绘垵妫欓埢鍫ユ⒑閸濆嫮鐏遍柛鐘崇墵楠炲啫顭ㄩ崗鍓у枛閹煎綊鏌呭☉娆忔灓婵犵數濮烽。浠嬪礈濠靛浜归柛鎰靛枟閸嬪鈹戦悩鎻掝仾鐎规洖寮剁换娑㈠箣濞嗗繒浠煎Δ鐘靛亼閸ㄧ儤绌辨繝鍥舵晬婵犻潧娴傛禒鈺傜箾鐎涙鐭婇柣鏍帶椤繐煤椤忓嫪绱堕梺鍛婃礋濞佳囧礈闁秵鈷戠紓浣癸供濞堟ê鈹戦鑺ュ唉闁挎繄鍋犵粻娑㈠箻娴ｈ銇濇い銏℃瀹曘劑顢涘顑儵姊婚崒娆掑厡缂侇噮鍨跺畷婵單熼梻瀵稿墾濡炪倕绻愬Λ宀勫磻閹炬剚娼╂い鎾跺枔閸戔€愁渻閵堝骸寮ㄩ柛搴☆煼绡撳〒姘ｅ亾闁哄本鐩弫鎰疀閺囩姌婊堟倵濞堝灝鏋︽い鏇嗗浂鏁囧┑鍌滎焾閻愬﹪鏌曟繛鍨姢鐟滅増鎸冲缁樻媴閸涘﹥鍎撻柣鐐村嚬閸嬪﹤鐣烽弴銏犵闁绘垵妫楀▓銊︾箾鐎电甯堕柣掳鍔戦崺娑㈠箣閻樼數锛滃銈嗙墬缁嬫帞绮堥崘顔界厸濠㈣泛顑嗛妵婵嬫煛鐏炲墽鈯曢柟顖涙婵℃悂鏁傛禒瀣殬闂傚倷妞掔槐顔惧緤閸ф绀傛俊顖濐唺缁诲棝鏌熼梻瀵割槮闁绘挻绋戦埞鎴︽偐閹绘帗娈插┑鐐叉噺閻楁鎹㈠┑瀣仺闂傚牊鍒€閵忋倖鐓ラ柡鍥埀顒侇殜閸┾偓妞ゆ帊鑳堕埢鎾绘煛閸涱喚娲寸€殿喗濞婇崺锟犲磼濠婂懐妲囬梻浣圭湽閸ㄨ棄顭囪閺嗏晠姊绘担鑺ャ€冮柣鎺炵畵楠炲啴宕掑杈ㄦ闂佺鎻梽鍕磹閻戣姤鐓曟繝闈涙椤忊晠鏌ｆ惔顔煎籍婵﹥妞藉畷婊堝箵閹哄秶鍑规繝鐢靛仜瀵爼鎮у鍛灊缂備焦锚椤曢亶鎮楀☉娆樼劷闁告ê宕埞鎴︽偐缂佹ɑ閿柣搴㈠嚬閸撶喎鐣峰┑瀣у璺侯儑閸樿棄顪冮妶鍡樺暗闁哥姵鍔曢悾宄扮暆閸曨剛鍘搁悗鍏夊亾閻庯綆鍓涜ⅵ濠电姵顔栭崰鎺楀磻閹剧粯鈷戦梻鍫熺〒缁犳岸鏌￠崨顔剧疄鐎规洘绻勭划娆愭償閹惧瓨鏉搁梻浣哥枃濡嫬螞濡や胶顩叉繝闈涱儐閻撱儲鎱ㄥΟ鍨厫妞ゃ儱妫楅…鍧楁嚋闂堟稑顫嶉梺缁樻尪閸庤尙鎹㈠┑瀣棃婵炴垶鐟Λ銈嗙節閳封偓閸℃稑鎽电紓浣虹帛閻╊垰鐣峰Δ鍛亗閹肩补妲呭鎾绘⒒閸屾碍鎼愭い锔藉閹广垹鈹戞繝搴⑿╅梻浣筋嚙缁绘垹鎹㈤崼銉ユ槬闁逞屽墯閵囧嫰骞掑鍫濆帯婵犫拃鍥︽喚闁哄瞼鍠栭悰顕€宕归鍙ユ偅闂備礁鐤囧Λ鍕囬崹顐ｅ弿闁逞屽墴閺岋絽螣濞茶鏅遍梺鍛婅壘閹虫ê顫忕紒妯诲闁惧繒鎳撶粭锟犳⒑閹肩偛濮傛繛浣冲棛浜辨繝鐢靛仦閸垶宕归崷顓犵焼濠㈣埖鍔栭悡鐔兼煙鏉堝墽鍒扮悮姘舵⒑閹惰姤鏁遍柛銊ユ健瀵鈽夊Ο閿嬫杸闂佺硶鍓濋〃蹇旂閹屾富闁靛牆鍟崝婊堟煟閻斿弶娅婄€殿喛顕ч埥澶娢熷鍕拹闁瑰嘲鎳愰崠鏍即閻旇桨绨介梻浣筋嚙鐎涒晠顢欓弽顓炵獥闁哄稁鍘搁埀顒婄畵閹粓鎸婃径瀣偓顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幘缂佺偓婢樺畷顒佹櫠缂佹ü绻嗛柤纰卞墮椤ｇ厧菐閸パ嶈含闁诡喗鐟╅、鏃堝礋閵娿儰澹曢梺鍝勭▉閸樿偐绮婚弶璇炬棃鏁愰崨顓熸缂備焦鍔栭〃濠囧蓟閻旂厧绠查柟浼存涧濞堫參寮堕埡鍌滅疄婵﹪缂氶妵鎰板箳閹存粌鏋堥梻浣侯焾缁绘垿鏁冮姀鐙€鍤曢柕濠忓椤╃兘鎮楅敐搴′簽闁告ü绮欏楦裤亹閹烘垳鍠婇梺鍛婃尰閻熝囨儉椤忓浂妯勯梺鍝勬湰閻╊垶寮幇鏉垮耿婵☆垶鏅查搹搴ㄦ⒒娴ｇ瓔鍤冮柛鐘虫礈閸掓帒鈻庨幘鎵佸亾娴ｇ硶妲堥柕蹇曞瑜旈弻娑㈠Ψ閵忊剝鐝栧┑鈩冨絻濞差參寮婚敐鍡樺劅闁靛繆鏅涢弲閬嶆⒑闂堚晝绉甸柛锝忕到閻ｇ兘骞嬮敃鈧粈瀣亜閺嶇數绋婚柡鍜冪稻缁绘繈妫冨☉妯峰亾婵犳埃鈧箓宕煎┑鎰櫊濠电娀娼уú銏＄濠婂牊鐓涚€广儱鍟俊鍧楁煃椤栨稒绀嬮柡宀嬬秮閺佹劙宕惰楠炲鈹戦纭峰姛缂侇噮鍨堕獮蹇涘川閺夋垵绐涙繝鐢靛Т閸婄懓鈻撳Ο鑽ょ瘈闁汇垽娼ф禒鈺傘亜閺囩喓鐭掓慨濠佺矙瀹曠喖顢橀悩鐢靛姽闁诲骸绠嶉崕閬嵥囨导鏉戠９闁绘垼濮ら悡鏇熴亜椤撶喎鐏ュù婊嗗Г缁绘盯鎳栭埡浣圭€婚梺瀹狀潐閸ㄥ潡銆佸▎鎴犻┏閻庯綆鍓欐慨濂告⒒娴ｈ櫣銆婇柡鍛矒閹囨偐鐠囧弶妲┑鐐村灟閸ㄥ湱绮婚敐澶嬬厽婵☆垰鍚嬮弳鈺呮煥濞戞瑧鐭掗柡宀嬬稻閹棃顢涘鍛咃綁姊洪崨濠庣劷闁哄拋鍋勯埥澶愭偨缁嬭法顦繛杈剧秬濞咃綁鏁嶅鍫熲拺缂備焦鈼ら鍫濈柈闁秆勵檪婢跺备鍋撻敐搴″缁炬儳銈稿鍫曞醇濞戞ê顬堢紓浣诡殕濮樸劑鍩€椤掆偓閻忔艾顭垮Ο灏栧亾濮樼厧娅嶉柛鈹惧亾濡炪倖甯掗敃锔剧矓閻㈠憡鐓曢悗锝庝簻椤忣厾鈧娲﹂崜姘嚗閸曨剛绠鹃柕蹇婂墲椤ュ牓鏌熼瑙勬珚婵☆偄鍟埥澶娢旈崘鈺佺缂傚倸鍊搁崐鐑芥嚄閸撲礁鍨濇い鏍仜閸ㄥ倻鎲搁悧鍫濈瑨闁绘帒鐏氶妵鍕箣閿濆棛銆婄紓浣疯兌閸忔﹢寮诲☉姘ｅ亾閿濆骸浜滃┑顔肩Ч閺屾盯鍩為幆褌澹曞┑锛勫亼閸婃牜鏁繝鍥ㄥ殑闁割偅娲栭悡鈧梺鍝勬储閸ㄦ椽鎮￠妷锔藉弿婵妫楅崝鎺旀喐閻楀牆绗氶柛瀣姍閺屟嗙疀閿濆懍绨存繛瀵稿О閸ㄥ綊鍩為幋锔绘晩缁绢參鏀遍弫鎯р攽閿涘嫬浠╂俊顐㈠閻忓姊洪崨濠傚闁告柨顦嵄闁割偁鍎辩痪褔鏌涢锝囶暡婵炲懎鎳愰幉鎼佸级濞嗘儳鍓崇紓浣介哺閹瑰洤鐣烽幒鎴旀瀻闁圭儤鍨电敮顖氣攽閻樻剚鍟忛柛鐘冲哺瀵偆鎷犻懠顒佹閻熸粎澧楃敮鎺楁煥閵堝棔绻嗛柕鍫濆€告禍鍓х磽娴ｅ搫校闁绘濞€瀵濡搁埡鍌氫簻闂佸憡绻傜€氬懘濮€閵堝棛鍘卞┑掳鍊撶粈渚€鍩㈤弴銏＄厸閻忕偟鍋撶粈澶岀磼閻樺磭娲村┑锛勬焿椤︽挳鏌ㄥ☉姘灈婵﹥妞藉畷銊︾節鎼达絽濮洪梻浣芥〃濞村洭顢氳閿濈偠绠涢弮鍌滅槇濠殿喗锕╅崜娑㈡晬濠婂啠鏀芥い鏃傜叓椤忓嫮鏆︽い鎺戝▊閻戣棄鐒垫い鎺嗗亾闁宠鍨块幃娆戔偓娑櫭棄宥囩磼閻愵剚绶茬紒澶婄秺婵℃挳宕橀埡鍐槇闂佺鏈划宥呪枔妤ｅ啯鈷戦柛婵嗗缁侇偆绱掓潏銊︾闁诡喖娼￠崺鈧い鎺戝閳锋垿鏌涢敂璇插绩婵＄偓鎮傞弻娑㈠Ω閳衡偓閹插墽鈧鍣崜鐔煎春閳ь剚銇勯幒宥嗩樂缂佽妫濋弻锝夊箛閸忓摜鐩庨梺閫炲苯澧柣妤佹礉閻忓绻涢幘鏉戠劰闁稿鎹囬弻銊モ攽閸繀娌悗鍨緲鐎氼厾鎹㈠☉銏″€锋い鎺戝€甸崑鎾趁洪鍛嫼缂備緡鍨卞ú鏍ㄦ櫠閸欏浜滄い鎾跺仦瀹告繄绱掗鍓у笡闁靛牞缍佸畷姗€鍩￠崘銊ョ闂傚倷娴囬～澶愬磿閹惰棄鐤柛褎顨嗛崕濠囨煏婵炵偓娅嗛柣鎾跺枛閺岀喖宕滆娴犳椽鏌涚€ｃ劌鐏查柡灞糕偓宕囨殕闁逞屽墴瀹曚即寮介婧惧亾娴ｇ硶鏋庨柟鐐綑娴犲ジ鏌ｈ箛鏇炰沪闁稿骸鍟块埢宥咁潩閼哥數鍘介柟鍏肩暘閸╁嫰宕箛娑欑厱闁绘ê纾晶鐢告煙椤斿搫鍔滅紒杞扮矙瀹曨偊濡烽妷褋鍋婇梻浣筋嚙閸戠晫绱為崱妯碱洸闁哄稁鍘肩紒鈺伱归悩宸剱闁绘挻鐟╁娲敇閵娧呮殸闁汇埄鍨崝鎴﹀蓟瀹ュ浼犻柛鏇ㄥ墮濞呫倝姊洪幎鑺ユ暠閻㈩垽绻濆璇测槈濮橆偅鍕冮梺鍛婃寙閸曨偅鐝梻鍌欒兌椤牏鎹㈤幇鐗堝剹闁稿瞼鍋涢拑鐔兼煟閺傚灝鎮戦柟顖滃仦閵囧嫰骞囬崜浣稿煂缂佸墽鍋撴繛濠傤潖閾忚瀚氶柍銉ㄦ珪閻忓秹姊洪幐搴ｎ暡闁靛洤瀚伴、鏇㈡晲閸モ晝鏆梻浣告贡閹虫挾鈧矮鍗抽獮鏍捶椤撶喎鐝伴梺鐐藉劥濞呮洟寮抽鍕ㄦ斀闁绘ê鐏氶弳鈺佲攽椤旀儳鍘寸€殿噮鍋婂畷銊︾節閸愩劌浼庨梻浣圭湽閸ㄨ棄顭囪瀵悂宕奸埗鈺佷壕妤犵偛鐏濋崝姘亜閿旇鐏ｅ瑙勬礉閵囨劙骞掗幘璺哄箞婵＄偑鍊栭崝褏寰婇崜褏鐭嗛柛鎰靛枟閻撶喖鏌熼幆褍鑸归柛鏃撶畵閺屸€崇暆鐎ｎ剛鐦堥悗瑙勬礀閻栧ジ宕洪埄鍐╁濡炲瀛╁畷鐔兼⒒閸屾瑧绐旈柍褜鍓涢崑娑㈡嚐椤栨稒娅犳い鏍仦閻撶喖鏌嶉埡浣告殲缂佲偓閸儲鐓?
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

// ---------------------------------------------------------------- WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴椤㈡洟鏁愰崱娆欑穿闂備線鈧偛鑻晶鍓х磼閻樿櫕灏柣锝夋敱缁虹晫绮欑拠淇卞妽閵囧嫰寮崶顬挻绻涢崨顓犲ⅵ婵﹦绮幏鍛村川婵犲倹娈橀梻浣告啞濡垹绮婚幘宕囨殾濞村吋娼欓崘鈧銈嗗姂閸╁嫬螞濠婂牊鈷戠紒澶婃鐎氬嘲鈻撻弴銏＄厓闂佸灝顑呴悘锕傛煏閸パ冾伃妤犵偞甯″畷鍗烆渻閹屾缂傚倸鍊搁崐椋庣矆娓氣偓钘濇い鏍剱閺佸銇勯幘璺盒㈤柛銊︾箓閳规垿鎮╃€圭姴顥濋梺鍛婂姀閸嬫挻淇婇悙顏勨偓鏍礉濡ゅ懎绐楅幖娣灮椤╂煡鏌ｉ幇闈涘⒒婵炲牅绮欓弻锝夊箛椤栨氨姣㈡繛瀵稿О閸ㄤ粙寮诲☉銏犵睄闁逞屽墰閸犲﹤顓兼径濞箓鏌涢弴銊ョ仭闁哄懏绮撻幃褰掑炊椤忓嫮姣㈡繛瀵稿Л閺呮盯鈥旈崘顔嘉ч柛鈩冾焽閸欏棛绱撴担铏瑰笡闁告梹鐟╁畷娲倷閸濆嫮顓洪梺鎸庢磵閸嬫挻顨ラ悙顏勭伈闁绘搩鍋婂畷鍫曞Ω閿旇瀚婚梻浣告惈椤︿即骞冮崒姘兼綎缂備焦蓱婵挳鏌ц箛鏇熷殌缂佹绻濆娲箰鎼淬垻娈ら梺閫炲苯澧い鏃€鐗犲畷鏉库槈閵忕姵妲梺鍝勭▉閸樿偐绮堥崱妞绘斀闁绘ɑ褰冮弳鐔兼⒒閸曨偄顏柡灞剧☉閳藉顫滈崼婵嗩潬闂?
// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊鐒﹂崕鎾绘⒑閹肩偛濡奸柛濠傛健瀵鈽夐姀鈺傛櫇闂佹寧绻傚Λ娑⑺囬妷褏纾藉ù锝呮惈灏忛梺鍛婎殕婵炲﹤顕ｇ拠娴嬫闁靛繆鏅滈弲婵嬫⒑閹稿海绠撴俊顐ｇ洴钘濋柕澹懐锛滈梺缁樺姦閸撴瑧绮堥崘鈹夸簻妞ゆ劑鍩勫Σ鍦磼瀹€鍐摵缂佺粯绻堝畷鍫曗€栭顒€娲﹂悡鏇熺箾閹存繂鑸归柡瀣洴閺岋絽鈹戦崶銊㈡嫽缂?WARP 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊鐒﹂崕鎾绘⒑閹肩偛濡奸柛濠傛健瀵鈽夐姀鈺傛櫇闂佹寧绻傚Λ娑⑺囬妷褏纾藉ù锝呮惈灏忛梺鍛婎殕婵炲﹤顕ｇ拠娴嬫婵☆垶鏀遍弬鈧梻浣告啞濞诧箓宕戦崟顒佸弿闁靛繈鍊栭埛鎴炵箾閼奸鍤欐鐐搭殜閺岀喖鎮烽悧鍫濇灎閻庢鍠栭…鐑藉极閹邦厼绶炲┑鐘插閸氬懘姊绘担鐟邦嚋缂佽鍊歌灋妞ゆ挾鍊ｅ☉銏犵妞ゆ挾濮烽敍婵囩箾鏉堝墽绋荤憸鏉垮暞缁傚秵銈ｉ崘鈹炬嫼缂備礁顑嗙€笛冿耿閹殿喚纾奸悗锝庡亜濞搭喚鈧娲╃紞鈧紒鐘崇洴瀵剟宕滆閻ｉ箖姊绘担铏瑰笡闁告梹鐟╄矾闁稿瞼鍋涢崥瑙勭箾閸℃璐╅柣鐔煎亰閻撱儵鏌涢鐘茬伄闁哄棭鍋勯埞?闂?
//   1) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴楠炴﹢鎳犻澶嬓滈梻浣规偠閸斿秶鎹㈤崘顔嘉﹂柛鏇ㄥ灠閸愨偓濡炪倖鍔﹀鈧慨瑙勵殜濮婃椽鏌呴悙鑼跺濠⒀冨⒔缁辨帡鎮╅搹顐㈢闂佷紮缍侀弨杈╃紦閻ｅ瞼鐭欓悹鍥﹀嫎閸旀垿寮婚弴鐔风窞婵炴垯鍨洪宥夋偠濮樺墽绉慨濠傤煼瀹曟帒鈻庨幋顓熜滈梻浣告贡閳峰牓宕戦崱娆忓灊婵炲棙鍔楅悷褰掓煃瑜滈崜娆撴偩閻戣棄顫呴柨娑樺濡绢噣姊洪崨濠勨槈闁挎洏鍊濋崺鈧い鎺嗗亾婵炵》绻濆璇测槈閳垛斁鍋撻敃鍌氱婵犻潧鎳愰幐澶娾攽閻愯尙鎽犵紒顔肩Ф閸掓帡骞樼拠鑼舵憰闂佺粯妫侀妴鈧柛瀣崌閹棄鈻撶捄銊ュЪ濠电偛顕慨鐢靛垝瀹ュ桅闁告洦鍨伴崡鎶芥煟閺冨洦顏犻柣鎾茬窔濮婃椽妫冨☉娆愭倷闁诲孩鐭崡鍐差嚕椤愶箑绀冩い鏃囧亹閿涙粓姊洪棃娑氬婵☆偒鍘艰灋闁哄稁鍋嗙壕?connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厱闁归偊鍨扮槐锕傛煟閵忕媭鐓兼慨濠勭帛閹峰懘鎮烽柇锕€娈濇繝鐢靛仜瀵爼宕归崸妤€绠栭柣鎰劋閺呮繈鏌涚仦鐐殤闁汇倕娲幃妤呭礂婢跺﹣澹曢梻浣告啞濞诧箓宕楀鈧獮?disconnected      -> 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗鎸抽幃娆徝圭€ｎ亙澹曢梺褰掓？閻掞妇鈧艾鎳橀弻锝夊棘閸喗鍊梺缁樻尭缁绘劙鍩為幋锔藉€烽梻鍫熺◥婢规洖鈹戦悙鍙夊櫤闁圭懓娲濠氬焺閸愨晛顎撶紓浣割儐椤戞瑦瀵奸崘顔解拺闁告繂瀚﹢鎵磼鐎ｎ偄鐏撮柛鈹垮劜瀵板嫭绻涢悙顒傗偓濠氭⒑瑜版帒浜伴柛鐘冲哺閸┿垼绠涘☉娆屾嫽婵炶揪绲介幉锟犲箚閸儲鍊垫慨妯稿劚婵倻鈧娲樺ú鐔煎箖閻ｅ瞼鐭欓柤鎰佸灡閹蹭即姊绘担鍛婃儓婵炴潙瀚Σ鎰板即閵忊€充痪闂侀€炲苯澧撮柡宀嬬稻閹棃顢涘鍛咃絾绻涚€涙鐭嬮柣妤冨Т閻ｇ兘骞嬮敃鈧粻濠氭偣閸ャ劌绲婚柛鐐妼椤啴濡堕崱妯烘殫闂佸摜濮撮柊锝夊春閳ь剚銇勯幒鎴濇殲缁绢厼澧庣槐鎺撴綇閵婏箑闉嶉梺鐟板槻閹虫﹢骞婇弽顓炵労闁告劑鍔庢导宀勬倵鐟欏嫭绌跨紓宥佸亾缂備胶濮甸惄顖氼嚕椤掑嫬绀堢憸蹇涙偩閻愵兛绻嗛柣鎰典簻閳ь剚鐗犻幃褍螖閸涱厺绱堕柟鍏肩暘閸ㄥ銆呴悜鑺ョ厵闁绘垶锕╁▓鏇㈡煟閹惧瓨绀冮柟渚垮妼铻ｉ柣鎾抽娴犳ɑ绻涚€涙鐭嗛柛妤佸▕瀵鏁嶉崟顏呭媰闂佸憡鎸嗛埀顒€危閸儲鈷戦悗鍦閸ゆ瑦绻涢幘顔芥暠缂佸倹甯￠弫鍐磼濮橀硸鍚呴梻浣告贡閸庛倝宕洪崼婢?
//   2) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴楠炴﹢鎳犻澶嬓滈梻浣规偠閸斿秶鎹㈤崘顔嘉﹂柛鏇ㄥ灠閸愨偓濡炪倖鍔﹀鈧慨瑙勵殜濮婃椽鏌呴悙鑼跺濠⒀冨⒔缁辨帡鎮╅搹顐㈢闂佷紮缍侀弨杈╃紦閻ｅ瞼鐭欓悹鍥﹀嫎閸旀垿寮婚弴鐔风窞婵炴垯鍨洪宥夋偠濮樺墽绉慨濠傤煼瀹曟帒鈻庨幋顓熜滈梻浣告贡閳峰牓宕戦崱娆忓灊婵炲棙鍔楅悷褰掓煃瑜滈崜娆撴偩閻戣棄顫呴柨娑樺濡绢噣姊洪崨濠勨槈闁挎洏鍊濋崺鈧い鎺嗗亾婵炵》绻濆璇测槈閳垛斁鍋撻敃鍌氱婵犻潧鎳愰幐澶娾攽閻愯尙鎽犵紒顔肩Ф閸掓帡骞樼拠鑼舵憰闂佺粯妫侀妴鈧柛瀣崌閹棄鈻撶捄銊ュЪ濠电偛顕慨鐢靛垝瀹ュ桅闁告洦鍨伴崡鎶芥煟閺冨洦顏犻柣鎾冲€荤槐鎾存媴娴犲鎽甸柣銏╁灡椤ㄥ﹪鐛箛娑樺窛闁哄鍨崇槐鍫曟⒑閸涘﹥绀€闁诲繑鑹捐濠㈣埖鍔栭埛?connected 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐鍛傜喎鈻庨幆褎顔勭紓鍌欒兌婵挳鎮樺璺何﹂柛鏇ㄥ枤閻も偓闂佸湱鍋撻幆灞轿涢悙鐢电＝濞达絿鏅崼顏堟煕婵犲啰绠炵€殿喖顭烽弫鍐焵椤掑啰浜介梻浣虹帛閼归箖顢氶鐘电焼闁糕剝銇涢弨浠嬫煟閹邦厽缍戦柣蹇ラ檮閵囧嫰顢橀悙娴嬪亾閸噮鍤曟い鏇楀亾鐎规洜鍘ч埞鎴﹀幢濞嗘垵鏄ユ繝纰夌磿閸嬫垿宕愰弽顓炵濡わ絽鍟壕濠氭煟閹邦剚鎯堢痪鐐閵囧嫯绠涢幘鏉戞缂備浇顕уΛ婵嬪蓟閿濆绠涢柛蹇撴憸閻╁酣姊洪柅鐐茶嫰婢ь垶鏌ｅΔ鍐ㄢ枅闁绘侗鍣ｉ獮鍥级閸ф鏁归梻浣告惈濞层劑宕戝☉銏″€垫い鎾卞灪閸嬧剝绻濇繝鍌涘櫣妞わ絽銈搁幃浠嬵敍濞戣鲸鐤侀悗瑙勬礃濞叉牞鐏冩繛杈剧秮椤ユ挾绮诲鑸碘拺闂傚牊绋撴晶鏇㈡煙閸愯尙绠荤€规洖鍟跨叅妞ゅ繐鎳愰崢浠嬫⒑鐟欏嫬鍔ょ€规洦鍓熷畷婵嗩煥閸涱垳锛滅紓鍌欑劍椤洨绮婚悙缈犵箚妞ゆ劑鍨归弳娆撴煃閽樺妲搁柍璇查铻ｅù鍏肩懅绾惧吋绻濈喊澶岀？闁稿鍨垮畷鎰板箣閿曗偓绾惧潡鏌ょ喊鍗炲闁搞劍绻堥弻鐔煎箲閹伴潧娈紓浣哄У閻楁粓鎯€椤忓牜鏁囬柣鎰綑椤庢稑鈹戦埄鍐ㄧ祷妞ゎ厼鍢查～蹇涙惞閸忓浜鹃柣褎鎮傚顔界節閵忊埄鎴炵┍婵犲洤閿ゆ俊銈勮兌閸橀潧螖閻橀潧浠滈柣蹇旂箞閹﹢鍩￠崨顔惧帗闂佽姤锚椤﹁棄螣閳ь剚绻濆▓鍨灀闁稿鎹囧铏圭磼濮楀棛鍔烽梺杞扮椤兘骞冩导鏉戠缂備焦菤閹风粯绻涙潏鍓ф偧闁烩剝鏌ㄩ…鍥箛閻楀牏鍘靛銈嗙墬濮樸劑鎮為崫銉㈠亾鐟欏嫭纾搁柛銊ㄦ缂傛捇鎮惧畝鈧惌娆撴偣娓氼垳鍘涙俊鑼跺煐娣囧﹪鎮欓鍕ㄥ亾閺嶎厼钃熼柕濠忛檮濞呯娀鏌ｉ敐鍛板妞ゃ儲宀搁弻锕€螣娓氼垱锛嗛悷婊呭鐢寮查弻銉︾厱妞ゆ劦鍋勬禍婊勭箾閸喎濮囬柍瑙勫灴閹瑩鎳犻鈧·鈧繝娈垮枛閿曘倗绱炴繝鍐х箚闁汇垻顭堢粈瀣亜閺嶃劎鈯曟繛鍛濮婅櫣绱掑Ο鍝勑曢梺鍛婃尰閼规崘顣鹃梺鍓插亖閸庢煡宕愰崹顐ょ闁割偅绻勬禒銏ゆ煛鐎ｎ偆鈯曢柕鍥у椤㈡﹢鎮欓崣澹晠姊烘导娆戞偧闁稿繑蓱娣囧﹪骞栨担鍝ュ幐婵犵數濮撮崯顖氣枍瀹ュ鐓熼柣鏂挎憸閻﹪姊虹敮顔惧埌闁伙絿鍏橀獮妯肩磼濡桨缂撻梻浣告啞缁嬫垼澧濋梺褰掓敱濡炶棄顫忓ú顏勫窛濠电姴瀚уΣ鍫ユ⒑閹稿孩纾搁柛搴㈠▕閸┾偓妞ゆ巻鍋撻柣蹇斿哺閹兘鍩￠崨顓℃憰濠电偞鍨堕敃鈺呮偂閵夆晜鐓冪憸婊堝礈閻旂厧绠栭柣鎰劋閺呮繂霉閿濆棗绲婚柟顔荤劍缁绘繈鎮介棃娴躲儵鏌℃担鍛婂暈闁逛究鍔戦幃婊堟寠婢跺鈧剟姊鸿ぐ鎺擄紵缂佲偓娴ｅ搫顥氬┑鐘崇閻撶喖鏌熼柇锕€澧柟顖氱墦閺屾盯濡搁妷銉㈠亾閸ф钃熸繛鎴欏焺閺佸啴鏌ㄥ┑鍡橆棤妞わ负鍎崇槐鎾存媴閸濆嫅锟犳煕濡や礁鈻曠€殿喛顕ч埥澶愬閳哄倹娅囬梻浣瑰缁诲倸螞濞戔懞鍥Ψ閳哄倵鎷洪梺纭呭亹閸嬫稒淇婃總鍛婄厱濠电姴鍊归崑銉р偓娈垮枦椤曆囧煡婢舵劕顫呴柍鈺佸暞閻濇洟姊洪懡銈呅㈡繛璇х畳閵囨劙宕橀鍡曠瑝婵炲鍘ч悺銊╁煕閹寸姷纾奸悗锝庡亽閸庛儵鏌涙惔銏犲闁哄瞼鍠栭幃鐑芥偋閸喐鍊烽梻浣风串缁插潡寮绘径鎰﹂柟鐗堟緲缁犺櫕绻濋棃娑欏婵炲懎娲缁樻媴閾忓箍鈧﹪鏌涢幘瀵哥疄闁轰礁顑夊铏规嫚閼碱剛顔夐梺鐟版啞婵炲﹤顕?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗娼欒灃闁逞屽墯缁傚秵銈ｉ崘鈺冨弰闂佺厧顫曢崐鏇熺闁秵鐓熼柟鍨暙娴滄壆鈧娲栭悥鍏间繆濮濆矈妲峰┑鈩冨絻閻楁挸顫忓ú顏勭闁绘劖褰冮‖鍫ユ⒑缁嬪尅宸ユ繛灏栤偓鎰佸殨濠电姵纰嶉崑鍕煟閹捐櫕鎹ｉ柣锝夌畺濮婅櫣绱掑鍡樼暥闂佺粯顨嗙划宀冪亱闂佸憡娲﹂崢鍓у婵傚憡鐓熼柟浼存涧婢ь噣鏌涢弬璇测偓鏍Φ閸曨垼鏁囬柣妯诲絻楠炲鎮楀▓鍨灈妞ゎ參鏀辨穱濠囧箹娴ｈ倽銊╂煏韫囧﹤澧查柡澶嬫倐濮婄粯鎷呴搹骞库偓濠囨煕閹惧绠為柡浣割儏椤啴濡惰箛姘倕闂佸搫鎳愭慨鎾綖韫囨稒鎯為柛锔诲幘閻撴挸鈹戦悙鍙夘棡闁稿孩濞婂畷婊堟焼瀹ュ棌鎷虹紓鍌欑劍閿曗晛鈻撻弴鐔翠簻闁靛鍎婚弨璇睬庨崶褝韬柟顔炬櫕缁瑧鎹勯…鎴炐у┑锛勫亼閸婃牠鎮уΔ鈧～婵嬪Ω閳哄倸浠鹃梺閫炲苯澧存慨濠勭帛閹峰懐绮欏▎鐐棏闂備胶顭堥鍛村箠鎼达絽寮叉繝娈垮枟閵囨盯宕戦幘娣簻闁靛繆鍓濈亸顓熴亜閹剧偨鍋㈢€规洦浜濋幏鍛存偡閹殿喗袙闂傚倸鍊风欢姘焽瑜旈幃褔宕卞☉妯肩枃闁瑰吋鎯岄悡鍫ュ极閸℃稒鐓忛煫鍥ь儏閳ь剚鐗犻幃锟犳偄閸忚偐鍘介梺鍝勫€搁悘婵堢棯瑜庣换娑㈠川椤撱劌濮涢梺?
//      -> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟侗鍎愭い銉ョ墛缁绘盯骞嬪┑鍡氬煘闂佸憡鎸绘竟鍡欐閹烘鍊锋い鎺嗗亾闁告柣鍊栭妵鍕敇閻樻彃骞嬮悗娈垮枛椤兘寮幇顓炵窞濠电姴鍠氬Λ鐔兼⒒娴ｇ顥忛柣鎾崇墦瀹曟澘螖閸涱厾锛欏銈嗙墱閸嬬偤鍩涢幋鐘电＜閻庯綆鍋掗崕銉╂煕鎼达紕效闁哄本鐩顒勫箰鎼淬垹闂繝纰樷偓鍐茬骇闁告梹鐟╅悰顔嘉熺亸鏍т壕婵炴垶顏鍫晛闁规儳顕弧鈧┑鐐茬墕閻忔繈寮搁悢鍏肩厱闁宠桨鑳舵晶娑氱磼閸屾氨效闁诡喗鐟ラ湁閻庯綆鍋嗛埀顒€顭峰娲濞戞氨鐤勯梺绋匡攻閻楃姴鐣烽幇鐗堝€婚柤鎭掑劤閸樺崬鈹戦悩缁樻锭婵☆偅顨婇、鏃堫敃閳惰姤妫冮幃鈺呭矗婢跺﹥鐏庢繝娈垮枛閿曘儱顪冮挊澶屾殾婵犲﹤鐗嗙粻鐢告煙閸濆嫮啸闁糕晛鎳樺缁樻媴娓氼垳鍔稿銈嗗灥閹虫劙鍩€椤掍礁鍤柛妯哄⒔閸掓帞鈧綆浜堕崥瀣熆閼哥灙鎴﹀疾椤掆偓閳规垿鎮╃拠褍浼愰柣搴㈠嚬閸撴氨绮?2 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓洪梺缁樺灥濡盯宕濋姘ｆ斀闁绘劖娼欓悘锔姐亜椤撶偞鍠樼€殿喗濞婂畷鍗炩槈濞嗘垵骞堥梻浣告惈濞层垽宕濈仦鍓ь洸闁绘劖鐣禍婊堟煥閺冨浂鍤欐鐐寸墵閺屽秶鎲撮崟顐や紝闂佽鍠掗弲婵堟閹烘嚦鐔哥節濮橆厽娈繝鐢靛Х閺佹悂宕戦悩娲绘晪婵犲﹤鎳愭稉宥夋煙鏉堝墽鐣遍柣鎾寸箘閳ь剙绠嶉崕閬嵥囨导鏉戠；闁归偊鍠氱壕钘壝归敐鍫殐闁绘帊绮欓弻娑樜熼幐搴闂佸疇顫夐崹鍧楀春閵夆晛骞㈡俊銈呭暕濞ｎ噣姊绘担渚劸妞ゆ垵妫濆畷婵單旈崨顓熺€銈嗘煥閻ㄥ嘲危閸儲鐓忛煫鍥ュ劤绾惧潡鏌涘Ο鍦煓婵﹥妞藉畷鐑筋敇閻愭彃顬嗘繝娈垮枛閿曘劌鐣烽悽绋挎瀬妞ゆ柨鐨峰Σ鍫ユ煏韫囥儳纾块柛妯虹仛缁绘盯骞樼壕瀣棟闂佽绻戠换鍐╃┍婵犲啰闄勯柛娑橈攻閸ゅ姊洪幐搴ｇ畵闁瑰嘲顑夐幃銏ゅ幢濞戞瑧鍘遍梺瑙勫礃鐏忔瑧鎷规导瀛樼厽闁圭儤顨堥悾娲煛瀹€鈧崰鏍€佸☉姗嗘僵闁告鍋愰崣鐑樼節濞堝灝鏋涢柨鏇樺劚椤啴鎸婃径灞炬闂侀潧顭俊鍥╁姬閳ь剟姊洪崨濠冨闁告挻鐟╁鎼佹晝閸屾稈鎷绘繛杈剧导鐠€锕傛倿閻愵兙浜滈柟瀛樼箖椤ャ垺顨ラ悙鏉戠伌鐎规洘鍎奸¨浣圭箾閸忚偐澧辩紒杈ㄦ尰閹峰懘鎳栭埄鍐ㄧ仸闁告帒锕獮姗€顢欓悾灞藉箺闂備胶绮濠氬储瑜庣粋宥嗗鐎涙鍘遍梺闈涚墕閹峰宕曢幋鐘电＜閺夊牄鍔嶇亸浼存煙瀹勭増鍤囩€规洘绮撻獮鎾诲箳閹存繍妫婇梻鍌氬€烽悞锕傚箖閸洖绀夐悘鐐靛亾濞呯娀骞栨潏鍓ф偧缁惧彞绮欓弻鐔煎箚瑜滈崵鐔搞亜閳哄啫鍘撮柟顔筋殜閺佹劖鎯斿┑鍫熸櫦闂備焦妞块崑鍕偓姘緲椤繘鎼归崷顓犵厯闂佸湱顭堢€涒晠骞忔潏鈺冪＝濞撴艾娲ら弸娑㈡⒑鐢喚绉?10 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎悗骞垮劚椤︻垳绮堢€ｎ偁浜滈柟鍝勭Ф閸斿秵銇勯弬鎸庡枠婵﹦绮幏鍛村川婵犲懐顢呴梻浣侯焾缁ㄦ椽宕愬┑鍡欐殾闁汇垹鎲￠弲婵嬫煃瑜滈崜鐔煎春閵夛箑绶炲┑鐐灮閸犳牠寮婚妸褉鍋撻敐搴′簼婵犮垺鍨垮缁樻媴缁涘娈梺鍛婂灩閺咁偆妲愰悙鍝勫耿婵☆垳鈷堝ú鎼佹⒑缂佹ê濮冮悹鈧敃鍌氭瀬濠电姴娲﹂悡鏇㈡煏婢舵ê鏋涘褜鍨堕弻宥夋煥鐎ｎ亞浼岄梺璇″枛閸㈡煡鍩㈡惔銈囩杸闁瑰灝鍟╅幃锝囩磽閸屾瑨顔夐柛瀣崌閺屾盯骞囬妸锔芥緭婵炲瓨绮嶇划鎾诲蓟閿熺姴鐐婄憸搴ㄋ夊鍫熺厵闁惧浚鍋勬慨宥夋煙椤栨氨鐒哥€规洖宕埥澶娾枎閹存繂绠為梻鍌欑窔閳ь剚绋戝畵鍡樼箾娴ｅ啿娲ら崙鐘绘煟閺傚灝鎮戦柣鎾跺枛閺岀喖骞嗚閹界姷绱掗埀顒勫礃椤忓棛锛滅紓鍌欑劍椤洤煤鐎涙﹩娈介柣鎰▕閸庡繘鏌嶇憴鍕伌鐎规洖宕～婵嬵敆閸屾艾绠ｉ梻鍌氬€搁崐椋庣矆娓氣偓楠炴牠顢曢敂缁樻櫈闂佹悶鍎洪崜娆戠玻濡ゅ懎绠规繛锝庡墮婵＄晫绱掗悩鍐叉诞婵﹦绮幏鍛存惞閻熸壆顐兼俊鐐€ら崢濂告偋韫囨洖鍨濋柡鍐ㄧ墱閺佸鏌嶈閸撶喖濡存笟鈧顕€宕煎┑瀣暪婵犳鍠楅敃顐ゅ緤閼恒儳顩插Δ锝呭暞閻撱儵鏌￠崶鈺佷粶闁逞屽墯閹倿寮婚妸鈺佄ч柛鈩冩礈缁犳岸姊洪棃娑氬閻庢凹鍣ｈ棟闁挎梻鏅粻楣冩煕椤愩倕鏋旂紒鐘靛仦閹便劍绻濋崒娑欏創闁轰礁鐗撻弻娑㈠Ψ閹存繃鍣哥紒杈ㄦ礋濮婄粯鎷呴懞銉ｂ偓鍐煟閹虹偟鐣垫鐐插暣瀹曟帒顫濇潏銊︻啎闂備焦瀵х换鍌炲箠婢舵劕缁╁ù鐘差儐閻撴洘銇勯幇顔夹㈤柣蹇ｅ櫍閺屻劌鈽夊▎鎴犵暫缂備胶绮粙鎺戭焽韫囨稑绀堢憸蹇氣叢濠碉紕鍋戦崐銈夊磻閸曨垁鍥偨缁嬫寧妲┑鐐村灟閸ㄥ湱绮诲☉娆嶄簻闁规崘娉涘暩濡炪倖姊瑰ú鐔奉潖閾忓湱鐭欓悹鎭掑妿椤旀垿姊虹粙娆惧剱闁告梹鐟╅悰顔界節閸パ呭€炲銈嗗笂閼冲爼宕㈤柆宥嗏拺鐎规洖娲﹂崵鈧梺璇茬箲缁诲牓骞冨鈧、鏃堝醇閻斿搫骞嶆俊鐐€栭悧妤冪矙閹烘澶愬醇濠靛啯鏂€闂佹枼鏅涢崯顖炲磹閹邦兘鏀介柨娑樺閻掓寧銇勯敃浣哥毢闁逞屽墯椤旀牠宕伴幘璇茬闁瑰瓨绻嶅鏍磽娴ｈ偂鎴炲垔閹绢喗鐓曟繛鎴烇公瀹搞儵鏌￠崱妤冨缂佺粯鐩獮妯尖偓闈涘閺嗏€愁渻閵堝啫濡奸柨鏇樺€濋幃楣冩偪椤栨ü姹楅梺鍦劋閸ㄥ綊顢欓崨瀛樷拺闁告稑锕ョ亸顓㈡煟韫囨梻绠撻悡銈嗐亜閹垮啯濞囬柍褜鍏涚粈渚€锝炲┑鍫熷磯闁惧繒鎳撴慨锔戒繆閻愵亜鈧牕顔忔繝姘；闁圭偓鐣禍婊勩亜閹扳晛鐏紒鐘茬－缁辨帗娼忛妸銉х懆闁句紮缍侀弻銈吤圭€ｎ偅鐝曢梺鎼炲€曢惌鍌氼潖缂佹ɑ濯撮悷娆忓娴犫晠姊虹粙鍨劉缂侇喗鎸搁悾宄邦潨閳ь剟骞冨▎鎿冩晞闁圭楠搁弨顓㈡⒒閸屾艾鈧悂宕愰幖浣哥９鐎瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰绠绘鐐村灴婵偓闁靛牆鎳愰娲⒑缂佹◤顏堝疮閹稿孩鍎熷┑鐘叉处閳锋垿鏌涢幘鏉戠祷濞存粍绻勭槐鎺旀嫚閹绘帩浼冮悗娈垮枟婵炲﹪寮崘顔肩＜婵炴垶鑹剧敮楣冩⒒婵犲骸浜滄繛灞傚€濋、鏍川閺夋垹鍔﹀銈嗗笂閻掞妇绮堥埀顒€鈹戦悙鏉戠祷闁诲繑绻堥崺鐐哄箣閿旇棄浜瑰銈嗘閸嬫劖瀵奸崶褉鏀介柣鎰▕濡插綊鏌ｉ埡濠傜仸鐎殿噮鍋婂畷姗€顢欓懞銉︾彇闂備胶顭堥鍥箠閹捐秮娲敇椤兘鍋撴笟鈧鎾閳╁啯鐝曢梻浣藉Г閿氭い锔诲亰瀹曟垿骞樼紒妯轰缓闂佸憡绋戦敃锕傚储娴犲鈷戠紓浣股戦悡銉╂煙鐠囇呯？婵炲棎鍨藉畷锝嗗緞閸粎鐩庨梻濠庡亜濞村嫮寰婇懖鈺勫С缂備焦菧娴滄粍銇勯幘璺轰沪闁稿鍎甸弻鐔碱敊閹冨箣闂佽鍟崶褔鍞跺銈嗗姧缂嶅棗螞瑜嶉埞?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵潙螖閳ь剚绂嶉幆褜娓婚柕鍫濈凹缁ㄥ鏌涢悢鍛婄稇闁伙絿鏌夐妵鎰板箳濠靛洦娅旈梻浣告啞娓氭宕归悧鍫熷弿婵炲樊浜濋埛鎺懨归敐鍕劅婵炲吋甯￠弻娑㈠即閻愬吀绮甸梺浼欑到婢у海妲愰幘瀛樺闁圭粯甯婃竟鏇炩攽閻橆喖鐏辨繛澶嬬⊕瀵板嫰宕堕鈧壕濠氭煙閹规劕鍚圭€规挷绶氶弻娑⑩€﹂幋婵囩亪婵犳鍠栨鎼佲€旈崘顔嘉ч煫鍥ㄦ尵濡诧綁姊洪幖鐐插婵炲鐩、姘舵晲閸℃鍤ら梺鍝勵槹閸わ箓濡搁埡鍌滃弳闂佸搫鍊搁悘婵嬪煕閺冣偓閵囧嫰寮埀顒€煤閻斿娼栨繛宸簼閸嬶繝姊洪銊х暠婵炲牆鐖煎铏圭矙濞嗘儳鍓抽梺鍝ュУ閻楃娀鎮伴鐣岀瘈闁搞儜鍐崺闂佽瀛╃粙鎺椻€﹂崶顒€鍌ㄩ柟鎯у绾句粙鏌涚仦鍓ф噮闁告柨绉归幏鎴炴綇閵婏絼绨婚梺闈涱槶閸庤櫕鏅堕敃鍌涚厸鐎光偓閳ь剟宕伴弽顓溾偓浣糕枎閹炬潙娈愰梺鍐叉惈閸烆參濡搁埡鍌氣偓鐢告偡濞嗗繐顏紒鈧埀顒勬⒑缂佹澧柛姘儐缁岃鲸绻濋崶褏顔愭繛杈剧秬閸婁粙濡歌閸犳劙鐓崶銊р槈閸ユ挳姊虹紒妯虹伇濠殿喓鍊濋幃锟犳偄閸忚偐鍘介梺褰掑亰閸撴盯骞楅悩缁樼厽闁靛绠戦悘鎾煛鐏炲墽娲撮柟顔规櫅閻ｇ兘宕惰閸氬倹淇婇悙顏勨偓銈夊磻閸曨厽宕查柟閭﹀枛瀵弶淇婇悙顏勨偓鏇犳崲閹邦喒鍋撳闂寸敖婵″弶鍔欏畷濂稿即閻斿弶瀚奸梻浣告啞缁嬫垿鏁冮妷锕€绶為柛鏇ㄥ灡閻撴洟鎮楅敐搴濈盎闁逞屽墮椤兘骞婇悙瀛樼秶闁靛ě灞剧カ闂佽鍑界紞鍡涘磻閳ь剟鏌熼柨瀣仢婵﹥妞藉畷銊︾節閸曘劍顫嶆俊鐐€ら崣搴ㄥ礈閻旂厧绠栭柣鎰暘閸嬪懘鏌涢幇銊︽珖闁告﹢浜堕弻锝嗘償椤栨粎校闂佺顑呯€氼參骞堥妸鈺佺倞妞ゆ巻鍋撶痪顓涘亾闂傚倷绶￠崜娆戠矓閻㈠憡鍋傛繛鍡樻尰閻撶喐淇婇妶鍌氫壕闂佺粯顨呴敃锕€鈻庨姀銈嗗殤妞ゆ帒鍊婚敍婊堟⒑缂佹鎳冮柟铏姉缁棃顢氶埀顒勫蓟閿涘嫪娌悹鍥ㄥ絻椤洭鎮楀▓鍨灍婵炲吋鐟ㄩ悘鍐⒑闁偛鑻晶顖滅磼閸屾氨效鐎规洘绮忛ˇ瀵哥棯?g_warpManualStop=true闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏㈡偘閳ュ厖澹曢梺姹囧灪椤旀牠鎮為幆顬″綊鎮╁▎蹇斿闁抽攱甯￠弻娑㈠即閵娿儰绨诲銈呮禋閸欏啴寮婚垾宕囨殕閻庯綆鍓涢惁鍫ユ倵鐟欏嫭绀冮柨鏇樺灪娣囧﹪骞栨担鑲濄劑鏌曡箛鏇炐″瑙勬礋濮婃椽鎮℃惔顔界稐闂佺锕ラ〃鍡欑矙婢跺鍚嬪璺侯儑閸樻悂姊虹粙鎸庢拱濞撴碍顨婂畷鏇㈠Ψ閳哄倻鍘介梺鍝勫€圭€笛囁夐姀銈嗙厸閻忕偠顕ч埀顒佺箞閻涱噣寮介妸锕€顎撻柣鐔哥懃鐎氫即寮鍕ㄦ斀闁绘ɑ鍓氶崯蹇涙煕閻樺磭澧悡銈夋煏婵炑冨濞肩喖姊洪崷顓炲妺妞ゃ劌妫濆畷鎴﹀箛椤撴粈绨婚梺鍝勫暙閸婂摜鏁捄琛℃斀妞ゆ洖妫涢悾鍨叏婵犲啯銇濈€规洦鍋婂畷鐔碱敇閻樿京绀冮梻鍌欑閹芥粍鎱ㄩ悽鍛婂亱闁绘宕靛畵渚€鏌涢幇鈺佸Ψ闁哄閰ｉ弻鐔煎垂椤斿吋娈查梺鍝勬噷閸ㄨ櫣鎹㈠☉娆愮秶闁告挆鍛呮艾鈹戦悙鍙夊珔缂傚秳绶氬畷娲閿濆洨鏉搁梺鍝勫€告晶鐣岀不濮樿埖鈷戠紓浣姑慨锕€霉濠婂嫮鐭掗柛鈹惧亾濡炪倖甯婄粈渚€鎮樼€电硶鍋撶憴鍕婵炲弶锚椤洩绠涘☉妯煎幐闂佸憡鍔︽禍鐐哄储閻樼粯鐓熼柣鏂挎憸閻苯顭胯椤ㄥ牓寮鈧獮鎺懳旀担鍝ュ絽?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨绘い鎺嬪灪閵囧嫰骞囬鍡欑厯闂佸搫琚崝鎴﹀箖閵忋倕浼犻柛鏇熷灟閸ㄥ鎯€椤忓牆绠氱憸婊堟偂婵傚憡鐓涢悘鐐插⒔閳藉鏌嶉挊澶樻█鐎规洩绻濋幃娆戔偓鐢告櫜婢规洟姊婚崒娆戭槮缁剧虎鍙冮獮濠囧箻閸撲胶鐓嬮悷婊呭鐢帡鎷戦悢鐑樺枑闁瑰濮锋稉宥嗙箾閹寸儑渚涙繛灏栨櫊閺屾稖绠涘顑挎睏缂備礁顦遍崗姗€寮?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厱闁归偊鍨扮槐锕傛煟閵忕媭鐓兼慨濠勭帛閹峰懘鎮烽柇锕€娈濇繝鐢靛仜瀵爼鎮ч悩鑼殾闁归偊鍨禍褰掓煙閻戞ê鐏╅柨娑欑洴閺岋絾鎯旈婊呅ｉ梺鍝ュУ椤ㄥ﹪骞冮垾鏂ユ闁靛骏绱曢崢閬嶆⒑鐟欏嫬顥愰柡鍛⊕娣囧﹥绺介崨濠勫帗閻熸粍绮撳畷婊冣枎閹绢垪鍋撻敃鈧悾锟犲箥椤旇姤顔曢梻浣哄仺閸庢煡宕滃顒夌劷闁冲搫鎳忛悡鐘绘煙闂傚鍔嶆繛鎳峰洦鐓冪憸婊堝礈濞嗘挻鍊舵慨妯块哺瀹曞弶绻涢幋娆忕仼缂佺媴缍侀弻锝夊箛椤掑倷绮甸梺闈╃导缁瑥顫忛搹鍦＜婵☆垵娅ｉ悷鎰磽娓氬洤鏋熼柣鐔叉櫅椤?204 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗娼欒灃闁逞屽墯缁傚秵銈ｉ崘鈺冨弰闂佺厧顫曢崐鏇熺闁秵鐓熼柟鍨暙娴滄壆鈧娲栭悥鍏间繆濮濆矈妲峰┑鈩冨絻閻楁挸顫忓ú顏勭闁绘劖褰冮‖鍫ユ⒑缁嬪尅宸ユ繛灏栤偓鎰佸殨濠电姵纰嶉崑鍕煟閹捐櫕鎹ｉ柣锝夌畺濮婅櫣绱掑鍡樼暥闂佺粯顨嗙划宀冪亱闂佸憡娲﹂崢鍓у婵傚憡鐓熼柟浼存涧婢ь噣鏌涢弬璇测偓鏍Φ閸曨垼鏁囬柣妯诲絻楠炲鎮楀▓鍨灈妞ゎ參鏀辨穱濠囧箹娴ｈ倽銊╂煏韫囧﹤澧查柡澶嬫倐濮婄粯鎷呴搹骞库偓濠冧繆閻欐瑥娲ら悿鐐節婵犲倹鍣介柟鍐叉嚇濮婄粯鎷呴崨濠冨創濡炪倧瀵岄崑濠傤嚕閹绘帩鐓ラ柛娑卞枛鎼村﹪姊洪崜鎻掍簼缂佽绉撮…鍥冀椤撶喓鍙勯棅顐㈡处閹歌崵鎷归敓鐘崇厽妞ゆ挾鍠撻幊鍥煛瀹€鈧崰鎾跺垝濞嗘挸鍨傛い鏇炴噹婵＄兘姊绘担鍛婅础闁哄苯顦扮粋宥夘敆閸屾侗娼熼梺瑙勫劤閻°劍鍒婄€靛摜纾兼繛鎴烇供閸庡繑绻涢崼顐㈠籍闁哄矉绲鹃幆鏃堝閻樺弶鐦撻梻浣告憸婵潧鐣濈粙璺ㄦ殾婵娉涚粻鎶芥煙閹冾暢闁伙箑鐗嗛埞鎴︻敊閺傘倓绶甸梺鍛婃尰缁诲牓鐛Δ鍛嵆闁靛繆妾ч幏娲⒒閸屾氨澧涘〒姘殔椤﹨顦查柍瑙勫灴閸ㄩ箖鎼归銏＄亷闁?true 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊為悷婊冪箻瀵娊鏁冮崒娑氬幗闂侀潧绻堥崺鍕倿閸撗呮／闁诡垎宀€鍚嬮梺鍝勭焿缂嶄線鐛崶顒夋晩闁兼亽鍎查惁搴ㄦ⒒娴ｈ銇熼柛妯圭矙閹兘鍩￠崨顔间簵濡炪倖鍔х粻鎴︽倷婵犲洦鐓忓┑鐐戔偓閸嬫挻淇婇幓鎺斿ⅱ缂佽鲸鎸婚幏鍛村箵閹哄秴顥氭繝鐢靛О閸ㄥジ宕洪弽顓炵闁哄洨濮锋稉宥夋偡濞嗗繐顏紒鐘荤畺閺屻劌鈹戦崱娑扁偓妤€顭胯閸犳牠鍩為幋锔芥櫖闁告洦鍋勬禒妯侯渻閵堝倹娅呴柕鍫㈩焾閻ｇ兘濡搁埡濠冩櫍濠电偞鍨堕悷锕傚级閸涘﹣绻嗛柣鎰典簻閳ь剚鐗曢～蹇旂節濮橆厼浠遍梺鍝勫暙閻楀﹪宕愰崸妤佺厽闁逛即娼ф晶鎵磼閻樺啿鍔ゆい顓℃硶閹瑰嫭绗熼姘闂備線娼уΛ娆戞暜閻愬灚顫曢柟鐑橆殢閺佸﹪鏌ゅù瀣珔闁愁亞鏁婚幃妤冩喆閸曨剛顦梺鍝ュУ閻楃娀濡存担绯曟婵☆垶鏀遍～宥呪攽椤旀枻渚涢柛鎾寸懅缁鎮欏ù瀣杸闂佺粯锚閻忔岸寮抽埡浣叉斀闁绘劏鏅涙禍鐐節閻㈤潧浠掗柛鏍█瀹曟鎮欓鍌楁闁剧粯鐗犻弻娑橆煥閳ь剟鎮為敃鍌氱柧婵犲﹤鐗婇埛鎴︽煕閹邦剙绾ч柟顖氱墦閺屾盯鎮㈢粙娆炬婵犮垼顫夊ú鐔煎箖閸撗傛勃缂佸銇樻竟鏇炍旈悩闈涗粶闁诲繑绻堝畷婵嗩潩椤撴繄绠氬銈嗗姧缁插潡骞婇崘顔界厽闁挎繂鎳庡Σ濠氭煟閿濆棛绠炵€规洜鍠栭、妤呭磼濮橆剛顔囬梻鍌欐祰椤曆呮崲閹烘纾绘繛鎴炵懄濞呯姴霉閻樺樊鍎忛柣鎾偓鎰佺唵闁兼悂娼ф慨鍫ユ煟閹惧啿鏆ｉ柟顔肩秺瀹曞爼濡歌瀵劌顪冮妶鍛劉妞ゃ劌锕獮妤呭醇閺囩喐娅滄繝銏ｆ硾椤戝洭宕ｉ崟顖涱棅妞ゆ劑鍨烘径鍕磼鐎ｎ亞澧︾€规洘鍨块獮姗€寮妷锔绘綌婵犵妲呴崹顖滄媰閿曞倸鍚规繛鍡樻尰閳锋帡鏌涚仦鎹愬闁逞屽墯閹倸鐣烽幇顒傛殝闁瑰啿锕ょ紞濠傜暦閹偊妾ㄥ┑鐐茬摠閻楃娀寮婚弴鐔虹闁绘劦鍓氶悵鏇㈡⒑閹惰姤鏁辨慨濠傤煼閸┾偓妞ゆ巻鍋撶紒鐘茬Ч瀹曟洟鏌嗗畵銉ユ喘椤㈡稑顭ㄩ崘鐐棥闂佽崵鍠愰悷銉р偓姘煎枤瀵囧焵椤掑嫭鈷掗柛灞炬皑婢ф稓绱掔€ｎ偄娴柟顖氳嫰铻栭柛鎰典簽閿?
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
        if (st == 0) {                     // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帗绂掓ィ鍐┾拺缂佸鐏濋銏ゆ煕閵娿儲鍋ョ€殿喖顭烽弫宥夊礋椤忓懎濯伴梺鑽ゅТ濞诧箒銇愰崘鈺傚弿閹兼番鍔嶉悡鏇㈡煏婢舵盯妾悘蹇ョ畵閺屸€崇暆閳ь剟宕伴弽褏鏆︽俊銈呮噺閸ゅ啴鏌嶉崫鍕偓褰掝敊閸愵喗鈷掑ù锝堟閵嗗﹪鏌￠崨顔俱€掗柍褜鍓氶崙褰掑礈濠靛鍊堕悗锝庡枟閳锋垿鏌涘☉姗堝姛闁宠棄顦甸弻锝嗗箠闁告梹鍨垮畷鍝勨槈閵忕姷顓洪梺鎸庢濡嫭绂掗鐐粹拺闁硅偐鍋涢崝姗€鏌涢弬鍧楀弰鐎规洩缍侀崺鈧い鎺嗗亾妞ゎ亜鍟存俊鍫曞川椤撗冨Ψ婵＄偑鍊戦崝宀勬偋韫囨拹锝夊箛閺夎法顔婂┑掳鍊撶粈浣圭瑜版帗鈷戦柤娴嬫杹閹凤繝鏌涙惔锝嗙効妞わ附鎸抽弻宥堫檨闁告挻绻堥敐鐐村緞婵炴帒鎼…銊╁醇濠靛牏鏋冮梻浣筋潐瀹曟ê鈻斿☉姗嗗殨闁告瑥顦换鍡涙煟閹板吀绨婚柍褜鍓氬ú婊堝箲閵忋倕绠涙い鏃傛嚀娴滅偓鎱ㄥΟ鐓庡付妤犵偞鐗犲畷鈩冩綇閵婏絼绨婚梺鐟版惈濡绂嶆ィ鍐┾拺閻庡湱濯鎰版煕閵娿儲鍋ユ鐐插暙閳诲酣骞樺畷鍥崜闂備胶鎳撻顓熸叏閸愬樊娴栭柟鎯ь嚟缁♀偓濠电偛鐗嗛悘婵嬪几閻斿吋鍤曠憸鏃堝蓟濞戙垹绫嶉柍褜鍓熷畷褰掓偨閸涘﹤浠奸梺缁樺灱濡嫰鎮橀幎鑺ョ厵濡鑳堕崝宥夋煕瀹ュ娑фい顏勫暣婵″爼宕卞Ο娲€虹紓鍌欑贰閸犳牠鎮ц箛娑欏殟閺夊牄鍔庣弧鈧┑顔斤供閸樻悂骞忓ú顏呪拺闁告稑锕︾粻鎾绘倵濮樼厧鏋﹂柍顏勭秺濮婄粯鎷呮笟顖滃姼闂備礁搴滅徊鍓х矉瀹ュ閱囬柡鍥╁仦濡差剟姊洪柅鐐茶嫰婢ь垶鏌曢崶褍顏鐐村笒椤撳ジ宕堕鍡欏炊濠电姷鏁搁崑娑㈡偋婵犲洠鈧箓宕堕埡鍌ゆ綗闂佸湱鍎ら〃蹇涘极婵犲洦鐓犳繛鏉戭儐濞呭懏銇勯妶鍌氫壕闂備浇顕ф鎼佹倶濮橆剦鐔嗘俊顖氱毞閸嬫挸顫濋悡搴＄闂佺懓绠嶉崹褰掑煡婢舵劕顫呴柣妯活問閸炴椽姊绘担鐑樺殌妞ゆ洦鍘介幈銊╂煥鐎ｅ灚顔勯梻鍌氬€搁崐宄懊归崶顒夋晪闁哄稁鍘肩粣妤呮煙閻戞﹩娈旈柣銈夌畺閺岋絽螣閼姐倖鑿囬梺閫炲苯鍘哥紒鑸佃壘椤曪綁骞橀鍡欑◤濡炪倖鏌ㄦ晶浠嬪煟閵堝棔绻嗛柣鎰典簻閳ь剚鐗犻獮鎰偅閸愩劎锛熼梺褰掑亰閸庨潧鈽夊鍡欏弳闂佸憡鍔х徊楣冨棘閳ь剟姊绘担铏瑰笡闁挎岸鏌ｈ箛鏂垮摵鐎殿喗濞婇崺锟犲川椤旀儳骞堥梺璇插嚱缂嶅棝宕滃▎鎰浄閺夊牃鏂侀崑鎾舵喆閸曨剛顦ョ紓鍌氱Т閿曘倝鎮鹃柨瀣檮缂佸鐏濆畵鍡涙⒑缂佹ê濮岄柛鈺傜墵楠炲瀵肩€涙ǚ鎷绘繛鎾村焹閸嬫挻绻涙担鍐叉椤ゅ倿姊绘担鍛婃儓闁瑰啿绻掗崚鎺楀箻瀹曞洦娈鹃梺鐟邦嚟婵參宕戦幘缁樻櫜閹肩补鍓濋悘宥夋⒑缂佹ɑ灏伴柣鐔叉櫊瀵鎮㈤悜妯烘瀭闂佸憡娲﹂崑鍕杺闂傚倷鐒﹂幃鍫曞磹閺嶎厽鍋＄憸蹇撐ｉ幇鏉跨婵°倐鍋撻柣鎺戠仛閵囧嫰骞掗幋婵愪患缂佺偓鍎抽崥瀣崲濞戙垹绠ｉ柣鎴濇閻ｈ偐绱撴担鍝勑ｉ柤褰掔畺閳ユ棃宕橀鍢壯囨煕閳╁啰鎳冩い顐庡懐纾藉ù锝囩摂閸ゆ瑩鏌涙繝鍌滀虎闁伙絿鍏樻俊鐑藉Ω閿濆嫭鐫忛梻浣告贡閸庛倝骞愭ィ鍐ㄥ偍闁规儳澧庣壕浠嬫煕鐏炲墽鎳呴柛鏂跨Ч閹锋垿鏌嗗鍡欏帗闂備礁鐏濋鍛归鈧弻锛勪沪閻ｅ睗銉︺亜瑜岀欢姘跺蓟濞戙垹绠婚悗闈涙啞閸ｎ喚绱撴笟鍥ф灕妞ゆ泦鍥舵晣闁稿繒鍘х欢鐐测攽閻樻彃顏电紒杈╂暬濮婂宕掑▎鎴ｇ獥闂佸憡鎸婚悷褏鍒掗弮鍌楀亾闂堟稒鎲稿☉鎾崇Ч閺岋絽螣绾拌鲸姣岄梺绋款儐閹瑰洭鎮伴鈧畷褰掝敊閻撳寒娼涘┑鐘垫暩閸嬫盯鎯囨导鏉戠９婵犻潧妫涢弳?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶鏉款熆鐟欏嫭绀冪紒缁樼箓椤繈顢橀悩鎻掔疄闂傚倷鐒﹂弸濂稿疾濞戙垹鐤ù鍏兼綑閻撯€愁熆閼搁潧濮堥柣鎾存礋閹﹢鎮欐０婵嗘婵犳鍠栭崐鍧楀蓟閿涘嫪娌悹鍥ㄧゴ閸嬫捇寮撮姀锛勭暢闂備浇顕ч崙鐣岀礊閸℃稑纾婚柛娑樼摠閸嬬喖鏌￠崶鈺佸壉婵?
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
constexpr wchar_t kAppVersion[] = L"3.20.0";

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
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氶梺璇叉唉椤煤韫囨稑纾块柟鎯版閻掑灚銇勯幒鎴姛缂佸鏁婚弻娑滅疀閺冩捁鈧法鈧娲橀崹鍧楃嵁濮椻偓瀹曟粍鎷呴搹瑙勬濠电姷鏁搁崑鐐哄垂閹増鎳岄梻渚€娼荤徊鐣岀礊婵犲洤钃熸繛鎴欏灩鎯熼梺鎸庢煥婢т粙鎮块崟顖涒拺閻犲洠鈧櫕鐏嶉梺鎸庢磸閸ㄤ粙鐛崘銊㈡瀻闁规儳鍟块悗顓烆渻閵堝棗濮х紒鑼跺劵閵囨劘顦规慨濠勭帛閹峰懘宕ㄦ繝鍐ㄥ壍婵犵妲呴崑鍕敋椤撶姴鍨濆┑鐘宠壘缁犲鎮归崶顏勮敿闁硅姤娲熷娲箰鎼达絿鐣靛銈忓瘜閸ㄦ娊鎮幆褜鍚嬪璺侯儑閸橀亶鏌ｈ箛鏇炰沪鐎规洘蓱缁旂喎螣婵傝棄缍婇幃鈺呭箛娴ｅ搫濮虹紓鍌欐祰妞存悂骞愭繝姘闁告稒娼欐导鐘绘煕閺囩喎鐓€缂佹彃澧介幑銏犫攽鐎ｎ亶娼婇梺瀹犳閹虫捇鍩€椤掆偓濞硷繝寮诲澶娢ㄦい鏍ㄧ矌閺嗐倝鎮楃憴鍕妞ゎ厼鐗愰悘鍐╃箾鏉堝墽鍒伴柟璇х節閹潡宕掗悙绮规嫽闂佺鏈懝楣冨焵椤掍焦鍊愰柟顔ㄥ嫮绡€闁告洦鍘鹃悡鎾绘⒑绾懏褰х紓宥呮瀵爼鎮滃Ο鑲╃槇闂佹眹鍨藉褍鏆╅梻浣芥〃閻掞箓骞冮崒姘辨殾闁瑰瓨绺惧Σ鍫ユ煏韫囧ň鍋撻崗鍛潟闂傚倷绶氬褔鈥﹂崼銉ョ？鐎广儱鎮垮ú顏嶆晜闁告侗鍘虹花濠氭⒒娴ｈ櫣甯涢柟鍝ヮ焾闇夐柣鎴ｅГ閸嬧晠鏌涘┑鍡楊仱闁衡偓娴犲绠抽柟鎯版绾惧綊鏌熼悧鍫熺凡缁炬儳顭烽弻鐔煎礈瑜忕敮娑㈡煟閹惧啿鏆ｉ柡宀嬬畱铻ｅ〒姘煎灡閳绘挸鈹戦垾鍐茬骇闁诡喖鍊垮璇差吋閸ャ劌鐝伴梺鍝勮閸庢椽鍩€椤掍緡娈滈柟钘夌埣閺佹劖寰勭€ｎ剙骞樺┑鐘愁問閸犳宕濋弴銏犵劦妞ゆ帊鑳舵晶顏堟煃缂佹ɑ灏伴柕鍫秮瀹曟﹢鍩為悙顒€顏洪梻鍌欒兌椤牓寮甸鍕仭闁挎洖浼栭崶顭戞Ч閹艰揪绲块敍婊勪繆閹绘帗婀伴柣妤佹礀閳诲秴顓奸崱妯哄伎婵犵數濮抽懗鍫曟儗濞嗘垟鍋撶憴鍕闁哥姵鐗犻妴浣割潨閳ь剟骞冮姀鐘垫殝闁规鍠栭幖鎼佹⒒閸屾艾鈧绮堟担鍦彾濠电姴娲ょ壕璇层€掑锝呬壕閻庤娲樺ú鐔风暦濮椻偓閹崇娀顢楁担璇″晫婵犵數鍋為幐濠氬春閸愵喖纾婚柟鍓х帛閻撴稓鈧厜鍋撻柍褜鍓熷畷浼村冀椤撶偟鐣哄┑鐘诧工閸犳艾顭囬妸鈺傜厱闁斥晛鍠氶悞浠嬫煛閸℃劕鍔ょ紒杈ㄦ崌瀹曟帒鈻庨幒鎴濆腐濠电姵顔栭崰鏍倶濮樿鲸宕叉繛鎴炵懅缁♀偓闂佹悶鍎滈崱妯诲暫闂傚倷鑳剁划顖毼涙笟鈧俊鎾焵椤掑嫭鐓曢悗锝庡亝瀹曞矂鏌熼鎸庮棄闁崇粯鎸鹃幑鍕Ω閵夈儛顏堟⒒娴ｅ憡鎲搁柛鐘虫崌閳ワ箓宕奸妷鈶╁亾閺冨牆绀冩い鏂挎瑜旈悡顐﹀炊閵婏妇顦柦鍐憾濮婄粯鎷呴搹鐟扮闂佺懓鎲￠幐鎶藉春閳ь剚銇勯幒宥囶槮濠⒀屽灡缁绘盯宕楅懖鈺侇潷缂備胶绮粙鎴︻敊韫囨侗鏁婇柤濮愬€楀▔璺ㄧ磽閸屾瑨鍏屽┑顔藉▕瀹曪繝骞庨悾灞界ウ闂佸搫绉查崝宥囩礊閸ヮ剚鐓曢柟鐐殔閸熶即鎮″Δ鈧埞鎴︽偐閹颁礁鏅遍梺鍝ュУ閻楁洜鍙呴梺闈浤涢崟鍨稐闂備礁婀遍崕銈夈€冮崱娑樼９闂佸灝顑冩禍婊堟煙閸濆嫮啸闁稿繐鐭傞弻娑㈠箣椤栨粎锛熺紓浣介哺閹稿骞忛崨鏉戠闁告瑥锛嶉妸鈺傗拺缁绢厼鎳忛悵顏堟煙閻熺増鍠橀柕鍡曠椤繈鎳滈崹顐ｇ彸濠电姰鍨煎▔娑㈡晝閿曞偆鏁婇柛娑樼摠閳锋垿鏌涜箛娑欙紵闁煎壊浜弻娑欐償閵堝鎽电紓浣稿€哥粔鎾€﹂妸鈺侀唶闁绘柨寮剁€氳偐绱撻崒姘偓鐑芥倿閿曚焦鎳岄梺璇茬箰濞存岸宕㈤悾灞惧床婵炴垶鍩冮崑鎾绘偨濞堣法鍔搁梺鍛娒悺銊╁Φ閸曨垰顫呴柨娑樺閸ｎ參鎮楅崹顐ｇ凡閻庢凹鍘鹃幑銏犫攽閸ャ劌鍔呴梺闈涚箳婵嘲危閿濆鈷掗柛灞剧懅缁愭梹绻涙担鍐叉处閸嬪鏌涢埄鍐︿簵婵炴垶菤閺嬪酣鏌熼幆褏孝妞わ箓娼ч悾鐑藉础閻戝棙瀵岀紓鍌欑劍钃遍柡澶婃啞娣囧﹪鎮欓鍕ㄥ亾閺嶎厼钃熼柕濞炬櫅缁犺銇勯幇鍓佺暠闁绘帒鐏氶妵鍕箳瀹ュ牆鍘＄紓浣哄Х閸嬬偤銆冮妷鈺傚€烽柟缁樺笚濞堫參鏌х紒妯煎⒌闁哄本绋戦…銊╁焵椤掑嫭鍊块柨鏇炲€搁悿楣冩煃鏉炴媽顔夐柡鈧禒瀣厽闁归偊鍓涢幗鐘电磽瀹ュ棗鐏ラ摶鏍煟濡搫绾ч柟鎻掓憸缁辨帗娼忛埡浣锋勃缂備胶绮换鍫濈暦閹偊妲鹃梺浼欑稻濡炶棄顫忓ú顏勭闁告瑥顦伴崕鎾绘⒑閻熸澘鏆辩紒缁樏悾鐑藉箛椤斿墽锛滃┑鈽嗗灠濠€杈╃不濮橆剦娓婚柕鍫濇婢ь剛绱掗鎯р枅鐎规洘娲熼幃鈺冪磼濡厧甯楅柣鐔哥矋缁挸鐣峰鍐炬僵閻犻缚娅ｉˇ顔尖攽閻愬弶顥為柟绋挎憸缁寮介鐔蜂壕闁荤喐婢橀顓㈡煕鐎ｎ亷韬柟顕嗙節婵＄兘鍩￠崒婊冨箞婵犵數鍋為崹闈涚暦椤掆偓椤曪絾銈ｉ崘鈺冨幐闂佺硶妾ч弲娑橆嚕椤旇姤鍙忓┑鐘叉噺椤忕姷绱掓潏銊ョ瑨閾伙絿绱撴担濮戭亪宕滈悽鐢电＝闁稿本鐟ㄩ崗宀€绱掗鍛仯闁瑰箍鍨藉畷濂稿Ψ閿曗偓娴犳帒顪冮妶鍡樷拻闁哄拋鍋嗗褔鍩€椤掑嫭鈷戠紒瀣儥閸庢挸鈹戦鍝勨偓鏇＄亱闂佸憡鍔戦崝搴ㄥ汲閿曞倹鐓忓┑鐐靛亾濞呮捇鏌℃担闈╄含闁哄本绋栫粻娑橆潩椤戞寧鐫忛梻浣告惈椤戝倿宕戦幘缁樷拻闁稿本鑹鹃埀顒佹倐瀹曟劖顦版惔銏╁仺濠殿喗锕╅崜锕傛倿閻ｅ本鍠愰柡鍐ㄥ€诲Σ鐢告⒒娓氣偓閳ь剛鍋涢懟顖涙櫠閹殿喚纾奸弶鍫涘妼濞搭噣鏌熼瑙勬珚闁圭厧缍婇、鏇㈠閵忊晜娈规繝纰夌磿閸嬫垿宕愰妶澶婄婵炲棙鎸婚弲婵囥亜韫囨挾澧曠紒鐘崇墬缁绘繃绻濋崒婊冾杸闂佹悶鍊曠粔褰掑蓟閺囩喎绶為柛顐ｇ箘娴犻箖鏌ｆ惔銏ｅ闁靛牏顭堥～蹇曠磼濡顎撻梺鎯х箳閹虫挾绮敓鐘斥拺闁革富鍙庨悞楣冩煙閼恒儳鐭掗柛鈹惧亾濡炪倖宸婚崑鎾绘煟韫囨梹鐨戦柡渚囧櫍楠炴帒螖閳ь剟鎮″┑瀣厵闁告挆鍛闂佺顑嗛崝娆撳箖闁垮濯撮柣銈庡灠閸橈繝姊虹涵鍛彧闁圭懓娲崹楣冩晝閸屾鈺呮煃鏉炴媽鍏屾い锔芥緲椤啴濡堕崱妯烘殫婵犳鍠氶崗妯侯嚕閹惰棄围濠㈣泛顑傞幏娲⒑绾拋娼愰柛鏃€娲熷畷銉ㄣ亹閹烘挾鍘遍柣搴秵閸撴瑦绂掗崡鐐╂斀闁挎稑瀚弳顒傗偓瑙勬礈閸犳牠銆佸☉姗嗘僵閺夊牃鏅滃鎴︽⒒閸屾瑧顦﹀鐟帮躬瀹曟垿宕ㄩ鐓庢惈椤粓鍩€椤掑嫨鈧線骞橀幇浣告倯婵犮垼娉涢鍌炲箯閾忓湱纾藉ù锝呭閸庢劖銇勯幋鐐垫噰鐎规洘娲濈粻娑㈠棘鐠佸磭鐩庨梻浣烘嚀閻°劎鎹㈠鍛傜喖鍩€椤掑嫭鍊垫繛鍫濈仢閺嬶附銇勯弴鍡楁搐閻撯€愁熆閼搁潧濮囨い顐㈡嚇閺屽秹宕崟顐熷亾閻熸壋鏋嶉柛銉墯閳锋帒銆掑锝呬壕濠电偘鍖犻崶銊ヤ罕闂佺硶鍓濈粙鎴犵不閻樿崵鍙撻柛銉ｅ妿閳洟鏌涙繝鍥ㄦ锭閾绘牠鏌ㄥ┑鍡樺櫣闁哄棛鍋ら弻宥夋煥鐎ｎ亞浼岄梺鍝勬湰閻╊垶寮崒婊勫珰闁圭粯甯為鎰版⒒娴ｅ憡鍟為拑杈ㄧ箾閸忚偐鎳囨繝鈧担绯曟斀闁绘顕滃銉╂煕閻旇泛宓嗙€规洘妞介弫鎰板炊閿濆懍澹曞┑鐐茬墕閻忔繈寮稿☉娆嶄簻妞ゆ挶鍨洪弫鍗炃庨崶褝韬鐐寸墬閹峰懘鎳栧┑鍕闁哄本娲濈粻娑欑節閸愮偓缍夋繝鐢靛仜閸氬顫忛崷顓熷床婵犻潧鐗嗛弸鍫熶繆椤栨稒銇熸繛鍏煎灴濮婃椽骞栭悙鎻掝瀴婵犵數鍋涢敃銈夋偩閻戣棄顫呴柨婵嗗閳诲矂鏌ｉ悩杈劅闁搞劌鐖煎璇差吋閸偅顎囬梻浣告啞閹歌顫濋妸鈺佺闁靛繒濮弨浠嬫倵閿濆骸浜滃ù鐘虫そ濮婂宕掑鍗烆杸闂佸憡宸婚崑鎾斥攽閻愭潙鐏熼柛銊︽そ閹繝寮撮姀鈥斥偓鐢告煥濠靛棝顎楀ù婊勭箞閹绠涙惔鈥崇ギ闂佸搫鏈ú鐔风暦閹偊妲诲銈呮禋閸樹粙銆冮妷鈺傚€烽弶鍫熷礃閳ь剝娉曢埀顒冾潐濞测晝鎹㈠┑瀣畺婵炲棙鎸哥粈鍌炴煕濞戞﹫宸ュù婊勭箘缁辨捇宕掑顑藉亾閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌ょ粙璺ㄤ粵闁告瑥绻橀弻娑⑩€﹂幋婵呯敖缂備胶濮甸崕鎶藉煘閹达附鍋愰柛娆忣槹閹瑩姊洪挊澶婃殶闁哥姵鐗犲濠氭晲婢跺﹥顥濋梺鍓茬厛閸犳宕愰鐐粹拺缂佸灏呴崝鐔虹磼椤曞懎鐏＄紒宀冮哺缁绘繈宕堕懜鍨珫婵犵數濞€濞佳兠归崒娑氼浄闁冲搫鍊荤粻楣冩倵閻㈡鐒鹃棄瀣⒑缁嬫鍎愰柛鏃€顨呭嵄闁圭増婢樼粻铏繆閵堝嫮顦﹂柍褜鍓熼弨閬嶅Φ閸曨垰绠崇€广儱鐗嗛崢锟犳⒑閸涘﹤鐓€缂佽鲸娲滃Σ鎰板箻鐎涙ê顎撻梺鍛婃尭瀵墎鐟х紓鍌氬€搁崐鍝ョ矓閹绢喗鏅濋柕鍫濐樈閺佸鏌ㄥ┑鍡橆棤缂佲檧鍋撴繝娈垮枟閿曗晠宕曢悽绋跨厸闁告侗鍠氶崢顏呯節閵忥絾纭鹃柣妤€妫濋幃姗€顢旈崨顐＄盎濡炪倖鍔戦崹璇茬摥闂備礁鐤囬～澶愬垂閸фぜ鈧礁鈻庨幋鐘烩攺闁诲函缍嗛崑鍡涘汲閵夛妇绡€缁剧増蓱椤﹪鏌涚€ｎ亝鍤囬柟顔惧厴閸╋繝宕ㄩ鐔蜂壕濞撴埃鍋撴鐐搭焽閹风娀骞撻幒鏇楀亾閸喒鏀介柣鎰级椤ョ偤鏌涢妸銉хШ闁轰礁绉撮埞鎴﹀炊閳哄喚鍟庨梺鍝勵槸閻楀棙鏅堕悾灞稿亾娴ｅ啫浜圭紒杈ㄥ浮椤㈡瑩鎮剧仦鎯ф珰闁诲氦顫夊ú锕傚磻婵犲洤绠圭憸鐗堝笒閹硅埖銇勯幘璺侯€撶紒杈ㄧ叀濮婃椽鎳￠妶鍛呫垺绻涚拠褏鐣抽柕鍥ㄥ姍瀹曟﹢顢欓崗鑹扮发婵犵數鍋涘Λ娆撳礉閹寸偟顩叉繝濠傚娴滄粓鏌熼幑鎰【濞寸媴濡囬埀顒傛嚀閹诧紕鎹㈤崼銉ヨ摕闁绘柨澹婂Σ褰掑箹缁懓顕￠柕蹇嬪€栭ˉ鍡楊熆閼搁潧濮堥柛濠勬暬閺屻劌鈹戦崱娑扁偓妤€霉濠婂懎浠遍柡灞界Х椤т線鏌涢幘瀵告噰闁炽儻绠戦悾锟犳焽閿旂晫绋侀梻浣瑰劤缁绘锝炴径鎰獥闁糕剝绋掗悡鏇㈡煛閸ャ儱濡煎ù婊呭仦閵囧嫰鏁傞崫鍕瀳闂佸疇顫夐崹鍧楀箖濞嗘挸鐭楀璺猴攻椤忊剝绻濋悽闈涗哗閻忓繑鐟╁畷浼村冀椤撶倣銉ャ€掑锝呬壕閻庢鍣崳锝呯暦婵傚憡鍋勯悘蹇庣娴滈箖鏌ｉ幋锝呅撻柣鎾跺枛閺岀喓鈧數顭堥埀顒佸灥椤繄鎹勯搹璇″數婵犵數鍋涘Ο濠冪濠靛鐓曢柟杈鹃檮閻撴洘绻涢幋鐑囧叕闁告梻鍠栭弻宥夋寠婢舵ɑ鈻堥梺鍝勮閸旀垵顕ｉ弶鎳崇喓鎮伴垾鍏呭闂佸搫绋侀崢鑲╃不閻愮儤鐓忓┑鐐靛亾濞呭懘鏌ｉ幇顒婅含闁哄苯绉瑰畷顐﹀礋椤愶絾顔嶉梻浣瑰缁嬫帞鍒掑鍥ㄥ床婵犻潧顑呴～鍛存煥濠靛棙顥犻柕鍡樺姍濮婃椽骞愭惔銏⑩敍缂備胶濮甸幑鍥ь嚕椤愩埄鍚嬮柛婊€绀侀幃鎴炵節閵忥絾纭炬い鎴濇喘瀹曘垽妫冨ù铏閹晠妫冨☉妤冩崟闂備浇顕х换鎴犳崲閸儱绠栨慨妞诲亾闁糕晪绻濆畷鎺懳熼崷顓犳晨濠碉紕鍋戦崐鏍礉瑜忓濠勬崉閵娧傜瑝闂佸搫绉查崝搴ｇ不妤ｅ啯鐓熼柟鎹愭珪閹癸絿绱掗悩渚殭闁宠鍨块崺鍕礃閳轰讲鍋撻幇顑芥斀闁挎稑瀚悞鎸庛亜閿旂晫鍙€婵﹥妞介獮蹇撶暆閸曨偅鎳欑紓鍌欒兌缁垳鎹㈤崼銉ョ畺濞寸姴顑嗛ˉ鍫熺箾閹寸儐娈旈柟钘夘儔濮婄粯鎷呯粵瀣秷濠碉紕鍋炵€笛呯矉瀹ュ鍊烽柛顭戝亽濞肩喖姊虹紒妯哄妞ゆ洦鍙冮幃鐤亹閹烘挾鍘撻悷婊勭矒瀹曟粌鈹戦崰顔界洴婵偓闁靛牆妫岄幏娲⒑闂堚晛鐦滈柛妯哄悑缁傚秹鎮欓璺ㄧ畾闂佺粯鍔曞鍫曞磻閵壯€鍋撳▓鍨珮闁稿锕ら悾鐑藉箳濡や礁鈧兘鏌ょ喊鍗炲⒋闁绘稒鎹囧缁樻媴缁嬫妫岄梺绋款儏閹虫劗鍒掑▎鎾崇闁绘劖澹嗙粻姘舵煟閻樺厖鑸柛鏂挎捣婢规洘绺介崨濠勫幗濠碘槅鍨伴悘婵嬪触瑜版帗鐓熼柨婵嗘閵囨繈鏌″畝瀣？濞寸媴绠撳畷婊嗩槼闁告帗鐩铏规嫚閳ヨ櫕鐏€闂佸搫鎳愭繛鈧柕鍡曠窔瀵挳濮€閻欌偓濞煎﹪姊虹紒姗堜緵闁稿鎳愰埀顒佹皑閸犳劗鎹㈠┑瀣仺闂傚牊鍒€閻愮儤鐓曢柕濞垮労閻撳吋顨ラ悙璇ц含妤犵偞锚鐓ょ紓浣股戝▍鍫濃攽閻橆喖鐏辨繛澶嬬洴閺佸啴鏁冮崒姘緢?
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
        // WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟扮粻濠氭煕閳规儳浜炬俊鐐€栫敮濠囨嚄閸洖鐓濋柟鍓х帛閻撴盯鏌涘☉鍗炴灓缂佺姵锕㈤弻娑㈠箳閹惧磭鐟ㄩ梺瀹狀嚙闁帮綁鐛Ο铏规殾闁搞儴娉涢弫钘夆攽閻樿尙妫勯柡澶婄氨閸嬫捇骞囬弶璺紱闂佽鍎虫晶搴ｅ婵傚憡鐓冪憸婊堝礈濮橆剦鍤楅柛鏇ㄥ幐閸嬫捇鏁愰崒娑欑彇缂備胶濮甸懝楣冣€旈崘顔嘉ч柛鈩冪懃椤呯磽娴ｈ櫣甯涢柛鏃€鐗犻妴鍐Ψ閵娧呮澑闂佺懓褰為懗鍫曞箠閹剧儵鈧箓濡搁埡浣侯槰闂佸啿鎼€氼噣銆侀崨瀛樷拻濞撴埃鍋撻柍褜鍓涢崑娑㈡嚐椤栨稒娅犳い鏂垮⒔绾惧ジ鏌ｅΟ鍨毢閺佸牓鎮楃憴鍕婵炶尙鍠栧濠氬幢濡ゅ啯娈奸梺闈涱槶閸庢煡宕板Ο姹囦簻闁靛骏绱曢埥澶嬨亜椤撴粌濮傜€规洜鍠栭、鏇㈠閻樻鍞归梻鍌氬€搁崐椋庢濮樿泛鐒垫い鎺戝€告禒婊堟煠濞茶鐏￠柡鍛閳ь剚绋掕摫闁告瑥绻愰埞鎴︽偐閹绘帗娈查梺闈涙处缁诲嫰鍩€椤掑喚娼愭繛璇х畵瀹曟垶绻濋崒婊勬闂佺粯姊婚埛鍫ュ极閸℃稒鐓曢悘鐐扮畽椤忓懐顩查悷娆忓缁♀偓闂佹眹鍨藉褎绂掑鍫熺厽閹烘娊宕濇惔銊ョ闁圭儤鎸婚崕鐔兼煏婵炲灝鍔ら柨娑欑洴濮婅櫣绱掑Ο鑽ゅ弳闂佸湱鈷堥崑濠傜暦閻熸壋鏀介悗锝庡亞閸欏棗鈹戦悩缁樻锭婵☆偅鐟╁畷顖炲箳濡や胶鍘搁梺鍛婂姂閸斿孩鏅堕弴鐘电＜闁逞屽墴瀹曟﹢鍩炴径鍝ョ泿闂傚鍋勫ú锕傚箰婵犳碍鏅柡鍐ㄥ€荤壕?Sniroute 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬪灝鐎俊銈忕到閸燁偆绮诲☉妯忓綊鏁愰崨顔跨缂備礁顑勯懗鍓佹閹捐纾兼慨姗嗗厴閸嬫捇鎮滈懞銉モ偓鍧楁煥閺囨氨鍔嶉柟鍐茬焸濮婄粯鎷呴崨濠傛殘闂佽崵鍠嗛崕鎶藉箲閵忋倕绠虫俊銈傚亾闁绘帒鐏氶妵鍕箳閹存繃鐏撳┑鐐插悑閸旀牜鎹㈠☉銏犵煑濠㈣泛鑻埅褰掓煟閹惧崬鈧繈寮婚弴鐔虹闁绘劦鍓氶悵鏃傜磼缂併垹骞愰柛瀣尰缁绘繂鈻撻崹顔界亶濡炪値鍘奸悧鎾崇暦濞差亜鍐€妞ゆ挾鍠庢禍妤€鈹戦悙鍙夘棡闁搞劌婀辨竟鏇㈡寠婢规繂缍婇弫鎰緞鐎ｎ偊鏁梻浣筋嚙缁诲棝寮查悩璇茶摕闁挎稑瀚ч崑鎾绘晲鎼粹€茬敖闂侀潧妫欑敮鐐垫閹烘挻缍囬柕濞垮劜鐠囩偤姊虹拠鈥虫珮濞存粍绮撻崺銏℃償閵堝洨鏉搁梺鍝勫€归娆徫ｉ柆宥嗏拻濞达綀娅ｇ敮娑㈡煙閸涘﹥鍊愮€规洦鍨堕獮姗€顢欓懝鐗堟啺闂備礁鎼ú銊╁窗鎼达絾顐介柣鎰嚟缁犻箖鏌熺€电浠﹂柣銊﹀灴閺岋繝宕卞▎寰絿绱掔紒妯肩疄闁糕晜鐩崺鈧い鎺戝€峰▽顏堟煟閿濆懎顨欐俊妤佹崌濮婄粯鎷呴崨濠冨創闂佹椿鍘奸ˇ閬嶅箲閵忕姭鏀介柛鈩冪懄濞堥箖姊洪崨濠庢畼闁稿缍侀幃銏ゅ礂閻撳孩顓奸梻渚€娼ч悧鍡涘箠鎼粹檧鏋旈柕澶堝劗閺€浠嬫煥濞戞ê顏╁ù婊冦偢閺屾稒绻涢崹顔瑰亾濠靛棛鏆﹂柕蹇嬪€栭崑鍌炲箹閹碱厼鏋ら柛鏃傚厴濮婅櫣鎹勯妸銉︾亖婵犳鍠栭顓犲垝婵犳碍鍋愮€规洖娲ㄩ惁鍫熺節閻㈤潧孝闁稿﹦绮弲璺衡槈閵忥紕鍘遍柣搴祷閸斿本鎱ㄩ崒娑欏弿濠电姴鍋嗛悡鑲┾偓瑙勬礀閵堟悂骞冮姀銏″仒闁斥晛鍟版禍鏍⒒閸屾瑧顦﹂柟璇х節閹虫繃銈ｉ崘銊э紮闂佸壊鐓堥崳顔碱焽閺嶎偆纾藉ù锝堝亗閹寸偛鍨旈柟缁㈠枟閸嬧剝绻涢崱妤冪妞ゅ繆鏅涢湁闁绘﹩鍋呭▍濠冩叏婵犲懏顏犵紒顔界懃閳诲酣宕滆婵ジ姊绘担渚劸闁挎洏鍎抽幑銏ゅ磼閻愭潙浠奸梺缁樺灱婵倝寮查幓鎺濈唵閻犺櫣灏ㄩ崝鐔搞亜閿斿搫濮傛慨濠冩そ濡啫霉閵堝棛娲寸€规洜鍠栭、妤佸緞婵犲啯鐝梻鍌氬€搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫤鐎规洘鐓￠弻娑樼暆閳ь剟宕愬☉銏犵倞闁靛鍎洪崵銈夋⒑閸濆嫷妲归柛銊ョ秺瀵槒顦规慨濠冩そ瀹曨偊宕熼鍛晧闂備胶顭堢€涒晜绻涙繝鍌滄殾闁挎繂顦粻鑽ょ磽娴ｈ鐒介柛姗€浜跺娲濞戣鲸顎嗙紓浣藉蔼婵倖绌辨繝鍥ㄥ仼鐎光偓閳ь剛澹曟總鍛婄厓鐟滄粓宕滈悢椋庢殾濠靛倸鎲￠崑鍕磽閸垹啸闁烩晛閰ｅ缁樻媴閻熼偊鍤嬪┑鈽嗗亗缁舵碍淇婇幘顔肩劦妞ゆ帒瀚悡鏇㈡倵閿濆骸浜濈€规洖鐭傞弻娑㈠煛鐎ｎ剛鐦堥梺璇″枟婵炲﹪骞嗛崶銊ょ箚濠?YouTube/Play 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鈧鍝勑ч崶褉鍋撻妶澶婄獥婵°倕鎳庢闂佸憡娲﹂崹浼村礃閳ь剙顪冮妶鍡楀Е婵犫懇鍋撶紓渚囧枛閻栫厧顫忓ú顏勭闁绘劖褰冮‖鍫ユ⒑缁嬪尅宸ユ繛灏栤偓鎰佸殨閻犲洦绁村Σ鍫熺節婵犲倸顒㈠鍥р攽閿涘嫬浜奸柛濠冨灴瀹曟繂鐣濋崟顐ょ枃闂佽宕橀崺鏍х暦閸欏鍙忔慨妤€妫楅崢鎾煛閳ь剚绂掔€ｎ偆鍘藉┑顔筋殔濡寮稿☉銏＄厽闊洦绋愰幉楣冩煛鐏炲墽娲撮柛鈺嬬節瀹曟帒顭ㄩ崘锝嗗闂佽姘﹂～澶娒哄Ο鍏煎床闁割偅绻嶉崵鏇灻归悩宸剾闁轰礁娲弻锝夊箛椤曞懏娈扮紓渚€浜舵禍璺侯潖閾忚瀚氶柟缁樺笒濮ｆ劗绱撻崒姘毙㈡俊顐ｇ箓閻ｇ兘寮剁拠鐐瀹曘劑顢欓梻瀵告殫闂傚倷绀侀幖顐⒚洪妸鈺佺獥闁糕剝顭囬々鍙夌節婵犲倻澧涢柣鎾寸懃闇夐柣鎾虫捣閹界姵绻涢幊宄板幘閻熼偊鐓ラ柛鏇ㄥ幘閻撯偓婵＄偑鍊ら崣鈧柛搴☆煼钘濋柛鎰▕閻斿棝鏌ｉ悢鍝勵暭妞ゃ儳濮垫穱濠囧矗婢跺﹤顫掑Δ鐘靛仦鐢帟鐏冮梺閫炲苯澧撮柟顔惧仱瀹曞綊顢曢悩杈╃泿闂備礁婀遍崕銈夊垂閻㈠壊鏁傞柍鍝勫亞濞堜粙鏌ｉ幇鍏哥盎闁诲浚浜炵槐鎺旂磼濡吋鍒涘Δ鐘靛仜椤戝骞冨▎鎾冲瀭妞ゆ劑鍨洪悵顕€姊?
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

    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝曢梻浣藉Г閿氭い锔诲枤缁辨棃寮撮姀鈾€鎷绘繛杈剧到閹诧繝宕悙鐑樼厱闁哄啯鎸鹃悾鐢碘偓瑙勬磻閸楀啿顕ｆ禒瀣垫晝闁靛繆鏅滈ˉ鈥斥攽閻樺灚鏆╁┑顔炬暩閸犲﹤顓兼径瀣簵濠电偛妫欓幐濠氭偂濞嗘挻鍋℃繛鍡楃箰椤忊晛鈹戦鍡欑暤闁哄本鐩俊鍫曞幢濞嗘劕娅樼紓鍌欐祰妞村摜鏁敓鐘茬畺闁冲搫鎳忛崵鎴炪亜閹烘垵鈧劙濡搁埡鍌楁嫼闁荤姴娲犻埀顒冩珪閻忓秴顪冮妶鍐ㄥ闁硅櫕锕㈠畷娲Ψ閿曗偓缁剁偛鈹戦悙闈涗壕闁诲孩妞藉娲箚瑜庣粋瀣煕鐎ｎ亜顏柟顔斤耿閸╋繝宕橀鍡床婵＄偑鍊栧Λ渚€宕戦幇顓熸珷闁靛繈鍊栭悡鍐喐濠婂牆绀堥柕濞у懐顦梺绯曞墲缁嬫垹绮ｅΔ浣风箚闁靛牆鎳忛崳娲煕鐎ｎ亜鈧潡寮婚敓鐘茬倞闁靛鍎虫禒鑲╃磽娴ｅ搫鞋妞ゎ偄顦遍幑銏犫槈閵忊剝娅滈柟鑲╄ˉ閳ь剚鍓氬濠氭⒒娴ｇ瓔鍤欐繛瀛樼缁傚秹顢旈崼婵堢暫濠碘槅鍨靛▔鏇㈡偪閳ь剟姊洪棃娑崇础闁告侗鍠楅鎾绘⒒閸屾瑧顦﹂柟纰卞亰閹绺界粙璺ㄧ崶闂佸搫绋侀崢褰掑焵椤戣法顦﹂柍钘夘槸椤粓宕卞Ο鍝勫帪濠碉紕鍋戦崐鏍暜婵犲洦鍊块柨鏇炲亞閺佸﹪鏌熼悜姗嗘畷闁绘挾鍠栭獮鏍庨鈧埀顑惧€曢…鍥箛椤撶姷顔曢梺鍛婄懃椤﹂亶鎯岄幒鏂哄亾鐟欏嫭纾婚柛妤€鍟块锝嗙鐎ｅ灚鏅ｅ┑鐘欏嫬鍔ゅù婊勫劤闇夐柨婵嗘噹椤ュ繑绻涚粭鍝勫闁哄苯绉烽¨渚€鏌涢幘璺烘灈闁绘侗鍣ｅ畷姗€鈥﹂幋鐘插汲闂備礁澹婇崑鍛崲閸岀偞鍤€闁割煈鍠掗弨浠嬫煃閽樺顥滈柣蹇嬪劤閳ь剝顫夊ú鐔奉焽瑜嶅畵鍕攽椤斿浠滈柛瀣尰閹便劍绻濋崒銈囧悑閻庤娲樼敮鎺楋綖濠靛鏁勯柦妯侯槷婢规洘淇婇悙宸剰婵炲鍏橀幆宀勫幢濡炲皷鍋撻幒鎴僵闁挎繂鎳嶆竟鏇熶繆閵堝洤啸闁稿鍋ら幃褔鎮╁Ч鍥ｅ亾娓氣偓瀵潙顫濇潏鈹垽姊绘担瑙勫仩闁稿孩妞藉畷婊堟晝閸屾碍鐎悗骞垮劚椤︿即鎮￠悢鍏肩厵闂侇叏绠戦悘鐘绘煟閵婏附銇濋柡宀嬬磿娴狅箓鎮剧仦瑙ｅ徍婵犳鍠栭敃銊モ枍閿濆應妲堥柣銏㈩焾瀹告繃銇勯弬鎯у珟闁逞屽墯閸旀洟鈥旈崘顔嘉ч幖绮光偓鑼嚬婵犵數鍋犵亸娆撳窗閺嵮呮殾婵炲樊浜滈悞鍨亜閹哄秹妾峰ù婊勭矒閺岀喐娼忛崜褏蓱缂佺虎鍘兼晶搴ｆ閹烘绠涙い鎾跺暱閹风懓顪冮妶鍐ㄥΩ闁稿海鏁婚獮鍐ㄢ枎韫囷絽鏅犲銈嗘⒒閸樠勭珶鎼淬劍鈷掑ù锝堝Г閵嗗啰绱掗埀顒佺瑹閳ь剙鐣烽幇鏉块敜婵°倐鍋撶紒鐘靛█閺岋綁骞囬浣瑰創闁哥儐鍨跺娲箰鎼粹懇鎷归梺绋块閸熷潡顢氬▎鎾村€绘俊顖濆亹閻﹀牓姊哄Ч鍥х伈婵炰匠鍕ⅰ濠碉紕鍋戦崐鏍ь潖瑜版帗鍋嬮柣妯垮皺閺嗭箓鏌℃径搴殾闁哄啫鐗嗗婵囥亜閺冨洤袚闁绘縿鍔戝缁樻媴閽樺鎯為梺鍝ュУ閸旀宕氶幒鎴悑闁告粈鑳堕崬闈涒攽椤斿浠滈柛瀣崌閹稿﹤鈹戠€ｎ偆鍘卞┑掳鍊曢崯顐﹀焵椤掑﹦绉柡浣瑰姈瀵板嫭绻濋崟顓熸緫濠碉紕鍋戦崐鏍垂閻旂厧鐤炬繝濠傚悩濞戙垹绀冮柕濞у嫭顔曢梻渚€娼ц墝闁哄應鏅犲顐ｇ節閸ャ劎鍘靛銈嗙墪濡宕导鎼晩闁圭儤姊荤壕浠嬫煕鐏炴崘澹橀柍褜鍓涢崗姗€骞婂Δ鍛唶闁哄洨鍠愰崕顏堟煙閸忚偐鏆橀柛鏂跨焸瀹曟帡濡搁妷銏℃杸闂佺鏈划鐘绘倿閸撗呯＜闁绘﹢娼ч埢鍫燁殽閻愬澧繛鐓庣箻瀹曟粏顦撮拑杈ㄤ繆閻愵亜鈧倝宕戦崨顒兼椽鎮㈤悡搴ゆ憰闂侀潧艌閺呮稑螞濮椻偓閺屾盯鈥﹂幋婵囩亐闂佸湱顭堥崯鎵閹烘鍊锋い鎺嗗亾闁告柣鍊濋弻娑橆潩椤掑鍓辩紓浣哥焷妞村摜鎹㈠┑瀣倞妞ゎ剦鍎搁崶銊у幈闁诲繒鍋涙晶浠嬫偂椤栨凹鐔嗛悷娆忓缁€鍐ㄇ庨崶褝韬柟顔界懇椤㈡棃宕熼妸銉ゅ閻熸粌绻掗崚鎺撶節濮橆剛顔掗梺褰掝暒閼冲爼宕濋悜鑺ュ仭婵犲﹤瀚ˉ鍫燁殽閻愭彃鏆為柕鍫秮瀹曟﹢鍩℃担鎻掍壕濠电姵纰嶉悡鐘绘煙椤撶喎绗掗柛鏃€绮嶇换娑㈠川椤斿墽鐓佺紓浣虹帛缁诲牊鎱ㄩ埀顒勬煥濠靛棙鍣介柛姗嗗亰閺岋箓宕橀缁樺枤闂佸搫鐭夌换婵嗙暦椤忓懏濯寸紒瀣儥濞煎姊绘担鍝ユ瀮妞ゎ偄顦靛畷褰掑锤濡も偓缁犳牠鏌嶉崫鍕殲閻庢碍姘ㄩ幉绋款吋閸℃鍤ら梺瑙勫婢ф鍩涢幋锔界厱婵犻潧瀚崝姘舵煛閸℃稐鎲鹃柡宀嬬節瀹曘劑顢涘В瑁ゅ灲閺岋綁鏁愰崶褍骞嬮梺璇″枓閺呮繈骞忛悩璇茬闁圭儤鍨堕弳蹇撯攽閿涘嫬浜奸柛濠冪墵楠炴劖銈ｉ崘銊ф焾闂佺鍕垫當缂佺媭鍨辩换娑橆啅椤旇崵鍑归梺鎶芥敱閸ㄥ潡寮诲☉銏℃櫆閻犲洦褰冪€涳綁姊虹紒妯虹闁稿鎸鹃幑銏犫槈濮橈絽浜鹃柣銏☆問閻掗箖鏌涘Ο缁樺唉闁哄本鐩崺鐐哄垂椤旂厧顫氶柣搴＄仛濠㈡鈧矮鍗抽悰顕€宕堕澶嬫櫍闂佺粯蓱瑜板啰绮荤拠娴嬫斀闁绘ê鐏氶弳鈺呮煕鐎ｎ剙啸濞存粍鎮傞、鏃堝醇椤愶絾娅旈柣鐔哥矋濡啴鐛崘顭戞建闁逞屽墯娣囧﹪鎮滈挊澶屽幐闂佺鏈划宀€鏁ィ鍐┾拻濞达綀顫夐崑鐘绘煕鎼搭喖鐏︾€规洘绻勬禒锕傛倷椤掆偓瀵潡鏌ｈ箛鏇炰粧闁告洘蓱缁傚秴顭ㄩ崼鐔哄弳闂佸搫娲﹂〃鍛閸撗呯＝闁稿本姘ㄥ瓭闂佹寧娲︽禍婊堫敋閿濆棛绡€婵﹩鍘兼禍婊呯磼閻愵剙顎滃瀛樻倐瀵煡鍩￠崨顔规嫼闂佺粯鎸哥€垫帒顭囬悢鍏肩厱濠电姴鍠氬▓婊堟煙椤曞棛绡€妤犵偞顭囬埀顒勬涧閹芥粓鎯侀崼銏㈢＜闁告挆鍐炬毉闂佹寧鑹鹃…鑳槼闁绘搫绻濆濠氭偄绾拌鲸鏅╅梺缁樻尭缁ㄥ爼宕戦幘璇插瀭妞ゆ洖鎳庡▓銊ヮ渻閵堝棗濮х紒鐘冲灩婢规洟宕楅懖鈺冾啎闂佺懓顕崑娑㈠吹椤掍胶绠鹃柡澶嬪灥椤忣參鏌″畝瀣ɑ闁诡垱妫冮、娑橆煥閸涘拑缍佸铏圭矙濞嗘儳鍓遍柣銏╁灲缁绘繂鐣峰ú顏勭劦妞ゆ帊闄嶆禍婊堟煙閸濆嫮啸闁稿繐鑻…鑳槻闁诲繑宀搁獮鍫ュΩ閿斿墽鐦堥梺鍛婃处閸樿偐绮敓鐘斥拺闁荤喐婢樺Σ濠氭煙閾忣個顏堟偩閻戣棄惟闁挎柨澧介惁鍫ユ⒑閸涘﹤濮傞柛鏂挎湰缁旂喎顓奸崶鈺冿紳闂佺鏈懝楣冨焵椤掑嫷妫戠紒顔肩墛缁楃喖鍩€椤掑啯顥ら梻浣告惈椤︿即顢栧▎鎾村€块柤鎭掑劤缁犻箖鏌涢埄鍏狀亪鎮橀妷褉鍋撳▓鍨灍闁绘挴鈧剚娼栫紓浣股戞刊鎾煟閻旂厧浜伴柛銈咁儏閳规垿鎮欓弶鎴犵シ缂備礁顦遍幊鎾绘偩閻戣棄绠ｉ柨鏇楀亾缂佺姴顭烽幃褰掑炊椤忓嫮姣㈠銈冨灩閹虫ê顫忕紒妯肩懝闁搞儜鍐炬交缂傚倷绀侀ˇ閬嶅极婵犳艾绠栭柍銉︽灱閺嬪酣鏌熼幆褏锛嶅Δ鐘叉喘濮婃椽宕滈幓鎺嶇凹缂備浇顕ч崐鍧楀春濞戙垹绫嶉柛顐ゅ枔閸樹粙姊洪幐搴ｇ畵闁绘鎳樺畷銉╊敃閵堝洨锛滈柡澶婄墑閸斿苯霉椤曗偓閺屾盯鍩為幆褌澹曞┑锛勫亼閸婃牜鏁幒鏂哄亾濮樼厧澧扮紒顔肩墢閳ь剨缍嗛崑浣圭濠婂牊鐓涚€广儱鍟俊鍧楁煏閸℃ê娴柡灞剧☉铻ｉ柛婵嗗閸╃偤姊虹€圭媭娼愰柛銊ユ健楠炲啴鍩￠崘顏嗭紲濠碘槅鍨伴…鐑藉极椤忓懐绡€缁剧増蓱椤﹪鏌涚€ｎ亝鍤囬柕鍡楀暞缁绘繈宕堕妷銏犱壕濞撴埃鍋撶€殿噮鍣ｅ畷鎺懳旈埀顒佺閻愵剛绠鹃柟瀵稿剱娴犻箖鏌ゅù瀣珕鐎规洘鐓￠弻娑㈠焺閸愵亖濮囬梺缁樻尰閻╊垶骞冨鈧幃娆愶紣濠靛棙顔勯梻浣告啞濮婂綊鎮烽埡鍛摕闁绘梻鍘ч拑鐔兼煏婢诡垰鎳忓В鍥⒒娴ｅ憡鎯堥柣顓烆槺濡叉劙寮撮悢渚祫闂佹寧娲栭崐鍝ョ矆閸垺鍠愬璺好￠敐澶婇唶婵犲灚鍔栫€靛矂姊洪棃娑氬濡ょ姵鎮傞悰顕€骞嬮敂鐣屽幈闂佸搫鍟犻崑鎾绘煕閵娧勬毈闁诡喗鍎抽悾锟犳焽閿旇棄缂撻梻渚€鈧偛鑻晶鎾煏閸℃洜绐旂€殿噮鍣ｅ畷鐓庘攽閸℃埃鍋撻崸妤佲拺闁告繂瀚婵嗏攽椤旀儳鍘撮挊婵嬫煏婵炲灝鍔楅柡鈧禒瀣厽婵☆垵娅ｆ禒娑㈡煛閸″繑娅呴柍瑙勫灴椤㈡岸宕ㄩ鐐电潉婵°倗濮烽崑娑氭崲濡櫣鏆︽慨妞诲亾妞ゃ垺鐟╅幊鏍煛婵犲唭褔姊婚崒娆戭槮濠㈢懓锕幃锟犲醇閵夈儳锛涢梺绯曞墲缁嬫垹鐥閹叉悂鎮ч崼婵堢懖闂佺粯鎸鹃崰鏍嵁閺嶎灔搴敆閳ь剟鎮炲ú顏呯厱闁绘棃顥撶粻濠氭煛鐏炲墽娲撮柟顔规櫊瀹曟﹢骞撻幒鎾村殘闂佽瀛╅鏍闯椤曗偓瀹曟垿宕ㄩ娑樺簥濠电娀娼уΛ顓烆焽閳哄懏鐓忓璺虹墕閻忊晛霉濠婂嫬鍔ら柕鍡樺笒椤繈鏁愰崨顒€顥氶梺璇插椤旀牠宕伴弴鐘插灊婵炲棙鎸堕埀顑跨椤粓鍩€椤掑嫬绠栭柕蹇嬪€曟导鐘绘煕閺囥劌浜濋弶鍫濈墛缁绘繄鍠婃径宀€锛熼梺绋款儐閸ㄥ灝鐣烽幇鏉垮唨妞ゆ挾鍠庢禍鍗炩攽鎺抽崐鏇㈠箠韫囨稑纾挎俊銈勯檷娴滄粓鏌￠崘銊モ偓鐟扳枍閺囥垺鐓?
    // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮诲☉妯锋婵鐗婇弫楣冩⒑閸涘﹦鎳冪紒缁樺浮閸┾偓妞ゆ巻鍋撶紒鐘茬Ч瀹曟洟鏌嗗畵銉ユ喘椤㈡稑顭ㄩ崨顖滃姸闂佽鍑界紞鍡涘磻娴ｅ湱顩叉繝濠傜墛閻撴瑩鏌熸担鍐╃彧婵炲懎鎳橀弻锝夊箳閺傚灝濡虹紓浣虹帛閻╊垰鐣峰Δ鍛闁兼祴鏅╁Σ瑙勭節濞堝灝鏋涢柨鏇閸掓帡顢涢悙鑼唵闂佸憡绋戦悺銊╁箚閻愮儤鐓熼柣鏃傤焾椤ュ銇勯幇顑惧仮婵﹦绮幏鍛存惞閻熸壆顐兼俊鐐€戦崝灞轿涘┑瀣ラ柛鎰靛枛闁卞洭鏌曟径鍫濆姢妞わ富鍋婂娲捶椤撶偛濡洪梺绯曟櫅閻楁挸鐣烽妷褉鍋撻敐搴℃灍闁绘挻娲熼弻鏇熷緞閸繄浠惧┑鐐叉噹濞差參寮婚敐澶婄閻庢稒锕╁Λ鍐ㄢ攽椤旂》鏀绘俊鐐舵閻ｇ兘濡搁敂鍓х槇闁瑰吋鐣崹濠氬汲椤撱垺鈷掑ù锝勮閻掔偓銇勯幋婵囶棦鐎规洘绻傞埢搴ょ疀閿濆懏顓块梻渚€娼ч…顓熶繆閸ヮ兙鈧懘鎮滈懞銉モ偓鐢告煥濠靛棝顎楀ù婊呭仱閺屾稑顫滈埀顒佺鐠轰警娼栨繛宸簼椤ュ牊绻涢幋锝夊摵閺佸牓姊绘担瑙勫仩闁告柨閰ｅ顐ｇ節濮樺崬绁︽繝鐢靛Т閹虫劙鎮块埀顒勬⒑鐟欏嫬鍔ら柣掳鍔戦幆浣规償閿濆洨锛?ASCII闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖闂備礁鎲￠幐鍡涘川椤旂瓔鍟呴梻鍌欐祰椤曆呪偓娑掓櫊閹虫繃銈ｉ崗顖氭处鐎靛ジ寮堕幊绛圭畵閺屾盯寮撮妸銉т哗缂備胶濮寸壕顓㈠焵椤掆偓缁犲秹宕曢崡鐏绘椽濡堕崱鏇犵畾闂佽姤锚椤︻偊寮ㄦ禒瀣厽闁归偊鍓欑痪褔鏌涢敐鍫燁棄闁宠鍨块、娆撳箚瑜嶉獮瀣⒑缁洘鏉归柛瀣尭椤啴濡堕崱妤€娼戦梺绋款儐閹稿墽妲愰幒妤佸亹闁肩⒈鍎疯閳ь剝顫夊ú妯好洪悢椋庢殾闁跨喓濮甸崐椋庘偓骞垮劚濡盯鍩ユ径鎰厓闁芥ê顦藉Ο鈧梺鎸庣箘閸嬨倝銆侀弽顓熷癄濠㈣泛顦ˉ?U+00A9(婵? 闂?U+00B7(闂? 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵潙螖閳ь剚绂嶉幆褜娓婚柕鍫濈凹缁ㄥ鏌涢悢鍛婄稇闁伙絿鏌夐妵鎰板箳濠靛洦娅旈梻浣告啞娓氭宕归悧鍫熷弿婵炲樊浜濋埛鎺懨归敐鍕劅婵炲吋甯￠弻娑㈠即閻愬吀绮甸梺浼欑到婢у海妲愰幘瀛樺闁圭粯甯婃竟鏇炩攽閻橆喖鐏遍柛鈺傜墵瀹曟繂螖閸涱厾鐓戦梺鐟板⒔缁垶鎮￠弴銏＄厪濠㈣埖绋撻崚鏉库攽闄囧Λ鍕煘閹达富鏁嶆繝闈涙濮规姊洪悷鏉跨骇闁诡喖鍊块獮鍐樄鐎规洜鍘ч埞鎴﹀醇閵忊€虫珯婵犲痉鏉库偓妤佹叏閻戣棄纾婚柟杈剧畱閸ㄥ倿鏌ｉ幋鐑嗙劷闁崇懓绉撮埞鎴︽偐閸欏銈╅梺杞扮缁夌敻骞堥妸銉庣喖宕稿Δ鈧幗鐢告⒑鐠嬪骸鍊荤粣鏃€鎱ㄦ繝鍐┿仢鐎规洦鍋婂畷鐔兼濞戞ê顥庨梻鍌欐祰瀹曠敻宕抽敂鎴掔箚闁搞儺鍓欑粻鏌ユ煏韫囨洖袥闁衡偓娴犲鍊甸柨婵嗙凹閹茬偓绻涢悡搴含婵﹥妞介獮鎰償閿濆洨鏆ゆ繝纰樻閸嬪懘鎯勯娑楃箚闁圭虎鍠栫粻娑㈡煟濡も偓閻楀啴骞忓ú顏呪拺闁革富鍙庨悞楣冩倵濞戞帗娅囧瑙勬礋楠炴牗鎷呴崷顓炲箞濠电姷鏁告慨鎾疮閹殿喚涓嶉柟鐐暘娴滄粓鏌￠崶鏈电敖缂佸鍠氶埀顒侇問閸犳牠鈥﹂柨瀣╃箚闁绘垼妫勫敮閻熸粍绮撳畷顖炲炊瑜夐弨浠嬫煟閹邦垰鐓愮憸鎶婂懐纾奸柤鑹板煐椤ュ鏌ㄩ弴妯虹伈闁哄苯妫楅濂稿川椤撶偘鎲?
    {
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴楠炴﹢鎳犻澶嬓滈梻浣规偠閸斿秶鎹㈤崘顔嘉﹂柛鏇ㄥ灠閸愨偓濡炪倖鍔﹀鈧柡澶樺弮濮婃椽鏌呴悙鑼跺濠⒀屽櫍閺屾盯鎮㈤崨濠勭▏闂佷紮绲块崗妯讳繆濮濆矈妲烽梺绋款儐閹告悂鍩ユ径濞炬瀺妞ゆ挆鍌滃嚬缂備礁鍊哥粔闈涱焽韫囨稑惟鐟滃繘鎯侀崼銉︹拺婵懓娲ら悘鍙夌箾娴ｅ啿鍟伴幗銉╂⒒閸屾瑨鍏屾い顓炵墦椤㈡牠宕ㄩ鍥ㄧ☉閳规垹鈧綆浜為悾?+ 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊為悷婊冪箻瀵娊鏁冮崒娑氬幈濡炪値鍘介崹鍨濠靛鐓曟繛鍡楃箳缁犲鏌″畝鈧崰鏍嵁閸℃稑绾ч柛鐔峰暞閹瑰洭寮诲☉銏犖ч柛娑卞弾娴煎啴姊洪崫鍕拱闁烩晩鍨堕妴渚€寮撮姀鈩冩珳闂佺硶鍓濋悷杈╂閳哄懏鈷掑〒姘ｅ亾闁逞屽墰閸嬫盯鎳熼娑欐珷闁告挆鍛紲闂佺粯顭堝▍鏇犱焊椤撱垺鐓熼柨婵嗩槹閺佽京绱掓潪鎵煓鐎规洏鍔嶇换婵嬪礃閳哄啫绀侀梻鍌氬€风粈渚€骞夐敓鐘茬妞ゆ帒瀚粈瀣亜閹捐泛鏋旂紒瀣╃窔濮婂宕掑顑藉亾閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏犖ч柛鈩冦仦缁剝淇婇悙顏勨偓鏍礉瑜忕划濠氬箣閻樺樊妫滈梺绉嗗嫷娈旂紒鐙欏洦鐓曟い鎰剁悼椤ｅ弶绻涢崨顓熷枠闁哄矉缍侀、姗€鎮欓幖顓燂紗闂備礁鎼張顒勬儎椤栫偛鏄ラ柛鏇ㄥ灠缁€鍐┿亜韫囧海顦︽い顐㈢焸濮婄粯鎷呴搹鐟扮闂佽鐡曢褏鍙呴梺缁樻⒒閸樠囨嫅閻旇　鍋撻獮鍨姎妞わ富鍨堕幃鈥斥槈濡繐缍婇弫鎰板川椤斿吋娈橀梻浣筋嚙缁绘垿骞愰幖浣哥厴闁硅揪闄勯崐鐑芥煠绾板崬澧い銈傚亾濠碉紕鍋戦崐銈夊储閻撳篃娲敇閵忕姷鐣鹃梺閫炲苯澧柕鍥у缁犳盯骞樼捄渚毇缂傚倷鑳舵慨鐢告偂閿熺姴钃熼柣鏃傤焾椤曢亶鎮楀☉娅虫垿鏌ㄩ鐐╂斀闁绘劘灏欐晶閬嶆煕濞嗗繐鏆欐い顐㈢箻閹煎綊宕楁径濠佸闂侀潻瀵岄崢鍏肩墡婵＄偑鍊戦崕閬嵥囨导鏉懳﹂柛鏇ㄥ灡閺呮粓鏌ｉ幇闈涘婵絽鐗撳铏圭磼濡湱绻侀梺鍝ュУ閻楃娀鏁愰悙宸叆闁割偅绻勯崝锕€顪冮妶鍡楀潑闁稿鎹囬弻锝夋晲閸パ冨箣闂佽桨鐒﹂幑鍥箖閳哄懎绀冩繛鏉戭儏娴滈箖鏌ｉ幋锝呅撻柣鎾冲暣瀵爼鎮欓棃娑辨毉濡炪倧绠戦崲鏌モ€︾捄銊﹀枂闁告洦鍓涢ˇ銉╂⒑鐎圭媭娼愰柛銊ユ健楠炲啫鈻庨幘宕囬獓闂佺懓顕慨瀛樻叏閸忚偐绡€闁汇垽娼ф禒褔鏌涘Ο鐘插枤閻掍粙鏌ㄩ悢鍝勑㈤柡鍕╁劦閺屽秷顧侀柛鎾村哺婵＄敻宕熼姘鳖吅闂佹寧绻傚Λ娑㈠Υ婵犲嫮纾藉ù锝囨嚀婵牊鎱ㄦ繝鍌滅Ш闁靛棗鍟存俊鐑藉煛閸屾埃鍋撴搴樺亾閻熸澘顥忛柛鐘崇墵瀹曟粓宕奸弴鐔叉嫼闁哄鍋炴竟鍡浰囬敃鍌涚厱濠电姴鍊归崑銉︻殽閻愯韬柟顔界懇瀹曨偊宕熸惔娑樺闁哄被鍔戝顕€鍩€椤掑嫬纾块柛鎰▕閸ゅ牓鏌ｉ幋鐑嗙劷缂佲檧鍋撻梻浣圭湽閸ㄨ棄顭囪閻楀孩淇婇悙顏勨偓鏍ь潖瑜版帗鍋嬮柣妯垮皺閺嗭箓鏌℃径搴殾闁哄啫鐗嗗婵囥亜閺冨洤袚闁绘繃鐗犲缁樻媴缁嬫寧姣愰梺鍦拡閸嬪﹪鐛繝鍐╁劅闁挎繂鎷嬪Λ婊勭節閻㈤潧校缁炬澘绉归幃锟犲即閵忥紕鍘撻梺瀹犳〃缁€渚€寮抽弴鐘电＜闁割偅娲戦幉楣冩煛鐏炵澧查柟宄版噽缁瑦鎯旈幘鍏呭闂佸壊鐓堥崳顔碱焽閺嶃劎绠剧€瑰壊鍠曢幉楣冩煛娴ｅ憡顥㈤柡灞界Х椤т線鏌涢幘瀵搞€掗柛鎺撳浮瀵噣宕奸悢铚傜紦缂傚倷绀侀鍫濃枖閺囥垺鏅繛鎴欏灪閳锋垹绱撴担鍏夋（妞ゅ繐妫楅ˉ姘亜閹捐泛啸濞戞挸绉磋灃闁挎繂鎳庨弳鐐烘煟閹惧鎳囬柟顔筋殜閹棄鈻撶捄銊ュЪ闂備礁鎲￠幑鍥ㄧ閸洖钃熼柨婵嗩槸缁犺櫕淇婇妶鍛殶闁哄棭鍋婂铏规嫚閳ヨ櫕鐏嶅銈冨妼閹虫﹢鏁愰悙娴嬫斀閻庯絽鐏氶弲婵嬫⒑闂堟稓绠冲┑顔惧厴婵″爼顢氶埀顒€顫忕紒妯诲闁告稑锕ラ崕鎾绘⒑閸濆嫮澧遍柛鎾寸懅閸欏懘姊虹紒妯活棃妞ゃ儲鎹囧鎶芥晝閸屾稓鍘靛銈嗙墱閸嬫稒绂嶉悧鍫滅箚缂備緡婢€闁垱鎱ㄦ繝鍕笡缂佺粯绻冮幆鏃堝灳閺傘儲瀚介梻鍌欑閹诧繝骞愮拠鑼殕闁归棿鑳跺畵渚€鏌涢鐘插姎閹喖姊洪幐搴㈢叆濠⒀傜矙閹線宕奸妷锔规嫼闂佸憡绋戦敃锕傛儗閸℃稒鐓熼柟铏瑰仜椤╊剛绱掓潏鈺佷槐鐎规洖銈告俊鐑芥晝閳ь剟鍩€椤掑倹鏆柡灞剧洴閳ワ箓骞嬪┑鍛嚬缂傚倷娴囬崺鏍偂閿熺姴绠栨俊銈傚亾妞ゎ偅绻堥、姗€鎮㈤崫鍕闂傚倷妞掔槐顔惧緤閸ф绀傛慨妞诲亾闁靛棔绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶顕€鏌℃笟鍥ф珝闁哄矉缍侀幃銏ゅ传閵壯呭帎缂傚倷鑳剁划顖炲礉濞嗘挸绠栧Δ锝呭暞閸婂鏌﹀Ο渚█闁哥偛鐖煎娲传閸曨剙绐涢梺鍝ュ枎濞硷繝鐛箛鎾佹椽顢旈崨顏呭闂備胶绮幐鍝モ偓鍨笒椤洭鎮㈤崗鑲╁幈婵犵數濮撮崯顖滅矆閸愵亞纾奸柛灞剧☉濞搭噣鏌℃担绋挎殻鐎规洘甯掗埥澶婎潩椤掆偓缁犵偤姊婚崒娆掑厡闁硅櫕鎹囬幃娲Ω瑜庡畷鏌ユ煕閺囥劌骞橀柣顓熸崌閺屟嗙疀閹惧啿杈呴梺绋款儐閹搁箖骞夐幘顔肩妞ゆ帒鍋嗗Σ浼存⒒娴ｈ鍋犻柛鏂跨箰閳绘柨鈽夐姀鈺傛櫔闂侀潧顦弲娑氱矆鐎ｎ偁浜滈柟鍝勭Ф椤︼箓鏌涢弬鎸庡櫣闁宠鍨块幃娆撳级閹寸姳妗撴俊鐐€戦崝灞轿涘┑瀣畾闁割偓绲块悿鈧┑鐐村灦閻熝囧储娴犲鐓欓柤鍦瑜把呯磼鏉堛劍绀嬮柟顕€绠栭、姗€濮€閿涘嫬骞堥梺鐟板悑閻ｎ亪宕濈€ｎ喖绠熷Δ锝呭暞鐎电姵绻濋棃娑欏偍濞存粍绮撻弻鈥愁吋閸愩劌顬嬮梺娲诲幖椤戝寮诲☉婊呯杸闁哄啫鍊婚悡澶愭⒑閸濆嫯瀚扮紒澶屽厴绡撳〒姘ｅ亾闁哄本鐩獮妯兼崉閻戞鈧顪冮妶搴′簻缂佺粯鍔楅崣鍛渻閵堝懐绠伴悗姘间簽濡叉劙宕奸弴鐔叉嫼闂佸憡绋戦敃銉﹀緞閸曨垱鐓曢柟鎯ь嚟閹冲懐绱掗鍡欏埌闁宠棄顦垫慨鈧柍閿亾闁归攱妞藉娲川婵犲嫮绱伴梺绋块瀹曨剟鎮洪銏♀拻闁稿本鐟чˇ锕傚箹鐎涙鐭岀紒杈╁仦缁楃喖鍩€椤掑嫭鍋樻い鏇楀亾鐎规洘锕㈤、娆撴寠婢跺棗浜鹃柣鎴ｅГ閸婄敻姊婚崼鐔衡姇闁瑰弶鎮傞弻锝夋晝閳ь剟鎮ч崱娑樼厴闁硅揪瀵岄弫濠囨煛閸屾ê鈧挾娆㈤悽鍛娾拺闁告稑顭懓璺ㄧ磼閻樺磭澧柣锝囧厴婵偓闁靛牆鎳愰鎰箾鏉堝墽鍒伴柛妯荤矒楠炲繘鎼归崷顓狅紳婵炶揪绲芥竟濠囧磿閹剧粯鐓曢幖绮规閻撳ジ鏌ㄥ┑鍫濅槐闁轰礁鍟村畷鎺戔槈濮橆剙绠炴繝鐢靛Х閺佸憡鎱ㄩ幘顔肩柈妞ゆ牗绮庨惌鍡涙煃瑜滈崜娆撯€旈崘顔嘉ч柛鈩冾焽閳规稓绱撴担鐟扮祷婵炵》绻濋悰顕€骞嬮敃鈧粈瀣亜閺嶎煈鍤ら柍鍝勫暞閸犳劙鐓崶銊р姇闁哄拋鍓氱换婵囩節閸屾粌顤€闂佺粯鎸荤粙鎴︹€︾捄銊﹀磯闁绘碍娼欐慨娑欑節閳封偓閸曨偆浼岄梺鍝勬湰閻╊垰顕ｉ鍕閹兼番鍨归梻顖滅磽閸屾瑧璐伴柛鐘崇墵閳ワ箓鎮滈挊澶岀暫濠电偛妫欓幐濠氬磹缂佹ü绻嗘い鏍ㄧ矊鐢埖顨ラ悙鑼ф慨濠勭帛閹峰懘鎮烽弶鍨戞繝鐢靛仜閻°劌鐣濈粙璺ㄦ殾闁硅揪绠戝婵囥亜閺嶃劎鎳呴柛鎾卞姂濮婂宕掑鍗烆杸闂佸吋婢橀埀顒傚枂娴滃綊鏌熼悜妯诲碍濞存粌銈稿娲川婵犱胶绻侀梺鎼炲妽婵炲﹤顕ｉ幎钘壩у璺侯儌閹疯櫣绱掔紒銏犲箹闁瑰啿绻橀幃鐢割敂閸喓鍘藉┑掳鍊曢崯顐﹀煝閸懇鍋撶憴鍕婵＄偘绮欏顐﹀箻缂佹ê浜归梺鑲┾拡閸擄箑危娴煎瓨鈷掑ù锝呮憸閺嬪啯淇婂鐓庡闁诡喚鍋ら弫鍐磼閵堝棙娅?All rights reserved"
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇顓滀簻闁哄啫鍊归崵鈧繛瀛樼矒缁犳牠寮诲☉銏犵疀闂傚牊绋掗悘鍫澪旈悩闈涗杭闁搞劎鍎ょ粚杈ㄧ節閸ャ劌鈧兘鎮楀☉娆樼劷妞わ负鍎靛娲捶椤撴稒瀚涢梺绋款儏閿曨亪宕规ィ鍐╂櫇闁稿本顕遍埡鍛厓閺夌偞澹嗛ˇ锔剧磼閹邦厾鈽夋い顏勫暣婵″爼宕ㄩ婊庡敹闂佽瀛╅崙瑙勭濠靛牏鍗氶柣鏃傚帶閸楁娊鏌ｉ幇銊︽珕闁哄倵鍋撳┑锛勫亼閸婃牕顫忔繝姘仱闁哄啫鐗婇崐鍨归悩宸剱闁绘挻娲樻穱濠囶敍濠婂唭銏ゆ煕閻樺啿鍝虹€规洜鏁诲鎾偄缂堢姷鐩庨梻浣告惈閸燁偊宕愭繝姘闁稿本绋撶粻楣冩煕椤愶絿鐭岄柣顓熺懇閺屸€崇暆鐎ｎ剛袦濡ょ姷鍋涘ú顓€佸Δ浣瑰闁告瑥顦辨禍鐐烘⒒閸屾瑨鍏屾い顓炵墢閳ь剚绋堥弲娑綖韫囨哎浜归柟鍝勵儌閸嬫捇宕橀鍢夈劑鏌嶉崫鍕偓鎼佸焵椤掑倸鍘撮柡灞剧☉閳诲氦绠涢敐鍠帮箓姊洪崫鍕靛剳濠殿喗鎸抽垾鏃堝礃椤斿槈褔鏌涢幇鈺佸濞寸娀绠栧娲倻閳哄倹鐝﹂梺鎼炲姀濞夋盯顢氶敐澶樻晝闁挎洍鍋撶紒鐘崇洴閺屸剝寰勬繝鍕檸闂佸吋婢橀悘婵嬪煘閹达箑鐓￠柛鈩冾殘娴狀垳绱撴笟鍥ф珮闁搞劌鍟块埢搴ㄥ閵堝孩鏅濋梺鍐茬亪閺呮粓寮埀顒勬⒒娴ｈ櫣甯涢柨姘舵煟韫囨柨鍝虹€殿喗濞婇崺锟犲川椤旀儳骞堥梺璇插嚱缂嶅棝宕滃▎鎰浄閺夊牃鏂侀崑鎾舵喆閸曨剛顦ョ紓鍌氱Т閿曘倝鎮鹃柨瀣檮缂佸鐏濆畵鍡涙⒑缂佹ê濮堟い鏃€鐗犲畷顒勫醇閺囩啿鎷绘繛杈剧到閹诧繝宕悙鐢电＜闁告繂瀚崑銉р偓瑙勬礃濞茬喖鐛惔銊﹀殟闁靛鍎辨竟鎺楁⒒娴ｇ儤鍤€濠⒀呮櫕閸掓帡顢涢悙鏉戜簵濠电偛妫欓崹鍦閽樺褰掓晲婢跺閿梺璇茬箚閺呮粎鎹㈠☉銏犲窛妞ゆ洖妫滈埀顒€娼￠弻锛勪沪缁嬪灝鈷夐梺鐟板槻閹虫ê螞閸愵喖鐓涢柛鏇ㄥ亝閸庢捇姊婚崒娆戭槮闁圭⒈鍋婇幆澶嬬附缁嬭法鐛ラ梺鍝勭▉閸樿偐绮婚弽顐ｅ枑闁哄倽娉曢弳锕€鈹戦崒姘暈闁稿瀚伴弻娑樷攽閸℃浠煎┑鐐茬墑閸庨潧顫忔繝姘＜婵炲棙鍩堝Σ顕€姊虹涵鍜佸殝缂佺粯绻堥獮鍐倻閽樺）銊╂煃閸濆嫬鈧懓鈻嶉崶褜娓婚柕鍫濇噽缁犱即鏌ｅΔ鈧Λ婵嬪箯閹达附鍋勯柛蹇氬亹閸樹粙姊洪崫鍕殭闁稿﹤鎽滈弫顕€宕滄担铏癸紲闂佺粯锚绾绢參宕ｉ埀顒勬⒑閸濆嫮鐏遍柛鐘冲姉閳ь兛绲婚崑鎰板焵椤掍胶鈯曢拑閬嶆⒑濞嗘儳骞栭柍瑙勫灴閹瑩鎳犻鈧·鈧梻浣告啞濮婄懓煤閻旂厧鏄ラ柣鎰惈缁狅綁鏌ㄩ弮鍥棄闁逞屽墰閸忔ê顫忔繝姘唶闁绘棁銆€濡插牊绻涚€电校闁挎洏鍨归～蹇涙惞閸︻厾锛滃┑鈽嗗灣閸樠囧焻瑜版帗鈷戦梻鍫熺〒婢ф盯鏌熼鐓庘偓鍧楀箖娴兼惌鏁嬮柍褜鍓欓悾宄邦潨閳ь剟銆佸Ο渚叆闁逞屽墮鏁堟俊銈呮噺閳锋垿鏌涢敂璇插箹闁告柨顑夐弻娑㈠Ω閵壯呅ㄩ悗瑙勬礃閸旀瑩骞冨▎鎾村€绘俊顖滃帶楠炴姊绘担铏瑰笡妞ゎ厼鐗嗛悾鐑芥偨閸偒娼熼梺缁橆殔閻楀繒绮绘ィ鍐╃厱婵犲﹤鍟弳鐔虹磼鐎ｎ厼鍚圭紒杈ㄥ浮瀹曟帒鈽夊Ο鏄忕檨闁诲孩顔栭崰鏍偉婵傚摜宓侀悗锝庡櫘閺佸洭鏌ｉ弮鍫婵¤缍佸濠氬磼濞嗘垹鐛㈠┑鐐板尃閸ャ劌浜遍梺绯曞墲閵囧倸鈽夊Ο閿嬬€婚梺鐟扮摠缁诲秴螞濠婂嫮绡€闁汇垽娼ф牎缂佺偓婢樼粔褰掑箖閿熺姴鍗抽柣鏂垮缁犳岸姊洪棃娑氬閻庢凹鍣ｈ棟闁挎洖鍊归悡鍐磽娴ｈ偂鎴犵矆鐎ｎ喗鐓?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟稒宸濈紒鈾€鍋撻梻浣侯焾閺堫剛鍒掑畝鍔肩兘鍩€椤掑嫭鈷掑ù锝勮閻掔偓銇勯幋鐐茬仼闁瑰箍鍨归埞鎴犫偓锝庝海閹芥洟姊洪崫鍕偍闁搞劌缍婇弻瀣炊閵娧呯槇闂佸壊鐓堥崑鍕叏閸儱鏋侀柛顐犲劜閳锋帒霉閿濆牊顏犻悽顖涚洴閺岀喖宕ㄦ繝鍐ㄢ偓鎰偓瑙勬礃閸旀瑥鐣烽悜绛嬫晣闁绘柨鍢茬花銉︾節閻㈤潧鈻堟繛浣冲厾娲Ω瑜嶆慨顒勬煃瑜滈崜鐔奉潖閾忓湱鐭欓柟绋垮閹烽亶姊洪崨濠傜伇妞ゎ偄顦辩划瀣箳閺傚搫浜炬繛鎴炵懐閻掔晫绱掗悩鑽ょ暫闁诡喗顨呴埥澶娾枎濡厧濮洪梻浣规た閸樺ジ顢栭崶鈺傤潟闁圭儤顨忛弫濠囨煠閹帒鍔氱痪鐐▕閹鈻撻崹顔界仌濡炪倖娉﹂崶褏鍙€婵犮垼鍩栭崝鏇綖閸涘瓨鐓冮柍杞扮閺嗙偤鏌ｅ┑鍥ㄧ婵﹥妞介幊锟犲Χ閸涘懌鍨虹换娑氭嫚瑜忛悾鎶芥煏閸ャ劌濮嶇€规洘锕㈤、鏃堝幢閺囩姷鎲归梻鍌欑劍閸庡磭鎹㈤幇顒婅€块弶鍫氭櫅椤ユ岸鏌﹀Ο渚＆鐟滅増甯楅弲鏌ユ煕閵夛絽濡兼繛鍫濇湰缁绘繂鈻撻崹顔界亪闂佺粯顨呴敃顏呬繆閻㈢绀嬫い鏍嚤閳哄懏鐓忓鑸电〒閻ｈ鲸绻涢崼鐕佹疁婵﹨娅ｉ崠鏍即閻斿憡绶梻浣侯焾鐎涒晞銇愰崘顕呮晪闁靛鏅涚粈瀣亜閺嶃劍鐨戦柣銈傚亾濠碉紕鍋戦崐鏍暜閹烘纾婚柛鈩冪☉閻ゎ噣鎮楅敐搴℃灍闁绘挾鍠栭獮鏍庨鈧埀顑惧€曢…鍥箛椤撶姷顔曢梺鍛婄懃椤﹁鲸鏅堕鍫熺厓缂備焦蓱椤ュ牓鏌℃担绋挎殻鐎规洘甯掗埥澶娾枎閹邦喖绀侀梻鍌氬€烽懗鍓佹兜閸洖绀堟繝闈涙灩閸濆嫷鐓ラ柛娑卞灟缁楀鈹戦绛嬬劸闁糕晜鐗犻崺娑㈠箣閿旂晫鍘惧┑鐐存綑椤戝棝寮惰ぐ鎺撶厵闁告垯鍊楃弧鈧梺鍝勬湰閻╊垶鐛▎鎾崇妞ゆ挾濮靛▓濂告⒒娴ｇ瓔鍤冮柛鐘冲哺瀹曟垿濡舵径濠勭暫濠电偛妫欓幖鈺呭极閸曨垱鐓曟繛鍡楃Т閸斻倝鏌ｉ埡渚€鍙勬慨濠勭帛閹峰懘宕ㄦ繝鍐ㄥ壍婵犵數鍋炲娆撳磹閸︻厾鐭夌€广儱顦伴崐鐑芥煟閵忋垺鏆╅柣鎾愁儏椤啴濡堕崱妯硷紩闂佺顑嗛幑鍥蓟閿涘嫪娌柛鎾楀嫬鍨遍梻浣虹《閺呮繈宕戦妶澶屽祦闁糕剝鍑瑰Σ鐑樼節閵忥綆娼愭繛鍙夌墵閸╃偤骞嬮敂鑺ユ珫闂佸憡娲﹂崑鍌炲级椤撱垺鈷戠紒瀣皡閺€濠氭煟濡や焦绀嬫い銏＄懆缁犳盯寮崒婊呮婵犳鍠楅敃鈺呭礈濞戙垺鏅繛鎴欏灪閳锋垿鏌ｉ悢鍛婄凡闁抽攱姊荤槐鎺楊敋閸涱厾浠搁梺璇″櫙缁绘繂鐣峰鈧、娆撴偩鐏炶棄绠洪梻鍌欒兌缁垶寮婚妸鈺佺疅闁挎稑瀚浠嬫煛鐏炶鍔滈柣鎾寸洴閺屾盯鍩ラ崱妤€绠荤紓浣割儑缁垶濡甸崟顖毼╅柨婵嗘噹婵箓姊虹拠鈥虫灍闁荤啿鏅涢悾鐑芥晲閸℃绐炴繝鐢靛Т閸熺増绂嶅畡鎵虫斀闁挎稑瀚禍濂告煕婵犲啫鐏寸€规洜鎳撶叅妞ゅ繐鎳庢禒顓㈡⒑閸愬弶鎯堥柟鍐叉捣缁寮介鐔哄幈闂佸湱鍋撻〃鍛村磹瑜版帒绀嬮柛鎰靛枟閳锋垹绱撴担濮戭亝鎱ㄩ敃鍌涚厱闁瑰灝瀚悾鐢碘偓?闂?
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
            // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟版晥濠电姭鍋撳〒姘ｅ亾婵﹨娅ｇ槐鎺懳熼搹閫涚礃婵犵妲呴崑鍕偓姘緲椤曪綁宕ㄦ繝鍕嚌闂侀€炲苯澧寸€殿喖顭烽幃銏ゅ礂鐏忔牗瀚介梺璇查叄濞佳勭珶婵犲伣锝夘敋閳ь剙顫忛搹鍦煓闁兼剚鍨煎鍫曟⒑閸涘﹥鐓ラ梺甯秮閻涱喗寰勯幇顒傤啇婵炶揪绲介幉锟犓夊┑瀣厽闁绘ê鍘栭懜顏堟煕閺傝法孝闁崇粯鎸婚妶锝夊礃閳圭偓瀚兼繝娈垮枤閹虫挸煤閵堝鍊堕柍杞扮贰閻斿棝鏌ｉ悢宄扮盎闂婎剦鍓涢埀顒侇問閸ｎ噣宕戞繝鍌滄殾闁割偅娲﹂弫鍡椕归敐鍛儓妤犵偞顨婂缁樻媴閸涘﹥鍎撻梺鍝ュ櫏閸嬪嫮鍙呴梺闈涚箞閸婃洟鎮為崹顐犱簻闁硅揪绲鹃ˉ澶娒瑰鍫㈢暫婵﹦绮幏鍛村川婵犲啫鍓垫俊鐐€栭崹闈浳涘┑瀣畺鐟滄棃骞冮埡鍐╁珰闁肩⒈鍓涢弳顐ｇ節瀵伴攱婢橀埀顒佹礋楠炲﹪骞囬鍌涙婵炴挻鍩冮崑鎾绘煛鐏炲墽娲撮柍銉畵楠炲鈹戦崶鈺€閭繝鐢靛У椤旀牠宕伴弽顓熺厐闁挎繂顦拑鐔兼煥濠靛棙顥為柛搴ｅ枛閺屾洝绠涢弴鐐愩垺鎱ㄩ敐鍛ⅵ婵﹥妞藉畷顐﹀Ψ閵夋劧绲块埀顒€鍘滈崑鎾绘煙闂傜鍏岀€规挷绶氶悡顐﹀炊閵娿儲鎯涙繛瀛樼矊缂嶅﹪寮婚悢鍏煎€绘俊顖濇娴犲ジ姊洪崨濞氭垿骞冮崒姘兼綎婵炲樊浜滃婵嗏攽閻樿精鍏岀紒璁崇窔濮婃椽宕妷銉愶綁鏌ｅΔ渚囨畼缂侇喖顑夐獮鎺懳旀繝鍐╂珫闂備胶绮崝鏍ь焽濞嗘劗顩锋い鎾跺亹閺€浠嬫煟濡鍤嬬€规悶鍎甸弻娑㈡偄閸涘﹤鍞夐悗瑙勬穿缂嶄礁鐣烽悢纰辨晬婵炴垶鑹鹃獮宥嗕繆閻愵亜鈧牠寮婚敐澶婄閻忕偟鐓ú顏勎у璺猴功椤㈠懘姊虹憴鍕姸濠殿喓鍊濋幃陇绠涘☉姘絼闂佹悶鍎滃鍡樻毎婵犵數鍋涢幊搴∥涘┑瀣摕闁跨喓濮村浠嬫煕閹板吀绨界悮锕傛⒒娴ｇ瓔鍤冮柛鐘崇墵瀹曟煡寮婚妷鈶╁亾閿曞倸惟闁宠桨绀佺粣娑橆渻閵堝棙瀵欓柛鏇ㄥ墰濡叉﹢姊婚崒娆戝妽闁活亜缍婂畷娲礋椤栨艾鐎柡澶婄墐閺呮粓寮冲鍫熺厽闁规壆澧楀☉褔鏌涢妸锔剧畵闁宠鍨块幃鈺佺暦閸ヨ埖娈归梻浣告惈閹冲繘鎮￠敓鐘茶摕婵炴垶锕╅悡銉╂煃瑜滈崜鐔煎箖閻愮儤鏅滈梺娆惧灠娴滈箖姊婚崼鐔恒€掗柣鎺撴倐閺屸€崇暆鐎ｎ剛蓱闂佽鍨卞Λ鍐€佸☉姗嗙叆闁告稑鎷戠徊鍓ф崲濞戞埃鍋撳☉娆樼劷闁活厼顑囩槐鎺楊敊閼恒儱纾冲Δ鐘靛仜濡瑧绮悢鐓庣劦妞ゆ帊妞掔换鍡涙煙闂傚顦﹂崬顖炴⒑閸涘﹤濮傞柛鏂挎捣缁柨煤椤忓懐鍘靛銈嗘⒒閹虫挻鏅堕弻銉︾厵妞ゆ牗绋掗ˉ鍫ユ煕閳哄鐣甸柛鈹惧亾濡炪倖甯掔€氼剛澹曟繝姘厵闁硅鍔﹂崵娆撴⒑鐟併倕鈧牗绌辨繝鍥ч柛灞剧煯婢规洟姊绘担鑺ヮ棄闁哥喍鍗冲畷浼村冀椤撶偠鎽曢梺鍝勬川閸犳挾绮绘ィ鍐╃厱妞ゎ厽鍨甸弸娑樷攽椤旂⒈妲搁柍瑙勫灴閸╁嫰宕橀妸褏銈烽梻浣虹帛椤ㄥ懘鏁冮敃鍌氱闁靛繒濮Σ鍫ユ煏韫囨洖孝闁兼澘鐏濋埞鎴﹀煡閸℃浠╁┑鐐茬摠绾板秶绮嬪澶樻晜闁告洦鍘藉▓鎯р攽閻樿宸ラ柟铏姉娴滄悂鎮介崨濠勫幍?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊璁查弸娆撴⒑缂佹ê绗╁┑顔哄€楅幑銏犫槈閵忕姴鑰垮┑鈽嗗灥椤曆呭枈瀹ュ鐓熼柣鏂挎憸閹虫洜绱掗悩铏磳妤犵偛鍟灃闁告侗鍠楀▍婊堟煙閼测晞藟闁逞屽墲鐏忔瑩寮弽顓熲拻濞达絽鎽滈敍宥夋煙閾忣偅灏电紒顔芥閹粙宕归銏犵哎婵犵數鍋為崹鍫曟晝閳哄懎鐤柛娑卞枔娴滄粓鏌￠崘銊︽悙闁告艾缍婇弻锝夊煛娓氣偓閸欏嫭鎱ㄦ繝鍐┿仢鐎规洏鍔嶇换婵囨媴瀹勯偊鍔夊┑锛勫亼閸娿倝宕戦崟顖€鍥焼瀹ュ懐顔夐梺闈涚箳婵參寮ㄦ禒瀣厱闁靛鍨哄▍鍥煟閿曗偓閻楁挸顫忛搹鍦煓闁圭瀛╅幏杈╃磼閻愵剚绶茬紒澶婄秺楠炲啫顫滈埀顒勭嵁濡偐纾兼俊顖炴敱鐎氬ジ姊绘担鍛婂暈缂佸搫娼″畷鏇㈡焼瀹ュ懎鍤戦柟鍏肩暘閸斿秹鎮″▎鎾寸叄闊浄绲介埀顒€顑夐、鏃堝醇椤愶絾娅旈梻浣瑰缁诲倿藝椤栫偞鐓侀柛銉墯閻撴盯鏌涢妷銏℃珔濞寸娀浜堕弻娑㈡倷閼哥數銆愰梺瀹狀潐閸ㄥ潡骞冨▎鎾崇骇闁瑰濮冲鎾绘⒒娴ｇ懓顕滅痪鏉跨Ч婵＄敻宕熼锝嗘櫌闂侀€炲苯澧撮挊婵嬫⒒閸喍绶辨繛鍏肩墬缁绘稑顔忛鑽ょ泿婵炵鍋愭繛鈧柡灞剧洴瀵挳濡搁妷銉㈡嫽婵犵妲呴崑鍡涘磻閵堝拋娼栭柧蹇撴贡绾惧吋鎱ㄥΔ鈧Λ娆撴偩閻戣姤鈷戦柛锔诲幗閸も偓濠电偛鐪伴崐婵嬨€?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊為悷婊冪箻瀵娊鏁冮崒娑氬幈濡炪値鍘介崹鍨濠靛鐓曟繛鍡楃箳缁犲鏌″畝瀣М濠殿喒鍋撻梺闈涚箚閺呮繈宕濋崫銉х＝濞达絾褰冩禍鐐節閵忥絾纭鹃柡鍫墴瀹曚即骞囬幍顔煎絼闂佹悶鍎崝宥囩矆閳ь剟姊洪幖鐐插缂佽鍊块崺鐐哄箣閿旇棄浜归柣搴℃贡婵挳藟濠靛牏纾藉ù锝呮惈椤庢挾绱撳鍕獢鐎殿喖顭锋俊鎼佸Ψ閵忊剝鏉搁梻浣虹《閸撴繈銆冮崱妞绘灁闁靛鍎弨鑺ャ亜閺冣偓椤戞瑥顭囬幇顓滀簻闁靛鍎诲銉︺亜椤愩垻绠婚柟鐓庣秺瀹曠兘顢橀悩闈涘脯闂傚倷鑳剁涵鍫曞礈濠靛鈧啴宕ㄩ幖顓熸櫆闁诲孩绋掗…鍥╃不妤ｅ啯鐓曟い鎰╁€曞▍蹇涙煟閹哄秶鐭欓柡灞界Х椤т線鏌涢幘瀵哥疄闁挎繄鍋炲鍕箛椤掑偆鍞跺┑锛勫仜椤戝懐鈧稈鏅犻、鏇㈡嚍閵夛箑寮垮┑鈽嗗灡鐎笛呮兜閸撗呯＜闁绘ê纾晶閬嶆煙閸欏鍊愰柟顔ㄥ洤閱囨繝闈涚墢閹冲棗鈹戦悙娈挎闁稿鍔栫换娑㈠焵椤掑嫭鐓涢悘鐐插⒔濞插鈧鍠楅幐铏繆閹间礁唯闁靛鍠楀ù鍥⒒閸屾瑨鍏屾い顓炵墢閳ь剚绋堥弲鐘荤嵁閹达箑鐐婄憸婊冾焽?YAML闂?
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
        // winget 闂?闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰效婵犫偓娓氣偓濮婅櫣绱掑Ο铏逛紘濠碘槅鍋勭€氼喚鍒掓繝姘亹缂備焦顭囬崢鐢告⒑绾拋娼愰柛鏃撶畵瀹曢潧鈻庨幋鐘碉紲闂佺粯锕㈠褔寮稿☉娆愬弿濠电姳鑳堕惌娆戔偓瑙勬礀閻栧ジ銆佸Δ鍛劦妞ゆ帊绶￠弨浼存⒒閸屾瑧顦︽繝鈧潏銊︽珷婵°倐鍋撴い顓炵仢铻ｉ柤娴嬫櫇閻掑ジ姊洪崨濠傚闁哄倸鍊圭粋宥咁煥閸喓鍘搁梺鍛婂姂閸斿秵寰勯崟顑句簻闁靛鍎哄Σ鎼佹婢舵劖鐓熸俊顖濇濠€鎾煕鐎ｃ劌鐏柟渚垮妽缁绘繈宕橀埞澶歌檸婵犵妲呴崑鍕疮椤愶附绠掗梻渚€娼ч悧鍡椢涘☉妯忚櫣鈧數纭堕崑鎾舵喆閸曨剛顦梺鍝ュУ閻楃娀濡存笟鈧鎾閳╁啯鐝抽梻浣虹《閸撴繈鎮烽妷鈺傚殑闁割偅娲橀埛鎺楁煕鐏炴崘澹橀柍褜鍓氶幃鍌氱暦閹邦収妲归幖杈剧悼閻掑吋绻涢幘鏉戠劰闁稿鎸婚〃銉╂倷瀹割喗鈻堥梺绯曟櫅鐎氭澘鐣峰鈧、鏃堝幢濡儤鐤勬繝鐢靛Т閻ュ寮舵惔鎾充壕闁哄洢鍨圭粻鐘诲箹鏉堝墽鎮奸柣顓炴閺屾盯顢曢妶鍛亖缂備胶濞€缁犳牠寮婚悢琛″亾濞戞瑯鐒介柟鍐插暣閺岋綀绠涢敐鍕仐闂佸搫鏈粙鎾诲焵椤掑﹥鈧儵宕橀妸銏″珶闂傚倷绶氬褍煤閵堝洠鍋撳顐㈠祮闁绘侗鍣ｉ獮鎺懳旈埀顒傜不閿濆棛绡€闁割煈鍋勬慨鍐煟閵夘喕閭慨濠勭帛閹峰懘鎼归悷鎵偧闂備礁婀遍…鍫モ€﹂悜钘夌畾闁告洦鍨奸弫宥夋煟閹邦垰钄奸柟鑺ユ礀閳规垿鎮欓弶鎴犱户闂佹悶鍔屽﹢杈╁垝鐠囨祴妲堥柕蹇ョ磿閸橀亶妫呴銏″闁圭顭烽獮澶愬箹娴ｅ湱鍘介梺瀹犳〃缁舵岸宕ｉ崟顖涚厸閻忕偟纭堕崑鎾诲箛娴ｅ憡鍊┑鐐舵彧缁叉崘銇愰崘鈺冾洸濡わ絽鍟悡娑㈡煕鐏炰箙顏堝礉濠婂牊鐓涢柛灞绢殔娴滈箖姊婚崒姘偓鐑芥嚄閸撲礁鍨濇い鏍仜缁€澶愭煛瀹擃喖鐬奸崝宄扳攽閻愬弶顥為柛銊ф暩濞嗐垽濡堕崶鈺冿紳婵炶揪缍€椤曟牠鎮為悾宀€纾奸柣妯垮吹閻ｆ椽鏌熼绛嬬劸缂佺姵鐩顕€宕掑锝呭綘闂傚倷鑳堕…鍫ヮ敄閸℃稑绠查柛銉墰瀹撲線鏌涢妷锝呭濠殿垱鎸抽弻娑樷槈濮楀棛鍙曢梺?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟稒宸濈紒鈾€鍋撻梻浣侯焾閺堫剛鍒掑畝鍕┾偓鍌毭洪鍛嫼闂佽姤锚椤︻垶寮抽悢鍏肩厱闁绘ê纾晶鐢碘偓娈垮枛椤嘲顕ｉ幘顔藉亜闁惧繗顕栭崯搴ㄦ⒒娴ｇ顥忛柣鎾崇墦瀹曚即寮介妸褏褰鹃梺鍛婃处閸嬧偓闁衡偓娴犲鐓熼柟閭﹀墮缁狙囨煃缂佹ɑ绀€闂囧绻濇繝鍌氼伀闁活厽甯為埀顒冾潐濞叉﹢鏁冮姀銈呮槬闁逞屽墯閵囧嫰骞掗崱妞惧婵＄偑鍊ゆ禍婊堝疮鐎涙ü绻嗛柛顐ｆ礀楠炪垺淇婇婊冨妺婵炲牜鍋婂缁樼瑹閳ь剙顭囬懡銈呯筏濠电姵鑹剧壕鍧楀级閸偄浜炲ù?0闂?1978335189闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏焊娴ｅ湱鈧姊洪悙钘夊姎缁剧虎鍘奸敃銏ゅ箻椤旂晫鍘遍梺鍝勫暊閸嬫捇鏌涢妸銉т虎闁伙絽鍢茬叅妞ゅ繐瀚崝锕€顪冮妶鍡楃瑐闂傚嫬绉电粋宥咁煥閸喓鍘甸梺缁樺灦閿氶柣蹇嬪劦閺屽秷顧侀柛鎾寸箓閻ｇ兘宕归銈囧骄闂佸搫娲ㄩ崰鎾绘⒔閸曨厾纾奸悗锝庡幗绾泛螖閻樺弶鍠樻慨濠冩そ瀹曟粓骞撻幒宥囧嚬缂傚倷绀侀鍡涘垂閼哥數鈹嶅┑鐘插瀹曞鏌曟繛褍鎷戠槐鏌ユ⒒娴ｈ櫣甯涢柨鏇樺灩椤洩顦崇紒鍌涘浮閺屽棗顓奸崱娆忓箰闁诲骸鍘滈崑鎾绘倵閿濆骸澧伴柨娑氬枑缁绘稓鈧數顭堥鎾剁磼閻樿櫕宕屾鐐诧躬閹瑩宕崟搴涘姂閺屾洘寰勫☉姗嗘喘婵炲瓨绮庨崗妯侯潖婵犳艾纾兼繛鍡樺姉閵堟澘鈹戦埄鍐︿粻闁告梹鍨块獮鍐倷閻戞ɑ娅囬梺绋挎湰濮樸劑藝椤斿皷鏀介柣鎰级椤ユ粎绱掔紒妯烘诞鐎殿喗濞婇弫鍐磼濞戞艾骞楅梻浣告惈閸燁偊宕愰悽绋跨闁跨喓濮甸悡娑樏归敐鍛暈闁哥姵锕㈤弻鈥崇暆鐎ｎ剙鍩岄柧浼欑秮閺屾稑鈹戦崱妤婁痪濠殿噯绲鹃崝娆忣潖濞差亜浼犻柛鏇㈡涧閸撻亶姊洪崷顓熷殌婵炲眰鍊濋敐鐐剁疀閺冨倻鐦堝┑顔斤供閸撴盯鏁嶅鍐ｆ斀闁绘劕寮堕ˉ鐐烘煕閳轰胶澧曢崡杈ㄣ亜閺囨浜惧┑顔硷攻濡炶棄鐣烽悜绛嬫晣婵炴垶鐟ラ悵閬嶆⒒娴ｄ警鐒炬い鎴濇噹铻炴繝闈涱儏閺勩儵鏌嶈閸撴岸濡甸崟顖氱鐎广儱娲ゆ俊浠嬫⒑濮瑰洤濡介柛銊ユ健瀵鈽夊鍛澑闂佸搫鍊稿Λ顓熺椤忓牆绠栭柣鎰靛墻濡插綊骞栧ǎ顒€鐏╅柡瀣€垮娲川婵犲啫顦╅梺绋款儐鐢€崇暦閻㈢围濠㈣泛顑傞幏娲⒑閸涘﹦绠撻悗姘槻鍗辩憸鐗堝笚閻撴洟鏌曢崼婵堢闁伙絿鏁婚弻鈩冩媴缁嬫寧娈婚悗瑙勬礃鐢帡鍩ユ径濠庢僵妞ゆ巻鍋撶紒鐘劦濮婄粯鎷呯粵瀣濠电偛顕崗妯侯嚕椤愶箑绠瑰ù锝嗙ゴ閸嬫捇鏁冮崒姘跺敹闂佺粯鏌ㄩ幖顐﹀焻娴煎瓨鈷掗柛灞剧懄缁佺増銇勯弴鍡楁噳閸嬫挸鈽夐幒鎾寸彋闂佺娅曠换鍐Χ閿濆绀冮柨婵嗗€告禍楣冩煥閺囩偛鈧綊宕愭繝姘參婵☆垯璀﹀Σ濂告煙閼恒儲绀嬫慨濠冩そ瀹曨偊宕熼鈧粣娑㈡⒑閸濄儱孝婵炴潙鍢查埢搴ㄥ閵堝棗鈧兘鎮楅棃娑欐喐妞ゆ梹娲熷娲偡闁箑娈堕梺绋款儑婵數绮╅悢濂夋建闁逞屽墴瀵鏁愭径妯绘櫍闂佺粯妫佸▍锝夋偪閸曨偀鏀?
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵婵炲棗绻掗崝鎾⒑鏉炴壆顦︽い鎴濇婵＄敻宕熼姘鳖啋闁荤姾娅ｉ崕銈夋倵妤ｅ啯鈷戦柛娑橈功閹冲啰绱掔紒妯哄婵犫偓娓氣偓濮婅櫣绮欑捄銊ь唶闂佸憡鑹鹃鍥╂閻愬搫绠ｉ柣妯虹仛閿涘繘姊虹拠鈥崇€婚柛鏇ㄥ亗濞ｎ喗绻濆▓鍨灈闁挎洏鍎遍—鍐╃鐎ｃ劉鍋撴担鑲濇棃宕ㄩ鐘插Е婵＄偑鍊栫敮鎺楀磹婵犳碍鍎楁繛鍡楃箞閳ь剚甯掗～婵嬫晲閸涱剙顥氶梺璇插椤旀牠宕抽鈧畷鎴炵節閸屾粍娈鹃梺鍓插亝濞叉牠鎮″☉銏＄厱閻忕偛澧介。鏌ユ煟鎼淬劎鐣烘慨濠勭帛閹峰懘鎼归悷鎵偧婵＄偑鍊戦崝宀勫箠濮椻偓楠炲啳顦圭€规洖宕埢搴ㄥ箣閻樺灚娈介梻鍌欑濠€閬嶅磿閵堝拋娼栭柤濮愬€曢ˉ姘攽閸屾碍鍟為柣鎾跺枑缁绘盯宕卞Δ鍐唹闂侀潻绲肩划娆撳蓟濞戙垹绠抽柡鍌涱儥閺嗩參姊洪棃娑欐悙閻庢矮鍗抽妴渚€寮崼婵堫槹濡炪倖鍔х徊楣冾敊瀹ュ鈷掑〒姘ｅ亾闁逞屽墰閸嬫盯鎳熼娑欐珷闁告挆鍛紳閻庡箍鍎遍幊蹇浰夐悙鐢电＜闁稿本姘ㄥ瓭濡炪値鍘归崝鎴濈暦婵傚憡鍋勭€瑰嫭婢橀埛鏇㈡⒒閸屾瑧绐旀繛浣冲洠鈧箓宕奸妷銉э紵閻庡箍鍎遍悧鍕瑜版帗鐓欓柣鎴炆戠亸顓灻瑰鍕垫疁闁哄瞼鍠栭幊鏍煛閸曞﹤顦版穱濠囨偑濞嗗繑鎼愮痪鎯ь煼閺岀喖骞戦幇闈涙闂備浇锟ラ崐鏍ㄧ┍婵犲洤围闁稿本鐭竟鏇㈡⒒娴ｈ姤顥夐柛鐔跺嵆瀹曚即寮介鐐舵憰闂侀潧艌閺呮粓宕戦崟顖涚厱婵犻潧妫楅悵鏃堟煏韫囧鐏痪鎯с偢閺屽秷顧侀柛鎾寸洴閸┾偓妞ゆ帒锕︾粔鐢告煕閻樻剚娈滈柛鈺傜洴楠炴帒螖娴ｅ搫骞堟俊鐐€栭崝鎴﹀磹閺囥垹鍑犻柡鍐ㄧ墛閻撳繘鏌涢埄鍏狀亞绮幒妤侇梿濠㈣埖鍔栭悡鐔告叏濡も偓濡绂嶅┑鍥╃闁告侗鍙忛弨濠氭煙娓氬灝濡界紒缁樼箞瀹曘劑顢氶崨顒€鎽嬮梻鍌欑缂嶅﹪藟閹惧绠鹃柍褜鍓熼弻娑㈠Ω閿曗偓閸斻倝鏌ｉ敐鍥у幋鐎规洖鐖奸弫鍌炴寠婢跺苯骞堢紓鍌氬€搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ巻鍋撻柡鍕╁劦閺屸€崇暤椤斿吋鍣烘繛鍫熸礋濮婇缚銇愰幒鎿勭吹闂佺厧鐤囧▔娑㈡偩閸偆鐟归柍褜鍓熷璇测槈閵忕姷鍔撮梺鍛婂姦娴滄牕危閸儲鈷戠紓浣姑慨鍥煥閺囨ê鍔﹀┑锛勬暬瀹曠喖顢涘槌栧敽闁诲骸绠嶉崕鍗灻归崒姣硷綁顢欓悾宀€顔曢柣搴㈢⊕椤洭鎯岀€ｎ剛纾兼い鏃囧Г鐏忣厽銇勯銏㈢闁圭厧缍婂畷鐑筋敇閻欏懐搴婂┑鐘殿暯濡插懘宕归棃娑掓瀺闁挎繂妫涢々椋庣磼鐎ｎ偒鍎ユ繛鎾愁煼閺屾洟宕煎┑瀣碘偓妤侇殽閻愬澧紒缁樼☉椤斿繘顢欓悡搴ｎ啇婵＄偑鍊栧ú鈺冪礊娓氣偓閻涱喗鎯旈妸锕€娈熼梺闈涱槶閸庢澘效濡ゅ啰纾介柛灞捐壘閳ь剛鍏橀幃鐐烘晝娴ｅ摜绋婇梻鍌欑缂嶅﹪寮ㄩ崡鐑嗘富闁芥ê顦藉鏍ㄧ箾瀹割喕绨荤€瑰憡绻傞埞鎴︽偐閹绘帩浠卞┑鈩冨絻閻楁挸顫忓ú顏勭閹兼番鍨婚敍姗€姊虹粙鍖℃敾缂佽鐗撻悰顕€宕橀妸搴ｅ枔缁辨帒螣閼测敩銈夋⒒娓氣偓濞佳嗗櫣闂佸憡绮堢粈渚€寮查銏♀拻闁稿本鐟чˇ锕傛煙鐠囇呯？闁告帗甯掗悾婵嬪礋椤愩倖鐒鹃梻浣筋潐椤旀牠宕板鑸靛剹闁圭偓鐣禍婊堟煛閸ヮ煈娈斿ù婊堢畺濮婃椽宕崟顓犲姽缂備浇寮撶划娆撳春閵夛箑绶為柟閭﹀墰椤㈠懘姊虹紒妯哄妞ゆ洦鍙€閵囨劖鎯旈妸锔惧幗闁瑰吋鐣崐銈咁焽閹邦厾绠鹃柛娆忣槺婢х敻鏌熼鍝勫姦鐎规洘顨婂鑽も偓闈涙憸閻ｇ偓绻濆▓鍨灍缂佽鍊块幆宀勫醇濠㈩亷缍侀幃婊堟嚍閵夈垺瀚奸梻浣告啞缁诲倻鈧氨鍏樺畷鏇㈠箛閻楀牏鍘藉┑掳鍊曢崯顐㈢摥婵＄偑鍊ら崑鍕崲閹邦喖寮叉俊鐐€曠换鎰偓姘煎墰缁絽鈽夊▎鎴犵槇闂佹眹鍨藉褍鐡梺璇插閸戝綊宕抽敐澶婄畺濡わ絽鍟崑銊╂煕濞戞﹫鍔熼柛妯圭矙閺岀喖宕楅懖鈺傛闂佹悶鍔岀紞濠傜暦閻㈠壊鏁嗛柛鏇ㄥ墰閸橀亶姊虹憴鍕姢妞ゆ洦鍙冮獮濠囧炊椤掍胶鍘遍梺宕囨嚀閻忔繈鎮橀埡鍌樹簻闁瑰墽鍋ㄩ崑銏⑩偓瑙勬磸閸旀垿銆侀弮鍫濆窛妞ゆ牗顨呮禍鎯р攽閻樺磭顣查柍閿嬪灴閹嘲鈻庤箛鎿冧患闂佸憡鏌ｉ崐鏍Φ閸曨垰鍗抽柕濞垮劚缁秹鎮楃憴鍕┛缂傚秳绀侀悾鐑藉础閻戝棙瀵岀紓鍌欑劍钃遍柡澶嬫倐濮婄粯鎷呴搹鐟扮闂佸憡姊归悧鏇⑩€﹂崶褉鏋庨柟鐐綑閳ь剙鐖奸弻銊╁棘濞嗙偓缍楁繛瀛樼矋缁秹濡甸崟顖氱閻犻缚妗ㄥ▽顏堟⒑閸濄儱校闁挎洏鍨藉濠氭偄绾拌鲸鏅╅梺璇″瀻閸屾侗娼熼梻鍌欐祰椤曟牠宕板Δ鍐ㄥ灊闁规崘顕х粻鐘绘煟閹邦剚鎯堥崬顖炴偡濠婂啰肖闁轰緡鍠栬灃闁告侗鍠氶崢浠嬫⒑缂佹ɑ鐓ラ柟鑺ョ矒閹瞼鈧綆鍓涚壕鍏笺亜閺冨洤袚閻忓浚鍙冮弻锝夋晲閸涱厽些濡炪値鍘归崝鎴濈暦濮椻偓閺佹劙宕卞▎灞戒壕濠电姵纰嶉埛鎴犳喐閻楀牆绗掑ù婊€鍗抽弻娑㈡晲韫囨洖鍩屽銈庡墮閿曨亪銆佸璺虹劦妞ゆ巻鍋撴い顐㈢箰鐓ゆい蹇撳缁卞爼姊洪崨濠冨闁稿鎳庨埢鎾活敇閵忊檧鎷虹紓浣割儏鐏忓懏螞濮樻墎鍋撶憴鍕闁轰焦鎮傚畷姘跺箳閹惧墎鐦堝┑顔斤供閸撴岸藟濠靛棌鏀介柣鎰级椤ユ粌顭胯缁瑥鐣烽鍕仺闁告稑艌閹锋椽姊虹粙璺ㄧ闁告艾顑夊畷鎰板箛椤撶姷顔曟繛杈剧到閸熻法鈧凹鍙冨畷锝堢疀濞戞瑧鍘遍梺闈涱樈閸犳牗鏅堕鐐寸厽闁规儳鐡ㄧ粈鍐磼缂佹娲寸€规洖宕灃闁告侗鍨板浠嬫⒒娴ｅ鈧偓闁稿鎸搁埞鎴︽偐瀹曞浂鏆￠梺缁樻尭缁绘﹢寮诲☉銏犖ㄩ柟瀛樼箓閺嬨倕霉閻撳骸鏆ｉ柟顔筋殔閳绘捇宕归鐣屼簽缂傚倷绶￠崰妤呮偡閿旂晫鈹嶅┑鐘叉搐缁犵懓霉閿濆牆鈧粙鍩￠崨顔惧帾婵犮垼鍩栭敋闁哄鍠愰妵鍕Ψ閿濆應鍋撻弴銏犵厴闁硅揪闄勯崑鎰版煕椤垵浜濇慨锝呭濮婅櫣绮欏▎鎯у壋闂佺顑冮崐鏇㈡偩閻戣姤鍋ㄩ柛鎾冲级閺呮粓姊洪崜鎻掍簼缂佽瀚板畷鎰版倷閻戞ǚ鎷洪梻鍌氱墛娑撶懓鈽夐姀鐘殿槰闂侀潧臎鐏炶姤娅忓┑鐘垫暩婵兘銆傞鐐潟闁哄洢鍨圭壕濠氭煙鏉堝墽鐣辩痪鎯х秺閺岋繝宕堕埡浣囷絿鈧娲橀悡锟犲蓟閻斿吋鐒介柨鏇楀亾闁哄鎹囬弻娑氣偓锝庡亝鐏忣厽銇勯锝囩疄鐎规洩绻濋幃娆撳箵閹烘棏鍚呴梻浣筋嚙濮橈箓锝炴径濞掑搫螣婵傝姤妞介幃銏＄附婢跺﹥顓挎俊鐐€栧ú宥夊磻閹惧灈鍋撶憴鍕闁靛牊鎮傞獮鍐閵堝懍绱堕梺鍛婃处閸嬪棝鎮烽弻銉︹拻濞达綀妫勯崥褰掓煕閻樺啿濮夐柛鎺撳笒椤撳ジ宕惰閻忓﹪姊虹憴鍕姢缁剧虎鍙冨鎶芥晜閹存帞绠氶梺闈涚墕閸婂憡绂嶉幆褜娓婚柕鍫濋娴滄繄绱掗鑺ュ磳鐎规洘妞介崺鈧い鎺嶉檷娴滄粓鏌熼崫鍕棞濞存粓绠栧鐑樺濞嗘垶鍋ч梺绋跨箲閿曘垽鎮伴閿亾閿濆骸鏋熼柡鍛箞閺屽秷顧侀柛鎾寸〒閸掓帗绻濆顓炴闂侀潧鐗嗗ú銊╂晬濠靛洨绠鹃弶鍫濆⒔缁夘剚銇勯弴鐔哄⒌闁诡喚鍋炵粋鎺斺偓锝庡亞閸樺崬鈹戦濮愪粶闁稿鎸搁湁婵犲﹤妫欑涵鐐亜椤愩垻绠婚柟鐓庢贡閹叉挳宕熼銏犵闂傚倷绀侀幉鈩冪瑹濡ゅ懎绐楅柡宓偓濡插牏鎲歌箛鏇燁潟闁规儳鐡ㄦ刊鎾煕閿旇骞栫€殿喓鍔庣槐鎾存媴閸濆嫅锝夋煟濡ゅ啫鈻堝┑鈥崇摠缁楃喖鍩€椤掆偓椤曪綁顢氶埀顒€鐣锋總鍛婂亜闁炬艾鍊归惈蹇曠磽閸屾艾鈧悂宕愰悜鑺ュ€块柨鏇炲€甸埀顒婄畵瀹曞爼顢楅埀顒勫磼閵娿儍褰掓偐瀹割喖鍓遍梺鎶芥敱閸ㄥ潡寮诲☉妯锋斀闁糕剝顨忔导鈧柣鐐村嚬娴滎亜顫忓ú顏勫窛濠电姴鍟伴崣鍡涙⒒娴ｇ绨荤紓宥勭窔閻涱噣宕卞Δ鍐╂畷闂佸憡娲﹂崑鍡涱敊婵犲洦鈷戦柣鐔稿閹界姵鎱ㄦ繝鍌滅Ш鐎殿噮鍓熼崺鈧?
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
            // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶鏉款熆鐟欏嫭绀冪紒缁樼箓椤繈顢橀悩鎻掔疄闂傚倷鐒﹂弸濂稿疾濞戙垹鐤ù鍏兼綑閻撯€愁熆閼搁潧濮堥柣鎾存礋閹﹢鎮欐０婵嗘婵犳鍠栭崐鍧楀蓟閿涘嫪娌悹鍥ㄧゴ閸嬫捇寮撮姀锛勭暢闂備浇顕ч崙鐣岀礊閸℃稑纾婚柛娑樼摠閸嬬喖鏌￠崶鈺佸壉婵炴挸顭烽弻鏇㈠醇濠靛鈧霉濠婂嫮绠栫紒缁樼洴閹崇娀顢楅埀顒勫几濞戞埃鍋撳▓鍨灈妞ゎ厾鍏橀獮鍐閵忊€充粧闂佺偨鍎遍崢鏍ㄧ珶閺囥垺鈷戞慨鐟版搐閻忣亝銇勯弮鈧悧鐘诲箖閻愬搫绠氭い顑解偓宕囩Ш闁诡喒鏅涢埢搴ょ疀閺傛妲遍梻浣虹帛閹告悂宕幍顔筋潟闁规儳鐡ㄦ刊鎾煠濞村澧查柛銊ユ健瀹曞搫鈽夐姀鐘殿唺濠德板€撻懗鍫曞储閹剧粯鍋℃繝濠傚閻帞鈧娲樼划宀勫煝鎼淬劌绠ｉ柣妯夸含閻熸繈鏌ｆ惔锛勭暛闁稿酣浜惰棟妞ゆ牗绮岄ˉ姘舵煕瑜庨〃鍡涘煕閹寸偑浜滈柟鍝勬娴滃墽绱撴担鍓叉Ц妞ゆ洦鍘鹃崚鎺撶節濮橆厼浜归柣鐘叉穿鐏忔瑩顢欓幇顔剧＝濞撴艾娲ら弸锔姐亜椤愩埄妯€妞ゃ垺鎹囧畷鍗炍熼崷顓犵暰闂備礁婀辩划顖滄暜閻愬瓨娅犻柕蹇嬪€栭悡娆撴煕濞嗗浚妲归柛濠冨姉閳ь剚顔栭崳顕€宕抽敐鍛殾闁绘挸绨堕弨钘夆攽閻樻彃鏆為柛?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厵缂備焦锚缁椦囨煃瑜滈崜锕傚矗閸愵煈娼栭柛婵嗗珔瑜斿畷鎯邦槾濞寸厧瀚—鍐Χ鎼粹€崇闂佺粯顨呯换鎺楀礆閹烘鍤掗柕鍫濇閺嬪倿姊洪崨濠冪５闁哄懏鐟ч懞杈ㄧ節濮橆厸鎷洪柣鐔哥懃鐎氼剟藟閹扮増鐓涢柛娑卞枤濞插鈧娲樼换鍌濈亽婵犻潧鍊婚弲顐﹀箠閹剧儵鈧箓濡搁埡浣歌€块梺鍐叉惈閸婅崵绮旀搴ｇ＝闁稿本鐟ㄩ崗宀€绱掗鍛仸闁诡垰瀚伴、娑㈡倷闂堟稓銈﹂梺璇插嚱缂嶅棝宕板Δ鍛９闁绘垼濮ら悡鏇㈡煙閼割剙濡芥繛鍛嚇閺屾稑鈹戞繝鍐句哗闂侀€炲苯澧い鏃€鐗犲畷浼村冀椤撴稈鍋撻敃鍌涘€婚柦妯侯槹閻庮剟姊洪崨濠傚Е闁哥姵鐗滄竟鏇㈠礂閼测晝顔曢梺鐟邦嚟閸嬬姵绔熷鈧幃浠嬵敍濞戣鲸鐤佸┑顔硷功缁垳绮悢鐓庣劦妞ゆ巻鍋撴い顓炴穿椤﹀綊鎸婂┑鍥ヤ簻闁规儳宕悘鈺冪磼閳ь剚寰勭仦绋夸壕闁稿繐顦禍楣冩⒑閸涘﹥澶勯柛鎾寸懇瀹曟粓顢橀悙鈺傛杸闂佺粯鍔曞鍫曞煝閺囩伝鐟邦煥閸曨厾鐤勯梺?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ礂閻撳簶鍋撶紒妯圭箚妞ゆ牗绻冮鐘裁归悩灞傚仮婵﹥妞藉畷顐﹀礋椤掑顥ｆ繝纰樻閸嬪懐鎹㈤崼婵愬殨濠电姵纰嶉崑鍕煕韫囨挾姣為柟宄邦煼閺岋絾鎯旈妸锔介敪婵犮垻鎳撻敃銈呭祫濡炪倖娲嶉崑鎾垛偓娈垮枛閻栧ジ宕洪敓鐘茬＜婵犲﹤鎳愰弶浠嬫⒒娴ｅ憡鎯堥柣顒€銈稿畷浼村冀瑜忛弳锔戒繆閵堝懏鍣洪柛銈呭暣閺屽秹宕崟顐熷亾瑜版帩鏁婇柛鎰靛枟閳锋帒霉閿濆懏鍟為柛鐔哄仱閺岋紕浠︾拠鎻掝潎闂佽鍨伴ˇ鐢稿箠閺嶎厼鐓涘ù锝呮憸娴滄澘鈹戦悩鍨毄濠殿喚鏁婚幊婵嬪箚瑜庨弳婊堟⒒閸屾瑧顦﹂柛鐔锋健楠炴牠顢曢敃鈧壕褰掓煙闂傚顦﹂柣鎺戠仛閵囧嫰骞掗幋婵冨亾瑜版帒姹叉い鎺戝閻撴盯鎮橀悙棰濆殭濠碘€炽偢閺屽秶鎲撮崟顐や紝閻庤娲栧畷顒勫煡婢舵劕绠ュù锝夋櫜婢规洖鈹戞幊閸婃洟骞婅箛娑欏亗闁哄洨鍠嶇换鍡樸亜閺嶃劎鐭岄悽顖涚洴閹绠涢妷褏鏆ら梺鍝勭焿缁辨洘绂掗敃鍌涘仼閻忕偞绺块崕鏌ュΦ閸曨噮妲虹紓浣藉煐瀹€绋款嚕鐠囨祴妲堥柕蹇曞Х椤旀帞绱撻崒姘卞ⅱ濠殿喖纾埀顒佺▓閺呯姴顫忓ú顏勬嵍妞ゆ挾濮甸幉姗€姊虹粙娆惧剰婵☆偅绋戝嵄闁圭増婢樼粻濠氭倵濞戞顏堫敁閹炬枼鏀芥い鏃傛櫕缁犳捇鏌ゅú璇茬仸妞ゃ垺妫冮、姗€濮€閳ユ枼鍋撻悽鍛婂仭婵炲棗绻愰顏勵熆鐠哄搫顏柡灞剧洴楠炴鎹勯悜妯间邯闁诲氦顫夊ú妯侯渻娴犲绠栨繛鍡樻尭娴肩娀鏌涢弴銊ュ季闁告艾鎳樺缁樻媴閻熼偊鍤嬪┑鐐村絻缁夌懓鐣烽幋锕€绠荤紓浣诡焽閸樹粙姊鸿ぐ鎺戜喊闁告挶鍔戦悡顒勵敆閸曨剛鍘搁柣蹇曞仧閸嬫挾绮堥崘顏嗙＜缂備焦顭囩粻姘辩磼閻樺磭娲寸€殿喗鎸抽幃鈺呮惞椤愵偄鏅犲┑鐘殿暜缁辨洟宕戦幋锕€纾归柡鍥╁У瀹曟煡鎮楅敐搴℃灈缁绢厸鍋撻梻浣告啞閸旀洖螣婵犲偆鐒?DoH/nginx 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟稒宸濈紒鈾€鍋撻梻浣侯焾閺堫剛鍒掑畝鍔肩兘鍩€椤掑嫭鈷掑ù锝呮啞閹牓鏌熷畡閭﹀剱缂佸倸绉撮埞鎴﹀醇閻斿嘲绨ユ繝鐢靛█濞佳囶敄閸℃稑纾婚柕濞炬櫆閳锋帡鏌涢銈呮瀻闁搞劋鐒︾换娑㈠箣濞嗗繒浠鹃梺缁樻尰濞茬喖骞冨Δ鍛櫜閹肩补鈧尙鍑归梺璇茬箰缁诲宕￠幎钘夎摕闁哄洢鍨归悙濠勬喐瀹ュ鏁傛い鎺戝€荤壕濂告煟濡櫣锛嶆繛鎻掔摠椤ㄣ儵鎮欓幖顓犲姺闂佸湱鎳撶€氼厾绮悢纰辨晬婵炴垶鐟х敮鍡涙⒒閸屾艾鈧悂宕愭搴ｇ焼濞撴埃鍋撴鐐寸墵椤㈡洟鏁冮埀顒傜不椤栫偞鍊甸柨婵嗛閺嬫稓绱掗埀顒勫醇閵夛妇鍘介梺鍝勫暙閸婂摜鏁崜浣虹＜闁绘ê纾晶顒勬煙娓氬灝濡兼い顏勫暟閹风娀鐓鐑嗘闂傚倸鍊峰ù鍥敋閺嶎厼绐楁俊銈呮噺閸嬶繝鏌嶉崫鍕櫣缂佺姷濮电换婵囩節閸屾碍娈滈梺鍝勵儐濡啴寮婚悢铏圭煓闁割煈鍠氬Σ鏉库攽閳藉棗浜滈悗姘緲椤繑绻濆顒傦紲濠电偛妫欑敮鎺楀储閿涘嫮纾藉〒姘搐濞呮﹢鏌涢妸锕€鈻曠€殿噮鍋婇、姘跺焵椤掑嫮宓侀柟鐑樺殾閺冨牆鐒垫い鎺嗗亾婵炴彃娼″缁樻媴閸濄儳楔濠碘槅鍋夊▔鏇犲垝閸喐濯撮柧蹇涒偓娑氱憹闂備礁鎼粙渚€宕㈡禒瀣；闁斥晛鍟扮粻楣冩煕閳╁喚娈樼紒鐘靛閵囧嫯绠涢敐鍕泿闂佸疇顫夐崹鍧椼€佸☉妯锋婵炲棗绻愰弨顓㈡⒒娴ｅ憡鎯堥柡鍫墰缁瑩骞嬮敃鈧悡婵嬪箹濞ｎ剙鈧鎮块埀顒勬⒑閹稿海绠撻柡灞斤龚椤﹁鎱ㄦ繝鍛仩缂佽鲸甯掕灒闁兼祴鏅濋弳顐︽⒒娴ｈ櫣甯涙い銊ョ墛閹便劑骞橀鍡欑◤闂婎偄娲︾粙鎺楀磹缂佹ü绻嗘い鏍ㄧ箥閸ゆ瑦銇勯鐔峰⒉缂佺粯绋撻埀顒傛暩椤牏鏁崼鏇熺厽閹烘娊宕濋幋锔衡偓渚€骞樼拠鎻掑敤濡炪倖鎸鹃崯鍧楀箯閾忓湱纾藉ù锝呭閸庡繘鎮樿箛鎾村殗鐎殿喓鍔戦幊锟犲Χ閸モ晪绱查梺鑽ゅТ濞测晝浜稿▎鎰珷闁挎洖鍊归悡娆愩亜閺冨浂鍤欑€规挸妫欓妵鍕閿涘嫭鍣伴梺鍝勮閸旀垿銆佸☉姗嗘僵妞ゆ劑鍩勫Λ婊堟⒒閸屾瑧鍔嶉悗绗涘懏宕查柛灞句緱濞堜粙鏌熼梻瀵割槮闁告垹濞€閺岋綁寮崹顔藉€梺缁樻尰閻熲晠骞冨鈧幃娆撳箵閹哄棗浜鹃柟闂寸缁€澶愭煥閺囩偛鈧綊鎮″☉銏″€堕柣鎰絻閳锋棃鏌曢崱妯虹瑨闂囧鏌ｅ▎蹇斿櫧闁伙絿鏁搁埀顒冾潐濞叉鍒掕箛娴板洭骞庨懞銉у幈闂佹寧绻傛绋库枍瀹ュ棙鍙忓┑鐘插亞閻撹偐鈧娲樼敮鎺楋綖濠靛绀傚鍓佹婵″洨妲愰幘瀛樺闁告挸寮舵闂備浇妗ㄩ悞锕傚箖閸岀偟宓侀柟杈剧畱椤懘鏌ｅ鍡椾簼妞ゎ偄绉瑰娲濞戞氨顔婃繝娈垮枤閸忔﹢銆佸Ο濂芥椽顢旈崨顏呭缂傚倸鍊烽悞锕傛晪婵犳鍠栭悧濠勬崲濞戙垹绀傞柤娴嬫櫆閸ｄ即鎮楀▓鍨灈闁哥喕鍎婚悘鍐⒑閸涘﹣绶遍柛鐘冲浮楠炲繘鎼归崷顓狅紳婵炶揪绲芥竟濠囧磿閹惧绠惧璺侯儐缁€瀣偓娈垮枙閸楀啿鐣烽幒妤佸€烽悗鐢殿焾楠炲牆鈹戦悩鍨毄濠殿喗鎸冲畷顖炲箥椤斿彞绗夐梺鍦亾閻ｎ亝绂嶅鍫熺厸闁搞儲婀圭花鐣岀磼鐎ｎ厼鍚圭紒杈ㄥ笧缁辨帒螣閼测晝鏆ゆ俊鐐€ら崑鍛崲閸儯鈧礁螖閸涱厾锛滃┑顔斤公缁茶姤绂嶆ィ鍐╃厽闁归偊鍠栭崝瀣煃闁垮鐏撮柡灞剧洴楠炲洭顢楅埀顒傜棯瑜旈弻鐔碱敍濮樺崬鈪甸梺鍝勬湰閻╊垶骞冮姀銏″磯闁告繂瀚妶顕€姊绘担鑺ャ€冪紒鈧担铏圭煋鐟滅増甯掔粻鏍ㄧ箾閸℃ɑ灏伴柛濠勭帛娣囧﹪顢涢悙瀛樻殸闁诲孩鑹鹃妶绋款潖缂佹ɑ濯撮柦妯侯槸閹偤姊洪崫銉バｉ柛鏃€鐗滅划鈺呮偄閻撳骸鑰垮┑鐐村灦椤洭顢欓幋锔解拺缂備焦锕╅悞鐣岀磼椤曞懎鐏﹂柟顕嗙節閹垽宕楅懖鈺佸箺闂備礁鎼崯鐘诲磻閹惧绠鹃柛蹇氬亹閹冲洭鏌熼鏂よ€块柟宕囧█椤㈡牠鎸婃径澶婎棜?
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


// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣濠垫劖缍楅梺閫炲苯澧柛濠傛健楠炴劖绻濋崘顏嗗骄闂佸啿鎼鍥╃矓椤旈敮鍋撶憴鍕８闁告梹鍨甸锝夊醇閺囩偟顓洪梺缁樼懃閹虫劙鐛姀銈嗏拻闁稿本鐟х粣鏃堟煃瑜滈崜娑㈠磻濞戙垺鍤愭い鏍ㄧ⊕濞呯娀鏌熺紒銏犳灍闁绘挻鐩幃姗€鎮欓幓鎺嗘寖闂侀潧妫欑敮锟犲蓟瀹ュ牜妾ㄩ梺鍛婃尪閸斿海妲愰悙鍝勫耿婵炴垶顭囬敍娑㈡⒑閸涘﹣绶遍柛姗€绠栧鎶芥晜闁款垰浜鹃柛蹇擃槸娴滈箖姊洪崨濠冨闁告挻鐩畷銏ゅ箹娴ｇ懓鈧敻鏌涜箛鎿冩Ц濞存粓绠栭弻锝嗘償椤栨粎校闂佸憡鎸婚惄顖炲极瀹ュ鍋勯柛婵勫劤椤旀洟鏌ｆ惔锝嗘毄妞ゎ厼鐗撻、鎾诲箻閺傘儲鏂€闂佺偨鍎村▍鏇㈠窗濡椿娈介柣鎰皺缁犲鏌熼瑙勬珖闁归濞€閹崇娀顢楁径濠冩澑闂傚倸鍊风粈浣革耿闁秴纾块柕鍫濐槸閺勩儵鏌涢锝囩闁绘帊绮欓弻鏇熷緞閸繂濮夐梺琛″亾濞寸姴顑嗛悡鐔镐繆椤栨繃顏犲ù鐘崇洴閺岋繝宕辫箛鏃€鐝氶梺鍝勬湰缁嬫捇鍩€椤掑﹦绉甸柛鎾寸懄缁傛帡鍩￠崨顔惧帾闂佹悶鍎崝灞炬叏瀹ュ洠鍋撶憴鍕闁挎洏鍨介崹楣冩晝閸屾氨顓洪梺缁樕戣ぐ鍐╊殭闂傚倸鍊搁崐鐑芥嚄閸洖纾婚柕濞炬櫅绾惧潡鏌＄仦璇插姎缂佲偓閸喐鍙忔俊顖涘绾箖鏌涘顒傜Ш闁哄矉绻濆畷鍫曞煛娴ｅ湱鈧厽绻涚€涙鐭嬬紒顔芥崌瀵鎮㈤崗鐓庘偓缁樹繆椤栨繃顏犲ù鐘虫尦濮婃椽鏌呴悙鑼跺濠⒀屽枤缁辨挸顓奸崪鍐ㄤ紣濡炪倖娲╃紞浣哥暦濡警鍟呮い鏃€鍎冲鎶芥⒒娴ｅ憡鍟炴繛鎻掔Ч瀵彃鈻庨幋锝呅℃繝鐢靛У绾板秹鍩涢幋锔界厱婵犻潧妫楅鈺呮煃瑜滈崜姘跺箖閸岀偛绠栨俊顖濄€€閺€浠嬫倵閿濆簼绨介柛姗€浜跺娲传閸曨偅娈梺缁橆殔閿曨亪骞冮敓鐘冲亜闁告縿鍎抽鏇㈡⒑閻熼偊鍤熼柛瀣枑閺呭爼顢旈崼鐔哄幈闂佸搫鍟幐楣冩偩閻㈠憡鐓?sniroute.cmd update check闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖闂備礁鎲￠幐鍡涘川椤旂瓔鍟呴梻鍌欐祰椤曆呪偓娑掓櫊閹虫繃銈ｉ崗顖氭处鐎靛ジ寮堕幊绛圭畵閺屾盯寮撮妸銉т哗缂備胶濮寸壕顓㈠焵椤掆偓缁犲秹宕曢崡鐐嶆盯顢橀悙宥囧枛瀹曟帒顭ㄩ崨顖ょ闯濠电偠鎻徊鍧楁偤閺冨牆鍚规繛鍡樺姃缁诲棙銇勯幇鍓佹偧缂佺姷鍋熼埀顒侇問閸犳盯顢氳閸┿儲寰勬繛銏㈠枛閺屻劎鈧絽鐏氱紞灞解攽閻樻剚鍟忛柛鐘愁殜閵嗗啴宕ㄧ€涙ê浜辨繝鐢靛Т濞层倝寮告笟鈧弻娑㈠箻濡も偓閹虫劙鏁嶅┑瀣拺缂佸瀵у﹢鐗堟叏濡濡奸柣鈽嗗幗缁绘繈鎮介棃娑楃捕濠碘槅鍨伴敃顏勭暦閿濆绀冩い顓熷灥濞堛劍绻濋悽闈浶ｇ痪鏉跨Ч瀹曟垿宕熼娑氬幗闂佽宕樺▍鏇㈠箲閿濆鐓熼柨婵嗩槺閻ｆ椽鏌熼鐓庢Щ妞ゎ厹鍔戝畷姗€顢撻鍡楁噳閸嬫挸鈻撻崹顔界仌濠电偛鎳忓ú鏍€﹂崶顏嗙杸婵炴垶顭傞埡鍛叆闁哄洦顨呮禍楣冩倵濞堝灝鏋涙繛灏栤偓鎰佹綎婵炲樊浜滅粻浼村箹鏉堝墽鎮奸柣锝囨暬濮婃椽鎮烽弶鎸幮╁銈嗗灥閹虫捇鎮橀幒妤佲拺闂侇偆鍋涢懟顖涙櫠鐎涙ɑ鍙忓┑鐘叉噺椤忕娀鏌熸搴♀枅闁搞劑绠栭幖褰掝敃閵堝嫰鍋楃紓鍌氬€搁崐鎼佸磹閸濄儳鐭撻柟缁㈠枟閺呮繃銇勮箛鎾跺闁稿蓱閵囧嫰寮村Δ鈧禍楣冩倵鐟欏嫭绀€闁绘牕銈搁妴浣肝旀担鍝ョ獮闁诲函缍嗛崑鍛存偟濠靛鈷掗柛灞剧懆閸忓瞼绱掗鍛仩閾荤偛霉閿濆洨銆婇柡瀣Ч閺屻劌鈹戦崱鈺傂﹂梺缁樻尰缁诲牓鎮￠锕€鐐婇柕濞р偓婵洭姊洪幎鑺ユ暠閻㈩垱甯″﹢渚€姊洪幐搴ｇ畵闁绘锕、鏃堟偐缂佹鍘梺绯曞墲椤ㄦ劙鐓鍕厸鐎光偓鐎ｎ剛鐦堥悗瑙勬磸閸旀垿銆佸▎鎾村€锋い鎺嗗亾闁搞劑浜跺缁樻媴閻戞ê娈岀紒鍓ц檸閸欏啫鐣锋导鏉戝唨妞ゆ挻澹曢崑鎾存媴缁洘顫嶉梺闈涚箚閳ь剚鏋奸崑鎾寸節濮橆厾鍘遍梺闈涱槶閸ㄥ搫鈻嶉崶顒佺厓缂佹鎼埀顒佺墱閹广垹鈹戠€ｎ偄浠洪梻鍌氱墛缁嬪牓寮搁崨顓涙斀闁绘劖婢樼亸鍐煕閵夈劌鐓愰柡鍛仱濮婃椽宕ㄦ繝浣虹箒闂佹悶鍔岀壕顓熺閹间焦鍋ㄧ痪鐗埫禍鐐殽閻愯尙浠㈤柛鏃€纰嶉妵鍕晜閸喖绁梺璇″櫙缁绘繂鐣峰鈧、鏃堝川椤撶喓鈧偓绻濋悽闈浶㈤柨鏇樺€濋幃褔宕卞▎鎴犵劶闂佸壊鍋侀崕鏌ユ偂閻斿吋鐓涢柛鎰╁妿婢ц鲸淇婇锝呯伌闁哄矉绻濆畷閬嶎敇閻戝棙鍠橀梻渚€鈧偛鑻晶浼存煙閾忣偅灏甸柍褜鍓氶崙褰掑矗閸愩劎鏆﹂柟鎯版缁狙囨煙閻愯棄濡芥俊鐐扮矙閵嗕線寮▎鎯ф倯闂佺硶鍓濊摫闁绘繃娲熷濠氬磼濞嗘埈妲梺瑙勭ゴ閸撴繄绮悢鑲烘棃宕ㄩ鐐碘偓顓㈡⒑鐟欏嫷鍟忛柛鐘愁殜瀵啿顭ㄩ崼鐔哄幍闂佸憡鎸嗛崨顓狀偧闂備焦濞婇弨閬嶅垂閸ф绠栫憸鐗堝笒閻愬﹥銇勮箛鎾愁伀婵絻鍨荤槐鎾存媴閸濆嫅锝呪攽閻愯韬€殿喛顕ч埥澶婎潩閿濆懍澹曢梺鎸庣箓妤犲憡鏅堕鈧弻锛勨偓锝庡亞閵嗘帡鏌嶈閸撴繈锝炴径濞掑搫螣婵傝姤妞介幃銏ゆ偂鎼达絿鏆梻浣风串缁蹭粙鎯夋總鍛婂剹婵炲棙鎸婚崑锝呫€掑鐓庣仯闁告ɑ绮庣槐?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆ繝鈧潏鈺傤潟闁圭儤鏌￠崑鎾绘晲鎼粹€插缂備焦澧庨弲婊呮崲濞戞﹩鍟呮い鏃囧吹閸戝綊姊虹粙娆惧剱闁告梹鐟╅獮鍐亹閹烘垹鍊炲銈呯箰鐎氼喖袙閵忋倖鈷戦柟绋挎捣閳洟鏌ｉ埡濠傜仩妞ゎ偄绻愮叅妞ゅ繐鎳庢禒顓㈡⒑閸濆嫷妲哥紒銊╀憾楠炴劙宕橀鍡欙紳婵炴挻鑹惧ú銈呪枍濮椻偓閺屾盯濡搁妸锔俱€愰梺浼欑悼閸忔﹢鐛€ｎ喗鏅滈柤鎭掑劜椤忕喖姊绘担铏瑰笡闁挎氨鐥紒銏犲籍鐎规洏鍨介獮鏍ㄦ媴閸忓瀚奸梻浣侯攰閸嬫劙宕戝☉銏犵婵せ鍋撻柡灞剧洴瀵剟宕稿Δ鈧浼存⒑闂堟稒顥滈柛濠傛健楠炲﹪鎮╁ú缁樻櫌闂佺鏈划锝囨閹绘巻鏀介柣鎰煐瑜把呯磼闊厾鐭欐鐐寸墵椤㈡洟鏁傜悰鈥充壕闁稿本绋撻梽鍕煕濞戞﹫鍔熼柛姗嗕邯濮婃椽骞栭悙鎻掑Η闂佸憡鍔曡ぐ鐐垫閺冨牊鐓熼幖娣焺閸熷繘鏌涢敐搴℃珝鐎规洘濞婇弫鎰緞婵犲啯袣闂備線鈧偛鑻晶顖炴煏閸パ冾伃妤犵偞甯￠獮瀣攽閹邦亞妫梻鍌欒兌椤牓顢栭崨鎼晞濠㈣埖鍔曢拑鐔兼煥濠靛棙鍟掗柣鏂垮悑閸婄粯淇婇鐐存暠闁诲繐妫欑换?GitHub API闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖濠德板€х徊浠嬪疮椤栫儐鏁佺€广儱顦伴埛鎴犵磼鐎ｎ偒鍎ラ柛搴＄箲閵囧嫰骞嬪┑鎰枅闂佽鍣换婵嬨€侀弴銏℃櫇闁逞屽墴閹瑦绻濋崶銊у帾婵犵數鍋涢悘婵嬪礉濮樿埖鐓涢悗锝庡亞閹冲懐绱掓潏銊﹀磳鐎规洘甯掗～婵喰掑▎娆戠М闁哄矉绲借灃闁告劑鍔夐弸鍛存倵濞堝灝鏋︽い鏇嗗洤鐓″鑸靛姇椤懘鏌ｅ鈧褎鎱ㄩ崼鏇熲拻濞达絽鎲￠崯鐐烘煕閺冩捇妾紒鍌氱Ч瀵粙鈥栭濠勭М鐎规洖銈告俊鐑芥晜閹冪闂備胶鎳撻崥瀣偩椤忓牆鍨傞柛蹇氬亹閻濊埖淇婇娑氭菇濞存粍绮撻弻銊╁籍閸ヨ泛娈梺璇查獜缁犳捇寮诲澶娢ㄦい鏍ㄥ嚬濞差參姊洪崷顓熷殌閻庢氨澧楁穱濠囧箹娴ｈ娅囬梺閫炲苯澧板瑙勬礋椤㈡稑鈽夊槌栧晭闂備礁婀遍崑鎾愁焽濞嗘挸鍚规い鎺戝閻撴洟鏌曟繛鍨闁告繄鍎ら妵鍕Ω閿旂偓鍣ч梺瀹狀潐閸ㄥ潡骞冮埡鍛瀭妞ゆ劧绲鹃惁搴ㄦ⒒娴ｄ警鐒炬い鎴濇閹嫰顢涘┑鍥舵（濠碉紕鍋戦崐鏍箰閸洖鍨傜憸鐗堝笒閸ㄥ倿鏌涚仦缁㈠殧鐟滅増甯楅崑鎰偓鐟板閸犳牠宕滈幎鑺ュ€甸悷娆忓鐏忣偆绱掗懜闈涘摵鐎殿喖顭烽崹楣冨箛娴ｅ憡鍊梺纭呭亹鐞涖儵鍩€椤掑啫鐨洪柣鏍憾濮婄粯鎷呮笟顖滃姼濡炪倖鍨靛Λ婵嬬嵁閹版澘绠瑰ù锝呮憸閸欌偓濠电姰鍨煎▔娑滃闂佸搫顑呯粔褰掑蓟瑜戠粻娑㈡晲閸涱剛鐩庨梻浣筋嚙缁诲牓宕曢悽绋胯摕闁挎繂顦伴弲鏌ユ煕閵夋垵鍟粻锝夋煟鎼淬値娼愭繛鍙夌矒瀹曚即寮介婧惧亾娓氣偓瀵粙顢橀悙鐢靛幀闂備線娼ч敍蹇涘川椤旇姤鐝ら梻鍌氬€搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾鐎规洦鍨跺畷绋课旈埀顒傜不鐟欏嫨浜滈柟鎷屾硾閻︽粓鏌℃担绋款伃闁哄本绋戦埥澶娾枎閹寸偞顔冮梻渚€鈧偛鑻晶顕€鏌熼搹顐€块柕鍡曠铻栭柛娑卞幘椤︽澘顪冮妶鍡楃瑨闁稿﹥鎮傞悰顔碱吋婢跺鍘介柟鑹版彧缁查箖宕甸埀顒勬煟鎼淬垻鐓柛妤€鍟块悾鐑藉箣閻愮數鐦堥梺鎼炲劀閸滃啰鍚归梻鍌欑閹碱偊宕愰崫銉ㄥС闂侇剙绉寸紒鈺伱归悩宸剱闁绘挸鍟撮弻鏇熷緞閸繂濮㈤梺鎸庣☉缁夊綊寮诲☉銏犵厸閻庯綆鍓氶崑褔姊洪柅鐐茶嫰婢у弶銇勯銏╂Ц閻撱倝鏌″搴″箹閸ユ挳姊洪崨濠勨槈闁挎洏鍎靛畷锝堢疀濞戞瑧鍘撻悷婊勭矒瀹曟粓鎮㈤悡搴ゆ憰閻庡箍鍎遍ˇ鎵偓鐢靛Т椤法鎹勬笟顖氬壘濠碘槅鍋撶槐鏇犳閹捐纾兼慨妯荤樂閵忋倖鐓熸い鎾跺仜閳ь剚鎮傞敐鐐剁疀閺冨倻鐦堝┑顔斤供閸撴盯鏁嶅鍐ｆ斀闁绘劕寮堕ˉ鐐烘煕閵娿儳浠涘ǎ鍥э攻缁傛帞鈧綆鍋嗛崢浠嬫煙閼圭増褰х紒鎻掓健楠炲﹤螖閸涱喚鍘卞┑鐐村灥瀹曨剟鎮橀敐澶嬬厽闁规儳鐡ㄥ畷灞炬叏婵犲懏顏犻柟鍙夋尦瀹曠喖妫冨☉妤勭箷闂備浇顕х换鎺楀磻濞戙垹纾归柛娑橈攻閸欏繘鏌涘畝鈧崑鐐烘偂閺囥垺鐓冮悷娆忓閸斻倕霉濠婂懎浜惧ǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍋呴弳婊堟煙閻戞﹩娈旂痪鎯х秺閺屻倖鎱ㄩ幇顑藉亾閺団懞澶婎潩椤戣姤鏂€闂佺鏈划鐘绘倿閸撗呯＜闁绘瑦鐟ュú锕傛偂閺囩喆浜滈柟鏉垮閹ジ鏌嶉挊澶樻Ц闁宠鍨垮杈ㄧ箾婢跺绀堟俊鍙夊姍楠炴帒螖娴ｉ晲姹楅梻鍌氬€搁悧濠勭矙閹邦兘鏌︽い蹇撳濞撳鏌曢崼婵嗘殭濠碘剝鎮傞弻娑欐償閿濆棙姣堝Δ鐘靛仜閸燁偊鍩㈡惔銊ョ闁哄鍨堕缁樹繆閻愵亜鈧牠宕洪崼銉ョ９闁靛牆顦悙濠囨煏婵炑冨暙缁犵偤姊婚崒娆戭槮闁汇倕娲敐鐐村緞閹邦剛锛熼梺鎯ф禋閸嬪棝銆呴弻銉︾厽闁逛即娼ч崢鍝ョ磼鐠囧弶顥㈤柡宀嬬秮楠炲洭宕楅崫銉ф晨闂備線鈧偛鑻晶顖炴煟濡や胶鐭掔€殿噮鍋婇、娆撴偩瀹€鈧惁鍫ユ⒑閹肩偛鍔橀柛鏂胯嫰閳绘捇骞嬮敂瑙ｆ嫼闂佸憡鎸昏ぐ鍐╃濠靛洨绠鹃柟缁樺笧缁犲鏌涢埞鎯т壕婵＄偑鍊栧濠氬磻閹剧粯鐓欓柧蹇ｅ亾閼版寧顨ラ悙鎻掓殭妞ゎ厹鍔戝畷濂告偄闁垮鏋€闂傚倷绶氬褔鏁嶈箛娑樼闁靛鍎哄ú顓熺節閻㈤潧啸闁轰礁鎲￠幈銊﹀閺夋垵鐎繝闈涘€婚…鍫ュ及閵夆晜鐓涚€广儱楠搁獮妤呮煕鎼粹€愁劉闁靛洤瀚板浠嬪Ω閵夈儲鐦撻梻浣筋嚙濮橈箓宕洪弽顓炍﹂柛鏇ㄥ灠閸楄櫕淇婇妶鍌氫壕婵炲瓨绮撶粻鏍ь潖濞差亜浼犻柛鏇ㄥ亽濡茬敻姊洪崨濠冨鞍缂佽鍊块、姘跺Ψ閳轰胶顦板銈嗗笒閸婂鎯侀崼銉︹拺闁告稑锕ユ径鍕煕濡崵鐭掑┑鈩冩倐閸╋繝宕掑Δ浣割伖闂傚倷绀侀幉锛勬崲閸屾壕鍋撳鐓庡箹闁挎洏鍨介獮宥夘敊閸撗嶇床闂佽鍑界紞鍡涘磻閸曨垱鍊堕柟缁㈠枟閻撱儲绻濋棃娑氬濞寸姵绮撻弻宥囨喆閸曨偆浠奸梺閫炲苯澧剧紒韫矙閹ê顫濈捄铏规煣闂侀潧绻堥崐鏍煕閹达附鐓曟繛鎴烇公閺€鑽ょ磼閳ь剟宕奸悢缈犵盎闂佹寧绻傜€氼噣鎯屽▎鎴斿亾鐟欏嫭绀冮柛銊ユ健閻涱喖螣娓氼垳鍞甸梺鍛婃处閸撴岸藟閸懇鍋撶憴鍕闁挎洏鍨藉畷娲焺閸愨晛顎撻梺闈╁瘜閸樻悂宕戦幘娣亝闁告劏鏂侀幏缁樼箾鏉堝墽鍒伴柟璇х節楠炲棝宕奸悢缈犵盎闂佹寧鏌ㄩ悘婵嗙暤閸℃ǜ浜滈柕蹇ョ磿婢ь亪鏌嶇拠鏌ュ弰妤犵偞锕㈠畷姗€宕ｆ径濠傚闂傚倸鍊烽懗鍓佸垝椤栫偛钃熼柕濞炬櫅鐟欙箓鎮楅敐搴℃灍闁稿鍊块弻锝夊棘閸喗鍊梺缁樻尭缁绘劙鈥﹂崸妤佸殝鐎电増顨忔禍顏勵嚕缁嬪簱鏋庨柟瀵稿Х閿涙粓姊洪棃娑辩劸闁稿﹥顨婇幆灞解枎閹惧鍘搁悗鍏夊亾闁逞屽墴瀹曚即寮介鐐电暫闂佺粯鍨煎Λ鍕棯瑜庣换娑㈠幢濡も偓缂嶆牜绱掓潏銊︾叆闁宠鍨块幃鈺佲枔閹稿孩鐦滈梻浣告啞閹歌崵鎹㈤崼銉у祦闁告劦鍠栫壕鍏肩箾閹寸偟鎳冪€殿喖娼″娲焻閻愯尪瀚板褎鎸抽弻锛勪沪鐠囨彃顬堥梺瀹犳椤︻垵鐏掓繛鎾村嚬閸垶骞冮敐鍡曠箚闁靛牆娲ゅ暩闂佺顑囬崑銈夊极閸愵喖顫呴柣娆屽亾婵炲吋鐗曢埞鎴︽偐鐎圭姴顥濈紒鐐劤閸氬鎹㈠☉銏犲耿婵☆垵娅ｆ禒瀛樼節绾版ǚ鍋撻懠顒傜厯闂佸搫鐭夌徊楣冨箚閺冨牊鏅查柛銉ｅ妼濞堟垿姊绘担鍛婂暈濞撴碍顨婂畷褰掑箮閼恒儱浜楅梺鐟板⒔缁垶鍩涢幋锔界厽闁绘梻鍘ф禍浼存煕閵堝洤鏋涢柡灞剧〒閳ь剨缍嗛崜娆撳几濞戞瑦鍙忓┑鐘辫兌閻瑧鈧娲滈崰鏍€佸☉妯锋婵﹩鍓欑粻浼存⒒閸屾瑧顦﹂柟璇х節瀹曟繈骞嬮敃鈧粈澶屸偓鍏夊亾闁告洦鍋嗛崝锕€顪冮妶鍡楃瑨闁哥噥鍋婂畷銉╁捶椤撶姷锛滃銈嗘⒒閳峰牓鎮￠懖鈹惧亾濞堝灝娅橀柛鎾寸懇閸┿垺鎯旈妸銉ь槶閻熸粌绉瑰顐﹀箹娴ｅ厜鎷绘繛杈剧悼椤牓骞冮幋锔界厵妞ゆ梻鍘ч埀顒佹倐閳ワ箓宕堕宥喰梻渚€鈧稓鈹掗柛鏃€鍨甸悾閿嬬附缁嬭銊╂煥閺冣偓閸庢娊鐛崼銉︹拻濞撴埃鍋撴繛浣冲洦鍋嬮柛鈩冪☉缁犵娀骞栭幖顓犲帥闁轰礁鍊归妵鍕箻閸楃偟浠鹃梺缁樻尰缁嬫捇鍩€椤掆偓閸樻粓宕戦幘缁樺€甸柨婵嗛婢т即鏌￠崱娆忊枅闁哄睙鍕嚤婵炲棙鍨甸崺宀勬⒑闂堟稒顥為悽顖涘浮閿濈偛鈹戠€ｎ偄浜楅柟鑹版彧缁插ジ鎮介懡銈囩＝濞达綀顕栧▓鏇㈡煕閵娿劌鐓愮紒宀冮哺缁绘繈宕堕‖顑洦鐓曢柕澶樺枤娴犮垽鏌曟径鍡樻珕闁绘挻娲熼幃妤呮晲鎼粹€茬盎闂佽绻愰顓㈠焵椤掑喚娼愭繛鍙夌矒閵嗕焦绻濋崶顬箓鏌熼悧鍫熺凡缂佺姵濞婇弻鐔煎箚瑜嶉。鎶芥煛閸℃劕鐏叉慨濠傤煼瀹曟帒顫濋钘変壕闁绘垼濮ら崐鍧楁煥閺冨牊鏆滈柛瀣尵缁厼鈽夊Ο璇叉閻庤娲橀悡锟犲蓟濞戞鏆嗛柍褜鍓熷畷鎴濃槈濮樺彉绗夐梺缁橆焽缁垶鎮″▎鎾粹拺妞ゆ挶鍔庨悾閬嶆煛鐎ｃ劌鈧洟鍩ユ径鎰鐎规洖娉﹁閺屸€崇暆閳ь剟宕伴幘鑸殿潟闁圭儤鍤﹂悢鍛婂闁哄鐏濋悘渚€姊婚崒娆掑厡缂侇噮鍨跺濠氬Ω閵夊孩绋戦埥澶愬閻樼數鏋€缂傚倷绀侀鍡涱敄濞嗘挻鍋傞柕澶嗘櫆閻撴洘銇勯幇鍓佹偧缂佺姵锕㈤弻锝夊箻閺夋垹浼屽┑顔硷攻濡炶棄鐣烽妸锔剧瘈闁稿本顕撮弴銏♀拺闁告縿鍎辨牎闂佺粯顨嗙划搴ㄥ礆閹烘梹瀚氱€瑰壊鍠楅崓鐢告⒑閻撳孩鎲搁柡浣规倐瀹曘垽鏌嗗鍡忔嫼闂佸憡绻傜€氼參宕掗妸鈺傜厱闁哄倽娉曢悞鎼佹煕閳规儳浜炬俊鐐€栫敮鎺楀磹閹间礁鐓濋柛顐犲劜閻撴瑦銇勯弬鎸庢儓闁诲繗灏欓埀顒冾潐濞诧箓宕戞繝鍌滄殾闁绘梻鈷堥弫鍐┿亜閹扳晛鐏い鏂垮濮婂宕掑顑藉亾妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠煕濮橆厽銇濆┑鈩冩倐閸┾剝鎷呴悮瀵稿耿濠电姷顣藉Σ鍛村垂闂堟稓鏆﹂柣銏犳啞閸?
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
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊閻樺樊妫岄梺杞扮閿曨亪寮婚垾鎰佸悑閹肩补鈧磭顔愮紓鍌欑劍閸旀牠銆冮崱妯尖攳濠电姴娲ゅ洿闂佸憡渚楅崣搴㈢妤ｅ啯鈷戦柣鐔告緲閺嗛亶鏌￠崼顐㈠闁瑰箍鍨归埞鎴﹀幢韫囨梻浜栭梻浣稿閻撳牓宕板Δ鍛€堕柛顐犲劜閳锋垿鏌熺粙鍨劉缁剧偓鎮傞弻娑欑節閸愮偓鐣煎┑鈥冲级閸旀瑩骞冨▎鎴斿亾闂堟稒鎲搁柣锕€鐗撳铏圭矙閹稿孩鎷辩紓渚囧枛缁夋挳鍩㈤幘娣亝闁告劏鏅涢埀顒傛暬閺岋綁鎮㈤崨濠勫嚒濡炪倕娴氭禍顏堝蓟濞戙垺鏅查柛鈩冾焽缁嬪洭鏌ｉ幘鍗炩偓鏍Φ閸曨喚鐤€闁瑰墽绱熼敐澶嬬厸闁告侗鍠氶惌鎺撴叏婵犲啯銇濈€规洏鍔嶇换婵嬪礃椤垶袩闂傚倷绀侀幉锟犳晪闂侀潧鐗婇幃鍌炴偘椤斿槈鏃堝礃閿濆懍澹曢梺鎸庣箓妤犳悂寮搁悢鍏肩厓闂佸灝顑呯粭鎺楁婢舵劖鐓ユ繝闈涙閸ｆ椽鏌涢悢鍝勪槐闁哄本鐩幃銏ゅ传閸曨亝顫曢梻浣告惈閻绱炴笟鈧獮鍐Χ婢跺﹦锛滃┑鐘诧工閹叉稑顭ㄩ崼鐔封偓鐢告偡濞嗗繐顏紒鈧崘鈺冪闁肩⒈鍓欓弸娑㈡煟濞戝崬鏋熼柟顖涙婵℃悂鏁冮埀顒€鈻撻悢鍏尖拺闂傚牊渚楀Σ鍫曟煕鎼存繂宓嗛柟顔炬櫕缁辨帒螣閼测晝妲囬梻浣圭湽閸ㄨ棄顭囪閸┿垽宕奸悢铏诡啎婵炶揪绲介崯鍧楀触濡　妲堟慨姗嗗亝閸曞啴鏌℃径灞戒粶閻忓繐鎳愮槐鐐存媴閸濄儲娈炬繝闈涘€婚…鍫濇暜闂備焦瀵у褰掑磿閹剁晫宓佺€广儱顦伴埛鎺懨归敐鍫燁仩閻㈩垱鐩弻娑㈠籍閹惧墎鏆ら悗瑙勬礃缁矂鍩㈡惔銊ョ疀妞ゆ挾鍠撹ぐ鎸庝繆閻愵亜鈧牠鎮у鍫濈；闁绘劗鍎ら崑鍌炴煛閸ャ儱鐏╃紒鐘茬秺瀵爼宕煎┑鍡忔寖缂佺偓宕樺▔鏇犳閹烘鍋愮€规洖娲ゅ銊╂⒑閸濆嫭婀版繛鑼枎閻ｇ兘骞嗛柇锔叫╂俊鐐€曞ù姘跺矗閸愵喖钃熼柍鈺佸暞婵绱掔€ｎ偄顕滄繛鍫熺叀濮婃椽鎮烽弶鎸幮╅梺纭呮珪閿氶柣锝囧厴閹粎绮电€ｎ偅娅嶆繝鐢靛閸曨偄濮㈡繛瀛樼矋閸庢娊鍩為幋锔藉亹闁告瑥顦ˇ鈺呮⒑閹惰姤鏁遍柛銊ユ健楠炲啳銇愰幒鎴犲€為梺鎸庣箓閸婂憡绂嶆ィ鍐╃叄闊浄绲芥禍鐐裁瑰鍡楃厫缂佺粯绻堥幃浠嬫濞村墎鍋ら弻娑㈠Ω閿斿浠㈤梺杞扮閸熷瓨鎱ㄩ埀顒勬煏閸繃鍣介柣锝嗘そ濮婅櫣鎷犻垾宕団偓濠氭煃瑜滈崜鐔奉嚕閵婏妇顩烽悗锝庡亞閸樺崬顪冮妶鍡楀Ё缂傚秮鍋撴繝銏ｎ潐閿氶棁澶嬬節婵犲倻澧㈤柣锝嗘そ閺岋紕浠︾拠鎻掝瀳闂佸疇顫夐崹鍨暦閸楃偐妲堥柟鍏哥娴滅偓淇婇妶鍌氫壕濡炪値鍙€閸庡藝闁秵鐓曟俊銈勭閳绘洜鈧鍠栭…鐑藉极閹版澘宸濇い蹇撴媼濞兼棃姊绘担鍛婃儓閻犲洨鍋ゅ畷姗€濡搁幇顓熺缂佺粯绻傞埢鎾诲垂椤旂晫浜梻浣瑰濞插繘宕曢柆宥佲偓鏃堝礃椤旇棄鐧勬繝銏ｆ硾閺堫剙鈻撴ィ鍐┾拺闂傚牊渚楅悡顓炩攽閳ヨ櫕宸濈€殿啫鍥х劦妞ゆ帒瀚埛鎴︽煕濞戞﹫鏀诲璺哄閺屾盯骞樼€靛憡鍣ч梺瀹狀嚙缁夌兘藝閺屻儲鐓冮悷娆忓閻忔挳鏌熼鐣屾噰闁瑰磭濞€椤㈡鎷呴悷鐗堢亪缂傚倸鍊搁崐椋庢閿熺姴绐楅柡宥庡亞閻挾鎲搁悧鍫濈瑨缂佺姵鐗犻弻锝夊Χ鎼达紕浼囧┑鈽嗗灠鐎氼剝鐏冮梺缁橈耿濞佳勭閿曞倹鐓曢柕濠忕畱椤曟粓鏌熸笟鍨闁糕斁鍋撳銈嗗笒鐎氼參鎮￠悢鍝ョ闁糕剝顨夌€氭澘霉濠婂嫬鍔ら柍瑙勫灴閹瑩鎳犻浣烘闂備礁鎼幊鎰箾閳ь剛鈧鍠栭…閿嬩繆閼搁潧绶炲┑鐘插閸熷洤鈹戦悙鑸靛涧缂傚秮鍋撳銈庡亜椤︾敻寮婚妸鈺婃晣闁绘棁娅ｉ鏇㈡煟鎼达絾鏆╂い顓炵墕閺嗏晠姊绘担鍛婂暈妞ゎ厼妫濆畷鎯邦槾闁告ɑ鎸冲铏规兜閸涱喖娑ч梻鍌氬鐎氭澘鐣烽幇鐗堝€婚柤鎭掑劤閸樺崬鈹戦悙鍙夘棞婵炲瓨鑹捐灒闁割偆鍠嶇换鍡樸亜閹扳晛鈧洘绂掕缁辨帗娼忛妸锕€闉嶉梺鐟板槻閹虫﹢骞栬ぐ鎺撳仭闁规鍣崑褔姊婚崒娆戭槮闁规祴鈧剚娼栫憸鐗堝笚閸嬶繝鏌曟繛鐐珕闁稿孩顨呰灃闁挎繂鎳庨弳鐐烘煃闁垮鐏撮柡灞剧☉閳藉顫滈崼鐕佹毉缂傚倷绀侀悥鍏肩閸洖钃熸繛鎴烆焸閻斿搫鏋堥弶鍫氭櫅婵ジ姊洪崨濠庢疁濞存粌鐖煎璇测槈閵忊晜鏅濋梺缁樕戣ぐ鍐╂叏閺囥垺鈷戠紓浣股戠亸浼存煟閻斿弶娅婄€殿喖顭烽幃銏㈠枈鏉堛劍娅栭梻浣虹《閸撴繈銆冮崨鏉戞辈闁靛繈鍊栭悡鏇㈡煃閸濆嫬鏆欏ù鐘洪哺閹便劍绻濋崘鈹夸虎闂佸搫鑻幊姗€骞冨▎鎾村殤閻犺桨璀︽导鍐ㄢ攽閻橆偅濯伴悘鐐垫櫕娴犳悂姊洪崫鍕効缂傚秳绶氬畷娲焺閸愵亞鐦堥梺绋挎湰缁牆顩奸幘鏂ユ斀闁绘ɑ顔栭弳顖炴煃瑜滈崜娆戝椤撱垹绠洪柣妯兼暩绾惧吋淇婇妶鍛殭闁哄鐩弻娑㈠煘閹傚濠碉紕鍋戦崐鏍暜閹烘柡鍋撳鐓庢灈闁伙絽鐏氱粭鐔煎焵椤掑嫬绠栨俊銈呮噺閺呮煡骞栫划鐟板⒉闁诲繐绉瑰娲传閸曨剙娅ょ紓浣筋嚙閻楁挸顕ｇ拠宸悑闁割偒鍋呴鍥⒒娴ｅ憡鍟為柟绋款煼閹嫰顢涢悙鍙夋К闂侀€炲苯澧柕鍥у楠炴帒顓奸崱娆嶅仒濠电姰鍨奸～澶庢懌闂侀潧娲ょ€氫即寮崒婊勫珰闁肩⒈鍏涘鎾绘⒒娴ｅ搫浠洪柛搴ｇ帛娣囧﹪宕堕鈧粻鏌ユ煕閺囥劌鐏犻幆鐔兼⒑閹稿孩纾甸柛瀣尵閻ヮ亞绱掗姀鐘典画缂備胶绮惄顖炵嵁濮椻偓瀵爼骞嬪┑鍥╂闂傚倷鐒﹂幃鍫曞礉鐎ｎ喖纭€闁告劘灏欓弳锔界節婵犲倻澧曠痪顓涘亾闂備胶绮崝蹇涘疾濠婂牆妫橀柍褜鍓熷缁樻媴閾忕懓绗￠梺鍛婃⒐濞叉牠顢氶敐澶婇唶闁哄洨鍋ゅΛ鐑芥⒑闂堚晛鐦滈柛妯哄⒔婢规洟鎳栭埡鍐紳婵炶揪绲垮▍銏ゅ礂椤掑嫭鐓犻柟宄版湰绾爼鏌嶈閸撴岸顢欓弽顓炵獥婵炴垯鍩勯弫瀣喐閺冨牆鏄ラ柕澶涚畱缁剁偤鏌熼柇锕€澧绘繛鐓庯躬濮婅櫣绱掑鍡欏姺闂佽桨鐒﹂幃鍌氼嚕閸洖宸濆┑鐘辫兌缁犳岸姊虹紒妯哄Е濞存粍绮撻崺鈧い鎺嶈兌婢ч亶鏌℃笟鍥ф灍缂佺粯绻堝畷鐔碱敇閻橀潧濮告繝鐢靛仩閹活亞寰婄捄銊﹀厹闁割偅娲栭崙鐘碘偓骞垮劚椤︿即鎮￠妷锔剧瘈闂傚牊绋掗ˉ鐐测攽椤栨艾鎮戠紒缁樼洴楠炴﹢骞囨担鍝勬倯闂備礁鎼張顒€煤閻旈鏆﹂柣鎾崇岸閺€浠嬫煕閵夈劌鐓愰柨娑楃窔濮婄粯鎷呴悷鎵虫灆闂佽　鍋撻梺顒€绉寸粈澶嬬箾閸℃ɑ灏柦鍐枑缁绘盯骞嬪▎蹇曚患缂備緡鍋勭粔褰掑蓟瀹ュ牜妾ㄩ梺鍛婃尵閸犳牠鐛崘顔藉€婚柤鎭掑劜濞呭棝姊烘导娆戝埌濠㈣泛娲?
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


// ---------------------------------------------------------------- GHA 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰惈閸ㄥ倿鏌涢锝嗙缂佺姳鍗抽弻鐔兼⒒鐎垫瓕绐楅梺杞扮鐎氫即寮诲☉銏╂晝闁挎繂娲╃粚鍧楁偡濠婂懎顣奸悽顖涘浮閹瑦绻濋崶銊у帗闁荤喐鐟ョ€氼剟宕濋妶鍚ょ懓顭ㄩ崼銏㈡毇闂佸搫鐭夌紞浣规叏閳ь剟鏌嶉挊澶嬵棞闁哄懘浜跺铏规偘閳ュ厖澹曞┑鐐差嚟閸樠囨偤閵娾晜鍋傞柣鏂垮悑閻撱儵鏌￠崶銊︾殤闁告柨绉甸妵鍕Ψ閵夘喗效闂?
// 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晜缂佹ɑ娅堥梻浣告啞濞诧箓宕归柆宥呭惞闁告洦鍨遍悡鏇熴亜閹板墎绋荤紒鈧崘顔界厱閻庯急鍕彆缂備胶绮换鍫澪涢崘銊㈡婵炲棗娴氬Σ杈╃磽閸屾艾鈧摜绮旈弶鎳ㄦ椽顢橀姀鐘烘憰闂佺粯姊婚崢褏绮诲杈ㄥ枑鐎广儱顦Ч鏌ュ级閸碍鏉归柛瀣尭閳绘捇宕归鐣屼粚婵犵數鍋涢幊蹇撁哄Ο鑲╃煔閺夊牄鍔庣弧鈧梺绋款儓婵倕螞閸愵喖鏄ラ柍褜鍓氶妵鍕箳閸℃ぞ澹曢梺鎹愬吹閸嬨倝骞冨Δ鍛棃婵炴垶鐟ラ弳鍫濐渻閵堝骸浜濇繛鑼枛瀵鎮㈢亸浣圭亖闂佸壊鐓堥崰妤呮倶閹惧墎纾奸柟顖嗗啰绋忕紓渚囧枟閻熲晛顕ｆ繝姘伋闁归绀侀幃鎴︽煙閼测晞藟闁逞屽墯閸撴艾顭囨ィ鍐┾拺婵懓娲ら悞娲煕閵婏附绶查摶鐐寸箾閸℃ê濮夐柍?+ 缂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ礃閹偤骞栧ǎ顒€濡奸柣顓燁殜楠炴牕菐椤掆偓婵¤偐绱掗幇顓ф疁闁哄备鈧剚鍚嬮幖绮光偓宕囶啈闂?闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愯弓鐢婚梻浣瑰濞叉牠宕愯ぐ鎺戠；閻庯綆鍋傜换鍡涙煏閸繃鍣归柡鍡欏枛閺岋綁顢橀悢绋跨３闂佸搫鐭夌紞渚€鐛€ｎ喗鏅查柛娑樻噺閹瑰洭寮婚敓鐘叉そ濞达絿顭堥～顐︽⒑闁偛鑻晶顕€鏌涘Ο鑽ょ煉鐎规洘鍨块獮妯肩磼濡厧甯楅柣鐔哥矋缁挸鐣峰鍫熷亜濡炲瀛╁▓鎯р攽閻愬弶顥為柟绋款煼瀹曢潧顓兼径瀣帾婵犮垼顕栭崹浼村嫉椤掑倻鐭堟い鎰堕檮閳锋帒霉閿濆洦鍤€妞ゆ洘绮庣槐鎺旀嫚閹绘巻鍋撻崫銉ュ疾闂備線娼ц墝闁哄懏鐩畷褰掑磼閻愬鍘甸梺璇″灡濠㈡顣块梻浣虹帛閹歌煤閻旂厧钃熼柣鏂垮悑閸婄粯淇婇婵愬殭闁哄棙顨婂铏光偓鍦У椤ュ銇勯敃浣峰惈婵″弶鍔欓獮妯兼嫚閼碱剨绱叉繝鐢靛仦閸ㄥ爼鎮烽妷褍顥氶柟鎯板Г閳锋垿寮堕悙鏉戭棆妞わ讣濡囩槐鎺撳緞鐏炴儳娈岄梺浼欑悼閸忔﹢骞冮埄鍐╁劅妞ゆ梹鍎冲鎶芥⒒娴ｅ憡鍟炲〒姘殜瀹曞綊鏌嗗鍛崶闂佸湱鍎ら〃鍡涘煕閹烘鐓曢悘鐐村礃婢规﹢鏌嶈閸撴盯鈥﹂崼銏犲疾闂備胶绮…鍫ヮ敋瑜忔竟鏇熺節濮橆厾鍘甸梺鍛婃寙閸涱厾顐奸梻?.rc 闂傚倸鍊搁崐宄懊归崶褏鏆﹂柛顭戝亝閸欏繒鈧箍鍎遍ˇ顖滅矆閸愨斂浜滄い鎾跺枎閻忥綁鏌ｉ妷顔荤敖缂佽鲸鎹囧畷鎺戔枎閹达絿鐛ラ梻浣虹帛閹告悂鎮ч幘宕囨殾闁规儼濮ら弲鎻掝熆鐠虹尨鏀婚柣搴墴閹鐛崹顔煎濠碘槅鍋呴〃鍫濃枎閵忋倖鍊烽柣鎴烆焽閸樺崬顪冮妶鍡楀Ё缂傚秮鍋撴繝銏ｎ潐閿氶棁?
// Enter = 缂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ礃閹偤骞栧ǎ顒€濡奸柣顓燁殜楠炴牕菐椤掆偓婵¤偐绱掗幇顓ф疁闁哄备鈧剚鍚嬮幖绮光偓宕囶啈闂備胶顭堥鍌炲疾濞戞粎浜欓梻浣瑰缁诲倿骞婃惔銊ユ辈闁绘劗鍎ら悡娑㈡煕濠娾偓缁€浣规櫠閹绢喗鐓欐い鏃傜摂濞堟棃鏌嶉挊澶樻Ц閾伙綁鏌ｉ幘宕囧哺闁稿鎹囬幆?= 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愯弓鐢婚梻浣瑰濞叉牠宕愯ぐ鎺戠；閻庯綆鍋傜换鍡涙煏閸繃鍣归柡鍡欏枛閺岋綁顢橀悢绋跨３闂佸搫鐭夌紞渚€鐛€ｎ喗鏅查柛鈾€鏅滈ˉ澶愭煛?
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

// 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愬弶鐤勯梻浣虹帛閸旓箓宕滈敃鈧埢鎾绘嚍閵夛箑寮垮┑鐘绘涧濡粓锝為敃鈧湁婵犲﹤鍟扮粔娲煙椤旂瓔娈滄俊顐㈠暙閳藉鈻庤箛鏃€鐣奸梺?gha fetch闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇閸撗呮殸闁诲孩鑹鹃ˇ浼村Φ閸曨垰绠崇€广儱妫Ο鍌滅磽娴ｇ瓔鍤欐俊顐ｇ箞瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鈧弻锝嗘償閵忥絽顥濆銈忓閺佽顕ｇ拠宸悑闁割偒鍋呴鍥⒒娴ｅ憡鍟為柟鎼佺畺瀹曠増鎯旈妸锕€浠奸梺缁樺灱濡嫬鏁梻浣瑰濡礁螞閸曨垰绀夐柛娑卞幐閺€浠嬫煟濡偐甯涙繛鎳峰洦鐓熸俊銈勮兌閻鎮楅棃娑栧仮鐎规洖鐖奸、妤佹媴閸濆嫬绠洪梻鍌欒兌缁垶宕濋弴銏╂晪妞ゆ挶鍨归悡鈥愁熆閼搁潧濮堥柣鎾跺枑閵囧嫰骞樼€靛摜鐣鹃梺绋跨箰閺堫剛鎹㈠☉銏犲耿闁归偊鍓涙导鍫濐渻閵堝啫鐏柨鏇樺灪閹便劑鍩€椤掑嫭鐓熸俊顖涙た閸熷繑淇婇幓鎺戣埞闁宠鍨堕獮濠囨煕婵炑冩噽娑撳秹鏌熸潏楣冩闁稿﹨鍩栫换婵嬫濞戝崬鍓伴梺鎼炲妼閸婂灝顫忓ú顏嶆晢闁逞屽墰缁棃鎮介崨濠備簵闂佸搫娲㈤崹娲偂濞嗘挻鐓熼柟瀵稿€栭幋锔藉亗闁靛璐熸禍婊堟煥濠靛棙鍣烽柛瀣ㄥ灲閺屸€崇暆鐎ｎ剛袦婵犵鍓濋幃鍌炲春閿熺姴纾兼繛鎴欏灪閺嗘﹢姊婚崒娆戭槮缁剧虎鍘鹃崚鎺楊敍濮樺吋缍庨悷婊呭鐢偛鐣垫担绯曟斀闁绘ê寮舵径鍕煟?
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
    // WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴椤㈡洟鏁愰崱娆欑穿闂備線鈧偛鑻晶鍓х磼閻樿櫕灏柣锝夋敱缁虹晫绮欑拠淇卞妽閵囧嫰寮崶顬挻绻涢崨顓犲ⅵ婵﹦绮幏鍛村川婵犲倹娈橀梻浣告啞濡垹绮婚幘宕囨殾濞村吋娼欓崘鈧銈嗗姂閸╁嫬螞濠婂牊鈷戠紒澶婃鐎氬嘲鈻撻弴銏＄厓闂佸灝顑呴悘锕傛煏閸パ冾伃妤犵偞甯″畷鍗烆渻閹屾缂傚倸鍊搁崐椋庣矆娓氣偓钘濇い鏍剱閺佸銇勯幘璺盒㈤柛銊︾箓閳规垿鎮╃€圭姴顥濋梺鍛婂姀閸嬫挻淇婇悙顏勨偓鏍礉濡ゅ懎绐楅幖娣灮椤╂煡鏌ｉ幇闈涘⒒婵炲牅绮欓弻锝夊箛椤栨氨姣㈡繛瀵稿О閸ㄤ粙寮诲☉銏犵睄闁逞屽墮閻ｆ繈骞栨担姝屾憰闂佺粯姊婚崢褔鎯屽Δ鈧…璺ㄦ喆閸曨厾顓兼繛瀛樼矤娴滎亜鐣峰ú顏勎ㄩ柨鏇楀亾缂佸墎鍋ら弻鐔兼焽閿曗偓婢х粯绻涢悡搴含婵﹨娅ｇ槐鎺戭潨閸絺鍋撻幐搴ｇ濞达絽鍟跨€氼參宕ｈ箛娑欑叆闁哄啠鍋撻柛銊︾箘閹广垽宕卞☉娆戝幗闂佽婢橀崥鈧紒銊ヮ煼閺屽秷顧侀柛鎾村哺閹冾煥閸繃鐎銈嗘磵閸嬫挻顨ラ悙鏉戞诞鐎殿噮鍓熷畷顐﹀礋椤忓嫷妫滃┑鐘愁問閸犳鈥﹂崶顒€鍌ㄧ憸搴ㄥ疾閸洖绠绘い鏃傛櫕閸樼敻姊绘担鍝ヤ虎妞ゆ垵鎳橀幃姗€骞橀鐣屽幍濡炪倖鏌ㄩ幖顐︽倶閼碱兘鍋撶憴鍕妞ゃ劌锕獮濠囧冀椤撶偟鍘告繛杈剧悼椤牓銆呴鈧埞鎴︽偐閸偅姣勬繝娈垮枙閸楀啿鐣风憴鍕檮缂佸瀵уΣ顒勬⒑閸濆嫮鈻夐柛妯圭矙瀵煡顢旈崼鐔叉嫽婵炶揪绲块悺鏃堝吹濞嗗精鏃堟偐閼碱剛顔婇悗鍨緲閿曪箓骞戦崟顖毼╅柨鏃傜帛缂?0 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ岸姊绘担钘壭撻拑杈╃磼鐠囪尙澧︾€殿喖顭烽幃銏焊娴ｅ湱浜栭梻浣稿閻撳牓宕板Δ鍛€剁€广儱顦伴埛鎴︽煕濠靛棗顏い銉у仧缁辨帡顢欓懖鈺冨姱闂佺硶鏅濋崑銈夌嵁鐎ｎ喗鏅滈柦妯侯槷濮规姊绘担鍛婅础缂侇噮鍨抽弫顕€鍩￠崒姘煎殼闂侀€炲苯澧存慨濠勭帛閹峰懘宕ㄦ繝鍌涙畼婵＄偑鍊戦崝灞轿涘Δ鍛仼婵犻潧顑呯粻鐟懊归敐鍡欐憙闁硅姤娲熷娲棘閵夛附鐝旈梺鍝ュ櫏閸嬪棗鈻庨姀銈嗗€烽柣鎴灻埀顒傛暬閺屻劌鈹戦崱娆忓毈缂備降鍔忓Λ鍕箒濠电姴锕ら崯浼村春閿濆棙鍙忓┑鐘插暞閵囨繄鈧娲﹂崑濠傜暦閻斿吋顥堟繛鎴灻ˉ宥夋⒒閸屾艾鈧绮堟担瑙勫劅婵犲﹤鎳愮粈濠偯归敐鍛棌闁搞倖娲熼幃褰掑炊瑜庨埢鏇㈡煕濮橆剦鍎忔い顓℃硶閹瑰嫭绗熼姘闂備浇鐨崘顭戜痪缂備胶绮换鍫熸叏閳ь剟鏌ㄥ┑鍡橆棤闁靛棙鍔曢—鍐Χ閸℃浠撮梺鐟板暱闁帮綁宕洪姀鐙€鍚嬮柛顐亝闉嬬紓鍌氬€峰ù鍥ㄣ仈閹间礁绠板┑鐘宠壘缁狀垶鏌涘☉娆愮稇缂佺姷鏁婚弻鐔煎传閸曨厜銏°亜閵夛妇顣茬紒缁樼箞閹粙妫冨ù鍓佸仱閺屾盯濡歌鐢爼鏌嶇紒妯荤叆闁宠棄顦甸崺妤呭煘閹傚闂佹寧娲栭崑鍡涘极閸℃稒鐓曢柡鍥ュ妼婢ь喖霉濠婂嫮鐭掓慨濠呮閸栨牠寮撮悙娴嬫嫟缂傚倷绀侀鍡欐暜閿熺姴绠栭柍銉︽灱閺嬪酣鏌熼幍铏珒缂併劌顭峰娲传閸曨厜鐘绘煕閺傚潡鍙勯柛鈹垮劜瀵板嫮鈧絻鍔嬬花濠氭⒑闂堟侗妲堕柛搴㈡そ瀹曟洟骞嬮悩鐢碉紲闁哄鐗勯崝灞矫归灏栨斀闁斥晛鍟崐鎰攽閿涘嫬鍘撮柡浣稿暣閸┾偓妞ゆ帊鑳堕々鐑芥煙缂併垹鏋熼柍閿嬪灴閹綊宕堕敐鍌氫壕闁惧浚鍋嗘禍顏堟⒒娴ｈ鍋犻柛銊︽そ閹繝鏁撻悩鍙夋К闂侀潧绻嗛崜婵嬪磿閻斿吋鐓冮梺娆惧灠娴滈箖姊洪崫鍕靛剰闂佸府缍佸濠氭偄閸忕厧鈧攱銇勯幒宥囶槮闁告挸缍婂娲焻閻愯尪瀚板褜鍠氱槐鎺撳緞婵犲偆鏆㈠┑鈥冲级閸旀瑩鐛Ο灏栧亾濞戞顏堫敁閹剧粯鈷戦柛娑橈攻鐏忔壆鎲搁弶鍨殻鐎规洘绮撳璺衡枎閻愵剙鐦滈梻渚€娼ч悧鍡欐崲閹烘鍋╅梺顒€绉甸悡鐔肩叓閸ャ劍鈷掓い锝堝皺缁辨帒鐣濋崘顔兼懙缂備胶绮换鍫濈暦閹偊妯勯梺鍝ュ仜閻栫厧顫忓ú顏勪紶闁告洦鍓欑粣娑㈡⒑缁嬫鍎愰柣鈺婂灦楠炲﹪骞樼拠鏌ユ暅濠德板€撶拋鏌ュ箯濞差亝鐓熼幖绮瑰墲鐠愨€斥攽椤旂偓鏆┑鈩冩尦瀹曟﹢鍩￠埀顒傛崲閸℃稒鐓熼柟閭﹀幗缂嶆垹绱掑Δ浣侯暡缂佺粯鐩幊婵嬫焽閿曗偓濞堫厼顪冮妶搴′簼缂侇喗鎹囧畷娲焵椤掍降浜滈柟鍝勬娴滄儳顪冮妶搴′簻缂佺粯甯炲Σ鎰板箳閹冲磭鍠撻幏鐘差啅椤旂懓浜鹃柟鍓х帛閳锋垿鏌涘☉姗堝姛缂傚倹宀搁弻娑欐償閵堝棛褰у銈冨妽閻熴儵鍩為幋锔藉亹闁割煈鍋呭В鍕⒑缁嬫鍎愰柨鏇樺灩閻ｇ兘鎮╃紒妯轰汗闁荤姴娲﹁ぐ鍐玻濞戞﹩娓婚柕鍫濇婢ь剛绱掗濂稿弰鐎规洏鍨介幃鈺冪磼濡厧骞楅梻浣虹帛閺屻劑骞楀鍫熷剹閻忕偟鐡旈悢鍡欐喐鎼淬劊鈧啴宕奸妷銉︾€銈嗘磵閸嬫挻顨ラ悙鏉戠瑨閾绘牕霉閿濆洨銆婇柛瀣崌瀹曠螖娴ｅ搫骞堟俊鐐€ら崢浠嬪垂閸偆顩查柡宥庣亹閻熼偊鐓ラ柛鏇ㄥ幘閻撳姊洪崫鍕伇闁哥姵鐗曢悾鐑筋敃閿曗偓缁€瀣亜閹炬剚妲堕柛濠冪箞瀵鈽夐姀鐘愁棟闁荤姵浜介崜杈樄闁哄备鈧磭鏆嗛悗锝庡墰琚﹀┑鐘殿暯閸撴繈骞冮崒鐐叉瀬闁告劦鍠栭悞鍨亜閹烘垵顏柛濠傜埣閺岀喓绱掗姀鐘崇亶闂佺粯鎸荤粙鎾诲焵椤掆偓閸樻粓宕戦幘缁樺€甸柨婵嗛婢ф壆鎮敂鎴掔箚闁靛牆娲ゅ暩闂佺顑囬崑銈夊箖閻戣棄鐓涘ù锝囧劋濞堥箖姊虹紒妯烩拻闁冲嘲鐗撳顐﹀炊椤掍胶鍘藉┑鈽嗗灥濞咃絾绂掑☉娆嶄簻妞ゆ劑鍨哄﹢鎵磼?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厾缁炬澘宕晶缁樹繆閺屻儰鎲鹃柡宀嬬秮閹垽宕妷褏鍘掑┑鐘媰閸曞灚鐣奸梺杞扮贰閸ｏ綁鐛幒鎳虫棃鍩€椤掑嫬鍨傞柛宀€鍋為悡鏇犫偓鍏夊亾闁逞屽墴瀹曟垿鎮欓悜妯轰簵闂佺粯妫冮弲鑼崲閸℃稒鐓忛柛顐ｇ箓閳ь剙鎲＄粋宥嗐偅閸愨斁鎷虹紓鍌欑劍閿曗晛鈻撻弮鍫熺叆闁哄洢鍔嬬花濠氭煟閿濆洤鍘撮柡浣瑰姍瀹曞ジ濮€閵忊剝绶┑鐘垫暩閸嬬偤宕归崼鏇熷仭鐟滄柨鐣烽姀銈呯缂備焦菤閹锋椽姊洪崨濠勨槈闁挎洩濡囩划濠氬箻缂佹ê鈧灚鎱ㄥΟ鐓庡付鐎殿噮鍠氶埀顒冾潐濞叉ɑ绻涙繝鍌滄殾闁绘垹鐡旈弫鍥煟濡⒈鏆滅紒鍗炲级娣囧﹪鎮欓鍕ㄥ亾閺嶎厼绀夐柟杈剧畱閸ㄥ倻绱撴担鑲℃垵鈻嶈箛娑欌拻闁稿本鐟︾粊鐗堛亜閺囩喓澧电€规洑鍗冲浠嬵敃閵堝浂妲稿┑鐘垫暩婵挳宕愭繝姘辈闁挎柨顫曟禍婊堟煙閺夊灝顣抽柣锝堜含缁辨帡鍩€椤掑倵鍋撻敐搴′簴濞存粍绮撻弻鐔煎传閸曨厜銈嗐亜閿旇偐鐣甸柡灞剧洴楠炴﹢鎳滈崹顐ｇ槗婵犳鍠栭敃銈夊箹椤愶絾娅忛梻浣规偠閸庢粓宕ㄩ鐐愩倝姊婚崒娆戭槮濠㈢懓锕幃锟犲醇閵夈儳顦梺缁樻椤ユ捇寮抽敃鍌涚厸闁搞儯鍎遍悘顏堟煕鐎ｃ劌濡跨紒杈ㄥ笧閳ь剨缍嗛崢濂稿礉閻㈠憡鐓欓柤鎭掑劜缁€瀣叏婵犲懏顏犵紒杈ㄥ笒铻ｉ柣鎴旀櫆鐎氫粙姊绘担渚劸缂併劍妞藉畷鎰板即閵忊€充患闂備礁鐏濋鍛閻愮儤鐓欓梺顓ㄧ畱楠炴﹢鏌涘Ο鎸庮棄闁宠鍨块、娆撳棘閵堝嫮杩旈梻浣告啞椤洭寮查銈嗩潟闁绘劕鎼粻锝嗙節閵忥紕绠撶紓宥咃工閻ｅ嘲螖閸涱喖浜楅柟鑲╄ˉ濡插懎顭囬弴鐘电＝闁稿本鐟︾粊鏉款渻鐎涙ɑ鍊愭鐐村灴瀹曟儼顦撮柡鍡畵閺岀喖鎮滃Ο铏瑰姼濠电偛鐨烽弲婊堝箞閵娾晜鏅查柛鈩冪懄閸ｏ綁姊洪幖鐐插婵＄偠濮ゆ穱濠囨偨缁嬭法顦板銈嗙墬濮樸劑顢橀崫鍕ㄦ斀闁绘灏欐禒娑㈡煕閺冣偓閻楁粓鎮疯濮婂宕掑▎鎰垫▊缂備線纭搁崰姘嚗婵犲啰顩烽悗锝庝簻缁ㄣ儵姊洪崫鍕潶闁稿寒鍣ｉ崺鈧い鎺戯功閻ｉ潧鈹戦埄鍐╁€愰柛鈹惧墲閹峰懏绗熼娴剁偤姊婚崒娆愮グ妞ゆ洘鐗犲畷瑙勬償閵娿儳顦板銈嗗姉閸犲孩绂嶉悙顑句簻闁哄稁鍋勬禒锕傛煃闁垮绗掗棁澶愭煥濠靛棛澧辨繛鍏煎姍閺屾稓鈧綆鍓欓埢鍫ユ煛鐏炲墽鈯曢柟顖涙婵偓閹鸿櫕绂掗姀銈嗏拺閻犲洠鈧櫕鐏嶉梺鎸庡哺閺屽秶鎲撮崟顐や患闂侀€炲苯澧剧紓宥呮瀹曟垿宕ㄩ娑欑€洪梺鎼炲労閸撴岸鍩涢幋鐘电＜閻庯綆鍋勯婊勭節閳ь剟骞嶉鍓э紲闁诲函缍嗛崑鍛暦瀹€鈧埀顒冾潐濞插繘宕归挊澹╂盯宕橀妸銏☆潔闁哄鐗勯崝宀勬儊濠婂牊鈷掑〒姘ｅ亾婵炰匠鍥ㄥ亱闁糕剝銇傚☉妯滄棃宕ㄩ灏栧亾閸洘鐓ラ柡鍥╁仜閳ь剙缍婂畷鎰版偨閻㈢數锛滃┑掳鍊撻懗鍫曞煡婢舵劖鐓欓柧蹇ｅ亜婵秹鏌熼姘辩劯鐎规洖銈告俊鐑芥晜閹冪濠电姴鐥夐弶搴撳亾閹版澘纾婚柟鍓х帛閻撶喐淇婇妶鍌氫壕闁诲孩鍑归崢鍓у垝鐎ｎ亶鍚嬮柛婊€鑳堕崣鍡涙⒑閸撴彃浜為柛鐘虫崌瀹曘垽骞栨担鍏夋嫼缂備礁顑嗙€笛冿耿閹殿喚纾奸悗锝庡亜閻忓瓨銇勯姀陇澹樻い顓滃姂瀹曟﹢宕滆閺佹悂姊洪懡銈呅㈤柟纰卞亰楠炴牠顢曢敂缁樻櫆濡炪倖鍔戦崐妤冨婵傜绾ч柛顐ｇ☉婵¤偐绱掑锕€娲﹂悡鍐喐濠婂牆绀堥柕濞у懐顦梺绯曞墲椤洭銆呴弻銉﹀仯閺夌偞澹嗙粔鍧楁煥濠靛棙鍋繛宸憾閺佸倿鏌涘☉鍗炵仩閻㈩垱顨婇弻锝夋偄閸濄儳鐓佺紓渚囧枟閹告悂鎮惧畡閭︾叆闁割偅鎯婇埡鍐╁枑闊洦娲滈惌鍡椻攽閻樺弶澶勯柣鎾存礋閺屽秹鍩℃担鍛婃闂佸疇妫勯ˇ閬嶆儉椤忓牆绠氱憸婊堟偂婵傚憡鐓涢悘鐐插⒔閳藉鏌嶉挊澶樻Ц閾伙綁鏌涜箛鎾虫倯闁哥姵鐗犲缁樻媴閻熸壋鏋欓梺琛″亾闁告鍎愬鏍р攽閻樺疇澹橀柣鎺戠仛閵囧嫰骞掗崱妞惧缂傚倷鑳舵慨瀵哥矓閻熸壆鏆﹂柡鍥ュ灪閻掕偐鈧鎸堕崕鍗灻洪敂閿亾娴ｅ啫浜归柍褜鍓氱粙鎺楁晪婵炲瓨绮嶉〃濠傤潖濞差亝顥堟繛鎴ｉ哺椤庡棙绻濈喊妯峰亾閾忣偄绐涚紓渚囧枤缁垶濡堕敐澶婄闁冲搫鍟獮宥夋⒑鐠囧弶鎹ｉ柡浣规倐閳ワ箓宕奸悢渚祫閻熸粍妫冨濠氭偄閻撳海顦ㄩ梺鍛婄懃椤﹁棄螞閻愬樊娓婚柕鍫濋瀵劍绻涙径瀣妤犵偛顦甸獮姗€顢欓懖鈺婃Ч闂備浇顕栭崹杈ㄧ珶鐎ｎ喖纾婚柟鎹愵嚙闁裤倖淇婇妶鍕厡闁告ɑ鎮傞幃妤呭礂婢跺﹣澹曢梻渚€鈧偛鑻晶瀵糕偓瑙勬磻閸楀啿顕ｆ禒瀣垫晣婵犙勫劤娴滄儳霉閿濆洨銆婃俊鎻掔墛娣囧﹪顢涢悙瀛樻殸闂佽楠搁…宄邦潖濞差亝鍋￠梺顓ㄧ畱濞堣埖绻濆▓鍨灈闁搞垺鐓￠幃楣冩倻閽樺鎽曢梺闈涱檧婵″洭宕?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻濞戔懞鍥偨缁嬫寧鐎梺鐟板⒔缁垶宕戦幇鐗堢厾缁炬澘宕晶缁樹繆閺屻儰鎲鹃柡宀嬬秮閹垽宕妷褏鍘掑┑鐘媰閸曞灚鐣奸梺杞扮贰閸ｏ綁鐛幒鎳虫棃鍩€椤掑嫬鍨傞柛宀€鍋為悡鏇犫偓鍏夊亾闁逞屽墴瀹曟垿鎮欓悜妯轰簵闂佺粯妫冮弲鑼崲閸℃稒鐓忛柛顐ｇ箓閳ь剙鎲＄粋宥嗐偅閸愨斁鎷虹紓鍌欑劍閿曗晛鈻撻弮鍫熺叆闁哄洢鍔嬬花濠氭煟閿濆洤鍘撮柡浣瑰姍瀹曞ジ濮€閵忊剝绶┑鐘垫暩閸嬬偤宕归崼鏇熷仭鐟滄柨鐣烽姀銈呯缂備焦菤閹锋椽姊洪崨濠勨槈闁挎洩濡囩划濠氬箻缂佹ê鈧灚鎱ㄥΟ鐓庡付鐎殿噮鍠氶埀顒冾潐濞叉ɑ绻涙繝鍌滄殾闁绘垹鐡旈弫鍥煟濡⒈鏆滅紒鍗炲级娣囧﹪鎮欓鍕ㄥ亾閺嶎厼绀夐柟杈剧畱閸ㄥ倻绱撴担鑲℃垵鈻嶈箛娑欌拻闁稿本鐟︾粊鐗堛亜閺囩喓澧电€规洑鍗冲浠嬵敃閵堝浂妲稿┑鐘垫暩婵挳宕愭繝姘辈闁挎柨顫曟禍婊堟煙閺夊灝顣抽柣锝堜含缁辨帡鍩€椤掑倵鍋撻敐搴′簴濞存粍绮撻弻鐔煎传閸曨厜銈嗐亜閿旇偐鐣甸柡灞剧洴楠炴﹢鎳滈崹顐ｇ槗婵犳鍠栭敃锔惧垝椤栫偛绠柛娑卞灡閸犲棝鏌涢弴銊ヤ簵闁告繃顨婂缁樻媴娓氼垳鍔搁梺鎸庢磸閸庣敻骞冭椤劑宕煎┑鍥┾偓顒€顪冮妶鍡樼５闁稿鎹囬弻娑㈠Ω閳哄啰鏆Δ鐘靛仜濞诧箓骞忛崨鏉戝窛濠电姴楠搁幃顏堟⒒閸屾艾鈧绮堟担鍦彾濠电姴娲ょ壕濠氭煕濞戝崬濮告繛宸簻閸愨偓濡炪倖鎸鹃崰搴㈢濡も偓閳规垿鎮欓弶鎴犱户闂佹悶鍔嶅浠嬪箖閿熺姴鍗抽柕蹇ョ磿閸樼敻姊洪柅娑氱シ婵炲懏娲栭埢宥夊川椤撴稒鏂€闂佹寧绋戠€氼剟宕濈€ｎ剛纾肩紓浣诡焽閳藉鏌ｉ敐鍡欑畺缂佺粯绻冪粭鐔煎焵椤掑嫬纾绘俊銈呮噺閳锋垿鏌涘┑鍡楊伀闁绘帟娉曠槐鎾愁吋閸曨厼濡介梺閫涚┒閸旀垿宕洪埀顒併亜閹烘垵顏柣鎾冲暣瀵爼鎮欓弶鎴偓婊堟煕韫囨捁瀚伴棁澶愭煟濞嗗苯浜鹃梺鍛婄懃閸燁偊锝炶箛娑欐優閻熸瑥瀚弸鍌炴⒑閸涘﹥澶勯柛妯圭矙楠炲鈻庤箛濠冩杸闂佺粯鍔曞鍫曞闯閻戞绠鹃柛娆忣樈閻掔晫绱掗鐣岊暡婵炵厧绻樻俊姝岊槾闁伙箑鐗撳娲川婵犲啰锛橀梺鍛婃煥閺堫剛绮嬪澶嬪€锋い鎺戝€婚鏇㈡⒑閻熼偊鍤熼柛瀣枛楠炲﹪骞囬悧鍫㈠幗闂佺懓鎼粔鍫曟儗濞嗘挻鐓涚€光偓鐎ｎ剛袦闂佽鍠撻崹娲囬悧鍫熷劅闁挎稖鍩囬崝鎴濐潖濞差亜浼犻柛鏇ㄥ枛椤忣厾绱撴担鍓叉Ш闁稿繑顭囬惁顒佺箾閹炬潙鐒归柛瀣崌閺岋紕浠﹂崜褎鍒涘Δ鐘靛仦椤洭鍩€椤掑嫭娑ч柟璇х節瀹曡櫕瀵肩€涙ǚ鎷虹紓渚囧灡濞叉﹢寮跺ú顏呯厱闁靛鍊楅惌娆撴煟濞戝崬鏋涢摶锝夋煕韫囨洖甯堕柣婵呭嵆閹鎮烽弶娆炬闂佸摜濮甸〃鍫㈡閻愬搫纾奸柣鎰ˉ閹风粯绻涢幘鏉戠劰闁稿鎸荤换娑欐媴閸愬弶澶勯柛瀣儔閺屾盯鍩勯崘顏佹缂備胶濮锋繛鈧柡宀€鍠栭獮鎴﹀箛闂堟稒鐣伴梻浣告惈椤︽澘顪冮挊澶樻綎濠电姵鑹剧壕鍏肩箾閸℃ê绗掗柛姗堢節濮婅櫣娑甸崨顔惧涧闂佸湱鎳撳ú銈夋偩閻戣棄唯鐟滃宕戦幘缁樻櫜閹肩补鍓濋悘宥夋⒑缁嬫鍎岄柡鍛矒婵＄敻宕熼鍓ф澑闂佽宕樺▔娑€傚ú顏呪拺闁硅偐鍋涙俊濂告煕婵犲倹璐￠柟骞垮灩閳藉顫滈崱妤佹珚婵＄偑鍊栧Λ浣哥暦閻㈢绀勭憸鐗堝笚閳锋垹绱掔€ｎ亜鐨＄€规悶鍎茬换娑氭兜妞嬪海鐦堥悗娈垮枛椤兘寮幇顓炵窞閻庯綆鍋呴悵鎶芥⒒娴ｅ憡鍟炲〒姘殜瀹曪絾鎯旈埈銉秮閹粙宕ㄦ繛鐐缂傚倷绀侀鍡涱敄濞嗘挸纾块柟鎵閻撴瑩鏌涘☉姗堟敾闁稿﹥鍔欓弻娑㈠煘閸喚浼堝Δ鐘靛仦椤洭骞忛悩璇茬闁圭儤鏌￠崑鎾活敇閻戝棙瀵岄梺闈涚墕濡瑩鍩涢幒妤佺厸闁告粈绀佹禍婵堢磼閸屾氨效妤犵偛娲幃褔宕煎顓烆伜?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟稒宸濈紒鈾€鍋撻梻浣侯焾閺堫剛鍒掑畝鍔肩兘鍩€椤掑嫭鈷掑ù锝勮閻掔偓銇勯幋鐐茬仼婵″弶鍔欏鎾閻樼數鏋€闂備礁缍婂Λ璺ㄧ矆娴ｈ櫣灏电€广儱顦伴悡鏇熴亜閹扳晛鈧洟寮搁幋鐐电闁告侗鍠氶惌鎺撴叏婵犲懏顏犵紒杈ㄥ笒铻ｉ柛婵嗗濞兼捇姊绘担鍛婅础闁冲嘲鐗撳畷鎴﹀幢濞嗗繐鐏婃繝鐢靛У閼瑰墽绮婚鈧弻銈夊箒閹烘垵濮㈤梺鍛婎焽閺佽顫忛搹鍦＜婵☆垰鎼闂備礁鎲￠幐璇差潩閵娧勵潟闁绘劕鎼粻锝夋煟閹板吀绨风紒銊嚙椤啴濡堕崱妤€顫庨梺鍝ュ枎濞尖€崇暦閹达箑绠婚悹鍥皺閻も偓濠电偠鎻徊鍧楀箠閹剧粯鍎婃繝濠傜墛閳锋帒銆掑锝呬壕濠电偘鍖犻崶銊ヤ罕闂佺硶鍓濋妵鍌氣槈濡粍妫冨畷姗€顢旈崱娆愭闂傚倷鑳堕…鍫ュ嫉椤掑倸鍨濋幖杈剧到椤ユ艾鈹戦崒姘暈闁绘挸鍟伴幉绋款煥閸繄顦┑顔筋焾濞夋稓绮婚鐐寸厱婵炴垵宕悘锟犳煕閻樺弶顥㈤柡灞剧洴瀵挳濡搁妷銈囩泿濠电姷顣介埀顒€鍟块弳閬嶆煃鐟欏嫬鐏撮柟顔规櫊瀹曞綊顢曢敐鍡欐婵犵數濮甸鏍窗閺嶎厽鍋ら柕濞р偓閸嬫捇妫冨☉鏍т划閻庤娲忛崝鎴﹀箠濠婂牜鏁勯柟绋挎捣濞兼牠姊婚崒娆戭槮婵犫偓闁秴纾块柕鍫濐槸閸ㄥ倻鎲搁悧鍫濆惞闁搞儺浜跺Ο鍕攽椤旂》宸ユい顓炲槻閻ｇ兘骞掗幋顓熷兊濡炪倖鍨煎Λ鍕妤ｅ啯鐓犲┑顔藉姇閳ь剚鐗曢悾鐑藉矗婢跺瞼鐦堥梻鍌氱墛缁嬫挻鏅堕幇鐗堢厱閻庯綆鍋呯亸顓㈡煃閽樺妯€妤犵偞甯￠獮妯尖偓鐢殿焾缁茶棄鈹戦敍鍕杭闁稿鍊栫粋宥咁煥閸繄鏌堥柣搴㈢⊕鐪夌紒璇叉閺屻倗绮欑捄銊ょ驳闂佺娴烽崰鏍蓟瀹ュ唯闁挎洍鍋撶紒鑼帛閵囧嫰濡烽敂鐣屾缂備浇椴搁幑鍥х暦閹烘垟鏋庨柟鐑樺灥鐢垰鈹戦悩鎰佸晱闁哥姵宀稿顐ゆ嫚閼碱剚娈鹃悷婊呭鐢帡鏌ㄩ妶鍡曠箚闁靛牆鍊告禍楣冩⒑鐠囪尙绠插┑鐐诧躬瀵濡搁埡浣虹潉闂佸壊鍋嗛崰鎰枔娴煎瓨鈷戠紒瀣儥閸庢劙鏌熼悷鐗堟悙闁伙絿鍏橀幃婊呯驳鐎ｎ偅娅嶉梻浣虹帛閸旀牠宕濋幋锕€纾块柣銏㈩焾閽冪喖鏌ㄥ┑鍡╂Ц閸ユ挳姊虹粙鎸庢拱闁活収鍠楃粩鐔煎即閵忊檧鎷婚梺绋挎湰閼归箖鍩€椤掑嫷妫戠紒顔肩墛缁楃喖鍩€椤掑嫮宓侀柡宥庡弾閺佸啴鏌ㄩ弴妤€浜剧紒鐐劤缂嶅﹪寮婚敃鈧灒濞撴凹鍨卞瓭闂備礁鎼鍡涙偋閻樿钃熼柨鐔哄Т閻愬﹪鏌曟径鍫濆姢闁告梹鎸冲?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏ゅ川婵犲嫮肖濠德板€х徊浠嬪疮椤栫儐鏁佺€广儱顦伴埛鎴犵磼鐎ｎ偒鍎ラ柛搴＄箲閵囧嫰骞嬪┑鎰枅闂佽鍣换婵嬨€侀弴銏℃櫇闁逞屽墴閹瑦绻濋崶銊у帾婵犵數鍋涢悘婵嬪焵椤掍緡娈橀柟骞垮灪缁楃喖鍩€椤掆偓椤繘鎮滃Ο璇差潯闂佽顔栭崳顔嘉ｉ鈧弻锝嗘償閵忋垻鍙濋梺鍛婎殔閸熷潡顢氶敐鍥ㄥ珰婵炴潙顑嗛～宥夋⒑闂堟稓绠冲┑顔肩墦婵℃悂鍩￠崒婊冨汲婵犵數濞€濞佳兾涘▎鎾抽棷閻熸瑥瀚ㄦ禍婊勩亜閹哄棗浜惧銈嗗灥椤︻垶锝炶箛娑樹紶闁告洦鍘藉▓楣冩⒑閹肩偛鍔楅柡鍛〒缁參鍨鹃弬銉︽杸闂佸疇妫勫Λ妤佺濠婂牊鐓曢柣鏂挎啞缂嶆垶銇勯弴顏嗘偧闁归濮撮蹇涱敊閸忕厧鈧垶姊绘担绋款棌闁稿甯″畷鎴﹀川閹碱厽鏅涢梺鎼炲労閸撴岸鍩涢幒鎳ㄥ綊鏁愰崨顔兼殘闂佸摜鍠撻崑銈夊蓟閻旇櫣鐭欐繛鍡楀⒔閸氬姊虹拠鈥虫灓闁稿繑锕㈠畷娲晸閻樻彃绐涘銈嗘尵閸犳劘鈪查梻鍌氬€风欢姘焽瑜忛幑銏ゅ幢濞戞顦┑鐘绘涧椤戝懐澹曢崸妤佺厽闁哄倸鐏濋幃鎴︽煟閹惧磭绠伴柍瑙勫灴閹瑩鎳犻鑳闂備礁鎲￠弻锝夊磹濠靛钃熸繛鎴欏灩缁秹鏌嶈閸撶喖鐛幇鏉跨鐎瑰壊鍠楃紞搴♀攽閻愬弶鈻曞ù婊勭矊椤斿繐鈹戦崱蹇旀杸闂佺粯蓱瑜板啴鍩€椤掍焦鍊愰柨婵堝仦閹棃濡搁敂瑙勫缂傚倷绀侀鍡涱敄濞嗘挸纾块柟鎵閻撴瑩鏌ｉ悢鍝勵暭闁哥姵顭囬埀顒冾潐濞叉牠骞栭锕€鐓″鑸靛姇椤懘鏌ｅΟ璇茬祷缂佷緡鍣ｅ缁樼瑹閳ь剙顭囪閹广垽宕卞Δ濠勨偓鍓佹喐閺冨牏宓侀煫鍥ㄦ媼濞差亶鏁傞柛娑变簼鐎氬ジ姊绘担鍛婂暈缂佸鍨块弫鍐晜閸撗傜瑝闂佺懓澧界划顖炲磹閸偅鍙忔俊顖滃帶鐢爼鏌ｈ箛銉х暤闁哄被鍔岄埞鎴﹀醇濠靛懏顫曢梻浣芥〃缁€渚€宕弶鎴犳殾闁圭儤鍨熷Σ鍫熸叏濮楀棗澧紒鐘靛仱濮婄粯鎷呮笟顖涙暞闂佺顑嗛崝妤呭箖娴兼潙唯闁宠桨鑳堕崝锕€顪冮妶鍡楃瑨闁哥噥鍨堕幃鐢割敂閸涱喖寮块梺鎸庣箓濡鎱ㄥ鍡樺弿濠电姴鍟妵婵囶殽閻愭潙濮堥柟顖涙婵偓妞ゎ偒鍠氶崰姗€姊婚崒娆戝妽闁诡喖鐖煎畷婵婄疀閹炬劖鐩畷鐔碱敍濞戞ü鎮ｉ梻浣虹帛閸ㄧ厧螞閸曨剙鍔旈梻鍌欑閻ゅ洤顩奸妸鈺傚殑闁煎憡顔栧▓浠嬫煕閵夘喖澧柍閿嬪灴閺岋綁鎮㈤悡搴濆枈闂佹悶鍊楅崰搴ㄥ煘閹达富鏁嶉柨婵嗘噹缁犲綊鏌涘Δ鍛喚闁哄瞼鍠撶槐鎺楀閻樺磭浜堕梻浣瑰▕閺€杈╂暜閹烘绠掗梻浣瑰缁诲倸煤閵娾晛绠洪柛灞剧〒缁犻箖鏌涜箛鏇炲付濠殿喖鐗撻弻鏇㈠炊瑜嶉顓犫偓娈垮櫘閸ｏ綁宕洪埀顒併亜閹烘垵顏柛瀣儔閺岋絽螣閸喚姣㈤梺鍝勬４缁犳捇寮婚埄鍐ㄧ窞閹兼惌鍠栧▓楣冩⒑閸濆嫬鏆欓柣妤€妫楅蹇撯攽閸ャ儰绨婚梺鍝勫暙閻楀﹪鏁嶉悢鍏肩厵妞ゆ洖妫涚粔顔芥叏婵犲偆鐓肩€规洘甯掗埢搴ㄥ箣閿濆洨宕堕梻鍌欑閹诧繝骞愭繝姘仭鐟滃繘宕氶幒妤€绫嶉柛顐ゅ枎娴犳椽姊洪崨濠勨槈闁挎洏鍊栫粋宥呂旈埀顒勫煘閹达附鍊烽柡澶嬪灩娴犙囨⒑閹肩偛濡芥俊鐐扮矙楠炲啴鏁撻悩鎻掑祮闂侀潧楠忕槐鏇㈠储閹间焦鈷戦柟鑲╁仜閸旀﹢鏌涙惔銊ゆ喚闁诡喗锕㈤、鏃堝川椤栨粠鍟庨梻浣烘嚀閸熷潡宕查幓鎹楁椽鏁冮崒娑樹簵闂佺粯妫侀崑鎰板矗閹剧粯鐓曢柕澶涚到閸旀岸鏌涢弬璇测偓妤冩閹烘鐓㈤柍褜鍓熷畷鎴﹀箻缂佹ǚ鎷绘繛杈剧到閹诧繝骞嗛崼銉︾厽妞ゆ挾鍎愬Σ鐑樹繆閸欏濮嶇€殿喗鎸抽幃銏＄瑹椤栨稓銈梻鍌欑劍鐎笛兠洪弽顓炵９鐟滅増甯掗崥鍦偓骞垮劚椤︿即鎮¤箛鎿冪唵闁煎摜鏁稿ú瀛樹繆缂併垹娅嶉柡灞剧⊕缁绘繈宕堕妸锔芥濠电儑绲藉ú銈夋晝椤忓牆绠栨繛鍡樻惄閺佸倿鏌涢弴妯哄濞存粓绠栭弻鈥愁吋鎼达絼姹楀┑鐐叉▕娴滄粓鎮″☉銏＄厱闁规澘鍚€缁ㄨ姤銇勯敂璇茬仭缂佺粯鐩獮姗€寮堕幋鐘插Р闂備胶顭堥鍡涘箰妤ｅ啫鐒垫い鎺戝枤濞兼劖绻涢崣澹濐亪鈥旈崘鈺冾浄閻庯綆浜濆Σ顒勬⒑闁偛鑻晶顖炴煏閸パ冾伃妤犵偞甯掗鍏煎緞鐎ｇ鍋撻弽銊х閻庢稒顭囬惌瀣磼椤旇姤宕岀€殿喖顭烽幃銏ゅ礂閻撳簶鍋撶紒妯圭箚妞ゆ牗绻冮鐘裁归悩铏稇妞ゎ亜鍟存俊鍫曞川椤旂虎娲跺┑鐐茬摠缁姵绂嶉鍕靛殨濠电姵纰嶉弲鎼佹煟濡搫绾ч柕鍥ㄧ箖缁绘稒娼忛崜褎鍋у銈庡幖閻楁捇鏁愰悙渚晝闁挎棁袙閹锋椽姊绘笟鍥т簽闁稿鐩幊鐔碱敍濞戞瑦鐝峰銈嗘磵閸嬫捇鏌＄仦鍓р槈閾伙綁鏌ゆ慨鎰偓妤呮偂婢舵劖鍊甸悷娆忓缁€鍐煕閵娿儲鍋ラ柣娑卞枛铻ｉ柤娴嬫櫇閿涙粌鈹戦埥鍡楃仯闁告鍥╁祦闁割偁鍎查埛鎺懨归敐鍥╂憘婵炲吋鍔栫换婵嬪閳晜鍨垮鏌ユ嚋閸偄鍔呴梺闈涱焾閸庢娊藝椤愶附鈷戦柛婵嗗濡插綊鏌ㄥ☉娆愮闁诡喚鍏橀弻鍥晜閹冩辈闂傚倷绀佹竟濠囧磻娓氣偓瀹曡绻濆顒佽緢濡炪倖鍔ч梽鍕煕閹达附鍋ｉ柛銉岛閸嬫捇鎼归銈勬喚闂傚倷鐒︾€笛兠鸿箛娑樺瀭濞寸姴顑囧畵渚€鏌涢幇鈺佸Ψ闁哄閰ｉ弻鐔衡偓鐢殿焾娴犳粎鐥紒銏犵仸闁哄备鍓濆鍕沪閹存帗鍩涘┑鐘媰閸曞灚鐤侀梺璇″灠閻楁劗绮诲☉妯锋婵炲棗鏈€氳棄鈹戦悙鑸靛涧缂傚秮鍋撳┑鈽嗗亜鐎氭澘鐣烽妷銊ｄ汗闁圭儤鎸鹃崢閬嶆⒑缂佹◤顏堝疮閹稿孩鍙忕€广儱娲犻崑鎾舵喆閸曨剛顦ㄧ紓浣筋嚙閻楀棝顢氶敐澶婄妞ゆ梻鈷堝濠囨⒑缂佹﹫鑰挎繛浣冲喚鍤曢柕濞垮劗閺€浠嬫煟濮楀棗鏋涢柣蹇涗憾閺屾盯鍩￠崒婊冣拰閻庤娲樼换鍫濐嚕閹绢喗鍋愰柛蹇撴憸閻熸繄绱撻崒姘偓鐑芥倿閿曚礁缍樺┑鐐茬摠缁秶鍒掗幘璇茶摕闁炽儲鍓氶崥瀣煕閹扳晛濡兼い顒€顑夊铏圭磼濡粯鍎庨梺鎼炲妺閸楁娊鍨鹃敂鐐磯闁靛绠戦弸鍌炴⒑閸涘﹥澶勯柛鎾寸洴钘濋柕濞у懐锛濇繛杈剧秬濞咃絿鏁☉姘辩＜閻犲洩灏欐晶锔筋殽閻愭彃鏆炵紒鍌涘笧閳ь剨缍嗛崑鎺懳涢崘銊㈡斀闁绘劖娼欓悘锔姐亜閵娿儳澧︽鐐村灴瀵挳鎮㈤搹鍦缂備礁澧芥晶妤勬懌濡炪値鍋勯敃顏堝蓟濞戞埃鍋撻敐搴濈盎妞ゅ浚鍘介妵鍕閳ュ啿鎽靛銈冨灪閿曘垽骞冨▎蹇ｅ悑闊洦娲滆ⅵ婵°倗濮烽崑娑樏洪鐑嗗殨闁割偅娲栭崹鍌涖亜閺冨洤浜归柕鍫櫍濮婂宕掑▎鎺戝帯缂備緡鍣崹浼达綖韫囨梻绡€婵﹫绲洪崑鎾绘晝閸屾岸鍞跺┑鐘茬仛閸旀洖鈻撻妸鈺傗拺缂佸妫楃€氬嘲鈻撻弴銏＄厓闂佸灝顑呴悘鈺呮煙閸欏鍊愰柟顔ㄥ洤閱囨繝闈涚墢閹虫牠姊绘担鐟邦嚋缂佸鎹囧畷浼村箻閼告娼熼梺鍦劋椤ㄥ懘鎮為崹顐犱簻闁瑰搫绉堕崝宥呪攽椤栨稒灏﹂柡灞熷棛鐤€婵炴垶甯掓禍鐐箾閹寸偞鐨戦柟鎻掔秺濮婂宕掑▎鎺戝帯闂佺娅曢幑鍥箖濞差亜惟鐟滃酣顢曟禒瀣厽闁靛牆楠搁悘娑樏瑰鍐Ш闁哄瞼鍠栭幃婊兾熺拠鏌ョ€烘繝鐢靛仜閹冲繒鍒掗幘鎰佹綎婵炲樊浜滅粻褰掓煟閹邦厼绲诲┑顔肩焸濮婃椽宕ㄦ繝鍐弳缂備礁顦紞濠囩嵁閸愵喗鍋ㄩ柛鎾冲级閺呮粓姊虹化鏇炲⒉闁荤啙鍥ㄦ櫖婵犻潧娲ㄧ粻楣冨级閸繂鈷旈柛鎺嶅嵆閺岀喓鍠婇崡鐐扮盎闂佸搫妫岄弲鐘差潖缂佹ɑ濯村〒姘煎灣閸斿摜绱撴担铏瑰笡闁搞劌娼″顐﹀礃椤旂⒈娼婇梺缁樼憿閸嬫捇鏌涢妶鍡樼闁宠鍨块幃鈺佺暦閸ヨ埖娈归梻浣告惈閹冲繘骞冮崒鐐茶摕闁挎繂鎲橀悢鐓庡瀭妞ゆ梻鍋撻妤佷繆閻愵亜鈧呯磽濮樿泛纭€闁告劕妯婂鏍р攽閻樺疇澹樼痪鎯у悑閹便劌螣閸濆嫯鍩炲銈冨€愰崑鎾剁磽閸屾艾鈧绮堟笟鈧、鏍幢濞戞顔囬梺鐓庮潟閸婃鎯屾径鎰厵闁绘垶锕╁▓銏ゆ煛娴ｅ憡鍠橀柡宀嬬秮楠炲洭濡搁敂鐣屽絾闂備礁鎼崐鐢稿磻閹剧粯鈷掗柛灞剧懆閸忓瞼绱掗鍛仸闁轰礁鍟撮崺锟犲川椤撶媴绱遍梻浣告啞濞诧箓宕滃☉顫偓鍛村蓟閵夛妇鍘靛銈嗙墪濡寮甸鍌楀亾闂堟稓鐒告慨濠冩そ濡啫鈽夊顒夋毇婵犵妲呴崑鍛存偡瑜庣粋宥夊箹娓氬洦鏅滈梺鍓插亞閸犳捇宕㈤柆宥嗏拺闂傚牊渚楅悡顓㈡⒒閸曨偄顏┑锟犳涧閳规垿宕煎顑垮闁荤喐鐟ョ€氼厾绮堥崘鈺冪闁告瑥顦辨晶鎶芥煟韫囨挸鏆ｆ慨濠冩そ瀹曘劍绻濇担铏圭畳闂備礁鎽滄慨闈涚暆缁嬭法鏆﹂柕蹇嬪€ら弫鍌炴煕濞戝崬鐏ｉ柣锕€鐗撳娲濞戙垻宕紓浣藉紦缁瑩骞冮敓鐘茬疀闁绘鐗忛崣鍡椻攽椤旂瓔鐒鹃柛鈺傜墵閹繝鎮㈤懖鈺佸絼闂佹悶鍎崕閬嶅礉閵堝鐓欐い鏂跨仢娴滃湱绱掓潏銊ョ瑲婵炵厧绻樻俊鎼佸Ψ閿曚胶妾ㄩ梻鍌欒兌绾爼寮笟鈧畷鎴﹀箻閹颁焦瀵岄梺闈涚墕濡绮崒婊呯＜闁逞屽墯閹峰懘鎮滃鍡椾缓闂備胶绮崝妤呭磿閵堝鍋傛繛鍡樻尰閳锋垶銇勯幘鍗炲闁伙綀椴搁〃銉╂倷鏉堟崘鈧潡鏌＄仦鍓р槈妞ゎ偅绮撻崺鈧い鎺嶈兌椤╂煡鎮规潪鎷岊劅婵炲吋鐗楃换娑橆啅椤旇崵鍑归梺绋款儜缁绘繈寮婚敐鍛傛棃鍩€椤掑嫭鍋嬪┑鐘叉搐绾惧潡鏌熼悙顒€澧繛鎾愁煼閺屾洟宕煎┑鍥舵闂佽绻堥崕鐢稿蓟閻斿吋鎯炴い鎰靛墲閳ь剝娉曢埀顒冾潐濞叉ê顪冩禒瀣槬闁逞屽墯閵囧嫰骞掑澶嬵€栨繛瀛樼矋缁捇寮婚悢鐓庝紶闁告洦鍘滈妶澶嬬厱闁哄倽娉曡倴缂備浇椴哥敮锟犲春閳ь剚銇勯幒鎴濃偓鐢稿磻閹剧粯鏅查幖绮瑰墲閻忓秹姊虹紒妯诲鞍闁荤噦绠撴俊鐢稿礋椤栨艾宓嗗┑掳鍊愰崑鎾趁瑰鍐ㄢ挃缂佽鲸鎸婚幏鍛村传閸曟埊缍侀弻锝呂旈崘銊㈡瀰闂佺粯渚楅崳锝呯暦濮椻偓婵℃悂濮€閿涘嫧鍋撴繝姘拺闁荤喐澹嗘禒銏ゆ煕閻曚礁鐏︾€殿喗鐓℃慨鈧柕鍫濇閹疯櫣绱撴担鍓插剱閻庣瑳鍐胯€块柣妤€鐗勬禍婊堟煏婵炲灝鍔滄い銉ｅ灲閺岀喖顢欓弬銈堚偓璺ㄢ偓瑙勬礃閿曘垽銆佸▎鎾村仼閻忕偠妫勭粻娲⒒娴ｄ警鏀板鐟扮墦楠炴捇顢旈崱姗嗘綗闂佸搫娲㈤崹娲磻閳哄倶浜滈柟鎯у船閻忣亪鏌ｉ幘瀛樼妤犵偞鐗曡彁妞ゆ巻鍋撳┑鈩冩そ閺岀喖鎳犻鈧禍鐐亜閵婏絽鍔﹂柟顔界懅閹风姾顦抽柡渚€绠栧铏规嫚閳ュ磭浠┑鈽嗗亜閸熸潙鐣?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵婵炲棗绻掗崝鎾⒑鏉炴壆顦︽い鎴濇婵＄敻宕熼姘鳖啋闁荤姾娅ｉ崕銈夋倵妤ｅ啯鈷戦柛娑橈功閹冲啰绱掔紒姗堣€跨€殿喖顭烽崺鍕礃閵娧呯嵁闂佽鍑界紞鍡樼閻愬搫绀夌紒瀣氨閺€浠嬫煟閹邦垰鐨烘慨锝囧仱閺岋繝宕ㄩ姘ｆ瀰閻庢鍠栭…閿嬩繆閹间礁鐓涘ù锝堟濞煎姊绘担渚劸闁哄牜鍓熼幆宀勵敊閻ｅ瞼褰鹃梺鍝勬储閸ㄦ椽宕愰悽鍛婂仭婵炲棗绻愰顏嗙磼閳ь剟鍩€椤掑嫭鍊垫繛鍫濈仢閺嬫稑顭胯闁帮綁鐛幋锕€顫呴柣姗嗗亝椤秹姊洪棃娑氱濠殿喚鍏橀、姗€宕崟銊︽杸闂佺粯鍔曞鍫曀夊鍛＜缂備焦锚缁楁氨绱掗崒娑樻诞闁硅櫕鐗犻崺锟犲礃椤忓海闂繝鐢靛仩閹活亞寰婃禒瀣妞ゆ劧绲挎晶鈩冪節閻㈤潧啸闁轰焦鎮傚畷鎴濃槈閵忊剝娅囧銈呯箰鐎氱兘宕甸弴鐐╂斀闁绘ê鐤囨竟姗€鏌涘Δ浣糕枙闁哄矉缍佸顕€宕奸锝庢缂傚倷鐒﹂崝妤呭磻閵堝拋娼栨繛宸簻缁€鍫ユ煙缂佹ê绗傜紒銊ㄥ亹缁辨挻鎷呯粵瀣闂佺锕ょ紞濠囩嵁閸儱惟闁挎棁妫勯崝鍛存⒑闂堟侗鐓柤闈涚秺楠炲繘鎼归崷顓狅紳闂佺鏈悷褔宕濆澶嬪珔闂侇剙绉甸崐鍫曠叓閸ラ鍒扮€殿噮鍣ｉ弻锛勪沪閸撗€濮囩紓浣虹帛缁诲牆鐣烽幒鎴旀婵☆垵顫夊Ο濠囨⒑鐠囨彃顒㈡い鏃€鐗犲畷浼村冀椤撴稈鍋撻敃鍌涘€婚柦妯侯槹閻庮剟姊洪悷鏉库挃缂侇噮鍨跺畷鎴﹀箛閻楀牏鍘剧紒鐐緲瀹曨剚鏅堕崜褏纾奸弶鍫涘妿閸欌偓濠殿喖锕︾划顖滅箔閻旂厧鐒垫い鎺嗗亾妞ゎ厼娲╅ˇ褰掓煙椤斻劌娲ら獮銏＄箾閹寸儐鐒介柣銈呮喘閺岀喖鎳濋悧鍫濇锭缂備礁寮剁€笛呯矙婢跺鍚嬪璺侯儑閸橀亶姊洪幐搴㈩梿妞ゆ泦鍥╁祦婵☆垵鍋愬Λ?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏焊娴ｅ湱鈧姊婚崟顐ｅ枠妞ゃ垺淇洪ˇ鏌ユ偂閵堝棎浜滈柟鍨暞婵炲洭鏌嶈閸忔稓绮堟笟鈧敐鐐差煥閸繄鍔﹀銈嗗笂閻掞箓宕ｈ箛娑欑厓鐟滄粓宕滈悢鐓庤摕闁挎繂鎷嬪銊╂煃瑜滈崜娆撯€﹂崶顏嶆Ъ闂?
    // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊閻樺樊妫岄梺杞扮閿曨亪寮婚垾鎰佸悑閹肩补鈧磭顔愰梻鍌氬€搁崑鍡涘垂闁秴桅闁告洦鍨伴崘鈧梺闈浤涢崨顖氬笌缂傚倸鍊峰ù鍥╃礄娴兼潙纾规繝闈涱儏閽冪喖鏌ㄥ┑鍡╂Ч闁哄懏鐓￠弻娑樷槈閸楃偞鐏嶉梺鍦厴娴滃爼骞冨Δ鍐╁枂闁告洦鍓涢ˇ銊╂⒑缂佹ɑ鎯堢紒缁樼箓椤曪絾绻濆顓炰簻闂佸憡绋戦敃锔剧矓閸洘鈷戦悹鍥ｂ偓宕団偓濠氭煕濞戝崬鏆曢柟鐑橆殕閳锋帒霉閿濆牆袚闁靛棗鍟扮槐鎺楀焵椤掍胶鐟归柍褜鍓欓悾鐑筋敍閻戝棙鏅濋梺鎸庢磵閸嬫捇鏌ｉ幘杈捐€块柡宀€鍠愬蹇涘礈瑜忛弳鐘电磼閻愵剙鍔ら柛姘儑閹广垹鈹戦崶鈺冪槇闂佺鏈粙鎺戠暦閹惰姤鈷戦柛娑橈攻閳锋劙鏌熼鐓庘偓鍨嚕婵犳碍鏅插璺侯儏娴滄粓姊洪崨濠勭細闁稿孩鐓″畷鐢稿磼閻愮补鎷婚梺绋挎湰閼归箖顢旈埡鍛€垫慨妯哄船椤ｇ厧菐閸パ嶈含闁诡喚鏅划娆戞崉椤垶效濠碉紕鍋戦崐鏍偋濡ゅ懏鍤屽Δ锝呭暙閼稿綊鏌曟繛鐐珕闁抽攱鍨垮濠氬醇閻斿墎绻侀梺缁樺浮缁犳牠寮诲☉銏犳閻犳亽鍔庨崝顖炴煢濡厧鏋戠紒缁樼箞濡啫鈽夐崡鐐插闂備胶顭堥鍥ㄦ叏閻㈢绠為柕濞垮剻閻旂厧浼犻柛鏇ㄥ枛閻撴﹢姊绘担鍛婃儓妞ゆ垵妫涚划娆撳箳濡も偓缁犵偤鏌曟繝蹇擃洭缂佲檧鍋撻梻浣告啞濞诧箓宕㈤懖鈺冪當闁挎稑瀚壕钘壝归敐鍫殐婵炲牊锕㈤弻娑㈡偐瀹曞洤鈪归梺浼欑悼閸忔﹢鐛崱娑欑劷闁挎柨褰ㄩ鍫熲拻濞达絿鐡旈崵娆戠磼缂佹ê濮嶉柟顔ㄥ洤绠婚柟棰佺劍缂嶅骸鈹戦悙鍙夆枙濞存粍绻堥幃鐐寸節濮橆厾鍘介梺褰掑亰閸ㄤ即鎷曟總鍛婂€垫慨姗嗗幗缁跺弶銇勯鈥冲姷妞わ箓浜堕弻娑樜旀担绯曟灆閻庢鍠涢褔鍩ユ径鎰潊闁炽儱鍘栫花濠氭煟鎼达絾鍤€閻庢矮鍗冲畷鎴炵節閸パ咁槷婵犮垼鍩栭崝鏍磹閻㈠憡鐓曟い顓熷灥娴滄繄绱掗懜鐢靛弨闁诡喗顨呴～婵嬫偂鎼淬垻浜剁紓鍌欑閸婂湱鏁敓鐘叉瀬闁告劦鍠栫壕鍏兼叏濡じ鍚柡鍡曞嵆閺岋綁鎮㈤崫銉х厐缂備胶绮敃銏狀嚕椤愩埄鍚嬮柛顐犲灮閺夌鈹戦悙鏉戠仸闁荤喆鍨介獮蹇曠磼濡偐顔曢柡澶婄墕婢т粙骞冩總鍛婄厱閹煎瓨绋戦埀顒佺箓閻ｇ兘鎮界粙璺ㄥ姦濡炪倖甯掔€氼參鍩涢幋锔藉€甸柣褎鎮傚顔界節閵忊埄鎴炵┍婵犲洤閿ゆ俊銈勮兌閸橀亶姊洪崷顓炲妺闁圭鎽滅划顓烆潩閼哥數鍘靛銈嗙墬缁嬫帡藟閸儲鐓冮柕澶樺灣閻ｅ灚顨ラ悙宸剶闁诡喗鐟ч埀顒佺⊕椤洩銇愰崱娑欌拻闁稿本鐟чˇ锕傛煙绾板崬浜伴挊婵囥亜閺嶎偄浠﹂柛瀣儔閺屾盯寮撮妸銉т哗缂備讲鍋撳鑸靛姇缁犲綊寮堕崼婵嗏挃闁诡喚鍘ч埥?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏㈡偘閳ュ厖澹曢梺姹囧灪椤旀牠鎮為幆顬″綊鎮╁▎蹇斿闁抽攱甯￠弻娑㈠即閵娿儰绨诲銈呮禋閸欏啴寮婚垾宕囨殕閻庯綆鍓涢惁鍫ユ倵鐟欏嫭绀冮柨鏇樺灪娣囧﹪骞栨担鑲濄劑鏌曡箛鏇炐″瑙勬礋濮婃椽宕ㄦ繝鍕窗闂佺瀛╅悡锟犮€?*闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰效婵犫偓娓氣偓濮婅櫣绱掑Ο铏逛紘濠碘槅鍋勭€氼喗绔熼弴鐔虹瘈婵﹩鍘鹃崣鍡椻攽閻樼粯娑ф俊顐幖鍗遍柛婵勫劤濡垶鏌熼鍡楁噽妤旈梻浣告惈婢跺洭鍩€椤掍礁澧柛姘儔楠炴牜鍒掗崗澶婁壕鐎规洖娴傞崯鍥╃磽閸屾瑨鍏岄柛瀣崌瀹曟洟宕樺顔惧姺闂佽法鍠撴慨鎾嫅閻斿吋鐓忛柛顐ｇ箖閸嬵亪鏌涢敂璇插箰闁稿鎹囬弫鎰償閳ヨ尙鍑归梻浣虹帛缁诲啫煤閻旂厧绠?12 缂?*闂?
    // first-run-setup 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴椤㈡洟鏁愰崱娆樻К闂備胶顭堢悮顐﹀礉鎼淬劌绠熼柟闂寸缁秹鏌涢锝嗗剷闁靛鏅滈悡鏇㈡煙閼割剙濡芥繛鍛閺屾稒鎯旈敐鍛亪闂佸搫鏈ú婵堢不濞戞埃鍋撻敐搴濈敖闁告梹娼欓埞鎴︽倷鐠鸿櫣姣㈠銈庡幖閻楁捇宕洪悙鍝勭闁挎棁妫勬禍褰掓煟閻樺弶鍘傞柛鎰屽懐鐓戦梻鍌氬€烽懗鍫曞箠閹捐鐤悹鎭掑妽瀹曟煡鏌熼幍顔碱暭闁稿鏅犻弻锝夊箣閿濆棭妫勭紓浣哄Х閺佸寮婚悢鐑樺枂闁告洖鐏氶悾鍓佺磽閸屾氨孝闁挎洦浜滈锝夘敃閿曗偓缁犳盯鏌℃径濠勪虎缂佹劖绋戦—鍐Χ閸℃﹩姊垮┑鐐差槹濞茬喖濡存笟鈧畷銊р偓娑櫱氶幏缁樼箾鏉堝墽鍒伴柟鑺ョ矒閺佸秴顓奸崶鈺冿紲濡炪倖娲栭幊搴ㄦ倶閻樼粯鍋傞柕鍫濐槹閻撶喖鏌￠崘銊モ偓鍝ユ暜閸洘鐓㈤柛鎰典簻閺嬫盯鏌＄仦璇插闁宠鍨垮畷閬嶅煛閸屽偊绠戦埞鎴﹀煡閸℃ぞ绨奸梺鎸庢磸閸ㄨ棄顕ｇ拠娴嬫闁冲灈鏂侀崑鎾绘晝閸屾氨顓哄┑鐘绘涧濞层劏銇愰鐐粹拻濞达絽鎲￠幆鍫熶繆椤愩垽鍙勭€规洖缍婇幊锟犲Χ閸♀晜缍楅梻浣筋潐閸庢娊鎮洪妸锕€鍨旈柟缁㈠枟閻撶喖鏌ｉ弬鎸庡暈缂佽泛寮堕妵鍕敃閵忊懣銏°亜椤忓嫬鏆ｅ┑鈥崇埣瀹曟﹢濡搁妷顔锯偓鎶芥⒒娴ｇ懓顕滄繛璇ч檮缁傚秴顭ㄩ崼顐ｆ櫔闂佹寧绻傚Λ娑氬姬閳ь剙鈹戦鏂や緵闁告ê鍚嬬粋宥咁煥閸曨亞绠氶梺缁樺姇濡﹪宕曡箛娑欏€垫繛鎴炲笚濞呭懘鏌嶇紒妯诲碍闁宠鍨归埀顒婄秵閸嬧偓闁归绮换娑㈠箻绾惧顥濆┑鐐茬湴閸婃洟顢氶敐澶婇敜婵°倓璁查幏濠氭⒑閸涘﹥澶勯柛瀣椤㈡棃顢旈崼鐔哄幗闂佺偨鍎辩壕顓犵不缂佹ɑ鍙忓┑鐘插亞閻撹偐鈧娲樼敮鎺楀煝鎼淬劌绠ｆい鎾跺晿濠婂嫮绡€闁汇垽娼цⅴ闂佺顑嗛幑鍥蓟濞戞鏃堝礃閵娿倖鐫忛梻浣告惈椤戝懐绮旇ぐ鎺戣摕闁哄浄绱曢悿鈧柣搴秵娴滅偞瀵奸崟顓犵＝濞达絾褰冩禍鐐節閻㈤潧孝濞寸厧鎳撻妵鎰板箳閹惧瓨鐝抽梻浣告啞娓氭宕ｆ惔鈭ユ稑鐣濋崟顑芥嫼闂佺鍋愰崑娑㈠焵椤掍胶澧悡銈夋煟閺傛娈犳繛鍏肩墪閳规垿鎮╃€圭姴顥濋梻浣稿船濞差參寮婚敐澶婃闁割煈鍣弳銏㈢磽娴ｇ瓔鍞虹紒鑸佃壘椤繐煤椤忓嫬绐涙繝鐢靛Т閹冲酣藟濠靛棭娓婚柕鍫濇閻忋儲銇勯敃鍌涙锭妞ゆ洩绲块幏鐘裁圭€ｎ偒娼旀繝纰樻閸ㄥ爼寮ㄩ柆宥忕稏鐎广儱顦伴埛鎴︽煕濞戞﹩鐓繛鍫燂耿閺屾稓鈧綆鍋嗛妴鎺楁煃瑜滈崜姘跺礃閼姐倕绶ゅù鐘差儏閻撴﹢鏌熸潏楣冩闁稿﹦鍏橀弻鐔虹磼濡櫣鐟查梺绋款煬閸ㄨ京鎹㈠┑瀣仺闂傚牊绋愮划鍫曟⒑閹稿孩纾搁柛濠冪箞閺佹劙鎮欓崜浣烘澑闂佺懓褰為悞锕€顪冩禒瀣ㄢ偓渚€寮崼婵堫槹濡炪倖鎸嗛崘鈺傛瘑闂傚倸鍊烽懗鍫曘€佹繝鍐彾闁割偁鍎辩粻鍨亜韫囨挻顥滅紒韬插€濋弻娑樷攽閸℃寮搁梺鎸庣箓椤︻垶鏌嬮崶顒佺厪濠㈣埖绋栭崥鍌炴煙瀹勭増鎯堥柍瑙勫灴閹瑧鎹勬ウ鎸庢毐婵＄偑鍊栧▔锕傚礋椤掍胶褰挎俊鐐€栧ú宥夊磻閹剧粯鐓涢悘鐐靛亾缁€瀣偓瑙勬礀閵堟悂骞冮姀銏㈢煓閹煎瓨鎸告禍楣冩煙闁箑寮炬繛鍫滅矙閺岋綁骞囬鐐电シ闂佸搫妫寸徊鐐┍婵犲浂鏁冮柕蹇曞У濞堫參姊虹€圭姵顥夋い锔炬暬閻涱噣骞掑Δ鈧粻濂告煕閹般劍娅囩紒澶愮畺濮婂宕掑顑藉亾妞嬪孩顐芥慨姗嗗墻閻掔晫鎲歌箛娑樼闁靛繈鍊曠粻鎶芥煙閻愵剚缍戠€殿喖娼″铏圭磼濡櫣浠存繝銏ｎ潐閻熲晛鐣烽崼鏇ㄦ晢濠㈣泛顑嗗▍鍥⒒娴ｇ懓顕滄慨濠傤煼瀵煡顢曢敃鈧崒銊ッ归悩宸剱闁抽攱鍨圭槐鎾存媴閸忓锛堝銈呯箰鐎氱兘宕甸弴銏＄厾闁归棿鐒﹀☉褏绱掗崜浣镐槐闁哄本鐩鎾Ω閵夈儳顔戦梻浣告憸婵敻宕濆▎鎾宠摕闁斥晛鍟欢鐐测攽閻樻彃顏ч柧蹇撻叄濮婃椽宕崟顒婄川缂備焦褰冨锟犲春閳ь剚銇勯幒宥囪窗闁哥喎绻橀弻娑㈡偐閹颁焦鐣介梺闈涙处閸旀瑥鐣峰鈧、娆撴嚃閳轰礁绠抽梻浣烘嚀閸氬鎮鹃鍫濈鐎光偓閳ь剛鍒掔紒妯肩瘈婵﹩鍘鹃崢鍛婄節閵忥絾纭炬い鎴濇嚇閹﹢鏁愭径瀣幈?up 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帊璁查弸娆撴⒑缂佹ê绗╁┑顔哄€楅幑銏犫槈閵忕姴鑰垮┑鈽嗗灥椤曆呭枈瀹ュ鐓熼柣鏂挎憸閹虫洜绱掗悩铏磳妤犵偛鍟灃闁告侗鍠楀▍婊堟煙閼测晞藟闁告挻绻堥幃妯侯吋婢跺鎷洪梺纭呭亹閸嬫盯鍩€椤掍焦绀嬮柟宕囧枛椤㈡稑鈹戦崼婊呰兒闂傚倸鍊烽悞锔锯偓绗涘懏宕查柛宀€鍋涚粻鐘诲箹濞ｎ剙濡兼慨鐟板级缁绘盯宕卞Ο铏逛患缂佺偓鍎抽妶鎼佸蓟閿濆應鏋庨悘鐐村灊婢规洟姊绘担瑙勫仩闁告柨鐭傞幃銉╂偂鎼搭喖娈ㄦ繝鐢靛У绾板秹宕戦崟顖涘€堕煫鍥ф捣閸斿秹鏌ｈ箛搴ｇ獢婵﹥妞藉畷婊堟嚑椤掆偓鐢儵姊洪崫銉バｆい銊ワ躬楠炲啫顫滈埀顒勫春閳ь剚銇勯幒鎴濐仾闁抽攱甯掗湁闁挎繂瀚鐔兼煟閹烘柨浜鹃柕鍥у楠炲鈹戦崶鑸碉紗婵°倗濮烽崑鐐哄垂閸喚鏆﹂柛顐ｆ处閺佸棝鏌嶈閸撴稓鍒掓繝姘唨妞ゆ挾鍠撻崢浠嬫⒑绾懏褰х紒鏌ョ畺钘熷璺虹灱绾惧ジ鏌ｅ▎鎰噧婵炶绠撻幃鈥斥枎閹寸姷锛滈柣搴秵娴滅偞绂掗姀掳浜滈柟鍝勵儏閻忣亪鏌曢崶褍顏€殿噮鍓欓埢搴ㄦ倷椤戭偓绠撳铏光偓鍦濞兼劙鏌涢妸銉﹀仴妤犵偛鍟撮崺锟犲川椤撶姷鏉稿┑鐐舵彧缁蹭粙骞夐垾鏂ユ瀺婵炴垯鍨洪埛鎺懨归敐鍛暈闁硅尙顭堥埞鎴︻敊閻愵剚姣勯梺鍝ュТ閿曘倝顢樻總绋跨倞闁靛鍎辩花銉︾節閻㈤潧浠﹂柛顭戝灦瀹曠銇愰幒鎾斥偓鍧楁煕椤垵浜栧ù婊勭矒閺岀喖鎮滃Ο铏逛淮濡炪倕娴氭禍顏堝蓟閿濆绠婚悗鐢电《濡插牓姊婚崶褜妯€闁哄被鍔岄埞鎴﹀幢濡儤顏￠梻浣烘嚀瀵爼骞冮崒姘煎殨妞ゆ劧绠戦柋鍥煏婢舵稓鐣遍柍褜鍓涢弫濠氬箖瀹勬壋鏋庨煫鍥ㄦ惄娴犲墽绱撴担鎻掍壕闂佸壊鍋侀崕鏌ユ偂閺囩喍绻嗘い鏍ㄧ矊瀛濆┑鐐茬湴閸婃繈骞冮垾鎰佹建闁逞屽墴瀵鎮㈤崨濠勭Ф闂佸憡鎸嗛崨顔筋啅闂傚倸鍊风粈浣哥暦椤掆偓鐓ゆい鎾卞灩閽冪喖鏌ㄥ┑鍡╂Ц婵☆偅锕㈤弻鐔虹矙閸噮鍔夐梺鍝ュУ閸旀瑥顫忕紒妯诲闁芥ê锛嶉幘缁樺仺妞ゆ牗绋戝ù顕€鏌涢埡鍐ㄤ槐妤犵偛顑夐弫鍌炴寠婢跺鐫忛梻鍌欑婢瑰﹪鎮￠崼銉ョ；闁告侗鍘滈幒妤€绀嬫い鏍ㄧ▓閹疯櫣绱撴担鍓插剱妞ゆ垶鐟╁畷鏇㈠箣閿旂晫鍘遍柣搴秵閸嬪棗煤閹绢喗鐓冮柕澶樺灣閻ｇ數鈧娲橀崕濂杆囬鈧弻锛勨偓锝庝邯閸欏嫰鏌″畝鈧崰鏍х暦瑜版帩鏁婇柛蹇撳悑椤斿姊绘担鍛婃儓闁硅櫕鍔欏畷鎴﹀Χ閸滀焦缍庡┑鐐叉▕娴滄粌顔忓┑鍡忔斀闁绘ɑ褰冮銈呪攽椤斿吋鎼愰柍瑙勫灴閹瑩骞撻幒鎾斥偓顖炴⒑閼姐倕鏆€闁告侗鍘奸悘濠冪節閻㈤潧校闁煎綊绠栧畷姗€鍩€椤掑嫭鈷戦梻鍫熶緱閻掗箖鏌涙繝鍐炬畼缂侇喛顕ч～婵嬫嚋绾版ɑ瀚奸梻鍌氬€搁悧濠勭矙閹惧瓨娅犻柡鍥ュ灪閻撴瑩鏌涢幇顓犲弨闁告瑥瀚埀顒侇問閸犳绻涙繝鍥х畺闁靛繈鍊曠粈鍌炴煠濞村娅呮鐐茬墛缁绘繂顕ラ柨瀣凡闁逞屽墯閹瑰洭骞婂Δ鍛唶闁哄洦菤閸嬫捇宕掗悙鑼舵憰闂侀潧顧€婵″洭宕㈤鍫熲拺闁硅偐鍋涢崝鈧梺鍛婂姇瑜扮偟妲愰弮鍫熺厽閹兼番鍩勯崯蹇涙煕閻樿櫕绀堥柛鎺撳笒閳诲酣骞囬鍡欑暰婵犳鍠楃换鍌炴儗閸惊锝夊箚瑜滃〒濠氭煏閸繃顥為悘蹇曟暬閺屾稑螣閻樺弶鎼愰柣鎾达耿閺岀喐娼忔ィ鍐╊€嶉梺绋匡功閸忔﹢骞冪憴鍕闁瑰嘲鐭堥崝鍛攽閳藉棗浜滈柣妤€妫濋崺鐐哄箣閿旇棄鈧鈧懓澹婇崰妤佹櫏闂傚倷鑳堕崕鐢稿疾閳哄懎鍨傚ù鐓庣摠閺呮繈鏌曡箛濠傚⒉濠殿垰顕槐鎺楊敍濞戞瑧顦ㄩ梺鍝勫€甸崑鎾绘⒒閸屾瑨鍏岀紒顕呭灦閳ワ箓宕奸妷顔芥櫔濠电姴锕ら崰姘跺疮閸涘瓨鐓忓璺烘濞呭棝鏌涙繝鍌滀粵闁逛究鍔岃灒闁绘挸楠告禒妯荤節閳封偓閸曨偒妫嗙紓浣介哺鐢繝銆佸璺哄窛妞ゆ挾鍋熼崫妤佷繆閻愵亜鈧呯不閹寸姴顥氭い鎾跺У椤?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕閵堝懎顏柡灞剧洴椤㈡洟鏁愰崱娆欑穿闂備線鈧偛鑻晶鍓х磼閻樿櫕灏柣锝夋敱缁虹晫绮欑拠淇卞妽閵囧嫰寮崶顬挻绻涢崨顓犲ⅵ婵﹦绮幏鍛存惞楠炲簱鍋撴繝鍥ㄧ厱闁规儳顕粻鐐烘煙椤旀儳鍘村┑锛勫厴閸┾剝鎷呴崫銉㈠亾椤撶偐鏀介柣妯肩帛濞懷囨煟濡も偓閸熸潙鐣烽幋锕€绠婚柤濮愬€曠粊锕傛⒑閸撹尙鍘涢柛鐘崇墬閹便劍鎯旈埦鈧弨浠嬫煟閹伴偊鏉洪柛銈咁儑缁辨帡鎮╅搹顐㈢濡炪倖娲╃紞浣哥暦濡警鍟呮い鏃€鍎冲鎶芥⒒娴ｇ顥忛柛瀣瀹曚即骞樼拠鎻掑亶闂佺懓澧界划顖炲磻閳╁啰绡€濠电姴鍊搁顐︽煟椤撶喎绗ч柍褜鍓濋～澶娒哄Ο鐓庡灊闁规崘顕х粻鏍煏韫囧鈧牠宕戦崒鐐茬婵烇綆鍓欓埀顒佺墵楠炲瀵肩€涙ǚ鎷绘繛鎾村焹閸嬫挻绻涙担鍐叉礌閳ь剨绠撳畷濂稿Ψ閵壯嶇吹婵＄偑鍊栭弻銊ノｉ崼銉︽櫜濠㈣泛锕崬鍫曟⒑缂佹ɑ鐓ュ褍娴风划璇差潩椤掑瀵岄梺闈涚墕濡稒鏅堕鍌滅＜閻犲洦褰冮埀顒€婀辩划瀣吋婢跺浠梺鐟板暱閿曘倗绱炴繝鍥х畺闁斥晛鍟崕鐔兼煏韫囨洖啸闁绘稈鏅濈槐鎾诲磼濞嗘劗銈版俊鐐茬摠閹倿鐛幇鐗堝殥闁靛牆娲ㄩ悾鍫曟⒑缂佹ɑ鈷掗柛妯犲洦鍊峰┑鐘插閸犳劙骞栧ǎ顒€濡介柛濠呮硾閵嗘帒顫濋敐鍛婵犳鍠栭敃銉ヮ渻閽樺鏆﹂柕濠忓缁♀偓闂佸憡鍔х徊鍓р偓姘煼濮婄粯鎷呯憴鍕哗闂佸憡鏌ㄩ柊锝咁嚕椤愶絿绡€婵ǜ鍎崑?5454 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓洪梺缁樺姇閻忔岸宕宠缁辨挻鎷呯粙娆炬殺闂佺顑冮崐婵嗩嚕鐠囨祴妲堥柕蹇曞閵婏负浜滈柡鍐ㄥ€哥敮鑸点亜椤愮喐娅婃慨濠呮缁辨帒螣閾忓湱鎳嗛梻浣筋嚙缁绘垵鐣濋崨濠佺箚闁汇垻顭堢粈瀣亜閺嶃劍鐨戞い鏂匡躬濮婅櫣鍖栭弴鐐测拤闂侀潧娲ㄩ崑鐐电博閻旂厧鍗抽柕蹇ョ磿閸樹粙鏌熼崗鑲╂殬闁糕晛瀚板畷顖濈疀濞戞瑧鍘遍梺缁樏壕顓熸櫠閻㈠憡鐓欐い鏂挎惈閸旀粎绱掗搹瑙勬珪缂侇喗鐟ч幑鍕Ω瑜庡В搴ㄦ⒒閸屾瑧鍔嶉柟顔肩埣瀹曟洟顢涢悙鑼槷闁瑰吋鐣崝濠囧焵椤掆偓閹虫﹢骞冨鍫熷殟闁靛鍎崑鎾绘偨閸涘﹦鍙嗗┑鐘绘涧濡繈顢撳Δ鈧湁婵犲﹤鎳庢禒閬嶆煛鐏炲墽鈽夐摶锝夋煕濠靛棗顏柛鈺冨仱濮婅櫣鈧湱濯鎰版煕閵娿儲鍋ラ柕鍡曠閳诲酣骞嬮悩鐑╂敽闂佽鍑界紞鍡涘礈濞嗘劖鍙忔繛宸簼閳锋帒霉閿濆懏鍟為柛鐔哄仦閵囧嫰顢橀悙鎼＆閻庤娲橀崕濂杆囬弻銉︾厱閻庯綆浜炴晥濠殿喖锕ら幖顐ｆ櫏闂佹悶鍎滈崨顒佸亝闂傚倷鑳堕、濠囶敋濠婂懏宕叉繝闈涱儏缁?444 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬闁告劦鍠楅悡銉╂煛閸ヮ煈娈斿ù婊堢畺濮婂搫效閸パ€鍋撳Δ鍛；闁规崘鍩栧畷鍙夌節闂堟稒宸濈紒鈾€鍋撻梻浣侯焾閺堫剛鍒掑畝鍕┾偓鍌毭洪鍛嫼闂佽姤锚椤︻垶寮抽悢鍏肩厱闁绘ê纾晶鐢碘偓娈垮枛椤嘲顕ｉ幘顔藉亜闁惧繗顕栭崯搴ㄦ⒒娴ｇ顥忛柣鎾崇墦瀹曚即寮介妸褏褰?502闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵噣宕奸悢鍛婎唶闂備胶顭堥鍡涘箰閸撗冨灊妞ゆ挾鍋愬Σ鍫熶繆椤栨繍鍤欐繛鍛囧洦鈷戞繛鑼额嚙楠炴鏌ｉ悢鍙夋珚鐎殿喖顭烽幃銏焊娴ｅ湱鈧姊婚崟顐ｅ枠妞ゃ垺淇洪ˇ鏌ユ偂閵堝棎浜滈柟鍨暞婵炲洭鏌嶈閸忔稓绮堟笟鈧敐鐐差煥閸繄鍔﹀銈嗗笂閻掞箓宕ｈ箛娑欑厓鐟滄粓宕滈悢鐓庤摕闁挎繂鎷嬪銊╂煃瑜滈崜娆撯€﹂崶顏嶆Ъ闂侀€涚┒閸旀垿寮崒鐐茬闁圭儤姊婚悾楣冩⒒娴ｈ櫣甯涢柛鏃撻檮缁傚秴顭ㄩ崼婵堝姦濡炪倖甯婇懗鑸垫櫠椤忓懌浜滈柡鍌濇硶缁犺尙绱掔紒妯肩畵妞ゎ偅绻堥、妤呭磼閿旀儳鑰块梻鍌氬€烽懗鍫曞箠閹惧瓨娅犻柣锝呰嫰閸ㄦ繃銇勯弽顐沪闁稿鍊块弻锟犲炊閳轰椒姹楅梺琛″亾濞寸姴顑嗛悡鐔镐繆椤栨繍鍤欑紒鑼额嚙闇夋繝濠傛噹娴犻亶鏌″畝瀣М妤犵偞锕㈤幃娆撴寠婢跺鍨濋梻鍌欑閹碱偊鎯夋總绋跨獥闁哄稁鍘归埀顑跨閳诲酣骞橀弶鎴炵枀闂備線娼ч¨鈧紒鎻掋偢閹艰鎯旈～顓犵畾闂侀潧鐗嗗ú銈呮毄闂備胶顭堥鍡涙儎椤栫偑鈧線寮介妸銉х獮闂佸綊鍋婇崜娑㈡偩濞差亝鈷戦柛鎰级閹牓鏌熷畡閭﹀剶鐎殿噮鍋婂畷姗€鍩炴径鍝ョ泿婵＄偑鍊栭崝锕傚磻閸曨垳宓侀柡宥冨妸娴滄粓鏌曟径娑氱暠闁告繄鍎ら幈銊︾節閸曨厼绗￠梺鐟板槻閹虫﹢骞愭繝鍐ㄧ窞濠电姴鍟崝鎺楁⒒閸屾瑧顦﹂柟鑺ョ矋閹便劑鎮界粙璺槷闂佸搫娲犻幊鍥焵椤掑﹦鐣电€规洖缍婇、姘跺川椤旇偐绱﹂梻鍌欑窔閳ь剛鍋涢懟顖涙櫠椤斿浜滈柡鍥朵簽缁夘喗顨ラ悙鏌ュ弰闁瑰磭鍋ゆ俊鐑藉Ψ閵堝洨娉块梻鍌氬€烽悞锕傚磿瀹曞洦宕查柟閭﹀墾閼板潡姊洪鈧粔鐢稿疾椤掍胶绡€濠电姴鍊搁弸銈嗙箾閹炬剚鐓奸柡灞炬礋瀹曠厧鈹戦崶鑸碉紒闂備胶绮幐鍫曞磿閻㈢钃熼柣鏂挎惈閺嬪牓鏌涘Δ鍐ㄤ粧缂併劌銈稿娲焻濞戞埃鏁€闂佸憡姊归崹鐢告偩閻戣棄绠抽柟瀛樻⒐閺佺娀姊虹拠鈥崇€婚柛灞惧嚬濡粌鈹戦悩鍨毄闁稿鐩幃褔宕卞☉娆忊偓鑸电節闂堟侗鍎忛柛鎴犲У缁绘盯骞嬪▎蹇曚痪闂佺粯鎸鹃崰鏍蓟閳ュ磭鏆ゆい鏃囧吹椤︼箑霉閻欌偓閸犳牞鐏冮梺缁橈耿濞佳勭濠婂牊鐓曢柣鏃堫棑閹界娀鏌熸笟鍨闁糕斁鍋撳銈嗗笒鐎氼參鎮¤箛娑欑厱妞ゆ劧绲跨粻鏍ㄣ亜閵壯冣枅闁哄本绋戦埢搴ょ疀閺冩垶锛嗛梻浣告惈閻寰婃ィ鍐ㄧ畾闁哄啫鐗滈弫宥嗙節婵犲倹鍟楅柨婵嗘缁犻箖鎮楀☉娆樼劷闁活厼顑夐弻锝呂旈崘銊愩垽鏌ｉ敐鍥у幋妤犵偛顑夐弫鍐焵椤掑倻涓嶉柤濮愬€楃壕钘壝归敐鍜佹綘妞ゅ繐鎳愰々閿嬬箾瀹割喕绨奸柍閿嬪灴閺屾稑鈽夊鍫濆闂佸疇妫勯ˇ鐢稿蓟閵娾晛鍗虫い蹇撴閺嗐倝鎮楃憴鍕８闁搞劋绮欏濠氬幢濡ゅ﹤鎮戦梺绯曗偓宕囩閻庢艾銈搁弻锝夋偄閸濄儳鐓傛繝鈷€鍕垫畼闁轰緡鍣ｉ獮鎺楀箻妫版繃閿ゅ┑鐐差嚟閸樠囨偤閵娾晜鍋傞柡鍥ュ灪閻撴瑩鏌熼鍡楄嫰濞堝爼姊虹€圭姵顥夋い锔炬暬瀵鏁撻悩鎻掕€块棅顐㈡处濮婂綊鎮靛顑芥斀妞ゆ梻銆嬮弨缁樹繆閻愭壆鐭欐鐐诧躬閹瑩宕崟搴涘妽閵囧嫰寮埀顒勫磿閾忣偅鍙忔繛宸簼閳锋帒霉閿濆懏鍟為柟鑼焾閳规垿顢欓懞銉ュ攭闂佸搫鑻敃顏堝极閹邦厼绶炲┑鐘插暞琚ｉ梻鍌欑窔濞佳嚶ㄩ埀顒傜磼閻樺啿鐏╂繛鍡愬灲閹瑩鎮滃Ο鐓庡箥闂傚倷绶￠崣蹇曠不閹寸偞娅犻柣妤€鐗勬禍婊堟煛閸愶絽浜鹃柣銏╁灡鐢繝鍨鹃弮鍫濈妞ゆ柨妲堣閺屾盯鍩勯崘顏呭櫑濡炪値鍊ら崶銊㈡嫼濠电偠灏褍顕ｆィ鍐╃厱闁绘ɑ鍓氬▓妯肩磼椤旇娅婇柟顔哄灲閹剝鎯旈垾宕囥偖闂傚倷鑳剁划顖毼涙担绯曟瀺闁挎繂顦伴崕濠囨煕椤愮姴鍔滈柍閿嬪灦閹便劑鎮烽悧鍫熸倷闂佽绻愬鍓佹崲濞戞瑦濯撮柛鎰絻椤忣偅銇勯埡鍛暠缂佺粯绻冪换婵嬪磼濮橀棿杩樼紓鍌欐祰椤曆兾涘▎鎾澄﹂柛鏇ㄥ灠缁犳盯鏌嶆潪鎷屽厡濞寸厧瀚板铏圭磼濡櫣鐟查梺璇″枛閸婂灝顕ｇ拠娴嬫闁靛繒濮烽崢鎾⒑鐠団€崇€婚柛灞惧嚬濡粌鈹戦悩娈挎殰缂佽鲸娲熷畷鎴﹀箣閿曗偓绾惧綊鏌″搴′簼闁哄棙绮撻弻鐔兼倻濮楀棙鐣堕梺姹囧€ら崰妤呭Φ閸曨垰鍐€妞ゆ劦婢€濞岊亪姊虹粙娆惧剳闁哥姵鍔欐俊鐢稿礋椤斿墽鏉搁梺瑙勫礃濞夋盯路閳ь剟姊绘担铏瑰笡闁规悂绠栧畷浼村冀椤撶偠鎽曢梺鍝勬川閸犳挾寮ч埀顒勬椤愩垺澶勬繛鍙夌矒瀵娊鎮╃紒妯锋嫼闁荤姴娲╃亸娆戠不濮樿埖鐓涢柛娑卞枤缁犳ɑ淇婇崣澶婂妤犵偞甯￠獮妯尖偓闈涙憸鑲栭梻鍌欑窔濞佳団€﹂崼銉ョ？闁瑰墎鐡旈弫鍕煕瀹€鈧崑鐐哄煕閹达附鈷戞い鎰╁€曟禒婊堟煠濞茶鐏￠柡鍛埣瀹曟粏顦寸痪鍓ф櫕閳ь剙绠嶉崕閬嶅箠韫囨蛋澶愭倷閻㈢數锛滈梺绋挎湰濮樸劌鐡俊鐐€ら崢褰掑礉閹存繄鏆﹂柣鎾崇岸閺€浠嬫煕閵夘喕绨介柛娆愬灴濮婂宕掑顑藉亾妞嬪孩顐介柨鐔哄Т缁€鍫熺箾閸℃ê鐏╅柣顓熸崌閺岋綁濮€閻樺啿鏆堥梺绋款儏閸婂潡寮婚敐澶婃闁割煈鍠楅崐顖炴⒑缂佹ɑ灏柛搴ゅ皺閹广垹鈹戠€ｎ偒妫冨┑鐐村灦閻燁垰螞閿熺姵鈷戦柣鐔告緲閹垿鏌涢弮鈧ú鐔凤耿娴ｇ硶鏀介柣妯款嚋瀹搞儵鏌涢悤浣镐喊闁糕晜鐩顕€宕掑鍜冪床闂佸搫顦悧鍕礉鐏炵煫褰掝敊闁款垰浜鹃悷娆忓缁€鍐偨椤栨稑娴柨婵堝仜閳规垿宕伴悧鍫濈瑨闁宠绉堕幏鐘诲矗婢跺﹥鐝撮梻浣侯焾椤戝棝骞愭ィ鍐ㄧ疅闁圭虎鍠栫粈瀣亜閹烘垵浜炴俊鑼嚀閳规垿鎮欓懠顒佹喖缂備緡鍠栭惌鍌炲箖閵夛妇闄勯柛娑橈工娴犳帒顪冮妶鍡樼５闁稿鎸鹃埀顒冾潐濞叉牠鎮ユ總鎼炩偓浣肝旈崨顓犲姦濡炪倖甯婄粈渚€宕濋弮鍫熲拻闁稿本鐟х粣鏃€绻涙担鍐叉硽閸ヮ剦鏁囬柕蹇曞Х閿涙瑩姊洪崫鍕闁?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ姀鐘差棌闁轰礁锕弻鈥愁吋鎼粹€崇缂備焦鍔栭〃鍡樼┍婵犲洤围闁告侗鍘藉▓鏌ユ⒑閸濆嫭顥炵紒顔肩У缁岃鲸绻濋崶顬囨煕濞戝崬骞楁繛鍫濈埣濮婂搫煤鐠囨彃绠洪梺鑽ゅ暱閺呯姴顕ｆ繝姘亜闁稿繒鍘ч埀顒傜帛娣囧﹪顢涢悙瀛樼彸缂備緡鍠栭悥鐓庮潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯婢х晫妲愰幒妤佸亹闁惧浚鍋勭喊宥夋⒑娴兼瑧绉柡鈧潏鈺冪处闁伙絽鐬奸惌娆撴偣閸ワ箑瀚庨柛鏂跨Ч婵＄敻宕熼姘辩潉闂佹悶鍎洪悘婵嬪箻鐎靛摜顔曢梺鍛婁緱閸樻儳鐡梻浣哥枃椤宕归崸妤€绠栭柍鍝勬噹閸ㄥ倹銇勯幇鍓佺ɑ闁伙箑楠搁埞鎴︽偐閹颁礁鏅遍梺鍝ュУ瀹€绋款嚕椤愶絿绡€婵﹩鍓涢悿鍥р攽閻樼粯娑фい鎴濇噽缁牓宕奸姀鈥虫瀾闂婎偄娲︾粙鎺楀磿濡ゅ懏鐓曢柍鈺佸暟閳洟鏌ｉ幘瀛樼闁逞屽墮閸樻粓宕戦幘缁樼厓鐟滄粓宕滈悢椋庢殾闁硅揪闄勯崐鐑芥煟閹寸伝顏呯濡も偓閳规垿鎮欓崣澶樻！闂佸湱顭堥…鐑藉箖閳ユ枼鏋庨柟鎵虫櫃缁ㄥ姊洪崫鍕犻柛鏂块叄閸╁﹪寮撮悩鐢碉紲闂佽鍨卞姗€鎯冨ú顏呯厵妞ゆ洖鎳庨弸娑欍亜椤愶絿绠為柟铏矌閸犲﹤螣鐠囧樊浼栭梻鍌氬€风粈浣革耿闁秮鈧箓宕煎婵嗙秺閹粓鎸婃径灞藉笌闂備礁鎼ú銊╁窗瀹ュ洦宕查柛鈩兦滄禍婊堟煙閹佃櫕娅呴柣蹇婃櫊閺岋繝宕遍鐘冲仹闂侀€炲苯澧い鏃€鐗犲畷浼村冀椤撴稈鍋撻敃鍌涚叆閻庯絺鏅濈粻姘舵⒑瑜版帗锛熺紒鈧担鍛婃殰闂傚倷绶氬褏鎹㈤崱妞綁宕ㄩ褏鍔烽梺鍝勫暊閸嬫挾绱掔紒妯肩疄闁诡喕绮欏Λ鍐ㄢ槈濡ゅ啯宕熷┑鐘殿暯濡插懘宕戦崨瀛樺仭闁宠桨鑳堕弳锔界節闂堟稒鐒鹃柡鍐ㄧ墕閻掑灚銇勯幒鎴濐仼闁绘挻绻堥弻鐔煎箲閹伴潧娈柛銉ョ摠缁绘繈濮€閿濆棛銆愬銈嗗灥閸婂潡鐛箛鎾佹椽顢旈崨顖氬箰濠电姰鍨煎▔娑㈡嚐椤栨稓顩烽柟鐑橆殕閻撴洘淇婇妶鍌氫壕闂佸摜鍠庡锟犲灳閺嶃劎绡€闁搞儯鍔岄埀顒€顭烽弻銈夊箒閹烘垵濮㈤梺鍛婎焾妞村摜鎹㈠┑瀣仺闂傚牊鍒€閵忋倖鐓曢柣鏃傤焾椤ュ绱掗纰辩吋闁诡喗鐟╅幃婊兾熼悡搴＄濠电姷鏁搁崑鐐哄垂闂堟稓鏆︽い鎺戝閸戠娀鏌曢崼婵愭Ч闁绘挸鍟伴幉绋款煥閸繄顦梺闈浤涢崨顖ｆЦ闂備胶纭堕崜婵堢矙閹寸姷鐜绘俊銈勬缁诲棝鏌曢崼婵囧櫤闁革絾妞介弻娑氣偓锝庝簻閳ь剙娼″濠氬即閻旈绐為梺绯曞墲钃遍柣婵囨礋濮婅櫣绮欓崠鈥冲闂佽桨绀侀…宄邦嚕婵犳艾惟闁靛鍨洪弬鈧梻浣虹《閸撴繃绗熷Δ鍛劦妞ゆ帊绶″▓鏃€銇勯鈥冲姷妞ぱ傜窔閺屾盯濡搁妷褍鐓熼悗娈垮枦椤曆囶敇閸忕厧绶為悗锝庡墮瀵娊姊虹拠鎻掑毐缂傚秴妫濆畷鎴﹀幢濞存澘娲俊姝岊槾缁炬崘妫勯湁闁挎繂鐗婇鐘绘偨椤栨稓娲撮柡宀嬬秮閸┾剝绻濋崒婊€妗撻梻浣虹帛娓氭宕板璺虹劦妞ゆ帒锕︾粔鐢告煕閻樻剚娈滅€规洘鍔欓幃婊堟嚍閵壯冨箰濠电姰鍨煎▔娑㈩敄閸涘瓨鍊堕柍鍝勬噺閻撱儲绻涢幋鐑囦緵婵¤尙鎳撹彁闁搞儜宥堝惈闂佽鍠楅悷鈺呫€侀弮鍫濈闁靛鍎版竟鏇㈡⒑閸濆嫯鐧侀柍褜鍓熷鎼佸冀椤撶喓鍙嗛梺鍝勬川閸嬫盯鍩€椤掆偓閹芥粎鍒掗崼鈶╁亾閿濆骸浜栧ù婊勭矒閺岀喖宕崟顒夋婵炲瓨绮撶粻鏍箖濡も偓閳绘捇宕归鐣屼邯婵犵數鍋涢惃鐑藉焵椤掍礁澧繛鍏肩墬缁绘稑顔忛鑽ょ泿缂備讲鍋撻柛鎰靛枟閻撴洘绻涢崱妤佺婵℃彃缍婇弻娑欐償閵忊€崇３濠殿喖锕ュ浠嬪箖閻戣棄绾ч柟绋垮閻︼絾绻濋悽闈涗粶妞ゆ洦鍙冨畷妤€顫滈埀顒勬偘椤旇棄绶為柟閭﹀墰椤旀帒顪冮妶鍡橆梿濠殿喓鍊曢埢宥夊幢濡偐鐦堝┑鐐茬墕閻忔繈寮搁悢鍏肩厱闁哄啠鍋撻悽顖滃仱閹崇偞娼忛妸褏鐦堝┑顔斤供閸樻悂骞忓ú顏呪拺闁告稑锕︾粻鎾绘倵濮樼厧鏋﹂柍顏勭秺濮婄粯鎷呴崨濠傛殘闂佸搫琚崝鎴﹀蓟婵犲洦鏅插璺猴功閻ｉ箖鎮楅崗澶婁壕闂侀€炲苯澧撮柛鈹惧亾濡炪倖甯掗敃锔剧矓閻㈠憡鐓曢柣妯诲墯濞堟粓鏌熼鍏夊亾閺傘儲顫嶉梺闈涢獜缁辨洟宕㈤柆宥嗏拺闁哄倶鍎插▍鍛存煕閻樺疇澹橀柍缁樻尭铻ｉ柤娴嬫櫇閿涙粓姊虹紒妯忣亪骞栭锕€瑙︾憸鐗堝笚閻撳啰鎲稿鍫濈婵﹢顤傞弫濠囨煛瀹ュ骸骞栫紒鎰殜閺屸€愁吋鎼粹€崇闂佺顑呴ˇ顖炲煘閹达箑纾兼繝濠傛捣閸旀悂姊洪崫鍕仴闁稿海鏁诲璇差吋婢跺﹦鍘告繛杈剧到閹测€斥枔閸欏绡€婵炲牆鐏濋悘锟犳煙閸涘﹤鈻曟鐐插暙閻ｏ繝骞嶉搹顐も偓濠氭椤愩垺澶勯柟灏栨櫆缁傛帗銈ｉ崘鈹炬嫼缂備礁顑呯亸鍛寸叕椤掑嫭鐓涢柍褜鍓氬蹇涘煛閸屾艾绨ユ繝鐢靛█濞佳兾涘畝鍕嚑闁哄啫鐗婇悡鏇熴亜椤撶喎鐏ラ柡瀣枛閺岋綀绠涢幘缂幯囨煛鐏炶鈧繂鐣疯ぐ鎺撴櫇闁逞屽墮鐓ら柨鏇炲€归崑鍌炴煛閸ャ儱鐏柣鎾寸洴閺屾盯骞囬崜浣稿煂婵炲瓨绮撶粻鏍ь潖濞差亜绠归柣鎰絻椤牆鈹戦悙鐐光偓瀣崲濠靛宓侀柛鎰ㄦ櫅椤曢亶鏌熺喊鍗炲箺闁哄應鏅犲娲礂闂傜鍩呴梺绋块叄娴滆泛鐣峰┑瀣窛閻庢稒菤閹锋椽姊婚崒姘卞缂佸鎸剧划濠氭倷閻戞鍘遍梺闈涱焾閸庢椽鎮℃總鍛婄厵妞ゆ牗鐟х粣鏃傗偓瑙勬礀閵堝憡淇婇悜鑺ユ櫆閻熸瑥瀚褰掓⒒閸屾瑧顦﹂柟璇х磿缁瑩骞嬮敂鑺ユ珖闂佹寧绻傚ú锕傤敃娴犲鈷戞い鎺嗗亾缂佸鏁婚幃锟犲即閵忥紕鍘繝鐢靛Т缁绘帡鎮鹃柆宥嗙厓缂備焦蓱缁€鍐煏閸パ冾伃濠殿喒鍋撻梺鎸庣☉鐎氼參宕虫导瀛樺€垫繛鍫濈仢閺嬬喖鏌熷灞剧彧闁逛究鍔戦崺鈧い鎺戝閻撳啴姊哄▎鎯х仩濞存粓绠栧娲偡閺夋寧些闂佸湱鈷堥崑濠囧春閻愬搫绠ｉ柨鏃囨閳ь剛鍏橀幃鐟懊洪鍡欑獥闂佽桨绀侀悘婵嬧€旈崘顔嘉ч柛鈩冾焾閸嬩線姊洪崨濠冨闁告瑥楠哥叅闁归棿鐒﹂埛鎴︽偡濞嗗繐顏╂い銉ヮ儏椤儻顦虫い銊ワ躬楠炲啴鏁撻悩鎻掑祮闂侀潧绻嗛埀顒佹灱閸嬫捇鎮介崨濠勫弳濠电娀娼уΛ婵嬵敁濡も偓闇夋繝濠傚缁犳﹢鏌嶈閸撴繈锝炴径濞掓椽寮介鐔峰壒闂佺鐬奸崑娑㈡嫅?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鎾閳╁啯鐝栭梻渚€鈧偛鑻晶鏉款熆鐟欏嫭绀冪紒缁樼箓椤繈顢橀悩鎻掔疄闂傚倷鐒﹂弸濂稿疾濞戙垹鐤ù鍏兼綑閻撯€愁熆閼搁潧濮堥柣鎾存礋閹﹢鎮欐０婵嗘婵犳鍠栭崐鍧楀蓟閳ュ磭鏆嗛悗锝庡墰琚﹂梻浣芥〃閻掞箓宕濆▎蹇曟殾闁靛ň鏅╅弫濠囨煠閹帒鍔氭い蹇庡嵆濮婂宕掑▎鎴М闂佸湱鈷堥崑鍕亱濠电偛妫欓崹鐢稿垂濠靛鐓欓柟瑙勫姦閸ゆ瑩鏌﹂崘顏勬灁闁逞屽墮缁犲秹宕曢柆宥嗗亱婵犲﹤鍠氶悗鍫曟煏婵炵偓娅嗛柍閿嬪灴閺屾稑鈽夊鍫熸暰闁诲繐绻掗弫濠氬蓟濞戙垺鍋愮€规洖娲ら埅鐟邦渻閵堝棙绌跨紒鎻掑⒔閹广垹鈹戦崱鈺傚兊濡炪倖鍨熼弬渚€寮查悩璇茶摕闁哄洨鍠庣欢鐐烘煕椤愶絿绠樺ù鐘冲劤椤啴濡舵惔鈥茬盎缂備礁顦伴幐鎶藉春閻愬搫绠ｉ柨鏇楀亾闁绘搫绻濋弻娑㈠焺閸愮偓鐣兼繛瀵稿Л閺呯姴顫忛搹瑙勫珰闁肩⒈鍓涢澶岀磽娴ｇ瓔鍤欓柣妤佹崌婵″瓨绗熼埀顒€顕ｉ鈧畷濂割敃閿濆棙鐝濋梻鍌欑窔濞佳呮崲閸℃鐎剁憸鏃堢嵁韫囨拋娲敂閸涱亝瀚奸梻浣告啞缁嬫垿鏁冮敂鍓х＝婵鍩栭悡鏇㈠箹濞ｎ剙濡洪柟鎻掑⒔閳ь剚顔栭崰妤佺箾婵犲洤钃熼柛鈩冾殢閸氬鏌涢埄鍐噧妤犵偛鐗撳铏规嫚閹绘帒姣愮紓鍌氱Т濡繂鐣烽幋锕€宸濇い鎾跺У濞堥箖鎮楅崗澶婁壕闂佸憡娲﹂崜娑㈠储閹间焦鍊甸柛蹇擃槸娴滈箖鏌ｆ惔顖滅У闁告挻绋栭埅鐢告⒒閸屾瑦绁版い鏇熺墵瀹曚即骞掑Δ鈧悿鐐節婵犲倸鏋ら柣鎺嶇矙閻擃偊宕堕妸褉妲堥梺缁樻尰缁嬫垿婀侀梺鎸庣箓閹冲酣藟韫囨稒鐓熼幖杈剧磿閹界姷绱掔紒妯肩疄闁诡喕绮欏Λ鍐归崜浣镐槐闁哄本鐩俊鎼佸Ψ椤旀儳鎮戞俊銈囧Х閸嬫盯鏁冮鍫㈠祦婵☆垯璀﹂悢鍡涙煕閺囥劌澧伴柍钘夘樀閺屽秶鎷犻弻銉ュ及濡ょ姷鍋為敋闁伙絿鍏樺畷鍫曞煛閸屾稓閽掗梻鍌氬€搁崐椋庢濮橆剦鐒界憸鎴炴櫠濠靛鈷戦柛婵嗗閸ｈ櫣绱掔€ｎ偅灏甸柛鎺撳笚缁绘繂顫濋鐐扮敾闂備胶鎳撻崯鍧楁晝閵堝洨绀婂┑鐘叉搐閺嬩線鏌熼悧鍫熺凡鐎瑰憡绻冮妵鍕籍閸ヮ煈妫勯梺鍝勵儐閸ㄥ灝顫忕紒妯诲濡炲绨肩憰鍡欑磽閸屾氨袦闁稿鎸绘穱濠囨倷鐎涙绋囬梺纭呭Г缁捇鐛幋锕€顫呴柣姗嗗亝椤秴鈹戦悙鍙夘棡闁告梹顨堢划鏃堝醇閺囩啿鎷洪梺鍛婄☉閿曘儳鈧灚鐟╅弻娑㈠即閵娿儰绨甸柡澶嗘櫆閿氶柍瑙勫灴閺佸秹宕熼鈩冩線闂備胶顭堣噹闁告侗鍘剧粵蹇涙⒑閸忚偐銈撮柡鍛洴瀹曠懓鈹戦崶鈺冾啎闂佺硶鍓濋〃鍫㈢不閹惰姤鐓?
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
