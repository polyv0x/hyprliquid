#include "WindowScene.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <unordered_map>

namespace WindowScene {
namespace {
using RenderWindow = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&,
                              bool, Render::eRenderPassMode, bool, bool);
using RenderWorkspace = void (*)(Render::IHyprRenderer*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
using Hyprutils::Utils::CScopeGuard;

CFunctionHook* workspaceHook = nullptr;
RenderWindow renderWindow = nullptr;
std::shared_ptr<SWindowScene> queuedScene;
bool queuingBase = false;

struct SBuffers {
    PHLMONITORREF monitor;
    SP<Render::IFramebuffer> background;
    SP<Render::IFramebuffer> image;
    std::weak_ptr<SWindowScene> scene;
    std::optional<CRenderBuffers::Clock::time_point> retired;
    uint64_t frame = 0;
};
std::unordered_map<MONITORID, SBuffers> buffers;

GLuint fbId(const SP<Render::IFramebuffer>& fb) {
    return dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get())->getFBID();
}

CGlassDecoration* decorationFor(PHLWINDOW window) {
    for (auto& decoration : g_pGlobalState->decorations)
        if (decoration && decoration->getOwner() == window)
            return decoration.get();
    return nullptr;
}

class CWindowScenePassElement : public IPassElement {
  public:
    explicit CWindowScenePassElement(std::shared_ptr<SWindowScene> scene) : m_scene(std::move(scene)) {}
    Render::CRenderPass pass;

    std::vector<UP<IPassElement>> draw() override {
        auto& renderer = *g_pHyprRenderer;
        const auto savedData = renderer.m_renderData;
        const auto monitor = savedData.pMonitor.lock();
        const auto source = savedData.currentFB;
        if (!monitor || !source)
            return {};

        const bool feedback = renderer.m_bBlockSurfaceFeedback;
        const bool snapshot = renderer.m_bRenderingSnapshot;
        const bool blur = monitor->m_blurFBShouldRender;
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        GLint scissorBox[4];
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
        CScopeGuard restore([&] {
            renderer.m_renderData = savedData;
            renderer.m_bBlockSurfaceFeedback = feedback;
            renderer.m_bRenderingSnapshot = snapshot;
            monitor->m_blurFBShouldRender = blur;
            renderer.bindFB(source);
            renderer.setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, scissor);
        });

        for (auto* framebuffer : {&m_scene->background, &m_scene->image}) {
            auto& fb = *framebuffer;
            if (!g_pGlobalState->renderBuffers.prepare(fb, source->m_size.x, source->m_size.y, source->m_drmFormat,
                    fb == m_scene->background ? "scene-background" : "scene-windows", std::format("monitor:{}", monitor->m_id)))
                return {};
            fb->setImageDescription(source->imageDescription());
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(source));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(fb));
            glBlitFramebuffer(0, 0, source->m_size.x, source->m_size.y,
                              0, 0, source->m_size.x, source->m_size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        renderer.bindFB(m_scene->image);
        renderer.m_bBlockSurfaceFeedback = true;
        renderer.m_bRenderingSnapshot = true;
        renderer.m_renderData.noSimplify = true;
        pass.render(CRegion(CBox{{}, monitor->m_transformedSize}));
        m_scene->ready = true;
        return {};
    }

    bool needsLiveBlur() override { return true; }
    bool needsPrecomputeBlur() override { return false; }
    bool disableSimplification() override { return true; }
    std::optional<CBox> boundingBox() override {
        return CBox{{}, g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize / g_pHyprRenderer->m_renderData.pMonitor->m_scale};
    }
    const char* passName() override { return "CWindowScenePassElement"; }
    ePassElementType type() override { return EK_CUSTOM; }

  private:
    std::shared_ptr<SWindowScene> m_scene;
};

std::shared_ptr<SWindowScene> collectScene(Render::IHyprRenderer* renderer, PHLMONITOR monitor, PHLWORKSPACE workspace) {
    if (!workspace || renderer->m_bRenderingSnapshot || renderer->m_renderData.projectionType != Render::RPT_MONITOR)
        return nullptr;

    struct SCandidate {
        PHLWINDOW window;
        CBox goal, current;
    };
    std::vector<SCandidate> candidates;
    bool floatingGeometryKnown = true;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->m_isMapped || window->isHidden() || !renderer->shouldRenderWindow(window, monitor))
            continue;
        const bool ignoreSpecial = window->m_monitorMovedFrom != -1 && window->m_workspace && !window->m_workspace->isVisible();
        if (!ignoreSpecial && workspace->m_isSpecialWorkspace != window->onSpecialWorkspace())
            continue;

        const auto goal = WindowGeometry::computeWindowBox(window, monitor, Desktop::View::IGeometric::GEOMETRIC_GOAL);
        const auto current = WindowGeometry::computeWindowBox(window, monitor);
        if (!window->m_transformers.empty() || !goal || !current) {
            if (window->m_isFloating) {
                // An arbitrary transform may extend beyond its geometry box.
                // Keep the existing tiled capture, but do not admit floats
                // whose lack of overlap we cannot establish.
                floatingGeometryKnown = false;
                continue;
            }
            return nullptr;
        }
        candidates.push_back({window, *goal, *current});
    }

    auto scene = std::make_shared<SWindowScene>();
    std::vector<CBox> boxes;
    bool hasGlass = false;
    for (const auto& candidate : candidates) {
        const auto& window = candidate.window;
        if (window->m_isFloating) {
            // A separate floating pane has no stacking dependency: capture it
            // regardless of which neighbour has focus. Overlapping floats
            // still use native occlusion and do not disable the tiled scene.
            if (!floatingGeometryKnown || std::ranges::any_of(candidates, [&](const auto& other) {
                    return other.window != window &&
                        (!candidate.goal.intersection(other.goal).empty() ||
                         !candidate.current.intersection(other.current).empty());
                }))
                continue;
        }
        // Tiles use the destination layout so transient overlap while opening
        // a split cannot remove their reflections. Capture uses current boxes.
        for (const auto& other : boxes)
            if (!candidate.goal.intersection(other).empty())
                return nullptr;
        boxes.push_back(candidate.goal);
        scene->windows.emplace_back(window);
        if (auto* decoration = decorationFor(window); decoration && decoration->usesGlass())
            hasGlass = true;
    }
    return hasGlass && scene->windows.size() > 1 ? scene : nullptr;
}

void renderWorkspace(Render::IHyprRenderer* renderer, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    auto original = reinterpret_cast<RenderWorkspace>(workspaceHook->m_original);
    auto scene = collectScene(renderer, monitor, workspace);
    // Ineligible scenes used to return before expired-monitor cleanup. Retirement
    // drops only the cache's references: queued passes keep their images alive.
    for (auto& [_, storage] : buffers)
        if (!storage.monitor && !storage.retired) storage.retired = CRenderBuffers::Clock::now();
    if (!scene) {
        if (hasRetired()) g_pGlobalState->renderBuffers.scheduleCleanup();
        original(renderer, monitor, workspace, now);
        return;
    }

    auto& storage = buffers[monitor->m_id];
    storage.monitor = monitor;
    storage.retired.reset();
    storage.frame = g_pGlobalState->renderBuffers.frame();
    // Two workspaces can be queued in one frame during transitions. Never
    // overwrite an earlier queued scene's immutable background or neighbour map.
    if (!storage.scene.expired()) {
        storage.background.reset();
        storage.image.reset();
    }
    if (!storage.background)
        storage.background = renderer->createFB("hyprliquid-scene-background");
    if (!storage.image)
        storage.image = renderer->createFB("hyprliquid-scene-windows");
    scene->background = storage.background;
    scene->image = storage.image;
    storage.scene = scene;

    auto capture = makeUnique<CWindowScenePassElement>(scene);
    const auto previousScene = queuedScene;
    queuedScene = scene;
    CScopeGuard restoreScene([&] { queuedScene = previousScene; });
    {
        const auto savedData = renderer->m_renderData;
        const bool snapshot = renderer->m_bRenderingSnapshot;
        auto redirect = renderer->redirectPass(&capture->pass);
        queuingBase = true;
        renderer->m_bRenderingSnapshot = true;
        CScopeGuard restore([&] {
            renderer->m_renderData = savedData;
            renderer->m_bRenderingSnapshot = snapshot;
            queuingBase = false;
        });
        for (const auto& window : scene->windows) {
            // Only our glass plus the client's main surfaces. Popups, shadows
            // and other plugins' decorations belong to the normal scene pass.
            if (auto* decoration = decorationFor(window.lock()))
                decoration->draw(monitor, 1.0f);
            renderWindow(renderer, window.lock(), monitor, now, false, Render::RENDER_PASS_MAIN, false, false);
        }
    }
    renderer->addPassElement(std::move(capture));

    // The auxiliary scene reads the wallpaper across the monitor. Repaint it
    // completely on this already scheduled frame, without scheduling another
    // frame or retaining last frame's windows in the backdrop.
    renderer->m_renderData.damage.add(CBox{{}, monitor->m_transformedSize});
    renderer->m_renderData.finalDamage.add(CBox{{}, monitor->m_transformedSize});
    original(renderer, monitor, workspace, now);
}
} // namespace

bool initialize() {
    for (const auto& match : HyprlandAPI::findFunctionsByName(PHANDLE, "renderWindow"))
        if (match.demangled.contains("IHyprRenderer::renderWindow(")) {
            renderWindow = reinterpret_cast<RenderWindow>(match.address);
            break;
        }
    if (!renderWindow)
        return false;
    for (const auto& match : HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspaceWindows"))
        if (match.demangled.contains("IHyprRenderer::renderWorkspaceWindows(")) {
            workspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, reinterpret_cast<void*>(renderWorkspace));
            if (workspaceHook && workspaceHook->hook())
                return true;
            break;
        }
    destroy();
    return false;
}

void destroy() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("CWindowScenePassElement");
    if (workspaceHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, workspaceHook);
    workspaceHook = nullptr;
    renderWindow = nullptr;
    queuedScene.reset();
    buffers.clear();
    queuingBase = false;
}

std::shared_ptr<SWindowScene> queuedFor(PHLWINDOW window) {
    if (queuedScene && std::ranges::any_of(queuedScene->windows, [&](const auto& w) { return w == window; }))
        return queuedScene;
    return nullptr;
}

bool buildingBase() { return queuingBase; }

void collectUnused() {
    const auto now = CRenderBuffers::Clock::now();
    std::erase_if(buffers, [&](const auto& item) {
        return !item.second.monitor || (item.second.retired && now - *item.second.retired >= std::chrono::seconds(2));
    });
}

bool hasRetired() {
    return std::ranges::any_of(buffers, [](const auto& item) { return !item.second.monitor || item.second.retired.has_value(); });
}

void retireMonitor(MONITORID monitor) {
    if (auto it = buffers.find(monitor); it != buffers.end()) {
        it->second.monitor.reset();
        it->second.retired = CRenderBuffers::Clock::now();
        g_pGlobalState->renderBuffers.scheduleCleanup();
    }
}

void finishFrame(MONITORID monitor, uint64_t frame) {
    // This also covers fullscreen/solitary and empty-workspace frames which
    // bypass renderWorkspaceWindows entirely. Other monitors keep their cache.
    if (auto it = buffers.find(monitor); it != buffers.end() && it->second.frame != frame && !it->second.retired) {
        it->second.retired = CRenderBuffers::Clock::now();
        g_pGlobalState->renderBuffers.scheduleCleanup();
    }
}
} // namespace WindowScene
