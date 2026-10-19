#pragma once

#include "Material.hpp"

namespace BuiltInPresets {

// Five distinct materials; old names remain available to saved profiles and tags.
inline std::unordered_map<std::string, SCustomPreset> getAll() {
    std::unordered_map<std::string, SCustomPreset> presets;
    const auto add = [&](const std::string& name, const std::string& parent = "") -> SPresetValues& {
        auto& preset = presets[name];
        preset.name = name;
        preset.inherits = parent;
        return preset.shared;
    };
    const auto alias = [&](const std::string& name, const std::string& target) {
        add(name, target);
        presets.at(name).hidden = true;
    };
    const auto tones = [&](const std::string& name, float rim) {
        auto& preset = presets.at(name);
        preset.dark.face_slope = 0.72f;
        preset.dark.face_intercept = 0.06f;
        preset.light.face_slope = 0.68f;
        preset.light.face_intercept = 0.30f;
        preset.dark.rim_opacity = rim * 0.65f;
        preset.light.rim_opacity = rim * 0.85f;
    };
    add("frosted");
    tones("frosted", 0.0f);
    alias("default", "frosted");
    alias("measured", "frosted");

    auto& platform = add("soft-glass");
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
    tones("soft-glass", platform.rim_opacity);

    alias("platform", "soft-glass");
    for (const auto* name : {"lighter", "darker", "ultra_darker", "no_bleed"})
        alias(name, "soft-glass");

    // Inherit through the old names so existing Lua overrides still reach
    // their descendants. Customized aliases are exposed as user presets.
    auto& lens = add("clear-glass", "platform");
    lens.outer_refraction_amount = 108.0f;
    lens.outer_refraction_inv_height = 0.018f;
    lens.aberration_amount = 19.0f;
    lens.blur_alpha0 = 0.3f;
    lens.blur_alpha3 = 0.0f;
    lens.face_opacity = 0.25f;
    lens.holding_tone_opacity = 0.0f;
    lens.rim_opacity = 1.2f;
    // Keep the face tones inherited from soft-glass, with the lens's own rim.
    presets.at("clear-glass").dark.rim_opacity = lens.rim_opacity * 0.65f;
    presets.at("clear-glass").light.rim_opacity = lens.rim_opacity * 0.85f;
    alias("lens", "clear-glass");

    auto& frostedGlass = add("frosted-glass", "lens");
    frostedGlass.blur_alpha0 = 0.96f;
    frostedGlass.blur_alpha3 = 1.0f;

    auto& clearEdge = add("clear-edge", "lens");
    clearEdge.blur_alpha0 = 0.02f;
    clearEdge.blur_alpha3 = 1.0f;
    clearEdge.blur_dist3 = 38.4f;
    alias("frosted-full", "frosted-glass");
    alias("frosted-clear-edge", "clear-edge");
    return presets;
}

} // namespace BuiltInPresets
