// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// Command line front end of the CyModelHub service, for scripts and quick checks.

#include <cymodelhub/client.hpp>

#include "platform.hpp"
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
using cymodelhub::Json;

const char* usage =
    "CyModelHubCli [--data PATH] <command>\n"
    "  status                         service status\n"
    "  list [words] [--type T] [--modality M] [--format F] [--json]\n"
    "  where <name>                   path of the best installed match\n"
    "  info <id>                      full record of a model\n"
    "  scan [root]                    look for models again and wait\n"
    "  roots                          watched locations\n"
    "  add-root <folder>              watch a folder\n"
    "  add <path>                     register one file or model folder\n"
    "  install <url> | install <repository> <file>   download into the CyModelHub store\n"
    "  duplicates                     identical models stored several times\n"
    "  jobs                           background jobs\n"
    "  call <operation> [json]        any operation of the service\n"
    "  operations                     every operation with its JSON schema\n"
    "  mcp-config                     MCP client configuration (HTTP)\n"
    "  stop                           stop the service\n";

std::string human_size(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit < 2 ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
}

int wait(cymodelhub::Client& client, const Json& reply) {
    const auto job = client.wait_job(reply.at("job").get<std::string>());
    std::cout << job.dump(2) << '\n';
    return job.value("state", std::string()) == "done" ? 0 : 1;
}

int run(std::vector<std::string> arguments) {
    cymodelhub::Options options;
    arguments.erase(arguments.begin());
    if (arguments.size() >= 2 && arguments[0] == "--data") {
        options.data_directory = modelhub::from_utf8(arguments[1]);
        arguments.erase(arguments.begin(), arguments.begin() + 2);
    }
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h" || arguments[0] == "help") {
        std::cout << usage;
        return arguments.empty() ? 2 : 0;
    }
    const auto command = arguments[0];
    const auto argument = [&](std::size_t index) { return index < arguments.size() ? arguments[index] : std::string(); };
    cymodelhub::Client client(options);

    if (command == "status") std::cout << client.invoke("hub_status").dump(2) << '\n';
    else if (command == "list") {
        cymodelhub::Query query;
        query.limit = 500;
        bool json = false;
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const auto& word = arguments[index];
            if (word == "--json") json = true;
            else if (word == "--type" && index + 1 < arguments.size()) query.type = arguments[++index];
            else if (word == "--modality" && index + 1 < arguments.size()) query.modality = arguments[++index];
            else if (word == "--format" && index + 1 < arguments.size()) query.format = arguments[++index];
            else query.text += (query.text.empty() ? "" : " ") + word;
        }
        std::size_t total = 0;
        const auto models = client.list(query, &total);
        if (json) {
            auto items = Json::array();
            for (const auto& model : models) items.push_back(model.raw);
            std::cout << items.dump(2) << '\n';
        } else {
            for (const auto& model : models)
                std::cout << std::left << std::setw(13) << model.id << std::setw(11) << model.modality
                          << std::setw(14) << model.type << std::setw(13) << model.format << std::right
                          << std::setw(10) << human_size(model.size) << "  " << model.name
                          << (model.missing ? "  [missing]" : "") << '\n';
            std::cout << models.size() << " of " << total << " models\n";
        }
    } else if (command == "where") {
        const auto path = client.path_of(argument(1));
        if (path.empty()) {
            std::cerr << "Not installed\n";
            return 1;
        }
        std::cout << modelhub::to_utf8(path) << '\n';
    } else if (command == "info") std::cout << client.invoke("models_get", {{"id", argument(1)}}).dump(2) << '\n';
    else if (command == "scan") {
        Json args = Json::object();
        if (!argument(1).empty()) args["root"] = argument(1);
        return wait(client, client.invoke("scan_start", args));
    } else if (command == "roots") std::cout << client.invoke("roots_list").dump(2) << '\n';
    else if (command == "add-root") return wait(client, client.invoke("roots_add", {{"path", argument(1)}}));
    else if (command == "add") std::cout << client.invoke("models_add", {{"path", argument(1)}}).dump(2) << '\n';
    else if (command == "install") {
        Json args = argument(2).empty() ? Json{{"url", argument(1)}}
                                        : Json{{"repository", argument(1)}, {"file", argument(2)}};
        const auto job = client.invoke("install_start", args).at("job").get<std::string>();
        const auto result = client.wait_job(job, std::chrono::hours(24), [](const Json& state) {
            const auto done = state.value("done", std::uint64_t{0}), total = state.value("total", std::uint64_t{0});
            std::cerr << '\r' << human_size(done) << (total ? " / " + human_size(total) : std::string()) << "      ";
        });
        std::cerr << '\n';
        std::cout << result.dump(2) << '\n';
        return result.value("state", std::string()) == "done" ? 0 : 1;
    } else if (command == "duplicates") std::cout << client.invoke("models_duplicates").dump(2) << '\n';
    else if (command == "jobs") std::cout << client.invoke("jobs_list").dump(2) << '\n';
    else if (command == "call") {
        const auto args = argument(2).empty() ? Json::object() : Json::parse(argument(2));
        std::cout << client.invoke(argument(1), args).dump(2) << '\n';
    } else if (command == "operations") std::cout << client.describe().dump(2) << '\n';
    else if (command == "mcp-config") std::cout << client.mcp_configuration().dump(2) << '\n';
    else if (command == "stop") {
        options.start_service = false;
        cymodelhub::Client running(options);
        if (running.available()) running.invoke("hub_shutdown");
        std::cout << "stopped\n";
    } else {
        std::cerr << usage;
        return 2;
    }
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
        const int code = run(arguments);
        // Do not rely on the flush at process exit: on Windows, network worker threads that are
        // stopped mid-work at that moment can make the runtime skip it, losing the output.
        std::cout.flush();
        return code;
    } catch (const cymodelhub::Error& error) {
        std::cerr << error.code << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
