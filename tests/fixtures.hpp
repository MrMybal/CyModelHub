// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// Tiny synthetic model files: real headers, no weights.

#pragma once

#include "platform.hpp"

#include <cymcp/core.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

namespace fixtures {
namespace fs = std::filesystem;
using modelhub::Json;

inline int failures = 0;

inline void check(bool condition, const std::string& label) {
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << label << '\n';
}

inline void set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// A fresh folder, with every well-known model store redirected inside it so a test never
// reads the models of the person running it.
inline fs::path sandbox(const std::string& name) {
    const auto root = fs::temp_directory_path() / ("cymodelhub-" + name + "-" + std::to_string(modelhub::process_id()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / "home");
    set_environment("CYMODELHUB_DATA", modelhub::to_utf8(root / "data"));
    set_environment("HF_HUB_CACHE", modelhub::to_utf8(root / "hf" / "hub"));
    set_environment("OLLAMA_MODELS", modelhub::to_utf8(root / "ollama"));
    set_environment("USERPROFILE", modelhub::to_utf8(root / "home"));
    set_environment("HOME", modelhub::to_utf8(root / "home"));
    set_environment("HF_TOKEN", "");
    return root;
}

inline void write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
}

template <class T> void put(std::string& out, T value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
inline void put_string(std::string& out, const std::string& value) {
    put<std::uint64_t>(out, value.size());
    out += value;
}
inline void gguf_text(std::string& out, const std::string& key, const std::string& value) {
    put_string(out, key);
    put<std::uint32_t>(out, 8);
    put_string(out, value);
}
inline void gguf_u32(std::string& out, const std::string& key, std::uint32_t value) {
    put_string(out, key);
    put<std::uint32_t>(out, 4);
    put<std::uint32_t>(out, value);
}

// A chat LLM header: llama architecture, Q4_K_M, a token array to skip and a long chat template.
inline std::string gguf_llm(const std::string& name, char filler = 'x') {
    std::string out = "GGUF";
    put<std::uint32_t>(out, 3);
    put<std::uint64_t>(out, 0);
    put<std::uint64_t>(out, 7);
    gguf_text(out, "general.architecture", "llama");
    gguf_text(out, "general.name", name);
    gguf_u32(out, "general.file_type", 15);
    gguf_u32(out, "llama.context_length", 8192);
    gguf_text(out, "tokenizer.ggml.model", "gpt2");
    put_string(out, "tokenizer.ggml.tokens");
    put<std::uint32_t>(out, 9);
    put<std::uint32_t>(out, 8);
    put<std::uint64_t>(out, 300);
    for (int i = 0; i < 300; ++i) put_string(out, "token" + std::to_string(i));
    gguf_text(out, "tokenizer.chat_template", std::string(5000, 't'));
    out.append(4096, filler);
    return out;
}

inline std::string safetensors(const Json& header, char filler = 's') {
    const auto text = header.dump();
    std::string out;
    put<std::uint64_t>(out, text.size());
    out += text;
    out.append(4096, filler);
    return out;
}

inline Json tensor() { return {{"dtype", "F16"}, {"shape", {1, 1}}, {"data_offsets", {0, 2}}}; }

inline std::string lora_sdxl() {
    Json header = Json::object();
    header["__metadata__"] = {{"ss_base_model_version", "sdxl_base_v1-0"}, {"ss_network_module", "networks.lora"}};
    header["lora_unet_down_blocks_1.lora_up.weight"] = tensor();
    header["lora_unet_down_blocks_1.lora_down.weight"] = tensor();
    header["lora_te2_text_model.lora_up.weight"] = tensor();
    return safetensors(header, 'l');
}

inline std::string checkpoint_sdxl() {
    Json header = Json::object();
    header["model.diffusion_model.input_blocks.0.0.weight"] = tensor();
    header["model.diffusion_model.label_emb.0.0.weight"] = tensor();
    header["conditioner.embedders.1.model.ln_final.weight"] = tensor();
    header["first_stage_model.decoder.up.0.block.0.conv1.weight"] = tensor();
    return safetensors(header, 'c');
}

inline std::string vae() {
    Json header = Json::object();
    header["decoder.up.0.block.0.conv1.weight"] = tensor();
    header["encoder.down.0.block.0.conv1.weight"] = tensor();
    header["quant_conv.weight"] = tensor();
    return safetensors(header, 'v');
}

// A .fermion container with the layout of the real ones: an 8-byte length, a JSON index, then
// the records. The default index holds one record of each kind, with lengths their shapes allow.
inline Json fermion_index() {
    return Json::array({{{"n", "encoder.layers.0.linear"}, {"k", "five_value"}, {"shape", {2, 5}}, {"b", 11}},
                        {{"n", "decoder.embedding"}, {"k", "int6"}, {"shape", {2, 4}}, {"b", 10}},
                        {{"n", "encoder.table"}, {"k", "fp16"}, {"shape", {32, 32}}, {"b", 2048}},
                        {{"n", "joint.head.bias"}, {"k", "fp16"}, {"shape", {4}}, {"b", 8}}});
}
inline std::string fermion_container(const Json& index = fermion_index(),
                                     const std::string& format = "fermion-five-value-parakeet-v1") {
    const auto header = Json{{"format", format}, {"index", index}}.dump();
    std::string out;
    put<std::uint64_t>(out, header.size());
    out += header;
    for (const auto& record : index) out.append(record.at("b").get<std::size_t>(), 'w');
    return out;
}

// The three files of a Fermion speech model. The patches replace fields of the manifest and
// of the "fermion" block of the configuration, to build folders that contradict themselves.
inline void fermion_folder(const fs::path& folder, const std::string& container, const std::string& name,
                           const Json& manifest_patch = Json::object(),
                           const Json& config_patch = Json::object()) {
    const auto digest = cymcp::sha256(container);
    Json manifest = {{"format", "fermion-five-value-parakeet-v1"},
                     {"status", "PASS"},
                     {"container", "model.fermion"},
                     {"container_sha256", digest},
                     {"container_bytes", container.size()},
                     {"five_value_modules", 1},
                     {"dense_tables", "int6"},
                     {"base_model", "nvidia/parakeet-tdt-0.6b-v3"},
                     {"license", "cc-by-4.0"}};
    manifest.update(manifest_patch);
    Json block = {{"model", name},
                  {"container", "model.fermion"},
                  {"container_format", "fermion-five-value-parakeet-v1"},
                  {"container_sha256", digest},
                  {"container_bytes", container.size()}};
    block.update(config_patch);
    const Json config = {{"sample_rate", 16000},
                         {"model_type", "parakeet"},
                         {"joint", {{"vocabulary", {"a", "b"}}}},
                         {"fermion", block}};
    write(folder / "model.fermion", container);
    write(folder / "config.json", config.dump(1));
    write(folder / "packed_manifest.json", manifest.dump(1));
}
} // namespace fixtures
