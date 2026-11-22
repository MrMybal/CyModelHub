// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "files.hpp"
#include "formats.hpp"
#include "package.hpp"

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
    if (!request.any_extension && !model_extension(from_utf8(name)))
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
    std::string commit;    // revision the request was resolved to, when the server says so
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
        if (const auto commit = response.headers.find("x-repo-commit"); commit != response.headers.end())
            if (const auto value = lower(commit->second);
                value.size() == 40 && value.find_first_not_of("0123456789abcdef") == std::string::npos)
                resolved.commit = value;
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
            throw cymcp::Error("ProviderAccessRequired",
                               "The provider refused access (HTTP " + std::to_string(response.status) +
                                   "). A gated model needs an access token accepted by that provider");
        throw cymcp::Error("DownloadFailed", "The server answered HTTP " + std::to_string(response.status));
    }
    throw cymcp::Error("DownloadFailed", "Too many redirects");
}
} // namespace

std::string huggingface_url(const std::string& repository, const std::string& file, const std::string& revision) {
    return "https://huggingface.co/" + percent_encode_path(repository) + "/resolve/" +
           percent_encode_path(revision.empty() ? "main" : revision) + "/" + percent_encode_path(file);
}

std::string download_name(const Download& request) { return file_name(request); }

std::string public_url(const std::string& url) {
    auto address = url.substr(0, url.find_first_of("?#"));
    const auto scheme = address.find("://");
    if (scheme == std::string::npos) return address;
    const auto host = scheme + 3, path = std::min(address.find('/', host), address.size());
    if (const auto user = address.rfind('@', path); user != std::string::npos && user >= host)
        address.erase(host, user + 1 - host);
    return address;
}

Downloaded download_file(const Download& request, const Job& job) {
    const auto name = file_name(request);
    fs::create_directories(request.directory);
    const auto target = request.directory / from_utf8(name);
    auto partial = target;
    partial += ".part";
    std::error_code error;
    if (fs::exists(target, error)) throw cymcp::Error("AlreadyExists", name + " is already in this folder");

    job.update(request.progress_base, request.progress_total, name);
    const auto resolved = resolve(request, job);
    auto expected = lower(request.sha256);
    if (expected.empty()) expected = resolved.sha256;
    if (!expected.empty() && !hex64(expected)) throw cymcp::Error("InvalidHash", "SHA-256 must be 64 hex digits");
    if (request.size && resolved.total && request.size != resolved.total)
        throw cymcp::Error("SizeMismatch", name + " does not have the expected size on the server");
    if (request.known && request.known(expected)) {
        Downloaded skipped;
        skipped.skipped = true;
        skipped.sha256 = expected;
        skipped.commit = resolved.commit;
        return skipped;
    }
    const auto total = request.progress_total ? request.progress_total : resolved.total;

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
                job.update(request.progress_base + done, total, name);
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
        if (request.size && done != request.size) {
            fs::remove(partial, error);
            throw cymcp::Error("SizeMismatch", name + " does not have the expected size; it was removed");
        }
    }

    Downloaded result;
    result.commit = resolved.commit;
    result.size = fs::file_size(partial, error);
    if (!expected.empty() || request.always_hash) {
        job.update(request.progress_base + result.size, total, "Verifying " + name);
        const auto actual = lower(cymcp::sha256_file(partial, job.cancellation()));
        if (!expected.empty() && actual != expected) {
            fs::remove(partial, error);
            throw cymcp::Error("HashMismatch", "The downloaded file does not match its SHA-256; it was removed");
        }
        result.sha256 = actual;
        result.verified = !expected.empty();
    }
    fs::rename(partial, target, error);
    if (error) throw cymcp::Error("WriteFailed", "Cannot finalise " + name);
    result.path = target;
    return result;
}

PackageDownloaded download_package(const PackageDownload& request, const Job& job) {
    PackageDownloaded result;
    auto package = request.package;
    fs::create_directories(request.folder);
    std::uint64_t total = 0, done = 0;
    bool sized = true;
    for (const auto& file : package.at("files")) {
        sized = sized && file.contains("size");
        total += file.value("size", std::uint64_t{0});
    }
    if (!sized) total = 0;
    for (auto& file : package.at("files")) {
        job.cancellation().check();
        const auto relative = file.at("path").get<std::string>();
        const auto target = package_file(request.folder, relative, true);
        std::error_code error;
        if (request.staging && fs::is_regular_file(target, error) && file.contains("size") &&
            fs::file_size(target, error) != file["size"].get<std::uint64_t>())
            fs::remove(target, error); // a leftover of the wrong size cannot be this member
        if (fs::exists(target, error)) {
            // Left by an earlier attempt, or already part of the model: checked below.
            done += fs::is_regular_file(target, error) ? fs::file_size(target, error) : 0;
            continue;
        }
        Download download;
        download.url = request.address(relative);
        cymcp::parse_url(download.url);
        download.directory = target.parent_path();
        download.filename = to_utf8(target.filename());
        download.any_extension = true;
        download.always_hash = true;
        download.token = request.token;
        download.sha256 = file.value("sha256", std::string());
        download.size = file.value("size", std::uint64_t{0});
        download.progress_base = done;
        download.progress_total = total;
        // The folder was just created inside the model: check again that nothing leads out.
        fs::create_directories(download.directory);
        package_file(request.folder, relative, true);
        try {
            const auto downloaded = download_file(download, job);
            done += downloaded.size;
            if (result.commit.empty()) result.commit = downloaded.commit;
            // A hash announced by the server is a reference, like one given by the caller.
            if (!file.contains("sha256") && !downloaded.sha256.empty()) {
                file["sha256"] = downloaded.sha256;
                file["hashSource"] = downloaded.verified ? "manifest" : "measured";
            }
        } catch (const cymcp::Error& failure) {
            // An optional member that the source does not have is simply absent.
            if (file.value("required", true) || failure.code != "DownloadFailed") throw;
        }
    }
    // Everything is read once more, whole: what an earlier attempt left is not trusted blindly.
    auto verified = verify_package(request.folder, std::move(package), true, &job, job.cancellation());
    if (request.staging)
        for (const auto& file : verified.package.at("files"))
            if (const auto state = file.value("state", std::string()); state == "hash" || state == "size") {
                std::error_code error;
                fs::remove(package_file(request.folder, file.at("path").get<std::string>()), error);
            }
    result.package = std::move(verified.package);
    result.validation = std::move(verified.validation);
    result.size = verified.size;
    result.files = verified.files;
    result.modified = verified.modified;
    return result;
}
} // namespace modelhub
