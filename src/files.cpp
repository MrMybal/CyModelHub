// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "files.hpp"

#include <fstream>

namespace modelhub {
namespace {
std::uint64_t tree_size(const fs::path& path) {
    std::error_code error;
    if (fs::is_regular_file(path, error)) return fs::file_size(path, error);
    std::uint64_t total = 0;
    for (fs::recursive_directory_iterator item(path, fs::directory_options::skip_permission_denied, error), end;
         !error && item != end; item.increment(error)) {
        std::error_code ignored;
        if (item->is_regular_file(ignored)) total += item->file_size(ignored);
    }
    return total;
}

// Chunked copy so a large model reports progress and can be cancelled.
void copy_file_checked(const fs::path& from, const fs::path& to, const Job& job, std::uint64_t& done,
                       std::uint64_t total) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) throw cymcp::Error("CopyFailed", "Cannot copy " + to_utf8(from.filename()));
    std::vector<char> buffer(4 * 1024 * 1024);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        job.cancellation().check();
        out.write(buffer.data(), in.gcount());
        if (!out) throw cymcp::Error("CopyFailed", "Cannot write " + to_utf8(to.filename()));
        done += static_cast<std::uint64_t>(in.gcount());
        job.update(done, total, to_utf8(from.filename()));
    }
    out.close();
    std::error_code error;
    if (!out || fs::file_size(from, error) != fs::file_size(to, error))
        throw cymcp::Error("CopyFailed", "The copy of " + to_utf8(from.filename()) + " is incomplete");
}

void copy_tree(const fs::path& from, const fs::path& to, const Job& job, std::uint64_t& done, std::uint64_t total) {
    std::error_code error;
    if (fs::is_regular_file(from, error)) {
        copy_file_checked(from, to, job, done, total);
        return;
    }
    fs::create_directories(to);
    for (fs::directory_iterator item(from), end; item != end; ++item) {
        const auto target = to / item->path().filename();
        if (item->is_directory()) copy_tree(item->path(), target, job, done, total);
        else if (item->is_regular_file()) copy_file_checked(item->path(), target, job, done, total);
    }
}
} // namespace

fs::path move_model(const Model& model, const fs::path& directory, bool leave_link, const Job& job,
                    std::string& warning) {
    std::error_code error;
    if (!fs::exists(model.path, error)) throw cymcp::Error("NotFound", "The model is no longer on disk");
    if (path_within(directory, model.path))
        throw cymcp::Error("InvalidPath", "The destination is inside the model itself");
    fs::create_directories(directory, error);
    if (error) throw cymcp::Error("WriteFailed", "Cannot create the destination folder (" + system_text(error.message()) + ")");
    const auto target = directory / model.path.filename();
    if (path_key(target) == path_key(model.path)) return model.path;
    if (fs::exists(target, error)) throw cymcp::Error("AlreadyExists", "The destination already has this name");
    fs::rename(model.path, target, error);
    if (error) {
        // Another drive: copy, compare sizes, and only then remove the original.
        const auto total = tree_size(model.path);
        std::uint64_t done = 0;
        try {
            copy_tree(model.path, target, job, done, total);
        } catch (...) {
            std::error_code cleanup;
            fs::remove_all(target, cleanup);
            throw;
        }
        if (tree_size(target) != total) {
            std::error_code cleanup;
            fs::remove_all(target, cleanup);
            throw cymcp::Error("CopyFailed", "The copy does not match the original; nothing was removed");
        }
        fs::remove_all(model.path, error);
        if (error) warning = "The original could not be removed: " + to_utf8(model.path);
    }
    if (leave_link && warning.empty()) {
        if (model.folder) fs::create_directory_symlink(target, model.path, error);
        else fs::create_symlink(target, model.path, error);
        if (error) warning = "The link at the old location could not be created (" + system_text(error.message()) + ")";
    }
    return target;
}

fs::path link_model(const Model& model, const fs::path& directory, const std::string& mode) {
    std::error_code error;
    // Link to the real file, not to another link (Hugging Face snapshots are links to blobs).
    const auto source = fs::canonical(model.path, error);
    if (error) throw cymcp::Error("NotFound", "The model is no longer on disk");
    fs::create_directories(directory, error);
    if (error) throw cymcp::Error("WriteFailed", "Cannot create the destination folder (" + system_text(error.message()) + ")");
    const auto target = directory / model.path.filename();
    if (fs::exists(target, error) || fs::is_symlink(target, error))
        throw cymcp::Error("AlreadyExists", "The destination already has this name");
    if (mode == "hard") {
        if (model.folder) throw cymcp::Error("InvalidMode", "A folder needs a symbolic link");
        fs::create_hard_link(source, target, error);
        if (error) throw cymcp::Error("LinkFailed", "Hard link failed; both places must be on the same drive (" +
                                                        system_text(error.message()) + ")");
    } else {
        if (model.folder) fs::create_directory_symlink(source, target, error);
        else fs::create_symlink(source, target, error);
        if (error) throw cymcp::Error("LinkFailed", "Symbolic link failed (" + system_text(error.message()) +
                                                        "). On Windows this needs Developer Mode or elevation");
    }
    return target;
}
} // namespace modelhub
