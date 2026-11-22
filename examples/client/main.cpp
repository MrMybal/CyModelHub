// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: MIT

// Ask CyModelHub where a model is, instead of installing another copy.
//
//   find_model                      lists the installed language models
//   find_model sd_xl_base_1.0       prints the path of the best installed match

#include <cymodelhub/client.hpp>

#include <iostream>

int main(int argc, char** argv) {
    try {
        cymodelhub::Client hub; // starts the service when it is not running

        if (argc > 1) {
            const auto model = hub.resolve(argv[1]);
            if (!model) {
                std::cerr << argv[1] << " is not installed on this computer\n";
                return 1;
            }
            std::cout << model->name << " (" << model->type << ", " << model->architecture << ")\n"
                      << model->path.string() << std::endl;
            return 0;
        }

        cymodelhub::Query query;
        query.modality = "text";
        query.sort = "size";
        query.descending = true;
        std::size_t total = 0;
        for (const auto& model : hub.list(query, &total))
            std::cout << model.name << "  [" << model.format << ' ' << model.quantization << "]  "
                      << model.path.string() << '\n';
        std::cout << total << " language models installed" << std::endl;
        return 0;
    } catch (const cymodelhub::Error& error) {
        // ServiceUnavailable: CyModelHub is not installed, or its service could not start.
        std::cerr << error.code << ": " << error.what() << '\n';
        return 2;
    }
}
