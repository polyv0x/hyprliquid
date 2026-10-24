#version 300 es
precision highp float;
precision highp int;
uniform sampler2D tex; // Exact, padded native crop
uniform sampler2D sceneTex, backgroundTex;
uniform ivec2 sampleOrigin;
uniform ivec4 ownerBounds;
uniform float neighborContribution;
uniform int useScene;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

vec3 inputTexel(ivec2 local) {
    local = clamp(local, ivec2(0), textureSize(tex, 0) - 1);
    ivec2 p = clamp(sampleOrigin + local, ivec2(0), textureSize(sceneTex, 0) - 1);
    if (all(greaterThanEqual(p, ownerBounds.xy)) && all(lessThan(p, ownerBounds.zw)))
        return texelFetch(tex, local, 0).rgb;
    return mix(texelFetch(backgroundTex, p, 0).rgb, texelFetch(sceneTex, p, 0).rgb, neighborContribution);
}
void main() {
    if (useScene == 0) { fragColor = texture(tex, v_texcoord); return; }
    // Select the scene per full-resolution texel BEFORE bilinear reduction.
    // This preserves owner boundaries, odd sizes and offscreen edge extrusion.
    vec2 p = v_texcoord * vec2(textureSize(tex, 0)) - 0.5;
    ivec2 a = ivec2(floor(p));
    vec2 f = fract(p);
    fragColor = vec4(mix(mix(inputTexel(a), inputTexel(a + ivec2(1, 0)), f.x),
                         mix(inputTexel(a + ivec2(0, 1)), inputTexel(a + 1), f.x), f.y), 1.0);
}
