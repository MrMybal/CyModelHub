// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "jobs.hpp"

namespace modelhub {
struct AppUpdate {
    std::string tag, version, url, sha256;
    std::size_t size;
    bool prerelease;

    Json describe() const;
};

bool valid_app_version(std::string_view version);
std::optional<AppUpdate> select_app_update(const Json& releases, std::string_view current, bool prereleases);
bool matches_app_update(const std::string& bytes, const AppUpdate& update);
Json check_app_updates(std::string_view current, bool prereleases, const cymcp::Cancellation& cancellation);
Json download_app_update(const fs::path& directory, std::string_view current, std::string_view tag,
                         bool prereleases, const Job& job);
} // namespace modelhub
