// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "installs.hpp"
#include "files.hpp"
#include "package.hpp"
#include "scanner.hpp"

#include <algorithm>
#include <cymcp/http.hpp>

namespace modelhub {
namespace {
using cymcp::Error;

std::string text(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string trimmed(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    return value.substr(start);
}

bool hex64(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool on_disk(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

// Releases a reservation when the job that holds it has really stopped.
struct Release {
    Reservations& reservations;
    std::string artifact;
    ~Release() { reservations.release(artifact); }
};

// The part of a query that tells two files apart, without anything that looks like a
// credential or a signature: it may be hashed and stored.
std::string identity_query(const std::string& url) {
    const auto mark = url.find('?');
    if (mark == std::string::npos) return {};
    const auto query = url.substr(mark + 1, url.find('#', mark) - mark - 1);
    std::vector<std::string> kept;
    for (std::size_t start = 0; start <= query.size();) {
        const auto end = std::min(query.find('&', start), query.size());
        const auto part = query.substr(start, end - start);
        const auto name = lower(part.substr(0, part.find('=')));
        static constexpr const char* words[] = {"token", "key",        "sig",     "auth",  "secret",
                                                "pass",  "credential", "expires", "x-amz", "policy"};
        const bool secret = std::any_of(std::begin(words), std::end(words), [&](const char* word) {
            return name.find(word) != std::string::npos;
        });
        if (!part.empty() && !secret) kept.push_back(part);
        start = end + 1;
    }
    std::sort(kept.begin(), kept.end());
    std::string joined;
    for (const auto& part : kept) joined += part + '&';
    return joined.empty() ? std::string() : cymcp::sha256(joined).substr(0, 16);
}

// What makes a complete model the same model: where it comes from, its revision, its variant,
// and its files with the hashes expected for them. The display name is not part of it.
Json package_identity(const Json& package, const std::string& base) {
    const auto& identity = package.at("identity");
    Json files = Json::array();
    for (const auto& file : package.at("files"))
        files.push_back(
            {{"path", text(file, "path")},
             {"sha256", text(file, "hashSource") == "manifest" ? text(file, "sha256") : std::string()}});
    std::sort(files.begin(), files.end(), [](const Json& a, const Json& b) { return a["path"] < b["path"]; });
    const auto repository = lower(text(identity, "repository"));
    return {{"kind", "package"},
            {"repository", repository},
            // A mirror of a repository holds the same model; without a repository the address
            // is the only thing that says where the files come from.
            {"base", repository.empty() ? public_url(base) : std::string()},
            {"revision", text(identity, "revision").empty() ? "main" : text(identity, "revision")},
            {"variant", text(identity, "variant")},
            {"files", std::move(files)}};
}

// A folder or file name made from a display name.
std::string plain_name(std::string name) {
    for (auto& c : name)
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("/\\:*?\"<>|", c)) c = '-';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    return name;
}

struct Plan {
    bool package = false, repair = false;
    Download request; // one file
    Json manifest;    // a complete model
    std::string base, token, title, repository, revision, variant;
    fs::path target; // the file, or the folder of a complete model
    Json identity;   // stored and shown: holds no credential
    std::string artifact;
};

Plan plan_install(const Hub& hub, const Json& arguments) {
    Plan plan;
    const auto directory =
        arguments.contains("destination") ? destination(hub, arguments, "destination") : managed_store();
    plan.token = text(arguments, "token");
    plan.repository = trimmed(text(arguments, "repository"));
    plan.revision = trimmed(text(arguments, "revision"));
    plan.variant = trimmed(text(arguments, "variant"));
    const auto url = text(arguments, "url"), file = text(arguments, "file"),
               filename = text(arguments, "filename");
    const auto expected = lower(text(arguments, "sha256"));
    if (!expected.empty() && !hex64(expected)) throw Error("InvalidHash", "SHA-256 must be 64 hex digits");

    if (arguments.contains("package")) {
        if (!url.empty() || !file.empty() || !expected.empty())
            throw Error("InvalidArguments",
                        "A package lists its own files: give no url, file or sha256 with it");
        plan.package = true;
        plan.manifest = normalize_package(arguments.at("package"));
        auto& identity = plan.manifest["identity"];
        // The repository, revision and variant may be given beside the package or inside it.
        for (const auto& [key, value] :
             {std::pair{"repository", &plan.repository}, std::pair{"revision", &plan.revision},
              std::pair{"variant", &plan.variant}}) {
            const auto inside = text(identity, key);
            if (!inside.empty() && !value->empty() && inside != *value)
                throw Error("InvalidArguments", std::string(key) + " is given twice with different values");
            if (value->empty()) *value = inside;
            identity[key] = *value;
        }
        plan.base = text(arguments, "baseUrl");
        while (!plan.base.empty() && plan.base.back() == '/') plan.base.pop_back();
        if (plan.base.empty() && plan.repository.empty())
            throw Error("InvalidArguments",
                        "Give the repository of the package, or the baseUrl of its files");
        // HTTPS, or plain HTTP from this computer only (a local mirror).
        if (!plan.base.empty()) cymcp::parse_url(plan.base);
        auto folder = filename;
        if (folder.empty()) {
            folder = text(identity, "name");
            if (folder.empty()) folder = plan.repository.substr(plan.repository.find_last_of('/') + 1);
            if (!plan.variant.empty()) folder += "-" + plan.variant;
            folder = plain_name(folder);
        }
        if (folder.find('/') != std::string::npos || !safe_relative_path(folder))
            throw Error("InvalidName", "Give a plain folder name for the model");
        if (text(identity, "name").empty()) identity["name"] = folder;
        auto& provenance = plan.manifest["provenance"];
        if (text(provenance, "source").empty())
            provenance["source"] = plan.base.empty() ? "huggingface" : "url";
        if (text(provenance, "url").empty())
            provenance["url"] =
                plan.base.empty() ? "https://huggingface.co/" + plan.repository : public_url(plan.base);
        plan.target = directory / from_utf8(folder);
        plan.identity = package_identity(plan.manifest, plan.base);
        plan.title = "Install " + (plan.repository.empty() ? public_url(plan.base) : plan.repository);
    } else {
        plan.request.url = url;
        if (url.empty()) {
            if (plan.repository.empty() || file.empty())
                throw Error("InvalidArguments", "Give a url, or a repository and a file, or a package");
            plan.request.url = huggingface_url(plan.repository, file, plan.revision);
        }
        cymcp::parse_url(plan.request.url);
        plan.request.filename = filename;
        plan.request.sha256 = expected;
        plan.request.always_hash = true;
        plan.target = directory / from_utf8(download_name(plan.request));
        plan.identity = {{"kind", "file"}, {"sha256", expected}, {"variant", plan.variant}};
        if (url.empty()) {
            plan.identity["repository"] = lower(plan.repository);
            plan.identity["revision"] = plan.revision.empty() ? "main" : plan.revision;
            plan.identity["file"] = file;
        } else {
            plan.identity["url"] = public_url(url);
            plan.identity["query"] = identity_query(url);
        }
        plan.title = "Install " + public_url(plan.request.url).substr(0, 200);
    }
    const auto site = plan.package ? (plan.base.empty() ? std::string("https://huggingface.co/") : plan.base)
                                   : plan.request.url;
    if (plan.token.empty() && site.rfind("https://huggingface.co/", 0) == 0)
        plan.token = environment("HF_TOKEN");
    plan.request.token = plan.token;
    plan.artifact = Reservations::artifact_id(plan.identity);
    // Shown with the installation, not part of what identifies it.
    plan.identity["name"] = to_utf8(plan.target.filename());
    return plan;
}

void place(const Hub& hub, Model& model) {
    const auto root = root_of(hub, model.path);
    model.root = root ? root->id : "manual";
    model.source = root ? root->kind : "manual";
}

// The catalogue entry of a complete model, from its checked contract.
Model package_model(const Hub& hub, const fs::path& folder, Json package, Json validation,
                    const cymcp::Cancellation& cancellation) {
    ScanContext scan;
    scan.cancellation = cancellation;
    auto model = describe_tree(folder, scan);
    const auto& identity = package.at("identity");
    const auto& classification = package.at("classification");
    if (!text(identity, "name").empty()) model.name = text(identity, "name");
    bool declared = false;
    for (const auto& [key, field] :
         {std::pair{"format", &model.format}, std::pair{"type", &model.type},
          std::pair{"modality", &model.modality}, std::pair{"architecture", &model.architecture},
          std::pair{"quantization", &model.quantization}})
        if (const auto value = text(classification, key); !value.empty()) {
            *field = value;
            declared = true;
        }
    if (declared) model.classified_by = "manifest";
    if (model.format.empty()) model.format = "folder";
    model.repo = text(identity, "repository");
    model.revision = text(identity, "revision");
    model.variant = text(identity, "variant");
    model.url = text(package.at("provenance"), "url");
    model.package = std::move(package);
    model.validation = std::move(validation);
    place(hub, model);
    return model;
}

std::string problems_summary(const Json& validation) {
    std::string summary;
    for (const auto& issue : validation.value("problems", Json::array())) {
        if (summary.size() > 300) break;
        summary += (summary.empty() ? "" : ", ") + text(issue, "path") + " (" + text(issue, "problem") + ")";
    }
    return summary;
}

Jobs::Work install_file(Hub& hub, Plan plan, const fs::path& target) {
    return [hub = &hub, plan = std::move(plan), target](const Job& job) {
        const Release release{hub->reservations, plan.artifact};
        auto request = plan.request;
        request.directory = target.parent_path();
        request.filename = to_utf8(target.filename());
        std::optional<Model> same;
        // The server may announce the hash of the file: content that is already installed
        // under another name or from another address is not downloaded a second time.
        request.known = [&](const std::string& sha256) {
            if (sha256.empty()) return false;
            for (auto& model : hub->catalog.models())
                if (!model.folder && !model.missing && lower(model.sha256) == sha256 && on_disk(model.path) &&
                    presence(model).state != "invalid") {
                    same = std::move(model);
                    return true;
                }
            return false;
        };
        const auto downloaded = download_file(request, job);
        if (downloaded.skipped) return Json{{"model", same->to_json(false)}, {"reused", true}};
        ScanContext scan;
        scan.cancellation = job.cancellation();
        auto model = describe_path(downloaded.path, scan);
        if (!model) throw Error("NotAModel", "The downloaded file is not a usable model");
        place(*hub, *model);
        model->url = public_url(plan.request.url);
        model->repo = plan.repository;
        model->artifact = plan.artifact;
        model->variant = plan.variant;
        model->revision = !downloaded.commit.empty() ? downloaded.commit
                          : plan.repository.empty()  ? plan.revision
                          : plan.revision.empty()    ? std::string("main")
                                                     : plan.revision;
        if (!model->folder) {
            model->sha256 = downloaded.sha256;
            // What the structure of the file already told (a container that lacks its
            // companions is not usable) is kept beside the hash.
            auto validation = model->validation.is_object() ? model->validation : Json::object();
            const bool ready = text(validation, "state").empty();
            validation["state"] = ready ? "ready" : text(validation, "state");
            validation["level"] = "hashes";
            validation["checked"] = now_seconds();
            validation["size"] = model->size;
            validation["modified"] = model->modified;
            validation["hashSource"] = downloaded.verified ? "expected" : "measured";
            validation["hashedAt"] = now_seconds();
            validation["integrity"] = !ready ? "none" : downloaded.verified ? "verified" : "recorded";
            if (!validation.contains("problems")) validation["problems"] = Json::array();
            model->validation = std::move(validation);
        }
        return Json{{"model", hub->catalog.add(std::move(*model)).to_json(false)}};
    };
}

Jobs::Work install_package(Hub& hub, Plan plan, const fs::path& target) {
    return [hub = &hub, plan = std::move(plan), target](const Job& job) {
        const Release release{hub->reservations, plan.artifact};
        std::error_code error;
        // A new model is assembled beside its final place and only moved there once every
        // file passed its check; a model that lacks files is completed where it is.
        auto staging = target;
        if (!plan.repair) {
            staging += ".incomplete";
            if (fs::exists(target, error))
                throw Error("AlreadyExists", "The destination already has a folder of this name");
        }
        PackageDownload request;
        request.package = plan.manifest;
        request.folder = staging;
        request.token = plan.token;
        request.staging = !plan.repair;
        request.address = [&](const std::string& relative) {
            if (plan.base.empty()) return huggingface_url(plan.repository, relative, plan.revision);
            // The members of a package are plain paths: only spaces and the like need escaping.
            std::string escaped;
            static constexpr char digits[] = "0123456789ABCDEF";
            for (const unsigned char c : relative) {
                if (std::isalnum(c) || std::strchr("-_.~/", c)) {
                    escaped += static_cast<char>(c);
                } else {
                    escaped += '%';
                    escaped += digits[c >> 4];
                    escaped += digits[c & 15];
                }
            }
            return plan.base + "/" + escaped;
        };
        auto downloaded = download_package(request, job);
        // The branch that was asked for, and the commit the server resolved it to.
        auto& identity = downloaded.package["identity"];
        identity["requestedRevision"] = plan.revision.empty() ? "main" : plan.revision;
        identity["revision"] = !downloaded.commit.empty() ? downloaded.commit
                               : plan.revision.empty()    ? std::string("main")
                                                          : plan.revision;
        const auto state = text(downloaded.validation, "state");
        if (state != "ready") {
            if (plan.repair) {
                // The entry tells what is still wrong; nothing was overwritten.
                auto model = package_model(*hub, staging, downloaded.package, downloaded.validation,
                                           job.cancellation());
                model.artifact = plan.artifact;
                hub->catalog.add(std::move(model));
            }
            throw Error(state == "invalid" ? "ValidationFailed" : "ModelIncomplete",
                        "The model did not pass its check: " + problems_summary(downloaded.validation));
        }
        if (!plan.repair) {
            fs::rename(staging, target, error);
            if (error)
                throw Error("WriteFailed",
                            "Cannot finalise the model folder (" + system_text(error.message()) + ")");
        }
        auto model = package_model(*hub, target, std::move(downloaded.package),
                                   std::move(downloaded.validation), job.cancellation());
        model.artifact = plan.artifact;
        const auto added = hub->catalog.add(std::move(model));
        hub->catalog.absorb(target, added.id);
        return Json{{"model", added.to_json(false)}};
    };
}

Json reservation_answer(const Reservations::Reservation& reservation, const fs::path& requested) {
    return {{"job", reservation.job},
            {"ticket", reservation.ticket},
            {"joined", reservation.joined},
            {"resumed", reservation.resumed},
            {"requesters", reservation.requesters},
            {"destination", to_utf8(reservation.target)},
            {"destinationHonored", path_key(reservation.target) == path_key(requested)}};
}

// --- Strict resolution ----------------------------------------------------------------------

struct Wanted {
    std::string path, sha256;
    std::optional<std::uint64_t> size;
};

int rank(const std::string& integrity) {
    return integrity == "verified" ? 3 : integrity == "recorded" ? 2 : integrity == "sizes" ? 1 : 0;
}

struct Verdict {
    std::string status = "ready";
    Json problems = Json::array();
    std::string integrity;
};

void worsen(Verdict& verdict, const char* status) {
    static constexpr const char* order[] = {"ready", "unverified", "incomplete", "invalid"};
    const auto level = [&](const std::string& value) {
        return std::find(std::begin(order), std::end(order), value) - std::begin(order);
    };
    if (level(status) > level(verdict.status)) verdict.status = status;
}

// Does this installed model hold the files the application needs, with the evidence it asks?
Verdict judge(const Model& model, const std::vector<Wanted>& files, const std::string& sha256,
              const std::string& minimum) {
    Verdict verdict;
    const auto now = presence(model);
    verdict.integrity = now.integrity;
    const auto note = [&](const std::string& path, const char* kind, const std::string& detail = {}) {
        Json issue = {{"path", path}, {"problem", kind}};
        if (!detail.empty()) issue["detail"] = detail;
        verdict.problems.push_back(std::move(issue));
    };
    if (now.state == "missing" || now.state == "incomplete" || now.state == "invalid") {
        verdict.status = now.state == "invalid" ? "invalid" : "incomplete";
        verdict.problems = now.problems;
        return verdict;
    }
    const bool hashes_fresh = rank(now.integrity) >= 2;
    bool sizes_given = !files.empty();
    std::error_code error;
    for (const auto& wanted : files) {
        std::uint64_t size = 0;
        std::string hash;
        bool found = false, hashed = false;
        if (model.package.is_object()) {
            for (const auto& file : model.package.value("files", Json::array()))
                if (lower(text(file, "path")) == lower(wanted.path)) {
                    found = text(file, "state") == "ok" &&
                            fs::is_regular_file(model.path / from_utf8(text(file, "path")), error);
                    size = file.value("size", std::uint64_t{0});
                    hash = text(file, "sha256");
                    hashed = file.contains("hashedAt") && hashes_fresh;
                }
        } else if (model.folder) {
            const auto path = model.path / from_utf8(wanted.path);
            found = fs::is_regular_file(path, error);
            if (found) size = fs::file_size(path, error);
        } else {
            found = lower(to_utf8(model.path.filename())) == lower(wanted.path) &&
                    fs::is_regular_file(model.path, error);
            if (found) size = fs::file_size(model.path, error);
            hash = lower(model.sha256);
            hashed = hashes_fresh;
        }
        if (!found) {
            note(wanted.path, "missing");
            worsen(verdict, "incomplete");
            continue;
        }
        if (wanted.size && *wanted.size != size) {
            note(wanted.path, "size",
                 std::to_string(size) + " bytes instead of " + std::to_string(*wanted.size));
            worsen(verdict, "invalid");
        }
        if (!wanted.size) sizes_given = false;
        if (wanted.sha256.empty()) continue;
        if (hash.empty() || !hashed) {
            note(wanted.path, "unverified",
                 "The SHA-256 of this file has not been checked since it last changed");
            worsen(verdict, "unverified");
        } else if (hash != wanted.sha256) {
            note(wanted.path, "hash", "The file is not the expected one");
            worsen(verdict, "invalid");
        }
    }
    if (!sha256.empty() && !model.folder) {
        if (lower(model.sha256) != sha256) {
            note(to_utf8(model.path.filename()), "hash", "The file is not the expected one");
            worsen(verdict, "invalid");
        } else if (!hashes_fresh) {
            note(to_utf8(model.path.filename()), "unverified",
                 "The SHA-256 of this file has not been checked since it last changed");
            worsen(verdict, "unverified");
        }
    }
    // Being there is not being whole: without the evidence asked for, the answer is not "ready".
    if (verdict.status == "ready") {
        if (minimum == "hashes" && !hashes_fresh) {
            note("", "unverified", "No SHA-256 was checked for this model: run models_verify");
            worsen(verdict, "unverified");
        } else if (minimum == "sizes" && rank(now.integrity) < 1 && !sizes_given) {
            note("", "unverified",
                 "No expected size is known for this model: give the sizes or hashes of its "
                 "files, or run models_verify");
            worsen(verdict, "unverified");
        }
    }
    return verdict;
}
} // namespace

std::optional<Root> root_of(const Hub& hub, const fs::path& path) {
    std::optional<Root> best;
    for (auto& root : hub.catalog.roots())
        if (root.enabled && path_within(path, root.path) &&
            (!best || path_key(root.path).size() > path_key(best->path).size()))
            best = std::move(root);
    return best;
}

fs::path absolute_path(const Json& arguments, const char* key) {
    const auto path = from_utf8(text(arguments, key));
    if (!path.is_absolute()) throw Error("InvalidPath", "An absolute path is required");
    return path.lexically_normal();
}

fs::path destination(const Hub& hub, const Json& arguments, const char* key) {
    const auto path = absolute_path(arguments, key);
    if (!root_of(hub, path))
        throw Error("OutsideLocations", "The destination must be inside a location registered in CyModelHub. "
                                        "Add the folder as a location first (roots_add).");
    return path;
}

void check_install(const Hub& hub, const Json& arguments) { (void)plan_install(hub, arguments); }

Json start_install(Hub& hub, const Json& arguments) {
    auto plan = plan_install(hub, arguments);
    // A complete model that lacks files is completed where it is, not installed a second time.
    if (plan.package)
        for (const auto& model : hub.catalog.models())
            if (model.artifact == plan.artifact && model.folder && on_disk(model.path)) {
                plan.target = model.path;
                plan.repair = true;
            }
    const auto requested = plan.target;
    std::optional<Model> installed;
    const auto reservation = hub.reservations.acquire(
        plan.artifact, plan.identity, plan.target, [&](const fs::path& target, bool) -> std::string {
            // Looked at under the reservation: a job that just ended has already added its model.
            for (auto& model : hub.catalog.models()) {
                if (model.missing || !on_disk(model.path)) continue;
                const auto state = presence(model).state;
                const bool same =
                    model.artifact == plan.artifact && (plan.package ? state == "ready" : state != "invalid");
                const bool content = !plan.package && !plan.request.sha256.empty() && !model.folder &&
                                     lower(model.sha256) == plan.request.sha256 && state != "invalid";
                if (same || content) {
                    installed = std::move(model);
                    return {};
                }
            }
            plan.repair = plan.repair && path_key(target) == path_key(requested);
            return hub.jobs.start("install", plan.title,
                                  plan.package ? install_package(hub, plan, target)
                                               : install_file(hub, plan, target));
        });
    auto answer = reservation_answer(reservation, requested);
    answer["artifact"] = plan.artifact;
    answer["installed"] = false;
    if (installed) {
        // Nothing to download. The answer still names a job, so every caller waits the same way.
        const auto model = installed->to_json(false);
        answer["job"] = hub.jobs.start(
            "install", plan.title, [model](const Job&) { return Json{{"model", model}, {"reused", true}}; });
        answer["ticket"] = "";
        answer["installed"] = true;
        answer["model"] = model;
        answer["destination"] = to_utf8(installed->path);
        answer["destinationHonored"] = path_key(installed->path) == path_key(requested);
    }
    return answer;
}

void check_registration(const Hub&, const Json& arguments) {
    (void)absolute_path(arguments, "root");
    (void)normalize_package(arguments.at("package"));
}

Json start_registration(Hub& hub, const Json& arguments) {
    const auto root = absolute_path(arguments, "root");
    auto package = normalize_package(arguments.at("package"));
    std::error_code error;
    if (!fs::is_directory(root, error)) throw Error("NotFound", "The model folder does not exist");
    if (text(package["provenance"], "source").empty()) package["provenance"]["source"] = "application";
    if (text(package["identity"], "name").empty()) package["identity"]["name"] = to_utf8(root.filename());
    const bool hashes = arguments.value("verify", std::string("hashes")) != "sizes";
    // The same identity as an installation of this model by the Hub would have: either way
    // of bringing the model is then recognised by the other.
    const auto artifact =
        Reservations::artifact_id(package_identity(package, text(package["provenance"], "url")));
    const Json identity = {{"kind", "register"},
                           {"root", path_key(root)},
                           {"model", artifact},
                           {"verify", hashes ? "hashes" : "sizes"},
                           {"name", text(package["identity"], "name")}};
    const auto reserved = Reservations::artifact_id(identity);
    const auto reservation = hub.reservations.acquire(reserved, identity, root, [&](const fs::path&, bool) {
        return hub.jobs.start("register", "Register " + text(package["identity"], "name"),
                              [hub = &hub, root, package, hashes, artifact, reserved](const Job& job) {
                                  const Release release{hub->reservations, reserved};
                                  auto verified =
                                      verify_package(root, package, hashes, &job, job.cancellation());
                                  auto model =
                                      package_model(*hub, root, std::move(verified.package),
                                                    std::move(verified.validation), job.cancellation());
                                  model.artifact = artifact;
                                  const auto added = hub->catalog.add(std::move(model));
                                  hub->catalog.absorb(root, added.id);
                                  return Json{{"model", added.to_json(true)}};
                              });
    });
    auto answer = reservation_answer(reservation, root);
    answer["artifact"] = artifact;
    return answer;
}

Json start_verification(Hub& hub, const Json& arguments) {
    const auto model = hub.catalog.find(text(arguments, "id"));
    if (!model) throw Error("NotFound", "No model has this ID");
    if (model->folder && !model->package.is_object())
        throw Error("NoContract",
                    "This folder has no list of files to check. Describe it with models_register");
    const bool hashes = arguments.value("level", std::string("hashes")) != "sizes";
    const Json identity = {
        {"kind", "verify"}, {"id", model->id}, {"level", hashes ? "hashes" : "sizes"}, {"name", model->name}};
    const auto reserved = Reservations::artifact_id(identity);
    const auto reservation =
        hub.reservations.acquire(reserved, identity, model->path, [&](const fs::path&, bool) {
            return hub.jobs.start("verify", "Verify " + model->name,
                                  [hub = &hub, model = *model, hashes, reserved](const Job& job) {
                                      const Release release{hub->reservations, reserved};
                                      job.update(0, 0, model.name);
                                      Model checked = model;
                                      if (model.package.is_object()) {
                                          auto verified = verify_package(model.path, model.package, hashes,
                                                                         &job, job.cancellation());
                                          checked.package = std::move(verified.package);
                                          checked.validation = std::move(verified.validation);
                                      } else {
                                          verify_file(checked, hashes, job.cancellation());
                                      }
                                      const auto updated = hub->catalog.update(model.id, [&](Model& m) {
                                          m.package = checked.package;
                                          m.validation = checked.validation;
                                          m.sha256 = checked.sha256;
                                          m.missing = !on_disk(m.path);
                                      });
                                      return Json{{"model", updated.to_json(true)}};
                                  });
        });
    return reservation_answer(reservation, model->path);
}

Json check_model(const Hub& hub, const Json& arguments) {
    const auto wanted = [&](const char* key) { return lower(trimmed(text(arguments, key))); };
    const auto id = trimmed(text(arguments, "id"));
    const auto repository = wanted("repository"), name = wanted("name"), file = wanted("file"),
               sha256 = wanted("sha256"), revision = wanted("revision"), variant = wanted("variant");
    if (id.empty() && repository.empty() && name.empty() && file.empty() && sha256.empty())
        throw Error("InvalidArguments", "Say which model: an id, a repository, a name, a file or a sha256");
    if (!sha256.empty() && !hex64(sha256)) throw Error("InvalidHash", "SHA-256 must be 64 hex digits");
    const auto minimum = arguments.value("integrity", std::string("sizes"));
    std::vector<Wanted> files;
    for (const auto& item : arguments.value("files", Json::array())) {
        Wanted entry;
        entry.path = text(item, "path");
        entry.sha256 = lower(text(item, "sha256"));
        if (!safe_relative_path(entry.path))
            throw Error("UnsafePath", "Not a plain relative path inside the model");
        if (!entry.sha256.empty() && !hex64(entry.sha256))
            throw Error("InvalidHash", "SHA-256 must be 64 hex digits");
        if (const auto size = item.find("size"); size != item.end() && size->is_number_unsigned())
            entry.size = size->get<std::uint64_t>();
        files.push_back(std::move(entry));
    }

    // An installation in progress for the same request.
    const auto installing = hub.reservations.find([&](const Json& identity) {
        const auto kind = text(identity, "kind");
        if (kind != "file" && kind != "package") return false;
        if (!id.empty()) return false;
        const auto holds = [&](const std::string& value, bool hash) {
            if (kind == "file")
                return hash
                           ? text(identity, "sha256") == value
                           : lower(text(identity, "file")) == value || lower(text(identity, "name")) == value;
            for (const auto& member : identity.value("files", Json::array()))
                if (lower(text(member, hash ? "sha256" : "path")) == value) return true;
            return false;
        };
        const auto shown = lower(text(identity, "name"));
        if (!repository.empty() && text(identity, "repository") != repository) return false;
        if (!revision.empty() && lower(text(identity, "revision")) != revision) return false;
        if (!variant.empty() && lower(text(identity, "variant")) != variant) return false;
        if (!name.empty() && shown != name && shown.substr(0, shown.find_last_of('.')) != name) return false;
        if (!file.empty() && !holds(file, false)) return false;
        if (!sha256.empty() && !holds(sha256, true)) return false;
        return true;
    });
    const auto with_install = [&](Json answer) {
        if (installing) {
            answer["job"] = installing->at("job");
            answer["install"] = *installing;
        }
        return answer;
    };

    // Who is meant: exact identity only, never "the closest name".
    std::vector<Model> candidates;
    for (auto& model : hub.catalog.models()) {
        if (!id.empty()) {
            if (model.id != id) continue;
        } else {
            const auto holds = [&](const std::string& value, bool hash) {
                if (!model.package.is_object())
                    return hash ? lower(model.sha256) == value
                                : lower(to_utf8(model.path.filename())) == value;
                for (const auto& member : model.package.value("files", Json::array()))
                    if (lower(text(member, hash ? "sha256" : "path")) == value) return true;
                return false;
            };
            if (!repository.empty() && lower(model.repo) != repository) continue;
            if (!name.empty() && lower(model.name) != name && lower(to_utf8(model.path.filename())) != name &&
                lower(to_utf8(model.path.stem())) != name)
                continue;
            if (!file.empty() && !holds(file, false)) continue;
            if (!sha256.empty() && !holds(sha256, true)) continue;
        }
        candidates.push_back(std::move(model));
    }
    if (candidates.empty()) return with_install({{"status", installing ? "installing" : "absent"}});

    // What it must be.
    struct Judged {
        Model model;
        Json reasons = Json::array();
        Verdict verdict;
    };
    std::vector<Judged> judged;
    for (auto& model : candidates) {
        Judged entry;
        const auto differs = [&](const char* key, const std::string& actual) {
            const auto expected = wanted(key);
            if (!expected.empty() && lower(actual) != expected)
                entry.reasons.push_back(std::string(key) + " is " + (actual.empty() ? "unknown" : actual) +
                                        ", not " + text(arguments, key));
        };
        differs("format", model.format);
        differs("architecture", model.architecture);
        differs("quantization", model.quantization);
        differs("type", model.type);
        differs("modality", model.modality);
        differs("variant", model.variant);
        if (!revision.empty()) {
            // A branch name is accepted when it is the one the model was installed from.
            const auto requested =
                model.package.is_object()
                    ? lower(text(model.package.value("identity", Json::object()), "requestedRevision"))
                    : std::string();
            const auto snapshot = lower(text(model.metadata, "revision"));
            if (lower(model.revision) != revision && requested != revision && snapshot != revision)
                entry.reasons.push_back("revision is " +
                                        (model.revision.empty() && snapshot.empty() ? std::string("unknown")
                                         : model.revision.empty()                   ? snapshot
                                                                                    : model.revision) +
                                        ", not " + text(arguments, "revision"));
        }
        if (entry.reasons.empty())
            entry.verdict = judge(model, files, id.empty() ? sha256 : std::string(), minimum);
        entry.model = std::move(model);
        judged.push_back(std::move(entry));
    }
    const auto describe = [](const Judged& entry) {
        auto model = entry.model.to_json(false);
        Json value = {{"model", std::move(model)}};
        if (!entry.reasons.empty()) {
            value["status"] = "incompatible";
            value["reasons"] = entry.reasons;
        } else {
            value["status"] = entry.verdict.status;
            value["problems"] = entry.verdict.problems;
        }
        return value;
    };
    Json listed = Json::array();
    for (std::size_t index = 0; index < judged.size() && index < 8; ++index)
        listed.push_back(describe(judged[index]));

    std::vector<const Judged*> compatible, ready;
    for (const auto& entry : judged)
        if (entry.reasons.empty()) {
            compatible.push_back(&entry);
            if (entry.verdict.status == "ready") ready.push_back(&entry);
        }
    if (compatible.empty()) {
        if (installing) return with_install({{"status", "installing"}, {"candidates", std::move(listed)}});
        return {{"status", "incompatible"},
                {"reasons", judged.front().reasons},
                {"candidates", std::move(listed)}};
    }
    const auto chosen = [&](const Judged& entry, const char* status) {
        const auto& model = entry.model;
        auto record = model.to_json(true);
        record["integrity"] = entry.verdict.integrity;
        Json answer = {{"status", status},
                       {"model", std::move(record)},
                       {"path", to_utf8(model.path)},
                       {"root", to_utf8(model.folder ? model.path : model.path.parent_path())},
                       {"integrity", entry.verdict.integrity},
                       {"problems", entry.verdict.problems}};
        // What the engine opens: the file a contract names, else the model itself.
        const auto entry_file = model.package.is_object() ? text(model.package, "entry") : std::string();
        answer["entry"] = to_utf8(
            entry_file.empty() ? model.path : (model.path / from_utf8(entry_file)).lexically_normal());
        return answer;
    };
    if (ready.size() > 1) {
        // Several copies of the same content are one model; different contents are not.
        const auto& first = ready.front()->model;
        const bool copies = std::all_of(ready.begin(), ready.end(), [&](const Judged* entry) {
            const auto& model = entry->model;
            return (!first.sha256.empty() && lower(model.sha256) == lower(first.sha256)) ||
                   (!first.fingerprint.empty() && model.fingerprint == first.fingerprint);
        });
        if (!copies) return {{"status", "ambiguous"}, {"candidates", std::move(listed)}};
        std::stable_sort(ready.begin(), ready.end(), [](const Judged* a, const Judged* b) {
            return rank(a->verdict.integrity) > rank(b->verdict.integrity);
        });
    }
    if (!ready.empty()) {
        auto answer = chosen(*ready.front(), "ready");
        if (ready.size() > 1) answer["candidates"] = std::move(listed);
        return answer;
    }
    if (installing) return with_install({{"status", "installing"}, {"candidates", std::move(listed)}});
    // Not usable as it is. With several candidates, none is picked for the caller.
    if (compatible.size() > 1) {
        const bool all = std::all_of(compatible.begin(), compatible.end(), [&](const Judged* entry) {
            return entry->verdict.status == compatible.front()->verdict.status;
        });
        return {{"status", all ? compatible.front()->verdict.status : "ambiguous"},
                {"candidates", std::move(listed)}};
    }
    return chosen(*compatible.front(), compatible.front()->verdict.status.c_str());
}
} // namespace modelhub
