---
id: integrate
title: Connect scripts, applications and assistants
description: Look up model paths through the CLI, the C++ client, local HTTP or MCP.
section: Integration
parent: overview
order: 40
revision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Command line {#cli}

Run the CLI from the installation directory or add it to your shell's command path.
The CLI and the desktop application use the same catalogue when they use the same data
directory.

```text
CyModelHubCli status
CyModelHubCli list --type lora
CyModelHubCli where sd_xl_base_1.0
CyModelHubCli operations
```

`where` prints the file or folder path of the best installed match. Use `list --json` for
structured output. `operations` lists the complete operation catalogue and parameter schemas.

## C++ client {#cpp}

The client is MIT-licensed and requires C++20. Add the CyModelHub source checkout to your
CMake project with `add_subdirectory` and link `CyModelHub::Client`.

```cpp
#include <cymodelhub/client.hpp>

cymodelhub::Client hub;
auto model = hub.resolve("sd_xl_base_1.0", "model", "image");
if (model) {
    const auto& installed_path = model->path;
    // Pass installed_path to your application's runtime.
}
```

`resolve()` returns an empty value when there is no match. Service errors can be caught as
`cymodelhub::Error`. `Client::invoke()` accepts a service operation and JSON arguments.
If the service executable is installed elsewhere, set `CYMODELHUB_SERVICE` to its path
or pass it in `cymodelhub::Options`.

## Assistants and local HTTP {#mcp}

For an MCP client that supports HTTP, use **Settings → Copy the MCP configuration**.
The copied configuration contains a local access token. Keep it private. A client that
launches a stdio server can run the installed `CyModelHubMcp` executable instead.

Other languages can send JSON to `POST <url>/v1` with `Content-Type: application/json`
and `Authorization: Bearer <token>`. The loopback URL and token are in your data folder's
`service.json`. The following request lists installed LoRA adapters.

```json
{"method":"invoke","name":"models_list","arguments":{"type":"lora"}}
```

Long-running operations such as `install_start` return a job ID. Use `jobs_get` to poll
that ID or `Client::wait_job()` in C++.
