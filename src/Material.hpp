#pragma once

#include "LiquidMaterial.hpp"
#include "RustCore.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

struct SMaterial {
    EMaterialFamily family = EMaterialFamily::LEGACY;
    bool darkTheme = true;
#define GLASS_PARAM(name, value, lo, hi) float name = value;
#include "MaterialParams.inc"
#undef GLASS_PARAM
};

// Local Liquid uses the wallpaper/real underlay for its filter support, even
// when that support crosses the owner edge. Outward rays opt into the color
// scene. Keep legacy optics on their existing dependency path.
inline bool materialNeedsNeighborScene(const SMaterial& material) {
    if (material.family != EMaterialFamily::LIQUID) return true;
    const auto& rays = effectiveLiquidMaterial(material).refraction;
    return (rays.innerHeight > 0 && rays.innerAmount > 0) ||
        (rays.outerHeight > 0 && rays.outerAmount > 0 && rays.outerOpacity > 0);
}

// Negative material values are valid; use an out-of-range inheritance sentinel.
inline constexpr float SENTINEL_FLOAT = -1000000.0f;

struct SPresetValues {
#define GLASS_PARAM(name, value, lo, hi) float name = static_cast<float>(SENTINEL_FLOAT);
#include "MaterialParams.inc"
#undef GLASS_PARAM
};

struct SCustomPreset {
    std::string   name;
    std::string   inherits;
    // Compatibility aliases stay usable without duplicating the preset picker.
    bool          hidden = false;
    std::optional<EMaterialFamily> family;
    SPresetValues shared;
    SPresetValues dark;
    SPresetValues light;
};

inline EMaterialFamily resolveMaterialFamily(std::string name, const std::unordered_map<std::string, SCustomPreset>& presets) {
    // Bound even malformed/cyclic configurations. Family is metadata, never a
    // numeric setting or a comparison against the selected preset's name.
    for (size_t depth = 0; depth <= presets.size(); ++depth) {
        const auto it = presets.find(name);
        if (it == presets.end()) break;
        if (it->second.family) return *it->second.family;
        if (it->second.inherits.empty()) break;
        name = it->second.inherits;
    }
    return EMaterialFamily::LEGACY;
}
