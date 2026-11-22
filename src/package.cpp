// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "package.hpp"
#include "formats.hpp"

#include <algorithm>
#include <set>

namespace modelhub {
namespace {
using cymcp::Error;

[[noreturn]] void invalid(const std::string& message) { throw Error("InvalidManifest", message); }

bool hex64(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string trimmed(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    return value.substr(start);
}

std::string text(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// An object made of short strings only, limited to the keys of the contract.
Json strings(const Json& manifest, const char* group, std::initializer_list<const char*> keys) {
    Json result = Json::object();
    for (const auto* key : keys) result[key] = "";
    const auto found = manifest.find(group);
    if (found == manifest.end()) return result;
    if (!found->is_object()) invalid(std::string(group) + " must be an object");
    for (const auto& [key, value] : found->items()) {
        if (!result.contains(key)) invalid("Unknown field " + std::string(group) + "." + key);
        if (!value.is_string() || value.get<std::string>().size() > 2048)
            invalid(std::string(group) + "." + key + " must be a short text");
        result[key] = trimmed(value.get<std::string>());
    }
    return result;
}

bool reserved_name(std::string name) {
    name = lower(name.substr(0, name.find('.')));
    if (name == "con" || name == "prn" || name == "aux" || name == "nul") return true;
    return name.size() == 4 && (name.rfind("com", 0) == 0 || name.rfind("lpt", 0) == 0) && name[3] >= '1' &&
           name[3] <= '9';
}

Json problem(const std::string& path, const char* kind, const std::string& detail = {}) {
    Json value = {{"path", path}, {"problem", kind}};
    if (!detail.empty()) value["detail"] = detail;
    return value;
}

struct Stat {
    bool file = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};
Stat look(const fs::path& path) {
    Stat result;
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return result;
    result.file = true;
    result.size = fs::file_size(path, error);
    result.modified = file_time_seconds(fs::last_write_time(path, error));
    return result;
}

bool has_number(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_number_unsigned();
}

// Strongest claim the facts of a set of files support.
std::string integrity_of(const Json& files, bool ready) {
    if (!ready) return "none";
    // Sizes count as evidence as soon as a manifest announced one: a model is ready only when
    // every announced size matched. Hashes count when every required file was read.
    bool hashed = true, expected_hashes = true, expected_sizes = false, any = false;
    for (const auto& file : files) {
        if (!file.value("required", true)) continue;
        any = true;
        hashed = hashed && file.contains("hashedAt");
        expected_hashes = expected_hashes && text(file, "hashSource") == "manifest";
        expected_sizes = expected_sizes || text(file, "sizeSource") == "manifest";
    }
    if (!any) return "none";
    if (hashed) return expected_hashes ? "verified" : "recorded";
    return expected_sizes ? "sizes" : "none";
}
} // namespace

bool safe_relative_path(std::string_view path) {
    if (path.empty() || path.size() > 400 || path.front() == '/' || path.back() == '/') return false;
    std::size_t depth = 0;
    for (std::size_t start = 0; start <= path.size();) {
        const auto end = std::min(path.find('/', start), path.size());
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.front() == ' ' || part.back() == ' ' ||
            part.back() == '.' || ++depth > 16)
            return false;
        for (const unsigned char c : part)
            if (c < 0x20 || c == 0x7F || std::strchr("\\:*?\"<>|", c)) return false;
        if (reserved_name(std::string(part))) return false;
        start = end + 1;
    }
    return true;
}

fs::path package_file(const fs::path& root, const std::string& relative, bool writing) {
    if (!safe_relative_path(relative))
        throw Error("UnsafePath", "Not a plain relative path inside the model: " + relative.substr(0, 120));
    const auto path = (root / from_utf8(relative)).lexically_normal();
    if (!path_within(path, root)) throw Error("UnsafePath", "The path leaves the model folder");
    if (writing) {
        // A link planted in the folder must not send a download somewhere else.
        std::error_code error;
        const auto real_root = fs::weakly_canonical(root, error);
        const auto real_parent = fs::weakly_canonical(path.parent_path(), error);
        if (error || !path_within(real_parent, real_root))
            throw Error("UnsafePath", "The path leaves the model folder");
    }
    return path;
}

Json normalize_package(const Json& manifest) {
    if (!manifest.is_object()) invalid("The package must be an object");
    for (const auto& [key, value] : manifest.items())
        if (key != "identity" && key != "provenance" && key != "classification" && key != "entry" &&
            key != "files" && key != "dependencies")
            invalid("Unknown field " + key);
    Json package = Json::object();
    package["identity"] = strings(manifest, "identity", {"name", "repository", "revision", "variant"});
    package["provenance"] = strings(manifest, "provenance", {"source", "url", "application"});
    package["classification"] =
        strings(manifest, "classification", {"format", "type", "modality", "architecture", "quantization"});
    // An address may carry a credential in its query: only what precedes it is kept.
    auto address = text(package["provenance"], "url");
    package["provenance"]["url"] = address.substr(0, address.find_first_of("?#"));

    const auto files = manifest.find("files");
    if (files == manifest.end() || !files->is_array() || files->empty() || files->size() > 2000)
        invalid("files must list between 1 and 2000 files");
    Json list = Json::array();
    std::set<std::string> seen;
    bool required = false;
    for (const auto& item : *files) {
        if (!item.is_object()) invalid("Each file must be an object");
        for (const auto& [key, value] : item.items())
            if (key != "path" && key != "size" && key != "sha256" && key != "role" && key != "required")
                invalid("Unknown field files." + key);
        const auto path = text(item, "path");
        if (!safe_relative_path(path))
            invalid("Not a plain relative path inside the model: " + path.substr(0, 120));
        // Windows and macOS do not tell "A.bin" from "a.bin".
        if (!seen.insert(lower(path)).second) invalid("The file is listed twice: " + path);
        Json file = {{"path", path}, {"role", text(item, "role").substr(0, 40)}, {"required", true}};
        if (const auto flag = item.find("required"); flag != item.end()) {
            if (!flag->is_boolean()) invalid("files.required must be true or false");
            file["required"] = flag->get<bool>();
        }
        if (const auto size = item.find("size"); size != item.end()) {
            if (!size->is_number_unsigned()) invalid("files.size must be a number of bytes");
            file["size"] = size->get<std::uint64_t>();
            file["sizeSource"] = "manifest";
        }
        if (auto hash = lower(text(item, "sha256")); !hash.empty()) {
            if (!hex64(hash)) invalid("files.sha256 must be 64 hex digits");
            file["sha256"] = std::move(hash);
            file["hashSource"] = "manifest";
        } else if (item.contains("sha256") && !item["sha256"].is_string()) {
            invalid("files.sha256 must be 64 hex digits");
        }
        required = required || file["required"].get<bool>();
        list.push_back(std::move(file));
    }
    if (!required) invalid("At least one file must be required");
    package["files"] = std::move(list);

    package["entry"] = "";
    if (const auto entry = manifest.find("entry"); entry != manifest.end()) {
        if (!entry->is_string()) invalid("entry must be a path listed in files");
        const auto wanted = entry->get<std::string>();
        if (!wanted.empty() && !seen.contains(lower(wanted))) invalid("entry must be a path listed in files");
        package["entry"] = wanted;
    }

    Json dependencies = Json::array();
    if (const auto found = manifest.find("dependencies"); found != manifest.end()) {
        if (!found->is_array() || found->size() > 32) invalid("dependencies must list at most 32 models");
        for (const auto& item : *found) {
            if (!item.is_object()) invalid("Each dependency must be an object");
            Json dependency = {{"required", true}};
            for (const auto* key : {"name", "repository", "revision", "variant", "sha256"})
                dependency[key] = "";
            for (const auto& [key, value] : item.items()) {
                if (key == "required" && value.is_boolean())
                    dependency[key] = value;
                else if (dependency.contains(key) && key != "required" && value.is_string() &&
                         value.get<std::string>().size() <= 200)
                    dependency[key] = trimmed(value.get<std::string>());
                else
                    invalid("Unknown or malformed field dependencies." + key);
            }
            if (text(dependency, "name").empty() && text(dependency, "repository").empty() &&
                text(dependency, "sha256").empty())
                invalid("A dependency needs a name, a repository or a SHA-256");
            dependencies.push_back(std::move(dependency));
        }
    }
    package["dependencies"] = std::move(dependencies);
    return package;
}

Verified verify_package(const fs::path& root, Json package, bool hashes, const Job* job,
                        const cymcp::Cancellation& cancellation) {
    Verified result;
    Json problems = Json::array();
    bool missing = false, damaged = false;
    std::uint64_t total = 0, done = 0;
    if (hashes)
        for (const auto& file : package.at("files"))
            total += look(package_file(root, text(file, "path"))).size;

    for (auto& file : package.at("files")) {
        cancellation.check();
        const auto relative = text(file, "path");
        const auto found = look(package_file(root, relative));
        if (!found.file) {
            file.erase("hashedAt");
            if (file.value("required", true)) {
                file["state"] = "missing";
                problems.push_back(problem(relative, "missing"));
                missing = true;
            } else {
                file["state"] = "absent";
            }
            continue;
        }
        result.size += found.size;
        ++result.files;
        result.modified = std::max(result.modified, found.modified);
        // A size or a hash seen for the first time becomes the reference for later checks.
        if (!has_number(file, "size")) {
            file["size"] = found.size;
            file["sizeSource"] = "measured";
        } else if (file["size"].get<std::uint64_t>() != found.size) {
            file["state"] = "size";
            file.erase("hashedAt");
            problems.push_back(problem(relative, "size",
                                       std::to_string(found.size) + " bytes instead of " +
                                           std::to_string(file["size"].get<std::uint64_t>())));
            damaged = true;
            continue;
        }
        // Hashes checked before describe the file only while it has not been written again.
        if (file.value("modified", std::int64_t{0}) != found.modified) file.erase("hashedAt");
        if (hashes) {
            if (job) job->update(done, total, "Verifying " + relative);
            const auto actual = lower(cymcp::sha256_file(package_file(root, relative), cancellation));
            done += found.size;
            if (text(file, "sha256").empty()) {
                file["sha256"] = actual;
                file["hashSource"] = "measured";
            } else if (text(file, "sha256") != actual) {
                file["state"] = "hash";
                file.erase("hashedAt");
                problems.push_back(problem(relative, "hash", "The content does not match its SHA-256"));
                damaged = true;
                continue;
            }
            file["hashedAt"] = now_seconds();
        }
        file["modified"] = found.modified;
        file["state"] = "ok";
    }
    for (auto& found : structure_problems(root, package)) {
        problems.push_back(std::move(found));
        damaged = true;
    }
    const std::string state = damaged ? "invalid" : missing ? "incomplete" : "ready";
    result.validation = {{"state", state},
                         {"integrity", integrity_of(package.at("files"), state == "ready")},
                         {"level", hashes ? "hashes" : "sizes"},
                         {"checked", now_seconds()},
                         {"problems", std::move(problems)}};
    result.package = std::move(package);
    return result;
}

void verify_file(Model& model, bool hashes, const cymcp::Cancellation& cancellation) {
    const auto found = look(model.path);
    const auto name = to_utf8(model.path.filename());
    auto previous = model.validation.is_object() ? model.validation : Json::object();
    Json problems = Json::array();
    std::string state = "ready";
    // A digest given by the store the file came from is a reference, like one checked at install.
    auto source = text(previous, "hashSource");
    if (source.empty() && !model.sha256.empty()) source = model.source == "ollama" ? "expected" : "measured";
    bool hashed =
        previous.contains("hashedAt") && previous.value("modified", std::int64_t{0}) == found.modified;
    if (!found.file) {
        state = "incomplete";
        problems.push_back(problem(name, "missing"));
        hashed = false;
    } else if (has_number(previous, "expectedSize") &&
               previous["expectedSize"].get<std::uint64_t>() != found.size) {
        state = "invalid";
        problems.push_back(problem(name, "size"));
        hashed = false;
    } else if (hashes) {
        const auto actual = lower(cymcp::sha256_file(model.path, cancellation));
        if (model.sha256.empty() || source == "measured") {
            // Nothing to compare with, or only an earlier measure: a change is damage all the same.
            if (!model.sha256.empty() && previous.contains("hashedAt") && model.sha256 != actual) {
                state = "invalid";
                problems.push_back(
                    problem(name, "hash", "The content changed since its SHA-256 was recorded"));
            } else {
                model.sha256 = actual;
                source = "measured";
            }
        } else if (lower(model.sha256) != actual) {
            state = "invalid";
            problems.push_back(problem(name, "hash", "The content does not match its SHA-256"));
        }
        hashed = state == "ready";
    }
    for (auto& issue :
         structure_problems(model.path.parent_path(), Json{{"files", Json::array({Json{{"path", name}}})}})) {
        problems.push_back(std::move(issue));
        state = "invalid";
    }
    Json validation = {
        {"state", state},     {"level", hashes ? "hashes" : "sizes"}, {"checked", now_seconds()},
        {"size", found.size}, {"modified", found.modified},           {"problems", std::move(problems)}};
    if (has_number(previous, "expectedSize")) validation["expectedSize"] = previous["expectedSize"];
    if (!source.empty()) validation["hashSource"] = source;
    if (hashed && state == "ready")
        validation["hashedAt"] = hashes ? now_seconds() : previous.value("hashedAt", now_seconds());
    validation["integrity"] = state != "ready" ? "none"
                              : validation.contains("hashedAt")
                                  ? (source == "expected" ? "verified" : "recorded")
                              : validation.contains("expectedSize") ? "sizes"
                                                                    : "none";
    // A container that needs companion files stays incomplete, however sound it is itself.
    if (state == "ready" && text(previous, "state") == "incomplete" && previous.value("companions", false)) {
        validation["state"] = "incomplete";
        validation["companions"] = true;
        validation["integrity"] = "none";
        for (const auto& issue : previous.value("problems", Json::array()))
            if (text(issue, "problem") == "missing") validation["problems"].push_back(issue);
    }
    model.validation = std::move(validation);
}

Presence presence(const Model& model) {
    Presence result;
    std::error_code error;
    if (model.missing || !fs::exists(model.path, error)) {
        result.state = "missing";
        result.problems.push_back(problem("", "missing"));
        return result;
    }
    if (!model.validation.is_object()) return result;
    result.state = text(model.validation, "state");
    result.integrity = text(model.validation, "integrity");
    result.problems = model.validation.value("problems", Json::array());
    if (!model.package.is_object()) {
        if (result.state != "ready") return result;
        const auto found = look(model.path);
        if (model.validation.value("size", found.size) != found.size) {
            result.state = "invalid";
            result.integrity = "none";
            result.problems.push_back(problem(to_utf8(model.path.filename()), "size"));
        } else if (model.validation.value("modified", found.modified) != found.modified &&
                   (result.integrity == "verified" || result.integrity == "recorded")) {
            result.integrity = model.validation.contains("expectedSize") ? "sizes" : "none";
        }
        return result;
    }
    // A hash mismatch found by a full check stays known; sizes and presence are looked at again.
    Json problems = Json::array();
    bool missing = false, damaged = false, stale = false, expected_sizes = false;
    for (const auto& issue : result.problems)
        if (const auto kind = text(issue, "problem"); kind == "hash" || kind == "structure") {
            problems.push_back(issue);
            damaged = true;
        }
    for (const auto& file : model.package.value("files", Json::array())) {
        if (!file.value("required", true)) continue;
        const auto relative = text(file, "path");
        expected_sizes = expected_sizes || text(file, "sizeSource") == "manifest";
        if (!safe_relative_path(relative)) continue;
        const auto found = look(model.path / from_utf8(relative));
        if (!found.file) {
            problems.push_back(problem(relative, "missing"));
            missing = true;
        } else if (has_number(file, "size") && file["size"].get<std::uint64_t>() != found.size) {
            problems.push_back(problem(relative, "size"));
            damaged = true;
        } else if (file.value("modified", found.modified) != found.modified) {
            stale = true;
        }
    }
    result.problems = std::move(problems);
    result.state = damaged ? "invalid" : missing ? "incomplete" : "ready";
    if (result.state != "ready")
        result.integrity = "none";
    else if (stale && (result.integrity == "verified" || result.integrity == "recorded"))
        result.integrity = expected_sizes ? "sizes" : "none";
    return result;
}

void refresh_validation(Model& model) {
    if (!model.validation.is_object()) return;
    const auto now = presence(model);
    if (now.state == "missing") return;
    model.validation["state"] = now.state;
    model.validation["integrity"] = now.integrity;
    model.validation["problems"] = now.problems;
}
} // namespace modelhub
