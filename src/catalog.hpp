// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model.hpp"

namespace modelhub {
// The persistent list of installed models and of the places where they are looked for.
// Thread-safe; every change is written to disk before the call returns.
class Catalog {
    mutable std::mutex mutex_;
    fs::path file_;
    std::vector<Model> models_;
    std::vector<Root> user_roots_;
    std::vector<std::string> disabled_; // built-in locations switched off by the user
    std::uint64_t revision_ = 0, counter_ = 0;
    int detector_ = 0; // version of the format detection that produced the entries

    void save_locked();
    std::string new_id_locked(const fs::path& path);
    Model* find_locked(const std::string& id);

  public:
    struct MergeResult {
        std::size_t found = 0, added = 0, missing = 0;
    };

    // Raise when detection improves: entries of an older catalogue are then read again once.
    static constexpr int detector_version = 1;

    explicit Catalog(fs::path file);
    bool detector_current() const;
    void mark_detector_current();

    // Grows with every change, so a client can tell cheaply whether to reload.
    std::uint64_t revision() const;
    std::vector<Model> models() const;
    std::optional<Model> find(const std::string& id) const;

    std::vector<Root> roots() const;
    Root add_root(const fs::path& path, std::string label, bool recursive);
    // Removing or disabling a location also forgets the models that were found in it.
    bool remove_root(const std::string& id);
    bool enable_root(const std::string& id, bool enabled);

    // Replace what is known about one location with a fresh scan. User edits are kept, a file
    // that moved keeps its entry, and a file that vanished is marked missing.
    MergeResult merge_scan(const Root& root, std::vector<Model> found);
    // Re-check the entries that were added by hand.
    void refresh_manual();
    Model add(Model model);
    bool remove(const std::string& id);
    Model update(const std::string& id, const std::function<void(Model&)>& change);
};
} // namespace modelhub
