#pragma once

#include "RenderSafety.hpp"

#include "HyprlandCompat.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprutils/math/Box.hpp>
#include <optional>

namespace WindowGeometry {

[[nodiscard]] inline std::optional<CBox> computeWindowBox(PHLWINDOW window, PHLMONITOR monitor,
        Desktop::View::IGeometric::eGeometricValueType geometry = Desktop::View::IGeometric::GEOMETRIC_CURRENT) {
    if (!window || !monitor || !std::isfinite(monitor->m_scale) || monitor->m_scale <= 0)
        return std::nullopt;

#if HYPRLIQUID_WINDOW_PRESENTATION
    const auto presentation = HyprlandCompat::windowPresentation(window);
    auto box = window->geometricBox(geometry);
    box.translate(presentation.workspaceOffset + presentation.floatingOffset - monitor->m_position);
#else
    const auto workspace = window->m_workspace;
    const auto workspaceOffset = workspace && !HyprlandCompat::pinned(window)
        ? workspace->m_renderOffset->value()
        : Vector2D();

    auto box = window->geometricBox(geometry);
    box.translate(workspaceOffset);
    box.translate(-monitor->m_position + HyprlandCompat::floatingOffset(window));
#endif
    box.scale(monitor->m_scale);
    box.round();
    if (!RenderSafety::sampleGeometry(box.x, box.y, box.w, box.h, 0))
        return std::nullopt;
    return box;
}

} // namespace WindowGeometry
