precision highp float;

varying vec2 vUV;

uniform sampler2D source_texture;   // backdrop, sharp
uniform sampler2D blur_texture;     // backdrop, blurRadius 45
uniform vec2  uRes;
uniform vec2  uInvRes;

// --- shape ---
uniform vec2  uCenter;
uniform vec2  uHalf;
uniform float uCornerRadius;
uniform float uSuperness;           // superellipse exponent; 2.0 = plain rounded rect

// --- background material ---
uniform float outer_refraction_amount;
uniform float outer_refraction_inv_height;
uniform float inner_refraction_amount;
uniform float inner_refraction_inv_height;
uniform float refraction_threshold0;
uniform float refraction_threshold1;
uniform vec4  blur_alpha;           // blur_alpha0..3
uniform vec4  blur_dist;            // blur_dist0..3
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

// --- foreground material ---
uniform float aberration_amount;
uniform float inv_aberration_height;
uniform vec2  aberration_angle;

// --- rim outline ---
uniform vec3  rim_color;
uniform float rim_width;
uniform float rim_soft;
uniform float rim_opacity;
uniform vec2  uLightDir;

// --- face tone and coverage ---
uniform float face_slope;          // 0.6811
uniform float face_intercept;      // 0.2788
uniform float use_affine_face;     // 1 = affine, 0 = luma matrix
uniform float coverage_offset;     // 0.20
uniform float coverage_width;      // 2.00

// Luma matrix, factored:
//   out = c - 0.371 * luma709(c) + 0.235
const vec3  LUMA709   = vec3(0.2126, 0.7152, 0.0722);
const float CM_WEIGHT = 0.371;
const float CM_LIFT   = 0.235;

// The regression comparison selects the affine face tone.
vec3 face_color_matrix(vec3 c) {
  vec3 matrix = c - CM_WEIGHT * dot(c, LUMA709) + CM_LIFT;
  vec3 affine = c * face_slope + face_intercept;
  return mix(matrix, affine, use_affine_face);
}

// Superellipse SDF. At uSuperness == 2 this is the familiar rounded
// rect; larger values produce flatter corners.
float supercircle_sdf(vec2 p, vec2 half_size, float r, float n) {
  vec2  q  = abs(p) - (half_size - r);
  vec2  qc = max(q, 0.0);
  float corner = pow(pow(qc.x, n) + pow(qc.y, n), 1.0 / n) - r;
  return min(max(q.x, q.y), 0.0) + corner;
}

float sdf_at(vec2 p) {
  return supercircle_sdf(p - uCenter, uHalf, uCornerRadius, uSuperness);
}

// Outward normal by central difference — cheap, and the SDF is analytic.
vec2 sdf_normal(vec2 p) {
  const float e = 1.0;
  return normalize(vec2(
    sdf_at(p + vec2(e, 0.0)) - sdf_at(p - vec2(e, 0.0)),
    sdf_at(p + vec2(0.0, e)) - sdf_at(p - vec2(0.0, e))
  ) + 1e-6);
}

// Four-stop ramp: blur_alpha0..3 keyed on distance from the edge.
// Sharpens inward, which is why text under the centre stays readable
// while the rim stays milky.
float blur_ramp(float depth) {
  float a = mix(blur_alpha.x, blur_alpha.y, smoothstep(blur_dist.x, blur_dist.y, depth));
  a = mix(a, blur_alpha.z, smoothstep(blur_dist.y, blur_dist.z, depth));
  a = mix(a, blur_alpha.w, smoothstep(blur_dist.z, blur_dist.w, depth));
  return a;
}

vec3 sample_backdrop(vec2 uv, float blur_t) {
  return mix(texture2D(source_texture, uv).rgb,
             texture2D(blur_texture,   uv).rgb, blur_t);
}

void main() {
  vec2  p  = vUV * uRes;
  float d  = sdf_at(p);                 // < 0 inside
  vec3  bg = texture2D(source_texture, vUV).rgb;

  // Nothing to do well outside the shape.
  if (d > 2.0) { gl_FragColor = vec4(bg, 1.0); return; }

  vec2  n     = sdf_normal(p);
  float depth = max(-d, 0.0);           // px travelled in from the edge

  // ---- pass 4: refraction -------------------------------------------
  // Two bevel zones. Each is a linear height ramp off the edge
  // (hence *_inv_height), displacing the sample along the SDF normal.
  float h_outer = clamp(1.0 - depth * outer_refraction_inv_height, 0.0, 1.0);
  float h_inner = clamp(1.0 - depth * inner_refraction_inv_height, 0.0, 1.0);
  float zone    = smoothstep(refraction_threshold0, refraction_threshold1, depth);

  float amt = mix(outer_refraction_amount * h_outer * h_outer,
                  inner_refraction_amount * h_inner, zone);
  vec2  disp = n * amt;

  // ---- pass 5: chromatic aberration ---------------------------------
  float h_ab = clamp(1.0 - depth * inv_aberration_height, 0.0, 1.0);
  vec2  ab   = aberration_angle * aberration_amount * h_ab;

  float blur_t = blur_ramp(depth);
  vec3 refr = vec3(
    sample_backdrop(vUV + (disp + ab) * uInvRes, blur_t).r,
    sample_backdrop(vUV +  disp       * uInvRes, blur_t).g,
    sample_backdrop(vUV + (disp - ab) * uInvRes, blur_t).b
  );

  // ---- edge_bleed_* : backdrop dragged outward under the rim --------
  float bleed = clamp(1.0 - depth * edge_bleed_inv_height, 0.0, 1.0);
  vec3  bleed_c = texture2D(blur_texture, vUV + n * edge_bleed_amount * uInvRes).rgb;
  bleed_c *= 1.0 - bleed_darken * bleed;
  refr = mix(refr, bleed_c, bleed * edge_bleed_opacity);

  // ---- face_cm0..2 + holding_tone -----------------------------------
  refr = mix(refr, face_color_matrix(refr), face_opacity);
  refr = mix(refr, vec3(0.5), holding_tone_opacity);
  refr = mix(bg, refr, refraction_opacity);

  // ---- shadow_* : inner shadow, offset down -------------------------
  float sd = max(-sdf_at(p + vec2(0.0, shadow_offset)), 0.0);
  float sh = clamp(1.0 - sd * shadow_inv_height, 0.0, 1.0);
  refr *= 1.0 - shadow_amount * sh * sh;

  // ---- pass 3: rim_outline ------------------------------------------
  // One band off the same SDF, modulated by facing so the highlight
  // catches on two opposing edges instead of ringing the whole shape.
  float band = pow(1.0 - smoothstep(0.0, rim_width, depth), max(rim_soft, 0.001));
  float lit  = dot(n, uLightDir);
  float spec = pow(max(lit, 0.0), 3.0) + 0.55 * pow(max(-lit, 0.0), 6.0);
  refr += rim_color * band * spec * rim_opacity;

  // Coverage ramp with an explicit offset and width.
  float cov = clamp(0.5 - (d + coverage_offset) / coverage_width, 0.0, 1.0);
  gl_FragColor = vec4(mix(bg, refr, cov), 1.0);
}
