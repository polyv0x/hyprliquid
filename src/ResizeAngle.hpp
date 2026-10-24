#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

// Follow the already interpolated native size, including its easing/spring.
// No timer, animation callbacks, or writes to Hyprland's animated variable.
class CResizeAngle {
  public:
    using Size = std::array<double, 2>;

    float value() const { return m_value; }
    void reset(float value = 0.0f) {
        m_active = false;
        m_value = value;
    }

    float update(float peak, bool animating, const Size& begun, const Size& goal, const Size& current) {
        const double dx = goal[0] - begun[0], dy = goal[1] - begun[1];
        const double length2 = dx * dx + dy * dy;
        if (!animating || length2 < 1e-12 || !std::isfinite(length2) || !std::isfinite(peak)) {
            reset();
            return m_value;
        }
        const double progress = ((current[0] - begun[0]) * dx + (current[1] - begun[1]) * dy) / length2;
        if (!std::isfinite(progress)) {
            reset();
            return m_value;
        }

        const double t = std::clamp(progress, 0.0, 1.0);
        if (!m_active || begun != m_begun || goal != m_goal || peak != m_peak) {
            // Retargeting a running resize starts from the last drawn angle.
            // A material edit partway through uses the remaining native phase.
            m_phaseStart = m_active && begun == m_begun && goal == m_goal ? t : 0.0;
            m_start = m_active ? m_value : 0.0f;
            m_begun = begun;
            m_goal = goal;
            m_peak = peak;
            m_active = true;
        }
        const double phase = m_phaseStart < 1.0 ? std::clamp((t - m_phaseStart) / (1.0 - m_phaseStart), 0.0, 1.0) : 1.0;
        // A bounded sweep with a smooth turnaround. An interrupted first half
        // blends from its current angle; every completed sweep ends at zero.
        const float wave = phase >= 1.0 ? 0.0f : static_cast<float>(std::sin(std::numbers::pi * phase));
        m_value = phase < 0.5 ? std::lerp(m_start, peak, wave) : peak * wave;
        return m_value;
    }

  private:
    Size m_begun{}, m_goal{};
    bool m_active = false;
    float m_value = 0.0f, m_start = 0.0f, m_peak = 0.0f;
    double m_phaseStart = 0.0;
};
