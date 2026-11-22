// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "service.hpp"

namespace modelhub {
// The watched location that contains a path; the deepest one when locations are nested.
std::optional<Root> root_of(const Hub& hub, const fs::path& path);
// An absolute path given by a caller. Throws InvalidPath.
fs::path absolute_path(const Json& arguments, const char* key);
// The same, inside a registered location: files are only ever written there. Throws
// OutsideLocations.
fs::path destination(const Hub& hub, const Json& arguments, const char* key);

// install_start. `check_install` validates the request without any effect; it runs before an
// idempotency key is looked up, so a remembered answer is never given to a request that
// would be refused today.
void check_install(const Hub& hub, const Json& arguments);
Json start_install(Hub& hub, const Json& arguments);
// models_register: a complete model an application already has, catalogued where it is.
void check_registration(const Hub& hub, const Json& arguments);
Json start_registration(Hub& hub, const Json& arguments);
// models_verify: presence, sizes and, on request, hashes of an installed model.
Json start_verification(Hub& hub, const Json& arguments);
// models_check: the strict answer to "can I use this model now?".
Json check_model(const Hub& hub, const Json& arguments);
} // namespace modelhub
