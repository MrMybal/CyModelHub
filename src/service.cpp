// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "service.hpp"
#include "app_updates.hpp"
#include "files.hpp"
#include "installs.hpp"
#include "package.hpp"
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

Hub::Hub(fs::path directory)
    : data_directory(std::move(directory)), catalog(data_directory / "catalog.json"),
      ledger(data_directory / "requests.json", cymcp::sha256(cymcp::random_token(16)).substr(0, 8)),
      reservations(data_directory / "installs.json") {
    jobs.on_finished = [this](const Json& job) { ledger.job_finished(job); };
}

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
    scan_active_ = true;
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
        // The files of a complete model belong to its entry: they are not listed one by one.
        std::erase_if(found, [&](const Model& model) {
            return std::any_of(previous.begin(), previous.end(), [&](const Model& known) {
                return known.package.is_object() && known.folder &&
                       path_key(known.path) != path_key(model.path) && path_within(model.path, known.path);
            });
        });
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
    // An operation that changes something accepts an idempotency key: sent again with the same
    // key and the same arguments, it answers as the first time and has no second effect.
    // `guard` holds the checks that must pass before a remembered answer is given.
    using Guard = std::function<void(const Json&)>;
    const auto add = [&registry, hub](cymcp::Operation operation, Guard guard = {}) {
        const bool keyed = !operation.read_only && operation.name != "hub_shutdown" &&
                           operation.name != "jobs_cancel" && operation.name != "install_leave";
        if (keyed)
            operation.input_schema["properties"]["idempotencyKey"] = text(
                128, "Optional. 8 to 128 letters, digits, '.', '_', ':' or '-', chosen at random by the "
                     "caller and holding no secret. The same key with the same arguments returns the "
                     "first answer (with \"replayed\": true) instead of acting again; with other arguments "
                     "it is refused as IdempotencyConflict. Remembered for 7 days.");
        operation.handler = [hub, keyed, name = operation.name, guard = std::move(guard),
                             handler = std::move(operation.handler)](const Json& args,
                                                                     const CallContext& context) {
            try {
                if (guard) guard(args);
                const auto key = keyed ? string_argument(args, "idempotencyKey") : std::string();
                if (!keyed || !args.contains("idempotencyKey")) return handler(args, context);
                if (key.size() < 8 ||
                    key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                          "0123456789._:-") != std::string::npos)
                    throw Error("InvalidArguments",
                                "idempotencyKey must be 8 to 128 letters, digits, '.', '_', "
                                "':' or '-'");
                auto plain = args;
                plain.erase("idempotencyKey");
                return hub->ledger.run(name, key, Ledger::fingerprint(name, plain),
                                       [&] { return handler(plain, context); });
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
                                   {"jobsRunning", hub->jobs.running_count()},
                                   {"installs", hub->reservations.list().size()}};
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
                           std::error_code error;
                           const auto size = fs::file_size(model.path, error);
                           const auto modified = file_time_seconds(fs::last_write_time(model.path, error));
                           hub->catalog.update(model.id, [&](Model& m) {
                               m.sha256 = hash;
                               // The hash is a measure taken now: later checks compare with it.
                               auto validation = m.validation.is_object() ? m.validation : Json::object();
                               const auto state = validation.value("state", std::string("ready"));
                               validation["state"] = state;
                               validation["level"] = "hashes";
                               validation["checked"] = now_seconds();
                               validation["size"] = size;
                               validation["modified"] = modified;
                               validation["hashedAt"] = now_seconds();
                               if (!validation.contains("hashSource")) validation["hashSource"] = "measured";
                               if (!validation.contains("problems")) validation["problems"] = Json::array();
                               validation["integrity"] = state != "ready" ? "none"
                                                         : validation["hashSource"] == "expected"
                                                             ? "verified"
                                                             : "recorded";
                               m.validation = std::move(validation);
                           });
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

    const Json file_list = {
        {"type", "array"},
        {"minItems", 1},
        {"maxItems", 2000},
        {"description", "The files of the model."},
        {"items",
         object_schema({{"path", text(400, "Path inside the model folder, with forward slashes. No "
                                           "drive, no \"..\".")},
                        {"size", {{"type", "integer"}, {"minimum", 0}, {"description", "Size in bytes."}}},
                        {"sha256", text(64, "SHA-256, 64 hex digits.")},
                        {"role", text(40, "weights, shard, index, config, tokenizer, manifest...")},
                        {"required", boolean("False for a file the model works without. Default: true.")}},
                       {"path"})}};
    const auto names = [](std::initializer_list<std::pair<const char*, const char*>> fields) {
        Json properties = Json::object();
        for (const auto& [name, description] : fields) properties[name] = text(2048, description);
        return object_schema(std::move(properties));
    };
    Json package = object_schema(
        {{"identity", names({{"name", "Display name."},
                             {"repository", "Where the model comes from, e.g. org/name."},
                             {"revision", "Revision of the source. A commit is immutable; a branch is not."},
                             {"variant", "Which build of the model, e.g. q8, fp16, int6."}})},
         {"provenance", names({{"source", "huggingface, url, application..."},
                               {"url", "Address of the source. A query is not kept."},
                               {"application", "The application that brought the model."}})},
         {"classification", names({{"format", "Storage format."},
                                   {"type", "Role, see models_list."},
                                   {"modality", "Domain, see models_list."},
                                   {"architecture", "Model family."},
                                   {"quantization", "Quantization."}})},
         {"entry",
          text(400, "Path, among `files`, of the file the engine opens. Empty when it takes the folder.")},
         {"files", file_list},
         {"dependencies",
          {{"type", "array"},
           {"maxItems", 32},
           {"description", "Other models this one needs. They are recorded, not installed."},
           {"items", object_schema({{"name", text(200, "Name.")},
                                    {"repository", text(200, "Repository.")},
                                    {"revision", text(200, "Revision.")},
                                    {"variant", text(200, "Variant.")},
                                    {"sha256", text(64, "SHA-256 of its main file.")},
                                    {"required", boolean("Default: true.")}})}}}},
        {"files"});
    package["description"] =
        "A complete model: {identity: {name, repository, revision, variant}, provenance: "
        "{source, url, application}, classification: {format, type, modality, architecture, "
        "quantization}, entry, files: [{path, size, sha256, role, required}], dependencies: "
        "[{name, repository, revision, variant, sha256, required}]}. Only `files` is "
        "required. Paths are relative to the model folder; a path that would leave it is "
        "refused. See docs/INTEGRATION.md.";

    add({"install_start",
         "Download a model into a registered location and add it to the catalogue: one file (a direct "
         "https url, or a Hugging Face repository and file), or a complete model made of several files "
         "(`package`). Always answers with a job. Asking for a model that is being installed joins that "
         "installation (`joined`) and returns the same job with a `ticket` of its own; asking for one "
         "that is installed returns it (`installed`) through a job that is already done. Nothing is "
         "downloaded twice: when the same model is asked for another destination, the answer names "
         "the place it is or will be (`destination`, `destinationHonored`: false) and models_link can "
         "show it elsewhere. A complete model is assembled in a folder ending in .incomplete, every "
         "file is checked, and only then does it take its name and appear as ready. Interrupted "
         "downloads resume, also after a restart of the service. A failed job carries a `code`: "
         "ProviderAccessRequired, HashMismatch, SizeMismatch, ValidationFailed, ModelIncomplete, "
         "DownloadFailed, AlreadyExists, Cancelled.",
         object_schema({{"url", text(2048, "Direct https:// address of the model file (http:// is accepted "
                                           "for this computer only).")},
                        {"repository", text(200, "Hugging Face repository, e.g. org/name.")},
                        {"file", text(500, "File inside the repository, e.g. model.Q4_K_M.gguf.")},
                        {"revision", text(100, "Branch, tag or commit. Default: main. Give a commit for an "
                                               "identity that cannot change.")},
                        {"variant", text(100, "Which build of the model this is, when several exist.")},
                        {"package", package},
                        {"baseUrl", text(2048, "With `package`: address under which its files are found "
                                               "(baseUrl/path). Default: the Hugging Face repository.")},
                        {"destination", text(4096, "Absolute path of the folder, inside a registered "
                                                   "location. Default: CyModelHub's own store.")},
                        {"filename", text(200, "Name of the file, or of the folder of a complete model. "
                                               "Default: the name in the address, or the model name.")},
                        {"sha256", text(64, "Expected SHA-256 of the single file, 64 hex digits.")},
                        {"token", text(400, "Access token for gated downloads. It is sent only to the site "
                                            "of the address; it is never stored, logged or returned.")}}),
         output,
         false,
         [hub](const Json& args, const CallContext&) { return start_install(*hub, args); },
         {}},
        [hub](const Json& args) { check_install(*hub, args); });

    add({"install_leave",
         "Stop waiting for an installation, a registration or a verification started or joined with "
         "this ticket. The work goes on for the other requesters; it is cancelled only when the last "
         "one leaves.",
         object_schema({{"job", text(24, "Job ID.")}, {"ticket", text(64, "Ticket returned with the job.")}},
                       {"job", "ticket"}),
         output,
         false,
         [hub](const Json& args, const CallContext&) {
             const auto job = string_argument(args, "job");
             const auto left = hub->reservations.leave(job, string_argument(args, "ticket"));
             const bool cancelled = left.last && hub->jobs.cancel(job);
             return Json{{"left", left.known}, {"cancelled", cancelled}, {"requesters", left.requesters}};
         },
         {}});

    add({"installs_list",
         "Installations, registrations and verifications in progress, with the number of requesters "
         "waiting for each, and the installations a stop of the service left unfinished (state "
         "interrupted): the next install_start for the same model takes them up where they stopped.",
         object_schema(Json::object()),
         output,
         true,
         [hub](const Json&, const CallContext&) { return Json{{"installs", hub->reservations.list()}}; },
         {}});

    add({"models_register",
         "Catalogue a complete model that an application already has on disk, where it is: nothing is "
         "copied or moved. The package lists every file the model needs; each one is looked for under "
         "`root`, its size is compared and, unless verify is sizes, its SHA-256 is computed. Runs as a "
         "job whose result is the model with its `state`: ready, incomplete (a required file is "
         "missing) or invalid (a size or a hash does not match). A model is never announced ready "
         "because its folder exists.",
         object_schema({{"root", text(4096, "Absolute path of the model folder.")},
                        {"package", package},
                        {"verify",
                         {{"type", "string"},
                          {"enum", {"hashes", "sizes"}},
                          {"description", "hashes (default) reads every file once; sizes only "
                                          "looks at presence and sizes."}}}},
                       {"root", "package"}),
         output,
         false,
         [hub](const Json& args, const CallContext&) { return start_registration(*hub, args); },
         {}},
        [hub](const Json& args) { check_registration(*hub, args); });

    add({"models_verify",
         "Check an installed model again, as a job: presence and sizes of its files and, with level "
         "hashes, their SHA-256 against the expected or the recorded ones. Finds missing and damaged "
         "files; never repairs or deletes anything.",
         object_schema(
             {{"id", identifier},
              {"level",
               {{"type", "string"}, {"enum", {"hashes", "sizes"}}, {"description", "Default: hashes."}}}},
             {"id"}),
         output,
         false,
         [hub](const Json& args, const CallContext&) { return start_verification(*hub, args); },
         {}});

    add({"models_check",
         "Strict answer to \"can I use this model now?\", for applications. Unlike models_resolve it "
         "never picks the closest match. Say exactly which model (id, or repository, name, file, "
         "sha256) and what it must be (format, architecture, quantization, revision, variant, files). "
         "status is one of: ready (use `path`, or `entry` for the file the engine opens), absent, "
         "installing (see `job`), incomplete (required files are missing), invalid (a size or a hash "
         "does not match: the validation failed), unverified (the evidence asked with `integrity` was "
         "never gathered: run models_verify), incompatible (see `reasons`), ambiguous (several "
         "different models answer: see `candidates`). Reads no file content.",
         object_schema({{"id", identifier},
                        {"repository", text(200, "Exact repository, e.g. org/name.")},
                        {"name", text(200, "Exact display name, file name or file name without extension.")},
                        {"file", text(400, "Exact file name, or path of a file of a complete model.")},
                        {"sha256", text(64, "SHA-256 of the file, or of one file of a complete model.")},
                        {"revision", text(100, "Revision the model must come from.")},
                        {"variant", text(100, "Variant the model must be.")},
                        {"format", filters.at("format")},
                        {"architecture", filters.at("architecture")},
                        {"quantization", text(60, "Quantization, e.g. Q4_K_M.")},
                        {"type", filters.at("type")},
                        {"modality", filters.at("modality")},
                        {"files",
                         {{"type", "array"},
                          {"maxItems", 2000},
                          {"description", "Files the application needs, with the sizes and hashes "
                                          "it expects."},
                          {"items", object_schema({{"path", text(400, "Path inside the model.")},
                                                   {"size", {{"type", "integer"}, {"minimum", 0}}},
                                                   {"sha256", text(64, "Expected SHA-256.")}},
                                                  {"path"})}}},
                        {"integrity",
                         {{"type", "string"},
                          {"enum", {"none", "sizes", "hashes"}},
                          {"description", "Least evidence accepted. none: being on disk is "
                                          "enough. sizes (default): the expected sizes are "
                                          "known and match. hashes: the SHA-256 of the files "
                                          "was checked and they did not change since."}}}}),
         output,
         true,
         [hub](const Json& args, const CallContext&) { return check_model(*hub, args); },
         {}});

    add({"jobs_list",
         "Background jobs (scan, install, register, verify, move, hash) with their state and "
         "progress.",
         object_schema(Json::object()),
         output,
         true,
         [hub](const Json&, const CallContext&) { return Json{{"jobs", hub->jobs.list()}}; },
         {}});

    add({"jobs_get",
         "State of one job: running, done, failed or cancelled, with progress and result; a "
         "failed job has a `code`. A job started with an idempotency key is still answered "
         "after a restart of the service, with how it ended, or with state interrupted when "
         "the service stopped first.",
         object_schema({{"id", text(24, "Job ID.")}}, {"id"}),
         output,
         true,
         [hub](const Json& args, const CallContext&) {
             const auto id = string_argument(args, "id");
             if (auto job = hub->jobs.find(id)) return Json{{"job", std::move(*job)}};
             if (auto past = hub->ledger.job(id)) return Json{{"job", std::move(*past)}};
             throw Error("NotFound", "No job has this ID");
         },
         {}});

    add({"jobs_cancel",
         "Ask a running job to stop. A job that several requesters wait for is not stopped "
         "by one of them (`shared`: true): each uses install_leave; `force` stops it for "
         "everyone.",
         object_schema(
             {{"id", text(24, "Job ID.")}, {"force", boolean("Stop the job even when others wait for it.")}},
             {"id"}),
         output,
         false,
         [hub](const Json& args, const CallContext&) {
             const auto id = string_argument(args, "id");
             const auto requesters = hub->reservations.requesters(id);
             if (requesters > 1 && !args.value("force", false))
                 return Json{{"cancelled", false}, {"shared", true}, {"requesters", requesters}};
             return Json{{"cancelled", hub->jobs.cancel(id)}};
         },
         {}});

    const auto current_version = text(32, "Version of the desktop application requesting the update.");
    const auto previews = boolean("Include numbered prereleases. Defaults to true.");
    add({"app_updates_check", "Check GitHub for a newer CyModelHub Windows installer with a SHA-256.",
         object_schema({{"currentVersion", current_version}, {"prereleases", previews}}, {"currentVersion"}),
         output, true, [](const Json& args, const CallContext& context) {
             const auto current = string_argument(args, "currentVersion");
             if (!valid_app_version(current)) throw Error("InvalidArguments", "Invalid CyModelHub version");
             return check_app_updates(current, args.value("prereleases", true), context.cancellation);
         }, {}});
    add({"app_updates_download", "Download and verify a CyModelHub Windows installer from its GitHub release. "
         "The verified executable is stored in the private service data directory and is never run by the service.",
         object_schema({{"currentVersion", current_version}, {"prereleases", previews},
                        {"tag", text(33, "GitHub tag returned by app_updates_check.")}}, {"currentVersion", "tag"}),
         output, false, [hub](const Json& args, const CallContext&) {
             const auto current = string_argument(args, "currentVersion");
             const auto tag = string_argument(args, "tag");
             if (!valid_app_version(current) || !tag.starts_with('v') ||
                 !valid_app_version(std::string_view(tag).substr(1)))
                 throw Error("InvalidArguments", "Invalid CyModelHub version or release tag");
             const auto prereleases = args.value("prereleases", true);
             return Json{{"job", hub->jobs.start("app_update", "Download CyModelHub " + tag,
                 [directory = hub->data_directory, current, tag, prereleases](const Job& job) {
                     return download_app_update(directory, current, tag, prereleases, job);
                 })}};
         }, {}});

    add({"hub_shutdown", "Stop the CyModelHub service. Applications start it again when they need it.",
                   object_schema({{"whenIdle", boolean("Refuse shutdown while a job is running.")}}),
                   output, false, [hub](const Json& args, const CallContext&) {
                       if (args.value("whenIdle", false) && !hub->jobs.quiesce_if_idle())
                           throw Error("ServiceBusy", "Wait for the running jobs before updating CyModelHub");
                       hub->shutdown = true;
                       return Json{{"stopping", true}};
                   }, {}});
    return registry;
}
} // namespace modelhub
