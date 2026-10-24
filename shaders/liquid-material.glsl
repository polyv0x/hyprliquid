// Float instruction transcriptions, plus explicitly named approximations.
// Base material values are defined in materials/liquid.json.
float liquidSaturate(float x) { return clamp(x, 0.0, 1.0); }

float refractAmount(float depth, float height, float amount) {
    if (height <= 0.0 || amount == 0.0) return 0.0;
    float t = liquidSaturate(depth / height);
    return amount * (1.0 - sqrt(t * (2.0 - t)));
}

// CPU stop interpolation, not the unknown four-coefficient GPU packing.
float blurWeight(float distance) {
    float value = uBlurOpacities[0];
    for (int i = 1; i < 5; ++i) {
        float lo = uBlurDistances[i - 1], hi = uBlurDistances[i];
        float t = hi > lo ? liquidSaturate((distance - lo) / (hi - lo)) : step(hi, distance);
        value = mix(value, uBlurOpacities[i], t);
    }
    return value;
}

vec3 approximateYCC(vec3 color, vec3 ycc, vec4 fill) {
    float y = dot(color, vec3(0.2126, 0.7152, 0.0722));
    vec3 result = vec3(ycc.x) + (ycc.y - ycc.x) * mix(vec3(y), color, ycc.z);
    return mix(result, fill.rgb, fill.a);
}

float highlightBand(float depth, float height, float curvature) {
    float width = max(fwidth(depth), 0.0001);
    float t = liquidSaturate(depth / max(height, 0.000001));
    float profile = mix(t < 1.0 ? 1.0 : 0.0, 1.0 - t, curvature);
    return height <= 0.0 ? 0.0 : profile * liquidSaturate(depth / width + 0.5)
        * liquidSaturate((height - depth) / width + 0.5);
}
float highlightDirection(vec2 normal, vec2 direction, float cosineSpread) {
    return liquidSaturate((dot(direction, normal) - cosineSpread) / max(1.0 - cosineSpread, 0.000001));
}
float highlightResponse(float a, float coefficient) {
    return a / max(1.0 + coefficient * (1.0 - a), 0.000001);
}
vec4 keyFillHighlight(vec4 s, vec4 p0, vec4 p1, vec4 p2, vec4 keyColor, vec4 fillColor) {
    float g = max(length(s.yz), 0.000001);
    vec2 normal = s.yz / g;
    float depth = -(s.x + p2.w) / g;
    float key = highlightBand(depth, p0.x, p2.z)
        * highlightDirection(normal, vec2(p0.w, p1.x), p0.y);
    float fill = highlightBand(depth, p1.y, p2.z)
        * highlightDirection(normal, p2.xy, p1.z);
    if (depth < -5.0 || key + fill < 0.000001) return vec4(0.0);
    return keyColor * highlightResponse(key, p0.z) + fillColor * highlightResponse(fill, p1.w);
}
vec4 approximateHighlightMatrix(vec4 highlight) {
    // Premultiplied interpretation: a zero highlight must add no bias.
    return clamp(vec4(dot(uHighlightRow0, highlight), dot(uHighlightRow1, highlight),
                      dot(uHighlightRow2, highlight), dot(uHighlightRow3, highlight))
                      + uHighlightBias * highlight.a, 0.0, 1.0);
}
