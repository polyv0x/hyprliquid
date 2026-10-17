#version 300 es
precision highp float;
precision highp int;

uniform sampler2D tex;
uniform sampler2D sceneTex;
uniform sampler2D backgroundTex;
uniform ivec2 sampleOrigin;
uniform ivec4 ownerBounds;
uniform float neighborContribution;

layout(location = 0) out vec4 fragColor;

void main() {
    // Integer fetches preserve the native framebuffer pixels and extrude its
    // edge into offscreen gutters, matching the full-strength blit path.
    ivec2 p = clamp(sampleOrigin + ivec2(gl_FragCoord.xy), ivec2(0), textureSize(tex, 0) - 1);
    bool inside = all(greaterThanEqual(p, ownerBounds.xy)) && all(lessThan(p, ownerBounds.zw));
    if (inside)
        fragColor = texelFetch(tex, p, 0);
    else
        fragColor = mix(texelFetch(backgroundTex, p, 0), texelFetch(sceneTex, p, 0), neighborContribution);
}
