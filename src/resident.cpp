// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "resident.hpp"

namespace modelhub {
Resident::Resident(std::shared_ptr<Hub> hub) : hub_(std::move(hub)), registry_(make_registry(hub_)) {}

Resident::~Resident() { stop(); }

void Resident::write_descriptor(bool running) const {
    const Json descriptor = {{"name", "CyModelHub"},
                             {"version", CYMODELHUB_VERSION},
                             {"url", "http://127.0.0.1:" + std::to_string(port_)},
                             {"port", running ? port_ : 0},
                             {"pid", running ? process_id() : 0},
                             {"token", token_},
                             {"executable", to_utf8(executable_path())}};
    // Owner-only: the token gives full control of the catalogue.
    write_file_atomic(hub_->data_directory / "service.json", descriptor.dump(2), true);
}

int Resident::start(int preferred_port) {
    // The token survives restarts so MCP client configurations stay valid.
    if (const auto content = read_file(hub_->data_directory / "service.json", 65536)) {
        const auto previous = Json::parse(*content, nullptr, false);
        if (previous.is_object()) token_ = previous.value("token", std::string());
    }
    if (token_.size() < 32) token_ = cymcp::random_token();

    mcp_ = std::make_unique<cymcp::McpServer>(*registry_, "CyModelHub");
    const auto endpoint = [this](const Json& request, const cymcp::CallContext& context) {
        if (request.is_object() && request.contains("jsonrpc")) return mcp_->handle(request, context);
        return cymcp::dispatch_app(*registry_, request, context);
    };
    http_ = std::make_unique<cymcp::LocalHttpServer>(token_, endpoint, 8);
    try {
        // The usual port, unless another program (or a service for another data folder) has it.
        port_ = http_->start(loopback_port_taken(preferred_port) ? 0 : preferred_port);
    } catch (const std::exception&) {
        http_ = std::make_unique<cymcp::LocalHttpServer>(token_, endpoint, 8);
        port_ = http_->start(0);
    }
    write_descriptor(true);
    return port_;
}

void Resident::stop() {
    if (!http_) return;
    mcp_->cancel_all();
    http_->stop();
    http_.reset();
    mcp_.reset();
    try {
        write_descriptor(false);
    } catch (const std::exception&) {
    }
}
} // namespace modelhub
