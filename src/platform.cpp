// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

#include "platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace modelhub {
fs::path from_utf8(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}

namespace {
// Length of the valid UTF-8 sequence at the start of `text`, or 0.
std::size_t utf8_length(std::string_view text) {
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(text[index]); };
    const auto lead = byte(0);
    if (lead < 0x80) return 1;
    const std::size_t length = lead >= 0xF0 && lead <= 0xF4 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 ? 2 : 0;
    if (!length || lead > 0xF4 || text.size() < length) return 0;
    for (std::size_t index = 1; index < length; ++index)
        if ((byte(index) & 0xC0) != 0x80) return 0;
    if (lead == 0xE0 && byte(1) < 0xA0) return 0; // overlong
    if (lead == 0xED && byte(1) > 0x9F) return 0; // surrogates
    if (lead == 0xF0 && byte(1) < 0x90) return 0; // overlong
    if (lead == 0xF4 && byte(1) > 0x8F) return 0; // beyond U+10FFFF
    return length;
}

bool valid_utf8(std::string_view text) {
    while (!text.empty()) {
        const auto length = utf8_length(text);
        if (!length) return false;
        text.remove_prefix(length);
    }
    return true;
}

std::string replace_invalid(std::string_view text) {
    std::string result;
    while (!text.empty()) {
        const auto length = utf8_length(text);
        result.append(length ? text.substr(0, length) : std::string_view("?"));
        text.remove_prefix(length ? length : 1);
    }
    return result;
}
} // namespace

std::string system_text(std::string_view text) {
    if (valid_utf8(text)) return std::string(text);
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide > 0) {
        std::wstring buffer(static_cast<std::size_t>(wide), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), buffer.data(), wide);
        return replace_invalid(cymcp::path_utf8(fs::path(buffer)));
    }
#endif
    return replace_invalid(text);
}

// File names that are not valid Unicode must not break a JSON reply.
std::string to_utf8(const fs::path& path) { return replace_invalid(cymcp::path_utf8(path)); }

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return value;
}

std::string environment(const char* name) {
#ifdef _WIN32
    const std::wstring wide(name, name + std::strlen(name));
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(wide.c_str(), value.data(), size));
    return to_utf8(fs::path(value));
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

void set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(std::wstring(name, name + std::strlen(name)).c_str(), from_utf8(value).c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path home_path() {
#ifdef _WIN32
    if (auto profile = environment("USERPROFILE"); !profile.empty()) return from_utf8(profile);
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &raw))) result = raw;
    CoTaskMemFree(raw);
    return result;
#else
    return from_utf8(environment("HOME"));
#endif
}

fs::path data_path() {
    if (auto custom = environment("CYMODELHUB_DATA"); !custom.empty())
        return fs::absolute(from_utf8(custom)).lexically_normal();
#ifdef _WIN32
    if (auto local = environment("LOCALAPPDATA"); !local.empty()) return from_utf8(local) / "CyModelHub";
    return home_path() / "AppData" / "Local" / "CyModelHub";
#elif defined(__APPLE__)
    return home_path() / "Library" / "Application Support" / "CyModelHub";
#else
    if (auto xdg = environment("XDG_DATA_HOME"); !xdg.empty()) return from_utf8(xdg) / "cymodelhub";
    return home_path() / ".local" / "share" / "cymodelhub";
#endif
}

fs::path descriptor_path() { return data_path() / "service.json"; }

fs::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return buffer;
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code error;
    auto resolved = fs::canonical(buffer.c_str(), error);
    return error ? fs::path(buffer.c_str()) : resolved;
#else
    std::error_code error;
    return fs::read_symlink("/proc/self/exe", error);
#endif
}

unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t file_time_seconds(const fs::file_time_type& time) {
    // Portable conversion: std::chrono::clock_cast is not available for file_clock everywhere.
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

std::optional<std::string> read_file(const fs::path& path, std::size_t max_bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string content;
    char buffer[65536];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        content.append(buffer, static_cast<std::size_t>(file.gcount()));
        if (content.size() > max_bytes) return std::nullopt;
    }
    return content;
}

void write_file_atomic(const fs::path& path, std::string_view content, bool owner_only) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        cymcp::PrivateOutputFile file(nullptr, &std::fclose);
        if (owner_only) {
            file = cymcp::open_private_output_file(temporary);
        } else {
#ifdef _WIN32
            std::FILE* raw = nullptr;
            if (_wfopen_s(&raw, temporary.c_str(), L"wb") == 0) file.reset(raw);
#else
            file.reset(std::fopen(temporary.c_str(), "wb"));
#endif
        }
        if (!file || std::fwrite(content.data(), 1, content.size(), file.get()) != content.size() ||
            std::fflush(file.get()) != 0)
            throw cymcp::Error("WriteFailed", "Cannot write " + to_utf8(path.filename()));
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw cymcp::Error("WriteFailed", "Cannot replace " + to_utf8(path.filename()));
#else
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) throw cymcp::Error("WriteFailed", "Cannot replace " + to_utf8(path.filename()));
#endif
}

bool loopback_port_taken(int port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    bool taken = false;
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe != INVALID_SOCKET) {
        // An exclusive bind fails as soon as any listener holds the port.
        BOOL exclusive = TRUE;
        setsockopt(probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        taken = bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0;
        closesocket(probe);
    }
    WSACleanup();
    return taken;
#else
    // Elsewhere a second bind on a listening port simply fails, which the caller handles.
    (void)port;
    return false;
#endif
}

#ifdef _WIN32
namespace {
// Quote one argument the way CommandLineToArgvW reads it back.
std::wstring quoted(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
        }
        if (c != L'\\') result += c;
    }
    result.append(backslashes * 2, L'\\');
    return result + L'"';
}
} // namespace
#endif

bool spawn_detached(const fs::path& executable, const std::vector<std::string>& arguments) {
#ifdef _WIN32
    std::wstring command = quoted(executable.wstring());
    for (const auto& argument : arguments) command += L' ' + quoted(from_utf8(argument).wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr, nullptr,
                        &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        // Double fork: the service is adopted by init and never becomes a zombie of the caller.
        setsid();
        if (fork() != 0) _exit(0);
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        std::vector<std::string> storage{executable.string()};
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return true;
#endif
}

FileLock::FileLock(const fs::path& path) {
    if (path.has_parent_path()) {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
    }
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle != INVALID_HANDLE_VALUE) handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
    const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (file >= 0 && flock(file, LOCK_EX | LOCK_NB) == 0) handle_ = file;
    else if (file >= 0) close(file);
#endif
}

FileLock::~FileLock() {
    if (handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(static_cast<int>(handle_));
#endif
}
} // namespace modelhub
