// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "coordination.hpp"

#include <algorithm>

namespace modelhub {
namespace {
using cymcp::Error;

std::string text(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// Arguments that carry a credential never enter a fingerprint or a file.
void strip_secrets(Json& arguments) {
    if (!arguments.is_object()) return;
    arguments.erase("idempotencyKey");
    arguments.erase("token");
}

// What is kept of a finished job: enough for the requester to learn how it ended.
Json outcome_of(const Json& job) {
    Json outcome = Json::object();
    for (const auto* key :
         {"id", "kind", "title", "state", "code", "message", "started", "finished", "result"})
        if (const auto found = job.find(key); found != job.end()) outcome[key] = *found;
    return outcome;
}
} // namespace

Ledger::Ledger(fs::path file, std::string run) : file_(std::move(file)), run_(std::move(run)) {
    const auto content = read_file(file_);
    const auto json = content ? Json::parse(*content, nullptr, false) : Json();
    const auto records = json.is_object() ? json.find("records") : json.end();
    if (!json.is_object() || records == json.end() || !records->is_array()) return;
    for (const auto& item : *records) {
        if (!item.is_object()) continue;
        Record record;
        record.operation = text(item, "operation");
        record.fingerprint = text(item, "fingerprint");
        record.run = text(item, "run");
        record.job = text(item, "job");
        record.created = item.value("created", std::int64_t{0});
        record.response = item.value("response", Json());
        record.outcome = item.value("outcome", Json());
        const auto key = text(item, "key");
        if (key.size() == 64 && !record.operation.empty()) records_.emplace(key, std::move(record));
    }
    std::lock_guard lock(mutex_);
    prune_locked();
}

std::string Ledger::fingerprint(const std::string& operation, const Json& arguments) {
    auto copy = arguments;
    strip_secrets(copy);
    // Object keys are sorted, so the same arguments always give the same text.
    return cymcp::sha256(operation + '\n' + copy.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Ledger::prune_locked() {
    const auto oldest = now_seconds() - retention_seconds;
    std::erase_if(records_,
                  [&](const auto& item) { return !item.second.starting && item.second.created < oldest; });
    while (records_.size() > max_records) {
        auto victim = records_.end();
        for (auto item = records_.begin(); item != records_.end(); ++item)
            if (!item->second.starting &&
                (victim == records_.end() || item->second.created < victim->second.created))
                victim = item;
        if (victim == records_.end()) break;
        records_.erase(victim);
    }
}

void Ledger::save_locked() {
    Json records = Json::array();
    for (const auto& [key, record] : records_) {
        if (record.starting) continue;
        records.push_back({{"key", key},
                           {"operation", record.operation},
                           {"fingerprint", record.fingerprint},
                           {"run", record.run},
                           {"job", record.job},
                           {"created", record.created},
                           {"response", record.response},
                           {"outcome", record.outcome}});
    }
    const Json json = {{"version", 1}, {"records", std::move(records)}};
    write_file_atomic(file_, json.dump(1, ' ', false, Json::error_handler_t::replace), true);
}

Json Ledger::run(const std::string& operation, const std::string& key, const std::string& fingerprint,
                 const std::function<Json()>& work) {
    const auto hashed = cymcp::sha256(key);
    bool resumed = false;
    {
        std::unique_lock lock(mutex_);
        prune_locked();
        for (;;) {
            const auto found = records_.find(hashed);
            if (found == records_.end()) break;
            auto& record = found->second;
            if (record.operation != operation || record.fingerprint != fingerprint)
                throw Error("IdempotencyConflict",
                            "This idempotency key was already used for another request. "
                            "Use a new key for new arguments");
            if (record.starting) {
                // The first request with this key is being handled: answer both the same way.
                changed_.wait(lock);
                continue;
            }
            // A job that an earlier run of the service left unfinished never had its effect.
            if (!record.job.empty() && record.outcome.is_null() && record.run != run_) {
                resumed = true;
                break;
            }
            auto response = record.response;
            response["replayed"] = true;
            if (!record.outcome.is_null()) response["outcome"] = record.outcome;
            return response;
        }
        auto& record = records_[hashed];
        record.operation = operation;
        record.fingerprint = fingerprint;
        record.run = run_;
        record.created = now_seconds();
        record.starting = true;
        record.job.clear();
        record.outcome = Json();
    }
    Json response;
    try {
        response = work();
    } catch (...) {
        // A request that was refused had no effect: the key stays free for a corrected one.
        std::lock_guard lock(mutex_);
        records_.erase(hashed);
        changed_.notify_all();
        throw;
    }
    if (resumed && response.is_object()) response["resumed"] = true;
    std::lock_guard lock(mutex_);
    auto& record = records_[hashed];
    record.starting = false;
    record.response = response;
    record.job = response.is_object() ? text(response, "job") : std::string();
    save_locked();
    changed_.notify_all();
    return response;
}

void Ledger::job_finished(const Json& job) {
    const auto id = text(job, "id");
    std::unique_lock lock(mutex_);
    // The job may end before the request that started it has been recorded.
    changed_.wait_for(lock, std::chrono::seconds(10), [&] {
        return std::none_of(records_.begin(), records_.end(),
                            [](const auto& item) { return item.second.starting; });
    });
    bool found = false;
    for (auto& [key, record] : records_)
        if (record.job == id && record.run == run_) {
            record.outcome = outcome_of(job);
            found = true;
        }
    if (found) save_locked();
}

std::optional<Json> Ledger::job(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& [key, record] : records_) {
        if (record.job != id) continue;
        if (!record.outcome.is_null()) return record.outcome;
        if (record.run != run_)
            return Json{{"id", id},
                        {"state", "interrupted"},
                        {"code", "Interrupted"},
                        {"message", "The service stopped before this job ended. Send the same request again, "
                                    "with the same idempotency key, to take it up again"}};
    }
    return std::nullopt;
}

Reservations::Reservations(fs::path file) : file_(std::move(file)) {
    const auto content = read_file(file_);
    const auto json = content ? Json::parse(*content, nullptr, false) : Json();
    const auto installs = json.is_object() ? json.find("installs") : json.end();
    if (!json.is_object() || installs == json.end() || !installs->is_array()) return;
    // Whatever is listed was being installed when the service stopped.
    for (const auto& item : *installs) {
        if (!item.is_object()) continue;
        Entry entry;
        entry.identity = item.value("identity", Json::object());
        entry.target = from_utf8(text(item, "target"));
        entry.started = item.value("started", std::int64_t{0});
        const auto artifact = text(item, "artifact");
        if (!artifact.empty() && entry.target.is_absolute()) interrupted_.emplace(artifact, std::move(entry));
    }
}

std::string Reservations::artifact_id(const Json& identity) {
    return "a" + cymcp::sha256(identity.dump(-1, ' ', false, Json::error_handler_t::replace)).substr(0, 31);
}

Json Reservations::describe(const std::string& artifact, const Entry& entry, const char* state) {
    return {{"artifact", artifact},
            {"state", state},
            {"job", entry.job},
            {"identity", entry.identity},
            {"target", to_utf8(entry.target)},
            {"started", entry.started},
            {"requesters", entry.tickets.size()}};
}

void Reservations::save_locked() const {
    Json installs = Json::array();
    for (const auto* group : {&active_, &interrupted_})
        for (const auto& [artifact, entry] : *group)
            installs.push_back({{"artifact", artifact},
                                {"identity", entry.identity},
                                {"target", to_utf8(entry.target)},
                                {"started", entry.started}});
    const Json json = {{"version", 1}, {"installs", std::move(installs)}};
    write_file_atomic(file_, json.dump(1, ' ', false, Json::error_handler_t::replace), true);
}

Reservations::Reservation
Reservations::acquire(const std::string& artifact, const Json& identity, const fs::path& target,
                      const std::function<std::string(const fs::path& target, bool resumed)>& start) {
    std::lock_guard lock(mutex_);
    Reservation reservation;
    reservation.artifact = artifact;
    reservation.ticket = cymcp::sha256(cymcp::random_token(16)).substr(0, 16);
    if (const auto running = active_.find(artifact); running != active_.end()) {
        running->second.tickets.insert(reservation.ticket);
        reservation.job = running->second.job;
        reservation.target = running->second.target;
        reservation.joined = true;
        reservation.requesters = running->second.tickets.size();
        return reservation;
    }
    Entry entry;
    entry.identity = identity;
    entry.target = target;
    entry.started = now_seconds();
    if (const auto left = interrupted_.find(artifact); left != interrupted_.end()) {
        // Partial files are where the interrupted installation put them: continue there.
        entry.target = left->second.target;
        reservation.resumed = true;
    }
    for (const auto& [other, running] : active_)
        if (path_key(running.target) == path_key(entry.target))
            throw Error("DestinationBusy", "Another model is being installed at this place");
    entry.tickets.insert(reservation.ticket);
    reservation.target = entry.target;
    reservation.requesters = 1;
    const auto place = active_.emplace(artifact, std::move(entry)).first;
    try {
        // The job cannot release the reservation before it is complete: release() waits for
        // this lock.
        place->second.job = start(place->second.target, reservation.resumed);
    } catch (...) {
        active_.erase(place);
        throw;
    }
    interrupted_.erase(artifact);
    if (place->second.job.empty()) {
        active_.erase(place);
        reservation.requesters = 0;
    } else {
        reservation.job = place->second.job;
    }
    save_locked();
    return reservation;
}

void Reservations::release(const std::string& artifact) {
    std::lock_guard lock(mutex_);
    if (!active_.erase(artifact)) return;
    try {
        save_locked();
    } catch (const std::exception&) {
    }
}

Reservations::Left Reservations::leave(const std::string& job, const std::string& ticket) {
    std::lock_guard lock(mutex_);
    Left left;
    for (auto& [artifact, entry] : active_) {
        if (entry.job != job) continue;
        left.job = entry.job;
        left.known = entry.tickets.erase(ticket) > 0;
        left.requesters = entry.tickets.size();
        left.last = left.known && entry.tickets.empty();
    }
    return left;
}

std::size_t Reservations::requesters(const std::string& job) const {
    std::lock_guard lock(mutex_);
    for (const auto& [artifact, entry] : active_)
        if (entry.job == job) return entry.tickets.size();
    return 0;
}

Json Reservations::list() const {
    std::lock_guard lock(mutex_);
    Json installs = Json::array();
    for (const auto& [artifact, entry] : active_) installs.push_back(describe(artifact, entry, "running"));
    for (const auto& [artifact, entry] : interrupted_)
        installs.push_back(describe(artifact, entry, "interrupted"));
    return installs;
}

std::optional<Json> Reservations::find(const std::function<bool(const Json& identity)>& match) const {
    std::lock_guard lock(mutex_);
    for (const auto& [artifact, entry] : active_)
        if (match(entry.identity)) return describe(artifact, entry, "running");
    return std::nullopt;
}
} // namespace modelhub
