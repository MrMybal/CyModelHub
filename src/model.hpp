// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "platform.hpp"

namespace modelhub {
// Classification vocabulary. Values are open strings so a user or a newer scanner can add more.
//   type:     the role of the file      model, lora, vae, controlnet, text_encoder, vision_encoder,
//                                       embedding, upscaler, adapter, projector, other
//   modality: what it works on          text, image, video, audio, embedding, multimodal, 3d, other
//   format:   how it is stored          gguf, safetensors, pickle, onnx, diffusers, transformers, ...
struct Model {
    std::string id, name, format, type = "model", modality = "other", architecture, quantization;
    fs::path path;
    bool folder = false, missing = false, unsafe = false, favorite = false;
    std::uint64_t size = 0;
    std::uint32_t files = 1;
    std::int64_t modified = 0, added = 0;
    // fingerprint: fast content identity used to spot copies. sha256: full hash, computed on request.
    std::string fingerprint, sha256, source, root, repo, url, classified_by, notes;
    std::vector<std::string> tags;
    // Fields edited by the user. A scan never overwrites them.
    std::vector<std::string> locked;
    Json metadata = Json::object();

    Json to_json(bool details) const;
    static Model from_json(const Json& value);
    bool is_locked(std::string_view field) const;
};

struct Filter {
    std::string query, type, modality, format, architecture, source, root, tag;
    std::optional<bool> favorite, missing;
};

// Comparable form of a path: normalised, and case-insensitive where the file system is.
std::string path_key(const fs::path& path);
// True when `child` is `parent` or lies inside it.
bool path_within(const fs::path& child, const fs::path& parent);
bool matches(const Model& model, const Filter& filter);
Filter filter_from_json(const Json& arguments);

// Rank installed models against what an application asks for: an ID, a SHA-256, a repository,
// a file name or a display name. Higher is better; 0 means no match.
int resolve_score(const Model& model, std::string_view wanted);

struct Root {
    std::string id, label, kind = "folder"; // folder, huggingface, ollama, lmstudio, managed
    fs::path path;
    bool enabled = true, recursive = true, builtin = false;

    Json to_json() const;
};
} // namespace modelhub
