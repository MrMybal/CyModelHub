// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// Complete models, the .fermion format, strict resolution, idempotency keys and installation
// reservations, in one process and inside a sandbox: no model of the person running the test
// is read.

#include "fixtures.hpp"

#include "coordination.hpp"
#include "formats.hpp"
#include "package.hpp"
#include "service.hpp"

#include <thread>

using namespace modelhub;
using fixtures::check;

namespace {
Json wait(Hub& hub, const Json& answer) {
    for (int i = 0; i < 1500; ++i) {
        const auto job = hub.jobs.get(answer.at("job").get<std::string>());
        if (job.at("state") != "running") return job;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {{"state", "timeout"}};
}

std::string code_of(const cymcp::Registry& registry, const char* operation, const Json& arguments) {
    try {
        registry.invoke(operation, arguments);
    } catch (const cymcp::Error& error) {
        return error.code;
    }
    return "accepted";
}

Json named(const cymcp::Registry& registry, const std::string& name) {
    const auto listed = registry.invoke("models_list", {{"limit", 500}});
    for (const auto& model : listed.at("models"))
        if (model.value("name", std::string()) == name) return model;
    return Json::object();
}

std::string text(const Json& value, const char* key) { return value.value(key, std::string()); }

bool mentions(const Json& problems, const std::string& path, const std::string& kind) {
    for (const auto& issue : problems)
        if (text(issue, "path") == path && text(issue, "problem") == kind) return true;
    return false;
}

Json file_entry(const fs::path& folder, const std::string& name, const char* role) {
    const auto content = *read_file(folder / from_utf8(name));
    return {{"path", name}, {"size", content.size()}, {"sha256", cymcp::sha256(content)}, {"role", role}};
}

// Names, sizes and times of everything in a folder: what "nothing was copied or moved" means.
std::string listing(const fs::path& folder) {
    std::vector<std::string> lines;
    for (fs::recursive_directory_iterator item(folder), end; item != end; ++item)
        if (item->is_regular_file())
            lines.push_back(to_utf8(item->path().lexically_relative(folder)) + ':' +
                            std::to_string(item->file_size()) + ':' +
                            std::to_string(file_time_seconds(item->last_write_time())));
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (const auto& line : lines) joined += line + '\n';
    return joined;
}

void touch_later(const fs::path& path) {
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(30));
}
} // namespace

int main() {
    const auto root = fixtures::sandbox("complete");
    const auto library = root / "library";
    const auto speech = library / "speech";

    // --- Paths of a contract -----------------------------------------------------------------
    for (const auto* good : {"model.fermion", "unet/diffusion_pytorch_model.safetensors", "a b/c-1_2.bin"})
        check(safe_relative_path(good), std::string("accepted path: ") + good);
    for (const auto* bad : {"", "../escape.bin", "a/../../b", "/etc/passwd", "C:/x.bin", "a\\b.bin", "a//b",
                            "a/./b", "dir/", "con.txt", "x.", "a/ b", "a:b"})
        check(!safe_relative_path(bad), std::string("refused path: ") + bad);

    // --- The .fermion container --------------------------------------------------------------
    const auto container = fixtures::fermion_container();
    const auto loose = root / "containers";
    const auto inspect = [&](const std::string& name, const std::string& content) {
        fixtures::write(loose / name, content);
        return inspect_fermion(loose / name);
    };
    auto seen = inspect("good.fermion", container);
    check(seen.valid && seen.records == 4 && seen.format == fermion_parakeet_format, "valid container");
    check(seen.kinds["fp16"] == 2 && seen.kinds["int6"] == 1 && seen.kinds["five_value"] == 1,
          "record kinds counted");
    check(seen.file_bytes == 8 + seen.header_bytes + seen.payload_bytes, "size is prefix + index + records");
    seen = inspect("truncated.fermion", container.substr(0, container.size() - 5));
    check(!seen.valid && seen.problem.find("truncated") != std::string::npos, "truncated container refused");
    seen = inspect("trailing.fermion", container + "x");
    check(!seen.valid && seen.problem.find("follow") != std::string::npos, "trailing bytes refused");
    check(!inspect("tiny.fermion", "abc").valid, "a file shorter than its length field is refused");
    check(!inspect("cut-index.fermion", container.substr(0, 40)).valid,
          "an index that does not fit is refused");
    check(!inspect("gguf.fermion", fixtures::gguf_llm("Not Fermion")).valid,
          "a GGUF file is not a Fermion container");
    auto records = fixtures::fermion_index();
    records[0]["b"] = 40;
    check(!inspect("five.fermion", fixtures::fermion_container(records)).valid,
          "impossible five_value length refused");
    records = fixtures::fermion_index();
    records[2]["b"] = 2046;
    check(!inspect("fp16.fermion", fixtures::fermion_container(records)).valid, "wrong fp16 length refused");
    records = fixtures::fermion_index();
    records[1]["b"] = 9;
    check(!inspect("int6.fermion", fixtures::fermion_container(records)).valid, "wrong int6 length refused");
    records = fixtures::fermion_index();
    records[1]["k"] = "float128";
    check(!inspect("kind.fermion", fixtures::fermion_container(records)).valid,
          "unknown record kind refused");
    seen = inspect("future.fermion", fixtures::fermion_container(records, "fermion-future-v9"));
    check(seen.valid && seen.format == "fermion-future-v9",
          "another fermion format is checked for its size only");
    check(!inspect("other.fermion", fixtures::fermion_container(fixtures::fermion_index(), "safetensors"))
               .valid,
          "an index that announces another format is refused");

    // --- Fermion models in a watched location --------------------------------------------------
    fixtures::fermion_folder(speech / "good", container, "Phonon-Test");
    fixtures::fermion_folder(speech / "truncated", container, "Phonon-Truncated");
    fixtures::write(speech / "truncated" / "model.fermion", container.substr(0, container.size() - 5));
    fixtures::fermion_folder(speech / "size", container, "Phonon-Size",
                             {{"container_bytes", container.size() + 1}});
    fixtures::fermion_folder(speech / "disagree", container, "Phonon-Disagree", Json::object(),
                             {{"container_sha256", std::string(64, 'b')}});
    fixtures::fermion_folder(speech / "format", container, "Phonon-Format", {{"format", "fermion-other-v2"}});
    fixtures::fermion_folder(speech / "badhash", container, "Phonon-BadHash",
                             {{"container_sha256", std::string(64, 'c')}},
                             {{"container_sha256", std::string(64, 'c')}});
    fixtures::fermion_folder(speech / "noconfig", container, "Phonon-NoConfig");
    fs::remove(speech / "noconfig" / "config.json");
    fixtures::write(library / "lone" / "model.fermion", container);
    const auto probe = probe_folder(speech / "good");
    check(probe && probe->format == "fermion" && probe->architecture == "parakeet" &&
              probe->modality == "audio" && probe->quantization == "five-value-int6" &&
              probe->name == "Phonon-Test",
          "Fermion folder classified from its manifest and configuration");
    check(probe && probe->package.at("files").size() == 3,
          "a Fermion model is its container, configuration and manifest");

    auto hub = std::make_shared<Hub>(data_path());
    auto registry = make_registry(hub);
    check(wait(*hub, registry->invoke("roots_add", {{"path", to_utf8(library)}})).at("state") == "done",
          "scan");

    auto good = named(*registry, "Phonon-Test");
    check(good.value("folder", false) && good.value("complete", false) && text(good, "format") == "fermion" &&
              text(good, "state") == "ready" && text(good, "integrity") == "sizes",
          "valid Fermion model: one entry, ready, sizes known from its manifest");
    check(text(named(*registry, "Phonon-Truncated"), "state") == "invalid", "truncated container: invalid");
    check(text(named(*registry, "Phonon-Size"), "state") == "invalid",
          "manifest that announces another size: invalid");
    check(text(named(*registry, "Phonon-Disagree"), "state") == "invalid",
          "configuration and manifest that disagree: invalid");
    check(text(named(*registry, "Phonon-Format"), "state") == "invalid",
          "manifest and container that announce different formats: invalid");
    check(text(named(*registry, "noconfig"), "state") == "incomplete", "missing configuration: incomplete");
    const auto lone = named(*registry, "model");
    check(!lone.value("folder", true) && text(lone, "format") == "fermion" &&
              text(lone, "state") == "incomplete",
          "a container alone is catalogued but not ready");
    check(registry->invoke("models_list", {{"format", "fermion"}}).at("total") == 8,
          "one entry per Fermion model");

    // Strict resolution of a Fermion model.
    const Json need = {{"name", "Phonon-Test"}, {"format", "fermion"}, {"architecture", "parakeet"}};
    auto answer = registry->invoke("models_check", need);
    check(text(answer, "status") == "ready" && from_utf8(text(answer, "path")) == speech / "good" &&
              from_utf8(text(answer, "entry")) == speech / "good",
          "ready: the path given back is the folder the engine opens");
    auto strict = need;
    strict["integrity"] = "hashes";
    check(text(registry->invoke("models_check", strict), "status") == "unverified",
          "hashes asked before any was checked: unverified");
    check(wait(*hub, registry->invoke("models_verify", {{"id", good.at("id")}})).at("state") == "done",
          "verify");
    check(text(registry->invoke("models_check", strict), "status") == "ready", "hashes checked: ready");
    strict["files"] = Json::array(
        {{{"path", "model.fermion"}, {"sha256", cymcp::sha256(container)}, {"size", container.size()}}});
    check(text(registry->invoke("models_check", strict), "status") == "ready",
          "expected container hash matches");
    strict["files"][0]["sha256"] = std::string(64, 'd');
    check(text(registry->invoke("models_check", strict), "status") == "invalid",
          "another expected hash: invalid");
    auto other = need;
    other["format"] = "gguf";
    answer = registry->invoke("models_check", other);
    check(text(answer, "status") == "incompatible" && !answer.at("reasons").empty(),
          "another format: incompatible");
    check(text(registry->invoke("models_check", {{"name", "Phonon-Truncated"}}), "status") == "invalid",
          "strict check of a damaged model: invalid");
    check(text(registry->invoke("models_check", {{"name", "noconfig"}}), "status") == "incomplete",
          "strict check of a model without its configuration: incomplete");
    check(text(registry->invoke("models_check", {{"id", lone.at("id")}, {"integrity", "none"}}), "status") ==
              "incomplete",
          "strict check of a lone container: incomplete");
    // A manifest whose hash is wrong passes the size check and fails the real one.
    const auto bad_hash = named(*registry, "Phonon-BadHash");
    check(text(bad_hash, "state") == "ready", "sizes alone do not see a wrong hash");
    auto verified = wait(*hub, registry->invoke("models_verify", {{"id", bad_hash.at("id")}}));
    check(verified.at("state") == "done" && text(verified.at("result").at("model"), "state") == "invalid" &&
              mentions(verified.at("result").at("model").at("validation").at("problems"), "model.fermion",
                       "hash"),
          "verification finds the container that does not match its manifest");
    check(text(registry->invoke("models_check", {{"name", "Phonon-BadHash"}}), "status") == "invalid",
          "and the strict check then says invalid");

    // --- A complete model an application already has -------------------------------------------
    const auto owned = root / "apps" / "tool" / "models" / "nova";
    fixtures::write(owned / "weights-00001-of-00002.gguf", fixtures::gguf_llm("Nova Pack"));
    fixtures::write(owned / "weights-00002-of-00002.gguf", std::string(3000, 'n'));
    fixtures::write(owned / "config.json", R"({"context": 8192})");
    fixtures::write(owned / "tokenizer" / "tokenizer.json", R"({"tokens": ["a", "b"]})");
    const std::string commit = "0123456789abcdef0123456789abcdef01234567";
    const auto contract = [&](const fs::path& folder) {
        return Json{{"identity",
                     {{"name", "Nova Pack"},
                      {"repository", "acme/nova-pack"},
                      {"revision", commit},
                      {"variant", "q4"}}},
                    {"provenance",
                     {{"application", "CyToolsCore"}, {"url", "https://example.com/nova?token=SECRET"}}},
                    {"classification",
                     {{"format", "gguf"},
                      {"type", "model"},
                      {"modality", "text"},
                      {"architecture", "llama"},
                      {"quantization", "Q4_K_M"}}},
                    {"entry", "weights-00001-of-00002.gguf"},
                    {"files", Json::array({file_entry(folder, "weights-00001-of-00002.gguf", "weights"),
                                           file_entry(folder, "weights-00002-of-00002.gguf", "shard"),
                                           file_entry(folder, "config.json", "config"),
                                           file_entry(folder, "tokenizer/tokenizer.json", "tokenizer")})},
                    {"dependencies", Json::array({{{"repository", "acme/nova-vae"}, {"required", false}}})}};
    };
    const auto package = contract(owned);
    const auto before = listing(owned);
    auto registered =
        wait(*hub, registry->invoke("models_register", {{"root", to_utf8(owned)}, {"package", package}}));
    auto model = registered.at("state") == "done" ? registered.at("result").at("model") : Json::object();
    check(text(model, "state") == "ready" && text(model, "integrity") == "verified" &&
              model.value("complete", false),
          "registered complete model: ready, hashes verified");
    check(from_utf8(text(model, "path")) == owned && listing(owned) == before,
          "registered where it is: nothing copied, moved or rewritten");
    check(!fs::exists(data_path() / "models" / "nova"), "nothing was put in the store of the Hub");
    check(text(model, "repo") == "acme/nova-pack" && text(model, "revision") == commit &&
              text(model, "variant") == "q4" && text(model, "architecture") == "llama",
          "identity and classification come from the contract");
    check(text(model.at("package").at("provenance"), "url") == "https://example.com/nova",
          "a query in the address of the source is not kept");
    check(model.at("package").at("dependencies").size() == 1, "dependencies are recorded");

    Json wanted = {{"repository", "acme/nova-pack"}, {"revision", commit}, {"variant", "q4"},
                   {"quantization", "Q4_K_M"},       {"format", "gguf"},   {"integrity", "hashes"},
                   {"files", package.at("files")}};
    for (auto& file : wanted["files"]) file.erase("role");
    answer = registry->invoke("models_check", wanted);
    check(text(answer, "status") == "ready" && from_utf8(text(answer, "root")) == owned &&
              from_utf8(text(answer, "entry")) == owned / "weights-00001-of-00002.gguf",
          "strict check: ready, with the shared folder and the file to open");
    auto different = wanted;
    different["revision"] = "fedcba9876543210fedcba9876543210fedcba98";
    answer = registry->invoke("models_check", different);
    check(text(answer, "status") == "incompatible" &&
              answer.at("reasons").dump().find("revision") != std::string::npos,
          "another revision: incompatible, with the reason");
    different = wanted;
    different["variant"] = "q8";
    check(text(registry->invoke("models_check", different), "status") == "incompatible",
          "another variant: incompatible");
    different = wanted;
    different["quantization"] = "Q8_0";
    check(text(registry->invoke("models_check", different), "status") == "incompatible",
          "another quantization: incompatible");
    check(text(registry->invoke("models_check", {{"repository", "acme/unknown"}}), "status") == "absent",
          "absent");
    check(code_of(*registry, "models_check", Json::object()) == "InvalidArguments",
          "a check needs an identity");

    // A file that disappears, then one that is damaged without changing size.
    const auto tokenizer = owned / "tokenizer" / "tokenizer.json";
    const auto original = *read_file(tokenizer);
    fs::remove(tokenizer);
    answer = registry->invoke("models_check", wanted);
    check(text(answer, "status") == "incomplete" &&
              mentions(answer.at("problems"), "tokenizer/tokenizer.json", "missing"),
          "a required file that vanished: incomplete, at once");
    auto damaged = original;
    damaged[damaged.size() / 2] = '#';
    fixtures::write(tokenizer, damaged);
    touch_later(tokenizer);
    check(text(registry->invoke("models_check", wanted), "status") == "unverified",
          "a file rewritten since its hash was checked: no longer verified");
    verified = wait(*hub, registry->invoke("models_verify", {{"id", model.at("id")}}));
    check(text(verified.at("result").at("model"), "state") == "invalid",
          "verification finds the damaged file");
    check(text(registry->invoke("models_check", wanted), "status") == "invalid",
          "strict check after it: invalid");
    fixtures::write(tokenizer, original);
    verified = wait(*hub, registry->invoke("models_verify", {{"id", model.at("id")}}));
    check(text(verified.at("result").at("model"), "state") == "ready" &&
              text(registry->invoke("models_check", wanted), "status") == "ready",
          "restored file: ready again");

    // Registrations that must not be announced ready, and contracts that are refused.
    const auto partial = root / "apps" / "tool" / "models" / "partial";
    fs::create_directories(partial);
    fs::copy(owned, partial, fs::copy_options::recursive);
    auto lacking = contract(partial);
    lacking["identity"]["variant"] = "partial";
    fs::remove(partial / "config.json");
    registered =
        wait(*hub, registry->invoke("models_register", {{"root", to_utf8(partial)}, {"package", lacking}}));
    check(text(registered.at("result").at("model"), "state") == "incomplete",
          "registration with a missing file: incomplete");
    auto wrong = lacking;
    fixtures::write(partial / "config.json", R"({"context": 8192})");
    wrong["files"][1]["sha256"] = std::string(64, 'e');
    registered =
        wait(*hub, registry->invoke("models_register", {{"root", to_utf8(partial)}, {"package", wrong}}));
    check(text(registered.at("result").at("model"), "state") == "invalid",
          "registration with a wrong hash: invalid");
    auto optional = lacking;
    optional["files"].push_back({{"path", "extras/readme.md"}, {"required", false}});
    registered =
        wait(*hub, registry->invoke("models_register", {{"root", to_utf8(partial)}, {"package", optional}}));
    check(text(registered.at("result").at("model"), "state") == "ready", "an optional file may be absent");
    for (const auto* path : {"../escape.bin", "/etc/passwd", "C:/Windows/x.bin", "weights\\\\x.bin"}) {
        auto escaping = package;
        escaping["files"].push_back({{"path", path}});
        check(code_of(*registry, "models_register", {{"root", to_utf8(owned)}, {"package", escaping}}) ==
                  "InvalidManifest",
              std::string("contract refused for the path ") + path);
    }
    auto twice = package;
    twice["files"].push_back({{"path", "CONFIG.json"}});
    check(code_of(*registry, "models_register", {{"root", to_utf8(owned)}, {"package", twice}}) ==
              "InvalidManifest",
          "a file listed twice is refused");
    auto entry = package;
    entry["entry"] = "elsewhere.gguf";
    check(code_of(*registry, "models_register", {{"root", to_utf8(owned)}, {"package", entry}}) ==
              "InvalidManifest",
          "an entry that is not one of the files is refused");
    check(code_of(*registry, "models_register",
                  {{"root", to_utf8(root / "nowhere")}, {"package", package}}) == "NotFound",
          "a folder that does not exist is refused");
    check(code_of(*registry, "models_register", {{"root", "relative/folder"}, {"package", package}}) ==
              "InvalidPath",
          "a relative folder is refused");

    // A folder of a watched location whose files were already listed one by one.
    const auto pack = library / "packs" / "lyra";
    fixtures::write(pack / "lyra.Q8_0.gguf", fixtures::gguf_llm("Lyra", 'y'));
    fixtures::write(pack / "config.json", "{}");
    wait(*hub, registry->invoke("scan_start", Json::object()));
    check(registry->invoke("models_list", {{"query", "lyra"}}).at("total") == 1,
          "the file is first listed alone");
    const Json lyra = {{"identity", {{"name", "Lyra Pack"}, {"repository", "acme/lyra"}}},
                       {"files", Json::array({file_entry(pack, "lyra.Q8_0.gguf", "weights"),
                                              file_entry(pack, "config.json", "config")})}};
    registered =
        wait(*hub, registry->invoke("models_register", {{"root", to_utf8(pack)}, {"package", lyra}}));
    auto listed = registry->invoke("models_list", {{"query", "lyra"}});
    check(listed.at("total") == 1 && listed.at("models")[0].value("complete", false) &&
              text(listed.at("models")[0], "source") == "folder",
          "the complete model replaces the entry of its file");
    wait(*hub, registry->invoke("scan_start", Json::object()));
    listed = registry->invoke("models_list", {{"query", "lyra"}});
    check(listed.at("total") == 1 && text(listed.at("models")[0], "integrity") == "verified" &&
              text(listed.at("models")[0], "name") == "Lyra Pack",
          "a later scan keeps what the registration established");

    // --- Never "the closest one" ---------------------------------------------------------------
    fixtures::write(library / "a" / "twin.gguf", fixtures::gguf_llm("Twin", 'a'));
    fixtures::write(library / "b" / "twin.gguf", fixtures::gguf_llm("Twin", 'b'));
    fixtures::write(library / "c" / "same.gguf", fixtures::gguf_llm("Same", 's'));
    fixtures::write(library / "d" / "same.gguf", fixtures::gguf_llm("Same", 's'));
    wait(*hub, registry->invoke("scan_start", Json::object()));
    answer = registry->invoke("models_check", {{"name", "twin"}, {"integrity", "none"}});
    check(text(answer, "status") == "ambiguous" && answer.at("candidates").size() == 2,
          "two different models with one name: ambiguous, none is picked");
    check(registry->invoke("models_resolve", {{"query", "twin"}}).at("found") == true,
          "the approximate search still answers as before");
    check(text(registry->invoke("models_check", {{"name", "same"}, {"integrity", "none"}}), "status") ==
              "ready",
          "two copies of the same content are one model");
    check(text(registry->invoke("models_check", {{"name", "same"}}), "status") == "unverified",
          "a file that was only seen is not announced ready");
    const auto same_size = fs::file_size(library / "c" / "same.gguf");
    check(text(registry->invoke("models_check",
                                {{"name", "same"},
                                 {"files", Json::array({{{"path", "same.gguf"}, {"size", same_size}}})}}),
               "status") == "ready",
          "with the expected size it is");
    check(text(registry->invoke("models_check",
                                {{"name", "same"},
                                 {"files", Json::array({{{"path", "same.gguf"}, {"size", same_size + 1}}})}}),
               "status") == "invalid",
          "with another expected size it is invalid");

    // --- Idempotency keys ----------------------------------------------------------------------
    fixtures::write(root / "loose" / "solo.gguf", fixtures::gguf_llm("Solo", 'm'));
    fixtures::write(root / "loose" / "duo.gguf", fixtures::gguf_llm("Duo", 'u'));
    const Json add = {{"path", to_utf8(root / "loose" / "solo.gguf")}, {"idempotencyKey", "test-key-0001"}};
    const auto first = registry->invoke("models_add", add);
    const auto revision = registry->invoke("hub_status", Json::object()).at("revision");
    const auto second = registry->invoke("models_add", add);
    check(!first.contains("replayed") && second.value("replayed", false) &&
              second.at("model").at("id") == first.at("model").at("id"),
          "same key, same arguments: the first answer");
    check(registry->invoke("hub_status", Json::object()).at("revision") == revision, "and no second effect");
    check(code_of(*registry, "models_add",
                  {{"path", to_utf8(root / "loose" / "duo.gguf")}, {"idempotencyKey", "test-key-0001"}}) ==
              "IdempotencyConflict",
          "same key, other arguments: conflict");
    check(code_of(*registry, "models_add",
                  {{"path", to_utf8(root / "loose" / "duo.gguf")}, {"idempotencyKey", "short"}}) ==
                  "InvalidArguments" &&
              code_of(*registry, "models_add",
                      {{"path", to_utf8(root / "loose" / "duo.gguf")},
                       {"idempotencyKey", "bad key with spaces"}}) == "InvalidArguments",
          "malformed keys are refused");
    check(code_of(*registry, "models_add",
                  {{"path", to_utf8(root / "loose" / "missing.gguf")},
                   {"idempotencyKey", "test-key-0002"}}) == "NotAModel" &&
              registry
                  ->invoke("models_add", {{"path", to_utf8(root / "loose" / "duo.gguf")},
                                          {"idempotencyKey", "test-key-0002"}})
                  .contains("model"),
          "a refused request leaves its key free");
    const Json keyed = {{"root", to_utf8(owned)}, {"package", package}, {"idempotencyKey", "test-key-0003"}};
    const auto started = registry->invoke("models_register", keyed);
    check(wait(*hub, started).at("state") == "done", "keyed job");
    const auto again = registry->invoke("models_register", keyed);
    check(again.value("replayed", false) && again.at("job") == started.at("job") &&
              text(again.at("outcome"), "state") == "done",
          "a job is not started twice, and its outcome is given");
    // A token is neither part of the identity of a request nor remembered.
    auto with_token = keyed;
    with_token["idempotencyKey"] = "test-key-0004";
    check(Ledger::fingerprint("install_start", {{"url", "https://example.com/m.gguf"}, {"token", "one"}}) ==
              Ledger::fingerprint("install_start", {{"url", "https://example.com/m.gguf"}, {"token", "two"}}),
          "the token does not enter the fingerprint of a request");

    // After a restart the keys are still known.
    hub.reset();
    registry.reset();
    hub = std::make_shared<Hub>(data_path());
    registry = make_registry(hub);
    check(registry->invoke("models_add", add).value("replayed", false),
          "a key is remembered across a restart");
    const auto later = registry->invoke("models_register", keyed);
    check(later.value("replayed", false) && text(later.at("outcome"), "state") == "done",
          "so is the outcome of its job");
    check(text(registry->invoke("jobs_get", {{"id", started.at("job")}}).at("job"), "state") == "done",
          "and the job can still be asked about");
    const auto stored = *read_file(data_path() / "requests.json") + *read_file(data_path() / "catalog.json");
    check(stored.find("test-key-0001") == std::string::npos && stored.find("SECRET") == std::string::npos,
          "neither keys nor credentials are written to disk");

    // The checks that guard an operation run before a remembered answer is given.
    const Json download = {{"url", "http://127.0.0.1:9/guard.gguf"},
                           {"destination", to_utf8(library / "downloads")},
                           {"idempotencyKey", "test-key-0005"}};
    const auto attempt = registry->invoke("install_start", download);
    check(wait(*hub, attempt).at("state") == "failed", "nothing listens there: the download fails");
    check(registry->invoke("install_start", download).value("replayed", false),
          "while allowed, the answer is replayed");
    std::string library_root;
    const auto locations = registry->invoke("roots_list", Json::object());
    for (const auto& item : locations.at("roots"))
        if (from_utf8(text(item, "path")) == library) library_root = text(item, "id");
    registry->invoke("roots_remove", {{"id", library_root}});
    check(code_of(*registry, "install_start", download) == "OutsideLocations",
          "once the destination is no longer allowed, the remembered answer is not given");

    // --- The ledger, alone -----------------------------------------------------------------------
    {
        const auto file = root / "ledger" / "requests.json";
        int calls = 0;
        const auto work = [&] {
            ++calls;
            return Json{{"job", "job-a"}};
        };
        {
            Ledger ledger(file, "run-a");
            ledger.run("install_start", "ledger-key-1", "print", work);
        }
        Ledger restarted(file, "run-b");
        check(text(*restarted.job("job-a"), "state") == "interrupted",
              "a job cut by a stop of the service is known as such");
        const auto resumed = restarted.run("install_start", "ledger-key-1", "print", work);
        check(calls == 2 && resumed.value("resumed", false), "its request, sent again, takes the work up");
        restarted.job_finished({{"id", "job-a"}, {"state", "done"}});
        Ledger third(file, "run-c");
        const auto replay = third.run("install_start", "ledger-key-1", "print", work);
        check(calls == 2 && replay.value("replayed", false) && text(replay.at("outcome"), "state") == "done",
              "once it ended, nothing is done again");
        bool conflict = false;
        try {
            third.run("install_start", "ledger-key-1", "other", work);
        } catch (const cymcp::Error& error) {
            conflict = error.code == "IdempotencyConflict";
        }
        check(conflict, "ledger conflict");
        // Two requests with one new key at the same moment: one effect.
        std::atomic_int effects{0};
        std::vector<std::thread> threads;
        for (int count = 0; count < 8; ++count)
            threads.emplace_back([&] {
                third.run("models_add", "ledger-key-2", "print", [&] {
                    ++effects;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return Json{{"done", true}};
                });
            });
        for (auto& thread : threads) thread.join();
        check(effects == 1, "eight simultaneous requests with one key have one effect");
    }

    // --- Reservations, alone ---------------------------------------------------------------------
    {
        const auto file = root / "reservations" / "installs.json";
        const Json identity = {{"kind", "file"}, {"repository", "acme/nova"}, {"file", "nova.gguf"}};
        const auto artifact = Reservations::artifact_id(identity);
        const auto target = root / "store" / "nova.gguf";
        std::atomic_int starts{0};
        const auto start = [&](const fs::path&, bool) {
            ++starts;
            return std::string("job-1");
        };
        {
            Reservations reservations(file);
            const auto one = reservations.acquire(artifact, identity, target, start);
            const auto two =
                reservations.acquire(artifact, identity, root / "elsewhere" / "nova.gguf", start);
            check(!one.joined && two.joined && two.job == one.job && two.ticket != one.ticket &&
                      two.requesters == 2 && two.target == target && starts == 1,
                  "the second requester joins the first installation, at its place");
            auto newer = identity;
            newer["revision"] = "v2";
            check(Reservations::artifact_id(newer) != artifact, "another revision is another artifact");
            bool busy = false;
            try {
                reservations.acquire(Reservations::artifact_id(newer), newer, target, start);
            } catch (const cymcp::Error& error) {
                busy = error.code == "DestinationBusy";
            }
            check(busy, "two artifacts are not written to one place");
            auto left = reservations.leave(one.job, one.ticket);
            check(left.known && !left.last && left.requesters == 1,
                  "one requester leaves: the other still waits");
            check(!reservations.leave(one.job, one.ticket).known, "a ticket is used once");
            left = reservations.leave(one.job, two.ticket);
            check(left.known && left.last, "the last requester leaves: the work may be cancelled");
            // Not released: the service stops here.
        }
        std::atomic_int resumed_starts{0};
        fs::path given;
        bool resumed = false;
        {
            Reservations reservations(file);
            const auto pending = reservations.list();
            check(pending.size() == 1 && text(pending[0], "state") == "interrupted" &&
                      from_utf8(text(pending[0], "target")) == target,
                  "an unfinished installation is found again");
            const auto next = reservations.acquire(artifact, identity, root / "elsewhere" / "nova.gguf",
                                                   [&](const fs::path& place, bool was) {
                                                       ++resumed_starts;
                                                       given = place;
                                                       resumed = was;
                                                       return std::string("job-2");
                                                   });
            check(next.resumed && resumed && given == target && next.target == target,
                  "it is taken up again where its partial files are");
            reservations.release(artifact);
            check(reservations.list().empty(), "released when the job has stopped");
            // Many requesters at once: one installation.
            std::atomic_int once{0};
            std::vector<std::thread> threads;
            std::mutex guard;
            std::set<std::string> jobs, tickets;
            for (int count = 0; count < 12; ++count)
                threads.emplace_back([&] {
                    const auto reservation =
                        reservations.acquire(artifact, identity, target, [&](const fs::path&, bool) {
                            ++once;
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            return std::string("job-3");
                        });
                    std::lock_guard lock(guard);
                    jobs.insert(reservation.job);
                    tickets.insert(reservation.ticket);
                });
            for (auto& thread : threads) thread.join();
            check(once == 1 && jobs.size() == 1 && tickets.size() == 12,
                  "twelve simultaneous requesters: one installation, twelve tickets");
            reservations.release(artifact);
        }
        check(Reservations(file).list().empty(), "nothing is left once every installation ended");
    }

    hub.reset();
    std::error_code error;
    fs::remove_all(root, error);
    std::cout << (fixtures::failures ? "FAILED" : "OK") << '\n';
    return fixtures::failures ? 1 : 0;
}
