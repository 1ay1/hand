// hand::proc — Windows.

#include <hand/core/proc.hpp>

#if defined(_WIN32)

#include <windows.h>
#include <objbase.h>     // CoInitializeEx (WIN32_LEAN_AND_MEAN leaves it out)
#include <shellapi.h>    // ShellExecuteW

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace hand::proc {

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/// One argument as CommandLineToArgvW will read it back (MSDN's rules:
/// backslashes are literal unless they precede a quote).
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++bs; ++i; }
        if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; }
        else { q.append(bs, L'\\'); q += a[i]; }
    }
    return q + L"\"";
}

void drain(HANDLE h, std::string* out) {
    char buf[8192];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out->append(buf, got);
}

std::string exe_name_of(DWORD pid) {
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string out;
    if (::QueryFullProcessImageNameW(h, 0, buf, &n)) {
        std::wstring p{buf, n};
        p = p.substr(p.find_last_of(L"\\/") + 1);
        if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0) p.resize(p.size() - 4);
        out = narrow(p);
    }
    ::CloseHandle(h);
    return out;
}

}  // namespace

Result run(const std::vector<std::string>& argv, std::string_view in, int timeout_ms) {
    Result r;
    if (argv.empty()) return r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!::CreatePipe(&in_r, &in_w, &sa, 0) || !::CreatePipe(&out_r, &out_w, &sa, 0) || !::CreatePipe(&err_r, &err_w, &sa, 0)) return r;
    ::SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote(widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(in_r); ::CloseHandle(out_w); ::CloseHandle(err_w);
    if (!ok) { ::CloseHandle(in_w); ::CloseHandle(out_r); ::CloseHandle(err_r); return r; }
    r.started = true;
    // both pipes drained at once, or a chatty stderr would wedge the child
    std::thread to([&] { drain(out_r, &r.out); });
    std::thread te([&] { drain(err_r, &r.err); });
    if (!in.empty()) { DWORD put = 0; ::WriteFile(in_w, in.data(), static_cast<DWORD>(in.size()), &put, nullptr); }
    ::CloseHandle(in_w);
    if (::WaitForSingleObject(pi.hProcess, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms)) == WAIT_TIMEOUT) {
        r.timed_out = true;
        ::TerminateProcess(pi.hProcess, 124);
        ::WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.code = static_cast<int>(code);
    to.join(); te.join();
    ::CloseHandle(out_r); ::CloseHandle(err_r);
    ::CloseHandle(pi.hThread); ::CloseHandle(pi.hProcess);
    return r;
}

std::vector<Listener> tcp_listeners(bool* ok) {
    // GetExtendedTcpTable, loaded at runtime (iphlpapi is not otherwise
    // linked). The tables are declared here rather than pulling in
    // iphlpapi.h, whose include order fights windows.h.
    struct Row4 { DWORD state, laddr, lport, raddr, rport, pid; };
    struct Row6 { UCHAR laddr[16]; DWORD lscope, lport; UCHAR raddr[16]; DWORD rscope, rport, state, pid; };
    using Fn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, int, ULONG);
    static const Fn get = [] {
        const HMODULE m = ::LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(m, "GetExtendedTcpTable"))) : nullptr;
    }();
    std::vector<Listener> out;
    if (ok) *ok = get != nullptr;
    if (!get) return out;
    constexpr int kListener = 3;   // TCP_TABLE_OWNER_PID_LISTENER
    std::map<DWORD, std::string> names;
    const auto name = [&](DWORD pid) -> const std::string& {
        auto it = names.find(pid);
        if (it == names.end()) it = names.emplace(pid, exe_name_of(pid)).first;
        return it->second;
    };
    for (const ULONG af : {ULONG{2} /*AF_INET*/, ULONG{23} /*AF_INET6*/}) {
        DWORD size = 0;
        (void)get(nullptr, &size, FALSE, af, kListener, 0);
        std::vector<unsigned char> buf(size + 1024);
        size = static_cast<DWORD>(buf.size());
        if (get(buf.data(), &size, FALSE, af, kListener, 0) != NO_ERROR) continue;
        const DWORD n = *reinterpret_cast<const DWORD*>(buf.data());
        for (DWORD i = 0; i < n; ++i) {
            DWORD lport = 0, pid = 0;
            if (af == 2) {
                const auto* row = reinterpret_cast<const Row4*>(buf.data() + sizeof(DWORD)) + i;
                lport = row->lport; pid = row->pid;
            } else {
                const auto* row = reinterpret_cast<const Row6*>(buf.data() + sizeof(DWORD)) + i;
                lport = row->lport; pid = row->pid;
            }
            const int port = static_cast<int>(((lport & 0xFF) << 8) | ((lport >> 8) & 0xFF));
            if (std::any_of(out.begin(), out.end(), [&](const Listener& l) { return l.port == port && l.pid == static_cast<int>(pid); })) continue;
            out.push_back({port, static_cast<int>(pid), name(pid)});
        }
    }
    return out;
}

std::vector<RawSession> putty_sessions(const std::string& root) {
    std::vector<RawSession> out;
    const std::wstring key = widen(root.empty() ? std::string{"Software\\SimonTatham\\PuTTY\\Sessions"} : root);
    HKEY sessions = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &sessions) != ERROR_SUCCESS) return out;
    wchar_t sub[512];
    for (DWORD i = 0;; ++i) {
        DWORD n = 512;
        if (::RegEnumKeyExW(sessions, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY s = nullptr;
        if (::RegOpenKeyExW(sessions, sub, 0, KEY_READ, &s) != ERROR_SUCCESS) continue;
        RawSession rs;
        // PuTTY URL-encodes session names ("My%20server")
        const std::string enc = narrow(sub);
        for (std::size_t k = 0; k < enc.size(); ++k) {
            if (enc[k] == '%' && k + 2 < enc.size()) { rs.name += static_cast<char>(std::strtol(enc.substr(k + 1, 2).c_str(), nullptr, 16)); k += 2; }
            else rs.name += enc[k];
        }
        wchar_t vname[256];
        BYTE data[4096];
        for (DWORD j = 0;; ++j) {
            DWORD vn = 256, dn = sizeof data, type = 0;
            if (::RegEnumValueW(s, j, vname, &vn, nullptr, &type, data, &dn) != ERROR_SUCCESS) break;
            if (type == REG_SZ || type == REG_EXPAND_SZ) {
                std::wstring v{reinterpret_cast<const wchar_t*>(data), dn / sizeof(wchar_t)};
                while (!v.empty() && v.back() == L'\0') v.pop_back();
                rs.values[narrow(vname)] = narrow(v);
            } else if (type == REG_DWORD && dn == 4) {
                rs.values[narrow(vname)] = std::to_string(*reinterpret_cast<const DWORD*>(data));
            }
        }
        ::RegCloseKey(s);
        out.push_back(std::move(rs));
    }
    ::RegCloseKey(sessions);
    return out;
}

std::vector<std::string> pick_files(const std::string& title) {
    // comdlg32 at runtime: only a click on "Upload" ever needs it
    struct OFN {
        DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCWSTR lpstrFilter; LPWSTR lpstrCustomFilter;
        DWORD nMaxCustFilter, nFilterIndex; LPWSTR lpstrFile; DWORD nMaxFile; LPWSTR lpstrFileTitle; DWORD nMaxFileTitle;
        LPCWSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension; LPCWSTR lpstrDefExt;
        LPARAM lCustData; void* lpfnHook; LPCWSTR lpTemplateName; void* pvReserved; DWORD dwReserved, FlagsEx;
    };
    using Fn = BOOL(WINAPI*)(OFN*);
    static const Fn open = [] {
        const HMODULE m = ::LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(m, "GetOpenFileNameW"))) : nullptr;
    }();
    std::vector<std::string> out;
    if (!open) return out;
    (void)::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<wchar_t> buf(32768, L'\0');
    const std::wstring t = widen(title);
    OFN o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = ::GetForegroundWindow();
    o.lpstrFilter = L"All files\0*.*\0";
    o.lpstrFile = buf.data();
    o.nMaxFile = static_cast<DWORD>(buf.size());
    o.lpstrTitle = t.c_str();
    o.Flags = 0x00000200 /*ALLOWMULTISELECT*/ | 0x00080000 /*EXPLORER*/ | 0x00001000 /*FILEMUSTEXIST*/ | 0x00000008 /*NOCHANGEDIR*/;
    if (open(&o)) {
        // one path, or a directory followed by names, NUL-separated
        std::vector<std::wstring> parts;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) parts.emplace_back(p);
        if (parts.size() == 1) out.push_back(narrow(parts[0]));
        else for (std::size_t i = 1; i < parts.size(); ++i) out.push_back(narrow(parts[0] + L"\\" + parts[i]));
    }
    ::CoUninitialize();
    return out;
}

std::string downloads_dir() {
    if (const char* u = std::getenv("USERPROFILE"); u && *u) return std::string{u} + "\\Downloads";
    return std::filesystem::temp_directory_path().string();
}

void reveal(const std::string& path) {
    const std::wstring args = L"/select,\"" + widen(path) + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

std::string temp_path(std::string_view stem) {
    static unsigned n = 0;
    return (std::filesystem::temp_directory_path() /
            (std::string{stem} + "-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++n) + ".txt")).string();
}

}  // namespace hand::proc

#endif  // _WIN32
