#version 300 es
precision highp float;

// Drawn only where the original material has full coverage and blur, with
// every edge effect exactly zero. The boundary still uses glass.frag unchanged.
uniform sampler2D tex;
uniform sampler2D blur_texture;
uniform vec2 uvPadding;
uniform float face_slope;
uniform float face_intercept;
uniform float face_opacity;
uniform float holding_tone_opacity;
uniform float glass_opacity;
uniform float fadeAlpha;
uniform float blur_alpha3;
uniform float refraction_opacity;
uniform float interiorCoverage;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 uv = v_texcoord * (1.0 - 2.0 * uvPadding) + uvPadding;
    vec2 halfTexel = 0.5 / vec2(textureSize(tex, 0));
    uv = clamp(uv, halfTexel, 1.0 - halfTexel);
    vec3 bg = texture(tex, uv).rgb;
    // Keep endpoint mixes dynamic: some drivers lower mix to x+a*(y-x),
    // whose rounding must match the original even when a is exactly one.
    vec3 refr = mix(bg, texture(blur_texture, uv).rgb, blur_alpha3);
    refr = mix(refr, refr * face_slope + face_intercept, face_opacity);
    refr = mix(refr, vec3(0.5), holding_tone_opacity);
    refr = mix(bg, refr, refraction_opacity);
    float glassA = clamp(glass_opacity * fadeAlpha, 0.0, 1.0);
    fragColor = vec4(clamp(mix(bg, refr, interiorCoverage), 0.0, 1.0) * glassA, glassA);
}
