// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// Format detection, scanning, the catalogue and every service operation, in one process.

#include "fixtures.hpp"

#include "app_updates.hpp"
#include "formats.hpp"
#include "scanner.hpp"
#include "service.hpp"

#include <cymcp/http.hpp>

#include <latch>

using namespace modelhub;
using fixtures::check;

namespace {
Json wait(Hub& hub, const Json& id) {
    for (int i = 0; i < 600; ++i) {
        const auto job = hub.jobs.get(id.get<std::string>());
        if (job.at("state") != "running") return job;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {{"state", "timeout"}};
}

Json find(const Json& models, const std::string& name) {
    for (const auto& model : models)
        if (model.value("name", std::string()) == name) return model;
    return Json::object();
}

bool fails(const cymcp::Registry& registry, const char* operation, const Json& arguments, const char* code) {
    try {
        registry.invoke(operation, arguments);
    } catch (const cymcp::Error& error) {
        return error.code == code;
    }
    return false;
}

void check_app_update_selection() {
    const std::string installer = "fixture installer";
    const auto digest = "sha256:" + cymcp::sha256(installer);
    const auto release = [&](const std::string& version, bool prerelease) {
        const auto name = "CyModelHub-" + version + "-windows-x64-setup.exe";
        const auto tag = "v" + version;
        const Json asset{{"name", name}, {"state", "uploaded"}, {"size", installer.size()}, {"digest", digest},
                         {"browser_download_url", "https://github.com/MrMybal/CyModelHub/releases/download/" +
                                                  tag + "/" + name}};
        return Json{{"tag_name", tag}, {"draft", false}, {"prerelease", prerelease}, {"assets", Json::array({asset})}};
    };
    auto draft = release("9.0.0", false);
    draft["draft"] = true;
    auto unverified = release("8.0.0", false);
    unverified["assets"][0]["digest"] = nullptr;
    auto foreign = release("7.0.0", false);
    foreign["assets"][0]["browser_download_url"] = "https://example.com/installer.exe";
    const auto releases = Json::array({release("0.1.10", true), release("0.1.2", false), draft, unverified, foreign});
    const auto preview = select_app_update(releases, "0.1.1", true);
    const auto stable = select_app_update(releases, "0.1.1", false);
    check(preview && preview->version == "0.1.10" && preview->prerelease, "newest numeric app release");
    check(stable && stable->version == "0.1.2" && !stable->prerelease, "stable app releases only");
    check(!select_app_update(releases, "0.1.10", true), "skip equal, older and ineligible app releases");
    check(preview && matches_app_update(installer, *preview) &&
              !matches_app_update("fixture installeR", *preview) &&
              !matches_app_update(installer + "!", *preview), "reject a changed installer digest or size");
}
} // namespace

int main() {
    check_app_update_selection();
    const auto root = fixtures::sandbox("core");
    const auto library = root / "library";

    // --- Fixtures -------------------------------------------------------------------------
    fixtures::write(library / "llm" / "Nova-7B.Q4_K_M.gguf", fixtures::gguf_llm("Nova 7B"));
    fixtures::write(library / "loras" / "style.safetensors", fixtures::lora_sdxl());
    fixtures::write(library / "checkpoints" / "dream.safetensors", fixtures::checkpoint_sdxl());
    fixtures::write(library / "checkpoints" / "dream-copy.safetensors", fixtures::checkpoint_sdxl());
    fixtures::write(library / "vae" / "decoder.safetensors", fixtures::vae());
    fixtures::write(library / "upscale_models" / "4x-sharp.pth", std::string(4096, 'p'));
    fixtures::write(library / "notes.txt", "not a model");
    fixtures::write(library / "pipeline" / "model_index.json", R"({"_class_name":"FluxPipeline","_diffusers_version":"0.30.0"})");
    fixtures::write(library / "pipeline" / "transformer" / "diffusion_pytorch_model.safetensors", fixtures::vae());
    const auto snapshot = root / "hf" / "hub" / "models--acme--nova-gguf" / "snapshots" / "abc123";
    fixtures::write(snapshot / "nova.Q8_0.gguf", fixtures::gguf_llm("Nova HF", 'h'));
    fixtures::write(root / "hf" / "hub" / "models--acme--nova-gguf" / "refs" / "main", "abc123\n");
    const std::string digest(64, 'a');
    fixtures::write(root / "ollama" / "blobs" / ("sha256-" + digest), fixtures::gguf_llm("Nova Ollama", 'o'));
    fixtures::write(root / "ollama" / "manifests" / "registry.ollama.ai" / "library" / "nova" / "latest",
                    Json{{"layers", {{{"mediaType", "application/vnd.ollama.image.model"},
                                      {"digest", "sha256:" + digest}}}}}.dump());

    // --- Format detection -----------------------------------------------------------------
    auto probe = probe_file(library / "llm" / "Nova-7B.Q4_K_M.gguf");
    check(probe.format == "gguf" && probe.type == "model" && probe.modality == "text", "GGUF chat model");
    check(probe.architecture == "llama" && probe.quantization == "Q4_K_M", "GGUF architecture and quantization");
    check(probe.metadata.value("llama.context_length", 0) == 8192, "GGUF context length");
    check(probe.metadata.value("chatTemplate", false), "GGUF chat template seen after a skipped array");

    probe = probe_file(library / "loras" / "style.safetensors");
    check(probe.format == "safetensors" && probe.type == "lora" && probe.architecture == "sdxl" &&
              probe.modality == "image", "safetensors SDXL LoRA");
    probe = probe_file(library / "checkpoints" / "dream.safetensors");
    check(probe.type == "model" && probe.architecture == "sdxl", "safetensors SDXL checkpoint");
    probe = probe_file(library / "vae" / "decoder.safetensors");
    check(probe.type == "vae", "safetensors VAE");
    probe = probe_file(library / "upscale_models" / "4x-sharp.pth");
    apply_path_hints(probe, library / "upscale_models" / "4x-sharp.pth");
    check(probe.format == "pickle" && probe.unsafe && probe.type == "upscaler", "pickle upscaler from its folder");
    const auto pipeline = probe_folder(library / "pipeline");
    check(pipeline && pipeline->format == "diffusers" && pipeline->architecture == "flux", "Diffusers folder");
    check(!model_extension(library / "notes.txt"), "text files are not models");

    // System messages are not always UTF-8 (a French Windows says "accès refusé" in its code page).
    bool serialisable = true;
    try {
        (void)Json(system_text("acc\xE8s refus\xE9")).dump();
    } catch (const std::exception&) {
        serialisable = false;
    }
    check(serialisable && system_text("déjà UTF-8") == "déjà UTF-8", "system text is made valid UTF-8");

    // --- Service --------------------------------------------------------------------------
    auto hub = std::make_shared<Hub>(data_path());
    const auto registry = make_registry(hub);
    check(wait(*hub, hub->start_scan({})).at("state") == "done", "first scan");
    auto listed = registry->invoke("models_list", Json::object());
    check(listed.at("total") == 2, "Hugging Face and Ollama stores found");
    check(find(listed.at("models"), "nova:latest").value("sha256", std::string()) == digest, "Ollama digest reused");
    check(find(listed.at("models"), "nova.Q8_0").value("repo", std::string()) == "acme/nova-gguf",
          "Hugging Face repository");

    auto added = registry->invoke("roots_add", {{"path", to_utf8(library)}, {"label", "Library"}});
    check(wait(*hub, added.at("job")).at("state") == "done", "scan of an added location");
    listed = registry->invoke("models_list", {{"limit", 500}});
    check(listed.at("total") == 9, "all models catalogued once (got " + listed.at("total").dump() + ")");
    check(find(listed.at("models"), "pipeline").value("folder", false), "folder model listed as one entry");
    check(registry->invoke("models_list", {{"type", "lora"}}).at("total") == 1, "filter by type");
    check(registry->invoke("models_list", {{"modality", "text"}}).at("total") == 3, "filter by modality");
    check(registry->invoke("models_list", {{"query", "nova 7b"}}).at("total") == 1, "word search");
    const auto facets = registry->invoke("models_facets", Json::object());
    check(facets.at("total") == 9 && !facets.at("modality").empty(), "facets");

    auto resolved = registry->invoke("models_resolve", {{"query", "dream.safetensors"}});
    check(resolved.at("found") == true && from_utf8(resolved.at("path").get<std::string>()).filename() ==
                                              "dream.safetensors", "resolve by file name");
    check(registry->invoke("models_resolve", {{"query", "acme/nova-gguf"}}).at("found") == true, "resolve by repository");
    check(registry->invoke("models_resolve", {{"query", "nothing-like-this"}}).at("found") == false, "resolve miss");

    const auto copies = registry->invoke("models_duplicates", Json::object());
    check(copies.at("groups").size() == 1 && copies.at("groups")[0].at("models").size() == 2, "duplicate copies");

    // User edits survive a scan; a reset returns to detection.
    const auto lora = find(listed.at("models"), "style").at("id").get<std::string>();
    registry->invoke("models_update", {{"id", lora}, {"type", "adapter"}, {"tags", {"portrait", "portrait", "v2"}},
                                       {"favorite", true}, {"notes", "keep"}});
    check(wait(*hub, hub->start_scan({})).at("state") == "done", "second scan");
    auto record = registry->invoke("models_get", {{"id", lora}}).at("model");
    check(record.at("type") == "adapter" && record.at("tags").size() == 2 && record.at("favorite") == true &&
              record.at("notes") == "keep", "edits kept after a scan");
    record = registry->invoke("models_update", {{"id", lora}, {"reset", {"type"}}}).at("model");
    check(record.at("type") == "lora", "reset returns to the detected type");

    // A file renamed outside CyModelHub keeps its entry.
    fs::rename(library / "loras" / "style.safetensors", library / "loras" / "style-v2.safetensors");
    wait(*hub, hub->start_scan({}));
    record = registry->invoke("models_get", {{"id", lora}}).at("model");
    check(from_utf8(record.at("path").get<std::string>()).filename() == "style-v2.safetensors" &&
              record.at("notes") == "keep" && record.at("missing") == false, "renamed file keeps its entry");

    // Move inside registered locations only; hard link without copying.
    const auto vae = find(listed.at("models"), "decoder").at("id").get<std::string>();
    check(fails(*registry, "models_move", {{"id", vae}, {"destination", to_utf8(root / "elsewhere")}},
                "OutsideLocations"), "move outside the locations is refused");
    auto moved = wait(*hub, registry->invoke("models_move", {{"id", vae}, {"destination", to_utf8(library / "moved")}})
                                .at("job"));
    check(moved.at("state") == "done" && fs::exists(library / "moved" / "decoder.safetensors") &&
              !fs::exists(library / "vae" / "decoder.safetensors"), "move");
    const auto linked = registry->invoke("models_link", {{"id", vae}, {"destination", to_utf8(library / "linked")}});
    check(fs::exists(from_utf8(linked.at("link").get<std::string>())), "hard link");
    const auto ollama = find(listed.at("models"), "nova:latest").at("id").get<std::string>();
    check(fails(*registry, "models_move", {{"id", ollama}, {"destination", to_utf8(library)}}, "ManagedElsewhere"),
          "models of another tool are not moved");

    const auto hashed = wait(*hub, registry->invoke("models_hash", {{"id", vae}}).at("job"));
    check(hashed.at("state") == "done" && hashed.at("result").at("sha256").get<std::string>().size() == 64, "hash");

    // Manual addition outside every location, then a vanished file.
    fixtures::write(root / "loose" / "solo.gguf", fixtures::gguf_llm("Solo", 'm'));
    const auto manual = registry->invoke("models_add", {{"path", to_utf8(root / "loose" / "solo.gguf")}}).at("model");
    check(manual.at("source") == "manual" && manual.at("modality") == "text", "manual addition");
    check(fails(*registry, "models_add", {{"path", to_utf8(library / "notes.txt")}}, "NotAModel"), "not a model");
    fs::remove(root / "loose" / "solo.gguf");
    wait(*hub, hub->start_scan({}));
    check(registry->invoke("models_get", {{"id", manual.at("id")}}).at("model").at("missing") == true,
          "vanished file marked missing");
    check(registry->invoke("models_remove", {{"id", manual.at("id")}}).at("removed") == true, "forget an entry");

    // Downloads are confined and named safely before any network use.
    check(fails(*registry, "install_start", {{"url", "http://example.com/model.gguf"}}, "InsecureTransport"),
          "plain HTTP download refused");
    check(fails(*registry, "install_start", {{"url", "https://example.com/m.gguf"},
                                             {"destination", to_utf8(root / "elsewhere")}}, "OutsideLocations"),
          "download outside the locations refused");
    check(fails(*registry, "install_start", {{"url", "https://example.com/setup.exe"}}, "InvalidName"),
          "download of a non-model file refused");

    // The catalogue is reloaded from disk as it was.
    const auto before = registry->invoke("models_list", {{"limit", 500}}).at("total");
    Catalog reloaded(data_path() / "catalog.json");
    check(reloaded.models().size() == before && reloaded.find(lora) && reloaded.find(lora)->notes == "keep",
          "catalogue persisted");
    check(reloaded.roots().size() == hub->catalog.roots().size(), "locations persisted");

    // Updating must neither interrupt a job nor allow one to start after shutdown begins.
    std::latch finish_job(1);
    const auto pending = hub->jobs.start("test", "Test guarded shutdown", [&](const Job&) {
        finish_job.wait();
        return Json::object();
    });
    check(fails(*registry, "hub_shutdown", {{"whenIdle", true}}, "ServiceBusy") && !hub->shutdown,
          "guarded shutdown preserves running jobs");
    finish_job.count_down();
    check(wait(*hub, pending).at("state") == "done", "job finishes before update");
    check(registry->invoke("hub_shutdown", {{"whenIdle", true}}).at("stopping") == true && hub->shutdown,
          "guarded shutdown accepts an idle service");
    check(fails(*registry, "scan_start", Json::object(), "ShuttingDown"),
          "no job starts once update shutdown begins");

    hub.reset();
    std::error_code error;
    fs::remove_all(root, error);
    std::cout << (fixtures::failures ? "FAILED" : "OK") << '\n';
    return fixtures::failures ? 1 : 0;
}
