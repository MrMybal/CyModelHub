// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

#pragma once

// Shared by the client library and the service. This file and platform.cpp depend on the C++
// standard library, the operating system and nlohmann/json only, so that an application can
// link the client without any other project.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace modelhub {
namespace fs = std::filesystem;
using Json = nlohmann::json;

inline constexpr int default_service_port = 48787;

// A failure with a stable identifier: WriteFailed, NetworkError, Timeout, ResponseTooLarge.
class Failure : public std::runtime_error {
  public:
    std::string code;
    // For a failed exchange: whether the request had started to leave. When it had not, the
    // server never saw it and sending it again cannot repeat anything.
    bool request_sent = false;
    Failure(std::string failure_code, const std::string& message, bool sent = false)
        : std::runtime_error(message), code(std::move(failure_code)), request_sent(sent) {}
};

fs::path from_utf8(std::string_view value);
std::string to_utf8(const fs::path& path);
std::string lower(std::string value); // ASCII only
// Text coming from the operating system (error messages, exception texts) as valid UTF-8.
// On Windows such text is in the system code page; elsewhere invalid bytes are replaced.
std::string system_text(std::string_view text);
std::string environment(const char* name);
void set_environment(const char* name, const std::string& value);
fs::path home_path();
// Per-user data directory. CYMODELHUB_DATA overrides it (tests, portable installs).
fs::path data_path();
fs::path descriptor_path(); // <data>/service.json: how a local application reaches the service
fs::path executable_path();
unsigned long process_id();
std::int64_t now_seconds();
std::int64_t file_time_seconds(const fs::file_time_type& time);

// Whole file, or nullopt when it is missing, unreadable or larger than the limit.
std::optional<std::string> read_file(const fs::path& path, std::size_t max_bytes = 64 * 1024 * 1024);
// Write to a temporary sibling, then replace the destination in one step. With `owner_only`
// the file is created readable and writable by the current user alone.
void write_file_atomic(const fs::path& path, std::string_view content, bool owner_only = false);

struct LocalRequest {
    int port = 0;
    std::string method = "GET", path = "/";
    std::string body;   // sent as application/json when the method is not GET
    std::string bearer; // access token, empty for none
    std::chrono::milliseconds timeout{60000};
    std::size_t max_response_bytes = std::size_t{64} * 1024 * 1024;
};
struct LocalResponse {
    int status = 0;
    std::string body;
};
// One HTTP/1.1 exchange with a server of this computer, on 127.0.0.1 only: the request and
// its token never leave the machine. Throws Failure (NetworkError, Timeout, ResponseTooLarge).
LocalResponse local_http(const LocalRequest& request);
// True when another program already listens on this loopback TCP port. Windows lets two
// programs share a listening port, so a server must check before it claims a fixed one.
bool loopback_port_taken(int port);
// Start a program that outlives the caller, without a console window.
bool spawn_detached(const fs::path& executable, const std::vector<std::string>& arguments);

// Exclusive, process-lifetime lock used to keep a single service per data directory.
class FileLock {
    std::intptr_t handle_ = -1;

  public:
    explicit FileLock(const fs::path& path);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool held() const { return handle_ != -1; }
};
} // namespace modelhub
