// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "app_data.hpp"

struct SDL_Texture;
struct SDL_Window;

namespace app {
// Load every locales/*.json next to the executable. A file is
// { "locale": "fr", "name": "Français", "translations": { "English text": "Texte" } }.
void load_locales();
// Language of the operating system when a catalogue exists for it, otherwise English.
std::string system_locale();

class Ui {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Ui(Data& data, Settings& settings, SDL_Window* window, SDL_Texture* logo);
    ~Ui();
    void draw();
    // True while something moves on screen (a job runs), so the host keeps redrawing.
    bool animating() const;
    // Used by --capture to render a given screen without interaction.
    void preset(int page, bool select_first, const std::string& dialog, bool relative_paths);
};
} // namespace app
