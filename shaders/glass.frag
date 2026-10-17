#version 300 es
precision highp float;

// The material arithmetic uses logical, bottom-up coordinates.
// Hyprland supplies a cropped, padded backdrop, window geometry and fade alpha.
uniform sampler2D tex;               // sharp backdrop
uniform sampler2D blur_texture;
uniform vec2 fullSize;               // window extent in framebuffer pixels
uniform vec2 uvPadding;
uniform float radius;
uniform float roundingPower;
uniform float pixelScale;
uniform float fadeAlpha;

uniform float outer_refraction_amount;
uniform float outer_refraction_inv_height;
uniform float inner_refraction_amount;
uniform float inner_refraction_inv_height;
uniform float refraction_threshold0;
uniform float refraction_threshold1;
uniform float blur_alpha0;
uniform float blur_alpha3;
uniform float blur_dist3;
uniform float edge_bleed_amount;
uniform float edge_bleed_inv_height;
uniform float edge_bleed_opacity;
uniform float bleed_darken;
uniform float shadow_amount;
uniform float shadow_inv_height;
uniform float shadow_offset;
uniform float face_opacity;
uniform float holding_tone_opacity;
uniform float refraction_opacity;
uniform float aberration_amount;
uniform float inv_aberration_height;
uniform float rim_width;
uniform float rim_soft;
uniform float rim_opacity;
uniform float face_slope;
uniform float face_intercept;
uniform float use_measured_face;
uniform float coverage_offset;
uniform float coverage_width;
uniform float glass_opacity;

// Optional layer surface composition.
uniform sampler2D maskTex;
uniform int useMask;
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float maskAlphaThreshold;
uniform int maskMode;
uniform int regionRectCount;
uniform vec4 regionRects[16];

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

vec2 backdropUV(vec2 localUV) {
    vec2 uv = localUV * (1.0 - 2.0 * uvPadding) + uvPadding;
    vec2 halfTexel = 0.5 / vec2(textureSize(tex, 0));
    return clamp(uv, halfTexel, 1.0 - halfTexel);
}

vec3 face_color_matrix(vec3 c) {
    vec3 recipe = c - 0.371 * dot(c, vec3(0.2126, 0.7152, 0.0722)) + 0.235;
    return mix(recipe, c * face_slope + face_intercept, use_measured_face);
}

float sdf_at(vec2 p) {
    vec2 halfSize = fullSize / (2.0 * pixelScale);
    float r = clamp(radius / pixelScale, 0.0, min(halfSize.x, halfSize.y));
    float exponent = max(roundingPower, 1.0);
    vec2 q = abs(p) - (halfSize - r);
    vec2 qc = max(q, 0.0);
    float corner = pow(pow(qc.x, exponent) + pow(qc.y, exponent), 1.0 / exponent) - r;
    return min(max(q.x, q.y), 0.0) + corner;
}

vec2 sdf_gradient(vec2 p, float e) {
    return vec2(
        sdf_at(p + vec2(e, 0.0)) - sdf_at(p - vec2(e, 0.0)),
        sdf_at(p + vec2(0.0, e)) - sdf_at(p - vec2(0.0, e))
    );
}

vec2 sdf_normal(vec2 p) {
    // Keep the native outline's one-physical-pixel normal for rim lighting.
    return normalize(sdf_gradient(p, 1.0 / pixelScale) + 1e-6);
}

vec2 optical_normal(vec2 p, float depth, vec2 outlineNormal) {
    // Inward offsets of a small rounded corner eventually meet at a sharp
    // diagonal. Average the slope over a footprint that grows into the glass
    // so refraction and edge bleed turn through that seam continuously.
    // Coverage, depth and rim lighting still use the exact native shape.
    float pixel = 1.0 / pixelScale;
    float footprint = max(pixel, 0.5 * depth);
    if (footprint <= pixel)
        return outlineNormal;
    vec2 slope = sdf_gradient(p, footprint) / (2.0 * footprint);
    // Let opposing sides cancel in small windows instead of normalizing a
    // vanishing gradient into an arbitrary full-strength displacement.
    return slope / max(length(slope), 0.5);
}

float blur_ramp(float depth) {
    vec4 a = vec4(blur_alpha0, mix(blur_alpha0, blur_alpha3, 0.08),
                  mix(blur_alpha0, blur_alpha3, 0.55), blur_alpha3);
    vec4 d = vec4(0.0, 0.22, 0.55, 1.0) * blur_dist3;
    float t = mix(a.x, a.y, smoothstep(d.x, d.y, depth));
    t = mix(t, a.z, smoothstep(d.y, d.z, depth));
    return mix(t, a.w, smoothstep(d.z, d.w, depth));
}

vec3 sample_backdrop(vec2 uv, float blur_t) {
    vec2 padded = backdropUV(uv);
    return mix(texture(tex, padded).rgb, texture(blur_texture, padded).rgb, blur_t);
}

void main() {
    vec2 uv = v_texcoord;
    vec4 surfacePixel = vec4(0.0);
    bool hasMask = useMask == 1;
    if (hasMask) {
        surfacePixel = texture(maskTex, uv * maskUVScale + maskUVOffset);
        if (maskMode == 1) {
            bool inside = false;
            vec2 pos = uv * fullSize;
            for (int i = 0; i < regionRectCount; ++i) {
                vec4 r = regionRects[i];
                if (all(greaterThanEqual(pos, r.xy)) && all(lessThanEqual(pos, r.xy + r.zw))) {
                    inside = true;
                    break;
                }
            }
            if (!inside) { fragColor = surfacePixel; return; }
        } else if (surfacePixel.a < maskAlphaThreshold) {
            // Preserve low-alpha shadows even when they do not request glass.
            fragColor = surfacePixel;
            return;
        }
    }

    vec2 p = (uv - 0.5) * fullSize / pixelScale;
    float d = sdf_at(p);
    float cov = clamp(0.5 - (d + coverage_offset) / coverage_width, 0.0, 1.0);
    if (cov <= 0.0) { fragColor = surfacePixel; return; }

    vec2 n = sdf_normal(p);
    float depth = max(-d, 0.0);
    vec2 opticalN = optical_normal(p, depth, n);
    vec3 bg = texture(tex, backdropUV(uv)).rgb;
    float h_outer = clamp(1.0 - depth * outer_refraction_inv_height, 0.0, 1.0);
    float h_inner = clamp(1.0 - depth * inner_refraction_inv_height, 0.0, 1.0);
    float zone = smoothstep(refraction_threshold0, refraction_threshold1, depth);
    float amt = mix(outer_refraction_amount * h_outer * h_outer,
                    inner_refraction_amount * h_inner, zone);
    vec2 disp = opticalN * amt;

    float h_ab = clamp(1.0 - depth * inv_aberration_height, 0.0, 1.0);
    vec2 ab = vec2(0.9239, 0.3827) * aberration_amount * h_ab;
    vec2 invSize = pixelScale / fullSize;
    float blur_t = blur_ramp(depth);
    vec3 refr = vec3(
        sample_backdrop(uv + (disp + ab) * invSize, blur_t).r,
        sample_backdrop(uv + disp * invSize, blur_t).g,
        sample_backdrop(uv + (disp - ab) * invSize, blur_t).b
    );

    float bleed = clamp(1.0 - depth * edge_bleed_inv_height, 0.0, 1.0);
    vec3 bleed_c = texture(blur_texture, backdropUV(uv + opticalN * edge_bleed_amount * invSize)).rgb;
    bleed_c *= 1.0 - bleed_darken * bleed;
    refr = mix(refr, bleed_c, bleed * edge_bleed_opacity);
    refr = mix(refr, face_color_matrix(refr), face_opacity);
    refr = mix(refr, vec3(0.5), holding_tone_opacity);
    refr = mix(bg, refr, refraction_opacity);

    float sd = max(-sdf_at(p + vec2(0.0, shadow_offset)), 0.0);
    float sh = clamp(1.0 - sd * shadow_inv_height, 0.0, 1.0);
    refr *= 1.0 - shadow_amount * sh * sh;

    float band = pow(1.0 - smoothstep(0.0, rim_width, depth), max(rim_soft, 0.001));
    float lit = dot(n, vec2(-0.5736, 0.8192));
    float spec = pow(max(lit, 0.0), 3.0) + 0.55 * pow(max(-lit, 0.0), 6.0);
    refr += vec3(1.0, 0.99, 0.96) * band * spec * rim_opacity;

    // Apply coverage before SDR clipping. Clipping the rim
    // first loses bright highlights on antialiased corners. The sharp sample
    // is the exact destination pixel, so this mix preserves the background.
    // Then apply the decoration's fade with Hyprland's premultiplied blending.
    // Window content is drawn afterwards and remains unfiltered.
    float glassA = clamp(glass_opacity * fadeAlpha, 0.0, 1.0);
    vec3 color = clamp(mix(bg, refr, cov), 0.0, 1.0);
    fragColor = hasMask
        ? surfacePixel + vec4(color * glassA, glassA) * (1.0 - surfacePixel.a)
        : vec4(color * glassA, glassA);
}
