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
    // Members of a complete model are not all weights: configuration, tokenizer, manifest...
    bool any_extension = false;
    // Compute the SHA-256 even when nothing announces one, so that it can be recorded.
    bool always_hash = false;
    std::uint64_t size = 0; // expected size, 0 when unknown
    // Position of this file inside a job that downloads several.
    std::uint64_t progress_base = 0, progress_total = 0;
    // Called once the server said what the file is, before any data is fetched. Return true
    // when this content is already installed: nothing is downloaded then.
    std::function<bool(const std::string& sha256)> known;
};
struct Downloaded {
    fs::path path;
    std::string sha256;    // of the file, when it was computed
    bool verified = false; // the hash matched the expected or the announced one
    bool skipped = false;  // `known` said the content is already installed
    std::uint64_t size = 0;
    std::string commit; // revision the server resolved the request to, when it says so
};
Downloaded download_file(const Download& request, const Job& job);
// Name the file will have on disk. Throws InvalidName.
std::string download_name(const Download& request);
std::string huggingface_url(const std::string& repository, const std::string& file, const std::string& revision);
// An address without its query, fragment or user part: what may be stored and shown.
std::string public_url(const std::string& url);

// Download the missing files of a complete model into `folder` and check every file. Files
// already there are kept when they match the contract; a file that does not match is never
// overwritten. `address` gives the address of one member. Returns the checked package; the
// model is usable only when its validation state is "ready".
struct PackageDownload {
    Json package;
    fs::path folder;
    std::string token;
    std::function<std::string(const std::string& relative)> address;
    // The folder is a staging area nobody else uses: a member that fails its check is removed
    // from it, so that the next attempt downloads it again. Never set for a published model.
    bool staging = false;
};
struct PackageDownloaded {
    Json package, validation;
    std::uint64_t size = 0;
    std::uint32_t files = 0;
    std::int64_t modified = 0;
    std::string commit;
};
PackageDownloaded download_package(const PackageDownload& request, const Job& job);
} // namespace modelhub
