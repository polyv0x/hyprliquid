#ifndef LAYER_SHAPES
float sdf_at(vec2 p) {
    vec2 halfSize = fullSize / (2.0 * pixelScale);
    float r = clamp(radius / pixelScale, 0.0, min(halfSize.x, halfSize.y));
    float exponent = max(roundingPower, 1.0);
    vec2 q = abs(p) - (halfSize - r);
    // Both positive parts are zero throughout the rectangular core. Preserve
    // the original subtraction, but avoid three powers for every SDF sample
    // used by coverage, finite-difference normals, refraction and shadows.
    if (max(q.x, q.y) <= 0.0)
        return max(q.x, q.y) - r;
    vec2 qc = max(q, 0.0);
    float corner = pow(pow(qc.x, exponent) + pow(qc.y, exponent), 1.0 / exponent) - r;
    return min(max(q.x, q.y), 0.0) + corner;
}
#endif

vec2 sdf_gradient(vec2 p, float e) {
    return vec2(
        sdf_at(p + vec2(e, 0.0)) - sdf_at(p - vec2(e, 0.0)),
        sdf_at(p + vec2(0.0, e)) - sdf_at(p - vec2(0.0, e))
    );
}

vec2 sdf_normal(vec2 p) {
    // Keep the native outline's one-physical-pixel normal for rim lighting.
    return normalize(sdf_gradient(p, 1.0 / pixelScale) + 1e-6);
}

vec2 optical_normal(vec2 p, float depth, vec2 outlineNormal) {
    // Inward offsets of a small rounded corner eventually meet at a sharp
    // diagonal. Average the slope over a footprint that grows into the glass
    // so refraction and edge bleed turn through that seam continuously.
    // Coverage, depth and rim lighting still use the exact native shape.
    float pixel = 1.0 / pixelScale;
    float footprint = max(pixel, 0.5 * depth);
    if (footprint <= pixel)
        return outlineNormal;
    vec2 slope = sdf_gradient(p, footprint) / (2.0 * footprint);
    // Let opposing sides cancel in small windows instead of normalizing a
    // vanishing gradient into an arbitrary full-strength displacement.
    return slope / max(length(slope), 0.5);
}

float underlay_coverage(vec2 point, vec4 bounds, vec2 rounding) {
    vec2 halfSize = bounds.zw * 0.5;
    float r = clamp(rounding.x, 0.0, min(halfSize.x, halfSize.y));
    vec2 edge = abs(point - bounds.xy - halfSize) - halfSize;
    if (max(edge.x, edge.y) >= 0.5)
        return 0.0;
    vec2 q = edge + r;
    // Straight edges and the interior need only rectangle arithmetic. Evaluate
    // the superellipse only in a rounded corner's small footprint.
    if (min(q.x, q.y) <= 0.0)
        return clamp(0.5 - (max(q.x, q.y) - r), 0.0, 1.0);
    vec2 qc = max(q, 0.0);
    float exponent = max(rounding.y, 1.0);
    float d = min(max(q.x, q.y), 0.0) +
        pow(pow(qc.x, exponent) + pow(qc.y, exponent), 1.0 / exponent) - r;
    return clamp(0.5 - d, 0.0, 1.0);
}
