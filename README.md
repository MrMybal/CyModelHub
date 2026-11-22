<table width="100%">
  <tr>
    <td width="300" align="left" valign="top">
      <a href="BUILD.md">Build</a><br>
      <a href="docs/API.md">Service operations</a><br>
      <a href="VERSION.md">Release notes</a><br><br>
      <sub>README</sub><br>
      <a href="README.md">English</a> · <a href="README.fr.md">Français</a>
    </td>
    <td width="400" align="center" valign="middle">
      <h1>CyModelHub</h1>
    </td>
    <td width="300" align="right" valign="top">
      <p><sub>License</sub><br><a href="LICENSE"><strong>GPL-3.0-only</strong></a><br><sub>client library: <a href="LICENSE-MIT">MIT</a></sub></p>
      <p><sub>Version</sub><br><a href="VERSION.md"><strong>0.1.0</strong></a><br><strong>IN DEVELOPMENT</strong></p>
      <p><sub>Platforms</sub><br>Windows · Linux · macOS</p>
    </td>
  </tr>
</table>

**One catalogue of the AI models installed on your computer, shared by every application.**
Install a model once; every program that asks CyModelHub finds it, instead of downloading
and storing its own copy.

> **IN DEVELOPMENT · 0.1.0** — built and tested on Windows x64. The Linux and macOS code paths
> are written but have not been compiled or run yet. See the [release notes](VERSION.md).

[![The model list with categories, and the record of the selected model](docs/screenshots/models.png)](docs/screenshots/models.png)

## What it does

- **Finds the models you already have.** The Hugging Face cache, Ollama and LM Studio stores
  are read automatically; add any other folder (ComfyUI, a models drive) to watch it too.
- **Understands what each file is.** Language models, image and video diffusion, audio,
  embeddings, LoRA, VAE, ControlNet, text encoders, upscalers. GGUF and safetensors headers
  are read (architecture, quantization, context length, base model); Diffusers, Transformers
  and MLX folders count as one model. Weights are never loaded and pickle files are never
  opened. Detection is a best effort: correct it by hand and your choice is kept.
- **Answers applications.** "Where is `sd_xl_base_1.0`?" returns the path of the installed
  file, by name, file name, repository, SHA-256 or ID.
- **Manages the files.** Tags, notes and favourites; duplicates and the space they waste;
  move a model to another drive; make it visible in another application's folder with a hard
  or symbolic link, without copying it.
- **Installs models.** From a Hugging Face repository or an HTTPS address, with progress,
  resume and SHA-256 verification.
- **Works with AI assistants.** Every operation is also an MCP tool.

CyModelHub never deletes a model file. It only writes inside the locations you registered.

## The parts

| Program | Role |
| --- | --- |
| `CyModelHubService` | The local service that owns the catalogue. One per user, started on demand, reachable on the loopback interface only, with an access token. |
| `CyModelHub` | The desktop application ([CyUI](https://github.com/MrMybal/CyUI)), in English and French. It uses the same client library as any other program. |
| `CyModelHub::Client` | The C++ library an application links to find and manage models. |
| `CyModelHubMcp` | MCP stdio server for AI clients that launch a command. The service also serves MCP over HTTP. |
| `CyModelHubCli` | Command line access for scripts and quick checks. |

## Use it from your application (C++)

```cmake
add_subdirectory(external/CyModelHub)   # builds only the client library when embedded
target_link_libraries(my_app PRIVATE CyModelHub::Client)
```

```cpp
#include <cymodelhub/client.hpp>

cymodelhub::Client hub;                                   // starts the service when needed
if (auto model = hub.resolve("sd_xl_base_1.0", "model", "image"))
    load(model->path);                                    // already installed: use it
else
    hub.invoke("install_start", {{"repository", "stabilityai/stable-diffusion-xl-base-1.0"},
                                 {"file", "sd_xl_base_1.0.safetensors"}});

cymodelhub::Query loras;                                  // LoRAs made for SDXL
loras.type = "lora";
loras.architecture = "sdxl";
for (const auto& lora : hub.list(loras)) show(lora.name, lora.path);
```

The library is MIT-licensed, needs C++20 and pulls
[CyMCPSDK](https://github.com/MrMybal/CyMCPSDK) (MIT) for JSON and HTTP. A complete example is in [examples/client](examples/client). The service is
found through `CYMODELHUB_SERVICE`, next to your program, or where it last ran.

Any language can talk to the service directly: read `service.json` in the CyModelHub data
folder (`url`, `token`), then `POST <url>/v1` with `Authorization: Bearer <token>` and
`{"method":"invoke","name":"models_resolve","arguments":{"query":"..."}}`.
[All operations](docs/API.md).

## Use it from an AI assistant (MCP)

In the application, **Settings → Copy the MCP configuration** gives the HTTP configuration,
token included. For a client that launches a command, point it at `CyModelHubMcp`:

```json
{ "mcpServers": { "cymodelhub": { "command": "/path/to/CyModelHubMcp" } } }
```

The assistant then has `models_list`, `models_resolve`, `models_update`, `install_start`,
`roots_add` and the other [operations](docs/API.md).

## Command line

```sh
CyModelHubCli list --type lora          # installed LoRAs
CyModelHubCli where sd_xl_base_1.0      # path of the best installed match
CyModelHubCli add-root /data/models     # watch a folder
CyModelHubCli install TheOrg/Model-GGUF model.Q4_K_M.gguf
```

## Languages

The interface text is English in the source and translated by plain JSON files in
[locales](locales). To add a language, copy `fr.json`, translate the values and drop the
file next to the others: it appears in **Settings → Language**.
`python scripts/check_locales.py` lists missing entries.

## Build

```powershell
./scripts/build.ps1        # Windows: service, client, CLI, MCP relay, application, tests
```

[Build instructions for Windows, Linux and macOS](BUILD.md).

## License

Copyright (c) 2026 Cyberalien.

- **The service, the desktop application, the command line tool and the MCP relay** are
  licensed under [GPL-3.0-only](LICENSE).
- **The client library** — `include/cymodelhub/client.hpp`, `src/client.cpp`, `src/platform.*`,
  the build script and `examples/client` — is licensed under [MIT](LICENSE-MIT). Any
  application, open or closed, may link it under the MIT terms; it reaches the service as a
  separate program, over local HTTP. Keep the copyright and permission notice with your
  distribution.

Each source file states its license in its `SPDX-License-Identifier` line. CyModelHub builds on CyUI (MIT), CyMCPSDK (MIT), Dear ImGui (MIT),
SDL (zlib), nlohmann/json (MIT), cpp-httplib (MIT) and PicoSHA2 (MIT); their licenses are
copied next to the application at build time.
