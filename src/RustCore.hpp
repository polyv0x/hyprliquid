#pragma once

#include <array>
#include <cstdint>
#include <optional>

struct SMaterial;
struct SLiquidMaterial;

// Synchronous copied values only. No host pointers, callbacks, Lua operations,
// allocations or exceptions in the normal adapter path. Rust catches panics
// and returns an explicit status; failures produce disabled/empty results.
SMaterial liquidControlDefaults(bool dark) noexcept;
SLiquidMaterial effectiveLiquidMaterial(const SMaterial& material) noexcept;
void sanitizeMaterial(SMaterial& material) noexcept;
float materialBlurSigma(const SMaterial& material) noexcept;
float materialOutputExtent(const SMaterial& material) noexcept;
int materialPadding(const SMaterial& material, float scale) noexcept;

namespace RenderSafety {
struct SSampleGeometry {
    int width, height;
    int x0, y0, x1, y1;
    std::array<int, 4> owner;
};

bool integerCoordinate(double value) noexcept;
bool dimension(double value) noexcept;
std::optional<SSampleGeometry> sampleGeometry(double x, double y, double w, double h, int padding) noexcept;
std::optional<int> mapBlitCoordinate(int value, int from0, int from1, int to0, int to1) noexcept;
std::optional<uint64_t> bufferBytes(int width, int height, uint32_t blockWidth, uint32_t blockHeight, uint32_t bytesPerBlock) noexcept;
}
