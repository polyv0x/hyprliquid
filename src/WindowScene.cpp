#include "DamageRegion.hpp"
#include "WindowScene.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"
#include "RendererSymbols.hpp"
#include "NativeDecorationState.hpp"
#include "WindowGeometry.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#if HYPRLIQUID_RENDER_CONTEXT
#include <hyprland/src/render/scene/SceneSelection.hpp>
#endif
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <bit>
#include <unordered_map>

namespace WindowScene {
namespace {
#if HYPRLIQUID_RENDER_CONTEXT
using RenderWindow = void (*)(Render::IHyprRenderer*, Render::CRenderContext&, PHLWINDOW, PHLMONITOR,
                              const Render::SWindowRenderPresentation&, const Time::steady_tp&,
                              bool, Render::eRenderPassMode, bool, bool);
using RenderWorkspace = void (*)(Render::IHyprRenderer*, Render::CRenderContext&, PHLMONITOR, PHLWORKSPACE,
                                 const Time::steady_tp&, Render::eSceneMode);
#else
using RenderWindow = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&,
                              bool, Render::eRenderPassMode, bool, bool);
using RenderWorkspace = void (*)(Render::IHyprRenderer*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
#endif
using Hyprutils::Utils::CScopeGuard;

CFunctionHook* workspaceHook = nullptr;
RenderWindow renderWindow = nullptr;
std::shared_ptr<SWindowScene> queuedScene;
bool queuingBase = false;
uint64_t nextBackgroundGeneration = 0;
uint64_t nextInputSerial = 0;

struct SBuffers {
    PHLMONITORREF monitor;
    SP<Render::IFramebuffer> background;
    SP<Render::IFramebuffer> image;
    std::weak_ptr<SWindowScene> scene;
    std::optional<CRenderBuffers::Clock::time_point> retired;
    uint64_t frame = 0;
    uint64_t inputSerial = 0;
    // Retired/recreated scenes must not match inputs retained by the backdrop cache.
    uint64_t backgroundGeneration = ++nextBackgroundGeneration;
    std::vector<uint64_t> key;
    std::vector<uint64_t> inputKey;
    std::vector<SWindowScene::SDamage> damage;
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

bool surfacesInsideWindow(PHLWINDOW window) {
    const auto root = window->resource();
    if (!root || !window->wlSurface() || window->wlSurface()->small())
        return false;

    const auto size = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    bool contained = true;
    // Match the native main pass's traversal and minimum child quad size.
    // A child outside the pane can cover another pane's base glass, so those
    // trees must retain the original interleaved rendering order.
    root->breadthfirst([&](SP<CWLSurfaceResource> surface, const Vector2D& offset, void*) {
        const auto& state = surface->m_current;
        if (surface == root || !state.texture || state.size.x < 1 || state.size.y < 1)
            return;
        contained &= offset.x >= 0 && offset.y >= 0 &&
            offset.x + std::max(state.size.x, 2.0) <= size.x &&
            offset.y + std::max(state.size.y, 2.0) <= size.y;
    }, nullptr);
    return contained;
}

class CWindowScenePassElement : public HyprlandCompat::CPassElement {
  public:
    explicit CWindowScenePassElement(std::shared_ptr<SWindowScene> scene) : m_scene(std::move(scene)) {}
    Render::CRenderPass pass;
    Render::CRenderPass basePass;
    bool reuse = false;
    bool reuseBackground = false, reuseScene = false, repairScene = false;

    std::vector<UP<IPassElement>> draw() override {
        auto& renderer = *g_pHyprRenderer;
        const auto savedData = HyprlandCompat::renderData();
        const auto monitor = savedData.pMonitor.lock();
        const auto source = savedData.currentFB;
        if (!monitor || !source || !RenderSafety::dimension(source->m_size.x) || !RenderSafety::dimension(source->m_size.y))
            return {};

        const bool feedback = HyprlandCompat::blockSurfaceFeedback();
        const bool snapshot = HyprlandCompat::renderingSnapshot();
        const bool blur = monitor->m_blurFBShouldRender;
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        GLint scissorBox[4];
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
        CScopeGuard restore([&] {
            HyprlandCompat::renderData() = savedData;
            HyprlandCompat::blockSurfaceFeedback() = feedback;
            HyprlandCompat::renderingSnapshot() = snapshot;
            monitor->m_blurFBShouldRender = blur;
            HyprlandCompat::bindFB(source);
            renderer.setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            HyprlandCompat::scissor(CBox(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]), false);
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, scissor);
        });

        const auto copy = [&](const SP<Render::IFramebuffer>& from, const SP<Render::IFramebuffer>& to, const CRegion* region = nullptr) {
            g_pGlobalState->renderBuffers.touch(from);
            g_pGlobalState->renderBuffers.touch(to);
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(from));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(to));
            if (region) {
                const auto transform = HyprlandCompat::targetTransform(monitor);
                region->copy().transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y).forEachRect([&](const auto& r) {
                    glBlitFramebuffer(r.x1, r.y1, r.x2, r.y2, r.x1, r.y1, r.x2, r.y2, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                });
            } else glBlitFramebuffer(0, 0, source->m_size.x, source->m_size.y,
                              0, 0, source->m_size.x, source->m_size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        };
        if (!reuse) for (auto* framebuffer : {&m_scene->background, &m_scene->image}) {
            auto& fb = *framebuffer;
            if (!g_pGlobalState->renderBuffers.prepare(fb, source->m_size.x, source->m_size.y, source->m_drmFormat,
                    fb == m_scene->background ? "scene-background" : "scene-windows", std::format("monitor:{}", monitor->m_id)))
                return {};
            fb->setImageDescription(source->imageDescription());
            if (fb == m_scene->background && !reuseBackground) copy(source, fb);
        }

        if (!reuse && !reuseScene) {
            HyprlandCompat::blockSurfaceFeedback() = true;
            HyprlandCompat::renderingSnapshot() = true;
            HyprlandCompat::renderData().noSimplify = true;
            // Work in a native-sized scratch image, then commit only the
            // affected core. The extra border rebuilds all undisplaced Liquid
            // lookups without reading the retained scene's later clients as an
            // earlier pane's backdrop. No changed filter phase or blend order.
            auto work = m_scene->regional ? g_pGlobalState->renderBuffers.acquire(source->m_size.x, source->m_size.y, source) :
                CRenderBuffers::CLease(g_pGlobalState->renderBuffers, nullptr);
            auto image = m_scene->image;
            std::optional<CRegion> repair;
            const bool workspaceReady = m_scene->regional && g_pGlobalState->renderBuffers.prepare(work->sharp,
                source->m_size.x, source->m_size.y, source->m_drmFormat, "auxiliary-repair", std::format("monitor:{}", monitor->m_id));
            if (repairScene && workspaceReady) {
                image = work->sharp;
                image->setImageDescription(source->imageDescription());
                repair = DamageRegion::bounded(m_scene->auxiliaryDamage.copy().expand(4 * m_scene->windows.size() + 4),
                                                CBox{{}, monitor->m_transformedSize});
            }
            bool baseCopied = false;
            if (m_scene->cacheBase || m_scene->cacheTiledBase) {
                auto cached = g_pGlobalState->renderBuffers.acquireBackdrop(source->m_size.x, source->m_size.y, source,
                    std::format("monitor:{}:base", monitor->m_id), m_scene->backgroundKey, false);
                if (cached) {
                    const bool hit = cached->contentValid;
                    if (hit || g_pGlobalState->renderBuffers.prepare(cached->sharp, source->m_size.x, source->m_size.y,
                            source->m_drmFormat, "scene-base", std::format("monitor:{}", monitor->m_id))) {
                        if (!hit) {
                            cached->sharp->setImageDescription(source->imageDescription());
                            copy(m_scene->background, cached->sharp);
                            HyprlandCompat::bindFB(cached->sharp);
                            HyprlandCompat::drawPass(basePass, CRegion(CBox{{}, monitor->m_transformedSize}));
                            cached->contentValid = !m_scene->failed;
                        } else for (const auto& ref : m_scene->windows)
                            if (!m_scene->cacheTiledBase || !HyprlandCompat::floating(ref))
                                if (auto* decoration = decorationFor(ref.lock())) decoration->reusedSceneBase();
                        copy(cached->sharp, image, repair ? &*repair : nullptr);
                        baseCopied = true;
                    }
                }
            }
            if (!baseCopied) {
                copy(m_scene->background, image, repair ? &*repair : nullptr);
                if (m_scene->cacheBase || m_scene->cacheTiledBase) {
                    // If the shared image cannot fit, retain the smaller
                    // per-window input caches used by the fallback path.
                    const bool allBase = m_scene->cacheBase, tiledBase = m_scene->cacheTiledBase;
                    m_scene->cacheBase = m_scene->cacheTiledBase = false;
                    CScopeGuard restoreBase([&] { m_scene->cacheBase = allBase; m_scene->cacheTiledBase = tiledBase; });
                    HyprlandCompat::bindFB(image);
                    m_scene->repair = repair;
                    HyprlandCompat::drawPass(basePass, repair ? *repair : CRegion(CBox{{}, monitor->m_transformedSize}));
                    m_scene->repair.reset();
                }
            }
            HyprlandCompat::bindFB(image);
            m_scene->repair = repair;
            HyprlandCompat::drawPass(pass, repair ? *repair : CRegion(CBox{{}, monitor->m_transformedSize}));
            m_scene->repair.reset();
            if (repair) copy(image, m_scene->image, &m_scene->auxiliaryDamage);
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
            const auto transform = HyprlandCompat::targetTransform(monitor);
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

    bool needsLiveBlur() override { return !reuse && !reuseBackground; }
    bool needsPrecomputeBlur() override { return false; }
    bool disableSimplification() override { return !reuse; }
    std::optional<CBox> boundingBox() override {
        const auto monitor = HyprlandCompat::renderData().pMonitor.lock();
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
    if (!monitor || !workspace || HyprlandCompat::renderingSnapshot() || g_pSessionLockManager->isSessionLocked() ||
        HyprlandCompat::renderData().projectionType != Render::RPT_MONITOR)
        return nullptr;

    auto scene = std::make_shared<SWindowScene>();
    std::vector<CBox> tiledBoxes;
    bool hasGlass = false;
    bool local = !HyprlandCompat::special(workspace) && !monitor->m_activeSpecialWorkspace &&
        !workspace->m_renderOffset->isBeingAnimated() && workspace->m_alpha->value() == 1.0f;
    bool cacheBackground = local;
    bool disjoint = true, legacy = true, containedSurfaces = true, allGlass = true;
    std::vector<CBox> currentBoxes;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!HyprlandCompat::mapped(window) || window->isHidden() || !renderer->shouldRenderWindow(window, monitor))
            continue;
        const bool ignoreSpecial = HyprlandCompat::movingFromMonitor(window) && window->m_workspace && !HyprlandCompat::visible(window->m_workspace);
        if (!ignoreSpecial && HyprlandCompat::special(workspace) != window->onSpecialWorkspace())
            continue;

        if (HyprlandCompat::hasTransformers(window)) {
            cacheBackground = false;
            local = false;
            // Transformed floats keep their native pass without disabling
            // the ordinary windows' capture.
            if (HyprlandCompat::floating(window))
                continue;
            return nullptr;
        }
        if (!HyprlandCompat::floating(window)) {
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
            if (!box->intersection(other).empty()) { local = false; disjoint = false; }
        currentBoxes.push_back(*box);
        // Reordering base glass below clients requires every surface to stay
        // inside its pane, including browser video/content subsurfaces.
        // Native main-surface quads fill the pane even when buffer dimensions
        // differ (notably scaled Xwayland). Small surfaces use different
        // placement rules and retain native interleaving conservatively.
        if (!surfacesInsideWindow(window)) {
            legacy = false;
            containedSurfaces = false;
        }
        // Animations/snapshots and real underlays keep native painting order.
        if (window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated() ||
            HyprlandCompat::alphaWithout(window, Desktop::View::WINDOW_ALPHA_ACTIVE) != 1.0f) {
            local = false;
            cacheBackground = false;
        }
        if (auto* decoration = decorationFor(window); decoration && decoration->usesGlass()) {
            hasGlass = true;
            if (decoration->needsNeighborScene()) local = false;
            if (decoration->usesLiquidGlass()) legacy = false;
        } else { local = false; legacy = false; allGlass = false; } // Preserve native shadows and other materials' ordering.
    }
    // Match the compositor's planes while preserving each plane's z-order.
    // A float may precede a tile in WindowState after either is raised/lowered;
    // copying that list verbatim would reflect the tile over the floating pane.
    const auto focused = Desktop::focusState()->window();
    std::ranges::stable_sort(scene->windows, {}, [&](const PHLWINDOWREF& window) {
        return HyprlandCompat::floating(window) ? (HyprlandCompat::pinned(window) ? 3 : 2) : (window == focused ? 1 : 0);
    });
    scene->local = local;
    scene->cacheBackground = cacheBackground && g_pGlobalState->damageSurfaceHook;
    scene->cacheBase = scene->cacheBackground && disjoint && legacy;
    scene->cacheInputs = scene->cacheBase && Desktop::fadingOutState()->fadeouts().empty();
    scene->cacheTiledBase = !local && !scene->cacheBase && scene->cacheBackground && containedSurfaces && allGlass && !tiledBoxes.empty();
    // Keep the exact destination samples at antialiased tile edges independent
    // of neighboring clients, including fractional positions and Liquid corners.
    for (size_t i = 0; scene->cacheTiledBase && i < tiledBoxes.size(); ++i) {
        auto padded = tiledBoxes[i];
        padded.expand(2);
        for (size_t j = i + 1; j < tiledBoxes.size(); ++j)
            if (!padded.intersection(tiledBoxes[j]).empty()) scene->cacheTiledBase = false;
    }
    if (!local && scene->cacheBackground && containedSurfaces && allGlass && scene->windows.size() > 1 &&
        Desktop::fadingOutState()->fadeouts().empty()) {
        const auto bottom = scene->windows.front().lock();
        auto bounds = WindowGeometry::computeWindowBox(bottom, monitor).value();
        bounds.expand(-2);
        bool contained = !HyprlandCompat::floating(bottom) && !bounds.empty();
        for (size_t i = 1; contained && i < scene->windows.size(); ++i) {
            const auto box = WindowGeometry::computeWindowBox(scene->windows[i].lock(), monitor).value();
            contained = box.x >= bounds.x && box.y >= bounds.y &&
                box.x + box.w <= bounds.x + bounds.w && box.y + box.h <= bounds.y + bounds.h;
        }
        if (contained) scene->independentWindow = bottom;
    }
    scene->regional = !scene->local && !scene->cacheInputs && scene->cacheBackground && containedSurfaces && allGlass &&
        Desktop::fadingOutState()->fadeouts().empty();
    if (scene->cacheInputs || scene->independentWindow || scene->regional) for (const auto& ref : scene->windows) {
        const auto window = ref.lock();
        if (HyprlandCompat::popupCount(window) || HyprlandCompat::grouped(window)) { scene->cacheInputs = false; scene->independentWindow.reset(); scene->regional = false; }
        for (const auto& decoration : HyprlandCompat::decorations(window)) {
            const auto type = decoration->getDecorationType();
            if (type != DECORATION_SHADOW && type != DECORATION_BORDER && type != DECORATION_INNER_GLOW &&
                decoration.get() != decorationFor(window)) { scene->cacheInputs = false; scene->independentWindow.reset(); scene->regional = false; }
#if HYPRLIQUID_WINDOW_PRESENTATION
            if (decoration.get() != decorationFor(window) &&
                !NativeDecorationState::appendKey(scene->nativeDecorationKey, decoration)) {
                scene->cacheInputs = false;
                scene->independentWindow.reset();
                scene->regional = false;
            }
#endif
        }
    }
    return hasGlass && (local || scene->windows.size() > 1 || scene->cacheInputs) ? scene : nullptr;
}

std::vector<uint64_t> localKey(const SWindowScene& scene, PHLMONITOR monitor, PHLWORKSPACE workspace) {
    std::vector<uint64_t> key;
    const auto number = [&](double value) { key.push_back(std::bit_cast<uint64_t>(value)); };
    key.push_back(reinterpret_cast<uintptr_t>(workspace.get()));
    key.push_back(g_pGlobalState->getSceneGeneration(monitor));
    key.push_back(buffers.at(monitor->m_id).backgroundGeneration);
    key.push_back(reinterpret_cast<uintptr_t>(eglGetCurrentContext()));
    key.push_back(static_cast<uint64_t>(HyprlandCompat::renderData().projectionType));
    key.push_back(HyprlandCompat::renderData().transformDamage);
    number(monitor->m_scale);
    key.push_back(monitor->m_transform);
    number(monitor->m_position.x); number(monitor->m_position.y);
    number(monitor->m_transformedSize.x); number(monitor->m_transformedSize.y);
    const auto source = HyprlandCompat::renderData().currentFB;
    key.push_back(source ? source->m_drmFormat : 0);
    key.push_back(source && source->imageDescription() ? source->imageDescription()->id() : 0);
    for (const auto& ref : scene.windows) {
        const auto window = ref.lock();
        key.push_back(reinterpret_cast<uintptr_t>(window.get()));
        const auto box = WindowGeometry::computeWindowBox(window, monitor).value();
        number(box.x); number(box.y); number(box.w); number(box.h);
        number(HyprlandCompat::rounding(window)); number(HyprlandCompat::roundingPower(window));
        if (auto* decoration = decorationFor(window)) decoration->appendCacheKey(key);
    }
    // Geometry, opacity, mapping and order of wallpaper/bottom layers also
    // matter when no client has committed new pixels (native animations).
    for (size_t plane = 0; plane < 2; ++plane) {
        key.push_back(plane);
        for (const auto& layer : monitor->m_layerSurfaceLayers[plane]) {
            if (!layer) continue;
            key.push_back(reinterpret_cast<uintptr_t>(layer.get()));
            key.push_back(HyprlandCompat::mapped(layer));
            const auto position = layer->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const auto size = layer->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            number(position.x); number(position.y); number(size.x); number(size.y);
            number(layer->alpha().get(Desktop::View::LS_ALPHA_FADE)->value());
        }
    }
    return key;
}

std::vector<uint64_t> inputKey(const SWindowScene& scene) {
    auto key = scene.backgroundKey;
    key.push_back(scene.nativeDecorationKey.size());
    key.insert(key.end(), scene.nativeDecorationKey.begin(), scene.nativeDecorationKey.end());
    key.push_back(scene.regional);
    key.push_back(scene.regionalInputs);
    const auto number = [&](double value) { key.push_back(std::bit_cast<uint64_t>(value)); };
    static auto shadowEnabled = CConfigValue<Config::INTEGER>("decoration:shadow:enabled");
    static auto shadowRange = CConfigValue<Config::INTEGER>("decoration:shadow:range");
    static auto shadowSharp = CConfigValue<Config::INTEGER>("decoration:shadow:sharp");
    static auto shadowPower = CConfigValue<Config::INTEGER>("decoration:shadow:render_power");
    static auto shadowScale = CConfigValue<Config::FLOAT>("decoration:shadow:scale");
    static auto shadowOffset = CConfigValue<Config::VEC2>("decoration:shadow:offset");
    static auto glowEnabled = CConfigValue<Config::INTEGER>("decoration:glow:enabled");
    static auto glowRange = CConfigValue<Config::INTEGER>("decoration:glow:range");
    static auto glowPower = CConfigValue<Config::INTEGER>("decoration:glow:render_power");
    static auto borderPart = CConfigValue<Config::INTEGER>("decoration:border_part_of_window");
    static auto dimAround = CConfigValue<Config::FLOAT>("decoration:dim_around");
    for (const auto value : {*shadowEnabled, *shadowRange, *shadowSharp, *shadowPower,
            *glowEnabled, *glowRange, *glowPower, *borderPart}) number(value);
    number(*shadowScale); number((*shadowOffset).x); number((*shadowOffset).y); number(*dimAround);
    for (const auto& ref : scene.windows) {
        const auto window = ref.lock();
        number(HyprlandCompat::alpha(window));
        number(HyprlandCompat::borderSize(window));
        number(HyprlandCompat::reportedSize(window).x); number(HyprlandCompat::reportedSize(window).y);
        number(HyprlandCompat::surfaceScale(window));
        key.push_back(window->m_ruleApplicator->opaque().valueOrDefault());
        key.push_back(window->m_ruleApplicator->nearestNeighbor().valueOrDefault());
        key.push_back(window->m_ruleApplicator->noDim().valueOrDefault());
        key.push_back(window->m_ruleApplicator->dimAround().valueOrDefault());
#if HYPRLIQUID_WINDOW_PRESENTATION
        key.push_back(window->m_ruleApplicator->decorate().valueOrDefault());
        key.push_back(window->m_ruleApplicator->noShadow().valueOrDefault());
        key.push_back(window->m_ruleApplicator->noGlow().valueOrDefault());
        key.push_back(window->backend().traits().suggestsNoBorder);
        key.push_back(window->backend().traits().overrideRedirect);
        const auto shadowCutout = window->surfaceLogicalBox();
        key.push_back(shadowCutout.has_value());
        if (shadowCutout) {
            number(shadowCutout->x); number(shadowCutout->y); number(shadowCutout->w); number(shadowCutout->h);
        }
        number(window->presentation().dimPercent());
        number(window->presentation().notRespondingTint());
#else
        key.push_back(window->m_ruleApplicator->decorate().valueOrDefault());
        key.push_back(window->m_ruleApplicator->noShadow().valueOrDefault());
        key.push_back(window->m_X11DoesntWantBorders);
        key.push_back(window->isX11OverrideRedirect());
        const auto shadowCutout = window->surfaceLogicalBox();
        key.push_back(shadowCutout.has_value());
        if (shadowCutout) {
            number(shadowCutout->x); number(shadowCutout->y); number(shadowCutout->w); number(shadowCutout->h);
        }
        for (const auto* value : {window->m_dimPercent.get(), window->m_notRespondingTint.get(),
                window->m_borderFadeAnimationProgress.get(), window->m_borderAngleAnimationProgress.get(),
                window->m_shadowFadeAnimationProgress.get(), window->m_shadowAngleAnimationProgress.get(),
                window->m_glowFadeAnimationProgress.get(), window->m_glowAngleAnimationProgress.get()})
            number(value ? value->value() : 0);
        for (const auto* gradient : {&window->m_realBorderColor, &window->m_realBorderColorPrevious,
                &window->m_realShadowColor, &window->m_realShadowColorPrevious,
                &window->m_realGlowColor, &window->m_realGlowColorPrevious}) {
            number(gradient->m_angle);
            key.push_back(gradient->m_colors.size());
            for (const auto& color : gradient->m_colors) {
                number(color.r); number(color.g); number(color.b); number(color.a);
            }
        }
#endif
        // Encode the native drawing order and each surface's rendering state.
        // Buffer identity is deliberately absent: pixel commits use damage,
        // while movement, restacking and map/unmap must invalidate even when
        // no buffer pixels were damaged.
        const auto countIndex = key.size();
        key.push_back(0);
        window->resource()->breadthfirst([&](SP<CWLSurfaceResource> resource, const Vector2D& offset, void*) {
            ++key[countIndex];
            key.push_back(reinterpret_cast<uintptr_t>(resource.get()));
            number(offset.x); number(offset.y);
            key.push_back(resource->m_mapped);
            const auto surface = Desktop::View::CWLSurface::fromResource(resource);
            key.push_back(bool(surface));
            if (surface) {
                number(surface->m_alphaModifier); number(surface->m_overallOpacity);
                key.push_back(surface->m_fillIgnoreSmall);
                surface->m_visibleRegion.forEachRect([&](const auto& r) {
                    number(r.x1); number(r.y1); number(r.x2); number(r.y2);
                });
                key.push_back(UINT64_MAX);
            }
            const auto& state = resource->m_current;
            key.push_back(state.transform); key.push_back(state.scale);
            number(state.size.x); number(state.size.y);
            number(state.offset.x); number(state.offset.y);
            number(state.bufferSize.x); number(state.bufferSize.y);
            key.push_back(state.viewport.hasSource); key.push_back(state.viewport.hasDestination);
            number(state.viewport.source.x); number(state.viewport.source.y);
            number(state.viewport.source.w); number(state.viewport.source.h);
            number(state.viewport.destination.x); number(state.viewport.destination.y);
            state.opaque.forEachRect([&](const auto& r) {
                number(r.x1); number(r.y1); number(r.x2); number(r.y2);
            });
            key.push_back(UINT64_MAX);
            const auto texture = state.texture;
            key.push_back(bool(texture));
            key.push_back(texture && texture->m_opaque);
            key.push_back(texture && texture->m_imageDescription ? texture->m_imageDescription->id() : 0);
        }, nullptr);
    }
    return key;
}

#if HYPRLIQUID_RENDER_CONTEXT
void renderWorkspace(Render::IHyprRenderer* renderer, Render::CRenderContext& ctx, PHLMONITOR monitor,
                      PHLWORKSPACE workspace, const Time::steady_tp& now, Render::eSceneMode mode) {
    HyprlandCompat::CRenderScope scope(ctx);
    const auto original = [&] {
        reinterpret_cast<RenderWorkspace>(workspaceHook->m_original)(renderer, ctx, monitor, workspace, now, mode);
    };
    // Capture/workspace scenes have different privacy and presentation rules.
    // Keep the native pipeline there, including its content visibility checks.
    if (mode != Render::eSceneMode::MONITOR || ctx.m_renderingCapture) { original(); return; }
#else
void renderWorkspace(Render::IHyprRenderer* renderer, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    const auto original = [&] {
        reinterpret_cast<RenderWorkspace>(workspaceHook->m_original)(renderer, monitor, workspace, now);
    };
#endif
    auto scene = collectScene(renderer, monitor, workspace);
    // Ineligible scenes used to return before expired-monitor cleanup. Retirement
    // drops only the cache's references: queued passes keep their images alive.
    for (auto& [_, storage] : buffers)
        if (!storage.monitor && !storage.retired) storage.retired = CRenderBuffers::Clock::now();
    if (!scene) {
        if (monitor) if (const auto it = buffers.find(monitor->m_id); it != buffers.end())
            it->second.inputSerial = ++nextInputSerial;
        if (hasRetired()) g_pGlobalState->renderBuffers.scheduleCleanup();
        original();
        return;
    }

    auto& storage = buffers[monitor->m_id];
    scene->previousInputSerial = storage.inputSerial;
    scene->inputSerial = storage.inputSerial = ++nextInputSerial;
    storage.monitor = monitor;
    storage.retired.reset();
    storage.frame = g_pGlobalState->renderBuffers.frame();
    if (!scene->local && scene->windows.size() == 1 && scene->cacheInputs) {
        // With no neighbours, cache the native decoration's input in place.
        // Keeping its original pass preserves shadows/dimming below the glass
        // and avoids allocating or drawing a monitor-sized auxiliary scene.
        scene->backgroundKey = localKey(*scene, monitor, workspace);
        scene->inputKey = inputKey(*scene);
        scene->damage = std::move(storage.damage);
        storage.damage.clear();
        for (const auto& change : scene->damage) {
            HyprlandCompat::renderData().damage.add(change.region);
            HyprlandCompat::renderData().finalDamage.add(change.region);
            monitor->m_damage.damage(change.region);
        }
        scene->ready = true;
        if (storage.background || storage.image)
            g_pGlobalState->renderBuffers.discardBackdrop(std::format("monitor:{}:base", monitor->m_id));
        storage.background.reset();
        storage.image.reset();
        storage.cache.reset();
        storage.key.clear();
        storage.inputKey.clear();
        storage.scene = scene;
        const auto previousScene = queuedScene;
        queuedScene = scene;
        CScopeGuard restoreScene([&] { queuedScene = previousScene; });
        original();
        return;
    }
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
        const auto savedData = HyprlandCompat::renderData();
        const bool snapshot = HyprlandCompat::renderingSnapshot();
        auto redirect = HyprlandCompat::redirectPass(&capture->pass);
        queuingBase = true;
        HyprlandCompat::renderingSnapshot() = true;
        CScopeGuard restore([&] {
            HyprlandCompat::renderData() = savedData;
            HyprlandCompat::renderingSnapshot() = snapshot;
            queuingBase = false;
        });
        for (const auto& window : scene->windows) {
            // Only our glass plus the client's main surfaces. Popups, shadows
            // and other plugins' decorations belong to the normal scene pass.
            if (auto* decoration = decorationFor(window.lock())) {
                if (scene->cacheBase || (scene->cacheTiledBase && !HyprlandCompat::floating(window))) {
                    auto baseRedirect = HyprlandCompat::redirectPass(&capture->basePass);
                    decoration->draw(monitor, 1.0f);
                } else decoration->draw(monitor, 1.0f);
            }
            if (!scene->local)
#if HYPRLIQUID_RENDER_CONTEXT
                renderWindow(renderer, ctx, window.lock(), monitor, window->presentation().renderPresentation(),
                             now, false, Render::RENDER_PASS_MAIN, false, false);
#else
                renderWindow(renderer, window.lock(), monitor, now, false, Render::RENDER_PASS_MAIN, false, false);
#endif
        }
    }
    auto key = scene->local || scene->cacheBackground ? localKey(*scene, monitor, workspace) : std::vector<uint64_t>{};
    if (scene->cacheBackground) scene->backgroundKey = key;
    const auto source = HyprlandCompat::renderData().currentFB;
    if (scene->regional && source) {
        uint64_t bytes = CRenderBuffers::bytes(source->m_size.x, source->m_size.y, source->m_drmFormat);
        size_t pairs = 1; // Auxiliary repair workspace.
        for (const auto& ref : scene->windows) {
            if (ref == scene->independentWindow) continue;
            const auto cost = decorationFor(ref.lock())->regionalInputCost(monitor, source->m_drmFormat);
            bytes += cost.bytes;
            pairs += cost.pairs;
            if (bytes > CRenderBuffers::RETAINED_BYTES || pairs > CRenderBuffers::RETAINED_PAIRS) break;
        }
        // Oversubscribing named inputs evicts another receiver on every draw.
        // Keep regional auxiliary repair but use shared full inputs instead.
        scene->regionalInputs = bytes <= CRenderBuffers::RETAINED_BYTES && pairs <= CRenderBuffers::RETAINED_PAIRS;
    }
    if (scene->cacheInputs || scene->cacheTiledBase || scene->independentWindow || scene->regional) scene->inputKey = inputKey(*scene);
    scene->damage = std::move(storage.damage);
    storage.damage.clear();
    // Surface texture filtering reaches past integer-rounded native damage at
    // fractional scales. Cached glass bypasses live-blur expansion, so carry
    // the guarded surface damage into the actual framebuffer repaint too.
    for (const auto& change : scene->damage) {
        HyprlandCompat::renderData().damage.add(change.region);
        HyprlandCompat::renderData().finalDamage.add(change.region);
        // The fractional-filter guard must survive buffer reuse as well.
        monitor->m_damage.damage(change.region);
    }
    if (!scene->local && !scene->regional) {
        // Neighbor reflections change beyond native blur's presentation damage
        // even when finished-input caching is unavailable. Regional scenes
        // propagate their ordered optical damage separately below.
        // Native render damage is larger than final transfer damage: having
        // rendered the reflection correctly does not ensure it gets displayed.
        for (const auto& window : scene->windows) {
            const auto* decoration = decorationFor(window.lock());
            if (!decoration || !decoration->usesGlass()) continue;
            CRegion input;
            for (const auto& change : scene->damage)
                if (change.window != window) input.add(change.region);
            if (input.empty()) continue;
            const auto output = decoration->affectedDamage(monitor, input);
            HyprlandCompat::renderData().damage.add(output);
            HyprlandCompat::renderData().finalDamage.add(output);
            // beginRender already rotated the damage ring. Keep these new
            // optical changes pending for the next buffer acquisition too,
            // so an older swapchain buffer cannot bring stale reflections
            // back on an unrelated frame. Do not schedule an extra frame.
            monitor->m_damage.damage(output);
        }
    }
    capture->reuse = scene->local && g_pGlobalState->damageSurfaceHook && storage.cache->valid &&
        key == storage.key && source && storage.image->isAllocated() &&
        storage.image->m_size == source->m_size && storage.image->m_drmFormat == source->m_drmFormat;
    const bool reuse = capture->reuse;
    capture->reuseBackground = scene->cacheBackground && storage.cache->valid && key == storage.key && source &&
        storage.background->isAllocated() && storage.background->m_size == source->m_size &&
        storage.background->m_drmFormat == source->m_drmFormat;
    capture->reuseScene = capture->reuseBackground && (scene->cacheInputs || scene->cacheTiledBase || scene->independentWindow || scene->regional) &&
        scene->inputKey == storage.inputKey && scene->damage.empty();
    capture->repairScene = scene->regional && capture->reuseBackground && scene->inputKey == storage.inputKey &&
        storage.image->isAllocated() && storage.image->m_size == source->m_size && storage.image->m_drmFormat == source->m_drmFormat;
    if (scene->regional) {
        const CBox screen{{}, monitor->m_transformedSize};
        for (const auto& change : scene->damage) scene->auxiliaryDamage.add(change.region);
        // Auxiliary material rays see wallpaper. Only Liquid's undisplaced
        // native destination lookup can carry client changes across a pixel
        // boundary, once per pane in painting order.
        scene->auxiliaryDamage = DamageRegion::bounded(scene->auxiliaryDamage.copy().expand(4 * scene->windows.size()), screen);
        CRegion beneath;
        for (const auto& ref : scene->windows) {
            const auto window = ref.lock();
            const auto box = WindowGeometry::computeWindowBox(window, monitor).value();
            auto input = scene->auxiliaryDamage.copy().subtract(CRegion(box)).add(beneath);
            auto output = decorationFor(window)->affectedDamage(monitor, input);
            scene->receivers.push_back({ref, std::move(input), output});
            beneath.add(output);
            for (const auto& change : scene->damage) if (change.window == window) beneath.add(change.region);
        }
        // Rebuild the native backdrop beneath every propagated material change
        // before the receivers execute. A new cache needs a complete initial
        // image; later frames only repaint the conservatively affected region.
        if (!capture->repairScene) {
            beneath = CRegion(screen);
            scene->previousInputSerial = 0;
        }
        HyprlandCompat::renderData().damage.add(beneath);
        HyprlandCompat::renderData().finalDamage.add(beneath);
        monitor->m_damage.damage(beneath);
    }
    if (!reuse && !capture->reuseScene) storage.cache->valid = false;
    storage.key = std::move(key);
    storage.inputKey = scene->inputKey;
    const bool reuseBackground = capture->reuseBackground;
    HyprlandCompat::addPassElement(std::move(capture));

    // A fresh auxiliary background reads wallpaper across the monitor. Repaint
    // it completely on this already scheduled frame. A retained immutable
    // background needs no extra desktop damage, even when clients are changing.
    if (!reuse && !reuseBackground) {
        HyprlandCompat::renderData().damage.add(CBox{{}, monitor->m_transformedSize});
        HyprlandCompat::renderData().finalDamage.add(CBox{{}, monitor->m_transformedSize});
    }
    original();
}
} // namespace

bool initialize() {
    // If a future native implementation cannot be inspected, only dependent
    // cache reuse becomes ineligible; the plugin and immutable caches still work.
    NativeDecorationState::initialize();
    renderWindow = reinterpret_cast<RenderWindow>(rendererSymbol(PHANDLE, "renderWindow"));
    if (!renderWindow) {
        destroy();
        return false;
    }
    if (const auto address = rendererSymbol(PHANDLE, "renderWorkspaceWindows")) {
        workspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, address, reinterpret_cast<void*>(renderWorkspace));
        if (workspaceHook && workspaceHook->hook())
            return true;
    }
    destroy();
    return false;
}

void destroy() {
    HyprlandCompat::renderPass().removeAllOfType("CWindowScenePassElement");
    if (workspaceHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, workspaceHook);
    workspaceHook = nullptr;
    NativeDecorationState::destroy();
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

void damageWindow(PHLWINDOW window, const CRegion& globalDamage) {
    for (auto& [_, storage] : buffers) {
        const auto monitor = storage.monitor.lock();
        if (!monitor || !g_pHyprRenderer->shouldRenderWindow(window, monitor)) continue;
        auto damage = globalDamage.copy().translate(-monitor->m_position).scale(monitor->m_scale).expand(1);
        const auto box = WindowGeometry::computeWindowBox(window, monitor);
        if (!box) continue;
        damage.intersect(*box);
        if (damage.empty()) continue;
        auto found = std::ranges::find_if(storage.damage, [&](const auto& entry) { return entry.window == window; });
        if (found == storage.damage.end()) storage.damage.push_back({window, std::move(damage)});
        else found->region.add(damage);
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
        const bool expired = !item.second.monitor || (item.second.retired && now - *item.second.retired >= std::chrono::seconds(2));
        if (expired) g_pGlobalState->renderBuffers.discardBackdrop(std::format("monitor:{}:base", item.first));
        return expired;
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
        // Hidden clients can commit without monitor damage. Returning to this
        // scene must not reuse inputs from before the skipped workspace pass.
        it->second.inputSerial = ++nextInputSerial;
        it->second.retired = CRenderBuffers::Clock::now();
        g_pGlobalState->renderBuffers.scheduleCleanup();
    }
}
} // namespace WindowScene
