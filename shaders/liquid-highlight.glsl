// Shared by the Liquid body and Glass presets with Liquid rim lighting.
uniform vec4 uHighlight0, uHighlight1, uHighlight2, uKeyColor, uFillColor;
uniform vec4 uHighlightRow0, uHighlightRow1, uHighlightRow2, uHighlightRow3, uHighlightBias;

float liquidSaturate(float x) { return clamp(x, 0.0, 1.0); }

// Cumulative area and first moment of a square pixel projected onto the
// depth gradient. Normalize its larger component to 1; the smaller is ratio.
// The projected density is a trapezoid (a box on axis, a triangle at 45 deg).
vec2 highlightPixelIntegral(float x, float ratio) {
    float halfSupport = 0.5 * (1.0 + ratio);
    float plateau = 0.5 * (1.0 - ratio);
    float t = abs(x);
    float tail = max(halfSupport - t, 0.0);
    float area = tail * tail / (2.0 * max(ratio, 0.000001));
    float moment = area * (halfSupport - (2.0 / 3.0) * tail);
    if (t < plateau) {
        area = 0.5 - t;
        moment = 0.125 + ratio * ratio / 24.0 - 0.5 * t * t;
    }
    return vec2(x < 0.0 ? area : 1.0 - area, -moment);
}
float highlightBand(float depth, float height, float curvature, vec2 footprint) {
    // Integrate the entire ramp over the pixel, including both ends. Applying
    // edge ramps to a center-sampled profile leaves a hard inner cutoff and
    // makes a thin rim flicker with subpixel position. fwidth alone also treats
    // a diagonal pixel as a uniform box, making its rim softer than an axis.
    footprint = abs(footprint);
    float major = max(max(footprint.x, footprint.y), 0.0001);
    float ratio = min(footprint.x, footprint.y) / major;
    if (height <= 0.0) return 0.0;
    vec2 lower = highlightPixelIntegral(-depth / major, ratio);
    vec2 upper = highlightPixelIntegral((height - depth) / major, ratio);
    float area = upper.x - lower.x;
    float moment = depth * area + major * (upper.y - lower.y);
    return liquidSaturate(area - curvature * moment / height);
}
float highlightBand(float depth, float height, float curvature) {
    return highlightBand(depth, height, curvature, vec2(dFdx(depth), dFdy(depth)));
}
float highlightDirection(vec2 normal, vec2 direction, float cosineSpread) {
    return liquidSaturate((dot(direction, normal) - cosineSpread) / max(1.0 - cosineSpread, 0.000001));
}
float highlightResponse(float a, float coefficient) {
    return a / max(1.0 + coefficient * (1.0 - a), 0.000001);
}
vec4 keyFillHighlight(vec4 s, vec4 p0, vec4 p1, vec4 p2, vec4 keyColor, vec4 fillColor, vec2 footprint) {
    float g = max(length(s.yz), 0.000001);
    vec2 normal = s.yz / g;
    float depth = -(s.x + p2.w) / g;
    float key = highlightBand(depth, p0.x, p2.z, footprint / g)
        * highlightDirection(normal, vec2(p0.w, p1.x), p0.y);
    float fill = highlightBand(depth, p1.y, p2.z, footprint / g)
        * highlightDirection(normal, p2.xy, p1.z);
    if (depth < -5.0 || key + fill < 0.000001) return vec4(0.0);
    return keyColor * highlightResponse(key, p0.z) + fillColor * highlightResponse(fill, p1.w);
}
vec4 keyFillHighlight(vec4 s, vec4 p0, vec4 p1, vec4 p2, vec4 keyColor, vec4 fillColor) {
    return keyFillHighlight(s, p0, p1, p2, keyColor, fillColor, vec2(dFdx(s.x), dFdy(s.x)));
}
vec4 approximateHighlightMatrix(vec4 highlight) {
    // Premultiplied interpretation: a zero highlight must add no bias.
    return clamp(vec4(dot(uHighlightRow0, highlight), dot(uHighlightRow1, highlight),
                      dot(uHighlightRow2, highlight), dot(uHighlightRow3, highlight))
                      + uHighlightBias * highlight.a, 0.0, 1.0);
}
