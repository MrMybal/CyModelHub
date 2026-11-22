// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "formats.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace modelhub {
namespace {
bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }
bool starts(std::string_view text, std::string_view part) { return text.substr(0, part.size()) == part; }
bool ends(std::string_view text, std::string_view part) {
    return text.size() >= part.size() && text.substr(text.size() - part.size()) == part;
}
void set_once(std::string& field, std::string_view value) {
    if (field.empty() && !value.empty()) field = value;
}

std::string extension(const fs::path& path) { return lower(to_utf8(path.extension())); }

// Sequential reader with cheap forward skips, bounded so a damaged header cannot stall a scan.
class Reader {
    std::ifstream in_;
    std::vector<char> buffer_;
    std::size_t position_ = 0, length_ = 0;
    std::uint64_t consumed_ = 0, limit_;

    bool fill() {
        if (consumed_ >= limit_ || !in_) return false;
        in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        length_ = static_cast<std::size_t>(in_.gcount());
        position_ = 0;
        consumed_ += length_;
        return length_ > 0;
    }

  public:
    Reader(const fs::path& path, std::uint64_t limit)
        : in_(path, std::ios::binary), buffer_(256 * 1024), limit_(limit) {}

    bool read(void* destination, std::size_t size) {
        auto* out = static_cast<char*>(destination);
        while (size) {
            if (position_ == length_ && !fill()) return false;
            const auto take = std::min(size, length_ - position_);
            std::memcpy(out, buffer_.data() + position_, take);
            position_ += take;
            out += take;
            size -= take;
        }
        return true;
    }

    bool skip(std::uint64_t size) {
        const std::uint64_t available = length_ - position_;
        if (size <= available) {
            position_ += static_cast<std::size_t>(size);
            return true;
        }
        size -= available;
        position_ = length_ = 0;
        if (size > (std::uint64_t{1} << 40)) return false;
        in_.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        return static_cast<bool>(in_);
    }
};

// Model files are little-endian, like every platform CyModelHub targets.
template <class T> bool get(Reader& reader, T& value) { return reader.read(&value, sizeof(value)); }

bool get_string(Reader& reader, std::string& value, std::size_t keep) {
    std::uint64_t size = 0;
    value.clear();
    if (!get(reader, size) || size > (std::uint64_t{1} << 30)) return false;
    if (size > keep) return reader.skip(size);
    value.resize(static_cast<std::size_t>(size));
    return reader.read(value.data(), value.size());
}

constexpr std::array<std::uint8_t, 13> gguf_sizes{1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};

// Reads one GGUF value. Scalars and short strings are returned; arrays and long strings are skipped.
bool gguf_value(Reader& reader, std::uint32_t type, Json& out) {
    out = nullptr;
    switch (type) {
    case 0: { std::uint8_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 1: { std::int8_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 2: { std::uint16_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 3: { std::int16_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 4: { std::uint32_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 5: { std::int32_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 6: { float v; if (!get(reader, v)) return false; out = v; return true; }
    case 7: { std::uint8_t v; if (!get(reader, v)) return false; out = v != 0; return true; }
    case 8: {
        std::string v;
        if (!get_string(reader, v, 512)) return false;
        if (!v.empty()) out = v;
        return true;
    }
    case 9: {
        std::uint32_t element = 0;
        std::uint64_t count = 0;
        if (!get(reader, element) || !get(reader, count) || count > (std::uint64_t{1} << 32)) return false;
        if (element == 8) {
            std::string ignored;
            for (std::uint64_t i = 0; i < count; ++i)
                if (!get_string(reader, ignored, 0)) return false;
            return true;
        }
        if (element >= gguf_sizes.size() || !gguf_sizes[element]) return false; // nested arrays
        return reader.skip(count * gguf_sizes[element]);
    }
    case 10: { std::uint64_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 11: { std::int64_t v; if (!get(reader, v)) return false; out = v; return true; }
    case 12: { double v; if (!get(reader, v)) return false; out = v; return true; }
    default: return false;
    }
}

// llama.cpp general.file_type values.
const char* gguf_quantization(std::int64_t file_type) {
    static constexpr const char* names[] = {
        "F32",    "F16",    "Q4_0",   "Q4_1",    "",        "",       "",       "Q8_0",   "Q5_0",
        "Q5_1",   "Q2_K",   "Q3_K_S", "Q3_K_M",  "Q3_K_L",  "Q4_K_S", "Q4_K_M", "Q5_K_S", "Q5_K_M",
        "Q6_K",   "IQ2_XXS", "IQ2_XS", "Q2_K_S", "IQ3_XS",  "IQ3_XXS", "IQ1_S", "IQ4_NL", "IQ3_S",
        "IQ3_M",  "IQ2_S",  "IQ2_M",  "IQ4_XS",  "IQ1_M",   "BF16"};
    return file_type >= 0 && file_type < static_cast<std::int64_t>(std::size(names)) ? names[file_type] : "";
}

// Quantization or precision written in a file name, e.g. "Q4_K_M", "fp16".
std::string name_quantization(const std::string& lowered) {
    static constexpr const char* tokens[] = {
        "iq1_s", "iq1_m", "iq2_xxs", "iq2_xs", "iq2_s", "iq2_m", "iq3_xxs", "iq3_xs", "iq3_s", "iq3_m",
        "iq4_xs", "iq4_nl", "q2_k_s", "q2_k", "q3_k_s", "q3_k_m", "q3_k_l", "q4_k_s", "q4_k_m", "q5_k_s",
        "q5_k_m", "q6_k", "q8_0", "q5_0", "q5_1", "q4_0", "q4_1", "bf16", "fp16", "fp32", "fp8", "f16",
        "f32", "int8", "8bit", "4bit"};
    for (const auto* token : tokens) {
        const auto found = lowered.find(token);
        if (found == std::string::npos) continue;
        const auto end = found + std::strlen(token);
        const bool left = found == 0 || !std::isalnum(static_cast<unsigned char>(lowered[found - 1]));
        const bool right = end == lowered.size() || !std::isalnum(static_cast<unsigned char>(lowered[end]));
        if (!left || !right) continue;
        std::string result(token);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return result;
    }
    return {};
}

// Map the many spellings of a generation model family to one short key. `known` tells whether
// the name was recognised; otherwise the lowered input is returned unchanged.
std::string normalize_architecture(const std::string& raw, bool* known = nullptr) {
    auto value = lower(raw);
    if (known) *known = true;
    if (const auto slash = value.find('/'); slash != std::string::npos) value.resize(slash);
    static constexpr std::pair<const char*, const char*> families[] = {
        {"flux", "flux"},          {"stable-diffusion-3", "sd3"}, {"sd3", "sd3"},
        {"sdxl", "sdxl"},          {"stable-diffusion-xl", "sdxl"}, {"sd_xl", "sdxl"},
        {"stable-diffusion-v1", "sd15"}, {"sd_v1", "sd15"},     {"sd15", "sd15"},
        {"sd1", "sd15"},           {"stable-diffusion-v2", "sd2"}, {"sd_v2", "sd2"},
        {"sd2", "sd2"},            {"hunyuan", "hunyuan-video"}, {"hyvid", "hunyuan-video"},
        {"ltx", "ltx-video"},      {"cogvideo", "cogvideo"},    {"mochi", "mochi"},
        {"animatediff", "animatediff"}, {"wan", "wan"},         {"cascade", "stable-cascade"},
        {"pixart", "pixart"},      {"aura", "auraflow"},        {"lumina", "lumina"},
        {"kolors", "kolors"},      {"hidream", "hidream"},      {"chroma", "chroma"},
        {"qwen_image", "qwen-image"}, {"qwen-image", "qwen-image"}, {"cosmos", "cosmos"},
        {"stable-audio", "stable-audio"}, {"stable_audio", "stable-audio"}};
    for (const auto& [needle, family] : families)
        if (contains(value, needle)) return family;
    if (known) *known = false;
    return value;
}

std::string architecture_modality(const std::string& architecture) {
    static constexpr const char* video[] = {"wan", "hunyuan-video", "ltx-video", "cogvideo", "mochi",
                                            "animatediff", "svd", "cosmos"};
    for (const auto* name : video)
        if (architecture == name) return "video";
    return architecture == "stable-audio" ? "audio" : "image";
}

bool probe_gguf(const fs::path& path, Probe& probe) {
    Reader reader(path, std::uint64_t{256} << 20);
    char magic[4]{};
    std::uint32_t version = 0;
    std::uint64_t tensors = 0, count = 0;
    if (!reader.read(magic, 4) || std::memcmp(magic, "GGUF", 4) != 0) return false;
    probe.format = "gguf";
    probe.classified_by = "header";
    if (!get(reader, version) || version < 2 || version > 99 || !get(reader, tensors) || !get(reader, count) ||
        count > 65536)
        return true;
    auto& meta = probe.metadata;
    meta["ggufVersion"] = version;
    meta["tensors"] = tensors;
    bool tokenizer = false, chat = false, pooling = false, vision = false;
    std::string architecture, general_type, adapter;
    std::int64_t file_type = -1;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string key;
        std::uint32_t type = 0;
        Json value;
        if (!get_string(reader, key, 256) || !get(reader, type) || !gguf_value(reader, type, value)) break;
        if (key == "tokenizer.ggml.model") tokenizer = true;
        else if (key == "tokenizer.chat_template") chat = true;
        else if (starts(key, "clip.vision.") || (key == "clip.has_vision_encoder" && value == true)) vision = true;
        if (value.is_null()) continue;
        if (key == "general.architecture" && value.is_string()) architecture = lower(value.get<std::string>());
        else if (key == "general.type" && value.is_string()) general_type = lower(value.get<std::string>());
        else if (key == "adapter.type" && value.is_string()) adapter = lower(value.get<std::string>());
        else if (key == "general.file_type" && value.is_number_integer()) file_type = value.get<std::int64_t>();
        if (ends(key, ".pooling_type")) pooling = true;
        if (starts(key, "general.") || key == "adapter.type" || ends(key, ".context_length") ||
            ends(key, ".embedding_length") || ends(key, ".block_count") || ends(key, ".pooling_type"))
            meta[key] = value;
    }
    meta["chatTemplate"] = chat;
    probe.quantization = gguf_quantization(file_type);

    if (general_type == "adapter" || adapter == "lora") {
        probe.type = "lora";
        probe.modality = "text";
        probe.architecture = architecture;
    } else if (general_type == "projector" || general_type == "mmproj" || (architecture == "clip" && vision)) {
        probe.type = "projector";
        probe.modality = "multimodal";
        probe.architecture = architecture;
    } else if (!tokenizer && !chat && (architecture == "t5" || architecture == "t5encoder" ||
                                       architecture == "umt5" || architecture == "clip")) {
        // Text encoders packaged for diffusion pipelines.
        probe.type = "text_encoder";
        probe.modality = "image";
        probe.architecture = architecture;
    } else if (contains(architecture, "bert") || pooling) {
        probe.type = "model";
        probe.modality = "embedding";
        probe.architecture = architecture;
    } else if (tokenizer || chat) {
        probe.type = "model";
        probe.modality = "text";
        probe.architecture = architecture;
    } else if (!architecture.empty()) {
        // Diffusion weights converted to GGUF carry the family as their architecture.
        bool known = false;
        const auto family = normalize_architecture(architecture, &known);
        if (known) {
            probe.type = "model";
            probe.architecture = family;
            probe.modality = architecture_modality(family);
        } else {
            probe.architecture = architecture;
        }
    }
    return true;
}

struct TensorNames {
    bool lora = false, lora_te2 = false, lora_transformer = false, unet = false, label_emb = false;
    bool cond_xl = false, cond_sd1 = false, cond_sd2 = false, flux = false, sd3 = false, wan_patch = false;
    bool wan_cross = false, hunyuan = false, ltx = false, motion = false, vae_decoder = false;
    bool non_vae = false, controlnet = false, clip_text = false, t5 = false, clip_vision = false;
    bool textual_inversion = false;
    std::size_t count = 0;

    void add(std::string_view key) {
        ++count;
        if (contains(key, "lora_up") || contains(key, "lora_down") || contains(key, "lora_A") ||
            contains(key, "lora_B") || contains(key, "lokr_") || contains(key, "hada_") ||
            contains(key, ".lora.")) {
            lora = true;
            if (contains(key, "lora_te2")) lora_te2 = true;
            if (contains(key, "lora_transformer") || contains(key, "double_blocks") ||
                contains(key, "single_blocks"))
                lora_transformer = true;
        }
        if (contains(key, "model.diffusion_model.")) unet = true;
        if (contains(key, "label_emb.")) label_emb = true;
        if (contains(key, "conditioner.embedders.1.")) cond_xl = true;
        if (contains(key, "cond_stage_model.transformer.")) cond_sd1 = true;
        if (contains(key, "cond_stage_model.model.")) cond_sd2 = true;
        if (contains(key, "double_blocks.") || contains(key, "single_blocks.")) flux = true;
        if (contains(key, "joint_blocks.")) sd3 = true;
        if (contains(key, "patch_embedding.weight")) wan_patch = true;
        if (contains(key, ".cross_attn.") && contains(key, "blocks.")) wan_cross = true;
        if (contains(key, "txt_in.individual_token_refiner")) hunyuan = true;
        if (contains(key, "scale_shift_table") && contains(key, "transformer_blocks.")) ltx = true;
        if (contains(key, "motion_modules") || contains(key, "temporal_transformer")) motion = true;
        if (contains(key, "control_model.") || contains(key, "input_hint_block") ||
            contains(key, "controlnet_cond_embedding") || contains(key, "controlnet_blocks"))
            controlnet = true;
        if (contains(key, "text_model.encoder.layers.")) clip_text = true;
        if (contains(key, "encoder.block.")) t5 = true;
        if (contains(key, "vision_model.encoder.layers.")) clip_vision = true;
        if (key == "emb_params" || key == "clip_l" || key == "clip_g" || contains(key, "string_to_param"))
            textual_inversion = true;
        const bool vae_key = starts(key, "decoder.") || starts(key, "quant_conv") ||
                             starts(key, "post_quant_conv") || starts(key, "first_stage_model.") ||
                             starts(key, "vae.") || (starts(key, "encoder.") && !starts(key, "encoder.block."));
        if (!vae_key) non_vae = true;
        if (contains(key, "decoder.up") || contains(key, "decoder.mid") || contains(key, "decoder.conv_in"))
            vae_decoder = true;
    }
};

bool probe_safetensors(const fs::path& path, Probe& probe) {
    std::ifstream file(path, std::ios::binary);
    std::uint64_t size = 0;
    char first = 0;
    if (!file.read(reinterpret_cast<char*>(&size), sizeof(size)) || size < 2 ||
        size > (std::uint64_t{512} << 20) || !file.get(first) || first != '{')
        return false;
    probe.format = "safetensors";
    if (size > (std::uint64_t{48} << 20)) return true; // too large to inspect cheaply
    std::string header(static_cast<std::size_t>(size), '{');
    file.read(header.data() + 1, static_cast<std::streamsize>(size - 1));
    if (static_cast<std::uint64_t>(file.gcount()) != size - 1) return true;
    const auto json = Json::parse(header, nullptr, false);
    if (!json.is_object()) return true;

    TensorNames names;
    std::string declared; // family announced by the training metadata
    std::string suffix;
    for (auto item = json.begin(); item != json.end(); ++item) {
        if (item.key() != "__metadata__") {
            names.add(item.key());
            continue;
        }
        if (!item->is_object()) continue;
        static constexpr const char* kept[] = {
            "modelspec.architecture", "modelspec.title", "modelspec.author", "modelspec.license",
            "modelspec.description",  "ss_base_model_version", "ss_sd_model_name", "ss_network_module",
            "ss_network_dim",         "ss_network_alpha",  "ss_output_name",  "ss_resolution",
            "format"};
        for (const auto* key : kept)
            if (const auto found = item->find(key);
                found != item->end() && found->is_string() && found->get_ref<const std::string&>().size() <= 512)
                probe.metadata[key] = *found;
        if (const auto spec = item->find("modelspec.architecture"); spec != item->end() && spec->is_string()) {
            declared = spec->get<std::string>();
            if (const auto slash = declared.find('/'); slash != std::string::npos)
                suffix = lower(declared.substr(slash + 1));
        } else if (const auto base = item->find("ss_base_model_version"); base != item->end() && base->is_string()) {
            declared = base->get<std::string>();
        }
    }
    probe.metadata["tensors"] = names.count;
    const auto family = declared.empty() ? std::string() : normalize_architecture(declared);
    probe.classified_by = "tensors";

    if (names.lora || contains(suffix, "lora")) {
        probe.type = "lora";
        probe.architecture = !family.empty() ? family
                             : names.lora_transformer ? "flux"
                             : names.lora_te2 ? "sdxl"
                             : "";
    } else if (names.controlnet || contains(suffix, "control")) {
        probe.type = "controlnet";
        probe.architecture = family;
    } else if (names.hunyuan) {
        probe.type = "model";
        probe.architecture = "hunyuan-video";
    } else if (names.wan_patch && names.wan_cross) {
        probe.type = "model";
        probe.architecture = "wan";
    } else if (names.motion && !names.cond_sd1 && !names.cond_xl) {
        probe.type = "adapter";
        probe.architecture = "animatediff";
    } else if (names.flux) {
        probe.type = "model";
        probe.architecture = !family.empty() ? family : "flux";
    } else if (names.sd3) {
        probe.type = "model";
        probe.architecture = "sd3";
    } else if (names.ltx) {
        probe.type = "model";
        probe.architecture = "ltx-video";
    } else if (names.unet) {
        probe.type = "model";
        probe.architecture = !family.empty() ? family
                             : names.cond_xl || names.label_emb ? "sdxl"
                             : names.cond_sd1 ? "sd15"
                             : names.cond_sd2 ? "sd2"
                             : "";
    } else if (names.count && !names.non_vae && names.vae_decoder) {
        probe.type = "vae";
        probe.architecture = family;
    } else if (names.clip_vision && !names.clip_text) {
        probe.type = "vision_encoder";
    } else if (names.clip_text || names.t5) {
        probe.type = "text_encoder";
        probe.architecture = names.t5 ? "t5" : "clip";
    } else if (names.textual_inversion && names.count <= 8) {
        probe.type = "embedding";
        probe.architecture = family;
    } else {
        probe.classified_by.clear();
        probe.architecture = family;
        return true;
    }
    if (!probe.architecture.empty() && probe.type != "text_encoder")
        probe.modality = architecture_modality(probe.architecture);
    else if (probe.type != "model")
        probe.modality = "image";
    return true;
}

Json read_json(const fs::path& path) {
    const auto content = read_file(path, 4 * 1024 * 1024);
    return content ? Json::parse(*content, nullptr, false) : Json();
}

struct NameHint {
    const char* word;
    const char* type;
    const char* modality;
    const char* architecture;
};

// Folder names used by the common generation front ends.
constexpr NameHint folder_hints[] = {
    {"loras", "lora", "image", ""},           {"lora", "lora", "image", ""},
    {"lycoris", "lora", "image", ""},         {"checkpoints", "model", "image", ""},
    {"stable-diffusion", "model", "image", ""}, {"diffusion_models", "model", "image", ""},
    {"unet", "model", "image", ""},           {"vae", "vae", "image", ""},
    {"vae_approx", "vae", "image", ""},       {"controlnet", "controlnet", "image", ""},
    {"t2i_adapter", "controlnet", "image", ""}, {"upscale_models", "upscaler", "image", ""},
    {"esrgan", "upscaler", "image", ""},      {"realesrgan", "upscaler", "image", ""},
    {"swinir", "upscaler", "image", ""},      {"upscalers", "upscaler", "image", ""},
    {"embeddings", "embedding", "image", ""}, {"textual_inversion", "embedding", "image", ""},
    {"clip", "text_encoder", "image", ""},    {"text_encoders", "text_encoder", "image", ""},
    {"clip_vision", "vision_encoder", "image", ""}, {"ipadapter", "adapter", "image", ""},
    {"ip-adapter", "adapter", "image", ""},   {"style_models", "adapter", "image", ""},
    {"animatediff_models", "adapter", "video", "animatediff"}, {"llm", "model", "text", ""},
    {"llms", "model", "text", ""},            {"text-generation", "model", "text", ""},
    {"tts", "model", "audio", ""},            {"stt", "model", "audio", ""},
    {"whisper", "model", "audio", "whisper"}, {"audio", "model", "audio", ""},
    {"audio_checkpoints", "model", "audio", ""}, {"rvc", "model", "audio", "rvc"},
    {"video", "model", "video", ""},          {"video_models", "model", "video", ""}};

// Words found in file names. Earlier entries win.
constexpr NameHint file_hints[] = {
    {"whisper", "", "audio", "whisper"},      {"xtts", "", "audio", "xtts"},
    {"kokoro", "", "audio", "kokoro"},        {"musicgen", "", "audio", "musicgen"},
    {"stable_audio", "", "audio", "stable-audio"}, {"stable-audio", "", "audio", "stable-audio"},
    {"bark", "", "audio", "bark"},            {"encodec", "", "audio", "encodec"},
    {"animatediff", "", "video", "animatediff"}, {"hunyuanvideo", "", "video", "hunyuan-video"},
    {"hunyuan_video", "", "video", "hunyuan-video"}, {"cogvideo", "", "video", "cogvideo"},
    {"mochi", "", "video", "mochi"},          {"ltx-video", "", "video", "ltx-video"},
    {"ltxv", "", "video", "ltx-video"},       {"wan2", "", "video", "wan"},
    {"svd", "", "video", "svd"},              {"controlnet", "controlnet", "image", ""},
    {"control_", "controlnet", "image", ""},  {"esrgan", "upscaler", "image", "esrgan"},
    {"realesr", "upscaler", "image", "esrgan"}, {"swinir", "upscaler", "image", "swinir"},
    {"upscal", "upscaler", "image", ""},      {"t5xxl", "text_encoder", "image", "t5"},
    {"umt5", "text_encoder", "image", "t5"},  {"clip_l", "text_encoder", "image", "clip"},
    {"clip_g", "text_encoder", "image", "clip"}, {"clip_vision", "vision_encoder", "image", "clip"},
    {"mmproj", "projector", "multimodal", ""}, {"lora", "lora", "", ""},
    {"vae", "vae", "image", ""},              {"flux", "", "image", "flux"},
    {"sdxl", "", "image", "sdxl"},            {"sd_xl", "", "image", "sdxl"},
    {"sd3", "", "image", "sd3"},              {"sd15", "", "image", "sd15"},
    {"sd1.5", "", "image", "sd15"},           {"v1-5", "", "image", "sd15"},
    {"embed", "", "embedding", ""},           {"tts", "", "audio", ""}};

void apply(Probe& probe, const NameHint& hint, const char* how) {
    const bool changed = (probe.type.empty() && *hint.type) || (probe.modality.empty() && *hint.modality) ||
                         (probe.architecture.empty() && *hint.architecture);
    set_once(probe.type, hint.type);
    set_once(probe.modality, hint.modality);
    set_once(probe.architecture, hint.architecture);
    if (changed) set_once(probe.classified_by, how);
}

// Diffusers pipelines announce their family in the pipeline class name.
void classify_pipeline(Probe& probe, const std::string& class_name) {
    const auto name = lower(class_name);
    static constexpr std::pair<const char*, const char*> classes[] = {
        {"stablediffusionxl", "sdxl"},   {"stablediffusion3", "sd3"},     {"flux", "flux"},
        {"hunyuanvideo", "hunyuan-video"}, {"wan", "wan"},                {"ltx", "ltx-video"},
        {"cogvideo", "cogvideo"},        {"mochi", "mochi"},              {"animatediff", "animatediff"},
        {"stablevideodiffusion", "svd"}, {"stableaudio", "stable-audio"}, {"audioldm", "audioldm"},
        {"musicldm", "musicldm"},        {"stablecascade", "stable-cascade"}, {"pixart", "pixart"},
        {"auraflow", "auraflow"},        {"lumina", "lumina"},            {"kolors", "kolors"},
        {"hidream", "hidream"},          {"qwenimage", "qwen-image"},     {"stablediffusion", "sd15"}};
    for (const auto& [needle, family] : classes)
        if (contains(name, needle)) {
            probe.architecture = family;
            break;
        }
    probe.modality = contains(name, "audio") || contains(name, "music") ? "audio"
                     : contains(name, "video") ? "video"
                     : probe.architecture.empty() ? "image"
                     : architecture_modality(probe.architecture);
}

// Transformers checkpoints announce a model_type; map the ones that are not text generators.
void classify_transformers(Probe& probe, const Json& config) {
    const auto model_type = lower(config.value("model_type", std::string()));
    std::string head;
    if (const auto found = config.find("architectures");
        found != config.end() && found->is_array() && !found->empty() && found->front().is_string())
        head = lower(found->front().get<std::string>());
    probe.architecture = model_type;
    static constexpr std::pair<const char*, const char*> types[] = {
        {"whisper", "audio"},   {"wav2vec2", "audio"}, {"hubert", "audio"},    {"speecht5", "audio"},
        {"bark", "audio"},      {"vits", "audio"},     {"musicgen", "audio"},  {"encodec", "audio"},
        {"clap", "audio"},      {"seamless", "audio"}, {"moonshine", "audio"}, {"parakeet", "audio"},
        {"llava", "multimodal"}, {"qwen2_vl", "multimodal"}, {"paligemma", "multimodal"},
        {"idefics", "multimodal"}, {"blip", "multimodal"}, {"florence", "multimodal"},
        {"clip", "multimodal"}, {"siglip", "multimodal"}, {"vit", "image"},    {"dinov2", "image"},
        {"convnext", "image"},  {"sam", "image"},      {"detr", "image"},      {"yolos", "image"},
        {"segformer", "image"}, {"depth", "image"},    {"bert", "embedding"},  {"roberta", "embedding"},
        {"mpnet", "embedding"}, {"e5", "embedding"}};
    for (const auto& [needle, modality] : types)
        if (contains(model_type, needle)) {
            probe.modality = modality;
            break;
        }
    if (probe.modality.empty())
        probe.modality = contains(head, "causallm") || contains(head, "lmhead") ||
                                 contains(head, "conditionalgeneration") || contains(head, "seq2seq")
                             ? "text"
                             : "";
    if (probe.modality == "multimodal" && (model_type == "clip" || model_type == "siglip"))
        probe.type = "vision_encoder";
}

// --- Fermion ------------------------------------------------------------------------------

bool multiply(std::uint64_t& total, std::uint64_t factor) {
    if (factor && total > (std::uint64_t{1} << 62) / factor) return false;
    total *= factor;
    return true;
}

// Whether a record of this kind and shape can have this many bytes, according to the reader
// of the format. Returns an empty text when it can.
std::string fermion_record_problem(const std::string& kind, const std::vector<std::uint64_t>& shape,
                                   std::uint64_t bytes) {
    std::uint64_t total = 1;
    for (const auto dimension : shape)
        if (!multiply(total, dimension)) return "a shape is too large";
    if (kind == "fp16") return bytes == total * 2 ? "" : "a fp16 record has the wrong length";
    if (kind == "five_value") {
        if (shape.size() != 2) return "a five_value record is not a matrix";
        // Five ternary digits per byte, one bit per non-zero value, two rows of fp16 scales.
        const auto rows = shape[0], codes = rows * ((shape[1] + 4) / 5), scales = rows * 4;
        const auto least = codes + scales, most = least + (total + 7) / 8;
        return bytes >= least && bytes <= most ? "" : "a five_value record has an impossible length";
    }
    if (kind == "int6" || kind == "int8") {
        if (shape.empty() || shape[0] == 0) return "an integer record has no rows";
        const auto scales = shape[0] * 2;
        if (bytes < scales) return "an integer record is shorter than its scales";
        const auto body = bytes - scales;
        if (kind == "int8") return body >= total ? "" : "an int8 record is too short";
        // Four 6-bit values in every three bytes.
        return body % 3 == 0 && body / 3 * 4 >= total ? "" : "an int6 record has the wrong length";
    }
    return "unknown record kind " + kind.substr(0, 40);
}

Json fermion_manifest(const fs::path& folder) {
    const auto manifest = read_json(folder / "packed_manifest.json");
    return manifest.is_object() &&
                   starts(manifest.value("format", Json()).is_string() ? manifest["format"].get<std::string>()
                                                                       : std::string(),
                          "fermion-")
               ? manifest
               : Json();
}

std::string fermion_container_name(const Json& manifest) {
    const auto found = manifest.find("container");
    const auto name =
        found != manifest.end() && found->is_string() ? found->get<std::string>() : std::string();
    // A plain file name beside the manifest; anything else falls back to the usual name.
    return name.empty() || name.find_first_of("/\\:") != std::string::npos || name == "." || name == ".."
               ? "model.fermion"
               : name;
}

void classify_fermion(Probe& probe, const std::string& format, const std::string& dense_tables) {
    probe.format = "fermion";
    probe.type = "model";
    probe.classified_by = "header";
    probe.metadata["containerFormat"] = format;
    if (format == fermion_parakeet_format) {
        // The only published kind so far: a quantized Parakeet speech recogniser.
        probe.architecture = "parakeet";
        probe.modality = "audio";
        probe.quantization = dense_tables.empty() ? "five-value" : "five-value-" + lower(dense_tables);
    }
}
} // namespace

FermionContainer inspect_fermion(const fs::path& path) {
    constexpr std::uint64_t max_header = std::uint64_t{16} << 20;
    FermionContainer result;
    std::error_code error;
    result.file_bytes = fs::file_size(path, error);
    std::ifstream file(path, std::ios::binary);
    unsigned char prefix[8]{};
    if (error || !file.read(reinterpret_cast<char*>(prefix), sizeof(prefix))) {
        result.problem = "the file is shorter than its length field";
        return result;
    }
    for (int index = 7; index >= 0; --index) result.header_bytes = (result.header_bytes << 8) | prefix[index];
    if (result.header_bytes < 2 || result.header_bytes > max_header ||
        result.header_bytes > result.file_bytes - 8) {
        result.problem = "the announced index does not fit in the file";
        return result;
    }
    std::string text(static_cast<std::size_t>(result.header_bytes), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        result.problem = "the index cannot be read";
        return result;
    }
    const auto header = Json::parse(text, nullptr, false);
    const auto format = header.is_object() ? header.find("format") : header.end();
    const auto index = header.is_object() ? header.find("index") : header.end();
    if (!header.is_object() || format == header.end() || !format->is_string() || index == header.end() ||
        !index->is_array()) {
        result.problem = "the index is not the expected JSON object";
        return result;
    }
    result.format = format->get<std::string>();
    if (!starts(result.format, "fermion-")) {
        result.problem = "the container announces an unknown format";
        return result;
    }
    // Record kinds are only known for the published format; another one is checked for its size.
    const bool known = result.format == fermion_parakeet_format;
    for (const auto& entry : *index) {
        const auto valid = entry.is_object() && entry.contains("n") && entry["n"].is_string() &&
                           entry.contains("k") && entry["k"].is_string() && entry.contains("shape") &&
                           entry["shape"].is_array() && entry["shape"].size() <= 8 && entry.contains("b") &&
                           entry["b"].is_number_unsigned();
        if (!valid) {
            result.problem = "an index entry is malformed";
            return result;
        }
        std::vector<std::uint64_t> shape;
        for (const auto& dimension : entry["shape"]) {
            if (!dimension.is_number_unsigned()) {
                result.problem = "an index entry is malformed";
                return result;
            }
            shape.push_back(dimension.get<std::uint64_t>());
        }
        const auto kind = entry["k"].get<std::string>();
        const auto bytes = entry["b"].get<std::uint64_t>();
        if (known)
            if (auto issue = fermion_record_problem(kind, shape, bytes); !issue.empty()) {
                result.problem = std::move(issue);
                return result;
            }
        if (bytes > (std::uint64_t{1} << 62) - result.payload_bytes) {
            result.problem = "the records are larger than any file";
            return result;
        }
        result.payload_bytes += bytes;
        ++result.records;
        ++result.kinds[kind.substr(0, 40)];
    }
    const auto expected = 8 + result.header_bytes + result.payload_bytes;
    if (result.records == 0)
        result.problem = "the container holds no record";
    else if (result.file_bytes < expected)
        result.problem = "the file is truncated: " + std::to_string(result.file_bytes) +
                         " bytes instead of " + std::to_string(expected);
    else if (result.file_bytes > expected)
        result.problem = "bytes follow the last record";
    result.valid = result.problem.empty();
    return result;
}

Json structure_problems(const fs::path& root, const Json& package) {
    Json problems = Json::array();
    const auto report = [&](const std::string& path, const std::string& detail) {
        problems.push_back({{"path", path}, {"problem", "structure"}, {"detail", detail}});
    };
    const auto files = package.find("files");
    if (files == package.end() || !files->is_array()) return problems;
    std::error_code error;
    for (const auto& file : *files) {
        const auto relative = file.value("path", std::string());
        const auto path = root / from_utf8(relative);
        if (extension(path) != ".fermion" || !fs::is_regular_file(path, error)) continue;
        const auto container = inspect_fermion(path);
        if (!container.valid) report(relative, container.problem);
    }
    // A Fermion folder says three times what its container is: the manifest, the configuration
    // and the container itself must agree.
    const auto manifest = fermion_manifest(root);
    if (!manifest.is_object()) return problems;
    const auto name = fermion_container_name(manifest);
    if (fs::is_regular_file(root / from_utf8(name), error)) {
        const auto container = inspect_fermion(root / from_utf8(name));
        if (container.valid && container.format != manifest.value("format", std::string()))
            report("packed_manifest.json", "the manifest and the container announce different formats");
        if (const auto bytes = manifest.find("container_bytes");
            bytes != manifest.end() && bytes->is_number_unsigned() &&
            bytes->get<std::uint64_t>() != container.file_bytes)
            report("packed_manifest.json", "the manifest announces another size for the container");
    }
    if (fs::is_regular_file(root / "config.json", error)) {
        const auto config = read_json(root / "config.json");
        if (!config.is_object()) {
            report("config.json", "the configuration is not a JSON object");
        } else if (const auto block = config.find("fermion"); block != config.end() && block->is_object()) {
            for (const auto& [mine, theirs] :
                 {std::pair{"container_format", "format"}, std::pair{"container_sha256", "container_sha256"},
                  std::pair{"container_bytes", "container_bytes"}, std::pair{"container", "container"}})
                if (block->contains(mine) && manifest.contains(theirs) && (*block)[mine] != manifest[theirs])
                    report("config.json",
                           std::string("the configuration and the manifest disagree on ") + theirs);
        }
    }
    return problems;
}

bool model_extension(const fs::path& path) {
    static constexpr const char* extensions[] = {".gguf",   ".safetensors", ".sft",     ".ckpt",
                                                 ".pt",     ".pth",         ".bin",     ".onnx",
                                                 ".tflite", ".engine",      ".mlmodel", ".fermion"};
    const auto actual = extension(path);
    return std::any_of(std::begin(extensions), std::end(extensions),
                       [&](const char* candidate) { return actual == candidate; });
}

std::uint64_t minimum_model_size(const fs::path& path) {
    // ".bin" and ".onnx" are used for many small non-model files; embeddings can be a few kilobytes.
    const auto actual = extension(path);
    if (actual == ".bin") return std::uint64_t{8} << 20;
    if (actual == ".onnx" || actual == ".tflite" || actual == ".engine") return std::uint64_t{256} << 10;
    return 1024;
}

Probe probe_file(const fs::path& path) {
    Probe probe;
    const auto actual = extension(path);
    char magic[4]{};
    {
        std::ifstream file(path, std::ios::binary);
        file.read(magic, sizeof(magic));
    }
    // Content first: an Ollama blob has no extension, and a renamed file keeps its header.
    if (std::memcmp(magic, "GGUF", 4) == 0) probe_gguf(path, probe);
    else if (actual == ".safetensors" || actual == ".sft") probe_safetensors(path, probe);
    else if (actual == ".fermion") {
        const auto container = inspect_fermion(path);
        const auto name = to_utf8(path.filename());
        std::string dense;
        for (const auto& [kind, count] : container.kinds)
            if (starts(kind, "int")) dense = kind;
        classify_fermion(probe, container.format, dense);
        probe.metadata["records"] = container.records;
        probe.metadata["recordKinds"] = container.kinds;
        Json problems = Json::array();
        std::string state = "incomplete";
        if (!container.valid) {
            state = "invalid";
            probe.classified_by.clear();
            problems.push_back({{"path", name}, {"problem", "structure"}, {"detail", container.problem}});
        }
        // The engine opens the folder: the container needs its configuration and its manifest.
        std::error_code error;
        for (const auto* companion : {"config.json", "packed_manifest.json"})
            if (!fs::is_regular_file(path.parent_path() / companion, error))
                problems.push_back({{"path", companion}, {"problem", "missing"}});
        probe.validation = {{"state", state},           {"integrity", "none"},
                            {"level", "sizes"},         {"companions", true},
                            {"checked", now_seconds()}, {"problems", std::move(problems)}};
    }
    if (probe.format.empty()) {
        if (actual == ".safetensors" || actual == ".sft") probe.format = "safetensors";
        else if (actual == ".gguf") probe.format = "gguf";
        else if (actual == ".onnx") probe.format = "onnx";
        else if (actual == ".tflite") probe.format = "tflite";
        else if (actual == ".engine") probe.format = "tensorrt";
        else if (actual == ".mlmodel") probe.format = "coreml";
        else if (std::memcmp(magic, "ggml", 4) == 0 || std::memcmp(magic, "lmgg", 4) == 0 ||
                 std::memcmp(magic, "ggjt", 4) == 0 || std::memcmp(magic, "tjgg", 4) == 0)
            probe.format = "ggml";
        else if (actual == ".ckpt" || actual == ".pt" || actual == ".pth" || actual == ".bin") {
            // PyTorch pickles (zip archives or raw pickle streams) are never opened.
            probe.format = "pickle";
            probe.unsafe = true;
        } else
            probe.format = actual.empty() ? "unknown" : actual.substr(1);
    }
    return probe;
}

std::optional<Probe> probe_folder(const fs::path& path) {
    std::error_code error;
    if (const auto manifest = fermion_manifest(path); manifest.is_object()) {
        // A Fermion model is its folder: the container, its configuration and its manifest.
        Probe probe;
        const auto text = [&](const char* key) {
            const auto found = manifest.find(key);
            return found != manifest.end() && found->is_string() ? found->get<std::string>() : std::string();
        };
        classify_fermion(probe, text("format"), text("dense_tables"));
        for (const auto* key : {"base_model", "license", "status", "five_value_modules", "container_bytes"})
            if (const auto found = manifest.find(key); found != manifest.end()) probe.metadata[key] = *found;
        const auto config = read_json(path / "config.json");
        if (config.is_object()) {
            for (const auto* key : {"model_type", "sample_rate"})
                if (const auto found = config.find(key); found != config.end()) probe.metadata[key] = *found;
            if (const auto block = config.find("fermion"); block != config.end() && block->is_object())
                if (const auto model = block->find("model"); model != block->end() && model->is_string())
                    probe.name = model->get<std::string>();
        }
        Json container = {
            {"path", fermion_container_name(manifest)}, {"role", "weights"}, {"required", true}};
        if (const auto bytes = manifest.find("container_bytes");
            bytes != manifest.end() && bytes->is_number_unsigned()) {
            container["size"] = *bytes;
            container["sizeSource"] = "manifest";
        }
        if (const auto hash = lower(text("container_sha256"));
            hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos) {
            container["sha256"] = hash;
            container["hashSource"] = "manifest";
        }
        probe.package = {
            {"identity",
             {{"name", probe.name},
              {"repository", ""},
              {"revision", ""},
              {"variant", to_utf8(path.filename())}}},
            {"provenance", {{"source", "scan"}, {"url", ""}, {"application", ""}}},
            {"classification",
             {{"format", probe.format},
              {"type", probe.type},
              {"modality", probe.modality},
              {"architecture", probe.architecture},
              {"quantization", probe.quantization}}},
            {"entry", ""},
            {"files",
             Json::array({std::move(container),
                          Json{{"path", "config.json"}, {"role", "config"}, {"required", true}},
                          Json{{"path", "packed_manifest.json"}, {"role", "manifest"}, {"required", true}}})},
            {"dependencies", Json::array()}};
        return probe;
    }
    if (fs::is_regular_file(path / "model_index.json", error)) {
        const auto index = read_json(path / "model_index.json");
        if (!index.is_object()) return std::nullopt;
        Probe probe;
        probe.format = "diffusers";
        probe.type = "model";
        probe.classified_by = "header";
        const auto class_name = index.value("_class_name", std::string());
        probe.metadata["pipeline"] = class_name;
        if (const auto version = index.find("_diffusers_version"); version != index.end() && version->is_string())
            probe.metadata["diffusersVersion"] = *version;
        classify_pipeline(probe, class_name);
        return probe;
    }
    if (!fs::is_regular_file(path / "config.json", error)) return std::nullopt;
    const auto config = read_json(path / "config.json");
    if (!config.is_object() || (!config.contains("model_type") && !config.contains("architectures")))
        return std::nullopt;
    bool weights = false, mlx = contains(lower(to_utf8(path.filename())), "mlx");
    for (fs::directory_iterator item(path, fs::directory_options::skip_permission_denied, error), end;
         !error && item != end; item.increment(error)) {
        const auto name = lower(to_utf8(item->path().filename()));
        const auto actual = extension(item->path());
        if (name == "weights.npz") mlx = weights = true;
        if (actual == ".safetensors" || actual == ".bin" || actual == ".h5" || actual == ".msgpack" ||
            actual == ".onnx" || actual == ".pt" || actual == ".pth")
            weights = true;
    }
    if (!weights) return std::nullopt;
    Probe probe;
    probe.format = mlx ? "mlx" : "transformers";
    probe.type = "model";
    probe.classified_by = "header";
    for (const auto* key : {"model_type", "architectures", "torch_dtype", "transformers_version"})
        if (const auto found = config.find(key); found != config.end()) probe.metadata[key] = *found;
    classify_transformers(probe, config);
    if (fs::is_regular_file(path / "modules.json", error)) probe.modality = "embedding";
    return probe;
}

void apply_path_hints(Probe& probe, const fs::path& path, std::string_view extra) {
    // The closest folders say the most: models/loras/sdxl/style.safetensors is a LoRA.
    auto folder = path.parent_path();
    for (int depth = 0; depth < 5 && folder.has_filename(); ++depth, folder = folder.parent_path()) {
        const auto name = lower(to_utf8(folder.filename()));
        for (const auto& hint : folder_hints)
            if (name == hint.word) apply(probe, hint, "folder");
    }
    const auto file = lower(to_utf8(path.filename()));
    for (const auto& hint : file_hints)
        if (contains(file, hint.word)) apply(probe, hint, "name");
    const auto context = lower(std::string(extra));
    for (const auto& hint : file_hints)
        if (!context.empty() && contains(context, hint.word)) apply(probe, hint, "name");
    set_once(probe.quantization, name_quantization(file));
    if (probe.type.empty()) probe.type = "model";
    if (probe.modality.empty()) probe.modality = "other";
}

std::string file_fingerprint(const fs::path& path, std::uint64_t size) {
    constexpr std::uint64_t block = 256 * 1024;
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string data = std::to_string(size) + ':';
    const auto head = static_cast<std::size_t>(std::min(size, block));
    const auto offset = data.size();
    data.resize(offset + head);
    file.read(data.data() + offset, static_cast<std::streamsize>(head));
    if (size > block) {
        const auto tail = static_cast<std::size_t>(std::min(size - block, block));
        const auto second = data.size();
        data.resize(second + tail);
        file.seekg(static_cast<std::streamoff>(size - tail), std::ios::beg);
        file.read(data.data() + second, static_cast<std::streamsize>(tail));
    }
    if (!file) return {};
    return cymcp::sha256(data).substr(0, 32);
}
} // namespace modelhub
