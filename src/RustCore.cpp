#include "RustCore.hpp"
#include "Material.hpp"
#include "hyprliquid-bridge/ffi.rs.h"

#include <atomic>
#include <cstdio>

namespace {
namespace ffi = hyprliquid::ffi;
static_assert(sizeof(int) == sizeof(int32_t));

bool succeeded(ffi::Status status) noexcept {
    if (status == ffi::Status::Ok) return true;
    if (status == ffi::Status::InternalFailure) {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set())
            std::fputs("[hyprliquid] Rust calculation failed; discarding its result\n", stderr);
    }
    return false;
}

ffi::Material toRust(const SMaterial& value) noexcept {
    ffi::Material result{};
    result.liquid = value.family == EMaterialFamily::LIQUID;
    result.dark_theme = value.darkTheme;
#define GLASS_PARAM(name, ...) result.name = value.name;
#include "MaterialParams.inc"
#undef GLASS_PARAM
    return result;
}

SMaterial fromRust(const ffi::Material& value) noexcept {
    SMaterial result;
    result.family = value.liquid ? EMaterialFamily::LIQUID : EMaterialFamily::LEGACY;
    result.darkTheme = value.dark_theme;
#define GLASS_PARAM(name, ...) result.name = value.name;
#include "MaterialParams.inc"
#undef GLASS_PARAM
    return result;
}

#include "hyprliquid-bridge/LiquidConversions.hpp"

SMaterial disabledMaterial() noexcept {
    SMaterial result;
    result.glass_opacity = 0;
    return result;
}
}

SMaterial liquidControlDefaults(bool dark) noexcept {
    const auto result = ffi::liquid_control_defaults(dark);
    return succeeded(result.status) ? fromRust(result.value) : disabledMaterial();
}

SLiquidMaterial effectiveLiquidMaterial(const SMaterial& material) noexcept {
    const auto result = ffi::effective_liquid_material(toRust(material));
    return succeeded(result.status) ? fromRust(result.value) : SLiquidMaterial{};
}

void sanitizeMaterial(SMaterial& material) noexcept {
    const auto result = ffi::sanitize_material(toRust(material));
    // The caller's value is replaced only after Rust has completed the operation.
    material = succeeded(result.status) ? fromRust(result.value) : disabledMaterial();
}

float materialBlurSigma(const SMaterial& material) noexcept {
    const auto result = ffi::material_blur_sigma(toRust(material));
    return succeeded(result.status) ? result.value : 0;
}

float materialOutputExtent(const SMaterial& material) noexcept {
    const auto result = ffi::material_output_extent(toRust(material));
    return succeeded(result.status) ? result.value : 0;
}

int materialPadding(const SMaterial& material, float scale) noexcept {
    const auto result = ffi::material_padding(toRust(material), scale);
    return succeeded(result.status) ? result.value : 0;
}

namespace RenderSafety {
bool integerCoordinate(double value) noexcept {
    const auto result = ffi::integer_coordinate(value);
    return succeeded(result.status) && result.value;
}
bool dimension(double value) noexcept {
    const auto result = ffi::dimension(value);
    return succeeded(result.status) && result.value;
}
std::optional<SSampleGeometry> sampleGeometry(double x, double y, double w, double h, int padding) noexcept {
    const auto result = ffi::sample_geometry(x, y, w, h, padding);
    if (!succeeded(result.status)) return std::nullopt;
    const auto& g = result.value;
    return SSampleGeometry{g.width, g.height, g.x0, g.y0, g.x1, g.y1, g.owner};
}
std::optional<int> mapBlitCoordinate(int value, int from0, int from1, int to0, int to1) noexcept {
    const auto result = ffi::map_blit_coordinate(value, from0, from1, to0, to1);
    if (!succeeded(result.status)) return std::nullopt;
    return result.value;
}
std::optional<uint64_t> bufferBytes(int width, int height, uint32_t blockWidth, uint32_t blockHeight, uint32_t bytesPerBlock) noexcept {
    const auto result = ffi::buffer_bytes(width, height, blockWidth, blockHeight, bytesPerBlock);
    if (!succeeded(result.status)) return std::nullopt;
    return result.value;
}
}
