// Frozen native Liquid medium before editable controls, including the local-shadow correction.
#version 300 es
precision highp float;

uniform sampler2D tex; // Exact full-resolution destination crop, including outside the owner.
uniform sampler2D backdrop_texture; // Retained half-resolution material input.
uniform sampler2D blur_texture;
uniform sampler2D clean_backdrop_texture;
uniform sampler2D clean_blur_texture;
uniform int useNeighborContribution;
uniform float neighbor_contribution, neighbor_falloff;
uniform highp sampler2D underlayShapes;
uniform int underlayCount;
uniform vec2 fullSize, uvPadding;
uniform float radius, roundingPower, pixelScale, fadeAlpha, glass_opacity;
uniform vec2 outputExtent; // Quad expansion in physical pixels; fullSize remains the shape size.
uniform int renderShadow;

uniform vec4 uRefraction;
uniform vec3 uOuter;
uniform float uBlurDistances[5], uBlurOpacities[5];
uniform vec4 uFace, uFaceFill;
uniform vec4 uShadow;
uniform vec2 uShadowOffset;
uniform vec4 uHighlight0, uHighlight1, uHighlight2, uKeyColor, uFillColor;
uniform vec4 uHighlightRow0, uHighlightRow1, uHighlightRow2, uHighlightRow3, uHighlightBias;

uniform sampler2D maskTex;
uniform int useMask, maskMode, regionRectCount;
uniform vec2 maskUVOffset, maskUVScale;
uniform float maskAlphaThreshold;
uniform vec4 regionRects[16];

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;
float sdf_at(vec2 p) {
    vec2 halfSize = fullSize / (2.0 * pixelScale);
    float r = clamp(radius / pixelScale, 0.0, min(halfSize.x, halfSize.y));
    float exponent = max(roundingPower, 1.0);
    vec2 q = abs(p) - (halfSize - r);
    vec2 qc = max(q, 0.0);
    float corner = pow(pow(qc.x, exponent) + pow(qc.y, exponent), 1.0 / exponent) - r;
    return min(max(q.x, q.y), 0.0) + corner;
}

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


// Float instruction transcriptions, plus explicitly named approximations.
// Frozen material equations for pixel regression checks.
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


vec2 backdropUV(vec2 uv) { return uv * (1.0 - 2.0 * uvPadding) + uvPadding; }
vec3 sampleClamped(sampler2D inputTexture, vec2 uv) {
    vec2 halfTexel = 0.5 / vec2(textureSize(inputTexture, 0));
    return texture(inputTexture, clamp(uv, halfTexel, 1.0 - halfTexel)).rgb;
}
vec2 underlayWeights(vec2 receiver, vec2 innerUV, vec2 outerUV) {
    vec2 weight = vec2(0.0);
    if (useNeighborContribution == 0) return weight;
    for (int i = 0; i < underlayCount; ++i) {
        vec4 bounds = texelFetch(underlayShapes, ivec2(0, i), 0);
        vec4 roundingFlags = texelFetch(underlayShapes, ivec2(1, i), 0);
        if (roundingFlags.z < 0.5) continue;
        vec2 rounding = roundingFlags.xy;
        float receiving = underlay_coverage(receiver * fullSize, bounds, rounding);
        weight = max(weight, receiving * vec2(
            underlay_coverage(innerUV * fullSize, bounds, rounding),
            underlay_coverage(outerUV * fullSize, bounds, rounding)));
        if (all(greaterThanEqual(weight, vec2(1.0)))) break;
    }
    return weight;
}
float shadowVisibility(vec2 receiver) {
    float occlusion = 0.0;
    for (int i = 0; i < underlayCount; ++i) {
        vec4 roundingFlags = texelFetch(underlayShapes, ivec2(1, i), 0);
        if (roundingFlags.w < 0.5) continue;
        vec4 bounds = texelFetch(underlayShapes, ivec2(0, i), 0);
        occlusion = max(occlusion, underlay_coverage(receiver * fullSize, bounds, roundingFlags.xy));
    }
    return 1.0 - occlusion;
}
vec3 sampleBackdrop(vec2 uv, float blur, float underlay) {
    vec2 padded = backdropUV(uv);
    vec3 result = mix(sampleClamped(backdrop_texture, padded), sampleClamped(blur_texture, padded), blur);
    if (useNeighborContribution == 1) {
        vec3 clean = mix(sampleClamped(clean_backdrop_texture, padded), sampleClamped(clean_blur_texture, padded), blur);
        result = mix(clean, result, mix(neighbor_contribution, 1.0, underlay));
    }
    return result;
}
float rayBlur(float distance, float depth) {
    float weight = blurWeight(distance);
    // Optional native artistic control, consistently applied to each ray.
    return 1.0 - (1.0 - weight) * exp2(-max(depth, 0.0) * neighbor_falloff / 8.0);
}
void main() {
    vec2 uv = (v_texcoord * (fullSize + 2.0 * outputExtent) - outputExtent) / fullSize;
    vec2 p = (uv - 0.5) * fullSize / pixelScale;
    float d = sdf_at(p);
    float coverage = liquidSaturate(0.5 - d / max(fwidth(d), 0.0001));
    float e = 1.0 / pixelScale;
    vec2 gradient = sdf_gradient(p, e) / (2.0 * e);
    // Match the reference's unit highlight normal at native superellipse corners.
    vec2 normal = gradient / max(length(gradient), 0.000001);
    vec2 optical = optical_normal(p, max(-d, 0.0), normal);
    // Derivatives must run before any nonuniform mask/coverage return.
    vec4 highlight = keyFillHighlight(vec4(d, normal, 1.0), uHighlight0, uHighlight1, uHighlight2, uKeyColor, uFillColor);
    vec4 tinted = approximateHighlightMatrix(highlight);

    vec4 surfacePixel = vec4(0.0);
    bool masked = useMask == 1;
    if (masked) {
        surfacePixel = texture(maskTex, uv * maskUVScale + maskUVOffset);
        bool inside = maskMode != 1;
        if (maskMode == 1) for (int i = 0; i < regionRectCount; ++i) {
            vec4 rect = regionRects[i];
            if (all(greaterThanEqual(uv * fullSize, rect.xy)) && all(lessThanEqual(uv * fullSize, rect.xy + rect.zw))) inside = true;
        }
        if (!inside || (maskMode != 1 && surfacePixel.a < maskAlphaThreshold)) { fragColor = surfacePixel; return; }
    }
    float shadowDistance = sdf_at(p + uShadowOffset);
    float shadowCoverage = renderShadow == 1 && shadowDistance < 4.0 * uShadow.z
        ? 0.5 * exp(-0.5 * pow(max(shadowDistance, 0.0) / max(uShadow.z, 0.0001), 2.0)) * uShadow.w * (1.0 - coverage)
        : 0.0;
    if (shadowCoverage > 0.0) shadowCoverage *= shadowVisibility(uv);
    if (coverage <= 0.0 && shadowCoverage <= 0.0 && tinted.a <= 0.0) { fragColor = surfacePixel; return; }

    float inner = refractAmount(-d, uRefraction.x, uRefraction.y);
    float outer = refractAmount(-d, uRefraction.z, uRefraction.w);
    vec2 innerUV = uv + optical * inner * pixelScale / fullSize;
    vec2 outerUV = uv + optical * outer * pixelScale / fullSize;
    vec2 underlay = underlayWeights(uv, innerUV, outerUV);
    vec3 body = sampleBackdrop(innerUV, rayBlur(d + inner, -d), underlay.x);
    float outerWeight = liquidSaturate((d - uOuter.x) / max(uOuter.y - uOuter.x, 0.000001)) * uOuter.z;
    body = mix(body, sampleBackdrop(outerUV, rayBlur(d + outer, -d), underlay.y), outerWeight);
    body = mix(body, approximateYCC(body, uFace.xyz, uFaceFill), uFace.w);
    vec3 scene = sampleClamped(tex, backdropUV(uv));
    // A cast shadow attenuates the exact destination at this pixel. Sampling
    // a displaced material ray here copies neighboring content outside glass.
    vec3 result = scene * (1.0 - shadowCoverage);
    result = mix(result, body, coverage);
    result = tinted.rgb + result * (1.0 - tinted.a);
    float alpha = clamp(glass_opacity * fadeAlpha, 0.0, 1.0);
    vec4 glass = vec4(clamp(result, 0.0, 1.0) * alpha, alpha);
    fragColor = masked ? surfacePixel + glass * (1.0 - surfacePixel.a) : glass;
}
