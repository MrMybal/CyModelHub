// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// End to end: the client library starts the real service, then the same catalogue is read
// through the application API, through MCP over HTTP and through the MCP stdio relay.

#include "fixtures.hpp"

#include <cymcp/mcp.hpp>
#include <cymodelhub/client.hpp>

using fixtures::check;
using fixtures::Json;

namespace {
// The text or structured payload of an MCP tool result.
Json payload(const Json& result) {
    if (result.contains("structuredContent")) return result.at("structuredContent");
    return Json::parse(result.at("content").at(0).at("text").get<std::string>(), nullptr, false);
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc < 3) {
        std::cerr << "usage: modelhub_client_tests <CyModelHubService> <CyModelHubMcp>\n";
        return 2;
    }
    const fixtures::fs::path service(argv[1]), relay(argv[2]);
    const auto root = fixtures::sandbox("client");
    const auto library = root / "library";
    fixtures::write(library / "llm" / "Nova-7B.Q4_K_M.gguf", fixtures::gguf_llm("Nova 7B"));
    fixtures::write(library / "loras" / "style.safetensors", fixtures::lora_sdxl());

    cymodelhub::Options options;
    options.data_directory = root / "data";
    options.service_executable = service;
    try {
        {
            cymodelhub::Options passive = options;
            passive.start_service = false;
            check(!cymodelhub::Client(passive).available(), "no service before the first use");
        }
        cymodelhub::Client hub(options);
        const auto status = hub.invoke("hub_status");
        check(status.value("name", std::string()) == "CyModelHub", "service started on demand");
        check(hub.endpoint().rfind("http://127.0.0.1:", 0) == 0, "loopback endpoint");

        const auto added = hub.invoke("roots_add", {{"path", modelhub::to_utf8(library)}});
        check(hub.wait_job(added.at("job").get<std::string>()).value("state", std::string()) == "done", "scan job");

        std::size_t total = 0;
        const auto models = hub.list({}, &total);
        check(total == 2 && models.size() == 2, "two models listed");
        cymodelhub::Query loras;
        loras.type = "lora";
        check(hub.list(loras).size() == 1, "typed query");
        const auto llm = hub.resolve("Nova-7B.Q4_K_M", "model", "text");
        check(llm && llm->architecture == "llama" && llm->path.filename() == "Nova-7B.Q4_K_M.gguf", "resolve");
        check(hub.path_of("style").filename() == "style.safetensors", "path of a model");
        check(hub.path_of("not-installed").empty(), "path of an absent model");
        check(llm && hub.get(llm->id) && !hub.get("000000000000"), "get by ID");
        try {
            hub.invoke("models_move", {{"id", llm ? llm->id : ""}, {"destination", modelhub::to_utf8(root / "out")}});
            check(false, "service errors reach the caller");
        } catch (const cymodelhub::Error& error) {
            check(error.code == "OutsideLocations", "service errors keep their code");
        }

        // A second client finds the running service instead of starting another one.
        cymodelhub::Client second(options);
        check(second.invoke("hub_status").at("revision") == hub.invoke("hub_status").at("revision"),
              "one service shared by every client");

        // A service for another data folder runs beside the first one, on its own port.
        {
            cymodelhub::Options other = options;
            other.data_directory = root / "data-other";
            cymodelhub::Client beside(other);
            const auto folder = beside.invoke("hub_status").value("dataDirectory", std::string());
            check(folder.find("data-other") != std::string::npos && beside.endpoint() != hub.endpoint(),
                  "two data folders, two services, two ports");
            check(hub.invoke("hub_status").value("models", 0) == 2, "the first service still answers");
            beside.invoke("hub_shutdown");
        }

        // MCP over HTTP, with the configuration an AI client would be given.
        const auto configuration = hub.mcp_configuration().at("mcpServers").at("cymodelhub");
        const auto bearer = configuration.at("headers").at("Authorization").get<std::string>();
        cymcp::McpClient http(configuration.at("url").get<std::string>(), bearer.substr(7));
        http.connect();
        check(http.list_tools().size() >= 20, "MCP HTTP lists the tools");
        const auto listed = payload(http.call_tool("models_list", {{"modality", "text"}}));
        check(listed.value("total", 0) == 1, "MCP HTTP tool call");
        bool refused = false;
        try {
            cymcp::McpClient anonymous(configuration.at("url").get<std::string>(), std::string(40, 'x'));
            anonymous.connect();
        } catch (const std::exception&) {
            refused = true;
        }
        check(refused, "a wrong token is refused");

        // MCP stdio relay, as launched by an AI client.
        cymcp::ProcessOptions process;
        process.executable = relay;
        process.arguments = {"--data", modelhub::to_utf8(options.data_directory)};
        cymcp::McpClient stdio(process);
        stdio.connect();
        check(stdio.list_tools().size() == http.list_tools().size(), "MCP stdio exposes the same tools");
        const auto where = payload(stdio.call_tool("models_resolve", {{"query", "style"}}));
        check(where.value("found", false), "MCP stdio tool call");

        hub.invoke("hub_shutdown");
        cymodelhub::Options passive = options;
        passive.start_service = false;
        bool stopped = false;
        for (int i = 0; i < 50 && !stopped; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stopped = !cymodelhub::Client(passive).available();
        }
        check(stopped, "service stops on request");
    } catch (const std::exception& error) {
        check(false, std::string("unexpected error: ") + error.what());
        try {
            cymodelhub::Options passive = options;
            passive.start_service = false;
            cymodelhub::Client(passive).invoke("hub_shutdown");
        } catch (const std::exception&) {
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::error_code error;
    fixtures::fs::remove_all(root, error);
    std::cout << (fixtures::failures ? "FAILED" : "OK") << '\n';
    return fixtures::failures ? 1 : 0;
}
