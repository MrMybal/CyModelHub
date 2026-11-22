// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "jobs.hpp"
#include "model.hpp"

namespace modelhub {
// Move a model file or folder into `directory`. Works across drives (copy, check, then remove
// the original). With leave_link, a link at the old place keeps other tools working; `warning`
// explains why when that link could not be created.
fs::path move_model(const Model& model, const fs::path& directory, bool leave_link, const Job& job,
                    std::string& warning);
// Make the model visible in another folder without copying it. mode: "hard" or "symbolic".
fs::path link_model(const Model& model, const fs::path& directory, const std::string& mode);
// Download one file over HTTPS into `directory`, resuming a previous attempt when possible.
struct Download {
    std::string url, filename, sha256, token;
    fs::path directory;
};
struct Downloaded {
    fs::path path;
    std::string sha256; // set when the content was verified
};
Downloaded download_file(const Download& request, const Job& job);
std::string huggingface_url(const std::string& repository, const std::string& file, const std::string& revision);
} // namespace modelhub
