#pragma once

#include "ResizeAngle.hpp"

struct json_object;

#include "GlassRenderer.hpp"
#include "PluginConfig.hpp"
#include "WindowScene.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/Framebuffer.hpp>

class CGlassDecoration : public IHyprWindowDecoration {
  public:
    explicit CGlassDecoration(PHLWINDOW window);
    ~CGlassDecoration() override;

    [[nodiscard]] SDecorationPositioningInfo getPositioningInfo() override;
    void                                     onPositioningReply(const SDecorationPositioningReply& reply) override;
    void                                     draw(PHLMONITOR monitor, float const& alpha) override;
    [[nodiscard]] eDecorationType            getDecorationType() override;
    void                                     updateWindow(PHLWINDOW window) override;
    void                                     damageEntire() override;
    [[nodiscard]] eDecorationLayer           getDecorationLayer() override;
    [[nodiscard]] uint64_t                   getDecorationFlags() override;
    [[nodiscard]] std::string                getDisplayName() override;

    [[nodiscard]] PHLWINDOW getOwner();
    void                    renderPass(PHLMONITOR monitor, const float& alpha,
                                       const std::shared_ptr<SWindowScene>& scene = {}, bool sceneBase = false);
    [[nodiscard]] bool      usesGlass() const { return resolveEnabled(); }
    [[nodiscard]] bool      usesLiquidGlass() const;
    [[nodiscard]] json_object* diagnostics() const;
    void                    onFullscreenStateChanged();

    WP<CGlassDecoration> m_self;

    [[nodiscard]] int samplingPadding(PHLMONITOR monitor) const;

  private:
    PHLWINDOWREF m_window;

    // Track last rendered position/size to detect actual changes and seed damage
    Vector2D m_lastPosition;
    Vector2D m_lastSize;
    CResizeAngle m_displayAngle;
    float m_renderedDisplayAngle = 0.0f;

    // Whether we currently hold the noblur window property on our window.
    // Glass replaces Hyprland's blur, and Hyprland's cached-blur optimization
    // (blur:new_optimizations) composites translucent windows against a
    // snapshot taken before plugin decorations render — without noblur the
    // glass is invisible on static windows (#46).
    bool m_noBlurApplied = false;
    bool m_noShadowApplied = false;
    std::optional<bool> m_savedNoShadow;
    float m_outputExtent = 0.0f;
    std::optional<CBox> m_lastDamageBox;

    void updateNoBlurProp(bool glassEnabled);
    void withdrawNoBlur();
    void updateShadowProp(bool enabled);

    [[nodiscard]] bool        resolveEnabled() const;
    [[nodiscard]] bool        resolveThemeIsDark() const;
    [[nodiscard]] std::string resolvePresetName() const;
    [[nodiscard]] const SPresetValues* runtimeOverrides() const;

    friend class CGlassPassElement;
};
