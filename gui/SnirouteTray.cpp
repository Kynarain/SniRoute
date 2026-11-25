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

// WARP 闂傚倷绀侀幖顐も偓姘卞厴瀹曡瀵奸弶鎴犵暰婵炶揪绲芥竟濠囧疮閸涘瓨鐓犻柣鎾抽椤ｅ磭绱掗悩鑽ょ暫闁哄瞼鍠栭獮渚€骞掗幋婵嗩潙闂備線鈧偛鑻晶顕€鏌涙繝鍕珪闁瑰箍鍨介獮瀣晝閳ь剟鎮?0=闂傚倷绀侀幖顐︽偋濠婂嫮顩叉繝濠傜墛閸嬧晠鏌曟繛褍瀚弲?1=闂佽姘﹂～澶愭偤閺囩姳鐒婃い蹇撴瀹曞弶绻涢幋娆忕仼缂?2=闂傚倷绀侀幖顐︽偋濠婂嫮顩查柨婵嗘处瀹曞弶绻涢幋娆忕仼缂佺姵濞婇弻鏇熷緞濞戞艾鐭濋梺?
// 闂?warp-cli status 闂傚倷绀侀幉锛勬暜閸ヮ剙纾归柡鍥ュ灪閸嬧晛螖閿濆懎鏆為柡鍜佸墰閳ь剙鍘滈崑鎾绘煕閹板墎鍒板ù婊堢畺閹﹢鎮欑捄杞版睏闂佽崵鍠愮换鍫ュ蓟閿熺姴绀嬮梻鍫熺〒娴狀垰鈹戦悩顐壕闂佸憡渚楅崹宕囧妤ｅ啯鐓欓悗娑欘焽缁犳挻顨ラ悙顏勭伈闁哄被鍔岄埥澶娾枎閹寸偞鍎滱SQUE 闂傚倷绀侀幉锟犮€冮崨顖涘床婵°倕鎷嬮弫濠囨煕濞戝崬鏋ら柍缁樻閹鏁愭惔鈥愁潾缂備礁顦紞濠囧蓟濞戞﹩娓婚柣鎴旀櫅娴滈箖鏌ц箛锝呬函闁告凹鍋呯换娑欐綇閸撗冨煂濠碘槅鍋呴悷褏鍒掓繝鍥ㄦ櫇闁稿本绋撻崢鎺楁椤愩垺澶勯柟绋挎憸閹峰寮婚妷锔惧幘闂佹儳娴氶崑鍕倶婵犲嫮纾兼俊銈傚亾妞わ妇鏁诲濠氬川椤撗勫兊闂佸憡鎸嗛崪浣剐?
// 闂傚倷绀侀幉锟犳偡椤栨稓顩叉繝闈涙４閼?status 闂?Connected 闂傚倷绀佺紞濠傤焽瑜忕划鏃堝箮閽樺鍩炲銈嗗笒閸婂綊宕戝鈧弻鏇熺箾閸喖濮堕梺鍛婅壘閸婂潡寮婚悢鐓庣妞ゆ洖鎳忛ˉ鏍ь渻閵堝懏绂嬮柛鎾寸洴楠炲繒鈧綆鍠栧洿闂佸憡娲﹂崰姘端囨导瀛樷拻闁稿本纰嶉崯姘舵煕閵夘垳鐣辨い銉﹀哺濮婃椽宕ㄦ繝鍕櫧闂佹悶鍔嶆繛濠傤嚕椤愶箑围濠㈣泛谩?
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

// 闂傚倸鍊风欢锟犲磻閸涱喚鈹嶉柧蹇氼潐瀹?sniroute.cmd 闂傚倷绀佸﹢閬嶆偡閹惰棄骞㈤柍鍝勫€归弶?warp on/off闂傚倷鐒︾€笛呯矙閹寸偟闄勯柡鍐ㄥ€荤粻鏂款熆鐠鸿櫣鐏卞鐟板暱椤法鎹勯悮鏉戝缂備焦鍔х换婵嬪蓟閵娿儮妲堟俊顖濆吹娴兼亞rp-cli 闂備礁鎼ˇ顐﹀疾濠靛纾婚柣鎰仛閺嗘粓鏌熺紒銏犳灈闁活厽顨嗛妵鍕冀閵娧呯暤闂佸憡蓱閹告娊骞冮悜钘夊嵆婵﹢纭稿Σ顕€姊洪幖鐐测偓鏍垂閸︻厼鍨濋柡鍐ㄧ墕缁犳娊鏌ｉ幇顔芥毄妞わ富鍋婂?
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


// 闂?winget 闂備浇顕уù鐑藉箠閹惧嚢鍥箮閸?Cloudflare WARP闂傚倷绶氬褍螞濡ゅ懎纾瑰ù鐘差儐閹叉﹢鎮楅敐搴″幋闁稿鎸鹃幉鎾礋閸℃ê鐏寸€殿喖顭烽幃銏焊娴ｈ鏆婇梻浣筋潐濠㈡ɑ鏅舵惔锝囩焼濠㈣泛艌濡插牓鏌熼幆褏锛嶉崯鎼佹⒑閹肩偛鈧牜鍒掑畝鍕垫晩闊洦绋戠涵鈧梺缁樺姌鐏忔瑩鍩€椤掆偓椤兘寮诲☉妯锋瀻闁归偊鍓涙禒鎾⒑娴兼瑧绁烽柛鏂块叄楠炲螖閸涱厼绐涘銈嗘尵閸嬬喎螣娓氣偓濮?
// 婵犵數鍋炲娆撳触鐎ｎ喗鍤屽Δ锝呭暞閸ゅ洤鈹戦悩鎻掓殶闁崇粯妫冮弻宥堫檨闁告挻鐟╅崺?UAC闂傚倷鐒︾€笛呯矙閹存繐鑰跨紒妤冩珔P 闂備浇宕甸崰鎰洪幋婢濆綊宕堕锕€顦扮换婵嬪炊瑜嶉悵姗€鎮楅獮鍨姎闁瑰啿閰ｉ敐鐐哄Χ婢跺鍘卞┑鐘诧工閸熶即鎮為幖浣圭厓闁荤喐澹嗘晥閻庤娲樼换鍫ュ灳閿曞倸绠抽柡鍐ㄥ亞濡叉挳姊绘担鐟邦嚋缂佸鍨块幊鐔碱敍閻愭潙浠奸梺闈浤涢崟顐ｆ珴闂備礁鎲℃笟妤呭储妤ｅ啫姹叉慨妯垮煐閻撴洟鏌熼悜姗嗘畷闁轰線浜堕弻锛勪沪閺傘倛鍚梺鍦劜缁蹭粙鈥﹂妸鈺佺闁靛ě灞拘ч梻鍌欒兌缁垶鈥﹂崼銉ユ槬闁哄稁鍘奸悞鍨亜閹存繃鍋ユ俊鍓у厴閺?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 婵犵數鍋涢悺銊╁吹鎼淬劌纾规俊銈呭暙閸?CLI闂傚倷鐒︾€笛呯矙閹烘鍤屽Δ锝呭暞閸嬧晠鏌ｅΟ鍝勬毐妞ゆ洝椴搁幈銊ヮ潨閸℃顫╅柣銏╁灛閸庣敻骞?winget闂傚倷鐒︾€笛呯矙閹达箑瀚夋い鎺戝閸庡鏌涘☉娆愮稇閻?winget 闂備浇顕х换鎰崲閹邦垬浜瑰璺虹灱缁犳棃鏌涘☉鍗炴灍濠?Cloudflare 闂備浇顕у锕傦綖婢跺孩鎳岄梻浣告惈閻ジ宕伴幘璇茬劦妞ゆ帒鍊归崵鈧銈忛檮婵炲﹨妫㈡繝鐢靛Т濞层倗绮堝畝鍕厱婵炴垵宕禍鎰版煕?
        // 婵犵數鍋為崹鍫曞箰閸濄儳鐭撻梻鍫熷厷閿濆洨鐭欓柛鐔峰暞鐢喓绮诲☉銏犵闁规儼妫勮ぐ?Windows 闂傚倸鍊风欢锟犲窗濞戞瑦鍙忛柕鍫濇嫅閼?winget闂傚倷鐒︾€笛呯矙閹存繐鑰挎い銏㈢檮C闂傚倷绶氬褍螞閺冨牊鍤勯柛鎾叉祰婵娊鎮归幁鎺戝濞戞挸绉归弻宥堫檨闁告挾鍠栭獮鍐樄鐎规洜鍘ч埞鎴﹀醇濠靛牆骞€闂備浇宕甸崰鎰版偡閵堝洦顐介柕鍫濇处椤洘鎱ㄥΟ鎸庣【鏉?
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


// WARP 闂傚倷绀侀幖顐﹀磻閸℃稑鍌ㄩ柤娴嬫櫆椤洟鏌熸潏鍓х暠閻熸瑱绠撻獮鏍庨鈧悘鈺呭箚閻斿吋鈷戦柛婵嗗婢ч亶鏌涢幘瀵告噧闁?DNS 闂傚倷绀侀幖顐﹀磹娴犲缍栧鑸靛姂閳ь兛绀侀鍏煎緞婵犲嫷鍞靛┑鐘灱濞夋盯鏁冮妷褏鐭嗛柍褜鍓熷娲传閸曞灚肖闂佺瀵掗崹鍫曞春閳?
//
// 婵犵數鍋為崹鍫曞箲娴ｅ壊娴栭柕濞у啫鐏婇梺鍓插亞閸犳捇鍩㈤弴銏＄厱妞ゆ劗濮撮崝婊兠归悩绛硅€块柟顔惧厴瀵潙顫濇鏍ㄐ滈梻浣风串缁叉寧绻涢埀顒勬煛娴ｅ摜孝妞ゎ偁鍨介獮鈧┑澶嬫 闂傚倷绀侀幖顐も偓姘卞厴瀹曞綊鏌嗗鍛緢闂佹寧绻傞ˇ顖溾偓姘槹缁绘稑顔忛鑽ゅ嚬闂佷紮绠戦悧鎾愁潖濞差亶鏁嗗ù锝堫潐閸庢挾绱撴担浠嬪摵缂佽瀚伴崺鈧い鎺戝€归崵鈧梺鍝ュ櫏閸ㄦ娊寮鈧崺鈧い鎺戝€荤壕濂告煟閹伴潧澧い鏇熺矋娣囧﹪鎮欓幓鎺嗗亾閸ф绠栧ù鐘差儏鎯熼梺鎸庢磵閸嬫捇鏌ｉ幘瀛樼闁逞屽墮閻忔碍绔熺€ｎ喖纾婚柟鎯х亪閸嬫捇宕归銏╂闂佺顑嗛幐鎼佹箒闂佺粯锕╅崰鏍礉濞嗘挻鍋?缂傚倸鍊搁崐鎼佸疮椤栫偛鍨傞柛顐ｆ礀閽冪喖鏌涢妷锝呭妞?闂傚倷鐒︾€笛呯矙閹达附鍎楀〒姘ｅ亾闁糕斁鍋撳銈嗗坊閸嬫捇鏌ゅú璇茬仸闁诡喖娼″畷鐔碱敍濞戞帗瀵栭梻浣告啞娓氭宕滃☉銏″仭闁规壆澧楅悡娑㈡煕椤愵偄浜滃褜鍠楃换婵嬪焵?
// DoH/nginx 闂傚倷绀侀幉锟犳偡椤栫偛鍨傞柣銏㈩焾缁€鍌涙叏濡炶浜鹃悗瑙勬礀閻栧ジ骞冮埡鍌涚秶妞ゆ劑鍊ゅΣ褰掓⒒娴ｅ憡鍟炴い銊ユ噹鐓ら柕濠忓瘜濞堜粙鏌涢妷顔煎鏉╂繃绻涙潏鍓у埌闁硅姤绮撻崺鐐测槈濞嗘垹鐦堥悷婊冪Ч瀹曨垶濡搁埡浣哄姦濡炪倖鍨煎▔鏇㈠礉瀹ュ鐓曢柕濠忕到婵＄晫绱掗崒娑樼瑲闁诡垱妫冩慨鈧柍銉ョ－婢跺嫰姊绘担鍛婂暈闁告垵缍婂畷褰掓偋閸喎寮块梺鍐叉惈閹冲繘宕甸埀顒勬倵閸忓浜鹃梺閫炲苯澧い鏂跨箲濞煎繘濡搁妷褏鐛┑鐐存尰閼瑰墽鑺遍柆宥呯；闁规崘顕х壕鍏肩節闂堟稓澧㈡い鏃€鍔欏缁樻媴閻熸壆绁烽梺鍏兼た閸ㄨ京绮嬮幒鎴旀婵炲棙鍨熼弸鏍⒑闂堟稓澧曢悗姘煎弮椤㈡洟鎸婃竟婵堟嚀椤劑宕橀幆褍濡х紓?
//   1) 濠电姷顣藉Σ鍛村磻閳ь剟鏌涚€ｎ偅宕岄柡?5444(DoH 闂傚倷绀侀幉锟犲箰閸濄儳鐭撻柛顐ｆ礃閸? / 5454(DoH 闂傚倷绀侀幉锟犳嚌閹灐褰掓倻缁涘鏅? / 443(nginx) 闂傚倷绀侀幖顐も偓姘卞厴瀹曡瀵奸弶鎴犵暰婵炴挻鍩冮崑鎾绘煛鐏炵晫澧﹂柟绋匡攻瀵板嫮鈧綆鍘奸幆鍫ユ⒒?
//   2) 缂傚倸鍊搁崐鎼佸磹閽樺娴栭柕濞炬櫅缁犳煡鏌曡箛瀣偓鏇犵不濞戙垻鍙撻柛銉戝嫭宕冲銈呯箰瀹曨剟鍩ユ径鎰闁告剬鍛晨婵犳鍠楄彠闁告柨閰ｉ獮鎴﹀礋椤栨氨顓哄┑鐘诧工閹峰瀵奸崶褉鏀?sniroute.cmd up 闂傚倷鑳堕、濠傗枖濞戙垺鏅濋柕鍫濐槹閸嬧晠鏌ｅΟ铏癸紞妞も晠鏀遍妵鍕箳瀹ュ浂妲銈呭閻熲晠骞冪涵鍜佹▌闂佽　鍋撻柛妤冨剱閻?
// 闂傚倷鑳堕…鍫㈡崲閸儱绀夐柟杈剧畱绾惧潡鎮楅悽鍛婃珳闁绘帒锕鍫曞醇濮橆厽鐝旈悗瑙勬礃濞茬喖寮诲☉姗嗘僵妞ゆ帒瀚烽埀顒侇殜閺屻倝寮堕幋顖濆惈濡ょ姷鍋為幑鍥嵁閸ヮ剦鏁婇弶鐐村妞规娊姊绘担鐟邦嚋缂佸鍨块幆灞剧瑹閳ь剟銆佸▎鎾崇劦妞ゆ帒瀚悡鐔搞亜椤愵偄澧┑鈥茬矙閺屾盯鍩￠崒婊勫垱闂佽鍨伴惌鍌涗繆閻戣棄鐓涢柍褜鍓氱粩?闂佽姘﹂～澶愭偤閺囩姳鐒婃繛鍡樺灥閸ㄦ繈鏌ㄩ悢鍝勑㈢紒鈧崱娑欑厱闁靛鍨电€氼噣藝閵夆晜鈷戦柛婵嗗婢ч亶鏌涢幘瀵告噧闁?DNS 闂傚倷绀侀幖顐﹀磹娴犲缍栧鑸靛姂閳?闂?
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
        // up 婵犵數鍋炲娆撳触鐎ｎ喗鍤屽Δ锝呭暙閸屻劍绻涢幋娆忕仼缁?闂備浇宕垫慨宥夊炊閵婏附娈告繝鐢靛Л閸嬫捇鏌涢妷顔煎缂佲偓婢舵劖鐓冮柕澶堝劚閺嗛亶鏌￠崨顔炬噰闁哄本绋撻埀顒婄秵閸嬪绔熷Ο濂藉綊鎮╅懠顒佹喖闂佽鍨卞Λ鍐蓟閵娧€鍋撻敐鍛粵闁抽攱娲熷娲传閸曨噮娼堕梺绋挎唉瀹曢潧危閹邦兘鏀介柛顐犲灩瀹曘儱顪冮妶鍡欏闁肩懓澧界槐鐐哄箻閺傘儲顫嶉梺瑙勫劤绾绢厽鐗庢繝纰樺墲瑜板啴骞楀鍛灊闁冲搫鍟犻崑鎾绘晲鎼存繄鐩庨梺缁樻尰缁诲牓寮诲☉婊呯杸閻庯綆浜滈～鍥⒑缁嬪潡顎楁い鏇ㄥ幗缁傚秹宕ㄦ繝鍐Ф闂佸疇妗ㄧ粈浣哄閸ф鈷戦梻鍫熺⊕椤ユ粓鏌涙繝浣虹煓闁诡喖娼″畷鍗炩枎閹邦剛褰撮梻渚€娼х换鍡涘礈濠靛洨顩锋俊顖欒閻?30 缂傚倸鍊风粈渚€藝閹殿喗鏆滄い蹇撴椤?
        // 婵犵數鍋涢顓熸叏鐎靛摜鐭撶€规洖娲ㄧ粻鏂款熆鐠哄搫顨欓柣鎺戯躬瀵爼宕煎顓熺彅閻庤娲樺ú鐔煎箖濡法鐤€闁规儳鐤囬崺鐐烘⒑閸濄儱鏆遍柛瀣崌閺岋綁鎮╅棃娑樺闂佹悶鍔岀壕顓犲垝閳哄懎绾ч柟瀛樻⒐閻?up闂傚倷鐒︾€笛呯矙閹达附鍋嬪┑鐘插亰閼板潡鏌涢妷顔煎缂佲偓婢舵劖鐓冮柕澶堝劚閺嗛亶鎮规担鍦弨闁哄被鍊楅埀顒€婀辨慨鐢杆夋径鎰厱闁靛ě鍐炬毉缂備浇妗ㄧ划娆忕暦閹扮増鍋嗗ù锝囶攰閺呇冣攽閻愭潙鐏﹂柟鍛婃倐閺佸啴鏁愰崶鈺佸触闂佸搫顦伴崹鐔煎几鎼淬劍鐓ラ柣鏂挎惈瀛濆┑鐐叉噷閸ㄤ粙寮诲☉姗嗘僵闁绘挸娴锋禒顓熺節閳封偓瀹ュ洨鏆ら悗娈垮櫘閸撶喖銆佸鈧俊姝岊槾妞わ富鍋婂?
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
        // up 闂傚倷鐒﹂惇褰掑礉瀹€鈧埀顒佸嚬閸欏啴銆佸Δ鈧…銊╁川椤旇姤顔囬梻浣告啞缁哄潡宕曢崘娴嬫灃闁靛繈鍊栭悡娑橆熆鐠洪缚瀚伴柡瀣閹便劌螣缁嬪灝顬夌紓浣稿€圭敮锟犲极閹版澘閱囨繛鎴炲笒娴滈箖鏌涢幇闈涙灈閻庢艾顦遍埀顒€绠嶉崕閬嶅箠鎼搭煉缍栭柕鍫濇礌閸嬫捇宕烽褏鍔搁悗娈垮枦閸╂牠宕氭繝鍥ㄦ優閻熸瑥瀚惃顐︽⒑閸涘﹤鐏熼柛濠冾殜閻涱噣濮€閵忋垻锛滈梺鍝勫暞閸ㄧ敻鎮橀悩鍨枑闁绘鐗忓ú瀛橆殽閻愭潙娴┑顔瑰亾闂佹寧绋戠€氥劑宕戦悙鐑樷拺闁告稑锕﹂幊鎰瑰鍛槐鐎殿噮鍋婃俊鑸靛緞婵犲嫰鐛撻梻渚€娼чˇ顓㈠磿闁秶宓侀柟杈剧畱缁犲綊鎮楀☉娅虫垹绮氱捄銊х＜閺夊牄鍔庣粻鏍磼鐎ｎ亶妲告い鎾炽偢瀹曞ジ顢曢敐鍥╁幀闂傚倷绶氬鑽ゆ嫻閻旂厧绀夌€光偓閸曨剙浠奸梺缁橆焽缁垶鎮￠崒婧惧亾楠炲灝鍔氶柟鍐茬箻瀹?
        // 闂傚倷绀侀幖顐︽偋閸℃瑧鐭撻悗娑櫳戦崣蹇涙煟閺傚灝鎮戦柛搴＄Ч閺屾盯寮撮妸銉ョ閻庤娲栭悥濂稿蓟濞戞ǚ鏋庢繛鍡樺灩琚﹂梻渚€娼荤紞渚€藟閹捐绠柣妯绘た閺佸秵鎱ㄥ鍡楀濞寸厧鐗撳?濠电姷鏁搁崑娑㈡儑閺屻儱围闁糕剝顨婂Λ鐑芥⒒?闂?
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


// WARP 闂傚倷绀侀幖顐も偓姘卞厴瀹曡瀵奸弶鎴犵暰婵炴挻鍩冮崑鎾绘煙椤斿灝袚婵炵厧绻橀崺锟犲磼濞戞瑯鍚傞梻鍌欑濠€閬嵥夐幇顑锯偓鎺楀捶椤撴稑浜鹃梻鍫熺〒閻掑摜鈧娲樼换鍫熶繆閹间礁唯闁挎洍鍋撻柛鎾愁煼濮婂搫煤缂佹ê鈻忛梺鍛婃煥閿曨亪宕洪埀顒併亜閹哄棗浜鹃梺绋匡工缂嶅﹪骞冮妷鈺傛櫜闁搞儯鍔岄娑㈡⒑缂佹﹩娈旈柣妤€锕幃妤咁敊濞?data\.warp-enabled闂?
//   闂備浇顕х€涒晝绮欓幒妤佹櫔婵犵數濮崑?  -> 闂傚倷鐒﹀鍨焽閸ф绀夌€广儱顦弰銉︾箾閹寸偟顣查柡鍡樼矒閹﹢鎮欓棃娑楀缂?WARP 闂佽瀛╅鏍窗閹烘纾婚柟鍓х帛閻撶喐銇勯幇顔兼瀻濞存粓绠栧娲箰鎼达絺妲堥梺缁橆殕閹瑰洤鐣烽幎钘夌濞达絾鐡曡婵＄偑鍊栭悧妤冨枈瀹ュ纾归梻鍫熻儐鎼淬劌鐐婇柕濞у懐鏆梺璇查閻忔艾螞閸愵喖鏋?
//   婵犵數鍋為崹鍫曞箰閸濄儳鐭撻柟缁㈠枟閸嬨倝鏌曟繛鐐珔闁?-> 婵犳鍠楃敮妤冪矙閹烘せ鈧箓宕奸妷顔芥櫍婵犵數濮甸懝鍓х矆閸岀偞鐓欓柟顖涙緲琚氶梺鎸庣⊕濡啴寮婚妸銉㈡婵☆垳鍘ч。瑙勪繆閵堝洤孝婵炲樊鍘奸悾鐑筋敃閿濆洨鎳濋梺閫炲苯澧伴柛鎺撳浮椤㈡稑顭ㄩ崟顐偓娑㈡⒑闂堟稓澧曟繛灞傚€楅悷褔姊绘担绛嬪殐闁稿繑绋撳▎銏ゅΧ閸ヮ煈娼熼梻渚囧墮缁夊鎲撮敂濮愪簻闁哄秲鍔忔竟妯兼偖閻斿吋鈷戦柛婵嗗閺嗙偤鏌涙惔銈呬汗闁逞屽墯閸濆酣宕归挊澶屾殾闁挎繂顦粻锝夋煟濡灝鐨洪柣銈傚亾 WARP 闂備礁鎼ˇ顐﹀疾濠婂牃鈧箓宕奸姀鈥冲簥濠电偞鍨崹娲磿閹剧粯鐓冪憸婊堝礈濞戞瑧顩查柛娑橈攻缂嶅洭鏌曟繝蹇擃洭闁告梻鍏橀幃妤€鈻撻崹顔界亐闂佺顑嗛幑鍥蓟?
// 闂傚倷鐒﹀鍨焽閸ф绀夌€广儱顦伴崑澶愭煥濠靛棙澶勫┑顖氥偢閺岋絽螖閳ь剟鎮ф繝鍥ㄥ€垫い鏃€绁硅ぐ鎺撳亹闁告劖褰冩慨鎼佹⒑闁偛鑻晶顔尖攽閸屾稒鈷掔紒顕呭幗瀵板嫰骞囬鈧宄邦渻閵堝棛澧紒璇茬Ч瀹?JSON闂傚倷鐒︾€笛呯矙閹达箑瀚夋い鎺嗗亾閸楅亶鏌涘☉姗堝姛闁崇粯妫冨鍫曟倷閺夋埈妫嗛梺鍝ュ枎閿曨亪寮?.autostart-off 婵犵數鍎戠徊钘壝洪敂鐐床闁告劦浜栭崑鎾诲垂椤愶綆妫冮悗瑙勬磸閸庤櫕绂掗敃鍌涘殐闁冲搫锕ｇ槐妯肩磽閸屾瑧顦︽い锔诲灣缁梻鈧稒顭囬々閿嬬箾閹寸們姘跺几閺嶎厽鐓涢柛銉ｅ墸濮樿泛纾?
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

// 闂傚倷绀侀幉锟犲礄瑜版帒鍨傞柣妤€鐗婇崣蹇涙煃閸濆嫭鍣圭紒顐㈢Ч閺屻倝骞栨担瑙勯敪闂佹娊鏀卞Λ鍐箖?婵犳鍠楃敮妤冪矙閹烘せ鈧箓宕奸妷顔芥櫍婵犵數濮甸懝鍓х矆閸岀偞鐓欓柟顖涙緲琚氶梺?闂傚倷鐒︾€笛呯矙閹烘埈娼╅柕濞炬櫅閺嬩線鏌曢崼婵愭Ц缂佺媴缍佸娲垂椤曞懎鍓冲┑顔硷躬缁犳牠寮诲☉姗嗘僵闁绘挸娴锋禒顖炴⒑鏉炰即妾烽柛濠冪墪椤洦绻濆顒傚€為梺闈涱槶閸ㄨ绂嶉幆顬″綊鏁愭径妯活棖缂?WARP 闂備礁鎼ˇ顐﹀疾濠婂牃鈧箓宕奸姀鈥冲簥濠电偞鍨崹娲磿閹剧粯鐓?-> 闂傚倷绀侀幖顐﹀磻閸℃稑鍌ㄩ柤娴嬫櫆椤洟鏌熸潏鍓х暠鏉?
// 闂傚倷绀侀幉锟犳偡椤栨稓顩叉繝濠傚枦閼版寧銇勮箛鎾跺閻熸瑱闄勯妵鍕冀閵娿劌顥濋悶姘卞枛濮婃椽宕崟顐ｆ濠电偛顦扮粙鎺旂矉閹烘垟妲堟繛鍡樺灍閺嬫牠姊洪柅鐐茶嫰婢ь噣鏌嶇拠鏌ュ弰闁糕斂鍎靛畷銊╊敍濠婂孩啸婵犵數鍋為崹鍫曞箰閸濄儳鐭撻柣銏㈩焾濡ê霉閿濆牜娼愬ù婧垮€栫换娑㈠箣閻戝棛鍔烽梺鍝勬閸楁娊寮婚敓鐘茬妞ゆ挾鍠撻妴鎰攽閻愬弶鍣洪悽顖ょ節瀵偄顓奸崨顖涙畷闂佸憡娲︽禍鐐垫濮樿埖鈷戠紒瀣硶缁犳壆绱掓径瀣唉妤犵偛绻戠€佃偐鈧稒锚娴犳椽姊洪棃娑辩叚缂佺姵鍨块幃锟犲即閵忥紕鍘甸梻鍌氱墛娓氭宕曡箛鎾斀妞ゆ棁顫夊▍鍡樼箾閻撳寒鐓奸柟顔界懇瀵爼骞嬪┑鎰╅梻鍌欒兌閹虫捇鎮樺璺虹濠电姵纰嶉崐鍫曟煥閺囩偛鈧瓕绻?
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

// ---------------------------------------------------------------- WARP 闂傚倷鑳堕…鍫ヮ敄閸愵喖纾块梺顒€绉寸粻鏍偓骞垮劚椤︻垰顔忓┑鍡忔斀闁绘ê寮舵刊濂告煕?
// 婵犵數鍋為崹鍫曞箰閸洖纾规俊銈呮嫅缂?WARP 婵犵數鍋為崹鍫曞箰閸濄儳鐭撻柣鎴ｆ缁€鍌涙叏濡炶浜鹃梺?闂?
//   1) 闂傚倷鑳剁划顖炩€﹂崼銉ユ槬闁哄稁鍘奸悞鍨亜閹寸偛顕滄い锕傤棑缁?connected 闂傚倷绀侀幉锟犳偡閿曚焦鎳岄梻?disconnected      -> 缂傚倸鍊烽悞锕傚蓟閵娾晩鏁勯柛娑欐綑閺勩儵鏌曡箛瀣偓鏍倿缂佹ɑ鍙忔俊顖氥仒閸氼偊鏌涘▎灞戒壕
//   2) 闂傚倷鑳剁划顖炩€﹂崼銉ユ槬闁哄稁鍘奸悞鍨亜閹寸偛顕滅紒浣哄閵?connected 婵犵數鍋犻幓顏嗗緤閽樺）娑樜旈崨顓炴優濡炪倖鎸堕崹褰掓儗濡ゅ懏鐓ユ繝闈涙椤ユ粓鎮硅閻°劎鎹㈠☉銏犵骇闁规惌鍘奸崜顔剧磽娴ｈ娈旀い锔炬暬瀵宕ㄩ褎鍍甸梺鍛婃寙閸滀焦袩婵犵數鍋為崹鍫曞箰閸洖鍨傞柛锔诲幐閸嬫捇宕归鍡樺灴閵?缂傚倸鍊烽悞锕€螞韫囨稑鍨傞柟鎯版绾惧鏌ㄥ┑鍡樺闁稿鐗楅妵鍕箛閸洘顎嶅銈呭閻熲晠寮?
//      -> 闂備礁鎼ˇ顐﹀疾濠靛纾婚柣鏂垮悑閸嬨劎绱撴担濮戭亞娆㈤悙鍝勭骇闁割偒鍋勬禍顖滄偖?2 濠电姷鏁搁崑娑樜涘畝鍕婵犻潧娲﹀▍鐘绘煟濡偐甯涢柣鎺旂帛閹便劌螣婢剁鎯堥梺鍛婄憿閸嬫捇姊绘担鐟邦嚋缂佸鍨剁缓浠嬪籍閸屾粎鐣?10 闂傚倷绀侀幉锛勬暜閹烘嚦娑樷攽鐎ｎ亞顔囬梺瑙勫礃椤曆呯矆閸愵喗鐓曟繛鎴濆船閺嬬喎霉閻樿秮顏堚€﹂崸妤佸殝闁汇垻鏁搁崣婵堢磽娴ｈ鈷愭繛鍏肩懇閸┿垺鎯旈妸銉у弨闂佸憡渚楅崹鎶剿囬鈧缁樼瑹閸パ冾潻闂佽绻戠换鍫濈暦閵忋倕围濠㈣泛锕﹂ˇ顐︽倵閻熸澘顏鐟版椤?
// 闂傚倷鐒﹀鍨焽閸ф绀夌€广儱顦弰銉︾箾閹寸偟鎳呴柍缁樻閹綊宕堕妸銉хシ閻炴氨鍠栧娲传閸曨偂绨介梺鍛婎焾閸嬫劕危閹版澘绠ｉ柨鏇楀亾缂?g_warpManualStop=true闂傚倷鐒︾€笛呯矙閹寸偟闄勯柡鍐ㄥ€荤粻鏂款熆閼搁潧濮囩紒鐘劦閺岀喖鎮欓浣虹▏濡炪們鍎辩粔褰掑蓟?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊风欢锟犲磻閸涱喚鈹嶉柧蹇氼潐瀹?WARP 闂傚倷绀侀幉锟犳偡閿曞倹鍋嬮柡鍐ㄥ€婚崡姘舵倵濞戞瑯鐒介柍?204 缂傚倸鍊烽悞锕€螞韫囨稑鍨傞柟鎯版绾惧鏌ㄥ┑鍡╂Ц鏉╂繈姊虹粙鎸庢拱缂佽瀚划濠氬蓟閵夛妇鍘?true 闂備浇宕甸崑鐐电矙韫囨稑绀夐幖娣妼妗呭┑顔筋焾濞夋稓绮堥崟顖涚厱婵犻潧妫楅鈺冪磽瀹ュ棙宕岄柡灞剧☉閳诲骸螣閾忛€涘寲闂備浇妗ㄩ梽宥夊磹濠靛洣绻?
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
        if (st == 0) {                     // 濠电姷鏁搁崑娑㈡儑閺屻儱围閹兼番鍩勫Λ鍛存⒑鐠囨煡顎楃紒鐘茬Ч瀹曠懓煤椤忓嫭妲┑鐐村灦濮樸劑宕″顓滀簻闁哄秲鍎遍埀顒侇殘缁牊寰勬繛鎺戠秺閹晠顢曢～顓烆棜婵犵數鍋為崹鍫曞蓟閵娾晜鍋嬮柟鎹愵嚙缁€鍌涗繆椤栨繂鍚归柍缁樻⒒閳ь剙绠嶉崕閬嶅箠瀹ュ洤鍨?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倷绀侀幖顐﹀磻閸℃稑鍌ㄩ柤娴嬫櫆椤?
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
constexpr wchar_t kAppVersion[] = L"3.16.0";

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
    // 闂傚倷绀侀崥瀣偓绗涘懎鍨濋悘鐐垫櫕閺嗭箓鏌ｉ弮鍌氬付缂佺姵婢橀…鍧楁嚋闂堟稑袝濠电偛鐡ㄥú鐔煎箖濡も偓閳藉鈻庡Ο鑽ゆ殾闂備椒绱梽鍕崲閸儳宓侀柟鐑橆殔缁犳娊鏌熺€电啸妞わ絾鐓″娲箰鎼达絺濮囩紒鐐緲缁夌懓顕ｉ幎鑺ュ亜闁绘挸娴烽澶婎渻閵堝棙顥嗗┑顔哄€濋幃妤咁敊濞村骸缍婇幃鈺呮倻濡鈧劙鎮峰鍛暭闁绘搫绻濋獮濠偽旈崨顔兼疅闂侀潧顧€缁犳挸顬婇悜鑺モ拺闁绘挸瀵掑▓鐘裁瑰鍕畵閾荤偞銇勯幇鍫曟闁稿顑夐弻娑㈩敃閵堝懏鐎鹃梺鍦厴娴滃爼寮婚妸銉㈡婵☆垳鍘ч·鈧繝鐢靛Л閸嬫挻銇勯幘鍗炵仼缂佺姵濞婇弻鐔兼倻濡櫣浠肩紒鐐緲缁夊綊寮婚妸銉㈡婵妫欓鍡欑磽娴ｄ粙鍝虹紒璇插暙椤洨鎷犲ù瀣€婚梺瑙勫礃濞呮洟顢旈悙鐑樷拺闁告稑锕ゆ慨鍥╃磼婢跺﹦锛嶉柟骞垮灩椤繃娼忛妸銉у酱濠电偠鎻紞渚€藟閹炬椿鏁傞弶鍫涘妿缁犻箖鏌涢悩顐㈠⒉濠㈢懓顑夊畷?
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
        // WARP 闂備浇顕х€涒晝绮欓幒妤€绀夋俊顖濓骏娴滃湱鈧箍鍎遍ˇ顖滄兜閳ь剟姊洪崫鍕ォ濞存粎鍋熺划锝呂旈崘顏嗭紳?Sniroute 闂傚倷绀侀幉鈥愁潖缂佹ɑ鍙忕€规洖娲ㄩ崡姘舵煙缂併垹鏋涚紒鐙欏洦鐓冮柕澶涚畱婢ь垶鏌￠崱妯荤凡妞ゎ厼娼￠幊婊堟偨闂堟稈鍚傞梻浣规偠閸娿倝宕ｉ崘銊ф殾闁挎繂顦伴弲鏌ョ叓閸ャ劍绀堟い锔衡偓鈺玱gle/YouTube/Play 缂傚倸鍊搁崐鎼佸疮椤栫偛鍨傞柛褎顨呰繚婵犮垼鍩栭崝鏍磻鐎ｎ喗鐓曟い鎰靛亜婢ф壆绱掗埀顒勫幢濞戞瑧鍘?
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

    // 闂傚倷绀侀幖顐︽偋閸愵喖纾婚柟鎹愵嚙缁犲綊鎮楀☉娅虫垹绮堢€ｎ喗鐓曢幖娣灪鐏忥箓鏌熼姘殻鐎规洜鍠栭、姗€鎮滈崱妯荤様闂傚倷绀侀幖顐λ囬鐐茬柈闁硅泛顫曢埀顑跨窔楠炲鏁傞懞銉︾彨婵犳鍠楅敋鐟滄澘顦卞Σ鎰旈崨顔惧幘缂佺虎鍘奸幊搴ｆ閿斿墽纾兼俊銈傚亾妞わ箓娼ч悾鐑芥晲婢舵ɑ鏅┑鐘诧工閸熲晠宕犻弽顓熲拺闁圭娴烽埥澶愭煠濞茶鐏ラ柕鍥ㄥ姌椤﹀綊鏌熼鑽ょ煓濠碘剝鎮傞弫鍌涚鐎ｎ偒鈧牠姊绘担鍛婃儓缂佸娼欑叅闁挎洍鍋撻崡閬嶆煙鏉堝墽鐣辩紒鈧崟顖涚厱婵犻潧妫楅瀛樹繆閺屻儰鎲鹃柡灞剧☉椤劑宕橀鍡曠礃濠电姭鎷冮崟鍨杹閻庢鍠氶弫濠氥€侀弽顓熺叆妞ゆ牗鐭竟?
    // 濠电姷鏁告慨宥夊礋椤愩埄娼曢梻浣虹帛閻楁鍒掑鍥╃煔闁告鍋愰弨浠嬫煕閵夋垵鎳忓暩缂?ASCII闂傚倷鐒︾€笛呯矙閹达箑瀚夋い鎺嶇劍濞呯姷鈧箍鍎卞Λ娑€?U+00A9(婵? 闂?U+00B7(闂? 闂傚倷鐒﹀鍨焽閸ф绀夐煫鍥ㄦ媼瑜版帒骞㈡俊顖氱毞閺嬫瑦绻濋姀锝嗙【闁挎洏鍊濆鏌ュ箹娴ｅ湱鍘?
    {
        // 闂傚倷鑳剁划顖炪€冮崨瀛樺亱濠电姴鍋嗛悞?+ 闂備浇宕垫慨鎶芥嚄閸洍鈧箓宕奸敐鍐ф睏闂佸搫琚崕杈╃矆閸℃稒鐓曢柍鈺佸枤濞堟鐥幑鎰靛殭闁宠棄顦甸獮姗€宕￠悙鈺佷壕婵炴埈娼块埀顑跨窔楠炲鏁傞懞銉︾彨婵犳鍠楅敃鈺呭磻閸℃侗鏁傞梺顒€绉甸崑鐘崇箾閹寸儐鐒鹃悗姘嵆閺屾洟宕惰闊剚鎱ㄦ繝鍜佹綈鐎垫澘瀚板畷顐﹀Ψ瑜忓畷鍫曟⒑閼姐倕浠﹂柛鎴濈秺瀹曟垶寰勬惔娑楁睏閻庡箍鍎遍ˇ顖滄兜閳ь剟姊洪崫鍕窛闁哥姴閰ｉ幃鐐寸節濮橆厾鍘搁梺绋胯閸婃洖顕ｇ捄銊х＜閺夊牄鍔庣粻鐐烘煙閾忣偅灏靛ù鐙呯畵閹虫牠鍩￠崘鈺佹"All rights reserved"
        // 闂傚倷绀侀幉锟犮€冮崱妞曞搫顭ㄩ崨顏勪壕婵鍘у顔筋殽閻愬樊鍎旀鐐叉喘椤㈡盯鏁愰崱妯绘緫婵犵數鍋為崹鍫曞箰閸濄儳鐭撻柟缁㈠櫘閺佸﹪鏌熼悧鍫熺凡闁告瑥锕弻鐔煎箲閹伴潧娈愬┑鐐靛帶閻栧ジ骞冭ぐ鎺戠倞閻犻缚娅ｆ禒鏉戭渻閵堝棙纾婚柛妤佸▕閻?闂備礁鎼ˇ顐﹀疾濠婂牊鍋￠柕鍫濐槹閻撳倹绻濇繝鍌氼仴濞存粍绮撻弻娑㈡晜鐠囨彃绠虹紓浣瑰姈椤ㄥ懘鍩ユ径鎰鐎规洖娉﹂敐鍡曠箚闁肩⒈鍓﹂悞鐐亜閺囶亞绉柛鈹惧亾濡炪倖宸婚崑鎾绘煃閻熸澘鏆ｆ鐐叉喘椤㈡瑩鎮剧仦鐐啅"闂?
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
            // 闂備浇顕уù鐑藉极閹间礁鍌ㄧ憸鏂跨暦閻㈠壊鏁囬柣鏃囥€€閺嬫牠鎮楅獮鍨姎閻庢凹鍠氶幏褰掑煛閸涱喖浠悷婊冮叄閹囧即閵忊晜鏅ｉ悷婊呭鐢帞绮婚敐鍜佹富閻庯綆浜滈惁銊ッ归悡搴♀偓鍧楀蓟閵娿儮妲堟繛鍡樏崜绫怚 婵犵數鍋炲娆撳触鐎ｎ偆鈹嶆繛宸簻閸氳绻濇繝鍌氭灓闁哥喎鎳樺鍫曞醇濞戞ê顬夐梺鍝勬閻楁捇寮?WARP 闂備浇宕垫慨鐢稿礉瑜忕划濠氬箣閿濆啩姹楅梺绉嗗嫷娈樼€规挷绶氶幃褰掑传閸曨剚鍎撻梺?YAML闂?
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
        // winget 闂?闂佽娴烽幊鎾诲箟闄囬妵鎰板礃椤旇棄浠奸梺闈浤涢崟顐ｆ珴闂備礁鎲℃笟妤呭礈濞戙垺鍋℃い鎺戝閻撴盯鏌涢幇鍏哥盎閺嶁€斥攽閻愭潙绲绘繛灏栤偓鎰佸殨?闂備礁鎼ˇ顐﹀疾濠婂牆钃熼柕濞垮剭濞差亜鍐€妞ゆ挾鍣ュΛ?0闂?1978335189闂傚倷鐒︾€笛呯矙閹次层劑鍩€椤掑倻纾奸弶鍫涘妿缁犵偤鏌＄仦鑺ュ枠闁轰焦鍔欏畷鐔碱敇閻樺啿鏋撻梻鍌欑閹碱偆鈧氨鍏樺畷瑙勫閺夋嚦锕傛煙鐎电校閻庢艾顭烽弻鏇熷緞濞戞艾鐭濋梺?
        // 婵犵數鍋涢顓熸叏娴兼潙纾块梺顒€绋侀弫鍥╂喐閻楀牆绗掔紒鐙呯到椤啰鈧綆浜滈惁銊ッ归悡搴♀偓鍧楀蓟閿熺姴绀嬮梻鍫熺〒娴犫晠姊虹拠鈥崇仧缂佽埖宀搁獮鍡涘炊椤掍礁浜滈梺缁樺灦椤洭寮抽悩缁樷拺闁告稑锕ら悘杈ㄧ箾閸滃啰绉柣娑卞櫍瀹曞爼濡歌閺嬫牠姊洪崨濠勭畵閻庢凹鍓欓埢宥呪枎閹惧鍙嗗┑鐐村灦椤洭鎮為悜鑺ョ厓鐟滄粓宕滃▎鎰檮闁哄啫鍊荤粻鏂款熆閼搁潧濮囬柛娆忥攻閵囧嫰寮介妸銊ヮ棟濠电偞鍤崶銊у幗濡炪倕绻愮€氱兘宕曢弮鍫熺厓鐟滄粓宕楀鈧畷鎴﹀箻缂佹鍘遍梺纭呮彧缁查箖顢旈悩缁樼厽闁哄秲鍔庨惌鎺戔攽?
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
            // 闂傚倷绀侀幖顐﹀磻閸℃稑鍌ㄩ柤娴嬫櫆椤洟鏌熺€电浠ч柍缁樻⒒閳ь剙绠嶉崕閬嶅箠鎼粹檧妲堢憸鏃堝蓟?闂傚倷绀侀幉锟犲垂閻㈢绠规い鎰╁劘娴滆绻涢幋鐐冩岸寮婚敃鍌涚厓闁靛鍔岄惃娲煕?闂傚倷鐒︾€笛呯矙閹烘垹鏆嗛柟闂寸杩濋柟鍏肩暘閸斿瞼绮婚妷鈺傚€甸柨婵嗛閸樻挳鏌嶈閸撴俺鎽紓浣歌嫰瀹曨剟鍩為幋锕€鐐婇柕濠忕畱椤忎即姊?DoH/nginx 闂備礁鎼ˇ顐﹀疾濠婂牊鏅繝鐢靛Л閸嬫挻銇勮箛鎾跺闁哄拋鍓熼幃姗€鎮欓懜娈挎闂佺粯甯掗妶鎼佸箖鐟欏嫭濯撮柛娆忣槹閻濇牠姊烘潪浼存闁稿﹥绻堥悰顔锯偓锝庡枟閹层儵鏌涚仦鍓ф噮闂傚绉电换娑㈠箣閻愭潙鏆楅梺绋跨昂閸婃繂顕ｆ繝姘倞闁冲搫鍟伴敍鐐烘⒑閻熸壆鎽犻柣鐔村灲瀹?
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


// 濠电姷顣藉Σ鍛村磻閳ь剟鏌涚€ｎ偅宕岄柡宀嬬磿娴狅妇鎷犻幓鎺戭潥婵犵數鍋炶ぐ鍐焽閳ユ剚鍤曟い鎺戝€归弳婊勩亜閺傚灝鈷旈柣娑栧劦閺?sniroute.cmd update check闂傚倷鐒︾€笛呯矙閹达箑瀚夋い鎺嶆缁诲棝鏌涘☉姗堟敾缂傚秴娲弻鐔煎箚瑜嶉弳杈ㄦ叏閿濆拋妯€闁诡喖缍婇獮鍥敇閻旇櫣鈻忛梻浣虹《閺呮盯鎳熼鐑囩稏婵犻潧顑呯猾宥夋煃瑜滈崜鐔风暦瀹勬噴娲敂閸涱垰甯撻梺璇茬箳閸嬬喖宕曢悽绋跨；?
// 闂傚倸鍊搁崐绋棵洪悩璇茬；闁瑰墽绮崑锟犳煛閸ャ劎顣茬紒浣哄閵囧嫰寮埀顒勬偋閻樺樊娼?GitHub API闂傚倷鐒︾€笛呯矙閹达附鍋嬮煫鍥ㄥ喕缂嶆牠鏌涢埄鍐槈閻庢艾顦…璺ㄦ崉娓氼垱楔濠电偛顦崹浠嬬嵁閺嶎偀鍋撳☉娅虫垵鐣烽崟顑句簻闁冲搫顑呴々顒勬煙瀹勯偊鍎斿┑鈥崇埣瀹曞ジ濮€閳藉棙缍岄梻鍌欑劍鐎笛呯矙閹次层劑鍩€椤掑倻纾奸弶鍫涘妿缁犵偞顨ラ悙鑼ч柛鈹惧亾濡炪倖甯婇懗鍫曘€呴悜鑺ョ厪濠㈣埖鐩顕€鏌℃径濠勭Ш闁哄本绋戦～婵嬵敆閸曠鍋撳Δ鍐╁枑闁哄顑欓崵鐔兼煟閵夘喕绨介柟鐟板婵℃悂濮€閿涘嫬骞橀梻鍌欑劍鐎笛呯矙閹寸偟闄勯柡鍐ㄥ€荤粻鏂款熆閼搁潧濮堥柛銈呭閺岋箑螣娓氼垱笑濡ょ姷鍋戦崹褰掓箒闂佹寧绻傞幊鎰不閹绘帩鐔嗙憸婊堝矗閸愵喖绠氶柛鏇ㄥ€犻悢鍝勫闁哄洨鍋樻竟?
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
        // 濠电姵顔栭崰妤勬懌濠殿喖锕ょ紞濠傜暦瀹勬噴娲敂閸曨厼鎽嬮梻浣告啞閻燂綁宕曢幎鑺ュ仭婵犻潧妫屽Σ鍫ユ煙閻愵剙澧柛搴＄箲閵囧嫰鏁冩担鍦€婇梺璇″枟濠㈡﹢鎮惧┑鍫氬亾閿濆骸澧伴柣娑栧劦濮婃椽宕ㄦ繝鍕櫧婵炲瓨绮嶉悷褔骞戦姀鐙€娼╅柤鎼佹涧娴犵厧鈹戦悙鍙夘棞缂佺粯鍔欒矾闁告侗鍘稿Σ鍫ユ煙閹冭埞闁逞屽墯閻╊垶濡存笟鈧獮瀣晝閳ь剛鈧艾顦…璺ㄦ崉娓氼垰鍓卞┑鐐叉噹缁绘﹢寮诲☉銏犵疀妞ゆ帊绀佸▍锝咁渻閵堝棙鈷愰柛鏃€鐟︽穱?
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


// ---------------------------------------------------------------- GHA 濞戞挸顑堝ù鍥┾偓鐢殿攰閻﹁棄顩?
// 闁哄鑳堕悾婵囨綇閹惧啿寮虫俊妤€妫寸槐鎵磽閺嶎剛甯嗘俊?+ 缁绢収鍠栭悾?闁告瑦鐗楃粔鐑芥晬瀹€鈧崙鑺ョ閿濆洨鍨抽柛鎺撶☉缂傛捇鏁嶉崼婊呯憹闂傚洠鍋撻悷?.rc 閻犙冨缁噣鏁嶆径鍫氬亾?
// Enter = 缁绢収鍠栭悾楣冩晬鐎涘炒c = 闁告瑦鐗楃粔鐑藉Υ?
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

// 闁告艾楠歌ぐ瀵告崉?gha fetch闁挎稑鏈俊鎼佸嫉閳ь剟宕ユ惔婵堫伇閻炴稑鐭佺欢顓㈠礄妤﹀灝鍨遍柛鎺斿閻ㄩ潧鈻旀幊閳?
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
    // WARP 闂傚倷鑳堕…鍫ヮ敄閸愵喖纾块梺顒€绉寸粻鏍偓骞垮劚閹叉ê鈽夐姀鐘靛姦濡炪倖甯掔€氼剟鎷戦悢鍏肩厪濠㈣埖锚閻忥絿绱?0 缂傚倸鍊风粈渚€藝閹殿喗鏆滃┑鐘插閸楁岸鏌℃径瀣亶闁哄閰ｉ弻宥夋寠婢跺娅х紓浣插亾濠㈣泛顭悢鍡涙煙椤栨稑顥嬬紒銊ユ健閹嘲鈻庡▎鎾搭€嶇紓浣割儏椤﹀灚淇婇崼鏇炲耿闁宠桨妞掓潻姗€姊绘担鍝ョШ婵炰匠鍛煋鐟滅増甯掗悞鍨亜閹哄秶鍔嶉柣銊﹀灴閺岋綁骞欓崟顓犵槇婵?
    // 闂傚倷绀侀幉锟犲礄瑜版帒鍨傞柣妤€鐗婇崣蹇涙煃閸濆嫭鍣圭紒顐㈢Ч閹綊宕崟顒佸創闁汇埄鍨崕鐢稿蓟閻旂儤宕夐柣姘綑娴兼劗绱撻崒姘毙㈡い鎴濐槸閻ｅ嘲顭ㄩ崘鎯ф倯闂佹悶鍎洪悘婵嬫晝閸屾稓鍘靛銈嗗灦鐎笛呯矈閻楀牄浜滈柡鍌涘婢跺嫮绱掔€ｎ亶妲洪柍褜鍓ㄧ紞鍡涘磻閸涘瓨鈷旂€广儱顦伴悡銉︾箾閹寸偟鎳嗗瑙勶耿閺岀喖顢涘▎鎴犵崲閻?闂傚倷绀侀幉锟犲礄瑜版帒鍨傞柣妤€鐗婇崣蹇涙煃閸濆嫭鍣圭紒顐㈢Ч閹綊宕崟顒佸創闂佹悶鍊栫敮鈥愁嚕閸洖鐓涢柛鎰╁妺濞岊亪鏌ｉ姀鈺佺仭闁规瓕娅曟穱濠勨偓娑欘焽閻熷綊鏌嶈閸撴瑩鍩㈤幘璇差潊闁靛牆鎳撹闂佺懓鍚嬮悾顏堝垂濞差亝鍊块柣锝呭閸嬫挸鈻撻崹顔界亐闂?闂備礁鎼ˇ顐﹀疾濠婂牊鍋＄憸鏃堟晲閻愬搫绠抽柟鐐綑椤€愁渻閵堝棗绗傜紒鈧担瑙勫劅濠电姴浼呰ぐ鎺戠闂傚倻绻濈€涒晛顪冮妶搴′壕缂佺姵鍨块垾锔锯偓鍦Х閻も偓闂佸搫鍊藉▔鏇㈩敂?
    // 闂傚倷鐒︾€笛呯矙閹达附鍋嬮煫鍥ㄤ緱閺佸嫰鏌涢妷锝呭闁崇粯妫冮弻宥堫檨闁告挻鐟╅崺銏℃償閵娿儳鍔撮梺鍛婂姉閸嬫捇鍩涢弴銏♀拺闁告繂瀚～锕傛煕鎼粹€虫毐闁挎洏鍨藉畷銊╁级閹寸媭妲撮梻浣筋潐椤旀牠宕板Δ鍛仼闂侇剙绉甸悡娑㈡煕閺囥垺娑ч柣蹇ョ節閺岋紕浠﹂懞銉︽倷濡炪倖娲╃紞渚€宕洪埀顒併亜閹烘垵顏柡鍜佸墰閳ь剙鍘滈崑鎾绘煕閹板墎鍒板ù婊堢畺閹﹢鎮欑捄杞版睏闂佽崵鍠愰崝鏇㈠煡婢舵劕绠绘い鏍ㄧ煯婢规洘绻濋悽闈涗粶婵☆偄顕Σ鎰板即閻旂寮块梺褰掓？缁€浣虹不閹存緷褰掓晲閸ャ劍鐝紓浣哄Х閸忔﹢寮婚敓鐘茬闂傚牊绋撴禒顓炩攽閻橆偄浜惧銈嗘尪閸ㄦ椽宕曞Δ鍛厪濠㈣埖锚閻忥絿绱掗埀顒佸緞閹邦厾鍘遍梺鐟版惈椤戝懘鍩€椤掍焦绀嬮柟顕€绠栭幃娆擃敆閸屾鐏冮柣搴＄畭閸庨亶骞婃惔鈱?婵犵數鍋涢顓熸叏閺夋埈娼╅柨鏃傚亾瀹曞弶绻涢幋娆忕仼缂佺姵濞婇弻鏇熷緞閸繂濮庨梺缁樻尭閸婂潡寮婚敓鐘茬＜闁靛牆鎳庣粻鍦磽?闂傚倷鐒︾€笛呯矙閹次诲洭顢橀姀鐘靛姦?
    // 濠电姵顔栭崳顖滃緤閻ｅ本宕查柟杈惧瘜閺佸啫鈹戦崒婊庣劸閻熸瑱闄勯妵鍕冀閵娿劌顥濋悶姘卞枛濮婄粯绗熼崶褎鐏堥梺閫炲苯澧伴柛鐔峰瀵板嫮浠﹂幆褏锛忛梻浣烘嚀閻°劎鍒掓惔顭戞晜閺夊牄鍔庣粻?up闂傚倷鐒︾€笛呯矙閹寸偟闄勯柡鍐ㄥ€哥欢?*闂佽娴烽崑锝夊磹閺嶎厼纾块柤娴嬫櫆瀹?12 缂?*闂?
    // first-run-setup 闂傚倷鑳堕崢褔銆冩惔銏㈩洸婵犲﹤鎳忓畷鎻捗归敐鍛础闁崇粯娲熼弻锝夊棘閸噮鏆㈢紓浣瑰姈椤ㄥ﹪寮婚悢椋庢殝闂侇叏闄勬瓏闂備礁婀卞▍銏㈢礊娴ｅ壊鍤曢梻鍫熶緱閸氬鏌涘☉鍗炲季婵炲吋濞婂娲箰鎼达絺妲堥梺娲诲墮閵堟悂骞冮棃娑掓闁靛繒濮烽ˇ顐︽煟閻樺弶澶勭憸鏉垮暣瀵彃鈻庨幘鏉戜化?up 婵犵數鍋炲娆撳触鐎ｎ喗鍤屽Δ锝呭暙缁狀垶鏌熷▓鍨灀闁稿鎹囬幃鐑藉级閹稿孩顔夊┑鐘愁問閸犳碍绻涢埀顒勬煛娴ｅ摜效鐎规洜鍘ч埞鎴﹀醇閻斿摜閽掑┑鐘垫暩閸嬬偛顭囧▎鎿冩晞闁告稒娼欑粻鏍ㄤ繆閵堝懎鏆熼柛妤佺閵囧嫰寮介妸褏顓煎┑鐐跺濞夋盯婀佸┑鐘诧工鐎氼剛绮缁?
    // 闂傚倷鑳堕…鍫ヮ敄閸℃瑥鍨濈€广儱娲﹂弳婊堟煃瑜滈崜鐔煎蓟閵娿儮妲堟俊顖濆亹娴煎洨绱撴笟鍥ф灁闁告梹鍨甸?5454 濠电姷鏁搁崑娑欏緞閸ヮ剙鐒垫い鎺嗗亾鐎殿喖鐖奸幆鍥ㄧ鐎ｎ偆鍘卞┑顔筋焽閸樠囧储閻㈠憡鐓?444 闂備礁鎼ˇ顐﹀疾濠婂牆钃熼柕濞垮剭?502闂傚倷鐒︾€笛呯矙閹次诲洭顢橀姀鐘靛姦濡炪倖宸婚崑鎾绘煕婵犲偆鐓奸柟顔芥そ楠炲鎮╅顫闂佽崵鍠愬姗€寮抽悢铏圭＜妞ゆ柨鎼慨鍌溾偓娈垮枛閻栧ジ骞冮姀銈嗙叆閻庯綆浜栭崑鎾绘偨閸涘﹦鍘遍梺褰掑亰閸撴稑鏆╅梺鑺ヮ焽閸犲酣鍩ユ径鎰妞ゆ牗鐭竟鏇熺節閻㈤潧浠滄俊顐㈢焸閸┾偓妞ゆ帒鍟悵顏堟煟閵婏箑鐏╃紒杈ㄥ笧閳ь剨缍嗘禍婊堝汲闁秵鐓熼柨婵嗘搐娴滃墽绱掗崒姘毙х€规洟浜堕獮鍥Ω閵夈倕顥?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 婵犳鍠楃敮妤冪矙閹烘せ鈧箓宕奸妷顔芥櫍婵犵數濮甸懝鍓х矆閸岀偞鐓欓柟顖涙緲琚氶梺鎸庣⊕濡啴寮婚妸銉㈡婵炲棙鍔楀▓銈夋⒑鏉炰即妾烽柛濠冪箞閻涱喚鈧綆鍠楅悡銉╂倵閿濆骸澧柍閿嬫礃缁绘盯骞嬮悙鏉戠）濡炪倖娉﹂崟顒€寮块梺鍐叉惈閹冲海鎲撮敂濮愪簻闁哄秲鍔庨惌瀣煛閸℃绠荤€殿喖鐖奸崺鈩冩媴鐟欏嫭鍊锋繝娈垮枟鑿ч柛鏃€鍨块悰顔芥償閵忋垻鐦堥梺鍛婁緱閸ㄦ娊顢欓弴銏♀拺婵炶尪顕ф禍鎰版煕鐎ｎ偅宕岄柡?WARP 闂傚倷绀侀幖顐﹀磻閸℃稑鍌ㄩ柣锝呮湰閺嗘粓鏌嶈閸撶喖寮婚妸銉㈡婵☆垰鐏濋顓㈡⒑閸濄儱鏆遍柛瀣崌閺岋綁鎮╂潏鈺佸婵犮垻鎳撳Λ娆撳箞閵娾晛绠瑰ù锝呮憸閻ｈ鲸绻涙潏鍓ф偧妞わ絼绮欏畷?
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
