// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// MCP stdio server for AI clients that launch a command. It owns no catalogue: every tool call
// is forwarded to the running CyModelHub service, so all clients see the same models.

#include <cymodelhub/client.hpp>

#include "platform.hpp"
#include <cymcp/mcp.hpp>
#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
int run(const std::vector<std::string>& arguments) {
    cymodelhub::Options options;
    bool configuration = false;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            std::cout << "CyModelHubMcp [--data PATH] [--config]\n"
                         "Runs an MCP stdio server backed by the local CyModelHub service.\n"
                         "With --config, prints the MCP client configuration of the HTTP endpoint and exits.\n";
            return 0;
        }
        if (argument == "--config") configuration = true;
        else if (argument == "--data" && index + 1 < arguments.size())
            options.data_directory = modelhub::from_utf8(arguments[++index]);
        else {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    cymodelhub::Client client(options);
    if (configuration) {
        std::cout << client.mcp_configuration().dump(2) << std::endl; // flushed before the process exits
        return 0;
    }
    cymcp::Registry tools(cymcp::Policy::allow_all());
    const auto catalogue = client.describe(); // kept alive for the loop below
    for (const auto& operation : catalogue.at("operations")) {
        cymcp::Operation tool;
        tool.name = operation.at("name").get<std::string>();
        tool.description = operation.value("description", std::string());
        tool.input_schema = operation.at("inputSchema");
        tool.output_schema = operation.value("outputSchema", cymcp::Json::object());
        if (const auto hints = operation.find("annotations"); hints != operation.end() && hints->is_object())
            tool.read_only = hints->value("readOnlyHint", false);
        tool.handler = [&client, name = tool.name](const cymcp::Json& args, const cymcp::CallContext&) {
            try {
                return client.invoke(name, args);
            } catch (const cymodelhub::Error& error) {
                throw cymcp::Error(error.code, error.what());
            }
        };
        tools.add(std::move(tool));
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    cymcp::McpServer(tools, "CyModelHub").serve_stdio(std::cin, std::cout);
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) arguments.push_back(modelhub::to_utf8(modelhub::fs::path(argv[i])));
#else
int main(int argc, char** argv) {
    const std::vector<std::string> arguments(argv, argv + argc);
#endif
    try {
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << cymcp::redact_error_message(error.what()) << '\n';
        return 1;
    }
}
