// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "service.hpp"
#include "files.hpp"
#include "scanner.hpp"

#include <algorithm>
#include <cymcp/http.hpp>
#include <map>
#include <unordered_map>

namespace modelhub {
namespace {
using cymcp::CallContext;
using cymcp::Error;

Json object_schema(Json properties, Json required = Json::array()) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
            {"additionalProperties", false}};
}
Json text(int limit, const char* description) {
    return {{"type", "string"}, {"maxLength", limit}, {"description", description}};
}
Json boolean(const char* description) { return {{"type", "boolean"}, {"description", description}}; }
Json integer(int minimum, int maximum, const char* description) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}
const Json identifier = text(24, "Model ID, as returned by models_list or models_resolve.");
const Json output = {{"type", "object"}};

std::string string_argument(const Json& arguments, const char* key) {
    const auto found = arguments.find(key);
    return found != arguments.end() && found->is_string() ? found->get<std::string>() : std::string();
}

fs::path absolute_path(const Json& arguments, const char* key) {
    const auto path = from_utf8(string_argument(arguments, key));
    if (!path.is_absolute()) throw Error("InvalidPath", "An absolute path is required");
    return path.lexically_normal();
}

// The watched location that contains a path; the deepest one when locations are nested.
std::optional<Root> root_of(const Hub& hub, const fs::path& path) {
    std::optional<Root> best;
    for (auto& root : hub.catalog.roots())
        if (root.enabled && path_within(path, root.path) &&
            (!best || path_key(root.path).size() > path_key(best->path).size()))
            best = std::move(root);
    return best;
}

// Files are only ever written inside locations the user registered.
fs::path destination(const Hub& hub, const Json& arguments, const char* key) {
    const auto path = absolute_path(arguments, key);
    if (!root_of(hub, path))
        throw Error("OutsideLocations", "The destination must be inside a location registered in CyModelHub. "
                                        "Add the folder as a location first (roots_add).");
    return path;
}

Model require(const Hub& hub, const Json& arguments) {
    auto model = hub.catalog.find(string_argument(arguments, "id"));
    if (!model) throw Error("NotFound", "No model has this ID");
    return std::move(*model);
}

bool external_store(const Model& model) { return model.source == "huggingface" || model.source == "ollama"; }

std::string trimmed(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    return value.substr(start);
}

void sort_models(std::vector<Model>& models, const std::string& key, bool descending) {
    const auto by_name = [](const Model& a, const Model& b) { return lower(a.name) < lower(b.name); };
    std::stable_sort(models.begin(), models.end(), by_name);
    if (key == "size")
        std::stable_sort(models.begin(), models.end(), [](const Model& a, const Model& b) { return a.size < b.size; });
    else if (key == "modified")
        std::stable_sort(models.begin(), models.end(),
                         [](const Model& a, const Model& b) { return a.modified < b.modified; });
    else if (key == "added")
        std::stable_sort(models.begin(), models.end(), [](const Model& a, const Model& b) { return a.added < b.added; });
    else if (key == "type")
        std::stable_sort(models.begin(), models.end(), [](const Model& a, const Model& b) {
            return std::tie(a.modality, a.type, a.architecture) < std::tie(b.modality, b.type, b.architecture);
        });
    if (descending) std::reverse(models.begin(), models.end());
}

Json facets(const std::vector<Model>& models) {
    std::map<std::string, std::map<std::string, std::size_t>> counts;
    std::uint64_t size = 0;
    std::size_t missing = 0, favorites = 0;
    for (const auto& model : models) {
        ++counts["type"][model.type];
        ++counts["modality"][model.modality];
        ++counts["format"][model.format];
        ++counts["source"][model.source];
        if (!model.architecture.empty()) ++counts["architecture"][model.architecture];
        for (const auto& tag : model.tags) ++counts["tag"][tag];
        size += model.missing ? 0 : model.size;
        missing += model.missing;
        favorites += model.favorite;
    }
    Json result = {{"total", models.size()}, {"size", size}, {"missing", missing}, {"favorites", favorites}};
    for (const auto* group : {"type", "modality", "format", "source", "architecture", "tag"}) {
        auto values = Json::array();
        for (const auto& [value, count] : counts[group]) values.push_back({{"value", value}, {"count", count}});
        result[group] = std::move(values);
    }
    return result;
}

Json duplicates(const std::vector<Model>& models) {
    std::map<std::string, std::vector<const Model*>> groups;
    for (const auto& model : models)
        if (!model.missing && !model.fingerprint.empty()) groups[model.fingerprint].push_back(&model);
    auto result = Json::array();
    std::uint64_t wasted = 0;
    for (const auto& [fingerprint, members] : groups) {
        if (members.size() < 2) continue;
        auto items = Json::array();
        std::size_t copies = 0;
        for (const auto* model : members) {
            items.push_back(model->to_json(false));
            // A symbolic link takes no room; it is listed but not counted as a copy.
            copies += !model->metadata.value("symlink", false);
        }
        const auto extra = copies > 1 ? (copies - 1) * members.front()->size : 0;
        wasted += extra;
        result.push_back({{"fingerprint", fingerprint}, {"size", members.front()->size}, {"wasted", extra},
                          {"models", std::move(items)}});
    }
    return {{"groups", std::move(result)}, {"wasted", wasted}};
}
} // namespace

Hub::Hub(fs::path directory) : data_directory(std::move(directory)), catalog(data_directory / "catalog.json") {}

std::string Hub::start_scan(const std::string& root_id) {
    std::lock_guard lock(scan_mutex_);
    if (scan_active_) {
        // "Everything" covers any single location already waiting.
        if (root_id.empty()) scan_pending_.assign(1, std::string());
        else if (std::find(scan_pending_.begin(), scan_pending_.end(), std::string()) == scan_pending_.end() &&
                 std::find(scan_pending_.begin(), scan_pending_.end(), root_id) == scan_pending_.end())
            scan_pending_.push_back(root_id);
        return scan_job_;
    }
    scan_active_ = true;
    scan_job_ = jobs.start("scan", "Scan", [this, first = root_id](const Job& job) {
        auto summary = Json::array();
        for (auto next = first;;) {
            summary.push_back(scan(next, job));
            std::lock_guard pending(scan_mutex_);
            if (scan_pending_.empty()) {
                scan_active_ = false;
                break;
            }
            next = scan_pending_.front();
            scan_pending_.erase(scan_pending_.begin());
        }
        return Json{{"scans", std::move(summary)}};
    });
    return scan_job_;
}

Json Hub::scan(const std::string& root_id, const Job& job) {
    const auto previous = catalog.models();
    std::unordered_map<std::string, const Model*> by_path;
    // Entries written by an older detector are read again instead of being reused.
    if (catalog.detector_current())
        for (const auto& model : previous) by_path[path_key(model.path)] = &model;
    auto summary = Json::array();
    std::size_t total = 0;
    for (const auto& root : catalog.roots()) {
        if (!root.enabled || (!root_id.empty() && root.id != root_id)) continue;
        ScanContext context;
        context.cancellation = job.cancellation();
        context.known = [&](const fs::path& path) -> const Model* {
            const auto found = by_path.find(path_key(path));
            return found == by_path.end() ? nullptr : found->second;
        };
        context.progress = [&](std::size_t found, const fs::path& current) {
            job.update(total + found, 0, to_utf8(current));
        };
        job.update(total, 0, to_utf8(root.path));
        auto found = scan_root(root, context);
        const auto merged = catalog.merge_scan(root, std::move(found));
        total += merged.found;
        summary.push_back({{"root", root.id}, {"found", merged.found}, {"added", merged.added},
                           {"missing", merged.missing}});
    }
    if (root_id.empty()) {
        catalog.refresh_manual();
        if (!job.cancellation().cancelled()) catalog.mark_detector_current();
    }
    return Json{{"roots", std::move(summary)}, {"found", total}};
}

std::shared_ptr<cymcp::Registry> make_registry(std::shared_ptr<Hub> hub) {
    auto registry = std::make_shared<cymcp::Registry>(cymcp::Policy::allow_all());
    // Every operation reports failures as a coded error with readable UTF-8 text, whatever threw.
    const auto add = [&registry](cymcp::Operation operation) {
        operation.handler = [handler = std::move(operation.handler)](const Json& args, const CallContext& context) {
            try {
                return handler(args, context);
            } catch (const Error&) {
                throw;
            } catch (const Failure& failure) {
                throw Error(failure.code, system_text(failure.what()));
            } catch (const Json::exception& error) {
                throw Error("InvalidArguments", error.what());
            } catch (const std::exception& error) {
                throw Error("OperationFailed", system_text(error.what()));
            }
        };
        registry->add(std::move(operation));
    };
    const Json filters = {
        {"query", text(200, "Words that must all appear in the name, path, repository, architecture or tags.")},
        {"type", text(40, "Role: model, lora, vae, controlnet, text_encoder, vision_encoder, embedding, upscaler, "
                          "adapter, projector, other.")},
        {"modality", text(40, "Domain: text (LLM), image, video, audio, embedding, multimodal, 3d, other.")},
        {"format", text(40, "Storage format: gguf, safetensors, pickle, onnx, diffusers, transformers, mlx...")},
        {"architecture", text(60, "Model family, e.g. llama, qwen2, sdxl, sd15, flux, wan, whisper.")},
        {"source", text(40, "Where it was found: huggingface, ollama, lmstudio, managed, folder, manual.")},
        {"root", text(40, "ID of a watched location (see roots_list).")},
        {"tag", text(60, "A user tag.")},
        {"favorite", boolean("Only favourites (true) or only non-favourites (false).")},
        {"missing", boolean("Only entries whose file disappeared (true) or only present ones (false).")}};

    add({"hub_status", "CyModelHub service status: version, data folder, model count, running jobs. The "
                                 "revision number grows whenever the catalogue changes.",
                   object_schema(Json::object()), output, true, [hub](const Json&, const CallContext&) {
                       const auto models = hub->catalog.models();
                       std::uint64_t size = 0;
                       for (const auto& model : models) size += model.missing ? 0 : model.size;
                       return Json{{"name", "CyModelHub"},
                                   {"version", CYMODELHUB_VERSION},
                                   {"platform", cymcp::platform_id()},
                                   {"dataDirectory", to_utf8(hub->data_directory)},
                                   {"managedStore", to_utf8(managed_store())},
                                   {"models", models.size()},
                                   {"size", size},
                                   {"revision", hub->catalog.revision()},
                                   {"jobsRunning", hub->jobs.running_count()}};
                   }, {}});

    auto list_properties = filters;
    list_properties["sort"] = {{"type", "string"}, {"enum", {"name", "size", "modified", "added", "type"}},
                               {"description", "Sort key. Default: name."}};
    list_properties["descending"] = boolean("Reverse the sort order.");
    list_properties["offset"] = integer(0, 1000000, "First result to return.");
    list_properties["limit"] = integer(1, 500, "Page size. Default: 100.");
    add({"models_list", "List the AI models installed on this computer and shared between applications "
                                  "(LLM, diffusion, video, audio, LoRA, VAE...). Filter by words, type, modality, "
                                  "format, architecture or source. Each model has an ID and an absolute path.",
                   object_schema(list_properties), output, true, [hub](const Json& args, const CallContext&) {
                       const auto filter = filter_from_json(args);
                       auto models = hub->catalog.models();
                       std::erase_if(models, [&](const Model& model) { return !matches(model, filter); });
                       sort_models(models, args.value("sort", std::string("name")), args.value("descending", false));
                       const auto offset = args.value("offset", std::size_t{0});
                       const auto limit = args.value("limit", std::size_t{100});
                       auto items = Json::array();
                       for (auto index = offset; index < models.size() && items.size() < limit; ++index)
                           items.push_back(models[index].to_json(false));
                       return Json{{"models", std::move(items)}, {"total", models.size()}, {"offset", offset}};
                   }, {}});

    add({"models_get", "Full record of one model: path, classification, hashes, tags, notes and the "
                                 "metadata read from its header.",
                   object_schema({{"id", identifier}}, {"id"}), output, true,
                   [hub](const Json& args, const CallContext&) {
                       return Json{{"model", require(*hub, args).to_json(true)}};
                   }, {}});

    add({"models_resolve", "Find where a model is installed. Give a name, a file name, a repository "
                                     "(org/name), a SHA-256 or an ID; the best installed match is returned with its "
                                     "absolute path, so an application can load it instead of downloading it again.",
                   object_schema({{"query", text(300, "Name, file name, repository, SHA-256 or ID.")},
                                  {"type", filters.at("type")},
                                  {"modality", filters.at("modality")},
                                  {"format", filters.at("format")}},
                                 {"query"}),
                   output, true, [hub](const Json& args, const CallContext&) {
                       Filter filter;
                       filter.type = string_argument(args, "type");
                       filter.modality = string_argument(args, "modality");
                       filter.format = string_argument(args, "format");
                       const auto wanted = string_argument(args, "query");
                       std::vector<std::pair<int, Model>> ranked;
                       for (auto& model : hub->catalog.models()) {
                           if (model.missing || !matches(model, filter)) continue;
                           if (const auto score = resolve_score(model, wanted)) ranked.emplace_back(score, std::move(model));
                       }
                       std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                           return a.first != b.first ? a.first > b.first : a.second.modified > b.second.modified;
                       });
                       if (ranked.empty()) return Json{{"found", false}, {"alternatives", Json::array()}};
                       auto alternatives = Json::array();
                       for (std::size_t index = 1; index < ranked.size() && index <= 5; ++index)
                           alternatives.push_back(ranked[index].second.to_json(false));
                       return Json{{"found", true},
                                   {"score", ranked.front().first},
                                   {"path", to_utf8(ranked.front().second.path)},
                                   {"model", ranked.front().second.to_json(true)},
                                   {"alternatives", std::move(alternatives)}};
                   }, {}});

    add({"models_facets", "Counts of installed models per type, modality, format, source, architecture "
                                    "and tag, with the total size on disk.",
                   object_schema(Json::object()), output, true,
                   [hub](const Json&, const CallContext&) { return facets(hub->catalog.models()); }, {}});

    add({"models_duplicates", "Groups of models with identical content stored at several places, and the "
                                        "disk space the extra copies use.",
                   object_schema(Json::object()), output, true,
                   [hub](const Json&, const CallContext&) { return duplicates(hub->catalog.models()); }, {}});

    add({"models_update", "Edit how a model is classified and annotated. A field set here is kept by later "
                                    "scans; list a field in `reset` to return it to automatic detection.",
                   object_schema({{"id", identifier},
                                  {"name", text(200, "Display name.")},
                                  {"type", filters.at("type")},
                                  {"modality", filters.at("modality")},
                                  {"architecture", filters.at("architecture")},
                                  {"tags", {{"type", "array"}, {"maxItems", 32}, {"items", text(60, "Tag.")},
                                            {"description", "Replaces the tag list."}}},
                                  {"notes", text(8192, "Free text. An empty string clears it.")},
                                  {"favorite", boolean("Mark or unmark as favourite.")},
                                  {"reset", {{"type", "array"}, {"maxItems", 4},
                                             {"items", {{"type", "string"},
                                                        {"enum", {"name", "type", "modality", "architecture"}}}},
                                             {"description", "Fields to detect automatically again."}}}},
                                 {"id"}),
                   output, false, [hub](const Json& args, const CallContext&) {
                       const auto current = require(*hub, args);
                       std::vector<std::string> reset;
                       if (const auto found = args.find("reset"); found != args.end())
                           reset = found->get<std::vector<std::string>>();
                       // Re-read the file outside the catalogue lock when a field goes back to automatic.
                       const auto fresh = reset.empty() ? std::nullopt : describe_path(current.path);
                       const auto model = hub->catalog.update(current.id, [&](Model& m) {
                           const auto set = [&](const char* key, std::string& field) {
                               if (!args.contains(key)) return;
                               field = trimmed(args.at(key).get<std::string>());
                               if (!m.is_locked(key)) m.locked.emplace_back(key);
                           };
                           set("name", m.name);
                           set("type", m.type);
                           set("modality", m.modality);
                           set("architecture", m.architecture);
                           for (const auto& field : reset) {
                               std::erase(m.locked, field);
                               if (!fresh) continue;
                               // Names of Hugging Face and Ollama entries come back with the next scan.
                               if (field == "name") m.name = external_store(m) ? m.name : fresh->name;
                               else if (field == "type") m.type = fresh->type;
                               else if (field == "modality") m.modality = fresh->modality;
                               else if (field == "architecture") m.architecture = fresh->architecture;
                           }
                           if (const auto tags = args.find("tags"); tags != args.end()) {
                               m.tags.clear();
                               for (const auto& item : *tags) {
                                   auto tag = trimmed(item.get<std::string>());
                                   if (!tag.empty() && std::find(m.tags.begin(), m.tags.end(), tag) == m.tags.end())
                                       m.tags.push_back(std::move(tag));
                               }
                           }
                           if (args.contains("notes")) m.notes = args.at("notes").get<std::string>();
                           if (args.contains("favorite")) m.favorite = args.at("favorite").get<bool>();
                       });
                       return Json{{"model", model.to_json(true)}};
                   }, {}});

    add({"models_add", "Register by hand a model file, or a model folder (Diffusers, Transformers, MLX), "
                                 "that lies outside the watched locations. Nothing is copied or moved.",
                   object_schema({{"path", text(4096, "Absolute path of the file or folder.")}}, {"path"}), output,
                   false, [hub](const Json& args, const CallContext& context) {
                       const auto path = absolute_path(args, "path");
                       ScanContext scan;
                       scan.cancellation = context.cancellation;
                       auto model = describe_path(path, scan);
                       if (!model)
                           throw Error("NotAModel", "No model file or recognised model folder at this path");
                       if (const auto root = root_of(*hub, path)) {
                           model->root = root->id;
                           model->source = root->kind;
                       } else {
                           model->root = "manual";
                           model->source = "manual";
                       }
                       return Json{{"model", hub->catalog.add(std::move(*model)).to_json(true)}};
                   }, {}});

    add({"models_remove", "Forget a model: its entry leaves the catalogue. The file itself is never "
                                    "deleted; a later scan lists it again if it is in a watched location.",
                   object_schema({{"id", identifier}}, {"id"}), output, false,
                   [hub](const Json& args, const CallContext&) {
                       return Json{{"removed", hub->catalog.remove(string_argument(args, "id"))}};
                   }, {}});

    add({"models_move", "Move a model file or folder to another registered location, for example a larger "
                                  "drive. Runs as a job (see jobs_get). Models owned by Hugging Face or Ollama stay "
                                  "where those tools expect them.",
                   object_schema({{"id", identifier},
                                  {"destination", text(4096, "Absolute path of the destination folder, inside a "
                                                             "registered location.")},
                                  {"leaveLink", boolean("Leave a symbolic link at the old place so other "
                                                        "applications still find the model.")}},
                                 {"id", "destination"}),
                   output, false, [hub](const Json& args, const CallContext&) {
                       const auto model = require(*hub, args);
                       if (external_store(model))
                           throw Error("ManagedElsewhere", "This model belongs to the " + model.source +
                                                               " store; use models_link to share it instead");
                       const auto target = destination(*hub, args, "destination");
                       const auto leave_link = args.value("leaveLink", false);
                       const auto job = hub->jobs.start("move", "Move " + model.name,
                                                        [hub = hub.get(), model, target, leave_link](const Job& job) {
                           std::string warning;
                           const auto moved = move_model(model, target, leave_link, job, warning);
                           const auto root = root_of(*hub, moved);
                           const auto updated = hub->catalog.update(model.id, [&](Model& m) {
                               m.path = moved;
                               m.missing = false;
                               m.root = root ? root->id : "manual";
                               m.source = root ? root->kind : "manual";
                           });
                           Json result = {{"model", updated.to_json(false)}};
                           if (!warning.empty()) result["warning"] = warning;
                           return result;
                       });
                       return Json{{"job", job}};
                   }, {}});

    add({"models_link", "Make an installed model visible inside another application's folder without "
                                  "copying it: a hard link (same drive, no extra space) or a symbolic link.",
                   object_schema({{"id", identifier},
                                  {"destination", text(4096, "Absolute path of the folder that receives the link, "
                                                             "inside a registered location.")},
                                  {"mode", {{"type", "string"}, {"enum", {"hard", "symbolic"}},
                                            {"description", "Default: hard for a file, symbolic for a folder."}}}},
                                 {"id", "destination"}),
                   output, false, [hub](const Json& args, const CallContext&) {
                       const auto model = require(*hub, args);
                       const auto mode = args.value("mode", std::string(model.folder ? "symbolic" : "hard"));
                       return Json{{"link", to_utf8(link_model(model, destination(*hub, args, "destination"), mode))}};
                   }, {}});

    add({"models_hash", "Compute the SHA-256 of a model file as a job. The hash identifies the exact file "
                                  "on Hugging Face or Civitai and proves that two copies are identical.",
                   object_schema({{"id", identifier}}, {"id"}), output, false,
                   [hub](const Json& args, const CallContext&) {
                       const auto model = require(*hub, args);
                       if (model.folder) throw Error("InvalidModel", "Only single files can be hashed");
                       const auto job = hub->jobs.start("hash", "Hash " + model.name,
                                                        [hub = hub.get(), model](const Job& job) {
                           job.update(0, 0, model.name);
                           const auto hash = lower(cymcp::sha256_file(model.path, job.cancellation()));
                           hub->catalog.update(model.id, [&](Model& m) { m.sha256 = hash; });
                           return Json{{"id", model.id}, {"sha256", hash}};
                       });
                       return Json{{"job", job}};
                   }, {}});

    add({"roots_list", "List the locations where models are looked for: the stores of Hugging Face, Ollama "
                                 "and LM Studio when present, CyModelHub's own store, and the folders added by the "
                                 "user.",
                   object_schema(Json::object()), output, true, [hub](const Json&, const CallContext&) {
                       const auto models = hub->catalog.models();
                       auto roots = Json::array();
                       for (const auto& root : hub->catalog.roots()) {
                           auto value = root.to_json();
                           value["models"] = std::count_if(models.begin(), models.end(),
                                                           [&](const Model& m) { return m.root == root.id; });
                           roots.push_back(std::move(value));
                       }
                       return Json{{"roots", std::move(roots)}};
                   }, {}});

    add({"roots_add", "Watch a folder for models, for example the models folder of ComfyUI or a drive "
                                "dedicated to models, and scan it.",
                   object_schema({{"path", text(4096, "Absolute path of an existing folder.")},
                                  {"label", text(80, "Name shown for this location.")},
                                  {"recursive", boolean("Also look in sub-folders. Default: true.")}},
                                 {"path"}),
                   output, false, [hub](const Json& args, const CallContext&) {
                       const auto root = hub->catalog.add_root(absolute_path(args, "path"),
                                                               string_argument(args, "label"),
                                                               args.value("recursive", true));
                       return Json{{"root", root.to_json()}, {"job", hub->start_scan(root.id)}};
                   }, {}});

    add({"roots_remove", "Stop watching a folder added by the user and forget the models found in it. "
                                   "No file is deleted.",
                   object_schema({{"id", text(40, "Location ID.")}}, {"id"}), output, false,
                   [hub](const Json& args, const CallContext&) {
                       return Json{{"removed", hub->catalog.remove_root(string_argument(args, "id"))}};
                   }, {}});

    add({"roots_enable", "Switch a location on or off. Switching one off forgets the models found in it; "
                                   "switching it on scans it again.",
                   object_schema({{"id", text(40, "Location ID.")}, {"enabled", boolean("New state.")}},
                                 {"id", "enabled"}),
                   output, false, [hub](const Json& args, const CallContext&) {
                       const auto id = string_argument(args, "id");
                       const auto enabled = args.at("enabled").get<bool>();
                       if (!hub->catalog.enable_root(id, enabled)) throw Error("NotFound", "No location has this ID");
                       Json result = {{"id", id}, {"enabled", enabled}};
                       if (enabled) result["job"] = hub->start_scan(id);
                       return result;
                   }, {}});

    add({"scan_start", "Look for models again in one location, or in all of them. Runs as a job; unchanged "
                                 "files are not read again.",
                   object_schema({{"root", text(40, "Location ID. Omit to scan everything.")}}), output, false,
                   [hub](const Json& args, const CallContext&) {
                       return Json{{"job", hub->start_scan(string_argument(args, "root"))}};
                   }, {}});

    add({"install_start", "Download a model into a registered location and add it to the catalogue. Give "
                                    "either a direct HTTPS url, or a Hugging Face repository and file. Runs as a job "
                                    "with progress; an interrupted download resumes. The SHA-256 is verified when "
                                    "it is known.",
                   object_schema({{"url", text(2048, "Direct https:// address of the model file (http:// is "
                                                     "accepted for this computer only).")},
                                  {"repository", text(200, "Hugging Face repository, e.g. org/name.")},
                                  {"file", text(500, "File inside the repository, e.g. model.Q4_K_M.gguf.")},
                                  {"revision", text(100, "Branch, tag or commit. Default: main.")},
                                  {"destination", text(4096, "Absolute path of the folder, inside a registered "
                                                             "location. Default: CyModelHub's own store.")},
                                  {"filename", text(200, "Name to save as. Default: the name in the address.")},
                                  {"sha256", text(64, "Expected SHA-256, 64 hex digits.")},
                                  {"token", text(400, "Access token for gated downloads. It is sent only to the "
                                                      "site of the address and is not stored.")}}),
                   output, false, [hub](const Json& args, const CallContext&) {
                       Download request;
                       request.url = string_argument(args, "url");
                       const auto repository = string_argument(args, "repository");
                       if (request.url.empty()) {
                           const auto file = string_argument(args, "file");
                           if (repository.empty() || file.empty())
                               throw Error("InvalidArguments", "Give a url, or a repository and a file");
                           request.url = huggingface_url(repository, file, string_argument(args, "revision"));
                       }
                       // HTTPS, or plain HTTP from this computer only (a local mirror).
                       cymcp::parse_url(request.url);
                       request.directory = args.contains("destination") ? destination(*hub, args, "destination")
                                                                        : managed_store();
                       request.filename = string_argument(args, "filename");
                       request.sha256 = string_argument(args, "sha256");
                       request.token = string_argument(args, "token");
                       if (request.token.empty() && request.url.rfind("https://huggingface.co/", 0) == 0)
                           request.token = environment("HF_TOKEN");
                       const auto job = hub->jobs.start("install", "Install " + request.url.substr(0, 200),
                                                        [hub = hub.get(), request, repository](const Job& job) {
                           const auto downloaded = download_file(request, job);
                           ScanContext scan;
                           scan.cancellation = job.cancellation();
                           auto model = describe_path(downloaded.path, scan);
                           if (!model) throw Error("NotAModel", "The downloaded file is not a usable model");
                           const auto root = root_of(*hub, downloaded.path);
                           model->root = root ? root->id : "manual";
                           model->source = root ? root->kind : "manual";
                           model->url = request.url;
                           model->repo = repository;
                           model->sha256 = downloaded.sha256;
                           return Json{{"model", hub->catalog.add(std::move(*model)).to_json(false)}};
                       });
                       return Json{{"job", job}};
                   }, {}});

    add({"jobs_list", "Background jobs (scan, install, move, hash) with their state and progress.",
                   object_schema(Json::object()), output, true,
                   [hub](const Json&, const CallContext&) { return Json{{"jobs", hub->jobs.list()}}; }, {}});

    add({"jobs_get", "State of one job: running, done, failed or cancelled, with progress and result.",
                   object_schema({{"id", text(24, "Job ID.")}}, {"id"}), output, true,
                   [hub](const Json& args, const CallContext&) {
                       return Json{{"job", hub->jobs.get(string_argument(args, "id"))}};
                   }, {}});

    add({"jobs_cancel", "Ask a running job to stop.",
                   object_schema({{"id", text(24, "Job ID.")}}, {"id"}), output, false,
                   [hub](const Json& args, const CallContext&) {
                       return Json{{"cancelled", hub->jobs.cancel(string_argument(args, "id"))}};
                   }, {}});

    add({"hub_shutdown", "Stop the CyModelHub service. Applications start it again when they need it.",
                   object_schema(Json::object()), output, false, [hub](const Json&, const CallContext&) {
                       hub->shutdown = true;
                       return Json{{"stopping", true}};
                   }, {}});
    return registry;
}
} // namespace modelhub
