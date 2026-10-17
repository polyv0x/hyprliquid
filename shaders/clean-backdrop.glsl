// Reconstruct the clean sharp crop from existing textures: native pixels in
// the owner, immutable scene background outside. This is exactly the image
// previously materialized in a separate full-resolution framebuffer.
uniform sampler2D clean_texture;
uniform ivec2 cleanOrigin;
uniform ivec4 cleanOwnerBounds;

bool clean_inside(ivec2 p) {
    return all(greaterThanEqual(p, cleanOwnerBounds.xy)) && all(lessThan(p, cleanOwnerBounds.zw));
}

vec3 clean_texel(ivec2 local) {
    local = clamp(local, ivec2(0), textureSize(tex, 0) - 1);
    ivec2 p = clamp(cleanOrigin + local, ivec2(0), textureSize(clean_texture, 0) - 1);
    return clean_inside(p) ? texelFetch(tex, local, 0).rgb : texelFetch(clean_texture, p, 0).rgb;
}

vec3 sample_clean(vec2 uv, vec3 sharp, bool haveSharp) {
    vec2 size = vec2(textureSize(tex, 0));
    uv = clamp(uv, 0.5 / size, 1.0 - 0.5 / size);
    vec2 p = uv * size - 0.5;
    ivec2 a = ivec2(floor(p));
    ivec2 limit = textureSize(clean_texture, 0) - 1;
    ivec2 lo = clamp(cleanOrigin + a, ivec2(0), limit);
    ivec2 hi = clamp(cleanOrigin + a + 1, ivec2(0), limit);
    if (clean_inside(lo) && clean_inside(hi))
        return haveSharp ? sharp : texture(tex, uv).rgb;
    if (any(lessThan(hi, cleanOwnerBounds.xy)) || any(greaterThanEqual(lo, cleanOwnerBounds.zw)))
        return texture(clean_texture, (vec2(cleanOrigin) + uv * size) / vec2(limit + 1)).rgb;

    // Only a one-texel strip crosses the owner boundary. Filter the four
    // selected texels there instead of interpolating two already-filtered
    // images, which would leak neighbour colour across the boundary.
    vec2 f = fract(p);
    return mix(mix(clean_texel(a), clean_texel(a + ivec2(1, 0)), f.x),
               mix(clean_texel(a + ivec2(0, 1)), clean_texel(a + 1), f.x), f.y);
}

vec3 sample_clean(vec2 uv) {
    return sample_clean(uv, vec3(0.0), false);
}
