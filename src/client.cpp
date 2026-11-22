// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

#include <cymodelhub/client.hpp>

#include "platform.hpp"
#include <cymcp/http.hpp>
#include <mutex>
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
} // namespace

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

    fs::path data_directory() const {
        return options.data_directory.empty() ? modelhub::data_path() : options.data_directory;
    }

    // Read <data>/service.json. False when the service is not announced as running.
    bool read_descriptor(std::string& found_url, std::string& found_token, fs::path& executable) const {
        const auto content = modelhub::read_file(data_directory() / "service.json", 65536);
        const auto json = content ? Json::parse(*content, nullptr, false) : Json();
        if (!json.is_object()) return false;
        executable = modelhub::from_utf8(text(json, "executable"));
        found_token = text(json, "token");
        const auto port = json.value("port", 0);
        if (port <= 0 || port > 65535 || found_token.size() < 32) return false;
        found_url = "http://127.0.0.1:" + std::to_string(port);
        return true;
    }

    static bool healthy(const std::string& address) {
        try {
            cymcp::HttpRequest request;
            request.url = address + "/health";
            request.timeout = std::chrono::milliseconds(2000);
            return cymcp::HttpClient().perform(request).status == 200;
        } catch (const std::exception&) {
            return false;
        }
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
        std::string found_url, found_token;
        fs::path recorded;
        if (read_descriptor(found_url, found_token, recorded) && healthy(found_url)) {
            url = std::move(found_url);
            token = std::move(found_token);
            return;
        }
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
            if (read_descriptor(found_url, found_token, recorded) && healthy(found_url)) {
                url = std::move(found_url);
                token = std::move(found_token);
                return;
            }
        }
        throw Error("ServiceUnavailable", "The CyModelHub service did not answer in time");
    }

    Json post(const Json& body) {
        // A restarted service changes its address or token: reconnect once, then give up.
        for (int attempt = 0;; ++attempt) {
            std::string address, credential;
            {
                std::lock_guard lock(mutex);
                if (url.empty()) connect();
                address = url;
                credential = token;
            }
            cymcp::HttpRequest request;
            request.method = "POST";
            request.url = address + "/v1";
            request.body = body.dump(-1, ' ', false, Json::error_handler_t::replace);
            request.headers["Content-Type"] = "application/json";
            request.headers["Authorization"] = "Bearer " + credential;
            request.timeout = options.request_timeout;
            request.max_response_bytes = std::size_t{256} * 1024 * 1024;
            cymcp::HttpResponse response;
            try {
                response = cymcp::HttpClient().perform(request);
            } catch (const cymcp::Error& error) {
                if (attempt == 0 && (error.code == "NetworkError" || error.code == "Timeout")) {
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
    if (impl_->url.empty() || !Impl::healthy(impl_->url)) impl_->connect();
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
    return impl_->post({{"method", "invoke"}, {"name", operation}, {"arguments", arguments}});
}

Json Client::describe() { return impl_->post({{"method", "describe"}}); }

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
