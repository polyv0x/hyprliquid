#include "GlassLayerCompositeElement.hpp"
#include "GlassLayerSurface.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "LayerGeometry.hpp"

#include <cmath>

CGlassLayerCompositeElement::CGlassLayerCompositeElement(const SGlassLayerCompositeData& data)
    : m_data(data) {}

std::vector<UP<IPassElement>> CGlassLayerCompositeElement::draw() {
    // Restoration is required even if the client disappeared after redirection.
    if (m_data.layerState)
        m_data.layerState->compositeAndRestore(g_pHyprRenderer->m_renderData.pMonitor.lock(), m_data.alpha, m_data.maskSource);

    return {};
}

std::optional<CBox> CGlassLayerCompositeElement::boundingBox() {
    if (!m_data.layerState)
        return std::nullopt;

    auto layerSurface = m_data.layerState->getLayerSurface();
    if (!layerSurface)
        return std::nullopt;

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    auto box = LayerGeometry::computeLayerBox(layerSurface, monitor);
    if (!box)
        return std::nullopt;

    const float scale = monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
    box->scale(1.0 / scale).expand(m_data.layerState->samplingPadding(monitor) / scale).noNegativeSize().round();
    if (!RenderSafety::sampleGeometry(box->x, box->y, box->w, box->h, 0))
        return std::nullopt;

    return box;
}

bool CGlassLayerCompositeElement::needsLiveBlur() {
    return false;
}

bool CGlassLayerCompositeElement::needsPrecomputeBlur() {
    return false;
}
