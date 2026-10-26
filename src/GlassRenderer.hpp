#pragma once

#include "PluginConfig.hpp"
#include "LayerShape.hpp"

#include <array>
#include <span>
#include <optional>
#include <GLES3/gl32.h>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <hyprutils/math/Region.hpp>

// Shared GL rendering pipeline used by both window decorations and layer surfaces.
// Callers own their sample framebuffers; these functions operate on passed-in state.
namespace GlassRenderer {

// Bounds and optional damage are in native framebuffer pixels. Copies keep the
// exact native format, dimensions and already blended material bytes.
void transferOutput(const SP<Render::IFramebuffer>& native, const SP<Render::IFramebuffer>& cached,
                    const std::array<int, 4>& bounds, bool restore, const CRegion* damage = nullptr,
                    std::optional<std::array<int, 2>> origin = std::nullopt);

inline constexpr int SAMPLE_PADDING_PX = 60;

// Must match the `regionRects[16]` array size declared in shaders/glass.frag.
inline constexpr int MAX_REGION_RECTS = 16;

// Box-local pixel rect uploaded to the shader's regionRects uniform array.
struct SRegionRect {
    float x = 0, y = 0, w = 0, h = 0;
};
static_assert(sizeof(SRegionRect) == 4 * sizeof(float));

// Layers only: alpha mask from the temp FBO that captured the rendered surface.
// Constrains the glass effect to regions where the layer has visible content.
// Windows do not use masking, they pass mask=nullptr to applyGlassEffect.
struct SMaskInfo {
    GLuint   textureId;
    GLenum   target;
    Vector2D uvOffset; // mapping from glass box UV → full surface UV
    Vector2D uvScale;
    float    alphaThreshold = 0.001f;

    // 0 = alpha threshold, 1 = protocol strips, 2 = recovered rounded cards
    int                                        maskMode        = 0;
    std::array<SRegionRect, MAX_REGION_RECTS>  regionRects{};
    int                                        regionRectCount = 0;
    std::array<SRegionRect, LayerShape::MAX_SHAPES> shapeBounds{};
    std::array<float, LayerShape::MAX_SHAPES> shapeRadii{};
    int shapeCount = 0;
};

// Windows beneath the receiver are part of its real backdrop, not an extra
// neighbour contribution. Each shape is two RGBA texels: local framebuffer
// bounds (x, y, width, height), then radius, rounding power, underlying-window
// eligibility and exterior-shadow occlusion. Legacy shaders ignore the flags.
using SUnderlayShape = std::array<float, 8>;

struct SNeighborBackdrop {
    SP<Render::IFramebuffer> background, blurred;
    std::array<GLint, 2> sampleOrigin;
    std::array<GLint, 4> ownerBounds;
    std::span<const SUnderlayShape> underlayShapes;
    SP<Render::IFramebuffer> halfSharp, scene;
};

bool sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                       CBox box, Vector2D& outPaddingRatio, int padding = SAMPLE_PADDING_PX,
                       SP<Render::IFramebuffer> neighbours = nullptr,
                       SP<Render::IFramebuffer> sceneBackground = nullptr, float neighborContribution = 1.0f,
                       const CRegion* damage = nullptr);

// On every exit, re-bind callerFramebuffer and restore the entry viewport.
// A false result must not be drawn or marked as a valid cached blur.
[[nodiscard]] bool blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer>& blurredFramebuffer, float sigma,
                    SP<Render::IFramebuffer> callerFramebuffer, const SNeighborBackdrop* clean = nullptr,
                    SP<Render::IFramebuffer> materialBackdrop = nullptr, const CRegion* damage = nullptr);

[[nodiscard]] bool prepareMaterialBackdrop(SP<Render::IFramebuffer> sample, SP<Render::IFramebuffer>& halfSharp,
                    SP<Render::IFramebuffer> caller, const SNeighborBackdrop* composition = nullptr, float contribution = 1.0f,
                    const CRegion* damage = nullptr);

// When mask is non-null (layers only), the shader composites the surface content
// over the glass effect in a single pass. When mask is null (windows), the shader
// outputs the glass effect alone.
void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> blurredFramebuffer,
                      SP<Render::IFramebuffer> targetFramebuffer,
                       CBox& rawBox, CBox& transformedBox,
                       float alpha, float cornerRadius, float roundingPower,
                       const Vector2D& paddingRatio, const SMaterial& material,
                       const SMaskInfo* mask = nullptr,
                       const SNeighborBackdrop* neighbors = nullptr,
                       SP<Render::IFramebuffer> materialBackdrop = nullptr, bool renderShadow = true,
                       const CRegion* visible = nullptr);

} // namespace GlassRenderer
