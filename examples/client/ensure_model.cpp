// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

// How an application gets exactly the model it needs through CyModelHub, without ever
// keeping a private copy of the weights:
//
//   1. ask whether exactly this model is ready (strict check);
//   2. ready: use its shared path;
//   3. absent or being installed: have CyModelHub install it, joining whoever started;
//   4. or: download it yourself, then register the complete model with CyModelHub.
//
//   ensure_model                      the Hub installs the model when it is missing
//   ensure_model --own <folder>       register a model this program already has in <folder>
//
// The model below is an example: replace the repository, the revision and the files, with
// their sizes and SHA-256, by those of the model your application ships with.

#include <cymodelhub/client.hpp>

#include <iostream>

namespace {
using cymodelhub::Availability;

// What the application needs, written once: it is both what is asked and what is installed.
cymodelhub::Package wanted() {
    cymodelhub::Package package;
    package.name = "Example Speech";
    package.repository = "example-org/example-speech";
    package.revision = "0123456789abcdef0123456789abcdef01234567"; // a commit: cannot change
    package.variant = "int6";
    package.application = "ensure_model";
    package.entry = ""; // the engine opens the folder
    package.files = {
        {"model.fermion", 177000000, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
         "weights"},
        {"config.json", {}, {}, "config"},
        {"packed_manifest.json", {}, {}, "manifest"},
    };
    return package;
}

int use(const std::filesystem::path& model) {
    // Hand this path to the engine. It is shared: never write into it, never copy it into a
    // per-task workspace, and do not delete it when the task ends.
    std::cout << model.string() << std::endl;
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    const auto package = wanted();
    cymodelhub::Requirement need;
    need.repository = package.repository;
    need.revision = package.revision;
    need.variant = package.variant;
    need.files = package.files;
    need.integrity = "hashes"; // the weights must have been read and matched, not merely seen

    try {
        cymodelhub::Client hub; // starts the service when it is not running
        auto found = hub.check(need);

        // Present but never read: have it read once, then ask again.
        if (found.status == Availability::unverified && found.model) {
            hub.wait(hub.verify(found.model->id));
            found = hub.check(need);
        }
        switch (found.status) {
        case Availability::ready:
            return use(found.entry);

        case Availability::absent:
        case Availability::installing:
        case Availability::incomplete: {
            if (argc > 2 && std::string(argv[1]) == "--own") {
                // The application downloaded the model itself: tell the Hub where it is.
                // Nothing is copied or moved; every file is read once to check it.
                const auto model = hub.wait(hub.register_model(argv[2], package));
                if (model.state == "ready") return use(model.path);
                std::cerr << "The model in " << argv[2] << " is " << model.state << '\n';
                return 3;
            }
            // Let the Hub install it. If another application already started, this joins its
            // installation: one download, one copy on disk, the same result for everyone.
            cymodelhub::InstallRequest request;
            request.package = package;
            const auto installation = hub.install(request);
            if (installation.joined) std::cerr << "Joining an installation in progress\n";
            const auto model = hub.wait(installation, std::chrono::hours(6), [](const cymodelhub::Json& job) {
                if (job.value("total", 0ULL))
                    std::cerr << '\r' << job.value("done", 0ULL) * 100 / job.value("total", 1ULL) << " %"
                              << std::flush;
            });
            return use(model.path);
        }

        case Availability::invalid:
            // A size or a hash does not match. Nothing is repaired or deleted automatically.
            std::cerr << "The installed model failed its validation: " << found.problems.dump() << '\n';
            return 4;
        case Availability::incompatible:
            for (const auto& reason : found.reasons)
                std::cerr << "Not the model asked for: " << reason << '\n';
            return 5;
        case Availability::ambiguous:
            std::cerr << "Several different models answer; say which one with its id, revision or hashes\n";
            return 6;
        case Availability::unverified:
            std::cerr << "The model could not be verified\n";
            return 7;
        }
    } catch (const cymodelhub::Error& error) {
        // ServiceUnavailable: CyModelHub is not installed or could not start.
        // ProviderAccessRequired: the provider wants a token (InstallRequest::token).
        // HashMismatch, SizeMismatch, ValidationFailed, ModelIncomplete, DownloadFailed, Cancelled.
        std::cerr << error.code << ": " << error.what() << '\n';
        return error.code == "ServiceUnavailable" ? 2 : 8;
    }
    return 1;
}
