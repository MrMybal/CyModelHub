// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// Runs one local HTTP exchange for tests/local_http.py and prints its outcome as JSON:
//
//     modelhub_http_probe <port> <method> <path> <timeout ms> <max bytes> [body] [bearer]

#include "platform.hpp"

#include <iostream>

int main(int count, char** values) {
    if (count < 6) return 2;
    modelhub::LocalRequest request;
    request.port = std::atoi(values[1]);
    request.method = values[2];
    request.path = values[3];
    request.timeout = std::chrono::milliseconds(std::atoi(values[4]));
    request.max_response_bytes = static_cast<std::size_t>(std::atoll(values[5]));
    if (count > 6) request.body = values[6];
    if (count > 7) request.bearer = values[7];
    modelhub::Json outcome;
    try {
        const auto response = modelhub::local_http(request);
        outcome = {{"status", response.status}, {"size", response.body.size()}};
        // Large bodies are compared by size and by their first and last bytes.
        if (response.body.size() <= 4096)
            outcome["body"] = response.body;
        else
            outcome["edges"] = response.body.substr(0, 16) + response.body.substr(response.body.size() - 16);
    } catch (const modelhub::Failure& failure) {
        outcome = {{"error", failure.code}};
    }
    std::cout << outcome.dump(-1, ' ', false, modelhub::Json::error_handler_t::replace) << std::endl;
    return 0;
}
