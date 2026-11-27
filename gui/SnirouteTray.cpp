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

// WARP 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠Χ閸屾矮澹曞┑顔矫畷顒勫储鐎电硶鍋撶憴鍕妞ゎ偄顦遍埀顒勬涧閵堢顕ｉ崼鏇炵闁绘ê鐏氬В搴㈢節閻㈤潧浠╅柟娲讳簽瀵板﹪鎳為妷褏褰炬繝鐢靛Т濞层倝鎮块鈧弻娑樷槈濡吋鎲奸梺缁樻尵婵炩偓闁哄矉缍侀獮姗€骞庣粵瀣у亾濡や降浜滈柨婵嗘噽閿涘秶绱掓潏銊ユ诞闁诡喒鏅犻幊锟犲Χ閸ャ劍鐦掗梻鍌欑閹碱偊鎯夋總绋跨獥闁哄稁鍘惧畵浣搞€掑锝呬壕濡ょ姷鍋炵敮锟犵嵁鐎ｎ亖鏀介柛鈾€鏅滅紞鎴︽⒒閸屾艾鈧娆㈠璺虹劦妞ゆ帒鍊告禒婊堟煠濞茶鐏￠柡鍛埣椤㈡岸鍩€椤掑嫬钃熸繛鎴炵懅缁♀偓闂佸憡娲﹂崜姘舵偟椤忓牊鈷戦柣鐔稿閻ｎ參鏌涢妸銈囩煓闁绘侗鍠氶埀顒婄秵閸犳寮插┑瀣厓鐟滄粓宕滈悢鐓庣畺?0=闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晜閽樺缃曟繝鐢靛Т閿曘倗鈧凹鍣ｉ妴鍛村矗婢跺瞼鐦堟繝鐢靛Т閸婄粯鏅跺☉銏＄厱閻庯急鍕彋闂佸搫鏈ú妯兼崲濞戞粠妲婚柣搴㈠嚬閸撶喎顕?1=闂傚倸鍊峰ù鍥敋瑜嶉～婵嬫晝閸岋妇绋忔繝銏ｅ煐閸旀牠宕戦妶澶嬬厸闁搞儮鏅涘皬闂佺粯甯掗敃銉╁Φ閸曨喚鐤€闁圭偓鎯屽Λ锟犳倵鐟欏嫭绀€鐎殿喖澧庨幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帗绂掗懖鈺冪＝?2=闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晜閽樺缃曟繝鐢靛Т閿曘倗鈧凹鍣ｉ妴鍛村蓟閵夛妇鍙嗗┑鐘绘涧濡瑦寰勯崟顓涘亾鐟欏嫭绀€鐎殿喖澧庨幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帗绂掗懖鈺冪＝濞达絽鎼牎婵犵數鍋涢敃顏勵嚕婵犳艾鍗抽柣鏃堫棑缁愮偛鈹戦悙鏉戠仸闁煎綊绠栭幆灞解攽鐎ｎ偀鎷?
// 闂?warp-cli status 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫宥夊礋椤掍焦顔囬梻浣规偠閸庢椽宕滃▎鎴濐棜鐟滅増甯楅悡娑㈡煕閵夛絽鍔氶柣蹇ｄ邯閺屾稓鈧急鍕彅闁剧粯鐗犻弻銊モ攽閸℃ê娅ｉ梺鍝勬閸嬨倝寮婚敓鐘叉そ濞达絿顭堥～顐︽⒑闁偛鑻晶顕€鏌涘Ο鑽ょ煉鐎规洘鍨块獮妯肩磼濡厧骞堥梻浣筋潐濠㈡ɑ鏅舵惔銊ョ闁哄锛曢悷閭︾叆闁割偁鍨婚弳顐︽⒑鐠団€虫灍闁挎洏鍨介獮鍐ㄢ枎閹寸偛纾柡澶屽仧婢ф鎯堣箛娑欌拻濞达綀妫勯崥褰掓煕閻樺啿濮夐柟骞垮灲瀹曞ジ濡烽妷銊︽啺闂備浇娉曢崰鎾存叏鐎靛摜鐭嗛悗锝庡枟閳锋帡鏌涢銈呮灁闁靛棙甯掗湁闁绘瑢鍋撻柛銊ゅ嵆閸╃偤骞嬮敂钘変汗濡炪倖鍔﹀鈧瑙勬礋濮婄粯鎷呴崨濠傛殘婵炴挻纰嶉〃濠傜暦閻熸壆鏆﹂柛銉戝本顥″┑掳鍊х徊浠嬪疮椤栫偞鍋傛繛鍡樻尰閸婂灚绻涢幋鐐垫噽闁绘帞鏅槐鎺楁偐閾忣偄闉嶅銈冨妸閸庣敻骞冨▎鎿冩晜闁告洦鍘虹槐娆撴⒒娴ｅ憡鎯堥悶姘煎亰瀹曟繂鐣濋崟顐ゅ弨婵犮垼娉涜墝闁哄閰ｉ弻鐔衡偓鐢殿焾娴狅箓鏌涙惔銏☆潟SQUE 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐椤旂懓浜鹃柛鎰靛枛瀹告繃銇勯弽銊р槈閹兼潙锕ら埞鎴炲箠闁稿﹥娲熼獮蹇曗偓锝庡枛閺嬩礁鈹戦悩鍙夊闁绘挻娲樼换娑㈠箣濠靛棜鍩為梺鍝勵儍閸婃繈寮诲鍥╃＜婵☆垱妞垮Λ鍐⒑鐠団€虫灍妞ゃ劌锕顐﹀箛椤撶喎鍔呴梺鐐藉劜閸撴艾顭囬崜褏纾介柛灞剧懅椤︼附銇勯敂璇茬仯缂佽京鍋炵换婵嬪炊瑜戦幗鏇炩攽閻愭潙鐏﹂柨鏇楁櫅閵嗘帗绻濆顓犲幗闂佺懓鐡ㄥ鐟扳枍閸涱劶鐟邦煥閸垻鏆梺鍝勭潤閸℃瑧鏉搁梺鎸庣箓閹冲秹宕欐禒瀣拺闁告稑锕ら崵顒勬煕鐎ｎ亝鍣介柟骞垮灩閳藉鈻庨幇顔拘ら梻浣告啞閹稿棝宕橀妸褍甯撴繝鐢靛Х椤ｈ棄危閸涙潙纾婚柛娑樼摠閸嬶紕鎲歌箛娑樼闁圭儤鎸剧弧鈧梺鎼炲劘閸斿秴鈻嶉崶顒佲拺缂佸瀵у﹢鎵磼鐎ｎ偅宕岀€规洏鍨介獮鎺懳旀笟鍥棅婵＄偑鍊栭崝鎴﹀垂閻戞ê绶為柛鏇ㄥ灡閻撴瑧绱掔€ｎ偄顕滈柟鍙夋倐閺岀喎鐣￠柇锕€鍓堕悗娈垮枛椤攱淇婇悜鑺ユ櫆闁诡垎鍐啋闂傚倸鍊烽悞锕傚磿閸愯鐟邦潩鐠鸿櫣锛涢梺鍛婃处閸ㄩ亶宕愮捄琛℃斀闁绘ɑ褰冮顐ょ棯閹冩倯濞ｅ洤锕俊鍫曞磼濮橆偄顥氬┑锛勫亼閸娧勪繆閸ヮ剙鏋侀悹鍥ф▕閸ゆ洖鈹戦悩瀹犲鐎瑰憡绻冮妵鍕箻濡も偓鐎氼剟宕楁繝鍥ㄢ拻濞达絽鎲￠崯鐐烘煙缁嬫寧顥㈢€规洦浜濋幏鍛村礈?
// 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愯弓鐢绘俊鐐€栭悧妤冪矙閹炬眹鈧懘宕ｆ径宀€鐦堥梻鍌氱墛缁嬫帡鏁嶉弮鍫熺厾?status 闂?Connected 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗ù锝堟缁€濠傗攽閻樻彃浜為柣鎺旀櫕閹叉瓕绠涢弴鐐茬亰闂佸搫鍟悧鍡欑不椤曗偓閺岋箑螣娓氼垱歇闂佺硶鏅濋崑娑⑩€旈崘顔嘉ч柛鈩冾殘閻熴劑姊虹粙鍖″伐缂傚秴锕ら悾鐑藉箣濠垫劖寤洪梺閫炲苯澧寸€殿喖顭峰鎾偄閾忚鍟庨梻浣稿閻撳牓宕伴弽銊﹀弿闁割偅娲橀埛鎴︽煕濞戞﹫榫氬瑙勶耿閺屾稒绻濋崒娑樹淮閻庢鍠栭…鐑藉箖閵忋倖鍋傞幖杈剧秵閸熷淇婇悙顏勨偓鏍洪弽顓炵疇闊洦铔嬫径鎰唨鐟滄粌銆掓繝姘厪闁割偅绻冮崳铏圭磼閸屾凹鍎旈柡灞剧洴楠炴鈧潧鎲￠崚娑樜旈悩闈涗粶缂佺粯甯￠崺鈧い鎺戯功缁夌敻鏌涢悩宕囧ⅹ婵炴彃娼″缁樻媴閸涘﹤鏆堝┑鐐额嚋缁犳挸鐣烽弶娆炬僵缂佹棏鍨板ú锔锯偓浣冨亹閳ь剚绋掕彜闁瑰嘲顭峰铏圭矙閹稿孩鎷辩紒鐐緲缁夌懓鐣烽姘兼僵闁煎摜鏁搁崢閬嶆⒑闂堟侗妯堥柛銊﹀▕閹偓娼忛妸锝勭盎闂侀潧顧€缁犳垿宕悜妯诲弿濠电姴鍟妵婵堚偓瑙勬磸閸斿秶鎹㈠┑瀣妞ゆ巻鍋撴繛鍛囧洦鈷掑ù锝堫潐閸嬬娀鏌涢弬璺ㄐｇ紒缁樼⊕缁绘繈宕掗妶鍛吙婵＄偑鍊栭崝褏绮婚幋婵囩函婵犵數濮伴崹鐓庘枖濞戞粎妾?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣椤愪粙鏌ㄩ悢鍝勑㈢痪鎯ь煼閺屾稑鈽夐崡鐐寸亾闂佸磭鎳撶粔褰掑蓟瑜戠粻娑橆潩閸忚偐绉烽柣?sniroute.cmd 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗ù锝夋交閼板潡姊洪鈧粔鐢稿磻閿熺姵鐓欓柟顖滃椤ュ顨ラ悙顏勭伈闁哄苯绉瑰畷顐﹀礋椤愮喎浜剧憸鐗堝笒缁?warp on/off闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔衡偓鐢殿焾娴犙囨⒒閸曨偄顏柡宀嬬節瀹曟﹢濡搁妷銏犱壕闁煎鍊楁稉宥夋煛閸屾侗鍎ラ柣鏂挎閹銈﹂幐搴哗闂佽绻愬畷顒勨€旈崘顔藉仭闁哄瀵уВ鎰渻閵堝骸寮ㄦ繛澶嬫礋楠炴垿宕熼娑樹粡闂佸搫顦伴崹鐢革綖閻樼數纾介柛灞剧懅閸斿秹鏌涢弬鐐叉噺瀹曞弶绻濋棃娑卞剰闁藉啰鍠栭弻鏇熺箾閸喖濮夊┑鈩冨絻閻楀﹥绌辨繝鍥舵晬婵犻潧妫楅幆鐐寸箾閺夋垵鎮戝ù婊呪偓婕?cli 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵嬫濞戝崬鍓扮紒鐐劤椤兘寮婚敐澶婄疀妞ゆ帒鍊风划鐢告⒑閸濆嫭顥炵紒顔芥崌瀵鎮㈤搹鍦紲闂侀潧绻掓慨鐢告倶閸儲鈷戞繛鑼额嚙楠炴銇勯妸銉︻棦濠碉紕鏁诲畷鐔碱敍濮橆剙鏁ら梻渚€娼ц噹闁告侗鍨卞▓濂告⒒閸屾瑧顦﹂柟鐚溿倖鎳岄梻浣烘嚀閹测剝绻涙繝鍥╁祦闁告劦鍠楅崐濠氭煢濡警妲哥€规挸妫楅埞鎴︻敊閺傘倛鈧法鐥鐙€鐒鹃崡杈ㄣ亜閺囨浜惧┑顔硷攻濡炰粙鐛弽顓熷€锋繛鏉戭儏娴滈箖鏌″搴″箹闁搞劌鍊块弻娑㈡晜鐠囨彃绠归梺鎼炲妽缁诲牓寮婚敓鐘茬闁靛ě鍐炬毇缂傚倷鑳舵慨鍨箾婵犲洤钃熼柨婵嗩槸缁犳稒銇勯弮鍥撴慨锝呭椤啴濡惰箛鎾舵В闂佺顑呴敃銈夛綖?
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


// 闂?winget 闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃秹婀侀梺缁樺灱濡嫮绮婚悩缁樼厵闁诡垎鍐╊啈闂佹悶鍎洪崜娆戠不椤曗偓閺?Cloudflare WARP闂傚倸鍊搁崐鎼佸磹妞嬪孩顐芥慨姗嗗墻閻掔晫鎲稿鍫罕婵犲痉鏉库偓鏇㈠箠鎼达絽顥氶柣鐔稿櫞瑜版帗鍋愮€瑰壊鍠栭崜閬嶆⒑閻熸澘绾ч柨鏇樺灲楠炲啫螖閸涱喗娅滈柟鑲╄ˉ閳ь剙鍟跨粻锝夋⒒娴ｄ警鐒炬い鎴濇楠炴劖銈ｉ崘銊х枀闂佸湱铏庨崰鏍矆鐎ｎ喗鐓曢柍鈺佸幘椤忓牊鍊堕悗鐢电《閸嬫挸鈻撻崹顔界亪濡炪値鍘鹃崗姗€鐛崘顔碱潊妞ゎ偒鍘鹃崜銊︾箾鏉堝墽绉い顐㈩樀瀵櫕绻濋崶銊㈡嫽婵炶揪绲块悺鏂款焽閹邦厾绡€闁靛繆鍩勯幋锕€鐤鹃柤绋跨仛閸庣喖鏌ㄥ┑鍡樺窛闁绘帒鍚嬬换婵嬪閿濆棛銆愰柤鐟版湰娣囧﹪骞撻幒鏂库叺闂佸搫鐬奸崰鏍嵁閸℃凹妲鹃梺鎸庣☉缁夌懓鐣烽鐐茬濞达絽鍘滈幏娲⒑绾懎浜归柛瀣洴閸┾偓妞ゆ帊鑳舵晶閬嶆煕閹烘挸绗掗柣锝嗙箞瀹曠喖宕归锝嗙彟闂傚倸顭崑鍕洪敂鍓х煓闁瑰濮甸崗婊堟煃瑜滈崜鐔奉潖閾忓湱纾兼俊顖氭惈椤垿鏌ｈ箛鎾剁闁荤啿鏅犲畷娲焵椤掍降浜滈柟鍝勬娴滄儳顪冮妶搴″箻闁稿繑锚椤曪絿鎷犲ù瀣潔濠碘槅鍨堕弨閬嶆倵婵犳碍鈷戠憸鐗堝笒娴滀即鏌涢幘瀵糕姇缂佸倹甯￠獮姗€顢欑憴锝嗗濠电偞娼欓崥瀣偡瑜忕划濠囨倻閼恒儳鍘遍梺鍝勫€稿锟犲矗閸曨剦娈介柣鎰▕濡叉悂鎽堕弽顓熺厱婵炴垵宕獮鏍磼閹邦厾鈽夋い顏勫暣婵″爼宕卞Ο鐑樻闂備胶顭堥鍥窗鎼淬倗浜藉┑鐐存尰閸╁啴宕戦幘瀛樺弿?
// 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰▕閻掕姤绻涢崱妯诲碍閻熸瑱绠撻幃妤呮晲鎼粹剝鐏嶉梺鎼炲€曢懟顖濈亙闂佹寧绻傞幊搴ㄥ汲閻愮儤鐓曢柕鍫濇噺閹兼劙鏌嶇憴鍕伌闁诡喒鏅犻獮鎾诲箳閹炬惌鍟€闂傚倷绀佺壕顓犲垝椤栨娑㈠礃椤旇壈鎽曢悗骞垮劚閻楁粌顬婇妸鈺傗拺闁告稑锕ョ亸浼存煟閻斿弶娅婄€?UAC闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔衡偓娑欘焽缁犱即鏌ら幏灞剧《缂佽鲸甯掕灃闁告劏鏅濊ぐ鍖?闂傚倸鍊搁崐宄懊归崶褏鏆﹂柣銏㈩焾缁愭鏌熼柇锕€鍔掓繛宸簻缁狅絾绻濋姀锛勭畵缂傚秴锕ら悾鐑藉醇閺囥劍鏅為梺鎸庢磵閸嬫挻銇勯敂钘夘棆闁瑰箍鍨归埞鎴犫偓锝庝簽閸婄偤鎮峰鍐ч柟顕€娼ч～婊堝焵椤掑嫬绠栨俊銈呮噽瀹撲線鏌涢…鎴濇灈濠殿喖閰ｅ娲偡閺夋寧姣愰梻浣稿簻缂嶄線寮幇鐗堝€烽柛婵嗗珋瑜嶉…璺ㄦ崉娓氼垰鍓抽梺鍛娒畷顒勫煘閹达附鍋愰悹鍥囧啩绱ｉ梻浣告憸閸犲秹宕￠幎钘夌畺闁绘劕鎼粻鐟懊归敐鍛础闁告ɑ鎹囧娲嚒閵堝懏鐎炬繝銏㈡嚀濡瑩寮查妷鈺傜厽閹兼番鍊ゅ鎰箾閸欏鐒介柟骞垮灲瀹曞ジ濡烽妷褌绱滈梻浣瑰濞叉牠宕愰崫銉т笉闁规儼濮ら悡娑㈡煕閹扳晛濡煎ù婊呭仦娣囧﹪宕ｆ径瀣攭濠殿喖锕︾划顖炲箯閸涘瓨鍋￠梺顓ㄧ畱濞堟繄绱撻崒娆戭槮妞ゆ垵妫濆畷褰掑锤濡も偓缁犵娀鏌ｉ弮鍌楁嫛闁轰礁绉归弻锝夊箛椤撶喓绋囨繛瀵稿Т閵堢顫忓ú顏咁棃婵炴番鍊栭惄顖氱暦閻旂⒈鏁冮柨婵嗘瑜版椽姊婚崒姘偓椋庣矆娓氣偓楠炲鍩勯崘顏嗘嚌濠德板€曢幊搴ㄥ磼閵娿儙鏃堟晲閸涱厽娈┑顔炬嚀瀵爼骞堥妸銉庣喖宕归鎯у缚闂備胶绮幐璇裁洪悢鐓庤摕闁绘柨鍚嬮崐缁樻叏濡も偓濡瑩鎮鹃悜鑺モ拺閺夌偞澹嗙粣鏂棵瑰鍐煟鐎殿喖顭烽弫宥夊礋椤忓懎濯伴梻浣告惈閸婂爼宕愬☉銏犺Е妞ゆ劧闄勯埛鎴︽煕閿旇寮鹃柛鏃€绮庣槐鎺楃叓椤撶姷鐓撻梺鐐藉劵缁犳挻淇婇幖浣哥厸濞达絼璀﹀Σ浼存⒒娓氣偓濞佳嗗櫣闁诲繒鍋炵€氬氦顦规慨濠傤煼瀹曟帒鈻庨幒鎴濆腐缂傚倷绶￠崳顕€宕归崼鏇炵伋闁挎洖鍊搁柋鍥煏婢跺牆鍔滄俊宸墴濮婃椽宕崟顓涙瀱闂佸憡锚閵堟悂骞冮悙鍝勫瀭妞ゆ劗濮崇花濠氭⒑閻熸壆鎽犵紒璇插暣瀹曟垿濡烽敂璺ㄧ畾闂佸憡鎸嗛崘銊ョ闂?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴欏灪閸嬨倝鏌曟繛鐐珔闁告艾缍婇獮鏍庨鈧俊鑲╃棯閹规劦娼愬ǎ鍥э躬婵″爼宕ㄩ鐔割唲闂?CLI闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔兼倻濮楀棙鐣烽梺鎼炲€曢懟顖濈亙闂佹寧绻傞幊搴ㄥ汲閻愮儤鐓曢悗锛卞嫭鐝氶梺鍝勭焿缁插€熺亽闂佸憡绻傜€氼厼袙閹邦剦娓婚柕鍫濇缁€澶婎渻鐎涙ɑ鍊愭鐐茬墦婵℃悂濡烽钘夌紦闂備胶纭堕崜婵嬨€冮崨杈剧稏闁冲搫鎳忛悡鐔兼煏韫囧﹥娅呴柣蹇旂洴閺屾稒鎯旈敐鍡樻瘓濡?winget闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇閸撗呮殸闁诲孩鑹鹃ˇ浼村Φ閸曨垰绠抽柟瀛樼箥娴犻箖姊虹粙鍧楊€楁い锔炬暬瀵鈽夊Ο閿嬵潔濠电偛妫欓崝妤冪矙閸ヮ剚鐓?winget 闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃繘骞戦姀銈呯疀妞ゆ棁妫勬惔濠囨⒑瑜版帒浜伴柛顭戝墯缁傚秹鎮烽柇锔惧數闁荤娀缂氬▍锝夋倶鏉堚晝纾奸柣妯挎珪椤ュ牓鏌＄仦鍓р槈闁宠棄顦靛畷锟犳倷鐎甸晲鎲炬繝?Cloudflare 闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃秹锝炲┑瀣櫇闁稿矉濡囩粙蹇旂節閵忥絾纭鹃悗娑掓櫊楠炲﹤鐣濋崟顑芥嫽婵炶揪绲介幉锟犲箚閸儲鐓熸い鎺嗗亾闁靛牏顭堥悾閿嬪閺夋垹顔掗柣鐘叉穿鐏忔瑩宕濋敃鈧—鍐Χ閸℃鐟ㄩ梺绋匡工缂嶅﹤鐣烽悽绋跨劦妞ゆ巻鍋撴い顏勫暣婵℃儼绠涘☉娆樷偓宥嗙節閻㈤潧浠滈柨鏇樺妼铻為柕蹇ョ磿缁♀偓闂佹眹鍨藉褍鐡繝鐢靛仜閻即宕愬Δ鍐ㄥ灊闁割偅绻勯弳鍡涙煕閺囥劌澧伴柛妯哄船閳规垿鎮欓弶鎴犱淮閻庤娲﹂崜姘辩矉瀹ュ绠涢柣妤€鐗忛崢?
        // 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰惈閸ㄥ倿鏌涢锝嗙缂佺姳鍗抽弻娑樷攽閸曨偄濮㈤梺娲诲幗閹瑰洤顫忔繝姘唶闁绘柨鍢查獮蹇涙⒑閹稿海绠撴繛灞傚姂閹苯鈻庨幘瀵稿幈闂佺粯鏌ㄩ崲鏌ュ汲閻愮儤鍋ㄦい鏍ㄧ矊閺嬫梻绱掗娆惧殭闁宠棄顦垫慨鈧柣妯活問閸熷姊绘担绛嬫綈闁稿海鍘ц灋闁告洦鍠撴禍?Windows 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣椤愪粙鏌ㄩ悢鍝勑㈢紒鎰殕缁绘盯骞嬮悙鍨櫗闂佸憡鐟ョ换姗€寮婚弴銏犻唶婵犻潧娲ら悵鏃堟⒑?winget闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔衡偓娑欘焽缁犱即鏌ら悧鍫濐嚋闁靛洤瀚版慨鈧柕蹇嬪灪椤庡舶闂傚倸鍊搁崐鎼佸磹妞嬪孩顐芥慨姗嗗墻閻掔晫鎲稿鍫罕闂備礁鎼崯顐︽偋婵犲洤鍚归柛鏇ㄥ灡閻撴洟鏌熼幆褍绾х紒浣规緲閳规垿顢欓挊澶嗘灆闂佽鍠栫紞濠囩嵁娓氣偓楠炴帡骞嬪┑鎰偓宄扳攽閻愭潙鐏﹂柟绋挎憸缁棁銇愰幒鎴ｆ憰閻庡箍鍎遍悧婊冾瀶閵娾晜鈷戦柛娑橈攻鐏忕敻鏌涢悩宕囧⒌闁绘侗鍣ｅ畷姗€顢樿閳诲矂鏌ｆ惔銈庢綈婵炲弶绮撳畷銏ｎ樄闁糕晝鍋ら獮瀣晝閳ь剟鏌嬮崶銊х瘈闂傚牊绋撴晶鏇燁殽閻愨晛浜鹃梻鍌氬€搁崐宄懊归崶褏鏆﹂柣銏㈩焾缁愭鏌熼幍顔碱暭闁稿绻濋弻鏇㈠醇濠靛洤顦╁銈嗗姃缁瑩寮婚弴銏犻唶婵犻潧娲らˇ鈺侇渻閵堝骸浜濇繛鍙夛耿楠炲牓濡搁妷搴ｅ枛楠炴劖鎯旈敐鍐ｅ亾閹版澘绾?
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


// WARP 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晝閳ь剛澹曟繝姘厱闁斥晛鍟伴埥澶愭煕鐏炶濮傞柡灞诲€曢湁閻庯綆鍋呴悵鏇烆渻閵堝骸浜濇繛鑼枛瀵鎮㈤崨濠勭Ф闂佸憡鎸嗛崨顔筋啅闂備浇宕甸崰鎰版偡鏉堚晝涓嶉柟鎹愬吹瀹撲線鏌″搴ｄ粓閹兼惌鐓堥弫鍡涙煃瑜滈崜鐔煎箖濮椻偓閸╋繝宕ㄩ鐘垫毎闂備胶绮弻銊╁触鐎ｎ喖鐓曢柟杈鹃檮閻撴洘绻濋棃娑欘棞妞ゅ浚鍋勯…鑳樄濞存粌鐖煎璇测槈閵忕姷顔掗柣搴ㄦ涧閹诧繝宕宠濮?DNS 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晝閳ь剛澹曢悷鎳婂綊鎮╁顔煎妧缂傚倸绉甸悧鏇⑩€﹂崸妤佸殝闂傚牊绋戦～鍥⒑闁偛鑻崢鍝ョ磼閳ь剚绗熼埀顒勩€佸鈧畷妤呮偂鎼达絿鐛┑鐘垫暩婵鈧氨鍏樺畷顒勬濞戞帗鏂€闂佺粯锕╅崰鏍倶鏉堛劎绠惧璺侯儑濞叉挳鏌℃担绋挎殻濠碉紕鏌夐ˇ鎾煟椤撶偞顥㈤柡灞界Х椤т線鏌涢幘璺烘瀻妞ゎ偄绻愮叅妞ゅ繐瀚槐鍫曟⒑閸涘﹥绀€闁诲繑淇洪崐鎾⒒閸屾瑨鍏屾い顓炵墢閳ь剙鐏氱敮鈥崇暦瑜版帒閱囬柡鍥╁仦濡差剟姊?
//
// 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰惈閸ㄥ倿鏌涢锝嗙缂佺姵褰冮湁闁挎繂鎳庨ˉ蹇旂箾鐎涙澧甸柡灞炬礃缁绘稖顦查柛鐕佸亰閹ɑ绻濋崶銊㈡嫼闂佸憡鎸昏ぐ鍐╃閻愮儤鐓曢柣妯挎珪瀹曞矂鏌涢埞鍨伈鐎殿喗鎸虫慨鈧柨娑樺楠炲牊淇婇悙顏勨偓鏍礉濡や焦鍙忛柟缁㈠枛缁€澶嬬節婵犲倸骞樼憸鐗堝笚閸嬫劗绱掑☉姘兼敯闁逞屽墮濞硷繝寮婚悢纰辨晩闁诡垎鍐ㄧ闁诲孩顔栭崰妤€顭囧▎鎿勭稏婵犻潧娴傚娆撴煛瀹ュ啫澹夋繝濠傜墛閳锋帒霉閿濆妫戝☉鎾瑰皺缁辨帡宕ｆ径濠庡殝缂備胶绮惄顖炲春閳ь剚銇勯幒鎴濐仾闁绘挻绋戦湁闁挎繂鎳忛幉鍝モ偓娑欑箓椤啴濡舵惔鈥茬按闂佹悶鍔嬬划娆撴偘椤曗偓閸┾偓妞ゆ巻鍋撻柍瑙勫灦瀵板嫮鈧綆鍋掑?闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠Χ閸屾矮澹曞┑顔矫畷顒勫储鐎电硶鍋撶憴鍕缂傚秴锕濠氬幢濡ゅ﹤鎮戦梺鍛婁緱閸犳稓娆㈤姀銈嗏拻濞达絿鎳撻婊呯磼鐠囨彃鈧儻妫熷銈嗙墬婢规洟宕戦幘鎰佹僵妞ゆ帒顦版晥缂傚倷鑳剁划顖滅矙閹达富鏁婇煫鍥ㄤ緱閺佸﹪鏌ゆ慨鎰偓鏇㈠吹椤掑嫭鈷掑ù锝夘棑娑撹尙绱掗悩鍐茬伌闁诡啫鍥х闁归鐒︾紞搴♀攽閻愬弶鈻曞ù婊冪埣瀵偊宕卞鏇炵秺閺佹劙宕奸锝囩Х闂備胶顭堢€涒晠骞愰崜褎顫曢柟鎯х摠婵潙霉閻樺樊鍎忛柟鐣屾暩缁辨挻鎷呯粵瀣闁诲孩鐭崡鍐茬暦濞差亜鐒垫い鎺嶉檷娴滄粓鏌熼悜妯虹仴闁逞屽墮缂嶅﹤鐣烽悽绋跨劦妞ゆ帒瀚埛鎴︽煕濠靛嫬鍔氭繛鍛箻閺屾盯濡搁敃鈧埢鏇犫偓娈垮櫘閸嬪﹪銆佸璺虹劦妞ゆ帒瀚弰銉╂煃瑜滈崜姘跺Φ閸曨垰绠抽柟瀛樼妇閸嬫捇鎳￠妶鍜佹婵犻潧鍊搁幉锟犳偂閻斿吋鐓欏ù鐓庣摠濞懷冾熆瑜滈崰妤呭Φ閸曨垰鍗抽柣鏃囨閻撲焦绻涢敐鍛悙闁挎洦浜獮鍐ㄢ枎閹惧磭顔岄梺鐟版惈濡绂嶆ィ鍐╃厱鐟滃酣銆冮崨顖滀笉闁哄稁鐏愯ぐ鎺撳亹鐎瑰壊鍠栭崜浼存煙椤栨粌鏋涙慨濠冩そ楠炴劖鎯旈姀顫穿闂備胶顭堥鍡涘箲閸ヮ剙钃熼柨婵嗩槸缁犲鎮楀☉娆樼劷妞わ富鍙冨娲焻閻愯尪瀚板褜鍣ｉ弻锝堢疀閺冨偆鏆＄紓浣规⒒閸犳捇鍩€椤掑﹦绉甸柛鐘崇墱婢规洘绻濆顓犲幍闂佽鍨庨崨顒勫仐闂備胶顭堥鍡涘箲閸パ呮殾鐟滅増甯╅弫鍐煏韫囧﹥娅嗘い銉︽崌濮婄粯鎷呮笟顖滃姼濡炪倖鍨靛Λ婵嬬嵁閹版澘绠瑰ù锝呮贡閻ｆ娊姊婚崒娆掑厡缂侇噮鍨堕弫鍐煛閸涱厾鐛ラ梺鍝勭▉閸樿偐绮堟径瀣闁糕剝蓱鐏忎即鏌?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮闁汇値鍠楅妵鍕冀椤愵澀绮堕梺鎼炲妼閸婂潡寮诲☉銏╂晝闁挎繂妫涢ˇ銉╂⒑閽樺鏆熼柛鐘崇墵瀵鈽夐姀鐘栥劑鏌ㄥ┑鍡樺櫣妞ゎ剙顑呴—?闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇妤ｅ啯顎嶉梺绋垮椤ㄥ懘濡撮幒鎴僵闁挎繂鎳嶆竟鏇㈡⒒娴ｈ櫣銆婇柡鍌欑窔瀹曟垿骞橀幇浣瑰瘜闂侀潧鐗嗗Λ妤呭锤婵犲洦鐓曢悗锝庡亝瀹曞矂鏌＄仦璇测偓鏇犲弲闁荤姴娲╃亸娆愮閹间焦鈷戦悹鍥ｂ偓铏亪濠电偛鐨烽埀顒€纾弳锕傛煟閺冨倵鎷￠柡浣哥У缁绘盯骞嬮悙鑼懖闁诲骸鐏氶悧鐘差潖缂佹ɑ濯撮柛娑橈工閺嗗牊绻涢幘瀵割暡妞ゃ劌锕ら悾宄邦煥閸愶絾顫嶉梺闈涚箚閳ь剙鍘栭悽濠氭⒒娴ｇ瓔娼愬鐟版閺呰泛螖閸涱喖浜楀┑鐐村灟閸ㄦ椽鎮￠弴鐔翠簻闁逛即娼ф禍婊兠瑰鍕煀妞ゎ叀鍎婚¨渚€鏌涢悩鎰佹畼闁瑰箍鍨归埞鎴犫偓锝庝簽閸?
// DoH/nginx 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愯弓鐢绘俊鐐€栭悧婊堝磻濞戙垹鍨傞柛宀€鍋為悡鐔兼煏韫囧鐒洪柣鎺戝⒔缁辨帡鍩€椤掑嫬纾兼繛鎴炵懃瀵灝鈹戦敍鍕哗妞ゆ泦鍕洸濡わ絽鍟崐鐢告偡濞嗗繐顏紒鈧埀顒勬⒑缂佹澧柕鍫熸倐閻涱噣宕橀鑲╁幐闂佺鏈粙鎾剁矓鐠轰警娓婚柕鍫濇婵倿鏌涙繛鍨偓鏇炲祫閻熸粎澧楃敮妤呭煕閹烘嚚褰掓晲閸涱喖鏆堥梺鍝ュ枔閸嬬偤濡甸崟顖毼╅柕澶涚畱濞咃綁鏌ｉ幘鍗炩偓婵嬪蓟閺囩喓绡€闊洦鎸惧Σ锝呪攽閻愯尙澧旂紒顔界懇瀵鈽夐姀鐘栥劍銇勯弮鍌氬付妞ゎ偀鏅犲鍫曞煛閸屾粎鐣虹紓浣虹帛缁嬫帒顭囪箛娑樼鐟滃秹宕哄畝鍕拺缁绢厼鎳庤缂備緡鍠楅幑鍥х暦濞差亝鍊锋繛鏉戭儐椤秴鈹戦悙鍙夘棡闁搞劌缍婇幆渚€宕奸妷锕€浠┑鐘诧工閸熺増鏅ラ柣搴ゎ潐濞诧箓宕归懜鍏哥箚闁归棿绀侀悡娑樏归敐鍛儓濠殿噯闄勬穱濠囨倷椤忓嫧鍋撻弽顓炲瀭闁绘挸绨堕弸鏍煛閸ャ儱鐏╃紒鈧径灞稿亾楠炲灝鍔氭い锔垮嵆閹繝寮撮姀锛勫帾婵犵數濮寸换鎺楀礆閺夊簱鏀介柨娑樺濞呮洜绱掓潏銊ユ诞鐎规洘甯掗埥澶娢熼懖鈺傚殐闂傚倷娴囬鏍垂閸楃倣娑㈠礃閳哄倸寮块梺閫炲苯澧撮柡灞界Ч婵＄兘濡疯缁辩偞绻濋姀锝嗙【閻庢碍婢橀～蹇曠磼濡顎撻梺鍛婄☉閿曘倝寮抽崼銉︹拺闁告稑锕ら悘銉х磽瀹ュ拑宸ラ柣锝囨焿閵囨劙骞掗幘鍏呯綍闂備礁澹婇崑鍡涘窗鎼粹槅鍤曢柛褎顨嗛埛鎴︽煕閹邦剙绾ч柟顖氱墦閺岀喖宕橀懠顒傤啋閻庤娲滈弫濠氬春閳ь剚銇勯幒鎴濐仾闁稿﹦鏁婚弻娑滅疀閹垮啯笑婵炲瓨绮撶粻鏍ь潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯閸犳濡甸崟顖氭閻犳亽鍔庨澶娾攽閻愰潧甯剁紒缁樕戞穱濠囧箹娴ｈ倽銊ф喐韫囨稒鍋樻い鏂跨毞閺€浠嬫煟閹邦剛鎽犻悘蹇庡嵆閺屻倝鎮烽弶搴撴寖闂佸吋妞芥禍鍫曞蓟閸℃鍚嬮柛娑卞灲缁遍亶姊绘担绛嬫綈鐎规洘锕㈤、姘愁槾濠㈣娲熷畷妤呮嚃閳哄啰妲囬梻鍌氬€搁悧濠勭矙閹惧瓨娅犻柕寰涢绨婚梺鍝勫暊閸嬫捇鏌涢弮鈧悷銉╋綖韫囨洜纾兼俊顖濐嚙椤庢捇姊洪懡銈呮瀾濠㈢懓妫涚划濠囨倻閼恒儮鎷洪梺绋跨箰閸氬濡甸悢鍏肩厱闁靛鍔嬮崥顐ょ磼椤旂⒈鍎旀鐐村浮楠炲寮埀顒勶綖瀹ュ應鏀介柣鎰级椤ョ偤鏌涢妸褍鏋涚€殿喗鎮傚浠嬵敇閻斿弶瀚奸梻鍌氬€搁悧濠勭矙閹惧瓨娅犻柡鍥ュ灪閸婂灚鎱ㄥΟ鐓庡付鐎殿噮鍠楅妵鍕閳╁啰顦伴梺鍦焾閿曘儳绮╅悢灏佹斀闁割偆鍠庡▓灞筋渻閵堝骸骞戦柛鏃€鍨甸悾宄邦潨閳ь剟鐛崱姘兼Щ婵犵濮嶉崨顖滐紳?
//   1) 婵犵數濮烽弫鍛婃叏閻戝鈧倿鎸婃竟鈺嬬秮瀹曘劑寮堕幋鐙呯幢闂備線鈧偛鑻晶鎾煛鐏炲墽銆掗柍褜鍓ㄧ紞鍡涘磻閸涱厾鏆︾€光偓閸曨剛鍘?5444(DoH 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閼碱剦鍞堕梻浣告啞缁哄潡宕曢幎鑺ュ剹闁瑰墽绮悡鏇熴亜閹板墎绋荤紒鈧崘顔界厱? / 5454(DoH 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愬弶鐤勯梻浣筋嚃閸ㄥジ鎮橀幇顖樹汗闁圭儤鎸搁埀顒冨吹缁辨帒鈽夊鍡楀壉闂? / 443(nginx) 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠Χ閸屾矮澹曞┑顔矫畷顒勫储鐎电硶鍋撶憴鍕妞ゎ偄顦遍埀顒勬涧閵堢顕ｉ崼鏇炵闁绘ê鐏氬В搴㈢節閻㈤潧浠﹂柟绋款煼瀹曟椽宕橀鑲╋紱闂佺懓澧界划顖炴偂濞戙垺鍊堕柣鎰仛濞呮洖顭胯缁犳捇寮婚悢铏圭煓闁告牑鍓濋弫楣冩倵閸偅绶查悗姘煎櫍閸┾偓妞ゆ帒锕︾粔鐢告煕濡亽鍋㈡鐐叉瀹曞ジ濡烽敂瑙勫?
//   2) 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锕€螣娓氼垱鈻撳┑鐐茬摠閻楃娀寮婚弴鐔虹闁绘劦鍓氶悵鏃傜磽娴ｅ搫校闁绘搫绻濆濠氬即閿涘嫮鏉搁柣搴秵娴滄牠宕戦幘璇插嵆闁绘ê鍚€缁楀鈹戦悙鏉戠仧闁搞劌顭峰畷锝夊箻缂佹鍘遍梺闈涱槹閸ㄧ數鈧凹鍘奸悾鐑藉礃瀹割喗瀵岄梺闈涚墕閹虫劗绮婚幍顔瑰亾鐟欏嫭澶勯柛鎾跺枛瀹曟椽濡烽敃鈧欢鐐烘煙闁箑澧版い鏃€鎹囧娲川婵犲倸顥堥梺鍛婁緱閸ㄩ亶寮查妸銉㈡斀闁绘ɑ鍓氶崯蹇涙煕閻樻剚娈欑憸鎵█濮婃椽宕ㄦ繝鍐弳闂備礁搴滅紞渚€鎮伴鈧獮瀣晝閳ь剛绮堢€ｎ偁浜滈柡宥冨妽閻ㄦ垶銇勯幘铏儓闁宠鍨块幃鈺冩嫚瑜嶆导鎰版⒑閻熸澘妲绘い鎴濇閳ь剟娼ч妶绋跨暦閹偊妲烽梺琛″亾?sniroute.cmd up 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ磸閳ь兛鐒︾换婵嬪磼濡や胶浜欐繝鐢靛仦閸垶宕瑰ú顏勭柧婵犻潧顑嗛悡蹇涙煕椤愶絿绠ユ俊鎻掔秺閺屾稓鈧急鍕彋闂佸搫鐭夌徊鍊熺亽闂佹儳绻掑▍銏㈡閻愬樊娓婚柕鍫濆€瑰▍濠囨煛閳ь剟鏌嗗鍛傦箓鏌涢弴銊ョ仩缂佺姴纾埀顒€绠嶉崕鍗灻归崒姣硷綁顢欑喊杈ㄥ瘜闂侀潧鐗嗛幊搴敆閵忋倖鐓熼柣鏂垮级濞呭﹥顨ラ悙鎻掓殶婵炴垹鏁诲畷顏呮媴閸忓鐎搁梻鍌氬€峰ù鍥Υ閳ь剟鏌涚€ｎ偅宕岄柡灞剧☉铻栭柛鎰╁妼婢瑰姊?
// 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ磵閳ь剨绠撳畷濂稿閳ュ啿绨ラ梻浣稿閸嬩線宕曟潏鈺冪焼濠㈣埖鍔栭悡娆撳级閸繂鈷旈柣锝堜含閻ヮ亪骞嗚閸嬨儵鏌熼绛嬫疁闁诡喕绮欏畷銊︾節閸愵亜绀侀梻鍌欒兌缁垳鏁幒妤佹櫇妞ゅ繐瀚烽崵鏇㈡煕椤愶絾绀€闂佸崬娲﹂幈銊ヮ潨閸℃绠洪梺缁樼箖濡啴骞冨Δ鍐╁枂闁告洦鍓涢ˇ銊モ攽閻愯泛鐨洪柛鐘崇墪椤曪絿鎷犲ù瀣潔濠殿喗顨呭Λ娆撳磽闂堟侗娓婚柕鍫濇缁楁帡鎮楀鍗炲幋闁糕斁鍋撳銈嗗笂缁垛€斥枔濠婂牊鐓涢悘鐐额嚙閳ь剚绻傞锝夊醇閺囩偟顓哄銈嗙墬缁诲倿骞嗛崼鐔剁箚闁靛牆娲ゅ暩闂佺顑囬崑銈夌嵁閹达箑鐐婃い鎺戝€哥粊锕傛⒑閹肩偛鍔撮柛鎿勭畵瀵偅绻濋崶褏楠囬梺缁樺姈濞兼瑩藟鐎ｎ亶娓婚悷娆忓閳绘洘鎱ㄦ繝鍕笡闁瑰嘲鎳橀幃鐑芥焽閿曗偓濞堟繄绱撻崒娆戭槮妞ゆ垵妫濆畷褰掑锤濡も偓缁犳岸鎮橀悙鎻掆挃闁荤喎缍婇弻宥堫檨闁告挾鍠栨俊瀛樻媴缁洘鐎婚梺瑙勫劤绾绢參宕濋敃鈧—鍐Χ閸℃鐟ㄩ柣搴㈠嚬閸欏啴骞冮敓鐘冲亜闁硅偐鍋樼花璇差渻閵堝棗濮﹂柛瀣閺呭爼顢欑亸鏍ㄦ杸闂佺偨鍎寸亸娆撴儗濞嗘挻鐓涢悘鐐插⒔濞叉挳鏌涢埡瀣瘈鐎规洘甯掗…銊╁礋椤愩垻浠梻鍌氬€峰ù鍥敋閺嶎厼鍨傚ù鐓庣摠閸庢鏌涚仦鍓р棩缂佽妫濋弻锝夊箣閿濆棭妫勯梺缁樻尰閻╊垶寮诲鍫闂佸憡鎸诲銊у垝?闂傚倸鍊峰ù鍥敋瑜嶉～婵嬫晝閸岋妇绋忔繝銏ｅ煐閸旀牠宕戦妶澶嬬厸闁搞儮鏅涘皬闂佺粯甯掗敃銉ф崲濞戙垹骞㈡俊顖濇娴犳挳姊洪幖鐐插缂佽鐗撳濠氬Ω閳哄倸浜滈梺鍛婄箓鐎氬懘濮€閵忋垻锛滈梺閫炲苯澧寸€规洖宕埥澶娾枎閹存繂绠為梻鍌欑窔濞佳囁囨导鏉戝瀭闁汇垻顣介崑鎾愁潩閻撳孩鐝濋柦妯荤箞閺屾洘寰勯崱妯荤彆闂佹娊鏀遍崹鍧楀蓟濞戞ǚ鏀介柛鈩冾殢娴尖偓濠电姭鎷冮崶锔藉枤闂佸搫鏈惄顖炵嵁濡皷鍋撻棃娑欏暈闁革急鍥ㄢ拺?DNS 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晝閳ь剛澹曢悷鎳婂綊鎮╁顔煎妧缂傚倸绉甸悧鏇⑩€﹂崸妤佸殝闂傚牊绋戦～鍥⒑?闂?
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
        // up 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰▕閻掕姤绻涢崱妯诲碍閻熸瑱绠撻幃妤呮晲鎼粹剝鐏嶉梺鎼炲€曢懟顖濈亙闂佹寧绻傞幊搴ㄥ汲濞嗘挻鐓曢悘鐐额嚙婵＄晫绱掔紒妯兼创妤犵偛顑呴埢搴ょ疀閺囨碍鍋呯紓?闂傚倸鍊搁崐宄懊归崶褏鏆﹂柛顭戝亝閸欏繒鈧箍鍎遍ˇ顖炴倷婵犲洦鐓忓┑鐘茬箻濡绢喗绻涢崼婵囧暈缂佺粯绻堥幃浠嬫濞磋埖鐩弻娑氣偓锝庡亝瀹曞矂鏌＄仦鍓ф创濠碉紕鍏橀、娆撴偂鎼搭喗浜ょ紓鍌氬€风拋鏌ュ磻閹炬剚鐔嗛柤鎼佹涧婵牓鏌ｉ幘鍐叉殻闁哄本娲樺鍕醇濠靛棗顫撻梻浣告惈濡繃绂嶉崼鏇炶摕闁跨喓濮村婵囥亜閺冨倸浜鹃柛锝勫嵆濮婃椽宕崟顒佹嫳缂備礁顑嗛幑鍥春閳ь剚銇勯幒鎴姛缂佸鏁婚弻娑氣偓锝庝憾濡插湱绱掗弮鍌氭瀻閾伙絽鈹戦崒婵囶棄缂傚秴锕獮鍐煛閸涱喖娈ゅ銈嗗笂閻掞箓宕伴弽顓熲拻濞达絿鐡旈崵鍐煕閵娿儱鑸归摶鐐烘煕閹伴潧鏋涢柦鍐枛閺屾洘绻涜閸嬫捇鏌涚€ｎ偅宕岄柡浣瑰姍瀹曘劑顢涘▎鎴︹攺闂傚倷鐒︽繛濠囧绩閸楃伝娲偄婵傚缍庡┑鐐叉▕娴滄粍瀵奸悩缁樼厱闁哄洢鍔屽▍妯荤箾閻撳海鍩ｆ慨濠呮缁瑩骞愭惔鈩冩暘闁诲氦顫夊ú鐔奉焽瑜嶅畵鍕⒑瑜版帒浜伴柛蹇旓耿瀵劍绂掔€ｎ偆鍘卞銈嗗姉婵挳鎮橀埡鍐ｅ亾鐟欏嫭纾搁柛搴ㄤ憾閳ユ棃宕橀鍢壯囨煕閳╁啰鎳冩い顐庡洦鈷戦柤娴嬫櫆閸も偓婵犫拃鍛珪婵″弶鍔欓幃娆撳传閸曨収鍟嬮梻浣告惈閸婂爼宕曢弻銉稏鐎广儱顦伴埛鎴︽偡濞嗗繐顏╅柛鏂诲€楅惀顏嗙磼閵忕姴绠洪梺缁橆殔鐎涒晝鎹㈠┑鍫濇瀳婵☆垰鎼埛澶愭偡濠婂嫭绶查柛鐔告尦閻涱喖螖閳ь剟鈥﹂妸鈺佸窛妞ゆ挾鍋熸禍浼存⒒娴ｇ鏆遍柟纰卞亰瀹曨垶鎮╃拠鑼紱闂佺懓澧界划顖炲疾閺屻儱绠归悗娑欘焽缁犳﹢鏌ｉ埡渚€鍙勬慨濠呮缁辨帒螣鐠囨煡鐎虹紓鍌欐祰椤曆囨偋閹炬剚鍤曢悹鍥ㄧゴ濡插牊绻濇繝鍌涘櫧闁哄鎮傞弻锝嗘償椤栨粎校婵炲瓨绮嶇划鎾绘晲閻愬搫鐐婃い鎺嶈閹疯櫣绱撴担鍓插剰婵炵》绻濋、鎾澄旀担鍏哥盎闂佸搫娲㈤崝宀勭嵁濡ゅ啰纾奸柛灞炬皑鏁堥悗瑙勬磸閸斿秶鎹㈠┑瀣妞ゅ繐鏌婇妶澶嬧拻濞达絽鎽滈弸鍐╀繆濡炵厧濡跨紒顔肩墛閹峰懘宕崟鍨棥闂備胶顫嬮崟鍨暦闂佹娊鏀遍崹鍨潖婵犳艾閱囬柣鏃囥€€婵洤顪冮妶鍐ㄥ姕缂侇喗鎹囧璇测槈濞嗘垹鐦堟繛杈剧秬濞咃綁鎮￠幘缁樷拺閻犲洠鈧櫕鐏堝┑鐐茬毞閳ь剙纾弳锕傛煕濡ゅ啫浜归柡瀣叄閺岀喖鏌囬敃鈧晶鎵喐閻楀牊灏︽慨濠勭帛缁楃喖鍩€椤掆偓椤洩顦撮柟骞垮灲瀹曞崬鈽夊Ο纰辨Ф婵犵數濞€濞佳兠洪妸鈹库偓鍛存煥鐎ｂ晝绠氬銈嗙墬閻熴劑顢楅悢鍏肩厽?30 缂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕闁芥ɑ绻堥弻鐔封枔閸喗鐏嶉梺鍝勬缁秹濡甸崟顔剧杸闁圭偓鎯屽Λ鈥愁渻?
        // 濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴欏焺閺佸銇勯幘璺烘瀾闁告瑥绻橀幃妤呮濞戞瑦鍠愰梺娲诲幗閹搁箖鍩€椤掑喚娼愭繛鍙夌墪鐓ら柕濞у懍绗夐梺鍝勫€归娆撴偄閸℃稒鍋ｉ柛婵嗗閹牊銇勯妸锔炬噰闁哄矉缍侀獮鎺楀箣椤栨繂鎯堥柣搴㈩問閸犳牠鎮ラ悡搴ｆ殾闁绘挸瀵掑鈺傘亜閹捐泛鏋旂憸鏉挎嚇閺岋絾鎯旈妶搴㈢秷濠电偛寮堕敋閻撱倝鏌ｉ弮鍌氬付缂佺姵鐗楁穱濠囶敍濠靛洨銆婇梺姹囧€愰崑鎾绘⒒娴ｇ瓔娼愰柛搴㈠▕閹潡宕堕鈧弰銉╂煟閹邦喖鍔嬮柍閿嬪灴閺屾稑鈹戦崟顐㈠闂佸搫妫濇禍鍫曞蓟濞戞埃鍋撻敐搴′簼鐎规洖鐭傞弻鈥崇暆鐎ｎ剛袦闂佽鍠掗弲鐘参涢崘銊㈡婵☆垯璀﹂悗閬嶆⒒閸屾瑧鍔嶉柟顔肩埣瀹曟繂鐣濋埀顒佺珶閺囥埄鏁囬柣妯诲絻閻忓﹪姊洪崘鍙夋儓闁瑰啿娴烽惀顏囶樄闁哄瞼鍠撻埀顒佺⊕钃遍柍閿嬪姍閺?up闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇妤ｅ啯顎嶉梺绋款儏椤戝懘鍩為幋锔藉亹闁圭粯甯婇崰濠囨⒑閸忓吋绶叉繛纭风節瀵鈽夐姀鐘栥劍銇勯弮鍌氬付妞ゎ偀鏅濈槐鎾存媴閹绘帊澹曞┑鐘灱閸╂牠宕濋弽顓熷亗闁告劦鍠楅悡蹇擃熆鐠鸿櫣澧曢柛鏃€宀搁弻锟犲幢濞戞鍠氶梺璇″枦椤骞忛崨鏉戞嵍妞ゆ挾鍋涚紒鈺呮⒒娴ｅ憡鎯堥悶姘煎亰瀹曟劕螖閸涱厾鍔﹀銈嗗坊閸嬫挻绻濋埀顒佹綇閵婏箑寮块梺姹囧灪濞煎本寰勭€ｎ亞绐為梺褰掑亰閸橀箖宕㈤柆宥嗏拺闂傚牊涓瑰☉銏犵闁绘劦鍓氶惁鏃傜磽閸屾艾鈧嘲霉閸パ屾禆闁靛ě鍐ㄧ亰濠电偛妫楃换鎺楀汲閿曞倹鐓欓柟娈垮枛椤ｅジ鏌涚€ｎ亝顥滈棁澶愭煥濠靛棙绁╅柡鈧导瀛樼厸闁告稑娲ら崯鐘诲绩娴犲鐓熼柟閭﹀幗缂嶆垿鏌ｈ箛銉х暤闁哄瞼鍠栧畷銊︾節閸愩劉鍋撻幇鐗堢厸濞达絿顭堥弳锝夋煛娴ｇ懓濮嶇€规洖鐖奸崺鈩冩媴濮濆睗鏇㈡⒒閸屾瑧顦﹂柟纰卞亰钘濆ù鍏兼綑閸ㄥ倿鏌ｉ弮鍌氬付闁告垹濞€楠炴牕菐椤掆偓婵′粙鏌ｉ幘鍗炲姦闁哄矉缍佸顒勫箰鎼淬垹鍓甸柣搴㈢⊕缁诲倿鍩為幋锔藉€烽柛娆忣槸濞咃繝姊洪幖鐐插缂侇喗鐟ラ锝囨嫚濞村顫嶅┑顔筋殔濡瑩宕撻悽鍛娾拺缂備焦蓱鐏忣厽绻涢幘顔芥暠缂佸倹甯￠、姗€鎮㈤搹鍦闂備礁鍟块惃婵嬪磻閹惧灈鍋撻獮鍨姎婵炲眰鍔戝鎶藉Χ婢跺鈧灚绻涢崼婵堜虎婵炲懏锕㈤弻娑㈠箻鐠虹儤鐏堥梺闈涙缁€渚€锝炲鍫濈劦妞ゆ帊妞掔换鍡樻叏濠靛棛鐒炬俊鍙夊劤椤啴濡惰箛鎾舵В闂佺顑呴敃銈夛綖?
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
        // up 闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇炲€归崕鎴犳喐閻楀牆绗掔紒鈧径灞稿亾閸忓浜鹃梺閫炲苯澧撮柛鈹惧亾濡炪倖甯婄粈渚€宕甸鍕厱婵炲棗绻愰弳锝夋煏閸℃洜顦﹂摶鏍煃瑜滈崜鐔煎灳閿曞倸惟闁宠桨绀佺粣娑橆渻閵堝棙顥嗗┑顔哄€濋、娆撳炊椤掍讲鎷绘繛杈剧到閹诧繝宕悙鐢电＜闁告繂瀚崑銉р偓瑙勬礃濞茬喎鐣峰Ο濂藉湱鈧綆鍋嗘禍娆撴⒒娓氣偓濞佳呮崲閸儱纾归柡宥庡幗閸嬪倹绻涢幋鐑囦緵闁绘柨妫濋幃鐟懊洪鍡欑獥闁诲孩鐭崡鎶藉蓟閿涘嫧鍋撻敐搴濈盎妞ゆ帇鍨介弻鐔哥瑹閸喖顬堥柧浼欑悼缁辨帞鈧綆浜炴禒銏°亜椤掆偓椤﹁京妲愰幘瀛樺缂佸绨遍崑鎾诲捶椤撶喐娈伴梺璺ㄥ枔婵挳寮告笟鈧弻鐔兼偋閸喓鍑￠梻浣稿船濞诧妇鎹㈠☉銏犵闁绘劘灏欓悷銊︾箾鐎涙鐭掔紒鐘崇墵瀵鈽夐姀鐘电杸闂傚倸鐗婄粙鎺楁倶閸儲鐓熼幖娣灱婢规ɑ銇勯敃鍌欐喚闁糕斁鍋撳銈嗗坊閸嬫挾绱掗悩鑼х€规洘娲熼弻鍡楊吋閸涱垳鏋冮梺鐟板悑閹矂鎮℃径宀€绀婇柡宥庡幗閻撳繘鏌涢锝囩畺缂佲偓瀹€鍕厱閻庯綆鍋呭畷宀€鈧娲滈崗姗€銆佸Ο娆炬Ь闂佸憡姊归幃鍌炲箖濡も偓閳绘捇宕归鐣屼粚闂備胶纭堕弲婊堟偋閻樿尙鏆︽慨姗嗗幘缁♀偓闂佹悶鍎崝宥夊磿椤忓牊鐓熼柣鏂挎憸閹冲啴鎮楀鐓庡⒋闁诡垰鍟撮、鏇㈡晜鐟欙絾瀚奸梻浣告啞缁嬫垿鏁冮妶澶嬪€堕柣鏂垮悑閻撴洖鈹戦悩鎻掓灓婵炲牊绮撻弻锝呪槈閸楃偞鐝濇繝娈垮枓閸嬫捇姊洪棃娑氱疄闁搞劌顭烽弫宥咁煥閸啿鎷洪梺鍛婄箓鐎氼剟寮抽悙鐑樼厱闁靛ě鍕瘓闂佽鍠楅敃銏ゅ箖閳哄懎鍨傛い鎰╁灪閻庨箖姊绘担铏瑰笡妞ゃ劌鎳橀幃褑绠涢幘浣芥閳ь剚绋掗敃鈺佲枔娴犲鐓熼柟閭﹀幗缂嶆垶绻涢幖顓炴灈闁宠鍨块、娆撴偡妫颁礁顥氶梻鍌氬€烽悞锔锯偓绗涘懐鐭欓柟瀵稿Л閸嬫挸顫濋妷銉ヮ潎閻庤娲橀崹鍧楀箖濞嗘挻鍊绘俊顖炴敱鐎氬ジ姊绘担鍛婂暈缂佸鍨块弫鍐晝閸屾氨鐤囬梺褰掑亰閸庮亪鎮烽柇锔惧弳闂佸憡渚楅崢鍝勎ｉ幇鐗堝€垫繛鍫濈仢濞呮﹢鏌涚€ｎ亷鏀诲ǎ鍥э躬閹虫粓妫冨☉姘辩嵁濠电姷鏁告慨瀵糕偓姘嵆閹即骞樼紒妯锋嫽婵炴挻鍩冮崑鎾寸箾娴ｅ啿娉氶崶顒夋晣闁靛繒濮烽敍鐔兼⒒娓氬洤澧紒澶庢硾閻ｅ灚绗熼埀顒勫蓟閻斿憡缍囬柛鎾楀懏娈哥紓鍌欒兌婵绱炴繝鍥х畺婵°倐鍋撻柍钘夘槸閳诲秹鎼归銏患缂備緡鍠楀銊╁箲閸曨垰惟鐟滃繘鏁嶅鍫熺厸濠㈣泛锕︽晶鎴︽煕閺傛寧鎹ｇ紒顔碱煼瀵粙顢曢妶鍥风床闂佽鍑界紞鍡樼鐠轰綍锝夊川婵炲じ绨婚梺鐟板⒔閸嬨劑宕戦姀鈶╁亾鐟欏嫭绀€闁靛牊鎮傞妴渚€寮撮姀鈩冩珳闂佹悶鍎弲娑㈢嵁閳ь剟姊婚崒姘偓鎼佸磹妞嬪孩顐芥慨姗嗗墻閻掍粙鏌ゆ慨鎰偓鏍偓姘煼閺岋綁寮崒姘粯缂備讲鍋撳璺哄閸嬫捇宕楁径濠佸闂備礁鎲″ú锕傚礈濞嗘劗顩峰┑鍌氭啞閳锋垹绱撴担鐧镐緵闁绘帞鏅槐鎺楊敋閸涱厾浠搁梺璇″櫙缁绘繂鐣烽幒鎴旀敠闁诡垎鍌氼棜婵°倗濮烽崑娑㈡倶濠靛绀夋慨妯垮煐閻撴瑩鏌涢幇顖氱毢缂佺姾宕甸埀?
        // 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晜閽樺缃曢梻浣虹《閸撴繈鎮疯閹矂骞樼紒妯衡偓鍨箾閹寸儐浼嗛柟杈剧畱閻鐓崶銊р姇闁绘挾鍠栭弻锟犲磼濮樺彉铏庨梺璇″枟閸ㄥ潡寮诲☉娆愬劅闁挎稑瀚呰閺岋紕浠﹂崜褎鍒涢悗娈垮枟閹歌櫕淇婇幖浣肝ㄩ柕澹苯鏅梻浣筋嚙濮橈箓锝炴径濞掓椽寮介鐔蜂函婵犻潧鍊婚…鍫ユ嫅閻斿摜绠鹃柟瀵稿€戝璺哄嚑閹兼番鍨荤粻楣冩煕閳╁厾顏堟倶閳哄啯鍋栭柨鏇炲€归埛鎺戙€掑锝呬壕濠电偠澹堝畷鐢垫閻愬鐟归柍褜鍓濆Λ銏ゆ⒑鐟欏嫬绀冩い鏇嗗懐涓嶆い鏍仦閻撶喐淇婇婊呭笡闁靛洨鍠栭弻鈩冩媴閸濄儛锝夋煙娓氬灝濡兼い顐ｇ矒瀹曞崬螖閳ь剟锝為锝囩閻庣數顭堢敮鍫曟煟濡や焦灏い?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋娆忕仾闁稿孩鍨块弻锛勪沪鐠囨彃濮庨柛銉︽尦濮婅櫣鍖栭弴鐐测拤濡炪們鍔岄敃銈堢亱闂佺粯鍨兼慨銈夊煕?闂?
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


// WARP 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠Χ閸屾矮澹曞┑顔矫畷顒勫储鐎电硶鍋撶憴鍕妞ゎ偄顦遍埀顒勬涧閵堢顕ｉ崼鏇炵闁绘ê鐏氬В搴㈢節閻㈤潧浠﹂柟绋款煼瀹曟椽宕橀鑲╋紱闂佺懓澧界划顖炴偂濞嗘劑浜滈柡鍌濇硶娴犮垻鎮顑芥斀闁绘劙娼х敮鍓佺磼缂佹﹫鑰跨€规洘妞介弫鎾绘偐閼碱剨绱叉繝鐢靛仦閸ㄥ爼鎮烽鐐茶Е闁稿瞼鍋為埛鎺楁煕鐏炲墽鎳呮い锔奸檮缁绘繈鍩€椤掑嫭鐒肩€规挶鍎遍ˇ鐢哥嵁閸ヮ剦鏁勯梺娆惧灠娴滈箖鏌熼悜姗嗘當闁圭鍩栭妵鍕箻鐎电硶妲堟繛瀛樼矒缁犳牕顫忔繝姘唶闁绘梹浜介埀顒佸浮閺岋綁骞掗幋鐐村枑闂侀€炲苯澧叉い顐㈩槸鐓ゆ俊顖氬悑瀹曟煡鏌涢銈呮瀺缂佽妫濋弻鐔兼⒒鐎靛壊妲柛鐑嗗灦濮婃椽骞愭惔銏紩闂佺顑嗛幑鍥蓟濞戙垹绠婚柟棰佽兌閸氬鈹戦纭峰伐闁圭⒈鍋嗛崣鍛磽閸屾瑧鍔嶉柨姘舵煃鐠囪尙绠绘慨濠冩そ瀹曘劍绻濋崘顏勫汲闂備焦瀵уú锔界椤忓嫮鏆︽繛宸簻閻掑灚銇勯幒宥夋濞存粍绮撻弻鐔煎传閸曨剦妫炴繛瀛樼矒缁犳牕顫忛搹鍦煓闁告牑鈧厖绱ｇ紓鍌氬€哥粔鎾晝椤忓牏宓侀柛鎰靛枛鍞梺鍐叉惈閸婄懓鈻嶅鍫熲拺闁硅偐鍋涢崝姗€鏌涢弬璺ㄧ劯妞ゃ垺鎸搁埥澶愬閳锯偓閹疯櫣绱撻崒娆戝妽闁挎洍鏅涢埢鎾诲籍閸喓鍘藉┑掳鍊愰崑鎾绘煥閺囥劋閭鐐插暙铻栭柛婊€鐒﹂弲鈺佲攽?data\.warp-enabled闂?
//   闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃繘鍩€椤掍胶鈻撻柡鍛箘閸掓帒鈻庨幘宕囶唺濠德板€撻悞锕€鈻嶉弬搴撴斀闁绘ê鐏氶弳鈺佲攽椤曞懎寮€?  -> 闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇楀亾妞ゎ亜鍟村畷褰掝敋閸涱垰濮洪梻浣侯潒閸曞灚鐣剁紓浣插亾濠㈣泛澶囬崑鎾荤嵁閸喖濮庡銈忓瘜閸ㄨ泛顕ｆ导鏉懳ㄩ柨鏂垮⒔椤旀洟姊洪悷閭﹀殶闁稿鍠栭妴鍌炲蓟閵夛妇鍘搁梺绯曞墲椤洭鎯岄幒妤佺厵妞ゆ梹鏋婚懓鍧楁煙椤旂晫鎳囨俊顐㈠暙閳藉螖閳ь剟藟濮樿京纾?WARP 闂傚倸鍊峰ù鍥敋瑜忛埀顒佺▓閺呯娀銆佸▎鎾冲唨妞ゆ挾鍋熼悰銉╂⒑閸︻厼鍔嬫い銊ユ噽婢规洘绻濆顓犲幍闂佸憡鎸嗛崨顓狀偧闂備胶绮幐楣冨窗閹版澘桅闁告洦鍨扮粻娑欍亜閺傚灝鎮戦柣搴ｇ帛缁绘稓鈧稒顭囬惌鎺旂磼閻樺磭澧い顐㈢箰鐓ゆい蹇撳椤斿洭鏌熼懝鐗堝涧缂佹彃鎼嵄闁割偁鍎查埛鎴犵磽娴ｇ櫢渚涙繛鍫熸礋閺岀喖鎮烽悧鍫熸倷闂佷紮绲块崗姗€鐛惔銊﹀癄濠㈣泛鐭堥崯鍥р攽閻愯埖褰х紒鏌ョ畺閹椽寮撮埥鍡樻緭濠电姷顣槐鏇㈠磻閹达箑纾归柡宥庡幗閸嬪淇婇妶鍛殭闁哄鐗忛埀顒€绠嶉崕閬嵥囬婊冾棜鐟滅増甯楅埛鎺楁煕椤愩倕鏋欓柛搴㈠姍楠炴牕菐椤掆偓婵″ジ鏌ｉ幇顒婅含闁哄本娲樼换娑滎槻闁硅櫕鍔欏鎶筋敃閿旇В鎷洪柣鐘叉处閻擄繝顢撳Δ鍛厽闊洦妫佹竟姗€鎽堕悙鐑樼厱闁逛即娼ч弸娑㈡煛?
//   濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰惈閸ㄥ倿鏌涢锝嗙缂佺姳鍗抽弻娑樷攽閸曨偄濮㈤梺娲诲幗閹瑰洭寮婚悢铏圭＜闁靛繒濮甸悘宥夋⒑缁嬫鍎嶉柛濠冪箞瀵寮撮悢铏诡啎闂佺粯鍔﹂崜姘舵偟閺冨牊鈷?-> 濠电姷鏁告慨鐢割敊閺嶎厼绐楁俊銈呭暞閺嗘粍淇婇妶鍛殶闁活厽鐟╅弻鐔兼倻濡晲绮堕梺閫炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劍銇勯弮鍥撴繛鍛Т閳规垿鎮╅崹顐ｆ瘎婵犳鍠氶弫濠氬箠濠靛绀堢憸蹇涙儗閸℃稒鐓曠€光偓閳ь剟宕戦悙鐑樺亗婵炲棙鎸婚悡娆愩亜閺嶃劎鈯曠紒鎯板皺閹喖顫濋懜纰樻嫼闂佸湱顭堝ù鐑藉煀閺囩喍绻嗘い鎰╁灩閺嗭絿鈧鍠栭…閿嬩繆閹间礁唯闁靛繆妲呭鏃€绻濈喊妯活潑闁搞劍濞婂畷銏ｎ樄闁靛棗鍊婚幉鎾礋椤忓棛鐣鹃梻渚€娼ч悧鍡椕洪妶鍛懃濠电姷鏁搁崑娑樜熸繝鍥у偍濠靛倸鎲￠崑鈺呮煟閹寸姷鎽傞柡浣稿暣閺屻劌鈹戦崱妯虹獩闂佺娅曠换鍫濐潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯閸楁娊寮诲☉銏犵闁规崘娅曠拠鐐差渻閵堝啫鐏紒瀣灴閳ワ箓濡搁埡浣侯槹濡炪倖鍔﹂崑鈧柛瀣尭閳藉濮€閳锯偓閹锋椽姊婚崒姘卞缂佸鎸婚弲鍫曞即閻旇櫣顔曢柣蹇曞仜閸婃悂鍩€椤掍緡娈滈柟顔炬焿椤﹁鎱ㄦ繝鍕笡闁瑰嘲鎳愮划鐢碘偓锝庝簼閻ｄ即姊绘担渚劸缂佺粯鍨圭划娆撳箻閺傘儲鐎婚梺闈涚箞閸婃洖娲块梻浣规偠閸庢椽鎮￠崼婵愮劷闁绘柨鍚嬮埛鎺戙€掑顒佹悙濠⒀屽枤缁辨帗寰勬繝搴℃闂佺懓寮堕幐鎶藉极閸屾稒鍙忛柟閭︿簽閻╁酣姊绘担鍛婃儓缂佸鐓″畷婵婄疀閺冨倻褰惧┑鈽嗗灠閸氬宕戦弽顓熺厽闁哄倸鐏濋幃鎴︽煃闁垮鐏撮柡灞剧☉閳规垿宕卞Δ濠佹偅闂備礁鎼Λ娑㈠磻閵堝钃熸繛鎴炵懄閸庣喖鏌曢崼婵囧櫢婵顨婂娲焻閻愯尪瀚板褜鍨堕弻娑樷攽閸℃稑寮伴悗瑙勬礀缂嶅﹪骞愭繝鍐ㄧ窞閻忕偟鍋撻鏇㈡⒒娴ｇ懓顕滅紒璇插€胯棟妞ゆ牗绮庢稉宥夋煥濠靛棭妲归柣鎾跺枑娣囧﹪顢涘鍙樿檸闂佹眹鍔嶅浠嬪蓟閿濆围闁稿本鐭竟?WARP 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵囩節閸屾粌顣甸梺閫炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙娆炬綂闂佺偨鍎遍崯璺ㄨ姳閵夛妇绡€闁汇垽娼ф禒锕傛煕椤垵鐏︾€规洜鎳撶叅妞ゅ繐瀚敍鐔兼⒑閻熸澘鈷旂紒顕呭灦閹繝宕橀鍛瀾濠电姴锕ら悧鍡欑矆閸喓绠鹃柟瀵稿仧閹冲懏銇勯埡鍌滃弨闁哄本绋戦埥澶婎潨閸喐鏆扮紓鍌氬€哥粔鏉懨洪銏犺摕闁哄洨鍠撶弧鈧棅顐㈡处閹尖晛煤椤撱垺鈷戦柛娑橈攻閳锋帡鏌涜箛鏃撹€挎鐐插暙铻栭柍褜鍓熼崺銏ゅ箻鐠囨彃鐎銈嗘⒒閺咁偅绂嶉幇鐗堚拻濞达絼璀﹂悞鐐亜閹存繃顥㈡鐐村灴瀹曞爼顢楅埀顒勬嫅?
// 闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇楀亾妞ゎ亜鍟村畷褰掝敋閸涱垰濮洪梻浣侯潒閸曞灚鐣剁紓浣插亾濠㈣泛澶囬崑鎾荤嵁閸喖濮庡銈忕細閸楀啿鐣烽幋鐐茬窞闁归偊鍘鹃崣鍡椻攽閻樼粯娑ф俊顐ｇ懄瀵板嫰宕熼鐔告杸濡炪倖鐗楅崫搴ㄥ磻閵忋倖鐓涚€光偓鐎ｎ剙鍩岄柧缁樼墵閺屽秷顧侀柛鎾跺枛楠炲啳顦崇紒缁樼箞瀹曞爼濡搁妷銏犱壕闁割煈鍋佹禍婊堟煛閸愶絽浜剧紓浣疯兌椤㈠﹪濡甸幇鏉跨闁规儳鍘栧Ч妤呮⒒娴ｅ憡鍟為柛鏃€鐗為妵鎰板礃閳哄倸寮块梺褰掓？閻掞箓鍩涢幋锔解拺妞ゆ劑鍊曟禒婊堟煠濞茶鐏￠柡鍛埣椤㈡瑧浜搁弽銊︽澑闂備胶顭堥張顒傜矙閹烘鐓曢柟鐑樻⒒绾惧吋銇勯弴鐐村櫣妤犵偞顨堥埀顒€鐏氬妯尖偓姘嵆閻涱噣宕堕澶嬫櫌闂侀€炲苯澧い鏇到閻ｏ繝鏌囬敂钘夌槣闂備線娼ч悧鍡椢涘☉娆愭珷妞ゆ帒鍊荤壕濂告偣閸ヮ亜鐨洪弫鍫ユ倵?JSON闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇閸撗呮殸闁诲孩鑹鹃ˇ浼村Φ閸曨垰绠抽柛鈩冦仦婢规洟姊洪崨濠庢疁濞存粌鐖煎璇测槈濡攱顫嶅┑顔筋殔閻楀棙鎱ㄥ☉銏♀拺鐎规洖娲ㄧ敮娑欎繆椤愩垹鏆欐い鏇秮瀹曞ジ寮撮悢灏佸亾閻戣姤鐓涘璺侯儏閻掔偓淇婇銏☆棦婵﹥妞藉畷顐﹀Ψ閵夛妇鈧姊洪幐搴㈠濞存粠浜滈?.autostart-off 濠电姷鏁告慨鐑藉极閹间礁纾块柟瀵稿Т缁躲倝鏌﹀Ο渚＆婵炲樊浜濋弲婊堟煟閹伴潧澧幖鏉戯躬濮婃椽宕ㄦ繝鍌毿曟繛瀛樼矋閻楃姴鐣烽幋锕€绠婚悹鍥ㄥ絻閻庮厼顪冮妶鍡楀Ё缂傚秴妫楄灋闁告劦鍠楅崐鐢告偡濞嗗繐顏痪鐐倐閺屾稒鎯旈妶鍡欏涧缂備礁鍊圭敮锟犲极閸愵喖纾兼繛鎴炆戦悾浼存⒒娴ｇ鏆遍柟纰卞亰閺佸啴鏁愰崶顭戞綗濠碘槅鍨甸崑鎰娴犲鐓曢悘鐐插⒔閹冲懏銇勯敂濂告闁靛洤瀚伴弫鍌滄嫚閼碱兛鍖栫紓鍌欑贰閸犳顫忔繝姘劦妞ゆ帊鑳堕埊鏇熴亜椤撶偞鍠橀柕鍡楁嚇閺屻劎鈧綆鍓涢鏇㈡⒑閻熼偊鍤熼柛濠冨灥椤繄鎹勯崫鍕闂備礁鎼粙渚€宕㈡禒瀣亗婵炴垯鍨洪悡鏇㈡煏婢舵稓鍒板褍鎲￠幈銊ノ熼幐搴ｃ€愮紒?
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

// 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閼碱剦妲遍柣鐔哥矌婢ф鏁幒妤€鍨傞柛宀€鍋為悡鐔镐繆閵堝倸浜鹃梺缁橆殔閿曨亜鐣烽敐鍫㈢杸婵炴垶鐟ч崢鎾绘⒑閸涘﹦绠撻悗姘煎弮瀹曟娊宕烽鐘碉紲濡炪倖鍔戦崹缁樻櫏闂備礁鎼悮顐﹀磹濠靛宓侀柡宥冨妽婵挳鎮峰▎蹇擃伃闁轰緡浜缁樻媴閻熸壋鏋欓梺琛″亾闁告鍊ｅ☉銏犵妞ゆ棁鍋愰悾?濠电姷鏁告慨鐢割敊閺嶎厼绐楁俊銈呭暞閺嗘粍淇婇妶鍛殶闁活厽鐟╅弻鐔兼倻濡晲绮堕梺閫炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劍銇勯弮鍥撴繛鍛Т閳规垿鎮╅崹顐ｆ瘎婵犳鍠氶弫濠氬箠濠靛绀堢憸蹇涙儗閸℃稒鐓曠€光偓閳ь剟宕戦悙鐑樺亗婵炲棙鎸婚悡娆愩亜閺嶃劎鈯曠紒鎯板皺閹喖顫濋懜纰樻嫼?闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔兼倻濡櫣鍔稿┑鐐茬毞閺呯娀寮婚弴鐔虹闁绘劦鍓氶悵鏃堟⒑閸濆嫷鍎庣紒鑸靛哺瀵寮撮姀鐘诲敹濠电姴鐏氶崝鏍懅缂傚倸鍊烽懗鑸靛垔鐎靛摜绀婂ù锝呭濞兼牗绻涘顔荤盎闁搞劌鍊归妵鍕即閻愭潙娅ｉ梺鍛婃尭閸熷潡鍩為幋锔绘晩缁绢厾鏌夐崥顐ょ磽娴ｅ搫校闁绘濮撮锝囨嫚濞村顫嶅┑顔筋殔濡瑩宕撻悽鍛娾拺缂備焦蓱鐏忣厽绻涢幘顔芥暠缂佸倹甯￠、娑㈡倷缁瀚奸梺鍝勵槺閸嬫﹢宕￠崘宸€堕柣鎴ｅГ閻撴洖鈹戦悩鎻掓殶濠⒀屼簼閵囧嫰顢橀悙鏉戭槱缂備胶绮换鍌烇綖濠靛鏁嗛柛灞惧焹閸嬫捇鎮欓悜妯锋嫼闂傚倸鐗婃笟妤€危閸洘鐓曢柕濠庣厛濞兼劗绱掗崒姘毙ф鐐叉閳ユ牠鍨惧畷鍥┬ㄩ梺杞扮劍閸旀牕顕ラ崟顐嬬喎煤缂佹﹩妫濈紓?WARP 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵囩節閸屾粌顣甸梺閫炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙娆炬綂闂佺偨鍎遍崯璺ㄨ姳閵夛妇绡€闁汇垽娼ф禒锕傛煕椤垵鐏︾€规洜鎳撶叅妞ゅ繐瀚敍鐔兼⒑閻熸澘鈷旂紒顕呭灦閹?-> 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晝閳ь剛澹曟繝姘厱闁斥晛鍟伴埥澶愭煕鐏炶濮傞柡灞诲€曢湁閻庯綆鍋呴悵鏇烆渻閵堝骸浜濇繛鑼枛瀵鎮㈤崨濠勭Ф闂佸憡鎸嗛崨顔筋啅闂?
// 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愯弓鐢绘俊鐐€栭悧妤冪矙閹炬眹鈧懘宕ｆ径宀€鐦堟繝鐢靛Т閸婃悂寮搁敃鍌涚厾闁绘鐗嗛婊堟煏閸パ冾伂缂佺姵鐩獮妯兼崉娓氼垱姣囬梻浣藉吹閸犳劙鎮烽柆宥嗏挃闁告洦鍨版闂佸憡娲﹂崹浼村礃閳ь剟姊洪棃娑辩劸闁告柨鐭傝棢婵犻潧顑嗛崑鐘虫叏濡搫鑸归柡瀣⊕閹便劍绻濋崘鈹夸虎閻庤娲﹂崑濠傜暦閻旂⒈鏁冮柨婵嗘－閸熷牆鈹戦悩鍨毄闁稿鐩棟闁规鍠氶惌娆撴煙閻戞ɑ鈷掗柣顓烆樀閺岀喖鎮滃Ο铏逛憾濠碘剝褰冮悧濠勬崲濞戙垹骞㈡俊顖濇娴滃爼姊洪崫鍕靛剱闁绘濮撮～蹇撁洪鍛姷闂佺粯鍔樼亸顏嗏偓姘緲椤儻顧侀柛锝忕秮瀵顓奸崶銊ョ彴闂佸搫琚崕鍗烆嚕娴煎瓨鈷戠紒顖涙礃閺夋椽鏌涙惔銊︽锭闁伙絿鍏樻俊鎼佸煛婵犲啯娅栨繝鐢靛Т閿曘倗鈧稈鏅涢弳鈺傜節閻㈤潧啸闁轰焦鎮傚畷鎴︽倷閸濆嫬鐎梺鍓插亝濞叉牜绮绘导瀛樼厱婵犻潧瀚崝銈夋煟椤撶喐宕岄柡宀嬬秮婵偓闁靛繆鏅濋崝绋库攽閳藉棗浜濋柨姘舵婢舵劖鐓ユ繝闈涙婢ц鲸绻涢崗鐓庡闂囧绻濊閻忔繈鍩€椤掍胶澧遍柟骞垮灩閳藉濮€閻樻鍚呴梻浣虹帛閸ㄩ潧螞濞戙垹绀夐柣鎴ｅГ閳锋垿鏌涘┑鍡楊仾妞ゃ儲鐟╅弻娑樜旀担绯曟灆閻庢鍠栭…鐑藉极閹剧粯鍋愰柤纰卞墻濡茬増淇婇悙顏勨偓鏍箰妤ｅ啫绐楅柟鎹愵嚙杩濋梺褰掑亰閸忔﹢寮ㄦ禒瀣厽闁归偊鍓欑痪褔鏌涢敐鍡樸仢闁诡喕绮欓、娑㈠Χ閸モ晝妲囬柣搴㈩問閸ｎ噣宕戦崟顖ｆ晣濠靛倻顭堝婵囥亜閺嶃劎鈯曢柣锝囧厴濮婄粯鎷呴崨濠傛殘濠电偠顕滈梽鍕矉瀹ュ鍊烽柛顭戝亽濞肩喎鈹戦绛嬬劸闁糕晜鐗犻崺娑㈠箣閻樼數锛滈柣搴秵閸樼晫娑甸崜浣虹＜闁绘ê鍟块ˉ瀣磼鏉堛劌绗氱€垫澘瀚埀顒婄秵閸嬪嫰宕径濞㈡棃鎮╅棃娑楃捕缂備胶绮崹褰掑焵椤戣法绁烽柛瀣姍閸┾偓妞ゆ帊鑳堕埊鏇㈡煥濮橆優褰掓偐閾忣偁浠㈠┑顔硷攻濡炶棄螞閸愩劉妲堥弶鍫氭櫅瑜板绱撻崒娆掑厡濠殿喚鏁诲畷褰掑锤濡も偓缁犳牠鏌ㄩ悢鍝勑㈤柛妤佸▕閺屾洝绠涢妷褏锛熼梺鍛婎焽閺佽顫忔繝姘＜婵ê宕·鈧┑鐐存尰绾板秹銆冩繝鍌滄殾闁哄浄绱曢悿鈧梺鍦檸閸ㄩ亶寮埀顒佷繆閻愵亜鈧牕螞娓氣偓閿濈偞寰勬繛鐐€洪梺绯曞墲椤洨绮绘ィ鍐╃厽闁规儳鍟块惁銊╂煟閹剧偨鍋㈤柡宀€鍠栭、娆撴偩鐏炴儳娅氶柣搴㈩問閸犳牠鎮ユ總鍝ュ祦閻庯綆浜栭弨浠嬫煙闁箑甯ㄩ柍鍝勬噺閳锋帡鏌涚仦鍓ф噯闁稿繐鐭傞弻鐔兼惞椤愶絽纾抽梺璇″枟閿氭い顐ｇ箘閹瑰嫰鎼圭拠鈥虫暪婵犵數濮烽弫鍛婃叏閻㈤潧鏋堢€广儱顦壕褰掓煕椤愶絾绀冮柣鎾卞劦閺岋繝宕堕埡浣风捕闂侀€炲苯澧柣鈩冩礈閹?
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

// ---------------------------------------------------------------- WARP 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ磵閳ь剨绠撳畷濂稿Ψ椤旇姤娅堥梻浣告啞閸旓箓宕伴弽顐㈩棜闁秆勵殕閳锋垶銇勯幒鍡椾壕缂備礁顦顓犲垝婵犳艾鍐€妞ゆ劗鍠庢禍鐐殽閻愯尙浠㈤柛鏃€纰嶉妵鍕晜鐠囪尙浠╁銈嗘煥缁绘劙鍩為幋锕€骞㈤煫鍥ㄦ⒐閺夋悂姊绘担铏瑰笡闁挎氨鈧鍠涢崺鏍礆婵犲啰闄勯柛娑橈功閸?
// 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰惈閸ㄥ倿鏌涢锝嗙缂佺姳鍗抽弻娑㈩敃閿濆棛顦ョ紒鐐礃椤绌辨繝鍥ч柛娑卞枛閻濇梻绱?WARP 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰惈閸ㄥ倿鏌涢锝嗙缂佺姳鍗抽弻娑樷攽閸曨偄濮㈤梺娲诲幗閹瑰洭寮婚敐澶婄闁挎繂妫Λ鍕磽娴ｆ彃浜鹃梺绋挎湰缁嬫帡宕ｈ箛鏂剧箚闁绘劙顤傞崵娆徝瑰鍫㈢暫婵?闂?
//   1) 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛鎾茬閸ㄦ繃銇勯弽顐汗闁逞屽墾缁犳挸鐣锋總绋课ㄩ柕澶涢檮琚ｉ梻鍌欑閹碱偆绮欐笟鈧畷銏＄附閸涘﹤鈧潡鏌涢…鎴濅簴濞存粍绮撻弻鐔衡偓鐢殿焾娴犳粍銇勯弴鐔虹煁闁靛洤瀚伴弫鍐磼閵堝棭妫栫紓?connected 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愯弓鐢婚梻浣瑰濞叉垿鎮為敃鍌氱疇鐎光偓閸曨兘鎷?disconnected      -> 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴鐐测偓鎼佹嫅閻斿吋鐓忓┑鐐靛亾濞呮捇鏌℃担绋款伃闁哄本绋戦埥澶娾枎閹邦喚鈻忛梻浣告惈鐎氥劑宕曢悽绋胯摕闁哄浄绱曢悿鈧柣搴秵娴滄牠宕戦幘璇插唨妞ゆ挾鍠庨埀顒冩硶缁辨挻鎷呴懖鈩冨灴瀹曪綀绠涢弬鍓х畾濡炪倖鐗楅崫搴㈢閹烘鐓曟慨妯煎帶娴滀即鏌＄仦鍓р槈闁宠娴锋禒锕傚箣閹烘埈妫?
//   2) 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛鎾茬閸ㄦ繃銇勯弽顐汗闁逞屽墾缁犳挸鐣锋總绋课ㄩ柕澶涢檮琚ｉ梻鍌欑閹碱偆绮欐笟鈧畷銏＄附閸涘﹤鈧潡鏌涢…鎴濅簴濞存粍绮撻弻鐔衡偓鐢殿焾娴犳粍銇勯弴鐔虹煂缂佽鲸甯楅幏鍛村传閸曞灚姣夐梻?connected 濠电姷鏁告慨鐑藉极閹间礁纾婚柣妯款嚙缁犲灚銇勮箛鎾搭棞缂佽翰鍊濋弻锕€螣閻氬绀嗗┑鐐村灦椤倿寮崼婵嗙獩濡炪倖鎸鹃崑鐐哄磿椤忓懍绻嗛柣鎰典簻閳ь剚鐗犻獮鎰板醇閺囩偛鐎悷婊呭鐢宕曞Δ浣风箚闁靛牆鎳忛崳娲煟閹惧崬鍔滅紒缁樼箞濡啫鈽夊▎妯伙紒婵＄偑鍊戦崕鑼垝閹捐绠栫痪顓炴媼濡插姊虹€癸附婢樻俊鍧楁煙楠炲灝鐏╅柍钘夘樀婵偓闁绘鏁婚悰鎾绘⒒娴ｇ瓔娼愰柟顖氱焸瀹曘垺绺界粙璺槶濡炪倖鏌ㄦ晶鐣屽閽樺褰掓晲閸偄娈愬┑鐐茬墛濮婂綊濡甸崟顖涙櫆闁绘劦鍓氬В鍫ユ倵濞堝灝鏋涙い顓炲槻閻ｇ兘濡搁埡濠冩櫍閻熸粌閰ｅ畷鎾绘偨閸涘ň鎷洪梺鍛婄☉閿曘儳鈧灚鐟╅弻娑橆煥閳ь剟鎮為敃浣插悁濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰惈閸ㄥ倿鏌涢锝嗙缂佺姳鍗抽弻娑㈩敃閿濆棛顦ラ梺鎼炲妼閸婂潡寮诲☉銏℃櫆閻犲洦褰冪粻浼存⒑缁嬫鍎愰柟鐟版搐閻ｇ柉銇愰幒婵囨櫇闂佺硶鍓濋敋闁诲繑鎸抽弻?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴妤€浜鹃柧鑽ゅ仱閻擃偊宕堕妸褉妲堥梺鎼炲妼閸婂潡寮婚悢鐓庣畾闁绘鐗滃Λ鍕磼閻愵剙鍔ゆい顓犲厴瀵濡搁妷銏℃杸闂佺硶鍓濋敋妞わ腹鏅犲铏圭矙濞嗘儳鍓遍梺缁橆殕椤ㄥ﹥淇婇悽绋跨妞ゆ牗姘ㄩ悿鈧梻浣稿閸嬪懎煤濮椻偓椤㈡挸顓奸崪浣瑰瘜闂侀潧鐗嗛幊搴敆閵忋倖鐓熼柣鏂垮级濞呭﹦鈧?
//      -> 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵嬫濞戝崬鍓扮紒鐐劤椤兘寮婚敐澶婃闁割煈鍠楅崐顖炴⒑缁嬫鍎嶉柛鏂挎捣濡叉劙骞樼€涙ê顎撴繝娈垮枟閸╁牊绂嶉悙娴嬫瀻闁靛繈鍊栭崐鐑芥煕濠靛棗顏Δ鐘叉喘濮婃椽宕滈幓鎺嶈埅闂佺顑呯€氼厾绮嬪澶樻晬婵犲﹤瀚禒?2 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋鐑嗙劯婵炴垶顭囬弳鍡涙煕閺囥劌浜炴い锔诲幖閳规垿鎮╃紒妯婚敪濠电偠顕滅粻鎴﹀煝瀹ュ鍋愮紓浣诡焽閸樿棄鈹戦埥鍡楃仴闁稿鍔楅弫顔尖槈閵忥紕鍘介梺鍦劋濮婅崵鏁☉銏＄厵濞撴艾鐏濇俊濂告懚閿濆拋鐔嗛柛鎾茬贰閻掗箖鏌熼姘卞ⅵ婵﹥妞藉畷銊︾節閸曨剙鏋﹂梻浣侯焾椤戝棝骞戦崶褜娼栫紓浣股戞刊鎾煟閻旂厧浜伴柛銈咁儑缁辨挻鎷呴崜鎻掑壉闂佹悶鍔屾晶搴ｇ磽閹惧顩烽悗锝庝簽閻嫰姊虹粙鎸庢拱缂侇喖閰ｉ幃?10 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫宥夊礋椤掍焦顔囬梻浣告贡閸庛倝宕甸敃鈧埥澶娢熼柨瀣澑闂佽鍑界紞鍡樼閻愪警鏁婇柛銉墯閳锋垿鎮峰▎蹇擃仼缂佲偓閸愨斂浜滈柡鍥ф閹虫劙鎯岄崱娑欑厱闁逛即娼ч弸鐔兼煟閹惧瓨绀冪紒缁樼洴楠炲鈹戦崱姘厴闂備礁鎼鍥窗鎼淬劍绠掗梻浣虹帛椤牏绮旈鈧、鏃堝醇濮橈絽浜鹃柨鏇炲€搁柨銈嗕繆閵堝嫮顦︽繛鍫熺箞濮婂搫效閸パ呬紕闂佽桨鐒﹂幃鍌氱暦閿濆應鏀介柛顐犲灮閿涙繃绻涙潏鍓хК妞ゎ偄顦甸崺娑㈠箛椤撶姷顔曢梺绋跨箺閸嬫劙骞婇崶顒佺厱闁崇懓鐏濋悘顏堟煙椤栨稒顥堝┑鈩冩倐婵＄柉顦茬€殿喓鍔戝缁樻媴閸涘﹤鏆堟繛鎾寸椤ㄥ﹤鐣疯ぐ鎺戠闁告挸鐏濆ú顓€佸璺虹劦妞ゆ巻鍋撴い顐㈢箳缁辨帒螣閼测晜鍤岄梻浣规偠閸庢粓宕橀悙顑挎唉闂傚倸鍊峰ù鍥敋瑜忛幑銏ゅ箣閻樺啿搴婇梺鍓插亝缁诲嫰寮抽敃鍌涚厪闊洤顑呴埀顒佹礀濞插灝鈹戦悩顔肩伇婵炲鐩弫鍐晝閸嬬懓娲、鏇㈡晜閽樺鍋撻悽鍛婄厽闁绘柨鎲＄欢鍙夈亜韫囷絼绨绘い顓炴健閹兘鎮ч崼顐ｎ棧婵?
// 闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇楀亾妞ゎ亜鍟村畷褰掝敋閸涱垰濮洪梻浣侯潒閸曞灚鐣剁紓浣插亾濠㈣泛澶囬崑鎾荤嵁閸喖濮庡銈忓瘜閸ㄨ泛顕ｆ导鏉懳ㄩ柨鏂垮⒔椤旀洟姊洪悷閭﹀殶闁稿鍠栭獮濠囧川鐎涙鍘撶紓鍌欑劍钃辨い銉ョ墦閺岀喖顢欓懖鈺冃ㄩ悗瑙勬礀閻栧吋淇婇幖浣肝ㄧ憸蹇涘Χ閻戣姤鐓熼柣鎰摠閻ㄦ垿鏌涢悩宕囧ⅹ妞ゎ偄绻愮叅妞ゅ繐瀚槐鍫曟⒑閸涘﹥澶勯柛瀣€婚懞杈ㄧ鐎ｎ偀鎷洪梺鍛婄☉椤剟鎮炴ィ鍐╃厱閻庯綆鍋勬慨鍥础闁秵鐓欓柣妤€鐗婄欢鑼磼閻欏懐绉柡灞诲姂瀵潙螖閳ь剚绂嶉崜褏纾?g_warpManualStop=true闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔衡偓鐢殿焾娴犙囨⒒閸曨偄顏柡宀嬬節瀹曟﹢濡搁妷銏犱壕闁煎鍊楁稉宥夋煛閸屾侗鍎ラ柣鏂挎閺屻倝骞栨担瑙勯敪婵犳鍠栧ú銊ф閹烘鍋愭い鏃傚帶婵箓姊洪崫鍕伇闁哥姵鐗犻獮鍐ㄢ枎閹垮啯鏅㈡繛杈剧秬濞咃綁鍩㈣箛鏂剧箚闁绘劦浜滈埀顒佸灴瀹曟洘娼忛埡鍐厠閻熸粎澧楃敮鎺楁嫅?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣椤愪粙鏌ㄩ悢鍝勑㈢痪鎯ь煼閺屾稑鈽夐崡鐐寸亾闂佸磭鎳撶粔褰掑蓟瑜戠粻娑橆潩閸忚偐绉烽柣?WARP 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閸愯弓鐢婚梻浣瑰濞叉牠宕愯ぐ鎺戠；閻庯綆鍠楅悡娑㈡煕閹扳晛濡奸柍褜鍓欓…宄扮暦閳ユ剚娼╅柤鎼佹涧閳ь剙鐏氱换娑㈠箣閻愬灚鍣介梺缁樺笂缁瑩寮?204 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柣鎴ｅГ閸婂潡鏌ㄩ弴妤€浜鹃柧鑽ゅ仱閻擃偊宕堕妸褉妲堥梺鎼炲妼閸婂潡寮婚悢鐓庣畾闁绘鐗滃Λ鍕磼閻愵剙鍔ゆい顓犲厴瀵濡搁妷銏℃杸闂佺硶妾ч弲婊呯懅闂佸搫顦弲婊呮崲閸繍娼栭柧蹇撴贡閻瑩鏌熺粙鍨劉闁圭柉浜槐鎾存媴缁涘娈柣搴㈠嚬閸犳岸宕氶幒鎾剁瘈婵﹩鍓濋幗鏇㈡⒑闂堟侗妾у┑鈥虫喘瀹?true 闂傚倸鍊搁崐宄懊归崶褏鏆﹂柣銏㈩焾绾惧鏌ｉ幇顔芥毄闁活厽鐟╅悡顐﹀炊閵娧€妲堢紓浣插亾濠㈣埖鍔曠粻瑙勭箾閿濆骸澧┑锛勫帶椤╁ジ宕ㄩ娑欐杸濡炪倖姊婚悺鏃堟倿閻愵剛绠惧璺侯儑閵嗘帞绱掗鑲╁ⅵ鐎规洜鍠栭、娑樷槈濮橆剙绠炲┑鐘垫暩婵炩偓婵炰匠鍏炬稑螖閸滀焦鏅滈梺鍐叉惈閸熸壆澹曢悾灞稿亾楠炲灝鍔氭俊顐ｇ懃閻ｅ嘲鐣濋崟顒傚幐闁诲繒鍋涙晶浠嬪煡婢舵劖鐓冮悹鍥ㄧ叀椤庢鎽堕敐澶嬬叄闊洦绋堥崑鎾斥槈濡警鍤欓梻鍌氬€搁崐宄懊归崶褜娴栭柕濞炬櫆閳锋梻鈧箍鍎遍ˇ顖滃鐟欏嫮绡€闂傚牊绋掗惌妤冪磼?
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
        if (st == 0) {                     // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋娆忕仾闁稿孩鍨块弻锛勪沪鐠囨彃濮庨柛銉︽尦閺岀喖宕楅懖鈺傛闂佺硶鏅涚€氼剝鐏嬮梺鍛婄☉閻°劑鍩涢幋锔藉仯闁搞儯鍔庨崣鈧銈呭椤ㄥ牏妲愰幒妤佸亹闁肩⒈鍎疯閳ь剝顫夊ú婊堝箠閹捐泛寮叉俊鐐€曠换鎰偓姘煎幖鍗辨い鎺嗗亾闁宠鍨块幃娆撳级閹寸姳妗撴繝娈垮枟鑿ч柛鏃€鍨甸悾鐑藉灳閹颁焦鍍靛銈嗘尰缁牏鑺辨繝姘拺闁告繂瀚ⅹ闂佺閰ｆ禍鍫曞春閳ь剚銇勯幒宥囪窗婵炲牊顭囩槐鎺楊敊閼测晛顤€閻庡灚婢樼€氼厾鎹㈠☉銏犵闁瑰濮锋闂備浇顕栭崰鎺楀疾閻樻眹鈧線寮撮～顔剧◤濡炪倖鎸鹃崕鎰涘鍐ｆ斀闁绘ê鐏氶弳鈺呮煕鐎ｎ剙浠辩€规洖缍婂畷濂稿即閻愯埖鎲伴梻渚€娼ц墝闁哄懏绮撳畷鎴犫偓锝庡枟閻撴瑩鏌熺憴鍕Е闁搞倖鐟х槐鎺楀焵椤掑嫬纾兼繛鎴烆殘缁犳艾顪冮妶鍡欏缂佽鍊垮畷銉ㄣ亹閹烘挾鍘撶紓鍌欑劍钃遍柍閿嬪浮閺屽秷顧侀柛鎾寸懅缁顓兼径濠勶紵闂備緡鍓欑粔瀵哥不閻樼鍋撻獮鍨姎婵炲眰鍊濆畷?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晝閳ь剛澹曟繝姘厱闁斥晛鍟伴埥澶愭煕鐏炶濮傞柡灞诲€曢湁閻庯綆鍋呴悵鏇烆渻?
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
constexpr wchar_t kAppVersion[] = L"3.19.0";

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
    // 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾鐎规洏鍎抽埀顒婄秵閸忔﹢宕戦幘鍓佺當婵炴垶蓱閸ｅ綊鏌涢妸锔剧疄闁诡喗锕㈤幃娆撳垂椤愶絿褰ч梻浣告惈濡绮婚幘璇茶摕闁挎繂顦猾宥夋煕鐏炴崘澹樺ù鐘愁焽缁辨挻鎷呴崫鍕戯絾绻濋姀鈽呰€块柍銉畵瀹曠螖娴ｅ憡鐤傞梻鍌氬€搁悧濠勭矙閹达綆鏉烘繝鐢靛Х閺佹悂宕戝☉銏″仱闁靛鍎嶅ú顏呭亜闁绘挸娴烽悾鍝勨攽鎺抽崐鎰板磻閹剧粯鐓冮柦妯侯樈濡叉悂鏌嶇拠鏌ヮ€楅摶锝夋煠婵劕鈧牕鈻撴ィ鍐┾拻闁稿本鐟﹂ˇ椋庣磼妲屾牕娅嶆慨濠佺矙瀹曠喖顢橀悢宄扮哎闂備礁澹婇崑渚€宕曢崘鑼殼濞撴埃鍋撻柡宀€鍠栭幃婊冾潨閸℃鏆ョ紓鍌欒兌婵灚绻涙繝鍥ц摕闁绘棁銆€閸嬫捇鎮藉▓璺ㄥ姼闁哥喓顭堥—鍐Χ韫囨洜蓱闂佺粯鎸撮埀顒佸墯濞兼牗绻涘顔荤盎缂佺姳鍗抽獮鏍ㄦ綇閸撗冪煗婵犳鍠栧ú銊ф閹烘鍊锋い鎺嶈兌閸戯紕绱撴担鍓叉Ш闁硅櫕鎹囬、姘舵晲婢跺﹦顔呴梺鍏间航閸庤鲸绂嶅鍫熲拺缂備焦蓱鐏忣厽绻涚€电鍘存い銏＄懄瀵板嫭绻濇惔銏犵槣闂備線娼ч悧鍡椢涘▎鎾嶅宕卞缁樻杸濡炪倖鏌ㄩ幖顐﹀焵椤掍胶绠炴鐐插暙铻栭柛婊€鐒﹂弲鈺佲攽閻愬瓨缍戝Δ鐘叉憸缁辩偞绻濋崶褏鐣洪梺鍐叉惈閹冲繘宕愮紒妯圭箚妞ゆ牗绮犻崕鐘绘煃瑜滈崜姘跺礉濞嗘挸绠栫€瑰嫰鍋婇悡銉╂煕濞戝崬寮鹃柡鍡╁弮濮婅櫣绱掑Ο缁樺創缂備胶绮换鍫ユ偘椤旂晫绡€闁稿濮ゅΛ鍐ㄧ暦閵娾晩鏁婇柛蹇氬亹閺嬧偓闂傚倸鍊风欢姘焽瑜旂瘬闁逞屽墰缁辨帡鎮╅搹顐㈢濡炪値鍓欓敃顏堝箖濠婂牊鍤嶉柕澶堝灪鐎氬ジ姊绘担铏瑰笡闁圭鎽滈埀顒€鐏氱敮鎺楀煝閹剧粯鍋愰悷浣疯兌閹虫捇鈥﹂妸鈺佺妞ゆ挾濮烽弳銈夋⒑閹规劕鍚归柛瀣仱婵＄敻宕熼姘辩杸闂佸壊鍋呭ú姗€顢撳澶嬧拺缂佸顑欓崕蹇斻亜閹存繍妯€鐎殿喛顕ч埥澶愬閳哄倹娅囬梻渚€娼ч悧鍡涘箠韫囨稒鍊靛Δ锝呭暞閳锋垿鏌涢敂璇插箹闁告ɑ娼欓湁婵犲﹤鍟伴崺锝団偓娈垮枛椤攱淇婇幖浣肝ㄩ柕蹇婃濞兼梹绻濈喊妯活潑闁搞劍濞婂畷銏ｎ樄鐠侯垶鏌嶈閸撴氨鎹㈠┑瀣仺闂傚牊鍒€濞戙垺鐓曢悗锝庡亝鐏忎即鏌曢崶褍顏鐐达耿瀹曪繝鎮欓崗鍛婂亝缂傚倸鍊烽懗鑸垫叏閸偆绠惧┑鐘叉搐閽冪喖鏌ｉ弬鍨倯闁稿﹦绮穱濠囶敍濠靛浂浠╂繛瀵稿Ь閸嬫劗妲愰幒妤佸€锋い鎺嶈兌閸戯紕绱撴担鍓叉Ц缂傚秴锕ら锝嗙節濮橆儵鈺呮煏婢跺牆鐏い顐㈢Т閳规垿顢欑粵瀣暥濠碘槅鍋呴悷鈺呫€侀幘璇插耿婵炲棙鍨归敍婵囩箾鏉堝墽瀵肩紒顔界懇瀹曨偊鎼归崷顓狅紲闁荤姴娲﹁ぐ鍐汲濞嗘劑浜滄い鎾跺仦瀹告繈鏌熸搴⌒㈤棁澶愭倵閿濆骸浜為柍褜鍓欓…宄邦潖閾忚鍠嗛柛鏇ㄥ亞椤︺劌鈹戦悙鍙夊櫤婵炶尙鍠栭妴渚€寮崼鐔封偓鐑芥煟閹寸們姘跺箯濞差亝鈷戦柛娑橈功閳藉鏌ㄩ弴妯衡偓鏍箞閵娾晛鐐婇柍鍝勫暟閿涙粍绻濋姀锝嗙【闁挎洩绠撻弫宥咁吋婢跺鍘靛Δ鐘靛仜閻忔繈鎮橀埡鍌樹簻妞ゆ劧绲跨粻鏍ㄧ箾閻撳海绠诲┑鈩冩倐婵＄柉顦查梺鎷岄哺缁绘繈鎮介棃娑楀摋闂佽妞挎禍鐐垫閻愬鐟归柍褜鍓濆Λ銏ゆ⒑閸︻厼浜炬俊鐐叉健瀵偊宕掗悙鑼獓闂佸壊鍋呯粙鎴炰繆鐠恒劎纾奸柣妯垮吹閻ｆ椽鏌＄仦鍓ф创闁诡喒鏅犻、鏇㈠閻橆偅瀚熸繝鐢靛О閸ㄥ骞婇幘娲绘晞濠㈣泛锕﹂弳?
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
        // WARP 闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃繘鍩€椤掍胶鈻撻柡鍛箘閸掓帒鈻庨幘宕囶唺濠德板€愰崑鎾剁磼閳ь剚寰勭€ｂ晝绠氬銈嗙墬缁绘劖顨ヨ箛鎾佺懓顭ㄩ崘鈺勭闂侀€炲苯澧紒鐘茬Ч瀹曟洟鏌嗗畵銉ユ喘椤㈡稑顭ㄩ崟顐㈠箲闂備線鈧偛鑻晶瀛樻叏婵犲啯銇濈€规洦鍋婂畷鐔碱敆閸屾ê浜规繝鐢靛仜閻°劎鍒掓惔銊ョ；闁绘柨鎼崹婵嬫煥濠靛棙鍟掗柡鍐ㄧ墕缁€鍐┿亜韫囨挻锛旂紒?Sniroute 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸崹楣冨箛娴ｅ湱绋佺紓鍌氬€烽悞锕佹懌闂佸憡鐟ョ换鎺楀焵椤掑喚娼愭繛鍙夌墪鐓ら柕濞炬櫅绾偓濠殿喗顭堥崺鏍偂濞嗘垹纾藉ù锝夋涧閻忊晠鏌＄€ｎ偆銆掔紒杈ㄥ浮閹亜鈻庤箛鏂款槱闂佺粯鎸搁崯鎾蓟閺囩喎绶炴繛鎴炴皑閺嗙姵绻濋埛鈧仦鐣屼桓闂佸搫鐭夌换婵嗙暦閸楃倣鐔兼嚒閵堝懎娈ュ┑锛勫亼閸娿倝宕㈤悡搴劷闁跨喓濮寸粻鐘崇節婵犲倻澧涢柛瀣ㄥ姂濮婂宕奸悢琛℃灁闂佸憡鑹鹃崐鍨潖缂佹ɑ濯撮悷娆忓娴犫晠姊虹粙娆剧劸闁稿﹥绻傞悾鐑芥晲婢跺﹦顦ㄩ梺闈浤涢崟顒夊晭闂傚倷鐒︾€笛呮崲閸岀倛鍥ㄥ閺夋垹鏌ч梺鍝勮閸庢娊宕ｉ幘缁樼厱闁靛绲芥俊鐣岀磼閳ь剟宕奸悢缈犵盎闂佹寧妫侀妴鈧柛瀣崌閸╋繝鎮抽惇濡塭/YouTube/Play 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮闁汇値鍠楅妵鍕冀椤愵澀绮堕梺鎼炲妼閸婂潡寮诲☉婊庢Ъ濡炪們鍔岄幊鎾舵崲濮橆兘鏀介柣姗嗗枛閻忛亶鏌涢埡鍌滃⒌鐎规洘绻堝浠嬵敇閻愮绱遍梺璇插嚱缂嶅棝宕板Δ鍛亗闁哄洨鍠嗘禍婊堟煙娴煎瓨娑уù婊勭矊椤儻顦冲鐟版濡叉劙骞掑Δ鈧悞鍨亜閹烘垵顏╂鐐灪缁绘盯骞嬮悙鍨櫘闂?
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

    // 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晜閽樺缃曢梻浣告啞閸旓箓宕伴弽顐㈩棜濠电姵纰嶉悡娆撴煙鐟欏嫬濮﹂柛銈嗙懅缁辨帡鎮╅懠顒傂ㄩ梺璇″枟椤ㄥ懘鍩ユ径濞炬瀺闁惧浚鍋勯悘鈺冪磼椤旇偐澧㈤柍褜鍓ㄧ紞鍡涘窗濡ゅ懏鍋傞柡鍥ュ灩缁犺绻涢敐搴″闁诲浚浜幃妯跨疀閵壯呮殼闂佸搫鐬奸崰鏍€佸☉姗嗘僵妞ゆ帊鐒﹂鎺楁煟鎼淬値娼愭繛鍙夌矒瀹曚即寮介婧惧亾娴ｅ壊娼ㄩ柍褜鍓熼獮鍐煥閸繂鑰垮┑鈽嗗灥瀹曢潧危濮椻偓濮婂宕掑顑藉亾妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇氱秴闁搞儺鍓﹂弫宥夋煟閹邦垰鐨洪柡灞界墦濮婅櫣娑甸崨顔俱€愬銈庡亝濞茬喖宕洪埀顒併亜閹达絾纭剁紒鎰⒐椤ㄣ儵鎮欏顔煎壎闂佽桨绀侀崐鍧楀箠閻愬搫唯闁挎梹鍎冲鐑樼節閻㈤潧校妞ゆ梹鐗犲畷鏉课旈崨顔芥珨闂佽崵鍠愮划宥咁熆濮椻偓钘濋柛妤冨€ｉ敐澶婄疀妞ゆ垟鏅滃Λ鍐ㄧ暦閵娾晩鏁婇柟顖嗗啰顔掔紓鍌氬€烽懗鍫曟惞鎼淬劌鍌ㄥ┑鍌滎焾缁犵娀骞栨潏鍓хɑ妞ゎ偅娲熼弻銊╁棘閸喒鎸冪紒鐐劤閸氬绌辨繝鍥ч柛灞剧煯婢规洘淇婇悙顏勨偓褏绮婚幘鎰佺劷鐟滄棃骞冩ィ鍐╁€婚柤鎭掑劜濞呮粍绻濋姀锝呯厫閽冮亶鏌￠崪浣稿闁宠鍨块幃鈺冩嫚瑜嶆导鎰版⒑閸濄儱鏆遍柡鍛Т閻ｇ兘鎮╃拠鎻掑敤濡炪倖鎸鹃崯鍧楀箯濞差亝鈷戦柛锔诲弾閻掔偓绻涚€电鍘撮柛鈹垮劜瀵板嫰骞囬鐘插箻婵犵數鍋犵亸顏堫敋瑜旈幃姗€濡烽埡鍌滃帾闂佹悶鍎崝灞炬叏鐏炰勘浜滈柨鏇楀亾缂傚秴锕濠氭偄绾拌鲸鏅╅梺鐓庮潟閸婃鎮￠幘瀵哥瘈缁剧増锚婢ф煡鏌熼鐓庘偓鍨嚕椤愶箑纾兼繛鎴炲嚬濞村嫰鏌ｆ惔顖滅У闁稿甯￠崺鈧い鎺嶈兌婢у灚鎱ㄦ繝鍕笡闁瑰嘲鎳樺畷銊︾節閸愩劌澹嶇紓鍌氬€风粈渚€藝閺夋鐒芥繛鍡樺灥瀵煡姊绘担鐟邦嚋婵炴彃绉瑰畷鎴﹀箻鐠囪尙顢呴梻渚囧墮缁夌敻鎮″▎鎾崇骇闁割偅绻傞埛鏃堟煟閿濆牅鍚紒杈ㄥ浮閸┾偓妞ゆ帒瀚粈瀣亜閺嶃劎銆掗柛妯哄船閳规垿鎮╃紒妯婚敪濠碘槅鍋呴〃濠囥€侀弮鍌楀亾濞戞鑲╂崲閸℃稒鐓涢悘鐐额嚙閸旀瑩鏌熼弻銉х暫闁哄矉绱曟禒锕傚礈瑜夊Σ鍫濐渻閵堝骸骞戦柛鏃€鍨甸悾宄邦潨閳ь剟銆侀弮鍫濆耿闁哄洨濮烽ˇ銊モ攽閻樺灚鏆╁┑顕呭弮楠炲繘宕橀鑲╊槹闂佸壊鐓堥崑鍡涘级瑜版帗鐓熼幖娣焺閸熷繘鏌涢悩鍐插摵鐎殿噮鍋呯换婵嗩潩閵夈垹浜惧〒姘ｅ亾鐎殿喕绮欓、姗€鎮㈤崫鍕辈濠碉紕鍋戦崐鏍偋濡ゅ懏鍎楁い鏃傛櫕閻?
    // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繒鈧箍鍎遍ˇ顖滅矆鐎ｎ偁浜滈柟鎵虫櫅閻掔儤绻涢崗鍏肩婵﹦绮幏鍛存惞閻熸壆顐奸梻浣虹帛椤ㄥ棝銆冮崼銉ョ闁圭儤鍩堥崥瀣煕閵夈垺娅囬柣鎾存濮婃椽宕ㄦ繝搴㈢暭闂佺顑嗛崝娆忣嚕閵婏妇顩烽悗锝庡亞閸橀亶姊洪棃娑辨Ч闁搞劎鏁婚獮濠呯疀閹惧瓨鐦庣紓?ASCII闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇閸撗呮殸闁诲孩鑹鹃ˇ浼村Φ閸曨垰绠崇€广儱娲ゆ俊钘夆攽閻愬弶鍣藉┑顔惧厴閸┾偓妞ゆ巻鍋撶紒鐘茬Ч瀹曟洟宕￠悙宥嗙☉閳藉锝為钘変壕?U+00A9(濠? 闂?U+00B7(闂? 闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏇楀亾妞ゎ亜鍟村畷褰掝敋閸涱垰濮洪梻浣侯潒閸曞灚鐣剁紓浣插亾濠㈣埖鍔楅崣鎾绘煕閵夛絽濡藉┑顖濆亹閹叉悂鎮ч崼婵堢懆濡ょ姷鍋戦崹鐑樼┍婵犲浂鏁嶆慨妯块哺閻т線姊洪崫鍕靛剱闁荤噦濡囬幑銏犫攽鐎ｎ亶娼婇梺鎸庣箓濡盯濡撮幇鐗堚拺闁圭瀛╃壕鎼佹煕婵犲啰绠撴い鏇秮瀵濡烽妷褜鍟囧┑鐐舵彧缁茶棄锕㈤柆宥呭偍?
    {
        // 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛鎾茬閸ㄦ繃銇勯弽顐杭闁逞屽墮閸熸潙鐣烽妸褉鍋撳☉娅亝绂嶆潏銊х瘈闁汇垽娼у瓭闂佺顑呭Λ婵嬪箖?+ 闂傚倸鍊搁崐宄懊归崶褏鏆﹂柛顭戝亝閸欏繘鏌熼幆鏉啃撻柛銈呭閺屾盯顢曢敐鍡欙紩闂侀€炲苯澧剧紒鐘虫尭閻ｉ攱绺介崨濠冩珳闂佸憡鍓崟顓熺稁闂傚倸鍊风粈渚€骞栭銈嗗仏妞ゆ劧绠戠壕鍧楀级閸碍娅囬柣顓炴閺屾盯鍩勯崘顏佸闂佺粯鎸诲ú鐔煎蓟瀹ュ鐓涘ù锝呮啞閻忔挸鈹戦悙鑼妞ゎ厼娲幆鈧い蹇撶墕缁犳煡鏌熸导瀛樻锭婵炲牜鍙冨铏光偓鍦У椤ュ銇勯敂鐐毈闁绘侗鍠栭～婊堝焵椤掆偓閻ｇ兘鏁撻悩鍐测偓鐑芥煃鏉炵増顦峰瑙勬礀閳规垿鎮欓弶鎴犲姼濠电偟鍘у锟犲春閳ь剚銇勯幋锝嗙《缂佹劖姊归〃銉╂倷瀹割喖鍓堕梺杞扮閸婂潡骞婇悙鍝勎ㄩ柨鏃€鍎冲鐑樼節閻㈤潧校妞ゆ梹鐗犲畷鏉课旈崨顔芥珖闂佸啿鎼幊搴ｅ婵犳碍鐓曢柍鈺佸暕缁堕亶鏌℃担绋库偓鍨潖濞差亶鏁嗛柍褜鍓涚划鏃堟偨缁嬭法锛涢梺缁樏壕顓犵不妤ｅ啯鐓欓悗鐢殿焾閸撻亶鏌ｉ幒妤冪暫闁诡喗顨呴～婵嬵敃閵忕姷銈梻浣告惈閺堫剙煤閻旈鏆﹂柟顖炲亰濡绢亪姊绘繝搴″⒒闁告挻宀搁獮鍫ュΩ閿斿墽鐦堥梺鍛婄矆閻掞妇绱為崼銉﹀€甸柛顭戝亝缁舵煡鎮楀顓熺凡闁伙絿鍏橀、鏇㈡晝閳ь剙鏁柣鐔哥矊缁绘劙鎮鹃悜钘夐唶闁哄洨鍟块幏娲⒑閻撳寒娼熼柛濠冩礃缁傛帡鏁冮崒娑氬幈闂佺懓鐡ㄧ换鍕矓閾忓厜鍋撶憴鍕闁搞劏娉涢銉╁礋椤掍礁鍔呭┑鐐村灦椤ㄥ棝鎯堣箛娑欑厽閹艰揪绱曢悾顓㈡煕鎼淬劋鎲鹃挊婵囥亜閺嶃劎鐭嬮柛蹇旂矒閺屽秷顧侀柛鎾跺枎椤繐煤椤忓嫪绱堕梺鍛婃处閸撴瑧绮ｅ☉銏♀拺闁告縿鍎卞瓭闂備礁搴滅紞渚€鐛崘顔藉€烽悗闈涙憸閻﹀牆鈹戦鏂や緵闁告﹢绠栧畷銏ゅ箹娴ｅ厜鎷虹紓浣割儓閸庡锝為敃鍌涚厱濠电姴鍟粈鈧銈嗘穿缁插潡骞戦崟顖毼╃憸蹇涙晬濠婂牊鐓涘璺猴功婢ф垿鏌涢弬鎸庢崳缂侇喖顭烽幃娆撴倻濡厧骞嶉梻浣瑰劤濞存岸宕戦崨顖欑剨闂傚牊鑰借ぐ鎺撳亼闁告侗鍨抽弳銈夋⒑绾懏鐝柣妤冨█瀹曟椽鏁撻悩鑼槰闂佸疇妗ㄩ悞锕傤敂?All rights reserved"
        // 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐椤旂懓浜鹃柛鎰靛枛楠炪垺淇婇悙瀛樼闁圭⒈鍋婇垾锕傚Ω閳轰礁绐涘銈呯箰鐎氼亝绔熼弴鐐╂斀妞ゆ梻鐡旈悞浠嬫煕濡姴鍠氶崵鏇熴亜閺冨倻鎽傛繛鍫滅矙閺岋綁骞囬浣叉灆闂佺瀛╁褰掋€冮妷鈺傚€烽柛娆忣槸閺嬬姴顪冮妶鍐ㄧ仾闁烩晩鍨跺顐﹀箛閺夊灝鑰垮┑鈽嗗灣缁垳娆㈤鈶╂斀闁绘ê鐏氶弳鈺呮煕鐎ｎ剙浠辩€规洖缍婂畷濂稿即閻愭鍞堕梻浣告啞缁哄潡宕曢幎鑺ュ剹闁瑰墽绮悡娆戠磽娴ｉ潧鐏╂繛鍛耿閺屸剝鎷呴棃鈺勫惈闂佸搫鐬奸崰鏍箖瑜斿畷濂告偄閸濆嫬娈ラ梻鍌欑閹诧繝鎮烽妷鈺傛櫇妞ゅ繐鐗嗛拑鐔兼煟閺冨倸甯剁紒鐘崇叀閺岀喐瀵肩€涙ɑ閿┑鐐茬墛閸旀洟鍩為幋锔藉€烽梻鍫熺☉娴犳﹢姊虹紒妯煎ⅹ闁靛牊鎮傞悰顕€宕橀婊€姹楅梺鍦劋閸ㄥ綊宕愰悙鐑樼厽闁绘宕电槐鐗堢箾閸滃啰绋荤紒鍌涘浮瀵爼骞嬮鐔风槣闂備線娼ч悧鍡椢涘▎鎴濐棜濠电姵纰嶉悡鏇熶繆閵堝嫮顦﹂柍缁樻礋閺?闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵囩節閸屾粌顤€闂佺顑戠换婵嬪蓟閺囥垹閱囨繝鍨姈鏁堥梻浣虹帛閹告悂宕愰崷顓熷床婵犻潧娲ㄧ弧鈧梺绋挎湰閸戣绂掔€涙绠鹃悗娑欘焽閻绱掗鑺ュ磳鐎殿喛顕ч埥澶愬閳╁啯鐝曢梺鑽ゅТ濞诧箒銇愰崘顏嗕笉闁惧繐婀辩壕钘壝归敐鍥ㄥ殌濠殿喖鐗婇妵鍕Ω閵夛箑娈楅梺绯曟杺閸庢彃顕ラ崟顖氱疀妞ゆ帒鍋嗛崯瀣煟鎼淬値娼愭繛鍙夌墪閳绘棃鏁冮崒娑欐珳闂佺硶鍓濆ú婊呯不濮樿埖鈷戦柤娴嬫杹閹凤繝鏌涢幙鍕暤闁诡喚鍋ら幃娆擃敄閸欍儳鐩庨梻浣告惈濞村倹绂嶉悙鐢电煋妞ゆ洍鍋撻柡灞剧洴閸╃偤骞嗚婢规洖鈹戦敍鍕杭闁稿﹥鐗曢蹇旂節濮橆剛锛涢梺鐟板⒔缁垶鎮￠崘顔界厽闁绘柨鎲＄欢鏌ユ煛閸☆厾绋绘い銊ｅ劦閹瑩宕ｆ径濠冪亷婵＄偑鍊戦崹娲偡閳哄懎绠栭柛鎾楀倹鍕冮梺缁樺姦閸擄箓宕?闂?
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
            // 闂傚倸鍊搁崐宄懊归崶顒夋晪鐟滃秹婀侀梺缁樺灱濡嫰寮告笟鈧弻鐔兼⒒鐎靛壊妲梺绋胯閸斿酣骞夐幖浣告閻犳亽鍔嶅▓楣冩⒑濮瑰洤鐏╁鐟帮躬瀵偊宕堕浣哄幗闂佸搫鍟ú銏ゅ焵椤戣棄浜鹃梻浣告惈椤戝棝鎮ч悩璇茬畺婵°倕鎳愬畵渚€鏌涢…鎴濇灈濠殿喖閰ｉ弻锝嗘償閵忕姴姣堥梺鍝ュУ瀹€鎼佺嵁韫囨哎浜归柟鐑樺灩閸樻悂姊洪崨濠佺繁闁哥姵鐗楃粋鎺楊敍閻愭潙浠┑鐘诧工閸熸挳宕ｉ崟顖涚厵妞ゆ柣鍔屽ú銈夊础閹惰姤鐓忛煫鍥э攻濞呭棝鏌￠崪鍐М闁诡喖鍢查…銊╁川椤撗勬瘔闂佹眹鍩勯崹杈╂暜閻愰潧鍨濆┑鐘崇閺呮煡鏌涘鈧悞锔锯偓闈涚焸閺岋絾鎯旈婊呅ｆ繛瀛樼矋缁捇骞嗘笟鈧俊鎼佸Ψ閸愩劎绉洪柟顔瑰墲閹棃鍩﹂埀顒勫磻閹捐閿ゆ俊銈傚亾闁藉啰鍠栭弻鏇熺箾閸喖濮夊┑鈩冨絻閻楀﹦鎹㈠☉銏犲耿婵☆垰娴氭禍顏勭暦濠婂懐灏甸柟?濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰▕閻掕姤绻涢崱妯诲碍閻熸瑱绠撻幃妤呮晲鎼粹€茬敖闂佸磭鎳撶粔鐢垫崲濞戞﹩鍟呮い鏃囧吹閻╁酣姊洪崨濠佷孩妞ゎ偄顦遍幑銏犫攽閸モ晝鐦堥梺绋挎湰绾板秹鎮橀幘缁樷拺闁告縿鍎遍弸搴ㄦ煙閾忣個顏堫敋閿濆閱囬柡鍥╁仱閸炶泛鈹戦悙鏉戠仸闁挎碍銇勯鈧ˇ闈涱潖濞差亜浼犻柛鏇ㄥ墻濡偤姊虹紒姗嗘畷闁圭懓娲ら?WARP 闂傚倸鍊搁崐宄懊归崶褏鏆﹂柛顭戝亝閸欏繘鏌ｉ姀銏╃劸缂佲偓婢跺本鍠愰煫鍥ㄦ礀閸ㄦ繂鈹戦悩瀹犲缂佺媴缍侀弻銊モ攽閸℃娈ㄥ┑顔款潐椤ㄥ﹤顫忛搹鍦煋闁糕剝顨呴瀛樼箾閸噥鐒介柍褜鍓濋～澶愬箰妞嬪孩顐芥慨妯挎硾缁犳牜鎲搁悧鍫濈瑨濞磋偐濞€閺屾盯寮撮妸銉ヮ潽闂佺瀛╅幑鍥ь潖?YAML闂?
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
        // winget 闂?闂傚倸鍊峰ù鍥敋瑜嶉湁闁绘垼妫勭粻鐘绘煙閹规劦鍤欑紒鐘靛枛濮婁粙宕堕鈧闂佸湱澧楀妯肩矆閸愨斂浜滈柡鍐ㄦ处椤ュ霉閻樺眰鍋㈡慨濠冩そ濡啫霉閵堝棛娲寸€规洜鍠栭、鏇㈡晲閸℃瑥鑳堕梻鍌氬€搁崐椋庣矆娓氣偓楠炲鍩勯崘顏嗘嚌濠德板€曢幊搴ｇ矆閸喓绠鹃柟瀛樼懃閻忣亪鏌涚€Ｑ冨⒉闁靛洤瀚伴獮鎺楀箣濠垫劒鐥梻浣虹帛閹稿鎯勯鐐茶摕婵炴垯鍨圭粻娑㈡煕韫囨挻鎲搁柣鈺侀叄閺屸€愁吋娴ｆ彃浜鹃柡鍌樺劜閺傗偓闂備胶绮崝鏍ь焽濞嗘垵绶ょ紓浣诡焽缁犻箖鎮樿箛鏃傚ⅳ闁稿鎹囬獮鍥ㄦ媴閸涘鍚?闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵囩節閸屾粌顣洪梺钘夊暟閸犳牠寮婚弴鐔虹闁割煈鍠栨竟鍕攽閻愬弶鈻曞ù婊勭矒瀹曟﹢鍩€椤掆偓椤啴濡堕崱妯烘殫闂侀潻绲婚崕杈亱?0闂?1978335189闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔封枎閳ュ磭婀撮柛鏃€鍨垮畷娲焵椤掍降浜滈柟鐑樺灥閳ь剝宕垫竟鏇熺附缁嬭法楠囬梺鍓插亝缁嬫垶淇婄捄銊х＜闁绘﹢娼ф禒閬嶆煛瀹€瀣？濞寸媴绠撻幊鐐哄Ψ閵夛妇浜烽梻鍌欐祰濡椼劑鎮為敃鍌氱婵炲棗绻掗弳锕傛煟閺冨倵鎷￠柡浣告喘閺岋絽螣閸濆嫭姣愰梺鍝勵儐閹瑰洤顫忔繝姘＜婵炲棙鍩堝Σ顕€姊洪崷顓涙嫛闁稿妫濋崺鈧い鎺嶇劍閻ㄦ垿鏌涜箛鏂嗩亪鎮炬搴㈠枂闁告洦鍋掗崵銈夋⒑閸濆嫷妲归柛銈忕畵閺佸啴宕掑☉姘箥闂佽娴烽弫鎼佲€﹂崼鐔哄ⅰ闂備浇顕х€涒晠鎳濇ィ鍐ｂ偓锕傛倻閽樺鎽曢梺鍝勬川閸犲海娆㈤悙瀵哥闁瑰鍋犳竟姗€鏌ｉ鐔虹疄婵?
        // 濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴欏焺閺佸銇勯幘璺烘瀾闁告瑥绻愰湁闁稿繐鍚嬬紞鎴犵棯閹勫仴婵﹥妞介、妤呭焵椤掑倻鐭欏〒姘ｅ亾鐎殿噮鍋婂畷鍫曞煛閸屾碍鐎鹃梻浣虹帛椤ㄥ懘鎮ч崱娆戠當闁圭儤姊荤壕濂告煟濞嗗繑鍣介柛鎺斿閵囧嫰顢旈崟顐ｆ闂侀€炲苯澧剧紓宥呮缁傚秴顭ㄩ崼鐔峰墾闂侀潧顭堥崕宥堛亹閹烘挸浜楅柟鑲╄ˉ濡狙囧磻閹捐閿ゆ俊銈傚亾闁藉啰鍠栭弻銊╂偄閸濆嫅銏㈢磼閳ь剛鈧綆鍠楅埛鎺楁煕椤愩倕鏋旈柕鍡樺笒闇夐柣姗嗗亝濞呭﹥鎱ㄦ繝鍛仩闁归濞€閸ㄦ儳鐣烽崶锔藉煕缂傚倸鍊峰ù鍥春閺嵮呮殕闁归棿鑳跺畵渚€鏌涢埄鍐槈闁绘劕锕ラ妵鍕箳瀹ュ浂妲繛瀛樼矋缁挸顫忛搹鍦＜婵☆垵娅ｆ禒绋款渻閵堝骸骞栨繛宸幖椤曪綁骞庨懞銉ヤ汗缂傚倷鐒﹁彜闁归攱妞藉娲川婵犲嫧妲堥梺鎸庢磸閸婃繈骞冨Ο缁樼秶闁靛ě鍜佸晭闂備礁鎲＄划宀勫疮閹殿喚鐭撴い鏇楀亾闁哄矉绲介埥澶愬础閻愬褰庨柣搴ゎ潐濞叉牠鎮ラ崗闂寸箚婵繂鐭堝Σ鐑芥⒑閸濆嫷鍎愰柣妤冨Т椤繐煤椤忓嫬绐涙繝鐢靛Т鐎氼噣鎮鹃悽鍛婄厽閹兼番鍨归崵顒勬煕閹惧鎳囬柛鈹垮灩椤撳ジ宕ㄩ鍛偓濠氭⒑鐟欏嫬鍔ゆい鏇ㄥ弮瀹曪綁宕卞缁樻杸闂佺粯鍔栧娆撴倶閿斿浜滄い鎾跺仦閸犳﹢鏌熼鎯т槐闁诡喗绮撻幊鐐哄Ψ瑜嶇敮妤呮煟閻斿摜鐭嬬紒顔芥尭閻ｅ嘲顭ㄩ崘锝嗙€婚梺褰掑亰閸ｏ絽顬婇鈧娲传閸曨偅娈梺绋匡龚瀹曠數鍒掓繝姘婵炲棗澧介崬鐢告⒑閸忓吋鍊愭繛浣冲嫭鍙忛柛銉墯閻撴洘绻涢崱妤呯崪闁衡偓婵犳碍鐓忛柛銉戝喚浼冮悗娈垮枙缁瑦淇婇幖浣肝╅柕澶樺枟椤ャ倕鈹戦悩鍨毄闁稿鍋ゅ畷鐢割敂閸繄鍊為梺闈浤涢崘銊ь啈婵犵绱曢崑鎴﹀磹閺囩姵宕查柟閭﹀枓閸嬫挸顫濋崡鐐插箣閻庤娲樺ú鐔奉嚕椤曗偓瀹曞ジ鎮㈤崫鍕瑲闂佽崵鍠愮划宥囧垝閹惧磭鏆︽俊銈傚亾妞ゎ偅绮撻崺鈧い鎺嗗亾闁伙絿鍏橀獮瀣晝閳ь剛绮婚懡銈囩＝濞达綀顕栭悞浠嬫煕濮椻偓娴滆泛顫忛搹瑙勫厹闁告侗鍠栧☉褏绱撴担鍦弨缂佺姵鐗犻妴渚€寮崼鐔蜂汗缂傚倷鐒﹂…鍥储娴犲鈷戦柛婵嗗琚梺鍛婃煥闁帮綁骞嗗畝鍕闁瑰瓨姊归弬鈧?
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
            // 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晝閳ь剛澹曟繝姘厱闁斥晛鍟伴埥澶愭煕鐏炶濮傞柡灞诲€曢湁閻庯綆鍋呴悵鏇烆渻閵堝骸浜濇繛鑼枛瀵鎮㈢悰鈥充壕闁汇垺顔栭悞鎯归悩娆忔处閻撳啰绱撴担鑲℃垿鍩涢幒妤佺厓鐟滄粓宕滃▎鎴犱笉鐎广儱顦壕鍧楁⒑椤掆偓缁夊绮婚悩璇茬缂侇喛顫夐鍡樹繆閹绘帞澧㈤柟鍙夋倐瀵噣宕煎┑濠冩啺?闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閹绘帞鈧參姊哄Ч鍥х伄妞ゎ厼鐗忕划鑽ゆ喆閸曗晙绨婚梺瑙勫閺呮盯宕濆Ο濂界懓顭ㄩ崱鈺傜秷缂備胶绮惄顖炵嵁鐎ｎ喗鍊烽柛鎰ㄦ櫅閸濊櫣鈧鍠栭…鐑藉极閸愵喖纾兼繛鎴炶壘鐢姊绘笟鈧褔藝椤撱垹绀夌€光偓閸曨剙鍓┑鐐叉▕娴滄繈鎮?闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔兼倻濡櫣浠奸梺鍝勬濡繈寮婚悢鍏尖拻閻庨潧澹婂Σ顕€寮堕埡鍌滅疄闁哄瞼鍠栧畷妤呮嚃閳哄倹顔冮梻浣告啞閺屻劑鎯夐懖鈺佸灊濠电姵鑹惧敮闂佸啿鎼崐鎼佸焵椤掑倹鏆柡灞诲妼閳规垿宕卞☉鎵佸亾濡ゅ懏鐓曟俊顖滅帛鐏忥箓鏌＄仦鐣屝ユい褌绶氶弻娑㈠箻缂堢姳瀛╅梺鍦摂閸嬪棛妲愰幘瀛樺婵繂鑻顓㈡倵鐟欏嫭澶勯柛鎾跺枛瀹曟椽鎮欓崫鍕吅闂佹寧娲嶉崑鎾绘煟閹邦剨韬柡灞炬礃缁绘繆绠涢弴鐘虫婵＄偑鍊曠换瀣础閸愬樊娼?DoH/nginx 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵囩節閸屾粌顤€闂佸搫鎷嬫禍婵堟崲濠靛鍋ㄩ梻鍫熷垁濞戙垺鐓曢悗锝庡亝鐏忎即鏌曢崶褍顏紒鐘崇洴楠炴鎹勬笟顖涙緫闂傚倷绀侀幖顐﹀箯鐎ｎ喖绀堥柣鏃傚帶缁犳牗鎱ㄥ璇蹭壕闂佽鍠楅悷鈺呭箠濠婂啠鏋旈柟绋垮瘨濡嫰姊婚崒娆掑厡缂侇噮鍨抽弫顕€骞掑Δ鈧洿闂佸綊妫跨粈浣虹不閺嶎厽鍋℃繛鍡楃箰椤忣亜鈹戦娑欏唉闁哄本绋戦埢搴ょ疀閿濆柊褔姊虹紒妯肩畺闁绘濮撮～蹇涙倻濡顫℃繛瀵稿帶閻°劑顢撻弽顓熲拺缂佸灏呴弨鑽ょ磼鐠囪尙澧﹂柟顔诲嵆椤㈡瑩鏌ㄩ姘闂佹寧绻傛鎼佸几閻斿吋鐓欓悘鐐插€搁崝锕傛煛鐏炲墽銆掑ù鐙呯畵瀹曟粏顦抽柛锝庡櫍濮婂宕掑鍏夹︾紓浣割槺閺佹悂骞戦姀鐘闁靛繒濮烽娲⒑缂佹ê濮堟繛鍏肩懇瀵啿螖閸涱喒鎷虹紓浣割儓濞夋洟寮伴崒鐐寸厱濠电姴鍟扮粻鎾淬亜閺囶亞绋荤紒缁樼箓椤繈顢楅崒姘ｅ亾閻愮儤鈷戦柛鎰级閹牓鏌涢悢璺哄祮闁轰礁绉归幃娆撴倻濡攱瀚奸梻浣藉吹閸犳劖绔熼崱娑樼闁绘绮悡鐔兼煟閺冣偓濞兼瑩鎮橀懠顑藉亾?
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


// 婵犵數濮烽弫鍛婃叏閻戝鈧倿鎸婃竟鈺嬬秮瀹曘劑寮堕幋鐙呯幢闂備線鈧偛鑻晶鎾煛鐏炲墽銆掗柍褜鍓ㄧ紞鍡涘磻閸涱厾鏆︾€光偓閸曨剛鍘搁悗鍏夊亾閻庯綆鍓涢敍鐔哥箾鐎电顎撳┑鈥虫喘楠炲繘鎮╃拠鑼唽闂佸湱鍎ら崺鍫濐焽閵夈儮鏀介柣妯虹仛閺嗏晠鏌涚€ｎ剙浠╅柕鍥ㄥ姍瀹曟﹢鈥﹂幋鐘插Ш闂備胶鍋ㄩ崕鏌ュ礈濮樿泛鍚归柡鍥╁枂娴滄粓鏌熼悜妯虹仴闁逞屽墮缂嶅﹤顕ｉ崘宸叆闁告洍鏅欑花濠氭⒑閸濆嫬鈧悂鎮樺┑瀣厱闁哄啫鐗婇悡鐔哥箾閹寸偟澧柛鏃撶畵閺?sniroute.cmd update check闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇閸撗呮殸闁诲孩鑹鹃ˇ浼村Φ閸曨垰绠崇€广儱妫Ο鍌滅磽娴ｇ瓔鍤欐俊顐ｇ箞瀵鈽夊Ο閿嬵潔濠殿喗顨呴悧濠囧极閸撗呯＝闁稿本姘ㄥ瓭濠电偛妯婇崣鍐嚕婵犳碍鍋勯柣鎾虫捣閻ｉ箖鎮峰鍐х€殿喛娅曞蹇涘Ω閿曗偓瀵潡姊洪幐搴ｇ畵闁瑰嘲顑呰灒闁逞屽墴濮婅櫣鎷犻垾铏亪缂傚倸绉撮敃顏堟偘椤曗偓瀹曞爼顢楁径瀣珝闂備胶绮Λ浣糕枍閿濆鐓濋煫鍥ㄧ⊕閳锋帒霉閿濆牊顏犻柕鍡楋躬閺岋繝宕ㄩ鐐垱闂佺懓纾崰鏍€佸☉銏″€婚柛銉㈡櫇閳笺倖绻濋悽闈涒枅婵炰匠鍥舵晞闁告侗鍨崇亸鐢碘偓骞垮劚椤︿即鎮￠崘顏呭枑婵犲﹤鐗嗙粈鍫ユ煟閺冨牜妫戦柡鍡閳ь剛鎳撶€氼參宕抽弶鎳ㄦ椽顢斿鍡樻珕闂備礁鎲℃笟妤呭垂閹殿喗鏆滈柟鍓х帛閳锋垿鎮归崶顏勭毢缂佺姵濞婇弻娑氣偓锝庡墮閺嬫稓鈧娲樺ú鐔煎箖閻ｅ瞼鐭欓悹渚厜缁?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧湱绱掔€ｎ偓绱╂繛宸簼閸嬫劙鎮归崶顏勭毢闁挎稒鐩娲偡閺夊簱鎸冪紓渚囧櫘閸ㄨ泛鐣烽幋锔芥櫢闁绘ê纾崢鎼佹⒑閹肩偛鍔橀柛鏂块叄閵嗗倿鎳犻鍌滐紲婵炶揪绲介幖顐︺€傛總鍛婄厪闁搞儜鍐句純閻庢鍣崳锝夊春閳ь剚銇勯幒鎴濐仾闁稿顑夐弻锝呂熼悜妯锋灆濠?GitHub API闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇妤ｅ啯顎嶉梺绋款儏椤戝鎮￠锕€鐐婇柕濞垮劚閺嬫瑧绱撻崒姘毙ｉ柣妤冨█瀵鈽夐姀鐘靛姶闂佸憡鍔︽禍鏍ｉ崼銉︾厽閹兼番鍨兼竟妯汇亜閿旇鐏﹂柍銉閹瑰嫰濡搁敃鈧壕顖涚箾閹炬潙鍤柛銊ㄩ哺椤ㄦ瑥鈹戦悩鍨毄闁稿鐩棟妞ゆ挶鍨归崹鍌毭归悩宸剳鐎规挷绶氶弻鈥愁吋鎼粹€茬爱闂佺顑嗛幐鎼佸煡婢跺ň鏋嶉柧蹇ｅ亜閻忋儵鏌ｉ敐鍥у幋鐎规洜鍠栭、鏍矗閵壯呮晨闂傚倷绀侀崯鍧楀箹椤愶富鏁勯柛娑欑暘閳ь剙鎳橀、妤呭礋椤掑倸骞嶉柣搴ｆ嚀鐎氫即宕戞繝鍥х？闁哄倸绨遍弨浠嬫煃閵夈儳锛嶉柛鈺嬬悼閳ь剝顫夊ú鏍Χ閸涘﹥鍙忛柍褜鍓熼弻宥夋寠婢跺妫＄紓鍌氱Т閻倸顫忔繝姘＜婵炲棙鍨垫俊浠嬫煟鎼达絿鎳楅柛娑卞灣閻撴捇姊虹憴鍕靛仹閻忕偛鍊告慨鍌炴煕閳规儳浜炬俊鐐€栫敮鎺楀磹閼姐倕顥氬┑鍌滎焾缁狙囨煕椤愶絿鈽夊┑陇娉曠槐鎺楁偐闂堟稐娌銈冨妸閸庣敻骞冨▎鎾村殤妞ゆ埈鍊ら崶銊у幈闂佸疇顫夐崕铏閻愵兛绻嗛柣鎰典簻閳ь剚鐗滈弫顔界節閸ャ劌娈戦梺鍓插亝濞插繘鍩€椤掆偓閹虫﹢骞冨鍫熷殟闁靛／鍐ㄥ婵犵數濮伴崹濂稿春閺嶎厽鍎庢い鏍ㄥ嚬閻庤埖銇勯弴妤€浜鹃梺鍝勫閸撴繂顕ラ崟顒傜瘈闁告洦鍘滈妸鈺傗拺闁告繂瀚﹢鎵磼鐎ｎ偄鐏撮柨婵堝仜閳规垹鈧絽鐏氶弲銏ゆ⒑閸涘﹥绀堟い锝勭矙瀹曟垿骞樺鍕瀹曟﹢鍩℃担鍦偓楣冩⒒娴ｅ憡鎯堟い锔垮嵆椤㈡牕鈻庨幘鍐叉優闂佺粯鏌ㄩ崥瀣偂閻斿吋鐓忓鑸得弸娆戠磼閵娿倗鐭欓柡宀€鍠栭幃鐑藉级濞嗗彞绱斿┑鐘殿暯閸撴繈骞冮崒娑欏弿闁逞屽墴閺屻劌鈽夊Ο渚患濡ょ姷鍋為敃銏狀潖婵犳艾纾兼繛鍡樺灥婵′粙鏌ｆ惔锝囨嚄闁告侗鍨抽悡鎾绘⒑閻熼偊鍤熼柛瀣枛濮婁粙宕熼娑氬幐闂佸憡鍔戦崝宀勫焵椤掑啫鍚圭紒顔碱煼瀵剙鈻庨崜褍鏁搁梻浣稿悑閹倸顭囪閹便劑宕奸妷锔惧幈闂侀潧鐗嗛幊搴敂椤撱垺鐓涚€光偓鐎ｎ剛鏆犻柧浼欑到閵嗘帒顫濋悡搴ｄ画缂佹鍨舵穱濠囧Χ閸ヮ灝銉╂煕鐎ｎ偄鐏寸€规洜顢婇妵鎰板箳閹惧墎鏆㈤梻鍌氬€烽悞锔锯偓绗涘懏宕查柛宀€鍋涚粻鐘绘煙闁箑鏋ゅ☉鎾崇Ч閺岀喓绱掑Ο杞板垔闂佺粯鏌ㄥΛ娑㈠箟缁嬫鐓ラ柛顐ｇ箘閻撴垿姊洪崨濠傚Е闁哥姵鐗滅划璇差潩閼哥數鍘遍梺鍝勬储閸斿矂鍩€椤掑倸鈻堥柟顔哄灲瀹曨偊宕熼顐㈡闂傚倷绀侀幖顐⒚洪妸鈺佺；婵☆垵宕甸悵?
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
        // 婵犵數濮烽弫鍛婃叏閻㈠壊鏁婇柡宥庡幖缁愭淇婇妶鍛仾闁瑰啿鏈换婵嗏枔閸喗鐏堥梺鎸庢磸閸婃妲愰悙瀵哥瘈闁稿本绮嶅▓楣冩倵閻熸澘顏柛锝嗘綑鐓ゆい蹇撶У閺呮粓姊洪崨濠冨闁告ê銈搁獮妤冣偓锝庡枟閳锋帒霉閿濆懏鍟為柛鐔哄仱閺岋綁鎮ら崒婊呅滈悗瑙勬礃濞茬喖鐛惔銊﹀殟闁靛鍎伴悽缁樼節閻㈤潧鈻堟繛浣冲吘娑氫沪鏉炲尅缍佸畷濂稿Ψ閿旀儳骞嶉梻浣虹帛閸旓箓宕滃▎鎰珷妞ゆ牜鍋為悡鏇㈠箹鏉堝墽纾跨紒鐘崇叀閺屾洟宕惰椤忣參鏌℃担绋挎殲闁瑰嘲鎳樺畷鐑筋敇濠ф儳浜惧┑鐘叉处閳锋垿鎮归崶锝傚亾閾忣偆浜舵繝鐢靛О閸ㄦ椽鏁冮姀銈呯畺闁诡垎鈧弨浠嬫煕椤愶綀澹樺ù婊堢畺閺屻劌鈹戦崱娑扁偓妤€顭胯閸楁娊寮婚敐鍛闁哄诞鍐ㄐ曟繝娈垮枛閿曘儱顪冮挊澶屾殾闁靛濡囩弧鈧梺鍛婃处閸撴瑥鈻嶈閳规垿鎮欓懠顒佹喖缂備緡鍠栫粔褰掑箖閻ゎ垼妯勫Δ鐘靛仦閸ㄦ寧鎱ㄩ埀顒勬煟濞嗗苯浜惧┑鐐茬毞閺呯娀寮婚妶澶婄濞达綀顫夐柨顓熺箾鐎电啸闁告鍥х厴闁硅揪闄勯崐鐑芥煕濞嗗浚妯堟俊顐ゅ仧缁辨挻鎷呴搹鐟扮闂佸憡姊归悷銊╂儗妤ｅ啯鈷戦柛娑橈梗缁堕亶鏌涘Ο纰辩劸閸楅亶鏌涢鐘插姕闁绘挻鐟╅弻鐔碱敍閸℃鏆㈤柛鈺冨仱濮婃椽鏌呴悙鑼跺濠⒀屽灦閺岋綁鍩℃繝鍌滀桓婵犵鈧磭鎽犵紒妤冨枛閸┾偓妞ゆ帒瀚畵渚€鎮楅敐搴℃灍闁哄懏绻堥弻宥堫檨闁告挻鐩崺鈧い鎺嶆祰婢规ɑ銇勯敂璇茬仸闁炽儻濡囬幑鍕Ω閿曗偓绾绢垱绻涢幘鏉戝毈闁搞劋鍗冲畷婊堝础閻愨晜鏂€闂佺粯鍔曞鍫曞闯閸︻厾纾肩紓浣癸公閼拌法鈧鍠涢褔鍩ユ径鎰潊闁绘鏁搁弶鍛婁繆閻愵亜鈧牜鏁繝鍕焼濞达絿纭堕弸宥夋煥濠靛棙濯兼繛鎾愁煼閺屾洟宕煎┑鍥舵！闂佹娊鏀遍崝娆撳蓟濞戙垹绫嶉柍褜鍓熼幃鐑芥晜閻ｅ备鏀?
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


// ---------------------------------------------------------------- GHA 婵犵數鍋為崹鍫曞箰閹间緡鏁勯柛顐ｇ贩瑜版帒鐐婇柍瑙勫劤娴滈箖鏌ｉ姀鈽嗗晱闁衡偓娴煎瓨鐓熼柨鏇氱劍椤ュ銇?
// 闂傚倷绀侀幖顐︻敄閸曨垱鍤勯柛顐ｆ礃閸嬧晜绻濋棃娑欏缂傚秴娲弻鐔煎箚瑜嶉弳杈┾偓娈垮枦濞呮洘绌辨繝鍋芥棃鍩€椤掆偓铻為悗闈涙啞椤洟鏌熷▓鍨灓缁惧彞绮欓弻鈥愁吋鎼粹€愁潾闁汇埄鍨板Λ娆愮┍?+ 缂傚倷鑳堕搹搴ㄥ矗鎼淬劌绐楅柡宥庡幗閸?闂傚倷绀侀幉锟犳偡閿曞倹鍋嬫俊銈呭暟閻捇鏌ｉ幋锝呅撻柡鍜佸墰閳ь剙鍘滈崑鎾绘煃瑜滈崜鐔风暦濞嗘挻鍤嶉柕澹本绁梻浣瑰缁诲倸煤閵娾晛鍨傞柟鎯板Г閻撴洟鏌熼悜妯诲暗闁宠棄顦辩槐鎾诲磼濞戞瑥纾抽梺杞扮缁夌懓鐣烽悡搴叆闁告侗鍨遍崰鎺楁⒒閸屾艾鈧煤閻樿纾婚柟鍓х帛閸?.rc 闂備浇宕垫慨宥夊礃椤垳鐥紓鍌欑贰閻撳牓宕抽敐澶婃瀬鐎广儱妫楃欢鐐烘煕椤愶綀澹樺ù?
// Enter = 缂傚倷鑳堕搹搴ㄥ矗鎼淬劌绐楅柡宥庡幗閸嬧晛螖閿濆懎鏆為柡鍜佸墴閹鈽夊Ο鐓庘偓鐧?= 闂傚倷绀侀幉锟犳偡閿曞倹鍋嬫俊銈呭暟閻捇鏌ｉ幋锝嗩棄鏉?
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

// 闂傚倷绀侀幉锟犳嚌閻愵剦娈芥慨婵嗚娴滃綊鎮楅棃娑欏暈鐎?gha fetch闂傚倷鐒︾€笛呯矙閹达箑瀚夋い鎺嶆缁诲棝鏌熼梻瀵割槮閻庢艾顦甸弻宥堫檨闁告挾鍠庨悾鐑藉Ψ閿旇棄鍔呭┑鐘绘涧閻楁粍瀵奸崶顒佺厽闁绘劕顕埥澶愭煟椤撴繆鍏屾繛鍡愬灲椤㈡﹢濮€閻樻妲卞┑掳鍊х粻鎴︽倶濠靛鍨傞梺顒€绉甸悡鏇㈡煙閻戞ɑ鐓ユい锝呫偢閺岋綁濡搁埡鍌涢敪闂佸磭绮褰掔嵁婵犲洦鐓?
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
    // WARP 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ磵閳ь剨绠撳畷濂稿Ψ椤旇姤娅堥梻浣告啞閸旓箓宕伴弽顐㈩棜闁秆勵殕閳锋垶銇勯幒鍡椾壕缂備礁顦顓犲垝婵犳艾鍐€妞ゆ劗鍠庢禍鐐殽閻愯尙浠㈤柛鏃€宀搁弻鐔煎矗婢规繍浜崺銉﹀緞閹邦剦娼婇梺缁橈耿濞佳勬叏閿旀垝绻嗛柣鎰典簻閳ь剚鐗滈弫顕€骞掗弬鍝勪壕婵鍘ф晶鎾煙闁垮鐏撮柟顔哄灲瀹曟鎳栭埡浣稿婵犵數濮伴崹濂稿春閺嶎厽鏅梻浣筋嚙妤犲繒寰婄捄銊︻潟?0 缂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕闁芥ɑ绻堥弻鐔封枔閸喗鐏嶉梺鍝勬缁矂鍩為幋锔藉亹闁圭粯甯╂导鈧梻浣告啞椤ㄥ棗鐣濋幖浣歌摕闁斥晛鍟欢鐐烘倵閿濆簼绨藉ù婊冪埣濮婃椽宕崟闈涘壈闂備礁搴滅紞浣割嚕鐠囧樊鍚嬪璺侯儏閻у嫭绻濋姀锝嗙【妞ゆ垵鎳庨埢宥堫槾缂佽鲸鎸婚幏鍛村箵閹哄秴顥氭繝鐢靛О閸ㄧ厧鈻斿☉鈶┾偓锕傤敆閸曨剙浜滈梺绯曞墲缁嬫帡鎮″▎鎰╀簻闁哄秲鍔庨埥澶嬨亜閵夈儺鍎戠紒杈ㄥ浮婵℃悂濡烽敃鈧禒鎾⒑鐠団€虫灈闁搞垺鐓￠崺銏℃償閳锯偓閺嬪酣鏌熼悙顒佸剹闁逞屽墮缁夊墎妲愰幘瀛樺闁告挻褰冮崜鏉款渻閵堝繒鐣遍柣蹇旂缁岃鲸绻濋崶褔鍞堕梺鍝勬川閸嬫盯鎳撻崸妤佲拺閻庡湱濮甸妴鍐╀繆閻愭潙绗氭繛鑹邦嚙椤粓鍩€椤掆偓椤繒绱掑Ο璇差€撻梺鍛婄缚閸庤櫕顨欏┑鐘垫暩閸嬫﹢宕犻悩璇插窛妞ゆ牗绮庨崢鐘绘煟閻斿摜鐭屽褎顨堥弫顕€骞掑Δ浣糕偓鍧楁煕椤垵浜栧ù婊勭矒閺岀喖宕崟顓夈倝鏌涢弬璺ㄐч柡宀嬬秮婵℃悂鏁冮埀顒勬倶閹绢喗鐓涚€光偓鐎ｎ剛袦濡ょ姷鍋為悷鈺佺暦閻旂⒈鏁囬柣妯虹仛椤本绻?
    // 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閼碱剦妲遍柣鐔哥矌婢ф鏁幒妤€鍨傞柛宀€鍋為悡鐔镐繆閵堝倸浜鹃梺缁橆殔閿曨亜鐣烽敐鍫㈢杸婵炴垶鐟ч崢鎾绘⒑閸涘﹦绠撻悗姘煎弮瀹曟娊宕烽鐘碉紲濡炪倖鍔戦崹缁樻櫏闂備浇顕栭崰鏍礊婵犲倻鏆︽い鎰剁畱缁€瀣亜閹哄秶顦﹂柛鎾舵暬濮婂搫效閸パ呭姶闂佸壊鐓堥崜鐔风暦閺囥垺鍋ㄧ紒瀣劵閹芥洟姊虹紒妯烩拻闁稿簺鍊曢悾閿嬪緞閹邦厾鍘藉┑顔斤供閸樼晫绱為幋婢濆綊宕楅悡搴☆潕缂備浇椴搁幑鍥х暦閹烘埈娼╂慨锝嗙懀閸ㄦ椽濡甸崟顖氱婵犲灚鍔栫瑧闂備椒绱徊浠嬪床閺屻儮鈧箓濡搁埡浣侯槰闂佽鍨庨崟顐熷亾椤栫偞鈷掑ù锝堫潐閸嬬娀鏌涙惔銏°仢闁诡喗锚閳规垹鈧綆鍋呭▍鍥⒑缁嬫寧婀扮紒瀣崌瀹曘垽妫冨☉杈ㄥ瘜闂侀潧鐗嗗Λ妤呮倶閿曞倹鍊电紒妤佺☉閹虫劙鎯岄崼銉︾厽婵°倐鍋撻柣妤€瀚粋宥咁煥閸喓鍘搁梺绋挎湰缁嬫垿顢氬鍐剧唵閻犲搫鎼顐ょ磼鏉堛劌绗ч柍褜鍓ㄧ紞鍡樼鐠轰綍锝吤洪鍛帗閻熸粍绮撳畷婊堝Ω瑜忕粈濠囨煕閳╁啰鈽夌痪鎯ь煼閺屾稑鈽夊Ο鍏兼喖闂佹娊鏀卞濠氬焵椤掆偓缁犲秹宕曢柆宓ュ洦瀵肩€涙ê浜楅梺闈涱檧婵″洨绮绘ィ鍐╃厵閻庣數顭堟禒褔鏌熼崘鍙夘棞妞ゎ叀娉曢幉鎾礋閹壋鍋撻崸妤佺厸鐎光偓閳ь剟宕伴弽顓溾偓浣糕槈濡攱鐎婚梺鐟邦嚟婵嘲鐣烽弻銉︾厽?闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偛顦甸弫鎾绘偐閼碱剦妲遍柣鐔哥矌婢ф鏁幒妤€鍨傞柛宀€鍋為悡鐔镐繆閵堝倸浜鹃梺缁橆殔閿曨亜鐣烽敐鍫㈢杸婵炴垶鐟ч崢鎾绘⒑閸涘﹦绠撻悗姘煎弮瀹曟娊宕烽鐘碉紲濡炪倖鍔戦崹缁樻櫏闂備浇顕栭崰鏍礊婵犲倻鏆︽い鎰剁畱缁€瀣亜閹哄秶顦﹂柛鎾舵暬濮婄粯鎷呯憴鍕哗闂佺锕ラ悧婊堝极椤曗偓閸ㄩ箖骞囨担鍛婎吙闂備礁澹婇崑鍛洪弽顓熷亗婵炴垯鍨洪悡鏇㈡煙閹佃櫕娅呭┑锛勫劋缁绘稑鐣濇繝渚€鍋楅梺鍝勭焿缂嶄焦鎱ㄩ埀顒勬煃鏉炴媽鍏屽ù鐙€鍙冨铏规喆閸曨厽鎲欏┑鐐叉噺濞叉绮氭潏銊х瘈闁告洏鍔屾禍鐐箾閹寸偟鎳愰柣鎺嶇矙閺岋綁鎮㈡搴Ｐㄩ梺鍝勮嫰缁夌兘篓娓氣偓閺屾盯骞樼€靛憡鍣伴梺绯曟杺閸ㄤ粙鐛Ο鍏煎珰鐎瑰壊鍠楃紞鍌炴⒒娓氣偓濞佳囨偋閸℃稑绠犻柟鎹愵嚃閺夋椽姊婚崒娆掑厡闁硅櫕鎹囧畷銉р偓锝庡枟閸嬧晜銇勮箛鎾跺闁搞劌鍊圭换娑橆啅椤旇崵鍑归梺绋匡工濞硷繝寮婚敐澶嬫櫜闁告侗鍙庡Σ鐢告⒑缁嬫鍎愰柟鍛婃倐閸┿垽骞樼拠鎻掔€銈嗘⒒閺咁偅绂嶉幇鐗堚拻?闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵囩節閸屾粌顤€闂佺顑戠槐鏇㈠箟閹间礁绫嶉柛顐ゅ枑濞呮粓姊虹紒妯哄闁圭⒈鍋嗙划濠氬箮閼恒儳鍘甸梺缁樺姦閸撴氨绱為幋鐐簻妞ゆ劗鍟块崑鎾诲箛娴ｇ懓鐦滈梻渚€娼ч悧鍡椢涘Δ鍐當闁稿本绮庣壕濂告煃瑜滈崜姘跺箯閸涱垱鍠嗛柛鏇ㄥ亜婵″啿鈹戦悩鍨毄濠殿喖鐡ㄧ€靛ジ宕ㄩ懜顑挎睏闂佸湱鍎ら崹褰捤囪濮婂宕掑顑藉亾閼姐倖宕叉繝闈涚墐閸嬫挸鈽夐幒鎾寸彅濡炪値浜滈崯瀛樹繆閼稿灚鍎熼柍銉﹀絻椤ユ氨绱撻崒娆掑厡濠殿喚鏁诲畷褰掑锤濡も偓閻忔娊鏌ㄩ弮鍫熸殰闁稿鎹囧畷鐑筋敇閻愭劑鍎甸弻锝夊Χ閸屾矮澹曢梻鍌氬€风粈渚€骞栭锕€纾归柦妯侯槴閺嬫牠鏌￠崶銉ョ労闁?
    // 闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔告綇妤ｅ啯顎嶉梺绋款儏椤戝鎮￠锕€鐐婇柕濞垮€楃换渚€姊烘潪鎵槮閻庢矮鍗冲璇测槈閵忕姈銊╂煥濠靛棙鍣规い顒€顑夊鍝勭暦閸モ晛绗″┑鈽嗗亜閸熸潙顕ｇ拠宸悑闁割偒鍋呴鍥⒒娴ｅ憡鍟為柟绋款煼閹兘鍩￠崨顓熸К闂侀潧绻嗛崜婵嬪磿閻斿吋鐓忓┑鐐茬仢閸斻倝鏌涢弮鈧幐鍐差潖濞差亜宸濆┑鐘插€搁～搴ㄦ⒑缁嬫鍎愰柟鐟版喘瀹曞搫鈽夐姀鐘插祮闂侀潧绻嗗褔骞忓ú顏呪拺闁告稑锕︾粻鎾绘倵濮樼厧鐏﹂柨婵堝仱閺佸啴宕掑☉姘箞闂佺鍋愮悰銉╁焵椤掑啯鐝慨锝嗗姍濮婃椽骞愭惔銏紭闂佹悶鍔忓Λ鍕偩閻戣棄惟闁宠桨鑳舵鍥⒑閻熼偊鍤熷┑顖ｅ幖鍗遍柟缁㈠枟閳锋帒霉閿濆洨鎽傛繛鍏煎姈閵囧嫰寮埀顒勬偋閻樿尙鏆﹂柡澶庮嚦閺冨牆宸濇い鏃堟？閹綁姊婚崒娆戣窗闁告挻鐟х划鏃堟偨閸涘﹤浜楀┑鐐村灟閸ㄦ椽鎮￠弴銏＄厸闁搞儯鍎遍悘顏呯箾閹冲嘲娲﹂悡鐔肩叓閸ャ儱鍔ょ紒鈾€鍋撻梻浣告惈閻妲愰弴鐔侯浄闁挎洖鍊归崵宥夋煏婢舵盯妾柛濠囨敱娣囧﹪鎮欓鍕ㄥ亾閺嶎灐娲煛閸愵亞顦繛鎾村焹閸嬫挾鈧娲樺浠嬪春閳ь剚銇勯幒宥夋濞存粍绮撻弻鐔兼倻濡櫣浠村銈呮禋娴滎亪寮婚敓鐘叉そ濞达絿顭堥～顐︽⒑闁偛鑻晶顕€鏌涘Ο鑽ょ煉鐎规洘鍨块獮妯肩磼濡厧骞堥梻浣筋潐濠㈡ɑ鏅舵惔銊ョ闁哄锛曢悷閭︾叆闁割偁鍨婚弳顐︽⒑鐠団€虫灍闁挎洏鍨介獮鍐ㄢ枎閹寸偛纾柡澶屽仧婢ф鎯堣箛娑欌拻濞达綀妫勯崥褰掓煕閻樺啿濮嶇€规洘绻堝鎾閻橀潧寮ㄥ┑鐘灱閸╂牠宕濋弴鐘典笉缂備焦菧娴滄粓鏌″鍐ㄥ闁绘搩鍨伴…璺ㄦ喆閸曨剛顦ㄧ紓浣虹帛缁诲牓骞冩禒瀣棃婵炴垶顨堥幑鏇熺節绾版ɑ顫婇柛瀣椤㈡岸顢橀悙宥忕秮楠炲洭寮堕崹顔肩ギ闂備胶绮濠氣€﹂崼婵愬殨闁秆勵殕閳锋垹鎲搁悧鍫濈瑲闁挎稓鍠撶槐鎺楀焵椤掍焦濯撮柧蹇涒偓娑氱憹闂備胶鎳撻悺銊ф閻ゎ垬浜归柟鐑樻尰濞呮粓姊洪幖鐐插姌闁告柨绉归幃鐢割敂閸℃瑧锛濇繛杈剧到閹碱偄鏆╅梻浣侯焾缁绘垿鏁冮姀鐙€鍤曞┑鐘崇閺呮悂鏌ｅΟ鍨毢妞わ缚鍗冲濠氬磼濮樺崬顤€缂備礁顑嗛幐鑽ょ矉閹烘鏁囬柣鎰ㄦ櫆閺傗偓闂備胶绮敃鈺呭磻閸曨剛顩查柟顖嗗本瀵岄梺闈涚墕濡瑧浜搁鍫熺厱闁靛闄勯妵婵堚偓瑙勬礃濞叉牞鐏冮梺鍛婁緱閸犳岸宕㈤鍛瘈闁靛骏绲介悡鎰版煥濮樿埖鐓熼煫鍥ュ劤閹偐绱掓潏銊ユ诞闁糕斁鍋撳銈嗗笂缁€浣烘閻愮儤鐓欓梺顓ㄧ畱楠炴﹢鏌涘鈧禍璺侯潖濞差亝鍋￠柣妤€鐗婇崕鎾愁渻閵堝棗鐏ラ柟铏耿瀹曟椽鍩€椤掍降浜滈柟鍝勭Ф閸斿秶绱掗埀顒傗偓锝庡枟閻撴瑦銇勯弴妤€浜剧紓浣哄У閻楃娀鐛崘銊㈡瀻闁瑰灝鍟弲銏ゆ⒑缁嬫寧婀版い銊ユ閹﹢宕橀鐣屽幗闁硅壈鎻槐鏇㈡偩椤撱垺鐓曢幖娣妺閹茬偓顨ラ悙璇ф敾闁诡垱妫冮崺?濠电姷鏁告慨鐑藉极閹间礁纾绘繛鎴欏焺閺佸銇勯幘璺烘瀾闁告瑥绻橀弻鈩冨緞鐎ｎ亞鍔稿┑鐐茬毞閺呯娀寮婚妸鈺佺睄闁稿本鐭竟鏇㈡倵鐟欏嫭绀€鐎殿喖澧庨幑銏犫槈閵忕姷顓哄┑鐐叉缁绘帗绂掗懖鈺冪＝濞达絽鎼牎婵犵數鍋涢敃顏勵嚕婵犳艾鍗抽柣鏃堫棑缁愮偤姊洪崜鎻掍簼缂佽鍊归幈銊︽償閵婏腹鎷虹紓鍌欑劍钃遍悘蹇ｅ弮閺屾稒绻濋崒娑樹淮閻庢鍠栭…鐑藉极閹剧粯鍋愰柤纰卞墾缁辨煡姊绘笟鈧褔鎮ч崱娑樼疇閹艰揪绲挎稉宥夋煕閿旇骞樼痪?闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔封枎闄囬褍煤椤擃潿鈧礁顫滈埀顒佹叏閳ь剟鏌ｅ鈧褎鎱?
    // 婵犵數濮烽弫鍛婃叏閻㈠壊鏁婇柡宥庡幖闂傤垱銇勯弽銊х煀缂佽翰鍊濋弻锝夋晲閸涱喗鎷遍悗瑙勬礃閻擄繝寮婚悢鍛婄秶闁诡垎鍛掗梻浣芥〃缁€渚€宕锕€鐓橀柟杈剧畱绾惧吋绻濇繝鍌涙崳闁告梹鎮傞弻锝夋偄閸濄儲鍤傞梻鍌氬鐎氱増淇婇悽绋跨妞ゆ牗鑹鹃崬銊╂⒑闂堟侗鐒鹃柛鏂跨焸钘熸繝闈涱儐閸嬬姵鎱ㄥΟ鍝勮埞闁哄绋掗幈銊︾節閸曨厼绗＄紓浣诡殘閸犳牕鐣烽幆閭︽Ъ闂佽绻愰悧鎾愁潖濞差亝鐒婚柣鎰蔼鐎氭澘顭胯閸楁娊寮诲☉銏″亜鐎瑰嫰鍋婇崝澶愭倵閸偅绶查悗姘煎枟缁傛帡鏁冮崒姘辩暰閻熸粌绻橀弫宥堢疀濞戞瑢鎷绘繛杈剧悼閸庛倝宕甸埀顒勬⒑鐎癸附婢樻俊鍧楁煕閹烘挸绗氶柟顖涙閳ワ箓骞嬮悙瀛樼彆闂備礁鎼ˇ顖炴偋閸曨垰绀夐幖杈剧悼娑?up闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔衡偓鐢殿焾娴犙囨⒒閸曨偄顏柡宀嬬節瀹曟﹢濡搁妷銏犱壕闁告縿鍎查?*闂傚倸鍊峰ù鍥敋瑜嶉湁闁绘垼妫勭壕濠氭煥濠靛棭妲哥痪鎯х秺閺屸€愁吋鎼粹€崇缂佺偓鍎冲锟犲蓟閵堝悿鍦偓锝庡亝閻濇洟鎮?12 缂?*闂?
    // first-run-setup 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ礀閸屻劎鎲搁弮鍫澪ラ柛鎰ㄦ櫆閸庣喖鏌曡箛瀣労婵炶尙顭堥埞鎴︽偐鐠囇冧紣闂佺懓鍟跨换鎰版偩閻戣棄绠甸柟瑙勵殔缂嶅﹪寮幇鏉垮窛妞ゆ棁娉曢、鍛存⒒娴ｅ摜锛嶇紒顕呭灠鐓ら柣鏃傚帶閽冪喖鏌ㄥ┑鍡╂Ц婵☆偅锕㈤弻娑⑩€﹂幋婵囩彯闂佸搫妫庨崹鑽ゆ閹惧瓨濯撮柣鐔告緲椤秴顪冮妶鍐ㄥ闁挎洦浜滈锝嗙節濮橆厼浜滄俊鐐差儏鐎涒晛鈻撳┑瀣拻濞撴艾娲ゅ鍧楁⒒閸曨偄顏柣鈥崇箻濮婂宕掑▎鎴М濠电姭鍋撻柛妤冨亹閺嬪秹鏌曡箛瀣伄缂佲偓婵犲伅褰掓晲閸涱収妫岄梺鎼炲€栧ú鐔奉潖婵犳艾閱囬柣鏂垮缁讳線姊洪崨濠呭妞ゆ垵顦靛璇测槈濡攱顫嶉梺鍛婎殘閸嬫稓鈧冻绲介埞鎴︽倷閹绘帗鍊繝鐢靛仜閿曘倝锝炶箛鎾佹椽顢旈崟顓у敹闂佺澹堥幓顏嗗緤閸濆嫀锝夊醇閵夛腹鎷哄┑鐐跺蔼椤曆勬櫠椤曗偓閺屾洟宕奸悢绋库偓鎰殽閻愭彃鏆ｆ俊顐㈠暙閳藉骞掗幙鍐╃秾闂傚倷绶氬褏鎹㈤幒鎾村弿闁绘垵澹欓崶顒夋晝闁挎梻鏅崢浠嬫⒑缂佹◤顏勵嚕閼搁潧绶為柛鏇ㄥ幗閸犳劙鏌℃径濠勪虎闁哄棴绲块埀顒侇問閸犳銇愰崘顔肩厺閹兼番鍔岀粻濠氭煛婢跺鐏ｉ柛?up 濠电姷鏁告慨鐑藉极閹间礁纾婚柣鎰▕閻掕姤绻涢崱妯诲碍閻熸瑱绠撻幃妤呮晲鎼粹剝鐏嶉梺鎼炲€曢懟顖濈亙闂佹寧绻傞幊搴ㄥ汲濞嗘垹纾奸柣娆屽亾闁搞劌鐖煎濠氭偄鐏忎焦鐏侀梺鍓茬厛閸犳鎮橀埀顒勬⒒娴ｄ警鐒炬い鎴濇楠炴垿宕堕鈧粻鏍煟閹达絾顥夌紒鐙欏洦鐓欑紒瀣仢椤掋垺銇勯弬娆炬Ц闁宠鍨块幃鈺呭箛娴ｅ憡娅涢梻浣告憸婵數鍠婂鍥ㄥ床婵炴垯鍨归悞鍨亜閹烘垵顏柣鎾寸☉闇夐柨婵嗘噺閹叉悂寮崼銉﹀€甸悷娆忓缁€鍫ユ煕濡姴娲ら悡姗€鏌熸潏鍓х暠闂佸崬娲弻锝夊棘閹稿孩鍠愰梻浣藉Г鐢帡鍩為幋锔藉亹闁割煈鍋呭В鍕⒑缁嬫鍎戦柛瀣洴閳ワ箓宕惰閺嬪酣鏌熼崹顔兼殲闁哄懐鍋ゅ娲川婵犲嫧濮囧┑鐐插悑閻熝呭垝婵犳艾鍐€闁靛鍊楃粻姘舵⒑闂堟稓澧曢柟鍐查叄瀵娊鎮㈤崗鑲╁幈濠德板€撻懗鍫曘€傞弻銉︾厪闁搞儜鍐句純閻庢鍠曠划娆愪繆濮濆矈妲惧銈嗘尵閸樠囧煘閹达附鍊烽悹杞拌濡牆鈹戦悙宸Ч闁烩晩鍨伴敃銏℃媴缁洘鏂€闂佺粯顭堥婊冾啅閵夆晜鍊垫慨妯煎帶婢ф壆绱掗鍛绩妞わ箑纾槐?
    // 闂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柛顐ｆ磵閳ь剨绠撳畷濂稿Ψ椤旇姤娅堥梻浣虹《閸撴繈鎮烽妷鈺佸瀭婵犻潧鐗冮崑鎾荤嵁閸喖濮庡┑鐐额嚋缁犳挸顕ｉ崘宸叆闁割偆鍠撻崢鎾绘偡濠婂嫮鐭掔€规洘绮撻幃銏ゆ偂鎼淬倖鎲伴梻渚€娼ч…鍫ュ磿椤斿槈锝夊醇閻斿墎绠氬銈嗙墬缁诲倹绂嶉悷鎳婂綊鎮℃惔銏犵獩缂備浇椴搁幐鑽ょ箔閻旂厧鐐婄憸搴ㄦ倶娓氣偓濮婃椽宕ㄦ繝鍐ｆ嫻闂佹悶鍔庨弫濠氥€?5454 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柟闂寸绾剧粯绻涢幋鐐垫噧缂佸墎鍋ら弻娑㈠Ψ椤旂厧顫╅梺缁樺笒閻忔岸濡甸崟顖氱闁糕剝銇炴竟鏇㈡煟鎼淬埄鍟忛柛鐘崇墵閹儲绺界粙璺ㄧ暰闂佹悶鍎崝搴ㄣ€呴悜鑺ュ€甸柨婵嗛娴滅偤鏌涘Ο鍝勮埞闁宠鍨块、娆戠驳鐎ｎ剙濮洪梻浣告啞椤棝宕惰閸嬪秹姊哄Ч鍥х仼闁硅绻濋幃?444 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曚綅閸ヮ剦鏁冮柨鏇楀亾闁汇倗鍋撶换婵囩節閸屾粌顣洪梺钘夊暟閸犳牠寮婚弴鐔虹闁割煈鍠栨竟?502闂傚倸鍊搁崐鎼佸磹閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫧闁活厽鐟╅弻鐔封枎闄囬褍煤椤擃潿鈧礁顫滈埀顒佹叏閳ь剟鏌ｅ鈧褎鎱ㄩ敂鎴掔箚闁绘劦浜滈埀顒佺墪椤斿繑绻濆顒傦紱闂佺懓澧界划顖炴偂閺囩偐鏀介柣妯诲絻娴滅偤鏌ｉ幘鐐藉仮闁哄瞼鍠栭、娆撴嚍閵夛絼铏庢俊銈囧Х閸嬫盯顢栨径鎰畺闁冲搫鎷嬮弫鍐┿亜椤愵偄骞栨い銉﹀哺濮婄粯鎷呴挊澶婃優闂佸摜濮甸崝鏇⑩€﹂崹顕呮建闁逞屽墮椤曪綁骞庨懞銉ヤ簻闂佹儳绻愬﹢閬嶆晬濠婂喚娓婚柕鍫濇閻撱儵鏌熺喊鍗炰簼闁逛究鍔戝畷鎺戔攦閹傚濠电偛鐗嗛悘婵嬪几濞戙垺鐓熼柡宓礁浠Δ鐘靛仜閸熷瓨鎱ㄩ埀顒勬煏閸繃顥犻柛娆忔閺岋絾鎯旈婊呅ｆ繛瀛樼矋閻楃姴鐣烽幋锕€绠荤紓浣姑禒娲⒑閸涘﹦鈽夐柨鏇畵瀹曘垽鏌嗗鍡忔嫼閻熸粎澧楃敮鎺撶娴煎瓨鐓曢柟鎯ь嚟閳藉鏌￠崱蹇旀珚婵﹥妞介幊鐐哄Ψ椤旀儳濮洪梻浣告憸婵挳鏌婇敐澶婃槬闁靛绠戠欢鐐烘煙闁箑澧版い鏃€甯掗—鍐Χ閸℃瑥顫ч梺娲诲弾閸犳绮╅悢鐓庡嵆闁绘棁娅ｉ惁鍫ユ⒑濮瑰洤鐏叉繛浣冲嫮顩锋繝濠傚缁诲棙銇勯幇鈺佺伄闁绘帗鎮傞弻娑㈠煘閹傚濠碉紕鍋戦崐鏍暜閹烘鐤い鏍仦閸嬬喐銇勮箛鎾跺闁绘挾鍠栭弻鏇熺節韫囨洜鏆犻梺璇茬箚閺呮繄妲愰幒鎾寸秶闁靛鍎抽鍌炴⒑闁偛鑻晶鍓х磽瀹ュ懏顥炵紒鍌氱Т椤劑宕煎┑鍥发闂傚倷绶￠崜娆戠矓閻㈠憡鍋傞柣鏂垮悑閻撱儲绻濋棃娑欘棡闁瑰吋鍔曢湁婵犲﹤鍟埛鏃傜磼鏉堛劌娴€规洘甯掗～婵喰掑▍璇叉噳閸嬫挾鎲撮崟顒傤槹婵炲瓨绮岄悥濂告偘椤曗偓瀹曞爼顢楅埀顒佸劔闂備線娼чˇ顓㈠磹閺団懞?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 濠电姷鏁告慨鐢割敊閺嶎厼绐楁俊銈呭暞閺嗘粍淇婇妶鍛殶闁活厽鐟╅弻鐔兼倻濡晲绮堕梺閫炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劍銇勯弮鍥撴繛鍛Т閳规垿鎮╅崹顐ｆ瘎婵犳鍠氶弫濠氬箠濠靛绀堢憸蹇涙儗閸℃稒鐓曠€光偓閳ь剟宕戦悙鐑樺亗婵炲棙鎸婚悡娆愩亜閺嶃劎鈯曠紒鎯板皺閹喖顫濋懜纰樻嫼闂佸湱顭堝ù鐑藉煀閺囩喍绻嗘い鎰╁灩閺嗭絿鈧鍠栭…閿嬩繆閹间礁唯闁靛繆妲呭鏃€绻濋悽闈涗粶婵☆偅鐟╁畷婵單旈埀顒勫煝閹捐围濠㈣泛顑傞幏娲煛婢跺苯浠掗柛妤€鍟块‖濠囨倻閼恒儳鍘辨繝鐢靛Т閸熸壆绮婚悙鐑樼厽婵炴垵宕弸銈夋煃瑜滈崜銊х礊閸℃稑绐楁俊銈呮噺閸嬪倿鏌曟径鍡樻珕闁稿﹦鏁婚弻銊モ攽閸℃侗鈧顭胯閸ㄥ爼寮诲澶嬬叆閻庯綆鍋嗛ˇ銊х磽娴ｈ櫣甯涢柣鈺婂灦閻涱喚鈧綆鍠楅崐鐑芥煛婢跺鐏╅柨娑橆槹娣囧﹪鎮欓鍕ㄥ亾閺嵮€鏋栭柨鏇炲€哥粈瀣亜閹哄棗浜鹃悗娈垮枛濞尖€愁潖濞差亜绀冮柛娆忣槹閸庢捇姊洪悷鏉挎毐婵炲鍏橀獮澶愬箻椤旇姤娅嗘繝娈垮枟閸旀帞鑺辨繝姘拺闁告繂瀚ⅹ闂佸憡鏌ㄩ柊锝夊箚瀹€鈧埀顒婄秵閸犳鎮″☉銏＄厱闁斥晛鍠氬▓妯肩磼閻樺搫鍚归柍褜鍓氶鏍窗閺嶎厽鍋夊┑鍌滎焾閺勩儵鏌嶉埡浣告殲濠殿垱鎸抽幃宄扳枎韫囨搩浠鹃梺绋匡躬閺€杈╂崲濠靛棌鏋旈柛顭戝枟閻忓秹鏌ら崹锕€娲﹂悡鏇㈡煛閸愶絽浜鹃梺鎼炲妼濞硷繝骞冩导鎼晩闁兼亽鍎遍崝鍛存⒑闂堟稓绠為柛銊ヮ煼閹線宕奸妷锔规嫼闂佸憡绋戦敃锝囨闁秵鐓曢柕濠忕畱閳绘洘銇勯姀锛勬噰鐎殿喗鎸虫慨鈧柍閿亾闁瑰嘲鎼埞鎴︽倷鐠洪缚鏁垮銈嗘肠閸曨収娲搁梺瑙勫婢ф鎮￠弴銏″€甸柨婵嗛娴滄繄鈧娲栭惌鍌炲蓟?WARP 闂傚倸鍊搁崐鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈡晝閳ь剛澹曟繝姘厱闁斥晛鍟伴埥澶愭煕鐏炶濮傞柡宀嬬秮閺佹劙宕ㄩ鑺ュ闂備礁鎼Λ娆戝垝閹捐钃熺€广儱鐗滃銊╂⒑閸涘﹥灏甸柛鐘崇墪椤曪絾绻濆顑┾晠鏌曟径鍫濈仾妞ゎ偄绉撮埞鎴﹀煡閸℃浠╅梺璇茬箲缁诲牓銆侀弮鍫晣闁靛繆妾ч幏娲⒑閸涘﹦缂氶柛搴ㄤ憾瀵娊鏌嗗鍡欏幈闁诲函缍嗛崑鍛暦瀹€鍕厸鐎光偓鐎ｎ剛袦闂佽鍠掗弲婊冾焽韫囨稑鐓涘ù锝呭閻庡瓨绻濋悽闈浶為柛銊ヮ煼楠炲﹪骞樺鍕☉閳诲酣骞樺畷鍥╂澖闂備線娼ц墝闁哄懏绋撶划濠氭偡閹冲﹤缍婇弫鎰板川椤旇棄鏋戦梻浣风串缂嶁偓濡炴潙鎽滈幑銏犫槈濞嗘劗绉堕梺鍛婃寙閸曨偂杩樺┑锛勫亼閸娧呭緤閼测晛鍨濇繛鍡楃箳閺?
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
