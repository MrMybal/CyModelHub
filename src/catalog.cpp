// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "catalog.hpp"
#include "scanner.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace modelhub {
namespace {
bool on_disk(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

// Refresh the detected facts of an entry while keeping its identity and the user's edits.
void merge_into(Model& existing, Model incoming) {
    const bool same_content = existing.size == incoming.size && existing.modified == incoming.modified;
    incoming.id = existing.id;
    incoming.added = existing.added;
    incoming.tags = std::move(existing.tags);
    incoming.notes = std::move(existing.notes);
    incoming.favorite = existing.favorite;
    incoming.locked = std::move(existing.locked);
    if (incoming.url.empty()) incoming.url = std::move(existing.url);
    if (incoming.repo.empty()) incoming.repo = std::move(existing.repo);
    if (incoming.sha256.empty() && same_content) incoming.sha256 = std::move(existing.sha256);
    if (incoming.is_locked("name")) incoming.name = std::move(existing.name);
    if (incoming.is_locked("type")) incoming.type = std::move(existing.type);
    if (incoming.is_locked("modality")) incoming.modality = std::move(existing.modality);
    if (incoming.is_locked("architecture")) incoming.architecture = std::move(existing.architecture);
    incoming.missing = false;
    existing = std::move(incoming);
}
} // namespace

Catalog::Catalog(fs::path file) : file_(std::move(file)) {
    const auto content = read_file(file_);
    const auto json = content ? Json::parse(*content, nullptr, false) : Json();
    if (!json.is_object()) return;
    detector_ = json.value("detector", 0);
    if (const auto models = json.find("models"); models != json.end() && models->is_array())
        for (const auto& item : *models) {
            if (!item.is_object()) continue;
            auto model = Model::from_json(item);
            if (!model.id.empty() && !model.path.empty()) models_.push_back(std::move(model));
        }
    if (const auto roots = json.find("roots"); roots != json.end() && roots->is_array())
        for (const auto& item : *roots) {
            if (!item.is_object()) continue;
            Root root;
            root.id = item.value("id", std::string());
            root.label = item.value("label", std::string());
            root.path = from_utf8(item.value("path", std::string()));
            root.enabled = item.value("enabled", true);
            root.recursive = item.value("recursive", true);
            if (!root.id.empty() && !root.path.empty()) user_roots_.push_back(std::move(root));
        }
    if (const auto disabled = json.find("disabledRoots"); disabled != json.end() && disabled->is_array())
        for (const auto& item : *disabled)
            if (item.is_string()) disabled_.push_back(item.get<std::string>());
}

void Catalog::save_locked() {
    ++revision_;
    Json models = Json::array(), roots = Json::array();
    for (const auto& model : models_) models.push_back(model.to_json(true));
    for (const auto& root : user_roots_) roots.push_back(root.to_json());
    const Json json = {{"version", 1}, {"detector", detector_}, {"models", models}, {"roots", roots},
                       {"disabledRoots", disabled_}};
    write_file_atomic(file_, json.dump(1, ' ', false, Json::error_handler_t::replace));
}

std::string Catalog::new_id_locked(const fs::path& path) {
    for (;;) {
        auto id = cymcp::sha256(to_utf8(path) + ':' + std::to_string(now_seconds()) + ':' +
                                std::to_string(++counter_))
                      .substr(0, 12);
        if (!find_locked(id)) return id;
    }
}

Model* Catalog::find_locked(const std::string& id) {
    const auto found = std::find_if(models_.begin(), models_.end(), [&](const Model& m) { return m.id == id; });
    return found == models_.end() ? nullptr : &*found;
}

bool Catalog::detector_current() const {
    std::lock_guard lock(mutex_);
    return detector_ == detector_version;
}

void Catalog::mark_detector_current() {
    std::lock_guard lock(mutex_);
    if (detector_ == detector_version) return;
    detector_ = detector_version;
    save_locked();
}

std::uint64_t Catalog::revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
}

std::vector<Model> Catalog::models() const {
    std::lock_guard lock(mutex_);
    return models_;
}

std::optional<Model> Catalog::find(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(models_.begin(), models_.end(), [&](const Model& m) { return m.id == id; });
    return found == models_.end() ? std::nullopt : std::optional<Model>(*found);
}

std::vector<Root> Catalog::roots() const {
    auto roots = known_roots();
    std::lock_guard lock(mutex_);
    for (auto& root : roots)
        root.enabled = std::find(disabled_.begin(), disabled_.end(), root.id) == disabled_.end();
    roots.insert(roots.end(), user_roots_.begin(), user_roots_.end());
    return roots;
}

Root Catalog::add_root(const fs::path& path, std::string label, bool recursive) {
    std::error_code error;
    if (!path.is_absolute() || !fs::is_directory(path, error))
        throw cymcp::Error("InvalidPath", "The location must be an existing folder with an absolute path");
    for (const auto& known : known_roots())
        if (path_key(known.path) == path_key(path))
            throw cymcp::Error("AlreadyExists", "This location is already watched as " + known.label);
    std::lock_guard lock(mutex_);
    for (auto& existing : user_roots_)
        if (path_key(existing.path) == path_key(path)) {
            existing.recursive = recursive;
            existing.enabled = true;
            if (!label.empty()) existing.label = std::move(label);
            save_locked();
            return existing;
        }
    Root root;
    root.id = "r" + cymcp::sha256(path_key(path)).substr(0, 10);
    root.label = label.empty() ? to_utf8(path.filename().empty() ? path : path.filename()) : std::move(label);
    root.path = path.lexically_normal();
    root.recursive = recursive;
    user_roots_.push_back(root);
    save_locked();
    return root;
}

bool Catalog::remove_root(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::find_if(user_roots_.begin(), user_roots_.end(), [&](const Root& root) { return root.id == id; });
    if (found == user_roots_.end()) return false;
    user_roots_.erase(found);
    std::erase_if(models_, [&](const Model& model) { return model.root == id; });
    save_locked();
    return true;
}

bool Catalog::enable_root(const std::string& id, bool enabled) {
    const auto builtin = known_roots();
    std::lock_guard lock(mutex_);
    const auto user =
        std::find_if(user_roots_.begin(), user_roots_.end(), [&](const Root& root) { return root.id == id; });
    if (user != user_roots_.end()) {
        user->enabled = enabled;
    } else if (std::any_of(builtin.begin(), builtin.end(), [&](const Root& root) { return root.id == id; })) {
        std::erase(disabled_, id);
        if (!enabled) disabled_.push_back(id);
    } else {
        return false;
    }
    if (!enabled) std::erase_if(models_, [&](const Model& model) { return model.root == id; });
    save_locked();
    return true;
}

Catalog::MergeResult Catalog::merge_scan(const Root& root, std::vector<Model> found) {
    MergeResult result;
    result.found = found.size();
    std::lock_guard lock(mutex_);
    std::unordered_map<std::string, std::size_t> by_path;
    for (std::size_t index = 0; index < models_.size(); ++index) by_path[path_key(models_[index].path)] = index;
    std::unordered_set<std::size_t> seen;
    for (auto& incoming : found) {
        const auto key = path_key(incoming.path);
        if (const auto known = by_path.find(key); known != by_path.end()) {
            merge_into(models_[known->second], std::move(incoming));
            seen.insert(known->second);
            continue;
        }
        // A file that was renamed or moved outside CyModelHub keeps its entry and its notes.
        std::size_t moved = models_.size();
        if (!incoming.fingerprint.empty())
            for (std::size_t index = 0; index < models_.size(); ++index)
                if (!seen.contains(index) && models_[index].fingerprint == incoming.fingerprint &&
                    !on_disk(models_[index].path)) {
                    moved = index;
                    break;
                }
        if (moved != models_.size()) {
            by_path.erase(path_key(models_[moved].path));
            merge_into(models_[moved], std::move(incoming));
            by_path[key] = moved;
            seen.insert(moved);
            continue;
        }
        incoming.id = new_id_locked(incoming.path);
        incoming.added = now_seconds();
        incoming.missing = false;
        models_.push_back(std::move(incoming));
        by_path[key] = models_.size() - 1;
        seen.insert(models_.size() - 1);
        ++result.added;
    }
    for (std::size_t index = 0; index < models_.size(); ++index) {
        auto& model = models_[index];
        if (model.root != root.id || seen.contains(index) || on_disk(model.path)) continue;
        if (!model.missing) ++result.missing;
        model.missing = true;
    }
    save_locked();
    return result;
}

void Catalog::refresh_manual() {
    std::lock_guard lock(mutex_);
    bool changed = false;
    for (auto& model : models_) {
        if (model.root != "manual") continue;
        const bool missing = !on_disk(model.path);
        changed = changed || missing != model.missing;
        model.missing = missing;
    }
    if (changed) save_locked();
}

Model Catalog::add(Model model) {
    std::lock_guard lock(mutex_);
    const auto key = path_key(model.path);
    for (auto& existing : models_)
        if (path_key(existing.path) == key) {
            if (model.root.empty()) model.root = existing.root;
            if (model.source.empty()) model.source = existing.source;
            merge_into(existing, std::move(model));
            save_locked();
            return existing;
        }
    model.id = new_id_locked(model.path);
    model.added = now_seconds();
    models_.push_back(model);
    save_locked();
    return model;
}

bool Catalog::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!std::erase_if(models_, [&](const Model& model) { return model.id == id; })) return false;
    save_locked();
    return true;
}

Model Catalog::update(const std::string& id, const std::function<void(Model&)>& change) {
    std::lock_guard lock(mutex_);
    auto* model = find_locked(id);
    if (!model) throw cymcp::Error("NotFound", "No model has this ID");
    change(*model);
    save_locked();
    return *model;
}
} // namespace modelhub
