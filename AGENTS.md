# Agents

CyModelHub: a local service that catalogues the AI models installed on a computer and shares
them between applications, with a CyUI desktop application, a C++ client library, a CLI and
MCP access. Read `README.md` first, then `BUILD.md`.

- Build and test on Windows with `./scripts/build.ps1` (CMake is not on the PATH; the script
  finds the one shipped with Visual Studio). Stop a running service before rebuilding:
  `CyModelHubCli stop`.
- The service owns the catalogue. The application, the CLI and the MCP relay only go through
  `CyModelHub::Client`; add a feature as a service operation in `src/service.cpp` first.
- Two licenses. The client library is MIT (`include/cymodelhub/client.hpp`, `src/client.cpp`,
  `src/platform.*`, `CMakeLists.txt`, `examples/client`); everything else is GPL-3.0-only.
  The targets `cymodelhub_client` and `modelhub_platform` must never compile or include a GPL
  source: shared code needed by the client goes into `src/platform.*`. Give each new file the
  matching SPDX line.
- After changing an operation: `python scripts/make_api_doc.py build/release/CyModelHubCli.exe`.
- Safety rules that must hold: never delete a model file; write only inside registered
  locations (`destination()` in `src/service.cpp`); never unpickle or load weights; the token
  in `service.json` stays owner-only and is never logged.
- Text coming from the operating system goes through `system_text()` before it reaches JSON.
- Interface strings are English in `src/app_ui.cpp` (`T()`, `L()`) and translated in
  `locales/*.json`; run `python scripts/check_locales.py` after adding one.
- Follow CyUI's rules in the application: colours from `cyui::palette()`, sizes through
  `cyui::dp()`. Check a screen with
  `CyModelHub --data <dir> --capture out.png [--page n] [--dialog add|install|settings] [--locale fr]`.
- Platform-specific code belongs in `src/platform.cpp`. Linux and macOS are targets but have
  not been built yet: say so rather than claiming they work.
- Tests must not read the user's models: use `fixtures::sandbox()` or the same environment
  overrides.
