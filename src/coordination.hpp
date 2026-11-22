// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model.hpp"

#include <condition_variable>
#include <map>
#include <set>

namespace modelhub {
// Requests that change something may carry an idempotency key. The ledger remembers what each
// key did, so that a request sent again after a lost answer has no second effect:
//   same key, same arguments   -> the remembered answer, and the outcome of its job if any;
//   same key, other arguments  -> IdempotencyConflict;
//   key whose job was cut short by a stop of the service -> the work is taken up again.
// Kept in <data>/requests.json for `retention_seconds` after the request; neither the key
// (stored as its SHA-256) nor the fingerprint of the arguments contains a credential.
class Ledger {
  public:
    static constexpr std::int64_t retention_seconds = 7 * 24 * 3600;
    static constexpr std::size_t max_records = 2000;

    Ledger(fs::path file, std::string run);
    // Fingerprint of a request: its operation and arguments, without the key or any token.
    static std::string fingerprint(const std::string& operation, const Json& arguments);
    // Runs `work` at most once per key; see above. `work` returns the answer of the operation.
    Json run(const std::string& operation, const std::string& key, const std::string& fingerprint,
             const std::function<Json()>& work);
    // Remembers how a job ended, for the requests that started it.
    void job_finished(const Json& job);
    // A job of this run or of an earlier one, as far as the ledger knows it.
    std::optional<Json> job(const std::string& id) const;

  private:
    struct Record {
        std::string operation, fingerprint, run, job;
        std::int64_t created = 0;
        bool starting = false;
        Json response, outcome;
    };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    fs::path file_;
    std::string run_;
    std::map<std::string, Record> records_; // by SHA-256 of the key

    void save_locked();
    void prune_locked();
};

// One installation per artifact. A second request for the same artifact joins the work in
// progress instead of downloading again; each requester holds a ticket, and the work is only
// cancelled when the last of them leaves. The reservation lasts until the job really stopped.
// It is written to <data>/installs.json: what a stopped service left there is taken up again,
// at the same place, by the next request for the artifact.
class Reservations {
  public:
    struct Reservation {
        std::string artifact, job, ticket;
        fs::path target;
        bool joined = false;  // another requester had already started this installation
        bool resumed = false; // continues what a previous run of the service left unfinished
        std::size_t requesters = 0;
    };
    struct Left {
        bool known = false, last = false;
        std::size_t requesters = 0;
        std::string job;
    };

    explicit Reservations(fs::path file);
    static std::string artifact_id(const Json& identity);
    // Join the installation of this artifact, or reserve it and call `start`, which returns
    // the ID of the job it started, or an empty text when there was nothing to do. `start`
    // receives the place to install to: the requested one, or the place an interrupted
    // installation was using. Throws DestinationBusy when another artifact is being written
    // to the same place.
    Reservation acquire(const std::string& artifact, const Json& identity, const fs::path& target,
                        const std::function<std::string(const fs::path& target, bool resumed)>& start);
    // Called by the job when it has really stopped, whatever its outcome.
    void release(const std::string& artifact);
    // One requester gives up. `last` tells that nobody waits any more.
    Left leave(const std::string& job, const std::string& ticket);
    std::size_t requesters(const std::string& job) const;
    // Installations in progress, and those a stopped service left unfinished.
    Json list() const;
    // The installation in progress whose identity satisfies `match`, as in list().
    std::optional<Json> find(const std::function<bool(const Json& identity)>& match) const;

  private:
    struct Entry {
        std::string job;
        Json identity;
        fs::path target;
        std::set<std::string> tickets;
        std::int64_t started = 0;
    };
    mutable std::mutex mutex_;
    fs::path file_;
    std::map<std::string, Entry> active_, interrupted_;

    void save_locked() const;
    static Json describe(const std::string& artifact, const Entry& entry, const char* state);
};
} // namespace modelhub
