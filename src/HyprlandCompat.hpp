#pragma once

#include <hyprutils/utils/ScopeGuard.hpp>
#include <utility>

// Header layout and renderer capabilities are detected independently. A moved
// window header alone says nothing about the renderer API in a future release.
// Probe through pkg-config's dedicated Hyprland include root (src/...). Using
// hyprland/src/... here could find newer /usr headers while hyprpm selected an
// older header tree, mixing two incompatible layouts in the same translation.
#if __has_include(<src/desktop/view/window/Window.hpp>)
#include <hyprland/src/desktop/view/window/Window.hpp>
#else
#include <hyprland/src/desktop/view/Window.hpp>
#endif

#if __has_include(<src/workspace/HLWorkspace.hpp>)
#include <hyprland/src/workspace/HLWorkspace.hpp>
#else
#include <hyprland/src/desktop/Workspace.hpp>
#endif

#if __has_include(<src/desktop/view/window/WindowPresentation.hpp>)
#define HYPRLIQUID_WINDOW_PRESENTATION 1
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/render/WindowRenderPresentation.hpp>
#else
#define HYPRLIQUID_WINDOW_PRESENTATION 0
#endif

#if __has_include(<src/render/Context.hpp>)
#define HYPRLIQUID_RENDER_CONTEXT 1
#include <hyprland/src/render/Context.hpp>
#else
#define HYPRLIQUID_RENDER_CONTEXT 0
#endif

namespace HyprlandCompat {

#if HYPRLIQUID_WINDOW_PRESENTATION
// Presentation is owned by queued draw data. A raw pointer is borrowed only
// during a synchronous draw; trivial TLS avoids pinning the plugin at dlclose.
inline thread_local const Render::SWindowRenderPresentation* boundPresentation = nullptr;
inline thread_local const Desktop::View::CWindow* presentedWindow = nullptr;

class CPresentationScope {
  public:
    CPresentationScope(const PHLWINDOW& window, const Render::SWindowRenderPresentation& presentation)
        : m_previous(boundPresentation), m_previousWindow(presentedWindow) {
        boundPresentation = &presentation;
        presentedWindow = window.get();
    }
    ~CPresentationScope() { boundPresentation = m_previous; presentedWindow = m_previousWindow; }
    CPresentationScope(const CPresentationScope&) = delete;
    CPresentationScope& operator=(const CPresentationScope&) = delete;

  private:
    const Render::SWindowRenderPresentation* m_previous;
    const Desktop::View::CWindow* m_previousWindow;
};

inline Render::SWindowRenderPresentation windowPresentation(const PHLWINDOW& window) {
    if (boundPresentation && presentedWindow == window.get()) return *boundPresentation;
    return window->presentation().renderPresentation();
}
#endif

template <typename View>
[[nodiscard]] bool mapped(const View& view) {
    if constexpr (requires { view->mapped(); })
        return view->mapped();
    else if constexpr (requires { view->m_isMapped; })
        return view->m_isMapped;
    else
        return view->m_mapped;
}

template <typename Window>
[[nodiscard]] bool floating(const Window& window) {
    if constexpr (requires { window->isFloating(); })
        return window->isFloating();
    else
        return window->m_isFloating;
}

template <typename Window>
[[nodiscard]] bool pinned(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return bool(window->m_state & Desktop::View::WINDOW_STATE_PINNED);
#else
    return window->m_pinned;
#endif
}

template <typename Window>
[[nodiscard]] auto floatingOffset(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->presentation().floatingOffset();
#else
    return window->m_floatingOffset;
#endif
}

template <typename Window>
[[nodiscard]] bool movingFromMonitor(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->presentation().movingFromMonitor();
#else
    return window->m_monitorMovedFrom != -1;
#endif
}

template <typename Window>
[[nodiscard]] auto rounding(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->presentation().rounding();
#else
    return window->rounding();
#endif
}

template <typename Window>
[[nodiscard]] auto roundingPower(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->presentation().roundingPower();
#else
    return window->roundingPower();
#endif
}

template <typename Window>
[[nodiscard]] auto alpha(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->alpha().getTotal();
#else
    return window->effectiveAlpha();
#endif
}

template <typename Window, typename Channel>
[[nodiscard]] auto alphaWithout(const Window& window, Channel channel) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->alpha().getTotalWithout(channel);
#else
    return window->alphaTotalWithout(channel);
#endif
}

template <typename Window>
[[nodiscard]] auto borderSize(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->presentation().borderSize();
#else
    return window->getRealBorderSize();
#endif
}

template <typename Window>
[[nodiscard]] auto reportedSize(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->backend().reportedSize();
#else
    return window->getReportedSize();
#endif
}

template <typename Window>
[[nodiscard]] auto surfaceScale(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->backend().surfaceScale();
#else
    return window->m_X11SurfaceScaledBy;
#endif
}

template <typename Window>
[[nodiscard]] bool hasTransformers(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->effects().hasActiveTransformers();
#else
    return !window->m_transformers.empty();
#endif
}

template <typename Window>
[[nodiscard]] bool grouped(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return bool(window->grouping().group());
#else
    return bool(window->m_group);
#endif
}

template <typename Window>
[[nodiscard]] auto popupCount(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->popupTreeSize();
#else
    return window->popupsCount();
#endif
}

template <typename Window>
decltype(auto) decorations(const Window& window) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->presentation().decorations();
#else
    return (window->m_windowDecorations);
#endif
}

template <typename Surface>
auto surfaceDamage(const Surface& surface) {
    if constexpr (requires { surface->computeDamage(surface->getSurfaceBoxGlobal()); }) {
        // 0.57 consumes accumulated damage for asynchronous buffers while
        // computing it. Observe the commit without starving the native renderer
        // of its surface or buffer damage when its hook resumes.
        const auto resource = surface->resource();
        if (!resource) return CRegion{};
        auto savedDamage = resource->m_current.damage.copy();
        auto savedBufferDamage = resource->m_current.bufferDamage.copy();
        Hyprutils::Utils::CScopeGuard restore([&] {
            resource->m_current.damage = std::move(savedDamage);
            resource->m_current.bufferDamage = std::move(savedBufferDamage);
        });
        return surface->computeDamage(surface->getSurfaceBoxGlobal());
    } else
        return surface->computeDamage();
}

template <typename Workspace>
[[nodiscard]] bool visible(const Workspace& workspace) {
    if constexpr (requires { workspace->visible(); })
        return workspace->visible();
    else
        return workspace->isVisible();
}

[[nodiscard]] inline bool special(const PHLWORKSPACE& workspace) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return workspace->type() == Workspace::eWorkspaceType::SPECIAL;
#else
    return workspace->m_isSpecialWorkspace;
#endif
}

} // namespace HyprlandCompat
