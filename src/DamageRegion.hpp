#pragma once

#include <hyprutils/math/Region.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

// All regions are integer pixel scissors. Never change a quad, texture size or
// projection to restrict a draw: doing so changes interpolation/filter phases.
namespace DamageRegion {
using namespace Hyprutils::Math;

inline uint64_t area(const CRegion& region) {
    uint64_t result = 0;
    region.forEachRect([&](const auto& r) { result += uint64_t(r.x2 - r.x1) * uint64_t(r.y2 - r.y1); });
    return result;
}

// Maximum source-pixel reach of the existing Gaussian pyramid. Account for
// every reduction/reconstruction tap and a texel of bilinear support, including
// the nominally aligned axes. Shadow extent is an output bound, not a filter
// displacement, and must not be added to every material dependency.
inline double blurSupport(int sourceWidth, int sourceHeight, float sigma, bool materialBackdrop) {
    if (sigma <= 0) return 0;
    int width = (sourceWidth + 1) / 2, height = (sourceHeight + 1) / 2;
    const double baseX = double(sourceWidth) / width, baseY = double(sourceHeight) / height;
    double reduction = materialBackdrop ? 1 + std::max(baseX, baseY) : 1.5;
    double reconstruction = 0;
    while ((sigma * width / sourceWidth > 2.8f || sigma * height / sourceHeight > 2.8f) && (width > 1 || height > 1)) {
        reduction += 1.5 * std::max(double(sourceWidth) / width, double(sourceHeight) / height);
        width = std::max(1, (width + 1) / 2);
        height = std::max(1, (height + 1) / 2);
        reconstruction += 1.25 * std::max(double(sourceWidth) / width, double(sourceHeight) / height);
    }
    const double scaleX = double(sourceWidth) / width, scaleY = double(sourceHeight) / height;
    const double sigmaX = std::sqrt(std::max(0.0, sigma * sigma - 0.4 * (scaleX * scaleX - baseX * baseX)));
    const double sigmaY = std::sqrt(std::max(0.0, sigma * sigma - 0.4 * (scaleY * scaleY - baseY * baseY)));
    return reduction + reconstruction + 12 * std::max(sigmaX, sigmaY) / 2.8 + 4 * std::max(scaleX, scaleY);
}

inline CRegion bounded(CRegion region, const CBox& bounds) {
    region.intersect(bounds);
    uint64_t area = 0;
    size_t count = 0;
    region.forEachRect([&](const auto& r) {
        ++count;
        area += uint64_t(r.x2 - r.x1) * uint64_t(r.y2 - r.y1);
    });
    if (area >= bounds.w * bounds.h * 0.75) return CRegion(bounds);
    // Pixman already merges touching spans. Bound draw-call growth for sparse
    // client damage without allowing a rectangle to be blended twice.
    if (count > 32) return CRegion(region.getExtents());
    return region;
}

inline CRegion map(const CRegion& region, const Vector2D& from, const Vector2D& to,
                   double haloX = 2, double haloY = 2, bool backwards = false) {
    CRegion result;
    region.forEachRect([&](const auto& r) {
        const auto low = [&](double p, double scale, double halo) {
            return std::floor(backwards ? p * scale - halo : (p - halo) * scale);
        };
        const auto high = [&](double p, double scale, double halo) {
            return std::ceil(backwards ? p * scale + halo : (p + halo) * scale);
        };
        const double x0 = std::clamp(low(r.x1, to.x / from.x, haloX), 0.0, to.x);
        const double y0 = std::clamp(low(r.y1, to.y / from.y, haloY), 0.0, to.y);
        const double x1 = std::clamp(high(r.x2, to.x / from.x, haloX), 0.0, to.x);
        const double y1 = std::clamp(high(r.y2, to.y / from.y, haloY), 0.0, to.y);
        if (x1 > x0 && y1 > y0) result.add(CBox{x0, y0, x1 - x0, y1 - y0});
    });
    return bounded(std::move(result), CBox{{}, to});
}
}
