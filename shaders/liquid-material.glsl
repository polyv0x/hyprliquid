// Float instruction transcriptions, plus explicitly named approximations.
// Base material values are defined in materials/liquid.json.
#include "liquid-highlight.glsl"

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
