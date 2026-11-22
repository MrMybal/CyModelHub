// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "files.hpp"
#include "formats.hpp"

#include <cymcp/http.hpp>
#include <fstream>
#include <limits>

namespace modelhub {
namespace {
constexpr int max_redirects = 6;

bool hex64(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string unquoted(std::string value) {
    if (value.rfind("W/", 0) == 0) value.erase(0, 2);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    return value;
}

std::string percent_decode(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            result += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            result += text[i];
        }
    }
    return result;
}

std::string percent_encode_path(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += digits[c >> 4];
            result += digits[c & 15];
        }
    }
    return result;
}

// A safe file name: the last URL segment, without query, folders or reserved characters.
std::string file_name(const Download& request) {
    auto name = request.filename;
    if (name.empty()) {
        auto path = request.url.substr(0, request.url.find_first_of("?#"));
        name = percent_decode(path.substr(path.find_last_of('/') + 1));
    }
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
        name.size() > 200)
        throw cymcp::Error("InvalidName", "Give a plain file name for the downloaded model");
    if (!model_extension(from_utf8(name)))
        throw cymcp::Error("InvalidName", "The file name must end with a model extension such as .gguf or "
                                          ".safetensors");
    return name;
}

std::string origin(const cymcp::Url& url) {
    return url.scheme + "://" + url.host + ":" + std::to_string(url.port);
}

struct Resolved {
    std::string url;       // after redirects
    std::string sha256;    // announced by the server, when it does
    std::uint64_t total = 0;
    bool ranges = false;
};

// Follow redirects with a one-byte range request: it reveals the final address, the size and
// whether the server can resume, without downloading anything.
Resolved resolve(const Download& request, const Job& job) {
    const cymcp::HttpClient client;
    Resolved resolved;
    resolved.url = request.url;
    const auto first_origin = origin(cymcp::parse_url(request.url));
    for (int hop = 0; hop <= max_redirects; ++hop) {
        cymcp::HttpRequest probe;
        probe.url = resolved.url;
        probe.headers["Range"] = "bytes=0-0";
        // The credential is only ever sent to the site it was given for.
        if (!request.token.empty() && origin(cymcp::parse_url(resolved.url)) == first_origin)
            probe.headers["Authorization"] = "Bearer " + request.token;
        probe.timeout = std::chrono::seconds(60);
        probe.error_body_limit = 64 * 1024;
        bool whole_file = false;
        cymcp::HttpResponse response;
        try {
            response = client.perform(probe, job.cancellation(), [&](const char*, std::size_t size) {
                // A server that ignores Range starts sending everything: stop it there.
                whole_file = whole_file || size > 1;
                return !whole_file;
            });
        } catch (const cymcp::Error&) {
            if (!whole_file) throw;
            return resolved;
        }
        if (const auto etag = response.headers.find("x-linked-etag"); etag != response.headers.end())
            if (const auto value = lower(unquoted(etag->second)); hex64(value)) resolved.sha256 = value;
        if (response.status >= 300 && response.status < 400) {
            const auto location = response.headers.find("location");
            if (location == response.headers.end() || location->second.empty())
                throw cymcp::Error("DownloadFailed", "Redirect without a destination");
            resolved.url = location->second.rfind("/", 0) == 0
                               ? origin(cymcp::parse_url(resolved.url)) + location->second
                               : location->second;
            continue;
        }
        if (response.status == 206) {
            resolved.ranges = true;
            if (const auto range = response.headers.find("content-range"); range != response.headers.end())
                if (const auto slash = range->second.find('/'); slash != std::string::npos)
                    try {
                        resolved.total = std::stoull(range->second.substr(slash + 1));
                    } catch (const std::exception&) {
                    }
            return resolved;
        }
        if (response.status == 200) return resolved;
        if (response.status == 401 || response.status == 403)
            throw cymcp::Error("AccessDenied", "The server refused access (HTTP " + std::to_string(response.status) +
                                                   "). A gated model needs an access token");
        throw cymcp::Error("DownloadFailed", "The server answered HTTP " + std::to_string(response.status));
    }
    throw cymcp::Error("DownloadFailed", "Too many redirects");
}
} // namespace

std::string huggingface_url(const std::string& repository, const std::string& file, const std::string& revision) {
    return "https://huggingface.co/" + percent_encode_path(repository) + "/resolve/" +
           percent_encode_path(revision.empty() ? "main" : revision) + "/" + percent_encode_path(file);
}

Downloaded download_file(const Download& request, const Job& job) {
    const auto name = file_name(request);
    fs::create_directories(request.directory);
    const auto target = request.directory / from_utf8(name);
    auto partial = target;
    partial += ".part";
    std::error_code error;
    if (fs::exists(target, error)) throw cymcp::Error("AlreadyExists", name + " is already in this folder");

    job.update(0, 0, name);
    const auto resolved = resolve(request, job);
    auto expected = lower(request.sha256);
    if (expected.empty()) expected = resolved.sha256;
    if (!expected.empty() && !hex64(expected)) throw cymcp::Error("InvalidHash", "SHA-256 must be 64 hex digits");

    std::uint64_t offset = resolved.ranges && fs::is_regular_file(partial, error) ? fs::file_size(partial, error) : 0;
    if (resolved.total && offset >= resolved.total) offset = 0; // stale leftover: start again
    {
        std::ofstream out(partial, std::ios::binary | (offset ? std::ios::app : std::ios::trunc));
        if (!out) throw cymcp::Error("WriteFailed", "Cannot write into " + to_utf8(request.directory));
        cymcp::HttpRequest get;
        get.url = resolved.url;
        if (offset) get.headers["Range"] = "bytes=" + std::to_string(offset) + "-";
        if (!request.token.empty() &&
            origin(cymcp::parse_url(resolved.url)) == origin(cymcp::parse_url(request.url)))
            get.headers["Authorization"] = "Bearer " + request.token;
        get.timeout = std::chrono::milliseconds(0);
        get.idle_timeout = std::chrono::seconds(90);
        get.max_response_bytes = std::numeric_limits<std::size_t>::max();
        get.error_body_limit = 64 * 1024;
        std::uint64_t done = offset;
        const auto response =
            cymcp::HttpClient().perform(get, job.cancellation(), [&](const char* data, std::size_t size) {
                out.write(data, static_cast<std::streamsize>(size));
                done += size;
                job.update(done, resolved.total, name);
                return static_cast<bool>(out);
            });
        out.close();
        if (response.status != 200 && response.status != 206)
            throw cymcp::Error("DownloadFailed", "The server answered HTTP " + std::to_string(response.status));
        if (!out) throw cymcp::Error("WriteFailed", "The disk refused the data (is it full?)");
        if (offset && response.status == 200) {
            // The server restarted from the beginning although a range was asked.
            fs::remove(partial, error);
            throw cymcp::Error("DownloadFailed", "The server could not resume; start the download again");
        }
        if (resolved.total && done != resolved.total)
            throw cymcp::Error("DownloadFailed", "The download stopped early; start it again to resume");
    }

    Downloaded result;
    if (!expected.empty()) {
        job.update(0, 0, "Verifying " + name);
        const auto actual = lower(cymcp::sha256_file(partial, job.cancellation()));
        if (actual != expected) {
            fs::remove(partial, error);
            throw cymcp::Error("HashMismatch", "The downloaded file does not match its SHA-256; it was removed");
        }
        result.sha256 = actual;
    }
    fs::rename(partial, target, error);
    if (error) throw cymcp::Error("WriteFailed", "Cannot finalise " + name);
    result.path = target;
    return result;
}
} // namespace modelhub
