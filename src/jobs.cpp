// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "jobs.hpp"

namespace modelhub {
namespace {
constexpr std::size_t kept_finished = 40;
}

Jobs::Jobs() : run_(cymcp::sha256(cymcp::random_token(16)).substr(0, 4)) {}

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
        if (quiescing_) throw cymcp::Error("ShuttingDown", "CyModelHub is stopping for an update");
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
        entry->id = "j" + run_ + "-" + std::to_string(++counter_);
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
        std::string state = "done", message, code;
        const auto failed = [&](const std::exception& error, std::string error_code) {
            state = entry->cancellation.cancelled() ? "cancelled" : "failed";
            code = state == "cancelled" ? "Cancelled" : std::move(error_code);
            message = cymcp::redact_error_message(system_text(error.what()));
        };
        try {
            result = work(Job(*this, entry->id, entry->cancellation));
        } catch (const cymcp::Error& error) {
            failed(error, error.code);
        } catch (const Failure& error) {
            failed(error, error.code);
        } catch (const std::exception& error) {
            failed(error, "OperationFailed");
        } catch (...) {
            state = "failed";
            code = "OperationFailed";
            message = "Unexpected error";
        }
        Json record;
        {
            std::lock_guard lock(mutex_);
            entry->state = std::move(state);
            entry->code = std::move(code);
            if (!message.empty()) entry->message = std::move(message);
            entry->result = std::move(result);
            entry->finished = now_seconds();
            record = describe(*entry);
        }
        if (on_finished) {
            try {
                on_finished(record);
            } catch (const std::exception&) {
            }
        }
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

bool Jobs::quiesce_if_idle() {
    std::lock_guard lock(mutex_);
    for (const auto& entry : entries_)
        if (entry->state == "running") return false;
    quiescing_ = true;
    return true;
}

Json Jobs::describe(const Entry& e) {
    Json value = {{"id", e.id},       {"kind", e.kind},       {"title", e.title},
                  {"state", e.state}, {"message", e.message}, {"done", e.done},
                  {"total", e.total}, {"started", e.started}, {"finished", e.finished}};
    // Stable identifier of a failure, e.g. ProviderAccessRequired, HashMismatch, Cancelled.
    if (!e.code.empty()) value["code"] = e.code;
    if (!e.result.is_null()) value["result"] = e.result;
    return value;
}

Json Jobs::list() const {
    std::lock_guard lock(mutex_);
    Json jobs = Json::array();
    for (const auto& e : entries_) jobs.push_back(describe(*e));
    return jobs;
}

std::optional<Json> Jobs::find(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_)
        if (e->id == id) return describe(*e);
    return std::nullopt;
}

Json Jobs::get(const std::string& id) const {
    if (auto job = find(id)) return std::move(*job);
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
