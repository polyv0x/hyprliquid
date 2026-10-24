#pragma once

#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <optional>
#include <unordered_map>

// A single native animation per monitor, shared by all its Liquid windows.
class CHoverAngles {
  public:
    ~CHoverAngles() { clear(); }
    void refresh(bool force = true);
    void update(const Vector2D& position, bool force = false);
    void remove(MONITORID monitor);
    void clear();
    float value(const PHLMONITOR& monitor) const;
    float target(const PHLMONITOR& monitor) const;
    bool animating(const PHLMONITOR& monitor) const;

  private:
    struct SMonitor {
        PHLMONITORREF monitor;
        PHLANIMVAR<float> angle;
        float lastValue = 0;
    };
    std::unordered_map<MONITORID, SMonitor> m_monitors;
    std::optional<Vector2D> m_pointer;
    void changed(MONITORID monitor);
};
