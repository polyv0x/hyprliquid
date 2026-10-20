#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

struct SMaterial {
#define GLASS_PARAM(name, value, lo, hi) float name = value;
#include "MaterialParams.inc"
#undef GLASS_PARAM
};

inline void sanitizeMaterial(SMaterial& material) {
#define GLASS_PARAM(name, value, lo, hi) \
    material.name = std::isfinite(material.name) ? std::clamp(material.name, lo, hi) : value;
#include "MaterialParams.inc"
#undef GLASS_PARAM
    // GLSL smoothstep is undefined when its edges are equal or reversed.
    material.refraction_threshold1 = std::max(material.refraction_threshold1,
                                             material.refraction_threshold0 + 0.01f);
}

inline int materialPadding(const SMaterial& material, float scale) {
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
    SPresetValues shared;
    SPresetValues dark;
    SPresetValues light;
};
