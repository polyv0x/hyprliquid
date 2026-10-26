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
uniform int transparentTarget;

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
#ifdef LAYER_SHAPES
#include "layer-shape.glsl"
#endif
#include "native-shape.glsl"
#include "liquid-material.glsl"

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
#ifdef LAYER_SHAPES
    selectLayerShape(p);
#endif
    float d = sdf_at(p);
    float coverage = liquidSaturate(0.5 - d / max(fwidth(d), 0.0001));
    float inner = refractAmount(-d, uRefraction.x, uRefraction.y);
    float outer = refractAmount(-d, uRefraction.z, uRefraction.w);
    float e = 1.0 / pixelScale;
    vec2 normal = vec2(0.0), optical = vec2(0.0);
    bool refracting = inner != 0.0 || outer != 0.0;
    // With both rays exactly zero, only the highlight bands need a normal.
    // Keep a two-pixel/logical-pixel guard around those bands so their quad
    // derivatives still see the original normals. Color arithmetic stays on
    // the original path, including all endpoint mixes in high-bit-depth FBOs.
    float highlightLimit = max(max(uHighlight0.x, uHighlight1.y), 0.0)
        + abs(uHighlight2.w) + 2.0 * max(e, 1.0);
    if (refracting || -d <= highlightLimit) {
        vec2 gradient = sdf_gradient(p, e) / (2.0 * e);
        normal = gradient / max(length(gradient), 0.000001);
        if (refracting) optical = optical_normal(p, max(-d, 0.0), normal);
    }
    // The two-pixel guard includes the derivative neighbours of both bands.
    // Beyond it the original highlight is exactly zero, so interior pixels
    // need neither its directional response nor its derivative arithmetic.
    vec4 highlight = vec4(0.0);
    if (-d <= highlightLimit)
        highlight = keyFillHighlight(vec4(d, normal, 1.0), uHighlight0, uHighlight1, uHighlight2, uKeyColor, uFillColor);
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
#ifdef LAYER_SHAPES
        // The recovered cards replace the region's many quantized strips.
        // Keep gaps and rounded cutouts transparent, including with >16 strips.
        if (maskMode == 2) inside = coverage > 0.0;
#endif
        if (!inside || (maskMode != 1 && surfacePixel.a < maskAlphaThreshold)) { fragColor = surfacePixel; return; }
    }
    float shadowCoverage = 0.0;
    if (renderShadow == 1 && coverage < 1.0) {
        float shadowDistance = sdf_at(p + uShadowOffset);
        if (shadowDistance < 4.0 * uShadow.z)
            shadowCoverage = 0.5 * exp(-0.5 * pow(max(shadowDistance, 0.0) / max(uShadow.z, 0.0001), 2.0)) * uShadow.w * (1.0 - coverage);
    }
    if (shadowCoverage > 0.0) shadowCoverage *= shadowVisibility(uv);
    if (coverage <= 0.0 && shadowCoverage <= 0.0 && tinted.a <= 0.0) { fragColor = surfacePixel; return; }

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
    if (transparentTarget == 1) {
        // Native close snapshots start transparent. Preserve the coverage of
        // the destination, black shadow, pane and highlight in that order;
        // an opaque exterior would freeze transparent black into a frame.
        // Normal monitor rendering retains its original color/alpha path.
        vec2 halfTexel = 0.5 / vec2(textureSize(tex, 0));
        float destinationAlpha = texture(tex, clamp(backdropUV(uv), halfTexel, 1.0 - halfTexel)).a;
        float composedAlpha = destinationAlpha + (1.0 - destinationAlpha) * shadowCoverage;
        composedAlpha = mix(composedAlpha, 1.0, coverage);
        composedAlpha = tinted.a + composedAlpha * (1.0 - tinted.a);
        glass.a = clamp(composedAlpha, 0.0, 1.0) * alpha;
    }
    fragColor = masked ? surfacePixel + glass * (1.0 - surfacePixel.a) : glass;
}
