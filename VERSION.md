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
- Desktop application built with CyUI, in English and French.
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
