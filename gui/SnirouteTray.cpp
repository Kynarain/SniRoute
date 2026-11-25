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

// WARP 闂備礁鎼€氱兘宕规导鏉戠畾濞达絽澹婇崯鍛存煠閻撳骸顣崇紒鐘荤畺閺岀喖骞侀幒鎴濆闂侀€炲苯澧柛濠勬櫕閹广垽骞嬮敃鈧悙?0=闂備礁鎼悧婊勭濠婂牊鍋╅柕濞у嫬鏅?1=闁诲氦顫夐悺鏇犱焊椤忓棙宕叉繛鎴欏灩缁?2=闂備礁鎼悧婊勭閿濆洦宕叉繛鎴欏灩缁犳娊鏌曟径娑氬矝闁?
// 闂?warp-cli status 闂備礁鎲＄敮鍥磹閺囥垺鍋╁Δ锝呭暞閺咁剛鈧厜鍋撻柛鎰剁到娴滈箖鎮橀悙璺轰汗闁荤喐绻堥弻锟犲礋闂堟稓浠┑鐘亾闁告侗鍨崇壕楣冩煙鐎涙绠撴鐐灲閺屻劌鈽夊▎鎴炴儝ASQUE 闂備礁鎲￠〃鍛崲濡ゅ拋鏁婇柛娑卞枤閳绘棃鎮楅敐搴″缂佸缍婇弻娑橆渻閻戔晛浜鹃柧蹇ｅ亯閸氼偅绻涙潏鍓у埌婵☆偅鐟х划濠囨晝閸屾稓鍘掗棅顐㈡处閹稿摜鎷嬮弻銉︾厾闁惧浚鍋勯悘濠勭磼濡も偓椤︾敻寮婚崨顓ф僵闁告挆鍜佹Х
// 闂備礁鎲￠悷顖涚濠靛棴鑰?status 闂?Connected 闂備礁缍婂褏绮旈幎钘夎埞妞ゆ帒鍊归崑姗€鏌曟繛鍨姶闁告艾鍊块弻鐔煎箻椤曞懏顥栧銈呮禋閸撴盯骞忕€ｎ喖围闁告洦鍠氶ˇ浼存⒒閸屾碍鍟氶柛銉畱椤ユ岸姊洪崨濠勬噽闁搞劍濞婂顐﹀Χ婢跺á?
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

// 闂傚倷绶￠崑鍛┍閾忚宕?sniroute.cmd 闂備礁婀遍悷鎶藉幢閳哄倹鏉?warp on/off闂備焦瀵х粙鎴炵附閺冨倻绠斿璺虹灱妞瑰啿顭跨捄鐚村姛缂佹劧绻濋弻銊モ槈濡崵浼恆rp-cli 闂佸搫顦弲婵嬪磻閻愬灚鏆滈柟缁㈠枛閻銇勯弽銊х畵闁告ɑ鎸抽幃鐑藉即濮橀硸妲梺鎼炲€栭崹鍦垝閺冨牆绠抽柣鎰暩椤︻偊姊?
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


// 闂?winget 闂佽娴烽幊鎾凰囬幎鍓?Cloudflare WARP闂備線娼уΛ妤呭磹娴犲鎲橀悗锝庡厴閸嬫挾鎲撮崟鍡樺灴瀵鎮㈢亸浣规暊闂佽婢樻晶搴ｇ矆婢跺ň妲堥柟鎯х－鍟搁梺鎼炲€栫划宀勵敊韫囨稑纭€闁绘劘灏欓埀顒€顭烽弻娑樷枎閹邦剛浠撮梺浼欑祷閸斿酣骞夊Δ鍛窛妞ゆ挾鍋熷Ο渚€姊?
// 濠电偞娼欓崥瀣嚌妤ｅ啯鍤囧┑鐘插暟閳绘棃鏌嶈閸撴瑩鍩?UAC闂備焦瀵х粙鎴濓耿缁楃櫅P 闂佽崵鍠愬ú鎴澝归崶顑﹀绻濋崶褍鐝橀悗骞垮劚閹冲酣锝為妶澶嬬厱婵犲﹤鍟伴悞鎼佹煃閻熸壆效鐎规洘绻堥垾锕傚箳閺冨偆妲撮梻浣瑰缁嬫垿鎳熼鐐村仼闁靛ě鍕櫝闂佸憡渚楅崢楣冨汲濮樿埖鐓曢柟鐑橆殕閺侀亶鏌＄仦鏂よ含闁圭绲介…銊╁礃閵娧屾Ч闂備胶绮…鍫ュ春閺嶎厼鐒垫い鎴濇健濡剧兘鏌?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 濠电偛鐡ㄩ崵搴ㄥ磹濡ゅ啫鍨?CLI闂備焦瀵х粙鎺楁嚌妤ｅ啯鍋╅柣妯哄暱椤曡鲸鎱ㄥ鍡楀閻㈩垬鍎甸幃?winget闂備焦瀵х粙鎴﹀嫉椤掑嫭鍎夐柛娑欐綑鐎?winget 闂佽绻愮换鎰亹婢跺瞼绠旈柛娑卞枟婵?Cloudflare 闂佽姘﹂～澶庢懌闂佸搫鐬奸崰鎾诲焵椤掑倹鍤€妞わ附濞婅棢濠电姴娲ょ粈宀勬煕濞戝崬浜愰柛?
        // 濠电偞鍨堕幐鍝ョ矓闂堟党锝囩矙閸熷啯甯熺粻娑㈠箻閹惰棄褰?Windows 闂傚倷绶￠崰娑欐叏閵堝拑鑰?winget闂備焦瀵х粙鎴濓耿椤㈢癄C闂備線娼уΛ鏃堟嚄閸撲浇濮抽悹鎭掑妿娑撳秹鏌嶈閸撶喖骞冭瀹曠厧鈹戦崼婵堝幀闂佽崵鍠愰悷銈囨閵堝洦顫曟慨妯挎硾杩?
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


// WARP 闂備礁鎼崑鍡涘储閼测晜顫曢柟杈剧畱鐟欙箓骞栧ǎ顒€鐏╅幆鐔兼⒑閸濆嫮澧遍柛鎾寸懃閿?DNS 闂備礁鎼崐浠嬶綖婢舵劑鈧礁顓兼径濠勵唵婵犮垼娉涢敃銉х矆閳ь剟姊洪崫鍕垫Ф闁稿寒鍨堕崺鈧?
//
// 濠电偞鍨堕幑浣割浖閵娧冨灊闁割偆鍠撻埢鏇㈡煕椤愮姴鍔滃ù鐘筹耿閹兘寮村槌栨М闂佷紮绲挎繛鈧柡浣哥Т椤垽骞€婵夋槈 闂備礁鎼€氱兘宕归柆宥呮辈闁挎繂顦€氬绻涘顔荤凹闁伙箑鐗撳娲敆娴ｈ鍎撶紓浣介哺缁诲嫰鍩€椤掑倹鍤€闁哥噥鍨抽弫顕€鍩€椤掑倻纾奸柣鎰靛墮椤曟粍淇婇悙鎻掆偓鍧楀箖娴犲惟闁挎洍鍋撻柣鎾存礈閳ь剙鐏氭竟瀣磻閹惧灈鍋撻崹顐㈩棎闁稿鎸搁湁闁绘﹩鍠栭崝娆撴偣?缂傚倸鍊搁崯顖炲垂閸洖钃熼柛銉ｅ妿椤?闂備焦瀵х粙鎴︽儗娓氣偓閸┾偓妞ゆ巻鍋撻柤娲诲灦閹潡宕熼娑掓寖闂佸憡渚楅崜娑㈡偡閹扮増鐓涢柛顐亜婢ь喗绻濋埀?
// DoH/nginx 闂備礁鎲￠悷顖炲垂閻㈢绀傛慨妞诲亾鐎规洖鐖奸幃鈺傛綇椤愩値妲归梻浣告啞椤ㄥ懎煤閵婏富娈介柛銉墮杩濇繛杈剧到閹芥粓鍩炲☉娆戠瘈鐟滃秹宕妸鈺佺劦妞ゆ垼娉曢崝宥夋煕閵婏絽濡界紒鍌涘笚閹棃濮€閳ュ磭澶勯梻浣告啞閸戝綊宕归悧鍫熷弿闁冲搫鎳忛崵鈧悗鍏夊亾闁逞屽墮椤斿繑娼忛妸銉х獮婵炴挻鑹剧花閬嶅磻閹捐纾兼繝闈涚墢椤旀劙姊绘担鐟扮祷闁兼椿鍨辩粋鎺戔槈濞嗘垟鏋栭梺闈涚墕鐎氼參顢曢挊澹濈懓顭ㄩ崘鎯у妧缂?
//   1) 婵犵妲呴崑鈧柛瀣崌閺?5444(DoH 闂備礁鎲￠幐鍝ョ矓閸洘鍋? / 5454(DoH 闂備礁鎲￠懝鎯归悜绛嬫晜? / 443(nginx) 闂備礁鎼€氱兘宕规导鏉戠畾濞撴埃鍋撻柡灞界墦閹稿﹥寰勭€ｎ厼鎯堥梻?
//   2) 缂傚倸鍊搁崐钘夘浖閵娾晛绠查柕蹇嬪€曠粻娑㈢叓閸パ勬崳妞ゅ繐宕埥澶愬箼閸愌呯泿濠殿喗菧閸斿酣骞戦崟顖氱婵犲﹤鎷嬫导鍥р攽?sniroute.cmd up 闂備胶顢婂▔娑㈡晝閵堝鍋╅柣妯虹－椤╅攱銇勯幒宥囶槮妞ゅ孩鐟╅幃纭咁樄闁衡偓閸楃儐鐒?
// 闂備胶顭堢换鍫ュ礉閹达箑纾块悗鐢告櫜閻掑﹪寮堕崼姘珔鐎规洘娲熼弻娑橆潩椤掑嫷鈧鏌ら弶鎴含妤犵偞鎹囬獮鍥敊鏉炴壆椹抽梻浣瑰缁嬫垿鎯屾笟鈧、娆撳焵椤掑嫭鐓熸い顐墮婵′粙鏌涢埡鍌滄创闁诡垰鐭傛俊鐑藉煛閳ь剚绔?闁诲氦顫夐悺鏇犱焊濞嗘垵鍨濋柨鐔哄Т缁€鍡涙煕閵夋垵瀚ˇ銉╂⒑閸濆嫮澧遍柛鎾寸懃閿?DNS 闂備礁鎼崐浠嬶綖婢舵劑鈧?闂?
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
        // up 濠电偞娼欓崥瀣嚌妤ｅ啫鍌ㄦ繛鎴欏灩绾?闂佽崵濮嶉崶銊︽殸濠电姭鍋撻柛銉墮缁€澶愭煃閵夈儱鏆遍柡鍛懇閺屾稓鈧綆鍋夋竟妯好归悩鑼憼闁诡垱妫冮弻銊р偓锝呯仛閳锋洟姊洪崫鍕潶闁稿氦宕靛Σ鎰攽閸垹宕ュ銈嗙墬閼瑰墽绱為幘鏂ユ闁规儳纾牎濠碘剝褰冮幗婊呭垝閳哄啠鍋撻敐搴濈盎闁绘挻绻堥弻娑滅疀鐎ｎ亜顫囬梺绋块椤曨厽绂嶉崨濠冪秶闁宠桨绀佺壕鍧楁⒑闂堟稒顥滈柛濠佺矙閹潡宕卞▎鎰彴闂侀潧绻嗛崜婵囩濡警鐔?30 缂傚倷绀侀ˇ鎵暜椤忓棙顫?
        // 濠电偛顕慨瀵哥矓瀹曞洨绠斿璺哄閻掑﹪寮堕崼姘珔鐎规洘娲熼幃妯跨疀閹惧疇鍩為梺鍝ュ暱閸嬫捇鏌ｉ悩闈涘妺闁搞劌纾划鈺呭级閹存梹鐎?up闂備焦瀵х粙鎴︽偋婵犲偊鑰块柛銉墮缁€澶愭煃閵夈儱鏆遍悹浣圭叀閺屻倗鈧湱濮甸ˉ澶愭煕閵娧冾暭缂佽桨绮欏畷鎰版偆娴ｇ鏅у┑鐐村灦閹告悂鏁冮敐鍥╁崥闁哄鍨熼弸搴ㄦ煥閻斿搫孝婵炲懌鍨介弻娑橆潩閻撳海浠繝鈷€宥囩暤鐎殿噮鍓熼、姗€濡歌椤︻偊姊?
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
        // up 闂備焦鐪归崝宀€鈧凹鍙冮、妤€顭ㄩ崨顔芥闂佸憡绺块崕鍐测枖閵忋倖鐓涘璺鸿嫰閺嬪孩鎱ㄥΟ绋垮缂佸倹甯￠弫鎰板醇濞戞帒浜鹃柛鎰靛枛鐎氬鈧箍鍎遍幊搴綖閵堝洠鍋撻崷顓х劸鐎殿喛鍩栭崚濠囨惞鐟欏嫬鐨梺鍛婂灟閸婃鐛姀銏㈢＜闁哄啯鍨甸悘鐘垫喐閻楀牏娲存鐐村浮婵偓闁挎稑瀚ㄩ崑鐐烘⒑閸涘﹦鎳愬ù婊呭仱瀵偊濡舵径濠勯獓闂侀潧顦崕閬嶇嵁閹达箑绠归悗娑櫳戠粚璺ㄧ磼鏉堛劎绠栫紒瀣槸椤撳ジ宕奸锝囩厬闂備線娼荤拹鐔煎礉瀹€鍕仼闁绘绮悡鍌溾偓骞垮劚閹冲繘宕?
        // 闂備礁鎼悧鍡欑矓鐎涙ɑ鍙忛柣鏂垮悑閸庡秹鏌涢弴銊ュ箻鐎规洖鐖奸弻娑樷枎濞嗘垹袦闂侀潻缍侀ˉ鎾诲箯閻樻椿鏁嶆慨姗嗗墰娴煎牓姊?婵犵數鍋涢惇鏌ュΧ閸℃妫烽梻?闂?
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


// WARP 闂備礁鎼€氱兘宕规导鏉戠畾濞撴埃鍋撻柟顔垮Г濞煎繘鍩￠崒娑欘吂闂備礁婀遍ˉ鎰€掗崷顓涘亾闂堟稓鐒哥€规洘绻堟俊鎼佸Ψ閿曗偓閸撳姊哄ú缁樺▏闁告柨锕崺鈧い鎺嗗亾闁稿﹤缍婇幃銉╂晲閸ャ劌顎涢梺缁橆殔閻楀﹪鎮楅娴?data\.warp-enabled闂?
//   闂佽瀛╃粙鎺楁晪濠电姭鍋?  -> 闂備焦妞垮鍧楀礉瀹ュ鏄ユ繛鎴炵閺嗘粓鎮橀悙闈涗壕缂?WARP 闁诲孩顔栭崰鎺楀磻閹剧粯鐓熸い鎰枎娴滈箖姊洪幐搴ｂ槈闁绘鎹囧畷鎶藉箹娴ｆ瓕袝濡炪倖鐗楃喊宥夊磹闂堟胆搴ㄥ炊閵娧呯暫闁诲骸鐏氬Λ鍐极?
//   濠电偞鍨堕幐鍝ョ矓閹绢喗鍋ら柕濞炬櫅閹?-> 濠殿喗甯楃粙鎺椻€﹂崼銉晣濠电姵鑹剧粈鍌炴煙閹澘袚闁挎稒妫冮弻銊モ槈濡厧顣规俊銈囧Т濞差厼鐣烽锝囩懝闁逞屽墰閸掓帡顢涘鍕€涢梺闈涚墕濞层倗鐟ч梻浣筋嚃閸忔稓娆㈤妶鍥潟闂侇剙绉寸憴锔姐亜閺嶃劏澹樼悮鐔兼⒑閸濆嫬鏆為柛搴ゅ亹閳ь剚鍝庨崹钘夌暦閿濆绠ｉ柣妯垮皺閻も偓 WARP 闂佸搫顦弲婊堚€﹂崼銏″床婵炴垯鍨洪崕鎾绘煃瑜滈崜娑欑閸涘﹥缍囬柕濠忓閸旂兘鎮楀▓鍨灀闁稿鎹囬弻?
// 闂備焦妞垮鍧楀礉瀹ュ鍋夐柨婵嗘处婵ジ鏌ｅΔ鈧悧濠囨倵椤旀祹褰掓偐閸愭彃濮搁梺閫炲苯澧┑鍌涙⒒缁厽寰勯幇顒€寮峰銈嗙墬缁诲秹宕?JSON闂備焦瀵х粙鎴﹀嫉椤掆偓鍗遍柛娑橈功閳绘棃寮堕悙鏉戭棆闁哥喎锕弻?.autostart-off 濠电儑绲藉ú锔炬崲閸愵亖鍋撻崹顐ｎ棃鐎规洏鍎虫禒锕傛嚃閳哄﹣绱樼紓鍌欑椤︻垳绮旂€涙顩锋繛鎴炃氶弸鏍煛閸ャ剣姘跺磻?
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

// 闂備礁鎲￠崙褰掑垂閻楀牊鍙忛柍鍝勬噹缁秹鏌ら幖浣规锭闁抽攱妫冮幃?濠殿喗甯楃粙鎺椻€﹂崼銉晣濠电姵鑹剧粈鍌炴煙閹澘袚闁?闂備焦瀵х粙鎺戭潩閵娾晛鏋侀柕鍫濐槸缁狅綁姊洪崹顕呭剳婵﹪绠栭弻娑橆潩閻撳海浠梺杞伴檷閸婃牕顭囨繝姘倞闁靛鍨规禍鎯归敐澶樻缂?WARP 闂佸搫顦弲婊堚€﹂崼銏″床婵炴垯鍨洪崕鎾绘煃?-> 闂備礁鎼崑鍡涘储閼测晜顫曢柟杈剧畱杩?
// 闂備礁鎲￠悷顖涚濠婂喛鑰挎い蹇撶墕鐟欙附銇勯弽銊ㄥ鐞氱喖姊洪崫鍕殺婵炲绋掔粋鎺戔槈濞嗘垟鏋栭梺閫炲苯澧柍璇查叄閸┿儵宕ㄩ婊庢Х濠电偞鍨堕幐鍝ョ矓閻㈢妫樺ù锝堫潐娴溿倖绻涢幋鐑嗙劷闁哄棔鍗抽弻锟犲礃椤撶喓銆愬┑鐐叉噺鐢繝寮鍛殕闁告洦浜炵槐姘舵⒑缁嬭法绠扮紒澶嬫尦楠炲繑瀵肩€涙ê浠洪梺闈涱煭缁犳垿鎮￠弴銏＄厵闂傚牊渚楅崕蹇撯攽椤旇娅嗘繛鐓庮煼閹瑩寮堕幋婵愭Щ闂備胶鎳撻悘姘跺箰婵犳碍鍊堕柨鏇炲€歌繚?
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

// ---------------------------------------------------------------- WARP 闂備胶顭堥鍐磿闁秴绠栫€广儱顦婵嗏攽閻樺弶濯奸柛?
// 濠电偞鍨堕幐鍫曞磹濡ゅ拑缍?WARP 濠电偞鍨堕幐鍝ョ矓閻戣棄绀傛慨妞诲亾闁?闂?
//   1) 闂備胶绮…鍫ュ春閺嶎厼鐒垫い鎴炲椤﹂绱?connected 闂備礁鎲￠悷锕佹懌闂?disconnected      -> 缂傚倷鐒﹂弻銊╊敄閸涙潙鏄ラ柕蹇嬪€栭悞缁樻叏濡じ鍚柛娆屽亾
//   2) 闂備胶绮…鍫ュ春閺嶎厼鐒垫い鎴炲缁佺増銇?connected 濠电偠鎻徊钘壩涘Δ鍛惞妞ゆ挶鍨归惌妤呮煥濠靛棙顥滈悹褍鐡ㄧ换娑㈠级閹搭厼鍓紓浣诡殔椤︾敻寮婚崨顓ф僵闁告挆鍜佹П濠电偞鍨堕幐鍫曞垂閸︻厸鍋撻崹顐嗘垿銆?缂傚倷鐒﹀Λ蹇涘垂閹惰棄纾婚柨婵嗘处閸嬫牗銇勯幇鍫曟妞ゅ孩鐟╅弻?
//      -> 闂佸搫顦弲婵嬪磻閻斿吋鍋ㄧ紓浣姑欢鐐哄级閸偄浜悮?2 婵犵數鍋涘Λ宀勫礈濠靛洦娅犻柣妯肩帛閻掔粯鎱ㄥΟ澶稿惈闁告瑢鍋撻梻浣瑰缁嬫垶绺介弮鍌滅當?10 闂備礁鎲＄敮鎺懳涘┑瀣闁规崘顕х粈鍐煕濞戝崬鏋熷ù鐘趁…鍧楁嚋閻㈢數鍙濈紓浣规⒐濞兼瑩鍩㈡惔銊ョ叀闁告侗鍨抽ˇ顕€姊绘笟鍥у闁诲繑绻堝畷銏ゅΧ婢跺﹦顦悗鐟板婢瑰棗顭?
// 闂備焦妞垮鍧楀礉瀹ュ鏄ユ繛鎴炵懅閳绘棃鎮归崶銊ョ祷鐞氱喖姊洪崫鍕簽闁告鍋愬Σ鎰板箣閿曗偓缁?g_warpManualStop=true闂備焦瀵х粙鎴炵附閺冨倻绠斿鑸靛姇缁犮儵鏌熼悙顒佺稇妞ゃ儱绉归弻?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倷绶￠崑鍛┍閾忚宕?WARP 闂備礁鎲￠悷锕傛偋閺冨倻鍗氶悗娑欘焽閳?204 缂傚倷鐒﹀Λ蹇涘垂閹惰棄纾婚柨婵嗩槸杩濋梺绋挎湰缁诲嫮绮婚弻銉︾厱?true 闂佽崵鍋炵粙蹇涘礉鎼淬劌桅婵娉涚粈鍕煕濠靛棗顏╃紓宥嗘崌閺屾稑鈻庡Ο铏逛化闂佽桨闄嶉崐婵囦繆?
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
        if (st == 0) {                     // 婵犵數鍋涢惇鏌ュΧ鎼淬埄妫呴梺璇查缁犲秹宕瑰ú顏勬槬婵炴垶姘ㄩ崡姘亜閺嶃儱鈧绮堟径濞掑綊鎮╅顫濠电偞鍨堕弻銊╂偋閹捐绀傛俊顖濆吹閳绘梻鈧箍鍎遍幊宥囧垝?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂備礁鎼崑鍡涘储閼测晜顫?
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
constexpr wchar_t kAppVersion[] = L"3.15.0";

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
    // 闂備礁鍚嬬€笛呭垝鐏炵晫鏆﹂柣鏃傚帶缁犳澘顭块懜闈涘Е婵炲瓨娲熼幃妤€鈽夊▎妯荤暦闂佷紮闄勭换鍫ョ嵁閹烘绠抽柟瀵稿Х椤ｆ煡姊洪幐搴ｂ姇缁炬澘绉瑰鎶芥偄閻撳海顔夊銈嗘婵倝鎮楅娴庡綊鎮╅悜妯笺€愰悷婊呭閻擄繝骞婂Δ鍛殟闁靛绠撳鐑芥⒑閻撳寒娈犲ù婊勭箓铻炴い鎰堕檮閸嬪鏌涢銈呮瀾闁圭兘浜堕弻銊モ槈濡厧顤€濠电姭鍋撴い鎾卞灩缁犳娊鏌熼悜妯虹仼缁炬澘绉归弻銊モ槈濮楀棙顎嗙紓浣介哺缁诲啫顭囩拠娴嬫瀻闁规崘娅曢鐐烘⒑閸涘﹤濮囩紒澶婄－閹广垹顫濇潏銊ョ彴婵炶揪缍侀ˉ鎾活敂鏉堛劎绠鹃柛鐘墲婢瑰宕?
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
        // WARP 闂佽瀛╃粙鎺楀礉濡￥浜圭€广儱顦涵鈧梺鍝勬祩娴滅偟绮ｅΔ鍐＝?Sniroute 闂備礁鎲″缁樻叏瀹曞洨鍗氶柟缁㈠枛缁狙囨煃閵夛箑澧柡鍡樻緲椤潡鎳滈悽闈涒吂闂佹悶鍊ら崣鍐ㄧ暦閿濆鏅查煫鍥ㄦ礈椤︺€╫ogle/YouTube/Play 缂傚倸鍊搁崯顖炲垂閸ф违濠㈣埖鍔栭崑瀣煕椤愵偄澧扮紒鈧崱娑欑厪?
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

    // 闂備礁鎼悧鍐磻閹捐绠归悗娑櫳戠粈瀣煕鎼淬垺灏﹂柟顖氬暣瀹曠喖顢橀悜鍡樻瘔闂備礁鎼ˇ顖炲疮閹跺鈧線骞嬮敂鑺ユ珫濠殿喗锚瑜板妲愬Δ鍛厾缁绢厼鎳庣槐锔剧磼濡も偓椤﹂潧鐣烽敐澶樻晬婵犲﹤鍟╅崠鏍⒑閹稿海鈽夐柤娲诲灥閵囨劘顦归柟顔荤矙婵℃悂鏁傛禒瀣€栭梻浣告惈缁夋潙煤閿曗偓鍗遍柟杈剧畱缁€鍕煕濠靛棗顏存俊鏌ヤ憾閺屾稑顭ㄩ崘顏嗕紘婵犫拃鍕垫疁鐎殿喚鏁婚、鏍煥椤栨矮澹?
    // 婵犵數濮嶉崟顐㈩潕闂佺粯鐗楃划宥囩矚閸楃偐鏀介柛銉戝懏啸缂?ASCII闂備焦瀵х粙鎴﹀嫉椤掍焦娅犵€广儱妫涢々?U+00A9(濠? 闂?U+00B7(闁? 闂備焦妞垮鍧楀礉韫囨拋褰掑幢濡皷鏋欐繝銏ｆ硾閿曘倝寮查幖浣圭厪?
    {
        // 闂備胶绮〃鍛存偋婵犲偆鐒?+ 闂佽崵濮抽懗鍫曗€﹂崼锝冧汗闁哄被鍎辩粈鍡涙煕閳╁喚娈樼痪鎹愵嚙閳藉骞橀崡鐐╁亾濞戭潿鈧線骞嬮敂鑺ユ珫濠殿喗锕╅崑鍡涱敂闁秵鍋犳繛鎴烆焾鐎氫即鏌曢崶褝韬慨濠咁潐瀵板嫰宕妷褏宕堕梺鑽ゅ仦閸戝綊宕戞径搴涗汗鐎广儱顦涵鈧梺鍝勬川閸犲酣鎮炴繝姘厸闁稿被鍊曞璺ㄧ磼鏉堛劎绠為柟铏尵娴狅箓鎳栭埡鍐╁櫢"All rights reserved"
        // 闂備礁鎲￠〃鍡椕哄鍛亾濮樼厧寮鐐差儔楠炲洭顢涢敐鍡樻澑濠电偞鍨堕幐鍝ョ矓閹绢噮鏁婇柟鐗堟緲閸欏﹪鏌熼幑鎰靛殐婵炵厧鐖奸幃褰掑炊鐠鸿櫣浠村銈嗘磻閸楁娊鐛?闂佸搫顦弲婊堟偡閵堝鐓傛繝濠傚娴滄粓鏌涢敂璇插箺缂佹劖顨呴埥澶愬箻瀹曞泦锝嗕繆閼碱剦鐒炬い鏇秮閸┾偓妞ゆ巻鍋撻柍鐟板暣楠炲洭顢欓悾灞炬"闂?
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
            // 闂佽娴烽弫鎼佸储瑜斿畷鐢割敇閻旇　鏋栭悗骞垮劚鐎氼喚鎷归埡鍛仯鐟滃酣鎮ч弴銏╂晣鐟滅増甯掔粻锝咁渻鐎ｎ亜鐦ㄥù鐓庡€块弻銊モ槈濞嗘ê鍓籐I 濠电偞娼欓崥瀣┍濞差亜鍚规繝濠傚枤閸熷懘寮堕崼娑樺闁哄棗鐗撻弻?WARP 闂佽崵濮甸崝褏绮婚幋锝冧汗闁秆勵殘瀹撲線鎮归崫鍕儓闁?YAML闂?
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
        // winget 闂?闁诲海鎳撻幉陇銇愰崘顔藉仼闁靛ě鍕櫝闂佸憡渚楅崜娑㈡偡椤掑嫭鐓涢柛鎰典簻鏍″┑鐐村絻濞尖€愁嚕?闂佸搫顦弲婊堝蓟閵娿儍娲冀椤撶噥妫?0闂?1978335189闂備焦瀵х粙鎴βㄩ埀顒傜磼鏉堛劎绠為柡灞芥喘閺佹劙宕熼鐘冲枓闂備礁鎼€氱兘宕规导鏉懳﹂柟瀵稿У鐎氬鏌曟径娑氬矝闁?
        // 濠电偛顕慨浼村磿闁稁鏁囩憸鐗堝笒缁狅絽顪冪€ｎ亜鐦ㄥù鐓庡€块弻锟犲礋闂堟稓浠╅梺璇″灟缁舵岸骞嗛崶顒佸亜闁绘垶顭囬弳鐘绘⒑閸涘﹤鐏辨繛鍜冪秮閻涱噣宕堕妸褉鏋栭梺鍛婄箓鐎氼剙鈻嶅▎鎾寸叆婵炴垶顭囬悞鐑芥煃瑜滈崜娆愮附閺冨倻绠斿鑸靛姇閸欏﹥銇勯弽銊ㄥ婵炴嚪鍥ㄧ厽妞ゅ繐瀚烽崕鏃堟煃瑜滈崗姗€宕戦幘缁樼厱闁硅揪绲鹃鐘绘煟閺嶃劎鐭掑┑?
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
            // 闂備礁鎼崑鍡涘储閼测晜顫曢柟瀵稿仧閳绘梻鈧箍鍎遍幊搴♀槈瑜旈弻?闂備礁鎲￠崹鐢稿箹椤愩儯浜规繛鎴炃氶弻锕傛煃閵夈劌鐨洪柛?闂備焦瀵х粙鎺戠暆閹间礁违閹兼番鍔岀粻銉╂倵閿濆骸鍘撮柍褜鍓氳摫缂佸苯宕埞鎴﹀炊閵婏箑顏伴梻?DoH/nginx 闂佸搫顦弲婊堟晪濠电姭鍋撴い蹇撶墛閺咁剟鎮橀悙鑸殿棄闁绘帒銈搁幃瑙勬媴閸欏鐝栭梺杞伴檷閸婃繈鐛€ｎ喗鎲ラ柛灞剧懅闂夊秵绻涢幋鐐村暗闁稿簺鍊濆濠氬炊閳哄啰锛為梺鐟扮摠閻熴垽宕?
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


// 婵犵妲呴崑鈧柛瀣崌閺岋紕浠︾拠鎻掑濠电偞褰冨鈥愁嚕椤掑倹鏆滄い鏂垮⒔閻涖儵鏌?sniroute.cmd update check闂備焦瀵х粙鎴﹀嫉椤掍椒绻嗛柛娑橈攻缂嶅洭鏌熼幆褍鏆辨慨锝咁樀閹綊骞囬鐔虹▏闂佺硶鏅涢懟顖烇綖濠靛绫嶉柍褜鍓熷畷宄懊洪鍛帓闁诲繒鍋熼崕鐢稿磻?
// 闂傚倸鍊稿ú鐘诲磻閹剧粯鍋￠柡鍥ㄧ缁佺増銇勯弮鈧悧鐘差潖?GitHub API闂備焦瀵х粙鎴︽偋韫囨冻缍栭柛鈩冪☉鐎氬顭跨捄渚Ш婵炲鍨介獮鏍偓娑櫳戝畷鍕亜閳哄顩柟宄邦儔婵″爼宕奸姀鈽嗘綌闂備焦瀵х粙鎴βㄩ埀顒傜磼鏉堛劎绠炴鐐茬Ч閸┾偓妞ゆ帊鑳堕々鐑芥煏婢舵盯妾柡澶婄秺閺屾稑顫濋鍕碘偓妤冩喐閺夋鍤熼柣銉簽閹瑰嫰濡搁姀锛勫幘闂備焦瀵х粙鎴炵附閺冨倻绠斿鑸靛姈閸ゅ嫰鏌﹀Ο渚Ц妤犵偑鍨归湁闁挎繂鎳愮粻鎻掝熆瑜滈崣鍐箚閸曨倠鐔哄寲閺囩偘澹?
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
        // 婵犳鍠楄摫婵﹤缍婂畷宄懊洪鍕摋闂佸憡鐟ｉ崕鎶芥偡濠靛棌妲堥柟鐐墯閸庡繑銇勯敃浣圭《闁诡喗婢橀悾婵堚偓锝庡墰閻涖儵姊洪崨濠勬噽濞存粍鐟ч幑銏狀潩閼搁潧浠煎┑鐐叉缁绘劙路閸涱厸妲堥柟鎯у船閳ь剚鐩妴渚€骞嬮敃鈧€氬顭跨捄渚剱婵炲懎绻橀弻娑㈠箛椤掍礁娅ｅ銈嗘⒐閸旀瑦淇?
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


// ---------------------------------------------------------------- GHA 娑撳娴囩€电鐦藉?
// 閺嬩胶鐣濇潏鎾冲弳濡楀棴绱扮紓鏍帆濡?+ 绾喖鐣?閸欐牗绉烽敍宀€鍑芥禒锝囩垳閸掓稑缂撻敍鍫滅瑝闂団偓鐟?.rc 鐠у嫭绨敍澶堚偓?
// Enter = 绾喖鐣鹃敍瀛峴c = 閸欐牗绉烽妴?
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

// 閸氬骸褰寸捄?gha fetch閿涘本濡搁張鈧崥搴濈鐞涘矁绶崙楦垮垱閸掔増鐨靛▔掳鈧?
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
    // WARP 闂備胶顭堥鍐磿闁秴绠栫€广儱鎲樺☉銏犵劦妞ゆ帒瀚拑鐔兼煏婢舵ê鐏ｇ紒?0 缂傚倷绀侀ˇ鎵暜婵犲嫮鍗氶柡澶嬪灍閺嬪酣鏌嶉挊澶嬪櫧缂佲偓婢跺鐔嗛柟顖涘缁ㄥ潡鎮峰▎娆撴缂佸顦垫俊鍫曞幢閳轰椒杩橀梻浣哥秺濞佳呯矓瑜版帒鐒垫い鎺嶇劍閻ㄦ垿鏌ｉ幙鍕瘈濠?
    // 闂備礁鎲￠崙褰掑垂閻楀牊鍙忛柍鍝勬噹缁秹鎮归崫鍕儓閻㈩垬鍎甸弻鐔烘崉閻氭潙浼愮紓鍌氱Т椤戝鐣峰鍐惧悑闁搞儺鐏濋敃鍌涚厵妞ゆ垶瀵х粊鐗堛亜閺傛寧澶勭紒瀣槺閳ь剨缍嗛崑鍛存⒔瀹ュ鐓ユ繛鎴炵懆婢规﹢鏌熼娆戠獢鐎?闂備礁鎲￠崙褰掑垂閻楀牊鍙忛柍鍝勬噹缁秹鎮归崫鍕儓闁搞倖甯″鍫曞煛閸愩劋娌柣銏╁灡閹歌櫕淇婄€涙鐟归柍褜鍓欓埢鎾诲閵堝懓袝闁瑰吋鐣崹娲倿閻ｅ备鍋撳▓鍨灀闁?闂佸搫顦弲婊堟偡瑜旈敐鐐哄箳閹炬潙顫″銈嗗笂缁€浣规償婵犲伅褰掑礂闂傜繝瀛╁銈庡亾缁犳垿鈥︾€圭姷鐤€闁哄倽娉曢?
    // 闂備焦瀵х粙鎴︽偋韫囨侗鏁勯柛銉ｅ妿閳绘棃鏌嶈閸撴瑩鍩㈡惔銊ョ労闁告劗鍋撻埛鏇㈡⒑閸濆嫯顫﹂柛搴″暱閿曘垽宕ㄩ弶鎴狀槴闂佽顔栭崰妤呮偩闁秵鐓涢柛鏇㈡涧閻忥繝鏌＄仦鑺ユ悙妞ゆ洩缍侀崺鈧い鎺戝閺咁剛鈧厜鍋撻柛鎰剁到娴滈箖鎮橀悙璺轰汗闁荤喐鍔曢埥澶愬箻椤栨矮澹曟繝鐢靛仜濡妲愰弴鐔稿弿闁归棿绀佺粻鎴澝归敐鍥ㄦ珪缂佺姷鍏橀弻锟犲礋闂堟稓浠┑鐘亾妞ゆ挶鍨洪崕妤呮煏婢舵ê鐏ｇ紒鈧径鎰厱闁瑰搫顑呴埀顒佹礋閹箖鎮欓鍌楁灃閻庡箍鍎遍幊搴⌒?濠电偛顕慨鏉戭潩閿旂偓宕叉繛鎴欏灩缁犳娊鏌曟径鍫濆姎闁绘挸鍊块弻锟犲磼閵堝懎绠圭紓?闂備焦瀵х粙鎴λ囬銏犵劦?
    // 婵犳鍣徊鐣屾崲閹达富鏁冨┑鍌滎焾鐟欙附銇勯弽銊ㄥ鐞氱喖姊绘笟鍥ф灈闁逞屽墰閸熷孩寰勭仦鎯х／闂佺懓鐡ㄧ划搴敂鏉堛劎绠?up闂備焦瀵х粙鎴炵附閺冨倸绶?*闁诲海鍋ｉ崐鏍磿閼测晜宕?12 缂?*闂?
    // first-run-setup 闂備胶鍘ч〃搴㈢濠婂懏宕插ù锝呮贡閳绘洟鏌ｉ弬鍨暢缂佹劖顨婇弻鐔风暋闁附歇闂佸湱娅㈢紞浣割嚕闂堟侗鍚嬮柛娑卞弾濞兼娊姊洪幐搴ｂ槈闁活剙銈搁幃闈涒槈閵忕姷顦柣鐘叉处瑜板啴寮插▎鎾村仯?up 濠电偞娼欓崥瀣嚌妤ｅ啫绠柟娈垮枓閸嬫捇鎮烽弶鎸庢婵犳鍠氭繛鈧柡浣哥Ч瀹曠厧鈹戦崼鐔哥钒婵犵數鍋炲娆擃敄閸涙潙绠栨俊銈呭暟閸楁碍銇勯弽銊х婵炶壈娉涢湁婵犲﹤瀚粭褎绻?
    // 闂備胶顭堥鍡欏垝瀹ュ洦鏆滈柍褜鍓熼弻銊モ槈濡偐浼囩紓渚囧枔閸旀垵顕?5454 婵犵數鍋涙径鍥焵椤掆偓瀵爼鎯囨禒瀣厱婵鍘ч崢鐢告煃?444 闂佸搫顦弲婊堝蓟閵娿儍?502闂備焦瀵х粙鎴λ囬銏犵劦妞ゆ巻鍋撻柛濠傤煼閹椽骞嬮悩顐壕闁荤喐婢橀弳鐔虹磼椤斿搫濮傜€殿喖鐖奸幃銏ゆ煥鐎ｎ亖鍋撻悽鍛婄厱闁归偊鍓涘暩闁芥鍠庨埥澶愬箻椤栨矮澹曟繝鐢靛仜濡矂鍩€椤掑啯鐝柣銊﹀灩缁辨帞鈧綆浜滈弳閬嶆煟閿濆洤浜剧紒鍌氱Ч瀹曢亶骞囬妸銉ゅ?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 濠殿喗甯楃粙鎺椻€﹂崼銉晣濠电姵鑹剧粈鍌炴煙閹澘袚闁挎稒妫冮弻銊モ槈濞嗘劗娈ら梺杞伴檷閸婃繈鐛€ｎ喗鐓ラ悗锝庡墯閳锋洘绻涢幋鐐村磩妞ゆ泦鍕弿闁冲搫鎳庣憴锔姐亜閺嶃劎鐭嬮柡鍡楃箻瀵爼鍩℃担瑙勬倷濠殿喗菧閸旀垿鐛惔銏㈢瘈闁告侗鍨抽鏇㈡⒑濞茶浜愰柛瀣崌閺?WARP 闂備礁鎼崑鍡涘储閻ｅ本鏆滈柍褜鍓熼弻銊モ槈濡灝顏梺鍝ュ暱閸嬫捇鏌ｉ悩杈╁妽濠㈢懓妫欓幈銊╁箹娴ｅ摜鐣辨繛杈剧悼椤ｄ粙宕?
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
