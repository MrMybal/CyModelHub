// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "catalog.hpp"
#include "jobs.hpp"

namespace modelhub {
// The state behind the service: one catalogue and its background jobs.
struct Hub {
    fs::path data_directory;
    Catalog catalog;
    Jobs jobs; // declared after the catalogue: jobs stop before the catalogue goes away
    std::atomic_bool shutdown{false};

    explicit Hub(fs::path directory);
    // Scan one location, or every enabled one when the ID is empty. Returns the job ID. While a
    // scan runs, further requests are queued behind it and share its job.
    std::string start_scan(const std::string& root_id);

  private:
    std::mutex scan_mutex_;
    bool scan_active_ = false;
    std::string scan_job_;
    std::vector<std::string> scan_pending_;
    Json scan(const std::string& root_id, const Job& job);
};

// Every operation of CyModelHub, exposed identically to applications (/v1) and to MCP (/mcp).
std::shared_ptr<cymcp::Registry> make_registry(std::shared_ptr<Hub> hub);
} // namespace modelhub
