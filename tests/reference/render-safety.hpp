// Frozen pre-Rust geometry (2026-10-24). Test only; call mapping only on defined inputs.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace ReferenceRenderSafety {

inline bool integerCoordinate(double value) {
    return std::isfinite(value) && value >= std::numeric_limits<int>::min() &&
           value <= std::numeric_limits<int>::max();
}

inline bool dimension(double value) {
    // Leave room for rounding up a half-resolution buffer with (size + 1) / 2.
    return std::isfinite(value) && value >= 1 && value < std::numeric_limits<int>::max() &&
           std::floor(value) == value;
}

struct SSampleGeometry {
    int width, height;
    int x0, y0, x1, y1;
    std::array<int, 4> owner;
};

// Validate in floating point before narrowing or doing signed integer arithmetic.
// Wayland geometry is client-controlled and output scaling can exceed int range.
inline std::optional<SSampleGeometry> sampleGeometry(double x, double y, double w, double h, int padding) {
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 || padding < 0)
        return std::nullopt;
    const double width = std::ceil(w) + 2.0 * padding, height = std::ceil(h) + 2.0 * padding;
    const double left = std::floor(x), top = std::floor(y);
    const double right = std::ceil(x + w), bottom = std::ceil(y + h);
    const double x0 = left - padding, y0 = top - padding;
    const double x1 = x0 + width, y1 = y0 + height;
    if (!dimension(width) || !dimension(height) ||
        !integerCoordinate(left) || !integerCoordinate(top) ||
        !integerCoordinate(right) || !integerCoordinate(bottom) ||
        !integerCoordinate(x0) || !integerCoordinate(y0) ||
        !integerCoordinate(x1) || !integerCoordinate(y1))
        return std::nullopt;
    return SSampleGeometry{static_cast<int>(width), static_cast<int>(height),
        static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1),
        {static_cast<int>(left), static_cast<int>(top), static_cast<int>(right), static_cast<int>(bottom)}};
}

// The product can exceed INT_MAX even when every coordinate is a valid GL int.
inline int mapBlitCoordinate(int value, int from0, int from1, int to0, int to1) {
    return static_cast<int>(int64_t(to0) + (int64_t(value) - from0) * (int64_t(to1) - to0) / (int64_t(from1) - from0));
}

} // namespace RenderSafety
