#pragma once

#include "Material.hpp"
#include <cstdint>
#include <optional>
#include <string>

// Runtime ownership never enters the config parser's pending preset map.
struct SRuntimeProfile {
    std::optional<bool> enabled;
    std::optional<std::string> basePreset;
    std::optional<std::string> theme;
    SPresetValues overrides;
};

struct SRuntimeControl {
    SRuntimeProfile profile;
    std::string epoch;
    uint64_t revision = 0;
    uint64_t generation = 0;
};

void registerRuntimeControl();
void unregisterRuntimeControl();
// Reload clears panel ownership; a dynamic config() edit also yields to the user.
void runtimeBaselineChanged(bool reloaded);
bool runtimeEnabled();
std::string runtimeBasePreset();
bool runtimeThemeIsDark();
