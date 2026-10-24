#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>

namespace PointerAngle {
// One 257-entry octant table serves every monitor and resolution. Pointer events
// use a ratio, two table reads and interpolation; no per-event atan2 or pixels.
inline std::optional<float> lookup(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    const double ax = std::abs(x), ay = std::abs(y);
    // Keep the last heading near the screen center, where direction is unstable.
    if (ax <= 8 && ay <= 8 && x * x + y * y <= 64) return std::nullopt;
    static const auto table = [] {
        std::array<float, 257> values{};
        for (size_t i = 0; i < values.size(); ++i)
            values[i] = std::atan(double(i) / 256.0) * 180.0 / std::numbers::pi;
        return values;
    }();
    const double index = std::min(ax, ay) / std::max(ax, ay) * 256.0;
    const auto lower = std::min(size_t(index), table.size() - 2);
    float angle = std::lerp(table[lower], table[lower + 1], float(index - lower));
    if (ax > ay) angle = 90.0f - angle;
    if (y > 0) angle = 180.0f - angle;
    return x < 0 ? -angle : angle; // clockwise from the top of the monitor
}

inline float wrap(float angle) {
    return std::isfinite(angle) ? std::remainder(angle, 360.0f) : 0.0f;
}

inline float nearest(float current, float target) {
    if (!std::isfinite(current)) return wrap(target);
    return current + wrap(target - current);
}
} // namespace PointerAngle
