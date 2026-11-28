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

// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柣妯荤垹閸ャ劎鍘遍柣蹇曞仜婢т粙鎮￠姘肩唵閻熸瑥瀚粈鈧梺瀹狀潐閸ㄥ潡銆佸▎鎾村剹妞ゆ劦鍋傜花濠氭煟閿濆洤鍘存い銏☆殜瀹曠喖顢曢姀鐘辨喚闂備浇顕уù鐑藉极閸濄儲鍏滈柛顐ｆ礀绾惧鏌熼幑鎰厫闁哥姴妫濋弻娑㈠即閵娿儱顫銈忓瘜閸犳牠鈥旈崘顔嘉ч幖绮光偓鑼泿婵＄偑鍊х粻鎾寸閸洖鏄ラ柍褜鍓氶妵鍕箳閹存繍浠惧┑鐐村毆閸ャ劎鍘梺鍓插亖閸╁嫰鎮為悙顑句簻闁哄浂浜炵粔顔锯偓瑙勬礀閵堢顕ｉ幘顔藉€锋繛鍫濈仢閹牓姊婚崒娆掑厡缂侇噮鍨堕弫瀣⒑鐠囪尙绠查柤瑙勫礃椤ｉ箖鏌熼崗鑲╂殬闁搞劏宕靛褔鍩€椤掑嫭鈷戞慨鐟版搐閻忓弶绻涙担鍐插椤╃兘鏌嶉崫鍕櫤闁绘挻鐟︾换娑㈡嚑妫版繂鏁界紓浣靛妿閺咁偊鍩€椤掆偓閻忔艾顭垮鈧弫鍐敂閸繄鐤囬梺缁樺姇閹碱偄鏁柣鐔哥矊缁绘濡甸幇鐗堝€锋い鎺戝€婚惁鍫ユ⒑濮瑰洤鐏叉繛浣冲啰鎽ュ┑鐘垫暩閸嬫盯鎯囨导鏉戠９婵犻潧顑呯粻鏍煕瑜庨〃蹇涘极鐎ｎ喗鐓冪憸婊堝礈閻斿鍤曢悹鍥ㄧゴ濡插牊淇婇鐐存暠妞ゎ偄绉撮埞鎴﹀煡閸℃浠╅梺鍛婅壘椤戝鐛径瀣檮闁告稑艌閹疯櫣绱撻崒娆戝妽閽冮亶鎮樿箛娑欐锭闁宠鍨块幃娆撳煛娴ｅ嘲顥氶梻鍌氬€风粈渚€骞栭锔藉剹濠㈣泛鐬肩粈濠傘€掑锝呬壕闂佺粯渚楅崰鏍綖濠婂牆鐒垫い鎺戝瀹撲線骞栨潏鍓ф偧濡炶濞婇幃妤呮晬閸楃偛鏆曞瑙勬礀閳规垶骞婇柛濠冩崌濮婅棄顓奸崱娆樻锤濠电姴锕ら悧濠囨偂閵夛妇绡€闂傚牊绋掗ˉ鐐烘煕閿濆嫮鐭欓柡灞诲妼閳藉螣婢跺箍鍊楃槐鎺撴綇閵娿儳鐟插┑鐐靛帶缁绘ɑ淇婇幖浣肝ㄩ柨鏇氳兌濡俱劑姊婚崒娆愮グ妞ゆ洘鐗犲畷浼村箳濡も偓閻ら箖鏌ｅΟ娆惧殭缂佺姷濞€閺屻劑鎮㈤崫鍕戙垽宕堕幘顔解拺闁硅偐鍋涢崝姗€鏌涢弬璺ㄐч柟顖欑窔楠炴帒螖娴ｅ弶瀚介梻浣侯焾閺堫剟鎮疯钘濋柨鏇炲€归悡娆戠棯閺夊灝鑸瑰ù婊€鍗抽弻鐔碱敍濮樺吋效缂備浇娅ｉ弲顐ゅ垝濞嗘垶宕夐柕濞у啰鍊為梻鍌氬€风粈渚€骞栭銈囩煓濞撴埃鍋撶€规洏鍨介獮鍡氼槾闁哄棎鍊曢湁闁挎繂妫欓崑鐘绘煃瑜滈崜姘辨暜閹烘缍栨繝闈涱儐閺呮煡鏌涘☉鍗炲妞ゃ儲鑹鹃埞鎴炲箠闁稿﹥顨嗛幈銊╂倻閽樺锛涢梺缁樺姇閻忔岸寮冲鍫熺叆闁绘柨鎼暩閻庤鎸风欢姘跺箖濡ゅ懏鏅查幖瀛樼箘閹稿姊虹悰鈥充壕婵炲濮撮鍡涙偂閺囥垺鐓冮梺娆惧灠娴滈箖姊洪崫鍕櫤缂侇喖娴峰Σ鎰版倷閸濆嫮顓洪梺鎸庢琚欓柟宄邦煼濮婅櫣绮欓幐搴㈡嫳闂佺厧缍婄粻鏍春閳ь剚銇勯幒鎴濐伌婵☆偅鍨剁换娑㈠幢閹邦剛浠煎銈庡亝缁诲嫰骞戦崟顖涙優闁荤喐婢樿婵犵绱曢崑鎴﹀磹閺嶎偅鏆滈柟鐑橆殔绾惧綊鏌涘☉姗堝姛妞も晠鏀辩换婵囩節閸屾粌顤€闂佺粯鎸炬慨鐢垫崲濞戙垺鍤戝Λ鐗堢箓濞堫參姊虹拠鏌ョ崪缂佺粯绻堝濠氭晸閻樻彃绐涘銈嗘尵婵挳鎮￠悢濂夋富闁靛牆鍟悘顏嗏偓鍏夊亾闁归棿绀侀弰銉╂煃瑜滈崜姘跺Φ閸曨垰绠抽柟瀛樼箥娴犲ジ姊洪挊澶婃殶闁哥姵鍔楅幑銏犫槈閵忕姷顓洪梺缁樺姇閻忔岸宕抽鍓х＝濞达絽寮堕鍡涙煕鐎ｎ偅宕屾慨濠勭帛閹峰懘宕ㄦ繝鍌涙畼婵犵數鍋犻婊呯不閹惧磭鏆︽繝濠傚枤濞尖晠鏌ら幖浣规锭濞寸姍鍐炬富闁靛牆鎳愮粻浼存煟濡も偓閿曨亪宕哄☉銏犵闁挎梻鏅崢浠嬫⒑缂佹ɑ鈷掓い顓炵墦钘熼悗锝庡枟閻撶喖鏌ㄥ┑鍡涱€楃€殿噮鍣ｉ弻锛勪沪閸撗勫垱婵犵鍓濋幃鍌涗繆閸洖鐐婇柍鍝勫暙楠炲棝姊婚崒娆掑厡缂侇噮鍨辩粭鐔肺旈崨顓犵崶濠殿喗顭堝▔娑㈡倿閸偁浜滈柟鐑樺煀閸旂喓绱掓径灞炬毈闁哄本鐩幃鈺呭箛娴ｅ湱鏉归柣搴㈩問閸犳銇愰崘顔肩厴闁瑰濮崑鎾绘晲鎼粹€茬敖闂佸憡蓱閹瑰洭骞冨畡閭︾叆闁告洦鍘鹃悡澶愭倵鐟欏嫭绀冪紒顔芥崌楠炲啴濮€閵堝棗浜楅柟鑹版彧缁插潡寮?0=闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇洟姊绘担钘壭撻柨姘亜閿旇鏋ょ紒杈ㄦ瀵挳鎮㈤搹鍦闂備焦鐪归崹钘夘焽瑜嶉悺顓㈡⒒娴ｇ懓顕滄繛鎻掔箻瀹曟劕螖閸涱厾鍔﹀銈嗗笂缁€渚€宕甸鍕厱闁挎繂绻掔粔顔尖攽閳╁啯灏︾€规洏鍔戝鍫曞箣閻橀潧骞€婵犵數濮伴崹鐓庘枖濞戙垺鍎斿┑鍌氭啞閸庡﹪鏌涢銈呮灁缂佲檧鍋撻梻浣圭湽閸ㄨ棄顭囪閻☆參姊绘担渚劸闁挎洩濡囬崚鎺楊敍閻愯尙鏌ч柣鐘荤細閵嗏偓婵℃彃鐗撻弻鏇＄疀閵壯咃紵闂佸憡锕㈡禍鍫曞蓟閿濆棙鍎熸い鏍ㄧ▓閸嬫捇濡烽埡浣侯槹濡炪倖甯掗崐鍝ョ礊閸愵喗鈷掗柛灞剧懅椤︼箓鏌熺喊鍗炰喊鐎殿喚鏁婚、妤呭焵椤掑倸寮叉繝纰樻閸ㄤ即宕ョ€ｎ亞鏆﹂柛娆忣槺缁犻箖鏌熺€电浠ч柣顓滃€栨穱濠囧箵閹烘繄鍚嬮梺鍝勭焿缁查箖骞嗛弮鍫濐潊闁绘ê寮堕惁鎾绘⒒娴ｅ憡鍟為柟鎼佺畺瀹曠増鎯旈…鎴炴櫔?1=闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟版晥闂佹寧绻勯崑娑㈠煘閹寸姭鍋撻敐搴′簼婵炲懎娲铏圭矙鐠恒劎鍔规繝纰樷偓铏窛缂侇喗鐟ㄧ粻娑㈠籍閸屾粎妲囬梻渚€娼ф蹇曞緤娴犲鍋傞柟鎵閻撴洟鏌￠崘锝呬壕闂佺粯顨堟慨鎾偩閻戣棄绠ｉ柨鏇楀亾閸ュ瓨绻濋姀锝嗙【妞ゆ垵娲畷銏ゅ箹娴ｅ厜鎷洪梺纭呭亹閸嬫盯宕濆Δ鍛厸闁告侗鍠氶埥澶愭煟椤垵澧存慨濠勭帛閹峰懘鎼归悷鎵偧闂佹眹鍩勯崹杈╂暜閿熺姴鏋侀柛鎰靛枛鍞梺瀹犳〃缁插ジ鏁冮崒娑氬幈闂佸搫娲㈤崝宀勫几閵堝鐓熼柕鍫濆€告禍楣冩⒒閸屾瑧顦︽繝鈧柆宥呯；闁规崘顕х粻姘舵倶閻愯泛鈻忔繛鎴欏灪閺呮悂鏌ｅΟ鍝勬毐闁逞屽墰閺佸骞冨畡鎵虫瀻闊洦鎼╂禒鍓х磽娴ｆ彃浜鹃梺閫炲苯澧撮柟顔筋殔閳绘捇宕归鐣屼邯濠电姭鎷冮崘顔煎及濡ょ姷鍋為崹鎸庢叏閳ь剟鏌ｅ鍡椾簼妞ゎ偄绉瑰娲濞戞氨顔婃繝娈垮枤閸忔﹢銆佸鈧畷顐﹀礋閵婏附鏉搁梻浣虹帛閸旀洖顕ｉ崼鏇€澶婎煥閸涱垳锛滅紓鍌欑劍閿氱紒妤佹皑缁辨帡宕掑☉妯昏癁闂佺娅曢悧鐘诲春閿熺姴绀冩い蹇撴捣闂?2=闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇洟姊绘担钘壭撻柨姘亜閿旇鏋ょ紒杈ㄦ瀵挳鎮㈤搹鍦闂備焦鐪归崹钘夘焽瑜嶉悺顓㈡⒒娴ｇ懓顕滄繛鎻掔箻瀹曟劕螖閸涱厾鍔﹀銈嗗笂缁€渚€宕甸鍕厱闁挎繂绻掔粔顔尖攽閳╁啯灏︾€规洏鍔戝鍫曞箣閿濆棙鍟洪梻鍌欑窔濞佳嚶ㄩ埀顒€鈹戦垾铏枠鐎规洩缍佸畷鍗烆渻缂佹ɑ鏉搁梻浣虹帛椤洨鍒掗姘ｆ鐟滃孩绌辨繝鍥舵晝闁挎繂娲﹂崳顓㈡倵閸偅绶查悗姘緲閻ｇ兘鎮㈢喊杈ㄦ櫍濠电偞鍨剁湁濠㈣娲熼弻锝夋偄閸濄儲鍣ч柣搴㈠嚬閸樺墽鍒掗崼銉ョ劦妞ゆ帒瀚崐鍨箾閸繄浠㈤柡瀣☉椤儻顦查梺甯秮閻涱噣骞嬮敃鈧～鍛存煟濮楀棗浜濇い顐㈢Ч濮婃椽妫冨☉姘鳖唺婵犳鍠氶崗姗€銆佸鈧畷顐﹀礋閵婏附鏉搁梻浣虹帛閸旀洖顕ｉ崼鏇€澶婎煥閸涱垳锛滅紓鍌欑劍閿氱紒妤佹皑缁辨帡宕掑☉妯昏癁闂佺娅曢悧鐘诲春閿熺姴绀冩い蹇撴捣闂傤垱绻濋悽闈涗哗闁规椿浜炲濠冪鐎ｎ亞顔愬銈嗗姧缁叉寧鏅堕埄鍐瘈闁汇垽娼у暩闂佽桨鐒﹂幃鍌氱暦閹存績妲堥柕蹇婂墲濞呭洤顪冮妶鍛閻庢氨鍏樺畷鐢稿即閻愨晜鏂€闂佺粯锚閻ゅ洦绔熷鈧弻娑樜旀担绯曟灆闂佸搫鐭夌紞浣割嚕椤曗偓瀹曞ジ顢曢敐搴㈩棝缂傚倸鍊烽悞锕傚礉濡ゅ懎纾绘繛鎴欏灩閻撴﹢鏌熸潏楣冩闁稿﹦鍏橀弻鈩冨緞鐎ｎ亞浠兼繛瀵稿У閹倸顫忓ú顏呭亗閹艰揪绲挎缂傚倷鑳舵慨鐢告偋閻樿崵宓侀柛鈩冪懁濞岊亪鎮峰▎娆戠ɑ闁哄倵鍋撻梻浣筋嚙閸戠晫绱為崱娑樼；闁逞屽墴閺?
// 闂?warp-cli status 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬倵楠炲灝鍔氭い锔诲灣缁牏鈧綆鍋佹禍婊堟煙閸濆嫮肖闁告柨绉甸妵鍕棘閹稿骸鏋犲┑顔硷功缁垶骞忛崨顖涘枂闁告洦鍋傞崫妤呮⒒娴ｄ警鐒鹃悗娑掓櫅椤啴鎸婃径灞炬濠电姴锕ら崯鐘诲几鎼淬劍鐓欓悗娑欘焽缁犮儲淇婇锝囩煉闁诡喖鍢查…銊╁川椤撗勬瘔闂佹眹鍩勯崹閬嶃€冩繝鍥х畺闁炽儲鏋煎Σ鍫ユ煏韫囥儳纾块柛姗€浜跺娲濞戣鲸顎嗙紓浣藉紦缁瑥鐣峰┑鍡╂僵闁煎摜鏁搁崢浠嬫椤愩垺绁紒鎻掋偢閺屽洭顢涢悙瀵稿幐闁诲繒鍋涙晶浠嬪Υ閹烘鐓冪憸婊堝礈閵娾晛绠戦柕澶嗘櫅缁€瀣亜閹烘垵鈧摜寰婃禒瀣拻濞达絿顭堥埛鏃傜磼椤曞懎鐏﹂柟顔ㄥ洦鍋愰悹鍥蔼閹芥洟姊洪棃娑辩劸闁稿寒鍣ｅ銊︾鐎ｎ偆鍘遍梺鏂ユ櫅閸橀箖顢旈崟銊︾€洪梺鎸庣箓椤︿即鍩涢幋锔界厱婵犻潧妫楅顏呫亜閵壯冣枅闁哄本绋撻埀顒婄祷閸斿矂鍩€椤掍胶绠撴い鏇稻缁绘繂顫濋鐔割仧闂備胶绮敋鐎殿喖鐖兼俊闈涒攽閸モ晝顔曢柡澶婄墕婢т粙骞冮崗绗轰簻妞ゆ挾鍋熸晶锕傛煥濠靛牆浠辨い銏℃礋閺佸倻鎲撮敐鍡楊伖闂傚倸鍊风欢锟犲磻閸℃稑纾绘繛鎴欏灪閸ゆ劖銇勯弽銊р姇婵炲懐濮甸妵鍕即濡も偓娴滈箖姊洪崫鍕拱缂佸鍨奸悘鎺楁⒑閻撳寒娼熼柛濠冾殜閹剝寰勯幇顓炩偓鐢告偡濞嗗繐顏紒鈧崘顔界厱闁靛鍎查崑銉╂煟濞戝崬鏋涢摶锝夋煠濞村娅囬柨娑欑矊閳规垿鍩ラ崱妤冧化闂佸憡顭嗛崶鈺佺ウ闂佸壊鍋侀崕鏌ュ煕閹烘嚚褰掓晲閸モ晜鎲樺┑鐐插悑閸旀牜鎹㈠┑瀣潊闁愁厸鏅滈崹鍨嚕閺屻儲鍤嬮柛顭戝亜閸斿懘姊洪棃娑辩劸闁稿孩濞婅棟妞ゆ劧闄勯埛鎴︽煕濠靛棗顏存俊鎻掔秺閺屻劌鈽夊▎鎰槹闂佽鍠氶崗姗€寮婚崶顒佹櫆闁诡垎鍐╄緢闂傚倸鍊风粈浣圭珶婵犲洤纾诲〒姘ｅ亾鐎规洝顫夌换婵嗩潩椤掍浇绶㈡俊鐐€栭崝褔姊介崟顖氱厱闁硅揪闄勯崑锝夋煕閵夘垳宀涢柛瀣崌閹煎綊顢曢妶鍕枤闂傚倸鍊烽悞锔锯偓绗涘厾楦跨疀濞戞ê鐎繛瀵稿Т椤戝洤鐣垫笟鈧弻娑㈠箛閳轰礁唯闂佸搫顑呴柊锝夊蓟閻旇　鍋撻悽娈跨劸濞寸姵绮庨惀顏嗙磼閵忕姷浠搁梺鍝勫閸撴繂顕ラ崟顓濇勃闁瑰瓨甯楅崺娑樷攽閻橆偅濯伴柛鏇炵仛閻や線姊虹拠鈥崇仩闁绘鍔楃划璇测槈濡攱顫嶅┑鐐叉閺堫剛鈧俺妫勯埞鎴︽倷閼搁潧娑х紓鍌氱М閸嬫挸鈹戦埥鍡椾簻閻庢碍婢橀悾鐑藉Ψ閵婏絼姹楅梺鍦劋閹搁箖宕㈤柆宥嗏拺缂備焦鈼ら鍫濈柈闁圭瀛╅崣蹇擃熆閼搁潧濮堥柣鎾寸懄椤ㄣ儵鎮欓懠顑胯檸闂佽绻楃亸娆撳焵椤掑喚娼愭繛鍙夌墵婵″爼宕ㄦ繝浣虹畾闂佺粯鍨煎Λ鍕暜闂備線娼ч…顓㈡⒔閸曨垰鐤繛宸簼閳锋帡鏌涚仦鍓ф噭缂佷胶澧楃换娑欏緞鐎ｎ偆顦伴悗瑙勬磻閸楁娊鐛Ο灏栧亾濞戞顏勵嚕閸ф鐓熼幖娣妽濞懷囨煙閼恒儳鐭掗柟顖氱焸瀹曞崬鈽夊▎蹇婂亾閻㈠憡鐓ユ繝闈涙椤庢鏌＄€ｎ剙鏋涢柡灞界Ч閺屻劎鈧綆浜濈拠鐐烘⒑閸濆嫭婀伴柣鈺婂灦閵嗕線寮撮姀鐘栄囨煕濞戝崬鏋ゅù婊勭墵濮婄粯绗熼埀顒€顭囪铻炴繛鍡樺灦閺嗘粓鏌熼悜姗嗘畷缂佺姵甯″缁樻媴閾忕懓绗￠梺鑽ゅ枂閸ㄨ棄鐣烽幋锕€绠荤紓浣诡焽閸橀亶姊洪棃娑辩劸闁稿﹥娲栭悾鐢稿幢濞戞瑧鍘遍梺鍐叉惈閸燁偅绂掓潏顭戞闁绘劕妯婇崕鏃堟煛娴ｇ鈧潡骞愭繝鍐ㄧ窞濠㈣泛顑囧鏍ㄧ節绾板纾块柛瀣灴瀹曟劙寮介鐐殿槷闂佹寧娲嶉崑鎾淬亜椤撶偞鍋ラ柛鈹惧亾濡炪倖宸婚崑鎾淬亜椤撯剝纭堕柟鍙夋尦瀹曠喖顢楅崒銈喰у┑鐘愁問閸犳牠鏁冮敂鎯у灊妞ゆ牜鍋涚粻顖炴煕濞戞瑦缍戠€瑰憡绻傞埞鎴︽偐閹绘巻鍋撻悷鎵虫灁婵☆垵銆€閺€浠嬫煟閹邦剛鎽犻悘蹇斿閻ヮ亪寮剁捄銊愌囨煏閸℃鏆炵紒缁樼箞瀹曟帒顭ㄩ崟顒夊晭闂傚倷娴囧畷鐢稿窗閹邦厾鐝堕柛鈩冪☉濮规煡鏌ㄩ弴鐐测偓褰掓偂閺囥垺鐓忓璺侯儏閻忋儵鏌￠崼顐㈠⒋閽樼喖鏌嶉妷锔界伇闁衡偓娴犲绠抽柟鎯版绾惧湱鎲歌箛鎿冨殫濠电姴鍟伴々鐑芥倵閿濆簼绨介柡灞界墕椤啴濡堕崱娆忊拡闂佺顑囬崑銈呯暦閹达箑纾兼繛鎴炴皑椤旀洟姊虹化鏇炲⒉閽冮亶鎮樿箛搴″祮闁哄本绋掔换婵嬪磼濠娾偓缁數绱撴担铏瑰笡闁烩晩鍨堕悰顔锯偓锝庡枟閸婄兘鏌涢…鎴濅簻婵炲憞鍥ㄢ拻濞达綀娅ｇ敮娑㈡偨椤栨粌浠辩€殿喗鐓￠、鏇㈠Χ閸♀晜缍楅梻浣告贡閸庛倝銆冮崱娑樼；闁规壆澧楅悡锝夌叓閸ラ鍒板ù婊勭矒濮婄粯寰勬繝鍕戙垺銇勯鍕殻濠碘€崇埣瀹曟﹢濡搁妷搴樻櫊濮婅櫣绮欓幐搴㈠闯闂佽桨绀侀…鐑芥偘椤曗偓瀹曟﹢濡搁姀锛勨偓濠氭⒑閻熸壆浠㈤柛鐕佸灦瀵彃顭ㄩ崼鐔叉嫼闂佸憡绋戦敃銉﹀緞閸曨垱鐓曟俊顖涘椤忕姵淇婇崣澶婂闁宠閰ｉ獮妯煎垝鐟欏嫬顏烘繝鐢靛仩閹活亞寰婃禒瀣疅闁跨喓濮寸粻姘舵煕椤愮姴鍔滈柛瀣У閹便劌螣閻撳骸浠樻繛瀛樼矋婢瑰棝鍩€椤掑喚娼愭繛鍙夘焽閹广垽宕掗悙鏉戜患濠电娀娼ч鍛矆鐎ｎ偁浜滈柟鎵虫櫅閳ь剚娲栭锝夊Ω閵夈垺鏂€闂佺粯锕╅崰鏍倶鏉堛劎绠惧璺侯儑椤厧顭胯缁诲牆顫忓ú顏勪紶闁告洟娼ч崜浼存⒒娴ｇ绨荤紓宥勭椤曪絾绻濆顓炰簻闁荤偞绋堥埀顒€鍘栨竟鏇㈡⒑濮瑰洤鐏い鏃€鐗犻幃鐐哄箚椤€崇秺閹亪宕ㄩ婊勬闂備礁鎼張顒傜矙閹烘绠氶柡鍐ㄧ墕椤懘鏌嶉挊澶嬵棡缂傚秵銆桻UE 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬婊€绨婚梺鍝勫暙閸婂綊宕甸埀顒佺箾鐎涙鐭掔紒鐘崇墵瀵鈽夐姀鐘电杸闂傚倸鐗婄粙鎺楁倶閸儲鍊甸柣鐔哄閸熺偟绱掔拠鎻掓殻濠碉紕鏁诲畷鐔碱敍濮橆剙鏁ら梻渚€娼ц噹闁逞屽墮鍗遍柛顐犲劜閻撴瑩鏌涜箛鎾虫倯缂傚秵鍨块弻銊╁即濡　鍋撳┑瀣槬闁绘劕鎼粻锝夋煟閹邦垰鐨洪柣銈呮喘濮婄粯鎷呮笟顖滃姼闂佹寧娲忛崕杈╁弲闂佺粯妫侀崑鎰暤娓氣偓閻擃偊宕堕妸锔撅紩闂佺顑嗛幑鍥极閹邦厽鍎熼柍鈺佸暟娴滎亪姊绘担鍛婃儓妞ゆ垵瀚划鍫熺瑹閳ь剟宕洪悙鍝勭闁挎棁妫勬禍褰掓⒑閸涘﹦鎳冩い锔诲灦钘濋柍鍝勬噺閳锋垹绱撴担鑲℃垹浜搁悧鍫㈢闁告瑥顦介悞浠嬫煙楠炲灝鐏╅柍钘夘樀婵偓闁绘ɑ顔栭崥鍛節閻㈤潧鈻堟繛浣冲吘娑橆煥閸繃妲梺缁樺姉閸庛倝鍩涢幋锔界厱婵犻潧妫楅鎾煕鎼达紕效闁哄本绋掔换婵嬪礃椤忓棛鏉介柣搴㈩問閸犳盯顢氳椤㈡ɑ绺界粙璺ㄥ€為梺鍐叉惈閸犳稓妲愰崣澶岀瘈缁剧増蓱椤﹪鏌涢妸銉ヨ埞妞ゎ亜鍟村畷褰掝敊閵夛妇娲寸€规洘锕㈤、娆戞喆閿濆棗顏归梻浣藉吹婵潙煤閻樿鐒垫い鎺戝暞閻濐亪鏌ｈ箛鎾缎㈡い顏勫暣婵″爼宕橀妸銉€抽梻浣瑰濞诧附绂嶅鍫晪闁挎繂顦介弫鍐煥閺囨浜剧紓浣哄У缁嬫帡濡甸崟顖氱閻犺櫣鍎ら悗濠氭⒑閸涘﹥鐓ラ柟铏～蹇撁洪鍛偓鐑芥煢濡警妲稿┑顔芥礋濮婃椽宕ㄦ繝鍐ㄩ瀺闂佺厧缍婄粻鏍灳閿曞倸鐐婃い鎺嗗亾缂佲偓閸垺鍠愰煫鍥ㄦ礀椤ユ艾霉閻樺樊鍎愰柣鎾存礋閹﹢鎮欓幓鎺嗘寖闂佺懓鍟块幊蹇涘Φ閸曨垱鏅滃┑鍌氭啞椤庡秹姊洪棃娑欐悙閻庢矮鍗冲顐﹀磼濠婂嫬鐝伴梺鑲┾拡閸忔盯宕犻弽顐ょ＝闁稿本鐟︾粊鏉棵瑰搴″⒋鐎规洘鍨块幃娆撳垂椤愩垹绨ユ繝鐢靛█濞佳囶敄閸涘瓨鍊峰┑鐘插閸犳劙鏌熸潏鍓х暠缂佺姵妞介弻锟犲炊閳轰椒姹楅梺琛″亾濞寸姴顑嗛悡鐔兼煙闁箑骞楃紓宥嗗灴閺岋綀绠涢妷褏鏆ら梺鍝勮閸旀垵顕ｉ弶鎳虫梹鎷呴崷顓炵到闂傚倷绶氬褍螞濞嗘垶鏆滄俊銈呭暞瀹曞弶绻濋棃娑欙紞婵炲皷鏅滈妵鍕箻鐠虹洅銏☆殽閻愭惌娈滄慨濠勭帛閹峰懘鎮滃Ο鐑樼暚闂備礁顓介弶鍨瀳濡炪値鍋勭换姗€骞冮悜钘夌闁惧浚鍋嗛埀顒佹そ濮婃椽宕ㄦ繝浣虹箒闂佸憡鏌ㄩ悥濂稿箖閻戣姤鐒介柨鏃€鍎冲鎶芥⒒娴ｇ顥忛柛瀣╃窔瀹曡绻濆顒佽緢濡炪倖鍔ч梽鍕煕閹达附鐓曟繝闈涙椤忣偅绻涚粭鍝勨偓婵嬪蓟濞戙垹鐒洪柛鎰典簼閸ｎ噣姊洪崫鍕垫Ч闁诡喖鍊搁～蹇撁洪鍕唶闁硅壈鎻徊鍧楁偩閻㈠憡鈷戦柣鐔告緲閺嗚京绱掓径灞惧殌闁伙絽鍢查埢搴ㄥ箛椤曞懏绁梺璇插嚱缂嶅棝宕戦埀顒勬煙闁垮銇濇慨濠冩そ瀹曘劍绻濋崟銊︻潔闂備焦瀵уú蹇涘磹濠靛棛鏆﹂柣銏犲閺佸棝鏌涢弴銊ュ闁告﹢浜堕弻锝嗘償椤栨粎校婵炲瓨绮嶇换鍫濈暦閾忣偆顩烽悗锝庡亞閸橀潧螖閻橀潧浠滈柣蹇旂箞閹﹢鍩￠崨顔惧帗闂佽姤锚椤﹁棄危閹间焦鐓冮悹鍥ㄧ叀閸欏嫭顨ラ悙鍙夊枠妞ゃ垺鐟╅獮蹇撐熺粙搴撳亾濠靛棭娼栫紓浣股戞刊鎾煕濞戞﹫宸ラ柡鍡楃墦濮婃椽宕烽褏鍔稿┑鈽嗗亝缁诲倿鎮惧畡閭︽建闁逞屽墯娣囧﹤顫㈠畝濠冃╅梺姹囧焺閸ㄩ亶骞愰懡銈嗗床婵犻潧顑嗛崑銊╂⒒閸喎鍨侀柕澶涜礋娴滄粓鏌ㄥ┑鍡欏妞ゃ儱顦靛畷锟犳焼瀹ュ棛鍘卞┑鐐村灦閻燂妇绱為幋鐘垫／闁诡垎宀€鍚嬮梺鍝勬湰缁嬫垿鍩ユ径濞㈢喖宕楅崗鐓庢灈闂傚倷鑳堕…鍫ヮ敄閹寸姵顫曢柡鍥ュ灩缁犵喐鎱ㄥ┑鍡欑伇闁汇倐鍋撴繝鐢靛仦閸ㄥ爼鈥﹂崶顑﹀洭顢曢敂瑙ｆ嫼闂侀潻瀵岄崢濂稿礉鐎ｎ喗鐓欑紒瀣仢閳锋梻鈧鍟崶褏鍔﹀銈嗗笒鐎氼參鍩涢幋锔界厵闁兼祴鏅涙禒婊堟煕閺冣偓閿曘垽寮诲☉銏犳閻犳亽鍓遍姀銈嗙厓閻犲洩灏欐晥閻庤娲栭悥濂搞€佸Δ浣瑰闁告瑥顦鍦磽閸屾艾鈧绮堟笟鈧、鏍幢濞嗗苯浜剧紒妤佺☉閸犳盯鎳撻崸妤佺厵婵炲牆鐏濋弸銈囩棯閹佸仮闁诡喗顨婇弫鎰償閳ヨ尙鍑归柣搴ゎ潐濞叉牠鎯夋總绋跨劦妞ゆ帒鍠氬鎰箾閹绘帞绠荤€规洝顫夌粋鎺斺偓锝庡墮缁侊箓姊洪崷顓炲妺闁搞倧绠撳顕€鍩€椤掑倻绠旈柣鏃傚帶閻愬﹥銇勯鐔风仸妞ゃ儱锕ョ换婵堝枈婢跺瞼锛熼梺绋款儐閸ㄥ灝鐣烽幇鏉垮唨妞ゆ挾鍋熼ˇ顕€姊虹涵鍛涧缂佺姵鍨垮畷褰掑磼閻愬鍘介梺鐟邦嚟娴兼繈顢旈崱姗嗗仺闂佺粯鍔楅崕銈夋偂閺囥垺鐓涢柛銉ｅ劚婵″ジ鏌ｈ箛銉х瘈闁哄矉缍侀獮妯尖偓闈涙啞閸ｎ喚绱撴担浠嬪摵閻㈩垳鍋ら崺鈧い鎺戯功缁夌敻鏌涚€ｎ亜顏い銏¤壘椤劑宕奸悢鍝勫箥闂備礁鎲￠悷銉┧囨导鏉戠９闁规壆澧楅悡娑氣偓鍏夊亾闁逞屽墴瀹曠増鎯旈妸锕€浠奸梺璺ㄥ枔婵敻鎮炴禒瀣叆闁哄洦顨呮禍楣冩⒑閸涘﹥灏柣鏍с偢楠炲啫螖閳ь剟鍩㈤幘璇插瀭妞ゆ梻鏅禍顏呬繆閻愵亜鈧倝宕ｉ崘顔肩哗閺夊牄鍔庨埞宥呪攽閻樺弶澶勯柛濠傤煼閺屻倝骞侀幒鎴濆濠电偛鎳庡Λ婵嗩潖閾忚瀚氶柍銉ョ－閳ь剙顭烽弻娑㈠箛閸忓憡鍠氶梺鍝ュ枎椤戝顫忔繝姘＜婵﹩鍏橀崑鎾诲箹娴ｅ摜锛欓梺缁樺灱婵倝宕愰崸妤佺叆闁哄洨鍋涢埀顒€鎽滄竟鏇㈠锤濡や胶鍘遍梺褰掑亰閸ㄧ厧顫忛崸妤佺厽闁绘梻鍘ф禍鐗堢箾鐏忔牗娅婇柟顔款潐閵堬箓骞愭惔顔诲摋闂備焦瀵х粙鎺楁儎椤栨凹娼栨繛宸簻瀹告繂鈹戦悩鎻掓殭妞わ腹鏅犲娲箹閻愭彃顫囨繛瀛樼矎濞夋盯鎮鹃悜钘夌疀妞ゆ棁妫勭壕顖炴煟閻愬鈻撻柛鎾寸箞瀵剟鍩€椤掑嫭鈷掑ù锝堟鐢盯鎷戞潏鈺傚枑闁哄鐏濋弳锝嗐亜閵忊剝顥堟い銏℃礋婵″爼宕堕…鎺旂闂傚倷娴囬鏍垂鎼淬劌绀冮柍杞拌閸嬫捇顢涘☉姘鳖啎闂佹寧绻傞幊蹇涘箹閹扮増鐓ラ柡鍥ュ妺缁ㄧ厧菐閸パ嶈含鐎规洖銈稿鎾倷绾板搴婂┑鐘愁問閸犳鏁冮埡鍛；闁告洦鍨佹径濠庢僵妞ゆ垼濮ら弬鈧梻浣瑰缁嬫垹鈧凹鍓氱粋宥嗙附閸涘﹦鍘辨繝鐢靛€崘顭戜还缂備浇顕уΛ婵嬪蓟濞戙垹唯妞ゆ梻鍘ч～鈺呮⒑閸濆嫷鍎庣紒鑸靛哺楠炲啳銇愰幒鎴犲€為悷婊冮叄閸┿垽寮撮姀锛勫幈闂侀潧鐗嗗Λ娆戠矆閳ь剟姊虹€圭媭娼愰柛銊ユ健楠炲啴鍩￠崘锝呬壕闁革富鍘煎瓭濡炪們鍎茬换鍫ュ箖濡ゅ懏鍋￠柣妤€鐗嗛弳鍫㈢磽娴ｇ瓔鍤欓柛鐔风摠娣囧﹪鎮界粙璺槹濡炪倖鐗楀銊╊敊閹烘垟鏀介幒鎶藉磹閹版澘纾婚柟鍓х帛閸婂灚鎱ㄥΟ鐓庡付濠⒀冾嚟閳ь剝顫夊ú姗€銆冮崱娆愬床婵犻潧顑呴悙濠囨煏婵炲灝鐏悗姘虫閳规垿鎮欓懜闈涙锭缂備浇寮撶划娆撶嵁婢舵劖鏅搁柣妯垮皺閺屽牓姊虹紒妯哄闁稿簺鍊濋幃鈥斥枎閹寸姷锛滈柣搴秵閸嬪嫰顢氬鍕╀簻闁靛绲介悘顕€鏌嶈閸撴繈锝炴径濞掓椽鏁傞悾宀婃锤濠电娀娼ч鍛不椤栫偞鐓曟繛鎴炵懄缂嶆垹绱掗埀?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幈闂佸綊鍋婇崹鎵閿曞倹鐓熼柕蹇嬪灮鐢稑菐閸パ嶈含闁诡喗鐟╅、鏃堝礋閵娿儰澹曢梺鍝勭▉閸樹粙宕戠€ｎ偂绻嗛柕鍫濇噹閺嗙喖鏌ｉ鐔烘噰闁哄瞼鍠栭幃娆擃敆閸屾稖寮撮梻渚€鈧偛鑻晶顕€鏌熼搹顐€顏堟偩閻戣姤鏅查柛鈩冾殘缁愮偤鎮楅崗澶婁壕闂侀€炲苯澧撮柟顖欑窔瀹曞ジ濡烽敂瑙勫闂備胶顭堥張顒€顫濋妸锔芥珷婵炴垶姘ㄧ壕濂告倵閿濆骸浜滄繛鎼櫍閺屸剝鎷呴崫銉愌呪偓娈垮櫘閸ｏ絽鐣锋總鍛婂亜闁告繂瀚粻?status 闂?Connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂傚洤顦甸弻銊モ攽閸℃瑥顤€濡炪倕绻掓慨椋庢閹烘鐒垫い鎺嶈兌缁♀偓闂佺鏈〃鍡涘棘閳ь剟姊绘担铏瑰笡閽冭京鎲搁弶鍨殲缂佸倸绉归幃娆撴倻濡厧骞楅梻浣告贡閸庛倕顫忛悷鎵虫瀺闁哄洢鍨洪悡娆撴煕濞嗗浚妲洪柟钘夊暟缁辨帡鎮╁畷鍥ㄥ垱閻庢鍠楅幐鎶藉箖濞嗘挻鍤戞い鎴濈仢閺嬪牓姊婚崒姘偓椋庣矆娓氣偓楠炴牠顢曢敃鈧悿顔姐亜閹板爼妾柛瀣儔閺屾盯鍩勯崘顏呭櫘婵炴垶鎸哥粔鐢稿Φ閸曨垰鍗虫俊銈傚亾濞存粓绠栧娲传閸曨厾鍔圭紓浣哄У閸ㄥジ鎮橀幒鎾剁闁圭偓娼欓崵顒勬煕閵娿劑鍝洪柣鈥崇箻濮婂宕掑▎鎺戝帯缂佺虎鍘奸悥鐓庮嚕閹绘巻鏀介悗锝庝簽閿涙稑鈹戦悙鏉戠亶闁瑰啿娲崺鈧い鎺嶇劍椤ュ牏鈧娲橀敃銏ゃ€佸▎鎾冲簥濠㈣鍨板ú锕傛偂閺囥垺鐓冮柍杞扮閺嬨倖绻涢崼鐕傝€块柡宀嬬秮閹垽寮堕幋婵愭綆婵犳鍠栭敃銊モ枍閿濆洤鍨濇繛鍡樻尭缁€宀勬煃閵夈儱甯犵紒鎲嬬悼缁辨捇宕掑顑藉亾閸濄儳鐭撻柟缁㈠枟閺呮繈鏌曢崼婵愭Ч闁稿鏅犻弻锝夊箣閿濆棭妫勭紓浣哄缁插墽鎹㈠┑瀣瀭妞ゆ劑鍨归～宥夋倵閻愮懓鈧牕顪冮挊澶樻綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顦叉い鏇ㄥ弮閸┾偓妞ゆ帊绶￠崯蹇涙煕閻樺磭澧甸柍銉畱閻ｏ繝鏌囬敃鈧▓銊╂⒑閸︻叀妾搁柛鐘愁殜瀹曟垿宕熼娑氬弰闂婎偄娲﹂崙褰掑吹閸愵喗鐓曢柣鏃傚厴閸欏嫭鎱ㄦ繝鍕笡闁瑰嘲鎳愮划娆忊枎閻愵剦妫忛梻鍌欒兌缁垶骞愰幖浣瑰亱闁规儳纾弳锕€霉閸忓吋缍戦柛鎰ㄥ亾闂備線娼ч…顓犵不閹存繍鍤曢柛褎顨嗛埛鎴︽煕閹炬潙绲诲ù婊勭箖缁绘盯宕ｆ径娑溾偓鍧楁煕閳哄倻娲存鐐差儔閺佸倿鎮剧仦钘夌疄濠电姷鏁搁崑鐐哄垂閸洘鏅濇い鎰剁睄濞戞﹩娼╂い鎺戭槺閸炵敻鏌ｉ悢鍝ユ噧閻庢碍鎮傞幊鎾诲锤濡や胶鍘搁柣蹇曞仜婢т粙鍩婇弴鐘电＜缂備焦顭囩粻鐐碘偓瑙勬礃鐢帡鍩ユ径濞㈢喓鎲撮崟顑芥灆闂傚倷娴囬褏鈧稈鏅犻、娆撳冀椤撶偟鐛ラ梺鍝勭▉閸樻悂鍩€椤掑﹦鐣甸柟顔界矒閹稿﹥寰勫畝鈧弳顐︽⒒娓氣偓濞佳呮崲閸儱纾归柡宥庡幖绾惧鏌涘畝鈧崑娑氱不瑜版帒绾ч柛顐ｇ箓閳锋梻绱掓径灞炬毈闁哄本鐩幃銏＄附婢跺﹤澹夊┑鐑囩到濞层倝鏁冮鍫濈畺婵炲棙鎼╅弫鍌炴煕閺囨ê濡煎ù婊堢畺閺屸€愁吋鎼达絼姹楀┑鐐叉▕娴滄粓宕橀埀顒€顪冮妶鍡樺暗闁稿鍠栭幃鍧楀炊椤掍緡妫呭銈嗗姂閸ㄧ儤寰勯崟顖涚叄闁哄倹顑欓崕鎰偓鍨緲鐎氫即鐛崶顒夋晢闁逞屽墴瀹曨剟濡搁埡鍌氫画濠电姴锕ょ€氼噣鎯屾繝鍥ㄧ厪闁糕剝锚缁楁氨绱掔紒妯肩畵妞ゎ偅绻堥、姗€鎮╅幓鎺戭洭闂傚倸鍊风粈浣圭珶婵犲洤纾婚柛娑卞灡瀹曟煡鏌涢幇闈涙灈闂傚偆鍨堕弻褑绠涢幘纾嬬缂佺偓鍎抽妶鎼佸蓟濞戞矮娌柟顖嗗啯鐦撻梻浣侯焾鐎涒晛螞閸愵喖钃熼柨娑樺濞岊亪鏌涢幘妤€瀚崹杈ㄤ繆閵堝洤啸闁稿鍋ら垾锕傚炊椤掆偓閻掑灚銇勯幒鎴濐仾婵炴嚪鍥ㄧ厵閻犲泧鍛槇閻庤娲橀懝楣冨煡婢跺á鐔奉煥閸曨剦妫冮梺璇″枓閺呯姴螞閸愩劉妲堟俊銈呭暞瀹曠喓绱撻崒姘偓鐑芥嚄閸撲礁鍨濇い鏍ㄧ矋閺嗘粓鏌ㄩ悢鍝勑㈤柡鍕╁劦閺屽秷顧侀柛鎾村哺婵＄敻宕熼姘鳖唺闂佽鎯岄崹鐗堟叏濞戞氨纾藉ù锝呭濡叉椽鏌℃担鍓茬吋鐎殿喛顕ч埥澶愬閳ュ厖姹楅柣搴ゎ潐濞叉牕煤閵堝鐒婚柣鐔稿閺€浠嬫煟閹邦厽缍戠紒鑼帛缁绘稓浜搁弽銈呬壕闁归鐒︾紞搴ｇ磽閸屾瑧鍔嶉拑鍗炩攽椤栨稒灏﹂柡灞剧☉閳藉顫滈崼婵呯矗闂備礁鎼Λ妤呮偋閸℃稑鐓橀柟杈鹃檮閸婂嘲螞閻楀牏绠撴繛鍫熺箘缁辨挻鎷呴崫鍕戯綁鏌熼崨濠冨€愰柟顕€绠栭幃婊堟寠婢跺矂鐛撴繝鐢靛仜濡盯宕戦幘璇茬＝闁汇垻鏁哥壕钘壝归敐鍫綈妞ゃ儲妫冮弻娑㈠Ω閿旂晫鍑￠柣鎾卞€濋弻銊╁籍閸喐娈伴梺绋款儐閹稿骞忛崨鏉戠闁靛鍊栭惈蹇涙⒒娓氣偓閳ь剛鍋涢懟顖涙櫠椤栨粎纾奸悗锝庡亜缁椻晝鎲搁悧鍫㈢煉婵﹥妞介幃鐑藉级閹稿巩鈺侇渻閵堝骸骞栭柛銏＄叀閿濈偠绠涘☉娆愬劒闂侀潻瀵岄崢楣冩偂閹剧粯鈷戦柛锔诲弾閻掗箖鎮楀☉鎺撴珚妤犵偛绻戝蹇涘煛閸愵亷绱查梻浣虹帛閸旓箓宕滃杈偨闁肩⒈鍓涚壕鐓庮熆鐠虹儤婀伴柛銈傚亾闂備椒绱徊鍧楀礂濮椻偓閵嗕礁鈽夊鍡樺兊闂佺绻掗崢褔宕楁繝鍥ㄢ拻濞达絽鎼敮鍫曟煙閼恒儳鐭掔€殿噮鍋婇獮鏍ㄦ媴缁嬪灝浜堕梻鍌欑贰閸撴瑧绮旈悽绋跨厱闁硅揪闄勯埛鎺楁煕椤愩倕鏋嶇紒鎯伴哺娣囧﹪顢涘鍗炩叺闂佸搫鏈惄顖涗繆閹间焦鏅濋柍褜鍓熼崺銏ゅ即閵忥紕鍘靛銈嗘⒐閸庤櫕绂嶉悙顒傜闁绘挸娴风粻璇测攽閳╁啯鍊愰柡浣瑰姍瀹曠喖顢橀悩铏珤闂傚倸鍊搁崐椋庢濮橆剦鐒界憸鏃傜槵闂侀€炲苯澧扮紒杈ㄥ浮閹晠宕橀懠顑跨礉闁诲氦顫夊ú锕傚磻婵犲洤绠栨繝濠傚悩閻旂儤瀚氶柟缁樺笂缁辩喐绻濋悽闈浶ラ柡浣告啞閹便劑骞橀鑲╂焾濡炪倖鐗滈崑娑欘攰婵犵數濞€濞佳囨偋閸愵喖纾婚柟鎹愬煐閸犲棝鏌涢弴銊ュ闁挎稒锕㈠娲川婵犲啰鍙嗙紓浣割槸閻栧ジ鐛幋锕€顫呴柣姗嗗亝閺傗偓闂佽鍑界紞鍡樼閻愭牳鍥级濞嗙偓瀵岄梺闈涚墕濡宕告繝鍥ㄧ厱閻庯綆鍋呯亸鎵磼缂佹娲寸€规洏鍔戝畷鎯邦槼濠㈣泛瀚伴弻宥夋煥鐎ｎ亞鐟ㄩ梻鍥ь樀閺屻劌鈹戦崱娆忊拡濠电偛鍚嬮崝娆撳蓟濞戞埃鍋撻敐搴″闁宠鐗撻弻锛勪沪閸撗勫垱閻庢鍣崜娑㈠箲閸曨垰惟闁规澘澧庨幃鑲╃磼鏉堛劌绗х紒杈ㄥ笒铻ｉ悘蹇旂墪椤瞼绱撻崒娆掑厡缂侇噮鍨抽幑銏ゅ醇濠靛牊娈鹃梺鍦劋椤ㄥ懎娲块梻浣告啞娓氭宕抽鈧畷銉モ枎韫囧﹥鏂€闂佹寧绋戠€氼剚绂嶆總鍛婄厱濠电姴鍟版晶鐢碘偓瑙勬礃缁诲棝藝鐎靛摜纾兼い鏃傗拡閻撳吋顨ラ悙宸剰闁宠鍨垮畷鍫曞煛娴ｅ憡鐦ｆ繝鐢靛Х閺佸憡鎱ㄩ悜钘夋瀬闁告稑锕ラ崣蹇撁归崗鍏肩稇闁搞劌鍊块弻锝夊箻瀹曞洤鍝洪梺鍝勵儐閻楁鎹㈠☉銏犵闁绘劗鏁搁悡鍌氣攽?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃闁硅櫕鎹囬垾鏃堝礃椤忎礁浜鹃柨婵嗙凹缁ㄧ粯銇勯幒瀣仾闁靛洤瀚伴獮鍥敂閸℃瑧鍘梻浣告惈鐞氼偊宕濋幋锕€绠栭柕蹇嬪€曟导鐘绘煕閺囩喎鐏熼柛銊ヮ煼閹偓妞ゅ繐鐗嗙粻姘辨喐濠婂牊鍋傚┑鍌氭啞閻撴盯鎮橀悙鎻掆挃闁宠棄顦甸弻宥夋寠婢舵ɑ鈻堥悗瑙勬穿缁绘繈骞冨▎鎴斿亾閻㈠憡娅滃瑙勬礋濮婂宕掑▎鎴М缂佸墽铏庨崣鍐嵁婵犲洤绠婚柛鎾茶兌閻撳鎮峰鍛暭閻㈩垱甯￠幏鎴︽偄閻戞ê鏋戦梺鐟邦嚟婵攱绋夊鍡欑闁瑰鍎戞笟娑欑箾鐏忔牗娅婇柡灞剧缁犳稑顫濋鎸庣潖缂傚倷绀侀ˇ閬嶅礂濮椻偓瀵?sniroute.cmd 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂傚洤顦甸弻銊モ攽閸♀晜效婵炲瓨鍤庨崐婵嬪蓟閵堝绾ч柟绋块娴犳潙鈹戦纭锋敾婵＄偘绮欓妴浣肝旈崨顓犲姦濡炪倖甯掗崐濠氭儗閹剧粯鐓熼柕蹇嬪焺閻掑墽鐥幆褜鐓奸柡灞诲姂閹垽宕崟鎴秮閺岋綁骞樼€靛憡鍣伴梺鍝勭灱閸犳牠銆佸☉姗嗙叆闁告劗鍋撳В澶嬩繆閻愵亜鈧垿宕曢柆宓ュ洦瀵煎ǎ顑藉亾閸愵喖唯闁冲搫鍊搁埀顒勬敱閵囧嫯绠涢幘鎰佷紑婵炲鍘ч悧鎾愁潖濞差亜浼犻柛鏇ㄥ枛椤忣厾绱撴担鍓叉Ш闁硅櫕鎹囬幃楣冩倻缁涘鏅濋梺鎸庢磵閸嬫挾绱掗埀顒傗偓锝庡亖娴滄粓鏌熼柇锕€鏋涢柡瀣闇夋繝濠傚暙閳锋棃鏌熼崣澶嬪€愰柟顔ㄥ洤閱囨繝闈涚墢閹冲棛绱?warp on/off闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋垺鍋橀柍銉ュ帠婢规洟姊哄Ч鍥х仾妞ゆ梹鐗犻幃鐐哄箚椤€崇秺閹晛鈻庤箛鏂款槱闂佹娊鏀辩敮锟犲蓟濞戙垹鍗抽柕濞垮劙缁ㄨ顪冮妶鍛闁告瑥鍟村濠氬灳瀹曞洦娈曢柣搴秵閸撴盯鎯侀崼銉﹀€甸悷娆忓缁€鍐煥閺囨ê鐏ǎ鍥э躬楠炴牗鎷呴懖婵勫姂閺屾洝绠涢弴鐐愨€愁熆鐟欏嫭绀嬫慨濠冩そ閹剝鎯旈鐣岀◥闂備胶顭堥敃銉┿€冮崱娆戠煓濠㈣泛顭崥瀣熆鐠轰警鍎戦柛妯绘倐濮婅櫣绮欓幐搴㈡嫳婵炴挻纰嶉〃濠傜暦閺囥垹唯闁冲搫鍊婚崢浠嬫⒑閸濆嫬鈧湱鈧瑳鍥佸濮€閵堝棛鍘靛銈嗘⒐椤戞瑥顭囬幇鐗堢厪闁割偒鍓涢悾鍨殽閻愭潙濮堥柟顖涙椤㈡瑧鎷犻崣澶嬪暉闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃繘骞戦姀銈呯疀妞ゆ帒鍊瑰▓鎯ь渻閵堝棗绗掗悗姘€鍥х劦妞ゆ帊鐒﹂ˉ鍫⑩偓瑙勬礃閿曘垽銆佸▎鎾村癄濠㈣泛顦遍弫楣冩⒒閸屾瑧顦﹂柟纰卞亰钘濋柟娈垮枓閸嬫挾绮☉妯烘暘闁圭儤顨呯粻娑欍亜閹烘埈妲归悗闈涚焸濮婃椽妫冨☉姘暫婵°倗濮烽…鍫ヮ敋閿濆惟闁挎柨澧介鏇熺節閵忥絾纭炬い鎴濇缁牏鈧綆鍋掑▓浠嬫煟閹邦厽缍戦柣蹇擃嚟閳ь剝顫夊ú婊堝窗閺嶎厹鈧礁鈽夊Ο閿嬵潔濠碘槅鍨板﹢閬嶆儉閸涘瓨鈷掗柛灞剧懅椤︼箓鏌熺喊鍗炰喊閽樻繂霉閸忓吋缍戦柛銊ュ€块弻锝夊閵堝棔姘︾紓鍌氱У閻楃娀寮婚敐鍛傜喖宕楅崗鍏肩槑缂備焦鍎宠ぐ鐐靛垝濞嗘挸钃熸繛鎴炵煯濞岊亪鏌涢幘妤€瀚▍姗€姊绘担鍛婂暈闁哄被鍔庣划鏃囥亹閹烘垼鎽曞┑鐐村灟閸ㄧ懓螞濮椻偓閺岋綁骞囬鈧痪褔鏌涢敐鍫燁仩闁逞屽墲椤煤閺嵮屽殨闁告挷鐒﹀畷鍙夌節闂堟侗鍎愭い銉ョ墛缁绘盯骞嬪┑鍡氬煘闂佸憡鎸烽崡鍐差潖濞差亝瀵犲璺猴攻濞堝爼姊洪崫銉バｉ柣妤冨Т椤曪絾绻濆顒€宓嗛梺缁樻濞咃綁顢欓弴銏♀拺闁告捁灏欓崢娑㈡煕閻樺磭澧柟渚垮妽瀵板嫭绻涢悙顒佹澑闂備焦鏋奸弲娑㈠疮椤愩倕绶ゅ┑鐘崇閻撶喐淇婇妶鍌氫壕闂佹寧娲忛崕宕囧垝濮橆厽缍囬柕濞у懐妲囬梻浣规偠閸庢挳宕洪弽顓炵柧妞ゆ巻鍋撻柍瑙勫灴閹晝绱掑Ο濠氭暘婵犵妲呴崑鍛淬€冩繝鍥╁祦闁糕剝绋掗崐鐑芥倵闂堟稒鎲告い鏃€娲熷娲传閸曞灚效闂佹悶鍔庨弫濠氱嵁閸愵喖绠ｆ繝銏＄箘閹虫捇鈥﹂妸鈺佸窛妞ゅ繐瀚竟鏇炩攽?cli 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊渚楅崕鎰亜閵夈儳澧涚紒缁樼洴楠炲鈹戦崱姘厴闂備礁鎲￠幐缁樼珶閸℃瑦顫曢柟鐑橆殕閸婇攱銇勯幒鍡椾壕闂佸憡鏌ｉ崐鏍Φ閸曨垼鏁囬柣妯诲絻楠炲鎮楀▓鍨灈妞ゎ厾鍏樺顐﹀箛椤撶偟绐炴繝鐢靛Т鐎氼噣寮堕崨濠佺箚闁绘劦浜滈埀顒佺墱閺侇噣骞掑Δ鈧壕鐟邦渻鐎ｎ亪顎楅柛銊︾箞閺岋綁濮€閻樺啿鏆堥梺鎶芥敱閸ㄥ潡寮诲☉妯锋斀闁糕剝顨忔禒鎯ь渻閵堝啫鍔ら柛瀣枔濡叉劙骞掑Δ濠冩櫓闂佸吋浜介崕杈╁閹惰姤鍊垫繛鍫濈仢閺嬫稒銇勯鐘插幋妤犵偛鍟存慨鈧柕鍫濇閸庮亪姊洪棃鈺佺槣闁告瑥绻掑Σ鎰板蓟閵夛腹鎷绘繛鎾村焹閸嬫挻绻涙担鍐叉噺瀹曟煡鏌熼悜妯诲鞍闁告瑥绻橀弻锝夊閻樺啿鏆堥梺绋匡工閻栧ジ寮诲☉銏╂晝闁绘ɑ褰冩慨鏇㈡⒑閹惰姤鏁遍柛銊ュ閹广垹鈽夐姀鈥虫婵☆偆澧楃换鍌氣枔椤撶倣鏃堟偐闂堟稐娌銈冨妼濡繃淇婇悽绋跨妞ゆ牗顕遍埡鍛厪濠㈣埖绋戦惁婊勪繆椤愵剟妾紒缁樼箘閸犲﹥寰勫畝鈧敍鐔兼⒑鏉炵増绁版い鏇嗗洦鍋╅柣鎴ｅГ閸嬪嫮鐥幏宀勫摵闁哄拋鍓欓埞鎴︻敊閺傘倓绶甸梺鍛婃尰閻熲晛顕ｉ锕€围濠㈣泛顑傞幏缁樼箾閹炬潙鐒归柛瀣尰缁绘稒鎷呴崘鍙夘棞婵炲拑缍佸缁樻媴閸涘﹤鏆堟繛鎾寸椤ㄥ﹤鐣疯ぐ鎺戠闁绘劏鏅滈悘浣圭節闂堟稑鈧悂骞夐敓鐘茬厱闁圭儤顨嗛悡鏇㈡倶閻愭彃鈷旈柟鍐叉噺閵囧嫰鏁傞懖鈺冩殼闂佸搫鐬奸崰鏍箖椤斿皷鏀﹂柛顭戝枓閸嬫捇寮介鐐电枃閻庡厜鍋撻柛鏇ㄥ厴閹风粯绻涙潏鍓ф偧闁稿簺鍊濆畷鐢稿焵椤掑嫭鈷戦悷娆忓閵堟挳鏌涢幘瀵哥畼闁瑰箍鍨归埥澶娾枎閹寸姷妲囬梻浣规偠閸庮噣寮插☉姘辩焾闁挎洖鍊归埛鎴︽煕濞戞﹫宸ュ┑顕嗙畵閺屾盯鎮╅幇浣圭杹閻庤娲橀崝鏍崲濠靛棭娼╂い鎾跺Т瀵喗绻濈喊妯活潑闁割煈鍨抽幏鍐晝閳ь剙宓勯梺鍛婄缚閸庮噣宕戦幘鑸靛枂闁告洦鍓欑喊宥呪攽閳藉棗浜濇い銊ワ躬瀹曟椽鎮欓崫鍕吅闂佹寧姊荤划顖炲疾閳哄懏鈷戦柛婵嗗閳ь剙鐖煎畷鎰槈閵忕姷鍔﹀銈嗗笒閸燁垳娑甸崼鏇熺厽闁靛鍠曢柇顖涖亜閵忊槄鑰块柟顕呬邯閸┾偓妞ゆ帒瀚崐鍨旈敐鍛殭缁绢厸鍋撻梺鍝勵槸閻楀嫰宕濇繝鍐洸婵犲﹤鐗婇悡娑㈡煕閵夘喚鍘涙繛鍫熸煥闇夋繝濠傚閸斻倝鏌嶇憴鍕伌妞ゃ垺鐟у☉鐢告煥鐎ｎ偅姣庡┑鐘殿暜缁辨洟宕戝Ο鐓庡灊婵炲棙鎸婚崑妯汇亜閺囨浜鹃悗娈垮枙缁瑩銆佸鈧幃銏焊娴ｉ晲澹曢梻浣哥仢椤戝洨绮绘ィ鍐╃厸濠㈣泛顑呴悞楣冩煕鎼达紕绠伴棁澶嬬節婵犲倻澧㈤柣锝嗘そ閺岋紕浠︾化鏇炰壕闁归绀侀幃鎴︽煙閼测晞藟闁逞屽墮绾绢參顢欓幇鐗堚拻濞达綀顫夐崑鐘绘煕閺傝法鐒哥€规洘鍔欏畷绋课旈埀顒勬嫅閻斿摜绠鹃柟瀛樼懃閻忣亪鏌￠崨顔剧煉闁诡喖缍婂畷鍫曞Ω閵壯呫偡缂傚倷鑳舵慨瀵哥礊婵犲偆娼栨繛宸簻缁犲綊鏌ｉ幇顓炵祷妞ゎ剙顦辩槐鎾存媴缁涘娈梺缁橆殕閹告悂顢氶敐鍡欑瘈婵﹩鍘藉▍銏ゆ⒑缂佹﹩鐒鹃悘蹇旂懇钘濋柕濞炬櫆閳锋垿姊婚崼鐔惰吂婵炴垯鍨圭粈鍐煟閹伴潧澧柣锔藉笧缁辨捇宕掑顑藉亾閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌ょ粙璺ㄤ粵闁告瑥绻橀弻娑⑩€﹂幋婵呯敖缂備胶濮甸崕鎶藉煘閹达附鍋愰柛娆忣槹閹瑩姊洪挊澶婃殶闁哥姵鐗犲濠氬Ω閵夈垺鏂€闂佺硶妾ч弲婊呯懅缂傚倸鍊烽懗鑸垫叏閻㈢數鐭欓柟鐑橆殔妗呴梺鍛婃处閸ㄦ壆鐚惧澶嬬厱闁靛鍠氭禒銏ゆ煙閹呮憼闁告瑥绻橀弻銊モ攽閸℃ê娅ｅ銈呯箚閺呮粓濡甸崟顖ｆ晜闁告洦鍋呭▓缁樼節绾版ê澧查柛搴ゅ亹缁鈽夐姀鐘殿啋闂佺厧鎲￠弲顐﹀箳濡や讲鎷绘繛杈剧导鐠€锕傛倿閻愵兙浜滈柟瀛樼箖閸ゅ洭鏌℃担绋挎殻濠德ゅ煐瀵板嫬鈽夊鎵佹（?
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


// 闂?winget 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐差渻閵堝棗绗掓い锔垮嵆瀵煡顢旈崼鐔蜂画濠电姴锕ら崯鐗堟櫏婵犵數濮崑鎾炽€掑锝呬壕濠殿喖锕ㄥ▍锝囨閹烘嚦鐔兼惞闁稓绀冨┑鐘殿暯濡插懘宕戦崟顓涘亾濮樼厧鏋ら柛鎺撳笚缁绘繂顫濋鐐版睏缂傚倸鍊烽悞锕傗€﹂崶顒€鍌ㄩ柣銏犳啞閳锋垿鎮归崶锝傚亾瀹曞洠鍋撴繝姘厱闁硅埇鍔嶅▍鎾绘煕閻旈澧︽慨濠勭帛閹峰懐鎲撮崟顐″摋闂備胶顭堢€涒晛顪冮挊澶屾殾婵犲﹤鍟犻弸搴ㄦ煙鐎电小缂佹顦—鍐Χ閸℃锛曢梺绋款儐閹瑰洭寮?Cloudflare WARP闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨绘い鎺嬪灪閵囧嫰骞囬姣挎捇鏌熸笟鍨妞ゎ偅绮撳畷鍗炍旈埀顒勫煕閹烘鈷戠紓浣姑粭褔鏌￠崪浣镐喊妤犵偛顦辩划娆忊枎閹勫€梻浣稿閸嬫帒顭块埀顒傜磽閸愨晜绀€闁宠鍨块幃鈺呮嚑椤掍緡妫勯梻浣告惈椤︻垳鑺遍柆宥呯；闁规崘顕ч崡鎶芥煏韫囨洖啸闁汇倕娲弻鐔兼嚌閻楀牆娑х紓浣藉紦缁瑨妫㈠┑顔筋焾閸╂牠鎮￠悢鍏肩厽闁哄啫鍋嗛悞鐐箾閸涱垰浠ч柟鍙夋倐閹囧醇濠靛牏鎳栭梻浣侯焾椤戝棝宕濆Δ鍛劦妞ゆ帒鍊归崵鈧繝銏㈡嚀閿曨亜鐣锋导鏉戝唨妞ゆ挾鍋熼ˇ顓㈡⒒娓氬洤澧紒澶屾暬閸╂盯骞嬮敂鐣屽幗闂佺粯鏌ㄩ幉锛勬閼碱剛纾奸柣娆愮懃濞诧箓鎮￠妷鈺傜厸闁搞儺鐓侀鍫熷€堕柛娆忣樈濞堜粙鏌ｉ幇顓炵祷闁哄棙鐟╅幗鍫曞冀椤撶喓鍘卞┑鐐村灥瀹曨剟寮搁悢鍝ョ闁告侗鍠氶惌鎺楁煛瀹€鈧崰鏍箠閺嶎厼鐓涢柛鏇烇工椤︾敻寮诲鍫闂佸憡鎸婚悷鈺佺暦椤栨粍宕夐柕濞у倻鐟濋梻浣瑰缁诲倿藝娴兼潙鐓曢柟鐑樺殮瑜版帗鏅查柛鏇ㄥ櫘閸斿懘姊虹紒妯虹闁稿鎹囨俊鐢稿礋椤栨氨顓哄┑鐘绘涧濞村倸螞閿曗偓铻栭柣姗€娼ф禒锕傛煕閺冣偓閻楃姵淇婇崼鏇熸櫜濠㈣泛锕﹂ˇ銊╂⒑闂堟丹娑㈠川椤旈敮鍋撻锔解拻闁稿本鐟чˇ锔界節閳ь剟鏌嗗鍡樺劒闁瑰吋鐣崝宀€绮ｅΔ鍛厸鐎广儱楠告禍鐐烘煟椤撶偞顥㈤柟顔筋殜閺佹劖鎯旈埄鍐憾闂備胶绮幐璇裁洪悢鐓庤摕鐎广儱顦悡娑樏归敐鍥剁劸缂佺姾顕ч—鍐Χ閸涱垳顔囬梺缁橆殔閿曨亜鐣烽幇鏉块唶闁哄洨鍠庨埀顒傛暬閺岋綁濮€閵忊剝姣勯梺闈涙閿曨亪寮诲☉姘ｅ亾閿濆骸浜濋悘蹇ｅ弮閺屽秶绱掑Ο璇茬３閻庤娲栫紞濠囥€佸▎鎾村仼閻忕偛銈搁崑妤佺節绾板纾块柛瀣灴瀹曟劙濡舵径濠傚亶婵°倧绲介崯顐ょ矆閸屾稒鍙忔俊鐐额嚙娴滈箖姊虹紒妯圭繁闁革綇绲介悾宄邦潨閳ь剟銆佸▎鎴濇瀳閺夊牄鍔庣粔鍗炩攽閿涘嫬浜奸柛濞垮€濆畷鎴﹀箳濡も偓閸屻劌螖閿濆懎鏆欑紒鈧崼銉︾厪濠电姴绻愰々顒傜磼閻樿崵鐣虹€殿喖鐖煎畷鐓庘攽閸″繑瀵栫紓鍌欑椤︿粙宕滃璺何﹂柛鏇ㄥ灱閺佸啴鏌曡箛濠冩珕闁宠鐗撻幃妤€鈻撻崹顔界仌濠电偛鎳忓ú婊堝箲閵忕姭鏀介悗锝庝簽閸婄偤姊洪棃娑辩劸闁搞劏娅ｉ埀顒佷亢濡嫰鍩為幋锔藉€烽柤鎼佹涧濞懷呯磽娴ｇ懓绲绘繛纭风節楠炲啫煤椤忓嫭顥濆┑鐐叉婢т粙宕濋悜鑺モ拺鐟滅増甯掓禍浼存煕濡搫鈷旂€殿啫鍥х劦妞ゆ帒瀚埛鎴︽⒒閸喓銆掔紒鐘冲哺閺屾盯鍩℃担鍓蹭純濡ょ姷鍋涢澶愬极閸愵喖鐒垫い鎺戝閸嬵亜螖閿濆懎鏆為柣鎾跺Х缁辨帞鈧綆鍘界涵璺好归悩宕囨创闁哄本绋掗幆鏃堟晲閸涱喚浜梻浣告惈鐞氼偊宕濆畝鍕厴闁硅揪绠戦獮銏′繆椤栨繃顏犻柣娑掓櫊濮婄粯鎷呴搹鐟扮闁汇埄鍨辩敮鈥崇暦閵壯€鍋撻敐搴″鐎规洖寮剁换婵嬫濞戝崬鍓板銈庡亜閹虫﹢寮婚妸銉㈡斀闁糕剝锕╁Λ銈夋⒑闂堟稒顥為柛鏃€鐟╁濠氬Χ婢跺浠梺缁橆殔閻楀﹪骞忔潏銊х闁挎繂鎳忛幖鎰版煥閺囥劋閭柣娑卞櫍楠炴鎷犻懠顒夊敽闂備礁鎼崐鍝ヨ姳闁秴鐭楁繛宸簼閳锋帒霉閿濆懏鍟為柟顖氱墦閺岋綁顢楅埀顒勫Χ缁嬭法鏆﹀ù鍏兼綑閸愨偓濡炪倖鎸炬慨瀵哥矈閿曞倹鈷戠紒顖涙礀婢ф煡鏌涢幋鐘冲殌閸楀崬螖閿濆懎鏆為柍閿嬪灴閺岀喓绮欓幐搴㈠闯闂佸疇妫勯ˇ杈╂閹烘挸绶為悘鐐村劤濞堝矂姊烘导娆戞偧闁稿繑锕㈤妴浣糕槈閵忊€斥偓鐑芥煠绾板崬澧婚柛鐔锋搐閳规垿鎮欓懜闈涙锭缂備浇寮撶划娆忕暦閵忕媭鐓ラ柛顐ｇ箘椤斿棗鈹戦悙鍙夆枙濞存粍绻堥崺娑㈠箣閻樺灚锛忓銈嗘尵閸嬬偤宕抽悷鎳婄懓顭ㄩ崘顏喰ㄩ梺鍝勬湰缁嬫捇鍩€椤掑﹦绉甸柛瀣鐓ら柟缁㈠枟閻撴洟鏌嶇憴鍕姢濞存粎鍋撴穱濠囨倷椤忓嫧鍋撻弽顐ｆ殰濠电姴娲﹂崵鍕煠缁嬭法浠涙繛鍛█濮婃椽顢楅埀顒傜矓閻㈠憡鍋傞柡鍥ュ灪閻撴瑩鏌ｉ幋鐐嗘垹浜搁鐘电＜婵°倓绀佸ù顕€鏌″畝瀣М闁轰焦鍔欏畷鍗炩枎閹寸姵婢栭梺璇叉唉椤煤濠婂牆绐楅柡宓懏娈鹃悷婊呭鐢晠寮崘顔界叆婵犻潧妫欓ˉ鐘垫喐閹殿喖鏋涙慨濠傤煼瀹曟帒顫濋钘変壕闁归棿璁查埀顒婄畵椤㈡宕熼銈忕幢闂備礁鎲″ú锕傚磹鐎ｎ€㈡椽顢旈崨顔界彆闂備礁鎲￠幐鍡涘川椤栨艾绠氶梻鍌氬€烽懗鍫曞箠閹炬椿鏁嬫い鎾跺枑閸欏繘鏌ｉ姀銏╃劸缂佲偓閸屾稓绡€濠电姴鍊绘晶娑㈡煟閹惧啿鏆熼柟鑼焾椤劑宕煎┑鍫Ф闂備胶绮弻銊┿€冩惔鈽嗙劷闁哄稁鍘介悡锝夌叓閸ャ劍灏伴悹鎰剁磿閻ヮ亪骞嗚閹垵菐閸パ嶈含妞ゃ垺绋戦～婵嬵敇閻斿嘲澹嶅┑锛勫亼閸娿倝宕戦崨鏉戝瀭闁秆勵殔閽冪喖鏌ㄥ┑鍡欏ⅱ闁汇倐鍋撻梻浣筋潐閸庤櫕鏅舵惔顭戞晩濠㈣埖鍔栭埛鎴︽煠婵劕鈧洟寮抽柆宥嗙厸闁告侗鍨板瓭闂佷紮绲介崲鑼弲濡炪倕绻愮€氼噣宕濋敃鈧—鍐Χ閸℃鐟愰梻鍌氬缁夌數绮嬪鍜佺叆闁割偆鍠撻崢鐢告⒑閸涘﹦绠撻悗姘煎弮钘熸繝濠傚幘閻熼偊鐓ラ柛鏇ㄥ亜婵垹顪冮妶搴濈盎闁哥喎鐡ㄦ穱濠囨嚋閸偄鍔呴梺鐐藉劚绾绢參顢欓幒妤佲拻濞达絼璀﹂悞鍓х磼缂佹ê鐏撮柡浣割儏椤啴濡堕崱妯尖敍缂備焦褰冨锟犵嵁韫囨稒鍋愰悷浣风濠€閬嶅焵椤掑﹦绉甸柛瀣笒椤洭寮埀顒傛崲濠靛牆鏋堟俊顖涙た濞兼垿姊洪幖鐐插闁绘牜鍘ч锝夊Ω閳哄倻鍔堕悗骞垮劚濡鍩€椤掑嫭娑ч柍瑙勫灴閹晛鐣烽崶鑸垫闂備焦濞婇弨閬嶅垂瑜版帒绠熼柟闂寸劍閸嬪鏌涢锝囩畼闁荤喆鍔嶇换娑氣偓娑欘焽閻棝鏌涘Δ浣糕枙鐎殿喛顕ч埥澶愬閻樻鍟嬮梺璇叉捣閺佸摜娑甸懜鍨弿闁搞儮鏅濈壕钘壝归敐鍥剁劸闁逞屽墮濠€閬嶅极椤曗偓閺佹捇鎮╅棃娑氥偊婵犳鍠楅妵娑㈠磻閹炬番浜滈柕蹇ョ磿閹冲懘鏌熼崣澶嬪唉闁轰焦鍔欏畷鍗炍旀繝浣烘そ濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰▕閻掕姤绻涢崱妯诲碍閻熸瑱绠撻幃妤呮晲鎼粹剝鐏嶉梺绋款儜缁辨洟骞夊宀€鐤€闁哄洨鍋涢悘濠冪節閻㈤潧孝婵炶绠撳畷鎰節濮橆厾鍘鹃梺璇″幗鐢帡宕濋妶澶嬬厱婵せ鍋撳ù婊嗘硾椤繐煤椤忓嫪绱堕梺鍛婃处閸嬧偓闁稿鎸剧划娆忊枎閸撗冨Ш闂備浇娉曢崰鎾存叏閹绢喗鍎楁俊銈呮噺閻撴洟鏌￠崶銉ュ濠⒀呯帛缁绘盯宕煎鍛厯闂佸搫鐭夌紞渚€鐛崶顒夋晢濞达絽寮堕悘鍡樼節绾版ǚ鍋撻崘鑼獓闂佽鍠栭崐鍧楃嵁濡ゅ懎閱囬柡鍥╁仜閸炪劌顪冮妶鍡樺暗闁哥姵鎹囧畷銏ゅ础閻愨晜鏂€闂佺粯鍔栧娆撴倶閵壯€鍋撶憴鍕闁告挾鍠栭幃浼搭敋閳ь剙顕ｆ禒瀣垫晝闁靛牆娴勭槐鏌ユ⒒娴ｅ摜绉洪柛瀣躬瀹曘垺銈ｉ崘銊у弨婵犮垼娉涢鍐╃濠婂嫨浜滈煫鍥ㄦ尭椤忊晠鏌￠崱顓犲埌闁宠鍨块崹鎯х暦閸パ呭幗闁诲氦顫夊ú鏍х暦椤掑啰浜介梻浣虹帛閸ㄦ儼鎽銈嗘煥閸婂灝顫忔繝姘＜婵炲棙甯掗崢鈥愁渻閵堝骸骞栭柣妤佹崌閵嗕線寮介鐐靛€炲銈嗗坊閸嬫挾绱掗幇顓ф疁妤犵偞鐗曡彁妞ゆ巻鍋撻柍褜鍓氶〃鍡欑矉瀹ュ瀵犲璺烘閺傗偓闂備胶绮崝鏇㈡偤閵娧傜剨濞寸厧鐡ㄩ悡鏇㈡煃鏉炴壆顦﹂柡鍡悼閳ь剝顫夊ú姗€宕归崸妤冨祦婵☆垰鐨烽崑鎾斥槈濞咁収浜滈?
// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婄拋鏌ュ几濞嗘挻鈷戠紓浣姑粭鈺佲攽椤斿搫鈧骞戦姀鐘闁靛繒濮撮懓鍨攽閳藉棗鐏ユい鏇嗗懎鏋堢€广儱顦伴悡鐔兼煟閺傛寧鎲搁柟铏礈缁辨帡鎮╅搹顐㈢３濡ょ姷鍋涢崯顐ョ亙闂佸憡渚楅崰妤€鈻嶅鍫熺厵闁兼祴鏅炶棢闂佸憡鎸荤换鍫ュ箖濡警鍚嬪璺侯儌閹锋椽姊虹涵鍛汗闁稿鐩崺鈧い鎺嶇劍缁€瀣煙瀹曞洤鏋涙い銏＄☉閳规垿宕煎鍛板厭闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噺瀹曟煡鏌涘畝鈧崑娑氱不閻樿绠规繛锝庡墮婵″吋鎱ㄧ憴鍕弨闁哄矉缍侀獮鍥敊閻撳骸顬嗛梻浣虹帛閹歌煤閻旂厧钃熼柡鍥ュ灩闁卞洦绻濋棃娑欐悙婵炲懌鍊濆娲偡閺夋寧鍊梺鍛婃⒐閻熲晛顕ｇ拠宸悑闁搞儯鍔岄弸鍌炴⒑閸涘﹥澶勯柛瀣鐎电厧鐣濋崟顑芥嫼闁荤姴娲犻埀顒冩珪閻忊偓闂備礁鎼幊鎰箾閳ь剟鏌ｅ☉鍗炴珝妤犵偞顭囬幏鐘绘嚑椤掑鏁介梻鍌欑濠€閬嶅磻閹捐绠氱€光偓閸曨偆鏌堥梺閫炲苯澧存慨濠傤煼瀹曟帒顫濋钘変壕濡炲娴烽惌鍡椕归敐鍛儓妞ゃ儲纰嶉妵鍕箻鐠虹洅銏ゆ煕閵婏妇绠栭柕鍥у瀵粙鈥﹂幋婵囶啌婵犵數鍋為崹顖炲垂閸︻厾鐭嗛柛鎰ㄦ杺娴滄粓鏌￠崘銊︽悙妞ゃ儱绉归弻鐔兼嚍閵夛妇顦伴梺璇″枟椤ㄥ﹪鎮伴鈧畷褰掝敊閻撳寒娼涢梻鍌欒兌缁垶銆冮崱娆忓灊鐎光偓閸曘劉鍋撻弽銊х瘈闁搞儺鐏涢埡鍛厓闁告繂瀚埀顒傛暬楠炲繐煤椤忓應鎷洪梺鍛婄☉閿曪箓鍩ユ径鎰叆闁哄洦锚閸斻倕霉濠婂啯鍟為悗浣冨亹閳ь剚绋掗…鍥储娴犲鈷戠紓浣股戦悡銉р偓娈垮枦濞夋盯鍩㈠鍡欑瘈闁告洏鍔屾禍?UAC闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋垺鍋橀柍銉ュ帠婢规洖鈹戦悙鏉戠仸闁荤噥鍨堕幃鐐烘偩瀹€鈧壕濂告煟濡搫鑸圭€规挸妫濋弻锛勪沪鐠囨祴鍋撳┑瀣祦闊洦娲戝▽顏堟煕閹炬ぞ璁查崑鎾寸節閸曨厾锛濇繛杈剧导缁瑩宕ú顏呭仺妞ゆ牗绋戠粭鈺呮煟韫囨挸鏆ｆ慨濠冩そ瀹曘劍绻濋崒姘兼綂闂備礁鎼幊蹇曟崲閸℃稑桅闁圭増婢樼粈?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐烘偡濠婂啰绠荤€殿喗濞婇弫鍐磼濞戞艾骞楅梻渚€娼х换鍫ュ春閸曨垱鍊块柛鎾楀懐锛滈梺褰掑亰閸欏骸鈻撳鍫熺厸鐎光偓閳ь剟宕伴弽顓炶摕闁搞儺鍓氶弲婵嬫煃瑜滈崜鐔风暦濠靛绠抽柟鎯у椤旀洟鎮楅悷鏉款棌闁哥姵娲滈懞閬嶆嚒閵堝洨锛滈梺缁樼懃閹虫劗绮欓懡銈囩＜缂備焦顭囩粻鐐测攽椤旂懓浜鹃梻浣瑰缁嬫垹鈧凹鍣ｉ幃楣冩偨閻㈢數锛濋梺绋挎湰濮樸劌鐡梻浣瑰濞插繘宕愬┑瀣畺濡わ絽鍟崐濠氭煢濡警妯€闁哥偛鐖煎娲传閸曨剙鍋嶉梺鍛婃煥缁夌懓顕ｉ弻銉﹀€烽柣鎴炃氶幏娲⒑閸︻収鐒鹃悗娑掓櫇婢规洟鎮介崨濠勫幈闁诲函缍嗛崑鍛焊娴煎瓨鐓忛柛銉戝喚浼冮梺杞扮閸婂潡骞愭繝鍐ㄧ窞婵☆垱妞垮Λ鍡涙⒒閸屾瑨鍏岄柟铏尵缁顓兼径濠傜€悷婊呭鐢帡鎮″鈧弻鐔衡偓鐢殿焾琚ラ梺绋款儐閹瑰洭寮幇顓熷劅婵犻潧鐗婇弲濂告⒒娴ｄ警鐒鹃柨鏇畵瀹曟垿濡舵径濠勭暫閻庣懓瀚伴崑濠囨偂閵夆晜鐓曢柟鎵暯閸嬫捇顢涢崱妤佸闁绘挻鐟﹂妵鍕即濡も偓娴滄儳鈹戦悙鏉垮皟闁搞儵顥撶粙蹇撯攽閻樿宸ラ柣妤€妫濋幃鐐哄垂椤愮姳绨婚梺鍦劋閸ㄧ敻顢旈埡鍐＜闁绘ê纾晶顒€菐閸パ嶈含濠碘€崇埣瀹曘劑顢欓崗纰变槐闂佽姘﹂～澶愬箰妞嬪孩顐芥慨妯挎硾閽冪喐绻涢幋鏃€鍣伴柍褜鍓ㄧ粻鎾荤嵁鐎ｎ亖鏀介柛銉㈡櫃闁垱绻濋悽闈浶ｆい鏃€鐗犲畷浼村冀椤撶喓鍘愰梺鎸庣箓閹峰顭囬弽銊х鐎瑰壊鍠曠花濂告煕鐎ｅ墎绡€闁哄备鍓濆鍕偓锝庝憾閸ゅ鈹戦鍡欑ɑ闁告梹鐟ラ～蹇曠磼濡顎撶紓浣割儓濞夋洟鎯勬惔锝囩＝闁稿本姘ㄥ皬缂備浇鍩栭懝楣冾敋閿濆鏁冮柨鏇楀亾缂佺姵鐩弻锝呂旈埀顒勬偋閸℃稑鐒垫い鎺戝€搁崢鎾煛鐏炲墽鈽夐柍瑙勫灴瀹曞崬螖閸愩劌鍤戦梺鑽ゅ枑缁瞼绮旇ぐ鎺戠伋闁挎梻鍋撳畷澶愭煏婵犲繒鐣辩痪缁㈠灡缁绘盯骞橀弶鎴濇瘓闂佹悶鍔嬮崡鍐茬暦濠婂牆绠柤鎭掑劗閹锋椽姊洪崨濠勨槈闁愁垱娲熼、鏃堝礋椤撶喐顔曟俊鐐€栫敮鎺斺偓姘煎墴閹€愁潨閳ь剟寮婚悢鍛婄秶濡わ絽鍟宥夋⒑缁嬫鍎愰柛鏃€鐟╅獮鍐亹閹烘垹鍊為梺鎼炲劀閸愨晜顓跨紓鍌欑劍閸炶崵绱炴担鍓叉綎缂備焦蓱婵挳鏌涘☉姗堟敾闁哥姵鍔欏娲传閵夈儛銏㈢磼婢跺本鍤€闁伙絿鍏橀弫鎾绘偐閼碱剦鍞撮梻浣藉Г閿氭い锕備憾閹儳煤椤忓應鎷虹紓鍌欑劍閿氬┑顔兼喘閺岀喓鍠婇崡鐐扮敖缂備緡鍠栭…鐑藉箖閻戣棄绠犲┑鐘插€荤粔娲煛娴ｇ懓濮嶇€规洏鍔戦、娆戞崉椤垶鏁ら梻鍌氬€风粈渚€骞夐檱閹筋偊姊洪崷顓熷殌婵炲眰鍊濋敐鐐剁疀閹句焦妞介、鏃堝川椤撶噥鍚嬮梻鍌欑窔濞佳囨偋閸℃稑绠犻煫鍥ㄧ⊕閸婄敻鏌ｉ幇闈涘缂佲檧鍋撻梻渚€娼х换鍫ュ垂濞差亜鍚归柍褜鍓欓埞鎴﹀煡閸℃ぞ绨奸梺鑽ゅ暱閺呮盯顢氶敐澶婅摕闁靛鍎欓妸鈺傜厓闁告繂瀚埀顒€顭峰畷銏＄鐎ｎ偆鍘甸梺绋跨箺閸嬫劙寮冲鈧弻娑欑箾閸喒鍋撻弴鈶┾偓锕傚炊椤掆偓缁犳稒銇勯幘璺盒ユい鏃€娲熷娲川婵犲嫭鍣у銈忕細缁瑩骞冮悙瀵割浄閻庯綆鍋嗛崢闈浳旈悩闈涗粶闁诲繑绻堥幃姗€鎳犻钘変壕閻熸瑥瀚粈鈧梺闈涚墕閹测剝绌辨繝鍥ㄥ€婚柦妯猴級閵娧勫枑鐎光偓閳ь剙危閹邦剦鐓ラ柛顐ゅ暱閹风粯绻涙潏鍓ф偧闁硅櫕鎹囬、姘煥閸曨厾顔曢梺鍛婄懃椤﹁鲸鏅堕悽纰樺亾鐟欏嫭绀冮柛鏃€鐟ラ悾鐑筋敃閿曗偓鍞悷婊冪灱缁厽寰勯幇顓涙嫼闁荤喐鐟ョ€氼剟宕濈捄琛℃斀妞ゆ柨鎼埀顒佺箞楠炲啴鍨鹃弬銉︻潔闂侀潧楠忕槐鏇㈠储闁秵鈷戦悷娆忓椤ユ劙鏌￠崨顔锯姇濞ｅ洤锕幃锟犵嵁閻旂儤鍤€妞ゎ厹鍔戝畷銊╊敇濠靛洤濯版繝鐢靛剳缁茶棄煤閵堝鏅濇い蹇撳閺嗭箓鏌ㄥ┑鍡橆棞缂佸墎鍋ら幃妤呮晲鎼存繄鐩庨梺琛″亾妞ゆ挾鍠撶弧鈧┑鐐茬墕閻忔繈寮稿☉銏＄叆闁哄浂浜滈々顒勬煕閹烘挸娴い銏＄洴閹瑩鎳犻澶婃暩闂傚倷绀侀幉锛勭矙閹达附鍋ら柕濞炬櫅绾惧鏌ㄩ悢鍝勑ｉ柍閿嬪灦閵囧嫰骞橀崡鐐差瀷闂佷紮缍€妞村摜鎹㈠☉銏犻唶闁绘棁娅ｉ悾鑲╃磽娴ｄ粙鍝洪柟鐟版搐閻ｇ兘骞掗幋鏃€鐎婚梺瑙勬儗閸樺€熲叺婵犵绱曢崑鎴﹀磹閺嶎厼鍨傞梻鍫熷厷濞戞ǚ鏀介柛鈩冪懄濞堥箖鎮峰鍛暭閻㈩垱甯￠弻銊╁Χ閸℃洜绠氶梺闈涚墕閸婂憡绂嶉崜褏纾藉ù锝呮惈鍟告繝鐢靛亹閸嬫捇姊洪崫鍕効缂傚秳绀侀锝夘敋閳ь剙鐣烽幒鎳虫梹鎷呴崫銉ラ檷闂傚倸鍊峰ù鍥ь浖閵娧呯焼濞达綀娅ｉ惌鎾舵喐閻楀牆绗掗柦鍐枛閺屻劌鈹戦崱鈺傂︾紓浣哄У缁嬫帡濡甸崟顖氱閻犺櫣鍎ら悘浣虹磽娴ｅ壊鍎忔繛璇у閹广垹鈽夊锝呬壕闁汇垺顔栭悞鍓р偓娑欑箞濮婃椽妫冨☉姘叡濡炪値鍘奸悧鎾绘晲閻愬墎鐤€闁瑰彞鐒﹀浠嬨€侀弮鍫濇そ濞达綀顕栧Λ鍐ㄢ攽閻樺灚鏆╅柛瀣仱閹箖顢旈崼婵堬紮闂佸搫绋侀崢浠嬪磿鎼搭潿浜滈柡宥冨妽閻ㄦ垿鏌￠崱顓犵暤闁哄矉缍佸顕€宕掑Δ浣哥彵闂備浇妗ㄧ粈渚€宕幘顔艰摕闁靛鍎弨浠嬫煕閳╁喛渚涙俊顐犲€濋幃宄邦煥閸涱収鏆┑锛勫仩濡嫰锝炶箛鏃傜瘈婵﹩鍓涢敍婊冣攽椤旀枻渚涢柛蹇旓耿瀹曟垿骞橀幖顓熜ч柟鑹版彧缁插潡鎮為崗鑲╃闁圭偓娼欓悞褰掓煕鐎ｎ偅灏版い銊ｅ劦閹瑥顔忛瑙ｆ瀰闂備胶纭堕弲娑氣偓姘緲閻ｅ嘲顫滈埀顒勩€侀弮鍫濆耿婵☆垳鍎ら悿鍕節閻㈤潧孝闁汇儱顦靛鑸垫償閹惧厖澹曢梺鍝勬储閸ㄥ湱绮婚悩璇茬婵烇綆鍓欐俊鑲╃棯閹佸仮闁哄本娲樼换娑㈠垂椤旂厧顥欓梻浣告惈閸燁偊鎮ф繝鍥х柧闁哄被鍎查悡鏇熺箾閹存繂鑸规鐐搭殕缁绘盯宕奸銏犵闂佸疇顫夐崹鍧椼€佸▎鎾村€锋い鎺嗗亾婵炲牆鐭傞幃妤€鈻撻崹顔界亪闂佺粯鐗曢妶鎼佹偘椤曗偓瀹曞ジ濡烽妷搴樻櫊閺屾洘寰勬繝鍐╁闯闂佸摜濮甸悧鏇㈡偩閻ゎ垬浜归柟鐑樼箖閺呪晠姊虹拠鈥崇€婚柛鎰典簽缁愭姊婚崒姘偓鎼佸磹閻戣姤鍤勯柛鎾茬閸ㄦ繃銇勯弽顐粶缂佺姳鍗抽幃褰掑炊椤忓秵鈷栧┑鐐叉▕娴滄繈宕戦崟顖涚厽闁规崘娅曢崬澶愭煙閼恒儲绀嬫慨濠呮缁辨帒螣閼姐倕寮抽梻浣告啞閼归箖顢栨径濠勬殾闁归偊鍠氱壕鍏间繆椤栨繍鍤欑€殿喖鐏濋埞鎴炲箠闁稿﹥鍔欏畷鎴﹀箻缂堢姷绠氬銈嗗姧缁插潡骞婇崶顒佺厵妞ゆ洍鍋撶紒鐘崇墵楠炲啫顭ㄩ崼鐔锋疅闂侀潧顦崹铏光偓姘偢濮婃椽宕崟鍨ㄩ梺绋款儑閸嬬偟绮╅悢鐓庡耿婵炲棙鍨归獮鎾绘⒑閸濆嫬鈧綊鏁嶉弴銏″仭闂侇叏濡囬崣鍡涙⒑閸涘﹤濮﹀ù婊勭矒閹繝鎮㈤崗鐓庘偓鍨箾閸繄浠㈤柡瀣⊕閵囧嫰顢橀悩鎻掑箣閻庢鍣崑鍕敇閸忕厧绶炲┑鐘插濡茶淇婇悙顏勨偓鏇犳崲閹版澘绠犻幖娣灩閸ㄦ柨霉閻撳海鎽犻柍閿嬪笒闇夐柨婵嗘噺閸熺偤鏌熼姘卞ⅵ闁哄本绋掔换婵嬪磼濠婂啰鍘介柣搴ゎ潐濞叉﹢宕濋弴銏犵叀濠㈣埖鍔曢～鍛存煟濡吋纾柡鍥╁剱濞撳鏌曢崼婵囶棞濠殿啫鍥ㄢ拻闁告洦鍋勯顓㈡煕閳哄倻娲存鐐差儔閺佸倿鎮剧仦钘夌疄闂傚倷鐒︾€笛兾涙笟鈧、姘愁槻妞ゆ洩绲鹃妶锝夊礃閳哄啫骞堥梻浣告惈閸熺娀宕戦幘鍓佺＜妞ゆ柨鍚嬮幑锝嗕繆閸欏濮嶆鐐村笒铻栭柛鎰╁妼缁插吋淇婇悙顏勨偓鏍箰閻愵剚鍙忛柣鎴ｅГ閸嬪倿鏌熼崜褏甯涢柍閿嬪灴閺岋綁骞囬娑辨闂佸憡姊婚弫鍝ユ閹烘鍋愰柧蹇ｅ亜闂夊秵绻涢敐鍛悙闁挎洦浜悰顕€寮介妸锕€顎撻梺鍛婄缚閸庨亶鐛幇顑芥斀闁绘劘鍩栬ぐ褏绱掗懠璺轰汗闁瑰嘲鍟撮弫鍐焵椤掆偓瀹撳嫭绻涚€电甯堕柣掳鍔戦崺娑㈠箳閹存瑢鍋撻幒鎴僵闁挎繂鎳嶆竟鏇熺節閻㈤潧浠掗柛鏍█瀹曞崬螖娓氬洣鍝楅梻鍌氬€峰ù鍥敋瑜忛幑銏ゅ箳濡も偓绾惧鏌ｉ幇顕呮毌闁稿鎸搁～婵嬵敆娴ｅ憡鏁俊鐐€х粻鎺楋綖婢舵劕绠為柕濠忓缁♀偓闂佺琚崐妤呭触鐎ｎ喗鍊甸悷娆忓缁€鍐偨椤栨稑娴柛鈹垮灪閹棃濡搁妷褜鍞洪梻浣烘嚀閻°劎鎹㈤崒鐐村殘闁归偊鍠氱壕浠嬫煕鐏炴崘澹橀柍褜鍓熼ˉ鎾斥枎閵忋倖鏅搁柣妯荤叀濡嘲顪冮妶鍡樼５闁稿鎸鹃埀顒冾潐濞叉牜绱炴繝鍌滄殾濠靛倻顭堥崡鎶芥煟閹邦剙鎮佺紒杈ㄧ矒濮婄粯鎷呴悷閭﹀殝濠电偛寮堕悧鐘茬暦閹版澘绠瑰ù锝呮贡閸樻垿姊洪幖鐐插姶闁告挻宀搁幃陇绠涢幙鍐數闁荤娀缂氬▍锝嗘櫠閺囥垺鐓曢柡鍐ｅ亾缂侇喖绉电粚杈ㄧ節閸屻倕鏅犲銈嗘⒒閸樠囧煕鐎ｎ亖鏀芥い鏃傘€嬮弨缁樹繆閻愯埖顥夐柣锝囧厴椤㈡洟鏁冮埀顒傜矆鐎ｎ偁浜滈柟鎯ь嚟閳洟鏌ｆ惔銊ゆ喚婵﹦绮幏鍛村川婵犲倹娈橀梻浣瑰瀹€鎼佸蓟閺囥垹閱囬柣鏃傤焾閳ь剚鍔栭〃銉╂倷閹绘帗娈婚梺璇″枟閻熲晛鐣烽锕€绀嬫い鎰╁€愰崑鎾活敇閻戝棙瀵岄梺闈涚墕濡鎱ㄦ径濞炬斀妞ゆ棁鍋愰敍宥夋煠妤﹀潡鍝虹紒缁樼箓椤繈顢樺☉娆忣伖闂傚倷娴囧畷鐢稿窗閹邦厾鐝堕柛鈩冪☉缁犳椽鏌ｅΟ鍏兼毄缁炬儳銈搁幃褰掑炊閵娿儳绁烽梺鍝勬４缁插潡鍩€椤掑喚娼愭繛鍙夛耿瀹曞綊骞愭惔婵堢畾闂佺粯鍨兼慨銈夊疾濠婂牊鍋ｉ柟顓熷笒婵″潡鏌ｉ敐澶岀暫婵﹦绮幏鍛村川婵犲倹娈橀梻浣告贡椤牆螞濠靛棛鏆︽慨妞诲亾濠碘剝鎮傛俊鐤檨闁告艾鎳樺?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帗绂掓ィ鍐┾拺缂佸顑欓崕宥夋煕婵犲啰绠炵€殿喖顭峰鎾偄閾忚鍟庨梻浣虹帛閸旓箓宕滃鑸靛仧闁哄啫鐗婇埛鎴︽煕濞戞﹫榫氬瑙勵焽缁辨挸顓奸崪鍐惈闂佺粯渚楅崳锝咁嚕娴犲鈧牠濡烽妷鈺佸及濡炪們鍨洪〃濠囧春閳ь剚銇勯幒宥嗩樂缂佽妫濋弻銈吤圭€ｎ偅鐝栧┑鈽嗗亽閸ㄥ爼寮婚悢鐑樺枂闁告洦鍋勮婵犵數鍋涢崥瀣礉閺団€崇倒闂備焦鎮堕崕婊冾吋閸繃鍎撴繝鐢靛Х椤ｄ粙鍩€椤掆偓绾绢參宕洪敐鍥ｅ亾鐟欏嫭纾搁柛鏃€鍨块妴浣割潨閳ь剟骞冮姀銈呯闁圭粯甯楅弳銏ゆ⒒?CLI闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕绀傞柣鎾崇岸閸嬫挾绱掑Ο璇插伎濠德板€愰崑鎾翠繆椤愶絿鎳囬柟顕€绠栭幃婊堟嚍閵夈垺瀚奸梻浣侯攰閸嬫劙宕戝☉銏犵劦妞ゆ帊鐒︾粈瀣煙瀹曞洤鏋涙い銏＄☉閳规垿宕煎鍛板厭闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噺瀹曟煡鏌涘畝鈧崑娑氱不閻樿绠规繛锝庡墮婵″吋鎱ㄧ憴鍕弨闁哄矉缍侀獮鍥敊閻撳骸顬嗛梻浣虹帛閹歌煤閻旂厧绠栨俊銈呮噺閺咁剟鏌涘Δ鍐ㄤ粶妞ゅ浚浜弻锝呪攽閸パ冩懙濠殿喖锕ュ钘夌暦椤愶箑绀嬫い鎾跺Т婵晫绱撻崒娆戝妽鐟滄澘鍟撮崺鈧い鎺戝€搁弸鏂棵瑰鈧划娆忣潖缂佹ɑ濯撮柛娑橈工閺嗗牏绱撴担绛嬪殭闁稿﹤顭烽崺鈧い鎺嶇閸ゎ剟鏌涘Ο鍨棈妞わ綁绠栧楦裤亹閹烘垳鍠婇梺鍛婃惈缁犳垿濡撮幒鎾剁瘈婵﹩鍘鹃崢顏堟⒑閸撴彃浜濈紒璇插暣钘熼柣鎰暩绾惧ジ鏌嶈閸撴艾顕ラ崟顒傜瘈閹兼番鍨归惁婊堟⒑鐠囨彃顒㈢紒瀣笩閹筋偊姊虹粙鍖℃敾闁告梹鐗犻妴鍐Ψ閳哄倸鈧兘鏌ょ喊鍗炲闁愁亜鐏氱换婵堝枈濡搫鈷夐梺璇″枛閸婅绌辨繝鍥ㄥ€荤紒娑橆儐閺呪晠姊洪懞銉冾亪藝鏉堚晜顫曢柨鏇炲€归埛鎺楁煕鐏炲墽鎳嗛柛蹇撶灱閻ヮ亪顢橀悙闈涚厽閻庤娲樼划宀勫煘閹寸姭鍋撻敐鍐ㄥΨ闁稿鎹囧畷姗€顢欓悡搴ｇ崺闂佸搫顦悧鍡樻櫠閻ｅ瞼鐭欓煫鍥ㄧ⊕閳锋垿鏌涢幇顒€绾ч柟顖氱墦閺岀喖宕橀懠顒傤啋闂佽鍣换婵嬪箖閵忋倕绀傞柤娴嬫櫅鐢箖姊绘繝搴′簻婵炲眰鍊濋弫鍐Ψ閵夈垺鐎洪梺鍛婄☉閻°劑鎮￠悢鑲╁彄闁搞儯鍔嶉埛鎺撶箾閸欏灏﹂柡宀嬬磿娴狅箓宕滆婵洭姊虹拠鈥崇仭婵☆偄鍟村顐﹀箛閺夊灝鑰垮┑鈽嗗灥瀹曢潧危濡も偓閳?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栭梻鍌欑閹诧繝骞愰崱娑樺窛妞ゆ梻鐡旈崯鍛存⒒閸屾瑦绁版い鏇嗗應鍋撳☉鎺撴珚闁硅櫕绻冮妶锝夊礂椤栨碍澶勯悗闈涖偢瀵爼骞婄粵鍦暤闁哄本鐩鎾Ω閵壯傚摋缂傚倷鑳舵慨鍨箾婵犲洤钃熼柣鏃囥€€閸嬫挸鈽夊▎妯煎姺缂備胶濮伴崕閬嶆箒闂佺粯顭堝畷鐢告偩閻戞ɑ鍙忓┑鐘叉噺椤忕姷绱掗鐣屾噰鐎规洜顭堣灃濠电偞鍎虫禍鐐繆閵堝嫰妾峰ù婊勭矒閺屻劑寮崒姘闂佸搫妫崜娑㈠焵椤掍緡鍟忛柛鐘崇洴椤㈡俺顦归柛鈹垮劜瀵板嫭绻涢姀銏犳灈闁哄被鍔庨埀顒婄到閻忔氨绱炵仦瑙ｆ斀闁绘ê鐏氶弳鈺呮煕鐎ｎ偆鈽夐摶鐐寸箾閸℃ɑ灏紒鈧径瀣╃箚闁靛牆鎳庨弳鐔兼煟椤撶喓鎳囬柡灞剧洴婵＄兘顢欓悡搴浇闂?winget 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐差渻閵堝棗绗掓い锔垮嵆瀵煡顢旈崼鐔蜂画濠电姴锕ら崯鎵不缂佹﹩娈介柣鎰綑閻忔潙鈹戦鐟颁壕闂備線娼ч悧鍡涘箠閹扮増鍋柍褜鍓氭穱濠囨倷椤忓嫧鍋撻弽顬稒鎷呴懖婵囩洴瀹曠喖顢楁担绋垮Τ濠电姷鏁告慨鏉懨洪敃鍌氱厱闁瑰濮甸崰鎰版煟濡も偓閻楀棛绮幒鎳ㄧ懓顭ㄩ埀顒勫础閹惰棄钃熸繛鎴旀噰閳ь剨绠撻獮瀣攽閸♀晜娴囩紓鍌氬€风粈渚€宕愰崫銉х煋鐟滅増甯掔粻鏍煟閹达絽袚闁绘帊绮欓弻銊╁籍閳ь剟宕曢幎钘夋瀬闁归棿鐒﹂埛鎴︽煠婵劕鈧洟鍩㈤崼銏㈢＝闁稿本绋栨竟姗€鏌嶇拠鑼ч柡浣瑰姈瀵板嫮鈧綆浜栭崑鎾诲醇閺囩偟楠囬梺鍓插亝绾板秴鈻嶉崶鈺冩／闁诡垎浣镐划闂佸搫鐭夌徊鍊熺亽闂佸湱顭堝ù鐤亹瑜嶉—鍐Χ閸愩劌濮庨梺缁橆殕閹瑰洤顕ｆ繝姘櫖闁告洦浜濋崟鍐⒑閸涘﹥瀵欓柍褜鍓欏嵄闁割偁鍎查埛鎴︽倵閸︻厼校妞ゃ儱顦伴妵鍕晝閸屾稒閿梺浼欑悼閸忔﹢寮幘缁樺亹闁告劖绁撮崑鎾绘倻閼恒儱鈧敻鏌ｉ姀鐘冲暈婵炲懏绮撻弻鐔兼嚑椤掆偓娴滃墽绱?Cloudflare 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐差渻閵堝棗绗掓い锔垮嵆瀵煡顢旈崼鐔蜂画濠电姴锕ら崯鐗堟櫏闂備焦瀵х换鍕磻濞戙垹鐓橀柟瀵稿Л閸嬫捇鏁愭惔鈥茬凹濠电偛鎳庡ú顓烆潖閾忓湱鐭欓悹鎭掑妿閸旇绻濈喊妯峰亾閾忣偄绐涚紓渚囧枟閻熴劎绮诲☉銏犵睄闁稿本绮庡Σ鍥⒒娓氣偓濞佳勵殽韫囨洖绶ら柛鎾楀嫬鍘规俊銈忕到閸燁垶宕愰崹顐ょ闁瑰鍋涚粭姘箾閸涱叏鏀绘い銊ｅ劦閹瑧鎷犺娴兼劙姊烘导娆戠ɑ缂佽鐗嗛悾鐑芥偄绾拌鲸鏅為梺鍏间航閸庨亶顢旈悜妯肩瘈闁汇垽娼ф禒鈺呮煙濞茶绨界€垫澘锕ョ粋鎺斺偓锝庝簽閻ゅ懘姊虹捄銊ユ灁濠殿喖顕划璇差潩閼哥數鍘卞銈嗗姉婵挳宕濆鍫熺厽闁规崘娉涢弸鎴︽煏閸パ冾伃妤犵偞甯″畷鍗烆渻閹屾闂傚倸鍊风欢姘焽瑜旈幃褑绠涘☉鎺嗗亾閿曞倸閱囬柕澶涚畱娴犲ジ姊绘担鍝ヤ虎妞ゆ垵鎳橀、姘舵焼瀹ュ棛鍘告繝銏ｆ硾椤戝懘鎮橀埡鍌樹簻闁哄啠鍋撻悽顖ょ節瀵鏁愭径瀣汗闂佸憡鐟ラˇ閬嶅煘濞戙垺鐓熼煫鍥ㄦ尵缁犳煡鏌ｉ悢鍙夋珔闁伙絽鍢查埞鎴犫偓锝庡亝濞呭洭姊洪柅鐐茶嫰婢ф挳鏌嶉妷锔筋棃鐎规洘锕㈤、娆撳床婢诡垰娲﹂悡鏇㈡煃閳轰礁鏋ゆ繛鍫燂耿閺岋綁鎮㈤弶鎴濐潎濠殿喖锕ㄥ▍锝囧垝濞嗘挸绀岄柍銉ュ帠缁憋絿绱撻崒姘偓鍝ョ矓閹绢喗鏅濋柕鍫濐槹閸嬧晠鏌ｉ幋锝呅撻柛瀣ㄥ妿缁辨帞鈧綆鍘界涵鍫曟煕閺冩挾鐣辨い顏勫暣婵″爼宕卞Δ鈧鎴︽⒑缁嬫鍎愰柟绋款煼婵＄敻宕熼锝嗘櫆闂佸憡娲﹂崑鍛枔缂佹绡€缁剧増锚婢ф煡鏌涙惔銈呬汗缂侇喛顕ч埥澶娢熼柨瀣垫綌婵犵數鍋涘Λ娆掑綔闂佺顑嗛幐鎼侇敇婵傜骞㈡繛鍡樺灩濡插洭姊绘担鍦菇闁搞劏妫勯…鍥槼缂佸倹甯掗…銊╁醇閻斿搫寮抽梻浣告惈濞诧箓鏁嬫繝纰樷偓鐐藉仮闁硅棄鐖奸幃娆撴倻濡厧甯鹃梻濠庡亜濞层垽宕曞畷鍥ь棜閻犳亽鍔庣壕濂告煃闁款垰浜鹃梺绋款儐閹瑰洤顫忕紒妯诲闁革富鍘介懣鍥⒑閹肩偛濮€婵☆偄瀚伴垾锕傚炊椤忓秵些闂備胶鍎靛Σ鍛村储閻ｅ本宕叉繝闈涱儐閸嬨劑姊婚崼鐔衡棩缂侇喖鐖煎鍝劽虹拋宕囪兒闂佸憡顨呴崯鍧楁偩閻戣棄绠涙い鎴旀櫆濡啫鐣峰鈧俊鎼佸Ψ瑜岄崰濠囨⒒閸屾瑧顦﹀鐟帮躬瀹曟垿宕ㄩ娑樺簥闂佸憡娲﹂崹鏉啃ч弻銉︾厱闁斥晛鍟伴埊鏇㈡煟閹惧瓨绀嬮柡宀嬬節瀹曞爼濡烽妷銉€冲┑鐘媰閸屾艾绁梺鍝勬湰缁嬫垼鐏掗梺鍛婄箓鐎氼垶宕洪悙鐑樷拺閻庡湱濯鎰版煕閵娿儲鍋ユ鐐插暙閳诲酣骞樺畷鍥崜闂備胶鎳撻顓熸叏閸愮潿搴敋閳ь剟寮婚敐鍡樺劅闁靛牆瀛╃紞鍫濃攽閻愰鍤嬬紒鐘虫尭閻ｅ嘲顭ㄩ崘鐐兊闁哄鐗冮弲婵嬫倿閸ф鍊垫鐐茬仢閸旀碍銇勯敂璇茬仯缂侇喛顕ч埥澶愬閿涘嫬骞楁繝寰锋澘鈧劙宕戦幘缁樼厽婵°倐鍋撶紒缁樏悾?
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婇悞锕傚磿閹剧粯鈷戦柟鑲╁仜婵″ジ鏌涙繝鍌涘仴鐎殿喛顕ч埥澶愬閳哄倹娅囬梻浣瑰缁诲倸螞濞戔懞鍥Ψ瑜忕壕钘壝归敐鍛儓鐏忓繘姊洪崨濠庢畷濠电偛锕ら锝囨嫚濞村顫嶅┑鈽嗗灦閺€閬嶅棘閳ь剟姊绘担鍛婂暈婵炶绠撳畷鎴﹀礋椤掍礁寮块梺闈涚箞閸婃牠鍩涢幋鐐电闁煎ジ顤傞崵娆愵殽閻愭惌娈滈柡宀€鍠栭幃鐑芥偋閸喐鍊锋俊鐐€ら崑鍕崲閹寸姵宕叉繝闈涙－濞尖晜銇勯幒鎴濐仼闁轰礁缍婂缁樻媴閾忕懓绗￠梺鍝勮閸旀垵鐣烽弻銉ヨ摕闁靛／鍐偊闂傚鍋勫ú锔剧矙閹烘鐓曢柟杈鹃檮閻撴瑧绱掔€ｎ偄顕滈柟鐧哥悼缁辨帡鎮╅搹顐㈤瀺缂備胶绮粙鎾寸閿曞倸纾兼慨姗嗗墻濞煎酣姊绘担绛嬪殐闁搞劋鍗抽幊鐔碱敍閻愯尙鍘洪柟鍏肩暘閸斿瞼绮绘繝姘€甸柣銏☆問閻掔偓顨ラ悙鑼ⅵ婵﹦绮幏鍛存惞閻熸壆顐奸梻浣告惈鐞氼偊宕濋幋婵堟殾闁哄被鍎遍拑鐔兼煏婢跺牆鍔滈弶鍫濈墦濮婅櫣绱掑Ο鍝勵潕闂佺绨洪崐婵嗙暦閹达箑惟闁挎棁妗ㄧ花濠氭⒑閸濆嫮袪闁告柨閰ｉ幆灞剧節閸ャ劎鍘搁柣搴秵閸嬪懘鍩涢幒鏃傜＜閺夊牄鍔屽ù顔姐亜閵忥紕鎳冮柍璇查叄楠炲棜顦虫い鏂垮濮婄粯鎷呴崫銉︾€┑鈽嗗亜鐎氭媽妫熼梺鍓茬厛閸嬪懘宕ｈ箛娑欑厓鐟滄粓宕滈悢鐓庤摕闁挎繂鎲橀悢璁胯櫣鎷犻崣澶嬬彵闂傚倷绀侀幗婊堝窗鎼搭煈鏁勫璺侯煬濞兼牜绱撴担鑲℃垶鍒婄€靛摜纾兼繛鎴烇供閸庢劗绱撳鍛ⅵ婵﹨娅ｇ划娆撳箰鎼淬垹闂梻浣告啞椤棝宕卞▎搴ｇ秿闂傚倸鍊风粈渚€骞夐垾瓒佹椽鏁冮崒姘辩崶闂佽鍎抽顓€?Windows 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃闁硅櫕鎹囬垾鏃堝礃椤忎礁浜鹃柨婵嗙凹缁ㄧ粯銇勯幒瀣仾闁靛洤瀚伴獮鍥敂閸℃瑧鍘梻浣告惈鐞氼偊宕濋幋锕€绠栭柕蹇嬪€曟导鐘绘煕閺囩喎鐏熼柛銊ㄥ吹濡叉劙骞掑Δ鈧粻娑欍亜閹炬瀚弳锔剧磽閸屾瑨鍏岀紒顕呭灦閹嫰顢涢悙闈涚ウ闁诲函缍嗛崰妤呭磹閻戣姤鐓曟い顓熷灥娴滄粍绻涢崨顕呮疁婵﹦绮幏鍛村川婵犲倹娈橀梻浣藉吹閸犲棝宕曢幎钘夌闁靛繒濯銊╂煃瑜滈崜娆擃敋閿濆棛绡€婵﹩鍓欏畵鍡涙⒑闂堟稓绠冲┑鐐╁亾闂佸摜鍋犲▔娑㈠煘閹达附鍋愮紓浣股戦柨顓炩攽閻愯尙婀撮柛濠冪箞楠炲啴鎮界粙璺ㄧ潉闂佸壊鍋嗛崯鍧楀箯?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋垺鍋橀柍銉ュ帠婢规洖鈹戦悙鏉戠仸闁荤噥鍨堕幃鐐烘偩瀹€鈧壕濂告煟濡搫鑸圭€规挸妫濋弻锛勪沪鐠囨祴鍋撳┑瀣畺鐟滄柨鐣烽悡搴樻斀闁归偊鍓氶悿鍌炴⒒閸屾瑧绐旀繛浣冲厾娲Χ閸ワ絽浜炬慨妯哄船椤ｅジ鏌熸笟鍨闁糕斁鍋撳銈嗗笒鐎氼參鎮¤箛娑氬彄闁搞儜灞藉壈闂佽娴氭禍婵嬪Φ閸曨剚鍎熼梽鍥春濡ゅ懏鈷掗柛灞捐壘閳ь剚鎮傚畷鎰板箹娴ｅ摜锛欓梺褰掓？缁€浣哄瑜版帗鐓欓梻鍌氼嚟椤︼妇鐥崜褎鍤€妞ゎ亜鍟伴埀顒婄秵娴滄粓顢楅姀掳浜滈柟閭﹀灠琚氶梺閫涚┒閸斿矂锝炲鍫濆耿婵°倐鍋撻柍閿嬪浮濮婅櫣绱掑Ο铏诡儌闂佸搫鎷嬮崑濠囩嵁婢跺瞼鐭欐繛鍡樺劤閹垿姊洪崜鎻掍航婵犫懇鍋撶紓鍌氬暞濞茬喎顫忔繝姘＜婵炲棙鍩堝Σ顕€姊洪崷顓涙嫛闁稿锕ら悾鐑筋敍閻戝棙鏅濋梺鎸庢濡嫭绂嶆潏銊х瘈闁汇垽娼у瓭濠电偛鐪伴崐婵嗙暦閵夈劊浜归柟鐑樻尵閸橀亶姊洪崫鍕偍闁告柨鐭傞幃姗€鏁撻悩宕囧幗闂佽鍎抽悺銊х矆鐎ｎ喗鐓涚€光偓閳ь剟宕伴弽顓犲祦闁糕剝鍑瑰Σ鑽ょ磽娴ｅ搫鐝￠柛娑卞灣閿涙粍绻涙潏鍓хК妞ゎ偄顦卞▎銏ゅ蓟閵夛妇鍘撻柣鐔哥懃鐎氼剟鎮樼€涙ǜ浜滈柕蹇ョ磿閹冲洭鏌熺粙鍖℃敾鐎垫澘瀚板畷鍗炍熼梻瀵告晼闂傚倸鍊搁崐宄懊归崶顒夋晪闁哄稁鍘肩粣妤呮煛瀹ュ骸浜炵紒鈧繝鍌楁斀闁绘ê寮跺婵堚偓瑙勬尫缁€渚€濡撮幒鎴僵闁挎繂鎳嶆竟鏇熶繆閵堝洤啸闁稿鍋熼弫顕€鏁撻悩闈涚ウ闁诲函缍嗘禍鏍绩娴犲鐓欓梺顓ㄧ畱閺嬫捇鏌涚€ｎ偅灏柣锝忕節楠炲秹鎼归锝嗘緫闂傚倷鑳剁划顖炲礉閺嵮岀劷婵炲棙鎸婚崐鍫曟煥閺囩偛鈧綊鎮″▎鎴犵＜閻庯綆鍋勯婊堟煙閹绘帗鎲哥紒杈ㄥ浮椤㈡瑩鎳為妷顔解挅闂備線娼уú锕傚礉濞嗘挾宓侀柟鐑橆殔缁狅綁鏌ㄥ┑鍡橆棡闁哥喐妞藉铏规嫚閺屻儺鈧绱掗悩鑼х€规洘娲熼弻鍡楊吋閸℃ぞ缃曟繝鐢靛Т閿曘倝宕板鑸靛€甸柛顐ｆ礃閻撳繐鈹戦悙闈涗壕婵炲懎妫濋弻宥夋煥鐎ｎ亞浼岄梺鍝勬湰缁嬫垿鍩ユ径濠庢建闁割偆鍠愰弳浼存⒑鐠囪尙绠扮紒缁樺浮瀵偅绻濆顒冩憰濠电偞鍨崹娲磻閹邦喒鍋撶憴鍕婵炲眰鍊濋崺娑樼暆閸曨兘鎷虹紓鍌欑劍鐪夌紒鍫曚憾閺屾盯鏁愯箛鏇炲煂闂佷紮绲介崲鏌ワ綖濠婂牆鐒垫い鎺戝閳ь兛绀佽灒婵炲棛鍋撴瓏闂傚倷绀侀悿鍥敋瑜旈幆宀勫磼閻愯尪鎽曢梺鎸庣箓濡瑩宕曢悢鍏肩厪闁割偅绻傞銏㈢磽瀹ュ懐澧曢柍瑙勫灴閹瑩骞撻幒鏃堢崜缂傚倷绶￠崰妤呭箰閹间焦鍋╅柣鎴ｆ椤懘鏌ㄥ┑鍡楊劉闁宠鐭傚缁樻媴閾忓箍鈧﹪鏌￠崨顔剧疄鐎规洘鍨挎俊鑸靛緞鐏炵晫銈﹂梺璇插嚱缂嶅棝宕板Δ鍛柧婵犻潧顑嗛悡鍐喐濠婂牆绀堥柣鏃傚帶閽冪喖鎮楅敐搴℃灈闁绘劕锕弻鏇熺箾瑜嶉幊鎰ｉ崶顒佲拻闁稿本鑹鹃埀顒佹倐閹勭節閸曨厾鐓撻梺瑙勫劤閻°劌鈻嶉悩缁樼厸闁搞儮鏅涘▍蹇旂箾閸粎鐭欓柡宀嬬秮楠炲洭濡搁敂鐣屽絽濠电偛鐡ㄧ划鎾剁不閺嵮屾綎濠电姵鑹剧壕鍏兼叏濮楀棗鍘撮柛瀣崌楠炴牗鎷呴崫銉串闁诲海鎳撶€氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐烘偡濠婂啰绠荤€殿喗濞婇弫鍐磼濞戞艾骞楅梻渚€娼х换鍫ュ春閸曨垱鍊块柛鎾楀懐锛滈梺褰掑亰閸欏骸鈻撳鍫熺厸鐎光偓閳ь剟宕伴弽顓犲祦鐎广儱顦介弫濠勭棯閹峰矂鍝烘慨锝咁樀濮婄粯鎷呮笟顖滃姼濡炪倖鍨堕崹褰掑箲閵忕姭鏀介悗锝庝海閹芥洟姊洪崫鍕偍闁搞劌缍婇弻瀣炊閵娧呯槇闂傚倸鐗婄粙鎺楀箹閹邦厹浜滈柨鏃囶潐濞呭懏銇勯鍕殻濠碘€崇埣瀹曞崬螖閳ь剟锝為崼銏㈢＝濞撴艾娲ら弸鐔兼煟閻斿弶娅呮い鏇稻缁绘繂顫濋鈧畵鍡涙⒑闂堟稓绠冲┑鐐╁亾闂佸摜鍋犲▔娑㈠煘閹达附鍋愮紓浣股戦柨顓炩攽閻愯尙婀撮柛濠冪箚濡喖姊虹€圭姵銆冪紒鍨涒偓鐐解偓鎺撶節濮橆厾鍘梺鍓插亝缁诲牓顢撳Δ鈧湁婵犲﹤瀚粻鏍磼缂佹娲寸€规洩绲惧鍕槈濠ф儳浜鹃柟绋垮瘨濞堜粙鏌ｉ幇顖氱毢濠⒀嶇畱閳规垿鍩勯崘鈺佲偓鎰攽閿熺姵鏁遍柟顖涙閺佹劙宕ㄩ鍙ラ偗濠德板€楁慨鐑藉磻閻愬搫绀夐柡宥庡幖缁犳岸鏌￠崘銊у闁哄懏鐓￠弻娑㈠箛閸撲礁鍩屾繛瀛樼矒缁犳牠寮婚悢鍏煎亱闁割偆鍠撻崙锛勭磽?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇牠姊绘笟鈧埀顒傚仜閼活垱鏅堕弶娆剧唵閻熸瑥瀚粈鍐磼缂佹绠撴い顐ｇ箞椤㈡﹢鎮╅幓鎺旂杽闂傚倸鍊烽悞锕傚几婵傜鐤炬繛鎴欏灩閻ゎ喖霉閸忓吋缍戦柣鎾偓鎰佺唵闁兼悂娼ф慨鍫ユ煟閹惧瓨绀嬮柟顔斤耿閹瑦锛愬┑鍡橆唲濠电姵顔栭崰鏍磹閸ф钃熼柨娑樺濞岊亪鎮归崶銊с偞闁稿鎹囧鎾閿涘嫮鏆㈤梻鍌欐祰椤曆囧礄閻ｅ瞼绀婇柛鈩冪☉绾惧鏌涘☉妯兼憼闁稿鍠栭弻锟犲炊閳轰礁澹夊┑鐐存尭椤兘寮婚弴銏犻唶婵犻潧顑愰埀顒侇殔闇夋繝濠傚缁犳牜绱掔紒妯兼创闁瑰磭濞€椤㈡鍩€椤掑嫬鍑犳繛鎴炲焹閸嬫挸鈻撻崹顔界亪濡炪値鍘鹃崗姗€鐛崘顔碱潊闁靛牆鎳庣粣娑欑節閻㈤潧孝閻庢凹鍠涢崐鎾⒒閸屾艾鈧绮堟笟鈧獮澶愭晸閻樿尙顔囬梺绯曞墲缁嬫垵顔忓┑鍥ヤ簻闁哄啫鍊婚幗鍌炴煕閻旈攱鍤囨慨濠傤煼瀹曟帒鈻庨幇顔哄仒闁诲氦顫夊ú婊堝极鐠囪尙鏆﹀ù鍏兼綑缁犳盯鏌ｅΔ鈧悧鍡樼椤撱垺鐓涘璺猴功婢ф劙鏌￠崨顔剧畺缂佸顦鍏煎緞鐎ｎ剙骞嶉梻浣侯攰椤宕濋弴銏犺Е闁革富鍘搁崑鎾舵喆閸曨剙鐭紓浣藉煐瀹€绋款嚕婵犳艾鐏抽柟棰佺閹垿鏌熼懝鐗堝涧缂佽尪鍋愰崚鎺楀箻缂佹鍘甸梺绋跨箰閸氬宕曞Δ鍛厽闁规儳纾晶锔锯偓娈垮櫘閸嬪﹤鐣烽崡鐐╂婵炲棙鍨电敮妤呮煟閻斿摜鐭嬬紒顔芥尭閻ｅ嘲顭ㄩ崼鐔蜂簻闂佺粯鎸稿ù鐑芥偩閸濆嫧鏀芥い鏂款潟娴犳粓鏌涚€ｎ偅宕岄柡灞剧洴閸╋紕鈧綆鍓涢惁鍫ユ倵鐟欏嫭纾搁柛鏃€鍨块妴浣割潨閳ь剟骞冮埡鍛瀭妞ゆ劧绲垮Σ銉╂⒒閸屾艾鈧悂宕愰悜鑺ュ殑闁告挷绀侀崹婵囥亜閺嶎偄浠滈柦鍐枛閺屾洘绻涢悙顒佺彅闁荤喐鐟辩粻鎾诲箖濡ゅ懏鏅查幖绮瑰墲閻忓牓姊虹紒妯诲鞍婵炶尙鍠栧濠氭偄閸涘﹦绉舵俊銈忕到閸燁垶顢撳澶嬧拺缂備焦蓱鐏忕増绻涢崣澶樼劷闁瑰箍鍨归埞鎴犫偓锝庡亽濡啫鈹戦悙鏉戠仸闁荤噥鍨拌灋闁绘劕妯婂〒濠氭煏閸繃鍣虹紒鍌氼儔閺屾稓鈧綆浜滈顓㈡煃閵夘垳鐣甸柟铏殜椤㈡盯鏁愰崱蹇旂€稿┑鐘垫暩閸嬫盯鎮洪妸褍鍨濈€光偓閸曨剙浜梺缁樻尭鐎垫帡宕甸弴銏″仯闁搞儯鍔庨妶瀛樹繆閹绘帞澧﹂柡灞炬礉缁犳盯寮撮悙鎰╁劜閵囧嫰寮埀顒傛暜閿熺姴钃熼柨婵嗘閸庣喖鏌曟繝蹇涙闂佹鍙冨铏规兜閸滀礁娈濈紓浣虹帛缁诲倿鎮炬搴樺亾閸︻厼顎屾俊顖氬閳?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇牠姊绘笟鈧埀顒傚仜閼活垱鏅堕弶娆剧唵閻熸瑥瀚粈瀣煙椤旀儳鍘存鐐诧攻缁绘繈宕掑鍛呫劑姊虹拠鈥虫灀闁哄懏鐩、娆掔疀濞戣鲸鏅╅梺缁樻尭妤犳瓕鐏囩紓鍌氬€搁崐鎼佸磹閹间礁纾归柛婵勫劤閻捇鏌ｉ姀鐘冲暈闁稿顑夐弻锟犲炊閿濆棗娅氶梺閫炲苯澧剧紒鐘虫尭閻ｉ绮欑拠鐐⒐閹峰懘宕ㄦ繝鍕緭闂傚倸鍊搁崐鎼佸磹閹间焦鍋嬪┑鐘插閻瑩鏌熸潏鎹愵吅缂傚秵鐗犻弻娑㈠Ψ椤旂厧顫梺鎶芥敱閸ㄥ灝顫忓ú顏嶆晝闁靛牆鎳嶇划鍫曟⒑閸忓吋銇熼柛銊ㄦ硾閻ｇ兘濮€閵堝懍绱堕梺闈涳紡閸滃啰搴婇梻鍌欑窔閳ь剛鍋涢懟顖涙櫠椤栨粎纾兼俊銈呭暙閺嬫盯鏌涢埞鎯т壕婵＄偑鍊栫敮鎺斺偓姘ュ姂閸┾偓妞ゆ垼娉曢ˇ锔姐亜椤愶絿鐭掗柛鈹惧亾濡炪倖宸婚崑鎾绘煟閿濆懎妲婚摶鏍煕濞戝崬鏋涘ù婊勵殜閺岀喖鎳濋悧鍫濇锭缂備焦褰冨锟犲箖娴兼惌鏁婇悘蹇旂墬閺傗偓闂備胶绮敋闁诲繑宀稿鎶藉煛娴ｅ弶鏂€濡炪倖姊婚埛鍫ュ吹閻斿吋鐓冪憸婊堝礈濮橆剦娼╅柕濞炬櫅缁€鍌涗繆椤栨繃顏犻柡鍡檮閵囧嫰骞掗幋婵愪痪濡炪們鍎遍悧濠勬崲濞戙垹绠ｉ柣鎰皺閻熸煡姊洪崫鍕偓鐢稿磻閹剧粯鈷掗柛灞剧懆閸忓瞼绱掗鍛仸闁轰礁鍟撮崺锟犲川椤撶姷宕堕梻浣告惈缁嬩線宕㈡禒瀣９闁割偅娲栫痪褔鏌涢锝団槈濠德ゆ硶缁辨帡鎮╅崘鎻掑Б濡炪値鍙€濞夋洟骞戦崟顒傜懝妞ゆ牗鑹炬竟瀣節閻㈤潧浠滅€殿喖鐖奸獮鎰板箮閽樺鎽曢梺鏂ユ櫅閸燁垱鍒婄€靛摜纾奸悗锝庡亜椤曟粍绻涢崼銏㈠笡缂佺粯绻勯崰濠冨緞瀹€鈧敍鐔兼⒑閸濆嫯瀚版い锔跨祷濡喖姊洪崷顓℃闁哥姵顨婇幃锟犳偄閹肩偘绨婚梺瑙勫礃濞夋稒绂掗敂濮愪簻闁靛绲介悘鏌ユ煛瀹€瀣？濞寸媴绠撻幃鍓т沪濠靛嫬鍔滈柕鍥у椤﹁京绱掗懜闈涘摵鐎殿喛顕ч埥澶愬閻樻鍟嬮梺鑽ゅЬ濞咃綁宕曢妶澶嬪€甸悹鍥ф▕濞撳鏌曢崼婵嬵€楀ù婊勭矌缁辨帞绱掑Ο鍝勵潕闂佸憡鐟ラ幊搴ㄢ€旈崘顔嘉ч柛娑卞灣椤斿洭鏌ｆ惔銏犲毈濞存粎鍋涘嵄闁圭増婢橀柋鍥煛閸モ晛鏆遍柟閿嬫そ濮婃椽宕ㄦ繝鍕ㄦ闂佹寧娲忛崕宕囧垝閸儱鐒垫い鎺戝閳锋垿鎮归崶顏勭毢缂佺姵褰冭彁闁搞儯鍔嶉妵婵堚偓瑙勬礃閸旀瑩鐛鈧、娆戞喆閿濆棗顏洪梻鍌欒兌椤牓寮甸鍕仭闁靛ň鏅涚粈鍌炴倶閻愭澘瀚庡ù婊勭矋閵囧嫰骞樼捄杞版埛婵犫拃浣哥伈闁哄苯绉烽¨渚€鏌涢幘瀵告噰闁诡喗锚椤繃娼忛妸锔筋唶闂備胶鍋ㄩ崕杈╁椤撱垹姹查柨鏃傚亾閸犳劙鏌ｅΔ鈧悧鍡欑箔閹烘鈷戦柛妤冨仦閸犳﹢鏌″畝瀣瘈鐎规洖鐖奸崺鈩冩媴闁垮濯板┑鐘殿暯閳ь剝娅曢埢鏇㈡煕閹捐泛鏋庢い?
//
// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婇悞锕傚磿閹剧粯鈷戦柟鑲╁仜婵″ジ鏌涙繝鍌涘仴鐎殿喛顕ч埥澶愬閳哄倹娅囬梻浣瑰缁诲倸螞濞戔懞鍥Ψ瑜忕壕钘壝归敐鍛儓閺嶏繝鎮峰鍕凡闁哥喐娼欓敃銏＄瑹閳ь剙顫忓ú顏勭閹艰揪绲块悾鐢告⒑閻熸澘鏆遍梺甯秬濡垽妫呴銏″婵﹨宕电划缁樸偅閸愨晛鈧灚绻涢幋鐐垫噽闁绘帟妫勯…鑳槾闁轰礁顭峰濠氭晬閸曨亝鍕冮梺缁樺姦閸撴盯藝閵娧呯＝濞达綀娅ｇ敮娑氱磼鐎ｎ偆澧甸挊婵嬫煛鐏炶鍔滈柣鎾存礋閺岋綁寮村槌栨М婵炲瓨绮堥崡鎶藉蓟閻旂⒈鏁婄紓鍌氱摠閸ㄥ綊骞戦姀鐘斀閻庯綆浜為崐鐐烘⒑闂堟侗鐒鹃柛銊ㄦ閳ь剚鑹鹃妶绋款潖缂佹ɑ濯撮柛娑橈工閺嗗牓姊洪悷鐗堝暈婵″弶甯℃俊鐢稿箛閺夎法顦ㄩ梺鍐叉惈閸犳艾煤閹间焦鈷戠紓浣姑慨澶愭煕鎼存稑鈧繈骞冮敓鐘插嵆闁靛骏绱曢崢钘夆攽閳藉棗鐏￠悗绗涘洦鍋熸い蹇撴閸嬫挾鎲撮崟顒傤槬闂佹椿鍘奸崐鍨嚕鐠囨祴妲堥柕蹇曞Х閸樻挳姊洪崜鑼帥闁哥姵纰嶇€靛ジ宕奸妷锕€鈧灚绻涢崼婵堜虎闁哄鍠栭弻鐔奉潰鐏炲墽褰ч梺閫涚┒閸旀垿宕洪埀顒併亜閹烘垵顏柣鎾卞劜缁绘盯骞嬮悘娲讳邯椤㈡棃鍩￠崒銈嗩啍闂佺粯鍔樼亸娆愭櫠濞戞〒搴ㄥ炊瑜濋煬顒勬煙椤旂晫鎳囨い銏℃瀹曠喖濡搁妷銈咁棜闂備礁鎼粙渚€宕㈤懖鈺冪焼濠㈣泛顑勭换鍡涙煏閸繂顏柛鏂跨Ч閺屾稑鈻庤箛鏇狀啋闂佸搫鐬奸崰鎾舵閹烘顫呴柣妯虹－娴滎亞绱撻崒娆掝唹闁稿鎸搁…璺ㄦ崉娓氼垰鍓崇紓浣叉閸嬫挸鈹戦悩鍨毄濠殿喗鎸冲畷鎰板箹娴ｅ搫绁﹀┑鈽嗗灠閸氬宕伴幇鐗堢厽婵°倐鍋撻柣妤€妫涚划顓烆潩閼哥數鍘遍柣搴秵閸嬪嫭鎱ㄦ径宀€纾奸弶鍫涘妼缁楁帡鏌嶉挊澶樻Ц妞ゎ偅绻堝畷妤呭礂閸忓吋顔夐梻鍌氬€风欢姘跺焵椤掑倸浠滈柤娲诲灡閺呭爼顢欓悾宀€顔曠紒缁㈠弮椤ユ挾寮ч埀顒勬倵濞堝灝鏋涙い顓犲厴楠炲啴濮€閵忕姵鐎抽柡澶婄墑閸斿秴鈻嶉幘缁樷拻濞达絿鐡旈崵鍐煕閵娿儵鍙勭€规洘锕㈡俊姝岊檨闁告艾鎳樺缁樻媴缁涘娈柣搴㈢▓閺呯娀銆佸▎鎾冲唨妞ゆ挾鍋熼悰銉モ攽鎺抽崐鎰板磻閹炬番浜滄い鎾跺仦閹兼劙鏌嶇拠鏌ュ弰妤犵偞鐟╁畷鐔碱敇閻橆偅锛堟繝鐢靛Х閺佸憡绻涢埀顒佺箾娴ｅ啿鍘捐ぐ鎺戠＜婵鍋愰崝宄扳攽閻愬弶顥為柟绋款煼閹€斥槈濮橈絽浜炬鐐茬仢閸旀岸鏌涢悤浣镐簼缂佹梻鍠愬鍕偓锝庡墰閻﹀牊绻濋悽闈浶㈤柛濠勭帛閺呰泛鈽夐姀锛勫帗闂備礁鐏濋鍛箔閹烘顥嗗鑸靛姈閻撱儲绻濋棃娑欙紞婵☆垰鐗婃穱濠囶敃閿濆懐浠撮梺瀹犳椤︾敻鐛鈧幃鐑藉箥椤旂瓔鍤勭紓鍌氬€峰ù鍥ㄣ仈閸濄儲鏆滈柍銉ョ－閺嗭箓鏌ㄥ┑鍡橆棞缂佸墎鍋涢埞鎴︽偐閺屻儺鈧绻涢崼鐔虹畼缂佽鲸鎹囧畷鎺戔枎閹烘垵甯紓鍌欑贰閸ｎ噣宕归崼鏇炵畾闁告洦鍨奸弫宥夋煟閹邦剙绾ф俊宸墴濮婃椽鏌呴悙鑼跺濠⒀屽灦閺屾洟宕惰椤忣厽顨ラ悙鏉戞诞妤犵偛顑呴埞鎴﹀箛椤忓棙锛忛梻鍌氬€烽懗鍓佸垝椤栫偛绠伴柤濮愬€楅惌娆撴煙鏉堥箖妾紒鐘冲浮濮婄粯鎷呴悷閭﹀殝缂備浇顕ч崐鍧楃嵁婵犲懐鐤€婵炴垶顭囬悿鍛存⒑閸涘﹦缂氶柛搴櫍瀹曟垿骞橀弬銉︻潔濠电偛妫欓崹褰掓偩闂堟侗娓婚柕鍫濈凹缁ㄥ鏌涢悢椋庢憼濞ｅ洤锕幊婊堝垂椤愩垹濡抽梻浣哄仺閸庡浜稿▎鎾崇濠㈣埖鍔栭埛鎺懨归敐鍫綈闁稿濞€閺屾盯寮▎鎯у壋闂佸憡甯楃敮鎺楀煝鎼淬劌绠婚柡澶嬪灣缁勪繆閻愵亜鈧牕煤瀹ュ纾婚柟鍓х帛閻撴洟鏌嶇憴鍕姢濞存粎鍋撴穱濠囨倷椤忓嫧鍋撻弽褜鍟呭┑鐘宠壘绾惧鏌熼崜褏甯涢柣鎾冲暣閹嘲鈻庤箛鎿冧患闂佽楠忔俊鍥焵椤掆偓閻忔艾顭垮Ο灏栧亾濮樼厧娅嶉柛鈹惧亾濡炪倖甯掗敃锔剧矓閻㈠憡鐓曢悗锝庡亜缁楁帗銇?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柣妯荤垹閸ャ劎鍘遍柣蹇曞仜婢т粙鎮￠姘肩唵閻熸瑥瀚粈鈧梺瀹狀潐閸ㄥ潡銆佸▎鎾村剹妞ゆ劦鍋傜花濠氭煟閿濆洤鍘存い銏☆殜瀹曠喖顢曢姀鐘辨喚闂備浇顕уù鐑藉极閸濄儲鍏滈柛顐ｆ礀绾惧鏌熼幑鎰厫闁哥姴妫濋弻娑㈠即閵娿儱顫銈忚礋閸庡磭妲愰幘璇茬＜婵ɑ鐦烽姀銈嗙叆闁哄洢鍔嬬花缁樸亜閺囶亞绋荤紒缁樼箓椤繈顢楅埀顒勬嚀閸喒鏀介幒鎶藉磹閺囥垺鏅濋柕鍫濐槸缁犳牠鏌熸潏楣冩闁抽攱鍨块弻娑樷槈濮楀牊顣肩紓浣哥埣娴滃爼寮诲☉銏″亹鐎规洖娲㈤埀顒佸笚缁绘盯宕遍幇顒備紙婵犳鍠掗崑鎾绘⒑闂堟稓澧曟俊顐㈢焸楠炲繒绱掑Ο鑲╊啎闁哄鐗嗘晶浠嬪箖婵傚憡鐓欓柧蹇ｅ亝瀹曞本銇勯姀陇澹樻い顓滃姂瀹曘劑顢涘鎰簥闂備浇宕垫慨鏉懨洪敃浣典汗闁告劦鍠栭悞鍨亜閹烘垵鏆欓柛鏂跨仛娣囧﹪顢曢妶鍛€诲銈庝簻閸熷瓨淇婇崼鏇炲耿婵炲棗娴氬宄扳攽閻橆喖鐏辨い顐㈩槸鐓ら柣鏃堫棑閺嗭箓鏌熸潏鍓х暠缂佺姴顭烽弻鐔革紣娴ｅ搫濡介梺绋跨Ч濞佳団€旈崘顔嘉ч柛鈩冾殘閻熴劌顪冮妶蹇擃洭濠⒀勵殜瀵煡濡疯绾句粙鏌涚仦鎹愬闁逞屽墰閸忔﹢骞婂Δ鍛濞达絿顭堥悘濠傤渻閵堝棛澧紒顔奸叄閹苯鈻庨幘瀵稿幍闁哄鐗婇崕鎶芥儊濠婂牊鐓涘ù锝夋交闊剟鏌ｉ幙鍐ㄤ喊鐎规洖鐖兼俊鎼佸Χ閸モ晝鐣抽梻鍌欐祰濡椼劎绮堟笟鈧弫鍐敂閸繆鎽曢梺闈涚墕濡瑩宕ｈ箛娑欑厵闂侇叏绠戦崢鎾煕鐎ｎ偅宕岀€殿喗鎸虫慨鈧柣妯哄级閸庮亝淇婇悙顏勨偓鏍暜閹烘柡鍋撳鐓庡籍闁糕晜鐩獮鎺楀箠閵娿儳绉洪柡浣瑰姈瀵板嫬螣濞茬粯顥涙繝鐢靛仦閸ㄥ爼骞戞担杞扮剨婵炲棙鍨堕～鏇㈡煙閻戞﹩娈曢柛瀣姍閺屾盯骞囬姘辨婵炲鍘ч惌鍌氼潖閾忚瀚氶柍銉ㄦ珪濞堫參姊洪幖鐐插妧闁告粈绀侀弲顏堟⒒閸屾瑧顦︾紓宥咃躬閵嗗倿鏁冮崒娑樷偓鍧楁煥閺傚灝鈷旂紒鈧繝鍥х缂侇喛顫夐鍡涙煛閳ь剚绂掔€ｎ偆鍘介梺褰掑亰閸撴瑧鐥閺屽秶绱掑Ο鑽ゎ槬闂傚洤顦扮换婵囩節閸屾碍娈堕梺绯曟櫔缁插潡鍩€椤掍緡鍟忛柛锝庡櫍瀹曟垶绻濋崒婊勬闂佸憡顨堥崕鎰€掓繝姘厸闁搞儮鏅涙禒褏绱掓径瀣仢闁哄备鍓濋幏鍛存偡閺夋娼氶柣搴ゎ潐濞叉牠濡堕幖浣碘偓渚€寮撮姀鈩冩珳闂佹悶鍎弫鈺呭Ψ閳哄倵鎷婚梺绋挎湰閻熴劑宕楃仦淇变簻妞ゆ挾鍋熸晶銏ゆ煃瑜滈崜姘辩矙閸儱鐤鹃柣妯垮吹瀹撲線鏌熼悧鍫熺凡缂佲偓閸垺鍠愰幖鎼厜缂嶆牠鏌￠崶銉ョ仾闁绘挻鐟╅弻鐔碱敍閸℃鍣洪柟鎻掑悑缁绘繂鈻撻崹顔句画濠电偛鍚嬮悷鈺侊耿娓氣偓濮婅櫣绱掑Ο蹇ｄ簻铻ｅ┑鐘叉搐绾惧潡鐓崶銊р姇闁绘挻鐟╅弻锝夋晲閸涱喗鎷遍梺鍝勬閺呮繄妲愰幒鎾寸秶闁靛绠戦棄宥夋⒑閻熸澘妲婚柟铏姉閸掓帡鎮界喊妯轰壕闁挎繂绨肩花鍏笺亜閺傚灝顏慨濠呮閹风娀骞撻幒婵嗗Ψ闂備焦妞块崣搴ㄥ闯閿濆懐鏆﹂柨鐔哄Т缁€鍐煠绾板崬澧繛鍫熷劤閳规垿鎮欓崣澶嗘灆婵炲瓨绮嶇划鎾诲箖閻愬搫鍨傛い鎰С缁ㄥ姊洪悷鐗堟儓缂佸鎳撻‖濠傜暋閹冲﹦鎳撻…銊╁礋椤撶姷鍘滈梻浣告惈閻ジ宕伴弽顓炵畺婵犲﹤鍚橀悢鍏兼優闂侇偅绋掑Ο濠囨⒒閸屾瑧绐旈柍褜鍓涢崑娑㈡嚐椤栨稒娅犳い鏂垮⒔绾剧晫鈧箍鍎遍幏鎴濐啅閵夆晜鐓熼柨婵嗘搐閸樻挳鏌熼鐐珪缂侇喗鐟ч幑鍕Ω閵夈儛鏇炩攽閿涘嫬浜奸柛濠冪墱閺侇噣骞掗弬鍝勪壕婵鍘у顕€鏌涢埡鍌滄创妤犵偛顑夐弫鍌炴偩鐏炶棄绠炲┑鐘垫暩婵炩偓婵炰匠鍛亾濮樼厧澧寸€规洘绻傞～婵嬵敇閻樻鍟庨梻浣告啞娓氭宕㈤崜褏涓嶅┑鐘崇閻撴盯鎮橀悙鎻掆挃婵炲弶娼欓埞鎴﹀焺閸愨晛鈧劙鏌℃担绋挎殻闁糕斁鍋撳銈嗗笒鐎氼剟鎮″┑瀣厸闁搞儮鏅涜ⅴ闂佺顑嗛幐鎼佸煝閹捐鍨傛い鏃傛櫕瑜邦垶姊绘担渚劸妞ゆ垵鎳橀弫鍐敂閸惊銉ッ归敐鍛棌婵炲皷鏅犻幃瑙勩偊閹稿寒浠╅梺鍛婃缁犳垿鈥旈崘顔嘉ч柛鈩冾殘閻熴劑鏌ｆ惔銏犲毈闁告挾鍠庨锝嗗閺夋嚦銊╂煃閸濆嫬鈧宕㈤幘缁樺仭婵犲﹤瀚惌鎺斺偓瑙勬礃缁矂锝炲┑鍫熷磯濞达絾娲╃粻鎾诲蓟濞戙垹鍗抽柕濞垮劤娴犫晝绱撴担鍝勑ｅ┑鐐诧躬瀵鎮㈢悰鈥充壕婵炴垶鐟悞钘夆攽閳ヨ櫕鍠橀柡灞剧〒閳ь剨缍嗛崑鍛暦瀹€鍕厵缂佹稑婀辩弧鈧繝纰樷偓宕囧煟鐎规洦浜濋幏鍛嫚閳╁喛绱栨繝鐢靛Х閺佸憡绻涢埀顒佺箾娴ｅ啿娲ょ壕褰掓煙闂傚顦︾紒鐘靛枛閺屻劑鎮㈤崫鍕戯綁鏌涚€ｎ亜鈧潡寮婚敐澶婃闁割煈鍠楅崐顖炴⒑缂佹ɑ灏伴柣鐔村劦濠€渚€姊虹紒妯诲碍婵炲鍏橀獮妤呮偐瀹曞洨顔曢梺鐟板暱閸㈡煡宕板鑸靛亜闁糕剝鐟﹂崰鎰節婵犲倻澧曠紒鎰殕娣囧﹪濡堕崨顔兼婵℃鎳樺娲川婵犲啫顦╅梺鎼炲妺閸楀啿鐣烽敓鐘茬闁兼亽鍎抽崢浠嬫⒑閸濆嫬鏆欐繛璇х畵椤㈡棃鎮╃紒妯煎幗闂佽宕樼亸娆撳礉瀹ュ洨纾肩紓浣诡焽濞叉挳鏌℃担鐟板鐎规洏鍔戦、娆撴嚍閵夈儮鍋撻悜鑺モ拻濞达絿鎳撻婊勭箾鐠囇囨缂佸倸绉归、鏃堝醇閵忕姷銈﹂梻浣告啞閸旀垿宕濇径鎰嚑閹兼番鍔嶉悡娆撴煙椤栧棗鎷戠槐鐐差渻閵堝棙鐓ラ柣顓炲€搁～蹇撁洪鍛画闂佺粯顨呴悧濠囧磿閹炬枼鏀介柍钘夋娴滄繃銇勯妸銉︻棤缂侇喖顑呴濂稿幢閹邦兛绨奸梻浣告啞閸斿繘寮插┑瀣偍闂侇剙绉甸崑鈩冪節婵犲倸鏆ｉ柛娆忓閺屾洟宕卞Δ鈧弳锝団偓瑙勬磸閸旀垿銆佸☉姗嗙叆闁逞屽墰缁厽寰勯幇顓涙嫼闂佸憡绻傜€氼噣鍩㈡径鎰厽闊洦鏌ㄦ慨宀勬煏閸ャ劌濮嶆鐐村浮楠炴﹢宕楅崨顔炬綎闂備浇顕уù鐑藉箠閹惧鐝跺┑鐘叉搐缁愭鏌″搴″箹缂佲偓閸緷褰掓偂鎼达絾鎲奸梺缁樻尰閻燂箓濡甸崟顖氬唨闁靛ě鍛帎闂備礁鎼鍡欑矙閹烘绠為柕濠忓缁♀偓闂佸憡鍔忛弬鍌涚閵忕姈鏃堟偐闂堟稐娌梺鍛婃⒐閻楃娀鐛崱娑樼睄闁割偅绻嶅濠傤渻閵堝骸浜介柛鐘虫皑缁岸宕稿Δ浣叉嫽闂佺鏈悷銊╁礂鐏炰勘浜滄い鎾跺仧婢э附銇勯姀锛勫⒌鐎规洖宕埥澶娢熺喊鍗炴暭闂傚倷鐒﹂幃鍫曞磿濞差亜绀堟繛鍡樻尰鐎氬懘鏌ｉ弬鍨倯闁绘挶鍎茬换婵嬫濞戞瑱绱炲┑鈩冨絻閹芥粎妲愰幒妤佸亹鐎瑰壊鍠氶崥瀣⒑鐠団€虫灍妞ゃ劌鎳橀崺銉﹀緞婵炵偓鐎诲┑鈽嗗灣閸樠勬叏閻戞ü绻嗛柣鎰典簻閳ь儸鍛亾闂堟稓鐒哥€规洩缍佸畷姗€鈥﹂幋鐐电▉婵犵數鍋涘Ο濠冪濠靛鍊垮┑鐘崇閻撶喖鏌熼柇锕€鐏犻柦鍕偢閹顫濋鐔哄嚒濡炪値鍙€閸庡篓娓氣偓閺屾盯鏁愯箛鏇犘滈悗娈垮枛椤兘寮幇鏉块唶闁靛繈鍨哄鎴︽⒒娴ｅ憡鎯堥柛鐕佸亰瀹曟垿宕卞☉妯昏緢闂佹寧绋戠€氼喚澹曢挊澹濆綊鏁愰崼顐ｇ秷闂佹娊鏀辩敮锟犲蓟濞戙垺鍋愰柧蹇ｅ亜绾炬娊姊虹化鏇熸澒闁稿鎸搁—鍐Χ閸℃鐟ㄩ梺鎸庢穿婵″洨鍒掗弮鍫熷仺闁告稑锕﹂崢閬嶆⒑缂佹◤顏嗗椤撶喐娅犻柛妤冨剱閻斿棝鎮峰▎蹇擃仾缂佲偓閸愵亞纾肩紓浣诡焽缁犳挻銇勯锝囩疄妞ゃ垺锕㈤幃鈺呭箵閹烘棏鍚嬮梻鍌氬€搁崐宄懊归崶顒夋晪闁哄稁鍘奸崹鍌炲箹濞ｎ剙濡肩€瑰憡绻冮妵鍕箳閹存繍浠兼繛瀵稿У閸旀瑥顫忔繝姘＜婵炲棙甯掗崢鈥愁渻閵堝骸骞栭柣妤佹崌閵嗕線寮介鐐茶€垮┑鐐村灦椤洭顢欐径鎰拺闁硅偐鍋涢崝鈧梺鍛婁緱閸犳顢欓弴銏＄厽闁绘柨鎽滈惌灞筋熆瑜庨〃鍫ュ极椤曗偓閸╋繝宕ㄩ鐔衡偓顒勬⒑閸涘﹤濮﹂柛鐘愁殜閹ょ疀濞戞艾褰勯梺鎼炲劀瀹ュ棙鏆版繝鐢靛仜閹冲骸螞濞嗘挸桅闁告洦鍨板敮闂佹寧妫佸Λ鍕閸愭祴鏀芥い鏃傘€嬮崝鐔虹磼椤曞懎鐏︽鐐茬箻瀹曘劑顢欓崜褎鍤屾俊鐐€栭悧妤冨垝鐏炵偓鍙忛柛蹇涙？缁诲棝鏌ｉ幇鍏哥盎闁逞屽墯閻楃姴鐣疯ぐ鎺擃棃婵炵娅曠粙鎴﹀煘閹寸姭鍋撻敐搴″缂佷緤绠撳娲捶椤撶儐鏆┑鐘灪椤洨鍒掓繝姘仭闁瑰啿锕ょ紞濠囧极閹版澘宸濇い鏃囨硶椤㈠懘姊绘担椋庝覆缂佽尙鍋撶换娑欑節閸屻倖缍庢繝鐢靛У閼瑰墽澹曢崗鑲╃闁糕剝锚缁楁帡鏌涘鍫㈢？缂佽鲸鎸荤粭鐔煎炊瑜庨悵顕€姊洪幖鐐插闁绘牜鍘ч锝夘敃閿曗偓缁€鍐┿亜閺冨洤浜归柛妯绘倐濮婃椽宕ㄦ繝浣虹箒闂佸憡锕㈢粻鏍箖娴兼潙唯闁冲搫鍊甸幏娲⒑閸涘﹦绠撻悗姘煎櫍閸┿垺寰勯幇顓犲幈濠碘槅鍨伴悘婵囩閸︻厾纾兼い鏃囧亹婢ф稓鈧灚婢樼€氼噣鍩€椤掑﹦绉甸柛鎾寸洴椤㈡瑩寮撮姀鈾€鎷虹紓渚囧灡濞叉牗鏅堕弻銉﹀珔闂侇剙绉甸崐鍫曠叓閸パ囶€楃€规挸妫濋弻?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁绘劦鍓欓崝銈囩磽瀹ュ拑韬€殿喖顭烽幃銏ゅ礂鐏忔牗瀚介梺璇查叄濞佳勭珶婵犲伣锝夘敋閳ь剙顫忛崫鍕懷囧炊瑜夐崑鎾诲Χ婢跺﹦鐛ュ┑掳鍊曢幊搴㈩攰闂備礁鎲″ú锕傚垂娴兼潙绀冮柍褜鍓欓—鍐Χ閸℃袝濠电姭鎳囬崑鎾剁磽娓氬洤鏋涢柣鏍帶椤繐煤椤忓嫮顔愰梺缁樺姈瑜板啴鈥栭崼銉︹拺缂佸灏呭銉︾箾鐏忔牑鍋撻幇浣告闁荤姴娲︾粊鏉懳ｉ崼銉︾厪闊洤锕ュ▍鍡涙煛閸涱喚绠炴慨濠冩そ楠炴劖鎯旈敐鍥╂殼婵犵妲呴崑鍛存儎椤栨繃顥ら梻渚€娼чˇ顐﹀疾濠婂牆鐓曢柟杈鹃檮閻撱垺淇婇娆掝劅婵″弶鎮傞弻锟犲幢濞嗗繑鐏堥梺鍝勬湰閻╊垶骞冮埡浣烘殾闁搞儜鈧幏浼存煟鎼淬埄鍟忛柛鐘崇洴椤㈡俺顦归柛鈹垮劜瀵板嫰骞囬澶嬬秱闂備胶绮摫闁绘鎹囧畷婵嬪箣閿曗偓閽冪喖鏌曟繛鍨姉闁衡偓娴犲鐓曢柍鈺佸幘椤忓嫧鏋嶉柨婵嗘媼濞撳鏌曢崼婵嬵€楀褜鍠楅妵鍕箣濠靛洤娈楅梺?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栨繝寰锋澘鈧呭緤娴犲鐤い鏍剱閺佷胶鈧箍鍎遍ˇ浼村煕閹寸姷纾奸悗锝庝簽娴犮垺銇勯幒瀣仾闁靛洤瀚版俊鎼佸Ψ閿曗偓濞堟绻濈喊妯哄⒉闁瑰憡濞婇悰顕€骞掑Δ鈧粻锝嗐亜閹捐泛鏋庨柛蹇擄躬濮婄粯鎷呴悷閭﹀殝缂備浇顕ч崐鍧楃嵁婵犲偆鍚嬮柛鈩冪懅瑜伴箖姊洪崫鍕偍闁搞劍妞介崺娑㈠箳閹炽劌缍婇弫鎰板醇閻曚椒鍝楅梻渚€娼уΛ妤呮晝椤忓牆钃熼柨鐔哄Т绾惧吋绻涢崱妯虹仼闁绘稏鍨介幃妤冩喆閸曨剛顦ㄩ梺鎼炲妼濞硷繝鎮伴纰辨建闁逞屽墴閻涱噣宕堕妸锕€顎撻梺鑽ゅ枑婢瑰棗危閿濆鈷掗柛灞剧懅缁愭梹绻涙担鍐叉处閸嬪鏌涢埄鍐︿簵婵炴垶顭傞弮鍫濆窛妞ゆ挶鍨洪弳鍛攽閻樺灚鏆╁┑顔芥綑鐓ら柨鏇炲€归崑鍌炴煛閸ャ儱鐏柛濠勬暬閺屻劌鈹戦崱娑扁偓妤€霉濠婂嫮绠橀柍褜鍓濋～澶娒洪弽顓熷剹闁稿瞼鍋涢拑鐔兼煥濞戞ê顏ф繛宀婁邯閹綊宕堕妸褋鍋為梺绋款儐閹瑰洤顕ｉ幘顔藉亹闁圭粯甯為弻褔姊婚崒娆愮グ鐎规洖鐏氶幈銊╁级閹炽劍妞介崺锟犲礃瑜忛崜銊モ攽閻愬弶顥為柛鏃€顨婇妴鍛存倻閼恒儳鍘甸梻鍌氬€搁顓㈠礉瀹ュ鐓冮梺鍨儏閻忔挳鏌熼鑲╃Ш鐎规洖鐖奸弫鎰板磼濮橆偄顥氶梻浣瑰劤濞存岸宕戦崱妯碱洸妞ゅ繐鎳愮弧鈧梺姹囧灲濞佳勭濠婂牊鐓熼柕濞у啫骞嬮梺绯曟杹閸嬫挸顪冮妶鍡楀潑闁稿鎸鹃惀顏嗙磼閵忕姴绫嶉悗娈垮枟婵炲﹪寮崘顔肩＜婵炴垶鑹鹃獮妤呮⒒娴ｅ憡鎯堥柛鐕佸亰瀹曟劙鎮界粙璺唵闂佽法鍠撴慨鐢告偂濞戞◤褰掓晲閸涱喗鍠愰柡宥忕磿缁辨挻鎷呴搹鐟扮闂佺儵鏅╅崹鍫曟偘椤旈敮鍋撻敐搴℃灍闁稿﹦鍏橀弻銈囧枈閸楃偛顫梺纭呮珪閻楃姴顫忛搹瑙勫珰闁哄被鍎洪埀顒侇殜閺岋綀绠涢弬鍨懙闂佽鍟崶銊ヤ汗閻庣懓澹婇崰妤冪礊鎼达絿纾介柛灞剧懅閸斿秹鎷戦崡鐐╂斀妞ゆ牗绋掔亸锕傛煛鐏炲墽鈽夐柍钘夘槸椤粓宕煎┑鍛煑闂傚倷绀侀幖顐⑽涘Δ鍛亱濠电姴瀚畷鍙夌箾閹存瑥鐏╃紒鐘差煼閹妫冨☉姗嗘闂佸搫妫岄埀顒佸墯濞撳鏌曢崼婵嗘殭濠碘€炽偢閺屻劑寮村Ο琛″亾濠靛绠栭柟顖嗗懏娈濋梻渚囧弿婵″洭宕ｉ崱娑欌拺闁告稑锕ゆ慨褏绱掗弻銉х暫闁挎繄鍋涢鍏煎緞鐎Ｑ勫闂傚倸鍊搁悧濠勭矙閹惧墎涓嶆慨妯垮煐閻撳啰鎲稿鍫濈婵炲棙鎸搁崒銊╂煛瀹ュ骸骞楅柛瀣ㄥ妼閳规垿鎮╁畷鍥舵殹闂佹娊鏀辩敮鎺楁箒闂佹寧绻傚ú銊╁箺閸屾稓绠鹃柛蹇曞帶婵洦銇勯鐐村仴闁诡喚鍏橀幃褔宕奸锝忕磼闂傚倷绀侀幖顐﹀箠閹炬枼鏋栨繛鎴炵矌娴滄瑩姊绘担鍛婂暈濞撴碍顨婂畷浼村冀閵娧屾锤濠德板€愰崑鎾绘煃鐟欏嫬鐏撮柟顔界懇瀵爼骞嬮悩杈ㄥ煕闂傚倷鐒﹂幃鍫曞礉瀹ュ拋娓诲ù鐘差儏缁犳牠鏌ㄩ悢鍝勑㈤柛妤佸▕閺岋綁寮崒婊呯槇缂備降鍔岄…宄邦潖濞差亝鐒绘繛鎴灻粊顔尖攽閻愭澘灏冮柛鏇ㄥ弾濞茬鈹戦悩璇у伐闁告﹢绠栭幃鐑芥焽閿旀儳寮抽梻浣告啞濞诧箓宕㈡ィ鍐╁亗闁逞屽墯娣囧﹪鎮欓鍕ㄥ亾閵堝鐭楅柍褜鍓熼弻娑欐償閳藉棛鍚嬬紒鈩冩尭閵嗘帒顫濋敐鍛闂備礁鎼張顒勬儎椤栫偛绠栭柍鍝勬噹缁犳稑霉閿濆懏璐￠柡鍡╁弮濮婄粯鎷呴悜妯烘畬缂備胶濮寸粔鐟扮暦绾懌浜归柟鐑樺灩閸樻挳姊洪悷閭﹀殶濠殿噮鍙冨畷鎴﹀箻缂佹ɑ娅滈柟鐓庣摠缁诲嫰寮弽顓熲拺?
// DoH/nginx 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幈闂佸綊鍋婇崹鎵閿曞倹鐓熼柕蹇嬪灮鐢稑菐閸パ嶈含闁诡喗鐟╅、鏃堝礋閵娿儰澹曢梺鍝勭▉閸樹粙宕戠€ｎ偆绡€濠电姴鍊绘晶鏇犵棯閸撗呭笡缂佺粯鐩獮瀣枎韫囨洑鎮ｉ梻浣规偠閸斿矂宕愰崸妤€钃熸繛鎴烆焽閺嗗棝鏌嶈閸撶喎鐣烽幋锔藉€烽柣鎴灻禍妤呮⒑缂佹ɑ鐓ラ柛姘儔閹ょ疀濞戞艾褰勯梺鎼炲劀鐏炶棄澹堥梻浣虹帛鐢顪冩禒瀣摕闁挎繂顦粻鎶芥煙鐎涙濡囬柟绋垮暟缁辨挻鎷呮禒瀣懙闁汇埄鍨界换婵嗙暦濞差亜鐒垫い鎺嶉檷娴滄粓鏌熼悜妯虹仴妞ゅ繆鏅濋惀顏堝箚瑜嶉幃鎴犵磼缂佹娲存鐐差儔閹瑩宕归銏＄彛闂佽瀛╅鏍窗閺嶎厽鍊舵繝闈涱儏閻撴﹢鏌熸潏楣冩闁哄拋鍓熼弻娑㈠即閵娿儰绨甸梺鍛婄箖椤ㄥ懘鈥旈崘顔嘉ч柛鈩冾焾閸嬩線姊洪崨濠冨闁告瑥楠哥叅闁告稑锕ｇ换鍡涙煏閸繄绠抽柛鎺嶅嵆閺屾盯鎮ゆ担鍝ヤ桓閻庤娲橀崝娆撳箖娴犲宸濆┑鐘插€烽悽缁樼節閻㈤潧浠滄俊顐ｎ殘閹广垽骞囬柇锔芥櫆濡炪倕绻愬Λ娑氬婵傚憡鐓冪憸婊堝礈閻旂厧鏄ラ柍褜鍓氶妵鍕箳閹存繍浠鹃梺鎶芥敱閸ㄥ湱妲愰幘瀛樺閻犲浄绱曢崝閿嬬節閳封偓鐏炲ジ鍋楅梺鍝勬湰濞茬喎鐣锋總鍛婂亜缂佸绨遍崑鎾诲箛閻楀牏鍘藉┑鐐村灥瀹曨剙鈻嶅鍥ｅ亾鐟欏嫭绀冮柨鏇樺灲閵嗕礁鈻庨幘鏉戞異闂佸疇妗ㄧ粈浣虹不娴煎瓨鈷掗柛灞剧懆閸忓本銇勯鐐靛ⅵ鐎殿喚鏁婚、妤呭磼濠婂懐鍘梻浣筋潐閸庤櫕鏅舵惔銊﹀剹闁瑰墽绮崑锝夊级閻愭潙顎滄い鎺斿枑缁绘盯骞橀幇浣哄悑闂佸搫鏈ú鐔风暦閻撳簶鏀介柛銉ㄥ煐椤ワ絽鈹戦悩鎰佸晱闁哥姵鐗犲畷鎰板锤濡も偓閽冪喐绻涢幋鐐垫噮缂佺娀绠栭弻娑⑩€﹂幋婵囩亐闂佺顑嗛幑鍥ь嚕閹绢喗鍊烽柤纰卞厵閸嬫帡姊绘担钘夊惞闁哥姵鍔楅崚鎺撴償閵忊剝鐝峰┑掳鍊曢崯顖炲汲濠婂嫪绻嗛柕鍫濇噺閸ｅ綊鏌ｉ幘瀛樼闁哄瞼鍠栭幃婊兾熼悜姗嗗敶闁荤喐绮庢晶妤冩暜濡ゅ懎鐤鹃柡灞诲劜閻撴洘绻涢幋婵嗚埞闁哄鐩弻锟犲幢濡ゅ啫鈪靛┑顔硷攻濡炶棄鐣烽锕€唯闁靛濡囨禍鏍⒒娴ｄ警鐒炬い鎴濇喘瀹曟垿濡堕崱鏇犵畾闂佹眹鍨婚…鍫㈢矆鐎ｎ偁浜滈柡宥庣厛閸ゃ倝鏌嶉崫鍕櫤闁绘挸绻愰…鍧楁嚋閻㈢偣鈧帡鏌ｉ敐鍫ュ摵缂佺粯鐩畷顏堝礃椤忓柊婊堟⒑閸濆嫮鐒跨紓宥勭窔閻涱喖顫滈埀顒€鐣烽敓鐘冲€烽柍杞版婢规洖鈹戦悩缁樻锭妞ゆ垵鎳橀幏鎴︽偄閸忚偐鍘搁梺鎼炲劗閺呮盯寮搁弮鍌滅＜闁抽敮鍋撻柛瀣崌濮婄粯绻濇惔鈥茬盎濠电偠顕滅粻鎾荤嵁閹版澘绠氭い顒夊墾缂嶄線寮幇鏉垮窛妞ゅ繐鎳忛弶鎼佹⒒娴ｈ櫣甯涢柛鏃€顭囨禍绋库枎閹邦厽鐝烽梺鍝勫暙閸婅崵澹曢崗绗轰簻闁哄啫鍊瑰▍鏇㈡煙閸愬弶宸濋柍褜鍓氶鏍窗濞戙埄鏁嬬憸鏃堝春閵夛箑绶為柟閭﹀墻濞煎﹪姊虹紒妯曟垿鎮ч幘璇茬鐎广儱顦闂佸憡娲﹂崹鎵尵瀹ュ鐓曢悘鐐村礃婢规ê霉閻樿秮顏堚€旈崘顔嘉ч幖绮光偓鑼埍闂備礁鎼幊鎰叏閹绢噮鏁嬮柨婵嗩槸闁卞洭鏌￠崶鈺佷户闁告ɑ鎮傚铏圭矙閹稿孩鎷辩紓渚囧枛闁帮綁骞冩ィ鍐╂優闁革富鍘鹃敍婵囩箾鏉堝墽绉繛鍜冪悼閺侇喖鈽夊杈╋紲闁诲函缍嗛崑鍕箔閹烘挶浜滄い鎾跺仦缁岃法绱掗悩宕団槈闁宠棄顦灒缁炬媽椴歌倴闂備浇宕甸崰鎰垝瀹€鈧划鏃堟偡閹殿喗娈鹃梺鍛婄箓鐎氼噣寮抽崱娑欑厱闁哄洢鍔岄崢鎾煙閻ゎ垱顏犵紒杈ㄥ笧缁辨帡濮€閻樿尙顔愰梻浣烘嚀閸熷灝顫濋敃鍌樷偓鍌滄崉閵娧咃紳闂佺鏈悷褏鎷规导瀛樼厱闁绘ɑ鍓氬▓婊勵殽閻愭潙鐏寸€规洘鍎奸ˇ鎾煛閸☆參妾紒缁樼☉椤斿繘顢欓懡銈呭毈闂備胶顭堥敃顏堝绩鏉堚晜宕叉繛鎴欏灩缁狙囨煙閹碱厼骞栭柛鎾冲船椤啴濡舵惔鈥茬盎缂備礁顦伴幐鎶藉春閻愬搫绠ｉ柨鏃囨閳ь剛鍏橀弻娑樷枎韫囷絾鈻撳┑鈽嗗亾缁绘繂顫忓ú顏勭闁绘劖褰冮‖鍫㈢磽閸屾氨孝闁挎洦浜悰顕€宕卞Ο鑲╂嚌闂侀€炲苯澧柣锝夋敱缁虹晫绮欑拠淇卞姂閺屻劑寮村Δ鈧禍鐐箾鐎电顎岄柛妯恒偢閸╃偤骞嬮敂钘変汗闁荤姴娉ч崘褏鐭楅梻鍌欑閹芥粓宕伴幘瀛樻珷婵°倕鎳庨悿楣冩煏婢跺棙娅嗛柣鎾跺枛楠炴牕菐椤掆偓閳ь兙鍊曢…鍥箛椤撶姷顔曢柣鐘叉厂閸涱垱娈奸柣搴ゎ潐濞叉牜绱炴繝鍥х疇婵犲﹤鐗嗙粈鍌炴煕濠靛嫬鍔ら柣锝夌畺濮婄粯鎷呴崫銉ㄥ┑鈽嗗亯濞夋洜鍒掗崼鐔虹懝闁逞屽墴楠炲啴鍨鹃弬銉︻潔濠碘槅鍨扮换鎰礊婵犲洤鏋侀柟鐗堟緲瀹告繃銇勯弮鍥棄闁告捇娼ч埞鎴︽偐閹颁礁鏅遍梺闈╃秵閸ㄨ泛顫忔禒瀣妞ゆ帒鍊甸弨铏節閻㈤潧孝婵炶绠撳畷鎰版煥鐎ｃ劋绨婚棅顐㈡处閹告悂顢旈妶澶嬬厱閻庯綆鍋呭畷宀€鈧鍠曠划娆撱€佸鈧幃娆撴嚑椤戣棄浜炬い鎾卞灪閳锋垹绱撴担鑲℃垹浜搁鐘电＜闁靛鍎抽崺锝団偓娈垮枟閹倸顕ｆ禒瀣垫晣闁绘柨鎼獮鎰版⒒娴ｈ鍋犻柛搴灦瀹曟洟鏌嗗鍡椻偓鍫曟煃閸濆嫬鏆熺痪鎯с偢閺屽秷顧侀柛鎾磋壘椤曘儵宕熼婵囧媰缂備礁顑呴妵妯荤珶閺囩倣鏃堟偐闂堟稐绮堕梺璇茬箲缁诲牆鐣峰┑鍡╂僵妞ゆ挻绮堢花濠氭⒑閹稿孩鐓ラ柣蹇旂箓閻ｇ敻宕卞☉娆戝幍濡炪倖娲栧Λ娑氬姬閳ь剟鎮楀▓鍨灍闁规瓕娅曢幈銊╁焵椤掑嫭鐓ユ繛鎴灻鈺呮偨椤栨稑鍔ら柍瑙勫灴閹晠宕归锝嗙槑濠电姵顔栭崰鏍敋椤撱垹绠查柕蹇嬪€曠粻铏繆閵堝倸浜剧紓浣插亾闁糕剝绋掗悡娑橆熆鐠虹尨鍔熸慨妯稿姂閺岋繝鍩€椤掍胶顩烽悗锝庡亞閸欏棗鈹戦悙鏉戠仸闁挎碍銇勮箛锝勭凹缂佺粯鐩畷銊╊敊閻ｅ矈妲剁紓鍌欐祰妞村摜鏁Δ鈧…鍥疀濞戞鈺呮煏婢舵稓鐣抽柣褌绶氶弻锝嗘償閵堝孩缍堝┑鐐插级椤洭寮鈧獮鎺懳旈埀顒勬偂閳ユ剚鐔嗛悹鍝勩偨閿熺姵鍋勯柛蹇曞帶濞堟劙姊洪崘鍙夋儓闁稿﹦顭堥埢鎾诲箛閻楀牃鎷婚梺绋挎湰閼归箖鍩€椤掆偓閸㈡煡婀侀梺鎼炲労閸擄箓寮€ｎ喗鐓涚€广儱楠搁獮鏍煕閵娿儱鈧潡寮诲☉妯滄棃宕橀妸銏犱壕闁挎繂鎳夊Σ鍫ユ煏韫囨洖啸妞わ负鍔戝娲礃閸欏鍎撻梺绋匡功椤牓顢氶敐澶婇敜婵°倓璁查幏娲⒒娓氬洤浜為柛瀣洴閹崇喖顢涘☉娆愮彿闂佸湱铏庨崰妤呮偂濞戙垺鍊堕柣鎰仛濞呮洟寮崼鐔虹瘈缁炬澘顦辩壕鍧楁煕韫囨枏鎴炵┍婵犲洦鍋い鏍电稻椤庡绱撻崒娆愮グ濞存粠鍓熷畷鎴︽倷閸偄搴婂┑鐘绘涧椤戝懘锝為崨瀛樼叆婵犻潧妫欓ˉ鐘绘煏閸℃ê濮嶉柡宀嬬稻閹棃濮€閵忊晜娈规繝鐢靛О閸ㄦ椽鏁冮姀銈嗘櫜闁绘劕澧庨悿鈧梺鍝勬川婵兘鏁嶅鍐ｆ斀闁宠棄妫楅悘銉︺亜閺囧棗娴傞弫渚€鏌熼崜褏甯涢柍閿嬪灴閺屾稑鈽夊鍫濆闂佸疇妫勯ˇ鐢稿蓟閵娾晛鍗虫俊顖濄€€閸嬫挸鈹戦崱鈺佹闂佽鍎煎Λ鍕煁閸ヮ剚鐓忓鑸电閸氬倿鏌熼柨瀣仢婵﹥妞藉畷銊︾節閸曘劍顫嶉梻浣瑰濞插繘宕愬┑瀣畺婵せ鍋撳┑锛勬焿椤﹀鐥崜褎娅曢柍褜鍓欑粻宥夊磿闁秴绠柟瀛樼箥閸熷懘鏌曟径鍡樻珕闁绘挾鍠栭弻銊モ攽閸℃ê顦╅梺缁樺笒濞硷繝寮婚弴銏犵倞闁靛鍎遍～鎺懳旈悩闈涗沪閻㈩垽绻濋悰顔碱潨閳ь剙鐣峰Δ鍛窛妞ゆ牗绮庡崗闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗ù锝呮贡閻濊泛鈹戦悩鍙夋悙缁炬儳顭烽弻娑㈠焺閸愵亖妲堢紓浣瑰姈椤ㄥ﹪鐛弽顐㈠灊閻熸瑥瀚峰鎴︽⒑閹肩偛鈧牕煤閻旂厧钃熸繛鎴炵矤濡茬厧顪冮妶鍐ㄥ婵☆偅绻堝畷鍝勨槈閵忕姷顓洪梺鎸庢⒒閺咁偊宕㈤柆宥嗏拺鐟滅増甯掓禍浼存煕閹惧鎳呴柣銉海椤﹀綊鏌″畝鈧崰鏍€佸☉姗嗘僵闁告鍋愰崣娲⒒娴ｅ憡鎯堥柣顒€銈稿畷浼村冀瑜忛弳锔芥叏濡炶浜鹃梺鐟扮－婵炩偓妞ゃ垺顨婂畷鎺楀Χ閸涱喗鐝ㄩ梻鍌欐祰椤曆冾潩閿曞偊缍栧璺衡姇閸濆嫷鐓ラ柛顐ｇ箖閻庮剚绻濋悽闈浶ｉ柤褰掔畺瀵即濡烽埡鍌滃帗閻熸粍绮撳畷婊冾潩鐠轰綍锕傛煕閺囥劌鐏犵紒鐘冲▕閺岀喓鈧稒顭囩粻銉ッ归悩鑽ょ暫婵﹨娅ｇ划娆撳礌閳ュ啿顫犻梻浣芥〃缁€渚€鈥﹀畡閭﹀殨閻犲洦绁村Σ鍫熺節婵犲倹鍣介柡澶嬫倐濮婃椽鎮烽幍顔昏檸闂佸摜鍣ラ崹鎶芥偡瑜庣换婵嬫偨闂堟稐绮堕梺瀹︽澘濡块柛鎺撳浮楠炴﹢宕滄担椋庣憹闂備礁鎼拠鐐典沪閸撗呪枆濠电姷鏁搁崑娑樜熸繝鍐洸婵犻潧顑嗛崐鍨攽閻樺磭顣查柣鎾跺枛閺岀喖鏌囬敃鈧悘鐘绘煢閸曨偁鍋㈤柟顔筋殔椤繈顢橀悢鍝勫殥婵＄偑鍊ら崣鈧柛搴☆煼绡撳〒姘ｅ亾闁哄本鐩幃鈺呭箛娴ｅ湱鏉瑰┑鐘殿暜缁辨洟寮拠鑼殾闁绘梻鈷堥弫宥嗘叏濮楀棗寮炬俊顐ゅ枛濮婃椽鎳￠妶鍛咃絽霉濠娾偓缁瑥鐣峰┑鍥ㄥ劅妞ゎ厽鍨堕弲鈺傜節閵忥絾纭鹃懝宀勬煛閳ь剚绂掔€ｎ偆鍘介梺瑙勫婢ф鈽夎閹粙顢涘顒夋殺缂備浇椴哥敮鐐垫閹烘嚦鐔煎即閻旈浠哥紓浣哥焷妞村摜鎹㈠┑瀣倞鐟滃繘顢欓幇顓犵閺夊牆澧界粔顒勬煕婵犱胶鐭欏┑鈩冪摃椤︽娊鏌涢悩鍐插闁绘搩鍋婂畷鍫曞Ω瑜夋慨鍥⒑缁嬫鍎愰拑閬嶆煏閸パ冾伃鐎殿噮鍓涢幑鍕Ω閿旂偓顎囬梻鍌欐祰閵嗏偓闁稿鎸搁湁闁绘ê妯婇崕鎰版煟閹捐泛鏋涢柡宀€鍠栭弻鍥晝閳ь剚鏅舵导瀛樼厵闁伙絽鑻埢鍫ユ煙椤旂瓔鐒剧紒鐘崇⊕閹棃鍨惧畷鍥ュ仏闂傚倷绀侀幖顐ゆ偖椤愶箑纾规繝闈涱儏缁犳牠鏌ｉ幋锝嗩棄缂佺嫏鍥х閻庢稒蓱鐏忎即鎮楅崹顐ゅ⒌婵﹦绮幏鍛矙閹稿骸鈧垰鈹戦敍鍕哗濠殿喓鍊楃划瀣吋婢跺﹪鍞堕梺鍝勬川閸嬬娀骞楅弴銏♀拺缂侇垱娲栨晶鍙夈亜閵娿儳绠荤€规洏鍎抽埀顒婄秵閸犳鎮￠弴鐔虹闁瑰瓨绻傞懜褰掓煟韫囨艾鐨虹紒杈ㄥ浮閺屽棗顓奸崟顒€鏋ゆ繝娈垮枛閿曪妇鍒掗鐐茬闁告侗鍨虫す鎶芥倵閿濆骸浜炵紒澶屽厴閺岋絾鎯旈妶搴㈢秷濠电偛寮堕敃銏ゅ极閸愵喖顫呴柕鍫濆暊閸嬫挻鎷呴崜鍙夊兊闂佸綊顣﹂悞锕傜€峰┑鐘垫暩婵即宕规總闈╃稏濠㈣泛鈯曞ú顏呭亜濡炲瀛╁▓鎯р攽鎺抽崐鏇㈠箠韫囨稑鍚归柛鎰靛枟閻撳啴鏌涘┑鍡楊仼闁哄棛鍠愭穱濠囧箵閹烘挸鏋犲┑顔硷工椤嘲鐣烽幒鎴僵妞ゆ垼妫勬禍楣冩煙闂傚鍔嶉柛瀣儏閳规垿鎮╅幓鎺濅紑闂佹椿鍘介悷鈺呭蓟閻旂厧绠氱憸蹇涘箺閻樺磭绠鹃柛娑卞灠閳诲牓鏌＄仦鐐妞ゆ洏鍎遍埥澶愬閳哄倹顓肩紓鍌欓檷閸斿矂鈥﹀畡閭︽綎婵炲樊浜滄导鐘绘煕閺囥劌浜濇繛鍫熺矒濮婅櫣绮欏▎鎯у壉闂佸湱鎳撳ú顓烆嚕鐠囨祴妲堥柕蹇曞Х閻嫰姊洪柅鐐茶嫰婢ф挳鏌熼鎯у幋濠碉紕鍏橀崺锛勨偓锝庡墰閳笺倝姊绘繝搴′簻婵炶绠戠叅婵犲﹤鎳庨ˉ姘舵煕韫囨稒锛熺紒璇叉閵囧嫰寮介妸褏顓奸梺鎼炲€栭悷锕傚Φ閸曨剚鍎熼柕蹇娾偓宕囷紦婵犳鍠栭敃銉ヮ渻娴犲绠犻柨鐔哄Т鍥撮梺鍛婁緱閸犳岸鍩€椤掑倸浜剧紒缁樼箞婵偓闁靛繆鈧櫕鐣繝纰樻閸嬪懐绮欓幘璇茬闁圭儤鎸剧弧鈧梺鎼炲劘閸斿矂鍩€椤掆偓椤兘寮婚妶澶婄畳闁圭儤鍨垫慨澶愭⒑瑜版帗鏁辨俊鐐舵椤繘鎼归崷顓犵厯闁荤姵浜介崝搴敊閸ヮ剚鈷戦柛娑橈龚婢规ɑ銇勯幋婵愭Ч濞ｅ洤锕幃浠嬪川婵犲倷绨婚梻浣侯焾缁绘宕戦幇鏉垮偍闂侇剙绉甸埛鎴︽⒒閸喍绶遍柣鎺楃畺閺屾盯寮▎鎯у壎閻庤娲忛崕鐢搞€侀弴銏犖ч柛銉㈡櫔缁卞弶淇婇悙顏勨偓鏍ь潖婵犳艾鐓曢柛顐犲劚绾惧潡鏌￠崘锝呬壕濡炪們鍔婇崕鐢稿箖濞嗘挸绾ч柟瀵稿仩閻т焦淇婇妶鍥ラ柛瀣洴椤㈡牗寰勬繝搴℃濡炪倖娲嶉崑鎾绘煕閳规儳浜炬俊鐐€栫敮鎺斺偓姘憸缁辩偤寮介鐔蜂壕妤犵偛鐏濋崝姘舵煙閸愬樊妯€鐎殿喓鍔嶇粋鎺斺偓锝庡亞閸樹粙姊鸿ぐ鎺戜喊闁告挻鐟ч惀顏囶槼闁靛洤瀚版俊鐤槻濞寸娀浜堕弻锛勪沪閸撗勫垱婵犵鍓濋悺鏇⑺囬弻銉︾厸閻庯綆鍋嗛妴鎺楁煃瑜滈崜姘额敊閺嶎厼绐楁俊銈呮噹缁犳牠鏌涚仦鎯у毈闁搞倖娲栭湁闁绘ê妯婇崕娑㈡煛娴ｅ憡鍠橀柡宀嬬秮瀵剟宕归楣冪崜闂佽瀛╅懝楣冨Χ缁嬫娼栧┑鐘宠壘绾惧吋鎱ㄥ鍡楀幋闁稿鎹囬獮鏍ㄦ媴閻熼缃曞┑鐘垫暩婵鈧凹鍣ｉ幆灞解枎閹惧鍘甸梺璇″瀻閸涱喗鍠栨繝鐢靛仜閹虫劖绻涢埀顒勬煛瀹€瀣瘈鐎规洖鐖兼俊鐑藉Ψ瑜岄惀顏堟⒒娴ｄ警鐒鹃柨鏇樺€濋幃妯侯潩鐠鸿櫣鐤勯梺闈涱焾閸庢瑩鎮㈤崗鐓庝簵闁硅偐琛ラ埀顒冨皺鐢盯姊虹拠鎻掝劉妞ゆ梹鐗犲畷鎶筋敋閳ь剙鐣烽弶璇炬棃宕ㄩ鍥风畵閺屾盯寮撮妸銉т哗濡炪値鍋勯幊姗€寮诲澶婄厸濞达絽鎲″▓璺侯渻閵堝繒绱伴柛妤€鍟块～蹇撁洪鍕祶濡炪倖鎸嗛崨顓炐梻鍌欑劍鐎笛兠鸿箛娑樼？鐎规洖娲ら崹婵嬫煃閸濆嫭鍣洪柛瀣閹绠涙惔锝呭闂佸搫鍊甸崑鎾绘⒒閸屾瑧顦﹀鐟帮躬瀹曟垿宕卞☉妯肩崶闁硅偐琛ュ褔寮搁弮鍫熷€堕柣鎰暩閹藉倹绻涢幘鎰佺吋闁哄本娲熷畷鐓庘攽鐎ｇ鍋撳Δ渚囨闁绘劖娼欓悘鎾煛鐏炲墽娲寸€殿噮鍣ｉ崺鈧い鎺戝閸ㄥ倿鏌ｉ姀鐘冲暈闁稿鏅濋埀顒傛嚀鐎氫即宕戞繝鍌ょ劷闁靛ň鏅滈悡鍐喐濠婂牆绀堥柣鏂垮悑閸嬫ɑ銇勯弴妤€浜鹃悗瑙勬礀瀹曨剟锝炲┑瀣濠㈣泛鍘滈弲娑㈠煘閹达附鍋愭繛鍫熷濮ｅ本绻濆▓鍨灈缂佸鎳撻悾鐑藉Ω閳哄﹥鏅┑鐘诧攻閸旀姊?
//   1) 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婄婵犲灚鍔栫紞妤呮⒑闁偛鑻晶顕€鏌涙繝鍌涘仴妤犵偞鍔栫换婵嬪礃椤忓棗楠勯梻浣稿暱閹碱偊顢栭崶鈺冪煋妞ゆ棃鏁崑鎾舵喆閸曨剛锛橀梺鍛婃⒐閸ㄧ敻顢氶敐澶婇唶闁哄洨鍋熼娲⒑缂佹鎳冮柟铏姍閻涱噣濮€閵堝棌鎷婚梺绋挎湰閻熝呯玻閺冨牊鐓冪憸婊堝礈濞戙垹纾绘繛鎴欏灪閸ゆ劖銇勯弽銊р姇婵炲懐濞€閺岀喓绱掗姀鐘崇亶闂佺粯鎸婚惄顖炲箖濮椻偓閹瑩骞撻幒鍡樺瘱闂備線娼уΛ娆戞暜閿熺姴钃熺€广儱鐗滃銊╂⒑閸涘﹥灏柛鏂挎捣濡叉劙鎮欓崫鍕€垮┑鐐村灦椤洭鏁嶅☉銏♀拺闁告稑锕ｇ欢閬嶆煕濮椻偓缁犳牕顕ｉ幎鑺ユ櫆闂佹鍨版禍楣冩煕韫囨搩妲稿ù婊堢畺濮婃椽宕ㄦ繝鍐槱闂佸憡鎸婚惄顖氱暦?5444(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幘缂佺偓婢樺畷顒佹櫠婵犳碍鐓曢柣鎰皺閸╋絾鎱ㄦ繝鍕笡闁瑰嘲鎳樺畷銊︾節閸屾稒鐣肩紓鍌氬€风粈渚€骞栭銏╃劷闁炽儱纾弳锕傛煛閸ャ儱鐏╃紒鐙€鍣ｉ弻銈夊箒閹烘垵濮庨梺鍛婃尭缂嶅﹤顫忓ú顏呭仭闁哄绨遍幐鍐磽娓氬洤娅橀柛銊ョ埣楠炲啴鏁撻悩鎻掑祮闂佺粯姊荤换婵堣姳婵犳碍鈷戦悷娆忓缁舵彃顭胯濞撮鍒掑▎鎾村殥闁靛牆娲ㄩ敍婊堟⒑闁偛鑻晶瀵糕偓瑙勬礃閿曘垽銆佸▎鎾村仼閻忕偠妫勭粻? / 5454(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幈闂佸綊鍋婇崜娆戠棯瑜旈弻锝夊Χ閸涱収浼冨┑顔硷功缁垶骞忛崨顖滈┏閻庯綆浜濋悾浼存⒒娴ｈ鍋犻柛鏂跨焸婵″爼骞栨担鍝ョ暫濠殿喗銇涢崑鎾搭殽閻愬弶鍠樻い銏＄☉铻ｉ悷娆忓濞兼牠姊婚崒娆戭槮婵犫偓闁秴纾块柕鍫濐槸缁犱即鏌熼梻瀵割槮闁绘帒鐏氶妵鍕箳閹存繃娈梺鍛婅壘濠€杈╂閹烘挻缍囬柕濞垮劤閻熴劑姊虹€圭姵顥夋い锔诲灦閿濈偛顭ㄩ崼婵嗚€垮┑掳鍊愰崑鎾愁熆閻熼偊妯€婵? / 443(nginx) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柣妯荤垹閸ャ劎鍘遍柣蹇曞仜婢т粙鎮￠姘肩唵閻熸瑥瀚粈鈧梺瀹狀潐閸ㄥ潡銆佸▎鎾村剹妞ゆ劦鍋傜花濠氭煟閿濆洤鍘存い銏☆殜瀹曠喖顢曢姀鐘辨喚闂備浇顕уù鐑藉极閸濄儲鍏滈柛顐ｆ礀绾惧鏌熼幑鎰厫闁哥姴妫濋弻娑㈠即閵娿儱顫銈忓瘜閸犳牠鈥旈崘顔嘉ч幖绮光偓鑼泿婵＄偑鍊х粻鎾寸閸洖鏄ラ柍褜鍓氶妵鍕箳閹存繍浠惧┑鐐村毆閸ャ劎鍘梺鍓插亖閸╁嫰鎮為悙顑句簻闁哄浂浜炵粔顔锯偓瑙勬礀閵堢顕ｉ幘顔藉€锋繛鍫濈仢閹牓姊婚崒娆掑厡缂侇噮鍨堕弫瀣⒑鐠囪尙绠查柤瑙勫礃椤ｉ箖鏌熼崗鑲╂殬闁搞劏宕靛褔鍩€椤掑嫭鈷戞慨鐟版搐閻忓弶绻涙担鍐插椤╃兘鏌ㄩ弴鐐测偓褰掓偂濞嗘垹纾奸悗锝庡亽閸庛儵鏌ｉ幘鐟颁汗闁逞屽墲椤煤濡警娓婚柦妯侯槺閺嗭附鎱ㄥ璇蹭壕濡炪們鍨洪悷鈺呭箠閺嶎厼鐓涢悗锝庡墮閼板潡姊婚崒姘偓鐑芥嚄閸洖绠犻柟鎯х摠濞呯娀鏌ｉ敐鍛板闁搞劍绻冮妵鍕冀椤愵澀娌梺绋款儏閸婂湱鎹㈠☉銏犵婵炲棗绻掓禒楣冩⒑缁嬪尅宸ラ柣鏍с偢瀵鏁愭径濠勭杸濡炪倖甯掗崐椋庡垝闂堟稈鏀介柣鎰级閸ｇ儤绻涢崣澶屽⒌闁炽儻绠撻幊妤咁敍閿濆棭鈧绱撻崒娆掑厡濠殿喚鏁婚獮鎴﹀炊瑜滈崵鏇炩攽閻樺磭顣查柛瀣閺屟嗙疀閹剧纭€闂佺粯鎸婚幑鍥ь潖濞差亜宸濆┑鐘插閳娊姊洪崨濠冨鞍缂佽鐗嗛锝夘敃閿濆啫浜濋梺鍛婂姀閺呮盯鍩€椤掑倹鏆柡灞剧洴閳ワ箓骞嬪┑鍛嚬缂傚倷娴囬崺鏍偂閿熺姴绠栨俊銈傚亾妞ゎ偅绻堥幃鈩冩償閵忋垹缍嗛梻鍌欒兌绾爼寮笟鈧畷鎴﹀箻閹颁焦瀵岄梺闈涚墕濡绮幒妤佺叆闁哄浂浜滈々顒傜磼椤旇姤顥堥柟顔荤矙瀹曘劍绻濋崟顐㈢疄濠电姷顣藉Σ鍛村垂鏉堛劎顩插ù鐘差儏绾惧鏌曡箛濠冾潑婵炲牅绮欓弻锝夊箛椤掆偓缁狙勩亜閵夛絽鐏柍褜鍓濋～澶娒洪弽顓炍ч柛娑橈梗缁诲棝鏌ｉ幋锝呅撻柡鍛矒閹嘲鈻庤箛鎿冧患濡?
//   2) 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁绘劦鍓欓崝銈囩磽瀹ュ拑韬€殿喖顭烽幃銏ゅ礂鐏忔牗瀚介梺璇查叄濞佳勭珶婵犲伣锝夘敊閸撗咃紲闂佺粯鍔﹂崜娆撳礉閵堝洨纾界€广儱鎷戦煬顒傗偓娈垮枛椤兘寮崘顔肩劦妞ゆ帒鍟版禍钘夆攽閻愬瓨灏伴柛鎴濈秺瀹曞綊鏌嗗鍛幒闂佸湱鍋撻弸濂稿绩娴犲鐓熼柟閭﹀灠閻ㄦ椽鏌熼悾灞解枅闁哄矉绲借灃闁告劑鍔夐弸鍛存倵濞堝灝鏋涙い顓炲槻椤曪綁骞橀钘変簻闂佹儳绻楅鏍疮鎼淬劍鈷掑ù锝堟鐢盯鏌涢弮鎾剁暤鐎规洘绮岄～婵嬫嚋闂堟稐鎴烽梻浣告惈閸燁偊宕愰懡銈咁棜闁芥ê顥㈣ぐ鎺撴櫜闁告侗鍠楅崕鎾绘煛瀹ュ繒绡€婵﹨娅ｇ槐鎺懳熺紒妯哄壍缂傚倷鑳剁划顖滄崲閸岀儐鏁嬮柨婵嗘缁♀偓濠殿喗锕╅崜娆忕暤閸℃稒鈷戦柟绋挎捣閳藉鎮楀鐓庢珝鐎殿喖鐖奸獮鏍ㄦ媴閸︻厼骞楅梺鐟板悑閹矂宕瑰畷鍥╃煋闂傚牊鑰介悷閭︾叆闁告洦鍘奸埛宀勬倵鐟欏嫭绀冮柛銊ユ健閻涱喖螣閸忕厧鐝伴梺鍦帛鐢帞绮婇鈧缁樻媴閾忕懓绗￠梺鎸庡哺閺屾稑顫濋敐鍛缂傚倸鍊烽悞锕傘€冮崨姝ゅ洭顢涢悙鑼幋闂佺鎻梽鍕磹閻戣姤鐓涘璺侯儏閻忊晛霉閻樻瑥娲﹂埛鎴︽煙閼测晛浠滃┑鈥冲悑閵囧嫰顢橀悙鍙壭╅梺浼欑悼閸忔﹢寮幇顓炵窞濠电姴瀚烽崯瀣磽閸屾艾鈧兘鎮為敃鍌椻偓锕傚炊椤掆偓閸屻劑姊洪鈧粔鐢稿煕閹达附鈷掗柛顐ゅ枍缁惰鲸淇婇幓鎺旂Ш闁哄本鐩俊姝岊槼闁哄棌鏅犻弻宥堫檨闁告挻宀稿畷娆掋亹閹烘垵鍋嶆繝闈涘€搁幉锟犲磻閳哄懏鐓熼柟杈剧稻椤ュ绱掗埀顒勫礃椤忎礁浜鹃柣鐔告緲椤ュ繘鏌涢悩鎰佹畼闁逞屽墴濞佳囨儗閸屾凹娼栨繛宸簼椤ュ牊绻涢幋鐐跺妞わ絾妞藉铏规兜閸涱喚褰ч梺鍛婃⒐椤ㄥ牓宕氶幒鎾剁瘈婵﹩鍓涢鐓庮渻閵堝棙鈷掗柟铏尰缁傚秵銈ｉ崘鈺佷画濠电偛妫楃换鎰邦敂椤忓嫷鐔嗛悹鍝勬惈椤忣參鏌＄仦鍓ф创妤犵偞顭囬幑鍕倻濡棿閭梺璇叉唉椤煤濡警娓婚柤鎭掑劙缁诲棝鏌ｉ幋锝呅撻柡鍛箞閺屽秷顧侀柛鎾寸☉閳诲酣濮€閵堝棗鈧兘鏌ｉ幋鐑嗙劷闁告鏁诲缁樼瑹閸パ冧紟缂備胶濮甸崹鍫曞疾閸洘鍋嬮柛顐ｇ◥缁ㄥ姊洪崫鍕殜闁稿鎹囬弻鐔兼偡閻楀牊鎮欏銈庡亜缁绘劗鍙呭銈呯箰鐎氼剛绮ｅ☉娆戠瘈闁汇垽娼у瓭闂佺锕ラ幃鍌濇＂闂佸壊鍋侀崕鏌ュ煕閹达附鐓曟繛鎴烇公濮婃绱掗崼鏇氭喚闁哄本鐩俊鎼佸煛閳ь剟骞夐崜褉鍋撳▓鍨灍闁绘挴鍓濇穱濠囧箹娴ｈ倽銊╂煏韫囧﹤澧查柡澶嬫倐濮婄粯鎷呴搹鐟扮闁藉啴浜堕弻娑㈠箻閺夋垵鎽甸悗瑙勬处閸ㄧ數绮诲☉妯锋婵炲棙鍨甸獮鍫ユ⒒娴ｈ櫣甯涢拑閬嶆煕閹剧澹橀柍缁樻尭閳诲酣骞嬮悙鍙夌€鹃梻浣筋潐椤旀牠宕伴弽顓炵厺闁割偆鍠愰崣蹇撯攽閻樻彃鏆為柕鍥ㄧ箘閳ь剝顫夊ú蹇涘礉瀹ュ洦宕叉繝闈涱儏缁€鍐┿亜韫囨挸顏╂慨瑙勭叀濮婂宕掑顑藉亾妞嬪海鐭嗗ù锝堫潐閸庣喐绻濇繝鍌涘櫧缂佲偓婵犲倶鈧帒顫濋敐鍛闂備浇顕栭崰鏇㈠础閹跺鈧礁顫濈捄铏瑰姦濡炪倖甯掔€氼喖鐣垫笟鈧幃妤呮晲鎼粹剝鐏嶉梺鍝勬噺缁诲牓寮诲鍫闂佸憡鎸荤粙鎾诲礆閹烘閱囬柍鍨涙櫅娴滈箖鏌ㄥ┑鍡涱€楀ù婊勭墪闇夋繝濠傚閻帡鏌＄仦绯曞亾閹颁礁鎮戦梺鍛婂姂閸斿矂鈥栨径鎰拺婵懓娲ゆ俊浠嬫煕閵娿儳鍩ｅ┑锛勬暬瀹曠喖顢涘槌栧晪闂備焦鍎冲ù姘跺磻閸涙潙纾块柟鍓х帛閳锋垿鎮楅崷顓烆€屾繛鍏煎姍閺屾盯濡搁妷锕€浠村Δ鐘靛仜閸熸挳宕洪敓鐘茬闁宠桨鑳惰ぐ鎾煟閻斿摜鐭婄紒澶屾暩閳ь剟娼ч妶鎼佺嵁閸ヮ剚鍋嬮柛顐ｇ鐎氬ジ姊绘担钘夊惞闁哥姵鍔曢…鍨熸担鏇悼缁辨帒螣闂€鎰泿闂備胶鎳撻悺銊ф崲閸愵啟澶愭偐缂佹鍘撻悷婊勭矒瀹曟粓鎮㈡總澶屽姺閻熸粍姊规穱濠囧礈娴ｈ櫣鐓撻柣鐘充航閸斿秴鈻撴ィ鍐┾拺閻犲洠鈧磭浠搁梺绋款儏閿曘倕宓勯梺缁樺灱婵倝鍩涢幋锔藉仩婵炴垶宸婚崑鎾诲礂閸涱収妫?sniroute.cmd up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存い銏℃礋閺佹劙宕卞▎娆戝枠闂傚倷绶氶埀顒傚仜閼活垶宕㈤幖浣圭厽闁硅櫣鍋涢々顒勬煙楠炲灝鐏╅柍瑙勫灩閳ь剨缍嗘禍鐐烘晬濠婂啠鏀介幒鎶藉磹閹剧粯鍤勯柤鍝ユ暩椤╁弶绻涢崱妯哄缂佲檧鍋撻梻浣圭湽閸ㄨ棄顭囪缁傛帡鏁冮崒娑氬幈濡炪値鍘介崹鐢告倶閿旈敮鍋撶憴鍕闁硅櫕鎸鹃崣鍛渻閵堝懐绠伴悗姘煎櫍瀵槒顦查柍瑙勫灴閹晝绱掑Ο濠氭暘婵＄偑鍊栭崹闈浳涘┑瀣畺闂勫洨绮诲☉妯锋婵炲棙鍨甸獮鍫熶繆閻愵亜鈧牠宕濊瀵板﹦鎹勯妸褌绗夐梺闈涱檧缂傛氨鎹㈤崱娑欑厵缂備焦锚缁椦呯磼婢跺銇濋柡宀嬬磿娴狅箓宕滆閳ь剚甯￠弻宥堫檨闁告挶鍔戦獮鈧柕澶嗘櫅缁€瀣亜閹烘垵鈧摜绱為崘顔解拻闁稿本鐟чˇ锕傛煙绾板崬浜伴柟顖氭湰瀵板嫮浠﹂悾宀€鐡橀梻浣侯焾閿曪箓宕伴幘瀵割洸濞寸姴顑嗛埛鎺懨归敐鍥ㄥ殌闁告柣鍊楃槐鎺旂磼濡櫣鐟ㄩ梺瀹狀嚙缁夎鎱ㄩ埀顒勬煏韫囧﹥顫婃繛鑲╁枛濮婅櫣绱掑Ο鍝勵潓濠碘槅鍨伴敃銈夊Υ閹烘挾绡€婵﹩鍘鹃崢顏堟⒑閸撴彃浜濈紒璇插€块崺鈧い鎺戝€归崵鈧梺瀹狀嚙缁夌數鎹㈠┑瀣倞闁靛ě鍐ㄧ闂傚倷绶氶埀顒傚仜閼活垱鏅堕幘顔界厸閻忕偠濮らˉ婊勩亜閹剧偨鍋㈢€规洏鍔戝畷鎺楁晜閸撗勬闂備礁鎼張顒勬儎椤栨凹鍤曢柟缁㈠枛鎯熼梺闈涳紡閸屾碍鎲㈢紓鍌氬€搁崐鐑芥嚄閼稿灚鍙忛悗鐢殿焾椤ユ碍銇勯幘鍗炵仼闁绘帒鐏氶妵鍕箳閸℃ぞ澹曠紓鍌欒兌婵绮旈悷鎵殾闁哄洢鍨圭涵鈧梺璇茬箺椤曆呯礊婵犲倻鏆﹂柟鐑樺毄閹烘梹鍏滃瀣捣琚︽俊鐐€戦崹娲偡瑜斿畷鐗堢節閸愨晝绉堕梺闈涱焾閸庡啿危閿濆鈷掗柛灞剧懅缁愭梹绻涙担鍐叉处閸嬪鏌涢埄鍐槈缂佺姷濞€楠炴牠骞栭鐘插弗闂佽桨绀佸Λ婵嬪蓟閺囷紕鐤€閻庯綆浜栭崑鎾诲冀椤撶喎浜楅梺缁樻煥閸氬鎮￠悢鍏肩厸闁稿本姘ㄦ禒銏㈢磼閻欐瑥瀚粻楣冩煕濞戝崬寮块柡鈧紒妯镐簻闁靛绲介崝锕傛煙椤旂晫鎳囨鐐存崌楠炴帡骞橀幖顓炴櫃婵犵數濮烽弫鎼佸磻閻愬搫鍨傜憸鐗堝笒閺嬩線鎮归崶顏勭毢闁哄棴闄勯妵鍕疀閹炬潙娅ゆ繝娈垮灡閹告娊寮诲☉婊呯杸闁瑰灝鍟晥闂備浇顕х€涒晠骞冮崒姘兼綎濠电姵鑹剧壕鍏兼叏濮楀棗鍘撮柛瀣尭閻ｏ繝骞婃繝鍌滅Ш鐎规洖鐖奸、妤呭焵椤掑啰绠介梻鍌欑窔閳ь剛鍋涢懟顖涙櫠閹绢喗鐓涢悘鐐插⒔閵嗘帡鏌嶈閸撱劎绱為崱娑樼；闁告侗鍘鹃弳锔锯偓鍏夊亾闁告洦鍓涢崢鎼佹煟韫囨洖浠滃褌绮欓崺銉﹀緞閹邦厽鍤夐梺鍝勭▉閸樹粙鎮￠弴銏＄厵闁逛絻娅曞▍鍛攽閿涘嫬甯舵い顓炴健閹兘鏌囬敂瑙ｆ瀰婵?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存い銏℃礋閺佹劙宕卞▎娆戠┛闂傚倷绶氶埀顒傚仜閼活垱鏅堕崜褏纾奸柣妯挎珪鐏忣參鏌ｉ敐澶嬫暠缂佽櫣鏅划娆忊枎閻愵剛绉遍梻鍌欒兌閸嬨劑宕曢柆宥呯柈閻犳亽鍔忔慨鎶芥煏婢跺棙娅嗛柍閿嬪笒闇夐柨婵嗘祩閻掔偓銇勯妷銉х闁哄本绋撻埀顒婄岛閺呮繄绮ｉ弮鍌楀亾鐟欏嫭绀冩繛鍙壝…鍥疀濞戞鍘搁梺鍛婂姦娴滅偤宕濈捄琛℃斀闁绘﹩鍠栭悘閬嶆煕閳哄倻澧电€规洘绻堝浠嬵敇閻旇渹绨垫繝鐢靛仜濡瑩骞愰崫銉ф／鐟滄棃寮诲☉銏╂晝闁挎繂娲ㄩ悾鐢告⒑閹惰姤鏁辨俊顐㈠暣瀵鏁愭径瀣珳闂佸壊鍋呯划宀勫箚閸儲鈷戞慨鐟版搐閸旀潙霉濠婂簼閭柣娑卞櫍瀹曞崬顫濋崗澶诡亪姊绘担渚劸妞ゆ垵鍟村畷鏇㈡偨缁嬭儻鎽曢梺缁樻⒒閳峰牓寮繝鍕＜婵炴垶锕╅崕鎰版煟閵堝嫮绐旀慨濠呮閹风娀鍨鹃搹顐や簽缂傚倷绶￠崰妤呮偡閵夆晜鍋╅柣鎴ｆ鎯熼梺鎸庢煥婢т粙鎯侀崼銉︹拺闁告稑锕ゆ慨锕€霉濠婂嫮鐭岀紒顔肩墛缁楃喖鍩€椤掆偓椤繑绻濆顒傦紲濠电偛妫欑敮鎺楀储閿涘嫮纾藉〒姘搐濞呮﹢鏌涢妸锕€鈻曠€殿噮鍋婇、姘跺焵椤掑嫮宓侀柟鐑樺殾閺冣偓閹峰懐鎲撮崟顓炵悼婵犵绱曢崑鎴﹀磹閺囩姵宕查柟閭﹀枓閸嬫挸顫濋崡鐐插箣閻庤娲滈弫璇差嚕閹绢喖顫呴柨娑樺楠炲牊淇婇悙顏勨偓鏍礉瑜忓濠囧礈瑜忛惌鍡涙煃瑜滈崜鐔奉潖缂佹ɑ濯寸紒瀣劵閸╃偛鈹戦悙棰濆殝缂佺姵鎹囬悰顕€宕奸妷銉庘晠鏌曟径妯烘灍鐎点倖妞藉铏瑰寲閺囩偛鈷夊銈冨妼閹虫劗鍒掔拠瑁佹椽顢旈崨顏呭缂傚倸鍊烽悞锕傗€﹂崶鈺冧笉闁哄秲鍔嬬换鍡樸亜閹扳晛鐏╅柡鍡稻椤ㄣ儵鎮欓幓鎺撴闁剧粯鐗犻弻娑㈠箛閵婏附鐝旈梺鍝勵儏閸婂灝顫忓ú顏勫窛濠电姴鍟ˇ鈺呮⒑閸涘﹥灏伴柣鈺婂灥濡喖姊洪棃娑辩劸闁稿寒鍣ｅ銊︾鐎ｎ偆鍘介梺褰掑亰閸ㄨ京娑垫ィ鍐╃厽闁靛闄勯妵婵嬫煛鐏炲墽娲撮柟顔规櫅閻ｇ兘宕惰閸氬倹淇婇悙顏勨偓鏍洪埡鍐ㄧ筏闁秆勵殔缁狀垶鏌ｅΟ娆炬⒖闁革富鍘搁崑鎾绘晲鎼粹€茶埅濠电偛鍚嬪鍦崲濠靛棌鏋旈柛顭戝枟閻忓秴顪冮妶鍐ㄥ闁瑰啿姘﹂悘瀣攽閻愬弶顥為柟绋挎憸婢规洘绂掔€ｎ偀鎷婚梺鍓插亞閸犲秶娆㈡潏銊х闁圭偓鍓氶悡濂告煛鐏炵偓绀嬬€规洜鍘ч埞鎴﹀炊閼告妫х紓鍌氬€烽悞锕傘€冮崱娆愭殰闁跨喓濮寸粻鏍ㄤ繆閵堝倸浜惧銈庡亝缁诲牆鐣烽敓鐘冲€烽柟缁樺笧椤撴椽姊婚崒娆掑厡缁绢厼鐖煎顒佺瑹閳ь剙鐣烽幋锕€绠婚柟棰佺劍鐎靛矂姊洪棃娑氬婵☆偅顨堢划顓㈠磼濠婂懐锛滈梺鎼炲妽缁嬪牓宕戦幘璇叉闁靛濡囨禍鏍ㄧ節閻㈤潧孝闁挎洏鍊濋幃褎绻濋崶銊ヤ簵濠电偞鍨崹娲磹閸洘鐓熼柟浼村亰閺夋椽鏌涢妶鍡欐噰闁哄苯绉烽¨渚€鏌涢幘瀛樼殤闁瑰箍鍨藉畷鎺楁倻閸℃ɑ娅旈梻浣瑰缁诲倿藝椤栫偞鐓侀柛銉墯閻撴盯鏌涢妷銏℃珔濞寸姍鍕╀簻闁规崘娅曢幆鍫熴亜椤忓嫬鏆ｅ┑鈥崇埣瀹曞崬鈻庤箛锝嗘缂傚倸鍊峰ù鍥敋瑜斿畷鎰板锤濡ゅ啫绁﹂梺绯曞墲缁嬫垿鏌嬮崶顒佺厽闁哄倸鐏濋。宕囩磼閻樺啿鍝烘慨濠冩そ濡啫鈽夊顒夋毇婵犵數鍋涢惇浼村磹閺囥垹姹查柍鍝勬噺閳锋帒霉閿濆嫯顒熼柣鎺斿亾閵囧嫰骞嬪┑鍥ф灎閻庤娲橀崹鎸庝繆閼搁潧绶為悘鐐垫櫕閵堬箓姊绘担渚敯闁规椿浜炵划濠氬箣閿旇棄鈧嘲鈹戦悩鎻掓殧濞存粍绮撻弻鐔兼倻濡櫣浠撮梺閫炲苯澧柟鑺ョ矌閸掓帗绻濋崶銊︽珖闂佺鏈粙鍫ュ箯濞差亝鈷戠痪顓炴噹娴滃綊鏌涚€ｎ偆娲寸€规洘绻堥獮姗€顢欓悾灞藉箞闂備胶鎳撻悘婵堚偓姘煎櫍閹箖鎮界喊妯轰壕婵炲牆鐏濆▍姗€鏌涚€ｎ亝鍣介柟骞垮灩閳规垹鈧綆浜為崐鐐烘偡濠婂啰绠叉い銊ｅ劦瀹曞爼顢楁担鍙夊缂傚倸鍊烽悞锕傛晪婵犳鍠栭悧濠勬崲濞戙垹绀傞柤娴嬫櫆閸ｄ即鏌﹀Ο鐓庢灈闁诡喗顨婇弫鎰償濠靛洤鏋ゆ繝鐢靛仦閹告悂鈥﹂悜钘夌畺鐎瑰嫭澹嬮弸搴ㄧ叓閸ャ劍鎯勫ù鐘层偢濮婂搫鐣烽崶鈺佺濠电偛寮堕敃銏ょ嵁閸儱惟闁挎柨澧介惁鍫ユ⒑閸涘﹥澶勯柛妯垮亹缁參鏁撻悩鏂ユ嫼闂佽崵鍠愬妯何ｆ繝姘厵闁惧浚鍋撻懓鎸庮殽閻愭彃鏆ｉ柟顔界矒閹崇偤濡疯婵椽姊绘担鐟邦嚋婵炴彃绉瑰畷鎴﹀箻鐎靛摜顔曢梺鍓插亞閸犳挾绮诲Ο鑲╃＜閺夊牄鍔嶅畷宀€鈧娲樼敮鎺楀煝鎼淬劌绠ｆい鎾跺晿濠婂嫪绻嗛柣鎰典簻閳ь剚鐗犲畷褰掓濞磋櫕绋戦埞鎴﹀幢濞嗘劖顔曢柣鐔哥矌婢ф鏁幒妤佺叆闁靛牆妫旂换鍡涙煏閸繂鈧憡绂嶉崜褏纾藉ù锝呮惈鏍￠悗鍏夊亾闁归棿绀佺粻鏍ㄦ叏濡炶浜鹃梺璇″枟閻熲晠鐛幘璇茬闁糕剝锚閻﹂亶姊婚崒娆愮グ鐎规洜鏁诲畷鐗堟償閿濆洨顦繝鐢靛Т濞层倝鎷戦悢琛″亾楠炲灝鍔氶柣妤佺矊椤﹪濡搁埡鍌楁嫼闂佸憡绋戦敃銉т焊閻㈠憡鐓曢柣妯虹－婢ь剙菐閸パ嶅姛闁逞屽墯缁嬫帟鎽繝娈垮灠椤曨厾妲愰幒妤婃晬婵炴垶鐟чˇ銊╂⒑鐠囨煡顎楁い顓炲槻椤繘鎼圭憴鍕瀭闂佸憡娲﹂崑鍕叏閵忋垻纾奸柛鎾楀棙顎楅梺鍛婄懃閸燁偊鎮惧畡鎵虫斀閻庯綆鍋呭▍鍥⒑闁偛鑻晶鎾煃閵夛附顥堢€规洘锕㈤、娆撳床婢诡垰娲﹂悡鏇㈡煃閳轰礁鏋ゆ繛鍫燂耿閺岋綁鎮㈤弶鎴濐潎闂佸搫鐭夌徊楣冨箚閺冨牆顫呴柣妯哄级閻︽捇姊绘担鍛婂暈闁荤喆鍎靛畷顖炲箻椤旀儳绁﹂梺鍛婂姦閸犳寮查幖浣圭厽婵☆垰鎼弳鍗灻瑰鍕煉婵﹨娅ｅ☉鐢稿川椤撴繃鐫忛梻浣侯焾椤戝棝鈥﹂崶顒佸殟閺夊牃鏅滅€氭氨鈧懓澹婇崰妤冣偓闈涚焸濮婃椽妫冨☉姘暫濠碘槅鍋呴〃鍡涘箞閵娾晜鏅濋柛灞剧〒閸橀亶鏌ｆ惔顖滅У濞存粍绮撻、鏃堫敂閸喓鍘搁梺鍛婁緱閸橀箖宕洪敐鍡愪簻闁靛骏绱曢幊鍛瑰鍕€愮€殿喕绮欐俊鎼佹晜閼恒儳甯嗛梻鍌氬€搁崐鐑芥嚄閸洖纾婚柕濞炬櫅绾惧潡鎮楅悽鍛婃珳闁绘帒锕ョ换娑㈠幢濡闉嶉梺绋垮椤ㄥ棛鎹㈠☉銏犲耿婵☆垵顕ч棄宥夋⒑缂佹ɑ灏伴柣鈺婂灦楠炲啫顫滈埀顒勫箖濞嗘挸绠甸柟鍝勬鐎垫牗绻濋悽闈涗粶鐎殿喖鐖奸獮鎰板箮閽樺鎽曞┑鐐村灟閸ㄥ綊鎮″☉銏″€甸柨婵嗛婢т即鏌ｉ敃鈧悧鎾诲箖濡ゅ啯鍠嗛柛鏇ㄥ墰椤︺劑鏌ｉ姀鈺佺仭閻㈩垽绻濋崹楣冩晝閸屾鈺呮煃鏉炴媽鍏屾い锔芥緲椤啴濡堕崱妤€顫囬梺鎼炲妿缁垳绮嬮幒妤婃晣闁靛繆妾ч幏濠氭⒑缁嬫寧婀伴柤褰掔畺閸┾偓妞ゆ巻鍋撻柛銏＄叀濠€渚€姊洪幖鐐插姶闁告搫绠撳顐も偓锝庡枟閻撴稓鈧箍鍎辨鎼佺嵁閺嶎厽鐓曢柕濞垮劚閳ь剚鎮傚﹢渚€姊虹紒妯诲碍婵炲鍏橀獮妤呮偐缂佹鍘遍柟鍏肩暘閸╁嫬鈻撳鍫熺厸閻忕偛澧介妴鎺懨归悪鍛暤鐎规洘绮忛ˇ顔戒繆椤愮喐娅囩紒杈ㄦ尰閹峰懐绮电€ｎ亝顔勬繝纰樻閸嬪懐鎹㈤崼婵愬殨濠电姵纰嶉弲鎻掝熆鐠虹尨鍔熸い鎾存そ濮婃椽骞愭惔锝囩暤濠碘槅鍋掗崢鑹扮亱闂佸憡娲﹂崹閬嶅煕閹寸姷纾藉ù锝堝亗閹寸姳鐒婂ù鐓庣摠閻撶喖鏌嶉崫鍕跺伐闁诲骏濡囬埀顒侇問閸犳盯顢氳椤㈡ɑ绺界粙鍧楀敹濡炪倖鍔х紞鍡涘焵椤掆偓濞差厼顫忕紒妯诲闁告稑锕ら弳鍫ユ⒑鐟欏嫮鎽冩い鏇嗗浄缍栨繝闈涱儏鎯熼悷婊冪Ч瀹?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟版晥闂佹寧绻勯崑娑㈠煘閹寸姭鍋撻敐搴′簼婵炲懎娲铏圭矙鐠恒劎鍔规繝纰樷偓铏窛缂侇喗鐟ㄧ粻娑㈠籍閸屾粎妲囬梻渚€娼ф蹇曞緤娴犲鍋傞柟鎵閻撴洟鏌￠崘锝呬壕闂佺粯顨堟慨鎾偩閻戣棄绠ｉ柨鏇楀亾閸ュ瓨绻濋姀锝嗙【妞ゆ垵娲畷銏ゅ箹娴ｅ厜鎷洪梺纭呭亹閸嬫盯宕濆Δ鍛厸闁告侗鍠氶埥澶愭煟椤垵澧存慨濠勭帛閹峰懘鎼归悷鎵偧闂佹眹鍩勯崹杈╂暜閿熺姴鏋侀柛鎰靛枛鍞悷婊冮閻ｇ兘宕ｆ径宀€顔曢梺鐟扮摠閻熴儵鎮橀埡鍌ゆ闁绘劦浜滈悘顏勄庨崶褝韬い銏＄☉閳规垿宕堕張闈涙憢婵犵數鍋涢顓熸叏閻㈢绠伴柛鎰▕濞兼牗绻涘顔荤盎缂佺姴缍婇弻锝夊箛椤撶偟绁峰銈庡墮椤﹁京妲愰幘瀛樺缂佹稑顑呭▓顓㈡⒑缂佹﹩娈曢柟鍛婃倐椤㈡岸鏁愰崱娆戠槇濠殿喗锕╅崕鐢稿煛閸涱喚鍘撻梺鍛婄箓鐎氼剟鍩€椤掆偓閹诧紕绮嬪鍜佺叆闁割偆鍟块幏娲⒑閸涘﹦鈽夐柨鏇缁骞樼紒妯衡偓鍨叏濮楀棗澧柛銈呮搐閳规垿顢欓悷棰佸闂傚倷绶氬褏鎹㈤崼銉ュ瀭濠电姵纰嶉弲顒佺節婵犲倻澧涢柍閿嬪灴濮婃椽顢曢妶鍛捕闂佸吋妞块崹閬嶅疾閸撲讲鍋撻悽鐢点€婇柛瀣尵閹叉挳宕熼鍌ゆК闁诲氦顫夊ú锕傚礈閻旂厧鏄ラ柕澶涚畱缁剁偛鈹戦悙闈涗壕闁诲骸顭峰娲偡閹殿喗鎲肩紓浣筋嚙閸婅崵鍒掓繝姘€烽柣鎴炃氶幏濠氭⒑缁嬫寧婀伴柣鐔濆懐鐜婚柡鍐ｅ亾缂佺粯绋掗幏鍛村礃閸欏銆楅梺鎼炲劘閸斿矂顢氶柆宥嗙厸濠㈣泛顑呴悘銉╂煟鎼搭喖寮慨濠傛惈鏁堥柛銉戝懍绱曟俊鐐€х徊鑲╁垝濞嗗繒鏆﹂柟杈剧畱缁犲鏌熸０浣哄妽闁稿秶鏁诲铏圭磼濡纰嶉柣搴㈢▓閺呯娀骞冮棃娑掓斀閻庯綆鍋嗛崣鍐ㄢ攽閳藉棗鐏ョ€规洜鏁哥划濠氭倷閻戞鍘搁柣蹇曞仧閸嬫挾绮堥崘顏佸亾閸偅绶查悗姘緲閻ｇ兘宕￠悙宥囧枛閹筹繝濡堕崨顖溕戦梻鍌氬€搁崐鐑芥倿閿旂晫绠惧┑鐘叉搐缂佲晠姊洪鈧粔鎾垂閸岀偞鐓曠憸搴ㄣ€冮崨瀛樺珔闁绘柨鎽滅粻楣冩煙鐎电鈧垵顫濈捄铏瑰摋婵炲濮撮鍡涙偂閺囥垺鐓冮柍杞扮閺嬨倖绻涢崼娑樼仼闂囧鎮樿箛鏃傚ⅹ濞存粍鍎抽埞鎴︽偐閸偅姣勬繝娈垮櫘閸欏啴鐛箛娑樼妞ゆ棁鍋愰崐鐐烘⒑閹稿孩顥嗘俊顐㈠瀵悂濡舵径瀣ф嫽婵炶揪绲介幉锟犲箚閸儲鐓涢柛顐亜婢ф挳鏌熼姘伃妞ゃ垺鐩幃娆撴濞戞哎鍋婂┑鐘殿暯濡插懘宕规导瀛樺剭闁绘垼妫勭壕濠氭煙閸撗呭笡妞ゃ儱鐗婄换娑㈠箣閻愬灚鍣ч梺鍝勬閻楁挸顫忓ú顏咁棃闁冲搫鍊甸崑鎾诲Ψ閳轰胶鍊為梺闈涱焾閸ㄨ櫣鈧?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇牠姊绘笟鈧埀顒傚仜閼活垱鏅堕弶娆剧唵閻熸瑥瀚粈瀣煙椤旀儳鍘存鐐诧攻缁绘繈宕掑鍛呫劑姊虹拠鈥虫灀闁哄懏鐩、娆掔疀濞戣鲸鏅╅梺缁樻尭妤犳瓕鐏囩紓鍌氬€搁崐鎼佸磹閹间礁纾归柛婵勫劤閻捇鏌ｉ姀鐘冲暈闁稿顑夐弻锟犲炊閿濆棗娅氶梺閫炲苯澧剧紒鐘虫尭閻ｉ绮欑拠鐐⒐閹峰懘宕ㄦ繝鍕緭闂傚倸鍊搁崐鎼佸磹閹间焦鍋嬪┑鐘插閻瑩鏌熸潏鎹愵吅缂傚秵鐗犻弻娑㈠Ψ椤旂厧顫梺?闂?
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
        // up 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婄拋鏌ュ几濞嗘挻鈷戠紓浣姑粭鈺佲攽椤斿搫鈧骞戦姀鐘闁靛繒濮撮懓鍨攽閳藉棗鐏ユい鏇嗗懎鏋堢€广儱顦伴悡鐔兼煟閺傛寧鎲搁柟铏礈缁辨帡鎮╅搹顐㈢３濡ょ姷鍋涢崯顐ョ亙闂佸憡渚楅崰妤€鈻嶅鍫熺厵闁兼祴鏅炶棢闂佸憡鎸荤换鍫ュ箖濡警鍚嬪璺侯儌閹锋椽姊虹涵鍛汗闁稿鐩崺鈧い鎺嶇劍缁€瀣煙瀹曞洤鏋涙い銏＄☉閳规垿宕煎鍛板厭闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噺瀹曟煡鏌涘畝鈧崑娑氱不閻樿绠规繛锝庡墮婵″吋鎱ㄩ悷鏉跨骇缂佺粯鐩畷鍗炍熺拠鏌ョ€洪梻浣虹帛閹歌煤閻旂厧绠栨慨妞诲亾闁诡喗鐟﹂敍鎰攽閸℃﹩鍞叉繝鐢靛Х椤ｎ喚妲愰弴銏犵柧妞ゆ劑鍊栭～鏇㈡煙閻戞ɑ鈷掔痪鎯у悑娣囧﹪顢涘顒佸€梺鍛婂笚缁嬫垼鐏冮梺缁橈耿濞佳勭濠婂嫨浜滈柟瀛樼箖閸ゅ洭鏌涢埞鍨仾闁诡垱妫冩俊鍫曞炊閳哄倹绶梻鍌欑閹碱偄煤閿旀儳鏋堢€广儱顦壕濠氭煕濞戝崬鐏ｇ痪?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐烘偡濠婂啰绠荤€殿喗濞婇弫鍐磼濞戞艾骞堟俊鐐€ら崢浠嬪垂閸偆顩叉繝闈涱儐閻撴洘绻涢崱妤冪缂佺姵濞婇弻宥堫檨闁告挻鐟х划璇差吋婢跺﹦锛熼梻渚囧墮閸楁洟宕堕澶嬫櫖闂佺粯鍔栧娆撳焵椤掆偓閸㈡煡鍩為幋锔藉亹闁告瑥顦ˇ鈺呮⒑缂佹ɑ灏紒缁樺姍閸╃偤骞嬮敂钘変汗闂佽偐鈷堥崜娑㈩敊閹烘垟鏀介柨娑樺濡炬悂鏌涢悩鎰佹畼闁瑰箍鍨归埥澶愬閻樿鏁规繝鐢靛█濞佳兠洪妶澶婃辈闁割偁鍨荤壕钘壝归敐鍫燁仩閻㈩垱绋撶槐鎺旀嫚閼碱剙鈪靛Δ鐘靛仜閸燁垳绮嬮幒鏂哄亾閿濆骸浜芥俊顐㈠暙閳规垿鎮欏▓鎸庮棑闂佺硶鏅滈悧鐘诲箚閸曨垼鏁嶉柣鎰蔼閹芥洖鈹戦悙鏉戠仸闁糕晛鍟村畷鎴﹀箻缂佹ɑ娅滈柟鑲╄ˉ閳ь剝灏欓崙褰掓煟鎼淬値娼愭繛鍙夌墵閹矂宕掗悙鑼舵憰闂佹寧绋戠€氼亜鈻介鍫熺厱闁圭偓顨呯€氼剟宕归崡鐐╂斀闁绘ɑ顔栭弳婊呯磼鏉堛劍绀嬬€规洘顨呴～婊堝焵椤掑嫬违濞达絿纭堕弸搴ㄦ煙閻愵剚缍戝ù婊勵殜閺岀喖宕楅崗鐓庡壒闂佸摜濮甸〃鍡欑矉瀹ュ围闁搞儮鏅濋敍婵嬫⒑缁嬫寧婀伴柤褰掔畺閸┾偓妞ゆ帒瀚峰Λ鎴︽煙瀹勯偊鍎旂€殿喖顭锋俊鐑藉Ψ瑜濈槐閬嶆⒒娴ｅ憡璐￠柛瀣崌瀹曟粌顫濋懜闈涗簻闂佺硶鍓濈粙鎺楁偂閻樼粯鐓欓梻鍌氼嚟閸斿秵绻涢幊宄板暊閺€鑺ャ亜閺冨洤浜瑰褝绠撻弻鈥崇暆鐎ｎ剛袦濡ょ姷鍋為敃銏犵暦濮椻偓瀹曪絾寰勭€ｉ潧鏅梻鍌氬€风粈渚€骞栭锕€瀚夋い鎺嗗亾閻撱倖淇婇娆掝劅闁搞倖顨婇弻娑㈠即閵娿儳浠╅梻浣稿船濞诧妇鎹㈠┑瀣棃婵炴垶鐟Λ鐐差渻閵堝骸浜濋柟鐟版搐椤繒绱掑Ο璇差€撻梺鍛婄☉閿曘儵宕曢幘鏂ユ斀闁宠棄妫楁禍婵堢磼鐠囨彃鏆熺紒顔碱儏椤撳吋寰勬繝鍥ф暪闂備礁鎼ú銊╁磻閻樿绠洪柡鍥ュ灪閳锋垿鎮归幁鎺戝闁哄鏌ㄩ埞鎴︻敊閼恒儳绋囧銈嗘煥缁绘劙鍩為幋锕€鐐婇柕澶堝€楅惄搴ㄦ⒒娴ｅ憡鎯堥柛鐕佸亰瀹曟劙宕ㄦ繝鍕垫锤婵°倧绲介崯顖炴偂閺囥垺鐓ユ繝闈涙椤忊晝鈧鎸稿Λ娆撳箞閵婏妇绡€闁告劏鏂傛禒銏ゆ倵鐟欏嫭澶勯柛瀣工閻ｇ兘鎮㈢喊杈ㄦ櫌婵炶揪绲块幊鎾活敂閹殿喚纾介柛灞捐壘閳ь剟顥撶划鍫熺瑹閳ь剟銆侀弽顓炲耿婵炴垶顭囬澶愭⒑閹肩偛鍔撮柛鎾寸懇瀵即濡烽埡鍌滃帗閻熸粍绮撳畷婊冾潩鐠轰綍锕傛煕閺囥劌鐏犵紒鐘冲▕閺岀喖骞栭鐐典哗婵犳鍠楃粙鎾舵閹惧瓨濯撮柛鎾冲级妤旈梻浣芥〃缁€渚€鈥﹀畡閭﹀殨閻犲洦绁村Σ鍫熸叏濮楀牏鍒板ù婊堢畺閺屻劌鈹戦崱娆忓毈闂佺灏欓崑鎾寸┍婵犲洤绠甸柟鐑樻崄绾偓缂傚倷娴囨ご鍝ユ暜閳ユ剚鍤曟い鏇楀亾鐎规洘甯掗～婵嬵敇閻橆偄浜惧┑鐘崇閻擄絽霉閸忚偐鎳呴柛鎺嶅嵆閺屽秶鎲撮崟顐や紝閻庤娲樼敮鎺楀煘閹达箑鐐婇柤鍛婎問濡嫮绱撻崒姘偓鎼佸磹閸濄儳鐭撻柟缁㈠枟閺呮繃銇勮箛鎾跺ⅱ鐎规挷绶氶弻娑㈠箛闂堟稒鐏堥梺缁樻尰閻╊垶寮诲☉妯锋闁告鍋為悘鍫濃攽閻愯尙澧戦柛濠冩礋閳ユ棃宕橀鍢壯囨煕閳╁喚娈橀柣鐔稿姍濮婅櫣绱掑Ο娲殝缂備胶濮甸幐鎼佹偩闁垮顕遍柡澶嬪灥閸炪劌顪冮妶鍡樺暗闁哥喎娼￠獮蹇曠磼濡偐顔曢柡澶婄墕婢т粙骞冩總鍛婄厽闁斥晛鍟ˉ鍫⑩偓瑙勬礈閺佽鐣峰鈧、娆撴偩鐏炶棄绠為梻鍌欑窔濞佳団€﹂崼銉ョ？闂侇剙绉甸崵瀣喐閻楀牆绗氶柟铏哺閺岋綁骞囬鐓庡闂佺粯鎸诲ú鐔煎蓟閻旂顕遍悗娑櫳戦柨顓㈡⒑閸濆嫷鍎愰柣鈺婂灦瀵鏁冮崒姘鳖槰濡炪倕绻愮€氼參鎮橀崼銉︹拺闁告繂瀚﹢鐗堢箾閸欏鐒介柟骞垮灩閳藉顭ㄦ惔鈶裤儵姊绘担渚劸妞ゆ垵妫濋獮鎴﹀炊椤掆偓閽冪喐绻涢幋鐐茬劰闁稿鎹囬弫鎰償閳ヨ尙鍑归柣搴ゎ潐濞叉牠鎯岄崒鐐茶摕闁斥晛鍟刊鎾煟閻斿憡绶叉い顐ｅ浮閹鎲撮崟顒傦紭闂佸憡姊归崹鍧楀Υ娴ｇ硶妲堟俊顖炴敱閻庡姊虹涵鍛涧闁奸潧缍婂銊ッ洪鍛嫽闂佺鏈懝楣冨焵椤掑倸鍘撮柟顔惧仱閺佸倿鎮惧畝鈧鏇熺節閻㈤潧孝婵炲眰鍊楃槐鎺楀煛閸涱喚鍘卞┑掳鍊曢崯鈺冩閾忣偆绠剧€瑰壊鍠曠花濂告煕鐎ｎ亜顏╅柍瑙勫灴瀹曟帒顫濋崗澶寡呯磽閸屾氨孝濡ょ姵鎮傞崺鐐哄箣閿旇棄鈧兘鏌涘▎蹇ｆТ闁哄鐟︾换娑氣偓娑欘焽閻绱掗鑲┬ら柍褜鍓熷褔濡堕幖浣哥畺闁冲搫鍟扮壕鍏间繆椤栨粌甯舵鐐茬墦濮婄粯鎷呴崨濠冨創濠电偛鐪伴崹钘夌暦濠靛牅娌悷娆欑稻閻庡搫鈹戦埥鍡楃仴鐎规洜鏁哥划缁樸偅閸愨晝鍘介梺瑙勫礃濞呮洟骞戦敐澶嬬厱闁冲搫顑囩粙濠氭煙娓氬灝濡界紒缁樼箞瀹曘劑顢氶崨顒€鎽嬬紓鍌氬€风欢锟犲窗濡ゅ懏鍋￠柍鍝勬噽瀹撲線鏌熼柇锕€骞楅柛搴ｅ枛閺屽秹鍩℃担鍛婃闂佸搫妫欓敃銏狀潖閾忚瀚氶柛娆忣槸閺€顓烆渻閵堝骸浜滄い锔诲灣閸欏懘姊虹紒妯荤叆妞ゃ劌閰ｉ幃鐐烘偩鐏炴儳鏋戦悗骞垮劚閹虫劙寮抽妶澶嬬厱闁哄洢鍔岄悘锟犳煃闁垮鐏╅柟鍙夋倐閹囧醇濠靛牏鎳嗗┑鐐茬摠缁牓宕￠幎钘夎摕婵炴垶绮庨悿鈧梺鍝勫暞閹逛線鍩€椤掑啯纭堕柍褜鍓氶鏍窗閹捐绀夌€广儱娲ㄩ惌澶愭煙閻戞ɑ鐓涢柛瀣崌閺佹劖鎯旈垾鑼晼闂備礁鎲￠敋鐎规洦鍓熼垾鏃堝礃椤斿槈褔鏌涢埄鍐炬當妞ゎ偄瀛╃换婵嗏枔閸喗鐏嶉梺鐟版啞婵炲﹪鐛崘顭戞建闁逞屽墴瀹曟椽鏁撻悩鑼槰闂佹寧绻傞幊宥嗙珶閺囥垺鐓熼柣鏂挎憸閹冲啴鎮楀鐓庡箻濠㈣娲熼獮鏍ㄦ媴閸︻厼骞堥梺鐟板悑閻ｎ亪宕濈€ｎ喖绠熷Δ锝呭暞閻撳啴鏌曟径娑㈡妞ゃ儱鐗忛埀顒冾潐濞叉﹢鏁冮姀銈冣偓浣糕枎閹惧啿绨ユ繝銏ｎ嚃閸ㄦ澘煤閿曞倹鍋傞柡鍥ュ灪閻撳啴鏌嶆潪鎵槮闁哄棛鍠栭弻鐔煎礃閹绘帗娈查梺闈涙搐鐎氫即銆侀弴鐔稿劅闁斥晛鍟ˇ鈺呮⒑閹惰姤鏁遍柛銊ユ健瀵濡堕崥銈呮贡閳ь剨缍嗛崑鍛存偟閺囥垺鈷戦柟鑲╁仜閳ь剚鍔欏畷鎴﹀箻閺傘儲鏂€闂佺粯锕╅崑鍛垔娴煎瓨鐓曟繛鎴濆船閺嬬喖鏌ｅ┑鎰盎闁宠鍨块崹楣冨礃閼碱剟鐛撻梻浣告啞濮婂綊鎮烽埡鍛祦闁告劑鍔夐弸搴ㄦ煙閹冨笭缂佸崬鐖煎娲川婵犲啫顦╅梺鍛婄懃闁帮絽鐣烽婧惧亾閿濆骸鏋熼柍閿嬪笒闇夐柨婵嗘噺閸熺偤鏌熼姘卞ⅵ闁哄本绋掔换婵嬪磼濠婂啰鍘介柣搴ゎ潐濞叉ê煤閻斿鍤曞┑鐘宠壘鍞銈嗙墬缁酣鍩ｉ妶澶嬬厽閹艰揪绱曢悾顓㈡煕鎼淬劋鎲鹃挊婵喢归崗鍏肩稇闁绘挻娲熼弻鐔告綇妤ｅ啯顎嶉梺绋款儌閸撴繄鎹㈠☉銏犲耿婵°倕鍟伴鍥ь渻閵堝懐绠氶柡鍛█瀵鈽夐姀鐘愁棟闁荤姴娲︾粊鎾磻閹炬緞鏃堝川椤旂厧澧绘俊鐐€曠换鎰涘☉姘棜濠靛倸鎲￠悡蹇涚叓閸ャ劍绀€鐎涙繄绱撴担鍝勑ｆ俊鐐舵椤繘鎼归崷顓狅紲濠殿喗顨呭Λ娆撴偩閸洘鐓熼幖娣灮閳绘捇鏌￠崨顔剧疄妤犵偞鍨挎慨鈧柣姗嗗亝閺傗偓闂備礁澹婇崑鍛崲閸愵喗鍊甸柛鎰ゴ閺€浠嬫煃閽樺顥滈柣蹇曞█閺岀喓鍠婇崡鐐板枈闂佺硶鏅滅粙鎺戭嚗閸曨垰绠涙い鎾跺仒閻㈢粯绻濋悽闈浶㈤柨鏇樺€楅埀顒佸嚬閸樻儳鈻庨姀銈呰摕闁靛濡囬崢閬嶆⒑缂佹ɑ鐓ラ柟璇х磿娴滄悂鏁冮崒娑氬幗濠电偞鍨靛畷顒勫几濞戙垺鎳氶柡宥庡幗閻撳啰鎲稿鍫濈闁绘梻鍘ч悘鎶芥煥閺囩偛鈧憡鍎梻浣哥枃濡椼劎绮堟担铏圭幓婵炴垶锕╁〒濠氭煏閸繃顥為悘蹇曟暬閺屾稓鈧綆鍓欓弸鎴犵磼閸屾氨校閻庝絻鍋愰埀顒佺⊕鑿ら柟椋庣帛缁绘稒娼忛崜褎鍋у銈庡幖閻楁挸顕ｉ幎鑺ョ劶鐎广儱妫涢崢鐢电磼閻愵剚绶茬€规洦鍓氱粋宥夊箥椤旂懓浜鹃悷娆忓婢跺嫰鏌涢妸銉у煟妤犵偛鍟撮崺锟犲礃閳轰胶褰撮梻浣藉亹閳峰牓宕滈妸褎顫曢梺顒€绉甸埛鎺懨归敐鍥у妺闁搞倐鍋撳┑鐘媰閸涱喗鐝梺鍛婂笚鐢繝銆佸☉銏″€烽柤纰卞墯閻擄綁姊绘担鍛婃儓閻炴凹鍋婂畷鏇熸綇閳哄啩绗夐悷婊呭鐢宕愰崹顐ょ闁瑰鍋熼幊鎰版煟閹烘梹娅曠紒杈ㄥ浮閹晝绮电涵鍛亾閸ф鐓涚€光偓鐎ｎ剛袦闂佺硶鏂侀崜婵堢箔閻旂厧鐒垫い鎺戝閻鏌涢弴銊ュ闁告瑥绻愰埞鎴︽偐閸欏娅ゅ銈呯箳婵妲愰幒鎾寸秶闁靛鍎抽悷銊╂煣娴兼瑧绉柟顔肩秺瀹曞爼濡歌瀵劑姊虹拠鍙夊攭婵炲拑绲垮Σ鎰板箻鐠囪尙锛滃┑鐐叉閸旀濡堕弶娆炬富闁靛牆妫欓懖鐘绘煕閵夛絽濡煎ù婊堜憾濮婃椽宕烽鐐板闂佸憡鎸诲畝绋跨暦閻㈢鍗抽柕蹇娾偓鍏呭寲闂備焦鎮堕崕顖炲礉鎼淬劌围閻庢稒眉缁诲棝鏌熺紒妯虹濠⒀勭〒缁辨帞绱掑Ο鑲╃暫缂備胶绮换鍫濈暦閹烘垟妲堟俊顖涙た娴犻亶姊婚崒娆戭槮闁圭⒈鍋婅棟濞村吋娼欑粻鏍煕瀹€鈧崑娑㈡嫅閻斿吋鐓ユ繝闈涙椤ョ偤鏌涙惔锝呮瀾闁靛洤瀚板浠嬪Ω瑜忛悡鍌炴煛瀹ヤ讲鍋撻弬銉︽杸闂佺粯鍔樼亸娆撳箺閻樼數纾兼い鏃囧亹鑲栭梺鍛婂笚鐢繝鐛Ο鑲╃＜婵☆垳绮悵鎶芥⒒娴ｈ櫣甯涢柛鏃€娲熼獮鏍敃閿旇棄鈧嘲鈹戦悩鎻掓殧濞存粍绮撻弻鐔煎传閸曨厜銉︺亜閺傛妯€闁哄瞼鍠栭幃鍧楊敍濞戝彉鍝楃紓鍌欐祰妞存悂骞愭繝姘闁告稒娼欑粈鍫ユ煙缂佹ê绗掑鐟扮墛缁绘繈鎮介棃娑楁埛闂侀潧妫楅鍥箲閵忕姭鏀介悗锝冨妷閸嬫挻绗熼埀顒勭嵁濡警鐔嗛柛鏇ㄥ墯閳锋劙鏌熷畡鐗堝殗鐎规洘顨婂畷顐﹀Ψ瑜庨弲銊╂⒒閸屾艾鈧绮堟笟鈧獮鏍敃閳锋碍妞芥慨鈧柕鍫濇噽椤︻噣鎮楅崗澶婁壕闂佸憡娲﹂崢鑲╃矈閿曗偓閳规垶骞婇柛濠冩礋瀹曨垶骞嶉纰辨婵犻潧鍊搁幉锟犳偂閺囥垺鍊堕柣鎰版涧娴滈箖鏌ｉ妸锕€鐏寸€殿噮鍋婂畷濂稿Ψ閿曗偓閳ь剛鏁婚幃宄扳枎韫囨搩浠剧紒鍓у亾閹倿寮诲☉銏犳閻犳亽鍓遍妶澶嬬厵妤犵偛鐏濋悘鈺呮煃鐟欏嫬鐏╅柍褜鍓ㄧ紞鍡樼閻愭牳鍥Ω閵夘喗瀵岄梺闈涚墕閹虫劗绮绘导瀛樼厸閻忕偞鏋婚煬顒€鈹戦檱閸╂牕顕ラ崟顓涘亾閿涘崬瀚鐑樼節閻㈤潧浠╅柟娲讳簽瀵板﹥绂掔€ｎ亞顔戝┑鐘诧工閻楀棝宕欓悩缁樼厱闁硅埇鍔嶅▍鍐磼鐠囧弶顥炲ǎ鍥э躬閹瑩妫冨☉妯圭帛濠电姴鐥夐妶鍡欍€婄紓浣介哺鐢繝銆佸▎鎾村殐闁宠桨绶￠弶鎼佹⒒娴ｇ儤鍤€闁搞垺鐓￠獮濠偽熼搹瑙勬濡炪倖鍔х粻鎴犵矆鐎ｎ喗鐓曟い顓熷灥娴滆姤淇婇鐘插姦婵﹤顭峰畷鎺戔枎閹烘垵甯俊鐐€ら崑鍕敄婢跺﹦鏆﹂柣鏃傚帶閸ㄥ倹銇勯幇鈺佲偓鏍р枔妤ｅ啯鈷掗柛灞剧懅閸斿秴鈹戦悙璇ц含鐎殿喓鍔戦弻鍡楊吋閸涱厾鈧參姊洪崜鑼帥闁革綆鍠栭…鍥冀瑜夐弨浠嬫煟濡櫣鏋冨瑙勶耿濮婃椽宕￠悙鏉戞灎闂佸搫鐭夌紞浣割嚕椤曗偓瀹曞爼濡烽姀鐘卞闂侀€炲苯澧柍瑙勫灴椤㈡瑩宕楅崗鍏煎€锋俊鐐€ゆ禍婊堝疮鐎涙ü绻嗛柛顐ｆ礀缁€鍐煏婵炲灝鍔氭い顐ｆ皑缁辨捇宕掑▎鎴ｇ獥闂佸摜濮甸〃濠囩嵁閹达箑鐐婄憸婊堝吹閺囥垺鍋ｉ柛銉ｅ妿閵堝瓨淇婇幓鎺斿缂佺粯鐩畷鍗炍熼搹閫涙偅闂備椒绱粻鎴︽偋婵犲嫭宕叉繛鎴炵懄缂嶅洭鏌涢幘妤€鎲涘顑芥斀闁绘劕寮剁€氬懐绱撳鍕獢妤犵偛鍟撮弫鎾绘偐閼碱剦鍟嬬紓鍌氬€烽悞锕佸綔闂佸綊鏀卞浠嬪蓟閿濆鍋愰柛娆忣槺椤﹂亶姊洪柅鐐茶嫰婢у弶绻涢崨顔界闁诡喗锕㈠畷鐓庘攽閸愨晜鏉搁梻浣虹帛閸旀浜稿▎寰帡鎮欓悜妯煎帗閻熸粍绮撳畷婊冣枎閹存繍妫滃銈嗘尪閸ㄧ懓效閺屻儲鐓ラ柡鍥╁仜閳ь剙缍婇幃锟犲即閻斿墎绠氶梺闈涚墕閹冲酣寮冲▎寰濈懓顭ㄩ崘顏喰ㄩ梺鍝勭焿缁辨洟鍩€椤掑﹦绉靛ù婊庝邯瀹曪綁宕熼娑氬幐閻庡厜鍋撻柍褜鍓熷畷浼村冀椤撶偠鎽曢梺鎼炲労閸撴岸寮插┑瀣厓鐟滄粓宕滃璺虹柧闁割偅娲栫粻銉︺亜閺冨洦顥夐柡瀣灴濮婅櫣绱掑鍡樼暥闂佺粯顨呴敃锔界閹间焦鏅滈柤鎭掑劚瀵寧绻濋悽闈浶㈤悗姘煎枤閺侇喖鈽夊杈╋紲濠德板€曢崯顐﹀几濞戙垺鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍ь啅婵犳艾纾婚柟鐐暘娴滄粍銇勯幘璺轰沪缂佸本瀵ч妵鍕晜閼恒儱鍞夐梺鍝勭灱閸犳牠鎮伴鈧畷褰掝敊閸欍儳绀勯梺璇叉唉椤煤閺嵮呮殾妞ゆ帒瀚悡鈥愁熆鐠哄ソ锟犳偄閸濆嫭鐎抽柡澶婄墑閸撻亶濡堕崨顏呮杸闂佺粯锚閻忔岸寮抽埡浣叉斀闁绘劏鏅涙禍鐐節閻㈤潧浠掗柛鏍█瀹曟鎮╅搹顐犱虎婵犵鍓濋幃鍌炲春閻愬瓨鍎熼柟鎯у帠婢规洟姊洪崨濠勨槈闁宦板姂閹繝濡烽埡鍌氣偓鐢告煥濠靛棗鈧懓鈻嶉崶鈺冪＜闁绘ê鐤囨竟妯汇亜椤忓嫬鏆ｅ┑鈥崇埣瀹曞崬鈻庤箛锝嗘闂傚倷娴囧畷鐢稿窗閸℃稑绀夐柟杈捐礋閳ь兛绀佽灃闁告侗鍠栨禍婊堟⒑缁嬭法绠洪柛瀣姍瀹?30 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃闁硅櫕鎹囬垾鏃堝礃椤忎礁浜鹃柨婵嗙凹缁ㄥジ鏌熼惂鍝ョМ闁哄矉缍侀、姗€鎮欓幖顓燁棧闂備線娼уΛ娆戞暜閹烘缍栨繝闈涱儐閺呮煡鏌涘☉鍗炲妞ゃ儲宀稿缁樻媴閽樺－鎾绘嫅鏉堚晝纾奸悹鍥皺婢э妇鈧鍠栭…鐑藉箖閵忣澀鐒婂ù锝呮贡娴滄牠姊绘担绋款棌闁绘挸鐗撳畷鏉课旈崨顔尖偓鍓佲偓骞垮劚椤︿即鍩涢幋锔界厱婵犻潧妫楅顏呫亜閵壯冧户缂佽鲸甯￠、娆撴嚃閳诡儸鍐ｆ斀闁挎稑瀚弳顒傗偓瑙勬礈閸犳牠銆佸▎鎾崇鐟滃骸顭块幋锔解拻濞达絽鎲＄拹锟犳煕鐎ｎ偅宕屾鐐叉娴狅附娼幐搴ｆ创闁搞劑绠栭獮鍥ㄦ媴閻熸壆妲?
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帡宕濋幘顔解拺閺夌偞澹嗛ˇ锔姐亜閹存繃顥㈠┑锛勬暬瀹曠喖顢涘槌栧晪闂佽崵濮惧▍锝夊磿閵堝鍊靛Δ锝呭暞閳锋垿鏌涘☉姗堝姛闁瑰啿鍟扮槐鎺旂磼濮楀牐鈧寧顨ラ悙鎻掓殭閾绘牠鏌涘☉鍗炴珮婵☆偄鍟埞鎴︽倷閺夋垹浠搁梺鑽ゅ櫐缁犳挸鐣锋导鏉戠疀闁绘鐗冮幏鍝勨攽閻愯泛钄兼い鏇嗗洨宓佹俊銈呮噺閻撴瑩鏌熼梻纾嬪厡闁伙絾妞介弻娑㈠煘閹傚濠碉紕鍋戦崐鏍暜閹烘绐楁慨姗嗗墻閻掍粙鏌熼柇锕€骞樼紒鐘荤畺閺屾稑鈻庤箛锝喰ㄦ繝鈷€灞奸偗闁诡噯绻濇俊鑸靛緞鐎ｎ剙甯惧┑鐘垫暩閸婎垶宕橀埡浣诡仧缂傚倷鐒﹂〃鍛此囬棃娑辨綎婵炲樊浜滄导鐘绘煕閺囥劌浜愰柛瀣崄閵囨劙骞掑┑鍥ㄦ珗婵犵數鍋涘Λ娆撳箰婵犳艾纾婚柛鏇ㄥ灡閻撴洟鏌嶉埡浣告殶闁愁垱娲熼弻娑氣偓锝呭缁夘噣鏌＄仦鍓р槈闁宠鍨垮畷鍗炍旀繝浣烘／闂傚倷娴囬鏍垂娴煎瓨鍋嬪┑鐘叉搐妗呴梺鍛婃处閸ㄧ増鍎梻浣瑰濮婂宕戦幘璇查棷濞寸厧鐡ㄩ埛鎴︽煕濠靛棗顏柣鎺曟硶缁辨挸顓奸崟顓犵崲闂佺粯渚楅崳锝夌嵁閹烘嚦鏃堝焵椤掑倻涓嶉柨婵嗘娴滄粓鏌″鍐ㄥ缂佺姵鎹囬弻鐔碱敍濮樺崬鈪甸梺鍝勭焿缁查箖骞嗛弮鍫濐潊闁宠桨鐒﹀▍娑㈡⒒娴ｅ憡鎲稿┑顔炬暬閹囨偐鐠囪尙鐣洪梺闈涱槴閺呮粓宕戦崒鐐茬閺夊牆澧界粙缁樼箾閸儳鐣烘慨濠呮缁辨帒螣鐠囪尙顣查梺璇查閻忔氨鏁幒妞烩偓锕傚炊椤掆偓閻撴盯鏌涚仦涔呰偐鑺辨繝姘拺閻熸瑥瀚粈鍐╃箾婢跺娲寸€殿喗鎮傚顕€宕掑☉妯荤€鹃梻浣告惈椤︽壆鈧瑳鍥у惞闁搞儺鍓氶悡娑氣偓鍏夊亾閻庯綆鍓涜摫闂備浇顕栭崹鍗炍涢崘鈺€绻嗛柤绋跨仛閸庣喖鏌曡箛瀣伄閸熷憡绻濋悽闈浶ラ柡浣规倐瀹曟垵鈽夊鍡楁闂佸壊鍋呭ú姗€寮查鍫熲拺缂備焦蓱鐏忣亪鏌涙繝鍐疄鐎殿喖顭烽弫鎰緞婵犲嫮娼夐梻浣侯焾閺堫剟鎳濋悙顒傤浄婵☆垵鍋愮壕钘壝归敐鍛儓閺嶏繝姊虹紒姗嗘畷妞ゃ劌妫涚划姘綇閵娧呯槇闂佹悶鍎撮崺鏍疾椤掆偓閳规垿鎮╃紒妯婚敪濠电偛鐪伴崝鎴炰繆鐎涙绡€闁搞儯鍔夐幏鍝勨攽椤旂偓鍤€婵炲眰鍊濋崺鈧い鎺嶇婵秶鈧娲橀崹鍧楃嵁濡偐纾兼俊顖炴敱鐎氳棄鈹戦悙鑸靛涧缂佹彃娼￠幃娲棘濞嗗墽鍔烽梺瑙勫婢ф鎮￠弴銏犵婵烇綆鍓欓悘鈺呮煃缂佹ɑ绀嬮柡宀€鍠栭、娆撳礂閻撳簼娣柣搴ゎ潐濞叉牠鎮ユ總鎼炩偓浣肝旈崨顓犲姦濡炪倖甯掔€氼剟宕ｆ繝鍥ㄧ厪濠㈣泛妫欏▍鍡涙煟閹捐泛鏋涢柡宀€鍠栭弻鍥晝閳ь剟寮稿☉銏＄厱闁哄倹顑欓崕鏃堟煛鐏炵晫效闁哄被鍔庨埀顒婄秵娴滅偞瀵煎畝鍕拺闁告繂瀚﹢鎵磼鐎ｎ偄鐏撮柛鈺冨仱楠炲鏁冮埀顒傜矆鐎ｎ偁浜滈柟鎵虫櫅閻忊晜銇勯锝嗗磳婵﹦绮幏鍛村川婵犲啫鍓垫繝纰樻閸嬪懐鎹㈤崘顏嗙焿鐎广儱顦柋鍥煛閸モ晛浠╅柟鑺ユ礀閳规垿鎮欓弶鎴犱桓闂佺硶鏅涢崯鏉戠暦閹达箑绠荤紓浣股戝▍婊堟煙閼测晞藟闁逞屽墲鐏忔瑩宕欒ぐ鎺撶厽閹兼番鍊ゅ鎰箾閸欏澧甸柟顖氱焸瀹曟帡鎮欓懠鑸垫啺闂備胶鍋ㄩ崕杈╁椤撱垹姹查柛鈩冪⊕閸婄敻鏌ㄥ┑鍡涱€楀褎澹嗛幃顕€鏁冮崒娑掓嫽婵炶揪绲块悺鏃堝吹閸愵喗鐓曢柣妯哄暱濞搭喚鈧鍠楅悡锟犲箖閳哄懎鐭楅柛灞剧〒濞插鈧娲橀敃銏′繆閹间礁顫呴柍钘夋缂嶅苯鈹戦悩娈挎毌婵℃彃鎳樺畷褰掝敍濠婂嫬顎忛梺鎸庢礀閸婂綊宕愰悽鍛娾拺妞ゆ巻鍋撶紒澶屾暬閸╂盯骞掑Δ浣哄幈闁诲繒鍋涙晶浠嬪箠閸涘瓨鐓曢柡鍌濇硶鏁堥梺鍝勭灱閸犳牠銆佸▎鎾村殐闁宠桨鑳堕崢浠嬫煟鎼淬値娼愭繛鍙夘焽閹广垽宕掗悙鏉戜患濠电娀娼ч鍛存倿閸偁浜滈柟鍝勭Х閸忓矂鏌ｅ┑鍡欏煟闁哄矉绻濆畷鍫曞Ψ閵壯冨Ф闂備浇妗ㄧ粈浣该洪銏犺摕闁挎繂鎲橀悢鐑樺珰闁肩⒈鍓涢妶鐑芥⒒娴ｇ瓔鍤欑紒缁樺姍閺佸啴顢旈崟鍨稁濠电偛妯婃禍婊呯矆閸愵喗鐓曟繛鍡楁禋濡插綊鏌涙惔銏″磳婵﹥妞介幃鐑藉级閹稿孩鐦ｆ繝鐢靛仜椤曨參宕楀鈧獮鍡涘焵椤掍降浜滈煫鍥ㄦ尰缁佲晜淇婇姘伃婵﹥妞藉畷顐﹀礋椤撶姳绱橀梻浣告憸婵敻骞戦崶顒€鏄ラ柍褜鍓氶妵鍕箳瀹ュ牆鍘￠梺鎰佸灡濞茬喖骞忛幋锔界劶鐎广儱妫涢崢鎾绘⒒娴ｅ摜浠㈡い鎴濇噺閹便劌顓兼径瀣幐?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栨繝寰锋澘鈧呭緤娴犲鐤い鏍剱閺佷胶鈧箍鍎遍ˇ浼村煕閹寸姷纾奸悗锝庡亽閸庛儵鏌涙惔锛勭闁靛洤瀚伴獮瀣攽閸パ勭暬闂備胶纭堕弲婵嬪磻閵堝宓侀悗锝庡枟閺呮粓鏌﹀Ο渚Х婵顨婂缁樻媴閸涘﹨纭€缂備緡鍣崹鎶藉极椤旂晫绡€闁搞儜鍛炊濠电姷鏁告慨鏉懨洪敃鍌氱厱闁硅揪闄勯悡鏇㈢叓閸ャ劍灏柟顔藉灩缁辨帞鎹勯崫鍕獓缂備胶绮粙鎾斥槈閸偒妲归幖杈剧悼濡插洭鏌ｆ惔銏╁晱闁哥姵鐩、姘愁樄闁糕斂鍎插鍕箛椤掑缍傞梻浣虹帛钃遍柣妤佹崌瀹曟繂顓兼径濞箓鏌涢弴銊ョ仩缂侇偄绉归弻娑氫沪閹规劕顥濇繛瀵稿У閿氭い顏勫暣婵¤埖鎯旈垾鑼埍闂備礁鎼幊蹇曟崲閸曨倠锝夊箛閺夎法顔掗柣搴㈢⊕閿氭い搴㈡崌濮婃椽宕烽鐘茬闁汇埄鍨伴敃銉х博閻旂厧鍗抽柣鎰ㄦ櫆閺傗偓闂備胶绮敃鈺呭窗閺嶎厽鍊堕梺顒€绉甸悡鏇㈡煃閸濆嫬鈧粯鏅堕悽纰樺亾鐟欏嫭绀冪紒璇茬墕椤曪絾绂掔€ｅ灚鏅ｉ梺缁樻濞撳湱鑺遍悽鍛娾拻濞达絽鎲￠崯鐐烘煕閺冩挾鐣电€规洘婢樿灃闁告侗鍠栨禍妤呮椤愩垺澶勯柟灏栨櫊閹垽宕卞☉娆忎化婵°倧闄勭€笛囶敂閻樺樊鐔嗙憸搴∶洪悢鐓庤摕婵炴垯鍨圭猾宥夋煃瑜滈崜娆撴偩濠靛绠瑰ù锝嗙摃閹芥洟姊虹捄銊ユ灁濠殿喗鎸抽悰顕€濮€閿涘嫮顔曢梺鐟邦嚟閸庢劕鈻撻弴銏＄厱闁绘ê鍟块崫鐑樻叏婵犲懏顏犻柟椋庡█閸ㄩ箖鎼归銈勭矚濠碉紕鍋戦崐銈夊磻閸曨垁鍥偨閸濄儱绁﹂棅顐㈡处缁嬫垵顔忓┑瀣厸濠㈣泛顑呴悘锔锯偓瑙勬尭缁夋挳鈥旈崘顔嘉ч柛鈩兠弳妤呮⒑缁嬫鍎愮紒瀣尵濡叉劙骞掑Δ鈧悡娑㈡煕濞戝崬鏋撻柟椋庣帛缁绘稒娼忛崜褍鍩岄梺纭咁嚋缁绘繈鐛崱娑橀唶闁靛绠戞禒鈺佲攽椤旂瓔鐒介柛妯犲嫮顩查柟娈垮枓閸嬫挾鎲撮崟顒傤槰闂佸憡姊瑰ú婵嬫倶閸愵喗鈷戦柛娑橈梗缁堕亶鏌涘鈧粻鏍х暦濠靛鏅濋柍褜鍓熼垾鏃堝礃椤斿槈褔鏌涢埄鍐炬當婵炲吋鍨垮铏圭矙濞嗘儳鍓遍梺瑙勭摃瀹曠敻骞戦姀鐘斀閻庯綆浜為崝锕€顪冮妶鍡楃瑐闁绘帪濡囩槐鐐哄炊椤掍胶鍘繝鐢靛Т缁绘帡鎮炬潏鈹惧亾濞堝灝鏋涙繛灏栤偓鎰佹綎闁告繂瀚呰瀹曟儼顦村ù鐘冲劤閳规垿鎮欓棃娑樹粯闂佸搫鐗滈崜娆擃敋閵夆晛绀嬫い鎾寸☉娴滈箖鏌ㄥ┑鍡欏嚬缂併劎鏅槐鎺楀箛椤旈棿妲愬┑顔硷躬缂傛岸濡甸幇鏉跨闁瑰瓨绮岄弸鍫ユ⒒娴ｅ憡鍟為柨鏇樺灮缁寮借閺嗭箓鏌曡箛瀣偓鏍倿閼测斁鍋撻獮鍨姎婵☆偄鐭傞獮蹇撁洪鍛嫽闂佺鏈銊︽櫠濞戞ǚ妲堥柟鎹愬煐閸も偓闂佸疇妫勯ˇ鍨叏閳ь剟鏌ｅΟ娲诲晱婵¤弓鍗冲缁樻媴閾忕懓绗￠梺鍛婃缁犳挸鐣峰鍐炬僵闁兼悂娼ч崜銊╂⒑閸濆嫬鏆欓柛濠呭吹婢规洘绂掔€ｎ偆鍘遍柣蹇曞仦瀹曟ɑ绔熷鈧弻宥堫檨闁告挻鐟╁畷鏌ュ蓟閵夛妇鐣抽梻鍌欑劍閹爼宕曞鍛勃闁告挆灞捐埞闂傚倸鍊风欢姘焽瑜戦崳褰掓⒑閸涘﹤濮傞柛鏂垮缁傚秹鎮ч崼銏㈢槇闂佹眹鍨藉褎绂掑鍕箚妞ゆ劧缍嗗▓姗€鏌熼獮鍨伈妤犵偞甯掕灃闁逞屽墮鏁堥柡灞诲劜閻撱儵鏌￠崶鈺佷粶闁逞屽墮缂嶅﹪骞冮垾鏂ユ瀻闁规儳顕崢闈涒攽閻愯尙澧曢柣蹇旂箞瀵鈽夊▎宥勭盎闂佹寧绻傞幊宀勫磻閵忋倖鐓涢悘鐐插⒔閵嗘帡鏌嶈閸撱劎绱為崱妯碱洸婵犻潧鐟ゆ径濠庣叆闁割偆鍣ュ鎸庣節閵忥絽鐓愰柛鏃€鐗犻幃锟犲Ψ閿斿墽鐦堥梻鍌氱墛缁嬫帡藟閻愮數纾煎ù锝呭级濞呭﹪鏌″畝瀣瘈闁糕斁鍋撳銈嗗笂缁€渚€顢氶柆宥嗗€垫繛鎴炵懐閻掍粙鏌涘Ο缁樺€愭慨濠冩そ瀹曘劍绻濋崟銊︻潔婵犵數鍋涢惇浼村磹濠靛棛鏆︽い鏍仦閸嬫劙鎮归崶顏勭毢闁告繃顨嗙换娑氣偓鐢殿焾瀛濆銈嗗灥濡繈骞冮敓鐘冲亜闁稿繗鍋愰崢鐢告⒒娓氬洤寮跨紒鐘冲灴閻涱喖螖閸愵亞锛濋悗骞垮劚濡鎮橀幘顔界厸鐎光偓鐎ｎ剛袣缂備胶濮甸惄顖涗繆閻ゎ垼妲鹃梺鍝勬閸婃繂顫忓ú顏勪紶闁告洦鍘炬导鍥⒑閸濄儱校闁绘娲熼幃楣冩倻閽樺）鈺呮煥閺傚灝鈷旈柣锕€鐗撳娲川婵犲倸顫囬梺鍛婃煥椤﹂潧鐣烽幋锕€绠婚悹鍥皺閻も偓闂備胶绮〃鍛存偋韫囨稑鍌ㄩ柛娑橈功缁犻箖寮堕崼婵嗏挃闁诡喖鍚嬮妵鍕敇閻愰潧鈪甸悗娈垮枟婵炲﹪寮幇顓炵窞閻庯綆鍓欓獮鎰攽閻愯埖褰х紒鎻掓健楠炲﹪骞樼€涙ê寮块悗骞垮劚濞茬娀宕戦幘鑸靛枂闁告洦鍓涢ˇ銉╂⒑缂佹澧柕鍫㈩焾閻ｅ嘲煤椤忓嫮鍔撮梺鍛婂姀閺呮盯顢撻幘鏂ユ斀妞ゆ梻鏅粻鎾绘煟椤愶綆娈滅€殿喗鎸虫慨鈧柍閿亾闁瑰嘲顭峰铏圭矙閹稿孩鎷遍梺鑽ゅ暀閸ヤ礁娲弫鍐磼濞戞艾骞嶇紓浣哄亾濠㈡ê鐣烽浣侯洸濞寸厧鐡ㄧ€电姵绻濋棃娑欑仼鐟滅増甯掔粈鍌炴煕韫囨艾浜圭紒瀣喘濮婃椽骞栭悙鎻掝瀳闂佺锕ゅ鈥愁嚕婵犳碍鏅插璺猴功閻嫰姊虹粙鎸庢拱闁煎湱鍋撶粋鎺戭潨閳ь剙顫忓ú顏勫窛濠电姴鍊绘禒鍏肩節閻㈤潧浜归柛瀣尭铻栭柣姗€娼ф禒锕傛煟濡や焦绀夌憸棰佺椤啴濡堕崱妤€娼戦梺绋款儐閹告悂鍩為幋锕€鐏抽柤纰卞墰閻撴捇姊洪崫鍕効缂傚秳绶氶悰顔碱潨閳ь剙鐣烽敓鐘冲€烽柟缁樺醇閺囥垺鈷掑ù锝呮啞閹牓鏌ｉ鐑嗘Ш缂佽京鍋為幏鍛村礈閹绘帗顓挎俊鐐€栫敮鎺斺偓姘煎墰缁粯瀵奸弶鎴狀啇濠电儑缍嗛崜娆撴倶閳哄懏鐓曢柟鎹愬煐閺嗩剟鏌″畝瀣ɑ闁诡垱妫冩慨鈧柍鍨涙櫅椤瞼绱撻崒娆愮グ妞ゆ泦鍥х９闁秆勵殔閺勩儵鏌ｉ幇顒佹儓缂佺媴缍侀弻銊╁籍閸ヮ煈妫勯梺閫炲苯澧伴柛蹇旓耿瀵鈽夊Ο閿嬬€婚棅顐㈡祫缁插墽鐟у┑鐘垫暩閸嬫盯骞忛幋鐘冲床婵犻潧妫鏍ㄧ箾瀹割喕绨荤紒鐘茬秺閺岋綁骞囬鐐电シ濡炪値鍓欏Λ娑氭閹惧鐟归柛銉戝嫮浜堕梻浣藉吹閸犲棝宕曢悽鎼炩偓浣割潩閼稿灚娅滈梺鎼炲劘閸斿矂鎮甸弶搴撴斀闁绘劕鐡ㄧ紞鎴炪亜閹存繃顥㈤柨婵堝仜椤撳ジ宕ㄩ鍛澑婵＄偑鍊栧濠氭偤閺傚簱鏋旈柡鍐ｅ亾濞ｅ洤锕、鏇㈡晲閸ャ劌娅氶柣搴ゎ潐濞叉﹢骞戦崶顒€绠栧ù鐘差儏瀹告繂鈹戦悙闈涗壕閻庢俺鍋愮槐鎾诲磼濮橆兘鍋撻悜鑺ュ€块柨鏇氱劍閹冲矂姊虹拠鑼婵炲瓨宀稿畷銏ゆ偩鐏炴儳搴婂┑鐐村灟閸ㄥ湱绮荤紒妯镐簻闁哄啫娲よ闂佸搫妫涙慨椋庢閹惧瓨濯寸紒瀣氨閸嬫捇宕烽鐔告闂佽法鍠撴慨浼村焵椤戣法顦︽い顐ｇ矒閸┾偓妞ゆ帒瀚粻鏍煏韫囧鈧牠骞嗛悙鐑樼厱闁挎棁顕ч獮妤併亜椤愩垻鐒告慨濠傤煼瀹曟帒鈻庨幋顓熜滈梻浣告憸閸犲骸顭囬敓鐘茬畾闁告劑鍔夐弨浠嬫倵閿濆簼绨介柨娑欑洴濮婅櫣鎲撮崟顐㈠Б闂佽绻愰悧鎾崇暦閹达箑绠婚柤濮愬€曠粊锕傛⒑閸撹尙鍘涢柛鐘崇墬閹便劍鎯旈埦鈧弨浠嬫煟閹邦垰鐨烘繝鈧幘顔界厱闁哄啠鍋撻柛銊ф暬椤㈡岸鏁愭径濠勵唴闂佽姤锚閿涘顦叉い顓℃硶閹瑰嫭绗熼姘闂備線鈧偛鑻晶顕€鏌ｈ箛鏃€鐨戦柟骞垮灩閳藉濮€閻樿鏁规繝鐢靛█濞佳囨偋閸涘瓨鎯為幖绮规閺€浠嬫煟閹邦剙绾ч柛锝嗘そ閺屻劑寮村Ο铏逛紙閻庢鍠涢褔鍩ユ径鎰潊闁绘ɑ顔栭崯瀣⒑閼姐倕鞋婵炲拑缍佸畷鏇㈠Χ婢跺鈧爼鏌涢鐘插姕闁抽攱鍨垮濠氬醇閻旇　濮囧銈冨劜閻熝呮閹烘鐒垫い鎺嶈兌閻熷綊鏌嶈閸撶喖寮幇鐗堝€风€瑰壊鍠氶崣鍡涙⒑閸撴彃浜濈紒璇插瀹曟繈鏁冮埀顒勨€旈崘顔嘉ч柛鈩冾殘閻熸劕鈹戦敍鍕沪閻㈩垳鍋ら獮鎴﹀閵堝懎鑰垮┑鈽嗗灥椤曆冾嚕閸喒鏀介柣妯肩帛濞懷勪繆椤愶絿娲撮柟顔惧仱閹瑩顢栫捄銊х暰闂備礁鎲￠悷銉┧囬鐐茬闁靛牆妫庢禍婊勩亜韫囨挸顏╅柡鍡到閳规垿鍩勯崘銊ュЦ缂備胶濮甸惄顖炵嵁濮椻偓閹虫粓寮仦鎯ф诞婵﹦绮幏鍛瑹椤栨粌濮兼俊鐐€栭崹鐢稿箠濮椻偓瀵偊宕橀鍢壯冾熆閼哥數銆掔紒?
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
        // up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗鎸抽幃娆徝圭€ｎ亙澹曢悷婊呭鐢帞澹曢崸妤佺厵閻庣數顭堟牎闂佸摜濮甸崝娆撳蓟閿濆憘鏃堝焵椤掑嫭鍋嬮柛鈩冪懅缁犳棃鏌熼悜妯烩拻缁炬儳銈搁弻宥堫檨闁告挻鑹鹃銉╁礋椤撴繃鍕冪紓浣割儏閵囨ɑ绔熼弴銏♀拺缂佸娉曠粻鐗堛亜閿旇鐏＄紒鍌氱У閵堬綁宕橀埞鐐闂傚倷绶￠崑鍡涘磻濞戙垺鍤愭い鏍ㄧ⊕濞呯娀鏌熺紒銏犳灍闁绘挻娲熼弻宥囨喆閸曨偄濮㈡繛瀛樼矌閸嬫挻绌辨繝鍥ㄥ€锋い蹇撳閸嬫捇寮介锝嗘婵犵數濮寸€氼噣鎯岄崱妞尖偓鎺戭潩閿濆懍澹曢柣搴ゎ潐濞叉粓寮繝姘モ偓浣肝旈崨顓狀槹濡炪倖甯掗崐鎼佺嵁閸喓绡€闁汇垽娼ф禒婊勪繆椤愶綆娈橀柟骞垮灲楠炲洭寮堕幐搴ょ发闂備焦瀵х换鍌炈囨导瀛樺亗闊洦绋掗悡鏇㈡煃閳轰礁鏆炵紒鈧崼鐔翠簻闁挎棁鍋愰悾鐢告煙閽樺鍩ｇ€殿喕绮欓、姗€鎮㈤崫鍕瑲闂佽崵鍠愮划宥囧垝閹惧磭鏆︽繝濠傜墛閸嬪嫰鏌ｉ幘铏崳濞寸厧顑夊娲箰鎼淬垻顦ラ梺绋匡工閹诧繝宕曢锔解拻濞达絽鎽滈弸鍐╀繆濡炵厧濡跨紒顔肩墛閹峰懘鎼归崷顓炵婵犵數鍋為崹鍫曟晝閳轰降鈧帗绻濆顓犲帾闂佸壊鍋呯换鍐夐悙娣簻闁靛鍎查ˉ婊堟煃鐟欏嫬鐏存い銏＄懇瀹曨偊宕熼妸銉ゅ濠电娀娼ч鍐焵椤戣法顦﹂柍璇查叄楠炴ê鐣烽崶褉鍋撻悙宸富闁靛牆妫楃粭鍌炴偣娴ｅ湱鍙€妤犵偛绻掔槐鎺懳熼懖鈺婂晭闂佸搫顦悧鍡樻櫠娴犲绀嗗ù鐓庣摠閻撴瑩鎮归崶鍥ф噽閻﹀牓鎮楃憴鍕闁搞劌鐖奸獮鍐ㄢ枎閹炬潙浠洪梺姹囧灮椤ｎ喚妲愰弻銉︹拻濞达絽鎲￠崯鐐电磼鐠囨彃鈧鍩€椤掍礁鍤柛娆忓暙閻ｇ兘骞嬮敃鈧敮閻熸粌顦靛畷鎴﹀箻閼搁潧鏋傞梺鍛婃处閸撴盯藝閵娿儮鏀介柣鎰硾閻ㄦ椽鏌涢悩鏌ュ弰闁诡喗鍎抽悾锟犲箯閺冨倸鏋戠紒杞扮矙閹稿﹥寰勭€ｎ厾顢呴梻渚€鈧偛鑻晶鍓х磽瀹ュ懏顥㈢€规洘鍨垮畷鍗炍熺喊杈ㄩ敜婵犵數鍋涘Λ娆撳箰婵犳艾鐓曢柟鐑樺灟閳ь剚甯掗～婵嬫晲閸涱剙顥氬┑鐘垫暩閸嬫﹢宕犻悩璇插窛妞ゆ梻鍘х花銉╂⒒娴ｇ顥忛柛瀣╃窔瀹曟洘娼忛埡渚囨閻熸粎澧楃敮妤呮偂濞戞埃鍋撻獮鍨姎濡ょ姵鎮傞悰顔嘉旈崨顔惧幈闁诲函缍嗘禍婊堝焵椤掆偓濠€閬嶅箲閵忕姭妲堥柕蹇曞Х椤撴椽姊虹紒妯虹仴婵炶绲垮▎銏ゆ偨閸涘ň鎷虹紓鍌欑劍钃遍柣鎾卞劜娣囧﹪顢曢敐鍥╃杽濡ょ姷鍋涢崯鎾箖閻戣棄绠犲┑鐘插暞閵囨繃銇勯姀鈩冾棃鐎规洖宕埢搴ㄥ箣閻樿尙鍊為梻鍌氬€峰ù鍥敋瑜忛埀顒佺▓閺呯娀骞嗗畝鍕垫晪闁逞屽墮閻ｇ兘鏁撻悩鑼唴闂佽姤锚椤︽娊骞楅弴銏♀拺闁圭娴烽埥澶愭倵濮樷偓閸パ咃紱闂佺懓澧界划顖炲疾閺屻儱绠归悗娑欘焽缁犳﹢鏌ｉ埡渚€顎楁い顏勫暣婵″爼宕卞Δ鍐句紳闂備焦鎮堕崝瀣垝濞嗗浚鍤曞┑鐘崇閸嬪嫰鏌涘┑鍕姢闁硅櫕绻堝娲礈閼碱剙甯ラ梺鍝ュУ閻楃娀鍨鹃弽顓為唶闁靛濡囬崣鍕箾閺夋垵鎮戦柣鐔濆洤绠栭柤娴嬫櫇绾惧ジ寮堕崼娑樺缂佹う鍥ㄧ厓鐟滄粓宕滈妸褏绀婇柛鈩冾焽椤╂煡鏌ｉ幇顒夊殶妞も晝鍏橀弻鏇烆嚗濞戙埄鏆掔紒銊嚙椤啴濡堕崱妤€娼戦梺绋款儐閹稿濡甸崟顖涙櫇濞达絽鍢查幆鍫濃攽閳ュ啿绾ч柛鏃€鐟╅悰顔嘉熺亸鏍т壕婵炴垶顏鍫晛闁规儳澧庣壕钘壝归敐鍛棌闁稿骸绻掔槐鎺楀Ω閳衡偓閹插墽鈧娲橀崹鍧楃嵁濡吋瀚氶柛娆忣槸缁佽埖淇婇悙顏勨偓鏍箰妤ｅ啫绐楅柟閭﹀厴閺嬫柨霉閻撳海鎽犻柍閿嬪灴閹綊骞侀幒鎴濐瀳闂佸搫顑嗛弻銊╁煘閹达富鏁婇柤鎭掑劚閸炲鎮楀▓鍨灈闁硅绱曠划顓㈡偄閻撳海鍔﹀銈嗗笒鐎氼剟鎷戦悢鍏肩厽闁哄倸鐏濋幃鎴︽煕鐎ｎ亶鍎旈柡灞剧洴椤㈡洟濡堕崨顔句簴闂備礁鎲￠崺濠勬崲濠靛棭娼栫紓浣股戞刊瀵哥磼鐎ｎ厼鍔甸柛鈺冨仜閳规垿鎮欐０婵嗘疂婵犵鈧櫕鍠樻鐐村灴婵偓闁绘﹩鍋呴～宥夋⒑闂堟稓绠冲┑顔惧厴椤㈡ê煤椤忓應鎷虹紓鍌欑劍閿氬┑顔肩焸閹绠涢弮鍌涘櫚闂佽鍠氶崗妯讳繆閹间焦鏅滈柛鎾楀苯鏅梻浣筋嚙鐎涒晝绮欓幒妞烩偓锕傚炊椤掍礁鍓归梺鍦劋濮婅崵澹曟總鍛婄厽婵☆垵娅ｉ敍宥咁熆瑜忛弫鎼佸焵椤掍緡鍟忛柛鐘崇洴椤㈡俺顦归柛鈹垮劜瀵板嫰骞囬澶嬬秱闂備胶绮…鍥极閹间礁绾ч柟闂寸劍閳锋帡鏌涚仦鎹愬闁逞屽墯閹倿骞冭缁绘繈宕熼鐘靛幆闂備礁婀遍崕銈夈€冮崱娑樼９闁割偅娲橀悡鏇熴亜閹邦喖孝闁告梹绮撻弻锝夊箻鐠虹儤鐏堝Δ鐘靛仦閻楁洝褰佸銈嗗坊閸嬫捇鏌ｈ箛鎾宠埞妞ゎ叀娉曢幉鎾礋閸℃ê鐏村┑锛勬暬瀹曠喖顢涘☉娆愮彇闂備胶顭堥張顒勬偡閿曞倸绐楁慨妯垮煐閳锋垹绱掗娑欑闁哄缍婇弻娑氣偓锝庡亝鐏忣厽銇勯鍕殻濠碘€崇埣瀹曞崬螖閳ь剙顭囬幋锔解拺缂佸顑欓崕鎰版煙缁嬪灝鈷旀俊鍙夊姍楠炴帒螖娴ｉ晲姹楅梻浣藉亹閳峰牓宕曢悿顖ｆЬ闂侀€炲苯澧叉い顐㈩槸鐓ゆ俊顖氥偨濞差亝鍋勯柣鎾虫唉閹芥洟姊虹化鏇炲⒉妞ゃ劌绻樺畷銉р偓锝庡枟閻撴洘绻涢幋婵嗚埞闁诲骏缍侀弻锛勨偓锝庝簻閺嗭絾鎱ㄦ繝鍐┿仢闁诡喚鍏樺鍫曞箰鎼粹檧鍋撻鐐粹拺閻犲洠鈧磭浠╅梺娲诲幖閸婂潡鐛崘顔肩労闁告劑鍔庣粣鐐烘倵閸忓浜鹃梺閫炲苯澧扮紒顔肩墛缁绘繈宕堕妸褍骞愰柣搴＄畭閸庤鲸顨ラ幖浣哄祦婵°倕鎳忛悡鐔兼煙閹咃紞缂佺姴顭烽弻锛勪沪閸撗勫垱濡炪們鍨哄ú鐔煎极閹版澘鐐婇柍鍝勫暞濮ｅ洨绱撻崒姘偓椋庢媼閺屻儱纾婚柟鎯х亪閸嬫捇宕楁径濠佸闂備礁鎲″ú锕傚礈濞戙垹鍌ㄩ梺顒€绉甸悡鐔煎箹閹碱厼鐏ｇ紒澶屾暬閺屾稓鈧綆浜濋崳褰掓煟閿濆懎妲婚柣锝嗙箞閸┾偓妞ゆ帒瀚悞鍨亜閹烘垵鏋ゆ繛鍏煎姈缁绘盯宕ｆ径宀€鐓夐悗瑙勬礃椤ㄥ懘锝炲鍫濈劦妞ゆ帒瀚繚婵炶揪缍€閸婁粙鎮㈢亸浣圭€婚梺缁樺姦閸庣兘顢楅崟顑芥嫽婵炶揪绲介幉锟犲疮閻愬瓨鍙忓┑鐘插€荤粔鐑橆殽閻愭彃鏆ｇ€规洘甯￠幃娆撴嚑椤掑倹姣囧┑鐘茬棄閺夊簱鍋撻幇鏉跨；闁瑰墽绮悡鍐磽娴ｈ偂鎴濈暦瀹€鈧埀顒冾潐濞叉牜绱炴繝鍥モ偓浣糕枎閹炬潙浠奸柣蹇曞仩閸嬫劙鎯屽鑸碘拻闁稿本鑹鹃埀顒傚厴閹虫宕滄担鐟板幑闂佸壊鍋呭ú鏍偪閻愵剛绡€濠电姴鍊绘晶娑㈡煕鐎ｎ亶鍎旈柡宀嬬到铻ｉ柛顭戝枤濮ｃ垽姊洪崫鍕紨闂傚嫬瀚伴獮鍫ュΩ閵夘喗寤洪梺绯曞墲椤ㄥ懐绮婚懡銈囩＝濞达絽寮堕鍡涙煕鐎ｎ偅宕屾慨濠勭帛閹峰懐鎲撮崟顐″摋闂備胶顭堢€垫帡宕圭捄铏规殾婵犻潧妫崥瀣熆鐠虹尨鍔熼柨娑氬枎椤啴濡堕崨顖滎唶闂佺粯顨呴敃顏堝箖閿熺姵鍋勯柛蹇氬亹閸樹粙姊洪崫鍕偓鍦偓绗涘洤绠熼柟闂寸劍閻撴瑩鏌涢幇顓炵祷闁哄棴缍侀弻鐔碱敊缁涘鐣跺銈庡亝缁诲牓骞冮埄鍐╁劅闁抽敮鍋撻柟濂夊亰濮婄粯鎷呯粵瀣異闂佹悶鍔嬮崡鍐茬暦椤栫偛绠绘い鏃囥€€閸嬫挻绗熼埀顒€顕ｉ幘顔碱潊闁斥晛鍟悵鏇㈡⒑閼姐倕鏋戦柣鐔讳含瀵板﹪宕滆閸嬫挸顫濋鎯т划濠殿喖锕︾划顖炲箯閸涙潙宸濆┑鐘插€瑰▓妯肩磽閸屾瑧顦︽い鎴濇瀹曞綊宕稿Δ鈧弸渚€鏌涢幇闈涙灈閸ュ瓨绻濋姀锝嗙【妞ゆ垵鎳橀崺鈧い鎺嗗亾闁绘牕銈稿濠氭晲婢跺﹥顥濋梺鍓茬厛閸犳宕愰鐐粹拺缂備焦蓱鐏忕増绻涢崣澶屽⒌闁糕晝鍋ら獮瀣晜閽樺姹楅梻浣告贡缁垳鏁Δ鍛€堕柟鐐灱閺€浠嬫煟閹邦垰鐨哄褎绋撶槐鎺楊敊閼恒儱纾抽悗娈垮枛椤兘寮幇鏉垮窛妞ゅ繐鎷嬪鏃堟⒒娴ｅ憡鍟炴い銊ョ墦瀹曟垿鎮欓悜妯轰户濠电娀娼уú銊у姬閳ь剙鈹戦悙鑼闁诲繑绻堝鎼佸箻缂佹鍘遍柣搴秵閸嬪懎鐣风仦鐐弿濠电姴鍟妵婵囦繆椤愩垹鏆欓柍钘夘槸椤繃娼忛埡鍌涚钒闂傚倸鍊烽悞锕傚箖閸洖绀夌€光偓閸曘劉鍋撻敃鍌涘€婚柦妯侯槹閻庮剟鎮楅獮鍨姎闁瑰嘲顑夐幃锟犲Ψ閳哄倻鍘卞銈嗗姂閸婃洟寮抽柆宥嗙厵闂佸灝顑嗛妵婵囨叏婵犲啯銇濈€规洏鍔嶇换婵嬪礋椤撶姵娈奸梻浣筋嚙鐎涒晠宕欒ぐ鎺戠闁绘柨顨庨崵鏇㈡煙閹澘袚闁稿﹦鍏橀弻锝夊箣閻戝棛鍔烽梺鍛婏耿娴滆泛顫忓ú顏咁棃婵炴番鍊栭惄顖氱暦閻旂⒈鏁嗗ù锝囧劋椤粓姊婚崒姘偓宄懊归崶顒夋晪闁哄稁鍘肩粣妤佷繆閵堝懏鍣洪柡鍛箞閺屾洝绠涚€ｎ亖鍋撻弴鐘典笉闁哄稁鍘介悡鍐煕濠靛棗顏柛锝呯秺閺屾盯濡搁妷銉㈠亾瑜版帒桅闁告洦鍨扮粻娑㈡煃鏉炴媽鍏屽ù鐘荤畺濮婅櫣鎷犻弻銉ュ及缂備胶濮甸悧鏇綖韫囨洜纾兼俊顖濐嚙椤庢捇姊洪幆褏绠抽柟铏尵缁參鍨鹃幇浣瑰瘜闂侀潧鐗嗛崯顐ｄ繆婵傚憡鐓欓柧蹇ｅ亾閼版寧顨ラ悙鎻掓殺妞わ箑澧庣槐鎺楁偐瀹曞洦鍒涘Δ鐘靛仦钃遍柟宄版嚇閹崇偤濡烽璇叉憢闂傚倷绶氶埀顒傚仜閼活垱鏅堕婊呯＜閻庯綆鍋勫ù顕€鏌℃担绋挎殻闁糕斁鍓濋幏鍛村矗婢跺奔绨存繝鐢靛仜椤曨厽鎱ㄩ幘顕呮晞闁糕剝绋掗崑鍌炴煟閺傚灝鎮戦柣鎾寸懇濮婃椽顢橀妸褏鏆犲Δ鐘靛仦椤ㄥ牏妲愰幘鎰佸悑闁糕剝顨堟禒鍝ョ磽娴ｈ櫣甯涢柣鈺婂灦閻涱噣寮介褎鏅ｉ梺缁樺姇椤曨厽绂嶉鍫熲拻濞达絽鎽滈弸鍐┿亜椤愩埄妯€鐎规洖缍婂畷绋款渻鐏忔牕浜惧ù锝囩《閺嬪酣鏌熼悙顒佺稇濞寸媭鍘芥穱濠囶敃閿旂粯娈ョ紓浣插亾濞撴埃鍋撻挊鐔告叏濡灝鐓愰柍閿嬪浮閺屾稓浠﹂幑鎰棟闂侀€炲苯澧伴柛蹇旓耿楠炲啴鎮欓悜妯绘珕闂備焦顑欓崹鐗堢閸撗呯＜婵°倓鑳堕埥澶愭煙閾忣偄濮嶉柟顖氳嫰閳诲酣骞樼€电骞嶉梺璇叉捣閺佹悂鈥﹂崼锝傚彺闂傚倷鑳堕…鍫ヮ敄閸℃稑绠伴柟闂寸筏缂嶆牗绻濋棃娑卞剰閸烆垶姊洪棃娑辨Ф闁稿骸鎼…鍥ㄦ償閵婏妇鍘介柟鍏肩暘閸娿倕顭囬幇顓犵闁告瑥顧€閼拌法鈧娲栫紞濠傜暦缁嬭鏃堝焵椤掑倻涓嶉柡宥冨妿缁犻箖鏌涢埄鍏╂垹浜告导瀛樼厱濠电姴瀚敮娑樓庨崶褝韬い銏＄洴閹瑧鈧稒蓱濞堟悂姊虹拠鎻掝劉闁煎綊绠栨俊鍓佺矙濞嗙偓缍庣紓鍌欑劍钃卞┑顖涙尦閺屾稑鈽夊鍫濅紣闂佸搫妫楅悧蹇曟閹惧瓨濯撮柛鎾村絻閸撻亶姊洪幖鐐插婵炲皷鈧剚鍤曟い鎰剁畱缁€鍐┿亜閺冨洤袚婵炲懎娲铏圭矙閹稿孩鎷卞┑顔角滈崝鎴﹀箖閸ф鐐婃い鎺嶈閹锋椽鎮峰鍛暭閻㈩垱甯楃粋宥嗗鐎涙鍘遍柟鍏兼儗閸犳洟鏌囬鐐寸厵妞ゆ梹鍎抽崢鎾煛鐏炶姤鍠橀柡浣稿€块獮鍡氼槻婵鐓″濠氬磼濞嗘埈妲梺瑙勭ゴ閸撴繄绮悢鑲烘棃宕ㄩ鐐蹭憾闂備礁鎲＄换鍌溾偓姘ｅ亾闂佹寧绻傞ˇ顖滅不濞戙垺鐓熸俊銈傚亾闁绘妫濋崺鈧い鎺嗗亾闁诲繑姘ㄩ幑銏犫槈閵忕娀鍞跺┑鐘绘涧閻楀懘鎼规惔鈾€鏀介柣鎰级閸ｆ椽鏌ㄩ弴妯虹伈鐎殿喛顕ч埥澶婎潩閿濆懍澹曢梺鎸庣箓缁ㄥジ骞冨澶嬬厸闁逞屽墰閹风娀鎯堥鈧柊锝呯暦閹偊妾梻濠庡墻閸撶喖寮婚悢鍏兼優妞ゆ劑鍊楅悡澶娾攽閳藉棗浜滈柛鐕佸亰閸┿儲寰勬繝搴㈠兊闁哄鐗冮弲娑氱矈閿曗偓閳规垿鎮╅崹顐ｆ瘎婵犳鍠氶崗妯侯嚕椤愶箑宸濆┑鐘插暙瀵潡鏌ｆ惔锝嗘毄缁绢厼鐖煎畷鎴﹀箻閹颁焦鍍靛銈嗗姂閸ㄥ湱绮婇鈧铏规嫚閳ヨ櫕鐏嗛梺鍛婎殕婵炲﹪鎮伴鍢夌喖鎳栭埡鍐跨床婵犵妲呴崹鍫曞绩鏉堚斁鍋撳铏棄闁宠鍨块幃娆戔偓娑櫭棄宥夋⒑缁洘娅呴柛鐔告綑閻ｇ兘骞嬮敃鈧粻濠氭倵闂堟稒鎲告い鏃€娲樼换娑欐綇閸撗冨煂闂佸摜鍠庨悘婵嬪煝閺傛娼╅悹楦挎閸婄偛顪冮妶鍡楃瑨妞わ缚鍗冲鏌ユ晲婢跺鎷洪梻鍌氱墛缁嬫挾绮婚崘娴嬫斀妞ゆ梹鍎抽崢鎾煛娴ｅ摜效闁诡喓鍨藉畷妤冧焊閺嶃劌顏烘繝鐢靛仦閹稿鎳濋幆顬℃椽濡堕崱妯荤彿濡炪倖鏌ㄦ晶鐣屽閻撳寒鐔嗛悹鍝勬惈椤掋垻鐥銏㈢暫闁哄矉缍侀弫鎰板川椤栨瑧椹崇紓鍌欑劍椤ㄥ牓宕伴弽顓炴槬闁逞屽墯閵囧嫰骞掗幋婵愪患闂佽棄鍟伴崰鏍蓟閿濆妫橀柟绋块閺嗗牏绱撻崒姘毙㈡繛宸弮瀵鈽夐姀鐘殿啋濠德板€愰崑鎾绘煙閸愯尙绠伴柍缁樻崌瀹曨偊濡疯閿涙繈姊虹粙鎸庢拱闁荤喆鍔戝畷妤冧沪鐟欙絾鏂€濡炪倖姊婚埛鍫ュ吹閻旇櫣纾奸弶鍫氭櫅娴狅妇绱掔紒妯肩疄鐎规洘鍎奸ˇ鎾煟閿濆懏鎯堥柍瑙勫灦楠炲﹪鏌涙繝鍐ㄥ鐎规洘鍨块獮妯肩磼濡厧甯楅梻浣藉Г閿氭い锕佹鍗遍柟闂寸劍閻撳啴鎮归崶顏勭毢閺佸牓姊洪崷顓炲付缂傚秴锕妴渚€寮撮姀鐙€娼婂銈庡亽閸樹粙宕戦崨顖滅＝闁稿本鑹鹃埀顒勵棑濞嗐垽鏁撻悩鑼崶濠德板€愰崑鎾淬亜椤愶絿绠炲┑鈩冩倐閸┾剝鎷呮笟顖氭暭闂傚倷绀侀幉鈥趁洪敃鍌氬瀭闁绘鐗嗛崝鏃堟⒑閼姐倕鏋戠紒顔肩灱閹广垹顫滈埀顒€顕ｉ銏╁悑闁告粈鑳堕崣鍡涙⒑閸撴彃浜為柛鐘虫崌瀹曘垽宕ㄦ繝鍕槇闂侀潧楠忕徊鍓ф兜妤ｅ啯鐓ラ柡鍥朵邯濡绢噣鏌￠崨顓犲煟妤犵偛顑夐弫鍌炴偩鐏炶棄绠為梻鍌欑閹碱偊宕愰崷顓涘亾缁楁稑娲ょ粻鏌ユ煥濠靛棙宸濈痪鎯у悑閵囧嫰寮崒妤佸珱闂佺粯鎹侀崑鎰板焵椤掍緡鍟忛柛鐘虫礈閸掓帒鈻庨幘鎵佸亾娓氣偓瀵挳濮€閻樻爠鍥ㄧ厱闁靛鏅╁Λ鎴﹀箹閺夋埊韬慨濠勭帛閹峰懐绮电€ｎ亝顔勯梻浣告啞閸ㄥ綊寮查銈嗩潟闁绘劕鎼獮銏′繆椤栨壕鎷℃繛鍙夋倐閺岋綁鎮╅悜妯糕偓鍐磽瀹ュ懐效闁轰焦鍔栧鍕節閸屾粎宕烘繝鐢靛Х閺佹悂宕戝☉銏犖︽俊顖濆婵啿鈹戦悩宕囶暡闁抽攱鍨块弻锝夋偄閸涘﹦鍑￠梺鎶芥敱濡啴寮诲☉姘ｅ亾閿濆啫濡煎┑顔煎€婚埀顒冾潐濞叉﹢宕归幐搴㈠弿闁逞屽墴閺屽秹宕瑰☉娆愮彅婵炲瓨绮撶粻鏍箖瀹勬壋鏋庨煫鍥ㄦ惄娴犲墽绱撴担鎻掍壕闂侀€炲苯澧存慨濠冩そ濡啫鈽夊顒夋澑闂備浇顕栭崰鏍磹閹稿簼绻嗛悗娑欘焽閻熷綊鏌嶈閸撴瑩顢氶敐澶婄妞ゆ梻鈷堝濠囨⒑閺傘儲娅呴柛鐕佸灦閹敻骞庨懞銉㈡嫽婵炶揪缍€椤宕戦悩缁樼厱閹兼惌鍠栧▍宥団偓娈垮枟瑜板啴鍩ユ径鎰潊闁靛繒濮烽妶锕傛⒒娓氣偓閳ь剛鍋涢懟顖涙櫠鐎涙ɑ鍙忓┑鐘插亞閻撹偐鈧娲樼敮鎺楋綖濠靛鏁勯柦妯侯槷婢规洟姊鸿ぐ鎺擄紵缂佲偓娴ｅ搫顥氶柣鐔煎亰濞撳鎮楅敐搴濈盎妞ゆ帇鍨洪妵鍕箛椤栨鎾绘煙娓氬灝濡兼い顐ｇ矒瀹曞崬螖閳ь剟鍩涢幒妤佲拺缂備焦锚缁楀倻绱掗鐣屾噰鐎殿喖顭锋俊鍫曞幢濡搫浼庨梻浣哥秺閸嬪﹪宕楀鈧畷鎴﹀箻鐠囨彃鍞ㄥ銈嗗姉閸犲孩绂嶉悙顒佸弿婵妫楁晶濠氭煟閹剧偨鍋㈤柡灞糕偓宕囨殕閻庯綆鍓涜ⅵ闁诲孩顔栭崳顕€宕抽敐鍛殾闁圭儤鍩堝鈺傘亜閹达絾纭堕悽顖涚〒缁辨捇宕掑顑藉亾妞嬪孩濯奸柡灞诲劚绾惧鏌熼悙顒€澧柣鏂挎閹綊宕崟顒佸創濡炪倧瀵岄崜鐔煎蓟濞戞埃鍋撻敐搴′簼鐎规洖鐬奸埀顒冾潐濞叉﹢銆冮崱妤婂殫闁告洦鍓涚弧鈧繛杈剧到婢瑰﹤螞濠婂牊鈷掗柛灞捐壘閳ь剟顥撶划鍫熺瑹閳ь剟鐛径鎰伋閻℃帊鐒﹀浠嬪极閸愵喖纾兼慨姗€纭稿Σ鑸电節閻㈤潧浠滄俊顐ｇ懇瀹曟繂螖閸涱偀鍋撻崨顓犳殝闁逛絻娅曢弬鈧梻浣侯焾閺堫剛鍠婂澶婄柈闁绘劗鍎ら悡鍐⒑閸噮鍎忛柣蹇婃櫇缁辨帗娼忛妸锕€闉嶉梺鐟板槻閹虫﹢骞嗛崒鐐粹拹闁归偊鍠涚换鎴︽⒒閸屾瑨鍏岀紒顕呭灣閺侇噣鎮欓崫鍕煣濡炪倖鍔戦崐鏍綖閺囥垺鐓欓柣鎴烇供濞堛垽鏌℃担鍓插剶闁哄本绋戦埥澶愬础閻愯尙顔掑┑鐐差嚟婵敻骞冮崒姘兼綎闁惧繗顫夌€氭岸鏌嶉妷銊︾彧闁绘繍鍨崇槐鎾存媴閾忕懓绗＄紓浣筋嚙閸婂潡骞冩ィ鍐╁€婚柦妯侯槺椤斿﹪姊洪悷鎵憼婵﹤缍婂顐︽偐缂佹ǚ鎷洪柣鐘叉礌閳ь剙纾埀顒€顭烽弻娑氫沪閹冩瘓濠碘槅鍋呯划宀勫煘閹寸偛绠犻梺绋匡攻椤ㄥ棝骞堥妸鈺傚€婚柦妯侯槺閿涙稑鈹戦悙鏉戠仧闁搞劍妞藉畷鎰版嚋閻㈢數鐦堥梻鍌氱墛娓氭宕曡箛鏇犵＜闁逞屽墯瀵板嫮鈧綆鍋勫鍨攽閳藉棗鐏犻柣蹇旂箞閹繝骞囬悧鍫㈠幗闂佽鍎抽顓㈠箠閸ヮ剚鐓涢悘鐐插⒔濞插瓨顨ラ悙鍙夊枠妞ゃ垺绋戦～婵囨綇椤垵娈戠紓鍌氬€搁崐鐑芥嚄閼稿灚鍙忛柟顖嗗懏娈鹃梺姹囧灮椤牓鎮?
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇洟姊绘担钘壭撻柨姘亜閿旇鏋ょ紒杈ㄦ瀵挳濮€閳锯偓閹风粯绻涙潏鍓хК婵炲拑缍佹俊瀛樼節閸ャ劎鍘遍梺瑙勫劤椤曨厾绮婚悙鐑樼厵妞ゆ梻鍋撻悞鎸庛亜閿曗偓缂嶅﹪寮婚悢纰辨晬婵浜崝顖毼旈悩闈涗沪妞ゎ厼娲ㄥΣ鎰板箳閹冲磭鍠撻幃浼村灳閸忓懎顥氶梻浣稿閻撳牓宕戦崱娆戜笉濡わ絽鍟悡娆撴倵閻㈡鐒鹃柛鎾冲船闇夐柣鎾冲椤ャ垽鏌″畝鈧崰鎰焽韫囨稑绀堢憸蹇涘汲閻樼粯鈷戞繛鑼额嚙楠炴銇勯妸銉含闁诡噯绻濋、鏇㈡晝閳ь剟鎮欐繝鍥ㄧ厪濠电倯鈧崑鎾斥攽椤斿吋鍠樻慨濠呮缁辨帒螣鐠囧弶娈梻浣告憸婵敻鎮ч悩宸殨濠电姵纰嶉弲鎼佹煟濡灝鐨洪柨娑欑矊閳规垿顢欓惌顐海閵囨劖寰勯幇顓熷劒闁瑰吋鐣崝宥夊煕閹达附鍋ｉ柛銉岛閸嬫捇鎼归銈勬喚闂傚倷鐒﹂幃鍫曞礉鐏炵瓔鐒介柍銉﹀墯閸ゆ洟鎮归崶銊с偞婵℃彃鐗婄换娑㈠幢濡搫顫岄梺鍛婃煥閹虫ê顫忓ú顏勭閹艰揪绲哄Σ鍫ユ煟鎼淬垼澹橀柟铏崌钘濈憸鐗堝笚閻撴稓鈧厜鍋撻悗锝庡墰閿涚喐绻涚€电顎撶紒鐘虫尭閻ｅ嘲顭ㄩ崱鈺傂梻浣告啞鐢鎯勯鐐茬畺婵°倐鍋撻柍缁樻崌瀹曞綊顢欓悾灞兼喚闂傚倷娴囬～澶愭偤閺囩偐鏋嶉柡鍥╁枔閳瑰秴鈹戦悩鍙夋悙缂佺姷鎳撻湁闁挎繂鐗嗛埀顑跨矙婵℃悂鍩￠崒婊冨妇濠电姰鍨奸鏍垂娴煎瓨鍤愭い鏍仜閻ょ偓銇勮箛鎾跺闁抽攱甯掗湁闁挎繂娲ㄩ幗鍌炴煕閵堝棛鎳勯柟渚垮妼椤粓宕奸姀銏℃闂備焦瀵х换鍕磻閻愬樊鍤楅柛鏇ㄥ墰缁犻箖鏌熼悜妯诲鞍闁靛洦绻勯埀顒侇問閸犳洜鍒掑▎鎾扁偓浣割潨閳ь剟骞冮姀銈嗘啣闁稿本鑹惧В鎰攽閻樺灚鏆╁┑鐐╁亾濠电偘鍖犻崶褏锛欐繝鐢靛У閸╁啴鍩€椤掑﹦鐣电€规洖銈告俊鐑芥晝閳ь剟鎮甸弮鍫熲拺缂備焦蓱閻撱儵鏌熼懞銉х煂缂侇喚绮妶锝夊礃閳哄啫骞嶉梺璇叉捣閺佹悂鈥﹂崼銉ョ劦妞ゆ帊绀侀悘銉︺亜椤愶絿绠橀柟鐟板瀹曨偊宕熼锝庡悋闂傚倷鑳堕幊鎾诲触鐎ｎ喗鍋╂い蹇撶墕閸ㄥ倿鏌ゆ慨鎰偓妤佺▔瀹ュ憘鏃堟晲閸涱厽娈查梺缁樻尰濞茬喖寮诲澶婄厸濞达絿顭堢粻璇差渻閵堝懐绠伴柣妤€锕畷鎰板醇閺囩喓鍘撻梺鍛婄箓鐎氼剟寮抽悙鐑樼厱閻庯綆鍋嗘晶鐢告煛鐏炶濮傜€殿喗鎸抽幃娆徝圭€ｎ亙澹曢悷婊呭鐢帡鎮￠弴銏＄厵闁绘垶锚閻忕喖鏌嶈閸撴氨鏁幒妤嬬稏婵犻潧顑嗛弲鏌ユ煕濞戝崬澧い銉﹁壘閳规垿鎮╅崹顐ｆ瘎闂佺顑囨慨鐢电博閻旂厧閱囨繝闈涚墛濞堥箖姊哄Ч鍥х仼闁诲繑宀搁敐鐐哄即閵忥紕鍘介梺褰掑亰閸擄箓宕甸悢鍏肩厽闁绘棃顥撶粔娲煛鐏炵晫啸妞ぱ傜窔閺屾盯骞樼€靛摜鐣甸柧鑽ゅ仱閺屾洝绠涚€ｎ亖鍋撻弽顓炵厱闁硅揪闄勯崑鈩冪箾閸℃绠版い蹇婃櫇缁辨帡鍩€椤掑嫬绀冮柍鐟般仒缁ㄥ姊洪崫鍕悙婵☆偅顨婇獮濠囧箛椤撶姭鏋栭悗骞垮劚濡孩绂嶅鍫熺厸鐎广儱楠告禍婊兠归悪鍛暤闁哄矉缍侀獮妯兼崉閻戞鈧偓绻涢敐鍛悙闁挎洦浜妴浣糕槈濮楀棛鍙嗛梺鍛婁緱閸橀箖顢曟總鍛娾拻濞撴埃鍋撴繛浣冲厾娲Ω閳轰胶鐛ラ梺鍝勭▉閸樺ジ鎷戦悢鍏肩厓闁宠桨绀侀弳鐐测攽椤栨稒灏﹂柡灞剧☉閳规垿宕熼銏狀棟闂備焦瀵х换鍌炈囨导瀛樺亗婵炲棗绻堥埀顒佸笒椤繈顢楅崒娆掓濠电姷顣介埀顒€鍟块幃鎴︽煏閸パ冾伃妞ゃ垺娲熼弫鎰板炊閳哄啫甯ㄩ梺璇叉唉椤煤閺嵮呮殾妞ゆ帒鍟版禍娆撴⒒娓氣偓閳ь剛鍋涢懟顖涙櫠閹绢喗鐓ユ繝闈涚墕娴犺鲸銇勯姀鈩冪闁轰焦鍔欏畷鍫曞煛婵犲倹娅楅梻鍌欐祰椤曆兠归悜钘夋瀬闁归棿璁查埀顒婄畵瀹曞ジ濮€閳╁啯顔囬梻浣稿閸嬪懎煤濮椻偓閹繝鎮㈤梹鎰畾闂侀潧鐗嗙€氼噣宕濆澶嬪€堕煫鍥风到楠炴鏌?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婄闁挎繂妫Λ鍕⒑閸濆嫷鍎庣紒鑸靛哺瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁诡垎鍐ｆ寖缂備緡鍣崹鎶藉箲閵忕姭妲堥柕蹇曞Х椤撳搫鈹戦悙鍙夘棞缂佺粯甯楃粋鎺撱偅閸愨斁鎷虹紓浣割儏閻忔繈顢楅姀銈嗙厱闁靛鍎查崑銉р偓娈垮枛椤兘寮澶婄妞ゅ繐鎳庢刊浼存⒑閼姐倕孝婵炶绠掗妵鎰板礃閳哄倸寮块柟鍏肩暘閸斿秹鎮￠弴銏＄厪濠㈣埖鐩顕€鎮樿箛銉╂闁逛究鍔嶇换婵嬪川椤曞懍鍝楅梻浣告啞閻楁鎮ч悩宸殨闁圭虎鍠楅崐閿嬬箾閺夋埈鍎愰悗姘卞枎閳规垿鏁嶉崟顐℃澀闂佺锕ラ崹鍨暦濠靛棛鏆嗛柛鏇ㄥ墯濞呭洭姊洪棃娑氬闁绘绻戠粋宥夋焼瀹ュ棌鎷绘繛杈剧秬濞咃絿鏁☉銏＄厱闁靛鍎遍幃鎴︽煙娓氬灝濮傚┑陇鍩栧鍕節閸曨偄绠?闂?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柣妯荤垹閸ャ劎鍘遍柣蹇曞仜婢т粙鎮￠姘肩唵閻熸瑥瀚粈鈧梺瀹狀潐閸ㄥ潡銆佸▎鎾村剹妞ゆ劦鍋傜花濠氭煟閿濆洤鍘存い銏☆殜瀹曠喖顢曢姀鐘辨喚闂備浇顕уù鐑藉极閸濄儲鍏滈柛顐ｆ礀绾惧鏌熼幑鎰厫闁哥姴妫濋弻娑㈠即閵娿儱顫銈忓瘜閸犳牠鈥旈崘顔嘉ч幖绮光偓鑼泿婵＄偑鍊х粻鎾寸閸洖鏄ラ柍褜鍓氶妵鍕箳閹存繍浠惧┑鐐村毆閸ャ劎鍘梺鍓插亖閸╁嫰鎮為悙顑句簻闁哄浂浜炵粔顔锯偓瑙勬礀閵堢顕ｉ幘顔藉€锋繛鍫濈仢閹牓姊婚崒娆掑厡缂侇噮鍨堕弫瀣⒑鐠囪尙绠查柤瑙勫礃椤ｉ箖鏌熼崗鑲╂殬闁搞劏宕靛褔鍩€椤掑嫭鈷戞慨鐟版搐閻忓弶绻涙担鍐插椤╃兘鏌ㄩ弴鐐测偓褰掓偂濞嗘垹纾奸悗锝庡亽閸庛儵鏌ｉ幘鐟颁汗闁逞屽墲椤煤濡警娓婚柦妯侯槺閺嗭附鎱ㄥ璇蹭壕濡炪們鍨洪悷鈺呭箠閺嶎厼鐓涢悗锝庡墮閼板潡姊婚崒姘偓鐑芥嚄閸洖绠犻柟鎯х摠濞呯娀鏌ｉ敐鍛板闁搞劍绻冮妵鍕冀椤愵澀娌梺绋款儏閸婂湱鎹㈠☉銏犲耿婵☆垰鎼～鍥ㄧ箾鐎涙鐭嬬紒顔芥崌瀵鏁撻悩鑼紲濠电娀娼уú銊╁Υ婵犲啰绠鹃悗鐢殿焾琚ㄩ梺鎼炲妼椤兘鐛崘顕呮晩缁炬媽椴搁崓闈涱渻閵堝棗鐏卞┑顔哄€濆顒勫焵椤掑嫭鈷掑ù锝堟鐢盯鏌涢弮鈧悷銉┾€﹂崶顏嶆Ь闂佽桨绶￠崳锝呯暦濠婂嫭濯撮柧蹇曟缁辫尙绱撻崒姘偓鐑芥倿閿曞倹鏅濇い鎰堕檮閸も偓闁荤姵浜介崝澶愬磻閹捐埖鍠嗛柛鏇ㄥ墰椤︺劌鈹戦敍鍕粧缂侇喗鐟ラ锝夘敃閿曗偓缁犺崵绱撴担濮戭亝绂嶈ぐ鎺撯拺闁兼祴鏂侀幏锟犳煕閹惧崬濡挎俊鍙夊姍瀹曪絾寰勫畝鈧惁鍫ユ⒑濮瑰洤鐏叉繛浣冲嫮顩烽柨鏇炲€归悡鏇㈡煏婵炲灝鍔ら柛鈺嬬秮閺岀喖顢欓幆褍骞嬪銈冨灪閻╊垶骞冨▎鎾村殤缂傚倸瀛╁ú鐔奉潖閾忓湱鐭欓悹鎭掑妿娴煎洭姊虹粙娆惧剳闁稿鍊濆畷鍝勨槈閵忕姷顔婂┑掳鍊撻懗鍫曞储闁秵鐓熼煫鍥ㄦ礀娴犳粌顭胯缁瑩鐛繝鍥у窛妞ゆ棁妗ㄧ花濠氭⒑閹稿孩鐓ラ柕鍫㈩焾椤﹪顢欓崜褏锛滅紓鍌欑劍椤洨绮婚悙鐑樼厱闁宠鍎虫禍鐐繆閻愵亜鈧牜鏁幒鏂哄亾濮樼厧寮柟顔惧仱閹虫牠鍩為幆褌澹曢柣鐔哥懃鐎氼厾浜搁鍫熺厱閹兼番鍔嬮幉楣冩寠濠靛鐓熼柕蹇曞У閹插摜鈧鎸风欢姘跺蓟濞戙垹唯妞ゆ梻鍘ч埛鎺楁⒑鏉炴壆顦﹂悗姘緲椤繘宕崟銊︾€婚梺璇″瀻閸涱剙鎽嬫繝鐢靛仜閻°劎鍒掗幘鍓佷笉闁哄稁鍘奸拑鐔兼煟閺傚灝鎮戦柛濠勭帛閹便劌螖閳ь剙螞濞嗘挻鍋╅柛顐ｆ礃閳锋垿鏌涢敂璇插季闁衡偓娴煎瓨鐓曢柍鍝勫€绘晶闈涒攽閿涘嫭鏆€规洜鍠栭、娑橆潩妲屾牕鏅梻浣筋嚙濞寸兘寮崫銉﹀厹閻犺桨缍嶉敐澶婇唶闁绘棁娅ｉ鏇㈡煟鎼淬垻鈯曟い顓炴喘閹矂骞掗弮鍌滐紲闂佺粯锚绾绢厽鏅堕鍛簻妞ゆ劑鍨荤粻鑼磼缂佹绠撴い顐ｇ箞椤㈡宕熼锝嗘瘑闂傚倸鍊烽懗鍓佸垝椤栨凹娼栭悷娆忓椤╂彃霉閻樺樊鍎忛柣鎺戠仛閵囧嫰骞掑鍥舵М濠电偞鎯岄崳锝夊蓟閳ュ磭鏆嗛悗锝庡墰琚︽俊銈囧Х閸嬬偟鏁敓鐘靛祦閻庯綆鍠楅崐鐑芥煛婢跺鐏﹂柣搴ㄧ畺濮婂宕掑▎鎴犵崲闂侀€炲苯澧伴柛瀣洴閹崇喖顢涘☉娆愮彿闂佸憡鐟ラˇ顓熺濠婂嫨浜滈柟鎵虫櫅閻掔儤淇婇崣澶嬪€愰柟顕嗙節婵″爼宕遍弴鐘电暰婵＄偑鍊栭悧妤呮嚌妤ｅ啫绠栭柟瀵稿Л閸嬫挾鎲撮崟顒傤槰闂佺粯鎼换婵嗩嚕鐠囨祴妲堥柕蹇婃櫆閺呮繈姊洪棃娑氬闁瑰啿绻橀幃妤呮惞閸︻厾锛濇繛杈剧悼閻℃棃宕靛▎鎰箚妞ゆ劧绲跨粻鐐碘偓娈垮枛椤兘骞冮姀銈呯閻忓繑鐗楃€氫粙姊虹拠鏌ュ弰婵炰匠鍕彾濠电姴浼ｉ敐澶樻晪闁逞屽墴瀵鈽夐姀鈥斥偓濠氭煕閳╁喚娈樺ù鐘殿焾閳规垿顢欑涵閿嬫暰濠碉紕鍋樼划娆撴偘椤曗偓楠炲洭顢橀悢宄板Τ闂備線娼уù姘熆娴ｇ儤顫曢柍鍝勬噺閳锋帒霉閿濆嫯顒熼柣鎺斿亾閵囧嫰骞嬪┑鍥舵＆濡ょ姷鍋為崹鍨暦閸洦鏁嗛柍褜鍓熼幏鎴︽偄閸濄儳顔曢梺鐟扮摠閻熴儵鎮橀埡鍐＜闁绘ê妯婇悡濂告煛瀹€鈧崰鎰版晬閹邦厽濯撮柦妯侯槸鐢垶姊绘担鍛婂暈闁煎綊绠栭、鏍川閺夋垹鍘撮梺纭呮彧鐎靛矂寮繝鍕／妞ゆ挾鍋涘Σ璇裁归崗鐓庡婵﹥妞藉畷鐑筋敇濞戞瑥鐝遍梻浣侯焾椤戝懎螞濠靛棛鏆﹂柨婵嗩槸瀹告繃銇勯弽銊х煂闁挎稒绻堝铏圭矙閹稿孩鎷遍梺鑽ゅ暀閸パ咁槷閻庡箍鍎遍ˇ浼存偂閵夛附鍙忔俊顖氱仢閻撴劙鏌ｉ幘鍐叉殻闁诡喖缍婂畷鍫曨敂閸曨厽顕楃紓鍌欒兌婵鍒掗鐐茬疄闁靛濡囩弧鈧梺鍛婂姀閺傚倹绂掗姀銈嗗€甸悷娆忓绾炬悂鏌涢弬璺ㄐら柟骞垮灩閳规垹鈧綆浜為ˇ銊ヮ渻閵堝懐绠伴悗姘煎墮鏁堥柡灞诲劜閳锋帡鏌涚仦鍓ф噮闁告柨绉归幃妤冪箔濞戞ê骞樻繛宸簼閺呮粓鏌ｉ敐鍛劸婵炶尙顭堥—鍐Χ閸涱垳顔囬柣搴㈠嚬閸ｏ絽顕ｉ幎鑺ユ櫆闁伙絽鐬奸鏇㈡倵閻熸澘顥忛柛鐘虫礈閼鸿鲸绻濆顓犲幗闂佸湱鍎ら崹瑙勭濞戙垺鐓忛柛銉戝喚浼冨Δ鐘靛仦鐢帡顢橀崗鐓庣窞閻庯絿顣介崑鎾诲冀閵娧咁啎闁诲孩绋掗…鍥儗鐎ｎ剛纾兼い鏃囧Г瀹曞瞼鈧鍠栭…鐑藉箖閵忋倖鍋傞幖瀛樼箘閻愬﹪姊绘担鍛婂暈婵炶绠撳畷婊堟晝閳ь剝鐏嬮梺缁樺姇椤曨厾绮绘ィ鍐╁€垫繛鎴炵懐閻掍粙鏌ｉ鐔风缂佽鲸甯￠幃鈺佺暦閸パ€鍚傛俊鐐€ら崑鍕崲濮椻偓楠炴牞銇愰幒鎴炲祶濡炪倖鎸炬慨鐑藉储椤栫偞鈷掑ù锝呮啞閸熺偤鏌ｅΔ浣哥伌闁糕斁鍋撳銈嗗坊閸嬫捇鏌涘Ο鑽ゅ⒈婵″弶鍔欓弫鎰板炊閳哄喛绱抽梻浣侯焾閺堫剟鎳濇ィ鍐ㄧ劦妞ゆ巻鍋撻柟鑺ョ矌閸掓帡寮崼婵堫啋濡炪倖妫佹慨銈呪枍閸パ屾富闁靛牆鎳愮粻浼存煟濡も偓缁绘劙鎮鹃崹顐ょ懝闁逞屽墴瀵鈽夐姀鐘电杸闂傚倸鐗婄粙鎺楁倶閸儲鐓曢柣鎴濇閻忥附鎱ㄦ繝鍐┿仢鐎规洘锕㈠畷锝嗗緞鐎ｎ亜澹嶉梻鍌欒兌椤牓鏁冮敃鍌氱疇闁规崘顕ч悡婵堚偓骞垮劚閹峰鎮￠妷鈺傜厱妞ゆ劑鍊曢弳閬嶆煙闁垮銇濇慨濠呮濞戠敻宕ㄩ婵囩潖闂備胶顭堥鍡涙儎椤栨氨鏆︽繝闈涙－濞笺劑鏌嶈閸撶喎顕ｇ拠娴嬫闁靛繒濮甸ˉ婵嬫煟閻樺厖鑸柛鏂块叄瀹曟繈顢涘鍏煎瘜闂侀潧鐗嗛崯顐︽倶椤忓牊鐓欓柛娑橈攻閸婃劙鏌涢埞鍨姕鐎垫澘瀚伴獮鍥敆婢跺绉遍梻鍌欑劍婵炲﹪寮鈧畷鎴﹀箻缂佹鍘甸梺姹囧€ら崹閬嶎敂閻樼數纾奸弶鍫涘妽瀹曞瞼鈧娲樼敮鎺楀煝鎼淬劌绠ｆ繝闈涙－閻庢挳姊婚崒娆戝妽閻庣瑳鍏犲搫顓兼径濠勬煣濠电偞鍨崹褰掓偂濠靛鐓欓柟瑙勫姦閸ゆ瑧绱掗銏⑿ч柡灞剧洴椤㈡洟濡堕崨顔句簴闂備礁鎲￠…鍫澪涢崟顖氱厴闁瑰鍋涚粻鐘绘⒑缁嬪尅鏀婚柛鏃€鐟ラ悾鐑藉箣閿曗偓缁犺崵绱撴担璇＄劷闁告瑥妫濆娲传閸曨剙鍋嶉梺鍛婃煥椤戝寮婚崶顒夋晬婵炴垼椴搁鏃堟⒑缂佹ê濮堢憸鏉垮暣瀵啿鈻庨幘瀛樺殙闂佸搫绋侀崢浠嬫偂閺囩喓绡€濠电偞鍎虫禍楣冩⒑缂佹ê閲滅紒鐘虫尭椤曪綁寮婚妷銉у幐婵炶揪绲藉鍫曞棘閳?data\.warp-enabled闂?
//   闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐差渻閵堝棗绗掓い锔垮嵆瀵煡顢旈崼鐔蜂画濠电姴锕ら崯鎵不婵犳碍鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍暜婵犲洦鍤勯柛顐ｆ礀閻撴繈鏌熼崜褏甯涢柣鎾寸洴閺屾稑鈽夐崡鐐寸亾缂備胶濮甸敃銏ゅ蓟濞戙垹绠抽柟鎯х－閻熴劑姊虹€圭媭鍤欓梺甯秮閻涱喖螣閾忚娈鹃梺鎼炲劥濞夋盯寮挊澶嗘斀闁绘灏欏Λ鍕煛婢跺﹦姘ㄩ柛瀣崌楠炴绱掑Ο琛″亾閸ф鐓ラ柡鍥殔娴滈箖姊虹€圭媭鍤欑紒澶屾嚀椤曪綁顢楁担鐟板妳闂佽鍎抽悺銊╁级閹间焦鈷掑ù锝堟鐢盯鏌ㄥ鑸电厽闊洦鏌ㄩ崫铏光偓娈垮枟婵炲﹪宕洪埄鍐╁闁告瑥顦伴弶鍛婁繆閻愵亜鈧牕煤閺嶎厼绠犻幖绮规閸ゆ洘銇勯弮鍫熸殰闁?  -> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗娼欒灃闁逞屽墯缁傚秹骞嗚濞撳鏌曢崼婵堝嚬缂併劌顭烽弻娑㈡偄閸涘﹦绋囬梺浼欑悼閺屽濡甸幇鏉跨婵犻潧娲﹀▍鏃堟⒒娴ｅ憡鍟炲〒姘殜瀹曞綊鎮ч崼鐔峰伎濠电偛妯婃禍婵嬪煕閹烘嚚褰掓晲閸曨噮鍔呭┑鐐插悑鐢繝寮诲☉銏犲嵆闁绘劕鍚€缁敻姊烘导娆戝埌濠⒀冩捣濡叉劙骞樼€涙ê顎撻梺鍦帛鐢﹥绔熼弴鐐╂斀闁绘﹩鍠栭悘杈ㄧ箾婢跺鈯曠€垫澘瀚板畷鍫曨敆閳ь剛澹曟繝姘厵闁硅鍔曢崥鍦偓瑙勬尫缁舵岸寮诲☉鈶┾偓锕傚箣濠靛洨浜┑鐘愁問閸犳牗顨ラ幖浣测偓鏃堝礃椤斿槈褔鐓崶銊﹀暗婵¤缍佸娲箹閻愭彃顫呭┑鐐差槹閻╊垶銆佸鑸垫櫜闁糕剝顨忛崵銈夋⒑閸濆嫷妲归柛銈忕畵婵℃悂鍩￠崒婊冨汲闂備礁鎼崐鎼佹倶濠靛鐓曢柡鍐ｅ亾闁靛洤瀚板顕€鍩€椤掆偓鐓ら柣鏂款殠濞兼牗绻涘顔荤凹闁稿绻濆娲敇閵娧呮殸濠电偛鐗嗛悥鐓庮潖閾忓湱鐭欐繛鍡樺劤閸撴娊姊洪崫銉バｉ柣妤冨У娣囧﹪骞橀鑲╋紲闂佺粯鍔樼亸顏堝箺閺囥垺鈷戦梻鍫熶緱濡狙冣攽閳ヨ櫕鍠樼€规洏鍨介獮鏍ㄦ媴閸忓瀚肩紓鍌欑贰閸ㄥ崬煤閺嶃劍娅犻柛娆忣槶娴滄粍銇勯幘璺轰沪闁哥姵锕㈤弻鐔碱敍濮樿京鍔Δ鐘靛仦鐢帟鐏冩繛杈剧到閹碱偊鐛鍡曠箚闁绘劦浜滈埀顒佺墪椤繗銇愰幒鎴濆殤婵犵數濮电喊宥夊吹閳ь剟姊虹粔鍡楀濞堟洟鏌ｉ幘瀵告噭闁靛洤瀚板顕€宕掑☉娆戝涧闂備胶鎳撻崯鍨洪敂鐣屸攳濠电姴娴傞弫鍐煏韫囨洖校婵炲牜鍙冨铏光偓鍦У椤ュ銇勯敂璇茬仴闁诲繐鍟村娲焻閻愯尪瀚板褎鎸抽幐濠囨偄閸涘﹤寮垮┑鈽嗗灠閵囨﹢宕ラ銈囨／?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟扮粻濠氭煕閳规儳浜炬俊鐐€栫敮濠囨嚄閸洖鐓濋柟鍓х帛閻撴盯鏌涘☉鍗炵仩闁宠鐗撻弻鏇㈠幢閺囩媭妲梺瀹狀嚙闁帮綁鐛鈧畷姗€骞撻幒鎾存婵犵绱曢崑鎴﹀磹閺嶎厼绠板Δ锝呭暙绾惧鏌ｉ弬鍨倯闁稿锕㈤弻鏇熷緞閸℃ɑ鐝曢梺鎶芥敱閸ㄥ潡寮诲☉銏℃櫆閻犲洦褰冪粻褰掓⒑閸涘﹥鐓ユい鎴濇婵＄敻宕熼姣尖晠鏌曟径娑氱暠婵炲憞鍕瘈闁靛骏缍嗗鎰箾閸欏鐒介柟骞垮灩閳规垿宕遍埡鍌氬厞婵＄偑鍊栭幐鐐叏閹绢喚宓佺€广儱顦伴埛鎺懨归敐鍛暈闁哥喓鍋ら弻鐔虹矙閹稿寒妫﹂悗瑙勬磸閸旀垿銆佸鈧幃顏堝焵椤掑嫬纾荤憸鏂款潖婵犳艾纾兼繛鍡樺笒閸橈紕绱撴笟鍥ф珮闁搞劌鐖奸悰顕€骞囬婊冧簼闂佸憡鍔戦崝搴ㄦ偘閵夆晜鈷戦柛锔诲幖椤ｅ吋绻濋姀鈭╂垿銆佹繝鍥ㄢ拻濞达絽鎲￠崯鐐寸箾鐠囇呯暤鐎规洖缍婇獮宥夘敊閸欍儳鐟濇繝鐢靛仦閸ㄥ爼鎮烽敐鍡欘洸婵犲﹤鐗婇悡娑㈡煕鐏炵偓鐨戝ù鐘灲閺岀喖顢欓挊澶屼紝闂佸搫鐭夌徊楣冨箚閺冨牊鏅查柛銉戝拋鍋х紓鍌氬€烽懗鍓佸垝椤栨粎鐭欓柟鎹愵嚙閻掑灚銇勯幒宥堝厡闁愁垱娲橀妵鍕敇閻愭潙鏋犻梺璇″灠閻倿鐛幒妤€绫嶉柛灞剧玻缁辨煡姊绘担铏瑰笡闁挎氨鐥紒銏犲箺闁哄懎鐖奸、鏃堝川椤愶紕鐩庢俊鐐€栭崝鎴﹀垂閼姐倗涓嶅ù鐓庣摠閸嬪倿鏌曢崼婵囶棑濞存粍绮撻悡顐﹀炊閵婏箑纰嶅銈呮禋閸嬪懘濡甸崟顖氭闁圭楠搁弸鐘绘⒑閸濆嫮鐏遍柛鐘崇墵楠炲﹤鈹戠€ｎ偄浜遍梺鍓插亝缁诲啴鏌ㄩ鐘电＝闁稿本鐟ч崝宥囨喐閺夊灝鏆ｆ鐐寸墵椤㈡洟濮€閻樼偣鍋掗梻鍌氬€风粈浣圭珶婵犲洤纾诲〒姘ｅ亾鐎规洘娲熷濠氬Ψ閵壯冨箞闂備胶鎳撻顓熸叏妞嬪骸顥氶柦妯侯棦瑜版帗鏅查柛銉厛娴犙勭箾閹惧顣茬紒瀣笧閹广垹鈽夐姀鐘诲敹闂佺粯鏌ㄩ幗婊埶囨导瀛樷拺闁告繂瀚悘閬嶆煕閻樺磭澧垫鐐插暣閹粓鎳為妷銉ょ綍闂備礁澹婇崑鍡涘窗閹版澘纾归柣鎴ｅГ閳锋帒霉閿濆妫戝☉鎾瑰皺缁辨帡骞撻幒鎾充淮閻庤娲橀〃鍛达綖濠婂牆鐒垫い鎺戝閸嬫ɑ銇勯弮鍥撻柛搴ｅ枛閺屾洘绻濊箛鏇犳殸闂佽皫鍐仾缂佺粯绻堟慨鈧柨婵嗘閵嗘劙姊哄ú璇插箹闁绘鎸搁悾鐑筋敍閻愯尙鍊為悷婊冾樀瀵劍绂掔€ｎ偆鍘介梺褰掑亰閸ㄤ即鎮￠幇顔藉枑闁硅泛锕ら幊鎰婵傚憡鐓涢悘鐐额嚙閸斻倝鏌ｉ敐鍡樸仢闁哄瞼鍠栭、娆撴嚍閵夛絼铏庨柣搴㈩問閸犳骞愰幎钘夋槬闁靛鏅涢獮銏′繆椤栨繂鍚圭紓鍫ｎ嚙閳规垿鎮╅崹顐ｆ瘎婵犳鍠氶崗姗€濡撮崒鐑嗘晩闁绘劦鍓﹀鐔兼⒑閸濆嫭鍌ㄩ柛銊ユ贡婢规洘绻濆顓犲幍闁哄鐗嗘晶浠嬫偩鏉堚晝妫柟顖嗗懐楔闂佸搫澶囬埀顒佸墯閸氬骞栫划鍏夊亾瀹曞浂鍟堥梻鍌欒兌椤牓顢栭崨鎼晞闁糕剝顭囬埞宥呪攽閻樺弶鎼愰崶鎾⒑閸涘﹣绶遍柛妯挎閳绘捇顢橀姀鈾€鎷洪梺鍛婄箓鐎氱兘宕曡箛娑欑厽婵°倐鍋撶紒缁橈耿瀹曟椽鍩€椤掍降浜滈柟鍝勬娴滃墽绱撴担鍝勑㈢紒澶屾嚀閻ｇ兘寮撮姀锛勫姸閻庤鎸堕崕鍗灻洪顫偓浣糕槈濮楀棛鍙嗛梺鍛婂姉閸嬫捇藟閿熺姵鐓熼柣鏂挎憸閻苯顭胯椤ㄥ牓寮鍢夋棃宕ㄩ鐘插箞闂備礁婀遍崕銈夈€冮崱娑欏亗闁哄洨鍠嗘禍婊堟煙閹佃櫕娅呴柍褜鍓氬ú鐔奉嚕缁嬪簱鏋庨柟鎯ь嚟閸橀亶鏌熼崗鑲╂殬闁搞劏顫夐幈銊ヮ吋閸♀晜顔旈梺缁樺姇椤曨厽鏅堕鍫熷殨闁归偊鍘规禍婊堟煙閸濆嫭顥滃ù婊呭亾缁绘繄鍠婂Ο宄颁壕闁绘劘妫勬禒顕€姊洪崫鍕効缂傚秳绶氶悰顕€宕堕澶嬫櫌婵犵數濮撮幊搴ㄥ箚閸儲鈷掑ù锝呮啞閹牓鏌￠崼顐㈠閻撱倖淇婇姘倯鐎规洖寮剁换娑㈠箣閻愮數鐓犲┑鈩冨絻椤嘲顫忛搹瑙勫厹闁告侗鍠氳ぐ褔姊洪崨濠冣拹闁荤喆鍎甸、姘舵晲婢跺﹦顔呯紓浣圭☉椤戝棝寮插┑瀣拺闁圭瀛╅ˉ鍡欐偖濞嗘挻鐓欓梺鍨儐閵囨繈鏌″畝瀣М闁诡喒鏅犲畷锝嗗緞婵犲孩袩闂傚倷鑳剁划顖炲箹椤愩倖宕叉繝闈涱儍閳ь兛绶氶獮妯侯熆閸曨剚顥堢€规洏鍔戦、娆撳箚瑜嶇粻娲⒒閸屾凹鐓柛瀣鐓ら柨鏇楀亾閾荤偛霉閿濆懏璐￠柣婵婂煐閹便劌顪冪拠韫闂備胶顢婂Λ鍕偉婵傜绠栨繛鍡樻尰閸婄粯淇婇婊冨付妤犵偛鐗撳缁樼瑹閳ь剟鍩€椤掍胶鈽夌€规挸妫欑换娑欐媴閸愬弶鎼愰柛鎴犲У缁绘盯骞嬪▎蹇曚痪闂?
//   濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婇悞锕傚磿閹剧粯鈷戦柟鑲╁仜婵″ジ鏌涙繝鍌涘仴鐎殿喛顕ч埥澶愬閳哄倹娅囬梻浣瑰缁诲倸螞濞戔懞鍥Ψ瑜忕壕钘壝归敐鍛儓鐏忓繘姊洪崨濠庢畷濠电偛锕ら锝囨嫚濞村顫嶅┑鈽嗗灦閺€閬嶅棘閳ь剟姊绘担鍛婂暈婵炶绠撳畷鎴﹀礋椤掍礁寮块梺闈涚箞閸婃牠鍩涢幋鐐电闁煎ジ顤傞崵娆愵殽閻愭惌娈滈柡宀€鍠栭幃鐑芥偋閸繃鐏庨柣搴㈩問閸犳牠鈥﹂悜钘夌畺闁靛繈鍊栭幆鐐烘煕閿旇寮跨紒杈ㄧ叀濮婄粯绗熼埀顒€顭囪閹广垽骞掗幘鏉戝伎闂佹眹鍨归幉锟犲磹閸撲讲鍋撻獮鍨姎妞わ缚鍗抽崺娑㈠箣閻樼數锛滈柣搴秵閸嬫帡宕曢妷鈺傜厱閹艰揪绱曟晥闂佸搫鏈粙鎺旀崲濠靛绀冩い蹇撴閺変粙鏌ｆ惔銏╁晱闁哥姵鐗犻垾锕€鐣￠柇锕€娈ㄩ梺鍦檸閸犳宕戦崟顖涚叄闊洦鍑归崵鍐煕閻旂兘鍙勬慨濠勭帛閹峰懘鎼归悷鎵偧闂備礁鎲″Λ鎴犵不閹惧磭鏆︽繝濠傚枤濞尖晠鏌ら幖浣规锭濞寸姍鍥ㄢ拺闁告繂瀚弳娆撴煟濡も偓閿曨亪宕?-> 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊閻樺樊妫岄梺杞扮閿曨亪寮婚垾鎰佸悑閹肩补鈧磭顔愮紓鍌欑劍閸旀牠銆冮崱妯尖攳濠电姴娲ゅ洿闂佸憡渚楅崢钘夆枔閺屻儲鈷戦柛婵嗗椤ョ偟绱掗鑲┬ｇ紒宀冮哺缁绘繈宕堕‖顑洦鐓曟繛鎴濆船楠炴绻涢崼婵堝煟婵﹤鎼叅閻犲洦褰冪粻娲⒑閼姐倕鏆遍柡鍛Т椤曪絾绻濆顓炰簻闂佺绻掗崢褔鍩€椤掑倻甯涘ǎ鍥э躬椤㈡盯鎮欑€涙褰哥紓鍌欑贰閸犳牠鎮ラ悡搴綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顦插褏鏅Σ鎰板箳濡や礁浜归梺鎯ф禋閸嬪嫮鍠婂澶嬧拺濞村吋鐟х粔顕€鏌￠埀顒佹綇閳哄啰澶勯梺浼欑到閻ジ鎯屽▎鎾村仯闁搞儻绠戠€氬酣鏌涢弬璺ㄐу┑锛勬暬瀹曠喖顢涘杈╂綁闂備焦鎮堕崕娲倶濠靛绠婚悗闈涙憸椤旀洟姊洪崨濠佺繁闁告ɑ鍎抽悺顓㈡⒒娴ｅ摜鏋冩い顐㈩樀瀹曞綊宕稿Δ鈧粻鏍煃閸濆嫭鍣归柛銊ュ€归妵鍕箛閸撲胶蓱闂佷紮绠戞鎼佸煘閹达附鍋愰柟棰佺閺呴亶姊洪崫銉バｇ€光偓缁嬫鍤曟い鎰剁悼缁♀偓濠殿喗锕╅崜娑㈡偪閸パ€鏀介柣妯肩帛濞懷勩亜閹存繃鍣界紒顔肩墦瀹曞ジ濮€閳ヨ櫕鐎鹃梻濠庡亜濞诧妇绮欓幒妤€纾挎俊銈呮噺閻撴洟鏌嶉埡浣告殶闁愁垱娲熼弻锝夊箻鐎涙顦梺閫炲苯澧柛妯荤矒瀹曟垿骞樼紒妯煎帗閻熸粍绮撳畷婊堟偄妞嬪孩娈鹃梺纭呮彧闂勫嫰宕愰悜鑺ョ厽闁瑰浼濋鍛洸婵°倐鍋撻柍瑙勫灴閹瑩宕ｆ径妯伙紒闂備礁婀遍…鍫モ€﹂悜钘夌畺闁炽儲鏋奸弸搴ㄦ煙閹佃櫕娅冪紒銊ヮ煼濮婃椽宕崟顓夌娀鏌涢弬鍧楀弰闁糕晛锕鎾偐閻㈢绱查梻浣筋嚃閸ㄥ崬顭垮鈧幆鍐洪鍛幍濡炪倖娲栭幊搴ㄥ几濞戞瑣浜滄い鎰剁悼缁犵偤鏌熼搹顐ょ煂闁哥姴锕よ灒閻犲洤妯婃导鏍⒒閸屾艾鈧绮堟担闈╄€块梺顒€绉埀顒婄畵瀹曠厧顭垮┑鍡欑Ш闁诡喗绮撻幐濠冨緞鐏炶棄绗岄梻鍌欑閹碱偄煤閵娾晛绐楃€广儱娲﹀畷鏌ユ煕閳╁叇婊勭濠婂牊鐓欓柟浣冩珪濞呭懘鏌ｈ箛濠冩珚闁哄矉绻濆畷閬嶎敇閻樺啿浠归梻渚€鈧偛鑻晶顖涖亜閺冣偓閻楃姴鐣锋导鏉戝唨妞ゆ挻绋堥崑鎾绘晝閸屾稓鍙嗛柣搴岛閺呮繄绮诲鑸碘拺鐟滅増甯楅敍鐔虹磼閳ь剚鎷呴崨濠冩闂傚倸鍊风欢姘焽瑜忛幑銏ゅ幢濡烆澁缍佸畷銊╊敇瑜忛崝鐑芥⒑閸濆嫬鏆婇柛瀣尵缁辨帞绱掑Ο鑲╃暭闂佸摜濮撮敃銈堢亽濠电偠灏欑划顖滅礊婢舵劖鈷掑ù锝堫潐閸嬬娀鏌涢弬璺ㄐｇ紒缁樼⊕缁绘繈宕掑鍕啎闂備線娼ф蹇曠礊閸℃あ鐔煎礋椤栨稈鎷洪梻鍌氱墛缁嬫帡藟濠婂牊鐓曞┑鐘插亞閻撳吋顨ラ悙宸█妤犵偞锕㈤、娆撴嚃閳哄﹥效濠碉紕鍋戦崐鏇犳崲閹邦儵娑樷槈閵忊€充患婵°倧绲介崯顖炲煕閹烘垯鈧帒顫濋敐鍛婵犵數鍋橀崠鐘诲炊閵娿儰缃曢梻浣虹《閸撴繈藝閸濆娊娲敂閸曞汞鍥ㄧ厱婵炴垵宕悘锟犳煙閸愭彃鏆炵紒缁樼箞閹粙妫冨ù韬插劦閺屸剝鎷呯憴鍕偓鎰偓瑙勬礃閸ㄧ敻鍩ユ径濞㈢喎顭ㄩ崨顓熺€剧紓浣虹帛缁诲牆鐣烽幆閭︽Щ闂佺顑呯粔鐢垫崲濠靛顥堟繛鎴炃氶崑鎾诲箹娴ｅ摜鐤呴梺璺ㄥ枔婵澹曟繝姘厓闁告繂瀚崳娲煟閹捐泛鏋涢柡宀€鍠撻埀顒傛暩椤牆鏆╅梻浣侯攰濡嫰宕愰崸妤€钃熼柍鈺佸暞婵绱掔€ｎ偄顕滄繛鍫濐煼濮婃椽宕崟顕呮蕉闂佸憡鏌ㄩ惌鍌炲春閻愬搫绠ｉ柨鏇楀亾妤犵偑鍨烘穱濠囶敍濠靛浂浠╅梺缁樼▓閺呯姴顫忕紒妯诲濞撴凹鍨遍弫顖氣攽閻愬弶鍣烘繛鍙夌矒楠炴垿濮€閵堝懘鍞跺┑鐘茬仛閸旀牜绱炴惔鈾€鏀介柣鎰级閳绘洖霉濠婂嫮绠為柟顔惧仦缁绘繂顫濋鐘插箺闂備礁缍婇崑濠囧垂娴煎瓨瀚婂┑鍌氭啞閸婂灚鎱ㄥ鍡楀箺缂佽泛寮堕妵鍕敇閳ュ啿濮峰銈忕畵娴滃爼寮诲☉妯滄梹鎷呯化鏇熺亞闁诲孩顔栭崰娑㈩敋瑜旈崺銉﹀緞閹邦剦娼婇梺缁樕戦鏍船濞差亝鈷掑ù锝囩摂濞兼劗鈧娲橀敋闁宠绉瑰鎾偐閸愯尙褰撮梻浣虹帛閸旀洖螣婵犲倶鈧帗绻濆顓犲帾闂佸壊鍋呯换鍌炲汲濞嗘挻鐓熼煫鍥风导缁ㄧ晫绱掓潏銊ョ闁逞屽墾缂嶅棝宕戦崟顖涘€堕柟缁㈠枟閻撳啴鏌曟径妯虹仯闁伙綁娼ч埞鎴﹀焺閸愨晛鈧劙鏌涢埞鎯у⒉闁瑰嘲鎳忕粭鐔碱敍濞村爢鍐ｆ斀闁挎稑瀚禍濂告煕婵犲啰澧电€规洘绻堥弫鍐磼濮樻唻绱遍梻渚€鈧偛鑻晶鎾煛鐏炲墽銆掗柍褜鍓ㄧ紞鍡涘磻閸涱垯鐒婃い鎾卞灪閻撳啴鏌﹀Ο渚▓婵☆垪鍋撳┑鐘愁問閸犳帡宕戦幘缁樷拺闁规儼濮ら弳婊堟煕鐎ｎ偅宕岄柡宀€鍠栭弻銊р偓锝庡亖娴犮垹鈹戦纭峰伐妞ゎ厼鍢查悾鐑藉箳閹搭厽鍍甸梺鍛婎殘閸庢劙寮堕崫銉х＝闁稿本鐟чˇ锔姐亜閹存繃顥㈡鐐村姈缁绘繂顫濋鍌涚叄闂備礁澹婇崑鍛洪弽顓炵闁规儼濮ら悡鐔兼煛閸愩劍澶勬い蹇曞У閵囧嫰寮埀顒€煤閻旂厧钃熼柨婵嗙墢閻も偓闂佸搫娲ㄩ崑妯煎垝閸洘鈷戠紒瀣皡閺€濠氭煙椤旂厧鈧灝鐣峰ú顏勭劦妞ゆ帊闄嶆禍婊堟煙閸濆嫮肖閻犳劗澧楃换娑㈠醇閻旇櫣鐓夐梺鍝勭灱閸犳牠銆佸▎鎾村€锋い鎺戝€告慨鑺ヤ繆閻愵亜鈧呯不閹烘缍栧鑸靛姇缁犵偤鏌曟繝蹇擃洭缂佲檧鍋撻梻浣告啞濞诧箓宕㈤懖鈺冪當闁跨喓濮甸埛鎴︽煟閻斿憡绶叉俊鎻掝煼閺岀喖鎼归銏狀潕闂佸憡甯楃敮锟犲箖閻ｅ苯鏋堟俊顖濇〃婢规洟姊洪幐搴ｇ畵缂併劏鍋愰懞杈ㄧ附閸涘﹦鍘介梺鎸庣箓鐎氼剙鐣甸崱妯诲弿濠电姴瀚敮娑㈡煙瀹勭増鍣虹紒妤冨枛椤㈡稑顭ㄩ崘銊愵亞绱撻崒姘偓鐑芥嚄閸撲礁鍨濇い鏍仜閸ㄥ倿鏌涢敂璇插箹闁搞劍绻冪换娑㈠幢濡纰嶇紓浣哄Т椤兘寮婚敓鐘茬＜婵☆垰鎼慨鏇㈡⒑鐠囨煡顎楁い顓炲槻椤繐煤椤忓拋妫冨┑鐐村灦濮樸劑鎮块崟顖涒拺缂佸灏呴弨缁樼箾閸欏澧悡銈夋煕瑜庨〃鍡涘煕閹烘嚚褰掓晲閸偅缍堥梺绋款儑婵炩偓闁哄本绋掗幆鏃堝鎺虫禒銏ゆ⒑鐠団€虫珯缂佺粯绻傞悾鐑芥偂鎼存ɑ鏂€闂佸綊鍋婇崰鏍ㄦ叏濞差亝鈷掑ù锝堟鐢盯鏌＄仦璇插鐎规洏鍎抽埀顒婄秵閸犳牠鎮￠弴銏＄厵闁绘垶锚閻忕喖鏌嶈閸撴氨鏁幒妤嬬稏婵犻潧顑愰弫鍡椕归敐鍫綈闁稿﹤鍢查埞鎴︽偐椤愶絽顎忛柣蹇曞仩婵倝鎮橀幘鍓佺＝濞达絼绮欓崫娲偨椤栨侗娈旀い鏇樺劦瀹曠喖顢楅崒婊呮闂佺鍋愮悰銉╁礈濠靛鏁婄€广儱顦伴埛鎺懨归敐鍥у妺闁搞倐鍋撻柣搴㈩問閸犳牠鎮ユ總鍝ュ祦闁圭増婢樼粻顕€鏌﹀Ο渚Ч闁诲寒鍓熷铏圭矙閹稿孩鎷辩紓浣割儐鐢€崇暦閿濆牏鐤€婵炴垶鐟ч崢鐢告⒒娓氬洤寮跨紓鍌涙皑閼鸿鲸绂掔€ｎ偆鍘介梺瀹犳〃缁舵岸宕ｉ崟顒佸弿濠电姴瀚敮娑㈡煙瀹勭増鍤囩€规洏鍔嶇换婵嬪礃閵娿儱顥掔紓鍌氬€搁崐椋庣矆娓氣偓钘濆ù鐓庣摠閸嬪倿鏌嶉妷銉э紞闁哄棴闄勭换婵嬫濞戝啿濮涢梺姹囧€愰崑鎾绘⒒娴ｅ憡鎯堥柛鐕佸亰瀹曟劙寮拌箛瀣╂睏闂佽鍨庨埀顒勫绩娴犲鐓冮柦妯侯槹椤ユ粓鏌ｈ箛鏇炩枅闁哄本绋戦～婵嬵敆婢跺﹤澹庨柣搴ゎ潐濞叉﹢宕归崹顕呭殨濞寸姴顑愰弫鍥煟閺傛寧鎯堟鐐搭殜濮婄粯鎷呴崨濠冨創闂佺锕ラ幃鍌炲箖濡　鏀介悗锝庝簽閻ｆ椽姊虹涵鍛涧闂傚嫬瀚伴幃锟犲礃椤旇В鎷洪梺鍓茬厛閸ｎ噣宕曡箛娑欑厽闊洦姊圭亸锕傛煛瀹€瀣？濞寸媴绠撳畷婊嗩槷婵℃彃鐗撳铏光偓鍦濞兼劙鏌涢妸銉т虎闁伙絿鍏樺畷锟犳倷瀹ュ棙鈷愮紒缁樼箖閹峰懘鎮锋０浣哄嚬闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠煑閼恒儳鈽夐柍钘夘樀婵偓闁绘顣槐閬嶆⒒娓氣偓濞佳囨偋閸℃蛋鍥敍閻愭潙鐝旈梺缁樻煥閹芥粎绮绘ィ鍐╃厵閻庣數顭堟禒褔鏌熼崘鎻掝伃闁哄本绋掗幆鏃堟晲閸涱喚浜梻浣告惈閺堫剙煤閻旈鏆﹂柣鎾崇岸閺€浠嬫煕閵夋垵瀚锋禒褍鈹戦悩鎰佸晱闁哥姵鐩敐鐐村緞閹扳斁鍋撻崘鈺冪瘈闁稿被鍊栫紞搴♀攽閻愬弶鈻曞ù婊勭箞閹偞绻濆顓犲幗闂佸綊鍋婇崹浼存嫊婵傚憡鍊垫慨姗嗗幗缁跺弶銇勯鈥冲姷妞ぱ傜窔閺屾盯濡搁妷褍鐓熼悗娈垮枦椤曆囧煡婢跺á鐔兼煥鐎ｎ偅婢戦梻鍌欒兌绾爼宕滃┑鍫㈢煓闁瑰瓨绻嶉崵鏇灻归悡搴ｆ憼闁稿﹦鏁婚幃宄扳枎韫囨搩浠剧紓浣插亾闁逞屽墰缁辨捇宕掑姣欍垽鏌ㄩ弴銊ら偗闁绘侗鍣ｉ獮鍥敇閻樼數妲囬梻浣告啞閸旀垿宕濇惔锝囩幓闁绘劗鍎ら悡鐔肩叓閸ャ劍绀€濞寸姍鍥ㄧ厱閻庯綆鍋呭畷灞俱亜閵忥紕鎳囩€殿喗鎸虫慨鈧柍閿亾闁归绮换娑欐綇閸撗勫仹闂佺娅曢幑鍥€佸顒夌叆闁告侗鍨抽敍婊堟煟閻樺弶澶勭憸鏉垮暣閸┾偓妞ゆ帒鍟崝鍨繆椤愩倕鏋庢い顏勫暣婵″爼宕卞▎蹇ｆ椒缂傚倷绶￠崰鏍偓娑掓櫇缁瑦寰勬繝搴℃倯婵犮垼娉涢鍥矗閸℃せ鏀介柣妯肩帛濞懷勪繆椤愵偄骞栭崡杈╂喐閻楀牆绗氶柣鎾跺枛閺岀喖骞戦幇顓犮€愰梺鍝勵儐閸ㄤ絻褰侀梺鎼炲劀瀹ュ牆鎯堟俊鐐€戦崹铏圭矙閹达讣缍栨繝闈涱儏閻淇婇娑橆嚋妞ゅ孩顨婂濠氬磼濞嗘垵濡介梺娲讳簻缂嶅﹤鐣峰┑鍡╁悑闁告粈鐒︾€靛本绻涚€电孝妞ゆ垵鎳橀幏鎴︽偄閸忚偐鍙嗗┑鐘绘涧濡厼危閹间礁鐐婇柟缁㈠枟閳锋垹绱掔€ｎ偄顕滄繝鈧导瀛樼厽妞ゆ挆鍐╃亶缂?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊绋掑婵堢磼閳锯偓閸嬫捇姊绘担渚劸闁哄牜鍓涢崚鎺戠暆閸曗斁鍋撻崒鐐村仺闁告稑艌閹锋椽姊绘笟鍥т簽闁稿鐩幊鐔碱敍濞戞瑦鐝烽梺鍛婃寙閸涱垽绱查梻浣虹帛椤牆鈻嶉弴鐘辩剨妞ゆ挶鍨洪悡鐔兼煥濠靛棭妲归柡浣戒含缁辨帡鎼圭憴鍕彧缂備緡鍠楅悷銉╁煝鎼淬劍鍊锋い鎺戝€昏ⅸ闂傚倸鍊搁崐鐑芥嚄閸洖纾婚柕濞炬櫅绾惧潡姊洪鈧粔鎾偪椤曗偓閹綊骞侀幒鎴濐潊婵犳鍠楁繛濠囧蓟閺囩喎绶炴繛鎴烇供濞差厾绱撴担鍙夘€嗛柛瀣崌濮婄粯鎷呴崷顓熻弴闂佹悶鍔忓Λ鍕€﹂崶顏嶆Ъ缂備礁鍊圭敮锟犲极閸愵喖纾兼繛鎴炶壘楠炲牊淇婇悙顏勨偓銈夊储娴犲鍨傞柣銏犳啞閸婂爼鏌ㄩ弮鍫熸殰闁稿鎸鹃幉鎾礋椤掑偆妲堕梻浣侯攰濞呮洟骞愭ィ鍐ㄧ煑闁告侗鍙庡〒濠氭煏閸繃鍣界紒鐘卞嵆閹顫濋悡搴㈢彎闂佽桨绀佺粔褰掑箖閵忋倕绀傞悘蹇旂墬鐎氬ジ姊绘担钘夊惞闁哥姵鍔曢…鍨潨閳ь剟宕哄☉銏犵睄闁稿本绮庨敍婊冾渻閵堝棙绀€闁瑰啿閰ｉ幃姗€鏁冮崒娑氬幍濡炪倖娲栧Λ娑氬姬閳ь剟鎮楃憴鍕闁挎洏鍨介妴渚€寮崼婵嗙獩濡炪倖鎸鹃崰鎾诲焵椤掑倸浜剧紒缁樼箞閹粙妫冨ù韬插灲閺屻劑寮村Ο琛″亾濠靛绠栫憸鏂跨暦閸楃偐鏋庨柟瀵稿Х閸斿爼姊绘担绋款棌闁稿妫濆畷浼村箻閸撲椒绗夋俊銈忕到閸燁垶鎮″▎鎾村€甸柣銏☆問閻掓儳霉閻樻瑥娲﹂悡娆撴煕閹邦剙绾ч柛锝嗘そ閺屾洟宕惰椤忣參鏌涢埡瀣瘈鐎规洘甯掗…銊╁礃閵娧冩憢闂傚倸鍊风粈渚€骞栭锕€瀚夋い鎺戝€婚惌娆撴煙鏉堝墽鐣遍柣鎾偓鎰佺唵閻犺桨璀﹂悡顒佺箾缁楀搫濮傞柡灞剧洴椤㈡洟濡堕崨顔锯偓楣冩⒑閸濆嫭顥炲鐟版濡叉劙骞樼拠鑼紲濠殿喗锕╅崗姗€宕戦幘璇蹭紶闁靛／鍛帬闂備礁鎼ˇ浼村吹濮橆優娲敂瀹ュ棙娅嶉梻渚€娼х换鎺撴叏妤ｅ啫绠洪柡鍥ュ灪閳锋垿鏌涘┑鍡楊仾鐎瑰憡绻堥弻娑㈡偐閾忣偄鏆楅悗娈垮暙閸パ呭姦濡炪倖甯掔€氼參藟婵犲啨浜滈柟鎵虫櫅閻忣亜顭跨捄鍝勵伃闁哄瞼鍠撴禍鎼佸冀閵婏妇褰呴梺缁樻尪閸婃牠濡甸崟顖氱闁告鍋熸禒楣冩⒑閹惰姤鏁遍柛銊ユ健瀵鈽夊Ο閿嬵潔濠殿喗顨呴悧濠囧极妤ｅ啯鈷戦柟鎯板Г閺侀亶鎮介銈囩瘈鐎殿喛顕ч埥澶婎煥閸涱垱婢戦梻浣告惈閸燁垶骞戞笟鈧崺鈧い鎺嶇椤曟粍銇勯妸锝呭姦闁诡喗鐟╅獮鎾诲箳閹炬惌鍞查梻浣稿⒔缁垶鎮ч悩璇茶摕鐎广儱鐗滃銊╂⒑閸涘﹥灏甸柛鐘崇墪閻ｅ嘲煤椤忓嫷娼婇梺闈涚墕閹虫劙顢欓幒妤佺厽闁绘ê寮舵径鍕喐閺夊灝鏆ｉ柟顔筋殜椤㈡﹢鎮滈崱妯虹槣闂備線娼ч悧鍡椢涘▎鎾崇厱闁圭儤顨嗛悡娑㈡煕濠娾偓缁€浣圭濠靛牏纾奸柛灞炬皑鏁堝Δ鐘靛仜濞差參骞冭瀹曠厧顫濋鐑嗕紲濠电姷鏁搁崑鐘诲箵椤忓棗绶ら柤娴嬫櫆婵即鏌ㄩ弴鐐测偓褰掑磹閸ф鐓熼柟浼存涧閸橀潧霉濠婂嫮鐭掗柡宀€鍠撻埀顒佺⊕椤洨绮婚弽銊ｄ簻闁靛绲介悘顏呫亜閵婏絽鍔﹂柟顔界懇瀵爼骞嬮悩杈╃闂佽姘﹂～澶娒洪弽顓熷亯濠靛倹鎮堕埀顑跨铻栭柛娑卞幘閸旓箑顪冮妶鍡楃瑨閻庢凹鍘鹃埀?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗娼欒灃闁逞屽墯缁傚秹骞嗚濞撳鏌曢崼婵堝嚬缂併劌顭烽弻娑㈡偄閸涘﹦绋囬梺浼欑悼閺屽濡甸幇鏉跨婵犻潧娲﹀▍鏃堟⒒娴ｅ憡鍟炲〒姘殜瀹曞綊鎮ч崼鐔峰伎濠电偛妯婃禍婵嬪煕閹烘嚚褰掓晲閸曨噮鍔呭┑鐐插悑鐢繝寮诲☉銏犲嵆闁绘劕鍚€缁敻姊烘导娆戝埌濠⒀冩捣濡叉劙骞樼€涙ê顎撻梺鍦帛鐢﹥绔熼弴鐐╂斀闁绘﹩鍠栭悘杈ㄧ箾婢跺鈯曠€垫澘瀚板畷鍫曨敆閳ь剛澹曟繝姘厵闁硅鍔曢崥鍦偓瑙勬尫缁舵岸寮诲☉鈶┾偓锕傚箣濠靛洨浜┑鐘愁問閸犳牗顨ラ幖浣测偓鏃堝礃椤斿槈褔鐓崶銊︾缂佹劖顨婂娲川婵犲孩鐣堕梺鍝ュ枎濞硷繝骞冩ィ鍐╁€婚柦妯侯槺椤撴椽姊虹紒妯哄閻忓繑鐟х划锝夋倷閻戞ǚ鎷洪悷婊呭鐢帗绂嶆导瀛樼厱婵せ鍋撶紒鐘崇墪閻ｇ兘鏁愭径濠傝€垮┑锛勫仧缁垶寮埀顒勬⒒娴ｈ櫣甯涙い顓炴川閸掓帡顢涘锝嗩潔閻熸粌瀛╃粚杈ㄧ節閸ヨ埖鏅濋梺鎸庣箓濞诧箓宕抽銏″€甸柛顭戝亝缁舵煡鎮楀顓熺凡闁伙絿鍏橀幃銏㈠枈鏉堛劍娅囬梻浣虹帛閺屻劑骞夐敓鐘茬骇闁告稑锕ｇ换鍡涙煟閹板吀绨婚柍褜鍓氶悧鐘诲箖瑜嶈灃闁告侗鍘虹槐鍫曟煙閸忚偐鏆橀柛鏂跨灱婢规洘绻濆顓犲帾闂婎偄娲ら鍛村焵椤掍胶澧甸柟顔光偓鏂ユ婵﹫绲芥禍楣冩煕韫囨搩妲稿ù婊堢畺閺岋絾鎯旈婊呅ｉ梺鍛婃尰閻熲晛鐣烽崫鍕殕闁告洦鍓涢崣鍕磽閸屾瑧鍔嶆い顓炴处閺呭爼鎮介崨濠勫幐闁诲繒鍋熼弲顐㈡毄婵＄偑浼囬崒婊呯崲闂佸搫鏈惄顖炵嵁濡吋宕夐柣鎴烆焽娴滎亝淇婇妶鍥ラ柛瀣洴瀹曨垶骞庨崜鍨搐閻ｇ兘宕堕埡鍐跨床缂傚倸鍊烽悞锕傗€﹂崶鈺冧笉闁绘劗顣介崑鎾舵喆閸曨剛顦ラ梺缁樼墪閸氬绌辨繝鍥х濞达綀鍊介妸鈺傜厪闊洦娲栧皬婵犮垼顫夊ú鐔奉潖濞差亜绀堥柤纰卞墮鐢儵姊虹粙娆惧剮闁绘帪濡囩划瀣吋閸滀胶鍙嗛梺鍓插亞閸犳捇宕㈤幖浣光拺闁告稑锕ゆ慨褏绱掗懠璺盒撶紒鍌氱Ч瀹曟粏顦寸痪鎹愭闇夐柨婵嗘处閻掑潡鏌涜箛鎾存拱闁靛洤瀚版慨鈧柣娆忔嚀閸氼偅绻濈喊澶岀？闁轰礁顭烽悰顕€宕堕鈧痪褔鎮规笟顖滃帥闁哥偞妞藉缁樻媴缁涘缍堥梺绋垮婵炲﹤鐣烽姀銈呭唨妞ゎ剦鍟崗鐐瀹曘劑顢欑憴鍕伜婵犵數鍋犻幓顏嗗緤娴犲绠熼柨鐔哄Т閻ゎ噣鏌ｉ幇顔煎妺闁绘挻娲熼弻锟犲礃閿濆懍澹曢梻浣虹帛椤ㄥ牓宕戦妶鍡曠箚闁汇垻顭堢粻娑㈡煛婢跺孩纭舵い锔哄姂濮婃椽宕橀崣澶嬪創闂佺锕﹂…鍫ヮ敋閿濆閿ゆ俊銈勮閹锋椽鎮峰鍛暭閻㈩垱顨婇弫宥夋偄閸忚偐鍘介梺纭呮閸嬬喖鎮鹃悽鍛婄厱闁冲搫鍊诲ú瀛橆殽閻愬樊鍎旈柡浣稿€婚幉鎾晲閸℃浼栨繝纰夌磿閸嬫垿宕愰弽顓炵闁硅揪绠戠壕褰掓煛閸モ晛鏋旀い鈺呮敱缁绘繃绻濋崒婊冾杸闂佺粯鎸炬慨鐢垫崲濞戙垺鍤戝Λ鐗堢箓濞堫參姊虹拠鏌ョ崪缂佺粯绻堝濠氭晸閻樻彃绐涘銈嗘尵婵挳鎮￠悢濂夋富闁靛牆鍟悘顏堟煟閻旀繂瀚々鏌ユ煙闂傚顦﹂柛鎰ㄥ亾闂備線娼ч…顓㈡⒔閸曨偒鐔嗛柟杈鹃檮閳锋帡鏌涚仦鍓ф噯闁稿繐鏈妵鍕敇閻愰潧鈪甸悗娈垮枤閺佸銆佸Δ鍛＜婵犲﹤鎳愰崢顖炴⒒娴ｅ憡璐￠柛搴涘€濋妴鍐川鐎涙ê浜楅梺鍝勬储閸ㄦ椽鎮″▎鎾寸厽闁瑰鍊栭幋锕€鐓曢柟鐑樻⒒濡垶鏌熼鍡楀暞閸婎垶姊洪棃娑欐悙閻庢碍婢橀锝夊箻椤旇棄鈧兘鏌℃径瀣仸妞ゅ繒濮垫穱濠囧Χ閸ヮ灝銉╂煕鐎ｎ剙浠遍柕鍡楀暣瀹曞ジ濡烽妷褍濮︽俊鐐€栧濠氬磻閹剧粯鐓熼煫鍥ㄦ婢规ɑ銇勯鐐典虎閾伙綁鎮樿箛鏃傚ⅹ濞存粎鍋撻幈銊ヮ潨閸℃ぞ绨婚悗瑙勬尭濡繈寮婚敐鍛闁告鍋為悵婵嬫倵鐟欏嫭绀€闁绘牕銈搁妴浣肝旀担鍝ョ獮闁诲函缍嗛崑鍛存偟濠婂牊鈷掗柛灞剧懅缁愭棃鏌嶈閸撴盯宕戝☉銏″殣妞ゆ牗绋掑▍鐘炽亜閺傛娼熷ù婊勭矒閺岋繝宕堕張鐢垫晼闂佸憡甯婇崡鎶藉蓟閿濆鏅查煫鍥ㄦ礈閻﹀牓姊洪崫鍕婵炲樊鍙冨顐﹀磼閻愭潙鐧勬繝銏ｆ硾閺堫剝顣鹃梻鍌氬€搁崐鎼佸磹妞嬪孩顐芥慨姗嗗墻閻掔晫鎲搁弮鍫濈畺鐟滄柨鐣烽崡鐑嗘富闁靛骏绱曢埥澶愭煃閽樺妲搁柍璇查叄楠炲洭顢橀悩鎻掆枏婵犵绱曢崑鎴﹀磹閺嶎偅鏆滈柟鐑橆殔绾惧綊鏌ゆ慨鎰偓鏇⑺夊顑芥斀闁稿本纰嶉崯鐐烘煕鐎ｅ墎绉柡灞剧洴婵＄兘顢欓懡銈囨晨闂備焦鐪归崝宥咁渻閽樺鍤曟い鎰剁畱闁卞洭鏌曟径娑氱暠闁?JSON闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栭梻鍌欑閹诧繝骞愰崱娑樺窛妞ゆ梻鐡旈崯鍛存⒒閸屾瑦绁版い鏇嗗應鍋撳☉鎺撴珚闁硅櫕绻冮妶锝夊礂椤栨碍澶勯悗闈涖偢瀵爼骞婄粵鍦暤闁哄本鐩鎾Ω閵壯傚摋缂傚倷鑳舵慨鍨箾婵犲洤钃熸繛鎴欏灩閻掓椽鏌涢幇鍓佺窗婵炲矈浜濈换婵嬪閿濆孩缍堝┑鐐跺皺閸犲酣锝炶箛鎾佹椽顢旈崟顐ょ崺濠电姷鏁告慨瀵糕偓娑掓櫊閹寧鎷呴崷顓狀啎闁诲孩绋掗…鍥儗婵犲洦鐓熼柡宥庡亜鐢埖銇勯弴顏嗘偧闁归濮撮湁閻庯綆鍋掑鏃€绻濈喊妯活潑闁搞劌鐖煎銊╂焼瀹ュ繒绋忛悗骞垮劚閹冲寮ㄩ懞銉ｄ簻闁哄啫鍊婚幗鍌涚箾閸喐顥堥柡宀嬬到铻栭柍褜鍓欒灋婵炲棙鎸哥粻鐐烘煏婵炲灝鍔楁俊鎻掔墦閺屾洝绠涙繝鍐锯偓鍡涙煙闁垮銇濋柟顔筋殘閹叉挳宕熼鍌ゆК婵犵數鍋涢惇浼村礉鎼淬劌鏋佹い鏂跨毞濡插牊绻涢崱妤佹崳缂佺姵鑹鹃—鍐Χ閸℃顫囬梺鎼炲妼缂嶅﹤顕ｉ崘娴嬫瀻闁规壋鏅欑花濠氭⒑閸濆嫭绁╁ù婊呭仧缁棃顢欓柨顖氫壕閻熸瑥瀚粈鈧梺闈涚墢椤牓顢氶敐澶婄妞ゆ棁妫勬禍婊堟煟韫囨挾绠ｇ紒鈧担鍦洸濡わ絽鍟悡鐔兼煙鏉堝墽鍒扮悮姘舵⒑缂佹ɑ灏扮紒瀣灴閳ワ妇鎹勯妸锕€纾繛鎾村嚬閸ㄤ即宕滄导瀛樷拺缂備焦锚缁椦囨煕鐎ｎ偅灏扮紒宀冮哺缁绘繈宕堕懜鍨珖闂備線娼х换鍡椢ｉ崨顓囨盯鏁冮埀顒勫煘閹达富鏁婄痪顓㈡敱閺佹儳鈹戦敍鍕哗婵☆偄瀚伴幃楣冩倻缁涘鏅濋梺鎸庢磵閸嬫捇宕鐐粹拺闂傚牊渚楀褍鈹戦垾铏枠闁糕斁鍋撳銈嗗坊閸嬫挻銇勯鐐叉Щ妞ゎ偄绻愮叅妞ゅ繐瀚鍥煙閸忚偐鏆橀柛銊ョ秺钘濋柍鍝勫€荤粻楣冩倵濞戞瑯鐒介柣顓滃€曢湁婵犲﹤瀚惌鎺撱亜?.autostart-off 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆ繛纭风節瀵鎮㈢悰鈥充壕闁汇垺顔栭悞鍓р偓娑欑箘缁辨挻鎷呴悾灞界墯闂佺锕ョ换鍫濐嚕婵犳碍鏅濋柍褜鍓濋悘鎺撶箾閹炬潙鍤柛銊╀憾閺佸秹宕卞缁樻杸闂佺粯鍔曞鍫曞煝閺囩伝鐟邦煥閸曨厾鐤勯悗娈垮枟瑜板啴鈥﹂妸鈺侀唶闁绘柨鎼獮妤呮⒒娓氣偓閳ь剚绋戝畵鍡樼箾娴ｅ啿瀚▍鐘炽亜閺囨浜惧Δ鐘靛仦閻楃姴顕ｉ崼鏇炵妞ゆ牗绻傞幆鍫熺節濞堝灝鏋涢柨鏇樺劚椤啴鎸婃径灞炬闂侀潧顭俊鍥╁姬閳ь剟姊虹粙鎸庢拱妞ゆ洘鎸冲鎾偄閾忚鍟庨梺璇插缁嬫帡鈥﹂崶顒佸剹閻庯綆鍠楅悡鐔镐繆閵堝懎鏆欑€涙繈姊烘导娆戞偧闁稿繑锕㈤悰顔锯偓锝庡枟閺呮繈鏌嶈閸撴稓鍒掔紒妯肩瘈婵﹩鍘兼禒顖炴⒑閹肩偛鍔橀柛鏂跨灱瀵板﹥绻濆顓犲幗闁瑰吋鎯岄崰鏍嵁閺嶃劊浜滄い蹇撳閺嗭絽鈹戦垾宕囧煟鐎规洖宕灃闁逞屽墲閵嗘牜绱撻崒姘偓鎼佸磹閸濄儳鐭撻柡澶嬪殾濞戞鏃堝礋椤撴繄缍嶉梻鍌氬€风粈渚€骞夐敓鐘茬闁挎洖鍊哥粣妤佷繆閵堝懏鍣圭痪鎯х秺閺岋綁濮€閻樺啿鏆堥梺绋款儌閸撴繄鎹㈠☉銏犲耿婵°倕鍟伴鍥ь渻閵堝懏绂嬪ù婊呭仱閹偓妞ゅ繐鐗婇崐閿嬨亜閹哄秶顦﹂柍褜鍓氶崝娆撳蓟閿涘嫪娌柛鎾椻偓婵洭姊虹拠鈥崇仭婵☆偄鍟穱濠囧醇閺囩偛鑰垮┑鐐叉缁绘垿鏌ㄩ鐘电＝闁稿本鑹鹃埀顒勵棑缁牊绗熼埀顒€鐣烽幇鏉挎嵍妞ゆ挾鍠愬▓浼存⒑鐠恒劌鏋斿┑顔芥尦瀵憡绗熼埀顒勫蓟濞戙垹绠涢梻鍫熺⊕閻忓牏绱掗悙顒€鍔ら柛姘儑閹广垹鈽夐姀鐘殿吅闂佺粯鍔曞Λ娆撳垂閸ф绠栭柣鎰靛墮椤曢亶鎮楀☉娅辨岸骞忕紒妯肩閺夊牆澧介幃鍏笺亜椤撶偟澧︾€殿喗濞婇弻鍡楊吋閸℃瑥骞嶇紓浣哄亾濠㈡ê鐣烽浣侯洸濞寸厧鐡ㄩ悡娑樏归敐鍥剁劸闁哄棴缍侀弻鈩冩媴閻熸澘顫掗悗瑙勬礀閻栧ジ鍨鹃敃鍌氱闁绘劗鏁搁埣銉︾節閻㈤潧啸妞わ綀妫勫嵄闁告稒娼欓崹鍌炴煟閵忋埄鐒剧痪鎯ь煼閺岀喖鏌囬敃鈧弸娑欍亜閵夛箒澹橀棁澶愭煟濡儤鈻曢柛搴㈠姍閺岋綁骞樼€涙顦伴梺璇″枟閿曘垽骞冨▎鎾崇闁瑰搫妫欑€垫牠姊绘担鐑樺殌闁哥喎娼￠獮濠呯疀濞戞锕傛煕閺囥劌鐏￠柡鍛矊閳规垿宕掑顓炴殘濡炪倕楠哥粔鐟邦潖濞差亝顥堟繛鎴炵懄閹瑩鏌ｆ惔銏㈢叝闁告鍟块锝夘敃閿曗偓绾惧吋绻濇繝鍌氼仼闁绘繀鍗冲娲嚃閳圭偓瀚涢梺绋跨箲閻╊垰鐣烽悩璇插唨妞ゆ劑鍊楅敍婵嬫⒑缁嬫寧婀伴柣鐔濆懏瀚诲ù鐓庣摠閻撴洟鏌ｅΟ璇插婵炲牊姊归妵鍕敃閵忋垻顔夌紓浣虹帛缁诲倿锝炲┑瀣垫晢闁稿本鑹捐婵犵绱曢崑鎴﹀磹閺嶎偅鏆滃┑鐘叉处閸ゅ嫰鏌涢锝嗙闁绘挴鍋撻梻浣告惈濞层劑宕伴崱妯碱洸婵犲﹤瀚ㄦ禍婊堟煙閹规劖纭惧ù鐙呯畵閺屾盯鎮╃粵纭呪偓鍧楁煛鐏炵偓绀嬬€规洖宕灃濞达絽鎲￠悾濠氭⒒娴ｅ憡鎯堥悶姘煎亰瀹曟繃鎯旈妸銉у姦濡炪倖甯掗敃锔剧矓閻㈠憡鐓曢柟鎯ь嚟濞插瓨銇勯姀锛勬噰鐎殿喗鎸虫慨鈧柍閿亾闁归攱妞藉娲嚒閵堝懏鐏堥梺绋款儏閿曨亜鐣烽悽鍛婂亜闁稿繗鍋愰崢杈ㄧ節閻㈤潧孝闁哥噥鍋婇幃姗€濡烽敂杞扮盎濡炪倖鍔х徊璺ㄧ不濮椻偓閺岀喖鎮烽弶娆句純閻庤娲﹂崑濠傜暦閻旂⒈鏁冮柨婵嗛閹姊婚崒姘偓鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠Χ閸モ晝鍘┑鐐存尰閸╁啴宕戦幘鍨涘亾鐟欏嫭纾搁柛銊ㄦ缁骞掗弬鍝勪壕闁挎繂楠告禍婊兠瑰鍕垫當闁宠鍨块幃娆撳级閹寸姳鎴烽梻浣规偠閸斿秴顪冩禒瀣畺闁跨喓濮撮崡鎶芥煏韫囥儳纾块柛姗嗕簼缁绘繈濮€閿濆懐鍘紓浣割儐閹瑰洤鐣峰璺虹骇婵炲棗澧介崬鐢告偡濠婂啰效妤犵偛顦甸弫鎾绘偐閼碱剛鏉介梻渚€娼ч…鍫ュ磿濞嗘挻鍋勯柣鎾虫捣椤斿洭鏌熼懝鐗堝涧缂佹彃顭烽崺鈧い鎺嶇婵绱?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幘缂佺偓婢樺畷顒佹櫠缂佹ü绻嗛柡灞诲劙閹查箖鏌″畝瀣М闁诡喓鍨藉畷顐﹀Ψ瑜忛崢鎺戔攽閻橆偅濯伴柛鏇炵仛閻ｅジ姊烘潪鎵窗闁革綇缍侀悰顕€骞掗幊铏閸┾偓妞ゆ帒瀚崹鍌炴煕瀹€鈧崑鐐烘偂閺囩姭鍋撻崗澶婁壕闂侀€炲苯澧寸€规洘鍨块幃娆撴倻濡桨绨甸梻浣虹帛濡啴寮查懠顒佸床闁糕剝绋掗悡蹇涙煕椤愶絿绠撻柍褜鍓欓幉锛勭矉瀹ュ棎鍋呴柛鎰ㄦ杹閹疯櫣绱撻崒娆戝妽闁挎洍鏅涢埢鎾诲籍閸喓鍙嗛梺鍝勬储閸斿鑺辨繝姘厽闁挎繂娲ら崢鎾煛娴ｇ懓濮嶇€规洖銈告慨鈧柕蹇婂墲缁额偄鈹戦悩鍨毄闁稿鍋ゅ畷褰掑醇閺囩喎浠悷婊勬煥閻ｇ兘濮€閵堝懐顔掔紓鍌欑劍鑿ら柟閿嬫そ濮婃椽宕ㄦ繝鍕ㄦ闂佹寧娲╂俊鍥╁垝婵犳艾绠荤紓浣姑埀顒€鐏氶幈銊ノ熼悡搴′粯閻庢鍣崰姘跺焵椤掑喚娼愭繛鍙壣戠换娑欑節閸屾粍娈鹃梺缁樺灩閻℃棃寮崶顒佺厽婵☆垳鈷堥弳婊呯磼閸楃偛绾уǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘肩粈澶愭煙鏉堝墽鐣遍柛銊ュ€荤槐鎾存媴閼测剝鍨甸埢宥堢疀濞戞瑢鎷婚梺绋挎湰閻熝囁囬敃鍌涚厵缁炬澘宕禍鐐烘煙椤曞懎娅嶆い銏℃礋閺佸啴鍩€椤掑倸顥氶悷娆忓缁♀偓闂傚倸鐗婃笟妤呭磿閹寸姭鍋撶憴鍕矮缂佽埖宀稿濠氬灳閹颁礁鎮戦梺鍛婂姂閸斿矂鈥栨径瀣瘈婵炲牆鐏濋悘锟犳煙閸涘﹤鈻曟鐐插暙閻ｏ繝骞嶉搹顐も偓濠氭椤愩垺澶勯柟灏栨櫆鐎靛ジ宕橀瑙ｆ嫼闁哄鍋炴竟鍡欐嫻閺夋５鐟邦煥鎼存繄鐩庡銈庡亜缁绘帞妲愰幒鎳崇喓鎷犲顔瑰亾閹剧粯鈷戦柤濮愬€曢弸鎴濐熆閻熼偊鍎旂€殿喚顭堥埢搴ㄥ箻缁瀚奸梺鑽ゅ仦缁嬪牓鍩€椤掆偓閸樻牗绔熼弴銏♀拻濞达絽鎲￠崯鐐淬亜閵娿儳澧︾€规洘鍔欓弫鎰板川椤忓拋娼旈梻渚€娼х换鎺撴叏閻戝壙鍥Ω閵夘喗瀵岄梺闈涚墕濡瑩藟閸℃稒鐓曢悗锝庡亜婵秹鏌?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊閻樺樊妫岄梺杞扮閿曨亪寮婚垾鎰佸悑閹肩补鈧磭顔愮紓鍌欑劍閸旀牠銆冮崱妯尖攳濠电姴娲ゅ洿闂佸憡渚楅崢钘夆枔閺屻儲鈷戦柛婵嗗椤ョ偟绱掗鑲┬ｇ紒宀冮哺缁绘繈宕堕‖顑洦鐓曟繛鎴濆船楠炴绻涢崼婵堝煟婵﹤鎼叅閻犲洦褰冪粻娲⒑閼姐倕鏆遍柡鍛Т椤曪絾绻濆顓炰簻闂佺绻掗崢褔鍩€椤掑倻甯涘ǎ鍥э躬椤㈡盯鎮欑€涙褰哥紓鍌欑贰閸犳牠鎮ラ悡搴綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顦插褏鏅Σ鎰板箳濡や礁浜归梺鎯ф禋閸嬪嫮鍠婂澶嬧拺濞村吋鐟х粔顕€鏌￠埀顒佹綇閳哄啰澶勯梺浼欑到閻ジ鎯屽▎鎾村仯闁搞儻绠戠€氬酣鏌涢弬璺ㄐу┑锛勬暬瀹曠喖顢涘杈╂綁闂備焦鎮堕崕娲倶濠靛绠婚悗闈涙憸椤旀洟姊洪崨濠佺繁闁告ɑ鍎抽悺顓㈡⒒娴ｅ摜鏋冩い顐㈩樀瀹曞綊宕稿Δ鈧粻鏍煃閸濆嫭鍣归柛銊ュ€归妵鍕箛閸撲胶蓱闂佷紮绠戞鎼佸煘閹达附鍋愰柟棰佺閺呴亶姊洪崫銉バｇ€光偓缁嬫鍤曟い鎰剁悼缁♀偓濠殿喗锕╅崜娑㈡偪閸パ€鏀介柣妯肩帛濞懷勩亜閹存繃鍣界紒顔肩墦瀹曞ジ濮€閳ヨ櫕鐎鹃梻濠庡亜濞诧妇绮欓幒妤€纾挎俊銈呮噺閻撴洟鏌嶉埡浣告殶闁愁垱娲熼弻锝夊箻鐎涙顦梺閫炲苯澧柛妯荤矒瀹曟垿骞樼紒妯煎帗閻熸粍绮撳畷婊堟偄妞嬪孩娈鹃梺纭呮彧闂勫嫰宕愰悜鑺ョ厽闁瑰浼濋鍛洸婵°倐鍋撻柍瑙勫灴閹瑩宕ｆ径妯伙紒闂備礁婀遍…鍫モ€﹂悜钘夌畺闁炽儲鏋奸弸搴ㄦ煙閹佃櫕娅冪紒銊ヮ煼濮婃椽宕崟顓夌娀鏌涢弬鍧楀弰闁糕晛锕鎾偐閻㈢绱查梻浣筋嚃閸ㄥ崬顭垮鈧幆鍐洪鍛幍濡炪倖娲栭幊搴ㄥ几濞戞瑣浜滄い鎰剁悼缁犵偤鏌熼搹顐ょ煂闁哥姴锕よ灒閻犲洤妯婃导?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕绀傞柣鎾崇岸閸嬫挾绱掑Ο鍦畾濡炪倖鐗楃换鍐敂閻樼粯鐓曢柡鍐ㄥ亞閻掗箖鏌嶇憴鍕伌闁诡喗鐟╅幊鐘活敆娴ｇ儤顎囬梻鍌欑閹碱偊骞婇幇顓犵闁逞屽墰閳ь剚顔栭崰鏍€﹀畡閭﹀殨闁圭虎鍠楅崑鍕煣韫囨凹鍤冮柛鐔烽叄濮婄粯鎷呴搹鐟扮闂佸憡妫戠粻鎾崇暦濠婂喚娼╅柤鎼佹涧娴犙囨⒑閸濆嫬鏆欓柣妤€锕崺娑㈠箣閿旂晫鍘卞┑鐘绘涧濡顢旈幘顔界厱閹肩补鍓濋幑锝囩磼鏉堛劌娴柟铏矒濡啫鈽夊Ο缁樺殘闂佽瀛╅鏍窗閺嶎厸鈧箑鐣￠柇锕€娈ㄩ梺鍦檸閸犳牠锝為崨瀛樼厽婵☆垱顑欓崵娆撴煛娴ｇ瓔娼愮紒缁樼箞閹粙妫冨ù韬插灲閺岋綀绠涢弬鍨懙閻庤娲樼换鍫濐嚕娴犲鏁冮柕鍫濇濞呮绱撻崒姘偓鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屻倝骞栨担瑙勯敪闂佹悶鍔嶅Λ鍐箖濡ゅ懏顥堟繛鎴炵懄閸犳劗绱撴担鎻掍壕婵犵數濮存导鍛般亹閹烘挻娅滈梺鍛婁緱閸欏骸螞鐏炶В鏀介柣鎰絻閹垿鏌ｅΔ渚囨畼闁瑰箍鍨归埥澶婎潨閸℃娅婃俊鐐€栧Λ浣哥暦閻㈠憡鍎庨幖娣妽閳锋垿鏌熼懖鈺佷粶濠碘€炽偢閺屾稒绻濋崒婊呅ㄦ繝纰夌磿閺佽鐣烽悢纰辨晬婵﹩鍓欑粊顕€姊绘担铏瑰笡闁告梹鐗曢…鍥р枎韫囧﹥鐎洪梺鎸庣箓椤︿即鍩涢幋锔界厱婵炴垶锕弨濠氭倶韫囷絽寮柡灞剧洴閹垽鏌ㄧ€ｎ亙娣梻浣虹《閺呮繈宕戦妶澶屽祦閻庯綆鍠楅弲婊呯磽娴ｈ偂鎴濃枍閹惧墎纾藉ù锝夌細濡炬悂鏌涘鈧粻鏍ь嚕缂佹ê绶為柟閭﹀墲琚濇俊鐐€栭崝鎴﹀磹濡ゅ啫顥氶柦妯侯棦瑜版帗鏅查柛娑卞枟閸庢捇鏌″蹇曠瘈婵﹨娅ｇ槐鎺戭潨閸℃鏆┑鐘愁問閸犳骞愰幎濮愨偓浣割潩閼稿灚娅滈梺鎼炲劘閸斿矂鎮甸弶搴撴斀闁绘劕鐡ㄧ紞鎴炪亜閹存繃顥㈤柨婵堝仜椤撳ジ宕ㄩ鍛澑婵＄偑鍊栧濠氭偤閺傚簱鏋旈柡鍐ｅ亾濞ｅ洤锕、鏇㈡晲閸ャ劌娅氶柣搴ゎ潐濞叉﹢骞戦崶顒€绠栧ù鐘差儏瀹告繂鈹戦悙闈涗壕閻庢俺鍋愮槐鎾诲磼濮橆兘鍋撻悜鑺ュ€块柨鏇氱劍閹冲矂姊虹拠鑼婵炲瓨宀稿畷銏ゆ偩鐏炴儳搴婂┑鐐村灟閸ㄥ湱绮荤紒妯镐簻闁哄啫娲よ闂佸搫妫涙慨椋庢閹惧瓨濯寸紒瀣氨閸嬫捇宕烽鐔告闂佽法鍠撴慨浼村焵椤戣法顦﹂柍钘夘樀婵偓闁炽儲鏋奸崑鎾搭槹鎼达絿锛滃銈嗘⒐椤戞瑥顭囬幇鐗堝€垫慨姗嗗厵閸嬨垺鎱ㄦ繝鍐┿仢鐎规洦鍋婂畷鐔煎垂椤愬秲鍔戝铏圭矙濞嗘儳鍓遍梺鎸庢磸閸ㄥ綊鎮鹃悜鑺ユ櫢闁绘灏欓ˇ銊╂倵閻熸澘顥忛柛鐘冲哺閸┾偓妞ゆ巻鍋撻柣鏍с偢瀵鏁愭径濠勵吅闂佹寧绻傞幉娑㈠箻缂佹鍘介梺瑙勫劤閻°劎绮堥埀顒勬⒑鐎圭媭娼愰柛銊ユ健楠炲啴鍩￠崨顓狀唽闂佸湱鍎ら幐濠氼敊瀹ュ應鏀介柣姗嗗亝婵即鎮橀悙璺轰汗闁告垵缍婂娲濞戞瑦鎮欓柣搴㈢煯閸楁娊濡存担鍓叉建闁逞屽墴楠炲啫鈻庨幘宕囬獓闂佽鎯岄崢鍊熲吀缂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛鎾茬閸ㄦ繃銇勯弽銊х煁鐎规洘鐓￠弻娑樼暆閳ь剟宕曢幋鐘电闁哄秲鍔庣弧鈧梻鍌氱墛娓氭宕曞☉銏＄厸濞达絽鎲￠ˉ銏ゆ煛鐏炲墽銆掑ù鐙呯畵楠炲棜顦撮柛鏂惧嵆濮婅櫣绮欏▎鎯у壉闂佸湱鎳撳ú顓㈢嵁閸愩劉鏋庨柟鐐綑閳ь剛绮穱濠囶敍閻愬瓨鏆犻柣搴㈣壘閵堢顫忔繝姘＜婵﹩鍏橀崑鎾诲箹娴ｇ懓浜辨繝鐢靛Т閸熶即鎮风憴鍕箚闁靛牆鍊告禍楣冩煕濡ゅ懍鎲鹃柡灞剧洴椤㈡洟鏁愰崱娆樻О闂備胶绮幐璇裁洪悢鐓庤摕闁哄洨鍠撶弧鈧柟鑹版彧缁茬晫鏁妷銉㈡斀闁绘劖褰冮幃鎴︽煕閺冣偓椤ㄥ牆危閹版澘绠虫俊銈傚亾缁炬儳鍚嬮幈銊ノ熼懡銈嗏枈閻熸粌閰ｉ妴鍐Ψ閳哄倸鈧兘鏌涘▎蹇ｆ▓婵☆偓绠撳娲倷閽樺濮曢梺缁橆殘婵炩偓鐎规洖缍婇獮鍡氼槾闁哄棴绠撻弻娑㈠Ψ閵忊剝娈ㄩ梺闈浥堥弲婊堝煕閹达箑绾ч柣鎰綑椤ュ鏌涢弬璺ㄐч柡灞剧洴瀵噣鍩€椤掑嫭鍋嬮柡鍥ュ焺閺佸鏌曟径鍡樻珔缂佲偓鐎ｎ偁浜滈柟閭﹀墻閸庢垿鏌涢悩鍙夋崳闁告瑥鎳愮槐鎾诲磼濞嗘垵濡介梺鎸庢磵閺呮稖鐏嬪┑鐘绘涧閻楀繒澹?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊绋掑婵堢磼閳锯偓閸嬫捇姊绘担渚劸闁哄牜鍓涢崚鎺戠暆閸曗斁鍋撻崒鐐村仺闁告稑艌閹锋椽姊绘笟鍥т簽闁稿鐩幊鐔碱敍濞戞瑦鐝烽梺鍛婃寙閸涱垽绱查梻浣虹帛椤牆鈻嶉弴鐘辩剨妞ゆ挶鍨洪悡鐔兼煥濠靛棭妲归柡浣戒含缁辨帡鎼圭憴鍕彧缂備緡鍠楅悷銉╁煝鎼淬劍鍊锋い鎺戝€昏ⅸ闂傚倸鍊搁崐鐑芥嚄閸洖纾婚柕濞炬櫅绾惧潡姊洪鈧粔鎾偪椤曗偓閹綊骞侀幒鎴濐潊婵犳鍠楁繛濠囧蓟閺囩喎绶炴繛鎴烇供濞差厾绱撴担鍙夘€嗛柛瀣崌濮婄粯鎷呴崷顓熻弴闂佹悶鍔忓Λ鍕€﹂崶顏嶆Ъ缂備礁鍊圭敮锟犲极閸愵喖纾兼繛鎴炶壘楠炲牊淇婇悙顏勨偓銈夊储娴犲鍨傞柣銏犳啞閸婂爼鏌ㄩ弮鍫熸殰闁稿鎸鹃幉鎾礋椤掑偆妲堕梻浣侯攰濞呮洟骞愭ィ鍐ㄧ煑闁告侗鍙庡〒濠氭煏閸繃鍣界紒鐘卞嵆閹顫濋悡搴㈢彎闂佽桨绀佺粔褰掑箖閵忋倕绀傞悘蹇旂墬鐎氬ジ姊绘担钘夊惞闁哥姵鍔曢…鍨潨閳ь剟宕哄☉銏犵睄闁稿本绮庨敍婊冾渻閵堝棙绀€闁瑰啿閰ｉ幃姗€鏁冮崒娑氬幍?-> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇牠姊绘笟鈧埀顒傚仜閼活垱鏅堕弶娆剧唵閻熸瑥瀚粈鍐磼缂佹绠撴い顐ｇ箞椤㈡﹢鎮╅幓鎺旂杽闂傚倸鍊烽悞锕傚几婵傜鐤炬繛鎴欏灩閻ゎ喖霉閸忓吋缍戦柣鎾偓鎰佺唵闁兼悂娼ф慨鍫ユ煟閹惧瓨绀嬮柟顔斤耿閹瑦锛愬┑鍡橆唲濠电姵顔栭崰鏍磹閸ф钃熼柨娑樺濞岊亪鎮归崶銊с偞闁稿鎹囧鎾閿涘嫮鏆㈤梻鍌欐祰椤曆囧礄閻ｅ瞼绀婇柛鈩冪☉绾惧鏌涘☉妯兼憼闁稿鍠栭弻锟犲炊閳轰礁澹夊┑鐐存尭椤兘寮婚弴銏犻唶婵犻潧顑愰埀顒侇殔闇夋繝濠傚缁犳牜绱掔紒妯兼创闁瑰磭濞€椤㈡鍩€椤掑嫬鍑犳繛鎴炲焹閸嬫挸鈻撻崹顔界亪濡炪値鍘鹃崗姗€鐛崘顔碱潊闁靛牆鎳庣粣娑欑節閻㈤潧孝閻庢凹鍠涢崐鎾⒒閸屾艾鈧绮堟笟鈧獮澶愭晸閻樿尙顔囬梺绯曞墲缁嬫垵顔忓┑鍥ヤ簻闁哄啫鍊婚幗鍌炴煕閻旈攱鍤囨慨?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幈闂佸綊鍋婇崹鎵閿曞倹鐓熼柕蹇嬪灮鐢稑菐閸パ嶈含闁诡喗鐟╅、鏃堝礋閵娿儰澹曢梺鍝勭▉閸樹粙宕戠€ｎ偂绻嗛柕鍫濇噹閺嗙喖鏌ｉ鐔烘噰闁哄瞼鍠栭幃娆擃敆閸屾稖寮撮梻渚€鈧偛鑻晶顕€鏌熼搹顐€顏堟偩閻戣姤鏅查柛鈩冾殘缁愮偤鎮楅崗澶婁壕闂侀€炲苯澧撮柟顖欑窔瀹曞ジ鎮㈤搹鍦闂備焦鐪归崹钘夘焽瑜嶉悺顓㈡⒒娴ｄ警鐒鹃柨鏇樺劦楠炲啴宕掑鐓庢闂佸綊妫块悞锕傚疾濠靛鐓曢悘鐐插⒔閵嗘帡鏌涘鈧粻鏍ь潖閾忓湱纾兼慨妤€妫欓悾鍫曟⒑缂佹﹩娈旀俊顐ｇ箞閵嗕礁顫濋澶屽弳闂佸壊鍋嗛崰鎾诲储椤忓牊鈷戦柟鑲╁仜閸斺偓闂佸憡鍔曡ぐ鐐垫閹惧墎纾介柛灞剧懆閸忓苯鈹戦鐐毈闁诡垰瀚伴、娑㈡倷闂堟稓銈︽繝纰樻閸ㄤ即宕ョ€ｎ亞鏆﹀璺侯焾閳ь剚甯掗～婵嬫偂鎼达絼鐢绘繝娈垮櫙缁茶棄煤椤撶儐娼栫紓浣股戞刊鎾煢濡警妲搁柟顖滃仱濮婃椽宕妷銉愶綁鏌涢弮鈧悷鈺呯嵁閸愵喗鍊婚柤鎭掑劤閸斿爼鎮楅獮鍨姎婵☆偄鐭傞獮鎰板礃椤旇В鎷洪梺鍛婄☉閿曘儲寰勯崟顖涚厱闁靛ě鍐炬毉濡炪們鍨归敃顏勵潖缂佹ɑ濯撮柛娑橈工閺嗗牆鈹戦悙棰濆殝缂佺姵鎸搁悾椋庢喆閸曨偒鍤ら梺鍝勵槹閸ㄥ綊藝閵娾晜鈷戦梺顐ゅ仜閼活垱鏅剁€涙ɑ鍙忓┑鐘插暞閵囨繃淇婇銏犳殭闁宠棄顦板蹇涘煛娴ｆ劅顏堟⒒閸屾瑧顦﹂柟璇х節瀵濡搁埡鍌氬壒闂佺鏈换鍐晬閸屾稓绡€闁汇垽娼ч埢鍫熺箾娴ｅ啿娴傞弫鍕煕閳╁啰鈽夌痪鎯ь煼閺岋絽螣閹稿海褰ч梺鍛婄懃缁绘垶绌辨繝鍥舵晬婵﹩鍘介崕鎾绘⒑閼测晩鐒剧紓宥咃躬瀵鏁嶉崟銊ヤ壕闁挎繂楠告禍鐐烘煃椤栨稒绀嬮柡宀€鍠愮粭鐔煎垂椤旂⒈鐎寸紓鍌欒兌缁垳鎹㈤崼婵堟殾婵せ鍋撻柛鈺冨仦瀵板嫬顫㈢仦缁⑩偓妤呮⒒娴ｇ瓔鍤欐慨姗堢畵閿濈偞寰勬繛鎺撴そ閺佸啴宕掑鎲嬬幢濠电姷鏁告慨鎾磹婵犳艾姹查柨鏇炲€归悡鐔兼煛閸愩劌鈧敻骞忛敓鐘崇厸濞达絿顭堥弳锝夋煛鐏炶濡奸柍瑙勫灴瀹曞崬顫滈崼銏㈩槸闂傚倷绀侀幗婊堝窗鎼淬劍鍋嬮柛鈩冪☉閻撴﹢鏌熸潏楣冩闁稿鍔欓弻娑⑩€﹂幋婵呯凹濠殿噯绲界€氭澘顫忛搹鍦煓婵炲棙鍎抽崜閬嶆⒑閻戔晜娅撻柛銊╂涧铻為柣鏂垮悑閳锋垿鎮峰▎蹇擃伌闁哥喎绻橀弻娑㈡偐閸愭彃鎽甸梺璇″灠閼活垶鍩㈡惔銊ョ閻庣數顭堥獮宥夋⒒娴ｈ櫣甯涢柛銊﹀劶閹筋偊姊洪幎鑺ユ暠閻㈩垽绻濆濠氭晲婢舵ɑ鏅ｉ梺缁樺灥濡瑩鍩㈤崼銉︹拺闁告繂瀚悘閬嶆煕閻樺磭澧垫鐐插暙椤劑宕楅崜褍鏋涢柟鐓庣秺閺屽懎鈽夊▎蹇旂亾濠电姷鏁告慨鐑姐€傛禒瀣婵犻潧顭堟禍褰掓煕閹伴潧鏋熼柛瀣儏閳规垿鎮╅幓鎺濅痪閻庤娲栧鍓佹崲濞戙垹绠ｆ繛鍡楃箳娴犵厧螖閻橀潧浜奸柛銊у劋缁岃鲸绻濋崶鑸垫櫖濠电娀娼уù鍌涚閳哄倻绠鹃悗娑欘焽閻﹪鏌ｉ弽顐㈠付妞ゎ偄绻愮叅妞ゅ繐瀚槐鍫曟⒑閸涘﹥绀嬫繛浣冲洤绀堥梺顒€绉甸埛鎴犵磽娴ｇ櫢渚涙繛鍫熋埞鎴︻敊閼恒儱鍞夐梺鎸庣箘閸嬬姷绮诲☉銏犵濞达綀顫夐妵婵囥亜閵忊剝顥堢€规洏鍔戦、娑橆潩椤戭偁鍎靛濠氬磼濞嗘帒鍘＄紓渚囧櫘閸ㄨ泛鐣峰┑鍠喖姊荤€电澧炬俊鐐€曠换鎰涘畝鍕；闁圭偓鍓氬鈺傘亜閹捐泛鏋旈柛鎴斺偓鎰佹富闁靛牆楠稿銊╂煕鎼粹槅鐓肩弧鎾炽€掑锝呬壕闂佸搫鏈惄顖炲极閹邦垳鐤€闁哄洨濮屽鍫熷€垫繛鍫濈仢閺嬶附銇勯弴鍡楁祩閺佸洤鈹戦崒婊庣劸闁绘劕锕弻鏇熺箾閸喖濮㈤悷婊呭閹稿啿顫忕紒妯诲闁告稑锕ラ崕鎾绘煟閻愭潙鍤柛瀣工閻ｇ兘寮撮姀鐘殿攨闂佺粯鍨靛Λ妤€鈻撻妸锔剧閻庣數顭堢敮鍫曟煟鎺抽崝鎴﹀春濞戙垹绠ｉ柣妯兼暩閿涙粌顪冮妶鍡欏缂佸甯炵划鍫ュ礃椤旂晫鍘告繝銏ｆ硾椤戝棝濡靛┑瀣厸閻忕偛澧介埊鏇㈡煙椤栨稒顥堝┑鈩冩倐閺佸倹绂掔€ｎ偅娈梻鍌氬€峰鎺旀椤旀儳绶ら柛褎顨呯粈鍌涗繆椤栨繍鍞虹紒璇叉閺岀喖姊荤€靛壊妲梺缁樻尰缁嬫垿鍩為幋锔藉亹闁圭粯甯楀▓鎻掆攽閻愬弶鍣洪柣妤侇殘閹广垹鈹戠€ｎ偄浠洪梻鍌氱墛閸掆偓闁靛繈鍊栭悡銉╂煛閸ヮ煁顏堝焵椤掍緡娈滈柛鈹惧亾濡炪倖甯婇懗鍫曞煝閹剧粯鐓涢柛娑卞枤濞插鈧鍠楁繛濠囧春閿熺姴纾兼繝濠傛噽濡插洭姊绘担鍦菇闁搞劏妫勯…鍥槻闁哄棌鏅犲缁樻媴閼恒儯鈧啴鏌ｉ幒鐐电暤妤犵偛鍟村畷鎺戭潩鏉堛劍顔曢梻浣侯攰閹活亪姊介崟顖氱９闁绘垼濮ら悡鏇熺節闂堟稒顥滄い蹇婃櫊閺岋絾鎯旈鐓庣睄濠殿喖锕ュ钘夌暦濠婂牆绠甸柟鍝勭Ф閸戣绻濋悽闈涗粶鐎殿喖鐖奸幃褍顭ㄩ崨顓炵亰缂傚倷鐒﹂敋妞ゆ洟浜堕幃妤€鈽夊▎妯煎姺闂佸憡锚瀹曨剟鍩為幋锔藉亹缂備焦蓱闁款參鏌ｆ惔銏犲毈闁告挾鍠庨悾宄扳攽鐎ｎ€冾熆鐠轰警鍎戦柛妯挎椤啴濡堕崱妯烘殫闂佸摜濮甸崝鏇㈡偩瀹勬壆鏆嗛柛鏇ㄥ墰閸樻悂鎮楅崗澶婁壕闁诲函缍嗛崜娑溾叺婵犵數濮甸鏍窗閹烘纾婚柟鍓х帛閳锋垿姊婚崼鐔恒€掔紒鐘冲哺閺岋繝宕ㄩ鍓х杽閻庤娲樼换鍕垝濞嗘劖鍎熼柟鎯х摠閺夋悂姊绘担鍝ユ瀮婵☆偄瀚灋婵°倓鑳堕々鍙夌節闂堟侗鍎愰柣鎾卞劜閹便劌螣閸喚鍘銈冨劚濡鈥﹂崸妤佸殝闂傚牊绋戦～宥夋⒑缂佹ɑ灏柛搴ゅ皺閹广垹鈹戠€ｎ偒妫冨┑鐐村灦閻燁垰螞閵堝棛绡€闁冲皝鍋撻柛鈩冪☉閹界數绱撴担铏瑰笡闁烩晩鍨伴悾宄拔旈崨顔间簵闁硅偐琛ュΣ鍛焽閹剧粯鈷掗柛灞剧閹兼劖銇勯敂钘夘棆闁瑰箍鍨归埞鎴炵節鎼粹懣鈺呮⒒娴ｇ瓔鍤欑紒缁樺灩閹广垽宕奸妷銉︽К闂侀€炲苯澧柕鍥у楠炴帒顓奸崼婵嗗腐濠电姭鎷冮崶锔藉枤闂佸搫鐬奸崰鏍偘椤曗偓瀹曞綊顢欓崣銉﹀亝闂傚倷绀侀悿鍥涢崟顖€鍥垂椤愶絽寮块梺閫炲苯澧撮柡宀嬬到铻ｆ繛鑼额嚙閺呴亶姊洪崨濠呭闁硅櫕锚椤繒绱掑Ο璇差€撻梺鎯х箳閹虫挾绮敓鐘斥拺闁硅偐鍋涙慨鍌涚箾娴ｅ啿妫涙禍杈ㄧ節閻㈤潧浠﹂柛顭戝灦瀹曞綊宕烽鐘电劶婵犮垼鍩栭崝鏍偂閻斿吋鐓欓弶鍫濆⒔閸掓壆鎮鑸碘拺闁规儼濮ら弫閬嶆煕閵娿儲鍋ョ€殿喛顕ч埥澶娢熼柨瀣澑闂備胶纭堕崜婵嬨€冭箛鏃傤浄闁诡垎灞惧瘜闂侀潧鐗嗛崯顐﹀礉濠婂牊鐓熼柣鏂垮级濞呭﹦鈧鍠涢褔鍩ユ径濞㈢喎顭ㄩ崟顑芥嫻闂佺懓鍢查幊鎰板礌閺嶎厼鍗抽柣鏃囨〃缁ㄤ粙姊绘担绛嬪殭閻庢稈鏅犻、娆撳冀椤撶偟鐛ラ梺鍝勭▉閸樻悂鍩€椤掑﹦鐣甸柟顔界矒閹稿﹥寰勭€ｎ兘鍋撻鍕拺闁荤喐婢橀埛鏃傜磼椤曞懎鐏︾€规洘鍨块獮鍥偋閸垹骞樼紓浣哄亾濠㈡ê鐣烽浣规珷缂備焦眉缁诲棝鏌ょ喊鍗炲妞わ絿鍘ц彁闁搞儜宥堝惈闂佽鍠楅悷鈺呫€侀弮鍫濈闁靛鍎版竟鏇㈡⒑閸濆嫮鈻夐柛妯垮亹缁寮剁拠鐐閺佹劙宕ㄩ鐔割唲缂傚倷鐒﹂崝鏍€冩繝鍥ц摕闁绘梻鍘х粻鏌ユ煙娴煎瓨娑ф繛鍫弮瀵爼鍩￠崒婊呯杽濠殿喖锕紓姘跺Φ閹版澘绠抽柟瀛樼矊閺嬪牓姊绘担渚劸缂佺粯鍨块弫鍐閻欌偓閸ゆ洟鏌曟繝蹇擃洭妞も晝鍏橀幃妤呮晲鎼粹€茬敖闂佸憡眉缁瑥顫忔ウ瑁や汗闁圭儤鍨抽崰濠囨⒑閸涘﹥灏伴柣鐔濆洦鍎夋い蹇撴祩濡嫰姊洪崫鍕拱闁烩晩鍨跺顐﹀箛閺夊灝鑰垮┑鈽嗗灦閺呮煡鎼规惔銊︹拻濞达絿鐡旈崵鍐煕閻樿櫕绀堥柛鎺撳笒閳诲酣骞橀幍浣镐壕濞达絿纭跺Σ鍫ユ煏韫囨洖顫嶇憸鏃堝蓟濞戙垹唯闁靛繆鍓濋悵鏍р攽閳╁啫绲绘繛宸弮瀵鏁愰崱妯哄妳闂侀潧绻嗛弲娑㈠疾闁秵鈷戦梻鍫氭櫇缁夌敻鏌涢敐蹇曞埌闁伙絿鍏橀獮瀣晝閳ь剛绮堢€ｎ偁浜滈柡宥囨暩缁嬪鏌￠崪鍐ɑ缂佺粯绻堝Λ鍐ㄢ槈濡嘲浜惧┑鐘冲焹閳ь剨绠撳畷鐓庘攽閹邦剚娅婃繝鐢靛█濞佳兠洪姀锛勵洸婵犲﹤鐗婇悡娑氣偓骞垮劚閸燁偅淇婇崸妤佺厓妞ゆ牗绋掔粈瀣煛瀹€瀣М闁轰焦鍔欏畷鎯邦槻妤犵偛顑呴埞鎴︻敊绾嘲濮涚紓渚囧櫘閸ㄥ爼鐛箛娑樺窛闁哄鍨电粣娑欑節閻㈤潧孝闁稿﹦鎳撻埢鎾澄熼懡銈囩槇闂佹眹鍨藉褎绂掗埡鍌樹簻闁哄洨鍠撻惌鎺撴叏婵犱胶鐭欑€规洜鍠栭、姗€鎮╅悽闈涘Ы闂佽楠哥粻宥夊磿闁单鍥ㄥ閺夋垹锛欓梺缁樺灱婵倝鎮￠弴鐔翠簻妞ゆ挾鍠庨悘銉ッ瑰鍡椥撶紒缁樼洴閹虫牠鍩℃担鍦偓濠氭⒑鐎圭媭娼愰柛銊ユ健閵嗕礁鈻庨幋鐘烩攺闁诲函缍嗛崜娆愭叏婵傚憡鈷掑ù锝堟閵嗗﹪鏌￠崨顔剧煉闁诡啫鍥ㄥ亹閻犲洤寮跺Σ顒€鈹戦悙鏉戠仧闁搞劌婀辩划濠氭晲婢跺鍘藉┑鈽嗗灠閸氬寮抽埡鍛叆婵炴垶鐟ч惌鎺楁煛瀹€瀣ɑ闁诡垱妫冮、娑橆煥閸涘拑缍佸娲川婵犲海鍔烽梺鍝勬媼閸嬪嫰鍩ユ径鎰仺缂佸娉曢ˇ顓熺箾鏉堝墽绉繛鍜冪秮閺佸秴顭ㄩ崼鐔叉嫼缂傚倷鐒﹂埣銈夘敂閸繄鏌堥梺绉嗗嫷妾璺哄閸嬫捇鏁愭惔鈥愁潾缂佺偓鍎抽妶绋款嚕閸洖閱囨繛鎴灻‖鍫㈢磽娴ｈ娈曟慨妯稿姂閸┾偓妞ゆ巻鍋撻柣蹇旇壘椤灝螣鐏忔牕浜炬慨妯煎帶濞呭秹鏌涢埞鎯т壕婵＄偑鍊栫敮鎺楁晝閿斿墽鐭撻柣銏犳啞閻撴洟鎮楅敐搴濈盎妞ゅ浚鍘鹃埀顒冾潐濞诧箓宕滃顒夊殫闁告洦鍓涚粻楣冩煏韫囧﹤澧叉い銉ョ墦閺岀喖顢欑憴鍕彋濠碘槅鍋勯崯顐﹀煡婢跺á鏃堝礃閳轰胶顦扮紓鍌氬€搁崐鎼佸磹閻戣姤鍤勯柛鎾茬閸ㄦ繃銇勯弽顐粶闁搞劌鍊婚幉鎼佹偋閸繄鐟ㄩ梺缁樺笒閻忔岸濡甸崟顖氱闁挎繂妫涢妴濠勭磽娴ｇ柉鍏岄柛蹇旓耿瀵鈽夊锝呬壕闁挎繂楠告禍婊冣攽椤旇偐效闁哄本鐩崺鐐哄箚瑜屾竟鏇炩攽閿涘嫬浜奸柛濠冪墱閺侇喗绻濋崶銊ユ畱闂佸壊鍋呭ú鏍偂閳ь剟姊洪崫鍕偍闁搞劍妞介幃锟犲Ψ閿旇棄寮垮┑顔筋殔濡宕濋幒鏃€鍠愰柣妤€鐗嗙粭姘舵煕鐎ｎ偄濮嶉柡灞熷棛鐤€闁挎繂鎳嶇花瀛樼箾鐎电袨闁搞劌缍婇崺鐐哄箣閿旂粯鏅╃紒缁㈠弮閺€閬嶅极閹€鏀介柨娑樺娴犫晜淇婇銏狀伂闁诲繐顑夊娲川婵犲倸顫囬梺鍛婃煥椤︻垰宓勯梺鍓插亖閸庤京鐥閺屾盯顢曢敐鍥舵殹濠电偛鎳庨幊鎰板箟閹间礁绾ф繛鍡樺劤閸撳磭绱撴担浠嬪摵闁圭懓娲ら悾鐑藉箳閹存梹鐎婚梺鍦劋閸ㄧ數鏁姘ｆ斀闁绘ɑ鍓氶崯蹇涙煕閻樺啿鍝虹€殿噮鍋嗛幏鐘绘嚑椤掍焦顔曢柣鐔哥矌婢ф鏁幒妤佺叆闁靛牆妫旂换鍡涙煏閸繂鈧憡绂嶉崜褏纾藉ù锝呮惈鏍￠梺缁橆殘婵炩偓鐎殿喖顭锋俊鎼佸煛閸屾矮绨婚梻浣告啞缁诲倻鈧艾鎳樻慨鈧柕鍫濇閸樺崬鈹戞幊閸婃挾绮堟笟鈧崺銏ゅ即閵忥紕鍘搁柣蹇曞仧閸嬫挾绮堟径宀€纾奸柣妯虹－濞叉潙鈹戦敍鍕効妞わ附鎸抽弻銊モ槈濮橆剚鐏堝┑顔硷攻濡炶棄鐣烽妸锔剧瘈閹艰揪绲芥慨娲⒒娴ｈ姤銆冩繛鏉戞喘椤㈡俺顦归柨婵堝仩缁犳盯寮崒婊呮婵犳鍠楅敃鈺呭礈濞戙垺鏅繝濠傚暊閺€鑺ャ亜閺冨倻鍙撴い蹇撳閺嗭附銇勯幒鎴濃枈闁绘垼妫勯悞鍨亜閹哄棗浜鹃梺瀹狀潐閸ㄥ潡骞冨▎鎴斿亾濞戞顏堟瀹ュ洨纾奸柣鎰靛墯缁惰尙绱掓径濠勭Ш濠碘剝鎸冲畷姗€鍩￠崘顏嗘闂備胶顭堥張顒傚垝瀹ュ應鏋斿Δ锝呭暞閳锋垿鏌涘┑鍡楊仾缂佷緡鍋嗙槐鎺撴綇閵婏妇顦伴梺璇″枛濞硷繝宕洪埀顒併亜閹烘垵顏柍閿嬪灴閺屾盯鏁傜拠鎻掔哗濠碉紕鍋撻幃鍌炲蓟濞戙垹惟闁宠　鍋撻柟鎻掑⒔閳ь剚顔栭崳顕€宕滈悢鐓庢槬闁逞屽墯閵囧嫰骞掑鍡╂▊缂備浇顕уΛ婵嬪蓟閿濆绠涢柛蹇撴憸閻╁酣姊洪柅鐐茶嫰婢ь垶鏌ｅΔ浣圭闁诲繐顑嗙换娑㈠箻閺夋垹鍔伴梺绋款儐閹瑰洭寮婚妸銉㈡斀闁割偅绻€濞岊亪鎮楅崹顐ｇ凡閻庢凹鍘鹃幑銏犫槈閵忊€斥偓閿嬨亜閹烘垵鈧敻宕戦幘瓒佹椽顢旈崨顏呭缂傚倷绶￠崹鍗灻洪弽銊︽珷闁告瑥顦禍婊勩亜閹捐泛浠︾€瑰憡绻勭槐鎺楊敊閸撗冪闂侀潧鐗炵紞浣哥暦濮椻偓閸╋繝宕橀妸銉ь啈闂傚倸鍊峰ù鍥綖婢舵劕纾块柟鎯版閻ゎ噣鏌涜椤ㄥ棝宕曟繝鍥ㄧ厪濠电偟鍋撳▍鍡涙煟閹捐泛鏋涢柡宀€鍠栧畷婊嗩槻濞寸姵妞介弻娑氣偓锝冨妼閻忔煡鏌＄仦绯曞亾瀹曞洦娈曢梺閫炲苯澧寸€规洑鍗冲浠嬵敇濠ф儳浜惧ù锝囩《閺嬪酣鏌熼悙顒佺稇濞寸姴銈搁弻锝堢疀閺囩偘娌梺绋块閸熷潡鍩㈠鍜佹僵闁煎摜鏁搁崢浠嬫煙閸忚偐鏆橀柛鈺佸瀹曨垵绠涘☉娆戝幈闂佺粯锚绾绢厽鏅堕悽鍛婄厵妞ゆ柨鎼崝婊呯磼鏉炴壆鐭欑€规洦鍋婃俊鐑藉Ψ瑜嬮崑濠囨⒒娴ｇ瓔鍤欓柛鎴犳櫕缁辩偤宕卞Ο纰辨锤闂佸搫绋侀崢鑲╃磼閳轰急褰掓偐瀹割喖鍓遍梺缁樻尰閻熲晛顫忓ú顏嶆晣闁靛ě鍛闂佹眹鍩勯崹顒勫礉閹达箑钃熺€广儱顦导鐘绘煕閺囥劌澧繛鍛€濆娲箮閼恒儲鏆犻柣銏╁灲缁绘繂顕ｇ拠娴嬫婵﹩鍋呴崟鍐⒑閸涘﹥瀵欓柛鏇ㄥ亝閻濐垶姊婚崒娆掑厡妞ゎ厼鐗忛幑銏ゅ箛閻楀牆鍓归梺绋跨灱閸嬫盯鎷戦悢鍏肩厽闁哄倸鐏濋幃鎴︽煙椤栨粌浠﹂柕鍥у楠炲洭宕滄担绋跨厒缂備礁澧介崑鎾寸箾婵犲偆娼栭柧蹇氼潐鐎氭岸鏌嶉妷銊︾彧闁诲繐绉瑰娲箰鎼达綆鏆￠梺闈涙搐鐎氫即銆侀弴銏℃櫜闁搞儮鏅濋弳顓㈡⒒娴ｅ憡璐￠柟铏尵閳ь剚鐭崡鎶界嵁閺嶎厼鎹舵い鎾跺枎閻濅即姊洪悙钘夊姤婵炲懏娲熷鎶筋敂閸曘劍鏂€闂佺粯锚閻忔岸寮抽埡浣叉斀妞ゆ梹鍎抽崢瀵糕偓娈垮櫘閸嬪﹤鐣烽妸锔剧瘈闁告劑鍔屾导搴ㄦ⒒娴ｅ湱婀介柛銊ㄦ椤洩顦圭€殿喗鎮傚畷濂稿煑閳轰椒澹曞Δ鐘靛仜閻忔繈宕濆顓滀簻闁挎棁顕ч悘锝咁熆鐟欏嫭绀夐柕鍥ㄥ姍楠炴帡骞樼捄鍝勭疄濠碉紕鍋戦崐鏍礉瑜忓濠囧礈瑜忛惌鍡涙煕閹伴潧鏋熼柣鎾跺枛閺岀喖骞嗚閼稿綊鏌涢弮鎾剁暤闁哄备鈧磭鏆嗛悗锝庡墰閻﹀牓鎮楃憴鍕闁绘牕銈稿畷娲焺閸愨晛顎撴俊鐐差儏鐎涒晛鐣烽悢鍏尖拻闁稿本鐟х粣鏃堟煃瑜滈崜娑㈠磻濞戙垺鍤愭い鏍ㄧ⊕濞呯姵銇勯弽顐沪闁绘挾鍠栭弻宥夊煛娴ｅ憡娈茬紓浣插亾闁割偁鍎查悡?
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

// ---------------------------------------------------------------- WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存い銏℃礋閺佹劙宕卞▎娆戠┛闂傚倷绶氶埀顒傚仜閼活垱鏅堕崜褏纾奸柣妯挎珪鐏忣參鏌ｉ敐澶嬫暠缂佽櫣鏅划娆戞嫚娣囧崬濡介柕鍥у瀵噣宕堕‖顔芥尰缁绘盯宕ㄩ鐘测叺濠殿喖锕︾划顖炲箯閸涙潙宸濆┑鐘插€瑰▓姗€姊绘担鍛婂暈婵☆偅鍨圭划濠氬箻瀹曞洦娈炬繛鏉戝悑濞兼瑩宕橀埀顒€顪冮妶鍡楀闁糕晛瀚灋婵犲﹤鐗婇埛鎴犵磼婢跺﹥顥滈悗姘槻閳绘捇寮撮姀锛勫帗闂備礁鐏濋鍛存倶閿曞倹鐓忛柛銉戝喚浼冨Δ鐘靛仦鐢€崇暦閸楃儐娓婚柟顖嗗本顥＄紓鍌氬€搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾閽樻繃銇勯弽顐㈠壉闁轰礁顑嗛妵鍕箻鐠虹洅銏ゆ煕閵婏妇绠撻柍瑙勫灴閹晝鈧湱濮撮ˉ婵嬫⒑閸涘﹤濮€闁稿鎸绘穱濠囨倷椤忓嫧鍋撻弽顓炵婵°倕鎳庣粣妤呭箹濞ｎ剙鐏い鈺傜叀閺岋綁骞囬棃娑樷拻濠电偛鐗呯划娆撳蓟閿濆绠涙い鏍ㄦ皑濮ｃ垺绻涚€电袨闁搞劋绮欏璇测槈閵忕姷鐫勯梺閫炲苯澧伴柛鐘诧工椤撳吋寰勬繛顫畵閺屾盯寮撮妸銉т画闂佸搫鎳忕划鎾诲箖瑜版帒鐐婃い蹇撳濮ｃ垺绻涚€电袥闁哄懏鐩垾鏃堝礃椤斿槈褔鏌涢埄鍐剧劷闁告瑥妫涚槐鎾存媴閾忕懓绗￠梺鍛婃⒐閻熲晛鐣峰ú顏呭€烽柛婵嗗椤撴椽姊洪幐搴㈢５闁稿鎸婚〃銉╂倷椤忓嫮浼堥梺缁樻惄閸嬪﹤鐣烽崼鏇炍╅柨鏃囶潐鐎氬姊绘担鍛婃儓妞わ缚鍗抽獮鍐磼濮樺吋缍庣紓鍌欑劍钃卞┑顖涙尦閺屟嗙疀閺囩喎娈岀紓浣诡殣缁绘繂顫忓ú顏勭閹兼番鍨诲▓銈夋⒑闁偛鑻晶顖涖亜閺冣偓閻楃姴鐣烽弶搴撴闁靛繒濮靛Σ顒勬⒑閸濆嫮鈻夐柛妯垮亹缁牓宕卞缁樻杸闂佺粯蓱瑜板啴寮抽悢鍏尖拻闁告洦鍋勯顓㈡煛鐏炲墽鈽夐柍钘夘槸椤粓宕煎┑鍡╂浆闂?
// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婇悞锕傚磿閹剧粯鈷戦柟鑲╁仜婵″ジ鏌涙繝鍌涘仴鐎殿喛顕ч埥澶愬閳哄倹娅囬梻浣瑰缁诲倸螞濞戔懞鍥Ψ瑜忕壕钘壝归敐鍛儓鐏忓繘姊洪崨濠庢畷濠电偛锕ら锝囨嫚濞村顫嶉梺闈涚箚閺呮粓寮插┑瀣拺闁圭娴风粻鎾翠繆椤愶絿娲撮挊婵嬫煏婢诡垰鎳愰敍婊堟⒑缂佹ê濮﹂柛鎾寸懅缁牓宕橀埞澶哥盎濡炪倖鍔戦崺鍕ｉ崷顓犵＜鐎光偓閸愵喖鎽电紓浣虹帛缁诲牆鐣烽崼鏇椻偓鏍敄椤掆偓濞诧箓鎮￠弴鐔虹闁瑰瓨绻傞懜褰掓煛鐎ｎ偆娲撮柡宀嬬到閳规垿宕堕妸銏″缂?WARP 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婇悞锕傚磿閹剧粯鈷戦柟鑲╁仜婵″ジ鏌涙繝鍌涘仴鐎殿喛顕ч埥澶愬閳哄倹娅囬梻浣瑰缁诲倸螞濞戔懞鍥Ψ瑜忕壕钘壝归敐鍛儓鐏忓繘姊洪崨濠庢畷濠电偛锕ら锝囨嫚濞村顫嶅┑鈽嗗灦閺€閬嶅棘閳ь剟姊绘担鍛婂暈婵炶绠撳畷鎴﹀礋椤掍礁寮块梺闈涚箞閸婃牠鍩涢幋鐐电闁煎ジ顤傞崵娆愵殽閻愭惌娈滈柡宀€鍠栭幃鐑芥偋閸繃鐏庨柣搴㈩問閸犳牠鈥﹂悜钘夋瀬闁归偊鍘肩欢鐐测攽閻樻彃顏撮柛姘嚇濮婄粯鎷呴悷閭﹀殝缂備浇顕ч崐姝岀亱濡炪倖鎸鹃崐锝呪槈閵忕姷顦板銈嗘尵婵兘鏁嶅┑鍥╃閺夊牆澧界粙鑽ゆ喐閺夊灝鏆炵紒鍌氱У閵堬綁宕橀埞鐐缂傚倷绀侀鍡欌偓绗涘喛鑰块柟娈垮枤绾惧ジ鎮楅敐搴′簻婵炴惌鍠氶埀顒冾潐濞测晝绱為埀顒傜磼閻樺磭娲寸€殿噮鍓熷畷婊嗩槾闁伙綁绠栧缁樻媴閾忕懓绗￠梺鍛婃⒐閻熲晞鐭鹃梺绋跨灱閸嬫盯宕ョ憴鍕闁糕剝顨堢拹鈺呮煟閻旂厧浜伴柣鎾卞劦閺屾盯顢曢悩鑼紕闂佸搫妫崑鍕煘?闂?
//   1) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存鐐达耿閹崇娀顢楁径瀣撴粓姊绘担瑙勫仩闁告柨绉堕幑銏ゅ礃椤斿槈锕傛煕閺囥劌鐏犻柛鎰ㄥ亾婵＄偑鍊栭崝锕€顭块埀顒佹叏鐟欏嫷娈滄慨濠冩そ閺屽懘鎮欓懠璺侯伃婵犫拃鍐ㄢ挃缂佽鲸甯￠幃鈺呮惞椤愩垻顣查梻浣风串缂嶄線寮ㄦ潏鈺冪处闁伙絽鐬奸惌娆撴偣閸ワ箑瀚庨柛鏃€鍨垮濠氬即閻旈绐炲┑鐐村灟閸ㄦ椽顢撳澶嬪仩婵ê澧界粔顔芥叏婵犲偆鐓肩€规洘甯掗埢搴ㄥ箣椤撶啘婊堟⒒娴ｅ憡璐￠柍宄扮墦瀹曟垿宕卞▎蹇撶亰濠电偛妫欓崝妤呮偡瑜版帗鐓冪憸婊堝礈濞嗘挻鍋╅柣鎴ｆ椤懘鏌ㄥ☉妯侯伃婵＄虎鍣ｅ娲川婵犲嫧妲堥梺鎸庢磸閸婃繈宕洪埀顒併亜閹哄棗浜惧┑鐐点€嬬换婵嗩嚕鐠囨祴妲堥柍鍨涙杹閸嬫捇鏁冮崒姘鳖吅濠电娀娼ч幊鎰板箰婢跺ň鏀介柣鎰皺閹界姷绱掗鑲┬ら柛鎺撳浮楠炴鎷犻懠鑸垫啺闂備胶绮Λ渚€濡撮埀顒勬煕鐎ｎ偅宕岄柟顔挎閳绘挾鎹勯妸銉バ繝鐢靛仜椤曨厽鎱ㄩ悽闈涘灊鐎广儱顦闂佸憡娲﹂崹浼村础閹惰姤鐓熼柡鍐ㄦ处椤忕娀鏌ｉ幘鑼獢婵﹥妞藉Λ鍐ㄢ槈濞嗘劖鍊烽梺璇插閻噣宕￠崘宸殨妞ゆ劧绠戠粈鍐┿亜閺冨洤浜归柨娑欑矒濮婃椽妫冨☉姘暫濠碘槅鍋掗崢鑹扮亱闂佸搫绋侀崑鍡欏?connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幈闂佸綊鍋婇崹鎵閿曞倹鐓熼柕蹇曞閻撳吋鎱ㄦ繝鍕笡闁瑰嘲鎳橀幃鐑芥焽閿曗偓濞堟繃绻濋悽闈涗粶鐎殿喖鐖煎畷褰掑锤濡も偓缁犳牠鏌ｉ幇顔煎妺闁哄懏绻堥弻娑氫沪閻愵剛娈ら梺姹囧€曞ú顓㈠箖濡ゅ懎绀傚璺猴梗婢规洟姊绘担鍛婂暈婵炶绠撳畷妤€顫滈埀顒勭嵁?disconnected      -> 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鏁愭径濠勵吅闂佹寧绻傞幉娑㈠箻缂佹鍘辨繝鐢靛Т閸婂綊宕戦妷鈺傜厸閻忕偠顕ф慨鍌溾偓娈垮枟閹告娊骞冨▎寰濆湱鈧綆浜欐竟鏇㈡⒑瑜版帗锛熼柣鎺炲閳ь剚鑹鹃幊姗€寮婚敐澶婃闁割煈鍠楅崐顖炴⒑缂佹ɑ灏紒缁樺姍閸╃偤骞嬮敂钘夆偓鐑芥⒒閸喓鈼ュ瑙勬礀閳规垿鎮欓崣澶婃闂佸湱鎳撳ú顓烆嚕婵犳艾鐒洪柛鎰ㄦ櫅椤庢挾绱撴担鍓插剱妞ゆ垶鐟︾€靛ジ宕橀瑙ｆ嫼闂佸憡绻傜€氼厼锕㈤幍顔剧＜閻庯綆鍋勯悘鎾煕閳瑰灝鍔滅€垫澘瀚换娑㈡倷椤掑倵鍋撴繝姘拺鐟滅増甯掓禍浼存煕閻樺啿鍝洪柛鈹垮灱缁犳稑鈽夊▎灞惧濠电偠鎻徊浠嬪箟閿熺姴绠氶柛顐犲劜閸婂灚鎱ㄥΟ绋垮姎濠殿喖鍊婚埀顒冾潐濞叉ê煤閻旂厧绠栭柣锝呯灱閻瑩鏌ら幇浣哥仭闁硅弓鍗冲缁樻媴閸涘﹥鍎撳┑鐐存綑鐎氼喖危閹版澘鍗抽柕蹇娾偓鍏呯棯闂備線鈧偛鑻晶鎾煛瀹€瀣ɑ闁诡垱妫冮、娑橆煥閸涘拑绲剧换娑氣偓娑欘焽閻鏌ｅΔ鍐ㄐ㈤柣锝囧厴楠炲鏁冮埀顒傜不婵犳碍鍋ｉ柛銉ｅ妼缁插鏌涢悙顏勫妞ゎ亜鍟存俊鍫曞幢濡儤娈梻浣告憸婵挳鏌婇敐澶婃槬闁逞屽墯閵囧嫰骞掗幋婵囨缂佺虎鍘兼晶搴ｆ閹烘挻缍囬柕濠忕畱闂夊秹姊洪悷鏉挎Щ闁硅櫕锕㈤獮濠囧冀椤撶偟鍔撮梺鍛婂姂閸斿孩瀵煎畝鍕€甸悷娆忓缁岃法绱撳浣镐壕缂傚倷鑳舵慨鐢告儎椤栨凹鍤曟い鎺戝缁€鍫㈡喐韫囨稒鍋╅柣鎰靛厸缁诲棝鏌ｉ幇鍏哥盎闁逞屽墯閻楃娀骞冭铻栭柛娑卞幒缁卞爼鏌熼崗鑲╂殬闁搞劌顭烽妴鍛存倻閼恒儳鍘甸梺缁樺灦閿曗晛鈻撻弴銏＄厽闁规儳鐡ㄧ粈鍐煙娓氬灝濡奸摶锝夋煟閹炬娊顎楀ù鐘成戠换娑氣偓娑欘焽閻棝鏌涘Δ浣糕枙鐎殿喖顭烽弫宥夊礋椤忓懎濯伴梻浣告啞閹稿棝鍩€椤掆偓鍗遍柛顐犲劜閳锋垿鎮楅崷顓烆€岄柛銈嗙懄缁绘盯骞橀鑺ユ殸缂備礁鍊圭敮锟犲极閸愵喖纾兼慨姗€纭搁崥鍛存⒒娴ｅ憡璐￠柛搴涘€濆畷娲醇濠?
//   2) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存鐐达耿閹崇娀顢楁径瀣撴粓姊绘担瑙勫仩闁告柨绉堕幑銏ゅ礃椤斿槈锕傛煕閺囥劌鐏犻柛鎰ㄥ亾婵＄偑鍊栭崝锕€顭块埀顒佹叏鐟欏嫷娈滄慨濠冩そ閺屽懘鎮欓懠璺侯伃婵犫拃鍐ㄢ挃缂佽鲸甯￠幃鈺呮惞椤愩垻顣查梻浣风串缂嶄線寮ㄦ潏鈺冪处闁伙絽鐬奸惌娆撴偣閸ワ箑瀚庨柛鏃€鍨垮濠氬即閻旈绐炲┑鐐村灟閸ㄦ椽顢撳澶嬪仩婵ê澧界粔顔芥叏婵犲偆鐓肩€规洘甯掗埢搴ㄥ箣椤撶啘婊堟⒒娴ｅ憡璐￠柍宄扮墦瀹曟垿宕卞▎蹇撶亰濠电偛妫欓崝妤呮偡瑜版帗鐓冪憸婊堝礈濞嗘挻鍋╅柣鎴ｆ椤懘鏌ㄥ☉妯侯伃婵＄虎鍣ｅ娲川婵犲嫧妲堥梺鎸庢磸閸婃繈宕洪埀顒併亜閹哄棗浜惧┑鐐点€嬬换婵嗩嚕鐠囨祴妲堥柍鍨涙杹閸嬫捇鏁冮崒姘鳖吅濠电娀娼ч幊鎰板箰婢跺ň鏀介柣鎰皺閹界姷绱掗鑲┬ら柛鎺撳浮楠炴鎷犻懠鑸垫啺闂備胶绮Λ渚€濡撮埀顒勬煕鐎ｎ偅宕岄柟顔挎閳绘挾鎹勯妸銉バ繝鐢靛仜椤曨厽鎱ㄩ悽闈涘灊鐎广儱顦闂佸憡娲﹂崹浼村础閹惰姤鐓熼柡鍐ㄦ处椤忕娀鏌ｉ幘鍐测偓鑽ゆ閹惧瓨濯村ù鐘差儏閹界敻鏌ｉ姀鈺佺仭妞ゃ劌锕悰顔跨疀濞戞ê绐涢梺鍝勵槹閸ㄥ磭妲愰崼鏇熲拺闁告稑锕ョ粈鈧梺璇茬箲閼规崘顣炬繝銏ｅ煐閸旀牠鍩?connected 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愰崨鍌滃枎閳诲酣骞嗚椤斿嫮绱撻崒娆掑厡濠殿喗鎸抽幃妯侯潩鐠轰綍锕傛煕閺囥劌鏋ら柣銈傚亾闂備礁婀遍崑鎾诲箚鐏炶娇娑㈡倷閻㈢數锛濇繛杈剧悼閺咁偊宕奸鍫熺厱濠电姴鍟扮粻鐐碘偓娈垮枛椤兘寮崘顔肩劦妞ゆ帒鍟版禍浠嬫⒒娴ｈ櫣甯涢柤褰掔畺椤㈡岸顢橀悩鐢电劶闂佺硶鍓濋妵娑㈠绩娴犲鐓熼柟閭﹀幗缂嶆垿鏌ｈ箛銉╂闁靛洤瀚伴、姗€鎮欏ù瀣壕闁割煈鍣崵鏇熴亜閺囨浜鹃悗瑙勬礈閸樠囧煘閹达箑骞㈡繛鍡樺灥閺勩儲绻濈喊澶岀？闁稿鍨垮畷鎰板冀椤撶偟顔囨俊銈忕到閸燁偆澹曟繝姘厽闁归偊鍓氶幆鍫㈢棯閻愵剙顕滈柕鍥у缁犳盯骞橀弶鎴烆仧缂傚倷娴囬褍螞濠靛钃熼柨婵嗩槸缁犳盯鏌涜箛鎾存喐闁烩晛閰ｅ娲焻閻愯尪瀚板褜鍨堕弻锝呂旈崘銊㈡瀰闂佺粯渚楅崳锝夌嵁閸ヮ剙绾ч柛褎顨呴弫褰掓⒒娴ｅ憡鎯堟繛灞傚姂瀹曟垵鈽夐姀鈥斥偓鍨亜閹惧崬鐏柛瀣у墲缁绘繃绻濋崒娑樻濡炪倧绲介妶鎼佸箖娴犲鏁嶆繛鎴ｉ哺閻や線鎮楃憴鍕婵炲弶鐗為悘鍐╃箾鏉堝墽绉い銉︽尵缁顫濋懜纰樻嫼闂傚倸鐗婄粙鎾存櫠閺囥垺鐓欓柛鎰皺缁犺崵鈧娲栭崯璺ㄥ弲濡炪倕绻愰幊鎰板储娴犲鈷戦悷娆忓閸斻倗鈧娲﹂崜鐔风暦濠靛棭鐓ラ柛娑卞灣閿涙粎绱撻崒娆戝妽妞ゎ厼娲ㄧ划濠氭倷缂堢姷绠氬銈嗗姂閸ㄥ綊寮冲▎鎾寸厓闁芥ê顦藉Σ鎼佹煃鐠囨煡顎楅摶锝吤归崗鑲╂噮缁炬儳鍚嬬换婵堝枈婢跺瞼锛熼梺绋款儐閸ㄥ灝鐣烽幇鏉跨闁挎洍鍋撶痪鎯ф健閺屻倗鍠婇崡鐐差潻闂佹悶鍔嶇换鍫ュ蓟閻旂厧绠查柟浼存涧濞堫厾绱撴担鍝勑ｉ柣妤佺矒閹偓妞ゅ繐鐗滈弫鍥煟閹邦厽缍戞い搴㈢☉閳规垿鍩勯崘銊хシ濡炪値鍘奸崲鏌ワ綖韫囨稒鎯為柛蹇擃槸娴滈箖鏌ｈ閹倸顬婇鐣岀瘈闁靛繆鍩勯幋鐐碘攳濠电姴娲ら柨銈嗕繆閵堝嫯鍏岄柛姗€娼ц灃闁绘﹢娼ф禒婊堟煟韫囨梻绠為柟顔斤耿閸╋繝宕ㄩ鎯у笚闂備浇濮ら敋妞わ箒妫勮灒闁逞屽墯缁绘繂鈻撻崹顔界亐闂佺顑嗛幑鍥ь潖閾忓湱纾兼慨妤€妫涢崝鐑芥⒑鏉炴壆顦︽い顓犲厴楠炲啯瀵奸弶鎴狀啋缂傚倷鐒﹁彜闁归绮换娑欐綇閸撗勫仹闂佺儵鍓濋弻銊┾€﹂崶顒€绠涢柣妤€鐗忛崢闈涱渻閵堝棛澧慨妯稿姂閹偤宕妷銏犱壕閻熸瑥瀚壕鎼佹煕閵娿劍顏犵紓鍌涙崌閹儳鐣濋埀顒勬儗濞嗘挻鍋ｅ〒姘煎灣閸樻稒淇婇懠璺虹厫濞ｅ洤锕幃娆擃敂閸曘劌浜鹃柡宥庡幖閽冪喖鏌曟繝蹇涙婵炲懐濞€閺岋綁鏁愰崨鏉款伃濡炪們鍎插畝鎼佸蓟閿曗偓铻ｅ〒姘煎灡閿涘棝鎮峰鍛暭閻㈩垱顨婂鏌ュ蓟閵夛妇鍘卞銈嗗姦閸嬪嫭绂嶅鍕闁割偆鍠庢慨鍥煃鐟欏嫬鐏撮柟顔界懇閹崇娀顢楅埀顒劼烽埀顒佺節濞堝灝鏋涢柨鏇樺€楃槐鐐寸節閸愌呯畾闂佹眹鍨婚…鍫㈢矆鐎ｎ偁浜滈柡宥冨妿閳洘绻涢崨顓燁棦婵﹨娅ｇ槐鎺懳熼崫鍕戞洟姊洪崨濠冨鞍闁艰宕橀。楣冩⒑閸撴彃浜栭柛搴㈢叀瀹曟劙宕归銈囶啎闂佸壊鍋呯换鍕閵忋倖鐓涢悗锝庡亞閳洟鏌曢崶褍顏い銏★耿閹瑩宕ｆ竟顓涙櫊濮婄儤瀵煎▎鎴炲仹闂佺绻戣摫濞ｅ洤锕獮鏍ㄦ媴閸濄儱骞愬┑鐘垫暩婵挳宕姘ｆ瀺鐎广儱顦伴悡鐔兼煟閺傛寧鎲搁柣顓烇躬濮婅櫣鏁鎯у煂闂佷紮绲块崗姗€鐛Ο鑲╃＜婵☆垵妗ㄧ紓鎾绘⒒娴ｅ憡鍟炵紒瀣灱婢ф繈姊鸿ぐ鎺撴暠婵＄偠妫勯～蹇撁洪鍕獩婵犵數濮寸€氀兾ｉ崼銉︹拺闁圭瀛╃壕鎼佹煕鎼淬垹鈻曢柛鈹惧亾濡炪倖宸婚崑鎾绘煟韫囨柨鍝洪柟顔惧厴閸╋繝宕ㄩ鈩冩啺婵犵數鍋為崹鍫曟晝閳哄懏鍋傞柕澶嗘櫆閻撳啰鎲稿鍫濈闁绘梻鍘х粻鏍煟閹邦喖鍔嬮柡鍛箓闇夐柨婵嗘缁插鏌熼鑽ゅ妽缂佺粯绻堥幃浠嬫濞磋翰鍎甸弻鈩冩媴閸涘﹤鏆堥梺閫涚┒閸旀垿骞冨鍫熷癄濠㈣泛顑囬埀顒夊墴濮婇缚銇愰幒鎿勭吹缂備讲鍋撳ù锝呮惈椤ユ艾鈹戦悩宕囶暡闁绘挾鍠栭弻鐔兼焽閿曗偓婢ь垶鏌熼姘卞ⅵ闁哄本鐩俊鎼佸Ψ閵夈垹浜鹃柛褎顨呴拑鐔哥箾閹存瑥鐒洪柡浣稿暣閺屻劌鈹戦崱姗嗘￥濡炪倧璐熼崕宕囨閹惧瓨濯撮柛婵嗗珔閿熺姵鐓曟俊銈勭閳绘洜鈧鍠涢褔鍩ユ径鎰潊闁冲搫鍊瑰▍鍥⒒娴ｇ懓顕滅紒璇插€歌灋婵炴垟鎳為崶顒€唯闁冲搫鍊甸幏娲⒑绾懎浜归柛瀣⊕娣囧﹥绺介崨濠勫幈婵犵數濮撮崐褰掑磻閵壯€鍋撳▓鍨灕妞ゆ泦鍥х叀濠㈣埖鍔曢～鍛存煃閳轰礁鏆為柣婵囨礋濮婃椽鎳￠妶鍛呫垺绻涚拠褏鐣抽柕鍥ㄥ姍瀹曟﹢顢旈崱鏇犵憹濠电偛顕崢褔鎮洪妸鈺佺厱闁瑰濮风壕濂告倵閿濆骸浜介柛搴涘劦閺屾稒鎯旈姀鐘差潚闂佸搫鐬奸崰鏍箖閻戣姤鍋嬮柛顐ゅ枑閸婄兘姊绘担椋庝覆缂佹彃娼″缁樺緞閹邦儵锕傛煙閻楀牊绶茬紒鐘烘珪缁绘繈妫冨☉娆忣槱濠电偛鎳庡ú顓烆潖缂佹ɑ濯撮柧蹇撶畭閳ь剙锕弻娑㈠箻鐎靛摜鐤勯梺杞扮椤戝顫忛搹瑙勫珰闁肩⒈鍓涢澶愭⒑閻熺増鍟炲┑鐐诧工椤?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鏁愭径濠勵吅闂佹寧绻傞幉娑㈠箻缂佹鍘辨繝鐢靛Т閸婂綊宕戦妷鈺傜厸閻忕偠顕ф慨鍌溾偓娈垮枟濞兼瑨鐏冮梺閫炲苯澧紒鍌氱У閵堬綁宕橀埡鍐ㄥ及闂備胶鍘ч～鏇㈠磹閺囩喓顩烽梺顒€绉甸悡鐔兼煙閸喖鏆欏ù婊勫閳ь剝顫夊ú鏍偉閸忛棿绻嗘慨婵嗙焾濡插嘲鈹戦埄鍐ㄧ祷闁绘鎸搁～蹇撁洪鍕啇闂佺粯鍔栬ぐ鍐€栭崼銉︹拺缂佸灏呭銉︾箾鐏忔牑鍋撻幇浣告婵犵數濮电喊宥夊磻閸曨垱鐓熼柟鎹愭珪閹癸綁鏌ｉ敐澶岀暫婵﹨娅ｇ槐鎺戭潨閸℃鏆ラ梻浣虹帛椤ㄥ棛鍒掑畝鍐罕闂備礁鎲″ú锕傚储閸撗冾棜濠靛倸鎲￠悡鐔兼煙娴煎瓨娑у褜鍣ｉ弻娑㈠棘鐠囨祴鍋撻弽顓炍﹂柛鏇ㄥ灱閺佸洭鏌ｅΟ璇茬祷妤犵偛顑夐幃妤€鈻撻崹顔界亪濡炪値鍙冮弨杈ㄧ┍婵犲洤绠瑰ù锝堝€介妸鈺傜厪闊洤锕ゆ晶鏌ユ煛婢跺鍊愭慨濠勭帛閹峰懘鎼归獮搴撳亾婵犲洦鐓曢柟鎯ь嚟缁犵偤鏌℃担鍓插剰妞ゎ亜鍟存俊鎯扮疀濮橆剚鍊曢梻浣告惈閹虫劖鎱ㄩ幘顕嗙稏闊洦绋掗幆鐐烘煕閿旇骞橀柣鎾存尭閳规垿鎮欓崣澶樻！闂佺瀛╂繛濠傜暦濠婂牊鐒肩€广儱妫岄幏铏圭磽閸屾瑧鍔嶉柨鏇楁櫅閳绘捇寮撮悢缈犵盎闂侀潧顭堥崕濠氬绩鐠囩潿搴ㄥ炊瑜濋煬顒勬煙椤旂偓娅曠紒顔界懅閹瑰嫰鈥﹂幋鐐愌冣攽閿涘嫬浜奸柛濠冪墵閹兾旈埀顒勶綖濠靛惟闁冲搫鍊告禒楣冩⒑闁偛鑻晶瀛樻叏婵犲嫮甯涢柟宄版噽缁瑥鈻庨悙顒夋闂傚倷鑳堕…鍫ヮ敄閸涙潙绠犻幖杈剧到瀵弶绻濆▓鍨灍闁靛洦鐩畷鎴﹀箻濞ｎ兛绨婚梺闈涚箚閸撴繄浜搁鐔翠簻闁瑰墽鍋ㄩ崑銏⑩偓瑙勬处娴滄繈骞忛崨瀛樺仭闁规鍠楄闂傚倸鍊搁崐椋庢濮橆剦鐒界憸鏃堝箖瑜斿畷鍗炩槈濡偐鏋冮梺鐟板悑閹矂鎮￠崼銉ユ瀬闁糕剝绋掗悡蹇涚叓閸パ屽剰闁逞屽墯閻楃娀骞冮敓鐘冲亜闁稿繗鍋愰崢浠嬫⒑閸濆嫬鈧悂鎮樺┑鍫㈡／鐟滃海鎹㈠☉銏犲窛妞ゆ挶鍔屾慨娑㈡⒑?
//      -> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊渚楅崕鎰亜閵夈儳澧涚紒缁樼洴楠炲鈹戦崱姘厴闂備礁鎲￠幐缁樼珶閸℃瑦顫曢柟鐑橆殕閸婇攱銇勯幒鍡椾壕闂佸憡鏌ｉ崐鏍Φ閸曨垼鏁囬柣妯诲絻楠炲鎮楀▓鍨灈妞ゎ厾鍏樺顐﹀箛椤撶偟绐炴繝鐢靛Т閸犳艾螞閻斿吋鈷掑ù锝囶焾椤ュ繘鏌ｉ幘宕囧ⅵ鐎规洘婢樿灃闁告侗鍘鹃敍娆忣渻閵堝棛澧遍柛瀣仱閸╂盯骞嬮悩鐢碉紲闁诲函缍嗛崑鎺楀磿閵夆晜鐓曢幖杈剧磿鏁堥梺鍝勬湰閻╊垰顕ｉ鍕閹兼番鍨归梻顖涚節绾版ǚ鍋撻崘鑼獓闂佸憡姊归悷鈺呮偘椤斿槈鐔轰焊閺嵮傚濠电偞鍨堕顏堫敂閸偅鏅涢梺瑙勫劤椤曨厾寮ч埀顒€鈹戦悙鑼闁诲繑绻堝鎼佹偄閸忚偐鍘遍梺瀹犳〃閼宠埖鏅跺☉姘辩＜闁稿本姘ㄦ晥闂佽鍠楅悷銉╂箒闁诲函缍嗛崑鍡涘焵椤掆偓椤嘲顫忓ú顏咁棃婵炴垶姘ㄩ悿鍕⒑缁嬪尅鏀婚柣妤冨Т閻ｇ兘骞囬悧鍫濃偓濠氭煠閹帒鍔ら柛妯哄船閳规垿鎮╃紒妯婚敪濠碘槅鍋呴〃濠囥€侀弮鍫晝闁虫儼濮ゅΛ鍐箖閳哄懎鐭楀璺猴攻閻忓孩绻濆▓鍨灈闁挎洏鍎遍—鍐寠婢跺本娈惧┑鐘诧工閻楀棛绮婚悽鍛婄厵闁告繂瀚暩闂佺硶鏅涢幊妯侯潖缂佹ɑ濯村〒姘煎灣閸旂顪冮妶鍡楃仴闁硅櫕鍔欓崺鈧い鎺嶇閸ゎ剟鏌涘Ο鍝勨挃闁告帗甯為埀顒婄秵娴滃爼宕ョ€ｎ亶鐔嗛柤鍝ョ仚閹达箑鐤炬い鎺嗗亾闁宠鍨块幃鈺冩嫚瑜嶆导鎰版煟鎼淬垹鍤柛鎾存皑缁?2 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婄闁挎繂妫Λ鍕⒑閸濆嫷鍎庣紒鑸靛哺瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁诡垎鍐ｆ寖缂備緡鍣崹鎶藉箲閵忕姭妲堥柕蹇曞Х椤撴椽姊虹紒妯虹仴婵☆偅鐩畷婵嬵敍濮橈絾鏂€闂佺粯鍔栧娆撴倶閿斿浜滄い鎾跺仦閸犳鈧鍠楁繛濠傜暦閸楃偐妲堟繛鍡樺灥楠炲牓姊绘担鍛婃儓婵炲眰鍨藉畷婵堜沪閸撗屾锤闂佺粯鍔栭悾顏呯濠婂牊鐓ラ柡鍐ㄦ祩閸ゆ瑦顨ラ悙瀵稿⒌闁哄苯绉堕幉鎾礋椤愩倓绱濋梻浣筋嚃閸犳帡寮插┑鍫燁潟闁圭儤鍤﹂悢鍝ョ瘈婵﹩鍘藉▓顒佺節閻㈤潧啸闁轰焦鎮傚畷鎴︽偐濞茬粯鏅┑鐘诧工閹虫劖绋夊澶嬬厵閺夊牆澧介悾閬嶆煟閹惧绠橀柍褜鍓欑粻宥夊磿闁单鍥ㄥ閺夋垹锛涢梺褰掑亰閸犳氨澹曢挊澹濆綊鏁愰崼顐㈡異闂佺粯甯婄划娆撳蓟濞戞鐔煎箰鎼搭喗顥夐梻浣哥枃椤宕归崸妤€鏄ラ柕澶嗘櫅楠炪垺淇婇妶鍛殨婵烇綆鍣ｅ缁樻媴娓氼垳鍔稿銈嗗灥濡繂鐣峰┑鍠棃宕ㄩ鐔衡偓顒€顪冮妶鍡樼叆闁汇劌鍢插嵄闁割偁鍎查悡蹇涚叓閸ャ儱鍔ょ痪鎯ф健閺屾稑螣閸︻厾鐓撳┑顔硷攻濡炶棄鐣烽悜绛嬫晣闁绘劖褰冮‖鍡樼節濞堝灝鏋涢柨鏇樺妼閻ｇ兘鎮界粙鎸庣€銈嗘煥閻ㄥ嘲危閸儲鐓忛煫鍥ュ劤绾惧潡鏌涘Ο铏逛粵缂佺粯鐩獮妯煎鐎ｎ亶妫熼梻浣筋嚙缁绘垹鎹㈤崘鈺冣攳濠电姴鍟伴梽鍕煕濞戞﹫鏀婚柛銈咁樀濮婃椽骞愭惔锝囩暤闂佺懓鍢查澶愬箖閵忋倕骞㈡繛鎴炵懅閸橀亶姊虹憴鍕閻忓繑鐟ч幏瑙勫鐎涙鍘介梺鍦劋椤ㄥ牓鎮惧ú顏呯厸鐎光偓閳ь剟宕伴弽顓溾偓浣糕槈濮楀棙鍍甸梺鍛婎殘閸嬫劙宕ｉ悜妯肩瘈婵炲牆鐏濋弸娑㈡煥閺囨ê鍔氭い顏勫暣閹稿﹥寰勭仦鐐啎闂備線娼ч…顓熶繆閸モ晛濮柍褜鍓熷娲川婵犲啫顦╅梺鍛婃尰閻熲晛顕ｉ幖浣规櫇闁稿本绋堥幏缁樼箾鏉堝墽瀵奸悹鈧敃鍌涘€块柣鎰靛厵娴滄粓鏌熺€涙绠栨い銉ｅ灪椤ㄣ儵鎮欓弶鎴犱紝閻庤娲橀崕濂嘎ㄦ担鍦闁稿繗鍋愭晶杈╃磼鏉堛劍灏伴柟宄版嚇閹虫牠鍩勯崘銊т桓闂佸憡甯掗敃顏堢嵁濮椻偓椤㈡瑩鎮剧仦钘夌闂傚倷鑳剁划顖氼潖婵犳艾鍌ㄧ憸搴ｇ矉瀹ュ棗顕遍悗娑欘焽閸橀亶姊洪棃娑氬闁瑰嘲顑夊畷鏇㈠箣閻樼數锛滈柡澶婄墑閸斿秶浜告导瀛樼厵鐎瑰嫮澧楅崵鍥┾偓瑙勬礃缁捇鐛幘璇茬闁瑰瓨绻嶅Λ濠囨⒒閸屾艾鈧兘鎮為敃鍌氱畺闁割偅娲栫粈澶愭倶閻愰潧浜炬繛鍛█楠炴牗娼忛崜褎鍋х紒鎯у綖缁瑩寮婚悢鐓庣畾鐟滄粓宕甸悢鎼炰簻闁冲搫鍟崢鎾煙椤旂瓔娈滈柡浣瑰姈閹柨鈹戦崼鐔告闂傚倷鐒﹀鍧楀磻閸曨厸鍋撳顓熺凡妞ゎ偄绻橀幖褰掑捶椤撶姷鍘梻浣告贡椤牏鈧稈鏅犻獮蹇旀綇閳哄啰锛濇繛鎾磋壘濞层倝寮稿☉銏♀拺閻㈩垼鍠氱粔顔筋殽?10 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬倵楠炲灝鍔氭い锔诲灣缁牏鈧綆鍋佹禍婊堟煙閸濆嫮肖闁告柨绉甸妵鍕棘閹稿骸鏋犲┑顔硷功缁垶骞忛崨鏉戝窛濠电姴鎳愰、鍛存⒒娴ｄ警鐒剧紒銊︽そ瀹曟劕鈹戦崱娆愭闂佹眹鍨归幉锟犲疾濠靛鐓冪憸婊堝礈閻旂厧鏄ラ柕澶涚畱缁剁偛鈹戦悙顏勭伄闁哥姵鐗犲濠氬Ω瑜夐崑鎾绘晲鎼粹€茬凹濠电姰鍨洪崹鍨潖缂佹ɑ濯寸紒娑橆儏濞堫參姊洪崨濠傜仼闁哄拋鍋嗗Σ鎰版倷閸濆嫬鑰垮┑鈽嗗灣閳峰牆煤閹间焦鈷戠紓浣姑慨鍐偣閳ь剟鏁冮崒姘€繝鐢靛Т濞诧箓鎮￠弴銏＄厪濠㈣埖锚閺嬫稑顭胯閸ㄥ爼寮诲澶嬬叆閻庯綆浜炴导宀勬⒑鐠団€虫灈闁搞垺鐓￠崺銏℃償閵堝洨鏉搁梺鐟扮仢閸熲晠骞冮敐鍥╃＝闁稿本鐟ㄩ澶愭煕鐎ｎ偅宕岄柡灞剧洴楠炲洭濡搁敂鑺ョ翻濠电偛鐡ㄧ划宥囧垝閹捐钃熼柨鐔哄Т閻愬﹦鎲告惔銊嬪顭ㄩ崼鐔哄幍闂佹儳娴氶崑鍕叏瀹ュ鐓欐い鏍ㄦ皑閻掑摜鈧娲栧畷顒勫煡婢跺ň鏋庨柟瀛樼箓缁犵偤姊婚崒娆戠獢闁逞屽墯缁嬫垵鐣甸崱妯肩濞达絽鍟垮ú銈夊礄閻樼粯鐓熼柡鍌氱仢閹垿鏌ｉ幘璺烘灈闁哄瞼鍠栭獮鍡氼槾闁圭晫濮风槐鎺楀焵椤掑嫬绀冩い蹇撴閿涙粎绱撻崒娆戝妽妞ゎ厼娲ょ叅閻庢稒锕╁▓浠嬫煟閹邦剚鈻曢柛搴＄箻閺屽秶鎲撮崟顐や紝閻庤娲栧畷顒勶綖濠靛鏁嗛柛灞捐壘缁狅綁姊婚崒姘偓鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晜缂佹ɑ娅堥梻浣规偠閸庢椽宕滃▎鎴犵幓婵°倕鎳庣粻瑙勭箾閿濆骸澧┑锛勬櫕缁辨帡鎮╅崘鍙夎癁濠殿喖锕︾划顖炲箯閸涘瓨鎯為柣鐔稿椤愬吋淇婇悙顏勨偓銈夊储婵傚憡鍋嬮煫鍥ㄦ礀閸ㄦ繈鏌￠崘銊у⒈闁轰礁顑夐弻宥堫檨闁告挾鍠栨俊瀛樼瑹閳ь剙顕ｉ鈧畷鐓庘攽鐎ｎ亝鏆┑鐘愁問閸犳鏁冮銈呯筏闁兼亽鍎抽々鎻捨旈敐鍛殲闁绘挶鍎甸弻锟犲炊閳轰椒绮堕梺閫炲苯澧柟顔煎€垮濠氬Ω閳轰絼褔鏌涢埄鍐╃缂佺姵宀稿娲濞戞艾顣洪柣搴㈠嚬閸ｏ綀妫熼梺鎸庢⒒閺咁偆绮绘ィ鍐╃厱妞ゆ劑鍊曢弸鏃傜磼閻橀潧浠﹂柟渚垮妽缁绘繈宕掑☉娆忓壍闂佽桨绀侀悧鎾诲蓟濞戙垹唯闁瑰瓨绻冮崳鍝ョ磼鏉堛劍绀嬫慨濠勭帛閹峰懘鎳為妷锝傚亾閸愵喗鐓熼柟铏瑰仧閻ｅ灚顨ラ悙鎻掓殻鐎规洘甯掗～婵囨綇閵婏富鍟庨梻鍌欑劍鐎笛呮崲閸岀偛绠犲鑸靛姇缂佲晛霉閻樺樊鍎愰柣鎾存礃閵囧嫰骞囬澶广垽鏌ｈ箛锝呮珝闁哄被鍔岄埥澶娾枎閹寸姷鏆紓鍌欒兌缁垳绮欓幒鎴劷闊洦绋戠粈鍫㈡喐韫囨埃鍙㈡繝纰夌磿閸嬫垿宕愰妶澶婄；闁告洦鍨侀崶顒佸仺缂佸瀵уΣ顒€鈹戦悙鏉戠仧闁搞劌婀辩划璇测槈濞嗗秳绨婚梺瑙勫礃濞夋稑鏆╂俊鐐€栧鐟懊洪悢濂夋綎闁惧繐婀遍惌娆撴偣閹帒濡挎い鏃€鍨垮铏圭矙濞嗘儳鍓遍梺鍛婃⒐閻熲晠鎮伴鐣岀瘈闁搞儜鍛偓鐐差渻閵堝棗绗傞柤鍐茬埣瀹曘垽鏌嗗鍡忔嫼閻庤娲栧ú锕傚吹閳ь剟姊虹拠鑼缂佽鐗撻獮鍐潨閳ь剟銆侀弮鍫濋唶闁绘柨鎼獮宥嗕繆閻愵亜鈧牠鎮уΔ鍐煓闁圭儤顧傛径鎰唶婵犻潧鍟弬鈧梻浣规灱閺呮盯宕鐐茬９闁归偊鍏橀弨浠嬫煥濞戞ê顏柣鎺撳閵囧嫰鏁冩担鍝ユ闂侀€炲苯澧い鏃€鐗犲畷浼村箻鐠囪尙顦梺鐟扮摠缁诲秴顭囬弽顐ょ＝濞达綀鍋傞幋鐐插灁闁圭虎鍠楅悡鏇熺箾閹寸儑鍏€规悶鍎甸弻锟犲幢濡ゅ啫顤€缂備胶绮惄顖炵嵁濡皷鍋撻棃娑橆棌闁哄棌鈧剚娓婚柕鍫濆暙婵″ジ鏌ㄩ弴妯衡偓婵嬪箖妤ｅ啯鍋い鏍煎Ч妤呮⒑閸︻厼鍔嬮柛銊ョ秺椤㈡瑩宕ㄧ€涙ǚ鎷洪梺鍛婄☉閿曘儳浜搁銏＄厽闊洦姊荤粻鎾绘偂閵堝棎浜滈柟鍨暞婵炲洭鏌嶈閸忔稓绮堟笟鈧敐鐐测攽閸喎纾梺鎯х箳閹虫挻鎱ㄩ敂鎴掔箚闁绘劦浜滈埀顒佺墪椤斿繑绻濆顒傦紱闂佸湱鍋撻悾顏呯濠婂嫨浜滈柟鎵虫櫅閻忣喚绱掗悩鑼紞缂佽鲸甯楀蹇涘Ω閵壯呮噯闂佸彞绱紞渚€寮婚妶鍚ゅ湱鈧綆鍋呴悵鏇㈡⒑閹肩偛鈧洟鎯岄崒姘兼綎缂備焦蓱婵挳鎮峰▎蹇擃仼濞寸姭鏅犲铏圭矙閸噮鍔夌紓渚囧枟閹告悂鎮惧畡閭︽建闁逞屽墴楠炲啫鈻庨幙鍐╂櫈闂佸湱顭堟鎼佸绩椤撱垺鈷掗柛灞捐壘閳ь剚鎮傚畷鎰板箹娴ｅ摜锛欓悗鐟板閸ｇ銇愰幒鎴犲€炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟扮粻缁橆殽閻愭潙鐏村┑顔瑰亾闂侀潧鐗嗛幊鎰邦敇濞差亝鈷戠紓浣光棨椤忓牆鐤柟绋垮閸庣喎鈹戦悩鍙夊闁抽攱鍨块弻娑㈠箻閺夋垹绁锋繛瀛樼矋缁诲嫮妲愰幒鏃€瀚氶柟缁樺俯娴煎啴鎮楀▓鍨灍濠电偛锕顐﹀礃椤旇偐锛滃┑鐐村灦閼瑰墽鏁ィ鍐┾拻濠电姴楠告禍婊勭箾鐏炲倸鈧繈銆侀弽顓炲窛闁哄鍨归崝锕€顪冮妶鍡楃瑐闁绘帪濡囩划鍫ュ焵椤掆偓閳规垿鎮欓弶鎴犵シ闂佽绻戠换鍫ュ春閻愬搫绠ｉ柨鏃囨娴滅懓顪冮妶鍡橆梿闁稿鍔栫€靛ジ宕惰閺€浠嬫煟閹邦剚鈻曟俊鎻掔秺閺岋綁鍩℃繝鍌滀桓閻庢鍣崑濠傜暦濮椻偓椤㈡瑩鎳為妷锔惧絿闂傚倷鑳堕…鍫ヮ敄閸ヮ剙绠犻柟鎹愮М濞差亶鏁囨い顐厴閸嬫挻绗熼埀顒€顕ｉ幘顔碱潊闁斥晛鍟悵鏇㈡⒒娴ｈ棄袚闁挎碍銇勯敂璇蹭喊鐎规洘鍨块獮妯肩磼濡桨缂撻梻浣告啞缁嬫垿鏁冮敃鍌氬偍濞寸姴顑嗛埛鎴犵磽娴ｈ偂鎴︽偂閵夆晜鐓欓悹鍥囧懐锛熷┑鐐叉閸ㄨ棄鐣烽敐鍡楃窞闁割偅绮庨惄搴ㄦ⒒婵犲骸浜滄繛鏉戝瀵板﹪鎳栭埡鍕＄紓鍌欑劍鑿уù婊勭矋閵囧嫰骞樼捄杞版勃闂佺顑冮崕鐢稿蓟閻旂⒈鏁婇柟顖嗗啫绠ｉ梻浣筋嚃閸燁偊宕惰閸炶泛顪冮妶鍡楀Ё缂傚秴妫楄灋鐟滃秹鍩?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗娼欒灃闁逞屽墯缁傚秹骞嗚濞撳鏌曢崼婵堝嚬缂併劌顭烽弻娑㈡偄閸涘﹦绋囬梺浼欑悼閺屽濡甸幇鏉跨婵犻潧娲﹀▍鏃堟⒒娴ｅ憡鍟炲〒姘殜瀹曞綊鎮ч崼鐔峰伎濠电偛妯婃禍婵嬪煕閹烘嚚褰掓晲閸曨噮鍔呭┑鐐插悑鐢繝寮诲☉銏犲嵆闁绘劕鍚€缁敻姊烘导娆戝埌濠⒀冩捣濡叉劙骞樼€涙ê顎撻梺鍦帛鐢﹥绔熼弴鐐╂斀闁绘﹩鍠栭悘杈ㄧ箾婢跺鈯曠€垫澘瀚板畷鍫曨敆閳ь剛澹曟繝姘厵闁硅鍔曢崥鍦偓瑙勬尫缁舵岸寮诲☉鈶┾偓锕傚箣濠靛洨浜┑鐘愁問閸犳牗顨ラ幖浣测偓鏃堝礃椤斿槈褔鐓崶銊﹀暗婵¤缍佸娲箹閻愭彃顫呭┑鐐差槹閻╊垶銆佸鑸垫櫜闁糕剝顨忛崵銈夋⒑閸濆嫷妲归柛銈忕畵婵℃悂鍩￠崒婊冨汲闂備礁鎼崐鎼佹倶濠靛鐓曢柡鍐ｅ亾闁靛洤瀚板顕€鍩€椤掆偓鐓ら柣鏂款殠濞兼牗绻涘顔荤凹闁稿绻濆娲敇閵娧呮殸濠电偛鐗嗛悥鐓庮潖閾忓湱鐭欐繛鍡樺劤閸撴娊姊洪崫銉バｉ柣妤冨█閹即顢欓悾宀€鐦堥梺鎼炲劀閸愵亞宕洪梻浣筋嚙鐎涒晝绮欓幒妞烩偓锕傚炊椤掆偓閸屻劑鏌熼幆褍顣崇痪鍙ョ矙閺屾稓浠﹂崜褎鍣梺鍛婃煥缁夊綊骞忛幋鐐寸秶闁靛绲肩花濠氭⒑闂堟侗妲堕柛搴ゆ珪閺呭爼鏁冮崒娑氬幐閻庡厜鍋撻柍褜鍓熷畷浼村冀椤撴壕鍋撴担绯曟瀻闁圭偓娼欏▓鎰版⒑閸愬弶鎯堥柛鐑嗗弮婵℃悂鍩￠崒姘ｅ亾閻㈠憡鍋℃繛鍡楃箰椤忣亞绱掗埀顒勫焵椤掑嫭鈷戠紓浣诡焽婢ь亪鏌涘顒夊剱缂佸矁椴哥换婵嬪炊瑜忛鎰箾鏉堝墽绉柛濠佺矙婵℃瓕顦抽柛鐘冲姍閻擃偊宕堕妸褉妲堥柛銉︽尦濮婅櫣绱掑Ο铏逛紘婵犳鍠撻崐婵嬪箖閿熺姵鍋勯柛蹇氬亹閸樹粙姊鸿ぐ鎺戜喊闁告挻鐩獮妤呮偐缂佹鍘介梺闈涱煭缁犳垿鎮橀幘顔界厸閻忕偛澧藉ú鎾煙椤旇娅呴柣锝囧厴瀹曟儼顦柛姘€规穱濠囨倷椤忓嫧鍋撻妶澶婄；闁告洦鍘藉畷鏌ユ煙闁箑鏋涚€殿喖寮舵穱濠囨倷椤忓嫧鍋撻弴鐘冲床闁归偊鍠掗崑鎾愁潩闂傚鏁栧┑鈥冲级閸旀瑥鐣锋總绋垮嵆闁绘柨寮剁€氬ジ姊绘担鍛婂暈缂佸鍨块弫鍐Ψ閿曗偓缁剁偤鏌涢弴銊ョ仭闁绘挻娲熼幃妤呮晲鎼存繄鐩庨梺閫炲苯澧い顓犲厴楠炲﹪鎮欑€涙绉堕梺闈浤涚仦鍓с偒闂備浇顕ч崙鐣岀礊閸℃稑纾婚柍褜鍓熼弻鐔兼煥鐎ｎ偁浠㈠┑顔硷攻濡炶棄鐣烽妸锔剧瘈闁告洏鍔嶉～宥嗕繆閻愵亜鈧倝宕ｆ惔銊ョ闁绘梻鍘х粻鏍煟閹邦厾鏆橀柛瀣嚇閺屾盯骞囬妸锔界彇闂佸憡锕㈡禍鍫曞蓟閿濆棙鍎熸い鏍ㄧ矌鏍￠梻浣侯焾椤戝懐鈧凹鍙冮獮鍫ュΩ閳轰胶鍊炲銈嗗坊閸嬫挾鐥娑樹壕闂傚倸鍊风欢锟犲礈濞嗘垹鐭撻柣銏犳啞閸嬪倹绻涢崱妯诲鞍闁绘挾鍠愭穱濠囧Χ閸屾矮澹曢梻浣虹帛椤ㄥ懘鏁冮敃鈧埢搴ㄥ閵堝棗娈ゅ銈嗗笒閸婂鏁嶅鍫熲拺缂備焦顭囬幊鍐煙閾忣偄濮夌紒顔芥椤㈡岸鍩€椤掑嫬钃熼柨娑樺濞岊亪鎮归崶銊ョ祷妞ゎ偄娲幃妤€鈻撻崹顔界亶濠电偛鍚嬮悷銊╂倶閸愵喗鈷戦梺顐ゅ仜閼活垱鏅堕婊呯＜闁稿本姘ㄦ晥閻庤娲樼划蹇浰囩€靛摜妫?g_warpManualStop=true闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋垺鍋橀柍銉ュ帠婢规洟姊哄Ч鍥х仾妞ゆ梹鐗犻幃鐐哄箚椤€崇秺閹晛鈻庤箛鏂款槱闂佹娊鏀辩敮锟犲蓟濞戙垹鍗抽柕濞垮劙缁ㄨ顪冮妶鍛闁告瑥鍟村濠氬灳瀹曞洦娈曢柣搴秵閸撴盯鎯侀崼銉﹀€甸悷娆忓缁€鍐煥閺囨ê鐏ǎ鍥э躬楠炴牗鎷呴懖婵勫姂閺屾洝绠涢弴鐐愨€愁熆鐟欏嫭绀嬫慨濠冩そ閹剝鎯旈鐣岀◥闂備胶顭堥敃銉┿€冮崱娆戠煓濠㈣泛顭崥瀣熆鐠轰警鍎戦柛妯绘倐濮婅櫣绮欓幐搴㈡嫳婵炴挻纰嶉〃濠傜暦閺囥垹唯闁冲搫鍊婚崢浠嬫⒑閸濆嫬鈧湱鈧瑳鍥佸濮€閵堝棛鍘搁柣蹇曞仩椤曆囧焵椤掍胶绠為柣娑卞櫍瀵粙濡搁敃鈧鎾绘煟閻斿摜鎳冮悗姘嵆瀵偊顢旈崟銊︽杸闂佺粯蓱閸撴岸宕箛娑欑厱闁绘ê纾晶顏堟偂閵堝鐓忓┑鐐戝啫顏存繛鑲╁枛濮婃椽宕烽鐐插濡炪們鍔屽Λ婵嗙暦閹达箑绠涙い鎾寸矆缁ㄥ姊洪崫鍕殭闁稿﹤鎽滈弫顔炬崉鐞涒剝鏂€濡炪倖娲栧Λ娑㈡偩閸偅鍙忓┑鐘插暞閵囨繄鈧娲﹂崑濠傜暦閻旂⒈鏁囬柣妯夸含缁€鍐⒒閸屾瑧顦﹂柟鑺ョ矋閹便劑鎮介崨濠備罕闂佺粯顭堝畷闈涚暤娓氣偓閺屾盯骞囬埡浣肝ㄩ梺鍝勵儏闁帮綁寮婚悢鐓庡瀭妞ゆ柨鍚嬪▓鏌ユ⒑閸濆嫭鍣烽柛銊ㄦ閹广垹鈽夊▎鎰Ф闂佸憡鎸嗛崨鍛壘閳规垿鎮欓崣澶婄彅缂傚倸绉崇欢姘暦濞差亜顫呴柨婵嗘川閺夋悂姊洪崫鍕偓鍛婃櫠閻ｅ瞼涓嶆慨妯垮煐閳锋垹绱撴担濮戭亝鎱ㄩ敃鈧湁婵犲﹤瀚惌鎺楁煕閳规儳浜炬俊鐐€栫敮濠勭矆娓氣偓閹鈧數纭堕崑鎾舵喆閸曨剛顦ㄥ┑鐐插级閿氭い顓炴穿缁犳稑鈽夊Ο鐓庡箰闂備礁鎲￠崝锔界閻愬搫鍌ㄩ柣妯肩帛閻撶喖鏌ｉ弬鎸庢喐闁活厼鑻…鑳槼妞ゃ劌鐗撳顐︻敋閳ь剟鐛幒鎳虫梹鎷呴崫銉ョ到?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃闁硅櫕鎹囬垾鏃堝礃椤忎礁浜鹃柨婵嗙凹缁ㄧ粯銇勯幒瀣仾闁靛洤瀚伴獮鍥敂閸℃瑧鍘梻浣告惈鐞氼偊宕濋幋锕€绠栭柕蹇嬪€曟导鐘绘煕閺囩喎鐏熼柛銊ヮ煼閹偓妞ゅ繐鐗嗙粻姘辨喐濠婂牊鍋傚┑鍌氭啞閻撴盯鎮橀悙鎻掆挃闁宠棄顦甸弻宥夋寠婢舵ɑ鈻堥悗瑙勬穿缁绘繈骞冨▎鎴斿亾閻㈠憡娅滃瑙勬礋濮婂宕掑▎鎴М缂佸墽铏庨崣鍐嵁婵犲洤绠婚柛鎾茶兌閻撳鎮峰鍛暭閻㈩垱甯￠幏鎴︽偄閻戞ê鏋戦梺鐟邦嚟婵攱绋夊鍡欑闁瑰鍎戞笟娑欑箾鐏忔牗娅婇柡灞剧缁犳稑顫濋鎸庣潖缂傚倷绀侀ˇ閬嶅礂濮椻偓瀵?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幈闂佸綊鍋婇崹鎵閿曞倹鐓熼柕蹇曞閻撳吋鎱ㄦ繝鍕笡闁瑰嘲鎳橀幃鐑芥焽閿曗偓濞堟繃绻濋悽闈涗粶鐎殿喖鐖奸幃褔鎮╅懠顒佹闂佸綊鍋婇崹顒佺瑜版帗鐓欓柣鎴灻悘鈺呮煥濞戞瑧娲撮柡宀嬬稻閹棃顢涘鍛咃綁姊洪崫銉バｆい銊ワ躬楠炲啴鍨鹃弬銉︻潔闂侀潧楠忕槐鏇㈠储闁秵鈷戦悷娆忓椤ユ劙鏌￠崨顔锯姇濞ｅ洤锕ョ缓浠嬪川婵犲嫬甯楅柣鐔哥矋缁挸鐣峰鍐ｆ瀻闁圭虎鍨崑鎾绘晝閳ь剟鎮鹃敓鐘茬妞ゆ棁濮ら鏇㈡⒒娴ｅ搫浠洪柛搴㈢叀瀹曟粌顫濋澶屽姺闂佸啿鎼幊蹇涙偂閻樼粯鐓欓梻鍌氼嚟閸斿秵绻涢幊宄版处閻撳啰鎲稿鍫濈婵炲棙鎸婚崐鑸垫叏濡潡鍝虹€规洖寮剁换娑㈠箣濞嗗繒浠肩紓浣哄缂嶄線寮婚敐澶婄疀妞ゆ帒鍊风划鐢告⒑闂堚晝瀵肩紒顔界懃椤繘鎼归崷顓狅紲濠碘槅鍨靛▍锝夋偡閹邦喚纾藉〒姘搐閺嬬喖鏌ｉ悢鍙夋珔妞?204 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鏁愭径濠勵吅闂佹寧绻傞幉娑㈠箻缂佹鍘辨繝鐢靛Т閸婂綊宕戦妷鈺傜厸閻忕偠顕ф慨鍌溾偓娈垮枟濞兼瑨鐏冮梺閫炲苯澧紒鍌氱У閵堬綁宕橀埡鍐ㄥ及闂備胶鍘ч～鏇㈠磹閺囩喓顩烽梺顒€绉甸悡鐔兼煙閸喖鏆欏ù婊勫閳ь剝顫夊ú鏍偉閸忛棿绻嗘慨婵嗙焾濡插嘲鈹戦埄鍐ㄧ祷闁绘鎸搁～蹇撁洪鍕啇闂佺粯鍔栬ぐ鍐€栭崼銉︹拺缂佸灏呭銉︾箾鐏忔牑鍋撻幇浣告婵犵數濮电喊宥夊磻閸曨垱鐓熼柟鎹愭珪閹癸綁鏌ｉ敐澶岀暫婵﹨娅ｇ槐鎺戭潨閸℃鏆ラ梻浣虹帛椤ㄥ棛鍒掑畝鍐罕闂備礁鎲″ú锕傚储閸撗冾棜濠靛倸鎲￠悡鐔兼煙娴煎瓨娑у褜鍣ｉ弻娑㈠棘鐠囨祴鍋撻弽顓炍﹂柛鏇ㄥ灱閺佸洭鏌ｅΟ璇茬祷妤犵偛顑夐幃妤€鈻撻崹顔界亪濡炪値鍙冮弨杈ㄧ┍婵犲洤绠瑰ù锝堝€介妸鈺傜厪闊洤锕ゆ晶鏌ユ煛婢跺鍊愭慨濠勭帛閹峰懘鎼归獮搴撳亾婵犲啩绻嗛柣娆愮懃濞层倝鎮块悙顒傜瘈濠电姴鍊归崳浠嬫煙閸愬弶鍤囨慨濠勭帛閹峰懘宕ㄦ繝鍐ㄥ壍婵＄偑鍊х€靛矂宕戞繝鍌ゅ殨闁圭粯甯╅悡銉╂煕濞戝崬鏋涢幖鏉戯躬濮婃椽宕滈幓鎺嶇凹缂備浇顕х粔鎾€﹂崶顒€鍐€妞ゆ挾鍠撻崣鍕椤愩垺澶勯柟姝屽吹閹峰綊鏁撻悩宕囧幗濡炪倖鎸鹃崰鎾诲箠閸ヮ剚鐓涚€光偓閳ь剟宕伴幘璺哄灊婵炲棙鎸搁崹鍌涖亜閺囩偞鍣瑰┑锛勫厴濮婄粯鎷呴崨濠呯闂佸搫鑻ˇ鎵矉瀹ュ鏁嗛柛灞句緱濞肩喖姊虹憴鍕姢闁宦板妽閸掑﹦鈧潧鎽滅壕鍏肩箾閹寸儑渚涢柛搴＄箲缁绘盯宕奸銏犵睄闂佸搫鐭夌徊楣冨箚閺冨牆顫呴柣妯哄级閻︽捇姊绘担鍛婃喐濠殿噣娼ч悾婵堢矙鐠恒劍娈惧┑顔筋焾濞夋稓绮婚幎鑺ョ厵闁诡垎鍐炬殺闂佷紮绠戦悧鍡涘煘閹达富鏁婇柡鍌樺€撶欢鐢告⒑閸涘﹥灏扮紒璇茬墦閻涱喖螖閸涱厼宓嗛梺闈涚箚濡狙囧箯濞差亝鈷掗柛灞炬皑婢ф稑銆掑顓ф當妞ゎ剙锕ㄩˇ鏌ユ煃鐟欏嫬鐏撮柛銊╃畺閹煎綊顢曢姀锛勪悍闂?true 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐烘偡濠婂啰绠荤€殿喗濞婇弫鍐磼濞戞艾骞楅梻渚€娼х换鍫ュ春閸曨垱鍊块柛鎾楀懏锛忛梺璇″瀻瀹€鈧崥瀣⒑閸濆嫮鐒跨紓宥勭窔閻涱噣宕堕澶嬫櫓闂佸吋浜介崕鏌ユ儊鎼淬劍鈷掑ù锝堫潐閵囩喖鏌涘Ο鍦煓闁诡喚鍏橀崺锟犲川椤旈棿绨垫俊鐐€栭崝褏绮婚幋锔藉€峰┑鐘叉处閻撳繐鈹戦悙鎴斿亾闁稿鎸绘穱濠囧箵閹烘柨鈪辩紓浣介哺閹稿骞忛崨鏉戠闁瑰搫绉撮ˉ姘節閻㈤潧袨闁搞劌銈稿畷娲冀椤撶偟顦梺鍝勬川婵攱绋夊澶嬪仭婵炲棗绻愰顐ょ磼閻樿崵鐣洪柡灞诲妼閳规垿宕卞☉鎵佸亾濡も偓椤儻顧侀柛鐘崇墵閸╃偤骞嬮敂鑺ユ珫闂佸憡娲﹂崑鍕濡警娓婚柕鍫濆€瑰▍鍛存煏閸埄鐒鹃柣锝囧厴婵℃悂鍩℃繝鍐╂珝婵犵數鍋為崹鍫曟偡閿曞倸绾ч柛娑橈梗缁诲棝鏌ｉ幇鍏哥盎闁逞屽墯閻楁洟锝炶箛鏃傜瘈婵﹩鍘兼禒閬嶆⒑閸濆嫬鏆欓柣妤€锕畷鎰板锤濡や胶鍘介梺閫涘嵆濞佳勬櫠閹殿喚纾奸柣妯哄暱閸斻倖銇勯鈩冪《闁圭懓瀚粭鐔碱敍濮橆剙顥愰梻鍌欑窔濞佳兾涘▎鎴炴殰闁绘劕鐏氶～鏇㈡煙閻戞﹩娈㈤柡浣革躬閺屻倕霉鐎ｎ偅鐝旈梺鎰佷簽閺佸骞冨Δ鍐╁枂闁告洦鍓涢ˇ顓㈡⒑閸濄儱校闁绘濞€婵″瓨鎷呯化鏇燁潔濠碘槅鍨堕弨閬嶏綖瀹ュ應鏀芥い鏃€鏋绘笟娑㈡煕閹惧鎳呯紒顔碱煼閹瑥霉鐎ｎ偅鏉搁梻浣虹帛閿氶柣蹇斿哺瀵娊鍩℃担鍙夋杸闂佺粯鍔忛弲娑欑閻愵剛绡€闁汇垽娼ф禒鎺楁煕閺嶎偄鈻堢€规洘顨婇幃娆擃敆閸屾顫嶉梺鍓х帛閻楃娀寮诲☉姗嗙叆闁逞屽墴閹偤鏁冮崒姘辨煣濠电姴锕ら悧濠囧煕閹达附鐓曢柟閭﹀墮缁狙囨煙椤栨氨澧﹂柡灞剧洴閹垻绮欏▎鐐棆濠电姰鍨奸～澶娒洪悢鐓庣畺闁告挆鍌涘媰缂備礁顑呴妵妯荤珶閺囩倣鏃堟偐闂堟稐绮堕梺璇茬箲缁诲牆鐣峰┑鍡╂僵妞ゆ挻绻勭粻姘渻閵堝棗濮х紒鎻掓健楠炲﹪宕橀鐣屽幗闂佹寧绻傞幊搴∥ｆ繝姘厽闁挎繂妫涚粻鐐碘偓瑙勬礈閸犳牠銆佸Δ鍛＜婵﹩鍓涢鍥⒒閸屾瑦绁版い鏇嗗懏宕查柟鐑橆殔绾剧粯绻涢幋鐐垫噭婵炲懐濮撮湁闁绘ê妯婇崕蹇涙煟閹绢垪鍋撻幇浣哄數闂佸吋鎮傚褎鎱ㄩ崼銉︾厽闁规儳鍟块弳锝夋煙椤旇偐绉虹€规洖鐖兼俊姝岊槻鐎殿喓鍔岄—鍐Χ閸涱収鍔夊銈冨妼閿曨亪鐛Δ鍛唶闁哄洨鍠愬▍婊勭節閵忥絾纭炬い鎴濇喘瀹曪綁宕熼娑辨濡炪倖鍔戦崹鐑樺緞閸曨厾纾奸悗锝庝簽婢э妇鈧娲橀崹鍧楃嵁濮椻偓瀵剟濡烽敂缁樼秾濠电姷顣藉Σ鍛村垂椤栨粍濯伴柨鏇炲€搁崥瑙勭箾閸℃ɑ灏伴柍閿嬪浮閺屾稓浠﹂幑鎰棟闂侀€炲苯澧柟顔煎€搁悾鐑藉箛椤掑倹娈濋梺鐟板暱閿曘倗绱炴繝鍌滄殾闁诡垶鍋婂銊モ攽閻愯尙鎽犻柣妤冨█瀵寮撮悢铏诡啎闂佺粯鍔﹂崜姘舵偟閺囥垺鈷戦柟鎯板Г閺佽鲸鎱ㄦ繝鍌ょ吋闁糕斁鍋撳銈嗗坊閸嬫挾绱掗悩鑼х€规洘娲熼弻鍡楃暤閵夈儲鍠樻い銏＄☉椤劑宕橀悙顒夋％闂備浇宕甸崰鎰版偡閵壯€鍋撳鐓庢灓鐎殿啫鍥х劦妞ゆ帒瀚埛鎺楁煕鐏炵偓鐨戝褎绋撶槐鎺斺偓锝庡亜濞搭噣鏌熼姘冲閾绘牠鏌涢幇鍏哥敖闁?
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
        if (st == 0) {                     // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婄闁挎繂妫Λ鍕⒑閸濆嫷鍎庣紒鑸靛哺瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁诡垎鍐ｆ寖缂備緡鍣崹鎶藉箲閵忕姭妲堥柕蹇曞Х椤撳搫鈹戦悙鍙夘棞缂佺粯甯楃粋鎺撱偅閸愨斁鎷虹紓浣割儏閻忔繈顢楅姀銈嗙厱闁靛鍎查崑銉р偓娈垮枛椤兘寮澶婄妞ゅ繐鎳庢刊浼存⒑閼姐倕孝婵炶绠掗妵鎰板礃閳哄倸寮块柟鍏肩暘閸斿秹鎮￠弴銏＄厪濠㈣埖鐩顕€鎮樿箛銉х暤闁哄备鈧磭鏆嗛柍褜鍓熷畷浼村冀瑜忛弳锔戒繆閵堝懏鍣洪柛銈呭暣閺屽秹宕崟顐熷亾瑜版帩鏁婇柛鎰靛枟閳锋帒霉閿濆牊顏犻柕鍡楋躬閺岋繝宕ㄩ淇扁偓鎺楁煃瑜滈崜姘跺礄瑜版帒绀堟繝闈涱儐閸婂爼鎮楅敐搴℃灍闁抽攱鍨块弻娑樷槈濮楀牆濮涢梺瀹犳椤︾敻寮婚敐鍡楃疇闂佸憡姊归崹鍨暦閸濆嫧妲堥柕蹇曞Х椤撴椽姊洪幐搴㈩梿婵☆偄瀚粋鎺楊敍閻愮补鎷洪梺纭呭亹閸嬫盯宕濆鈧弻娑㈠棘閸ф寮伴悗瑙勬穿缂嶄線宕洪埀顒併亜閹哄棗浜惧銈庝簻閸熷瓨淇婇崼鏇炲窛妞ゆ挆鍐ㄎㄥ┑锛勫亼閸婃垿宕濆畝鍕亱闊洦鎷归敐澶婄疀闁哄娉曢濠傗攽鎺抽崐鎾剁矆娴ｅ湱顩茬憸鐗堝笚閳锋垿鏌ゅù瀣澒闁瑰嚖绻濋弻娑欐償閿濆棛鍔瑰銈忛檮閹告娊寮诲鍫闂佸憡鎸荤换鍫ユ晲閻愭潙绶炲┑鐐村笒閹碱偊鈥﹂妸鈺侀唶婵犻潧鐗忛弸鍐⒒娴ｇ瓔娼愮€规洘锚閳绘柨鈽夊鍡楁闂佸憡鐟ラˇ顔炬崲閸℃稒鐓熼柟鏉垮悁缁ㄥ鏌嶈閸撴艾煤濠婂牆绠查柕蹇嬪€曠粻娑欍亜閺冨倹鍤€濞存粎鍋撻幈銊ノ熼悡搴′粯濡ょ姷鍋為悧鐘茬暦閳╁啯缍囬柕濠忕导缁ㄥ姊洪悷鐗堟儓婵☆偅顨嗙粋宥嗐偅閸愨斁鎷洪柣搴℃贡婵厼顭囬幇鐗堢厱闁靛鍎查崑銉︻殽閻愭彃鏆欓柍璇查叄楠炴ê鐣烽崶顒傚礈闂傚倷鑳堕幊鎾活敋椤撶喐鍙忛柛鎰▕濞存牠鏌熼幆褜鍤熺紒鈾€鍋撴繝鐢靛仜閻楀棝鎮樺┑瀣嚑闁绘柨鍚嬮崵鏇犳喐閺冨牆钃熸繛鎴欏灩缁秹鏌嶈閸撶喎鐣疯ぐ鎺撳仺闁告稑锕ゆ禒濂告⒑缂佹ê鐏辨俊顐㈠閹﹢骞庨懞銉у幍婵☆偊顣﹂懗鍫曞礉瀹ュ鐓曠€广儱顦板☉褎銇勯鍕殻濠碘€崇埣瀹曞崬螣濞差亞鈧櫣绱撻崒娆戣窗闁哥姵鐗犻幃褑绠涘☉娆忔疅闁哄鐗勯崝搴ｅ姬閳ь剙鈹戦鏂や緵闁告ɑ鎮傞獮蹇撁洪鍛嫼闂佸憡绋戦敃锔剧不閹剧粯鍊垫慨妯煎帶閺嬶箓鏌嶉鍕Ш婵﹦绮幏鍛瑹椤栨粌濮奸梻鍌欑鎼存粎寰婇崜褏鐭夌€广儱顦柋鍥煛閸モ晛浠︽俊宸墴濮婃椽鏌呴悙鑼跺濠⒀屽灦閺屾洟宕惰椤忣厽顨ラ悙鏉戠瑨妞ゆ挸銈稿畷鍫曨敂閸℃瑥鐐婃繝鐢靛Х閺佹悂宕戝☉銏″亱濠电姴娲犻埀顒婄畵瀹曞爼鍩￠崒銈嗙稐闂備礁婀遍崕銈夈€冭箛娑樻瀬濠电姴娲﹂悡鐘崇箾閺夋埈鍎愭繛鍛噺閵囧嫰濡堕崒姘闂傚倷娴囬褎顨ラ幖浣瑰€舵慨姗嗗墻閻斿棙淇婇姘辨癁闁稿鎸搁～婵嬫偂鎼达紕顔掗梻浣烘嚀缁犲秹宕硅ぐ鎺戠叀濠㈣埖鍔曢～鍛存煟濡椿鍟忛柛鐐存そ濮婄粯鎷呴悜妯烘畬濡炪倖娲﹂崢浠嬪箞閵娾晜鐓ラ悗锝呯仛椤斿嫰姊婚崒姘偓鎼佸磹瀹勬噴褰掑炊椤掑鏅梺鍝勭▉閸樿偐绮ｅΔ鍛厵闁绘垶锕╁▓鏃堟煟閵堝鐣洪柡宀嬬到铻ｉ柤濮愬€栭懣鍥⒑闁偛鑻崢鍝ョ磼閼镐絻澹樻い鏇秮楠炴﹢顢氶崱娆戔棨婵＄偑鍊栭弻銊︽櫠娴犲鐓熼柕鍫濇缁诲棝鏌ｉ幇鍏哥盎闁逞屽墯閻楃娀鐛幇顓滃亝闁告劑鍔庨敍鐔兼⒑瑜版帒浜伴柛搴ㄦ涧閳藉顫滈崱娆忓汲闂備礁鎲￠崝褏寰婃ィ鍐ㄦ闁逞屽墴濮婄粯鎷呴搹鐟扮闂佹寧宀搁弻锝堢疀閺傚灝鎽甸悗娈垮枟婵炲﹪宕洪敓鐘插窛妞ゆ梹鍎抽獮鍫ユ⒑鐠囨彃鍤辩紓宥呮瀹曟粌鈻庨幇顕呮祫闁哄鐗冮弬渚€宕戦幘鑸靛枂闁告洦鍓涢ˇ銉х磽閸屾氨孝闁挎洏鍊濋幃楣冩煥鐎ｎ剟妾紓浣割儏閻忔繂鐣甸崱娑欌拺缂備焦锚婵矂鏌涢埡鍌滃⒌妤犵偛顦扮€佃偐鈧稒菤閹风粯绻涢幘鏉戠劰闁稿鎸荤换娑欐媴閸愬弶顥滄い銈呭€垮缁樻媴閸涘﹥鍎撻梺鐟板暱缁绘帡宕氶幒妤€绠荤€规洖娲﹀▓楣冩⒑閻熼偊鍤熷┑顕呭弮瀹曟垿骞樼紒妯绘珳闁硅偐琛ラ崜婵嬫倶瀹ュ鈷戠紓浣股戠亸浼存煟閻斿弶娅婄€殿喖顭烽幃銏ゆ倻濡儤鐎梻浣告啞濞诧箓宕戦崟顕呮澓闂傚倸鍊烽悞锕傚箖閸洖纾归柡宥庡幗閸嬶紕鎲歌箛鎾愶綁骞囬弶璺唺濠德板€愰崑鎾绘煟閹烘垹浠涢柕鍥у楠炴帡骞嬪┑鎰偅缂備胶鍋撻崕鎶藉触鐎ｎ剚宕叉繛鎴欏灩缁狅綁鏌ｉ幋婵囶棡闁伙綀鍋愮槐鎾存媴閸濆嫅锝夋煠瑜版帞鐣洪柍銉︽瀹曟﹢顢欓崲澹洦鐓曢柍鈺佸暟閹冲啯銇勯搴℃噽绾捐棄霉閿濆洨鎽傞柛銈嗙懇閺屾稒绻濋崒婊€铏庨梺浼欑悼閸忔ɑ淇婇悜钘壩╅柨婵嗗閻繘姊绘担鍛婅础闁稿簺鍊濋獮鎰偅閸愩劌鍋嶉梺瑙勫劤婢у海澹曟禒瀣厱閻忕偛澧介幊鍛存煕閺傝法效闁归攱鍨块弻鍡楊吋閸℃瑥甯楅梻鍌欑閻忔繈顢栭崨顒煎綊顢氶埀顒勫蓟閿涘嫪娌柣锝呯潡閵夛负浜滅憸宀€娆㈠璺鸿摕婵炴垯鍨圭粻濠氭倵闂堟稒鍟為柛锝庡枤缁辨挻绗熼崶褎鐏曞銈嗘肠閸ヨ埖鏅ｉ梺绋跨箳閸樠呮閻愭祴鏀介柣妯诲絻椤忕數绱掓潏鈺傛毈婵﹤顭峰畷鎺戔枎閹达絿鐛ラ梻浣告啞閹稿鎮疯閸掓帡寮崒妤€浜鹃梻鍫熺⊕閹茬鈽夐幘宕囆ч柡宀嬬到铻ｇ痪鎷岄哺濮ｅ矂姊虹粙娆惧剱闁圭懓娲幃浼搭敋閳ь剙鐣疯ぐ鎺濇晩闁绘挸瀵掑娑樷攽閻樺灚鏆╅柛瀣洴閹洦瀵奸弶鎴狅紮濠电娀娼уΛ娑㈠汲?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇牠姊绘笟鈧埀顒傚仜閼活垱鏅堕弶娆剧唵閻熸瑥瀚粈鍐磼缂佹绠撴い顐ｇ箞椤㈡﹢鎮╅幓鎺旂杽闂傚倸鍊烽悞锕傚几婵傜鐤炬繛鎴欏灩閻ゎ喖霉閸忓吋缍戦柣鎾偓鎰佺唵闁兼悂娼ф慨鍫ユ煟閹惧瓨绀嬮柟顔斤耿閹瑦锛愬┑鍡橆唲濠电姵顔栭崰鏍磹閸ф钃熼柨娑樺濞岊亪鎮归崶銊с偞闁稿鎹囧鎾閿涘嫮鏆㈤梻鍌欐祰椤曆囧礄閻ｅ瞼绀婇柛鈩冪☉绾惧鏌涘☉妯兼憼闁稿鍠栭弻锟犲炊閳轰礁澹夊┑?
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
constexpr wchar_t kAppVersion[] = L"3.20.1";

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
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ岸姊虹拠鍙夊攭妞ゎ偄顦叅闊洦绋戠壕鍧楁煙閹増顥夐柣鎺戠仛閵囧嫰骞掗幋顖氬缂備礁顦遍弫濠氬蓟濞戞粎鐤€闁哄啯鎹侀埀顒冩硶閳ь剝顫夊ú姗€宕归崸妤冨祦婵せ鍋撶€规洘绮嶉幏鍛存惞鐟欏嫭顔旀繝鐢靛Х閺佹悂宕戦悙鍝勫瀭闁诡垳澧楅幊宀勬⒒娓氣偓濞艰崵寰婇悾宀€绀婂┑鐘叉搐閽冪喐绻涢幋娆忕仼閹喖姊洪幐搴㈢叆濠⒀傜矙閹潡宕熼娑掓嫼闁荤姴娲犻埀顒冩珪閻忓秹姊洪幐搴㈢８闁搞劋绮欓悰顕€宕橀妸銏＄€婚梺瑙勫劤閸熷潡鎮楅鐑嗘富闁靛牆妫楁慨褏绱掗懠璺哄姷闁靛洦鍔橀ˇ瑙勬叏婵犲嫮甯涢柟宄版嚇瀹曘劍绻濋崘銊ュ濠电姷顣藉Σ鍛村磻閸曨垼鏁嬫い鎾跺Т閸ㄦ繂鈹戦悩瀹犲缂佺姴顭烽幃褰掑炊椤忓嫯鏁块梺纭呭Г濞茬喎顫忓ú顏勭閹艰揪绲块悾闈涱渻閵堝繐鐦滈柛銊﹀▕閹噣骞嗚閸氬顭跨捄渚剳闁告﹢浜堕弻锝堢疀閺囩偘娌悗瑙勬礃钃辩紒鏃傚枎铻ｅ┑鍌濇缂嶅﹪骞冮埡鍛疀濞达絽鎼慨铏圭磽閸屾瑦绁板瀛樻倐楠炴劖绻濆顒傤唵闂佸憡绋掑娆愬閻樼粯鐓曢柡鍥╁枎閻撯偓缂備焦褰冩晶浠嬪箲閵忕姭鏀介悗锝庝憾濞煎﹪姊虹€圭姵顥夐柟铏崌閸┾偓妞ゆ巻鍋撴繛纭风節瀵顓兼径濞劍銇勯弽銊х煂闁哄棎鍊濋幃妤冩喆閸曨剛顦銈庡幖閻楁劙鎮橀崘鈺冪閺夊牆澧介崚浼存煙鐠囇呯瘈闁诡喖娼″畷鎺楁倷缁瀚介梻浣侯焾閺堫剟鎳濇ィ鍐ㄧ劦妞ゆ帊鐒﹂崐鎰版煙椤斻劌瀚弧鈧梺鍛婃处閸橀箖鎮￠幘缁樷拺缁绢厼鎳忚ぐ褏绱撳鍛棦鐎殿喖鐖奸幃婊兾熼懖鈺冩闂備焦鐪归崹钘夘焽瑜嶉弳鈺呮⒒娴ｈ姤銆冮柣鎺炵畵楠炲啴宕掑杈ㄦ闂佺懓鐡ㄧ缓鎯ｉ崼銉︾厪闊洤艌閸嬫捇宕楅崨顕呮綗闂傚倸鍊风欢姘焽瑜旈垾锕傚醇閵夈儳锛熼悗骞垮劚閹灝煤椤忓秵鏅滈梺鍛婁緱閸樿櫣鑺辨繝姘拻濞达綀娅ｇ敮娑㈡煙閸濄儺鐒鹃棁澶愭煟閹达絽袚闁稿鏅犻弻娑樷攽閸℃浠遍梺琛″亾濞寸姴顑呯粻鎶芥煙閹増顥夌痪鎯х秺閺岀喖鎮ч崼鐔哄嚒缂佺偓鍎抽…鐑藉蓟閻旂厧绀堢憸蹇曟暜濞戙垺鐓熼柟鎯у暱閺嗭綁鏌″畝瀣暠閾伙絽銆掑鐓庣仭闁崇粯鎸搁埞鎴﹀灳閸愯尙楠囬梺璇″枛閸婂灝顕ｇ拠宸悑闁搞儯鍔岄悵浼存⒑閸濆嫯顫﹂柛搴㈡そ閸┾偓妞ゆ帊绶″▓婊堟煙閽樺鍩ｉ柡浣瑰姈瀵板嫮鈧綆鍓欓獮妯衡攽閻樻剚鍟忛柛鐘愁殜瀹曟繈寮撮姀鐘靛姦濡炪倖甯婇懗鑸垫櫠椤忓牊鐓冪紓浣股戠亸浼存煏閸剛绉€规洘锕㈤崺鐐烘倷椤掆偓椤忔椽姊婚崒娆掑厡妞ゎ厼鐗撳鐢割敆閸曨剙浠梺鎸庢礀娴兼牠宕堕…鎴炐ч柟鑹版彧缁插潡鏁嶅鍕箚闁圭粯甯楀﹢浼存煟濡や焦绀€闁宠绉撮濂稿幢濡搫浼庡┑鐘垫暩婵娊鎳楅崼鏇熷剹婵炲棙鍨熼崑鎾舵喆閸曨剛顦梺鍝ュУ閻楃娀濡存担鍓叉建闁逞屽墴楠炲啴濮€閻樺灚娈濋梺瑙勵問閸犳牠骞栭幖浣光拻闁稿本鑹鹃埀顒勵棑缁牊鎷呴崷顓犲骄婵犵數濮村ú銈囧鐠囧眰鈧帒顫濋敐鍛闁诲氦顫夊ú妯好洪悢椋庢殾婵せ鍋撻柟宕囧█椤㈡鎷呯拠鈥虫櫕濠电姷鏁搁崑鐐哄箰婵犳艾鏄ラ柛鎰靛枛绾惧鏌熼崜褏甯涢柣鎾寸〒閳ь剙鍘滈崑鎾绘煃瑜滈崜鐔风暦娴兼潙鍐€妞ゆ挾鍋熼悾鍝勨攽閻樿宸ラ柛鐘宠壘椤洭濡搁埡鍌滃幈闂佹枼鏅涢崰姘舵倿娴犲鐓涢柛鈩冨姇閸斻倗绱掓潏銊﹀磳鐎规洘甯掗埢搴ㄥ箳閹存繂鑵愭繝鐢靛У椤旀牠宕伴弽顓熷€舵慨妯块哺瀹曞弶绻涢幋鐐垫噮缂佲檧鍋撻梻浣规偠閸庢粓宕卞Ο璇叉毇闂傚倸鍊烽懗鍓佸垝椤栨娑欑瑹閳ь剚淇婇幘顔肩劦妞ゆ帒瀚悡鏇㈡倵閿濆骸浜濈€规洖鐭傞弻鐔碱敊閹稿寒妫勯梺宕囩帛閹搁箖骞戦崟顖毼╅柕澶堝剾椤忓牊鈷掑ù锝呮啞閹叉悂鏌涢悩铏磳闁炽儻绠撳畷濂稿Ψ閳垛晛浜鹃柡鍐ㄧ墕缁€鍐┿亜閺傛寧顫嶇憸鏃堟偂椤愶箑鐐婇柕濠忓椤︻參鏌﹂崘顔绘喚婵﹦绮幏鍛存惞閻熸壆顐奸梻浣烘嚀閹诧繝骞愰幎钘夋槬闁逞屽墯閵囧嫰骞掑鍥舵М婵犫拃灞界仭缂佺粯鐩畷妤呮嚃閳轰緡娼风紓鍌欒兌缁垳绮欓幋锔绘晩闊洦渚楅弫濠囨煠婵劕鈧鈻旂捄銊х＝闁稿本鐟ㄩ崗灞解攽椤旇棄鈻曠€规洩绻濋獮搴ㄦ偩鐏炵晫銈﹂梻浣告惈缁夋煡宕濆鍥╃闁搞儺鍓氶悡鏇㈡煙閻愵剦娈旈柛鐔跺嵆閹€澄旈埀顒勫煘閹达附鍋愰柟棰佺閺呴亶姊洪崫銉バｉ柣妤佹礈閸欏懘姊洪棃娴ゆ盯宕熼崹顐ょ◥闂傚倷绀佸﹢閬嶅磿閵堝鈧啴宕卞☉妯硷紮闂備礁鐏濋鍐╃濠婂牊鐓欓柛婵嗗鍟搁梺绋跨箰閻倿寮诲☉銏犵妞ゆ牗绮庨敍鐔虹磽娴ｄ粙鍝洪柟绋款煼楠炲繘宕ㄩ弶鎴狀槶闂佸憡鐟ラ敍宥夊Ω閳哄倵鎷虹紒缁㈠幖閹冲繘鎮甸鍫熺厵闁荤喓澧楅崰姗€鏌曢崱鏇狀槮妞ゎ偅绻堥幊婊堝垂椤愶絿褰告繝鐢靛О閸ㄧ厧鈻斿☉銏℃櫇闁挎洖鍋嗛弫濠囨煕濞戞鎽犻柍閿嬪灴閺屾稖绠涘☉娆欑吹闂佺厧澹婃禍顏堝蓟濞戞瑦鍎熼柕鍫濐槸閹界數绱撴担绋库偓鍝ョ矓閹绢喒鈧箓濡搁埡渚€鍞堕梺缁樻煥瀵墎鈧俺鍋愮槐鎾诲磼濮橆兘鍋撻悜鑺ュ€块柨鏇氱劍閹冲矂姊虹拠鑼婵炲瓨宀稿畷銏ゆ偩鐏炴儳搴婂┑鐐村灦閸╁啴宕戦幘缁樺仺婵炲牐娉曢崝鎼佹⒑閸涘⊕顏堟偤閵娾晛桅闁告洦鍨伴～鍛存煥濞戞ê顏柛锝庡弮閹宕归锝囧嚒闁诲孩鍑归崢楣冨箲閵忕姭鏀介柛銉ｅ妼閸斿懘姊洪棃娑氱濠殿喖鍢插嵄闁挎繂顦伴埛鎺楁煕鐏炲墽鎳呯紒鎰⒐缁绘稒鎷呴崘鍙夊闁稿顑夐弻娑㈠焺閸愶缚绨靛┑鐐村灦鑿ら柡瀣叄閺岀喖骞嗚瀛濆銈忕畵濞佳囨偩閻戣棄纭€闁绘垵妫欑€靛瞼绱撻崒娆戝妽閽冮亶鏌℃径濠冨暈濞ｅ洤锕幃娆擃敂閸曘劌浜鹃柡宥庡幖閽冪喖鏌曟繛鐐珔缂佺姾顫夐妵鍕箛閸撲胶鏆犻梺缁樺笒閻忔岸濡甸崟顖氱鐎广儱鐗嗛崢锛勭磽娴ｅ搫啸闁稿鍋ら妴鍐Ψ閳哄倸鈧兘鏌熺紒妯虹瑲婵炲牜鍙冮弻褏绱掑Ο鐓庘拰闂佸搫鏈粙鎴︹€﹂妸鈺佺劦妞ゆ帒瀚崐鍧楁煥閺囩偛鈧悂鎮挎ィ鍐╃厓閺夌偞濯介澶愭煛閳ь剚绂掔€ｎ偆鍘介梺褰掑亰閸撴盯骞楅悩鐢电＝鐎广儱妫楅悘銉╂懚閺嶎厽鐓熸俊顖氭惈缁狙囨煙閹绘帗鎲哥紒杈ㄥ笚濞煎繘鍩℃担閿嬪媰缂傚倷鑳剁划顖滄崲閸繄鏆﹂柟顖炲亰濡查箖姊烘导娆戠ɑ婵＄偠妫勯～蹇撁洪鍕槰闂佸憡鐟ラˇ浼村磿閹剧粯鈷戦柣鐔告緲閺嗚京绱掔紒妯忣亪鎮鹃悜钘夌疀妞ゆ柨澧介敍婊冣攽閳藉棗鐏犳繝鈧潏鈺冧笉婵﹩鍓﹀〒濠氭煏閸繃顥犲褜鍓涚槐鎺楊敊閹€鏋呴悗瑙勬礃濞茬喖骞冮埡鍐＜婵☆垳鍘х敮妤呮煟閻斿摜鐭嬬紒顔芥尭閻ｅ嘲顭ㄩ崘鐐兊闁荤娀缂氬▍锝壦囬灏栨斀闁绘劕寮堕ˉ鐐烘煙缁嬫寧鎲哥紒顔碱煼瀵粙顢曢悢铚傚闂佽崵鍠愬姗€顢旈銏＄厱閻庯綆浜烽煬顒勬煙椤曞棛绡€濠碉紕鍏橀崺锟犲磼濠婂啫绠炲┑鐘垫暩閸嬬偤宕归棃娑氭殾妞ゆ巻鍋撴い鏇稻閵堬綁宕橀埡鍐ㄥ箰闂備胶纭跺褔寮插☉姗嗗殨妞ゆ棁濮ら崣蹇撯攽閻樺弶鍣烘い蹇曞Х缁辨帗娼忛妸銉х懆闁句紮绲跨槐鎾存媴婵埈浜畷婊堟偨閻㈢數锛濋梺绋挎湰閻熴劑宕楀畝鈧槐鎺楊敋閸涱厾浠梺鐟扮畭閸ㄨ棄鐣锋總绋课ㄩ柨鏇楀亾濞寸姵甯掗—鍐Χ閸℃ǚ鎷婚梺鍝勬媼閸嬪棗顕ｈ閸┾偓妞ゆ帒瀚埛鎴犵磼鐎ｎ偒鍎ラ柛搴㈡尭閳规垿顢欓崫鍕ㄥ亾閺囷紕浜遍梻浣告啞閸旀垿宕濇惔銊ユ辈闁挎洖鍊归悡蹇撯攽閻愰潧浜炬繛鍛崌閺屸剝鎷呴棃鈺勫惈闂佸搫鏈粙鏍不濞戞﹩娼╂い鎴炲閻繘姊绘担鍛婃儓妞ゆ垵鍊垮畷鎴﹀箻缂佹ǚ鎷婚梺绋挎湰閼归箖鍩€椤掍焦鍊愮€规洘鍔栭ˇ鐗堟償閵忊晛浠烘繝娈垮枟閿曗晠宕滈敃鍌涘€块柣锝呮湰閸犳劙鏌￠崘銊モ偓璇参ｉ悽鍛娾拻濞撴埃鍋撻柍褜鍓涢崑娑㈡嚐椤栨稒娅犻柟娈垮枤绾惧ジ寮堕崼娑樺婵炴惌鍣ｉ弻鐔碱敊鐟欏嫭鐝氶梺鍦帶缂嶅﹪銆侀弴銏狀潊闁靛繆鏅滃▓瑙勭節绾板纾块柛瀣灴瀹曟劙濡舵径濠勵槶濠电偛妫欓幐濠氬疾濠靛洢浜滈煫鍥ㄦ尵婢ф洜绱掗悩宕囧缂佺粯绻冪换婵嬪磼濠婂喚鏉搁梻浣规偠閸婃洜绮旇ぐ鎺戣摕闁哄洨鍠庣欢鐐烘煕椤愶絿绠樺ù鐘荤畺閺岋絾鎯旈姀锝咁棟闂侀潧鐗忛…鍫ワ綖韫囨洜纾兼俊顖濐嚙椤庢捇姊洪幆褏绠抽柟铏尵缁參鏁撻悩鏂ユ嫼闂侀潻瀵岄崣搴ㄦ倿妤ｅ啯鐓欓柤鎭掑劤閻帡鏌涢埞鎯т壕婵＄偑鍊栧濠氬磻閹剧粯鐓熼煫鍥ㄦ⒐閻ㄦ垿鏌℃笟鍥ф珝妤犵偞甯掕灃闁逞屽墴閹€斥攽鐎ｎ偆鍘甸梺鍛婃寙閸涱厾顐奸梻浣侯焾椤戝棝宕濆▎鎾崇畺闂佸灝顑嗘刊鎾煟閵堝骸鐏犻柛姗堢節濮婅櫣娑甸崨顔惧涧闂佸湱鎳撳ú顓㈠垂妤ｅ啯鏅濋柛灞绢嚤閳哄懏鐓冮弶鐐村閸忓本銇勯敂鑺ョ凡妞ゎ亜鍟存俊鍫曞幢濡儤娈┑鐘愁問閸犳岸宕楀Ο渚殨闁规儼妫勫洿婵犮垼娉涢鍌炲箯濞差亝鈷戠痪顓炴媼濞兼劙鏌涢弮鈧ú鐔风暦閵夈劊浜归柟鐑樻尵閸橀亶鏌ｆ惔顖滅У闁告挻鐟︾粋鎺楀础閻愨晜鏂€闂佹寧绋戠€氼參寮抽鍌楀亾鐟欏嫭绀堥柛鐘崇墵閵嗕礁鈽夊鍡樺兊闁哄鐗冮弲婊冾熆閹达附鈷掗柛灞剧懅椤︼箑顭块悷甯含鐎规洘鍨垮畷銊╊敆閻撳孩鎯堟い顐ｇ矒閸┾偓妞ゆ帒瀚ㄩ埀顑跨窔楠炴﹢骞囨担鍛婂€┑鐘灱濞夋盯顢栭崒鐐茬闁惧繐婀辩壕钘壝归敐鍛棌闁稿孩鍔栭妵鍕箣閻愬灚鍣伴悗瑙勬礃濞叉繄绮诲☉銏犳閻犱礁纾惄搴ㄦ⒒娴ｇ儤鍤€闁宦板妿閹广垹顓奸崨鍌滃枛閸┾偓妞ゆ帒瀚崐鍨箾閸繄浠㈤柡瀣堕檮閵囧嫰寮撮崱妤佹悙闁绘挴鈧剚鐔嗛柤鎼佹涧婵洦銇勯銏″殗闁哄苯绉瑰畷顐﹀礋椤愮喎浜鹃柣鐔稿閺嬪秹鏌涢妷顔煎闁抽攱甯掗妴鎺戭潩閿濆懍澹曟繝鐢靛仒閸栫娀宕堕妸銉ょ綍闂備胶纭堕崜婵堢矙閹寸姷涓嶉柣妯垮吹閸欐捇鏌涢妷锝呭闁愁垱娲熼弻娑欑節閸ャ劍閿柧缁樼墵閺屻劌鈹戦崱妯烘闂佸搫妫涢崑銈夊蓟瀹ュ鐓ラ悗锝庝簽娴煎矂姊洪崫鍕拱闁烩晩鍨跺顐﹀磼濠婂嫬鐝伴梺鍦帛鐢偤顢欓幇鐗堚拻濞达絽鎲￠崯鐐烘嫅鏉堛劎绠鹃柛蹇氬亹閹冲洦绻濋埀顒佹媴缁洘鏂€闂佺粯顭堥婊冾啅閵夆晜鐓曢柣鏃囨硾瀹撳棛鈧娲樺浠嬪极閹版澘鐐婇柨婵嗘噸婢规洖鈹戦悙鑼闁诲繑绻堝绋库槈濞嗗秳绨诲銈嗘尪閸ㄦ椽寮崡鐐冨酣宕惰闊剚顨ラ悙瀵稿闁瑰嘲鎳橀幊鏍р攽閸曨偄顫囬梺鍝勬湰濞叉繄绮诲☉娆戠瘈闁告劗鍋撶紞澶嬬節閻㈤潧浠滈柛姘儏椤繘宕烽鐔峰簥濠电娀娼ч悧鍡涘几閸喍绻嗘い鏍ㄧ⊕閵囩喐绻涢崗鐓庣伌婵﹥妞介獮鏍倷閹绘帩鐎村┑鐘垫暩閸嬫盯鏁冮妶澶嬪仼闁绘垼妫勯～鍛存煥濠靛棗顒㈤柍璇茬焸閺岋綁鎮╂潏顐妳闂佹悶鍔岄悥鐓庮嚕鐠囧樊鍚嬮柛顐亝椤庡洭姊绘担鍛婂暈闁规瓕顕ч～婵嬪Ω閳轰胶顔夐梺闈涚箳婵參寮ㄦ禒瀣€甸柨婵嗛娴滄粌霉閻樿櫕銇濇慨濠傤煼瀹曟帒顫濋崡鐑嗘澑闂備礁鎲″濠氬磻閹炬枼鏀介柣鎰綑閻忕喖鏌涢妸锔姐仢闁诡噯绻濆鎾閳ユ枼鍋撻悽鍛婄叆婵犻潧妫濋妤€霉濠婂嫮绠為柟顔筋焾缁犳稒鎯旈垾宕囥偖闂備礁鎼張顒€煤閻旈鏆﹂柟顖炲亰濡茶顪冮妶鍛闁告ü绮欓妴鍐Ψ閳哄倸鈧兘寮堕崼鐔风闁逞屽墲濞夋洟鍩€椤掑喚娼愭繛娴嬫櫇閹广垹鈹戠€ｎ亞鐣哄┑鐐叉閹稿摜绮堥崒鐐寸厱婵炴垶锕銉ッ瑰鍕畼缂佽鲸鎹囧畷鎺戭潩椤戣棄浜鹃柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屻倗绮欑捄銊ょ驳闂佸憡鐟ョ换姗€寮诲☉鈶┾偓锕傚箣濠靛懐鏁栫紓鍌欒兌婵敻宕曢幎钘夌厴闁硅揪闄勯崑鎰版煕濞嗗浚妲归柟顔界懇濮婃椽鎸婃径濠冩闂佸摜濮甸悧鐘差嚕婵犳碍鏅插璺猴攻椤ユ繈姊洪崜鑼帥闁哥姵顨婂畷鎰邦敍閻愮补鎷虹紓浣割儏鐏忓懘宕濋悢铏圭＜妞ゆ洖鎳庨悘锛勭磼瀹€鍐摵缂佺粯绻堝畷鍫曟嚋閸偅鐝┑鐘垫暩婵炩偓婵炰匠鍏犲綊宕掑☉姘兼祫闂佸啿鎼崐鑽ょ不妤ｅ啯鍊甸柣銏☆問閻掗箖妫勭€ｎ喗鈷戠紒瀣儥閸庢劙鏌涢弮鈧〃鍛祫闂佸湱澧楀妯肩不閾忣偂绻嗛柕鍫濆椤︼箓鏌嶈閸撶喖寮ㄩ柆宥呂﹂柛鏇ㄥ灠缁犲磭鈧箍鍎遍悧鍡涘储閿熺姵鈷戦柛娑橈工閻撯偓缂備浇椴稿ú婊兾ｉ幇鏉跨閻庢稒锚椤庢捇姊洪崨濠冨碍鐎殿喖澧庨悷褏绱撻崒姘偓鎼佸磹閸濄儳鐭撻柟缁㈠枟閺呮繈鏌曢崼婵愭▓闁轰礁顑夐弻銊モ攽閸℃﹩妫ょ紓浣叉閸嬫挻绻濆▓鍨灍闁挎洍鏅犲畷鏇㈡偨缁嬭法鍘搁梺鍛婁緱閸犳岸宕㈤鍛瘈闁靛骏绲剧涵楣冩煟濡も偓濡繈骞冨鈧、鏃堝川椤愶紕鐩庢俊鐐€栭崝鎴﹀垂鐠囪尙鎽ラ梻鍌欑閻ゅ洭锝炴径鎰瀭闁秆勩仠閳ь兛绀侀埢搴ㄥ箣閻樼儵鏀洪梺璇插嚱缂嶅棝宕戦崱娑樻辈闁靛濡囩弧鈧紒鍓у钃辨い顐躬閺屾稓鈧綆浜濋崵鍥煙椤旀儳鍘撮柛鈺嬬節瀹曘劑顢曢悢铚傚婵炴挻鍩冮崑鎾搭殽閻愭惌鐒介柟椋庡█楠炴捇骞掗弴妤€浜鹃柛顭戝枓閺€浠嬫煟閹邦剙绾фい銉у仱閺屾盯濡搁妷褏楔闂佽桨绀佺粔鎾煘閹达箑鐐婇柍鍝勫枤閸熷酣姊洪崫鍕垫Ц闁绘妫欓弲鍫曟偩鐏炵浜惧ù锝呭暙娴滃湱绱掓潏銊ユ诞妞ゃ垺鐟╅幃鍓т沪閹勭彴闂佽姘﹂～澶娒洪敃鍌氱；濠电姴娲ょ粻鍦喐閻楀牆绗掗柟纭呭煐閵囧嫰骞樼捄渚偓鎾剁磼鏉堛劎鍙€婵﹥妞介幊锟犲Χ閸涘懌鍨虹换娑氭嫚瑜忛悾鎶芥煏閸ャ劌濮嶇€规洘锕㈤、娆撳礂閼规壆鍙戝┑鐘垫暩閸嬫稑螞濞嗘挸绀夐柟瀵稿仧椤╁弶绻濇繝鍌氼仴濞存粍绮撻弻鐔煎箲閹邦厾銆愭繛瀵稿婵″洭鍩€椤掑倹鍤€闁硅绱曢幑銏ゅ醇閵夈儴鎽曢梺缁樻煥婢瑰﹤危瑜版帒绠圭紒顔炬嚀椤曪繝鏌曡箛瀣偓鏍夐崱妤婄唵闁兼悂娼ф慨鍫ユ煕婵犲嫭鏆柡灞诲妼閳规垿宕卞☉鎵佸亾濡も偓闇夋繝濠傛噹娴犻亶鏌＄仦鐣屝ユい褌绶氶弻娑㈠箻鐎靛憡鍣伴梺鐐藉劵缁犳垿鎮鹃敓鐘崇劷闁挎梻鏅粙渚€姊绘担鐟板姢缂佺粯顨婇敐鐐村緞閹邦剛鐛ラ梺绯曞墲缁嬫帡鎮￠弴銏＄厸闁搞儯鍎辨俊鐓幟瑰鍛沪闁靛洤瀚伴弫鍌炲传閸曨偒鐎抽梻浣哥枃椤宕归崸妤€绠栨繛鍡樻尭閻顭跨捄鐚存敾妞ゅ繆鍓濈换婵嬫偨闂堟稐鍝楅柣蹇撶箲閻熲晛鐣锋导鏉戝唨妞ゆ挾鍊ｉ妷銉ｂ偓鎺戭潩閿濆懍澹曢柣搴㈩問閸ｎ噣宕抽敐鍛殾濠靛倸鎲￠崑鍕倶韫囧海顦﹀ù婊堢畺閺岋綁骞嬮敐鍛呮捇鏌ｉ幘璺烘灈闁哄瞼鍠撻埀顒傛暩椤牓鍩€椤掍礁鐏ユい顐ｇ箘閹瑰嫰鎼圭拠鈥虫暩濠电姷鏁搁崑娑樜熸繝鍐洸婵犻潧顑呴悡鏇㈡煙鏉堥箖妾柣鎾存礃缁绘盯骞嬮悜鍥у彆闂佸憡姊婚崰鏍蓟瀹ュ瀵犲鑸瞪戦埢鍫ユ⒑閸濆嫯顫﹂柛鏃€鍨甸锝夊级閹冲磭鍠撻幃浼村灳閸忓懎顥氶梻浣告惈缁嬩線宕㈤懖鈺冧笉闁绘劗鍎ら悡蹇撯攽閻愰潧浜炬繛鍛嚇閺岋綁骞囬濠呭惈闂佸搫鑻粔鐟扮暦椤愶箑绀嬫い鎰剁稻椤斿秹姊绘担鐟邦嚋缂佸甯為崚鎺戭吋閸ャ劌搴婂┑鐘绘涧椤戝懘锝為崨瀛樼叆婵犻潧妫欓ˉ鐘绘煏閸℃ê濮嶆慨濠冩そ楠炴劖鎯旈姀鈶╂晬缂傚倷鑳舵慨鐢稿箲閸パ屽殨妞ゆ劑鍩勯崥瀣煕濠娾偓缁€渚€骞冮幋鐐电瘈闁靛骏绲剧涵鐐亜閹存繃鍠樼€规洏鍨藉Λ鍐归搹顐ｎ棃闁诡喒鏅犲Λ鍐ㄢ槈濡ゅ啯宕熼梻鍌欐祰椤曆呮崲閹寸姵宕查柛顐犲劚缁犳牗鎱ㄥ璇蹭壕闂佺硶鏅换婵嗙暦閹烘嚦鐔煎捶椤撶姵鏅ㄦ繝纰夌磿閸嬫垿宕愰弽顓炵鐟滃繐顕ユ繝鍕磯闁靛ě鍌滅憹闂備礁鎼粔鏌ュ礉鎼达絿涓嶅Δ锝呭暞閻撶喖鏌熼幑鎰彧闁圭櫢绲跨槐鎺楁偐娓氼垰娈梺瀹狀潐閸ㄥ潡宕洪崟顖氱闁宠桨璁查崑鎾诲箛椤戣姤鏂€闂佹寧绋戠€氼參鎮為幐搴涗簻闁挎梻鏅崣鈧┑顔硷攻濡炰粙鐛箛鎾舵殕闁告洦鍋呴幊娆戠磽閸屾瑨鍏岀紒顕呭灣閹广垽宕奸妷銉х暫婵炲濮撮鍡浰夐崼鐔虹闁瑰鍎愬▓鏃堟煙閽樺鍎旀慨濠勭帛閹峰懐绮电€ｎ偆绉锋繝纰夌磿閸嬬偟鈧瑳鍛焿鐎广儱顦伴崐鐑芥煕椤垵浜芥繛鑲╁枛濮婅櫣绱掑Ο鍝勵潓濡炪倖娲﹂崣鍐箖閻ゎ垬浜归柟鐑樻尵閸樻捇鎮峰鍕煉鐎规洘绮岄埞鎴﹀窗椤旂晫娲村┑顔瑰亾闂侀潧鐗嗗Λ宀勫箯濞差亝鈷戦梻鍫氭櫅閻︽粓鏌涘Ο鍝勬倯缂佸倸绉归幃娆擃敄鐠恒劎鐣鹃梻浣虹帛閸旀洖顕ｉ崼鏇炵；闁靛／鈧崑鎾斥枔閸喗鐝梺绋款儏閿曘倝鎮鹃悜钘夌婵°倓绀侀埀顒傚厴閺屻倗鍠婇崡鐐测拫闂佺粯绋掗幑鍥ь潖缂佹ɑ濯寸紒瀣儥濡矂姊虹粙娆惧剰闁硅櫕鍔楅崚鎺戔枎閹惧磭顓洪梺缁樺姈婢瑰棛绮诲鑸电厽闁绘ê鍘栭懜顏堟煕閺傛寧鎹ｅ瑙勬礃缁轰粙宕ㄦ繝鍕箺婵犵妲呴崹浼存倶濠靛瑙︾憸鐗堝笚閻撶喖鏌ㄥ┑鍡橆棡闁靛洦绻堥弻鈥崇暆鐎ｎ剛锛熸繛瀵稿缁犳挸鐣峰鍡╂Ъ闂佸憡甯楅惄顖氼潖閾忚瀚氶柍銉ㄦ珪閻忊偓闂備礁鎼幊鎰箾閳ь剟鏌曢崱鏇犵獢鐎殿喗鎸虫慨鈧柣娆屽亾婵炵厧锕铏圭磼濮楀牅绶甸梺绋款儏閹虫劙鍩€椤掍焦鐨戦柛鐘冲姉閹广垹鈹戠€ｎ偄浠洪梻鍌氱墛閸掆偓闁绘劗鍎ら悡鏇㈡煏婵炲灝鍔撮柣鎾冲悑椤ㄣ儵鎮欑拠褑鍚Δ鐘靛仦閿氶悡銈囩磽娴ｈ偂鎴濃枍閸績鏀介柣姗嗗枛閻忚鲸绻涙径瀣创闁轰礁鍟撮弫鍐磼濮橆叀绶?
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
        // WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐差渻閵堝棗绗掓い锔垮嵆瀵煡顢旈崼鐔蜂画濠电姴锕ら崯鎵不婵犳碍鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍暜婵犲洦鍤勯柛顐ｆ礀閻撴繈鏌熼崜褏甯涢柣鎾寸洴閺屾稑鈽夐崡鐐寸亾缂備胶濮甸敃銏ゅ蓟濞戙垹绠抽柟鎯х－閻熴劑姊虹€圭媭鍤欓梺甯秮閻涱喖螣閾忚娈鹃梺鎼炲劥濞夋盯寮挊澶嗘斀闁绘灏欏Λ鍕煛婢跺﹦姘ㄩ柛瀣崌楠炲洭寮剁捄顭掔幢闂備浇顫夐崕铏櫠鎼达絽顥氬┑鍌氭啞閻撳啰鎲稿鍫濈婵﹩鍓﹂崵妤呮煕閺囥劌骞愰柛瀣崌閺佹劙宕掑☉娆戝絿缂傚倷鑳舵慨鐢告嚌妤ｅ啠鈧棃宕橀鍢壯囨煕閳╁啰鎳冩い顐庡懐纾藉ù锝堟鐢盯鏌涢弮鈧悧鐘诲Υ閸愵喖唯闁靛ě鍛緫闂備胶鎳撹ぐ鐐烘嚄閸洖绠犻柟鍓у劦閳ь剨绠撴俊鎼佸煛娴ｄ警妲伴梻浣稿暱閹碱偆鈧凹鍣ｉ妴渚€宕ㄧ€涙ǚ鎷绘繛鎾村焹閸嬫捇鏌嶈閸撴盯宕戝☉銏″殣妞ゆ牗绋掑▍鐘炽亜閺傚灝鈷旂痪鎯с偢閺岋絽螣閸喚姣㈤柡浣哥墦閹鎲撮崟顒傤槰濠电偠灏欓崰鏍ь嚕婵犳艾骞㈡俊銈呭暞濞堝ジ姊洪棃娑辨Ф闁稿孩鐓″畷鏉课熷Ч鍥︾盎闂侀潧楠忕槐鏇㈠煡婢跺浜滄い鎾寸矊婵倻鈧娲滈崰鏍€侀弴銏狀潊闁绘ɑ顔栭崬褰掓⒒閸屾艾鈧悂宕愭搴㈩偨婵娉涢悞鍨亜閹烘垵鈧憡绂掑鍫熺厾婵炶尪顕ч悘锟犳煛閸涱厼顣抽柍褜鍓氱粙鎺曟懌闂佸憡鐟ョ换鎰板煘閹达附鍋愰柟缁樺笚濞堟煡姊洪棃娑欏缂佽瀚伴崺鈧い鎺戝枤濞兼劖绻涚拠褏鐣电€规洘鍨剁换婵嬪磼濠婂嫭顔曢梻浣虹帛濮婂鍩涢崼銉ユ瀬闁糕剝绋掗悡鏇㈡倶閻愭壆鐣芥い蹇撴噽椤╂煡鎮峰▎蹇擃伀缂佲檧鍋撻梻浣圭湽閸ㄨ棄顭囪缁傛帒顭ㄩ崼鐔哄幗闁圭儤濞婂畷婵囨償閵娿儳顦梺鍦劋閹稿宕曢悢鍏肩厪濠电偛鐏濋崝銈夋煥濞戞瑧娲存慨濠呮缁辨帒螣閼姐倕寮抽梻浣告贡閳峰牓宕戞繝鍌滄殾闁荤喐澹嬮弨浠嬫倵閿濆骸浜為柛娆忔閳规垿鎮╃紒妯婚敪濠碘槅鍋呴悷鈺佺暦椤栫偛绠虫俊銈勮兌閸樻悂姊洪崨濠傚闁告柨瀛╅弲鍫曞即閻樼數锛滈梺閫炲苯澧寸€规洘锕㈤崺鐐村緞閸濄儳娉块梻鍌氼煬閸嬪嫬煤閿曞倸绠板┑鐘崇閺咁剟鏌￠崘銊モ偓鑽ゅ?Sniroute 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸鐏濋悗顓熶繆閵堝繒鍒伴柛鐕佸亞缁鈽夊Ο蹇撶秺閺佹劙宕ㄩ璺攨缂傚倷绀侀鍕嚄閸撲焦顫曢柟鎹愵嚙绾惧吋鎱ㄥ鍡楀幋闁稿鎹囬幃婊堟嚍閵夈儮鍋撻崸妤佺叆闁哄洦姘ㄩ崝宥夋煙閸愯尙鐒告慨濠勭帛閹峰懘宕ㄦ繝鍌涙畼闂備浇宕甸崰鍡涘磿閹惰棄绠查柕蹇嬪€曠粻铏繆閵堝倸浜鹃梺缁樺笒閻忔岸濡甸崟顖氱闁瑰瓨绻冮悘鎾斥攽閻愭彃鎮戦柛鏃€鐗滈幑銏犫槈閵忕姷鐓戞繝銏ｆ硾閼活垶鍩呰ぐ鎺撶厽闁规儳宕埀顒佺箞瀵寮撮悢铏诡啎闂佺粯鍔﹂崜姘舵偟閺冨倻纾肩紓浣靛灩楠炴劙鏌涚€ｎ偅灏扮紒缁樼箓閳绘捇宕归鐣屼憾婵＄偑鍊ら崢濂告偋閹惧磭鏆︽繛宸簻閸愨偓濡炪倖鎸鹃崰搴㈢濡も偓閳规垿鎮欓崣澶樻！闂佹悶鍔屽﹢杈ㄧ珶閺囥垺瀵犲瑙勭箓缂嶅﹪寮幇顓炵窞閻庯綆鍋婇弫顏堟⒒娴ｇ瓔鍤欑紒璇叉瀵煡鎮╃拠鑼舵憰闂佹寧绋戠€氀囧磻閹剧粯鏅查幖绮光偓鑼晼闂備線娼уΛ娆戞暜閻愬灚顫曢柟鐑樻尰缂嶅洭鏌曟繛鍨姕閻犲洨鍋ゅ铏规嫚閳ヨ櫕鐝繛瀛樼矋缁捇宕洪姀鈩冨劅闁靛牆娲ㄩ弶鎼佹⒑閸濆嫬鈧綊顢栧▎寰綁鏌嗗鍡忔嫽婵炶揪缍€濞咃絿鏁☉銏＄厵闁告稑锕ラ崐鎰偓瑙勬处閸ㄥ爼鐛鈧、娆撴嚌閻楀牊鍟洪梻鍌欑閹碱偄煤閵娾晛绐楅幖杈剧稻椤愪粙鏌ｉ幇顒夊殶缂佺娀绠栭弻鐔衡偓鐢殿焾娴狅箓鏌ｉ妸锕€鐏撮柡宀嬬節瀹曞崬鈻庡顖ょ悼缁辨帞绱掑Ο鑲╃杽闂佺硶鏅滈惄顖炲春閳ь剚銇勯幒鎴濃偓宄扳柦椤忓牊鐓熼柨婵嗘噹椤ㄦ瑦淇婂Δ浣瑰磳婵﹦绮幏鍛村川婵犲啫鍓甸梻浣规た閸樺ジ藝闁秴绠查柕蹇曞Л閺€浠嬫煕閳╁啰鎳勬繛鍫ョ畺濮婃椽宕ㄦ繝搴㈢暭闂佺顭紞渚€骞冮姀銈呯闁绘挸瀛╅濂告⒒娓氣偓濞佳囨偋閸℃稑绠犻幖绮规閺嬫棃鏌曟径鍫濆姉闁衡偓娴犲鐓ユ繛鎴灻鈺伱瑰鍐﹀仮闁哄本绋掔换娑㈠垂椤旂懓浜炬繝闈涙閺嗭箓鏌曡箛瀣偓鏍磻閸岀偛绠归柟纰卞幖閺嬨倝鏌涢弮鍌氬幋婵﹨娅ｉ幑鍕Ω閵夛妇浜栧┑鐘愁問閸犳牠顢氶鐘插灊濠电姵纰嶉崑鎰偓瑙勬礀濞层劑鎯侀崼鐔虹瘈闁汇垽娼у瓭闂佺锕︾划顖炲疾鐠鸿　妲堥柕蹇ョ磿閸橀亶鏌ｆ惔顖滅シ闁告柨鏈幈銊╁磼濞戞瑥寮挎繝鐢靛Т閸嬪棝鎮￠懖鈹惧亾鐟欏嫭绀€闁靛牊鎮傞獮鍐閵忊€充粧闂佹枼鏅涢崯鐗堢閺嶎厽鈷掗柛灞剧懅椤︼箓鏌熺拠褏绡€闁硅櫕绻冮妶锝夊礃閵娧嶇串闂備礁澹婇悡鍫ュ闯椤旂⒈鐒介柡宥庡亞绾捐棄霉閿濆牆浜楅柟瀵稿仜閸ㄦ棃鏌熺紒銏犳灍闁稿鍓濈换娑㈠幢濡ゅ啰顔囧銈呯箚閺呮盯婀侀梺缁橈供閸嬪懎鈻嶆繝鍐╁弿濠电姴鎳忛鐘电磼椤旂晫鎳冮柍璇查叄瀹曟粏顦插┑顔笺偢濮婄粯鎷呮笟顖滃姼闂佹寧娲忛崕鎶藉箲閵忋倕纾奸柣鎰綑娴犲ジ姊虹紒妯虹伇濠殿喓鍊濆鏌ュ箵閹烘繄鍞甸柣鐘烘鐏忋劑宕濋悢鎼炰簻闁挎柨鎼慨鍌涙叏婵犲啯銇濇俊顐㈠暙闇夐柕鍫濇濞插鈧娲滈崰鏍€佸Δ浣哥窞濠电姴鍟悵顒勬⒒閸屾艾鈧悂宕愰幖浣哥９闁绘垼濮ら崐鍧楁煥閺冨牊鏆滈柛瀣尵缁厼鈽夊Ο璇叉閻庤娲橀悡锟犲蓟濞戞鏆嗛柍褜鍓熷畷鎰槈閵忕姷鍊為梺闈涱焾閸庢椽宕甸妶澶嬧拺闁告繂瀚峰Σ褰掓煕閵娿儳绉虹€殿喗妲掗ˇ瑙勬叏婵犲啯銇濈€规洦鍋婂畷鐔碱敋閸涱喛鏅ч梻鍌欒兌椤牏鈧稈鏅滅换娑欑節閸屾粍娈鹃梺鎸庣箓椤︻垳绮婚懡銈囩＝濞达綀顕栭悞浠嬫煕濮椻偓娴滆泛顫忓ú顏咁棃婵炴垼椴搁鍡欑磽娴ｈ棄钄煎┑顔哄€栫粚杈ㄧ節閸ャ劌浠奸悗鍏夊亾闁逞屽墰婢规洘绺介崨濠勫帗閻熸粍绮撳畷婊堟偄妞嬪孩娈炬繝闈涘€搁幉锟犲磻閸曨厾纾介柛顐犲灩鍟搁梺鐑╂櫅闁帮絽顫忕紒妯诲闁荤喖鍋婇崵瀣攽閳藉棗浜楃紒鑸电娣囧﹪骞橀鑲╁姦濡炪倖甯掔€氼參鎮￠弴銏″€甸柨婵嗛娴滄繄鈧娲栭惌鍌炲蓟濞戙垹鐓涢悗锝庡墰閻﹀牓姊虹拠鈥虫灍濠电偛锕獮鍡涘炊閵娿倗绠氭繝?YouTube/Play 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁绘劦鍓欓崝銈囩磽瀹ュ拑韬€殿喖顭烽幃銏ゅ礂鐏忔牗瀚介梺璇查叄濞佳勭珶婵犲伣锝夘敋閳ь剙顫忛崫鍕懷囧炊瑜夐崑鎾诲Χ婢跺﹦鐛ュ┑掳鍊曢幊搴㈩攰闂備礁鎲″ú锕傚垂娴兼潙绀冮柍褜鍓欓—鍐Χ閸℃袝濠电姭鎳囬崑鎾剁磽娓氬洤鏋涢柣鏍帶椤繐煤椤忓嫮顔愰梺缁樺姈瑜板啴鈥栭崼銉︹拺缂佸灏呭銉︾箾鐏忔牑鍋撻幇浣告闁荤姴娲︾粊鏉懳ｉ崼鐔虹瘈濠电姴鍊搁銏狀潰閸パ€鏀介柨娑樺娴滃ジ鏌涙繝鍐ㄧ伌鐎规洘绻傞悾婵嬪礋椤愩倗鏋冮梻浣筋潐瀹曟﹢宕洪弽褏鏆﹂柛娆忣槹閸欏繑鎱ㄥΔ鈧Λ妤呭储閹绢喗鐓涢柍褜鍓氱粋鎺斺偓锝庡亞閸樿棄鈹戦绛嬫當婵☆偅顨婂绋库槈閵忥紕鍘介棅顐㈡处缁嬫劙骞夋ィ鍐╃厸閻忕偛澧藉ú鎾煕閳哄绡€鐎规洘甯掗…銊╁礃閿濆棗顏梻浣筋嚙濮橈箓锝炴径濞掑搫螣閸忕厧搴婇梺鍓插亝缁诲秹宕甸弴鐏诲綊鎮╁顔煎壘闂佽桨绀佸ú顓㈠蓟閿濆绠涙い鏇炴噺濞堟壆绱撴笟鈧禍鑸电鐠轰警娼栭柧蹇氼潐鐎氭岸鏌熺紒妯虹瑨婵絾鍔楃槐鎾诲磼濮樻瘷銏′繆椤愶絿绠撻柣锝囧厴瀵墎鎷犻悙顒侇棃鐎规洏鍔戦、娆撴⒒鐎靛憡娈梻鍌氬€风粈渚€骞栭鈷氭椽濡搁埡浣虹崶闂佺硶鍓濋…鍥€呴崣澶岀瘈濠电姴鍊绘晶娑㈡煟閹惧鎳冮棁澶愭煟閹捐櫕鎹ｉ柟鐣屽У缁绘盯骞婂畡鏉垮箻闁荤喖鍋婇悡銉╂煕閺囥劌骞橀柣鎾亾濠碉紕鍋戦崐銈夊矗閳ь剟鏌涙惔鈽嗙吋閽樻繈鏌涢幇顒佲枙闁绘柨妫濋弻锝夋偄閸濆嫷鏆銈冨劤婵灚绌辨繝鍥х煑濠㈣泛锕ら～鎺懳旈悩闈涗沪閻㈩垱甯熼悘鍐⒑闁偛鑻晶鎾煙椤旀儳浠辩€规洖缍婇、鏇㈡偐鏉堚晝娉块梻鍌欑濠€閬嶅磿閵堝鍨傞柣銏犲閺佸倿鏌嶉崫鍕簽婵炲牅绮欓弻锝夊箛閸忓摜鐩庨梺璇叉禋娴滅偟妲愰幒鏃傜＜婵☆垰鍚嬮崚娑樜旈悩闈涗粶妞ゆ垵顦甸獮鍐ㄢ枎閹惧啿鐎銈嗗姂閸ㄦ椽鎮甸鈧?
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

    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇洟姊绘担钘壭撻柨姘亜閿旇鏋ょ紒杈ㄦ瀵挳濮€閳锯偓閹风粯绻涙潏鍓у埌闁硅绻濆畷顖炴倷閻戞鍘遍梺鍝勫暞閹搁箖鎮鹃悽纰樺亾鐟欏嫭纾婚柛妤€鍟块锝嗙鐎ｅ灚鏅濋梺闈涚箚閺呮粓藟閳ユ枼鏀介柣妯虹仛閺嗏晛鈹戦鐐毄闁哥姴锕ら鍏煎緞鐎ｎ亙绨垫繝鐢靛仜濡瑩骞愭繝姘亗婵炲棙鎸婚崑鈩冪箾閸℃绠版い蹇婃櫅閳规垿顢氶崱娆戞殼闂佸搫鏈惄顖涗繆閸洖骞㈡繛鍡樺姇濞呮绱撻崒娆愮グ濡炴潙鎽滈弫顕€鏁撻悩鑼暫闂佸啿鎼幊蹇涘吹閹寸偑浜滈柟鐑樺灥閳ь剚鍔欐俊鎼佸煛閸屾瀚奸梺鑽ゅТ濞茬娀鍩€椤掑啯鐝柣蹇撶Т椤啴濡堕崘銊ヮ瀳闂佺娅曢敃銏犵暦濞差亜唯闁挎洍鍋撶紒鍓佸仜閳规垿鎮欓棃娑楀闂佽瀛╁钘夘潖濞差亜绠氱憸搴ｇ矉鐎ｎ喗鐓曢悗锝庝簻椤忣參鏌熼鏂よ€块柛鈺嬬節瀹曟﹢顢旈崱顓犲簥濠碉紕鍋戦崐鏍涙笟鈧畷鎴﹀箛椤撶喐鐝烽梺闈涚箞閸婃牠鎮￠崘顏呭枑婵犲﹤鐗嗙粈鍫ユ煏婵炑冩噽椤︻垶姊虹化鏇炲⒉缂佸鍨圭划锝呂旀担椋庣畾闂侀潧鐗嗛幊蹇涘闯濞差亝鐓曢悗锝庝簻閳ь剙娼″濠氭晸閻樿尙鍊為梺闈涱槶閸庤櫕绂掗懖鈺冪＝濞达絽鎼瀷濡炪値鍋勯ˇ閬嶅箲閵忕姭妲堥柕蹇婂墲濞呮粓鏌熼懖鈺勊夐柍褜鍓氶崜姘焽婵犳碍鈷掑ù锝囩摂閸ゆ瑦绻涚€涙顣茬紒鍌氱Ч椤㈡棃宕煎┑鍫㈡毇婵犵妲呴崹闈涒枖閺囥垺鍋柍褜鍓熷娲濞戝磭纭€闂佸憡渚楅崰妤咁敊閹剧粯鈷掗柛灞剧懅椤︼箓鏌熺喊鍗炰喊闁诡垰顦扮缓鐣岀矙鐠恒劎宕堕梻浣告惈缁嬩線宕㈣ぐ鎺戠劦妞ゆ垼娉曢ˇ锕傛煃閽樺妲告い顐ｇ矒瀹曞崬螣閸濆嫬甯繝纰夌磿閸嬫垿宕愰弽顐ｆ殰濠电姴娲﹂崐鍧楁煥閺囩偛浜為柡浣革躬閺岀喖鎮滃鍡樼暦闂佺粯鎸鹃崰鏍嵁閺嶎灔搴敆閳ь剟鍩€椤戞儳鈧洟鈥﹂崶顒€绠涙い鎾跺Х椤旀洟姊洪崨濠勬噧妞わ箓浜堕幆宀勫箳閺傚搫浜鹃悷娆忓绾惧鏌涘Δ鈧崯鍧楊敋閿濆棛顩烽悗锝呯仛閺咃絽鈹戦悩娆忓閸斻倕霉濠婂嫬鍔ら棁澶愭煥濠靛棙鍣规い銉ョ箲缁绘稓绮崫鍕潎闂佸搫鑻粔鐑铰ㄦ笟鈧弻娑㈠箻鐠虹儤鐏堥梺缁樹緱閸ｏ絽鐣峰鈧垾锕傚箣閻樻彃姹查梻鍌欑婢瑰﹪宕戦崨顖涘床闁稿瞼鍋為崵鈧梺鍓茬厛閸犳帡寮ㄦ禒瀣厓闁芥ê顦伴ˉ婊堟煟韫囧鍔ら柍褜鍓濋～澶娒洪悢濂夌劷鐟滃秴鐣甸崟顐熸斀妞ゆ柨顫曟禒婊堟煕鐎ｎ偅灏伴柟渚垮妽缁绘繈宕掗妶鍥ф倯闁诲氦顫夊ú妯兼暜閹烘缍栨繝闈涱儛閺佸嫰鏌﹀Ο渚Х濠㈣娲樻穱濠囨倷瀹割喖鍓板┑鐐差嚟閸忔﹢骞嗗畝鍕耿婵＄偛鐨烽崑鎾诲箳閹搭厽鍍甸梺鎸庣箓閹冲秵绔熼弴鐔剁箚闁靛牆娲ゅ暩闂佺顑囬崑銈夊箖瑜旈幃鈺佇ч崶锝呬壕濞撴埃鍋撶€殿喗娼欓～婵囨綇閳诡兙鍨藉缁樻媴鐟欏嫬浠╅梺绋垮濡炶棄鐣峰鍫熸櫇闁稿本纰嶉悗顒勬倵楠炲灝鍔氭い锔垮嵆閹繝鎮㈤崗鑲╁幍闂備緡鍙忕粻鎴︽倶閻樼粯鐓熼柕濠忛檮閵囨繈鏌″畝瀣？濞寸媴绠撻幃鍓т沪鐟欙絽寰嶅┑鐘愁問閸犳牠鏁冮妸銉㈡瀺闁挎繂鎳夊Σ鍫ユ煟閵忋埄鐒剧€瑰憡绻冮妵鍕棘閹冪彑闂侀€炲苯澧柛鏃€娲熼垾鏃堝礃椤斿槈褔骞栫划鍏夊亾閼碱剙鍤┑鐘垫暩閸嬬姷浜稿▎鎾崇獥闁哄诞鍛濠电偛妯婃禍婊堟倿閸偁浜滈柟鍝勭Ч濡惧嘲霉濠婂嫮鐭掗柡宀€鍠愬蹇涘礈瑜忚摫缂備胶铏庨崢鍏兼櫠鎼达絾顫曢柟鐑橆殔缁犳稒銇勯幘璺烘毐闁归鎳撻—鍐Χ閸愩劌惟闂佺娴烽弫濠氱嵁閸愩劉鏋庨煫鍥风到閺呯姴顪冮妶鍡樷拻闁告鍕彾閹兼番鍔嶉埛鎺懨归敐鍫澬撻柕鍡楀暟缁辨帡鍩€椤掍胶鐟归柍褜鍓欓悾鐑藉箛閺夊潡鏁滃┑掳鍊愰崑鎾剁磼閻橀潧鈻堥柡宀嬬秮楠炲洭顢楁担鐟板壍闂佸摜鍎愰崹璺侯潖濞差亜绠伴幖绮规噰閹峰綊姊虹粙鍧楊€楅柛鐔锋健椤㈡艾鈻庨幘鏉戔偓缁樹繆椤栨繂浜归柣锕€鐗撳鍝勑ч崶褏浼堝┑鐐板尃閸曨厼鈪版繝纰夌磿閸嬫垿宕愰弽褜娼栫憸鐗堝笚閸嬪鏌ｅΟ鍨毢闁哄棴绠撻弻鈩冨緞鎼搭喖娈鹃梺鍝勫暙閻楀棗顔忓┑鍥ヤ簻闁哄啫娲よ闂佺粯绺块崝鎴濐潖缂佹ɑ濯撮柦妯侯槸閹偤姊洪崫銉バｉ柛鏃€顨婂畷姘跺箳閹搭厼鎮戦梺鍛婄矆閼冲爼宕棃娑掓斀妞ゆ柨顫曟禒婊堟煕鐎ｎ偅宕岄柟绋匡工閳规垹鈧綆鍋嗛崢鍗炩攽鎺抽崐鏇㈠疮椤愶箑鐒垫い鎺戯功缁夘噣鏌℃担鐟板鐎垫澘瀚换婵嬪礋椤撶喐绶繝纰夌磿閸嬫垿宕愰弽顓炲瀭闁绘梻鍘ч悿鐐節婵犲倸鎮╂繛鎴欏灩缁€鍐煏婵炑冨椤旀洟姊绘笟鈧褑澧濋梺鍝勬噳閺呯姴顕ｉ锝囩瘈闁搞儯鍔庨崢闈涱渻閵堝棛澧俊顐ｎ殜瀹曨垱瀵奸幖顓熸櫓闂佸湱鍎ゅ鑽ゅ娴犲鐓曢悘鐐村礃婢规﹢鏌嶈閸撴盯宕楀鈧獮濠偽旈崨顓㈠敹闂佸搫娲ㄩ崰鎰板磿椤忓牊鐓欓柛蹇氬亹閺嗘﹢鏌涢弬璺ㄧ劯鐎规洘甯℃俊鎼佸Ψ閵忊剝鏉搁梻浣侯焾閺堫剛鍒掗悩缁樺€块柛鎾楀懐锛滈梺缁橈耿濞佳囧煝閸噥娈介柣鎰嚟婢ь剚绻涢悡搴ｇ鐎规洘绮忛ˇ鎾嫅鏉堛劋绻嗛柣鎰典簻閳ь兙鍊濆畷鎴﹀川椤掑吋妞介幃銏ゆ偂鎼淬倖鎲伴梻渚€娼ц墝闁哄懏绋戣灋婵せ鍋撻柡灞剧洴閳ワ箓骞嬪┑鍥╀簮闂備礁婀遍…鍫ュ疮閹殿喗顫曢柟鐑橆殕閸婇攱銇勯幒鍡椾壕闂佸憡鏌ｉ崐婵嬪蓟濞戞﹩娼╂い鎺戭槸閸撹京绱撴担鍝ョ劮濡炴潙鎽滈幑銏犫攽鐎ｎ亞鍊炲銈庡墻閸庢悂宕堕妸褍骞堥梺璇茬箳閸嬫稒鏅舵禒瀣亗妞ゆ牗鍑归悢鍡涙偡濞嗗繐顏紒鈧崘銊囧酣宕惰闊剟鏌熼鐣屾噰妞ゃ垺妫冨畷鐔煎Ω閵夈倕顥氶柣鐔哥矊缁绘帡宕氶幒鎾剁瘈婵﹩鍓涢鎺楁⒑瑜版帩鏆掗柤鍐茬埣瀹曟繈鎮滈懞銉ヤ画濠电姴锕ょ€氼參藟閸噥娈介柣鎰絻閺嗙偤鏌曢崼顒傜М鐎规洘锕㈤崺鈩冩媴閹绘帊澹曟繝鐢靛У绾板秹鎮￠悩缁樼厵妞ゆ挾鍠庣粭鎺楁煕閺冣偓缁秶鎹㈠☉銏犲窛妞ゆ柨澧介悡浣虹磽娴ｈ櫣甯涚紒璇茬墛閹便劑鍩€椤掑嫭鐓ユ繝闈涙閸ｄ粙鏌涘鍡椾喊闁哄矉绠撳畷姗€顢欑紒妯烘灓闂備礁鎼惌澶屾崲濠靛棛鏆︽い蹇撴噺婵绱掔€ｎ偒鍎ユ繛鐓庮煼濮婄粯鎷呴崫銉︾€銈庡亜椤﹂潧鐣疯ぐ鎺戦敜婵°倐鍋撶紒鐘崇墵閺屽秹宕崟顐ｆ闁诲孩纰嶅銊╁箟缁嬫鍚嬮柛鈩冾殢閸ゃ倝姊虹憴鍕棆濠⒀勵殜閸╂盯骞嬮敂鐣屽幈濠电娀娼х€氼剟宕濆鑸电厸闁糕剝绋愰幉楣冩煛瀹€瀣瘈鐎规洏鍔戦、娑樞掔憗銈呯伈闁哄矉缍侀弫鎰板炊瑜嶉獮瀣⒑鐠団€虫灀闁哄懏绻堥獮蹇涙偐鐠囪尙顔岄梺鍦劋閸ㄥ爼寮宠箛鎾斀闁挎稑瀚禍濂告煕婵犲啰澧垫鐐村姈閵堬綁宕橀妸褎鐒鹃梻浣侯潒閸曞灚鐣剁紓浣哄閸ㄨ京鎹㈠☉姗嗗晠妞ゆ棁宕甸崙褰掓⒑閹惰姤鏁遍柛銊ユ健瀵鈽夐姀鈩冩珕闁荤姴娲﹁ぐ鍐偓娑崇秮濮婅櫣绱掑Ο铏诡儌闂佺顑嗛幐楣冨箲閵忕姭妲堟慨姗堢到娴滈箖鏌ｉ姀鈽嗗晱闁绘帡绠栭弻娑樜熺紒妯哄攭闂佸搫鏈惄顖炲春閻愬搫鍨傛い鏃傚帶椤岸鏌ｆ惔锛勪粵婵犮垺顭囬埀顒佺煯閸楁娊鎮伴鈧畷鍫曨敆婢跺娅嶉梻浣虹帛钃辩憸鏉垮暟缁崵鎷犲ù瀣杸闂佺粯锚閻忔岸寮抽埡鍛厱閻庯綆鍓欏暩婵炲瓨绮嶉幃鍌炪€侀弮鍫濋唶妞ゆ劧绲惧▍鏃堟煟閻斿摜鐭嬫俊顐㈠暣閻涱噣宕橀…鎴炲缓闂侀€炲苯澧ǎ鍥э躬閹粓鎸婃径灞藉箰闂備胶顭堥張顒傚垝瀹€鈧弫顔姐偅閸愨斁鎷绘繛杈剧秬椤宕戦悩缁樼厱閹兼惌鍠栭悘锔锯偓瑙勬礃缁诲嫭绂掗敃鍌涘€锋い鎺嗗亾鐎殿喖娼￠弻锝堢疀閺囩偘绮ч梺鍛婃礃钃辩紒鍌氱Т椤劑宕奸悢鍝勫汲闂備礁鎼ú锕傘€冮崨顔绢洸闁告挆鍛紳闂佺鏈銊ョ摥闂備焦瀵уú锔界濠婂牜鏁嬮柨婵嗘缁♀偓濠殿喗锕╅崢鍏肩濠婂懐纾奸柣鎰靛墮椤庢粌顪冪€涙ɑ鍊愮€殿喗鐓￠崺锟犲川椤旇瀚奸梻浣虹帛閹稿摜鈧灚甯掗…鍥偄閸忚偐鍘辨繝鐢靛Т閸犳艾鈻撳鍫熺厵妞ゆ洖妫涚弧鈧Δ鐘靛仦椤洭鍩€椤掑嫭娑ч柟鑺ョ矒閹線宕奸姀銏紲闂佸憡鎸嗛崘鐐瘔闂備焦瀵ч懝楣冣€﹂懗顖ｆЪ闂佺粯鎼换婵嗩嚕婵犳碍鍋勭痪鎷岄哺閺呪晠姊虹紒妯诲碍缂併劌鐖煎畷鎴﹀箻鐠囨彃鐎銈嗘礀閹冲骸鈻撻妸銉富闁靛牆妫楁慨褏绱掗悩鍐茬仼濠㈣娲熷畷妤呮嚃閳哄喚鍟庨梻浣烘嚀椤曨參宕戝☉銏犲惞妞ゆ帊鑳剁粻楣冩煛婢跺鐏ユい蹇ｅ弮閺屸€崇暆鐎ｎ剛蓱闂佽鍨卞Λ鍐€佸☉姗嗙叆闁告稑鎷戠紞浣割潖閾忓湱鐭欐繛鍡樺劤閸撻亶鏌ｉ姀鈺佺伈缂佺粯绻傞悾宄懊洪鍕姦濡炪倖甯婇梽宥嗙濠婂牊鐓欓柛婵嗗鍟搁梺绋跨箰閼活垶鈥﹂懗顖ｆЩ闂佽绻戝畝鎼佺嵁閸儱惟闁挎柨澧介惁鍫ユ⒑閸涘﹥澶勯柛妯垮亹缁參鏁撻悩鏂ユ嫼闂佽崵鍠愬妯何ｆ繝姘厵闁惧浚鍊嬮鍫熷仼闁绘垼妫勬儫闂佹寧鏌ㄦ晶浠嬫儊閸儲鈷戦柛娑橈工婵倿鏌涢弬鎸庢拱缂佹梻鍠庨濂稿炊閳哄喛绱抽梻浣侯焾閺堫剟鎳濇ィ鍐ㄧ劦妞ゆ帒瀚峰Λ鎴犵磼椤旇偐澧涚紒妤冨枛閸┾偓妞ゆ帒鍟ˉ銈夋⒒娴ｅ憡鎯堟い锔垮嵆椤㈡牕鈻庨幘鏉戔偓鍧楁煠閹帒鍔ょ紒鐘荤畺閺屾盯鍩勯崗鐙€浜幃姗€濡疯閸嬫挸鈻撻崹顔界亶闂佺粯鎸撮埀顒佸墯濞兼牜绱撴担鑲℃垶鍒婇幘顔界厽闁绘梻鍘ф禍浼存煕閵堝拋鍎忛柍瑙勫灴閹瑩寮堕幋鐘点偡缂傚倷绀侀ˇ閬嶅箠閹剧粯鍋╅柣鎴ｆ缁狅綁鏌ㄩ弴妤€浜剧紓浣哄Т椤兘骞冭ぐ鎺戠倞妞ゅ繐瀚В銏狀渻閵堝啫鐏╅柟铏～蹇曠磼濡偐鎳濋梺鎼炲劀閸愮偓浜ょ紓鍌氬€风粈渚€藝闁秴鏋佸┑鐘宠壘缁犳牠鏌嶉妷锔界伇闁哄閰ｉ弻鐔煎箚瑜忛敍宥呂旈悩鍙夊枠婵﹥妞藉畷婊堝箵閹哄秶鍑圭紓鍌欐祰椤曆囧磹閸ф鏄ユ繛鎴欏灩缁秹鏌涢銈呮灁闁告ü绮欏娲箰鎼达絿鐣甸梺缁橆殔閹虫ê鐣烽妷鈺婃晝闁靛牆娲ㄩ敍婊堟煛婢跺﹦澧戦柛鏂胯嫰闇夋い鏇楀亾闁哄本鐩崺鐐哄箚瑜屾竟鏇炩攽閿涘嫬浜奸柛濠冪墱閺侇噣骞掗弬鍝勪壕婵鍘ф晶鎵磼椤旇偐澧㈤柍褜鍓ㄧ紞鍡涘磻娴ｅ湱顩叉繝濠傜墛閻撴稓鈧箍鍎遍崯顐ｄ繆閸ф鐓忛柛鈩兠ù顕€鏌＄仦鍓р槈閾伙綁鏌涘┑鍡楊伂闁糕晝鍋ゅ铏光偓鍦濞兼劙鏌涢妸銉﹀仴妤犵偛鍟撮崺锟犲礃椤忓棴绱叉繝纰樻閸ㄤ即鈥﹂悜钘夋瀬妞ゅ繐鎳愮弧鈧紒鍓у钃辨い顐躬閺屾稓鈧綆浜濋崵鍥煏閸℃鏆炵紒缁樼箞瀹曞爼濡烽姀鐘卞婵炴挻鍩冮崑鎾垛偓娈垮櫘閸ｏ絽鐣烽幒鎳虫棃鍩€椤掍胶顩查柣鎰靛墰缁犻箖鏌熺€电鍓卞ù鐘荤畺閺屻倕霉鐎ｎ偅鐝曢悗瑙勬礃閻擄繝寮诲☉銏犵労闁告劦浜栨慨鍥⒑缂佹ɑ灏伴柣鈺婂灦楠炲啫顫滈埀顒勫箖濞嗘劧绱ｆ繝闈涙椤斿嫰姊绘担鍛婂暈婵﹤缍婇幃鐑藉煛閸涱叀鎽曢梺缁樻⒒閸樠囨嫅閻斿吋鐓忓璇″灠閹冲繐袙閸ヮ剚鈷掑ù锝呮啞閹牓鏌ｉ鐑嗘Ш婵″弶鍔欏鎾偄娓氼垱绁梻浣瑰濞叉牠宕愰崫銉х焼闁割偁鍨洪崰鎰熆鐠哄ソ锟犳晲婢跺﹪鍞跺┑鐘茬仛閸旀牜鈧潧鐭傚娲濞戞艾顣烘俊銈囧У閹倿鎮伴鈧獮瀣晜閻ｅ苯骞堥梻浣告惈閸熺娀宕戦幘缁樼厱闁靛ě鍕瘓闂佽鍠楅崕鎶芥偩閿熺姵鐒介柨鏃囨缂傛捇姊绘笟鈧埀顒傚仜閼活垱鏅堕幘顔界厪闁糕剝娲滅粣鏃傗偓娈垮櫘閸ｏ絽鐣烽悡搴樻斀闁糕剝鍩冮崑鎾诲锤濡や讲鎷洪梺鍛婄箓鐎氼厼顔忓┑鍡忔斀妞ゆ梹鍎抽崢鎾寠濠靛鐓忓┑鐐茬仢閸旀鏌￠埀顒佺鐎ｎ偆鍘藉┑鈽嗗灥濞咃絾绂掑☉銏＄厸闁糕€崇箲濞呭懘鏌嶇憴鍕伌妞ゃ垺宀稿畷銊╊敇閻愮數娼夊┑掳鍊楁慨鐑藉磻濞戞碍宕叉俊顖濇閺嗭附鎱ㄥ璇蹭壕濡炪們鍨洪悷鈺呭箠閺嶎厼鐓涘┑鐘插敪瑜斿濠氬磼濞嗘埈妲繝銏㈡嚀閿曨亪骞冮敓鐘查唶闁靛鍎抽敍娑㈡⒑缁洖澧茬紒瀣灩閻ヮ亣顦撮柟鍙夋倐閹囧醇濠靛牏鎳栭梻浣虹帛閹搁箖宕伴弽顓犲祦闁哄稁鐏旀惔顭戞晢闁逞屽墴閹偛煤椤忓懐鍘遍梺缁樻椤ユ挾绮绘繝姘厸閻忕偛澧藉ú鎾煙椤旇娅婄€规洘锕㈤獮鎾诲箳閹炬潙娈橀梻浣筋嚙鐎涒晠顢欓弽顓為棷妞ゆ洍鍋撶€规洘鍨垮畷銊╊敍濞戞ê绨ユ繝鐢靛█濞佳兾涢鐑嗙劷闁冲搫鎳忛悡蹇擃熆鐠鸿櫣澧曢柣蹇婃櫅闇夋繝濠傚閸斻倝鏌曢崱妯虹瑨妞ゎ偅绻堥弫鎰板川椤掆偓椤ユ岸姊虹拠鎻掝劉妞ゆ梹鐗犲畷浼村即閻樻彃鐏婂┑鐐叉閹稿爼鍩€椤戣法顦︽い顐ｇ矒閸┾偓妞ゆ帒瀚粻鏍煏韫囧鈧洘瀵奸悩缁樼厱闁哄洢鍔屾禍婵嬪级閸繄澧涚紒缁樼箘閸犲﹥寰勫畝鈧敍鐔兼⒑缁嬫鍎愰柛銊ョ仢閻ｇ兘骞囬弶鍨敤濡炪倖鎸鹃崰搴㈢閸楃偐鏀介幒鎶藉磹閺囥垹绠犻煫鍥ㄧ☉绾炬寧淇婇妶鍕濞存粍绮撻弻锟犲礃閵娿儮鍋撻悷鎵虫瀺闁哄洢鍨洪悡?
    // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婄闁挎繂妫Λ鍕⒑閸濆嫷鍎庣紒鑸靛哺瀵鈽夊Ο閿嬵潔濠殿喗顨呴悧濠囧极妤ｅ啯鈷戦柛娑橈功閹冲啰绱掔紒妯烘诞闁糕斁鍋撳銈嗗坊閸嬫挾绱掗悩鑼х€规洘娲熼弻鍡楃暤閵夈儲鍠樻い銏＄☉椤劑宕ㄩ婊冨Ц闂備浇顕ч崙鐣岀礊閸℃稑纾诲ù锝呮贡椤╁弶绻濇繝鍌滃闁绘挻鐟╅弻鐔告媴閸愨晝褰у┑鐐叉噹閹虫﹢寮婚敐澶婄闁哄倸鐏濇俊铏圭磽娴ｈ櫣甯涢柣鈺婂灠閻ｅ嘲螖閸涱厾顦ч梺鍏肩ゴ閺呪晛危鐟欏嫮绡€婵炲牆鐏濋弸娑㈡煥閺囶亜顩柛鎺撳浮椤㈡盯鎮欓懠顒夊數闂備礁鎲＄粙鎴︽偤閵娾晛绠氶柣鎰劋閻撶喖鏌ｉ弮鍌ょ劸妞ゃ儱顑嗛妵鍕箛椤戞儳浠┑顔硷功缁垶骞忛崨瀛樻優闁荤喐澹嗛鍏间繆閻愵亜鈧垿宕濈仦杞挎稑鈹戠€ｎ€儵鏌涢幇闈涙灈闂佸崬娲弻鏇熷緞閸繂濮㈠銈忓瘜閸嬪﹤顫忓ú顏勬嵍妞ゆ挾鍋涙俊娲⒑缁洘娅呴柣妤佹尭閻ｇ兘濡疯閸嬫捇鏁愭惔鈩冪亶闂佺粯鎸诲ú鐔煎蓟閺囩喎绶為柛顐ｇ箘娴犳儳鈹戦悙鍙夊櫣婵炲樊鍙冨濠氭晲婢跺﹦顔掗柣搴㈢⊕閿曗晛螞閸愩劉鏀芥い鏃傘€嬮弨缁樹繆閻愯埖顥夐柣锝囧厴婵℃悂鏁傞崜褏妲囬梺鐟板悑閻ｎ亪宕规繝姘辈妞ゆ挶鍨洪埛鎺懨归敐鍕劅闁绘帞鍋撻妵鍕箣濠靛浂妫﹂悗瑙勬礃缁诲倿鍩㈡惔銈囩杸闁挎繂鎳忛鍧楁⒒娓氣偓濞佳団€﹂鐔剁箚闁搞儺鍏欓埀顒€鎳橀幃婊堟嚍閵夈儮鍋撻悽鍛婄叆婵犻潧妫濋妤€霉濠婂懎浠遍柡灞剧☉椤粓鍩€椤掍胶顩查悹杞拌濞兼牗绻涘顔荤凹妞ゃ儱鐗婄换娑㈠箣閿濆鎽甸柡浣哥墦濮婄粯鎷呯憴鍕哗闂佸憡鏌ㄩ柊锝咁嚕椤愶絿绡€婵ê宕粊锔界節閻㈤潧孝闁硅櫕鍔欓幃鍧楀焵椤掑嫭鈷戦悷娆忓閸斻倝鏌ｆ幊閸旀垿骞嗘担瑙勫劅闁挎繂娲ㄩ敍?ASCII闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栭梻鍌欑閹诧繝骞愰崱娑樺窛妞ゆ梻鐡旈崯鍛存⒒閸屾瑦绁版い鏇嗗應鍋撳☉鎺撴珚闁硅櫕绻冮妶锝夊礂椤栨碍澶勯悗闈涖偢瀵爼骞婄粵鍦暤闁哄本鐩鎾Ω閵壯傚摋缂傚倷鑳舵慨瀵稿椤撱垹鐒垫い鎺嗗亾缂佺姴绉瑰畷鏇㈠础閻忕粯妞芥俊鍫曞幢閺囩姷鐣鹃梻浣藉Г閿氭い锔诲亰瀵劍绂掔€ｎ偆鍘介梺褰掑亰閸撴瑧鐥閺屾盯鏁愰崼鐕佹闂佸疇顫夐崹鍧椼€佸▎鎾崇畾鐟滃秹鐛€ｎ喗鈷戠紒顖涙礃閺夊綊鏌涚€ｎ偅灏い顏勫暣婵″爼宕卞Δ鈧鎴︽⒑缁嬫鍎愰柟绋垮⒔濡叉劙骞掑Δ浣镐汗闂佽偐鈷堥崕鐤槾闁逞屽墲椤煤濡ソ娲偄妞嬪孩娈鹃梺璺ㄥ枔婵敻宕愭搴樺亾楠炲灝鍔氭俊顐ｇ洴閸┿儲寰勯幇顓犲帗闂佽姤锚椤﹁棄螣閳ь剟姊洪幐搴ｇ畼闁稿鍊濋妴渚€寮介鐔风檮婵犮垼娉涢ˇ顖炈?U+00A9(濠? 闂?U+00B7(闂? 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿喗娼欒灃闁逞屽墯缁傚秹骞嗚濞撳鏌曢崼婵堝嚬缂併劌顭烽弻娑㈡偄閸涘﹦绋囬梺浼欑悼閺屽濡甸幇鏉跨婵犻潧娲﹀▍鏃堟⒒娴ｅ憡鍟炲〒姘殜瀹曞綊鎮ч崼鐔峰伎濠电偛妯婃禍婵嬪煕閹烘嚚褰掓晲閸曨噮鍔呭┑鐐插悑鐢繝寮诲☉銏犲嵆闁绘劕鍚€缁敻姊烘导娆戝埌濠⒀冩捣濡叉劙骞樼€涙ê顎撻梺鍦帛鐢﹥绔熼弴鐐╂斀闁绘﹩鍠栭悘閬嶆煕閳哄倻澧电€规洘绻傝灃闁告侗鍘鹃悡鎴︽⒑閻熸澘鈷旂紒顕呭灦閹繝寮撮姀锛勫帾婵犮垼鍩栫粙鎾诲礆閺夊簱鏀介梽鍥涢崟顖氱厴闁硅揪瀵岄弫宥嗙節闂堟稒顥欐慨瑙勵殜濮婃椽鎮烽弶璺ㄩ獓闂佽鍠栭崐鍧楃嵁閸愵収妯勯悗瑙勬礈閸樠囧煘閹达箑閱囬柕蹇娾偓铏彲濠电姴鐥夐弶搴撳亾濡や焦鍙忛柣鎴ｆ绾惧鏌熸潏鍓х暠闁搞劌鍊块弻锝夊箣閻戝棛鍔烽梺宕囨嚀缁夋挳鍩為幋锔藉亹闁告瑥顦遍妶鈺呮⒑鏉炴壆顦︾紒澶屾暬楠炲牓濡搁妷搴ｅ枛瀹曠螖閳ь剟骞楅悽鍛娾拺閻犲楠搁崐鑽ょ玻閺冣偓閹便劍绻濋崘鈹夸虎閻庤娲﹂崑濠傜暦閻斿吋顥堟繛鎴灻ˉ搴ㄦ⒒閸屾瑦绁扮€规洜鏁诲畷鎶芥晜閹存帞绠氶梺鎼炲労閸撴瑧绮婚弻銉︾厪闊洦娲栬ⅴ闂佽　鍋撳ù鐘差儐閸婄敻鏌ㄥ┑鍡欏嚬闁硅尙鍋撶换娑㈡偂鎼搭垵鍚┑顔硷攻濡炰粙鐛幇顓熷劅闁挎繂娲ㄩ弳銈嗙節绾版ɑ顫婇柛瀣嚇閹嫰顢涘☉妤冪畾闂佸湱铏庨崰鏍不濞戙垺鐓熸俊銈傚亾闁绘鍟撮獮蹇撁洪鍛嫼闂侀潻瀵岄崣搴ㄦ倿妤ｅ啯鍊垫繛鎴炲笚濞呭洤顭跨憴鍕妤犵偞鐗楅幏鍛村捶椤撶偛绠炴繝鐢靛Х閺佸憡鎱ㄩ幘顔肩柈闁规鍠氭稉宥夋煙閻愵剛鏆樺ù婊勭矒閺岋繝宕堕張鐢垫晼缂備礁顦介崰姘跺焵椤掍緡鍟忛柛鐘崇墵閳ワ箓鏌ㄧ€ｂ晝绠氶梺缁樺灱濡嫬鏁柣鐔哥矋缁挸鐣烽鐐茬倞鐟滃寮ㄦ禒瀣厽闁归偊鍨伴悡鎰喐閹跺﹤鎳愮壕濂告煠閼规澘鐓愭い銉ヮ樀閺屻劑寮村Ο铏逛紙闂佸搫鑻Λ妤咁敇婵傜宸濇い鎾跺仒閹?
    {
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存鐐达耿閹崇娀顢楁径瀣撴粓姊绘担瑙勫仩闁告柨绉堕幑銏ゅ礃椤斿槈锕傛煕閺囥劌鐏犻柛鎰ㄥ亾婵＄偑鍊栭崝锕€顭块埀顒勬煛婢舵ê寮慨濠冩そ閺屽懘鎮欓懠璺侯伃婵犫拃灞芥珝闁哄本鐩幃銏ゅ川婵犲嫮鈻忛梻浣风串缁插潡宕楀Ο璁崇箚婵繂鐭堝Σ鐑芥⒑缁嬫鍎愰柟鍛婃倐閸┿儲寰勬繛鐐€哄銈嗘寙閸屾粌鍤紓鍌欑閸婂摜绮旈棃娑辩劷闊洦绋戞儫閻熸粌绻橀幆渚€宕奸妷锔规嫼濠殿喚鎳撳ú銈夋倶閸欏绠惧ù锝呭暱閸熶即骞楅妷鈺傗拻闁稿本鐟ㄩ崗灞俱亜椤撶偟澧︽い銏＄墵瀹曘劑顢橀崶銊р槈闁宠鍨归埀顒婄秵娴滅偤鎮?+ 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐烘偡濠婂啰绠荤€殿喗濞婇弫鍐磼濞戞艾骞堟俊鐐€ら崢浠嬪垂閸偆顩叉繝闈涱儐閻撴洘绻涢崱妤冪缂佺姴顭烽弻鈥崇暆閳ь剟宕伴弽顓犲祦闁糕剝绋戠痪褔鏌涢悢宄版殲闁圭懓娲璇测槈閵忕姈褔鏌涘☉鍗炲季濞寸厧鍟村娲传閸曨剚鎷遍梺鐑╂櫓閸ㄥ爼濡存笟鈧鎾閳╁啯鐝抽梻浣虹《閸撴繈鎮锋潏鈺傤潟闁冲搫鎳忛埛鎺戙€掑锝呬壕闂侀€炲苯澧伴柛瀣洴閹崇喖顢涘☉娆愮彿闂佸憡鎸嗛崨顖ょ床闂備胶绮…鍫濃枍閺囩姳鐒婃い鎾卞灪閻撶喖鏌ㄥ┑鍡╂Ч闁轰浇浜槐鎺撴姜閹殿喚鐓撻悗瑙勬磸閸斿秶鎹㈠┑瀣闁冲搫鍟粈渚€姊婚崒姘偓椋庣矆娓氣偓楠炲鏁撻悩鑼槶濡炪倖甯掔€氼剛绮堢€ｎ偁浜滈柟鎹愭硾閺嬫梻绱掔€ｂ晝绐旀慨濠傤煼瀹曟帒顫濋钘変壕闁归棿绀佺壕鐟邦渻鐎ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鐘栄囨煕閳╁啨浠︾紒顔瑰墲娣囧﹪鎮欓鍕ㄥ亾閺嶎厼绀夌憸蹇曞垝婵犳艾绠ｉ柣妯烘▕濡粓姊虹粔鍡楀濞堟梻绱掗悪娆忔处閻撴洘銇勯幇鍓佹偧妞わ絽寮剁换娑㈠川椤撶喎鏋犻梺鍝勭焿缂嶄線銆佸鈧幃娆撳箹椤撶噦绱楅梻鍌欑閹碱偊寮甸鍕剮妞ゆ牜鍋涢弰銉╂煕閺囥劌鐏犵紒鈧崘鈹夸簻闊洤娴烽ˇ锔姐亜椤愩垻鐒告慨濠勭帛閹峰懘鎼归悷鎵偧闂備浇顫夐悺鏇㈩敋瑜忛崣鍛存⒑缂佹ɑ鈷掗柛妯犲洦瀚呴柣鏃囥€€閸嬫捇鐛崹顔煎濡炪倧瀵岄崹鍫曞箖閳ユ枼妲堟俊顖氱箰缂嶅﹪寮幇鏉垮窛妞ゆ柨鍚嬪▓姗€姊绘担绛嬪殭缂佺粯鍨块獮鎰板箹娴ｅ摜鍘撮梺纭呮彧闂勫嫰宕愰悜鑺ョ厾缁炬澘宕晶顔姐亜閵堝倸浜炬繝纰夌磿閸嬫垿宕愰妶澶婂偍闁绘挸绡冨ú顏嶆晣闁靛繒濮烽悾楣冩⒑闁偛鑻晶顖炴煏閸パ冾伂缂佺姵鐩獮妯兼崉娓氼垱姣囩紓鍌氬€烽懗鑸垫叏閻㈠憡鍋傞柨鐔哄Т閽冪喖鏌ｉ弮鍌ょ劸妞ゆ洟浜堕幃妤€鈽夊▍铏灴閺屻劑顢橀悙鈺傛杸闂佺粯鍔樼亸娆愭櫠闁秵鐓曟繛鍡楃箰閺嗘瑦銇勯銏㈢闁圭厧缍婂畷妤佸緞婵犱礁顥氶梻渚€娼荤€靛矂宕㈤崗鑲╁ⅰ濠碉紕鍋戦崐鎴﹀磿闁单鍥ㄥ閺夋嚦锕傛煕閺囥劌鐏￠柡鍛矒閺岋綁骞囬棃娑橆潽濠殿噯绲介悧鎾愁潖閾忓湱纾兼俊顖氭贡缁讳線姊洪崫銉バｉ柣妤冨█閺佹劙鎮欏顔惧弳闂佸壊鍋呯换鍕礉閿曗偓椤啴濡堕崱妤€娼戦梺绋款儐閹瑰洭寮婚敐澶嬫櫜闁搞儜鍐ㄧ闂備浇妗ㄩ悞锕傚箲閸ヮ剙绠栭柍鍝勬噹缁€鍐╃箾閺夋埈鍎忓ù婊堢畺閺岋綁骞嬮敐鍛呮捇鏌ｉ幘鍐叉殻鐎殿喖鐖奸幃娆撴濞戣鲸姣夋俊鐐€х粻鎴﹀床閺屻儮鈧妇鎹勯妸锕€鏋傞梺鍛婃处閸撴盯藝閵夆晜鈷戦悗鍦濞兼劙鏌涢妸銉﹀仴妤犵偛鍟埢搴ㄥ箻瀹曞洭鐛撻梻浣烘嚀椤曨厽鎱ㄧ€涙ɑ鍙忛柛蹇氬亹缁♀偓闂佹眹鍨藉褎绂掕閺屾稑螣閻樻彃鏋ら柣鎺嶇矙閺屻劑鎮㈤崫鍕戙垽鏌￠崟鈺佸姦闁哄苯绉烽¨渚€鏌涢幘鏉戝摵濠碉紕鏁诲畷鐔碱敍濮橀硸鍚呴梻浣瑰缁诲倸螞濞戙垹违濠电姴瀚壕钘壝归敐鍥ㄥ殌濠殿喖鐗婇幈銊︾節閸屾粎楔闂侀潧妫楅崯瀛樹繆閻戣棄鐓涢柛灞惧焹閸嬫挻顦版惔妯轰壕闁荤喐婢橀ˉ蹇涙煕閻樺磭澧电€规洘绮撳畷濂稿即閻斿弶瀚奸梺鍝勵槺閸嬬偞绔熼崱娴板洭鏁冮崒娑氬幈婵犵數濮撮崐褰掑磻閵夛富娈介柣鎰嚋闊剟鏌熼鐣屾噰鐎规洦鍋婂畷鐔告償濞戞ê顫囬梺鍝勮閸旀垵顕ｉ鈧崺鈧い鎺戝绾惧潡鏌涢幇顒佲枙闁搞倕鐗撻弻锝夊箣閻戝棛鍔风紓浣叉閸嬫捇姊绘担鍦菇闁搞劏妫勯…鍥槼闁绘瀛╂穱濠囨倷椤忓嫧鍋撻弽褜娼栫憸鐗堝笚閸嬪鏌ｅΟ鍨毢闁哄棴绠撻弻鈩冨緞鎼搭喖娈鹃梺鍝勫暙閻楀棗顔忓┑鍥ヤ簻闁哄啫娲よ闂佺粯绻冮悧鐘差潖缂佹ɑ濯寸紒瀣濮ｆ劙姊洪崷顓涙嫛闁稿锕悰顔界節閸愨晛鍔呴梺鎸庣箓閹峰螞濠婂嫮绡€闁汇垽娼ф牎缂佺偓婢樼粔褰掑箖閿熺姴鍗抽柕蹇ョ磿閸樻捇姊虹€圭姵銆冪紒鈧笟鈧鎶藉即閻樼數锛滈梺鍓插亝濞叉垿骞夋ィ鍐╃厸閻忕偟顭堟晶鏌ユ煙瀹勭増鍣界紒顔界懄閹棃骞橀崗鍛棜闂備礁澹婇悡鍫ュ闯椤旂⒈鐒介柡宥冨妿缁犲墽鈧懓澹婇崰鏇㈠箟妤ｅ啯鐓涘ù锝呮啞椤ャ垽鏌＄仦鐣屝ユい褌绶氶弻娑㈠箻鐎垫悶鈧帡鏌涢幒鎾虫诞鐎殿噮鍣ｅ畷濂告偄閾氬倻绱︾紓鍌氬€风粈渚€顢栭崼婵冩灃闁哄洢鍨洪弲顒佺箾閹存瑥鐏柍閿嬪灩缁辨挻鎷呴崗澶嬶紙濡炪倕绻愬Λ妤吽夊顓滀簻闁规崘娉涘暩婵炴垶鎸哥粔纾嬬亙闂佹寧绻傞幊搴ㄥ汲閻愮儤鐓熼柟鎯ь嚟閹冲洭鏌熼绛嬫疁闁诡喛妫勯埢鎾舵崉閵娿儱歇闂傚倷绀侀幉锟犲箲閸ャ劎顩查柛顐ｆ礀閽冪喖鏌ㄥ┑鍡╂Ц缂佺姾娅曟穱濠囧Χ閸涱厽娈堕梺鍝勬－閸嬪﹤顫忛搹瑙勫珰闁炽儴娅曢悘宥咁渻閵堝啫濡奸柟铏耿閺佹劙鎮欏ù瀣杸闁诲函绲介悘姘跺疾濠靛鈷戦梻鍫熺〒缁犲啿鈹戦鎯у幋濠碘€崇埣椤㈡岸鍩€椤掆偓椤繒绱掑Ο璇差€撻梺鍛婄☉閿曘儵宕曢幘缁樷拺闁告繂瀚晶閬嶆煕閹惧鎳呴柛娆忔嚇濮婅櫣绱掑Ο娲绘濡炪們鍎查幑鍥ь嚕閹惰姤鏅濋柛灞剧〒閸橀潧顪冮妶鍡欏⒈闁稿绋掔粋宥夋偋閸粎绠氱紓鍌欑贰濠⑩偓闂侇収鍨遍幈銊︾節閸曨厼绗＄紓浣虹帛缁诲啴骞嗛弮鍫濈伋闁哄倶鍎茬€氫粙姊婚崒娆戭槮闁硅绻濋獮鎰嫚閼碱剚娈曢梺褰掓？閼宠泛鐣垫笟鈧弻娑㈩敃閻樻彃濮庨柟顖滃枛濮婃椽骞愭惔銏㈠弳婵犫拃鍌滅煓闁诡垯绶氬畷濂稿Ψ閿旇瀚奸梻浣告啞缁嬫垿鏁冮敃鍌涘剹闁糕剝绋掗悡鐔兼煙閾忕懓浠滄い鈺婂墰缁辨帗娼忛埡浣锋閻庤娲栭妶鍛婁繆閻戣姤鏅濋柍褜鍓熼崺鈧い鎺戝€归弳顒勬煛鐏炲墽娲撮柍銉畵楠炲鈹戦崨顖氬毈缂傚倸鍊峰ù鍥春閺嶎厽鍋傞柨鐔哄Т缁犳牗淇婇妶鍌氫壕濡炪値鍋呯换鍫ャ€佸鈧幃銏ゅ传閸曨偅顓奸梻鍌氬€峰鎺旀椤旀儳绶ら柛褎顨呯粈鍌涙叏濡炶浜鹃梺闈涙缁舵艾顕ｉ幘顔碱潊闁斥晛鍟悵鏍⒒娓氣偓閳ь剛鍋涢懟顖涙櫠椤曗偓閺屸剝绗熼崶褎鐝濋梺鍝勭焿缂嶄線骞冮姀銈呬紶闁靛／鍛笌缂傚倸鍊烽懗鍓佸垝椤栫偛绀夋繛鍡樻尭缁犳牕螖閿濆懎鏆為柛濠傤煼閺岋箑螣娓氼垱鈻堥梺鍝ュ仜閻栫厧顫忓ú顏勪紶闁告洦鍓欑粣娑㈡⒑閸濄儱鏋庢繛纭风節閻涱喛绠涢幘浣规そ椤㈡棃宕ㄩ鍛伖闂傚倷鑳剁划顖炲箰閸濄儮鍋撻崹顐㈢瑨妞ゎ厼娲幃銏ゅ礂閼测晛骞堝┑鐘垫暩婵挳宕婊呯焼闁告劦浜炵壕濂告煕鐏炲墽鈽夋繛鎼櫍閺屸剝鎷呯粙鎸庢閻庤娲樼敮鎺楀煡婢跺娼╂い鎺嗗亾缂佺姷鍋ゅ濠氬磼濞嗘帒鍘￠梺纭呮珪閹瑰洭骞冨ú顏勎╃憸搴＄暦閺屻儲鐓曢柡鍥ュ妼楠炴﹢鏌ｉ鐔稿磳闁哄睙鍡欑杸闁规儳鍟挎潏鍛存⒑缁嬫鍎愰柟鎼佺畺楠炲骞橀鑲╊槹濡炪倖甯掗崑鍡椢ｆ导瀛樷拻濞达綀顫夐崑鐘绘煕閺傝法绠伴柍缁樻煥閳藉濮€閳哄倹娅旈梻渚€娼чˇ顐﹀疾濞戞氨鐭嗛悗锝庡亖娴滄粓鏌熼崫鍕ゆい锔肩畵閺屾盯寮幐搴℃闂佸疇顫夐崹鍧楀箖濞嗘挸绾ч柟瀵稿С濡楁挻淇婇悙顏勨偓鎴﹀礉鐏炶娇娑樷攽鐎ｎ亞鐣鹃梺鍓插亾缁插潡鎮块埀顒€鈹戦悙鏉戠仸闁荤啙鍥у偍濞寸姴顑嗛悡娆撴煠閸︻厼顣肩憸鎶婂懐纾奸弶鍫涘妽缁€瀣煙椤曗偓缁犳牠銆佸鈧慨鈧柨娑樺楠炲牓姊洪悷鏉挎倯闁伙綆浜畷婵堚偓锝庡枛缁犵喎螖閿濆懎鏆為悗鐢靛У缁绘繈妫冨☉娆忓亶婵炲瓨绮嶇划鎾诲蓟閳ユ剚鍚嬮柛鎰╁妼椤姊哄ú璇插箹妞ゆ垵顦靛璇测槈濠婂懐鏉搁梺鍝勫暙閸婂鎮℃径鎰拺闁告繂瀚€氭壆绱掓径灞藉幋缁℃挸銆掑锝呬壕闂佸搫鏈惄顖炵嵁濡吋宕夐柣鎴烆焽閳ь剝顕ч—鍐Χ鎼粹€茬盎缂備胶绮崝妤呭矗閸涱収娓婚柕鍫濇噽缁犱即鎮楀闂寸敖婵″弶鍔欏畷濂稿即閻斿弶瀚奸梻浣告啞缁嬫垿鏁冮妷锕€绶為柛鏇ㄥ灡閻撴洟鏌熼幆褜鍤熼柟鍐叉噽缁辨帡顢氶崱娆忓煂闂佸疇妫勯ˇ鍨叏閳ь剟鏌嶉柨顖氫壕闂佸綊鏀卞钘夘潖濞差亜宸濆┑鐘插缁变即姊虹粙鍧楊€楃€规洦鍓熼幃娲敇閵忊檧鎷婚梺绋挎湰閻熝囁囬敃鍌氱閻庢稒顭囬惌宀€绱掓潏鈺佷沪缂佹鍠栭崺鈧い鎺戝閸嬫ɑ銇勯弴妤€浜鹃悗瑙勬礃閿曘垽銆佸▎鎾村癄濠㈣泛妫楁禍楣冩煟閹达絽袚闁稿﹦鏁诲濠氬醇閻旇　濮囬梺鐟板级閹倿寮婚敐澶嬫櫇闁逞屽墴閹囧幢濞戞鍘撮梺纭呮彧鐎靛矂寮繝鍥ㄧ厸闁稿本锚閳ь剚鎸惧▎銏ゆ偨閸涘ň鎷洪梺鍛婄☉椤參鎳撶捄銊х＜闁绘ê纾晶顏堟煟閿濆洤鍘村┑顔瑰亾闂侀潧鐗嗛幊鎰邦敊閹邦厾绠鹃弶鍫濆⒔閸掍即鏌涘Ο鑽ょ煉妤犵偛绻橀幖褰掑捶椤撶媴绱冲┑鐐舵彧缁茶姤绔熸繝鍥х？闁瑰墽绮悡鏇㈠箹缁顫婇柣鎾炽偢閺屻劌鈹戦崼婵呮闂佽桨绀侀崯鏉戠暦閹烘垟妲堟慨姗嗗墮缁犵偞绻濋悽闈浶ラ柡浣告啞閹便劑骞橀鑲╂焾濡炪倖鐗楃划搴ㄦ儗閸℃稒鐓冪憸婊堝礈濞嗘挴鈧棃宕橀鍢壯囨煕閳╁喚鐒介柍瑙勭〒缁辨挻鎷呴悷鎵シ濠电偟銆嬬换婵嬫偘椤曗偓楠炲鏁冮埀顒傜矆鐎ｎ偁浜滈柡宥庣厛閸ゃ倝鏌嶉崫鍕殲闁哥姵鍔欓悡顐﹀炊閵娧€濮囬梺鍝勬媼閸撴氨鎹㈠┑鍥╃瘈闁稿本绮岄·鈧梻浣虹帛閹歌崵绮欓幋锔光偓锔炬崉閵婏箑纾梺缁樼濞兼瑦鎱ㄥ☉娆戠瘈闁冲皝鍋撻柛鏇ㄥ亞娴煎矂姊洪崫鍕拱闁烩晩鍨伴锝夘敆閸曨偆顔囬柟鍏肩暘閸ㄥ綊姊婚婊呯＝闁稿本鐟х拹浼存煕閻樺磭澧甸柍銉畵閹粓鎸婃径宀€鏆繝鐢靛仜濡瑩骞愭繝姘９缂備焦眉缁诲棙銇勯弽銊х煀閻㈩垵鍩栭〃銉╂倷閼碱儷褎鎱ㄦ繝鍕笡闁瑰嘲鎳橀幃鐑藉级閸啩鎴炵節閻㈤潧浠滈柣掳鍔岄悾婵堢矙鐠恒劍娈鹃梺纭呮彧缁犳垵顔忓┑鍥ヤ簻闁哄秲鍔庨幊鍛存煕閹惧崬濮傛慨濠傤煼瀹曟帒顫濋崡鐑嗘澑闂備礁鍚嬪姗€鍩€椤掑倸鏋傚ù婊冪秺閺岀喖鎮滃Ο璇茬婵炲瓨绮岄妶绋款潖濞差亜宸濆┑鐘辫兌缁讳線姊洪幖鐐插濠电偛锕ら锝夊箮閽樺）褍顭跨捄渚剬闁圭柉娅ｇ槐鎺旂磼閵忕姴绠归梺鐟板暱缁绘﹢骞冮悽鍓叉晜闁割偆鍠撻崢钘夆攽鎺抽崐鏇㈠疮椤愶箑鐓濋柛顐熸噰閸嬫挾鎲撮崟顒傤槰濠碉紕鍋樼划娆忣嚕椤愶箑绠荤紓浣姑禍褰掓⒑閼测斁鎷￠柛鎿勭畱鍗卞ù鐓庣摠閳锋帒霉閿濆懏鎲搁柡瀣暞娣囧﹤顪冮悡搴☆潎闂佽鍠氶崑銈夊极閸愵喖纾奸柕鍫濇濞?All rights reserved"
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬婊€绨婚梺鍝勫暙閸婂綊宕甸埀顒佺箾鐎涙鐭掔紒鐘崇墵瀵鈽夐姀鐘电杸闂傚倸鐗婄粙鎺楁倶閸惊鏃堟偐闂堟稐鏉梺鎼炲妿閸庛倗绮氭潏銊х瘈闁搞儯鍔岄埀顒傚厴閹鈽夊▎妯煎姺濡炪倧璐熼崕闈涱潖濞差亜鎹舵い鎾寸⊕鐎氭盯姊虹粙娆惧剰闁挎洦浜畷瑙勩偅閸愨晜娅囬梺绋挎湰椤曢亶鍩￠崨顔惧帗闁哄鍋炴竟鍡浰囬敂鍓х＜闁归偊鍘鹃埥澶嬨亜椤忓嫬鏆ｅ┑鈥崇埣瀹曘劑顢涘搴℃暪闂備浇顕х€涒晠宕欑憴鍕洸婵犻潧鐗忛崡姘舵煟閺冨倸甯堕柛妤佸▕閺岋綁骞囬妸锔界彆闂佸搫鍊甸崑鎾斥攽閿涘嫬浜奸柛濠冪墪椤繑绻濆顓炰槐闂佸搫鍟悧濠囧磹閸啔褰掓偐瀹割喖鍓遍梺缁樻尰濞叉ɑ绌辨繝鍥舵晬婵犲﹤鍞姀銈嗙厱闁绘ê鍟块崫铏光偓瑙勬礈閺佽顕ｉ幘顔藉亜缂傚牏濮烽惄搴ㄦ⒒娴ｅ憡鎯堥柛鐕佸亰瀹曟劖绻濆顒傤啈闂佺鏈粙鎾剁不妤ｅ啯鐓曟い鎰剁悼閻矂鏌ｉ鐔烘噰闁哄备鈧磭鏆嗛悗锝庡墰琚︽俊銈囧Х閸嬫稑煤椤擃潿鈧礁螖娴ｇ懓顎撻梺鍛婄懃椤﹁鲸绂嶉悙鐑樷拻闁稿本鐟ㄩ崗灞俱亜椤撶偟澧㈤柍褜鍓氱粙鍫ュ疾濞戭澁缍栭煫鍥ㄥ搸娴滃綊鏌熼崫鍕靛剬闁稿鎹囧畷姗€顢欓崲澶堝姂閺屽秹宕崟顐熷亾閹间礁鐒垫い鎺戝€搁崢鎾煛鐏炲墽鈽夐柍璇叉唉缁犳盯鏁愰崰甯畵濮婃椽宕崟闈涘壋婵犳鍠楅幐鎶藉灳閺冨牆绀冩い鏂挎瑜旈弻娑㈠箛閳轰礁顬嬫繛瀵稿█缁犳牕顫忓ú顏呭€婚柍鍝勫€归悵锕傛⒑閹肩偛濮€婵炲鐩、姘舵晲婢舵ɑ鏅濋梺鎸庢磵閸嬫挾绱掗悩宕囨创闁哄备鍓濆鍕節閸曨亣妾搁梻浣稿悑濠㈡﹢鎮樺┑瀣厴闁硅揪绠戦悡锟犳煕閳╁喚娈樺ù鐙€鍨崇槐鎾寸瑹閸パ勭彯闂佹悶鍔岄崯鍧楀煝鎼淬劌顫呴柕鍫濆閺呮繈姊洪崘鑼邯闁哄懏绮撳顒勫焵椤掑嫭鈷掑ù锝堟鐢盯鏌ㄥ鑸电厽闊洦鏌ㄩ崫铏光偓娈垮枟婵炲﹪宕洪敓鐘插窛妞ゆ梹鍎抽獮鍫ユ⒑鐠囨彃鍤辩紓宥呮瀹曟粌鈻庨幇顕呮祫闁哄鐗冮弬渚€宕戦幘鑸靛枂闁告洦鍓涢ˇ銉х磽閸屾氨孝闁挎洏鍊濋幃楣冩煥鐎ｎ剟妾紓浣割儏閻忔繂鐣甸崱娑欌拺缂備焦锚婵牊銇勯弮鈧悧鐘茬暦椤掑嫬閱囬柡鍥╁暱閹风粯绻涙潏鍓у埌闁硅绻濆畷顖炴倷閻㈢數锛滈梺鍛婄箓鐎氼參宕戦妷褉鍋撶憴鍕婵炶尙鍠栭悰顔芥償閵婏箑娈熼梺闈涱槶閸庤鲸绔熼幒妤佲拻濞达絿鍎ら崵鈧繝鈷€鍛珪闁告帗甯￠、娑㈡倷閺夋垳绨垫繝鐢靛仜濡瑩宕归崷顓烆棜闁芥ê顥㈣ぐ鎺撴櫜濠㈣泛顑嗛柨顓㈡⒑鐠囪尙绠氶柡鍛矌閹广垹鈽夐姀鐘茬獩濡炪倖娲栧Λ婊堝焵椤掆偓濞硷繝寮婚敍鍕勃缂佸鐏濋埛澶愭⒑閻熸澘妲婚柟铏灋闁告劦鍠栭悡娑㈡煕閺囥劌浜濋柛搴㈡崌濮婂宕掑▎鎴М闂佸湱鈷堥崑濠囧箚婢跺闄勭紒瀣硶閻涖儵姊洪崫鍕枆闁告鍋愮划濠氬冀椤愶絽鏋戦梺鍝勫€藉▔鏇㈠汲閿曗偓閳规垿宕掑顓炴殘闂佺顑呯€氫即寮诲☉妯锋斀闁糕剝顨忔禒鐓庘攽閻愯尙澧戦柛搴ㄦ涧椤繑绻濆顒傦紲濠电偛妫欓崺鍫澪ｉ鈧铏规兜閸滀礁娈濈紓浣虹帛缁诲牓鐛崘顔藉€婚柦妯猴級閵娾晜鐓冮柛婵嗗閳ь剛鎳撻埢宥夊炊瑜滃〒濠氭煏閸繃鍣界紒鐘卞嵆閺岋絽螖閳ь剙螞濠靛绠柟杈鹃檮閸嬪嫰鏌涜箛姘汗闁告ü绮欏娲传閸曨偅娈梺绋匡工閹芥粓寮鈧畷婊勬媴閾忕櫢绱查梻浣虹帛閿氱痪缁㈠弮瀹曪綁鍩€椤掑嫭鈷戦柛婵嗗閻忛亶鏌涢悩鍐插闁逞屽厸缁插宕戦幇鏉跨劦妞ゆ帊鑳堕埊鏇㈡嫅闁秵鈷戞繛鍡樺劤楠炴牠鏌嶇憴鍕伌闁诡喗鐟╅幊鐘活敆閳ь剟路閳ь剟姊绘担鍛婂暈婵﹦鎳撶叅闁绘梻鍘ч弰銉╂煟閹邦剚鎯堢紒鐙呯秮閺屻劑寮崶顭戞闂侀€炲苯澧伴柛蹇斆～蹇旂節濮橆剟鍞堕梺缁樻閵嗏偓婵℃彃鐗婄换娑氣偓鐢殿焾鏍￠梺鎸庢磸閸ㄥ綊锝炶箛娑欐優闁革富鍘鹃敍婊冣攽閳藉棗鐏ｉ柛妯犲洤鐒荤憸鐗堝笚閳锋垿姊婚崼鐔恒€掑褎鐩弻鐔碱敊閻撳簶鍋撻崸妤€绠栧ù鍏兼儗閺佸鏌嶈閸撴瑩鎮惧畡閭︽建闁逞屽墴閵嗕礁螣娓氼垳鍙嗛梺閫炲苯澧弫鍫熶繆閵堝懏鍣洪柍閿嬪灴閺屾盯鏁傜拠鎻掔闂佸憡鏌ㄩ澶愬蓟濞戙垹惟闁靛／鍛呫劑鎮楃憴鍕闁告梹鐟╅獮鍐ㄢ枎閹炬潙鈧粯淇婇婊冨付妤犵偞顨婂缁樻媴閾忕懓绗″銈庡幖閻楀棝鎮鹃悜鑺ュ仺闁割煈鍋掑鐔兼⒑缂佹﹩娈旈柣妤€绻掔划缁樸偅閸愨晝鍘卞┑鐘诧工閸燁偊寮抽悢铏圭＜閻庯綆鍘奸崥鍦磼鏉堛劌娴€规洘甯掗埥澶娢熼弰蹇曟闂佽瀛╅鏍窗閺嶎厽鍋夊┑鍌氭憸瀹撲線鎮楅敐搴℃珮闁轰礁娲弻锝夊籍閸偅顥栧┑陇顕滅紞浣割潖婵犳艾纾兼繛鍡樺灩閻涖垹鈹戦悙鏉垮皟闁搞儯鍔屾禍閬嶆⒑缁洖澧查柕鍥у€搁埥澶娢熼柨瀣偓濠氭⒑閻熸壆鎽犵紒璇茬Т铻炴繝濠傚缁♀偓闂佹眹鍨藉褎鐗庣紓浣哄亾濠㈡绮旇ぐ鎺戠畺闁跨喓濮撮崡鎶芥煟閺傚灝顣崇紒鐘冲哺濮婃椽妫冨☉姘暫闁诲孩鍑归崳锝堟闂佹寧娲栭崐褰掓偂閸愵亞纾藉ù锝堝亗閹寸姷鐭嗛悗锝庡枟閻?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊绋掑婵堢磼閳锯偓閸嬫捇姊绘担渚劸闁哄牜鍓涢崚鎺戠暆閸旇偐鍏橀崺鈧い鎺戝閳锋帒霉閿濆嫯顒熼柣鎺斿亾閵囧嫰骞嬮悙鑼患闂佺懓绠嶉崹褰掑煘閹寸姭鍋撻敐搴濇捣闁硅姤娲熷娲传閸曨剙鍋嶉梺鎼炲妼缂嶅﹪寮荤€ｎ喖鐐婇柕濞у懐妲囬梻浣稿閻撳牓宕戦崟顒佸弿闁割偁鍎遍弸渚€鏌涢鐘插姕闁抽攱甯掗湁闁挎繂鐗婇鐘绘偨椤栨稓娲撮柡宀€鍠栧畷銊︾節閸愩劉鍋撻幇顔瑰亾鐟欏嫭绀冮柛鏃€鐟ラ悾鐑芥倻缁涘鏅ｉ梺缁樻煥閸㈣尙鑺遍妷锔剧瘈闁汇垽娼ч埢鍫熺箾娴ｅ啿鍘惧ú顏勎╃憸宥嗘叏椤掑嫭鐓冪憸婊堝礈閻斿娼栭柧蹇撴贡閻瑩鏌熺粙鍨劉闁圭兘浜跺娲川婵犲倻浼囧銈庡亜椤﹁京鍒掔€ｎ喖绠抽柡鍌氭惈娴滅偓绻涢幋鐐垫噽闁绘帞鏅槐鎺楁偐閼姐倗鏆梺璇″枟椤ㄥ懘鍩ユ径濞炬瀻婵☆垳鍘ф慨娲⒒娴ｈ銇熼柛妯恒偢椤㈡牠宕堕埡鍌ゆ綗闂佸湱鍎ら〃蹇涘极婵犲洦鐓犻柟顓熷笒閸旀氨鐥悙顒€鈻曢柟顔筋殔閳绘捇宕归鐣屼粚婵＄偑鍊栧▔锕傚炊瑜忛崣鈧┑鐘灱閸╂牠宕濋弴顫稏闁告稑鐡ㄩ悡鍐煃鏉炴壆顦﹂柡鍡欏仱閺岋絽鈹戦崶銊ь槹濠殿喖锕ュ浠嬪箠閿熺姴围闁告稑鎳岄崹铏规崲濞戞碍瀚氱憸蹇涙偩閹惰姤鐓忛柛銉ｅ妼婵秶鈧娲橀敃銏ゃ€侀弮鍫濆耿闁哄洨濮烽幉褰掓⒒閸屾瑧鍔嶉柛搴＄－閹广垽骞囬濠呪偓鍧楀级閸碍娅呮い銉﹀哺閺岋箑螣娓氼垱锛嗛悷婊呭鐢寮查弻銉︾厱闁靛绲芥俊鍏肩箾閸繃婀扮紒缁樼箓閳绘捇宕归鐣屼邯闂備胶绮〃鍛存晝椤忓懍绻嗛柣銏㈩焾缁€瀣亜閺嶎煈鍤ら柍鍝勬噺閻撳繐顭块懜鐢点€掗柣锝堥哺缁绘盯宕奸悤浣圭杹濠殿喖锕ㄥ▍锝夊礌閺嶎厼鍗抽柣鏂挎啞缁额參姊绘担渚劸閻庢稈鏅為妵鎰板礃椤曞懏鏅梺闈涱槴閺呮稓绮堢€ｎ偁浜滈柡宥冨妽閻ㄦ垿鏌ｉ妶鍌氫壕婵犵绱曢崑鎴﹀磹閺嶎偅鏆滈柟鐑橆殔绾惧鏌涢埄鍐槈闁汇値鍣ｉ幃妤呮晲鎼粹剝鐏嶉梺缁樻尵閸犳牠鐛弽顬ュ酣顢楅埀顒勫焵椤戞儳鈧洟鈥﹂崶顒€绠涙い鎾跺Х椤旀洟姊洪崨濠勬噧妞わ箒椴搁弲鍫曨敂閸喓鍘撶紓鍌欑劍钃辨い銉ョ墦閺屸剝鎷呯粙鎸庢閻庤娲樼敮鎺楀煡婢跺ň鏋庨柟閭﹀枛缁€渚€姊婚崒姘偓鐑芥嚄閸撲焦鍏滈柛顐ｆ礀缁€鍫熺節闂堟稒鐏╅柛婵嗗閻撱儵鏌涘☉鍗炵仧缂佹顦埞鎴︻敊缁涘鍔搁梺绯曟櫆閻楃娀宕哄☉銏犵闁挎梻鏅崢鎯р攽閻愬瓨缍戞い鎴濇瀵儼銇愰幒鎾跺幍闂佸憡鍨崐妤冨姬閳ь剟姊洪崫鍕拱闁烩晩鍨堕悰顔尖枎閹惧磭顦板銈嗘尵婵潧鈻撴總鍛娾拻濞达絿鐡旈崵鍐煕閻樺啿鍝虹€规洘鍨挎俊鑸靛緞婵犲嫮鏆繝鐢靛仜濡瑩骞栭埡鍛瀬闁告洦鍨遍悡鏇熺箾閸℃孝闁告柣鍊濋弻锝夊煛娓氣偓閸欏嫭鎱ㄦ繝鍕笡闁瑰嘲鎳樺畷銊︾節閸愩劌澹嶅┑鐘垫暩閸嬬偛顭囧▎鎾崇９闁革富鍘鹃惌澶屸偓骞垮劚椤︿即宕愰悜鑺ョ厽闁靛繈鍨洪弳鈺呮煟閹炬剚鍎忔い顏勫暣婵″爼宕卞Ο纭风穿闂備胶顭堥鍡涘箲閸ヮ剙钃熼柨娑樺濞岊亪鏌涢幘妤€瀚崹閬嶆⒒娴ｈ櫣銆婇柡鍛箞瀹曟垿濡舵径灞界ウ闂佺硶鍓濋崙鐟拔ｉ悜妯肩瘈闁靛骏缍嗗鎰箾閸欏澧甸柛鈺冨仱楠炲鏁傞懞銉︾彨闂備礁鎲″ú锕傚磻閸岀偛绾фい鎾卞灪閳锋垹绱掔€ｎ亜鐨￠柡鈧繝姘厽婵°倓鐒︾粈瀣亜閵忥紕鎳嗙紒鐘崇洴瀵噣宕掑鍛潓濠电姵顔栭崰妤呮晝閳哄懎绀堟繛鎴欏灪閺咁剚绻涢幋娆忕仾闁抽攱鍨块弻锝夋偄閸涘﹦鍑￠梺鎶芥敱濮婅崵妲愰幒妤婃晪闁告侗鍘炬禒鎼佹⒑鐠団€虫珯缂佺粯绻傞悾宄邦潨閳ь剟銆佸▎鎾村仼閻忕偠妫勭粻娲⒒閸屾瑨鍏岀紒顕呭灦瀵濡搁埡浣虹枀闂佹寧绋戠€氼參顢曟禒瀣厸閻忕偠顕ч崝婊堟煟閹惧娲撮柡灞剧洴閸┿儵宕卞Δ鈧粻鑽ょ磽娴ｅ壊鍎戠紒顕呭灦婵＄敻宕熼姣尖晠鏌ㄥ┑鍡樺櫣濠殿噯绠撳铏规嫚閳ヨ櫕鐏嶉梺鑽ゅ暱閺呮盯鎮鹃悜鑺ユ櫜闁糕剝顨堢粣鐐寸節閻㈤潧孝闁哥喓澧楃粋宥呯暋閹佃櫕鏂€闂佹寧绋戠€氼剚绂嶆總鍛婄厱濠电姴鍟悘瀵糕偓瑙勬礈閹虫挾鍙呭銈呯箰閹冲孩绂掗銏♀拺闁告劕寮堕幆鍫ユ煙閸愬弶鎹ｇ紒顔碱煼瀵粙顢橀悢鍝勫箞闂備礁婀遍崑鎾汇€冮崨鏉戠９鐟滅増甯掔粈瀣煕閹伴潧鏋熼柍閿嬪灩缁辨挻鎷呮慨鎴簼閹便劑鏁冮崒娑氬幈闂佺懓鐏濈€氼噣鎮鹃悽纰樺亾?闂?
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
            // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐差渻閵堝棗绗掓い锔垮嵆瀵煡顢旈崼鐔蜂画濠电姴锕ら崯鐗堟櫏婵犵數濮崑鎾炽€掑锝呬壕濠殿喖锕ㄥ▍锝囨閹烘嚦鐔兼惞闁稓绀冨┑鐘殿暯濡插懘宕戦崟顓涘亾濮橆厽绶叉い鏇秮瀹曘劍绻濋崟顓熷殞闂備線鈧偛鑻晶瀵糕偓娈垮枛椤兘骞冮姀銈呯閻忓繑鐗楃€氫粙姊虹拠鏌ュ弰婵炰匠鍕彾濠电姴浼ｉ敐澶樻晪闁逞屽墮椤繘鎼归崷顓犵厯闂佸吋鍓氶崹鐓庮焽閸洘鈷戦柛娑橈攻閻撱儵姊虹敮顔剧М闁绘侗鍠楀鍕箛椤掑偆鍟囧┑鐐舵彧缁蹭粙骞夐敓鐘撳鈹戠€ｎ偆鍘介梺缁樏崢鏍嚋椤忓牊鐓曢柡鍌濇硶瀛濋梺宕囩帛閹稿濡堕敐澶婄闁冲湱鍋撶€氬吋绻濆▓鍨灓闁硅櫕鎸哥叅闁靛牆顦伴崐鍫曟煃鏉炴壆璐伴柣鏂挎閺岋綁鎮㈠畡鎵泿闂傚鍓﹂崜娑㈠焵椤掍緡鍟忛柛锝庡櫍瀹曟垶绻濋崒婊勬闂佸壊鍋呭ú锕傚极閸℃褰掓晲閸涱喗鍎撳Δ鐘靛仦椤ㄥ﹤顫忕紒妯诲闁告稑锕ラ崕鎾绘⒑閸濄儱娅忛柛瀣閸欏懘姊洪棃娑氱疄闁稿﹥娲熼幃鐐哄垂椤愮姳绨婚梺纭呮彧缁查箖藟婢跺⊕鐟邦煥閸垻鏆┑顔硷功缁垶骞忛崨鏉戝窛濠电姴鍟崜鍨繆閻愵亜鈧牠宕归棃娴虫稑鈹戠€ｎ亞鐣洪悷婊勬楠炲啴鍩￠崘鈺佺彴闂佽偐鈷堥崜娑㈠汲椤愶絿绡€鐎典即鏀卞姗€鍩€椤掍焦绀嬫鐐诧躬楠炲洭顢楅崒娑欘啌濠电偞鎸婚崺鍐磻閹剧粯鐓涢悘鐐插⒔濞叉挳鏌嶉妷顖滅暤妤犵偛顑呴埞鎴﹀炊閳衡偓闁垱绻濋悽闈浶ｆい鏃€鐗犲畷浼村冀椤撶喓鍘愰梺鎸庣箓椤︻垶鎷戦悢鍏肩叆婵犻潧妫欓ˉ鐐烘煕鎼达絽鏋涢柡灞炬礉缁犳盯寮撮悙鎰╁灪閹便劑鏁愰崨顖氣叺濠殿喖锕ュ钘夌暦椤愶箑唯闁靛鍔х徊鍧楀焵椤掆偓閸樻粓宕戦幘缁樼厵闂傚倻顥愰崗宀€鈧鎸风欢姘舵偂椤愶箑鐐婇柕濞垮劜閹稒绻涚€涙鐭婄紓宥咃躬瀵鎮㈤崗鐓庘偓缁樹繆椤栨繃顏犲ù鐘层偢濮婃椽宕ㄦ繛姘灴楠炲啴宕掑鍏肩稁濠电偛妯婃禍婊冾啅濠靛棌鏀介柣妯跨簿閸忓瞼绱掔拋宕囩獢婵﹥妞藉畷顐﹀Ψ閵夋劧缍侀弻锝呂旀笟鍥ㄧ暭缂備緡鍠栭澶愮嵁閹烘嚦鏃€绻濋崘鈺傜彨闂傚倷鑳剁划顖炲礉閺嵮岀劷婵炲棙鍔楅々閿嬨亜閹捐泛浜归柡鈧禒瀣厽婵☆垱顑欓崵瀣偓瑙勬偠閸庣敻寮诲☉銏″亜闁告稑锕ら崬澶愭倵鐟欏嫭绌跨紓宥勭閻ｇ兘鎮㈢喊杈ㄦ櫖濠电偞鍨堕懝楣冪嵁瀹ュ棔绻嗛柣鎰典簻閳ь剚鐗犲濠氭晲婢跺﹦顦柣蹇曞仧閻擃偄煤椤忓嫀褍顭跨捄鐚村姛妞ゃ垹鎳樺铏规喆閸曨偄濮告繝娈垮枔閸婃繈骞冮檱缁犳稑鈽夊顒€绲奸梻浣规偠閸庢粌顓奸崱妯绘瘞濠电姷鏁搁崑娑㈠箠鎼粹垾娑樷攽鐎ｎ亣鎽曢梺璺ㄥ枔婵潙顪冩禒瀣厱闁规澘鍚€缁ㄧ晫鎮敃鍌涒拻濞达絿鐡旈崵鍐煕閻樺磭澧电€规洘鐓″濠氬Ψ閳垛晛浜鹃柨鏇炲€告儫闂佸疇妗ㄧ粈浣虹玻濞戞﹩娓婚柕鍫濇鐎垫瑩鏌涢弴銊ュ婵″弶锕㈠濠氬磼濞嗘垵濡介梺娲讳簻缂嶅﹤鐣峰ú顏勭妞ゆ牗鑹鹃悗顓㈡煛婢跺﹦澧愰柡鍛矒瀵啿顭ㄩ崼鐔哄幗闂佽澹嗘晶妤€鈽夎閺屾盯濡搁敂鍓х暤闂佸疇顫夐崹鍧楀箖閳轰胶鏆﹂柛銉ㄥ煐濞堝綊姊绘担鍛婃儓闁瑰啿绻橀幃锟犳晸閻樿尪鎽曞┑鐐村灦閿曗晠鎮￠妷鈺傜厓鐟滄粓宕滈悢鐓庣畺闁绘劗鍎ら弲婊堟⒑濞嗘儳鐏犲ù婊堢畺濮婂宕奸悢鎭掆偓鎺楁煟閹烘挻鍊愰柡灞糕偓宕囨殕閻庯綆鍓涜摫闂備浇顕栭崹鍗炍涢崘顔衡偓浣糕槈濮楀棛鍙嗛梺鍛婄☉閹锋垹寰婇崜褎宕叉繛鎴炲焹閸嬫挸鈽夊▎妯煎姺闂佹椿鍘奸鍥╂閹烘鏁婇柤鎭掑劚绾惧啿螖閻橀潧浠滄俊顐ｇ懅缁參鎮㈤悡搴ｅ姦濡炪倖甯婂鎺旀崲閸℃稒鐓欓梻鍌氼嚟椤︼箓宕鐐粹拺闁告稑锕ゆ慨鍌炴煕閺傛寧鎹ｇ紒顔芥煥鐓ゆい蹇撴噽閸橀潧顪冮妶鍡樷拻闁硅櫕鎸婚弲鍫曞蓟閵夛妇鍘靛銈嗙墬缁嬫帡藟閸儲鐓曢柍鍝勵儑閻ｇ敻鏌涢埞鎯т壕婵＄偑鍊栫敮鎺斺偓姘煎墰婢规洘绻濆顓犲幍闂佺顫夐崝锕傚吹濞嗘挻鈷戦悷浣靛€曢埀顒佺墬缁岃鲸绻濋崶顬囨煕鐏炲墽鐓瑙勬礋濮婄粯鎷呴懞銉闂佸摜鍠嶉崡鍐茬暦娴兼潙鍐€妞ゆ挾鍋犻幗鏇㈡⒑閸濆嫭宸濋柛鐘虫尵缁粯銈ｉ崘鈺冨幈濡炪値鍘介崹鐢稿几濞戞ǚ鏀芥い鏃傗拡濡叉悂鏌嶇憴鍕伌闁糕晛瀚板畷姗€濡歌閵堢兘姊绘担铏瑰笡妞ゃ劌鎳橀弫鍐晝閸屾氨顔夐梺闈涚箳婵厼危閸儲鐓忛煫鍥ㄦ礀瀛濋梺鍏兼緲閻忔繈鍩為幋锕€鐓￠柛鈩冾殘娴犫晛鈹戦悙鑼憼缁炬澘绉剁划瀣吋婢舵ɑ鏅滈梺鍛婃处閸樿棄鈻撻幆褉鏀介柣妯款嚋瀹搞儵鏌熼搹顐㈠濞存粍鎮傞幃浠嬪川婵犲嫬骞?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婄拋鏌ュ几濞嗘挻鈷戠紓浣姑粭鈺佲攽椤斿搫鈧骞戦姀鐘闁靛繒濮撮懓鍨攽閳藉棗鐏ユい鏇嗗懎鏋堢€广儱顦伴悡鐔兼煟閺傛寧鎲搁柟铏礈缁辨帡鎮╅搹顐㈢３濡ょ姷鍋涢崯顐ョ亙闂佸憡渚楅崰妤€鈻嶅鍫熺厵闁兼祴鏅炶棢闂侀€炲苯澧查悘蹇旂懇瀵偊寮介鐔叉嫽婵炶揪绲介幗婊堟晬瀹ュ鐓欓柧蹇ｅ亝鐏忕數绱掗鑺ヮ棃闁诡喕绮欏畷褰掝敃閵忕姷鍝庡┑鐘垫暩閸嬬偤宕归崼鏇熸櫇闁冲搫鎳庨悿顕€鏌涘☉鍗炴灁濞存粍绮撻弻锟犲礃閵婏附鎮欓梺鍛婅壘缂嶅﹪寮婚敐澶婄厸濞撴埃鍋撻柛娆忓閹便劍绻濋崘鈹夸虎閻庤娲忛崝宥囨崲濠靛洦濯寸€瑰嫰鍋婇崝澶娾攽閿涘嫬浜奸柛濞垮€濆畷鎴﹀礋椤栤偓閸ヮ剚鐒肩€广儱鎳愰澶愭⒑闂堟稓绠冲┑顕呭弮瀵劍绂掔€ｎ偆鍘遍梺闈涱槶閸ㄥ搫鈻嶉崶顒佺厽闁挎洍鍋撻柣妤佹尭椤繘鎼归崷顓犵厯闂佸湱顭堢€涒晠骞忔潏鈺冪＜闁绘劦鍓氱欢鑼磼婢跺﹦绉烘鐐插暙椤粓鍩€椤掑嫮宓佹俊顖濆亹绾惧吋淇婇鐐存暠閻庢艾銈稿缁樻媴閸涘﹤鏆堢紓浣告惈濞尖€崇暦閺囥垺鐒肩€广儱鎳庨崵鎴︽煙閸忚偐鏆橀柛鏂跨Ч閹€斥枎閹惧鍙勯棅顐㈡祫缁蹭粙鍩€椤掆偓椤戝銆侀弮鍫濋唶妞ゆ劧绲惧▍鏃堟⒒娴ｇ懓顕滅紒璇插€胯棟妞ゆ牜鍋為悡渚€鏌涢妷顔煎闁绘挻鐩弻娑㈠Ψ閵忊剝鐝旀繛瀵稿█娴滃爼寮诲☉銏″€烽柤鍝ユ暩閵嗘劙姊虹€圭媭娼愰柛銊ユ健楠炲啫鈻庨幘宕囬獓闂佺懓顕慨鍐差浖閹剧粯鈷掑ù锝囨嚀椤曟粎鐥弶璺ㄐу┑锛勬暬瀹曠喖顢涢敐鍡樻珜闂備線鈧偛鑻晶鎾寠濠靛鈷掗柛顐ゅ枍缁惰鲸绻涢崗鑲╁缂佺粯绋戦蹇涱敊閼姐倗娉垮┑鐐殿棎閸嬫劖绻涢埀顒勬煛鐏炲墽娲寸€殿喗鎸虫俊鎼佸Ψ閵夈垺瀚藉┑鐘殿暯濡插懘宕戦崱娑樼；闁靛牆鎷嬪鏍煣韫囨挻璐＄痪鎯у悑閹便劌螖閳ь剙螞濞嗘挻鍋╅柣鎴ｅГ閳锋垿鏌涢敂璇插箺闁搞倐鍋撴繝鐢靛仜閻即宕愬┑瀣ㄢ偓?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳婀遍埀顒傛嚀鐎氼參宕崇壕瀣ㄤ汗闁圭儤鍨归崐鐐烘偡濠婂啰绠荤€殿喗濞婇弫鍐磼濞戞艾骞堟俊鐐€ら崢浠嬪垂閸偆顩叉繝闈涱儐閻撴洘绻涢崱妤冪缂佺姴顭烽弻鈥崇暆鐎ｎ剛袦婵犳鍠掗崑鎾绘⒑闂堟稓绠氶柡鍛箞瀹曟繈宕妷褏锛濇繛杈剧稻瑜板啯绂嶉悙顒傜瘈闁靛骏绲剧涵楣冩煛閸偄澧寸€规洑鍗抽獮鍥箥椤旂厧绲奸梻浣规偠閸庮垶宕濆鍥╃焼闁逞屽墴濮婃椽骞栭悙鎻掝瀴缂備浇顕ч崐鍧楀春閻愬搫绠ｉ柨鏃囨娴滃綊鏌ｆ惔鈩冭础濠殿喗鎸宠棢婵犻潧鐗忕壕钘壝归敐鍛儓妞ゅ孩鎸剧槐鎾愁吋閸曨厾鐛㈤悗娈垮枛椤攱淇婇幖浣肝ㄩ柕蹇婂墲閺夋悂姊绘担铏广€婇柛鎾寸箞閵嗗啴宕卞缁樼亖闂侀潧顭堥崕顕€寮ㄩ懞銉ｄ簻闁哄啠鍋撴い鎴炵懃椤洭骞囬婊€绨婚梺闈涱焾閸庤顔忛妷锔轰簻妞ゆ劑鍨荤粻濠氭煙閻撳海绉虹€规洜鍏橀、姗€鎮╅棃娑樿劘闂傚倸鍊烽懗鍓佹兜閸洖绀堟繝闈涱儍閳ь剙鍟村畷銊╁箹椤撶喐娅嗛梺璇插缁嬫帡鈥﹂崶鈺冧笉濡わ絽鍟悡鏇熴亜閹扳晛鈧洖鈻嶈箛娑欑厽闁瑰搫绉堕惌娆撴煛鐏炵晫啸妞ぱ傜窔閺屾盯骞樼€靛摜鐤勯梺鎸庣箘閸嬬偛顕ラ崟顖氱疀妞ゆ帒鍋嗛崬璺衡攽閿涘嫬浠滄い鎴濇噽閳ь剚绋堥弲鐘汇€侀弴銏″殟闁靛绠戝鍨攽閳藉棗鐏￠悗绗涘懏鍏滈柛鎾楀懐锛滈梺缁樏壕顓熸櫠闁秵鐓欓柛娆忣槹閸婃劙鏌熼銊ユ搐闁卞洦绻濋棃娑氬ⅱ闁瑰啿妫楅埞鎴︽倷濞堟寧顥撻梺绋款儐閸旀牜鎹㈠☉銏犵劦妞ゆ帒瀚悡娑㈡倶閻愭彃鈷旀繛鎻掝嚟閳ь剚顔栭崰妤呭箰閾忣偂绻嗛柟闂寸鍞梺闈涱樈閸犳霉閸ヮ剚鈷掗柛灞剧懆閸忓本銇勯鐐靛ⅱ闁逞屽墯缁嬪牓寮查悩鑽ゅ祦闁硅揪绠戦悙濠勬喐濠婂喚鐒?YAML闂?
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
        // winget 闂?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟版晥濠电姭鍋撳〒姘ｅ亾婵﹨娅ｇ槐鎺懳熼搹閫涚礃婵犵妲呴崑鍕偓姘煎枤閸掓帗绻濆顓炰汗缂傚倷鐒﹂…鍥储閻㈠憡鈷戠痪顓炴媼濞兼劙鏌涢弮鎾剁暤鐎规洟娼ч埢搴ㄥ箣閻樼绱查梻浣虹帛閿曘垹顭囪瀵鈽夊▎鎰伎婵犵數濮抽懗鍫曟儗濞嗘垟鍋撶憴鍕闁绘牕銈搁妴浣肝旈崨顓犲姦濡炪倖甯婄欢锟犲绩娴煎瓨鈷掗柛灞剧懅椤︼附绻濋埀顒佹綇閵婏附鐝峰┑掳鍊愰崑鎾淬亜椤撶偟浠㈤摶锝夋煠濞村娅囬柣鎺戙偢濮婃椽宕ㄦ繝鍌氼潊闂佸搫鍊搁崐鍦矉瀹ュ拋鐓ラ柛顐ゅ枔閸樻悂姊洪崨濠傚闁告柨绉靛鍕礋椤戝彞绨婚梺闈涱槶閸庡搫危閹间焦顥嗗鑸靛姈閻撶喐淇婇婵囶仩婵犫偓閹绢喗鐓曢悗锝冨妼閻忣亪鏌熸笟鍨缂佺粯绻堝畷姗€鍩炴径姝屾濠电姷顣藉Σ鍛村磻閸曨垰鐤い鎰堕檮缁犳帡姊绘笟鈧褔鎮ч崱妞㈡稑鈽夊Ο蹇氭閳ь剛鏁哥涵鍫曞磻閹捐埖鍠嗛柛鏇ㄥ墰椤︻參姊洪崫銉バｉ柣妤冨█婵″瓨绗熼埀顒€顕ｉ幘顔碱潊闁斥晛鍟悵鎶芥⒒娴ｈ櫣銆婇柛鎾寸箞閹兘濡烽埡鍌氭畱闂佸壊鍋呭ú姗€鍩涢幒妤佺厱閻忕偞宕樻竟姗€鏌嶈閸撴岸骞冮崒姘辨殾闁归偊鍙庡Σ褰掑箹鏉堝墽鎮奸柣鎺戝悑缁绘盯骞橀弶鎴犲姲闂佺顑嗛幐濠氥€冮妷鈺傚€风€瑰壊鍠楅埢鍫ユ⒑缁洘娅呴悗姘緲閻ｅ嘲顫滈埀顒勩€侀弮鍫濆耿婵☆垳鍎ら悿鍕節閻㈤潧孝闁汇儱顦靛鑸垫償閹惧厖澹曢梺鍝勬储閸ㄥ湱绮婚悩璇茬閺夊牆澧介幃濂告煟椤撶偞顥㈤柡灞剧洴椤㈡洟濡堕崨顔句簴缂傚倷鑳舵繛鈧紒鐘崇墵瀵鎮㈢悰鈥充壕婵炴垶鐟悞浠嬫煙閸愭彃鏆ｉ柡宀嬬秬缁犳盯鏁愰崟顖氫粣闂備礁鎼張顒傜矙閹捐鐒垫い鎺戯攻閳ь兙鍎靛畷姗€濡搁姀鈥崇彾闂傚倸鍊风欢姘焽瑜嶇叅闁靛牆娲犻崑鎾愁潩椤愩垹绁梺缁樹緱閸ｏ綁鐛幒鎳虫棃鍩€椤掑倻涓嶉柨婵嗘缁♀偓闂佸壊鐓堥崑鍕叏閸愵喗鐓熼柕澶樺枙闁垱鎱ㄦ繝鍕笡闁瑰嘲鎳橀幖褰掓偡閹殿噮鍋ч梻鍌欑濠€閬嶁€﹂崼銉⑩偓锕傛倻閽樺鐣鹃梺鍛婃处閸ㄥジ寮澶嬬厽闁归偊鍨伴拕濂告煙閼恒儲绀€闁宠鍨块幃娆撳级閹寸姳鎴烽梻浣规偠閸斿苯锕㈡潏鈺佸灊閻犲洦绁村Σ鍫ユ煏韫囥儳纾块柛姗€浜跺Λ鍛搭敃閵忊€愁槱闂佸湱顭堥…鐑界嵁婢舵劕绠瑰ù锝呮贡閸樹粙姊虹€圭姵銆冪紒鑸靛哺瀹曪綁宕熼娑氬幐闁诲繒鍋熺涵鍫曞磻閹捐绠涘ù锝呮啞閸婎垰鈹戦悙鑸靛涧缂佸弶宕橀妵鎰板礃閳哄喚娲告俊銈忕到閸燁垶鎮″☉銏＄厱閻忕偘绠欓鍫濈婵犲﹤鐗婇悡娑㈡煕鐏炵虎娈斿ù婊堢畺濮婂宕掑顑藉亾閻戣姤鍤勯柛鎾茬閸ㄦ繃銇勯弽顐粶缂佲偓婢舵劖鐓涚€规搩鍠栭惉濂稿礉瀹勬壋鏀介柣鎰级椤ョ偤鏌涢妸褎鏆╂繛鍡愬灲婵″爼宕堕埡鍐跨闯濠电偠鎻紞鈧い鏇熺墵閹偤鎮惧畝鈧壕濂告煟濡灝鍚归柣锝嗘そ閺岀喖顢欑粵瀣姼缂備胶濮甸惄顖氼嚕椤曗偓瀹曟帒顫濋敐鍛稑闂傚倸鍊烽懗鍫曗€﹂崼銉晞闁糕剝绋戠粻鏌ユ煕閵夘喖澧扮€规挷绶氶弻娑㈠Ψ閿濆懎顬夋繝娈垮灡閹告娊寮诲☉妯锋婵妫涢崣鏇㈡⒑?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊绋掑婵堢磼閳锯偓閸嬫捇姊绘担渚劸闁哄牜鍓涢崚鎺戠暆閸曗斁鍋撻崒姣椽顢旈崨顏呭闂備浇濮ら敋妞わ富鍨跺鎶芥偄閸忚偐鍘遍梺缁樏壕顓熸櫠閻㈢鍋撳▓鍨灈妞ゎ厼鍢查锝夊箻椤旇棄浜滈梺鎯х箺椤曟牠宕惔銊︹拻濞达絿顭堥ˉ蹇涙煟閹惧磭澧︾€规洑鍗冲浠嬪Ω瑜忚ぐ楣冩⒑閸涘﹥澶勯柛瀣у亾闂佽　鍋撳ù鐘差儐閻撶喖鏌熼柇锕€澧紒鐙欏洦鐓冪紓浣股戠粈鈧梻鍥ь槹缁绘繃绻濋崒姘间紑闂佹椿鍘界敮鐐哄焵椤掑喚娼愭繛鍙夛耿閺佸啴濮€閵堝懏妲梺閫炲苯澧柕鍥у楠炴帡宕卞鎯ь棜濠碉紕鍋戦崐銈嗙濠婂牆鐤悗娑櫭肩换鍡涙煕椤愶絾绀€妤犵偑鍨烘穱濠囶敍濠婂啫濡哄┑鐐茬墱閸嬪﹤顫忕紒妯肩懝闁逞屽墮椤洭鎳￠妶鍛瓘婵犵數濮甸懝鍓у閸ф绾ч柛顐亜娴滅偛霉?0闂?1978335189闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忣澀鐒婂ù锝呮贡閳ь剙顭峰娲倷閽樺濮庣紒鍓ц檸閸樺ジ鏁冮姀銈呯妞ゆ梻鏅崢閬嶆⒑閸濆嫬鏆婇柛瀣崌閺屾盯濡搁妷褌铏庨梺浼欑到閸㈣尙鍙呭銈呯箰鐎氼噣宕濋敃鈧—鍐Χ閸℃鐟愰梻鍌氬缁夌數绮嬪鍜佺叆闁割偆鍠撻崢鐢告⒑缂佹ê鐏﹂柨姘舵煟韫囧鍔﹂柡灞界Х椤т線鏌涢幘瀵哥畵闁伙絿鍏樺畷褰掝敃閵堝洤楠勯梻浣告惈濞层劑宕伴幘缁樷挃闁告洦鍘剧壕濂告倵閿濆骸骞楃痪顓℃硾铻栭柣妯哄级閸犳ɑ鎱ㄦ繝鍐┿仢鐎规洘绮撻獮鎾诲箳瀹ュ洤鍤紓鍌氬€风粈渚€顢栭崱娑樺瀭闁煎摜鏁搁埞宥呪攽閻樻彃顏€规洖顦甸弻鏇熺箾瑜嶉幏鎴犳閺屻儲鈷掑ù锝堟鐢盯鏌ㄩ弴妯虹仼妞ゎ厼娲╅ˇ宕囩磼閸屾稑娴柡灞芥椤撳ジ宕卞▎蹇撶闂佽楠搁崢婊堝磻閹剧粯鍊甸柨婵嗛婢т即鏌ㄥ☉姘瀾缂佺粯绋撻埀顒傛暩椤牓顢撻幘鍓佺＜闁绘娅曞畷灞绢殽閻愯韬柟顔界懇瀹曨偊宕熸惔娑樺闁哄本娲樺鍕槈濮楀棙鍠樺┑鐐茬摠缁酣宕楀Ο渚綎濠电姵鑹剧壕鍏肩箾閸℃ê濮夐柕鍫熸緲閳规垿鍩勯崘锔跨不闂佸憡姊归崹鍧楃嵁閸愵喗鍊烽柣鎴炆戝▍鍥⒑缁嬫寧婀版慨妯稿姂钘濇い鏂跨毞閺€浠嬫煟閹邦剙绾фい銉︾矌缁辨帞绱掑Ο鐑樿癁閻庢鍠楁繛濠囧极閸愵喖纾兼繛鎴炶壘楠炴姊绘担鍛婃儓闁哥噥鍋婂畷鎰版偨缁嬭法顔嗛梺璺ㄥ枔婵敻鎮″☉妯忓綊鏁愰崨顔兼殘闂佸摜濮甸敃銏ゅ蓟閳ュ磭鏆嗛悗锝庡墮閸╁矂鏌ф导娆戠М闁哄本绋戦埞鎴﹀幢濡ゅ﹣鐥繝娈垮櫙缁查箖宕濆▎蹇ｆ綎婵炲樊浜滄导鐘绘煕閺囥垺娑ч柛鎾讳憾濮婃椽宕烽鐔锋畬濠电偛鐪伴崐婵嬫晲閻愬墎鐤€闁哄啫鍊婚惁鍫濃攽椤旀枻渚涢柛鎾寸洴閺佸秴顭ㄩ崘锝嗘杸闂佺粯鍔曞鍫曀夐悙鐑樼厱闁宠桨鑳舵晶鏇㈠础鏉堛劊浜滈柡鍥殔娴滄儳鈹戦纭锋敾婵＄偠妫勯悾鐑芥倻缁涘鏅ｅ┑鐐村灦閻熴儵鎮甸柆宥嗏拻濞达絼璀﹂悞鐐亜閹存繃鍣归摶鐐寸節闂堟侗鍎忛柡鍕╁劦閺屽秷顧侀柛鎾村哺婵＄敻宕熼姘鳖唺閻庡箍鍎卞ú銈嗕繆娴犲鈷戞慨鐟版搐婵′粙鏌涢妸銉﹀仴鐎殿喛顕ч埥澶婎煥閸涱垱婢戦梻浣告惈閸婄螞椤撶喓顩叉い蹇撶墕缁犳牠鏌ｉ幇闈涘⒒婵℃彃缍婇獮鏍庨鈧悘鈺呮煛鐎ｎ亜鈧灝顫忓ú顏勫窛濠电姴鍟ˇ鈺呮⒑缁嬫鍎愰悽顖椻偓宕囨殾闁汇垻顭堝洿婵犮垼娉涢鍌炲箯濞差亝鈷戦柛娑橈功缁犳捇鎮楀顒€妲婚崡杈╂喐閻楀牆绗氶柣鎾存礋閺屾洟宕煎┑鍫㈩唺闂佷紮绲块弫濠氬蓟閳╁啯濯寸紒瀣濞堝鎮楃憴鍕閻㈩垱甯￠崺銉﹀緞婵犲孩鍍靛銈嗗坊閸嬫挾绱掗悩顔煎姦婵﹦绮幏鍛驳鐎ｎ亝顔勬繝鐢靛仜椤曨參宕楀Ο渚殨妞ゆ劧绠戠粻鐟懊归敐鍡欍偞闁稿鎹囬弫鍐磼濮樿泛鏁归梻浣虹帛閺屻劑骞栭锕€鐒诲ù鐓庣摠閳锋帡鏌涚仦鍓ф噭缂佷胶澧楅妵鍕即閸℃鍣抽柛瀣尭閳藉骞掗幘瀵稿綃闂備胶顢婂▍鏇犳崲閸愵喖围闁挎繂顦粈鍐煥濠靛棗鈧憡绂嶆ィ鍐╃叆闁哄洨鍋涢埀顒€缍婂畷鎰節濮橆厾鍙冨┑鈽嗗灟鐠€锕€危婵傚憡鐓欓柤鎭掑劜缁€瀣叏婵犲啯銇濈€规洦鍋婂畷鐔碱敆閳ь剛绮ｅ☉銏♀拺闁告縿鍎卞瓭濠电偞娼欓崲鏌ュ煝鎼淬劌顫呴柕鍫濇閳ь剛鍏橀幃妤呮濞戞瑦鍠愬銈嗘⒐濞茬喎顫忓ú顏呭仭闂侇叏绠戝▓鍫曟⒑缁嬫鍎戝┑顔炬暩缁晠鎮㈡總澶嬪缓闂侀€炲苯澧寸€殿喖顭烽弫鎰緞濡粯娅嶉梻浣虹帛濡礁鈻嶉敐澶嬪仾闁告洦鍋€閺€?
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帡宕濋幘顔解拺閺夌偞澹嗛ˇ锔姐亜閹存繃顥㈠┑锛勬暬瀹曠喖顢涘槌栧晪闂佽崵濮惧▍锝夊磿閵堝鍊靛Δ锝呭暞閳锋垿鏌涘☉姗堝姛闁瑰啿鍟扮槐鎺旂磼濡搫顫掑┑鐘亾濞撴埃鍋撴慨濠呮缁瑧鎹勯妸褜鍞堕梻浣告啞閼归箖顢栭崶鈺傤潟闁绘劕鎼粻锝夋煟濡櫣浠涢柨娑樼箻濮婅櫣鎷犻垾宕団偓濠氭煕閺囥劌浜楁繛锝庡枟缁绘繂鈻撻崹顔界亪闂佹寧娲忛崕閬嶁€旈崘鈺冾浄閻庯絻鍔夐崑鎾存媴閼叉繃妫冨畷銊╊敇閻樻彃袝濠碉紕鍋戦崐鏍暜閹烘纾瑰┑鐘崇閸庢绻涢崱妤冪疄闁逞屽墯鐢帡锝炲┑瀣櫜闁告侗鍓欓ˉ姘舵⒑鐠囨彃顒㈡い鏃€鐗犲畷鎶筋敋閳ь剙鐣烽幋鐐电瘈闁稿本绮嶅▓楣冩⒑閸撴彃浜濇繛鍙夌墵閹€斥槈閵忥紕鍘遍柣蹇曞仜婢т粙銆傞弻銉︾厽閹兼番鍔庨悾鐑樻叏婵犲嫮甯涢柟宄版嚇閹煎綊鎮烽幍顕呭仹濠碉紕鍋戦崐鎴﹀礉瀹€鍕疇婵せ鍋撴鐐插暢椤﹀湱鈧娲栧畷顒勫煝鎼淬劌绠ｉ柣妯虹仛濞堜粙姊婚崒娆戭槮婵犫偓闁秴纾块柕鍫濇媼濞兼牠鏌ゆ慨鎰偓鏇⑺夊顑芥斀闁稿本纰嶉崯鐐烘煟閹捐泛鏋戠紒缁樼洴瀹曞崬螖閸愵亶鍞归梻渚€娼荤徊鑲╁垝濞嗘挸钃熸繛鎴欏灩缁犳娊鏌￠崒娑卞劌闁哄棭鍙冨娲濞戞瑦鎮欓柣搴㈢煯閸楁娊濡存笟鈧顕€宕煎┑鍫Ч婵＄偑鍊栭崝褏寰婃ィ鍐炬晩鐎广儱顦伴埛鎺戙€掑锝呬壕闂侀€炲苯澧伴柛瀣洴閹崇喖顢涘☉娆愮彿闂佸憡鎸嗛崨顖ょ闯闁诲骸绠嶉崕閬嶅箠韫囨蛋澶愭倷閻㈢數锛滈梺绋挎湰濮樸劌鐡俊鐐€ら崢褰掑礉閹存繄鏆﹀┑鍌氭啞閸嬪嫮鈧懓瀚姗€鍩涢弴銏♀拻闁稿本鐟х粣鏃€绻涙担鍐叉礌閳ь剨绠撳畷濂稿Ψ閵壯嶇吹闁诲骸绠嶉崕閬嶆偋閸曨剛顩茬憸鐗堝笚閻撴瑩鏌ｉ幋鐐嗘垹浜搁鐏荤懓顭ㄩ崟鍨杹闂佸搫鐬奸崰鏍箠閺嶎厼鐓涢柛鏇烇工椤︾増绌辨繝鍥ㄥ亼婵炲棗绻戦幖鎰棯閹岀吋闁哄瞼鍠栭獮鎴﹀箛闂堟稒顔勯梻鍌欐祰閿熴儵宕愰弽銊р攳濠电姴娲ゅ洿闂佺鏈惌顔界珶閺囥垺鈷掑ù锝堝Г椤ュ鏌涢悢璺哄祮鐎规洑鍗冲浠嬵敇閻愯埖鎲伴梻渚€娼ц墝闁哄懏绮撳畷鎴﹀礋椤栨稓鍘卞┑鐘绘涧濡鎮甸弮鍫熺厪闊洤顑呴悘顔剧棯閹佸仮闁哄苯绉烽¨渚€鏌涢幘瀵告创闁糕斁鍋撳銈嗗笒閿曪妇绮旈悽鍛婄厱闁绘ɑ鍓氬▓婊堟煕閳哄倻娲存鐐村笒铻栧ù锝呮惈楠炲牊淇婇悙顏勨偓鏍礉閹达箑纾归柡鍥ュ灩閸戠娀鏌￠崘銊у闁绘挸绻橀弻娑㈠焺閸忕媭浜炵划顓㈠箳濡や緡姊挎繝銏ｅ煐閸旀牠鎮￠悢鍛婂弿婵°倐鍋撴俊顐ｎ殕缁傚秴鈹戦崶鈺冾啎闂佸憡渚楅崣蹇涘绩婵犳碍鐓欏〒姘仢婵＄晫绱掔紒妯肩疄鐎规洏鍔戦、姘跺川椤掆偓閹藉姊婚崒娆戭槮缂傚秴锕棢闁规儳顕粻楣冩煃瑜滈崜鐔煎蓟濞戙垹惟闁挎洍鍋撻柛鏂诲€濋弻锝夋晲閸パ冨箣閻庤娲栭悥濂稿极閸岀偞瀵犲璺鸿嫰楠炲牏绱撻崒姘偓鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗坊閸嬫捇鏌￠崟鈺佸姦闁哄备鈧磭鏆ゆい鏂垮悑閸ｇ儤绻涢崼鐔哥婵﹪缂氶妵鎰板箳閹垮嫮鍚归梻浣哄帶閻ゅ洤鈻斿☉銏″仼闁割煈鍋嗛悷褰掓煃瑜滈崜鐔奉嚕鐠囨祴妲堥柕蹇曞Х閸旀挳姊洪崨濠傚Е濞存粍鐗曞嵄闁割偁鍎查埛鎴犵磽娴ｅ顏呮叏閸ヮ剚鐓ラ柡鍥崝锕€鈹戦敍鍕毈鐎规洜鍠栭、娑橆潩妲屾牕鏁介梺璇查缁犲秹宕曢崡鐏诲綊宕掑В纭风秮椤㈡瑩鎮惧畝鈧鏇㈡煟鎼淬垻鈯曟い顓炴喘閹瞼鈧綆鍓涚壕鍏笺亜閺冨洤袚閻忓浚鍘介妵鍕敃閵忋垻顔掗梺鍦帶缂嶅﹤鐣烽悜绛嬫晣闁绘瑥鎳愭惔濠傗攽閻樻鏆俊鎻掓嚇瀹曞綊妫冨☉鎺撶€洪梺鎸庣箓濡盯銆呮搴ｇ＜閻庯綆鍋掗崕銉︾箾閹炬剚鐓奸柡灞炬礋瀹曠厧鈹戠€ｇ鍋撳Δ渚囨闁绘劕顕晶顒傜磼缂佹鈽夋い鏂跨箻椤㈡瑩鎮℃惔锝庡晣濠碉紕鍋戦崐鏍洪埡鍐濞撴埃鍋撻柣娑卞枟閹棃濡搁敃鈧▓鐔兼⒑闂堟侗妲堕柛搴㈡緲鏁堟俊銈呭暟绾句粙鏌涚仦鎹愬闁逞屽墰閸忔﹢骞冮悙鐑樻櫇濞达絽鎽滅粙濠囨⒒閸屾瑧顦︾紓宥咃躬瀵劑宕￠悜鍡樺瘜闂佽姤锚椤﹁棄顭囬弽銊х鐎瑰壊鍠曠花鑽も偓鐟版啞缁诲倿鍩為幋锔藉亹闁圭粯甯╂禒鍗炩攽閳╁啫绲婚柣妤佹尭椤繐煤椤忓嫮顔囬柟鍏肩暘閸ㄥ鏁嶅鈧铏圭矙閸栤剝鏁剧紓浣筋嚙閻楁捇鎮伴鈧畷姗€濡告惔锝呮灁缂佽鲸甯掕灒闁兼祴鏁╅妶澶嬧拻濞撴埃鍋撴繛浣冲棗娅ｉ梻浣告啞缁牏绮堟笟鈧鏌ヮ敇閵忊檧鎷婚梺绋挎湰閻熝囁囬敃鍌涚厵閻犲泧鍛紵闂佸憡甯楃敮鎺楁偩濠靛绀嬫い鎰╁€栭悞楣冩⒒娴ｇ瓔娼愭い鏃€鐗犲畷鏉款潩閼搁潧鍓归梺鍦亾閻ｎ亝绂嶅鍫熺厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹥妞藉畷顐﹀礋椤撶姴濮界紓鍌欐祰瀵挾鍒掑▎鎾虫槬闁靛绠戠欢鐐烘煙闁箑澧版い銏犳嚇濮婅櫣绱掑Ο鍝勵潓濡炪倖娲﹂崣鈧柕鍥ㄥ姈閹棃濡搁敂鎯у箺闂佺懓鍚嬮悾顏堝磹閵堝拋鐒介柟閭﹀幘缁犻箖鏌涘▎蹇ｆШ濠⒀呮暬閺岀喖顢欓崫鍕Е閻庤娲橀〃濠傤潖閼姐倐鍋撻棃娑欐喐闁伙絿鍋撶换婵嗏枔閸喗鐏嶇紓浣筋嚙閸婂潡骞嗗畝鍕唶婵犮埄浜风紞渚€骞冨鍫熷殟闁靛鍨虹€氬ジ姊绘担鍛婂暈缂佽鍊婚埀顒佹皑閸忔ê鐣烽弴銏犵疀闁绘鐗忛崢钘夆攽鎺抽崐鏇㈠疮椤愩垻鎽ュ┑锛勫亼閸娿倝宕戦崟顓熷床闁归偊鍠栧鍙変繆閻愵亜鈧洜鎹㈤幇顔瑰亾濮樼厧澧扮紒顕嗙到閳藉鈻庨幋鐘垫闂備焦鐪归崹钘夘焽瑜嶉悺顓㈡⒑鐠囨彃顒㈤柛鎴濈秺瀹曟娊鏁愭径濠勭暫婵°倧绲介崯顖炲磻閵娾晜鐓曟繛鎴烇公閸旂喖鏌涘Ο鍦煓闁哄瞼鍠栧畷妤呮嚃閳哄倹顔冮梻浣规偠閸斿瞼绱炴繝鍌滄殾闁汇垹澹婇弫鍡涙煕閺囥劌澧伴柛姗€浜跺铏规喆閸曨偄濮㈠銈嗘处閸欏啴鐛繝鍥х倞妞ゆ帊鑳堕崢閬嶆⒑瀹曞洦鍤€闁诲繑绻堥幃姗€鍩￠崒妯圭盎闂佺懓澧介崑銊╁磻閵忊懇鍋撶憴鍕８闁告梹鍨块妴渚€寮崼婵嗙獩濡炪倖鐗楅〃鍛閹€鏀介柣妯虹－椤ｆ煡鏌嶉柨瀣伌闁诡喖鍢查埢搴ょ疀閹垮啩鎮ｉ梻浣告啞閺岋綁宕愰弽顓炍﹂柛鏇ㄥ灠閸楁娊鏌曟繛鍨姎缂侇偄绉归幃妤冩喆閸曨剛鈹涚紓鍌氱С缁€渚€鎮鹃悜钘夌闁绘垵妫欑€靛瞼绱撻崒娆戝妽閽冮亶鏌℃径瀣€愭慨濠勭帛閹峰懘鎼归悷鎵偧闂備礁鎲″褰掓偋閺団懇鈧箓宕惰閺嬪酣鏌熼悙顒佺稇闁逞屽墮閻栧ジ寮婚妸鈺佹婵炲棛鍋撶紞妤佺箾鐎涙鐭嬬紒顔肩Ч婵＄敻宕熼姘鳖啋闁荤娀缂氬銊モ柦椤忓牊鈷戦柛婵勫劚鏍￠梺鎸庢磸閸ㄨ棄顕ｆ繝姘亜缁炬媽椴搁弲鈺呮⒑鐠団€崇€婚柛灞句緱濞肩喖姊婚崒娆愮グ妞ゆ洘鐗犲畷鏉课旈崘銊ョ亰闂佽宕橀褏绮婚悩缁樼厽闁归偊鍓氶幆鍫ュ船椤栫偞鍋℃繝濠傚暟鑲栭梺杞扮贰閸犳牞鐏冮梺鍛婁緱閸犳岸宕㈡禒瀣拺缂備焦蓱閻撱儵鏌熼懞銉х煉闁诡垰鐬奸埀顒婄秵閸撴稓澹曢崗绗轰簻闁哄啫娲よ闁诲繐娴氶崣鍐蓟閿濆鏅查柛娑卞幗浜涙俊鐐€ら崢褰掑礉閹存繄鏆︽慨妞诲亾闁轰焦鍔欏畷鍗炩枎鐏炴垝澹曟繝鐢靛У绾板秹鍩涢幋鐘冲枑闁绘鐗嗙粭鎺懨瑰鈧崡鎶藉蓟濞戙垺鏅查煫鍥ㄦ礀閸╁苯顪冮妶搴″闁挎洦浜妴浣割潩鐠鸿櫣鍔﹀銈嗗坊閸嬫挻銇勯銏㈢閻撱倖銇勮箛鎾愁仹缂佸崬鐖煎娲川婵犲啫顦╅梺绋款儏閹冲酣鍩㈤幘娲绘晣闁靛繆妾ч幏铏圭磽娴ｅ壊鍎忛悘蹇撴噺铻炴慨妯诲閸嬫挾鎲撮崟顒傤槶闂佽桨鐒﹂幃鍌氱暦濮樿泛绠抽柟鎯у閻﹀牆鈹戦鏂や緵闁告挻宀歌棢婵犻潧妫岄弨浠嬫煟閹邦剙绾фい銉︾矊椤儻顦辩紒顔界懃閻ｇ兘顢楅崟顒€浠洪梺鍛婄☉鑹岄柟閿嬫そ濮婅櫣绮欑捄銊ь唶闂佸憡鑹鹃澶婄暦閹版澘绠涙い鎾跺Х椤旀洘绻涙潏鍓у埌闁哥喕娉曢埀顒佸嚬閸欏啫鐣烽敐鍫㈢杸婵炴垶鐟ч崢閬嶆⒑闂堟侗妯堥柛鐘崇墬閺呭爼顢涢悙瀵稿幗闂佽鍎抽悺銊х矆閸愵亞纾肩紓浣诡焽濞插鈧娲栧畷顒冪亙闂佸憡渚楅崹鏉款渻娴犲鈷掑ù锝咁潟閳ь兘鍋撻梺绋款儐閹告悂鍩為幋锔藉亹鐎规洖娴傞弳锟犳⒑缂佹ɑ灏紒缁橈耿瀵鈽夐姀鐘栥劑鏌熺€涙绠撻柡瀣ㄥ€曢湁闁绘挸楠搁弳锝夋煙椤旂瓔娈旈柍缁樻崌瀹曞綊顢欓悾灞肩敖缂傚倸鍊风欢锟犲窗濡ゅ懏鍋￠柨鏃傛櫕閳瑰秴鈹戦悩鍙夋悙缂佺姷鎳撻湁闁挎繂鐗嗛埀顑跨矙閸╋繝宕ㄩ鎯у妇濠电姰鍨奸崺鏍晪闂佸搫顑囬崰鎰板Φ閸曨垰唯闁挎繂鎳夐崑鎾诲即閵忕姷鍘撮梺纭呮彧闂勫嫰宕戦幇鐗堢厱妞ゎ厽鍨垫禍婵囨叏閿濆懎顏慨濠呮缁瑥鈻庨幆褍澹嬮梻浣侯焾椤戝啴宕愰弴銏″仼闁绘垼濮ら崑銊╂煕閹惧啿绾ч柡鍛矒濮婃椽宕滈幓鎺嶇凹缂備浇顕х€氭澘鐣烽幇鐗堝€烽柣鎴炃氶幏娲⒒閸屾氨澧涘☉鎾舵嚀閳藉濮€閻樻妲伴梻渚€娼ц噹閻忕偠濮ゅ▍蹇撯攽閻樺灚鏆╁┑顔惧厴閵嗗倿顢欓悙顒夋綗闂佸搫娲㈤崹鍦婵犳碍鐓欓弶鍫濆⒔閻ｈ京鐥幆褏绉洪柡宀嬬節瀹曞爼鍩℃担鍥风悼閳ь剝顫夊ú姗€鎮￠敓鐘茶摕闁绘柨鍚嬮悞浠嬫煥閺囨浜鹃梺鍝勵儐閹瑰洭寮诲☉姘ｅ亾閿濆骸浜濋悘蹇ｅ幗閵囧嫰顢曢敐鍥╃杽閻庤娲╃换婵嬪箖濞嗘挸绠甸柟鐑樻閸氬懘姊绘担绛嬪殭婵﹫绠撻敐鐐村緞婵炴帒鎼灒濠靛倽濮ゅ浠嬪箖閵忥紕闄勫璺猴攻椤撴寧淇婇悙顏勨偓鏍哄澶婄；闁规儳鐏堥崑鎾舵喆閸曨剛顦ラ梺闈涚墛閹倿鐛崘顔碱潊闁靛牆鎳嶇槐鍫曟⒑閸涘﹥澶勯柛瀣閹兘寮婚妷锔规嫽婵炶揪缍€濡嫰宕ヨぐ鎺撶厱闁绘ê鍟挎慨澶愭煕閹烘挸绗掓い鎾炽偢瀹曟儼顦查柣蹇擄躬濮婅櫣鎲撮崟顐㈠Б缂佸墽铏庨崣鍐嚕閹惰姤鏅滈柟瀛樺笧缁犳岸姊洪棃娑氬闁稿﹤鎲＄粋宥夊箚瑜滃〒濠氭煏閸繈顎楀ù婊勭箘缁辨帡顢欓懞銉ョ３閻庤娲樺浠嬪春閳ь剚銇勯幒宥夋濞存粍绮撻弻鐔煎传閸曨剦妫炴繛瀛樼矒缁犳牕顫忛悜妯侯嚤婵炲棙鍨堕崑褔姊虹粙璺ㄧ闁挎洏鍨介幃浼搭敊闁款垰浜鹃柨婵嗛閺嬬喖鏌￠崨顔剧疄闁哄苯绉烽¨渚€鏌涢幘瀵搞€掗柛鎺撳笚缁绘繂顫濋鐐搭吋闂備線娼ч悧鍡椕洪妸鈺傛櫖婵犻潧娲ㄧ粻楣冨级閸繂鈷旂紒澶樺墯閵囧嫰寮撮悢鍝勨拰闂佽鍠氶崑鐐电矉閹烘柡鍋撻敐搴′簽闁告ê宕埞鎴︻敊婵劒绮堕梺绋款儐閹告悂婀佸┑鐘诧工濡瑧娑甸悙顑句簻妞ゆ劑鍨荤粻濠氭煙閻撳孩璐￠柟鍙夋尦瀹曠喖顢曢姀鐘殿唹闂傚倸鍊风粈渚€骞夐埄鍐懝婵°倕鎳庣粣妤呮煛瀹擃喒鍋撴俊鎻掔墢閹叉瓕绠涢弴鐕佹綗闂佽鍎抽悺銊﹀垔閹绢喗鐓曢柨鏃囶嚙楠炴牜鈧鍠撻崝搴ｆ閹惧瓨濯撮柛婵嗗珔閿濆鐓熸俊銈呭暙閳诲牆鈹戦垾宕囨憼缂佹鍠栭崺鈧い鎺嗗亾妞ゆ洩缍侀、姘跺焵椤掆偓閻ｉ攱绺介崨濠備簻闂佺偓鑹鹃崐褰掓儓韫囨洜纾介柛灞捐壘閳ь剚鎮傚畷鎰版倻閼恒儱鈧潡鏌ㄩ弴鐐测偓鐢稿焵椤掑﹦鐣电€规洖鐖奸、妤呭焵椤掑嫬纾奸柕濞垮剭瑜版帗鍋愮€瑰壊鍠栭崜閬嶆⒑閹惰姤鏁遍柛銊ユ健瀵鈽夊Ο閿嬫杸闂佺硶鍓濋〃蹇斿閳ь剟鏌ｉ悙鏉戝毈濞存粠浜滈～蹇撁洪鍕獩婵犵數濮撮崯浼村矗閸℃稒鈷掑ù锝囶焾缁ㄨ崵绱撳鍕獢闁绘侗鍣ｅ畷鍗炍旈崘鈺傜暦闂備礁鎲″ú锕傚磻閸℃侗鏁婂┑鐘叉处閳锋垿鏌ｉ悢绋款棆闁圭晫濮甸幈銊︾節閸屾粎楔閻庢鍣崜鐔煎春閳?
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
            // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇牠姊绘笟鈧埀顒傚仜閼活垱鏅堕弶娆剧唵閻熸瑥瀚粈鍐磼缂佹绠撴い顐ｇ箞椤㈡﹢鎮╅幓鎺旂杽闂傚倸鍊烽悞锕傚几婵傜鐤炬繛鎴欏灩閻ゎ喖霉閸忓吋缍戦柣鎾偓鎰佺唵闁兼悂娼ф慨鍫ユ煟閹惧瓨绀嬮柟顔斤耿閹瑦锛愬┑鍡橆唲濠电姵顔栭崰鏍磹閸ф钃熼柨娑樺濞岊亪鎮归崶銊с偞闁稿鎹囧鎾閿涘嫮鏆㈤梻鍌欐祰椤曆囧礄閻ｅ瞼绀婇柛鈩冪☉绾惧鏌涘☉妯兼憼闁稿鍠栭弻锟犲炊閳轰礁澹夊┑鐐存尭椤兘寮婚弴銏犻唶婵犻潧顑愰埀顒侇殔闇夋繝濠傚缁犳牜绱掔紒妯兼创闁瑰磭濞€椤㈡鍩€椤掑嫬鍑犳繛鎴炲焹閸嬫挸鈻撻崹顔界亪濡炪値鍘鹃崗姗€鐛崘顔碱潊闁靛繆鈧厖绮ч梻浣哄仺閸庨亶宕㈤弽銊х彾闁哄洢鍨洪埛鎴炴叏閻熺増鎼愰柣蹇ｄ簼閵囧嫰寮埀顒勬偋閻樿绠栭柣鎰惈缁犳碍銇勯瑙ｅ亾瀹曞洨楔闂佽鍠掗弲娑㈠煝鎼淬倗鐤€闁哄倹顑欏Σ閬嶆⒒娴ｈ櫣甯涢柟鍛婃倐瀹曨垶骞嶉绛嬫綗闂佽鍎抽悺銊﹀垔閹绢喗鐓犳繛鏉戭儏婢ф煡鏌涢妸銉﹀仴鐎规洖鎼埥澶愬閻樻鍞烘繝寰锋澘鈧捇鎳楅崼鏇炲偍闁瑰墽绮崑鈩冪節婵犲倸顏柣顓熷笧閳ь剝顫夊ú妯煎垝瀹€鍕厺閹兼番鍔岀粻锝夋煟濡じ鍚柣鐔哥箞閺岋絾鎯旈敍鍕殯闂佺閰ｆ禍鎯版濡炪倖鐗楃划宀勊夊鑸电厱鐟滃酣銆冮崱娑樼厱闁瑰鍋戞禍婊堟煙閸濆嫭顥滃ù婊冨⒔缁辨挻鎷呴崜鍙壭﹀銈嗘处閸橀箖宕氶幒鎾剁瘈婵﹩鍘兼禍褰掓煟閻樺弶绌块悘蹇旂懇椤㈡瑩骞囬鍓э紳婵炴挻鑹惧ú銈夊几閿斿浜滄い鎰╁焺濡偓濡炪們鍨洪幑鍥х暦閸楃倣鐔煎捶椤撶姷鏆伴梻鍌欑濠€杈╁垝椤栨粍鏆滈柣鎰摠濞呯娀鏌曡箛瀣偓鏍偂濞嗘挻鐓曟繛鍡楁禋濡插綊鏌涙繝鍐ㄥ闁逞屽墯椤旀牠宕抽鈧畷鎶芥晲閸涱垱娈鹃梺缁樻尭缁ㄥ爼寮ㄩ挊澶嗘斀闁绘ɑ褰冮弳鐐烘煕?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幍缂傚倷鐒﹂敋缂佹う鍥ㄧ厓鐟滄粓宕滈敃鍌氱煑闁告劦鐓堝鏍煕濠靛棗鐝旂憸鏂跨暦閹偊妲炬繛瀵稿帶鐎氼剟鈥旈崘顔嘉ч幖绮光偓宕囶啇闂備胶绮〃鍛崲閹烘绀嗛柟鐑橆殕閸ゆ帡鏌曢崼婵囶棞闁哄鍊垮娲川婵犲啰锛曢梺鍝勬噺閻熝囨嚍鏉堛劎绡€婵﹩鍘搁幏娲煟閻斿摜鎳冮悗姘煎墴钘熼柟鎵閻撴盯鏌涘☉鍗炴灓婵炴彃顕埀顒冾潐濞叉鎹㈤崒婵堜航濠电娀娼ч崐濠氬疾椤愶箑绠犻柟鍓у劦閳ь剨绠撴俊鎼佸煛娴ｆ瓕鈧潡姊洪崘鍙夋儓闁稿﹨宕电划鏃€顦版惔锝囷紳闂佺鏈悷銊╁礂瀹€鈧槐鎺楊敋閸涱厾浠搁梺璇″灠鐎氫即銆佸☉銏″€烽梻鍫熺〒閵堬箓姊虹拠鎻掑毐缂傚秴妫濆畷鏉课旈崨顓狅紮闂佺粯鍨兼慨銈夋偂閺囥垺鐓欓柤鍓插墮婵¤姤绻涢崨顓熷殗闁哄本绋戦埞鎴炵節閸愬彞鍝楅梻渚€鈧偛鑻晶顖涖亜閺冣偓閻楃姴鐣锋导鏉戝唨妞ゆ挻绋堥崑鎾绘晝閸屾稑鈧鏌﹀Ο渚Ч闁诲寒鍓熷娲川婵犲倸袝闂佸摜濮甸悧婊勭珶閺囥垹绀傞柤娴嬫櫇椤旀洟姊洪悷閭﹀殶闁稿濮电粩鐔奉潨閳ь剟骞冩禒瀣垫晬婵炴垼椴搁悿浣糕攽椤旂》鍔熺紒顕呭灣缁參鎮㈤悡搴ｅ姦濡炪倖宸婚崑鎾淬亜椤撶偞绌挎い锕€缍婇幐濠傗攽閸ャ儰绨婚梺瑙勫劤瀹曨剟鎮橀埡鍐＜闁逞屽墯瀵板嫮浠︾粙澶稿闂佺绻愰ˇ顖涚妤ｅ啯鈷戦柛娑橈攻婢跺嫰鏌涢幘瀵告噰鐎规洘绮撻、姗€鎮欓埡鍌涙澑闂備胶绮崝鏇烆嚕閸洖鐓濋柡鍥╀紳閻熼偊鐓ラ柛鏇ㄥ幘閻ゅ嫰姊?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕绀傞柣鎾崇岸閸嬫挾绱掑Ο鍦畾濡炪倖鐗楃换鍐敂閻樿褰掓偐鐏炲倸浠┑顔硷攻濡炶棄鐣烽锕€绀嬫い鎺戭槹椤ワ絾绻濈喊妯活潑闁稿鎳愰幑銏ゅ醇濠垫劕娈ㄦ繝鐢靛У绾板秹宕戦崟顖涚厱闊洦鎸惧В鐐烘煙瀹勯偊鐓奸柡宀嬬稻閹棃濡搁敂浠嬫暘濠电姰鍨婚幊鎾绘晝閵堝懎绁俊鐐€栧ú宥夊磻閹惧灈鍋撳▓鍨灈闁绘牕銈稿畷娲晸閻樿尙锛滃┑鐘诧工閹虫劙寮舵禒瀣拻濞达絽鎲￠幆鍫ユ煟椤掆偓閵堢鐣锋导鏉戝唨鐟滃繘寮抽敂鎴掔箚闁靛牆鎳忛崳娲煕閵堝懎鏆ｉ柡灞界Ч瀹曨偊宕熼鐔蜂壕鐟滅増甯╅弫濠囨煕閹伴潧鏋熼柍閿嬪笒闇夐柨婵嗘噺閸熺偤鏌涢悢鍝勪槐闁哄矉绱曟禒锔炬嫚閹绘帩娼庨梻浣筋嚃閸ㄤ即藝閻㈢绠犻柡宥庡幖閻撴稑霉閿濆懏鎲稿ù婊勬緲閳规垿鎮╅崹顐ｆ瘎婵犳鍠氶弫濠氬箠濠靛绠氱憸搴ㄥ汲濠婂牊鈷掗柛灞剧懅椤︼箓鏌涢悢閿嬪仴妤犵偞鐗犻、鏇㈡晝閳ь剛澹曡ぐ鎺撶厵闂傚倸顕ˇ锕傛煟閹烘垹浠涢柕鍥у楠炴帡骞嬪┑鍐ㄤ壕鐟滅増甯掑Ч鍙夈亜閹烘垵顏柣鎾寸洴閹﹢鎮欐０婵嗘婵犵鈧偨鍋㈤柡灞界Ф閹叉挳宕熼銈勭礉闁诲氦顫夊ú鏍х暦椤掑嫬鐓″鑸靛姇缁犮儱霉閿濆娅滃瑙勬礀閳规垶骞婇柛濠冩礋楠炲﹨绠涘☉娆忎簵闂佸搫娲ㄩ崰宥囨崲閸℃ǜ浜滈柡宥冨妿閻矂鎮介娑氭创闁诡喗顭堢粻娑㈠Ψ瑜忛弳銈夋⒑閸濆嫮鐒跨紒杈ㄦ礃缁傛帡鏁冮崒娑樹患闁诲繒鍋炵缓鍧楀磿閺屻儱桅闁告洦鍣Σ铏圭磽娴ｈ棄鐓愮€光偓缁嬫鍤曢悹鍥ㄧゴ濡插牓鏌曡箛鏇炐ユい鏃€甯炵槐鎾诲磼濮樺崬鈪辨繝娈垮枛绾绢參鍩€椤掍胶鈻撻柡鍛Т椤繐煤椤忓嫭宓嶅銈嗘尵婵敻骞夊鈧铏圭矙濞嗘儳鍓板┑鈽嗗亝缁嬫垵宓勯梺鍦濠㈡绮绘繝姘€垫繛鎴烆仾椤忓牜鏁侀柟鐐灱閺€鑺ャ亜閺冨倹娅曠紒鐘虫崌閺屻倕煤鐠囪尙浠稿銈冨灪濡啴銆佸鈧慨鈧柍銉︽灱閸嬫捇鎮介崨濠備画濠电偛妫楃换鎰邦敂椤忓嫷鐔嗛悹鍝勬惈椤忣參鏌＄仦鍓ф创妤犵偞顭囬幑鍕倻濡棿閭梺璇叉唉椤煤濡警娓诲ù鐘差儏缁犳牗绻涢崱妯诲碍濞磋偐濞€閺屾盯寮撮妸銉ュ闂佸憡鑹鹃幊妯侯潖缂佹ɑ濯撮柣鐔煎亰閸ゅ鈹戦悙鏉戠祷缂佸鎳撻悾鐑藉箣閿曗偓缁犺崵绱撴担璇＄劷闁告ü绮欏楦裤亹閹烘垳鍠婇梺鍛婃尪閸旀垿鎮￠鍕垫晢闁告洦鍓涢崢鎼佹煟韫囨洖浠ч柛瀣尵缁牓宕橀鍡欙紲缂傚倷鐒﹂…鍥╃不濮樿京纾奸柣妯虹－濞插鈧鍠楅幐鎶藉箖閳哄懏鎯炴い鎰靛亜閺呯姴鈹戦悩娈挎殰缂佽鲸娲熷畷鎴﹀箣閿曗偓绾惧綊鏌￠崶鈺佇ｇ€规洘鐓￠幃妤呮晲鎼粹剝鐏堢紒缁㈠幐閸嬫捇姊绘担鍛婂暈闁告梹娲栬灒濠电姴鍋嗛悞?DoH/nginx 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊绋掑婵堢磼閳锯偓閸嬫捇姊绘担渚劸闁哄牜鍓涢崚鎺戠暆閸旇偐鍏橀崺鈧い鎺戝閳锋帒霉閿濆懏鍟為柟顖氱墦閺岀喎鐣￠柇锕€鍓辩紓浣稿€哥粔鎾煘閹达箑閱囬柣鏂垮槻缁ㄣ儲绻濋悽闈涒枅婵炰匠鍥舵晞闁糕剝绋戠壕濠氭煏婵炵偓娅嗛柍閿嬪浮閺屾盯顢曢妶鍛€婚梺鎼炲妺閻掞妇鎹㈠☉銏犵婵炲棗绻掓禒楣冩⒑缂佹ɑ灏版繛鑼枛楠炲啫螖閸涱喗娅滈柟鑲╄ˉ閳ь剝灏欓崙褰掓⒑鐠囪尙绠扮紒璇差儔瀹曪繝骞庨挊澶庢憰闂佸搫娲㈤崹褰掓倷婵犲嫭鍠愮€广儱顦介弫鍌涖亜閹烘垵鈧崵澹曟總鍛婄厽婵☆垵娅ｉ敍宥嗙箾閹绘帞鎽犳い銊ｅ劦閹瑩骞栭鐘插Ш闂備礁婀遍幊鎾垛偓姘煎幘缁參鎮㈢喊杈ㄦ櫖濠电偞鍨堕悷褏鏁崱娑欌拻闁稿本鑹鹃埀顒佹倐瀹曟劖顦版惔锝囩劶婵炴挻鍩冮崑鎾搭殽閻愬澧垫い銏℃礋閺佸啴鍩€椤掑倻涓嶆い鏍仦閸婄敻鏌ㄥ┑鍡涱€楅柡瀣〒缁辨帡鍩€椤掑嫬閱囬柕澶涘閸樹粙姊洪崫鍕殭闁稿﹤鎽滈弫顕€宕滄担铏癸紲闂佺粯锚绾绢厽鏅堕鍕厵濞撴艾鐏濇俊鍏笺亜椤忓嫬鏆熼柟椋庡█閻擃偊顢橀悜鍡橆棥闂傚倸鍊搁崐宄懊归崶顒夋晪闁哄稁鍘肩粣妤佷繆閵堝懏鍣洪柛瀣剁節閺屽秹宕崟顒€娅ｇ紓浣哄Х婵數鎹㈠┑鍥╃瘈闁稿本纰嶅▓婊堟⒑閸濆嫷鍎愭俊顐㈠暣瀵鎮㈤搹鍦厯闂佸壊鐓堥崰姘ｉ弶搴撴斀闁宠棄妫楁禍婊堟倵濮橆厽绶叉い顐㈢箲缁绘繂顫濋鍌︾床婵犵數鍋涘Λ娆戞暜閹烘鍌ㄩ柨娑樺绾捐棄銆掑顒佹悙婵炲懏锕㈤弻娑㈠Ω閿曗偓閳绘洜鈧鍣崑濠囥€佸璺虹劦妞ゆ帒瀚畵渚€鏌熼悜妯烘闁哄啫鐗嗛悞鍨亜閹哄棗浜惧┑鐐村絻濞尖€愁潖缂佹ɑ濯撮柛婵勫劤妤旀繝纰樻閸嬪鈻旈弴鐘插灊闁割偆鍠愭刊鎾煣韫囨稈鍋撳☉姘辨喒闂傚倷绀侀幖顐ょ矙娓氣偓瀹曘垺绂掔€ｎ亞锛涢梺鏂ユ櫅閸熸壆绮绘ィ鍐╃厱闁斥晛鍠氬▓妯肩磼閻橀潧顣奸柕鍥у缁犳盯鏁愰崟顓犳晨闂備礁鐤囬～澶愬垂閸фぜ鈧礁鈽夊Ο閿嬵潔濠电偛妫楃换鎰板绩椤撱垺鈷掑ù锝呮啞閹牓鏌￠崼顐㈠缂侇喗鐟╅獮瀣晝閳ь剟鎮″┑瀣婵烇綆鍓欓埀顒侇殜閹潡鍩€椤掑嫭鈷戦柟绋挎捣缁犳捇鏌＄仦鏂ら練妞わ箒顫夐幈銊︾節閸涱噮浠╃紓浣介哺鐢帟鐏掗梺鍏肩ゴ閺呮繈寮抽锔解拻濞达綀娅ｇ敮娑欍亜閵娿儳澧涢柟渚垮姂楠炴﹢顢氶崱娆戔棨闂傚鍋勫ú锔剧矙閹烘纾圭紓浣姑肩换鍡樸亜閺嶃劎绠ラ柛銈嗙懄閵囧嫰顢曢悢宄扳拤缂備胶绮粙鎾诲焵椤掑倹鏆╂い顓炵墢閺侇噣宕奸弴鐔哄幗闁圭儤濞婂畷婵嬪箣閿旇　鍋撴笟鈧獮妯兼嫚閹绘帒鏁ゆ俊鐐€栭幐楣冨疮閸ф绠柧蹇撴贡绾捐棄霉閿濆懎顥忛柛搴＄箻閹绠涢幘鏉戞畻閻庢鍠撻崝鎴﹀箠閿熺姴围闁搞儮鏅槐鏌ユ⒑閼姐倕孝婵炴祴鏅濇禍绋库枎閹邦厽鐝烽梺鎸庢礀閸婂綊鎮″▎鎰╀簻闁哄啫娴傞崵娆戔偓瑙勬尭濡瑩濡甸崟顖氼潊闁挎稑瀚崳浼存⒑閸濆嫯顫﹂柛鏃€鍨块妴浣糕槈濮楀棙鍍靛銈嗗姂閸╁嫬螞濠婂牊鈷掗柛灞剧懅閸斿秹鎮楃粭娑樻噺瀹曟煡鏌涚仦鍙ョ繁婵炲牅绮欓弻鐔兼⒒鐎靛壊妲梺鍛婂灩婵炩偓闁哄矉缍佸顒勫垂椤旇棄鈧垶姊虹紒妯诲鞍闁荤啿鏅犻獮鍐潨閳ь剟骞冨▎鎾崇闁瑰搫妫楁禍楣冩煙闂傚顦︾紒鈧径鎰叆闁哄洨鍋涢埀顒€缍婇幃鈥斥槈閵忊€斥偓鍫曟煟閹邦厼绲婚柍閿嬫閺屾洟宕卞Ο铏圭懆闂傚洤顦甸弻锝呪枎韫囨柨娅ч梺浼欑悼閺佹悂鍩€椤掑喚娼愭繛鍙夘焽閸掓帟绠涘ù鏉挎喘楠炲酣鎳為妷褍骞堥梻浣瑰缁诲倹顨ョ粙搴撴瀺鐎广儱妫欓崣蹇撯攽閻樻彃浜為柣鎾瑰亹閳ь剝顫夊ú妯兼暜閹烘缍栨繝闈涱儏缁€鍌氼熆閸撲焦顏熷┑鈥虫川濡叉劙骞樼€涙ê顎撻梺鍛婃尭瀵埖顨欓梻鍌欐祰濡椼劑鎮為敃鍌氱畺闁稿瞼鍋熷畵渚€鏌熸潏鍓х暠妞ゎ偄鎳橀弻锝咁潨閸℃ぞ绨煎銈庡亜缁夌懓顫忓ú顏咁棃婵炴垶姘ㄩ濠冪節濞堝灝鏋ら柛蹇旓耿閵嗕礁螣婵傝姤妞介、鏃堝川椤忓懎顏圭紓鍌氬€搁崐鐑芥倿閿曞倹鏅┑鐘愁問閸犳牠鎮ф繝鍕床婵炴垯鍨圭粈鍌炴煠濞村娅嗛柛锝勫嵆閹鈻撻崹顔界亪闂佸摜鍠曢崕濠氭倶閸愵喗鈷戦柛娑橈梗缁堕亶鏌涢悩鍐叉诞妤犵偛绻橀幖褰掑捶椤撶媴绱冲┑鐐舵彧缁茶姤绔熸繝鍥х？闁规儳顕粻鎯ь熆鐠轰警鍎愮紒鈧€ｎ兘鍋撳▓鍨灆闁告鍟块悾鐑藉箳濡や礁鈧兘鎮楅悽娈跨劸妤犵偛鐗嗛埞鎴︽偐閸偅姣勬繝娈垮枟閹稿啿鐣烽鐐茬妞ゆ柨褰炵粭澶愭⒑閸︻厼浜鹃柣锝庝簼缁傚秴顭ㄩ崼鐔哄幐闂佹悶鍎插﹢鍦姳閻ｅ瞼纾奸悗锝庡幖閸氬湱绱掓潏銊ョ缂佽鲸甯掕灒闁兼祴鏅濋弳銈嗕繆閻愵亜鈧倝宕戦崨顖涘床闁割偁鍎埀顑跨铻栭柛娑卞幘閿涙粌鈹戦鏂ゅ叕缂佽尪濮ょ粋宥嗐偅閸愨晝鍘介梺褰掑亰閸犳牠宕濈€ｎ喗鐓冮梺鍨儏閻忔挳鏌＄仦鍓ф创妤犵偛娲、妤呭焵椤掑倻妫憸鏃堝蓟閻旂⒈鏁嶆慨妯哄船閳敻姊洪崫鍕拱闁烩晩鍨堕獮鍐閵忊€崇／闂佸憡绻傜€氼噣濡堕鈧缁樻媴閼恒儯鈧啰绱掗埀顒佹媴閾忓湱鐓嬮悷婊呭鐢帞绮婚弽銊х闁糕剝蓱鐏忎即鏌涙繝鍕笡濞ｅ洤锕、娑㈡倷鐎涙ɑ娈搁梺璇插閼归箖濡剁粙娆炬綎缂備焦蓱婵挳鏌﹀Ο渚Ц闁诡垳鍋ゅ娲传閵夈儛锝夋煕閺冣偓閻楁粎鍒掗埡鍛亜闁绘挸楠搁懓鍨攽閻愭潙鐏︽い顓炴喘椤㈡瑩骞嬮敂瑙ｆ嫼缂傚倷鐒﹂敃鈺呮倿閻ｅ瞼纾兼い鏇炴噹閻忥箓鏌熼鍡欑瘈闁诡喓鍨藉畷妤呮嚃閳轰礁绠洪梻鍌欑閹碱偊宕悩璇茬；闁规儳顕粻楣冩煕韫囨艾浜归柟鍐叉喘閺岀喖顢欓弬銈堚偓鍧楁煙瀹曞洤鈻堟い銏＄墵閹稿﹥寰勬径濠庢?
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


// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婄婵犲灚鍔栫紞妤呮⒑闁偛鑻晶顕€鏌涙繝鍌涘仴妤犵偞鍔栫换婵嬪礃椤忓棗楠勯梻浣稿暱閹碱偊顢栭崶鈺冪煋妞ゆ棃鏁崑鎾舵喆閸曨剛锛橀梺鍛婃⒐閸ㄧ敻顢氶敐澶婇唶闁哄洨鍋熼娲⒑缂佹鎳冮柟铏姍閻涱噣濮€閵堝棌鎷婚梺绋挎湰閻熝呯玻閺冨牊鐓冪憸婊堝礈濞戙垹纾绘繛鎴欏灪閸ゆ劖銇勯弽銊р姇婵炲懐濞€閺岀喓绱掗姀鐘崇亶闂佺粯鎸婚惄顖炲箖濮椻偓閹瑩骞撻幒鍡樺瘱闂備線娼уΛ娆戞暜閿熺姴钃熺€广儱鐗滃銊╂⒑閸涘﹥灏柛鏂挎捣濡叉劙鎮欓崫鍕€垮┑鐐村灦椤洭鏁嶅☉銏♀拺闁告稑锕ｇ欢閬嶆煕濮椻偓缁犳牕顕ｉ幎鑺ユ櫆闂佹鍨版禍楣冩煕韫囨搩妲稿ù婊堢畺濮婃椽宕ㄦ繝鍐槱闂佸憡鎸婚惄顖氱暦閵忋倕绠瑰ù锝囨嚀閳ь剛鏁婚弻娑滅疀閹垮啯笑婵炲瓨绮撶粻鏍蓟閿濆棙鍎熸い鏍ㄧ矌鏍￠梻浣告啞閹稿鎯勯鐐叉瀬鐎广儱顦伴崑鍕煕濠靛嫬鍔ゆい鏃€娲熼弻锝嗘償閿濆棙姣勫銈庡幖閻楁捇銆侀幘璇茬闁哄倶鍎查弬鈧梻浣哄仺閸庢潙鈻嶉弴銏犵獥婵☆垱妞垮▓浠嬫煟閹邦垰鐨虹紒鐘差煼閺岀喖顢欑憴鍕彇闂佸綊顥撴繛鈧柟宕囧█椤㈡寰勬繝鍐╂緫闂傚倸鍊搁崐椋庣矆娴ｉ潻鑰块梺顒€绉寸壕鍧楁煏閸繍妲搁柡鍕╁劦閺屾盯顢曢敐鍥╊吋闂佺粯甯婄划娆撳蓟閺囩喎绶為柛顐ｇ箓婵姊虹悰鈥充壕婵炲濮撮鍡涙偂閻旈晲绻嗘い鏍ㄧ箖椤忕姴霉閻樺磭娲撮柡宀嬬節瀹曡精绠涢弮鈧悵姘舵⒑閸濆嫭婀扮紒瀣崌閸┾偓妞ゆ帒锕︾粔鐢告煕閹惧鎳勭紒鍌涘浮閸╋繝宕ㄩ鎯у妇闂備焦鎮堕崕顖炲礉鐏炵偓鍙忕€广儱娲犻崑鎾舵喆閸曨剛顦ㄩ梺鎸庢磸閸ㄤ粙宕规ィ鍐╂櫇闁稿本姘ㄩ娲⒑缂佹〞鎴ｃ亹閸愨晩娈梻鍌氬€搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ礀绾惧鏌曟繛鐐珔缁炬儳娼￠弻锛勪沪鐠囨彃濮庣紓浣插亾闁割偆鍠愰崣蹇斾繆椤栨稑顕滅痪顓㈢畺閺屾稑顫濋鍌溞ㄩ梺鍝勭焿缁绘繂鐣烽崼鏇炵厸濞达絽婀遍埀顒夊幗缁绘稓鈧稒顭囬惌瀣磼椤旇姤宕岀€殿喖顭烽幃銏ゅ礂閻撳簶鍋撶紒妯圭箚妞ゆ牗绻冮鐘裁归悩铏唉婵﹥妞介弻鍛存倷閼艰泛顏繝鈷€灞芥灓缂佽鲸鎸搁濂稿椽閸愩劋绱ｆ俊鐐€栧ú鈺冪礊娴ｅ摜鏆︽俊顖欒閸熷懏銇勯弮鈧崕鍐差嚕閹惰姤鈷掑ù锝呮啞閸熺偞绻涢幓鎺斝х€殿喗褰冮埢搴ㄥ箣閿濆憛鈩冪節閻㈤潧校缁炬澘绉归崺娑㈠箣閿旂晫鍘卞┑鐘绘涧濡顢旈埡鍛厓鐟滄粓宕滃璺虹畺闁稿瞼鍋涚粻鏍ㄤ繆椤栨縿鈧偓闁衡偓娴犲鍊甸柨婵嗙凹缁ㄤ粙鏌涘鈧禍璺侯潖濞差亜浼犻柛鏇ㄥ亝濞堫參姊虹紒姗嗘當闁挎洦浜獮鍐晸閻樺啿浜滈梺鍛婄缚閸庢娊顢欓弴銏♀拺闁荤喖鍋婇崵鐔兼煕鐎ｎ剙鏋戦柡鍛埣椤㈡棃宕奸悢鍝勫箞闂備礁鎼崯顖炲箰妤ｅ啯鍋╅柣銏犳啞閻?sniroute.cmd update check闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栭梻鍌欑閹诧繝骞愰崱娑樺窛妞ゆ梻鐡旈崯鍛存⒒閸屾瑦绁版い鏇嗗應鍋撳☉鎺撴珚闁硅櫕绻冮妶锝夊礂椤栨碍澶勯悗闈涖偢瀵爼骞婄粵鍦暤闁哄本鐩鎾Ω閵壯傚摋缂傚倷鑳舵慨瀵稿椤撱垹鐒垫い鎺嗗亾缂佺姴绉瑰畷鏇㈠础閻愬秵鐩、姗€鎮欏鍥ф灈鐎规洘甯掗…銊╁川椤栥倗闂繝鐢靛仩閹活亞寰婇崸妤佸仱闁哄啫鐗嗛崥瑙勭箾閸℃ê濮冪紒璇叉閵囧嫰骞囬崜浣瑰仹缂備胶濮烽崑鐔煎焵椤掍緡鍟忛柛鐘崇洴椤㈡俺顦归柛鈹垮劜瀵板嫭绻涢姀銏犳灈闁哄被鍔庨埀顒婄到閻忔氨绱炵仦瑙ｆ斀闁绘ɑ鍓氶崯蹇涙煕閻樻剚娈滈柕鍡楀暣瀹曘劎鈧稒锚娴滆鲸绻濋悽闈浶㈡繛灞傚€濆鍛婄瑹閳ь剟寮诲☉銏犵婵°倐鍋撻柟铏姍閺佸秴鈹戠€ｎ偀鎷虹紓浣割儐鐎笛冿耿閻楀牊鍙忔俊顖滎焾婵″ジ鏌ｉ埥鍡楀箺缂佺粯绻堥幃浠嬫濞戞鎹曟繝纰樻閸ㄤ即鏁冮鍕殾闁挎繂顦粈鍐┿亜椤撶喎鐏ユ繛鍫涘妽缁绘繈鎮介棃娴讹絿鐥弶璺ㄐх€规洘鍨垮畷鐔碱敍濞戞艾骞楅梻浣筋潐瀹曟ê鈻嶉弴銏犵闁挎繂顦伴悡鐔兼煥濠靛棭妲洪柣锝嗘そ閺岀喖顢欓悡搴⑿╁銈庡幑閸旀垵鐣峰鈧、鎾活敍閸℃鍣抽柛瀣尭閳绘捇宕归鐣屼粚婵犵數鍋涢幊蹇撁洪弽顓涒偓锕傚炊椤忓棛鏉稿┑鐐村灦椤倿鍩￠崨顔惧弳闂佸搫娲﹂〃鍛妤ｅ啯鍊垫繛鍫濈仢閺嬫稒绻涚亸鏍ゅ亾閹颁焦缍庡┑鐐叉▕娴滄粎绮绘导鏉戠閺夊牆澧介幃濂告煟閿濆洦鏆慨濠冩そ閹兘寮堕幐骞晛顪冮妶鍡楃仴闁硅櫕鎹囬幃姗€骞掑Δ浣叉嫼闂備緡鍋嗛崑娑㈡嚐椤栨稒娅犻悗娑櫳戦崣蹇撯攽閻樺弶鍣烘い蹇曞█閺岀喐顦版惔鈾€鏋呴梺鎼炲姂缁犳牠骞栬ぐ鎺濇晝闁靛牆瀚伴崑妤冪磽閸屾艾鈧悂宕愰幖浣哥９闁告縿鍎抽惌鎾绘煙缂併垹鏋熼柡鍛箖閵囧嫯绠涢幘璺侯杸闂佺顑嗚摫闁靛洤瀚板鏉懳旈埀顒佺妤ｅ啯鍊甸悷娆忓缁€鈧梺缁樼墪閵堟悂濡存担鑲濇梹鎷呴崫銉х嵁闂佽鍑界紞鍡涘磻閸涘瓨鍋熸繝闈涱儐閳锋帡鏌涚仦鍓ф噯闁稿繐鐬肩槐鎺楊敋閸涱厾浠╅柧鑽ゅ仜闇夐柨婵嗘川閵嗗﹪鏌＄€ｎ亞效闁哄被鍔岄埞鎴﹀幢閳哄倐锕傛⒑缂佹ɑ灏扮紒璇茬墦閹繝顢曢敃鈧悙濠囨煏婵炑€鍋撳┑顔兼喘濮婃椽骞庨懞銉︽殸闁汇埄鍨辩敮鈥筹耿娓氣偓濮婃椽骞愭惔锝囩暤闂佺粯顨呴敃顏堛€侀弮鍫熷亹缂備焦顭囬崢顏堟⒑缁洖澧叉い銊﹀姍閻擃剟顢楅崟顒傚幐閻庡厜鍋撻悗锝庡墰閻﹀牓鎮楃憴鍕８闁告梹鍨块妴浣糕枎閹炬潙鈧攱銇勯幒鍡椾壕闂佹悶鍔戞禍璺侯潖缂佹ɑ濯撮柣鎴灻▓宀€绱掗崜褑妾搁柛娆忓暙閻ｉ攱瀵奸弶鎴濆敤濡炪倖鎸绘竟鏇㈠磻閹惧瓨濯寸紒顖涙礃椤秹姊洪棃娑氱畾闁逞屽墯閺嬪ジ宕戦幘瀵哥瘈婵﹩鍘鹃崢閬嶆⒑闂堟侗妲堕柛銊ユ惈閳诲秹宕堕浣哄帗缂備焦顨呴幖顐﹀焵椤掍胶澧遍柟骞垮灩閳规垹鈧綆鍋勬禒娲⒒閸屾氨澧涚紒瀣墦瀵悂宕ㄩ娑欐杸闂佺粯鍔栧妯间焊閸愵喗鐓曢柕澶堝妼閻撴劙鏌￠崨顖氫槐婵﹥妞藉畷銊︾節娴ｈ櫣绠掗梻浣规偠閸斿瞼澹曢鐔侯洸闁归棿鐒﹂崑銊х棯閻楀煫顏呯閻愵剦娈介柣鎰皺娴犮垽鏌涢弮鈧喊宥夊Φ閸曨垱鏅滈柛顭戝枛缁侇噣姊虹拠鈥虫珯缂佺粯绻傞悾宄邦潨閳ь剟銆侀弮鍫濆窛妞ゆ挾鍠撻埀顒傚亾缁绘繈鎮介棃娴躲垽鏌ㄩ弴妯衡偓婵嬪箖瑜斿畷鍗炩枎閹寸姷鍔堕梻浣稿閸嬩線宕曢弻銉﹀亗闁绘柨鍚嬮悡娑㈡煕閹扳晛濡垮褑椴告穱濠囶敃閿濆懐浼岄梺鍝勭焿缁绘繂鐣烽柆宥庢晣闁绘垵妫欓崰姗€姊绘笟鈧埀顒傚仜閼活垱鏅舵导瀛樼厵闁惧浚鍋呯亸鐢告煃瑜滈崜姘跺礄瑜版帒鐭楅柛鎰╁妿閺嗭箓鏌熼幆鐗堫棄缂佺嫏鍥ㄧ厵闁绘劘妫勬俊鑺ヤ繆閻愭壆鐭欓柕鍡曠窔瀵剙鈻庨幆褎鍊梻浣虹《閸撴繆鎽梺缁樼箖濞茬喎顫忔繝姘＜婵炲棙鍩堝Σ顕€姊虹憴鍕偞闁告挻绻勭划顓㈡偄閼茬儤妫冨畷銊╊敇閻愮鍋撻銏♀拺閻熸瑥瀚烽崯蹇涙煕閻樻剚娈滅€殿喖鍟块…銊╁醇閻斿搫骞嶉梻浣告啞閹稿棝宕ㄩ鐙€鍋ч梻鍌欑劍婵炲﹪寮ㄩ柆宥呭瀭闁秆勵殔缁犳牜鎲搁悧鍫濈瑨闁绘劕锕ラ妵鍕疀閹炬剚浼€濠殿噯绲婚崹鑽ゆ閹惧瓨濯撮柛婵嗗珔閿濆應鏀介柣鎰嚋闊剛鈧鍠涢褔鍩ユ径濠庢僵闁挎繂鎳嶆竟鏇㈡⒑閹稿海绠撳Δ鐘叉啞閺呭爼顢氶埀顒勫蓟閿涘嫧鍋撻敐搴′簽闁靛棙甯￠弻宥堫檨闁告挻绻堥敐鐐村緞婵炴帒鎼灒濠靛倽濮ゅ浠嬪箖閵忋倖鍋傞幖杈剧悼閺嗩偊姊绘担椋庝覆缂佽弓绮欓幆澶嬬附閸涘﹤鍓瑰┑鐐叉閹稿宕戦敐鍛偓鎺戭潩閻撳海浠梺鍛娚戠划搴ｆ?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁绘劦鍓欓崝銈嗙節閳ь剚娼忛埡鍌ゆ綗闂佸湱鍎ら弻锟犲磻閹剧粯鏅查幖绮光偓鎻掝棜缂傚倷鐒︽晶搴ㄥ疾濠婂懏宕叉繛鎴烇供閸熷懏銇勯弮鍥у惞闁告垵缍婂铏圭矙濞嗘儳鍓遍梺鍛婃⒐閻熲晠鐛崘顓滀汗闁圭儤鍨归崐鐐差渻閵堝懐绠伴悗姘煎枛琚欓柕蹇嬪€栭埛鎴︽煙缁嬫寧鎹ｉ柍顖涙礋閺岋綁鍩℃繝鍌滀哗濡炪値鍋勭换鎰弲濡炪倕绻愰幊搴㈢椤撱垺鈷戦柛婵嗗濡插摜绱掗妸鈺€鎲炬鐐村姍瀹曟﹢顢旈崱娆欑闯濠电偞鎸婚懝鎯洪妶鍛瀺婵せ鍋撻柡灞剧洴婵℃悂濡搁敂淇扁偓鎰版⒑娴兼瑧鎮奸柛蹇旓耿閻涱喚鈧綆鍠楅弲婊堟煠閹帒鍔滄い蹇曞枛濮婄粯鎷呴搹鐟扮闂佹寧姘ㄩ惀顏嗙磼閵忕姴绫嶉悗瑙勬磸閸ㄤ粙鐛弽銊﹀闁稿繐顦扮€氬ジ姊绘担渚敯闁稿鍔欏畷鎴濃槈閵忕姷顦┑顔姐仜閸嬫捇鏌＄仦鍓ф创鐎殿噮鍓熷畷绋课旈埀顒€顕ｆ导瀛樷拺闂傚牊绋掗ˉ婊堟煕婵犲倹鍋ユ鐐诧躬閹晛煤缂佹ɑ娅岄梻浣侯焾閺堫剛鍒掗敐鍥潟闁圭粯宸婚弨浠嬫煟閹邦垰鐓愮憸鎶婂懐纾奸棅顐幘閻瑦顨ラ悙瀵稿⒌妞ゃ垺娲熼弫鍌滄偘閳ュ厖澹曢梺绋挎湰缁嬫捇姊介崟顖涚厱婵炴垶锕崝鐔兼煕濮楀棔閭慨濠冩そ楠炴牠鎮欓幓鎺懳楅梻浣告啞閸旀洝銇愰悙鍨潟闁哄啫鐗婇悡鐔煎箹濞ｎ剙鐒洪柛鐔风箻閺屾盯鏁愭惔鈩冪彎閻庤娲樻繛濠囧极閹邦厼绶炲┑鐘插暞琚ｉ梻鍌欑窔閳ь剛鍋涢懟顖涙櫠椤栫偞鐓忛柛銉戝喚浼冨Δ鐘靛仦鐢繝鐛€ｎ亖鏀介柟閭︿簽濡垶姊婚崒娆掑厡妞ゎ厼鐗撻、鏍川閹碱厽鏅炴繝銏ｅ煐閸旀洟鎷戦悢鍏肩叆婵犻潧妫欓崯鎺楁煟閺傚灝鎮戦柛濠勭帛娣囧﹪顢涢悙瀛樻殸闂佽绻愬Λ娆戞崲?GitHub API闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栨繝寰锋澘鈧呭緤娴犲鐤い鏍剱閺佷胶鈧箍鍎遍ˇ浼村煕閹寸姷纾奸悗锝庡亽閸庛儵鏌涙惔锛勭闁靛洤瀚伴獮瀣攽閹邦厸鏋呴梻浣筋嚃閸ｎ垳鎹㈠┑瀣ㄢ偓渚€寮撮姀鈩冩珖闂侀€炲苯澧撮柟顔界懄缁绘繈宕堕妸褍甯惧┑鐘垫暩閸嬫盯鎮樺┑瀣婵鍩栭悡娑㈡倵閿濆骸浜為柟鍐叉噽缁辨帗娼忛妸锕€纾抽悗瑙勬礃鐢帡锝炲┑鍠版帒鈻庡▎鎴犘滈梺鍝勭焿缁插€熺亙闂佸憡鍔戦崝澶愬几閸涘瓨鍊垫繛鍫濈仢閺嬶附銇勯弴鍡楁搐閻撯€愁熆閼搁潧濮囨い顐㈡嚇閺岋絽顫滈埀顒€顭囪閹便劑宕奸弴鐔叉嫽婵炶揪绲介幉锟犲疮閻愮儤鐓曢柡鍐╂崌濡绢喚绱掗崒姘毙х€殿喕绮欓垾鏍敆婵犲嫮袦閻庤娲栭妶鍛婁繆閻戣姤鏅滈柟顖嗗啰顔戦梻鍌欒兌閹虫捇宕ョ€ｎ喗鍋╂い蹇撶墕閸ㄥ倿鏌涜箛姘汗闁绘繆鍩栨穱濠囶敍濞戞碍鑿囨繛瀛樼矋缁捇寮婚妸鈺佺睄闁搞儴娉涘▓顓㈡⒑鐠囨煡鐛滅紒鐘虫崌瀵顓兼径濞劍銇勯弽銊ュ毈婵炲樊鍙冨娲捶椤撶喎娈岄柣搴㈡皑婢ф绌辨繝鍥х濞达綀顫夊▍鍥⒑闁偛鑻晶鏉款熆鐟欏嫭绀嬫い銏＄☉閳藉顫濇鏍ф櫗闂傚倷绀佸﹢閬嶅磻閹炬剚鐒芥繛鍡樻尭閸氳銇勯幒鎴濐仾闁绘挻娲熼弻鏇熺箾閸喖顫囬梺鍛婄箘閸庛倝濡甸崟顖毼╅柨鏃傚亾閸Ｑ囨⒑鐎圭媭娼愰柛銊ユ健楠炲啴鍩￠崨顓炵€銈嗗姧缁查箖鎯佹惔銊︹拻濞达絼璀﹂悞鐐亜閹存繃顥㈤柟顖氬椤㈡稑鈹戦崶鑸碉紙婵犵绱曢崑鎴﹀磹閺嶎厼绠伴柛顐ｆ礀閸ㄥ倻鎲搁悧鍫濈瑨闁搞劌鍊块弻娑氫沪缂併垹娈ч悷婊呭鐢宕戦幇顔瑰亾閻熸澘顥忛柛鐘崇墵瀹曟粓骞庨懞銉モ偓鐢告偡濞嗗繐顏悘蹇ｅ亞缁辨帡鎳滈棃娑樻懙閻庢鍠栭…鐑藉垂妤ｅ啫绠涘ù锝呮啞閸婎垶姊虹涵鍛汗閻炴稏鍎甸崺鈧い鎺戝暙閻ㄦ椽鏌ｉ弽顓濇喚婵﹦绮幏鍛瑹椤栨粌濮兼俊鐐€栭崹闈浳涘┑瀣祦闁圭増婢樼粻鐟懊归敐鍛喐闁告瑢鍋撴繝鐢靛О閸ㄧ厧鈻斿☉婊冾杺闂備礁鎼鍛矓瑜版帒钃熺憸鎴犵不濞戙垺鏅查柛娑卞墰閻╁酣姊绘担绛嬪殭缂佽鐗撳畷鏇㈡偨缁嬭儻鎽曢梺鎸庣箓椤︿即寮查弻銉︾厱闁靛鍨甸崯顐ょ不閿濆鐓熼幖娣€ゅ鎰箾閸欏鐭掔€规洑鍗冲浠嬵敇濠ф儳浜惧〒姘ｅ亾鐎殿喕绮欓、姗€鎮欓悽闈涘箑闂傚倷绶氬褔鏁嶈箛娑樺窛妞ゆ棁濮ら悵銈夋⒒閸屾艾鈧悂宕愰幖浣哥９濡炲娴烽惌鍡椼€掑锝呬壕閻庤娲﹂崹璺虹暦缁嬭鏃堝焵椤掑倻涓嶉悷娆忓娴滄粓鏌熼幏灞剧【闁伙附绮撻弻鈩冩媴缁嬫浼冮梺鍝勬湰缁嬫垿鍩ユ径濞炬瀻闁瑰鍋為鍐⒒娓氣偓閳ь剛鍋涢懟顖涙櫠椤曗偓閺岀喖鎼归顒冣偓鍧楁煏閸℃洜顦﹂摶鏍煕濞戝崬骞樻い锔芥緲椤啴濡堕崱妤冪懆闂佺锕ラ幃鍌炴偘椤旂⒈鍚嬪璺侯儑閸樹粙鏌熼懝鐗堝涧缂佹煡绠栧畷鐢稿焵椤掑嫭鐓熼幖娣灮閻擃垶鏌涘Δ鈧崯鍧楁偩閻戣棄绠ｉ柣鎰暩閻﹀牓姊洪幖鐐插妧闁告粌鍟伴崥褰掓⒒閸屾瑧顦﹂柟纰卞亰瀹曟劙宕妷銊バ￠梻渚囧墮缁夊绱掗埡浼卞綊鎮╁顔煎壉闂佺粯鎸搁崯鎾蓟閺囩喎绶為柛顐ｇ箓婵垽姊洪幐搴ｂ槈缂佸缍婂璇测槈閵忕姷鍘搁柣搴秵閸撴岸宕戣濮婃椽鏌呴悙鑼跺濠⒀冨级閵囧嫰顢曢姀鈺傂﹂柣鎾卞€濋弻鈥愁吋鎼粹€崇闁搞儲鎸冲娲川婵犲嫧妲堥梺鎸庢磸閸庨潧鐣烽敐鍫㈢杸婵炴垶鐟ч崢鎾绘偡濠婂嫮鐭掔€规洘绮撻幃銏ゆ偂鎼淬倖鎲伴柣搴＄畭閸庨亶藝閹殿喒鍋撻悽闈浶㈡い顓℃硶閹瑰嫭绗熼姘婵犵妲呴崑鎾舵閺囩姵顫曢柟鎹愵嚙绾惧吋鎱ㄥΟ鑽ゆ▊闁靛繈鍊栭悡鐔搞亜閹捐泛浠滈柍褜鍓氶幃鍌炴晲閻愬墎鐤€闁哄啫鍊婚惁鍫濃攽椤旀枻渚涢柛鎾寸洴閺佸秴顭ㄩ崘锝嗘杸闂佺粯鍔曞鍫曀夐悙鐑樼厱闁靛鍎虫禒娑樓庨崶褝鏀荤紒鍌涘笧閳ь剨缍嗛崑鍡涘储娴犲鐓欓柤鍦瑜把呯磼閹绘帗鍋ユ鐐诧工铻栭柛娑卞枤閸樺崬鈹戦悙鏉戠仴鐎规洦鍓熼幃姗€鏁愭径瀣幗闂佽鍎抽悺銊ョ暦鐏炵偓鍙忓┑鐘叉噺椤忕娀鏌熼崣澶嬪唉鐎规洜鍠栧Λ鍐ㄢ槈濡ゅ嫮绠烽梻鍌欐祰椤曆呮崲閹烘纾绘繛鎴欏灩绾惧綊鏌涘☉姗堟敾闁告瑥绻橀弻娑樼暆閳ь剟宕戦悙鐑樺亗闁哄洢鍨洪悡鍐偡濞嗗繐顏╅柛鏂诲€曢湁婵犲﹤鎳庢禍鎯庨崶褝韬柟顔界懇椤㈡棃宕熼妸銉ゅ闂佸搫绋侀崑鍛村汲濠婂牊鐓欓柣鎴烇供濞堟梻鐥幆褏绉洪柡灞诲€栭幈銊╁箛椤戣棄浜鹃柡鍥ｆ嚍婢跺娼╂い鎴ｅГ閺傗偓闂備胶顭堥張顒傚垝閻樼粯鍊块柛鎾楀懐锛滈梺缁樼懄閻熴儱煤閿曞倹鍋傞柡鍥╁枂娴滄粓鏌熼弶鍨暢闁诡喖銈搁弻宥夋寠婢舵ɑ笑闂佸疇顫夐崹鍨瀶鏉堛劎绠惧璺侯儐缁€鍫熶繆閸欏濮嶆鐐村笒铻栧ù锝夋櫜濮规姊婚崒姘偓鎼佹偋婵犲嫮鐭欓柟閭﹀厴閺岋附銇勮箛鎾愁伌婵炴挸顭烽弻鏇㈠醇濠靛棙娈繝纰樺墲閹倿寮诲☉娆愬劅闁挎繂妫欏В鍫澪旈悩闈涗粶闁哥噥鍋婇崺銏℃償閵娿儳顔掗梺鍝勵槹閸ㄥ爼顢樼紒妯圭箚闁绘劦浜滈埀顒佺墵瀹曟椽宕奸妷銉э紮闂侀潧鐗嗛ˇ顖炴倷婵犲洦鐓忓┑鐐戝啫鏆欑紒鐘靛仱濮婂宕掑▎鎴М闂佹眹鍊曞ú顓㈡晲閻愭潙绶為柟閭﹀墰閿涚喖姊洪幆褎绂嬮柛瀣閵嗗懘寮婚妷锔惧幗闂侀€涘嵆濞佳囧储閸濄儳纾奸悹鍥у级椤ャ垽鏌″畝瀣М妤犵偛娲畷妤呭传閵壯勬櫒闂傚倷绶氶埀顒傚仜閼活垱鏅堕鐐寸厽婵°倓鑳堕惌鎺斺偓娈垮櫘閸嬪﹪銆佸▎鎾村仼鐎光偓閳ь剟鎯侀崼銉︹拺闁硅偐鍋涢崝姗€鏌涢弬鑳闁崇粯鎹囬獮瀣晜鐟欙絾瀚奸梻浣告啞閹告槒銇愰崘鈺冾洸婵犻潧娲ㄧ粻楣冩煙缂佹ê绗х紒鐘差煼閺屾盯鍩為幆褌澹曞┑锛勫亼閸婃牕顔忔繝姘；闁瑰墽绮悡娆撴煣韫囷絽浜鹃柤鐗堝椤ㄣ儵鎮欓幓鎺撴濡炪値鍘归崝鎴濈暦婵傚憡鍋勯梺鍨儑閺嬧偓闂傚倸鍊风欢姘焽瑜旈弫宥堢疀濞戞顦ㄩ梺闈涱焾閸庡搫煤椤撶喓绡€闁汇垽娼у暩闂佽桨绀侀幉锟犲箞閵婏箑顕遍柡澶嬪灥閻庮厽绻濋棃娑樷偓濠氣€﹂崼銉ュ強闁靛鏅滈悡娑氣偓骞垮劚妤犳悂鐛Δ鍛厱閹肩补鈧剚鍔夐梺闈涙搐鐎氭澘顕ｆ禒瀣╅柕澶堝劜閻︽捇姊绘担绛嬪殭婵﹫绠撳畷娲冀椤撶倣锕傛煕閺囥劌鐏犻柛妤勬珪娣囧﹪濡堕崒姘濠电偛鐡ㄧ划鎾剁不閺嵮屾綎婵炲樊浜滄导鐘绘煕閺囥劌浜芥俊鑼暬濮婃椽宕ㄦ繝鍐ㄩ瀺缂備浇顕ч崐鍧椼€佸璺何ㄩ柍杞拌兌椤︽澘顪冮妶鍡楃瑨闁稿﹤顭烽幆渚€宕奸妷锔规嫼闂佸憡绋戦敃銉﹀緞閸曨垱鐓曟俊顖濆吹閻帒鈹戦埄鍐╁€愰柛鈺嬬節瀹曟帒螖娴ｅ壊浼栭梻鍌氬€风粈渚€骞夐敍鍕床闁稿本澹曢崑鎾愁潩閻撳骸绠归梺鎸庢磸閸ㄤ粙鐛澶樻晩闁告挆宥囧簥闂備浇顕ч崙鐣岀礊閸℃稑纾婚柛鏇ㄥ灡閸婂爼鏌熺紒銏犳灍闁绘挶鍎茬换婵嬫濞戞艾顣烘繛瀵稿У缁捇寮诲鍥ㄥ枂闁告洦鍋嗘禒濂告⒑闁偛鑻晶鍓х磼闊厾鐭欓柟顔矫～婵堟崉閾忚鐓ｉ梻渚€娼х换鍫ュ磹閺嶎厼鐓曢柟杈鹃檮閻撴洘绻涢幋鐑囧叕闁衡偓閼姐倗纾奸柍褜鍓熷畷濂告偄缂堢姷鐩庨梻浣瑰缁诲倻鈧凹鍣ｉ幆灞解枎閹存柨浜鹃悷娆忓缁€鍐煕閵娿儲鍋ラ柣娑卞枛铻ｅ〒姘煎灣閸炵敻姊洪崨濠冨闁告挻宀歌棢闁割偀鎳囬崑鎾舵喆閸曨剛顦ㄩ梺鎸庢磸閸ㄨ棄鐣峰ú顏勭労闁告劏鏅涢鎾绘⒑闂堚晛鐦滈柛妯绘倐瀹曟垿骞樺ǎ顒€浜濋梺鍛婂姀閺備線骞忕紒妯肩閺夊牆澧介崚浼存煙鐠囇呯瘈妤犵偛妫濆畷濂告偄缂堢姷鐩庨梻浣瑰閺屻劑鎮樺┑鍡欐殼闁糕剝菧娴滄粓鏌曡箛銉х？濠⒀屼邯閺屽秶鎷犻弻銉ュ及濡ょ姷鍋為敃銏犵暦濮椻偓瀹曪絾寰勬繝鍌氼潏闂傚倸鍊搁崐鐑芥嚄閸撲礁鍨濇い鏍仜閽冪喖鏌曟繛鐐珔閻熸瑱绠撻幃妤呮晲鎼粹剝鐏嶉梺绋款儏閸婂潡寮婚敐澶婃闁割煈鍠楅崐顖炴⒑缂佹ɑ灏紒缁樺姍閳ワ箓宕稿Δ浣告疂閻庣數澧楅〃蹇旂椤忓嫷鍤曠紒瀣氨閺嬪酣鏌熺€电啸闁挎稒绮撳娲濞戣京鍔搁梺绋匡攻椤ㄥ﹪骞嗙仦瑙ｆ瀻闁规儳顕崢鎼佹倵閸忓浜鹃梺閫炲苯澧寸€规洑鍗冲浠嬵敇閻愮數鏆梻浣虹帛閸ㄧ厧螞閸曨厾妫憸搴ｆ崲濞戙垹骞㈡俊銈傚亾缂傚秵鐗滅槐鎺撴綇閵婏妇鍙嗛梺瀹狀潐閸ㄥ潡骞冮埡浣叉灁闁圭瀛╅惁婊堟⒒娴ｅ憡鍟為柟姝屽吹閹广垽宕奸妷褍绁﹂梺鍛婂姦閸犳牜澹曢崗鑲╃闁瑰鍋熼幊鍐偓娈垮枛濞尖€愁潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯閹告娊寮婚敍鍕勃閻犲洦褰冮‖鍫ユ⒑鐎圭姵顥夋い锔诲灥閻忔帗绻涢幘鏉戝毈闁割煈鍨堕獮鍐晲閸℃洜绠氶梺闈涚墕濞层倕鏆╅梻浣侯焾椤戝洭宕戦妶澶婃瀬闁告劦鍠栭～鍛存煟濞嗗苯浜惧┑鐐插悑閻楁洟鍩為幋锔藉亹閻庡湱濮撮ˉ婵堢磼閻愵剙鍔ら柛姘儑閹广垹鈽夐姀鐘茶€垮┑鈽嗗灥濞咃絾绂掔€涙绡€缁剧増菤閸嬫捇鎳犻鍌滃幆闂備礁鎼惌澶屽緤妤ｅ啫绠氶柡鍐ㄧ墛閺呮煡鏌涢妷锝呭婵炲牊鍨垮缁樻媴閸涘﹤鏆堟繛鎾寸椤ㄥ﹤鐣疯ぐ鎺戠闁兼亽鍎辨禍妤呮⒑閻熸澘鈷旂紒顕呭灦閸╂盯骞嬮敂鐣屽幗闂佺粯姊婚崢褎绂嶆导瀛樼厱闁靛牆娲ら弸娑㈡煛鐏炲墽銆掗柍褜鍓ㄧ紞鍡涘礈濞嗘挸鍑犳繛鎴炵懄閸欏繐鈹戦悩杈厡闁活厽鐟ч埀顒冾潐濞叉粓宕伴弽顓溾偓浣糕槈濡攱顫嶅┑顔斤供閸撴瑧绮绘导瀛樷拻闁稿本鐟чˇ锕傛煙鐠囇呯瘈鐎规洘绻堥獮瀣晝閳ь剛绮堟径灞稿亾閸忓浜鹃梺鍛婃处閸嬪棝宕濋敃鈧—鍐Χ閸℃鐟ㄩ梺鍝ュ櫏閸嬪﹤鐣烽妷鈺佹嵍妞ゆ挾濮烽敍婊冾渻閵堝棙鈷掗柍宄扮墦閹繝鎳栭埞鎯т壕婵炲牆鐏濆▍姗€鏌涢幘瀵告噰闁糕斂鍨洪幆鏃堝Ω閵壯屾Ф闁荤喐绮岀粔鐟邦嚕椤愶箑绠瑰ù锝呭帨閹风粯绻涙潏鍓ф偧妞ゎ厼鐗撻獮鍐箣閿旂晫鍘靛銈嗘⒒閸樠囧焵椤掍焦鍊愰柍銉畵瀹曞爼顢斿鍠邦亪姊绘笟鈧埀顒佺〒閳规帡鏌涢弮鈧崹鐢告偩闁垮闄勭紒瀣嚦閵娾晜鐓ラ柡鍐ｅ亾闁稿孩濞婇悰顕€宕奸妷锔规嫽婵炴挻鍩冮崑鎾寸箾娴ｅ啿娲﹂崑瀣煕閳╁啰鈽夌紒鐘靛█楠炴牠骞栭鐘插弗闂佽桨绀侀崐褰掑Φ閸曨垰绠婚柛妤冨仧娴犻箖姊虹紒妯诲鞍缂佸鎹囬崺鈧い鎺嗗亾闁告ɑ绮撳畷鎴﹀箻缂佹ê鈧敻鏌ㄥ┑鍡涱€楀褌鍗抽弻锟犲幢濞嗗繆鏋呴梺鍝勭潤閸曨偒鍤ゅ┑鐐叉閸ㄧ敻宕哄畝鍕拺闂傚牊绋掗ˉ鐐烘偨椤栨稑娴柨婵堝仜閳规垹鈧綆鍋勬禍妤呮煙閼圭増褰х紒鎻掋偢閹粙鎳￠妶鍥╋紳婵炶揪缍€椤曟牕鈻撻弴銏＄厱闁靛鍔岄悡鎰磼瀹€鍐摵缂佺粯绻堝畷鍫曗€栭顒€娲﹂悡鏇㈡煏婢舵ê鏋ゅù鐘灲閺屾洘寰勯崱妯荤彆闂佺粯鎸诲ú鐔煎箖濡ゅ懏鏅查幖绮光偓鑼泿闂備浇顕х换鎰邦敋椤撱垹鐒垫い鎺戝枤濞兼劖绻涢崣澶岀煉闁靛棔鐒︾换婵嬪炊椤剨绠撻弻鐔兼偋閸喓鍑＄紓浣哄У婵炲﹪寮婚悢鐓庣畾鐟滃秹銆傞幎鑺ョ厸闁糕剝鍔曢悘鍙夋叏婵犲偆鐓肩€规洘甯掗～婵嬵敄閽樺澹曢梺缁樺灱婵倝宕愰崸妤佺叆闁哄啫鐗婇弳婊堟煕鐎ｎ偅灏电紒顕呭幖閳藉螣鐠囧弶顏￠柣搴ゎ潐濞叉﹢鎮￠敓鐘茶摕婵炴垶顭囬弳鍡涙煃瑜滈崜鐔风暦閹存績妲堟慨妯哄綁缁楀姊虹紒姗嗙劷缂侇噮鍨堕幃鈥斥枎閹剧补鎷哄銈嗘尪閸斿酣鎮鹃柆宥嗙厸閻庯絻鍔岄埀顒佹礋閸┿儲寰勯幇顒傤啋閻庤娲栧▔锕侇樄闁哄备鈧磭鏆嗛柍褜鍓熷畷浼村箻閼告娼熼梺鍦劋閸わ箓鎮㈤崨濠傤潯闂佸搫顦悘婵嬫倶娓氣偓濮婂宕掑▎鎺戝帯缂備緡鍣崹璺侯嚕婵犳艾惟闁靛瀛╃粙鎴﹀煡婢舵劕顫呴柣妯兼暩閺嬧偓缂傚倸鍊风粈渚€顢栭崱娑辨晞婵炲棙鎸婚崑鍌炴煏婢跺棙娅嗛柣鎾存礃閵囧嫰骞囬崜浣瑰仹缂備胶濮甸敃銏ゅ蓟閿濆绠婚柡澶嬪灩娴煎苯鈹戦纭锋敾婵＄偠妫勯悾鐑藉Ω閿斿墽鐦堥梺绋挎湰椤曟挳寮撮姀鈾€鎷洪梺鍛婄缚閸庤鲸鐗庨梻浣虹帛椤ㄥ棛鍒掓惔銊ョ闁圭儤姊圭€氭氨鈧懓澹婇崰妤呭磽閻㈠憡鈷戦柣鎾冲閹叉悂鏌℃担瑙勫€愮€规洏鍨介弻鍡楊吋閸″繑瀚奸梻浣告啞缁诲倻鈧凹鍙冨畷鎺楀Ω閳哄倻鍘遍梺鍝勫€藉▔鏇㈡倿閹间焦鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍暜閹烘纾归柟闂寸閻撴繈鏌涢鐘插姕闁绘挻鐟﹂妵鍕棘閹稿孩鍎撻梺璇茬箺鐏忔瑩鍩€椤掑喚娼愭繛璇х畵瀹曟垶绻濋崒婊勬闂佺粯姊婚埛鍫ュ极閸愨斂浜滈柟鎵虫櫅閻忣亝銇勯弬鍨伃婵﹤顭峰畷鎺戭潩椤戣棄浜惧瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏犵厱婵﹩鍘介妵婵嗏攽閳╁啯鍊愰柛鈹惧墲閹峰懘鎮€电鑰挎繝鐢靛Х椤ｈ棄危閸涙潙鍨傞梻鍫熺〒閺嗭箓鏌ｉ姀鐘冲暈闁?
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
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婎潊闁绘ê妯婂Λ宀勬⒑鏉炴壆顦﹂柨鏇ㄤ邯瀵鍨鹃幇浣告倯闁硅偐琛ラ埀顒€纾鎰磽閸屾瑧鍔嶉柛鏃€鐗犻妴鍐幢濡皷鏀虫繝鐢靛Т濞层倕娲块梻浣告啞娓氭宕ｆ惔銏㈩浄濡わ絽鍟埛鎴︽煟閻斿憡绶查柡鍡涗憾閺岋繝宕奸銏狀潾闂佺懓绠嶉崹褰掑煘閹达箑骞㈤煫鍥ㄦ⒒娴滄牠姊绘担绋款棌闁绘挸鐗撳畷鏉课旈崨顔尖偓鍫曟煕椤愮姴鍔滈柍閿嬪灴閺岀喓绮欓崹顕呭妷缂佸墽鍋撻幃鍌炲蓟濞戞瑧绡€闁告劗鍋撻悾鐓庘攽閳ュ啿绾ч柛鏃€鐟╅獮鍐ㄢ枎閹存柨浜鹃梻鍫熺⊕閹叉悂鏌ｉ敃鈧悧鎾愁潖閾忓湱鐭欓柟绋垮閹疯京绱撴笟鍥ф灈缂佸鎸抽崺銏ゅ箻濞ｎ剙浜濋梺鍛婂姀閺呮盯鍩€椤掑倹鏆柡宀嬬秮閹垽宕ㄦ繝鍕殥婵＄偑鍊曞ù姘椤忓牆钃熸繛鎴欏灪閺呮煡鏌涢埄鍐剧劷缂佸娲弻锝夊箻閸楃偐鍋撻弽顓炍﹂柛鏇ㄥ枤閻も偓闂佺懓澧界槐鐔兼晲婢跺鍘搁梺鍛婁緱閸犳岸鎯岄幒鎾村弿濠电姴鍟妵婵堚偓瑙勬磸閸斿秶鎹㈠┑瀣妞ゎ厽鍨惰ⅸ闂傚倸鍊风粈渚€骞夐敓鐘虫櫔闂備線娼ч悧濠囧箖閸岀偞鍋樻い鏂挎閺冨牆绀冮柨婵嗘噸婢规洟姊洪幐搴ｇ畵濡ょ姵鎮傚鎼佹偄閸忚偐鍘撻梻浣哥仢椤戝懐绮幒妤侇梿濠㈣埖鍔栭悡銉︾節闂堟稒顥為柛锝嗘そ閺屾盯鎮㈤崫鍕闂佸搫鏈惄顖炲箖閵忋倕浼犻柛鏇ㄤ簼椤洟姊绘担鍛婃儓闁活厼顦辩槐鐐寸瑹閳ь剟鐛崘顔嘉у璺猴功閿涙粌鈹戦悩璇у伐闁瑰弶绋戦…銊╁醇閻斿皝鍋撻悽鍛婂仭婵炲棗绻愰顏嗙磼閳ь剟宕橀埡鍐啎闂佽偐鈷堥崜娆撳几濞戙垺鐓熸繛鎴濆船閺嬬喖鏌熼娑欘棃濠碘剝鎮傞弫鍐焵椤掆偓閳绘捇鎮㈤崗灏栨嫼闂傚倸鐗婃笟妤€危閸洘鐓曢幖瀛樼箓瀹撳棝鏌熼鐐珪缂佽鲸甯掕灒闁兼祴鏅濆Σ鍥⒒娴ｅ湱婀介柛銊ㄦ椤洩顦归柛鈹垮灲瀹曞ジ鎮㈤搹璇″晭濠电偠鎻徊浠嬪疮閸ф瑙︽俊顖濄€€濡插牊鎱ㄥ鍡椾簼闁告洖鍟撮弻鈩冨緞鐏炴垝绮堕柣蹇撶箰閹虫劗妲愰悙瀛樺闁告縿鍎插▓鐐節闂堟稑鈧鈥﹂崼婵囨殰闂傚倷鐒︾€笛冾潖瑜版帒纾块柟鍓佹櫕瀹撲胶鈧箍鍎遍ˇ浼村煕閹烘嚚褰掓晲閸噥浠╅柣銏╁灡閻╊垶寮诲☉銏犵睄闁规儳澧庨弳銈夋倵鐟欏嫭绀冪紒顔肩焸閸┿垺鎯旈妸銉х杸濡炪倖鎸鹃崰鎾广亹閹稿簼绻嗛柣鎰典簻閳ь剚鐗犻幃褍顭ㄩ崼婵堬紱闂佺粯鍔楅崕銈夊磻閸岀偞鐓涢柛銉ｅ劚閻忊晝绱掗悩鑼Ш鐎殿喖鐖煎畷鐓庘攽閸″繑瀵栫紓浣哄亾瀹曟ê鈻旈弴鐘愁潟闁圭儤顨嗛崑鎰偓瑙勬礀濞层倕顕ｉ妸鈺傗拺闁告繂瀚﹢鐗堢箾閼碱剙鏋庨柣锝囧厴楠炲棝鏌囬敂鍙晜淇婇悙顏勨偓鏇灻瑰璺虹煑闁告劦鍠栭拑鐔兼煃閳轰礁鏆炲┑顖氼嚟缁辨帞鈧綆鍋勯婊勭箾閸喓鍙€婵﹥妞介幃鐑藉级閹稿巩鈺呮⒑绾懏鐝柨姘舵煟閿濆洤鍘撮柟顔界矌缁數鈧綆鍋呭▍宥嗙節閻㈤潧顫掗柛鏇ㄥ亜婵垺绻涚€涙鐭嬮柛搴㈠▕閸╃偤骞嬮敂钘変汗闂佸憡鐟ラˇ顖炈囬埡鍛拺闁规儼濮ら弫閬嶆煕閵娿儲鍋ユ鐐插暢閵囨劙骞掗幋鐘测偓鐐烘⒑閹稿海绠撻柛濠傛啞缁傚秵銈ｉ崘鈺冨弰闂婎偄娴勭徊鑺ョ閻愯鐟邦煥閸℃鍘紓浣虹帛缁诲牓骞冩禒瀣棃婵炴潙澧庨崑銈夊蓟濞戙垹惟闁挎柨顫曟禒銏ゆ⒑鏉炴壆顦﹂柛鐔风摠閹便劑鍩€椤掑嫭鐓忛柛顐ｇ箖閸ｄ粙鏌ｉ敐鍡樸仢婵﹨娅ｉ幏鐘诲灳瀹曞洠鍋撴繝姘厓鐟滄粓宕滈悢濂夊殨闁靛濡囬々鐑芥倵閿濆骸浜為柛妯哄船椤啴濡堕崱妤€衼缂傚倸绉崑鎾寸節閵忥綆娼愰柨姘舵婢跺绡€濠电姴鍊绘晶銏ゆ煟閿濆棙銇濋柡宀嬬磿娴狅妇鎷犻幓鎺濈€抽梻浣哥枃椤宕归崹顔炬殾闁告鍋愬Σ鍫ユ煙閸忓摜顦﹀ù婊呭亾娣囧﹪濡堕崒姘婵＄偑鍊ら崣鈧柛搴☆煼钘濋梺顒€绉甸悡鏇熶繆閵堝嫮顦﹂柍缁樻礈閳ь剚顔栭崰鏍€﹂悜钘夋瀬闁圭増婢樺婵囥亜韫囨挻濯兼繛鍏兼濮婄粯鎷呴崨濠冨創闁荤姴娲ㄩ崑銈呯暦濮椻偓婵℃悂骞囬鐔侯槰缂備胶绮换鍌炲煝閹捐鍨傛い鏃傛櫕娴滎亪姊绘担鐟邦嚋婵炴彃绻樺畷鏇㈡焼瀹ヤ讲鍋撻弮鍫濈妞ゆ棁妫勯惂鍕節閵忥絾纭鹃柡鍫墮閳绘挻銈ｉ崘鈹炬嫼闂傚倸鐗婃笟妤呮偂椤撶偐鏀介柍銉ㄦ珪瀹告繄鈧鍟崶褏鍔﹀銈嗗笒鐎氼參鍩涢幋锔界厱婵炴垶锕弨璇差熆鐠哄搫顏柡灞剧洴楠炴鈧潧鎲￠崳褔姊虹€圭媭鍤欑紒澶屽厴钘濋柡灞诲劜閻撳啴鎮峰▎蹇擃仼闁诲繑鎸抽弻鐔碱敊閻ｅ本鍣伴梺鐟扮－婵炩偓妞ゃ垺顨嗛幏鍛存偡閻楀牏浜紓鍌氬€搁崐鎼佸磹妞嬪孩顐介柨鐔哄Т缁愭鏌″搴′簽闁活厽鎸鹃幉鎼佹偋閸繄鐟ㄧ紓浣哄У閻楃娀寮婚敐澶娢ч幖杈剧磿娴煎洤鈹戦埥鍡楃仩閻庢凹鍓濋悘鍐⒑缂佹﹫鑰挎繛浣冲嫮顩烽柨鏇炲€归悡鏇㈡煏婵犲繒鐣辨い鏇熺矒閺岀喐绗熼崹顔碱潎闂佺硶鏂侀崑鎾愁渻閵堝棗绗掗悗姘煎弮閹繝鎮㈤崫銉ь啎闂佺硶鍓濋〃澶屸偓姘緲闇夋繝濠傚閸斻倝鏌嶇憴鍕伌闁诡喗鐟╅幊鐘活敆娴ｇ儤顎囬梻鍌欑閹碱偊骞婇幇顓犵闁逞屽墰閳ь剚顔栭崰鏍€﹂柨瀣╃箚闁兼悂娼х欢鐐测攽閻樻彃顏撮柛鐔锋搐閳规垿鎮欓懜闈涙锭缂傚倸绉崑鎾愁渻閵堝骸浜滄い锔炬暬瀵濡搁埡濠冩櫍闂佺粯妫佸▍锝夘敊閺囥垺鐓熼幖杈剧稻閺嗏晜銇勯鐐靛闁哄棌鏅犲缁樻媴閸涘﹤鏆堝銈庡幖濡繂鐣烽幆閭︽Ь闂佸憡蓱閹稿啿顫忛搹瑙勫厹闁告侗鍠栧☉褔姊婚崒姘仼閻庢碍婢橀悾鐑藉箛閻楀牆鈧鏌ら幁鎺戝姢闁告ê宕埞鎴︽倷閸欏妫炲┑鐐茬摠閼规崘鐏掗梺鍓插亞閸犲秶鎹㈤崱妯镐簻闁规壋鏅涢埀顒佹礃缁傛帟顦寸紒杈ㄥ笚濞煎繘濡搁敃鈧棄宥夋⒑閻熸澘妲婚柟铏耿楠炴牞銇愰幒鎾充画闂佽顔栭崳顕€宕戣濮婂宕掑▎鎴М闂佽绁撮埀顒佸墯濞兼牜鎲搁悧鍫濈瑲闁稿绻濋弻鏇熺箾閻愵剚鐝曢梺绋垮椤ㄥ懓鐏冮梺鎸庣箓閹冲酣寮抽悙鐑樼厓闂佸灝顑呴悘鎾煛鐏炲墽鈽夐柍钘夘槸椤粓宕奸悤浣规瘔缂傚倸鍊风粈渚€鎮ラ崗鑲╊洸闁割偅娲栭拑鐔哥箾閹寸儐鐒搁柣鏂挎惈閺嬪牓寮堕崼姘珔濠殿喖銈稿娲川婵犲孩鐤佹繛瀛樼矊閻栫厧顕ｇ拠娴嬫闁靛繆鏅滈弲婵嬫⒑缂佹〞鎴ｃ亹閸愨晜鍙忛柡鍥ュ灪閳锋垹绱撴担鑲℃垹浜告导瀛樼厽闁绘柨寮跺▍濠勨偓娈垮枛椤兘骞冮姀銏犳瀳閺夊牄鍔嶅▍鏍⒒娴ｈ櫣銆婇柛鎾寸箞閵嗗啴宕ㄩ弶鎴炶緢闂侀潧绻堥崐鏍偂閺囥垺鐓冮柛婵嗗閺嗘瑥霉閻樻椽鍝洪柟渚垮妽缁绘繈宕橀埞澶歌檸闂備礁鎼懟顖炲箠濮椻偓楠炲啫鈻庨幘鏉戞闁荤姾妗ㄧ拃锔藉閸愩劉鏀介柣姗嗗亝婵即鎮橀悙鍨珪濞寸姵鎮傚娲传閸曨厾鍔圭紓鍌氱С缁舵艾鐣峰ú顏勭労闁告劦浜為惁鍫ユ⒑缁嬫寧婀扮紒顔肩墕椤╁ジ骞橀弬銉︽杸闂佺粯蓱椤旀牠寮抽鐐寸厓鐟滄粓宕滃▎鎴濐棜妞ゆ挶鍨圭粻娲煟濡吋鏆╃痪鎯у悑娣囧﹪濡堕崨顓熸闂佸搫顑嗛惄顖炲蓟濞戙垹鐓橀柟顖嗗倸顥氭繝纰夌磿閸嬫垿宕愰弽顐ｆ殰闁圭儤鏌￠崑鎾愁潩閻撳孩鐏堥梺浼欑到閻忔氨绮悢鐓庣劦妞ゆ帒瀚粻鏍ㄤ繆閵堝懏鍣洪柡鍛叀楠炴牜鍒掗悷鏉库拤闂佽绻愮粔鐟邦潖濞差亜浼犻柛鏇ㄥ墮濞呫倗绱撴担绛嬪殭闁绘鎸搁锝囨嫚瀹割喖鎮戦梺鍓插亽閸嬪懘顢撻崶顒佲拻濞达絽鎲￠崯鐐烘煙缁嬫鐓奸柟顖氬椤㈡盯鎮欓崣澶嬓氶梻渚€鈧偛鑻晶顖炴煏閸パ冾伃妤犵偞甯掗濂稿幢濞嗗秴浠掓繝鐢靛О閸ㄥジ锝炴径搴㈡噷闂備線娼уú銈団偓姘嵆瀵偊宕掑鍕彴闂佽偐鈷堥崗娑橆浖閹剧粯鈷掑ù锝呮惈娴犳椽鏌涙惔锝囧笡濞ｅ洤锕畷鍫曨敆閳ь剛绮婚弻銉︾厱闁哄洢鍔岄悘鐘诲箚閻斿吋鈷戦柟绋垮绾剧敻鏌涚€ｎ偅灏甸柣銉簽缁辨帡濮€閻樺吀鐢荤紓鍌欒兌缁垶鎯勯鐐靛祦婵せ鍋撶€殿喖鐖奸獮瀣攽閸モ晜顫滈梻鍌氬€烽悞锕傚箖閸洖绀夐悗锝庡枛绾偓闂佸憡鍔樼亸娆撳汲閿旂晫绡€濠电姴鍊绘晶鏇犵棯椤撴稑浜鹃梻鍌欒兌缁垶宕濊箛娑樼柧婵犲﹤鐗嗗Λ姗€鏌嶈閸撶喎顫忕紒妯诲闁惧繒鎳撶粭锟犳⒑閸涘﹥鈷愭繛鍙夌墵椤㈡岸鏁愭径濠囧敹闂佸搫娲ㄩ崑銈呂涢悜鑺モ拺闂傚牃鏅涢惁婊堟煕濡搫鈷斿瑙勬礋閹虫牠鍩￠崘顏庣闯濠电偠鎻徊鍨枍閵忋倕绀傛い鎺戝閻撶娀鏌熷畡鐗堟拱缁绢厼鐖奸弻宥堫檨闁告挻宀搁、娆撳冀椤撶偟鐛ュ┑鐐村灟閸╁嫰寮€ｎ剚鍠愰柡鍐ㄧ墕閺勩儵鏌曟径娑氱暠缂佸墎鍋ら弻鐔兼焽閿曗偓婢х粯绻涢悡搴含婵﹨娅ｇ槐鎺戭潨閸℃瑥濮洪梻浣芥〃閻掞箓骞冮崒姘煎殨闁割偅娲栧婵嗏攽閻樿精鍏岀紒鐘冲哺濮婅櫣绱掑Ο鍝勑曟繛瀛樼矋缁捇宕洪埀顒併亜閹哄秷鍏屽褔浜堕弻鈩冪瑹閸パ勭亶缂備胶绮换鍫濈暦閻旂⒈鏁囬柣姗€娼ф慨鍛婄節閻㈤潧浠╅柟娲讳簽瀵板﹦鎹勯妸锕€鍘归梺鍓插亝濞叉牠宕欓悩纰樺亾楠炲灝鍔氭い锔垮嵆閹繝濡烽敂鍓х槇闂傚倸鐗婄粙鎺椝夐悙娴嬫斀妞ゆ牗鑹鹃幃鎴犵磼缂佹娲存鐐达耿楠炲洦鎷呴崫鍕€梻鍌欑閹碱偊寮甸鈧叅闁绘棃顥撻弳锕傛煟閹惧磭宀搁柡鈧禒瀣厱闁靛鍔岄悡鎰版煥濞戞绐旀慨濠勭帛閹峰懘鎮烽幍铏亞闂備浇銆€閸嬫捇姊洪鈧粔瀵哥矆婢跺绠鹃柛鈩兩戠亸顓㈡煢閸愵亜鏋戠紒缁樼洴楠炲鈻庤箛鏇氭偅缂傚倷绶￠崑鍕矓瑜版帒钃熺€广儱鐗滃銊╂⒑閸涘﹥灏甸柛鐘崇墵閻涱噣宕橀钘夆偓濠氭煠閹帒鍔滄繛鍛濮婄儤瀵煎▎鎴濆煂婵犮垼娉涘ú顓烆嚕?
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


// ---------------------------------------------------------------- GHA 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾惧鏌ｉ幇顒佹儓闁搞劌鍊块弻娑㈩敃閿濆棛顦ョ紓浣哄С閸楁娊寮婚悢鍏尖拻閻庡灚鐡曠粣妤呮⒑鏉炴壆顦﹂悗姘嵆瀵鈽夐姀鈺傛櫇闂佹寧绻傚ú鈺冪矚閸ф鍋℃繝濠傛噹椤ｅジ鎮介娑樻诞闁诡喗鐟︾换婵嬪炊閵娧冨笚闂佽崵鍠愰悷銉р偓姘煎墴瀹曟繈濡堕崥銈囨嚀椤劑宕奸姀銏℃瘒闂備礁鎼惌澶岀礊娴ｈ鍙忛柍褜鍓熼弻宥夋寠婢跺妫為梺鍝勬嚇娴滆泛顫忛搹瑙勫仒闁炽儱鍘栨竟鏇炩攽閻愬樊鍤熼柛妯犲洦鍋ら柕濞炬櫆閸嬪倿鏌ｉ弬鍨倯闁绘挶鍎甸弻锟犲炊閵婏妇娈ら梺鍛婃煥缁夌敻濡甸崟顖毼ㄩ柕澶樺枟鏁堥梻?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴銏℃櫆缂備焦蓱濞呭牓姊绘担鍛婂暈婵炶绠撳畷褰掓焼瀹ュ懎鎯為梺鍛婃处閸ㄩ亶鎮￠弴鐔翠簻闁规澘澧庣粙鑽ょ磼閳ь剟宕橀鐣屽幈闁诲函鎬ラ崟顒傚絾缂傚倷鑳剁划顖滄崲閸惊娑㈠礃閵娿垺顫嶅┑鐐叉濞存艾危鏉堚晝纾介柛灞捐壘閳ь剙鎽滅划鏃堝级閹炽劍妞介、姗€濮€閻樼儤鎲伴梻浣虹帛濮婂宕㈣缁顫濇潏銊ユ瀾閻庡箍鍎遍ˇ顖毿ч弻銉ョ骇闁割偅纰嶉弶褰掓煕鐎ｎ偅灏柍缁樻崌瀹曞綊顢欓悾灞肩矚濠电姷鏁搁崑娑㈠箠韫囨拋鍝勎熼懖鈺冪厰闁哄鐗勯崝搴ｅ姬閳ь剟姊虹粙娆惧創濠殿喓鍊曡灋闁告劦鍠栭弰銉╂煃瑜滈崜姘跺Φ閸曨垰绠抽柛鈩冦仦婢规洟姊洪幑鎰惞闁稿鍊濋獮鍐ㄎ旈崨顕呮濠电偞鍨堕悷銉╁汲閸繍娓婚柕鍫濋娴滄繃绻涢懠顒€鏋涚€殿喖顭烽幃銏焊娴ｅ湱浜栭梻浣稿閻撳牓宕板Δ鍛€堕柟鎯у绾惧ジ鏌熼鍡楀暟缁嬪繒绱撴笟鍥ф灍闁荤啿鏅涢锝嗙節濮橆剛浼嬮梺褰掝暒缁€渚€骞冮幋锔界厵闁兼祴鏅炶棢闂侀€炲苯澧柛鎾磋壘椤洦銈ｉ崘鈹炬嫼濠殿喚鎳撳ú銈夋倿濞差亝鐓曢柕濠忛檮缁舵煡鎽堕悙瀵哥闁糕剝锚婵鏌?+ 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙闁诡垳鍋ら獮鏍庨鈧俊濂告煟椤撶噥娈滄鐐寸墪鑿愭い鎺嗗亾濠德ゅ亹缁辨帡骞囬褎鐤侀梺鍝勫閳ь剚鍓氶崥瀣箹缁厜鍋撳畷鍥跺晥闂?闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰紦閻㈠姊绘担鐟邦嚋婵炲弶鐗犲畷鎰亹閹烘垹锛涢柣搴秵閸嬪倻鎹㈤崱娑欑厪闁割偅绻冮崳褰掓煛閸℃瑥鏋涢柡宀嬬秮椤㈡﹢鎮㈢粙璺紦闂備礁鎼惌澶岀礊娓氣偓閻涱喚鈧綆鍠楅弲鏌ユ煕濞戞ɑ鍣洪柟鐟版喘瀵鏁撻悩鍙夈仢婵炶揪绲块…鍫ワ綖椤愶附鈷戦梺顐ゅ仜閼活垱鏅堕鈧弻娑樜熼懡銈囩厜閻庤娲橀崹鍧楃嵁濡偐纾兼俊顖滃帶鐢鏌ｉ悢鍝ョ煁缂侇喗鎸搁悾宄邦煥閸喎浜滄俊鐐差儏鐎涒晛鈻撻幆褉鏀介柣鎰级椤ョ偤鏌熺粙娆剧吋鐎规洟娼ч鍏煎緞鐎ｎ剙甯惧┑鐘灱椤曟牠宕规导鏉戝珘妞ゆ帒鍊婚惌鍫熴亜閹板爼妾柍閿嬪笒闇夐柨婵嗘处閸も偓濡炪倖娲樼划搴ｆ閹烘梹瀚氶柟缁樺坊閸嬫捇宕妷銉ョ柧闂傚倷绶氬褑澧濋梺鍝勬噺閻╊垰鐣疯ぐ鎺戠＜闁绘劕顕崢鐢告⒑鐠団€崇仭婵犮垺顭堥。鍧楁⒒娴ｈ櫣甯涢柟姝岊嚙鐓ら柣鏃傚帶閽冪喖鏌ｉ弬鍨倯闁稿﹦绮穱濠囶敍濠垫劕娈梺鍝勬椤ㄥ﹤顫忛搹鍏夊亾閸︻厼校妞ゃ儱顦伴妵鍕晝娴ｅ嘲鎯堝┑鈥冲级閸旀瑩鐛Ο鍏煎珰闁肩⒈鍓ㄧ槐鍙夌節閻㈤潧浠﹂柛銊ョ埣閹兘濡疯椤ユ岸鏌熼幆鏉啃撻柍閿嬪灴瀵爼鎮欓弶鎴濡炪倧璁ｆ俊鍥╂閹烘挸绶為悘鐐村劤濞堝矂姊烘导娆戞偧闁稿繑锕㈤獮鍐焺閸愨晛鍔呭銈嗘⒐閸庡啿顕ｉ幎鑺モ拻濞达絽鎲￠崯鐐层€掑顓ф疁鐎规洖缍婇弻鍡楊吋閸涱垳宕堕梻浣告贡閸庛倝銆冮崱娑樼厱闁圭儤顨嗛悡鏇㈡倶閻愭潙绀冨瑙勶耿閺屽秷顧侀柛鎾寸洴閳ワ箓宕奸姀鐘茬柧闂傚倷鑳剁划顖炩€﹂崼銉晪鐟滃繑绔熼弴鐔虹瘈婵﹩鍘鹃崢鐢告⒑閸涘﹥瀵欓柛娑卞幘椤愬ジ姊?.rc 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊瑜忛弳锕傛煕椤垵浜濋柛娆忕箳閳ь剙绠嶉崕閬嵥囬婊呯焼闁告劏鏂傛禍婊勩亜閹捐泛鏋庨柣蹇ョ秮閺岋綁濡烽鑽ゆ晼缂備浇椴搁幑鍥х暦閹烘垟鏋庨柟杈剧悼閻涖儵姊绘担铏瑰笡闁瑰憡鎮傞幃褔骞樺畷鍥ㄦ闂佽鍎兼慨銈夊疾閹绘帩鐔嗛悹铏瑰皑閺€濠氭煟鎼搭喖澧撮柟顔筋殜閻涱噣宕归鐓庮潛婵犵妲呴崑鍛淬€冮崼婵冩瀻闁靛繈鍊栭崐鐑芥煟閹寸儐鐒介柛妯哄船椤啴濡堕崱妤€衼缂傚倸绉崑鎾寸節閵忥綆娼愰柨姘舵?
// Enter = 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸ゅ嫰鏌涢锝嗙闁诡垳鍋ら獮鏍庨鈧俊濂告煟椤撶噥娈滄鐐寸墪鑿愭い鎺嗗亾濠德ゅ亹缁辨帡骞囬褎鐤侀梺鍝勫閳ь剚鍓氶崥瀣箹缁厜鍋撳畷鍥跺晥闂傚倷鑳堕…鍫ヮ敄閸岀偛鐤炬繛鎴炵矌娴滄瑩姊绘担鐟邦嚋缂佽鍊块獮濠冩償閵娿儲杈堥梺缁樺姉閸庛倝鎮″☉銏＄厱婵犲ň鍋撶紒鈧担瑙勬珷闁圭虎鍠楅悡娆愩亜閺冨倻鎽傛繛鍫熸閺屽秹鎸婃径妯恍﹂柧浼欑秮閺岋綁骞樺畷鍥у摵闂佺顑嗛幑鍥箚?= 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰紦閻㈠姊绘担鐟邦嚋婵炲弶鐗犲畷鎰亹閹烘垹锛涢柣搴秵閸嬪倻鎹㈤崱娑欑厪闁割偅绻冮崳褰掓煛閸℃瑥鏋涢柡宀嬬秮椤㈡﹢鎮㈢粙璺紦闂備礁鎼惌澶岀礊娓氣偓閻涱喚鈧綆鍠楅弲鏌ユ煕閳锯偓閺呮粓藟婢舵劖鐓?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋涢ˇ鐢稿极閹剧粯鍋愰柛鎰级閻ゅ嫰姊绘担铏瑰笡闁告棑绠撳畷婊堟晝閳ь剟鍩㈤幘缁樺殟闁靛绠戝鍨攽閻樼粯娑ф俊顐ｇ矒閿濈偤鏁冮埀顒勬箒濠电姴锕ら崯鎵矓濞差亝鐓欐い鏃傜摂濞堟粍淇婇銏犳殭闁宠棄顦埢搴ょ疀閺冣偓閻ｅジ姊?gha fetch闂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弮鍫熸殰闁稿鎸剧划顓炩槈濡娅ч梺娲诲幗閻熲晠寮婚悢鍛婄秶闁告挆鍛闂佽瀛╅懝楣兯囨导鏉懳﹂柛鏇ㄥ灠缁犲磭鈧箍鍎卞Λ顓炍熼崒婊呯＝濞达絿鐡旈崵娆愪繆椤愶絿绠炵€殿喖顭烽幃銏ゅ礂鐏忔牗瀚介梺璇查叄濞佳勭珶婵犲伣锝夘敋閳ь剟寮婚敐鍡樺劅闁靛骏绲介ˉ婵嗩渻閵堝繐顩柡浣筋嚙椤曪絿鎷犲顔兼倯闂佸壊鍋掗崑鍛搭敁閸ヮ剚鈷掑ù锝呮啞閸熺偤鏌熼幖浣虹暫鐎规洜澧楅幆鏃堝Ω閿曗偓娴犲ジ姊虹紒妯虹伇婵☆偄瀚弫顕€姊绘担鐟邦嚋婵☆偂绀佽灋闁告洦鍨扮粈澶愭煕濞戝崬骞愰柡鈧禒瀣厽婵☆垵鍋愮敮娑欑箾閹冲嘲娲﹂悡鐔镐繆閵堝嫯鍏岄柣顓烆儔閹妫冨☉鏍т划閻庤娲栭悥濂搞€佸Δ浣瑰闁告繂瀚粻娲⒒閸屾瑨鍏岀紒顕呭灦瀹曟繈寮撮姀鈺傛櫔濡炪倖鎸堕崹褰掓偂閳ユ剚鐔嗛柤鎼佹涧婵牓鏌ｉ幘璺烘瀾闁靛洤瀚伴獮妯尖偓闈涙憸閻ｉ箖姊虹粙璺ㄧ闁哄牜鍓涢幑銏犫槈閵忕姴鑰块梺褰掑亰閸撴稒瀵奸崼婵愭富闁靛牆鍟悘顏堟煥閺囨ê鐏柟渚垮姂閸┾偓妞ゆ帒瀚悡鐔镐繆椤栨稒銇熼柛鐔风箲娣囧﹪骞撻幒鎴ｅ煘闂佸疇顫夐崹鍫曠嵁婵犲洦鐓曞┑鐐戝啯鍣藉☉鎾崇Ч閺岀喐娼忔ィ鍐╊€嶉梺绋匡龚閸╂牜鎹㈠┑瀣棃婵炴垵宕崜浼存⒑閹肩偛濡奸柛濠傜仢椤繐煤椤忓秵鏅㈤梺閫炲苯澧扮紒顔芥閹粙宕ㄦ繝鍌欑暗闂備礁鎼ú銏ゅ垂濞差亝鍋傛繛鍡樻尰閻撶喖鏌熺€电鈧牠骞嬮敂钘変簵闂侀潧顧€鐠愮喐绂嶅鍫熺叆婵犻潧妫欓崳鐑芥煕鐎ｃ劌鐏查柡灞糕偓宕囨殕閻庯綆鍓涜ⅵ濠电姷顣介崜婵嬪箖閸岀偛鏄ラ柨鐔哄Т绾惧吋绻涢幋娆忕仾闁哄棙锕㈠濠氬磼濞嗘埈妲紒鍓ц檸閸橀箖宕氶幒妤婃晬婵ê鍚嬬紞搴ㄦ偡濠婂懎顣奸悽顖滃仜閻ｅ灚鎷呯化鏇熸杸闂佺粯锚瀵埖寰勯崟顖涚厽?
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
    // WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存い銏℃礋閺佹劙宕卞▎娆戠┛闂傚倷绶氶埀顒傚仜閼活垱鏅堕崜褏纾奸柣妯挎珪鐏忣參鏌ｉ敐澶嬫暠缂佽櫣鏅划娆戞嫚娣囧崬濡介柕鍥у瀵噣宕堕‖顔芥尰缁绘盯宕ㄩ鐘测叺濠殿喖锕︾划顖炲箯閸涙潙宸濆┑鐘插€瑰▓姗€姊绘担鍛婂暈婵☆偅鍨圭划濠氬箻瀹曞洦娈炬繛鏉戝悑濞兼瑩宕橀埀顒€顪冮妶鍡楀闁糕晛瀚灋婵犲﹤鐗婇埛鎴犵磼婢跺﹥顥滈悗姘槻閳绘捇寮撮姀锛勫帗闂備礁鐏濋鍛存倶閿曞倹鐓忛柛銉戝喚浼冨Δ鐘靛仦鐢€崇暦閸楃儐娓婚柟顖嗗本顥＄紓鍌氬€搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾閽樻繃銇勯弽顐㈠壉闁轰礁顑嗛妵鍕箻鐠虹洅銏ゆ煕閵婏妇绠撻柍瑙勫灴閹晝鈧湱濮撮ˉ婵嬫⒑閸涘﹤濮€闁稿鎸绘穱濠囨倷椤忓嫧鍋撻弽顓炵婵°倕鎳庣粣妤呭箹濞ｎ剙鐏い鈺傜叀閺岋綁骞囬棃娑樷拻濠电偛鐗呯划娆撳蓟閿濆绠涙い鏍ㄦ皑濮ｃ垺绻涚€电袨闁搞劋绮欏璇测槈閵忕姷鐫勯梺閫炲苯澧柣锝嗙箞楠炴牗鎷呭灞炬啺闂備胶绮濠氬储瑜旈幆灞轿旈埀顒勨€︾捄銊﹀枂闁告洦鍘鹃鍏肩箾鐎涙鐭ゅù婊庝簻閻ｅ嘲煤椤忓嫀銊╂煥閺囨浜剧紓浣稿閸嬨倝寮婚悢鍏肩劷闁挎洍鍋撳褏绮换娑㈡偂鎼搭垵鍚┑顔硷龚濞咃絿妲愰幒鎴建闁割偓绲洪崑鎾诲箰鎼达絿顔曟繛杈剧到閸熻法鈧凹鍙冨畷锝堢疀濞戞瑧鍙嗛梺鍝勫暊閸嬫捇鏌涢妸锔剧畼闁瑰箍鍨藉畷鍗炩槈濞嗘垵骞楅梻浣筋潐濠㈡﹢宕ラ埀顒傜磼閵娿儺鐓奸柡灞界Х椤т線鏌涢幘鏉戝摵闁诡啫鍐剧叆闁割偅绻冮悗顒€顪冮妶鍡樼５闁稿鎸婚〃銉╂倷閺夋垶璇為悗娈垮櫘閸撶喎鐣烽锕€绀嬫い蹇撳濡粌鈹戦悩鎰佸晱闁哥姵顨婇垾锕傚炊椤掆偓閸屻劎鎲告惔銊ョ柧闁割偅娲栫粻缁樸亜閺冨倹娅曢柛妯兼暬濮婄粯鎷呴崫銉よ檸濡炪倖鍨甸幊姗€骞冨鈧獮姗€顢欓悾灞藉箥婵＄偑鍊栭弻銊╁箹椤愶附鍊堕柤纰卞厴閸嬫挾鎲撮崟顒傤槰濡炪們鍔岄敃顏堢嵁婵犲洤鍐€妞ゆ挾鍋熼崢鍛婄箾鏉堝墽鎮兼い顓炵墦閵嗗懘顢楅埀顒勫煘閹达附鍋愰柛顭戝亝濮ｅ嫭绻濆▓鍨灆闁告鍟块悾椋庢喆閸曨參妾紓浣割儐鐎笛兾ｉ鍕拺闁告繂瀚埢澶愭煕濡湱鐭欑€殿喗鐓￠、鏃堝醇閻斿弶瀚藉┑鐐舵彧缁插潡鎮洪弮鍫濆惞婵炲棗绮鹃弮鍫熷亹闁肩⒈鍓涢濠囨倵閸偅绶查柨鏇畵楠炴垿宕熼姣尖晠鏌ㄩ弮鍌滃笡缂?0 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃闁硅櫕鎹囬垾鏃堝礃椤忎礁浜鹃柨婵嗙凹缁ㄥジ鏌熼惂鍝ョМ闁哄矉缍侀、姗€鎮欓幖顓燁棧闂備線娼уΛ娆戞暜閹烘缍栨繝闈涱儐閺呮煡鏌涘☉鍗炲妞ゃ儲宀稿缁樻媴閽樺－鎾绘嫅鏉堚晝纾奸悹鍥皺婢э妇鈧鍠栭…鐑藉箖閵忣澀鐒婂ù锝呮贡娴滄牠姊绘担绋款棌闁绘挸鐗撳畷鏉课旈崨顔尖偓鍓佲偓骞垮劚椤︿即鍩涢幋锔界厱婵犻潧妫楅顏呫亜閵壯冧户缂佽鲸甯￠、娆撴嚃閳哄啫濮遍梻浣虹《閺呮繈宕戦妶澶屽祦閻庯綆鍠楅弲婊堟煢濡警妲锋慨瑙勵殜濮婄粯鎷呴崨濠呯缂備緡鍣崹鎶藉极椤曗偓閸╋繝宕掑鐓庢闂備線鈧偛鑻晶瀛樻叏婵犲嫮甯涢柟宄版嚇瀹曘劍绻濋崒娑欑暭濠碉紕鍋戦崐鎴﹀礉鐏炶娇娑樜旈崨顔间患濠电娀娼ч鍛不閻熸噴褰掓晲閸℃瑦鎲欓梺纭呭Г濞茬喎顫忓ú顏勬闁靛闄勯悵鏃堟⒑閸濄儱娅忛柛瀣閳诲酣濮€閵堝棗鈧兘鏌ｉ幋鐏活亪鍩€椤掑倹鏆柡灞诲妼閳规垿宕卞▎蹇撴瘓缂傚倷闄嶉崝蹇撐涢崟顖涚畳婵犵數濮撮敃銈夊疮娴兼潙鏄ラ柨婵嗘閸欏繐鈹戦悩鎻掓殲闁靛洦绻勯埀顒冾潐濞诧箓宕戞繝鍌滄殾闁绘柨鍚嬮ˉ鍫熺箾閹寸伝顏埶夊澶嬧拻闁稿本鑹鹃埀顒勵棑缁牊鎷呯憴鍕妳濠电姴锕ら幊鎰矆婵犲伅褰掓晲閸涱収妫岄梺鎼炲€栧ú鐔煎箖瑜版帒鐐婄憸搴ㄥ煝閺囥垺鐓曟慨姗嗗墻閸庡繑銇勯鈩冪《闁圭懓瀚粭鐔碱敍濮橆剙顥嶉梻鍌欐祰閻偊宕橀…鎴滅棯缂傚倷鑳剁划顖滄崲閸喐鍙忛柍褜鍓熼弻銊モ攽閸℃﹩妫ら梺闈涙閸旀洟鈥旈崘顔嘉ч柛鈩冾殘娴犳挳姊洪悷鏉挎毐闂佸府缍佸畷娲閻欌偓閸氬鏌涢顐簼闂夊绱撻崒姘偓宄懊归崶銊ｄ粓闁归棿绀佺粻鏉库攽閻樺疇澹樼紒鐙€鍨堕弻娑樷槈濞嗘劗绋囩紓浣哄Х閺佸寮婚悢鐓庝紶闁告洦鍘滈姀掳浜滈柕澶涘椤ｈ尙绱掔紒妯肩疄闁诡喕绮欏Λ鍐归崜浣镐槐闁哄本鐩俊姝岊槻閻㈩垰鐖奸弻宥囩磼濡崵鍙嗛梺瀹犳椤︾敻宕哄Δ鍛厴闁诡垎鍌氼棜闂備焦瀵уú鏍磻閸℃稑鏋侀柛鈩冪⊕閻撴洟鏌￠崶銉ュ濠⒀屽枛闇夋繝濠傚閻帗鎱ㄦ繝鍛仩闁告牗鐗犲鎾倷濞村瀚熺紓鍌氬€风粈渚€顢栭崱娆愭殰闁跨喓濮寸粻鏍煃閵夛附鐏遍柡瀣叄閺岀喖骞嶉搹顐ｇ彃缂備降鍔岄…宄邦潖濞差亜浼犻柛鏇ㄥ帨閻樼粯鐓曢柡鍌氭健閸欏嫰鏌涢埞鍨姕鐎垫澘瀚埀顒婄祷閸斿鑺辨繝姘拺闂傚牊渚楀Σ鍫曟煕鎼淬垺銇濈€规洘娲熼獮瀣偐閻㈢绱查梺鍝勵槸閻楀嫰宕濈仦鐭綊顢欑亸鏍ㄦ杸闂佹枼鏅涢崯顖炲磹閹邦兘鏀介柨娑樺閸樻挳鏌℃担绋挎殻闁糕斁鍋撳銈嗗笂閼冲爼銆呴悜鑺ョ厵缂備降鍨归弸鐔兼煃闁垮鐏撮柟顔肩秺瀹曞爼鏁愰崒姘闂佹儳娴氶崑鍡樼椤忓牊鈷掑ù锝堫潐閸嬬娀鏌涢妸锔姐仢闁诡噯绻濋弫鎾绘偐閸欏袣闂備線娼х换鍡涘礈濠靛纾块柣鏂垮悑閻撳啴姊哄▎鎯х仩濞存粓绠栧娲传閸曢潧鍓伴梻浣稿簻缂嶄礁顕ｆ繝姘亜闁稿繒鍘ч埀顒勬敱閵囧嫰骞掑鍥舵М闂佸憡鎸哥紞濠傤潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯閸犳氨妲愰幒鎾崇窞濠电姴鍋嗛弳銏犫攽閳ュ啿绾ч柛鏃€鐟╅悰顔嘉熺亸鏍т壕婵炴垶顏鍫晛闁瑰墽绮埛鎴︽煕濞戞﹫鏀婚悘蹇斿閹叉悂寮堕崹顔芥閻庤娲樼划鎾愁嚕鐠鸿　鏋庨柣鎰靛墮閻︽粓姊绘笟鈧褔鎮ч崱娆愬床闁圭儤顨嗛崑鈺呮⒑椤掆偓缁夌敻鎮￠悢鑲╁彄闁搞儯鍔嶉埛鎺撱亜閿濆牆鐨虹紒杈ㄥ笒閻ｆ繈宕橀鍏兼嚈缂傚倷鑳剁划顖滄崲閸繄鏆﹂柟顖炲亰濡嫰姊洪崫銉ヤ粶闁绘牜鍘ч～蹇撁洪鍕炊闂佸憡娲﹂崜娆戠玻濞戙垺鈷戠紒瀣儥閸庢劙鏌ｉ埡濠傜仸妤犵偛锕獮妯兼嫚閺屻儲鏆呮繝寰锋澘鈧挾鎷嬮弻銉ョ婵炲樊浜濋悡鐔煎箹缁懓澧查悹鎰ㄢ偓鏂ユ斀妞ゆ梻鍋撻弳顒€鈹戦埄鍐╁唉鐎规洘锕㈤崺锟犲焵椤掑倹宕查柛鈩冪⊕閻撶喖鏌熼柇锕€骞楃紓宥嗗灩缁辨帒螖娴ｄ警鏆＄紓浣虹帛閻╊垶骞婂┑瀣劷闁挎洍鍋撴繛鍫幖椤啴濡舵惔鈥茬凹缂備緡鍠楅幑鍥х暦濞差亜鐒垫い鎺嶉檷娴滄粓鏌熼崫鍕棞濞存粍鍎抽—鍐Χ鎼粹€茬盎缂備胶绮敮鐐参ｉ幇鏉跨闁瑰啿纾崰鎾诲箯閻樺樊鍟呮い鏃傛嚀娴滈箖鏌熼崜褏甯涢柍閿嬪灴閺屾稑鈽夊鍫濆缂傚倸鍊瑰畝鎼佸蓟濞戞瑦鍎熼柕鍫濇瑜把冾渻閵堝啫濡介柣鐔村劦閸╃偤骞嬮敂钘変汗闂佸壊鐓堥崑鍛掗崟顖涒拺缂佸顑欓崕鎰版煥閺囨ê鐏╅柣锝囧厴閹晝绱掑Ο杞版睏闂佽崵濮村ú锕併亹閸愵亞鐜绘繛鎴烇供濞撳鏌曢崼婵囶棤濠⒀屽墰缁辨帡顢欐總绋垮及閻庤娲忛崹浠嬪箖閳哄啰纾兼俊顖滃帶楠炴姊绘担铏瑰笡闁哄被鍔戦獮妤€顭ㄩ崼鐔峰壒闁诲繒鍋熼悺鏃堟偄閸℃瑦鍠愰幖娣妸閳ь剙鍟村畷濂稿Ψ閵夛妇鈧剙顪冮妶鍡樼５闁稿鎸婚〃銉╂倷閺夋垹鐟ㄩ柧缁樼墪闇夐柨婵嗘川閵嗗﹪鏌涚€ｎ偅宕岀€规洜顭堣灃濞达絽鎼獮鍫熶繆閻愵亜鈧倝宕㈡禒瀣瀭闁割煈鍋嗛々鏌ユ煛瀹ュ海浜归柣鐔煎亰閻撱儵鏌涢弴銊ュ箻闁绘挸顦靛娲传閸曨厾浼囬梺鍝ュУ閻楁洟鎮鹃悜绛嬫晝闁挎洍鍋撶紒鈧€ｎ偁浜滈柟鐐墯濡插爼鏌涙繝鍐疄鐎殿喛顕ч埥澶愬閻樻剚妫熼梺鑽ゅУ娴滀粙宕滄潏顐▌闂佸搫澶囬埀顒€纾弳鍡涙倵閿濆骸澧扮悮锕€鈹戦悩娈挎毌闁告挻绻堥獮鍐磼閻愬弶鐎梺鍛婂姦閸犳牠鎮為崹顐犱簻闁圭儤鍨甸顏堟煕婵犲倻鍩ｉ柡宀€鍠撶槐鎺楀閻樺磭浜堕梻浣虹帛閹歌崵绮欓幘璇茬劦妞ゆ巻鍋撻柛妯荤矒瀹曟垿骞樼紒妯衡偓鐢告煥濠靛棝顎楀褎澹嗛幃顕€鏁傞幋鎺旂畾闂侀潧鐗嗗ú銈呮毄闂備胶顭堥鍥磻閵堝绠栭柣鎴ｆ閻撴稑霉閿濆洤鍔嬫繛鍫ョ畺濮婅櫣绱掑Ο鐑╂嫽闂佸啿鍢查悧鎾愁嚕椤愶箑鐐婃い鎺嶈兌閸樿棄鈹戦埥鍡楃仴婵炲拑绲剧粋鎺戔槈濞嗗秳绨诲銈嗗姂閸ㄥ搫锕㈤幍顔剧＜?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幘缂佺偓婢樺畷顒佹櫠缂佹ü绻嗛柡灞诲劙閹查箖鏌″畝瀣М闁诡喓鍨藉畷顐﹀Ψ瑜忛崢鎺戔攽閻橆偅濯伴柛鏇炵仛閻ｅジ姊烘潪鎵窗闁革綇缍侀悰顕€骞掗幊铏閸┾偓妞ゆ帒瀚崹鍌炴煕瀹€鈧崑鐐烘偂閺囩姭鍋撻崗澶婁壕闂侀€炲苯澧寸€规洘鍨块幃娆撴倻濡桨绨甸梻浣虹帛濡啴寮查懠顒佸床闁糕剝绋掗悡蹇涙煕椤愶絿绠撻柍褜鍓欓幉锛勭矉瀹ュ棎鍋呴柛鎰ㄦ杹閹疯櫣绱撻崒娆戝妽闁挎洍鏅涢埢鎾诲籍閸喓鍙嗛梺鍝勬储閸斿鑺辨繝姘厽闁挎繂娲ら崢鎾煛娴ｇ懓濮嶇€规洖銈告慨鈧柕蹇婂墲缁额偄鈹戦悩鍨毄闁稿鍋ゅ畷褰掑醇閺囩喎浠悷婊勬煥閻ｇ兘濮€閵堝懐顔掔紓鍌欑劍鑿ら柟閿嬫そ濮婃椽宕ㄦ繝鍕ㄦ闂佹寧娲╂俊鍥╁垝婵犳艾绠荤紓浣姑埀顒€鐏氶幈銊ノ熼悡搴′粯閻庢鍣崰姘跺焵椤掑喚娼愭繛鍙壣戠换娑欑節閸屾粍娈鹃梺缁樺灩閻℃棃寮崶顒佺厽婵☆垳鈷堥弳婊呯磼閸楃偛绾уǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘肩粈澶愭煙鏉堝墽鐣遍柛銊ュ€荤槐鎾存媴閼测剝鍨甸埢宥堢疀濞戞瑢鎷婚梺绋挎湰閻燂妇绮婇悧鍫涗簻闁哄洨鍠撴晶鐢碘偓瑙勬磻閸楀啿顕ｆ禒瀣垫晝闁靛牆娴傚Σ绋库攽閻樺灚鏆╁┑顔芥尦瀹曟劖绻濆顒佽緢闂佹寧鏌ㄩ～鏇熺濠婂牊鐓欓柡澶婄仢椤ｆ娊鏌ｉ敐鍫滃惈缂佽鲸甯￠崺鈧い鎺戝€甸崑鎾绘晲鎼粹€茬按婵炲瓨绮嶇划鎾诲蓟閻旂厧浼犻柛鏇ㄥ帨閵堝棎浜滈柨鏃囧亹閻ｇ敻鏌＄仦鍓ф创妤犵偞锕㈤幊婊堝垂椤愶絿妲楀┑鐘愁問閸犳牠鏁冮妶澶婄妞ゆ劧绲惧▍蹇涙⒒娴ｈ鍋犻柛搴㈢矒瀹曘劑顢橀悙鎰╁€濆濠氬磼濞嗘埈妲繝銏㈡嚀閿曨亪骞冮敓鐘查唶闁靛鍎抽ˇ顓㈡⒑缂佹ɑ顥堟い銉︽崌瀵娊鏁冮崒娑氬幐闂佹悶鍎崕閬嶆倶椤忓牊鐓曢悗锝冨妼婵¤法绱掓潏銊ョ闁逞屽墾缂嶅棝宕㈡總绋跨闁汇垹鎲￠悡娆撴煠閹帒鍔滅紒鈧€ｎ偅鍙忓┑鐘叉噺椤忕姷绱掓潏銊ョ瑨閾伙綁鏌ｉ幋鏃€娅嗛悗姘矙濮婄粯鎷呮笟顖滃姼缂備降鍔嶅钘夌暦閹版澘鍗抽柕蹇娾偓鍏呮偅闂傚倷绀侀悘婵嬵敄閸涱垱顐介柣鎰劋閻撴瑩姊洪銊х暠妤犵偞锕㈤弻娑樜熼幐搴闂佸疇顫夐崹鍧椼€佸▎鎾虫闁靛牆瀚潻鏃堟⒒娴ｅ憡鍟炴い顓炴喘瀵煡顢曢妶鍡╂綗闂佺粯鍔曢幖顐ょ不閿濆棛绡€闁靛骏绱曠粻鎾剁磽瀹ュ拑宸ラ柣锝呭槻铻栭柛娑卞枛娴滄鏌熼懖鈺勊夋俊鎻掓噹椤洭寮撮悩鐢碉紳闂佺鏈悷锔剧矈閺夋娓婚悗娑櫳戦崐鎰殽閻愭潙鐏寸€规洘鍎奸ˇ鎾煛閸☆厾鐣甸柡宀€鍠栭幃婊兾熼搹鐟板Ъ婵犵數鍋涢惃鐑藉疾濠婂牆绠為柕濞炬櫆閺呮煡鏌涢埄鍐噭闁革綇缍佸娲箹閻愭彃顬夊┑锛勫仩婵倖绌辨繝鍥ㄥ仺缂佸娉曢ˇ鏉款渻閵堝棛澧慨妯稿姂椤㈡﹢宕崟銊︽杸闂佺粯顭堢亸娆愮濞戙垺鐓曢柡鍐ｅ亾闁绘绮撻幃鐤樄婵﹤顭峰畷鎺戔枎閹板灚鈻婄紓鍌欑窔绾悂宕板顒夊殫濠电姴鍟伴々鐑芥倵閿濆簼绨荤紒銊ｅ劦濮婃椽宕崟顕呮蕉闂佺瀵掗崳锝夊春閳ь剚銇勯幒鎴姛闁伙綁娼ч埞鎴﹀焺閸愨晛鈧劙鏌涢埞鎯у⒉闁瑰嘲鎳忕粭鐔碱敍濞村墎鍋ゅ濠氬磼濞嗘劗銈板銈嗘礃閻楃姴鐣风憴鍕劅闁靛鍎抽ˇ鏉款渻閵堝棗濮夐柛鐘插缁傚秹鎮欓鍙ョ盎闂佸搫绋侀崑鍕閿曞倹鐓冮梺鍨儏缁楁帡妫佹径鎰叆婵犻潧妫涙晶杈ㄧ箾閸忕厧濮嶉柡灞剧〒閳ь剨缍嗛崜娆撳煝閸儲鐓涢悘鐐插⒔閳洟鏌熼娑欘棃濠殿喒鍋撻柟楦挎珪缁傛帡濮€閵堝棌鎷洪柣鐘叉礌閳ь剝娅曢悘宥夋⒑閹稿骸鍝洪柡灞界Ф閹叉挳宕熼銈勬偅闂備線鈧偛鑻晶鍓х磽瀹ュ懏顥㈢€规洘鍨垮畷銊╊敇濞戞瑧鈧椽姊洪幖鐐插姶闁告挻宀搁崺娑㈠箣閻樼數锛滈柣搴秵閸嬪嫰顢氬鍕瘈闁逞屽墴楠炲秹顢欓崜褝绱查梺璇插嚱缂嶅棝宕戦崨顓犳殾鐎光偓閳ь剟鍩€椤掑喚娼愭繛鎻掔箻瀹曞綊鎸婃竟鈺傜洴瀹曟﹢濡搁姀鈽嗘綌闂佸搫顦悧鍕礉瀹€鍕剨婵犲﹤鐗婇埛鎺戙€掑锝呬壕濠电偘鍖犻崶銊ヤ罕闂佺硶鍓濋妵鍌氣槈濡粍妫冨畷銊╊敊鐏忔牕浜鹃柛顐ｆ礃閻撱儵鏌￠崶鈺佷粶闁逞屽墮缂嶅﹤鐣烽幇鐗堝仺闁汇垻鏁搁敍婊冣攽鎺抽崐鎾绘嚄閸洖鐓″鑸靛姈閻撴瑩鏌ц箛锝呬簻濠殿喖绉归弻鐔碱敍濮樿京鍔悗瑙勬礀閵堝憡淇婇悜鑺ユ櫆闁诡垎鍐啈婵犵數濮撮惀澶愬级鎼存挸浜鹃柟鐗堟緲绾惧鏌熼崜褏甯涢柣鎾跺枑娣囧﹪濡堕崒姘闂佽瀛╅崙褰掑储閸撗冨灊閻庯綆浜堕崥瀣煕濠娾偓閼冲爼宕ｉ崱娑欌拺闁告挻褰冩禍鐐烘煕閻樿櫕宕岀€规洏鍨介獮鏍ㄦ媴閸忓瀚肩紓鍌欑椤戝棛鈧瑳鍐胯€块柟娈垮枤绾惧ジ鎮楅敐搴′簻闁诲繐鐡ㄩ妵鍕闄囨竟妯汇亜椤撴粌濮傜€规洘锕㈠畷婊嗩槻闁轰焦鎮傚娲嚒閵堝憛銏ゆ煙绾板崬浜版鐐寸墵椤㈡洟鏁傜紒妯绘珕婵＄偑鍊栭崝鎴﹀磹濡ゅ啫顥氬┑鍌滎焾缁狙囨煕椤愶絿鈽夊┑陇鍋愮槐鎺戭渻閿曗偓濞诧箓鎮￠崘顏呭枑婵犲﹤鐗嗙粈鍫ユ煏婵炑冩噽椤︻垶姊虹化鏇炲⒉妞ゎ厼娲妴鍛村蓟閵夛箑浠柡澶屽仦婢瑰棛绮旈崸妤佺叆婵犻潧妫欓崑顏呯箾瀹割喕鎲鹃柡浣稿€块弻娑樷槈閸楃偟浠╅柣銏╁灡椤ㄥ﹪寮婚敐澶嬪亜闁告縿鍎抽悡浣虹磽娓氬洤鏋熼柟鍛婃倐閹儳鐣￠柇锔惧弳闂佸壊鍋呴幆濠囧煛閸愨晛鏋戦棅顐㈡处濞叉粓鎯岄崱妞绘斀闁绘ê寮舵径鍕煟閹惧瓨绀嬮柡灞界Ч閸┾剝鎷呴崨濠冾啀闂備礁鐤囧Λ鍕囬柆宥嗗剦妞ゅ繐鐗嗙粻姘辨喐濠婂牊鍋傚┑鍌氭啞閻撴盯鎮橀悙鎻掆挃闁宠棄顦甸弻宥夋寠婢舵ɑ笑闁句紮缍侀弻娑滅疀閹捐櫕鍊梺鍝ュУ閻楃姴顫忕紒妯诲闁荤喐澹嬮弸娆撴⒑鐞涒€充壕闂佸憡顨堥崕鎰焽閺嵮€鏀介柣妯虹枃婢规﹢鏌ｉ幒鎴犱粵闁靛洤瀚伴獮鎺楀幢濡炴儳顥氱紓鍌氬€烽懗鑸垫叏鐎靛摜鐭撻柣鐔稿閺嗭箓鏌￠崶銉ョ仾闁绘帟鍋愰埀顒冾潐閹稿爼宕曢崡鐏绘椽鏁傞柨顖氫壕濞达絽鍟禍褰掓煃瑜滈崜姘辩矙閹烘鏅┑鐐茬摠缁秹銆冩繝鍌ゆ綎婵炲樊浜濋ˉ鍫熺箾閹达綁鍝烘い搴℃缁绘繄鍠婂Ο宄颁壕闁惧浚鍋勭粣娑氱磽娓氬洤鏋ょ紒顕呭灦婵″爼鏁愭径濠勵槰闂佸啿鎼崯顐︾嵁瀹ュ鈷戦悹鍥у级閹癸綁鏌℃担瑙勫€愰柍銉畵瀹曞ジ鎮㈡笟顖涚カ闁荤喐绮嶅Λ鍐嚕婵犳碍鍋勯柣鎾虫捣椤︺劑姊洪崨濠勬噧妞わ箒妫勮灋闁绘劕妯婂〒濠氭煏閸繈顎楃€殿喓鍔嶇换娑欏緞鐎ｎ偆顦板Δ鐘靛仜椤︾敻鐛鈧、娆撴嚃閳哄﹥效闂傚倷娴囬鏍垂鏉堛劎鐝堕悗锝庡枛绾惧鏌熼幑鎰靛殭闂佽￥鍊栨穱濠囧Χ閸曨喖鍘￠梺鍛娚戦幃鍌炲箖濡ゅ懎绀傚璺猴梗婢规洟姊绘笟鈧埀顒傚仜閼活垱鏅剁€电硶鍋撶憴鍕；闁告鍟块锝嗙鐎ｅ灚鏅ｅ┑鐘欏嫬鍔ゅù婊勫劤闇夐柨婵嗘川閵嗗﹥淇婇幓鎺斿濞ｅ洤锕、娑㈡倷鐎涙ɑ娈搁梻浣筋嚙妤犳悂鈥﹀畡閭︽綎婵炲樊浜濋崑锟犳⒑椤撱劎鐣辨繛鍫ｅ煐缁绘繂鈻撻崹顔界亪闂佹悶鍨洪悡锟犲箖妤ｅ啯鍊婚柦妯侯槼閹芥洟姊洪棃娑辨濠碘€虫喘瀹?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垟鎳為崶顒佸仺缂佸瀵ч悗顒勬⒑閻熸澘鈷旂紒顕呭灦瀹曟垿骞囬悧鍫㈠幘缂佺偓婢樺畷顒佹櫠缂佹ü绻嗛柡灞诲劙閹查箖鏌″畝瀣М闁诡喓鍨藉畷顐﹀Ψ瑜忛崢鎺戔攽閻橆偅濯伴柛鏇炵仛閻ｅジ姊烘潪鎵窗闁革綇缍侀悰顕€骞掗幊铏閸┾偓妞ゆ帒瀚崹鍌炴煕瀹€鈧崑鐐烘偂閺囩姭鍋撻崗澶婁壕闂侀€炲苯澧寸€规洘鍨块幃娆撴倻濡桨绨甸梻浣虹帛濡啴寮查懠顒佸床闁糕剝绋掗悡蹇涙煕椤愶絿绠撻柍褜鍓欓幉锛勭矉瀹ュ棎鍋呴柛鎰ㄦ杹閹疯櫣绱撻崒娆戝妽闁挎洍鏅涢埢鎾诲籍閸喓鍙嗛梺鍝勬储閸斿鑺辨繝姘厽闁挎繂娲ら崢鎾煛娴ｇ懓濮嶇€规洖銈告慨鈧柕蹇婂墲缁额偄鈹戦悩鍨毄闁稿鍋ゅ畷褰掑醇閺囩喎浠悷婊勬煥閻ｇ兘濮€閵堝懐顔掔紓鍌欑劍鑿ら柟閿嬫そ濮婃椽宕ㄦ繝鍕ㄦ闂佹寧娲╂俊鍥╁垝婵犳艾绠荤紓浣姑埀顒€鐏氶幈銊ノ熼悡搴′粯閻庢鍣崰姘跺焵椤掑喚娼愭繛鍙壣戠换娑欑節閸屾粍娈鹃梺缁樺灩閻℃棃寮崶顒佺厽婵☆垳鈷堥弳婊呯磼閸楃偛绾уǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘肩粈澶愭煙鏉堝墽鐣遍柛銊ュ€荤槐鎾存媴閼测剝鍨甸埢宥堢疀濞戞瑢鎷婚梺绋挎湰閻燂妇绮婇悧鍫涗簻闁哄洨鍠撴晶鐢碘偓瑙勬磻閸楀啿顕ｆ禒瀣垫晝闁靛牆娴傚Σ绋库攽閻樺灚鏆╁┑顔芥尦瀹曟劖绻濆顒佽緢闂佹寧鏌ㄩ～鏇熺濠婂牊鐓欓柡澶婄仢椤ｆ娊鏌ｉ敐鍫滃惈缂佽鲸甯￠崺鈧い鎺戝€甸崑鎾绘晲鎼粹€茬按婵炲瓨绮嶇划鎾诲蓟閻旂厧浼犻柛鏇ㄥ帨閵堝棎浜滈柨鏃囧亹閻ｇ敻鏌＄仦鍓ф创妤犵偞锕㈤幊婊堝垂椤愶絿妲楀┑鐘愁問閸犳牠鏁冮敂鎯у灊妞ゆ牜鍋涚粻顖炴煕濞戝崬鐏￠柛鐘叉閺屾盯寮撮妸銉ょ暗闂佸憡绻冮〃濠傤潖缂佹ɑ濯村〒姘煎灣閸旀悂姊洪幐搴㈢８闁稿海鏁婚獮鍐槻妞ゎ厹鍔戝畷鐓庘攽閸モ斁鍋撻鈧—鍐Χ閸℃锛曢梺绋款儐閹瑰洭寮诲☉銏犖╅柍鍝勫暟閺嗩偄螖閻橀潧浠滄繛璇х畵楠炲繘宕ㄩ弶鎴濈獩婵犵數濮存鎼佸箖椤忓牊鈷掗柛灞捐壘閳ь剟顥撶划鍫熸媴閸︻厾褰炬繝鐢靛Т濞层倗澹曟繝姘厱婵炴垵宕慨鍛婄箾瀹割喕绨婚柛鎰ㄥ亾婵＄偑鍊栭幐楣冨窗鎼淬垻顩叉俊銈傚亾闁宠鍨块幃娆撳级閹寸姳鎴烽梻浣规偠閸斿秴顪冩禒瀣畺闁跨喓濮撮崡鎶芥煏韫囥儳纾块柛妯兼暬濮婃椽鏌呭☉姘便偡濠电偛鎳忓ú鏍煝瀹ュ宸濇い鎾寸⊕閺傗偓闂備焦瀵х粙鎴犫偓姘煎墴瀹曟繄鈧綆鍓涚壕鑲╃磽娴ｈ鐒介柍钘夘樀閺岋綁鏁愰崱娆戠暫缂備胶绮换鍐箔閻旂厧鐒垫い鎺戝绾剧粯淇婇妶鍛櫤闁抽攱鍨块弻娑樷攽閸℃浼€闂佺粯甯熷▔鏇犳閹炬剚鍚嬮柛鏇ㄥ幖婵′粙姊洪柅娑氣敀闁告梹鍨垮畷娲焵椤掍降浜滈柟鐑樺灥椤忣亪鏌ｉ幘鍐叉殻鐎殿喖鐖奸幃娆撳级閹搭厸鍋撳鍫熺厱闊洦鎹佺€氫即妫佹径鎰厽婵炲棗鑻禍楣冩⒑閸涘﹦鎳冮柛鐕佸亰閿濈偠绠涘☉娆愬劒闁荤喐鐟ョ€氼剟寮搁崒鐐粹拺闁告稑锕ユ径鍕煕濡湱鐭欐鐐差槸閳诲氦绠涙繝鍐╂澑闂備胶绮崝鏇烆嚕閸洖闂柣鎴烆焽缁犻箖鏌涘▎蹇ｆ▓闁绘帞鏅槐鎺楊敊閻ｅ矈鏆″┑鐐靛帶缁绘ɑ淇婂宀婃Ь闂佷紮绠戦悧鎾愁潖濞差亜宸濆┑鐘插暟閿涙﹢姊洪崨濠冪叆闁哄牜鍓涚划瀣吋婢跺鈧攱銇勯幒鎴濃偓濠氼敊閺囥垺鈷戦柣鐔煎亰閸ょ喖鏌涚€ｎ剙鏋涙鐐诧躬楠炲洭鎮ч崼銏犲箺闂備胶鎳撻幖顐ょ矓閸洘鍎楁繛鍡樻尰閻撴稓鈧厜鍋撻悗锝庡墰琚﹂梻浣筋嚃閸犳捇宕瑰ú顎洭鎮ч崼鐔峰妳闂佹寧绋栭崺鍥礉閹存繍娼栨繛宸簻娴肩娀鏌涢弴銊ユ灈妞ゅ浚鍘剧槐鎾存媴閸撳弶楔闂佺绻戦…鍥儊椤掍胶绠鹃柟鐐綑閻掑綊鏌涚€ｎ偅宕岄柡宀嬬磿娴狅箓宕滆閸掓稑螖閻橀潧浠︽い顓炴喘閸┾偓妞ゆ帒瀚☉褔鏌熺拠褏绡€鐎规洝娅曠€佃偐鈧稒菤閹疯櫣绱撴笟鍥х仭婵炲弶锕㈠璺好洪鍛幈闂侀潧顭堥崐妤呮儗濞嗘挻鐓熸繛鎴濆船閺嬫盯鎽堕敐澶嬬厱闊洦娲栫敮鍫曟煟濠靛懎宓嗛柟顔筋殜閹兘寮跺▎鐐棏闂備礁鎽滄慨鐢搞€冮崼銏☆潟闁绘劕鎼壕濂告煟閹邦喛藟闁归绮换娑㈠箻閺夋垹鍔伴梺绋款儐閹歌崵鎹㈠☉娆愬闁告劕寮舵径鍕煕鐎ｎ亶鍎旈柡灞剧洴閸╁嫰宕橀浣诡潔缂傚倷鑳舵慨閿嬬箾閳ь剟鏌″畝鈧崰鏍嵁閹达箑绠涢梻鍫熺⊕閻ｄ即姊绘担鍛婃儓妞わ附婢橀—鍐寠婢舵ɑ缍庢繝鐢靛У閼瑰墽澹曢崗鑲╃闁糕剝锚缁楁帡鏌涘鍫㈢瘈婵﹨娅ｅ☉鐢稿川椤旀儳娑ч梻浣告贡閹虫挸煤閵堝鍋╅柣鎴ｆ鍞悷婊冾樀瀹曟垿骞樼紒妯绘珳闁硅偐琛ラ崜婵嬫倶瀹ュ鈷戠紒瀣儥閸庡矂鏌￠崨顔剧煉濠碉紕鏁诲畷鐔碱敊閸撗勬緫闂備浇顫夊畷妯衡枖濞戭潿鈧倸煤椤忓應鎷洪梺纭呭亹閸嬫稒淇婃總鍛婄厱濠电姴鍊圭拹锟犳煙楠炲灝鐏╅柍钘夘槸椤粓宕卞Δ浣圭彋濠碉紕鍋戦崐鏍涙担鍝ユ殾闁汇垻顭堢粈鍕喐閻楀牆绗氶柍閿嬪灩缁辨帞鈧綆浜滈惃锛勨偓瑙勬偠閸庤尙鎹㈠☉姘厹濡炲娴烽惁鍫ユ倵濞堝灝鏋涙い顓犲厴瀵偊骞囬鐐电獮闁诲函缍嗛崑鍛存偟閹惰姤鈷掑ù锝呮啞閸熺偛銆掑顓ф疁鐎规洩绲鹃幆鏃堝焾閵夘垳绉柟顔荤矙瀹曘劍绻涢悙顒€顏圭紓鍌氬€风粈渚€顢栭崱娑辨晞婵炲棙鎸哥壕鍧楁煙閹殿喖顣奸柣鎾寸懇閺屾稑鈽夊鍫熸暰闂佺锕ラ崝娆撳蓟濞戙垹鐓橀柛顭戝枤娴煎牆螖閻橀潧浠︽い顓炴喘楠炲繘鎮╃拠鑼唽闂佸湱鍎ら弻锟犲磻閹炬椿鏁囬柣鎴濇鐎靛矂姊洪棃娑氬婵☆偅鐟╅崺娑㈠箳濡や胶鍘搁梺鍛婄矆缁€浣圭濠靛牏纾奸柛灞炬皑鏁堝Δ鐘靛仜濞差參骞冭瀹曠厧顫濋鐑嗕紲?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊绋掑婵堢磼閳锯偓閸嬫捇姊绘担渚劸闁哄牜鍓涢崚鎺戠暆閸旇偐鍏橀崺鈧い鎺戝閳锋帒霉閿濆嫯顒熼柣鎺斿亾閵囧嫰骞嬮悙鑼患濠碘€冲级閸旀瑥顕ｉ幘顔碱潊闁绘鏁搁弸鈧梻鍌欑缂嶅﹤螞鐠恒劎鐭嗗ù锝堟鐏忕數鈧箍鍎遍ˇ浼存偂閺囩喆浜滈柟鎵虫櫅閳ь剚娲熷鎼佸箣閻愮數顔曢梺鍛婁緱閸犳岸鎯岄幒鎾村弿濠电姴鎳忛鐘电磼鏉堛劌绗掗摶锝夋煕濠靛棗顏存繛鍏兼崌濮婄粯鎷呴崨濠呯闂佸啿鍢查悧鎾崇暦閹达箑骞㈡繛鍡楃箰閻忓﹥绻濋悽闈浶ｉ柤鐟板⒔缁顢氶埀顒勫蓟閵堝绠掗柟鐑樺灥婵垽姊洪崨濠庣劷闁轰浇顕ч～蹇涙惞閸︻厾锛滃┑鈽嗗灠閹碱偉顤傞梻鍌欑閹诧繝骞愮拠宸僵闁靛ě鍕垫綗闂佺粯鍔曢幖顐ょ不閿濆鐓熼柟鏉垮悁缁ㄩ绱掗妸顭戝殭妞ゎ亜鍟存俊鍫曞幢濡も偓椤酣姊洪崫銉ユ瀻婵炲皷鈧磭鏆﹂柟杈剧畱缁犲鎮归崶顏勭毢闁汇倐鍋撴繝鐢靛仩閹活亞寰婇崸妤€绠犻柟鍓х帛閸庡﹥绻濇繝鍌滃闁抽攱甯掗妴鎺戭潩閿濆懍澹曟繝鐢靛仒閸栫娀宕堕妸銉ょ綍闂備胶纭堕崜婵嬪Φ閸屾埃妲堟俊顖涚矋濡啫鐣峰鈧、鏃堝幢濞嗘劖顔旈梻鍌氬€烽懗鍫曗€﹂崼銉ュ珘妞ゆ帒鍊搁崹婵嬪箹鏉堝墽鍒版い銉﹁壘閳规垿宕掑顓炴殘闂佺粯鎸搁崯浼村箟缁嬫鐓ラ柛顐ｇ箘椤︻厼鈹戦绛嬬劸婵炲绋撶划濠氼敍閻愬鍘卞┑鐐村灥瀹曨剟鎮橀敓鐘崇厱闁绘ê寮堕ˉ銏ゆ煛鐏炲墽娲寸€殿喗鎸虫俊鎼佸Ψ閵堝洨娉挎繝鐢靛Х椤ｄ粙鍩€椤掆偓閸熷潡寮抽柆宥嗙厓閻熸瑥瀚悘鎾煙椤旇娅婄€规洖缍婇、鏇㈡晲閸℃瑦顫栧┑鐘垫暩婵敻顢欓弽顓炵獥闁哄稁鍘介崑銈夋煏婵炑€鍋撻柛瀣崌濡啫鈽夐弽褌鍒掗柣搴ゎ潐濞插繘宕濋幋锕€绠犳繝濠傜墱閺佸嫰鏌熺粙鎸庢崳婵炲吋鐗犲濠氬磼濞嗘埈妲┑鐘亾闂侇剙绉寸壕鍧楁煏閸繍妲搁柛銊ュ€婚幉鎼佹偋閸繂鎯為梺鎼炲労娴滆泛螣閸曨偀鏀芥い鏃傘€嬪銉︺亜椤撶偛妲婚柣锝囧厴楠炴帡骞嬮鐔峰厞婵＄偑鍊栭崹鐓幬涢崟顒傤洸濡わ絽鍟悡鐘测攽椤旇棄濮囬柍褜鍓氶悧鏇㈡偩閻戣棄鐭楀璺虹灱閻﹀牓姊婚崒姘卞缂佸鎸婚弲鍫曞箛閻楀牏鍘遍柣搴秵閸嬪懐浜搁銏＄厓闁芥ê顦藉Ο鈧Δ鐘靛仦鐢繝鐛Ο灏栧亾閻㈡鐒剧紒鑼舵閳规垿鏁嶉崟顐℃澀闂佺顭堥崐鏍矉瀹ュ拋鐓ラ柛顐ｇ箘閺屽牓鏌ｆ惔銏⑩姇閻绱掔拠鍙夘棦闁哄被鍊楃划娆戞崉閵娿倗椹抽梻浣侯焾濞寸兘宕伴弽顓炶摕鐎广儱顦敮闂佹寧娲嶉崑鎾剁磼閼碱剛甯涢柕鍥у婵＄兘鏁傞悾灞绢吇缂傚倷娴囨ご鎼佸箲閸パ呮殾闁圭儤鍨熼弸搴ㄦ煙閻戞ê鐏ラ悽顖ｅ灠閳规垿鎮╅幇浣告櫛闂佸摜濮靛畝绋款嚕椤愩倖瀚氶柤纰卞墯濞堥箖鎮峰鍛暭閻㈩垱甯￠弻銊╁Χ閸℃洜绠氶梺闈涚墕閸婂憡绂嶆ィ鍐┾拺閻犲洩灏欑粻鎻掆攽閻愯韬€殿喖顭锋俊鎼佸煛娴ｈ櫣娼夐梻浣稿閸嬪棝宕伴幇顑芥灁濞寸厧鐡ㄩ埛鎴犵磼鐎ｎ偒鍎ラ柛搴㈠姍閺岀喖鎮烽悧鍫熸倷闂佷紮绲块崗姗€骞冨鍛┏閻庯綆鍋呭▍宥夋⒒娴ｈ櫣甯涢柛鏃€鐗犲畷婵嬪箣閿曗偓绾惧潡鏌ｉ姀銏╃劸闁藉啰鍠栭弻銊モ攽閸♀晜笑闁搞儲鎸冲铏圭矙閹稿孩鎷遍梺娲诲弾閸犳绮╅悢鐓庡嵆闁靛繆妾ч幏濠氭⒑缁嬫寧婀伴柤褰掔畺閸┾偓妞ゆ帒瀚峰Λ鎴犵磼椤旇偐澧涚紒妤冨枛閸┾偓妞ゆ帒瀚畵渚€鏌″搴″季闁轰礁鍟撮弻銊╁即濡も偓娴滃墽绱掗悙顒€鍔ょ紓宥咃躬瀵鏁冮埀顒冪亽婵炴挻鍑归崹鍗炵摥闂傚倷绀侀幖顐︻敄閸℃稒鍋嬮柣妯款嚙閽冪喖鏌ㄩ悢鍝勑㈤柣鎰躬閺屾洘寰勯崼婵嗗Б闂佸憡姊归幐鍐差潖?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋倕宸濆┑鐘插鑲栨繝寰锋澘鈧呭緤娴犲鐤い鏍剱閺佷胶鈧箍鍎遍ˇ浼村煕閹寸姷纾奸悗锝庡亽閸庛儵鏌涙惔锛勭闁靛洤瀚伴獮瀣攽閹邦厸鏋呴梻浣筋嚃閸ｎ垳鎹㈠┑瀣ㄢ偓渚€寮撮姀鈩冩珖闂侀€炲苯澧撮柟顔界懄缁绘繈宕堕妸褍甯惧┑鐘垫暩閸嬫盯鎮樺┑瀣劦妞ゆ帊绶″▓姗€鏌熼獮鍨仾缂佹鍠栭崺鈧い鎺嗗亾妞ゎ偄绻橀幃婊兾熺拠宸蒋闂備浇顫夐鏍闯椤斿槈锝夘敋閳ь剟寮婚敐鍡樺劅闁靛繈鍨婚崣婵嬫⒑閸涘娈旈柛鐔锋健椤㈡岸鏁愰崶銊ョ彴濠电偞娼欓鍡涳綖瀹ュ鈷戦梻鍫熺〒缁犲啿鈹戦鑲╁ⅵ濠碘剝鎮傞崺锟犲磼濠婂啫姹插┑鐘垫暩婵炩偓婵炰匠鍏炬稑鈻庨幘鎶芥７闁荤喐鐟ョ€氥劍绂嶅鍕╀簻闁瑰搫妫楁禍鎯ь渻閵堝棗鐏ユい锔诲灦閿濈偠绠涘☉妯圭炊闂佸憡娲﹂崢钘夆枔妤ｅ啯鈷戦柟鑲╁仜閸旀鏌￠崨顔俱€掔紒顕呭弮閸ㄩ箖寮妷锔芥澑闂備礁鐤囧Λ鍕涘Δ浣侯洸婵犲﹤鐗婇悡鏇㈡煟閺傛寧鍟炵紓宥嗗灦閵囧嫰寮撮鍡樺仹闂佸綊顥撴慨鎾敊韫囨侗鏁婇柛蹇曞帶閳ь剦鍨跺缁樻媴缁嬫妫岄梺绋款儐鐢€崇暦閹达箑宸濋柟纰卞幗閺呮盯姊洪幖鐐插姶闁告挻宀搁崺娑㈠箳閹炽劌缍婇弫鎰板川椤斿吋娈橀梻浣告憸閸犳捇宕戦妶澶婅摕闁绘棁娅ｉ惌娆愮箾閸℃鈷旈柛姘儔濮婅櫣鎷犻垾铏亾闂佺绻戦敃銏犵暦濞差亝鏅搁柣妯诲絻缁愭稑顪冮妶鍡樺暗闁哥姵鍔橀埅鏌ユ⒒閸屾艾鈧娆㈠顒夌劷鐟滃繘骞戦姀銈呭耿婵炴垶顭囬ˇ顓炩攽閻樼粯娑фい鎴濇噽婢规洟宕稿Δ浣哄幗闂佸搫鍊搁悘婵嬪箖閹达附鐓熼柟鎯х－缁犱即鏌嶇憴鍕伌闁诡喗鐟╅幊鐘活敄閼愁垱顎楅梻鍌欑閹诧繝寮婚敐澶婄９婵犻潧顑呴拑鐔哥箾閹存瑥鐏╃紒顐㈢Ч閺屽秷顧侀柛鎾跺枛閻涱噣骞囬弶璺槰閻庣懓澹婇崰妤冪礊鎼粹檧鏀介柣鎰级閳绘洖霉濠婂嫮鐭婃い鏂跨箰閳规垿宕辫箛鏃€鏉搁梻浣虹帛钃辩憸鏉垮暣閸┾偓妞ゆ帊鐒﹂崐鎰版煥濠靛牆浠﹂柟顖涙婵℃悂鏁傜憴鍕伖缂傚倸鍊风粈渚€顢栭崱娑辨晞婵炲棙鎸哥壕鍧楁煙閹殿喖顣奸柣鎾寸懇閺岋綁鎮㈤崫鍕垫毉闂佸摜濮甸…鍥焵椤掑喚娼愭繛鍙夌墵楠炴牠顢曢敃鈧悡鈥愁熆閼搁潧濮囨い顐㈡嚇閺岋絽螣鐠囪尙绁风紓浣风贰閸ｏ絽顫忕紒妯肩懝闁逞屽墮椤洩顦撮柟骞垮灲瀹曞崬螖婵犲嫧鍋撻崜浣瑰枑闁哄啫鐗忓畵渚€鐓崶銊﹀婵炲樊浜堕弫鍌炴煕濞戝彉绨奸悗姘偢濮婄粯鎷呴崨濠傛殘缂備礁顑嗛崹鍧楀极閸愵喗鏅滈柛鎾楀倻鐟濋梻浣烘嚀婢х晫鍒掗鐐茬９闁割煈鍋呴崣蹇斾繆椤栨粌甯堕悽顖氱埣閺岋綀绠涢妷褏鏆ら梺鍝勮閸斿矂鍩為幋锕€閱囨繝闈涙噺椤洟姊绘担鑺ャ€冪紒鈧笟鈧畷顖炲级閹寸姵娈鹃梺鍦劋閸ㄧ喎危閸喐鍙忔慨妤€妫楁晶顔剧磼閻橀潧浠辨慨濠勭帛閹峰懏绗熼娑欐殲闂備胶顭堥鍡涘礉濡ゅ懎绠栧ù鍏兼綑鍞梺瀹犳〃閼冲爼宕濋敃鈧—鍐Χ閸℃鐟ㄩ梺鍝ュ櫏閸ㄥ爼骞冮悽鍓叉晜闁告侗鍠栧鍧楁⒑閹稿海绠撴俊顐ｎ殕閹便劌顓奸崱妯哄伎婵犵數濮撮崯顖炲Φ濠靛浂娈介柣鎰綑婵牓鏌熼娑欘棃濠殿喒鍋撳銈庡亽閸犳岸宕板鈧濠氬磼濞嗘垵濡介梺璇″枛閻栫厧鐣峰┑濠勭杸闁圭偓鍔栭惄顖氱暦閻旂⒈鏁嶆繛鎴灻奸幃锝夋⒒娴ｈ櫣甯涢柛銊у帶铻為柛鏇ㄥ墮閸旀棃姊婚崒娆戭槮闁汇倕娲ら々濂稿Ω閳哄倸娈戦梺鐓庢啞椤旀牕鈻撴禒瀣厱闁靛鍠栨晶顖炴煃闁垮鐏撮柡宀嬬秮閹垽鎮℃惔婵嗘瀳闂備焦鎮堕崐妤呭窗鎼淬劌鐓橀柟杈惧瘜閺佸秹鏌ㄥ┑鍡樺櫣缂佺姴缍婇弻娑樜旈崨顒佸枤闂佸搫鐬奸崰鎾舵閹烘顫呴柣妯虹－娴滃爼姊绘担鐟扳枙闁衡偓鏉堚晜鏆滈柟鐑橆殕缁犳帡姊绘担鐟邦嚋缂佽鍊哥叅闁靛ň鏅涚粻娲煕鐏炲墽銆掔紒鐘荤畺閺屾稖绠涢弴鐐蹭粯婵犳鍠栭悧鎾诲蓟閺囥垹鐐婄憸宥夘敂椤撶姭鍋撳▓鍨珮闁革綇缍佸畷娲焵椤掍降浜滈柟鐑樺灥椤忣亪鏌涚€ｎ亶鍎旈柡宀嬬到铻ｉ柛顭戝枤濮ｃ垽姊洪崫鍕紨缂佺姵鎹囧濠氬焺閸愩劎绐為柟鍏兼儗閸犳牕鈻撴ィ鍐┾拺闁告繂瀚弳娆撴煟濡も偓濡顢欒箛鎾斀闁搞儯鍎扮花濠氭⒑閸濆嫬鏆欓柣妤€锕弫宥夋偄閸忚偐鍘靛銈嗘礀濡稓绮旈鑺ュ弿濠电姴鍋嗛悡鑲┾偓瑙勬礃鐢帡鍩㈡惔銊ョ闁挎繂娲ㄥ畷鍫曟⒒閸屾瑧顦﹂柟璇х節楠炴劖绻濆顓炰画閻熸粌绻樺畷姘跺箳濡も偓缁秹鏌涢銈呮瀻濞寸姵妞藉娲川婵犲嫧妲堥梺鎸庢磸閸婃牜绮嬪鍛傛棃鍩€椤掑嫬鐓橀柟杈鹃檮閸婄兘鏌℃径瀣仼濞寸姍鍥ㄢ拺闁硅偐鍋涙俊鑺ヤ繆閻愭壆鐭欐鐐插暣閺佹捇鎮╅幓鎺戠ギ闂備線娼ф蹇曟閺囥垹鍌ㄩ柟闂寸劍閳锋垿鏌熼懖鈺佷粶闁告梹锕㈤弻娑欐償閵娿倖鍠氶梺璇″枟閿曘垽銆侀弮鍫濆窛妞ゆ牗绮犻崯搴ㄦ⒒娴ｇ儤鍤€闁哥喎娼″畷鏌ュ箵閹规妞介弫鍐磼濞戞ü绨甸梻浣虹帛濡線宕戦幇鏉跨煑闁瑰墽绮悡鏇㈡煏婢舵稓鍒伴柛鏃€宀搁弻娑㈠棘鐠囨祴鍋撳Δ鍐╊潟闁圭儤顨呴悡銏ゆ煃瑜滈崜鐔风暦閹达箑绠荤紓浣骨氶幏缁樼箾鏉堝墽鍒伴柟璇х節楠炲棝宕奸妷锔惧幗濡炪倖鎸鹃崕鎰ｉ悜妯圭箚闁告瑥顦慨宥団偓娈垮枟閹告娊骞冮姀锛勭懝妞ゆ牗绋撻妶顐︽⒒閸屾瑧鍔嶉悗绗涘厾娲冀椤撶偟锛欓悷婊呭鐢帡宕ラ崷顓涘亾楠炲灝鍔氭い锔垮嵆閹ょ疀閹垮啰鍞甸梺鐓庢憸閺佺煤鐎涙ü绻嗙紓浣靛灩濞呭秹鏌＄仦鍓р姇缂佺粯绻堝畷鍫曞Ω閿旇姤顔忔繝鐢靛剳缁茶棄煤閵堝鏅濇い蹇撶墕缁犳牗绻涢崱妯绘儎闁轰礁鍊块弻娑㈠即濡搫顬堟繛瀛樼矒缁犳牠寮婚垾鎰佸悑閹艰揪绲煎Ч妤€鈹戦悙鍙夆枙濞存粍绮撻幃鈥斥槈閵忥紕鍘遍梺瑙勬緲閸氣偓缂併劏濮ら妵鍕晜鐠囪尙浠紓浣虹帛閻╊垶鐛鈧鍫曞箣閻樻彃袪闂傚倷鑳堕…鍫ヮ敄閸℃稑绠板Δ锝呭暙閻掑灚銇勯幒鎴濇灓婵炲吋鍔栫换娑㈠矗婢规繍浜垾鏃堝礃閳哄喚娴勯柣搴秵娴滄繂危椤掑嫭鈷戦梺顐ゅ仜閼活垱鏅堕鐐寸厪闁搞儜鍐句純濡ょ姷鍋炵敮鎺楊敇閸忕厧绶為悗锝囶暯閸嬫捇寮介妸褏顔曢柣搴㈢⊕椤洭鎯岀€ｎ剛纾兼い鏃囧Г瀹曞瞼鈧鍠栭…鐑藉箖閵忋倕绀傞柣鎾崇岸閸嬫挾绱掑Ο鍦畾濡炪倖鐗楃换鍐敂閻樿褰掓偐閾忣偆绋囧銈庝簻閸熷瓨淇婇崼鏇炲窛妞ゆ梻铏庡ú璺衡攽閻愯尙鎽犵紒顔惧У缁傚秹顢旈崟闈涙婵犵數濮电喊宥夊疾閹间焦鐓熸俊顖氭惈缁狙囨煏閸ャ劎绠栫紒缁樼⊕濞煎繘宕滆閸嬔冾渻閵堝骸骞栭柣妤佹崌閺佹劙鎮欐笟顖涙櫇闂佹寧妫佽闁归攱妞藉缁樼瑹閸パ傜敖闂佺顑嗛惄顖炲箠閻旂⒈鏁嶆繛鎴炵懄閻濆嘲顪冮妶鍡樼５闁稿鎹囬弻锛勪沪閸撗€妲堥柧浼欑秮閺屻倖鎱ㄩ幇顑藉亾濡ゅ懏鍋傚鑸靛姈閸婄敻鎮峰▎蹇擃仾缂佲偓閸愵喗鐓曢柕濞垮劜閸嬨儵鏌ｅ☉鍗炴灈閾伙綁鏌ゅù瀣珖闁挎稒绮岄埞鎴﹀煡閸℃浠梺鍛婎焼閸モ晛绁﹂梺鍓插亖閸庢煡鍩涢幒鎳ㄥ綊鏁愰崶鈺傛啒濠电偛鍚嬮崝鏍崲濠靛顫呴柍顓涙櫆閸ㄥ灝顕ｉ弻銉﹀殝闁割煈鍋勯崝鍛存⒑闂堟侗鐒鹃柛搴㈠▕钘濇い鎰堕檮閳锋垿鏌涘┑鍡楊伌婵℃彃缍婇弻銊モ槈濞嗘劗顦伴梺璇″枤閸忔﹢寮婚崶顒佹櫆闁诡垎鍐╄緢闂傚倸鍊风粈浣圭珶婵犲洤纾诲〒姘ｅ亾鐎规洝顫夌换婵嗩潩椤掍浇绶㈡俊鐐€栭崝褔姊介崟顖氱厱闁硅揪闄勯崑锝夋煕閵夘垳宀涢柛瀣崌閹煎綊顢曢妶鍕枤闂傚倸鍊烽悞锔锯偓绗涘厾楦跨疀濞戞ê鐎繛瀵稿Т椤戝洤鐣垫笟鈧弻娑㈠箛閳轰礁唯闂佸搫顑呴柊锝夊蓟閻旇　鍋撻悽娈跨劸濞寸姵绮庨惀顏嗙磼閵忕姷浠搁梺鍝勫閸撴繂顕ラ崟顓濇勃闁瑰瓨甯楅崺娑樷攽閻橆偅濯伴柛鏇炵仛閻や線姊虹拠鈥崇仩闁绘鍔楃划璇测槈濡攱顫嶅┑鐐叉閺堫剛鈧俺妫勯埞鎴︽倷閼搁潧娑х紓鍌氱М閸嬫挸鈹戦埥鍡椾簻閻庢碍婢橀悾鐑藉Ψ閵婏絼姹楅梺鍦劋閹搁箖宕㈤柆宥嗏拺缂備焦鈼ら鍫濈柈闁圭瀛╅崣蹇曗偓骞垮劚濞茬娀宕戦幘鑸靛枂闁告洦鍓涢ˇ銊х磽娴ｇ瓔鍤欓柣妤€妫濋、姘舵晲婢跺﹦顔掑銈嗘⒒閳峰牆顭囨繝鍥ㄢ拺缂備焦锕懓鎸庣箾娴ｅ啿鍠氶崵鏇㈡煏婵炲灝鍔楅柡鈧禒瀣厽婵妫楅弸娑㈡煟韫囨稐鎲鹃柡灞剧洴閸╋繝宕掑鍐ｆ嫲闁诲氦顫夊ú妯兼崲閸繍鍤曢柟缁㈠枟閸嬫劙鏌涜箛鎾存喐闁荤喐绻勭槐鎾诲磼濮橆兘鍋撻悜鑺ュ€块柨鏇氱缂嶆ê鈹戦悙鑼憼缂侇喖绉堕崚鎺楀箻鐠囪尪鎽曢梺鐐藉劜閸撴岸宕ョ€ｎ喗鐓曢柟鎵虫櫅婵″吋銇勯鈧澶婎潖閾忓湱纾兼俊顖滅帛閸庡酣姊洪幖鐐插闁告濞婇崹楣冩晜閻愵剙纾梺闈涱煭缁犳垿寮搁崒鐐粹拺闁告稑锕ユ径鍕煕閹惧娲撮挊婵嬫煏婵炑冩噽閿涙繃绻涙潏鍓хК婵炲拑绲块弫顔尖槈濮樿京锛滈柣鐘叉穿鐏忔瑦鏅堕敂绛嬫闁绘劖褰冮弳鐐电磼閸屾稑绗ч柍褜鍓ㄧ紞鍡涘磻閹烘嚦娑㈠礃閵娿垺鏂€闂佺粯鍔栧娆撴倶閿斿浜滈柕濞垮劤婢э附顨ラ悙鏉戠伌鐎殿喗鎸抽幃銏ゆ惞閸︻厽顫岀紓鍌欑婢ц姤鏅跺Δ鍕噷婵＄偑鍊ら崑鍕晝椤忓牆钃熸繛鎴炲焹閸嬫捇鏁愭惔婵堢泿濡炪倕娴氶崢浠嬪Φ閸曨垰顫呴柍銉ュ暱閹介潧顪冮妶鍐ㄧ仾闁挎洏鍨介獮鍐ㄢ枎韫囷絽鎮戦棅顐㈡处濞叉粏鈪靛┑掳鍊楁慨鐑藉磻濞戞◤娲敇閻戝棗娈ㄩ梺鍓插亝濞叉牠宕归崒娑栦簻闁哄啫娲ゆ禍褰掓煏閸偄娅嶆慨濠傤煼瀹曟帒鈻庨幒鎴濆腐缂傚倷绶￠崳顕€宕规导杈剧稏闊洦姊荤弧鈧┑顔斤公缁叉椽宕戦幘缁樻櫇闁稿本宀搁崬璺衡攽閻樿尙浠涢柛鏃€娲栭埢鎾诲Ω閳哄倵鎷虹紓浣割槸濡鈧艾鍢查埢鎾诲即閵忥紕鍘撻梻浣哥仢椤戝懘鎮橀埡鍛厵闁告瑥顦伴崐鎰版煙椤斻劌娲ら柋鍥ㄧ節闂堟稓澧㈤柟铏墵濮婄粯鎷呴悷閭﹀殝缂備礁顑嗛幑鍥х暦娴兼潙绠婚柤鍛婎問濞肩喖姊洪崷顓炲妺妞ゃ劌鎳橀幃鐐哄垂椤愮姳绨婚梺鐟版惈缁夊爼宕濆鍛斀妞ゆ牗绋掔亸锕傛煛鐏炵喎妫涢悿鈧┑鐐村灦鐢帗绂嶉悙顒傜闁瑰鍋為惃鎴︽煙閹绘帞绉烘慨濠傤煼瀹曟帒鈻庨幒鎴濆腐闂備胶顢婂▍鏇㈠箲閸ヮ剙绠栨繛宸簻鎯熼悷婊冮叄椤㈡洘绂掔€ｎ偆鍘介梺闈涚墕妤犳悂鎮樺☉妯忕懓顭ㄩ崘顏喰ㄩ梺鍝勭灱閸犳牠骞冨鍏剧喓鎷犻弻銉р偓鐑樼節閻㈤潧浠滈柟鍐茬箳閸掓帡骞橀幇浣圭稁濠电偛妯婃禍婊呯不瑜版帗鐓熼柟閭﹀幖缁茶鈹戦鑲╃劯婵﹥妞藉畷銊︾節閸愵亜寮崇紓鍌欑椤︻垳绱炴繝鍥╁祦闁告劦鍠楅崑銊╂煕閹惧啿绾ч柡鍛矒濮婅櫣鍖栭弴鐐测拤闂佽崵鍟欓崶銊︽珫濠电娀娼уú銊х不妤ｅ啫绾ч柛顐ｇ箓閳锋棃鏌涢幒宥呭祮闁哄瞼鍠撻崰濠囧础閻愭壆鐩庨梻浣告惈濡矂寮查悩宸綎缂備焦蓱婵潙銆掑鐓庣仯闁告柨鎽滅槐鎾存媴閾忕懓绗￠梺鎼炲妼濞尖€愁嚕椤愶箑绀冩い鏃傗拡濞煎﹪姊虹紒妯兼喛闁稿鎹囬弻娑㈠Χ閸℃顦ラ梺瀹狀潐閸ㄥ潡骞冮埡浣烘殾闁搞儴鍩栧▓褰掓⒒娴ｅ憡鎯堥柟鍐茬箻楠炲啴宕掗悙鑼舵憰闂佹寧绻傞幉姗€鎮㈤悡搴＄€銈嗘⒒閸嬫捇顢氬Δ浣风箚闁绘劦浜滈埀顑懐纾芥慨妯挎硾绾偓闂佸憡鍔曞Ο濠傤焽閺嵮€鏀介柣妯虹枃婢规鐥幆褍鎮戦柟渚垮妼铻ｉ柛婵嗗閸╃偛顪冮妶鍐ㄢ偓鎰板磻閹惧墎纾介柛灞捐壘閳ь剟顥撶划鍫熺瑹閳ь剟銆侀弽顓炲耿婵炴垶顭囬鍥⒑閻撳寒娼熼柛濠冾殜閹本寰勯幇顓犲幍闂佺粯鍨堕敃鈺佲枔閵忋倖鐓涘ù锝呮啞閸犳﹢鏌″畝瀣М妤犵偛娲俊鎼佹晜閻ｅ苯绲鹃梻鍌欑閹碱偊宕愰悽绋跨；闁瑰墽绮埛鎺楁煕鐏炲墽鎳嗛柛蹇撶灱缁辨帡顢氶崨顓犱桓闂佽桨绀侀崯鎾春閿熺姴宸濇い鎾跺缁遍亶姊绘担鍛婂暈婵炶绠撳畷婊冣槈椤兘鍋撻崨鏉戣摕闁靛濡囬崢闈涱渻閵堝棛澧俊顐ｎ殜瀵敻顢楅崒妤€浜鹃梻鍫熺〒閻掑憡鎱ㄦ繝鍐┿仢婵☆偄鍟埥澶婎潩椤掑姣囧┑鐘殿暯濡插懘宕戦崨瀛樺仭鐟滃海绮嬪澶婄濞撴艾娲﹂弲婊堟⒑閸撴彃浜為柛鐘虫崌瀹曘垽鏌嗗鍡忔嫼闂傚倸鐗婃笟妤呮偂椤撱垺鈷掗柛鏇ㄥ亜椤忣厼鈹戦敓鐘虫锭闁宠鍨垮畷鐓庮潩椤戝灝顥氶梺鑽ゅ枑閻熴儳鈧凹鍘剧划鍫ュ礃閳哄啰顔曢梺鍛婄懃椤﹁鲸鏅堕幎鑺ョ厽闊洦鎸搁弳锝嗘叏婵犲啯銇濈€规洏鍔嶇换婵囨媴閾忓湱鐣抽梻鍌欑閹芥粍鎱ㄩ棃娑氭殕缂佸娉曢弳锕傛煏韫囧鈧倝寮崒鐐寸厱婵炴垵宕悘锝夋煟閿曗偓閻楁挸顫忓ú顏咁棃婵炴垯鍨诲畷顏嗙磽娴ｈ棄绱︾紒顔界懇楠炲啴鏁撻悩鑼杸闂佺粯顨呴悧蹇涘矗閸℃せ鏀芥い鏃傜摂閻掗箖鏌涢埡鍌滃⒌闁诡噯绻濋幃銏ゆ嚃閳轰礁绲奸梻浣规偠閸庮垶宕曢柆宥呯闁靛牆顦伴悡娆愩亜閺傝法浠㈠ù婊冩贡缁辨帗娼忛妸銉х懖濠电偟鍘х换妯讳繆閹间礁唯闁挎洑鑳跺銊╂⒒閸屾瑨鍏岀痪顓炵埣瀵剚绗熼埀顒€鐣烽幋锕€绠婚柟棰佺劍鐎靛矂姊洪棃娑氬婵☆偅顨堢划顓㈠磼濠婂懐锛滈梺閫炲苯澧柟宄版嚇閹粌顫滈崱妞剧紦闂傚倷鑳剁划顖炲礉濡ゅ懎纾块柕鍫濐槹閸嬪倹绻涢崱妯诲鞍闁抽攱鍨堕妵鍕箻閸楃偛顬嬮梺浼欑秬妞存悂銆冮妷鈺傚€烽弶鍫熷礃閳ь剙娼￠弻锛勪沪閸撗€妲堝銈庡亝缁捇宕洪埀顒併亜閹哄秷鍏屾い鈺傜叀閹娼幏宀婂妳濠电偛鍚嬮悧妤冩崲濞戞﹩鍟呮い鏃囧吹閸戝綊姊虹粙娆惧劀缂佺粯绻堝濠氭晲閸涘倹妫冮崺鈧い鎺戝閸嬪鈹戦悩鍙夋悙缁炬儳娼￠弻鐔兼倷椤掆偓婢ь垱绻涢幘鎰佺吋闁哄本娲熷畷鐓庘攽閸ヨ埖顥ｉ梻浣筋嚙缁诲牓宕曢悽绋胯摕闁绘柨鍚嬮幆鐐淬亜閹伴潧澧查柍褜鍓濆▔鏇㈠焵椤掑喚娼愭繛鍙壝—鍐╃鐎ｎ亝妲梺閫炲苯澧柕鍥у楠炴帒顓兼径瀣碘偓鏍ㄧ箾鐎涙鐭嬬紒顔芥崌瀵鎮㈤悡搴濈炊闂佸憡娲﹂崢婊堝Χ婢跺鍘遍梺鍝勫€藉▔鏇″€寸紓鍌欐祰妞村摜鏁敓鐘叉槬闁逞屽墯閵囧嫰骞掗幋婵冨亾閻㈢纾婚柟鍓х帛閺呮煡骞栫划鐟板⒉闁诲繐绉瑰铏圭磼濡闉嶉梺鑽ゅ櫐缁犳挻淇婇悽绋跨妞ゆ牗鑹惧畵鍡椻攽鎺抽崐鎰板磻閹捐秮鐟邦煥閸愩劉鎸冪紓浣介哺閹稿骞忛崨鏉戜紶闁告洘鍩婄紞渚€寮婚敐鍛傛棃宕橀妸銏＄€伴梻浣虹帛娓氭宕抽敐鍛殾婵せ鍋撳┑鈩冩倐婵偓闁挎稑瀚ч崑鎾寸節濮橆厸鎷洪梺鑽ゅ枑婢瑰棙绂掗姀銈嗙厱闁绘洑绀侀悘锔锯偓娈垮枟閻撯剝鎱ㄩ埀顒勬煏閸繃顥戦柟鐤缁辨挻鎷呴崜鎻掑壉闁诲海鐟抽崘鑳偓鍧楁煟濡も偓閻楀嫭绂嶅鍫熺厪濠电偛鐏濋崝婊勩亜閵夛絽鐏查柡宀€鍠栭、娆撳棘閵堝牃鍋撶捄銊㈠亾鐟欏嫭绀冮柨鏇樺灲閵嗕礁鈻庨幘鏉戜患闁诲繒鍋犲Λ鍕不濞差亝鈷掑ù锝勮閺€鏉款熆閻熸壆澧︽鐐存崌椤㈡棃宕卞鍡樼稐闂備礁鎼ú銏ゅ垂濞差亜纾婚柍鍝勫€舵禍婊堟煙閹冭埞闁诲浚浜弻锝夊箻鐎涙顦板Δ鐘靛仦閻楁洝褰佸銈嗗坊閸嬫挸鈹戦埄鍐┿仢闁哄瞼鍠栭幊鐘活敆閳ь剚绂嶉悙顑句簻闁靛绲介崝锕傛煙椤旂晫鎳呴柟椋庡Ь椤︽娊鏌℃笟鈧粻鏍ь潖閾忚瀚氶柍銉ョ－娴狀厼鈹戦埥鍡椾簻闁哥喐娼欓悾?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閹冣挃缂侇噮鍨抽幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帡宕濋幘顔解拺閺夌偞澹嗛ˇ锔姐亜閹存繃顥㈠┑锛勬暬瀹曠喖顢涘槌栧晪闂佽崵濮惧▍锝夊磿閵堝鍊靛Δ锝呭暞閳锋垿鏌涘☉姗堝姛闁瑰啿鍟扮槐鎺旂磼濮楀牐鈧法鈧鍠栭…鐑藉春閸曨垰绀冮柕濞у懐宓侀梻浣筋嚙閸戠晫绱為崱妯碱洸闁绘劕鎼粈澶岀磼鐎ｎ亞姘ㄩ柡鈧禒瀣厽闁归偊鍨伴惃鐑樻叏閿濆洤浠遍柡宀嬬節瀹曘劑顢涘锝嗙€伴柣搴㈩問閸犳牠鈥﹂柨瀣╃箚闁归棿绀侀悡娑樏归敐鍫燁仩婵炵厧顦靛缁樻媴娓氼垳鍔搁梺鍝勭墱閸撶喖骞嗗畝鍕垫晩闁伙絽鐬艰ぐ楣冩⒑閸濆嫭鍌ㄩ柛銊︽そ瀹曟劙鎮介崨濠備画濠电偛妫楃换鎰邦敂椤忓棛纾奸柍褜鍓熼崺鈧い鎺戝閸婂灚绻涢崼婵堜虎闁哄绋戦…鑳槻闂佸府缍侀悰顕€骞嬮敃鈧～鍛存煟濮楀棗浜濇い顐㈢Ч濮婃椽妫冨☉姘鳖唺婵犳鍠氶崗姗€銆佸鈧畷顐﹀礋閵婏附鏉搁梻浣虹帛閸旀洖顕ｉ崼鏇€澶婎煥閸涱垳锛滅紓鍌欑劍閿氱紒妤佹皑缁辨帡宕掑☉妯昏癁闂佺娅曢悧鐘诲春閿熺姴绀冩い蹇撴捣闂傤垱绻濋悽闈涗哗闁规椿浜炲濠冪鐎ｎ亞顔愬銈嗗姧缁叉寧鏅堕埄鍐瘈闁汇垽娼у暩闂佽桨鐒﹂幃鍌氱暦閹存績妲堥柕蹇婂墲濞呭洤顪冮妶鍛閻庢氨鍏樺畷鐢稿即閻愨晜鏂€闂佺粯锚閻ゅ洦绔熷鈧弻娑樜旀担绯曟灆闂佸搫鐭夌紞浣割嚕椤曗偓瀹曞ジ顢曢敐搴㈩棝缂傚倸鍊烽悞锕傚礉濡ゅ懎纾婚柕鍫濇媼濞兼牗绻涘顔荤盎缂佲偓閸儲鐓欑紓浣姑粭鍌滅磼閵娿劌浜圭紒杈ㄦ尰閹峰懐绮电€ｎ亝顔勯梻浣侯焾閿曘倗绱炴繝鍥╁祦闁割偁鍎辨儫闂佹寧妫佸Λ鍕礉閸涘瓨鈷戦梻鍫熶緱閻擃參鏌ら棃娑氱Ш妤犵偛绻橀幖褰掑捶椤撶媴绱抽梻浣侯焾閺堫剟鎮疯瀹曟繂顓兼径瀣彅闂備緡鍓欑粔鐢稿磹閸洜鍙撻柛銉╊棑閸掓壆鈧鍣崳锝夊蓟閿涘嫪娌柛鎾椻偓婵洨绱撴担铏瑰笡缂佽鐗嗛悾鐑藉箳閹存梹顫嶅┑鈽嗗灥椤螣婵犲洦鈷戦悹鍥ㄥ絻椤掋垺銇勯弮鈧悧鐘茬暦娴兼潙鍐€妞ゆ挻绋堥崑鎾绘晝閸屾稑鈧鏌﹀Ο渚Ч闁诲寒鍓熷娲偡閺夊簱鎸冪紓渚囧櫘閸ㄨ泛鐣烽幋锕€绠涢柣妤€鐗忛崢鍓х磼閻愵剚绶茬€规洦鍓氶弲鍫曞礈瑜忕壕濂稿级閸稑濡块柛娆屽亾婵犳鍠栭敃锔惧垝椤栨粎绠旈柣鏃傚帶閻掑灚銇勯幒鍡椾壕濡炪値鍘煎ú鈺吽囪ぐ鎺撶厵妞ゆ柣鍔屽ú銈夌嵁閵忥紕绠鹃柟瀵稿剱閻掍粙鏌ｉ妶鍛枠闁哄瞼鍠栭幊婵嬫偋閸繃閿紓鍌欑瀵墎鈧瑳鍛煓濠㈣泛顭崥瀣熆鐠轰警鍎戦柛姗€浜跺娲箰鎼淬埄姊垮銈嗘肠閸モ晛绁﹀┑鈽嗗灥閸嬫劕螞?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忣澀鐒婂ù锝呮贡閳ь剙顭峰濠氬礋椤愶絽鏋犲銈冨灪娣囨椽藝閺屻儲鍋傞柕鍫濇娴滄粓鏌熼崹顔兼殲濠电偛娲弻宥堫檨闁稿繑绋撶划鍫熺瑹閳ь剟鏁愰悙宸叆闁割偅绻勯崝锕€顪冮妶鍡楃瑐闁绘帪绠撳畷锝堢疀濞戞瑧鍘撻悷婊勭矒瀹曟粓鎮㈤悡搴ゆ憰闂佹寧绻傞幏瀣焽閵娾晜鐓冪憸婊堝礈濞嗘挴鈧箓宕堕宥喰梻?
    // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婎潊闁绘ê妯婂Λ宀勬⒑鏉炴壆顦﹂柨鏇ㄤ邯瀵鍨鹃幇浣告倯闁硅偐琛ラ埀顒€纾鎰版⒒閸屾艾鈧悂宕戦崱娑樺瀭闂侇剙绉存闂佸憡娲﹂崹浼村礃閳ь剟姊洪棃娴ゆ盯宕ㄩ姘瑢缂傚倸鍊搁崐宄懊归崶鈺冪濞村吋娼欑壕瑙勭節闂堟侗鍎忛柦鍐枛閺屻劌鈹戦崱鈺傂ч梺鍝勬噺閻擄繝寮诲☉妯锋闁告鍋為悘宥夋⒑閸︻厼鍘村ù婊冪埣楠炲啫螖閸愨晛鏋傞梺鍛婃处閸撴盯藝閵娾晜鈷戠紓浣股戦幆鍫㈢磼缂佹绠撴い鏇稻缁绘繂顫濋鐐扮盎闂備礁鎲＄粙鎴︽晝閿斿墽鐭撻柛顐ｆ礃閳锋垿鎮归崶锝傚亾瀹曞洠鍋撴繝姘厱婵炴垵宕弳鏇㈡煙閻戞﹩娈曢柍閿嬪笒闇夐柨婵嗙墕琚氶梺闈涙閸熸壆妲愰幒妤€鐒垫い鎺嶈兌閻熷綊鏌嶈閸撴瑩鎮鹃悜绛嬫晬闁绘垵妫欓弲婵嬫⒑閹稿孩纾甸柛瀣崌閺岋綁骞樻潏鎹愨偓鍧楁煛瀹€鈧崰鎰焽韫囨稑绀堢憸蹇涘汲閻樼數纾奸柣鎰靛墮閸斻倝鏌涘顒夊剳闁瑰箍鍨归埞鎴﹀炊閳哄啰妲囬梻浣侯焾閺堫剛绮欓幒鎴犳殾闁规儼濮ら埛鎴︽煕濞戞﹫鏀婚柍閿嬪姍閺岀喖顢欓悡搴樺亾閸噮鍤曞┑鐘崇閺呮彃顭跨捄渚剰濞存粍绮撳娲川婵犲嫮绱伴梺绋垮閻撯€崇暦閻㈢纾奸柣鎰ˉ閹峰姊虹粙鎸庢拱闁煎綊绠栭、鏃堝煛閸涱喖鈧灚鎱ㄥΟ鍝勮埞妞わ絿鍘ц彁闁搞儜宥堝惈闂佽鍠氶弲顐ゅ垝濞嗘垶宕夋い顓熷灦鏁堟繝纰夌磿閸嬫垿宕愰弽顓熷亱婵°倕鎳忛崵灞轿旈敐鍛殭闁肩缍婇弻鏇熺箾閻愵剚鐝曢梺鎶芥敱閸ㄥ灝顫忔繝姘唶闁绘柨澧庣换渚€姊虹紒妯烘诞缂佺姵鐗犲璇测槈閵忕姵顥濋柣鐘充航閸斿酣宕濋鐐寸參婵☆垳鍘ч弸鎴犵磼缂佹绠炴俊顐㈠暙閳藉宕￠悙鎻掝棊闂傚倷鑳堕…鍫ヮ敄閸ャ劍鍙忛柣銏㈩焾缁犵偤鏌曟繛鍨壔闁绘梻鍘ф导鐘绘煕閺囥劌鏋涢柣鎾达耿濮婄粯鎷呴崨濠冨創濡炪倖鍨靛Λ娑氬垝濞嗘挸绠虫俊銈傚亾缂佺姷鍋ら弻鏇熺節韫囨搩娲紓浣叉閸嬫捇姊绘担鍛婂暈婵炶绠撳畷銏ゆ嚃閳哄啰鐣堕梺鎸庣☉鐎氼喚澹曢挊澹濆綊鏁愰崼顐㈡異濠电偛鐗婇敃銏ゅ蓟濞戙垺鍋愮€规洖娲ら埅褰掓⒑娴兼瑧鎮奸柛蹇旓耿閻涱噣宕卞☉娆戝姺闂佹寧鏌ㄨぐ銊╊敂閸啿鎷绘繛杈剧悼閻℃棃宕靛▎鎴犵＜缂備焦锚婵秹鏌熼銊ユ搐缁犲鏌熸０浣哄妽缂傚秴楠搁埞鎴︽倷閸欏鏋欐繛瀛樼矋缁诲牓骞冮悙瀵哥瘈婵﹩鍘鹃崢浠嬫⒑瑜版帒浜伴柛銊ゅ嵆閹锋洘绺介崨濠傗偓鍨叏濮楀棗骞楃紒璺哄级閵囧嫰顢橀垾鍐插Х濡炪倧绠撴禍鍫曞蓟濞戞鏃€鎷呯化鏇熺亞闁诲孩顔栭崰娑㈩敋瑜旈崺銉﹀緞閹邦剦娼婇梺鐐藉劚閸樻牜鑺辨繝姘厽閹艰揪绲鹃崵鈧柣搴㈢煯閸楀啿鐣烽幋鐐电瘈闁搞儜鍜佹Х濠电姰鍨奸崺鏍礉閺嶎厼纾归柣銏犳啞閻撴洘銇勯鐔风仴濞存粍绻勭槐鎺楁嚋閻㈤潧寮ㄩ梺璇″枟椤ㄥ懘锝炲┑瀣亗閹兼番鍨绘禍鍓佺磽閸屾瑧顦﹂柛濠傛贡閺侇噣鏁撻悩鍙夌€梺鍛婂姦閸犳牜澹曢崗鍏煎弿婵☆垰銇橀崥顐︽煛閸℃洖宓嗛柡宀嬬秮閹垽宕妷褏鍘愮紓鍌欒兌缁垶鏁冮姀鐙€鍤曟い鎰╁焺閸氬鏌涢鐘茬伄闁哄顭堥埞鎴︽倷閺夋垹浠搁梺鑽ゅ枂閸ㄤ粙鐛箛鏇犵＜婵☆垵鍋愰鏇㈡煛婢跺﹦澧曞褌绮欓獮鍐╃附閸涘﹦鍘遍柟鐓庣摠缁嬫垿鍩€椤掍胶绠撻柣锝囧厴閹晫绮欑捄銊ュЕ婵＄偑鍊栫敮鎺斺偓姘煎弮閸╂盯骞嬮敂钘夆偓鐢告煟瑜庨幃鍌氼瀶椤旂晫绡€闁靛繆鍩勯幋鐐碘攳濠电姴娲ら柨銈嗕繆閵堝嫯鍏岄柛姗€浜跺娲捶椤撶偛濡洪梺鍦焾閹芥粎鍒掗鐑嗘僵闁煎摜鏁搁崢闈涱渻閵堝棛澧紒瀣浮钘熼柛顐犲劜閻撳啴鏌曟径妯虹仯闁伙絽鐏氶〃銉╂倷瀹割喖鍓堕梺璇″枟閻熝囧焵椤掍胶鈯曟い顓炴穿閵囨劙宕卞☉娆屾嫽闂佺鏈悷褔藝閿曞倹鐓欑痪鏉垮船娴滀即鎸婂┑鍥ヤ簻闁哄稁鍋勬禒锕傛煕鐎ｎ亶鍎旈柡灞剧洴瀵挳濡搁妷褌鍝楃紓鍌欒閸嬫挸顭块懜闈涘缂佺姴缍婂鍫曞醇濠靛棌鎸冮梺璇″枤閸樠囧煡?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忋垺鍋橀柍銉ュ帠婢规洟姊哄Ч鍥х仾妞ゆ梹鐗犻幃鐐哄箚椤€崇秺閹晛鈻庤箛鏂款槱闂佹娊鏀辩敮锟犲蓟濞戙垹鍗抽柕濞垮劙缁ㄨ顪冮妶鍛闁告瑥鍟村濠氬灳瀹曞洦娈曢柣搴秵閸撴盯鎯侀崼銉﹀€甸悷娆忓缁€鍐煥閺囨ê鐏ǎ鍥э躬楠炴牗鎷呴懖婵勫姂閺屾洝绠涢弴鐐愨€愁熆鐟欏嫭绀嬫慨濠冩そ瀹曘劍绻濋崟顓犵獥闂備胶顭堢€涒晠鎮￠敓鐘偓?*闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱鈧懓瀚崳纾嬨亹閹烘垹鍊炲銈嗗笒椤︿即寮查鍫熷仭婵犲﹤鍟版晥濠电姭鍋撳〒姘ｅ亾婵﹨娅ｇ槐鎺懳熼搹閫涚礃婵犵妲呴崑鍕偓姘煎枟缁旂喖寮撮悢铏圭槇濠殿喗锕╅崢楣冨矗閸℃せ鏀介柣妯肩帛濞懷勪繆椤愵偄骞栭崡閬嶆煕濠靛嫬鍔ゆ俊顐灦閺岀喖顢涢崱妤佸櫧濡ゆ棃姊绘担鍛婃儓濠㈣泛娲崺鈧い鎺嶇婢ь垶鏌涘顒夊剶妤犵偞鐗滈崚鎺楀礂婢跺﹣澹曢悗瑙勬礀濞村倿宕崶鈺冪＝闁稿本鐟ㄩ崗宀勬煕鐎ｎ偅宕岀€规洘娲熷畷妯侯啅椤旀儳濮洪梻浣芥硶閸犳挻鎱ㄩ幘顔藉珔闁绘柨鍚嬮悡蹇涙煕椤愶絿绠栭柛瀣典邯閺屾盯鏁傜拠鎻掔闂佺顑嗛幑鍥极閹邦厽鍎熼柍銉ㄥ皺閸戝綊姊绘担铏瑰笡缂佽鍟叅闁绘梻鍘х粻?12 缂?*闂?
    // first-run-setup 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存い銏℃礋閺佹劙宕卞▎妯恍氶梻鍌欒兌椤牏鎮锕€绀夐幖娣妼缁犵喖鏌熼梻瀵割槮缂侇偄绉归弻娑㈩敃閿濆棗鍓烽梺闈涱槴閺呮粓鎮￠弴銏＄厵闁煎壊鍓欐俊鑺ョ箾閸涱厽顥㈤柡灞剧⊕閹棃鏁愰崨顔句邯闂備礁鎼張顒€煤濠靛牏涓嶆繛鎴炲焹閸嬫捇鏁愭惔婵堟晼闂佸憡姊瑰娆撳煘閹达附鍊烽悹楦挎濮ｃ垹顪冮妶搴″箹闁绘鎹囧畷娲倷閸濆嫮顓洪梺鎸庢濡嫭绂嶈ぐ鎺撶厽闁绘ê寮堕崢鍌炴煕閹板苯鎳愰悡鎴︽⒒閸屾艾鈧兘鎳楅崼鏇炵疇闁规崘顕ч悿顕€鎮归幁鎺戝鐎规洘鐓￠弻鐔煎箥椤旂⒈鏆梺绋款儌閺呯娀寮婚敐澶婄闁挎繂妫Λ鍕磽娴ｅ搫啸闁轰礁顭峰濠氭偄閻戞ê鏋傞梺鍛婃礀閻忔岸鎮鹃崜浣虹＝闁稿本姘ㄥ瓭闂佹寧娲︽禍婊堫敋閿濆鏁冮柨鏇楀亾缂佺姵鐩弻鈩冨緞婵犲嫪铏庣紓浣瑰姈缁嬫垿鈥旈崘顔嘉ч柛鈩冿供濮婂灝鈹戦悙宸Ч婵炶尙鍠栨俊瀛樼瑹閳ь剙鐣烽妸褉鍋撳☉娅辨岸骞忕紒妯肩閺夊牆澧介崚浼存煙閼恒儳鐭掗柡浣哥Т椤撳ジ宕堕埡鍐跨床婵＄偑鍊栧ú鏍箠鎼淬劍鍊堕柣妯肩帛閸嬪倿鏌曢崼婵愭Ч闁绘挾鍠栭弻锟犲礃閵娿儮鍋撻崫銉︽殰闁割偅娲橀悡銏ゆ煕閹板吀绨婚柡瀣洴閺岋紕浠︾拠鎻掝瀳闂佸疇顫夐崹鍨暦闁秴鐓涢柛灞藉亰缁犳垿鍩為幋锕€鐓￠柛鈩冦仦缁ㄥジ姊洪幐搴㈢８闁搞劏妫勯锝囨嫚濞村顫嶉梺鍐茬亪閺備線宕戦幘缁樻櫇闁稿本姘ㄩ鍝勨攽閻樼粯娑ф繛灞傚姀閵囨劙顢涢悙绮规嫽婵炶揪绲介幉锟犲箚閸喍绻嗘い鎰╁灲閸欏嫮鈧娲栫紞濠囧箠閿熺姴围闁糕檧鏅滅紞妤呮⒒娴ｇ瓔娼愰柛搴㈠▕閹椽濡搁敃鈧崹鏃堟煙缂併垹鏋熼柣鎾跺枛閺岋綁寮幐搴℃殘缂備浇娉涘鍫曞Φ閸曨垼鏁冮柕蹇婃嚕閵徛颁簻妞ゅ繐瀚弳锝呪攽閳ュ磭鍩ｇ€规洘锕㈡俊鎼佸Ψ椤旈敮鍋撻幎鑺モ拻濞达絿鎳撻婊勭箾鐠囇囨缂佸倸绉撮…銊╁醇椤愶絾娅旈梻浣瑰缁诲倸螞濞戞艾濮柍褜鍓欓埞鎴︻敊閺傘倓绶甸梺鍛娒崥瀣矉瀹ュ拋鐓ラ柛鏇ㄤ簽缁犳岸姊虹紒妯哄婵☆垰锕畷鏇＄疀濞戞瑥鈧灚绻涢幋鐐茬瑲婵炲懎鎳橀弻宥囩磼濡纰嶉梺瀹狀潐閸ㄥ綊鍩€椤掑﹦绉甸柛瀣у亾闂佸綊顥撶划顖滄崲濞戙垹绠荤痪鎯ь儏椤ユ繂鈹戦悙鑼勾闁稿﹥娲熼、姘舵晲婢跺﹪鏁滃┑掳鍊撶拋鏌ュ箯婵犳碍鈷戦柛娑橈攻婢跺嫰鏌涚€ｎ亜顏い銏℃椤㈡棃宕奸悢鍝勫箺闂備胶鍋ㄩ崕杈╁椤撶姷涓嶇紓浣股戦崣蹇撯攽閻樻彃浜為柣鎾瑰亹閳ь剝顫夊ú妯兼暜閹烘鐓濋幖娣妼缁狅絾銇勯幘璺烘櫩婵犲﹤瀚弧鈧梺姹囧灲濞佳嗏叴闂備胶顭堥鍡涘箲閸ヮ剙钃熸繛鎴烆焸閺冨牆绀冮柕濞垮€栭惈蹇涙⒒娴ｅ憡鎯堟い鎴濇噽缁棁銇愰幒鎴ｆ憰闂佸搫娴勭槐鏇㈡偪閳ь剟鏌ｆ惔顖滅У濞存粎鍋炵€靛ジ宕熼鐘碉紳婵炶揪绲捐ぐ鍐╃閻愵剛绡€闁汇垽娼у瓭婵炲鍘ч幊鎾诲Φ閹版澘绠抽柟鎯х摠閻濇娊姊绘担鍛婂暈濞撴碍顨婂畷锝嗘償閳儲绋戦悾婵嬪礋椤戣姤瀚奸梻浣侯攰閸嬫劙宕戝☉銏犵劦妞ゆ帊鑳舵晶顏堟偂閵堝鐓熼柡鍌涱儥濞堢姵绻涢崗鑲╁ⅹ闁宠鍨块幃鈺冣偓鍦Т椤ユ繈姊绘担绋胯埞婵炲樊鍙冨濠氭晲婢跺﹥顥濋梺鍓茬厛閸ｎ噣寮抽姀銏㈢＝濞达絿鐡旈崬铏圭磼閼镐絻澹樻い顐㈢箰鐓ゆい蹇撳缁愭稒绻濋悽闈浶㈤柟鍐查叄钘熸繝闈涙－濞撳鏌曢崼婵囶棞闁诲繈鍎查妵鍕晝閸屾稒閿銈嗘穿缁插潡骞忛悩瑁佸湱鈧綆鍋掑鏃€绻濈喊妯活潑闁搞劌鐖煎銊╂焼瀹ュ繒绋忛悗骞垮劚椤︿即鍩涢幋锔界厱婵炴垶锕╅悡顒佺箾閸噦鑰块柡灞剧〒閳ь剨缍嗛崑鍡涘Υ閹烘鐓冪憸婊堝礈濮樿泛绀冮柤濮愬€曠欢銈吤归悩宸剰闁绘挻锕㈤弻鐔告綇妤ｅ啯顎嶉梺绋匡功閸忔﹢寮婚悢铏圭＜婵☆垵娅ｉ悷鏌ユ⒑缁嬫鐓柛銊ㄤ含閹广垹鈹戠€ｎ偄浠洪梻鍌氱墛缁嬫劗鍒掗崼鏇熲拺闁圭瀛╃壕鎼佹煕婵犲啰绠為柡浣瑰姍閹瑩宕滄担鐑樻緫闂備胶鎳撹ぐ鐐烘倿閿曗偓椤啯绂掔€ｃ劉鍋撴笟鈧顕€宕煎┑鍫Ч婵＄偑鍊栭幐鍡涘礃閳哄倹鐦戦梻鍌氬€搁崐鐑芥嚄閸洏鈧焦绻濋崘顏嗗骄闂佸壊鍋侀崕杈╃不閸偁浜滈煫鍥ㄦ尰椤ユ粎绱掗煬鎻掆偓婵嬪蓟濞戞ǚ鏀介柛鈩冾殘瀵悂姊洪幐搴ｇ畵妞わ富鍨堕弻瀣炊椤掍胶鍘繝銏ｅ煐缁嬫牠宕ラ崒鐐寸厵鐎瑰嫮澧楅幆鍫ユ煃鐟欏嫬鐏撮柟顔界懅閹瑰嫭銈﹂幐搴㈡瘣濠碉紕鍋戦崐鏍р枖閿曞倸绀嬫い鎺嶈兌瑜版寧淇婇悙顏勨偓鏍哄澶婄；闁瑰墽绮悡娑㈡倶閻愰潧浜剧紒鈧€ｎ兘鍋撶憴鍕闁靛牊鎮傞獮鍐閵忋垻鐓撻柟鐓庣摠閹稿憡绂嶆ィ鍐╃厵闂侇叏绠戝鐐箾閸粎鐭欓柡宀嬬秮楠炲洭顢橀悙鐢点偡闂備礁鎼Λ瀵稿緤閻愵剛鈹嶅┑鐘叉祩閺佸啴鏌曡箛鏇炐ｆ繛鍫弮濮婅櫣鈧湱濮甸ˉ澶嬨亜閿旂偓鏆柣娑卞櫍楠炴帒螖閳ь剛绮绘總鍛婄厱闁硅埇鍔嶅▍鍥╃磼婢舵劗鐣烘慨濠傤煼瀹曟帒顫濋钘変壕濡炲瀛╅鑺ユ叏濮楀棗澧婚柣鎺旀櫕閹叉瓕绠涘☉妯碱唹闂侀潧绻堥崐鏇犵不閹惰姤鐓欓柣鎰靛墯缂嶆垹鈧鍠栧鈥愁潖閾忓湱纾兼俊顖濇娴犲瓨绻濋姀锝庢綈闁荤啿鏅涢悾鐑藉醇閺囥劍鏅㈡繝銏ｆ硾椤戝棗鈻嶉崶顒佲拻濞达絿鎳撻婊勬叏婵犲偆鐓肩€殿喗鐓￠、鏇㈡晝閳ь剟宕掗妸銉冨綊鎮╁顔煎壉闂佹娊鏀遍崹鍦閹惧瓨濯撮柛蹇擃槹閿涘牆顪冮妶鍛閻庢氨鍏樺畷鐢稿即閵忥紕鍘鹃梺褰掓？閻掞箑鈽夎缁辨帡宕滄担闀愭闂佸搫鏈惄顖氼嚕閹绢喖惟闁靛鍎抽鎴︽⒒娴ｅ憡鎲稿┑顔炬暬瀹曟繂鈻庨幘瀹犳憰闂佹枼鏅涢崯顐ゆ閻愭祴鏀介柣妯诲絻椤徰囨煣韫囨捇鍙勬慨濠冩そ瀹曨偊宕熼濠勫窛缂傚倷鐒﹁ぐ鍐焽閿熺姴鏄ラ柍褜鍓氶妵鍕箳瀹ュ洩绐楅梺鍝ュ枎缁绘﹢寮诲☉銏″亹闁归鐒﹂悾浠嬫⒑闂堟稒澶勯柛鏃€鐟ラ悾宄邦潨閳ь剟銆佸▎鎾村殐闁宠桨绀佺粻鎶芥⒒娴ｇ儤鍤€闁告艾顑夐幃楣冾敂閸繄顦悗鍏夊亾闁逞屽墰閸掓帞绱掑Ο鑲╃槇濠殿喗锕╅崢楣冨储閸涘﹦绡€闁靛骏绲剧涵鐐亜閹存繃鍤囬柟顔斤耿閺佹劖寰勭€ｎ剙骞?up 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊箣閿濆棭妫勯梺鍝勵儎缁舵岸寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ゆい顓犲厴瀵鏁愭径濠勭杸濡炪倖甯婄拋鏌ュ几濞嗘挻鈷戠紓浣姑粭鈺佲攽椤斿搫鈧骞戦姀鐘闁靛繒濮撮懓鍨攽閳藉棗鐏ユい鏇嗗懎鏋堢€广儱顦伴悡鐔兼煟閺傛寧鎲搁柟铏礈缁辨帡鎮╅搹顐㈢３濡ょ姷鍋涢崯顐ョ亙闂佸憡渚楅崰妤€鈻嶅鍫熺厵闁兼祴鏅炶棢闂佸憡鎸荤换鍫ュ箖濡警鍚嬪璺侯儌閹锋椽姊虹涵鍛汗闁稿鐩崺鈧い鎺嶇劍缁€瀣煙瀹曞洤鏋涙い銏＄☉閳规垿宕煎鍛板厭闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噺瀹曟煡鏌涘畝鈧崑娑氱不閻樿绠规繛锝庡墮婵″吋鎱ㄩ悷鏉跨骇缂佺粯鐩畷鍗炍熼搹閫涙偅缂備胶鍋撻崕鎶藉Χ閹间礁钃熼柨婵嗘噳閺嬪酣鎮橀悙鏉戠亰濠㈣娲熷缁樻媴鐟欏嫬浠╅梺鍛婃煥閻倿骞冮妷鈺傚亗閹兼惌鍠栧▓銊︾節閻㈤潧校缁炬澘绉瑰畷鎴﹀礋椤栨稑鈧爼鐓崶褎鎹ｉ柛鏂跨Ч閺岋綀绠涙惔锝囩崲濠殿喖锕ュ钘夌暦濠婂牊鍤戞い鎺嗗亾閻㈩垬鍎靛娲传閵夈儛锝嗐亜閵娿儻韬鐐插暙椤粓鍩€椤掑嫬鏄ラ柍褜鍓氶妵鍕箳閹存繍浠鹃梺鎶芥敱鐢帡婀侀梺鎸庣箓鐎氼垶顢楅悢鍏肩厽闁圭儤鏌ㄦ禍楣冩煏閸パ冾伃妤犵偛顑呴埞鎴﹀炊閼哥绱楀┑掳鍊楁慨鐑藉磻閻愬搫鍨傞柛顭戝枤閺嗭箓鏌涢锝嗗闁轰礁妫濋弻宥堫檨闁告挻绋撻崚鎺撶節濮橆剙鍞ㄥ銈嗘尵閸犳捇宕㈡禒瀣拺缁绢厼鎳忚ぐ褏绱掗弻銉х暫閽樼喎顭跨捄铏圭伇缁炬儳銈搁弻锝呪枎閹邦喗鍣у┑鐐额嚋缁犳捇骞冮垾鏂ユ瀻闁瑰濮烽敍婊堟煟鎼搭垳绉靛ù婊呭仦缁傛帡濮€鎺虫禍婊堟煙閸濆嫷鍎忛柣蹇ｄ邯閺屾洟宕惰椤忣厾鈧鍣崜娆撳煝鎼淬劍鍊锋い鎴亾缁犳挸顫忛搹鍏夊亾閸︻厼顎屾繛鍏煎姍閺屾盯濡搁妷锕€浠村Δ鐘靛仜閸熸挳宕洪敓鐘插窛妞ゆ挾濮烽弶绋库攽閻愯埖褰х紒韫矙楠炲鍨鹃弬銉︾€哄┑鐐村灟閸ㄦ椽鍩涢幒鎳ㄥ綊鏁愰崨顔兼殘闂佺灏欓…鍫ュ煘閹达富鏁婇柣鎰靛墯濮ｅ嫰姊洪崫銉バ㈤柨鏇樺€濋、妯荤附缁嬭法鍊為梺闈涱焾閸庤京鑺遍妷锔剧瘈闁汇垽娼ф禒锕傛煕椤垵鐏︾€规洜顢婇妵鎰板箳閹炬枼鍋撻崸妤佺厱妞ゎ厽鍨垫禍鏍瑰鍕煉闁哄瞼鍠栭幃婊兾熼搹閫涙樊婵＄偑鍊曞ù姘椤忓牆钃熼柨婵嗩槸缁犲鎮楅悽鐢点€婃俊鎻掔墦濮婂宕惰濡偓闂佸搫琚崝宀勫煘閹达箑骞㈡俊顖滃劋椤忥繝姊绘担鐑樺殌鐎殿喖鐖奸獮鍐磼濮樼厧娈ㄥ銈嗗姧缁犳垿鏌嬮崶顒佺厪濠㈣埖绋撻悾閬嶆煃瑜滈崜娑㈠极婵犳艾绠栫€瑰嫭澹嬮弸搴ㄧ叓閸ャ劍鎯勫ù鐘插⒔缁辨挻鎷呴幓鎺嶅闂備礁澹婇崑渚€宕曢弻銉﹀亗闁哄洨鍠嶇换鍡樸亜閺嶃劎鐭婄€涙繂鈹戦悙鑼勾闁稿﹥绻堥獮鍐灳閹颁焦寤洪梺閫炲苯澧寸€殿喖顭烽幃銏ゅ川婵犲嫮肖闂備礁鎲￠幐鍡涘川椤旂瓔鍟呴梻鍌氬€搁崐椋庣矆娴ｅ摜鏆︽い鎺嗗亾閻撱倖銇勯幘鍗炵仼闁藉啰鍠栭弻銊モ攽閸♀晜笑濠碘槅鍋呴敃銏ゅ蓟閻旇櫣鐭欓柛顭戝櫘閸斿姊洪崫銉バｉ柛鏃€鐟ラ～蹇曠磼濡顎撻梺鑺ッ敍宥夊箻缂佹ê浠哄銈嗙墬缁嬫垵霉椤曗偓閺屾盯鍩￠崘銊ゆ濡ょ姷鍋涢澶愬极閸岀偞瀵犲璺侯槹閻繘姊婚崒娆戭槮濠㈢懓锕幃锟犲醇閵夈儳锛涢梺鍛婁緱閸樻粓骞掑Δ鈧粈瀣亜閺嶃劎鈻撻柟鐤缁辨挻鎷呴崜鎻掑壉濡炪倖鍨堕悷鈺佺暦閺囥垹绠ｉ柨鏃傛櫕閸橀亶鏌ｆ惔顖滅У闁稿妫楃叅闁圭虎鍠楅悡鍐煏婢舵ê鐏ｉ柣锝囨暩閳ь剝顫夊ú姗€宕曟總鏉嗗洭顢氶埀顒勫蓟閿涘嫧鍋撻敐搴濋偗闁告瑥瀚伴弻鈥崇暆閳ь剟宕伴弽褏鏆︾憸鐗堝俯閺佸﹪鏌涜箛鎾虫倯妞ゆ柨顦靛缁樻媴閸涘﹥鍎撻梺纭呮珪閸旀瑥鐣烽幋锕€围闁告粈鐒︾紞搴♀攽閻愬弶鈻曞ù婊勭矊椤斿繐鈹戦崱蹇旀杸闂佺粯蓱瑜板啴顢旈妶鍛斀妞ゆ柨鍚嬮幖鎰版煃鐟欏嫬鐏撮柟顔界懇楠炴捇骞掗幘鏂ュ亾椤栫偞鈷戦柤濮愬€曢弳鈧梺鍛婁緱閸樺ジ鎮樻繝鍐瘈闁汇垽娼ф牎闂佺厧缍婄粻鏍х暦濮椻偓閸┾偓妞ゆ帒瀚埛鎴︽⒒閸喍绶遍柣鎺楃畺閺屾稒绻濋崘鐐暭缂備緡鍠涢褔锝炲┑瀣殝缁剧増蓱鐎氬ジ姊婚崒姘偓鎼佹偋婵犲嫮鐭欓柟鎯х摠濞呯娀鏌￠崶銉ョ仾闁绘挻鐟╅弻娑㈠箛椤撶姴寮ㄩ梺鍛婄懃鐎氼噣鍩€椤掍緡鍟忛柛鐘愁殕缁绘稒绻濋崶褏鐣洪梺闈涚箞閸婃洜绮堥崒鐐寸厾婵炴潙顑嗗▍鍛殽閻愯尙澧涚紒缁樼箓椤曘儵鏌ㄧ€ｎ偆鍑￠梺閫炲苯澧柟鐟版喘楠炲﹤螖閸涱參鍞堕梺鍝勬处鑿ら柛瀣崌瀹曟帡鎮欓懠鑸垫啺闂備線娼ч¨鈧┑鈥虫喘瀹曘垽顢旈崼鐔叉嫼闂佺鍋愰崑娑㈠礉閳ь剟姊洪崨濠傚鐟滄壆鍋熷Σ鎰板籍閸喓鍘介柟鍏肩暘閸╁嫰宕箛娑欑厱闁绘娅曠粈鍫ユ煕閹烘挸绗掗柍璇查叄楠炲洭顢旈崱娆戞毎濠电姵顔栭崰妤冩崲閸岀偞鍎楅柛顐ｆ儕閿濆绠氱憸婊冦€掓繝姘厪闁割偅绻冮ˉ鐐烘倶韫囨洘鏆柡灞剧☉铻ｉ柣妯哄级閹兼劙鏌ｉ幘杈捐€块柡宀€鍠愬蹇斻偅閸愨晩鈧秹姊虹粙鍖″姛闁稿繑锕㈤獮鍐喆閸曨剙顫￠梺鐟板槻閻牓宕濋崨顓涙斀闁宠棄妫楁禍婊堟煟濡も偓濡繈宕洪悙鍝勭闁挎棁妫勯埀顒勵棑閳ь剛鎳撴竟濠囧窗濡や焦娅忛梻鍌氬€烽懗鍫曞磿閻㈢鐤鹃柍鍝勬噹閸ㄥ倸霉閻撳海鎽犻柡鍛箞閺屾洝绠涙繝鍌氣拤婵犳鍨伴顓犳閹烘鏁嶆繛鎴炵懅椤︺劑姊洪崫鍕偓鐢稿磻閹剧粯鈷掗柛灞剧懆閸忓瞼绱掗鍛仸闁炽儻绠撳畷濂稿Ψ椤旇姤娅旀繝鐢靛Т閿曘倝宕板璺虹柈闁告稑鐡ㄩ悡蹇擃熆鐠虹儤顥炴繛鍛閺屾稒绻濋崒婊€绮甸梺閫涚┒閸斿矁鐏掗梺缁樻尭妤犲憡绂掑Ο鑽ょ瘈闁冲皝鍋撻柛鏇ㄥ亽濡棛绱撴担浠嬪摵閻㈩垽绻濋妴浣割潩鐠哄搫绐涘銈嗘尵閸嬬喖宕Δ浣风箚闁绘劦浜滈埀顑懐涓嶉柟瀵稿Т椤ユ碍銇勯幘璺盒ｆい?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱闁靛牆鎳庨顓㈡煛鐏炲墽娲存い銏℃礋閺佹劙宕卞▎娆戠┛闂傚倷绶氶埀顒傚仜閼活垱鏅堕崜褏纾奸柣妯挎珪鐏忣參鏌ｉ敐澶嬫暠缂佽櫣鏅划娆戞嫚娣囧崬濡介柕鍥у瀵噣宕堕‖顔芥尰缁绘盯宕ㄩ鐘测叺濠殿喖锕︾划顖炲箯閸涘瓨鎯炴鐐茬氨閸嬫挻绻濋崶銊у幈闂佽鍎抽顓犵不閻愮儤鐓欐い鏃€鍎抽崢鏉戔攽閿涘嫬鍘撮柛鈹惧墲閹峰懘宕妷銏犱壕妞ゆ挾鍋愰弨浠嬫煟濡偐甯涙繛鎳峰洦鐓熸俊銈傚亾闁哥喐娼欓悾鐑藉箣閿曗偓缁犲鏌ゆ慨鎰偓鏇犵矈閿曞倹鈷戦柛鎾瑰皺閸樻盯鏌涢悩宕囧闁逛究鍔嶉幆鏃堝煢閳ь剟寮ㄦ禒瀣厽闁逛即鍋婇弶娲煕閵堝拋鍎戠紒杈ㄥ浮閹晠鎼归銏㈩暡婵＄偑鍊栧ú鈺冪礊娴ｅ摜鏆︽俊顖欒閸熷懏銇勯弮鈧崕鍐差嚕閹惰姤鈷掑ù锝囶焾椤ュ繘鏌涚€ｎ亝顥㈢€规洑鍗抽獮妯兼嫚閹绘帒浜堕梻浣烘嚀婢х晫鍒掗鐐茬；闁斥晛鍟扮弧鈧繝鐢靛Т閸婃悂顢旈锔界厽妞ゆ挾鍠庣粭褔鏌嶈閸撴繈锝炴径濞掑搫螣閻撳骸鐏婇梺瑙勫礃椤曆呯不閺嶎厽鐓忛煫鍥ь儏閳ь剚鐗犲畷鎴﹀磼閻愯尙顔愬┑鐑囩秵閸撴瑩鍩€椤掍胶澧垫鐐差槹鐎佃偐鈧稒菤閹风粯绻涢幘鏉戠劰闁稿鎸荤换娑欐媴閸愬弶绀岄柍褜鍓ㄧ粻鎾崇暦婵傜唯闁靛／宥囧惞濠碉紕鍋戦崐鏍蓟閵娿儙锝夊醇閵夛附娅滄繝銏ｆ硾閿曨亪宕崼鏇熲拺缂備焦蓱閻撱儱顭胯濞撮鍒掔拠宸僵妞ゆ帒顦扮€靛矂姊洪棃娑氬婵☆偅绋掗弲鍫曨敆閸屾粎锛滈柣鐘叉处瑜板啴鍩€椤掆偓濠€杈╁垝鐎ｎ亶鍚嬪璺侯儏娴狀參姊洪悷鏉挎毐闁挎洏鍊楃槐鐐寸節閸パ呯暫闂佹枼鏅涢崯顖炲磿閻斿吋鐓忛煫鍥ㄦ礀鍟搁梺缁樼▓閺呮繄妲愰幘璇茬＜婵炲棙鍔楅妶鐗堜繆閻愯尙鎽犻柟顔煎€块悰顕€骞囬悧鍫濇闂侀潧鐗嗗ú銊╂偩閸洘鈷戠紓浣股戦埛鎺楁煕濡姴娲﹂崐宄扳攽閻樻彃顏柛鐘冲姍楠炴牕菐椤掆偓婵′粙鏌涙繝鍛【闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑鐘愁問閸犳牠鏁冮妷銉富闁芥ê顦遍弳锕傛煏婵犲繐顩紒鈾€鍋撻梻浣告啞閸斞呭緤閸撗€鍋撳顒夌吋婵﹦绮幏鍛喆閸曨偂鍝楅梻浣告啞閺屻劑鏌婇敐鍜佸殨妞ゆ劧绲跨弧鈧┑顔角滈崕顕€宕?5454 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘﹢寮婚敐澶婄闁挎繂妫Λ鍕⒑閸濆嫷鍎庣紒鑸靛哺瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鍓х＜闁诡垎鍐ｆ寖缂備緡鍣崹鎶藉箲閵忕姭妲堥柕蹇曞Х椤撴椽姊虹紒妯哄闁诲繑宀稿畷瀹狀槾缂佽鲸鎸婚幏鍛矙濞嗙偓娈洪梻浣侯焾椤戝啴宕愬┑鍡╁殨閻犲洦绁村Σ鍫ユ煏韫囨洖顫嶉柕濠忚礋娴滄粓鏌￠崘銊モ偓鍝ユ暜閼哥偣浜滄い鎰枑濞呭﹥鎱ㄦ繝鍛仩缂佽鲸甯掕灒闁惧繐婀遍幊鍡涙⒒娴ｇ瓔鍤欑紒缁樺灥閻ｆ繈宕ㄦ繝浣虹畾闂佹眹鍨婚…鍫㈢矆鐎ｎ偁浜滈柡宥冨妽閻ㄦ垶銇勯弬鍖¤含婵﹨娅ｉ崠鏍即閻愭祴鎷ら梻渚€娼уú銊╁磻閻愮數鍗氶柣鏃傚帶閸楁娊鏌曡箛銉х？闁告ü绮欓弻鐔煎礂閼测晜娈梺绯曟櫅鐎氭澘鐣烽婵堢杸婵炴垶鐟ч崢閬嶆⒑缂佹◤顏嗗椤撶喐娅犻柣銏犳啞閻撴瑦銇勯弬鎸庢儓闁告梹绮庣槐鎺楁惞鐟欏嫭鐝紓渚囧枟閻熝囧箲閸曨垰惟鐟滃骸袙鎼淬劍鈷掗柛灞剧懅閸斿秹鏌熼鑲╁煟鐎规洘娲熼、娑㈡倷閼碱剦妲烽梺鐟板悑閻ｎ亪宕濇繝鍥х劦妞ゆ巻鍋撻柟铏耿楠炲啫顭ㄩ崼鐔锋疅闂侀潧顦崕顕€宕戦幘缁樺仺闁告稑锕﹂崣鍡椻攽閻樼粯娑ф俊顐ｇ箞椤㈡挸螖閳ь剟婀佸┑鐘诧工閹冲孩绂掗柆宥嗙厸閻忕偛澧介埥澶愭懚閿濆鐓曟繝闈涙椤忣剟鏌涢埡鍐ㄤ槐婵﹨娅ｉ埀顒€婀辨刊顓烆焽閹扮増鐓曢柕濞垮劜閸嬨儵鏌曢崱鏇狀槮闁宠閰ｉ獮瀣偐閻戔晜鏁介梻浣筋嚙閸戠晫绱為崱娑樼婵炲棙鍔栭崣蹇旂箾瀹割喕绨奸柍閿嬪笒闇夐柨婵嗘噺閸熺偤鏌涢悢鍝勪沪闁靛洤瀚伴、姗€鎮欓幖顓燂紗闁诲氦顫夊ú姗€宕曟總鏉嗗洭寮婚妷锔惧幈闁诲函缍嗘禍鐐存櫏婵犳鍠栭敃銈夊箹椤愶絾娅忛梻浣规偠閸庢粓宕ㄩ浣镐簼闂傚倸鍊烽懗鍫曘€佹繝鍥舵晪婵犲﹤鎳忓畷鍙夌節闂堟侗鍎忕紒?444 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愮儤鍋嬮柣妯荤湽閳ь兛绶氬鏉戭潩鏉堚敩銏ゆ⒒娴ｈ鍋犻柛搴㈡そ瀹曟粓鏁冮崒姘€梺鍛婂姦閸犳鎮￠妷鈺傜厸闁搞儺鐓堝▓鏂棵瑰鍫㈢暫婵﹤鎼晥闁搞儜鈧崑鎾澄旈崨顓狅紱闂佽宕橀崺鏍х暦閸欏绡€闂傚牊绋掑婵堢磼閳锯偓閸嬫捇姊绘担渚劸闁哄牜鍓涢崚鎺戠暆閸曗斁鍋撻崒姣椽顢旈崨顏呭闂備浇濮ら敋妞わ富鍨跺鎶芥偄閸忚偐鍘遍梺缁樏壕顓熸櫠閻㈢鍋撳▓鍨灈妞ゎ厼鍢查锝夊箻椤旇棄浜滈梺鎯х箺椤曟牠宕惔銊︹拻濞达絿顭堥ˉ蹇涙煟閹惧磭澧︾€规洑鍗冲浠嬪Ω瑜忚ぐ?502闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚敐澶婄闁挎繂鎲涢幘缁樼厱濠电姴鍊归崑銉╂煛鐏炶濮傜€殿噮鍣ｅ畷濂告偄閸涘鍞堕梻鍌欒兌椤牓顢栭崱娑樼闁告挆鍐ㄧ亰濡炪倖鎸鹃崑鎰ｉ崼鐔剁箚妞ゆ牗绻嶉崵娆愮箾閸涘洤娲﹂埛鎴炵箾閼奸鍤欐鐐搭殜閺岋綁鎮㈤崣澶嬬彋閻庢鍠栭…鐑藉箖閵忣澀鐒婂ù锝呮贡閳ь剙顭峰濠氬礋椤愶絽鏋犲銈冨灪娣囨椽藝閺屻儲鍋傞柕鍫濇娴滄粓鏌熼崹顔兼殲濠电偛娲弻宥堫檨闁稿繑绋撶划鍫熺瑹閳ь剟鏁愰悙宸叆闁割偅绻勯崝锕€顪冮妶鍡楃瑐闁绘帪绠撳畷锝堢疀濞戞瑧鍘撻悷婊勭矒瀹曟粓鎮㈤悡搴ゆ憰闂佹寧绻傞幏瀣焽閵娾晜鐓冪憸婊堝礈濞嗘挴鈧箓宕堕宥喰梻渚€鈧稓鈹掗柛鏃€鍨垮顐﹀磼閻愯尙顓洪梺鍦劋濮婂鎮炬ィ鍐┾拻濞达綀娅ｇ敮娑㈡煕閺冩捇妾紒鍌氱Т椤劑宕煎┑鍫濆Е婵＄偑鍊栫敮濠囨嚄閼稿灚娅犳い蹇撴噷娴滄粓鏌￠崒婵囩《缂佺姾灏欑槐鎺旂磼濡偐鐣靛銈庡亝缁诲牓銆佸Δ鍛＜闁挎梹鍎抽懓鍧楁⒒閸屾艾鈧兘鎳楅崼鏇炵疇闁规儳鐡ㄥ▍鐘绘煟閿濆懓瀚伴柛銊︾箖閵囧嫰寮介顫勃闂佺顑呴崐鍧楀蓟閿熺姴鐐婇柍杞版濮规姊虹悰鈥充壕婵炲濮撮鍡涙偂閻旈晲绻嗘い鏍ㄧ箥閸ゆ瑧绱掗懠棰濆殭闂囧绻濇繝鍌涘櫣濞寸娀浜堕弻鈥崇暆鐎ｎ剛袦濡ょ姷鍋為敃銏ゅ箖濞嗘挻瀵犲璺侯槸閸ㄦ繈姊婚崒娆戭槮闁圭⒈鍋婇幆澶嬬附缁嬭法鐛ラ梺鍝勭▉閸樺綊鍩€椤戣法顦﹂柍璇查叄楠炴﹢寮堕幋鐐垫瀫闂傚倷绶氬褔篓閳ь剛绱掗幓鎺嬪仮闁硅壈顫夐幆鏃堬綖椤撶姷鐣鹃梻渚€娼ч悧鍡椕洪妶鍛瘎闂傚倷鑳堕…鍫ヮ敄閸℃稒鍎庢い鏍亼閳ь兛绶氬浠嬪Ω閵壯呯嵁闂備礁缍婇崑濠囧礈濞戙垺鍋╂繛宸簼閳锋垿鏌涢幇顒€绾ч柟顖氱墦閺岀喎鐣￠柇锕€鍓堕悗娈垮櫘閸嬪﹤鐣峰鈧崺鐐村緞閸濄儳娉垮┑锛勫亼閸婃牠宕濋敃鍌氱；闁告洦鍨冲畵渚€鏌″鍐ㄥΩ濞存粍绮撻弻鏇熷緞濞戞氨鏆犻梺鍛婄箘閸庛倝骞堥妸锔剧瘈闁告洦鍘肩粭锟犳⒑閻熸澘妲婚柟铏耿楠炴劖绻濋崘銊х獮婵犵數濮撮崯顐﹀礉閹烘鈷掗柛灞剧懅椤︼箓鏌熼懞銉х煁闁逛究鍔戦幃鐣岀矙鐠侯煈妲烽梻浣告惈濞茬娀骞婇崶顒€鐒垫い鎺戯功閻ｇ數鈧娲栫紞濠囥€佸璺哄窛妞ゆ棁鍋愮槐锕傛⒒閸屾瑧绐旈柍褜鍓涢崑娑㈡嚐椤栨稒娅犳い鏂款潟娴滄粓鏌￠崶鏈电敖缂佸鍠楅〃銉╂倷閺屻儱寮伴梺鐟扮－閸嬨倖淇婇悜钘壩ㄩ柕鍫濇川濞夊潡姊婚崒姘偓鐑芥倿閿曞倸纾跨€规洖娲﹀畷鏌ユ煙闁箑澧鹃柤鏉挎健濮婃椽顢楅埀顒傜矓閻㈢鐤炬い鎺嶈兌缁♀偓婵犵數濮撮崐鎼佸几閵堝棛绠鹃柟鐐墯閻撳ジ鏌＄仦鐐鐎规洜鍘ч埞鎴﹀炊閼哥绱掗梻鍌欒兌缁垶骞愰崼鏇炵？闁汇垻顭堥拑鐔兼煟閺傛寧鎯堥柡瀣墦閺屾稑螖閸愩劋绮х紓浣靛妼閵堢顫忓ú顏呯劵婵炴垶鍩冮弫鈧梻浣告啞濮婂綊宕归悽鍛婂仼闁绘垼妫勭粻鎶芥煙鐎涙ɑ鈷愰柡浣哄█濮婅櫣鎷犻垾宕団偓濠氭煕鐏炴儳鍤俊顐ｇ矊閳规垿鎮╅崹顐ｆ瘎闂佺顑嗛惄顖炲箖瑜斿畷鍗炩槈濞嗗繆鍋撻懜鐢电瘈闂傚牊渚楅崕蹇涙煕閹寸姴校缂佺粯鐩獮瀣枎韫囨洑鐥梻浣虹帛閹搁箖宕伴弽顓炶摕闁炽儱纾弳銈嗐亜閺冨洤鍚规い锔肩畱闇夐柣娆屽亾闁哥姵鐗為悘鍐⒑缂佹﹫鑰挎繛浣冲嫮顩锋繝濠傜墛閻撴洟鏌ｉ弮鍫闁圭晫濞€閺岀喐绗熼崹顔碱潎闂佺硶鏂侀崑鎾愁渻閵堝棗绗掗悗姘煎弮閹ょ疀濞戞瑧鍘卞銈嗗姧缁茶法绮婚弽銊ｄ簻闁靛／鍐ｆ瀰闂佸搫鏈粙鎴﹀煝鎼淬倗鐤€闁哄啯鍨堕敍鍡涙⒒娴ｅ憡鎯堥柣顓烆槺瀵板﹥銈ｉ崘銊х暰闂佸搫鍟悧婊堝极瀹ュ棛绡€濠电姴鍊归崯妤呮煥濠靛棙顥犵紒鐘荤畺閹鈽夊▎妯煎姺闂佹椿鍘奸澶愬蓟閿濆憘鏃堝礃閵婃劑鍨介弻锝夋晲閸パ冨箣濡ょ姷鍋涢澶愬极閸愵喖鐒垫い鎺戝€绘稉宥夋煠婵劕鈧澹曢挊澹濆綊鏁愰崪浣圭稑濡炪倕绻愰幊鎰般€呴柨瀣鐎瑰壊鍠曠花濂告煃闁垮鐏撮柡灞剧☉閳藉顫滈崼婵嗩潬闂備礁鐤囧Λ鍕囬悽绋胯摕闁靛ň鏅涢崡铏亜韫囨挻顥犻柡鍡愬€濋幃妤冩喆閸曨剛锛橀梺鎼炲妺缁瑥顕ｆ繝姘耿婵°倕锕ら幃鎴︽⒑缁洍鍋撳畷鍥╊唹闁诲孩鑹鹃妶鎼佸蓟閿濆鍋勯柛婵勫劤閻撳倹绻濋埛鈧崟鍨暭闂佽桨绶￠崳锝夌嵁閹烘绠诲Λ鐗堢箖闁裤倕鈹戦悙宸殶闁告鍥ㄥ仱闁靛ň鏅滈崑鍌炴煛閸ャ儱鐏柣鎾寸懇閺岀喖顢涢崱妤勫婵炲牆鐖煎铏光偓鍦У椤ュ銇勯敂鐐毈鐎殿喖顭烽弫鎾绘偐閹绘帟鈧潡妫呴銏″婵﹤缍婇幃闈涱潩椤戣姤鏂€濡炪倖姊婚妴瀣绩缂佹ü绻嗛柣鎰閻瑦顨ラ悙璇ц含闁诡喗鐟╁畷顐﹀礋鎼存稑濡介柕鍥у瀵噣鍩€椤掑嫬纾块柧蹇ｅ亝閸欏繑绻涘顔荤凹闁抽攱甯掗湁闁挎繂鎳忛崯鐐烘煙閼碱剦鐒鹃柍瑙勫灴椤㈡瑩鎳為妷銉ユ敪闂備礁鎼懟顖炴晝椤忓牆鏋侀柟閭﹀幖缁剁偛鈹戦悩鎻掓殲鐞氾綁姊婚崒娆戠獢婵炰匠鍤躲劑鍩€椤掑倻纾奸柣妯哄暱閻忊晜绻涢崱鎰伈闁诡喗鐟╅幃婊兾熼悡搴＄闂傚倸鍊风欢锟犲矗韫囨洜涓嶉柟瀵稿仦濞呯娀鏌ｅΔ鈧悧鍕濠婂牊鐓涢柛鎰剁到娴滈箖鏌ｉ姀鈺佺仭閻㈩垽绻濋崹楣冨籍閸繄顦ㄥ銈嗘煥濡插牐顦归柡灞剧洴閸╁嫰宕橀鍛珣婵＄偑鍊ら崐銈夊炊閵娿垺瀚兼繝鐢靛仩鐏忣亪顢氳椤曪絾銈ｉ崘鈺冨幈闂佺粯蓱閸撴艾鈻撳Ο鑲╃＜妞ゆ棁顫夊▍濠囨煙椤斿搫鐏查柟顔瑰墲閹棃鍨惧畷鍥ュ仏闂傚倸鍊烽懗鍓佸垝椤栨娑欐媴缁洘鐎洪梺鎸庣箓椤︿即宕曟繝鍥ㄧ厱妞ゆ劗濮撮崝婊堟煃闁垮鐏﹂柟渚垮姂閹兘鎮ч崼鐔稿€烽梻浣筋嚙缁绘劕顕ｉ崜浣瑰床婵炴垶鐟︽刊鎾煕閹邦厼绲绘い蹇ｅ亝閵囧嫰鍩￠崨顔芥殸缂備胶绮换鍐崲濠靛纾兼慨姗€妫挎潻妯肩磽閸屾瑦绁版い鏇嗗吘娑樷枎閹炬緞锕傛煕閺囥劌鐏犵紒鐘崇洴閺屽秵娼幏灞藉帯婵炲鍘х€氭澘顫忛搹鍦＜婵☆垵娅ｉ悷鏌ユ⒑鐠団€虫灈闁稿﹤鐏濋锝囨嫚濞村顫嶉梺闈涚箳婵兘宕㈤幘顔解拺閻犲洠鈧磭鈧鏌涚仦鎯у毈婵☆偅绮岄埞鎴︽偐濞堟寧娈扮紓浣介哺濞茬喎鐣烽幋锕€绠ｉ柨鏇楀亾缁炬儳缍婇弻鈥愁吋鎼粹€茬凹闂佸搫妫欑划鎾诲蓟閻斿吋鍊绘慨妤€妫欓悾鍫曟⒑濮瑰洤鈧倝宕板Δ鍛﹂柛鏇ㄥ灠閸愨偓濡炪倖鍔﹀鈧繛宀婁邯濮婅櫣绮欏▎鎯у壋闂佸摜濮甸崝娆愪繆閻㈢绀嬫い鏂垮⒔閺夋悂姊虹憴鍕婵炲鐩矾闁逞屽墴濮婄粯鎷呴搹鐟扮闂佽鎮傜粻鏍х暦娴兼潙鍐€妞ゆ挾鍋犻幗鏇㈡⒑閸濆嫭宸濋柛鐘虫尵瀵囧焵椤掑嫭顥婃い鎰╁灪婢跺嫭绻涢崣澶岀煉鐎殿喗濞婇幃鈺冪磼濡攱瀚奸梺鑽ゅТ濞测晝浜稿▎鎴犱笉婵鍩栭悡娑㈡煕濞戝崬鏋ょ紒鐘成戞穱濠囧矗婢跺﹤顫掑Δ鐘靛仦鐢繝鐛Ο灏栧亾闂堟稒鎲搁懖鏍⒒閸屾瑧绐旀繛浣冲洠鈧箓宕奸妷銉э紵闂佺懓澧庨悺鏃堝极閸曨垱鐓曠€光偓閳ь剟宕戦悙鍝勭厱闁硅揪闄勯埛鎴炪亜閹扳晛鈧洘绂掑鍫熺厾婵炶尪顕ч悘锟犳煛閸涱厾鍩ｇ€规洘绮忛ˇ瀵哥棯閸撗勬珪闁逞屽墮缁犲秹宕曢柆宥呯疇闊洦铔嬫径鎰€烽柣銏㈡暩閿涙粓姊虹粙鎸庢拱婵ǜ鍔岄悺顓熶繆閻愵亜鈧倝宕㈣ぐ鎺戠闁瑰瓨绻勯弳锕傛煟閹惧磭宀搁柡鈧禒瀣厱闁靛鍠曠花浠嬫煕濞嗘劕鐏存慨濠傤煼瀹曟帒顫濋钘変壕濡炲瀛╅浠嬫煥閻斿搫孝缂佲偓閸喓绠鹃柛鈩兠悘鈺呮煟椤撶喐宕岄柡宀嬬秮婵偓闁绘ê鍟块弳鍫ユ⒑缁嬫鍎忛柛濠傛健瀵鏁愭径濠冾棟闂佸壊鐓堥崰妤呭磹椤栫偞鈷戠紓浣股戠亸顏堟煕鎼淬倕鐨洪柟骞垮灩閳规垹鈧綆鍋掑Λ鍐ㄢ攽閻愭潙鐏﹂柣鐕佸灠铻為柨鐔哄У閳锋垿鏌ｉ悢鍛婄凡闁诡喗鍨块弻娑㈠籍閳ь剙煤閻斿嚖鑰垮ù锝囩《閺€浠嬫煟濡鍤嬬€规悶鍎甸弻娑㈡偆娴ｉ晲鍠婇梺绯曟櫆閻╊垰顕ｉ鈧畷鎺戭煥閸滃啰搴婇梻浣告惈椤︻垶鎮ч崟顖氱閻忕偟鐓ぐ鎺濇晩闂佹鍨版禍楣冩偡濞嗗繐顏紒鈧崘顔藉仺妞ゆ牗绋戝ù顕€鏌ㄥ┑鍫濅粶闁宠鍨垮畷浼存偋閸繄鐟ㄩ梺瀹狀嚙缁夊爼骞忛悩璇茬煑濠㈣泛锕ラ悵鎾⒒娴ｄ警鐒炬い鎴濇楠炴劖銈ｉ崘銊х枀闂佸湱铏庨崰鏍矆鐎ｎ偁浜滈柟鐑樺灥娴滅偞淇婇懠顒佸殌闁宠鍨块幃娆撴嚑椤掍焦鍠栫紓鍌欑贰閸犳牠鎯岄崒鐐茬畺闁靛濡囬梽鍕煕濞戞﹫宸ュù鐘冲笒椤啴濡堕崱妯硷紩闂佺顑嗛幐楣冨焵椤掑喚娼愭繛鍙夌墵閹儲绺介幖鐐╁亾娴ｈ倽鏃堝川椤撶姴濮︽俊鐐€栫敮濠勭矆娓氣偓瀹曟繈寮崼鐔叉嫽闂佺鏈悷褏绮ｉ弮鈧换娑欐媴閸愬弶纭介柛銉墻閺佸洭鏌曡箛鏇炐ラ柨娑欑懇濮婃椽宕崟顒傤槺闂?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌涘☉姗堟敾闁告瑥绻橀弻锝夊閻樺樊妫岄梺杞扮閿曨亪寮婚垾鎰佸悑閹肩补鈧磭顔愮紓鍌欑劍閸旀牠銆冮崱妯尖攳濠电姴娲ゅ洿闂佸憡渚楅崢钘夆枔閺屻儲鈷戦柛婵嗗椤ョ偟绱掗鑲┬ｇ紒宀冮哺缁绘繈宕堕‖顑洦鐓曟繛鎴濆船楠炴绻涢崼婵堝煟婵﹤鎼叅閻犲洦褰冪粻娲⒑閼姐倕鏆遍柡鍛Т椤曪絾绻濆顓炰簻闂佺绻掗崢褔鍩€椤掑倻甯涘ǎ鍥э躬椤㈡盯鎮欑€涙褰哥紓鍌欑贰閸犳牠鎮ラ悡搴綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顦插褏鏅Σ鎰板箳濡や礁浜归梺鎯ф禋閸嬪嫮鍠婂澶嬧拺濞村吋鐟х粔顕€鏌￠埀顒佹綇閳哄啰澶勯梺浼欑到閻ジ鎯屽▎鎾村仯闁搞儻绠戠€氬酣鏌涢弬璺ㄐу┑锛勬暬瀹曠喖顢涘杈╂綁闂備焦鎮堕崕娲倶濠靛绠婚悗闈涙憸椤旀洟姊洪崨濠佺繁闁告ɑ鍎抽悺顓㈡⒒娴ｅ摜鏋冩い顐㈩樀瀹曞綊宕稿Δ鈧粻鏍煃閸濆嫭鍣归柛銊ュ€归妵鍕箛閸撲胶蓱闂佷紮绠戞鎼佸煘閹达附鍋愰柟棰佺閺呴亶姊洪崫銉バｇ€光偓缁嬫鍤曟い鎰剁悼缁♀偓濠殿喗锕╅崜娑㈡偪閸パ€鏀介柣妯肩帛濞懷勩亜閹存繃鍣界紒顔肩墦瀹曞ジ濮€閳ヨ櫕鐎鹃梻濠庡亜濞诧妇绮欓幒妤€纾挎俊銈呮噺閻撴洟鏌嶉埡浣告殶闁愁垱娲熼弻锝夊箻鐎涙顦梺閫炲苯澧柛妯荤矒瀹曟垿骞樼紒妯煎帗閻熸粍绮撳畷婊堟偄妞嬪孩娈鹃梺纭呮彧闂勫嫰宕愰悜鑺ョ厽闁瑰浼濋鍛洸婵°倐鍋撻柍瑙勫灴閹瑩宕ｆ径妯伙紒闂備礁婀遍…鍫モ€﹂悜钘夌畺闁炽儲鏋奸弸搴ㄦ煙閹佃櫕娅冪紒銊ヮ煼濮婃椽宕崟顓夌娀鏌涢弬鍧楀弰闁糕晛锕鎾偐閻㈢绱查梻浣筋嚃閸ㄥ崬顭垮鈧幆鍐洪鍛幍濡炪倖娲栭幊搴ㄥ几濞戞瑣浜滄い鎰剁悼缁犵偤鏌熼搹顐ょ煂闁哥姴锕よ灒閻犲洤妯婃导鏍⒒閸屾艾鈧绮堟担闈╄€块梺顒€绉埀顒婄畵瀹曠厧顭垮┑鍡欑Ш闁诡喗绮撻幐濠冨緞鐏炶棄绗岄梻鍌欑閹碱偄煤閵娾晛绐楃€广儱娲﹀畷鏌ユ煕閳╁叇婊勭濠婂牊鐓欓柟浣冩珪濞呭懘鏌ｈ箛濠冩珚闁哄矉绻濆畷閬嶎敇閻樺啿浠归梻渚€鈧偛鑻晶顖涖亜閺冣偓閻楃姴鐣锋导鏉戝唨妞ゆ挻绋堥崑鎾绘晝閸屾稓鍙嗛柣搴岛閺呮繄绮诲鑸碘拺鐟滅増甯楅敍鐔虹磼閳ь剚鎷呴崨濠冩闂傚倸鍊风欢姘焽瑜忛幑銏ゅ幢濡烆澁缍佸畷銊╊敇瑜忛崝鐑芥⒑閸濆嫬鏆婇柛瀣尵缁辨帞绱掑Ο鑲╃杽闂佽鍠曠划娆徫涢崘銊㈡婵°倕鍟畷鐔封攽閻樻鏆俊鎻掓嚇瀹曟垿宕ㄧ€涙ê浠梺瀹犳〃閼冲爼寮抽敂鐣岀瘈闂傚牊绋掗悞楣冩煛閸愩劎澧曢柣鎺戠仛閵囧嫰骞掗幋婵愪患闂佺粯鎸荤换鍫ュ蓟閻旂厧绠查柟浼存涧濞堫參鏌涢妷銉ф憼缂佺粯绻堟慨鈧柨婵嗘閵嗘劕顪冮妶鍡楃仴闁稿﹤娼￠悰顔跨疀閹句焦妞介、鏃堝川椤栨艾绠版繝鐢靛О閸ㄧ厧鈻斿☉銏″殣妞ゆ牗绋撻々鐑芥煙閻戞﹩娈曢柣鎾存礃娣囧﹪濡堕崒姘闂備礁鎽滈崰搴☆焽閿熺姴鐏抽柡宥冨妿缁♀偓闂佹悶鍎崝宀勫焵椤掆偓椤兘寮婚妶澶婄畳闁圭儤鍨垫慨銏ゆ⒑閸涘鐒惧鏉戞憸閹广垹鈹戠€ｎ偄浠洪梻鍌氱墛閸掆偓闁靛繈鍊栭悡鏇㈡煟閺冨偆鐒炬い銉ヮ儑缁辨帡顢欑喊杈╁悑闂佽鍠楅悷鈺呭箖濠婂吘鐔兼偂鎼达紕顔戞繝鐢靛Х閺佹悂宕戦悙鍝勫瀭闂傚牊绋撻弳锔姐亜閹烘垵顏╅柛鎴犲█閺屾洟宕煎┑鎰ч梺缁樻尭閸熶即骞夌粙娆剧叆闁割偅绻勯ˇ顓㈡⒑闂堟丹娑㈠川椤栵絾笑闂傚倷鑳剁涵鍫曞礈濠靛牏鐭欓柟瀵稿Х閻滅粯淇婇妶鍕樂缂佽妫濋弻鏇㈠醇濠靛洤娅ら梺闈╃稻濡炰粙寮诲☉姘ｅ亾閿濆簼绨婚柍褜鍓欏鈥愁嚕婵犳艾鍗抽柣鏃堫棑缁愮偤姊虹化鏇炲⒉閽冮亶鏌ｅ┑鍥ㄧ婵﹨娅ｇ划娆撳礌閳ュ啿顫犻梻浣芥〃缁€渚€鈥﹀畡閭﹀殨濠电姵鑹炬儫闂侀潧顦崹娲棘閳ь剟姊绘担铏广€婇柛鎾寸箖缁楃喎螖閸涱厾鍔﹀銈嗗笂缁垛€斥枔閺冣偓閵囧嫰顢橀垾鍐插Х濡炪伇鍌滅獢闁哄本鐩俊鎼佸Ψ瑜嶉悡鐔兼倵濞堝灝鏋︽い鏇嗗浂鏁囬柛蹇曞帶缁剁偤鎮楅敐搴″鐎殿喗濞婂铏规嫚閹绘帒姣愮紓鍌氱Т濡繂鐣烽幋锕€骞㈡繛瀛樻緲濞差厽淇婂宀婃Ь缂佺偓宕樺Λ鍕箒闂佹寧绻傞悧濠囶敂閻樼粯鍋ㄦい鏍ㄧ〒濞叉挳鏌″畝瀣М闁糕斁鍓濈换婵嬪磼濠娾偓濡楁捇姊绘担铏瑰笡濞撴碍顨婂畷鏉款潩鐠鸿櫣鍔﹀銈嗗笒閿曪妇绮旈悽鍛婄厱闁绘ɑ鍓氬▓婊呪偓瑙勬礃閸旀瑩骞冨鍫熷殟闁靛／鍐ㄧ婵犵數濮伴崹鐓庘枖濞戙埄鏁勯柛娑樼摠閸婂爼鏌嶉崫鍕櫤闁绘挶鍎茬换娑㈠箣閻戝洣绶靛┑陇灏欓幊鎾瑰絹闂佹悶鍎滃鍫濇儓闂備浇顕栭崰妤呮偡閳哄懌鈧線寮崼婵堫槹闂侀潧顭堥崕鐗堢珶閺囥垺鈷戦柛婵嗗閻т線鏌嶈閸撶喎顕ｉ幖浣稿唨妞ゆ挾鍠撻崣鍡涙⒑閸濆嫭宸濋柛瀣洴閸┾偓妞ゆ巻鍋撻柟鑺ョ矌閸掓帡宕奸埗鈺佷壕闁挎繂楠告禍鏍瑰鍕煉闁哄瞼鍠栧畷顐﹀礋椤掑顥ｅ┑鐐茬摠缁挾绮婚弽顓炵畺婵°倐鍋撻柍缁樻崌瀹曞綊顢欓悾灞奸偗濠电姷鏁搁崑娑㈡儍閻戣棄鐒垫い鎺嶇婢ь垱绻涢崗鑲╁缂佺粯绋戦蹇涱敊閼姐倗娉跨紓鍌欒閸嬫捇鏌涢幇闈涙灍闁绘挻娲樼换娑㈠幢濡や胶顩板┑鈩冨絻缂嶅﹪寮诲☉娆愬劅闁靛繆鈧磭锛撴繝娈垮枛閿曘儱顪冩禒瀣畺闁绘垼妫勭痪褔鏌熺粙鍨劉闁伙讣绲剧换婵嬫偨闂堟稐绮跺銈嗘处閸欏啫鐣峰Δ鈧～婊堝焵椤掑嫭鍋樻い鏃囨缁剁偤鏌熼柇锕€澧版い鏃€甯掗—鍐Χ閸℃﹩姊挎繝娈垮枔閸婃洟鍩㈠澶婂耿婵☆垵鍋愰惁鍫濃攽閻愯尙澧曢柣蹇旂箞瀵悂鎮㈤崗鑲╁幈闂佸搫鍟犻崑鎾绘偨椤栨粌浠遍柟宕囧仦濞煎繘濡歌閻﹀牆鈹戦鏂や緵闁告ɑ鎮傞獮蹇撁洪鍛嫼闂佸憡绋戦敃锔剧不閹剧粯鍊垫慨妯煎帶閺嬶箓鏌嶉鍕Ш婵﹦绮幏鍛村川婵犲倹娈橀梻浣告惈鐞氼偊宕濋幋锕€钃熷┑鐘叉处閺呮彃顭跨捄鐚村姛闁伙綁绠栭幃妤呭礂婢跺﹣澹曢梻渚€鈧偛鑻晶鎾煕閳规儳浜炬俊鐐€栫敮鎺楁晝閿斿墽鐭撻柣銏犳啞閻撴洟鏌ｅΟ璇插婵炲牊绮撻弻鐔碱敊閸忓浜鹃柡鍌樺劜椤秹姊洪棃娑㈢崪缂佽鲸娲熷畷銏ゆ焼瀹ュ棌鎷洪梺鍝勫€堕崕鎻掆枍閸涘瓨鐓曢柣妯虹枃婢规﹢鏌嶇紒妯诲碍閾伙綁鏌ゅù瀣珖闁挎稒绮撳铏圭磼濡浚浜獮鏍敃閿曗偓鐟欙妇鎲搁悧鍫濈瑲闁绘挸鍟伴幉绋款煥閸繄顦┑顔斤耿椤ゅ倿寮繝鍥ㄧ厸鐎广儱楠搁獮鏍磼閹邦収娈滈柡灞糕偓鎰佸悑閹肩补鈧磭顔戦梻浣侯焾椤戝懘藝椤栫偛鐓橀柟杈剧畱绾惧吋绻濇繝鍌涙崳闁告梹鎮傚娲传閸曨偒浠撮梺绋挎捣閺佽顕ｇ拠宸悑濠㈣泛锕﹂崢鍛婄箾鏉堝墽鍒伴柟娴嬧偓鏂ユ灁闁告瑥顦辩弧鈧┑鐐茬墕閻忔繈鎮橀敓鐘崇厵闁告稑锕ら埢鏇燁殽閻愭彃鏆欓柣锝忕節楠炲秹鎼归銈傚亾婵犳碍顥婃い鎰╁灪婢跺嫰鏌熺亸鏍ㄦ珕缂佸倹甯楅妶锝夊礃閳圭偓瀚肩紓鍌欑椤戝懐浜搁崨瀵稿彆妞ゆ帒瀚悡娑㈡煃瑜滈崜姘焽韫囨稑鐓涢柛灞捐壘缁ㄣ儲绻濋悽闈涒枅婵炰匠鍏炬稑鐣濋崟顐㈠殤闂佸搫鍟悧濠囨偂閺囩喆浜滄い鎾跺枎閻忋儵鏌＄€ｎ剙鏋涢柡宀嬬秬缁犳盯骞樼紓骞洦鐓涢悘鐐额嚙閳ь剚绻傞悾鐤亹閹烘挻娅囬梺閫炲苯澧悡銈夋煥閺囩偛鈧綊宕戦崒鐐寸厸闁搞儯鍎遍悘顏堟煟閹惧娲撮柡灞剧洴楠炲洭宕滄担绋跨厒濠电偛鐡ㄧ划鎾剁不閺嵮屾綎婵炲樊浜滅粻褰掓煟閹邦厼绲绘い顒€鐗嗛埞鎴︽倷閻愬厜鍋撶€ｎ剚宕叉繝闈涱儑瀹撲線鏌涢幇銊︽珔妞ゆ洟浜堕弻鐔哄枈閸楃偛绠洪梺鍝勬噳閺呯姴顫忓ú顏勭闂傚倻顥愰崺鍛存⒑缁嬪潡鍙勫ù婊嗘硾閻ｅ嘲鈹戠€ｎ亜绐涢柣搴㈢⊕鑿ら柟閿嬫そ濮婂宕掑鍗烆杸缂備礁顑嗛幐鍓у垝婵犳碍鍊烽柣鎴烆焽閸橀亶姊洪棃娑辩劸闁稿孩妞介幃鈩冪附閸涘﹦鍘靛銈嗙墬閻熝呯玻閺冨倵鍋撶憴鍕闁靛牆鎲℃穱濠囨倻閼恒儲娅嗛柣鐔哥懃鐎氼剟顢旇ぐ鎺撯拻闁稿本鐟чˇ锕傛煙鐠囇呯？缂侇喗鐟╅獮瀣晜閼恒儲鐝栭梻浣瑰缁诲倸煤閿曞偆鏁冨ù鐘差儐閳锋垶銇勯幒鍡椾壕缂備礁顦遍弫濠氬箖閿熺姴鍗抽柕蹇ョ磿閸橆亝绻濋悽闈浶㈢紒缁樺浮閹箖鏌嗗鍡欏帗缂傚倷鐒﹁摫缂佲偓閸愵喗鐓忛柛銉戝喚浼冩繝娈垮枓閸嬫捇姊洪幐搴ｂ槈閻庢凹鍙冨畷铏鐎涙ê鈧灚绻涢崼婵堜虎闁哄鍠栭弻鐔奉潰鐏炲墽褰ч梺閫涚┒閸旀垿宕洪埀顒併亜閹烘垵顏柣鎾冲暣濮婂搫鈻庨幆褏浠╂繛瀛樼矒缁犳牕顫忓ú顏呭仭闁哄瀵т簺闂備礁婀遍埛鍫ュ磻婵犲洤鏄ラ柣鎰惈缁狅綁鏌ㄩ弮鍥棄闁逞屽墰閸忔﹢骞冮悷鎳婃椽顢旈崱娆戠崶闂備浇妗ㄧ粈渚€鎮樺┑瀣р偓鏃堝礃椤斿槈褔鏌涢埄鍐剧劸闁稿绶氬娲川婵犲啫顦╅梺鍛婄懃妤犲摜鍙呴梺褰掓？閻掞箓鍩涢幋锔藉仭婵炲棗绻愰鈺傘亜閵夈儺鍎忔い顓″劵椤﹁櫕銇勯妸銉含妤犵偛鍟撮弫鎾绘偐閹绘帒绁梻渚€娼х换鍡涘焵椤掍焦鐏遍柛瀣崌閹粙宕ㄦ繝鍕汲婵犵數濞€濞佳兾涘┑瀣垫晛婵°倐鍋撻棁澶嬬節婵犲倸顏紒鐘筹耿閺屽秷顧侀柛鎾寸箞閿濈偞寰勬繛鎺撴そ瀵粙顢橀悢宄板闂備胶顭堥惉濂稿磻濞戙垺瀚?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鎯у⒔閹虫捇鈥旈崘顏佸亾閿濆簼绨奸柟鐧哥秮閺岋綁顢橀悙鎼闂侀潧妫欑敮鎺楋綖濠靛鏅查柛娑卞墮椤ユ艾鈹戞幊閸婃鎱ㄩ悜钘夌；闁绘劗鍎ら崑瀣煟濡崵婀介柍褜鍏涚欢姘嚕閹绢喖顫呴柍鈺佸暞閻濇牠姊绘笟鈧埀顒傚仜閼活垱鏅堕弶娆剧唵閻熸瑥瀚粈鍐磼缂佹绠撴い顐ｇ箞椤㈡﹢鎮╅幓鎺旂杽闂傚倸鍊烽悞锕傚几婵傜鐤炬繛鎴欏灩閻ゎ喖霉閸忓吋缍戦柣鎾偓鎰佺唵闁兼悂娼ф慨鍫ユ煟閹惧瓨绀嬮柟顔斤耿閹瑦锛愬┑鍡橆唲濠电姵顔栭崰鏍磹閸ф钃熼柍銉ョ－閺嗗棝鎮楅敐搴″鐞氾箓姊绘担鑺ャ€冮柣鎺炵畵瀹曟繂鈻庤箛鏇熸闂侀潧艌閺呪晠寮繝鍥ㄧ厾闁诡厽甯掗崝姘亜韫囧骸宓嗘慨濠傤煼瀹曟帒鈻庨幋顓熜滈梻浣告贡閳峰牓宕戦崟顔句罕婵犵數鍋涘Λ娆撳垂閻㈢鍨傛繝闈涱儐閻撴瑩鏌熺憴鍕Е闁搞倖鐟╅弻锕傚礃椤忓嫭鐏侀梺閫炲苯澧紒鐘茬Ч瀹曟洟鏌嗗鍡椾罕濠电姴锕ら崰姘舵倵閸洘鐓忓┑鐐靛亾濞呭棝鏌嶉柨瀣伌闁哄本绋戦埥澶婎潨閸喐鏆伴梺璇茬箰缁绘帡寮繝姘摕婵炴垯鍨洪崑鎰偓瑙勬礀濞层倝鍩呴悷閭︽富闁靛牆妫欑粚璺ㄧ磼閹绘帒鈷旈柟骞垮灩閳规垿宕遍埡鍌氬厞婵＄偑鍊栭崹鐔煎棘娓氣偓瀵煡鎮╃拠鑼舵憰闂佸搫娲ㄩ崰搴ｆ閻愮儤鐓曟い鎰剁悼缁犳ê霉閻樺啿鍔ゆい顏勫暣婵¤埖鎯旈垾鑼泿缂傚倷绀侀ˇ浼村箰閹惰棄鏄ラ柣鎰惈缁狅綁鏌ㄩ弴妤€浜鹃梺缁樻惈缁绘繈寮诲☉銏犵労闁告劗鍋撻悾鍏肩箾鐎电袥闁哄懐濮撮～蹇涙惞鐟欏嫬鐝伴梺鑲┾拡閸撴盯顢欐径宀€纾藉ù锝囩摂閸ゆ瑩鏌ｅΔ浣瑰磳濠碘€崇摠缁楃喖鍩€椤掆偓椤曪綁顢氶埀顒€鐣锋總鍓叉晝闁挎繂妫欓悵婵嬫⒒閸屾瑧绐旀繛浣冲懏宕查柛鈩冾殢閻庡墎鎲搁弮鍫㈠祦闊洦鎷嬪ú顏嶆晜闁告侗浜濈€氬ジ姊绘担鍛婂暈缂佸鍨块弫鍐晜閸撗咃紳濠殿喗顭堥崺鏍偂閺囥垹绠规繛锝庡墮婵℃椽鏌熼幓鎺戔挃闁逞屽墯椤旀牠宕板Δ浣虹濠电姴娲ら拑鐔兼煕閳╁喚娈㈤柛姘儔閺屾盯鍩勯崘顏呭櫑濡ょ姷鍋涢悧鎾愁潖閾忚瀚氶柟缁樺笒濮ｆ劗绱撻崒姘毙㈡俊顐ｇ箓閻ｇ兘骞嬮敃鈧婵囥亜閹捐泛校婵炲牓绠栭幃妤呭礂婢跺﹣澹曢梻浣告啞濞诧箓宕滃☉銏犲偍闁归棿鐒﹂崐鐢告煕韫囨搩妲稿ù婊堢畺閺岋絾鎯旈婊呅ｉ梺鍛婃尰缁嬫牠鍩呴悽鍛娾拻闁稿本鐟︾粊鐗堛亜閺囩喓澧电€规洑鍗抽獮鎺懳旈埀顒勬偪閻愵剛绡€濠电姴鍊搁弸銈夋煟閹哄秶鐭欓柣鎿冨亰瀹曞爼濡歌濡插牓姊虹紒妯诲鞍缂佸鍨垮﹢渚€姊洪幐搴ｇ畵闁瑰啿閰ｈ棢闊洦绋掗悡鐔煎箹鏉堝墽纾块柟鐣屽Х缁辨帞绱掑Ο鑲╃杽闂佽鍠曠划娆徫涢崘顭嬪綊宕滄担闀愭闂佸搫鏈惄顖涗繆閹间礁唯妞ゆ梹鍎抽幃鎴炰繆閵堝洤啸闁稿鐩弫鍐敂閸垹绁﹀┑鈽嗗灟鐠€锕傛偄閸℃稒鐓曢柡鍥ュ妼婢т即鏌嶉挊澶樻█闁哄苯绉堕幏鐘诲蓟閵夈儱鍙婃俊銈囧Х閸嬬偤鏁嬮梺浼欑悼閸忔ê鐣烽崼鏇炵厸闁稿本绋撻柦鎺楁⒒閸屾艾鈧悂宕愭搴㈩偨婵﹩鍓﹂悞鐣屾喐閹寸偞娅犳繝闈涱儐閳锋垿鏌涘┑鍡楊仾闁革綀娅ｇ槐鎺斺偓锝庡亝鐏忕敻鏌涢幒鎾崇瑲缂佺粯绻傞～婵嬵敇閻愭壆鏁鹃梻鍌欒兌閹虫捇宕崸妤佹櫇闁靛牆娲ㄧ粈濠傗攽閻樺弶鎼愰柡瀣╃窔閺岀喖鎮ч崼鐔哄嚒閻庣懓鎲＄换鍐Φ閸曨垰绫嶉柛銉厛濡嫰姊洪崫鍕靛剱闁搞劌鐏濋～蹇曠磼濡顎撴俊鐐差儏缁ㄨ偐鎲伴崱娆戠＝闁稿本姘ㄨⅵ闂佺顑嗛幐缁樼┍婵犲洦鍊烽悗娑欘焽缁嬪洭姊虹涵鍛撶紒顔芥崌閻涱噣骞嬮敃鈧～鍛存煟濮楀棗浜濇い顐㈢Т閳规垿鎮欓崣澶樻！闂佸憡姊归〃鍫㈠垝閺冨牆閱囬柡鍥╁暱閹锋椽姊洪崨濠勨槈闁挎洏鍎抽埀顒€鐏氶悷鈺呭蓟濞戙垹鍗抽柕濞垮劙缁ㄧ敻鏌℃径鍡樻珕闁挎岸鏌嶇憴鍕伌闁轰礁绉瑰畷鐔碱敃閳╁啯绶氶梻鍌欒兌椤牐鍣归梺鍛婁緱閸樺墽绮佃箛娑欌拺闁稿繗鍋愰妶鎾煛閸涱喚娲寸€规洜鎳撻埞鎴﹀炊閳哄喚鍟庨梻浣虹《閸撴繈銆冮崼銏笉闁规儼濮ら悡?
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
