#version 300 es
precision highp float;

uniform sampler2D tex;
uniform vec2 direction; // tap spacing in texture coordinates
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    // 13 Gaussian taps, twice per axis at half resolution.
    const float w[7] = float[7](0.1964, 0.1760, 0.1217, 0.0650, 0.0267, 0.0085, 0.0021);
    vec3 color = texture(tex, v_texcoord).rgb * w[0];
    for (int i = 1; i < 7; ++i) {
        vec2 offset = direction * float(i);
        color += texture(tex, v_texcoord + offset).rgb * w[i];
        color += texture(tex, v_texcoord - offset).rgb * w[i];
    }
    fragColor = vec4(color, 1.0);
}
