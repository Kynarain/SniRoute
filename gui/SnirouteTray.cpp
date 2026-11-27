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

// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鐘栄囨煕鐏炲墽鐓瑙勬礀閳规垿顢欓惌顐簻閻ｇ兘顢楅崟顐㈠亶闁诲海鏁哥涵鍫曞磻閹捐埖鍠嗛柛鏇ㄥ墰椤︻喖顪冮妶搴′簻妞わ箓浜堕崺鈧い鎺戝濞懷囨煏閸埄鐒炬い鏇秮瀹曞ジ寮撮悙娈垮悈闂備胶绮敋闁诲繑鑹捐閹兼番鍨荤弧鈧梺姹囧灲濞佳勭閳哄懏鐓欐繛鑼额唺缁ㄧ晫鈧灚婢橀敃顏堝箠閻愬搫唯鐟滃繗銇愰悙顒傜瘈闁汇垽娼у瓭濠电偛鐪伴崐婵嬪箖閸ф鏁嬮柍褜鍓熷璇测槈濡攱顫嶅┑鈽嗗灠閸氬骞夋總鍛娾拺缂備焦蓱鐏忛潧鈹戦悙鈺佷壕闂備礁鎼惌澶岀礊娓氣偓閻涱喖顫滈埀顒勭嵁鎼达絿椹抽悗锛卞啩澹曞┑掳鍊撻梽宥嗙濠婂牊鐓ユ繝闈涙閸ｄ粙鏌ㄥ☉妯夹ょ紒杈ㄥ笚濞煎繘濡搁妷锕佺檨闂備浇顕栭崰鎺楀疾閻樿绠犻柨鐔哄Т鍥撮梺鎼炲劘閸斿秹鎯侀幒妤佲拻闁稿本鐟чˇ锕傛煙绾板崬浜伴柟顖氼槹缁虹晫绮欑捄銊у炊闂備礁鎼粙渚€宕㈤幆褏鏆ゅù锝嗘偠閳ь剚甯掗～婵嬫晲閸涱剙顥氬┑掳鍊楁慨鐑藉磻閻愬灚鏆滈柨鐔哄Х瀹撲線鎮楅敐搴濈按闁衡偓娴犲鐓曢柍閿亾闁哄懏绮庣槐鐐哄箣閿旇В鎷婚梺绋挎湰閼归箖鍩€椤掑嫷妫戞繛鍡愬灩椤繄鎹勯搹鐟板Е婵＄偑鍊栫敮鎺楀磹閸涘﹦顩锋繝濠傜墛閻撶姵绻涢懠棰濆殭闁诲骏绻濋弻锟犲川椤撶姴鐓熷銈冨灪瀹€鎼佸春閳ь剚銇勯幒鎴濐仼闁藉啰鍠愮换娑㈠箣閻愬灚鍣紓浣叉閸嬫捇姊绘担鍛婂暈婵炶绠撳畷婊冾潩閼搁潧浠у銈呯箰閻楀﹪鍩涢幋锔界厽闁绘梻顭堥ˉ瀣煟閿濆骸寮柡灞剧洴婵℃悂濡堕崶鈺冨幆闂備胶绮笟妤呭窗濮樿泛鐒垫い鎺戯功缁夌敻鏌涢悩鎰佹疁鐎殿喗褰冮埞鎴犫偓锝庡亞閸樻捇鎮峰鍕煉鐎规洘绮撻幃銏ゆ偂鎼达絿鏆?0=闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珕闂佽姤锚椤︻喚绱旈弴鐔虹瘈闁汇垽娼у瓭闂佹寧娲忛崐妤呭焵椤掍礁鍤柛锝忕秮婵℃挳宕ㄩ弶鎴犵厬婵犮垼娉涢惉濂告儊閸喓绡€闁汇垽娼у瓭闂佺锕︾划顖炲疾鐠鸿　妲堥柕蹇ョ磿閸橀亶鏌ｆ惔顖涒偓銉╁礋椤掑倸缍冮梻鍌欑閹碱偊寮甸鈧叅婵☆垰鍚嬪畷鍙夌箾閹寸偟鐭ゆ俊鎻掝煼閺岋絾鎯旈姀鐘叉瘓闂佸憡鎸鹃崰搴敋?1=闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪Ч閿濈偛鈹戠€ｎ偅娅囬梺绋跨焿婵″洨绮欒箛鏃傜瘈闁靛骏绲介悡鎰版煕閺冣偓閻楃姴鐣烽幋锕€围濠㈣泛顑囬崢鎼佹⒑閹肩偛鍔楅柡鍛☉閻剟姊绘担铏瑰笡閻㈩垱甯￠弫鍐Ψ閳轰絼锕傛煕閺囥劌鏋ら柣銈傚亾闂備礁婀遍崑鎾诲箚鐏炶娇娑㈡晸閻樺啿鈧敻鎮峰▎蹇擃仾缂佲偓閳ь剟鎮楀▓鍨灈濠⒀冮叄楠炴垿濮€閻橆偅顫嶉梺闈涚箳婵兘顢橀崫鍕ㄦ斀闁绘劕寮堕ˉ婊呯磼缂佹ê绗氱紒鍌涘浮閹虫牠鍩￠崘顏庣闯?2=闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珕闂佽姤锚椤︻喚绱旈弴鐔虹瘈闁汇垽娼у瓭闂佹寧娲忛崐妤呭焵椤掍礁鍤柛锝忕秮婵℃挳宕ㄩ弶鎴ｆ憰闂侀潧顧€婵″洭宕ｉ崱妞绘斀闁绘绮☉褎淇婇锝囨噭鐎垫澘瀚板畷鐔碱敇濞戞ü澹曢柣鐔哥懃鐎氼厾绮堥埀顒勬倵濞堝灝鏋涘褍閰ｉ獮鎴﹀閻橆偅顫嶉梺闈涚箳婵兘顢橀崫鍕ㄦ斀闁绘劕寮堕ˉ婊呯磼缂佹ê绗氱紒鍌涘浮閹虫牠鍩￠崘顏庣闯濠电偠鎻徊浠嬪箹椤愶絿澧℃繝鐢靛Х閺佹悂宕戝☉銏℃櫇妞ゅ繐瀚烽崵鏇炩攽閻樺疇澹橀柛妤佸▕閺岋綁寮崼顐ｎ棖缂備焦鍔楅崑娑㈠煘閹达附鍊烽柡澶嬪灩娴犳悂姊洪悡搴ｇШ缂佺姵鐗犻獮鍡欎沪鐟欙絾鏂€闁诲函缍嗛崑鈧柟?
// 闂?warp-cli status 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬鐎广儱顦粈瀣亜閹哄秶鍔嶆い鏂挎喘濮婄粯鎷呯憴鍕哗闂佺瀛╁钘夌暦濠婂啠鏋庨柟瀛樼箥濡粓鎮峰鍛暭閻㈩垱顨婇幃鈥斥槈閵忥紕鍘遍梺闈涱檧缁蹭粙宕濆鑸电厽闊浄绲奸柇顖炴煛鐏炲墽銆掗柍褜鍓ㄩ幀銉╁礋椤掑倸绲介梻浣稿⒔缁垶鎮ч悩璇茶摕闁靛鍎弨浠嬫煕閳╁厾顏勨枍閿濆鈷戦柛婵嗗椤ョ娀鏌涚€ｃ劌鈧繂顕ｆ繝姘櫢闁绘ê寮堕妵婵囩箾鏉堝墽鎮兼い顓炵墦閿濈偤顢曢敂瑙ｆ嫼闂備緡鍋嗛崑娑㈡嚐椤栨稒娅犳い鏇楀亾闁哄本绋戣灒闁煎鍊楅悡澶愭倵鐟欏嫭绀冮柛銊ユ健閻涱喖螣閼测晝锛滃┑鈽嗗灣閸樠囩嵁閸儲鈷掑ù锝囩摂濞兼劖绻濋姀鈭╂垿寮查懜鍨劅闁靛鍎抽ˇ顐︽⒑閸濆嫷妲归柨娑欐礋閹兘鏌囬敂鎯у汲闂備礁澹婇崑渚€宕规繝姘辈妞ゆ劧闄勯埛鎴︽偣閸ワ絺鍋撻搹顐や憾闂備焦瀵уú蹇涘垂娴犲宓侀柛鎰╁妷閺嬪酣鏌熺€电浠滅痪缁㈠灦閺屸剝寰勭仦鎴掓埛婵犫拃鍕垫疁闁诡垰鐗愮粻娑樷槈濞嗗本瀚藉┑鐐舵彧缂嶁偓婵☆偄瀚板畷銉ㄣ亹閹烘挾鍘遍梺缁樏崯鎸庢叏婢舵劖鐓欐鐐茬仢閻忚尙鈧娲栭妶鍛婁繆閻戣棄唯闁靛闄勯崯娲⒒閸屾瑦绁版繛澶嬫礋瀹曚即骞樼€涙ê寮块柣搴ㄦ涧閹芥粓鎯岄崱娑欏€甸柨婵嗛閺嬬喖鏌嶉柨瀣诞闁哄本鐩、鏇㈠Χ閸涱喚浜栭梻渚€娼уΛ娆戞暜閹烘绠掗梻浣虹帛閻熴垽宕戦幘缁樼厱闁靛鍊曞畵鍡涙煕閳哄啫浠辨鐐差儔閺佸倿鎸婃径澶嬬潖濠碉紕鍋戦崐鏍礉閿曗偓椤潡鍩€椤掆偓椤法鎲撮崟顒傤槹濠殿喖锕︾划顖炲箯閸涙潙宸濆┑鐘插€瑰▓妯衡攽閻愬瓨灏扮痪鏉跨Ч閵嗗啯绻濋崒婊勬闂佽崵鍠愭竟鍡涘汲閿曞倹鐓曢柕澶嬪灥閺堫剟藟閳ユ枼鏀介幒鎶藉磹瑜忓濠冪鐎ｎ亞鏌堝銈嗙墱閸嬬偤宕戦崒娑氱闁糕剝蓱鐏忎即鏌涙繝鍌滀粵缂佺粯鐩獮瀣倷閸偄娅ч梻浣虹帛鐢偤寮查銈嗩潟闁圭儤顨嗛崑鎰版煣韫囷絽浜滈梻澶婄Т椤啴濡堕崘銊ノ╅梺绋挎捣閺佸鐛崘銊㈡瀻闁瑰灝鍟弲婊堟⒑閸涘﹥澶勯柛妯挎濡叉劕鈻庨幘绮规嫽婵炶揪绲介幉锟犲箚閸儲鍋犳慨妯煎帶娴滄壆鈧娲樼换鍌炴偩濠靛绀嬫い鎰╁€曞銊モ攽閻橆喖鐏辨繛澶嬬婢ф繈姊洪崫鍕靛剰闂佸府缍佸濠氭偄鐞涒€充壕闁汇垺顔栭悞鎯归悪鍛暤闁哄本绋掗幆鏃堝閳藉棙缍桽QUE 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰亜閺冨倹鍤€濞存粓绠栭弻娑㈠箛闂堟稒鐏堥悗鐟版啞缁诲啴濡甸崟顖氬唨闁靛ě鈧Σ鍫ユ煙閸忓吋缍戦柨鏇樺€濋崺鐐哄箣閻愯尙鐤囬梻浣侯焾閿曘儱煤閻旇偐宓侀煫鍥ㄦ磵閸嬫捇鏁愭惔鈩冪亪闂佸搫顑勭粈渚€鍩為幋锔藉亹闁告瑥顦ˇ鈺呮⒑缂佹ɑ灏版繛鍙夘焽閹广垹鈽夐姀鐘殿吅濠电娀娼уΛ婊堝春閻愮儤鈷戦柛婵嗗閸庡秹鏌涙繝鍐疄鐎殿喛顕ч濂稿炊閳哄喛绱叉繝纰樻閸ㄥ崬顪冮崹顔ユ盯宕橀瑙ｆ嫼闁荤姴娲犻埀顒冩珪閻忓秴顪冮妶鍐ㄥ闁挎洦浜滈锝夘敃閿曗偓缁犳稒銇勯幘璺烘瀻闁告柨鎳樺娲倷閽樺濮曢梺鍛婃尰閼归箖鈥﹂崶顒€绀堢憸蹇曞娴犲鐓曢悘鐐插⒔閹冲懏銇勯敂濂告闁靛洤瀚伴弫鍌滄嫚閼碱兛鍖栫紓鍌欐祰娴滎剟宕戦悙鍨床婵犻潧顑呴悙濠勬喐閹达箑绠洪柡鍥╁亹閺€浠嬫煟閹邦厽缍戦柣蹇ョ畵閺屻劑寮村Δ浣圭彅闂侀潧妫欑敮妤冩崲濠靛棭娼╂い鎾跺Т楠炴姊绘担鐑樺殌闁宦板妼椤繘鎮烽幍铏€洪梺鍛婁緱閸斿爼鎮烽柇锔惧弳闂佸壊鍋侀崹濠氬汲椤愶附鈷戦柛婵嗗濞笺倝鏌涢埄鍐噮闁哄鎮傚娲箰鎼达絿鐣甸梺鐟板暱缁夌懓鐣峰▎鎰浄閻庯綆鍋€閹锋椽姊洪崨濠勨槈闁挎洏鍊濆畷鐢割敆閸曨剛鍘遍柣搴秵娴滄繈宕虫禒瀣厵妤犵偛鐏濋悘鈺呮煃閽樺妲搁柍璇查叄楠炲洭顢欓幏妯稿€濆缁樻媴閸涘﹤鏆堥梺鍦焾濡繂鐣峰鈧俊姝岊槻閻㈩垱鎸荤换婵嬫偨闂堟刀銉︺亜閿濆牊顥夐崡閬嶆煕濞戞瑦缍戠痪鎯ь煼閺屾稑鈽夊Ο鍏兼喖闂佺顑戠槐鏇㈠箟濮濆瞼鐤€婵炴垶顭囬ˇ顐︽⒑閸︻厼鍔嬮柟绋垮⒔瀵囧焵椤掑嫭鈷戦柟鑲╁仜閸旀﹢鏌涢弬璺ㄐ㈤柍璇茬Ч瀹曞爼顢楁担鍙夊缂傚倷绀侀鍡欌偓绗涘喛鑰块柟娈垮枤绾惧ジ鎮楅敐搴′簼鐎规洖鐬奸埀顒冾潐濞插繘宕规禒瀣祦闁圭儤鍤﹂弮鈧粭鐔煎炊椤垶顥婃繝纰夌磿閸嬫垿宕愰弽顓炵闁硅揪绠戦崹鍌炴煟閹寸伝顏嗘閻愮儤鐓曢柡鍥ュ妼閻忥繝鏌ｉ幘瀵告噮缂佽鲸甯為埀顒婄秵閸嬪嫰顢氬鍫熺厵闁告瑥顦伴崐鎰版煛瀹€鈧崰搴ㄦ偩閿熺姵鐒介柨鏇楀亾闁告挸鐖奸幃妤€鈻撻崹顔界亪濡炪値鍙冮弨杈ㄧ┍婵犲洦鍊婚柤鎭掑劜濞呭棝姊虹拠鈥崇€婚柛鎰典憾閸熷姊婚崒姘偓鎼佸磹閻戣姤鍊块柨鏇炲€哥壕鍧楁煕閹邦垼姊块柣鐔煎亰濞尖晠鎮规ウ鎸庮仩闁挎稒鐩娲川婵犲啫顦╅梺鎼炲姂娴滆泛鐣烽幇顔藉磯閻炴稈鍓濋弬鈧梻浣虹帛钃辩憸鏉垮暣椤㈡棃顢曢妶鍥紡闂佽鍨庨崘鈺佲偓顖涚箾閿濆懏鎼愰柨鏇ㄤ簼娣囧﹪宕奸弴鐐碉紲濠殿喗锕╅崑鍕夊顑芥斀闁挎稑瀚禍濂告煕婵炑冨缁诲棝鏌涢妷顔煎闁哄绶氶幃褰掑炊瑜庨埢鏇㈡煕閵堝棙绀€闁宠鍨块幃鈺冣偓鍦Т椤ユ繈鎮楅悷鐗堝暈缂佽鍟存俊鐢稿礋椤栨氨顔掑┑掳鍊愰崑鎾绘倵濮樼厧澧寸€规洘顨嗙换婵嬪炊閵娿垺瀚藉┑鐐舵彧缁蹭粙骞夐敓鐘茬柈闁绘劗鍎ら悡娆戠磼鐎ｎ偄顕滄い銉ｅ灮閳ь剝顫夊ú锔界濠靛绠柛娑欐綑缁€?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煕閹邦垰绱﹂柣銏㈢帛娣囧﹪鎮欓鍕ㄥ亾閺嶎厽鍋嬫俊銈呭暟閻瑩鏌熼悙顒傛菇闁逞屽墮閹虫ê鐣烽敐鍡楃窞鐎光偓閳ь剟鎯侀崼銉︹拻闁稿本姘ㄦ晶娑氱磼鐎ｎ偄娴柡浣哥Ч瀵噣宕奸悢鍝勫箻?status 闂?Connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴闇夐柨婵嗙墛椤忕姷绱掗埀顒佺節閸屾鏂€闂佺粯蓱瑜板啯绂嶉悙鐑樼厽闁圭儤姊瑰▍鏇㈡煙閸欏鎽冪紒鐘崇洴瀵挳鎮欓懠顑垮枈闂傚倷绀侀幖顐﹀疮椤愶附鍋嬮柛鈩冪懅娑撳秵銇勯弴妤€浜鹃梺鍝勭焿缁犳垼鐏掓繛鎾村嚬閸ㄨ鲸鐡忛梻鍌欒兌绾爼寮插┑瀣；婵炴垟鎳囬埀顒佹瀹曟﹢顢欓崲澹洦鐓曢柍鈺佸枤濞堟﹢鏌ｉ悢鏉戝婵﹨娅ｇ划娆撳礌閳ュ厖绱ｇ紓鍌氬€哥粔鎾晝閵堝鍋╅柣鎴ｆ缁狅絾绻濋崹顐㈠鐎点倖妞藉娲焻閻愯尪瀚板褍顕埀顒侇問閸犳牠鈥﹀畡閭﹀殨闁圭虎鍠楅崑鍕煣韫囨凹鍤冮柛鐔烽叄濮婄粯鎷呯粙娆炬闂佺粯鎸搁悧鎾崇暦娴兼潙鍐€闁靛绠戝鍧楁⒑閸撴彃浜濇繛鍙夛耿閸╂盯骞嬮敂鐣屽幈濠电偞鍨堕敃顐Ｎ涘顒夌唵閻熸瑥瀚搁懓鍧楁煛鐏炲墽鈯曠紒缁樼箞瀹曟帒鈽夊Ο瑙勬▕闂佽瀛╅鏍窗閺嶎厸鈧箓鎮滈挊澶岀暫闂侀潧绻堥崐鏍磻閸岀偛绠归弶鍫濆⒔缁夌敻鏌涢悢濂夊剱濞ｅ洤锕幃娆擃敂閸曘劌浜鹃柡宥庣亹濞差亜鍐€妞ゆ挾鍋熼悿鍥椤愩垺澶勯柧鏂款儐瀵板嫰骞囬鈧崬銊╂偡濠婂嫮鐭婇柕鍡樺笚缁绘繂顫濋鐘插妇闂備礁澹婇崑鍛崲閸愵喖闂柧蹇撴贡绾惧ジ鏌涚仦鎯у毈闁稿孩妫冮弻锛勪沪閸撗勫垱濡ょ姷鍋為…鍥焵椤掑嫭娑ч柟璇х節瀹曟艾鈽夊Ο婊勬閹晠妫冨☉妤冩崟缂傚倷鑳剁划顖滄暜閿熺姴鏄ラ柍褜鍓氶妵鍕箳閹搭垰濮涚紓浣割槺閺佸寮诲☉銏″亹鐎规洖娲ら埅鐟扳攽閻愭潙绲绘繛灏栤偓鎰佹綎缂備焦蓱婵挳鏌涘☉姗堝伐闁哄棗鐗嗛埞鎴︽倷妫版繂娈濈紓浣哄У閹告悂鎮鹃悜钘夌骇婵炲棛鍋撻崓鐢电磽娴ｈ顥旈柛銊︽緲鐓ら柨鏃堟暜閸嬫挻鎷呴崘銊ゆ睏闂侀€炲苯澧紒瀣笩瑜版粓姊洪悷鏉挎Щ妞ゎ厼鍢查～蹇涙惞閸︻厾鐓撻梺鍦焾鐎涒晠骞忔潏鈺冪＜闁绘劦鍓氱欢鑼磼婢跺本鍤€闁伙絿鍏橀、娑橆潩閸忕厧鍏婇梻浣哄帶閹芥粓寮幖浣稿偍闂侇剙绉甸埛鎴︽⒒閸喍绶辨俊顖氱墦閺屾盯濡搁敃鈧埢鏇㈡煙椤旀儳浜炬繛鐓庣箻婵℃悂鏁愰崟顓犳晨闂傚倷绶氬褔篓閳ь剛绱掗悩鍐茬伌鐎规洦鍋婇幃婊兾熺拠鎻掍紟濠电姷鏁告慨鎾疮椤栫偛桅婵犻潧鐗冮崑鎾舵喆閸曨剛锛橀梺鍛婃煥缁夊爼骞戦姀鐘斀閻庯綆浜為ˇ鏉款渻閵堝棗鍧婇柛瀣尰缁绘盯宕ㄩ崶褎澶勯柍閿嬪笒闇夐柨婵嗙墱濞兼劙鏌涚€ｎ剙鈻堥柡灞剧洴瀵剛鎹勯妸鎰剁悼缁辨帞绱掑Ο灏栧缂備胶绮换鍫濈暦閹烘围闁告侗鍣崥娆忊攽閿涘嫬浜奸柛濠冪墵瀹曟繆顦寸紒顔碱煼楠炲鈹戦崶鈺佸毐婵犵數濮烽弫鍛婃叏娴兼潙鍨傞柣鎾崇岸閺嬫牗绻涢幋鐐电煂婵?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簻椤掋垺銇勯幇顏嗙煓闁哄被鍔戦幃銏ゅ传閸曟垯鍨婚惀顏堝箚瑜滈悡濂告煛鐏炲墽鈽夐柍钘夘樀瀹曪繝鎮欑€甸晲澹曢梻鍌欑绾绢參骞婇幘鍓佺煋鐟滅増甯掗拑鐔烘喐閹寸姷涓嶆繛鎴烇供濞尖晠鏌涜箛姘汗缂佸鍏橀弻?sniroute.cmd 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴闇夐柨婵嗩槹娴溿倝鏌ら弶鎸庡仴婵﹥妞介、妤呭焵椤掑倻鐭撻柣銏㈩焾绾惧鏌ㄩ悢鍝勑ｉ柣鎾寸懇閺岀喖顢涘鍐炬毉濡炪們鍎遍ˇ浼淬€冮妷鈺傚€锋い蹇撳娴煎牓姊洪崫鍕缂佸鎳撻悾鐑筋敃閿曗偓缁€瀣亜閹邦喖鏋庡ù婊冨⒔閹叉悂鎮ч崼婵堢懆缂?warp on/off闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢悰鈥充壕闁汇垺顔栭悞鎯归悩娆忔处閳锋帡鏌涢弴銊ヤ簻妞ゅ浚鍙冮弻鈥崇暆鐎ｎ剛鐦堥悗瑙勬礃閿曘垺淇婇幖浣肝ㄩ柕蹇曞С婢规洟姊洪悡搴綗闁稿﹥顨嗙粙澶婎吋婢跺鍘搁梺绋挎湰娓氭宕曢妷鈺傜厽闁哄倹瀵чˉ銏ゆ煙椤旀娼愰柕鍫畵楠炴劖鎯旈璇叉憢闂傚倷娴囬褏鎹㈤幇顒傛殾妞ゆ帒瀚ч埀顒佹瀹曟﹢顢欓挊澶夌敾闂備礁鎼ˇ鎵偓绗涘唭鎺楀箛椤掑瀵岄梺闈涚墕妤犵顕ｉ妸锔剧濠㈣泛顑嗙粈瀣殽閻愭潙鐏寸€规洜鍠栭、娑樷槈濡湱鐥呴梻鍌欑閹碱偊藝娴兼潙鍨傞柣銏ゆ交缂嶆牠鏌ｅΟ鍏兼毄缁惧彞绮欓弻娑氫沪閸撗勫櫘闂佸憡鏌ㄧ粔褰掑蓟濞戙垹妫橀柣鎰级閸ｈ櫣鈧娲栧鍓佹崲濠靛顥堟繛鎴濆船閸撲即姊洪挊澶婃殶闁哥姵鐗犲濠氬即閻旇櫣顔曢梺鍓茬厛閸犳牗鎱ㄦ径濞炬斀闁斥晛鍟徊濠氭煟濡も偓閿曘儳绮氭潏銊х瘈闁搞儴鍩栭弲顒€鈹戦悩缁樻锭婵☆偅顨婇獮鍡涙倷鐎靛摜顔曢梺鍝勵槹閸ㄧ敻骞冮幋婢濈懓顭ㄩ崨顏勪壕婵?cli 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涱儐椤ュ牊绻涢幋婵嗚埞闁告挻澹嗙槐鎺楁倷椤掆偓閸斻倖銇勯鐘插幋鐎殿喖顭烽弫鎰緞婵犲嫮鏉告俊鐐€栫敮鎺楀磹妞嬪骸鍨濋柣銏犳啞閳锋垿鏌涘┑鍡楊仾妞ゃ儳鍋熺槐鎺楊敊閼恒儱纾抽悗娈垮枛椤兘骞冮姀銈嗘優闁革富鍙忕槐鏌ユ⒒娓氣偓濞佳呮崲閹烘挻鍙忛柣銏犳啞閸婂爼鏌涢鐘插姕闁抽攱鍨剁换娑㈡嚑妫版繂娈Δ鐘靛仦椤ㄥ棝濡甸崟顖毼╅柕澶涘瘜濡附绻濈喊澶岀？闁轰浇顕ч悾鐑芥偄绾拌鲸鏅┑顔斤供閸撴瑩寮妶澶嬧拻濞撴埃鍋撴繛浣冲棗娅ｉ梻浣告啞娓氭宕归崡鐐╂灁濠靛倸鎲￠埛鎺楁煕鐏炲墽鎳呮い锔肩畵閺岀喖鎮┃鍨偓鏍箠瀹€鍕拻濞达絿鍎ら崵鈧梺瑙勭ゴ閸撴繄鎹㈠☉娆戠瘈闁搞儮鏅涚粊锕傛⒑閸涘﹤濮﹂柛鐘愁殜瀹曟劖绻濆顓犲弮濠碘槅鍨拃锕€危閸濄儮鍋撶憴鍕碍婵☆偅顨婇崺鐐哄箣閿旂粯鏅╅梺鍝勫€堕崐娑㈠焵椤掑啯纭堕柣銉邯椤㈡﹢鎮埀顒勬倿妤ｅ啫纭€閺夊牄鍔婃禍婊堟煛閸ヮ煈娈斿ù婊勫劤閳规垿顢欑涵閿嬫暰濠碉紕鍋樼划娆撴偘椤曗偓瀵粙顢橀悢灏佸亾闁垮绠鹃柡澶嬪焾閸庡繐霉濠婂牏鐣洪柡灞糕偓鎰佸悑閹肩补鈧磭顔愰梻浣规偠閸斿矂宕愰崸妤€钃熸繛鎴欏灪閺呮粓鎮归崶銊ョ祷缂佺姴缍婂娲箹閻愭彃顬夌紓浣筋嚙閻楁挸顕ｆ繝姘櫢闁绘灏欓ˇ銊╂⒑闂堟丹娑㈠礃閻愵剚鐦掔紓鍌氬€搁崐鐑芥嚄閼稿灚鍙忛柛顭戝亞缁犳儳鈹戦悩鍙夋悙闁藉啰鍠栭弻銊モ攽閸♀晜笑缂備胶濮电粙鎺楀Φ閸曨垰绫嶉柛銉仢閹惧瓨鍙忛柨婵嗘噹椤忊晜銇勯鍕殻濠碘剝鍎肩粻娑㈠箻閼告晫鎺楁⒒娴ｄ警鐒炬い鎴濇嚇閺佸啴濡舵径娑氱◤?
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


// 闂?winget 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰效婵犫偓娓氣偓濮婅櫣绱掑Ο铏逛紘濠碘槅鍋勭€氼喚鍒掓繝姘亹缂備焦顭囬崢鐢告⒑鐠団€崇€婚柛鎰ㄦ櫓閸熷牓姊绘担瑙勫仩闁稿孩妞藉畷婊冣枎閹寸姳绗夊銈嗘磵閸嬫捇鏌?Cloudflare WARP闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亜顒㈡い鎰Г閹便劌顫滈崱妤€鈷掗梺缁樺笧閺咁偊骞夌粙娆惧悑闁割偒婢€缂冩洖鈹戦悩鑼闁哄绨遍崑鎾诲即閵忕姷鐤囬柟鑹版彧缁蹭粙藟濮樿埖鐓熼柣鏃傤焾濞呯偟鎲搁悧鍫濈瑲闁稿鍔楅埀顒傛嚀婢瑰﹪宕伴弽顓炵闂侇剙绉甸埛鎴︽煟閻斿憡绶茬紒鐙欏洦鐓ラ柡鍥悘鍙夘殽閻愭彃鏆欓摶鏍煕濞戝崬鏋熸繛鍛矒閺岀喖鎳栭埡鍕婂鏌嶈閸撴瑩宕捄銊т笉闁挎繂顦伴埛鎺懨归敐鍕劅闁绘帞鍋撻妵鍕箣濠靛浂妫﹀Δ鐘靛仦閸旀牠濡堕敐澶婄闁靛ě鍛€梻鍌欑濠€閬嶆惞鎼淬劌绐楅柡宥庡亞閻棝鎮楅敐搴℃灍闁绘挻娲熼弻宥夊煛娴ｇ绠诲銈呯箰閻楀﹪宕愰崼鏇熷€甸柣銏㈡暩閵嗗﹪鏌涚€ｎ偅灏柍缁樻崌瀹曞綊顢欓悾灞奸偗濠碉紕鍋戦崐銈夊储妤ｅ啫绀傛慨妞诲亾闁绘侗鍣ｅ畷姗€顢欑喊杈ㄧ秱婵＄偑鍊ら崑鎺楀储妤ｅ啫绀堥柕濠忓缁犻箖寮堕崼婵嗏挃缂佸鍓氶妵鍕敃閵忊晜鈻堥悗娈垮枦濞呮洜鎹㈠┑瀣倞闁靛鍨虹€氳棄鈹戦悙鑸靛涧缂佹彃娼￠幃娲棘濞嗗墽鍔烽梺褰掑亰閸樺墽寮ч埀顒勬⒑闂堟稓绠氶柛鈺佸楠炲鏁冮埀顒勬偪妤ｅ啯鐓犵紒瀣硶娴犳盯鏌涙惔锝呮灈闁哄被鍔岄埞鎴﹀幢濡櫣鐛╅梻浣虹帛鐢帡宕ョ€ｎ剚宕叉繝闈涱儏椤懘鏌ㄥ┑鍡橆棤闁靛棙鍔欓弻銈夋偡閻楀牊鎷辨繛锝呮搐閿曨亪鐛幘璇茬闁哄倸绨遍崣娲⒒娴ｅ憡鎯堥柣顒€銈稿畷浼村冀椤撶姴绁﹂梺绯曞墲閸戠懓危妤ｅ啯鈷戦柟绋挎捣閳藉绱掓径灞惧殌闁伙絿鍏橀、娑㈡倷閼碱剦鍟囧┑鐐舵彧缁蹭粙宕㈠鍫濈婵炲樊浜濋埛鎴犵棯椤撶偞鍣瑰ù婊冪秺閺屾稓鈧綆鍋嗗ú鎾煕閳规儳浜炬俊鐐€栫敮濠囨嚄閼稿灚娅犻梺顒€绉甸悡鏇㈡煙閻戞ɑ灏紒妤佸浮閺岋綁鏁愰崱娆戠杽閻庤娲滈崰鏍х暦瑜版帩鏁冮柨婵嗘瑜扮喖姊婚崒姘偓鎼佲€﹂鍕；闁告洦鍊嬪ú顏呮櫆闁告挆鍛幆闂備胶鎳撻顓熸叏閻㈢绀傛繝濠傜墛閻撳啰鎲稿鍫濈闁绘柨顨庡鏍煣韫囨挻璐＄痪鎯у悑娣囧﹪顢涘顓熷創濡炪値鍋呴崹鍧楀蓟閿濆牏鐤€闁规儳澧庨澶愭⒑閼姐倕鏆遍柡鍛Т閻ｅ嘲煤椤忓嫮鍔﹀銈嗗笂闂勫秵绂嶅鍫熺厵闁告繂瀚ˉ婊兠瑰鍕姢妞ゎ亜鍟存俊鑸垫償閳ュ磭顔掗梻浣侯焾缁绘垿鏁嬪銈嗘穿缁插潡骞忛悩瑁佸湱鈧綆鍋掑鏃€绻濈喊妯活潑闁搞劌鐖煎銊╂焼瀹ュ棗鈧潧鈹戦悩宕囶暡闁抽攱鍨归幉鎼佹偋閸繄鐟ㄦ繛瀛樼矆閸楁娊寮诲☉銏犵閻庣數纭舵慨鍥╃磽娴ｇ鈧湱鏁敓鐘靛祦婵せ鍋撴い銏＄懅閹叉挳鏁愰崱妤婁紪濠电姷鏁搁崑鐐差焽濞嗘挸瑙﹂悗锝庡枟閸嬶紕鎲歌箛鏇炲灊濠电姴娲﹂崐濠氭煠閹帒鍔ら柛姗€浜跺娲传閸曨偀鍋撶粙娆剧劷闁跨喓濮撮惌妤呮煕閺囥劌澧绘繛鍫滅矙閺岋綁骞囬浣叉灆濠碘€冲级閹倿骞楅崼鏇炲唨妞ゆ挾鍠撻崢鍗炩攽閻愭潙鐏ョ€规洦鍓熼悰顕€寮介銈囷紲闂佸綊鍋婇崢楣冨煡婢跺浜滄い蹇撳閺嗭絽鈹戦垾宕囧煟鐎规洖宕灒闁绘垶蓱椤斿倿姊婚崒娆掑厡妞ゎ厼鐗撻、鏍炊椤掆偓缁愭骞栧ǎ顒€鈧绂嶉挊澶嗘斀闁绘劕鐡ㄧ亸浼存煕閳轰礁鏆ｇ€规洘鍨块獮妯尖偓娑櫭?
// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍讲鏋欓梺缁樺笩婵倗鎹㈠☉銏犲耿婵☆垵顕х喊宥夋煟閻斿摜鎳曠紒鐘虫崌楠炲啫螖閸涱喗娅滈柟鑲╄ˉ閸撴繈鎮樺澶嬧拺闁硅偐鍋涢埀顒佹礋閹崇喖顢涘┑鍫滅胺闂傚倷鐒︾€笛呮崲閸岀偛绠犻幖娣妼濮规煡鏌ｉ幇顔煎妺闁绘挻娲熼弻鏇㈠醇濠靛洤娅ら梺鐟板悑閸旀瑩寮诲鍥ㄥ枂闁告洦鍋嗘导宀勬⒑鐠団€虫灀闁哄懐濞€閻涱噣骞樼拠鑼唺闂佸湱鍋撻幆宀勫疮閳ь剟姊婚崒姘偓椋庣矆娴ｅ搫顥氭い鎾跺Т閸ㄦ繃銇勯弽顭戝殼婵炴垯鍨圭粈鍐┿亜閺冨洤顥嶉柟鑺ユ礋閹鐛崹顔煎闂佺粯顨嗙划宀勨€栨繝鍥╅柍鍝勫€甸幏娲⒑閸涘﹦鈽夐柨鏇樺劤娴滃憡瀵肩€涙鍘介梺缁樻煥瀵泛鈻嶆繝鍕ㄥ亾?UAC闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢悰鈥充壕婵炴垶鐟悞鐣岀磼閻樺崬宓嗛柡灞诲€濋獮蹇曚沪閸撗佲偓濠勭磽娴ｄ粙鍝洪悽顖涘笩閻忓啴姊洪崨濠傚闁哄懏绻嗛妵鎰板礌?闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞鐟滃繘寮抽敃鍌涚厽闁靛繈鍩勯悞鍓х磼閹邦収娈滈柡宀€鍠栭弻鍥晝閳ь剟宕濋幒鎾剁鐎瑰壊鍠曠花鑽ょ磼閻欏懐绋荤紒缁樼箞婵偓闁挎稑瀚悾鐢电磽閸屾氨孝闁挎洏鍊濋幃楣冩倻閽樺鍞堕梺鍝勬储閸斿秹寮查悙鐑樷拺闁圭瀛╃壕鐢告煕鐎ｎ偅灏伴柕鍥у閺佸倿鎸婃径妯活棆闂備胶鎳撶粻宥夊垂瑜版帒鐓橀柟瀵稿Л閸嬫捇鏁愭惔婵堟晼闂佺锕﹂崑銈夊箖瀹勯偊鐓ラ柛鎰碘偓顖嗗洦鐓欐い鏇楀亾婵炰匠鍥风稏婵犲﹤鐗嗛悞鍨亜閹烘垵顏╃紒鐘崇墬娣囧﹪濡堕崨顔兼閻庤鎸风欢姘跺蓟濞戔懇鈧箓骞嬪┑鍥╀邯濠电姵顔栭崰鏍煀閿濆拋娼栨繛宸簼閸嬶繝鏌℃径瀣嚋婵絾鍔欏缁樻媴缁嬭法鐩庣紓鍌氱С缁舵艾顕ｉ锕€绠涢柣妤€鐗嗛埀顒傚厴閺屾稑鈹戦崱妤冨几閻熸粌绉归垾锔炬崉閵婏箑纾繛鎾村嚬閸ㄤ即宕滈幎鑺モ拺闁告稑鈯曢鍕殾妞ゆ帒瀚悡姗€鏌熸潏楣冩闁稿鍔欓幃褰掑炊閸パ冩殨缂佹唻缍佸缁樻媴閸涘﹥鍠愰梺鍝ュТ缁夌懓鐣烽敓鐘茬闁芥ê顦遍悾娲⒑缂佹ê濮囬柟纰卞亞缁鎮烽幊濠傜秺閺佹劙宕ㄩ璺攨闂備礁鎲¤摫闁圭懓娲ら～蹇撁洪鍛闂侀潧鐗嗛幊蹇涙倵閻愵剛绡€闁靛繈鍨洪崵鈧┑鈽嗗亝閻熲晛顕ｉ弻銉ノㄩ柍鍝勫€婚崢浠嬫煙閸忚偐鏆橀柛濞垮€曢…鍥箛椤撶姷顔曢梺鍛婄懃椤︿粙鎮炴禒瀣厵妤犵偛鐏濋悘鑼偓瑙勬礀閵堝憡淇婇悜钘壩ㄧ憸宀€妲愬鍫熲拻濞达絿鎳撻婊勭箾閸欏澧电€规洘鍔欏畷顐﹀Ψ瑜岀粭澶愭⒑鐟欏嫬鍔跺┑顔哄€濋幃鈥斥槈閵忥紕鍘遍梺瑙勫閺呮稒淇婇悡骞熺懓顭ㄩ崨顓濇勃婵烇絽娲ら敃顏勭暦閿濆棗绶為悗锝庝簻閺€顓熺節濞堝灝鏋涢柨鏇閸掓帡顢涢悙鑼唵闂佸憡绋戦悺銊╁磻閿熺姵鈷戞い鎾卞妿閻ｈ鲸绻涢崼鐔虹畼缂佽鲸鎹囧畷鎺戔枎閹搭厽袦婵＄偑鍊栭崹闈浳涘┑鍡欐殾鐟滅増甯掗柨銈嗕繆閵堝倸浜剧紓浣哄Х婵炩偓闁哄矉缍佸顕€宕掑Δ浣哥彵闂備浇妗ㄧ粈浣虹矓瑜版帒钃熼柨婵嗩槸缁犳稒銇勯幘璺烘灁缂佸娲︾换娑氣偓鐢殿焾瀛濋梺闈涚墢椤牓锝炶箛鎾佹椽顢旈崪浣诡棃婵犵數鍋為悾顏堝磹閺嶎厽鍎庢い鏍ㄦ皑閺嗭箓鏌ｉ弮鍌楁嫛闁轰礁鍟撮弻銊モ攽閸℃﹩妫ら悷婊呭濡炶棄顫忔繝姘＜婵﹩鍏橀崑鎾搭槹鎼达絿鐒兼繛鎾村焹閸嬫挻顨ラ悙宸█闁糕晛瀚板畷姗€顢旈崱妯烘疄濠电姴鐥夐弶搴撳亾閺囥垹绠犻幖娣妼绾惧ジ鏌曟繛鍨壔闁哄啫鐗婇弲鏌ユ煕濞戝崬骞楁繛鍫幖閳规垿顢欓悙顒€娈岄悗娈垮枛閻栧ジ鐛崼銉ノ╅柕澶婃捣閸犳牕鐣疯ぐ鎺濇晩闁诡垎鍐窗闂傚倸鍊烽懗鍓佸垝椤栫偛绠伴悹鍥梿濞差亝鍋勯柣鎾虫唉閹芥洟姊虹紒妯荤叆闁告艾顑夊畷鎰磼濡寮垮┑掳鍊愰崑鎾翠繆椤愶絿鎳囬柟顕€绠栭幃婊堟嚍閵夈垺瀚奸梺鍝勵槺閸嬬偞绔熼崱娆戠幓闁哄倹锛曢悷閭︾叆闁告劦浜滈悡鐔兼倵濞堝灝鏋涙い顓犲厴瀵偄顓兼径濠勵槹濡炪倕绻愰幊搴㈠垔娴煎瓨鈷掑ù锝呮啞閹牓鏌涙繝鍌滃煟鐎规洘鍔曢埥澶愬閻樼洅鏇烆渻閵堝棗濮ч梻鍕閸╂盯骞嬮敂鐣屽幈闂佹寧妫侀褍顕ｆィ鍐╃厱闁哄啠鍋撶紒顔兼捣濡叉劙骞掑Δ鍐ㄥ絼濡炪倖鎸炬慨鐑芥偂閹剧粯鈷戦柣鎰閸旂數绱掗悩铏鞍濞ｅ洤锕獮鏍ㄦ媴閸濄儱骞愬┑鐐舵彧缁茶偐鎷冮敃鈧嵄濞寸厧鐡ㄩ埛鎺戙€掑锝呬壕濠电偘鍖犻崱妤侇仩闂備浇顕х换鎺楀磻閻愮鍋撳顒佸攭妞わ箒顫夐幈銊︾節閸屻倗鍚嬮悗瑙勬礃鐢帡鍩㈡惔銊ョ闁瑰瓨绻傞懙鎰磽閸屾艾鈧娆㈤敓鐘查棷妞ゆ洍鍋撶€规洖缍婂畷濂稿即閻愬吀绱滈梻浣瑰濞叉牠宕愰幖浣圭厑闁搞儺鍓氶悡蹇擃熆鐠鸿櫣澧曢柛鏃€绮嶆穱濠傤啅椤旂厧鈷嬪┑顔硷攻濡炶棄鐣烽锕€绀嬫い鎾寸⊕閻庨亶姊绘担鍛婂暈闁挎岸鏌曢崼鐔稿€愭鐐插暣閹瑩宕崟顐も偓顓烆渻閵堝棗濮夊┑顔肩－閼鸿鲸绻濆顓涙嫼闂佽崵鍠愭竟鍡涘箺閻樼數纾奸悹鍥ㄥ絻閺嗭絿鈧娲橀崹鎸庝繆閻戣姤鏅滈悹鎭掑妿閻ｉ箖姊绘担鍛婂暈闁圭妫濆畷姗€濡搁妷褜鍚呴梻?
void RunWarpInstall() {
    Balloon(g_str.get(L"tip", L"sniroute"),
            g_str.get(L"warp_installing", L"Installing Cloudflare WARP (winget or official package)..."),
            NIIF_INFO);
    Log(L"[warp] winget install start");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣濞嗗繒浠鹃梺绋款儍閸婃繈寮婚弴鐔虹闁绘劦鍓氶悵鏃堟⒑閸涘﹨澹樼紓宥咃躬閻涱噣寮介‖銉ラ叄椤㈡鍩€椤掍椒绻嗛柤娴嬫櫇濡垶鏌熺憴鍕Е婵炲吋鍔曡彁闁搞儜宥堝惈婵犵鈧磭鍩ｇ€规洏鍔戦、姗€鎮㈤崜鎻掓暢闂?CLI闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崗鐓庘偓缁樻叏濡も偓濡瑩鎮鹃悜鑺モ拺闁硅偐鍋涢埀顒佹礋閹崇喖顢涘┑鍫滅胺闂傚倷鐒︾€笛呮崲閸岀偛绠犻幖娣妼濮规煡鏌ｉ幇顔煎妺闁绘挻娲熼幃妤呮晬閸楃偛顏柣婵囧哺濮婃椽宕崟顓犲姽缂備焦褰冮埀顒傚枔娴滀粙姊绘担鍛婂暈缂佽鍊婚埀顒佸嚬閸樿壈顣鹃梺褰掑亰閸擄箑銆掓繝姘厪闁割偅绻冮ˉ鐐电磼閳ь剚寰勬繝搴㈠瘜闁诲孩绋掕摫闁稿﹥鍔栭〃銉╂倷閼碱剙鈪垫繝纰樺墲閹倹淇婇悜绛嬫晩闁芥ê顦辩槐锕傛⒒閸屾瑨鍏岀痪顓炵埣瀹曟粌鈹戠€ｃ劉鍋撻崘顔煎窛閺夊牆澧界粙蹇涙⒑閸愬弶鎯堥柟鍐茬箻閹繝鎮㈤崗鑲╁帾闂婎偄娲ら敃銉モ枍閸涘瓨鐓熼煫鍥ㄦ⒒濞叉挳鏌＄仦鍓р姇闁诡垱妫冮弫鎰板幢濡崵妲楀┑?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф闂佸憡鎸嗛崨顔筋啅闂備浇顕х€涒晠鎳濇ィ鍏洦瀵奸弶鎳筹箓鏌涢弴銊ョ仩缂佺姵濞婇弻鐔衡偓娑欘焽缁犮儱霉閻樿崵鐣烘慨濠呮缁瑩宕稿Δ濞惧亾濡や降浜滈柨鏃傚亾閺嗩剛鈧鍠涢褔鍩ユ径濞㈢喖鏌ㄧ€ｅ灚缍屽┑鐘垫暩閸嬫稑螞濞嗘挸绀夋俊銈呭暟閻瑩鏌涢妷顔煎闁?winget 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰绠绘鐐村灴婵偓闁靛牆鎳愰悿鈧俊鐐€栧Λ浣肝涢崟顒佸劅濠电姴娲﹂埛鎴犳喐閻楀牆绗掑ù婊€鍗抽弻娑⑩€﹂幋婵嗩潽缂備礁鍊哥粔褰掑箖閻戣姤鐒介柨鏃€鍎抽弫鎼佹⒑閼姐倕鈻堢紓鍌涜壘閳诲秹鏁愭径瀣偓鍫曞级閸碍娅囩痪鎯с偢閺岋絽螣閹稿海褰у銈冨劚閻楁捇寮婚敍鍕勃闁告挆鈧Σ鍫ユ⒑鐎圭姵顥夋い锕傛涧閻ｇ兘鏁撻悩鍐测偓鐑芥倵閻㈠憡娅滈柟鑼亾缁?Cloudflare 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰效闁挎繄鍋涢埞鎴犫偓锝庡亝濞呭洭姊虹粙璺ㄧ効濠碘€虫川缁瑨绠涢弮鍌滅槇闂侀潧楠忕徊鍓ф兜妤ｅ啯鍊垫繛鎴炲笚濞呭﹥顨ラ悙璇у伐闁伙絾绻堝畷鐔碱敄閼恒儱顏烘繝鐢靛仩閹活亞寰婃禒瀣疅闁跨喓濮寸粻姘舵煕椤愮姴鍔滈柣鎾跺枑閵囧嫰骞掗崱妞惧闂備線娼ч悧蹇涒€﹂崼銉﹀仼闂佸灝顑呴閬嶆煛婢跺鐏╂い鏃€甯￠弻锝夋偐閸欏鈹涢柣蹇撶箲閻熲晛鐣峰┑瀣櫇闁逞屽墴閳ユ棃宕橀鍢壯囨煕閳╁喚娈橀柣鐔村姂濮婅櫣绮欓崠鈥充紣缂傚倸绉撮敃銈夋偩閻戣姤鍋ㄧ紒瀣硶閸旓箑顪冮妶鍡楀潑闁稿鎸婚妵鍕敂閸曨偅娈绘繝纰樺墲閸庤偐绮诲☉妯锋婵炲棙菤閸嬫挸顓奸崱娆戠槇闂佹眹鍨藉褎绂掑鍫熺叆闁哄洦锚婵″ジ鎽堕悙鐑樼厪闊洢鍎崇壕璺ㄧ磼閳锯偓閸嬫捇姊绘担鍦菇闁搞劏妫勯…鍥槻闁烩槅鍘界换婵嬫偨闂堟稐绮堕梺娲讳簷閸楀啿鐣烽幇顑╂棃宕橀妸銉т喊闂備礁澹婇崑鍛崲閸曨垰姹查柛鈩冪⊕閻撴洟鏌￠崶銉ュ濠⒀傚嵆閺屾稑螣閸濆嫯鍩為梺瀹狀潐閸ㄥ潡骞冨▎鎾崇骇闁瑰濮冲ǎ顕€鏌ｆ惔銈庢綈婵炶绠撳畷婊冾潩鏉堚晝鐒块悗骞垮劚椤︻垳绮诲☉銏＄厽婵°倐鍋撻柣妤€绻樺畷?
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍焦鍎撻梺鎼炲妼閸婂潡寮诲☉銏╂晝闁挎繂妫涢ˇ銉х磽娴ｅ搫小闁告濞婂璇测槈濡攱鏂€闂佸憡娲﹂崑鍕叏閵忋倖鈷戞繛鑼额嚙楠炴鏌熼悷鐗堟悙妞ゎ偄绻戠换婵嗩潩椤掑嫬鏁归梻浣虹帛閺屻劑宕查弻銉у祦闊洦绋掗埛鎴︽煙缁嬫寧鎹ｇ紒鐘虫尰缁绘稓浠﹂崒姘潎闂佽鍨伴懟顖炲煝鎼淬劌绠婚悗鐢殿焾楠炲牓姊绘担铏瑰笡闁哄被鍔戝畷鏌ュ蓟閵夈儲杈堥梺缁樺姉閸庛倝宕戦妸锔轰簻闁哄秲鍔庨惌濠囨煛鐎ｎ偅鈷掔紒杈ㄥ浮椤㈡瑥鈻庨幆褎顔勯梻浣哥枃濡嫰藝閸偅鍙忛柍褜鍓熼弻锝呂熷ú璇叉櫛闂佸摜鍠庨澶婎潖缂佹ɑ濯寸紒娑橆儐缂嶅牓姊虹粙鎸庢崳闁告鍡欎罕闂備礁鎲″ú锕傚窗閹惧顩?Windows 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簻椤掋垺銇勯幇顏嗙煓闁哄被鍔戦幃銏ゅ传閸曟垯鍨荤槐鎺楀箛椤撗勭暦缂備胶绮惄顖炵嵁鐎ｎ喗鍊烽柛顭戝亜濞呮姊绘担鍛婂暈闁荤喆鍎抽幑銏狀潨閳ь剙顕ｆ繝姘嵆闁靛繒濞€閸炶泛鈹戦悩缁樻锭婵炲眰鍊濋幃鐢稿籍閸啿鎷?winget闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢悰鈥充壕婵炴垶鐟悞鐣岀磼閻樺崬宓嗛柡灞诲€濋幃褔宕煎┑鎰疂闂備線娼уú銈団偓姘卞閹便劑鍩€椤掑嫭鐓忛煫鍥ь儏閻忣亝銇勬惔陇鍩楅梻鍌氬€搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亜顒㈡い鎰Г閹便劌顫滈崱妤€鈷掗梺缁樺笧閺咁偊骞夌粙娆惧悑闁割偒婢€缂冩洟姊婚崒娆戭槮闁圭⒈鍋婂畷顖烆敃閿旇棄浜辨繝鐢靛Т濞层倝宕ヨぐ鎺撶厱闁哄洢鍔岄悘锟犳煟閹惧瓨绀嬮柡宀€鍠栭獮鍡氼槻缂佺嫏鍛＜濞达綀顫夌欢鏌ユ煃鐟欏嫬鐏存い銏＄懇閹稿﹥寰勯崱妯间簽闂傚倷娴囬鏍窗閺嶎偆绀婂┑鐘叉川瀹撲礁銆掑锝呬壕濡ょ姷鍋炵敮锟犵嵁鐎ｎ亖鏀介柟閭﹀枓閸嬫挸鐣￠幍铏杸闂佺粯鍔栧娆撴倶閿曞倹鐓欑紒瀣閹插摜绱掗鑺ヮ梿闁靛洦鍔欓獮鎺楀箣閿濆棙鍟洪梺璇查缁犲秹宕曢柆宥嗗亱婵犲﹤鍠氶悗鍫曟煏婵炵偓娅嗛柍閿嬪灴閺屾稑鈽夊鍫熸暰闁诲繐绻掗弫濠氬蓟濞戙垺鍋愮€规洖娲ら埛宀勬⒑缂佹ü绶遍柛锝忕到閻ｅ嘲顫滈埀顒勩€佸Ο娆炬Ъ闂佸疇顕ч惌鍌炲蓟閿濆棙鍎熼柕鍫濆缂嶅牆鈹戦悙鎻掔骇缂侇喗鎸搁悾鐑藉閿濆孩鈻岄梻浣虹《閺呮繈宕戦妶澶屽祦閻庯綆鍠楅弲婵嬫煃瑜滈崜鐔煎蓟鐎ｎ喖鐐婇柕濞у懐妲囬梻鍌氬€搁悧濠勭矙閹惧瓨娅犻柡鍥╁櫏濞堜粙鏌ｉ幇銊︽珔濞存粓绠栧濠氬磼濮橆兘鍋撻幖浣哥９鐎瑰嫭鍣磋ぐ鎺戠倞鐟滃繘寮抽敃鍌涚厽闁靛繈鍩勯悞鍓х磼閹邦収娈滈柡宀€鍠栭獮宥夘敊绾拌鲸姣夐梻浣侯焾椤戞垹鎹㈠┑瀣摕闁哄洢鍨归柋鍥ㄧ節闂堟稒鎼愭い锔规櫅椤啴濡堕崱妤€顫堢紓渚囧枟閻熲晛顕ｆ繝姘嵆闁靛繒濞€閸炶泛鈹戦悩缁樻锭婵炲眰鍊濊棟闁宠桨绶″〒濠氭煏閸繈顎楀ù婊勭箖缁绘盯宕ｆ径娑溾偓鎸庮殽閻愯尙澧﹀┑鈩冩倐婵￠攱鎯旈敐鍛亪濡ょ姷鍋為崝鏍箚閺冨牊鏅查柛鎰剁到娴滈箖鏌熼悧鍫熺凡缂?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珖闂侀€炲苯澧板瑙勬礃缁绘繂顫濋鐘插箞闂備焦鏋奸弲娑㈠疮娴兼潙鐓″鑸靛姈閻撴洟鎮橀悙棰濆殭濠殿喖鍊块弻锛勪沪鐠囨祴鍋撻弴銏＄畳闂佽鍑界紞鍡涘磻閸涘瓨鍋熼柡鍥╁剱濞撳鏌曢崼婵嬵€楀ù婊勭箖缁绘盯鎳犻鈧弸娑氣偓娈垮枛椤兘骞冮姀銈呭窛濠电姴瀚倴闂傚倷绀侀幉锟犲箰閸℃稑宸濇い鏃傜摂閸熷懘姊婚崒娆愮グ鐎规洜鏁诲畷浼村箛閻楀牆浠柡澶婄墐閺呮繃绋夊澶嬬厵闁硅鍔曢崥鍦偓瑙勬尫缁舵岸寮婚垾鎰佸悑閹艰揪绲肩划鎾绘煙閸忓吋鍎楅柣鎾崇墦瀵偊宕卞☉娆戝帗閻熸粍绮撳畷婊堟偄閻撳海鐣哄┑顔姐仜閸嬫捇鏌涢埡瀣瘈鐎规洏鍔戦、姗€鎮╅崹顐ｇ槥闂傚倸鍊烽懗鍓佸垝椤栫偛钃熼柕濞炬櫅鐟欙箓鎮楅敐搴℃灈闁绘挻娲熼弻鐔告綇妤ｅ啯顎嶉梺缁樻尰濞叉鎹㈠┑瀣棃婵炴垶鐟Λ鐐差渻閵堝懏绂嬮柛瀣閳ワ箓鎳楅锝嗏枌濠电偛鐡ㄧ划宀勬偉閻撳寒鍤曢悹鍥ㄧゴ濡插牓鏌曡箛鏇炐ユい鏃€甯￠弻锝嗘償閵婏附閿梺纭咁嚋缁绘繂鐣风€圭媭妯勫┑?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珖闂侀€炲苯澧板瑙勬礋閹兘骞婃繝鍌滅Ш闁诡喒鏅涢蹇涱敊閻撳骸螚缂傚倸鍊搁崐鍝ョ矓閻㈠憡鍋嬮柡鍥ｆ噰閳ь剨绠撳畷绋课旀担鍛婄杺闂傚倸鍊搁悧濠勭矙閹达讣缍栭柛銉墯閳锋垿姊洪銈呬粶闁兼椿鍨跺畷銏ゅ传閵壯咃紲闂侀€炲苯澧紒妤冨枛閸┾偓妞ゆ帒瀚ㄩ埀顑跨椤粓鍩€椤掆偓閻ｅ嘲螖閸涱喖浜楅柟鑹版彧缁插潡鎮伴灏栨斀闁绘ê鐏氶弳鈺佲攽椤旂⒈鍤熼柍褜鍓氬銊╁礂濡櫣鏆︽い鎺戝椤ュ牊绻涢幋鐐茬瑲闁哄倵鍋撻梻鍌欒兌缁垶鏁冮埡鍛獥闁哄稁鍘介崐鍫曞级閸稑濡跨紒鐘冲劤椤法鎹勬笟顖氬壋濠电偛寮堕幐鎶藉蓟閳╁啯濯寸紒瀣濞堢粯绻濈喊澶岀？闁哄苯顦佃棟闁圭虎鍠楅悡鐔搞亜閹捐泛浠︽い銉ｅ灲閺岋紕浠﹂悾灞濄儲銇勮缁舵岸寮诲☉銏犵閻犺櫣鍎ら悗璇差渻閵堝骸浜滅紒缁樺姉閸欏懎顪冮妶鍛閻庢凹浜炲Σ鎰板醇閺囩啿鎷洪梺鍛婄☉閿曘儳绮堥埀顒勬⒑鐠囪尙绠插ǎ鍥ㄦそ瀹曟劙骞橀瑙ｆ嫽闂佺鏈悷銊╁礂鐏炰勘浜滄い鎾跺仧婢с垽鏌嶈閸撴瑩鎮樺杈ㄦ殰闁炽儱纾弳锔炬喐閻楀牆绗掗梺鍗炴喘閺岋繝宕堕埡浣锋勃濠碘€虫▕閸撶喎顫?
//
// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍焦鍎撻梺鎼炲妼閸婂潡寮诲☉銏╂晝闁挎繂妫涢ˇ銉х磽娴ｅ搫校鐟滄澘鍟村﹢渚€姊洪幐搴ｇ畵闁瑰啿閰ｈ棢闊洦姊荤粻楣冩倵濞戞瑯鐒藉褏鏁婚弻锛勪沪閻愵剛顦ㄧ紓浣虹帛缁嬫牠藝閺屻儲鐓曢柣鏇氱娴滀即鏌熼缂存垹鎹㈠┑瀣倞闁靛鍨虹€氬ジ姊绘担鍛婂暈闁瑰憡妲掗妵鎰板礃閳哄喚娲搁梺缁樺姉閸庛倝鎮￠弴銏＄厽婵☆垱瀵ч悵顏嗏偓瑙勬礀閻倿寮诲☉銏犵厴闁割煈鍠氭导鍫ユ倵濞堝灝鏋熼柟姝屾珪閹便劑鍩€椤掑嫭鐓ユ繛鎴灻鈺傤殽閻愯尙澧涘ǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘肩粈澶嬩繆閵堝嫮鍔嶉柛娆忕箻閺岀喓绱掗姀鐘崇亪缂備讲鍋撳璺侯儑缁♀偓婵犵數濮撮崐鎼佺嵁濡吋鍠愰柣妤€鐗嗙粭姘舵煕鐎ｎ偄濮夌紒杈ㄥ笒閳藉顫濋崗鍏兼殲闂備線鈧偛鑻晶顔界箾绾绡€鐎殿喖顭烽幃銏㈠枈鏉堛劍娅撻梻浣筋嚃閸ㄥ酣宕橀妸褜鍚呴梻浣筋嚙鐎涒晠顢欓弽顓炵獥婵°倐鍋撴い顓炴搐閳诲酣骞欓崟顓犫棨婵犵數濞€濞佳兠归崒姘肩劷婵炲棙鎸婚悡娑氣偓骞垮劚閸燁偅绔熸径瀣瘈濠电姴鍊绘晶娑㈡煃闁垮绗掗棁澶愭煥濠靛棭妯堟俊顐ｅ灥閳藉骞橀悷鎵缂備浇椴哥敮鈥崇暦閿濆棗绶炲┑鐘查濞堟繄绱撻崒娆掑厡缂侇噮鍨堕幆鍕敍閻愬弶妲梺閫炲苯澧柕鍥у楠炴帡骞嬪┑鎰棯闂備胶绮幐鑽ょ矙閹达附绠掗梻浣瑰缁诲倿骞婅箛娑樼疅闁告縿鍎崑鎾斥槈濞嗘垹鐣靛銈庝簻閸熷瓨淇婇懜鍨劅闁炽儴灏欓幐澶愭⒒娴ｈ鍋犻柛鏂款儑閸掓帒鈻庨幘鏉戜粧濡炪倖娲嶉崑鎾绘煕閳规儳浜炬俊鐐€栧濠氬磻閹剧粯鐓冮悷娆忓閻忥妇鈧灚婢樼€氼噣鍩€椤掑﹦绉甸柛瀣笒椤?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鐘栄囨煕鐏炲墽鐓瑙勬礀閳规垿顢欓惌顐簻閻ｇ兘顢楅崟顐㈠亶闁诲海鏁哥涵鍫曞磻閹捐埖鍠嗛柛鏇ㄥ墰椤︺儳绱撻崒姘毙㈤柨鏇ㄤ簻椤曪絾绻濆顒€鑰垮┑掳鍊曢敃銈夊箖閹达附鈷戦柛娑橈梗缁堕亶鏌涢悩宕囥€掓繛鍡愬灲婵偓闁靛牆妫岄幏缁樼箾鏉堝墽鎮奸柟铏崌椤㈡艾顭ㄩ崨顖滐紲闁荤姴娲﹁ぐ鍐焵椤掑啫鍔垫俊顐ゅ枎椤啴濡堕崱娆忣潷婵犮垼顫夊ú鐔风暦閹达箑绠婚柟棰佺劍閸嶉潧顪冮妶鍡楃瑨妞わ妇澧楅弲銉х磽閸屾艾鈧兘鎳楅崜浣稿灊妞ゆ牗绮庨惌娆撴煙鏉堟儳鐦滈柡浣革躬閻擃偊宕堕妸銈囩箒闂佽桨绀侀敃顏堝蓟閵堝棙鍙忛柟閭﹀厴閸嬫捇寮撮姀鐘叉優濡炪倖甯掔€氼參鍩涢幒鎳ㄥ綊鏁愭径妯活棖婵炴垶鎹佺亸娆戞閹烘鍋愰柛鎰皺娴煎矂姊虹拠鈥虫殭闁搞儜鍜佸晪闂備礁缍婇。锕傛倿閿斿墽绀婇幖绮规閺€浠嬫煟閹邦剙绾ч柍缁樻礀闇夋繝濠傚暟閸╋絿鈧鍣崑濠傜暦閸楃儐娓婚柡鍥╁仧缁夋椽鏌℃担鐟板鐎规洖銈搁、鏇㈡晲閸モ敩銉╂⒒閸屾瑨鍏屾い顓炵墢閳ь剚绋堥弲鐘荤嵁閹版澘绀堢憸搴綖閺囥垺鐓欓柟顖嗗懏鎲兼繝娈垮灡濞兼瑩婀侀梺缁樏Ο濠囧磿韫囨稒鐓欓柣锝呮湰閺嗏晝绱掓潏銊﹀鞍闁瑰嘲鎳愮划鐢碘偓锝庝簼椤斿嫰姊虹拠鎻掝劉闁活収鍣ｅ畷锟犲礃閼碱剚娈惧┑鐐叉▕娴滄粓鎮為崹顐犱簻闁瑰搫绉瑰宄懊瑰鍕煉闁哄瞼鍠栭幃婊兾熼搹閫涙樊闂備線鈧偛鑻晶顔剧磽瀹ュ拑宸ラ柣锝囧厴閹晫绮欑捄銊ュЕ婵＄偑鍊栫敮鎺斺偓姘煎弮閸╂盯骞嬮敂鐣屽幈濠电娀娼х€氼剟宕濆顓犵闁告侗鍘剧粻濠氭煛鐏炲墽娲村┑鈩冩倐閺佸啴鍩€椤掑嫬鐓濋柡鍥╁Л閸嬫挸鈻撻崹顔界彯闂佺顑呴敃顏堝Υ娴ｅ壊娼╅悹楦挎閸旓箑顪冮妶鍡楃瑨閻庢凹鍓熷浼村Ψ閳哄倻鍘撻悷婊勭矒瀹曟粌顫濈捄浣曪箓鏌涢弴銊ョ仩缂佺姵濞婇弻鐔衡偓娑欘焽婵″洭鏌涚€ｎ偅宕岄柟绛圭節婵″爼宕担璇℃％婵犵數濞€濞佳囧磹閹间礁绠熼柨鐔哄У閸嬪倿鏌ｉ弬鍨倯闁绘挻鐟ラ湁闁绘挸娴烽幗鐘崇箾閹冲嘲鍠氶悢鍡欐喐濠婂牆绐楁俊銈呮噹妗呴梺鍛婃处閸ㄤ即宕￠幎鑺ョ厽闁哄啫娲﹂鐘绘煟閹捐尙鍔嶇紒缁樼洴閺佹劙宕ㄩ鑺ュ€烽梻浣瑰濞诧附绂嶉鍫㈠祦闁告劑鍔夐弸搴ㄦ煙閹咃紞妞ゆ柨鐭傚娲偡閻楀牊鍎撳┑鈽嗗亝椤ㄥ棛绮嬪鍡愬亝闁告劏鏅濋崢閬嶆偡濠婂啴鍙勯柕鍡楀暣瀹曘劑顢涘鈧粭澶愭⒑閸濆嫮鈻夐柣蹇斿姌閵囨劙骞掗幘鍏呮睏闁诲海鎳撴竟濠囧窗閺嶎厼绀堝ù鐓庣摠閻撴瑦銇勯弽銊х煀闁哄绋掗幈銊︾節閸愨斂浠㈠Δ鐘靛仦閸旀牠骞嗛弮鍫濐潊妞ゎ偒鍠氱粚鍧楁⒒閸屾瑨鍏屾い顓炵墦椤㈡牠宕卞☉妯碱唹闂佹悶鍎洪崜娆撴嫅閻斿吋鐓ユ繝闈涙－濡插摜绱掗悩宸吋闁诡喗顨呴埥澶娾枎濡厧濮烘俊鐐€х€靛矂宕ｉ崘顭戞綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顧侀柛锝忕秮瀵鏁愰崼銏㈡澑闂佸搫鍟崑鍡涘汲閿涘嫮纾藉ù锝堫潐閳锋帡鏌涢悩铏磳闁糕斁鍋撳銈嗗笒閿曪妇绮旈悽鍛婄厱闁绘ê纾晶鍗烆熆鐟欏嫭绀冪紒缁樼箓椤繈顢橀悩鎻掔闂傚倷娴囬鏍垂鎼淬劌宸濇い鎺戝娴犳劙姊婚崒娆掑厡妞ゎ厼鐗撻、鏍幢濞戞顔夐梺鎼炲劀閸涱喗顔曢柣鐔哥矌婢ф鏁埡鍛瀬闁告劦鍠楅悡蹇涚叓閸パ嶆敾婵炲懎妫欓妵鍕Ψ閿旇棄纾冲┑顔硷功缁垶骞忛崨顔剧懝妞ゆ牗绮屾慨鍏间繆閻愵亜鈧牠宕归棃娴虫稑鈹戠€ｎ剙绁﹂梺鍦濠㈡绮婚悷鎳婂綊鏁愰崨顔跨闂佷紮绲炬繛濠傤潖婵犳艾纾兼繛鍡樺笒閸橈紕绱撴笟鍥ф珮闁搞劌鐖煎顐﹀礃椤旇偐鍘搁梺鍛婁緱閸橀箖鎮伴妷鈺傗拺闁告繂瀚埢澶愭煕濡灝浜圭紒顔肩墛瀵板嫮鈧綆鍓涢鏇㈡⒑缁洖澧查拑閬嶆倶韫囧骸宓嗛柡?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顕€姊哄Ч鍥р偓銈夊窗濡ゅ懎桅闁告洦鍨伴崘鈧銈嗗姦濠⑩偓缂侇喖鐖煎娲箹閻愭彃顬堥梺绋匡工濞尖€愁嚕鐠囨祴妲堥柕蹇婃櫆閺呮繈姊洪幐搴ｇ畵婵☆偅鐩棟闁靛鏅滈埛鎴︽煢濡警妲搁柡鍡欏枛閺屾盯鎮╁畷鍥р拰閻庢鍠涢褔鍩ユ径鎰潊闁绘ɑ鐗撻崝鎴﹀蓟閵娿儮鏀介柛鈩兠▍锝咁渻閵堝骸澧い鎴濇嚇閳?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф婵°倧绲介崯顖烆敁瀹ュ鈷戠紒瀣仢椤掋垺銇勯妸銉﹀殗濠碘剝鎸抽獮鎺楀箣椤撶喎鍏婇梻浣瑰缁诲倿骞婂鍡欑彾闁哄洢鍨洪埛鎺懨归敐鍫燁仩闁靛棗锕弻锟犲磼濞嗘垹鐛㈤悗瑙勬礃閸ㄥ潡鐛鈧獮鍥ㄦ媴閻熸壆妲ｉ梻鍌欑窔濞佳囨偋閸℃あ娑樜旈崨顓㈡暅婵犵數濮村ú锕傛偂閺囥垺鍊甸柨婵嗛娴滄繄鈧娲栭惌鍌炲蓟閿涘嫪娌悹鍥ㄧゴ閸嬫捇寮撮悩鎻掔柧闂備浇宕垫慨鏉懨洪埡鍐х剨婵炲棙鍔楅々鐑芥煙闂傚鍔嶉柍閿嬪灴閹綊宕堕敐鍌氫壕闁惧浚鍋嗘禍顏呯節閻㈤潧浠滈柣銊у厴閸┾偓妞ゆ巻鍋撶痪缁㈠弮瀵娊鏁冮崒娑氬幗闂佸搫鍟崐鐢稿箯閿熺姵鐓涘ù锝呮憸鏍＄紓浣虹帛閻╊垶鐛€ｎ喗鍊烽柤纰卞墰閹虫牠姊虹拠鏌ヮ€楅柣蹇斿哺閹囨偐瀹割喗缍庣紓鍌欑劍钃卞┑顖涙尦閺屾稑鈽夊鍫濅紣闂佸搫妫楅悧濠勬崲濞戙垹绠婚悗闈涘閺嗏€愁渻閵堝啫濡奸柨鏇樺€濋幃鎯х暋闁妇鍙嗛梺鍛婂姧缁查箖锝炲澶嬧拺闂傚牊绋撶粻姘舵煃瑜滈崜娆撳储閺嶎厽鍋ㄥ┑鐘崇閳锋帒霉閿濆洨鎽傛繛鍏煎姇椤潡鎮烽悧鍫！闂佸搫鎳撳▔娑滅亙闂佸憡渚楅崰鏍ㄧ濡も偓閳规垿鎮欓弶鎴犱户闂佹悶鍔嶅浠嬪箖閿熺姴鍗抽柣鏃傜節缁ㄥ姊洪柅娑樺祮婵炰匠鍕洸婵犲﹤鍘鹃悷閭︾叆闁告洦鍘鹃悡鈧俊鐐€ら崣鈧柛搴☆煼绡撳〒姘ｅ亾闁哄本鐩幃鈺呭箛娴ｅ湱鏆梻浣烘嚀缁犲秹宕硅ぐ鎺戠厴闁瑰濮崑鎾绘晲鎼存繄鏁栭梺?
// DoH/nginx 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煕閹邦垰绱﹂柣銏㈢帛娣囧﹪鎮欓鍕ㄥ亾閺嶎厽鍋嬫繝濠傜墕绾剧粯绻涢幋娆忕仼闁搞劌鍊块弻娑樼暆閳ь剟宕戦悙鐑樺亗闁绘柨鍚嬮悡蹇涚叓閸パ屽剰闁绘帗妞介弻锝夊箳閹存績鎸冪紓浣介哺鐢繝宕洪埀顒併亜閹烘垵顏╃痪鎯у悑缁绘盯骞嬮悙鍨櫑閻庢鍠栭悘婵嬪煘閹达附鏅柛鏇ㄥ亗閸濇顪冮妶鍡樿偁闁告洦鍙庡ú鍛婁繆閵堝繒鍒伴柛鐕佸灦瀹曟劙鎮介崨濠備画濠电偛妫楃换鎰邦敂椤忓棛纾奸柍褜鍓熼崺鈧い鎺戝閳锋垹绱撴担璇＄劷濠⒀屼邯閺屾洟宕奸悢绋库偓鎰版煟濞戝崬娅嶇€规洘锕㈤、娆撴嚃閳轰礁绠伴梻鍌欒兌椤牓寮甸鍌滅煓闁规儳澧庨惌鎾绘偣鏉炴媽顒熸繛鎾愁煼閺屾洟宕煎┑鍥舵婵犳鍠栭崐鍧楀蓟濞戞瑧绠鹃柛顭戝枓閸嬫捇寮撮悙鑼偒闂佽崵鍠愮划搴㈡櫠濡ゅ啯鏆滄俊銈呮噹閻撴洟鏌熼悜妯烘鐟滅増甯楅弲鏌ユ煕濞戝崬鏋涢柡鍡楃墦濮婃椽宕妷銉︾亖闂佺顑囬崑銈嗕繆閻㈢绀嬫い鏍嚤閳哄懏鐓忓鑸电〒閻ｈ鲸绻涢崪鍐М闁哄矉缍侀獮姗€宕￠悙鈺佷壕婵犻潧顑呴拑鐔兼煛閸モ晛鏋旂紒鈾€鍋撻梻濠庡亜濞诧箓骞愰幆顬綁鏁愰崨顏呮杸闂佺粯鍔樼亸娆愭櫠閺冨倻纾兼い鏃傛櫕閹冲洨鈧鍠涢褔鍩ユ径鎰潊闁绘ɑ鐗撻崝宥夊Φ閸曨垰绫嶉柛灞捐壘娴犳ê顪冮妶搴′簮闁哄懐濮撮锝夊醇閺囩偟鍘搁梺绋挎湰缁酣鎮鹃搹鍦＝濞达綀娅ｇ敮娑氱磼鐎ｎ偄绗掓い顓炴穿缁犳稑鈽夊Ο纰辨Ф闁荤喐绮岀粔鐟扮暦閸濆嫮鏆嗛柛鏇ㄥ厴閹疯櫣绱掔紒銏犲箹闁瑰啿姘﹂。璺ㄧ磽閸屾瑧璐伴柛鐘愁殜楠炴垿宕惰閺嗭附绻涘顔荤凹闁稿﹪鏀辩换娑㈠级閹搭厼鍓卞銈庡亜缁夋挳鍩為幋锔藉€烽柛娆忣樈濡繝姊洪幖鐐插缂傚秴锕獮鍡樼瑹閳ь剙鐣锋總绋课ㄩ柨鏇楀亾濞寸媭鍘奸埞鎴︽偐鐠囇冧紣闂佸摜鍠撴晶妤呭疾閵夆晜鐓熼幖娣€ゅ鎰箾鐠囇呯暤鐎规洖缍婇幊婊堝礂閸濄儳鐣鹃梻浣哥秺濡法绮堟笟鈧幃鈥斥槈濡繐缍婇弫鎰板川椤旇棄鍓靛┑鐘愁問閸ｎ垶姊介崟顒傗攳濠电姴娲﹂崐閿嬨亜韫囨挸顏ら柛瀣崌瀵粙顢橀悙鑼偓顓㈡⒑缂佹ɑ灏紒銊ョ埣瀵悂寮介鐔哄幐闂佹悶鍎崕閬嶆倶閳哄啰纾奸柍褜鍓氬鍕沪缁嬪じ澹曞Δ鐘靛仜閻忔繈宕濆顓滀簻闁挎柨鐏濆畵鍡涙煙椤曞棛绡€鐎殿喗鎸虫慨鈧柨娑樺鐢儳鈹戦悩鍨毄濠殿喖顕幑銏ゅ箳濡も偓缁€鍡涙煛婢跺﹦姘ㄩ柡鈧禒瀣叆婵炴垶锚椤忣亝绻涢崨顔界缂佽鲸甯楀蹇涘Ω閵夛箒鐧侀柣搴ゎ潐濞叉鏁幒妤€鐓″璺好￠悢鍏煎殐闁冲搫鍊稿▓鎰版⒒閸屾艾鈧嘲霉閸ヮ剦鏁嬮柡宥庡幖閸ㄥ倿鏌涘Δ鍐ㄢ偓锝呪槈閵忕姷顦ㄩ梺鍐叉惈閸婄顕ｉ崸妤佲拺闂侇偆鍋涢懟顖涙櫠閹绢喗鐓涢悘鐐垫櫕鏁堟繝纰夌磿閸忔ɑ淇婇悿顖ｆ缂備浇浜崑鐐垫崲濠靛顫呴柨婵嗘閵嗘劙鏌ｆ惔銏㈩暡婵犮垺锕㈤敐鐐剁疀閺囩姷锛滃┑鈽嗗灥椤曆囶敁閹剧粯鈷戦柛娑橈功閳藉鏌ㄩ弴妯衡偓婵嗩嚕閹惰棄閱囬柕澶涜吂閹锋椽姊洪崨濠勨槈闁挎洏鍊濋幃姗€濡疯绾剧晫鈧箍鍎遍幏鎴濐啅閵夆晜鐓熼柨婵嗘处閻掑潡鏌曢崶銊ュ妤犵偞甯￠獮姗€宕楅崨顖滅Э闂傚倸鍊风粈浣圭珶婵犲洤纾婚柛鈩冪☉缁愭骞栫划瑙勵潑闁搞倖娲熼弻娑滎槼妞ゃ劌妫濋崺娑㈠箣閿旂晫鍘遍梺褰掑亰閸撴瑧鐥閺岀喖顢涘鍗炩叺闂佸搫鐬奸崰鏍х暦濮椻偓閹崇娀顢楅崒銈呮櫔闂佽姘﹂～澶娒哄鍫濇瀬濠电姵鑹鹃弰銉╂煃瑜滈崜姘跺Φ閸曨垰绠抽柟瀛樼箥娴犻箖姊虹粙鍖″姛闁轰礁顭峰璇测槈濠婂懐鏉搁梺鐟扮仢閸燁垳鐟ф繝鐢靛仜閻°劎鍒掗幘鍓佷笉闁哄诞灞剧稁濠电偛妯婃禍婵嬫倿婵犳碍鐓熼柟閭﹀灠閽勫ジ鎮楀顓熺凡妞ゎ叀鍎婚¨渚€鏌涢悩鎰佹疁濠碉紕鏁诲畷鐔碱敍濮橆収妫熼梺鑽ゅУ娴滀粙宕濇惔顭戞晩濠㈣泛艌閺€浠嬫煟閹扮増娑ч悽顖氬缁辨帞绱掑Ο鏇熷灦缁岃鲸绻濋崶褏顔愭繛杈剧秬閸婁粙濡歌閸犳劙鐓崶銊р姇闁稿蓱閵囧嫰寮捄銊︾檨闂佽　鍋撳ù鐘差儐閻撶喖鏌熼柇锕€澧伴柟鐣屽█閹绠涙惔鈥崇ギ闂佸搫琚崐婵嬪箖閻戣棄绾ч幖瀛樻尰鐎垫牠姊绘担绋挎倯婵＄偠濮ょ粋宥夊醇閺囩偠鎽曢梺绯曞墲椤ㄥ繘宕ョ€ｎ喗鐓曟繛鎴濆船閻忚尙绱掗柆宥勬喚婵﹦绮幏鍛驳鐎ｎ偆绉烽柣搴ゎ潐濞叉﹢鏁冮姀銈冣偓浣割潩閹颁焦鞋濠电姰鍨奸～澶娒洪悢椋庢殾婵°倕鎳忛崵鍐煃閸濆嫬鏆熸俊鎻掓喘濮婂宕掑顑藉亾閹间焦鍋嬪┑鐘插閻瑩鏌熼幆褏鎽犳繛鍛█閺屾洖顕ュ☉銏╂殥缂併劌顭峰娲传閸曨偅娈滈梺绋款儐閹瑰洭寮诲☉銏犵睄闁逞屽墴閹兘濡烽埡瀣◤闂婎偄娲﹀ú婊呭閸忛棿绻嗘い鏍ㄧ箥閸ゆ瑦銇勬惔銏″磳婵﹥妞介幊锟犲Χ閸涱喚鈧偓绻濋姀銏″殌婵☆偅绋撻崚鎺撶節閸ャ劌鈧鏌ら幁鎺戝姉闁归攱妞藉铏圭矙鐠恒劎顔囬梺鍛婅壘椤戝淇婇悽鍛婂亜闁稿繗鍋愰崢閬嶆⒑闂堟侗鐒鹃柛鏂款儔瀹曘儵顢曢妶鍥╋紲濡炪倖姊婚埛鍫ュ磿閺冣偓椤ㄣ儵鎮欓弶鎴炶癁濡ょ姷鍋涢澶婎嚕椤曗偓閸┾偓妞ゆ帒瀚哥紞鏍偓骞垮劚閹冲寮ㄦ禒瀣厽闁归偊鍓欑痪褎銇勯妷褍浠遍柡灞剧洴婵℃瓕顦查柡瀣〒閳ь剚顔栭崰妤呭箖閸屾凹鍤曞ù鐘差儛閺佸洭鏌ｉ弬鍨骇閻庢艾銈稿濠氬磼濮橆兘鍋撻幖浣瑰亱濠电姴瀚惌娆撴煙閹呮憼婵炲懐濞€閺岋繝宕堕妷銉т痪闂佺锕ら悘姘跺箞閵娿儙鐔兼偂鎼粹€茬帛闁诲孩顔栭崳顕€宕板Δ鍛﹂柛鏇ㄥ灠椤懘鏌嶉埡浣告殶妞わ缚鍗冲娲捶椤撗呭姼闂佹寧娲忛崕宕囧垝閳哄懏鍋勯悘蹇庣劍閺傗偓闂備礁澹婇崑鍡涘窗鎼粹檧鏋旈悘鐐电摂濞撳鏌曢崼婵嬵€楁鐐村灴閺屾盯寮埀顒勫垂閻㈠憡鍋╃€瑰嫰鍋婂銊╂煃瑜滈崜鐔兼偘椤曗偓瀹曞崬顫濋崗澶光晛鈹戦悩娈挎毌濠殿喖绉瑰畷銊╊敍濠婃劗闂?
//   1) 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋婵愭綗闁逞屽墮閸婂潡骞愭繝鍐彾闁冲搫顑囩粔顔锯偓瑙勬磸閸旀垵顕ｉ崼鏇炵闁绘瑥鎳愰獮銏ゆ⒒閸屾瑧绐旈柍褜鍓涢崑娑㈡嚐椤栨稒娅犻柟缁㈠枟閻撴盯鎮橀悙鎻掆挃闁靛棙甯￠弻宥堫檨闁告挶鍔庣槐鐐哄幢濞戞锛涢梺鍛婁緱閸橀箖寮抽敂閿亾閸忓浜鹃梺鍛婃处閸撴盯宕?5444(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煠绾板崬澧婚柛鐐茬埣濮婄粯鎷呴崨濠傛殘缂備礁鎼鈥崇暦閺囥垹绠柤鎭掑劚閸撳綊姊洪悷鏉库挃缂侇噮鍨堕幃锟犲即閻旂繝绨婚梺瑙勬緲婢у海绮欓懡銈囩＜闁逞屽墴瀹曟﹢顢欓悾灞藉箞? / 5454(DoH 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煕閹邦剙绾ч柣銈呭濮婄粯鎷呯粵瀣異闂佹悶鍔岄妶鎼佸箖濮椻偓楠炲洭顢涘Ο瑙勭潖闂備礁婀遍崕銈夊箰閹间礁鐒垫い鎺戝暙閸氬湱绱掓潏銊ョ瑨闁宠棄顦～婊堝幢濡も偓婢瑰姊? / 443(nginx) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鐘栄囨煕鐏炲墽鐓瑙勬礀閳规垿顢欓惌顐簻閻ｇ兘顢楅崟顐㈠亶闁诲海鏁哥涵鍫曞磻閹捐埖鍠嗛柛鏇ㄥ墰椤︻喖顪冮妶搴′簻妞わ箓浜堕崺鈧い鎺戝濞懷囨煏閸埄鐒炬い鏇秮瀹曞ジ寮撮悙娈垮悈闂備胶绮敋闁诲繑鑹捐閹兼番鍨荤弧鈧梺姹囧灲濞佳勭閿曞倹鐓欑紒瀣儥閻撹偐鈧娲樺钘夌暦濮椻偓椤㈡瑩鎳栭埡瀣耿闂傚倷鑳堕幊鎾存櫠閻ｅ苯鍨濇い鏍仦閸嬪倹绻涢幋娆忕仾闁稿﹤鐖奸弻锝夊箛椤掑倷绮靛┑鐐叉噺濞叉牠鈥﹂懗顖ｆ缂備胶濮甸幑鍥ь嚕婵犳碍鍋勯柧蹇撴贡閻撴捇姊洪崨濠勫ⅳ闁告挻绻堝顐ｃ偅閸愨晛鈧敻鏌涢…鎴濅簼缂佽埖鐓￠幃妤€顫濋悡搴㈢彎闂佺硶鏂侀崑鎾愁渻閵堝棗绗掗柨鏇缁棃鎮介崨濠勫幈濠碘槅鍨辨禍浠嬪磻閵忊槅娈介柣鎰级椤ャ垻鈧娲栭妶鍛婁繆閻戣姤鏅滈悷娆忓椤?
//   2) 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磼閻愵剙鍔ょ紓宥咃躬瀵鏁冮埀顒冪亽婵炴挻鍑归崹閬嶅煝閹炬枼鏀介柣鎰皺閹界娀鏌ｅΔ鍐ㄢ枅鐎殿喖顭峰鎾偄閾忚鍟庨梻浣虹帛閸旓箓宕滃鑸靛仧闁哄啫鍊荤壕钘壝归敐鍛儓閺嶏繝姊虹紒妯绘儓缂佺粯绻傞锝嗙節濮橆剙宓嗛梺鎸庣☉鐎氼噣寮堕幖浣圭厽閹兼惌鍨崇粔闈浢瑰鍕⒌鐎规洘鍨块獮妯兼嫚閹绘帒绁梻浣虹帛閿氶柛姘ｅ亾缂備焦顨呴ˇ顖炲煘閹达附鍊烽柡澶嬪灩娴犙囨⒑閹肩偛濡兼い顓炲槻閻ｇ兘鏁愭径濠勵啋缂傚倷鐒﹂…鍥储闁秵鈷戦梻鍫熶緱濡插綊鏌涢妸褎鏆╅柍褜鍓氶崙褰掑储婵傚憡鍋╅柣鎴ｆ缁€鍐偓鐟板閸犳鈧潧鐭傚娲濞戞艾顣洪梺纭呮珪閸旀鍒掓繝姘妞ゆ梻鎳撴禍楣冩偡濞嗗繐顏璺哄閺屾盯骞樼捄鐑樼亪閻庤娲樺鑺ヤ繆閻戣姤鏅濋柍褜鍓涘▎銏ゆ倷閻戞鍘甸梻渚囧弿缁犳垶鏅堕悧鍫涗簻闁哄啠鍋撻柟鐟版搐椤繐煤椤忓嫬绐涙繝鐢靛Т閸婃悂藟閸儲鈷戦柛娑橈梗缁堕亶鏌涢妸鈺€鎲剧€殿喗鐓℃俊鎼佸Ψ閵忊剝鏉搁梻浣虹帛钃遍柛鎾村哺瀹曨垵绠涘☉娆戝幈闂佺粯蓱閸撴艾鈻撳▎鎴炲枑闁规鍠栭埢鍫熸叏婵犲啯銇濈€规洏鍔嶇换婵嬪礃椤忓嫬姹查梻鍌氬€风粈浣规償濠婂懐绀婂〒姘ｅ亾闁诡喕鍗抽、姘跺焵椤掑嫮宓侀悗锝庡枟閺呮繈鏌嶈閸撴稓鍒掗崼鈶╁亾閿濆骸浜栧ù婊勭矒閺屸€愁吋閸愩劌顬夐梺姹囧妽閸ㄥ爼濡甸崟顖氱闁惧浚鍋呴崕鎾绘⒑鐎圭媭娼愰柛銊ユ健楠炲啴鍩￠崘鈺佺彴閻熸粌绉电€靛ジ骞囬悧鍫氭嫼闂佽崵鍠愬妯何ｇ紒妯镐簻闁瑰瓨绻冮ˉ鐘绘煃瑜滈崜鐔奉焽瑜旀俊鍓佺矙鐠恒劍娈鹃梺璇″灦閸嬪﹤危閻戣姤鈷戦悶娑掆偓鍏呭?sniroute.cmd up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯顢曢敐鍡欙紭闂侀€炲苯鍘搁柣鎺炲閹广垹鈹戠€ｎ亞锛滃┑掳鍊撻懗鑸电濞嗘劗绡€闁汇垽娼ф禒锕傛煕椤垵鐏︾€规洜鎳撶叅妞ゅ繐瀚弻褍鈹戦悩缁樻锭妞ゆ垵妫濋幃陇绠涘☉娆戝幈濡炪倖鍔х徊璺ㄧ不閵夛缚绻嗛柟缁樺笧缁夋椽鏌＄仦鍓с€掗柍褜鍓ㄩ幀銉╁礋椤掑倸缍冮梻鍌欑閹碱偊鎯屾径灞界筏闁稿﹦鍠撴禍浠嬫⒒娴ｇ懓鍔ょ紒缁樺笒閳诲秹濮€閵忊槅娼熼梺缁樺姇濡﹤銆掓繝姘厪闁割偅绻傞埀顒傛嚀閳诲秵绻濋崶銊у幐闂侀€炲苯澧撮柡灞芥椤撳ジ宕ㄩ崒锔剧暤闁哄本鐩鎾Ω閵壯傚摋缂傚倷鑳舵慨瀵稿椤撱垹鐒垫い鎺嗗亾缂佺姴绉瑰畷鏇㈠础閻忚缍婂畷鎺懶掔涵椋庣М妞ゃ垺鐟ч崰濠冩綇閵娿儳妲ｉ梻鍌欑窔濞佳囨偋閸℃稑绠犻幖鎼厛閺佸棝鏌曡箛瀣偓鏍偂閻斿吋鐓熼柡鍌氱仢缁狙勭箾閸涱叏鏀绘い銊ｅ劦閹瑩骞撻幒鎾搭啀婵犵數鍋為崹褰掑极鐠囪尙鏆︽い蹇撴噺婵挳鏌涜箛鎿冩Ч闁诲孩鎮傚濠氬磼濮橆兘鍋撳畡鎳婂綊宕堕鈧繚闂侀€炲苯澧撮柡灞剧〒閳ь剨缍嗛崑鍛暦瀹€鍕厸閻忕偛澧介埥澶愭懚閺嶎厽鐓曢柟鎵虫櫅婵＄厧顭块悷閭︽█婵?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯顢曢敐鍡欙紩闂侀€炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙娆炬綂闂佺偨鍎遍崯璺ㄨ姳閵夆晜鈷掑ù锝囶焾椤ュ繘鏌涚€ｂ晝绐旂€规洘娲樺蹇涘煛閸愵亞鍔跺┑鐘灱閸╂牠宕濋弽顓熷亗婵炲棙鎸哥痪褔鏌涢锝囩畵闁抽攱妫冮弻锝夋晲閸粌鎯堥梺姹囧労娴滎亪鐛崱姘兼Ъ闂佺顑冮崕鐢稿蓟閻旂⒈鏁婄紒娑橆儐閻や線姊虹拠鈥虫灆缂侇喗鐟ラ悾鐑藉Ω閿斿墽鐦堥梺鍛婂姦娴滄粎绮堟笟鈧濠氬磼濞嗘帒鍘＄紓渚囧櫘閸ㄦ娊寮鈧獮鎺懳旀担瑙勭彇婵＄偑鍊曠换鎰偓姘卞厴瀹曠敻寮撮姀锛勫幈濡炪倖鍔х徊鍓х矆閳ь剟姊绘担绋胯埞婵炶绠撻獮鍫ュΩ閵夘喗寤洪梺绯曞墲椤ㄥ懐绮诲ú顏呪拺缂備焦顭囩粻鏍ㄤ繆椤愩垹鏆ｆ鐐插暙铻栭柛鎰ㄦ櫅閺嬪倿姊洪崨濠冨闁告挻鐩棟闁靛鍎弨浠嬫煟閹邦垱纭鹃柣銊︽そ閺屾盯鎮╁畷鍥р吂濡炪倖娲╃徊鍧楀箯閻樿鍦偓锝庡亽濞兼梹绻濆▓鍨灍妞ゃ劌鎳庤灋婵炲棙鎸哥壕浠嬫⒒閸喍绶辨繛鎾愁煼閺屾洟宕煎┑鍥舵缂備焦顨嗙敮锟犲箖濡も偓椤繈宕￠悙鎻掔闂備胶纭堕弬渚€宕戦幘鎰佹富闁靛牆妫楃粭鍌滅磼閸ㄦ稈鍋撻弬銉︾亖濠电姴锕ら悧濠囨偂濞戙垺鍊堕柣鎰邦杺閸ゆ瑩鏌嶈閸撴氨鎹㈤崒鐑嗘晪闁挎繂顦柋鍥煛閸モ晛浠фい鎾虫惈椤啴濡堕崱娆忣潷缂備浇顕ч崐鍧楃嵁閸℃稑閱囬柣鏂垮缁犳岸姊洪棃娑氬婵炲眰鍊曢弳鈺呮⒒娴ｄ警鐒炬い鎴濇喘瀹曟垿濡舵径灞界ウ闂佺鎻粻鎴︽倷婵犲啨浜滈柟鐑樺灥閳ь剙鎽滅划濠囨晝閸屾稈鎷洪梺纭呭亹閸嬫盯宕濋幘顔界厱闁瑰灝瀚悾鐢碘偓娈垮櫘閸嬪懐鎹㈠┑瀣倞鐟滃繑顨ラ崶顒佲拺缂備焦锚婵牊绻涢崗鑲╂噰閽樼喖鎮楅敐搴濇喚婵炴挸顭烽幃宄扳枎韫囨搩浠奸梺宕囩帛濞叉﹢骞堥妸锔剧瘈闁告洦鍘肩粭锟犳⒑閻熸澘妲婚柟铏耿楠炲啴鎮滈懞銉у姺闂佹寧娲嶉崑鎾寸箾閸喓绠樼紒杈ㄦ崌瀹曟帒鈻庨幋顓熜滄俊鐐€栭崹闈浳涘┑鍡欐殾鐟滅増甯掗柨銈嗕繆閵堝倸浜剧紓浣哄У瀹€鎼佸箖濮椻偓閹瑩骞撻幒鍡樺瘱闂備浇宕甸崰搴ｇ礊婵犲洤钃熺€广儱鐗滃銊╂⒑閸涘﹥灏甸柛鐘崇墬娣囧﹦鈧稒蓱婵绱掗娑欑闁诲骸顭峰铏规喆閸曨偄濮㈢紒鍓ц檸閸欏啫鐣峰┑瀣櫇闁逞屽墴閳ユ棃宕橀鍢壯囨煕閳╁喚娈橀柣鐔村姂閺岋絾鎯旈姀鐘叉瘓闂佸憡鐟ラ崯鎾嵁閸愵喗鏅搁柣妯哄暱娴滄粓姊虹涵鍛汗闁稿顭囬懞杈╂嫚瀹割喗瀵岄梺闈涚墕濡鎱ㄩ敃鍌涚厱閻庯綆浜滈顏堟煛閸涱厾鍩ｆい銏＄懅娴滄悂寮介妸锔芥緫闂傚倷鑳堕崑銊╁磿鐎甸晲鐒婃繛鍡樻尰閸庢绻涢崱妯诲鞍闁绘挻鐩幃姗€鎮欓幓鎺嗘寖濠电偛寮堕幐鎶藉蓟濞戙垹鐓涢悗锝庡墰閻﹀牓鎮楃憴鍕閻㈩垱甯￠垾锕傚Ω閳轰胶顦板銈嗗姂閸ㄧ粯绂掗銏♀拻闁稿本鑹鹃埀顒€鍢查湁闁搞儺鍓﹂弫瀣煛瀹ュ骸骞栭柛銊ュ€搁湁闁绘挸娴烽幗鐘绘煕鎼淬埄娈滈柡灞剧〒娴狅箓宕滆濡晝绱撴担绛嬪殭婵☆偅绻堝濠氭晲婢跺﹦顓洪梺鎸庣箓濡厼螞閸曨垱鈷戠紓浣股戠亸浼存煟閳哄﹤鐏︾€殿喛顕ч濂稿醇椤愶綆鈧洭姊绘担鍛婂暈闁规瓕顕ч～婵嬪Ω瑜嶉崹?闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪Ч閿濈偛鈹戠€ｎ偅娅囬梺绋跨焿婵″洨绮欒箛鏃傜瘈闁靛骏绲介悡鎰版煕閺冣偓閻楃姴鐣烽幋锕€围濠㈣泛顑囬崢鎼佹⒑閹肩偛鍔楅柡鍛☉閻剟姊绘担铏瑰笡閻㈩垱甯￠弫鍐Ψ瑜庡畷鍙夌箾閹存瑥鐏╂鐐灪娣囧﹪顢涘┑鍥朵哗婵炲濮甸幐鍐差潖濞差亜绠归柣鎰絻椤绱撴担绛嬪殭闁绘鎸搁锝嗙節濮橆儵鈺呮煃閸濆嫬鈧憡绂嶅鍫熲拺闁告稑锕︾粻鎾绘倵濮橆剚鍤囧┑顔瑰亾闂侀潧绻堥崹濠氭晬濠婂牊鈷戦梺顐ゅ仜閼活垱鏅剁€电硶鍋撶憴鍕鐎规洦鍓熼崺銉﹀緞婵炵偓鐎婚梺鐟扮摠缁诲倻绮婚悙鐑樷拻闁稿本鐟х粣鏃€绻涙担鍐叉硽閸ャ劌顕遍柡澶嬪灥閻庮參姊哄Ч鍥х伄妞わ絼绮欏畷鎴﹀箻閹颁焦鍍甸梺缁樻尭鐎涒晠鎮靛┑瀣參婵☆垵宕电粻鐐烘煛鐏炵偓绀冪€垫澘瀚板畷鍗炍熼懡銈呯稻闂傚倷鐒︽繛濠囧绩闁秴鍨傞柛褎顨呴拑鐔哥箾閹寸們姘跺绩娴犲鐓曢柍鈺佸枤濞堛垹霉鐏忔牕浜惧┑鐘垫暩婵參骞忛崘顔肩倞闁挎棁妫勯弸銈夋⒒娴ｅ憡鎯堥柡鍫墴閹嫰顢涢悙闈涚ウ濠碘槅鍨伴惃鐑藉磻閹剧粯顥堟繛鎴炵懃閺嗗牓姊洪棃鈺傗偓銉╁炊閵娿垺瀚?DNS 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珖闂侀€炲苯澧板瑙勬礋閹兘骞婃繝鍌滅Ш闁诡喒鏅涢蹇涱敊閻撳骸螚缂傚倸鍊搁崐鍝ョ矓閻㈠憡鍋嬮柡鍥ｆ噰閳ь剨绠撳畷绋课旀担鍛婄杺闂傚倸鍊搁悧濠勭矙閹达讣缍栭柛銉墯閳?闂?
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
        // up 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍讲鏋欓梺缁樺笩婵倗鎹㈠☉銏犲耿婵☆垵顕х喊宥夋煟閻斿摜鎳曠紒鐘虫崌楠炲啫螖閸涱喗娅滈柟鑲╄ˉ閸撴繈鎮樺澶嬧拺闁硅偐鍋涢埀顒佹礋閹崇喖顢涘┑鍫滅胺闂傚倷鐒︾€笛呮崲閸岀偛绠犻幖娣妼濮瑰弶绻涢崱妯诲鞍闁绘挻娲熼幃姗€鎮欐０婵嗘婵犵绱曢弲顐ゆ閹烘梻纾兼俊顖氬悑閸掓稑螖閻橀潧浠滄い鎴濇嚇閸┿垺鎯旈妶鍥╂澑闂佸搫娲︾喊宥夊磻閸涱垳纾?闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞鐟滃繘寮抽敃鍌涚厱妞ゎ厽鍨垫禍婵嬫煕濞嗗繒绠抽柍褜鍓欑粻宥夊磿闁单鍥敍閻愭潙鈧嘲鈹戦悩鍙夊闁绘挸绻愰埞鎴︽偐閼碱剛顔掑┑锛勮檸閸犳鎹㈠☉銏犻唶婵犻潧娲ら弳鍫㈢磽娴ｈ櫣甯涚紒璇茬墦楠炲啯绂掔€ｎ偒妫冨┑鐐殿棎閸╂牠鎯勯鐐茶摕婵炴垶鍩冮崑鎾绘晲鎼粹€茬凹閻庤娲栭惌鍌炲蓟閿涘嫪娌柛鎾楀嫬鍨卞┑鐘殿暜缁辨洟宕楀鈧妴浣糕枎閹炬潙浜楅柟鍏兼儗閸犳绂嶉妶鍥╃＝闁稿本鑹鹃埀顒勵棑閹峰寮婚妷銉э紱闂佸湱鍋撻崜姘舵偄閸℃稒鐓犻柟闂寸劍濞懷冣攽椤旇偐澧﹂柡宀嬬秮楠炴﹢宕橀崣澶嬵啋闂備礁鎼張顒€煤濡警鍤楅柛鏇ㄥ灠闁卞洦绻濋棃娑欘棞妞ゎ偅鎹囧缁樻媴閸涘﹥鍎撳┑鈽嗗亝缁诲啰绮嬪澶婇唶闁哄洨鍋犻幗鏇㈡⒑鐠恒劌鏋斿┑顔芥綑椤斿繐鈹戦崶銉ょ盎闂佸搫鍟崐鍛婄妤ｅ啯鐓曢柨婵嗗瀹撳棙鎱ㄦ繝鍐┿仢鐎规洦鍋婂畷鐔碱敆娴ｇ懓顏扮紓鍌氬€风粈渚€顢栭崱娑樼闁搞儺鍓欓弰銉╂煃瑜滈崜姘跺Φ閸曨垰绠抽柟鎼灠婵稓绱撴担鍓叉Ш闁轰礁顭峰璇测槈濮橈絽浜鹃柨婵嗙凹閹茬偓淇婇幓鎺撹础缂佽鲸甯″顕€宕掑顓犫偓濠氭煣娴兼瑧鍒伴柍瑙勫灴瀹曟帒鈹戦崶鑸殿棄缂傚倸鍊哥粔鎾晝椤忓牏宓侀柛鎰靛枛閻撴盯鏌涘☉鍗炴灈婵炲牄鍊曢—鍐Χ閸℃鐟愰梺缁樺釜缁犳挸鐣锋导鏉戝唨妞ゆ挾鍟块幏缁樼箾鏉堝墽鎮奸柣鈩冩瀹曠敻宕橀鐣屽幈闂侀潧顭堥崕閬嶆嚋瑜版帗鎳氶柣鎰劋閻撴洟鏌熸导瀛樻锭闁哄鐩弻锕傚礃椤忓嫭鐏堥梺鍝勬湰濞叉鎹㈠☉婊庢Х闂佺顑嗛幑鍥蓟濞戞埃鍋撻敐搴′簼鐎规洖鐭傞弻鈩冩媴閻熸澘顫掗悗瑙勬磸閸旀垿銆佸☉妯锋瀻闁硅揪鑵归弨娲⒒閸屾艾鈧兘鎮為敂鐣岀濠电姴娲ょ紒鈺呮煕濡ゅ啩绱虫繛宸簼閸嬪嫬鈹戦崒姘兼Ч缂傚秴楠搁埞鎴︽倷閸欏鏋欐繛瀛樼矋缁秶鈧潧銈搁幃鈺冪磼濡厧骞堥梻浣告惈濞层垽宕濈仦瑙ｆ瀺婵☆垵宕电粻楣冩煟閹捐櫕鎹ｉ柛鈺嬬稻閹便劍绻濋崨顕呬哗缂備緡鍠楅悷鈺呯嵁閹邦厽鍎熼柍鈺佸暞閺嗘﹢姊虹拠鍙夊攭妞ゎ偄顦叅闁绘柨顨庨悞鐣屾喐瀹ュ懐鏆ら柛鏇ㄥ灡閳锋垹鎲搁悧鍫濈瑨濞存粈鍗抽弻娑滅疀閺冩捁鈧法鈧鍠撻崝宥囩矉閹烘柡鍋撻敐搴′簽闁告ê宕—鍐Χ閸℃顫庢繝娈垮枟閹告娊骞冨鈧崺锟犲礃閿濆懍澹曢柣鐔哥懃鐎氼厾澹曢幖浣圭厱閹兼番鍔嬮幉楣冩煃閵夛附顥堢€规洘锕㈤、娆撳床婢诡垰娲﹂悡鏇㈡煃閳轰礁鏆熼柟鍐插暞閵囧嫰顢曟惔鈩冨闁抽攱鍨块弻銈吤圭€ｎ偅鐝曢梺鎼炲€愰崑鎾斥攽閻橆偅濯伴柛娑卞枟閻濐亜鈹戦垾鍐茬骇闁告梹鐟╅獮鍐ㄢ枎閹惧厖绱堕梺鍛婃处閸欏酣宕€ｎ喗鈷掑ù锝呮啞閹牓鏌涙繝鍌滃煟鐎规洘娲熷濠氬Ψ椤栨粎鈼ら柣搴＄畭閸庨亶藝娴兼潙鐓曢柟杈鹃檮閸嬧剝绻涢崱妤冪妞ゅ繆鏅犻弻娑㈠棘鐠囨祴鍋撳Δ鍛剦妞ゅ繐妫涚壕濂告煏韫囨洖孝缂佺姵妞藉铏圭磼濮楀棙鐣堕柣搴㈢▓閺呮繈骞戦姀鐘斀闁割偅绻冮悗鍐测攽閳藉棗鐏犻柟纰卞亰閸╂稒寰勯幇顓炰画濠电姴锕ょ€氼厾娆㈤弻銉︾厱闁绘柨鎲＄亸锕傛煟濞戝崬鏋涢摶鏍煃瑜滈崜鐔煎灳閿曞倸惟闁宠桨绀佺粣娑橆渻閵堝棙灏甸柛瀣枑缁傚秵瀵肩€涙ǚ鎷绘繛杈剧悼椤牓寮抽柆宥嗙厵缁炬澘宕禍鎵偓瑙勬处閸ㄥ爼骞冮埡鍐╁珰闁肩⒈鍓ㄧ槐閬嶆⒒娴ｇ儤鍤€濠⒀呮櫕閸掓帡顢涢悙鑼煣闂佸搫琚崕杈╃不瑜版帗鍊垫繛鎴炵懐閻掔晫绱掗悩绛硅€块柡宀嬬秮閸┾剝绗熼埀顒勫矗閸曨剚鍙忓┑鐘叉噺椤忕姷绱掓潏銊ョ瑨閾伙綁鎮归崶銊у弨闁诲氦娅ｇ槐鎾诲磼濞嗘劗銈板銈嗘肠閸ャ劌浜遍梺鍦亾閸撴岸宕甸弴銏″仯闁搞儯鍔庨妶瀛樹繆閹绘帞澧涚紒缁樼箖缁绘繈宕掑☉妯活仱闂備礁鎼ˇ浼村箖閸岀偛钃熼柨婵嗘閸庣喐銇勯弽銊х煂閺嶁€斥攽閻愯尙鎽犵紒顔肩Ф閸掓帡骞樼紒妯绘珳闂佺粯鍔曢幖顐︽倷婵犲啨浜滈柟鍝勭Х椤斿鏌熼悿顖涱仩缂佽鲸鎸婚幏鍛村礈閹绘帒澹堟繝鐢靛仧閵嗗鎹㈠┑瀣ㄢ偓渚€骞樺鍕⒐閹峰懘宕楅崫銉ф晨闂傚倷绀侀幖顐⒚洪姀銈呯鐎光偓閸曨厼绁﹀┑掳鍊曢崯鎵婵傚憡鐓曢悘鐐靛亾閻ㄦ垿寮崼銉﹀€甸悷娆忓绾炬悂鏌涢弬璺ㄐら柟骞垮灩閳规垹鈧綆浜為ˇ銊ヮ渻閵堝懐绠伴柡灞斤躬婵¤埖寰勭€Ｑ勫濠电偠鎻徊浠嬪箺濠婂牆鍑犻柛鎰ㄦ櫃缁诲棙淇婇悙闈涘付濠德ゆ硶缁辨帡顢欓懖鈺侇杸闂佺懓鍢查幊妯虹暦椤愶箑绀嬮柛顭戝亽濡儵姊婚崒娆掑厡妞ゎ偄顑夊畷鐔煎垂椤愩倖娈鹃梻鍌欑劍婵炲﹪寮ㄩ柆宥呭瀭闁割煈鍣鏍р攽閻樺疇澹橀梺鍗炴喘閺岋綁寮崶銉㈠亾閳ь剙鈹戦鍏兼悙妞ゎ亜鍟存俊鍫曞礃閵娿儱顫氱紓鍌欑贰閸犳骞戦崶褜鍤曢悹鍥ㄧゴ濡插牊绻涢崱妯虹仼闁伙箑鐗婄换娑欐綇閸撗呅氬┑鐐叉嫅缂嶄線骞冮敓鐘茬缂備焦菤閹锋椽鏌ｉ悩鍙夌闁逞屽墲濞呮洟鎮橀崼婵冩斀闁绘劘灏欏В鐐烘煃瑜滈崜娆戝椤撱垹姹查柨鏇炲€归悡鏇熶繆閵堝懎鏆欏ù婊冪秺閺岋紕鈧綆浜崣鍕煛瀹€鈧崰鏍蓟閸ヮ剚鏅濋柍褜鍓氶弲鍫曞箥椤旇棄鏋戦梺缁橆殔閻楀﹦浜搁敂鑺ュ弿濠电姴瀚敮娑氱磼濡ゅ啫鏋涢柛鈹惧亾濡炪倖宸婚崑鎾淬亜椤撶偞绌挎い锔芥尦閺岀喖鐛崹顔句紙閻庤娲栧畷顒勫煡婢跺á鐔哄枈鏉堛剱銈呪攽閻樺灚鏆╁┑鐐╁亾濠电偘鍖犻崗鐘虫そ婵℃悂鍩炴惔鎾充壕闁告稑鐡ㄩ悡銉╂倵閿濆倹娅囩紒鐘宠壘椤啴濡堕崱娆忣潷闂佽崵鍠嗛崝鎴︺€佸Δ鍛亜闁稿繗鍋愰崢?30 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氶梻浣藉Г钃辩紒璇茬墦瀵鎮㈢亸浣圭亖闂佸壊鐓堥崰妤呮倶瀹ュ鈷戦柛婵嗗椤ョ偟绱掗鑲┬у┑锛勬暬瀹曠喖顢欓崜褎婢戦梻浣告贡閸嬫捇骞嗙仦杞挎盯鍨鹃幇浣瑰瘜?
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣濞嗗繒鍔撮梺杞扮椤戝棝濡甸崟顖氱閻犺櫣鍎ら悗楣冩⒑閸涘﹦鎳冪紒缁橈耿楠炲啫螖閸涱噮妫冨┑鐐村灦閻燂箓宕伴幇鐗堚拺婵炶尪顕ч獮妤呮煙閹间胶鐣洪柛鈹惧亾濡炪倖甯掗崰姘焽閹邦厾绠鹃柛娆忣槺婢ь亪鎮￠妶澶嬬厪濠电倯鍐╁櫢缂佹顦靛娲传閸曨偀鍋撹ぐ鎺濇晞婵炲棙鎸婚崑鍕煕閳╁啰鈯曢柛瀣剁秮閺屾稑鈹戦崱妤婁痪闂佽鍨伴悧濠囧Φ閸曨垰惟闁挎梻鍋撻崳浼存⒑閸濆嫮鐒跨紓宥勭窔閻涱噣骞掑Δ鈧粻锝嗐亜閺嶃劎绠撻柟顖氱墦閺岋絾鎯旈姀鈺佹櫛闂佸摜濮甸悧鐘诲箖閵夆晜鍋傞幖杈剧稻濞堥箖姊虹紒妯诲碍閻庡灚甯掗…鍥煛閸屾ü绨婚梺瑙勫礃濞夋盯寮搁弮鍌涘枑闁哄瀵ч崵鍥煛瀹€瀣ɑ闁诡垱妫冩俊鑸垫償閵忋垻啸濠电姷鏁搁崑娑橆嚕閸洘鏅梺缁樻尪閸婃繈寮婚敐澶婄睄闁稿本鑹炬禒妯肩磽娴ｅ搫校闁绘顨嗙粚杈ㄧ節閸ヨ埖鏅┑鐘绘涧濞层劑濡存繝鍥ㄢ拺婵懓娲ら埀顒佸姍瀹曟垿骞樼紒妯锋嫽婵炶揪绲块悺鏂款焽閹扮増鐓曢幖娣灩閳绘洟鏌熼鍏煎仴鐎规洖鐖奸、妤呭焵椤掑嫬鍙婇柕澶嗘櫆閻撶喖鏌熼柇锕€鏋涢柛鏂款儔閺屽秹鏌ㄧ€ｎ亞浼岄梺鍝勬湰缁嬫垿鍩為幋锕€绀嬫い鎰╁灩椤捇姊绘担鍛婃儓婵☆偅绻冪粋宥夊醇閺囩偠鎽曞┑鐐村灦閸╁啴宕戦幘缁樻櫜閹肩补鈧尙鍑归柣搴ゎ潐濞叉牠鎯岄崒鐐茶摕闁炽儱纾弳鍡涙倵閿濆骸澧扮悮锕傛⒒娴ｇ瓔鍤冮柛鐘冲浮瀵煡鎮╅崣鍌涚洴瀹曟﹢濡搁姀鈽嗘綌婵犵妲呴崹顖滄媰閿曞倹鍊甸梺顒€绉甸埛鎺楁煕鐏炲墽鎳呴柛鏂跨Ч閺岀喖顢欓懖鈺佺厽閻庤娲樼换鍌炴偩濠靛鐒垫い鎺嶈兌閻濆爼鏌￠崶銉ョ労闁轰礁娲弻锝呂熺拠鑼シ闂佽绻愰敃顏勵潖濞差亜绀冮柛娆忣槹閸庢捇姊洪悷鏉挎毐婵炲鍏橀幆鈧い蹇撴祩濡嫰姊洪崫鍕伇闁哥姵鎹囬崺鈧い鎺嶈兌閳洟鎷戦柆宥嗙厓闂佸灝顑呮慨宥夋煛?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф婵°倧绲介崯顖烆敁瀹ュ鈷戠紒瀣儥閸庡繑銇勯幋婵囧殗闁糕晝鍋ら獮瀣晜閽樺姹楅梻浣告贡缁垳鏁繝鍥х獥濠电姴娲﹂埛鎴︽煕韫囨挸鎮戠紒璺哄级缁绘稓娑垫搴ｇ槇閻庢鍠涢褔鍩ユ径鎰潊闁绘ɑ鐗撻崝宥夊Φ閸曨垰绫嶉柛灞捐壘娴犳ê顪冮妶搴′簮闁哄懏绻勫Σ鎰板箻鐎涙ê顎撻梺鍦帛鐢﹥绔熼弴鐐╂斀闁绘﹩鍠栭悘閬嶆煕閳哄倻澧电€规洘绻堝浠嬵敇閻旇渹绨甸梻浣告啞閸旓箓宕板Δ鍛亗闊洦鎼╅悢鍡涙偣妤︽寧顏犲褎娲熼弻娑㈠籍閳ь剙鐣濋幖浣歌摕闁跨喓濮撮獮銏＄箾閹寸儐娈曢柛鐘冲哺濮婅櫣鎷犻垾铏仌濡炪値鍋勯ˇ鐢哥嵁韫囨稑宸濋柡澶嬪灦瀹撳秴顪冮妶鍡樺暗闁稿绋撶槐鎺楀煛閸涱喒鎷绘繛杈剧到閹诧繝骞嗛崼銉﹀仩婵鍘ф禍鎵偓瑙勬礃閸旀洝鐏冮梺鍛婁緱閸橀箖宕濋敃鈧—鍐Χ閸℃娼戦梺绋款儐閹歌崵鎹㈠┑瀣劦妞ゆ帊鐒︾紞鍥煏婵犲繒鐣辩€殿喖娼″鍝勑ч崶褏浠惧┑鐐靛帶閺堫剙顕ラ崟顓涘亾閿濆簼绨界紒鎰仱濮婇缚銇愰幒鎴滃枈闂佸憡锕㈢粻鏍х暦閵忋倖鐒肩€广儱妫岄幏娲⒒閸屾氨澧涘☉鎾舵嚀閳藉濮€閻樻妲伴梻浣虹帛閸旓箓宕滃鑸靛剨闁哄啫鍊荤壕浠嬫煕鐏炴崘澹橀柍褜鍓欓崲鏌ユ箒闂佹悶鍎滅仦鍓ь洬闂備線娼ц噹闁告劑鍔庢禍鐗堢節閻㈤潧浠滄俊顐ｎ殘閹广垽骞掑Δ鈧Ч鏌ユ煥閺囩偛鈧綊鎮″▎鎾寸厵婵炲牆鐏濋弸娑欍亜閿濆懌鍋㈤柡灞剧〒閳ь剨缍嗘禍婵嬎夊鍫燁梿濠㈣埖鍔栭悡銉︾節闂堟稒顥炵紒浣叉櫊閺岋繝鍩€椤掍礁顕遍悗娑欘焽閸樻悂姊洪崨濠勨槈婵炲眰鍊濆畷顖炴偐鐠囪尙鍝楁繛瀵稿Т椤戝棝鎮￠悢鍏肩厵闂侇叏绠戦獮妤冪磽瀹ュ棗鐏撮柡宀嬬秬缁犳盯濡疯閺嗐倝姊洪崫鍕伇闁哥姵鐗曢悾鐑藉Ω閿斿墽鐦堥梺鍛婂姂閸斿宕戦幘璇茬疀闁绘鐗忛崢鍛婄箾鏉堝墽鎮兼い顓炵墦瀵娊鏁愭径瀣幐婵炶揪绲块幊鎾存叏瀹ュ洠鍋撶憴鍕闁绘牕銈稿畷娲焺閸愨晛顎撳┑顔界箓閻寮撮姀鈾€鎷婚梺绋挎湰閻熝囁囬敃鍌涚厵缁炬澘宕禍浼存寠濠靛枹褰掑礂閸忚偐绋囬梺鎼炲妼閸婂潡寮婚敐澶婄睄闁稿本鑹炬禒姗€姊洪崨濠傜仼濠电偐鍋撳Δ鐘靛仦閻楁洝褰佸銈嗗坊閸嬫挸鈹戦垾鑼煓闁哄矉缍侀獮姗€宕￠悙鎻捫曢梻浣告惈閻绱炴担鍓插殨妞ゆ帒瀚粻浼村箹濞ｎ剙鐏╅柛鎾舵暬閺岋絾鎯旈姀鈶╁缂備浇顕ч崐鍧楀春閻愬搫绠ｉ柨鏃囨閳ь剛鍏橀弻娑樷枎韫囷絾笑濠电偛鎷戠换婵嗩潖濞差亜绠归柣鎰絻椤棛绱撴笟鍥ф灍闁荤喆鍎甸、姘舵晲閸ャ劌鐝板┑鐐存綑椤戝棝锝炲鍛斀妞ゆ梻鐡斿▓鏃€淇婇锝囨噰鐎规洘鎹囬幃浠嬪川婵炵偓瀚肩紓鍌氬€烽悞锕佹懌闁诲繐娴氶崢鐣屾崲濞戙垹绠绘い鏃囧Г閺嗙姷绱撴担绋库偓鍦暜閿熺姰鈧礁顫滈埀顒勫箖閵忋倖鎯為柛锔诲幗椤矂姊婚崒娆戭槮闁哥喎娼￠幆鍐ㄢ攽鐎ｎ亞锛涢梺瑙勫劤閻忓牓宕戦幘鑽ゅ祦闁割煈鍠栨慨搴♀攽閻愯尙婀撮柛鏃€鍨甸锝夊箮閽樺）褍顭跨捄渚剰闁逞屽墮閻忔氨鎹㈠☉銏犻唶婵犻潧鐗呴搹搴♀攽閻愬弶鍣洪柨鏇樺灲瀵鈽夐姀鐘殿啋闁荤姾娅ｉ崕銈夋倶閸儲鈷戦梻鍫熺⊕椤ユ瑧绱掗埀顒佺瑹閳ь剟鏁愰悙宸叆闁割偅绻勯崝锕€顪冮妶鍡楃瑐婵＄偞甯為幑銏ゅ幢濡寮垮┑鐘绘涧濡盯鎮為悙顑跨箚闁告瑥顦崝銈嗐亜椤忓嫬鏆ｅ┑鈩冨劶缁犳盯骞橀懜鏁屾帡姊绘担渚劸妞ゆ垵鎳橀弫鍐Χ婢舵稓绋?
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
        // up 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮撮悙娴嬪亾瑜版帒纾块柟瀵稿У閸犳劙鏌ｅΔ鈧悧鍡欑箔閹烘梻纾奸柍褜鍓氬鍕沪缁嬪じ澹曢梺绋跨箰椤︻垱绂嶆ィ鍐┾拺闂侇偆鍋涢懟顖涙櫠閹绢喗鐓曢柍瑙勫劤娴滅偓淇婇悙顏勨偓鏍暜婵犲嫮鐭嗗〒姘ｅ亾鐎规洜鏁婚、妤呭礋椤掑倸骞堟繝鐢靛仜濡鎹㈤幇鏉挎辈闁挎繂顦伴悡蹇涙煕閳╁啯绀堟い锔肩畵閹藉爼寮介鐔哄帗閻熸粍绮撳畷婊堟偄閻撳海浼嬮梺鎸庢礀閸婂憡鍎梻浣哥枃濡椼劎绮堟担铏圭幓婵炴垶锕╁〒濠氭煏閸繃顥炴い銉ユ閳规垿顢欓崫鍕ㄥ亾濠靛鈧礁鈻庨幘宕囧€炲銈嗗笂鐠佹煡骞忕紒妯肩閺夊牆澧介崚浼存煙鐠囇呯瘈鐎规洦鍨堕幃娆撴偨閻㈢绱查梻浣告啞缁诲倻鈧凹鍙冨畷鎴﹀Ψ瑜夐崑鎾舵喆閸曨剛顦ㄥ┑鐐跺皺閸犲酣鎮惧畡鎷旂喐绺介挊澶嬭础闁逞屽墾缂嶅棝宕戦崱妯碱洸婵炲棙鎸婚埛鎺戙€掑锝呬壕濠电偘鍖犻崨顔煎簥闂佸壊鍋侀崕杈╁瑜版帗鐓涚€广儱楠搁獮妤呮煕鐎ｎ亜鈧湱鎹㈠☉銏犵闁绘垵娲ｇ欢鐢告⒑缂佹ɑ鐓ユ俊顐ｇ箞楠炲啴鎮烽幊濠冩そ椤㈡棃宕卞▎鎴犲炊闂備浇顕х€涒晠鎯岄鈧畷锟犲箮閽樺鎽曢梺鎸庣☉鐎氀囧磻閹剧粯鏅查幖瀛樼箘閻╁骸顪冮妶鍡楃闁搞劋绮欏濠氭偄閸濄儳鎳濋梺鍓茬厛閸犳牠鈥栭崼銉︾叄濞村吋鐟ч幃鑲╃磼鏉堛劌绗ч柍褜鍓ㄧ紞鍡樼閻愬顩烽柕寰涢绨诲銈嗗坊閸嬫挻銇勯敃浣峰惈婵″弶鍔欓獮妯尖偓娑櫭鎾剁磽娴ｅ壊鍎忕紒銊╀憾瀹曟垿骞樼拠鍙夊祶濡炪倖鎸鹃崰鎰枔娴煎瓨鈷戦悹鎭掑妼閺嬫柨鈹戦鑺ュ唉鐎殿喖鎲＄粭鐔煎焵椤掑嫬钃熼柣鏂垮悑閸嬪鏌涢銈呮灁闁告埊绻濆缁樻媴缁嬭儻鍩炲┑鐐额嚋婵″洭骞戦姀鐘闁靛繒濮烽娲⒑缂佹ê濮岄悘蹇旂懇閹兘濡搁敂鍓ь啎闁诲孩绋掗…鍥儗閹烘梻纾奸柣妯虹－婢х數鈧鍠涢褔鍩ユ径鎰潊闁绘鏁搁弶鎼佹⒒閸屾艾鈧悂鎮ф繝鍕煓闁圭儤顨嗛崐鍫曟煕椤愮姴鍔滈柣鎾跺枛楠炴牕菐椤掆偓閻忓崬顭跨憴鍕嗘垿濡甸崟顖涙櫇闁稿本鐟﹂崰姘舵⒑缁洘鏉归柛瀣尭椤啴濡堕崱妤€娼戦梺绋款儐閹稿墽妲愰幒妤佸亹闁肩⒈鍎疯閳ь剝顫夊ú妯好洪悢鐓庤摕闁糕剝顨忛崥瀣煕濞戝崬鐏ｉ柡瀣暣濮婃椽鎮烽弶鎸庡€梺璇″灠閻倿骞冮埄鍐ㄧ窞鐎光偓閳ь剛绮堟繝鍥ㄧ厸鐎广儱楠搁獮妤呮煟閹惧磭绠婚柡灞剧洴椤㈡洟鏁愰崶鈺冩毇缂傚倷璁查崑鎾垛偓鍏夊亾闁告洦鍓涢崢閬嶆煟鎼搭垳绉甸柛瀣噹閻ｅ嘲鐣濋埀顒勫焵椤掑喚娼愭繛鍙夌矒瀹曟顫滈埀顒勫Υ娴ｈ倽鐔封枎閻愵儷顒勬⒒娴ｅ憡鍟炴慨濠傜秺楠炲啴宕掗悙鑼暫濠德板€愰崑鎾绘煃缂佹ɑ宕岀€规洖缍婇、娆撴偩鐏炶偐鐭氶梻鍌氬€烽懗鍓佹兜閸洖鐤炬繝濠傜墛閸嬪鏌ｅΟ鍨毢闁哄棴闄勯幈銊ヮ潨閸℃绠荤紓浣叉閸嬫捇姊绘担瑙勫仩闁稿寒鍨跺畷婵嗩吋婢跺﹦锛熷銈呯箰閻楀﹪鎮￠悢鍏肩厽闁哄倹瀵ч幉鎼佹煙閸愭彃鏆ｉ柟顔筋殔椤繈鎮℃惔鈾€鎷梻浣筋嚃閸ㄤ即宕幘顔衡偓渚€寮撮姀鈩冩珕闁荤喐鐟辩徊鍓р偓姘偢濮婄粯鎷呴崨濠傛殘缂備礁顑嗛崹鍧楀极閸愵喖围濠㈣泛顑呴埀顒€鐖奸弻锝夊棘閸喗鍊梺缁樻尰濞叉牠鍩為幋锔藉亹闁圭粯甯楅悘鎾斥攽閻愯尙澧涚紒顔芥崌瀵鏁愰崨顏咁潔闂佸憡顨堥崑鐐烘偟濠靛洨绡€婵炲牆鐏濋弸鎾绘煕鐎ｎ偅宕屾慨濠冩そ濡啫鈽夊杈╂澖闂備焦鎮堕崝宀勨€﹂悜钘夋瀬鐎广儱鎷嬮悡銉╂煕椤愩倕鏆遍柟閿嬫そ濮婃椽宕ㄦ繝鍕暤闁诲孩鍑归崜鐔奉嚕閹惰姤鍊烽柣鎴烆焽閸橀亶姊洪棃娴ゆ盯宕熼浣烘闂傚倷娴囬鏍窗濡ゅ懏鏅濋柕蹇嬪€曠粻鏍煃閸濆嫭鍣归柛銊ュ€归妵鍕箛閳轰胶浠鹃梺璇查叄缁犳牕顫忕紒妯诲闁惧繒鎳撶粭鈥愁渻閵堝啫濡奸柟铏耿楠炲啳顦剁紒鐘崇洴楠炴ɑ鎷呴懞銉哗闂侀€炲苯澧紒瀣浮閺佸啴鍩℃担鍙夌亖婵炲濮撮鍡涙偂閻斿吋鐓欓梺顓ㄧ畱楠炴绱撳鍡楃仸缂佺粯鐩獮鏍敇閻愬浜梻浣哥枃椤宕归崸妞尖偓浣糕枎閹炬潙浠俊顐︻暒缁€渚€藟濮樿埖鈷掗柛灞捐壘閳ь剛鍏橀幃鐐烘晜闁款垰浜剧紒妤佺☉閹虫劙鎯屽▎鎾寸厵閻庣數顭堣ⅴ闂佺顑嗛幐鎼侊綖濠靛唯闁靛鍎哄搴ㄦ煟鎼淬値娼愭繛鍙夛耿瀹曞綊宕稿Δ鈧粻鏍ㄧ箾閸℃ɑ灏伴柛濠勭帛娣囧﹪顢涢悙瀛樻殸闁诲孩鑹鹃妶绋款潖缂佹ɑ濯撮柛娑橈工閺嗗牏绱撴担鍓插剱闁搞劌娼″顐﹀礃椤旇姤娅囬梺绋挎湰濮樸劑鎮块崶顒佲拺鐟滅増甯掓禍浼存煕鎼搭喕閭柟顔惧厴閺屽洭鏁傞幆褍姹查梻鍌欑閹测剝绗熷Δ鍛偍闁告繂瀚€閿濆绠涢柣妤€鐗嗛埀顒€鐏氱换娑㈠醇濠靛牅铏庡┑鐐叉噺閿曘垽寮诲☉姘ｅ亾閿濆簼鎮嶉柡鈧拠鐫酣宕惰闊剟鏌熼搹顐ょ煉婵☆偄鍟埥澶婎潩鏉堚晛绁﹀┑鐘垫暩婵兘寮崨濠冨弿閻庣數纭堕崑鎾愁潩椤愩垹绁梺璇″枙閸楁娊鐛Ο鑲╃＜婵☆垶鏀辩€氳棄鈹戦悙瀛樺鞍闁糕晛鍟村畷鎴﹀箻鐎靛摜顔曟繛杈剧到閸熷灝鈻斿璺虹倞妞ゆ帒顦伴弲锝夋⒑闂堟稓绠冲┑顔惧厴閺佸秹鎮㈤崗灏栨嫽婵炴挻鑹惧ú銈嗘櫠椤斿墽纾煎璺哄绾箖鏌ｉ敐鍛粵缂佹鍠栭崺鈧い鎺戝閽冪喖鏌ｉ弬鎸庡暈缂傚秴娲弻娑㈠箻濡も偓閹冲繐鈻撻崫銉х＝闁稿本鐟ㄩ崗灞解攽椤旂⒈鍤熺紒杈╁仦缁绘繈宕惰閻ｅ搫鈹戞幊閸婃劙宕戦幘缁樼厓闁芥ê顦藉Σ鎼佹煃鐠囪尙效闁圭厧缍婇、鏇㈠椤厽鍋呯紓鍌氬€风欢锟犲窗濡も偓椤繈濡搁埡浣侯唹闂佸憡娲﹂崹鐗堝劔闁荤喐绮岀换姗€寮鍜佺叆闁割偆鍠撻崢鍛婄節閵忥絾纭鹃柨鏇檮閺呭爼骞嬮敂鐣屽幈闂佸搫鍊圭€笛囧箲閿濆洨纾兼い鏃傗拡閻撹偐鈧鍠曠划娆撱€侀弴銏犖ч柛銉╊棑鎼村﹪姊绘担绛嬪殭闁告垹鏅槐鐐哄幢濡⒈娲搁柣鐘烘〃缂嶅秹鏁愭径濠傜獩婵犵數鍋涢妵妯艰姳婵犳碍鈷戦柣鐔告緲閳锋棃鏌涚€ｃ劌濮傜€规洘鍨挎慨鈧柍鍨涙櫅娴滈箖鎮峰▎蹇擃仾缂佲偓閳ь剟姊洪棃娑氬闁诡喖鍊挎俊瀛樼瑹閳ь剙顕ｉ幘顔碱潊闁斥晛鍟悵鎶芥⒒娴ｈ鍋犻柛搴櫍瀵彃鈽夐姀銏犵ウ闂侀€炲苯澧存慨濠傤煼瀹曟帒顫濋钘変壕闁归棿绀佺壕鐟邦渻鐎ｎ亜顒㈡い鎰Г閹便劌顫滈崱妤€鈷掗梺缁樺笂缁瑩寮婚妶鍡樺弿闁归偊鍏橀崑鎾诲冀椤愮喎浜炬慨姗嗗墻閻撳ジ鏌″畝瀣М鐎殿噮鍣ｅ畷鎺戭潩椤戣法甯涚紓鍌氬€风拋鏌ュ磻閹炬剚鐔嗛悹鍝勬惈椤︻剟鏌涚€ｎ偅宕岀€规洘顨嗗鍕節娴ｅ壊妫滈梻鍌氬€风粈渚€骞夐垾瓒佹椽鏁冮崒姘鳖槶濠电偛妫欓崝妤呫€呭畡鎵虫斀闁稿本纰嶉崯鐐烘煃闁垮鐏╃紒杈ㄦ尰閹峰懘鎯傞梹鎰繑闂備胶绮敮鐐哄疾椤愩倖顫曢柟鐑橆殢閺佸鏌涘☉鍗炲箻濞寸姵鎮傚铏规嫚閳ヨ櫕鐝紓浣虹帛缁诲倿鎮鹃悜钘夌闁瑰瓨姊归弫鐘绘⒑鐠団€崇€婚柛灞惧嚬濡粌鈹戞幊閸婃鎱ㄩ悜钘夌；婵炴垯鍨洪崐鑸电節闂堟侗鍎忕紒鈧径瀣弿婵☆垰鐏濋悡鎰版煟閹惧鎳囬柡灞剧洴楠炲洭顢涘杈嚄缂傚倷鑳舵慨鎯х暦閻㈢鐒?
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珕闂佽姤锚椤︻喚绱旈弴銏♀拻濞达綀娅ｉ妴濠囨煕閹惧绠為柟顔炬焿椤﹀綊鏌熼姘辩劯妤犵偞顭囩槐鎺懳熺悰鈥充壕闁割煈鍋嗙粻楣冩煙鐎电鍓卞ù鐓庢閺岀喐娼忛崜褏鏆犻梺娲诲幗椤ㄥ﹪鎮￠锕€鐐婇柕濞р偓婵洭姊虹紒妯诲暗闁哥姵鐗犲濠氭晸閻樿尙锛滃┑顔矫ぐ澶愭惞鎼淬劍鈷戦悹鍥ｂ偓铏亶闂佹悶鍔屽鈥愁嚕鐠囨祴妲堟繛鍡樺姇閸斿懘姊洪幐搴ｂ槈閻庢艾鎳撻ˇ褰掓煛瀹€瀣？濞寸媴绠撳畷婊嗩槼闁告帗鐩幃妤€鈻撻崹顔界亶闂佽鐡曞▍鏇熺┍婵犲洤绠瑰ù锝堝€介妸鈺傜厪濠㈣鍨伴懟顖炲疾椤忓牊鈷掑ù锝囩摂閸ゆ瑦鎱ㄥ鍫㈢暤闁挎繄鍋炲鍕箾閹烘挻銇濈€殿喕绮欓、姗€鎮㈤摎鍌氬毐婵犵數濞€濞佳囧磹婵犳埃鈧箓宕奸妷锕€鐝旈梺缁樻煥閹芥粎绮绘ィ鍐╃厵閻庣數顭堥埀顒佸灥椤繄鎹勯崫鍕闂佺懓鍚嬮悾顏堝垂閼姐倗涓嶅Δ锝呭暞閻撴洟鏌嶉埡浣稿箻妞ゅ繐鐗婇崐鍫曟煃閸濆嫬鏆為柛瀣墵閺屻劑寮撮悙娴嬪亾瑜版帒鐓曢柟鐑樺灟閳ь剚甯掗～婵嬫晲閸涱剙顥氬┑鐘垫暩閸嬬姵绔熼崼婵堟殾闁汇垹鐏氶～鏇㈡煟閹邦剦鍤熼柣鐔风秺閺屽秷顧侀柛鎾寸箓铻為柕蹇嬪€栭埛鎴︽偡濞嗗繐顏╃紒鈧崘鈹夸簻闁哄洤妫楅幊鎰▔瀹ュ棎浜滈柡宥庡亜娴狅箓鏌ｉ幘璺烘瀾濞ｅ洤锕、娑橆煥閸涱厾顐奸梻渚€娼уú銊╁窗閺嶎厼钃熼柍鈺佸暞婵挳鏌涘┑鍕壘闁挎繂顦伴悡娆忋€掑顒備虎濠碘€冲悑閵囧嫰顢曢敐鍥╃厜閻庤娲栧畷顒冪亙闂侀€炲苯澧撮柨婵堝仱椤㈡洟鏁愰崶鈺婂晭闂佽娴烽弫鎼佲€﹂崼銏℃殰闁割偅娲橀悡鐔镐繆閵堝嫮鍔嶉悘蹇ｅ幗閵?婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵婵炲棗绻掓禒楣冩⒑缁嬪灝顒㈤柛銊ユ健瀵鏁嶉崟顏呭媰闁荤姴娲﹁ぐ鍐╂叏鎼淬劍鐓曢柕澶涢檮鐏忥附鎱ㄦ繝鍛仩闁告牗鐗犲鎾倷濞村瀚熷┑锛勫亼閸婃垿宕濆畝鍕櫇闁靛牆鐗忔禍閬嶆⒒娴ｈ櫣甯涢柛銊ュ悑閹便劑濡舵径濠勫幈?闂?
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


// WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鐘栄囨煕鐏炲墽鐓瑙勬礀閳规垿顢欓惌顐簻閻ｇ兘顢楅崟顐㈠亶闁诲海鏁哥涵鍫曞磻閹捐埖鍠嗛柛鏇ㄥ墰椤︻喖顪冮妶搴′簻妞わ箓浜堕崺鈧い鎺戝濞懷囨煏閸埄鐒炬い鏇秮瀹曞ジ寮撮悙娈垮悈闂備胶绮敋闁诲繑鑹捐閹兼番鍨荤弧鈧梺姹囧灲濞佳勭閿曞倹鐓欑紒瀣儥閻撹偐鈧娲樺钘夌暦濮椻偓椤㈡瑩鎳栭埡瀣耿闂傚倷鑳堕幊鎾存櫠閻ｅ苯鍨濇い鏍仦閸嬪倹绻涢崱妯哄濞存粍绮撻弻锟犲磼濠靛洨銆婃繛瀵稿О閸ㄥ骞冮纰辨僵妞ゆ垼濮ら弬鈧梻浣虹帛閸旀瑥顭囪閺侇噣宕滄担铏癸紲缂傚倷鐒﹂敃顐︽嚀鐠恒劉鍋撶憴鍕婵＄偘绮欏顐﹀箻缂佹ê浜归梺鑲┾拡閸撱劎妲愰崣澶岀瘈闁汇垽娼ф禒锕傛煕閵娿儳鍩ｉ柟顔惧厴椤㈡盯鎮欓懠缂庢洟姊虹粙璺ㄧ伇闁稿鍋ら崺娑㈠箳濡や胶鍘遍柣蹇曞仜婢т粙骞婇崨顔轰簻闁挎柨銈稿顔剧磼缂佹绠為柛鈹惧亾濡炪倖甯掔€氼參鎮為懖鈹惧亾鐟欏嫭灏柛搴ㄤ憾钘濋柣銏犳憸瀹撲線鏌涢妷顔煎⒒闁轰礁瀚板鍝勨枎閹呬哗婵炲瓨绮撶粻鏍蓟閻斿吋鍊绘慨妤€妫欓悾鍫曟⒑閸︻収鏀伴柛鈺傜墵婵＄敻宕熼姘鳖啋闁诲海鏁哥涵璺何ｉ崼鐔虹閻庢稒顭囬惌鎺旂磼閻樺磭澧い顐㈢箲缁绘繂顫濋鍕暪闂備胶绮瑙勭娴犲鐒垫い鎺嶇濞搭噣鏌″畝瀣М妤犵偞甯￠獮瀣倷閺夋垶鐎鹃梻鍌欑窔閳ь剛鍋涢懟顖涙櫠閸欏浜滄い鎰╁焺濡叉悂鎮￠妶鍡曠箚妞ゆ牗鑹鹃幃鎴犫偓瑙勬礃閻擄繝寮诲☉銏╂晝闁靛牆鎳忛悗铏圭磽娴ｇ瓔鍤欐俊顐ｇ箞瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鈧弻娑㈡倻閸℃浠稿┑顔硷攻濡炰粙鐛幇顓熷劅闁靛浚婢佺槐鈺呮⒒娴ｄ警鐒炬い鎴濇楠炴垿宕堕鈧拑鐔哥箾閹存瑥鐏╃紒鐘差煼閺岀喐锛愭担钘夊帯闂佸憡鑹鹃鍛村煘閹达富鏁婄痪顓炲槻娴兼劙姊洪崷顓涙嫛闁稿妫濆畷锝夊川椤栨粎锛濋梺绋挎湰閻熝囧礉瀹ュ鐓ユ慨妯垮煐閻撳啴鎮归崶顏勭毢缂佺姷绮幈銊︾節閸愨斂浠㈤悗瑙勬磸閸斿秶鎹㈠┑瀣妞ゅ繐瀚Ч鏌ユ⒒閸屾瑧鍔嶉悗绗涘厾娲晜閻ｅ矈娲稿銈呯箰鐎氼噣寮抽敂鐣岀鐎瑰壊鍠曠花濠氭煟閹烘垹浠涢柕鍥у楠炴帒顓兼径瀣碘偓鏍ㄧ箾鐎涙鐭嬬紒顔芥崌瀵鎮㈤悡搴濈炊闂佸憡娲﹂崜锕€螞閻愬绠鹃悗娑欘焽閻帞绱掗悩宕囧ⅹ妞ゎ偄绻橀幖褰掑捶椤撶姷鍘梻浣告啞閻楁垿鍩€椤掆偓閸樻牜妲愰敐鍥╃＝闁稿本鑹鹃埀顒€鎽滅划鏃堝箻椤旇姤娅囧銈呯箰閻楀繐鐣垫笟鈧弻娑㈠箛闂堟稒鐏堥柛鐑嗗灦濮婃椽宕橀崣澶嬪創闂佺锕﹂幊鎾诲煝瀹ュ拋鐓ラ柛顐ゅ暱閹锋椽姊虹涵鍛汗闁稿鐩畷婵嗩潨閳ь剟寮诲☉銏犳閻犳亽鍔庨崝顖氼渻閵堝啫鐏柟鍛婃倐閸┿儲寰勯幇顒夋綂闂佹娊鏁崑鎾绘煙閻ゎ垱顏犵紒杈ㄦ崌瀹曟帒鈻庨幋婵嗩瀴闂備焦瀵уú宥夊疾濞戙垹鐓濋柟鎹愵嚙缁秹鏌涢銈呮灁闁告妫勯埞鎴炲箠闁稿﹥鍔欏畷鎴﹀箻缂佹鍙嗛梺鍝勬储閸斿鏌囬娑辨闁绘劖褰冮弳娆撴懚閺嶎厽鐓曟繝濞惧亾闁绘帪绠撳鏌ュ煛娴ｅ弶鏂€?data\.warp-enabled闂?
//   闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰绠婚柛鈹惧亾濡炪倖甯婇懗鍫曞煝閹剧粯鐓涢柛娑卞枤缁犳﹢鏌涢幒鎾崇瑨闁宠閰ｉ獮妯虹暦閸ヨ泛鏁藉┑鐘茬棄閺夊簱鍋撻幘缁樺€块柨鏇楀亾闁宠绉瑰顒佹償閹惧瓨鏉搁梻浣虹帛閿氶柣蹇斿哺瀵娊鍩℃担鍙夋杸濡炪倖娲栭幊搴☆嚕椤旈敮鍋?  -> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮村Δ鈧禍鎯ь渻閵堝簼绨婚柛鐔告綑閻ｇ柉銇愰幒婵囨櫔闂佸憡渚楅崹鐗堟叏濞差亝鈷掑ù锝勮濞兼帡鏌涢弴鐐典粵闁伙絽澧庣槐鎾存媴閹绘帊澹曞┑鐘灱濞夋稒寰勯崶顒€纾婚柟鎹愬吹瀹撲線鏌涢…鎴濇灈濠殿喖楠搁—鍐Χ韫囨挾妲ｉ梺鎼炲姀濞夋盯顢氶敐鍡楊嚤闁哄鍤﹂妸鈺傜叆闁哄倸鐏濋埛鏃€銇勯弮鈧ú鐔奉潖濞差亝鍋￠梺顓ㄧ畱濞堝爼姊虹粙娆惧剳闁哥姵鐗犳俊鎾磼閻愯尪鎽曢梺闈涱檧婵″洭宕㈤幖浣光拺缂侇垱娲栨晶鍙夈亜椤撶偞鍠橀柟顖氱焸楠炴帒螖娴ｅ搫骞嶆俊鐐€栧褰掑几婵犳碍鍤€闁秆勵殕閻撴瑦銇勯弮鍌涙珪闁瑰啿娲︽穱濠囶敃閵忕姵娈梺瀹犳椤︻垵鐏冮梺閫炲苯澧撮挊鐔告叏濡じ鍚痪?WARP 闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪箻閸┾偓妞ゆ帊鑳堕埢鎾绘煛閸涱垰鈻堥柕鍡曠閳诲酣骞橀崘鎻掓暏婵＄偑鍊栭幐楣冨磻閻斿吋鍋橀柕澶嗘櫆閳锋垿鏌涢敂璇插箹闁告柨顑嗛妵鍕Ω閵夛箑娅ф繝銏ｎ潐濞叉鎹㈠┑鍡╂僵妞ゆ挾濮撮獮宥夋⒒娴ｅ憡鍟為柟绋挎瀹曘劑顢橀悪鈧崑褔姊婚崒娆掑厡缂侇噮鍨堕獮鎰偅閸愩劎鐛ラ梺鍦濠㈡ɑ顢婇梻浣告啞濞诧箓宕归幍顔句笉婵炴垶鐟ｆ禍婊堟煛閸屾氨浠㈤柟顔藉灴閺岋絾鎯旈敐鍥х缂備胶绮粙鎾诲焵椤掍胶鈯曟い顓炴喘閹矂骞掗弮鍌滐紲闂佺粯锚绾绢厽鏅堕鍛簻妞ゆ劑鍨荤粻浼存偂閵堝棎浜滈煫鍥ㄦ尭椤忣偅銇勯弬鎸庡枠闁哄瞼鍠栭幊婵嬫偋閸繃閿紓鍌欑劍瑜板啴骞栭銏犵カ闂備礁澹婇崑渚€宕曢弻銉ョ厱闁瑰濮风壕钘壝归敐鍥仧濞撴碍绋掔换娑㈠醇閻斿摜顦伴梺鍝勭灱閸犳牠骞冮悜鑺ュ亱闁割偆鍠愰崐鐑芥⒒娴ｉ涓茬紒鎻掓健瀹曟顫滈埀顒勬偘椤旇姤鍎熼柕濠忕畱閻у嫭绻濋姀锝嗙【闁活厼鐗撳畷顖炲炊瑜夐弨浠嬫煟閹邦垰鐓愮憸鎶婂懐纾奸柡灞诲劤閻ｆ椽鏌熼鑺ャ仢鐎殿喗鎸抽崺銉╁幢濡崵缈诲┑鐘垫暩婵兘銆傞鐐潟闁哄洢鍨圭壕濠氭煙鏉堝墽鐣辩痪鎯х秺閺屸€愁吋鎼粹€崇闂佺顑呴鍡樼┍婵犲洤围闁告侗鍘藉▓顓㈡⒑閸濆嫷鍎忛柣妤€绻橀崺鈧い鎺嗗亾缂佺姴绉瑰畷鏇㈡焼瀹撱儱娲、娑橆煥閸愮偓顥￠柣鐔哥矌婢ф鏁Δ鍛厱闁圭儤顨嗛悡鏇熴亜閹扳晛鈧洟寮稿▎鎾寸厱閹兼番鍨规慨宥嗩殽閻愬澧懣鎰亜閹哄棗浜炬繝纰樷偓鐐藉仮闁哄矉缍侀獮鍥敆婵犲懎鎯堥梻浣告惈閺堫剙煤濡吋宕叉繛鎴炵矤濡插姊虹涵鍛彧闁告梹鐟ラ锝夊箮缁涘鏅濋梺鎸庢琚欓柟閿嬫そ閺岋綁鎮╅崣澶婎槱闂佺粯鎼换婵嬨€侀幘婢勬棃宕ㄩ鎯у箺闂傚鍋勫ú锕€螞娴ｅ湱鐝舵慨妞诲亾闁硅棄鐖奸幃娆撴倻濡厧骞堥梻渚€鈧稑宓嗘繛浣冲洤鍑犳繛鎴欏灪閻?
//   濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍焦鍎撻梺鎼炲妼閸婂潡寮诲☉銏╂晝闁挎繂妫涢ˇ銉х磽娴ｅ搫小闁告濞婂璇测槈濡攱鏂€闂佸憡娲﹂崑鍕叏閵忋倖鈷戞繛鑼额嚙楠炴鏌熼悷鐗堝枠鐎殿喖顭烽幃銏ゆ惞閸︻叏绱查梻渚€娼х换鎺撴叏閻㈠憡鍊剁€广儱顦伴埛鎴犵磼鐎ｎ偒鍎ラ柛搴＄Ч閺屾稒绻濋崘顏嗙杽閻庢鍠栭…宄邦嚕閹绢喗鍋勯柧蹇氼嚃閸熷酣姊绘担铏瑰笡闁告棑绠撳畷婊冾潩閼搁潧浠ч梺鍝勫暙閻楀﹪鍩?-> 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣銏犲閺佸﹪鏌″搴″箹缂佹劖顨嗘穱濠囧Χ閸涱厽娈查梺鍝勬缁秵绌辨繝鍥ч柛娑卞幗濞堝爼姊哄ú璇插箺闁荤啿鏅犲濠氭偄閸忕厧鈧粯淇婇鐐存珳缂侇喖鐖煎娲焻閻愯尪瀚板褍澧界槐鎺楁偐閾忣偄纰嶉梺浼欑秮閺€杈╃紦閻ｅ瞼鐭欓悹鍥﹀嫎閸斿秹濡甸崟顖氱睄闁搞儺鐏濋幘瀵哥闁告侗鍘惧瓭闂佸疇顫夐崹鍧楀箖閳哄懎鍨傛い鎰剁稻閻﹀骸鈹戦悩鎰佸晱闁哥姵宀稿顐ｇ節濮橆剛鐤囧┑鐘绘涧椤戝懐绮堥崼銏″枑闊洦绋掗崕妤呮煕閳╁啰鈯曢柣鎾存礈閳ь剙鍘滈崑鎾绘煃瑜滈崜鐔风暦閹达附鍊烽柣鎴灻禍妤€鈹戦悙鍙夘棡闁圭顭烽幃鈥斥枎閹扳晙绨婚梺鍝勭Р閸斿酣鍩婇弴鐘电＜闁诡垱婢橀惃娲煙椤曞懎鏋涙い顐ｇ箞閹虫粎鍠婂Ο璇差伖闂傚倷绀佸﹢閬嶁€﹂崼婢濆綊鎮滈挊澶屽帎闂佸搫娲ㄩ崰宥囨崲閸℃ǜ浜滈柟鎵虫櫅閻忊晠鏌￠崱顓犳偧闁逞屽墯椤旀牠宕伴弽顓涒偓锕傛煥鐎ｂ晝绠氶梺褰掓？缁€浣告暜闂備線娼х换鍡椢ｉ崨顓х劷闁哄啠鍋撶紒缁樼箘閸犲﹤螣濞茬粯缍夐梻浣规偠閸斿秵绻涙繝鍌滄殾闁靛骏缍嗗Ο鍕⒑闂堟稒顥滈柛濠傤煼楠炲骞橀鑲╊槹濡炪倕绻愬Λ娑㈡偩妤ｅ啯鈷掑〒姘ｅ亾婵炰匠鍥ㄥ亱闁糕剝顦哄ú顏勎ч柛娑卞灡閹冲啯绻濋悽闈浶ラ柡浣规倐瀹曟垵鈽夊Ο婊呭枑缁绘繈宕惰閸嬪秵绻濋棃娑樷偓鎼佸箟閿熺姴纾婚柍鍝勬噺閻撶喖鏌熺€电啸闁硅棄鍊块弻鈩冩媴缁嬫寧娈婚梺鍝勮閸斿矂鍩為幋锕€骞㈡俊顖濇閻涒晠姊绘担渚敯婵炲懏娲滈幑銏ゅ醇濠垫劖缍庡┑鐐叉▕娴滄繈鎮炴繝姘厽闁归偊鍨伴拕濂告倵濮橆厽绶叉い顓″劵椤﹂亶鏌涘Δ浣糕枙鐎殿喛顕ч埥澶愬閻樻鍞洪梻浣筋潐瀹曟ê鈻嶉弴鐘冲珰闁绘劕妯婂〒濠氭煏閸繂鏆欓柣蹇ｄ簽缁辨帞鈧綆鍋勯悘鎾煃閵夘垳鐣靛┑鈩冩倐閸┾剝鎷呮笟顖浶у┑锛勫亼閸婃牠宕濋敃鍌氱；闁逞屽墴閺屾稓鈧綆鍋呯亸顓㈡煃閽樺妯€濠殿喒鍋撻梺鎶芥暜閸嬫捇鏌熼柨瀣仢婵﹤顭峰畷鎺戭潩閸楃儐鏉哥紓鍌欑椤戝棝骞愭繝姘柧闁割偅娲栭崡鎶芥煟閺冨洦顏犳い鏃€娲熼弻锝堢疀閺囩偘绮堕梺绋匡攻閹倿宕洪埀顒併亜閹哄秶璐版繛鍫熺矒閺岀喖顢欓悙顒傚姽濡炪倧绠掗～澶愬箞閵婏妇绡€闁告洦鍘肩粭锟犳⒑閻熸澘妲婚柟铏姉閸掓帡鎮界喊妯轰壕闁挎繂绨肩花濂告煟閿濆嫬宓嗘慨濠勭帛閹峰懏绗熼婊冨Ъ缂傚倷鑳剁划顖炲垂閸︻厼鍨濇繛鍡樻尭缁犲鏌￠崒妯哄姕闁诲骸顭峰娲濞戞氨鐤勯梺绋匡攻濞叉牕煤閸ф鈷掑ù锝堫潐閸嬬娀鏌涙惔銏°仢闁诡噯绻濆畷鐓庘攽閹邦喖濮洪梻浣虹帛閺屻劑宕ョ€ｎ喖鐓曢柟鐑樺灟閳ь剚甯掗～婵嬵敆娴ｈ鍊峰┑鐘媰鐏炶姤鐏撶紓浣介哺鐢顕ラ崟顒傜瘈閹肩补鍓濋宥夋⒒娴ｇ儤鍤€鐎殿喖鐖奸獮鎰板箮閽樺鐎梺绋挎湰缁嬫帡宕ｈ箛娑欑厵闂侇叏缂氱花浠嬫煟閳轰線鍙勬慨濠勭帛閹峰懘宕ㄦ繝鍐ㄥ壍缂傚倷绀侀ˇ浼存偂閳ュ磭鏆︽繝闈涳功閻も偓闂佸搫鍟崐鏄忋亹閹€鏀介柍钘夋閻忕娀鏌涘顒夊剶鐎规洘鍨垮浠嬵敇閻斿搫骞楅梻浣告惈閸婃悂鎮樺┑瀣畺闁硅揪闄勯悡鍐⒑閸噮鍎忛柣蹇旀尦閺岋紕浠﹂崜褉妲堥梺瀹狀潐閸ㄥ灝鐣烽崡鐐嶆梹绻濇担鐟颁簼闂傚倸鍊风粈渚€骞栭鈶芥稑鈽夐姀鐘碉紱闂侀潧鐗嗛ˇ顖炴嫅閻斿摜绠鹃柟瀵稿仧閹冲嫰鏌涙惔锝呮灈闁哄本娲熷畷鐓庘攽閸パ勵仧婵犳鍠涢～澶愩€冩繝鍌ゆ綎婵炲樊浜濋悞濠氭煟閹邦垰钄奸悗姘緲椤儻顧侀柛銊ョ埣瀵鈽夊Ο閿嬫杸闂佺硶鍓濈粙鎴濐嚕娴煎瓨鍊甸悷娆忓缁€鈧紓鍌氱Т閿曨亪鐛幇顓犵瘈闁告劑鍔庣粣鐐烘煟韫囨洖浠ч柛瀣崌椤㈡瑩寮撮姀鈾€鎷绘繛杈剧悼閹虫捇顢氬鍛＜閻犲洦褰冮埀顒冨劵濡喎顪冮妶鍡欏缂侇喖瀛╃粙澶婎吋婢跺鍙嗗┑鐘绘涧濡厼危瑜版帗鐓熼柟鎹愭硾閺嬫垵菐閸パ嶈含妞ゃ垺绋戦～婵嬪矗濡寧顎楅梻鍌欑劍閻綊宕濆鍜佹富濞寸姴顑呴拑鐔兼煥濠靛棭妲搁崶鎾⒑缁嬫寧婀伴柣顓у枟缁?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涙川缁♀偓闂佺鏈划宀勩€傞悽鍛娾拺闂侇偆鍋涢懟顖涙櫠閸撗呯＜闁绘娅曠亸顓㈡煟閿濆鏁辩紒铏规櫕缁瑥鈻庨悙顒傜П闂傚倷鑳堕崑銊╁磿闁秴鐤悹鎭掑姀婵娊鏌曟径娑橆洭缂佲檧鍋撻梻浣圭湽閸ㄨ棄顭囪缁傛帡鏁冮崒娑氬幈濡炪値鍘介崹鐢告倶閿旈敮鍋撶憴鍕闁硅櫕鎸鹃崣鍛渻閵堝懐绠伴悗姘煎櫍閺佸秹鎮㈤崗灏栨嫼闂佽崵鍠愬姗€鍩涢弮鍌滅＜妞ゆ洖鎳庨悘锕傛煙椤曞棛绡€鐎规洘锕㈤、鏃堝川椤撶喓鈧偓绻濋悽闈浶㈤柨鏇樺€濋幃褔宕卞▎鎴犵劶闂佸壊鍋嗛崰鎾剁不妤ｅ啯鐓欓悗鐢殿焾娴犙囨煙閸愬弶鍣洪柕鍥у閸╋繝宕掑鍐ㄧ哗闂備礁鎼張顒傜矙閹达箑鐓″璺侯煬濞笺劑鏌涢銈呮瀾闁哄棙澹嗙槐鎾诲磼濮橆兘鍋撻崫銉х煋闁哄鍤氬ú顏嶆晣闁靛繒濮鹃幗鏇㈡⒑閸濆嫭宸濋柛鐘虫尵瀵囧焵椤掑嫭顥婃い鎰╁灪婢跺嫰鏌熺亸鏍ㄦ珔閻撱倖銇勯幘鍗炵仾闁抽攱鍨块弻娑樷槈濮楀牊鏁鹃梺鎶芥敱鐢繝寮诲☉婊呯杸闁哄啯鎹侀埀顒佸椤ㄣ儵鎮欓幓鎺撴闁剧粯鐗犻弻宥堫檨闁告挾鍠栧畷娲閵堝懐顔掗柣鐘叉处瑜板啴鎮楅鐑嗘富闁靛牆妫欓埛鎺楁煛閸滀礁浜濈紒鍌氱Ч楠炲洭鎮ч崼姘濠电偠鎻徊鑲╂媰閿曞倹鍊块柣鎰靛厵娴滄粓鏌熺€涙绠栨い銉ｅ灪椤ㄣ儵鎮欓弶鎴犱紝閻庤娲栭悥濂搞€佸Δ鍛劦妞ゆ帒瀚€?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮村Δ鈧禍鎯ь渻閵堝簼绨婚柛鐔告綑閻ｇ柉銇愰幒婵囨櫔闂佸憡渚楅崹鐗堟叏濞差亝鈷掑ù锝勮濞兼帡鏌涢弴鐐典粵闁伙絽澧庣槐鎾存媴閹绘帊澹曞┑鐘灱濞夋稒寰勯崶顒€纾婚柟鎹愬吹瀹撲線鏌涢…鎴濇灈濠殿喖楠搁—鍐Χ韫囨洜绐楅梺鍛婎殔閸熷潡鎮鹃悜钘夌闁绘劘灏欑粣鐐烘⒑瑜版帒浜伴柛姗€绠栧畷锝夊幢濡炵粯鏂€闂佺粯顭囩划顖氣槈瑜庢穱濠囶敃閿濆洦鍣悗鍨緲鐎氭澘鐣烽悢纰辨晝闁绘柨鎲￠弶鍛婁繆閻愵亜鈧牠鎮уΔ鍛紶閹兼番鍔岀壕濠氭煏韫囧鈧牠鎮″☉姘ｅ亾閸忓浜鹃柣搴秵閸撴瑩宕哄畝鍕叄缂備焦顭囨晶鐢告煛鐏炵晫啸妞ぱ傜窔閺屾盯骞樼捄鐑樼亪濡ょ姷鍋涢崯鎶剿囧畷鍥╃＜缂備焦顭囩粻鐐碘偓瑙勬礀閻栧吋淇婇幖浣肝ㄩ柕蹇曞С婢规洟姊洪崜鑼帥闁稿鐒︾粋宥咁煥閸喓鍘搁梺鍛婂姧缁茶姤绂嶉崜褏纾藉ù锝囨焿閸忓本銇勯姀鐙呰含濠碉紕鏁婚獮鍥级鐠侯煈鍞洪梻浣筋潐閸庢娊宕㈤弽顬兾旈崨顔规嫽婵炶揪绲介幉锟犲疮閻愮儤鐓曢柡鍐ｅ亾闁绘鍋ゆ俊鐢稿箛閺夎法顦ㄩ梺鍐叉惈閸婄顕ｉ崸妤佲拺鐟滅増甯楅敍鐔兼煟閹虹偟鐣甸柛鈺傜洴楠炲鏁傜憴锝嗗婵＄偑鍊栭崝鎴﹀磹閺囩喓顩锋繝濠傜墛閻撶姵绻涢懠棰濆殭闁诲骏绻濋弻锟犲川椤撶姴鐓熷銈冨灪閻熝勭閹间礁鍐€闁靛闄勫鎴︽⒒閸屾瑨鍏屾い顓炵墦瀵敻顢楅崒婊呯厯闂佸湱鍎ら〃鍛存偂閺囥垺鐓欓柣鎴炆戦埛鎺旂棯閹冩倯闁靛洤瀚板鎾倷閺夋垶顏犳俊銈囧Х閸嬬偤銆冮崼銉ョ劦妞ゆ巻鍋撻柣蹇旇壘椤灝螣鐏忔牕浜炬慨姗嗗亜瀹撳棝鏌ｅ☉鍗炴珝鐎规洖鐖奸、妤佸緞鐎ｎ偅鐝滈梻鍌欑窔閳ь剛鍋涢懟顖涙櫠椤斿浜滈柡鍥朵簽閸掍即鏌ｉ敐蹇曠瘈闁哄苯娲弫鍌炴寠婢跺本袙闂傚倸鍊风欢姘焽瑜旈幃褔宕卞銏＄☉閳藉鈻庨幇顓犲娇婵＄偑鍊栫敮鎺楀磹閼姐倕顥氬┑鍌氭啞閸嬶綁鏌涢妷顔荤盎闁汇劍妞藉顐﹀醇閵夛箑鈧?JSON闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф闂佸憡鎸嗛崨顔筋啅闂備浇顕х€涒晠鎳濇ィ鍏洦瀵奸弶鎳筹箓鏌涢弴銊ョ仩缂佺姵濞婇弻娑㈠焺閸愶缚娌繝銏ｎ潐濞茬喎顫忓ú顏勫窛濠电姴瀛╅悿浣圭箾鐎涙鐭婇柣鏍帶椤曪絿鎷犲ù瀣潔濠碘槅鍨堕弨閬嶏綖瀹ュ應鏀芥い鏃傜摂濞堟棃鏌ｅΔ鈧Λ娆撳箞閵娿儮妲堥柕蹇婃閹锋椽鎮楃憴鍕婵炲眰鍔庨弫顔尖槈濞嗗海绠氬銈嗗姂閸ㄥ綊寮冲▎鎰╀簻闁哄浂浜炵粔顔锯偓瑙勬礀閵堢顕ｉ幘顔藉亜閻忓繋绀佹禍楣冩煟閹达絽袚闁绘挻绋戦…璺ㄦ崉娓氼垰鍓伴梺缁樺笧閸嬫挻绌辨繝鍥舵晝闁靛繆妲呭Λ锕€鈹戦纭锋敾婵＄偠妫勯悾鐑筋敃閿曗偓鍞梺闈涱檧婵″洭鍩€椤掆偓椤嘲顫忓ú顏勭閹兼番鍨归ˇ鈺傜箾鐎涙鐭ゅù婊勭矒椤?.autostart-off 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳娼￠弻鐔衡偓鐢殿焾瀛濈紓浣界堪閸婃繈寮婚敃鈧灒濞撴凹鍨遍敍鍡椻攽閻愬弶鈻曞ù婊勭箞瀵彃顭ㄩ崼鐔哄幗闂侀€涘嵆濞佳勬櫠椤曗偓楠炴牠寮堕幋顖濆惈濠殿喖锕ュ钘夌暦閵婏妇绡€闁稿本顕撮弴鐔虹閻庢稒顭囬惌瀣煟濡ゅ啫孝闁伙絿鍏橀獮瀣晝閳ь剛绮绘繝姘仯闁搞儯鍔岀徊濠氭煟鎼搭喖骞栨い顏勫暣婵″爼宕卞Δ鈧〖缂傚倸鍊哥粔鏉懳涘Δ鍕罕闂備礁鎲￠崝锕傚窗濡ゅ懎纾归柣銏犳啞閸嬧剝绻涢崱妤冪妞ゅ浚浜為惀顏堟倷椤掍礁鈧劙鏌＄仦鍓р姇闁诡垱妫冩俊鍫曞幢濞嗗繑閿紓鍌氬€风粈渚€宕愰崷顓熸殰闁跨喓濮撮弸渚€鏌涢幇闈涙灈缁炬儳鍚嬬换娑㈠箣閻愬棙鍨块幃鐐鐎涙ǚ鎷绘繛杈剧悼椤牓寮抽柆宥嗙厵缁炬澘宕禍浼存煛娴ｇ鏆ｉ柡浣瑰姍瀹曞爼鈥﹂幋鐐电◥濠电姷顣藉Σ鍛村垂閻㈢纾婚柟閭﹀枛椤ユ艾霉閻樺樊鍎愰柣鎾存礋閹﹢鎮欓幓鎺嗘寖闂佺懓鍟块幊蹇涘Φ閸曨垱鏅滃┑鍌氭啞椤庡秹姊洪棃娑欐悙閻庢矮鍗冲顐﹀磼濠婂嫬鐝伴梺鑲┾拡閸忔盯宕犻弽顐ょ＝闁稿本鐟х拹浼存煕閻樻剚娈旀い顐㈢箲缁绘繂顫濋鍌氬Е婵＄偑鍊栫敮濠囨嚄閸洖鐓€闁哄洨鍠嗘禍婊勩亜閹捐泛浠﹂柛鐘筹耿閺屾洟宕卞Δ浣告畻闂佸搫琚崝搴ㄥ焵椤掑﹦绉甸柛鎾寸洴椤㈡瑩寮撮姀鈾€鎷洪梺鑽ゅ枛閸嬪﹪宕甸悢鍏肩厱濠电姴鍟悘銉︺亜椤愶絿绠橀柟鐟板瀹曨偊宕熼锝庡悋闂傚倸鍊风粈渚€骞栭銈囩煓濞撴埃鍋撶€规洏鍨虹粋鎺斺偓锝庡亜娴滄鈹戦悙鏉戠仧闁搞劍妞介幃锟犲即閵忥紕鍘繝銏ｅ煐缁嬫捇宕氶弶娆剧唵鐟滃秹骞夐敓鐘茬疄闁靛鍎欓悢鐓庣閹艰揪绲婚埀顒佸姉缁?
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

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煠绾板崬澧绘俊鏌ヤ憾閺岋綁鎮㈤崫銉х厒婵犫拃鍕垫畼闁轰緡鍣ｉ獮鎺懳旈埀顒勫垂閸岀偞鐓曠€光偓閳ь剟宕戦悙鐑樺亗闁绘棃鏅茬换鍡涙煏閸繂鈧憡绂嶆ィ鍐┾拺缂備焦锕╁▓鏃堟煥閺囥劋绨婚柣锝囧厴閺佹劙宕奸姀銏℃緫婵犵數鍋為崹鍫曟偡瑜斿畷銏ゅ箻缂佹ǚ鎷洪梺鍛婄☉閿曪妇绮婚幘缁樺€垫慨妯煎帶瀵喚鈧娲樻繛濠傜暦閻戠瓔鏁囬柣妯碱暜缁卞弶淇婇悙顏勨偓鏍礉閹达箑鍨傜紓浣股戝▍蹇涙⒒閸屾瑧顦﹂柟纰卞亞閹噣顢曢敃鈧壕瑙勭節闂堟侗鍎戠€规挷绶氶弻鈥愁吋閸愩劌顬夋繝娈垮灡閹告娊骞冨畡鎵虫瀻闊洦鎼╂导鍐⒑鏉炴壆璐板ù婊庝簻椤繒绱掑Ο璇差€撻梺鑽ゅ枑婢瑰寮稿▎鎾粹拺閻炴稈鈧厖澹曢梻浣告啞椤ㄥ牓宕愰敐鍛闁靛繒濮烽ˇ銊ヮ渻閵堝棙顥嗛柛瀣姍閹?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣銏犲閺佸﹪鏌″搴″箹缂佹劖顨嗘穱濠囧Χ閸涱厽娈查梺鍝勬缁秵绌辨繝鍥ч柛娑卞幗濞堝爼姊哄ú璇插箺闁荤啿鏅犲濠氭偄閸忕厧鈧粯淇婇鐐存珳缂侇喖鐖煎娲焻閻愯尪瀚板褍澧界槐鎺楁偐閾忣偄纰嶉梺浼欑秮閺€杈╃紦閻ｅ瞼鐭欓悹鍥﹀嫎閸斿秹濡甸崟顖氱睄闁搞儺鐏濋幘瀵哥闁告侗鍘惧瓭闂佸疇顫夐崹鍧楀箖閳哄懎鍨傛い鎰剁稻閻﹀骸鈹戦悩鎰佸晱闁哥姵宀稿顐ｇ節濮橆剛鐤囧┑鐘绘涧椤戝懐绮堥崼銏″枑闊洦绋掗崕妤呮煕閳╁啰鈯曢柣鎾存礈閳ь剙鍘滈崑鎾绘煃瑜滈崜鐔风暦閹达附鍊烽柣鎴灻禍妤€鈹戦悙鍙夘棡闁圭顭烽幃鈥斥枎閹扳晙绨婚梺鍝勭Р閸斿酣鍩婇弴鐘电＜闁诡垱婢橀惃娲煙椤曞懎鏋涙い顐ｇ箞閹虫粎鍠婂Ο璇差伖?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崗鐓庘偓缁樹繆椤栨繃顏犻柛鏃傤焾閳规垿鎮欓懠顒佺檨闂佸搫鎳愭繛鈧€殿喖顭峰鎾偄閾忚鍟庨梻浣虹帛閸旓箓宕滃鑸靛仧闁哄啫鐗婇埛鎴︽煕濠靛棗顏撮柛搴℃捣缁辨帡鎳滈棃娑樻懙閻庢鍠栭…宄邦嚕閹绢喖顫呴柣妯款嚙閺佽绻濋悽闈浶㈤柣蹇斿哺瀹曟繈寮介銈嗗櫘缂傚倸鍊搁崐鎼佸磹閻戣姤鍤勯柤鎼佹涧閸ㄦ棃鎮楅棃娑欐喐缂佲偓婵犲伅褰掓晲閸涱収妫屽┑鐐插悑閻楁鎹㈠☉姗嗗晠妞ゆ棁宕甸惄搴ㄦ⒑閹肩偛濡奸柛濠傜秺婵＄敻宕熼姘祮闂佺粯鍔栧娆忊枍閿濆鈷戦柛娑橈攻鐏忣參鏌涢悢閿嬪仴闁糕晝鍋ら獮瀣晜缂佹ɑ娅撶紓浣鸿檸閸橀箖寮绘径鎰Е妞ゆ劑鍊楃壕钘壝归敐鍛儓閺嶏繝姊虹紒姗嗘畼濠殿喗鎸抽、姘舵晲閸ャ劌鐝板┑鐐存綑椤戝棝锝炲鍛斀妞ゆ梻鐡斿▓鏃€淇婇锝囨噰鐎规洘鎹囬幃浠嬪川婵炵偓瀚肩紓鍌氬€烽悞锕佹懌闁诲繐娴氶崢鐣屾崲濞戙垹绠绘い鏃囧Г閺嗙姷绱撴担绋库偓鍦暜閿熺姰鈧礁鈽夐姀鈥斥偓椋庣磼椤旀娼愰悗姘偢濮婃椽宕崟鍨ㄩ梺绋款儐閿曘垹鐣烽敓鐘茬鐎瑰壊鍠氶埀顒€鐖奸弻锝夊箣閿濆憛鎾绘煟閹惧瓨绀€闁宠鍨块幃鈺呭箵閹烘挻顔嶅┑鐘媰鐏炶偐鍑归梺闈涙搐鐎氫即銆佸鈧幃娆撳级閹搭厽袩缂傚倸鍊烽懗鍓佸垝椤栨粍宕查柛宀€鍎戠紞鏍ㄧ節闂堟侗鍎涢柡浣告閺屾稓浠﹂幆褏鍔伴梺绋款儐閹瑰洭骞冨▎鎾村€绘俊顖炴敱鐎氬ジ姊婚崒姘偓鎼佹偋婵犲啰鐟规俊銈傚亾閸楅亶鏌涢锝嗙闁绘挻娲熼弻鏇熺節鎼达絽甯ュ┑鐐插悑閸旀妲愰幒妤€纾兼慨妯荤樂瑜庨〃銉╂倷閸欏妫﹂梺鐐藉劜閻楃娀宕归幆褏鏆﹂柛銉㈡暕閵娾晜鈷戦弶鐐村閸斿秹鏌涢弮鈧悧鏇㈩敋閵夆晛绀嬫い鎰儑閸犲海鍙呯紓鍌欑劍閿曗晛螞濠靛牏纾?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涙川缁♀偓闂佺鏈划宀勩€傞悽鍛娾拺闂侇偆鍋涢懟顖涙櫠閸撗呯＜闁绘娅曠亸顓㈡煟閿濆鏁辩紒铏规櫕缁瑥鈻庨悙顒傜П闂傚倷鑳堕崑銊╁磿闁秴鐤悹鎭掑姀婵娊鏌曟径娑橆洭缂佲檧鍋撻梻浣圭湽閸ㄨ棄顭囪缁傛帡鏁冮崒娑氬幈濡炪値鍘介崹鐢告倶閿旈敮鍋撶憴鍕闁硅櫕鎸鹃崣鍛渻閵堝懐绠伴悗姘煎櫍閺佸秹鎮㈤崗灏栨嫼闂佽崵鍠愬姗€鍩涢弮鍌滅＜妞ゆ洖鎳庨悘锕傛煙?-> 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珖闂侀€炲苯澧板瑙勬礃缁绘繂顫濋鐘插箞闂備焦鏋奸弲娑㈠疮娴兼潙鐓″鑸靛姈閻撴洟鎮橀悙棰濆殭濠殿喖鍊块弻锛勪沪鐠囨祴鍋撻弴銏＄畳闂佽鍑界紞鍡涘磻閸涘瓨鍋熼柡鍥╁剱濞撳鏌曢崼婵嬵€楀ù婊勭箖缁绘盯鎳犻鈧弸娑氣偓娈垮枛椤兘骞冮姀銈呭窛濠电姴瀚倴闂傚倷绀侀幉锟犲箰閸℃稑宸濇い鏃傜摂閸熷懘姊?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煕閹邦垰绱﹂柣銏㈢帛娣囧﹪鎮欓鍕ㄥ亾閺嶎厽鍋嬫俊銈呭暟閻瑩鏌熼悙顒傛菇闁逞屽墮閹虫ê鐣烽敐鍡楃窞鐎光偓閳ь剟鎯侀崼鐔虹瘈闁汇垽娼у瓭闂佺锕ラ幃鍌氼嚕閹间焦鏅濋柛灞剧〒閸橀箖姊虹紒姗嗘當闁绘妫濋、姘煥閸喓鍘梺鎼炲劀閸愬彞绱撶紓鍌欒兌婵敻鎯勯鐐靛祦婵☆垰鍚嬪畷澶娿€掑鐓庣仭婵絽娲缁樻媴閽樺鎯為梺鍝ュУ閸旀瑩骞冮悜鑺ョ劶鐎广儱妫岄幐鍐⒑閸涘﹥澶勯柛銊у椤㈠﹪姊绘担鍛婂暈婵炶绠撳畷瑙勫閺夋垹顦ㄩ梺閫炲苯澧存慨濠冩そ濡啫鈽夋潏鈺佸Ъ闂備礁鎲￠弻銊╂儗閸屾繃锛傛繝鐢靛█濞佳囶敄閸℃稑纾婚柣妯挎珪閸欏繑淇婇姘儓闁肩缍婇弻锛勨偓锝庡亞閳洟鏌熸笟鍨缂佺粯绻堝畷姗€鍩炴径姝屾闂佽姘﹂～澶娒洪敃鍌氱；濠电姴鍊婚弳锕傛煟閺冨倵鎷￠柡浣稿暣閺屻劌鈹戦崱姗堢礊闂佸摜鍠庨悧鍡涘煘閹达附鍋愰柛顭戝亝濮ｅ嫰姊虹粙娆惧剱闁烩晩鍨靛Λ鐔兼⒑鐟欏嫷鍟忛柛鐘冲哺閹苯鈻庨幘瀵稿幍闂佺粯鍨惰摫闁抽攱甯￠弻锝夘敇閻戝棙鈻堥梺鍝勭灱閸犳牠骞冨鍏剧喖鎼归柅娑欏枤濠电姷顣介崜婵娿亹閸愵喗鍋嬪┑鐘插瀹曞弶绻涢幋娆忕仼妤犵偑鍨烘穱濠囶敍濠靛浂浠╂繛瀛樼矊閻栫厧顫忓ú顏勪紶闁告洟娼ч崜閬嶆⒑缂佹﹩娈樺┑顔芥尦閿濈偠绠涢幘浣规そ椤㈡棃宕ㄩ姘ラ梻鍌欒兌缁垶宕濆Ο闂寸剨妞ゅ繐妫岄崑鎾愁潩椤撶喓鍑″銈庡弨閸庡篓娓氣偓閺屾盯鏁愯箛鏇犘滈悗娈垮枦椤曆囶敇婵傜鐐婇柕濞垮劤瑜版挳姊绘担鍛婃儓閻炴凹鍋婂畷鏇㈠础閻戝棗娈ㄦ繛瀵稿帶閻°劑鍩涢幋鐘电＜妞ゆ牗绋掔粈鍐煛婢跺銇濋柡灞剧⊕閹棃濡搁敂浠嬫暘闂備椒绱徊鍧楀礂濡绻嗛柟闂寸閻撴稑鈹戦悩鎻掓殲婵炲懏鐗楃换婵嬫偨闂堟刀銏ゆ煥閺囨ê鈧鍩€椤掍胶鈻撻柡鍛洴瀵娊鍩￠崒婊呯槇闂佹眹鍨藉褍鏆╅梻浣芥〃閻掞箓骞冮崒姘辨殾闁硅揪闄勯崐鐑芥煕濠靛棗顏╅柣搴弮濮婃椽宕滈幓鎺嶇凹濠电偛寮堕悧婊呭垝缂佹ê顕遍悗娑欘焽閸樺崬鈹戦悩缁樻锭閻庢凹鍓熷畷婵嬪Χ婢跺鍘藉銈嗘尵閸犳劕鐣峰畝鍕厸鐎光偓鐎ｎ剛袦婵犳鍠掗崑鎾绘⒑闂堟稓绠氶柡鍛箞瀹曟繄绮欐惔鎾存杸闂佸疇妫勫Λ妤佺濠靛鐓ユ慨妯垮煐椤ュ棗顭块懜闈涘闁绘挶鍎茬换婵嬫濞戞瑯妫ゆ繝鈷€鍡涘摵缂佺粯鐩畷妤呮偂鎼粹槅娼撻梻鍌氭搐椤︽壆鎹㈠┑濠庢Щ闂佽绻戠换鍫ュ春閳ь剚銇勯幒宥堝厡濠⒀囦憾閺岀喖鐛崹顔句患闂佸疇妫勯ˇ鍨叏閳ь剟鏌ｅΟ娲诲晱闁告艾鎳樺缁樻媴閾忕懓绗￠梺鎼炲姂濞佳嗙亱濠电偞鍨崹鍦矆婢舵劖鐓熼柟杈剧到琚氶梺鎶芥敱閸ㄥ潡寮诲☉妯锋斀闁糕剝顨忔禒鎯ь渻閵堝啫鍔滈柣鐔叉櫊瀵鈽夊Ο婊勬⒐閹峰懐鍖栭弴鐔轰簽闂佽瀛╅鏍窗閺嶎厸鈧箓鎮滈挊澶嬬€梺鐟板⒔缁垶宕戦幇鐗堢厾缁炬澘宕晶缁樹繆閼碱剙顣煎ǎ鍥э躬閹瑩顢旈崟銊ヤ壕闁哄稁鍘肩粻鏉课旈敐鍛殭缂佹劖顨婇弻鐔煎箲閹伴潧娈弶鈺傜箞濮婇缚銇愰幒鎴滃枈闂佺绻戦敃銏狀嚕閵婏妇顩烽悗锝庡亞閸樹粙姊鸿ぐ鎺戜喊闁告挻鐟ч惀顏囶樄闁哄本鐩弫鎰板幢濡晲铏庨梻浣筋嚃閸犳洜鍒掑▎鎾扁偓浣糕槈閵忕姈褔鏌涢妷銏℃珖婵℃彃娲弻锝嗘償閵忊晛鏅遍梺闈╃秵閸ｏ絽鐣烽幋锕€绀嬫い鏍电稻閺咃絾绻濋棃娑樷偓濠氣€﹂崼婵愬晠婵犻潧娲㈡禍婊堟煛瀹ュ啫濡块柍顖涙礋閺岋綁鏁愰崶褍骞嬪┑顔硷功缁垶骞忛崨鏉戝窛濠电姴鍊瑰▓妯荤節閻㈤潧浠╂い鏇熺矒濮婁粙宕熼鐘电効閻庡箍鍎遍ˇ浼村磹閻戣姤鐓曟い顓熷灥娴滆姤绻涢懖鈺佹瀻闁宠鍨块、娆戠驳鐎ｎ剙濮奸梻浣虹《閺呮粓鎮ч悩璇叉槬婵炴垯鍨圭粻锝夋煟濡吋鏆╅柨娑欑矒閺岋絾鎯旈婊呅ｉ梺鍛婎焽閺咁偄鈽夐悽绋跨濞达綀娅ｉ敍婊堟⒑缂佹◤顏堝疮閸ф澶屸偓锝庡墰绾惧ジ寮堕崼娑樺缂佹姘ㄩ埀顒€鐏氬妯尖偓姘煎櫍閸┾偓妞ゆ帒锕︾粔鐢告煕鐎ｎ亜顏€规洦鍓氬鍕箾閵忊剝顥堥柟顔规櫊濡啫鈽夊Δ鍐╁礋缂傚倸鍊烽懗鍓佸垝椤栫偛鍨傜憸鐗堝笒閻掑灚銇勯幋锝嗙《缂佷胶鍏橀弻娑氣偓锝庡亜婵秹鏌涢埞鎯т壕婵＄偑鍊栫敮濠囨嚄閸洖鐓€闁哄洢鍨洪悡銉︽叏濮楀棗鍔掔憸鐗堝笚閸嬫劙鏌ц箛锝呬簴濞寸姰鍨归埞鎴︻敊绾攱鏁惧┑锛勫仩濡嫯鐏嬮梺鍛婂姂閸斿危閸儱绾ч柛顐ｇ濞呭懐鎲搁弶娆惧剳缂佽鲸鎹囧畷鎺戔枎閹烘垵甯┑鐘愁問閸犳岸寮拠鑼殾鐟滅増甯掗柨銈嗕繆閵堝倸浜剧紓浣哄У閻楃娀寮婚妸鈺傚亜闁告繂瀚呴姀銈嗙厱婵°倓绀侀埢鏇㈡煛鐏炵偓绀夌紒鐘崇洴婵＄柉顦撮柨娑氬枛濮婃椽宕ㄦ繝搴ｅ姺闂佽桨娴囬褔锝炶箛鏃傜瘈婵﹩鍓涢敍婊冣攽椤旂煫顏勭暦椤掑娂鐑藉焵椤掆偓閳规垿鎮欑€涙ê闉嶇紒鐐緲缁夊綊濡撮崘鈺冪瘈闁稿本绮嶅▓楣冩⒑閸濆嫭绁紒杈ㄦ礋閹潡鍩€椤掑嫭鈷戦柛锔诲弨濡炬悂鏌涢妸鈺€鎲剧€殿噮鍓熼崺鈧い鎺嶆缁诲棝鏌ｉ幇鍏哥盎闁逞屽墯閻楁洝鐏嬫繛鎾村焹閸嬫捇鏌ㄥ┑鍫濅沪鐎垫澘瀚换娑㈡倷椤掑倵鍋撳ú顏呪拺缂侇垱娲栨晶鍙夈亜椤撶偞宸濈紒顔剧帛閵堬綁宕橀埡鍐ㄥ箺闂備浇顫夐崕鎶藉疮閸ф鍎婇柕濞炬櫆閻撶喖鏌熼崜褍浠洪柛瀣ㄥ灲閺屸€崇暆閳ь剟宕伴弽顓溾偓浣糕枎閹炬潙浠奸柣蹇曞仦閸庡啿鈻嶅鑸电厽閹兼番鍩勯崯蹇涙煕閻樺磭澧甸柟顔哄劜缁轰粙宕妷銉с偊闂佽鍑界紞鍡樼閺嶎厼缁╁ù鐘差儐閻撴瑩姊洪銊х暠閻㈩垬鍔戦弻宥夊传閸曨剙娅ら梺鎶芥敱鐢繝寮诲☉姘勃闁告挆鍕珯闂備胶顭堢换鎰版儗閸岀偛钃熼柣鏂垮悑閹偞銇勯幇鍓佸埌缁剧偓濞婂铏规嫚閳ヨ櫕鐏嶉梺鎸庣閵囧嫰顢曢敐鍥╃暭闂佸湱鎳撶€氫即骞栭崷顓熷珰闁炽儴娅曢弳顏勨攽閻樺灚鏆╁┑顔惧厴瀵偊宕ㄦ繝鍐ㄥ伎闂佹眹鍨藉褔寮搁崼鈶╁亾楠炲灝鍔氭い锔诲灣婢规洝銇愰幒鎾跺幈濡炪倖鍔х徊鍓х矆閸愵喗鐓熼柟鎯у船閸旓箓鏌″畝瀣瘈鐎规洖鐖奸崺鈩冩媴妞嬪孩宕熼梻鍌欑窔閳ь剛鍋涢懟顖涙櫠椤栫偞鐓熼柍鈺佸暞缁€鍫ユ煙?
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

// ---------------------------------------------------------------- WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯顢曢敐鍡欙紩闂侀€炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劍銇勯弮鍥撴繛鍛墦濮婄粯鎷呴崨濠傛殘闂佸憡妫戠粻鎾崇暦娴兼潙鍐€妞ゆ劑鍩勫Λ婊堟⒑缁夊棗瀚峰▓鏇㈡煃闁垮鐏﹂柕鍥у楠炴帡宕卞鎯ь棜缂傚倸鍊风粈渚€藝椤栫儐鏁嬫い鎾跺Т閸ㄦ繂鈹戦悩瀹犲闁告劏鍋撴俊鐐€栭崝妤呭窗鎼淬垻顩查柣鎰靛墻濞堜粙鏌ｉ幇顖氱毢濞寸姰鍨介弻娑㈠籍閳ь剛鍠婂澶娢﹂柛鏇ㄥ灡閺呮粓鎮归崶顏勭毢濞寸姭鏅涢—鍐Χ閸℃鍙嗙紓浣虹帛閸旀瑩宕洪悙鍝勭闁挎洍鍋撴鐐灲閻擃偊宕堕妸锔规嫻闂佸搫顦伴幃鍌氼潖缂佹ɑ濯撮柧蹇曟嚀缁楋繝姊洪幐搴㈢殤闁逞屽墯椤旀牠宕板☉銏犳槬闁哄稁鍘肩粈鍡椻攽閻樻彃鏆熼梻鍕閺屾稑鈽夊鍫濆闂?
// 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍焦鍎撻梺鎼炲妼閸婂潡寮诲☉銏╂晝闁挎繂妫涢ˇ銉х磽娴ｅ搫小闁告濞婂璇测槈閵忊晜鏅濋梺鎸庣箓濡盯藝閵壯呯＜闁绘劦鍓欑粈鍐┿亜椤愩埄妲圭紒宀冮哺缁绘繈宕堕‖顑洦鐓曟繛鎴濆船閺嬫盯鏌ｅ┑鍥ㄢ拻缂?WARP 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍焦鍎撻梺鎼炲妼閸婂潡寮诲☉銏╂晝闁挎繂妫涢ˇ銉х磽娴ｅ搫小闁告濞婂璇测槈濡攱鏂€闂佸憡娲﹂崑鍕叏閵忋倖鈷戞繛鑼额嚙楠炴鏌熼悷鐗堝枠鐎殿喖顭烽弫鎰緞婵犲嫷鍚呴梻浣瑰缁诲倸螞椤撶倣娑㈠礋椤撶姷锛濇繛杈剧稻瑜板啯绂嶆ィ鍐┾拺缂佸瀵у﹢鎵磼鐎ｎ偄娴€规洩缍€缁犳盯寮崜褏鐣鹃梻浣虹帛閸旀瑩路閸岀偛鍚规繛鍡楃贩閻熼偊鐓ラ柛顐犲灮閺嗩偄鈹?闂?
//   1) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯骞橀懠顒夋М闂佹悶鍔嶇换鍐Φ閸曨垰鍐€妞ゆ劦婢€濮规姊洪柅鐐茶嫰婢у墽绱掗悩铏碍闁伙綁鏀辩缓鐣岀矙鐠囦勘鍔戦弻鏇熷緞濞戙垺顎嶉悶姘剧秮濮婂宕掑▎鎴М闂佸湱鈷堥崑鍡欏垝濞嗘劗鐟归柍褜鍓欓悾鐑藉閿涘嫰妾梺鍛婄☉閿曘倝鍩€椤掆偓濞硷繝寮诲☉鈶┾偓锕傚箣濠靛懐鎸夊┑鐐茬摠缁秶鍒掗幘璇茶摕闁绘棁銆€閸嬫捇鎮藉▓璺ㄥ姼婵炲濮电划宥夊Φ閸曨垰鍗抽柣鏃囨閻撲線姊洪棃娑欐悙閻庢矮鍗冲顐﹀礃椤旇偐锛滈梺闈涚墕濡厼螞閺嶎偆纾?connected 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煕閹邦垰绱﹂柣銏狀煼濮婄粯鎷呴悷閭﹀殝濠电偛寮堕崹鍧楀箖閻愮儤鏅濋柛灞炬皑閻ゅ洭鎮楅崗澶婁壕闂佸憡娲﹂崗姗€骞?disconnected      -> 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵挳鎮欏ù瀣壕闁归棿鐒︾€氬懘鏌ｉ弬鍨倯闁绘挸绻愰埞鎴︽倷闂堟稐澹曞┑鐐叉噺閹瑰洭寮婚埄鍐╁缂佸顑欐导鍐⒑閸濆嫭婀扮紒瀣灴閸┿儲寰勬繛鐐€婚梺褰掑亰閸犳岸鍩㈣箛娑欌拻濞达絽鎲￠幆鍫ユ倵濮樸儱濮傜€规洘娲熼幃鐣岀矙閼愁垱鎲伴梻浣告惈濞村嫮妲愰弴銏″仾闁逞屽墴閺岋絾鎯旈婊呅ｆ繛瀛樼矋閻楃姴鐣烽幋锕€绠婚悹鍥ㄥ絻閸炪劌顪冮妶鍡樺暗闁哥姴閰ｉ崺鈧い鎺戝暞绾墎绱掓潏銊﹀鞍闁瑰嘲鎳橀幊鏍焺閸愩劎浼岄悗瑙勬穿缂嶁偓缂佺姵鐩顒勫礈瑜忛悾鐐繆閻愵亜鈧牠鎮уΔ鍛紶閹兼番鍨婚々鐑芥煙閻戞﹩娈曢柣鎾存礃閹便劌螣閻撳骸浠樻繛瀛樼矆閸楁娊寮婚敍鍕勃闁告挆鈧Σ鍫ユ⒑鐎圭媭鍤欐繛鎾敱缁傛帡鏁冮崒姘鳖吅闂佸湱鍎ら崺鍫澪?
//   2) 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯骞橀懠顒夋М闂佹悶鍔嶇换鍐Φ閸曨垰鍐€妞ゆ劦婢€濮规姊洪柅鐐茶嫰婢у墽绱掗悩铏碍闁伙綁鏀辩缓鐣岀矙鐠囦勘鍔戦弻鏇熷緞濞戙垺顎嶉悶姘剧秮濮婂宕掑▎鎴М闂佸湱鈷堥崑鍡欏垝濞嗘劗鐟归柍褜鍓欓悾鐑藉閿涘嫰妾梺鍛婄☉閿曘倝鍩€椤掆偓濞硷繝寮诲☉鈶┾偓锕傚箣濠靛懐鎸夊┑鐐茬摠缁秶鍒掗幘璇茶摕闁绘棁銆€閸嬫捇鎮藉▓璺ㄥ姼婵炲濮电划宥夊Φ閸曨垰鍗抽柣鏃囨閻撳倻绱撴担浠嬪摵閻㈩垱顨婇獮蹇涘川閺夋垳绱堕梺鍛婃礀閻忔艾袙婢舵劖鈷?connected 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝呂熷▎鎯ф缂備胶濮撮悘姘跺Φ閸曨喚鐤€闁圭偓鎯屽Λ鐐电磽娴ｇ晫鍫柛濠冪箞瀵鏁冮埀顒冪亽闂佺粯鑹鹃顓犵矆閸℃せ鏀介柣鎰綑閻忥附銇勯鐐测偓鍨嚕椤曗偓瀹曠厧鈹戦崱娆戝春濠碉紕鍋戦崐鏍箰妤ｅ啫纾婚柣鎰惈绾炬寧銇勮箛鎾村櫢缂佽妫濋弻锝夊箛閸忓摜鐩庨梺閫炲苯澧柣妤冨█閻涱噣骞囬弶鍧楀敹闂佸搫娲ㄩ崑娑㈡倵椤撱垺鍋℃繝濠傛噹椤ｅジ鎮介娑辨疁鐎规洘娲栬灃濞达綁顥撶粻姘舵⒑闂堟稓澧曢柟鍐茬箻瀹曞啿煤椤忓懐鍘介梺瑙勫劤瀹曨剟宕濆鍛＜缂備焦顭囩粻鐐翠繆椤愩垹鏆欓柍钘夘槸閳诲骸螣娴兼瑧纾兼繝纰夌磿閸嬫垿宕愰幋锕€纾块柤纰卞墮閸ㄦ繈鏌熼幑鎰靛殭缂佺姵鐗滈惀顏堫敇閻愭潙顎涘┑鈩冨絻椤嘲顫忛搹鍏夊亾閻ф悂妾繝銏∩戞穱濠囧锤濡や胶鍘靛Δ鐘靛仜閻忔繈鎮橀埡鍛厓闁芥ê顦藉Ο鈧繝娈垮枓閸嬫捇姊虹紒姗嗙劷闁轰礁顭烽幃浼村箻缂佹ǚ鎷绘繛杈剧悼閻℃柨顭囬幇鐗堢厵妞ゆ牗姘ㄩ悞鍝モ偓瑙勬磸閸ㄨ櫣缂撻悾宀€鐭欓悹渚厛濡茶埖淇婇悙顏勨偓鏍蓟閵婏附娅犻柣锝呰嫰椤ユ岸鏌﹀Ο渚＆鐟滅増甯楅弲鏌ユ煕椤愵偄浜滄繛鍫熷姇閳规垿鎮欓懠顒€顤€濠殿喖锕ょ紞濠冧繆閻㈢绀嬫い鏍ㄧ⊕濞呭棝姊虹紒妯哄Е闁告挻鑹捐闁割偁鍎查崐鍨箾閸繄浠㈤柡瀣⊕閵囧嫰顢橀悙鍙壭╅梺浼欑悼閸忔ɑ淇婇幖浣哥厸濠电姴鍟▍宥夋煟閻斿摜鐭婇梺甯到閻ｇ兘骞樼紒妯轰缓闂佸憡绋戣墝闁归攱妞藉娲川婵犲嫧妲堥梺鎸庢磸閸庢娊鍩€椤掆偓閻忔岸鎮烽埡鍛摕婵炴垶锕╅悡銉╂煃瑜滈崜鐔煎箖閻愮儤鏅濆ù锝嗗絻閹焦绻濋悽闈浶ラ柡浣告啞閹便劑鎮滈挊澶嬬€梺褰掓？缁€浣哄婵犳碍鐓熼柟閭﹀墯閹牓鏌涢妸銉モ偓鍧楀蓟濞戙埄鏁冮柨婵嗘椤︺儳绱撴担鍝勑￠柛妤佸▕瀵鈽夐姀鈺傛櫇闂佹寧绻傚Λ娑⑺囬妷鈺傗拺闁硅偐鍋涙俊濂告煕婵犲倹鍋ョ€殿喛顕ч埥澶愬閳╁啯鐝曢梺鑽ゅТ濞诧箒銇愰崘顏嗕笉濞寸厧鐡ㄩ埛鎴犵磼鐎ｎ偒鍎ラ柛搴㈠姍閺岀喖鎮烽悧鍫熸倷闂佷紮绲块弻澶愬Φ閹版澘绠虫繝闈涙处濞呭洭姊绘担铏广€婇柛鎾寸箞閺佸姊虹拠鑼闁瑰憡濞婂?缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵潙螖閳ь剚绂嶆ィ鍐╃叄闁煎鍊曟禒閬嶆煟閹垮啫浜扮€规洖鐖兼俊姝岊槷婵℃彃鐗撳娲箹閻愭彃顬堥梺绋匡工濞尖€愁嚕婵犳碍鍋勯柣鎾虫捣閻ｉ箖姊虹紒姗嗘當闁绘绮岃灋闁告洦鍘剧壕濂告煟閹伴潧澧柛鏂诲€栭妵鍕敇閻樻彃骞嬮悗娈垮枛椤攱淇婇幖浣肝ㄩ柕蹇婂墲閺夋悂姊绘担铏广€婇柛鎾寸箞閺佸顪冮妶蹇氬悅闁哄懐濮撮～蹇涙惞閸︻厾鐓撳┑鐐叉閸庢娊宕滈柆宥嗏拺缂備焦锕╁▓鏇熴亜閵娿儻鏀诲ǎ鍥э躬閹晫绮欑捄顭戞Ч婵＄偑鍊栭悧妤€顫濋妸鈺傚仾闁逞屽墴濮婄粯鎷呯粙娆炬闂佺顑呴幊搴ｅ弲濠殿喗銇涢崑鎾淬亜閵忊剝灏い鎾炽偢瀹曨亝鎷呴悷鎵В闂傚倷绶氬褔鎮ч崱娑樼疇閹兼惌鐓堥弫鍡涙煏韫囧鈧牠鎮￠悢鍏肩厽闁哄倸鐏濈痪褎绻涢崨顓ㄥ姛闁?
//      -> 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涱儐椤ュ牊绻涢幋婵嗚埞闁告挻澹嗙槐鎺楁倷椤掆偓閸斻倖銇勯鐘插幋鐎殿喖顭烽弫鎰緞婵犲喚妫熼梻浣稿閻撳牓宕板Δ鍛９妞ゆ牜鍋為埛鎴犵磼鐎ｎ偒鍎ラ柛搴＄Ч閺屾盯寮幐搴㈠闯濠碘€冲级閸旀瑩鐛Ο灏栧亾濞戞顏堫敁閹惧绡€婵炲牆鐏濋弸鐔兼煕閳轰胶澧涚紒鍌氱Ч閹瑥霉鐎ｎ偆鈧姊洪棃娑氱疄闁稿﹥鐗犲畷鎰版倻閼恒儳鍘卞┑鐘绘涧濡顢旈鈶芥棃鎮╅崣澶婃灎濠殿喖锕ュ钘夌暦濠婂牆绠甸柟鍝勭Х閸╁懘姊绘担渚劸妞ゆ垵鎳愰埀顒佸嚬閸樺墽鍒掔€ｎ亶鍚嬪鑸瞪戦弲顒€鈹戦悩璇у伐閻庢凹鍓氱粋?2 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵闁绘垵妫涢崝顖氣攽閻愭潙鐏︽い顓炴喘瀵娊宕卞☉娆戝幈闂佸搫娲㈤崝灞剧閻愭番浜滈柨鏃囶嚙楠炴牠鏌嶇憴鍕伌闁诡喒鏅濈槐鎺懳熸繝姘殬濠电姷鏁搁崑鐘活敋濠婂懐涓嶉柟杈剧畱閻撴繄鈧箍鍎遍ˇ浼村磻閹邦喚纾藉ù锝堫嚃閻掍粙鏌涘Ο鎸庮棄闁宠鍨块崺銉╁幢濡ゅ啩娣梻浣侯焾椤戝棝宕濆Δ鍛瀬妞ゆ柨鐨峰Σ鍫ユ煏韫囥儳纾块柛妯圭矙濮婃椽宕烽鐐插濠殿喖锕ㄥ畷鐢稿极椤斿皷妲堥柕蹇ョ磿閸樺灚绻涢幘纾嬪闁诲繑绻冩穱濠冪附閸涘﹥鍤夐梺鎸庣箓閹峰鎮㈤崱娑欑厱闁规崘灏欑拹浼存煟閹烘鐣洪柡宀€鍠栭、娑橆潩閸楃偐鍙烘繝娈垮枛閿曘儱顪冮挊澶屾殾闁靛濡囩弧鈧梺鍛婃处閸撴瑩寮搁敃鍌涒拻濞达絼璀﹂悞鐐亜閹存繃顥㈡鐐村灴瀹曟儼顧佹繛鍏肩墱缁辨挻鎷呴懖鈩冨灦閸掑﹪骞橀鐣屽幗闂佺粯姊婚崢褎绂嶆导瀛樼厱闁靛牆鎷嬮崕鎴犵磼鏉堛劍灏伴柟宄版嚇瀹曟粓骞撻幒鎴濐棊闂傚倷鐒﹂幃鍫曞礉鐏炵偓娅犻幖杈剧悼绾句粙鏌熼幆褜鍤熸い鈺冨厴閹鏁愭惔婵堟晼闂佹椿鍋勭€氭澘顫忛搹鍦煓闁圭瀛╅幏杈╃磽娓氬洤鏋涢梺甯秮楠?10 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬鐎广儱顦粈瀣亜閹哄秶鍔嶆い鏂挎喘濮婄粯鎷呴崨濠呯闂佺绨洪崐婵嗙暦閻㈠憡鏅濋柍褜鍓熼崺銉﹀緞婵炪垻鍠栭弻銊р偓锝庡亝濠㈡垿姊绘担绛嬪殭闁告垹鏅槐鐐哄幢濡⒈娲搁梺缁樺姃鐠€锕傚极婵犲洦鐓曢柕澶樺枛婢ь垶鏌嶉柨瀣伌闁诡喖鍢查埢搴ょ疀閹垮啩鎮ｇ紓鍌欒閸嬫捇鏌涢幇銊︽澓濞存粍绮撻弻锟犲炊瑜庨ˉ婊堟煙閾忣偄濮嶉柟顖氱焸瀹曞崬鈽夊▎鎴濆箞闂備線鈧稑宓嗘繛浣冲洤鍑犻柣鏂垮悑閻撶喖鏌熼幆褏鎽犵紒鈧崘顏嗙＜缂備焦顭囧ú瀛橆殽閻愬樊鍎忛柍瑙勫灴瀹曞崬顫濋鍌氬箣闂傚倸鍊风粈渚€骞栭锔绘晞闁搞儺鍓欑粣妤呭箹濞ｎ剙濡界紒鐘冲浮濮婄粯鎷呴搹鐟扮濡炪値鍘奸悧蹇曞垝閺冨牜鏁嬮柍褜鍓熼妴渚€寮崼婵嬪敹濠殿喗锕徊鑺ョ妤ｅ啯鐓ラ柡鍥╁仜閳ь剚鎮傞弻銊╁Χ閸℃洜绠氶梺闈涚墕鐎氼噣藝閿旂晫绠鹃柛顐ゅ枔缁犵偞鎱ㄦ繝鍌涙儓閺佸牓鏌涢妷鎴濇噸缁辨洟姊绘担鑺ャ€冮柣鎺炵畵楠炲啴宕掑杈ㄦ闂佹寧绻傞幊澶愬绩娴犲鐓曟い鎰Т閻忣噣鏌ㄥ☉娆戠畺缂佺粯绋掑蹇涘礈瑜忚ⅲ婵＄偑鍊ら崑鍕囬悽绋挎槬婵炴垯鍨圭粻娑欍亜閹捐泛啸妞ゆ梹娲熷铏圭矙鐠恒劎顔戦梺绋款儐閸旀瑩鐛繝鍥х倞妞ゆ帊鑳堕崢閬嶆⒑瀹曞洦鍤€闁诲繑绻堥幃姗€顢旈崼鐔哄幍濡炪倖鐗楃粙鎺椝夐崼婵冩斀闁斥晛鍟崐鎰攽閿涘嫭鐒挎い锕佸皺閳ь剚顔栭崰鎾诲礉閹存繍娼栫紓浣股戞刊鎾煕濞戞﹫宸ラ柡鍡楃墛缁绘盯骞樼€靛壊鏆″銈冨妼閿曘倝鎮鹃悿顖樹汗闁圭儤鍨归鍛存⒑閸涘﹥灏柣蹇旂箓鐓ゆい鎿冩娇閳ь兛绀侀～婵堟崉閾忕懓濮︽俊鐐€栧濠氬磻閹炬番浜滄い鎰╁灮缁犲磭绱掓潏銊ョ瑨閾伙綁鏌ゅù瀣珕闁搞倕鐭傚缁樻媴鐟欏嫬浠╅梺绋垮缁挸鐣峰鈧幃娆擃敄閹稿骸鏀梻鍌氬€搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪箻楠炴垿濮€閵堝懐顓洪梺缁樏崯鎸庢償婵犲洦鈷戦柛鎾村絻娴滄繄绱掔拠鎻掝伃鐎殿喗濞婇弫鍐磼濞戞艾甯鹃梻濠庡亜濞层倝顢栭崨鏉戠劦妞ゆ帊鐒︾粈鈧┑鐐村絻閻忔繈鍩為幋锔藉亹妞ゆ棁鍋愭导鍥р攽閻愬樊妲归柣鈺婂灦瀵偊宕橀鑺ユ珖闂佺顑囬幊鎾趁洪顫偓渚€寮撮姀鈩冩珕闂佽姤锚椤︻偊宕戦幘缁樺仺闁告稑锕﹂崢浠嬫⒑缂佹ɑ鐓ラ柟璇х磿濞嗐垽宕ｆ径鍫滅盎闂婎偄娴勭徊鑲╄姳缂佹ǜ浜滄い鎾跺仦閸嬨儵鏌熼鎯у幋闁诡啫鍥ч唶妞ゆ劧缍嗗Λ褍鈹?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮村Δ鈧禍鎯ь渻閵堝簼绨婚柛鐔告綑閻ｇ柉銇愰幒婵囨櫔闂佸憡渚楅崹鐗堟叏濞差亝鈷掑ù锝勮濞兼帡鏌涢弴鐐典粵闁伙絽澧庣槐鎾存媴閹绘帊澹曞┑鐘灱濞夋稒寰勯崶顒€纾婚柟鎹愬吹瀹撲線鏌涢…鎴濇灈濠殿喖楠搁—鍐Χ韫囨挾妲ｉ梺鎼炲姀濞夋盯顢氶敐鍡楊嚤闁哄鍤﹂妸鈺傜叆闁哄倸鐏濋埛鏃€銇勯弮鈧ú鐔奉潖濞差亝鍋￠梺顓ㄧ畱濞堝爼姊虹粙娆惧剳闁哥姵鐗犻悰顔界節閸パ冪獩闁诲孩绋掗…鍥储閹惧墎纾介柛灞剧懅閸斿秹鎷戞潏銊ｄ簻闁靛鍎虫晶锕傛煛瀹€鈧崰鏍€佸▎鎾村殐闁冲搫鍞妸鈺傚€甸悷娆忓缁€鈧梺缁樼墪閸氬绌辨繝鍥х濞达綀鍊介妸褎鍠愰煫鍥ㄧ☉鍥撮梺缁樺灱婵倝鎮￠悢鍏肩厽闁归偊鍓涢幗鐘绘煟閵婏箑鐏撮柡灞剧洴閹晛鐣烽崶褉鍚傛俊鐐€ら崑鍕崲閹邦喖寮叉俊鐐€曠换鎰偓姘间簽濡叉劙宕奸弴鐔叉嫼闂佸憡绋戦敃銉﹀緞閸曨垱鐓曢悗锝庝簻閳ь剙顭烽幊鐐存綇閵娧屾祫闁诲函缍嗛崑鈧柟閿嬫そ濮婃椽宕ㄦ繝鍕ㄦ濡炪値鍙庨崜鐔煎箖閻愭番鍋呴柛鎰ㄦ櫇閸橀亶鏌ｆ惔顖滅У闁稿瀚幈銊╁炊椤掆偓绾偓闂備緡鍓欑粔鐢告偂濞嗘挻鐓熸俊銈傚亾闁绘锕﹀▎銏ゆ嚑椤掑倻锛滈梺缁樼懃閹虫劗绮旈鈧弻锛勪沪鐠囨彃顫囬悗娈垮枟濞兼瑨鐏冮梺閫炲苯澧紒鍌氱Ч瀹曟粏顦寸痪?g_warpManualStop=true闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢悰鈥充壕闁汇垺顔栭悞鎯归悩娆忔处閳锋帡鏌涢弴銊ヤ簻妞ゅ浚鍙冮弻鈥崇暆鐎ｎ剛鐦堥悗瑙勬礃閿曘垺淇婇幖浣肝ㄩ柕蹇曞С婢规洟姊洪悡搴綗闁稿﹥顨嗙粙澶婎吋婢跺鍘搁梺绋挎湰娓氭宕曢妷鈺傜厽闁哄倹瀵чˉ銏ゆ煛鐏炶鈧繈鐛弽銊﹀閻熸瑥瀚伴弫顏勨攽閻樻剚鍟忛柛鐘崇墪鐓ら柕濞у嫷娼熼梺鍦劋椤ㄥ棝宕戦幇顓滀簻闁哄啫鍊哥敮璺衡攽椤曞棛鐣垫慨濠冩そ瀹曨偊宕熼鐘辩礃闂備礁鎽滄慨鐢告偋閻樿崵宓侀柛鎰╁妷閺嬪酣鏌熼崹顔兼殲闁哄懌鍨虹换娑欐綇閸撗呅氬┑鐐叉嫅缂嶄線宕洪姀锝囩杸闁哄倸澧界粻姘舵⒑缂佹ê濮﹀ù婊勭矒閸┾偓妞ゆ帊绀侀悘瀵糕偓瑙勬礃濞叉ê顭囪箛娑樼厸闁告劦浜為崢鐘绘煟閻斿摜鐭屽褎顨堥弫顕€骞掑Δ浣哥彅?
bool g_warpManualStop = false;
int g_warpLastState = -1;
int g_warpFailStreak = 0;
ULONGLONG g_warpLastNotify = 0;

// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簻椤掋垺銇勯幇顏嗙煓闁哄被鍔戦幃銏ゅ传閸曟垯鍨婚惀顏堝箚瑜滈悡濂告煛鐏炲墽鈽夐柍钘夘樀瀹曪繝鎮欑€甸晲澹曢梻鍌欑绾绢參骞婇幘鍓佺煋鐟滅増甯掗拑鐔烘喐閹寸姷涓嶆繛鎴烇供濞尖晠鏌涜箛姘汗缂佸鍏橀弻?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煕閹邦垰绱﹂柣銏狀煼濮婄粯鎷呴悷閭﹀殝濠电偛寮堕悧鐘茬暦閹邦垬浜归柟鐑樺灩閿涙盯鏌ｆ惔顖滅У闁哥姵顨婇幃鈥斥槈閵忥紕鍘遍梺瑙勫閺呮稒淇婃總鍛婄厓鐟滄粓宕滃▎鎾偓锕€鐣￠幍顔芥闂佺偨鍎查崜姘焽閳哄懏鐓犻柟闂寸劍濞懷囨煃瑜滈崜娆撴倶濮樿鲸宕叉繛鎴欏灩缁狅綁鏌ｉ幇顒備粵闁革絼绮欏铏圭磼濡櫣鐟愮紓渚囧枟閻熲晛顕?204 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻锝夊箣閿濆憛鎾绘煕婵犲倹鍋ラ柡灞诲姂瀵潙螖閳ь剚绂嶆ィ鍐╃叄闁煎鍊曟禒閬嶆煟閹垮啫浜扮€规洖鐖兼俊姝岊槷婵℃彃鐗撳娲箹閻愭彃顬堥梺绋匡工濞尖€愁嚕婵犳碍鍋勯柣鎾虫捣閻ｉ箖姊虹紒姗嗘當闁绘绮岃灋闁告洦鍘剧壕濂告煟閹伴潧澧柛鏂诲€栭妵鍕敇閻樻彃骞嬮悗娈垮枛椤攱淇婇幖浣肝ㄩ柕蹇婂墲閺夋悂姊绘担铏广€婃俊鐙欏洤鐤炬繝濠傛噽閹冲懘姊绘担鍛婃儓妞わ富鍋婂鎻掝煥閸涱喖搴婇梺鍓插亝缁诲秴顭囬弽顓熺叄闊洦鎸荤拹锟犳煟椤撶喓鎳囬柡宀€鍠撶划娆撳垂椤曞懎濡烽梻浣告贡閺屽绂嶉鍌涱潟闁规儳鐡ㄦ刊瀵哥磼濞戞﹩鍎忔繛鍫弮閺岋絾鎯旈姀鐘叉瘓闂佸摜濮靛畝绋跨暦濮樿泛绠抽柟鎯у閻﹀牆鈹戦鏂や緵闁告挻绻堥獮妤呭即閵忊檧鎷洪梻鍌氱墛娓氭顬婅閳规垿鍨鹃搹顐㈡灎閻?true 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞鐟滃繘寮抽敃鍌涚厽闁靛繈鍩勯悞鍓х棯閹岀吋闁哄矉缍侀獮鍥敊閼恒儲鐦庨梻浣规た閸樹粙鎮烽埡鍛亗妞ゆ劧绠戦悙濠囨煏婵炑€鍋撴俊鎻掔墢缁辨挻鎷呴幓鎺嶅濠电姰鍨奸崺鏍礉閺囩姷涓嶉悷娆忓缁犻箖鏌ㄥ┑鍡涱€楀褜鍠栭埞鎴︽晬閸曨偄浠樺銈傛櫅閵堢鐣烽妸鈺婃晣婵炴垶鐟﹂弶鍛婁繆閻愵亜鈧牕顫忔繝姘仱闁哄啫鐗婇崐鍧楁煟閹伴潧澧扮紒鐘冲劤椤法鎹勬笟顖氬壋闂侀潧妫欑敮鐐垫閹烘鏁婇柤娴嬫櫅閳敻鎮楃憴鍕闁哥姵鐗犻妴浣糕槈濡攱顫嶅┑顔斤供閸撴瑧绮婚悙娴嬫斀闁绘ê鐏氶弳鈺佲攽閻愨晛浜炬繝鐢靛仒閸栫娀宕楅悙顒傗槈閾绘牠鏌涘鈧悞锕傚疾濠婂牊鈷戦柛鎰级閹牓鏌涢悢绋款棆濠㈣娲熼幃鍓т沪缁嬪じ澹曞Δ鐘靛仜閻忔繈宕濆顓濈箚妞ゆ劧绲块幊鍐煟閿濆懎妲婚柣锝嗙箞瀹曠喖顢楅崒姘闂備浇顕х换鎺楀磻濞戞瑦娅犲ù鐘差儏閻撯€愁熆閼搁潧濮堥柣鎾冲暣閹綊宕堕妸褍寮ㄥ銈呭椤ㄥ﹪骞楅崼鏇熸櫜濠㈣泛顑囬崣鍕椤愩垺澶勭紒瀣墦瀹曟垿骞橀弬銉︻潔濠碘槅鍨拃锕傚吹濞嗘挻鈷掗柛灞捐壘閳ь剚鎮傚畷鎰暋閹冲﹤缍婂畷鎯邦檨婵炲瓨鐗犻弻鏇熺箾閻愵剚鐝曢梺鎶芥敱濮婂鍩€椤掆偓缁犲秹宕曢柆宓ュ洭顢涘鍐炬闁荤喐鐟ョ€氼喚寮ч埀顒勬⒒閸屾氨澧涚紒瀣浮閹苯螖閸愵亞锛?
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
        if (st == 0) {                     // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵婵炲棗绻掓禒楣冩⒑缁嬪灝顒㈤柛銊ユ健瀵鏁嶉崟顏呭媰闁荤姴娲﹁ぐ鍐╂叏鎼淬劍鐓曢柕澶涢檮鐏忥箓鏌″畝鈧崰鏍х暦濡ゅ懏鍤冮柍鍝勫€归鍐⒒娴ｈ櫣銆婇柡鍛〒閳ь剚鍑归崜婵嬫倶鐎ｎ喗鈷戦柛娑橈功閳藉鏌ｆ幊閸旀垿宕哄☉銏犵闁挎棁妫勬禒顖炴⒑閹肩偛鍔橀柛鏂块叄瀹曪綁鍩€椤掆偓椤啴濡堕崨顓у妷濡炪們鍔岄悧蹇撐ｉ幇鏉跨婵°倓绀佹禍褰掓⒑閼测斁鎷￠柛搴ｆ焿椤︽挳鏌嶈閸撴繈锝炴径濞掑搫顭ㄩ崼婵堢枃闂佽宕樺▔娑橆嚕閸欏绻嗛柣鎰典簻閳ь剚娲滈幑銏ゅ箛椤旂懓浜炬慨妯煎帶楠炴牠宕℃潏銊ｄ簻闁瑰搫妫楁禍楣冩⒑鐎圭媭娼愰柛銊ユ健楠炲啫鈻庨幘宕囬獓闂佺懓顕慨鍐差浖閹惧绡€婵炲牆鐏濋弸鐔兼嚕瑜旈弻娑㈠籍閳ь剟宕归悽鍛婂仼闁绘垼妫勯悘鎶芥煙妫颁胶鍔嶉柛宥夋涧椤啴濡堕崱妯洪瀺缂備緡鍠栭悧蹇涙嚍鏉堛劎绡€婵﹩鍘搁幏娲⒑閸涘﹦绠撻悗姘煎枦閳綊姊绘担渚劸闂佸府绲剧粋宥夊醇閺囩偞妲梺閫炲苯澧柕鍥у楠炴帒顓奸崶顏嗙崶婵犵數鍋涢悧濠団€﹂崶鈺傤潟闁圭儤顨忛弫濠囨煠濞村娅呮い銈傚亾闂佽楠搁悘姘熆濡皷鍋撳鐓庡箻闁瑰箍鍨归埥澶愬閻樻鍞洪梻浣烘嚀椤曨厽鎱ㄩ柨瀣垫闂傚倸鍊峰ù鍥敋閺嶎厼绐楅柟鐑橆殔閻ら箖鏌ｅΟ鑽ゆ菇闁逞屽厸缁舵艾顕ｉ幘顕嗙稏妞ゆ柨澧介埣銈嗕繆閻愵亜鈧牠骞愭ィ鍐ㄧ？闁归偊鍎靛☉姗嗙叆闁告劧绲鹃弬鈧梻浣虹帛閿氶柣蹇斿哺瀵娊鍩￠崨顔惧幈闁诲函缍嗛崜娆愮鏉堚斁鍋撶憴鍕缂傚秴锕ら悾閿嬬附缁嬪灝宓嗛梺缁樺姌閸╂牠骞夋导瀛樷拻濞撴埃鍋撴繛浣冲棗顤傞梻浣告惈閹冲繒鍒掗幘宕囨殾闁瑰濮崑鎾绘晲鎼粹剝鐏嶉梺缁樻尰閻熲晠寮婚悢鐑樺枂闁告洦鍋勮闂備焦鎮堕崐鏍偡瑜忓Σ鎰板箳濡も偓閻掑灚銇勯幒鎴濐仼缁炬儳鍚嬬换娑㈠箣閻戝棙鐣肩紓浣哄У閼归箖鈥旈崘顔嘉ч柛鈩冪懃椤呯磽娴ｇ瓔鍤欓柛濠傜仢閻ｇ兘濡烽妸锝勬睏闂佸湱鍎ら幐楣冨储閹惧墎纾介柛灞剧懅閸斿秹鎷戦柆宥嗙厓闂佸灝顑呭ù顕€鏌＄仦鐣屝ユい褌绶氶弻娑㈠箻鐎靛憡鍣紓渚囧枦椤曆囶敇閸忕厧绶炲┑鐘插缁辩敻姊婚崒娆戣窗闁告挻鐟х划鏃傗偓闈涙憸娑撳秹鏌ｅΟ纰辨毌闁稿鎹囬悰顕€宕归鐓庮潛婵犵數鍋涢惇浼村磹濠靛棛鏆?
            g_warpLastState = 0;
            return 0;
        }
        if (st != 1) {                     // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珖闂侀€炲苯澧板瑙勬礃缁绘繂顫濋鐘插箞闂備焦鏋奸弲娑㈠疮娴兼潙鐓″鑸靛姈閻撴洟鎮橀悙棰濆殭濠殿喖鍊块弻锛勪沪鐠囨祴鍋撻弴銏＄畳闂佽鍑界紞鍡涘磻閸涘瓨鍋熼柡鍥╁剱濞?
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
constexpr wchar_t kAppVersion[] = L"3.19.2";

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
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛闁诲氦顫夊ú蹇涘磿閹惰棄鐒垫い鎺戯功缁夌敻鏌涜箛鏃撹€跨€规洘鍨块獮姗€宕滄担铏规殽婵犵數鍋為崹鎯版懌闂侀潻绲界紞濠囧蓟濞戙垹惟闁挎柨澧介悿鍕⒑鐠団€虫灍闁挎洏鍨介獮鍐ㄢ枎閹惧啿鐎銈嗗姧缁茶儻銇愯濮婄粯鎷呴崨濠冨創濠碘槅鍋勯顓犲垝婵犳艾绠婚悹鍥蔼閹芥洟姊洪幐搴ｇ畵妞わ富鍨抽悮鎯ь吋婢跺鍘遍柣蹇曞仦瀹曟ɑ绔熷Ο濂藉綊鎮╅幇浣哄姺缂備浇椴搁幐濠氬箯閸涙潙浼犻柛鏇熷煀缁插墽鎹㈠┑瀣潊闁宠棄鎳撻埀顒€娼￠弻宥夊Ψ椤栨粎鏆ら悗瑙勬礈椤牐鐏冩繛杈剧到閹诧繝鎮块崒鐐粹拻闁稿本鑹鹃埀顒佹倐閹勭節閸曨厾鐓撻梺纭呮彧缂嶅棝寮堕悜妯肩瘈闁汇垽娼у暩闂佽桨鐒﹂幃鍌氱暦閹存績妲堥柕蹇娾偓鍏呮闂備線娼ч…鍫ュ磿瀹ュ悿娲敂閸涱厺绨婚梻浣虹帛閹哥霉閻戣姤鍋╅柛婵嗗閺€浠嬪箳閹惰棄纾归柟鐗堟緲绾惧鏌熼崜褏甯涢柣鎾冲暣閺岋箑螣娓氼垱鈻撳┑鈥冲级閹倿寮诲鍥ㄥ珰闁哄被鍎洪埀顒侇殜閹藉爼鏁愭径瀣幘婵犳鍠楅崝鏇㈠焵椤掍胶澧柍缁樻尰閵堬綁宕橀埞鐐闂備胶顭堥張顒勬偡閿曞伖鍥槹鎼达絿锛滄俊鎻掓湰閻楁洖鈻嶅鍡樺弿濠电姳鑳堕惌娆戔偓瑙勬礈閸犳牠銆佸鈧幃銏犵暋閹殿喖鎼搁梻鍌氬€风粈浣圭珶婵犲洤纾诲〒姘ｅ亾鐎规洘娲熷畷姗€鎳犻浣诡啌濠电偞鎸婚崺鍐磻閹剧粯鐓涚€光偓閳ь剟宕伴弽顓炵畺婵犲﹤鍠氬銊╂煕閳╁喚鐒介柡鍡愬劤缁辨捇宕掑▎鎺戝帯婵犳鍠栭悘姘辨崲濞戞瑧绡€闁搞儜鍡樻啺闂備胶绮Λ渚€濡撮埀顒勬煕鐎ｎ偅宕岄柟顔挎閳绘挾鎹勯妸銉バ梻浣告憸閸犳捇鈥﹂崼銉⑩偓鏃堝礃椤斿槈褔鐓崶銊︾閽冮亶姊绘担铏瑰笡闁瑰憡鎸抽崺鈧い鎺嶇婢ь垱绻涢崗鑲╁缂佺粯绋戦蹇涱敊閼姐倗娉跨紓鍌欒兌婵娊宕￠幎鐣屽祦闁哄秲鍔嶇紞鍥煕閹炬鍟伴悡妤€鈹戦悩鎰佸晱闁哥姵鐗曠叅闁靛ě鍕垫綗闂佸湱鍎ら〃鍡涘磹闁垮浜滈柟鍝勭Х閸忓矂鏌涢幋顖滅？缂佽鲸鎸婚幏鍛村礈閸欏－銊╂⒑绾懏鐝柟鐟版喘閵嗕礁顫濋懜鍨珳婵犮垼娉涢敃锕傤敊閸涘瓨鈷戦柛蹇涙？閼割亪鏌涙惔銈夊摵缂佸倸绉撮…銊╁醇閻斿弶瀚肩紓鍌氬€烽悞锕佹懌闁诲繐娴氶崢鐣屾崲濞戞埃鍋撻悽娈跨劸闁告ê鐡ㄩ妵鍕閿涘嫭鍣悗鍨緲鐎氼厾鎹㈠┑鍥ㄥ劅闁靛繒濮峰Σ锝夋⒒閸屾瑧绐旀繛浣冲洦鍋嬮柛鈩冦亗濞戞ǚ鏋庨柟鎯хТ椤﹂潧鐣烽崡鐑嗘富缂備焦蓱閺夊憡淇婇悙顏勨偓鏍蓟閵娾晛绠规い鎰剁畱閻掑灚銇勯幒宥堝厡缂佺姷鍋為〃銉╂倷閹绘帗娈柧缁樼墵閺屾稑顭ㄩ埀顒勬倿閿曞倸鐤鹃柍杞拌閺€浠嬫煟閹邦剛鎽犵紓宥嗗灥铻栭柣妯哄级閹插摜绱掓潏鈺佷沪缂佺粯绻堝畷鎯邦槾闁伙絾妞藉娲礃閸欏鍎撻梺鐟板暱缁绘ê鐣烽幇顔剧＜婵☆垰婀辩粻姘渻閵堝棛澧紒顔惧█瀹曟洟鎮╃紒妯煎帗閻熸粍绮撳畷婊冾潩鐠鸿櫣顦┑鐐叉閹稿摜绮婚弽顐熷亾閻熸澘顏柛瀣躬閹繝濡烽埡鍌滃幈濠电偞鍨靛畷顒€顕ｆィ鍐╃厸闁糕檧鏅涘顔芥叏婵犲懏顏犵紒杈ㄥ笒铻ｇ紓浣姑崜鐢电磽閸屾瑨鍏岀紒顕呭灣閹广垽宕奸妷锕€浠掑銈嗘⒒閺咁偆寮ч埀顒勬⒑缁嬫鍎撳┑顔哄€曡灋闁告劑鍔庨弳锕傛煏婵炵偓娅撻柡浣革躬閺屾稖绠涘顑挎睏闂佸搫顑傞崑鎾绘⒒閸屾艾鈧悂宕愭搴㈩偨婵﹩鍓﹂悞鐣屾喐閺冨倻妲甸梻渚€鈧偛鑻晶鎵磼鏉堛劌娴柟顔规櫊閹煎綊顢曢姀銏╂殹濠碉紕鍋戦崐銈夊礈濞嗘挻鏅濇い蹇撶墕缁犳牗绻濇繝鍌滃闁搞倕绉归弻鏇熷緞閸繄浠鹃柣搴㈣壘閵堢顫忕紒妯诲闁惧繒鎳撶粭锟犳⒑閸︻収鐒鹃柟鑺ョ矒閸┾偓妞ゆ巻鍋撻柣蹇旀皑閺侇噣骞掑Δ鈧悡婵嬫煙閸撗呭笡闁稿鍔欓幃閿嬫媴閻ゎ垰鍘￠梺纭呮珪閹瑰洭鍨鹃敃鍌毼╅柍杞拌兌椤︽澘顪冮妶鍡樺暗濠殿喚鍏樺鎶藉Χ婢跺鎷洪梺纭咁潐閸旀洟宕ヨぐ鎺撶厱閻庯綆鍓欐禒鍗炩攽閿涘嫭鏆€规洜鍠栭、娑橆潩鏉堚晜婢戦梻鍌欑婢瑰﹪宕戦崨顒煎搫顫滈埀顒勩€侀幘鎰佸悑濠㈣泛顑傞幏铏圭磽娴ｅ壊鍎愭い鎴炵懇瀹曟洝绠涢弬璁崇盎闂佺懓鐡ㄧ换宥呂熼埀顒勬倵濞堝灝鏋︽い鏇嗗洤鐓″鑸靛姇椤懘鏌嶉崫鍕偓鐟扳枍閸ヮ剚鈷掑〒姘ｅ亾婵炰匠鍥ㄥ亱闁糕剝绋戠粻鐘荤叓閸ャ劎鈯曢柛濠囨涧铻栭柨婵嗘噹閺嗙偤鏌嶉柨瀣伌闁哄本鐩弫鍌滄嫚閹绘帞顔愰梻浣告啞钃辨繛鍏肩懇濠€浣糕攽閻樿宸ラ柛鐔跺嵆瀹曟椽鏁愰崶锝呬壕婵炲牆鐏濋弸娑欍亜椤撱垺鏁卞ǎ鍥э躬楠炴牗鎷呴懖婵勫姂閺屾洝绠涙繝鍐炬綉濠电偛鍚嬪鍦崲濠靛牆鏋堟俊顖涙た濞兼垿姊洪幖鐐插濠电偛锕ら悾鐑藉閿濆孩鈻岄柣鐘辫閸ㄥ爼寮诲鍫闂佸憡鎸诲銊╁箲閵忕姭鏀介悗锝庡亜娴犳椽姊婚崒姘卞闁告巻鍋撳┑鐐村灟閸ㄦ椽鎮￠弴銏″€甸柨婵嗛娴滄繈鎮樿箛搴″祮闁哄本娲熷畷鎯邦槻妞ゅ浚鍘介〃銉╂倷鏉堟崘鈧法鈧娲╃换婵嬪箖濞嗘挸绀傞柛娑橈工娴滄繄绱撻崒姘偓鎼佸磹閻戣姤鍤勯柤绋跨仛閸欏繘鏌涢…鎴濅簽缂佺姵鍎抽埞鎴︽偐閸欏鎮欓梺钘夊暟閸犳牠寮婚敐澶婃闁割煈鍠楅崐顖炴⒑缁嬪尅鍔熺紒顕呭灡缁岃鲸绻濋崶鑸垫櫖濠电娀娼уù鍌涚閳哄倻绠鹃悗鐢殿焾闉嬮梺绋款儐閸旀危閹版澘绠虫俊銈勭閳ь剟鏀遍妵鍕箳瀹ュ牆鍘￠梺鍛婂煀缁辨洜妲愰幘瀛樺闁告挸寮剁瑧缂傚倸鍊哥粔鎾晝閵堝鏁嬮柨婵嗘缁♀偓濠殿喗锕╅崕鐢稿煛閸涱喚鍘繝銏ｆ硾閻楀棝鎮橀鍛簻妞ゆ劑鍨诲瓭闂佸疇顫夐崹鍧椼€佸▎鎴犻┏閻庯綆鍋嗛弳銉︾節绾版ɑ顫婇柛瀣嚇閹兘鍩￠崨顐熷亾娓氣偓楠炴鎷犻幓鎺曗偓鍨攽閻愬弶顥為柛銊ョ秺閺佸秴鈹戦崶鈺冾啎闁哄鐗嗘晶鐣屸偓浣冨亹缁辨帡顢欓悾灞惧櫚閻庤娲﹂崑濠囧箹瑜版帒鎹舵い鎾跺缁辨煡姊洪懡銈呅㈡繛璇х畳閵囨劙宕橀鍏艰緢濠电偛妫欓崝鎴炵濠婂嫨浜滈柟鎹愭硾娴狅妇鈧懓鎲＄换鍫ュ蓟閻斿壊妲归幖绮瑰閵忋倖顥嗗鑸靛姈閸婄敻鏌ㄥ┑鍡涱€楀ù婊呭仱閺屽秷顧侀柛鎾寸懇閳ワ箑鐣￠柇锔界稁闂佹儳绻楅～澶愬窗閸℃稒鐓曢柡鍥ュ妼娴滅偞銇勯敂鍝勫闁宠鍨块幃娆撳矗婢跺﹥顏℃繝鐢靛仩鐏忔瑩宕伴弽顓炍ュ〒姘ｅ亾鐎殿噮鍣ｅ畷濂告偄鐏忎礁浜鹃柣鎴ｅГ閻撶喖鏌熺€电鈧垵顫濈捄铏诡唵濠电偛妯婃禍婵嬪煕閹达附鐓曟繛鎴烇公閸旂喖鏌嶉挊澶樻█闁哄被鍔戝鏉懳熺悰鈥充壕闁哄稁鍘肩粻鐐烘煏婵炵偓娅呴柣鎰躬閺屽秹宕崟顐ｆ闂佹寧绋掔划宥囨崲濠靛顫呴柨婵嗘閵嗘劙姊洪幐搴㈢┛缂佺姵鎹囧顐㈩吋閸滀礁鎮戞繝銏ｆ硾椤戝洭宕㈤棃娴虫棃鎮╅棃娑楃捕闂佽绻戠换鍫ュ箖濮椻偓閸╋繝宕掑Ο鍦泿婵＄偑鍊栭崝褏寰婄捄銊т笉闁哄秲鍔庣粻楣冩煟閹捐櫕鎹ｇ紒鐘侯嚙閳规垿鍩勯崘鈺佲偓鎰攽閿涘嫭鐒挎い锔界叀濮婃椽骞忓畝鍕懙缂備胶绮换鍫ュ箖娴犲顥堟繛鎴烆殔閹藉姊绘担绛嬫綈婵＄偞瀵х粋宥夋倷閸偒娼熼梺缁樺姇椤曨參鎮疯ぐ鎺撶厓鐟滄粓宕滃┑鍠盯濮€閵堝棌鎷洪梺闈╁瘜閸樺吋绂嶉悙顑跨箚闁绘劕寮堕崑銉р偓娈垮櫘閸嬪﹤鐣烽幒妤佸€烽柤纰卞墴閻涙捇姊绘担绋款棌闁稿鎳愮划娆撳箣閻愭壆绠氶柣鐘充航閸斿海澹曟總鍛婄厽婵☆垰鐏濋崥褰掓煟閿濆棙銇濋柡宀嬬磿娴狅箓宕滆閸掓盯姊虹拠鈥虫灀闁哄懐濞€閵嗕線寮撮姀鐙€娼婇梺缁橈供閸嬪懐鈧氨鍠愮换婵嬫偨闂堟刀鐐烘煕閵娿儺鐓兼鐐诧躬楠炴ê煤缂佹ɑ娅堝┑鐘灱濞夋盯鏁冮敃鍌氭辈?
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
        // WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰绠婚柛鈹惧亾濡炪倖甯婇懗鍫曞煝閹剧粯鐓涢柛娑卞枤缁犳﹢鏌涢幒鎾崇瑨闁宠閰ｉ獮妯虹暦閸ヨ泛鏁藉┑鐘茬棄閺夊簱鍋撻幇鏉跨；闁规儳澧庣壕濂告煃瑜滈崜姘嚗閸曨厸鍋撻敐鍌涙珖缂佺姵鑹鹃—鍐Χ閸℃瑥顫х紓浣虹帛閸旀牠銆冮妷銊х杸闁瑰彞鑳堕幊鎾烩€﹂妸鈺佺闁冲搫瀚、鍛存⒒娓氣偓閳ь剛鍋涢懟顖涙櫠椤斿墽纾奸柣妯垮皺鏁堥悗瑙勬礃濞茬喖寮婚崱妤冩殼闁靛鍎查崰妯汇亜閵忥紕鈽夋い顓滃姂瀹曠喖顢曢姀鐘殿唹闂傚倸鍊风欢姘跺焵椤掑倸浠滈柤娲诲灡閺呭墎鈧稒蓱閸欏繐鈹戦悩鎻掓殲闁靛洦绻勯埀顒冾潐濞诧箓宕戞繝鍌滄殾闁绘梻鈷堥弫鍡涙煕鐏炰箙顏呯鐟欏嫮绡€闁汇垽娼ф禒婊堟煟鎺抽崝搴ㄥ礆閹烘挻鍎熼柕濞垮劤閿涙盯姊虹紒妯荤叆闁圭⒈鍋婂畷鐟扳攽鐎ｎ偆鍙嗗┑鐘绘涧濡瑩宕幒妤佺厸闁告劑鍔庢晶鏇犵磼閳ь剟宕橀埞澶哥盎闂婎偄娲﹂幐濠氭晬閺冨倻纾?Sniroute 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋垮瀭濡わ絽鍟粻娑樏归敐鍛础缂佸鑳剁槐鎾诲磼濮橆兘鍋撻悜鑺ュ€块柨鏇氱劍閹冲矂姊绘担鍛婂暈闁荤喆鍎抽幑銏ゅ箳濡も偓閻掑灚銇勯幒鎴濇灓婵炲吋鍔栫换娑㈠矗婢跺苯鈪归柣鎾卞€濋弻鏇熺箾閻愵剚鐝旂紒缁㈠幐閸嬫挻绻濆▓鍨灍妞ゎ厼鐗撳畷娲冀椤撶喎浜楀┑鐐叉閸ㄥ湱澹曢挊澹濆綊鏁愭径瀣敪闂佽绻嗛弲鐘诲蓟閿涘嫧鍋撻敐搴′簽闁靛棙甯炵槐鎺撴綇閵娿儲璇為梺璇″櫘娴滄粓鍩㈡惔銈囩杸闁哄倹顑欏Σ閬嶆⒒娴ｈ櫣甯涢柟鍛婃倐瀹曨垶骞橀鑹版憰闂佸搫娲ㄩ崰搴ｆ閻愬绠鹃柟瀵稿仦閻ㄦ垿鏌￠崱娆徯ｇ紒缁樼箞閸╂盯鍩€椤掑倷娌柣锝呭濡楁捇姊绘担鍛婃儓闁活厼顦遍幑銏犫攽閸℃瑦娈鹃梺鍛婎殘閸婏綁鎮㈤崗鐓庢闂侀潧鐗嗛幊搴♀枔閵夈儮鏀介柨娑樺娴滃ジ鏌涙繛鍨偓婵嗙暦閵忋倖鍋傞幖鎼枤閸旂兘姊虹捄銊ユ灁濠殿喖顕划濠氭偐瀹曞洨鐦堟繝鐢靛Т閸婄粯鏅跺☉銏＄厱閻庯絻鍔屾慨鍌涙叏婵犲偆鐓肩€规洖銈搁幃銏㈡偘閳╁啰浜栭梻鍌欑閹诧繝鎳濇ィ鍐ㄧ９闁割煈鍣鏍磽娴ｈ偂鎴炲垔閹绢喗鍋℃繛鍡楃箰椤忊晛霉閻橆偅娅婃慨濠呮缁瑥鈻庨崜褍濮奸梻浣侯焾閿曘儳鎹㈤崒鐐村仼闁绘垼濮ら弲鎻掝熆鐠虹尨鍔熸い锔哄姂濮婃椽妫冨ù銈嗙洴瀹曠喖顢楁径濠冩珬闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噺瀹曟煡鏌涘畝鈧崐娑㈠炊閵娿儺鍤ら梺鍝勵槹閸ㄥ綊寮昏濮婃椽宕崟顕呮蕉闂佺瀛╂繛濠傜暦閿濆绠荤紓浣诡焽閸橀亶姊洪棃娑辨缂佽尪濮ゆ穱濠囨偩瀹€鈧壕濂告煃瑜滈崜鐔风暦婵傚憡鍋勭紓鍫㈠Х閻╁酣姊绘担鐟邦嚋婵☆偂绶氭俊鎾焵椤掑嫭鐓曢悗锝庡亝瀹曞矂鏌涢埡瀣瘈闁诡喗濞婇幆鍥ㄤ繆婵?YouTube/Play 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤缂嶅﹪寮婚悢鍏尖拻閻庨潧澹婂Σ顕€姊哄Ч鍥р偓銈夊窗濡ゅ懎桅闁告洦鍨伴崘鈧銈嗗姦濠⑩偓缂侇喖鐖煎娲箹閻愭彃顬堥梺绋匡工濞尖€愁嚕鐠囨祴妲堟繝濠傚姝囧┑锛勫亼閸婃垿宕濆畝鍕疇闁规崘鍩栧畷鍙夋叏濮楀棗鍘撮柡鈧禒瀣厽婵妫楅弸娑㈡煟韫囨稐鎲鹃柡灞剧洴閸╋繝宕掑鍐ｆ嫲闁诲氦顫夊ú妯兼崲閸繍鍤曞ù鐘差儛閺佸洭鏌ｉ幇顕呮殰缂侀亶浜跺铏规嫚閹绘帒姣愮紓鍌氱Т濡繂鐣烽弶璇炬棃宕ㄩ闂寸暗闂備礁鎼ú銊╁窗閸℃顩叉繝濠傜墛閻撴瑥霉閻撳海鎽犳繛鎳峰厾鐟邦煥閸曨厾鐓€濡炪値鍙€閸庡藝閸愬樊鐔嗛柣鐔哄椤ョ姵淇婇崣澶婂妤犵偞甯掕灃闁逞屽墴閹偤宕归鐘辩盎闂佸湱鍎ら崹鐢割敂閳哄偆娈介柣鎰典簻閻忣亞绱掔紒妯兼创妤犵偛顑夐幃娆撳垂椤愩垺鐝梻?
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

    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珕闂佽姤锚椤︻喚绱旈弴銏♀拻濞达絽鎲￠崯鐐烘煕閺冩挾鐣电€规洑鍗冲浠嬵敃閵忊晜顥″┑鐘垫暩婵數鍠婂澶嬪亗婵炲棙鎸婚悡娆撴偡濞嗗繐顏╁┑顕嗙畵閺屾盯濡堕崱娆愬櫘缂備浇椴哥敮锟犲箖閳哄懏鍤戞い鎺戝€愰妸鈺傗拺閻犲洠鈧櫕鐏嶅銈冨妼閹虫﹢宕洪妷锕€绶炲┑鐐靛亾閻庢椽姊洪幆褎绂嬮柛瀣閹﹢鍩￠崘顏嗭紲濡炪倖妫侀崑鎰櫠閵忋倖鐓冪憸婊堝礈閵娧呯闁糕剝绋戠粣妤佷繆閵堝懏鍣洪柛瀣€块弻锟犲炊閵夈儳浠肩紓浣哄Ь椤鎹㈠☉銏℃櫜閹肩补鈧剚娼婚梻浣筋嚙濞存碍绂嶉鍫濈畺婵☆垵娉曢悿鈧梺闈涳紡閸涱喗顔撻梻鍌欑閹碱偊鎯夋總绋跨獥闁哄稁鍘归埀顑跨閳藉顫滈崱妯哄厞婵＄偑鍊栫敮濠囨倿閿曞偆鏁婇柟鐑橆殕閻撶喖骞栧ǎ顒€鈧倕顭囬幇顓犵闁告瑥顦遍惌鎺斺偓瑙勬磻閸楀啿顕ｆ禒瀣垫晣婵犙勫劤娴滄儳霉閿濆懎顥忔繛绗哄姂閺屽秷顧侀柛鎾跺枛閻涱噣宕橀…鎴犲弳闂佸壊鍋呯换鍌炴嚀閸喒鏀介柍钘夋閻忋儳鈧娲熷褍宓勫┑顔姐仜閸嬫挻鎱ㄦ繝鍌ょ吋鐎规洘甯掗～婵嬵敄閽樺澹曟俊鐐差儏濞寸兘鎯岄崱妞尖偓鎺戭潩閿濆懍澹曟俊銈囧Х閸嬬偤鎮ч悩姹団偓渚€寮村杈┬㈤梻浣规偠閸庢椽宕滈敃鍌氭瀬鐎广儱顦伴悡鐔兼煙闁箑鐏犻柣銊︽そ閺岋紕浠﹂悾灞解叺濠殿喖锕ㄥ▍锝呪槈閻㈢宸濇い鏂惧嫎閳ь剚鍔曢—鍐Χ鎼粹€茬凹濠电偠灏欓崰鏍х暦濞差亜鐒垫い鎺嶉檷娴滄粓鏌熸潏鍓хɑ缁绢厼澧庣槐鎺楀箛椤撶啿鎷瑰銈冨妸閸庣敻骞冨▎蹇ｅ晠妞ゆ梻鍘ф竟搴ㄦ⒒娴ｈ姤銆冪紒鈧笟鈧畷鎰板锤濡も偓缁犵娀鏌ｉ幇顒佹儓閸烆垶姊洪幐搴⑩拹闁稿骸鍟块娆撴倻濡偐鐦堥梺姹囧灲濞佳勭墡婵＄偑鍊栧褰掓偋閻樿尙鏆﹂柡澶庮嚦閺冨牆宸濇い鏃囧Г閻濄劑姊绘担钘夊惞闁哥姵鍔楅崚鎺戭吋閸滀胶鍞靛┑顔姐仜閸嬫捇鎸婂┑瀣厱婵°倕鍟埀顒婄秮閺佹劖寰勬繝鍕澑婵＄偑鍊栭崹鐔煎疾濠婂吘娑㈠礃閵娧勬闂侀潧艌閺呪晠寮繝鍥ㄧ厵妞ゆ牕妫楅崯浼搭敊閹烘梻纾介柛灞捐壘閳ь剛鍏橀幊妤呭醇閺囩喐鍎柟鍏肩暘閸斿矂宕掗妸銉㈡斀闁稿本绮犻悞鍓х磼閻橀潧鈻堟鐐寸墬濞煎繘宕滆钃辨俊鐐€ら崑鍛洪悢鐓庤摕闁靛ň鏅涘Λ姗€鏌涢…鎴濇灀闁圭鍟扮槐鎺楁倷椤掆偓閸斻倝鏌涘顒夊剱缂佸矁椴哥换婵嬪炊椤儸鍥ㄧ厱閻忕偛澧介悡顖氼熆鐟欏嫭绀冨ǎ鍥э躬閹瑩顢旈崟銊ヤ壕鐟滃繒鍒掓繝姘闁归鑳堕崝鐑芥偡濠婂嫭顥堟鐐插暞閵堬綁宕橀埡浣插亾婵犳碍鐓犻柟顓熷笒閸旀粍绻涢崨顔剧煁缂佺粯绻堟慨鈧柨婵嗘噽閸橆偊鏌﹂崘顔绘喚闁哄矉绻濆畷顏呮媴缁嬫娼婚梻浣哥枃椤宕归崸妤€绠栭柍鍝勫暞鐎氭氨鎲稿鍡楊嚤闁规壆澧楅埛鎴︽煕濠靛嫬鍔氶柡鍡涗憾閺岋繝宕ㄩ姣裤垽鏌ｉ敐鍥у幋闁诡喒鏅濋幏鐘诲箵閹烘垶鏆忓┑锛勫亼閸婃牠骞愭ィ鍐ㄧ柈闁秆勵殔缁狀垱绻涘顔荤凹闁抽攱鍨块弻娑㈡晜鐠囨彃瀛ｉ梺缁樺笧閸嬫挾鎹㈠☉姘ｅ亾閻㈡鐒鹃柛妯绘尦閺屾盯鍩為崹顔煎閻庡灚婢樼€氫即鐛崶顒夋晣闁绘ɑ褰冪粻璇测攽閻樺灚鏆╅柛瀣Х娴滄悂顢旈崼顐ｆ櫔閻熸粍妫冮獮鍐潨閳ь剚淇婇悜钘夌厸闁稿本绮岀敮楣冩⒒娴ｈ鍋犻柛搴灦瀹曟繄浠﹂悙顒€寮块柣蹇曞仒閸曟ɑ绂嶅鍫熺叆闁哄洦顨呮禍鍓х磽閸屾氨孝闁挎洦浜滈锝嗙節濮橆厼浜滅紒鐐妞存悂寮查埡鍛拺闁绘挸瀵掑鐔兼煕婵犲喚娈滈柟顕嗙節楠炴鈧潧鎽滈惁鍫㈢磼閸撗冾暭闁挎艾顭胯閻擄繝寮婚悢纰辨晩闁绘挸绨堕崑鎾诲垂椤曞懎娈ㄥ銈嗗姧缁犳垹澹曢崗鑲╃闁瑰鍋涢崵顒佺箾閺夋垵顏柡宀嬬稻閹棃顢涘鍛咃綁姊虹粙娆惧剱閻㈩垽绻濆畷娲焵椤掍降浜滈柟鍝勭Х閸忓苯顭胯閻忔岸骞堥妸锔剧瘈闁告洦鍘肩粭锟犳⒑閻熸澘妲婚柟铏悾鐑藉Ω閿斿墽鐦堥梺鍛婂姂閸斿本绔熷鍥╃＝闁稿本鑹鹃埀顒勵棑缁牊绗熼埀顒冩闂佸搫顦伴娆撴倿閼恒儳绠鹃柛鈩兠悘銉р偓娈垮枟閻撯€愁潖缂佹ɑ濯撮柣鐔煎亰閸ゅ鈹戦悙鏉戠祷缂佸鎳撻悾鐑藉箣閿曗偓缁犲鎮归崶顏勭毢妞ゃ垹鎳樺缁樼瑹閸パ冾潻缂備礁顦遍弫濠氬箖閳ユ枼鏋庨柟鎯х－妤犲洭姊洪崜鎻掍簼缂佽鍊块崺娑㈠籍閸喓鍘介梺鎸庣箓閻楀懘宕ラ銈囩＜閺夊牄鍔屽ù顕€鏌涢埞鎯т壕婵＄偑鍊栫敮鎺斺偓姘煎墰缁牏鈧綆鍋佹禍婊堟煛瀹ュ啫濡块柕鍡樺浮閺屾稑螣閸濆嫯鍩為梺瀹狀潐閸ㄥ潡骞冮埡鍐＜婵☆垰顭烽弫顏呯節绾版ɑ顫婇柛瀣嚇閵嗗啯绻濋崶銉㈠亾娓氣偓瀵噣宕掑Δ鈧禍鐐箾閹寸儐浠鹃柤娴嬫櫆瀹曟煡鏌涢埄鍐姇闁绘挻鐩幃姗€鎮欐０婵嗘闂佸憡姊归悷鈺呭蓟閻旂厧钃熼柕澹懏姣囬梻浣告惈閻妲愰弴鐔侯浄闁挎洖鍊哥粈鍫㈡喐婢跺á锝夊醇濠垫劖瀵岄梺闈涚墕妤犳悂鐛幋锔界厱闁哄啠鍋撻柛銊ф暬閹儳鐣￠柇锔藉缓闂侀€炲苯澧撮柕鍡曠窔瀵噣宕煎┑鍡氣偓鍧楁⒑閸濆嫭宸濆┑顔惧厴钘濋柕濞垮劗閺€浠嬫煟濡櫣浠涢柡鍡忔櫅閳规垿顢氶崨顓炵睄濡ょ姷鍋涚换妯虹暦濮椻偓椤㈡瑩鎳栭埡濠冃ч梻鍌欑婢瑰﹪鎮￠崼銉ョ；闁糕剝绋戠痪褏鎲搁悧鍫濈瑲闁绘挾鍠栭獮鏍庨鈧悞娲煕閻旈绠婚柡灞剧洴閹晠宕橀幓鎺撴嚈闁诲孩顔栭崳顕€宕戦崨顖涘床婵犻潧妫鈺呮煏婢跺牆鐏╁ù婊勫劤閵嗘帒顫濋敐鍛闁诲孩顔栭崰鏇犲垝濞嗘挶鈧礁顫滈埀顒勫箖閵忋倕浼犻柛鏇ㄥ亝鏉堝牊绻濈喊澶岀？闁稿鍨垮畷鎰板冀椤撶喎浜卞┑掳鍊曢幊蹇涘磿濡や降浜滈柡鍐ㄥ€瑰▍鏇㈡煟?
    // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻娑樷槈濮楀牊鏁鹃梺鍛婄懃缁绘帡鍩€椤掆偓缁犲秹宕曢柆宓ュ洭顢涘鍛劶闁诲函缍嗛崑浣圭濠婂牊鐓欓柟浣冩珪濞呭懘鏌ｉ幒鏂垮妺缂佺粯鐩畷妤呭礂閼测晩妲规繝娈垮枛閿曪妇鍒掗鐐茬闁告稑鐡ㄩ幆鐐烘煟閻旂顥嬫い鎰偢濮婄粯鎷呴搹鐟扮濡炪們鍔屽Λ婵嬪Υ閸愵喖閱囬柕澶堝劤椤︻偊姊洪崷顓炲妺闁糕晛鐗撳畷銉р偓锝庡枟閻撴洟鏌曟径鍫濈仾婵炲懎娲弻锝夊箻鐎涙﹩妫冨┑顔硷攻濡炶棄鐣烽妸锔剧瘈閹兼番鍨婚弳顓㈡⒒娴ｄ警鐒炬い鎴濇瀹曟繂鈻庤箛锝呮闂侀潧顭俊鍥€呴悜鑺ュ€甸柨婵嗛娴滅偤鏌涘鈧禍璺侯潖濞差亝顥堟繛鎴ｉ哺鏁堥梻浣规偠閸斿酣寮繝姘卞祦濠电姴鎳愰悿鈧梺瑙勫劤閻°劑鎯佹惔锝囩＝?ASCII闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф闂佸憡鎸嗛崨顔筋啅闂備浇顕х€涒晠鎳濇ィ鍏洦瀵奸弶鎳筹箓鏌涢弴銊ョ仩缂佺姴纾埀顒€绠嶉崕鍗灻洪妶鍡曠箚闁芥ê顦弨浠嬫煟閹邦剙绾ч柛锝堟閳规垿顢欓幆褍骞嬮梺绯曟杹閸嬫挸顪冮妶鍡楀潑闁稿鎸剧槐鎺楁偐閼碱儷褏鈧娲樺ú鐔风暦閿熺姵鍊风€广儱妫涢埥澶愭煃閽樺妯€闁挎繄鍋ら、鏍寠婢跺顥?U+00A9(濠? 闂?U+00B7(闂? 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮村Δ鈧禍鎯ь渻閵堝簼绨婚柛鐔告綑閻ｇ柉銇愰幒婵囨櫔闂佸憡渚楅崹鐗堟叏濞差亝鈷掑ù锝勮濞兼帡鏌涢弴鐐典粵闁伙絽澧庣槐鎾存媴閹绘帊澹曞┑鐘灱閸╂牠宕濆Δ鍛煑闁瑰墽绮悡鏇㈡煏婢舵稓鍒板┑陇妫勯埞鎴︻敍濠靛棔姹楅梺鐟板级閹倿骞冭瀹曠厧鈹戦崼銏″櫙濠德板€楁慨鐑藉磻閹达箑鍨傞柣鎴烆焽閳瑰秴鈹戦悩鍙夌ォ闁轰礁绉甸幈銊ノ熼崸妤€鎽甸梺璋庡倻绐旀慨濠冩そ瀹曨偊宕熼棃娑樺闂備浇宕甸崳锔戒繆閸ヮ剙绠查柕蹇曞Л閺€浠嬫倵閿濆簼鎲炬繛鐓庯躬濮婃椽骞愭惔锝囩暤濠碘槅鍋呴惄顖涗繆閹绢喖绠涢柣妤€鐗冮幏娲⒑閸︻収鐒鹃悗娑掓櫇婢规洟骞栨担鍦幈婵犵數濮撮崯鎵不閹炬番浜滈柡鍥朵簽缁夘喚鈧鍠栭…閿嬩繆閻戣棄唯鐟滄粓宕崶褉鏀介柣鎰煐瑜把呯磼閼艰埖顥夐柨鏇樺灲閺屽棗顓奸崨顓炰憾?
    {
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯骞橀懠顒夋М闂佹悶鍔嶇换鍐Φ閸曨垰鍐€妞ゆ劦婢€閺夘參姊洪柅鐐茶嫰婢ь噣鏌涢悢鍛婄稇闁伙絿鍏樻俊姝岊槷闁稿鎸搁埥澶娾枍椤撗傜凹缂佸倸绉靛蹇涘Ω瑜忛惁鍫ユ⒑濮瑰洤鐏叉繛浣冲啰鎽ラ梻鍌欒兌椤牓顢栭崨顓囨稑鈹戠€ｎ亞鐣?+ 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞鐟滃繘寮抽敃鍌涚厱妞ゎ厽鍨垫禍婵嬫煕濞嗗繒绠婚柡宀€鍠栭獮鍡涘级閸熷啯鎹囬弻娑㈠Χ閸涱収浼冮梺鍝勬湰閻╊垶銆侀弴銏℃櫜闁糕剝鐟辩槐鈺呮⒒娓氣偓閳ь剛鍋涢懟顖涙櫠閸撗呯＜闁绘娅曠亸顓㈡煟閿濆鏁辩紒杞扮矙瀹曘劍绻濋崘鈺冨礁闂傚倷绀侀幉锟犲礈椤掑嫬绀嬫い鎾跺枔缁嬩線姊婚崒姘偓鎼佸磹妞嬪海鐭嗗〒姘ｅ亾妤犵偞鐗犻、鏇㈠Χ閸℃ぞ绮℃俊鐐€栭崝褏绮婚幋鐘差棜闁秆勵殔缁狙囨煕椤愶絿顣叉繛鍛喘閺岋綁顢橀悙鎼＆闂佸搫鏈惄顖炲春閸曨垰绀冩い蹇庣椤矂姊绘担铏瑰笡闁规瓕顕х叅闁绘梻鍘ч拑鐔衡偓骞垮劚椤︻垶鎮″☉妯忓綊鏁愰崨顔兼殘闂佽绻戦幐鎼佸煘閹达附鍊烽柤纰卞墮椤も偓婵＄偑鍊ら崢鐓幟洪銏犵畾闁逞屽墯閵囧嫯绠涢幘璺侯暫缂備胶濮甸悡锟犲蓟閻旂顕遍悗娑櫳戦柨顓炩攽閻愯尙澧遍柛娆忓暙椤繘鎼归崗澶婁壕闁革富鍘兼牎濡炪們鍎遍ˇ浼村Φ閸曨垱鏅滈柣鎰靛墯濮ｅ牓姊虹紒妯圭繁闁哥姵鐗犻敐鐐差煥閸繄鍔﹀銈嗗坊閸嬫捇鏌ｉ敐鍥у幋闁轰焦鎹囬幃鈺呭礃濞村浜鹃柣鎴ｅГ閻撳啴寮堕悙闈涱暭妞わ箑鍢查…璺ㄦ喆閸曨剛顦ラ梺瀹狀潐閸ㄥ潡骞冨▎鎾崇骇闁瑰濮存慨鍏肩節閻㈤潧浠ч柛妯犲喚鐒介柨鐔哄Т閺勩儵鏌嶈閸撴岸濡甸崟顖氱闁挎繂妫涢妴濠勭磽娴ｇ懓濮堟慨濠傜秺閵嗗啴濡烽埡鍌氣偓椋庘偓鐟板閸犳牠宕滈崼鏇熲拺閺夌偞澹嗛ˇ锕傛煕婵犲倹鍋ユ鐐诧躬閹瑩宕崟搴涘姂閺屻劑寮埀顒勫磿閸愬樊鍤堥柣鎴烆焽缁♀偓闂佹眹鍨藉褎鐗庢俊鐐€栧褰掓偋閻樿尙鏆﹂柡澶庮嚦閺冨牆宸濇い鏃囧Г閻濇牠姊绘担绋挎毐闁圭⒈鍋婇獮濠冩償閿濆拋妫滄繝鐢靛У绾板秹鎮￠弴銏＄厓闁宠桨绀侀弳鏇犵磼閸洑鎲鹃柡灞稿墲閹峰懐绮欐惔鎾充壕闁割煈鍣鏍ㄧ箾瀹割喕鎲鹃柡浣告閺屽秷顧侀柛鎾寸〒閸掓帡寮崼鐔蜂缓缂備礁顑堝▔鏇㈡晬濞戙垺鈷戠紓浣光棨椤忓棗顥氭い鎾跺Х娑撳秴螖閿濆懎鏆為柣鎾寸懇閹鎮藉▓璺ㄥ姼闂佸憡鎹囨禍鍫曞蓟閿濆绠虫俊銈呭暟閺嗩偊姊虹拠鈥虫灍妞ゃ劌鎳橀敐鐐测攽鐎ｅ灚鏅濋梺闈涚箳婵兘濡堕鈧缁樻媴閸涘﹥鍎撻梺鍝勭墱閸撴瑧鍙呴梺缁樻椤ユ捇寮抽敃鍌涚厵妞ゆ牜鍋涙禍鐗堜繆缂侇澀閭慨濠勭帛缁绘繃鎯旈垾鏂ユ嫽闂備礁鎲￠幐璇茬暆閹间胶宓侀柛顐犲劚鎯熼梺鎸庢煥婢т粙鎯侀崼銉︹拺闁告稑锕﹂惌鍡涙煟閹虹偛顩紒杈╁仱瀹曞ジ濡烽敃鈧埀顒傛暬閺屾盯鈥﹂幋婵呯凹缂備浇鍩栭悡锟犲箖濡も偓椤繈顢橀悢鍝勫殥闂備椒绱徊鍧楀礂濮椻偓閵嗕線寮撮姀鈩冩珖闂侀€炲苯澧柡渚囧櫍閺岋綁鎮㈤崫銉х厐缂備胶绮崝娆撳箖妤ｅ啯鍊婚柦妯侯樀閸炲爼姊洪崫鍕窛闁哥喎娼￠獮蹇撁洪鍛嫼闂佺粯鎸哥€垫帒顭囬悢鍏肩厱濠电姴鍟粈鍐磼閸屾稑娴柡浣稿暣瀹曟帒鈽夊顒€绠為梻鍌欒兌閹虫捇鎮洪妸褎宕查柛鏇ㄥ幘閻捇鏌ц箛鎾冲辅闁稿鎸鹃幉鎾礋椤掑偆妲伴梻浣规偠閸斿繐鈻斿☉銏╂晪闁靛鏅涚粈瀣亜閹哄秶顦﹂柛鏂挎噹閳规垿鎮欓弶鎴犱桓濡炪們鍔屽Λ婵嬪箚閸絿鐤€婵炴垶鐟ч崢浠嬫煙閼圭増褰х紒杈ㄦ礋閹箖顢橀姀锛勫幈闁瑰吋鐣崝瀣箟妤ｅ啯瀵犳繝闈涙储娴滄粓鏌″鍐ㄥ闁活厼顑夐弻娑滅疀閺冨倻鐓夐梺鍝勮嫰缁夌兘篓娓氣偓閺屾盯骞樼捄鐑樼€诲銈庡亜缁绘劗鍙呭銈呯箰鐎氼亞妲愰崼鏇熲拺闁告稑锕ユ径鍕煕閹惧鎳呯紒顕嗙到閳藉濮€閳锯偓閹锋椽姊洪崨濠勭細闁稿骸宕悺顓㈡⒒閸屾瑧顦﹂幖瀛樼矌缁辩偞绗熼埀顒勬偘椤曗偓瀹曟﹢顢欓挊澶嗗亾閻戣姤鍊甸梻鍫熺⊕閹叉悂鏌ｉ敃鈧悧鍡涘煘閹达富鏁婇柡鍌樺€撶欢鐢告⒑閸涘鑰跨紒鐘崇墪閻ｇ兘濮€閵堝懐顔愭繛杈剧到閸樻粓骞忛搹鍦＝濞达絽澹婇崕鎾绘煕鎼粹槅鐓奸柨婵堝仱閺佸啴宕掑☉姘箞濠电姷鏁告慨鎾疮椤栨粎鐭嗛柍褜鍓欓—鍐Χ閸℃鈹涚紓浣瑰絻濞硷繝鐛幋锕€绀嬫い鏍嚤閳哄啯鍠愰煫鍥ㄧ⊕閺咁剚绻濇繝鍌滃闁绘挻绋戦…璺ㄦ崉閻氭潙濮涙繝鈷€鍕伌闁哄本鐩顒勫箰鎼淬垹闂紓鍌欑贰閸犳牠鈥﹂悜钘夌畺婵炲棙鎸婚崐缁樹繆椤栨粌甯舵鐐茬Ч濮婄粯鎷呴悷鏉垮Б濠电偛鐡ㄥ畝绋跨暦閹达箑宸濇い鏍ㄧ懅閸撱劑姊婚崒姘卞闁兼澘鈧喆浜归柟鐑樻尭娴滃ジ姊洪崨濠佺繁闁搞劍濞婂鎶藉Χ婢跺鎷虹紒缁㈠幖閹冲繘鎮甸鍫熺厽婵°倕鍟埢鍫⑩偓瑙勬礃濡炰粙寮幘缁樺亹闁肩⒈鍓﹀Σ浼存⒒娴ｅ摜鏋冩俊妞煎姂閹偤鏁冮崒銈嗘櫆?All rights reserved"
        // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰亜閺冨倹鍤€濞存粓绠栭弻娑㈠箛闂堟稒鐏堝Δ鐘靛亼閸ㄧ儤绌辨繝鍥ㄥ€烽悗娑欘焽椤︺儵姊洪崷顓涙嫛闁稿锕崹楣冩晝閸屾鈺呮煃鏉炴壆顦︾紒鎰☉椤啴濡堕崨顖滎唶闁诲孩鍑规禍婵堢博閻旂厧鍗抽柣鎰ㄦ櫆閺傗偓婵＄偑鍊栧濠氭偤閺冨牊鍊垮ù鐘差儐閻撴洘淇婇婊冃㈤柛鐘冲哺瀹曠敻寮撮悢缈犵盎闂佸搫鍟崐濠氬箺閸屾稓绠鹃柛顐ｇ矌閻瑩鏌″畝瀣М妤犵偛娲、妤佹媴閸欏浜為梻鍌欒兌椤牏鈧稈鏅涢～蹇氥亹閹哄鍋撻崘顔嘉ㄩ柍鍝勫€搁埀顒傚厴閺屾稑鈻庤箛锝喰﹂梺鍝勵儑婵挳鈥旈崘顔嘉ч柛鎰╁妿娴犻箖姊洪悜鈺傛珦闁搞劏娉涢锝夘敃閿曗偓缁犳盯鏌℃径濠勪虎闁兼澘鐏濋埞鎴﹀煡閸℃浠紓渚囧櫘閸ㄥ啿鈻庨姀銈庢晝闁冲灈鏅滈弬鈧梻浣虹帛閿氶柣蹇斿哺瀵娊鍩￠崨顔惧幈闁诲函缍嗛崜娆愮鏉堚斁鍋撶憴鍕缂傚秴锕ら悾閿嬬附缁嬪灝宓嗛梺缁樺姈椤旀牠宕崼鏇熲拻濞达絽鎲￠崯鐐电磼閸濆嫭鍋ョ€规洘娲熼獮搴ㄦ嚍閵夈儱澹掗梻浣烘嚀婢х晫鍒掗鐐村亗婵炲棙鍨圭壕钘壝归敐澶嬫锭闁诲繆鏅滅换娑㈠川椤曞棜鈧潡鏌＄仦绋垮⒉闁瑰嘲鎳樺Λ鍐煛閸曨偅鍎撻梻鍌欑閹碱偊鎯夋總绋跨獥闁哄稁鍘肩粻鏍喐閺傝法鏆﹀┑鍌氭啞閸嬪嫰鏌涘┑鍡楊仼婵炲牄鍎靛濠氬磼濞嗘埈妲梺纭咁嚋缁绘繈骞冮悜钘壩ㄩ柍鍝勫€瑰▍鍥ь渻閵堝懐绠伴柣妤€妫濋幏鎴︽偄閸忚偐鍘介梺鍝勫暙閸婂摜鏁崜浣虹＜闁绘ê纾崣鈧梺鍝勭灱閸犳劗鈧絻鍋愰埀顒佺⊕钃遍梺娆惧幖閳规垿鎮欓懠顒€顤€闂佸憡姊瑰ú鐔煎春閻愬搫绠ｉ柨鏃囨閳ь剛鍏樺濠氬醇閻旇　妲堟繛瀵稿У閿曘垹顫忛搹鍦＜婵☆垳鍘ч埅褰掓⒑闂堟稓澧涢柟顔煎€块幃浼搭敋閳ь剙鐣峰鈧、妯侯煥閳ь剙效濡ゅ懏鈷戦柛锔诲幖閸斿鏌涢妸銉хШ鐎规洘鍔欓幃娆撴倻濡厧骞楅梻浣虹帛椤牆鐣烽悽鍨潟闁绘鐗忕粻楣冩煕濠婂啫鏆熺紒瀣吹缁辨帡宕掑☉妯昏癁閻庢鍠栭悥濂哥嵁鐎ｎ噮鏁囬柣鏃堫棑濡诧綁姊婚崒娆戠獢婵炰匠鍥ㄥ亱闁糕剝銇傚☉妯锋瀻闁瑰瓨绻嶅Λ婊勭節閻㈤潧校缁炬澘绉归幃锟犲即閻斿墎绠氶梺闈涚墕鐎氼噣藝閿曞倹鐓冪紓浣股戠粈瀣煛?闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涙川缁♀偓闂佺鏈划宀劼烽埀顒勬⒒娴ｄ警鐒炬い鎴炲灩閹广垹鈹戠€ｎ亣鎽曢梺鍝勬储閸ㄥ綊鏌嬮崶銊х瘈闁割煈鍋勬慨鍫ュ极閸儲鈷掑ù锝堟鐢盯鏌熼崨濠冨€愮€规洘鍔欏畷鐑筋敇閻斿嘲绨ユ繝鐢靛█濞佳兠洪妸褍濮柍褜鍓熷铏圭矙閹稿孩鎷遍梺鍛婂灱椤绮嬮幒鏂哄亾濞戞瑯鐒界紒鐘荤畺閹鈽夊▎妯煎姺闂佹椿鍘奸鍥╂閹烘鏁婇柤鎭掑劚绾炬娊鎮楀▓鍨灕妞ゆ泦鍥х叀濠㈣埖鍔曢～鍛存煃閳轰礁鏆為柣婵囨礋濮婃椽鎳￠妶鍛呫垺绻涚拠褏鐣抽柕鍥ㄥ姍瀹曟﹢顢旈崱鏇犵憹闂備焦鍎崇换鎰耿鏉堚晛顥氶柦妯侯棦瑜版帗鏅查柛銉ｅ妼濞堝本绻濆▓鍨灈闁绘锕俊鐢稿礋椤栨鈺呮煏婢舵稓鐣辨繛鍫燁殜濮婅櫣鍖栭弴鐔告緭闂佺瀛╄ぐ鍐敋閵夆晛绀嬫い鏍ㄦ皑閻も偓婵＄偑鍊栫敮鎺楀磻閸℃稑鐤悗锝庡枟閻撶喖骞栧ǎ顒€鈧倕顭囬幇顓犵闁告瑥顦辨晶顏堟煃缂佹ɑ顥堥柡浣稿暣瀹曟帒鈽夊▎鎰礁闂傚倷鑳剁涵鍫曞礈濠靛枹鍝勵煥閸涱垯绗夊┑顔筋焾閸╂牠鍩涢幋锔界厾婵炴潙顑嗛弶褰掓煙閸戙倗绡€闁哄本鐩獮娆撳礋椤撶姵娈奸梻浣筋嚃閸犳岸宕戦妶澶婄畺婵炲棙鎼╅弫鍕煕濞嗗秴鍔ら柣鈺侀叄濮婄粯鎷呴崨濠冨創濠电偞娼欓崐鍦矉瀹ュ鍊烽柣銏㈡暩閻撳顪冮妶鍡樼５闁稿鎹囬弻锛勪沪閸撗勫垱闂佺硶鏅濋崑銈夌嵁閸℃凹妲烘繝銏ｎ潐濞叉牠鍩為幋锔芥櫖闁告洦鍋傞弶顓㈡⒑缁嬪尅鏀婚柣妤佹礋椤㈡瑨绠涢弮鍌滅槇濠殿喗锕╅崜娑㈡晬濞戙垺鈷戦柣鐔告緲閳锋梻绱掗鍛仸闁诡噯绻濆畷姗€顢欓悾灞藉箺闂備胶绮弻銊╁箟閿涘嫭顐介柡灞诲劜閻撴盯鏌涢埥鍡楀箻缂佸绮妵鍕Ω閿濆懎濮﹂梺璇″枟閻熲晛鐣烽敐鍡楃窞濠电姴鍟版禍宄扳攽閿涘嫬浜奸柛濠冨灴瀹曠懓煤椤忓懎浠梺鍐叉惈閹冲海绮婚弽顓熺厱闁圭偓顨呴崐褰掑礋閸愵喗鈷戠紓浣姑慨锕傛煕閹垮嫮鐣电€?闂?
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
            // 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞妞ゆ帒顦伴弲顏堟偡濠婂啰效婵犫偓娓氣偓濮婅櫣绱掑Ο铏逛紘濠碘槅鍋勭€氭澘顕ｉ崨濠勭懝闁逞屽墴瀵鎮㈤崗灏栨嫽闁诲酣娼ф竟濠偽ｉ鈧铏圭矙閼愁垼娼堕梺鍛婃煥闁帮綁鐛径鎰濞达絽鎲￠ˉ婵嬫煟閻樺厖鑸柛鏂跨Т閳绘挻銈ｉ崘鈹炬嫼濠殿喚鎳撳ú銈夋倶閳轰緡鐔嗛柣鐔峰簻闊剛鈧鍣崑濠傜暦閸洦鏁嗗ù锝呮惈楠炴姊绘担鍛婃儓闁哥噥鍋勭叅闁靛繈鍊曢悞鍨亜閹达絾顥夊ù婊堢畺濮婄粯鎷呴崨濠冨創濡炪倖鍨靛Λ婵嬪箖瑜旈幃鈺冩嫚閼碱剛鏆繝寰锋澘鈧洟骞婇幇顒傛殼濞撴埃鍋撻柡灞剧洴閳ワ箓骞嬪┑鍥╀邯濠电姵顔栭崰鏍煀閿濆钃熼柨婵嗘閸庣喖鏌曡箛鏇炐㈡慨锝呯墦濮婃椽宕妷銉愶絿鈧厜鍋撻柟闂磋兌瀹撲線鐓崶銊ユ惛濞存粌缍婇弻鐔兼倻濡櫣浠奸梺鍛娚戦幃鍌氼潖濞差亜宸濆┑鐘辫兌缁讳線姊洪崫銉バｉ柣妤侇殘缁骞掑Δ濠冩櫖闂佺粯鍔栧娆愮椤撶偐鏀介柣妯款嚋瀹搞儵鏌涢悢鍛婂唉鐎规洩缍佸畷鐔碱敍濞戞艾骞嶆俊鐐€栭弻锝夊礉鐏炵煫娲Χ婢跺﹦顢呴梺瑙勫劶婵倝鎮¤箛娑氬彄闁搞儜宥嗘暰濠电偛鎳庡Λ婵嬪蓟閿熺姴妞介柛鎰典簽琚﹂梻浣筋嚃閸犳牠宕查弻銉⑩偓锕傚Ω閳轰礁绐涘銈嗘寙閸曨剛妲橀梻鍌欑劍閻綊宕洪崟顖氬瀭閺夊牃鏅滈弳婊堟煟閹扮増娑ч柛銊︾箓閳规垿鎮╁畷鍥舵殹闂佸搫鎳忛悡锟犲蓟濞戞﹩鐓ラ柍褜鍓熼幃鐐烘晜闁款垰浜鹃梻鍫熺〒閻掓悂鏌″畝瀣ɑ闁诡垱妫冮、娑橆煥閸涘拑绲剧换娑氣偓娑欘焽閻绱掗鑺ュ磳妤犵偛妫欑粭鐔煎焵椤掍椒绻嗛柟闂寸鍞梺鍛婂姂閸斿海绮斿ú顏呯厵妞ゆ梻鎳撴晶鏌ユ煙椤栨稒顥堥柛鈺嬬畵閸┾偓妞ゆ帒瀚壕濠氭煙閹规劦鍤欓梺瑁ゅ€栨穱濠囧Χ閸屾矮澹曢梻浣芥閸熶即宕伴弽顓炶摕闁哄洨鍠撶粻楣冩煕椤垵鏋涘┑顔碱槸閳规垿鍩勯崘銊хシ闂佺粯顨呴敃锕傚箲閵忕姭妲堥柕蹇曞Т閼板灝鈹戦埥鍡楃仩婵炲瓨纰嶇粋宥夘敂閸曨厽娈惧┑鐘诧工閹虫劗浜搁悽鍛婄厵?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍讲鏋欓梺缁樺笩婵倗鎹㈠☉銏犲耿婵☆垵顕х喊宥夋煟閻斿摜鎳曠紒鐘虫崌楠炲啫螖閸涱喗娅滈柟鑲╄ˉ閳ь剝灏欓弫鏍⒒娴ｅ摜锛嶉柟铏尵缁棃鎮介崹顐㈠簥濠电偞鍨堕敃鈺呭疮閸涱喓浜滈柡鍐ㄦ搐閸氬綊鏌ｉ埡渚€鍙勬慨濠冩そ瀹曘劍绻濇担宄邦劉婵＄偑鍊ら崑鍕囬柆宥呯闁靛繒濮弨浠嬫煕閵夈垺娅囬柣锕€鐗撳铏圭矙閹稿孩鎷辩紒鐐緲缁夊綊骞冨鈧獮妯肩磼濡攱瀚奸梻浣告啞缁哄潡宕曢柆宥呭嚑閹兼番鍔嶉悡娆撴煣韫囷絽鈧顢旈崼顐ｆ櫔闂佹寧绻傞ˇ顖炴煁閸ヮ剚鐓涢柛銉㈡櫅娴犻亶鏌涢悙鑸电【闁宠鍨块幃娆撳级閹寸姳妗撻梻浣瑰绾板秹濡甸崟顖ｆ晢闁逞屽墴钘濋梻鍫熶緱濞兼牗绻涘顔荤盎濞磋偐濞€閺屾盯寮撮妸銉モ拻濠碘槅鍋嗛崑銈咁潖閾忓湱纾兼慨妤€妫欓悾鐑芥⒑閸︻厽鍤€婵炲眰鍊濋、?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾圭€瑰嫭鍣磋ぐ鎺戠倞鐟滃繘寮抽敃鍌涚厱妞ゎ厽鍨垫禍婵嬫煕濞嗗繒绠婚柡宀嬬秮婵偓闁靛繆鏅濋崝鍝ョ磽娴ｆ彃浜炬繝銏ｆ硾閺堫剟宕伴幇鎵彄闁搞儯鍔嶇粈鈧梺鎼炲妽缁诲倿鍩為幋锔藉亹閻庡湱濮撮ˉ婵堢磽娴ｅ搫顎撶紓宥勭窔瀵濡搁妷銏℃杸闂佺硶鍓濋〃鍡椻枔閵娿儮鏀芥い鏃€顑欏鎰亜閵娿儻宸ユい顐㈢箻閹煎綊宕烽鐘靛帬闂備胶纭堕崜婵嬨€冮崨鎼晜閻庢稒顭囩粻楣冩煕椤愩倕娅忛柣鎺嶇矙閺屽秷顧侀柛鎾寸箞閿濈偞寰勯幇顒傤唶婵＄偛顑呯€涒晠顢曢懞銉﹀弿婵☆垱瀵х涵鍓х磼閻樺磭澧遍柟鍙夋倐閹囧醇濠靛牏鎳嗗┑鐐殿棎閸嬫劖绻涢埀顒勬煛鐏炲墽娲寸€殿喗鎸虫俊鎼佸Ψ閵夘喗娴囬梻鍌欒兌椤牏鈧稈鏅犻獮鎴﹀炊瑜滃?YAML闂?
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
        // winget 闂?闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪Ч濠€渚€姊虹紒妯虹伇婵☆偄瀚划濠氭偐缂佹鍘甸梺纭咁潐閸旓箓宕靛▎鎴犵＜闁绘﹢娼ч弸娑欐叏婵犱胶鐭欑€规洖鐖奸、妤呭焵椤掍緡鏀伴梻鍌欑濠€杈ㄦ櫠濡も偓椤灝螣閼测晝鐒奸梺鍛婂姀閺傚倹绂嶅鍫熺厸闁告劑鍔嶆径鍕亜閵夈儺妲搁棁澶愭煟濡櫣婀撮柛瀣ㄥ灪閹便劍绻濋崘鈹夸虎濠碘槅鍋勯崯顐︽箒闂侀潧鐗嗗Λ娑樏虹€电硶鍋撶憴鍕闁哥姵鐗犻妴渚€寮撮姀鈩冩珳闂佺硶鍓濋悷銉╂嚄閸洘鈷掗柛灞捐壘閳ь剚鎮傚畷鎰槹鎼达絿鐒兼繛鎾村焹閸嬫挻顨ラ悙宸█闁糕晛瀚板畷姗€顢旈崱妯烘疄濠电姴鐥夐弶搴撳亾閺囥垹绠犻幖杈剧悼閻棝鏌涢銈呮灁缂佺娀绠栭弻鐔衡偓娑欘焽閹冲啴鏌ｈ箛锝勯偗闁哄本绋撻埀顒婃€ラ崘銊㈡嫟闂備線娼уú銈団偓姘嵆閻涱噣骞掑Δ鈧粻锝嗙節閸偄濮冮柣銉邯濮婄粯鎷呴搹鐟扮闂佸湱顭堥…鐑藉箚閸曨垼鏁嶉柣鎰蔼閹芥洖鈹戦悙鏉戠仧闁搞劌婀辩划璇测槈閵忥紕鍘遍棅顐㈡处閹稿骞夐幖浣圭厽闁宠桨绶氶崣鍕煛鐏炵硶鍋撻幇浣告倯婵炶揪绲捐ぐ鍐╃妤ｅ啯鐓涢柛灞久崝婊堟煛閸屾浜鹃梻鍌氬€烽懗鍓佸垝椤栫偛绀夐柡宓瞼鍔峰┑鐐叉閸ㄧ數娆㈤妶鍥╃＝濞达綀顕栭悞鐣岀磼閻樿崵鐣洪柟顔筋焾缁犳盯寮崒姘ｅ彠闂備胶顭堥鍡涘箲閸ヮ剛宓侀柛銉ｅ妽婵挳鏌涘☉姗嗙叕闁?闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涙川缁♀偓闂佺鏈划宀勩€傚ú顏呪拺闁芥ê顦弳鐔兼煕閻樺磭澧电€殿喖顭峰鎾偄閾忚鍟庨梻浣稿閻撳牓宕伴弽銊х彾闁告洦鍋€閺€浠嬫煟閹邦剙绾ч柍缁樻礀闇夋繝濠傚閻帞鈧娲橀敃銏ゅ春閳ь剚銇勯幒鍡椾壕濡炪値浜滈崯瀛樹繆閸洖骞㈡俊顖滃劋濞堫偊姊绘笟鈧鑽ゅ緤婵犳艾纾块弶鍫亞娴?0闂?1978335189闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢亸浣圭€婚梺鐐藉劚绾绢厼锕㈤幘顔界厱闁哄啠鍋撻柛銊ョ仢閻ｅ嘲煤椤忓嫮鍔﹀銈嗗笂闂勫秵绂嶅鍫熺厵闁绘垶锚閻忋儵鏌嶈閸撴繂鐣烽崹顐ょ彾闁哄洨鍠撻梽鍕磼鐎ｎ厽纭跺Δ鐘叉喘濮婃椽宕滈幓鎺嶇凹缂備礁顑嗛崹鑸电┍婵犲嫭宕夐柕濞у拑绱查梻浣虹帛閿曘垹顭囪缁傛帡鏌嗗鍡欏幐閻庡厜鍋撻悗锝庡墰閿涚喐绻涚€电顎撶紒鐘虫崌楠炲﹪鎮欓崫鍕庛劑鏌曟径娑橆洭濞存粎鍏樺濠氬磼濞嗘劗銈板┑鈩冦仠閸旀垿骞冮悙鐑樻櫇闁稿本姘ㄩˇ顔尖攽閻愬弶顥滅紒缁樺浮瀵娊鏁冮崒娑氬幗闂佸搫鍟崐鐢稿箯閿熺姵鐓涘ù锝呮啞閸犳﹢鏌″畝瀣埌閾伙綁鏌涘┑鍡楊仾婵絾鍔欏娲传閸曢潧鍓遍梺鍦嚀濞层倝锝炶箛鏃傜瘈婵﹩鍓涢敍婊冣攽閻愬弶顥為柛鈺佺墕鍗辨い鏇楀亾婵﹥妞藉畷鐑筋敇濞戞瑥鐝遍梻浣侯焾椤戝懎螞濠靛鏄ラ柍褜鍓氶妵鍕箳瀹ュ洤濡介梺姹囧妽閸ㄥ潡寮诲☉婊呯杸闁哄倸妫禍顏堝箖閻愵剦妲归幖娣灩閺嬪倿姊洪崨濠冨闁稿甯″畷鐢稿Χ婢跺鎷洪梺鍛婄箓鐎氬嘲危瑜版帗鐓曢柕鍫濈箳閻ｇ敻鏌℃担绋挎殻鐎规洘甯掗埥澶婎潩椤掆偓缁犮儵姊绘担绛嬪殭婵炲鍏樺顐﹀箹娴ｆ祴鍋撻敃鍌氶唶闁绘柨鎼埅浼存⒒閸屾瑦绁版い鏇嗗應鍋撳☉鎺撴珚闁硅櫕绻冮妶锝夊礃閿濆倸浜鹃柨鏇炲€归崐濠氭煢濡警妲奸柟鑺ユ礋濮婃椽宕崟顒€绐涢梺鍝ュТ濞村嘲鈻庨姀銈嗗€烽悗闈涙憸椤旀洟姊洪悷閭﹀殶闁稿濮电粩鐔奉潨閳ь剟寮婚敐澶樻晣闁绘棁娅ｉ悿鍕攽?
        // 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣濞嗗繒鍔撮梺杞扮椤戝棝濡甸崟顖氱閻犺櫣鍎ら悗楣冩⒑閸涘﹦鎳冪紒缁樺姍濠€渚€姊虹粙璺ㄧ闁告艾顑囩槐鐐哄箣閻樺灚锛忛梺璇″瀻閸曨偂娣繝娈垮枛閿曘儱顪冩禒瀣ㄢ偓浣肝旈崨顓犲姦濡炪倖甯掗崐濠氭儗濞嗗繈鈧帒顫濋敐鍛闁诲孩顔栭崳顕€宕戞繝鍌滄殾闁割偅娲栭悡娑㈡煕鐏炲墽顣查柣搴ㄧ畺濮婄粯鎷呴搹鐟扮濡炪們鍔岄幊姗€骞冭瀹曞崬鈻庨幋鐘垫殽闂備礁婀遍崕銈咁潖閼姐倕顥氬┑鍌氭啞閻撶喐绻涢崱妤冪闁革絼绮欓弻娑㈠箳閺傛鏆梺闈涙搐鐎氫即銆侀弮鍫濈妞ゆ劧绲鹃宥夋⒒娓氣偓閳ь剛鍋涢懟顖涙櫠閸撗呯＝鐎广儱鎳忛ˉ鐐电磼閸屾氨孝妞ゎ厹鍔戝畷濂告偄瀹勬澘鈷曢梻鍌欑窔濞佳団€﹂崼銉ョ？鐎广儱鐗勬禍褰掓煙閻戞ɑ灏ù婊勵殜閺岀喖鎳栭埡鍕婂淇婇悪娆忔搐绾惧鏌熼幑鎰靛殭闂佽￥鍊栨穱濠囧Χ閸屾矮澹曢梻浣芥閸熶即宕伴弽顓炶摕闁靛ň鏅滈崑鍕煕濠靛棗鐝旈柕蹇嬪灮绾惧ジ鏌嶈閸撴盯鍩€椤掑﹦绉甸柛鐘愁殜閸╂盯骞掑Δ浣哄幈濡炪倖鍔戦崐鏇㈠几閺冨牊鐓忛柛鈩兠粭鎺楁婢舵劖鐓熸慨妤€妫楁禍婵囩箾閸涱叏鏀婚柟渚垮妽缁绘繈宕ㄩ鍛摋闂備礁缍婇ˉ鎾寸箾閳ь剟鏌涢妸锕€鍔ら柣锝囧厴瀹曞爼鏁傞挊澶屽幈缂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掆偓閺勩儵鏌″畵顔兼噺濞堟洟姊鸿ぐ鎺擄紵闁煎疇娉涢悾鍨瑹閳ь剟寮诲☉銏犵労闁告劦浜栧Σ鍫ユ⒑缂佹ê濮囬柨鏇樺劦婵＄敻宕熼姘鳖唺閻庡箍鍎卞ù鍌毼ｉ鐣岀閻庢稒顭囬惌瀣磼椤旇姤灏い顐㈢箻閹煎綊宕烽鐙呯床婵犵妲呴崹闈涒枍閿濆棛顩风紒瀣儥濞撳鏌曢崼婵嬵€楁鐐寸墬缁绘稑顔忛鐓庣濡炪倖娲╃紞渚€鐛惔銊﹀殟闁靛鍎板Ч妤冪磽閸屾艾鈧兘鎮為敃浣哥稊闂備礁缍婇弨鍗烆渻閽樺娼栨繛宸簻瀹告繂鈹戦悩鎻掝仱婵℃彃鐗撳娲箰鎼淬垻锛橀梺绋匡攻缁诲牓鐛崘銊庣喓绱掑Ο鑲┬ら梻渚€娼ц噹闁告粈绀侀弲顓㈡⒒閸屾瑧顦﹂柟璇х磿閸掓帒鐣濋崟顐ゆ焾闂佽顔栭崰姘舵儗閹炬番浜滈柡鍥殔娴滈箖姊洪崫鍕効缂佽弓绮欓崺銉﹀緞閹邦剛顢呴梺缁樺姇椤曨叀銇愭惔銊︾厽閹兼番鍊ゅ鎰箾閸欏澧甸柟顔哄劦瀹曟姊荤€靛摜鐣炬繝娈垮枟缁诲倿鎯岄崼婢綁鎮滈懞銉㈡嫼闂佸憡绻傜€氱兘宕曢幇鐗堢厽婵°倕鍟瓭濡炪値鍋勭换鎰弲濡炪倕绻愮€氼剛绮ｅ☉娆戠瘈闁汇垽娼у瓭闁诲孩鍑归崳锝夊箖妤ｅ啯鍋ㄩ柛娑橈功閸樹粙鏌熼崗鑲╂殬闁搞劌缍婂畷鐢割敆閸曨剛鍘遍梺瑙勫劤椤曨參骞婇崶顒佺厱闁崇懓鐏濋悘鈺傘亜閹剧偨鍋㈢€规洏鍔戦、鏃堝川椤栨浜惧┑鐘崇閳锋垿鎮峰▎蹇擃仼闁告柣鍊栭妵鍕即閵娿儱绫嶉悗瑙勬穿缂嶄礁鐣烽崡鐑嗘富缂備焦蓱閺夋悂姊绘担铏瑰笡闁告梹鐗曢…鍥р枎閹炬潙鈧爼鏌ㄩ弬娆炬綗濞存粍绮嶉妵鍕箻鐠鸿桨娌梺鍝ュУ閿曘垽寮婚悢纰辨晩闁诡垎鍌涱潟闂備浇顕栭崰妤冨垝閹捐绠犻柣鎰惈鍞悷婊冪Ф閺侇喖螖閸涱喚鍘介梺缁樻煥閹芥粓鎯岀€ｎ剛纾兼い鏃囧Г鐏忣參鏌ｉ敐鍛Щ妞ゎ厹鍔戝畷姗€鏁愰崱娆屽亾婵犳碍鈷戠憸鐗堝笒娴滀即鏌涢敐蹇曞埌妞ゎ剙锕、姘跺焵椤掆偓椤繐煤椤忓嫪绱堕梺鍛婃处閸嬪懎鈻撻銏♀拺缂佸灏呮Λ姘扁偓瑙勬礈閺佹悂宕氶幒鎾剁瘈婵﹩鍓氶ˉ婵嗏攽閻愬弶顥滃褌绮欏畷顒勬偨閸涘ň鎷洪梺绋跨箰閸氬宕愰幇顓犵濞达絽鍟跨€氼參宕ｈ箛娑欑厱闁靛鍠栨晶顖炴煟閹惧瓨绀冪紒缁樼洴瀹曞崬螖閸涱垰妞介梻浣姐€€閸嬫挸鈹戦悩宕囶暡闁绘挸绻橀弻娑㈠Ψ閹存繂鏋ゅù鐓庡暣閹鈻撻崹顔界亞缂備緡鍠楅悷锔界┍婵犲洤绠瑰ù锝堝€介埡鍛厪濠㈣埖锚閺嬬喐銇勯妷锝呪偓鏇㈠煘閹达附鍋愰柛顭戝亝濮ｅ嫰姊虹粙娆惧剳闁稿鍊曢悾鐑芥偨閸撳弶鏅滈梺鍓插亝缁诲嫰宕愰悙鐑樷拺闂傚牊涓瑰☉銏犵闁靛ě灞芥櫏婵犵數濮烽。顔炬閺囥垹纾婚柟杈剧畱绾惧綊鏌￠崶鈺佇ｇ€规洘鐓￠弻鐔兼焽閿曗偓閺嬫捇鏌涚€ｎ偅灏い顐ｇ箞瀹曪繝鎮欓幓鎺旑吅闂佽姘﹂～澶娒哄Ο濂芥椽鎮㈡總澶婃濡炪倖娲嶉崑鎾垛偓瑙勬礀閵堟悂骞冮姀銈呬紶闁告洦鍋嗛悷鏌ユ⒒娴ｈ棄鍚归柛鐘冲姉閸掓帒顓奸崶褍鐏婇梺瑙勫劤绾绢參寮抽敂鎴掔箚闁靛牆鍊告禍鎯ь渻閵堝骸浜濈紒顔芥崌瀹曟椽鍩€椤掍降浜滈柟鍝勬娴滈箖姊烘导娆戞偧闁稿繑锕㈤悰顔锯偓锝庡枟閺呮繈鏌嶈閸撴稓鍒掓繝姘殥闁靛牆娲ㄩ敍婵囩箾鏉堝墽绉い鏇熺墵閹偞绂掔€ｎ偆鍘卞┑顔姐仜閸嬫挸霉濠婂棙纭炬い顐㈢箻閹煎湱鎲撮崟顐㈠箲闂備礁鎲℃笟妤呭窗閺嵮€妲堢憸蹇曟閹惧瓨濯撮柛锔诲幖瀵劎绱撴担鍝勑ｉ柣妤冨█婵″瓨绗熼埀顒€顕ｉ鈧畷濂告偄閾氬倹鐫忕紓鍌氬€搁崐鐑芥倿閿曞倵鈧箓宕堕鈧崒銊ッ归悩宸剱闁抽攱鍨块弻娑樷攽閸℃浼€閻炴凹浜娲川婵犲啰鍙嗛梻浣稿簻缂嶄線鐛崱妤冩殕闁告洦鍋嗛濠囨⒑閻熸壆鎽犳慨濠傜秺瀵剟鍩€?
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
            // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珖闂侀€炲苯澧板瑙勬礃缁绘繂顫濋鐘插箞闂備焦鏋奸弲娑㈠疮娴兼潙鐓″鑸靛姈閻撴洟鎮橀悙棰濆殭濠殿喖鍊块弻锛勪沪鐠囨祴鍋撻弴銏＄畳闂佽鍑界紞鍡涘磻閸涘瓨鍋熼柡鍥╁剱濞撳鏌曢崼婵嬵€楀ù婊勭箖缁绘盯鎳犻鈧弸娑氣偓娈垮枛椤兘骞冮姀銏″仒闁炽儱鍘栨竟鏇㈡⒑濮瑰洤鐏い鏃€鐗犻幃鐐哄箚椤€崇秺閹晛鈻庤箛鏂款槱闂佺粯鎸搁崯鎵閹惧瓨濯撮柤娴嬪墲閸ㄥ潡宕哄☉銏犵婵°倓鑳堕崢鎾绘偡濠婂嫮鐭掔€规洘绮岄埢搴ㄥ箣閻樿京鐟濋柣搴＄畭閸庨亶藝椤栨粌顥氶柛褎顨嗛埛鎴炪亜閹哄棗浜剧紓浣割槸椤曨厾鍒掓繝姘亹閻犲洩灏欓鎰磽娓氬洤鏋︽い顐㈩樀椤㈡捇宕卞Ο鍦畾闂佸湱绮敮鐐存櫠閵忋倖鐓欓柛娆忣槹閸婃劗鈧鍣崳锝呯暦閻撳簶鏀藉┑鐘插暞閸?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煙缂佹ê绗ч柍褜鍓﹂崣鍐潖閸濆嫅褔宕惰娴煎嫬顪冮妶搴″箹闁绘绻掗崚鎺楁嚒閵堝棗鏋傞梺鍛婃磵閺呮瑧鑺辨繝姘拺閻熸瑥瀚ˉ鎰版煛閸涱喚娲寸€规洘绻傝灒濠靛倻鏅幊鎾烩€﹂妸鈺佸耿闁冲搫鍊荤粔椋庣磽閸屾瑨鍏岀紒顕呭灦閹嫰顢涢悙闈涚ウ闁诲函缍嗛崰妤呭磹閻戣姤鐓曢柟鑸妽濞呭懘鏌涘┑濠冾仩闁逞屽墯椤旀牠宕伴弽顓涒偓锕傛倻閽樺鐎梺鍛婂姦閸犳牜澹曢崗鑲╃闁瑰鍋犳竟姗€鎮介娑辨疁婵﹦绮粭鐔煎焵椤掆偓椤洩顦归挊婵囥亜閹惧崬鐏╃紒鈧径灞稿亾閸忓浜鹃梺鍛婃处閸撴瑩宕滈鐐╂斀闁绘劕寮堕埢鏇灻瑰鍕疄闁?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崗鐓庘偓缁樹繆椤栨繃顏犲ù鐘层偢濮婃椽宕崟顒夋濠碘槅鍋呯换鍫濐嚕婵犳碍鍋勯柛蹇撶毞閹峰鏌ｆ惔銊︽锭濠㈢懓锕ゅ嵄妞ゆ洍鍋撶€殿喖鐖奸崺锟犲磼濠婂懐鏉介梻浣告惈閻ジ宕伴弽褏鏆︽俊銈呮噺閸ゅ啴鏌嶉崫鍕偓褰掝敊閸愵喗鈷掑ù锝呮啞閸熺偤鏌＄仦璇插闁诡垰顦甸幊鏍煛娴ｅ摜浜板┑鐘垫暩婵敻鎳濋幆褎鏆滈梻鍌欑閸熷潡骞栭锕€纾归柟闂寸閻掑灚銇勯幒鎴濃偓褰掑汲椤掑嫭鐓涢悘鐐额嚙婵″ジ鏌嶇憴鍕伌鐎规洖宕埥澶愬箥娴ｉ晲澹曞┑掳鍊曢幊蹇涙偂閺囩喍绻嗘い鏍ㄧ矌鐢盯鎮樿箛銉х暤闁哄矉绱曟禒锕傛偩鐏炴縿鍎查妵鍕槺缂佽埖宀稿璇测槈閵忕姷顔掔紓鍌氱墢婵磭鈧稈鏅犲娲捶椤撶姵鎲橀梺绋款儏濡稑危閹版澘绠婚悗娑櫭鎾斥攽椤旂晫绠撻柤娲诲灦椤㈡棃顢橀姀鈥斥偓鐢告偡濞嗗繐顏璺哄閺屾盯骞樼捄鐑樼亪閻庤娲樺浠嬪箖濞嗘挸浼犻柛鏇ㄥ弾閸氬懘姊绘担鐟邦嚋婵炴彃绉瑰畷鎴﹀箻缂佹鍘介梺褰掑亰閸撱劑鐓鍕厸閻忕偟鍋撶粈鍐磼缂佹绠氱紒鐘崇洴瀵挳鎮╅搹顐晠婵犵绱曢崑鎴﹀磹閺囩姵宕查悗锝庡枛绾偓闂佸憡鍔曞Ο濠傤焽?DoH/nginx 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涙川缁♀偓闂佺鏈划宀劼烽埀顒勬⒒娴ｅ憡鎯堥柟宄邦儐缁傚秴鈹戦崼鐔峰簥濠电娀娼ч鍡涘磻閵娾晜鈷掗柛顐ゅ枎閸ㄤ焦绻涢幋娆忕仾闁绘挻娲熼幃妤呮晲鎼粹€茬凹闁诲繐绻嬮崡鎶藉蓟閺囥垹鐐婄憸宥夘敂椤掑倻纾奸柣妯虹－濞插瓨顨ラ悙鎼劷闁圭懓瀚粭鐔碱敍濞戞瑧甯嗛梻鍌氬€搁崐椋庣矆娓氣偓楠炴牠顢曢敃鈧粻顖炴倵閿濆骸鏋涚紒鈧崼銉︾厽闁哄啫鍊哥敮鍓佺磼閻樺磭澧柟渚垮妼椤啰鎷犻煫顓烆棜闂傚倷娴囬鏍窗濡ゅ懏鍋￠柍鍝勬噹缁犵姵绻濇繝鍌氭殜闁哄妫冮弻鐔虹矙閸喚妲板┑鈽嗗亜鐎氭澘顫忔繝姘＜婵炲棙甯掗崢锛勭磽娓氬洤娅橀柛銊﹀▕瀵偊顢氶埀顒勭嵁閹烘嚦鏃堝焵椤掆偓濞插潡姊绘担鍝ョШ婵☆偉娉曠划鍫熸媴閾忛€涚瑝闂佸搫绋侀崢浠嬪磻閳╁啰绠鹃柛鈩冾殘缁犵増銇勮箛锝勭盎闁宠鍨块、娑樷槈濞嗗繐鏀梻浣告惈閺堫剛绮欓幋锕€鐓濋幖娣€楅悿鈧梺鎸庣箓閺屽﹨顦规慨濠呮缁辨帒螣閼测晝鏆梻浣虹帛椤ㄥ牊鎱ㄩ幘顕嗙稏闊洦绋掗崐缁樹繆椤栨繍鍤欐い顐熷墲缁绘稓鈧數顭堢敮鍫曟煟鎺抽崝鎴︺€侀幘璇插唨妞ゆ挾鍟块幏铏圭磽娴ｅ壊鍎忛悘蹇撴嚇瀵劑鎳￠妶鍥╋紲闁荤姴娲╃亸娆愭櫠閿曞倹鐓欐い鏃囶嚙瀹撳棙銇勯姀锛勬噰闁哄被鍔戦、娑橆潩椤愩埄妫滈梻鍌欑劍鐎笛呮崲閸屾侗娈介柟闂寸閸戠娀鏌ｉ弬鍨倯闁绘挻鐟╅幃姗€鎮欓幓鎺嗗亾閹间礁绀夐柨鏇炲€归悡娑㈡倶閻愭彃鈷旈柕鍡樺笒闇夐柣娆忔噽閻ｇ數鈧娲樼划蹇浰囬幎鑺ョ厱闁挎繂楠稿▍宥嗘叏婵犲偆鐓肩€规洘甯掗～婵嬪礂婢剁櫢濡囩槐鎾存媴閸撳弶楔闂佽桨鐒﹂幃鍌炵嵁閹达箑顫呴柣姗嗗亝椤秹姊洪棃娑氱濠殿喚鍏橀、妯好洪鍛嫼缂傚倷鐒﹂敋濠殿喖鐗婄换娑㈠礂閼测晜鍣伴悗娈垮枛閸熻儻鐏冮梺鍛婁緱閸犳帡骞忛搹鍦＝濞达絽澹婇崕鎾寸箾婢跺绀嬬€殿喕鍗冲畷鎺楁倷鐎电骞堝┑鐘垫暩婵挳宕幍顔句笉闁圭偓鐣禍婊堟煛閸ユ湹绨界紒瀣吹缁辨帞绱掑Ο鑲╃暤濡炪値鍋呯换鍫ャ€佸Δ鍛＜婵﹫绲芥禍楣冩煟閹邦喖鍔嬮柍閿嬪灴閺屾盯骞囬鈧痪褔鏌熼姘卞ⅵ闁哄本鐩幃銏㈡崉閸濆嫮銈梻浣芥〃缁€浣虹矓瑜版帒绠栨繛鍡樻尰閸婄粯淇婇鐐存暠閻庢艾銈稿缁樻媴閽樺鎯為梺鍝ュУ閸旀牜绮╅悢鐓庡耿婵炴垶顭囬鎴︽⒑缂佹﹩鐒界紒顕呭灦閹繝鎮㈤崗鑲╁幗闂佸搫鍟犻崑鎾寸箾閸忚偐鎳囬柟顔斤耿閹崇娀顢栭挊澶夊?
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


// 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋婵愭綗闁逞屽墮閸婂潡骞愭繝鍐彾闁冲搫顑囩粔顔锯偓瑙勬磸閸旀垵顕ｉ崼鏇炵闁绘瑥鎳愰獮銏ゆ⒒閸屾瑧绐旈柍褜鍓涢崑娑㈡嚐椤栨稒娅犻柟缁㈠枟閻撴盯鎮橀悙鎻掆挃闁靛棙甯￠弻宥堫檨闁告挶鍔庣槐鐐哄幢濞戞锛涢梺鍛婁緱閸橀箖寮抽敂閿亾閸忓浜鹃梺鍛婃处閸撴盯宕㈤幖浣瑰€甸柛蹇擃槸娴滈箖鏌ｆ惔顖滅У闁告挻鐩弫宥夋偄閸濄儳顔曢柣搴ｆ暩椤牓顢撻幘鏂ユ斀闁炽儴娅曢崰妯活殽閻愯尙绠婚柟顔规櫇閹风娀鎳犻澶婃杸闂傚倷绀佸﹢閬嶅磿閵堝鏄ラ柛顐ｇ箥閻掍粙鏌曟径鍫濆姉闁衡偓娴犲鐓熸俊顖濇娴犳盯鏌￠崱蹇旀珚闁哄本绋撻埀顒婄秵閸撴瑦绂掗埡鍛厪闁搞儯鍔屾慨宥団偓瑙勬礃閿曘垽鍨鹃敃鍌氱闁绘ɑ褰冩闂傚倸鍊烽懗鍫曞磻閵娾晛纾块柡灞诲劚缁€鍫熸叏濡寧纭鹃柛姘秺閺岋繝宕堕埡浣圭€繛瀛樼矋缁捇寮婚悢鍏煎€绘俊顖濇娴犳挳姊洪柅鐐茶嫰婢ь喚绱撳鍜冨伐妞ゆ洩缍佸畷妯侯啅椤旀儳寮抽梻浣告啞濞插秹寮插▎鎴ｅС濠电姵纰嶉埛鎴︽煕濠靛棗顏╅柍褜鍓氶幃鍌炲箖濡　鏀介悗锝庝簽閸橀亶姊洪崫鍕殭闁绘锕幃锟犳偄閸濄儳顔曢梺鐟邦嚟閸嬬喐鏅堕鍫熺厱闁哄啯鎸鹃悾鐢告煛?sniroute.cmd update check闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф闂佸憡鎸嗛崨顔筋啅闂備浇顕х€涒晠鎳濇ィ鍏洦瀵奸弶鎳筹箓鏌涢弴銊ョ仩缂佺姴纾埀顒€绠嶉崕鍗炍涢鐐嶇喖宕掑鍛紳婵炶揪绲块悺鏃堝吹濞嗘劒绻嗘い鎰剁悼缁犵偟鈧鍠涢褔鍩ユ径濞㈢喖鏌ㄧ€ｅ灚缍屽┑鐘愁問閸犳銆冮崨瀛樺亱濠电姴娲ら弸渚€鏌涢幘妤€鎳愰敍婵嬫⒑缁嬫寧婀版慨妯稿妼閻☆厽绻濋悽闈涗粶婵☆垰锕畷锝夊礃椤垵娈ㄦ繝鐢靛У绾板秹宕戦崟顖涚厽闁规崘娅曢幑锝夋煟閿濆鐣洪柟顔煎槻椤劑宕橀顖嗗應鍋撳▓鍨灕婵炲懏娲栭…鍥疀濞戞鈺呮煥閺囨浜鹃悗娈垮枛濞尖€愁潖濞差亜绠伴幖杈剧悼閻ｇ敻姊洪悷鏉挎Щ妞ゆ垵鎳撻悘鎺楁⒑闁偛鑻晶瀛樻叏婵犲懏顏犻柟椋庡█閸ㄩ箖鎼归銈勯偗缂傚倸鍊搁崐鍝ョ矓閹绢喗鏅濇い蹇撶墛閸嬫ɑ銇勯弴妤€浜鹃悗瑙勬礀閻栧ジ銆佸Δ浣哥窞閻庯綆鍋呴悵婵嬫⒒閸屾瑨鍏岀紒顕呭灠铻炲ù锝囩《閺嬪秹鏌ㄥ┑鍡╂Ц闁绘挻绻堥悡顐﹀炊閵娧€濮囬梺鎶芥敱鐢帡婀侀梺鎸庣箓閻楀﹪顢旈悩缁樼厪闁糕剝顨愰煬顒勬煛瀹€瀣瘈鐎规洏鍔戦、娆撴倷椤掆偓閸ㄩ亶姊绘担鐑樺殌缁剧虎鍙冨畷浼村冀椤撴壕鍋撴担绯曟闁靛繆鈧枼鍋撴繝姘厱闁靛鍨哄▍鍥煃缁楀搫鈧牜鎹㈠┑瀣仺闂傚牊绋堥弸鍛攽閻愭澘灏冮柛銉ㄥ煐閺呯偤姊洪崨濠佺繁闁搞劌纾禍鎼佹偨绾版ê浜炬鐐茬仢閸旀碍銇勯敂鍨祮闁诡噯绻濆畷姗€顢旈崨顓熺€炬繝鐢靛Т閿曘倝鎮ч崱娆戠焼闁割偁鍎查悡鐔兼煛閸愩劎澧辨俊顐ｅ灴閺岋繝宕遍鐑嗘喘闂侀€炲苯澧伴柟铏尵閳ь剚鍑归崣鍐ㄧ暦閹惰棄绾ч柟鐐藉妽濡炰粙銆侀弬娆惧悑闁糕剝蓱閻濇洟姊婚崒娆戭槮闁规祴鍓濈粭鐔肺旈崨顓炵€梺瑙勵問閸犳寮冲鍫熺厵闁告挆鍛闂佹娊鏀遍崹鍧楀箖瑜版帒鐐婃い蹇撳濮ｃ垻绱撴担鍝勑ｅ┑鐐诧躬瀵鈽夊锝呬壕闁挎繂楠告晶顕€鏌＄€ｎ偆銆掗柍褜鍓濋～澶娒哄Ο濂芥椽鎮㈤悡搴ｇ暫闂佷紮绲介惉濂告儗濞嗘挻鍋ｅ〒姘煎灣閸樻粎绱?
// 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁炬儳缍婇弻鐔兼⒒鐎靛壊妲紒鐐劤濠€杈╂閹烘柡鍋撻敐搴′壕缂佹墎鏅滅换娑橆啅椤旇崵鍑归梺绋款儐閸旀瑩骞冭ぐ鎺戠倞妞ゅ繐瀚В銏ゆ⒑閹稿海鈯曢柣鈺婂灠椤繐煤椤忓懎浠梺鍝勵槸缁ㄩ亶骞愰崘顏嗙＝濞撴艾娲ゅ▍姗€鏌涢妸銊︾【闁伙絿鍏橀獮瀣晜閼恒儲顏熼梻浣虹帛閿氱痪缁㈠弮瀹曘垽骞栨担鍏夋嫼闂佺鍋愰崑娑㈠礉濮椻偓閺屾盯寮崸妤€寮伴梺闈涙閸婂潡骞婇悩娲绘晢闁稿本绮ｇ槐鎻掆攽閻愯埖褰х紒韫矙楠炴牠顢曢敂琛″亾閸屾稓闄勯柛娑橈功閸橆亪姊洪幖鐐插妧闁告劕褰炵槐鏃堟煟鎼淬埄鍟忛柛锝庡櫍瀹曟娊鏁愭径濠冩К闂侀€炲苯澧柕鍥у楠炴帡骞嬪┑鎰棯闂備胶顭堥鍛搭敄婢舵劕钃熼柨婵嗘啒閻斿吋鍊绘俊顖炴敱閻忓棙绻?GitHub API闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф婵°倧绲介崯顖烆敁瀹ュ鈷戠紒瀣儥閸庡繑銇勯幋婵愭█闁诡噯绻濋、鏇㈡晝閳ь剟鎮欐繝鍥ㄧ厪濠电偛鐏濋崝姘舵煛鐎ｎ偆鎳呯紒杈ㄦ崌瀹曟帒顫濆В娆欑秮閺岋絽螖閸愩劉鏋呴悗娈垮枦椤曆囧煡婢舵劕顫呴柣姗€娼ф慨鍫曟⒒娴ｅ憡鍟為柛鏃撻檮缁傚秹寮介‖顒婄秮瀹曞ジ濡烽敂鎯у箺闂佺懓鍚嬮悾顏堝垂閸忚偐鐝舵俊顖涚湽娴滄粓鏌ㄩ弮鍥跺殭闁诲骏绠撻弻宥夊Ψ椤栨凹娲梺鍦嚀鐎氱増淇婇幖浣规櫇闁逞屽墰婢规洟顢涘☉姘鳖啎闂佸湱鍋撳娆撳吹椤掑嫭鐓曢柕濞垮姂閸濈儤銇勯妸锔炬噧闁宠鍨块幃鈺呭垂椤愶絾鐦庨梻浣侯焾椤戝棝鎯勯婵囶棨婵＄偑鍊栭幐鍫曞垂瑜版帒鍨傞柛灞绢嚔瑜版帗鍋愮€瑰壊鍠栭崜鎶芥倵鐟欏嫭灏紒鑸靛哺瀵鍨鹃幇浣告倯闁硅偐琛ラ埀顒冨皺閻栭亶姊绘担渚劸妞ゆ垵妫濋獮鎰板箹娴ｅ摜鍙€婵犮垼娉涜墝闁哄绉归弻褑绠涢敐鍛盎闂佽绻堥崕鐢稿蓟閿濆鏅查柛銉戝啫绠ｉ柣搴ゎ潐濞叉粓宕伴弽顓溾偓渚€寮介鐐电厬闂侀潧锛忛崨顔芥珤闂傚倸鍊搁崐椋庣矆娓氣偓瀹曨垶宕稿Δ鈧粻瑙勩亜閹拌泛鐦滈柡浣稿閺屾稑鈽夊▎鎴炴闂侀€炲苯澧柟铏耿閵嗕礁螖閸涱厾顦板銈嗗笒閸婃悂鐛澶嬬厽閹艰揪绲鹃崵鈧柣搴㈢煯閸楀啿鐣烽幋鐐电瘈闁搞儜鍜冪吹闂備礁鎼崐鍝ヨ姳闁秴缁╁ù鐘差儐閻撳啴鏌曟径鍫濆姢闁挎稑绉归弻娑㈠煛鐎ｎ剚鍋ч梺閫炲苯澧叉い顐㈩槸鐓ら柡宥庡幖鍥撮梺鍛婄☉閿曘儵宕ｈ箛娑欑厓鐟滄粓宕滈悢鐓庤摕鐎广儱顦扮€电姴顭跨捄渚剾婵☆偓绱曠槐鎾诲磼濮樻瘷銏ゆ煟椤撶偛鈧悂锝炶箛鏃傜瘈婵﹩鍓涢敍婊冣攽閻愬弶顥為柛銊ョ仛娣囧﹥绂掔€ｎ偆鍘介柟鑹版彧缁插潡骞婂Δ鍛厱婵炴垵宕悘锝夋煟閹惧瓨宕屾慨濠呮閹叉挳宕熼棃娑楄繕闂佽绻掗崑娑㈠磹閸涘﹥鍙忛柛宀€鍋為悡鏇㈡煃鐟欏嫬鍔ゅù婊呭亾娣囧﹪鎮欓鍕ㄥ亾閺嶎偅鏆滈柟鐑橆殔绾惧綊鏌ゆ慨鎰偓鏇⑺夊顑芥斀闁稿本绮犻悞鍓х磼閻欐瑥娲﹂悡鏇熴亜閹板墎鎮奸柍钘夘槸閳规垿妾辨繛澶嬫礈濡叉劙骞掑Δ浣镐汗闂傚倸鐗婄粙鎰柦椤忓嫷娓婚柕鍫濆暙婵℃悂鏌涙惔锝嗘毈妤犵偛鍟埢搴ㄥ箻閺夋垶顓绘俊鐐€栭崺鍫ュ磹閵堝鐐婇柕濞у啫绠為梻鍌欑閻ゅ洭锝炴径鎰？闁惧浚鍋嗛々鏌ユ煟閹伴潧鍘哥紒璇叉閺岋綁骞囬崗鍝ョ泿闂侀€炲苯澧柣妤佺矒瀵偊顢欓悾宀€鐦堥梺鎼炲劘閸斿苯鈻撻幋锔解拺闁告挻褰冩禍婵囩箾閹绘帞绠婚柛鈹惧亾濡炪倖宸婚崑鎾绘煙閾忣偓鑰挎鐐插暙椤劑宕奸悢閿嬬枀闂備線娼ч敍蹇涘礃閵娿儺娲繝鐢靛Х閺佸憡鎱ㄦ导鏉戝瀭濠靛倻顭堥弰銉╂煛瀹ュ骸骞楅柛搴″閵囧嫰寮介妸銉ユ瘓闂佽姘﹂崺鏍Φ閸曨垰鍗虫俊銈傚亾濞存粓绠栧娲传閸曨偒妲甸梺鍛婃尰缁诲倿顢氶妷鈺佺妞ゆ帒鍊婚惁鍫ユ⒑閸涘﹥澶勯柛妯荤矒婵℃悂鍩￠崒妤佸闂備礁鎲＄换鍌溾偓姘煎幖閿曘垽骞嶉鍓э紲闁诲函缍嗛崑鍕倶閹绢喗鐓ユ繝闈涚墕娴犳粓鏌嶇憴鍕仼闁逞屽墾缁蹭粙鎮樺璺虹柧闁靛繈鍊栭埛鎴︽煕濞戞﹫鏀荤紒鈧崼鐔翠簻闁挎繂瀚惌娆戔偓瑙勬礃閸ㄥ潡鐛Ο渚唵闁告洦鍓氶ˉ鍫⑩偓瑙勬礃閿曘垽宕洪埄鍐╁闁革富鍘搁崑鎾淬偅閸愨斁鎷绘繛杈剧到閹诧繝骞嗛崼鐔翠簻闁挎柨鐏濆畵鍡樸亜閵忥紕澧柍璇查叄楠炴﹢宕橀崣澶婂姃闂傚倷鑳剁划顖炲蓟閵娾晛瑙﹂悗锝庡枟閸嬪倿鏌ｉ弬鍨倯闁绘挸绻愰…鍧楁嚋瀵版浜绋库枎閹寸姷锛滈梺闈涱焾閸婃鎯屽▎鎾寸厸鐎光偓閳ь剟宕伴弽顓炵畺闁绘垼妫勭痪褎绻涢崱妤€缍栫紒杈ㄦ煥閳规垿鎮╁▓鎸庢瘜闂佸憡鎸荤换鍫ョ嵁閸愵喖纾兼繛鎴炵懃瀵潡姊洪柅鐐茶嫰婢ф挳鏌＄仦璇插闁宠棄顦灒濞撴凹鍨幃锝嗕繆閵堝洤啸闁稿鍋ら弫鍐閻欌偓濞兼牕鈹戦悩瀹犲缁炬儳鍚嬬换娑㈠幢濡櫣浠存繝纰樷偓鑼煓闁哄矉绲鹃幆鏃堟晲閸ャ劌娈戦梻浣告啞娓氭宕归幎鑺ュ亗闁瑰墽绮埛鎴︽煟閻旂厧浜伴柛銈囧枛閺屾稓鈧綆鍓欓弸娑欐叏婵犱胶鐭欑€规洜鍠栭、娑樷槈濮橆剙绠伴梻鍌欑閹诧繝宕濋幋锕€绀夌€光偓閸曨偆鍔﹀銈嗗笒閸燁偊宕ラ崷顓犵＜妞ゆ梻鈷堥悡鑲┾偓娈垮櫘閸撴瑩鍩㈡惔銊ョ鐟滃秹寮幖浣光拻濞达絿顭堥幃鎴︽煙椤旂厧鈧悂鈥﹂崶顏嶆Ъ闂侀€涚┒閸旀垵鐣锋總绋课ㄩ柨鏃€鍎抽獮鍫ユ⒒娓氣偓濞佳囨偋閸℃稑绠犻幖鎼厛閺佸倹銇勯幘鍗炵仾闁绘挻绋撻埀顒€鍘滈崑鎾绘倵閿濆骸澧伴柡鍡欏█閺屟勫濞嗘垵鍩岄梺闈涙鐢帡锝炲┑瀣亗閹艰揪绲奸悽鑽ょ磽娴ｈ娈曢柛銊ㄥ煐缁岃鲸绻濋崶顬囨煕閵夘喚浠㈤柕澶嗘櫆閻撴洟鎮楅敐搴′簻闁诲繐顕埀顒冾潐濞叉粓銆佹繝鍥﹂柟鐗堟緲缁犳娊鏌熼幆褍顣抽柡鍡愬灲濮婂宕掑顑藉亾閻戣姤鍊块柨鏃堟暜閸嬫挾绮☉妯诲櫤鐎规洘鐓￠弻娑樼暆閳ь剟宕戝☉姘变笉闁绘绮悡娆撴⒑椤撱劎鐣遍柡瀣ㄥ€曢埥澶愬箻瀹曞泦褔鏌″畝鈧崰鎾舵閹烘嚦鐔告姜閺夊灝鐏侀梻鍌欒兌缁垶寮婚妸銉庢稑鈽夐姀鐘电枀缂備礁顑嗛娆撴偂閵夆晜鐓曟い鎰剁悼缁犳﹢鏌ｉ幘鏉戠伌婵﹥妞藉畷銊︾節閸屾鏇㈡⒑閸濄儱校闁绘绮庨崚鎺旀嫚瀹割喗鍍甸梺鐓庢憸閺佹悂宕㈤柆宥嗏拺闁告繂瀚崒銊╂煕閺傝法鐒搁柛鈹惧亾濡炪倖甯掗崐鎼佸煝閸儲鐓欐い鏂挎惈閻忚尙鈧娲﹂崑濠傜暦閻旂⒈鏁冩い鎰╁灪椤斿嫰姊婚崒姘偓椋庣矆娓氣偓楠炴牠顢曢埛姘そ婵℃悂鍩℃担鐚寸幢婵犵妲呴崹闈涚暦閻㈠憡鍋?
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
        // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ姀鐘差棌闁轰礁锕弻鈥愁吋鎼粹€崇缂備焦鍔栭〃鍡樼┍婵犲洤围闁告侗鍙庢禒楣冩⒑閻熸澘鏆遍柡鍫墰閹广垹鈹戦崱蹇旂亖闂佸壊鐓堥崰妤呮倶閸儲鈷戦柟绋垮绾炬悂鏌涙繝鍐炬畼婵″弶鍔欓幃娆戔偓闈涙憸閻﹀牓姊虹粙鎸庢拱缂侇喖绉撮埢鎾淬偅閸愨晛鈧敻鏌ｉ悢鍛婄凡妞ゅ浚浜弻娑㈡晲閸℃绋囬柣鎾卞€栭妵鍕疀閹剧紟锝夋煛閸涱喚鐭掓慨濠冩そ瀹曘劍绻濋崘顭戞П闂備礁鎲￠敋闁靛牊鎮傞悰顔嘉旈崘锝呬壕闁挎繂楠搁弸鐔兼煃闁垮绗掗棁澶愭煥濠靛棙鍣洪柛鐔哄仱閺屾盯鎮㈤崫鍕闂佸搫鐭夌紞渚€骞冮妶澶婄＜婵犲﹤鎲涘鍫熷€甸悷娆忓缁€鍐╃箾閼碱剙鏋涢柣娑卞枟閹棃濡搁敃鈧▓鐔兼⒑闂堟侗妲堕柛搴濆嵆閹晫绱掑Ο鑲╃槇闂佹眹鍨藉褔鍩㈤崼鐔虹濞达絽鍟块崥妯衡槈濮橆偅鍕冮柡澶屽仜鐏忓懐绱炴担鍝ユ殾濠靛倻顭堝敮闂佹寧姊归崕鎶界嵁瀹ュ鈷掑ù锝堟鐢盯鏌涢弮鎾剁暤鐎规洘绮岄埢搴ㄥ箛椤撶喓褰挎俊鐐€栭悧婊堝磻閻愮儤鍋傞柡鍥ュ灩缁犲綊寮堕崼婵嗏挃缁炬崘娉曠槐鎺楁偐瀹曞洤寮ㄩ梺鍝勬湰濞茬喎鐣烽幆閭︽Щ濡炪倕娴氶崣鍐蓟閳╁啯濯寸紒瀣濞堟煡姊洪悷鏉挎Щ闁硅櫕锚閻ｇ兘鎮滅粵瀣櫍濠电姍鍕姢濞存粍鍎抽埞鎴︽偐閸欏顦╅梺鎶芥敱閸ㄥ潡骞冭ぐ鎺戠倞闁挎繂鍊告禍楣冩煣韫囷絽浜炲ù婊嗗煐缁绘繈鎮介棃娴剁偤鏌涢妸锔姐仢闁轰礁鍟存慨鈧柕鍫濇噽閻ｆ椽姊虹拠鈥崇€婚柍褜鍓熷銊︾鐎ｎ偆鍘卞銈嗗姧缂嶁偓濠㈣锚闇夋繝濠傜墢閻ｆ椽鏌＄仦璇插闁宠鍨垮畷鍗炩槈閹典礁浜炬俊銈傚亾妞ゎ叀鍎婚ˇ閬嶆煕濡や礁鈻曠€殿喖顭烽弫鎰板川椤忓拋娼旈梻浣告惈鐠囩偤宕橀妸鎰礃缁绘繂鈻撻崹顔界亪闂佹寧娲忛崕閬嶁€旈崘顔藉癄濠㈣泛鏈▓楣冩⒑闂堟侗鐓┑鈥虫川瀵囧焵椤掑嫭鈷戦柛娑橈攻婢跺嫰鏌涢幘瀵告噧闁宠绉烽ˇ鏌ユ煃鐟欏嫬鐏撮柟顔界懇閹崇娀顢楁担鐟版灈缂傚倸鍊风欢锟犲窗閺嶎偆鐭撶憸鐗堝笒缁犳牠鏌ｉ妶搴＄伇婵☆垰瀚灃闁绘﹢娼ф禒锕傛煕閵婏箑顕滈柟渚垮姂閸┾偓妞ゆ帒瀚悡鐔哥箾閸℃瀚板ù婊勫劤閳规垿鎮欓懠顒佺檨闂佸搫鎳愭繛鈧€殿喖顭锋俊鑸靛緞婵犲嫷鍟囧┑鐐舵彧缂嶁偓妞ゎ偄顦甸弻銊╊敇閻旇櫣顔曢柣搴ｆ暩椤牆鏆╅梻浣告啞椤棝宕惰閸樻挳姊虹涵鍛涧闂傚嫬瀚板畷鎰版倻閼恒儳鍘卞┑鐐叉濞存艾螣閸喍绻嗘い鎰╁€曟禒褏绱掓潏銊﹀鞍闁瑰嘲鎳橀幖褰掓偡閹殿噮鍋ч梻鍌欑閹测€愁潖瑜版帗鍋￠柕濞炬櫆閸庢螖閿濆懎鏆為柍閿嬪灴閺屾稑鈽夊鍫燁暭缂備礁鐖兼禍鍫曞蓟濞戞鐔哄枈鏉堚晛濮奸梺鍛婎殜娴滃爼寮诲☉銏╂晝闁绘ɑ褰冩慨鏇㈡⒑缂佹ɑ灏伴柣鐔叉櫊瀵鎮㈢喊杈ㄦ櫖闂佺硶鍓濋〃鍛村汲閵忋倖鐓曢柍鍝勫暙娴犺鲸鎱ㄦ繝鍐┿仢闁哄苯鎳橀幃娆撴嚑鐠轰警浼冨┑鐘媰鐏炵晫浠搁梺鍝勭焿缂嶄線宕洪埄鍐瘈闁稿本绮堝鎾斥攽閻樻鏆柍褜鍓欑壕顓㈠箺閻樼數纾兼俊銈呭暙閺嬫盯鏌涢埞鎯т壕婵＄偑鍊栫敮鎺斺偓姘煎枛閻ｅ灚绗熼埀顒勫箖濡ゅ懏鏅查幖绮瑰墲閻忓秹姊洪崫鍕櫤缂佽鐗撳璇差吋閸偅顎囬梻浣告啞閹稿鎯勯鐐叉槬闁逞屽墯閵囧嫰骞掑鍡欍偘婵犮垼顫夎摫闁靛洤瀚伴弫鍌滄嫚閼碱兛妗撻梻浣哄仺閸庣粯淇婇崶顒€绠查柛鏇ㄥ灠鎯熼梺鎸庢磵閸嬫挾鐥紒銏犵仭缂佺粯鐩獮姗€寮堕幋婵囩槗闂備焦鎮堕崝瀣础閸愯尙鏆︽繝濠傜墕绾偓闂佺粯鍔忛弲婊堝棘閳ь剟姊绘担铏瑰笡闁告梹娲栭锝夊醇閺囩偤妫烽梺闈╁瘜閸樺墽澹曢懖鈺冪＝濞达絿娅㈤崗顒勬煠閹峰本纭堕柍褜鍓氶鏍窗濞戙埄鏁嬬憸鏃堝春閵夛箑绶為柟閭﹀墻濞煎﹪姊虹紒姗嗙劷闁轰焦鎮傚鍫曞川婵犱胶绠氶梺缁樺姦娴滄粓鍩€椤掍胶澧遍柡渚囧枟缁绘繈宕熼鐘靛姸濠电偠鎻徊璺ㄦ兜閸洖鍑犵€广儱顦伴悡銉︾節闂堟稒顥炲┑顖氬悑缁绘盯骞橀幇浣哄悑闂佸搫鏈ú鐔风暦閻撳簶鏀介柛銉ㄥ煐閿涗線姊绘担鐟扳枙闁衡偓闁秴绀夋繛鍡樻尭閽冪喐绻涢幋娆忕仼缂侇偄绉归弻宥堫檨闁告挾鍠栭獮鍐倻閼恒儲娅嗛梺浼欑到婢跺洭寮?
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


// ---------------------------------------------------------------- GHA 婵犵數濮烽弫鎼佸磻閻愬搫鍨傞柛顐ｆ礀缁犱即鏌熼梻瀵歌窗闁轰礁瀚伴弻娑㈩敃閿濆洩绌块悷婊呭鐢帡鎮欐繝鍥ㄧ厓閻熸瑥瀚崝銈吤瑰鍫㈢暫闁哄矉缍佹慨鈧柍钘夋閺呴亶姊虹悰鈥充壕婵炲鍘ч悺銊╂偂閻斿吋鐓ラ柡鍥ㄦ皑閸斿秵銇勯妷銉Ч闁?
// 闂傚倸鍊搁崐椋庣矆娓氣偓楠炴牠顢曢敂缁樻櫈闂佸憡娲﹂崹閬嶅吹閸曨垱鐓曟い鎰剁稻缁€鍐煕鐎Ｑ勬珕缂佺粯绻堝Λ鍐ㄢ槈濞嗗浚妲辩紓鍌氬€哥粔鏉懨洪銏犺摕闁绘梻鍘х粻姘辨喐瀹ュ姹查弶鍫氭杹閸嬫挸鈻撻崹顔界仌濠电偛鎳忓ú妯肩矚鏉堛劎绡€闁稿濮ゅΛ鍐春閳ь剚銇勯幒鍡椾壕闁捐崵鍋ら幃妤呮濞戞瑥鏆堝銈庡亝濞茬喖寮婚悢灏佹灁闁割煈鍠楅悘鎾剁磼閹冪稏缂侇喗鐟╁濠氬灳閹颁礁鎮戦柟鑲╄ˉ閳ь剚鍓氬楣冩⒑濮瑰洤鐒洪柛銊︽緲铻炴繛鍡樺姉閳?+ 缂傚倸鍊搁崐鐑芥嚄閸洘鎯為幖娣妼閻骞栧ǎ顒€濡肩紒鎰殜閺屸€愁吋鎼粹€崇闂?闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐蹭画闂佹寧娲栭崐褰掑磻鐎ｎ偂绻嗛柕鍫濇噹閺嗙喖鏌ｉ鐔稿磳闁哄矉缍侀獮瀣晲閸涘懏鎹囬弻锟犲椽娴ｇ顫梺閫炲苯澧柛妯荤矒瀹曟垿骞樼紒妯煎帗閻熸粍绮撳畷婊堟偄妞嬪孩娈惧┑鐐叉閹稿宕靛澶嬬厪濠㈣鍨伴張顒傜矈椤愶附鈷掑ù锝囨嚀椤曟粎绱掔拠鎻掆偓鍝ュ弲闂侀潧艌閺呮盯宕归崒鐐寸厵闁诡垱婢樿闂佺粯鎸诲ú鐔煎蓟閻斿吋鍊绘俊顖濐嚙閺嗘姊虹€圭姵顥夋い锕佷含濡叉劙骞樼拠鑼紲濠电偞鍨堕悷銉у閹惰姤鈷戦弶鐐村椤︼妇绱掓径灞惧殌闁伙絿鍏橀幃鈩冩償椤旀儳寮抽梻浣告啞娓氭宕归柆宥呯獥闁圭儤顨嗛埛鎺楁煕鐏炴崘澹橀柍褜鍓涢…鍫㈠弲闂佺粯顭堥褏澹曟繝姘厵闁告挆鍛闂?.rc 闂傚倸鍊峰ù鍥х暦閸偅鍙忕€广儱顦粈鍐┿亜椤撶喎鐏ｉ柣銉簽缁辨捇宕掑▎鎴ｇ獥闂佺粯鎸搁悧鎾崇暦閹惰姤鏅插璺猴攻閻庮剟鎮楅獮鍨姎婵☆偅顨堝▎銏ゆ倷閻戞鍘卞銈嗗姧缂嶁偓濠㈣锚闇?
// Enter = 缂傚倸鍊搁崐鐑芥嚄閸洘鎯為幖娣妼閻骞栧ǎ顒€濡肩紒鎰殜閺屸€愁吋鎼粹€崇闂佺顑傞弲娑滅亙闂佹寧绻傞幊搴ㄥ汲閻愮儤鐓涢柛婊€绀佹晶鎾煙椤旂瓔娈旈柍钘夘槸铻ｉ柣鎾崇岸閸嬫捇鎯?= 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐蹭画闂佹寧娲栭崐褰掑磻鐎ｎ偂绻嗛柕鍫濇噹閺嗙喖鏌ｉ鐔稿磳闁哄矉缍侀獮瀣晲閸♀晜顥夐柡?
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

// 闂傚倸鍊搁崐椋庣矆娓氣偓楠炲鏁撻悩鍐叉疄闂佺粯鍔﹂崜锕€鈻撻懞銉﹀弿婵犻潧妫滈～锕€霉濠婂啰绉洪柟顔筋殜濡啫鈽夊▎蹇旀畼闁?gha fetch闂傚倸鍊搁崐鐑芥倿閿旈敮鍋撶粭娑樻噽閻瑩鏌熸潏鍓х暠閻庢艾顦伴妵鍕箳瀹ュ棭妯傜紓浣筋嚙濡繈寮婚悢鍏尖拻閻庨潧澹婂Σ顕€鏌ｆ惔銏ｅ妞わ妇鏁诲璇差吋閸偅顎囬梻浣告啞閹搁箖宕版惔銊﹀仼闁绘垼妫勫敮闂佹寧妫佸Λ鍕礉閸涱厸鏀介柣妯肩帛濞懷囨煟濡や胶鐭嬮悗闈涖偢瀹曞爼顢楁担鍝勫箺闂備胶绮崝鏇㈩敋椤撱垹鐓″鑸靛姈閻撶喐銇勯幘瀵哥畾闁稿繐鏈换娑㈠幢閹邦剛浼堝銈冨灪閿曘垺鎱ㄩ埀顒勬煟濡椿鍟忔俊鎻掑船閳规垶骞婇柛濞у懐涓嶉柟杈鹃檮閸婅埖绻濋棃娑卞剰闁搞劌鍊垮娲敆閳ь剛绮旈悽鍛婂亗闁哄洢鍨洪悡娆撴煟閹寸倖鎴︽偂閵夛负浜滈柨婵嗘噷閸嬨垽鏌″畝瀣М濠碘剝鎮傞崺锟犲磼濞戙垺鏆橀梻鍌欑绾绢厾鍒掗姘兼綎鐟滅増甯炲畵浣糕攽閻樺弶澶勯柣?
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
    // WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯顢曢敐鍡欙紩闂侀€炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劍銇勯弮鍥撴繛鍛墦濮婄粯鎷呴崨濠傛殘闂佸憡妫戠粻鎾崇暦娴兼潙鍐€妞ゆ劑鍩勫Λ婊堟⒑缁夊棗瀚峰▓鏇㈡煃闁垮鐏﹂柕鍥у楠炴帡宕卞鎯ь棜缂傚倸鍊风粈渚€藝椤栫儐鏁嬫い鎾跺Т閸ㄦ繂鈹戦悩瀹犲闁告劏鍋撴俊鐐€栭崝妤呭窗鎼淬垻顩查柣鎰靛墻濞堜粙鏌ｉ幇顖氱毢濞寸姰鍨介弻娑㈠籍閳ь剙鐣濋幖浣歌摕闁绘梻鍘ч惌妤€顭跨憴鍕濞存粠浜畷娲Ψ閿曗偓缁剁偤鏌熼柇锕€澧绘繛鐓庯躬濮婅櫣绱掑鍫ｂ偓鎸庣箾娴ｅ啿瀚崣蹇涙煥閺冣偓閸ㄦ繄鎹㈤崱娑欑厽闁规澘鍚€缁ㄥ鏌嶈閸撴岸鎮у鍫濇瀬妞ゆ洍鍋撴鐐村浮瀵剟宕崟顏勵棜婵犳鍠楅…鍥储瑜庨弲鍫曞箻椤旂晫鍘甸梻浣哥仢椤戝懘鎮橀幘顔界厵妞ゆ柨鎼悘鑼偓瑙勬礃椤ㄥ﹪骞婇弽顓炵厸濞达絿顭堥々顒€鈹戦悩鍨毄濠殿喕鍗冲畷瑙勭附缁嬫寧妲梺鍝勭▉閸樹粙寮查鍫熲拻濞达絿鐡旈崵娆徫旈悩鑼鐎垫澘锕﹂幑鍕Ω閿旂粯缍?0 缂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌熼梻瀵割槮缁惧墽鎳撻—鍐偓锝庝簼閹癸綁鏌ｉ鐐搭棞闁靛棙甯掗～婵嬫晲閸涱剙顥氶梻浣藉Г钃辩紒璇茬墦瀵鎮㈢亸浣圭亖闂佸壊鐓堥崰妤呮倶瀹ュ鈷戦柛婵嗗椤ョ偟绱掗鑲╃劯闁糕晝鍋ら獮瀣晜閽樺姹楅梻浣告贡缁垳鏁埡鍌氼嚤闁逞屽墴濮婄粯鎷呴崨濠傛殘濡炪們鍔屽Λ妤呮偩濠靛绠瑰ù锝嗙摃閹芥洟姊洪弬銉︽珔闁哥噥鍋嗗▎銏ゆ倷閻戞ê鈧敻鏌ㄥ┑鍡欏嚬缂併劏妫勯湁婵犲﹤鍟伴崺锝嗘叏婵犲啯銇濈€规洦鍋婂畷鐔兼濞戞ê顥嶉梻鍌氬€风粈浣规償濠婂懐绀婂ù锝呭閸ゆ洟鎮归崶褎鈻曢柛姘儏椤法鎹勬笟顖氬壈闂佽皫鍐仾缂佺粯绻堟慨鈧柨婵嗘閵嗘劕顪冮妶鍡楃仴闁瑰啿閰ｉ崺銏狀吋閸偅鞋缂傚倷娴囨ご鎼佸箰婵犳艾绠柛娑欐綑缁犵敻鏌熼崫鍕㈡い銉︾缁绘繈鎮介棃娴剁偤鏌涢妸褍甯堕柍缁樻煥閳藉鍩楅埞鎯т壕闁挎洖鍋嗛弫鍡涙煕閺囥劌澧ù婊勭矒濮婅櫣鍖栭弴鐐测拤缂備礁顑嗙敮锟犲箖閳ユ枼鏋庨柟鎵虫櫃缁ㄥ姊洪崫鍕犻柛鏂块叄閸┿儲寰勭€ｃ劋绨婚梺闈涱槶閸庢椽宕曢幋鐘电＜閺夊牄鍔屽ù顔尖攽閳╁啯鍊愬┑锛勫厴閺佸啴鍩€椤掍胶顩烽柟缁㈠枟閳锋垿鎮归崶锝傚亾閾忣偆浜梻浣规偠閸ㄦ椽鎮￠敓鐘叉槬闁靛繆鍓濋崕鐔兼煃闁款垰浜鹃梺鍝勵儏闁帮綁寮婚悢鍏煎€锋い鎺嶇閸撳綊姊洪柅鐐茶嫰婢ь喚绱掓径濠傤暢婵″弶鍔欓獮妯尖偓娑櫭鎾绘⒑閸涘﹥灏扮憸鏉垮暣瀹曟粓寮跺▎鐐瘜闂侀潧鐗嗙换鎺楁偩闁秵鐓熼煫鍥ㄦ⒒椤ｈ尙绱掑畝鍐摵缂佺粯绻堝畷鎯邦樄闁哥偛鐖煎娲传閸曨剙绐涢梺绋款儐閻╊垶骞婇幘璇查敜婵°倓璁查幏娲煟鎼粹剝璐″┑顔炬暬婵℃挳宕橀埡鈧换鍡涙煟閹邦厽缍戠紒妤佺缁绘盯鎳濋柇锕€娈銈庡亝缁捇宕洪埀顒併亜閹哄棗浜惧銈庡亜缁绘帞妲愰幒鎳崇喓鎷犲顔瑰亾閹剧粯鈷戦柛娑橈功缂傛岸鏌涙惔銈嗙彧妞ゃ劍鐟ラ埞鎴︽偐閸偅姣勯梺绋款儐閿曘垹鐣烽悩缁樺亹閻犲洦褰冪粣娑橆渻閵堝棛澧紒顔奸叄瀹曘垽鎮╃紒妯煎幗闂佺粯鏌ㄩ幗婊堟儗鐏炵瓔鐔嗙憸搴ㄣ€冮崼銉ユ瀬妞ゆ洍鍋撴鐐村笒铻栧ù锝囩《閸嬫捇宕稿Δ浣哄幈濡炪値鍘介崹鍨閺嶎灐鐟邦煥閸曨厾鐓夐梺鍝勭灱閸犳牕鐣烽锕€绀嬫い鎾愁槶閸婃繈寮诲☉銏犳閻犳亽鍓辫閺屸€崇暆鐎ｎ剛袦婵犵鍓濋幃鍌炲极閸愵喖鐒垫い鎺戝閸婂爼鏌熺紒銏犳灍闁绘挻绋撻埀顒€鍘滈崑鎾绘倵閿濆骸澧扮悮锔戒繆閵堝洤啸闁稿鍋ら幃鐑藉煛娴ｇ儤娈鹃梺缁樻⒒閳峰牓寮崶顒佺厽婵☆垵娅ｆ禒娑欍亜椤愩垺婀扮紒?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煠绾板崬澧绘俊鏌ヤ憾閺岋綁鎮㈤崫銉х厒婵犫拃鍕垫畼闁轰緡鍣ｉ獮鎺懳旈埀顒勫垂閸岀偞鐓曠€光偓閳ь剟宕戦悙鐑樺亗闁绘棃鏅茬换鍡涙煏閸繂鈧憡绂嶆ィ鍐┾拺缂備焦锕╁▓鏃堟煥閺囥劋绨婚柣锝囧厴閺佹劙宕奸姀銏℃緫婵犵數鍋為崹鍫曟偡瑜斿畷銏ゅ箻缂佹ǚ鎷洪梺鍛婄☉閿曪妇绮婚幘缁樺€垫慨妯煎帶瀵喚鈧娲樻繛濠傜暦閻戠瓔鏁囬柣妯碱暜缁卞弶淇婇悙顏勨偓鏍礉閹达箑鍨傜紓浣股戝▍蹇涙⒒閸屾瑦绁版い鏇熺墵瀹曚即寮介銈囶槸婵犵數濮撮崐濠氬汲閿斿浜滈柟鏉垮閻ｈ京绱掗埀顒傗偓锝庡亖娴滄粓鏌熼崫鍕ゆい锔肩畵閺屾盯骞橀懜鍨瘓濠殿喖锕ら幖顐ｆ櫏闂佹悶鍎滈崨顓炐ら梻鍌欑婢瑰﹪鎮￠崼銉ョ闁绘棃顥撻弳锕傛煛閸ャ儱鐏柛瀣ㄥ妿缁辨帞鈧綆鍘奸崝鐢告煙閼恒儲绀嬫慨濠呮缁辨帒螣閻戔晜瀚介梻浣侯焾缁ㄦ椽宕愰弴銏″仼闂佸灝顑呯欢鐐烘煙闁箑骞橀柛妯挎閳规垿顢欓弬銈勭返闂佸憡顭囬弲顐ゆ閻愬搫绠ｆ繝銏＄箓缂嶅﹤鐣峰Δ鍛亗閹肩补妲呭鏇犵磽閸屾瑦绁板瀛樻倐楠炴垿宕惰閺嗭箓鏌熼悜妯虹劸婵炲皷鏅滈幈銊╂晲閸℃瑦鍣ｉ梺鎼炲妽濡炶姤淇婇悽绋跨妞ゆ牗姘ㄩ鍝勨攽閻樿尙浠涢柛鏃€鐗滈悷褔姊婚崒娆愵樂缂侀硸鍠氬濠冪鐎ｎ亜绨ラ梺鍝勮閸庮噣鍩€椤掑﹦鐣靛┑鈩冩倐閸┾剝鎷呮笟顖浶為梻鍌欐祰椤曟牠宕规惔銊ョ妞ゆ劗鍠庢禍鐐亜閺嶎偄浠﹂柍閿嬪笒闇夐柨婵嗙墱濞兼劙鏌涚€ｎ剙鈻堥柡灞剧⊕閹棃濮€鎺虫禒銏ゆ⒑鐠団€虫灍闁挎岸鏌嶇憴鍕仼闁逞屽墾缂嶅棝宕戦崨顓涙瀺闁搞儺鍓氶埛鎴犵磼鐎ｎ偄顕滄繝鈧幍顔剧＜閻庯綆鍋呭畷宀€鈧娲忛崹钘壩涢崘銊㈡閺夊牄鍔岄惁婊堟⒒娓氣偓濞佳囨偋閸℃あ娑樜旈崨顔尖偓鍫曟煥閺囩偛鈧綊宕愰悽鐢电＜婵°倓鑳堕埥澶愭煙閾忣偄濮嶉柟顖氱焸瀹曞ジ濡烽敂鎯у箺婵犲痉鏉库偓鎰板磻閹剧粯鐓熸俊銈傚亾閻庢凹鍘剧划瀣吋閸滀胶鍙嗛梺鍓插亞閸犳捇宕㈤幖浣光拺缂佸瀵у﹢鎵磼鐎ｎ偄鐏存い銏¤壘椤劑宕橀崜褍鏁搁梺鑽ゅТ閹碱偊骞栭锔绘晜妞ゆ劑鍊楃壕濂稿级閸稑濡肩紒妞﹀洦鐓冪憸婊堝礈閵娧呯闁糕剝顭囬々鏌ユ偣鏉炴壆绉块柨婵嗘偪濞差亶鏁傞柛娑卞灠鐢鏌ｉ悢鍝ョ煁缂侇喗鎸搁悾宄邦煥閸惊鈺冩喐韫囨洜鐭嗗┑鐘叉处閻撴洟鏌嶉埡浣告殶闁宠棄顦遍惀顏堝箚瑜滈悡濂告煛鐏炲墽鈽夐柍钘夘槸铻ｉ柛蹇撳悑閸犳牠姊绘担鐟扳枙闁衡偓閸楃儐娼栧┑鐘宠壘閻掑灚銇勯幒鍡椾壕缂備胶濮寸粔鐟扮暦閺囥垺鐒肩€规挶鍎卞ú锔锯偓浣冨亹閳ь剚绋掗敋濞存粍顨婂娲濞戣鲸顎嗘繝纰樷偓铏窛缂侇喚绮妶锝夊礃閳哄啫骞嶉梺璇叉捣閺佹悂鈥﹂崼鐔侯浄鐟滄棃寮婚悢鐓庣闁告瑥顦藉Λ鐐差渻閵堝骸寮ㄦ繛澶嬫礋楠炲骞橀鑲╊槹闂佽鍨辨竟瀣磻閹捐閿ゆ俊銈勮兌閸樻悂鎮楅崗澶婁壕闂侀€炲苯澧寸€规洑鍗冲浠嬵敇濠ф儳浜惧ù锝囩《濡插牊淇婇鐐存暠闁诲骸顭峰娲偡闁箑娈舵繝娈垮枛閸㈡煡鎮鹃悜钘夎摕闁靛濡囬崢?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑娑⑺囬悽绋挎瀬闁瑰墽绮崑鎰版煠绾板崬澧绘俊鏌ヤ憾閺岋綁鎮㈤崫銉х厒婵犫拃鍕垫畼闁轰緡鍣ｉ獮鎺懳旈埀顒勫垂閸岀偞鐓曠€光偓閳ь剟宕戦悙鐑樺亗闁绘棃鏅茬换鍡涙煏閸繂鈧憡绂嶆ィ鍐┾拺缂備焦锕╁▓鏃堟煥閺囥劋绨婚柣锝囧厴閺佹劙宕奸姀銏℃緫婵犵數鍋為崹鍫曟偡瑜斿畷銏ゅ箻缂佹ǚ鎷洪梺鍛婄☉閿曪妇绮婚幘缁樺€垫慨妯煎帶瀵喚鈧娲樻繛濠傜暦閻戠瓔鏁囬柣妯碱暜缁卞弶淇婇悙顏勨偓鏍礉閹达箑鍨傜紓浣股戝▍蹇涙⒒閸屾瑦绁版い鏇熺墵瀹曚即寮介銈囶槸婵犵數濮撮崐濠氬汲閿斿浜滈柟鏉垮閻ｈ京绱掗埀顒傗偓锝庡亖娴滄粓鏌熼崫鍕ゆい锔肩畵閺屾盯骞橀懜鍨瘓濠殿喖锕︾划顖炲箯閸涱垱鍠嗛柛鏇ㄥ亗閸濇姊绘担渚劸闁挎洏鍎甸幃褍顭ㄩ崼婵囩€銈嗘磵閸嬫捇鏌涢妸鈺冪暫妤犵偛娲﹂幏鍛村川婵犲骸鎮梻鍌氬€风粈浣圭珶婵犲洤纾婚柛娑卞姸濞差亜鍐€妞ゆ挾鍠庢禍妤€鈹戦悙鏉戠仧闁搞劍妞介幃锟犲即閵忥紕鍘甸梺閫涚祷濞呮洖鈻嶉崨顓涙斀闁挎稑瀚崝瀣磼缂佹鈽夐柣锝嗙箖缁绘繃绗熼埀顒勫磻濡ゅ懏鈷戦柛婵嗗閻掕法绱撳鍕妽闁逛究鍔戦崺鈧い鎺戝閻撳啴寮堕悙鏉戭€滈柛蹇撹嫰闇夐柣娆屽亾闁告瑥鍟～蹇涙惞鐟欏嫬鏋傞梺鍛婃处閸樹粙骞夊▎蹇婃斀闁绘劕寮堕崳鐑樼箾閸欏鐒界紒顔界濞煎繘濡歌閻﹀牓姊洪崨濠冪８闁告柨鏈粋宥夋倷椤掑倻顔曢梺鐟邦嚟閸嬬喖骞婇幇鐗堢厽闁瑰搫绉堕惌娆撴煛瀹€瀣М闁诡喓鍨洪ˇ鐗堟償閿涜埇鍔戝娲传閸曨喖顏紓浣割槺閸忔鐦繛鎾村焹閸嬫捇鏌＄仦鍓ф创妤犵偞顭囬埀顒勬涧閹诧繝宕虫导瀛樷拺缂侇垱娲橀弶娲煕閵娿倗鐭欓柣娑卞枛铻ｉ柛蹇曞帶閻濅即鎮楅悷鏉款棌闁哥姵顨堢槐鐐哄磼閻愮补鎷绘繛鎾村焹閸嬫挻绻涙担鍐叉处閸嬪鏌涢埄鍐槈缂佺姷濞€閺岀喖骞戦幇闈涙異闂佸搫顦板钘夘潖婵犳艾纾兼繛鍡樺笒閸橈繝姊虹涵鍛彧闁圭懓娲ら悾鐑藉Ψ瑜夐崑鎾绘晲鎼粹剝鐏嶉梺绋款儌閺呮粓濡甸崟顔剧杸闁规崘娉涢。娲⒑閹肩偛濡奸柛濠傛贡閹广垹鈽夊鍡楁櫊濡炪倖妫佸畷鐢稿礄瑜版帗鈷戠紒瀣皡瀹搞儲绻涚涵椋庣瘈鐎殿喖顭烽弫鎰緞鐎ｎ偅鐝抽梻浣告啞娓氭宕ｆ惔鈭ワ綁鎮介崨濞炬嫼缂備礁顑嗛娆撳磿閹扮増鐓欓柛娑橈攻閸婃劙鏌涢埞鍨伈妤犵偞顭囬幏鐘诲箵閹烘柡鍋撻鐑嗘富闁靛牆妫欓埛鎺楁煛閸滀礁浜濈紒鍌氱Ч楠炲洭鎮ч崼姘?闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涙川缁♀偓闂佺鏈划宀劼烽埀顒勬⒒娴ｄ警鐒炬い鎴炲灩濡叉劙寮撮姀鐘电枀闂佸綊妫跨粈浣虹尵瀹ュ鐓曟い鎰╁€曢弸鎴炵箾閸涱喚鐭掓慨濠呮缁辨帒螣閸濆嫷娼撻梻浣告贡閳峰牓宕戦崱娆忓灊濠电姵鑹剧粻顕€鏌ら幁鎺戝姢闁告鏁诲铏圭磼濡搫袝闂佸憡鎸诲銊ф閻愬搫绠ｉ柣鎰典簷缁ㄨ顪冮妶鍡楀闁哥喎娼″畷鎴﹀箻鐠囪尙鐤€婵炶揪绲块幊鎾绘儊濠婂牊鈷掑〒姘ｅ亾婵炰匠鍥ㄥ亱闁糕剝銇傚☉妯滄棃宕橀鍡欐殽闂備胶顭堥張顒傚垝鎼达絽顥氬┑鍌氭啞閻撳啰鎲稿鍫濈婵娉涚粻顖炴煕濞戝崬鐏￠柛鐘叉閺屾盯寮撮妸銉ょ盎婵犵鈧啿鏆遍柍瑙勫灴閹晠宕归锝嗙槑濠电姵顔栭崰鏍偤閵娧€鍋撻棃娑栧仮鐎规洏鍔戦幊婊堫敄閹稿海娼栭梻鍌欑濠€閬嶅磿閵堝鍨傜憸鐗堝吹閸ヮ亶妯勫┑顔硷工椤嘲鐣烽幒鎴僵妞ゆ垼妫勬禍楣冩煠婵劕鈧牕鐣烽崣澶岀瘈闂傚牊绋撴晶鎰版煕鐎ｎ偅灏柍钘夘樀楠炴帡骞樼€电绲藉┑锛勫亼閸娿倖绂嶅鍫濈柈閻庢稒眉缁诲棝鏌ょ粙璺ㄤ粵闁稿海鍠栭弻宥夊Ψ閿曗偓缁茬粯銇勯妷锔界殤缂佽鲸鎹囧畷鎺戔枎閹烘垵甯┑鐘愁問閸犳岸寮拠鑼殾鐟滅増甯掗柨銈嗕繆閵堝倸浜鹃梺璇茬箲婵炲﹪寮婚妸鈺佺睄闁割偆鍠愬▓浼存⒑缁嬫鍎愰柟鐟版搐閻ｇ兘鎮滅粵瀣櫍闂佺粯鍔栭崝鎴﹀磿閻㈢钃熼柨婵嗩槸鍥撮梺绋挎湰閻喗绔熼弴銏♀拻闁稿本鑹鹃埀顒勵棑缁牊绗熼埀顒勭嵁閺嶎収鏁冮柨鏇楀亾缁炬儳缍婇弻锕€螣娓氼垱孝闂佸搫顑嗛悧鐘诲蓟閿熺姴鐐婇柕澶堝劤閸旀挳姊?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈤崨濠勭Ф婵°倧绲介崯顖烆敁瀹ュ鈷戠紒瀣儥閸庡繑銇勯幋婵愭█闁诡噯绻濋、鏇㈡晝閳ь剟鎮欐繝鍥ㄧ厪濠电偛鐏濋埀顒侇殘閹广垺绗熼埀顒€顫忛悜妯诲闁规鍣Σ顕€鏌ｆ惔銏㈢叝闁告鍟块锝囨嫚濞村顫嶉梺闈涚箳婵牓濡搁埡鍌滃弳濠电娀娼уΛ娆撳闯鐟欏嫨浜滄い鎺嗗亾妞ゆ垵顦～蹇涘传閸曨厽娈鹃梺鎼炲劗閺呮稓绮垾鏂ユ斀闁宠棄妫楁禍婊堟煕閻斿憡缍戞い鏇悼閹风姴顔忛鍏煎€梻浣稿閸嬫帡宕戦崨鎼晛闁搞儺鍓氶埛鎺懨归敐鍛暈闁哥喓鍋ら弻鐔虹矙濞嗗墽鍚嬮梺璇″枤閸忔﹢宕洪敓鐘插窛妞ゆ挾鍠愯ⅲ闂傚倷绶氬褏鎹㈤崱娑樼婵犻潧顑呯壕鍧楁煟閺傚灝鎮戦柣鎾崇箰閳规垿鎮欓懠顑胯檸闂佸憡鏌ｉ崐婵嬪蓟濞戙垹绫嶉柍褜鍓熼獮鎰板礃瀹割喗缍庡┑鐐叉▕娴滄粌顔忓┑鍡忔斀闁绘ɑ褰冮埀顒佹倐閿濈偞鎯旈妸锔规嫼缂備礁顑嗛娆撳磿閹扮増鐓欓柣鐔哄閸犳鈧娲栭幖顐﹀煡婢舵劕顫呴柣妯诲絻缁侇噣姊绘笟鈧褏鎹㈤崱妤婄€剁憸鏃堢嵁韫囨拋娲敂閸涱亝瀚奸梻浣告啞缁嬫垿鏁冮敂鍓т笉闁瑰墽绮崐鍨叏濡厧甯堕柣蹇ョ畵閺屻劌鈹戦崼婵呮闂佽桨绀侀崯鏉戠暦閹烘垟妲堟慨姗嗗墮缁犵偤姊绘担渚敯闁稿鍔楅幃浼村Ψ閳轰胶鍔﹀銈嗗笒閸燁垶鎮甸鍛弿闁挎繂妫楁慨宥嗘叏婵犲啯銇濇鐐村姈閹棃濮€椤喚鍠橀梻鍌欑劍閹爼宕濊箛鎾愭盯宕熼娑樹患闂佺粯鍨煎Λ鍕劔闂備礁鐤囧銊╂嚄閼告娈介柛銉墯閳锋垿鏌ｉ悢鐓庝喊闁搞倗鍠庨埞鎴︻敍閿濆懎绠归柛妤呬憾閺岀喓绱掗姀鐘崇亶闂佹娊鏀辩敮鎺楁箒闂佹寧绻傚ú銊╁箺閸屾稓绠鹃柛蹇曞帶婵牓鏌曢崶褍顏€殿噮鍣ｉ崺鈧い鎺戝閸嬪鏌ｅΟ鍨毢闁哄棴绠撻弻鈩冨緞鎼搭喖娈鹃梺鍝勫暙閻楀棗顔忓┑鍥ヤ簻闁哄啫鐗婇敍鐔兼煙椤曞棛绉慨濠傤煼瀹曟帒鈻庨幋锝囩崶闂備礁鎲￠幐濠氭偡瑜忛崚鎺楀籍閸喎浠洪梺鍛婄☉閿曘倖绂嶅Δ鈧埞鎴︽倷閺夋垹浠ч梺鎼炲妽濡炰粙骞冮敓鐘插嵆闁靛骏绱曢崢鎼佹⒑閹肩偛鍔橀柛搴ㄤ憾閹﹢顢旈崨顖滎啎闂佺懓鍟块崲鎻捗洪敃鍌涘亗闁绘棁鍋愰崣鎾绘煕閵夛絽鍔氶柛鏂诲€楃槐鎺楀煢閳ь剟宕戦幘缁樷拻濞达絽鎲￠幆鍫ユ煟椤撶儐妲烘俊鍙夊姍瀵挳鎮㈡笟顖涚カ闂備焦瀵уú鏍磹瑜版帒鍚圭€广儱顦伴悡蹇擃熆閼哥數娲存俊缁㈠櫍閺屾稒绻濋崶銊︽殸婵烇絽娲ら敃顏堝箖濞嗘搩鏁傞柛鏇樺妼娴滈箖鏌″搴ｄ汗婵炲樊浜滈悡娑㈡煕閹板吀绨芥い锔诲灡缁绘盯骞橀弶鎴犲姲闂佺顑嗛幐楣冨焵椤掑喚娼愭繛鍙壝—鍐╃鐎ｎ亝妲梺閫炲苯澧柕鍥у楠炴帒顓兼径瀣碘偓鏍ㄧ箾鐎涙鐭嬬紒顔芥崌瀵鎮㈤崗鐓庘偓缁樹繆椤栨繃顏犲ù鐘虫綑椤啴濡堕崨顔绢洶婵炲瓨绮犳禍顏勵嚕婵犳碍鏅搁柣妯哄级閵囨繃绻涙潏鍓ф偧妞ゎ厼鐗撻敐鐐侯敃閿旇В鎷洪梻渚囧亞閸嬫盯鎳熼娑欐珷妞ゆ洍鍋撻柡灞剧☉铻ｉ柤濮愬€楅悡澶愭倵鐟欏嫭绀冮柛銊ユ健閻涱喖螣閼测晝锛滃┑鈽嗗灣閸樠囩嵁閸儲鈷掑ù锝囩摂濞兼劖绻濋姀鈭╂垿寮查懜鍨劅闁靛鍎抽ˇ顐︽⒑閸濆嫷妲归柨娑欐礋閹兘鏌囬敂鎯у汲闂備礁澹婇崑渚€宕规繝姘辈妞ゆ劧闄勯埛鎴︽偣閸ワ絺鍋撻搹顐や憾闂備焦瀵уú蹇涘垂娴犲宓侀柛鎰╁妷閺嬪酣鏌熺€电浠滅痪缁㈠灦閺屸剝寰勭仦鎴掓埛婵犫拃鍕垫疁闁诡垰鐗愮粻娑樷槈濞嗗本瀚藉┑鐐舵彧缂嶁偓婵☆偄瀚板畷銉ㄣ亹閹烘挾鍘遍梺缁樏崯鎸庢叏瀹ュ洠鍋撶憴鍕缂佽鐗嗛锝夊箻椤旂⒈娼婇梺缁橈耿濞佳冾嚕閵娿儮鏀介柣姗嗗枛閻忛亶鏌涢埡鍌滃⒌鐎规洘绻堝鎾偐閸忓摜鐟濈紓鍌氬€烽悞锕佸綘婵炲瓨绮嶇划鎾诲蓟閳ユ剚鍚嬮柛鎰╁妼椤姊虹紒妯绘儎闁搞劋鍗抽垾锔炬崉閵婏箑鏋傞梺鍛婃处閸撴盯藝閵娧呯＝濞达綀娅ｇ敮娑氱磼鐠囪尙澧︽鐐插暞缁傛帞鈧綆鍋掑Λ鍐ㄢ攽閻愭潙鐏︽い銊ョ墦楠炴垿寮撮悢铏圭槇缂佸墽澧楄摫妞ゎ偄锕弻娑氣偓锝庝簻椤忣厽銇勯姀鈥冲摵妞ゃ垺锕㈤幃娆忣吋韫囨洜袦濡ょ姷鍋涘ú顓烆嚕閸洖鍨傛い鏃囧亹閵堫噣姊婚崒娆掑厡缂侇噮鍨伴～蹇旂節濮橈絺鍋撻敃鍌氶唶婵犲灚鍔曞▓銊╂⒑缁夊棗瀚峰▓鏇㈡煃闁垮鐏╅柟鍙夋倐閹囧醇濠靛牏鎳栭梻浣瑰缁嬫捇宕伴幘鑸殿潟闁圭儤顨呴悞鍨亜閹哄秶鍔嶅┑顖涙尦閺屟嗙疀濞戞帒浜炬繛鎴炴皑閹插綊姊婚崒娆掑厡闁硅櫕鎹囬幃娲Ω瑜庨浠嬫煟閵堝骸鐏涘ù婊冪秺閺岀喖鎮滃Ο璇查瀺濠电偛鎳忕划鎾愁潖濞差亜绠归柣鎰絻婵矂姊洪崨濠冪叆缂佸缍婇獮鍐偨閸撳弶鏅滈梺绯曞墲閻熝囨晬濠靛洨绠鹃弶鍫濆⒔閸掍即鏌熺喊鍗炰簻闁哄棌鏅犲缁樻媴娓氼垳鍔哥紓浣虹帛閸ㄥ潡寮崘顔碱潊闁绘瑢鍋撻柛銈嗘礀閳规垿鎮╁畷鍥舵殹闂佸搫鎳忛幃鍌炲蓟閿濆憘鐔煎垂椤旂偓顕楁俊鐐€х紓姘跺础閸愬樊娼栧┑鐘宠壘绾惧吋鎱ㄥΟ鍝勮埞妞ゃ倐鍋撶紓鍌氬€风粈渚€顢栭崱娑樼闁煎鍊楅惌澶愭煙閻戞﹩娈㈤柡浣告喘閺岋綁骞囬妸锔界彆闂佸搫鍊甸崑鎾绘⒒閸屾瑨鍏岀紒顕呭灦閺佸啴鍩￠崨顓狅紱闂佸憡娲﹂崜娑€呴弻銉︾厵妞ゆ牕妫楅張顒傗偓闈涚焸濮婃椽妫冨☉姘暫濠碘槅鍋呴悷褎绂嶉幖渚囨晜闁割偆鍠撻崢閬嶆⒑闂堟侗鐓梻鍕婵￠潧鈹戦崼姘壕閻熸瑥瀚粈鍐╃箾閸欏澧抽柣蹇撳暣濮婃椽宕ㄦ繝浣虹箒闂佸摜濮靛畝绋跨暦閵忋値鏁傞柛娑卞灣閻﹀牓姊洪棃娑㈢崪缂佽弓绮欓幃锟犲箛閻楀牏鍙嗗┑顔筋焾閸╂牠鎮￠悢鑲╁彄闁搞儯鍎遍崝銈夋煙椤斿灝浜圭紒杈ㄥ笚濞煎繘濡搁妷锕佺檨闂備胶纭堕弬渚€宕戦幘鎰佹富闁靛牆妫楃粭鍌滅磼閳ь剚鎷呴悜姗嗗仺闂佺粯鍔楅崕銈夋偂濞嗘挻鈷戞い鎾卞妿閻ｈ鲸顨ラ悙杈捐€块柡灞剧☉椤粓鍩€椤掍胶顩查悹杞拌濞兼牗绻涘顔荤凹闁稿绻濋弻锝呂旈埀顒勬偋婵犲洤纾块柟鐐墯濞撳鏌曢崼婵囶棞闁诲繈鍎甸弻鐔兼惞椤愵剝鈧法鈧娲樺浠嬪春閳ь剚銇勯幒宥夋濞存粍绮撻弻鐔煎传閸曨厜銈夋煕閺傝法肖缂佽鲸甯￠崺鈧い鎺戝€甸崑鎾绘晲鎼粹剝鐏嶉梺缁樻尰閻燂箓濡甸崟顖氬嵆婵°倐鍋撳ù婊冨⒔缁辨挻鎷呴崫鍕戯綁鏌ｅΔ鍐ㄢ枅闁绘侗鍣ｅ畷姗€濡搁姀锛勨偓濠氭⒑閻熸壆浠㈤柛鐕佸灦瀵煡濮€閵堝棌鎷虹紓浣割儐鐎笛冿耿閻楀牄浜滈柕濞垮劜椤ャ垽鏌熼鏂よ€跨€规洘锕㈤、娆撴偩鐏炶棄绠洪梻浣侯攰婢瑰牓骞撻鍡橆潟闁哄洢鍨洪崑鈺傘亜閹惧崬鐏柣鎾存礋楠炴牕菐椤掆偓婵℃椽鏌熼懠顒€浜炬い銊ｅ劦閹瑧鎷犺閺侀箖姊虹拠鈥崇仭婵☆偄鍟村畷?濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁惧墽绮换娑㈠箣濞嗗繒鍔撮梺杞扮椤戝棝濡甸崟顖氱閻犺櫣鍎ら悗楣冩⒑閸涘﹦鎳冪紒缁橈耿瀵鍩勯崘銊х獮闁诲函缍嗘禍鐐哄礉缁嬪簱鏀介柣鎰皺濮ｇ偤鏌￠崨顖氣枅鐎殿喖顭锋俊鎼佸煛娴ｈ櫣娼夐梻浣侯焾閺堫剟鎯岄鐣岀彾闁哄洢鍨洪崐鐢告偡濞嗗繐顏紒鈧埀顒勬倵濞堝灝鏋涘褍閰ｉ獮鎴﹀閻橆偅顫嶉梺闈涚箳婵兘顢橀崫鍕ㄦ斀闁绘劕寮堕ˉ婊呯磼缂佹ê绗氱紒鍌涘浮閹虫牠鍩￠崘顏庣闯濠电偠鎻徊浠嬪箹椤愶絿澧℃繝鐢靛Х閺佹悂宕戝☉銏℃櫇妞ゅ繐瀚烽崵鏇炩攽閻樺疇澹橀柛妤佸▕閺岋綁寮崼顐ｎ棖缂備焦鍔楅崑銈咁潖濞差亜绀堥柟缁樺笂缁ㄨ偐绱撴担绛嬪殭闁稿﹤缍婇獮鍫ュΩ閿旇棄鍔呴梺闈涱煭閼靛綊骞忛搹鍦＝闁稿本鐟ч崝宥夋嫅闁秵鍊堕煫鍥风到瀵噣鏌＄仦鍓р姇缂佺粯绻堝畷鎺戔槈濡妯婇梺璇插椤旀牠宕伴弽顓涒偓锕傛倻閽樺鐎梺鐟板⒔缁垶宕戦幇鐗堢厾缁炬澘宕晶鍓х磼鏉堛劎鍙€婵﹦绮粭鐔煎焵椤掆偓椤洩顦归柟顔ㄥ洤骞㈡繛鎴烆焽閻ゅ洭鏌熼懝鐗堝涧缂佸弶瀵х粙澶婎吋婢跺鍘遍梺鎸庢椤曆囩嵁濡偐妫?闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢亸浣圭€婚梻鍕喘椤㈡俺顦查悡銈嗐亜閹垮啯濞囬柍褜鍏涚粈渚€锝炲鍫濈劦妞ゆ帊鐒﹂崣蹇涙煃瑜滈崜鐔煎蓟閿濆拋娼ㄩ柍褜鍓欓…鍥槼闁?
    // 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ姀鐘差棌闁轰礁锕弻鈥愁吋鎼粹€崇闂傚倸鍋嗛崹閬嶅Φ閸曨垰鍐€闁靛ě鍛帎缂傚倷娴囩紙浼村磹濠靛钃熼柨婵嗩槹閺呮煡鏌涘☉鍗炴灍闁圭兘浜堕幃妤冩喆閸曨剛顦ㄩ梺缁樻惈缁绘繂顕ｆ繝姘亜闁告稑锕︾粔鍫曟⒑鐠団€崇€婚柛娑卞暕閹烘鈷掑ù锝堝Г閵嗗啰绱掗埀顒佺瑹閳ь剙鐣烽鐑嗘晝闁挎洍鍋撻柣鎾达耿閺岀喐娼忛崜褏鏆犵紒鐐劤閸氬鎹㈠┑鍥╃瘈闁稿本绋掑畷鎶芥⒑閸涘﹥鈷愰柟顔煎€垮濠氭晲婢跺浜滈梺鍛婄缚閸庢煡宕甸崒鐐粹拻闁稿本鑹鹃鈺呮倵濮樺崬顣煎ǎ鍥э躬閹晫绮欑捄顭戞Ч婵＄偑鍊栭悧妤呮嚌妤ｅ啫鑸归柕濞炬櫆閳锋垿姊婚崼鐔剁繁闁绘帡绠栭弻娑㈠棘鐠恒劎鍔柦妯煎枑缁绘繈妫冨☉鍗炲壉闂佺顑囨慨鐢稿箞閵娿儙鐔煎传閸曨喖鐓橀梻浣告惈椤戝棛绮欓幒妤€绠為柕濠忓缁♀偓闂佸憡娲﹂崢鑲╃箔閿涘嫮纾藉ù锝堫嚃濞堟﹢鏌涢悩宕囧ⅹ闁伙絿鍏橀獮鍡涙焽閿斿彨顏堟⒒娴ｇ瓔鍤欑紒缁樺姍閹囧箻閹颁焦缍庡┑鐐叉▕娴滄繈鎮炴繝姘厽闁归偊鍨伴拕濂告倵濮橆厽绶叉い顓″劵椤﹂亶鏌涘Δ浣糕枙鐎殿喛顕ч埥澶愬閳ュ厖绨婚柣搴ｆ嚀鐎氫即宕戞繝鍥х濠㈣埖鍔栭崐鐢告煕椤垵浜濈紒鑸电叀閹顫濋悡搴㈢亶缂備礁鍊圭敮锟犲极閸愵喖纾兼慨妯夸含閺嗕即鏌ｉ悢鍝ョ煀缂佺粯锕㈠顐㈩吋閸垻鏉稿┑鐐村灦閻熴垽骞忕紒妯肩閺夊牆澧介幃濂告煕鎼存稑鈧繂鐣烽悽绋跨劦妞ゆ帒瀚埛鎴︽倵閻ф悂妾繝銏∩戞穱濠囧锤濡や胶鍘遍梺鍦劋閹稿摜绮鑸电厵妞ゆ牗绋掗ˉ鍫ユ煃閵夘垳鐣垫鐐差儔閹瑧鈧稒顭囪ぐ鍡涙⒒閸屾瑧顦﹂柟纰卞亰钘濇い鏍仦閸嬪鏌涢弴銊ョ仩缂佲偓婢舵劕绠归弶鍫濆⒔閹厧鈽?up闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢悰鈥充壕闁汇垺顔栭悞鎯归悩娆忔处閳锋帡鏌涢弴銊ヤ簻妞ゅ浚鍙冮弻鈥崇暆鐎ｎ剛鐦堥悗瑙勬礃閿曘垺淇婇幖浣肝ㄩ柕蹇曞С婢规洟姊洪崨濠勭細闁稿孩鐓￠、?*闂傚倸鍊搁崐鎼佸磹瀹勬噴褰掑炊椤掑鏅悷婊冪Ч濠€渚€姊虹紒妯虹伇婵☆偄瀚竟鏇熺節濮橆厾鍙嗗┑鐘绘涧濡厼危閸濄儳妫柟顖嗗懐楔闂佸搫澶囬埀顒佸墯閸氬骞栫划鍏夊亾瀹曞浂鍟囩紓鍌欒兌閸嬫捇宕曢崘宸劷闁跨喓濮撮拑鐔兼煏閸繃鍋柛锔诲幐閸嬫捇鏁愭惔鈥茬凹闂佺粯绻冨ú鐔煎箖?12 缂?*闂?
    // first-run-setup 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯顢曢敐鍡欘槬闂佺琚崝搴ㄥ箟閹间礁绫嶉柛顐ｆ儕閵夆晜鐓曢柟鑸妽濞呭棝鏌涙惔锝呮灈闁哄本娲濈粻娑氣偓锝庝簽閸旀潙鈹戦悙璺虹毢妞ゎ厼鐗撻崺鐐哄箣閿旇棄浜归柣鐘叉厂閸愌呯煑闂傚倷鑳堕幊鎾诲疮鐠恒劍宕查柟鎵閸嬧晠鏌ｉ幋锝嗩棄缂佺姷鏁婚弻鐔烘喆閸曞灚鐣剁紓鍌氱Т閿曨亜顕ｉ锕€绠涢柡澶婄仢缁愭稑顪冮妶鍡橆梿婵炲娲熼妴渚€宕ㄧ€涙ǚ鎷绘繛杈剧到閹芥粓鏁嶅鍥╃＜妞ゆ洖鎳庨悘鐘绘偂閵堝鐓熼柡鍐ㄥ€哥敮鍫曟煢閸愵亜鏋涢柡灞诲妼閳规垿宕遍埡鍌傦箑鈹戦埥鍡椾簼闁挎洏鍨藉璇测槈閳垛斁鍋撻敃鍌氱婵犻潧娲ㄨぐ顖炴⒒娴ｅ憡鎯堟俊顐㈤叄瀹曞綊鎳￠妶鍡╂綗闂佽鍎抽悺銊﹀垔閹绢喗鐓熼柣鏂挎啞缁跺弶銇勯銏⑿㈡い顏勫暣婵″爼宕橀妸銉€抽梻浣瑰濞诧附绂嶅鍫晪闁挎繂妫涚弧鈧┑顔斤供閸樺吋绂嶅鍕箚闁绘劕妯婇崕蹇涙倵濞戞帗娅呴柍缁樻尭閳规垹鈧綆鍋€閹风粯绻涢幘纾嬪婵炲眰鍊曢锝夊锤濡や讲鎷婚梺鍛婃处閸嬪嫰顢旈鍫熺厽闁炽儱纾粻缁樻叏婵犲偆鐓肩€规洘甯掗埢搴ㄥ箣椤撶啘婊勭節閻㈤潧袥闁稿鎹囬弻娑樜旈崘銊ゆ睏闂佸搫顑呯粔褰掑蓟閺囷紕鐤€閻庯綆浜炴导鍕磽娴ｆ彃浜炬繝鐢靛Т娴煎懓銇愰幒鎾存珳闂佸憡渚楅崣搴∥涘畝鍕拺闁硅偐鍋涢埀顒佺墪鐓ら柣鏂款殠濞兼牕鈹戦悩瀹犲闂佸崬娲弻锝夊棘閸噮鏆㈢紓浣筋唺缁舵艾顫忓ú顏勫窛濠电姴鎳庨ˉ婵嗩渻閵堝棗鐏ユい锕傛涧椤曪絿鎷犲ù瀣潔濠碘槅鍨堕弨閬嶏綖瀹ュ鈷戦柛娑橆煬濞堟﹢鏌涚€ｎ偆銆掗柍褜鍓氶崘鑽ゅ緤娴犲鐓橀柟杈鹃檮閸婄兘鏌熺紒妯虹瑲闁稿鍨辩换婵嬫偨闂堟稐绮堕梺鎸庢磸閸婃繈鏁愰悙鍓佺杸闁瑰彞鐒﹀浠嬨€侀弮鍫濈妞ゆ挆鍐╂毆闂傚倷鑳堕、濠冪珶閸儱绠垫い蹇撴缁躲倝鏌涘┑鍡楃弸闁挎繂顦柋鍥煏婢舵稖鍚傞柟宄版惈閳规垿鎮欑捄楦挎暱濡炪倖娉﹂崟顒佺彿濡炪倖娲嶉崑鎾绘煛鐏炵偓绀嬬€规洖銈搁幃銏㈢矙鎼存挸浜鹃柟閭﹀弾濞堜粙鏌ｉ幇顓炵祷闁哄棴绲炬穱濠囶敃閵忕姵娈梺瀹犳椤︾敻鐛幒妤€绠奸柛鎰ㄦ櫇缁夐箖姊婚崒姘偓椋庢濮橆剦鐒界憸蹇涘箲閵忋倕绠抽柟鐐綑瀵潡姊虹紒妯虹仴濠㈣鐟╁畷鍫曨敆婢跺娅囬梻浣瑰濮婂寮查锕€鍌ㄥù鐘差儐閳锋垹绱撴担鍏夋（妞ゅ繐瀚烽崵鏇㈡煠閹间焦娑х紒鍓佸仱閺屾盯寮撮妸銉ョ闂佸摜濮甸崝娆撳蓟閳╁啫绶炲┑鐘插閾忓酣姊洪崫鍕紨缂佹彃娼￠崺鈧い鎺嶇贰閸熷繘鏌涢悩鎰佹畽闁靛洦鍔欏畷姗€顢欓懖鈺佸箳闂佺懓鍚嬮悾顏堝礉瀹€鈧划缁樼節濮橆厾鍘告繝銏ｆ硾椤戝懘鎮橀敐澶嬬厱?up 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍讲鏋欓梺缁樺笩婵倗鎹㈠☉銏犲耿婵☆垵顕х喊宥夋煟閻斿摜鎳曠紒鐘虫崌楠炲啫螖閸涱喗娅滈柟鑲╄ˉ閸撴繈鎮樺澶嬧拺闁硅偐鍋涢埀顒佹礋閹崇喖顢涘┑鍫滅胺闂傚倷鐒︾€笛呮崲閸岀偛绠犻幖娣妼濮瑰弶绻涢崱妯虹仼缁炬儳銈搁弻锝呪枎鐏炴垝澹曢梻浣规偠閸斿矂鎮ラ悡搴殨濠电姵纰嶉崑鍕倶韫囧海鍔嶉柣蹇庣窔濮婃椽宕滈懠顒€甯ラ梺鍝ュУ椤ㄥ﹪骞冨鈧崺鈧い鎺戝閳锋帒霉閿濆嫯顒熼柣鎺斿亾閵囧嫰骞嬪┑鍥舵＆濡ょ姷鍋為崹鍨暦閸洦鏁嗛柍褜鍓涚划濠氬冀椤撶喓鍘介梺纭呮彧缁查箖藟婢跺瞼纾奸柣娆愮懃濞诧箓鎮″▎鎴犵＜閻庯綆浜炴禒銏°亜閹哄鐏柕鍥у瀵剙鈻庨悙顑箓姊虹€圭媭娼愰柛銊ユ健楠炲啴鍩￠崨顓犵杸婵炶揪绲介幉鈥斥枍濞戙垺鈷掑ù锝呮啞閹茬鈹戦鐐毄闁哥姴锕ら濂稿炊閵娿儱绨ユ繝鐢靛仦閸垶宕硅ぐ鎺撳€块柛顭戝亖娴滄粓鏌熼悜妯虹仴妞ゅ浚浜弻锝夊箻鐎电硶妲堥梻鍥ь樀閺屻劌鈹戦崱妯烘闂佺懓寮堕幃鍌氼嚕椤愶箑閱囬柕澶涚畱閳ь剛鏁婚幃宄扳枎韫囨搩浠剧紓浣插亾闁割偁鍎查悡鏇熶繆椤栨粌孝婵炲眰鍊濋幃鈥愁潨閳ь剟寮婚悢鍛婄秶闁告挆鍛闂傚倷绀佸畷顒€煤椤撱垹钃熼柨婵嗩槸濡﹢鏌熺粙鍨劉闁哥姵鍔欏缁樻媴閽樺－鎾绘偨椤栨稑娴柛鈺冨仱楠炲鏁傞挊澶夋睏闂備礁澹婇悡鍫ュ磻閸涱厜鎺楀礋椤栨稈鎷虹紓浣割儐椤戞瑩宕曢幋锔界厱閻庯綆鍋嗗ú鎾煃閵夘垳鐣电€规洘鍎奸ˇ顕€鏌＄€ｎ亪鍙勯柡宀€鍠栧畷褰掝敊閸忓吋顔夐梻浣告惈閹虫劙宕戦妶鍜佹綎婵炲樊浜滃婵嗏攽閻樻彃顏ゅ┑顔兼搐閳规垿鎮欓幓鎺撳€梺鑽ゅ暀閸涱厼鐏婃繝鐢靛У閼归箖宕橀埀顒勬⒑闂堟侗鐒鹃柛濠冾殘缁顫濋懜纰樻嫼闂傚倸鐗婄粙鎾存櫠閺囥垺鐓欓柛鎰叀閸欏嫮鈧鍠楁繛濠囧箖閵忋倕绀傞柤娴嬫櫅楠炲牊绻濆閿嬫緲閳ь剚鎹囬幊妤呭醇閺囨ǚ鍋撻崒鐐茶摕闁靛濡囬崢顏堟⒑閹肩偛鍔€闁告劕褰炵槐鏃堟煟鎼淬埄鍟忛柛鐘虫礈閸掓帒鈻庨幇顏嗙畾濠殿喗绻傞惌鍫澪ｉ幆褜娓婚柕鍫濇鐏忕敻鏌涘Ο鐘叉搐閻撴﹢鏌熸潏楣冩闁稿﹦鍏橀幃瑙勬姜閹峰矈鍔呭┑鈽嗗亜閻楀棝鍩為幋锔藉€风€瑰壊鍠楁晥闂備胶鍎甸弲鈺呭垂娴煎瓨鏅濋柕蹇婂墲婵绱掗娑欑闁哄倵鍋撻梻鍌欒兌缁垶鈥﹂崼銉晪婵犲﹤鍠氶崯鍛存煏婢跺棙娅嗛柛濠傜仛閹便劌螣閻撳骸浠樻繝鈷€鍕棆缂佽鲸甯￠、姘跺川椤撶偟鍝楁俊鐐€х粻鎴犲椤撶姵顫?
    // 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛銈呭閺屾盯顢曢敐鍡欙紩闂侀€炲苯澧剧紒鐘虫尭閻ｉ攱绺界粙璇俱劍銇勯弮鍥撴繛鍛墦濮婄粯鎷呴搹骞库偓濠囨煕閹惧绠為柟顔惧厴婵＄兘鍩℃担鍝モ偓顓炩攽閻樼粯娑ч柣妤€鍟村畷鎴﹀箻閼姐倕绁﹂梺鍓茬厛閸犳牗鎱ㄦ惔鈾€鏀介柣鎰邦杺閸ゅ绱掗悩铏碍妞ゆ洩缍佸畷妯侯啅椤旀儳寮抽梻浣稿閸嬪棝宕伴幘璇插偍闁瑰墽绮崑鈩冪節婵犲倸顏柣顓熷笧閳ь剝顫夊ú妯煎垝閹捐绠栭柕蹇嬪€栭崑鍌炲箹濞ｎ剙鈧牠骞夋导瀛樷拻濞撴埃鍋撴繛浣冲洠鈧箓宕奸妷銉э紵濡炪倖鏌ㄥΣ鍫ユ晲婢跺﹪鍞堕梺缁樻煥婢у海绮诲顒夋富闁靛牆妫涙晶顒傜磼鐠囨彃鈧湱绮嬪澶嬪仭闁瑰啿锕ょ紞濠囧箖閳╁啯鍎熼柕蹇曞Х閻涒晝绱撻崒娆愮グ濡炲瓨鎮傞獮鎰版嚒閵堝洨鐣堕梺缁樻⒒閸樠囨倷婵犲嫭鍠愰幖娣妽閸婅泛銆掑锝呬壕濠殿喖锕ュ钘夌暦閵婏妇绡€闁告劧绲剧€氬姊绘担瑙勫仩闁告柨閰ｅ顐ｇ節濮樸儮鍋?5454 婵犵數濮烽弫鍛婃叏閻戣棄鏋侀柛娑橈攻閸欏繘鏌ｉ幋锝嗩棄闁哄绶氶弻鐔兼⒒鐎靛壊妲紒鎯у⒔缁垳鎹㈠☉銏犵闁绘劕鐏氶崳褏绱撴担绋款暢闁稿鍊濆璇测槈閵忕姈銊︺亜閺冨倸甯舵い顐熸櫊濮婅櫣绱掑Ο铏圭懆闂佽绻戝畝鍛婁繆閻㈢绀嬫い鏍ㄦ皑椤斿﹪姊虹化鏇炲⒉闁靛洨鍋炵粩鐔煎即閵忥紕鍘介柟鍏肩暘閸╁嫰宕箛娑欑厱闁绘ê纾晶鐢告煙椤斿搫鍔滅紒铏规櫕缁瑧鎹勯妸褎姣庨梻鍌欑劍閹爼宕曢鐐茬閹兼番鍔婇埀顒€鎳橀幃婊堟嚍閵夈儮鍋撻悽鍛婄叆婵犻潧妫濋妤€霉濠婂懎浠遍柡灞剧☉铻ｉ柛婵嗗閸╃偤姊虹€圭媭娼愰柛銊ユ健閵嗕礁鈻庨幋鐘烩攺闁诲函缍嗛崜娆愭叏濞差亝鈷掑ù锝呮啞閸熺偞銇勯鐐搭棦鐎规洘鍎奸ˇ鏌ユ煕鐎ｎ亞效婵﹤鎼晥闁搞儜鍛偅闂備胶顢婇婊呮崲濠靛绠?444 闂傚倸鍊搁崐鎼佸磹閹间礁纾瑰瀣捣閻棗銆掑锝呬壕濡ょ姷鍋為悧鐘汇€侀弴姘辩Т闂佹悶鍎洪崜锕傚极閸愵喗鐓ラ柡鍥殔娴滈箖姊哄Ч鍥р偓妤呭磻閹捐埖宕叉繝闈涙川缁♀偓闂佺鏈划宀勩€傚ú顏呪拺闁芥ê顦弳鐔兼煕閻樺磭澧电€殿喖顭峰鎾偄閾忚鍟庨梻浣稿閻撳牓宕伴弽銊х彾?502闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾惧綊鏌ｉ幋锝呅撻柛濠傛健閺屻劑寮崼鐔告闂佺顑嗛幐鍓у垝椤撶偐妲堟俊顖濐嚙濞呇囨⒑濞茶骞楅柣鐔叉櫊瀵鎮㈢亸浣圭€婚梻鍕喘椤㈡俺顦查悡銈嗐亜閹垮啯濞囬柍褜鍏涚粈渚€锝炲鍫濈劦妞ゆ帊鐒﹂崣蹇涙煃瑜滈崜鐔煎蓟閿濆拋娼ㄩ柍褜鍓欓…鍥槼闁逛究鍔戦弫鍌炲箣閹烘梻鐣鹃梻浣虹帛閸旓附绂嶅鍫濈劦妞ゆ帊鑳舵晶顏呫亜閺傝法绠茬紒缁樼箓椤繈顢楅崒锔惧耿闂傚倷鑳堕幊鎾存櫠閻ｅ苯鍨濇い鏍仦閸嬪倿鏌￠崶鈺佷汗闁衡偓娴犲鐓熸俊顖濐嚙缁茶霉濠婂懎浠遍柡宀嬬秮楠炴﹢鎮欓挊澶夊垝闂備礁鎼惉濂稿窗閺嶎厹鈧礁鈻庨幘鏉戞疅闂侀潧顧€缁插ジ鎼规惔顫箚闁靛牆娲ゅ暩闂佺顑嗛惄顖炪€侀弽銊ョ窞闁归偊鍓涢悾娲⒑閸愬弶鎯堥柟宄邦儔瀵偊宕橀埞澶哥盎濡炪倖鍔﹂崑鍕嵁閺嶃劊浜滈柕澶涚畱閸濈儤鎱ㄦ繝鍕笡闁瑰嘲鎳橀幐濠冨緞婵犲啫鍔掗梻鍌欑閹芥粍鎱ㄩ悽绋跨闁哄洠鎳囬埀顒婄畵瀹曞綊顢氶崨顔肩紦闂備線鈧偛鑻晶顔姐亜閺囶亞绉鐐查叄閹崇偤濡烽妷銈囩泿闂傚倷鐒﹂崕宕囨崲閹邦剨鑰块梺顒€绉甸弲顒佺節婵犲倸鏋ゆ繛鎾愁煼閺屾洟宕煎┑鍥舵！闂佺粯鎸堕崕鐢稿蓟閻斿搫鏋堥柛妤冨仒缁ㄥジ姊洪柅娑氣敀闁告梹鍨甸悾鐑藉箳閹存梹鏁犻梺璇″瀻閸屾凹妫滃┑鐘垫暩閸嬫盯鎮ч崱娑欏€舵繝闈涱儏閸戠姵绻涢幋娆忕仾闁绘挾鍠栭弻鈥崇暤椤旇崵顦﹀ù鐙€鍠栬灃闁绘﹢娼ф禒婊堟煕閻旈鎽犻柟渚垮姂閸┾偓妞ゆ帒瀚悡蹇涙煕椤愶絿绠栨い銉у█閺屾稑鈻庤箛鏃戞＆闂佸搫鐭夌徊楣冨箚閺冨牜鏁嶆繝濠傛啗閿濆棛绠鹃悗娑欘焽閻鏌ｅΔ鍐ㄐ㈤柣锝囧厴楠炲鏁冮埀顒傜不閼姐倗纾藉ù锝咁潠椤忓懐顩锋繛宸簼閳锋垿鏌涘☉姗堝姛闁宠棄顦甸弻銊╁即椤忓棛鏆ら悗瑙勬磸閸ㄤ粙寮婚崱妤婂悑闁糕€崇箲鐎氬ジ鏌ｉ悢鍝ョ煂濠⒀勵殘閺侇噣骞掗幘棰濇锤婵炲鍘ч悺銊╂偂閺囥垺鐓欓柟顖嗗苯娈堕梺瀹犳椤︾敻寮婚敓鐘插耿闊洦姊归悵姘攽椤旂》鏀绘俊鐐扮矙楠炲﹪鎮欓崫鍕庛劍銇勯弮鈧崕铏叏濞差亝鈷掑ù锝呮啞閹茬鈹戦鑺ュ唉闁哄苯锕弫鎰緞婵犲啯袣闂備線娼ч¨鈧紒鐘冲灩濞嗐垽鎮欓悜妯煎幍闂備緡鍙忕粻鎴炴櫠閻楀牄浜滈柡鍐ｅ亾閻㈩垱甯￠垾鏃堝礃椤斿槈褔鏌涢埄鍐噧妞ゎ値鍥ㄢ拺婵炶尪顕у楣冩煕閻樻剚娈樼紒顔规櫊閹垽鎮℃惔鈥崇ギ闂備胶绮Λ浣糕枍閿濆鍎婇柛顐犲劜閳锋垶鎱ㄩ悷鐗堟悙闁诲繐寮剁换娑欐媴閸愭彃顏い鈺呮敱缁绘繃绻濋崒姘间还缂備浇顕уΛ娆撳Φ閸曨垰绠涢柍杞拌兌娴煎嫰姊虹紒妯虹瑲闁诡喖鍊垮璇测槈閵忕姷鍘撮梺璇″瀻閸屾凹妫滃┑鐘殿暜缁辨洟宕戦幋锕€纾归柡宥庡亝閺嗘粓鏌熼悜姗嗘當闁汇値鍠楅妵鍕冀椤愵澀娌梺绋款儑閸犳劙濡甸崟顔剧杸闁规崘娉涢·鈧梻浣虹帛閹搁箖宕伴弽顓炶摕闁哄洨鍠撶弧鈧棅顐㈡处濞叉粓寮抽悩缁樷拺閻犲洩灏欑粻姘舵煛閸涱喚绠樻俊鍙夊姍楠炴帡骞樼€靛摜肖闂備線娼ч…鍫ュ磿閹剁瓔鏁勯柛宀€鍋為埛鎴︽⒑椤愩倕浠滈柤娲诲灡閺呭爼宕滆绾剧晫鈧箍鍎遍幊蹇浰夐悙鐢电＜闁稿本姘ㄥ瓭濡炪値鍘归崝鎴濈暦閻撳簶鏀介柛銉簽閸欐垿姊婚崒姘偓椋庢閿熺姴绀堟繛鍡樺灩閻捇鏌ｉ姀鐘冲暈闁稿鍊块弻锝夊棘閸喗鍊梺缁樻尪閸庤尙鎹㈠┑瀣棃婵炴垶鐟Λ锟犳⒑閻熸澘鎮戦柛鏃€娲熷﹢浣糕攽閻樿宸ラ柛鐕佸亰閸╂盯寮崒婊咃紲闁哄鐗勯崝灞矫归閿亾鐟欏嫭绀冮悽顖涘浮閿濈偛鈹戦崰鐗堝笒閳诲秶鎷犻崣澶婃珴闂佺顑嗛幐楣冨箟閹绢喖绀嬫い鎺戝亞濡茬懓鈹戦悙鑼憼缂侇喖鐭傞幃銉︾附閸涘﹤浠掑銈嗘磵閸嬫挾鈧娲栭悥濂搞€佸Δ鍛劦妞ゆ帊绀侀崝鏃堟⒒閸屾瑧绐旀繛浣冲泚鍥敇閵忕姷锛欓梺鍝勬礌閹?
    SetTimer(g_hwnd, TIMER_STARTUP_UP, 12000, nullptr);

    // WARP 濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣銏犲閺佸﹪鏌″搴″箹缂佹劖顨嗘穱濠囧Χ閸涱厽娈查梺鍝勬缁秵绌辨繝鍥ч柛娑卞幗濞堝爼姊哄ú璇插箺闁荤啿鏅犲濠氭偄閸忕厧鈧粯淇婇鐐存珳缂侇喖鐖煎娲焻閻愯尪瀚板褍澧界槐鎺楁偐閾忣偄纰嶉梺浼欑秮閺€杈╃紦閻ｅ瞼鐭欓悹鍥﹀嫎閸斿秹濡甸崟顖氱睄闁搞儺鐏濋幘瀵哥闁告侗鍘惧瓭闂佸疇顫夐崹鍧楀箖閳哄懎鍨傛い鎰剁稻閻﹀骸鈹戦悩鎰佸晱闁哥姵宀稿顐ｇ節濮橆剛鐤囧┑鐘绘涧椤戝懐绮堥崼銏″枑闊洦绋掗崕妤呮煕閳╁啰鈯曢柣鎾存礈閳ь剙鍘滈崑鎾绘煃瑜滈崜鐔风暦閹达附鍊烽柣鎴灻禍妤€鈹戦悙鍙夘棡闁圭顭烽幃鈥斥枎閹扳晙绨婚梺鍝勭Р閸斿酣鍩婇弴鐘电＜闁诡垱婢橀惃娲煙椤曞懎鏋涙い顐ｇ箞閹虫粎鍠婂Ο璇差伖闂傚倷绀佸﹢閬嶁€﹂崼婢濆綊鎮滈挊澶屽帎闂佸搫娲ㄩ崰宥囨崲閸℃ǜ浜滈柟鎵虫櫅閻忊晠鏌￠崱顓犳偧闁逞屽墯椤旀牠宕伴弽顓涒偓锕傛煥鐎ｂ晝绠氶梺褰掓？缁€浣告暜闂備線娼х换鍡椢ｉ崨顓х劷闁哄啠鍋撶紒缁樼箞閹粙妫冨☉妤冩崟婵犵妲呴崑鍛存偡閳轰胶鏆︽繝闈涙焾閺冨牆鐒垫い鎺戝閻撴繈鏌熼幑鎰靛殭閸ュ瓨绻濋姀锝嗙【妞ゆ垵鍊块獮蹇撁洪鍛幐婵犮垼娉涢懟顖涚閹烘鐓曟俊銈傚亾闁哥喎娼￠垾鏍ㄧ節閸ャ劌鈧鏌ら幁鎺戝姢闁告椴哥换婵嬫偨闂堟刀銏ゆ煕閻旂顥嬬紒顔碱煼閹瑩鎮滃Ο鐓庡箺婵犵數鍋為崹闈涚暦椤掑嫬鍑犻柕鍫濐槹閻撳啰鎲稿鍫濈闁靛ě鍛槸闂佺硶鍓濈粙鎴犵玻濡や椒绻嗛柕鍫濇噺閸ｆ椽鏌涚€ｎ亜鈧潡寮婚弴鐔风窞闁糕剝蓱閻濇洟姊虹粙鍖″姛闁轰礁顭峰濠氬Ω閵夈垺鏂€闂佺硶鍓濇笟妤呭焵椤掍緡娈旀い顓″劵椤т線鏌涢妸銉у煟鐎殿喛顕ч鍏煎緞鐎ｎ剙寮抽梺璇插嚱缂嶅棝宕戦崱娴板洭濡歌绾捐棄霉閿濆牊顏犻悽顖涚洴閺岋綁鍩℃繝鍌滀桓闂佺粯渚楅崰姘跺焵椤掑﹦绉甸柛鐘愁殜瀹曟劙鎮滈懞銉у幐婵犮垼娉涢鍛存倶閳哄懏鐓ユ繛鎴烇供濡茬懓菐閸パ嶈含闁诡喗鐟╅、鏃堝礋閵娿儰澹曢梺鍝勮癁閳ь剟寮搁弽顓熺叆闁哄洨鍋涢埀顒€鎽滅划鍫⑩偓锝庡亖娴滄粓鏌熼崫鍕棞濞存粓绠栭幃妤€鈻撻崹顔界亪濠电偛鐨烽埀顒佸墯濞兼牗绻涘顔荤盎缂佲偓閸愵喗鐓曟繛鍡楁禋濡插綊鏌涙惔銏″磳婵﹥妞介幃鐑藉级閹稿孩鐦ｆ繝鐢靛仜椤曨參宕楀鈧悰顔藉緞閹邦剛顔掑銈嗘婵倕鈻嶉崱妯肩瘈婵炲牆鐏濋弸鐔兼煕閺冣偓鐢偤鎳炴潏銊х瘈婵﹩鍘搁幏娲⒑閸涘﹦绠撻悗姘煎枦閳綊姊绘担鍛婂暈闁哄被鍔戦弻濠囨晲婢跺﹦鐣鹃悗鍏夊亾闁逞屽墴閸┾偓妞ゆ帒锕︾粔鐢告煕閻樻剚娈滈柟顔光偓鏂ユ闁靛骏绱曢崢閬嶆⒑閺傘儲娅呴柛鐘宠壘閳绘挸螣閼测晝锛滈梺缁樏幖顐﹀触瑜版帗鐓冪憸婊堝礈濮橀鏁婇柡宥庡幖缁愭鏌″搴″箺闁稿顦埞鎴﹀磼濠婂海鍔搁梺鍝勫閸庣敻寮诲澶婄厸濞达絽鎲″▓鍙夌節濞堝灝鐏￠柟鍛婂▕楠炲啫鐣￠幍铏€婚棅顐㈡处閹尖晜绂掓ィ鍐┾拺缂佸灏呴煬顒勬煛閳ь剚娼忛埡鍌氬簥濠电娀娼уΛ宀勫几閺冨牊鐓曟い顓熷灥閺嬬喖鏌ｈ箛鎾缎ч柡灞诲€濆畷褰掓晝閳ь剙煤閿曞倹鍋傞柡鍥ュ灪閻撴盯鏌涢幇鍓佸埌濞存粓绠栧娲箹閻愭彃顬堝┑鐐点€嬬换婵嬬嵁閸愨晛顕遍柟纰卞幗閺呪晠姊洪崗闂磋埅闁稿酣浜跺畷婵嬪川鐎涙ǚ鎷洪梻鍌氱墛缁嬫挾绮婚悙鐑樼厱闁靛鍎洪悡濂告煙椤栨瑧绐旂€规洖銈告俊鐑芥晜鐟欏嫬顏归梻鍌欑閹诧紕绮欓幋锔芥櫇闁挎繂娲﹂浠嬫⒑椤掆偓缁夌敻鎮￠弴銏＄厪濠电姴绻掗悾閬嶆煃缂佹ɑ绀冮柕鍥у婵偓闁挎稑瀚崳浼存倵濞堝灝鏋熼柟姝屾珪閹便劑鍩€椤掑嫭鐓冮梺娆惧灠娴滈箖姊洪悷鏉挎Щ闁圭⒈鍋婇崺鐐哄箣閿旇棄鈧兘鎮瑰ú顏嗙窗闁轰礁鐏濋—鍐Χ閸℃鍋侀梺鍛婃处閸欏骸煤閹间焦鈷戦悷娆忓椤ュ顭胯椤ㄥ﹪骞冮敓鐘插嵆闁靛繆鈧枼鍋撻悽鍛婄叆婵犻潧妫濋妤€霉濠婂嫮绠橀柍褜鍓濋～澶娒洪弽顓熷剹闁稿瞼鍋涢拑?WARP 闂傚倸鍊搁崐鎼佸磹閹间礁纾归柟闂寸绾剧懓顪冪€ｎ亝鎹ｉ柣顓炴閵嗘帒顫濋敐鍛婵°倗濮烽崑鐐烘偋閻樻眹鈧線寮撮姀鈩冩珖闂侀€炲苯澧板瑙勬礃缁绘繂顫濋鐘插箞闂備焦鏋奸弲娑㈠疮娴兼潙鐓″鑸靛姈閻撴洟鎮橀悙棰濆殭濠殿喖鍊块弻鈥崇暆鐎ｎ剛袦闂佽桨鐒﹂崝娆忕暦閵娾晩鏁婇柤鎭掑劚椤忎即姊婚崒娆戭槮闁圭⒈鍋勮灋婵炲棙鍨甸崹婵嬫煙閹规劦鍤欓柦鍐枔閳ь剙绠嶉崕閬嶆偋濠婂喚鐎堕柕濞炬櫆閳锋垿鏌涘☉姗堟敾閻忓繒鏁婚弻娑㈡偐瀹曞洤鈪瑰銈嗘穿缁插墽鎹㈠┑鍡╂僵妞ゆ垟鏂侀弲鐘诲蓟閺囩喎绶為柛顐ｇ箘娴犳儳顪冮妶搴′簻缂佸鎸抽崺鐐哄箣閿曗偓閻擄繝鏌涢埄鍐炬畼濞寸姭鏅犲铏规嫚閼碱剛顔夌紓浣筋嚙閻楁捇濡存笟鈧顕€宕奸锝嗘珝闂備線娼х换鍡楊瀶瑜旈獮蹇撁洪鍛嫼闂佸憡绋戦敃锔剧磽濮樿埖鐓曢幖娣妺閹插墽鈧鍠楁繛濠囧蓟閸℃鍚嬮柛鈩冪懃楠炲牓姊虹拠鎻掑毐缂傚秴妫濆畷鎴﹀川椤撶姵娈鹃悗鍏夊亾闁告洦鍓涢崢鎼佹倵閸忓浜鹃柣搴秵閸撴稖鈪甸梻鍌欐祰椤曟牠宕伴幒妤€鐤炬繝濠傚枤閻掍粙鐓崶銊р槈闁绘挻绋戦湁闁挎繂鎳庨ˉ蹇涙煟鎼达紕鎽犵紒缁樼箞閹粙妫冨ù鍓佸仱閺屾盯濡搁妷顔惧悑濡ょ姷鍋涢敃顏堢嵁濡警鐔嗛柛鏇ㄥ墰閳藉鏌嶇拠鏌ュ弰妤犵偞锚閻ｇ兘宕堕埡鍌涚钒闂傚倸鍊风欢姘焽瑜戞晶婵嬫⒑閸濆嫭鍣虹紒瀣尵閸掓帗绻濆顓炰画闂佺懓鍟块敃銈囩礊婵犲洤鏋侀柟鐗堟緲瀹告繃銇勯弮鍥棄闁哄鍨垮缁樻媴妞嬪簼瑕嗙紓鍌氱М閸嬫挻淇婇悙瀛樼稇闁硅姤绮撻獮鎴﹀閻橆偅顫嶅┑鐐叉閸旀绮旈崼鏇熲拺闁告稑锕ョ€垫瑩鏌涢弴銊ヤ簵閺夆晜锚閳规垿鏁嶉崟顐℃澀闂佺臎閸涱厾绛忛梺鍏肩ゴ閺呮盯宕瑰┑鍥╃闁糕剝顨堢粻鎶芥煛?
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
