#pragma once

#include "HyprlandCompat.hpp"
#include <cstdint>
#include <vector>

class IHyprWindowDecoration;

namespace NativeDecorationState {
// Older hosts expose their resolved gradients directly on CWindow.
bool initialize();
void destroy();
bool appendKey(std::vector<uint64_t>& key, const SP<IHyprWindowDecoration>& decoration);
}
