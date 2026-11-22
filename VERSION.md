# CyModelHub release notes

[Français](VERSION.fr.md)

## 0.1.0 — First working version

**IN DEVELOPMENT.** Built and tested on Windows 10 x64 with MSVC 19.40.

- Local service with a persistent catalogue, started on demand, reachable on the loopback
  interface with an access token.
- Automatic discovery of the Hugging Face cache, Ollama and LM Studio stores, plus any watched
  folder; manual addition of a file or a model folder.
- Classification by role, domain and family from GGUF and safetensors headers, Diffusers,
  Transformers and MLX folders, then folder and file names. Manual corrections are kept.
- Resolve a model by name, file name, repository, SHA-256 or ID.
- Tags, notes, favourites, duplicates, SHA-256 on request.
- Move a model (also across drives), hard and symbolic links into other folders.
- Installation from Hugging Face or an HTTPS address, with resume and SHA-256 verification.
- C++ client library, command line tool, MCP over HTTP and an MCP stdio relay.
- The client library is self-contained: the C++ standard library, the operating system and a
  bundled copy of nlohmann/json. Embedding it in an application downloads nothing.
- Shared installations: applications that ask for the same model join one installation and
  get the same result; each has a ticket, and the work is cancelled only when the last
  requester leaves. An installation interrupted by a stop of the service is taken up again.
- Idempotency keys on every operation that changes something: a request sent again after a
  lost answer has no second effect; the same key with other arguments is a conflict. Kept 7
  days, across restarts. The client no longer repeats a write that carries no key.
- Complete models: a contract lists every file a model needs, with sizes and SHA-256. A model
  is installed in a staging folder and published only once every file passed its check, or
  registered where an application already has it, without copy or move.
- Strict resolution (`models_check`, `Client::check`): ready, absent, installing, incomplete,
  invalid, unverified, incompatible or ambiguous, never "the closest match".
- Real recognition of Fermion speech models (`.fermion`): the container index is read and
  checked against its size, its manifest and its configuration.
- Desktop application built with CyUI, in English and French.
- Windows notification area icon to reopen the application, open Settings or quit. Closing
  the window hides it in the notification area.
- Offline CyDoc manual in English and French, available from Settings or with **F1**.
- Licensing: GPL-3.0-only, except the client library, which is MIT so that any application
  may link it.

### Known limits

- **Linux and macOS have not been compiled or run.** The code avoids platform-specific
  calls outside `src/platform.cpp`, but only Windows was verified.
- The application was checked through its screens (models, locations, duplicates, activity,
  dialogs) in both languages; each button drives a service operation covered by the tests,
  but the clicks themselves were not exercised by an automated test.
- Detection is heuristic. Safetensors files with an unusual layout, pickle files and ONNX
  models are classified from their folder and file names only.
- Messages that come from the service (errors, job titles) are in English.
- The service does not start with the session; it starts when an application needs it.
- Installation from Hugging Face was tested against a local server that imitates its
  redirect and hash header, not against huggingface.co itself.
- No deletion of model files, no automatic replacement of duplicates by links, no Civitai
  search yet.
- A damaged file of an installed model is reported, never repaired: someone has to replace
  it. Staging folders and partial files of installations that never complete are left in place.
- Complete models: the files of a repository are not discovered (the caller lists them),
  dependencies are recorded but not installed, and Fermion's download archives (`.fsq`,
  `.bps.tar.zst`) are not unpacked: the application unpacks, then registers the folder.
- The tests of shared installations use fixtures and a local server only. Installation of a
  complete model from huggingface.co itself was not exercised.
- The desktop application does not show the state of a model (ready, incomplete, invalid) yet.
- See [docs/INTEGRATION.md](docs/INTEGRATION.md) for the limits of the integration path.
