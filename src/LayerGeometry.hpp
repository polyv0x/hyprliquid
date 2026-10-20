#pragma once

#include "RenderSafety.hpp"

#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprutils/math/Box.hpp>
#include <cmath>
#include <optional>

namespace LayerGeometry {

[[nodiscard]] inline std::optional<CBox> computeLayerBox(PHLLS layerSurface, PHLMONITOR monitor) {
    if (!layerSurface || !monitor || !std::isfinite(monitor->m_scale) || monitor->m_scale <= 0)
        return std::nullopt;

    // Full animated layer geometry in monitor-local framebuffer pixels.
    auto box = CBox{layerSurface->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
                    layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
    box.translate(-monitor->m_position);
    box.scale(monitor->m_scale).round().noNegativeSize();

    if (!RenderSafety::sampleGeometry(box.x, box.y, box.w, box.h, 0))
        return std::nullopt;

    return box;
}

} // namespace LayerGeometry
