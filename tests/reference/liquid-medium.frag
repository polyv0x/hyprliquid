#extension GL_OES_standard_derivatives : enable
precision highp float;
varying vec2 vUV;
uniform sampler2D source_texture; // full-resolution scene
uniform sampler2D backdrop_texture; // half-resolution backdrop (preset scale 0.5)
uniform sampler2D blur_texture; // separable Gaussian blur
uniform vec2 uRes, uCenter, uHalf;
uniform float uScale, uCornerRadius;
uniform vec4 uRefraction; // inner height/amount, outer height/amount
uniform vec3 uOuter; // outer distances 0/1 and opacity
uniform float uBlurDistances[5], uBlurOpacities[5];
uniform vec4 uFace; // black, white, saturation, opacity
uniform vec4 uFaceFill;
uniform vec4 uShadow; // amount, height, radius, opacity
uniform vec2 uShadowOffset;
uniform vec4 uShadowYCC, uShadowFill;
uniform vec4 uHighlight0, uHighlight1, uHighlight2;
uniform vec4 uKeyColor, uFillColor;
uniform vec4 uHighlightRow0, uHighlightRow1, uHighlightRow2, uHighlightRow3;
uniform vec4 uHighlightBias;

// Independent floating-point highlight equations for regression comparisons.
// Fragment stage; needs OES_standard_derivatives on WebGL1.

float referenceSaturate(float x) { return clamp(x, 0.0, 1.0); }

float referenceHighlightBand(float depth, float height, float curvature) {
    float t = referenceSaturate(depth / height);
    float profile = mix(t < 1.0 ? 1.0 : 0.0, 1.0 - t, curvature);
    float width = max(fwidth(depth), 0.0001);
    return profile * referenceSaturate(depth / width + 0.5)
        * referenceSaturate((height - depth) / width + 0.5);
}

float referenceHighlightDirection(vec2 normal, vec2 direction, float cosineSpread) {
    return referenceSaturate((dot(direction, normal) - cosineSpread)
                        / max(1.0 - cosineSpread, 0.000001));
}

float referenceHighlightResponse(float a, float coefficient) {
    return a / max(1.0 + coefficient * (1.0 - a), 0.000001);
}

// s = (signed distance, gradient x, gradient y, shape opacity).
// p0 = (height, cosine spread, direction x, direction y).
// p1 = (response coefficient, curvature, inset, unused).
vec4 referenceGlassHighlight(vec4 s, vec4 p0, vec4 p1, vec4 color) {
    float g = length(s.yz);
    vec2 normal = s.yz / g;
    float depth = -(s.x + p1.z) / g;
    float a = referenceHighlightBand(depth, p0.x, p1.y)
        * referenceHighlightDirection(normal, p0.zw, p0.y);
    if (depth < -5.0) a = 0.0;
    if (a < 0.000001) return vec4(0.0); // original discards the fragment
    return color * referenceHighlightResponse(a, p1.x);
}

// Packed layout reconstructed from the helper's parameter reads:
// p0 = (key height, key cosine spread, key response coefficient, key dir x)
// p1 = (key dir y, fill height, fill cosine spread, fill response coefficient)
// p2 = (fill dir x, fill dir y, shared curvature, inset)
vec4 referenceKeyFillHighlight(vec4 s, vec4 p0, vec4 p1, vec4 p2,
                           vec4 keyColor, vec4 fillColor) {
    float g = length(s.yz);
    vec2 normal = s.yz / g;
    float depth = -(s.x + p2.w) / g;
    float key = referenceHighlightBand(depth, p0.x, p2.z)
        * referenceHighlightDirection(normal, vec2(p0.w, p1.x), p0.y);
    float fill = referenceHighlightBand(depth, p1.y, p2.z)
        * referenceHighlightDirection(normal, p2.xy, p1.z);
    if (depth < -5.0) { key = 0.0; fill = 0.0; }
    if (key + fill < 0.000001) return vec4(0.0);
    return keyColor * referenceHighlightResponse(key, p0.z)
        + fillColor * referenceHighlightResponse(fill, p1.w);
}

// Separate displacement helper; inactive in the material below.
// p0 = (height, profile mix, inset, coverage offset), p1.xy rotates/scales.
vec4 referenceGlassDisplacement(vec4 s, vec4 p0, vec4 p1) {
    float depth = -(s.x + p0.z);
    float coveredDepth = depth - p0.w;
    float width = max(fwidth(coveredDepth), 0.0001);
    float coverage = referenceSaturate(coveredDepth / width + 0.5) * s.w;
    if (coveredDepth < -5.0) coverage = 0.0;
    float t = referenceSaturate(depth / p0.x);
    float flatProfile = 1.0 - (t < 1.0 ? 0.2928999960422516 : 0.0);
    float roundProfile = sqrt(1.0 - (1.0 - t) * (1.0 - t));
    float profile = mix(flatProfile, roundProfile, p0.y);
    vec2 gradient = vec2(dot(s.yz, vec2(p1.x, -p1.y)), dot(s.yz, p1.yx));
    vec2 offset = gradient * (1.0 - profile)
        * referenceSaturate((s.x + p0.z + p0.x) / width + 0.5);
    return vec4(offset, coverage, 1.0);
}


// Rounded-rectangle test geometry; derivatives handle the output pixel footprint.
float shape(vec2 p) {
    vec2 q = abs(p) - (uHalf / uScale - uCornerRadius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uCornerRadius;
}
vec2 normalAt(vec2 p) {
    const float e = 0.05;
    vec2 gradient = vec2(shape(p + vec2(e, 0.0)) - shape(p - vec2(e, 0.0)),
                         shape(p + vec2(0.0, e)) - shape(p - vec2(0.0, e)));
    // Choose a stable normal at the rectangle's nondifferentiable center.
    return length(gradient) > 0.000001 ? normalize(gradient) : vec2(0.0, 1.0);
}

// t = saturate(depth * inverseHeight); amount * (1 - sqrt(t * (2 - t))).
float refractAmount(float depth, float height, float amount) {
    if (height <= 0.0 || amount == 0.0) return 0.0;
    float t = referenceSaturate(depth / height);
    return amount * (1.0 - sqrt(t * (2.0 - t)));
}

// Evaluate all five blur stops directly. Duplicate stops are right-continuous.
float blurWeight(float distance) {
    float value = uBlurOpacities[0];
    for (int i = 1; i < 5; ++i) {
        float lo = uBlurDistances[i - 1], hi = uBlurDistances[i];
        float t = hi > lo ? referenceSaturate((distance - lo) / (hi - lo))
                          : step(hi, distance);
        value = mix(value, uBlurOpacities[i], t);
    }
    return value;
}
vec3 sampleBackdrop(vec2 uv, float weight) {
    return mix(texture2D(backdrop_texture, uv).rgb,
               texture2D(blur_texture, uv).rgb, weight);
}

// Luma, range, saturation and fill transform for the Liquid face color.
vec3 referenceYCC(vec3 c, vec3 ycc, vec4 fill) {
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 outColor = vec3(ycc.x) + (ycc.y - ycc.x) * mix(vec3(y), c, ycc.z);
    return mix(outColor, fill.rgb, fill.a);
}

void main() {
    vec2 p = (vUV * uRes - uCenter) / uScale;
    float d = shape(p);
    vec2 n = normalAt(p);
    vec3 scene = texture2D(source_texture, vUV).rgb;
    float coverage = referenceSaturate(0.5 - d / max(fwidth(d), 0.0001));

    float displacement = refractAmount(-d, uRefraction.x, uRefraction.y);
    vec2 refractedUV = vUV + n * displacement * uScale / uRes;
    vec3 body = sampleBackdrop(refractedUV, blurWeight(d + displacement));
    float outerDisplacement = refractAmount(-d, uRefraction.z, uRefraction.w);
    vec2 outerUV = vUV + n * outerDisplacement * uScale / uRes;
    float outerWeight = referenceSaturate((d - uOuter.x) / max(uOuter.y - uOuter.x, 0.000001)) * uOuter.z;
    body = mix(body, sampleBackdrop(outerUV, blurWeight(d + outerDisplacement)), outerWeight);
    body = mix(body, referenceYCC(body, uFace.xyz, uFaceFill), uFace.w);

    // Alternate shadow composition for body/highlight isolation tests.
    // Production uses local darkening, tested separately with analytic coverage.
    float shadowDistance = shape(p + uShadowOffset);
    float shadowCoverage = 0.5 * exp(-0.5 * pow(max(shadowDistance, 0.0)
                                              / max(uShadow.z, 0.0001), 2.0));
    float shadowDisplacement = refractAmount(-d, uShadow.y, uShadow.x);
    vec3 shadowColor = sampleBackdrop(vUV + n * shadowDisplacement * uScale / uRes, 0.0);
    shadowColor = referenceYCC(shadowColor, uShadowYCC.xyz, uShadowFill);
    vec3 result = mix(scene, shadowColor, shadowCoverage * uShadow.w * (1.0 - coverage));
    result = mix(result, body, coverage);

    // Key/fill highlights with explicitly supplied directions.
    vec4 highlight = referenceKeyFillHighlight(vec4(d, n, 1.0),
        uHighlight0, uHighlight1, uHighlight2, uKeyColor, uFillColor);
    // Apply the 4x5 color matrix to premultiplied colors.
    vec4 tinted = vec4(dot(uHighlightRow0, highlight), dot(uHighlightRow1, highlight),
                       dot(uHighlightRow2, highlight), dot(uHighlightRow3, highlight))
                       + uHighlightBias * highlight.a;
    tinted = clamp(tinted, 0.0, 1.0);
    result = tinted.rgb + result * (1.0 - tinted.a);
    gl_FragColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
