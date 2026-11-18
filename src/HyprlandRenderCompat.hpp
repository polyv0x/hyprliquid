#pragma once

#include "HyprlandCompat.hpp"

#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <utility>

namespace HyprlandCompat {

#if HYPRLIQUID_RENDER_CONTEXT
// Bind the context supplied by Hyprland, including nested/offscreen draws.
// Falling back to the renderer's context is only needed by legacy stage hooks.
inline thread_local Render::CRenderContext* boundContext = nullptr;

class CRenderScope {
  public:
    explicit CRenderScope(Render::CRenderContext& context)
        : m_previous(std::exchange(boundContext, &context)) {}
    ~CRenderScope() { boundContext = m_previous; }
    CRenderScope(const CRenderScope&) = delete;
    CRenderScope& operator=(const CRenderScope&) = delete;

  private:
    Render::CRenderContext* m_previous;
};

inline Render::CRenderContext& context() {
    return boundContext ? *boundContext : g_pHyprRenderer->context();
}
#endif

inline auto& renderData() {
#if HYPRLIQUID_RENDER_CONTEXT
    return context().m_data;
#else
    return g_pHyprRenderer->m_renderData;
#endif
}

// 0.56 draws the scene in physical output orientation. 0.57 normally draws
// in monitor coordinates and rotates only during its final output transfer.
// Follow the active target, including equally sized scratch framebuffers.
inline auto targetTransform(const PHLMONITOR& monitor) {
    return renderData().transformDamage && monitor
        ? Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform))
        : HYPRUTILS_TRANSFORM_NORMAL;
}

inline auto targetTransform() {
    return targetTransform(renderData().pMonitor.lock());
}

inline bool& renderingSnapshot() {
#if HYPRLIQUID_RENDER_CONTEXT
    return context().m_renderingSnapshot;
#else
    return g_pHyprRenderer->m_bRenderingSnapshot;
#endif
}

inline bool& blockSurfaceFeedback() {
#if HYPRLIQUID_RENDER_CONTEXT
    return context().m_blockSurfaceFeedback;
#else
    return g_pHyprRenderer->m_bBlockSurfaceFeedback;
#endif
}

inline auto& renderPass() {
#if HYPRLIQUID_RENDER_CONTEXT
    return context().m_pass;
#else
    return g_pHyprRenderer->m_renderPass;
#endif
}

inline void bindFB(const SP<Render::IFramebuffer>& framebuffer) {
#if HYPRLIQUID_RENDER_CONTEXT
    g_pHyprRenderer->bindFB(context(), framebuffer);
#else
    g_pHyprRenderer->bindFB(framebuffer);
#endif
}

template <typename... Args>
inline void scissor(Args&&... args) {
#if HYPRLIQUID_RENDER_CONTEXT
    Render::GL::g_pHyprOpenGL->scissor(context(), std::forward<Args>(args)...);
#else
    Render::GL::g_pHyprOpenGL->scissor(std::forward<Args>(args)...);
#endif
}

inline auto drawPass(Render::CRenderPass& pass, const CRegion& damage) {
#if HYPRLIQUID_RENDER_CONTEXT
    return pass.render(context(), damage);
#else
    return pass.render(damage);
#endif
}

template <typename... Args>
inline auto projectBoxToTarget(Args&&... args) {
#if HYPRLIQUID_RENDER_CONTEXT
    return g_pHyprRenderer->projectBoxToTarget(context(), std::forward<Args>(args)...);
#else
    return g_pHyprRenderer->projectBoxToTarget(std::forward<Args>(args)...);
#endif
}

inline void addPassElement(UP<IPassElement> element) {
#if HYPRLIQUID_RENDER_CONTEXT
    g_pHyprRenderer->addPassElement(context(), std::move(element));
#else
    g_pHyprRenderer->addPassElement(std::move(element));
#endif
}

inline auto& currentPass() {
#if HYPRLIQUID_RENDER_CONTEXT
    return g_pHyprRenderer->currentPass(context());
#else
    return g_pHyprRenderer->currentPass();
#endif
}

inline auto redirectPass(Render::CRenderPass* pass) {
#if HYPRLIQUID_RENDER_CONTEXT
    return g_pHyprRenderer->redirectPass(context(), pass);
#else
    return g_pHyprRenderer->redirectPass(pass);
#endif
}

#if HYPRLIQUID_RENDER_CONTEXT
// The plugin's pass implementations remain shared. Only Hyprland's virtual
// interface changes; each entry binds and restores the supplied render context.
class CPassElement : public IPassElement {
  public:
    virtual std::vector<UP<IPassElement>> draw() = 0;
    virtual void discard() {}
    virtual bool needsLiveBlur() = 0;
    virtual bool needsPrecomputeBlur() = 0;
    virtual std::optional<CBox> boundingBox() { return std::nullopt; }
    virtual bool disableSimplification() { return false; }
    virtual bool undiscardable() { return false; }
    virtual CRegion opaqueRegion() { return {}; }

    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) final {
        CRenderScope scope(ctx);
        return draw();
    }
    void discard(Render::CRenderContext& ctx) final { CRenderScope scope(ctx); discard(); }
    bool needsLiveBlur(Render::CRenderContext& ctx) final { CRenderScope scope(ctx); return needsLiveBlur(); }
    bool needsPrecomputeBlur(Render::CRenderContext& ctx) final { CRenderScope scope(ctx); return needsPrecomputeBlur(); }
    std::optional<CBox> boundingBox(Render::CRenderContext& ctx) final { CRenderScope scope(ctx); return boundingBox(); }
    bool disableSimplification(Render::CRenderContext& ctx) final { CRenderScope scope(ctx); return disableSimplification(); }
    bool undiscardable(Render::CRenderContext& ctx) final { CRenderScope scope(ctx); return undiscardable(); }
    CRegion opaqueRegion(Render::CRenderContext& ctx) final { CRenderScope scope(ctx); return opaqueRegion(); }
};

class CWindowDecoration : public IHyprWindowDecoration {
  public:
    using IHyprWindowDecoration::IHyprWindowDecoration;
    virtual void draw(PHLMONITOR monitor, const float& alpha) = 0;
    void draw(Render::CRenderContext& ctx, PHLMONITOR monitor, const float& alpha,
              const Render::SWindowRenderPresentation& presentation) final {
        CRenderScope scope(ctx);
        CPresentationScope presented(owningWindow(), presentation);
        draw(monitor, alpha);
    }
};
#else
using CPassElement = IPassElement;
using CWindowDecoration = IHyprWindowDecoration;
#endif

} // namespace HyprlandCompat
