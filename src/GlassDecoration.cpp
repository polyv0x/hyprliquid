#include "GlassDecoration.hpp"
#include "DamageRegion.hpp"
#include "BuiltInPresets.hpp"
#include "GlassPassElement.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"
#include "MaterialInfo.hpp"

#include <algorithm>
#include <bit>
#include <json-c/json.h>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

CGlassDecoration::CGlassDecoration(PHLWINDOW window)
    : IHyprWindowDecoration(window), m_window(window) {
}

CGlassDecoration::~CGlassDecoration() {
    // Hyprland's registry stores raw pointers and does not retire them when
    // a window dies. An allocator can later reuse this address for a native
    // decoration, which its plugin-unload sweep would then remove by mistake.
    if (g_pPluginSystem) if (auto* plugin = g_pPluginSystem->getPluginByHandle(PHANDLE))
        std::erase(plugin->m_registeredDecorations, static_cast<IHyprWindowDecoration*>(this));
    damageEntire();
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
    // Native decorations own the drop shadow for every material family.
    return info;
}

void CGlassDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {}

void CGlassDecoration::draw(PHLMONITOR monitor, float const& alpha) {
    if (!g_pGlobalState)
        return;

    const bool enabled = resolveEnabled();
    updateNoBlurProp(enabled);
    const auto material = resolveMaterial({resolvePresetName(), resolveThemeIsDark(), g_pGlobalState->config,
                                           g_pGlobalState->customPresets, runtimeOverrides()});
    const float previousAngle = m_renderedDisplayAngle;
    const auto owner = m_window.lock();
    if (enabled && material.family == EMaterialFamily::LIQUID && runtimeAnimateDisplayAngle() && owner && owner->sizeAnimation()) {
        const auto& animation = owner->sizeAnimation();
        const auto begun = animation->begun(), goal = animation->goal(), current = animation->value();
        m_displayAngle.update(material.display_angle_degrees, animation->isBeingAnimated(),
                              {begun.x, begun.y}, {goal.x, goal.y}, {current.x, current.y});
    } else {
        m_displayAngle.reset(enabled && material.family == EMaterialFamily::LIQUID ? material.display_angle_degrees : 0.0f);
    }
    m_renderedDisplayAngle = m_displayAngle.value();
    if (enabled && material.family == EMaterialFamily::LIQUID && runtimeAnimateHoverAngle() && owner)
        m_renderedDisplayAngle += g_pGlobalState->hoverAngles.value(owner->m_monitor.lock());
    // Diagnostics must pair the angle with geometry from the same draw. IPC
    // can otherwise observe a newer native animation tick beside an old angle.
    m_renderedAngleBox = WindowGeometry::computeWindowBox(owner, monitor);
    if (previousAngle != m_renderedDisplayAngle) {
        damageEntire();
        if (owner) if (const auto mon = owner->m_monitor.lock())
            g_pGlobalState->bumpSceneGeneration(mon);
    }
    if (!enabled)
        return;

    CGlassPassElement::SGlassPassData data{m_self, alpha, WindowScene::queuedFor(m_window.lock()), WindowScene::buildingBase()};
    if (!WindowScene::skipDecoration(owner))
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

bool CGlassDecoration::usesLiquidGlass() const {
    return resolveEnabled() && resolveMaterialFamily(resolvePresetName(), g_pGlobalState->customPresets) == EMaterialFamily::LIQUID;
}

bool CGlassDecoration::needsNeighborScene() const {
    return materialNeedsNeighborScene(resolveMaterial({resolvePresetName(), resolveThemeIsDark(), g_pGlobalState->config,
                                                       g_pGlobalState->customPresets, runtimeOverrides()}));
}

void CGlassDecoration::appendCacheKey(std::vector<uint64_t>& key) const {
    const auto material = resolveMaterial({resolvePresetName(), resolveThemeIsDark(), g_pGlobalState->config,
                                           g_pGlobalState->customPresets, runtimeOverrides()});
    key.push_back(resolveEnabled());
    key.push_back(static_cast<uint64_t>(material.family));
    key.push_back(material.darkTheme);
#define GLASS_PARAM(name, value, lo, hi) key.push_back(std::bit_cast<uint32_t>(material.name));
#include "MaterialParams.inc"
#undef GLASS_PARAM
    key.push_back(std::bit_cast<uint32_t>(m_renderedDisplayAngle));
}

CGlassDecoration::SRegionalInputCost CGlassDecoration::regionalInputCost(PHLMONITOR monitor, DRMFormat format) const {
    auto box = WindowGeometry::computeWindowBox(m_window.lock(), monitor);
    if (!box) return {CRenderBuffers::RETAINED_BYTES + 1, CRenderBuffers::RETAINED_PAIRS + 1};
    box->transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)),
                   monitor->m_transformedSize.x, monitor->m_transformedSize.y);
    const auto material = resolveMaterial({resolvePresetName(), resolveThemeIsDark(), g_pGlobalState->config,
                                          g_pGlobalState->customPresets, runtimeOverrides()});
    const auto geometry = RenderSafety::sampleGeometry(box->x, box->y, box->w, box->h,
        materialPadding(material, monitor->m_scale * material.effect_scale));
    if (!geometry) return {CRenderBuffers::RETAINED_BYTES + 1, CRenderBuffers::RETAINED_PAIRS + 1};
    const auto full = CRenderBuffers::bytes(geometry->width, geometry->height, format);
    const auto half = CRenderBuffers::bytes((geometry->width + 1) / 2, (geometry->height + 1) / 2, format);
    const bool liquid = material.family == EMaterialFamily::LIQUID;
    // Retained input, half-size blur/material, and Liquid's live destination.
    // Reserve a clean input too when reduced neighbor contribution may need it.
    const bool clean = material.neighbor_contribution < 1.0f;
    return {full * (liquid ? 2 : 1) + half * (1 + int(liquid)) * (clean ? 2 : 1),
            size_t(1 + int(liquid) + int(clean))};
}

CRegion CGlassDecoration::affectedDamage(PHLMONITOR monitor, const CRegion& input) const {
    const auto box = WindowGeometry::computeWindowBox(m_window.lock(), monitor);
    if (!box) return {};
    const auto material = resolveMaterial({resolvePresetName(), resolveThemeIsDark(), g_pGlobalState->config,
                                          g_pGlobalState->customPresets, runtimeOverrides()});
    const float scale = monitor->m_scale * material.effect_scale;
    auto output = *box;
    const int padding = materialPadding(material, scale);
    const auto geometry = RenderSafety::sampleGeometry(box->x, box->y, box->w, box->h, padding);
    if (!geometry) return CRegion(output);
    double rays = std::max(std::abs(material.outer_refraction_amount), std::abs(material.inner_refraction_amount)) + material.aberration_amount;
    if (material.edge_bleed_opacity > 0) rays = std::max(rays, double(material.edge_bleed_amount));
    const bool liquid = material.family == EMaterialFamily::LIQUID;
    if (liquid) {
        const auto effective = effectiveLiquidMaterial(material);
        rays = std::max(std::abs(effective.refraction.innerAmount), std::abs(effective.refraction.outerAmount));
    }
    const double support = DamageRegion::blurSupport(geometry->width, geometry->height, materialBlurSigma(material) * scale, liquid);
    const double samplingScale = std::max((geometry->width - 2.0 * padding) / box->w, (geometry->height - 2.0 * padding) / box->h);
    return DamageRegion::bounded(input.copy().expand(std::ceil(rays * scale * samplingScale + support + 4)), output);
}

void CGlassDecoration::renderPass(PHLMONITOR monitor, const float& alpha,
                                 const std::shared_ptr<SWindowScene>& scene, bool sceneBase) {
    bool completed = false;
    Hyprutils::Utils::CScopeGuard result([&] {
        if (scene && (scene->local || scene->cacheBase || scene->cacheTiledBase || scene->cacheInputs || scene->independentWindow || scene->regional) && !completed) scene->failed = true;
    });
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

    auto material = resolveMaterial(ctx);
    material.display_angle_degrees = m_renderedDisplayAngle;
    const bool liquid = material.family == EMaterialFamily::LIQUID;
    const float effectScale = monitor->m_scale * material.effect_scale;
    const float blurSigma = materialBlurSigma(material);
    float monitorScale  = monitor->m_scale;

    // Hyprland renders internal-fullscreen windows unrounded (dontRound), we need to
    // match, or the glass would show rounded gaps at the screen corners
    const bool fsUnrounded = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
    float cornerRadius  = fsUnrounded ? 0.0f : window->rounding() * monitorScale;
    float roundingPower = window->roundingPower();

    // Base glass sees only the background; the final pass sees the completed
    // neighbouring panes outside its bounds and the true backdrop inside.
    const auto backdrop = sceneBase && scene ? scene->background : source;
    const auto neighbours = !sceneBase && scene && !scene->local && scene->ready ? scene->image : nullptr;
    std::vector<GlassRenderer::SUnderlayShape> underlayShapes;
    if (scene && (scene->local || neighbours) && (liquid || material.neighbor_contribution < 1.0f)) {
        CBox receivingBox = transformBox;
        // The scene is ordered in native painting order. Only windows already
        // painted beneath this receiver can form its actual backdrop.
        bool beneath = true;
        for (const auto& ref : scene->windows) {
            const auto underneath = ref.lock();
            if (underneath == window) {
                if (!liquid) break;
                beneath = false;
                continue;
            }
            if (!underneath)
                continue;
            auto box = WindowGeometry::computeWindowBox(underneath, monitor);
            if (!box)
                continue;
            box->transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y);
            if (box->intersection(receivingBox).empty())
                continue;
            const bool unrounded = Fullscreen::controller()->getFullscreenModes(underneath).internal == Fullscreen::FSMODE_FULLSCREEN;
            // Adjacent panes always occlude exterior shadows, independently
            // of focus painting order. Overlapping floats retain native order:
            // a top pane's shadow may fall on a true underlying window.
            const bool shadowOccluder = liquid && (!beneath || box->intersection(transformBox).empty());
            underlayShapes.push_back({static_cast<float>(box->x - transformBox.x),
                                     static_cast<float>(box->y - transformBox.y),
                                     static_cast<float>(box->width), static_cast<float>(box->height),
                                     unrounded ? 0.0f : underneath->rounding() * monitor->m_scale,
                                     underneath->roundingPower(), beneath ? 1.0f : 0.0f, shadowOccluder ? 1.0f : 0.0f});
        }
    }
    // Falloff only changes the sharp/blur mix; it needs no reference backdrop.
    // A separate image is needed solely to protect actual underlays when the
    // additional neighbor contribution is reduced.
    const bool separateNeighbors = !(scene && scene->local) && material.neighbor_contribution < 1.0f &&
        std::ranges::any_of(underlayShapes, [](const auto& shape) { return shape[6] > 0.0f; });
    const int padding = materialPadding(material, effectScale);
    const auto geometry = RenderSafety::sampleGeometry(transformBox.x, transformBox.y, transformBox.w, transformBox.h, padding);
    if (!geometry)
        return;
    const auto width = geometry->width, height = geometry->height;
    // The lease begins at execution, never while passes are queued. Both the
    // auxiliary scene and snapshots consume their samples completely here.
    // All blits/draws use the same GL context and are ordered before the next
    // lease can overwrite them; no CPU/GPU wait or cross-context reuse is needed.
    const bool cacheBackground = sceneBase && scene && scene->cacheBackground && !scene->cacheBase &&
        !(scene->cacheTiledBase && !window->m_isFloating);
    const bool independent = scene && scene->independentWindow == window;
    const bool regional = !independent && !sceneBase && scene && scene->regionalInputs && scene->ready && !scene->failed;
    const bool cacheInputs = !sceneBase && scene && ((!liquid && scene->cacheInputs) || independent || regional) && scene->ready && !scene->failed;
    const SWindowScene::SReceiverDamage* receiver = nullptr;
    if (regional) for (const auto& damage : scene->receivers) if (damage.window == window) { receiver = &damage; break; }
    const bool broadUpdate = receiver && DamageRegion::area(receiver->output.copy().intersect(windowBox)) >= windowBox.w * windowBox.h * 0.65;
    const auto cacheOwner = std::format("monitor:{}:window:{}", monitor->m_id, reinterpret_cast<uintptr_t>(window.get()));
    auto samples = cacheBackground ? g_pGlobalState->renderBuffers.acquireBackdrop(width, height, backdrop,
        cacheOwner, scene->backgroundKey, true, liquid) : g_pGlobalState->renderBuffers.acquire(width, height, backdrop,
            cacheInputs ? cacheOwner : std::string{}, cacheInputs ? scene->inputKey : std::vector<uint64_t>{});
    // Damage belongs to this scene update. A pane that missed the preceding
    // update (for example during a popup or fallback) must rebuild its input.
    if (cacheInputs && samples->sceneSerial != scene->previousInputSerial) samples->contentValid = false;
    // Rebuilding most of a pane costs less than region bookkeeping, a second
    // destination crop and finished-image transfers. Keep its exact-size
    // storage, but use the original full pipeline on those frames.
    if (broadUpdate) samples->contentValid = false;
    CRegion inputDamage;
    const auto mapDamage = [&](const CRegion& damage) {
        CRegion mapped;
        damage.forEachRect([&](const auto& r) {
            CBox box(r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1);
            box.transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y);
            box = box.intersection(CBox{{}, source->m_size});
            if (box.empty()) return;
            const double left = std::clamp(box.x <= 0 ? 0 : box.x - geometry->x0, 0.0, double(width));
            const double top = std::clamp(box.y <= 0 ? 0 : box.y - geometry->y0, 0.0, double(height));
            const double right = std::clamp(box.x + box.w >= source->m_size.x ? width : box.x + box.w - geometry->x0, 0.0, double(width));
            const double bottom = std::clamp(box.y + box.h >= source->m_size.y ? height : box.y + box.h - geometry->y0, 0.0, double(height));
            if (right > left && bottom > top) mapped.add(CBox{left, top, right - left, bottom - top});
        });
        return mapped;
    };
    if (cacheInputs && samples->contentValid && (!independent || regional)) {
        if (receiver) inputDamage = mapDamage(receiver->input);
        else for (const auto& change : scene->damage) if (change.window != window) inputDamage.add(mapDamage(change.region));
        inputDamage = DamageRegion::bounded(std::move(inputDamage), CBox(0, 0, width, height));
    }
    const bool retainedInput = samples->contentValid;
    auto captureDamage = inputDamage.copy();
    if (regional && retainedInput) {
        auto valid = mapDamage(g_pHyprRenderer->m_renderData.damage);
        if (!liquid) {
            // Outside the owner Glass copies the complete auxiliary scene,
            // independently of the native target's current damage.
            const CBox ownerBox(transformBox.x - geometry->x0, transformBox.y - geometry->y0, transformBox.w, transformBox.h);
            valid.add(CRegion(CBox(0, 0, width, height)).subtract(CRegion(ownerBox)));
        }
        // Propagation/merging deliberately overestimates changed inputs. The
        // native framebuffer is undefined outside its damage; keep those
        // unchanged retained texels instead of capturing temporary zeroes.
        captureDamage.intersect(valid);
    }
    const bool partialUpdate = retainedInput && !inputDamage.empty();
    CBox cachedBox = transformBox;
    if (regional && liquid) cachedBox.expand(-std::ceil(cornerRadius + 2));
    samples->outputValid &= samples->contentValid && (regional || !partialUpdate);
    if (partialUpdate) samples->contentValid = false;
    Vector2D paddingRatio = samples->paddingRatio;
    // Liquid's auxiliary material input is wallpaper-only, but its exact sharp
    // crop still contains the clients already painted below this window. Keep
    // that crop current while retaining the independent half-size input/blur.
    if (!samples->contentValid || (cacheBackground && liquid)) {
        if (!GlassRenderer::sampleBackground(samples->sharp, liquid ? source : backdrop, transformBox, paddingRatio,
                                          padding, liquid ? nullptr : neighbours, scene ? scene->background : nullptr,
                                          separateNeighbors ? 1.0f : material.neighbor_contribution,
                                          partialUpdate ? &captureDamage : nullptr))
            return;
        ++m_captures;
        if (cacheBackground && liquid && samples->contentValid) ++m_backdropCacheReuses;
        if (partialUpdate) ++m_partialCaptures;
        samples->sharp->setImageDescription(backdrop->imageDescription());
    } else if (cacheInputs) ++m_windowBackdropReuses;
    else ++m_backdropCacheReuses;
    GlassRenderer::SNeighborBackdrop composition{
        scene ? scene->background : nullptr, nullptr, {geometry->x0, geometry->y0}, geometry->owner, underlayShapes,
        nullptr, neighbours};
    if (scene && scene->local) {
        // No neighboring client color enters the half-size reduction or blur.
        // The exact destination crop still supplies native boundary pixels.
        composition.scene = scene->background;
    }
    if (sceneBase && scene) {
        // The auxiliary material sees the immutable background, while the
        // exact destination crop still contains previously painted panes.
        composition.scene = scene->background;
        composition.ownerBounds = {0, 0, 0, 0};
    }
    if (liquid && !samples->contentValid && !GlassRenderer::prepareMaterialBackdrop(samples->sharp, samples->halfSharp, source, &composition,
                                                          separateNeighbors ? 1.0f : material.neighbor_contribution,
                                                          partialUpdate ? &captureDamage : nullptr)) return;
    // A zero-blur consumer passes a null result to the glass shader, but must
    // not discard storage that another matching window can use next.
    auto blurred = samples->blurred;
    if (samples->contentValid) {
        if (!samples->hasBlur) blurred.reset();
    } else {
        // Warm the retained-output scratch path on the first capture too,
        // avoiding a new allocation when the first small update arrives.
        if (cacheInputs && !partialUpdate && !broadUpdate) inputDamage = CRegion(CBox(0, 0, width, height));
        if (!GlassRenderer::blurBackground(samples->sharp, blurred, blurSigma * effectScale, source, nullptr,
                                       liquid ? samples->halfSharp : nullptr, cacheInputs && !broadUpdate ? &inputDamage : nullptr))
            return;
        if (blurred) {
            ++m_blurs;
            if (partialUpdate) ++m_partialBlurs;
            samples->blurred = blurred;
            blurred->setImageDescription(backdrop->imageDescription());
        }
        samples->paddingRatio = paddingRatio;
        samples->hasBlur = bool(blurred);
        samples->contentValid = !samples->cacheOwner.empty();
    }
    if (cacheInputs && !retainedInput) {
        auto valid = g_pHyprRenderer->m_renderData.damage.copy().transform(transform,
            monitor->m_transformedSize.x, monitor->m_transformedSize.y);
        const auto& bounds = geometry->owner;
        auto required = CRegion(CBox(bounds[0], bounds[1], bounds[2] - bounds[0], bounds[3] - bounds[1]))
            .intersect(CBox{{}, source->m_size});
        // Eviction may remove a pane's input after this frame was queued. Its
        // regular draw remains valid, but only a completely reconstructed
        // owner can seed a reusable material input or finished image.
        if (!required.subtract(valid).empty()) samples->contentValid = false;
    }
    // Liquid's antialiased boundary reads this pass's exact native
    // destination, including preserved pixels outside native damage. Keep that
    // live image separate from the retained logical backdrop used by material
    // reduction: refreshing a boundary must not poison the next partial blur.
    auto destination = regional && liquid && !broadUpdate ? g_pGlobalState->renderBuffers.acquire(width, height, source) :
        CRenderBuffers::CLease(g_pGlobalState->renderBuffers, nullptr);
    auto sharp = samples->sharp;
    if (destination) {
        // This unnamed entry may previously have served a full material draw.
        // Its old filters are not used by the live destination and would keep
        // a second set of half-size images pinned for every exact crop size.
        destination->blurred.reset();
        destination->halfSharp.reset();
        Vector2D ignoredPadding;
        if (!GlassRenderer::sampleBackground(destination->sharp, source, transformBox, ignoredPadding, padding)) return;
        sharp = destination->sharp;
        sharp->setImageDescription(source->imageDescription());
        g_pGlobalState->renderBuffers.identify(sharp, "window-destination", std::format("pool:{}", destination->id));
    }
    const auto owner = std::format("pool:{}", samples->id);
    g_pGlobalState->renderBuffers.identify(samples->sharp, cacheBackground ? "window-base-sharp" : "window-sharp", owner);
    g_pGlobalState->renderBuffers.identify(samples->blurred, cacheBackground ? "window-base-blurred" : "window-blurred", owner);
    if (liquid) g_pGlobalState->renderBuffers.identify(samples->halfSharp, "window-material-backdrop", owner);

    // The render alpha Hyprland hands decorations is activeInactive * fade.
    // Glass must follow fades (open/close, fullscreen, workspace moves) but
    // not the active/inactive dimming or opacity rules: those make the surface
    // more translucent — revealing more glass — and shouldn't wash out the
    // glass pane itself. Rebuild the fade-only alpha from its components.
    float glassAlpha = window->alphaTotalWithout(Desktop::View::WINDOW_ALPHA_ACTIVE);
    if (const auto workspace = window->m_workspace; workspace && !window->m_pinned)
        glassAlpha *= workspace->m_alpha->value();

    std::optional<CRegion> visible;
    const auto surface = window->wlSurface();
    const auto resource = window->resource();
    if (!sceneBase && !liquid && resource && surface && window->m_transformers.empty() &&
        resource->m_subsurfaces.empty() && resource->m_current.size == window->getReportedSize() &&
        window->effectiveAlpha() == 1.0f && surface->m_alphaModifier == 1.0f && surface->m_overallOpacity == 1.0f &&
        surface->m_visibleRegion.empty() && !window->positionAnimation()->isBeingAnimated() &&
        !window->sizeAnimation()->isBeingAnimated() && g_pHyprRenderer->m_renderData.projectionType == Render::RPT_MONITOR) {
        auto opaque = resource->m_current.opaque.copy().intersect(CBox{{}, resource->m_current.size});
        if (!opaque.empty() && resource->m_current.size.x > 0 && resource->m_current.size.y > 0) {
            opaque.scale(windowBox.size() / resource->m_current.size).translate(windowBox.pos()).expand(-std::ceil(cornerRadius + 2));
            if (!opaque.empty()) {
                visible = CRegion(windowBox).subtract(opaque);
                ++m_occludedDraws;
            }
        }
    }

    CBox outputBox = regional && liquid ? cachedBox : transformBox;
    const auto outputGeometry = RenderSafety::sampleGeometry(outputBox.x, outputBox.y, outputBox.w, outputBox.h, 0);
    std::array<int, 4> outputBounds{};
    if (outputGeometry) {
        const int x = std::clamp(outputGeometry->x0, 0, int(source->m_size.x));
        const int y = std::clamp(outputGeometry->y0, 0, int(source->m_size.y));
        outputBounds = {x, y, std::clamp(outputGeometry->owner[2], x, int(source->m_size.x)) - x,
                             std::clamp(outputGeometry->owner[3], y, int(source->m_size.y)) - y};
    }
    const bool cacheOutput = cacheInputs && !broadUpdate && samples->contentValid && (!separateNeighbors || regional) && outputBounds[2] > 0 && outputBounds[3] > 0 &&
        transformBox.x == std::floor(transformBox.x) && transformBox.y == std::floor(transformBox.y) &&
        transformBox.w == std::floor(transformBox.w) && transformBox.h == std::floor(transformBox.h) &&
        g_pGlobalState->renderBuffers.canRetainOutput(outputBounds[2], outputBounds[3], source->m_drmFormat, samples->output);
    const bool retainedOutput = cacheOutput && samples->outputValid && samples->outputBounds == outputBounds;
    std::optional<CRegion> outputDamage;
    if (regional && retainedOutput && receiver) outputDamage = receiver->output.copy();
    if (sceneBase && scene && scene->repair) outputDamage = scene->repair->copy();
    if (outputDamage) {
        auto drawDamage = outputDamage->copy();
        if (regional && liquid) {
            auto liveBox = windowBox;
            auto body = windowBox;
            body.expand(-std::ceil(cornerRadius + 2));
            drawDamage.add(CRegion(liveBox).subtract(CRegion(body)));
        }
        if (visible) visible->intersect(drawDamage);
        else visible = std::move(drawDamage);
    }
    if (retainedOutput && (!regional || (!liquid && outputDamage && outputDamage->empty()))) {
        GlassRenderer::transferOutput(source, samples->output, outputBounds, true);
        ++m_materialCacheReuses;
        samples->sceneSerial = scene->inputSerial;
        completed = true;
        return;
    }
    auto draw = [&](const GlassRenderer::SNeighborBackdrop* neighbors = nullptr) {
        // Outside this frame's damage the destination need not contain a
        // complete backdrop. Seed the whole retained output from the validated
        // native input before blending, including transparent rounded corners.
        std::optional<CRegion> nativeDamage;
        if (regional && outputDamage) nativeDamage = outputDamage->copy().transform(transform,
            monitor->m_transformedSize.x, monitor->m_transformedSize.y);
        if (cacheOutput)
            GlassRenderer::transferOutput(source, samples->sharp, outputBounds, true, nativeDamage ? &*nativeDamage : nullptr,
                                          std::array{geometry->x0, geometry->y0});
        // A scene rebuild can finish on an opaque client with blending off.
        // Transparent corner fragments must leave the real destination intact
        // before that completed image can be retained across future frames.
        const bool blend = cacheOutput && glIsEnabled(GL_BLEND);
        if (cacheOutput) g_pHyprOpenGL->setCapStatus(GL_BLEND, true);
        // Hyprland owns the drop shadow in both families. The material stays
        // inside its window, including when rendered into a closing snapshot.
        GlassRenderer::applyGlassEffect(sharp, blurred, source,
                                         windowBox, transformBox, glassAlpha,
                                         cornerRadius, roundingPower, paddingRatio, material, nullptr, neighbors,
                                         liquid ? samples->halfSharp : nullptr, false, visible ? &*visible : nullptr);
        if (cacheOutput) g_pHyprOpenGL->setCapStatus(GL_BLEND, blend);
        ++m_materialDraws;
        if (outputDamage) ++m_partialMaterials;
        if (cacheOutput) {
            // A pass can hold a read-only image queued before this rebuild.
            // Replace storage instead of overwriting that retained snapshot.
            if (samples->output && samples->output.strongRef() > 1) samples->output.reset();
            if (g_pGlobalState->renderBuffers.prepare(samples->output, outputBounds[2], outputBounds[3], source->m_drmFormat,
                                                     "window-material", cacheOwner)) {
                samples->output->setImageDescription(source->imageDescription());
                // Retain the result only after native blending. Glass output
                // stays inside its owner (whose crop is the true backdrop);
                // Liquid's sharp crop also retains the true exterior backdrop.
                GlassRenderer::transferOutput(source, samples->output, outputBounds, false, nativeDamage ? &*nativeDamage : nullptr);
                if (nativeDamage) GlassRenderer::transferOutput(source, samples->output, outputBounds, true);
                samples->outputBounds = outputBounds;
                samples->outputValid = true;
                if (independent && !regional) {
                    // Only the finished native pixels are needed while this
                    // backdrop is unchanged. Keeping another full-size input
                    // pair evicts the live video/foreground pairs on 4K stacks.
                    g_pGlobalState->renderBuffers.release(samples->sharp);
                    g_pGlobalState->renderBuffers.release(samples->blurred);
                    g_pGlobalState->renderBuffers.release(samples->halfSharp);
                    samples->contentValid = false;
                }
            }
        }
        if (cacheInputs) samples->sceneSerial = scene->inputSerial;
        completed = true;
    };
    if (separateNeighbors) {
        auto neighbors = composition;
        if (!liquid && blurSigma <= 0.0f) {
            // Clean sharp sampling is reconstructed from existing textures.
            // With no blur this path needs no additional framebuffer at all.
            draw(&neighbors);
            return;
        }
        // Liquid still needs its clean half-sharp input at zero blur. Legacy
        // materials only need the clean blur, reconstructed on first reduction.
        auto clean = g_pGlobalState->renderBuffers.acquire(regional ? width : (width + 1) / 2, regional ? height : (height + 1) / 2, backdrop,
            regional ? cacheOwner + ":clean" : std::string{}, regional ? scene->inputKey : std::vector<uint64_t>{});
        if (regional && (!retainedInput || clean->sceneSerial != scene->previousInputSerial)) clean->contentValid = false;
        const bool repairClean = clean->contentValid && partialUpdate;
        auto cleanBlurred = clean->sharp;
        if (!clean->contentValid || partialUpdate) {
            const CRegion full(CBox(0, 0, width, height));
            const CRegion* damage = regional && !broadUpdate ? (repairClean ? &inputDamage : &full) : nullptr;
            if (liquid && !GlassRenderer::prepareMaterialBackdrop(samples->sharp, clean->halfSharp, source, &composition, 0.0f,
                                                                  repairClean ? &inputDamage : nullptr)) return;
            if (!GlassRenderer::blurBackground(samples->sharp, cleanBlurred, blurSigma * effectScale, source,
                                               liquid ? nullptr : &neighbors, liquid ? clean->halfSharp : nullptr, damage)) return;
            clean->sharp = cleanBlurred;
            clean->hasBlur = bool(cleanBlurred);
            clean->contentValid = regional && samples->contentValid;
        } else if (!clean->hasBlur) cleanBlurred.reset();
        if (liquid) {
            neighbors.halfSharp = clean->halfSharp;
            g_pGlobalState->renderBuffers.identify(clean->halfSharp, "window-clean-material-backdrop", std::format("pool:{}", clean->id));
        }
        if (cleanBlurred) {
            cleanBlurred->setImageDescription(backdrop->imageDescription());
            g_pGlobalState->renderBuffers.identify(cleanBlurred, "window-clean-blurred", std::format("pool:{}", clean->id));
            neighbors.blurred = cleanBlurred;
        }
        if (regional) clean->sceneSerial = scene->inputSerial;
        draw(&neighbors);
        return;
    }
    draw(liquid && !underlayShapes.empty() ? &composition : nullptr);
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
    addMaterialInfo(result, material);
    json_object_object_add(result, "owns_shadow", json_object_new_boolean(false));
    json_object_object_add(result, "material_draws", json_object_new_uint64(m_materialDraws));
    json_object_object_add(result, "captures", json_object_new_uint64(m_captures));
    json_object_object_add(result, "blurs", json_object_new_uint64(m_blurs));
    json_object_object_add(result, "local_cache_reuses", json_object_new_uint64(m_cacheReuses));
    json_object_object_add(result, "backdrop_cache_reuses", json_object_new_uint64(m_backdropCacheReuses));
    json_object_object_add(result, "scene_base_cache_reuses", json_object_new_uint64(m_sceneBaseCacheReuses));
    json_object_object_add(result, "window_backdrop_reuses", json_object_new_uint64(m_windowBackdropReuses));
    json_object_object_add(result, "partial_captures", json_object_new_uint64(m_partialCaptures));
    json_object_object_add(result, "partial_blurs", json_object_new_uint64(m_partialBlurs));
    json_object_object_add(result, "occluded_draws", json_object_new_uint64(m_occludedDraws));
    json_object_object_add(result, "material_cache_reuses", json_object_new_uint64(m_materialCacheReuses));
    json_object_object_add(result, "partial_materials", json_object_new_uint64(m_partialMaterials));
    json_object_object_add(result, "animate_display_angle", json_object_new_boolean(runtimeAnimateDisplayAngle()));
    json_object_object_add(result, "animate_hover_angle", json_object_new_boolean(runtimeAnimateHoverAngle()));
    json_object_object_add(result, "rendered_display_angle_degrees", json_object_new_double(m_renderedDisplayAngle));
    if (m_renderedAngleBox) {
        auto* box = json_object_new_array();
        for (double value : {m_renderedAngleBox->x, m_renderedAngleBox->y, m_renderedAngleBox->w, m_renderedAngleBox->h})
            json_object_array_add(box, json_object_new_double(value));
        json_object_object_add(result, "rendered_angle_box", box);
    }
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
    if (!g_pHyprRenderer) return;
    if (m_lastDamageBox)
        g_pHyprRenderer->damageBox(*m_lastDamageBox);
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
    if (!std::isfinite(scale) || scale <= 0.0f)
        return;
    surfaceBox.expand(samplingPadding(monitor) / scale);

    if (RenderSafety::sampleGeometry(surfaceBox.x, surfaceBox.y, surfaceBox.w, surfaceBox.h, 0)) {
        g_pHyprRenderer->damageBox(surfaceBox);
        m_lastDamageBox = surfaceBox;
    }
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
