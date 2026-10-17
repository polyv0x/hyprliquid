#version 300 es
precision highp float;

uniform sampler2D tex;
uniform vec2 direction; // tap spacing in texture coordinates
uniform int filterMode; // 0: Gaussian, 1: downsample, 2: reconstruct
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    if (filterMode != 0) {
        // Filter every reduction instead of skipping pixels in the sharp
        // image. Smaller offsets during reconstruction soften the coarse
        // texel grid without expanding it into visible bilinear cells.
        vec2 offset = vec2(filterMode == 1 ? 0.5 : 0.25) / vec2(textureSize(tex, 0));
        vec3 color = texture(tex, v_texcoord + vec2(-offset.x, -offset.y)).rgb;
        color += texture(tex, v_texcoord + vec2(offset.x, -offset.y)).rgb;
        color += texture(tex, v_texcoord + vec2(-offset.x, offset.y)).rgb;
        color += texture(tex, v_texcoord + vec2(offset.x, offset.y)).rgb;
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
    fragColor = vec4(color, 1.0);
}
