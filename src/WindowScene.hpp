#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <memory>
#include <vector>

// One frame's tiled-window scene. The capture pass owns its private render
// pass; glass elements only retain this data, avoiding an ownership cycle.
struct SWindowScene {
    SP<Render::IFramebuffer> background;
    SP<Render::IFramebuffer> image;
    std::vector<PHLWINDOWREF> windows;
    bool ready = false;
};

namespace WindowScene {
bool initialize();
void destroy();
std::shared_ptr<SWindowScene> queuedFor(PHLWINDOW window);
bool buildingBase();
}
