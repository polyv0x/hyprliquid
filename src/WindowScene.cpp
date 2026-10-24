#include "WindowScene.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"
#include "RendererSymbols.hpp"
#include "WindowGeometry.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <bit>
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
uint64_t nextBackgroundGeneration = 0;

struct SBuffers {
    PHLMONITORREF monitor;
    SP<Render::IFramebuffer> background;
    SP<Render::IFramebuffer> image;
    std::weak_ptr<SWindowScene> scene;
    std::optional<CRenderBuffers::Clock::time_point> retired;
    uint64_t frame = 0;
    // Retired/recreated scenes must not match inputs retained by the backdrop cache.
    uint64_t backgroundGeneration = ++nextBackgroundGeneration;
    std::vector<uint64_t> key;
    std::shared_ptr<SWindowScene::SCache> cache;
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
    bool reuse = false;

    std::vector<UP<IPassElement>> draw() override {
        auto& renderer = *g_pHyprRenderer;
        const auto savedData = renderer.m_renderData;
        const auto monitor = savedData.pMonitor.lock();
        const auto source = savedData.currentFB;
        if (!monitor || !source || !RenderSafety::dimension(source->m_size.x) || !RenderSafety::dimension(source->m_size.y))
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

        if (!reuse) for (auto* framebuffer : {&m_scene->background, &m_scene->image}) {
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

        if (!reuse) {
            renderer.bindFB(m_scene->image);
            renderer.m_bBlockSurfaceFeedback = true;
            renderer.m_bRenderingSnapshot = true;
            renderer.m_renderData.noSimplify = true;
            pass.render(CRegion(CBox{{}, monitor->m_transformedSize}));
            if (m_scene->cache) m_scene->cache->valid = !m_scene->failed;
        }
        m_scene->ready = true;
        if (m_scene->local) {
            // The cached image contains background and glass, never client
            // foregrounds. Copy only damage left after native occlusion culling.
            // Both buffers keep the native size/format and projection, avoiding
            // resampling, another blend, or intermediate precision changes.
            g_pGlobalState->renderBuffers.touch(m_scene->image);
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(m_scene->image));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(source));
            auto damage = savedData.damage.copy().intersect(CBox{{}, monitor->m_transformedSize});
            const auto transform = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
            damage.forEachRect([&](const auto& rect) {
                CBox box{rect.x1, rect.y1, rect.x2 - rect.x1, rect.y2 - rect.y1};
                box.transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y);
                glBlitFramebuffer(box.x, box.y, box.x + box.w, box.y + box.h,
                                  box.x, box.y, box.x + box.w, box.y + box.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            });
            if (reuse) for (const auto& ref : m_scene->windows)
                if (auto* decoration = decorationFor(ref.lock()); decoration && decoration->usesGlass())
                    decoration->reusedLocalBackdrop();
        }
        return {};
    }

    bool needsLiveBlur() override { return !reuse; }
    bool needsPrecomputeBlur() override { return false; }
    bool disableSimplification() override { return !reuse; }
    std::optional<CBox> boundingBox() override {
        const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor || !std::isfinite(monitor->m_scale) || monitor->m_scale <= 0)
            return std::nullopt;
        return CBox{{}, monitor->m_transformedSize / monitor->m_scale};
    }
    const char* passName() override { return "CWindowScenePassElement"; }
    ePassElementType type() override { return EK_CUSTOM; }

  private:
    std::shared_ptr<SWindowScene> m_scene;
};

std::shared_ptr<SWindowScene> collectScene(Render::IHyprRenderer* renderer, PHLMONITOR monitor, PHLWORKSPACE workspace) {
    if (!monitor || !workspace || renderer->m_bRenderingSnapshot || g_pSessionLockManager->isSessionLocked() ||
        renderer->m_renderData.projectionType != Render::RPT_MONITOR)
        return nullptr;

    auto scene = std::make_shared<SWindowScene>();
    std::vector<CBox> tiledBoxes;
    bool hasGlass = false;
    bool local = !workspace->m_isSpecialWorkspace && !monitor->m_activeSpecialWorkspace &&
        !workspace->m_renderOffset->isBeingAnimated() && workspace->m_alpha->value() == 1.0f;
    bool cacheBackground = local;
    std::vector<CBox> currentBoxes;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->m_isMapped || window->isHidden() || !renderer->shouldRenderWindow(window, monitor))
            continue;
        const bool ignoreSpecial = window->m_monitorMovedFrom != -1 && window->m_workspace && !window->m_workspace->isVisible();
        if (!ignoreSpecial && workspace->m_isSpecialWorkspace != window->onSpecialWorkspace())
            continue;

        if (!window->m_transformers.empty()) {
            cacheBackground = false;
            local = false;
            // Transformed floats keep their native pass without disabling
            // the ordinary windows' capture.
            if (window->m_isFloating)
                continue;
            return nullptr;
        }
        if (!window->m_isFloating) {
            // Keep stacked tiling layouts native. Test destination boxes so
            // transient overlap while opening a split keeps its reflections.
            const auto goal = WindowGeometry::computeWindowBox(window, monitor, Desktop::View::IGeometric::GEOMETRIC_GOAL);
            if (!goal)
                return nullptr;
            for (const auto& other : tiledBoxes)
                if (!goal->intersection(other).empty())
                    return nullptr;
            tiledBoxes.push_back(*goal);
        }
        // Floats stay in the scene through overlap. Sampling uses this capture
        // only outside the owner, retaining its real backdrop inside its bounds.
        scene->windows.emplace_back(window);
        const auto box = WindowGeometry::computeWindowBox(window, monitor);
        if (!box) return nullptr;
        for (const auto& other : currentBoxes)
            if (!box->intersection(other).empty()) local = false;
        currentBoxes.push_back(*box);
        // Animations/snapshots and real underlays keep native painting order.
        if (window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated() ||
            window->alphaTotalWithout(Desktop::View::WINDOW_ALPHA_ACTIVE) != 1.0f) {
            local = false;
            cacheBackground = false;
        }
        if (auto* decoration = decorationFor(window); decoration && decoration->usesGlass()) {
            hasGlass = true;
            if (decoration->needsNeighborScene()) local = false;
        } else local = false; // Preserve native shadows and other materials' ordering.
    }
    // Match the compositor's planes while preserving each plane's z-order.
    // A float may precede a tile in WindowState after either is raised/lowered;
    // copying that list verbatim would reflect the tile over the floating pane.
    const auto focused = Desktop::focusState()->window();
    std::ranges::stable_sort(scene->windows, {}, [&](const PHLWINDOWREF& window) {
        return window->m_isFloating ? (window->m_pinned ? 3 : 2) : (window == focused ? 1 : 0);
    });
    scene->local = local;
    scene->cacheBackground = cacheBackground && g_pGlobalState->damageSurfaceHook;
    return hasGlass && (local || scene->windows.size() > 1) ? scene : nullptr;
}

std::vector<uint64_t> localKey(const SWindowScene& scene, PHLMONITOR monitor, PHLWORKSPACE workspace) {
    std::vector<uint64_t> key;
    const auto number = [&](double value) { key.push_back(std::bit_cast<uint64_t>(value)); };
    key.push_back(reinterpret_cast<uintptr_t>(workspace.get()));
    key.push_back(g_pGlobalState->getSceneGeneration(monitor));
    key.push_back(buffers.at(monitor->m_id).backgroundGeneration);
    key.push_back(reinterpret_cast<uintptr_t>(eglGetCurrentContext()));
    number(monitor->m_scale);
    key.push_back(monitor->m_transform);
    number(monitor->m_position.x); number(monitor->m_position.y);
    number(monitor->m_transformedSize.x); number(monitor->m_transformedSize.y);
    const auto source = g_pHyprRenderer->m_renderData.currentFB;
    key.push_back(source ? source->m_drmFormat : 0);
    key.push_back(source && source->imageDescription() ? source->imageDescription()->id() : 0);
    for (const auto& ref : scene.windows) {
        const auto window = ref.lock();
        key.push_back(reinterpret_cast<uintptr_t>(window.get()));
        const auto box = WindowGeometry::computeWindowBox(window, monitor).value();
        number(box.x); number(box.y); number(box.w); number(box.h);
        number(window->rounding()); number(window->roundingPower());
        if (auto* decoration = decorationFor(window)) decoration->appendCacheKey(key);
    }
    // Geometry, opacity, mapping and order of wallpaper/bottom layers also
    // matter when no client has committed new pixels (native animations).
    for (size_t plane = 0; plane < 2; ++plane) {
        key.push_back(plane);
        for (const auto& layer : monitor->m_layerSurfaceLayers[plane]) {
            if (!layer) continue;
            key.push_back(reinterpret_cast<uintptr_t>(layer.get()));
            key.push_back(layer->m_mapped);
            const auto position = layer->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const auto size = layer->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            number(position.x); number(position.y); number(size.x); number(size.y);
            number(layer->alpha().get(Desktop::View::LS_ALPHA_FADE)->value());
        }
    }
    return key;
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
        storage.cache.reset();
    }
    if (!storage.background)
        storage.background = renderer->createFB("hyprliquid-scene-background");
    if (!storage.image)
        storage.image = renderer->createFB("hyprliquid-scene-windows");
    scene->background = storage.background;
    scene->image = storage.image;
    if (!storage.cache) storage.cache = std::make_shared<SWindowScene::SCache>();
    scene->cache = storage.cache;
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
            if (!scene->local)
                renderWindow(renderer, window.lock(), monitor, now, false, Render::RENDER_PASS_MAIN, false, false);
        }
    }
    auto key = scene->local || scene->cacheBackground ? localKey(*scene, monitor, workspace) : std::vector<uint64_t>{};
    if (scene->cacheBackground) scene->backgroundKey = key;
    const auto source = renderer->m_renderData.currentFB;
    capture->reuse = scene->local && g_pGlobalState->damageSurfaceHook && storage.cache->valid &&
        key == storage.key && source && storage.image->isAllocated() &&
        storage.image->m_size == source->m_size && storage.image->m_drmFormat == source->m_drmFormat;
    const bool reuse = capture->reuse;
    if (!reuse) storage.cache->valid = false;
    storage.key = std::move(key);
    renderer->addPassElement(std::move(capture));

    // The auxiliary scene reads the wallpaper across the monitor. Repaint it
    // completely on this already scheduled frame, without scheduling another
    // frame or retaining last frame's windows in the backdrop.
    if (!reuse) {
        renderer->m_renderData.damage.add(CBox{{}, monitor->m_transformedSize});
        renderer->m_renderData.finalDamage.add(CBox{{}, monitor->m_transformedSize});
    }
    original(renderer, monitor, workspace, now);
}
} // namespace

bool initialize() {
    renderWindow = reinterpret_cast<RenderWindow>(rendererSymbol(PHANDLE, "renderWindow", "IHyprRenderer::renderWindow("));
    if (!renderWindow)
        return false;
    if (const auto address = rendererSymbol(PHANDLE, "renderWorkspaceWindows", "IHyprRenderer::renderWorkspaceWindows(")) {
        workspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, address, reinterpret_cast<void*>(renderWorkspace));
        if (workspaceHook && workspaceHook->hook())
            return true;
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

bool buildingBase() { return queuingBase && !(queuedScene && queuedScene->local); }

bool skipDecoration(PHLWINDOW window) {
    const auto scene = queuedFor(window);
    return !queuingBase && scene && scene->local;
}

void invalidateBackground(PHLMONITOR monitor) {
    if (!monitor) return;
    if (auto it = buffers.find(monitor->m_id); it != buffers.end()) {
        it->second.backgroundGeneration = ++nextBackgroundGeneration;
        if (it->second.cache) it->second.cache->valid = false;
    }
}

void invalidateAll() {
    for (auto& [_, storage] : buffers) {
        storage.backgroundGeneration = ++nextBackgroundGeneration;
        if (storage.cache) storage.cache->valid = false;
    }
}

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
        if (it->second.cache) it->second.cache->valid = false;
        it->second.retired = CRenderBuffers::Clock::now();
        g_pGlobalState->renderBuffers.scheduleCleanup();
    }
}
} // namespace WindowScene
