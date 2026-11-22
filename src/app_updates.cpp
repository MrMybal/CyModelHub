// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "app_updates.hpp"

#include <cymcp/http.hpp>

#include <array>
#include <charconv>
#include <mutex>

namespace modelhub {
namespace {
constexpr std::string_view release_api = "https://api.github.com/repos/MrMybal/CyModelHub/releases";
constexpr std::string_view release_page = "https://github.com/MrMybal/CyModelHub/releases";
constexpr std::size_t max_installer_size = 64 * 1024 * 1024;

using Version = std::array<unsigned, 3>;

// GitHub marks our numbered releases as prereleases independently of the tag.
std::optional<Version> version_of(std::string_view number) {
    Version version{};
    for (std::size_t index = 0; index < version.size(); ++index) {
        const auto dot = number.find('.');
        if ((dot == number.npos) != (index == version.size() - 1)) return {};
        const auto part = number.substr(0, dot);
        if (part.empty() || (part.size() > 1 && part.front() == '0')) return {};
        const auto end = part.data() + part.size();
        const auto result = std::from_chars(part.data(), end, version[index]);
        if (result.ec != std::errc{} || result.ptr != end) return {};
        if (dot != number.npos) number.remove_prefix(dot + 1);
    }
    return version;
}

std::string field(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool flag(const Json& object, const char* key, bool value) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>() == value;
}

std::optional<AppUpdate> installer_of(const Json& release, bool prereleases) {
    if (!flag(release, "draft", false) ||
        !(flag(release, "prerelease", false) || (prereleases && flag(release, "prerelease", true)))) return {};
    const auto tag = field(release, "tag_name");
    if (!tag.starts_with('v') || !version_of(std::string_view(tag).substr(1))) return {};
    const auto version = tag.substr(1);
    const auto filename = "CyModelHub-" + version + "-windows-x64-setup.exe";
    const auto url = std::string(release_page) + "/download/" + tag + "/" + filename;
    const auto assets = release.find("assets");
    if (assets == release.end() || !assets->is_array()) return {};
    for (const auto& asset : *assets) {
        if (field(asset, "name") != filename || field(asset, "state") != "uploaded" ||
            field(asset, "browser_download_url") != url) continue;
        const auto size = asset.find("size");
        const auto digest = field(asset, "digest");
        if (size == asset.end() || !size->is_number_unsigned() || size->get<std::uint64_t>() == 0 ||
            size->get<std::uint64_t>() > max_installer_size || digest.size() != 71 ||
            !digest.starts_with("sha256:")) continue;
        auto hash = lower(digest.substr(7));
        if (hash.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        return AppUpdate{tag, version, url, std::move(hash), size->get<std::size_t>(), flag(release, "prerelease", true)};
    }
    return {};
}

cymcp::HttpRequest github_get(std::string url, std::size_t limit) {
    cymcp::HttpRequest request;
    request.url = std::move(url);
    request.headers = {{"User-Agent", "CyModelHub/" CYMODELHUB_VERSION}};
    request.max_response_bytes = limit;
    request.timeout = std::chrono::seconds(35);
    return request;
}

Json release_metadata(std::string suffix, const cymcp::Cancellation& cancellation) {
    auto request = github_get(std::string(release_api) + suffix, 2 * 1024 * 1024);
    request.headers["Accept"] = "application/vnd.github+json";
    if (auto response = cymcp::HttpClient().perform(request, cancellation); response.status == 200) {
        auto json = Json::parse(response.body, nullptr, false);
        if (suffix.starts_with('?') ? json.is_array() : json.is_object()) return json;
    }
    throw cymcp::Error("UpdateCheckFailed", "Could not read CyModelHub releases from GitHub");
}

bool github_asset_host(const std::string& url) {
    const auto address = cymcp::parse_url(url);
    return address.scheme == "https" && address.port == 443 &&
           (address.host == "github.com" || address.host == "release-assets.githubusercontent.com" ||
            address.host == "objects.githubusercontent.com");
}

std::string fetch_installer(const AppUpdate& update, const Job& job) {
    auto url = update.url;
    for (int hop = 0; hop < 5; ++hop) {
        if (!github_asset_host(url)) break;
        auto request = github_get(url, update.size + 1);
        // Only 2xx payloads are collected by the sink.
        request.error_body_limit = 4096;
        std::string bytes;
        const auto response = cymcp::HttpClient().perform(request, job.cancellation(),
            [&](const char* chunk, std::size_t length) {
                if (length > update.size - bytes.size()) return false;
                bytes.append(chunk, length);
                job.update(bytes.size(), update.size);
                return true;
            });
        if (response.status == 200) return bytes;
        if (response.status < 300 || response.status >= 400) break;
        const auto redirect = response.headers.find("location");
        if (redirect == response.headers.end()) break;
        url = redirect->second;
    }
    throw cymcp::Error("UpdateDownloadFailed", "Could not download the CyModelHub installer from GitHub");
}
} // namespace

Json AppUpdate::describe() const {
    return {{"tag", tag}, {"version", version}, {"prerelease", prerelease}, {"size", size}};
}

bool valid_app_version(std::string_view version) { return version_of(version).has_value(); }

std::optional<AppUpdate> select_app_update(const Json& releases, std::string_view current, bool prereleases) {
    const auto installed = version_of(current);
    if (!installed || !releases.is_array()) return {};
    auto newest = *installed;
    std::optional<AppUpdate> selected;
    for (const auto& release : releases) {
        auto update = installer_of(release, prereleases);
        if (!update) continue;
        const auto version = *version_of(update->version);
        if (version <= newest) continue;
        newest = version;
        selected = std::move(update);
    }
    return selected;
}

bool matches_app_update(const std::string& bytes, const AppUpdate& update) {
    return bytes.size() == update.size && cymcp::sha256(bytes) == update.sha256;
}

Json check_app_updates(std::string_view current, bool prereleases, const cymcp::Cancellation& cancellation) {
    auto release = select_app_update(release_metadata("?per_page=30", cancellation), current, prereleases);
    return release ? Json{{"available", true}, {"release", release->describe()}} : Json{{"available", false}};
}

Json download_app_update(const fs::path& directory, std::string_view current, std::string_view tag,
                         bool prereleases, const Job& job) {
    if (!tag.starts_with('v') || !valid_app_version(tag.substr(1)))
        throw cymcp::Error("InvalidArguments", "Invalid CyModelHub release tag");
    const auto metadata = release_metadata("/tags/" + std::string(tag), job.cancellation());
    const auto update = select_app_update(Json::array({metadata}), current, prereleases);
    if (!update || update->tag != tag)
        throw cymcp::Error("UpdateNotAvailable", "This release has no eligible verified Windows installer");

    // This is a private service file, separate from all registered model locations.
    const auto path = directory / ("update-" + update->version + "-" + update->sha256.substr(0, 16) + ".exe");
    auto bytes = read_file(path, update->size);
    if (!bytes || !matches_app_update(*bytes, *update)) {
        bytes = fetch_installer(*update, job);
        if (!matches_app_update(*bytes, *update))
            throw cymcp::Error("HashMismatch", "The CyModelHub installer did not match its GitHub size and SHA-256");
        static std::mutex cache_mutex;
        std::lock_guard lock(cache_mutex);
        if (job.cancellation().cancelled()) throw cymcp::Error("Cancelled", "Update download cancelled");
        write_file_atomic(path, *bytes, true);
    }
    return {{"path", to_utf8(path)}, {"version", update->version}};
}
} // namespace modelhub
