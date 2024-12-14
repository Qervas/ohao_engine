#pragma once
#include <string>
#include <memory>
#include "imgui.h"

namespace ohao {

class PanelBase {
public:
    PanelBase(const std::string& name) : name(name) {
        windowFlags = ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse;
    }
    virtual ~PanelBase() = default;

    virtual void render() = 0;

    bool isVisible() const { return visible; }
    void setVisible(bool value) { visible = value; }
    const std::string& getName() const { return name; }

protected:
    bool visible{true};
    std::string name;
    ImGuiWindowFlags windowFlags{0};
};

}
