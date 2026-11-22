// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

#include "platform.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
// The order matters: winsock2.h, then windows.h, then the headers that need both.
#include <winsock2.h>

#include <windows.h>

#include <sddl.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
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

std::string narrow(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
} // namespace

std::string system_text(std::string_view text) {
    if (valid_utf8(text)) return std::string(text);
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide > 0) {
        std::wstring buffer(static_cast<std::size_t>(wide), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), buffer.data(), wide);
        return replace_invalid(narrow(fs::path(buffer)));
    }
#endif
    return replace_invalid(text);
}

// File names that are not valid Unicode must not break a JSON reply.
std::string to_utf8(const fs::path& path) { return replace_invalid(narrow(path)); }

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

namespace {
#ifdef _WIN32
// Security descriptor that gives full access to the current user and to nobody else. The
// caller releases it with LocalFree. Null when it cannot be built.
PSECURITY_DESCRIPTOR owner_only_descriptor() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> storage(size);
    const bool known = size && GetTokenInformation(token, TokenUser, storage.data(), size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!known || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid))
        return nullptr;
    const std::wstring rule = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(rule.c_str(), SDDL_REVISION_1, &descriptor,
                                                              nullptr))
        return nullptr;
    return descriptor;
}
#endif

// Create `path` anew and fill it. Access rights are set at creation, so that an owner-only
// file is never readable by someone else, even for an instant.
bool write_new_file(const fs::path& path, std::string_view content, bool owner_only) {
#ifdef _WIN32
    DeleteFileW(path.c_str());
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    if (owner_only && !(attributes.lpSecurityDescriptor = owner_only_descriptor())) return false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, owner_only ? &attributes : nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (attributes.lpSecurityDescriptor) LocalFree(attributes.lpSecurityDescriptor);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool written = true;
    while (written && !content.empty()) {
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(content.size(), std::size_t{1} << 20));
        written = WriteFile(file, content.data(), chunk, &count, nullptr) && count > 0;
        content.remove_prefix(count);
    }
    return CloseHandle(file) && written;
#else
    ::unlink(path.c_str());
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, owner_only ? 0600 : 0666);
    if (file < 0) return false;
    bool written = true;
    while (written && !content.empty()) {
        const auto count = ::write(file, content.data(), content.size());
        if (count < 0 && errno == EINTR) continue;
        written = count > 0;
        if (written) content.remove_prefix(static_cast<std::size_t>(count));
    }
    return ::close(file) == 0 && written;
#endif
}
} // namespace

void write_file_atomic(const fs::path& path, std::string_view content, bool owner_only) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    if (!write_new_file(temporary, content, owner_only))
        throw Failure("WriteFailed", "Cannot write " + to_utf8(path.filename()));
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Failure("WriteFailed", "Cannot replace " + to_utf8(path.filename()));
#else
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) throw Failure("WriteFailed", "Cannot replace " + to_utf8(path.filename()));
#endif
}

namespace {
#ifdef _WIN32
using Socket = SOCKET;
using Length = int;
constexpr Socket no_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
// True when the last socket call only asked to wait.
bool pending() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
// Windows Sockets must be started once per process.
bool sockets_ready() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
#else
using Socket = int;
using Length = std::size_t;
constexpr Socket no_socket = -1;
void close_socket(Socket socket) { ::close(socket); }
bool pending() { return errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool sockets_ready() { return true; }
#endif

using Deadline = std::chrono::steady_clock::time_point;

// Wait until the socket can be written to or read from. False when the deadline passes first.
bool wait_socket(Socket socket, bool writing, Deadline deadline) {
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count();
        if (left <= 0) return false;
#ifdef _WIN32
        // A refused connection is reported in the third set.
        fd_set ready, failed;
        FD_ZERO(&ready);
        FD_ZERO(&failed);
        FD_SET(socket, &ready);
        FD_SET(socket, &failed);
        timeval wait{static_cast<long>(left / 1000), static_cast<long>(left % 1000 * 1000)};
        return select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &failed, &wait) > 0;
#else
        pollfd entry{socket, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int result = poll(&entry, 1, static_cast<int>(std::min<long long>(left, INT_MAX)));
        if (result < 0 && errno == EINTR) continue;
        return result > 0;
#endif
    }
}

// Body of a chunked answer, or nullopt while its last chunk is missing.
std::optional<std::string> unchunked(std::string_view data) {
    std::string body;
    for (;;) {
        const auto line = data.find("\r\n");
        if (line == std::string_view::npos) return std::nullopt;
        std::size_t size = 0;
        const auto parsed = std::from_chars(data.data(), data.data() + line, size, 16);
        if (parsed.ec != std::errc() || parsed.ptr == data.data()) return std::nullopt;
        data.remove_prefix(line + 2);
        if (size == 0) return body; // trailers, if any, are ignored
        if (data.size() < size || data.size() - size < 2) return std::nullopt;
        body.append(data.substr(0, size));
        data.remove_prefix(size + 2);
    }
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
} // namespace

bool loopback_port_taken(int port) {
#ifdef _WIN32
    if (!sockets_ready()) return false;
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
    return taken;
#else
    // Elsewhere a second bind on a listening port simply fails, which the caller handles.
    (void)port;
    return false;
#endif
}

LocalResponse local_http(const LocalRequest& request) {
    const auto unsafe = [](const std::string& text) {
        return text.find_first_of("\r\n") != std::string::npos;
    };
    if (request.port <= 0 || request.port > 65535 || request.path.empty() || request.path.front() != '/' ||
        unsafe(request.method) || unsafe(request.path) || unsafe(request.bearer))
        throw Failure("NetworkError", "Invalid local request");
    if (!sockets_ready()) throw Failure("NetworkError", "Networking is not available");

    struct Connection {
        Socket socket = no_socket;
        ~Connection() {
            if (socket != no_socket) close_socket(socket);
        }
    } connection;
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    const Socket socket = connection.socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == no_socket) throw Failure("NetworkError", "Cannot open a local connection");
    // Non-blocking, so that every step obeys the deadline.
#ifdef _WIN32
    u_long enabled = 1;
    ioctlsocket(socket, FIONBIO, &enabled);
#else
    fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
    fcntl(socket, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int quiet = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &quiet, sizeof(quiet));
#endif
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(request.port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (!pending()) throw Failure("NetworkError", "The local service refused the connection");
        if (!wait_socket(socket, true, deadline))
            throw Failure("Timeout", "The local service did not answer in time");
        int error = 0;
#ifdef _WIN32
        int size = sizeof(error);
#else
        socklen_t size = sizeof(error);
#endif
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0 ||
            error != 0)
            throw Failure("NetworkError", "The local service refused the connection");
    }

    std::string message = request.method + ' ' + request.path +
                          " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(request.port) +
                          "\r\nConnection: close\r\nAccept: application/json\r\n";
    if (!request.bearer.empty()) message += "Authorization: Bearer " + request.bearer + "\r\n";
    if (request.method != "GET")
        message +=
            "Content-Type: application/json\r\nContent-Length: " + std::to_string(request.body.size()) +
            "\r\n";
    message += "\r\n";
    if (request.method != "GET") message += request.body;
#ifdef MSG_NOSIGNAL
    const int send_flags = MSG_NOSIGNAL;
#else
    const int send_flags = 0;
#endif
    for (std::size_t sent = 0; sent < message.size();) {
        const auto chunk = std::min<std::size_t>(message.size() - sent, std::size_t{1} << 20);
        const auto count = ::send(socket, message.data() + sent, static_cast<Length>(chunk), send_flags);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (!pending()) {
            throw Failure("NetworkError", "The local service closed the connection", true);
        } else if (!wait_socket(socket, true, deadline)) {
            throw Failure("Timeout", "The local service did not answer in time", true);
        }
    }

    // Read until the announced length, the last chunk, or the end of the connection.
    LocalResponse response;
    std::string data;
    std::size_t body_start = std::string::npos, length = std::string::npos;
    bool chunked = false, decoded = false;
    char buffer[16384];
    for (bool complete = false; !complete;) {
        const auto count = ::recv(socket, buffer, static_cast<Length>(sizeof(buffer)), 0);
        if (count == 0) break;
        if (count < 0) {
            if (!pending()) throw Failure("NetworkError", "The local service closed the connection", true);
            if (!wait_socket(socket, false, deadline))
                throw Failure("Timeout", "The local service did not answer in time", true);
            continue;
        }
        const auto seen = data.size();
        data.append(buffer, static_cast<std::size_t>(count));
        if (data.size() - 65536 > request.max_response_bytes && data.size() > 65536)
            throw Failure("ResponseTooLarge", "The answer of the local service is too large", true);
        if (body_start == std::string::npos) {
            const auto end = data.find("\r\n\r\n", seen < 3 ? 0 : seen - 3);
            if (end == std::string::npos) continue;
            body_start = end + 4;
            std::string_view head(data.data(), end);
            // "HTTP/1.1 200 OK", then one "Name: value" per line.
            const auto space = head.find(' ');
            if (head.substr(0, 5) != "HTTP/" || space == std::string_view::npos ||
                std::from_chars(head.data() + space + 1, head.data() + head.size(), response.status).ec !=
                    std::errc())
                throw Failure("NetworkError", "Unexpected answer from the local service", true);
            for (auto line = head.find("\r\n"); line != std::string_view::npos;) {
                head.remove_prefix(line + 2);
                line = head.find("\r\n");
                const auto field = head.substr(0, line);
                const auto colon = field.find(':');
                if (colon == std::string_view::npos) continue;
                const auto name = lower(std::string(trimmed(field.substr(0, colon))));
                const auto value = trimmed(field.substr(colon + 1));
                if (name == "content-length")
                    std::from_chars(value.data(), value.data() + value.size(), length);
                else if (name == "transfer-encoding")
                    chunked = lower(std::string(value)).find("chunked") != std::string::npos;
            }
        }
        if (chunked) {
            // The last chunk ends with an empty line: only then is decoding worth a try.
            if (std::string_view(data).substr(data.size() - 4) != "\r\n\r\n") continue;
            if (auto body = unchunked(std::string_view(data).substr(body_start))) {
                response.body = std::move(*body);
                complete = decoded = true;
            }
        } else if (length != std::string::npos) {
            complete = data.size() - body_start >= length;
        }
    }
    if (body_start == std::string::npos)
        throw Failure("NetworkError", "The local service closed the connection", true);
    if (chunked && !decoded) {
        auto body = unchunked(std::string_view(data).substr(body_start));
        if (!body) throw Failure("NetworkError", "The answer of the local service is incomplete", true);
        response.body = std::move(*body);
    } else if (!chunked) {
        if (length != std::string::npos && data.size() - body_start < length)
            throw Failure("NetworkError", "The answer of the local service is incomplete", true);
        response.body = data.substr(body_start, length);
    }
    if (response.body.size() > request.max_response_bytes)
        throw Failure("ResponseTooLarge", "The answer of the local service is too large", true);
    return response;
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
