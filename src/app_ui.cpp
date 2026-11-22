// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#include "app_ui.hpp"
#include "platform.hpp"

#include <SDL3/SDL.h>
#include <cyui/Core.h>

#include <algorithm>
#include <ctime>

namespace app {
namespace {
namespace fs = std::filesystem;
namespace icon = cyui::icon;

// Interface text is written in English and translated through the catalogues in locales/.
std::string T(const char* text) { return cyui::tr(text); }
// Same, for clickable labels: the widget identity stays the English text.
std::string L(const char* text) { return cyui::trLabel(text); }

struct Choice {
    const char* value;
    const char* label;
    const char* icon;
};
constexpr Choice modalities[] = {
    {"text", "Text (LLM)", icon::document}, {"image", "Image", icon::photo},
    {"video", "Video", icon::video},        {"audio", "Audio", icon::audio},
    {"embedding", "Embeddings", icon::connect}, {"multimodal", "Multimodal", icon::view},
    {"3d", "3D", icon::grid},               {"other", "Other", icon::more}};
constexpr Choice types[] = {
    {"model", "Model", icon::library},           {"lora", "LoRA", icon::presets},
    {"vae", "VAE", icon::colour},                {"controlnet", "ControlNet", icon::ruler},
    {"text_encoder", "Text encoder", icon::code}, {"vision_encoder", "Vision encoder", icon::camera},
    {"embedding", "Embedding", icon::tag},       {"upscaler", "Upscaler", icon::zoomIn},
    {"adapter", "Adapter", icon::link},          {"projector", "Projector", icon::share},
    {"other", "Other", icon::more}};
constexpr Choice sources[] = {
    {"huggingface", "Hugging Face", icon::cloud}, {"ollama", "Ollama", icon::cloud},
    {"lmstudio", "LM Studio", icon::cloud},       {"managed", "CyModelHub", icon::home},
    {"folder", "Folders", icon::folder},          {"manual", "Added by hand", icon::pin}};

template <std::size_t N> std::string label_of(const Choice (&choices)[N], const std::string& value) {
    for (const auto& choice : choices)
        if (value == choice.value) return T(choice.label);
    return value;
}

template <std::size_t N> int index_of(const Choice (&choices)[N], const std::string& value) {
    for (std::size_t index = 0; index < N; ++index)
        if (value == choices[index].value) return static_cast<int>(index);
    return static_cast<int>(N) - 1;
}

std::string human_size(std::uint64_t bytes) {
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof(text), unit < 2 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return text;
}

std::string date(std::int64_t seconds) {
    if (seconds <= 0) return "-";
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

void copy_text(char* buffer, std::size_t size, const std::string& value) {
    std::snprintf(buffer, size, "%s", value.c_str());
}

std::string joined(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) result += (result.empty() ? "" : ", ") + value;
    return result;
}

std::string file_url(const fs::path& path) {
    const auto utf8 = path.generic_u8string();
    std::string url = utf8.starts_with(u8"//") ? "file:" : utf8.starts_with(u8"/") ? "file://" : "file:///";
    for (const unsigned char c : utf8) {
        if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') {
            url += static_cast<char>(c);
        } else {
            char encoded[4];
            std::snprintf(encoded, sizeof(encoded), "%%%02X", c);
            url += encoded;
        }
    }
    return url;
}

// Show a folder in the system file manager.
void reveal(const fs::path& path) {
    std::error_code error;
    const auto folder = fs::is_directory(path, error) ? path : path.parent_path();
    SDL_OpenURL(file_url(folder).c_str());
}

// Native file dialogs answer later, possibly on another thread.
struct Browse {
    std::mutex mutex;
    std::string path;
    int target = 0;
} browse;

void SDLCALL browsed(void* target, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lock(browse.mutex);
    browse.path = files[0];
    browse.target = static_cast<int>(reinterpret_cast<std::intptr_t>(target));
}

enum BrowseTarget { browse_none, browse_add_field, browse_location };
enum Page { page_models, page_duplicates, page_locations, page_jobs };
} // namespace

void load_locales() {
    std::error_code error;
    const auto folder = modelhub::executable_path().parent_path() / "locales";
    for (fs::directory_iterator item(folder, error), end; !error && item != end; item.increment(error)) {
        if (item->path().extension() != ".json") continue;
        const auto content = modelhub::read_file(item->path(), 4 * 1024 * 1024);
        const auto json = content ? Json::parse(*content, nullptr, false) : Json();
        if (!json.is_object() || !json.contains("translations") || !json["translations"].is_object()) continue;
        const auto code = json.value("locale", std::string());
        if (code.empty()) continue;
        cyui::TranslationCatalog catalog;
        for (const auto& [key, value] : json["translations"].items())
            if (value.is_string()) catalog[key] = value.get<std::string>();
        const auto name = json.value("name", code);
        cyui::registerTranslations(code.c_str(), catalog, name.c_str());
    }
}

std::string system_locale() {
    int count = 0;
    std::string result = "en";
    if (auto** locales = SDL_GetPreferredLocales(&count)) {
        const auto available = cyui::availableLocales();
        for (int index = 0; index < count && locales[index]; ++index) {
            const std::string language = locales[index]->language ? locales[index]->language : "";
            if (std::any_of(available.begin(), available.end(),
                            [&](const cyui::LocaleInfo& info) { return info.code == language; })) {
                result = language;
                break;
            }
        }
        SDL_free(locales);
    }
    return result;
}

struct Ui::Impl {
    Data& data;
    Settings& settings;
    SDL_Window* window;
    SDL_Texture* logo;
    std::shared_ptr<const Snapshot> snap;

    Filter filter;
    char search[256]{};
    int page = page_models;
    std::string selected;
    Json detail; // full record of the selected model, once the service answered

    // Table order.
    std::vector<int> order;
    std::uint64_t order_generation = ~std::uint64_t{0};
    int sort_column = 0;
    bool sort_descending = false;

    // Edition of the selected model.
    std::string edit_id;
    char edit_name[256]{}, edit_family[64]{}, edit_tags[512]{}, edit_notes[4096]{};
    int edit_type = 0, edit_modality = 0;

    // Dialogs.
    bool show_add = false, show_install = false, show_move = false, show_link = false, show_settings = false,
         show_forget = false;
    char add_path[1024]{};
    int install_mode = 0, destination = 0, link_mode = 0;
    char install_repository[256]{}, install_file[512]{}, install_url[1024]{}, install_sha[80]{},
        install_token[256]{}, subfolder[256]{};
    bool leave_link = true, preset_select = false, capture_relative_paths = false;

    Impl(Data& d, Settings& s, SDL_Window* w, SDL_Texture* logo_texture)
        : data(d), settings(s), window(w), logo(logo_texture) {}

    void open_manual() {
        const auto directory = modelhub::executable_path().parent_path();
        const auto relative = fs::path("Documentation") / "Documentation.html";
        auto manual = directory / relative;
        std::error_code error;
        if (!fs::is_regular_file(manual, error))
            manual = directory.parent_path() / "Resources" / relative;
        const auto route = cyui::locale() == "fr" ? "#/fr/overview" : "#/en/overview";
        if (!fs::is_regular_file(manual, error) || !SDL_OpenURL((file_url(manual) + route).c_str()))
            cyui::notify(T("Could not open the user manual").c_str(), cyui::Tone::error);
    }

    std::string model_path_label(const Json& record) const {
        const auto path = record.value("path", std::string());
        if (!capture_relative_paths || path.empty()) return path;
        const auto file = modelhub::from_utf8(path);
        const auto root_id = record.value("root", std::string());
        for (const auto& root : snap->roots) {
            if (root_id.empty() || root.value("id", std::string()) != root_id) continue;
            const auto relative = file.lexically_relative(modelhub::from_utf8(root.value("path", std::string())));
            if (!relative.empty() && relative != "." && *relative.begin() != "..") return modelhub::to_utf8(relative);
            break;
        }
        return modelhub::to_utf8(file.filename());
    }

    // ---- Service requests ------------------------------------------------------------------

    void act(std::string success, std::function<Json(cymodelhub::Client&)> action) {
        data.run(std::move(action), [success = std::move(success)](const Json& result, const std::string& error) {
            if (!error.empty()) cyui::notify(error.c_str(), cyui::Tone::error, 7.0f);
            else if (result.is_object() && result.contains("warning"))
                cyui::notify(result["warning"].get<std::string>().c_str(), cyui::Tone::warning, 7.0f);
            else if (!success.empty()) cyui::notify(success.c_str(), cyui::Tone::success);
        });
    }

    void invoke(const char* success, std::string operation, Json arguments = Json::object()) {
        act(T(success), [operation = std::move(operation), arguments = std::move(arguments)](
                            cymodelhub::Client& client) { return client.invoke(operation, arguments); });
    }

    void select(const std::string& id) {
        selected = id;
        detail = Json();
        edit_id.clear();
        if (id.empty()) return;
        data.run([id](cymodelhub::Client& client) { return client.invoke("models_get", {{"id", id}}); },
                 [this, id](const Json& result, const std::string& error) {
                     if (error.empty() && id == selected && result.contains("model")) detail = result["model"];
                 });
    }

    void reload_detail() {
        if (!selected.empty()) select(std::string(selected));
    }

    const cymodelhub::Model* selected_model() const {
        for (const auto& model : snap->models)
            if (model.id == selected) return &model;
        return nullptr;
    }

    std::size_t facet(const char* group, const std::string& value) const {
        const auto found = snap->facets.find(group);
        if (found == snap->facets.end() || !found->is_array()) return 0;
        for (const auto& item : *found)
            if (item.value("value", std::string()) == value) return item.value("count", std::size_t{0});
        return 0;
    }

    // Locations that can receive files: not the stores owned by other tools.
    std::vector<const Json*> writable_roots() const {
        std::vector<const Json*> roots;
        for (const auto& root : snap->roots) {
            const auto kind = root.value("kind", std::string());
            if (root.value("enabled", false) && kind != "huggingface" && kind != "ollama") roots.push_back(&root);
        }
        return roots;
    }

    std::string destination_path() const {
        const auto roots = writable_roots();
        if (roots.empty()) return {};
        auto path = modelhub::from_utf8(roots[std::clamp(destination, 0, static_cast<int>(roots.size()) - 1)]->value(
            "path", std::string()));
        if (subfolder[0]) path /= modelhub::from_utf8(subfolder);
        return modelhub::to_utf8(path);
    }

    // ---- Shell -----------------------------------------------------------------------------

    void set_filter(Filter next, int target_page = page_models) {
        next.text = search;
        filter = std::move(next);
        page = target_page;
        data.set_filter(filter);
    }

    void top_bar() {
        if (cyui::beginTopBar("##top")) {
            if (cyui::searchField("##search", search, sizeof(search), T("Search models").c_str(), cyui::dp(340.0f))) {
                filter.text = search;
                page = page_models;
                data.set_filter(filter);
            }
            ImGui::SameLine();
            cyui::toolbarSeparator();
            ImGui::SameLine();
            if (cyui::iconTextButton(icon::refresh, L("Scan").c_str()))
                invoke("Scan started", "scan_start");
            cyui::tooltip(T("Look for models again in every location (F5)").c_str());
            ImGui::SameLine();
            if (cyui::iconTextButton(icon::add, L("Add...").c_str())) show_add = true;
            cyui::tooltip(T("Register a model file or watch a folder").c_str());
            ImGui::SameLine();
            if (cyui::iconTextButton(icon::download, L("Install...").c_str(), cyui::Variant::primary))
                show_install = true;
            cyui::tooltip(T("Download a model from Hugging Face or from an address").c_str());
            ImGui::SameLine();
            cyui::alignRight(ImGui::GetFrameHeight());
            if (cyui::iconButton(icon::settings, T("Settings").c_str())) show_settings = true;
        }
        cyui::endTopBar();
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
            invoke("Scan started", "scan_start");
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) open_manual();
    }

    const Json* running_job() const {
        for (const auto& job : snap->jobs)
            if (job.value("state", std::string()) == "running") return &job;
        return nullptr;
    }

    void status_bar() {
        if (cyui::beginStatusBar("##status")) {
            const auto& palette = cyui::palette();
            cyui::statusDot(snap->connected ? palette.success : palette.error);
            ImGui::SameLine();
            if (!snap->connected) {
                cyui::statusText(cyui::Tone::warning, "%s",
                                 (snap->error.empty() ? T("Starting the CyModelHub service...") : snap->error).c_str());
            } else {
                cyui::statusText(cyui::Tone::neutral, "%s  ·  %s  ·  %s",
                                 (std::to_string(snap->status.value("models", std::size_t{0})) + " " + T("models")).c_str(),
                                 human_size(snap->status.value("size", std::uint64_t{0})).c_str(),
                                 ("CyModelHub " + snap->status.value("version", std::string())).c_str());
                if (const auto* job = running_job()) {
                    const auto done = job->value("done", std::uint64_t{0}), total = job->value("total", std::uint64_t{0});
                    ImGui::SameLine(0.0f, cyui::dp(18.0f));
                    cyui::spinner("##busy", 5.0f);
                    ImGui::SameLine();
                    auto text = job->value("title", std::string());
                    if (job->value("kind", std::string()) == "install" || job->value("kind", std::string()) == "move")
                        text = job->value("message", text) + "  " + human_size(done) +
                               (total ? " / " + human_size(total) : std::string());
                    else if (done) text += "  " + std::to_string(done);
                    cyui::statusText(cyui::Tone::accent, "%s", text.c_str());
                    if (total) {
                        ImGui::SameLine();
                        cyui::progressBar(static_cast<float>(static_cast<double>(done) / static_cast<double>(total)),
                                          ImVec2(cyui::dp(160.0f), cyui::dp(6.0f)));
                    }
                }
            }
        }
        cyui::endStatusBar();
    }

    template <std::size_t N>
    void sidebar_choices(const char* title, const char* group, const Choice (&choices)[N], std::string Filter::*field) {
        bool header = false;
        for (const auto& choice : choices) {
            const auto count = facet(group, choice.value);
            if (!count) continue;
            if (!header) cyui::sidebarGroup(T(title).c_str());
            header = true;
            const bool active = page == page_models && filter.*field == choice.value;
            if (cyui::sidebarItem(choice.icon, L(choice.label).c_str(), active, std::to_string(count).c_str())) {
                Filter next;
                next.*field = choice.value;
                set_filter(std::move(next));
            }
        }
    }

    void sidebar() {
        if (cyui::beginSidebar("##nav", 236.0f)) {
            if (logo) {
                const float size = cyui::dp(28.0f);
                ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(logo)), ImVec2(size, size));
                ImGui::SameLine(0.0f, cyui::dp(6.0f));
                ImGui::AlignTextToFramePadding();
            }
            cyui::sidebarHeader("CYMODELHUB", CYMODELHUB_VERSION);
            const auto total = snap->facets.value("total", std::size_t{0});
            const auto favorites = snap->facets.value("favorites", std::size_t{0});
            const auto missing = snap->facets.value("missing", std::size_t{0});
            const Filter none;
            auto unfiltered = filter;
            unfiltered.text.clear();
            cyui::sidebarGroup(T("LIBRARY").c_str());
            if (cyui::sidebarItem(icon::library, L("All models").c_str(), page == page_models && unfiltered == none,
                                  std::to_string(total).c_str()))
                set_filter({});
            if (favorites) {
                Filter next;
                next.favorites = true;
                if (cyui::sidebarItem(icon::starFill, L("Favourites").c_str(), page == page_models && filter.favorites,
                                      std::to_string(favorites).c_str()))
                    set_filter(std::move(next));
            }
            if (missing) {
                Filter next;
                next.missing = true;
                if (cyui::sidebarItem(icon::warning, L("Missing files").c_str(), page == page_models && filter.missing,
                                      std::to_string(missing).c_str()))
                    set_filter(std::move(next));
            }
            sidebar_choices("KIND", "modality", modalities, &Filter::modality);
            sidebar_choices("ROLE", "type", types, &Filter::type);
            sidebar_choices("FOUND IN", "source", sources, &Filter::source);

            cyui::sidebarGroup(T("MANAGE").c_str());
            const auto groups = snap->duplicates.contains("groups") ? snap->duplicates["groups"].size() : 0;
            if (cyui::sidebarItem(icon::copy, L("Duplicates").c_str(), page == page_duplicates,
                                  groups ? std::to_string(groups).c_str() : nullptr))
                page = page_duplicates;
            if (cyui::sidebarItem(icon::folder, L("Locations").c_str(), page == page_locations,
                                  std::to_string(snap->roots.size()).c_str()))
                page = page_locations;
            std::size_t running = 0;
            for (const auto& job : snap->jobs) running += job.value("state", std::string()) == "running";
            if (cyui::sidebarItem(icon::clock, L("Activity").c_str(), page == page_jobs,
                                  running ? std::to_string(running).c_str() : nullptr))
                page = page_jobs;
        }
        cyui::endSidebar();
    }

    // ---- Models ----------------------------------------------------------------------------

    void sort_models() {
        const auto& models = snap->models;
        order.resize(models.size());
        for (std::size_t index = 0; index < models.size(); ++index) order[index] = static_cast<int>(index);
        const auto text = [&](const cymodelhub::Model& model) -> std::string {
            switch (sort_column) {
            case 1: return label_of(modalities, model.modality);
            case 2: return label_of(types, model.type);
            case 3: return model.architecture;
            case 4: return model.format;
            case 6: return label_of(sources, model.source);
            default: return modelhub::lower(model.name);
            }
        };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return modelhub::lower(models[a].name) < modelhub::lower(models[b].name);
        });
        if (sort_column == 5)
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return models[a].size < models[b].size; });
        else if (sort_column != 0)
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return text(models[a]) < text(models[b]); });
        if (sort_descending) std::reverse(order.begin(), order.end());
        order_generation = snap->generation;
    }

    std::string page_title() const {
        if (filter.favorites) return T("Favourites");
        if (filter.missing) return T("Missing files");
        if (!filter.modality.empty()) return label_of(modalities, filter.modality);
        if (!filter.type.empty()) return label_of(types, filter.type);
        if (!filter.source.empty()) return label_of(sources, filter.source);
        return T("All models");
    }

    void model_menu(const cymodelhub::Model& model) {
        if (ImGui::MenuItem(L("Open the folder").c_str())) reveal(model.path);
        if (ImGui::MenuItem(L("Copy the path").c_str())) {
            ImGui::SetClipboardText(modelhub::to_utf8(model.path).c_str());
            cyui::notify(T("Path copied").c_str(), cyui::Tone::success, 2.0f);
        }
        if (ImGui::MenuItem(model.favorite ? L("Remove from favourites").c_str() : L("Add to favourites").c_str()))
            invoke("", "models_update", {{"id", model.id}, {"favorite", !model.favorite}});
        ImGui::Separator();
        if (ImGui::MenuItem(L("Forget this entry").c_str())) {
            select(model.id);
            show_forget = true;
        }
    }

    void models_table() {
        const auto& models = snap->models;
        const auto flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##models", 7, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(L("Name").c_str(), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort |
                                                       ImGuiTableColumnFlags_NoHide, 3.2f);
        ImGui::TableSetupColumn(L("Kind").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(L("Role").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(L("Family").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(L("Format").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(L("Size").c_str(), ImGuiTableColumnFlags_WidthStretch |
                                                       ImGuiTableColumnFlags_PreferSortDescending, 0.8f);
        ImGui::TableSetupColumn(L("Found in").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        if (auto* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && specs->SpecsCount > 0) {
                sort_column = specs->Specs[0].ColumnIndex;
                sort_descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                order_generation = ~std::uint64_t{0};
                specs->SpecsDirty = false;
            }
        }
        if (order_generation != snap->generation || order.size() != models.size()) sort_models();

        const auto& palette = cyui::palette();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(order.size()));
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& model = models[static_cast<std::size_t>(order[static_cast<std::size_t>(row)])];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                cyui::ScopedID id(model.id.c_str());
                const bool current = model.id == selected;
                {
                    cyui::ScopedColor colour(ImGuiCol_Text, model.missing ? palette.textFaint : palette.text);
                    const auto name = (model.favorite && cyui::iconsAvailable() ? std::string(icon::starFill) + "  " : "") +
                                      model.name + "###row";
                    if (ImGui::Selectable(name.c_str(), current, ImGuiSelectableFlags_SpanAllColumns))
                        select(current ? std::string() : model.id);
                }
                if (ImGui::BeginPopupContextItem("##menu")) {
                    model_menu(model);
                    ImGui::EndPopup();
                }
                const auto cell = [&](const std::string& text) {
                    ImGui::TableNextColumn();
                    cyui::textDim("%s", text.c_str());
                };
                cell(label_of(modalities, model.modality));
                cell(label_of(types, model.type));
                cell(model.architecture);
                cell(model.format + (model.quantization.empty() ? "" : "  " + model.quantization));
                cell(model.missing ? T("missing") : human_size(model.size));
                cell(label_of(sources, model.source));
            }
        ImGui::EndTable();
    }

    void load_edit(const Json& record) {
        edit_id = record.value("id", std::string());
        copy_text(edit_name, sizeof(edit_name), record.value("name", std::string()));
        copy_text(edit_family, sizeof(edit_family), record.value("architecture", std::string()));
        copy_text(edit_notes, sizeof(edit_notes), record.value("notes", std::string()));
        std::vector<std::string> tags;
        if (record.contains("tags") && record["tags"].is_array()) tags = record["tags"].get<std::vector<std::string>>();
        copy_text(edit_tags, sizeof(edit_tags), joined(tags));
        edit_type = index_of(types, record.value("type", std::string()));
        edit_modality = index_of(modalities, record.value("modality", std::string()));
    }

    template <std::size_t N> bool choice_combo(const char* id, const Choice (&choices)[N], int* current) {
        std::vector<std::string> labels;
        std::vector<const char*> items;
        for (const auto& choice : choices) labels.push_back(T(choice.label));
        for (const auto& label : labels) items.push_back(label.c_str());
        return cyui::combo(id, current, items.data(), static_cast<int>(items.size()), -1.0f);
    }

    void save_edit() {
        Json arguments = {{"id", selected}};
        const auto changed = [&](const char* key, const std::string& value) {
            if (detail.value(key, std::string()) != value) arguments[key] = value;
        };
        changed("name", edit_name);
        changed("type", types[edit_type].value);
        changed("modality", modalities[edit_modality].value);
        changed("architecture", edit_family);
        changed("notes", edit_notes);
        auto tags = Json::array();
        std::string tag;
        for (const char* cursor = edit_tags;; ++cursor) {
            if (*cursor && *cursor != ',') {
                tag += *cursor;
                continue;
            }
            while (!tag.empty() && tag.front() == ' ') tag.erase(tag.begin());
            while (!tag.empty() && tag.back() == ' ') tag.pop_back();
            if (!tag.empty()) tags.push_back(tag);
            tag.clear();
            if (!*cursor) break;
        }
        arguments["tags"] = std::move(tags);
        data.run([arguments](cymodelhub::Client& client) { return client.invoke("models_update", arguments); },
                 [this](const Json&, const std::string& error) {
                     cyui::notify(error.empty() ? T("Saved").c_str() : error.c_str(),
                                  error.empty() ? cyui::Tone::success : cyui::Tone::error);
                     reload_detail();
                 });
    }

    void wrapped_mono(const std::string& text) {
        cyui::ScopedFont font(cyui::monoFont(), 0.9f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
    }

    void detail_panel() {
        const auto* model = selected_model();
        const bool loaded = detail.is_object() && detail.value("id", std::string()) == selected;
        if (!model && !loaded) {
            cyui::emptyState(T("Loading...").c_str(), "", nullptr);
            return;
        }
        const Json record = loaded ? detail : model->raw;
        if (loaded && edit_id != selected) load_edit(record);
        const auto path = modelhub::from_utf8(record.value("path", std::string()));
        const auto type = record.value("type", std::string()), modality = record.value("modality", std::string());

        cyui::heading(record.value("name", std::string()).c_str(), 2);
        cyui::badge(label_of(modalities, modality).c_str(), cyui::Tone::accent);
        ImGui::SameLine();
        cyui::badge(label_of(types, type).c_str());
        ImGui::SameLine();
        cyui::badge(record.value("format", std::string()).c_str());
        if (const auto family = record.value("architecture", std::string()); !family.empty()) {
            ImGui::SameLine();
            cyui::badge(family.c_str(), cyui::Tone::info);
        }
        if (const auto quantization = record.value("quantization", std::string()); !quantization.empty()) {
            ImGui::SameLine();
            cyui::badge(quantization.c_str());
        }
        cyui::space(6.0f);
        if (record.value("missing", false))
            cyui::callout(cyui::Tone::error, T("The file is no longer at this place. Scan again, or forget this entry.").c_str());
        if (record.value("unsafe", false))
            cyui::callout(cyui::Tone::warning,
                          T("Pickle file: loading it can run code. Prefer a safetensors version from a trusted source.").c_str());

        if (cyui::iconTextButton(icon::folder, L("Open the folder").c_str())) reveal(path);
        ImGui::SameLine();
        if (cyui::iconTextButton(icon::copy, L("Copy the path").c_str())) {
            ImGui::SetClipboardText(modelhub::to_utf8(path).c_str());
            cyui::notify(T("Path copied").c_str(), cyui::Tone::success, 2.0f);
        }
        ImGui::SameLine();
        const bool favorite = record.value("favorite", false);
        if (cyui::iconButton(favorite ? icon::starFill : icon::star,
                             (favorite ? T("Remove from favourites") : T("Add to favourites")).c_str(), favorite)) {
            invoke("", "models_update", {{"id", selected}, {"favorite", !favorite}});
            reload_detail();
        }
        cyui::space(4.0f);

        if (cyui::beginSection(L("File").c_str())) {
            if (cyui::beginProperties("##file", 120.0f)) {
                cyui::property((capture_relative_paths ? T("Relative path") : T("Path")).c_str());
                wrapped_mono(model_path_label(record));
                cyui::property(T("Size").c_str());
                auto size = human_size(record.value("size", std::uint64_t{0}));
                if (record.value("folder", false))
                    size += "  ·  " + std::to_string(record.value("files", 0)) + " " + T("files");
                ImGui::TextUnformatted(size.c_str());
                cyui::property(T("Modified").c_str());
                ImGui::TextUnformatted(date(record.value("modified", std::int64_t{0})).c_str());
                cyui::property(T("Found in").c_str());
                ImGui::TextUnformatted(label_of(sources, record.value("source", std::string())).c_str());
                if (const auto repository = record.value("repo", std::string()); !repository.empty()) {
                    cyui::property(T("Repository").c_str());
                    ImGui::TextUnformatted(repository.c_str());
                }
                cyui::property("SHA-256", T("Identifies the exact file, and proves that two copies are identical.").c_str());
                if (const auto hash = record.value("sha256", std::string()); !hash.empty()) {
                    wrapped_mono(hash);
                } else if (record.value("folder", false)) {
                    cyui::textFaint("-");
                } else if (cyui::button(L("Compute").c_str())) {
                    invoke("Hash started; see Activity", "models_hash", {{"id", selected}});
                }
                cyui::endProperties();
            }
            cyui::endSection();
        }

        if (cyui::beginSection(L("Classification").c_str())) {
            cyui::ScopedDisabled disabled(!loaded);
            if (cyui::beginProperties("##class", 120.0f)) {
                cyui::property(T("Name").c_str());
                cyui::textField("##name", edit_name, sizeof(edit_name), nullptr, -1.0f);
                cyui::property(T("Kind").c_str(), T("What the model works on.").c_str());
                choice_combo("##modality", modalities, &edit_modality);
                cyui::property(T("Role").c_str(), T("What the file is in a pipeline.").c_str());
                choice_combo("##type", types, &edit_type);
                cyui::property(T("Family").c_str(), T("Architecture or base model, e.g. sdxl, flux, llama.").c_str());
                cyui::textField("##family", edit_family, sizeof(edit_family), nullptr, -1.0f);
                cyui::property(T("Tags").c_str(), T("Separated by commas.").c_str());
                cyui::textField("##tags", edit_tags, sizeof(edit_tags), T("portrait, favourite, work").c_str(), -1.0f);
                cyui::property(T("Notes").c_str());
                ImGui::InputTextMultiline("##notes", edit_notes, sizeof(edit_notes),
                                          ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.5f));
                cyui::endProperties();
            }
            cyui::space(4.0f);
            if (cyui::primaryButton(L("Save").c_str())) save_edit();
            ImGui::SameLine();
            if (cyui::button(L("Detect again").c_str())) {
                data.run([id = selected](cymodelhub::Client& client) {
                             return client.invoke("models_update",
                                                  {{"id", id}, {"reset", {"name", "type", "modality", "architecture"}}});
                         },
                         [this](const Json&, const std::string& error) {
                             if (!error.empty()) cyui::notify(error.c_str(), cyui::Tone::error);
                             reload_detail();
                         });
            }
            cyui::tooltip(T("Forget the manual classification and read the file again").c_str());
            cyui::endSection();
        }

        if (cyui::beginSection(L("Share and move").c_str())) {
            ImGui::PushTextWrapPos(0.0f);
            cyui::textDim("%s", T("A link makes this model visible in another application's folder without "
                                  "copying it. Moving it frees space on this drive.").c_str());
            ImGui::PopTextWrapPos();
            cyui::space(4.0f);
            if (cyui::iconTextButton(icon::link, L("Link into a folder...").c_str())) {
                show_link = true;
                subfolder[0] = 0;
                link_mode = record.value("folder", false) ? 1 : 0;
            }
            ImGui::SameLine();
            const auto source = record.value("source", std::string());
            {
                cyui::ScopedDisabled external(source == "huggingface" || source == "ollama");
                if (cyui::iconTextButton(icon::exportFile, L("Move...").c_str())) {
                    show_move = true;
                    subfolder[0] = 0;
                }
            }
            if (source == "huggingface" || source == "ollama")
                cyui::tooltip(T("This model stays in the store of the tool that owns it").c_str());
            cyui::space(4.0f);
            if (cyui::button(L("Forget this entry").c_str(), cyui::Variant::outline)) show_forget = true;
            cyui::tooltip(T("Removes the entry from the catalogue. The file is not deleted.").c_str());
            cyui::endSection();
        }

        if (loaded && detail.contains("metadata") && !detail["metadata"].empty() &&
            cyui::beginSection(L("Header metadata").c_str(), false)) {
            if (cyui::beginProperties("##metadata", 170.0f)) {
                for (const auto& [key, value] : detail["metadata"].items()) {
                    cyui::property(key.c_str());
                    wrapped_mono(value.is_string() ? value.get<std::string>() : value.dump());
                }
                cyui::endProperties();
            }
            cyui::endSection();
        }
    }

    void models_page() {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float detail_width = selected.empty() ? 0.0f : std::min(cyui::dp(440.0f), ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::BeginChild("##list", ImVec2(selected.empty() ? 0.0f : -(detail_width + spacing), 0.0f));
        std::uint64_t size = 0;
        for (const auto& model : snap->models) size += model.missing ? 0 : model.size;
        cyui::panelHeader(page_title().c_str(),
                          (std::to_string(snap->models.size()) + " " + T("models") + "  ·  " + human_size(size)).c_str());
        cyui::space(4.0f);
        if (snap->models.empty()) {
            if (snap->facets.value("total", std::size_t{0}) == 0)
                cyui::emptyState(T("No models yet").c_str(),
                                 T("Add the folder where your models are, or install one. Models already in the "
                                   "Hugging Face, Ollama and LM Studio stores are listed automatically.").c_str(),
                                 icon::library);
            else
                cyui::emptyState(T("Nothing matches").c_str(), T("Change the search or pick another category.").c_str(),
                                 icon::search);
        } else {
            models_table();
        }
        ImGui::EndChild();
        if (!selected.empty()) {
            ImGui::SameLine();
            ImGui::BeginChild("##detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
            detail_panel();
            ImGui::EndChild();
        }
    }

    // ---- Other pages -----------------------------------------------------------------------

    void duplicates_page() {
        const auto wasted = snap->duplicates.value("wasted", std::uint64_t{0});
        cyui::panelHeader(T("Duplicates").c_str(),
                          (T("Identical models stored several times") + "  ·  " + human_size(wasted) + " " +
                           T("could be freed")).c_str());
        cyui::space(4.0f);
        const auto groups = snap->duplicates.find("groups");
        if (groups == snap->duplicates.end() || groups->empty()) {
            cyui::emptyState(T("No duplicates").c_str(), T("Every model is stored once.").c_str(), icon::check);
            return;
        }
        ImGui::BeginChild("##duplicates");
        int index = 0;
        for (const auto& group : *groups) {
            cyui::ScopedID id(index++);
            const auto& members = group.at("models");
            const auto title = members.front().value("name", std::string()) + "   ·   " + std::to_string(members.size()) +
                               " " + T("copies") + "   ·   " + human_size(group.value("size", std::uint64_t{0}));
            if (cyui::beginCard("##group", title.c_str())) {
                for (const auto& member : members) {
                    cyui::ScopedID row(member.value("id", std::string()).c_str());
                    if (cyui::iconButton(icon::folder, T("Open the folder").c_str()))
                        reveal(modelhub::from_utf8(member.value("path", std::string())));
                    ImGui::SameLine();
                    if (cyui::linkText((label_of(sources, member.value("source", std::string())) + "###open").c_str())) {
                        page = page_models;
                        set_filter({});
                        select(member.value("id", std::string()));
                    }
                    ImGui::SameLine();
                    wrapped_mono(model_path_label(member));
                }
            }
            cyui::endCard();
            cyui::space(6.0f);
        }
        ImGui::EndChild();
    }

    void locations_page() {
        cyui::panelHeader(T("Locations").c_str(), T("The folders where CyModelHub looks for models").c_str());
        cyui::space(4.0f);
        if (cyui::iconTextButton(icon::add, L("Watch a folder...").c_str(), cyui::Variant::primary))
            SDL_ShowOpenFolderDialog(browsed, reinterpret_cast<void*>(std::intptr_t{browse_location}), window, nullptr, false);
        cyui::tooltip(T("For example the models folder of ComfyUI, or a drive dedicated to models").c_str());
        cyui::space(6.0f);
        const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##roots", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(L("Location").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(L("Path").c_str(), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(L("Models").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn(L("Watched").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();
        for (const auto& root : snap->roots) {
            const auto id = root.value("id", std::string());
            cyui::ScopedID scope(id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(root.value("label", std::string()).c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            wrapped_mono(root.value("path", std::string()));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            cyui::textDim("%d", root.value("models", 0));
            ImGui::TableNextColumn();
            bool enabled = root.value("enabled", false);
            if (cyui::toggleSwitch("##enabled", &enabled))
                invoke("", "roots_enable", {{"id", id}, {"enabled", enabled}});
            ImGui::TableNextColumn();
            if (cyui::iconButton(icon::refresh, T("Scan this location").c_str()))
                invoke("Scan started", "scan_start", {{"root", id}});
            ImGui::SameLine();
            if (cyui::iconButton(icon::folder, T("Open the folder").c_str()))
                reveal(modelhub::from_utf8(root.value("path", std::string())));
            if (!root.value("builtin", false)) {
                ImGui::SameLine();
                if (cyui::iconButton(icon::trash, T("Stop watching this folder (no file is deleted)").c_str()))
                    invoke("Location removed", "roots_remove", {{"id", id}});
            }
        }
        ImGui::EndTable();
    }

    void jobs_page() {
        cyui::panelHeader(T("Activity").c_str(), T("Scans, downloads, moves and hashes").c_str());
        cyui::space(4.0f);
        if (snap->jobs.empty()) {
            cyui::emptyState(T("Nothing running").c_str(), T("Background work appears here.").c_str(), icon::clock);
            return;
        }
        const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##jobs", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(L("Task").c_str(), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(L("State").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn(L("Progress").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(L("Details").c_str(), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("##cancel", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableHeadersRow();
        for (auto job = snap->jobs.rbegin(); job != snap->jobs.rend(); ++job) {
            const auto id = job->value("id", std::string()), state = job->value("state", std::string());
            cyui::ScopedID scope(id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(job->value("title", std::string()).c_str());
            ImGui::TableNextColumn();
            const auto tone = state == "running" ? cyui::Tone::accent
                              : state == "done" ? cyui::Tone::success
                              : state == "failed" ? cyui::Tone::error
                              : cyui::Tone::warning;
            const auto label = state == "running" ? T("running")
                               : state == "done" ? T("done")
                               : state == "failed" ? T("failed")
                               : T("cancelled");
            cyui::badge(label.c_str(), tone);
            ImGui::TableNextColumn();
            const auto done = job->value("done", std::uint64_t{0}), total = job->value("total", std::uint64_t{0});
            if (state == "running")
                cyui::progressBar(total ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : -1.0f,
                                  ImVec2(-1.0f, cyui::dp(6.0f)));
            else cyui::textFaint("%s", date(job->value("finished", std::int64_t{0})).c_str());
            ImGui::TableNextColumn();
            cyui::textDim("%s", job->value("message", std::string()).c_str());
            ImGui::TableNextColumn();
            if (state == "running" && cyui::iconButton(icon::close, T("Cancel").c_str()))
                invoke("", "jobs_cancel", {{"id", id}, {"force", true}});
        }
        ImGui::EndTable();
    }

    // ---- Dialogs ---------------------------------------------------------------------------

    void destination_fields() {
        const auto roots = writable_roots();
        std::vector<std::string> labels;
        std::vector<const char*> items;
        for (const auto* root : roots)
            labels.push_back(root->value("label", std::string()) + "  —  " + root->value("path", std::string()));
        for (const auto& label : labels) items.push_back(label.c_str());
        destination = std::clamp(destination, 0, std::max(0, static_cast<int>(items.size()) - 1));
        if (cyui::beginProperties("##destination", 130.0f)) {
            cyui::property(T("Location").c_str(), T("Files are only written inside watched locations.").c_str());
            if (!items.empty()) cyui::combo("##root", &destination, items.data(), static_cast<int>(items.size()), -1.0f);
            cyui::property(T("Sub-folder").c_str(), T("Optional, e.g. loras/sdxl").c_str());
            cyui::textField("##subfolder", subfolder, sizeof(subfolder), T("optional").c_str(), -1.0f);
            cyui::endProperties();
        }
        cyui::space(2.0f);
        wrapped_mono(destination_path());
    }

    void add_dialog() {
        if (!cyui::beginModal(L("Add models").c_str(), &show_add, 560.0f)) return;
        ImGui::PushTextWrapPos(0.0f);
        cyui::textDim("%s", T("Watch a folder to list every model in it, now and later. Or register a single "
                              "model file or model folder that lies elsewhere. Nothing is copied or moved.").c_str());
        ImGui::PopTextWrapPos();
        cyui::space(8.0f);
        cyui::textField("##path", add_path, sizeof(add_path), T("Path of a folder or of a model file").c_str(), -1.0f);
        cyui::space(4.0f);
        if (cyui::iconTextButton(icon::folder, L("Choose a folder...").c_str()))
            SDL_ShowOpenFolderDialog(browsed, reinterpret_cast<void*>(std::intptr_t{browse_add_field}), window, nullptr, false);
        ImGui::SameLine();
        if (cyui::iconTextButton(icon::openFile, L("Choose a file...").c_str())) {
            static const SDL_DialogFileFilter filters[] = {
                {"Models", "gguf;safetensors;sft;ckpt;pt;pth;bin;onnx"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(browsed, reinterpret_cast<void*>(std::intptr_t{browse_add_field}), window, filters, 2,
                                   nullptr, false);
        }
        cyui::space(10.0f);
        const std::string path = add_path;
        cyui::ScopedDisabled disabled(path.empty());
        cyui::alignRight(cyui::buttonWidth(L("Watch this folder").c_str()) + cyui::buttonWidth(L("Add this model").c_str()) +
                         ImGui::GetStyle().ItemSpacing.x);
        if (cyui::button(L("Add this model").c_str())) {
            invoke("Model added", "models_add", {{"path", path}});
            show_add = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (cyui::primaryButton(L("Watch this folder").c_str())) {
            invoke("Location added; scanning", "roots_add", {{"path", path}});
            show_add = false;
            ImGui::CloseCurrentPopup();
        }
        cyui::endModal();
    }

    void install_dialog() {
        if (!cyui::beginModal(L("Install a model").c_str(), &show_install, 600.0f)) return;
        const auto hugging = T("Hugging Face"), address = T("Address");
        const char* const modes[] = {hugging.c_str(), address.c_str()};
        cyui::segmented("##mode", modes, 2, &install_mode);
        cyui::space(8.0f);
        if (cyui::beginProperties("##install", 130.0f)) {
            if (install_mode == 0) {
                cyui::property(T("Repository").c_str(), T("As shown on huggingface.co: owner/name").c_str());
                cyui::textField("##repository", install_repository, sizeof(install_repository), "owner/name", -1.0f);
                cyui::property(T("File").c_str(), T("The file to download from the repository.").c_str());
                cyui::textField("##file", install_file, sizeof(install_file), "model.Q4_K_M.gguf", -1.0f);
                cyui::property(T("Access token").c_str(), T("Only for gated models. Sent to huggingface.co only, never stored.").c_str());
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##token", install_token, sizeof(install_token), ImGuiInputTextFlags_Password);
            } else {
                cyui::property(T("Address").c_str(), T("Direct https:// link to the model file.").c_str());
                cyui::textField("##url", install_url, sizeof(install_url), "https://", -1.0f);
                cyui::property("SHA-256", T("Optional: the download is checked against it.").c_str());
                cyui::textField("##sha", install_sha, sizeof(install_sha), T("optional").c_str(), -1.0f);
            }
            cyui::endProperties();
        }
        cyui::space(8.0f);
        cyui::sectionTitle(T("Destination").c_str());
        destination_fields();
        const bool ready = install_mode == 0 ? install_repository[0] && install_file[0] : install_url[0] != 0;
        if (cyui::modalButtons("Download", "Cancel") == 1 && ready) {
            Json arguments = {{"destination", destination_path()}};
            if (install_mode == 0) {
                arguments["repository"] = install_repository;
                arguments["file"] = install_file;
                if (install_token[0]) arguments["token"] = install_token;
            } else {
                arguments["url"] = install_url;
                if (install_sha[0]) arguments["sha256"] = install_sha;
            }
            invoke("Download started; see Activity", "install_start", std::move(arguments));
            install_token[0] = 0;
        }
        cyui::endModal();
    }

    void move_dialog() {
        if (!cyui::beginModal(L("Move the model").c_str(), &show_move, 600.0f)) return;
        ImGui::PushTextWrapPos(0.0f);
        cyui::textDim("%s", T("The model is moved to the chosen location. Across drives it is copied, checked, "
                              "then removed from the old place.").c_str());
        ImGui::PopTextWrapPos();
        cyui::space(8.0f);
        destination_fields();
        cyui::space(6.0f);
        cyui::toggleSwitch(L("Leave a link at the old place").c_str(), &leave_link);
        cyui::tooltip(T("Other applications keep finding the model where it was").c_str());
        if (cyui::modalButtons("Move", "Cancel") == 1)
            invoke("Move started; see Activity", "models_move",
                   {{"id", selected}, {"destination", destination_path()}, {"leaveLink", leave_link}});
        cyui::endModal();
    }

    void link_dialog() {
        if (!cyui::beginModal(L("Link into a folder").c_str(), &show_link, 600.0f)) return;
        ImGui::PushTextWrapPos(0.0f);
        cyui::textDim("%s", T("The model appears in the chosen folder without being copied. A hard link needs the "
                              "same drive; a symbolic link works across drives.").c_str());
        ImGui::PopTextWrapPos();
        cyui::space(8.0f);
        destination_fields();
        cyui::space(6.0f);
        const auto hard = T("Hard link"), symbolic = T("Symbolic link");
        const char* const modes[] = {hard.c_str(), symbolic.c_str()};
        cyui::segmented("##linkmode", modes, 2, &link_mode);
        if (cyui::modalButtons("Create the link", "Cancel") == 1)
            invoke("Link created", "models_link",
                   {{"id", selected}, {"destination", destination_path()}, {"mode", link_mode == 0 ? "hard" : "symbolic"}});
        cyui::endModal();
    }

    void forget_dialog() {
        if (!cyui::beginModal(L("Forget this entry?").c_str(), &show_forget, 460.0f)) return;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(T("The entry, its tags and its notes leave the catalogue. The file itself is not "
                                 "deleted; a later scan lists it again if it is in a watched location.").c_str());
        ImGui::PopTextWrapPos();
        if (cyui::modalButtons("Forget", "Cancel", true) == 1) {
            invoke("Entry forgotten", "models_remove", {{"id", selected}});
            select({});
        }
        cyui::endModal();
    }

    void settings_dialog() {
        if (!cyui::beginModal(L("Settings").c_str(), &show_settings, 560.0f)) return;
        if (cyui::beginProperties("##settings", 150.0f)) {
            cyui::property(T("Language").c_str());
            const auto locales = cyui::availableLocales();
            std::vector<const char*> names;
            int current = 0;
            for (std::size_t index = 0; index < locales.size(); ++index) {
                names.push_back(locales[index].name.c_str());
                if (locales[index].code == cyui::locale()) current = static_cast<int>(index);
            }
            if (cyui::combo("##language", &current, names.data(), static_cast<int>(names.size()), -1.0f)) {
                settings.locale = locales[static_cast<std::size_t>(current)].code;
                cyui::setLocale(settings.locale.c_str());
                save_settings(settings);
            }
            cyui::property(T("Theme").c_str());
            const auto& themes = cyui::builtInThemes();
            std::vector<const char*> theme_names;
            int theme = 0;
            for (std::size_t index = 0; index < themes.size(); ++index) {
                theme_names.push_back(themes[index].name.c_str());
                if (themes[index].key == cyui::currentThemeKey()) theme = static_cast<int>(index);
            }
            if (cyui::combo("##theme", &theme, theme_names.data(), static_cast<int>(theme_names.size()), -1.0f)) {
                settings.theme = themes[static_cast<std::size_t>(theme)].key;
                cyui::setTheme(settings.theme.c_str());
                save_settings(settings);
            }
            cyui::property(T("Comfortable spacing").c_str());
            if (cyui::toggleSwitch("##density", &settings.comfortable)) {
                cyui::setDensity(settings.comfortable ? cyui::Density::comfortable : cyui::Density::compact);
                save_settings(settings);
            }
            cyui::endProperties();
        }
        cyui::space(10.0f);
        cyui::sectionTitle(T("AI assistants (MCP)").c_str());
        ImGui::PushTextWrapPos(0.0f);
        cyui::textDim("%s", T("An AI assistant can list, find, classify and install models through the Model "
                              "Context Protocol. Paste this configuration into your MCP client. It contains the "
                              "access token of this computer: keep it private.").c_str());
        ImGui::PopTextWrapPos();
        cyui::space(4.0f);
        if (cyui::iconTextButton(icon::copy, L("Copy the MCP configuration").c_str()))
            data.run([](cymodelhub::Client& client) { return client.mcp_configuration(); },
                     [](const Json& result, const std::string& error) {
                         if (!error.empty()) {
                             cyui::notify(error.c_str(), cyui::Tone::error);
                             return;
                         }
                         ImGui::SetClipboardText(result.dump(2).c_str());
                         cyui::notify(T("MCP configuration copied").c_str(), cyui::Tone::success);
                     });
        cyui::space(6.0f);
        cyui::textFaint("%s  %s", T("Data folder:").c_str(), snap->status.value("dataDirectory", std::string()).c_str());
        cyui::space(6.0f);
        if (cyui::button(L("User manual (F1)").c_str())) open_manual();
        if (cyui::modalButtons("Close", nullptr) != 0) show_settings = false;
        cyui::endModal();
    }

    void take_browsed() {
        std::string path;
        int target = browse_none;
        {
            std::lock_guard lock(browse.mutex);
            path.swap(browse.path);
            target = browse.target;
            browse.target = browse_none;
        }
        if (path.empty()) return;
        if (target == browse_add_field) copy_text(add_path, sizeof(add_path), path);
        else if (target == browse_location) invoke("Location added; scanning", "roots_add", {{"path", path}});
    }

    void draw() {
        snap = data.snapshot();
        data.pump();
        take_browsed();
        if (preset_select && selected.empty() && !snap->models.empty()) {
            preset_select = false;
            select(snap->models.front().id);
        }
        top_bar();
        status_bar();
        sidebar();

        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (!snap->connected) {
            cyui::emptyState(T("Connecting to the CyModelHub service").c_str(),
                             snap->error.empty() ? T("The service starts on its own; this takes a moment.").c_str()
                                                 : snap->error.c_str(),
                             icon::sync);
        } else if (page == page_duplicates) duplicates_page();
        else if (page == page_locations) locations_page();
        else if (page == page_jobs) jobs_page();
        else models_page();
        ImGui::End();

        add_dialog();
        install_dialog();
        move_dialog();
        link_dialog();
        forget_dialog();
        settings_dialog();
    }
};

Ui::Ui(Data& data, Settings& settings, SDL_Window* window, SDL_Texture* logo)
    : impl_(std::make_unique<Impl>(data, settings, window, logo)) {}
Ui::~Ui() = default;
void Ui::draw() { impl_->draw(); }

bool Ui::animating() const {
    if (!impl_->snap) return true;
    if (!impl_->snap->connected) return true;
    return impl_->running_job() != nullptr;
}

void Ui::preset(int page, bool select_first, const std::string& dialog, bool relative_paths) {
    impl_->page = page;
    impl_->preset_select = select_first;
    impl_->show_add = dialog == "add";
    impl_->show_install = dialog == "install";
    impl_->show_settings = dialog == "settings";
    impl_->capture_relative_paths = relative_paths;
}
} // namespace app
