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
//
// For an application that needs one exact model, use the strict path instead of resolve():
//
//     cymodelhub::Requirement need;
//     need.repository = "org/name";
//     need.files = {{"model.gguf", 4368439584, "9f2c..."}};
//     auto found = hub.check(need);
//     if (found.ready()) load(found.entry);
//     else if (found.status == cymodelhub::Availability::absent ||
//              found.status == cymodelhub::Availability::installing)
//         load(hub.wait(hub.install(request)).path);   // joins an installation in progress
//
// docs/INTEGRATION.md describes the whole path.

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
    // Stable identifier, e.g. ServiceUnavailable, NotFound, InvalidPath, AlreadyExists,
    // IdempotencyConflict, ProviderAccessRequired, HashMismatch, ValidationFailed, Cancelled.
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
    std::string revision, variant; // of the source it was installed or registered from
    // state: missing; ready, incomplete or invalid after a check; "present" when only seen.
    // integrity: none, sizes, recorded (hashes computed locally) or verified (hashes matched).
    std::string state, integrity;
    std::vector<std::string> tags;
    std::uint64_t size = 0;
    bool folder = false, missing = false, favorite = false;
    bool complete = false; // a complete model: its files are listed in raw["package"]
    bool unsafe = false;   // pickle-based file: loading it can run code
    Json raw;              // the complete record, including fields this struct does not name
};

// One file of a complete model: a path relative to the model folder, with forward slashes.
struct ModelFile {
    std::string path;
    std::optional<std::uint64_t> size; // bytes, when known
    std::string sha256;                // 64 hex digits, when known
    std::string role;                  // weights, shard, index, config, tokenizer, manifest...
    bool required = true;
};

// A complete model: every file an engine needs to run it, with where it comes from. Sent to
// install() to have CyModelHub download it, or to register_model() for a folder the
// application already has.
struct Package {
    std::string name, repository, revision, variant;                // identity
    std::string source, url, application;                           // provenance
    std::string format, type, modality, architecture, quantization; // classification, optional
    std::string entry; // file the engine opens, among `files`; empty when it takes the folder
    std::vector<ModelFile> files;
    Json dependencies = Json::array(); // [{name, repository, revision, variant, sha256, required}]
    Json to_json() const;
};

// What an application needs, for check(). Give what identifies the model exactly (id, or
// repository, name, file, sha256) and what it must be. Empty fields are not looked at.
struct Requirement {
    std::string id, repository, name, file, sha256;
    std::string revision, variant, format, architecture, quantization, type, modality;
    std::vector<ModelFile> files; // files the application will open, with expected sizes and hashes
    // Least evidence accepted: "none" (being on disk is enough), "sizes" (the expected sizes are
    // known and match), "hashes" (the SHA-256 of the files was checked, and they did not change).
    std::string integrity = "sizes";
};

enum class Availability {
    ready,        // use Resolution::path or ::entry
    absent,       // not installed: install it, or download it and register it
    installing,   // someone is installing it: install() joins, see Resolution::job
    incomplete,   // required files are missing
    invalid,      // a size or a hash does not match: the validation failed
    unverified,   // present, but the evidence asked for was never gathered: verify()
    incompatible, // installed, but not what was asked: see Resolution::reasons
    ambiguous     // several different models answer: see Resolution::candidates
};

struct Resolution {
    Availability status = Availability::absent;
    std::string status_name;
    std::optional<Model> model;
    std::filesystem::path path;  // the file, or the folder of a complete model
    std::filesystem::path root;  // the folder that holds the files
    std::filesystem::path entry; // what the engine opens
    std::string job;             // the installation in progress, when installing
    std::vector<std::string> reasons;
    Json problems = Json::array(), candidates = Json::array(), raw;
    bool ready() const { return status == Availability::ready; }
};

// One file (a url, or a repository and a file) or a complete model (a package).
struct InstallRequest {
    std::string url, repository, file, revision, variant, sha256;
    std::optional<Package> package;
    std::string base_url;              // with a package: its files are at base_url/path
    std::filesystem::path destination; // inside a registered location; empty: CyModelHub's store
    std::string filename;              // name of the file, or of the folder of a complete model
    std::string token;                 // for gated downloads; sent to the provider only, never stored
    // Sent again unchanged when the answer is lost, so the request has one effect. Empty: a new
    // key is made. Reuse a key only to repeat exactly the same request.
    std::string idempotency_key;
};

// The answer to install(), register_model() or verify(): a job, and a ticket to stop waiting
// for it without stopping it for the others.
struct Installation {
    std::string job, ticket, artifact, idempotency_key;
    bool joined = false;             // someone else had started this installation
    bool resumed = false;            // continues what an earlier run of the service left unfinished
    bool installed = false;          // nothing to do: the model was already there
    bool replayed = false;           // answer remembered for this idempotency key
    bool destination_honored = true; // false: the model is, or will be, at `destination` instead
    std::filesystem::path destination;
    Json raw;
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

    // Strict path. Never picks "the closest" model: the answer says whether exactly the model
    // asked for is ready, and otherwise why not.
    Resolution check(const Requirement& requirement);
    // Have CyModelHub install a model, or join the installation someone else started.
    Installation install(InstallRequest request);
    // Catalogue a complete model the application has on disk, where it is: no copy, no move.
    // Every file of the package is checked; with `hashes` its SHA-256 is computed.
    Installation register_model(const std::filesystem::path& root, const Package& package, bool hashes = true,
                                std::string idempotency_key = {});
    // Check an installed model again: presence and sizes, and SHA-256 with `hashes`.
    Installation verify(const std::string& id, bool hashes = true);
    // Wait for the job and return the model it produced. Look at Model::state after a
    // registration or a verification. Throws Error with the code of the failure, e.g.
    // ProviderAccessRequired, HashMismatch, ValidationFailed, ModelIncomplete, Cancelled,
    // Interrupted, Timeout.
    Model wait(const Installation& installation, std::chrono::milliseconds timeout = std::chrono::hours(24),
               const std::function<void(const Json&)>& progress = {});
    // Stop waiting. The work continues for the other requesters and is cancelled only when
    // the last one leaves. Returns true when it was cancelled.
    bool leave(const Installation& installation);
    // A fresh random key for an operation that must not happen twice.
    static std::string new_idempotency_key();

    // Any operation of the service by name (see describe()), e.g. "roots_add", "install_start".
    // After a network error or a timeout the request is sent again once, but only when that
    // cannot repeat an effect: it never left this program, the operation only reads, or the
    // arguments carry an "idempotencyKey". Otherwise the error reaches the caller.
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
