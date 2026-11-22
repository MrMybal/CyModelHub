// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

#include <cymodelhub/client.hpp>

#include "platform.hpp"
#include <algorithm>
#include <mutex>
#include <random>
#include <thread>

namespace cymodelhub {
namespace {
namespace fs = std::filesystem;

std::string text(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

void set(Json& arguments, const char* key, const std::string& value) {
    if (!value.empty()) arguments[key] = value;
}

Json files_json(const std::vector<ModelFile>& files, bool contract) {
    Json list = Json::array();
    for (const auto& file : files) {
        Json item = {{"path", file.path}};
        if (file.size) item["size"] = *file.size;
        set(item, "sha256", file.sha256);
        if (contract) {
            set(item, "role", file.role);
            if (!file.required) item["required"] = false;
        }
        list.push_back(std::move(item));
    }
    return list;
}

// Operations that only read, or that have the same effect however often they are sent.
bool repeatable(const std::string& operation, const Json& arguments) {
    static constexpr const char* safe[] = {
        "hub_status",        "models_list",  "models_get",  "models_resolve", "models_facets",
        "models_duplicates", "models_check", "roots_list",  "jobs_list",      "jobs_get",
        "installs_list",     "hub_shutdown", "jobs_cancel", "install_leave"};
    if (std::any_of(std::begin(safe), std::end(safe), [&](const char* name) { return operation == name; }))
        return true;
    const auto key = arguments.is_object() ? arguments.find("idempotencyKey") : arguments.end();
    return arguments.is_object() && key != arguments.end() && key->is_string() &&
           !key->get<std::string>().empty();
}

Installation installation_from_json(Json answer, std::string key) {
    Installation installation;
    installation.job = text(answer, "job");
    installation.ticket = text(answer, "ticket");
    installation.artifact = text(answer, "artifact");
    installation.idempotency_key = std::move(key);
    installation.joined = answer.value("joined", false);
    installation.resumed = answer.value("resumed", false);
    installation.installed = answer.value("installed", false);
    installation.replayed = answer.value("replayed", false);
    installation.destination_honored = answer.value("destinationHonored", true);
    installation.destination = modelhub::from_utf8(text(answer, "destination"));
    installation.raw = std::move(answer);
    return installation;
}
} // namespace

Json Package::to_json() const {
    Json package = {{"files", files_json(files, true)}};
    const auto group = [&](const char* name,
                           std::initializer_list<std::pair<const char*, const std::string*>> fields) {
        Json object = Json::object();
        for (const auto& [key, value] : fields) set(object, key, *value);
        if (!object.empty()) package[name] = std::move(object);
    };
    group("identity",
          {{"name", &name}, {"repository", &repository}, {"revision", &revision}, {"variant", &variant}});
    group("provenance", {{"source", &source}, {"url", &url}, {"application", &application}});
    group("classification", {{"format", &format},
                             {"type", &type},
                             {"modality", &modality},
                             {"architecture", &architecture},
                             {"quantization", &quantization}});
    set(package, "entry", entry);
    if (dependencies.is_array() && !dependencies.empty()) package["dependencies"] = dependencies;
    return package;
}

Model model_from_json(const Json& value) {
    Model model;
    model.id = text(value, "id");
    model.name = text(value, "name");
    model.path = modelhub::from_utf8(text(value, "path"));
    model.type = text(value, "type");
    model.modality = text(value, "modality");
    model.format = text(value, "format");
    model.architecture = text(value, "architecture");
    model.quantization = text(value, "quantization");
    model.source = text(value, "source");
    model.repository = text(value, "repo");
    model.sha256 = text(value, "sha256");
    model.revision = text(value, "revision");
    model.variant = text(value, "variant");
    model.state = text(value, "state");
    model.integrity = text(value, "integrity");
    model.complete = value.value("complete", false);
    if (const auto tags = value.find("tags"); tags != value.end() && tags->is_array())
        for (const auto& tag : *tags)
            if (tag.is_string()) model.tags.push_back(tag.get<std::string>());
    model.size = value.value("size", std::uint64_t{0});
    model.folder = value.value("folder", false);
    model.missing = value.value("missing", false);
    model.favorite = value.value("favorite", false);
    model.unsafe = value.value("unsafe", false);
    model.raw = value;
    return model;
}

struct Client::Impl {
    Options options;
    std::mutex mutex;
    std::string url, token;
    int port = 0;

    fs::path data_directory() const {
        return options.data_directory.empty() ? modelhub::data_path() : options.data_directory;
    }

    // Read <data>/service.json. False when the service is not announced as running.
    bool read_descriptor(int& found_port, std::string& found_token, fs::path& executable) const {
        const auto content = modelhub::read_file(data_directory() / "service.json", 65536);
        const auto json = content ? Json::parse(*content, nullptr, false) : Json();
        if (!json.is_object()) return false;
        executable = modelhub::from_utf8(text(json, "executable"));
        found_token = text(json, "token");
        found_port = json.value("port", 0);
        return found_port > 0 && found_port <= 65535 && found_token.size() >= 32;
    }

    static bool healthy(int service_port) {
        try {
            modelhub::LocalRequest request;
            request.port = service_port;
            request.path = "/health";
            request.timeout = std::chrono::milliseconds(2000);
            return modelhub::local_http(request).status == 200;
        } catch (const std::exception&) {
            return false;
        }
    }

    void use(int service_port, std::string service_token) {
        port = service_port;
        token = std::move(service_token);
        url = "http://127.0.0.1:" + std::to_string(port);
    }

    fs::path service_executable(const fs::path& recorded) const {
#ifdef _WIN32
        const char* name = "CyModelHubService.exe";
#else
        const char* name = "CyModelHubService";
#endif
        std::error_code error;
        std::vector<fs::path> candidates{options.service_executable,
                                         modelhub::from_utf8(modelhub::environment("CYMODELHUB_SERVICE")),
                                         modelhub::executable_path().parent_path() / name, recorded};
        for (const auto& candidate : candidates)
            if (!candidate.empty() && fs::is_regular_file(candidate, error)) return candidate;
        return {};
    }

    // Caller holds the mutex.
    void connect() {
        int found_port = 0;
        std::string found_token;
        fs::path recorded;
        if (read_descriptor(found_port, found_token, recorded) && healthy(found_port))
            return use(found_port, std::move(found_token));
        url.clear();
        if (!options.start_service)
            throw Error("ServiceUnavailable", "The CyModelHub service is not running");
        const auto executable = service_executable(recorded);
        if (executable.empty())
            throw Error("ServiceUnavailable", "CyModelHubService was not found. Install CyModelHub, or set "
                                              "CYMODELHUB_SERVICE to its path");
        std::vector<std::string> arguments;
        if (!options.data_directory.empty())
            arguments = {"--data", modelhub::to_utf8(options.data_directory)};
        if (!modelhub::spawn_detached(executable, arguments))
            throw Error("ServiceUnavailable", "The CyModelHub service could not be started");
        const auto deadline = std::chrono::steady_clock::now() + options.start_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (read_descriptor(found_port, found_token, recorded) && healthy(found_port))
                return use(found_port, std::move(found_token));
        }
        throw Error("ServiceUnavailable", "The CyModelHub service did not answer in time");
    }

    // `again`: sending the request a second time cannot repeat an effect.
    Json post(const Json& body, bool again) {
        // A restarted service changes its address or token: reconnect once, then give up.
        for (int attempt = 0;; ++attempt) {
            modelhub::LocalRequest request;
            {
                std::lock_guard lock(mutex);
                if (url.empty()) connect();
                request.port = port;
                request.bearer = token;
            }
            request.method = "POST";
            request.path = "/v1";
            request.body = body.dump(-1, ' ', false, Json::error_handler_t::replace);
            request.timeout = options.request_timeout;
            request.max_response_bytes = std::size_t{256} * 1024 * 1024;
            modelhub::LocalResponse response;
            try {
                response = modelhub::local_http(request);
            } catch (const modelhub::Failure& error) {
                // A request that never left can always be sent again. One that left may have
                // been carried out although its answer was lost.
                if (attempt == 0 && (error.code == "NetworkError" || error.code == "Timeout") &&
                    (again || !error.request_sent)) {
                    std::lock_guard lock(mutex);
                    url.clear();
                    continue;
                }
                throw Error(error.code, error.what());
            }
            if (response.status == 401 && attempt == 0) {
                std::lock_guard lock(mutex);
                url.clear();
                continue;
            }
            const auto reply = Json::parse(response.body, nullptr, false);
            if (response.status == 200 && reply.is_object()) return reply;
            if (reply.is_object() && reply.contains("error") && reply["error"].is_object()) {
                const auto& error = reply["error"];
                throw Error(text(error, "code").empty() ? "ServiceError" : text(error, "code"),
                            text(error, "message").empty() ? "The service refused the request"
                                                           : text(error, "message"));
            }
            throw Error("ServiceError", "Unexpected answer from the CyModelHub service (HTTP " +
                                            std::to_string(response.status) + ")");
        }
    }
};

Client::Client(Options options) : impl_(std::make_unique<Impl>()) { impl_->options = std::move(options); }
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

void Client::connect() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->url.empty() || !Impl::healthy(impl_->port)) impl_->connect();
}

bool Client::available() noexcept {
    try {
        connect();
        return true;
    } catch (...) {
        return false;
    }
}

std::string Client::endpoint() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->url;
}

Json Client::invoke(const std::string& operation, const Json& arguments) {
    return impl_->post({{"method", "invoke"}, {"name", operation}, {"arguments", arguments}},
                       repeatable(operation, arguments));
}

Json Client::describe() { return impl_->post({{"method", "describe"}}, true); }

std::string Client::new_idempotency_key() {
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device random;
    std::string key = "k";
    for (int index = 0; index < 8; ++index) {
        auto value = random();
        for (int nibble = 0; nibble < 4; ++nibble, value >>= 4) key += digits[value & 15];
    }
    return key;
}

Resolution Client::check(const Requirement& requirement) {
    Json arguments = Json::object();
    for (const auto& [key, value] :
         {std::pair{"id", &requirement.id}, std::pair{"repository", &requirement.repository},
          std::pair{"name", &requirement.name}, std::pair{"file", &requirement.file},
          std::pair{"sha256", &requirement.sha256}, std::pair{"revision", &requirement.revision},
          std::pair{"variant", &requirement.variant}, std::pair{"format", &requirement.format},
          std::pair{"architecture", &requirement.architecture},
          std::pair{"quantization", &requirement.quantization}, std::pair{"type", &requirement.type},
          std::pair{"modality", &requirement.modality}, std::pair{"integrity", &requirement.integrity}})
        set(arguments, key, *value);
    if (!requirement.files.empty()) arguments["files"] = files_json(requirement.files, false);
    auto answer = invoke("models_check", arguments);
    Resolution resolution;
    resolution.status_name = text(answer, "status");
    static constexpr std::pair<const char*, Availability> names[] = {
        {"ready", Availability::ready},
        {"absent", Availability::absent},
        {"installing", Availability::installing},
        {"incomplete", Availability::incomplete},
        {"invalid", Availability::invalid},
        {"unverified", Availability::unverified},
        {"incompatible", Availability::incompatible},
        {"ambiguous", Availability::ambiguous}};
    // A status this library does not know yet must never read as "ready".
    resolution.status = Availability::invalid;
    for (const auto& [name, status] : names)
        if (resolution.status_name == name) resolution.status = status;
    if (const auto model = answer.find("model"); model != answer.end() && model->is_object())
        resolution.model = model_from_json(*model);
    resolution.path = modelhub::from_utf8(text(answer, "path"));
    resolution.root = modelhub::from_utf8(text(answer, "root"));
    resolution.entry = modelhub::from_utf8(text(answer, "entry"));
    resolution.job = text(answer, "job");
    if (const auto reasons = answer.find("reasons"); reasons != answer.end() && reasons->is_array())
        for (const auto& reason : *reasons)
            if (reason.is_string()) resolution.reasons.push_back(reason.get<std::string>());
    resolution.problems = answer.value("problems", Json::array());
    resolution.candidates = answer.value("candidates", Json::array());
    resolution.raw = std::move(answer);
    return resolution;
}

Installation Client::install(InstallRequest request) {
    if (request.idempotency_key.empty()) request.idempotency_key = new_idempotency_key();
    Json arguments = {{"idempotencyKey", request.idempotency_key}};
    set(arguments, "url", request.url);
    set(arguments, "repository", request.repository);
    set(arguments, "file", request.file);
    set(arguments, "revision", request.revision);
    set(arguments, "variant", request.variant);
    set(arguments, "sha256", request.sha256);
    set(arguments, "baseUrl", request.base_url);
    set(arguments, "filename", request.filename);
    set(arguments, "token", request.token);
    if (!request.destination.empty()) arguments["destination"] = modelhub::to_utf8(request.destination);
    if (request.package) arguments["package"] = request.package->to_json();
    return installation_from_json(invoke("install_start", arguments), std::move(request.idempotency_key));
}

Installation Client::register_model(const std::filesystem::path& root, const Package& package, bool hashes,
                                    std::string idempotency_key) {
    if (idempotency_key.empty()) idempotency_key = new_idempotency_key();
    const Json arguments = {{"idempotencyKey", idempotency_key},
                            {"root", modelhub::to_utf8(std::filesystem::absolute(root))},
                            {"package", package.to_json()},
                            {"verify", hashes ? "hashes" : "sizes"}};
    return installation_from_json(invoke("models_register", arguments), std::move(idempotency_key));
}

Installation Client::verify(const std::string& id, bool hashes) {
    auto key = new_idempotency_key();
    const Json arguments = {{"idempotencyKey", key}, {"id", id}, {"level", hashes ? "hashes" : "sizes"}};
    return installation_from_json(invoke("models_verify", arguments), std::move(key));
}

Model Client::wait(const Installation& installation, std::chrono::milliseconds timeout,
                   const std::function<void(const Json&)>& progress) {
    const auto job = wait_job(installation.job, timeout, progress);
    const auto state = text(job, "state");
    if (state != "done") {
        auto code = text(job, "code");
        if (code.empty())
            code = state == "cancelled" ? "Cancelled" : state == "interrupted" ? "Interrupted" : "JobFailed";
        throw Error(code, text(job, "message").empty() ? "The job did not complete" : text(job, "message"));
    }
    const auto result = job.find("result");
    if (result == job.end() || !result->is_object() || !result->contains("model"))
        throw Error("JobFailed", "The job gave no model");
    return model_from_json(result->at("model"));
}

bool Client::leave(const Installation& installation) {
    if (installation.ticket.empty()) return false;
    return invoke("install_leave", {{"job", installation.job}, {"ticket", installation.ticket}})
        .value("cancelled", false);
}

std::vector<Model> Client::list(const Query& query, std::size_t* total) {
    Json arguments = {{"sort", query.sort}, {"descending", query.descending}, {"offset", query.offset},
                      {"limit", query.limit}};
    set(arguments, "query", query.text);
    set(arguments, "type", query.type);
    set(arguments, "modality", query.modality);
    set(arguments, "format", query.format);
    set(arguments, "architecture", query.architecture);
    set(arguments, "source", query.source);
    set(arguments, "tag", query.tag);
    const auto reply = invoke("models_list", arguments);
    if (total) *total = reply.value("total", std::size_t{0});
    std::vector<Model> models;
    if (const auto items = reply.find("models"); items != reply.end() && items->is_array())
        for (const auto& item : *items) models.push_back(model_from_json(item));
    return models;
}

std::optional<Model> Client::get(const std::string& id) {
    try {
        return model_from_json(invoke("models_get", {{"id", id}}).at("model"));
    } catch (const Error& error) {
        if (error.code == "NotFound") return std::nullopt;
        throw;
    }
}

std::optional<Model> Client::resolve(const std::string& wanted, const std::string& type, const std::string& modality) {
    Json arguments = {{"query", wanted}};
    set(arguments, "type", type);
    set(arguments, "modality", modality);
    const auto reply = invoke("models_resolve", arguments);
    if (!reply.value("found", false)) return std::nullopt;
    return model_from_json(reply.at("model"));
}

std::filesystem::path Client::path_of(const std::string& wanted, const std::string& type, const std::string& modality) {
    const auto model = resolve(wanted, type, modality);
    return model ? model->path : std::filesystem::path();
}

Json Client::wait_job(const std::string& id, std::chrono::milliseconds timeout,
                      const std::function<void(const Json&)>& progress) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto job = invoke("jobs_get", {{"id", id}}).at("job");
        if (progress) progress(job);
        if (text(job, "state") != "running") return job;
        if (std::chrono::steady_clock::now() >= deadline) throw Error("Timeout", "The job is still running");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

Json Client::mcp_configuration() {
    connect();
    std::lock_guard lock(impl_->mutex);
    Json server = Json::object();
    server["type"] = "http";
    server["url"] = impl_->url + "/mcp";
    server["headers"]["Authorization"] = "Bearer " + impl_->token;
    Json configuration = Json::object();
    configuration["mcpServers"]["cymodelhub"] = std::move(server);
    return configuration;
}
} // namespace cymodelhub
