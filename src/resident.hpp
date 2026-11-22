// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "service.hpp"
#include <cymcp/mcp.hpp>

namespace modelhub {
// The loopback HTTP endpoint of the service: /v1 for applications, /mcp for AI clients.
// While it runs, <data>/service.json tells local programs of the same user how to reach it.
class Resident {
    std::shared_ptr<Hub> hub_;
    std::shared_ptr<cymcp::Registry> registry_;
    std::unique_ptr<cymcp::McpServer> mcp_;
    std::unique_ptr<cymcp::LocalHttpServer> http_;
    std::string token_;
    int port_ = 0;

    void write_descriptor(bool running) const;

  public:
    explicit Resident(std::shared_ptr<Hub> hub);
    ~Resident();
    // Listens on the preferred port, or on a free one when it is taken. Returns the port.
    int start(int preferred_port);
    void stop();
    int port() const { return port_; }
};
} // namespace modelhub
