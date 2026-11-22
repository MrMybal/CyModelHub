// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "scanner.hpp"
#include "formats.hpp"

#include <algorithm>

namespace modelhub {
namespace {
constexpr int max_depth = 12;

void fill(Model& model, Probe probe) {
    model.format = std::move(probe.format);
    model.type = std::move(probe.type);
    model.modality = std::move(probe.modality);
    model.architecture = std::move(probe.architecture);
    model.quantization = std::move(probe.quantization);
    model.classified_by = std::move(probe.classified_by);
    model.metadata = std::move(probe.metadata);
    model.unsafe = probe.unsafe;
}

Model describe_file(const fs::path& path, const ScanContext& context) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    const auto modified = file_time_seconds(fs::last_write_time(path, error));
    if (context.known)
        if (const auto* previous = context.known(path);
            previous && !previous->folder && previous->size == size && previous->modified == modified &&
            !previous->fingerprint.empty()) {
            auto model = *previous;
            model.missing = false;
            return model;
        }
    Model model;
    model.path = path;
    model.name = to_utf8(path.stem());
    model.size = size;
    model.modified = modified;
    auto probe = probe_file(path);
    apply_path_hints(probe, path, context.hint);
    fill(model, std::move(probe));
    model.fingerprint = file_fingerprint(path, size);
    if (fs::is_symlink(path, error)) model.metadata["symlink"] = true;
    return model;
}

Model describe_folder(const fs::path& path, Probe probe, const ScanContext& context) {
    Model model;
    model.path = path;
    model.name = to_utf8(path.filename());
    model.folder = true;
    model.files = 0;
    std::vector<std::string> listing;
    std::error_code error;
    for (fs::recursive_directory_iterator
             item(path, fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink,
                  error),
         end;
         !error && item != end; item.increment(error)) {
        context.cancellation.check();
        if (item.depth() > 8) item.disable_recursion_pending();
        std::error_code ignored;
        if (!item->is_regular_file(ignored)) continue;
        const auto size = item->file_size(ignored);
        model.size += size;
        ++model.files;
        model.modified = std::max(model.modified, file_time_seconds(item->last_write_time(ignored)));
        listing.push_back(to_utf8(item->path().lexically_relative(path)) + ':' + std::to_string(size));
    }
    std::sort(listing.begin(), listing.end());
    std::string joined;
    for (const auto& line : listing) joined += line + '\n';
    model.fingerprint = "d" + cymcp::sha256(joined).substr(0, 31);
    apply_path_hints(probe, path, context.hint);
    fill(model, std::move(probe));
    return model;
}

bool skipped_folder(const std::string& name) {
    static constexpr const char* names[] = {"node_modules", "__pycache__", "site-packages", "venv",
                                            "$recycle.bin", "system volume information"};
    if (name.size() > 1 && name.front() == '.') return true;
    return std::any_of(std::begin(names), std::end(names), [&](const char* skipped) { return name == skipped; });
}

struct Walker {
    const ScanContext& context;
    std::vector<Model>& found;
    bool recursive = true;
    bool follow_links = false; // Hugging Face snapshots are made of links to blobs

    void walk(const fs::path& folder, int depth) {
        std::error_code error;
        for (fs::directory_iterator item(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && item != end; item.increment(error)) {
            context.cancellation.check();
            const auto& path = item->path();
            std::error_code ignored;
            if (item->is_directory(ignored)) {
                if (skipped_folder(lower(to_utf8(path.filename())))) continue;
                if (!follow_links && item->is_symlink(ignored)) continue;
                if (auto probe = probe_folder(path)) {
                    add(describe_folder(path, std::move(*probe), context));
                    continue;
                }
                if (recursive && depth < max_depth) walk(path, depth + 1);
            } else if (item->is_regular_file(ignored) && model_extension(path) &&
                       item->file_size(ignored) >= minimum_model_size(path)) {
                add(describe_file(path, context));
            }
        }
    }

    void add(Model model) {
        found.push_back(std::move(model));
        if (context.progress) context.progress(found.size(), found.back().path);
    }
};

// <hub>/models--org--name/snapshots/<revision>/...
void scan_huggingface(const Root& root, const ScanContext& context, std::vector<Model>& found) {
    std::error_code error;
    for (fs::directory_iterator item(root.path, fs::directory_options::skip_permission_denied, error), end;
         !error && item != end; item.increment(error)) {
        context.cancellation.check();
        const auto name = to_utf8(item->path().filename());
        if (name.rfind("models--", 0) != 0) continue;
        auto repo = name.substr(8);
        for (auto dash = repo.find("--"); dash != std::string::npos; dash = repo.find("--", dash + 1))
            repo.replace(dash, 2, "/");
        const auto snapshots = item->path() / "snapshots";
        fs::path snapshot;
        if (auto reference = read_file(item->path() / "refs" / "main", 256)) {
            while (!reference->empty() && std::isspace(static_cast<unsigned char>(reference->back())))
                reference->pop_back();
            std::error_code ignored;
            if (!reference->empty() && fs::is_directory(snapshots / *reference, ignored))
                snapshot = snapshots / *reference;
        }
        if (snapshot.empty()) {
            fs::file_time_type newest{};
            std::error_code ignored;
            for (fs::directory_iterator revision(snapshots, ignored), last; !ignored && revision != last;
                 revision.increment(ignored)) {
                std::error_code time_error;
                const auto time = revision->last_write_time(time_error);
                if (revision->is_directory(time_error) && (snapshot.empty() || time > newest)) {
                    newest = time;
                    snapshot = revision->path();
                }
            }
        }
        if (snapshot.empty()) continue;
        const auto first = found.size();
        auto scoped = context;
        scoped.hint = repo;
        if (auto probe = probe_folder(snapshot)) {
            auto model = describe_folder(snapshot, std::move(*probe), scoped);
            model.name = repo;
            found.push_back(std::move(model));
        } else {
            Walker{scoped, found, true, true}.walk(snapshot, 0);
        }
        for (auto index = first; index < found.size(); ++index) {
            auto& model = found[index];
            model.repo = repo;
            model.metadata["revision"] = to_utf8(snapshot.filename());
            // "model.safetensors" says nothing: show the repository, and the sub-folder if any.
            static constexpr const char* generic[] = {"model", "pytorch_model", "diffusion_pytorch_model",
                                                      "weights", "adapter_model", "consolidated",
                                                      "pytorch_lora_weights", "open_clip_pytorch_model"};
            const auto stem = lower(to_utf8(model.path.stem()));
            if (!model.folder && std::any_of(std::begin(generic), std::end(generic),
                                             [&](const char* name) { return stem == name; })) {
                const auto parent = model.path.parent_path().lexically_relative(snapshot);
                model.name = parent.empty() || parent == "." ? repo : repo + "/" + parent.generic_string();
            }
        }
        if (context.progress && found.size() > first) context.progress(found.size(), snapshot);
    }
}

// <models>/manifests/<registry>/<namespace>/<name>/<tag> points at <models>/blobs/sha256-<digest>.
void scan_ollama(const Root& root, const ScanContext& context, std::vector<Model>& found) {
    const auto manifests = root.path / "manifests";
    std::error_code error;
    for (fs::recursive_directory_iterator item(manifests, fs::directory_options::skip_permission_denied, error), end;
         !error && item != end; item.increment(error)) {
        context.cancellation.check();
        std::error_code ignored;
        if (!item->is_regular_file(ignored) || item.depth() != 3) continue;
        const auto content = read_file(item->path(), 1024 * 1024);
        const auto manifest = content ? Json::parse(*content, nullptr, false) : Json();
        const auto layers = manifest.is_object() ? manifest.find("layers") : manifest.end();
        if (!manifest.is_object() || layers == manifest.end() || !layers->is_array()) continue;
        for (const auto& layer : *layers) {
            if (!layer.is_object() || layer.value("mediaType", std::string()) != "application/vnd.ollama.image.model")
                continue;
            auto digest = layer.value("digest", std::string());
            if (digest.rfind("sha256:", 0) != 0) continue;
            const auto blob = root.path / "blobs" / ("sha256-" + digest.substr(7));
            if (!fs::is_regular_file(blob, ignored)) continue;
            auto model = describe_file(blob, context);
            const auto relative = item->path().lexically_relative(manifests);
            auto part = relative.begin();
            const auto registry = to_utf8(*part++);
            const auto space = to_utf8(*part++);
            const auto name = to_utf8(*part++);
            const auto tag = to_utf8(*part);
            model.repo = (space == "library" ? "" : space + "/") + name;
            model.name = model.repo + ":" + tag;
            model.sha256 = digest.substr(7);
            model.metadata["registry"] = registry;
            if (model.type.empty() || model.modality == "other") model.modality = "text";
            found.push_back(std::move(model));
            if (context.progress) context.progress(found.size(), blob);
        }
    }
}

void add_root(std::vector<Root>& roots, std::string id, std::string label, std::string kind, const fs::path& path) {
    std::error_code error;
    if (path.empty() || !fs::is_directory(path, error)) return;
    for (const auto& existing : roots)
        if (fs::equivalent(existing.path, path, error)) return;
    Root root;
    root.id = std::move(id);
    root.label = std::move(label);
    root.kind = std::move(kind);
    root.path = path;
    root.builtin = true;
    roots.push_back(std::move(root));
}
} // namespace

fs::path managed_store() { return data_path() / "models"; }

std::vector<Root> known_roots() {
    std::vector<Root> roots;
    const auto home = home_path();

    Root managed;
    managed.id = "managed";
    managed.label = "CyModelHub";
    managed.kind = "managed";
    managed.path = managed_store();
    managed.builtin = true;
    roots.push_back(std::move(managed));

    fs::path hub;
    if (auto cache = environment("HF_HUB_CACHE"); !cache.empty()) hub = from_utf8(cache);
    else if (auto hf_home = environment("HF_HOME"); !hf_home.empty()) hub = from_utf8(hf_home) / "hub";
    else if (auto xdg = environment("XDG_CACHE_HOME"); !xdg.empty()) hub = from_utf8(xdg) / "huggingface" / "hub";
    else hub = home / ".cache" / "huggingface" / "hub";
    add_root(roots, "huggingface", "Hugging Face", "huggingface", hub);

    // OLLAMA_MODELS replaces Ollama's default folder; it does not add to it.
    if (auto custom = environment("OLLAMA_MODELS"); !custom.empty())
        add_root(roots, "ollama", "Ollama", "ollama", from_utf8(custom));
    else
        add_root(roots, "ollama", "Ollama", "ollama", home / ".ollama" / "models");
#if !defined(_WIN32) && !defined(__APPLE__)
    add_root(roots, "ollama-system", "Ollama (system)", "ollama", "/usr/share/ollama/.ollama/models");
#endif
    add_root(roots, "lmstudio", "LM Studio", "lmstudio", home / ".lmstudio" / "models");
    add_root(roots, "lmstudio-cache", "LM Studio", "lmstudio", home / ".cache" / "lm-studio" / "models");
    return roots;
}

std::vector<Model> scan_root(const Root& root, const ScanContext& context) {
    std::vector<Model> found;
    std::error_code error;
    if (!fs::is_directory(root.path, error)) return found;
    if (root.kind == "huggingface") scan_huggingface(root, context, found);
    else if (root.kind == "ollama") scan_ollama(root, context, found);
    else Walker{context, found, root.recursive, false}.walk(root.path, 0);
    for (auto& model : found) {
        model.root = root.id;
        model.source = root.kind;
        if (root.kind == "lmstudio" && model.repo.empty()) {
            // LM Studio stores <publisher>/<repository>/<file>.
            const auto relative = model.path.lexically_relative(root.path);
            if (std::distance(relative.begin(), relative.end()) >= 3) {
                auto part = relative.begin();
                const auto publisher = to_utf8(*part++);
                model.repo = publisher + "/" + to_utf8(*part);
            }
        }
    }
    return found;
}

std::optional<Model> describe_path(const fs::path& path, const ScanContext& context) {
    std::error_code error;
    if (fs::is_directory(path, error)) {
        auto probe = probe_folder(path);
        if (!probe) return std::nullopt;
        return describe_folder(path, std::move(*probe), context);
    }
    if (!fs::is_regular_file(path, error)) return std::nullopt;
    // An unusual extension is accepted only when the content itself is recognised.
    if (!model_extension(path) && probe_file(path).format != "gguf") return std::nullopt;
    return describe_file(path, context);
}
} // namespace modelhub
