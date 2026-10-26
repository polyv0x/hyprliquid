#pragma once

#include "LiquidMaterialData.hpp"
#include <string_view>

enum class EMaterialFamily { LEGACY, LIQUID };

inline constexpr const char* materialFamilyName(EMaterialFamily family) {
    return family == EMaterialFamily::LIQUID ? "liquid" : "legacy";
}

// Base Liquid values before preset and scalar overrides. Light uses the default
// variant; dark uses its dedicated color and shadow values.
inline constexpr const SLiquidMaterial& liquidMaterial(bool dark) {
    return dark ? LIQUID_DARK : LIQUID_DEFAULT;
}

inline constexpr bool liquidParameter(std::string_view name) {
    for (const auto candidate : {
        "blur_sigma", "blur_alpha0", "blur_alpha3", "blur_dist3",
        "inner_refraction_amount", "inner_refraction_inv_height",
        "outer_refraction_amount", "outer_refraction_inv_height",
        "refraction_threshold0", "refraction_threshold1", "refraction_opacity",
        "face_opacity", "rim_width", "rim_opacity", "display_angle_degrees", "shadow_amount", "shadow_offset",
        "glass_opacity", "effect_scale", "neighbor_contribution", "neighbor_falloff"})
        if (name == candidate) return true;
    return false;
}

inline constexpr bool materialParameter(EMaterialFamily family, std::string_view name) {
    // Keep stored model fields and preset defaults compatible, but advertise
    // only rendered controls. Native Hyprland decorations own window shadows.
    return family == EMaterialFamily::LIQUID ?
        liquidParameter(name) && name != "shadow_amount" && name != "shadow_offset" : name != "display_angle_degrees";
}
