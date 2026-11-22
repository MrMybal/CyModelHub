// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model.hpp"

namespace modelhub {
struct ScanContext {
    cymcp::Cancellation cancellation;
    // Called from the scanning thread with the number of models found so far.
    std::function<void(std::size_t found, const fs::path& current)> progress;
    // Previous catalogue entry for a path. An unchanged file is reused without reading it again.
    std::function<const Model*(const fs::path&)> known;
    // Extra name that helps classification, e.g. the repository a snapshot belongs to.
    std::string hint;
};

// Model stores of well-known tools that exist on this computer, plus CyModelHub's own store.
std::vector<Root> known_roots();
// Default destination for models installed by CyModelHub.
fs::path managed_store();
std::vector<Model> scan_root(const Root& root, const ScanContext& context);
// One file or one model folder, for manual additions. nullopt when nothing usable is there.
std::optional<Model> describe_path(const fs::path& path, const ScanContext& context = {});
} // namespace modelhub
