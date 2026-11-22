# Integrating an application with CyModelHub

[Français](INTEGRATION.fr.md) · [All operations](API.md) · [C++ header](../include/cymodelhub/client.hpp) ·
[Complete example](../examples/client/ensure_model.cpp)

This guide is for an application, or a host of tools such as CyToolsCore, that needs **one
exact model** and must not keep a private copy of it. The service is common to every
application of the user; the weights it shares are persistent.

Everything below exists in three forms with the same names: the C++ client
(`cymodelhub::Client`), local HTTP (`POST <url>/v1`) and MCP tools.

## The path, in short

```cpp
#include <cymodelhub/client.hpp>
using cymodelhub::Availability;

cymodelhub::Package package;                 // what the application needs, written once
package.repository = "org/name";
package.revision   = "<commit>";             // a commit cannot change; a branch can
package.variant    = "int6";
package.files = {{"model.fermion", 177000000, "<sha256>", "weights"},
                 {"config.json", {}, {}, "config"},
                 {"packed_manifest.json", {}, {}, "manifest"}};

cymodelhub::Requirement need;                // 1. look for exactly this model
need.repository = package.repository;
need.revision   = package.revision;
need.variant    = package.variant;
need.files      = package.files;
need.integrity  = "hashes";

cymodelhub::Client hub;                      // starts the service when it is not running
auto found = hub.check(need);
if (found.status == Availability::unverified && found.model) {
    hub.wait(hub.verify(found.model->id));   // present but never read: read it once
    found = hub.check(need);
}
if (found.ready()) {
    run(found.entry);                        // 2. use the shared path, as it is
} else if (found.status == Availability::absent || found.status == Availability::installing ||
           found.status == Availability::incomplete) {
    cymodelhub::InstallRequest request;      // 3. install it, or join whoever started
    request.package = package;
    run(hub.wait(hub.install(request)).path);
    // 4. or: download it yourself, then
    //    run(hub.wait(hub.register_model(folder, package)).path);
} else {
    report(found.status_name, found.reasons, found.problems);
}
```

[`examples/client/ensure_model.cpp`](../examples/client/ensure_model.cpp) is the same path as a
program that builds with the client library alone.

## Where shared weights live

- Models are installed inside **registered locations** only: CyModelHub's own store
  (`<data>/models`) or a folder added with `roots_add`. Any other destination is refused
  (`OutsideLocations`).
- A registered model stays where the application put it. Nothing is copied or moved.
- **Never give a per-task or temporary workspace** as a destination or as the folder of a
  registered model, and never register such a workspace as a location. Shared weights must
  outlive the task: a task receives a path to read, not a folder to own.
- CyModelHub never deletes a model file and never overwrites one. An application must not
  delete or rewrite a shared model either: other applications are using the same files.

## What the answers mean

`models_check` (`Client::check`) answers with a `status`. An installation answers with a job
whose failure has a `code`. The client throws `cymodelhub::Error` with the same codes.

| Situation | How it shows |
| --- | --- |
| The model is usable | `ready`, with `path`, `root` and `entry` |
| Model absent | `absent` |
| Service unavailable | `Error` `ServiceUnavailable` (not installed, or could not start) |
| Installation in progress | `installing`, with the `job` to join |
| Model incomplete | `incomplete`, with the missing files in `problems`; job code `ModelIncomplete` |
| Incompatibility | `incompatible`, with `reasons` (format, architecture, quantization, revision, variant) |
| Provider access required | job code `ProviderAccessRequired` (HTTP 401 or 403 from the source) |
| Validation failed | `invalid`; job codes `ValidationFailed`, `HashMismatch`, `SizeMismatch` |
| Not proven yet | `unverified`: present, but the evidence asked for was never gathered |
| Several different models answer | `ambiguous`, with `candidates`: none is chosen |
| The source failed | job code `DownloadFailed`; `Cancelled`; `Interrupted` after a stop of the service |

`models_resolve` (`Client::resolve`) is unchanged: it is the approximate search, for people
and for "something like this". It returns the best match by name and must not be used to
decide that one exact model is installed.

## Strict resolution

`models_check` takes what identifies the model and what it must be.

- **Which model**: `id`, or any of `repository`, `name`, `file`, `sha256`. Matching is exact
  (case-insensitive), never "contains" or "closest".
- **What it must be**: `format`, `architecture`, `quantization`, `type`, `modality`,
  `revision`, `variant`. A model that is found but differs is `incompatible`, with the reason.
  A branch name is accepted as `revision` when it is the one the model was installed from.
- **Its files**: `files`, with the `size` and `sha256` the application expects. A missing file
  gives `incomplete`; another size or hash gives `invalid`.
- **How sure**: `integrity`.
  - `none`: being on disk is enough. Use it only when presence is all you need.
  - `sizes` (default): expected sizes are known (from the request, or from the contract or
    manifest of the model) and match.
  - `hashes`: the SHA-256 of the required files was computed and they have not changed since.

A folder or a single file that was merely seen by a scan is `present`, not `ready`: with the
default `integrity` it is `unverified` until the application gives expected sizes or hashes, or
`models_verify` has read it. `models_check` itself reads no file content; it compares what was
recorded with the sizes and modification times on disk, so a file that vanished or changed is
seen at once.

When several installed models satisfy the request, copies of the same content count as one
model; different contents give `ambiguous`.

## Complete models

A model may need several files: weights, shards, an index, a configuration, a tokenizer, a
manifest. The contract (`package`) says so explicitly:

```json
{
  "identity":       { "name": "Nova", "repository": "org/nova", "revision": "<commit>", "variant": "q4" },
  "provenance":     { "source": "huggingface", "url": "https://...", "application": "CyToolsCore" },
  "classification": { "format": "gguf", "type": "model", "modality": "text",
                      "architecture": "llama", "quantization": "Q4_K_M" },
  "entry":          "weights/model-00001-of-00002.gguf",
  "files": [
    { "path": "weights/model-00001-of-00002.gguf", "size": 4368439584, "sha256": "...", "role": "weights" },
    { "path": "weights/model-00002-of-00002.gguf", "size": 2147483648, "sha256": "...", "role": "shard" },
    { "path": "config.json", "role": "config" },
    { "path": "extras/readme.md", "role": "notes", "required": false }
  ],
  "dependencies": [ { "repository": "org/nova-vae", "revision": "<commit>", "required": true } ]
}
```

Only `files` is required. Paths are relative to the model folder, with forward slashes; a
path with a drive, a leading slash, `..`, a backslash or a reserved name is refused
(`InvalidManifest`), and a link that would lead a download out of the folder is refused
(`UnsafePath`). `entry` is the file the engine opens; leave it empty when the engine takes the
folder. Dependencies are recorded with the model; they are not installed for you.

After a check the model has:

- a **state**: `ready`, `incomplete` (a required file is missing) or `invalid` (a size or a
  hash does not match, or the structure of a known format is wrong);
- an **integrity**: `none`, `sizes` (the announced sizes match), `recorded` (every required
  file was read and its SHA-256 stored, at least one without a reference) or `verified` (every
  required file matched a SHA-256 given by the contract or announced by the source);
- per file, in `package.files`: its state, its size and hash, where each comes from
  (`manifest` or `measured`), and when the hash was last checked.

The structural fingerprint of a folder (names and sizes) is only used to spot copies. It is
never taken as an integrity check.

### Registering a model the application already has

`models_register` (`Client::register_model`) takes the folder and the contract. Every listed
file is looked for under the folder; its size is compared; unless `verify` is `sizes`, it is
read once to compute its SHA-256. The job ends `done` with the model **and its state**: a model
with a missing or wrong file is catalogued as `incomplete` or `invalid`, never as ready. Nothing
is copied, moved or rewritten, and entries of single files inside the folder are merged into
the one entry of the model.

### Having the Hub install it

`install_start` with a `package` (`Client::install`) downloads each file. Without `baseUrl`
the files come from the Hugging Face repository of the identity, at its revision; with
`baseUrl` they come from `baseUrl/path` (a mirror, or `http://127.0.0.1` for a local source).

- The model is assembled in `<name>.incomplete`, beside its final place.
- Each file resumes from its `.part` when the source allows ranges; files already complete
  are not fetched again.
- When all files are there, **every one is read and checked**. Only then is the folder renamed
  to its name and the model published as `ready`. Until then `models_check` says `absent` or
  `installing`.
- A failed or interrupted installation leaves its staging folder; the next request for the
  same model continues it. A file of the staging folder that fails its check is removed so
  that it is downloaded again.
- A published model that lost a file is completed where it is by the same request. A
  published file that is damaged is **never overwritten**: the job fails with
  `ValidationFailed` and the model stays `invalid` until someone replaces the file.

`models_verify` (`Client::verify`) checks an installed model again, on request.

## One installation for everyone

Each installation has an **identity**:

| Kind | Identity |
| --- | --- |
| One file from a repository | repository, revision (`main` when omitted), file, expected SHA-256, variant |
| One file from an address | the address without credentials, expected SHA-256, variant |
| A complete model | repository (or the base address when there is none), revision, variant, and the list of its files with their expected SHA-256 |

The display name, the folder name and the destination are not part of it. Give a commit as
`revision` for an identity that cannot change; with a branch, the commit the source resolved
it to is recorded with the model when the source announces it.

- A request for a model **that is being installed joins that installation**: same `job`,
  `joined: true`, and a `ticket` of its own. All requesters get the same result.
- A request for a model **that is installed** gets it at once: `installed: true`, the `model`,
  and a job that is already done, so every caller can wait the same way.
- **Another destination never makes a second copy.** The answer names the one place
  (`destination`) and says `destinationHonored: false`. Use `models_link` to show the model in
  another folder without copying it.
- Two different models cannot be written to the same place at once (`DestinationBusy`).
- The reservation lasts **until the job has really stopped** and its files are final.
- It is written to `<data>/installs.json`. If the service stops or is killed, the next service
  lists the installation as `interrupted` (`installs_list`), and the next request for the same
  model continues it where its partial files are (`resumed: true`).
- **Cancelling**: a requester calls `install_leave` with its ticket (`Client::leave`). The work
  goes on for the others and is cancelled only when the last one leaves. `jobs_cancel` on a
  job that several requesters wait for answers `shared: true` and stops nothing, unless
  `force` is given (the desktop application does, on the user's explicit request).

## Requests that may be sent twice

A request that changes something can carry an `idempotencyKey`: 8 to 128 characters among
letters, digits, `.`, `_`, `:` and `-`, chosen at random, holding no secret.

| Sent again with | Result |
| --- | --- |
| the same key and the same arguments | the first answer, with `replayed: true`; no second effect. For a job, the same `job` and, once it ended, its `outcome` |
| the same key and other arguments | `IdempotencyConflict` |
| the same key after the service stopped in the middle of its job | the work is taken up again (`resumed: true`) |

- Keys are kept **7 days** (2000 at most, the oldest leave first) in `<data>/requests.json`,
  readable by the user only, and survive restarts. `jobs_get` still answers for the job of a
  keyed request after a restart.
- The key is stored as its SHA-256. The fingerprint of a request is a hash of its arguments
  **without `token`**. Neither can be used to recover a credential.
- A request that was refused (bad arguments, destination outside the locations) does not use
  up its key.
- The checks of an operation are applied again before a remembered answer is given: a
  remembered `install_start` whose destination is no longer a registered location is refused.

The C++ client follows this. `install()`, `register_model()` and `verify()` create a key and
send it again unchanged. `invoke()` repeats a request after a network error or a timeout only
when that cannot repeat an effect: the request never left the program, the operation only
reads, or the arguments carry an `idempotencyKey`. Otherwise the error reaches the caller,
who knows whether sending again is acceptable.

## Fermion speech models (`.fermion`)

A Fermion model (Phonon family, `fermion-research`) is a **folder** of three files:
`model.fermion`, `config.json` and `packed_manifest.json`. The engine is given the folder.

CyModelHub recognises the real layout of the container, which has no magic number: an
unsigned 64-bit little-endian length, a JSON index `{"format", "index": [{"n", "k", "shape",
"b"}]}`, then the records. Reading the index alone (a few kilobytes), it checks that:

- the index fits in the file and is the expected JSON;
- the format is a `fermion-` format; for `fermion-five-value-parakeet-v1` each record kind
  (`fp16`, `int6`, `int8`, `five_value`) has a length its shape allows;
- the file is exactly as long as the length field, the index and the records together: a
  truncated file and trailing bytes are both refused.

For the folder it also checks that the manifest, the configuration and the container agree
(format, size, hash), and that the container has the size the manifest announces. The SHA-256
of the container is compared with `container_sha256` by `models_verify` or
`models_register`. Weights are never loaded and nothing is deserialised beyond JSON.

- A valid folder is one catalogue entry: format `fermion`, architecture `parakeet`, modality
  `audio`, quantization such as `five-value-int6`, state `ready`, integrity `sizes` until its
  hashes are checked. `path` and `entry` are the folder.
- A container alone is catalogued as `incomplete`: its configuration and manifest are needed.
- A truncated container, a manifest that announces another size or format, or a
  configuration that contradicts the manifest make the model `invalid`.

Fermion distributes these models as archives (`.fsq`, `.bps.tar.zst`) that its own tool
unpacks. CyModelHub does not unpack them: for these models the application downloads and
unpacks as it does today, then registers the folder (`models_register`). Other applications
then find it with `models_check`.

## Credentials

- `token` (for gated downloads) is sent to the origin of the address it was given for, and to
  no other host a redirect leads to. It is never written to disk, logged, or returned.
- `HF_TOKEN` is used for `huggingface.co` when no token is given.
- An address is stored and shown without its query, fragment and user part. A credential in a
  query string is used for the download and not kept.

## Limits

- Built and tested on Windows only. The Linux and macOS code has not been compiled.
- Installing from `huggingface.co` itself was not exercised: the tests use a local server.
  The files of a repository are not discovered: the caller lists them.
- Dependencies are recorded, not resolved or installed.
- Nothing is repaired by deleting or overwriting. Staging folders (`.incomplete`) and partial
  files (`.part`) of installations that never complete stay until someone removes them.
- A requester that disappears without calling `install_leave` keeps its ticket: the
  installation completes, but the remaining requesters cannot cancel it without `force`.
- A change of content is noticed through the size and the modification time (one-second
  resolution). `models_verify` reads the files and is the real check.
- Only the record kinds of `fermion-five-value-parakeet-v1` are known. Another `fermion-`
  format is checked for its overall size only.
- The desktop application does not show the new states yet; it lists complete models as one
  entry.
