// Only the layer variant includes this. Window shader source is unchanged.
uniform int layerShapeCount;
uniform vec4 layerShapeBounds[32];
uniform float layerShapeRadii[32];
vec2 layerCenter, layerHalfSize;
float layerRadius;

float layerDistance(vec2 p, vec2 center, vec2 halfSize, float r) {
    vec2 q = abs(p - center) - (halfSize - r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void selectLayerShape(vec2 p) {
    float nearest = 1e30;
    for (int i = 0; i < layerShapeCount; ++i) {
        vec4 box = layerShapeBounds[i];
        vec2 center = (box.xy + box.zw * 0.5 - fullSize * 0.5) / pixelScale;
        vec2 halfSize = box.zw / (2.0 * pixelScale);
        float r = clamp(layerShapeRadii[i] / pixelScale, 0.0, min(halfSize.x, halfSize.y));
        float d = layerDistance(p, center, halfSize, r);
        if (d < nearest) {
            nearest = d;
            layerCenter = center;
            layerHalfSize = halfSize;
            layerRadius = r;
        }
    }
}

// Use the existing normal/optical-normal helpers with this card's geometry.
// Every fragment chooses its card before derivatives or mask early returns.
float layer_sdf_at(vec2 p) { return layerDistance(p, layerCenter, layerHalfSize, layerRadius); }
#define sdf_at layer_sdf_at
