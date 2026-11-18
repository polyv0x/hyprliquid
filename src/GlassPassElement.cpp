#include "GlassPassElement.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

CGlassPassElement::CGlassPassElement(const SGlassPassData& data)
    : m_data(data) {
#if HYPRLIQUID_WINDOW_PRESENTATION
    if (data.decoration) if (const auto window = data.decoration->getOwner())
        m_presentation = HyprlandCompat::windowPresentation(window);
#endif
    if (!data.decoration || data.sceneBase || !data.scene || data.scene->failed)
        return;
    const auto window = data.decoration->getOwner();
    const auto monitor = HyprlandCompat::renderData().pMonitor.lock();
    if (!window || !monitor) return;
    const bool independent = data.scene->independentWindow == window;
    if ((data.scene->regional && !independent) || (!data.scene->cacheInputs && !independent)) return;
    m_cachedOutput = g_pGlobalState->renderBuffers.retainOutput(
        std::format("monitor:{}:window:{}", monitor->m_id, reinterpret_cast<uintptr_t>(window.get())),
        data.scene->inputKey, data.scene->previousInputSerial, HyprlandCompat::renderData().currentFB);
    if (!m_cachedOutput) {
        // Live-blur damage expansion follows the native blur radius, not our
        // complete input crop. A cache miss (including a subsurface layout
        // change or eviction) must repaint that crop before retaining it.
        if (auto box = WindowGeometry::computeWindowBox(window, monitor)) {
            box->expand(data.decoration->samplingPadding(monitor));
            HyprlandCompat::renderData().damage.add(*box);
            HyprlandCompat::renderData().finalDamage.add(*box);
        }
        return;
    }
    // A foreground commit does not alter its own glass. Neighbor changes take
    // the regular partial-input path. Pin its previous inputs through the
    // auxiliary draw, then release immediately before acquiring them again.
    if (!independent) for (const auto& change : data.scene->damage) if (change.window != window) {
        m_pendingInput = std::move(m_cachedOutput);
        break;
    }
}

void CGlassPassElement::discard() {
    // Culling a valid finished image (for example on a bar-only frame) does
    // not invalidate it. Advance its serial so the next small client update
    // can still reuse it instead of rebuilding from a partial framebuffer.
    if (m_cachedOutput) m_cachedOutput->sceneSerial = m_data.scene->inputSerial;
    m_cachedOutput.reset();
    m_pendingInput.reset();
}

std::vector<UP<IPassElement>> CGlassPassElement::draw() {
#if HYPRLIQUID_WINDOW_PRESENTATION
    std::optional<HyprlandCompat::CPresentationScope> presentation;
    if (m_presentation && m_data.decoration)
        presentation.emplace(m_data.decoration->getOwner(), *m_presentation);
#endif
    if (m_cachedOutput && m_data.decoration && m_data.decoration->getOwner()) {
        auto& data = HyprlandCompat::renderData();
        const auto monitor = data.pMonitor.lock();
        const auto transform = HyprlandCompat::targetTransform(monitor);
        CRegion damage;
        data.damage.forEachRect([&](const auto& r) {
            CBox box(r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1);
            box.transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y);
            damage.add(box);
        });
        GlassRenderer::transferOutput(data.currentFB, m_cachedOutput->output, m_cachedOutput->outputBounds, true, &damage);
        m_cachedOutput->sceneSerial = m_data.scene->inputSerial;
        ++m_data.decoration->m_materialCacheReuses;
        ++m_data.decoration->m_windowBackdropReuses;
        m_cachedOutput.reset();
        return {};
    }
    m_pendingInput.reset();
    if (m_data.decoration.valid())
        m_data.decoration->renderPass(HyprlandCompat::renderData().pMonitor.lock(), m_data.alpha,
                                     m_data.scene, m_data.sceneBase);
    m_cachedOutput.reset();
    return {};
}

std::optional<CBox> CGlassPassElement::boundingBox() {
    if (!m_data.decoration.valid())
        return std::nullopt;

    auto window = m_data.decoration->getOwner();
    if (!window)
        return std::nullopt;
#if HYPRLIQUID_WINDOW_PRESENTATION
    std::optional<HyprlandCompat::CPresentationScope> presentation;
    if (m_presentation) presentation.emplace(window, *m_presentation);
#endif

    const auto monitor = HyprlandCompat::renderData().pMonitor.lock();
    auto box = WindowGeometry::computeWindowBox(window, monitor);
    if (!box)
        return std::nullopt;

    if (m_cachedOutput) {
        return box->scale(1.0 / monitor->m_scale);
    }

    // Expand by our sampling padding so the render pass damages the full
    // area we read from. Without this, wallpaper outside the window box
    // but inside our padding isn't re-rendered, leaving stale content.
    const float scale = monitor->m_scale;
    box->scale(1.0 / scale).expand(m_data.decoration->samplingPadding(monitor) / scale);
    if (!RenderSafety::sampleGeometry(box->x, box->y, box->w, box->h, 0))
        return std::nullopt;
    return box;
}

bool CGlassPassElement::needsLiveBlur() {
    // Uncached windows need live blur so the render pass fully re-renders the
    // background behind the glass before we sample it. Without this,
    // partial damage (e.g. typing in a window below) leaves stale pixels
    // in the padded sampling region, causing blinking artifacts.
    // A pinned finished image has no live input dependency and needs only its
    // output damage. Pinning prevents pool eviction after this decision.
    return !(m_data.sceneBase && m_data.scene && m_data.scene->repair) &&
        !m_cachedOutput && m_data.decoration.valid() && m_data.decoration->getOwner();
}

bool CGlassPassElement::needsPrecomputeBlur() {
    return false;
}

bool CGlassPassElement::disableSimplification() {
    return !m_cachedOutput && m_data.decoration.valid() && m_data.decoration->getOwner();
}
