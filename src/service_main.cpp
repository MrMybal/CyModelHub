// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "resident.hpp"

#include <csignal>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

int run(const std::vector<std::string>& arguments) {
    int port = modelhub::default_service_port;
    bool scan = true;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            std::cout << "CyModelHubService [--data PATH] [--port N] [--no-scan]\n"
                         "Runs the local CyModelHub service: the shared catalogue of installed AI models.\n"
                         "Applications reach it through <data>/service.json (loopback HTTP, bearer token).\n";
            return 0;
        }
        if (argument == "--version") {
            std::cout << CYMODELHUB_VERSION << '\n';
            return 0;
        }
        if (argument == "--no-scan") scan = false;
        else if (argument == "--data" && index + 1 < arguments.size())
            modelhub::set_environment("CYMODELHUB_DATA", arguments[++index]);
        else if (argument == "--port" && index + 1 < arguments.size()) port = std::stoi(arguments[++index]);
        else {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    const auto data = modelhub::data_path();
    modelhub::fs::create_directories(data);
    cymcp::private_directory(data);
    // One service per data folder: a second start leaves the first one in charge.
    const modelhub::FileLock instance(data / "service.lock");
    if (!instance.held()) {
        std::cerr << "CyModelHub service is already running for " << modelhub::to_utf8(data) << '\n';
        return 0;
    }
    const auto hub = std::make_shared<modelhub::Hub>(data);
    modelhub::Resident resident(hub);
    const auto actual = resident.start(port);
    std::cerr << "CyModelHub " << CYMODELHUB_VERSION << " listening on http://127.0.0.1:" << actual << '\n';
    if (scan) hub->start_scan({});
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!interrupted && !hub->shutdown) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Let the reply to hub_shutdown leave before the listener closes.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    resident.stop();
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
