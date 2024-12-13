#pragma once
#include "ui/preferences/preferences.hpp"

namespace ohao {

class PreferencesWindow {
public:
    PreferencesWindow() = default;
    ~PreferencesWindow() = default;

    void render(bool* open);
    bool isOpen() const { return isWindowOpen; }
    void open() { isWindowOpen = true; tempPrefsInitialized = false; }

private:
    void renderAppearanceTab();
    void applySettings();

    bool isWindowOpen{false};
    AppearancePreferences tempPrefs;  // For storing temporary changes
    bool tempPrefsInitialized{false};
};

} // namespace ohao
