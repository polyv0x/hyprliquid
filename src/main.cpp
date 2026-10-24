#include "GlassDecoration.hpp"
#include "GlassLayerCompositeElement.hpp"
#include "GlassLayerPassElement.hpp"
#include "GlassLayerSurface.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "PluginConfig.hpp"
#include "RendererSymbols.hpp"
#include "WindowScene.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include <cstdlib>
#include <algorithm>
#include <iterator>
#include <optional>
#include <sstream>

static void clearLayerGlassOnClose(PHLLS layerSurface) {
    if (!g_pGlobalState || !layerSurface)
        return;

    // Drop cached layer glass immediately. Otherwise the previous glass output
    // can remain in the damage history while Hyprland switches to its close
    // snapshot path, showing stale/black pixels for a frame.
    std::erase_if(g_pGlobalState->layerSurfaces, [&](const auto& pair) {
        return pair.first == layerSurface.get() || pair.second->getLayerSurface() == layerSurface;
    });

    if (auto monitor = layerSurface->m_monitor.lock())
        g_pHyprRenderer->damageMonitor(monitor);
}

static void onNewWindow(PHLWINDOW window) {
    if (std::ranges::any_of(window->m_windowDecorations,
                            [](const auto& decoration) { return decoration->getDisplayName() == "Hyprliquid"; }))
        return;

    auto decoration = makeUnique<CGlassDecoration>(window);
    const auto* glass = decoration.get();
    g_pGlobalState->decorations.emplace_back(decoration);
    decoration->m_self = decoration;
    if (!HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(decoration)))
        return;

    // BOTTOM decorations draw in insertion order, not positioning priority.
    // Sample/draw the glass before this window's native shadow; otherwise
    // strong outward refraction pulls the shadow's cutout edge into the pane.
    // Moving our owned pointer preserves every other decoration's order and
    // leaves the native shadow/border passes and their clipping intact.
    auto& decorations = window->m_windowDecorations;
    const auto inserted = std::ranges::find_if(decorations, [glass](const auto& item) { return item.get() == glass; });
    if (inserted != decorations.end())
        std::rotate(decorations.begin(), inserted, std::next(inserted));
}

static void onCloseWindow(PHLWINDOW window) {
    std::erase_if(g_pGlobalState->decorations, [&window](const auto& decoration) {
        auto* deco = decoration.get();
        return !deco || deco->getOwner() == window;
    });
}

static CGlassDecoration* glassDecorationFor(const PHLWINDOW& window) {
    for (const auto& decoration : g_pGlobalState->decorations) {
        auto* deco = decoration.get();
        if (deco && deco->getOwner() == window)
            return deco;
    }
    return nullptr;
}

// Hyprland skips window decorations when internal fullscreen mode is
// FSMODE_FULLSCREEN, queue the glass pass from RENDER_PRE_WINDOW to avoid double-queue.
static void drawGlassForFullscreenWindow() {
    if (!g_pGlobalState)
        return;

    // screenshare/export and snapshots render standalone — no scene behind to sample
    if (g_pHyprRenderer->m_renderData.projectionType != Render::RPT_MONITOR || g_pHyprRenderer->m_bRenderingSnapshot)
        return;

    const auto window = g_pHyprRenderer->m_renderData.currentWindow.lock();
    if (!window)
        return;

    // decorations render normally, draw() already ran
    if (Fullscreen::controller()->getFullscreenModes(window).internal != Fullscreen::FSMODE_FULLSCREEN)
        return;

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor)
        return;

    // solitary frames render no background to sample
    if (monitor->m_solitaryClient.lock() == window)
        return;

    if (auto* deco = glassDecorationFor(window))
        deco->draw(monitor, 1.f); // alpha unused, recomputed in renderPass
}

// ── Layer surface support ────────────────────────────────────────────────────

static bool surfaceInTree(const SP<CWLSurfaceResource>& surface, const SP<CWLSurfaceResource>& root) {
    if (!root)
        return false;
    bool found = false;
    root->breadthfirst([&](SP<CWLSurfaceResource> s, const Vector2D&, void*) {
        if (s == surface)
            found = true;
    }, nullptr);
    return found;
}

using damageSurfaceFn = void (*)(Render::IHyprRenderer*, SP<CWLSurfaceResource>, double, double, double);

// Client commits are the only signal that content below a glassed layer changed
// (e.g. a playing video) — the scene-generation cache only sees window events.
static void hkDamageSurface(Render::IHyprRenderer* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    ((damageSurfaceFn)g_pGlobalState->damageSurfaceHook->m_original)(thisptr, surface, x, y, scale);

    if (!g_pGlobalState || !surface)
        return;

    const auto& config = g_pGlobalState->config;
    if (!config.layersEnabled || !**config.layersEnabled ||
        g_pGlobalState->layerSurfaces.empty())
        return;

    // cheap skip when nothing can want a live resample
    const bool globalLive = config.layersLiveResample && **config.layersLiveResample;
    if (!globalLive && std::ranges::none_of(g_pGlobalState->layerNamespaceLiveResample, [](const auto& kv) { return kv.second; }))
        return;

    const auto wlSurface = Desktop::View::CWLSurface::fromResource(surface);
    if (!wlSurface)
        return;

    // same region Hyprland damaged: commits without damage change nothing behind us
    CRegion damage = wlSurface->computeDamage();
    if (damage.empty())
        return;
    if (scale != 1.0)
        damage.scale(scale);
    damage.translate({x, y});
    const CBox damagedBox = damage.getExtents();

    for (const auto& [_, state] : g_pGlobalState->layerSurfaces) {
        const auto layer = state->getLayerSurface();
        if (!layer || !layer->m_mapped)
            continue;

        if (!state->liveResampleEnabled())
            continue;

        // a layer's own content is not its background
        if (surfaceInTree(surface, layer->wlSurface() ? layer->wlSurface()->resource() : nullptr))
            continue;

        const auto monitor = layer->m_monitor.lock();
        const float monScale = monitor ? monitor->m_scale : 1.0f;
        CBox sampleBox = CBox{layer->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
                              layer->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
        sampleBox.expand(state->samplingPadding(monitor) / monScale);

        if (sampleBox.overlaps(damagedBox))
            state->markBackgroundDirty();
    }
}

// Parse comma-separated config string into a set of trimmed values.
static void parseCommaSeparated(StringConfigPtr configPtr, std::unordered_set<std::string>& out) {
    out.clear();
    const auto raw = readStringConfig(configPtr);
    if (raw.empty()) return;

    std::istringstream stream{std::string(raw)};
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto start = token.find_first_not_of(" \t");
        auto end   = token.find_last_not_of(" \t");
        if (start != std::string::npos)
            out.insert(token.substr(start, end - start + 1));
    }
}

// Parse comma-separated "key<sep>value" pairs. The callback receives (key, valueStr) for each pair.
template <typename Fn>
static void parseKeyValuePairs(StringConfigPtr configPtr, char separator, Fn&& callback) {
    const auto raw = readStringConfig(configPtr);
    if (raw.empty()) return;

    std::istringstream stream{std::string(raw)};
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto sepPos = token.rfind(separator);
        if (sepPos == std::string::npos) continue;

        auto kStart = token.find_first_not_of(" \t");
        auto kEnd   = token.find_last_not_of(" \t", sepPos - 1);
        auto vStart = token.find_first_not_of(" \t", sepPos + 1);
        auto vEnd   = token.find_last_not_of(" \t");

        if (kStart != std::string::npos && kEnd != std::string::npos &&
            vStart != std::string::npos && vEnd != std::string::npos && kStart <= kEnd && vStart <= vEnd) {
            callback(token.substr(kStart, kEnd - kStart + 1),
                     token.substr(vStart, vEnd - vStart + 1));
        }
    }
}

static void parseLayerNamespaceFilters() {
    const auto& config = g_pGlobalState->config;
    parseCommaSeparated(config.layersNamespaces, g_pGlobalState->layerNamespaceFilter);
    parseCommaSeparated(config.layersExcludeNamespaces, g_pGlobalState->layerNamespaceExclude);

    g_pGlobalState->layerNamespacePresets.clear();
    parseKeyValuePairs(config.layersNamespacePresets, ':', [&](const std::string& ns, const std::string& preset) {
        g_pGlobalState->layerNamespacePresets.emplace(ns, preset);
    });

    g_pGlobalState->layerNamespaceMaskThresholds.clear();
    parseKeyValuePairs(config.layersNamespaceMaskThresholds, '=', [&](const std::string& ns, const std::string& val) {
        try { g_pGlobalState->layerNamespaceMaskThresholds.emplace(ns, std::stof(val)); } catch (...) {}
    });

    g_pGlobalState->layerNamespaceLiveResample.clear();
    parseKeyValuePairs(config.layersNamespaceLiveResample, '=', [&](const std::string& ns, const std::string& val) {
        if (val == "1" || val == "true" || val == "on" || val == "yes")
            g_pGlobalState->layerNamespaceLiveResample[ns] = true;
        else if (val == "0" || val == "false" || val == "off" || val == "no")
            g_pGlobalState->layerNamespaceLiveResample[ns] = false;
    });

    g_pGlobalState->layerNamespaceMaskModes.clear();
    parseKeyValuePairs(config.layersNamespaceMaskModes, '=', [&](const std::string& ns, const std::string& val) {
        if (auto mode = parseLayerMaskMode(val))
            g_pGlobalState->layerNamespaceMaskModes[ns] = *mode;
    });
}

static bool shouldGlassLayer(PHLLS layerSurface) {
    if (!layerSurface)
        return false;

    const auto& ns = layerSurface->m_namespace;

    // Exclusion takes priority
    if (g_pGlobalState->layerNamespaceExclude.contains(ns))
        return false;

    const auto& include = g_pGlobalState->layerNamespaceFilter;
    if (include.empty())
        return true;

    return include.contains(ns);
}

// Makes shouldBlur(PHLLS) return false for one renderLayer() call by mutating the
// surface state it reads; restored before any deferred pass element runs.
struct SLayerBlurSuppression {
    SP<Desktop::View::CWLSurface> surface;
    bool                          mutatedRegion    = false;
    bool                          mutatedHasEffect = false;
    CRegion                       savedRegion;

    explicit SLayerBlurSuppression(SP<Desktop::View::CWLSurface> wlSurface) : surface(std::move(wlSurface)) {
        if (!surface)
            return;
        if (surface->m_hasBackgroundEffect) {
            savedRegion = surface->m_blurRegion;
            surface->m_blurRegion.clear();
            mutatedRegion = true;
        } else {
            surface->m_hasBackgroundEffect = true;
            mutatedHasEffect               = true;
        }
    }

    ~SLayerBlurSuppression() {
        if (!surface)
            return;
        if (mutatedRegion)
            surface->m_blurRegion = savedRegion;
        if (mutatedHasEffect)
            surface->m_hasBackgroundEffect = false;
    }
};

using renderLayerFn = void (*)(Render::IHyprRenderer*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);

static void hkRenderLayer(Render::IHyprRenderer* thisptr, PHLLS layerSurface, PHLMONITOR monitor,
                           const Time::steady_tp& now, bool popups, bool lockscreen) {
    const auto& config = g_pGlobalState->config;

    // Never reuse a desktop backdrop above the session lock. Also honour native
    // visibility checks before queuing either half of the redirected pass.
    if (lockscreen || g_pSessionLockManager->isSessionLocked()) {
        if (layerSurface)
            g_pGlobalState->layerSurfaces.erase(layerSurface.get());
        ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
        return;
    }

    // Hyprland renders closing layers from snapshots. Do not inject the glass
    // pipeline while that snapshot is being captured: the snapshot framebuffer
    // starts transparent/black, so sampling it as a background can bake a black
    // rectangle into the fade-out snapshot.
    if (!layerSurface || !layerSurface->visible() || g_pHyprRenderer->m_bRenderingSnapshot) {
        ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
        return;
    }

    // Prune dead layer surfaces whose weak_ptr has expired (layer was destroyed
    // but never got a replacement at the same raw pointer address)
    std::erase_if(g_pGlobalState->layerSurfaces, [](const auto& pair) {
        return !pair.second->getLayerSurface();
    });

    // Only inject glass on the main surface pass, not popups
    if (!popups && config.layersEnabled && **config.layersEnabled && shouldGlassLayer(layerSurface)) {
        // Lazy-create per-layer state, replacing stale entries whose weak ref died
        // (can happen when a new CLayerSurface is allocated at the same address)
        auto* rawPtr = layerSurface.get();
        auto& layerStates = g_pGlobalState->layerSurfaces;
        auto it = layerStates.find(rawPtr);
        if (it != layerStates.end() && !it->second->getLayerSurface()) {
            it->second = std::make_shared<CGlassLayerSurface>(layerSurface);
        } else if (it == layerStates.end()) {
            it = layerStates.emplace(rawPtr, std::make_shared<CGlassLayerSurface>(layerSurface)).first;
        }

        if (!layerSurface->m_mapped) {
            ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
            return;
        }

        float alpha = layerSurface->alpha().getTotal();
        if (alpha < 0.001f) {
            ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
            return;
        }

        // nothing to glass this frame (region mode without a bound region, or an
        // explicit empty region): render as Hyprland normally would
        const auto maskSource = it->second->resolveMaskSource();
        if (maskSource == CGlassLayerSurface::EMaskSource::NONE) {
            ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
            return;
        }

        // Pre-surface: sample+blur background, redirect currentFB → temp FBO
        CGlassLayerPassElement::SGlassLayerPassData preData{it->second, alpha};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassLayerPassElement>(preData));

        const bool manageBlur = config.layersManageBlur && **config.layersManageBlur;
        std::optional<SLayerBlurSuppression> blurSuppression;
        if (manageBlur) {
            if (auto wlSurface = layerSurface->wlSurface())
                blurSuppression.emplace(wlSurface);
        }

        // Original renderLayer: surface renders into the redirected temp FBO.
        // The suppression must wrap only this call: shouldBlur() runs inside it,
        // and the composite element must later see the real protocol state.
        ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
        blurSuppression.reset();

        // Post-surface: restore currentFB, apply glass masked by temp FBO alpha, blit surface
        CGlassLayerCompositeElement::SGlassLayerCompositeData postData{it->second, alpha, maskSource};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassLayerCompositeElement>(postData));

        it->second->damageIfMoved();
        return;
    }

    // Call the original renderLayer
    ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
}


APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static void ensureShadowsEnabled() {
    // Hyprland's shadow pass supplies the backdrop used by window glass.
    const CConfigValue<Config::INTEGER> enabled{"decoration:shadow:enabled"};
    if (!*enabled) {
        HyprlandAPI::invokeHyprctlCommand("eval", "hl.config({ decoration = { shadow = { enabled = true } } })");
        if (!*enabled) // Legacy .conf sessions do not implement eval.
            HyprlandAPI::invokeHyprctlCommand("keyword", "decoration:shadow:enabled true");
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    // Internal render hooks require the exact Hyprland build and library ABI.
    if (HASH != CLIENT_HASH)
        throw std::runtime_error(std::format("hyprliquid: rebuild for the running Hyprland (plugin {}, running {})", HASH, CLIENT_HASH));

    g_pGlobalState = std::make_unique<SGlobalState>();

    g_pGlobalState->listeners.push_back(Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onNewWindow(w); }));

    g_pGlobalState->listeners.push_back(Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { onCloseWindow(w); }));

    g_pGlobalState->listeners.push_back(Event::bus()->m_events.layer.closed.listen([&](PHLLS layerSurface) { clearLayerGlassOnClose(layerSurface); }));
    g_pGlobalState->listeners.push_back(g_pSessionLockManager->m_events.lock.listen([] {
        // A layer can survive locking and later be allowed above the lock.
        // Its next desktop pass must acquire a fresh backdrop after unlocking.
        g_pGlobalState->layerSurfaces.clear();
        g_pGlobalState->hoverAngles.clear();
    }));
    g_pGlobalState->listeners.push_back(g_pSessionLockManager->m_events.unlock.listen([] {
        g_pGlobalState->hoverAngles.refresh();
    }));
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.input.mouse.move.listen(
        [](Vector2D position, Event::SCallbackInfo&) { g_pGlobalState->hoverAngles.update(position); }));
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.monitor.layoutChanged.listen(
        [] { g_pGlobalState->hoverAngles.refresh(); }));

    // Z-order / visibility changes invalidate layer glass caches on the affected monitor only.
    // Per-monitor to avoid triggering re-samples on idle monitors (feedback loop).
    auto bumpWindowMonitor = [&](PHLWINDOW w) {
        if (w) if (auto mon = w->m_monitor.lock()) g_pGlobalState->bumpSceneGeneration(mon);
    };
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.window.active.listen(
        [=](PHLWINDOW w, Desktop::eFocusReason) { bumpWindowMonitor(w); }));
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.window.fullscreen.listen(
        [=](PHLWINDOW w) {
            bumpWindowMonitor(w);
            if (auto* deco = glassDecorationFor(w))
                deco->onFullscreenStateChanged();
        }));

    g_pGlobalState->listeners.push_back(Event::bus()->m_events.render.stage.listen(
        [](eRenderStage stage) {
            if (stage == RENDER_PRE_WINDOW)
                drawGlassForFullscreenWindow();
            if (stage == RENDER_BEGIN) {
                g_pGlobalState->hoverAngles.refresh(false);
                g_pGlobalState->renderBuffers.beginFrame(g_pHyprRenderer->m_renderData.pMonitor.lock());
            }
            if (stage == RENDER_POST)
                g_pGlobalState->renderBuffers.endFrame();
        }));
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.monitor.removed.listen(
        [](PHLMONITOR monitor) {
            g_pGlobalState->hoverAngles.remove(monitor->m_id);
            WindowScene::retireMonitor(monitor->m_id);
        }));
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.monitor.destroyMon.listen(
        [](PHLMONITOR monitor) {
            g_pGlobalState->hoverAngles.remove(monitor->m_id);
            WindowScene::retireMonitor(monitor->m_id);
        }));
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.window.moveToWorkspace.listen(
        [=](PHLWINDOW w, PHLWORKSPACE) { bumpWindowMonitor(w); }));
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.workspace.active.listen(
        [&](PHLWORKSPACE ws) {
            if (ws) if (auto mon = ws->m_monitor.lock()) g_pGlobalState->bumpSceneGeneration(mon);
        }));

    // Clear pending presets/layers before config re-parse, commit after
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.config.preReload.listen([&]() {
        clearPendingPresets();
        clearPendingLayers();
    }));

    g_pGlobalState->listeners.push_back(Event::bus()->m_events.config.reloaded.listen([&]() {
        if (!g_pGlobalState)
            return;
        initConfigPointers(PHANDLE, g_pGlobalState->config);
        commitPendingPresets();
        parseLayerNamespaceFilters();
        commitPendingLayers(); // merge Lua layer() calls on top of string config
        validateConfig();
        ensureShadowsEnabled();
        runtimeBaselineChanged(true);
    }));


    registerConfig(PHANDLE);
    initConfigPointers(PHANDLE, g_pGlobalState->config);
    commitPendingPresets();

    registerRuntimeControl();

    ensureShadowsEnabled();

    for (auto& window : Desktop::viewState()->windows()) {
        if (window->isHidden() || !window->m_isMapped)
            continue;
        onNewWindow(window);
    }

    // Hook renderLayer for layer surface glass support
    if (const auto address = rendererSymbol(PHANDLE, "renderLayer", "IHyprRenderer::renderLayer(")) {
        g_pGlobalState->renderLayerHook = HyprlandAPI::createFunctionHook(PHANDLE, address, (void*)hkRenderLayer);
        if (g_pGlobalState->renderLayerHook && !g_pGlobalState->renderLayerHook->hook()) {
            HyprlandAPI::removeFunctionHook(PHANDLE, g_pGlobalState->renderLayerHook);
            g_pGlobalState->renderLayerHook = nullptr;
        }
    }

    if (!g_pGlobalState->renderLayerHook) {
        HyprlandAPI::addNotificationV2(PHANDLE, {
            {"text", std::string("[hyprliquid] Could not hook renderLayer — layer glass disabled")},
            {"time", (uint64_t)5000},
            {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
        });
    }

    // Hook damageSurface for live layer re-render on background content change
    if (const auto address = rendererSymbol(PHANDLE, "damageSurface", "IHyprRenderer::damageSurface(")) {
        g_pGlobalState->damageSurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, address, (void*)hkDamageSurface);
        if (g_pGlobalState->damageSurfaceHook && !g_pGlobalState->damageSurfaceHook->hook()) {
            HyprlandAPI::removeFunctionHook(PHANDLE, g_pGlobalState->damageSurfaceHook);
            g_pGlobalState->damageSurfaceHook = nullptr;
        }
    }

    if (!g_pGlobalState->damageSurfaceHook) {
        HyprlandAPI::addNotificationV2(PHANDLE, {
            {"text", std::string("[hyprliquid] Could not hook damageSurface — live layer re-render disabled")},
            {"time", (uint64_t)5000},
            {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
        });
    }

    if (!WindowScene::initialize())
        HyprlandAPI::addNotificationV2(PHANDLE, {
            {"text", std::string("[hyprliquid] Could not initialize neighbour capture — using native window order")},
            {"time", (uint64_t)5000},
            {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
        });

    // The reloaded listener commits user presets once. A second commit here
    // would replace them with built-ins after the pending map has been cleared.
    HyprlandAPI::reloadConfig();

    return {std::string(PLUGIN_NAME), std::string(PLUGIN_DESCRIPTION), std::string(PLUGIN_AUTHOR), std::string(PLUGIN_VERSION)};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (!g_pGlobalState)
        return;

    unregisterRuntimeControl();

    g_pGlobalState->listeners.clear();
    g_pGlobalState->hoverAngles.clear();

    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassLayerPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassLayerCompositeElement");
    WindowScene::destroy();

    for (auto& decoration : g_pGlobalState->decorations) {
        if (auto* deco = decoration.get())
            HyprlandAPI::removeWindowDecoration(PHANDLE, deco);
    }
    g_pGlobalState->decorations.clear();

    if (g_pGlobalState->renderLayerHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGlobalState->renderLayerHook);
        g_pGlobalState->renderLayerHook = nullptr;
    }

    if (g_pGlobalState->damageSurfaceHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGlobalState->damageSurfaceHook);
        g_pGlobalState->damageSurfaceHook = nullptr;
    }

    g_pGlobalState->layerSurfaces.clear();
    g_pGlobalState->renderBuffers.shutdown();
    g_pGlobalState->shaderManager.destroy();
    g_pGlobalState.reset();
}
