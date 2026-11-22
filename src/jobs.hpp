// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "platform.hpp"
#include <list>

namespace modelhub {
class Jobs;

// Handle given to background work to report progress and observe cancellation.
class Job {
    friend class Jobs;
    Jobs& jobs_;
    std::string id_;
    cymcp::Cancellation cancellation_;
    Job(Jobs& jobs, std::string id, cymcp::Cancellation cancellation)
        : jobs_(jobs), id_(std::move(id)), cancellation_(std::move(cancellation)) {}

  public:
    const std::string& id() const { return id_; }
    const cymcp::Cancellation& cancellation() const { return cancellation_; }
    // total == 0 means the amount of work is not known.
    void update(std::uint64_t done, std::uint64_t total, std::string message = {}) const;
};

// Long operations (scan, download, move, hash) run here so a request returns at once.
class Jobs {
    friend class Job;
    struct Entry {
        std::string id, kind, title, state = "running", message;
        std::uint64_t done = 0, total = 0;
        std::int64_t started = 0, finished = 0;
        Json result;
        cymcp::Cancellation cancellation;
        std::jthread thread;
    };
    mutable std::mutex mutex_;
    std::list<std::shared_ptr<Entry>> entries_;
    std::uint64_t counter_ = 0;

  public:
    using Work = std::function<Json(const Job&)>;
    ~Jobs();
    std::string start(std::string kind, std::string title, Work work);
    // ID of a job of this kind that is still running, or an empty string.
    std::string running(const std::string& kind) const;
    std::size_t running_count() const;
    Json list() const;
    Json get(const std::string& id) const;
    bool cancel(const std::string& id);
};
} // namespace modelhub
