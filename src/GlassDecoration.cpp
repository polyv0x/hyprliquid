#include "GlassDecoration.hpp"
#include "BuiltInPresets.hpp"
#include "GlassPassElement.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

#include <algorithm>
#include <json-c/json.h>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>

CGlassDecoration::CGlassDecoration(PHLWINDOW window)
    : IHyprWindowDecoration(window), m_window(window) {
}

CGlassDecoration::~CGlassDecoration() {
    withdrawNoBlur();
}

// Glass replaces Hyprland's blur for this window. Mark glassed windows with
// the noblur property so Hyprland composites their translucency against the
// live framebuffer (which contains the glass) instead of its pre-frame cached
// blur snapshot, which is captured before plugin decorations render (#46).
void CGlassDecoration::updateNoBlurProp(bool glassEnabled) {
    const auto& config = g_pGlobalState->config;
    const bool manage = config.manageWindowBlur && **config.manageWindowBlur;

    if (!manage || !glassEnabled) {
        withdrawNoBlur();
        return;
    }

    if (m_noBlurApplied)
        return;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            window->m_ruleApplicator->noBlur().set(true, Desktop::Types::PRIORITY_SET_PROP);
            m_noBlurApplied = true;
            damageEntire();
        }
    } catch (...) {}
}

void CGlassDecoration::withdrawNoBlur() {
    if (!m_noBlurApplied)
        return;
    m_noBlurApplied = false;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            window->m_ruleApplicator->noBlur().unset(Desktop::Types::PRIORITY_SET_PROP);
            damageEntire();
        }
    } catch (...) {}
}

// Fullscreen toggles re-apply window rules, which can drop the noblur prop
// while m_noBlurApplied still claims it's held.
void CGlassDecoration::onFullscreenStateChanged() {
    m_noBlurApplied = false;
    damageEntire();
}

bool CGlassDecoration::resolveEnabled() const {
    const bool globalEnabled = runtimeEnabled();

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const auto& tags = window->m_ruleApplicator->m_tagKeeper;
            // isTagged() already matches dynamic tags ("tag*") — no stripping needed here.
            // Disabled tag wins over enabled tag if both are present.
            if (tags.isTagged(std::string(TAG_DISABLED)))
                return false;
            if (tags.isTagged(std::string(TAG_ENABLED)))
                return true;
        }
    } catch (...) {}

    return globalEnabled;
}

bool CGlassDecoration::resolveThemeIsDark() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const std::string lightTag = std::string(TAG_THEME_PREFIX) + "light";
            const std::string darkTag  = std::string(TAG_THEME_PREFIX) + "dark";
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(lightTag))
                return false;
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(darkTag))
                return true;
        }

        const auto& config = g_pGlobalState->config;
        if (runtimeOverrides())
            return runtimeThemeIsDark();
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

std::string CGlassDecoration::resolvePresetName() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags()) {
                if (tag.starts_with(TAG_PRESET_PREFIX))
                    return stripDynamicTagMarker(tag.substr(TAG_PRESET_PREFIX.size()));
            }
        }

        return runtimeBasePreset();
    } catch (...) {}

    return "default";
}

const SPresetValues* CGlassDecoration::runtimeOverrides() const {
    const auto window = m_window.lock();
    if (window && window->m_ruleApplicator) {
        for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags())
            if (tag.starts_with(TAG_PRESET_PREFIX))
                return nullptr;
    }
    return &g_pGlobalState->runtimeControl.profile.overrides;
}

SDecorationPositioningInfo CGlassDecoration::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.priority       = 10000;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.desiredExtents = {{0, 0}, {0, 0}};
    return info;
}

void CGlassDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {}

void CGlassDecoration::draw(PHLMONITOR monitor, float const& alpha) {
    if (!g_pGlobalState)
        return;

    const bool enabled = resolveEnabled();
    updateNoBlurProp(enabled);
    if (!enabled)
        return;

    CGlassPassElement::SGlassPassData data{m_self, alpha, WindowScene::queuedFor(m_window.lock()), WindowScene::buildingBase()};
    g_pHyprRenderer->addPassElement(makeUnique<CGlassPassElement>(data));

    if (data.sceneBase)
        return;

    const auto window = m_window.lock();
    if (window) {
        const auto workspace = window->m_workspace;

        const bool wsAnimating = workspace && !window->m_pinned && workspace->m_renderOffset->isBeingAnimated();
        if (wsAnimating)
            damageEntire();

        const auto currentPosition = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto currentSize = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const bool moved = currentPosition != m_lastPosition || currentSize != m_lastSize;
        if (moved) {
            damageEntire();
            m_lastPosition = currentPosition;
            m_lastSize = currentSize;
        }

        // Bump layer cache only for actual scene changes (window moved/animating),
        // NOT from damageEntire() which fires in the damage system feedback path.
        if (moved || wsAnimating) {
            if (auto mon = window->m_monitor.lock())
                g_pGlobalState->bumpSceneGeneration(mon);
        }
    }
}

PHLWINDOW CGlassDecoration::getOwner() {
    return m_window.lock();
}

void CGlassDecoration::renderPass(PHLMONITOR monitor, const float& alpha,
                                 const std::shared_ptr<SWindowScene>& scene, bool sceneBase) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto window = m_window.lock();
    if (!window)
        return;

    const auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!source)
        return;

    auto optBox = WindowGeometry::computeWindowBox(window, monitor);
    if (!optBox)
        return;

    CBox windowBox    = *optBox;
    CBox transformBox = windowBox;

    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));
    transformBox.transform(transform,
        g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.x,
        g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.y);

    const bool isDark          = resolveThemeIsDark();
    const std::string preset   = resolvePresetName();
    const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets, runtimeOverrides()};

    const auto material = resolveMaterial(ctx);
    const float effectScale = monitor->m_scale * material.effect_scale;
    // Base glass sees only the background; the final pass sees the completed
    // neighbouring panes outside its bounds and the true backdrop inside.
    const auto backdrop = sceneBase && scene ? scene->background : source;
    const auto neighbours = !sceneBase && scene && scene->ready ? scene->image : nullptr;
    const int padding = materialPadding(material, effectScale);
    const int width = std::max(1, static_cast<int>(std::ceil(transformBox.width))) + 2 * padding;
    const int height = std::max(1, static_cast<int>(std::ceil(transformBox.height))) + 2 * padding;
    // The lease begins at execution, never while passes are queued. Both the
    // auxiliary scene and snapshots consume their samples completely here.
    // All blits/draws use the same GL context and are ordered before the next
    // lease can overwrite them; no CPU/GPU wait or cross-context reuse is needed.
    auto samples = g_pGlobalState->renderBuffers.acquire(width, height, backdrop);
    Vector2D paddingRatio;
    if (!GlassRenderer::sampleBackground(samples->sharp, backdrop, transformBox, paddingRatio,
                                          padding, neighbours, scene ? scene->background : nullptr, material.neighbor_contribution))
        return;
    samples->sharp->setImageDescription(backdrop->imageDescription());
    // A zero-blur consumer passes a null result to the glass shader, but must
    // not discard storage that another matching window can use next.
    auto blurred = samples->blurred;
    GlassRenderer::blurBackground(samples->sharp, blurred, material.blur_sigma * effectScale, source);
    if (blurred) {
        samples->blurred = blurred;
        blurred->setImageDescription(backdrop->imageDescription());
    }
    const auto owner = std::format("pool:{}", samples->id);
    g_pGlobalState->renderBuffers.identify(samples->sharp, "window-sharp", owner);
    g_pGlobalState->renderBuffers.identify(samples->blurred, "window-blurred", owner);

    float monitorScale  = monitor->m_scale;

    // Hyprland renders internal-fullscreen windows unrounded (dontRound), we need to
    // match, or the glass would show rounded gaps at the screen corners
    const bool fsUnrounded = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
    float cornerRadius  = fsUnrounded ? 0.0f : window->rounding() * monitorScale;
    float roundingPower = window->roundingPower();

    // The render alpha Hyprland hands decorations is activeInactive * fade.
    // Glass must follow fades (open/close, fullscreen, workspace moves) but
    // not the active/inactive dimming or opacity rules: those make the surface
    // more translucent — revealing more glass — and shouldn't wash out the
    // glass pane itself. Rebuild the fade-only alpha from its components.
    float glassAlpha = window->alphaTotalWithout(Desktop::View::WINDOW_ALPHA_ACTIVE);
    if (const auto workspace = window->m_workspace; workspace && !window->m_pinned)
        glassAlpha *= workspace->m_alpha->value();

    auto draw = [&](SP<Render::IFramebuffer> cleanSharp = nullptr, SP<Render::IFramebuffer> cleanBlurred = nullptr) {
        GlassRenderer::applyGlassEffect(samples->sharp, blurred, source,
                                         windowBox, transformBox, glassAlpha,
                                         cornerRadius, roundingPower, paddingRatio, ctx, nullptr,
                                         cleanSharp, cleanBlurred);
    };
    if (neighbours && material.neighbor_contribution > 0.0f && material.neighbor_falloff > 0.0f) {
        // Keep a second pooled sample only while drawing this window. Its
        // clean backdrop follows identical optics, so inward falloff removes
        // neighbour content without fading the receiving pane's main frost.
        auto clean = g_pGlobalState->renderBuffers.acquire(width, height, backdrop);
        Vector2D cleanPadding;
        if (GlassRenderer::sampleBackground(clean->sharp, backdrop, transformBox, cleanPadding,
                                            padding, neighbours, scene->background, 0.0f)) {
            clean->sharp->setImageDescription(backdrop->imageDescription());
            auto cleanBlurred = clean->blurred;
            GlassRenderer::blurBackground(clean->sharp, cleanBlurred, material.blur_sigma * effectScale, source);
            if (cleanBlurred) {
                clean->blurred = cleanBlurred;
                cleanBlurred->setImageDescription(backdrop->imageDescription());
            }
            const auto cleanOwner = std::format("pool:{}", clean->id);
            g_pGlobalState->renderBuffers.identify(clean->sharp, "window-clean-sharp", cleanOwner);
            g_pGlobalState->renderBuffers.identify(clean->blurred, "window-clean-blurred", cleanOwner);
            draw(clean->sharp, cleanBlurred);
            return;
        }
    }
    draw();
}

json_object* CGlassDecoration::diagnostics() const {
    auto* result = json_object_new_object();
    const auto window = m_window.lock();
    const auto preset = resolvePresetName();
    const auto material = resolveMaterial({preset, resolveThemeIsDark(), g_pGlobalState->config,
                                          g_pGlobalState->customPresets, runtimeOverrides()});
    json_object_object_add(result, "owner", json_object_new_string(std::format("window:{:p}", static_cast<void*>(window.get())).c_str()));
    json_object_object_add(result, "preset", json_object_new_string(preset.c_str()));
    json_object_object_add(result, "enabled", json_object_new_boolean(resolveEnabled()));
    json_object_object_add(result, "dark", json_object_new_boolean(resolveThemeIsDark()));
    auto* values = json_object_new_object();
#define GLASS_PARAM(name, value, lo, hi) json_object_object_add(values, #name, json_object_new_double(material.name));
#include "MaterialParams.inc"
#undef GLASS_PARAM
    json_object_object_add(result, "material", values);
    if (window) if (const auto monitor = window->m_monitor.lock()) {
        json_object_object_add(result, "monitor_id", json_object_new_int64(monitor->m_id));
        if (const auto box = WindowGeometry::computeWindowBox(window, monitor)) {
            auto* geometry = json_object_new_array();
            for (double v : {box->x, box->y, box->width, box->height})
                json_object_array_add(geometry, json_object_new_double(v));
            json_object_object_add(result, "pixel_box", geometry);
        }
        json_object_object_add(result, "padding", json_object_new_int(materialPadding(material, monitor->m_scale * material.effect_scale)));
    }
    return result;
}

eDecorationType CGlassDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CGlassDecoration::updateWindow(PHLWINDOW window) {
    damageEntire();
}

void CGlassDecoration::damageEntire() {
    const auto window = m_window.lock();
    if (!window)
        return;

    const auto workspace = window->m_workspace;
    auto surfaceBox = window->getWindowMainSurfaceBox();

    if (workspace && workspace->m_renderOffset->isBeingAnimated() && !window->m_pinned)
        surfaceBox.translate(workspace->m_renderOffset->value());
    surfaceBox.translate(window->m_floatingOffset);

    // Expand damage by our sampling padding so the render pass re-renders
    // background content (wallpaper, other windows) in the padded margin.
    // Without this, the scissored render pass leaves stale previous-frame
    // content in the padding area, causing noise artifacts.
    // surfaceBox is in logical coords; convert pixel padding to logical.
    const auto monitor = window->m_monitor.lock();
    const float scale = monitor ? monitor->m_scale : 1.0f;
    surfaceBox.expand(samplingPadding(monitor) / scale);

    g_pHyprRenderer->damageBox(surfaceBox);
}

eDecorationLayer CGlassDecoration::getDecorationLayer() {
    return DECORATION_LAYER_BOTTOM;
}

uint64_t CGlassDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CGlassDecoration::getDisplayName() {
    return "Hyprliquid";
}

int CGlassDecoration::samplingPadding(PHLMONITOR monitor) const {
    if (!g_pGlobalState) return GlassRenderer::SAMPLE_PADDING_PX;
    const auto preset = resolvePresetName();
    const SResolveContext ctx{preset, resolveThemeIsDark(), g_pGlobalState->config, g_pGlobalState->customPresets, runtimeOverrides()};
    const auto material = resolveMaterial(ctx);
    return materialPadding(material, (monitor ? monitor->m_scale : 1.0f) * material.effect_scale);
}
