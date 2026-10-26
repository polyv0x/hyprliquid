#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

// Recover smooth card outlines from a client's horizontal rounded-region
// strips, before output scaling/rotation. No framebuffer readback is needed.
namespace LayerShape {

inline constexpr int MAX_SHAPES = 32;
inline constexpr int MAX_RECTS = 512;

struct Rect {
    double x1, y1, x2, y2;
    bool operator==(const Rect&) const = default;
};

struct RoundedRect {
    double x, y, w, h, radius;
};

// A region is not necessarily a set of cards. Reject holes, asymmetric corners,
// overlaps and other non-rounded components instead of inventing an outline.
inline std::vector<RoundedRect> recognize(std::span<const Rect> rects) {
    if (rects.empty() || rects.size() > MAX_RECTS) return {};
    for (const auto& r : rects)
        if (!std::isfinite(r.x1) || !std::isfinite(r.y1) || !std::isfinite(r.x2) || !std::isfinite(r.y2) ||
            r.x2 <= r.x1 || r.y2 <= r.y1) return {};

    std::vector<int> component(rects.size(), -1);
    std::vector<RoundedRect> shapes;
    for (size_t first = 0; first < rects.size(); ++first) {
        if (component[first] != -1) continue;
        if (shapes.size() == MAX_SHAPES) return {};
        std::vector<size_t> connected{first};
        component[first] = static_cast<int>(shapes.size());
        for (size_t i = 0; i < connected.size(); ++i) {
            const auto& a = rects[connected[i]];
            for (size_t j = 0; j < rects.size(); ++j) {
                if (component[j] != -1) continue;
                const auto& b = rects[j];
                // Edge contact connects strips; a single corner does not.
                const double dx = std::min(a.x2, b.x2) - std::max(a.x1, b.x1);
                const double dy = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
                if ((dx > 0 && dy >= 0) || (dx >= 0 && dy > 0)) {
                    component[j] = component[first];
                    connected.push_back(j);
                }
            }
        }
        std::sort(connected.begin(), connected.end(), [&](size_t a, size_t b) { return rects[a].y1 < rects[b].y1; });
        Rect bounds = rects[first];
        for (auto index : connected) {
            const auto& r = rects[index];
            bounds.x1 = std::min(bounds.x1, r.x1); bounds.y1 = std::min(bounds.y1, r.y1);
            bounds.x2 = std::max(bounds.x2, r.x2); bounds.y2 = std::max(bounds.y2, r.y2);
        }
        const double width = bounds.x2 - bounds.x1, height = bounds.y2 - bounds.y1;
        if (!std::isfinite(width) || !std::isfinite(height)) return {};
        double low = 0, high = std::min(width, height) / 2;
        double nextY = bounds.y1;
        bool rounded = false;
        for (auto index : connected) {
            const auto& r = rects[index];
            if (r.y1 != nextY) return {}; // Missing row or multiple spans/hole in a row.
            nextY = r.y2;
            const double inset = r.x1 - bounds.x1;
            if (std::abs(inset - (bounds.x2 - r.x2)) > 0.01) return {};
            if (inset == 0) continue;
            rounded = true;
            const double depth = std::min(r.y2 - bounds.y1, bounds.y2 - r.y1);
            if (depth > height / 2) return {}; // A notch inside the card.
            // Clients round strip insets up to logical pixels. For a circular
            // corner: radius = inset + depth + sqrt(2 * inset * depth).
            // Intersect the radius intervals allowed by that quantization.
            const auto radiusAt = [depth](double x) { return x + depth + std::sqrt(2 * x * depth); };
            low = std::max(low, radiusAt(std::max(0.0, inset - 1.0)));
            high = std::min(high, radiusAt(inset));
        }
        if (low > high + 0.01) return {};
        double radius = rounded ? (low + high) / 2 : 0;
        // An unambiguous integer radius avoids turning an exact client corner
        // into a fractional one solely because its region was quantized.
        if (rounded && std::floor(high + 0.001) == std::ceil(low + 0.001)) radius = std::floor(high + 0.001);

        // Validate the whole profile, including full-width rows and both ends.
        // This also rejects a pair of touching/overlapping cards whose union is
        // no longer one rounded rectangle.
        for (auto index : connected) {
            const auto& r = rects[index];
            const double depth = std::min(r.y2 - bounds.y1, bounds.y2 - r.y1);
            const double dy = std::max(0.0, radius - depth);
            const double expected = radius - std::sqrt(std::max(0.0, radius * radius - dy * dy));
            const double inset = r.x1 - bounds.x1;
            if (inset + 0.01 < expected || inset - expected > 1.01) return {};
        }
        shapes.push_back({bounds.x1, bounds.y1, width, height, radius});
    }
    return shapes;
}

} // namespace LayerShape
