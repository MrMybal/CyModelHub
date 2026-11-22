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
  `src/platform.*`, `CMakeLists.txt`, `examples/client`, `third_party/nlohmann`); everything
  else is GPL-3.0-only. The targets `cymodelhub_client` and `modelhub_platform` must never
  compile or include a GPL source: shared code needed by the client goes into
  `src/platform.*`. Give each new file the matching SPDX line.
- The client library is self-contained. Those two targets use the C++ standard library, the
  operating system and `third_party/nlohmann/json.hpp` only: no CyMCPSDK, no other download.
  CyMCPSDK is for the service, the MCP relay and the tests. `third_party/nlohmann` must stay
  the same release as the copy in the pinned CyMCPSDK (CMake checks it).
- After changing an operation: `python scripts/make_api_doc.py build/release/CyModelHubCli.exe`.
  Keep `docs/INTEGRATION.md` and `docs/INTEGRATION.fr.md` true to what the operations do.
- Operations on shared models live in `src/installs.cpp` (install, register, verify, strict
  check), `src/package.cpp` (the complete-model contract and its checks) and
  `src/coordination.cpp` (idempotency ledger, installation reservations).
- Rules for shared installations: one installation per artifact identity (join, never
  duplicate); the reservation is released by the job itself when it has stopped; a model is
  published only after every required file passed its check; a write operation accepts an
  `idempotencyKey` and its checks run before a remembered answer is given; the client repeats
  a request only when that cannot repeat an effect.
- Never announce a model ready because a file or a folder exists, and never treat the
  names-and-sizes fingerprint as an integrity check.
- A format is recognised from its real layout (see `inspect_fermion` in `src/formats.cpp`):
  read headers and indexes only, never load weights, never invent a header.
- Safety rules that must hold: never delete or overwrite a model file; write only inside
  registered locations (`destination()` in `src/installs.cpp`); paths of a package go through
  `package_file()`; never unpickle or load weights; the token in `service.json` stays
  owner-only and is never logged; a download token or a query string is never stored, shown
  or put in a fingerprint.
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
