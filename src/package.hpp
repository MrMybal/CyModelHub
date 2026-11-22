// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "jobs.hpp"
#include "model.hpp"

namespace modelhub {
// A complete model: a folder whose files are all needed to run it (weights, shards, index,
// configuration, tokenizer, manifest). The contract an application sends is
//
//   { "identity":       { "name", "repository", "revision", "variant" },
//     "provenance":     { "source", "url", "application" },
//     "classification": { "format", "type", "modality", "architecture", "quantization" },
//     "entry":          "path of the file the engine opens, empty when it takes the folder",
//     "files":          [ { "path", "size", "sha256", "role", "required" } ],
//     "dependencies":   [ { "name", "repository", "revision", "variant", "sha256", "required" } ] }
//
// Paths are relative to the root of the model, with forward slashes. After a check each file
// also carries its state (ok, missing, absent, size, hash), where its size and hash come from
// (manifest or measured) and when its hash was last checked.

// True for a plain relative path that cannot leave its root: no drive, no "..", no reserved
// character or device name.
bool safe_relative_path(std::string_view path);
// Checks and completes a contract. Throws InvalidManifest.
Json normalize_package(const Json& manifest);
// Location of a member of a package. Throws UnsafePath. With `writing`, the folder that will
// receive the file must really lie inside the root (no link leading elsewhere).
fs::path package_file(const fs::path& root, const std::string& relative, bool writing = false);

struct Verified {
    Json package;    // the contract, with the state of each file
    Json validation; // state, integrity, level, checked, problems
    std::uint64_t size = 0;
    std::uint32_t files = 0;
    std::int64_t modified = 0;
};
// Compare the files of a package with the contract. With `hashes` every file is read once to
// compute its SHA-256; otherwise only presence and sizes are looked at. Weights are never
// loaded or interpreted.
Verified verify_package(const fs::path& root, Json package, bool hashes, const Job* job,
                        const cymcp::Cancellation& cancellation);
// The same for a model made of one file. Updates model.sha256 and model.validation.
void verify_file(Model& model, bool hashes, const cymcp::Cancellation& cancellation);

// What the disk says now, without reading any content: files that vanished or changed size
// since the last check, and whether the hashes checked then still describe the files.
struct Presence {
    std::string state = "present", integrity = "none";
    Json problems = Json::array();
};
Presence presence(const Model& model);
// Bring model.validation in line with presence(), keeping what a full check established.
void refresh_validation(Model& model);
} // namespace modelhub
