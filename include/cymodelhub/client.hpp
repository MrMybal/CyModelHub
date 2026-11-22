// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

// CyModelHub client library.
//
// Link CyModelHub::Client and ask the local CyModelHub service where a model is installed,
// instead of shipping or downloading your own copy:
//
//     cymodelhub::Client hub;
//     if (auto model = hub.resolve("sd_xl_base_1.0", "model", "image"))
//         load(model->path);
//
// The service is started on demand. Every call may throw cymodelhub::Error.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cymodelhub {
using Json = nlohmann::json;

class Error : public std::runtime_error {
  public:
    // Stable identifier, e.g. ServiceUnavailable, NotFound, InvalidPath, AlreadyExists.
    std::string code;
    Error(std::string error_code, const std::string& message) : std::runtime_error(message), code(std::move(error_code)) {}
};

struct Model {
    std::string id, name;
    std::filesystem::path path; // absolute path of the file, or of the folder when `folder` is set
    // type: model, lora, vae, controlnet, text_encoder, vision_encoder, embedding, upscaler, adapter...
    // modality: text, image, video, audio, embedding, multimodal, 3d, other
    // format: gguf, safetensors, pickle, onnx, diffusers, transformers, mlx...
    std::string type, modality, format, architecture, quantization, source, repository, sha256;
    std::vector<std::string> tags;
    std::uint64_t size = 0;
    bool folder = false, missing = false, favorite = false;
    bool unsafe = false; // pickle-based file: loading it can run code
    Json raw;            // the complete record, including fields this struct does not name
};

struct Query {
    std::string text; // words that must all match
    std::string type, modality, format, architecture, source, tag;
    std::string sort = "name"; // name, size, modified, added, type
    bool descending = false;
    std::size_t offset = 0, limit = 100; // limit: 1 to 500
};

struct Options {
    // Start the service when it is not running.
    bool start_service = true;
    // Where CyModelHubService is. Empty: CYMODELHUB_SERVICE, then next to this program, then
    // the place recorded by the last run of the service.
    std::filesystem::path service_executable;
    // Empty: the per-user CyModelHub data folder.
    std::filesystem::path data_directory;
    std::chrono::milliseconds start_timeout{15000};
    std::chrono::milliseconds request_timeout{60000};
};

class Client {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit Client(Options options = {});
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    // Reach the service, starting it if allowed. Called implicitly by the first request.
    void connect();
    // Same, without throwing.
    bool available() noexcept;
    // Address of the service, e.g. http://127.0.0.1:48787. Empty before a connection.
    std::string endpoint() const;

    // Models installed on this computer.
    std::vector<Model> list(const Query& query = {}, std::size_t* total = nullptr);
    std::optional<Model> get(const std::string& id);
    // Best installed match for a name, file name, repository, SHA-256 or ID.
    std::optional<Model> resolve(const std::string& wanted, const std::string& type = {},
                                 const std::string& modality = {});
    // Path of the best match, or an empty path when the model is not installed.
    std::filesystem::path path_of(const std::string& wanted, const std::string& type = {},
                                  const std::string& modality = {});

    // Any operation of the service by name (see describe()), e.g. "roots_add", "install_start".
    Json invoke(const std::string& operation, const Json& arguments = Json::object());
    // Catalogue of the operations with their JSON schemas.
    Json describe();
    // Wait for a background job. Returns its final record; `progress` sees each update.
    Json wait_job(const std::string& id, std::chrono::milliseconds timeout = std::chrono::hours(24),
                  const std::function<void(const Json&)>& progress = {});
    // Ready-to-paste MCP client configuration for the HTTP endpoint of the service.
    Json mcp_configuration();
};

Model model_from_json(const Json& value);
} // namespace cymodelhub
