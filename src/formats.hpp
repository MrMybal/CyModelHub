// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model.hpp"
#include <map>

namespace modelhub {
// What a file or folder says about itself. Detection is best effort: headers and tensor names
// come first, folder and file names fill what is still unknown, and the user can correct it.
struct Probe {
    std::string format, type, modality, architecture, quantization, classified_by;
    Json metadata = Json::object();
    bool unsafe = false; // pickle-based: loading it can execute code
    std::string name;    // display name found in the files, when there is one
    // A folder that is a complete model announces its files (see package.hpp).
    Json package;
    // What reading the structure already established, e.g. a damaged or lonely container.
    Json validation;
};

// A .fermion container (Fermion Research speech models): an unsigned 64-bit little-endian
// length, a JSON index {"format", "index": [{"n", "k", "shape", "b"}]}, then the records one
// after the other, with nothing behind the last one. There is no magic number and no checksum
// inside; the hash lives in packed_manifest.json beside the container.
inline constexpr const char* fermion_parakeet_format = "fermion-five-value-parakeet-v1";
struct FermionContainer {
    bool valid = false;
    std::string format, problem; // problem: why the structure is refused
    std::uint64_t header_bytes = 0, payload_bytes = 0, file_bytes = 0;
    std::size_t records = 0;
    std::map<std::string, std::size_t> kinds; // records per kind: fp16, int6, five_value...
};
// Reads the index only, never a record: a few kilobytes whatever the size of the model.
FermionContainer inspect_fermion(const fs::path& path);
// What is wrong in the structure of the files of a model, beyond sizes and hashes, as
// [{path, problem: "structure", detail}]. Formats that are not understood report nothing.
Json structure_problems(const fs::path& root, const Json& package);

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
