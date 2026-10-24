// Frozen pre-Rust working-tree equations (2026-10-24). Test only.
#pragma once
#include "Material.hpp"
// Scalar defaults expose meaningful controls without changing the canonical
// material. Inapplicable legacy fields keep their compatibility values.
inline SMaterial reference_liquidControlDefaults(bool dark) {
    SMaterial material;
    material.family = EMaterialFamily::LIQUID;
    material.darkTheme = dark;
    const auto& data = liquidMaterial(dark);
    material.blur_sigma = data.blur.radius;
    material.blur_alpha0 = data.blur.opacities.back();
    material.blur_alpha3 = data.blur.opacities.front();
    material.blur_dist3 = -data.blur.distances.front();
    material.inner_refraction_amount = data.refraction.innerAmount;
    material.inner_refraction_inv_height = 1.0 / data.refraction.innerHeight;
    material.outer_refraction_amount = data.refraction.outerAmount;
    // The outer ray is disabled by its zero amount. Give its height control a
    // useful starting point when an explicit amount enables it.
    material.outer_refraction_inv_height = 1.0 / data.refraction.innerHeight;
    material.refraction_threshold0 = -data.refraction.outerDistances[1];
    material.refraction_threshold1 = -data.refraction.outerDistances[0];
    material.refraction_opacity = 1.0f;
    material.face_opacity = data.face.opacity;
    material.rim_width = data.highlight.keyHeight;
    material.rim_opacity = 1.0f;
    material.shadow_amount = data.shadow.opacity;
    material.shadow_offset = data.shadow.offset[1];
    material.neighbor_falloff = 0.0f;
    return material;
}

inline SLiquidMaterial reference_effectiveLiquidMaterial(const SMaterial& material) {
    auto data = liquidMaterial(material.darkTheme);
    const auto defaults = reference_liquidControlDefaults(material.darkTheme);
    // Leave unedited doubles intact so uniforms and base material metadata
    // remain exactly equal, including theme values.
    if (material.blur_sigma != defaults.blur_sigma) data.blur.radius = material.blur_sigma;
    if (material.blur_alpha3 != defaults.blur_alpha3) data.blur.opacities[0] = material.blur_alpha3;
    if (material.blur_alpha0 != defaults.blur_alpha0)
        for (size_t i = 1; i < data.blur.opacities.size(); ++i) data.blur.opacities[i] *= material.blur_alpha0;
    if (material.blur_dist3 != defaults.blur_dist3)
        for (auto& distance : data.blur.distances) distance *= double(material.blur_dist3) / defaults.blur_dist3;
    if (material.inner_refraction_inv_height != defaults.inner_refraction_inv_height)
        data.refraction.innerHeight = 1.0 / material.inner_refraction_inv_height;
    if (material.inner_refraction_amount != defaults.inner_refraction_amount || material.refraction_opacity != 1.0f)
        data.refraction.innerAmount = double(material.inner_refraction_amount) * material.refraction_opacity;
    if (material.outer_refraction_amount != 0.0f && material.refraction_opacity != 0.0f) {
        data.refraction.outerAmount = double(material.outer_refraction_amount) * material.refraction_opacity;
        data.refraction.outerHeight = 1.0 / material.outer_refraction_inv_height;
    }
    if (material.refraction_threshold0 != defaults.refraction_threshold0 || material.refraction_threshold1 != defaults.refraction_threshold1)
        data.refraction.outerDistances = {-double(material.refraction_threshold1), -double(material.refraction_threshold0)};
    if (material.face_opacity != defaults.face_opacity) data.face.opacity = material.face_opacity;
    if (material.rim_width != defaults.rim_width) {
        data.highlight.keyHeight *= material.rim_width;
        data.highlight.fillHeight *= material.rim_width;
    }
    if (material.rim_opacity != defaults.rim_opacity) {
        for (auto& component : data.highlight.keyColor) component *= material.rim_opacity;
        for (auto& component : data.highlight.fillColor) component *= material.rim_opacity;
    }
    if (material.shadow_amount != defaults.shadow_amount) data.shadow.opacity = material.shadow_amount;
    if (material.shadow_offset != defaults.shadow_offset) data.shadow.offset[1] = material.shadow_offset;
    return data;
}

inline float reference_materialBlurSigma(const SMaterial& material) {
    return material.blur_sigma;
}

inline float reference_materialOutputExtent(const SMaterial& material) {
    if (material.family != EMaterialFamily::LIQUID) return 0.0f;
    const auto shadow = reference_effectiveLiquidMaterial(material).shadow;
    // The material's Gaussian coverage is deliberately truncated at four radii.
    return 4.0 * shadow.shadowRadius + std::max(std::abs(shadow.offset[0]), std::abs(shadow.offset[1])) + 1.0;
}

inline void reference_sanitizeMaterial(SMaterial& material) {
#define GLASS_PARAM(name, value, lo, hi) \
    material.name = std::isfinite(material.name) ? std::clamp(material.name, lo, hi) : value;
#include "MaterialParams.inc"
#undef GLASS_PARAM
    // GLSL smoothstep is undefined when its edges are equal or reversed.
    material.refraction_threshold1 = std::max(material.refraction_threshold1,
                                             material.refraction_threshold0 + 0.01f);
}

inline int reference_materialPadding(const SMaterial& material, float scale) {
    if (material.family == EMaterialFamily::LIQUID) {
        const auto liquid = reference_effectiveLiquidMaterial(material);
        // Keep the conservative sampling footprint; exterior shadows use
        // only the local destination, so the shadow displacement is spare room.
        const double rays = std::max({std::abs(liquid.refraction.innerAmount), std::abs(liquid.refraction.outerAmount),
                                      std::abs(liquid.shadow.amount)});
        const double padding = std::ceil((reference_materialOutputExtent(material) + rays + 4.5 * liquid.blur.radius + 2.0) * scale);
        return std::isfinite(padding) && padding >= 0 && padding <= std::numeric_limits<int>::max() / 4
            ? static_cast<int>(padding) : 0;
    }
    const float refraction = std::max(std::abs(material.outer_refraction_amount),
                                      std::abs(material.inner_refraction_amount)) + material.aberration_amount;
    const float bleed = material.edge_bleed_opacity > 0.0f ? material.edge_bleed_amount : 0.0f;
    // Two 13-tap passes per axis reach 4.29 sigma. Include the greatest
    // displaced sample as well, so the blur never reads an artificial edge.
    const double padding = std::ceil((std::max(refraction, bleed) +
                                      4.5 * material.blur_sigma + 2.0) * scale);
    if (!std::isfinite(padding) || padding < 0 || padding > std::numeric_limits<int>::max() / 4)
        return 0;
    return static_cast<int>(padding);
}
