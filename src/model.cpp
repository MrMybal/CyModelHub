// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "model.hpp"

#include <algorithm>

namespace modelhub {
namespace {
std::vector<std::string> strings(const Json& value, const char* key) {
    std::vector<std::string> result;
    if (const auto found = value.find(key); found != value.end() && found->is_array())
        for (const auto& item : *found)
            if (item.is_string()) result.push_back(item.get<std::string>());
    return result;
}

std::string text(const Json& value, const char* key, std::string fallback = {}) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : fallback;
}

template <class T> T number(const Json& value, const char* key, T fallback) {
    const auto found = value.find(key);
    return found != value.end() && found->is_number() ? found->get<T>() : fallback;
}

bool flag(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_boolean() && found->get<bool>();
}

bool same(const std::string& expected, const std::string& actual) {
    return expected.empty() || lower(expected) == lower(actual);
}
} // namespace

Json Model::to_json(bool details) const {
    Json value = {{"id", id},
                  {"name", name},
                  {"path", to_utf8(path)},
                  {"format", format},
                  {"type", type},
                  {"modality", modality},
                  {"architecture", architecture},
                  {"quantization", quantization},
                  {"folder", folder},
                  {"missing", missing},
                  {"unsafe", unsafe},
                  {"favorite", favorite},
                  {"size", size},
                  {"files", files},
                  {"modified", modified},
                  {"added", added},
                  {"fingerprint", fingerprint},
                  {"sha256", sha256},
                  {"source", source},
                  {"root", root},
                  {"repo", repo},
                  {"revision", revision},
                  {"variant", variant},
                  {"artifact", artifact},
                  {"state", state()},
                  {"integrity", integrity()},
                  {"complete", package.is_object()},
                  {"tags", tags}};
    if (details) {
        if (package.is_object()) value["package"] = package;
        if (validation.is_object()) value["validation"] = validation;
        value["url"] = url;
        value["classifiedBy"] = classified_by;
        value["notes"] = notes;
        value["locked"] = locked;
        value["metadata"] = metadata;
    }
    return value;
}

Model Model::from_json(const Json& value) {
    Model model;
    model.id = text(value, "id");
    model.name = text(value, "name");
    model.path = from_utf8(text(value, "path"));
    model.format = text(value, "format");
    model.type = text(value, "type", "model");
    model.modality = text(value, "modality", "other");
    model.architecture = text(value, "architecture");
    model.quantization = text(value, "quantization");
    model.folder = flag(value, "folder");
    model.missing = flag(value, "missing");
    model.unsafe = flag(value, "unsafe");
    model.favorite = flag(value, "favorite");
    model.size = number<std::uint64_t>(value, "size", 0);
    model.files = number<std::uint32_t>(value, "files", 1);
    model.modified = number<std::int64_t>(value, "modified", 0);
    model.added = number<std::int64_t>(value, "added", 0);
    model.fingerprint = text(value, "fingerprint");
    model.sha256 = text(value, "sha256");
    model.source = text(value, "source");
    model.root = text(value, "root");
    model.repo = text(value, "repo");
    model.url = text(value, "url");
    model.classified_by = text(value, "classifiedBy");
    model.notes = text(value, "notes");
    model.tags = strings(value, "tags");
    model.locked = strings(value, "locked");
    if (const auto found = value.find("metadata"); found != value.end() && found->is_object())
        model.metadata = *found;
    model.artifact = text(value, "artifact");
    model.revision = text(value, "revision");
    model.variant = text(value, "variant");
    if (const auto found = value.find("package"); found != value.end() && found->is_object())
        model.package = *found;
    if (const auto found = value.find("validation"); found != value.end() && found->is_object())
        model.validation = *found;
    return model;
}

bool Model::is_locked(std::string_view field) const {
    return std::find(locked.begin(), locked.end(), field) != locked.end();
}

std::string Model::state() const {
    if (missing) return "missing";
    return validation.is_object() ? text(validation, "state", "present") : "present";
}

std::string Model::integrity() const {
    return validation.is_object() ? text(validation, "integrity", "none") : "none";
}

std::string path_key(const fs::path& path) {
    std::error_code error;
    auto absolute = fs::absolute(path, error);
    auto key = to_utf8((error ? path : absolute).lexically_normal());
    while (key.size() > 3 && (key.back() == '/' || key.back() == '\\')) key.pop_back();
#ifdef _WIN32
    std::replace(key.begin(), key.end(), '/', '\\');
    key = lower(std::move(key));
#endif
    return key;
}

bool path_within(const fs::path& child, const fs::path& parent) {
    const auto inner = path_key(child), outer = path_key(parent);
    if (outer.empty() || inner.size() < outer.size() || inner.compare(0, outer.size(), outer) != 0) return false;
    return inner.size() == outer.size() || inner[outer.size()] == '/' || inner[outer.size()] == '\\' ||
           outer.back() == '/' || outer.back() == '\\';
}

bool matches(const Model& model, const Filter& filter) {
    if (!same(filter.type, model.type) || !same(filter.modality, model.modality) ||
        !same(filter.format, model.format) || !same(filter.architecture, model.architecture) ||
        !same(filter.source, model.source) || !same(filter.root, model.root))
        return false;
    if (filter.favorite && *filter.favorite != model.favorite) return false;
    if (filter.missing && *filter.missing != model.missing) return false;
    if (!filter.tag.empty()) {
        const auto wanted = lower(filter.tag);
        if (std::none_of(model.tags.begin(), model.tags.end(),
                         [&](const std::string& tag) { return lower(tag) == wanted; }))
            return false;
    }
    if (filter.query.empty()) return true;
    auto haystack = lower(model.name + '\n' + to_utf8(model.path) + '\n' + model.repo + '\n' +
                          model.architecture + '\n' + model.quantization + '\n' + model.format + '\n' +
                          model.type + '\n' + model.modality + '\n' + model.sha256);
    for (const auto& tag : model.tags) haystack += '\n' + lower(tag);
    // Every word of the query must appear somewhere.
    const auto query = lower(filter.query);
    std::size_t start = 0;
    while (start < query.size()) {
        const auto end = std::min(query.find(' ', start), query.size());
        if (end > start && haystack.find(query.substr(start, end - start)) == std::string::npos) return false;
        start = end + 1;
    }
    return true;
}

Filter filter_from_json(const Json& arguments) {
    Filter filter;
    filter.query = text(arguments, "query");
    filter.type = text(arguments, "type");
    filter.modality = text(arguments, "modality");
    filter.format = text(arguments, "format");
    filter.architecture = text(arguments, "architecture");
    filter.source = text(arguments, "source");
    filter.root = text(arguments, "root");
    filter.tag = text(arguments, "tag");
    if (const auto found = arguments.find("favorite"); found != arguments.end() && found->is_boolean())
        filter.favorite = found->get<bool>();
    if (const auto found = arguments.find("missing"); found != arguments.end() && found->is_boolean())
        filter.missing = found->get<bool>();
    return filter;
}

int resolve_score(const Model& model, std::string_view wanted_view) {
    const auto wanted = lower(std::string(wanted_view));
    if (wanted.empty()) return 0;
    if (wanted == lower(model.id)) return 100;
    if (!model.sha256.empty() && wanted == lower(model.sha256)) return 95;
    const auto name = lower(model.name);
    const auto file = lower(to_utf8(model.path.filename()));
    const auto stem = lower(to_utf8(model.path.stem()));
    const auto repo = lower(model.repo);
    if (wanted == file) return 90;
    if (wanted == name || wanted == stem) return 85;
    if (!repo.empty() && wanted == repo) return 80;
    // "org/name" also matches the name part of a repository, and the reverse.
    if (!repo.empty() && repo.size() > wanted.size() &&
        repo.compare(repo.size() - wanted.size(), wanted.size(), wanted) == 0 &&
        repo[repo.size() - wanted.size() - 1] == '/')
        return 70;
    if (name.rfind(wanted, 0) == 0 || stem.rfind(wanted, 0) == 0) return 60;
    if (name.find(wanted) != std::string::npos || file.find(wanted) != std::string::npos ||
        (!repo.empty() && repo.find(wanted) != std::string::npos))
        return 40;
    return 0;
}

Json Root::to_json() const {
    return {{"id", id},           {"label", label},         {"kind", kind},      {"path", to_utf8(path)},
            {"enabled", enabled}, {"recursive", recursive}, {"builtin", builtin}};
}
} // namespace modelhub
