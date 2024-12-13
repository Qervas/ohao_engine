#pragma once
#include <string>

namespace ohao {

struct AppearancePreferences {
    float uiScale{1.0f};
    std::string theme{"Dark"};
    bool enableDocking{true};
    bool enableViewports{true};
};

class Preferences {
public:
    static Preferences& get() {
        static Preferences instance;
        return instance;
    }

    void load();
    void save();

    AppearancePreferences& getAppearance() { return appearance; }
    const AppearancePreferences& getAppearance() const { return appearance; }
    void setAppearance(const AppearancePreferences& prefs) {
        appearance = prefs;
        save();
    }

private:
    Preferences() { load(); }
    ~Preferences() { save(); }

    std::string getPreferencesFilePath() const;
    void createDefaultPreferences();

    AppearancePreferences appearance;
    const std::string PREFERENCES_FILENAME = "preferences.json";
};

} // namespace ohao
