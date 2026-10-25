#pragma once
#include "Material.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

// A conservative rectangle wholly inside the original shader's exact cutoffs.
// The extra logical pixel keeps floating-point reciprocals off the boundaries.
inline std::optional<double> glassInteriorInset(const SMaterial& m, double radius, double pixelScale) {
    if (m.family != EMaterialFamily::LEGACY || m.blur_alpha3 != 1 || m.refraction_opacity != 1 || pixelScale <= 0)
        return std::nullopt;
    return (1 + std::max({radius / pixelScale, double(m.blur_dist3),
        1.0 / m.outer_refraction_inv_height, 1.0 / m.inner_refraction_inv_height,
        1.0 / m.inv_aberration_height, 1.0 / m.edge_bleed_inv_height,
        double(m.rim_width), std::abs(double(m.shadow_offset)) + 1.0 / m.shadow_inv_height,
        double(m.coverage_offset + m.coverage_width)})) * pixelScale;
}
