#version 300 es
precision highp float;
// Raymarched studio with a grid and rounded primitives.
out vec4 fragColor;
in vec2 vUV;
uniform vec2 uRes;
uniform int uVariant;

mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }

// --- rounded primitive distance functions ---
float sdRoundBox(vec3 p, vec3 b, float r){
  vec3 q = abs(p) - b + r;
  return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}
float sdRoundedCylinder(vec3 p, float ra, float rb, float h){
  vec2 d = vec2(length(p.xz) - 2.0*ra + rb, abs(p.y) - h);
  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - rb;
}
float sdOctahedron(vec3 p, float s){
  p = abs(p);
  float m = p.x + p.y + p.z - s;
  vec3 q;
  if (3.0*p.x < m)      q = p.xyz;
  else if (3.0*p.y < m) q = p.yzx;
  else if (3.0*p.z < m) q = p.zxy;
  else return m * 0.57735027;
  float k = clamp(0.5*(q.z - q.y + s), 0.0, s);
  return length(vec3(q.x, q.y - s + k, q.z - k));
}

float sdCone(vec3 p, float h, float bottom, float top){
  vec2 q = vec2(length(p.xz), p.y);
  vec2 k1 = vec2(top, h), k2 = vec2(top - bottom, 2.0 * h);
  vec2 ca = vec2(q.x - min(q.x, p.y < 0.0 ? bottom : top), abs(q.y) - h);
  vec2 cb = q - k1 + k2 * clamp(dot(k1 - q, k2) / dot(k2, k2), 0.0, 1.0);
  float sign = cb.x < 0.0 && ca.y < 0.0 ? -1.0 : 1.0;
  return sign * sqrt(min(dot(ca, ca), dot(cb, cb))) - 0.035;
}

// (distance, material id)
vec2 map(vec3 p){
  vec2 res = vec2(p.y + 1.0, 0.0);

  vec3 pb = p - vec3(-1.52, -0.40, 0.30);
  pb.xz = rot(0.44) * pb.xz;
  float db = sdRoundBox(pb, vec3(0.60, 0.60, 0.60), 0.22);
  if (db < res.x) res = vec2(db, 1.0);

  vec3 pc = p - vec3(0.12, -0.42, -0.85);
  pc.xz = rot(0.2) * pc.xz;
  float dc = sdRoundedCylinder(pc, 0.33, 0.17, 0.42);
  if (uVariant != 0) dc = length(p - vec3(0.05, -0.27, -0.35)) - 0.73;
  if (dc < res.x) res = vec2(dc, 2.0);

  vec3 po = p - vec3(1.58, -0.24, 0.42);
  po.xz = rot(-0.52) * po.xz;
  po.xy = rot(0.20) * po.xy;
  float doc = sdOctahedron(po, 0.84) - 0.16;
  if (uVariant != 0) doc = sdCone(p - vec3(1.58, -0.265, 0.40), 0.70, 0.65, 0.035);
  if (doc < res.x) res = vec2(doc, 3.0);

  return res;
}

vec3 calcNormal(vec3 p){
  vec2 e = vec2(1.0, -1.0) * 0.0015;
  return normalize(
    e.xyy * map(p + e.xyy).x + e.yyx * map(p + e.yyx).x +
    e.yxy * map(p + e.yxy).x + e.xxx * map(p + e.xxx).x);
}

float softShadow(vec3 ro, vec3 rd, float k){
  float res = 1.0, t = 0.03;
  for (int i = 0; i < 22; i++){
    float h = map(ro + rd*t).x;
    res = min(res, k*h/t);
    t += clamp(h, 0.03, 0.40);
    if (res < 0.005 || t > 11.0) break;
  }
  return clamp(res, 0.0, 1.0);
}

float calcAO(vec3 p, vec3 n){
  float occ = 0.0, sca = 1.0;
  for (int i = 0; i < 5; i++){
    float h = 0.02 + 0.15*float(i)/4.0;
    occ += (h - map(p + n*h).x) * sca;
    sca *= 0.80;
  }
  return clamp(1.0 - 2.2*occ, 0.0, 1.0);
}

// linear-light albedos; the frame is gamma-encoded at the end so the
// glass colour matrix operates on display-referred values, as CA's does
vec3 albedo(float id, vec3 p, float t){
  if (id < 0.5){
    // Filter width grows with ray distance, so the far field dissolves to
    // its mean colour instead of moireing. Supersampling alone cannot fix
    // a grid whose period falls below the sample spacing -- this can.
    float fw1 = min(0.030 + 0.0130 * t, 0.5);
    float fw2 = min(0.070 + 0.0600 * t, 0.5);
    vec2 g1 = abs(fract(p.xz * 0.8) - 0.5);
    vec2 g2 = abs(fract(p.xz * 4.0) - 0.5);
    float l1 = 1.0 - smoothstep(0.0, fw1, min(g1.x, g1.y));
    float l2 = 1.0 - smoothstep(0.0, fw2, min(g2.x, g2.y));
    vec3 base = vec3(0.012, 0.016, 0.038);
    base = mix(base, vec3(0.010, 0.120, 0.105), l2 * 0.30);
    vec3 grid = mix(base, vec3(0.035, 0.520, 0.440), l1 * 0.85);
    return uVariant == 2 ? grid * 0.28 : grid;
  }
  if (id < 1.5) return vec3(0.620, 0.030, 0.250);
  if (id < 2.5) return vec3(0.020, 0.170, 0.640);
  return vec3(0.820, 0.280, 0.020);
}

vec3 sky(vec2 uv){
  vec3 c = mix(vec3(0.006, 0.008, 0.022), vec3(0.020, 0.013, 0.042), uv.y);
  c = mix(c, vec3(0.045, 0.010, 0.055), smoothstep(0.0, 1.0, uv.x) * 0.55);
  float ar = uRes.x / uRes.y;
  c += vec3(0.60, 0.07, 0.26) * pow(max(0.0, 1.0 - length((uv - vec2(0.16, 0.80)) * vec2(ar, 1.0)) * 1.25), 3.0) * 0.55;
  c += vec3(0.05, 0.26, 0.62) * pow(max(0.0, 1.0 - length((uv - vec2(0.88, 0.58)) * vec2(ar, 1.0)) * 1.25), 3.0) * 0.55;
  c += vec3(0.55, 0.30, 0.03) * pow(max(0.0, 1.0 - length((uv - vec2(0.52, 0.95)) * vec2(ar, 1.0)) * 1.70), 3.0) * 0.45;
  return uVariant == 2 ? c * 0.16 : c;
}

vec3 render(vec2 uv){
  vec2 q = uv * 2.0 - 1.0;
  q.x *= uRes.x / uRes.y;

  vec3 ro = vec3(0.0, 1.15, 4.70);
  vec3 ta = vec3(0.05, 0.02, 0.0);
  vec3 ww = normalize(ta - ro);
  vec3 uu = normalize(cross(ww, vec3(0.0, 1.0, 0.0)));
  vec3 vv = cross(uu, ww);
  vec3 rd = normalize(q.x*uu + q.y*vv + 1.95*ww);

  vec3 col = sky(uv);

  float t = 0.0, mid = -1.0;
  for (int i = 0; i < 72; i++){
    vec2 h = map(ro + rd*t);
    if (h.x < 0.0010*t){ mid = h.y; break; }
    t += h.x;
    if (t > 22.0) break;
  }

  if (mid >= 0.0){
    vec3 p = ro + rd*t;
    vec3 n = calcNormal(p);
    vec3 base = albedo(mid, p, t);

    vec3 L = normalize(vec3(-0.52, 0.76, 0.40));
    float dif = clamp(dot(n, L), 0.0, 1.0);
    float sh  = softShadow(p + n*0.014, L, 12.0);
    float ao  = calcAO(p, n);
    float fre = pow(clamp(1.0 + dot(rd, n), 0.0, 1.0), 4.0);
    float bac = clamp(dot(n, normalize(vec3(0.62, 0.10, -0.72))), 0.0, 1.0);
    vec3  H   = normalize(L - rd);
    float spe = pow(clamp(dot(n, H), 0.0, 1.0), 48.0);

    vec3 lin = vec3(0.0);
    lin += (uVariant == 2 ? 0.95 : 2.10) * dif * sh * vec3(1.00, 0.94, 0.84);
    lin += 0.45 * ao        * vec3(0.20, 0.30, 0.55);
    lin += 0.40 * bac * ao  * vec3(0.85, 0.22, 0.42);
    col  = base * lin;
    col += (uVariant == 2 ? 0.85 : 1.60) * spe * sh * vec3(1.00, 0.96, 0.88);
    col += (uVariant == 2 ? 0.18 : 0.45) * fre * ao * vec3(0.45, 0.68, 1.00);

    col = mix(col, sky(uv), 1.0 - exp(-0.0022*t*t*t));
  }

  col = col / (1.0 + col * 0.50);          // soft shoulder
  return pow(clamp(col, 0.0, 1.0), vec3(0.4545));
}

void main(){
  // 4x rotated-grid supersampling. The grid is rotated rather than ordered
  // because almost every edge here is near-axis-aligned -- box faces, floor
  // lines -- and an ordered 2x2 gives those only two distinct coverage
  // steps. Tonemapping happens inside render(), per sample, so the tight
  // speculars on the rounded edges average as display values and cannot
  // blow a single subsample into a firefly.
  // Cost is one-time: the scene is cached to a texture, not redrawn.
  vec2 px = 1.0 / uRes;
  vec3 col = render(vUV + vec2( 0.125,  0.375) * px)
           + render(vUV + vec2( 0.375, -0.125) * px)
           + render(vUV + vec2(-0.125, -0.375) * px)
           + render(vUV + vec2(-0.375,  0.125) * px);
  col *= 0.25;

  float g = fract(sin(dot(floor(vUV * uRes), vec2(12.9898, 78.233))) * 43758.5453);
  col += (g - 0.5) * 0.022;

  fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
