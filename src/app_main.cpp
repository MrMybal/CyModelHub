// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

// CyModelHub desktop application: one SDL3 host for Windows, Linux and macOS.

#include "app_ui.hpp"
#include "platform.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cyui/Core.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <iostream>

namespace {
struct Arguments {
    std::string locale, theme, capture, dialog, data;
    int page = 0;
    bool select = false, help = false;
};

Arguments parse(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string word = argv[index];
        const auto value = [&]() -> std::string { return index + 1 < argc ? argv[++index] : ""; };
        if (word == "--locale") arguments.locale = value();
        else if (word == "--theme") arguments.theme = value();
        else if (word == "--data") arguments.data = value();
        else if (word == "--capture") arguments.capture = value();
        else if (word == "--dialog") arguments.dialog = value();
        else if (word == "--page") arguments.page = std::atoi(value().c_str());
        else if (word == "--select") arguments.select = true;
        else arguments.help = true;
    }
    return arguments;
}

struct Sdl {
    Sdl() {
        SDL_SetAppMetadata("CyModelHub", CYMODELHUB_VERSION, "com.cyberalien.cymodelhub");
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    }
    ~Sdl() { SDL_Quit(); }
};

float window_dpi(SDL_Window* window) {
    const float scale = SDL_GetWindowDisplayScale(window);
    const float pixels = SDL_GetWindowPixelDensity(window);
    return scale > 0 && pixels > 0 ? scale / pixels : 1.0f;
}

void run(const Arguments& arguments) {
    if (!arguments.data.empty()) modelhub::set_environment("CYMODELHUB_DATA", arguments.data);
    Sdl sdl;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("CyModelHub", 1360, 840, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    if (!window) throw std::runtime_error(SDL_GetError());
    SDL_SetWindowMinimumSize(window.get(), 960, 600);
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(SDL_CreateRenderer(window.get(), nullptr),
                                                                           SDL_DestroyRenderer);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    SDL_SetRenderVSync(renderer.get(), 1);

    auto settings = app::load_settings();
    if (!arguments.theme.empty()) settings.theme = arguments.theme;
    const auto& themes = cyui::builtInThemes();
    if (std::none_of(themes.begin(), themes.end(), [&](const cyui::Theme& theme) { return theme.key == settings.theme; }))
        settings.theme = cyui::defaultThemeKey;

    auto* context = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    cyui::Config config;
    config.themeKey = settings.theme.c_str();
    config.density = settings.comfortable ? cyui::Density::comfortable : cyui::Density::compact;
    float dpi = window_dpi(window.get());
    config.dpiScale = dpi;
    cyui::apply(config);
    app::load_locales();
    const auto locale = !arguments.locale.empty() ? arguments.locale
                        : !settings.locale.empty() ? settings.locale
                        : app::system_locale();
    if (!cyui::setLocale(locale.c_str())) cyui::setLocale("en");
    cyui::loadFonts();
    ImGui_ImplSDL3_InitForSDLRenderer(window.get(), renderer.get());
    ImGui_ImplSDLRenderer3_Init(renderer.get());
    SDL_ShowWindow(window.get());

    {
        app::Data data;
        app::Ui ui(data, settings, window.get());
        ui.preset(arguments.page, arguments.select, arguments.dialog);
        int frame = 0, settled = 0;
        for (bool running = true; running;) {
            SDL_Event event{};
            // Redraw at once on input, smoothly while something moves, and rarely when idle.
            const int wait = !arguments.capture.empty() || ui.animating() || frame < 10 ? 16 : 120;
            if (SDL_WaitEventTimeout(&event, wait)) do {
                    if (event.type == SDL_EVENT_QUIT ||
                        (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                         event.window.windowID == SDL_GetWindowID(window.get())))
                        running = false;
                    ImGui_ImplSDL3_ProcessEvent(&event);
                } while (SDL_PollEvent(&event));
            if (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) continue;
            if (const float current = window_dpi(window.get()); current != dpi) {
                dpi = current;
                cyui::applyDpi(dpi);
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ui.draw();
            cyui::renderNotifications();
            ImGui::Render();
            auto* draw = ImGui::GetDrawData();
            SDL_SetRenderScale(renderer.get(), draw->FramebufferScale.x, draw->FramebufferScale.y);
            const auto background = ImGui::ColorConvertU32ToFloat4(cyui::palette().bg0);
            SDL_SetRenderDrawColorFloat(renderer.get(), background.x, background.y, background.z, background.w);
            SDL_RenderClear(renderer.get());
            ImGui_ImplSDLRenderer3_RenderDrawData(draw, renderer.get());
            ++frame;
            if (!arguments.capture.empty()) {
                // Wait for the catalogue to be loaded and quiet, then save one frame and leave.
                settled = ui.animating() ? 0 : settled + 1;
                if (settled > 45 || frame > 1500) {
                    if (auto* pixels = SDL_RenderReadPixels(renderer.get(), nullptr)) {
                        SDL_SavePNG(pixels, arguments.capture.c_str());
                        SDL_DestroySurface(pixels);
                    }
                    running = false;
                }
            }
            SDL_RenderPresent(renderer.get());
        }
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto arguments = parse(argc, argv);
        if (arguments.help) {
            std::cout << "CyModelHub [--locale CODE] [--theme KEY] [--data PATH]\n"
                         "Desktop manager for the AI models shared between applications.\n";
            return 0;
        }
        run(arguments);
        return 0;
    } catch (const std::exception& error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CyModelHub", error.what(), nullptr);
        return 1;
    }
}
