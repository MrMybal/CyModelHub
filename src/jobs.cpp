// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "jobs.hpp"

namespace modelhub {
namespace {
constexpr std::size_t kept_finished = 40;
}

void Job::update(std::uint64_t done, std::uint64_t total, std::string message) const {
    std::lock_guard lock(jobs_.mutex_);
    for (const auto& entry : jobs_.entries_)
        if (entry->id == id_) {
            entry->done = done;
            entry->total = total;
            if (!message.empty()) entry->message = std::move(message);
            return;
        }
}

Jobs::~Jobs() {
    std::list<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock(mutex_);
        entries = entries_;
    }
    for (const auto& entry : entries) entry->cancellation.cancel();
    for (const auto& entry : entries)
        if (entry->thread.joinable()) entry->thread.join();
}

std::string Jobs::start(std::string kind, std::string title, Work work) {
    auto entry = std::make_shared<Entry>();
    std::vector<std::shared_ptr<Entry>> finished;
    {
        std::lock_guard lock(mutex_);
        // Forget the oldest finished jobs; their threads have already returned.
        std::size_t done = 0;
        for (const auto& existing : entries_) done += existing->state != "running";
        for (auto item = entries_.begin(); item != entries_.end() && done > kept_finished;) {
            if ((*item)->state == "running") {
                ++item;
                continue;
            }
            finished.push_back(*item);
            item = entries_.erase(item);
            --done;
        }
        entry->id = "j" + std::to_string(++counter_);
        entry->kind = std::move(kind);
        entry->title = std::move(title);
        entry->started = now_seconds();
        entries_.push_back(entry);
    }
    finished.clear(); // joins outside the lock
    // The thread must not own the entry: an entry is only ever destroyed (and its thread
    // joined) by whoever holds the list, never by the job's own thread.
    entry->thread = std::jthread([this, entry = entry.get(), work = std::move(work)] {
        Json result;
        std::string state = "done", message;
        try {
            result = work(Job(*this, entry->id, entry->cancellation));
        } catch (const std::exception& error) {
            state = entry->cancellation.cancelled() ? "cancelled" : "failed";
            message = cymcp::redact_error_message(system_text(error.what()));
        } catch (...) {
            state = "failed";
            message = "Unexpected error";
        }
        std::lock_guard lock(mutex_);
        entry->state = std::move(state);
        if (!message.empty()) entry->message = std::move(message);
        entry->result = std::move(result);
        entry->finished = now_seconds();
    });
    return entry->id;
}

std::string Jobs::running(const std::string& kind) const {
    std::lock_guard lock(mutex_);
    for (const auto& entry : entries_)
        if (entry->kind == kind && entry->state == "running") return entry->id;
    return {};
}

std::size_t Jobs::running_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : entries_) count += entry->state == "running";
    return count;
}

namespace {
Json describe(const std::string& id, const std::string& kind, const std::string& title, const std::string& state,
              const std::string& message, std::uint64_t done, std::uint64_t total, std::int64_t started,
              std::int64_t finished, const Json& result) {
    Json value = {{"id", id},     {"kind", kind},   {"title", title},     {"state", state},
                  {"message", message}, {"done", done}, {"total", total}, {"started", started},
                  {"finished", finished}};
    if (!result.is_null()) value["result"] = result;
    return value;
}
} // namespace

Json Jobs::list() const {
    std::lock_guard lock(mutex_);
    Json jobs = Json::array();
    for (const auto& e : entries_)
        jobs.push_back(describe(e->id, e->kind, e->title, e->state, e->message, e->done, e->total, e->started,
                                e->finished, e->result));
    return jobs;
}

Json Jobs::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_)
        if (e->id == id)
            return describe(e->id, e->kind, e->title, e->state, e->message, e->done, e->total, e->started,
                            e->finished, e->result);
    throw cymcp::Error("NotFound", "No job has this ID");
}

bool Jobs::cancel(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& entry : entries_)
        if (entry->id == id && entry->state == "running") {
            entry->cancellation.cancel();
            return true;
        }
    return false;
}
} // namespace modelhub
