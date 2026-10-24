#version 300 es
precision highp float;

uniform sampler2D tex;
#include "clean-backdrop.glsl"
uniform int cleanInput; // reconstruct the clean crop during its first reduction
uniform vec2 direction; // tap spacing in texture coordinates
uniform int filterMode; // 0: Gaussian, 1: downsample, 2: reconstruct, 3: retained input copy
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

vec3 sample_input(vec2 uv) {
    return cleanInput == 1 ? sample_clean(uv) : texture(tex, uv).rgb;
}

void main() {
    if (filterMode == 3) { fragColor = vec4(sample_input(v_texcoord), 1.0); return; }
    if (filterMode != 0) {
        // Filter every reduction instead of skipping pixels in the sharp
        // image. Smaller offsets during reconstruction soften the coarse
        // texel grid without expanding it into visible bilinear cells.
        vec2 offset = vec2(filterMode == 1 ? 0.5 : 0.25) / vec2(textureSize(tex, 0));
        vec3 color = sample_input(v_texcoord + vec2(-offset.x, -offset.y));
        color += sample_input(v_texcoord + vec2(offset.x, -offset.y));
        color += sample_input(v_texcoord + vec2(-offset.x, offset.y));
        color += sample_input(v_texcoord + vec2(offset.x, offset.y));
        fragColor = vec4(color * 0.25, 1.0);
        return;
    }

    // 13 Gaussian taps, twice per axis, at a resolution where adjacent
    // samples are no more than one texel apart.
    const float w[7] = float[7](0.1964, 0.1760, 0.1217, 0.0650, 0.0267, 0.0085, 0.0021);
    vec3 color = texture(tex, v_texcoord).rgb * w[0];
    for (int i = 1; i < 7; ++i) {
        vec2 offset = direction * float(i);
        color += texture(tex, v_texcoord + offset).rgb * w[i];
        color += texture(tex, v_texcoord - offset).rgb * w[i];
    }
    // Rounded/truncated weights total 0.9964. Normalize each pass so the
    // four-pass blur preserves flat colors when sharp detail blends into it.
    fragColor = vec4(color / 0.9964, 1.0);
}
