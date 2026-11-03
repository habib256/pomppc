// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "WinHost.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace fs = std::filesystem;

namespace winhost {

std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
    return out;
}

namespace {

std::string slashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

bool isAscii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x80; });
}

std::wstring envWide(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v ? std::wstring(v) : std::wstring();
}

}  // namespace

std::string exeDir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    const size_t slash = buf.find_last_of(L"\\/");
    if (slash != std::wstring::npos) buf.resize(slash);
    return slashes(narrow(buf));
}

std::string userHome() { return slashes(narrow(envWide(L"USERPROFILE"))); }

std::string dataRoot() {
    const std::wstring forced = envWide(L"POMPPC_DATA");
    if (!forced.empty()) return slashes(narrow(forced));
    return userHome() + "/POMPPC";
}

std::string downloadsDir() {
    // FOLDERID_Downloads : recopié ici, car KNOWNFOLDERID n'est défini que dans un .lib selon les
    // versions de mingw. [B] MSDN, « KNOWNFOLDERID ».
    static const GUID kDownloads = {0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(kDownloads, 0, nullptr, &path)) && path) out = slashes(narrow(path));
    if (path) CoTaskMemFree(path);
    return out.empty() ? userHome() + "/Downloads" : out;
}

std::string tempDir() {
    wchar_t buf[MAX_PATH + 2] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::string out = n ? narrow(std::wstring(buf, n)) : std::string("C:/Windows/Temp/");
    while (!out.empty() && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    return qemuPath(out);
}

std::string qemuPath(const std::string& path) {
    if (isAscii(path)) return path;
    // Le plus long parent qui existe prend sa forme courte ; le reste, s'il n'existe pas encore, doit
    // déjà être ASCII.
    fs::path p = fs::u8path(path);
    std::error_code ec;
    fs::path tail;
    while (!p.empty() && !fs::exists(p, ec)) {
        tail = p.filename() / tail;
        const fs::path up = p.parent_path();
        if (up == p) break;
        p = up;
    }
    wchar_t buf[32768];
    const DWORD n = GetShortPathNameW(p.wstring().c_str(), buf, 32768);
    if (n == 0 || n >= 32768) return {};
    std::string out = slashes(narrow(std::wstring(buf, n)));
    if (!tail.empty()) {
        std::string rest = slashes(narrow(tail.wstring()));
        while (!rest.empty() && rest.back() == '/') rest.pop_back();
        out += "/" + rest;
    }
    return isAscii(out) ? out : std::string();
}

LaunchHost launchHost(const std::vector<std::string>& overrides) {
    auto vars = std::make_shared<std::map<std::string, std::string>>();
    for (const std::string& kv : overrides) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) (*vars)[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    LaunchHost h;
    h.env = [vars](const std::string& name, std::string* value) {
        auto it = vars->find(name);
        if (it != vars->end()) {
            *value = it->second;
            return true;
        }
        const wchar_t* v = _wgetenv(wide(name).c_str());
        if (!v) return false;
        *value = narrow(v);
        return true;
    };
    h.fileExists = [](const std::string& path) {
        std::error_code ec;
        return fs::is_regular_file(fs::u8path(path), ec);
    };
    h.firstLine = [](const std::string& path) {
        std::ifstream in(fs::u8path(path));
        std::string text;
        std::getline(in, text);
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
        return text;
    };
    h.fileSize = [](const std::string& path) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(fs::u8path(path), ec);
        return ec ? 0ull : (unsigned long long)size;
    };
    // bind et pas connect : sous Windows, un connect refusé sur la boucle locale met environ deux
    // secondes à le dire (trois essais SYN), et il y a dix ports à essayer.
    // [F] Mesure de ces deux secondes non faite ici : ordre de grandeur connu de Winsock.
    h.portFree = [](int port) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return true;
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool free = bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
        closesocket(s);
        return free;
    };
    h.qemuPath = [](const std::string& path) { return qemuPath(path); };
    return h;
}

namespace {

// L'environnement du processus avec ces KEY=VAL par-dessus, trié comme CreateProcess le demande.
// [B] MSDN, « Changing Environment Variables » : bloc trié, sans égard à la casse.
std::wstring environmentBlock(const std::vector<std::string>& overrides) {
    const auto upper = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towupper(c); });
        return s;
    };
    std::map<std::wstring, std::wstring> vars;
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (const wchar_t* e = env; *e; e += wcslen(e) + 1) {
            const std::wstring kv(e);
            const size_t eq = kv.find(L'=', 1);   // =C:=C:\x : ces entrées commencent par =
            if (eq != std::wstring::npos) vars[upper(kv.substr(0, eq))] = kv;
        }
        FreeEnvironmentStringsW(env);
    }
    for (const std::string& kv : overrides) {
        const std::wstring w = wide(kv);
        const size_t eq = w.find(L'=');
        if (eq != std::wstring::npos) vars[upper(w.substr(0, eq))] = w;
    }
    std::wstring block;
    for (const auto& item : vars) {
        block += item.second;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

// Seuls ces handles passent à l'enfant : sans la liste, le socket QMP et les deux bus D-Bus, héritables
// par défaut, le suivraient, et leur fermeture par le frontal ne se verrait plus de l'autre côté.
struct InheritOnly {
    STARTUPINFOEXW si{};
    std::vector<char> attrs;
    std::vector<HANDLE> handles;
    bool init(std::vector<HANDLE> list) {
        handles = std::move(list);
        si.StartupInfo.cb = sizeof si;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrs.resize(size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) return false;
        return UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                         handles.size() * sizeof(HANDLE), nullptr, nullptr) != 0;
    }
    ~InheritOnly() {
        if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    }
};

HANDLE openNul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

}  // namespace

bool runTool(const std::vector<std::string>& args, std::string* out, const std::function<void(int)>& started) {
    if (args.empty()) return false;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (out) {
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE nulIn = openNul(GENERIC_READ);
    HANDLE nulOut = openNul(GENERIC_WRITE);
    InheritOnly inherit;
    const HANDLE stdOut = out ? writeEnd : nulOut;
    // Sans doublon : CreateProcess refuserait la liste.
    std::vector<HANDLE> passed = {nulIn, nulOut};
    if (out) passed.push_back(writeEnd);
    if (!inherit.init(std::move(passed))) {
        if (out) { CloseHandle(readEnd); CloseHandle(writeEnd); }
        CloseHandle(nulIn); CloseHandle(nulOut);
        return false;
    }
    inherit.si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    inherit.si.StartupInfo.hStdInput = nulIn;
    inherit.si.StartupInfo.hStdOutput = stdOut;
    inherit.si.StartupInfo.hStdError = nulOut;
    std::wstring cmd = wide(windowsCommandLine(args));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                   &inherit.si.StartupInfo, &pi);
    if (out) CloseHandle(writeEnd);
    CloseHandle(nulIn);
    CloseHandle(nulOut);
    if (!ok) {
        if (out) CloseHandle(readEnd);
        return false;
    }
    if (started) started((int)pi.dwProcessId);
    if (out) {
        out->clear();
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readEnd, buf, sizeof buf, &n, nullptr) && n > 0) out->append(buf, n);
        CloseHandle(readEnd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

void terminate(int pid) {
    if (pid <= 0) return;
    if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid)) {
        TerminateProcess(p, 1);
        CloseHandle(p);
    }
}

void openExternal(const std::string& target) {
    ShellExecuteW(nullptr, L"open", wide(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string chooseFile(const std::string& prompt) {
    wchar_t file[32768] = {};
    const std::wstring title = wide(prompt);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    // Appelé depuis le fil d'un travail de l'assistant : la boîte de dialogue veut un appartement COM.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::string chosen = GetOpenFileNameW(&ofn) ? slashes(narrow(file)) : std::string();
    if (SUCCEEDED(com)) CoUninitialize();
    return chosen;
}

bool spawnQemu(const std::string& exe, const std::vector<std::string>& args,
               const std::vector<std::string>& env, const std::string& logFile, Child* child, std::string* err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(wide(logFile).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) log = openNul(GENERIC_WRITE);
    HANDLE nulIn = openNul(GENERIC_READ);
    InheritOnly inherit;
    if (!inherit.init({nulIn, log})) {
        CloseHandle(log);
        CloseHandle(nulIn);
        if (err) *err = "POMPPC couldn't prepare QEMU's log.";
        return false;
    }
    inherit.si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    inherit.si.StartupInfo.hStdInput = nulIn;
    inherit.si.StartupInfo.hStdOutput = log;
    inherit.si.StartupInfo.hStdError = log;

    // Dans un Job avant de démarrer : même un QEMU tué net par le frontal mort ne reste pas derrière,
    // écrivant un disque dont le verrou vient de tomber.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    std::vector<std::string> argv = {exe};
    argv.insert(argv.end(), args.begin(), args.end());
    std::wstring cmd = wide(windowsCommandLine(argv));
    const std::wstring app = wide(exe);
    std::wstring block = environmentBlock(env);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(
        app.c_str(), cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, block.data(),
        nullptr, &inherit.si.StartupInfo, &pi);
    const DWORD error = GetLastError();
    CloseHandle(log);
    CloseHandle(nulIn);
    if (!started) {
        if (job) CloseHandle(job);
        if (err) *err = "POMPPC couldn't start QEMU (error " + std::to_string((unsigned long)error) + "): " + exe;
        return false;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        // Sans Job, on continue : ce processus resterait orphelin si le frontal plante, mais il tourne.
        std::fprintf(stderr, "QemuBridge: AssignProcessToJobObject error %lu\n", GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    child->process = pi.hProcess;
    child->job = job;
    child->pid = pi.dwProcessId;
    return true;
}

void* lockInstance(const std::string& path) {
    // Pas de bInheritHandle : QEMU ne doit pas prolonger le verrou. Pas de FILE_FLAG_DELETE_ON_CLOSE :
    // le fichier reste, c'est la règle du dépôt.
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

void unlockInstance(void* handle) {
    if (handle) CloseHandle(handle);
}

void startup(const std::string& logFile) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // Lancé depuis l'Explorateur, un exe de sous-système GUI n'a pas de stderr. Depuis un terminal ou
    // sous la CI, il en a un, et il reste là.
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
        std::error_code ec;
        fs::create_directories(fs::u8path(logFile).parent_path(), ec);
        if (FILE* f = _wfreopen(wide(logFile).c_str(), L"a", stderr)) setvbuf(f, nullptr, _IOLBF, 4096);
    }
}

}  // namespace winhost

#endif  // _WIN32
