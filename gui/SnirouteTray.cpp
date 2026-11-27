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

// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏犖ч柛灞剧煯婢规洖鈹戦鐭亜鐣烽鍕偍閻庣數纭堕崑鎾舵喆閸曨剛顦銈庡亜椤﹂亶鍩€椤掑嫭娑ч柕鍫㈩焾椤曪綁宕奸弴鐐殿吅闂佺粯锚閻忔艾袙鎼淬垻绡€闁汇垽娼ф禒鈺呮煙濞茶绨界€垫澘锕幊鐐哄Ψ瑜忚ぐ鐐節閻㈤潧孝婵炲眰鍊濋幃鍧楊敋閳ь剟寮诲☉妯锋婵☆垰鍚嬮幉濂告⒑缂佹ɑ灏靛┑鐐╁亾闂佸搫鐭夌紞渚€鐛鈧獮搴ｇ驳鐎Ｑ冧壕婵°倓闄嶆禍婊堟煥濠靛棙鍣介柨娑樼Ф缁辨帗娼忛妸銉﹁癁闂佽鍠掗弲鐘诲箠閿熺姴围闁搞儯鍔嶉惁鎺楁⒒閸屾瑧顦﹂柟纰卞亰閹绺界粙璺ㄧ崶闂佸搫绋侀崢鎯х暤娴ｆ悶鈧帒顫濋敐鍛婵°倗濮烽崑鐐垫暜閿熺姷宓侀悗锝庝簴閺€浠嬫煕閳锯偓閺呮粎绱為幋锔解拻闁稿本鑹鹃埀顒勵棑濞嗐垹顫濈捄铏瑰姦濡炪倖甯掗崐鍛婄濠婂牊鐓犳繛鑼额嚙閻忥繝鏌￠崨顓犲煟妞ゃ垺宀搁崺鈧い鎺戝閽冪喐绻涢幋鐐垫噮缂佲檧鍋撻梻浣告啞濞诧箓宕滃鑸靛仧妞ゅ繐鐗婇埛鎴︽煟閻旂顥嬮柣锝庡弮閺屾盯濡搁妶鍥╃厯闂佺粯渚楅崰姘跺焵椤掑﹦绉甸柛鐘愁殜瀵彃鈹戠€ｎ偆鍘撻悷婊勭矒瀹曟粓鎮㈤悡搴ｇ暫?0=闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫆闁芥ê顦純鏇熺節閻㈤潧孝闁挎洏鍊楅埀顒佸嚬閸ｏ綁濡撮崨鏉戠煑濠㈣泛鐬奸惁鍫熺節閻㈤潧孝闁稿﹦绮弲璺衡槈閵忥紕鍘遍柣搴€ラ崟顒傚綃闂備礁鎼張顒€煤濡吋宕叉繛鎴炵矤濡插鏌ｆ惔銏犲毈闁告挾鍠庨?1=闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃秹锝炲┑瀣櫇闁稿矉濡囩粙蹇旂節閵忥絽鐓愰柛鏃€鐗犲畷鎴﹀Χ婢跺鍘搁梺鎼炲劗閺呮稑鐨梻浣虹帛鐢帡鏁冮妷鈺佄﹂柛鏇ㄥ枤閻も偓闂佸湱鍋撻幆灞轿涢敓鐘冲€甸悷娆忓缁€鈧悗娈垮枛婢у酣骞戦姀鐘闁靛繒濮烽鍝勨攽閻愬弶顥滅紒缁樺笚缁傛帡鎳栭埡鍐紳?2=闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫆闁芥ê顦純鏇熺節閻㈤潧孝闁挎洏鍊楅埀顒佸嚬閸ｏ綁濡撮崨鏉戣摕闁靛濡囬崣鍡椻攽閻樼粯娑ф俊顐ｇ懄瀵板嫰宕熼娑樹壕閻熸瑥瀚粈鈧悗娈垮枛婢у酣骞戦姀鐘闁靛繒濮烽鍝勨攽閻愬弶顥滅紒缁樺笚缁傛帡鎳栭埡鍐紳婵炶揪绲介幖顐ｇ墡濠电姷鏁搁崑娑㈡晝椤忓嫷鍤曞┑鐘宠壘閸楁娊鏌ｉ弮鍫缂佹劗鍋涢埞鎴︽倷閺夋垹浠搁梺鐓庣秺缁犳牠骞嗙仦瑙ｆ斀閻庯綆鍋€閹?
// 闂?warp-cli status 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极瀹ュ绀嬫い鎺嶇劍椤斿洭姊绘担瑙勫仩闁稿孩妞藉畷婊冣枎閹存繍妫滈悷婊呭鐢鎮″☉銏＄厱闁靛绲介崝姘舵煟韫囷絼閭柡灞剧〒閳ь剨鎬ラ崟顒傚絽闂佸墽绮悧鐘诲蓟閵娿儮鏀介柛鈩兠▍锝夋⒑閸濆嫭顥犻柛瀣ㄥ€濆濠氭晸閻樺弶銇濇繛杈剧悼椤牓锝為锔解拺闂侇偆鍋涢懟顖涙櫠椤曗偓閺屾稑螣閼姐倗鐓夐悗瑙勬礃閸ㄥ潡鐛Ο鑲╃＜婵☆垳鍘ч獮鍫ユ⒒娴ｇ瓔娼愭繝銏∩戦弲鑸垫償閵娿儳顦梺鍝勵槹閿涙洟鎮烽柇锔惧弳闂佸壊鍋侀崹濠氬汲椤愶附鈷戦悹鍥ｂ偓铏亶闂佹寧娲忛崹浠嬬嵁閸愩劉鏋庨柟瀵稿仜绾绢垶鏌℃径灞戒户濠⒀勵殜閹牐绠涘☉娆屾嫽婵炶揪缍€濡嫰宕ヨぐ鎺撶厱闁绘ê鍟挎慨澶愭煙楠炲灝鐏茬€规洖銈告俊鐑藉Ψ閵婏附鍟洪梻鍌欐祰濞夋洟宕伴幘瀛樺弿閻庨潧鎽滈惌鍡涙倵閿濆骸鏋熼柍閿嬪浮閺屾盯顢曢妶鍛亖闂侀潧妫欑敮鎺楁箒闂佺粯鐟㈤崑鎾绘煕閵娿倕宓嗛柛鈺冨仱楠炲鏁傞挊澶夋睏婵＄偑鍊栭崝锕€顭块埀顒€顭跨憴鍕婵﹦绮幏鍛村川婵犲倹娈樺┑鐐存尰绾板秹銆冩繝鍌滄殾闁荤喐澹嗛弳锕傛煕閵夋垵鏈ˉ鈥斥攽鎺抽崐褏寰婃禒瀣柈妞ゆ牜鍋為崑鍌涚箾閸℃ɑ灏伴柛濠傜仛缁绘盯骞嬮悙鍨櫧闂佺粯甯為弲顐ゆ閹烘鍋愰柧蹇ｅ亜闂夊秴顪冮妶鍐ㄥΩ闁稿海鏁婚獮鍐ㄢ枎閹垮啯鏅滈梺鍛婃处閸樿櫣妲愬▎鎾粹拻濞达絽鎲￠幆鍫ユ偠濮樼厧浜扮€规洘绻傞悾婵嬪礋椤愩倕寮ㄥ┑鐘灱濞夋稖澧濋梺鍝勵儏闁帮綁寮婚悢琛″亾閻㈡鐒惧ù鐙呯畵閺屾稒鎯旈姀鈽嗘綗SQUE 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愭い鏃傛嚀娴滈箖鏌涢幇闈涙灈鐎瑰憡绻冮妵鍕冀閵娧€妲堥柟鍏兼綑閿曘倝鍩為幋鐐茬疇闂佺锕ュú鐔肩嵁韫囨洍鍋撻敐搴℃灈闁哄绀侀埞鎴︽偐閸欏顦╅梺缁樻尰濞叉鎹㈠☉銏犵婵犻潧妫滈崺鐐烘⒑閸濆嫷鍎嶉柛濠冪箞瀵顓奸崶鈺冿紲濠碘槅鍨卞鍨涢崘顔解拺閻犲洠鈧櫕鐏嶅銈冨妼閿曨亜顕ｉ锕€绠涙い鎾跺枎閸斿懘姊洪悙钘夊姕闁告挻鑹鹃…鍥礈瑜忕壕浠嬫煕鐏炲墽鎳呮い锔奸檮閵囧嫰鏁傜拠鑼化缂備浇浜崑鐐垫崲濠靛鐐婄憸鎴﹀箺閺囩偐鏀介柣鎰綑閻忥箓鏌ㄩ弴妤佹珔闁靛棙甯楃换婵嗩潩椤撶姴骞楅梻浣烘嚀閻°劌顫忛悷鎵虫瀺闁告侗鍔堕悷閭︾叆闁割偁鍨婚弳顐︽⒑閸濆嫮娼ら柛鈩冪懅閺夋悂姊洪幐搴ｇ畵闁瑰啿绉瑰畷娆愮鐎ｎ偀鎷洪梺鍛婄☉閿曘倝宕甸鍕厱閻庯綆浜濋崳浠嬫煙楠炲灝鐏╅柍钘夘槸閳诲酣骞囬鎷樸倝姊绘担鍛婂暈闁圭妫濆畷姗€濡歌鐢挻绻濋悽闈浶ユい锝堟鍗遍柛娑欐綑绾惧鏌涘☉妯兼憼闁稿绱曢幉姝岀疀濞戞顦梺鍦劋閹稿墽寮ч埀顒勬⒑閹肩偛鍔橀柛鏂跨Т閳诲秹宕堕浣叉嫼缂備礁顑嗙€笛冿耿閹殿喚纾奸悗锝庡亝瀹曞瞼鈧娲忛崹浠嬬嵁閹烘嚦鏃€绗熼崶顭戞濠碉紕鍋戦崐鏍礉閹达箑鍨傞柣鎴灻欢鐐烘煕閺囥劌鐏￠柣鎾寸懅缁辨帞鈧綆鍋勯婊堟煙閸欏鍊愰柡宀€鍠庨悾锟犳焽閿曗偓閸撳爼鎮楀▓鍨灈妞ゎ參鏀辨穱濠囨倻閼恒儲娅嗛梺璇″瀻閸愵亶鍟嬮梻鍌氬€搁崐鐑芥倿閿曞倸纾块柛鎰梿閻熼偊娼╅悹楦挎閿涙盯姊洪崨濠冨闁搞劑浜跺畷鎰崉鐞涒剝鏂€闂佺粯蓱瑜板啴顢旈銈囨／闁诡垎鍐╁€繛锝呮搐閿曨亝淇婇崼鏇炵＜婵﹩鍋勯ˉ姘攽閿涘嫬浜奸柛濞у嫪绻嗛柛銉墮閺嬩線鎮归崶褎鈻曢柛銈嗘礀閳规垿鎮╃€圭姴顥濋悗鐟版啞缁诲啴濡甸崟顖氱婵°倐鍋撻悗姘煎墴瀹曟绻濋崶銊㈡嫽婵炶揪绲介幉锟犲疮閻愮儤鐓欑紒瀣椤ャ垻鈧娲︽禍婵嬪箯閸涙潙绀?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰紦閻㈢粯淇婇悙顏勨偓鏍偋濡ゅ啰鐭欓柟鐐湽閳ь剙鎳樺畷锝嗗緞瀹€鈧惁鍫ユ⒒閸屾氨澧涚紒瀣浮閺佸秹寮崼鐔哄幘?status 闂?Connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗霉閿濆牊顏犵紒鈧繝鍌楁斀闁绘ɑ褰冩禍鐐烘煟閹烘梹娅曢柟鍙夌摃缁犳盯寮撮悙鑼喊闂備礁鎼崯顐︽偋閸℃瑧涓嶆い鏇楀亾闁哄矉绠戣灒濞撴凹鍨辨瓏闂備胶纭堕弲婵嬪磻濞戔懇鈧棃宕橀鍢壯囨煕閳╁喚娈橀柣鐔村姂濮婅櫣绮欓崠鈥充紣缂傚倸绉撮敃銈夋偩閻戣棄绠ｆ繝鍨姈瀵ゆ椽姊洪柅鐐茶嫰婢у鈧鍠栭…宄邦嚕閹绢喗鍋勯柧蹇氼嚃閸熷酣姊绘担绋款棌闁绘挸鐗撳畷浼村冀閵婏箑寮块梺鍓插亝濞叉﹢鍩涢幋锔界厱婵炴垶锕Λ姘熆鐟欏嫸鑰块柡灞剧⊕缁绘繈宕掑☉妯规樊闁诲孩顔栭崰鏍€﹂悜钘夌畺闁靛繈鍊栭崑鍌炲箹鏉堝墽绉甸柛鐔奉儐娣囧﹪鎮欓鍕ㄥ亾閺嶎灐娲冀椤撶偟鐤囬棅顐㈡处閾斿寰勯幇顒€鍞ㄩ悷婊勭矊閵嗘帗绻濆顓犲帾闂佸壊鍋呯换鍐闯閾忓湱纾奸柛灞惧嚬閸庢棃鏌＄仦鍓ф创妤犵偞顭囬埀顒勬涧閹诧繝宕氬☉妯滄棃鎮╅棃娑楃捕缂備胶绮敮锟犲春閳ь剚銇勯幒鎴姛缂佸鏁婚弻娑㈡偐瀹曞洤鈪瑰┑鐐村絻濞尖€愁潖缂佹ɑ濯撮柛娑橈工閺嗗牆鈹戦悙棰濆殝缂佺姵鎸搁悾鐑藉级濞嗙偓鍍电紓浣规閸ㄦ澘煤閿旈敮鍋撴担鍐ㄤ汗闁逞屽墯缁嬫帟褰滈梺鐟板槻椤嘲顫忛搹鍦煓闁圭瀛╅幏杈╃磼閻愵剚绶茬紒澶屾嚀閻ｇ兘顢涘鍏煎兊闂佺厧鎽滈弫鎼佸储闁秵鈷戦梻鍫熶緱濡牓鏌涢妸锕€鈻曢柟顔惧亾濞煎繘濡搁敐鍕泿闂備線娼ч¨鈧紒鐘冲灴瀹曨偊鎮滃Ο璇插伎婵犵數濮撮崯顖炲Φ濠靛牃鍋撶憴鍕８闁告柨绉堕幑銏犫攽鐎ｎ亞顦板銈嗗坊閸嬫挻绻涢崨鍥ф处閳锋帒霉閿濆牜娼愰柛瀣█閺屾盯寮捄銊愶絿绱掔紒妯尖姇缂佺粯绻堝畷鎺楀Χ閸涱噮鍚欏┑锛勫亼閸婃牠宕濊缁骞嬪┑鍥╁嚱濠电姷鏁告慨浼村垂閻撳簶鏋栨繛鎴炵矌濡?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亜顒㈡い鎰矙閺屻劑鎮㈤崫鍕戙垻鐥幆褜鐓奸柡灞剧☉閳藉宕￠悙瀵镐壕闂備礁纾幊鎾剁矓瑜版帒钃熺憸鎴犵不濞戞﹩娼╅柛蹇氬亹缁夌兘鏌?sniroute.cmd 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗霉閿濆浜ら柤鏉挎健濮婃椽顢楅埀顒傜矓閻㈢纾婚柨鐔哄У閻撴瑩鏌熼婊冾暭妞ゃ儱顦伴〃銉╂倷椤忓嫮浼堥梺鍝勮嫰缁夌懓鐣烽锕€绀嬫い鎰枎娴滃墽鎲搁悧鍫濈瑨缂?warp on/off闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢琛″亾閻㈡鐒惧ù鐘欏洦鈷掗柛鏇ㄥ亜椤忣參鏌″畝瀣瘈鐎规洘锕㈡俊鎼佸Ψ閵忕姳澹曢梺鐓庮潟閸婃绋夊澶嬬厸闁稿本渚楅崕銉╂煟閺傛寧顥㈤柟顔款潐閵堬箓骞愭惔顔诲摋闂備浇顕х换鎰暦椤掑嫧鈧棃宕橀钘変画闂佸搫顦扮€笛冃掗幇顒夋富闁靛牆楠稿銊︾箾婢跺绀嬫鐐村灴瀹曠喖顢涘☉妯圭病闂備礁鎼ˇ浼村垂閻㈤潻缍栭柣妯兼暩绾句粙鏌涚仦鍓ф噮闁告柨绉归弻娑㈠棘閻愬弶鍣虹€规洖寮剁换婵嬫濞戝崬鍓伴梺钘夊暟閸犳牠寮婚弴鐔虹闁割煈鍠栨慨澶娾攽閳╁啫绲婚柣妤€锕ョ粚杈ㄧ節閸ヨ埖鏅┑鐘绘涧濡骞嗛悙瀵哥闁哄鍨甸幃鎴澝瑰鍛亾濠?cli 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛顥堟繛鎴濆船閸撴壆绱掗悙顒€鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯掗崐椋庡垝閻㈠憡鈷戦柛婵嗗椤ョ偟绱掗鑺ュ磳鐎殿喖顭烽幃銏ゆ惞閸︻叏绱查梻渚€娼х换鎺撴叏閻㈠憡鍊堕柛顐犲劜閳锋垶绻涢懠棰濆殭妤犵偞顨嗛妵鍕Ω閵夛富妫︽繝纰夌磿閺佽鐣烽悢纰辨晬婵﹩鍓欓弫銈夋⒒娓氣偓濞佳嗗櫣闂佸憡渚楅崹鍗炩枔婵傚憡鈷掗柛灞剧懅椤︼箓鏌熼悮婧垮€栭幊宀勬⒒娴ｇ儤鍤€闁规祴鍓濈换娑欑節閸モ晛绁﹂梺鍛婂姦閸犳宕愭繝姘參婵☆垯璀﹀Σ鍝モ偓瑙勬尭濡鍩為幋锔绘晩闁哄倶鍊涢埀顒冩硶閻ヮ亪顢橀悪鈧悞楣冨础鏉堛劊浜滈柡鍥殔娴滄儳鈹戦纭锋敾婵＄偘绮欓悰顕€寮介鐔封偓閿嬬箾閺夋埈鍎忓ù婊堢畺閺屸€愁吋鎼粹€崇闂佹悶鍔岄崐鍧楀蓟濞戙垺鏅滈悹鍥ㄥ絻缁犲綊姊洪幖鐐插缂佽鐗撳濠氭晸閻樿尙顦ㄩ梺闈浤涢崘鐐瘒缂傚倸鍊烽懗鑸垫叏閸偆绠惧┑鐘叉搐閽冪喖鏌ㄥ┑鍡╂Ц缂佺姵绋掗妵鍕籍閸ヮ灝鎾存叏閿濆懎顏╂い顏勫暣婵℃儼绠涢幘鑸敌掗梻浣侯焾椤戝懘鏁冮妶澶涚稏?
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


// 闂?winget 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪Ч濠€渚€姊虹紒妯虹伇婵☆偄瀚划濠氭偐缂佹鍘甸梺璇″瀻閸愨晩鍟堥梻浣规偠閸庢椽宕滃▎鎴犱笉妞ゆ洍鍋撻柡?Cloudflare WARP闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣椤愯姤鎱ㄥ鍡楀⒒闁绘帞鏅幉绋款吋閸澀缃曞┑鐘茬棄閺夊簱鍋撻弴銏犵疇閹艰揪绲介ˉ姘舵煟閻旂娅炵憸鐗堝笚閸嬫劗鈧懓澹婇崰鏍礈闁秵鈷戦柣鐔告緲缁狙囨煥閺囨ê鐏叉鐐插暙铻栭柛娑卞枟濞呮粓鏌熼懖鈺勊夐柍褜鍓欓崯璺ㄧ不閿濆鈷掑ù锝勮閻掔偓銇勯幋婵囶棦妤犵偞鍔栭妶锝夊礃閵娧呮瀫闂備礁婀遍搹搴ㄥ窗閺嶎偆鐭嗛悗锝庡枟閻撴洟鏌嶉埡浣稿箻妞ゅ繐鐗婇崐鍫曟倵閻㈢數銆婇柛瀣尭閳绘捇宕归鐣屼邯婵＄偑鍊ら崢楣冨礂濮椻偓閻涱噣宕橀纰辨綂濡炪値鍋掗崢楣冨礈閵婏妇绠鹃弶鍫濆⒔缁夘剚銇勯銏╂█鐎殿喛娅曠换婵嬪炊閵娿垺瀚藉┑鐐舵彧缁插潡鎮洪弬娆剧劷闁归偊鍘剧弧鈧梺闈涚箚閸╁嫰骞嬮敃鈧悿楣冩煠缁嬭法浠涢柛搴ｅ枛閺屻劌鈹戦崱妯虹獩闂佺粯甯掗崥瀣崲濠靛顫呴柨婵嗘閵嗘劙鏌ら悷鐗堟拱濞ｅ洤锕獮鎾诲箳閺傚簱鍙洪梻浣告惈閻ジ宕伴弽顓犲祦闁糕剝鍑瑰Σ楣冩⒑閹稿海鈽夌紒澶屾嚀閻ｇ兘顢涢悙鑼啇婵炶揪绲介崢婊堝箯濞差亝鈷戠痪顓炴噹娴滃綊鏌涚€ｎ偆娲撮柛鈹惧亾濡炪倖甯婇懗鑸垫櫠闁秵鐓曢柟鐑樻尭缁楁帡鏌ｉ敐鍡欑疄鐎规洜鍠栧畷褰掝敃閿濆棛褰熼梻鍌氬€搁…顒勫磻閸曨個娲晜閸撗呯厯闂佺懓顕慨鐢稿礂濠婂牊鐓冪憸婊堝礈閻斿娼栭柧蹇撴贡绾惧吋淇婇姘儓妞ゎ偅鍨块弻锝堢疀閹惧墎顔夐梺鑽ゅ暱閺呯姴鐣峰ú顏勭劦妞ゆ帊闄嶆禍婊堟煙閸濆嫭顥滃ù婊勫劤椤啴濡舵惔鈥崇闂佺绻戦敋妞ゆ洩绲块幏鐘裁圭€ｎ偒娼旀繝纰樻閸ㄥ爼寮ㄩ柆宥嗗€靛┑鐘崇閳锋垹鎲搁悧鍫濈瑨濞存粈鍗抽弻娑㈠箻鐎电硶濮囩紓浣稿€圭敮锟犵嵁濮椻偓椤㈡瑧鎲撮敐鍡楊伖婵犵數鍋炲娆撳触鐎ｎ喗鍋＄憸蹇曞垝婵犲洦鍊婚柤鎭掑劤閸橀亶姊洪崫鍕偓绋款焽閿熺姴鐭楅柛鏇ㄥ墻濞堜粙鏌ｉ幇顒佲枙婵″弶鎮傞幗鍫曞冀椤撶喓鍘卞┑鐐村灥瀹曨剟鐛弽顐ょ＜闁归偊鍘鹃埥澶嬨亜椤忓嫬鏆ｅ┑鈥崇埣瀹曞崬螣閻戞ɑ顔傞梻鍌欒兌椤牓顢栭崶顒€绐楅幖娣€楁禍钘夆攽閻愬瓨灏伴柛鈺佸暣瀹曟垿骞樼€涙ê寮?
// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佲枙闁绘帟濮ょ换娑㈠幢濡纰嶉柣鐔哥懕缁犳捇骞冨Δ鍛櫜閹肩补鍓濋悘宥夋⒑閹肩偛鈧洟鎳熼婵堜簷闂備焦瀵х换鍌炲箠鎼淬劌姹查柣鎰劋閻撴洟鏌曢崼婵囧櫤闁瑰吋鍔欓弻宥囨喆閸曨偆浼岄梺璇″枓閺呯娀鐛幘璇茬闁圭偓鎯岄崯鈧梻鍌氬€风粈浣哄椤撶姴鍨濇い鏍嚤濞戙垹绀冩い鏃囧閹芥洟鎮楅獮鍨姎闁绘绮岄‖濠囧Ω閳哄倵鎷洪梺鍛婄☉閿曘儳浜告导瀛樼厽闁绘柨寮跺▍濠勨偓?UAC闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢琛″亾濞戞瑯鐒界紒鐘卞嵆閺屻倝骞忕仦鍓с€婄紓浣介哺鐢帟鐏冮梺鍛婂姀閺呮繆銇愰崠?闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊瑜忛弳锕傛煟閵忋埄鐒剧紒鎰殜閺岀喖鏌囬敃鈧崝鎺撶箾瀹割喕绨荤紒鐙呯稻缁绘繈濮€閿涘嫮鐣电紓鍌氱Т閿曘倝鎮鹃悜钘夐唶闁哄洢鍔嶉弲鐐烘⒑閹稿孩纾甸柛瀣尰閵囧嫰鏁傞挊澶樻闂佺懓绠嶉崹褰掑煘閹寸姭鍋撻敐搴濈敖闁稿﹦鍋ら幃宄邦煥閸愵€囨煙椤曗偓濞佳囷綖濠婂牆鐒垫い鎺戝缁犳牗淇婇妶鍛櫧鐎规挷绶氶弻娑⑩€﹂幋婵囩亪婵犳鍠栭柊锝咁潖濞差亝鍋￠柡澶嬪濮ｆ劙姊绘担绋跨盎缂傚秳绶氬顐﹀箛閻楀牆鈧兘鏌涘┑鍡楃弸鐟滃秹鈥︾捄銊﹀磯濞撴凹鍨伴崜鎶芥⒑閸涘⊕顏勭暦椤掑嫬鐓橀柟杈鹃檮閸嬫劙鎮归崶鍥у暕缁憋綁姊绘担鍛婃喐闁哥姴绉瑰畷锟犲箮閽樺鐣洪梺缁樺姇閹碱偆绮婚悷鎳婂綊鏁愰崨顔跨闂佸憡蓱閹瑰洤顫忓ú顏呭殥闁靛牆鎳忛悗鐐節閵忋垺鍤€婵☆偅鐟╁鏌ュΨ閳哄倻鍘介柟鍏肩暘閸娿倕顭囬幇顓犵闁告瑥顦介悞浠嬫煙楠炲灝鐏茬€规洖銈告俊鐑藉Ψ瑜岀槐婊堟⒒娴ｇ懓顕滄繛鍙夌墵瀹曟劙宕妷褌绗夐梺瑙勫劶婵倝鎮″☉銏＄厱闁规壋鏅涙俊鐓幟瑰鍛沪濞ｅ洤锕畷锝嗗緞鐎ｎ亜鏀繝娈垮枛閿曪妇鍒掗鐐茬闁告稑鐡ㄩ崑锟犳⒑椤撱劎鐣辨繛鍫熺箘缁辨捇宕掑▎鎴М濡炪倖鍨靛Λ婵嗙暦瑜版帒閿ゆ俊銈傚亾缂佺姷濞€閺岋綁寮崒妤佸珱闂佽桨绀佺粔褰掑蓟閿濆绠涙い鎾跺枔缁嬪洦绻涚€电孝闁靛牏顭堥～蹇撁洪鍜佹濠电偞鐣崐鏍儎椤栨氨鏆﹂柣鏃傗拡閺佸啴鏌ㄥ┑鍡橆棤鐟滅増妞藉濠氬磼濮橆兘鍋撴搴ｇ焼濞撴埃鍋撴鐐差樀閸╁嫰宕橀鍡樺殞婵犲痉鏉库偓鏇㈠箠鎼淬劌纾奸柕濞垮剻閺冨牊鏅查柛娑卞幗濞堫厼鈹戦鐐殌鐎殿喖鐖奸獮鍫ュΩ閵夊海鍠栧畷褰掝敊閹冪細闂傚倷鑳剁划顖炲箰鐠囪娲偄閻撳氦鎽曢梺缁樻煥閸氬宕愮紒妯诲弿婵°倐鍋撴俊顐ｇ懇閹箖鎮滈懞銉㈡嫼闁哄鍋炴竟鍡欑玻閺傛５鐟邦煥閸愵亜鐓熼悗娈垮枛椤兘寮澶婄妞ゅ繐鎳庢刊浼存⒒娴ｅ憡鎯堥柛濠傜埣瀹曟劕鈽夐姀鐘盒曞銈嗗姧闂勫嫰鍩涢幋锔界厱闁挎棁顕у楣冩煕閺冣偓缁海妲愰幒妤冨彄妞ゆ挾濮烽悡鎾绘⒑閻愯棄鍔电紒鐘虫尰娣囧﹪骞栨担鍝ュ幐婵炶揪绲肩拃锕€危娴煎瓨鈷掑〒姘ｅ亾婵炰匠鍡楁闂佽绻掗崑鐐碘偓姘唉椤﹁鎱ㄦ繝鍌ょ吋鐎规洘甯掗埢搴ㄥ箳閹存繂鑵愮紓鍌氬€风欢锟犲闯椤曗偓瀹曞綊宕奸弴鐐典紜闂佹寧娲栭崐鎼佹煁閸ヮ剚鐓忓璺虹墕閸旀粍淇婂顔煎⒋婵﹥妞藉畷顐﹀礋椤撴稒鐎遍梻浣告啞閿氶柕鍫熸倐楠炲啴鎮欓崫鍕€銈嗗姉婵磭鑺辨繝姘拺闁荤喐澹嗛幗鐘电磼鐠囨彃鏆ｇ€规洘鍨挎俊鐑芥晜鐠恒劎鐣鹃梻浣告啞閹稿棝宕橀妸銉ь吅闂?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋娆忕仾闁稿鍊濋弻鏇熺箾閻愵剚鐝旈梺鍛婅壘缂嶅﹪鐛弽顬ュ酣顢楅埀顒佷繆閼测晝妫柟瑙勫姦濞兼劕菐閸パ嶈含濠碘€崇埣瀹曘劑顢橀悢鍓插敳闂?CLI闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍏煎€绘慨妤€妫欓悾鐑芥⒑閹肩偛鈧洟鎳熼婵堜簷闂備焦瀵х换鍌炲箠鎼淬劌姹查柣鎰劋閻撴洟鎮楅敍鍗炲閻濇岸姊洪崫鍕効缂佹彃鈧喓浜介梻浣告啞缁诲倻鈧凹鍘艰闁归偊鍓﹀〒濠氭煏閸繃顥炵紒鈧径濠庢富閻庢稒蓱閸婃劖顨ラ悙鑼ⅵ濠碘剝鎮傛俊鐑筋敊閽樺绱﹂梻鍌欒兌绾爼宕滃┑瀣ㄢ偓鍐川鏉堝墽绋忛梺鍐叉惈閹冲繘鎮￠悢鍏肩厪闊洤锕ュ▍鍛存煟韫囨梻娲撮柡灞剧⊕閹棃鏁愰崱妯荤槗婵?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶闁告挆鍛闂佽瀛╅懝楣兯囨导鏉懳﹂柛鏇ㄥ灠缁犳娊鏌熺€涙绠ュù鐘荤畺濮婅櫣绮欓崸妤娾偓妤併亜閿旂偓鏆€殿喛顕ч埥澶娢熼柨瀣垫綌婵犵數鍋涘Λ娆撳礉濡ゅ啰鐭欓柛銉墯閻?winget 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪箻楠炴垿濮€閵堝懐鐤€濡炪倖妫佸Λ鍕償婵犲洦鈷戠憸鐗堝笒娴滀即鏌涢…鎴濆缂佸倸绉归幃鐑芥焽閿旀儳鏁搁梺鑽ゅ█缂傛艾鈻嶉敐澶嬪€堕弶鍫氭櫇绾惧ジ鏌ｅΟ鎸庣彧妞ゃ儱鐗撻弻锛勪沪閸撗€妲堥梺瀹犳椤﹂潧鐣烽敓鐘冲€烽悗鐢告櫜閹茬偓绻?Cloudflare 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪Ч閿濈偛鈹戠€ｎ偅娅囬梺绋跨焿婵″洨绮欒箛鏃傜瘈闁靛骏绲剧涵楣冩倵濞戞帗娅婃鐐诧工閻ｆ繈宕熼鑺ュ濠电偠鎻徊浠嬪箟閿熺姴绠氶柛顐犲劜閻撶喐銇勯幒鍡椾壕闂侀潧鐗忛…鍫ユ偩闁垮顕遍柡澶嬪灩椤旀帡鏌ｉ悩鍙夌┛閻忓繑鐟╁畷婵嬫晝閳ь剟鈥旈崘顔嘉ч柛鈩冾殘閻熴劑姊虹粙鍖″伐缂傚秴锕ら悾鐑芥偨缁嬭法鍔﹀銈嗗坊閸嬫挻銇勯鍕殻濠碘剝鍎肩粻娑樷槈濞嗘ǚ鍋撳鍡欑瘈闁汇垽娼ф禒婊堟煥閺囨ê濡奸摶鐐烘煏韫囥儳纾跨紒鈾€鍋撻梻浣圭湽閸ㄨ棄顭囪閻☆厽绻濋悽闈涗粶闁活亙鍗冲畷鎰旈崘銊ョ亰闂佸壊鍋呯换鍕汲閸℃稒鐓曢柡鍥ュ妼婢т即鏌涘Ο鍝勮埞闁宠鍨块幃娆撳级閹寸姳娣柣搴ゎ潐濞诧箓宕滃杈╃焿鐎广儱顦粻娑㈡煟濡も偓閻楀繘宕?
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佹儓闁搞劌鍊块弻娑㈩敃閿濆棛顦ョ紓浣哄С閸楁娊寮诲☉妯锋斀闁告洦鍋勬慨銏ゆ⒑濞茶骞楅柟鐟版搐椤繑绻濆顒勫敹闂佺粯鏌ㄩ崲鏌ョ嵁韫囨稒鈷戦柟绋挎捣缁犳挻绻涚仦鍌氬闁诡垰鑻埢搴ㄥ箻鐎电骞堥梻浣虹帛閺屻劑宕查弻銉ユ辈闁绘劗鍎ら崑銊︺亜閺嶃劎鐭婇柡瀣⒒缁辨帡顢欏▎鎯ф闂佸疇妫勯ˇ鍨叏閳ь剟鏌ｅΟ娲诲晱闁哥喎顑夊缁樻媴缁涘缍堥梺绋挎捣閸樠嗙亱闂佸憡娲﹂崰鎾寸?Windows 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亜顒㈡い鎰矙閺屻劑鎮㈤崫鍕戙垻绱掗幇顓ф畷缂佺粯鐩獮瀣倷閸偄娅楅梻浣告啞閻熴儳鎹㈠鈧濠氬即閵忕娀鍞跺┑鐘绘涧濞层倝鎮甸弮鍫熲拺?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢琛″亾濞戞瑯鐒界紒鐘卞嵆閺屻倝鎮ч崼婵愬殝闂侀潧娲ょ€氱増鎱ㄩ埀顒勬煏韫囧鐏い搴¤埗闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣椤愯姤鎱ㄥ鍡楀⒒闁绘帞鏅幉绋款吋閸澀缃曢梻鍌欑閹碱偊宕锔藉亱濠电姴娲ら崥褰掓煕閺囥劌鐏￠柣鎾存礋閺岀喖骞嗚缁狙呯磼娴ｈ绶查柍瑙勫灴椤㈡瑩鎸婃径鍡樼亞闂備浇顕栭崰鏍礊婵犲洨宓佸〒姘ｅ亾妤犵偞甯￠獮瀣攽閹邦喒鍋撳畡鎵虫斀闁绘劖娼欓悘锕傛煙缁嬫寧鎲哥紒顔芥閵囨劙骞掗幋锝嗘啺闁诲骸绠嶉崕閬嶆偋濠婂喚鐎堕柕濞炬櫆閳锋垿鏌涘☉姗堟敾閻忓繒鏁婚弻娑㈡偐瀹曞洤鈷岄梺缁樹緱閸ｏ絽鐣峰鈧、妯款槼闁宠鐭傞弻锝嗘償閵堝孩缍堝┑鐐插级缁挸鐣烽姀锝庢▌闂佺硶鏅濋崑銈夌嵁鐎ｎ喗鏅濋柍褜鍓熼弻瀣炊閵娧呯槇闂傚倸鐗婄粙鎾存櫠閺囩噥娈介柣鎰ㄦ櫅娴滈箖姊婚崒姘偓鎼佸磹瀹勬噴褰掑炊瑜忛弳锕傛煟閵忋埄鐒剧紒鎰殜閺岀喖骞嶉纰辨毉闂佺顑戠换婵嬪蓟閺囥垹閱囨繝闈涙搐椤︹晛顪冮妶鍡楀缂侇喗鐟╁濠氬即閵忕娀鍞跺┑鐘绘涧濞层倝藝閳轰緡娓婚柕鍫濋娴滄繃绻涢崣澶涜€挎鐐茬墦婵℃悂濡锋惔锝呮灈妤犵偞鍔栭幆鏃堟晲閸愶絽浜鹃柟鐗堟緲缁?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫇闁逞屽墰婢规洘绻濆顓犲幈闂佹枼鏅涢崯浼村煡婢舵劖鐓曢悘鐐额嚙婵倿鏌＄仦璇测偓鏇㈡箒闁诲函缍嗛崑鍛存偟閺囩儐娓婚柕鍫濋娴滄繃绻涢懠顒€鏋涚€殿喖顭烽幃銏ゅ川婵犲嫮肖闂備礁鎲￠幐鍡涘川椤旂瓔鍟呴梻鍌欐祰瀹曠敻宕伴幇鐗堝仭閺夊牃鏅濇稉宥夋煙閹规劕鍚圭€规挷绶氶弻鈥愁吋鎼达絼绮撻柟鍏兼儗閻撳牓寮崱娑欑厓鐟滄粓宕滈悢鐓庣畺婵せ鍋撻柛鈺嬬節瀹曘劑顢橀悩鍨瘞闂傚倷鑳剁划顖炲蓟閵娾晛瑙﹂悗锝庡枛閻撴洟鏌熸潏楣冩闁绘挻娲樼换婵嬫濞戞瑯妫炲銈呮禋閸嬪嫰鈥﹂懗顖ｆ▌婵炲瓨绮岄悥鐓庮嚕鐠囨祴妲堥柕蹇曞Х椤旀帡鏌ｆ惔銊︽锭闁硅绻濆畷瀹狀樄婵?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫇闁逞屽墰婢规洟鎮烽幊濠傜秺閹晛顔忛鐓庡Η缂傚倸鍊哥粔鐢告偋閺団懇鈧箓宕稿Δ浣告疂闂傚倸鐗婄粙鎴︼綖閸ヮ剚鈷戦梺顐ゅ仜閼活垶宕㈤崫銉х＜闁逞屽墯缁楃喖鍩€椤掑嫨鈧礁顫滈埀顒€鐣峰Δ鍛亗閹艰揪绲块悰顔尖攽閻樺灚鏆╁┑顔碱嚟閳ь剚姘ㄩ崗妯虹暦椤掑嫭顥堟繛鎴炲笚閺傗偓闂備胶绮敃鈺呭窗閺嶎厽鍊堕弶鍫涘妿缁犳儳顭跨捄渚剳婵炲弶鎸抽弻鈩冩媴缁嬫寧娈绘繝纰夌磿閺屽藝閹绢喗鐓熸い鎾跺仦椤ャ垽鏌＄仦鐣屝ユい褌绶氶弻娑㈠箻鐠虹儤鐎诲銈庡亜缁绘劗鍙呭銈呯箰鐎氼亞妲愰崼鏇熲拺闁告稑锕ョ粈鈧梺璇茬箲娣囨椽宕愰幘顔解拻闁稿本鐟ㄩ崗灞俱亜椤撶偟澧㈤柍褜鍓欓悘姘辨暜閳ュ磭鏆︾憸鐗堝笒闁卞洭鏌￠崶鈺佷沪婵″樊鍓熷?
//
// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佹儓闁搞劌鍊块弻娑㈩敃閿濆棛顦ョ紓浣哄У瑜板啴婀侀梺鎸庣箓閹冲酣藟韫囨梻绠鹃悗娑欘焽婢х敻鏌＄仦鐐缂佺粯绋栭ˇ鏌ユ煕閻曚礁浜伴柟顔缴戠换婵嬪炊閵娿垺瀚奸梻浣告啞閹告槒銇愰崘鈺冾洸闁绘劗鍎ら悡鏇㈡煟濡寧鐝€规洖鐭傞弻娑㈠煘閸喚浼堥悗娈垮枟閹歌櫕鎱ㄩ埀顒勬煥濞戞ê顏╂鐐茬墛娣囧﹪鎮欓鍕ㄥ亾閺嶎厼绀夋俊銈勭劍閸欏繘鏌熺紒銏犳灈缂佲偓婢跺绡€濠电姴鍊搁獮妯兼喐閻楀牆绗氶柛瀣姉缁辨帒鈽夊鍏兼暞闂侀€炲苯澧繛纭风節瀵鎮㈢喊杈ㄦ櫓闂佽鍨庨崘銊ь吅闂佽瀛╅鏍窗濡も偓椤洤鈻庨幙鍕◤濠电娀娼уù鍌氼焽濞嗘挻鐓涚€广儱鍟竟澶嬬節婵犲倻澧涢柍閿嬪笒闇夐柨婵嗩樈濡垵鈽夐幘鐟扮毢缂佽鲸甯″畷锝嗗緞婵犲骸娈濈紓鍌欒兌缁垶鎯勯鐐叉槬闁逞屽墯閵囧嫰骞掗幋婵愪痪闂佺粯鎸荤粙鎴︽箒闂佹寧绻傞幊蹇涘箟閸濄儮鍋撳☉娆戠畵妞ゎ亜鍟存俊鑸垫償閳ヨ尙鎸夐梻浣规偠閸斿鍒掑▎鎾村仒妞ゆ洍鍋撻柛鈹惧亾濡炪倖宸婚崑鎾绘煃鐟欏嫬鐏︾€垫澘瀚埀顒婄秵閸嬫帒顭?闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏犖ч柛灞剧煯婢规洖鈹戦鐭亜鐣烽鍕偍閻庣數纭堕崑鎾舵喆閸曨剛顦ョ紓鍌氱Т閿曨亜顕ｆ繝姘耿婵°倕锕ら幃鎴︽⒑閸涘﹣绶遍柛鐘崇〒濞嗐垽濮€閵堝棌鎷绘繛杈剧悼閹虫捇顢氬鍛＜閻犲洦褰冮埀顒冨劵濡喎顪冮妶鍡欏濠㈣娲熷畷鎴﹀箻閹颁焦鍍靛銈嗗笒椤︾増鏅ョ紓鍌氬€烽懗鍓佸垝椤栨粎鐭欓柟杈惧瘜閺佸﹪鐓崶銊ょ繁闁轰礁锕弻銈嗘叏閹邦兘鍋撻弴銏犲惞妞ゆ帒瀚埛鎺懨归敐澶樻濞戞捁灏欑槐鎺楁偐閸愯尙浼岄梺璇″暙閸パ咁啋闂佸綊顣﹂悞锔剧礊鎼粹檧鏀介柣鎰级閳绘洖霉濠婂啰鍩ｇ€殿噮鍋婂畷鍗烆渻閺囩偟绉洪柡浣瑰姍瀹曞ジ顢曢敐鍥┬ラ梻鍌欒兌椤牏鈧稈鏅犻獮鎰板礈瑜庨～鏇㈡煙閹呮憼濠殿垱娼欓湁闁绘ê妯婇崕蹇涙煙閻ｅ本鏆╃紒杈ㄦ尰閹峰懐绮电€ｎ亝顔勯梺璇插閻噣宕￠崘鑼殾婵炲樊浜滈悞鍨亜閹哄秹妾峰ù婊勭矒閺岀喖鎮滃Ο铏逛淮闂侀€炲苯澧紓宥咃工閻ｇ兘鎮界粙璺ㄥ姦濡炪倖甯掔€氼參鍩涢幋锔界厱婵犻潧瀚崝姘箾閸涱厾绠婚柡灞剧洴婵℃悂鏁冮埀顒勫煝閺囩姭鍋撳▓鍨珮闁稿锕妴浣割潩鐠鸿櫣鍔﹀銈嗗笒鐎氼剟寮伴妷鈺傜厓鐟滄粓宕滃璺何﹂柛鏇ㄥ灠缁犳娊鏌熺€涙濡囬柛瀣崌閹筹繝濡堕崪浣诡棥濠电娀娼ч崐鎼佸箟閿熺姵鍋傞柣鏂垮悑閻撴瑥霉閻撳海鎽犳繛鎳峰喚鐔嗙憸婊堝窗濡ゅ懎桅闁告洦鍨伴崡鎶芥煟閺冨洦顏犻柣鎾茬劍缁绘盯鏁愰崨顔芥倷闂佹寧娲︽禍顏堢嵁閸愩劉鏋庨柟鎯х－椤斿矂姊洪悷鐗堟儓婵☆偅顨嗙粋宥嗐偅閸愨晝鍘遍悷婊冮叄閵嗗啴宕ㄩ婊€绗夐梺鍝勭▉閻忔劘銇愰幒鎾充汗閻庣懓澹婇崰鏍礈娴煎瓨鐓欐い鏍ㄧ矊閺嬫稒鎱ㄦ繝鍐┿仢妤犵偞鍔栭幆鏃堝椤喚绌块梻鍌欒兌椤牓顢栭崱娑樼闁搞儺鍓欓拑鐔兼煥濠靛棭妲哥紒鐘差煼閹鈽夊▎妯煎姺濡炪倧瀵岄崣鍐潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯閸ｏ綁寮婚敐鍫㈢杸闁哄啫鍋嗛弳锛勭磽娴ｈ鈷掗柛鐘虫崌閸┾偓妞ゆ帒锕︾粔鐢告煕閻樺磭澧卞瑙勬礃缁绘繂顫濋鐘插箥闂備浇顕栭崹搴ㄥ川椤掑嫬浠愰梻鍌欒兌椤牓顢栭崱娑樼闁搞儜鍛閻熸粎澧楃敮鈺呭极閸愵喗鐓忛煫鍥э攻濞呭棙銇勯妷锔藉磳婵﹦绮幏鍛瑹椤栨粌濮兼俊鐐€栭崹闈浳涘┑瀣祦闁圭増婢樼粻鐟懊归敐鍛础闁伙絾濞婂濠氬磼濞嗘帒鍘＄紓渚囧櫘閸ㄥ爼寮崘顔肩厸闁告侗鍘鹃悰銉╂⒑閸濆嫮鈻夐柛妯垮亹缁牊寰勭€ｎ剛顔曢梺绯曞墲钃遍悘蹇庡嵆閺?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲梺姹囧€ら崰妤呭Φ閸曨垰鍐€妞ゆ劦婢€缁爼姊洪幖鐐插闁稿﹤娼″璇测槈閵忊晜鏅濋梺鎸庣箓濡盯藝閵夆晜鈷戦柦妯侯槸閺嗙喖鏌涢悩宕囧⒌鐎殿喛顕ч埥澶愬閻樻牓鍔戦弻銊モ攽閸℃ê娅ｅ銈庡墮椤戝懘鈥?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶濡わ絽鍟宥夋⒑缁嬪灝顒㈡い銊ユ嚇婵℃挳骞掗幋顓熷兊闂佹寧绻傞幊宥嗙珶閺囥垺鈷掑ù锝堟閵嗗﹪鏌￠崒娆戠獢鐎规洘鍨块獮姗€骞囨担鐟扮槣闂備線娼ч悧鍡椢涘Δ鍛敜濠电姴娲﹂悡鏇㈡倵閿濆骸浜濈€规洖鐭傞弻锛勪沪鐠囨祴鍋撻弴鐘插疾闂佽崵濮村ú鈺冧焊濞嗘劗顩烽柟闂寸劍閳锋垿鎮归崶锝傚亾閾忣偆浜繝鐢靛仜閻ㄧ兘鍩€椤掆偓绾绢參寮抽敃鍌涚厽闁哄啫鍊甸幏锟犳煛娴ｅ摜校缂佺粯鐩獮瀣倷閼碱剛鎳栭梺璇查閻忔岸鎮ч悩宸綎缂備焦蓱婵挳鏌涘☉姗堝伐闁哄棗鐗婄换娑㈠箻鐎靛壊鏆″銈冨妼閿曘倝鎮惧畡閭︾叆闁告劧绲鹃～宥夋⒑闂堟稓绠氶柍褜鍓欓崢鏍偨婵犳碍鈷掑ù锝囩摂濞兼劕顭块悷鐗堫棡闁哄懓娉涜灃闁告侗鍠栨禍妤€鈹戦悙鏉戠仧闁搞劍妞介幃锟犲即閻旂繝绨婚梺閫涘嵆濞佳勭濠婂厾鐟邦煥閸曨厾鐓€濡炪値鍙€閸庡篓娓氣偓閺屾盯鎮╅幇浣圭暭闂佺懓绠嶉崹褰掑煘閹寸姭鍋撻敐搴濈敖闁?
// DoH/nginx 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰紦閻㈢粯淇婇悙顏勨偓鏍偋濠婂牆纾绘繛鎴欏灩閸ㄥ倿鏌涘畝鈧崑鐐烘偂閻斿吋鐓忛煫鍥ь儏閻掓椽鏌ｉ幒鎴濃挃缂佽鲸甯￠崺鈧い鎺戝绾惧吋绻涢幋鐐垫噧鐎殿喖鐏濋埞鎴︽晬閸曨偂鍝楀銈嗘肠閸曨収娲告俊銈忕到閸燁垶宕愰悽鍛婂仭婵炲棗绻愰顏嗙磼閳ь剟鍩€椤掑嫭鈷戠紓浣诡焽婢ь亪鏌曢崼鐔稿€愰柣娑卞櫍瀹曟﹢顢欓懖鈺佸箰闂備胶顭堥張顒傜矙閹惧墎鐭撻悹杞拌濞撳鏌曢崼婵囶棞濠殿喖鍊块弻娑欑箾閸喒鍋撻弴鐐茬カ闁荤喐绮庢晶妤冩暜濡ゅ懎鐓曢柟鐑樺殮瑜版帗鏅查柛娑卞枛閺嗗牓姊洪崫銉ユ灁闁稿鍋ゆ俊鐢稿礋椤栨鈺呮煏婢舵稓鐣辨繛鍜冪秮閺岋綁骞橀崡鐐╁亾濠靛钃熼柡鍥╁枔缁♀偓闂婎偄娲﹂幐鎯ｉ敐鍛斀闁绘劘灏欐晶鏃傜磼椤旂晫鎳囩€殿喛顕ч埥澶愬閻樻牓鍔嶉妵鍕籍閸屾艾浠樺銈庡亐閺呯姴顕ｉ崼鏇炵厸闁稿本绮庨悾铏圭磽娴ｈ櫣甯涚紒瀣笒椤洩绠涘☉妯碱槶閻熸粌绉瑰畷鍝勭暆閸曨兘鎷虹紒缁㈠幖閹冲氦顣跨紓鍌欑贰閸犳骞戦崶褏鏆︽繛宸簼閸婇攱绻涢弶鎴剱妞ゎ偄绉撮埞鎴︽倷閸欏妫￠梺鎼炲妼缂嶅﹪骞嗘笟鈧畷濂稿Ψ閿曗偓娴狀厼鈹戦悩璇у伐闁哥喓澧楅弲銉╂煟鎼淬値娼愭繛璇х畵瀹曞綊鎳滈崗鍝ョ畾闂佸綊妫跨粈渚€鎮″☉妯忓綊鏁愰崨顔藉創婵犳鍣梽鍕┍婵犲洦鍊锋い蹇撳閸嬫捇寮介鐐茬€梺缁樻尭缁ㄥ爼寮搁弽顓熺厸闁搞儯鍎遍悘鈺冪磼閳ь剚寰勭仦绋夸壕妤犵偛鐏濋崝姘亜閿斿灝宓嗛柟顕嗙節瀵挳濮€閿涘嫬甯惧┑鐘垫暩婵鎹㈤幒妤€绀嗛柡澶婄氨閺€浠嬫煥濞戞ê顏繛鍛礈缁辨帗娼忛妸銉﹁癁閻庤娲樼敮鎺楀煡婢跺á鐔兼嚃閳哄倸娈愰梻鍌氬€峰ù鍥敋閺嶎厼鍨傞柛妤冨€ｅ☉銏犵闁冲搫鍊稿鍧楁⒑闁偛鑻晶鎾煛鐏炵晫效濠碉紕鍏樻俊鐤檪缂佽京鍋炵换婵嬪閿濆棛銆愰柣搴㈢濠㈡﹢锝炶箛鏇犵＜婵☆垵顕ч鎾绘⒑閸涘﹦鈽夐柨鏇樺€濆鎶藉醇閵夛腹鎷洪梺鍛婄☉閿曘倝鎮橀妷褏纾界€广儱鎷戝銉╂煟閿濆洦鐒块柕鍥ㄥ姍楠炴帡骞橀崗鍛秿闂傚倷绀佹竟濠囧磻閸℃稑绐楅幖绮规閸ゆ洟鏌涜椤ㄥ棝鍩涢幋锔界厱闁归偊鍓欑痪褔鏌熼姘卞ⅵ闁哄瞼鍠栧畷姗€鎳犻鍌ゅ晪闁诲氦顫夊ú婊堝极婵犳艾鏄ラ柍褜鍓氶妵鍕箳閹存繍浠鹃梺绋匡功閺佸寮诲☉婊呯杸闁瑰灝鍟瑧濠电偛鐡ㄧ划鎾剁不閺嵮屾綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顧侀柛鐘愁殜婵＄敻宕熼姘棟闁荤姵浜介崝搴敊婢跺ň鏀介柣鎰版涧鐢墎绱掔紒妯曟垶绌辨繝鍥х濞达綀鍊介妸褎鍠愰煫鍥ㄧ⊕閸嬫ɑ銇勯弬璺ㄦ癁闁衡偓娴犲鐓熼柟閭﹀墰閹界娀鎮樿箛搴″祮闁哄被鍊濋幃鐑藉级鎼存挻瀵栭梻浣稿悑濡炶姤绂嶉崼鏇炶摕闁糕剝顨忛崥瀣煕濞戝崬鐏茬紒閬嶄憾濮婄粯鎷呯粵瀣秷閻庤娲橀敃銏ゃ€佸鎰佹Ь婵犮垼顫夊ú鐔风暦濡ゅ懏鍤冮柍鍝勫暟濡插洭姊婚崒姘偓鎼佹偋婵犲嫮鐭欓柟鎯х摠濞呯娀鏌曞娑㈩暒缁ㄥ姊洪崫鍕殜闁稿鎹囬弻娑㈠籍閳ь剟鎮烽妷鈺嬬稏闊洦娲滅壕鍏间繆椤栨繍鍤欐い搴㈡崌濮婃椽鎳￠妶鍛€炬繝銏㈡嚀濡稓鍒掓繝鍥ㄥ€婚柤鎭掑劗閹锋椽姊虹粙璺ㄧ闁告艾顑夋俊鐢告偄閸忚偐鍘遍梺闈涱焾閸斿宕ラ銈囩＜妞ゆ梻鈷堥崕鏃€顨ラ悙鏉戞诞妤犵偛顑夊顕€鍩€椤掑嫸缍栫€广儱鎳夐弨浠嬫煟閹邦剙绾фい銉у仱閺屾盯濡歌閺嬫稓鈧鍠楅幃鍌氼嚕娴犲鏁囬柣鏂垮级鐎氬ジ姊婚崒姘偓鎼佹偋婵犲嫮鐭欓柟鎯х摠濞呯娀鏌￠崶銉ョ仾闁稿﹤鐏氶幈銊ノ熼悡搴′粯閻庢鍣崰妤呭Φ閸曨垰顫呴柍鈺佸暟椤︿即姊洪崷顓х劸闁挎洏鍎崇划鈺呮偄鐏忎焦鏂€闂佸壊鍋嗛崰搴♀枔鐏炵瓔娓婚柕鍫濋楠炴垿鏌涢弮鈧崹鐢告偩瀹勯偊娼ㄩ柍褜鍓熼悰顕€宕卞鍏夹╁┑鐘殿暯婵秹宕ㄩ婊愮闯?
//   1) 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴濐潟閳ь剙鍊块幐濠冪珶閳哄绉€规洏鍔戝鍫曞箣閻欏懐骞㈤梻鍌欑窔閳ь剛鍋涢懟顖涙櫠閹绢喗鐓涢悘鐐插⒔閵嗘帡鏌嶈閸撱劎绱為崱娑樼；闁告侗鍘鹃弳锔锯偓鍏夊亾闁告洦鍓涢崢?5444(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柤纰卞墻閸炲爼姊绘担鍛婂暈缂佸搫娼″畷鏇㈠箮閼恒儱鍓归梺鐟板⒔缁垶鎮￠弴鐔翠簻闁规澘澧庣粙鑽ょ磼閳ь剟宕橀鐣屽幈? / 5454(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰级閻ゅ嫰姊绘担绛嬪殐闁搞劌銈搁幃姗€骞囬妯规睏闂佸湱鍎ら幐鎼佸焵椤掑啫鍚圭紒杈ㄥ笒閳藉顫滈崱妤€澹夐梻? / 443(nginx) 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏犖ч柛灞剧煯婢规洖鈹戦鐭亜鐣烽鍕偍閻庣數纭堕崑鎾舵喆閸曨剛顦銈庡亜椤﹂亶鍩€椤掑嫭娑ч柕鍫㈩焾椤曪綁宕奸弴鐐殿吅闂佺粯锚閻忔艾袙鎼淬垻绡€闁汇垽娼ф禒锕傛煙缁嬫鐓肩€规洘妞藉畷姗€顢欓懖鈺嬬幢闂備胶鎳撴晶鐣屽垝椤栫偞鍋傛繛鎴欏灪閸婂爼鏌ｉ幇顒備粵婵炲懏娲栭…鑳檪缂佺姵鎹囧濠氭偄閾忓湱鐓撻梺鍛婄墤閸撴繈寮ィ鍐╁€甸柛顭戝亝缁舵煡鎮楀鐓庢珝闁糕斁鍋撳銈嗗笒閿曪妇绮旈悽鍛婄厱婵☆垱浜介崑銏☆殽閻愬弶顥㈢€规洖銈告俊鐑芥晜鐟欏嫬顏?
//   2) 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敃鈧灒濞撴凹鍨遍埢鎾斥攽閻愯尙鎽犻柣妤冨█瀵寮撮悢铏诡啎闂佺粯鍔﹂崜姘舵偟閺冨倻纾藉ù锝呮惈鏍￠梺缁樻惈缁绘繂顕ｆ繝姘嵆闁挎稑瀚弶鎼佹煟鎼搭垳绉靛ù婊勭墵瀹曟垿骞樼拠鎻掑祮闂佺粯锚閸氣偓缂佹顦埞鎴︽倷閺夋垹浠ч梺鎼炲妼椤嘲鐣烽敐澶婄缂備焦顭囬崢閬嶆⒑闂堟侗妲归柛銊ф暩閳ь剚鍑归崢濂告偩閻戣棄绀冪€瑰壊鍠楃€靛矂姊洪棃娑氬闁硅櫕鍔楃划濠氬箥椤旂懓浜鹃悷娆忓婢跺嫰鏌涢幘璺烘灈鐎规洘妞芥俊鐑芥晝閳ь剛娆㈤悙鐑樼厵闂侇叏绠戞晶鐗堛亜閺冣偓閹瑰洤顫忓ú顏勫窛濠电姴鍊搁ˉ鍫ユ⒑閸涘﹣绶遍柛銊╀憾瀵煡濡搁妷銏℃杸闂佺粯蓱閸撴岸宕箛娑欑厱闁绘ɑ鍓氬▓娆戞喐閹殿喖鈻堟慨濠冩そ瀹曘劍绻濋崘顏勫汲闂傚倷绀佹惔婊呯礊娓氣偓閹即顢氶埀顒勭嵁鐎ｎ喗鏅濋柍褜鍓涚划鍫⑩偓锝庡亖娴滄粓鏌″鍐ㄥ闁汇劍鍨堕妵鍕箻閾忣偅鍎撻梺瀹狀潐閸ㄥ潡骞冮埡鍐╁珰鐟滃秵瀵奸幇鐗堚拺闁荤喐婢樺Σ缁樸亜閹存繃顥犻柍褜鍓熷褔濡剁粙璺ㄦ殾闁诡垶鍋婂Σ鐑芥⒑鐞涒€充壕?sniroute.cmd up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙８闁逞屽厸閻掞妇鎹㈠┑瀣＜婵°倓鑳舵禍娆愮節閻㈤潧浠﹂柛顭戝灦瀹曠懓煤椤忓嫮鏌у┑鐘绘涧椤戝棝鎮¤箛娑欑厱妞ゆ劧绲跨粻銉︿繆閹绘帞绉洪柡灞剧〒閳ь剨鎬ラ崟顒傚綃闂備礁鎼惌澶屽緤閸婄喓浜介梻浣瑰劤缁绘帒鈻嶉姀銏☆潟闁绘劕妯婂〒濠氭煏閸繂鈧懓鈻嶆繝鍥ㄧ厸闁逞屽墴閺屽棗顓奸崨鍌︾畵閺屾盯寮撮妸銉т哗缂備胶濮寸壕顓㈠焵椤掆偓缁犲秹宕曢崡鐏诲綊宕掑В纭风秮椤㈡瑧鍠婃潏銊ョ槣闂備線娼ч悧鍡涘箠鎼搭煈鏁嗛柕蹇嬪€栭悡鐔兼煟閺傚灝绾ф繛鍛攻椤ㄣ儵鎮欓幓鎺撴濠电偞鍨归弫璇茬暦椤忓懏濯撮柛蹇擃槹閻庢悂姊婚崒姘偓宄懊归崶顒€违闁逞屽墴閺屾稓鈧綆鍋呭畷宀勬煛鐏炲墽鈽夐摶鏍煕閹扳晛濡煎鐟邦樀濮?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙５闁逞屽墾缁犳挸鐣锋總绋款潊闁炽儱鍟跨花銉╂⒒娴ｇ顥忛柛瀣╃窔瀹曟洘娼忛埡鍐劶婵犮垼鍩栭崝鏍偂濞嗘挸绾ч柛顐ｇ箓閳锋棃鏌ｉ敐鍫滃惈闁汇儺浜獮鍡氼槼闁稿鍎甸弻鐔碱敊缁涘鐤侀梺璇″枙缁瑥鐣烽妸锔剧瘈闁告劦浜滅粈渚€姊婚崒娆掑厡缂侇噮鍨抽弫顕€骞掑Δ浣规珖濡炪倕绻愮€氱兘宕甸弴銏＄厱妞ゆ劧绲剧粈鈧梻浣稿船濞诧箓骞堥妸銉建闁糕剝顨呯粻娲⒑缂佹绠栨俊顐㈠暣楠炲啫螖閸愨晛鏋傞梺鍛婃处閸撴盯藝閵娿儮鏀介柣鎰硾閻ㄦ椽鏌涢悩宕囧ⅹ妞ゆ洩绲块幏鐘裁圭€ｎ偒娼旀繝娈垮枟椤ㄥ懎螞濞嗘挸纾介梻鍫熶緱濞撳鏌曢崼婵囶棞缂佹甯￠幃妤€顫濋崡鐐插箣闂佺硶鏂侀崑鎾愁渻閵堝棗绗傜紒鍨涒偓鏂ユ灁婵犲﹤鐗婇悡娑㈡倶閻愰鍤欓柍褜鍓氱换鍌烆敋閿濆閱囬柡鍥╁仧椤撳搫顪冮妶鍡欏缂佽鍊块獮鍡涘醇閻斿墎绠氶梺闈涚墕濞层倕鏆╅梻浣侯焾椤戝洭宕戦妶澶屽祦闁硅揪绠戦悙濠冦亜閹烘垵鈧摜绮婇敃鍌涒拺闁硅偐鍋涢崝鎾煕閹垮嫮鐣电€殿噮鍋呯换婵嬪炊瑜忔鍥⒑缂佹ê濮堟繛鍏肩懇钘熼悗锝庝憾濞撳鎮峰▎蹇擃仼闁崇粯娲橀幈銊︾節閸曨厼绗￠梺鐟板槻閹虫﹢骞冮悜鑺ョ劷闁挎洍鍋撴繛鍫熺箘缁辨捇宕掑▎鎴М濡炪倖鍨靛Λ婵嗙暦瑜版帒閿ゆ俊銈傚亾缂佺姵宀搁幃姗€鎮欓幓鎺嗘寖闂佽崵鍠庣紞濠囧蓟瀹ュ牜妾ㄩ梺鍛婃尵閸犳牗淇婄€涙ɑ濯寸紒顖涙礃閻庡姊虹憴鍕姢缁剧虎鍙冨畷婵嬫晝閳ь剟鈥旈崘顔嘉ч柛鈩冾殘閻熴劑鏌ｆ惔銏犲毈闁告瑥鍟撮獮鍐晸閻樺啿浜滈梺纭呭亹閸嬫鑺辩拠宸富闁靛牆妫楁慨锕傛煕鐎ｎ亜顏柡鍛埣椤㈡瑧浜搁弽銊︽澑闂備胶鍋ㄩ崕瀵镐焊濞嗘挻鍎楁繛鍡樻尰閻撴盯鎮橀悙鎻掆挃婵炲弶鎸抽弻娑㈠煛鐎ｎ剛鐦堥悗瑙勬礃鐢帡鈥﹂妸鈺佺妞ゆ劑鍨绘禒顓㈡⒒閸屾艾鈧嘲霉閸ヮ剦鏁嬮柡宥庡幖閸ㄥ倸霉閻撳海鎽犻柛搴㈩殜閺屾稓浠﹂崜褉妫╃紓浣筋嚙濡繈寮婚敐澶婄闁挎繂妫Λ鍕⒑缂佹ɑ灏伴柣鈺婂灦瀵顓奸崼顐ｎ€囬梻浣告啞閹歌顫濋妸褍鍨?闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃秹锝炲┑瀣櫇闁稿矉濡囩粙蹇旂節閵忥絽鐓愰柛鏃€鐗犲畷鎴﹀Χ婢跺鍘搁梺鎼炲劗閺呮稑鐨梻浣虹帛鐢帡鏁冮妷褎宕叉繛鎴欏灩楠炪垺淇婇婵囶仩濞寸姵鎸冲娲箹閻愭彃顬夌紓浣筋嚙閻楁挸顕ｆ繝姘╅柍鍝勫€告禍婊堟⒑閸涘﹦绠撻悗姘嚇婵偓闁靛繈鍨婚敍婊堟⒑闁偛鑻晶瀵糕偓瑙勬礀瀹曨剟鍩ユ径濞炬瀻闁瑰瓨绻傜粻鐐烘⒒閸屾瑧绐旀繛浣冲泚鍥ㄥ閺夋垵鐎梺姹囧灮椤ｄ粙宕戦幘鎰佹僵闁绘挸瀛╅悵婵嬫煢濡崵绠為柡灞炬礃瀵板嫰宕卞Ο鑽ゅ絾闂備焦濞婇弨閬嶅垂閸ф钃熸繛鎴炃氶弨浠嬫煕閳╁喚娈㈠ù灏栧亾婵犵數濮幏鍐炊閿旇棄鏋ら梻浣告惈閺堫剟鎯勯鐐靛祦婵☆垰鐨烽崑鎾绘濞戞瑥鏆堥梺闈╂€ラ崶銊㈡嫼?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫇闁逞屽墰婢规洟鎮烽幊濠傜秺閹晛顔忛鐓庡Η缂傚倸鍊哥粔鐢告偋閺団懇鈧箓宕稿Δ浣告疂闂傚倸鐗婄粙鎴︼綖閸ヮ剚鈷?闂?
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
        // up 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佲枙闁绘帟濮ょ换娑㈠幢濡纰嶉柣鐔哥懕缁犳捇骞冨Δ鍛櫜閹肩补鍓濋悘宥夋⒑閹肩偛鈧洟鎳熼婵堜簷闂備焦瀵х换鍌炲箠鎼淬劌姹叉繛鍡樻尰閻撴洟鎮橀悙棰濆殭濠碉紕鏅槐鎺旂磼濡吋鍒涘Δ鐘靛仜椤戝懘鍩㈡惔銈囩杸闁哄洦纰嶉崑鍛磽?闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊瑜忛弳锕傛煕椤垵浜濋柛娆忕箳閳ь剙绠嶉崕閬嵥囬鐐村€峰┑鐘叉处閻撳繐鈹戦悩鑼婵＄虎鍠楃换娑㈠醇濠靛洤鏆堢紓浣虹帛缁诲牓骞冩禒瀣棃婵炵鍩栭惄顖炲蓟濞戞埃鍋撻敐搴′簼鐎规洖鐭傞弻锛勪沪閸撗勫垱婵犵绱曢崗姗€銆佸▎鎾村亗閹兼惌鍠楁禍銈囩磽閸屾艾鈧鎷嬮弻銉ョ；闁圭偓鍓氶悢鍡涙煠閹间焦娑у┑顔肩墦閺岋綁骞橀崘鍙夋闂佸搫鏈ú妯侯嚗閸曨垰閱囨繝闈涙椤捇姊绘担鍛婃儓婵☆偅绻冪粋宥夊醇閺囩偠鎽曢梺璺ㄥ枔婵潙顔忓┑鍥ヤ簻闁哄啫鍊告禍楣冩煕閿濆嫬宓嗘慨濠冩そ瀹曨偊宕熼浣瑰缂傚倷绀侀鍡涘箲閸ヮ剙鏄ラ柍褜鍓氶妵鍕箳閹搭垰濮涚紓浣割槺閺佸寮诲☉姘ｅ亾閿濆簼鎲炬俊鎻掓贡缁辨帡寮崒姘€婚柧浼欑到閳规垿宕掑┑鍥舵缂傚倸绉撮敃顏堢嵁閸愵喖鐓涢柛娑卞枛濞堛倕顪冮妶鍡楃瑐闁绘帪绠撳畷浼村冀椤撶啿鎷绘繛杈剧悼閻℃棃宕甸崘顔界厱闁靛鍎遍懜褰掓懚閻愮儤鐓曢柟浼存涧閺嬫盯鏌﹂崘顏勬灈闁哄本娲樼换娑滎槷闁稿鎹囬弻娑氣偓锝庡亝瀹曞矂鏌℃担鐟板鐎规洏鍔戦、娑樷枎閹达腹鏀洪梻鍌氬€烽悞锔界箾婵犲洤缁╅柛妤冧紳濞差亝鍋勫┑鍌氼槹缂嶅骸鈹戦悙鍙夆枙濞存粍绮嶇€靛ジ鎮╃紒妯煎幈闂佸搫娲㈤崝灞解枍濡崵绠鹃柣鎾虫捣閸╋絾鎱ㄦ繝鍛仩缂侇喗鐟╅獮鎰償閳╁啯鏆橀梺璇叉唉椤煤閻斿鐒界憸宥呯暤閸曨垱鈷戠憸鐗堝笒娴滀即鏌涜箛鏃撹€跨€殿喓鍔嶇粋鎺斺偓锝庡亞閸樺崬顪冮妶鍡楀濠殿喗鎸抽幃姗€鍩￠崘锝呬壕閻熸瑥瀚壕鎼佹煕鎼淬劋鎲鹃柍銉︽瀹曟﹢顢欓崲澹洦鐓曢柍鈺佸暟閹冲啯銇勯搴℃处閳锋垿鏌ゅù瀣珕闁搞倐鍋撳┑鐘媰閸涱喗鐝┑鈥冲级閸旀瑩骞冨▎鎾充紶闁告洦鍙庨崯瀣⒒娴ｅ憡鎯堥柛濠傜埣瀹曟洟寮婚妷顖滅◤閻庡箍鍎遍ˇ浼村煕閹达附鍋℃繛鍡楃箰椤忊晠鏌涢弬璇测偓妤呮儉椤忓棛纾奸柕蹇曞Т缁犳椽姊虹紒姗嗘當閻庢稈鏅濋幑銏犫攽閸繃鐎冲┑鈽嗗灠閹碱偊鍩涙径鎰仭婵犲﹤瀚欢鏌ユ煕閻斿憡灏﹂柣娑卞枛铻栭柍褜鍓熼垾锕傚Ω閳轰礁绐涘銈嗘尵閸嬬喐绂嶆导瀛樷拻濞达絿顭堥弳閬嶆煙绾板崬浜扮€规洦鍨堕幃鈺冩嫚閼碱剨绱遍梻浣烘嚀婢х晫鍒掗鐐茬柧闁哄被鍎辩粻褰掓倵濞戞瑯鐒界紒鐘筹耿閺岋綁鍩℃笟鈧崣鍕叏婵犲懏顏犵紒杈ㄥ笒铻ｉ悹鍥ㄧ叀閻庤櫣绱撻崒娆愮グ妞ゆ泦鍥ㄥ亱闁圭偓鍓氶崵鏇㈡偣閸ャ劎銈存俊鎻掔墛缁绘繃绻濋崒娑樻闂佸搫顦伴幃鍌炲蓟閿濆棙鍎熸い鏍ㄧ矌鏍″┑鐐茬摠缁秶鍒掗幘缁樻櫜闁绘劕鎼悙濠冦亜閹哄秷顔夐柟鐤缁辨挻鎷呴崜鎻掑壈濠电偟銆嬬换婵嬨€侀幘婢勬梹鎷呴崗鍝ョ泿闂備礁鎼ú銏ゅ礉瀹€鍕祦婵°倕鍟扮壕濂告煕鐏炵偓鐨戦弫鍫ユ倵鐟欏嫭纾搁柛鏂跨Ф閹广垹鈹戠€ｎ亞顦ㄥ銈呯箰閺屽﹪濡舵径瀣ф嫽婵炶揪绲介幗婊堝几閸愨晙绻嗘俊鐐靛帶婵¤法绱掗鑲╁闁瑰嘲鎳樺畷顐﹀礋閸偒妫ラ梻鍌欒兌椤宕熼崹顐ゆ殾闂備焦濞婇弨閬嶅垂閸噮娼栧┑鐘宠壘闁卞洭鏌ｉ弮鍥モ偓鈧┑顔兼搐椤啴濡堕崘銊ュ缂備緡鍠楅幑鍥ь嚕鐠囨祴妲堟繛鍡樺灩閻﹀牊绻涙潏鍓хК婵炲拑缍侀幃锟犲箻缂佹ǚ鎷洪柣鐘叉礌閳ь剝娅曢悘鍫濃攽閻愯尙姣為柍褜鍓欑壕顓㈠汲閿曞倹鐓曟俊銈呭暙娴滃綊鏌＄€ｎ亪鍙勯柡宀€鍠栭弻鍥晝閳ь剚鏅堕幍顔藉枑闁绘鐗婄亸锔芥叏婵犲嫮甯涚紒妤冨枛閸┾偓妞ゆ巻鍋撴い顓炴穿椤︽挳鏌熼獮鍨伈鐎规洖宕埥澶娢熺喊杈ㄐゅ┑鐘垫暩婵炩偓婵炰匠鍏犳椽濡搁埞搴撳亾閸涘瓨鐓ラ悗锝傛櫇缁犳艾顪冮妶鍡欏闁荤喆鍔戦、妤呮偄閸忚偐鍘?30 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛闂佽姤蓱缁诲牓寮婚悢灏佹灁闁割煈鍠楅悘宥夋⒑閸濆嫭顥炵紒顔肩Ч婵＄敻宕熼鍓ф澑闂佸湱鍋撻幆灞轿涢垾鎰佹富?
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋娆忕労闁轰礁顑嗛妵鍕箻鐠虹儤鐎鹃梺鍛婄懃缁绘﹢骞冨Δ鍛棃婵炴垶鐟﹂崰鎰版⒑濞茶骞楅柟鎼佺畺閸┾偓妞ゆ帒鍠氬鎰箾閸欏澧悡銈夋煏婵炑冩噸缁楀姊洪崫鍕偓褰掝敄濞嗘挻鍋勯柛鈩冪⊕閸嬶綁鏌涘┑鍡楊仾闁诡垰鐗婇妵鍕Ω閿旂偓鍣伴梺鍝勭焿缂嶄線鐛幒妤€绠ｆい鏍ㄧ箓閹牓鏌ｆ惔銏╁晱闁哥姵鐗犻幃銉╂偂鎼达絾娈鹃梺缁樻尭鐎垫帒顭囬埡鍌樹簻闁规崘娉涢弸鏃傛喐閺夋寧鍤囬柡宀嬬稻閹棃濡舵惔銏㈢Х婵犵數鍋涘鍫曟晪闁绘挶鍊濋弻锝夊籍閸屾艾浠樼紓浣哄У閻楁绌辨繝鍥舵晬婵犻潧娲ㄩ妴濠囨⒑濮瑰洤鈧劙宕戦幘缁樷拻濞达絿鐡斿鎰版煕鎼淬垹鈻曢柟顔兼健瀹曞爼顢楅埀顒勫及閵夆晜鐓熼柟閭﹀枛閸斿鏌嶉柨瀣伌闁哄本绋戦埞鎴﹀礋椤愩垹顫撻梻浣告惈濡繃绂嶉崼鏇炶摕婵炴垶鍩冮崑鎾绘晲鎼粹€茬凹閻庤娲栭惌鍌炲蓟閳ュ磭鏆嗛悗锝庡墰琚﹂梻浣筋嚃閸犳帡寮查悩鍙傛盯宕橀妸銏☆潔濠碘槅鍨拃锕傛倵闁秵鈷掗柛灞剧懅閸斿秹鏌熼鑲╁煟鐎规洘绻傞悾婵嬪焵椤掍胶鐝堕柡鍥ュ焺閺佸洭鏌ｅΟ璇茬祷闁诲繐锕娲礃閸欏鍎撻梺鐟板暱濞寸兘鎯€椤忓浂妯勯梺鍝勭灱閸犳捇鍩€椤掍胶鈯曢拑閬嶆煃闁垮濮嶉柡?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶濡わ絽鍟宥夋⒑缁嬫鍎忔い鎴濇嚇閸╃偤骞嬮敂钘変汗闂佸湱绮敮濠囧窗婵犲洦鈷戦柛蹇撳悑缁跺弶绻涚涵椋庣瘈鐎殿喛顕ч埥澶愬閻樻牓鍔嶉妵鍕籍閸屾艾浠樺銈庡亐閺呮繄妲愰幘瀛樺闁圭粯甯婃竟鏇炩攽閻橆喖鐏遍柛鈺傜墵瀹曟繈寮介鐔蜂簵闂佸憡鍔﹂崰妤呮偂韫囨搩鐔嗛悹楦挎婢ф洟鏌涢弮鈧畝鎼佸蓟閿熺姴骞㈡繛鎴烆殕閸犳岸姊虹拠鈥虫灕妞ゎ偄顦甸獮蹇涘川閺夋垶宓嶅銈嗘尵閸嬫稓绱掗埡鍛拻濞达絽鎲￠幆鍫ユ偠濮樼厧浜扮€规洘鍔曡灃闁告侗鍘鹃崝锕€顪冮妶鍡楀潑闁稿鎸荤换婵嬪焵椤掍焦缍囬柕濠忕畱瀵潡姊哄Ч鍥х仾婵炵厧鏈鍕偓锝庝簽缁愮偤姊鸿ぐ鎺戜喊闁告﹢绠栧畷銏ゆ焼瀹ュ棌鎷洪梻鍌氱墛娑撶懓鈽夐姀鐘殿槰闂佺粯鍔﹂崜姘舵儊閺冨倻纾介柛灞捐壘閳ь剙鍢查湁闁搞儜灞剧闂侀潧臎閸愩劎浜版繝鐢靛仜濡鎹㈤幒妤€姹查柨鏇炲€归悡娆撴煙濞堝灝鏋涙い锝呫偢閺屾稓鈧綆浜濋ˉ婊堟婢舵劖鐓ユ繝闈涙缁佲晠鏌￠埀顒佸鐎涙鍘搁梺鍛婄☉濞层倝宕悩璇茬哗濞寸姴顑嗛悡鐔兼煙闁箑骞楃紓宥嗗灴閺岋綀绠涢妷褏鏆ら梺鍝勭灱閸犳牕鐣烽妸锔剧瘈闁告劑鍔夐崑鎾诲箛閻楀牏鍘告繛杈剧悼椤牓寮抽敐澶嬬厸濞达絿鎳撴慨宥団偓瑙勬礀閻栧ジ宕洪埄鍐╁婵繂鐫楅弴銏♀拻闁稿本鐟чˇ锕傛煙绾板崬浜伴挊婵喢归崗鍏肩稇闁搞劌鍊块弻锝夊籍閸屾艾浠橀梺鍛婂灩婵炩偓妤犵偞鐗曡彁妞ゆ巻鍋撳┑鈥茬矙閺岋綁骞橀崡鐐插Е闂佸搫鐭夌紞浣割嚕椤掑嫬绠伴幖娣灩閸撶敻鏌ｆ惔銏⑩姇缂佽鍊块崺鐐哄箣閿旇棄鈧兘鏌涘▎蹇ｆЦ婵炲拑绻濆娲箹閻愭彃顬嗙紓渚囧枟閻熴儵顢氶敐鍥ㄥ珰婵炴潙顑嗛～宥呪攽椤旂瓔娈旀俊顐ｇ懇瀹曟捇鎮介崨濞炬嫼缂傚倷鐒﹁摫閻忓浚鍘界换娑㈠箻椤旇姤鏆犵紓浣稿€圭敮锟犮€佸鈧幃銏ゆ惞閸︻厽顫岄梻鍌欑閸熷潡鎯冨┑瀣；闁规儳鐏堥崑鎾荤嵁閸喖濮庡┑鐐茬湴閸旀垵顕ｉ幎钘壩у璺侯儏閳ь剙鐏氱换娑㈠醇濠靛牅铏庡┑鐐叉噺閿曘垽寮诲☉銏犵閻犺櫣鍎ら悘鍫ユ⒑闂堟稒顥欑紒鈧笟鈧敐鐐差煥閸繄鍔﹀銈嗗笂濡炴帞鎹㈤崱妯诲弿婵犻潧妫涢悞鐐繆閸欏鍔ゆい顏勫暣婵℃儼绠涢幘鑸敌掗梻浣侯焾椤戝懘鏁冮妶澶涚稏?
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
        // up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴鐐测偓褰掑磿閹寸姵鍠愰柣妤€鐗嗙粭鎺旂磼閳ь剚寰勭仦绋夸壕闁稿繐顦禍楣冩⒑闁偛鑻晶鎾煕閳规儳浜炬俊鐐€栫敮濠勭矆娓氣偓瀹曠敻顢楅崟顒傚幈濠电偛妫楃换鎰板汲閿濆鐓忛柛鈩冩礈椤︼箓鎽堕弽顓熺厓鐟滄粓宕滈悢鐓庣伋闁挎洖鍊告儫闂佸疇妗ㄧ粈浣虹玻濞戞﹩娓婚柕鍫濇椤ュ棗鈹戦鍝勨偓婵嬨€佸▎鎾崇倞妞ゆ帊璁查幏缁樼箾鏉堝墽鍒伴柟璇х節瀹曨垶鎮欓悽鐢碉紲闂佸憡绻傜€氼參宕戦妷褉鍋撶憴鍕婵炶尙鍠庨悾宄拔熸總钘夋贡閳ь剨缍嗛崑鍡樼濞嗘挻鈷掑〒姘ｅ亾婵炰匠鍛床闁割偁鍎辩壕褰掓煛瀹ュ骸骞楅柛瀣€圭换娑㈠箣閻戝洣绶甸梺缁樻煥濡繈骞冮悷鎳婃椽顢旈崱娆戠崶闂佽瀛╅惌顕€宕￠幎钘夎摕闁挎稑瀚ч崑鎾绘晲鎼存繄鐩庡銈嗗竾閸ㄤ粙寮婚悢鍝ョ懝闁割煈鍠栭‖鍫ユ煣娴兼瑧鎮肩紒杈ㄥ笧閳ь剨缍嗘禍鐐寸閵徛颁簻妞ゆ巻鍋撴い锕佷含濡叉劙骞樼€涙ê顎撶紓浣割儏缁ㄩ亶宕戦幘璇叉嵍妞ゆ挾鍠愬▓浼存⒑鐠恒劌鏋斿┑顔芥尦瀵憡绗熼埀顒勫蓟閻斿吋鍋嬮柛顐ゅ枔閸戯繝姊绘担绋胯埞婵炶濡囬幑銏犫槈閵忕姷顓洪梺缁樺姌鐏忔瑩鎮烽妸锔剧閻庢稒顭囬惌鎺旂磼閻樺磭澧电€殿喛顕ч埥澶愬閻樼數鏉搁梻鍌氬€搁悧濠勭矙閹烘鍊堕柛顐犲劜閻撶喖骞栧ǎ顒€鐏卞瑙勆戦妵鍕晝閸屾瑦鍠氶梺绯曟杹閸嬫挸顪冮妶鍡楀潑闁稿鎸剧槐鎺楁偐閼碱儷褏鈧娲樺ú鐔煎蓟閸℃鍚嬮柛娑卞灣閺嬪啴姊洪悷鏉挎倯闁诡垰鐭傞幃鈩冨緞瀹€鈧粈濠囨煛瀹ュ骸骞楅柣鎾崇箻閺屾盯顢曢敐鍥╃暫缂備讲鍋撶€光偓閸曨剛鍘遍柣搴秵閸嬪懎鐣峰畝鈧埀顒冾潐濞叉粓宕楀鈧妴浣肝熷▎鐐梻浣告啞濮婂綊骞冮崒鐐茬畺婵°倐鍋撻柍缁樻崌瀹曞綊顢欓悾灞肩矚闂傚倷鑳剁涵鍫曞疾濠婂牊鍋嬮柣妯垮皺閺嗭附鎱ㄥ鍡楀箻缂佲檧鍋撻梻浣规偠閸庮垶宕濆澶婄？妞ゅ繐鐗婇悡鐔兼煟閺傛寧鎲搁柟鍐插暣閹顫濋悡搴♀拫闂佽鍨伴崯鎾€侀弴銏℃櫆閻熸瑱绲剧€氬ジ姊绘担鍛婂暈缂佸鍨块弫鍐Χ婢跺鈧爼鏌ｉ弬鍨倯闁绘挻娲栭埞鎴︽偐閹绘帗鐏撳┑鐐茬墛缁捇寮婚敐鍛闁告鍋為悵婵囩節濞堝灝鏋撻柛瀣崌濮婃椽妫冨☉姘辩杽闂佹悶鍔岄…鐑藉极瀹ュ拋鐓ラ柛顐ゅ暱閹锋椽姊洪崨濠勭畵閻庢凹鍓熷鎶芥倷閻戞鍘遍梺闈浤涢崟顒佺槗闂備浇顕栭崰妤呮晝閵忋倕绠栭柍鍝勬噹閸ㄥ倹銇勯幇鈺佺仾闁诲酣绠栧缁樻媴閾忕懓绗″銈冨妼閹虫﹢骞冭缁犳盯骞樻担鑺ヮ仩闁逞屽墯缁嬫帡鏁冮埡浣叉灁濞寸姴顑嗛悡鐔兼煙闁箑骞楃紓宥嗗灦缁绘盯骞栭鐐寸亪闂佸疇顫夐崹鍧椼€佸▎鎾村仭濡绀侀ˉ姘舵⒒閸屾艾鈧兘鎮為敂閿亾缁楁稑鎳愰惌娆撴煙鐎电袥闁稿鎸搁～婵嬪Ψ閵夈儺娼庨柣搴ゎ潐濞叉﹢宕归崸妤€绠栨繛鍡樻尰閸婄粯淇婇鐐存暠閻庢艾銈稿缁樻媴閸涘﹤鏆堢紓浣割儐閸ㄥ潡寮崘顔芥櫇闁稿本姘ㄩ悿鍥⒑瑜版帒浜伴柛搴邯閹兘鏌囬敂鎯у汲闂備礁鎲℃笟妤呭储閸濆嫀锝夊箛閻楀牆鈧灚绻涢崼婵堜虎婵炲懏锕㈤弻娑氣偓锝庝悍閺€璇睬庨崶褝韬柟铏矒濡啫鈽夊杈╁祦婵犵數濮烽弫鍛婃叏鐎电硶鍋撳顐㈠祮闁诡喕鍗抽獮妯肩磼濡攱瀚藉┑鐐存尰閸╁啴宕戦幘瀵哥濞达絽鍟垮▔姘跺炊椤掑鏅ｉ梺闈涚箳婵兘鏁嶉悢鍏尖拻濞撴艾娲ゆ晶顔剧磼婢跺孩纭鹃柣锝呯仛缁楃喖鍩€椤掑嫬钃熼柣鏂挎啞缂嶅洭鏌涢幘妤€鎳忓▓鍝ョ磽閸屾瑨鍏屽┑顔碱嚟缁辩偞绻濋崶褏鐣哄┑掳鍊愰崑鎾绘煃閽樺妲搁柍璇茬Ч閹煎綊顢曢姀顫偅缂傚倷绶￠崰妤€顫濋妸鈺佺闁告洦鍨版儫閻熸粌绻橀弫宥咁煥閸喓鍘告繝銏ｆ硾閿曪附鏅堕幋锔界厱闁哄倹瀵ч幑锝囩磼椤旂⒈鐓肩€殿喕绮欓、鏇㈠Χ閸ラ搴婇梻浣筋嚙閸戠晫绱為崱妯碱洸閻犺桨缍嶉敐澶婂窛濠电偛銇樼花濠氭⒑閻熸澘鈷旈柛瀣ㄥ姂瀹曟垿濮€閳垛晛浜鹃悷娆忓缁€鈧梺闈涚墛閹倿濡存笟鈧鎾閳╁啯鐝抽梻浣规偠閸庮噣寮插☉銏㈠祦闁逞屽墴濮婂宕掑顑藉亾閹间礁纾瑰瀣椤愯姤鎱ㄥ鍡楀⒒闁绘帊绮欓弻銈嗘叏閹邦兘鍋撻弽顐熷亾濮橆剦鐓奸柡宀嬬秮瀵噣宕掑顑跨帛缂傚倷璁查崑鎾愁熆鐠哄搫顦柛瀣崌瀹曟寰勬繝浣割棜闂傚倷绀侀幉鈥趁洪敃鍌氱婵炲棙鍔楅々宄扳攽閸屾碍鍟為柍閿嬪灩缁辨挻鎷呴惂闀愮返闂佺粯甯為弲顐ゆ閹烘鏁嬮柛娑卞幘娴犳悂姊虹拠鈥虫珯缂佺粯绻傞悾鐑藉箳閹存梹鏁犻梺璇″瀻閸屾凹妫滃┑掳鍊楁慨鐑藉磻濞戙垺鍊舵繝闈涱儏缁€澶嬫叏濡灝鐓愰柣鎾寸懇閺屾盯骞囬姘辨缂備胶濮惧畷鐢稿焵?
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫆闁芥ê顦純鏇㈡⒒娴ｈ櫣銆婇柛鎾寸箞閹柉顦归柟顖氱焸楠炴绱掑Ο琛″亾閸偆绠鹃柟瀵稿剱娴煎棝鏌熸潏鍓х暠闁活厽顨婇悡顐﹀炊閵娧€濮囬梺缁樻尵閸犳牠寮婚敓鐘茬＜婵ê褰夐搹搴ㄦ⒑鐠団€虫灍闁搞劌娼″璇测槈濞嗘劕鍔呴梺鎸庣☉鐎氬懓顦归柡宀嬬磿娴狅箓宕滆閸掓盯鎮楀▓鍨灍闁规瓕娅曟穱濠囧箹娴ｈ倽銊╂煏婢诡垰鑻弲顏堟⒒娴ｇ瓔鍤欐慨姗堢畵閿濈偞寰勬繛鎺撴そ瀵粙顢橀悢铚傚嚱濠电娀娼ч崐濠氣€﹂崼銉﹀珔闁绘柨鎽滅粻楣冩煙鐎电鈧垵顫濈捄鍝勫殤闁瑰吋鐣崹鑽ょ不妤ｅ啯鐓曢柍鈺佸幘椤忓牊鍊堕柍鍝勫暞閸嬫牠鏌ㄩ弴鐐测偓褰掑煕閹烘垯鈧帒顫濋敐鍛婵犵數鍋犳竟鍫濈暦閻㈠灚顫曢柣鎰嚟閻熷綊鏌嶈閸撴繂螞閵忋倖鈷戦悷娆忓缁€鍐┿亜閺囧棗鎳愭稉宥嗐亜閺嶎偄浠﹂柣鎾跺枑娣囧﹪顢涘鍛闂侀潧娲ㄩ崰鏍蓟閳╁啯濯撮柛婵勫剾閿濆鐓欏〒姘仢婵″吋銇勯锝囩煉鐎规洖宕灃闁逞屽墴閿濈偤顢曢敐鍥╊啎闁诲海鏁搁…鍫㈡暜閸洘鐓熸俊銈勭劍鐏忣厽銇?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣濞嗗繒浠鹃梺绋垮閸ㄥ潡寮婚敍鍕勃閻犲洦褰冩慨搴ㄦ煕閵夛附灏︽慨濠呮閸栨牠寮撮悙娴嬫嫟婵＄偑鍊戦崝宀勬晝閵堝牏浜遍梻浣虹帛閸ㄥ吋鎱ㄩ妶澶婄厱?闂?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏犖ч柛灞剧煯婢规洖鈹戦鐭亜鐣烽鍕偍閻庣數纭堕崑鎾舵喆閸曨剛顦銈庡亜椤﹂亶鍩€椤掑嫭娑ч柕鍫㈩焾椤曪綁宕奸弴鐐殿吅闂佺粯锚閻忔艾袙鎼淬垻绡€闁汇垽娼ф禒锕傛煙缁嬫鐓肩€规洘妞藉畷姗€顢欓懖鈺嬬幢闂備胶鎳撴晶鐣屽垝椤栫偞鍋傛繛鍡樺姂娴滄粓鏌￠崒婵囩《濞寸姰鍨婚幃顔碱潩椤戣姤鏂€闂佺粯鍔欏褏鏁崜浣虹＜缂備焦锕懓璺ㄢ偓瑙勬礃濡炰粙寮幘缁樺亹闁肩⒈鍓ㄧ槐鍙夌節閻㈤潧浠﹂柛銊ョ埣閹兘顢涢悙鑼缎曢梺绋跨灱閸嬬偤鍩涢幒妤佺厱閻忕偛澧介幊鍛亜閿斿ジ妾紒缁樼箞閸┾偓妞ゆ帒瀚悞鑲┾偓瑙勬尪閸庨亶藝閻㈠摜宓侀柛銉墻閺佸嫰姊哄▎鎯х仩濞存粓绠栭弻鐔兼倻濮楀棙鐣堕梺鍦攰閸╂牠濡甸崟顖氱閻庣數纭跺Σ鍫熺箾鐎涙鐭掔紒鐘崇墪椤繑绻濆顒勫敹闂佺粯姊规禍浠嬪焵椤掍礁娴柡宀嬬秮楠炴帡骞嬮悙鏉戞瀾闂備線鈧偛鑻晶鍙夈亜椤愩埄妲搁悡銈嗕繆椤栨艾鎮戠€规洘鐓￠弻娑㈩敃閵堝懏鐎虹紓浣筋嚙濡繈寮婚悢鍏尖拻閻庨潧澹婂Σ顕€鏌涢悜鍡楃仸婵﹥妞介獮鎰償閵忣澁绱╅梻浣侯焾椤戝棝骞戦崶顒€钃熸繛鎴欏灩缁犲鏌熸０浣藉厡闁告艾顑呴埞鎴︻敊绾嘲浼愰梺鍦拡閸嬪棝宕ｉ崨顖滅＝闁稿本鐟ч崝宥夋煥濮樿埖鐓冮悹鍥皺缁犵粯鎱ㄦ繝鍐┿仢鐎规洏鍔嶇换婵嬪礃椤忓嫬姹查梻鍌欑劍鐎笛兠洪敂鐣岊洸妞ゅ繐瀚弳锔界箾瀹割喕绨婚柣鎺戠仛閵囧嫰骞掑澶嬵€栨繛瀛樼矋缁捇寮婚悢鐓庝紶闁告洦鍓﹀Λ鐐寸箾鐎涙鐭掔紒鐘崇墪椤繘鎼归崷顓犵厯闂佸憡鐗戦埀顒€鍘栫槐锝囩磽閸屾艾鈧摜绮旈幘顔芥櫇妞ゅ繐鐗忓畵渚€鏌涢幇闈涙灈閸烆垶姊洪崘鍙夋儓闁稿﹦鎳撻埢宥咁煥閸啿鎷洪梺纭呭亹閸嬫盯宕濆鈧弻娑㈠棘鐠恒劎鍔銈冨灪閹告悂鍩ユ径鎰潊闁抽敮鍋撻柟鐤缁辨捇宕掑▎鎴濆闂佹寧娲嶉弲娑㈠煝閹捐绫嶉柛顐ゅ枔閸樿棄鈹戞幊閸婃劙宕戦幘缁樼叆闁哄洢鍔嬮柇顖涱殽閻愭彃鏆欓摶鏍煕濠娾偓閻掞箓寮查埡浣叉斀?data\.warp-enabled闂?
//   闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪箻閸┾偓妞ゆ帊鑳堕埢鎾绘煛閸涱喚绠橀柛鎺撳笒閳诲酣骞樺畷鍥跺敽婵犲痉鏉库偓鎾绘倿閿曗偓閳诲秹寮惔鎾存杸闂佺粯锚閻忔岸寮抽埡浣叉斀妞ゆ洖鎳庡顔锯偓?  -> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴妤€浜惧銈庝簻閸熸潙鐣疯ぐ鎺濇晪闁告侗鍨版慨娲⒒娴ｄ警娼掗柛鏇炵仛閻ｅ墎绱撴担鎻掍壕婵犮垼娉涙径鍥磻閹捐崵宓侀柛顭戝枛婵骸顪冮妶蹇撶槣闁搞劏娉涢锝嗗閺夋嚦銊╂煥閺傚灝鈷旀い鏃€娲熷娲偡闁箑娈堕梺绋款儑閸犳牠濡撮崒鐐茶摕闁靛濡囬崢鎼佹⒑缁洖澧叉い顓炴喘閹矂骞掑Δ浣哄幍濡炪倖姊归弸濠氭嚀閸ф鐓欐い鏃傛櫕閹冲洦淇婇銏犳殭闁宠棄顦灃闁逞屽墴钘熸慨妯夸含绾?WARP 闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃繘鍩€椤掍胶鈻撻柡鍛█閵嗕礁鈻庨幘鍐插敤濡炪倖鎸鹃崑鐔兼偘閵夆晜鈷戦柛锔诲幖閸斿銇勯妸銉﹀櫧濠㈣娲樼换婵嗩潩椤撶姴骞嶉梻浣告啞閹稿棝宕ㄩ鐙€鍋ч梻鍌欒兌缁垶骞愭ィ鍐ㄧ獥闁圭増婢樻闂佸憡娲﹂崹鎵不濞戞瑣浜滈柡鍌氱仢閹垿鏌ｆ惔锝囧笡缂佺粯绋撻埀顒佺⊕椤洭鎯岄幒鏃傜＜闁绘ê纾晶顏呫亜椤愩垻绠伴悡銈嗐亜韫囨挸顏い鏂挎喘閺岀喖鎳濋悧鍫濇锭缂備焦褰冮幖顐㈠祫闂佸壊鍋侀崕鏌ュ煕閹寸姷纾藉ù锝囨娓氭稒绻涢崼鐔哥闁哄瞼鍠栭幃鐑芥偋閸喐鍊烽梻浣风串缁插潡宕楀鈧悰顔芥償閵婏箑鐧勬繝銏ｆ硾閻牓宕崶褉鏀介柣鎰煐瑜把呯磼閺屻儳鐣洪柟顔芥そ瀵挳鍩ラ崱妯荤翻婵犵數濮烽。顔炬閺囥垹纾婚柟杈剧畱绾惧綊鏌″搴″箺闁稿顑嗘穱濠囧Χ閸涱厽娈梺鍝勵儏閻楀繘鍩€椤掆偓缁犲秹宕曢柆宓ュ洭顢涘鍐炬閻熸粎澧楃敮妤呭煕閹烘鐓曟い鎰╁€曢弸娆撴煕鎼淬垹濮嶆鐐寸墪鑿愭い鎺嗗亾濠碘€炽偢閺岋綁骞囬濠呭惈闂佸搫鏈ú妯兼崲濞戞粠妲婚梺纭呮珪閸旀瑥顕ｉ幎绛嬫晝闁挎棁袙閹锋椽鏌ｉ悩鍙夊闁绘搫绻濋、鎾澄旈崨顔惧幗闂婎偄娲﹀Λ浣圭珶濮椻偓閹藉爼鎮欓悜妯煎幈闂侀€涘嵆濞佳囧几濞戙垺鐓?
//   婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佹儓闁搞劌鍊块弻娑㈩敃閿濆棛顦ョ紓浣哄С閸楁娊寮诲☉妯锋斀闁告洦鍋勬慨銏ゆ⒑濞茶骞楅柟鐟版喘瀵鎮㈤搹鍦紲闂侀潧绻掓慨鐢告倶瀹ュ鈷戠紒瀣儥閸庡秹鏌涙繝鍐疄鐎殿喖顭峰鎾偄閾忚鍟庨梻浣虹帛閸旓箓宕滃鑸靛仧闁哄啫鐗婇埛?-> 婵犵數濮烽弫鍛婃叏閻㈠壊鏁婇柡宥庡幖缁愭淇婇妶鍛殲闁哄棙绮嶆穱濠囧Χ閸涱厽娈堕梺娲诲幗閻熲晠寮婚悢鍏煎€绘俊顖炴櫜缁爼姊洪柅鐐茶嫰婢у墽绱掗悩铏碍闁伙綁鏀辩缓鐣岀矙鐠囦勘鍔嶉妵鍕籍閸ヮ灝鎾寸箾閸涱厾孝闁宠鍨块幃鈺呭垂椤愶絾鐦庡┑鐘愁問閸犳岸寮繝姘疇婵犻潧顑呯粈鍫㈡喐韫囨稒鍎楅柛鈩冪⊕閻撴洜鈧厜鍋撻柍褜鍓熷畷鎴︽倷閻戞ê浜楀┑鐐叉閹稿鎮″▎鎰╀簻闁哄秲鍔庨埊鏇犵磼閹澘鐨洪柟顕呭枛椤繈鎳滅喊妯诲闂備礁婀遍…鍫澝归悜钘夌厐闁哄洨鍠嶇换鍡樸亜閹扳晛鐏╅柡鍡悼閳ь剚顔栭崰鏍€﹂柨瀣╃箚闁归棿绀佸敮闂侀潧绻嗗Σ鍛焽閺冣偓缁绘繄鍠婂Ο娲绘綉闂佹悶鍔嶆繛濠傜暦閵忥綆妯勯梺闈涙閸婂骞夐幘顔肩妞ゅ繐妫涢悾楣冩⒒娓氣偓濞佳囨偋閸℃娲Χ閸涱垱鎳冩繝鐢靛Х閺佹悂宕戝☉妯滅喐绻濋崶褍鍋嶆繝闈涘€搁幉锟犲磻閳哄懏鐓熼柟瀵稿Х閹藉倿鏌℃担绋挎殻闁哄被鍔岄埞鎴﹀幢濡櫣鐛╅梻浣侯攰濞呮洜鎹㈤崼婵愭綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顦遍柛妤佸▕瀵鈽夐姀鐘殿唺闂佽宕樺▍鏇犳嫚閻愬樊娓婚柕鍫濆暙閻忣亞绱掔€ｎ偄鐏撮柍銉畵婵℃悂鍩℃担渚Ч婵＄偑鍊栭崝锕傚磻閳ь剟鏌涚€ｎ偅灏柍钘夘樀婵偓闁抽敮鍋撻柟閿嬫そ濮婂宕掑鍗烆杸缂備礁顑嗛幐濠氬疾閸洖鍗抽柣鏃囨椤旀洟鏌ｈ箛鏇炰粶闁稿﹥鎮傞崺鈧い鎺嶇贰濞堟粓鏌熼鐐効妞わ箒顫夐幈銊︾節閸曨厼绗￠梺鐟板槻閹虫劗鍒掗悽纰樺亾閿濆簼绨奸柣锝勫嵆濮婄粯鎷呮笟顖滃姼缂備胶绮崹鍦垝濞嗘挸绠婚柡鍌樺劜閻庡姊洪棃娑氱疄闁稿﹥娲栧ú鍧楁⒒娴ｈ鍋犻柛搴㈡そ閹繝宕煎┑鎰姺闂佺粯鏌ㄩ崥瀣煕閹烘垯鈧帒顫濋浣规倷婵犫拃灞芥灓缂佽鲸甯楀鍕節鎼粹剝顔嶉梻浣烘嚀瀵爼骞愰幎钘夋瀬闁稿本绋掗崣蹇涙煙闁缚绨介柣鈺侀叄濮婄粯鎷呴崨濠冨創缂備礁顦伴悡鈥崇暦濠靛﹦鐤€闁哄啫鍊昏ぐ鎯р攽閳藉棗鐏犻柛姘儔瀹曟垿寮介鐔哄幗闂佸搫鍊搁悘婵嬪箖閹达附鐓冮梺鍨儏閻忔挳鏌＄仦鍓р槈闁宠鍨垮畷鍗炍旀繝浣瑰亝闂傚倷绀侀幖顐⑽涘☉銏犵；闁靛牆顦拑鐔哥箾閹寸偟鎳勯柛搴ｅ枛閺屾洟宕煎┑鍥ф濠殿喛顫夐〃濠傤潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯閸ㄥ爼寮诲☉妯锋斀闁糕剝绋戝浼存倵鐟欏嫭绀€缂傚秴锕獮鎰節閸愩劎绐為柣蹇曞仧閸嬫捇顢欓弴銏♀拻濞达絿鎳撻婊呯磼鐠囨彃鈧儻妫熷銈嗙墬缁孩绋夊澶嬬叆婵犻潧妫Σ褰掓煟閹捐泛鏋戝ǎ鍥э躬椤㈡稑顫濋崣妯挎闂備焦鐪归崝宥咁渻娴犲钃熼柨婵嗩槸鍥撮梺绋挎湰閻喗绔?WARP 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛洨绡€闁稿本绮岄。鐢告⒑闁偛鑻晶鍓х磼閻樿櫕灏柣锝夋敱缁虹晫绮欏▎鐐秱闂備胶鍋ㄩ崕閬嶅疮鐠恒劏濮抽柕澶涘缁♀偓闂佹眹鍨藉褎绂掗敃鍌涚厱妞ゎ厽鍨甸悘锔锯偓瑙勬礈閹虫挾鍙呭銈呯箰鐎氼噣鏁嶉悢鍏尖拺闁荤喐婢橀埛鏃傜磼椤曞懎鐏﹂柟顕嗙節瀹曟﹢顢旈崨顓熺€炬繝鐢靛Т閿曘倝鎮ч崱娆戠焼闁割偆鍠撶粻楣冩煙鐎电浠ч柟鍐叉噺閵囧嫰鍩￠崒婊冨绩闂佸搫鏈粙鎴﹀煡婢跺娼ㄩ柛顐ゅ枑閺嗘壆绱撻崒姘偓鍝ョ矓閺夋嚚娲敇閵忕姾鎽曢梺鍝勬川閸犳挾寮ч埀顒勬椤愩垺澶勯柟灏栨櫅鐓ゆい鎾卞灪閳锋垿鏌涘☉姗堟敾闁抽攱甯￠弻娑滅疀閺冩捁鈧寧顨ラ悙鎻掓殭閾绘牠鏌嶈閸撶喖宕洪姀銈呯閻犲洦褰冮悗顓烆渻閵堝棙鈷掗柡鍜佸亝缁傚秹骞囬悧鍫氭嫽婵炶揪绲肩拃锕傛倿閻愵兙浜滈柟瀛樼箖椤ャ垺顨ラ悙鏉戠伌鐎规洖鐖奸、妤呭焵椤掑嫭瀚?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴妤€浜惧銈庝簻閸熸潙鐣疯ぐ鎺濇晪闁告侗鍨版慨娲⒒娴ｄ警娼掗柛鏇炵仛閻ｅ墎绱撴担鎻掍壕婵犮垼娉涙径鍥磻閹捐崵宓侀柛顭戝枛婵骸顪冮妶蹇曠窗闁告鍟块悾鐑藉箣閻愯尙绐為梺褰掑亰閸橀箖宕ｉ崱妞绘斀闁绘绮☉褎淇婇锝囨噭鐎垫澘瀚板畷鐔碱敃閻斿憡鏉告俊鐐€栭悧妤呭传鎼淬劌纾婚柕蹇嬪€栭悡娑氣偓鍏夊亾閻庯綆鍓欓崺宀勬煣缂佹澧甸柡灞界Х椤т線鏌涢幘璺烘灈妤犵偛鍟抽ˇ宕囩磼缂佹绠炵€规洖鐖兼俊鎼佸Ψ閵忕姳澹曢梺鍓茬厛閸嬩焦绂嶅鍫熺厸闁告劧绲芥禍鍓х磽娴ｇ柉鍏屾い銏狅躬婵＄敻骞囬弶璺唺闂佽鍎抽崢鏍уΔ鍛拻濞达絽鎲￠崯鐐烘煕閺冣偓閻楃偤濡甸幇鏉跨闁冲搫鍊稿鍧楁⒑瑜版帗锛熼柣鎺炵畵閸╂盯骞嬮敂瑙ｆ嫼濡炪倖鍔戦崐鏇熺濠婂牊鐓犳繛鑼额嚙閻忥繝鏌￠崨顓犲煟妞ゃ垺鐟ф禍鎼佸冀閵婏附婢戦梻鍌欒兌椤牓寮甸鍌滅煓闁圭儤顨呴悡鏇㈡煙閻戞ɑ鈷掔痪鎯у悑閵囧嫰寮撮悙鏉戞濡ょ姷鍋為〃鍫ュ焵椤掆偓閻忔艾顭垮Ο灏栧亾濮橆偄宓嗛柣娑卞櫍瀹曞爼顢楁径瀣珜闂備線鈧偛鑻晶顔姐亜閺囶亞鍒伴柣锝忕節閺屽洭鏁傞挊澶屾В闂傚倷绶氬褔鎮ч崱妞㈡稑鈽夊▎鎰彿濡炪倖甯掗崐鑽ゅ婵傚憡鍋ｉ柛銉簻閻ㄦ椽寮崼銉﹀€?JSON闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶闁告挆鍛闂佽瀛╅懝楣兯囨导鏉懳﹂柛鏇ㄥ灠缁犳娊鏌涢埄鍐︿沪濠㈣娲熷娲川婵犲孩鐤佹繛瀛樼矊閻栫厧顕ｇ拠娴嬫婵☆垶鏀遍～宥呪攽椤旂瓔娈旈柣妤€妫欓幈銊モ槈閵忊檧鎷洪悗瑙勬礀濞层劎鏁☉娆庣箚妞ゆ劑鍨归弳娆愩亜閺囶亞绉€规洖銈稿鎾偄鐏忎礁浜鹃柣鎴ｅГ閻撴稑顭跨捄渚剰闁绘帞鍋撴穱濠囶敃閵忊槅妫﹀┑顔硷攻濡炶棄鐣烽锕€唯闁靛濡囬埀顒€顭峰娲箰鎼淬垹顦╂繛瀛樼矤娴滄粓顢?.autostart-off 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧潡鏌熺€电孝缂佽翰鍊濋弻锕€螣娓氼垱锛嗗┑鐐叉▕娴滄繈寮插鍫熺厽闁逛即娼ф晶顕€骞栭弶鎴含婵﹥妞藉畷銊︾節閸屾鏇熺箾鐎涙鐭嬮柣妤冨Т閻ｇ兘骞嬮敃鈧粻濠氭偣閸ャ劌绲婚柣搴幖椤啴濡堕崱妤€衼缂傚倸绉村Λ妤勭亱闂佸憡鍔﹂崰妤呭磹閻㈠憡鍋℃繛鍡楃箰椤忣亞鐥悙顒佸€愰柡灞剧⊕閹棃濡堕崱娆忔锭缂傚倷绀侀崐鍦暜閿熺姴鏋侀柛鎰靛枛绾惧吋绻涢幋鐐嗘垿鎮炬导瀛樷拻濞达絿顭堥弳閬嶆煙绾板崬浜伴柡浣稿暣閺佹劙宕堕…鎴炵稐婵犵妲呴崹鐢稿磻閹邦喖顥氬ù鐘差儐閻撴洟鎮橀悙鎻掆挃闁瑰啿鎳忛妵鍕晜婵傚憡顎嶉梺闈涙搐鐎氫即寮崒婊勫珰闁肩⒈鍏涢崠鏍磽閸屾瑧璐伴柛鐘愁殔椤繑绻濆顒傚姦濡炪倖甯婇懗鍫曞煀閺囩喆浜滄い鎾跺仦閸犳﹢鏌曢崱妤佸殗闁哄被鍔庨埀顒婄秵閸撴盯顢欓弴銏♀拺闁荤喖鍋婇崵鐔兼煕婵犲啫鐏ユい顐ｇ箘閹瑰嫰宕崟顐ｎ吇闂傚倷绀侀幖顐ょ矙娓氣偓瀹曘垺绂掔€ｎ偄浜楀┑鐐村灟閸ㄦ椽鎮￠弴銏＄厪濠㈣埖绋撻崚鏉款熆瑜嶉幉锟犲箞閵娿儙鐔煎箰鎼达絻鈧劗绱?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柤纰卞墻濡查亶鏌ｉ悢鍝ョ煂濠⒀勵殘閺侇噣骞掑Δ鈧崹鍌炴煕瀹€鈧崑鐐烘偂閻旈晲绻嗛柕鍫濆€告禍楣冩⒑缂佹﹩娈旈柨鏇ㄤ簻閻ｇ兘鏁愰崼銏㈡澑濠电偞鍨堕悷褔宕㈤幘缁樷拺闁告稑锕︾粻鎾绘倵濮樼厧寮€规洘濞婂畷鐑筋敇閻樼绱叉俊鐐€栭崝鎴﹀垂缂佹ɑ娅忛梻鍌欑閹碱偆鎮锕€纾规繝闈涱儑瀹撲線鏌″鍐ㄥ濠殿垱鎸抽幃宄扳枎韫囨搩浼冮梺杞扮贰娴滎亜顫忕紒妯诲闁荤喐澹嬮弸娆撴⒑鐞涒€充壕闂佸憡顨堥崐锝呪槈閵忕姷顦ㄥ銈嗘閸嬫劙鎮?婵犵數濮烽弫鍛婃叏閻㈠壊鏁婇柡宥庡幖缁愭淇婇妶鍛殲闁哄棙绮嶆穱濠囧Χ閸涱厽娈堕梺娲诲幗閻熲晠寮婚悢鍏煎€绘俊顖炴櫜缁爼姊洪柅鐐茶嫰婢у墽绱掗悩铏碍闁伙綁鏀辩缓鐣岀矙鐠囦勘鍔嶉妵鍕籍閸ヮ灝鎾寸箾閸涱厾孝闁宠鍨块幃鈺呭垂椤愶絾鐦庡┑鐘愁問閸犳岸寮繝姘疇婵犻潧顑呯粈鍫㈡喐韫囨稒鍎楅柛鈩冪⊕閻撴洜鈧厜鍋撻柍褜鍓熷畷鎴︽倷閻戞ê浜楀┑鐐叉閹稿鎮″▎鎰╀簻闁哄秲鍔庨埊鏇犵磼閹澘鐨洪柟顕呭枛椤繈鎳滅喊妯诲?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍏煎€绘俊顖濇閸旂鈹戦悙鑼癁闁哄懐濞€瀵寮撮悢铏诡啎闂佺粯鍔﹂崜姘舵偟閺冨牊鈷戦柛婵嗗閸庡海绱掗懜闈涘摵鐎殿喖顭峰鎾閻樿鏁规繝鐢靛Т閻忔岸宕濋弽顐ゆ噮缂傚倸鍊搁崐鐑芥嚄閼搁潧鍨旈悗闈涙憸缁€濠偯归敐鍛棌婵炲吋鐗楃换娑橆啅椤旇崵鐩庨梺鎼炲妼閸婂綊濡甸崟顖氬嵆闁绘劖娼欏▍锝夋⒑閸涘﹥灏柛鐔锋健閸╃偤骞嬮敂缁樻櫓缂佺虎鍘鹃弻澶愬触椤愩倗纾藉ù锝呮惈鏍￠梺缁橆殘婵挳顢氶敐鍥ㄥ珰婵炴潙顑嗛～宥呪攽椤旂瓔娈旀俊顐ｇ懇瀹曟捇鎮介崨濞炬嫼缂傚倷鐒﹁摫閻忓浚鍘界换娑㈠箻椤旇姤鏆犵紓浣稿€圭敮锟犮€佸☉銏″€风紒顔款潐鐎氬ジ姊洪崫鍕垫Ш闁稿锕㈠畷锟犲礃瀹割喚鈧爼鏌ｉ幋锝呅撻柣鎾存礀閳规垿鎮╅幓鎺撴婵犫拃灞肩凹闁靛洤瀚伴、姗€鎮欓弶鎴П缂傚倷鑳剁划顖滄崲閸岀儑缍栨繝闈涱儛閺佸棝鏌涚仦鎯х劰闁稿鎹囬幃娆撴倻濡攱瀚奸梻鍌氬€搁悧濠冪瑹濡も偓鍗遍柛顐ｆ礃閻撴洟鏌曟繝搴ｅ帥婵炲吋鍔楃槐鎺楀磼濮樻瘷褎顨ラ悙鍙夘棦闁炽儲鐗犻崹鎯х暦閸モ敩銊╂⒑鏉炴壆鍔嶉柛鏃€鐗曢銉╁礋椤愬鍠庣叅缂備焦锕╁Λ婵堢磽?WARP 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛洨绡€闁稿本绮岄。鐢告⒑闁偛鑻晶鍓х磼閻樿櫕灏柣锝夋敱缁虹晫绮欏▎鐐秱闂備胶鍋ㄩ崕閬嶅疮鐠恒劏濮抽柕澶涘缁♀偓闂佹眹鍨藉褎绂掗敃鍌涚厱妞ゎ厽鍨甸悘锔锯偓瑙勬礈閹虫挾鍙呭銈呯箰鐎氼噣鏁嶉悢鍏尖拺闁荤喐婢橀埛鏃傜磼椤曞懎鐏﹂柟?-> 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫇闁逞屽墰婢规洘绻濆顓犲幈闂佹枼鏅涢崯浼村煡婢舵劖鐓曢悘鐐额嚙婵倿鏌＄仦璇测偓鏇㈡箒闁诲函缍嗛崑鍛存偟閺囩儐娓婚柕鍫濋娴滄繃绻涢懠顒€鏋涚€殿喖顭烽幃銏ゅ川婵犲嫮肖闂備礁鎲￠幐鍡涘川椤旂瓔鍟呴梻?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰紦閻㈢粯淇婇悙顏勨偓鏍偋濡ゅ啰鐭欓柟鐐湽閳ь剙鎳樺畷锝嗗緞瀹€鈧惁鍫熺節閻㈤潧孝闁稿﹥鎮傚鎼佹晝閸屾稓鍘鹃梺缁橆殔閻楀棝顢氬鍫熺厪闁搞儜鍐句紓缂備胶濮甸惄顖炵嵁濡吋宕夊〒姘煎灡濮ｅ洭姊绘担钘夊惞闁哥姵鍔欓幃鐑芥焼瀹ュ棌鎸冮梺鍛婃处閸ㄧ増顢婇梻浣告啞濞诧箓宕规导鏉戠闁逞屽墴濮婃椽妫冨☉杈╁姼闂佸憡鏌ㄩ惌鍌濇＂濠电娀娼ч鍡涘磻閻樿櫕鍙忔俊顖氭惈閼稿綊鏌＄€ｎ偆鈯曢柟渚垮妽缁绘繈宕橀埞澶歌檸闁诲氦顫夊ú锕傚磻婵犲倻鏆﹂柣鏃傗拡閺佸啴鏌ㄥ┑鍡橈紞闁哥喎鐗嗛埞鎴︽偐閸偅姣勯梺绋款儐閻╊垵妫熼梺瑙勵問閸犳岸鎯屽▎鎾寸厵闁绘垶蓱閳锋帡鏌ｉ鐑嗘█闁哄瞼鍠栭幃婊兾熼搹閫涙喚婵犵鍓濊ぐ鍐偋婵犲嫭宕叉繛鎴欏灩楠炪垺淇婇婵囶仩濞存粌鐖煎娲传閸曢潧鍓遍梺缁橆殘婵挳锝炶箛鎾佹椽顢旈崨顖氬Х闂備胶绮崝妯间焊椤忓棌鍋撳顓熺凡妞ゎ叀鍎婚¨渚€鏌涢敐蹇曠М鐎殿喛顕ч濂稿炊閵娿儳褰撮梻浣告惈鐞氼偊宕曢崡鐑嗗殨濞寸厧鐡ㄩ埛鎴犵磼椤栨稒绀冮柡澶嬫そ閺屾稒鎯旈妸锔介敪闂佷紮绲块崗妯讳繆閹间礁鐓涘┑鐘插暞濞呮牗绻濋悽闈浶㈤柨鏇樺€楅埀顒佺▓閺呮盯寮抽埡鍌滅瘈闁汇垽娼у暩闂佽桨鐒﹂幃鍌氱暦閹达附鍊烽柛婵嗗閻庮參姊洪崜鎻掍簼婵炲弶鐗滅划缁樺鐎涙鍘卞┑鐘绘涧鐎氼剟宕濋妶澶嬬厽妞ゆ挾鍠愬畷宀勬煛瀹€瀣М濠殿喒鍋撻梺闈涚箚閺呮繈宕濈粙搴撴斀闁宠棄妫楁禍婵嬫煥濮樿埖顥嗗鑸靛姈閻撱儲绻濋棃娑欘棤濠⒀嗛哺缁绘盯宕楅悡搴☆潓闂傚洤顦扮换婵婎槻闁诲繑绻堥崺鈧い鎺嶈兌婢ч亶鏌熼獮鍨仼闁宠棄顦垫慨鈧柣妯活問閸氬懘姊绘担铏瑰笡闁搞劑娼ц灋婵炴垯鍨圭粈澶愭煟閹达絽袚闁抽攱鍨块弻娑樷攽閸℃浠惧銈冨劜閻熲晠寮诲☉妯滄梹鎷呯化鏇熺亞闁诲孩顔栭崰鏍€﹂悜钘夋瀬闁瑰墽绮崑鎰版煠绾板崬澧绘俊鑼娣囧﹪鎮欓鍕ㄥ亾閺嶎厼绠板Δ锝呭暙缁愭鏌熼幑鎰靛殭鏉╂繈姊鸿ぐ鎺戜喊闁稿繑锕㈠銊︾鐎ｎ偆鍘介梺褰掑亰閸撴瑧鐥閺屾盯鏁愰崱妯镐虎闂佽鍠曠划娆撱€佸☉銏犖ч柛銉㈡櫇濡插洭鏌ｆ惔銏╁晱闁革綆鍣ｅ畷鎴﹀礋椤栵絾鏅ｆ繝闈涘€婚…鍫濐啅濠靛洢浜滈柡宥冨妿閳洟鏌ｉ敐鍥у幋婵﹦绮幏鍛村川婵犲倹娈樻繝鐢靛仩椤曟粓姊介崟顓犵焿鐎广儱顦伴崐鐑芥煕椤垵浜芥繛鑲╁枎閳规垿顢欑粵瀣姼闂佺硶鏅滈悧鐘诲春濞戙垹绠ｉ柣妯兼暩閿涙粓鏌ｆ惔顖滅У闁告鏅☉鐢稿礈娴ｈ櫣锛滈梺缁樏崯鍧椝夌€ｎ剛纾奸弶鍫涘妼缁楁氨鈧灚婢樼€氼噣鍩€椤掑﹦绉甸柛瀣瀹曨剚寰勬繛銏℃閹晠妫冨☉妤冩崟缂傚倷鑳剁划顖炲垂瑜版帒鐒垫い鎴ｆ硶缁佺兘鏌涚€ｎ偄濮嶉柛鈹惧亾濡炪倖甯婇懗鍫曞煀閺囥垺鐓ユ慨姗嗗劒瑜版帗鍋愰柧蹇ｅ亖娴犮垹鈹戦纭锋敾婵＄偠妫勮灋闁告劑鍔夊Σ鍫ュ级閸碍娅呯憸鏉款儑缁辨捇宕掑▎鎺戝帯婵犳鍠氶弫璇茬暦瑜版帒閿ゆ俊銈傚亾缂佺姵鐗犻弻銊╂偄閸濆嫅銏ゆ煕濡や礁鈻曢柡灞炬礉缁犳盯濡疯閿涚喖姊洪崨濠庣劷闁轰浇顕ч～蹇旂節濮橆剛锛滃┑顔矫畷顒劼烽埀顒€鈹戦悙瀛樺鞍缁炬澘绉归妴鍐╃節閸屾粍娈鹃梺鍝勬祫缁辨洟鎮块埀顒勬⒑閸︻叀妾搁柛銊╀憾瀵剟鍩€椤掍椒绻嗛柣鎰典簻閳ь剚鐗曡灋濞撴埃鍋撻柨婵堝仦瀵板嫭绻涢悙顒傗偓娲⒑缁洖澧叉い顓炴川缁粯銈ｉ崘鈺冨幗闂佽鍎抽崯鍧楁儊閵娾晜鐓熼柟鍓у仺閸嬨垽鏌″畝鈧崰鏍€佸▎鎾村仼閻忕偞鍎冲▍姘舵煟鎼淬埄鍟忛柛鐘崇墵閹儲绺介崫銉ョウ闁诲函缍嗘禍鏍绩娴犲鐓欓梺顓ㄧ畱鐢劑鏌嶉崫鍕櫤闁抽攱甯￠弻娑氫沪閸撗勫櫙闂佺绻愰惌鍌炲蓟閻斿吋鎯炴い鎰剁到绾炬娊姊虹拠鈥虫灍闁挎碍銇勯锝囩畼闁圭懓瀚伴幖鍦嫚閳ヨ櫕鏆┑鐘垫暩婵兘寮崨濠冨弿闁汇垽娼ч弸鍫⑩偓骞垮劚椤︻垳澹曡ぐ鎺撶厱妞ゆ劧绲剧粈鍐煟閹惧崬鍔﹂柡宀嬬節瀹曞爼鍩℃担椋庢崟闂備線鈧偛鑻晶顖炴煟閳╁啯绀堥柟?
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

// ---------------------------------------------------------------- WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙５闁逞屽墾缁犳挸鐣锋總绋课ㄦい鏃囧Г濞呭牓姊绘担鍛婂暈闁告棑绠撳畷浼村冀椤愩埄妫滈梺绉嗗嫷娈曢柍閿嬪灦閵囧嫰骞掗崱妞惧缂傚倷绀侀ˇ顖烆敋椤撶姴鍨濆┑鐘宠壘閸愨偓濡炪倖鍔楅崰搴㈢閻愵剦娈介柣鎰皺娴犮垽鏌涢弮鈧喊宥夊Φ閸曨垱鏅滈悹鍥皺娴犫晛顪冮妶鍡樼叆缂佺粯鍔欓崺鐐哄箣閿曗偓楠炪垽鐓崶銊︹拹闁哄鎮傚缁樻媴閾忕懓绗￠梺鎸庢皑閳ь剚顔栭崰娑㈠春閺嶎厼绀嗗┑鐘插暟闂勫嫰鏌涘☉姗堝姛闁?
// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佹儓闁搞劌鍊块弻娑㈩敃閿濆棛顦ョ紓浣哄С閸楁娊寮诲☉銏╂晝闁挎繂妫涢ˇ銉х磼閻愵剙绀冩い顐㈩槹缁岃鲸绻濋崶顬囨煕濞戝崬鏋涢柣婵囨⒒缁?WARP 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佹儓闁搞劌鍊块弻娑㈩敃閿濆棛顦ョ紓浣哄С閸楁娊寮诲☉妯锋斀闁告洦鍋勬慨銏ゆ⒑濞茶骞楅柟鐟版喘瀵鏁愭径濠勵吅闂佹寧绻傚Λ顓炍涢崟顓犵＝濞达絾褰冩禍楣冩⒑缁嬫寧婀扮紒瀣浮瀹曪綀绠涢弬鍓х畾闂佺粯鍔欓·鍌炲吹濞嗗緷鐟邦煥閸垻鏆┑?闂?
//   1) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢幘鑼槮闁搞劍绻冮妵鍕冀椤愵澀姹楅梺閫炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劑鏌曟径娑㈡鐞氾綁姊婚崒娆戭槮闁圭⒈鍋嗙划娆愮瑹閳ь剙鐣烽姀锛勯檮闁告稑锕ら埀顒€娼￠弻娑⑩€﹂幋婵呯按婵炲瓨绮嶇划鎾诲蓟閻旇　鍋撻悽娈跨劸濞寸姵绮嶉妵鍕即閻旇櫣鐓侀梺闈涙搐鐎氫即寮崘顔肩＜闁靛牆妫Λ鏍磽?connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰紦閻㈠姊绘担鐟邦嚋婵炲弶鍨块幃鐐烘晝閸屾氨鐤囬悗鍏夊亾闁告洦鍏橀幏?disconnected      -> 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮撮悙娴嬪亾閹间焦瀚呴柣鏂垮悑閻撳繐鈹戦悙闈涗壕婵炲懏鎹囬弻鈩冩媴缁嬫浼冮梺鍝勬湰缁嬫垿鍩ユ径濞炬瀻闁归偊鍠氶埢蹇涙⒒娴ｅ憡鎯堥悗姘ュ姂瀹曟洟鎮界粙鑳憰闂佸搫娴勭槐鏇㈡偪閳ь剟鏌ｆ惔顖滅У濞存粍鐗犲畷鎴﹀箻鐠囨彃鍞ㄥ銈嗘尵閸犲酣鍩€椤掑啯纭剁紒杈ㄦ尰閹峰懘鎳栭埄鍐ㄧ伌鐎规洩缍€缁犳盯寮崜褏鐣炬俊鐐€栭悧妤呭传鎼淬垻顩烽柟鐑橆殕閻撴洘鎱ㄥΟ鐓庡付濞存粈鍗抽弻锛勪沪閸撗€妲堥梺瀹狀嚙濞撮攱绂掗敃鍌氱闁圭儤鍩堝Λ?
//   2) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢幘鑼槮闁搞劍绻冮妵鍕冀椤愵澀姹楅梺閫炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劑鏌曟径娑㈡鐞氾綁姊婚崒娆戭槮闁圭⒈鍋嗙划娆愮瑹閳ь剙鐣烽姀锛勯檮闁告稑锕ら埀顒€娼￠弻娑⑩€﹂幋婵呯按婵炲瓨绮嶇划鎾诲蓟閻旇　鍋撻悽娈跨劸濞寸姵绮嶉妵鍕即閻旇櫣鐓傜紓浣介哺鐢骞忛崨鏉戜紶闁告洖鐏氬В澶愭⒒?connected 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｅΟ娆惧殭缂佺姴鐏氶妵鍕疀閹炬惌妫炵紓浣界堪閸婃繈寮婚敃鈧灒闁绘艾顕粈鍡椻攽閻愭潙鐏︽い顓炲€垮顕€宕煎┑鍡欑崺婵＄偑鍊栭幐楣冨磻閻愬搫纾挎い蹇撴噸缁诲棝鏌ｉ幇鍏哥盎闁逞屽墯閻楃娀鐛幇鏉块唶闁哄洨鍋涢悗顓㈡偡濠婂懎顣奸悽顖涱殜瀹曟洖螖娴ｉ绠氶梺闈涚墕閹冲繘宕冲ú顏呯厽闁规儳宕崝婊呯磼缂佹绠炴俊顐㈠暙閳藉鈻庡Ο浼欑磼濠碉紕鍋戦崐鎴﹀磿閼碱剙鍨濋柟鎹愵嚙缁犳牜鐥鐐村婵℃彃顭峰铏光偓鐧搁檮濠㈡ɑ淇婇崸妤佺厵妤犵偛鐏濋悘鈺呮煃閽樺妯€濠殿喒鍋撻梺缁橆焽閺佸鎮伴幘缁樷拻濞达絿鐡斿鎰版煙椤栨氨鐒哥€规洏鍨虹缓鐣岀矙鐠侯煈妲舵俊鐐€栭弻銊︽櫠閻ｅ苯顥氶柦妯侯棦瑜版帗鏅查柛顐亜濞堟劕鈹戦悙鑼婵﹤缍婃俊鐢稿礋椤栨稒娅嗛梺缁樺姦閸撴艾袙閸儲鍊垫繛鍫濈仢閺嬫稒銇勯鐐叉Щ闁伙絿鍏樻俊鎼佸煛婵犲啯娅嶉柣鐔哥矊闁帮絽鐣烽幘缁樺仺闁告稑艌閹锋椽姊洪崨濠勨槈闁挎洏鍎抽埀顒€鐏氶悷鈺呭蓟濞戞﹩鐓ラ柍褜鍓熼幃鐐烘晝娴ｆ彃鎮佹繝鐢靛Х閺佸憡鎱ㄩ悜钘夋瀬闁归棿绀佺壕濠氭煟閹邦剚鎯堥柛銊ュ€块弻娑㈩敃閿濆棛顦ョ紓浣哄С閸楁娊寮诲☉銏╂晝闁挎繂妫涢ˇ銉╂⒑閹肩偛濡奸柛濠傛健瀵鈽夐姀鈩冩珕闁荤姴娲﹁ぐ鍐不娴煎瓨鈷戠紒瀣儥閸庢劙鏌熼悷鐗堟悙闁伙絿鏌夐妵鎰板箳濠靛洦娅囬梻浣虹《閸撴繈鏁嬮梺璇茬箲閹告娊寮?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮村Δ鈧禍楣冩煣閼姐倕浠遍柣鎿冨亰瀹曞爼濡歌濡插牓姊洪幖鐐插闁稿﹤娼″濠氭偄閻撳海鐣鹃梺缁橆殔閻楁粌螞閸曨厾纾奸柣鎰靛墮閸斻倖銇勯鐘插幋鐎殿喖顭锋俊鎼佸Ψ閵忊剝鏉搁梻浣虹《閸撴繈鏁嬪銈忚吂閺呯姴顫忛搹鍦煓婵炲棙鍎抽崜閬嶆⒑缂佹﹩娈曟い銊ワ攻娣囧﹪鎮界粙璺槹濡炪倖鐗楀銊╂偪閳ь剟姊绘担绋款棌闁稿鎳庣叅婵せ鍋撴い銏℃尭椤撳ジ宕担鐟扮槣闂備線娼ч悧鍡涘箠鎼搭煈鏁嗛柕蹇嬪€栭悡鐔兼煟閺傚灝绾ф繛鍛功閳?
//      -> 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛顥堟繛鎴濆船閸撴壆绱掗悙顒€鍔ゆい顓犲厴瀵鏁愭径濠冾棟闂佸壊鐓堥崰妤呭磹椤栫偞鈷戠紒瀣儥閸庡秹鏌涢弬鎸庢崳婵″弶鍔欓獮妯尖偓娑櫭鎾寸節濞堝灝鏋熼柛鈺佺墛缁傚秹鎮欏ù瀣€婚梺闈涚箞閸婃牠宕愰悜鑺ョ厱婵犻潧妫楅顐⑽旈悩鍙夊枠婵﹥妞藉畷婊堝箵閹哄秷鍩呴梻浣侯焾椤戝懐鈧凹鍘剧划瀣吋婢舵ɑ鏅┑鐘诧工鐎氼剚绂?2 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣閻戝棛鍔┑鐐村灦椤洭寮抽崱娑欑厱闁哄洢鍔屾禍鐐淬亜閿旇骞栭柍瑙勫灴閹晝绱掑Ο濠氭暘婵犵數鍋犻婊呯不閹达箑鐓濈€广儱顦伴崑鎰磽娴ｈ鐒介柛妯挎閳规垿鍩ラ崱妤冧淮闂佺顑嗛崝妤呭极椤斿皷妲堥柕蹇ョ磿閸樹粙姊洪崷顓炲妺婵﹨宕甸弫顔尖槈閵忥紕鍘垫繛鎾磋壘閻忔繃淇婃總鍛婃嚉闁挎繂鎷嬮悢鍡涙煕閹捐尙璐伴柣鎺楃畺閺岀喖顢涘鍗炩叺濠殿喖锕ュ钘夌暦閵婏妇绡€闁告洦鍓欓弸锕傛⒒娴ｄ警鐒炬い鎴濇楠炴垿宕惰濞兼牜绱撴担鑲℃垶鍒婇幘顔界厽闁绘梻鍘ф禍浼存煕閵堝拋鍎戠紒杈ㄦ尰閹峰懘宕滈幓鎺戝闂備焦鎮堕崝灞炬櫠鎼达絿纾介柟鎯ь嚟椤╃兘鎮楅敐搴濈敖闁活偄瀚板铏圭矙閹稿孩鎷辩紓渚囧枛闁帮綁骞?10 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极瀹ュ绀嬫い鎺嶇劍椤斿洭姊绘担鍛婅础闁稿簺鍊濆畷鐢告晝閳ь剟鍩ユ径濞㈢喖鏌ㄧ€ｎ偅婢戦梻浣筋嚙閸戠晫绱為崱妯碱洸闁绘劒璀﹂弫濠囨煕閵夘喖澧柍閿嬪灴閹嘲鈻庤箛鎿冧患缂備讲鍋撻柛鎰ㄦ杺娴滄粓鏌￠崶褎顥滈柟铏姍閹矂宕卞☉娆戝幈闂侀€涘嵆濞佳囧几閻斿吋鐓熼柟鎯х摠缁€鍐磼缂佹娲存鐐差儏閳规垿宕卞顒傚幋闂傚倷绀侀幖顐︻敄閸ヮ剙绐楅幖娣妽缁犳帡姊绘担铏瑰笡妞ゎ厼鐗忕划鏃堫敋閳ь剟銆侀弮鍫濋唶婵﹫绲芥禍楣冩煥閺囩偛鈧悂鏌ㄩ妶鍡曠箚闁靛牆瀚ˇ锔界箾閸喓绠炴慨濠傛惈鏁堥柛銉戝懍绱曢梻浣芥〃閻掞箓骞冮崒姘辨殾闁挎繂鎳夐弨浠嬫煕椤愮姴鐏柨娑欑箖缁绘稒娼忛崜褏袣濡炪値鍋勯ˇ鐢稿春濞戙垹绠涙い鎾跺Х椤旀洟姊虹粙璺ㄧ闁稿鍔欓獮濠囧炊椤掍胶鍘遍梺宕囨嚀閻忔繈鎮橀鍫熺厵妞ゆ牗绋掗ˉ鍫濃攽閳╁啯鍊愬┑锛勬焿椤﹁尙鈧鍠撻崝鎴濐潖缂佹ɑ濯撮柛娑橈工閺嗗牊绻涢幘瀵割暡妞ゃ劌锕ら悾鐤亹閹烘垹顔呴梺鍛婃尭閻忔繂煤椤擃潿鈧礁顫濈捄铏瑰姦濡炪倖宸婚崑鎾淬亜椤愩垻绠崇紒杈ㄥ笒铻ｉ柤娴嬫櫆閸ゅ矂姊绘担瑙勫仩闁稿孩绮撳畷姗€鎮欓鎸庡攭闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃繘骞戦姀銈呯闁绘ê鍟挎惔濠囨⒑閸撴彃浜濈紒璇插瀵娊鏁冮崒娑氬帾闂婎偄娲ら鍛村焵椤掍焦绀€婵炴彃鐏濋埞鎴︽偐椤旇偐浼囧┑鐐差槹閻╊垶寮崘顔芥櫇闁稿鎳撳ú顓€侀弴銏℃櫆闁芥ê顦崑鎾绘偨閸涘﹦鍘介梺缁樻煥閹诧紕娆㈤崣澶堜簻闊浄绲肩花缁樸亜椤撶偞鍋ラ柟顔惧厴閹囧醇椤愶綆妫у┑?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴妤€浜惧銈庝簻閸熸潙鐣疯ぐ鎺濇晪闁告侗鍨版慨娲⒒娴ｄ警娼掗柛鏇炵仛閻ｅ墎绱撴担鎻掍壕婵犮垼娉涙径鍥磻閹捐崵宓侀柛顭戝枛婵骸顪冮妶蹇撶槣闁搞劏娉涢锝嗗閺夋嚦銊╂煥閺傚灝鈷旀い鏃€娲熷娲偡闁箑娈堕梺绋款儑閸犳牠鐛繝鍥у窛閻庢稒顭囬崢鎾剁磽閸屾瑧鍔嶉拑杈ㄣ亜閵夈儳澧﹂柡宀€鍠栭、娆撴嚃閳哄唭銊╂倵鐟欏嫭绀€闁绘牕鍚嬫穱濠囧箹娴ｈ倽銊ф喐韫囨稑围闁绘垼濮ら悡鐔兼煟閹邦剛鎽犻柣銊﹀灴閺屾盯鎮╁畷鍥р吂濡炪値鍋勭换鎰弲濡炪倕绻愮€氼亞妲愰崼鏇熲拺闁告稑锕ユ径鍕煕鐎ｎ亜鈧鎳炴潏銊ь浄閻庯綆鍋€閹锋椽姊洪崨濠勨槈妞ゎ収鍓熼幃鐐淬偅閸愨晝鍘遍柣搴秵閸嬪嫭鎱ㄩ崶顒€纭€闂侇剙绉甸悡娆撴煟濡も偓閻楀﹦娆㈤懠顒傜＜闁绘瑥鎳愮粔顕€鏌＄仦璇插鐎殿喗娼欒灃闁逞屽墯缁傚秹宕滆绾?g_warpManualStop=true闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢琛″亾閻㈡鐒惧ù鐘欏洦鈷掗柛鏇ㄥ亜椤忣參鏌″畝瀣瘈鐎规洘锕㈡俊鎼佸Ψ閵忕姳澹曢梺鐓庮潟閸婃绋夊澶嬬厸闁稿本渚楅崕銉╂煟閺傛寧顥㈤柡灞诲€濋獮鏍ㄦ媴鐟欏嫰鏁┑鐘愁問閸犳牕煤閵娧勵潟闁圭儤顨嗛崑鎰亜閺冨倸甯跺┑顕嗙畵濮婃椽宕崟顓犱紘闂佸摜濮甸悧鐘荤嵁閸愩劉鏋庨柟鍨暞閺呫垺绻涙潏鍓хК婵炲拑缍侀崺銏ｇ疀閺傚墽绠氶梺缁樺姦娴滄粓鍩€椤掍礁鐏寸€规洘娲樺蹇涘煛閸愵亞鍘犻柣鐔哥矌婢ф鏁幒妤佸珔?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亜顒㈡い鎰矙閺屻劑鎮㈤崫鍕戙垻鐥幆褜鐓奸柡灞剧☉閳藉宕￠悙瀵镐壕闂備礁纾幊鎾剁矓瑜版帒钃熺憸鎴犵不濞戞﹩娼╅柛蹇氬亹缁夌兘鏌?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰紦閻㈠姊绘担鐟邦嚋婵炲弶鐗犲畷鎰亹閹烘垹锛涢柣搴秵閸犳鎮″☉銏＄厱闁规壋鏅涙俊濂告煃瑜滈崜娆撯€﹀畡鎵殾闁炽儲鍓氬鈺呮煠閹间焦娑ч柍褜鍓欓悘姘辨崲濞戙垹绠ｉ柣鎰仛閸ｄ粙姊虹紒妯虹瑐缂侇喗鐟╁?204 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮村Δ鈧禍楣冩煣閼姐倕浠遍柣鎿冨亰瀹曞爼濡歌濡插牓姊洪幖鐐插闁稿﹤娼″濠氭偄閻撳海鐣鹃梺缁橆殔閻楁粌螞閸曨厾纾奸柣鎰靛墮閸斻倖銇勯鐘插幋鐎殿喖顭锋俊鎼佸Ψ閵忊剝鏉搁梻浣虹《濡狙囧疾濠婂懐鎳呴梻浣告惈椤︻偊寮插鍛床闁割偅绻嶅鏍煣韫囨挻璐￠柣顓熺懇閺岀喓绮欓崹顕呭妷闂佸湱鏌夋禍顒傛閹惧瓨濯寸紒娑橆儏濞堫參鏌ｆ惔銏犲毈闁哥姵宀稿畷姘跺箳閹惧墎鐦堝┑顔斤供閸撴繈骞楅弴銏♀拺闂傚牊渚楀褍鈹戦垾铏枠鐎?true 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊瑜忛弳锕傛煟閵忋埄鐒剧痪鎯ь煼閺岋綁骞囬鑺ユ瘎闂佹椿鍘介悷鈺呮偂椤愶箑鐐婇柕濞р偓濡插牏绱撴担鎻掍壕婵犮垼鍩栭崝鏇犵不鐟欏嫮绠鹃柨婵嗛婢ь喖鈹戦敍鍕付妞も晛銈稿畷銊╊敇濞戞瑦鏉告俊鐐€栧濠氭偤閺冨牊鍊块柣鎰靛墰缁犳儳顭跨捄渚剳闁靛棙甯炵槐鎺楊敊閼测晛鈪甸悗瑙勬礈閸犳牠銆佸☉妯锋婵﹩鍓欑粻鐐测攽閻樺灚鏆╁┑鐐╁亾濠电偘鍖犻崗鐐☉铻栭柛婊€鐒﹂弲婊堟⒑閸愬弶鎯堥柛鐔稿婢规洟鎮剧仦绋夸壕妤犵偛鐏濋崝姘繆椤愶絿鎳冮柣锝呭槻閻ｆ繈宕熼鍌氬箰闂佽绻掗崑娑欐櫠娴犲鐓″鑸靛姈閻撳啴鎮归崶銊у弨妞ゅ孩顨婇幗鍫曟晲婢跺鍙勯棅顐㈡处缁嬪牓宕戦幘鏂ユ婵☆垯璀﹂崵娆撴⒒閸屾艾鈧悂宕愬畡鎳婂綊宕惰濞存牠鏌曟繛鐐珕闁抽攱姊婚埀顒€绠嶉崕閬嵥囬婊冾棜閻熸瑥瀚弧鈧梻鍌氱墛缁嬫帡鎯屽Δ鍐＜?
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
        if (st == 0) {                     // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣濞嗗繒浠鹃梺绋垮閸ㄥ潡寮婚敍鍕勃閻犲洦褰冩慨搴ㄦ煕閵夛附灏﹂柡宀€鍠栧畷妤呮嚃閳哄倹顔冮梻浣虹《閺呮稓鈧凹鍓濋悘瀣⒑閸涘﹦鈽夐柣掳鍔戦崺娑㈠箣閿旇棄浠梺鎼炲劘閸斿酣宕ｉ埀顒€顪冮妶鍛劉妞ゃ劌鐗忓Σ鎰板箳濡や礁浜归梺鑲┾拡閸庣柉顦撮柍褜鍓濋～澶娒哄鍫濈疇闁规崘娉涘鍙変繆閻愵亜鈧洜鎹㈤幇顔瑰亾濮樼厧骞栭崡杈ㄣ亜閹哄棗浜鹃梺瀹狀潐閸ㄥ潡骞冨▎鎾崇骇闁瑰濮冲鎾寸節濞堝灝鏋熼懣褔鏌涢弮鈧崹鐢告偩閻戣棄鐏抽柟棰佺劍閸嶉潧顪冮妶鍡樺鞍缂侇喖鐗忛懞杈ㄧ節濮橆厸鎷洪梺鍛婄箓鐎氼喛鈪归梻浣侯焾闁帮絾绂嶉崼鏇炴槬闁逞屽墯閵囧嫰骞掑鍥獥濠电偛鐗婇…鍥╂閹烘鏁婇柤娴嬫櫅椤も偓闁诲骸鐏氬妯尖偓姘煎幘閹广垹鈽夐姀鐘殿唺闂佺懓顕慨閿嬵殭闂傚倷娴囬鏍窗閹烘鐤鹃柣妯荤湽閳ь兛绶氬鎾綖椤斿墽鈼ゆ俊鐐€栭幐楣冨磿閹邦儵娑橆煥閸愶絾鏂€闂佺粯锚閻忔岸寮抽埡鍛厱閻庯綆鍓欐禒杈┾偓瑙勬礀缂嶅﹤鐣锋總绋垮嵆闁绘劘鍩栭幉浼存⒒娓氣偓濞佳嗗闂佸搫鎳忕划鎾崇暦閹寸姭鍋撻敐搴℃灍闁绘挻鐟╅弻鐔烘喆閸曨偄袝闂佹悶鍊栭悷褏妲愰幒妤€鐒垫い鎺戝绾惧吋绻涢幋鐑嗘畼缂佺姵鑹鹃—鍐Χ閸℃瑥顫х紓浣筋嚙閸婂灝鐣烽妷銊ｄ汗闁圭儤鎸鹃崢鎾剁磽閸屾瑧鍔嶉拑閬嶆煃闁垮娴柡灞界Х椤т線鏌涢幘瀵告噮缂侇喛顕ч鍏煎緞婵犲嫸绱甸梻鍌欑贰閸撴瑧绮旂€靛摜涓嶉柣妯碱暯閸嬫捇鐛崹顔煎濠电偛鐪伴崐婵嗙暦?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫇闁逞屽墰婢规洘绻濆顓犲幈闂佹枼鏅涢崯浼村煡婢舵劖鐓曢悘鐐额嚙婵倿鏌＄仦璇测偓鏇㈡箒闁诲函缍嗛崑鍛存偟閺囩儐娓?
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
constexpr wchar_t kAppVersion[] = L"3.19.1";

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
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕閻庤娲忛崕鎶藉焵椤掑﹦绉甸柛蹇旓耿瀹曟垿骞橀崜浣虹暥濠电偞鍨惰摫闁革絽缍婇弻娑㈠Ω閿斿墽鐤勯梺璇″枟閿曘垽骞冨▎鎾冲瀭妞ゆ劧绲胯ぐ褔姊绘担鍛婃儓婵☆偄顕划濠氬箻鐠囪尪鎽曢梺鎸庣箓椤︻垳鐚惧澶嬬厱閻忕偞宕樻竟妯好归悩鎰佺劷缂佽鲸鎸婚幏鍛村传閸曟埊绲剧换婵嬪閳藉懓鈧潡鏌嶉妷顖滅暤鐎规洜顭堣灃濞达絽鎲￠悿鍌炴⒒閸屾艾鈧悂鎮ф繝鍕煓闁硅揪缍嗛弶鐑樼節閻㈤潧啸闁轰焦鎮傚畷鎴濃槈閵忊€充槐闂侀潧顭堥崕宥吤洪鍛簻闂佺粯鎸稿ù鐑芥偩閸濆嫧鏀介幒鎶藉磹閹版澘纾婚柟鍓х帛閻撳啴鏌﹀Ο渚▓婵″弶鎮傞弻宥囨嫚閺屻儺鈧鎽堕敐澶嬬厾濠殿喗鍔曢埀顒佺墪閳绘挻銈ｉ崘鈹炬嫽闂佺鏈悷锕偹囨搴ｇ＜濡插本鐗曞▍宥嗘叏婵犱胶鐭欑€规洜鍠栭、姗€鎮㈠畡鎵搸闂傚倷绀佹竟濠囧磻娓氣偓瀹曟洟宕橀懠顒佹婵炴挻鍩冮崑鎾绘煛瀹€鈧崰鏍箖濠婂喚娼ㄩ柛鈩冾焽閺嗐儳绱撻崒娆掑厡濠殿喖鐏氱换娑欑節閸パ嗘憰闂佺粯妫侀妴鈧柛瀣崌閹棄鈻撶捄銊ュЪ闂佸摜鍠撻…鍫モ€旈崘顔嘉ч煫鍥ㄦ礈钃遍梻浣虹帛閹告挳鍩€椤掍礁澧繛鍏肩墬缁绘稑顔忛鑽ょ泿缂備胶濮抽崡鎶界嵁閺嶃劍缍囬柛鎾楀啰鐓楀┑鐘愁問閸犳牕煤閵娧勵潟闁圭儤顨嗛崐閿嬨亜閹哄秷鍏岄柛鎴磿缁辨挻鎷呴崜鍙壭ㄩ梺纭呮珪閹瑰洭銆佸鑸垫櫜濠㈣泛锕﹂鍛存⒑閸忛棿鑸柛搴ら哺缁傚秴顭ㄩ崼鐔叉嫼缂傚倷鐒﹁摫閻忓浚鍘界换娑氣偓鐢殿焾閸樺瓨銇勯姀锛勬噭鐎垫澘瀚换婵囨償閵忕姷妲ｉ梻鍌欑窔濞佳囨偋閸℃あ娑樷枎閹惧秴顦靛畷鍗烆渻缂佹ɑ鏉告俊鐐€栭弻銊╁箹椤愶箑鐒垫い鎺嶈兌缁犵偞顨ラ悙鎻掓殭閾绘牠鏌涘鈧悞锕傚疾閳轰讲鏀介柣鎰摠缂嶆垵螖閻樺弶鎲哥紒杈╁仦缁绘繈宕惰閻ｆ椽姊洪崘鍙夋儓闁瑰啿绻樺畷鎰磼濡湱绠氬銈嗙墬缁娀宕曢悩缁樼厓鐟滄粓宕滃璺虹婵炲棙鎸哥粻鏍偓鐟板閸嬪﹪鎮￠妷鈺傜厱婵炴垵宕楣冩煛閸♀晛寮慨濠呮缁辨帒螣缂佹ê鍓电紓鍌欒兌缁垳鎹㈤崼銉﹀仒妞ゆ梻鏅弧鈧梺绋款儓婵倕螞閸愩劎鏆﹂柕濞炬櫓閺佸﹪鏌涜箛姘汗闁哄鍋撻梻鍌氬€搁崐椋庢濮橆剦鐒界憸鏃傜槵闂侀€炲苯澧扮紒杈ㄥ浮閹晠鎼归銏㈩暡婵＄偑鍊ら崜娆撴晝椤忓牆绠栨繝濠傜墛閸ゅ秹鏌曟径鍫濈仾閻庢艾銈稿缁樻媴閾忕懓绗￠梺鍦焾閹芥粓鍩€椤掆偓閻忔氨鏁幒妤€鐓濋柟鍓х帛閸嬫劙鎮锋担鐤厡闁硅櫕鎹囬垾锕傚Ω閳轰胶顦板銈嗘尵婵兘寮抽妶澶嬧拺闁硅鍔曢崥褰掓煕鐎ｎ剙浠卞┑锛勬暬瀹曠喖顢涘杈╂澑闂備礁澹婇崑鍛哄鈧、鎾愁吋婢跺鎷虹紓浣割儐椤戞瑩宕曡箛鏂讳簻闁瑰瓨绻嶅Ο鈧悗娈垮枦椤曆囧煡婢舵劕顫呴柍鍝勫€瑰▍鍥⒒娓氣偓濞佳囨偋閸℃稑绠犻煫鍥ㄧ⊕閸婇潧螖閿濆懎鏆為柍閿嬪灴閺屾盯鏁傜拠鎻掔闂佸憡蓱濞兼瑩婀佸┑鐘诧工閸熶即宕洪敐鍥ｅ亾濞堝灝鏋涙い顓㈡敱娣囧﹪骞栨担鑲濄劑鏌曡箛濠冾潑婵炲吋姊圭换婵堝枈濡椿娼戦梺鎼炲妽婵炲﹤鐣烽姀锝庢▌閻犱警鍨堕弻宥堫檨闁告挻姘ㄩ幑銏犫攽鐎ｎ偄浠洪梻鍌氱墛閸掆偓婵炴垯鍨洪悡鏇㈡倵閿濆骸浜濋悘蹇庡嵆閺屾洟宕惰椤忣厽顨ラ悙杈捐€跨€规洩绻濋幃娆撳礂閸涘﹤浜濈紓鍌氬€搁崐鐑芥嚄閼稿灚鍙忛柛顭戝亞缁犳儳鈹戦悩鍙夋悙闁藉啰鍠栭弻锝夊棘閸喗鍊梺绋匡功缁垱绌辨繝鍥舵晬婵犻潧娴傛禒鈺傜箾鐎电鞋闁稿鍔楀Σ鎰板箳濡や礁鈧攱銇勯幒宥堝厡闁告埊绱曠槐鎾存媴閸撳弶笑缂傚倸绉撮敃銈夘敋閿濆棛绡€婵﹩鍎甸埡鍛厪濠㈣泛鐗嗛悘顏呫亜椤愩垻孝闁宠鍨块、娆戠驳鐎ｎ偆鏆ユ繝纰樻閸嬪懘鎮烽埡鍛偓渚€骞樼拠鎻掕€垮┑鐐叉閸ㄥ綊鏁嶅┑鍥╃閺夊牆澧界€佃偐绱掗鐣屾噰鐎规洦鍋婇幖褰掑捶椤撶媴绱查梺鑽ゅТ濞诧箒銇愰崘顔兼辈婵炲棙鍔戞禍婊勩亜閹捐泛浠︾€瑰憡绻堥弻鐔割槹鎼粹寬銏ゆ婢舵劖鍊甸柨婵嗛娴滅偤鏌嶈閸撴瑩鈥﹀畡閭︽綎闁惧繗顫夐崰鍡涙煕閺囥劌浜炴い锔哄妼閳规垿鎮欓崣澶婃濠电偠灏欓崰鏍Υ娓氣偓瀵噣宕奸悢灏佸亾閻戣姤鐓熼柟瀵稿€戝璺虹婵炲樊浜濋埛鎴︽煕濞戞﹫鍔熼柍钘夘樀閺屻劑寮村Ο琛″亾閺嶎厼绠為柕濞炬櫅閻愬﹪鏌嶉崫鍕殶闁挎稒绮嶇换婵嬪閿濆棛銆愰梺鎸庢穿缁犳捇寮鍜佸悑濠㈣泛顑囬崢闈浳旈悩闈涗粶闁诲繑绻堥幃姗€鍩￠崒妯圭盎濡炪倖鍔х徊璺ㄧ不閺嶃劎绠鹃柣鎾虫捣缁犺鈹戦埄鍐╁€愬┑锛勬焿椤︽煡姊洪幏宀勫摵缂佺粯绻堥幃浠嬫濞戞鎽嬮梻浣筋潐濡炴寧绂嶉悙鍨潟闁绘劕顕悷褰掓煃瑜滈崜婵單涢姀銈嗏拺闁革富鍘兼禍鐐繆閻愬弶鍋ョ€殿噮鍋婂畷鎺楁倷閼碱剟鐛撻梻浣稿閸嬪懐绮欓幋鐐扮箚閻犳亽鍔庣壕濂告煟濡灝鍚归柣锝嗘そ閺岋紕浠﹂崜褎鍒涢梺璇″枓閺呯娀銆侀弴銏狀潊闁绘﹩鍋呯€氱喐绻濋悽闈浶為柛銊ヮ煼楠炲﹪骞樺ú缁樻櫈婵犮垼娉涢敃锕傚汲?
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
        // WARP 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪箻閸┾偓妞ゆ帊鑳堕埢鎾绘煛閸涱喚绠橀柛鎺撳笒閳诲酣骞樺畷鍥跺敽婵犲痉鏉库偓鎰板磻閹惧墎纾奸柍褜鍓氬鍕偓锝傛櫇缁犳艾顪冮妶鍡欏缂佺粯鍔栭〃銉ㄧ疀閹句胶鎳撻…銊╁礃閳哄嫮顢呴梻渚€鈧偛鑻晶顔剧磼閻樿尙效鐎规洘娲熼弻鍡楃暤閵夈儲鍠樻い銏＄☉椤劑宕熼銏犵闂傚倷绶氶埀顒傚仜閼活垱鏅剁€涙ɑ鍙忓┑鐘插暞閵囨繄鈧娲﹂崑濠傜暦閻旂⒈鏁嗛柛灞久禍瑙勭節閻㈤潧浠滈柣掳鍔庨崚鎺撴償閵娿儳锛涢梺缁樻煥閹碱偊宕瑰┑瀣叆婵犻潧妫欓崯鎺楁煛閸愩劎澧曠紒鈧崘鈹夸簻闊洦鎸婚敍鏃傜磼?Sniroute 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿垂妤ｅ啫绠涘ù锝呮贡缁嬩胶绱撻崒姘偓鐑芥倿閿曚焦鎳岄梻浣告啞閻熴儳鎹㈤幒妤€鐒垫い鎺戝枤濞兼劖绻涢崣澶屽ⅹ閻撱倝鏌曟繛鐐珔缁绢厸鍋撴繝娈垮枟椤牓宕洪弽顓熷亗婵炲棙鍨圭壕钘壝归敐澶嬫锭闁诲繆鏅犻弻锛勨偓锝庡亞閵嗘帞绱掓潏銊ユ诞闁诡噮浜滈埢搴ょ疀閺傛妲遍梻浣虹帛閹告悂宕幘顔艰摕闁哄洨鍠庣欢鐐寸箾閹寸偞鐨戦柡鍡欏У缁绘繈鍩涢埀顒備沪閻ｅ奔妗撻梻浣告惈閻鎹㈠┑鍡欐殾闁告鍊ｉ悢鍏煎殥闁靛牆鎳庡▓銉モ攽閿涘嫬浜奸柛濞垮€濆畷銏ゆ偂鎼搭喚鍔烽梺璺ㄥ枔婵绮婚悩宕囩瘈濠电姴鍊绘晶娑㈡煕鐎ｃ劌濮傛慨濠傤煼瀹曞ジ鎮㈢悰鈩冪亖闂備礁鎲￠懝楣冨磹閸噮娼栫紓浣股戞刊鎾偡濞嗗繐顏╁ù鐘櫊濮婅櫣绮欏▎鍓у姼闂佺锕ョ换鍌炴偩閻戣姤鏅插璺猴功椤︺劑姊洪棃娴ゆ盯宕熼澶婃櫗闂傚倸鍊烽悞锔锯偓绗涘懏宕查柛宀€鍊涢崶銊ヮ嚤闁哄鍨归弻褔姊洪崫鍕潶闁稿孩濞婂畷锝夊箻缂佹鍘遍梺闈涱檧缁茶姤淇婇悾宀€纾奸柍褜鍓熷畷濂告偄缂堢姷鐩庨梻浣瑰濡線濡撮埀顒勬煕鐎ｎ偅宕岄柛鈺嬬節閹娊鎯囨俊濉?YouTube/Play 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲梺姹囧€ら崰妤呭Φ閸曨垰鍐€妞ゆ劦婢€缁爼姊洪幖鐐插闁稿﹤娼″璇测槈濠婂孩歇婵＄偑鍊戦崝宀勫箠閹捐埖宕叉慨姗嗗厴閺€浠嬫煟濮楀棗鏋涢柣蹇涗憾閺屾盯鍩￠崒婊冣拰閻庤娲樼换鍫濐嚕娴犲鏁囬柣鎰暜缁遍亶姊虹拠鎻掑毐缂傚秴妫濆畷鏉课旈崨顔间簵闂佸搫娲ㄩ崰鍡樼濠婂牊鐓欏ù鐓庣摠濞懷兠瑰鍕煀妞ゎ叀鍎婚ˇ鍐差熆閻熺増顥犳俊鍙夊姍楠炴帒螖閳ь剟鎮為崹顐犱簻闁圭儤鍨甸鈺傤殽閻愵亜鐏紒缁樼洴楠炲鎮欓崹顐㈡珮闂?
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

    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫆闁芥ê顦純鏇㈡⒒娴ｅ憡鍟為柛鏃撶畵瀹曚即寮介銏╂婵犵數濮电喊宥夋偂濞嗘挻鐓欓悷娆忓婵箓鏌涢妶鍡欐噮缂佽鲸甯￠幃鈺呮嚑椤掑倐銊╂⒑鐠団€虫灍妞ゃ劌鎳橀崺銉﹀緞婵炵偓鐎洪梺鎯ф禋閸嬪嫰鎮橀埡鍐＜妞ゆ棁鍋愭晶銏ゆ煃瑜滈崜銊х礊閸℃稑绐楁俊銈呮噺閸嬪倿鏌￠崶銉ョ仼缂佺姾顫夌换娑㈡晲鎼粹€愁潻闂佽娴氭禍顏堝箖濡法鐤€闁靛／鍛闂備礁鎼惉濂稿窗閺嶎厹鈧礁鈽夊鍡樺兊濡炪倖甯婇悞锕傤敊閹烘鐓熼幖娣€ゅ鎰箾閸欏鐭掔€规洑鍗冲浠嬵敇濠ф儳浜惧ù锝呭濞笺劑鏌嶈閸撶喖鐛崘顭戠叆闁割偅绻傞懓鍨攽閳藉棗鐏ョ€规洟娼у嵄婵せ鍋撴慨濠傤煼瀹曟帒顫濋钘変壕濡炲娴烽惌鍡椼€掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极瀹ュ鐓熼柟閭﹀灠閻ㄦ椽鏌＄仦鐣屽ⅵ婵﹨娅ｅ☉鐢稿川椤斾勘鈧劕顪冮妶搴′簼婵炶尙鍠栧畷娲焵椤掍降浜滈柟杈剧稻绾墎绱掗幇顓熲拹妞ゃ劊鍎甸幃娆忣啅椤旂厧澹庨梻浣芥〃缁€渚€宕愰崸妤€绠犻柣鎰惈鍞梺鎸庢⒐閸庡啿顕欓悜妯肩瘈闁汇垽娼ф牎濡炪倖姊归悧鐘茬暦閺夎鏃堝川椤旇姤鐝ㄩ梻浣藉吹閸犳劗鍒掑鍜佺唵婵せ鍋撻挊婵嬫煕濡ゅ啫鈧綁鏁愭径濠勭杸濡炪倖鍨熼弲婊兾涢崘銊ф殾闁靛ň鏅╅弫濠囨煙椤栧棗鍟伴鎺旂磽閸屾艾鈧兘鎳楅崼鏇熸優閹兼番鍔岄崒銊モ攽閸屾粠鐒剧紒鐘靛█楠炴牗娼忛崜褏蓱濡炪値鍋呭ú鐔煎蓟閵娾晛妫橀柛顭戝枓閹稿啰绱掗悙顒€鍔ら柛姘儐缁岃鲸绻濋崶顬囨煕鐏炲墽鐓瑙勬礃娣囧﹪鎮欓鍕ㄥ亾瑜忕划濠氬箻閹颁胶鍔烽悷婊勬楠炲啯銈ｉ崘鈺佲偓濠氭煠閹帒鍔滄繛鍛矋缁绘繈濮€閿濆懐鍘柦鍐憾閺岋繝宕担绋款潻闂佸疇顫夐崹鍧楀箖閳哄啯瀚氱憸宥嗗閹扮増鈷戦柛婵勫劚閺嗛亶鏌￠崨顖毿㈤柣锝囧厴閹晝鎷犻幓鎺戞暏婵＄偑鍊栭幐楣冨疮閸ф绠繛宸簼閳锋垿鏌涢敂璇插季闁绘帞鍋撶换娑氣偓鐢殿焾閸樻挳鏌涢埞鍨姕鐎垫澘瀚伴獮鍥敇閻樻彃绠诲┑鐘垫暩閸嬬姷浜搁鍫晪鐟滄棃骞冨鈧俊鐑藉煛閸屾粌甯鹃梻浣规偠閸庮垶宕濈仦鐐弿閻忕偘鍕樻禍婊堟煥閺囨浜剧紓鍌氱Т閿曨亜顕ｆ繝姘亜缁炬媽椴搁弲鈺呮⒑閻撳寒娼熼柛濠冾殜閹繝骞樼€靛摜鐦堢紒鍓у閿氬褎鐓￠弻鐔碱敊閻撳簶鍋撻崹顕呭殨妞ゆ劧绠戠壕鍏肩箾閹寸偛鍤繛鏉戝閺岋絾鎯旈婊呅ｉ梺绋款儐鐢繝宕洪埀顒併亜閹哄秷鍏屽褍鐏氶幈銊︾節閸曨厼绗￠梺鐟板槻閹虫ê鐣烽妸锔剧瘈闁告劑鍔屾竟宥囩磽閸屾艾鈧绮堟笟鈧棟闁哄顑欓悞鑺ョ箾閸℃ê鐏ョ€殿喗鐓″缁樻媴閻熼偊鍤嬪┑鐐村絻缁夌懓鐣烽幋锕€绠婚悹鍥皺椤㈠懘姊绘笟鍥у缂佸鏁婚幃鈥斥枎閹惧磭楠囬梺鍓插亝缁诲倿鍩涢弮鍫熺厽闁挎繂鐗呴崥顐ょ磼鏉堛劌娴柛鈹惧亾濡炪倖甯掔€氼剛绮堢€ｎ偁浜滈柡宥冨妿閵嗘帡鏌涘Ο鍝勮埞闁宠鍨块幃鈺冪磼濡鏁繝纰樻閸嬪懘銆冩繝鍥モ偓渚€寮崒妤€浜炬繛鎴烆仾閼测晜宕查柛鈩冪⊕閻撴盯鎮橀悙棰濆殭闁告梹鐟╅弻鐔煎蓟閵壯呮毇闂佸搫鐭夌槐鏇熺閿曞倸绀堢憸澶娢ｉ崼婵愭富闁靛牆楠搁獮鎴︽煕閺冣偓閸ㄧ敻鎮惧畡閭︽建闁逞屽墴閵嗕線寮崼婵嗚€块梺鍝勬川婵兘藝閵娿儮鏀介柣妯虹仛閺嗏晛鈹戦鍛籍妤犵偛绻樺畷姗€顢欓懖鈺婃Ч闂備礁澹婇悡鍫ュ磻閸℃稑绾х憸鐗堝笚閻撶喖骞栧ǎ顒€鐒洪柛鐔风箻閺屾盯鎮╅崘鎻掓懙閻庢鍣崑鍛崲濠靛棭娼╅柕澶堝灩娴滄儳銆掑锝呬壕閻庢鍠曠划娆撱€佸鈧幃銏ゅ传閸曨偅杈堟繝纰夌磿閸嬫垿宕愰弽顓熷亱婵°倕鎳忛崕妤併亜閺冨倹娅曢柣?
    // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻掗埀顒€绠嶉崕閬嵥囬婊呯焼閻庯綆鍋佹禍婊堟煙閹佃櫕娅呴柣鎺斿劋缁绘盯宕楅崗鑲╊槹濠殿喖锕︾划顖炲箯閸涘瓨鎯為柣鐔稿椤愬ジ姊绘担铏瑰笡妞ゃ劌妫濋妴鍐醇閵夈儳顦梺鍦劋閸╁牓宕ョ€ｎ喗鐓曢柕澶堝灪濞呭洭鏌ｉ幘瀛橆棃婵﹥妞藉畷銊︾節鎼淬垻鏆梻浣侯焾椤戝棝宕濆▎蹇ｅ殨闁靛濡囬々鐑芥倵閿濆骸浜為柛姗€浜跺娲濞戣鲸效闂佹悶鍔庨弫濠氱嵁婵犲懐鐤€闁规儳鐡ㄩ惁搴ｇ磽?ASCII闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶闁告挆鍛闂佽瀛╅懝楣兯囨导鏉懳﹂柛鏇ㄥ灠缁犲磭鈧箍鍎卞ú銈嗕繆閽樺鏀介柣鎰级閸ｈ棄鈹戦鎯у幋闁糕斁鍋撳銈嗗坊閸嬫挾绱掗悩鑼х€规洘娲熷畷锟犳倷瀹ュ棛鈽夐柍钘夘樀閿濈偤顢栭挊澶夊?U+00A9(婵? 闂?U+00B7(闂? 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴妤€浜惧銈庝簻閸熸潙鐣疯ぐ鎺濇晪闁告侗鍨版慨娲⒒娴ｄ警娼掗柛鏇炵仛閻ｅ墎绱撴担鎻掍壕婵犮垼鍩栭崝妤呭矗閹剧粯鐓曢柕澶涚到婵¤棄鈹戦婵嗕汗闁瑰弶鎮傞幃褔宕煎┑鍫㈡噯婵°倗濮烽崑鎴﹀垂閻戞鈹嶅┑鐘叉祩閺佸秵鎱ㄥΟ鍧楀摵闁谎傜窔濮婃椽宕崟闈涘壉闂佽崵鍣︽俊鍥箲閵忕姭鏀介悗锝庝憾濞煎﹪姊洪幐搴ｇ畵婵☆偅鐩俊鎾箛閻楀牃鎷洪梺鍦焾鐎涒晝澹曢幖浣圭厱濠电姴鍟扮粻鎾淬亜閺囶亞绉€殿喖顭锋俊鐑藉Ψ瑜滈崯鍥р攽閻愯埖褰х紒鑼舵閿曘垽鏌嗗鍛亶?
    {
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢幘鑼槮闁搞劍绻冮妵鍕冀椤愵澀鏉梺閫炲苯澧柛鐔告綑閻ｇ兘濡歌閸嬫挸鈽夊▍顓т簼缁傚秵娼忛妸褏鐦堥梺姹囧灲濞佳冪摥闂備胶顭堥鍛涘┑瀣畺?+ 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊瑜忛弳锕傛煕椤垵浜濋柛娆忕箻閺岀喖骞嗛弶鍟冩捇鏌涢妶鍛伃闁哄本鐩、鏇㈡晲閸℃瑱绱╅梻渚€鈧偛鑻晶鍓х磼閻樿櫕灏柣锝夋敱缁轰粙宕ㄦ繝鍐╃彸闂備礁鎲￠崜顒勫礋椤撶喓绋侀梻鍌氬€搁崐椋庣矆娓氣偓楠炴牠顢曢妶鍡椾粡濡炪倖鍔х粻鎴犲閸ф绾ч柛顐ｇ濞呭洭鏌ｉ鐐搭棦闁哄本鐩崺鍕礃椤忎礁顫岄梻浣虹帛閹歌煤閻旂厧钃熺€广儱顦悡娑樏归敐鍛暈闁诲繑鎸搁埞鎴︽倷閼碱剙顤€濡炪値鍘煎ú顓㈠箚閳ь剚銇勮箛鎾跺缂佺姵鐓￠弻鐔稿鐎涙ɑ閿┑鐐茬墱閸欏啫顫忛搹鍏夊亾閸︻厼校妞ゃ儱顦伴妵鍕晜閻愵剚姣堥梺缁樹緱閸犳牠锝炲鍫濈劦妞ゆ巻鍋撻柣锝囧厴閺佹捇鎮╅崘娴嬪亾閻戣姤鐓冮弶鐐靛椤﹀嘲顭跨憴鍕闁宠鍨块幃娆撳级閹寸姴濮兼繝鐢靛仧閸樠冾焽閿熺姴鏄ラ柍褜鍓氶妵鍕箣閿濆棛銆婄紓浣瑰姈濮婂綊銆冮妷鈺傚€风€瑰壊鍠栭崜鍫曟⒑鏉炴壆顦﹂柛濠傛健楠炲﹪鎮欓崫鍕庛劑鏌ㄩ弮鈧崕鍐差嚈閻戞绡€闁汇垽娼ф牎濡炪倖姊归悧鐘茬暦閺夎鏃堝川椤旇姤鐝栭梻浣稿暱閹碱偊骞婃惔锝咁棜濠电姵纰嶉悡鏇㈡煃閳轰礁鏆曠紒鍫曚憾閺屸剝鎷呯粙搴撳亾閸噮娼栨繛宸憾閺佸棝鏌嶈閸撴稓鍒掗弮鍫熷仺缂佸娉曢敍娑㈡⒑缂佹◤顏嗗椤撶姷涓嶅Δ锝呭暞閻撴瑩鎮楅悽娈跨劸闁告捇浜堕弻锝夊箳濡ゅ啰鏆梺璇″枟椤ㄥ懘锝炲┑瀣垫晝闁靛繒濮烽妶顕€姊绘担鍛婃儓闁哄牜鍓欑叅闁绘棃顥撻弳锕傛煙椤栫偛浜版俊缁邯濮婄粯绻濇惔鈥斥拻闂佸憡鎸诲畝鎼佺嵁閸儱惟闁挎柨澧介惁鍫ユ⒑閸涘﹦鐭嗛柣鎺炲缁辩偤宕奸妷锕€鈧敻鏌涢…鎴濅簼缂佽埖鐓￠幃妤€顫濋鐔哄嚒闂佷紮绲块崗姗€銆侀弴銏℃櫇闁逞屽墮閺侇噣鏌ｉ悢鍝ョ煀缂佺粯鍔欓幃楣冩倻閽樺鍞堕梺鍝勬川閸熷潡骞忓ú顏呪拺闁绘挸瀵掑鐔兼煕婵犲啯绀冪紒鍌涘浮閺佸啴宕掑☉姘箞闂備胶鎳撻悺銊ф崲閸曨厾鐭撻柧蹇撳帨閸嬫挾鎲撮崟顒傤槰闂佹悶鍔忓▔娑㈩敋閵夆晛绀嬫い鎺嶇閸斿懎鈹戦悙鏉戠仸妞ゃ劌妫濋幆鍫ｇ疀濞戞瑧鍘介柟鑹版彧缁辨洟鎮鹃銏＄厱閹兼番鍔嬮幉楣冩寠濠靛洢浜滈柡宥冨妿閻鏌涜箛鏃傜煉闁哄苯绉烽¨渚€鏌涢幘璺烘瀻妞ゎ偄绻愮叅妞ゅ繐瀚槐鍫曟⒑閸涘﹥澶勯柛鎾寸懅缁絽鈽夐姀鈾€鎷洪梺鍛婄缚閸庡崬鐡梻鍌欑鎼存粎绱炴笟鈧悰顕€宕橀钘夆偓鐑芥倵闂堟稒鎲搁柣锕€鐗嗛埞鎴︻敊閺傘倓绶甸梺鍛婏耿缁犳牕鐣烽姀銈呯濞达絽鍘滈幏铏圭磽娴ｅ壊鍎撻柛搴☆煼閿濈偤鏁冮崒娑氬幈婵犵數濮撮崯顖滅矆閳ь剙顪冮妶鍡樼┛缂佹彃娼￠獮鎴﹀礋椤栨鈺冩喐韫囨稒鏅繝濠傜墛閻撴稑顭跨捄鐚村姛濠⒀勫灴閺屾盯寮幐搴㈠闯缂備緡鍠栭…鐑藉箖濞嗘挻鍊绘俊顖滃帶楠炲秹姊绘担鐟板姢婵炲瓨宀稿畷鎴﹀川椤栨瑧鍓ㄩ梻鍌氱墛閼板€熴亹閹烘挸浜奸梺鍛婁緱閸ㄦ娊寮抽妶澶嬧拺缁绢厼鎳忛悵顏堟煟濡ゅ啫鈻堢€规洘妞介弫鎾绘偐閼碱剦妲伴梻浣哥枃濡椼劑鎮為敃鍌ゆ晜?All rights reserved"
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愭い鏃傛嚀娴滈箖鏌涢幇闈涙灈妤犵偑鍨烘穱濠囨倷鐎涙顦ラ梺鍦拡閸嬪﹪鍨鹃敃鍌毼╅柍杞扮缁愭稑顪冮妶鍛閻庢凹浜濈粩鐔煎即閻愨晜鏂€濡炪倖姊婚悺鏃堟倿娴犲鐓曟俊顖滃Т閸犳岸宕甸弴鐔翠簻闁哄啫鍊婚幗鍌涚箾閸粎鐭欓柡宀嬬秮楠炲洭顢楁担鍙夌亞闂備胶顭堢€涒晛顫忚ぐ鎺嬧偓鍐Ψ閳哄倸鈧兘鏌涘▎蹇ｆЦ闁哄濮撮—鍐Χ閸愩劎浠鹃梺鐑╂櫓閸ㄨ泛顕ｉ锕€绠涢柡澶婄仢閼板灝鈹戦埥鍡楃仯缂侇噮鍨冲▎銏ゎ敃閳垛晜鏂€闂佺粯锚閻忔岸寮抽埡鍛厱閻庯綆鍓欐禒杈┾偓瑙勬礀缂嶅﹤鐣锋總绋垮嵆闁绘劖顔栭崬鍫曟⒒娴ｅ憡鍟炵紒鍝勬健瀹曟洟骞庨懞銉ュ壒闂佺懓澧界划顖炴偂濞嗘垹纾藉ù锝夋涧閻忊晜绻涢崨顕嗚€块柡灞稿墲閹峰懘妫冮埡鍕儓闂備礁鎼惉濂稿窗閺嶎厼绠栫憸鏂跨暦婵傚憡鍋勯柛婵嗗濞堛儵姊婚崒娆戭槮闁硅绻濋幃鐑藉Ψ閳哄倹娅囧銈呯箰閻楀棝鎷戦悢鍏肩厽闁哄啫鍊哥敮鍓佺磼閻樺磭鍙€闁哄瞼鍠愮€佃偐鈧稒蓱闁款厼鈹戦悙鑼闁告梹娲熼崺鐐哄箣閿旇棄鈧兘姊婚崼鐔衡槈濞寸姵锕㈠铏圭磼濡厧鈪归梺闈涚墛閹倿鎮伴鈧畷姗€顢樺鈧Ч妤呮⒑閸︻厼鍔嬮柛銊ョ秺瀹曟劙鎮欓悜妯煎幗闂佺粯顭堝畷鐢垫閻楀牏绠鹃柛婊冨暟缁嬭崵绱掗崒娑樻诞鐎殿喖鐖奸獮瀣敇閻旈妲ｉ梻鍌欑窔濞佳囨偋閸℃あ娑樷枎閹存繍妫滄繝鐢靛У绾板秹鎮￠弴鐔剁箚闁靛牆瀚ˇ锕傛煃缂佹ɑ绀嬮柡?闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛洨绡€闁稿本绮岄·鈧梻浣侯焾椤戞垹鎹㈠┑瀣摕闁哄洢鍨归柋鍥ㄧ節閸偄濮堥弫鍫ユ⒒娴ｈ櫣甯涢柟鍛婃倐瀹曟劙宕烽鐔峰簥濠电娀娼уú銊у姬閳ь剟姊虹粙鎸庢拱闁告垼顫夌粋鎺斺偓娑欘焽缁犻箖鎮楀☉娆樼劷闁活厼顑囩槐鎺楊敊閼恒儱纾抽悗娈垮枦椤曆囧煡婢舵劕顫呴柍鈺佸暞閻濇洟姊洪懡銈呅㈡繛璇х畳閵囨劙宕橀鍡曠瑝闂佹儳绻愬﹢杈╁閽樺褰掓晲閸ャ劌娈屾繝娈垮枛閻楀﹪濡甸崟顖毼╅柕澶涚畱濞堟姊虹化鏇熸澓闁稿孩褰冮銉╁礋椤栨氨鐤€濡炪倖甯掗崑鍡涘疮鐎ｎ喗鐓熼幖娣€ゅ鎰箾閸欏澧柍缁樻閺佸啴宕掑☉娆愮彸闂備胶纭堕崜婵喢哄鍛笉婵鍩栭埛鎴︽煠濞村鏉归柟鍑ょ節閺屾盯骞欓崟顓犳殼闂佽鍠氶崑銈夊箖濞嗘搩鏁勯柛娆嶅劤閻╁酣姊绘担鍛婃儓婵炴潙鍊圭粋宥夋倷閻㈢數鐓嬪銈嗘磵閸嬫捇鏌＄仦鍓ф创闁糕晝鍋ら獮鍡氼槺濠㈣娲栭埞鎴︽晬閸曨偂鏉梺绋匡攻閻楁洟顢欒箛鏃傜瘈婵﹩鍓涢敍娑㈡⒑閻熸澘鈷旂紒顕呭灦閹繝宕橀鐣屽幗闂佺粯鏌ㄩ幉锛勬閺屻儲鐓涢柛鈽嗗幘缁嬬粯銇勯妸锝呭姦闁诡喗鐟╁畷锝嗗緞婵犲啰浜峰┑锛勫亼閸婃垿宕瑰ú顏呭仭闁冲搫鎳庣粻鏍煕閹炬鍊归崟鍐⒑缂佹ê濮﹂柛鎿勭畵瀹?闂?
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
            // 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪Ч濠€渚€姊虹紒妯虹伇婵☆偄瀚板鍛婄瑹閳ь剟寮婚悢鍏尖拻閻庨潧澹婂Σ顕€姊虹粙鑳潶闁告柨閰ｉ獮澶愬箹娴ｅ憡顥濋柣鐘充航閸斿秴鈻撴ィ鍐┾拺婵懓娲ら悘鈺侇熆閻熷府韬€殿噮鍋婂畷鍫曨敆娴ｅ搫骞楅梻浣告惈閸燁偄煤閵忋倕鐒垫い鎴ｆ娴滈箖姊绘担鍛婃儓妞ゆ垵妫濋幃褔鎮╃拠鑼暫濠德板€曢幊鎰暤娓氣偓閺屾盯鈥﹂幋婵囩亪婵犳鍠栭柊锝夊蓟閿濆棙鍎熼柕蹇曞Т濮ｅ牓姊洪崫銉バｇ€光偓閹间胶宓侀煫鍥ㄥ搸娴滃綊鏌熼悜妯虹仼闁告ɑ鎮傚娲川婵犱胶绻侀梺鍝ュУ閻楁绮嬮幒妤婃晬闁绘劖娼欐禒顓炩攽閻樿宸ラ柛鐔告尦瀹曪綁宕熼娑氬幍濡炪倖鏌ｉ崝灞矫洪妶澶婄闁规儼濮ら悡蹇涚叓閸パ嶆敾婵炲懎妫濋弻锟犲椽閸愵亞袦闂佽鍠栭崲鏌モ€﹂妸鈺佸窛妞ゆ挆鍕様闂備焦鐪归崺鍕垂鏉堚晜鏆滈柣鎰版涧閸ㄦ繂鈹戦悩宕囶暡闁哄懏鐓￠弻娑橆煥閳ь剟鎮為敂閿亾闂堟稓鐒搁柡宀嬬稻閹棃顢涘鍛咃絾绻涚€涙鐭嬬紒顔芥崌楠炲棙绗熼埀顒佷繆閹间礁唯闁告劑鍔庣粔娲煙椤旂懓澧查柟顖涙閸╋箓鍩€椤掑嫬纾婚柟鎹愵嚙闁裤倖淇婇妶鍌氫壕闂佽棄鍟伴崰鏍蓟閺囩喓绠鹃柛顭戝枛婵鈹戦埄鍐ㄧ祷闁绘锕﹂幑銏犫槈閵忕姴鑰垮┑鈽嗗灠濞存碍绂嶉鍕殾婵犲﹤鎳愮亸鐢告煙?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佲枙闁绘帟濮ょ换娑㈠幢濡纰嶉柣鐔哥懕缁犳捇骞冨Δ鍛櫜閹肩补鈧尙鏁栭梻浣哥－閹虫挾绮旈悽鍨床婵炴垶锕╅崯鍛亜閺冨洤鍚归柣鈺侀叄濮婃椽宕ㄦ繝浣峰濡炪値鍋勯ˇ閬嶅箲閵忕姭鏀介柛銉㈡櫇閻﹀牓姊虹粙鎸庢拱缁炬澘绉归幃姗€骞樼紒妯锋嫼闂佸憡绺块崕閬嶅几鎼淬劍鐓欓柧蹇ｅ€嬮鍫晪闁挎繂顦柋鍥煛閸モ晛浠遍柛鐐舵硾閳规垿鎮欓弶鎴犱桓闂佹寧纰嶉妵鍕敆閳ь剟藝闂堟侗娼栨繛宸簻娴肩娀鏌涢弴銊ュ⒒婵☆偆鍋ゅ铏圭磼濮楀棙鐣烽梺鍦嚀濞层倝顢?WARP 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊瑜忛弳锕傛煕椤垵浜濋柛娆忕箻閺岋綁濮€閵忊晝鍔哥紓浣插亾濠㈣泛鏈崰鎰扮叓閸ャ劍绀€闁搞劍绻傞埞鎴︽偐鐎圭姴顥濈紓浣哄缂嶄線寮婚妸銉㈡斀闁糕剝顨嗗▓銊モ攽椤旀娼愭い銊ワ工椤繘鎼归崷顓犵厠闂佺硶鍓濋〃鍛搭敂鐎涙绠鹃柛顐ゅ櫏閻掍粙鏌嶈閸撴繈锝炴径鎰濡炲瀛╅鑺ユ叏濡寧纭剧紒鐘崇墱閹叉悂鎮ч崼婵堢懆婵炵鍋愭繛鈧柡灞剧洴瀵挳濡搁妷銉浇闂備胶顭堢€涒晠骞戦崶褜娼?YAML闂?
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
        // winget 闂?闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃秹婀侀梺缁樺灱濡嫮绮婚悩缁樼厵闁硅鍔﹂崵娆戠磼閻橀潧鏋涙慨濠佺矙瀹曞爼顢楅埀顒侇攰闂備礁婀辨晶妤€顭垮Ο鑲╃焼闁告劏鏂傛禍婊堟煛閸愩劍澶勬い銉ヮ槸闇夐柣妯虹湴閸嬨垺鎱ㄦ繝鍐┿仢婵☆偄鍟湁闁靛牆妫涘ú瀵糕偓瑙勬礈閸犳牠銆侀弴銏℃櫜闁糕剝鐟ラ懗鍫曟⒒閸屾艾鈧悂宕愭搴ｇ焼濞撴埃鍋撴鐐差樀閸╁嫰宕橀鍡樺殞婵犲痉鏉库偓鏇㈠箠鎼达絿鐭嗛柛顐ゅ枔缁犻箖鏌熺€涙鎳冮柣蹇ｄ邯閺屾稓鈧急鍐ㄢ拤闂侀潧娲ょ€氫即鐛幒妤€绠ｆ繝鍨姃閻ヮ亪姊绘担铏瑰笡闁圭顭烽幆鍕敍閻愯尪鎽曞┑鐐村灟閸ㄥ湱绮诲☉銏＄厱闊洦鎸婚幉鎼佹煟閳轰線鍙勯柡灞糕偓鎰佸悑濞达絾褰冩禍楣冩煛閸屾ê鍔滈柡鍌楀亾闂傚倷鑳剁划顖炲礉閺嵮岀劷婵炲棙鍨电欢銈囩磽娴ｈ鐒界紒鐘荤畺閹绠涢弮鍌氣叧闂佺顑嗛幑鍥嵁閸ャ劍濯撮柛娑橆煬閸?闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛洨绡€闁稿本绮岄。娲⒑閽樺鏆熼柛鐘崇墵瀵寮撮悢铏诡啎闂佸壊鐓堥崰鏍ㄧ珶閸曨偀鏀介柣鎰级閳绘洖霉濠婂嫮鐭掔€规洘锕㈤崺鈧い鎺嗗亾妞ゎ亜鍟存俊鍫曞幢濡儤娈梻渚€娼荤徊濠氬磿鏉堫偆浜?0闂?1978335189闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢灏佹瀻闁炽儱纾﹢鎾煕閺冣偓閸ㄥ灝鐣峰ú顏勭劦妞ゆ帊闄嶆禍婊堟煙閻戞ê鐏ラ柍褜鍓濆畷鍨珶閺囩喓闄勭紒瀣硶妤犲洭姊洪崜鎻掍簼缂佸鍨舵穱濠勬崉閵娧咃紲闂佺粯锕㈠褎绂掗柆宥嗙厸鐎光偓鐎ｎ剛锛熸繛瀵稿缁犳捇骞婇悙鍝勎ㄩ柕澶涘娴滅兘姊婚崒娆愮グ婵℃ぜ鍔戦幃鐐烘晝閸屾氨顦┑鐐叉缁绘帡寮抽敃鍌涚厽闁哄啫鍊甸幏锟犳煛娴ｅ憡鍠橀柡宀嬬到铻ｉ柛婵嗗濮ｆ劙姊洪崫鍕靛剱闁圭懓娲ら～蹇旂節濮橆剛锛滃┑鐐叉閸╁牆危椤曗偓濮婃椽宕烽娑欏珱闂佺顑呭Λ婵嬪春閳ь剚銇勯幒宥囧妽闁汇劍鍨块弻娑滅疀閺傚棭浜幃鐐槹鎼淬垹鏋傞梺鍛婃处閸嬫帡宕甸妶澶嬧拺闁告繂瀚峰Σ褰掓煕閵堝繒鐣甸柡浣稿暣瀹曟帒鈽夊顒€绠ラ梻浣筋嚙濞寸兘寮幖浣测偓锕傚醇閻斿搫鈪伴梻鍌欐祰椤曆呪偓娑掓櫊閹虫繃銈ｉ崘锝傚亾閿曞倹鍊婚柦妯侯槼閹芥洟姊洪崫鍕窛闁哥姴娴峰▎銏ゆ倷鐎靛摜顔曢梺鐟邦嚟閸嬬姵绔熷鈧弻锝夘敇閻旇櫣鐤勫┑?
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋娆忕労闁轰礁顑嗛妵鍕箻鐠虹儤鐎鹃梺鍛婄懃缁绘劙婀侀梺绋跨箰閸氬绱為幋鐘垫／闁诡垎鍕淮濠殿喖锕ュ浠嬨€佸Δ鍛劦妞ゆ帒鍊婚惌娆忋€掑锝呬壕閻庢鍣崑濠傜暦閸洖鐓涢柛灞剧閻庨箖姊绘担铏瑰笡妞ゃ劌鎳橀幃褔宕卞▎鎴犵暥闂佸湱鍎ゅ鑽ゅ婵傚憡鐓熸繛鍡楃箲閸ｄ粙鏌涢幒鏂款暭闁靛洤瀚伴、鏃堝礋椤愶絾顔嶉梻渚€鈧偛鑻晶鍓х磽瀹ュ懏顥炵紒鍌氱Т椤劑宕奸悢宄板⒕闂備線娼ч…鍫ュ磿瀹ュ牄浜归柟鐑樻尭娴滄鏌熼懖鈺勊夋俊鐙欏洤纾婚柟鎹愵嚙闁裤倖淇婇妶鍌氫壕闂佽棄鍟伴崰鏍蓟閵娾晜鍋勯柛婵嗗珔閵忋垻纾奸柍褜鍓涢埀顒婄秵閸犳鍩涢幒妤佺厱妞ゆ劑鍊曢弸鏃堟煏閸℃ê绗掗棁澶愭煟濮楀棗浜濇繛鍛攻閹便劍绻濋崨顕呬哗闂佸綊顥撴繛鈧柛銊﹀劤閻ｇ兘宕堕敂钘夌厱缂傚倸鍊搁崐宄懊归崶顒€鏄ラ柡宓懏娈曢梺褰掓？閼宠泛鐣垫笟鈧弻娑㈠焺閸愵亖妲堥梺缁樺姇閿曘儵濡甸崟顖氱鐎广儱娴傚Σ顔界箾鐎涙鐭嬬紒顔芥尭椤繘鎼归崷顓狅紲濠碘槅鍨靛▍锝嗙缁嬫娓婚柕鍫濋楠炴牗绻涘顔煎箹妞ゆ洩缍侀獮搴ㄦ嚍閵夈儰姹楃紓鍌氬€烽悞锕佸綔闂佸綊鏀卞钘夘潖濞差亜宸濆┑鐘插濡插牓姊洪幐搴㈢８闁稿﹥绻堥獮鍐ㄎ熺紒妯肩Ф闂侀潧臎閸滀礁鏅梻鍌欑閹诧紕鍒掑畝鍕柈闁规鍠氶惌鎾淬亜閺囨浜鹃梺鍝勭焿缁蹭粙鍩ユ径鎰闁绘劕顕ぐ搴ㄦ煟鎼淬値娼愭繛鍙夌墵閹儵宕楅梻瀵哥畾濠殿喗绻傞惌鍫澪ｉ悜鑺モ拺闁告繂瀚烽崕鎰版煟濡ゅ啫孝妞ゎ偄绻愮叅妞ゅ繐瀚粣娑欑節閻㈤潧孝閻庢凹鍣ｉ幃楣冩偨閸涘﹦鍘介柟鍏肩暘閸ㄥ綊宕甸鍕厱闁规儳顕幊鍥煕閳瑰灝鐏╂い鎾炽偢瀹曘劑顢旈崨顖楀亾婵犳碍鈷戦悷娆忓閸斻倖銇勯弴銊ュ籍鐎规洩缍佸畷鍗烆渻缂佹ɑ鏉搁梻浣虹帛閸旀牕顭囧▎鎾村€堕柨鏂款潟娴滄粍銇勯幘璺轰沪闁哥姵锕㈤弻鐔碱敊閹傛闂佽鍠楃划鎾诲箠閻愬搫唯鐟滃秶鏁Δ鍛厽闁绘柨鎽滈惌瀣磼椤旇姤灏柣锝呭槻椤劑宕橀敐鍡欌偓濠氭⒑瑜版帒浜伴柛锝忕到椤﹪顢氶埀顒€顫忓ú顏勪紶闁告洦鍋呭▓顓㈡⒑缁嬪尅榫氱€规洜鏁搁崚鎺撶節濮橆剚顥濆┑鐐叉婢т粙宕悽鍛娾拺闁稿繐鍚嬮崐鎰箾娴ｅ啿瀚崣蹇涙煕閵夘喖澧柣鎾存礃缁绘盯宕卞Δ鍛椽闂佽　鍋撳┑鐘崇閻撳繘鏌涢妷鎴濆枤娴煎啴鎮楀▓鍨灆缂侇喗鐟︽穱濠囧箹娴ｈ倽鈺呮煏婢舵ê鏋熸い銉ｅ€曢埞鎴︽偐閸偅姣勯梺绋款儑閸嬨倕鐣烽悽鍓叉晜闁割偅绻勯崐鐐烘⒑闂堟丹娑㈠礃閵娧屽晥濠电姷顣槐鏇㈠磻閹达箑纾归柡鍥╁У瀹曟煡鏌熼柇锕€鏋撻柛瀣尭椤繈宕￠悙鎻掔闁诲氦顫夊ú妯好洪悢濂夊殨妞ゆ洍鍋撶€规洖銈搁幃銏ゅ传閸曨偆鐟查梻浣藉吹閸犳劗鍒掑鍥у灊闁规儳纾弳锔戒繆閵堝倸浜惧銈庡亝缁捇宕洪埀顒併亜閹哄棗浜鹃梺浼欑悼閸忔﹢鐛€ｎ喗鏅濋柍褜鍓涚划濠氭嚒閵堝洨锛濇繛杈剧秬椤曟牠鎮炴禒瀣厱婵せ鍋撳ù婊嗘硾椤繘鎼圭憴鍕幑闂佸憡渚楅崰鏍р槈瑜忕槐鎾存媴閸︻厼寮ㄧ紓浣哄У閻楃娀濡存笟鈧顕€宕奸悢铚傛睏缂傚倸鍊烽悞锕傗€﹂崶顒€鍌ㄥù鐘差儐閳锋垿鏌涘┑鍡楊伀鐞氼亪姊洪崨濠冪叆闂佸府缍侀獮鍡楃暆閸曨偆顔婇梺鐟扮摠濮婂綊寮埀?
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
            // 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫇闁逞屽墰婢规洘绻濆顓犲幈闂佹枼鏅涢崯浼村煡婢舵劖鐓曢悘鐐额嚙婵倿鏌＄仦璇测偓鏇㈡箒闁诲函缍嗛崑鍛存偟閺囩儐娓婚柕鍫濋娴滄繃绻涢懠顒€鏋涚€殿喖顭烽幃銏㈡偘閳ュ厖澹曢梺姹囧灪椤旀牠鎮為幆顬″綊鎮╁▎蹇斿闁绘挸鍟扮槐鎾存媴閼测剝鍨块崺娑㈠箳濡や胶鍘撻悷婊勭矒瀹曟粌鈻庨幋鐘辩瑝閻庡箍鍎遍ˇ顖滃閸ф鈷戞い鎺嗗亾缂佸顕划濠氭偐鐠囪尙顔愮紓渚囧枦椤顢撻崱妯圭箚闁圭粯甯炴晶銏ゆ煙閸欏鍊愮€殿噮鍣ｅ畷鐓庘攽婵犲啯鍟?闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柟缁樺笧閳ь剦鍙冨鍝勑ч崶褏浼勫銈庡幖閻楀繒鍒掗懡銈嗗枂闁告洍鏅欑花濠氭⒑鐟欏嫬顥愰柡鍛洴瀹曟繂螣婵傜晫鎳撻…銊╁幢閳哄倻绉风紓鍌欒兌缁垶鎯勯鐐靛祦閻庯綆鍠楅崐鐑芥煕閹般劍娅呴柛婵婃閳ь剚顔栭崰鏍€﹂悜钘夋瀬闁告劦鍠栫壕鍏肩箾閹寸偠澹橀悽顖涱殜濮婄粯绗熼埀顒€顭囪钘濇い鎾卞灩缁€澶屸偓鍏夊亾闁告洦鍓欓崜顓炩攽閻愬弶鈻曞ù婊勭箞閹?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍏煎€绘俊顖濇娴犲ジ姊洪崫鍕棞婵☆偅绻堝濠氭偄閸忓皷鎷婚柣搴ㄦ涧婢瑰﹤危椤曗偓瀵爼鍩￠崒婊呯杽闂佸搫鐬奸崰鏍х暦濡ゅ懏鍤冮柍鍝勫€归鍐⒒娴ｅ憡鍟為柡灞诲姂閹鎳栭埡浣哥亰婵犵數濮甸懝鎯ф暜闂備礁鍟块幖顐﹀磹閹间礁鐒垫い鎺戝€归弳顒勬煛鐏炶濡奸柍瑙勫灴瀹曞崬鈽夐幍浣镐壕婵°倕鎳忛悡鏇熶繆椤栨粎甯涢悘蹇ョ畵閺岋紕浠﹂悾灞濄儲銇勮缁舵岸寮诲☉銏犵缂傚牏濮崇€涒晠姊洪崷顓犳憘闁稿妫涘Σ鎰板箻鐎涙ê顎撳┑顔界箓閼活垶顢旈銏″€甸悷娆忓婢跺嫰鏌涢幘璺烘灈鐎规洘妞介幃娆撳传閸曨収鍚呴梻浣瑰濞插秹宕戦幘缁樼厽闁归偊鍓ㄩ煬顒勬煛鐏炵偓绀冪紒缁樼箚缁犳盯寮撮悩铏啅濠碉紕鍋戦崐鏇犳崲鐎ｎ喖纭€闁告劕妯婂?DoH/nginx 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛洨绡€闁稿本绮岄·鈧梻浣告惈閹峰绂嶅┑鍫熷床婵犻潧顑嗛崑銊╂⒒閸喎鍨佹繛鎴欏灪閻撴洟鎮楅敐搴′簼閻忓繋鍗抽弻鏇㈠炊瑜嶉顒傜磼閻樺磭娲存鐐搭焽閹瑰嫭绗熼娑欑帆闂傚倸鍊风粈渚€骞栭锕€绠悗锝庡枛缁€鍫ユ煟閺冨倸甯剁紒鐘崇墬閹便劌顪冪拠韫闂備浇顕栭崰妤呮偡閳哄懎绠犳繝濠傚暊閺嬫棃鏌熺粙鍨槰婵☆偄瀚板濠氬磼濞嗘帒鍘＄紓渚囧櫘閸ㄦ娊寮鈧獮鎺懳旈埀顒€娲块梻浣哥秺濡法绮堟担铏逛笉闁哄稁鍘介崑鈩冪箾閸℃绠版い蹇ｄ簻閳规垿顢涘☉娆忓攭闂佸搫鏈粙鎴﹀煝鎼淬倗鐤€闁挎繂鏌婅濮婅櫣绱掑Ο鑲╃暫闂佺粯顨堟慨鎾綖韫囨稒鍊绘俊顖濐嚙椤剝绻涚€电甯堕柣掳鍔戦、鎾诲冀椤撶啿鎷虹紓浣割儏鐏忓懘寮ㄩ懡銈囩＜閻犲洩灏欐晶锕傛煙椤旇宓嗘い銏＄懇閺屻劑顢涘顐㈩棜闂備焦瀵х换鍌涱殽閹间礁鍑犻柣鏂垮悑閻撴瑩鎮橀悙鎻掆偓鎼佸礉閿曞倹鐓涢悘鐐插⒔閵嗘帒霉閻欏懐鐣电€规洘绮忛ˇ鎶芥煕閿濆骸娅嶆慨濠傤煼瀹曟帒顫濋崗澶癸妇绱撴担鍓叉Ш闁轰焦鎮傞獮鎴﹀閻橆偅顫嶉梺闈涚箳婵兘顢樺ú顏呪拺缂備焦锚婵牊绻涢崗鑲╂噰鐎殿喖鍟胯灃闁告侗鍠掗幏铏圭磽娴ｅ壊鍎撴繛澶嬫礋瀵即宕掗悙瀵稿幈婵犵數濮撮崯鎵不閹炬番浜滈柡鍥朵簽缁嬭崵绱掔紒妯肩畵妞ゎ偅绻堥、妤呭磼濮橈絽浜鹃柣鎰劋閳锋垿鏌涢幇顒€绾ч柟顖氱墦閺屾盯鎮㈢捄鍝勭ギ闂佽桨绀佺粔褰掑箖濞嗘挻鍊绘俊顖炴敱鐎氬ジ姊绘担钘夊惞闁哥姵鍔栫粩鐔煎幢濞戞顔戦梺缁橆焽缁垶鎮￠悢鍏肩厽闁哄啠鍋撴繛鍏肩懇閹﹢鎳犻钘変壕?
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


// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴濐潟閳ь剙鍊块幐濠冪珶閳哄绉€规洏鍔戝鍫曞箣閻欏懐骞㈤梻鍌欑窔閳ь剛鍋涢懟顖涙櫠閹绢喗鐓涢悘鐐插⒔閵嗘帡鏌嶈閸撱劎绱為崱娑樼；闁告侗鍘鹃弳锔锯偓鍏夊亾闁告洦鍓涢崢鎼佹倵閸忓浜鹃柣搴秵閸撴盯鏁嶉悢鍝ョ閻庣數顭堥鎾斥攽閳ヨ櫕鍠樻鐐茬箻閹晝鎷犻懠顒夊斀闂備礁婀遍崕銈夊春閸繍鐒介柕澶堝劗閺€浠嬫煟濡櫣浠涢柡鍡忔櫊閺屾稓鈧綆鍓欐禒鈺呮煏閸ャ劌濮嶇€规洘锕㈤垾锕傚箣閻樻彃楔闂傚倷鑳堕崑銊╁磿閺屻儱绀堟慨妯挎硾閸氬綊鏌￠崶鈺佹瀭濞存粍绮撻弻鐔兼倻濡櫣浠撮梺閫炲苯澧紓宥咃工椤曪綁宕樺顔惧弳闂佸憡娲嶉弲娆戣姳婵犳碍鈷戦柛婵嗗閳ь剚鎮傞幃妯衡攽鐎ｎ亞鍘遍梺鍝勫暙閻楀﹪鎮￠悢鍝ョ闁瑰鍋熸晶顏堟煕閺冩挾鐣甸柡?sniroute.cmd update check闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶闁告挆鍛闂佽瀛╅懝楣兯囨导鏉懳﹂柛鏇ㄥ灠缁犲磭鈧箍鍎卞Λ顓炍熼崒婊呯＝濞达絿鐡旈崵娆愪繆椤愶絿绠炵€殿喛顕ч埥澶娢熼柨瀣垫綌婵犳鍠楅〃鍛存偋婵犲洤鏋侀柛鎾楀懐锛濋梺绋挎湰濮樸劌鐡繝鐢靛仜濡﹪宕ｉ崘顭戝殨濠电姵纰嶉崑鍕煟閹捐櫕鎹ｉ柣锝夌畺閹嘲顭ㄩ崘顎呪偓娈垮枦濞呮洖顭囪箛娑樜╅柨鏇楀亾鐎殿喖娼″娲箰鎼达絿鐣甸梺鐟板槻椤戝懓鐏掗梺閫炲苯澧存慨濠呮閹风娀鍨鹃搹顐や邯缂傚倸鍊哥粔鎾晝椤忓牊鍋樻い鏇楀亾鐎规洖鐖奸、妤佸緞鐎ｎ偅鐝濋梻鍌欒兌缁垰螞娴ｇ硶鏋嶉柨婵嗩槸閻撴繈鐓崶銊р姇闁抽攱甯掗湁闁挎繂鐗婇鐘绘煏閸℃韬柡宀嬬節瀹曘劑顢欓悙顒€鍨遍梻浣烘嚀绾绢參宕伴弽顓溾偓浣糕槈閵忊€斥偓濠氭煕閵夈垺娅囬柍绗哄€栫换婵嬫偨闂堟稈鏋呭┑鐐板尃閸ヨ埖鏅為梺鍛婁緱閸ㄥ磭浜搁悽纰樺亾楠炲灝鍔氭い锔垮嵆閹繝宕橀鍛瀾濠电姴锕ら悧鍡欑矆閸儲鐓熼柡鍐ㄧ墱濡垿鏌￠崱顓烆洭闁逞屽墰閹虫挾鈧凹鍙冨畷鎶藉级閹炽劍妞介、鏂款吋閸℃ɑ鐝曢梻鍌欑閹测剝绗熷Δ鍛瀭闁规鍠楅弳婊堟煙閸撗呭笡闁抽攱鍨块幃褰掑炊椤忓嫮姣㈢紓浣哄У婵炲﹪寮诲☉姘ｅ亾閿濆骸澧柡瀣〒閳ь剝顫夊ú妯好洪悢鐓庣畺闁伙絽鐬奸惌娆撴偣娓氼垳鍘滅紒?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀辩槐鎺斺偓锝庡亾缁扁晜绻涘顔荤凹闁稿鍔欓幃褰掑炊椤忓嫮姣㈤梺鎸庣⊕閻╊垰顫忓ú顏呭仭闁哄绨遍幐鍐磽娓氬洤娅橀柛銊ㄦ硾閻ｇ兘骞嬮敂鑺ユ闂佺粯锚绾绢參宕㈤幖浣光拺闁硅偐鍋涢崝姗€鏌涢弬鍧楀弰闁靛棗鍊块幊鐘活敆閸屾粣绱插┑鐐舵彧缁蹭粙骞栭锔衡偓鍌涚附閸涘﹦鍘梺鎼炲劀閸愬彞绱旈柣搴㈩問閸ｎ噣宕抽敐澶婃槬闁逞屽墯閵囧嫰骞掗幋婵愪痪闂佺顑呴澶愬蓟閿濆憘鐔兼倻濡攱鐏嗘繝?GitHub API闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶濡わ絽鍟宥夋⒑缁嬫鍎忔い鎴濐樀閹繝顢曢敃鈧悙濠囨煏婵炲灝鍔氶柡瀣懅缁辨捇宕掑姣欙綁鏌ｅΔ鍐ㄢ枅鐎殿喛顕ч埥澶愬閻橀潧濮堕梻浣告啞閸旓附绂嶉弽顬綁宕奸妷锔惧幗闁瑰吋鐣崹鍏肩珶濡眹浜滈柨鏃囶嚙閻忥箓鏌嶉妷顖氼洭闁圭懓瀚版俊鎼佹晝閳ь剛澹曢娑氱闁圭偓娼欓崵顒勬煕閵娿劑鍝烘い銊︾懃閳规垿鎮╅崹顐ｆ瘎闂佺顑嗛惄顖濇濡炪倖鎸堕崹褰掑垂閸屾褰掓偐瀹割喖鍓抽悗瑙勬尫缁舵岸寮婚垾鎰佸悑閹肩补鈧尙鐖遍梻浣侯焾椤戝棝骞愰幖浣哥叀濠㈣泛艌閺嬪秹鏌ц箛锝呬簻闁诲繈鍎甸弻锝夋晲閸パ冨箣閻庤娲滈崰鏍€侀弽顓炵煑闁靛／鍛櫒闂傚倸鍊风粈渚€宕崸妤€绠规い鎰跺瘜閺佸嫰鏌涘☉娆戞殬闁逞屽墮閹虫﹢銆佸Δ鍛妞ゆ帒鍊搁獮宥夋煟鎼达絾鍤€閻庢矮鍗冲畷鎴炵節閸パ咃紵闂佸搫鍊哥花閬嶅绩娴犲鐓冮柕澶堝劤閿涘秹鏌涢埡瀣偧闁逞屽墲椤煤閺嶎厼围闁告稑锕ラ崣蹇涙煃瑜滈崜鐔煎蓟瀹ュ瀵犲璺侯儛濡紕绱撻崒姘毙㈤柣顓炲€搁～蹇旂節濮橆剛锛滃┑鐐叉閸ㄥ灚淇婃禒瀣厽閹艰揪绲块幊妤呮煕濞戝崬鐏ｉ柣鎾存崌濮婅櫣鎲撮崟闈涗还闁诲繒鍋涢崐鍛婃叏閸岀偞鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍暜閹烘纾归柤濮愬€曢ˉ姘攽閸屾粠鐒剧紒鐙欏洦鐓曟い鎰剁悼閳藉鈹戦檱濞夋洜妲愰幒妤佸亹闂傚牊绋愬▽顏勵渻閵堝啫濡搁柛搴ｆ暬楠炲啫鈻庨幘鏉戞濡炪倖鍩堥崐銈夊炊閵娧冨箞闂備礁鐤囬～澶愬磿閾忣偆顩查柣鎰靛厸缁诲棝鏌ｉ幇鍏哥盎闁逞屽墯閻楁粓寮鐣岀瘈闁搞儯鍔屽▓鎴︽⒑閸撴彃浜濇繛鎻掔箻閸┾偓妞ゆ巻鍋撻柟铏耿楠炲啫顭ㄩ崼鐔锋疅闂侀潧锛忛崘銊ヮ洭濠电姷鏁告慨浼村垂婵傜鏄ラ柡宥庡幗閸庡孩銇勯弽銊ュ毈闁诲氦鍩栭妵鍕即濡も偓娴滈箖姊洪崫鍕槵闁告挻绻傞銉╁礋椤掑倻鐦堥梺鍛婃处閸樻粓濡搁埡鍌楁嫼闂佸憡绻傜€氼厼锕㈤幍顔剧＜閻庯綆鍋勯悘鎾煥濠靛牆浠滈柍瑙勫灩閳ь剨绲介悘姘跺疾閵忋倖鈷戦柛娑橈攻缁€鍫熴亜閿濆嫮鐭欑€规洘鍨块獮妯侯熆閸曨剚顥堢€规洘锕㈤崺鈩冩媴閸︻厸鍋撴ィ鍐┾拻濞达絽鎲￠幆鍫熴亜閿斿灝宓嗘い銏＄墪閳诲酣骞橀崘鍙夊劒闂備胶绮弻銊╁触鐎ｎ喗鍋傞柣鏂垮悑閻撳繐顭块懜寰楊亪寮稿▎鎴犵＜闁靛鍊楅惌娆撴煛瀹€鈧崰鏍箖閻戣棄绾ф繛鍡楀綖缁辨柨鈹戦悩娈挎毌闁告挻绻堥獮鍐磼濞戞瑥寮块梺閫炲苯澧撮柡灞诲妼閳藉螣娓氼垯鎮ｆ俊銈囧Х閸嬬偤鏁冮姀鐙€娼栧┑鐘宠壘绾惧吋绻涢崱妯虹仴濠碘€茬矙閺岋絾鎯旈敐鍥ㄥ殑闂佸憡渚楅崹鎶芥偂閹剧粯鈷戦柣鐔煎亰閸ょ喖鏌涚€ｎ剙鏋涙慨濠佺矙瀹曠喖顢涘☉姘箰闂備礁鎲￠崝鎴﹀礉瀹€鍕劦妞ゆ帒鍟崥鍦磼椤旂⒈鐓肩€殿噮鍓欓埢搴ㄥ礈瑜嶉弫鎼佹⒒娴ｇ鎮戦柟顔煎€搁…鍥槼闁逛究鍔戝畷濂稿Ψ閿旀儳骞堥梻渚€娼ч悧鍡涘箠鎼搭煈鏁傛い鎾卞灪閻撴稓鈧厜鍋撻悗锝庡墰閺嗙娀鏌ф导娆戝埌闁靛棙甯掗～婵嬫偂鎼达絼鐢荤紓浣诡殕閸ㄨ埖绌辨繝鍥ч柛銉仢閵夆晜鐓曢悗锝庡亜閻忓鈧娲滈、濠囧Φ閹版澘绠抽柟鎯у閺嗐垽姊婚崒姘偓鐑芥倿閿旈敮鍋撶粭娑樻噺瀹曟煡鏌涘畝鈧崑娑氱不閻樼粯鐓欓梺顓ㄧ畱閺嬨倕鈽夐幘宕囆ч柡宀€鍠撶槐鎺懳熸潪鏉垮灁闂備胶绮弻銊ノ涘☉銏犵疅缂佸顑欓悡銉╂煕椤愶絿绠橀柣鎾村灴濮婃椽宕ㄦ繝鍌毿曢梺鍝ュУ閻楁粎鍒掔拠宸僵闁煎摜鏁搁崢閬嶆⒑閸濆嫭鍌ㄩ柛鏂跨焸閸┾偓妞ゆ帒鍊搁埢鍫ユ煙椤斿搫鐏茬€规洦鍋婂畷鐔碱敃椤愩垺顔勯梻鍌氬€风粈渚€骞栭鈷氭椽濡搁埡浣猴紱濠碘槅鍨靛畷鐢告偟?
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
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣銏犲閺佸﹪鏌″搴″箹缂佹劖顨嗘穱濠囧Χ閸涱収浠鹃梺鐟板暱閺堫剛鎹㈠┑鍡忔灁闁割煈鍠楅悘鍫ユ⒑閹稿孩纾搁柛濠冾殘濡叉劙鎮欑€靛摜鐦堥梺绋挎湰缁秴鈻撴ィ鍐╁€甸柣鐔告緲椤忣亪鏌涢敐鍡樼稇閻撱倖銇勮箛鎾缎ｉ柡鍛矒濮婃椽宕ㄦ繝鍐槱闂佸憡锚閵堟悂鐛Δ鍐ｅ亾閿濆骸鏋熼柍閿嬪笒闇夐柨婵嗘噺閸熺偤鏌涢悢鍝勪槐闁哄矉缍侀幃銈夊磼濠婂憛婊堟倵鐟欏嫭绀冩繛鑼枛閻涱喗鎯旈妸锕€娈熼梺闈涱槶閸庝即鎮界紒妯肩瘈闁汇垽娼ч埢鍫熺箾娴ｅ啿鍚樺☉姘勃閺夌偛灏呯紞浣哥暦婵傜唯闁挎梹鍎抽獮宥夋⒒娴ｈ櫣甯涢柛鏃撶畵瀹曟粌鈻庨幇顓熺彿濡炪倖鐗滈崑鐐烘偂閺囥垹绠归弶鍫濆⒔绾捐法绱掗悩宕囧弨闁哄本娲熷畷鎯邦槻妞ゅ浚鍙冮弻鈩冩媴缁嬫寧娈查梺鐟板槻閹虫ê鐣烽悜绛嬫晣婵犙勫劤娴滄儳鈹戦悩鍙夊闁抽攱鍨块幃褰掑炊閿濆倸浜鹃柧蹇ｅ亞娴滆埖绻濋悽闈浶為柛銊︽そ閺佸啴濮€閵堝懐鐣洪梺璇″瀻閳ь剟寮ㄦ禒瀣厱妞ゆ劧缍€婢规ê霉濠婂牏鐣洪柡灞诲妼閳规垿宕卞☉鎵佸亾濡も偓椤儻顦遍柛妤佸▕瀵鏁愰崨顏咁潔闂佸搫璇為崘銊愭洘绻濆▓鍨灈闁挎洏鍎遍—鍐寠婢跺本娈鹃梺闈涱煭婵″洨寮ч埀顒勬⒑閸涘﹥澶勯柛鎾寸懃閳诲秷顦查柍瑙勫灴閹瑩鎳犻浣瑰枛缂傚倷绶￠崰鏍矓瑜版帒绠栭柣銈庡灱濡嫬螖閻橀潧浠﹂柛銊﹀閹便劑鍩€椤掑嫭鐓熸繛鍡楄嫰娴滄儳鈹戦悙鑼癁闁哄懐濞€瀵濡舵径濠勵啇婵炶揪缍€椤鏌ㄩ鐔虹閻庣數顭堝暩闂佸憡顭嗛崶褏鍘撮梺纭呮彧闂勫嫰宕愰悜鑺ョ厱婵炲棗娴氬Ο鍫熶繆椤愩倕浠х紒杈ㄦ尰閹峰懘鎼归悷鎵偧闂備礁鎲″褰掓偡閵娾晜鍎楀Δ锝呭暞閳锋垿鏌涘☉姗堟缂佸爼浜堕弻娑樜熺喊杈╁姼闁告浜堕弻娑㈩敃閻樻彃濮曢梺缁樻尰閻熲晠寮婚悢纰辨晬闁糕剝顨呴弳銏ゆ煕閳哄啫浠辨慨濠冩そ閺屽懘鎮欓懠璺侯伃婵犫拃灞界仸闁哄矉缍侀崺鈩冪節閸屾粈妗撳┑鐘殿暯閳ь剙纾幗鐘电磼濡ゅ啫鏋涢柛鈹惧亾濡炪倖甯掔€氼喖鐣垫笟鈧幃妤呮晲鎼粹剝鐏嶉梺鍝勬噺缁诲牓寮诲鍫闂佸憡鎸婚惄顖炲春閳ь剚銇勯幒宥嗙グ濠㈣蓱閵囧嫰鏁傜拠鑼桓闂佺偨鍎绘俊鍥箲閸曨垰惟闁挎洍鍋撶痪缁㈠灡缁绘盯骞橀弶鎴濇瘓闂佹悶鍔嬮崡鍐茬暦濠婂牆纭€闁绘劏鏅滈弬鈧梻浣虹帛閸旀洖顕ｉ崼鏇為棷闁革富鍘剧壕鑲╃磽娴ｇ櫢鍏柤鎷屾硶閳ь剚顔栭崰娑㈩敋瑜旈崺銉﹀緞閹邦剦娼婇梺缁橆焽閺佹悂寮堕崨濠佺箚闁绘劦浜滈埀顒佺墱閺侇喗绻濋崟顓犵劶婵炶揪绲跨涵鍫曞几瀹ュ鐓ユ繝闈涙婵吋绻涢幘鎰佺吋闁哄本娲熷畷鐓庘攽閸ヨ埖锛侀梻浣瑰▕閺€閬嶅礉濞嗘挸钃熸繛鎴欏灩缁秹鏌嶈閸撶喖骞冮悜鑺ユ櫆闁伙絽澶囬弨?
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


// ---------------------------------------------------------------- GHA 濠电姷鏁搁崑鐐哄垂閸洖绠伴柟闂寸贰閺佸嫰鏌涢锝囪穿鐟滅増甯掗悙濠囨煃鐟欏嫬鍔ゅù婊堢畺閺岋綁濮€閳藉棗鏅遍梺琛″亾濞寸厧鐡ㄩ悡鐔兼煥閺囨氨鍔嶆い銉ヮ槹閵?
// 闂傚倸鍊风粈渚€骞栭锔绘晞闁告洦鍨遍崵鍕煕椤愶絾绀冮柛瀣ф櫆缁绘繈妫冨☉娆忣槱缂傚倸绉村ú顓㈠蓟閻旂厧绠氱憸宥夊汲鏉堚斁鍋撳▓鍨灕婵炲懏娲樼粚杈ㄧ節閸嬭姤妫冮崺鈧い鎺嗗亾閾荤偤鎮楅棃娑欏暈妞ゎ偅娲熼弻鐔封枔閸喗鐏撶紒鎯у綖缁瑩寮婚垾鎰佸悑閹肩补鈧剚娼鹃梺姹囧焺閸ㄦ澘螞濞嗘劗鈹?+ 缂傚倸鍊烽懗鍫曟惞鎼淬劌鐭楅幖娣妼缁愭鏌″搴″箺闁?闂傚倸鍊风粈渚€骞夐敓鐘冲仭闁挎洖鍊归崑瀣繆閵堝懎鏆熼柣顓熸崌閺岋綁骞嬮敐鍛呮捇鏌￠崪浣稿闁逞屽墮閸樻粓宕戦幘缁樼厓鐟滄粓宕滈悢椋庢殾婵炲棙鎸婚崵宥夋煏婢诡垰鏈粊顐︽⒒娴ｇ懓顕滅紒璇插€哥叅闁靛ň鏅涢崹鍌炴煙閹澘袚闁绘挻娲熼弻鐔兼倻濡鏆楅梺瀹犳椤﹁京妲愰幘璇茬＜婵炴垶鐟ョ壕鎶芥⒑鏉炴壆顦︾紒澶屾嚀閻ｇ兘鎮℃惔顔惧弳闂佸憡渚楅崹閬嶅窗閹烘鈷掗柛灞捐壘閳ь剛顭堢叅闁绘顕х壕濠氭煙閸撗呭笡闁?.rc 闂傚倷娴囧畷鍨叏瀹ュ绀冩い顓熷灣閻ヮ亞绱撻崒娆戣窗闁绘挸鐗撳畷鎶芥晲婢跺﹥鐎悗骞垮劚濡娆㈤悙鐑樼厱妞ゆ劧缍€婢规ê霉?
// Enter = 缂傚倸鍊烽懗鍫曟惞鎼淬劌鐭楅幖娣妼缁愭鏌″搴″箺闁稿鏅涜灃闁挎繂鎳庨弳鐐烘煛閸滀礁澧撮柟顔筋殔閳藉螣閻撳簶鍋撻惂?= 闂傚倸鍊风粈渚€骞夐敓鐘冲仭闁挎洖鍊归崑瀣繆閵堝懎鏆熼柣顓熸崌閺岋綁骞嬮敐鍡╂閺?
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

// 闂傚倸鍊风粈渚€骞夐敓鐘冲殞闁绘劦鍓﹀▓鑺ユ叏濠靛棜顫﹀ù婊冪秺閹妫冨☉娆忔殘閻?gha fetch闂傚倸鍊烽悞锔锯偓绗涘懐鐭欓柟杈剧畱鐎氬銇勯幒宥嗩樂缂佽妫濋弻鐔兼⒒鐎靛壊妲柣搴㈣壘椤︾敻寮诲鍫闂佸憡鎸鹃崰搴ㄦ偩閻戣棄唯闁挎棁妫勯崝鍛攽閻樼粯娑ч柣妤佺矋鐎靛ジ宕堕浣哄幗闂佺粯鍔曢顓㈠煡婢舵劖鐓熸い鎾寸箚閸忓本绻涢崱鎰伈妞ゃ垺锕㈡慨鈧柣妯活問濡插崬鈹戞幊閸娧呯不閹达附鍊舵繝闈涱儏閸ㄥ倿姊洪鈧粔鐢告偂閺囥垺鐓欓柣鎴炆戦悡銉︺亜閿濆懌鍋㈤柡宀嬬秮婵℃悂鍩￠崒娑㈡暘闂備礁纾划顖氼潖瑜版帞宓佸┑鐘叉处閻?
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
    // WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙５闁逞屽墾缁犳挸鐣锋總绋课ㄦい鏃囧Г濞呭牓姊绘担鍛婂暈闁告棑绠撳畷浼村冀椤愩埄妫滈梺绉嗗嫷娈曢柍閿嬪灦閵囧嫰骞掗崱妞惧缂傚倷绀侀ˇ顖烆敋椤撶姴鍨濆┑鐘宠壘閸愨偓濡炪倖鍔楅崰搴㈢閻愵剦娈介柣鎰皺娴犮垽鏌涢弮鈧畝鎼佸蓟閻旂厧鐭楀瑙勭箥娴滎亪宕洪妷锕€绶為柟閭﹀墻濞煎﹪姊虹紒姗堣€挎繛浣冲嫭鍙忛柨鏃€鍨濈换鍡涙煟閹板吀绨婚柍褜鍓氶悧婊堝极椤曗偓楠炴帡寮崫鍕濠殿喗顭囬崢褎鏅堕幘顔界厵闂佸灝顑呴悘鎾煙椤斿搫鐏茬€规洘顨婇幊鏍煛娴ｇ顩┑鐘垫暩婵即宕规總绋挎槬闁哄稁鍘介弲顏堟⒒娴ｇ瓔鍤欏Δ鐘茬箳瀵板﹦鎹勯妸锔绘綗?0 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛闂佽姤蓱缁诲牓寮婚悢灏佹灁闁割煈鍠楅悘宥夋⒑閸濆嫭顥炵紒顔肩焸閸╃偤骞嬮敂钘変汗闂佸湱绮敮鈺傚閳ь剟姊绘担鍛婂暈妞ゃ劌妫楅悾婵嬪箹娴ｆ瓕鎽曢梺鏂ユ櫅閸燁偆娆㈤悙鐑樺€甸柨婵嗙凹缁ㄨ棄霉濠婂啰鍩ｆ慨濠冩そ瀹曨偊宕熼棃娑樺闂傚倷绀佹惔婊呯礊娴ｅ壊鍤曢悹鍥ф▕閸氬顭跨捄渚剰闁谎冨缁绘繈濮€閿濆棛銆愬銈嗗灥閹冲酣鍩㈠鍫Ь缂備浇椴搁幐濠氬箯閸涙潙绠甸柟鍝勭Т椤ユ碍绻濋悽闈浶為柛銊у帶閳绘柨鈽夐埗鈹惧亾閿曞偆鏁嗛柛鏇ㄥ墮娴滄粓姊虹化鏇炲⒉缂佸甯￠幃鈥斥枎閹扳晙绨婚梺鍝勭Р閸斿酣鍩ユ径瀣ㄤ簻闁靛鍎洪崕鎴犵磼鏉堛劌娴┑鈩冩倐婵＄兘鏁冮埀顒佺閹绢喗鈷戦悹鍥ｂ偓铏亪闂佹悶鍨洪悡锟犲春閵忊剝鍎熼柍閿亾闁哄閰ｉ弻鐔兼倷椤掍礁鍓归梺閫炲苯澧紒澶婂濡叉劙骞樼€涙ê顎撻梺鍛婃尰瑜板啴宕滈弶娆炬富闁靛牆绻掗悾閬嶆煟韫囨梻顣茬紒宀冮哺缁绘繈宕惰閸炲爼姊洪崫鍕窛闁稿鐩幊鎾诲锤濡や讲鎷洪柣搴℃贡婵敻濡撮崘鈺€绻嗛柣鎰綑缁楁碍绻涢懝閭﹀殭妞ゎ偅绮撻崺鈧い鎺嗗亾妞ゎ偄绻掔槐鎺懳熺拠宸偓鎾绘⒑閸涘﹦缂氶柛搴ゆ珪椤ㄦ瑥鈹戦悩鍨毄闁稿锕㈠畷鐘绘偐鐠囨彃绐涘銈嗙墬缁酣宕㈤悩缁樼厽闁绘柨鎽滈惌灞筋熆瑜庨〃鍫ュ极椤曗偓楠炴帒螖娴ｇ硶鍋撻崸妤佺厱妞ゎ厽鍨垫禍鏍瑰鍕煉闁哄瞼鍠栧畷顐﹀礋椤撳鍊濋弻娑㈠棘鐠恒剱褔鏌″畝瀣М濠碘剝鎮傞弫鍐焵椤掑嫭鍊堕柟缁㈠枟閻撴稓鈧厜鍋撻悗锝庡墰琚︽俊銈囧Х閸嬬偤鎮烽埡浣烘殾闁绘梻鈷堥弫鍥煟濡櫣浠涙い顐㈡湰缁?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柤纰卞墻濡查亶鏌ｉ悢鍝ョ煂濠⒀勵殘閺侇噣骞掑Δ鈧崹鍌炴煕瀹€鈧崑鐐烘偂閻旈晲绻嗛柕鍫濆€告禍楣冩⒑缂佹﹩娈旈柨鏇ㄤ簻閻ｇ兘鏁愰崼銏㈡澑濠电偞鍨堕悷褔宕㈤幘缁樷拺闁告稑锕︾粻鎾绘倵濮樼厧寮€规洘濞婂畷鐑筋敇閻樼绱叉俊鐐€栭崝鎴﹀垂缂佹ɑ娅忛梻鍌欐祰椤曟牠宕伴弽顐ょ濠电姴鍊婚弳锔姐亜閹板墎鐣辩紒鈧€ｎ偁浜滈柟鍝勭Ф椤︼箓鏌涢幘鑸垫毈婵﹤鎼晥闁搞儜鍛Ф闂備礁澹婇悡鍫ュ礈閻旈鏆﹂柡鍥ュ灪閸嬨劎绱掔€ｎ厼鍔甸柟鑺ユ礋濮婅櫣绱掑Ο鐑╂嫽闂佺绨洪崐鏇㈡偩闁垮绶為柟閭﹀幘閸樿棄鈹戦鏂や緵闁告鏅槐鐐哄箣濠㈡繂缍婂畷妤呮偂鎼粹槅娼曠紓鍌欐祰妞存悂骞戦崶褏鏆﹂柟鐑樺焾濞尖晜鎱ㄩ敐鍡欐噣闁搞劍妞芥俊鐢稿礋椤栨氨顓哄┑鐘茬仛閸旀牜鐟ч梻鍌欐缁鳖喚寰婃禒瀣簥闁哄被鍎埀顒婄畵婵℃悂鍩℃担渚О闂備浇顕栭崹搴ㄥ礋椤愮喎浜炬い鏍仦閳锋帒霉閿濆牜娼愰柛瀣█閺屾稒鎯旈姀掳浠㈤梺璇″枟閿氶柍瑙勫灩閳ь剨缍嗛崑鍛枍閸ヮ剚鈷戠紒瀣濠€鎵磼鐎ｎ偅宕岀€规洏鍨藉Λ鍐ㄢ槈鏉堛劌鐦滈梻渚€娼ч悧鍡椢涘Δ鍛€堕柨鏇炲€归崐鐢电磼濡や胶鈽夐柟铏姍閹矂宕奸妷锔惧幗濠德板€愰崑鎾绘煟濡も偓鐎氼厾绮嬪鍜佺叆闁割偆鍠撻崢鎼佹⒑缁嬫寧婀扮紒瀣灴椤㈡艾顭ㄩ崘鍓у數闁荤姴鎼幖顐︻敂椤愩倗纾奸弶鍫涘妼缁椦囨煃瑜滈崜銊х礊閸℃顩查悹杞扮秿閿濆悿娲敂閸涱垰甯楅柣鐔哥矋缁挸鐣峰鍫澪╃憸蹇曠矆婵犲洦鐓曢柍鈺佸暟閳藉鐥幆褜鐓奸柡灞剧☉閳藉螣閸忓吋鍠栭梻浣瑰▕閺€鍗烆潖婵犳艾鐒垫い鎺嗗亾缂佺姴绉瑰畷鏇㈡焼瀹撱儱娲︾€佃偐鈧稒锚娴滄姊洪棃娑辨濠碘€虫川缁粯銈ｉ崘鈺冨幍闁诲海鏁搁…鍫熺瑜旈弻鐔煎礃閸欏妫炲銈庡弨濞夋洟骞夐幘顔肩闁诡垱澹嬮崑鎾诲锤濡や胶鍘搁悗鍏夊亾闁逞屽墴瀹曚即寮介婧惧亾娴ｇ硶妲堟俊顖炴敱閻庡姊洪悷閭﹀殶濠殿喖鍢查悾鐑藉蓟閵夛妇鍘?闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柤纰卞墻濡查亶鏌ｉ悢鍝ョ煂濠⒀勵殘閺侇噣骞掑Δ鈧崹鍌炴煕瀹€鈧崑鐐烘偂閻旈晲绻嗛柕鍫濆€告禍楣冩⒑缂佹﹩娈旈柨鏇ㄤ簻閻ｇ兘鏁愰崼銏㈡澑濠电偞鍨堕悷褔宕㈤幘缁樷拺闁告稑锕︾粻鎾绘倵濮樼厧寮€规洘濞婂畷鐑筋敇閻樼绱叉俊鐐€栭崝鎴﹀垂缂佹ɑ娅忛梻鍌欐祰椤曟牠宕伴弽顐ょ濠电姴鍊婚弳锔姐亜閹板墎鐣辩紒鈧€ｎ偁浜滈柟鍝勭Ф椤︼箓鏌涢幘鑸垫毈婵﹦绮幏鍛喆閸曨偂鍝楅梻浣侯焾閿曘儵鎮у鍫濇瀬妞ゆ洍鍋撻柛銊╃畺楠炲洦鎷呴崨濠庡悪闂傚倷绀佹竟濠囧磻閸涱劶娲冀椤撶喎浜楀┑鐐村灟閸ㄦ椽鎮￠弴銏＄厵闁逛絻娅曞▍鍛攽閿涘嫬鍔嬬紒缁樼☉閻ｆ繃绻濇笟鈧崑妤呮⒑閸濆嫮鐒跨紓宥勭劍閹便劑鍩€椤掑嫭鐓冮弶鐐村閸忓苯霉閻欌偓閸欏啫顫忛搹瑙勫枂闁告洦鍘介幉娆忊攽閻愬弶鍣烘繛鍙夘焽缁碍娼忛妸褏鐦堥梺鍛婃磸閸斿本绂嶉悙顒傜闁瑰鍋熼幊鎰版煟閹哄秶鐭欓柡宀嬬秮閹垺顦版惔锛般劑姊洪崫鍕缂佸鍏樼瘬濞撴埃鍋撻柡灞剧洴楠炴鈧潧鎲￠崳浼存⒑缁洘鏉洪柛銊ょ矙閻涱喖螣閸忕厧鐝伴悗鐟板閸犳绱為崒鐐粹拻濞撴埃鍋撴繛浣冲洦鍋嬮柛鈩冪☉缁犵娀鏌熼幑鎰靛殐闁哄妞藉濠氬磼濞嗘帒鍘￠梺纭呮珪閹瑰洤鐣烽妷褉鍋撻敐搴℃灍闁稿鏅滈妵鍕疀閹捐泛顣洪梺鎼炲妼閸婂湱鎹㈠☉姗嗗晠妞ゆ棁宕甸崙褰掓⒑缁嬪尅宸ユ繛纭风節瀵鏁愭径瀣珳闂佸憡渚楅崣搴∥ｉ悽鍛娾拺缂佸顑欓崕鎰版煙閸涘﹥鍊愰柛鈹垮灲楠炴鎷犻幓鎺斺偓顓烆渻閵堝棙鈷掗柡鍜佸亝缁傚秹骞囬悧鍫氭嫽?闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛洨绡€闁稿本绮岄·鈧梻浣侯焾椤戞垹妲愰弴銏犵疅闁归棿绀佺猾宥夋煕椤愩倕鏋戞繛鍛矒濮婅櫣绱掑Ο鍝勵潓闂佸湱鈷堥崑鍡欏垝婵犳艾绠柤鎭掑劤閸樼敻姊虹紒妯哄Е闁告挻姘ㄧ槐鐐哄箣閻愵亙绨诲銈嗗姉閸熷潡宕戦幘璇茬疀濞达絿鎳撻惁婊堟⒒娓氣偓濞佳囨偋閸℃あ娑樜旈崘顏嗙暥闂佺鏈划搴ｅ婵傚憡鐓冪憸婊堝礈濮樿泛绠柛娑卞灡閸犲棝鏌涢弴銊ヤ簻濠碘€冲暱閳规垿鎮╅崹顐ｆ瘎婵犳鍠栭悺銊р偓闈涖偢瀹曘劑鎳滈鎸庣潖闂備礁婀遍崕銈夊垂瑜版崵鍥樄婵﹤顭峰畷鎺戭潩椤戣棄浜鹃柤濮愬€栧畷鍙夌節闂堟稓澧愰柛瀣尭閳藉骞掗幘瀵稿絽婵＄偑鍊ゆ禍婊堝疮鐎涙ü绻嗛柤绋跨仛閸庣喖鏌嶉妷锕€绲绘い銉︽皑缁辨捇宕掑▎鎺戝帯婵犳鍠氶弫璇茬暦瑜版帒閿ゆ俊銈傚亾闁诲繑濞婇弻銊╁籍閸喐娈伴梺绋款儐閹瑰洤鐣烽悜绛嬫晣闁绘劖鍔戦崕鐢稿蓟閿濆围闁稿本鐭竟鏇㈡⒒閸屾艾鈧绮堟笟鈧獮鏍敃閿曗偓绾惧綊鏌﹀Ο渚Т闁哄鐗犻弻锟犲炊閵夈儳鍔撮梺?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶濡わ絽鍟宥夋⒑缁嬫鍎忔い鎴濐樀閹繝顢曢敃鈧悙濠囨煏婵炲灝鈧鎹㈡笟鈧鐑樻姜閹殿噮妲柣搴㈢煯閸楀啿顕ｇ拠娴嬫闁靛繒濮堥妸鈺傜叆婵犻潧妫欓崳瑙勩亜椤掆偓椤戝顫忛崫鍕殾闁搞儮鏅涚粭鈥斥攽閳藉棗浜滈柛鐔告綑椤曪絿鎷犲顔兼倯闂佸壊鍋掗崑鍛搭敁閸ヮ剚鈷掑ù锝呮啞閸熺偤鏌熺粙娆剧吋闁诡喚鍏橀崺锟犲川椤撶喐袣闂備線娼х换鍡涘礈濠靛纾块柣鏂垮悑閻撳繐鈹戦悙鑼虎闁告柣鍊濋弻娑㈠籍閳ь剟骞愰崘宸綎婵炲樊浜滃婵嗏攽閻樻彃鈧悂锝炴惔銊︹拺缂佸顑欓崕鎰版煙閻熺増鍠樼€规洖鎼埥澶愬閻樻彃绁梻渚€娼х换鍡楊瀶瑜旈獮蹇撁洪鍛嫼闂佸憡绋戦敃锔剧不閹剧粯鍊垫慨妯煎帶閻忥箓鏌ㄥ┑鍫濅槐闁轰礁鍟村畷鎺戔槈濮橆剙绠為梻浣侯攰閸嬫劗鎮伴妷鈺佺劦妞ゆ帒鍟悵顏呮叏閿濆棗濮嶆慨濠冩そ楠炴劖鎯旈姀顫喘闂備焦鎮堕崝蹇撐涢崟顖涘仼闁绘垼妫勬儫闂佸疇妗ㄩ懗鑸殿殽閸ヮ剚鈷戦柣鐔煎亰閸ょ喎鈹戦锝呭箹閸楅亶鏌熺紒銏犳灍闁抽攱甯掗湁闁挎繂娲ㄩ幗鍌涚箾閸忕厧濮堥柕鍥у瀵噣鍩€椤掑嫭鍋嬮柣妯垮皺閺嗭箓鏌℃径搴殾闁哄啫鐗嗗婵囥亜閺冨牊锛熼柟顕嗙秮濮婂宕掑▎鎴ｇ獥闂佸憡鎸婚悷褏鍒掗弮鍫熷仺闁告稑锕ゆ禍妤€鈹戦悙鏉戠仧闁搞劍妞介幃锟犲即閵忥紕鍘搁梺鎼炲劘閸庨亶鎮橀鍛闁瑰啿鍢插ú锕傛偂閻旇偐鍙撻柛銉ｅ劚閸斻倗绱掗埦鈧崑鎾绘⒒娴ｅ憡鎯堥柣顓烆槺濡叉劙寮撮悢渚祫闂佹寧娲栭崐褰掑吹瀹ュ鐓忓鑸电洴濡绢噣鏌涙繝鍥ㄦ暠濞ｅ洤锕幃娆擃敂閸曘劌浜鹃柡宥庣亹濞差亜鐓涢柛鎰典簽椤︻垱绻涢幘鏉戠劰闁稿鎸鹃埀顒冾潐濞叉ê顪冩禒瀣槬闁逞屽墯閵囧嫰骞掑澶嬵€栨繛瀛樼矋缁捇寮婚悢鍏煎€绘俊顖濇娴犳潙顪冮妶鍛濞存粠浜濠氭晸閻樺弶銇濇繛杈剧悼椤牓锝為锔解拺闂侇偆鍋涢懟顖涙櫠椤曗偓閺屾稑螣閼姐倗鐓夐悗瑙勬礃閸ㄥ潡鐛Ο鑲╃＜婵☆垳鍘ч獮鍫ユ⒒娴ｇ瓔娼愭繝銏∩戦弲鑸垫償閵娿儳顦梺鍝勵槹閿涙洟鎮烽柇锔惧弳闂佸壊鍋侀崹濠氬汲椤愶附鈷戦悹鍥ｂ偓铏亶闂佹寧娲忛崹浠嬬嵁閸愩劉鏋庨柟瀵稿仜绾绢垶鏌℃径灞戒户濠⒀勵殜閹牐绠涘☉娆屾嫽婵炶揪缍€濡嫰宕ヨぐ鎺撶厱闁绘ê鍟挎慨宥団偓瑙勬礃缁诲牆顕ｉ幘顔碱潊闁绘﹢娼у銊モ攽閻橆喖鐏遍柛鈺傜墵瀹曟繈寮撮悩鍏哥瑝缂傚倷鐒﹁彠濞存粍绮撻弻鈥愁吋閸愩劌顬嬮梺缁樻惄閸ㄤ即鈥︾捄銊﹀枂闁告洦鍓涢ˇ銊х磽娴ｈ櫣甯涚紒璇茬墦楠炲啯绂掔€ｎ偒妫冨┑鐐村灦椤ㄥ牓骞戦弴鐔虹瘈缁剧増蓱椤﹪鏌涚€ｎ亜顏い銏″哺椤㈡﹢鎮欏蹇曠М妤犵偛娲鍫曞垂椤旇偐銈梻鍌欒兌缁垰顫忔繝姘ｂ偓锕傚醇濠垫劕娈ㄩ梺绉嗗嫷娈曢柍閿嬪灩閹叉悂鎮ч崼婵堢懖闂佹寧绋撻崰鎾舵閹烘鐒垫い鎺嶇劍婵挳鏌ц箛娑掑亾濞戞氨鎲归梻鍌欒兌閹虫捇鎮洪妸褎顐介柣銈庡灛娴滃綊鏌熼悜妯诲鞍婵炲懏绮撳娲箹閻愭彃濮岄梺鍛婃煥缁夊綊骞冮悽鍓叉晜闁糕剝鐟ч敍婵囩箾鏉堝墽鍒伴柟纰卞亜閺嗏晠姊绘担渚劸缂佺粯鍨块弫鍐閻欌偓閸ゆ洖鈹戦悩宕囶暡闁哄懏鎮傞弻锝呂熼崹顔炬濡炪倧缂氶崡鍐差潖婵犳艾纾兼慨妯哄船椤も偓缂傚倷绀侀鍡涘箰閼姐倗鐭夐柟鐑橆殢閺佸洭鏌ｉ幇銊︽珕闁哄倵鍋撻梻鍌欒兌缁垶鏁冮埡鍛；闁告洦鍓涢々鏌ユ煙椤栧棗鏈€靛矂姊洪棃娑氬婵☆偅鐟ф禍鎼侇敂閸喓鍘遍梺闈涱煭闂勫嫰濡靛┑鍫氬亾鐟欏嫭绀冩繛鍙夌墳閻忓啴姊洪崨濠佺繁闁哥姵宀稿畷銏ゎ敂閸涱垳鐦堥梺闈涢獜缁蹭粙鎮￠幇鐗堢叆婵鍩栭悡鐔肩叓閸ャ儱鍔ら柟顔垮亹缁辨帗娼忛妸銉﹁癁闂佺硶鏂侀崑鎾愁渻閵堝棗绗傜紒鈧担鐑橆偨闁绘劗鍎ら悡娆撴⒑椤撱劎鐣辨鐐达耿閺屾稑顫滈埀顒佺鐠轰警娼栨繛宸簼閸嬶繝鏌ｅΔ鈧悧濠囧磿閹炬剚娓婚柕鍫濇閻忋儵鏌熼搹顐€跨€规洘妞介崺鈧い鎺嶉檷娴滄粓鏌熼崫鍕ら柛鏂跨Ф缁辨帡鍩€椤掑倵鍋撻敐搴℃灍闁绘挻鐟﹂妵鍕即濡も偓娴滃墽绱撴担鍝勑ｉ柣妤冨█閻涱噣宕橀妸銏＄€婚梺鐟扮仢閸燁垶寮查姀銈嗏拺缂佸瀵у﹢鐗堛亜閵娿儲顥㈤柟顔斤耿瀹曟﹢顢欓悾灞藉箺闂佺澹堥幓顏嗘閺囥垺鍋╂い鎾卞灪閻撴洟骞栧ǎ顒€濡洪柟鑼亾椤ㄣ儵鎮欑拠褎鏁鹃梺璇″灡濡啴宕?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋娆忕労闁轰礁顑嗛妵鍕箻鐠虹儤鐎鹃梺鍛婄懃缁绘﹢寮婚埄鍐ㄧ窞閻庯綆浜為崝绋库攽閻愯尙姣為柡鍛█瀵濡搁埡浣虹潉闂佺鏈惌顔界珶閺囥垺鍊甸悷娆忓缁€鈧悗娈垮枛婢у酣骞戦姀鐘闁靛繒濮烽鍝勨攽閻愬弶顥滅紒缁樺笚缁傛帡鎳栭埡鍐紳婵炶揪绲介幖顐ｇ墡濠电姷鏁搁崑娑㈡晝椤忓嫷鍤曞┑鐘宠壘閸楁娊鏌ｉ弮鍫缂佹劗鍋ゅ娲礈閹绘帊绨肩紓浣筋嚙閸婂綊骞堥妸锔藉劅闁靛鑵归幏铏圭磽閸屾瑧鍔嶉拑閬嶆倶韫囷絽寮柡灞剧⊕缁绘繈宕掑☉妯规樊闁诲孩顔栭崰鏍€﹂悜钘夋瀬闁瑰墽绮崑鎰版煠绾板崬澧剧紒杈ㄧ叀濮婄粯绗熼埀顒€顭囪閹囧幢濞戞鐤囬柟鑹版彧缁叉寧绋夊澶嬬厱闁挎棁顕ч獮妯肩棯?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢灏佹瀻闂勫洭顢氳鐓ゆい鎿冩娇閳ь兛绀侀～婊堝焵椤掍焦鍙忛柍褜鍓熼弻锝咁潨閳ь剙顭囪閹?
    // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣銏犲閺佸﹪鏌″搴″箹闂傚偆鍨遍妵鍕冀閵娧呯厐缂備浇缈伴崐婵嬪蓟閿濆鏅查柛娑卞枟閹烽亶鎮楃憴鍕闁绘搫绻濆濠氭偄閸涘﹦绉堕梺璇″瀻閸涱啩鎺楁⒒娴ｈ姤銆冪紒鈧笟鈧畷顖烆敃閿曗偓閻撴﹢鏌熸潏鍓х暠缁炬儳鍚嬬换婵囩節閸屾稒宕抽梺鍛婃⒐閹倿寮婚敐澶嬪亜闁告縿鍎查崵鍌炴⒒閸屾艾顏╅悗姘卞娣囧﹪鎮界粙璺槹濡炪倖鐗楅懝楣冨船閵娾晜鈷戦梻鍫熶緱閻掗箖鏌涢弬璺ㄧ劯閽樼喐绻濋棃娑卞剱闁稿濮甸幈銊ノ熼崫鍕煘闂佸搫顑嗙粙鎺楀箞閵婏妇绡€闁告洦鍘肩粭锛勭磽娴ｈ娈橀柛鐘崇墪閻ｇ兘骞嗛柇锔叫梻浣筋嚙缁绘劙鎮ч幘鎰佹綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顦遍柛妤佸▕瀵鈽夐姀鈥充簻閻庣懓瀚伴崑濠囧礉婢舵劖鍊甸柛顭戝亝缁舵煡鎮楀鐓庢灍缂佸倹甯￠弫鍐磼濮樿京鏆伴柣鐔哥矊缁绘﹢寮鍫㈢杸婵炴垶鐟㈤幏缁樼箾鏉堝墽鎮奸柛搴涘€濆畷鐢稿焵椤掑嫭鈷戦悗鐧搁檮濠㈡ɑ淇婇崸妤佺厱闁圭儤鎸哥粭姘舵煙椤栨稒顥堥柍銉畵楠炲鎮欑€涙褰嗛梻鍌欑閹碱偊藝椤栫偞鍋嬮柛鏇ㄥ灠缁€澶愬箹鏉堝墽鎮煎☉?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢琛″亾閻㈡鐒惧ù鐘欏洦鈷掗柛鏇ㄥ亜椤忣參鏌″畝瀣瘈鐎规洘锕㈡俊鎼佸Ψ閵忕姳澹曢梺鍛婄缚閸庢煡顢?*闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃秹婀侀梺缁樺灱濡嫮澹曟繝姘叆婵犻潧妫Σ鍝ョ棯閹呯Ш闁哄备鈧剚鍚嬮幖绮光偓宕囶啇缂備胶鍋撻崕鍐差焽閿熺姴钃熼柕鍫濇偪閸︻厸鍋撻敐搴′簼闁绘繃娲熼幃?12 缂?*闂?
    // first-run-setup 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙闁稿被鍔庨幉鎼佸籍閸惊銉╂煕閹般劍娅嗛柛搴ｅ枛閺屾洝绠涚€ｎ亞鍔村┑鐐跺皺椤牓鍩為幋锔藉亹閻犲泧鍐х矗闂備胶鎳撻崯璺ㄦ崲閹扮増鍋╅柣鎴ｆ缁犵敻鏌熺憴鍕垫當缂傚秴锕顐﹀箛閺夊灝绐涘銈嗘濞夋洟銆侀崨瀛樷拻濞达絽鎽滈敍宥囩磼椤曞懎鐏犻悡銈夋煟閺冨倸甯堕柦鍐枛閺屻劌鈹戦崱鈺傂﹀┑鈽嗗亝閿曘垽寮诲☉鈶┾偓锕傚箣濠靛洨褰梻浣告惈濡酣宕归懡銈嗩潟闁规儳鐡ㄦ刊鎾煟閻斿憡绶叉い顐㈢Т椤啴濡堕崘銊ヮ瀳闂佹寧娲︽禍婊堫敋閿濆棛绡€婵﹩鍘兼禍婊勪繆閻愬樊鍎忛悗娑掓櫅閳绘挸鈹戠€ｎ偀鎷绘繛鎾磋壘濞层倕顕ｉ崸妤佲拻闁告洦鍋勯顏堟煟閳ュ磭绠绘慨濠傤煼瀹曟帒鈻庨幋顓熜滄繝鐢靛Л閸嬫捇鏌涘Δ鍐ㄤ汗闁哄绉归弻鏇＄疀鐎ｎ亞浼勭紓浣插亾濠电姴浼呰ぐ鎺撴櫜闁告侗鍙庡Λ宀勬⒑閹肩偛鈧牕煤閻斿娼栧┑鐘宠壘闁卞洭鏌ｉ弬鍨暢缂佽绶氬娲川婵犲懎顥濆銈嗗灥椤﹂潧顕ｇ拠娴嬫婵☆垶鏀遍～宥夋⒑閸涘娈橀柛瀣〒閳ь剚鍐荤徊浠嬪煘閹达附鍊烽柟缁樺笚閸婎垱绻濋悽闈涗粶闁挎洏鍊濋敐鐐剁疀閹句焦妞介、鏃堝礋椤撗冩暪闂備胶顢婃竟鍫ュ箵椤忓棗绶ら柛婵嗗珋閿濆閱囬柕澶涜吂閹峰搫鈹戦悙璺鸿敿妞ゆ泦鍕珷妞ゆ洍鍋撻柡灞炬礋瀹曞ジ鎮㈢粙搴撳亾閹邦収娈介柣鎰絻閺嗭絾淇婇銏犳殭闁宠棄顦甸獮鎺楀箼閸愨晝绉鹃梻鍌氬€风欢姘焽瑜忛幑銏ゅ箳閹炬潙寮块梺缁樺灥婢规瑩宕堕澶嬫櫇闂佹寧姊婚弲顐﹀储娴犲鈷戠紓浣光棨椤忓嫷鍤曢柤鎼佹涧缁剁偤鏌涢弴銊ュ箺闁哥姵鍔欓弻鈩冨緞婵犲嫪铏庨梺鍝勬４缁插潡鍩€椤掍緡鍟忛柛鐘愁殙閵囨劙宕橀鑲╁幒闁瑰吋鐣崝宀€绮绘繝姘厸濠㈣泛顑呴悘锝夋煕?up 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佲枙闁绘帟濮ょ换娑㈠幢濡纰嶉柣鐔哥懕缁犳捇骞冨Δ鍛櫜閹肩补鍓濋悘宥夋⒑閹肩偛鈧洟鎳熼婵堜簷闂備焦瀵х换鍌炲箠鎼淬劌姹叉繛鍡樺灩绾惧ジ鏌ｅ▎灞戒壕闂佹悶鍔岄悥鐓庮嚕婵犳碍鍋勯悘蹇庣劍閻忎線姊洪崜鑼帥闁哥姵顨婇幃姗€鍩€椤掑嫭鈷掑ù锝勮閻掔偓銇勯幋婵囶棦妤犵偞鍨垮畷鍫曨敆閳ь剛绮婚弽顓熺厽闁硅揪绲鹃ˉ澶岀磼閻欐瑥娲﹂悡娆戠磼鐎ｎ亞浠㈡い鎺嬪灪閵囧嫰寮▎鐐﹂梺瀹狀潐閸ㄥ潡骞冮埡鍛疀濞达絽鎲″▍娑㈡⒒娴ｅ憡鎲稿┑顔炬暩閸犲﹤顓奸崶銊ュ簥濠电偞鍨崹褰掓倿閸偁浜滈柟鐑樺灥椤忣亪鏌ｉ幘瀵糕槈闂囧鏌ㄥ┑鍡樺櫤闁瑰弶鎮傚顐﹀醇閵夛箑鈧敻鎮峰▎蹇擃仾缂佲偓閸儲鐓曟俊顖滃Т濞层倝鎮″鈧弻鐔告綇閸撗呮殸闂備礁宕ú顓㈠蓟閿濆妫橀柟绋垮閸犳劙姊绘担钘壭撻悽顖涘浮閸╃偤骞嬮敂钘変汗闂佸壊鐓堥崑鍛掗崟顖涒拺缂佸顑欓崕鎴︽煕鐎ｎ偆娲撮柍銉畵瀹曟儼顦柡瀣叄閺岀喖宕归鍏兼闂佸搫鎳愰崑銈咁潖濞差亜宸濆┑鐘插婵洤鈹戦悙鎻掓倯闁荤啙鍛灊濠电姵鑹鹃崘鈧梺闈涱焾閸婃绮诲鑸碘拺闂傚牊绋撴晶鏇㈡煙閸愭煡鍙勭€殿喗濞婇幃銏ゅ礂閼测晛骞堟繝寰锋澘鈧捇鎳楅崼鏇樷偓鍌炲蓟閵夛妇鍘梺鎼炲劀閸愬彞绱旈柣搴㈩問閸犳洜鍒掑▎鎰箚婵繂鐭堝Σ鎯ь渻閵堝棙灏甸柛妯犲洤鐓橀柟杈鹃檮閸婄兘鎮规潪鎷岊劅婵☆偄鐗嗛埞鎴︽倷瀹割喗效闂佺儵鏅╅崹浼存晝閵忊剝濯寸紒顖涙礃閺傗偓闂備胶绮…鍫ヮ敋濠婂喚鍟呴柕澶嗘櫆閸婂灚鎱ㄥΟ鐓庡付濠⒀勫缁辨帡顢氶崨顓炵哗濡炪倧绠戠壕顓犳?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙５闁逞屽墾缁犳挸鐣锋總绋课ㄦい鏃囧Г濞呭牓姊绘担铏广€婇柛鎾寸箞閹兘濡烽埡浣哥€┑鐘绘涧閻楀啴宕戦幘鑽ゅ祦闁割煈鍠栨慨搴♀攽閻愰鍤嬬紒鐘虫尭椤曪綁宕樺顔惧弳闂佸壊鍋嗛崰鎾诲储閹剧粯鍋℃繝濠傚閻帞鈧娲樼划鎾诲箖閵忋倖鍋傞幖娣€栭幉浼存⒒娓氣偓濞佳団€﹂崼銉ョ？妞ゆ柨妲堥敐澶婇唶闁绘柨澧庣粻姘渻閵堝棛澧紒璇插€圭粋宥夋偡閹冲﹤缍婇幃鈩冩償閵忕姷鐛╃紓鍌欐祰妞存悂骞愰懡銈囩當闁绘梻鍘ч悙濠勬喐鎼淬劍鍊跺〒姘ｅ亾婵﹥妞藉畷銊︾節閸愶絾瀚婚梻浣规偠閸斿酣寮繝姘モ偓?5454 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣閻愬灚鍣х紓浣稿閸嬨倝寮诲☉銏犖ㄦい鏃傚帶椤晠姊虹紒妯虹瑨闁诲繑宀告俊鐢稿礋椤栨氨顔婇梺绯曞墲閵囩偞绔熼弴銏＄厽閹兼番鍩勯崯蹇涙煕閻樺磭澧甸柟顔哄劜缁虹晫绮欑捄銊ф毎闂備焦鎮堕崕顖炲礉鎼淬劊鈧懘鎮滈懞銉モ偓鐢告煥濠靛棝顎楀ù婊呭仱閺屾稑螣閸濆嫯鍩為梺瀹狀潐閸ㄥ潡銆佸▎鎴犻┏閻庯綆鍓欐慨娲⒒娴ｅ憡鍟炴い顓炴瀹曟儼顦查柛瀣Ч濮婂搫效閸パ呬患闂佺顕滅换婵嬪箖?444 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫鍐煥閺囨浜鹃梺姹囧€楅崑鎾舵崲濠靛洨绡€闁稿本绮岄。娲⒑閽樺鏆熼柛鐘崇墵瀵寮撮悢铏诡啎闂佸壊鐓堥崰鏍ㄧ珶?502闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢灏佹瀻闂勫洭顢氳鐓ゆい鎿冩娇閳ь兛绀侀～婊堝焵椤掍焦鍙忛柍褜鍓熼弻锝咁潨閳ь剙顭囪閹便劑鏁傞幋鎺旂畾闂佺粯鍔︽禍婊堝焵椤掍胶澧い鏂跨箲缁绘繂顫濋鍌︾幢闂備胶鎳撴晶鐣屽垝椤栫偞鍋傞柡鍥╁亹閺€浠嬫煟濡绲诲ù婊呭仱閺岋綁骞橀悙钘変划闂佸搫鐬奸崰鏍€佸▎鎾村殟闁靛绲奸搹搴繆閵堝洤啸闁稿鐩、鏍ㄥ緞閹邦剛鐣洪梺鍐叉惈閹峰寮崘鈹夸簻妞ゆ劦鍋勯獮鏍ㄣ亜閵夛箑鍝烘慨濠勭帛閹峰懘鎸婃径濠冨劒闂備礁鎽滄慨鐢稿礉閺団懇鈧箓宕归鍛缓闂侀€炲苯澧い鏇秮楠炲酣鎳為妷銉ょ盎闂備焦鍎崇换鎰耿闁秵鏅繝濠傚枤濞撳鏌曢崼婵囶棡闁绘挶鍎甸弻鐔哄枈閸楃偘绨奸梺閫涚┒閸旀垵鐣烽幒鎴旀敠闁诡垎鍌氼棜婵犵數鍋涢悧鍡涙倶濠靛鍑犳繛鎴欏灪閻撶喖鏌″畵顔荤娴狀喖螖閻橀潧浠滈柛鐔风摠閹便劑鍩€椤掑嫭鐓忛柛顐ｇ箖椤ョ娀鏌涘▎蹇旑棦闁哄矉绲鹃幆鏃堫敍濠婂憛锝嗙箾鐎涙鐭嬮柣妤冨Т閻ｇ兘骞嬮敃鈧粻鑽ょ磽娴ｅ顏呯濞差亝鈷戦柛娑橈功閳藉鏌ㄩ弴顏嗙暤鐎规洏鍨介弻鍡楊吋閸″繑瀚奸柣鐔哥矌婢ф鏁幒鎾额洸濞寸厧鐡ㄩ悡鏇㈡煙閹屽殶闁宠棄顦甸弻锟犲幢韫囨梹鐝氬┑顔硷攻濡炰粙骞婇悙鍝勎ㄦい鏃€鍎虫慨娲⒒娴ｅ憡鎲稿┑顔芥尦閺屽﹪鏁愭径濠冩К闂侀潧顧€缁犳垹娆㈤悙鐑樼厵闂侇叏绠戞晶鐗堛亜閺冣偓鐢帡鈥旈崘顔嘉ч柛鈩冪懃椤囨⒑濞茶寮鹃柛鐘愁殘缁晠鎮㈤悡搴″祮闂佺粯妫佸▍锝夋儊閸儲鈷戞慨鐟版搐閻忓弶绻涙担鍐插椤╅攱绻濇繝鍌氼仹缂佽妫欓妵鍕箛閳轰胶浼勯梺缁樺笚閹倿寮诲☉銏犵厴闁诡垎鍌氼棜婵犵绱曢崑鎴﹀磹閺嶎偅鏆滈柟鐑橆殔閻ゎ喗銇勯弽顐沪闁稿鍠愰妵鍕疀閹捐泛顤€闂佺粯鎸鹃崰鏍蓟閺囩喓绡€闊洦娲滈弳鐘绘⒑鐠囪尙绠氶柡鍛箘濡叉劙骞掗幘瀵哥Ф闂侀潧顭堥崕鎶筋敄閸岀偞鈷戦梺顐ゅ仜閼活垱鏅堕崜褏纾界€广儱鎳忛ˉ鐐电磼閸屾氨孝妞ゎ厹鍔戝畷鐓庘攽閸ヮ亞鍙戦梻鍌氬€风欢锟犲礈濞嗘垹鐭撻柣銏犳啞閸嬪倿鏌ｉ弬鍨倯闁绘挶鍎茬换婵嬫濞戞瑯妫￠梺鐟板悑閸旀洟婀佸┑鐘诧工閸燁偊鍩涢弮鍌滅＜閺夊牄鍔屽ù顔锯偓瑙勬礃鐢帡锝炲┑鍠版帒鈻嶇拠鍙夊櫝闁稿鎸鹃幉鎾礋椤掑偆妲瑰┑鐐茬摠缁矂鎮ユ總鍛婂仒妞ゆ洍鍋撶€规洖鐖奸、妤呭焵椤掍礁鍔旈梻鍌欑窔濞佳囁囬銏犵９闁哄洠鎳?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 婵犵數濮烽弫鍛婃叏閻㈠壊鏁婇柡宥庡幖缁愭淇婇妶鍛殲闁哄棙绮嶆穱濠囧Χ閸涱厽娈堕梺娲诲幗閻熲晠寮婚悢鍏煎€绘俊顖炴櫜缁爼姊洪柅鐐茶嫰婢у墽绱掗悩铏碍闁伙綁鏀辩缓鐣岀矙鐠囦勘鍔嶉妵鍕籍閸ヮ灝鎾寸箾閸涱厾孝闁宠鍨块幃鈺呭垂椤愶絾鐦庡┑鐘愁問閸犳岸寮繝姘疇婵犻潧顑呯粈鍫㈡喐韫囨稒鍎楅柛鈩冪⊕閻撴洜鈧厜鍋撻柍褜鍓熷畷鎴︽倷閻戞ê浜楀┑鐐叉閹稿鎮″▎鎰╀簻闁哄秲鍔庨埊鏇犵磼閹澘鐨洪柟顕呭枛椤繈鎳滅喊妯诲闂備礁婀遍…鍫澝归悜钘夌厐闁哄洨鍠嶇换鍡樸亜閹扳晛鐏╅柡鍡悼閳ь剚顔栭崰鏍€﹂柨瀣╃箚闁归棿绀佸敮闂侀潧绻嗗Σ鍛焽閺冣偓缁绘繈鎮介棃娑楃捕濠碘槅鍋呴悷鈺佺暦濠靛柈鏃堝焵椤掑嫬鐓濋柟鎹愵嚙鍥存繝銏ｆ硾椤戝倿骞忓ú顏呯厸濠㈣泛鑻禒鎺楁煕濡も偓閸熷潡鈥栨繝鍥ㄥ€婚柤鎭掑劤閸樿鲸绻濋悽闈浶㈤柛鐔稿缁鎮欓悜妯煎幗濠电偞鍨靛畷顒勫几閵堝鐓冪憸婊堝礈閵娧呯闁糕剝绋戠粣妤佷繆閵堝懏鍣洪柛瀣€块弻鏇熷緞閸℃ɑ鐝曢梺绋匡功閺佸寮婚妸銉㈡斀闁糕剝渚楅埀顒侇殔椤儻顧侀柛銊ョ埣瀵顓兼径瀣弳闁诲函缍嗛崑鍡浰囬妸褏纾藉ù锝堟鐢盯鏌ｉ埡濠傜仸闁绘侗鍠氶埀顒婄秵閸犳宕愰悜鑺ョ厸濠㈣泛顑呴悘鈺呮煥濞戞﹩妲瑰ǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄诞鈧弸鏍煥閺囩偛鈧摜绮堢€ｎ偁浜滈柟鍝勬娴滈箖鎮楀▓鍨灈婵炲皷鈧剚娼栨繛宸簻缁€鍐煕濞嗗浚妲归柛搴㈡崌濮婃椽鎮烽弶鎸庢瘣濠电偛顕崗姗€鐛径鎰妞ゆ棁濮ゅ▍鍡樼節濞堝灝鏋熼柛鏃€甯為懞杈ㄧ節濮橆厸鎷洪梺鍛婄箓鐎氼喛鈪归梻浣告啞閺屻劑鏌婇敐澶婄畾鐎光偓閳ь剟鍩€椤掑﹦绉甸柛鐘愁殜閹€斥槈閵忥紕鍘遍梺鏂ユ櫅閸犳艾鈻撳Ο鑲╃＜闁绘ê鎼崥褰掓煃瑜滈崜姘额敊閺嶎厼绐楅柡宥庡幗閸嬪鈹戦崒婊庣劸闁哄嫨鍎甸弻宥夊煛娴ｅ憡娈叉繝娈垮灡閹告娊骞冨畡鎵虫瀻闊洦鎼╂禒楣冩⒑缁嬪尅韬柡鈧潏鈺傚床婵犻潧妫岄弸鏃堟煕椤垵鏋熼柣蹇撶Ч閺屻倝宕归敃鈧ú锕傛偂閺囥垺鐓涢柛鎰剁到娴滈箖姊洪幖鐐插婵炵》绻濋獮鍐╁閹碱厽鏅╅梺鍏间航閸庨亶宕濋崨瀛樷拺闂傚牊绋撶粻鐐烘煕閵娿儺鐓奸柟顖欑窔瀹曞ジ濡烽敂瑙勫闂備礁鎲＄粙鎴︽晝閿濆洦顐介梺顒€绉甸悡鏇㈡煏婵犲繒鐣遍柍缁樻礃閵囧嫰濮€閿涘嫭鍣伴悗娈垮枟閹歌櫕鎱ㄩ埀顒勬煃闁款垰浜鹃梺鐟板槻閹碱偊鍩為幋锔藉€烽悹娲細閺佸灝顪冮妶鍡樿偁闁告洦鍙庡ú鎼佹⒑鐟欏嫬顥嬪褎顨婇幃锟犲即閵忊€斥偓鐢告煥濠靛棝顎楀ù婊勭箘閳ь剝顫夊ú鏍儗閸岀偛钃?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫇闁逞屽墰婢规洘绻濆顓犲幈闂佹枼鏅涢崯浼村煡婢舵劖鐓曢悘鐐额嚙婵倿鏌″畝瀣М闁轰焦鍔欏畷銊╊敊閼恒儱顏伴梻鍌欑閹碱偄螞濞嗘垵鍨濋柟鎹愵嚙閽冪喓鈧箍鍎遍悧婊冾瀶閵娾晜鈷戦柛娑橈攻鐏忕敻鏌涢悩宕囧ⅹ妞ゆ洩绲剧换婵嗩潩椤戔斁鏅犻弻鏇熷緞閸繄浠惧銈庡亜缁夋挳鍩為幋锕€鐓￠柛鈩冾殘娴犫晠姊虹拠鑼缂佽鐗撻妴渚€寮崼顐ｆ櫍闂侀潧绻嗗褔骞忓ú顏呪拺闁告稑锕︾紓姘舵煕鎼淬劋鎲剧€殿喗濞婇弻鍡楊吋閸℃瑥骞堥梺璇插嚱缂嶅棝宕戦崨顓犳殾鐎光偓閸曨剛鍘搁悗鍏夊亾閻庯綆鍓涜ⅵ闂備浇顕栭崰鎺楀疾濠婂喚鐒介煫鍥ㄧ☉閻撴稑霉閿濆懎顥忛柣搴＄摠缁绘繈鎮介棃娴剁偤鏌涢妸銉吋妤犵偛锕獮妯侯熆閸曨剛鈽夐柍璇查叄楠炴ê鐣烽崶鈺傛緰闂傚倷绶氬褑澧濋梺鍝勬噺缁嬫挾鍒掓繝姘仭闁瑰啿锕ょ紞濠囧极閹版澘宸濇い鏃囨閺嬫垿姊绘担椋庝覆缂傚秮鍋撴俊鐐存綑閹芥粓骞戦姀鐘婵炲棙鍔楃粔鍫曟⒑閸涘﹥瀵欓柛鏇ㄥ亗鏉╂ê鈹戦敍鍕杭闁稿ě鍛筏闁兼祴鏅涢崹婵囩箾閸℃绠抽柡?
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
