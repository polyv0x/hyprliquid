#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprutils/math/Region.hpp>
#include <memory>
#include <optional>
#include <vector>

// One frame's neighbouring-window scene. The capture pass owns its private render
// pass; glass elements only retain this data, avoiding an ownership cycle.
struct SWindowScene {
    SP<Render::IFramebuffer> background;
    SP<Render::IFramebuffer> image;
    std::vector<PHLWINDOWREF> windows;
    bool ready = false;
    // Local panes can be painted below clients once and reused as an exact
    // native-format backdrop. Geometry is still available for shadow occlusion.
    bool local = false;
    bool failed = false;
    bool cacheBackground = false;
    bool cacheBase = false;
    bool cacheInputs = false;
    // Stable overlapping stacks retain inputs and repair native material
    // results in painting order. These do not use the disjoint-pane shortcut.
    bool regional = false;
    // Auxiliary repair can still run when the per-pane working set cannot fit.
    bool regionalInputs = false;
    struct SReceiverDamage { PHLWINDOWREF window; CRegion input, output; };
    std::vector<SReceiverDamage> receivers;
    CRegion auxiliaryDamage;
    std::optional<CRegion> repair;
    // Disjoint tiles share their immutable auxiliary material even when live
    // floating panes must still be interleaved with client surfaces above them.
    bool cacheTiledBase = false;
    // The bottom tiled pane can have an unchanged backdrop even while floats
    // wholly inside its owner rectangle repaint above it.
    PHLWINDOWREF independentWindow;
    uint64_t inputSerial = 0, previousInputSerial = 0;
    std::vector<uint64_t> backgroundKey;
    std::vector<uint64_t> inputKey;
    struct SDamage { PHLWINDOWREF window; CRegion region; };
    std::vector<SDamage> damage;
    struct SCache { bool valid = false; };
    std::shared_ptr<SCache> cache;
};

namespace WindowScene {
bool initialize();
void destroy();
void collectUnused();
bool hasRetired();
void retireMonitor(MONITORID monitor);
void finishFrame(MONITORID monitor, uint64_t frame);
std::shared_ptr<SWindowScene> queuedFor(PHLWINDOW window);
bool buildingBase();
bool skipDecoration(PHLWINDOW window);
void invalidateBackground(PHLMONITOR monitor);
void damageWindow(PHLWINDOW window, const CRegion& globalDamage);
void invalidateAll();
}
