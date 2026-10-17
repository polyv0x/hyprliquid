#pragma once

#include "Material.hpp"

namespace BuiltInPresets {

// Built-in materials with names suitable for window tags. "measured" uses
// the global defaults; "platform" supplies the base for the other presets.
inline std::unordered_map<std::string, SCustomPreset> getAll() {
    std::unordered_map<std::string, SCustomPreset> presets;
    const auto add = [&](const std::string& name, const std::string& parent = "") -> SPresetValues& {
        auto& preset = presets[name];
        preset.name = name;
        preset.inherits = parent;
        return preset.shared;
    };
    add("default");
    add("measured");

    auto& platform = add("platform");
    platform.use_measured_face = 0.0f;
    platform.outer_refraction_amount = 52.0f;
    platform.inner_refraction_amount = -7.0f;
    platform.blur_alpha3 = 0.82f;
    platform.blur_dist3 = 96.0f;
    platform.edge_bleed_opacity = 0.46f;
    platform.bleed_darken = 0.17f;
    platform.holding_tone_opacity = 0.05f;
    platform.shadow_amount = 0.2f;
    platform.aberration_amount = 7.5f;
    platform.rim_opacity = 0.92f;

    auto& lighter = add("lighter", "platform");
    lighter.holding_tone_opacity = 0.19f;
    lighter.rim_opacity = 1.15f;
    lighter.bleed_darken = 0.06f;

    auto& darker = add("darker", "platform");
    darker.holding_tone_opacity = 0.0f;
    darker.face_opacity = 0.55f;
    darker.bleed_darken = 0.36f;
    darker.rim_opacity = 0.7f;
    darker.shadow_amount = 0.34f;

    auto& ultra = add("ultra_darker", "platform");
    ultra.holding_tone_opacity = 0.0f;
    ultra.face_opacity = 0.2f;
    ultra.bleed_darken = 0.55f;
    ultra.rim_opacity = 0.5f;
    ultra.shadow_amount = 0.46f;
    ultra.refraction_opacity = 0.88f;

    auto& lens = add("lens", "platform");
    lens.outer_refraction_amount = 108.0f;
    lens.outer_refraction_inv_height = 0.018f;
    lens.aberration_amount = 19.0f;
    lens.blur_alpha0 = 0.3f;
    lens.blur_alpha3 = 0.0f;
    lens.face_opacity = 0.25f;
    lens.holding_tone_opacity = 0.0f;
    lens.rim_opacity = 1.2f;

    auto& noBleed = add("no_bleed", "platform");
    noBleed.edge_bleed_opacity = 0.0f;
    noBleed.bleed_darken = 0.0f;
    return presets;
}

} // namespace BuiltInPresets
