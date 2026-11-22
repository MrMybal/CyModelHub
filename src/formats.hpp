// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model.hpp"

namespace modelhub {
// What a file or folder says about itself. Detection is best effort: headers and tensor names
// come first, folder and file names fill what is still unknown, and the user can correct it.
struct Probe {
    std::string format, type, modality, architecture, quantization, classified_by;
    Json metadata = Json::object();
    bool unsafe = false; // pickle-based: loading it can execute code
};

// True when the file name looks like model weights worth cataloguing.
bool model_extension(const fs::path& path);
// Smallest size for which a file with this extension is treated as a model.
std::uint64_t minimum_model_size(const fs::path& path);
// Reads only headers: never loads weights, never unpickles.
Probe probe_file(const fs::path& path);
// A model stored as a directory (Diffusers pipeline, Transformers or MLX checkpoint).
std::optional<Probe> probe_folder(const fs::path& path);
// Fill unknown fields from the folder names, the file name and an optional extra name such as
// the repository the file came from.
void apply_path_hints(Probe& probe, const fs::path& path, std::string_view extra = {});
// Fast content identity: size plus the first and last blocks. Copies share it.
std::string file_fingerprint(const fs::path& path, std::uint64_t size);
} // namespace modelhub
