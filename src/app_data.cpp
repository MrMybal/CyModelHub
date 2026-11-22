// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "app_data.hpp"
#include "platform.hpp"

namespace app {
namespace {
modelhub::fs::path settings_file() { return modelhub::data_path() / "app.json"; }
} // namespace

Settings load_settings() {
    Settings settings;
    const auto content = modelhub::read_file(settings_file(), 65536);
    const auto json = content ? Json::parse(*content, nullptr, false) : Json();
    if (!json.is_object()) return settings;
    settings.locale = json.value("locale", settings.locale);
    settings.theme = json.value("theme", settings.theme);
    settings.comfortable = json.value("comfortable", settings.comfortable);
    settings.update_prereleases = json.value("updatePrereleases", settings.update_prereleases);
    return settings;
}

void save_settings(const Settings& settings) {
    try {
        const Json json = {{"locale", settings.locale}, {"theme", settings.theme},
                           {"comfortable", settings.comfortable}, {"updatePrereleases", settings.update_prereleases}};
        modelhub::write_file_atomic(settings_file(), json.dump(2));
    } catch (const std::exception&) {
        // Preferences are a convenience: a read-only data folder must not stop the application.
    }
}

Data::Data() : worker_([this] { work(); }) {}

Data::~Data() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

std::shared_ptr<const Snapshot> Data::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void Data::set_filter(Filter filter) {
    {
        std::lock_guard lock(mutex_);
        if (filter == filter_) return;
        filter_ = std::move(filter);
        reload_ = true;
    }
    wake_.notify_all();
}

void Data::refresh() {
    {
        std::lock_guard lock(mutex_);
        reload_ = true;
    }
    wake_.notify_all();
}

void Data::run(Action action, Done done) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back({std::move(action), std::move(done)});
    }
    wake_.notify_all();
}

void Data::pump() {
    std::deque<Finished> finished;
    {
        std::lock_guard lock(mutex_);
        finished.swap(finished_);
    }
    for (auto& item : finished)
        if (item.done) item.done(item.result, item.error);
}

void Data::work() {
    std::uint64_t revision = ~std::uint64_t{0};
    auto last_poll = std::chrono::steady_clock::now() - std::chrono::hours(1);
    for (;;) {
        std::deque<Task> tasks;
        Filter filter;
        bool reload = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(250),
                           [&] { return stop_ || reload_ || !tasks_.empty(); });
            if (stop_) return;
            tasks.swap(tasks_);
            reload = reload_;
            reload_ = false;
            filter = filter_;
        }
        for (auto& task : tasks) {
            Finished finished{std::move(task.done), {}, {}};
            try {
                finished.result = task.action(client_);
            } catch (const std::exception& error) {
                finished.error = error.what();
            }
            std::lock_guard lock(mutex_);
            finished_.push_back(std::move(finished));
            reload = true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (reload || now - last_poll > std::chrono::milliseconds(700)) {
            poll(reload, filter, revision);
            last_poll = now;
        }
    }
}

void Data::poll(bool reload, const Filter& filter, std::uint64_t& revision) {
    auto next = std::make_shared<Snapshot>(*snapshot());
    try {
        next->status = client_.invoke("hub_status");
        next->jobs = client_.invoke("jobs_list").at("jobs");
        const auto current = next->status.value("revision", std::uint64_t{0});
        if (reload || current != revision || !next->connected) {
            Json arguments = {{"limit", 500}};
            if (!filter.text.empty()) arguments["query"] = filter.text;
            if (!filter.type.empty()) arguments["type"] = filter.type;
            if (!filter.modality.empty()) arguments["modality"] = filter.modality;
            if (!filter.source.empty()) arguments["source"] = filter.source;
            if (!filter.root.empty()) arguments["root"] = filter.root;
            if (filter.favorites) arguments["favorite"] = true;
            if (filter.missing) arguments["missing"] = true;
            std::vector<cymodelhub::Model> models;
            for (std::size_t offset = 0; offset < 100000; offset += 500) {
                arguments["offset"] = offset;
                const auto page = client_.invoke("models_list", arguments);
                for (const auto& item : page.at("models")) models.push_back(cymodelhub::model_from_json(item));
                if (models.size() >= page.value("total", std::size_t{0}) || page.at("models").empty()) break;
            }
            next->models = std::move(models);
            next->facets = client_.invoke("models_facets");
            next->roots = client_.invoke("roots_list").at("roots");
            next->duplicates = client_.invoke("models_duplicates");
            ++next->generation;
            revision = current;
        }
        next->connected = true;
        next->error.clear();
    } catch (const std::exception& error) {
        next->connected = false;
        next->error = error.what();
        revision = ~std::uint64_t{0};
    }
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(next);
}
} // namespace app
