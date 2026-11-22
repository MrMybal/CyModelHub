// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cymodelhub/client.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace app {
using cymodelhub::Json;

// What the interface shows. Published as a whole by the worker thread; the interface only
// ever reads an immutable copy.
struct Snapshot {
    bool connected = false;
    std::string error;
    Json status = Json::object(), facets = Json::object(), roots = Json::array(), jobs = Json::array(),
         duplicates = Json::object();
    std::vector<cymodelhub::Model> models;
    std::uint64_t generation = 0; // changes whenever `models` was reloaded
};

struct Filter {
    std::string text, type, modality, source, root;
    bool favorites = false, missing = false;
    bool operator==(const Filter&) const = default;
};

struct Settings {
    std::string locale; // empty: follow the system
    std::string theme = "mid-grey";
    bool comfortable = true;
};
Settings load_settings();
void save_settings(const Settings& settings);

// The application talks to the service through the same client library as any other program.
// Requests run on a worker thread so the interface never waits on them.
class Data {
    using Action = std::function<Json(cymodelhub::Client&)>;
    using Done = std::function<void(const Json& result, const std::string& error)>;
    struct Task {
        Action action;
        Done done;
    };
    struct Finished {
        Done done;
        Json result;
        std::string error;
    };

    cymodelhub::Client client_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> tasks_;
    std::deque<Finished> finished_;
    std::shared_ptr<const Snapshot> snapshot_ = std::make_shared<Snapshot>();
    Filter filter_;
    bool reload_ = true, stop_ = false;
    std::thread worker_;

    void work();
    void poll(bool reload, const Filter& filter, std::uint64_t& revision);

  public:
    Data();
    ~Data();
    std::shared_ptr<const Snapshot> snapshot() const;
    void set_filter(Filter filter);
    void refresh();
    // Run a request in the background; `done` is called later from pump(), on the caller's thread.
    void run(Action action, Done done = {});
    void pump();
};
} // namespace app
